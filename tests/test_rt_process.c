/* Process-boundary tests with the real dispatcher, DOS kernel and BIOS.
 * Only translated instruction bodies, the terminal and the clock are tiny
 * fixtures. All EXECs and exits go through the guest interrupt interface. */
#define _POSIX_C_SOURCE 200809L
#include "bios.h"
#include "hle.h"
#include "rt.h"
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

enum {
    CHILD_FAULT, IRQ_FAULT, ROOT_FAULT, OVERLAY_FAULT,
    CHILD_BREAK, DEFAULT_BREAK, BLOCKED_BREAK, HOOKED_BREAK, ROOT_BREAK,
};
static int scenario;
static unsigned child_runs, child_irqs, parent_ticks, parent_breaks, steps, renders;
static unsigned child_breaks;
static int child_busy, break_sent;
static uint64_t last_input_poll;
static uint64_t now_ns = 1000000000ull;
static uint16_t parent_psp, child_psp, child_env, child_allocation;
static Cpu saved_parent;
static uint16_t saved_flags;
static uint8_t parent_vectors[1024];
static char fixture_dir[] = "/tmp/vc-process-XXXXXX";

static int parent_run(uint32_t off, uint16_t loadseg);
static int basic_run(uint32_t off, uint16_t loadseg);
static int overlay_run(uint32_t off, uint16_t loadseg);
/* Distinct bytes prevent the synthetic images from matching one another.
 * Their MZ headers identify files; Image supplies their load metadata. */
static const uint8_t vc_file[256] = {[0 ... 255] = 0xa5};
static const uint8_t ovl_file[272] = {'M', 'Z', 1, [16 ... 271] = 0xb6};
static const uint8_t basic_file[272] = {'M', 'Z', 2, [16 ... 271] = 0xc7};
const Image image_vc_com = {
    .name = "VC.COM", .bytes = vc_file, .size = sizeof vc_file, .run = parent_run,
};
const Image image_vc_ovl = {
    .name = "VC.OVL", .is_exe = 1, .bytes = ovl_file + 16, .size = 256,
    .hdr_sp = 0x800, .min_alloc = 0x100, .max_alloc = 0x100, .run = overlay_run,
};
const Image image_gwbasic = {
    .name = "GWBASIC.EXE", .is_exe = 1, .bytes = basic_file + 16, .size = 256,
    .hdr_sp = 0x800, .min_alloc = 0x100, .max_alloc = 0x100, .run = basic_run,
};
const Image image_bootlogo = {.name = "LOGO.COM"}; /* Not executed by this fixture. */
const Image image_rogue = {.name = "ROGUE.EXE"}; /* Not executed by this fixture. */
const Image image_vz = {.name = "VZ.COM"}; /* Not executed by this fixture. */
const Image image_kermit = {.name = "KERMIT.EXE"}; /* Not executed by this fixture. */
const EmbeddedFile embedded_files[] = {
    {"VC.COM", vc_file, sizeof vc_file}, {"VC.OVL", ovl_file, sizeof ovl_file},
    {"GWBASIC.EXE", basic_file, sizeof basic_file},
};
const int embedded_file_count = sizeof embedded_files / sizeof embedded_files[0];

int clock_gettime(clockid_t clock, struct timespec *time) {
    uint64_t ns = now_ns;
    if (clock == CLOCK_REALTIME) ns += 1700000000ull * 1000000000ull;
    time->tv_sec = (time_t)(ns / 1000000000ull);
    time->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

void term_idle(int milliseconds) {
    if (milliseconds > 0) now_ns += (uint64_t)milliseconds * 1000000ull;
    if (child_busy) {
        CHECK(now_ns - last_input_poll <= 50000000ull);
        last_input_poll = now_ns;
        if (!break_sent) {
            break_sent = 1;
            bios_request_break();
        }
    }
}
void term_shutdown(void) {}
void term_suspend(void) { CHECK(0); } /* Nothing here may invoke a host shell. */
void term_resume(void) {}
void term_render(void) { ++renders; }
void term_invalidate(void) {}
void term_bell(void) {}
void term_clear_pending(void) {}
void term_flush_input(void) {}

static void vector(unsigned number, uint16_t segment, uint16_t offset) {
    wr16(0, (uint16_t)(number * 4), offset);
    wr16(0, (uint16_t)(number * 4 + 2), segment);
}

static void divisor(unsigned channel, uint16_t value) {
    port_out8(0x43, (uint8_t)((channel << 6) | 0x36));
    port_out8((uint16_t)(0x40 + channel), (uint8_t)value);
    port_out8((uint16_t)(0x40 + channel), (uint8_t)(value >> 8));
}

static uint16_t read_divisor(unsigned channel) {
    uint8_t low = port_in8((uint16_t)(0x40 + channel));
    return (uint16_t)(low | ((uint16_t)port_in8((uint16_t)(0x40 + channel)) << 8));
}

static void no_code(void) {
    cpu.cs = cpu.ip = 0;
}

static void exec_child(const char *name, uint16_t return_ip) {
    memcpy(mem + lin(parent_psp, 0x1000), name, strlen(name) + 1);
    wr16(parent_psp, 0x1100, 0);
    wr16(parent_psp, 0x1102, 0x1200);
    wr16(parent_psp, 0x1104, parent_psp);
    wr8(parent_psp, 0x1200, 0);
    wr8(parent_psp, 0x1201, '\r');
    cpu.ds = cpu.es = parent_psp;
    cpu.a.x = 0x4b00;
    cpu.b.x = 0x1100;
    cpu.d.x = 0x1000;
    cpu.c.x = 0x6464;
    cpu.si = 0x3131;
    cpu.di = 0x4242;
    cpu.bp = 0x5353;
    flags_set(0x0ed6);
    saved_parent = cpu;
    saved_flags = flags_get();
    cpu_int(0x21, return_ip);
}

static void check_parent(void) {
    CHECK(cpu.cs == parent_psp && cpu.ds == parent_psp && cpu.es == parent_psp);
    CHECK(cpu.ss == saved_parent.ss && cpu.sp == saved_parent.sp);
    CHECK(cpu.a.x == saved_parent.a.x && cpu.b.x == saved_parent.b.x);
    CHECK(cpu.c.x == saved_parent.c.x && cpu.d.x == saved_parent.d.x);
    CHECK(cpu.si == saved_parent.si && cpu.di == saved_parent.di && cpu.bp == saved_parent.bp);
    CHECK(flags_get() == saved_flags && !cpu.cf && !rt_halted);
}

static void check_recovery(void) {
    char screen[2001];
    for (unsigned i = 0; i < 2000; ++i) screen[i] = mem[0xb8000 + i * 2];
    screen[2000] = 0;
    check_parent();
    CHECK(!memcmp(mem, parent_vectors, sizeof parent_vectors));
    CHECK(read_divisor(0) == 0x1234 && read_divisor(2) == 0x4321);
    CHECK(port_in8(0x21) == 0xa4 && port_in8(0x61) == 0x14);
    CHECK(rt_speaker_hz() == 0 && !bios_take_break());
    CHECK(parent_ticks == 0 && parent_breaks == 0); /* No replay of child events. */
    CHECK(child_runs == 1 && child_irqs == (unsigned)(scenario == IRQ_FAULT));
    CHECK(child_breaks == (unsigned)(scenario == HOOKED_BREAK));
    CHECK(rd16((uint16_t)(child_psp - 1), 1) == 0);
    CHECK(rd16((uint16_t)(child_env - 1), 1) == 0);
    CHECK(rd16((uint16_t)(child_allocation - 1), 1) == 0);
    CHECK(renders > 0);
    if (scenario == CHILD_BREAK || scenario == DEFAULT_BREAK || scenario == BLOCKED_BREAK) {
        CHECK(break_sent);
        CHECK(rd16(0x40, 0x1a) == rd16(0x40, 0x1c)); /* No synthetic NUL. */
        CHECK(strstr(screen, "Ctrl-Break."));
    } else {
        CHECK(strstr(screen, "No translated code at 0000:0000."));
    }
    CHECK(strstr(screen, "GWBASIC.EXE stopped."));
}

static int parent_run(uint32_t off, uint16_t loadseg) {
    if (off != 0xc0 && off != 0xc8 && (off > 80 || off % 8)) return -1;
    CHECK(++steps < 1000);
    CHECK(loadseg == cpu.cs + 0x10);
    if (off == 0xc0 || off == 0xc8) {
        if (off == 0xc0) ++parent_ticks;
        else ++parent_breaks;
        CHECK(parent_ticks < 20 && !cpu.ifl);
        cpu.ip = pop16();
        cpu.cs = pop16();
        flags_set(pop16());
        return 0;
    }
    switch (off) {
    case 0:
        parent_psp = cpu.ds;
        if (scenario == ROOT_FAULT) { no_code(); break; }
        if (scenario == ROOT_BREAK) {
            vector(0x1b, parent_psp, 0x1c8);
            bios_request_break();
            cpu.ip = 0x108;
            break;
        }
        cpu.es = parent_psp;
        cpu.a.x = 0x4a00;
        cpu.b.x = 0x1000;
        cpu.sp = 0xf000;
        cpu_int(0x21, 0x108);
        break;
    case 8:
        if (scenario == ROOT_BREAK) {
            CHECK(parent_breaks == 1 && !bios_take_break());
            CHECK(rd16(0x40, 0x1a) == rd16(0x40, 0x1c));
            rt_exited = 1;
            break;
        }
        CHECK(!cpu.cf);
        cpu.ds = parent_psp;
        cpu.d.x = 0x2000;
        cpu.a.x = 0x1a00;
        cpu_int(0x21, 0x110);
        break;
    case 16:
        divisor(0, 0x1234); /* Nondefault parent hardware must be restored. */
        divisor(2, 0x4321);
        port_out8(0x21, 0xa4);
        port_out8(0x61, 0x14);
        vector(0x1b, parent_psp, 0x1c8);
        vector(0x1c, parent_psp, 0x1c0);
        memcpy(parent_vectors, mem, sizeof parent_vectors);
        exec_child(scenario == OVERLAY_FAULT ? "VC.OVL" : "GWBASIC.EXE", 0x118);
        break;
    case 24:
        child_busy = 0;
        check_recovery();
        cpu.a.x = 0x4d00;
        cpu_int(0x21, 0x120);
        break;
    case 32:
        CHECK(cpu.a.x != 0); /* Abnormal child exit is visible to its parent. */
        if (scenario == CHILD_BREAK || scenario == DEFAULT_BREAK || scenario == BLOCKED_BREAK)
            CHECK(cpu.a.h == 1); /* DOS termination type: Ctrl-Break. */
        cpu.a.x = 0x2f00;
        cpu_int(0x21, 0x128);
        break;
    case 40:
        CHECK(cpu.es == parent_psp && cpu.b.x == 0x2000);
        cpu.a.x = 0x5100;
        cpu_int(0x21, 0x130);
        break;
    case 48:
        CHECK(cpu.b.x == parent_psp);
        cpu.ds = parent_psp;
        cpu.d.x = 0x3200;
        cpu.a.x = 0x1000;
        cpu_int(0x21, 0x138);
        break;
    case 56:
        CHECK(cpu.a.l == 0xff); /* The aborted child's copied FCB is stale. */
        now_ns += 20000000ull;
        cpu.ip = 0x140;
        break;
    case 64:
        CHECK(parent_ticks > 0); /* PIC/IRQ-in-service cannot remain stuck. */
        exec_child("GWBASIC.EXE", 0x148);
        break;
    case 72:
        check_parent();
        CHECK(child_runs == 2);
        cpu.a.x = 0x4d00;
        cpu_int(0x21, 0x150);
        break;
    case 80:
        CHECK(cpu.a.x == 42);
        rt_exited = 1;
        break;
    }
    return 0;
}

static int basic_run(uint32_t off, uint16_t loadseg) {
    if (off != 0x80 && off != 0x88 && (off > 32 || off % 8)) return -1;
    CHECK(++steps < 1000 && cpu.cs == loadseg);
    if (off == 0x80) {
        CHECK(scenario == IRQ_FAULT && !cpu.ifl);
        ++child_irqs;
        no_code(); /* Deliberately abandon IRQ 0 without sending EOI. */
        return 0;
    }
    if (off == 0x88) {
        CHECK(scenario == HOOKED_BREAK && !cpu.ifl);
        ++child_breaks;
        cpu.ip = pop16();
        cpu.cs = pop16();
        flags_set(pop16());
        return 0;
    }
    switch (off) {
    case 0:
        if (++child_runs == 2) {
            CHECK(rt_speaker_hz() == 0);
            cpu.a.x = 0x4c2a;
            cpu_int(0x21, 8);
            break;
        }
        child_psp = cpu.ds;
        child_env = rd16(child_psp, 0x2c);
        cpu.a.x = 0x4800;
        cpu.b.x = 0x10;
        cpu_int(0x21, 8);
        break;
    case 8:
        CHECK(!cpu.cf);
        child_allocation = cpu.a.x;
        CHECK(rd16((uint16_t)(child_allocation - 1), 1) == child_psp);
        memset(mem + lin(child_psp, 0x200), 0, 37);
        memcpy(mem + lin(child_psp, 0x201), "CHILD   DAT", 11);
        cpu.d.x = 0x200;
        cpu.a.x = 0x0f00;
        cpu_int(0x21, 16);
        break;
    case 16:
        CHECK(cpu.a.l == 0);
        memcpy(mem + lin(parent_psp, 0x3200), mem + lin(child_psp, 0x200), 37);
        cpu.d.x = 0x300;
        cpu.a.x = 0x1a00;
        cpu_int(0x21, 24);
        break;
    case 24:
        vector(0x08, loadseg, 0x80);
        if (scenario == DEFAULT_BREAK)
            vector(0x1b, STUB_SEG, 0x1b);
        else if (scenario != CHILD_BREAK && scenario != BLOCKED_BREAK)
            vector(0x1b, loadseg, 0x88);
        vector(0x1c, loadseg, 0x90);
        divisor(0, 2983);
        divisor(2, 2700);
        port_out8(0x61, 3);
        CHECK(rt_speaker_hz() > 0);
        if (scenario >= CHILD_BREAK) {
            port_out8(0x21, 0xff);
            cpu.ifl = 1;
            child_busy = 1;
            last_input_poll = now_ns;
            cpu.ip = 0x20;
            if (scenario == BLOCKED_BREAK) {
                cpu.a.x = 0x0700; /* DOS single-byte input is interruptible too. */
                cpu_int(0x21, 0x20);
            }
        } else if (scenario == IRQ_FAULT) {
            port_out8(0x21, 0xfe);
            now_ns += 10000000ull;
            cpu.ip = 0x20; /* IRQ delivery must preempt this untranslated IP. */
        } else {
            port_out8(0x21, 0xff);
            cpu.ifl = 0;
            bios_request_break();
            now_ns += 1000000000ull;
            no_code();
        }
        break;
    case 32:
        CHECK(scenario != BLOCKED_BREAK); /* Break must never return a NUL. */
        CHECK(now_ns - last_input_poll <= 50000000ull);
        if (scenario == HOOKED_BREAK && child_breaks == 1) {
            CHECK(rd16(0x40, 0x1a) == rd16(0x40, 0x1c));
            child_busy = 0;
            no_code(); /* Recovery checks below still exercise the fault path. */
        } else {
            now_ns += 1000000ull; /* Tight translated work: no BIOS or DOS calls. */
        }
        break;
    }
    return 0;
}

static int overlay_run(uint32_t off, uint16_t loadseg) {
    (void)loadseg;
    if (off) return -1;
    CHECK(scenario == OVERLAY_FAULT);
    no_code();
    return 0;
}

static int run_case(const char *name, int which, int expected) {
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(3);
        scenario = which;
        CHECK(chdir(fixture_dir) == 0);
        CHECK(setenv("HOME", fixture_dir, 1) == 0);
        if (expected == 70) CHECK(freopen("/dev/null", "w", stderr));
        dos_core_init();
        dos_fs_init();
        bios_init();
        char executable[256];
        CHECK(snprintf(executable, sizeof executable, "%s/VC.COM", fixture_dir) > 0);
        dos_start(executable, (const uint8_t *)"", 0);
        rt_run();
        exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    int actual = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (actual != expected)
        fprintf(stderr, "FAIL process %s: exit %d (wait status %d), wanted %d\n",
                name, actual, status, expected);
    return actual != expected;
}

static void write_fixture(const char *name, const uint8_t *data, size_t size) {
    char path[256];
    CHECK(snprintf(path, sizeof path, "%s/%s", fixture_dir, name) > 0);
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(data, 1, size, file) == size);
    CHECK(fclose(file) == 0);
}

int main(void) {
    CHECK(setenv("VC_LOG", "/dev/null", 1) == 0);
    CHECK(mkdtemp(fixture_dir));
    for (int i = 0; i < embedded_file_count; ++i)
        write_fixture(embedded_files[i].name, embedded_files[i].data, embedded_files[i].size);
    write_fixture("CHILD.DAT", (const uint8_t *)"child", 5);
    int failed = 0;
    failed += run_case("untranslated child restores parent", CHILD_FAULT, 0);
    failed += run_case("untranslated child IRQ restores parent", IRQ_FAULT, 0);
    failed += run_case("root VC remains fatal", ROOT_FAULT, 70);
    failed += run_case("VC.OVL child remains fatal", OVERLAY_FAULT, 70);
    failed += run_case("Ctrl-Break stops child with inherited parent hook", CHILD_BREAK, 0);
    failed += run_case("Ctrl-Break stops child with default hook", DEFAULT_BREAK, 0);
    failed += run_case("Ctrl-Break interrupts a blocking DOS read", BLOCKED_BREAK, 0);
    failed += run_case("Ctrl-Break preserves the child's own hook", HOOKED_BREAK, 0);
    failed += run_case("Ctrl-Break leaves root VC running", ROOT_BREAK, 0);
    const char *names[] = {"VC.COM", "VC.OVL", "GWBASIC.EXE", "CHILD.DAT"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char path[256];
        CHECK(snprintf(path, sizeof path, "%s/%s", fixture_dir, names[i]) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(fixture_dir) == 0);
    printf("test_rt_process: 9 cases, %d failures\n", failed);
    return failed ? 1 : 0;
}
