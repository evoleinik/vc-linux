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
static unsigned ticks, breaks, waits, steps, polls, read_waits;
static uint64_t last_input_poll;
static uint16_t original_flags;
static uint16_t read_function;
static unsigned read_interrupt;
static int scenario;
enum {
    MASKED_TIMER, RATE_TIMER, BREAK_IRQ, HALT_IRQ, MUTABLE_CODE, PRINTER,
    BUSY_POLL, BLOCKED_READ,
};

int clock_gettime(clockid_t clock, struct timespec *time) {
    uint64_t ns = now_ns;
    if (clock == CLOCK_REALTIME) ns += 1700000000ull * 1000000000ull;
    time->tv_sec = (time_t)(ns / 1000000000ull);
    time->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

void term_idle(int milliseconds) {
    ++polls;
    if (scenario == BUSY_POLL) {
        CHECK(now_ns - last_input_poll <= 50000000ull);
        last_input_poll = now_ns;
    }
    if (milliseconds > 0) {
        ++waits;
        now_ns += (uint64_t)milliseconds * 1000000ull;
        if (scenario == BLOCKED_READ) {
            if (++read_waits == 1) {
                bios_request_break();
            } else if (read_waits == 2) {
                CHECK(bios_key_push(0x2d78));
                if (read_function == 0x0a00)
                    CHECK(bios_key_push(0x1c0d));
            }
        }
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
int dos_abort_break(void) { return 0; }
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
        CHECK(scenario == BREAK_IRQ || scenario == BLOCKED_READ);
        CHECK(!cpu.ifl && !cpu.tf);
        if (scenario == BLOCKED_READ) {
            CHECK(cpu.sp == 0xeff4); /* Keep the interrupted read's frame. */
            CHECK(rd16(cpu.ss, cpu.sp) == read_interrupt);
            CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == STUB_SEG);
        } else {
            CHECK(cpu.sp == 0xeffa);
            CHECK(rd16(cpu.ss, cpu.sp) == 2);
            CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == 0x2000);
        }
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
    case BUSY_POLL:
        CHECK(now_ns - last_input_poll <= 50000000ull);
        if (now_ns >= stop_ns) {
            CHECK(polls >= 5 && waits == 0);
            rt_exited = 1;
        } else {
            now_ns += 1000000ull; /* Work that never polls, sleeps or does INTs. */
        }
        break;
    case BLOCKED_READ:
        if (off == 0) {
            cpu.a.x = read_function;
            cpu_int((uint8_t)read_interrupt, 1);
        } else {
            CHECK(breaks == 1 && read_waits == 2 && cpu.sp == 0xf000);
            CHECK(cpu.ifl && rd16(0x40, 0x1a) == rd16(0x40, 0x1c));
            if (read_function == 0x0a00) {
                CHECK(rd8(cpu.ds, 0x101) == 2);
                CHECK(rd8(cpu.ds, 0x102) == 'a' && rd8(cpu.ds, 0x103) == 'x');
                CHECK(rd8(cpu.ds, 0x104) == '\r');
            } else {
                CHECK(cpu.a.l == 'x'); /* The break was never an input byte. */
                if (read_interrupt == 0x16) CHECK(cpu.a.x == 0x2d78);
            }
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

static void busy_dispatch_poll(void) {
    setup(BUSY_POLL);
    cpu.ifl = 0; /* Host input remains live even if the guest masks IRQs. */
    while (bios_key_push(0x2d78)) {} /* Queued keys do not suppress the poll. */
    last_input_poll = now_ns;
    stop_ns = now_ns + 250000000ull;
    rt_run();
}

static void yield_without_terminal(void) {
    setup(MUTABLE_CODE);
    unsigned before = polls;
    now_ns += 100000000ull;
    rt_budget = -1;
    rt_yield();
    CHECK(rt_budget == 20000 && polls == before);
}

static void blocked_read(unsigned interrupt, uint16_t function) {
    setup(BLOCKED_READ);
    read_interrupt = interrupt;
    read_function = function;
    port_out8(0x21, 0xff);
    wr16(0, 0x1b * 4, 32);
    wr16(0, 0x1b * 4 + 2, 0x2000);
    if (function == 0x0a00) {
        cpu.ds = 0x7000;
        cpu.d.x = 0x100;
        wr8(cpu.ds, 0x100, 8);
        CHECK(bios_key_push(0x1e61)); /* An edited prefix survives INT 1Bh. */
    }
    rt_run();
}
static void bios_read_break(void) { blocked_read(0x16, 0x0000); }
static void extended_read_break(void) { blocked_read(0x16, 0x1000); }
static void dos_echo_read_break(void) { blocked_read(0x21, 0x0100); }
static void dos_raw_read_break(void) { blocked_read(0x21, 0x0700); }
static void dos_read_break(void) { blocked_read(0x21, 0x0800); }
static void dos_line_break(void) { blocked_read(0x21, 0x0a00); }

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
static const uint8_t supplemental_code[24] = {
    0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0xf7, 0xda, 0x90, 0xfb, 0x90, 0xfa, 0x90, 0x99,
    0x50, 0x51, 0x52, 0x53, 0x58, 0x59, 0x5a, 0x5b,
};
static unsigned supplement_calls;
static int supplement_parent(uint32_t off, uint16_t loadseg) {
    CHECK(loadseg == 0x2000);
    if (off != 10) return -1;
    CHECK(supplement_calls == 1 && cpu.d.x == (uint16_t)-123);
    rt_exited = 1;
    return 0;
}
static int supplement_body(uint32_t off, uint16_t loadseg) {
    CHECK(loadseg == 0x2000);
    if (off != 8) return -1;
    supplement_calls++;
    cpu.d.x = (uint16_t)-cpu.d.x;
    cpu.ip += 2;
    return 0;
}
static const Image supplemented = {
    .name = "SUPPLEMENT.COM", .bytes = supplemental_code, .size = sizeof supplemental_code,
    .run = supplement_parent,
};
static void supplement_setup(int copied) {
    setup(MUTABLE_CODE);
    memcpy(mem + 0x20000, supplemental_code, sizeof supplemental_code);
    rt_register_image(&supplemented, 0x2000);
    cpu.d.x = 123;
    cpu.ip = 8;
    cpu.ifl = 0;
    if (copied) {
        memcpy(mem + 0x30000, supplemental_code, sizeof supplemental_code);
        cpu.cs = 0x3000;
    }
}
static void image_supplement(void) {
    supplement_setup(0);
    rt_register_supplement(&supplemented, supplement_body);
    rt_register_supplement(&supplemented, supplement_body); /* Idempotent. */
    rt_run();
}
static void copied_image_supplement(void) {
    supplement_setup(1);
    rt_register_supplement(&supplemented, supplement_body);
    rt_run();
}
static void changed_supplement_refused(void) {
    supplement_setup(0);
    rt_register_supplement(&supplemented, supplement_body);
    wr8(cpu.cs, cpu.ip, 0xcc);
    rt_run();
}
static void wrong_image_supplement_refused(void) {
    supplement_setup(0);
    rt_register_supplement(&fixture, supplement_body);
    rt_run();
}

static void cga_color_port(void) {
    bios_init();
    cpu.a.x = 4;
    bios_int10();
    port_out8(0x3d9, 0x12); /* Intense palette 0, green background. */
    CHECK(port_in8(0x3d9) == 0x12);
    CHECK(bios_cga_color_register() == 0x12);
    CHECK(bios_cga_color(0) == 2 && bios_cga_color(1) == 10);
    CHECK(bios_cga_color(2) == 12 && bios_cga_color(3) == 14);
    cpu.a.x = 0x0b00;
    cpu.b.x = 0x0101; /* BIOS palette selection keeps intensity/background. */
    bios_int10();
    CHECK(port_in8(0x3d9) == 0x32);
    CHECK(bios_cga_color(1) == 11 && bios_cga_color(2) == 13);
    CHECK(bios_cga_color(3) == 15);
}

static unsigned catalog_calls;
static int catalog_run(uint32_t off, uint16_t loadseg) {
    if (off) return -1;
    CHECK(loadseg == 0x2000 + (catalog_calls % 5) * 0x100);
    CHECK(mem[lin(loadseg, 0)] == 0x90 + catalog_calls % 5);
    if (++catalog_calls == 10) rt_exited = 1;
    else cpu.cs = (uint16_t)(0x2000 + (catalog_calls % 5) * 0x100);
    return 0;
}

static void five_program_catalog(void) {
    /* VC, its overlay, BASIC, Logo and Rogue must coexist even after their
     * child sessions return. Revisit each entry to check none was evicted. */
    static const uint8_t bytes[5][8] = {{0x90}, {0x91}, {0x92}, {0x93}, {0x94}};
    Image images[5];
    memset(&cpu, 0, sizeof cpu);
    memset(images, 0, sizeof images);
    for (unsigned i = 0; i < 5; ++i) {
        images[i].name = "CATALOG.EXE";
        images[i].bytes = bytes[i];
        images[i].size = sizeof bytes[i];
        images[i].run = catalog_run;
        uint16_t seg = (uint16_t)(0x2000 + i * 0x100);
        memcpy(mem + lin(seg, 0), bytes[i], sizeof bytes[i]);
        rt_register_image(&images[i], seg);
    }
    cpu.cs = 0x2000;
    cpu.ss = 0x8000;
    cpu.sp = 0xf000;
    rt_run();
    CHECK(catalog_calls == 10);
}

static uint16_t resumed_loadseg;
static unsigned resumed_calls;
static int resumed_image_run(uint32_t off, uint16_t loadseg) {
    if (off) return -1;
    CHECK(loadseg == resumed_loadseg);
    CHECK(rd16(loadseg, 1) == (uint16_t)(0x1234 + loadseg));
    resumed_calls++;
    rt_exited = 1;
    return 0;
}

static void same_image_return(int forced, int corrupt_parent) {
    /* A relocated immediate inside the first twelve bytes prevents the
     * copied-code fallback from confusing two loads of the same EXE. */
    static const uint8_t parent_code[16] = {
        0xb8, 0x34, 0x12, 0x50, 0x9f, 0x5b, 0x50, 0x90,
        0x58, 0x9e, 0x59, 0x5a, 0x5b, 0x5d, 0x5e, 0xcb,
    };
    static const uint8_t new_code[16] = {
        0xbb, 0x34, 0x12, 0x50, 0x9f, 0x5b, 0x50, 0x90,
        0x58, 0x9e, 0x59, 0x5a, 0x5b, 0x5d, 0x5e, 0xcb,
    };
    static const uint32_t relocs[] = {1};
    const Image parent = {
        .name = "RECURSE.EXE", .is_exe = 1, .bytes = parent_code, .size = sizeof parent_code,
        .relocs = relocs, .nrelocs = 1, .run = resumed_image_run,
    };
    const Image newcomer = {
        .name = "RESIDENT.EXE", .is_exe = 1, .bytes = new_code, .size = sizeof new_code,
        .relocs = relocs, .nrelocs = 1, .run = resumed_image_run,
    };
    setup(MUTABLE_CODE);
    memcpy(mem + 0x20000, parent_code, sizeof parent_code);
    wr16(0x2000, 1, 0x3234);
    rt_register_image(&parent, 0x2000);
    wr16(0, 0x60 * 4, 0x2222);
    port_out8(0x21, 0xaa);
    /* Even a mutation before capture must not become approved code:
     * capture must preserve the registered snapshot, not reread RAM. */
    if (corrupt_parent) wr8(0x2000, 0, 0xcc);
    RtProcessState *state = rt_save_process_state(&parent);
    CHECK(state);
    memcpy(mem + 0x30000, parent_code, sizeof parent_code);
    wr16(0x3000, 1, 0x4234);
    rt_register_image(&parent, 0x3000);
    memcpy(mem + 0x40000, new_code, sizeof new_code);
    wr16(0x4000, 1, 0x5234);
    rt_register_image(&newcomer, 0x4000);
    wr16(0, 0x60 * 4, 0x3333);
    port_out8(0x21, 0x55);
    rt_finish_process_state(state, forced);
    CHECK(rd16(0, 0x60 * 4) == (forced ? 0x2222 : 0x3333));
    CHECK(port_in8(0x21) == (forced ? 0xaa : 0x55));
    resumed_loadseg = cpu.cs = 0x2000;
    cpu.ip = 0;
    cpu.ifl = 0;
    rt_run();
    CHECK(resumed_calls == 1);
    /* Returning to the parent must not evict newly registered, distinct
     * images: VC can still call copied or resident code from its overlay. */
    resumed_loadseg = cpu.cs = 0x4000;
    rt_exited = 0;
    rt_run();
    CHECK(resumed_calls == 2);
}

static void same_image_normal_return(void) { same_image_return(0, 0); }
static void same_image_forced_return(void) { same_image_return(1, 0); }
static void same_image_changed_parent_refused(void) { same_image_return(0, 1); }
static void same_image_forced_changed_parent_refused(void) { same_image_return(1, 1); }

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
    failed += run_test("busy dispatcher polls within 50 ms", busy_dispatch_poll, 0);
    failed += run_test("rt_yield leaves terminal work to dispatcher", yield_without_terminal, 0);
    failed += run_test("Ctrl-Break retries BIOS read without NUL", bios_read_break, 0);
    failed += run_test("Ctrl-Break retries extended BIOS read", extended_read_break, 0);
    failed += run_test("Ctrl-Break retries DOS echoed read", dos_echo_read_break, 0);
    failed += run_test("Ctrl-Break retries DOS raw read", dos_raw_read_break, 0);
    failed += run_test("Ctrl-Break retries DOS read", dos_read_break, 0);
    failed += run_test("Ctrl-Break preserves a buffered line", dos_line_break, 0);
    failed += run_test("HLT yields at dispatcher", halt_irq, 0);
    failed += run_test("speaker ports/frequencies", speaker_ports, 0);
    failed += run_test("declared mutable operand", patched_operand, 0);
    failed += run_test("undeclared opcode refusal", patched_opcode_refused, 70);
    failed += run_test("printer timeout", printer_status, 0);
    failed += run_test("CGA color-select port and BIOS", cga_color_port, 0);
    failed += run_test("listing-proved image supplement", image_supplement, 0);
    failed += run_test("copied listing-proved supplement", copied_image_supplement, 0);
    failed += run_test("changed supplement bytes refused", changed_supplement_refused, 70);
    failed += run_test("supplement scoped to its image", wrong_image_supplement_refused, 70);
    failed += run_test("all five translated programs remain callable", five_program_catalog, 0);
    failed += run_test("same-image EXEC restores parent registration", same_image_normal_return, 0);
    failed += run_test("same-image forced exit restores parent registration", same_image_forced_return, 0);
    failed += run_test("same-image return refuses changed parent bytes", same_image_changed_parent_refused, 70);
    failed += run_test("same-image forced return refuses changed parent bytes", same_image_forced_changed_parent_refused, 70);
    printf("test_machine: 27 cases, %d failures\n", failed);
    return failed ? 1 : 0;
}
