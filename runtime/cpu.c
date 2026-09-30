/* The state and interrupt entry shared by all translated images. */
#include "cpu.h"

Cpu cpu;
uint8_t mem[MEM_SIZE];
int32_t rt_budget = 10000;
int32_t rt_code_delta;

uint16_t flags_get(void) {
    return (uint16_t)(2u | cpu.cf | ((uint16_t)cpu.pf << 2)
        | ((uint16_t)cpu.af << 4) | ((uint16_t)cpu.zf << 6)
        | ((uint16_t)cpu.sf << 7) | ((uint16_t)cpu.tf << 8)
        | ((uint16_t)cpu.ifl << 9) | ((uint16_t)cpu.df << 10)
        | ((uint16_t)cpu.of << 11) | ((uint16_t)(cpu.iopl_nt & 7u) << 12));
}

void flags_set(uint16_t f) {
    cpu.cf = f & 1u;
    cpu.pf = (f >> 2) & 1u;
    cpu.af = (f >> 4) & 1u;
    cpu.zf = (f >> 6) & 1u;
    cpu.sf = (f >> 7) & 1u;
    cpu.tf = (f >> 8) & 1u;
    cpu.ifl = (f >> 9) & 1u;
    cpu.df = (f >> 10) & 1u;
    cpu.of = (f >> 11) & 1u;
    cpu.iopl_nt = (f >> 12) & 7u;
}

void cpu_int(uint8_t n, uint16_t ret_ip) {
    /* Unicorn wraps SP, not the second physical byte of a word at FFFF.
     * Do not use the fixed header's per-byte-wrapping push16 helper here. */
    const uint16_t frame[3] = {flags_get(), cpu.cs, ret_ip};
    for (unsigned i = 0; i < 3; ++i) {
        cpu.sp = (uint16_t)(cpu.sp - 2);
        uint32_t a = lin(cpu.ss, cpu.sp);
        CPU_NOTE_WRITE(a);
        mem[a] = (uint8_t)frame[i];
        CPU_NOTE_WRITE(a + 1);
        mem[a + 1] = (uint8_t)(frame[i] >> 8);
    }
    cpu.ifl = 0;
    cpu.tf = 0;
    cpu.ip = rd16(0, (uint16_t)(4u * n));
    cpu.cs = rd16(0, (uint16_t)(4u * n + 2u));
}
