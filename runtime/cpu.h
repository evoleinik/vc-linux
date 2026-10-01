/* 8086 machine state shared by the translated code, the runtime and the
 * DOS/BIOS layer.
 *
 * Memory is one flat array. seg:off maps to seg*16+off with no wrap at 1 MB
 * (A20 on), so the array holds 1 MB plus the 64 KB above it.
 *
 * The CPU model is a 386 running 16-bit real-mode code, because that is what
 * VC ran on in practice and what the unicorn reference emulates. Undefined
 * flags follow unicorn. */
#ifndef VC_CPU_H
#define VC_CPU_H

#include <stdint.h>

#define MEM_SIZE 0x110000u

typedef union {
    uint16_t x;
    struct { uint8_t l, h; };
} Reg16;

typedef struct {
    Reg16 a, b, c, d;
    uint16_t si, di, bp, sp;
    uint16_t es, cs, ss, ds;
    uint16_t ip;
    uint8_t cf, pf, af, zf, sf, tf, ifl, df, of; /* each 0 or 1 */
    uint8_t iopl_nt; /* FLAGS bits 12-14 as last loaded by POPF or IRET */
} Cpu;

extern Cpu cpu;
extern uint8_t mem[MEM_SIZE];

#ifdef CPU_TRACE_WRITES
void cpu_trace_write(uint32_t addr); /* test builds only */
#define CPU_NOTE_WRITE(a) cpu_trace_write(a)
#else
#define CPU_NOTE_WRITE(a) ((void)0)
#endif

static inline uint32_t lin(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }
static inline uint8_t rd8(uint16_t seg, uint16_t off) { return mem[lin(seg, off)]; }
static inline void wr8(uint16_t seg, uint16_t off, uint8_t v) {
    uint32_t a = lin(seg, off);
    CPU_NOTE_WRITE(a);
    mem[a] = v;
}
/* A word at offset FFFFh wraps to offset 0 of the same segment. */
static inline uint16_t rd16(uint16_t seg, uint16_t off) {
    return rd8(seg, off) | (uint16_t)(rd8(seg, (uint16_t)(off + 1)) << 8);
}
static inline void wr16(uint16_t seg, uint16_t off, uint16_t v) {
    wr8(seg, off, (uint8_t)v);
    wr8(seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
}

static inline void push16(uint16_t v) { cpu.sp -= 2; wr16(cpu.ss, cpu.sp, v); }
static inline uint16_t pop16(void) { uint16_t v = rd16(cpu.ss, cpu.sp); cpu.sp += 2; return v; }

/* FLAGS image: bit 1 set, bit 15 clear, bits 12-14 from iopl_nt. */
uint16_t flags_get(void);
/* Load every flag from an image (POPF, IRET, SAHF uses its own low-byte path). */
void flags_set(uint16_t f);

/* Enter interrupt n the way INT does: push FLAGS, CS and ret_ip, clear IF and
 * TF, load CS:IP from the vector table at 0000:n*4. The caller then returns to
 * the runtime dispatcher, which runs whatever CS:IP now points at. */
void cpu_int(uint8_t n, uint16_t ret_ip);

/* Port I/O. Provided by the runtime. */
uint8_t port_in8(uint16_t port);
uint16_t port_in16(uint16_t port);
void port_out8(uint16_t port, uint8_t v);
void port_out16(uint16_t port, uint16_t v);

/* Translated code calls RT_TICK() on every backward jump and every LOOP, so
 * a program spinning on the BIOS clock or the keyboard still sees time pass.
 * rt_yield() is provided by the runtime and resets rt_budget. */
extern int32_t rt_budget;
void rt_yield(void);
#define RT_TICK() do { if (--rt_budget < 0) rt_yield(); } while (0)

/* HLT returns to rt_run with IP at its successor. Only the dispatcher may
 * wait for a hardware event; translated code must never suspend Asyncify. */
extern int rt_halted;

/* Bytes a running copy of translated code sits away from where it was
 * loaded. The dispatcher sets it while it runs code the program copied to a
 * new address, so every IP the code computes points into the copy. Otherwise
 * it is 0. */
extern int32_t rt_code_delta;

/* Translated code reached something it cannot run. Never returns. */
_Noreturn void rt_fault(const char *fmt, ...);

#endif
