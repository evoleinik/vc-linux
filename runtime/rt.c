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
#include "guest_mem.h"
#include "hle.h"
#include "modem.h"
#include "rt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "web_source.h"
EM_JS(void, browser_speaker, (double hz), {
    if (Module['vcSpeaker']) Module['vcSpeaker'](hz);
});
#endif

void dos_casemap_upper(void); /* dos_fs.c */

int hle_redirect;
int rt_exited;
int rt_exit_code;
int rt_halted;

/* ---- logging and faults -------------------------------------------------- */

static FILE *logf;
static int logging_enabled = 1;

void rt_set_logging(int enabled) {
    logging_enabled = !!enabled;
    if (!logging_enabled && logf) { fclose(logf); logf = NULL; }
}

static FILE *log_open(void) {
    if (!logging_enabled) return NULL;
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

static time_t midnight, next_midnight; /* local midnight, in epoch seconds */

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

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

void rt_yield(void) {
    rt_budget = 20000;
    rt_update_clock();
    /* Under Emscripten RT_TICK reaches here from translated functions behind
     * Image.run. Never sleep on that indirect stack: ASYNCIFY_IGNORE_INDIRECT
     * leaves it uninstrumented. Terminal input/rendering/yielding all belong
     * to rt_run's direct chain, on native builds as well as in the browser. */
}

/* ---- ports --------------------------------------------------------------- */

static uint8_t crtc_index, crtc_regs[32], retrace;

/* The firmware starts both used PIT channels in lobyte/hibyte square-wave
 * mode. Channel 0 normally divides by 65536; GW-BASIC temporarily uses 2983
 * (~400 Hz) and chains the original IRQ 0 at the normal 18.2 Hz itself. */
#define PIT_HZ 1193182u
typedef struct {
    uint16_t reload, partial;
    uint8_t access, mode, writing_high, reading_high;
} PitChannel;
static PitChannel pit[3] = {
    {.access = 3, .mode = 3}, {.access = 3, .mode = 2}, {.access = 3, .mode = 3}
};
static uint8_t speaker_control, pic_mask;
static double speaker_last_hz;
static uint64_t timer_next_ns;
static unsigned timer_pending;
static int timer_in_service;
/* The master 8259 is edge triggered. Kermit EOIs before draining RBR, so
 * treating UART's still-high output as another request would recurse into
 * the same byte. Masked edges are retained until IF and OCW1 allow delivery. */
static uint8_t serial_line, serial_pending, serial_in_service, pic_read_isr;

static void serial_edge(void) {
    int asserted = modem_irq_pending();
    if (asserted && !serial_line) serial_pending = 1;
    serial_line = (uint8_t)asserted;
}

static void serial_poll(uint64_t now) {
    modem_tick(now);
    serial_edge();
}

static void pic_eoi(uint8_t command) {
    if ((command & 0x60) == 0x60) {
        /* OCW2 specific EOI, used by Kermit's IRQ 4 handler. */
        if ((command & 7) == 0) timer_in_service = 0;
        if ((command & 7) == 4) serial_in_service = 0;
    } else if (command & 0x20) {
        /* Non-specific EOI clears only the highest-priority active IRQ. */
        if (timer_in_service) timer_in_service = 0;
        else serial_in_service = 0;
    }
}

static uint64_t timer_period_ns(void) {
    unsigned divisor = pit[0].reload ? pit[0].reload : 65536u;
    return (uint64_t)divisor * 1000000000ull / PIT_HZ;
}

double rt_speaker_hz(void) {
    if ((speaker_control & 3) != 3 || (pit[2].mode != 2 && pit[2].mode != 3))
        return 0;
    return (double)PIT_HZ / (pit[2].reload ? pit[2].reload : 65536u);
}

static void speaker_update(void) {
    double hz = rt_speaker_hz();
    if (hz == speaker_last_hz) return;
    speaker_last_hz = hz;
#ifdef __EMSCRIPTEN__
    browser_speaker(hz);
#endif
}

typedef struct Known Known;
static Known *save_known_image(const Image *parent);
static void restore_known_image(Known *saved);

struct RtProcessState {
    Known *parent_image;
    uint8_t vectors[1024];
    PitChannel pit[3];
    uint8_t speaker_control, pic_mask;
    unsigned timer_pending;
    int timer_in_service;
    uint8_t pic_read_isr;
};

RtProcessState *rt_save_process_state(const Image *parent) {
    RtProcessState *state = malloc(sizeof *state);
    if (!state) return NULL;
    state->parent_image = parent ? save_known_image(parent) : NULL;
    if (parent && !state->parent_image) { free(state); return NULL; }
    (void)guest_read(state->vectors, 0, sizeof state->vectors); /* constant range */
    memcpy(state->pit, pit, sizeof pit);
    state->speaker_control = speaker_control;
    state->pic_mask = pic_mask;
    state->timer_pending = timer_pending;
    state->timer_in_service = timer_in_service;
    state->pic_read_isr = pic_read_isr;
    return state;
}

void rt_finish_process_state(RtProcessState *state, int restore) {
    if (!state) return;
    if (restore) {
        (void)guest_write(0, state->vectors, sizeof state->vectors); /* constant range */
        memcpy(pit, state->pit, sizeof pit);
        speaker_control = state->speaker_control;
        pic_mask = state->pic_mask;
        timer_pending = state->timer_pending;
        timer_in_service = state->timer_in_service;
        pic_read_isr = state->pic_read_isr;
        /* A forcibly stopped serial child cannot restore its IRQ handler or
         * drop DTR itself. Disconnect it before returning to VC's vectors. */
        modem_reset();
        serial_line = serial_pending = serial_in_service = 0;
        /* Do not replay the child's elapsed time, abandoned IRQ or Break
         * into its parent. Keep the parent's programmed timer frequency. */
        timer_next_ns = monotonic_ns() + timer_period_ns();
        rt_halted = 0;
        bios_take_break();
        bios_cancel_read();
        speaker_update();
    }
    restore_known_image(state->parent_image);
    free(state);
}

static void pit_write(unsigned channel, uint8_t v) {
    PitChannel *p = &pit[channel];
    if (p->access == 3 && !p->writing_high) {
        p->partial = v;
        p->writing_high = 1;
        return;
    }
    if (p->access == 1) p->reload = v;
    else if (p->access == 2) p->reload = (uint16_t)(v << 8);
    else p->reload = (uint16_t)(p->partial | (v << 8));
    p->writing_high = 0;
    p->reading_high = 0;
    if (channel == 0) {
        timer_next_ns = monotonic_ns() + timer_period_ns();
        timer_pending = 0;
    } else if (channel == 2) {
        speaker_update();
    }
}

static uint8_t pit_read(unsigned channel) {
    /* Sufficient for divisor readback; this is not a cycle-accurate counter
     * latch. Neither translated image samples the running PIT counters. */
    PitChannel *p = &pit[channel];
    if (p->access == 2) return (uint8_t)(p->reload >> 8);
    if (p->access == 1) return (uint8_t)p->reload;
    p->reading_high ^= 1;
    return p->reading_high ? (uint8_t)p->reload : (uint8_t)(p->reload >> 8);
}

uint8_t port_in8(uint16_t port) {
    if (port >= 0x3f8 && port <= 0x3ff) {
        serial_poll(monotonic_ns());
        uint8_t value = modem_port_in(port);
        serial_edge();
        return value;
    }
    switch (port) {
    case 0x20: return pic_read_isr ? (timer_in_service ? 1 : 0) | (serial_in_service ? 16 : 0)
                                 : (timer_pending ? 1 : 0) | (serial_pending ? 16 : 0);
    case 0x21: return pic_mask;
    case 0x40: case 0x41: case 0x42: return pit_read(port - 0x40);
    case 0x61: return speaker_control;
    case 0x3DA: case 0x3BA:
        retrace ^= 0x09; /* toggle display-enable and vertical retrace */
        return retrace;
    case 0x3D4: return crtc_index;
    case 0x3D5: return crtc_regs[crtc_index & 31];
    case 0x3D6: return 0; /* no OS/2 VIO */
    case 0x3D9: return bios_cga_color_register();
    default: return 0xFF;
    }
}
uint16_t port_in16(uint16_t port) { return port_in8(port) | (uint16_t)(port_in8((uint16_t)(port + 1)) << 8); }
void port_out8(uint16_t port, uint8_t v) {
    if (port >= 0x3f8 && port <= 0x3ff) {
        /* Port polling may spin inside translated code. Advance the UART
         * without sleeping or delivering interrupts on that indirect stack. */
        serial_poll(monotonic_ns());
        modem_port_out(port, v);
        serial_edge();
    } else if (port >= 0x40 && port <= 0x42) pit_write(port - 0x40, v);
    else if (port == 0x43) {
        unsigned channel = v >> 6, access = (v >> 4) & 3;
        if (channel < 3) {
            PitChannel *p = &pit[channel];
            if (access) {
                p->access = (uint8_t)access;
                p->mode = (uint8_t)((v >> 1) & 7);
                if (p->mode >= 6) p->mode -= 4; /* Modes 6/7 alias 2/3. */
                p->writing_high = 0;
            }
            p->reading_high = 0;
            if (channel == 2) speaker_update();
        }
    } else if (port == 0x61) {
        speaker_control = v;
        speaker_update();
    } else if (port == 0x20) {
        if ((v & 0x18) == 0x08) {
            if (v & 2) pic_read_isr = v & 1; /* OCW3 read IRR/ISR selection. */
        } else pic_eoi(v);
    }
    else if (port == 0x21) pic_mask = v;
    else if (port == 0x3D4) crtc_index = v;
    else if (port == 0x3D5) crtc_regs[crtc_index & 31] = v;
    else if (port == 0x3D9) bios_cga_color_select(v);
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

struct Known {
    const Image *img;
    uint16_t loadseg;
    uint32_t base;
    uint8_t *snap;
    int32_t deltas[8]; /* offsets of copies seen so far */
    int ndeltas;
};

/* VC's two parts plus BASIC, Logo, Rogue and VZ need six entries, which
 * survive child termination. Leave two slots for further compiled-C images. */
#define MAX_KNOWN 8
static Known known[MAX_KNOWN];
static int nknown;
static struct { const Image *image; RtImageRunner run; } supplements[MAX_KNOWN];
static unsigned nsupplements;

#ifdef __EMSCRIPTEN__
/* A ring of distinct canonical block addresses in recency order. Hash chains
 * make a repeated idle-loop entry an O(1)-expected move to the ring's head,
 * so a long keyboard wait cannot erase all history outside that loop. No
 * listing or source-line lookup happens in the dispatch path. */
#define SOURCE_HISTORY 1024u
#define SOURCE_HASH 2048u
#define SOURCE_STACK_WORDS 1024u
typedef struct {
    const Image *image;
    uint32_t offset;
    uint16_t cs, ip;
    unsigned older, newer, hash_next;
} SourceEntry;
static SourceEntry source_history[SOURCE_HISTORY];
static unsigned source_hash[SOURCE_HASH]; /* indices plus one; zero is empty */
static unsigned source_count, source_newest;

static unsigned source_bucket(const Image *image, uint32_t offset) {
    return (unsigned)(((uintptr_t)image >> 2) ^ (offset * 2654435761u)) &
           (SOURCE_HASH - 1);
}

static void source_enter(const Image *image, uint32_t offset,
                         uint16_t cs, uint16_t ip) {
    unsigned bucket = source_bucket(image, offset), index;
    for (unsigned link = source_hash[bucket]; link; link = source_history[link - 1].hash_next) {
        SourceEntry *entry = &source_history[link - 1];
        if (entry->image != image || entry->offset != offset) continue;
        index = link - 1;
        entry->cs = cs;
        entry->ip = ip;
        if (index == source_newest) return;
        source_history[entry->older].newer = entry->newer;
        source_history[entry->newer].older = entry->older;
        goto newest;
    }
    if (source_count < SOURCE_HISTORY) {
        index = source_count++;
    } else {
        index = source_history[source_newest].newer; /* oldest in the ring */
        SourceEntry *old = &source_history[index];
        unsigned *link = &source_hash[source_bucket(old->image, old->offset)];
        while (*link != index + 1) link = &source_history[*link - 1].hash_next;
        *link = old->hash_next;
        source_history[old->older].newer = old->newer;
        source_history[old->newer].older = old->older;
    }
    source_history[index] = (SourceEntry){
        .image = image, .offset = offset, .cs = cs, .ip = ip,
        .hash_next = source_hash[bucket]
    };
    source_hash[bucket] = index + 1;
    if (source_count == 1) {
        source_history[index].older = source_history[index].newer = index;
        source_newest = index;
        return;
    }
newest:
    source_history[index].older = source_newest;
    source_history[index].newer = source_history[source_newest].newer;
    source_history[source_history[index].newer].older = index;
    source_history[source_newest].newer = index;
    source_newest = index;
}
#endif

void rt_register_supplement(const Image *img, RtImageRunner run) {
    if (!img || !run) rt_fault("invalid image supplement");
    for (unsigned i = 0; i < nsupplements; ++i) {
        if (supplements[i].image != img) continue;
        if (supplements[i].run != run) rt_fault("conflicting image supplements");
        return;
    }
    if (nsupplements == MAX_KNOWN) rt_fault("too many image supplements");
    supplements[nsupplements].image = img;
    supplements[nsupplements++].run = run;
}

static int run_image(Known *k, uint32_t off) {
    int result = k->img->run(off, k->loadseg);
    if (result == 0) return 0;
    for (unsigned i = 0; i < nsupplements; ++i)
        if (supplements[i].image == k->img)
            return supplements[i].run(off, k->loadseg);
    return result;
}

#ifdef __EMSCRIPTEN__
/* A rejected entry has no guest side effects and must not appear in the
 * history. Image.run cannot suspend, so recording immediately after its
 * successful return still describes the entry at the saved CS:IP. */
static int source_run_image(Known *k, uint32_t off) {
    uint16_t cs = cpu.cs, ip = cpu.ip;
    int result = run_image(k, off);
    if (!result) source_enter(k->img, off, cs, ip);
    return result;
}
#define run_image source_run_image
#endif

void rt_register_image(const Image *img, uint16_t loadseg) {
    uint32_t base = (uint32_t)loadseg << 4;
    Known *k = NULL;
    for (int i = 0; i < nknown; i++) if (known[i].img == img) k = &known[i];
    if (!k) {
        if (nknown == MAX_KNOWN) rt_fault("too many images");
        k = &known[nknown++];
        k->img = img;
        k->snap = malloc(img->size ? img->size : 1);
        if (!k->snap) rt_fault("out of memory registering %s", img->name);
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
    /* The loader has already proved this range; never read past guest RAM. */
    if (guest_read(k->snap, base, img->size)) rt_fault("%s loaded outside guest memory", img->name);
    rt_log("loaded %s at %04X (linear %05X, %u bytes)", img->name, loadseg, base, img->size);
}


static int run_moved(Known *k, uint32_t off, uint32_t L) {
    rt_code_delta = (int32_t)(L - (k->base + off));
    int r = run_image(k, off);
    rt_code_delta = 0;
    return r;
}

static Known *find_known(const Image *img) {
    for (int i = 0; i < nknown; i++) if (known[i].img == img) return &known[i];
    return NULL;
}

static Known *save_known_image(const Image *parent) {
    Known *current = find_known(parent);
    if (!current) return NULL;
    Known *saved = malloc(sizeof *saved);
    if (!saved) return NULL;
    *saved = *current;
    saved->snap = malloc(parent->size);
    if (!saved->snap) { free(saved); return NULL; }
    /* Keep the originally approved, relocated bytes. Copying live RAM
     * here would silently approve an undeclared parent code mutation. */
    memcpy(saved->snap, current->snap, parent->size);
    return saved;
}

static void restore_known_image(Known *saved) {
    if (!saved) return;
    Known *current = find_known(saved->img);
    if (!current) rt_fault("lost parent image registration");
    free(current->snap);
    /* A recursive EXEC replaced this same Image's relocation base and
     * snapshot. Restore only its parent, keeping other newly loaded images
     * registered for VC's resident/copied code, and prefer the parent. */
    *current = known[nknown - 1];
    known[nknown - 1] = *saved;
    free(saved);
    nmoved_reset();
}

static int code_matches(const Known *k, uint32_t off, uint32_t L, uint32_t n) {
    const uint8_t *code = guest_span(L, n);
    if (off + n > k->img->size || !code) return 0;
    if (!memcmp(code, &k->snap[off], n)) return 1;
    /* Ignore only declared mutable bytes, never a whole image. Emitted code
     * reads patched operands from guest memory; VZ's source-proved two-byte
     * opcode slot additionally checks its finite variants before executing. */
    for (uint32_t j = 0; j < n; ++j) {
        if (code[j] == k->snap[off + j]) continue;
        uint32_t low = 0, high = k->img->nmutable;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (k->img->mutable_offsets[middle] < off + j) low = middle + 1;
            else high = middle;
        }
        if (low == k->img->nmutable || k->img->mutable_offsets[low] != off + j)
            return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
/* Four wasm32 words; browser_source_snapshot copies every value before
 * returning, so callers never retain aliases into the wasm heap. */
typedef struct {
    const char *image;
    uint32_t offset, cs, ip;
} SourceLocation;
typedef struct {
    uint32_t sp, word;
    SourceLocation near, far;
} SourceStackWord;
_Static_assert(sizeof(SourceLocation) == 16, "source location JS wire layout");
_Static_assert(sizeof(SourceStackWord) == 40, "source stack JS wire layout");

static int source_matches(const Known *k, uint32_t off, uint32_t L,
                          int preceding, uint32_t width) {
    if (off > k->img->size) return 0;
    uint32_t n;
    if (preceding) {
        n = off < 3 ? off : 3;
        if (!n || L < n) return 0;
        return code_matches(k, off - n, L - n, n);
    }
    n = k->img->size - off < width ? k->img->size - off : width;
    return n && code_matches(k, off, L, n);
}

static SourceLocation source_location(uint16_t cs, uint16_t ip, int preceding) {
    SourceLocation out = {.cs = cs, .ip = ip};
    uint32_t L = lin(cs, ip);
    /* Preserve dispatch order: most recently loaded live image, exact
     * cached copy, then already-observed deltas. Unlike run_at, never probe
     * a translation to discover an instruction boundary. The fetched map
     * makes that final check without changing guest state. */
    for (int i = nknown - 1; i >= 0; --i) {
        Known *k = &known[i];
        if (L < k->base || L > k->base + k->img->size) continue;
        uint32_t off = L - k->base;
        if (!source_matches(k, off, L, preceding, 6)) continue;
        out.image = k->img->name;
        out.offset = off;
        return out;
    }
    for (int i = 0; i < nmoved; ++i) {
        Moved *m = &moved[i];
        if (m->lin != L) continue;
        Known *k = find_known(m->img);
        if (!k || !source_matches(k, m->off, L, preceding, 3)) continue;
        out.image = k->img->name;
        out.offset = m->off;
        return out;
    }
    for (int i = nknown - 1; i >= 0; --i) {
        Known *k = &known[i];
        for (int j = 0; j < k->ndeltas; ++j) {
            int64_t off = (int64_t)L - k->base - k->deltas[j];
            if (off < 0 || off > k->img->size ||
                !source_matches(k, (uint32_t)off, L, preceding, 3)) continue;
            out.image = k->img->name;
            out.offset = (uint32_t)off;
            return out;
        }
    }
    return out;
}

EM_JS(void, browser_source_resolved,
      (const char *name, unsigned offset, unsigned cs, unsigned ip), {
    if (Module['vcSourceResolved']) Module['vcSourceResolved'](name ? {
        image: UTF8ToString(name), offset: offset, cs: cs, ip: ip,
    } : null);
});

EMSCRIPTEN_KEEPALIVE void vc_source_resolve(unsigned cs, unsigned ip, int preceding) {
    SourceLocation where = source_location((uint16_t)cs, (uint16_t)ip, preceding);
    browser_source_resolved(where.image, where.offset, where.cs, where.ip);
}

EM_JS(void, browser_source_snapshot,
      (unsigned cs, unsigned ip, unsigned ss, unsigned sp,
       const SourceLocation *current, unsigned kind, unsigned return_cs, unsigned return_ip,
       const SourceStackWord *stack, unsigned words,
       const SourceLocation *entries, unsigned count, unsigned truncated), {
    if (!Module['vcSourceSnapshot']) return;
    function location(pointer) {
        var p = pointer >>> 2, name = HEAPU32[p];
        return {
            image: name ? UTF8ToString(name) : null,
            offset: HEAPU32[p + 1], cs: HEAPU32[p + 2], ip: HEAPU32[p + 3],
        };
    }
    var now = location(current), frames = [], recent = [];
    now.kind = ['instruction', 'interrupt', 'continuation'][kind];
    now.returnCS = return_cs;
    now.returnIP = return_ip;
    for (var i = 0; i < words; ++i) {
        var at = stack + i * 40, p = at >>> 2;
        var near = location(at + 8), far = location(at + 24);
        frames.push({sp: HEAPU32[p], word: HEAPU32[p + 1],
                     near: near.image ? near : null, far: far.image ? far : null});
    }
    for (var i = 0; i < count; ++i) recent.push(location(entries + i * 16));
    Module['vcSourceSnapshot']({version: 1,
        raw: {cs: cs, ip: ip, ss: ss, sp: sp}, current: now,
        stack: frames, recent: recent, stackTruncated: !!truncated,
        historyCapacity: 1024, historyFull: count === 1024,
    });
});

EMSCRIPTEN_KEEPALIVE void vc_source_snapshot(void) {
    uint16_t cs = cpu.cs, ip = cpu.ip, return_cs = cs, return_ip = ip;
    uint16_t stack_sp = cpu.sp;
    uint32_t L = lin(cs, ip);
    unsigned kind = 0;
    if (L >= (STUB_SEG << 4) && L < (STUB_SEG << 4) + STUB_END) {
        /* The CPU is really in the host BIOS/DOS stub. Its guest interrupt
         * frame supplies the truthful suspended context, not the previous
         * block's first instruction. Only call it an INT when its live
         * opcode and vector agree; hardware IRQ continuations stay labelled
         * continuations instead of fabricating a guest INT instruction. */
        unsigned stub_off = L - (STUB_SEG << 4);
        ip = return_ip = rd16(cpu.ss, cpu.sp);
        cs = return_cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
        /* The suspended context already accounts for this stub's return.
         * Its IP/CS/(FLAGS) words are not older CALL frames. Advance just
         * the snapshot cursor, with the same 16-bit wrap as pop16(). */
        if (stub_off < 0x100 || stub_off == STUB_INT8_RETURN)
            stack_sp = (uint16_t)(stack_sp + 6);
        else if (stub_off == STUB_CASEMAP)
            stack_sp = (uint16_t)(stack_sp + 4);
        kind = 2;
        if (stub_off < 0x100 && rd8(cs, (uint16_t)(ip - 2)) == 0xcd &&
            rd8(cs, (uint16_t)(ip - 1)) == stub_off) {
            ip -= 2;
            kind = 1;
        } else if ((stub_off == 3 && rd8(cs, (uint16_t)(ip - 1)) == 0xcc) ||
                   (stub_off == 4 && rd8(cs, (uint16_t)(ip - 1)) == 0xce)) {
            --ip;
            kind = 1;
        }
    }
    SourceLocation current = source_location(cs, ip, 0);
    SourceStackWord stack[SOURCE_STACK_WORDS];
    unsigned available = (0x10000u - stack_sp) / 2u;
    unsigned words = available < SOURCE_STACK_WORDS ? available : SOURCE_STACK_WORDS;
    for (unsigned i = 0; i < words; ++i) {
        uint16_t sp = (uint16_t)(stack_sp + i * 2);
        uint16_t word = rd16(cpu.ss, sp);
        uint16_t far_cs = rd16(cpu.ss, (uint16_t)(sp + 2));
        stack[i] = (SourceStackWord){
            .sp = sp, .word = word,
            .near = source_location(cs, word, 1),
            .far = source_location(far_cs, word, 1),
        };
    }
    SourceLocation entries[SOURCE_HISTORY];
    unsigned index = source_newest;
    for (unsigned i = 0; i < source_count; ++i) {
        SourceEntry *entry = &source_history[index];
        entries[i] = (SourceLocation){
            .image = entry->image->name, .offset = entry->offset,
            .cs = entry->cs, .ip = entry->ip,
        };
        index = entry->older;
    }
    browser_source_snapshot(cpu.cs, cpu.ip, cpu.ss, cpu.sp, &current, kind,
                            return_cs, return_ip, stack, words, entries, source_count,
                            available > words);
}
#endif

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
        if (code_matches(k, off, L, n) && run_image(k, off) == 0) { n_direct++; return 1; }
    }
    for (int i = 0; i < nmoved; i++) {
        Moved *m = &moved[i];
        Known *k = find_known(m->img);
        if (m->lin == L && code_matches(k, m->off, L, 3)) {
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
            if (off < 0 || off + 3 > k->img->size || !code_matches(k, (uint32_t)off, L, 3)) continue;
            if (run_moved(k, (uint32_t)off, L) == 0) { n_delta++; note_moved(k, (uint32_t)off, L); return 1; }
        }
    }
    const uint32_t K = 12;
    n_search++;
    for (int i = nknown - 1; i >= 0; i--) {
        Known *k = &known[i];
        for (uint32_t off = 0; off + K <= k->img->size; off++) {
            if (k->snap[off] != mem[L] || !code_matches(k, off, L, K)) continue;
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
    case 0x08:
        /* The BIOS chains the user tick with a real interrupt frame. Its
         * return lands at a C continuation; no translated function is ever
         * called recursively from a timer or terminal handler. */
        cpu.cs = STUB_SEG;
        cpu_int(0x1c, STUB_INT8_RETURN);
        hle_redirect = 1;
        return 1;
    case 0x0c: pic_eoi(0x64); return 1; /* BIOS stray IRQ 4. */
    case 0x10: bios_int10(); return 0;
    case 0x14:
        serial_poll(monotonic_ns());
        cpu.a.x = modem_bios(cpu.a.h, cpu.a.l, cpu.d.x);
        serial_edge();
        return 0;
    case 0x16: bios_int16(); return 0;
    case 0x17: cpu.a.h = 0x01; return 0; /* No printer: timeout, not ready. */
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
        if (bios_take_read_retry() && !hle_redirect) {
            /* Blocking BIOS/DOS reads enable keyboard IRQs while waiting.
             * Leave the original interrupt frame intact and retry this stub
             * after INT 1Bh; returning now would fabricate a guest input byte. */
            cpu.ifl = 1;
            return;
        }
        if (!hle_redirect) stub_return(iret);
    } else if (off == STUB_CASEMAP) {
        dos_casemap_upper();
        cpu.ip = pop16();
        cpu.cs = pop16();
    } else if (off == STUB_EXIT) {
        rt_exited = 1;
    } else if (off == STUB_INT8_RETURN) {
        timer_in_service = 0;
        stub_return(1);
    } else if (!dos_abort_untranslated()) {
        rt_fault("jump into BIOS ROM at F000:%04X", off);
    }
}

/* kill -USR1 <pid> logs the registers and the last dispatched addresses. */
static volatile sig_atomic_t dump_requested;
#ifndef __EMSCRIPTEN__
static void on_usr1(int sig) { (void)sig; dump_requested = 1; }
#endif
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

static void timer_poll(uint64_t now) {
    uint64_t period = timer_period_ns();
    if (!timer_next_ns) timer_next_ns = now + period;
    if (now < timer_next_ns) return;
    uint64_t elapsed = (now - timer_next_ns) / period + 1;
    timer_next_ns += elapsed * period;
    if (!cpu.ifl || timer_in_service || (pic_mask & 1)) {
        /* A masked PIC latches one request, it does not queue every edge. */
        if (!timer_pending) timer_pending = 1;
    } else {
        /* Time spent suspended in the browser is not a guest CLI section.
         * Catch up short pauses, with a cap for backgrounded tabs. */
        timer_pending = elapsed >= 64 || timer_pending + elapsed >= 64 ?
                        64 : timer_pending + (unsigned)elapsed;
    }
}

static void dispatch_events(void) {
    static uint64_t last_poll;
    uint64_t now = monotonic_ns();
    if (now - last_poll >= 16000000ull || rt_halted) {
        /* This direct rt_run chain is allowed to suspend Asyncify. All
         * Image.run invocations have returned before we poll the browser. */
        int wait_ms = 0;
        if (rt_halted) {
            uint64_t due = timer_next_ns > now ? timer_next_ns - now : 0;
            wait_ms = due ? (int)((due + 999999ull) / 1000000ull) : 1;
            if (wait_ms > 10) wait_ms = 10;
        }
#ifdef __EMSCRIPTEN__
        /* A busy child may never consume ordinary typeahead. term_idle can
         * return immediately for a nonempty BIOS ring, so guarantee a page
         * turn here regardless, before reading newly delivered host keys. */
        emscripten_sleep(0);
#endif
        term_idle(wait_ms);
        now = monotonic_ns();
        last_poll = now;
    }
    timer_poll(now);
    serial_poll(now);
    if (!cpu.ifl) return;
    if (bios_take_break()) {
        rt_halted = 0;
        if (!dos_abort_break()) cpu_int(0x1b, cpu.ip);
    } else if (timer_pending && !timer_in_service && !(pic_mask & 1)) {
        --timer_pending;
        timer_in_service = 1;
        rt_halted = 0;
        cpu_int(0x08, cpu.ip);
    } else if (serial_pending && !serial_in_service && !timer_in_service && !(pic_mask & 16)) {
        serial_pending = 0;
        serial_in_service = 1;
        rt_halted = 0;
        cpu_int(0x0c, cpu.ip);
    } else if (rt_halted && rd16(0x40, 0x1a) != rd16(0x40, 0x1c)) {
        rt_halted = 0; /* BIOS keyboard service has already queued the key. */
    }
}

void rt_run(void) {
#ifndef __EMSCRIPTEN__
    signal(SIGUSR1, on_usr1);
#endif
    while (!rt_exited) {
        if (rt_budget < 0) rt_budget = 20000;
        dispatch_events();
        if (rt_halted) continue;
        uint32_t L = lin(cpu.cs, cpu.ip);
        recent[recent_at++ & 255] = L;
        if (dump_requested) dump_recent();
        if (!(recent_at & 15)) rt_update_clock(); /* programs wait on 0040:006C */
        if (L >= (STUB_SEG << 4) && L < (STUB_SEG << 4) + STUB_END) {
            stub((uint16_t)(L - (STUB_SEG << 4)));
            continue;
        }
        if (dos_run_psp()) continue;
        if (!run_at(L) && !dos_abort_untranslated())
            rt_fault("no translated code at %04X:%04X", cpu.cs, cpu.ip);
    }
}
