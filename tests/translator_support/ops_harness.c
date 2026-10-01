/*
 * An independent 16-bit real-mode reference.  Generated functions, runtime/cpu.c
 * and this file are linked into one library, compiled with CPU_TRACE_WRITES.
 * Unicorn owns a separate memory array and never calls a CPU runtime helper.
 */
#include "cpu.h"
#include "ops_harness.h"
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WRITE_CAPACITY = 512, PORT_CAPACITY = 8, DIAGNOSTIC_CAPACITY = 24576 };
static uc_engine *reference;
static uc_context *pristine_context;
static uint8_t original_memory[MEM_SIZE], reference_memory[MEM_SIZE];
static uint32_t translated_writes[WRITE_CAPACITY], reference_writes[WRITE_CAPACITY];
static unsigned translated_write_count, reference_write_count;
/* direction (0=IN, 1=OUT), full port number, access width, transferred value */
static uint32_t translated_ports[PORT_CAPACITY][4], reference_ports[PORT_CAPACITY][4];
static unsigned translated_port_count, reference_port_count;
static const OpsInstruction *current;
static uint32_t starting[OPS_NREG];
static unsigned current_sample;
static uint16_t current_loadseg;
static uint32_t current_seed;
static uint64_t checked_states, checked_divide_errors;
static uint64_t resampled_states;
static char diagnostic[DIAGNOSTIC_CAPACITY], trapped_fault[1024];
static size_t diagnostic_length;
static jmp_buf fault_return;
static int unexpected_interrupt;
static unsigned interrupts_dispatched;
int rt_halted;

static const int register_ids[OPS_NREG] = {
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS,
    UC_X86_REG_IP, UC_X86_REG_EFLAGS
};
static const char *const register_names[OPS_NREG] = {
    "AX", "BX", "CX", "DX", "SI", "DI", "BP", "SP",
    "ES", "CS", "SS", "DS", "IP", "FLAGS"
};

static uint32_t random_word(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void append(const char *format, ...) {
    if (diagnostic_length >= sizeof diagnostic - 1) return;
    va_list args;
    va_start(args, format);
    int size = vsnprintf(diagnostic + diagnostic_length,
                         sizeof diagnostic - diagnostic_length, format, args);
    va_end(args);
    if (size > 0) {
        size_t room = sizeof diagnostic - diagnostic_length - 1;
        diagnostic_length += (size_t)size < room ? (size_t)size : room;
    }
}

static void note_write(uint32_t *addresses, unsigned *count, uint32_t address) {
    for (unsigned i = 0; i < *count; ++i)
        if (addresses[i] == address) return;
    if (*count == WRITE_CAPACITY || address >= MEM_SIZE) {
        fprintf(stderr, "ops write trace overflow or unmapped address 0x%x\n", address);
        abort();
    }
    addresses[(*count)++] = address;
}

void cpu_trace_write(uint32_t address) {
    note_write(translated_writes, &translated_write_count, address);
}

static uint32_t port_value(uint32_t port, int size) {
    return (port * UINT32_C(0x1357) + (port >> 8) * UINT32_C(0x5d) + UINT32_C(0x246b)) &
           (size == 1 ? UINT32_C(0xff) : UINT32_C(0xffff));
}

static void note_port(uint32_t events[PORT_CAPACITY][4], unsigned *count,
                      uint32_t direction, uint32_t port, uint32_t size, uint32_t value) {
    if (*count >= PORT_CAPACITY) abort();
    events[*count][0] = direction;
    events[*count][1] = port;
    events[*count][2] = size;
    events[*count][3] = value;
    ++*count;
}

uint8_t port_in8(uint16_t port) {
    uint8_t value = (uint8_t)port_value(port, 1);
    note_port(translated_ports, &translated_port_count, 0, port, 1, value);
    return value;
}
uint16_t port_in16(uint16_t port) {
    uint16_t value = (uint16_t)port_value(port, 2);
    note_port(translated_ports, &translated_port_count, 0, port, 2, value);
    return value;
}
void port_out8(uint16_t port, uint8_t value) {
    note_port(translated_ports, &translated_port_count, 1, port, 1, value);
}
void port_out16(uint16_t port, uint16_t value) {
    note_port(translated_ports, &translated_port_count, 1, port, 2, value);
}

void rt_yield(void) { rt_budget = INT_MAX; }

_Noreturn void rt_fault(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(trapped_fault, sizeof trapped_fault, format, args);
    va_end(args);
    longjmp(fault_return, 1);
}

static void unicorn_write(uc_engine *uc, uc_mem_type type, uint64_t address,
                          int size, int64_t value, void *data) {
    (void)uc; (void)type; (void)value; (void)data;
    for (int i = 0; i < size; ++i)
        note_write(reference_writes, &reference_write_count, (uint32_t)address + i);
}

static uint32_t unicorn_in(uc_engine *uc, uint32_t port, int size, void *data) {
    (void)uc; (void)data;
    uint32_t value = port_value(port, size);
    note_port(reference_ports, &reference_port_count, 0, port, size, value);
    return value;
}

static void unicorn_out(uc_engine *uc, uint32_t port, int size,
                        uint32_t value, void *data) {
    (void)uc; (void)data;
    note_port(reference_ports, &reference_port_count, 1, port, size, value);
}

/* Intel real-mode interrupt entry, independently implemented from the manual:
 * decrement SP by two, write FLAGS; likewise CS, then the continuation IP;
 * clear TF/IF; read the little-endian IP and CS in the four-byte IVT entry.
 * 386 word accesses continue physically beyond offset FFFF after computing the
 * 16-bit effective address.  No generated helper, flags_get or cpu_int is used.
 */
static void reference_push(uint32_t ss, uint32_t *sp, uint16_t value) {
    *sp = (*sp - 2) & 0xffff;
    uint32_t address = (ss << 4) + *sp;
    reference_memory[address] = (uint8_t)value;
    reference_memory[address + 1] = (uint8_t)(value >> 8);
    note_write(reference_writes, &reference_write_count, address);
    note_write(reference_writes, &reference_write_count, address + 1);
}

static uint16_t reference_word(uint32_t address) {
    return reference_memory[address] | (uint16_t)reference_memory[address + 1] << 8;
}

static void unicorn_interrupt(uc_engine *uc, uint32_t vector, void *data) {
    (void)data;
    int software = (current->special & OPS_INT) && vector == current->vector;
    int into = (current->special & OPS_INTO) && vector == 4;
    int divide = (current->flags_kind == OPS_DIV || current->flags_kind == OPS_IDIV)
                 && vector == 0;
    /* TF is randomized too.  Unicorn reports #DB after a step but deliberately
     * does not dispatch it.  Stop-after-one-instruction comparison is the state
     * before debugger entry, so leave that state alone (including TF).
     */
    if (vector == 1 && !software) return;
    if (!software && !into && !divide) {
        unexpected_interrupt = (int)vector;
        uc_emu_stop(uc);
        return;
    }
    uint32_t sp = 0, ss = 0, cs = 0, flags = 0;
    uc_reg_read(uc, UC_X86_REG_SP, &sp);
    uc_reg_read(uc, UC_X86_REG_SS, &ss);
    uc_reg_read(uc, UC_X86_REG_CS, &cs);
    uc_reg_read(uc, UC_X86_REG_EFLAGS, &flags);
    uint16_t continuation = (uint16_t)(starting[OPS_IP] +
                                     (divide ? 0 : current->length));
    reference_push(ss, &sp, (uint16_t)flags);
    reference_push(ss, &sp, (uint16_t)cs);
    reference_push(ss, &sp, continuation);
    flags &= ~(UINT32_C(0x100) | UINT32_C(0x200));
    uint32_t ip = reference_word(vector * 4);
    cs = reference_word(vector * 4 + 2);
    uc_reg_write(uc, UC_X86_REG_SP, &sp);
    uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags);
    uc_reg_write(uc, UC_X86_REG_CS, &cs);
    uc_reg_write(uc, UC_X86_REG_IP, &ip);
    ++interrupts_dispatched;
    if (divide) ++checked_divide_errors;
    uc_emu_stop(uc);
}

static bool unicorn_invalid_instruction(uc_engine *uc, void *data) {
    /* Unicorn 2.1.4 routes the valid software INT 6 (CD 06) through its #UD
     * hook, not UC_HOOK_INTR. Recover only those exact fetched bytes and the
     * unchanged starting IP, then use the same independent Intel interrupt
     * entry implementation as every other vector. Real invalid opcodes are
     * still errors; no case/state or register/flag/memory comparison is lost.
     */
    uint32_t address = ((uint32_t)current_loadseg << 4) + current->offset;
    uint32_t ip = 0;
    if (!(current->special & OPS_INT) || current->vector != 6 ||
        current->length != 2 || reference_memory[address] != 0xcd ||
        reference_memory[address + 1] != 6)
        return false;
    if (uc_reg_read(uc, UC_X86_REG_IP, &ip) || ip != starting[OPS_IP])
        return false;
    unicorn_interrupt(uc, 6, data);
    return true;
}

static void unicorn_code(uc_engine *uc, uint64_t address, uint32_t size, void *data) {
    (void)size; (void)data;
    if ((current->special & OPS_REP) &&
        address != ((uint32_t)current_loadseg << 4) + current->offset)
        uc_emu_stop(uc);
}

static uc_err initialize(void) {
    if (reference) return UC_ERR_OK;
    uint32_t rng = UINT32_C(0xa6e4b10d);
    for (uint32_t address = 0; address < MEM_SIZE; ++address)
        original_memory[address] = (uint8_t)random_word(&rng);
    uc_err error = uc_open(UC_ARCH_X86, UC_MODE_16, &reference);
    if (error) return error;
    error = uc_mem_map_ptr(reference, 0, MEM_SIZE, UC_PROT_ALL, reference_memory);
    if (error) return error;
    uc_hook hook;
    error = uc_hook_add(reference, &hook, UC_HOOK_MEM_WRITE, unicorn_write, NULL, 1, 0);
    if (error) return error;
    error = uc_hook_add(reference, &hook, UC_HOOK_INTR, unicorn_interrupt, NULL, 1, 0);
    if (error) return error;
    error = uc_hook_add(reference, &hook, UC_HOOK_INSN_INVALID,
                        unicorn_invalid_instruction, NULL, 1, 0);
    if (error) return error;
    error = uc_hook_add(reference, &hook, UC_HOOK_CODE, unicorn_code, NULL, 1, 0);
    if (error) return error;
    error = uc_hook_add(reference, &hook, UC_HOOK_INSN, unicorn_in, NULL, 1, 0,
                        UC_X86_INS_IN);
    if (error) return error;
    error = uc_hook_add(reference, &hook, UC_HOOK_INSN, unicorn_out, NULL, 1, 0,
                        UC_X86_INS_OUT);
    if (error) return error;
    error = uc_context_alloc(reference, &pristine_context);
    if (error) return error;
    return uc_context_save(reference, pristine_context);
}

static uint16_t effective_offset(const OpsMemory *operand) {
    uint32_t offset = (uint32_t)operand->displacement;
    if (operand->base >= 0) offset += starting[(unsigned)operand->base];
    if (operand->index >= 0) offset += starting[(unsigned)operand->index];
    return (uint16_t)offset;
}

static uint32_t effective_address(const OpsMemory *operand) {
    return (starting[(unsigned)operand->segment] << 4) + effective_offset(operand);
}

static void put_memory(uint32_t address, uint8_t value) {
    mem[address] = reference_memory[address] = value;
}

static void set_divisor(uint16_t value) {
    if (current->divisor_memory >= 0) {
        uint32_t address = effective_address(&current->operands[(unsigned)current->divisor_memory]);
        put_memory(address, (uint8_t)value);
        if (current->width == 16) put_memory(address + 1, (uint8_t)(value >> 8));
    } else if (current->divisor_register >= 0) {
        uint32_t *reg = &starting[(unsigned)current->divisor_register];
        if (current->divisor_part == 1) *reg = (*reg & 0xff00) | (value & 0xff);
        else if (current->divisor_part == 2) *reg = (*reg & 0xff) | ((value & 0xff) << 8);
        else *reg = value;
    }
}

static void make_starting_state(uint32_t *rng) {
    for (unsigned i = 0; i < OPS_NREG; ++i)
        starting[i] = random_word(rng) & 0xffff;
    starting[OPS_FLAGS] = (starting[OPS_FLAGS] & 0x7fd5) | 2;
    uint32_t address = ((uint32_t)current_loadseg << 4) + current->offset;
    uint32_t minimum_cs = address > 0xffff - current->length
                          ? (address - (0xffff - current->length) + 15) >> 4 : 0;
    uint32_t maximum_cs = address >> 4;
    starting[OPS_CS] = minimum_cs + random_word(rng) % (maximum_cs - minimum_cs + 1);
    starting[OPS_IP] = address - (starting[OPS_CS] << 4);
    static const uint16_t stack_edges[] = {0, 1, 0xffff, 0xfffe, 2, 3, 4, 5, 6};
    if (current_sample < sizeof stack_edges / sizeof stack_edges[0])
        starting[OPS_SP] = stack_edges[current_sample];
    if (current->special & OPS_REP)
        starting[OPS_CX] = current_sample < 3 ? current_sample : random_word(rng) % 41;
    static const uint8_t counts[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 255};
    if (current->count_from_cl && current_sample < sizeof counts)
        starting[OPS_CX] = (starting[OPS_CX] & 0xff00) | counts[current_sample];
    for (unsigned i = 0; i < current->memory_count; ++i) {
        const OpsMemory *operand = &current->operands[i];
        int adjustable = operand->base >= 0 ? operand->base : operand->index;
        if (adjustable >= 0 && current_sample >= 3 && current_sample <= 5) {
            uint16_t edge = current_sample == 3 ? 0xffff : current_sample == 4 ? 0 : 0xfffe;
            starting[(unsigned)adjustable] = (uint16_t)(starting[(unsigned)adjustable] +
                                            edge - effective_offset(operand));
        }
        uint32_t operand_address = effective_address(operand);
        for (unsigned j = 0; j < operand->size; ++j)
            put_memory(operand_address + j, (uint8_t)random_word(rng));
    }
    if (current->flags_kind == OPS_DIV || current->flags_kind == OPS_IDIV) {
        if (current_sample == 0) set_divisor(0); /* Required divide-by-zero #DE. */
        else if (current_sample == 1) {
            starting[OPS_AX] = 0xffff;
            starting[OPS_DX] = 0x7fff;
            set_divisor(1); /* Quotient overflow, independent of zero detection. */
        } else if (current_sample == 2) {
            starting[OPS_AX] = starting[OPS_DX] = 0;
            set_divisor(1); /* Also ensure that a successful divide is exercised. */
        }
    }
    /* Operand setup could touch code through randomized segments.  The code
     * fetch bytes, including each MZ relocated word, are installed last in both
     * memories.  Reads of overlapping instruction/data bytes therefore agree.
     */
    for (unsigned i = 0; i < current->length; ++i)
        put_memory(address + i, current->bytes[i]);
    for (unsigned i = 0; i < current->relocation_count; ++i) {
        unsigned at = current->relocations[i];
        uint16_t value = (uint16_t)(current->bytes[at] |
                                   (uint16_t)current->bytes[at + 1] << 8);
        value = (uint16_t)(value + current_loadseg);
        put_memory(address + at, (uint8_t)value);
        put_memory(address + at + 1, (uint8_t)(value >> 8));
    }
    /* GW-BASIC patches only proved operand fields, not opcodes. Exercise
     * different live immediates/far pointers in every random starting state. */
    for (unsigned i = 0; i < current->mutable_count; ++i)
        put_memory(address + current->mutable_offsets[i], (uint8_t)random_word(rng));
}

/* VC's source contains no self-modifying instructions.  Keep random writes out
 * of the executing code's translation-block page(s), not out of ordinary data
 * and never out of an opcode family.  Unicorn 2.1.4 count=1 can return before a
 * write commits when the write invalidates its current TB, even for bytes after
 * the instruction; REP can otherwise overwrite and re-decode its own prefix.
 * Rejected states are replaced, leaving 32 actual comparisons per instruction.
 */
static int touches_code_page(uint32_t address, unsigned size) {
    uint32_t instruction = ((uint32_t)current_loadseg << 4) + current->offset;
    uint32_t first = instruction & ~UINT32_C(0xfff);
    uint32_t last = (instruction + current->length + 15 + 0xfff) & ~UINT32_C(0xfff);
    return address < last && address + size > first;
}

static int writes_into_code_page(void) {
    for (unsigned i = 0; i < current->memory_count; ++i) {
        const OpsMemory *operand = &current->operands[i];
        if (!operand->writable) continue;
        unsigned repetitions = (current->special & OPS_REP) ? starting[OPS_CX] : 1;
        int step = starting[OPS_FLAGS] & 0x0400 ? -(int)operand->size : operand->size;
        uint16_t offset = effective_offset(operand);
        uint32_t segment = starting[(unsigned)operand->segment] << 4;
        for (unsigned j = 0; j < repetitions; ++j) {
            if (touches_code_page(segment + offset, operand->size)) return 1;
            offset = (uint16_t)(offset + step);
        }
    }
    uint32_t stack_segment = starting[OPS_SS] << 4;
    for (unsigned i = 1; i <= current->stack_write_words; ++i)
        if (touches_code_page(stack_segment + (uint16_t)(starting[OPS_SP] - i * 2), 2))
            return 1;
    return 0;
}

static uint16_t defined_flags(void) {
    /* A faulting DIV never completes, so its arithmetic flags are preserved,
     * not undefined. The independent interrupt-entry oracle accounts for IF/TF.
     */
    if (interrupts_dispatched) return UINT16_C(0xffff);
    unsigned kind = current->flags_kind;
    uint16_t mask = (uint16_t)~current->undefined_flags;
    if (kind < OPS_SHIFT_LEFT || kind > OPS_ROTATE_CARRY) return mask;
    unsigned masked_count = (current->count_from_cl ? starting[OPS_CX] : current->immediate_count) & 31;
    unsigned count = masked_count;
    if (kind == OPS_ROTATE_CARRY) count %= current->width + 1;
    if (!count) return UINT16_C(0xffff); /* Zero count preserves every flag. */
    if (masked_count != 1) mask &= (uint16_t)~0x0800; /* OF only defined for count one. */
    else mask |= 0x0800;
    if (kind <= OPS_SHIFT_ARITH) {
        mask &= (uint16_t)~0x0010; /* AF is undefined for nonzero shifts. */
        if (kind != OPS_SHIFT_ARITH && count >= current->width)
            mask &= (uint16_t)~0x0001;
        else mask |= 0x0001;
    }
    return mask;
}

static void set_translated_state(void) {
    cpu.a.x = starting[OPS_AX]; cpu.b.x = starting[OPS_BX];
    cpu.c.x = starting[OPS_CX]; cpu.d.x = starting[OPS_DX];
    cpu.si = starting[OPS_SI]; cpu.di = starting[OPS_DI];
    cpu.bp = starting[OPS_BP]; cpu.sp = starting[OPS_SP];
    cpu.es = starting[OPS_ES]; cpu.cs = starting[OPS_CS];
    cpu.ss = starting[OPS_SS]; cpu.ds = starting[OPS_DS];
    cpu.ip = starting[OPS_IP]; flags_set(starting[OPS_FLAGS]);
    rt_budget = INT_MAX;
    rt_halted = 0;
}

static void get_translated_state(uint32_t state[OPS_NREG]) {
    state[OPS_AX] = cpu.a.x; state[OPS_BX] = cpu.b.x;
    state[OPS_CX] = cpu.c.x; state[OPS_DX] = cpu.d.x;
    state[OPS_SI] = cpu.si; state[OPS_DI] = cpu.di;
    state[OPS_BP] = cpu.bp; state[OPS_SP] = cpu.sp;
    state[OPS_ES] = cpu.es; state[OPS_CS] = cpu.cs;
    state[OPS_SS] = cpu.ss; state[OPS_DS] = cpu.ds;
    state[OPS_IP] = cpu.ip; state[OPS_FLAGS] = flags_get();
}

static void describe_start(void) {
    diagnostic_length = 0;
    diagnostic[0] = '\0';
    append("%s\nbytes:", current->description);
    for (unsigned i = 0; i < current->length; ++i) append(" %02x", current->bytes[i]);
    append("\nimage offset=0x%05x loadseg=0x%04x sample=%u seed=0x%08x\nstarting state:",
           current->offset, current_loadseg, current_sample, current_seed);
    for (unsigned i = 0; i < OPS_NREG; ++i)
        append("%s%s=%04x", i % 7 == 0 ? "\n  " : " ", register_names[i], starting[i]);
    append("\n  defined FLAGS mask=%04x (undefined=%04x)\n", defined_flags(),
           (uint16_t)~defined_flags());
    append("  memory seed=a6e4b10d; deterministic replay is this instruction, samples 0..%u\n",
           current_sample);
    for (unsigned i = 0; i < current->memory_count; ++i) {
        uint32_t address = effective_address(&current->operands[i]);
        append("  operand %u at %04x:%04x physical=%05x\n", i,
               starting[(unsigned)current->operands[i].segment],
               effective_offset(&current->operands[i]), address);
    }
}

static int contains(const uint32_t *addresses, unsigned count, uint32_t address) {
    for (unsigned i = 0; i < count; ++i) if (addresses[i] == address) return 1;
    return 0;
}

const char *ops_check_instruction(unsigned index, unsigned states) {
    uc_err error = initialize();
    if (error) {
        snprintf(diagnostic, sizeof diagnostic, "Unicorn initialization: %s", uc_strerror(error));
        return diagnostic;
    }
    if (index >= ops_instruction_count || states < 32) {
        snprintf(diagnostic, sizeof diagnostic, "bad instruction index or fewer than 32 states");
        return diagnostic;
    }
    current = &ops_instructions[index];
    current_seed = UINT32_C(2166136261) ^ current->offset;
    for (unsigned i = 0; i < current->length; ++i)
        current_seed = (current_seed ^ current->bytes[i]) * UINT32_C(16777619);
    uint32_t rng = current_seed ? current_seed : 1;
    current_loadseg = (uint16_t)(0x1000 + random_word(&rng) % 0x9000);
    memcpy(mem, original_memory, MEM_SIZE);
    memcpy(reference_memory, original_memory, MEM_SIZE);
    for (current_sample = 0; current_sample < states; ++current_sample) {
        unsigned attempts = 0;
        do {
            make_starting_state(&rng);
            if (!writes_into_code_page()) break;
            ++resampled_states;
            if (++attempts == 128) {
                describe_start();
                append("could not generate a non-self-modifying state in 128 attempts\n");
                return diagnostic;
            }
        } while (1);
        translated_write_count = reference_write_count = 0;
        translated_port_count = reference_port_count = 0;
        unexpected_interrupt = -1;
        interrupts_dispatched = 0;
        trapped_fault[0] = '\0';
        set_translated_state();
        if (setjmp(fault_return)) {
            describe_start();
            append("translated instruction called rt_fault: %s\n", trapped_fault);
            return diagnostic;
        }
        int result = current->run(current_loadseg);
        uint32_t actual[OPS_NREG], expected[OPS_NREG] = {0};
        get_translated_state(actual);
        void *values[OPS_NREG];
        for (unsigned i = 0; i < OPS_NREG; ++i) values[i] = &starting[i];
        /* Restore hidden CPU state too: without this, Unicorn remembers the
         * previous #DE and turns the following case into a spurious #DF.
         */
        error = uc_context_restore(reference, pristine_context);
        if (!error) error = uc_reg_write_batch(reference, register_ids, values, OPS_NREG);
        if (!error) {
            uint64_t address = ((uint32_t)current_loadseg << 4) + current->offset;
            /* Host writes through mem_map_ptr do not invalidate translated TBs. */
            error = uc_ctl_remove_cache(reference, address, address + current->length + 16);
            if (!error)
                error = uc_emu_start(reference, address,
                                     (current->special & OPS_REP) ? address + current->length : UINT64_MAX,
                                     0, (current->special & OPS_REP) ? 128 : 1);
        }
        if (error || unexpected_interrupt >= 0) {
            describe_start();
            append("Unicorn error=%s unexpected interrupt=%d\n", uc_strerror(error), unexpected_interrupt);
            return diagnostic;
        }
        for (unsigned i = 0; i < OPS_NREG; ++i) values[i] = &expected[i];
        error = uc_reg_read_batch(reference, register_ids, values, OPS_NREG);
        if (error) {
            describe_start(); append("Unicorn register read: %s\n", uc_strerror(error));
            return diagnostic;
        }
        int different = result != 0;
        uint16_t flag_mask = defined_flags();
        for (unsigned i = 0; i < OPS_NREG; ++i) {
            uint16_t mask = i == OPS_FLAGS ? flag_mask : 0xffff;
            if ((actual[i] ^ expected[i]) & mask) different = 1;
        }
        if (translated_write_count != reference_write_count) different = 1;
        for (unsigned i = 0; i < reference_write_count; ++i) {
            uint32_t address = reference_writes[i];
            if (!contains(translated_writes, translated_write_count, address) ||
                mem[address] != reference_memory[address]) different = 1;
        }
        if (translated_port_count != reference_port_count ||
            memcmp(translated_ports, reference_ports, translated_port_count * sizeof translated_ports[0]))
            different = 1;
        if (different) {
            describe_start();
            if (result) append("function return: C=%d expected=0\n", result);
            for (unsigned i = 0; i < OPS_NREG; ++i) {
                uint16_t mask = i == OPS_FLAGS ? flag_mask : 0xffff;
                if ((actual[i] ^ expected[i]) & mask)
                    append("%s differs: C=%04x Unicorn=%04x xor=%04x compared_mask=%04x\n",
                           register_names[i], actual[i], expected[i] & 0xffff,
                           (actual[i] ^ expected[i]) & 0xffff, mask);
            }
            append("written byte counts: C=%u Unicorn=%u\n", translated_write_count, reference_write_count);
            for (unsigned i = 0; i < translated_write_count; ++i) {
                uint32_t address = translated_writes[i];
                int present = contains(reference_writes, reference_write_count, address);
                if (!present || mem[address] != reference_memory[address])
                    append("write[%05x]: C=%02x Unicorn=%s%02x\n", address, mem[address],
                           present ? "" : "not written; value=", reference_memory[address]);
            }
            for (unsigned i = 0; i < reference_write_count; ++i) {
                uint32_t address = reference_writes[i];
                if (!contains(translated_writes, translated_write_count, address))
                    append("write[%05x]: C=not written Unicorn=%02x\n", address, reference_memory[address]);
            }
            if (translated_port_count != reference_port_count ||
                memcmp(translated_ports, reference_ports, translated_port_count * sizeof translated_ports[0])) {
                append("port I/O events differ: C=%u Unicorn=%u\n", translated_port_count, reference_port_count);
                for (unsigned i = 0; i < translated_port_count; ++i)
                    append("  C %s port=%04x size=%u value=%04x\n",
                           translated_ports[i][0] ? "OUT" : "IN", translated_ports[i][1],
                           translated_ports[i][2], translated_ports[i][3]);
                for (unsigned i = 0; i < reference_port_count; ++i)
                    append("  Unicorn %s port=%04x size=%u value=%04x\n",
                           reference_ports[i][0] ? "OUT" : "IN", reference_ports[i][1],
                           reference_ports[i][2], reference_ports[i][3]);
            }
            return diagnostic;
        }
        ++checked_states;
    }
    return NULL;
}

uint64_t ops_checked_states(void) { return checked_states; }
uint64_t ops_checked_divide_errors(void) { return checked_divide_errors; }
uint64_t ops_resampled_states(void) { return resampled_states; }
