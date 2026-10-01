/* DOS kernel pieces that are about processes and memory, not files:
 * the MCB chain, PSPs, environments, EXEC and terminate, interrupt vectors,
 * version and system info, plus the minor BIOS and multiplex interrupts.
 *
 * Everything VC can see is real bytes in `mem`: the MCB chain, PSPs, the
 * List of Lists. VC edits MCB headers directly and walks the chain itself,
 * so this code reads the chain back from memory every time. */
#include <errno.h>
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

/* Run `script` with /bin/sh in the DOS current directory, the terminal handed
 * back while it runs. Values that come from file names are never pasted into
 * the script: they go in as positional arguments ($0, $1), which the shell
 * does not parse. */
static int host_run(const char *script, const char *arg0, const char *arg1) {
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
    const char *home = getenv("HOME");
    if (!word[0]) snprintf(path, sizeof path, "%s", home ? home : "/");
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
    }
    rt_log("cd %s: %s", path, failed ? "failed" : "ok");
    return failed;
}

/* data/VCEDIT.EXT maps every file to `vc-edit !.!`. VC puts the file name in
 * place of !.!, and everything after the word is taken as that one name, so
 * no shell ever parses it: `$(id).txt` stays a file name. */
static const char EDIT_WORD[] = "vc-edit ";

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
        if (n > 255) n = 255;
        for (size_t i = 0; i < n; i++) wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + i), cmd[w + i]);
        wr8(DOS_SEG, (uint16_t)(SCRATCH_OFF + n), 0);
        char host[4096];
        if (dos_fs_to_host(DOS_SEG, SCRATCH_OFF, host, sizeof host)) {
            static const char msg[] = "File not found\r\n";
            con_write((const uint8_t *)msg, sizeof msg - 1);
            return 2;
        }
        return host_run("exec ${EDITOR:-vi} \"$1\"", "vc-edit", host);
    }
    return host_run(utf8, NULL, NULL);
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
    static char envbuf[32768];
    size_t elen = env_strings(envseg ? envseg : rd16(cur_psp, 0x2C), envbuf, sizeof envbuf);
    int err = load_image(img, envbuf, elen, tail, dos_prog, parent_psp, ret_cs, ret_ip);
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
    wr8(DOS_SEG, LOL_OFF + 0x20, 8);
    wr8(DOS_SEG, LOL_OFF + 0x21, 8); /* LASTDRIVE=H */
    /* NUL device header, end of the device chain */
    uint16_t nul = LOL_OFF + 0x22;
    wr16(DOS_SEG, nul, 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 2), 0xFFFF);
    wr16(DOS_SEG, (uint16_t)(nul + 4), 0x8004);
    memcpy(&mem[a + nul + 10], "NUL     ", 8);
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
    case 0x28: term_idle(5); return 0; /* DOS idle: the program is waiting */
    case 0x2E: {
        /* COMMAND.COM's back door: DS:SI is a count byte, the command, CR */
        uint8_t line[128];
        for (int i = 0; i < 128; i++) line[i] = rd8(cpu.ds, (uint16_t)(cpu.si + i));
        size_t len = line[0] > 126 ? 126 : line[0];
        const uint8_t *cr = memchr(line + 1, '\r', len);
        if (cr) len = (size_t)(cr - (line + 1));
        last_retcode = (uint16_t)((line[0] >= 126 ? refuse_long_command() : run_dos_command(line + 1, len)) & 0xFF);
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

void dos_start(const char *host_prog, const uint8_t *tail, int tail_len) {
    mark_valid_drives();
    uint8_t dos_prog[128];
    if (utf8_to_dos(host_prog, dos_prog, sizeof dos_prog)) {
        /* A user error, not a crash: say what to change. */
        term_shutdown();
        fprintf(stderr, "vc: %s: DOS needs this path under 128 bytes in code page 866;"
                        " set XDG_CONFIG_HOME to a shorter directory\n", host_prog);
        exit(1);
    }

    char env[2048];
    size_t n = 0;
    char tmpdos[160] = "C:\\tmp";
    const char *tmp = getenv("TMPDIR");
    uint8_t conv[128];
    if (tmp && tmp[0] == '/' && !utf8_to_dos(tmp, conv, sizeof conv)) snprintf(tmpdos, sizeof tmpdos, "%s", conv);
    const char *vars[] = {"COMSPEC=C:\\bin\\sh", "PATH=C:\\", "PROMPT=$P$G", NULL};
    for (int i = 0; vars[i]; i++) n += (size_t)snprintf(env + n, sizeof env - n, "%s", vars[i]) + 1;
    n += (size_t)snprintf(env + n, sizeof env - n, "TEMP=%s", tmpdos) + 1;
    n += (size_t)snprintf(env + n, sizeof env - n, "TMP=%s", tmpdos) + 1;

    uint8_t t[128] = {0};
    if (tail_len > 126) tail_len = 126;
    t[0] = (uint8_t)tail_len;
    memcpy(t + 1, tail, (size_t)tail_len);
    t[1 + tail_len] = 0x0D;
    if (load_image(&image_vc_com, env, n, t, (const char *)dos_prog, 0, STUB_SEG, STUB_EXIT))
        rt_fault("cannot load VC.COM");
}
