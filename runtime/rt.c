/* The dispatcher. Translated code runs until it hits a transfer it cannot
 * follow statically. Then it sets CS:IP and returns here. This loop finds
 * what lives at CS:IP: a C interrupt handler stub, or translated code of a
 * loaded image. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bios.h"
#include "hle.h"
#include "rt.h"

void dos_casemap_upper(void); /* dos_fs.c */

int hle_redirect;
int rt_exited;
int rt_exit_code;

/* ---- logging and faults -------------------------------------------------- */

static FILE *logf;

static FILE *log_open(void) {
    if (logf) return logf;
    const char *p = getenv("VC_LOG");
    char path[512];
    if (!p) {
        const char *cache = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (cache && *cache) snprintf(path, sizeof path, "%s/vc-linux", cache);
        else snprintf(path, sizeof path, "%s/.cache/vc-linux", home ? home : "/tmp");
        mkdir(path, 0755);
        strncat(path, "/vc.log", sizeof path - strlen(path) - 1);
        p = path;
    }
    logf = fopen(p, "a");
    if (logf) setvbuf(logf, NULL, _IOLBF, 0);
    return logf;
}

void rt_log(const char *fmt, ...) {
    FILE *f = log_open();
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

static void dump_state(FILE *f) {
    fprintf(f, "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n",
            cpu.a.x, cpu.b.x, cpu.c.x, cpu.d.x, cpu.si, cpu.di, cpu.bp, cpu.sp);
    fprintf(f, "CS=%04X IP=%04X DS=%04X ES=%04X SS=%04X FLAGS=%04X\n",
            cpu.cs, cpu.ip, cpu.ds, cpu.es, cpu.ss, flags_get());
    fprintf(f, "stack:");
    for (int i = 0; i < 12; i++) fprintf(f, " %04X", rd16(cpu.ss, (uint16_t)(cpu.sp + 2 * i)));
    fprintf(f, "\ncode: ");
    for (int i = 0; i < 16; i++) fprintf(f, " %02X", rd8(cpu.cs, (uint16_t)(cpu.ip + i)));
    fputc('\n', f);
}

_Noreturn void rt_fault(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    term_shutdown();
    fprintf(stderr, "vc: fatal: %s\n", msg);
    dump_state(stderr);
    FILE *f = log_open();
    if (f) { fprintf(f, "fatal: %s\n", msg); dump_state(f); }
    exit(70);
}

/* ---- clock and yielding -------------------------------------------------- */

static struct timespec last_idle;
static time_t midnight, next_midnight; /* local midnight, in epoch seconds */

/* The BIOS tick counter at 0040:006C, 18.2 ticks a second since midnight.
 * Cheap enough to call on every few dispatches: localtime only runs once a day. */
void rt_update_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ts.tv_sec >= next_midnight) {
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        int crossed = next_midnight != 0;
        midnight = ts.tv_sec - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        next_midnight = midnight + 86400;
        if (crossed) wr8(0x40, 0x70, 1);
    }
    uint64_t ms = (uint64_t)(ts.tv_sec - midnight) * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    uint32_t ticks = (uint32_t)(ms * 1193182ull / 65536ull / 1000ull);
    wr16(0x40, 0x6C, (uint16_t)ticks);
    wr16(0x40, 0x6E, (uint16_t)(ticks >> 16));
}

static long ms_since(const struct timespec *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

void rt_yield(void) {
    rt_budget = 20000;
    rt_update_clock();
    if (ms_since(&last_idle) >= 40) {
        term_idle(0);
        clock_gettime(CLOCK_MONOTONIC, &last_idle);
    }
}

/* ---- ports --------------------------------------------------------------- */

static uint8_t crtc_index, crtc_regs[32], retrace;

uint8_t port_in8(uint16_t port) {
    switch (port) {
    case 0x3DA: case 0x3BA:
        retrace ^= 0x09; /* toggle display-enable and vertical retrace */
        return retrace;
    case 0x3D4: return crtc_index;
    case 0x3D5: return crtc_regs[crtc_index & 31];
    case 0x3D6: return 0; /* no OS/2 VIO */
    default: return 0xFF;
    }
}
uint16_t port_in16(uint16_t port) { return port_in8(port) | (uint16_t)(port_in8((uint16_t)(port + 1)) << 8); }
void port_out8(uint16_t port, uint8_t v) {
    if (port == 0x3D4) crtc_index = v;
    else if (port == 0x3D5) crtc_regs[crtc_index & 31] = v;
}
void port_out16(uint16_t port, uint16_t v) { port_out8(port, (uint8_t)v); port_out8((uint16_t)(port + 1), (uint8_t)(v >> 8)); }

/* ---- loaded images ------------------------------------------------------- */

/* One entry per image, kept after its memory is reused: a program may keep
 * running routines it copied elsewhere, and those are found by matching their
 * bytes against `snap`, the image as it was last loaded and relocated. */
/* Code the program copied elsewhere: where it runs, and what it is a copy of. */
typedef struct { uint32_t lin; const Image *img; uint32_t off; } Moved;
#define MAX_MOVED 256
static Moved moved[MAX_MOVED];
static int nmoved;
static void nmoved_reset(void) { nmoved = 0; }

typedef struct {
    const Image *img;
    uint16_t loadseg;
    uint32_t base;
    uint8_t *snap;
    int32_t deltas[8]; /* offsets of copies seen so far */
    int ndeltas;
} Known;

#define MAX_KNOWN 4
static Known known[MAX_KNOWN];
static int nknown;

void rt_register_image(const Image *img, uint16_t loadseg) {
    uint32_t base = (uint32_t)loadseg << 4;
    Known *k = NULL;
    for (int i = 0; i < nknown; i++) if (known[i].img == img) k = &known[i];
    if (!k) {
        if (nknown == MAX_KNOWN) rt_fault("too many images");
        k = &known[nknown++];
        k->img = img;
        k->snap = malloc(img->size);
    }
    k->loadseg = loadseg;
    k->base = base;
    k->ndeltas = 0;
    /* Newest first, so the byte check below prefers the image loaded last
     * where an old one's area was reused. */
    Known t = *k;
    *k = known[nknown - 1];
    known[nknown - 1] = t;
    k = &known[nknown - 1];
    nmoved_reset();
    memcpy(k->snap, &mem[base], img->size);
    rt_log("loaded %s at %04X (linear %05X, %u bytes)", img->name, loadseg, base, img->size);
}


static int run_moved(Known *k, uint32_t off, uint32_t L) {
    rt_code_delta = (int32_t)(L - (k->base + off));
    int r = k->img->run(off, k->loadseg);
    rt_code_delta = 0;
    return r;
}

static Known *find_known(const Image *img) {
    for (int i = 0; i < nknown; i++) if (known[i].img == img) return &known[i];
    return NULL;
}

static void note_moved(Known *k, uint32_t off, uint32_t L) {
    int32_t d = (int32_t)(L - (k->base + off));
    int seen = 0;
    for (int i = 0; i < k->ndeltas; i++) seen |= k->deltas[i] == d;
    if (!seen && k->ndeltas < 8) {
        k->deltas[k->ndeltas++] = d;
        rt_log("%s copy at offset %+d (first seen at %05X)", k->img->name, d, L);
    }
    if (nmoved < MAX_MOVED) moved[nmoved++] = (Moved){L, k->img, off};
}

static uint64_t n_dispatch, n_direct, n_cached, n_delta, n_search;

static int run_at(uint32_t L) {
    n_dispatch++;
    for (int i = nknown - 1; i >= 0; i--) {
        Known *k = &known[i];
        if (L < k->base || L >= k->base + k->img->size) continue;
        uint32_t off = L - k->base;
        uint32_t n = k->img->size - off < 6 ? k->img->size - off : 6;
        if (memcmp(&mem[L], &k->snap[off], n) == 0 && k->img->run(off, k->loadseg) == 0) { n_direct++; return 1; }
    }
    for (int i = 0; i < nmoved; i++) {
        Moved *m = &moved[i];
        Known *k = find_known(m->img);
        if (m->lin == L && memcmp(&mem[L], &k->snap[m->off], 3) == 0) {
            n_cached++;
            return run_moved(k, m->off, L) == 0;
        }
    }
    /* A copy we already know: the same offset, and at least 3 matching bytes
     * at an instruction start. A block's last instruction is followed by
     * different bytes in the copy, so a longer match cannot be required. */
    for (int i = nknown - 1; i >= 0; i--) {
        Known *k = &known[i];
        for (int j = 0; j < k->ndeltas; j++) {
            int64_t off = (int64_t)L - k->deltas[j] - k->base;
            if (off < 0 || off + 3 > k->img->size || memcmp(&mem[L], &k->snap[off], 3)) continue;
            if (run_moved(k, (uint32_t)off, L) == 0) { n_delta++; note_moved(k, (uint32_t)off, L); return 1; }
        }
    }
    const uint32_t K = 12;
    n_search++;
    for (int i = nknown - 1; i >= 0; i--) {
        Known *k = &known[i];
        for (uint32_t off = 0; off + K <= k->img->size; off++) {
            if (k->snap[off] != mem[L] || memcmp(&k->snap[off], &mem[L], K) != 0) continue;
            if (run_moved(k, off, L) != 0) continue; /* not an instruction start */
            note_moved(k, off, L);
            return 1;
        }
    }
    return 0;
}

/* ---- interrupt stubs ----------------------------------------------------- */

static void stub_return(int iret) {
    uint16_t ip = pop16(), cs = pop16(), fl = pop16();
    if (iret) flags_set(fl);
    else cpu.ifl = (fl >> 9) & 1;
    cpu.ip = ip;
    cpu.cs = cs;
}

static void unimplemented_21(void) {
    static uint8_t seen[256];
    if (!seen[cpu.a.h]++) rt_log("unimplemented INT 21h AX=%04X at %04X:%04X", cpu.a.x, rd16(cpu.ss, (uint16_t)(cpu.sp + 2)), rd16(cpu.ss, cpu.sp));
    cpu.a.x = 1;
    cpu.cf = 1;
}

static int do_int(uint8_t n) {
    if (n != 0x16) hle_other_calls++;
    switch (n) {
    case 0x10: bios_int10(); return 0;
    case 0x16: bios_int16(); return 0;
    case 0x33: bios_int33(); return 0;
    case 0x21: {
        /* VC_TRACE=1 logs every DOS call with the string at DS:DX and the result. */
        static int trace = -1;
        if (trace < 0) trace = getenv("VC_TRACE") != NULL;
        Cpu before = cpu;
        rt_update_clock();
        if (!dos_con_int21() && !dos_fs_int21() && !dos_core_int21()) unimplemented_21();
        if (trace && before.a.h > 0x0C) {
            char str[80];
            int i = 0;
            for (; i < 79; i++) {
                uint8_t c = rd8(before.ds, (uint16_t)(before.d.x + i));
                if (c < 32 || c > 126) break;
                str[i] = (char)c;
            }
            str[i] = 0;
            if (before.a.h == 0x56 || before.a.x == 0x7156) {
                char dst[80];
                int j = 0;
                for (; j < 79; j++) {
                    uint8_t c = rd8(before.es, (uint16_t)(before.di + j));
                    if (c < 32 || c > 126) break;
                    dst[j] = (char)c;
                }
                dst[j] = 0;
                rt_log("  rename target ES:DI \"%s\"", dst);
            }
            rt_log("int21 %04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X \"%s\" -> CF=%d AX=%04X",
                   before.a.x, before.b.x, before.c.x, before.d.x, before.si, before.di, str,
                   cpu.cf, cpu.a.x);
        }
        return 0;
    }
    case 0x00: {
        uint16_t ip = rd16(cpu.ss, cpu.sp), cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
        rt_fault("divide error at %04X:%04X", cs, ip);
    }
    default:
        return dos_int_other(n);
    }
}

static void stub(uint16_t off) {
    hle_redirect = 0;
    if (off < 0x100) {
        int iret = do_int((uint8_t)off);
        if (!hle_redirect) stub_return(iret);
    } else if (off == STUB_CASEMAP) {
        dos_casemap_upper();
        cpu.ip = pop16();
        cpu.cs = pop16();
    } else if (off == STUB_EXIT) {
        rt_exited = 1;
    } else {
        rt_fault("jump into BIOS ROM at F000:%04X", off);
    }
}

/* kill -USR1 <pid> logs the registers and the last dispatched addresses. */
static volatile sig_atomic_t dump_requested;
static void on_usr1(int sig) { (void)sig; dump_requested = 1; }
static uint32_t recent[256];
static unsigned recent_at;

static void dump_recent(void) {
    dump_requested = 0;
    FILE *f = log_open();
    if (!f) return;
    fprintf(f, "state on SIGUSR1: dispatches %llu direct %llu cached-moved %llu by-offset %llu searched %llu, moved table %d\n",
            (unsigned long long)n_dispatch, (unsigned long long)n_direct, (unsigned long long)n_cached,
            (unsigned long long)n_delta, (unsigned long long)n_search, nmoved);
    dump_state(f);
    fprintf(f, "last dispatches (oldest first):");
    for (unsigned i = 0; i < 256; i++) {
        uint32_t v = recent[(recent_at + i) & 255];
        if (v) fprintf(f, "%s%05X", i % 16 ? " " : "\n  ", v);
    }
    fputc('\n', f);
}

void rt_run(void) {
    signal(SIGUSR1, on_usr1);
    while (!rt_exited) {
        uint32_t L = lin(cpu.cs, cpu.ip);
        recent[recent_at++ & 255] = L;
        if (dump_requested) dump_recent();
        if (!(recent_at & 15)) rt_update_clock(); /* programs wait on 0040:006C */
        if (L >= (STUB_SEG << 4) && L < (STUB_SEG << 4) + STUB_END) {
            stub((uint16_t)(L - (STUB_SEG << 4)));
            continue;
        }
        if (!run_at(L)) rt_fault("no translated code at %04X:%04X", cpu.cs, cpu.ip);
    }
}
