/* Private metadata for the exhaustive, one-instruction Unicorn comparison. */
#ifndef TRANSLATOR_OPS_HARNESS_H
#define TRANSLATOR_OPS_HARNESS_H

#include <stdint.h>

enum {
    OPS_AX, OPS_BX, OPS_CX, OPS_DX, OPS_SI, OPS_DI, OPS_BP, OPS_SP,
    OPS_ES, OPS_CS, OPS_SS, OPS_DS, OPS_IP, OPS_FLAGS, OPS_NREG
};
enum {
    OPS_NORMAL, OPS_SHIFT_LEFT, OPS_SHIFT_RIGHT, OPS_SHIFT_ARITH,
    OPS_ROTATE, OPS_ROTATE_CARRY, OPS_DIV, OPS_IDIV
};
enum { OPS_INT = 1, OPS_INTO = 2, OPS_REP = 4 };

typedef struct {
    int8_t base, index, segment;
    uint8_t size, writable;
    int32_t displacement;
} OpsMemory;

typedef struct {
    int (*run)(uint16_t);
    const char *description;
    uint32_t offset;
    uint8_t bytes[16];
    uint8_t length, relocation_count, relocations[8];
    uint16_t undefined_flags;
    uint8_t flags_kind, width, count_from_cl, immediate_count;
    uint8_t special, vector, memory_count, stack_write_words;
    OpsMemory operands[3];
    int8_t divisor_register, divisor_part, divisor_memory;
} OpsInstruction;

extern const OpsInstruction ops_instructions[];
extern const unsigned ops_instruction_count;

/* NULL means all states passed; otherwise a complete diagnostic is returned. */
const char *ops_check_instruction(unsigned index, unsigned states);
uint64_t ops_checked_states(void);
uint64_t ops_checked_divide_errors(void);
uint64_t ops_resampled_states(void);

#endif
