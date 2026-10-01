/* Deterministic machine tests. A tiny Image.run fixture supplies instruction
 * boundaries, while the real dispatcher, CPU interrupt frames, BIOS, PIT and
 * speaker logic run unchanged. The host clock/terminal and unrelated DOS
 * services are replaced here, never in the production build. */
#define _POSIX_C_SOURCE 200809L
#include "bios.h"
#include "cpu.h"
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

static uint64_t now_ns = 1000000000ull, stop_ns;
static unsigned ticks, breaks, waits, steps;
static uint16_t original_flags;
static int scenario;
enum { MASKED_TIMER, RATE_TIMER, BREAK_IRQ, HALT_IRQ, MUTABLE_CODE, PRINTER };

int clock_gettime(clockid_t clock, struct timespec *time) {
    uint64_t ns = now_ns;
    if (clock == CLOCK_REALTIME) ns += 1700000000ull * 1000000000ull;
    time->tv_sec = (time_t)(ns / 1000000000ull);
    time->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

void term_idle(int milliseconds) {
    if (milliseconds > 0) {
        ++waits;
        now_ns += (uint64_t)milliseconds * 1000000ull;
    }
}
void term_shutdown(void) {}
void term_invalidate(void) {}
void term_bell(void) {}
void term_clear_pending(void) {}
void term_flush_input(void) {}
void dos_casemap_upper(void) {}
int dos_core_int21(void) { return 0; }
int dos_run_psp(void) { return 0; }
int dos_abort_untranslated(void) { return 0; }
int dos_fs_int21(void) { return 0; }
int dos_int_other(uint8_t number) { (void)number; return 1; }

static void iret(void) {
    cpu.ip = pop16();
    cpu.cs = pop16();
    flags_set(pop16());
}

static int fixture_run(uint32_t off, uint16_t loadseg) {
    CHECK(loadseg == 0x2000);
    CHECK(++steps < 5000);
    if (off == 16) {
        CHECK(!cpu.ifl && !cpu.tf);
        ++ticks;
        if (scenario == RATE_TIMER) {
            port_out8(0x20, 0x20); /* Guest IRQ 0 sends EOI before IRET. */
        } else {
            CHECK(cpu.sp == 0xeff4); /* IRQ 0 frame plus INT 1Ch frame. */
            CHECK(rd16(cpu.ss, cpu.sp) == STUB_INT8_RETURN);
            CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == STUB_SEG);
            CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 8)) == 0x2000);
            CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 10)) == original_flags);
        }
        flags_set(0); /* Both IRETs must restore all saved flags. */
        iret();
        return 0;
    }
    if (off == 32) {
        CHECK(scenario == BREAK_IRQ);
        CHECK(!cpu.ifl && !cpu.tf);
        CHECK(cpu.sp == 0xeffa);
        CHECK(rd16(cpu.ss, cpu.sp) == 2);
        CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == 0x2000);
        ++breaks;
        iret();
        return 0;
    }
    if (off > 2) return -1;
    switch (scenario) {
    case MASKED_TIMER:
        if (off == 0) {
            cpu.ifl = 0;
            now_ns += 1000000000ull;
            CHECK(ticks == 0); /* Time alone cannot recursively enter code. */
            cpu.ip = 1;
        } else if (off == 1) {
            CHECK(ticks == 0); /* A dispatch with IF=0 latches, not delivers. */
            cpu.ifl = 1;
            cpu.ip = 2;
        } else {
            CHECK(ticks == 1); /* Masked timer edges coalesce like the PIC. */
            CHECK(cpu.sp == 0xf000 && flags_get() == original_flags);
            rt_exited = 1;
        }
        break;
    case RATE_TIMER:
        if (now_ns >= stop_ns) rt_exited = 1;
        else now_ns += 1000000ull;
        break;
    case BREAK_IRQ:
        if (off == 0) {
            cpu.ifl = 0;
            bios_request_break();
            bios_request_break(); /* An already-pending request coalesces. */
            CHECK(breaks == 0 && cpu.cs == 0x2000 && cpu.ip == 0);
            cpu.ip = 1;
        } else if (off == 1) {
            CHECK(breaks == 0);
            cpu.ifl = 1;
            cpu.ip = 2;
        } else {
            CHECK(breaks == 1 && cpu.sp == 0xf000);
            CHECK(flags_get() == original_flags);
            CHECK(!bios_take_break());
            rt_exited = 1;
        }
        break;
    case HALT_IRQ:
        if (off == 0) {
            rt_halted = 1;
            rt_budget = -1;
            cpu.ip = 1;
        } else {
            CHECK(!rt_halted && ticks == 1 && waits > 0);
            CHECK(rt_budget == 20000 && cpu.sp == 0xf000);
            CHECK(now_ns >= 1054925401ull && now_ns < 1060000000ull);
            rt_exited = 1;
        }
        break;
    case MUTABLE_CODE:
        CHECK(off == 0 && rd16(cpu.cs, 1) == 0xbeef);
        rt_exited = 1;
        break;
    case PRINTER:
        if (off == 0) {
            cpu.a.x = 0x0200; /* Printer status request. */
            cpu_int(0x17, 1);
        } else {
            CHECK(cpu.a.h == 1 && cpu.sp == 0xf000 && cpu.ifl);
            rt_exited = 1;
        }
        break;
    }
    return 0;
}

static const uint8_t code[64] = {0xb8, 0, 0, 0x90, 0xfa, 0xfb};
static const uint32_t mutable_offsets[] = {1, 2};
static const Image fixture = {
    .name = "MACHINE.COM", .bytes = code, .size = sizeof code,
    .run = fixture_run, .mutable_offsets = mutable_offsets, .nmutable = 2,
};

static void setup(int which) {
    scenario = which;
    memset(&cpu, 0, sizeof cpu);
    memset(mem, 0, sizeof mem);
    for (unsigned n = 0; n < 256; ++n) {
        wr16(0, (uint16_t)(n * 4), (uint16_t)n);
        wr16(0, (uint16_t)(n * 4 + 2), STUB_SEG);
    }
    bios_init();
    memcpy(mem + 0x20000, code, sizeof code);
    rt_register_image(&fixture, 0x2000);
    cpu.cs = 0x2000;
    cpu.ss = 0x8000;
    cpu.sp = 0xf000;
    flags_set(0x0ed7);
    original_flags = flags_get();
    wr16(0, 0x1c * 4, 16);
    wr16(0, 0x1c * 4 + 2, 0x2000);
}

static void masked_timer(void) { setup(MASKED_TIMER); rt_run(); }
static void halt_irq(void) { setup(HALT_IRQ); rt_run(); }
static void printer_status(void) { setup(PRINTER); rt_run(); }
static void ctrl_break_irq(void) {
    setup(BREAK_IRQ);
    wr16(0, 0x1b * 4, 32);
    wr16(0, 0x1b * 4 + 2, 0x2000);
    rt_run();
}

static void timer_rate(unsigned divisor, unsigned expected) {
    setup(RATE_TIMER);
    wr16(0, 0x08 * 4, 16);
    wr16(0, 0x08 * 4 + 2, 0x2000);
    port_out8(0x43, 0x36);
    port_out8(0x40, (uint8_t)divisor);
    port_out8(0x40, (uint8_t)(divisor >> 8));
    stop_ns = now_ns + 1000000000ull;
    rt_run();
    CHECK(ticks == expected);
}
static void slow_timer(void) { timer_rate(0, 18); }
static void fast_timer(void) { timer_rate(0x0ba7, 399); }

static void patched_operand(void) {
    setup(MUTABLE_CODE);
    wr16(cpu.cs, 1, 0xbeef);
    rt_run();
}

static void patched_opcode_refused(void) {
    setup(MUTABLE_CODE);
    wr8(cpu.cs, 0, 0xcc);
    rt_run(); /* Must fail, not execute the unchanged translation. */
}

static void close_frequency(double wanted) {
    double difference = rt_speaker_hz() - wanted;
    if (difference < 0) difference = -difference;
    CHECK(difference < 0.0001);
}

static void speaker_ports(void) {
    /* The fork's default octave 4 divides NOTTAB's 4186/4699/5274 by 4
     * (GWSTS.ASM), so bare PLAY "CDE" is 1047/1175/1319, not middle C. */
    static const unsigned notes[] = {800, 440, 1047, 1175, 1319};
    close_frequency(0);
    for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; ++i) {
        unsigned divisor = (1193181u + notes[i] / 2) / notes[i];
        port_out8(0x43, 0xb6); /* Channel 2, low/high, square wave. */
        port_out8(0x42, (uint8_t)divisor);
        port_out8(0x42, (uint8_t)(divisor >> 8));
        port_out8(0x61, 0xff);
        CHECK(port_in8(0x61) == 0xff);
        close_frequency(1193182.0 / divisor);
        CHECK(port_in8(0x42) == (uint8_t)divisor);
        CHECK(port_in8(0x42) == (uint8_t)(divisor >> 8));
        port_out8(0x61, 0xfe); close_frequency(0); /* Gate off. */
        port_out8(0x61, 0xfd); close_frequency(0); /* Speaker data off. */
        port_out8(0x61, 0xfc); close_frequency(0);
    }
    port_out8(0x43, 0xb6);
    port_out8(0x42, 0);
    port_out8(0x42, 0);
    port_out8(0x61, 3);
    close_frequency(1193182.0 / 65536); /* Zero reload means 65536. */
    port_out8(0x42, 0x34);
    close_frequency(1193182.0 / 65536); /* No half-written frequency. */
    port_out8(0x42, 0x12);
    close_frequency(1193182.0 / 0x1234);
    port_out8(0x43, 0xb0); close_frequency(0); /* Not an oscillating mode. */
    port_out8(0x43, 0xbe); close_frequency(1193182.0 / 0x1234); /* 7 aliases 3. */
    port_out8(0x43, 0x96);
    port_out8(0x42, 7);
    close_frequency(1193182.0 / 7);
    port_out8(0x43, 0xa6);
    port_out8(0x42, 2);
    close_frequency(1193182.0 / 512);
    port_out8(0x61, 0); close_frequency(0);
}

typedef void (*Test)(void);
static int run_test(const char *name, Test test, int expected_status) {
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(3); /* A missing interrupt must fail, not hang the test gate. */
        if (expected_status == 70) CHECK(freopen("/dev/null", "w", stderr));
        test();
        exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    int good = WIFEXITED(status) && WEXITSTATUS(status) == expected_status;
    if (!good) fprintf(stderr, "FAIL machine %s: status %d, wanted %d\n",
                       name, status, expected_status);
    return !good;
}

int main(void) {
    CHECK(setenv("VC_LOG", "/dev/null", 1) == 0);
    int failed = 0;
    failed += run_test("masked INT8 -> INT1Ch and IRET", masked_timer, 0);
    failed += run_test("18.2 Hz default PIT", slow_timer, 0);
    failed += run_test("400 Hz reprogrammed PIT", fast_timer, 0);
    failed += run_test("IF-gated Ctrl-Break frame", ctrl_break_irq, 0);
    failed += run_test("HLT yields at dispatcher", halt_irq, 0);
    failed += run_test("speaker ports/frequencies", speaker_ports, 0);
    failed += run_test("declared mutable operand", patched_operand, 0);
    failed += run_test("undeclared opcode refusal", patched_opcode_refused, 70);
    failed += run_test("printer timeout", printer_status, 0);
    printf("test_machine: 9 cases, %d failures\n", failed);
    return failed ? 1 : 0;
}
