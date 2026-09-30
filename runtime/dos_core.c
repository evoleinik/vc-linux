/* DOS kernel pieces that are about processes and memory, not files:
 * the MCB chain, PSPs, environments, EXEC and terminate, interrupt vectors,
 * version and system info, plus the minor BIOS and multiplex interrupts.
 *
 * Everything VC can see is real bytes in `mem`: the MCB chain, PSPs, the
 * List of Lists. VC edits MCB headers directly and walks the chain itself,
 * so this code reads the chain back from memory every time. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cp866.h"
#include "hle.h"
#include "rt.h"

#define DOS_SEG     0x0070u   /* DOS data */
#define INDOS_OFF   0x0011u
#define LOL_OFF     0x0026u   /* List of Lists; first MCB word just below */
#define CDS_OFF     0x0100u
#define SCRATCH_OFF 0x0700u   /* 256 bytes for paths we hand to dos_fs */
#define FIRST_MCB   0x00F0u
#define MEM_TOP     0xA000u   /* 640 KB */

static uint16_t cur_psp;
static uint8_t alloc_strategy;
static uint16_t last_retcode;
static uint8_t break_flag;

typedef struct {
    uint16_t child;    /* PSP of the running child */
    Cpu parent;        /* parent registers at its EXEC call */
    uint16_t dta_seg, dta_off;
} Proc;
static Proc procs[8];
static int nprocs;

/* ---- MCB chain ------------------------------------------------------------ */

static uint16_t first_mcb(void) { return rd16(DOS_SEG, LOL_OFF - 2); }
static uint8_t mcb_type(uint16_t m) { return rd8(m, 0); }
static uint16_t mcb_owner(uint16_t m) { return rd16(m, 1); }
static uint16_t mcb_size(uint16_t m) { return rd16(m, 3); }
static void mcb_set(uint16_t m, uint8_t type, uint16_t owner, uint16_t size) {
    wr8(m, 0, type);
    wr16(m, 1, owner);
    wr16(m, 3, size);
}
static int mcb_ok(uint16_t m) { uint8_t t = mcb_type(m); return t == 'M' || t == 'Z'; }
static uint16_t mcb_next(uint16_t m) { return (uint16_t)(m + 1 + mcb_size(m)); }

/* Merge runs of free blocks. Returns 0, or 7 if the chain is broken. */
static int mcb_merge(void) {
    uint16_t m = first_mcb();
    for (;;) {
        if (!mcb_ok(m)) return 7;
        if (mcb_type(m) == 'Z') return 0;
        uint16_t n = mcb_next(m);
        if (!mcb_ok(n)) return 7;
        if (mcb_owner(m) == 0 && mcb_owner(n) == 0) {
            mcb_set(m, mcb_type(n), 0, (uint16_t)(mcb_size(m) + 1 + mcb_size(n)));
            continue;
        }
        m = n;
    }
}

static int mem_alloc(uint16_t paras, uint16_t owner, uint16_t *seg, uint16_t *largest) {
    int err = mcb_merge();
    *largest = 0;
    if (err) return err;
    uint16_t pick = 0, m = first_mcb();
    for (;;) {
        if (mcb_owner(m) == 0) {
            uint16_t sz = mcb_size(m);
            if (sz > *largest) *largest = sz;
            if (sz >= paras) {
                int strat = alloc_strategy & 3;
                if (!pick || strat == 2 || (strat == 1 && sz < mcb_size(pick))) {
                    if (!pick || strat != 0) pick = m;
                }
            }
        }
        if (mcb_type(m) == 'Z') break;
        m = mcb_next(m);
    }
    if (!pick) return 8;
    uint16_t sz = mcb_size(pick);
    if (sz == paras) {
        wr16(pick, 1, owner);
        *seg = (uint16_t)(pick + 1);
    } else if ((alloc_strategy & 3) == 2) { /* last fit: take the top */
        uint16_t rest = (uint16_t)(sz - paras - 1);
        uint16_t top = (uint16_t)(pick + 1 + rest);
        mcb_set(top, mcb_type(pick), owner, paras);
        mcb_set(pick, 'M', 0, rest);
        *seg = (uint16_t)(top + 1);
    } else {
        uint16_t rest = (uint16_t)(pick + 1 + paras);
        mcb_set(rest, mcb_type(pick), 0, (uint16_t)(sz - paras - 1));
        mcb_set(pick, 'M', owner, paras);
        *seg = (uint16_t)(pick + 1);
    }
    return 0;
}

static int mem_free(uint16_t seg) {
    uint16_t m = (uint16_t)(seg - 1);
    if (!mcb_ok(m)) return 9;
    wr16(m, 1, 0);
    return 0;
}

static int mem_resize(uint16_t seg, uint16_t paras, uint16_t *maxp) {
    uint16_t m = (uint16_t)(seg - 1);
    if (!mcb_ok(m)) return 9;
    int err = mcb_merge();
    if (err) return err;
    uint16_t sz = mcb_size(m);
    if (paras <= sz) {
        if (paras < sz) {
            uint16_t rest = (uint16_t)(m + 1 + paras);
            mcb_set(rest, mcb_type(m), 0, (uint16_t)(sz - paras - 1));
            mcb_set(m, 'M', mcb_owner(m), paras);
            mcb_merge();
        }
        return 0;
    }
    uint32_t avail = sz;
    uint16_t n = mcb_next(m);
    if (mcb_type(m) == 'M' && mcb_ok(n) && mcb_owner(n) == 0) avail = sz + 1u + mcb_size(n);
    if (avail < paras) { *maxp = (uint16_t)avail; return 8; }
    uint8_t last = mcb_type(n);
    if (avail == paras) {
        mcb_set(m, last, mcb_owner(m), paras);
    } else {
        uint16_t rest = (uint16_t)(m + 1 + paras);
        mcb_set(rest, last, 0, (uint16_t)(avail - paras - 1));
        mcb_set(m, 'M', mcb_owner(m), paras);
    }
    return 0;
}

static void mem_free_owned(uint16_t psp) {
    uint16_t m = first_mcb();
    while (mcb_ok(m)) {
        if (mcb_owner(m) == psp) wr16(m, 1, 0);
        if (mcb_type(m) == 'Z') break;
        m = mcb_next(m);
    }
    mcb_merge();
}

/* ---- helpers -------------------------------------------------------------- */

static void set_vec(uint8_t n, uint16_t seg, uint16_t off) { wr16(0, (uint16_t)(n * 4), off); wr16(0, (uint16_t)(n * 4 + 2), seg); }
static uint16_t vec_off(uint8_t n) { return rd16(0, (uint16_t)(n * 4)); }
static uint16_t vec_seg(uint8_t n) { return rd16(0, (uint16_t)(n * 4 + 2)); }

static void fs_call(uint8_t ah, uint16_t ds, uint16_t dx) {
    Cpu save = cpu;
    cpu.a.h = ah;
    cpu.ds = ds;
    cpu.d.x = dx;
    dos_fs_int21();
    cpu = save;
}
static void get_dta(uint16_t *seg, uint16_t *off) {
    Cpu save = cpu;
    cpu.a.h = 0x2F;
    dos_fs_int21();
    *seg = cpu.es;
    *off = cpu.b.x;
    cpu = save;
}

static void fail(uint16_t err) { cpu.a.x = err; cpu.cf = 1; }

/* DOS string at seg:off to a C string, CP866 bytes kept as they are. */
static void get_str(uint16_t seg, uint16_t off, char *out, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        uint8_t c = rd8(seg, (uint16_t)(off + i));
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static void cp866_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; i++) {
        uint32_t u = in[i] < 0x80 ? in[i] : cp866_to_ucs(in[i]);
        if (u < 0x80) out[o++] = (char)u;
        else if (u < 0x800) { out[o++] = (char)(0xC0 | u >> 6); out[o++] = (char)(0x80 | (u & 63)); }
        else { out[o++] = (char)(0xE0 | u >> 12); out[o++] = (char)(0x80 | ((u >> 6) & 63)); out[o++] = (char)(0x80 | (u & 63)); }
    }
    out[o] = 0;
}

/* ---- environment and PSP -------------------------------------------------- */

/* Build an environment block: the strings, an empty string, the word 1, then
 * the program's DOS path. Returns its segment, or 0 when out of memory. */
static uint16_t make_env(const char *strings, size_t slen, const char *prog, uint16_t owner) {
    size_t plen = strlen(prog) + 1;
    size_t total = slen + 1 + 2 + plen;
    uint16_t seg, largest;
    if (mem_alloc((uint16_t)((total + 15) / 16), owner, &seg, &largest)) return 0;
    uint32_t a = (uint32_t)seg << 4;
    memcpy(&mem[a], strings, slen);
    mem[a + slen] = 0;
    mem[a + slen + 1] = 1;
    mem[a + slen + 2] = 0;
    memcpy(&mem[a + slen + 3], prog, plen);
    return seg;
}

/* The strings part of an existing environment, up to and without the final NUL. */
static size_t env_strings(uint16_t env, char *out, size_t cap) {
    uint32_t a = (uint32_t)env << 4;
    size_t n = 0;
    while (n + 1 < cap && n < 32768) {
        if (mem[a + n] == 0 && (n == 0 || mem[a + n - 1] == 0)) break;
        out[n] = (char)mem[a + n];
        n++;
    }
    if (n == 0) return 0;
    return n; /* ends with the NUL of the last string */
}

static void build_psp(uint16_t psp, uint16_t end_seg, uint16_t env, uint16_t parent,
                      uint16_t term_cs, uint16_t term_ip, const uint8_t *tail) {
    uint32_t a = (uint32_t)psp << 4;
    memset(&mem[a], 0, 256);
    wr8(psp, 0x00, 0xCD);
    wr8(psp, 0x01, 0x20);
    wr16(psp, 0x02, end_seg);
    wr16(psp, 0x0A, term_ip);
    wr16(psp, 0x0C, term_cs);
    wr16(psp, 0x0E, vec_off(0x23));
    wr16(psp, 0x10, vec_seg(0x23));
    wr16(psp, 0x12, vec_off(0x24));
    wr16(psp, 0x14, vec_seg(0x24));
    wr16(psp, 0x16, parent);
    static const uint8_t jft[5] = {1, 1, 1, 0, 2};
    for (int i = 0; i < 20; i++) wr8(psp, (uint16_t)(0x18 + i), i < 5 ? jft[i] : 0xFF);
    wr16(psp, 0x2C, env);
    wr16(psp, 0x32, 20);
    wr16(psp, 0x34, 0x18);
    wr16(psp, 0x36, psp);
    wr16(psp, 0x38, 0xFFFF);
    wr16(psp, 0x3A, 0xFFFF);
    wr8(psp, 0x40, 7);
    wr8(psp, 0x41, 10);
    wr8(psp, 0x50, 0xCD);
    wr8(psp, 0x51, 0x21);
    wr8(psp, 0x52, 0xCB);
    for (int i = 0; i < 11; i++) { wr8(psp, (uint16_t)(0x5D + i), ' '); wr8(psp, (uint16_t)(0x6D + i), ' '); }
    if (tail) memcpy(&mem[a + 0x80], tail, 128);
    else wr8(psp, 0x81, 0x0D);
}

/* ---- loading a translated image -------------------------------------------- */

/* Allocate, build the PSP, copy and relocate the image, set the entry
 * registers. Returns 0 or a DOS error. */
static int load_image(const Image *img, uint16_t env_strings_seg, const uint8_t *tail,
                      const char *dos_prog, uint16_t parent, uint16_t term_cs, uint16_t term_ip) {
    char envbuf[32768];
    size_t elen = env_strings_seg ? env_strings(env_strings_seg, envbuf, sizeof envbuf) : 0;
    uint32_t img_paras = (img->size + 15) / 16;
    uint32_t need, want;
    if (img->is_exe) {
        need = 0x10 + img_paras + img->min_alloc;
        want = 0x10 + img_paras + img->max_alloc;
    } else {
        need = 0x10 + (img->size + 0x100 + 15) / 16;
        want = 0xFFFF;
    }
    if (want > 0xFFFF) want = 0xFFFF;
    /* Like DOS: the environment first, then the program takes what it wants. */
    uint16_t env = make_env(envbuf, elen, dos_prog, 0xFFFF);
    if (!env) return 8;
    uint16_t psp, largest;
    int err = mem_alloc((uint16_t)want, 0xFFFF, &psp, &largest);
    if (err == 8 && largest >= need) err = mem_alloc(largest, 0xFFFF, &psp, &largest);
    if (err) { mem_free(env); return err; }
    uint16_t block = mcb_size((uint16_t)(psp - 1));
    wr16((uint16_t)(psp - 1), 1, psp);
    wr16((uint16_t)(env - 1), 1, psp);

    build_psp(psp, (uint16_t)(psp + block), env, parent ? parent : psp, term_cs, term_ip, tail);
    const char *base = strrchr(dos_prog, '\\');
    base = base ? base + 1 : dos_prog;
    for (int i = 0; i < 8; i++) {
        char c = base[i];
        if (!c || c == '.') { while (i < 8) wr8((uint16_t)(psp - 1), (uint16_t)(8 + i++), 0); break; }
        wr8((uint16_t)(psp - 1), (uint16_t)(8 + i), (uint8_t)c);
    }

    uint16_t loadseg = (uint16_t)(psp + 0x10);
    memcpy(&mem[(uint32_t)loadseg << 4], img->bytes, img->size);
    if (img->is_exe)
        for (uint32_t i = 0; i < img->nrelocs; i++) {
            uint32_t at = ((uint32_t)loadseg << 4) + img->relocs[i];
            uint16_t v = (uint16_t)(mem[at] | mem[at + 1] << 8);
            v = (uint16_t)(v + loadseg);
            mem[at] = (uint8_t)v;
            mem[at + 1] = (uint8_t)(v >> 8);
        }
    rt_register_image(img, loadseg);

    cur_psp = psp;
    fs_call(0x1A, psp, 0x80);
    memset(&cpu.a, 0, sizeof cpu.a);
    cpu.b.x = cpu.c.x = cpu.d.x = cpu.si = cpu.di = cpu.bp = 0;
    cpu.ds = cpu.es = psp;
    if (img->is_exe) {
        cpu.cs = (uint16_t)(loadseg + img->hdr_cs);
        cpu.ip = img->hdr_ip;
        cpu.ss = (uint16_t)(loadseg + img->hdr_ss);
        cpu.sp = img->hdr_sp;
    } else {
        cpu.cs = cpu.ss = psp;
        cpu.ip = 0x100;
        cpu.sp = 0xFFFE;
        wr16(psp, 0xFFFE, 0);
    }
    cpu.ifl = 1;
    cpu.df = 0;
    return 0;
}

static const Image *known_image(const char *dos_path) {
    const char *b = dos_path;
    for (const char *p = dos_path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    if (!strcasecmp(b, "VC.OVL")) return &image_vc_ovl;
    if (!strcasecmp(b, "VC.COM")) return &image_vc_com;
    return NULL;
}

/* ---- running host commands ------------------------------------------------ */

static int host_cwd(char *out, size_t cap) {
    wr8(DOS_SEG, SCRATCH_OFF, '.');
    wr8(DOS_SEG, SCRATCH_OFF + 1, 0);
    return dos_fs_to_host(DOS_SEG, SCRATCH_OFF, out, cap);
}

static int host_run(const char *cmd) {
    char cwd[4096];
    if (host_cwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    rt_log("run: %s (in %s)", cmd, cwd);
    term_suspend();
    struct sigaction ign = {.sa_handler = SIG_IGN}, oint, oquit;
    sigaction(SIGINT, &ign, &oint);
    sigaction(SIGQUIT, &ign, &oquit);
    int status = 127;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (chdir(cwd) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0) ;
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    sigaction(SIGINT, &oint, NULL);
    sigaction(SIGQUIT, &oquit, NULL);
    term_resume();
    return status;
}

/* A DOS command tail (count byte, text) to a host command line. "/C cmd"
 * means run cmd with the shell. Anything else runs the program itself. */
static int exec_host(const char *host_prog, const uint8_t *tail) {
    uint8_t n = tail[0] > 126 ? 126 : tail[0];
    char text[1024];
    cp866_to_utf8(tail + 1, n, text, sizeof text);
    char *t = text;
    while (*t == ' ' || *t == '\t') t++;
    char cmd[4096];
    if ((t[0] == '/' || t[0] == '-') && (t[1] == 'c' || t[1] == 'C') && (t[2] == ' ' || t[2] == 0)) {
        snprintf(cmd, sizeof cmd, "%s", t + 2);
    } else {
        snprintf(cmd, sizeof cmd, "'%s' %s", host_prog, t);
    }
    return host_run(cmd);
}

/* ---- EXEC and terminate --------------------------------------------------- */

static void do_exec(void) {
    if (cpu.a.l != 0) { fail(1); return; }
    char dos_prog[260];
    get_str(cpu.ds, cpu.d.x, dos_prog, sizeof dos_prog);
    uint16_t pb_seg = cpu.es, pb = cpu.b.x;
    uint16_t envseg = rd16(pb_seg, pb);
    uint16_t tail_off = rd16(pb_seg, (uint16_t)(pb + 2)), tail_seg = rd16(pb_seg, (uint16_t)(pb + 4));
    uint8_t tail[128];
    for (int i = 0; i < 128; i++) tail[i] = rd8(tail_seg, (uint16_t)(tail_off + i));
    char shown[160];
    int n = 0;
    for (int i = 1; i < 128 && tail[i] != 0x0D && n < 150; i++)
        n += tail[i] >= 32 && tail[i] < 127 ? snprintf(shown + n, sizeof shown - n, "%c", tail[i])
                                            : snprintf(shown + n, sizeof shown - n, "\\x%02X", tail[i]);
    shown[n] = 0;
    rt_log("exec %s tail[%u] \"%s\"", dos_prog, tail[0], shown);

    const Image *img = known_image(dos_prog);
    if (!img) {
        char host[4096];
        if (dos_fs_to_host(cpu.ds, cpu.d.x, host, sizeof host) || access(host, X_OK)) { fail(2); return; }
        last_retcode = (uint16_t)(exec_host(host, tail) & 0xFF);
        cpu.cf = 0;
        return;
    }
    if (nprocs == (int)(sizeof procs / sizeof procs[0])) { fail(8); return; }
    Proc *p = &procs[nprocs];
    p->parent = cpu;
    get_dta(&p->dta_seg, &p->dta_off);
    uint16_t ret_ip = rd16(cpu.ss, cpu.sp), ret_cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
    uint16_t parent_psp = cur_psp;
    int err = load_image(img, envseg ? envseg : rd16(cur_psp, 0x2C), tail, dos_prog, parent_psp, ret_cs, ret_ip);
    if (err) { cpu = p->parent; fail((uint16_t)err); return; }
    p->child = cur_psp;
    nprocs++;
    hle_redirect = 1;
}

static void terminate(uint8_t code, int tsr, uint16_t keep) {
    uint16_t psp = cur_psp;
    last_retcode = (uint16_t)(code | (tsr ? 0x300 : 0));
    rt_log("terminate psp %04X code %u%s", psp, code, tsr ? " (resident)" : "");
    if (tsr) {
        uint16_t maxp;
        mem_resize(psp, keep < 6 ? 6 : keep, &maxp);
    } else {
        mem_free_owned(psp);
    }
    set_vec(0x22, rd16(psp, 0x0C), rd16(psp, 0x0A));
    set_vec(0x23, rd16(psp, 0x10), rd16(psp, 0x0E));
    set_vec(0x24, rd16(psp, 0x14), rd16(psp, 0x12));
    uint16_t term_ip = rd16(psp, 0x0A), term_cs = rd16(psp, 0x0C);
    if (nprocs > 0 && procs[nprocs - 1].child == psp) {
        Proc *p = &procs[--nprocs];
        cpu = p->parent;
        cur_psp = rd16(psp, 0x16);
        fs_call(0x1A, p->dta_seg, p->dta_off);
        pop16();
        pop16();
        uint16_t fl = pop16();
        flags_set(fl);
        cpu.cf = 0;
    } else {
        cur_psp = rd16(psp, 0x16);
    }
    cpu.cs = term_cs;
    cpu.ip = term_ip;
    hle_redirect = 1;
}

/* ---- List of Lists and DOS data ------------------------------------------- */

static void init_dos_data(void) {
    uint32_t a = (uint32_t)DOS_SEG << 4;
    memset(&mem[a], 0, 0x800);
    wr16(DOS_SEG, LOL_OFF - 2, FIRST_MCB);
    for (int i = 0; i < 0x16; i += 2) wr16(DOS_SEG, (uint16_t)(LOL_OFF + i), 0xFFFF); /* DPB, SFT, devices, buffers: none */
    wr16(DOS_SEG, LOL_OFF + 0x10, 512);
    wr16(DOS_SEG, LOL_OFF + 0x16, CDS_OFF);
    wr16(DOS_SEG, LOL_OFF + 0x18, DOS_SEG);
    wr16(DOS_SEG, LOL_OFF + 0x1A, 0xFFFF);
    wr16(DOS_SEG, LOL_OFF + 0x1C, 0xFFFF);
    wr8(DOS_SEG, LOL_OFF + 0x20, 3);
    wr8(DOS_SEG, LOL_OFF + 0x21, 3);
    /* NUL device header, end of the device chain */
    uint16_t nul = LOL_OFF + 0x22;
    wr16(DOS_SEG, nul, 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 2), 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 4), 0x8004);
    memcpy(&mem[a + nul + 10], "NUL     ", 8);
    /* CDS: A: and B: invalid, C: a physical drive at C:\ */
    for (int d = 0; d < 3; d++) {
        uint16_t e = (uint16_t)(CDS_OFF + d * 0x58);
        wr8(DOS_SEG, e, (uint8_t)('A' + d));
        wr8(DOS_SEG, (uint16_t)(e + 1), ':');
        wr8(DOS_SEG, (uint16_t)(e + 2), '\\');
        wr16(DOS_SEG, (uint16_t)(e + 0x43), d == 2 ? 0x4000 : 0);
        wr16(DOS_SEG, (uint16_t)(e + 0x4F), 2);
    }
    mcb_set(FIRST_MCB, 'Z', 0, (uint16_t)(MEM_TOP - FIRST_MCB - 1));
}

void dos_core_init(void) {
    memset(mem, 0, MEM_SIZE);
    for (int n = 0; n < 256; n++) set_vec((uint8_t)n, STUB_SEG, (uint16_t)n);
    memset(&mem[(uint32_t)STUB_SEG << 4], 0xCF, STUB_END); /* IRET bytes, for anyone who looks */
    mem[0xFFFFE] = 0xFC;                                  /* machine model: AT */
    init_dos_data();
    wr16(0x40, 0x13, 640);                                /* base memory in KB */
}

/* ---- INT 21h -------------------------------------------------------------- */

int dos_core_int21(void) {
    uint16_t seg, largest, maxp;
    int err;
    switch (cpu.a.h) {
    case 0x00: terminate(0, 0, 0); return 1;
    case 0x25: set_vec(cpu.a.l, cpu.ds, cpu.d.x); return 1;
    case 0x35: cpu.es = vec_seg(cpu.a.l); cpu.b.x = vec_off(cpu.a.l); return 1;
    case 0x30: cpu.a.l = 7; cpu.a.h = 10; cpu.b.h = 0xFF; cpu.b.l = 0; cpu.c.x = 0; return 1;
    case 0x31: terminate(cpu.a.l, 1, cpu.d.x); return 1;
    case 0x33:
        switch (cpu.a.l) {
        case 0x00: cpu.d.l = break_flag; return 1;
        case 0x01: break_flag = cpu.d.l & 1; return 1;
        case 0x05: cpu.d.l = 3; return 1;
        case 0x06: cpu.b.l = 7; cpu.b.h = 10; cpu.d.l = 0; cpu.d.h = 0; return 1;
        default: cpu.a.l = 0xFF; return 1;
        }
    case 0x34: cpu.es = DOS_SEG; cpu.b.x = INDOS_OFF; return 1;
    case 0x37: if (cpu.a.l == 0) { cpu.a.l = 0; cpu.d.l = '/'; } else cpu.a.l = 0xFF; return 1;
    case 0x48:
        err = mem_alloc(cpu.b.x, cur_psp, &seg, &largest);
        if (err) { fail((uint16_t)err); cpu.b.x = largest; }
        else { cpu.a.x = seg; cpu.cf = 0; }
        return 1;
    case 0x49:
        err = mem_free(cpu.es);
        if (err) fail((uint16_t)err); else cpu.cf = 0;
        return 1;
    case 0x4A:
        maxp = 0;
        err = mem_resize(cpu.es, cpu.b.x, &maxp);
        if (err) { fail((uint16_t)err); cpu.b.x = maxp; }
        else cpu.cf = 0;
        return 1;
    case 0x4B: do_exec(); return 1;
    case 0x4C: terminate(cpu.a.l, 0, 0); return 1;
    case 0x4D: cpu.a.x = last_retcode; last_retcode = 0; cpu.cf = 0; return 1;
    case 0x50: cur_psp = cpu.b.x; return 1;
    case 0x51: case 0x62: cpu.b.x = cur_psp; return 1;
    case 0x52: cpu.es = DOS_SEG; cpu.b.x = LOL_OFF; return 1;
    case 0x55:
        memcpy(&mem[(uint32_t)cpu.d.x << 4], &mem[(uint32_t)cur_psp << 4], 256);
        wr16(cpu.d.x, 0x16, cur_psp);
        wr16(cpu.d.x, 0x02, cpu.si);
        cur_psp = cpu.d.x;
        return 1;
    case 0x58:
        switch (cpu.a.l) {
        case 0x00: cpu.a.x = alloc_strategy; cpu.cf = 0; return 1;
        case 0x01: alloc_strategy = cpu.b.l; cpu.cf = 0; return 1;
        case 0x02: cpu.a.l = 0; cpu.cf = 0; return 1;
        case 0x03: if (cpu.b.x == 0) cpu.cf = 0; else fail(1); return 1;
        default: fail(1); return 1;
        }
    case 0x63: cpu.ds = DOS_SEG; cpu.si = SCRATCH_OFF + 0xF8; wr16(DOS_SEG, SCRATCH_OFF + 0xF8, 0); cpu.a.l = 0; cpu.cf = 0; return 1;
    case 0x66:
        if (cpu.a.l == 1) { cpu.b.x = cpu.d.x = 866; cpu.cf = 0; }
        else if (cpu.a.l == 2) cpu.cf = 0;
        else fail(1);
        return 1;
    default:
        return 0;
    }
}

/* ---- other software interrupts -------------------------------------------- */

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

int dos_int_other(uint8_t n) {
    switch (n) {
    case 0x11: cpu.a.x = rd16(0x40, 0x10); return 0;
    case 0x12: cpu.a.x = rd16(0x40, 0x13); return 0;
    case 0x15: cpu.a.h = 0x86; cpu.cf = 1; return 0;
    case 0x1A: {
        rt_update_clock();
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        switch (cpu.a.h) {
        case 0x00: cpu.c.x = rd16(0x40, 0x6E); cpu.d.x = rd16(0x40, 0x6C); cpu.a.l = rd8(0x40, 0x70); wr8(0x40, 0x70, 0); return 0;
        case 0x02: cpu.c.h = bcd(tm.tm_hour); cpu.c.l = bcd(tm.tm_min); cpu.d.h = bcd(tm.tm_sec); cpu.d.l = 0; cpu.cf = 0; return 0;
        case 0x04: cpu.c.h = bcd((tm.tm_year + 1900) / 100); cpu.c.l = bcd(tm.tm_year % 100); cpu.d.h = bcd(tm.tm_mon + 1); cpu.d.l = bcd(tm.tm_mday); cpu.cf = 0; return 0;
        default: cpu.cf = 1; return 0;
        }
    }
    case 0x20: terminate(0, 0, 0); return 0;
    case 0x27: terminate(0, 1, (uint16_t)((cpu.d.x + 15) >> 4)); return 0;
    case 0x28: term_idle(0); return 0;
    case 0x2E: {
        uint8_t line[128];
        for (int i = 0; i < 128; i++) line[i] = rd8(cpu.ds, (uint16_t)(cpu.si + i));
        uint8_t len = line[0] > 126 ? 126 : line[0];
        char cmd[1024];
        cp866_to_utf8(line + 1, len, cmd, sizeof cmd);
        char *cr = strchr(cmd, '\r');
        if (cr) *cr = 0;
        last_retcode = (uint16_t)(host_run(cmd) & 0xFF);
        cpu.a.x = 0;
        return 0;
    }
    case 0x2F:
        if (cpu.a.x == 0x1680) { term_idle(1); cpu.a.l = 0; }
        else if (cpu.a.x == 0x4300) cpu.a.l = 0;
        return 0;
    case 0x67: cpu.a.h = 0x84; return 0;
    case 0x24: cpu.a.l = 3; return 1;
    default: return 1;
    }
}

/* ---- the first process ---------------------------------------------------- */

void dos_start(const char *dos_prog, const uint8_t *tail, int tail_len) {
    char env[2048];
    size_t n = 0;
    const char *tmp = getenv("TMPDIR");
    char tmpdos[512] = "C:\\tmp";
    if (tmp && tmp[0] == '/') {
        snprintf(tmpdos, sizeof tmpdos, "C:%s", tmp);
        for (char *p = tmpdos; *p; p++) if (*p == '/') *p = '\\';
    }
    const char *vars[] = {"COMSPEC=C:\\bin\\sh", "PATH=C:\\", "PROMPT=$P$G", NULL};
    for (int i = 0; vars[i]; i++) n += (size_t)snprintf(env + n, sizeof env - n, "%s", vars[i]) + 1;
    n += (size_t)snprintf(env + n, sizeof env - n, "TEMP=%s", tmpdos) + 1;
    n += (size_t)snprintf(env + n, sizeof env - n, "TMP=%s", tmpdos) + 1;

    uint16_t envseg, largest;
    if (mem_alloc((uint16_t)((n + 1 + 15) / 16), 0, &envseg, &largest)) rt_fault("no memory for the environment");
    memcpy(&mem[(uint32_t)envseg << 4], env, n);
    mem[((uint32_t)envseg << 4) + n] = 0;

    uint8_t t[128] = {0};
    if (tail_len > 126) tail_len = 126;
    t[0] = (uint8_t)tail_len;
    memcpy(t + 1, tail, (size_t)tail_len);
    t[1 + tail_len] = 0x0D;
    if (load_image(&image_vc_com, envseg, t, dos_prog, 0, STUB_SEG, STUB_EXIT))
        rt_fault("cannot load VC.COM");
    mem_free(envseg);
}
