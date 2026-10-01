#define _GNU_SOURCE
/* DOS process tests through public interrupts. The translations are tiny
 * stand-ins: file identity and command parsing do not need generated code. */
#include "rt.h"
#include "hle.h"

#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t vc_file[] = {0xCD, 0x20};
static const uint8_t ovl_file[] = {'M', 'Z', 1, 0, 0, 0, 0, 0, 0xCB};
static const uint8_t basic_file[] = {'M', 'Z', 2, 0, 0, 0, 0, 0, 0x90, 0xCB};
static const uint8_t logo_file[] = {0xB8, 0x04, 0x00, 0xCD, 0x10, 0xCD, 0x20};
static const uint8_t association[] = "bas: gwbasic !.!\r\n";
const Image image_vc_com = {.name = "VC.COM", .bytes = vc_file, .size = sizeof vc_file};
const Image image_vc_ovl = {
    .name = "VC.OVL", .is_exe = 1, .bytes = ovl_file + 8, .size = sizeof ovl_file - 8,
    .hdr_sp = 0x100, .min_alloc = 0x10, .max_alloc = 0x20,
};
const Image image_gwbasic = {
    .name = "GWBASIC.EXE", .is_exe = 1, .bytes = basic_file + 8, .size = sizeof basic_file - 8,
    .hdr_sp = 0x100, .min_alloc = 0x10, .max_alloc = 0x20,
};
const Image image_bootlogo = {.name = "LOGO.COM", .bytes = logo_file, .size = sizeof logo_file};
const EmbeddedFile embedded_files[] = {
    {"VC.COM", vc_file, sizeof vc_file}, {"VC.OVL", ovl_file, sizeof ovl_file},
    {"GWBASIC.EXE", basic_file, sizeof basic_file}, {"VC.EXT", association, sizeof association - 1},
    /* The installed asset name is independent of the translation's label. */
    {"BOOTLOGO.COM", logo_file, sizeof logo_file},
};
const int embedded_file_count = sizeof embedded_files / sizeof embedded_files[0];
int hle_redirect, rt_exited, rt_exit_code;
static const Image *registered;
static unsigned host_runs, checks;
static uint16_t parent_psp;
static char fixture[128], original_cwd[4096], output[4096];

void rt_register_image(const Image *img, uint16_t seg) { (void)seg; registered = img; }
RtProcessState *rt_save_process_state(void) { return malloc(1); }
void rt_finish_process_state(RtProcessState *state, int restore) { (void)restore; free(state); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_update_clock(void) {}
void rt_yield(void) { rt_budget = 1000; }
void term_suspend(void) { host_runs++; }
void term_resume(void) {}
void term_shutdown(void) {}
void term_render(void) {}
void term_idle(int ms) { (void)ms; }
void con_write(const uint8_t *buf, size_t n) {
    size_t used = strlen(output), room = sizeof output - used - 1;
    if (n > room) n = room;
    memcpy(output + used, buf, n);
    output[used + n] = 0;
}
_Noreturn void rt_fault(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

#define CHECK(cond) do { checks++; if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

static void host_file(const char *name, const uint8_t *bytes, size_t n) {
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, n, file) == n);
    CHECK(fclose(file) == 0);
}

static void guest_string(uint16_t off, const char *text) {
    size_t n = strlen(text);
    for (size_t i = 0; i <= n; i++) wr8(parent_psp, (uint16_t)(off + i), (uint8_t)text[i]);
}

static void fresh_machine(void) {
    dos_core_init();
    dos_fs_init();
    char file[256];
    snprintf(file, sizeof file, "%s/VC.COM", fixture);
    dos_start(file, (const uint8_t *)"", 0);
    parent_psp = cpu.ds;
    cpu.es = parent_psp;
    cpu.a.h = 0x4A;
    cpu.b.x = 0x1000;
    CHECK(dos_core_int21() && !cpu.cf);
    registered = NULL;
    hle_redirect = 0;
    host_runs = 0;
    output[0] = 0;
    cpu.cs = cpu.ss = cpu.ds = cpu.es = parent_psp;
    cpu.sp = 0xE000;
    cpu.ifl = 1;
    cpu.ip = 0x2345;
}

static void path_env(const char *value) {
    cpu.a.h = 0x48;
    cpu.b.x = 0x100;
    CHECK(dos_core_int21() && !cpu.cf);
    uint16_t env = cpu.a.x;
    char text[512];
    int n = snprintf(text, sizeof text, "PATH=%s", value);
    CHECK(n > 0 && (size_t)n + 2 < sizeof text);
    text[n + 1] = 0;
    memcpy(mem + lin(env, 0), text, (size_t)n + 2);
    wr16(parent_psp, 0x2C, env);
}

static void exec_file(const char *name, const char *tail) {
    guest_string(0x1000, name);
    size_t n = strlen(tail);
    CHECK(n < 126);
    wr8(parent_psp, 0x1200, (uint8_t)n);
    for (size_t i = 0; i < n; i++) wr8(parent_psp, (uint16_t)(0x1201 + i), (uint8_t)tail[i]);
    wr8(parent_psp, (uint16_t)(0x1201 + n), '\r');
    wr16(parent_psp, 0x1100, 0);
    wr16(parent_psp, 0x1102, 0x1200);
    wr16(parent_psp, 0x1104, parent_psp);
    cpu.ds = cpu.es = parent_psp;
    cpu.d.x = 0x1000;
    cpu.b.x = 0x1100;
    cpu.a.x = 0x4B00;
    cpu_int(0x21, 0x2345);
    CHECK(dos_core_int21());
}

static void command(const char *text, int quick) {
    if (quick) {
        size_t n = strlen(text);
        CHECK(n < 126);
        wr8(parent_psp, 0x1200, (uint8_t)n);
        for (size_t i = 0; i < n; i++) wr8(parent_psp, (uint16_t)(0x1201 + i), (uint8_t)text[i]);
        wr8(parent_psp, (uint16_t)(0x1201 + n), '\r');
        cpu.ds = parent_psp;
        cpu.si = 0x1200;
        cpu_int(0x2E, 0x2345);
        dos_int_other(0x2E);
    } else {
        char tail[128];
        int n = snprintf(tail, sizeof tail, " /C %s", text);
        CHECK(n > 0 && n < 126);
        exec_file("C:\\bin\\sh", tail);
    }
}

static void child_tail(const char *expected) {
    CHECK(hle_redirect && registered);
    size_t n = strlen(expected);
    CHECK(rd8(cpu.ds, 0x80) == n);
    CHECK(!memcmp(mem + lin(cpu.ds, 0x81), expected, n));
    CHECK(rd8(cpu.ds, (uint16_t)(0x81 + n)) == '\r');
}

static void end_child(void) {
    hle_redirect = 0;
    cpu.a.x = 0x4C2A;
    CHECK(dos_core_int21());
    CHECK(hle_redirect && cpu.cs == parent_psp && cpu.ip == 0x2345);
    CHECK(cpu.sp == 0xE000 && cpu.ifl && !cpu.cf);
    cpu.a.h = 0x4D;
    CHECK(dos_core_int21() && cpu.a.x == 42);
}

static void test_exec_identity(void) {
    host_file("BASIC.EXE", basic_file, sizeof basic_file);
    host_file("BASIC.COM", basic_file, sizeof basic_file);
    const char *names[] = {"BASIC.EXE", "BASIC.COM"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        fresh_machine();
        exec_file(names[i], " /D TEST.BAS");
        CHECK(registered == &image_gwbasic);
        child_tail(" /D TEST.BAS");
        end_child();
    }
    uint8_t changed[sizeof basic_file];
    memcpy(changed, basic_file, sizeof changed);
    changed[3] ^= 1; /* Header only: translated load module is still equal. */
    host_file("BAD.EXE", changed, sizeof changed);
    fresh_machine();
    exec_file("BAD.EXE", "");
    CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 11 && !host_runs);
    fresh_machine();
    exec_file("MISSING.EXE", "");
    CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 2 && !host_runs);
    CHECK(mkfifo("HANG.EXE", 0600) == 0);
    fresh_machine();
    signal(SIGALRM, SIG_DFL);
    alarm(5); /* A regression to blocking fopen must fail instead of hanging. */
    exec_file("HANG.EXE", "");
    alarm(0);
    CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 5 && !host_runs);
    fresh_machine();
    exec_file("VC.OVL", "");
    CHECK(registered == &image_vc_ovl);
    end_child();
}

static void test_logo_identity(void) {
    /* A COM program obeys exactly the same complete-file identity rule as
     * the MZ interpreter; neither a .COM suffix nor the LOGO name is magic. */
    host_file("BOOTLOGO.COM", logo_file, sizeof logo_file);
    host_file("LOGO.COM", logo_file, sizeof logo_file);
    host_file("TURTLE.EXE", logo_file, sizeof logo_file);
    const char *names[] = {"BOOTLOGO.COM", "LOGO.COM", "TURTLE.EXE"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        fresh_machine();
        exec_file(names[i], "");
        CHECK(registered == &image_bootlogo && !host_runs);
        CHECK(cpu.ip == 0x100 && cpu.cs == cpu.ds && cpu.ss == cpu.ds);
        CHECK(!memcmp(mem + lin(cpu.ds, 0x100), logo_file, sizeof logo_file));
        child_tail("");
        end_child();
    }
    for (unsigned byte = 0; byte < sizeof logo_file; ++byte) {
        uint8_t changed[sizeof logo_file];
        memcpy(changed, logo_file, sizeof changed);
        changed[byte] ^= 1;
        host_file("LOGO.COM", changed, sizeof changed);
        fresh_machine();
        exec_file("LOGO.COM", "");
        CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 11 && !host_runs);
    }
    host_file("LOGO.COM", logo_file, sizeof logo_file);
    for (int quick = 0; quick < 2; ++quick) {
        fresh_machine();
        command("bootlogo", quick);
        CHECK(registered == &image_bootlogo && !host_runs);
        end_child();
        fresh_machine();
        command("logo", quick);
        CHECK(registered == &image_bootlogo && !host_runs);
        end_child();
    }
}

static void test_search(void) {
    host_file("FIRST.COM", vc_file, sizeof vc_file);
    host_file("FIRST.EXE", basic_file, sizeof basic_file);
    host_file("BIN1/FIRST.EXE", basic_file, sizeof basic_file);
    host_file("BIN1/ONLY.EXE", basic_file, sizeof basic_file);
    host_file("BIN2/ONLY.COM", vc_file, sizeof vc_file);
    host_file("BIN2/LATER.EXE", basic_file, sizeof basic_file);
    const struct { const char *command; const Image *image; } cases[] = {
        {"first", &image_vc_com}, {"fIrSt.ExE /D", &image_gwbasic},
        {"only", &image_gwbasic}, {"later", &image_gwbasic},
        {"H:\\BIN2\\ONLY", &image_vc_com}, {"\"H:\\BIN1\\ONLY.EXE\" X", &image_gwbasic},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh_machine();
        path_env("H:\\BIN1;H:\\BIN2");
        command(cases[i].command, i & 1);
        CHECK(hle_redirect && registered == cases[i].image && !host_runs);
        end_child();
    }
    /* PATH is inherited from DOS, not getenv("PATH"), and a full path does
     * not silently substitute an unrelated executable from it. */
    fresh_machine();
    path_env("H:\\BIN1;H:\\BIN2");
    exec_file("H:\\BIN2\\NOFILE.EXE", "");
    CHECK(!hle_redirect && cpu.cf && cpu.a.x == 2 && !host_runs);
}

static void test_association(void) {
    host_file("safe;touch PWNED;.bas", (const uint8_t *)"10 END\r\n", 8);
    for (int quick = 0; quick < 2; quick++) {
        host_file("GWBASIC.EXE", basic_file, sizeof basic_file);
        fresh_machine();
        command("gwbasic safe;touch PWNED;.bas", quick);
        CHECK(registered == &image_gwbasic && !host_runs);
        child_tail(" safe;touch PWNED;.bas");
        end_child();
        CHECK(access("PWNED", F_OK) != 0);
        CHECK(unlink("GWBASIC.EXE") == 0);
        fresh_machine();
        command("gwbasic safe;touch PWNED;.bas", quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(strstr(output, "Program not found"));
        CHECK(access("PWNED", F_OK) != 0);
    }
}

static void test_psp_services(void) {
    fresh_machine();
    uint16_t copy = (uint16_t)(parent_psp + 0x100);
    cpu.a.x = 0x2600;
    cpu.d.x = copy;
    CHECK(dos_core_int21());
    CHECK(!memcmp(mem + lin(copy, 0), mem + lin(parent_psp, 0), 256));
    cpu.a.h = 0x51;
    CHECK(dos_core_int21() && cpu.b.x == parent_psp);
    cpu.cs = parent_psp;
    cpu.ip = 1;
    CHECK(!dos_run_psp());
    cpu.ip = 0;
    wr8(parent_psp, 0, 0x90);
    CHECK(!dos_run_psp());
    wr8(parent_psp, 0, 0xcd);
    cpu.cs = copy; /* A copied PSP is not the live process's entry thunk. */
    CHECK(!dos_run_psp());
    cpu.cs = parent_psp;
    CHECK(dos_run_psp());
    CHECK(cpu.cs == STUB_SEG && cpu.ip == 0x20);
    CHECK(rd16(cpu.ss, cpu.sp) == 2 && rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == parent_psp);
}

static void open_fcb(uint16_t seg, uint16_t off, const char *name) {
    for (unsigned i = 0; i < 37; i++) wr8(seg, (uint16_t)(off + i), 0);
    for (size_t i = 0; i <= strlen(name); i++) wr8(seg, (uint16_t)(off + 40 + i), (uint8_t)name[i]);
    cpu.ds = cpu.es = seg;
    cpu.si = (uint16_t)(off + 40);
    cpu.di = off;
    cpu.a.x = 0x2900;
    CHECK(dos_fs_int21() && cpu.a.l == 0);
    cpu.d.x = off;
    cpu.a.h = 0x0f;
    CHECK(dos_fs_int21() && cpu.a.l == 0);
}

static void test_child_fcb_lifetime(void) {
    host_file("PARENT.DAT", (const uint8_t *)"parent", 6);
    host_file("CHILD.DAT", (const uint8_t *)"child", 5);
    fresh_machine();
    guest_string(0x3100, "PARENT.DAT");
    cpu.a.x = 0x3d00;
    cpu.d.x = 0x3100;
    CHECK(dos_fs_int21() && !cpu.cf);
    uint16_t parent_handle = cpu.a.x;
    open_fcb(parent_psp, 0x3000, "PARENT.DAT");
    for (unsigned i = 0; i < 80; i++) {
        registered = NULL;
        hle_redirect = 0;
        exec_file("BASIC.EXE", "");
        CHECK(hle_redirect && registered == &image_gwbasic);
        uint16_t child = cpu.ds;
        open_fcb(child, 0x180, "CHILD.DAT");
        memcpy(mem + lin(parent_psp, 0x3500), mem + lin(child, 0x180), 37);
        end_child(); /* Deliberately never close the child's FCB. */
        cpu.a.h = 0x10;
        cpu.d.x = 0x3500;
        CHECK(dos_fs_int21() && cpu.a.l == 0xff); /* Its copied token is stale. */
    }
    cpu.a.h = 0x1a;
    cpu.d.x = 0x4000;
    CHECK(dos_fs_int21());
    cpu.a.h = 0x21;
    cpu.d.x = 0x3000;
    CHECK(dos_fs_int21() && cpu.a.l == 3);
    CHECK(!memcmp(mem + lin(parent_psp, 0x4000), "parent", 6));
    cpu.a.h = 0x10;
    CHECK(dos_fs_int21() && cpu.a.l == 0);
    cpu.a.h = 0x3f;
    cpu.b.x = parent_handle;
    cpu.c.x = 6;
    cpu.d.x = 0x4000;
    CHECK(dos_fs_int21() && !cpu.cf && cpu.a.x == 6);
    CHECK(!memcmp(mem + lin(parent_psp, 0x4000), "parent", 6));
    cpu.a.h = 0x3e;
    CHECK(dos_fs_int21() && !cpu.cf);
}

static int remove_fixture(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st; (void)walk;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

int main(void) {
    CHECK(getcwd(original_cwd, sizeof original_cwd));
    strcpy(fixture, "/tmp/vc-exec-XXXXXX");
    CHECK(mkdtemp(fixture));
    CHECK(setenv("HOME", fixture, 1) == 0);
    CHECK(chdir(fixture) == 0);
    CHECK(mkdir("BIN1", 0700) == 0 && mkdir("BIN2", 0700) == 0);
    host_file("VC.COM", vc_file, sizeof vc_file);
    host_file("VC.OVL", ovl_file, sizeof ovl_file);
    test_exec_identity();
    test_logo_identity();
    test_search();
    test_association();
    test_psp_services();
    test_child_fcb_lifetime();
    CHECK(chdir(original_cwd) == 0);
    CHECK(nftw(fixture, remove_fixture, 16, FTW_DEPTH | FTW_PHYS) == 0);
    printf("DOS EXEC: %u checks passed\n", checks);
    return 0;
}
