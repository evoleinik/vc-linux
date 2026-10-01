/* COM1 through the real dispatcher, interrupt frames, 8259 and INT 14h.
 * Only the clock, terminal and unrelated DOS services are test doubles. */
#define _POSIX_C_SOURCE 200809L
#include "bios.h"
#include "cpu.h"
#include "hle.h"
#include "modem.h"
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

enum { BOUNDARIES, PRIORITY, HALTED, BIOS_SERIAL };
static int scenario;
static uint64_t now_ns = 1000000000ull;
static unsigned serials, timers, steps;
static uint16_t saved_flags;

int clock_gettime(clockid_t id, struct timespec *out) {
    uint64_t ns = now_ns + (id == CLOCK_REALTIME ? 1700000000000000000ull : 0);
    out->tv_sec = (time_t)(ns / 1000000000ull);
    out->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
void term_idle(int ms) { if (ms > 0) now_ns += (uint64_t)ms * 1000000ull; }
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
int dos_int_other(uint8_t n) { (void)n; return 1; }

static void iret(void) {
    cpu.ip = pop16();
    cpu.cs = pop16();
    flags_set(pop16());
}
static uint8_t isr(void) {
    port_out8(0x20, 0x0b);
    return port_in8(0x20);
}
static uint8_t irr(void) {
    port_out8(0x20, 0x0a);
    return port_in8(0x20);
}
static void receive(uint8_t byte) {
    port_out8(0x3f8, byte); /* UART internal loopback, no fake modem data. */
    now_ns += 2000000ull;
    CHECK(port_in8(0x3fd) & 1);
}

static int run(uint32_t off, uint16_t loadseg) {
    CHECK(loadseg == 0x2000 && ++steps < 100);
    if (off == 32) {
        CHECK(scenario == PRIORITY && !cpu.ifl && !cpu.tf);
        ++timers;
        CHECK(isr() == 0x11); /* IRQ 0 preempts the active lower-priority IRQ 4. */
        port_out8(0x20, 0x20);
        CHECK(isr() == 0x10); /* General EOI must not clear both. */
        iret();
        return 0;
    }
    if (off == 16) {
        CHECK(!cpu.ifl && !cpu.tf && cpu.sp == 0xeffa);
        CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 2)) == 0x2000);
        CHECK(rd16(cpu.ss, (uint16_t)(cpu.sp + 4)) == saved_flags);
        ++serials;
        CHECK(port_in8(0x3fa) == 4);
        CHECK(isr() & 0x10);
        if (scenario == PRIORITY && serials == 1) {
            CHECK(port_in8(0x3f8) == 'A');
            receive('B'); /* Another rising edge while IRQ 4 is in service. */
            CHECK(irr() & 0x10);
            cpu.ifl = 1;
            cpu.ip = 17;
            now_ns += 55000000ull;
            return 0;
        }
        port_out8(0x20, 0x60); /* Wrong specific EOI cannot acknowledge IRQ 4. */
        CHECK(isr() & 0x10);
        port_out8(0x20, 0x64);
        CHECK(!(isr() & 0x10));
        if (scenario == BOUNDARIES) {
            /* Kermit EOIs before reading RBR. Even STI plus a dispatch
             * boundary must not manufacture another edge from that level. */
            cpu.ifl = 1;
            cpu.ip = 17;
            return 0;
        }
        CHECK(port_in8(0x3f8) == (scenario == PRIORITY ? 'B' : 'A'));
        iret();
        return 0;
    }
    if (off == 17) {
        CHECK(serials == 1);
        if (scenario == BOUNDARIES) {
            CHECK(port_in8(0x3f8) == 'A');
            CHECK(!(irr() & 0x10));
        } else {
            CHECK(scenario == PRIORITY && timers == 1);
            CHECK(isr() == 0x10 && (irr() & 0x10));
            port_out8(0x20, 0x64);
        }
        iret();
        return 0;
    }
    if (scenario == BIOS_SERIAL) {
        switch (off) {
        case 0:
            CHECK(rd16(0x40, 0) == 0x3f8 && (rd16(0x40, 0x10) & 0xe00) == 0x200);
            cpu.a.x = 0x00e3; /* 9600, 8N1. */
            cpu.d.x = 0;
            cpu_int(0x14, 1);
            break;
        case 1:
            CHECK(cpu.a.h == 0x60 && (cpu.a.l & 0x30) == 0x30);
            port_out8(0x3fb, 0x83);
            CHECK(port_in8(0x3f8) == 12 && port_in8(0x3f9) == 0);
            port_out8(0x3fb, 3);
            port_out8(0x3fc, 0x13); /* Loopback, IRQ output disabled. */
            cpu.a.x = 0x014b;
            cpu_int(0x14, 2);
            break;
        case 2:
            CHECK(cpu.a.l == 'K' && !(cpu.a.h & 0x80));
            now_ns += 2000000ull;
            cpu.a.x = 0x0300;
            cpu_int(0x14, 3);
            break;
        case 3:
            CHECK((cpu.a.h & 0x61) == 0x61 && (cpu.a.l & 0x30) == 0x30);
            cpu.a.x = 0x0200;
            cpu_int(0x14, 4);
            break;
        case 4:
            CHECK(cpu.a.l == 'K' && !(cpu.a.h & 0x80));
            cpu.a.x = 0x0200;
            cpu_int(0x14, 5);
            break;
        case 5:
            CHECK(cpu.a.h & 0x80); /* Empty read is a BIOS timeout. */
            cpu.d.x = 1;
            cpu.a.x = 0x0300;
            cpu_int(0x14, 6);
            break;
        case 6:
            CHECK(cpu.a.h & 0x80); /* No fabricated COM2. */
            CHECK(cpu.sp == 0xf000 && cpu.ifl);
            rt_exited = 1;
            break;
        default: return -1;
        }
        return 0;
    }
    if (off == 0) {
        if (scenario == BOUNDARIES) cpu.ifl = 0;
        receive('A');
        CHECK(serials == 0); /* IN/OUT must never call the ISR recursively. */
        cpu.ip = 1;
        if (scenario == HALTED) rt_halted = 1;
    } else if (scenario == BOUNDARIES && off < 3) {
        CHECK(serials == 0 && (irr() & 0x10));
        if (off == 1) {
            /* Test IF with IRQ 4 unmasked first, then OCW1 with IF set. */
            port_out8(0x21, 0xff);
            cpu.ifl = 1;
        }
        else port_out8(0x21, 0xef);
        cpu.ip++;
    } else {
        CHECK(serials == (scenario == PRIORITY ? 2u : 1u));
        CHECK(!rt_halted && cpu.sp == 0xf000 && flags_get() == saved_flags);
        CHECK(!(isr() & 0x10) && !(irr() & 0x10));
        rt_exited = 1;
    }
    return 0;
}

static const uint8_t bytes[64] = {0x90};
static const Image image = {.name = "SERIAL.COM", .bytes = bytes, .size = sizeof bytes, .run = run};
static void setup(int which) {
    scenario = which;
    memset(&cpu, 0, sizeof cpu);
    memset(mem, 0, sizeof mem);
    for (unsigned n = 0; n < 256; ++n) {
        wr16(0, n * 4, (uint16_t)n);
        wr16(0, n * 4 + 2, STUB_SEG);
    }
    bios_init();
    modem_init(NULL);
    memcpy(mem + 0x20000, bytes, sizeof bytes);
    rt_register_image(&image, 0x2000);
    cpu.cs = 0x2000;
    cpu.ss = 0x8000;
    cpu.sp = 0xf000;
    flags_set(0x0ed7);
    saved_flags = flags_get();
    wr16(0, 0x0c * 4, 16);
    wr16(0, 0x0c * 4 + 2, 0x2000);
    wr16(0, 0x08 * 4, 32);
    wr16(0, 0x08 * 4 + 2, 0x2000);
    port_out8(0x3fb, 0x83);
    port_out8(0x3f8, 12);
    port_out8(0x3f9, 0);
    port_out8(0x3fb, 3);
    port_out8(0x3fc, 0x1b);
    port_out8(0x3f9, 1);
    port_out8(0x21, which == BIOS_SERIAL ? 0xff : which == PRIORITY ? 0xee : 0xef);
}
static void boundaries(void) { setup(BOUNDARIES); rt_run(); }
static void priority(void) { setup(PRIORITY); rt_run(); }
static void halted(void) { setup(HALTED); rt_run(); }
static void bios(void) { setup(BIOS_SERIAL); rt_run(); }
static void forced_exit(void) {
    setup(BOUNDARIES);
    RtProcessState *saved = rt_save_process_state(NULL);
    CHECK(saved);
    receive('A');
    CHECK(irr() & 16);
    rt_finish_process_state(saved, 1);
    CHECK(port_in8(0x3f9) == 0 && port_in8(0x3fc) == 0);
    CHECK(!(irr() & 16) && !(isr() & 16));
}

int main(int argc, char **argv) {
    const struct { const char *name; void (*test)(void); } tests[] = {
        {"irq4-boundaries", boundaries}, {"irq4-priority-eoi", priority},
        {"irq4-halt", halted}, {"int14", bios}, {"serial-forced-exit", forced_exit},
    };
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
        if (argc == 2 && strcmp(argv[1], tests[i].name)) continue;
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) { tests[i].test(); _exit(0); }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        printf("PASS serial machine: %s\n", tests[i].name);
        ++ran;
    }
    CHECK(ran);
    return 0;
}
