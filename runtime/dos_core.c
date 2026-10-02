/* DOS kernel pieces that are about processes and memory, not files:
 * the MCB chain, PSPs, environments, EXEC and terminate, interrupt vectors,
 * version and system info, plus the minor BIOS and multiplex interrupts.
 *
 * Everything VC can see is real bytes in `mem`: the MCB chain, PSPs, the
 * List of Lists. VC edits MCB headers directly and walks the chain itself,
 * so this code reads the chain back from memory every time. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cp866.h"
#include "dos_fs.h"
#include "guest_mem.h"
#include "hle.h"
#include "rt.h"

#ifdef __EMSCRIPTEN__
#include "web_programs.h"
#include "web_files.h"
#endif

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
static int door_mode;
/* Only these two installed paths are VC 4.99's internal reloads. A different
 * directory may legitimately contain another byte-matched VC.COM. */
static char builtin_dos[2][128], builtin_short[2][128], builtin_host[2][4096];
static char vc405_directory[128];

void dos_core_set_door(int enabled) { door_mode = !!enabled; }

typedef struct {
    uint16_t child;    /* PSP of the running child */
    const Image *image;
    RtProcessState *machine;
    uint32_t break_vector; /* Inherited INT 1Bh is not the child's hook. */
    uint8_t exec_break, exec_break_state; /* 0 idle, 1 queried, 2 temporarily off. */
    Cpu parent;        /* parent registers at its EXEC call */
    uint16_t dta_seg, dta_off;
    char temp_dir[32]; /* Private, short VZ swap directory; not guest memory. */
    char vc_startup[128]; /* DOS2 loader finishes PSP/env after AH55. */
} Proc;
static Proc procs[8];
static int nprocs;

/* AH55 also creates PSPs for programs such as DEBUG that later switch back
 * with AH50 without ever EXECing the copy. Their inherited handle references
 * belong to the creating program, not to the guest's current-PSP register. */
typedef struct PspCopy {
    uint16_t psp, owner;
    struct PspCopy *next;
} PspCopy;
static PspCopy *psp_copies;

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
/* Where a block ends, without 16-bit wrap. Past FFFFh the chain is broken. */
static uint32_t mcb_end(uint16_t m) { return (uint32_t)m + 1 + mcb_size(m); }

/* Merge runs of free blocks. Returns 0, or 7 if the chain is broken.
 * A guest can forge any header. A block, or two blocks merged, that ends
 * past FFFFh would wrap the 16-bit segment back into the chain: that is a
 * broken chain, never a cycle the native walk follows forever. Each step
 * moves m up or extends its end, so the walk is bounded, and a successful
 * merge proves the chain from first_mcb() ascends to a 'Z' inside 64 KiB
 * paragraphs. The later walks in mem_alloc and mem_resize rely on that. */
static int mcb_merge(void) {
    uint16_t m = first_mcb();
    for (;;) {
        if (!mcb_ok(m) || mcb_end(m) > 0xFFFF) return 7;
        if (mcb_type(m) == 'Z') return 0;
        uint16_t n = (uint16_t)mcb_end(m);
        if (!mcb_ok(n) || mcb_end(n) > 0xFFFF) return 7;
        if (mcb_owner(m) == 0 && mcb_owner(n) == 0) {
            mcb_set(m, mcb_type(n), 0, (uint16_t)(mcb_end(n) - m - 1));
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
    while (mcb_ok(m) && mcb_end(m) <= 0xFFFF) { /* forged wrap: see mcb_merge */
        if (mcb_owner(m) == psp) wr16(m, 1, 0);
        if (mcb_type(m) == 'Z') break;
        m = (uint16_t)mcb_end(m);
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

/* SETVER is keyed to the byte-matched translation, never its launch name.
 * Keeping this data together prevents DOS 2's exact version checks from
 * changing the version seen by VC or by any unrelated translated program. */
static const struct { const char *image; uint16_t version; } version_table[] = {
    {"COMMAND.COM", 0x0002}, {"EDLIN.COM", 0x0002}, {"DEBUG.COM", 0x0002},
    {"FIND.EXE", 0x0002}, {"MORE.COM", 0x0002}, {"SORT.EXE", 0x0002},
    {"FC.EXE", 0x0002},
};

static uint16_t image_version(const Image *img) {
    for (size_t i = 0; i < sizeof version_table / sizeof version_table[0]; ++i)
        if (!strcmp(img->name, version_table[i].image)) return version_table[i].version;
    return 0x0A07;
}

/* Build an environment block: the strings, an empty string, the word 1, then
 * the program's DOS path. Returns its segment, or 0 when out of memory. */
static uint16_t make_env(const char *strings, size_t slen, const char *prog, uint16_t owner) {
    /* With no variables, the strings list still contributes one NUL before
     * the separator. VC scans for two NULs before the executable trailer. */
    if (!slen) { strings = ""; slen = 1; }
    size_t plen = strlen(prog) + 1;
    size_t total = slen + 1 + 2 + plen;
    uint16_t seg, largest;
    if (mem_alloc((uint16_t)((total + 15) / 16), owner, &seg, &largest)) return 0;
    uint32_t a = (uint32_t)seg << 4;
    static const uint8_t separator[3] = {0, 1, 0};
    /* A forged MCB chain can place the block anywhere: check it whole. */
    if (!guest_span(a, total) || guest_write(a, strings, slen) ||
        guest_write(a + slen, separator, sizeof separator) ||
        guest_write(a + slen + 3, prog, plen)) {
        mem_free(seg);
        return 0;
    }
    return seg;
}

/* The strings part of an existing environment, up to and without the final NUL. */
static size_t env_strings(uint16_t env, char *out, size_t cap) {
    const uint8_t *s = guest_span((uint32_t)env << 4, 32768);
    size_t n = 0;
    if (!s) return 0;
    while (n + 1 < cap && n < 32768) {
        if (s[n] == 0 && (n == 0 || s[n - 1] == 0)) break;
        out[n] = (char)s[n];
        n++;
    }
    if (n == 0) return 0;
    return n; /* ends with the NUL of the last string */
}

static int build_psp(uint16_t psp, uint16_t end_seg, uint16_t env, uint16_t parent,
                     uint16_t term_cs, uint16_t term_ip, const uint8_t *tail) {
    uint32_t a = (uint32_t)psp << 4;
    if (!guest_span(a, 256) || guest_fill(a, 0, 256)) return 8;
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
    static const uint8_t jft[5] = {0, 1, 2, 3, 4};
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
    if (tail) return guest_write(a + 0x80, tail, 128) ? 8 : 0;
    wr8(psp, 0x81, 0x0D);
    return 0;
}

/* A grown JFT lives in an ordinary DOS block owned by its PSP. Allocate the
 * full supported capacity once: later smaller requests leave active handles
 * intact, and normal process cleanup frees the block with its other memory. */
static int grow_job_file_table(uint16_t requested) {
    if (requested > DOS_MAX_HANDLES) return 4;
    if (!cur_psp) return 6;
    uint16_t count = rd16(cur_psp, 0x32);
    uint32_t old = lin(rd16(cur_psp, 0x36), rd16(cur_psp, 0x34));
    if (count > DOS_MAX_HANDLES || old + count > MEM_SIZE) return 6;
    if (requested <= count) return 0;
    uint16_t seg, largest;
    int error = mem_alloc((DOS_MAX_HANDLES + 15) / 16, cur_psp, &seg, &largest);
    if (error) return error;
    if (guest_fill((uint32_t)seg << 4, 0xFF, DOS_MAX_HANDLES) ||
        guest_move((uint32_t)seg << 4, old, count)) {
        mem_free(seg);
        return 8;
    }
    wr16(cur_psp, 0x32, DOS_MAX_HANDLES);
    wr16(cur_psp, 0x34, 0);
    wr16(cur_psp, 0x36, seg);
    return 0;
}

/* ---- loading a translated image -------------------------------------------- */

/* Allocate, build the PSP, copy and relocate the image, set the entry
 * registers. Returns 0 or a DOS error. */
static int load_image(const Image *img, const char *envbuf, size_t elen, const uint8_t *tail,
                      const char *dos_prog, uint16_t parent, uint16_t term_cs, uint16_t term_ip) {
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
    uint16_t loadseg = (uint16_t)(psp + 0x10);
    /* The caller can forge a free MCB just below 1 MiB + 64 KiB. Refuse an
     * image whose segments would wrap, or that would end past guest memory,
     * before writing anything. */
    if ((uint32_t)psp + 0x10 + img_paras > 0x10000u ||
        !guest_span((uint32_t)psp << 4, 0x100u + img->size)) {
        mem_free(psp);
        mem_free(env);
        return 8;
    }
    wr16((uint16_t)(psp - 1), 1, psp);
    wr16((uint16_t)(env - 1), 1, psp);

    err = build_psp(psp, (uint16_t)(psp + block), env, parent ? parent : psp, term_cs, term_ip, tail);
    if (err) { mem_free(psp); mem_free(env); return err; }
    wr16(psp, 0x40, image_version(img));
    dos_fs_inherit_process(psp, parent);
    const char *base = strrchr(dos_prog, '\\');
    base = base ? base + 1 : dos_prog;
    for (int i = 0; i < 8; i++) {
        char c = base[i];
        if (!c || c == '.') { while (i < 8) wr8((uint16_t)(psp - 1), (uint16_t)(8 + i++), 0); break; }
        wr8((uint16_t)(psp - 1), (uint16_t)(8 + i), (uint8_t)c);
    }

    if (guest_write((uint32_t)loadseg << 4, img->bytes, img->size)) {
        mem_free(psp);
        mem_free(env);
        return 8;
    }
    if (img->is_exe)
        for (uint32_t i = 0; i < img->nrelocs; i++) {
            /* Trusted image data, but still checked against MEM_SIZE. */
            uint8_t *at = img->relocs[i] + 2u <= img->size ?
                guest_span(((uint32_t)loadseg << 4) + img->relocs[i], 2) : NULL;
            if (!at) rt_fault("%s: relocation %u outside its image", img->name, (unsigned)i);
            uint16_t v = (uint16_t)(at[0] | at[1] << 8);
            v = (uint16_t)(v + loadseg);
            at[0] = (uint8_t)v;
            at[1] = (uint8_t)(v >> 8);
        }
    rt_register_image(img, loadseg);

    cur_psp = psp;
    dos_fs_set_process(cur_psp);
    fs_call(0x1A, psp, 0x80);
    /* The native command bridge has no resident COMMAND to prepare EXEC's
     * two default FCBs. Parse the command tail with the same DOS service a
     * shell uses; EDLIN consumes PSP:5Ch for an unqualified filename. */
    uint16_t fcb_status = 0;
    if (tail) {
        cpu.ds = cpu.es = psp;
        cpu.si = 0x81;
        for (unsigned i = 0; i < 2; ++i) {
            cpu.di = (uint16_t)(0x5C + 16 * i);
            cpu.a.x = 0x2901;
            dos_fs_int21();
            if (cpu.a.l == 0xFF) fcb_status |= (uint16_t)(0xFFu << (8 * i));
        }
    }
    memset(&cpu.a, 0, sizeof cpu.a);
    cpu.a.x = fcb_status;
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

/* Non-built-in EXECs select a translation by the complete original file,
 * including its MZ header and relocation table: comparing only the load module
 * would accept a damaged header. VC.COM and VC.OVL bypass this disk check. */
#ifdef __EMSCRIPTEN__
#define IMAGE_COUNT WEB_IMAGE_COUNT
#else
static const Image *const images[] = {
    &image_vc_com, &image_vc_ovl, &image_gwbasic, &image_bootlogo, &image_rogue, &image_vz,
    &image_kermit, &image_command, &image_edlin, &image_debug, &image_find, &image_more,
    &image_sort, &image_fc, &image_vc405, &image_vcsetup405
};
#define IMAGE_COUNT (sizeof images / sizeof images[0])
#endif

static const EmbeddedFile *image_file(size_t index) {
    /* Installation names locate trusted reference bytes, not the program
     * being EXECed. bootLogo's translation keeps its original NASM label;
     * the candidate on disk is still matched only by its complete bytes. */
#ifdef __EMSCRIPTEN__
    const char *name = web_image_filename(index);
#else
    const Image *img = images[index];
    const char *name = img == &image_bootlogo ? "BOOTLOGO.COM" :
                       img == &image_vcsetup405 ? "VC405/VCSETUP.COM" : img->name;
#endif
    for (int i = 0; i < embedded_file_count; i++)
        if (!strcmp(embedded_files[i].name, name)) return &embedded_files[i];
    return NULL;
}

static int file_error(void) {
    return errno == ENOENT ? 2 : errno == ENOTDIR || errno == ENAMETOOLONG ? 3 :
           errno == ENOMEM ? 8 : 5;
}

static int known_image(const char *host, const Image **out) {
    *out = NULL;
    /* An executable must be an ordinary file. Open nonblocking before the
     * type check so EXEC of a FIFO cannot hang while waiting for a writer. */
    int fd = dos_fs_open_readonly(host);
    if (fd < 0) return file_error();
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return 5; }
    FILE *file = fdopen(fd, "rb");
    if (!file) { int err = file_error(); close(fd); return err; }
    int possible = 0;
    for (size_t i = 0; i < IMAGE_COUNT; i++) {
        const EmbeddedFile *f = image_file(i);
        if (f && st.st_size == f->size) possible = 1;
    }
    if (!possible) {
        rt_log("unsupported executable %s: no matching translation", host);
        fclose(file);
        return 11;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *bytes = malloc(size ? size : 1);
    if (!bytes) { fclose(file); return 8; }
    size_t got = fread(bytes, 1, size, file);
    int extra = fgetc(file);
    int err = ferror(file) ? 5 : 11;
    size_t matched = IMAGE_COUNT;
    if (!ferror(file) && got == size && extra == EOF)
        for (size_t i = 0; i < IMAGE_COUNT; i++) {
            const EmbeddedFile *f = image_file(i);
            if (!f || size != f->size) continue;
            const uint8_t *reference = f->data;
#ifdef __EMSCRIPTEN__
            int reference_error = web_files_reference(f, &reference);
            if (reference_error) { err = reference_error; break; }
#endif
            if (!memcmp(bytes, reference, size)) {
                matched = i;
                err = 0;
                break;
            }
        }
    free(bytes);
    fclose(file);
    if (matched != IMAGE_COUNT) {
#ifdef __EMSCRIPTEN__
        err = web_load_image(matched, out);
#else
        *out = images[matched];
#endif
    }
    if (err == 11) rt_log("unsupported executable %s: no matching translation", host);
    return err;
}

static int is_vz_image(const Image *img) {
#ifdef __EMSCRIPTEN__
    return web_image_is_vz(img);
#else
    return img == &image_vz;
#endif
}

static int start_child(const Image *img, const char *dos_prog, const uint8_t *tail, uint16_t envseg);

/* ---- running host commands ------------------------------------------------ */

#ifndef __EMSCRIPTEN__
static int host_cwd(char *out, size_t cap) {
    wr8(DOS_SEG, SCRATCH_OFF, '.');
    wr8(DOS_SEG, SCRATCH_OFF + 1, 0);
    return dos_fs_to_host(DOS_SEG, SCRATCH_OFF, out, cap);
}
#endif

/* Run `script` with /bin/sh in the DOS current directory, the terminal handed
 * back while it runs. Values that come from file names are never pasted into
 * the script: they go in as positional arguments ($0, $1), which the shell
 * does not parse. */
static int host_run(const char *script, const char *arg0, const char *arg1) {
    /* This is the last shared boundary for COMSPEC, INT 2Eh, direct host
     * commands, and editor launch. No door path may reach fork/exec. */
    if (door_mode) {
        static const char message[] = "Bad command or file name\r\n";
        con_write((const uint8_t *)message, sizeof message - 1);
        term_render();
        return 127;
    }
#ifdef __EMSCRIPTEN__
    (void)script;
    (void)arg0;
    (void)arg1;
    const char *message =
        "No shell in the browser, only cd works here. The Linux version runs "
        "commands: github.com/evoleinik/vc-linux\r\n";
    /* VC has switched to its user screen. Writing through CON preserves the
     * message there; a host stdout write would not update its video memory. */
    con_write((const uint8_t *)message, strlen(message));
    term_render();
    return 127;
#else
    char cwd[4096];
    if (host_cwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    rt_log("run: %s%s%s (in %s)", script, arg1 ? " -- " : "", arg1 ? arg1 : "", cwd);
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
        execl("/bin/sh", "sh", "-c", script, arg0 ? arg0 : "sh", arg1, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        pid_t r;
        while ((r = waitpid(pid, &st, 0)) < 0 && errno == EINTR) ;
        if (r == pid) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        else rt_log("waitpid: %s", strerror(errno));
    }
    sigaction(SIGINT, &oint, NULL);
    sigaction(SIGQUIT, &oquit, NULL);
    term_resume();
    return status;
#endif
}

/* UTF-8 path to a NUL-terminated DOS path in code page 866, `/` becoming `\`.
 * Returns 0, or -1 if it does not fit in cap or has a character CP866 lacks. */
static int utf8_to_dos(const char *path, uint8_t *dos, size_t cap) {
    size_t n = 0;
    if (path[0] == '/') {
        if (cap < 3) return -1;
        dos[n++] = 'C';
        dos[n++] = ':';
    }
    for (const unsigned char *p = (const unsigned char *)path; *p;) {
        uint32_t u = *p++;
        if (u >= 0xC0) {
            int more = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
            u &= 0x3Fu >> more;
            while (more-- && (*p & 0xC0) == 0x80) u = u << 6 | (*p++ & 0x3F);
        }
        int c = u == '/' ? '\\' : ucs_to_cp866(u);
        if (c < 0 || n + 1 >= cap) return -1;
        dos[n++] = (uint8_t)c;
    }
    dos[n] = 0;
    return 0;
}

/* COMMAND.COM ran `cd` itself, changing the DOS current directory, and VC's
 * panel follows it. A child shell cannot do that for us, so do it here.
 * Linux forms work too: `cd` alone and `~` mean $HOME, `/` separates, and one
 * argument may be quoted. Anything shell-like goes to the shell instead.
 * Returns -1 if cmd is not a plain cd, else its exit status. */
static int internal_cd(const char *cmd) {
    while (*cmd == ' ') cmd++;
    size_t kw = !strncasecmp(cmd, "chdir", 5) ? 5 : !strncasecmp(cmd, "cd", 2) ? 2 : 0;
    if (!kw || (cmd[kw] && cmd[kw] != ' ' && cmd[kw] != '\\' && cmd[kw] != '/' && cmd[kw] != '.'))
        return -1;
    const char *arg = cmd + kw;
    while (*arg == ' ') arg++;
    size_t alen = strlen(arg);
    while (alen && arg[alen - 1] == ' ') alen--;
    char word[1024];
    if (alen >= sizeof word) return -1;
    if (alen >= 2 && (arg[0] == '"' || arg[0] == '\'') && arg[alen - 1] == arg[0]) {
        /* one quoted word: no other quote of that kind inside, and inside
         * double quotes nothing the shell would expand */
        memcpy(word, arg + 1, alen - 2);
        word[alen - 2] = 0;
        if (strchr(word, arg[0]) || (arg[0] == '"' && strpbrk(word, "$`\\"))) return -1;
    } else {
        memcpy(word, arg, alen);
        word[alen] = 0;
        if (strpbrk(word, ";&|<>`$(){}*?\"' \t")) return -1;
    }
    char path[2048];
    const char *home = door_mode ? NULL : getenv("HOME");
    if (door_mode && (!word[0] || (word[0] == '~' && (!word[1] || word[1] == '/'))))
        snprintf(path, sizeof path, "H:\\%s", word[0] ? word + 1 : "");
    else if (door_mode && word[0] == '/') snprintf(path, sizeof path, "H:%s", word);
    else if (!word[0]) snprintf(path, sizeof path, "%s", home ? home : "/");
    else if (word[0] == '~' && (!word[1] || word[1] == '/'))
        snprintf(path, sizeof path, "%s%s", home ? home : "", word + 1);
    else snprintf(path, sizeof path, "%s", word);
    /* An absolute path inside $HOME goes to drive H:, anything else on / to C:. */
    char onh[2048];
    const char *dospath = path;
    if (path[0] == '/' && home && home[0] == '/') {
        char *home_real = realpath(home, NULL), *path_real = realpath(path, NULL);
        const char *p = path_real ? path_real : path;
        size_t hn = home_real ? strlen(home_real) : 0;
        if (hn > 1 && !strncmp(p, home_real, hn) && (p[hn] == '/' || !p[hn])) {
            snprintf(onh, sizeof onh, "H:%s", p[hn] ? p + hn : "/");
            dospath = onh;
        }
        free(home_real);
        free(path_real);
    }
    uint8_t dos[256]; /* the scratch area holds 256 bytes */
    int failed = utf8_to_dos(dospath, dos, sizeof dos) != 0;
    if (!failed) {
        for (size_t i = 0; i < sizeof dos; i++) {
            wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), dos[i]);
            if (!dos[i]) break;
        }
        Cpu save = cpu;
        cpu.a.x = 0x713B;
        cpu.ds = DOS_SEG;
        cpu.d.x = SCRATCH_OFF;
        dos_fs_int21();
        failed = cpu.cf;
        if (!failed && dos[1] == ':') { /* like a Linux cd: go there, drive and all */
            cpu.a.h = 0x0E;
            cpu.d.l = (uint8_t)(cp866_upper(dos[0]) - 'A');
            dos_fs_int21();
        }
        cpu = save;
    }
    if (failed) {
        static const char msg[] = "Invalid directory\r\n";
        con_write((const uint8_t *)msg, sizeof msg - 1);
        if (door_mode) term_render();
    }
    rt_log("cd %s: %s", path, failed ? "failed" : "ok");
    return failed;
}

/* Host resolution always goes through the DOS drive and alias rules. The
 * kernel scratch area is only 256 bytes; never let a long PATH corrupt its
 * neighbouring MCB. */
static int dos_path_host(const char *path, char *host, size_t cap) {
    size_t n = strlen(path);
    if (n >= 256) { errno = ENAMETOOLONG; return -1; }
    for (size_t i = 0; i <= n; i++) wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), (uint8_t)path[i]);
    return dos_fs_to_host(DOS_SEG, SCRATCH_OFF, host, cap);
}

/* Classic DOS true-name gives the short spelling that old programs parse.
 * AH=60 has a 128-byte result buffer. Use disjoint scratch halves and leave
 * every caller register intact, including an EXEC's pending return frame. */
static int short_dos_path(const char *path, char out[128]) {
    size_t n = strlen(path);
    if (n >= 128) return 3;
    for (size_t i = 0; i <= n; ++i)
        wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), (uint8_t)path[i]);
    Cpu saved = cpu;
    cpu.a.x = 0x6000;
    cpu.ds = cpu.es = DOS_SEG;
    cpu.si = SCRATCH_OFF;
    cpu.di = SCRATCH_OFF + 128;
    dos_fs_int21();
    int err = cpu.cf ? cpu.a.x : 0;
    if (!err) get_str(DOS_SEG, SCRATCH_OFF + 128, out, 128);
    cpu = saved;
    return err;
}

static const Image *builtin_image(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == ':' || *p == '\\' || *p == '/') base = p + 1;
    int which = !strcasecmp(base, "VC.COM") ? 0 : !strcasecmp(base, "VC.OVL") ? 1 : -1;
    if (which < 0) return NULL;
    char normalized[260];
    size_t length = strlen(path);
    if (length >= sizeof normalized) return NULL;
    for (size_t i = 0; i <= length; ++i) normalized[i] = path[i] == '/' ? '\\' : path[i];
    int match = (builtin_dos[which][0] && !strcasecmp(normalized, builtin_dos[which])) ||
                (builtin_short[which][0] && !strcasecmp(normalized, builtin_short[which]));
    if (!match && builtin_host[which][0]) {
        char host[4096];
        match = !dos_path_host(path, host, sizeof host) && !strcmp(host, builtin_host[which]);
    }
    return match ? (which ? &image_vc_ovl : &image_vc_com) : NULL;
}

static void environment_value(const char *name, char *out, size_t cap) {
    out[0] = 0;
    uint16_t env = rd16(cur_psp, 0x2C);
    const uint8_t *block = guest_span((uint32_t)env << 4, 32768);
    size_t key = strlen(name);
    if (!env || !block) return;
    for (size_t at = 0; at < 32768 && block[at];) {
        const uint8_t *s = block + at;
        const uint8_t *end = memchr(s, 0, 32768 - at);
        if (!end) return;
        size_t n = (size_t)(end - s);
        if (n > key && s[key] == '=' && !strncasecmp((const char *)s, name, key)) {
            n -= key + 1;
            if (n >= cap) n = cap - 1;
            memcpy(out, s + key + 1, n);
            out[n] = 0;
            return;
        }
        at += n + 1;
    }
}

/* Returns 1 for a regular DOS file, 0 when absent, or a negative DOS error.
 * DOS programs do not need the host executable permission bit. */
static int program_candidate(const char *path, char *host, size_t cap) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == ':' || *p == '\\' || *p == '/') base = p + 1;
    /* Native command lookup has always left these basenames to the host,
     * even when cwd contains another installation's identical VC.COM.
     * Browser/door still allow other copies through byte-matched lookup. */
#ifndef __EMSCRIPTEN__
    if (!door_mode && (!strcasecmp(base, "VC.COM") || !strcasecmp(base, "VC.OVL"))) return 0;
#endif
    if (builtin_image(path)) return 0;
    if (dos_path_host(path, host, cap)) {
        int err = file_error();
        return err == 2 || err == 3 ? 0 : -err;
    }
    struct stat st;
    if (stat(host, &st)) {
        int err = file_error();
        return err == 2 || err == 3 ? 0 : -err;
    }
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* COMMAND.COM searches the current directory first, then each DOS PATH
 * entry, choosing .COM before .EXE within each directory. An explicit path
 * or drive never causes a search in an unrelated PATH directory. */
static int find_program_path(const char *word, const char *path,
                             char *dos, size_t dcap, char *host, size_t hcap) {
    const char *base = word;
    int explicit_path = 0;
    for (const char *p = word; *p; p++)
        if (*p == ':' || *p == '\\' || *p == '/') { base = p + 1; explicit_path = 1; }
    const char *dot = strrchr(base, '.');
    if (dot && strcasecmp(dot, ".COM") && strcasecmp(dot, ".EXE")) return 0;
    const char *suffix[] = {dot ? "" : ".COM", dot ? NULL : ".EXE", NULL};
    const char *entry = path;
    size_t dlen = 0;
    for (;;) {
        for (int i = 0; suffix[i]; i++) {
            int sep = dlen && entry[dlen - 1] != '\\' && entry[dlen - 1] != '/' && entry[dlen - 1] != ':';
            int n = snprintf(dos, dcap, "%.*s%s%s%s", (int)dlen, entry, sep ? "\\" : "", word, suffix[i]);
            if (n < 0 || (size_t)n >= dcap) continue;
            int found = program_candidate(dos, host, hcap);
            if (found) return found;
        }
        if (explicit_path || !*entry) return 0;
        /* The initial iteration is the current directory; later iterations
         * use a semicolon-delimited PATH component. Empty entries mean cwd. */
        if (dlen) entry += dlen;
        if (*entry == ';') entry++;
        dlen = strcspn(entry, ";");
        if (!dlen && !*entry) return 0;
    }
}

static int find_program(const char *word, char *dos, size_t dcap, char *host, size_t hcap) {
    char path[2048];
    environment_value("PATH", path, sizeof path);
    return find_program_path(word, path, dos, dcap, host, hcap);
}

/* Shipped associations of the form "ext: program [literal args] !.!" are DOS-only. This
 * is data-driven, not a special interpreter command: removing its EXE must
 * never send a file name containing shell syntax to /bin/sh. Custom user
 * association templates retain VC's existing command semantics. */
static int dos_file_association(const char *word) {
    size_t wlen = strlen(word);
    for (int i = 0; i < embedded_file_count; i++) {
        const EmbeddedFile *f = &embedded_files[i];
        if (strcmp(f->name, "VC.EXT")) continue;
        const uint8_t *p = f->data, *end = p + f->size;
        while (p < end) {
            const uint8_t *line = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            const uint8_t *eol = p;
            while (p < end && (*p == '\r' || *p == '\n')) p++;
            const uint8_t *colon = memchr(line, ':', (size_t)(eol - line));
            if (!colon) continue;
            const uint8_t *start = colon + 1;
            while (start < eol && (*start == ' ' || *start == '\t')) start++;
            const uint8_t *last = start;
            while (last < eol && *last != ' ' && *last != '\t') last++;
            if ((size_t)(last - start) != wlen || strncasecmp((const char *)start, word, wlen)) continue;
            while (last < eol && (*last == ' ' || *last == '\t')) last++;
            while (eol > last && (eol[-1] == ' ' || eol[-1] == '\t')) eol--;
            /* Permit fixed words such as Kermit's TAKE, but never turn a
             * template containing shell operators into a trusted command. */
            while (eol - last > 3 &&
                   ((*last >= 'A' && *last <= 'Z') || (*last >= 'a' && *last <= 'z') ||
                    (*last >= '0' && *last <= '9') || *last == '-' || *last == ',' ||
                    *last == ' ' || *last == '\t')) last++;
            if (eol - last == 3 && !memcmp(last, "!.!", 3)) return 1;
        }
    }
    return 0;
}

/* Private launch errors select one useful DOS diagnostic. They become error 3
 * at the EXEC boundary, not an extra misleading "Program not found" line. */
enum { VZ_LONG_DIRECTORY = 0x100, VZ_LONG_FILE, VZ_LONG_TAIL };

static int vz_path_error(int err) {
    return err >= VZ_LONG_DIRECTORY && err <= VZ_LONG_TAIL;
}

static int command_error(int err) {
    rt_log("DOS command error %d", err);
    const char *msg = err == VZ_LONG_DIRECTORY ? "Current directory too long for VZ\r\n" :
                      err == VZ_LONG_FILE ? "File path too long for VZ\r\n" :
                      err == VZ_LONG_TAIL ? "Command line too long for VZ\r\n" :
                      err == 2 || err == 3 ? "Program not found\r\n" :
                      err == 8 ? "Not enough memory\r\n" :
                      err == 11 ? "Invalid program format\r\n" : "Access denied\r\n";
    con_write((const uint8_t *)msg, strlen(msg));
    if (vz_path_error(err)) term_render(); /* VC immediately restores its panels. */
    return vz_path_error(err) ? 3 : err;
}

static int vz_short_directory(char out[128]) {
    int err = short_dos_path(".", out);
    /* An overlong AH=60 result is also an unsafe VZ current directory. */
    if (err == 3 || (!err && strlen(out) + 13 >= 64)) return VZ_LONG_DIRECTORY;
    return err;
}

/* Kermit's DOS command line is itself a command language: unbraced commas
 * start commands, and STAY is needed to suppress its automatic EXIT. For
 * the shipped association, everything after this fixed prefix is ONE file.
 * Hand Kermit a pinned absolute 8.3 spelling, just as we do for VZ. Other
 * typed Kermit commands retain Kermit's own grammar. */
static int kermit_take_tail(const Image *img, uint8_t tail[128], DosPathLease **lease) {
    static const char prefix[] = " stay, take ";
    const size_t plen = sizeof prefix - 1;
    if (strcmp(img->name, "KERMIT.EXE") || tail[0] <= plen ||
        strncasecmp((const char *)tail + 1, prefix, plen)) return 0;
    char file[128], original_host[4096];
    size_t n = tail[0] - plen;
    memcpy(file, tail + 1 + plen, n);
    file[n] = 0;
    if (dos_path_host(file, original_host, sizeof original_host)) return file_error();
    int err = short_dos_path(file, file);
    if (err) return err;
    /* DOS permits braces and other punctuation in 8.3 names. Kermit may
     * expand those even inside a filename; refuse them rather than guess
     * at quoting, including punctuation retained in ancestor aliases. */
    for (const uint8_t *p = (const uint8_t *)file; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p >= 128 || strchr(":\\._~-", *p))) return 11;
    n = strlen(file);
    if (plen + n > 126) return 11;
    err = dos_fs_pin_path(file, original_host, lease);
    if (err) return err;
    tail[0] = (uint8_t)(plen + n);
    memcpy(tail + 1 + plen, file, n);
    tail[plen + n + 1] = '\r';
    return 0;
}

static int dos_program_command(const uint8_t *cmd, size_t len) {
    size_t pos = 0, begin = 0;
    if (len && cmd[0] == '"') {
        begin = ++pos;
        while (pos < len && cmd[pos] != '"') pos++;
        if (pos == len) return -1;
    } else {
        while (pos < len && cmd[pos] != ' ' && cmd[pos] != '\t') pos++;
    }
    size_t wlen = pos - begin;
    if (!wlen || wlen >= 256) return -1;
    char word[256];
    memcpy(word, cmd + begin, wlen);
    word[wlen] = 0;
    if (begin) pos++;
    if (pos < len && cmd[pos] != ' ' && cmd[pos] != '\t') return -1;
    int association = dos_file_association(word);
#ifdef __EMSCRIPTEN__
    if (!association) {
        int quoted = 0;
        for (size_t i = 0; i < len; ++i) {
            if (cmd[i] == '"') quoted = !quoted;
            else if (!quoted && (cmd[i] == '<' || cmd[i] == '>' || cmd[i] == '|'))
                return -1; /* COMMAND.COM owns redirection and pipe parsing. */
        }
    }
#endif
    char dos[256], host[4096];
    int found = find_program(word, dos, sizeof dos, host, sizeof host);
    /* Typed commands only belong to DOS after a complete byte match.
     * Shipped association words remain DOS-only even when lookup fails:
     * their tails can contain file names with shell metacharacters. */
    if (found < 0) return association ? command_error(-found) : -1;
    if (!found) return association ? command_error(2) : -1;
    const Image *img;
    int err = known_image(host, &img);
#ifdef __EMSCRIPTEN__
    /* A recognized DOS file whose download failed must not fall through to
     * the no-shell message. EXEC reports its ordinary DOS access error. */
    if (err && err != 11) return command_error(err);
#endif
    if (err) return association ? command_error(err) : -1;
#ifndef __EMSCRIPTEN__
    /* VC 4.05 adds exactly one native command. Newly recognized old/setup
     * bytes under any other typed name must retain their host meaning.
     * Direct DOS EXEC (including COMMAND's loader) still matches by bytes. */
    if (!door_mode && (img == &image_vc405 || img == &image_vcsetup405) &&
        strcasecmp(word, "vc405"))
        return association ? command_error(11) : -1;
#endif
    if (association) {
        /* A shipped association names its interpreter, not any translation.
         * A renamed VC image would parse the file tail as commands itself. */
        size_t stem = strcspn(img->name, ".");
        if ((wlen != stem && wlen != strlen(img->name)) || strncasecmp(word, img->name, wlen)) {
            rt_log("unsupported executable %s: wrong translation for %s", host, word);
            return command_error(11);
        }
    }
    uint8_t tail[128] = {0};
    size_t n = len - pos;
    if (n > 126) return command_error(11);
    tail[0] = (uint8_t)n;
    memcpy(tail + 1, cmd + pos, n);
    tail[n + 1] = '\r';
    DosPathLease *lease = NULL;
    err = kermit_take_tail(img, tail, &lease);
    if (err) return command_error(err);
    /* Programs otherwise own their argument syntax; GW-BASIC itself
     * inserts quotes. No shell sees a shipped association's DOS tail. */
    err = start_child(img, dos, tail, 0);
    if (err) dos_fs_release_path(lease);
    else if (lease) dos_fs_bind_path(lease, cur_psp);
    return err ? command_error(err) : 0;
}

/* data/VCEDIT.EXT maps every file to `vc-edit !.!`. VC puts the file name in
 * place of !.!, and everything after the word is taken as that one name, so
 * no shell ever parses it: `$(id).txt` stays a file name. */
static const char EDIT_WORD[] = "vc-edit ";

/* SCRATCH_OFF holds the file name VC selected. VZ's DOS parser stops at
 * spaces, commas and '+', so hand it the file layer's absolute 8.3 spelling.
 * The two 128-byte scratch halves keep AH=60's input and output disjoint.
 * A renamed/changed/missing editor is an error, never a host-shell command. */
static int edit_in_vz(const char *original_host) {
    char file[128], directory[128];
    get_str(DOS_SEG, SCRATCH_OFF, file, sizeof file);
    int err = vz_short_directory(directory);
    if (!err) err = short_dos_path(file, file);
    if (err) return command_error(err);
    size_t n = strlen(file);
    /* VZ.INC's PATHSZ is 64, tighter than the DOS command-tail limit.
     * Its makefulpath copies the canonical name into that fixed buffer. */
    if (n >= 64) {
        rt_log("VZ path exceeds its 63-byte limit: %zu bytes", n);
        return command_error(VZ_LONG_FILE);
    }
    char dos[256], host[4096];
    int found = find_program("VZ.COM", dos, sizeof dos, host, sizeof host);
    if (found <= 0) return command_error(found < 0 ? -found : 2);
    const Image *img;
    err = known_image(host, &img);
    if (err || !is_vz_image(img)) return command_error(err ? err : 11);
    /* Generated 8.3 names normally follow the current directory contents.
     * Hold this spelling to the selected file until the editor exits, so a
     * neighbor's creation/deletion cannot redirect a later save. */
    DosPathLease *lease;
    err = dos_fs_pin_path(file, original_host, &lease);
    if (err) return command_error(err);
    uint8_t tail[128] = {0};
    tail[0] = (uint8_t)(n + 1);
    tail[1] = ' ';
    memcpy(tail + 2, file, n);
    tail[n + 2] = '\r';
    err = start_child(img, dos, tail, 0);
    if (err) dos_fs_release_path(lease);
    else dos_fs_bind_path(lease, cur_psp);
    return err ? command_error(err) : 0;
}

/* Run one COMMAND.COM-style command line, given in code page 866 without its
 * CR. Both ways VC runs commands come here: EXEC of COMSPEC with "/C cmd",
 * and INT 2Eh when "Quick execute commands" is on. */
static int run_dos_command(const uint8_t *cmd, size_t len) {
    while (len && (cmd[0] == ' ' || cmd[0] == '\t')) { cmd++; len--; }
    char utf8[1024];
    cp866_to_utf8(cmd, len, utf8, sizeof utf8);
    int cd = internal_cd(utf8);
    if (cd >= 0) return cd;
    const size_t w = sizeof EDIT_WORD - 1;
    if (len > w && !memcmp(cmd, EDIT_WORD, w)) {
        /* Everything after the word is one DOS file name, exactly as VC wrote
         * it, trailing spaces included. The DOS layer maps it to the real host
         * file, so a displayed name with a generated suffix finds its file. */
        size_t n = len - w;
        if (n >= 128) return command_error(3);
        for (size_t i = 0; i < n; i++) wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), cmd[w + i]);
        wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + n), 0);
        char host[4096];
        if (dos_fs_to_host(DOS_SEG, SCRATCH_OFF, host, sizeof host)) {
            static const char msg[] = "File not found\r\n";
            con_write((const uint8_t *)msg, sizeof msg - 1);
            return 2;
        }
#ifndef __EMSCRIPTEN__
        const char *editor = getenv("EDITOR");
        if (!door_mode && editor && *editor) return host_run("exec $EDITOR \"$1\"", "vc-edit", host);
#endif
        return edit_in_vz(host);
    }
    int program = dos_program_command(cmd, len);
    if (program >= 0) return program;
#ifdef __EMSCRIPTEN__
    /* VC's private editor/association bridge above remains safe; actual
     * DOS commands and batch syntax belong to Microsoft's shell. */
    if (len > 122) return command_error(11);
    char shell[260], host[4096];
    environment_value("COMSPEC", shell, sizeof shell);
    if (!*shell || dos_path_host(shell, host, sizeof host)) return command_error(2);
    const Image *img;
    int err = known_image(host, &img);
    if (err || strcmp(img->name, "COMMAND.COM")) return command_error(err ? err : 11);
    uint8_t tail[128] = {0};
    tail[0] = (uint8_t)(len + 4);
    memcpy(tail + 1, " /C ", 4);
    memcpy(tail + 5, cmd, len);
    tail[len + 5] = '\r';
    err = start_child(img, shell, tail, 0);
    return err ? command_error(err) : 0;
#else
    return host_run(utf8, NULL, NULL);
#endif
}

/* A DOS command line holds 126 bytes. VC cuts a longer one silently, so a
 * line that fills it may have lost its end: F4 on a long name could open a
 * shorter name, a typed rm could lose part of its path. Run none of them. */
static int refuse_long_command(void) {
    static const char msg[] = "Command line too long\r\n";
    con_write((const uint8_t *)msg, sizeof msg - 1);
    rt_log("refused a command that fills the 126-byte DOS limit");
    return 1;
}

/* A DOS command tail (count byte, text, CR) to a host command. "/C cmd" means
 * run cmd like COMMAND.COM. Anything else runs the program itself. */
static int exec_host(const char *host_prog, const uint8_t *tail) {
    size_t n = tail[0] > 126 ? 126 : tail[0];
    const uint8_t *t = tail + 1;
    const uint8_t *cr = memchr(t, '\r', n);
    if (cr) n = (size_t)(cr - t);
    while (n && (*t == ' ' || *t == '\t')) { t++; n--; }
    if (n >= 2 && (t[0] == '/' || t[0] == '-') && (t[1] == 'c' || t[1] == 'C') && (n == 2 || t[2] == ' ')) {
        if (tail[0] >= 126) return refuse_long_command();
        return run_dos_command(t + 2, n - 2);
    }
    /* the program's own path comes in as $0, the DOS command tail follows */
    char args[1024], script[1100];
    cp866_to_utf8(t, n, args, sizeof args);
    snprintf(script, sizeof script, "exec \"$0\" %s", args);
    return host_run(script, host_prog, NULL);
}

/* ---- EXEC and terminate --------------------------------------------------- */

/* MAIN.ASM appends "\\VZTEMP.$$$" into TMPPATHSZ=32 with an unbounded
 * strcpy. Give VZ a short, private swap directory: a host TMPDIR may exceed
 * that limit, and sharing /tmp would collide with another VZ's fixed name.
 * Retain all other variables and other children's TMP/TEMP unchanged. */
static int vz_environment(char *env, size_t *length, size_t cap, Proc *p) {
    size_t used = 0;
    for (size_t at = 0; at < *length;) {
        const char *end = memchr(env + at, 0, *length - at);
        if (!end) return 10;
        size_t n = (size_t)(end - (env + at)) + 1;
        if (strncasecmp(env + at, "TMP=", 4) && strncasecmp(env + at, "TEMP=", 5)) {
            memmove(env + used, env + at, n);
            used += n;
        }
        at += n;
    }
    if (door_mode) {
        /* Create it through H:'s accounting: it is one of the session's 4096
         * entries, and a full session refuses the EXEC with disk full. */
        static unsigned sequence;
        int err = 80;
        for (unsigned attempt = 0; attempt < 64 && err == 80; ++attempt) {
            unsigned salt = (unsigned)time(NULL) ^ ((unsigned)getpid() << 8) ^ (++sequence * 2654435761u);
            snprintf(p->temp_dir, sizeof p->temp_dir, "v%06x", salt & 0xffffffu);
            char dos[32];
            snprintf(dos, sizeof dos, "H:\\%s", p->temp_dir);
            err = dos_fs_make_temporary(dos);
        }
        if (err) { p->temp_dir[0] = 0; return err == 39 ? 39 : 5; }
    } else {
        strcpy(p->temp_dir, "/tmp/vXXXXXX");
        if (!mkdtemp(p->temp_dir)) { p->temp_dir[0] = 0; return 5; }
    }
    const char *names[] = {"TMP", "TEMP"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        int n = snprintf(env + used, cap - used, "%s=%s%s", names[i],
                         door_mode ? "H:\\" : "C:\\tmp\\",
                         p->temp_dir + (door_mode ? 0 : 5));
        if (n < 0 || (size_t)n >= cap - used) return 8;
        used += (size_t)n + 1;
    }
    *length = used;
    return 0;
}

static void remove_vz_temp(Proc *p) {
    if (!p->temp_dir[0]) return;
    /* Only our own directory and VZ's known swap names, never recursive
     * removal. Dp+ can lowercase DOS-created names on the host. */
    const char *names[] = {"VZTEMP.$$$", "vztemp.$$$", "FILES.$$$", "files.$$$"};
    char path[64];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (door_mode) {
            snprintf(path, sizeof path, "H:\\%s\\%s", p->temp_dir, names[i]);
            int err = dos_fs_remove_temporary(path);
            if (err && err != 2) rt_log("cannot remove VZ swap %s: DOS error %d", path, err);
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", p->temp_dir, names[i]);
        if (unlink(path) && errno != ENOENT) rt_log("cannot remove VZ swap %s: %s", path, strerror(errno));
    }
    if (door_mode) {
        snprintf(path, sizeof path, "H:\\%s", p->temp_dir);
        int err = dos_fs_remove_temporary(path);
        if (err) rt_log("cannot remove VZ temp %s: DOS error %d", path, err);
    } else if (rmdir(p->temp_dir)) rt_log("cannot remove VZ temp %s: %s", p->temp_dir, strerror(errno));
    p->temp_dir[0] = 0;
}

static int vz_document_path(char path[128]) {
    const char *wildcard = strpbrk(path, "*?");
    if (!wildcard) {
        int err = short_dos_path(path, path);
        return err == 3 ? VZ_LONG_FILE : err;
    }
    /* A mask is not a missing literal file: AH=60 would invent an alias for
     * it. Canonicalize only the parent and preserve VZ's final-name pattern.
     * DOS/VZ do not expand wildcard directory components here. */
    char *leaf = path + (path[1] == ':' ? 2 : 0);
    for (char *p = path; *p; ++p) if (*p == '\\' || *p == '/') leaf = p + 1;
    if (wildcard < leaf) return 3;
    char pattern[128];
    strcpy(pattern, leaf);
    if (leaf == path) strcpy(path, ".");
    else if (leaf == path + 2 && path[1] == ':') strcpy(path + 2, ".");
    else *leaf = 0;
    int err = short_dos_path(path, path);
    if (err) return err == 3 ? VZ_LONG_FILE : err;
    size_t n = strlen(path), len = strlen(pattern);
    int separator = n && path[n - 1] != '\\';
    if (n + separator + len >= 64) return VZ_LONG_FILE;
    if (separator) path[n++] = '\\';
    memcpy(path + n, pattern, len + 1);
    return 0;
}

/* Expand each document operand, not only F4's first file: a safe CWD can still
 * be followed by an unsafe explicit path. MAIN.ASM's readopt owns '-' options
 * and DEF selectors; reference selectors also have VZ-specific search rules.
 * Keep that auxiliary syntax, while quoted documents become 8.3 names. */
static int vz_command_tail(const uint8_t *tail, uint8_t out[128]) {
    size_t pos = 1, end = (size_t)tail[0] + 1, used = 0;
    int options = 1;
    if (tail[0] > 126) return VZ_LONG_TAIL;
    while (pos < end && tail[pos] != '\r') {
        if (tail[pos] == ' ' || tail[pos] == '\t') {
            if (used == 126) return VZ_LONG_TAIL;
            out[++used] = tail[pos++];
            continue;
        }
        if (options && tail[pos] == '-') {
            do {
                if (used == 126) return VZ_LONG_TAIL;
                out[++used] = tail[pos++];
            } while (pos < end && tail[pos] > ' ');
            continue;
        }
        /* DEF selectors and reference files have a leading VZ marker, not a
         * DOS path component. Keep a standalone marker and its spacing too. */
        int definition = options && (tail[pos] == '/' || tail[pos] == '+');
        int reference = tail[pos] == '@';
        if (definition || reference) {
            if (used == 126) return VZ_LONG_TAIL;
            out[++used] = tail[pos++];
            if (reference && pos < end && tail[pos] == '@') {
                if (used == 126) return VZ_LONG_TAIL;
                out[++used] = tail[pos++];
            }
            while (pos < end && (tail[pos] == ' ' || tail[pos] == '\t')) {
                if (used == 126) return VZ_LONG_TAIL;
                out[++used] = tail[pos++];
            }
            if (pos == end || tail[pos] == '\r') break;
            size_t start = pos;
            while (pos < end && tail[pos] > ' ') ++pos;
            size_t n = pos - start;
            if (n >= 64) return VZ_LONG_FILE;
            if (used + n > 126) return VZ_LONG_TAIL;
            /* These names may be relative to VZDEF or the executable, not
             * the CWD. Rewriting them here would silently change the file. */
            memcpy(out + used + 1, tail + start, n);
            used += n;
            if (reference) options = 0;
            continue;
        }
        /* readopt stops at the first document. Later '-' and '/' prefixes
         * are filenames, so they must not bypass the length check. */
        options = 0;
        int quoted = tail[pos] == '"';
        if (quoted) ++pos;
        size_t start = pos;
        while (pos < end && (quoted ? tail[pos] != '"' : tail[pos] > ' ')) ++pos;
        size_t n = pos - start;
        if (!n) return 11;
        if (quoted) {
            if (pos == end) return 11;
            if (++pos < end && tail[pos] > ' ') return 11;
        }
        char path[128];
        memcpy(path, tail + start, n); path[n] = 0;
        int err = vz_document_path(path);
        if (err) return err;
        n = strlen(path);
        if (n >= 64) return VZ_LONG_FILE;
        if (used + n > 126) return VZ_LONG_TAIL;
        memcpy(out + used + 1, path, n);
        used += n;
    }
    out[0] = (uint8_t)used;
    out[used + 1] = '\r';
    return 0;
}

static int prepare_vz_paths(const char *program, char short_program[128],
                            DosPathLease **executable, DosPathLease **directory) {
    char host[4096], short_directory[128];
    *executable = *directory = NULL;
    /* makefulpath appends a separator and up to twelve 8.3 name bytes, then
     * its NUL, into PATHSZ=64. Reserve that headroom even for an empty tail:
     * VZ's own Open dialog can later supply a relative filename. */
    int err = vz_short_directory(short_directory);
    if (err) return err;
    if (dos_path_host(program, host, sizeof host)) return 3;
    err = short_dos_path(program, short_program);
    if (!err && strlen(short_program) >= 64) err = VZ_LONG_FILE;
    if (!err) err = dos_fs_pin_path(short_program, host, executable);
    /* Typed new files remember the DOS current directory, even when the
     * editor lives on another drive. Keep those ancestor aliases stable
     * too; otherwise a save could follow a neighboring directory's name. */
    if (!err && dos_path_host(".", host, sizeof host)) err = 3;
    if (!err && strlen(short_directory) > 3) /* A drive root has no aliases. */
        err = dos_fs_pin_path(short_directory, host, directory);
    if (err) {
        dos_fs_release_path(*directory);
        dos_fs_release_path(*executable);
        *executable = *directory = NULL;
    }
    return err;
}

#ifndef __EMSCRIPTEN__
/* Captured from the first native VC, not from a renamed COMMAND or a guest
 * environment. Only secondary DOS shells get this directory on PATH. */
static char native_dos2_directory[128];
#endif

static int command_environment(char *env, size_t *length, size_t cap, const char *program) {
    /* Microsoft's transient reload uses a 40-byte COMSPEC buffer. The
     * executable may be DOS2.COM or any other byte-identical renamed copy. */
    char shell[128];
    int err = short_dos_path(program, shell);
    if (err || strlen(shell) >= 40) return err ? err : 3;
    size_t read = 0, used = 0;
    while (read < *length) {
        const char *end = memchr(env + read, 0, *length - read);
        if (!end) return 8;
        size_t size = (size_t)(end - (env + read)) + 1;
        if (strncasecmp(env + read, "COMSPEC=", 8)) {
            memmove(env + used, env + read, size);
            used += size;
        }
        read += size;
    }
    int size = snprintf(env + used, cap - used, "COMSPEC=%s", shell);
    if (size < 0 || (size_t)size >= cap - used) return 8;
    used += (size_t)size + 1;
#ifndef __EMSCRIPTEN__
    if (!door_mode && native_dos2_directory[0]) {
        char directory[128];
        err = short_dos_path(native_dos2_directory, directory);
        if (err) return err;
        size_t dlen = strlen(directory), at = 0;
        int found = 0;
        while (at < used) {
            size_t size = strlen(env + at) + 1;
            if (!strncasecmp(env + at, "PATH=", 5)) {
                size_t value = at + 5, first = strcspn(env + value, ";");
                found = 1;
                /* A nested shell inherits this prefix already. Keep its
                 * parent's complete PATH without multiplying entries. */
                if (first != dlen || strncasecmp(env + value, directory, dlen)) {
                    int separator = env[value] != 0;
                    size_t extra = dlen + separator;
                    if (extra > cap - used) return 8;
                    memmove(env + value + extra, env + value, used - value);
                    memcpy(env + value, directory, dlen);
                    if (separator) env[value + dlen] = ';';
                    used += extra;
                }
                break;
            }
            at += size;
        }
        if (!found) {
            size = snprintf(env + used, cap - used, "PATH=%s", directory);
            if (size < 0 || (size_t)size >= cap - used) return 8;
            used += (size_t)size + 1;
        }
    }
#endif
    *length = used;
    return 0;
}

static int is_vc405_image(const Image *img) {
    return !strcmp(img->name, "VC405.COM") || !strcmp(img->name, "VCSETUP.COM");
}

/* Both versions interpret VC= as their settings home. Give 4.05 its private
 * directory (VC.ASM:MainPth, VCSETUP.ASM:Init10), but never pass that setting
 * into a 4.99 child: the two versions' VC.INI formats are incompatible.
 * Work only on the child's copied strings, including duplicate/cased keys. */
static int vc_environment(const Image *img, char *env, size_t *length, size_t cap) {
    size_t read = 0, used = 0;
    while (read < *length) {
        const char *end = memchr(env + read, 0, *length - read);
        if (!end) return 8;
        size_t size = (size_t)(end - (env + read)) + 1;
        if (strncasecmp(env + read, "VC=", 3)) {
            memmove(env + used, env + read, size);
            used += size;
        }
        read += size;
    }
    *length = used;
    if (img == &image_vc_com) return 0;
    char directory[128];
    int err = short_dos_path(vc405_directory, directory);
    if (err) return err;
    /* MainPth has LenPath=68 and reserves two bytes for a separator/NUL. */
    if (strlen(directory) > 65) return 3;
    int size = snprintf(env + used, cap - used, "VC=%s", directory);
    if (size < 0 || (size_t)size >= cap - used) return 8;
    *length = used + (size_t)size + 1;
    return 0;
}

static int start_child(const Image *img, const char *dos_prog, const uint8_t *tail, uint16_t envseg) {
    if (nprocs == (int)(sizeof procs / sizeof procs[0])) return 8;
    char short_program[128];
    uint8_t short_tail[128] = {0};
    DosPathLease *executable = NULL, *directory = NULL;
    if (is_vz_image(img)) {
        /* VZ derives its DEF name from this environment path with the same
         * 8.3 parser it uses for documents. Host config directories can
         * contain spaces, commas and '+', even when the COM name is short. */
        int err = prepare_vz_paths(dos_prog, short_program, &executable, &directory);
        if (!err) err = vz_command_tail(tail, short_tail);
        if (err) {
            dos_fs_release_path(directory);
            dos_fs_release_path(executable);
            return err;
        }
        dos_prog = short_program;
        tail = short_tail;
    }
    if (is_vc405_image(img)) {
        int err = short_dos_path(dos_prog, short_program);
        if (err || strlen(short_program) >= 68) return err ? err : 3;
        dos_prog = short_program;
    }
    rt_log("load %s: translation %s", dos_prog, img->name);
    Proc *p = &procs[nprocs];
    p->temp_dir[0] = 0;
    p->vc_startup[0] = 0;
    const Image *parent_image = nprocs ? procs[nprocs - 1].image : &image_vc_com;
    p->machine = rt_save_process_state(parent_image);
    if (!p->machine) {
        dos_fs_release_path(directory);
        dos_fs_release_path(executable);
        return 8;
    }
    p->image = img;
    p->exec_break_state = 0;
    p->break_vector = lin(rd16(0, 0x1b * 4 + 2), rd16(0, 0x1b * 4));
    p->parent = cpu;
    get_dta(&p->dta_seg, &p->dta_off);
    uint16_t ret_ip = rd16(cpu.ss, cpu.sp), ret_cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
    uint16_t parent_psp = cur_psp;
    static char envbuf[32768];
    size_t elen = env_strings(envseg ? envseg : rd16(cur_psp, 0x2C), envbuf, sizeof envbuf);
    int err = !strcmp(img->name, "COMMAND.COM") ?
        command_environment(envbuf, &elen, sizeof envbuf, dos_prog) : 0;
    if (!err && (is_vc405_image(img) || img == &image_vc_com))
        err = vc_environment(img, envbuf, &elen, sizeof envbuf);
    if (!err && is_vz_image(img)) err = vz_environment(envbuf, &elen, sizeof envbuf, p);
    if (!err) err = load_image(img, envbuf, elen, tail, dos_prog, parent_psp, ret_cs, ret_ip);
    if (err) {
        dos_fs_release_path(directory);
        dos_fs_release_path(executable);
        remove_vz_temp(p);
        rt_finish_process_state(p->machine, 0);
        p->machine = NULL;
        cpu = p->parent;
        return err;
    }
    dos_fs_bind_path(executable, cur_psp);
    dos_fs_bind_path(directory, cur_psp);
    p->child = cur_psp;
    nprocs++;
    hle_redirect = 1;
    return 0;
}

/* The supplied COMMAND snapshot contains Microsoft's own EXEC implementation.
 * It reads/relocates the image itself, then calls DOS 2's AH=55h to create its
 * child PSP. Adopt that child only after BOTH the just-closed complete file
 * and the bytes actually loaded in memory match a preserved translation.
 * No program bytes are decoded or executed by the host. */
static int loaded_bytes_match(const Image *img, uint16_t loadseg) {
    uint32_t base = (uint32_t)loadseg << 4;
    const uint8_t *loaded = guest_span(base, img->size);
    if (!loaded) return 0;
    uint8_t *expected = malloc(img->size ? img->size : 1);
    if (!expected) return 0;
    memcpy(expected, img->bytes, img->size);
    for (uint32_t i = 0; i < img->nrelocs; ++i) {
        uint32_t at = img->relocs[i];
        if (at + 1 >= img->size) { free(expected); return 0; }
        uint16_t value = (uint16_t)(expected[at] | expected[at + 1] << 8);
        value = (uint16_t)(value + loadseg);
        expected[at] = (uint8_t)value;
        expected[at + 1] = (uint8_t)(value >> 8);
    }
    int matched = !memcmp(expected, loaded, img->size);
    free(expected);
    return matched;
}

static Cpu dos2_saved_frame(uint16_t psp) {
    Cpu saved = cpu;
    uint16_t seg = rd16(psp, 0x30), off = rd16(psp, 0x2E);
    saved.a.x = rd16(seg, off); saved.b.x = rd16(seg, (uint16_t)(off + 2));
    saved.c.x = rd16(seg, (uint16_t)(off + 4)); saved.d.x = rd16(seg, (uint16_t)(off + 6));
    saved.si = rd16(seg, (uint16_t)(off + 8)); saved.di = rd16(seg, (uint16_t)(off + 10));
    saved.bp = rd16(seg, (uint16_t)(off + 12)); saved.ds = rd16(seg, (uint16_t)(off + 14));
    saved.es = rd16(seg, (uint16_t)(off + 16));
    saved.ss = seg; saved.sp = (uint16_t)(off + 18); /* IP, CS, FLAGS remain. */
    return saved;
}

static int adopt_dos2_child(uint16_t child) {
    uint16_t ip = rd16(cpu.ss, cpu.sp), cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
    /* COMMAND hooks INT 21h for all of its descendants. The process on top
     * may be VC, Kermit or BASIC; the loader is identified by its INT frame,
     * including COMMAND's relocated transient code. */
    const Image *loader = rt_image_return("COMMAND.COM", cs, ip);
    if (!loader) return 0;
    char host[4096];
    const Image *img = NULL;
    int err = !dos_fs_take_closed_file(cur_psp, host, sizeof host) ? 11 : known_image(host, &img);
    uint16_t loadseg = (uint16_t)(child + 0x10);
    if (!err && !loaded_bytes_match(img, loadseg)) {
        /* MZ max-allocation=0 asks DOS to load at the top of its allocation. */
        uint16_t high = (uint16_t)(child + mcb_size((uint16_t)(child - 1)) - (img->size + 15) / 16);
        if (!img->is_exe || img->max_alloc || !loaded_bytes_match(img, high)) err = 11;
        else loadseg = high;
    }
    if (nprocs == (int)(sizeof procs / sizeof procs[0])) err = 8;
    char vc_program[128] = {0};
    if (!err && (is_vc405_image(img) || img == &image_vc_com)) {
        Cpu caller = dos2_saved_frame(cur_psp);
        char program[260];
        get_str(caller.ds, caller.d.x, program, sizeof program);
        err = short_dos_path(program, vc_program);
        if (!err && is_vc405_image(img) && strlen(vc_program) >= 68) err = 3;
    }
    RtProcessState *machine = NULL;
    const Image *parent_image = nprocs ? procs[nprocs - 1].image : loader;
    if (!err && !(machine = rt_save_process_state(parent_image))) err = 8;
    if (err) {
        /* AH=55h has no error convention. Restore the original EXEC caller's
         * saved frame, returning the ordinary DOS error there instead of
         * letting an unapproved loader target run (or killing its shell). */
        Cpu saved = dos2_saved_frame(cur_psp);
        /* This early unwind bypasses EXEC.ASM's restore_ctrlc. Restore only
         * its recorded query -> disable sequence, not arbitrary BREAK edits. */
        Proc *shell = nprocs ? &procs[nprocs - 1] : NULL;
        if (shell && shell->exec_break_state == 2) break_flag = shell->exec_break;
        if (shell) shell->exec_break_state = 0;
        mem_free_owned(child);
        cpu = saved;
        cpu.ip = pop16(); cpu.cs = pop16(); flags_set(pop16());
        cpu.a.x = (uint16_t)err; cpu.cf = 1;
        hle_redirect = 1;
        return -1;
    }
    Proc *p = &procs[nprocs++];
    memset(p, 0, sizeof *p);
    p->child = child; p->image = img; p->machine = machine;
    memcpy(p->vc_startup, vc_program, sizeof vc_program);
    p->parent = dos2_saved_frame(cur_psp);
    p->break_vector = lin(vec_seg(0x1B), vec_off(0x1B));
    get_dta(&p->dta_seg, &p->dta_off);
    rt_register_image(img, loadseg);
    rt_log("load %s: translation %s (DOS-hosted loader)", host, img->name);
    return 1;
}

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

    /* COMSPEC alone is an internal bridge to the command parser, not an
     * unrecognised DOS program handed to a host executable. This is also
     * present in the browser, where /bin/sh itself does not exist. */
    char command_path[260];
    for (size_t i = 0; i <= strlen(dos_prog); i++)
        command_path[i] = dos_prog[i] == '/' ? '\\' : dos_prog[i];
    if (!strcasecmp(command_path, door_mode ? "H:\\COMMAND.COM" : "C:\\bin\\sh")) {
        int status = exec_host("/bin/sh", tail);
        if (hle_redirect) return; /* the command has loaded a DOS child */
        last_retcode = (uint16_t)(status & 0xFF);
        cpu.cf = 0;
        return;
    }
#ifdef __EMSCRIPTEN__
    /* The VC command-line bridge must still recognize vc-edit and the
     * injection-safe shipped associations before passing normal /C text
     * to COMMAND.COM. A shell's own EXEC is not intercepted. */
    if (!strcasecmp(command_path, "H:\\COMMAND.COM") &&
        (!nprocs || procs[nprocs - 1].image == &image_vc_com ||
         procs[nprocs - 1].image == &image_vc_ovl)) {
        size_t pos = 1;
        while (pos <= tail[0] && (tail[pos] == ' ' || tail[pos] == '\t')) ++pos;
        if (pos + 1 <= tail[0] && tail[pos] == '/' &&
            (tail[pos + 1] == 'C' || tail[pos + 1] == 'c')) {
            int status = exec_host(NULL, tail);
            if (hle_redirect) return;
            last_retcode = (uint16_t)(status & 0xFF);
            cpu.cf = 0;
            return;
        }
    }
#endif
    /* These two images are vc itself. In particular, VC.COM reloads VC.OVL
     * after every command, even if another vc replaced its installation or
     * the config directory no longer exists. Do not consult the disk. */
    const Image *img = builtin_image(command_path);
    int err = 0;
    if (!img) {
        char host[4096];
        if (dos_fs_to_host(cpu.ds, cpu.d.x, host, sizeof host)) { fail((uint16_t)file_error()); return; }
        err = known_image(host, &img);
    }
    if (!err) err = start_child(img, dos_prog, tail, envseg);
    if (vz_path_error(err)) err = command_error(err);
    if (err) fail((uint16_t)err);
}

static void forget_psp_copy(uint16_t psp) {
    for (PspCopy **link = &psp_copies; *link;) {
        PspCopy *copy = *link;
        if (copy->psp != psp) { link = &copy->next; continue; }
        *link = copy->next;
        free(copy);
    }
}

static void close_psp_copies(uint16_t owner) {
    for (PspCopy **link = &psp_copies; *link;) {
        PspCopy *copy = *link;
        if (copy->owner != owner) { link = &copy->next; continue; }
        *link = copy->next;
        dos_fs_close_process(copy->psp);
        mem_free_owned(copy->psp);
        free(copy);
    }
}

static void terminate(uint8_t code, int tsr, uint16_t keep) {
    uint16_t psp = cur_psp;
    last_retcode = (uint16_t)(code | (tsr ? 0x300 : 0));
    rt_log("terminate psp %04X code %u%s", psp, code, tsr ? " (resident)" : "");
    if (tsr) {
        /* These host launch leases are not DOS handles belonging to resident
         * code. Drop them without closing a generic TSR's live FCBs. */
        dos_fs_release_process_paths(psp);
        uint16_t maxp;
        mem_resize(psp, keep < 6 ? 6 : keep, &maxp);
    } else {
        close_psp_copies(psp);
        dos_fs_close_process(psp);
        forget_psp_copy(psp);
        mem_free_owned(psp);
    }
    set_vec(0x22, rd16(psp, 0x0C), rd16(psp, 0x0A));
    set_vec(0x23, rd16(psp, 0x10), rd16(psp, 0x0E));
    set_vec(0x24, rd16(psp, 0x14), rd16(psp, 0x12));
    uint16_t term_ip = rd16(psp, 0x0A), term_cs = rd16(psp, 0x0C);
    if (nprocs > 0 && procs[nprocs - 1].child == psp) {
        Proc *p = &procs[--nprocs];
        remove_vz_temp(p);
        rt_finish_process_state(p->machine, 0);
        p->machine = NULL;
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
    dos_fs_set_process(cur_psp);
    cpu.cs = term_cs;
    cpu.ip = term_ip;
#ifdef __EMSCRIPTEN__
    if (term_cs == STUB_SEG && term_ip == STUB_EXIT) rt_exit_code = code;
#else
    if (door_mode && term_cs == STUB_SEG && term_ip == STUB_EXIT) rt_exit_code = code;
#endif
    hle_redirect = 1;
}

static Proc *abortable_child(void) {
    if (!nprocs) return NULL;
    Proc *p = &procs[nprocs - 1];
    if (p->image == &image_vc_com || p->image == &image_vc_ovl) {
        /* Keep fatal diagnostics for the outer VC and its overlay, but a
         * nested VC belongs to its DOS caller too. A fault there must not
         * take down the COMMAND/Kermit/BASIC session that launched it. */
        int nested = 0;
        for (int i = 0; i + 1 < nprocs; ++i)
            if (procs[i].image != &image_vc_com && procs[i].image != &image_vc_ovl) {
                nested = 1;
                break;
            }
        if (!nested) return NULL;
    }
    /* DEBUG and similar programs run a PSP they made with AH55/AH26. Walk
     * its DOS ancestry back to the registered program. A forged cycle is
     * bounded by the number of possible 16-bit PSP segments. */
    uint16_t psp = cur_psp;
    for (unsigned depth = 0; depth < 0x10000u; ++depth) {
        if (psp == p->child) return p;
        uint16_t parent = rd16(psp, 0x16);
        if (!parent || parent == psp) break;
        psp = parent;
    }
    return NULL;
}

static void abort_child(Proc *p, uint8_t code, uint8_t how) {
    RtProcessState *machine = p->machine;
    p->machine = NULL;
    cur_psp = p->child;
    dos_fs_set_process(cur_psp);
    terminate(code, 0, 0);
    last_retcode |= (uint16_t)how << 8;
    /* terminate also writes DOS exit vectors; restore the full parent IVT
     * after that, including GW-BASIC's otherwise abandoned timer hooks. */
    rt_finish_process_state(machine, 1);
}

/* MS-DOS 2's own EXEC copies only environment strings, without the DOS 3+
 * executable trailer VC needs, and writes PSP:2Ch after AH55. Wait until
 * the first DOS call from the actual child, then replace only its copy.
 * Normal native EXEC already prepared this environment in start_child. */
static int finish_vc_environment(void) {
    Proc *p = nprocs ? &procs[nprocs - 1] : NULL;
    if (!p || !p->vc_startup[0] || cur_psp != p->child) return 0;
    uint16_t ip = rd16(cpu.ss, cpu.sp), cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
    if (rt_image_return("COMMAND.COM", cs, ip)) return 0;
    static char envbuf[32768];
    uint16_t old_env = rd16(cur_psp, 0x2C);
    size_t length = env_strings(old_env, envbuf, sizeof envbuf);
    int err = vc_environment(p->image, envbuf, &length, sizeof envbuf);
    uint16_t env = 0;
    if (!err) env = make_env(envbuf, length, p->vc_startup, cur_psp);
    if (!err && !env) {
        /* A COM owns all remaining DOS memory. Return enough of its high
         * allocation for the new environment while retaining its full
         * 64 KiB segment/stack, and keep the public PSP bound accurate. */
        uint16_t size = mcb_size((uint16_t)(cur_psp - 1)), largest;
        size_t paras = ((length ? length : 1) + 4 + strlen(p->vc_startup) + 15) / 16;
        if (size > 0x1000u + paras + 1 &&
            !mem_resize(cur_psp, (uint16_t)(size - paras - 1), &largest)) {
            wr16(cur_psp, 2, (uint16_t)(cur_psp + mcb_size((uint16_t)(cur_psp - 1))));
            env = make_env(envbuf, length, p->vc_startup, cur_psp);
        }
        if (!env) err = 8;
    }
    p->vc_startup[0] = 0;
    if (err) {
        char message[96];
        int n = snprintf(message, sizeof message,
                         "\r\nCannot prepare VC environment (DOS error %d).\r\n", err);
        con_write((const uint8_t *)message, (size_t)n);
        term_render();
        abort_child(p, (uint8_t)err, 0);
        return 1;
    }
    wr16(cur_psp, 0x2C, env);
    if (old_env && mcb_owner((uint16_t)(old_env - 1)) == cur_psp) mem_free(old_env);
    return 0;
}

int dos_abort_untranslated(void) {
    Proc *p = abortable_child();
    if (!p) return 0;
    char message[160];
    snprintf(message, sizeof message, "\r\nNo translated code at %04X:%04X. %s stopped.\r\n",
             cpu.cs, cpu.ip, p->image->name);
    rt_log("No translated code at %04X:%04X. %s stopped.", cpu.cs, cpu.ip, p->image->name);
    con_write((const uint8_t *)message, strlen(message));
    term_render(); /* Make the diagnostic visible before VC redraws its panels. */
    abort_child(p, 70, 0);
    return 1;
}

int dos_abort_break(void) {
    Proc *p = abortable_child();
    if (!p) return 0;
    uint32_t handler = lin(rd16(0, 0x1b * 4 + 2), rd16(0, 0x1b * 4));
    if (handler != p->break_vector && handler != lin(STUB_SEG, 0x1b))
        return 0;
    char message[96];
    snprintf(message, sizeof message, "\r\nCtrl-Break. %s stopped.\r\n", p->image->name);
    rt_log("Ctrl-Break. %s stopped.", p->image->name);
    con_write((const uint8_t *)message, strlen(message));
    term_render();
    abort_child(p, 0, 1); /* INT 21h/4Dh reports termination by Ctrl-Break. */
    return 1;
}

/* ---- List of Lists and DOS data ------------------------------------------- */

static void init_dos_data(void) {
    uint32_t a = (uint32_t)DOS_SEG << 4;
    (void)guest_fill(a, 0, 0x800); /* constant, always inside guest memory */
    wr16(DOS_SEG, LOL_OFF - 2, FIRST_MCB);
    for (int i = 0; i < 0x16; i += 2) wr16(DOS_SEG, (uint16_t)(LOL_OFF + i), 0xFFFF); /* DPB, SFT, devices, buffers: none */
    wr16(DOS_SEG, LOL_OFF + 0x10, 512);
    wr16(DOS_SEG, LOL_OFF + 0x16, CDS_OFF);
    wr16(DOS_SEG, LOL_OFF + 0x18, DOS_SEG);
    wr16(DOS_SEG, LOL_OFF + 0x1A, 0xFFFF);
    wr16(DOS_SEG, LOL_OFF + 0x1C, 0xFFFF);
    wr8(DOS_SEG, LOL_OFF + 0x20, 8);
    wr8(DOS_SEG, LOL_OFF + 0x21, 8); /* LASTDRIVE=H */
    /* NUL device header, end of the device chain */
    uint16_t nul = LOL_OFF + 0x22;
    wr16(DOS_SEG, nul, 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 2), 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 4), 0x8004);
    (void)guest_write(a + nul + 10, "NUL     ", 8);
    /* CDS: A: and B: invalid, C: a physical drive at C:\ */
    for (int d = 0; d < 8; d++) {
        uint16_t e = (uint16_t)(CDS_OFF + d * 0x58);
        wr8(DOS_SEG, e, (uint8_t)('A' + d));
        wr8(DOS_SEG, (uint16_t)(e + 1), ':');
        wr8(DOS_SEG, (uint16_t)(e + 2), '\\');
        wr16(DOS_SEG, (uint16_t)(e + 0x43), 0); /* valid drives are marked in dos_start */
        wr16(DOS_SEG, (uint16_t)(e + 0x4F), 2);
    }
    mcb_set(FIRST_MCB, 'Z', 0, (uint16_t)(MEM_TOP - FIRST_MCB - 1));
}

void dos_core_init(void) {
    memset(builtin_dos, 0, sizeof builtin_dos);
    memset(builtin_short, 0, sizeof builtin_short);
    memset(builtin_host, 0, sizeof builtin_host);
    vc405_directory[0] = 0;
    while (psp_copies) {
        PspCopy *copy = psp_copies;
        psp_copies = copy->next;
        free(copy);
    }
    (void)guest_fill(0, 0, MEM_SIZE);
    for (int n = 0; n < 256; n++) set_vec((uint8_t)n, STUB_SEG, (uint16_t)n);
    (void)guest_fill((uint32_t)STUB_SEG << 4, 0xCF, STUB_END); /* IRET bytes, for anyone who looks */
    mem[0xFFFFE] = 0xFC;                                  /* machine model: AT */
    init_dos_data();
    wr16(0x40, 0x13, 640);                                /* base memory in KB */
}

/* The PSP entry is a DOS-provided termination thunk, not translated program
 * code. GW-BASIC's SYSTEM returns here with a far RET as DOS 1.x programs
 * did. Recognise only this process's intact kernel-created thunk. */
int dos_run_psp(void) {
    if (cpu.cs != cur_psp || cpu.ip != 0 || rd8(cur_psp, 0) != 0xCD || rd8(cur_psp, 1) != 0x20)
        return 0;
    cpu_int(0x20, 2);
    return 1;
}

/* ---- INT 21h -------------------------------------------------------------- */

int dos_core_int21(void) {
    if (finish_vc_environment()) return 1;
    uint16_t seg, largest, maxp;
    int err;
    switch (cpu.a.h) {
    case 0x00: terminate(0, 0, 0); return 1;
    case 0x25: set_vec(cpu.a.l, cpu.ds, cpu.d.x); return 1;
    case 0x26:
        /* DOS 1.x Create PSP, used when GW-BASIC moves its data segment.
         * Unlike AH=55h this does not change the current process. */
        if (guest_move((uint32_t)cpu.d.x << 4, (uint32_t)cur_psp << 4, 256)) { fail(8); return 1; }
        wr16(cpu.d.x, 0x16, cur_psp);
        return 1;
    case 0x35: cpu.es = vec_seg(cpu.a.l); cpu.b.x = vec_off(cpu.a.l); return 1;
    case 0x30: cpu.a.x = cur_psp ? rd16(cur_psp, 0x40) : 0x0A07; cpu.b.h = 0xFF; cpu.b.l = 0; cpu.c.x = 0; return 1;
    case 0x31: terminate(cpu.a.l, 1, cpu.d.x); return 1;
    case 0x33: {
        Proc *shell = nprocs ? &procs[nprocs - 1] : NULL;
        uint16_t ip = rd16(cpu.ss, cpu.sp), cs = rd16(cpu.ss, (uint16_t)(cpu.sp + 2));
        if (shell && !rt_image_return("COMMAND.COM", cs, ip) &&
            (shell->child != cur_psp || strcmp(shell->image->name, "COMMAND.COM")))
            shell = NULL;
        switch (cpu.a.l) {
        case 0x00:
            if (shell) { shell->exec_break = break_flag; shell->exec_break_state = 1; }
            cpu.d.l = break_flag;
            return 1;
        case 0x01: {
            int exec_disable = shell && shell->exec_break_state == 1 &&
                !(cpu.d.l & 1) && shell->exec_break == break_flag;
            /* BREAK is global. Any later explicit write supersedes old
             * snapshots, including the source's normal restore in a child. */
            for (int i = 0; i < nprocs; ++i) procs[i].exec_break_state = 0;
            if (exec_disable) shell->exec_break_state = 2;
            break_flag = cpu.d.l & 1;
            return 1;
        }
        case 0x05: cpu.d.l = 3; return 1;
        case 0x06: cpu.b.l = 7; cpu.b.h = 10; cpu.d.l = 0; cpu.d.h = 0; return 1;
        default: cpu.a.l = 0xFF; return 1;
        }
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
    case 0x50: cur_psp = cpu.b.x; dos_fs_set_process(cur_psp); return 1;
    case 0x51: case 0x62: cpu.b.x = cur_psp; return 1;
    case 0x52: cpu.es = DOS_SEG; cpu.b.x = LOL_OFF; return 1;
    case 0x55: {
        /* Preflight before adoption: a bad copy must not leave a process
         * registered after the PSP creation itself has failed. */
        if (!guest_span((uint32_t)cpu.d.x << 4, 256) ||
            !guest_span((uint32_t)cur_psp << 4, 256)) { fail(8); return 1; }
        err = adopt_dos2_child(cpu.d.x);
        if (err < 0) return 1;
        PspCopy *copy = NULL;
        if (!err && cpu.d.x != cur_psp) {
            copy = malloc(sizeof *copy);
            if (!copy) { fail(8); return 1; }
            copy->psp = cpu.d.x;
            copy->owner = nprocs ? procs[nprocs - 1].child : cur_psp;
            forget_psp_copy(cpu.d.x);
            copy->next = psp_copies;
            psp_copies = copy;
        }
        /* The new PSP may overlap the current one: memmove, never memcpy. */
        if (guest_move((uint32_t)cpu.d.x << 4, (uint32_t)cur_psp << 4, 256)) { fail(8); return 1; }
        wr16(cpu.d.x, 0x16, cur_psp);
        wr16(cpu.d.x, 0x02, cpu.si);
        wr16(cpu.d.x, 0x32, 20); /* EXEC inherits only the inline twenty slots. */
        wr16(cpu.d.x, 0x34, 0x18);
        wr16(cpu.d.x, 0x36, cpu.d.x);
        if (err > 0) wr16(cpu.d.x, 0x40, image_version(procs[nprocs - 1].image));
        dos_fs_inherit_process(cpu.d.x, cur_psp);
        cur_psp = cpu.d.x;
        dos_fs_set_process(cur_psp);
        return 1;
    }
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
    case 0x67:
        err = grow_job_file_table(cpu.b.x);
        if (err) fail((uint16_t)err);
        else cpu.cf = 0;
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
    case 0x28: term_idle(5); return 0; /* DOS idle: the program is waiting */
    case 0x2E: {
        /* COMMAND.COM's back door: DS:SI is a count byte, the command, CR */
        uint8_t line[128];
        for (int i = 0; i < 128; i++) line[i] = rd8(cpu.ds, (uint16_t)(cpu.si + i));
        size_t len = line[0] > 126 ? 126 : line[0];
        const uint8_t *cr = memchr(line + 1, '\r', len);
        if (cr) len = (size_t)(cr - (line + 1));
        int status = line[0] >= 126 ? refuse_long_command() : run_dos_command(line + 1, len);
        if (hle_redirect) return 0;
        last_retcode = (uint16_t)(status & 0xFF);
        cpu.a.x = 0;
        return 0;
    }
    case 0x2F:
        if (cpu.a.x == 0x1680) { term_idle(10); cpu.a.l = 0; } /* release the time slice: idle */
        else if (cpu.a.x == 0x4300) cpu.a.l = 0;
        return 0;
    case 0x67: cpu.a.h = 0x84; return 0;
    case 0x24: cpu.a.l = 3; return 1;
    default: return 1;
    }
}

/* ---- the first process ---------------------------------------------------- */

/* Mark each drive the file layer serves as a physical drive in the CDS, which
 * programs walk to list drives. */
static void mark_valid_drives(void) {
    for (int d = 0; d < 8; d++) {
        char root[4] = {(char)('A' + d), ':', '\\', 0};
        for (int i = 0; i < 4; i++) wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), (uint8_t)root[i]);
        char host[4096];
        int valid = dos_fs_to_host(DOS_SEG, SCRATCH_OFF, host, sizeof host) == 0;
        wr16(DOS_SEG, (uint16_t)(CDS_OFF + d * 0x58 + 0x43), valid ? 0x4000 : 0);
    }
}

static int start_first(const Image *image, const char *dos_prog,
                       const uint8_t *tail, int tail_len) {
    mark_valid_drives();
    char env[2048];
    size_t n = 0;
    char tmpdos[160] = "C:\\tmp";
    const char *tmp = door_mode ? NULL : getenv("TMPDIR");
    uint8_t conv[128];
    if (tmp && tmp[0] == '/' && !utf8_to_dos(tmp, conv, sizeof conv)) snprintf(tmpdos, sizeof tmpdos, "%s", conv);
    if (door_mode) strcpy(tmpdos, "H:\\");
    const char *vars[] = {
#ifdef __EMSCRIPTEN__
        "COMSPEC=H:\\COMMAND.COM",
#else
        door_mode ? "COMSPEC=H:\\COMMAND.COM" : "COMSPEC=C:\\bin\\sh",
#endif
        "PROMPT=$P$G", NULL};
    for (int i = 0; vars[i]; i++) n += (size_t)snprintf(env + n, sizeof env - n, "%s", vars[i]) + 1;
    char program_dir[128];
    snprintf(program_dir, sizeof program_dir, "%s", dos_prog);
    char *slash = strrchr(program_dir, '\\');
    if (slash) slash[1] = 0;
    else strcpy(program_dir, "C:\\");
    if (image == &image_vc_com) {
        const char *names[] = {"VC.COM", "VC.OVL"};
        for (int i = 0; i < 2; ++i) {
            int length = snprintf(builtin_dos[i], sizeof builtin_dos[i], "%s%s", program_dir, names[i]);
            if (length < 0 || (size_t)length >= sizeof builtin_dos[i]) return 3;
            if (short_dos_path(builtin_dos[i], builtin_short[i])) builtin_short[i][0] = 0;
            if (dos_path_host(builtin_dos[i], builtin_host[i], sizeof builtin_host[i])) builtin_host[i][0] = 0;
        }
    }
#ifdef __EMSCRIPTEN__
    strcpy(vc405_directory, "H:\\VC405");
#else
    int vc405_length = snprintf(vc405_directory, sizeof vc405_directory,
                               door_mode ? "H:\\VC405" : "%sVC405", program_dir);
    if (vc405_length < 0 || (size_t)vc405_length >= sizeof vc405_directory) return 3;
#endif
#ifdef __EMSCRIPTEN__
    n += (size_t)snprintf(env + n, sizeof env - n, "PATH=H:\\;H:\\DOS;H:\\GAMES;%s;C:\\", program_dir) + 1;
#else
    native_dos2_directory[0] = 0;
    if (door_mode)
        n += (size_t)snprintf(env + n, sizeof env - n, "PATH=H:\\;H:\\GAMES;H:\\.VC") + 1;
    else {
        int size = snprintf(native_dos2_directory, sizeof native_dos2_directory, "%sDOS2", program_dir);
        if (size < 0 || (size_t)size >= sizeof native_dos2_directory) return 3;
        n += (size_t)snprintf(env + n, sizeof env - n, "PATH=%s;C:\\", program_dir) + 1;
    }
#endif
    n += (size_t)snprintf(env + n, sizeof env - n, "TEMP=%s", tmpdos) + 1;
    n += (size_t)snprintf(env + n, sizeof env - n, "TMP=%s", tmpdos) + 1;
    if (is_vc405_image(image)) {
        int err = vc_environment(image, env, &n, sizeof env);
        if (err) return err;
    }

    uint8_t t[128] = {0};
    if (tail_len > 126) tail_len = 126;
    t[0] = (uint8_t)tail_len;
    memcpy(t + 1, tail, (size_t)tail_len);
    t[1 + tail_len] = 0x0D;
    return load_image(image, env, n, t, dos_prog, 0, STUB_SEG, STUB_EXIT);
}

void dos_start(const char *host_prog, const uint8_t *tail, int tail_len) {
    uint8_t dos_prog[128];
    if (utf8_to_dos(host_prog, dos_prog, sizeof dos_prog)) {
        /* A user error, not a crash: say what to change. */
        term_shutdown();
        fprintf(stderr, "vc: %s: DOS needs this path under 128 bytes in code page 866;"
                        " set XDG_CONFIG_HOME to a shorter directory\n", host_prog);
        exit(1);
    }
    if (start_first(&image_vc_com, (const char *)dos_prog, tail, tail_len))
        rt_fault("cannot load VC.COM");
}

int dos_door_start(const char *program) {
    if (!door_mode) return 5;
    const Image *image = &image_vc_com;
    char dos_prog[256] = "H:\\.VC\\VC.COM", host[4096];
    static const uint8_t vc_tail[] = " /std /notsr";
    if (program) {
        int found = find_program_path(program, "H:\\;H:\\GAMES", dos_prog, sizeof dos_prog,
                                      host, sizeof host);
        if (found <= 0) return found < 0 ? -found : 2;
        int err = known_image(host, &image);
        if (err) return err;
    }
    return start_first(image, dos_prog, program ? (const uint8_t *)"" : vc_tail,
                       program ? 0 : (int)sizeof vc_tail - 1);
}
