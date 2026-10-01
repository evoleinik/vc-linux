#define _GNU_SOURCE
/* DOS process tests through public interrupts. The translations are tiny
 * stand-ins: file identity and command parsing do not need generated code. */
#include "rt.h"
#include "hle.h"
#include "dos_fs.h"

#include <assert.h>
#include <dirent.h>
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
static const uint8_t rogue_file[] = {'M', 'Z', 3, 0, 0, 0, 0, 0, 0xB8, 0x00, 0x4C, 0xCD, 0x21};
static const uint8_t vz_file[] = {0xB8, 0x00, 0x4C, 0xCD, 0x21};
static const uint8_t kermit_file[] = {'M', 'Z', 4, 0, 0, 0, 0, 0, 0x90, 0x90, 0xCB};
static const uint8_t association[] = "bas: gwbasic !.!\r\ntak: kermit stay, take !.!\r\n";
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
const Image image_rogue = {
    .name = "ROGUE.EXE", .is_exe = 1, .bytes = rogue_file + 8, .size = sizeof rogue_file - 8,
    .hdr_sp = 0x200, .min_alloc = 0x20, .max_alloc = 0x30,
};
const Image image_vz = {.name = "VZ.COM", .bytes = vz_file, .size = sizeof vz_file};
const Image image_kermit = {
    .name = "KERMIT.EXE", .is_exe = 1, .bytes = kermit_file + 8, .size = sizeof kermit_file - 8,
    .hdr_sp = 0x100, .min_alloc = 0x10, .max_alloc = 0x20,
};
const EmbeddedFile embedded_files[] = {
    {"VC.COM", vc_file, sizeof vc_file}, {"VC.OVL", ovl_file, sizeof ovl_file},
    {"GWBASIC.EXE", basic_file, sizeof basic_file}, {"VC.EXT", association, sizeof association - 1},
    /* The installed asset name is independent of the translation's label. */
    {"BOOTLOGO.COM", logo_file, sizeof logo_file},
    {"ROGUE.EXE", rogue_file, sizeof rogue_file},
    {"VZ.COM", vz_file, sizeof vz_file},
    {"KERMIT.EXE", kermit_file, sizeof kermit_file},
};
const int embedded_file_count = sizeof embedded_files / sizeof embedded_files[0];
int hle_redirect, rt_exited, rt_exit_code;
static const Image *registered;
static const Image *captured_parent;
static int capture_fails;
static unsigned host_runs, checks, renders;
static uint16_t parent_psp;
static char fixture[128], original_cwd[4096], output[4096];

void rt_register_image(const Image *img, uint16_t seg) { (void)seg; registered = img; }
RtProcessState *rt_save_process_state(const Image *parent) {
    captured_parent = parent;
    return capture_fails ? NULL : malloc(1);
}
void rt_finish_process_state(RtProcessState *state, int restore) { (void)restore; free(state); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_update_clock(void) {}
void rt_yield(void) { rt_budget = 1000; }
void term_suspend(void) { host_runs++; }
void term_resume(void) {}
void term_shutdown(void) {}
void term_render(void) { ++renders; }
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
    captured_parent = NULL;
    capture_fails = 0;
    hle_redirect = 0;
    host_runs = 0;
    renders = 0;
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

static void test_rogue_identity(void) {
    /* Compiled C is still a DOS executable: its name and extension do not
     * select the runner, and even a header-only change must be refused. */
    const char *names[] = {"ROGUE.EXE", "DUNGEON.EXE", "RENAMED.COM"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        host_file(names[i], rogue_file, sizeof rogue_file);
        fresh_machine();
        exec_file(names[i], " -r");
        CHECK(registered == &image_rogue && !host_runs);
        CHECK(cpu.ip == 0 && cpu.cs == cpu.ds + 0x10 && cpu.sp == 0x200);
        CHECK(!memcmp(mem + lin(cpu.cs, 0), rogue_file + 8, sizeof rogue_file - 8));
        child_tail(" -r");
        end_child();
    }
    for (size_t byte = 0; byte < sizeof rogue_file; ++byte) {
        uint8_t changed[sizeof rogue_file];
        memcpy(changed, rogue_file, sizeof changed);
        changed[byte] ^= 1;
        host_file("DUNGEON.EXE", changed, sizeof changed);
        fresh_machine();
        exec_file("DUNGEON.EXE", "");
        CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 11 && !host_runs);
    }
    for (int quick = 0; quick < 2; ++quick) {
        fresh_machine();
        command("rogue", quick);
        CHECK(registered == &image_rogue && !host_runs);
        end_child();
    }
    CHECK(rename("ROGUE.EXE", "BIN1/ROGUE.EXE") == 0);
    fresh_machine();
    path_env("H:\\BIN1");
    command("rogue", 0);
    CHECK(registered == &image_rogue && !host_runs);
    end_child();
}

static void test_vz_identity_and_edit(void) {
    host_file("VZ.COM", vz_file, sizeof vz_file);
    host_file("EDITOR.EXE", vz_file, sizeof vz_file);
    fresh_machine();
    exec_file("EDITOR.EXE", " NEW.TXT");
    CHECK(registered == &image_vz && !host_runs);
    child_tail(" H:\\NEW.TXT");
    end_child();
    for (unsigned byte = 0; byte < sizeof vz_file; ++byte) {
        uint8_t changed[sizeof vz_file];
        memcpy(changed, vz_file, sizeof changed);
        changed[byte] ^= 1;
        host_file("EDITOR.EXE", changed, sizeof changed);
        fresh_machine();
        exec_file("EDITOR.EXE", "");
        CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 11 && !host_runs);
    }
    const char *name = "safe;touch PWNED;.txt";
    host_file(name, (const uint8_t *)"original\r\n", 10);
    for (int quick = 0; quick < 2; ++quick) {
        fresh_machine();
        command("vz NEW.TXT", quick);
        CHECK(registered == &image_vz && !host_runs);
        child_tail(" H:\\NEW.TXT");
        end_child();
        for (int empty = 0; empty < 2; ++empty) {
            CHECK((empty ? setenv("EDITOR", "", 1) : unsetenv("EDITOR")) == 0);
            fresh_machine();
            command("vc-edit safe;touch PWNED;.txt", quick);
            CHECK(registered == &image_vz && !host_runs);
            unsigned n = rd8(cpu.ds, 0x80);
            CHECK(n > 4 && n <= 126 && rd8(cpu.ds, 0x81) == ' ');
            CHECK(rd8(cpu.ds, (uint16_t)(0x81 + n)) == '\r');
            char path[128], actual[4096], expected[4096];
            memcpy(path, mem + lin(cpu.ds, 0x82), n - 1);
            path[n - 1] = 0;
            CHECK(!strpbrk(path, " ;,+") && !strncmp(path, "H:\\", 3));
            guest_string(0x3200, path);
            CHECK(!dos_fs_to_host(parent_psp, 0x3200, actual, sizeof actual));
            snprintf(expected, sizeof expected, "%s/%s", fixture, name);
            CHECK(!strcmp(actual, expected)); /* The short tail names the original host file. */
            end_child();
            CHECK(access("PWNED", F_OK) != 0);
        }
        /* F4 is DOS-only even if a different known program takes VZ's name,
         * or VZ disappears. No filename may fall through to the host shell. */
        const uint8_t *bad[] = {logo_file, rogue_file, (const uint8_t *)"damaged", NULL};
        const size_t sizes[] = {sizeof logo_file, sizeof rogue_file, 7, 0};
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (bad[i]) host_file("VZ.COM", bad[i], sizes[i]);
            else CHECK(unlink("VZ.COM") == 0);
            fresh_machine();
            command("vc-edit safe;touch PWNED;.txt", quick);
            CHECK(!hle_redirect && !registered && !host_runs);
            CHECK(strstr(output, bad[i] ? "Invalid program format" : "Program not found"));
            CHECK(access("PWNED", F_OK) != 0);
        }
        host_file("VZ.COM", vz_file, sizeof vz_file);
    }
}

static void test_vz_path_buffer_limit(void) {
    CHECK(unsetenv("EDITOR") == 0);
    /* H:\ plus six eight-byte directory components is 56 bytes. Even the
     * 63-byte filename cannot make the current directory safe for VZ's Open
     * dialog, whose later relative name could take twelve bytes. */
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(mkdir("DEPTH000", 0700) == 0);
        CHECK(chdir("DEPTH000") == 0);
    }
    host_file("AA.TXT", (const uint8_t *)"safe", 4);
    host_file("AAA.TXT", (const uint8_t *)"safe", 4);
    for (int quick = 0; quick < 2; ++quick) {
        fresh_machine();
        command("vc-edit AA.TXT", quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(!strcmp(output, "Current directory too long for VZ\r\n"));
        fresh_machine();
        command("vc-edit AAA.TXT", quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(!strcmp(output, "Current directory too long for VZ\r\n"));
    }
    CHECK(chdir(fixture) == 0);
}

/* Make the full short CWD exactly length bytes, independent of the random
 * host fixture prefix (which DOS exposes as the root of H:). */
static void enter_short_directory(size_t length) {
    CHECK(chdir(fixture) == 0);
    char name[9];
    static unsigned serial;
    snprintf(name, sizeof name, "D%zuX%u", length, serial++);
    CHECK(mkdir(name, 0700) == 0 && chdir(name) == 0);
    size_t used = 3 + strlen(name);
    while (used < length) {
        size_t n = length - used - 1;
        if (n > 8) n = length - used == 10 ? 7 : 8;
        CHECK(n > 0 && n < sizeof name);
        memset(name, 'D', n); name[n] = 0;
        CHECK(mkdir(name, 0700) == 0 && chdir(name) == 0);
        used += n + 1;
    }
}

static void current_short_directory(char out[128]) {
    Cpu saved = cpu;
    guest_string(0x3200, ".");
    cpu.ds = cpu.es = parent_psp;
    cpu.si = 0x3200; cpu.di = 0x3300; cpu.a.x = 0x6000;
    CHECK(dos_fs_int21() && !cpu.cf);
    strcpy(out, (const char *)(mem + lin(parent_psp, 0x3300)));
    cpu = saved;
}

static void test_vz_cwd_boundary(void) {
    const size_t lengths[] = {51, 54, 63, 128, 50};
    for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        enter_short_directory(lengths[i]);
        host_file("NOTES.TXT", (const uint8_t *)"safe", 4);
        for (unsigned entry = 0; entry < 4; ++entry) {
            fresh_machine();
            char directory[128], expected[160];
            if (lengths[i] < 128) {
                current_short_directory(directory);
                CHECK(strlen(directory) == lengths[i]);
            }
            if (entry < 2) command("vz NOTES.TXT", entry);
            else exec_file("H:\\VZ.COM", entry == 2 ? " NOTES.TXT" : "");
            if (lengths[i] + 13 >= 64) {
                CHECK(!hle_redirect && !registered && !host_runs);
                CHECK(!strcmp(output, "Current directory too long for VZ\r\n"));
                CHECK(renders == 1); /* Flush before VC restores its panels. */
                if (entry >= 2) CHECK(cpu.cf && cpu.a.x == 3);
            } else {
                CHECK(registered == &image_vz && !host_runs);
                if (entry == 3) child_tail("");
                else {
                    snprintf(expected, sizeof expected, " %s\\NOTES.TXT", directory);
                    child_tail(expected);
                }
                end_child();
            }
        }
    }
    CHECK(chdir(fixture) == 0);
}

static void test_vz_command_file_paths(void) {
    const struct { const char *input, *expected; } cases[] = {
        {" NEW.TXT", " H:\\NEW.TXT"},
        {" -z", " -z"},
        {" -Eb+ NEW.TXT OTHER.TXT", " -Eb+ H:\\NEW.TXT H:\\OTHER.TXT"},
        {" -z -e NEW.TXT\tBIN1\\OTHER.TXT", " -z -e H:\\NEW.TXT\tH:\\BIN1\\OTHER.TXT"},
        {" .\\BIN1\\..\\NEW.TXT", " H:\\NEW.TXT"},
        {" \"typed long filename.txt\" NEW.TXT", " H:\\TYPEDL~1.TXT H:\\NEW.TXT"},
        {" NEW.TXT -another-long-filename.txt", " H:\\NEW.TXT H:\\-ANOTH~1.TXT"},
        {" NEW.TXT /BIN1/OTHER.TXT", " H:\\NEW.TXT H:\\BIN1\\OTHER.TXT"},
        {" / CUSTOM.DEF -Eb+ NEW.TXT", " / CUSTOM.DEF -Eb+ H:\\NEW.TXT"},
        {" /ONE.DEF+TWO.DEF -Eb+ NEW.TXT", " /ONE.DEF+TWO.DEF -Eb+ H:\\NEW.TXT"},
        {" +EXTRA.DEF NEW.TXT", " +EXTRA.DEF H:\\NEW.TXT"},
        {" @@LIST.TXT", " @@LIST.TXT"},
    };
    host_file("typed long filename.txt", (const uint8_t *)"safe", 4);
    host_file("-another-long-filename.txt", (const uint8_t *)"safe", 4);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        for (unsigned entry = 0; entry < 3; ++entry) {
            fresh_machine();
            if (entry == 2) exec_file("H:\\VZ.COM", cases[i].input);
            else {
                char cmd[128];
                snprintf(cmd, sizeof cmd, "vz%s", cases[i].input);
                command(cmd, entry);
            }
            CHECK(registered == &image_vz && !host_runs);
            child_tail(cases[i].expected);
            end_child();
        }
    }
}

static void test_vz_command_file_limits(void) {
    enter_short_directory(56);
    host_file("AA.TXT", (const uint8_t *)"safe", 4);
    host_file("AAA.TXT", (const uint8_t *)"safe", 4);
    fresh_machine();
    char directory[128];
    current_short_directory(directory);
    CHECK(strlen(directory) == 56);
    CHECK(chdir(fixture) == 0);
    /* A safe CWD does not make an explicit filename in a deep directory safe.
     * Include a missing file and a second argument, not only F4's one file. */
    const struct { const char *prefix, *name; int accepted; } cases[] = {
        {" NEW.TXT ", "AAA.TXT", 0},
        {" -Eb+ ", "NEW.TXT", 0},
        {" ", "AA.TXT", 1},
        {" ", "NN.TXT", 1},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        fresh_machine();
        char tail[256];
        snprintf(tail, sizeof tail, "%s%s\\%s", cases[i].prefix, directory, cases[i].name);
        exec_file("H:\\VZ.COM", tail);
        if (cases[i].accepted) {
            CHECK(registered == &image_vz && !host_runs);
            child_tail(tail);
            end_child();
        } else {
            CHECK(!hle_redirect && !registered && !host_runs && cpu.cf && cpu.a.x == 3);
            CHECK(!strcmp(output, "File path too long for VZ\r\n"));
        }
    }
    enter_short_directory(50);
    fresh_machine();
    /* Three individually safe names would overflow the rebuilt DOS tail. */
    exec_file("H:\\VZ.COM", " A.TXT B.TXT C.TXT");
    CHECK(!hle_redirect && !registered && !host_runs && cpu.cf && cpu.a.x == 3);
    CHECK(!strcmp(output, "Command line too long for VZ\r\n"));
    CHECK(chdir(fixture) == 0);
}

static void test_vz_wildcard_paths(void) {
    CHECK(mkdir("wild parent directory", 0700) == 0);
    const struct { const char *input, *expected; } cases[] = {
        {" *.TXT", " H:\\*.TXT"},
        {" -e BIN1\\N?TE.*", " -e H:\\BIN1\\N?TE.*"},
        {" H:*.TXT", " H:\\*.TXT"},
        {" .\\BIN1\\..\\N??.*", " H:\\N??.*"},
        {" \"wild parent directory\\*.TXT\"", " H:\\WILDPA~1\\*.TXT"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        for (unsigned entry = 0; entry < 3; ++entry) {
            fresh_machine();
            if (entry == 2) exec_file("H:\\VZ.COM", cases[i].input);
            else {
                char cmd[128];
                snprintf(cmd, sizeof cmd, "vz%s", cases[i].input);
                command(cmd, entry);
            }
            CHECK(registered == &image_vz && !host_runs);
            child_tail(cases[i].expected);
            end_child();
        }
    }
    fresh_machine();
    exec_file("H:\\VZ.COM", " WILD*\\NOTE.TXT");
    CHECK(!hle_redirect && !registered && !host_runs && cpu.cf && cpu.a.x == 3);

    enter_short_directory(56);
    fresh_machine();
    char directory[128];
    current_short_directory(directory);
    CHECK(chdir(fixture) == 0);
    const char *patterns[] = {"*A.TXT", "*AA.TXT", "A?.TXT", "AA?.TXT"};
    for (unsigned i = 0; i < sizeof patterns / sizeof patterns[0]; ++i) {
        fresh_machine();
        char tail[160];
        snprintf(tail, sizeof tail, " %s\\%s", directory, patterns[i]);
        exec_file("H:\\VZ.COM", tail);
        if (i & 1) {
            CHECK(!hle_redirect && !registered && !host_runs && cpu.cf && cpu.a.x == 3);
            CHECK(!strcmp(output, "File path too long for VZ\r\n") && renders == 1);
        } else {
            CHECK(registered == &image_vz && !host_runs);
            child_tail(tail); /* A 63-byte full pattern still fits. */
            end_child();
        }
    }
    enter_short_directory(50);
    fresh_machine();
    current_short_directory(directory);
    char expected[160];
    snprintf(expected, sizeof expected, " %s\\ABCDEFG?.TXT", directory);
    exec_file("H:\\VZ.COM", " ABCDEFG?.TXT");
    CHECK(registered == &image_vz && !host_runs);
    child_tail(expected);
    end_child();
    fresh_machine();
    exec_file("H:\\VZ.COM", " *.TXT *.BAS *.ASM");
    CHECK(!hle_redirect && !registered && !host_runs && cpu.cf && cpu.a.x == 3);
    CHECK(!strcmp(output, "Command line too long for VZ\r\n") && renders == 1);
    CHECK(chdir(fixture) == 0);
}

static const char *child_environment(const char *name) {
    const char *at = (const char *)(mem + lin(rd16(cpu.ds, 0x2c), 0));
    size_t n = strlen(name);
    for (; *at; at += strlen(at) + 1)
        if (!strncmp(at, name, n) && at[n] == '=') return at + n + 1;
    return NULL;
}

static void test_vz_temp_environment(void) {
    const char *names[] = {"VZ.COM", "BASIC.EXE"};
    for (unsigned i = 0; i < 2; ++i) {
        fresh_machine();
        path_env("H:\\"); /* Reserve a roomy environment for this fixture. */
        static const char environment[] =
            "PATH=H:\\\0TMP=C:\\a very long temporary directory\\nested\0"
            "TEMP=C:\\another very long temporary directory\0CUSTOM=preserved\0";
        memcpy(mem + lin(rd16(parent_psp, 0x2c), 0), environment, sizeof environment);
        exec_file(names[i], "");
        CHECK(hle_redirect && !host_runs);
        char private_temp[4096] = "";
        if (i) {
            CHECK(!strcmp(child_environment("TMP"), "C:\\a very long temporary directory\\nested"));
            CHECK(!strcmp(child_environment("TEMP"), "C:\\another very long temporary directory"));
        } else {
            const char *temporary = child_environment("TMP");
            CHECK(temporary && !strncmp(temporary, "C:\\tmp\\", 7));
            CHECK(strlen(temporary) + sizeof "\\VZTEMP.$$$" <= 32);
            CHECK(!strcmp(temporary, child_environment("TEMP")));
            guest_string(0x3200, temporary);
            CHECK(!dos_fs_to_host(parent_psp, 0x3200, private_temp, sizeof private_temp));
            struct stat st;
            CHECK(stat(private_temp, &st) == 0 && S_ISDIR(st.st_mode));
            CHECK((st.st_mode & 0777) == 0700);
            char swap[4120];
            snprintf(swap, sizeof swap, "%s/VZTEMP.$$$", private_temp);
            host_file(swap, (const uint8_t *)"private swap", 12);
        }
        CHECK(!strcmp(child_environment("PATH"), "H:\\"));
        CHECK(!strcmp(child_environment("CUSTOM"), "preserved"));
        end_child();
        if (!i) CHECK(access(private_temp, F_OK) != 0 && errno == ENOENT);
    }
}

static void test_door_vz_quota_cleanup(void) {
    char root[256];
    snprintf(root, sizeof root, "%s/doorquota", fixture);
    CHECK(mkdir(root, 0700) == 0);
    CHECK(chdir(root) == 0);
    host_file("VZ.COM", vz_file, sizeof vz_file);
    /* The quota charges whole pages: VZ.COM's own pages plus exactly one. */
    long system_page = sysconf(_SC_PAGESIZE);
    const uint32_t page = system_page > 4096 ? (uint32_t)system_page : 4096;
    const uint32_t quota = ((uint32_t)sizeof vz_file + page - 1) / page * page + page;
    for (unsigned fault = 0; fault < 2; ++fault) {
        dos_core_set_door(0);
        fresh_machine();
        CHECK(dos_fs_init_door(root, quota) == 0);
        dos_core_set_door(1);
        path_env("H:\\");
        exec_file("H:\\VZ.COM", "");
        CHECK(registered == &image_vz && !host_runs);
        const char *temporary = child_environment("TMP");
        CHECK(temporary && !strncmp(temporary, "H:\\v", 4));
        char dos_swap[64], host_temp[4096];
        snprintf(dos_swap, sizeof dos_swap, "%s\\VZTEMP.$$$", temporary);
        guest_string(0x3200, temporary);
        CHECK(!dos_fs_to_host(parent_psp, 0x3200, host_temp, sizeof host_temp));
        guest_string(0x3300, dos_swap);
        Cpu child = cpu;
        cpu.a.x = 0x3c00; cpu.ds = parent_psp; cpu.d.x = 0x3300; cpu.c.x = 0;
        CHECK(dos_fs_int21() && !cpu.cf);
        uint16_t handle = cpu.a.x;
        cpu.a.x = 0x4200; cpu.b.x = handle; cpu.c.x = 0;
        cpu.d.x = (uint16_t)(page - 1); /* one byte fills the last free page */
        CHECK(dos_fs_int21() && !cpu.cf);
        cpu.a.x = 0x4000; cpu.b.x = handle; cpu.c.x = 1; cpu.d.x = 0x3300;
        CHECK(dos_fs_int21() && !cpu.cf && cpu.a.x == 1);
        if (!fault) {
            cpu.a.x = 0x3e00; cpu.b.x = handle;
            CHECK(dos_fs_int21() && !cpu.cf);
        } else {
            /* A crashed VZ has not closed its swap handle or a duplicate. */
            cpu.a.x = 0x4500; cpu.b.x = handle;
            CHECK(dos_fs_int21() && !cpu.cf);
        }
        /* Runtime-owned cleanup must also remove a read-only abandoned swap. */
        cpu.a.x = 0x4301; cpu.d.x = 0x3300; cpu.c.x = 1;
        CHECK(dos_fs_int21() && !cpu.cf);
        cpu = child;
        if (fault) {
            CHECK(dos_abort_untranslated());
            CHECK(hle_redirect && cpu.cs == parent_psp && cpu.ip == 0x2345);
        } else end_child();
        CHECK(access(host_temp, F_OK) != 0 && errno == ENOENT);
        guest_string(0x3400, "H:\\AFTER.DAT");
        cpu.a.x = 0x3c00; cpu.ds = parent_psp; cpu.d.x = 0x3400; cpu.c.x = 0;
        CHECK(dos_fs_int21() && !cpu.cf);
        handle = cpu.a.x;
        cpu.a.x = 0x4000; cpu.b.x = handle; cpu.c.x = 1; cpu.d.x = 0x3400;
        CHECK(dos_fs_int21() && !cpu.cf && cpu.a.x == 1);
        cpu.a.x = 0x3e00; cpu.b.x = handle;
        CHECK(dos_fs_int21() && !cpu.cf);
        cpu.a.x = 0x4100; cpu.d.x = 0x3400;
        CHECK(dos_fs_int21() && !cpu.cf);
    }
    dos_core_set_door(0);
    dos_fs_init();
    CHECK(unlink("VZ.COM") == 0);
    CHECK(chdir(fixture) == 0);
    CHECK(rmdir(root) == 0);
}

static int door_try_entry(const char *path) {
    Cpu saved = cpu;
    guest_string(0x3400, path);
    cpu.a.x = 0x3c00; cpu.ds = parent_psp; cpu.d.x = 0x3400; cpu.c.x = 0;
    CHECK(dos_fs_int21());
    int error = cpu.cf ? cpu.a.x : 0;
    if (!error) {
        cpu.b.x = cpu.a.x; cpu.a.x = 0x3e00;
        CHECK(dos_fs_int21() && !cpu.cf);
    }
    cpu = saved;
    return error;
}

static void door_delete_entry(const char *path) {
    Cpu saved = cpu;
    guest_string(0x3400, path);
    cpu.a.x = 0x4100; cpu.ds = parent_psp; cpu.d.x = 0x3400;
    CHECK(dos_fs_int21() && !cpu.cf);
    cpu = saved;
}

static void door_vz_entry_case(int full) {
    char root[256], file[64];
    snprintf(root, sizeof root, "%s/doorentries", fixture);
    CHECK(mkdir(root, 0700) == 0 && chdir(root) == 0);
    host_file("VZ.COM", vz_file, sizeof vz_file);
    CHECK(mkdir("ENTRIES", 0700) == 0);
    const unsigned seeded = DOS_DOOR_ENTRY_LIMIT - (full ? 2 : 3);
    for (unsigned i = 0; i < seeded; ++i) {
        snprintf(file, sizeof file, "ENTRIES/F%04u", i);
        host_file(file, (const uint8_t *)"", 0);
    }
    dos_core_set_door(0);
    fresh_machine();
    CHECK(dos_fs_init_door(root, UINT64_C(8) * 1024 * 1024) == 0);
    dos_core_set_door(1);
    path_env("H:\\");
    int bypass = 0;
    if (full) {
        /* VZ needs one new private directory even before it creates a swap
         * file. This EXEC must fail with disk full at exactly 4096 entries. */
        exec_file("H:\\VZ.COM", "");
        bypass = hle_redirect || registered || !cpu.cf || cpu.a.x != 39;
        if (hle_redirect && registered == &image_vz) end_child();
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            hle_redirect = 0; registered = NULL;
            exec_file("H:\\VZ.COM", "");
            CHECK(registered == &image_vz && hle_redirect && !host_runs);
            const char *temporary = child_environment("TMP");
            CHECK(temporary && !strncmp(temporary, "H:\\v", 4));
            char host_temp[4096];
            guest_string(0x3200, temporary);
            CHECK(!dos_fs_to_host(parent_psp, 0x3200, host_temp, sizeof host_temp));
            struct stat st;
            CHECK(!stat(host_temp, &st) && (st.st_mode & 0777) == 0700);
            int error = door_try_entry("H:\\DURING");
            if (error != 39) ++bypass;
            if (!error) door_delete_entry("H:\\DURING");
            end_child();
            CHECK(access(host_temp, F_OK) < 0 && errno == ENOENT);
            CHECK(door_try_entry("H:\\AFTER") == 0);
            error = door_try_entry("H:\\ONE_MORE");
            if (error != 39) ++bypass;
            if (!error) door_delete_entry("H:\\ONE_MORE");
            door_delete_entry("H:\\AFTER");
        }
    }
    dos_core_set_door(0);
    dos_fs_init();
    for (unsigned i = 0; i < seeded; ++i) {
        snprintf(file, sizeof file, "ENTRIES/F%04u", i);
        CHECK(unlink(file) == 0);
    }
    CHECK(rmdir("ENTRIES") == 0 && unlink("VZ.COM") == 0);
    CHECK(chdir(fixture) == 0 && rmdir(root) == 0);
    printf("door VZ entry quota: %s: %s\n", full ? "full-cap EXEC" : "repeated EXEC/terminate accounting",
           bypass ? "FAILED" : "passed");
    CHECK(!bypass);
}

static void test_door_vz_entry_cap(void) { door_vz_entry_case(1); }
static void test_door_vz_entry_reclaim(void) { door_vz_entry_case(0); }

static unsigned open_descriptor_count(void) {
    DIR *dir = opendir("/proc/self/fd");
    CHECK(dir != NULL);
    unsigned n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') ++n;
    CHECK(closedir(dir) == 0);
    return n;
}

static void test_vz_process_capture_failure(void) {
    /* Rogue's parent-image snapshot argument and VZ's pre-capture path
     * leases must survive the merge together, including the failure path. */
    CHECK(chdir("BIN1") == 0);
    fresh_machine();
    unsigned before = open_descriptor_count();
    capture_fails = 1;
    exec_file("H:\\VZ.COM", "");
    CHECK(captured_parent == &image_vc_com);
    CHECK(!hle_redirect && !registered && cpu.cf && cpu.a.x == 8 && !host_runs);
    CHECK(open_descriptor_count() == before);
    fresh_machine();
    exec_file("H:\\VZ.COM", "");
    CHECK(captured_parent == &image_vc_com);
    CHECK(registered == &image_vz && !host_runs);
    end_child();
    CHECK(open_descriptor_count() == before);
    CHECK(chdir(fixture) == 0);
}

static void vz_exit_cleanup(int check_temp) {
    CHECK(chdir("BIN1") == 0);
    /* Test both DOS resident services before normal exits and a fault. */
    for (unsigned how = 0; how < 6; ++how) {
        fresh_machine();
        unsigned before = open_descriptor_count();
        exec_file("H:\\VZ.COM", " -z");
        CHECK(registered == &image_vz && !host_runs);
        CHECK(open_descriptor_count() > before);
        guest_string(0x3200, child_environment("TMP"));
        char temporary[4096], swap[4120], files[4120];
        CHECK(!dos_fs_to_host(parent_psp, 0x3200, temporary, sizeof temporary));
        snprintf(swap, sizeof swap, "%s/VZTEMP.$$$", temporary);
        snprintf(files, sizeof files, "%s/files.$$$", temporary);
        host_file(swap, (const uint8_t *)"swap", 4);
        host_file(files, (const uint8_t *)"files", 5);
        hle_redirect = 0;
        switch (how) {
        case 0:
            cpu.a.x = 0x3100; cpu.d.x = 0x100;
            CHECK(dos_core_int21());
            break;
        case 1:
            cpu.d.x = 0x1000;
            dos_int_other(0x27);
            break;
        case 2: cpu.a.x = 0x4c00; CHECK(dos_core_int21()); break;
        case 3: cpu.a.x = 0; CHECK(dos_core_int21()); break;
        case 4: dos_int_other(0x20); break;
        case 5: CHECK(dos_abort_untranslated()); break;
        }
        CHECK(hle_redirect && cpu.cs == parent_psp && cpu.ip == 0x2345);
        CHECK(cpu.sp == 0xE000 && cpu.ifl && !cpu.cf);
        cpu.a.h = 0x4d;
        CHECK(dos_core_int21());
        CHECK(cpu.a.x == (how < 2 ? 0x300 : how == 5 ? 70 : 0));
        int removed = access(temporary, F_OK) != 0 && errno == ENOENT;
        /* Leave no test-created /tmp/vXXXXXX behind on a deliberate red run. */
        if (!removed) {
            CHECK(unlink(swap) == 0 && unlink(files) == 0);
            CHECK(rmdir(temporary) == 0);
        }
        if (check_temp) CHECK(removed);
        else CHECK(open_descriptor_count() == before);
    }
    CHECK(chdir(fixture) == 0);
}

static void test_vz_resident_temp(void) { vz_exit_cleanup(1); }
static void test_vz_resident_leases(void) { vz_exit_cleanup(0); }

static void test_vz_executable_path_is_short(void) {
    CHECK(mkdir("editor cfg+,", 0700) == 0);
    host_file("editor cfg+,/VZ.COM", vz_file, sizeof vz_file);
    fresh_machine();
    exec_file("H:\\editor cfg+,\\VZ.COM", "");
    CHECK(registered == &image_vz && !host_runs);
    const char *program = (const char *)(mem + lin(rd16(cpu.ds, 0x2c), 0));
    while (*program) program += strlen(program) + 1;
    program += 3; /* empty string, count=1, program's DOS name */
    CHECK(!strpbrk(program, " ,+") && !strncmp(program, "H:\\", 3));
    guest_string(0x3200, program);
    char actual[4096], expected[4096];
    CHECK(!dos_fs_to_host(parent_psp, 0x3200, actual, sizeof actual));
    snprintf(expected, sizeof expected, "%s/editor cfg+,/VZ.COM", fixture);
    CHECK(!strcmp(actual, expected));
    end_child();
}

static void test_vz_child_keeps_directory_paths(void) {
    CHECK(mkdir("configparent-a", 0700) == 0 && mkdir("configparent-b", 0700) == 0);
    CHECK(mkdir("currentparent-a", 0700) == 0 && mkdir("currentparent-b", 0700) == 0);
    host_file("configparent-b/VZ.COM", vz_file, sizeof vz_file);
    host_file("configparent-b/VZ.DEF", (const uint8_t *)"definition", 10);
    CHECK(chdir("currentparent-b") == 0);
    fresh_machine();
    exec_file("H:\\configparent-b\\VZ.COM", " NEW.TXT");
    CHECK(registered == &image_vz && !host_runs);
    const char *program = (const char *)(mem + lin(rd16(cpu.ds, 0x2c), 0));
    while (*program) program += strlen(program) + 1;
    program += 3;
    char definition[128], saved_name[128], actual[4096], expected[4096];
    strcpy(definition, program);
    strcpy(strrchr(definition, '\\') + 1, "VZ.DEF");
    guest_string(0x3200, ".");
    cpu.ds = cpu.es = parent_psp;
    cpu.si = 0x3200; cpu.di = 0x3300; cpu.a.x = 0x6000;
    CHECK(dos_fs_int21() && !cpu.cf);
    strcpy(saved_name, (const char *)(mem + lin(parent_psp, 0x3300)));
    strcat(saved_name, "\\NEW.TXT");
    CHECK(rmdir("../configparent-a") == 0 && rmdir("../currentparent-a") == 0);
    guest_string(0x3200, definition);
    CHECK(!dos_fs_to_host(parent_psp, 0x3200, actual, sizeof actual));
    snprintf(expected, sizeof expected, "%s/configparent-b/VZ.DEF", fixture);
    CHECK(!strcmp(actual, expected));
    guest_string(0x3200, saved_name);
    cpu.a.x = 0x3c00; cpu.c.x = 0; cpu.d.x = 0x3200;
    CHECK(dos_fs_int21() && !cpu.cf);
    cpu.b.x = cpu.a.x; cpu.a.h = 0x3e;
    CHECK(dos_fs_int21() && !cpu.cf);
    CHECK(access("NEW.TXT", F_OK) == 0);
    end_child();
    CHECK(chdir(fixture) == 0);
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

static void test_kermit_association(void) {
    const char *names[] = {"safe;touch PWNED;.tak", "safe,run touch PWNED,.tak"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
      host_file(names[i], (const uint8_t *)"exit\r\n", 6);
      for (int quick = 0; quick < 2; quick++) {
        char line[128], expected[128];
        snprintf(line, sizeof line, "kermit stay, take %s", names[i]);
        host_file("KERMIT.EXE", kermit_file, sizeof kermit_file);
        fresh_machine();
        guest_string(0x3100, names[i]);
        cpu.a.x = 0x6000; cpu.si = 0x3100; cpu.di = 0x3200;
        CHECK(dos_fs_int21() && !cpu.cf);
        snprintf(expected, sizeof expected, " stay, take %s", (const char *)(mem + lin(parent_psp, 0x3200)));
        CHECK(!strchr(expected + 12, ',') && !strchr(expected, ';'));
        command(line, quick);
        CHECK(registered == &image_kermit && !host_runs);
        child_tail(expected);
        end_child();
        CHECK(unlink("KERMIT.EXE") == 0);
        fresh_machine();
        command(line, quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(strstr(output, "Program not found"));
        CHECK(access("PWNED", F_OK) != 0);
        /* A different supported DOS program cannot receive the TAKE tail. */
        host_file("KERMIT.EXE", vc_file, sizeof vc_file);
        fresh_machine();
        command(line, quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(strstr(output, "Invalid program format"));
        CHECK(access("PWNED", F_OK) != 0);
      }
    }
    /* Some legal DOS short-name punctuation is Kermit syntax, not a
     * filename. Refuse it without launching an interpreter or a shell. */
    host_file("x{y}.tak", (const uint8_t *)"exit\r\n", 6);
    host_file("KERMIT.EXE", kermit_file, sizeof kermit_file);
    for (int quick = 0; quick < 2; quick++) {
        fresh_machine();
        command("kermit stay, take x{y}.tak", quick);
        CHECK(!hle_redirect && !registered && !host_runs);
        CHECK(strstr(output, "Invalid program format"));
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

static void test_resident_fcb_lifetime(void) {
    host_file("RESIDENT.DAT", (const uint8_t *)"resident", 8);
    for (unsigned how = 0; how < 2; ++how) {
        fresh_machine();
        exec_file("BASIC.EXE", "");
        CHECK(registered == &image_gwbasic && !host_runs);
        uint16_t child = cpu.ds;
        open_fcb(child, 0x180, "RESIDENT.DAT");
        memcpy(mem + lin(parent_psp, 0x3500), mem + lin(child, 0x180), 37);
        if (how) {
            cpu.d.x = 0x1000;
            dos_int_other(0x27);
        } else {
            cpu.a.x = 0x3100; cpu.d.x = 0x100;
            CHECK(dos_core_int21());
        }
        CHECK(hle_redirect && cpu.cs == parent_psp && cpu.ip == 0x2345);
        /* Releasing host-side path leases must not close a generic TSR's
         * DOS FCB. Simulate its later callback with its PSP current. */
        cpu.a.h = 0x50; cpu.b.x = child;
        CHECK(dos_core_int21());
        cpu.ds = parent_psp; cpu.d.x = 0x3500; cpu.a.h = 0x10;
        CHECK(dos_fs_int21() && cpu.a.l == 0);
        cpu.a.h = 0x50; cpu.b.x = parent_psp;
        CHECK(dos_core_int21());
    }
}

static int remove_fixture(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st; (void)walk;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

int main(int argc, char **argv) {
    CHECK(getcwd(original_cwd, sizeof original_cwd));
    strcpy(fixture, "/tmp/vc-exec-XXXXXX");
    CHECK(mkdtemp(fixture));
    CHECK(setenv("HOME", fixture, 1) == 0);
    CHECK(chdir(fixture) == 0);
    CHECK(mkdir("BIN1", 0700) == 0 && mkdir("BIN2", 0700) == 0);
    host_file("VC.COM", vc_file, sizeof vc_file);
    host_file("VC.OVL", ovl_file, sizeof ovl_file);
    host_file("VZ.COM", vz_file, sizeof vz_file);
    host_file("BASIC.EXE", basic_file, sizeof basic_file);
    const struct { const char *name; void (*run)(void); } suites[] = {
        {"identity", test_exec_identity},
        {"logo", test_logo_identity},
        {"rogue-identity", test_rogue_identity},
        {"vz-identity", test_vz_identity_and_edit},
        {"vz-file-limit", test_vz_path_buffer_limit},
        {"vz-cwd-boundary", test_vz_cwd_boundary},
        {"vz-command-paths", test_vz_command_file_paths},
        {"vz-command-limits", test_vz_command_file_limits},
        {"vz-wildcard-paths", test_vz_wildcard_paths},
        {"vz-temp", test_vz_temp_environment},
        {"door-vz-quota", test_door_vz_quota_cleanup},
        {"door-vz-entry-cap", test_door_vz_entry_cap},
        {"door-vz-entry-reclaim", test_door_vz_entry_reclaim},
        {"vz-capture-failure", test_vz_process_capture_failure},
        {"vz-resident-temp", test_vz_resident_temp},
        {"vz-resident-leases", test_vz_resident_leases},
        {"vz-executable", test_vz_executable_path_is_short},
        {"vz-directories", test_vz_child_keeps_directory_paths},
        {"search", test_search},
        {"association", test_association},
        {"kermit-association", test_kermit_association},
        {"psp", test_psp_services},
        {"fcb", test_child_fcb_lifetime},
        {"resident-fcb", test_resident_fcb_lifetime},
    };
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof suites / sizeof suites[0]; ++i)
        if (argc == 1 || (argc == 2 && !strcmp(argv[1], suites[i].name))) {
            suites[i].run(); ++ran;
        }
    CHECK(ran > 0);
    CHECK(chdir(original_cwd) == 0);
    CHECK(nftw(fixture, remove_fixture, 16, FTW_DEPTH | FTW_PHYS) == 0);
    printf("DOS EXEC: %u checks passed\n", checks);
    return 0;
}
