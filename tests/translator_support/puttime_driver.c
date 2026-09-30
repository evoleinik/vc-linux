/* Exhaustive, independent Unicorn comparison of the generated overlay image.
 * No assembly routine is copied or hand-translated here: image_vc_ovl.run is
 * the normal generated dispatcher, and Unicorn executes the original MZ bytes.
 */
#include "cpu.h"
#include "image.h"
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PT_NREG = 14, PT_MAX_WRITES = 128, PT_EXTRA_CASES = 256 };
enum { PT_AX, PT_BX, PT_CX, PT_DX, PT_SI, PT_DI, PT_BP, PT_SP,
       PT_ES, PT_CS, PT_SS, PT_DS, PT_IP, PT_FLAGS };

typedef struct {
    uint32_t puttime_off, bindec_off, subs_base, format_off, separator_off;
    uint32_t loadseg, cs_delta, format, exhaustive, reverse;
} PutTimeConfig;

typedef struct {
    uint32_t address[PT_MAX_WRITES];
    uint8_t value[PT_MAX_WRITES];
    unsigned count;
    int overflow;
} PutTimeTrace;

static PutTimeTrace pt_c_writes, pt_uc_writes;
static jmp_buf pt_fault_env;
static char pt_fault_text[512];
static unsigned pt_yields, pt_uc_bindec_calls;

static int pt_note(PutTimeTrace *trace, uint32_t address, uint8_t value)
{
    unsigned i;
    for (i = 0; i < trace->count; ++i)
        if (trace->address[i] == address)
            break;
    if (i == PT_MAX_WRITES || address >= MEM_SIZE) {
        trace->overflow = 1;
        return -1;
    }
    if (i == trace->count)
        trace->address[trace->count++] = address;
    trace->value[i] = value;
    return 0;
}

void cpu_trace_write(uint32_t address)
{
    if (pt_note(&pt_c_writes, address, 0) < 0)
        rt_fault("PutTime wrote outside its bounded trace at 0x%x", address);
}

_Noreturn void rt_fault(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(pt_fault_text, sizeof pt_fault_text, format, args);
    va_end(args);
    longjmp(pt_fault_env, 1);
}

void rt_yield(void)
{
    if (++pt_yields > 2)
        rt_fault("PutTime exceeded the backward-branch budget");
    rt_budget = 10000;
}

uint8_t port_in8(uint16_t port) { rt_fault("unexpected IN8 %x", port); }
uint16_t port_in16(uint16_t port) { rt_fault("unexpected IN16 %x", port); }
void port_out8(uint16_t port, uint8_t value)
{
    rt_fault("unexpected OUT8 %x=%x", port, value);
}
void port_out16(uint16_t port, uint16_t value)
{
    rt_fault("unexpected OUT16 %x=%x", port, value);
}

static void pt_uc_write(uc_engine *uc, uc_mem_type type, uint64_t address,
                        int size, int64_t value, void *user)
{
    (void)type;
    (void)user;
    if (size < 1 || size > 8) {
        pt_uc_writes.overflow = 1;
        uc_emu_stop(uc);
        return;
    }
    for (int i = 0; i < size; ++i) {
        if (pt_note(&pt_uc_writes, (uint32_t)address + i,
                    (uint8_t)((uint64_t)value >> (i * 8))) < 0) {
            uc_emu_stop(uc);
            return;
        }
    }
}

static void pt_uc_bindec(uc_engine *uc, uint64_t address, uint32_t size,
                         void *user)
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user;
    ++pt_uc_bindec_calls;
}

static void pt_store_word(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
}

static void pt_cpu_from_regs(const uint32_t values[PT_NREG])
{
    memset(&cpu, 0, sizeof cpu);
    cpu.a.x = values[PT_AX]; cpu.b.x = values[PT_BX];
    cpu.c.x = values[PT_CX]; cpu.d.x = values[PT_DX];
    cpu.si = values[PT_SI]; cpu.di = values[PT_DI];
    cpu.bp = values[PT_BP]; cpu.sp = values[PT_SP];
    cpu.es = values[PT_ES]; cpu.cs = values[PT_CS];
    cpu.ss = values[PT_SS]; cpu.ds = values[PT_DS];
    cpu.ip = values[PT_IP];
    flags_set((uint16_t)values[PT_FLAGS]);
}

static void pt_cpu_to_regs(uint32_t values[PT_NREG])
{
    values[PT_AX] = cpu.a.x; values[PT_BX] = cpu.b.x;
    values[PT_CX] = cpu.c.x; values[PT_DX] = cpu.d.x;
    values[PT_SI] = cpu.si; values[PT_DI] = cpu.di;
    values[PT_BP] = cpu.bp; values[PT_SP] = cpu.sp;
    values[PT_ES] = cpu.es; values[PT_CS] = cpu.cs;
    values[PT_SS] = cpu.ss; values[PT_DS] = cpu.ds;
    values[PT_IP] = cpu.ip; values[PT_FLAGS] = flags_get();
}

static int pt_generated(const PutTimeConfig *config, unsigned *bindec_calls)
{
    unsigned dispatches = 0;
    uint32_t base = config->loadseg * 16;
    *bindec_calls = 0;
    pt_yields = 0;
    rt_budget = 10000;
    if (setjmp(pt_fault_env))
        return -1;
    while (cpu.cs != 0x9000 || cpu.ip != 0x1234) {
        uint32_t address = lin(cpu.cs, cpu.ip);
        if (++dispatches > 64)
            rt_fault("PutTime exceeded 64 dispatcher transfers");
        if (address < base || address >= base + image_vc_ovl.size)
            rt_fault("PutTime left the image at %04x:%04x", cpu.cs, cpu.ip);
        uint32_t off = address - base;
        if (off == config->bindec_off)
            ++*bindec_calls;
        if (image_vc_ovl.run(off, (uint16_t)config->loadseg) != 0)
            rt_fault("PutTime reached non-instruction image offset 0x%x", off);
    }
    return 0;
}

/* Returns the number of complete comparisons, or -1 with a reproducer.
 * reference is independently read and relocated from VC.OVL by the Python
 * test; generated-image bytes and relocation data are deliberately not used
 * to create Unicorn's memory image.
 */
int puttime_check(const uint8_t *reference, uint32_t reference_size,
                  const PutTimeConfig *config, char *message, uint32_t capacity)
{
    static const int reg_ids[PT_NREG] = {
        UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
        UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
        UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS,
        UC_X86_REG_IP, UC_X86_REG_EFLAGS
    };
    static const char *reg_names[PT_NREG] = {
        "AX", "BX", "CX", "DX", "SI", "DI", "BP", "SP",
        "ES", "CS", "SS", "DS", "IP", "FLAGS"
    };
    static const uint16_t edge_times[] = {
        0x0000, 0x0001, 0x001f, 0x0020, 0x0760, 0x07ff, 0x5800, 0x6000,
        0x6800, 0xb800, 0xc000, 0xf800, 0x63c7, 0xa2a5, 0xfffe, 0xffff
    };
    uc_engine *uc = NULL;
    uc_hook write_hook, bindec_hook;
    uc_err error;
    int result = -1;
    unsigned index = 0, time = 0, generated_calls = 0;
    unsigned count = config->exhaustive ? 65536 : PT_EXTRA_CASES;
    uint32_t original_regs[PT_NREG], generated_regs[PT_NREG];
    void *reg_pointers[PT_NREG];
    uint32_t base = config->loadseg * 16;
    uint8_t separator = config->exhaustive ? ':' : (config->reverse ? '.' : '|');
    uint8_t stack[96], output[64], input[4], actual_guard[96];
    char detail[768] = "";

#define PT_FAIL(...) do { snprintf(detail, sizeof detail, __VA_ARGS__); goto cleanup; } while (0)
#define PT_UC(call) do { error = (call); if (error != UC_ERR_OK) \
    PT_FAIL("Unicorn %s: %s", #call, uc_strerror(error)); } while (0)

    if (config->format > 1 || config->loadseg > 0xffff ||
        config->format_off > 0xffff || config->separator_off > 0xffff ||
        config->puttime_off >= reference_size ||
        config->bindec_off >= reference_size ||
        base + reference_size > 0x50000)
        PT_FAIL("invalid PutTime test configuration");

    memset(mem, 0x6a, MEM_SIZE);
    if (image_vc_ovl.size != reference_size)
        PT_FAIL("generated image length %u != original length %u",
                image_vc_ovl.size, reference_size);
    memcpy(mem + base, image_vc_ovl.bytes, image_vc_ovl.size);
    for (uint32_t i = 0; i < image_vc_ovl.nrelocs; ++i) {
        uint32_t off = image_vc_ovl.relocs[i];
        if (off + 1 >= reference_size)
            PT_FAIL("generated relocation 0x%x outside image", off);
        uint16_t value = mem[base + off] | (uint16_t)(mem[base + off + 1] << 8);
        pt_store_word(mem + base + off, (uint16_t)(value + config->loadseg));
    }
    if (memcmp(mem + base, reference, reference_size))
        PT_FAIL("generated load module/relocations differ from original MZ");

    PT_UC(uc_open(UC_ARCH_X86, UC_MODE_16, &uc));
    PT_UC(uc_mem_map(uc, 0, MEM_SIZE, UC_PROT_ALL));
    /* Start with identical non-image canaries, then overwrite code with the
     * separately loaded reference bytes, not the generated image descriptor. */
    PT_UC(uc_mem_write(uc, 0, mem, MEM_SIZE));
    PT_UC(uc_mem_write(uc, base, reference, reference_size));
    PT_UC(uc_hook_add(uc, &write_hook, UC_HOOK_MEM_WRITE,
                      (void *)pt_uc_write, NULL, 1, 0));
    PT_UC(uc_hook_add(uc, &bindec_hook, UC_HOOK_CODE,
                      (void *)pt_uc_bindec, NULL,
                      base + config->bindec_off, base + config->bindec_off));

    for (int i = 0; i < PT_NREG; ++i)
        reg_pointers[i] = &original_regs[i];

    for (index = 0; index < count; ++index) {
        time = config->exhaustive ? index :
            (index < sizeof edge_times / sizeof *edge_times ? edge_times[index] :
             (uint16_t)(index * 0x9e37u));
        /* JWasm can word-align the Subs contribution within DGROUP. Choose
         * the paragraph below it, then derive IP from the physical address. */
        uint32_t cs = (uint16_t)(config->loadseg + config->subs_base / 16 + config->cs_delta);
        uint32_t initial[PT_NREG] = {
            (uint16_t)(0x1111 ^ time), (uint16_t)(0x2222 ^ (time * 3)),
            (uint16_t)(0x3333 ^ (time * 5)), (uint16_t)(0x4444 ^ (time * 7)),
            0x2400, 0x4000, (uint16_t)(0x8888 ^ time), 0x8000,
            0x5000, cs, 0x7000, 0x6000,
            (uint16_t)(base + config->puttime_off - cs * 16),
            0x0202 | (time & 0x08d5) | ((time << 2) & 0x7000) |
                (config->reverse ? 0x0400 : 0)
        };
        uint32_t stack_address = initial[PT_SS] * 16 + initial[PT_SP] - 32;
        uint32_t output_address = initial[PT_ES] * 16 + initial[PT_DI] - 32;
        uint32_t input_address = initial[PT_DS] * 16 + initial[PT_SI];
        uint32_t format_address = initial[PT_ES] * 16 + config->format_off;
        uint32_t separator_address = initial[PT_ES] * 16 + config->separator_off;
        uint8_t format = (uint8_t)config->format;

        memset(stack, 0x6a, sizeof stack);
        pt_store_word(stack + 32, 0x1234);
        pt_store_word(stack + 34, 0x9000);
        memset(output, 0xee, sizeof output);
        input[0] = 0xa5; input[3] = 0x5a;
        pt_store_word(input + 1, (uint16_t)time);
        memcpy(mem + stack_address, stack, sizeof stack);
        memcpy(mem + output_address, output, sizeof output);
        memcpy(mem + input_address, input, sizeof input);
        mem[format_address] = format;
        mem[separator_address] = separator;
        PT_UC(uc_mem_write(uc, stack_address, stack, sizeof stack));
        PT_UC(uc_mem_write(uc, output_address, output, sizeof output));
        PT_UC(uc_mem_write(uc, input_address, input, sizeof input));
        PT_UC(uc_mem_write(uc, format_address, &format, 1));
        PT_UC(uc_mem_write(uc, separator_address, &separator, 1));

        memcpy(original_regs, initial, sizeof initial);
        PT_UC(uc_reg_write_batch(uc, reg_ids, reg_pointers, PT_NREG));
        pt_cpu_from_regs(initial);
        memset(&pt_c_writes, 0, sizeof pt_c_writes);
        memset(&pt_uc_writes, 0, sizeof pt_uc_writes);
        pt_uc_bindec_calls = 0;

        if (pt_generated(config, &generated_calls) < 0)
            PT_FAIL("translated routine faulted: %s", pt_fault_text);
        PT_UC(uc_emu_start(uc, base + config->puttime_off, 0x91234, 0, 4096));
        PT_UC(uc_reg_read_batch(uc, reg_ids, reg_pointers, PT_NREG));
        pt_cpu_to_regs(generated_regs);
        if (original_regs[PT_CS] != 0x9000 || original_regs[PT_IP] != 0x1234)
            PT_FAIL("original routine did not return: %04x:%04x",
                    original_regs[PT_CS], original_regs[PT_IP]);
        if (generated_calls != 3 || pt_uc_bindec_calls != 3)
            PT_FAIL("BinDec dispatch count: original %u, translated %u, expected 3",
                    pt_uc_bindec_calls, generated_calls);

        for (int i = 0; i < PT_NREG; ++i) {
            /* Last CMP defines every arithmetic flag. All other modeled
             * FLAGS bits are preserved; only reserved bits are excluded. */
            uint32_t mask = i == PT_FLAGS ? 0x7fd7 : 0xffff;
            if ((original_regs[i] & mask) != (generated_regs[i] & mask))
                PT_FAIL("%s original=0x%04x translated=0x%04x initial=0x%04x mask=0x%x",
                        reg_names[i], original_regs[i], generated_regs[i], initial[i], mask);
            if (i != PT_DI && i != PT_SP && i != PT_CS && i != PT_IP && i != PT_FLAGS &&
                original_regs[i] != initial[i])
                PT_FAIL("original routine did not preserve %s: initial=0x%04x final=0x%04x",
                        reg_names[i], initial[i], original_regs[i]);
        }
        if (original_regs[PT_SP] != 0x8004 ||
            original_regs[PT_DI] != (config->reverse ? 0x3ff7 : 0x4009))
            PT_FAIL("unexpected original SP/DI: %04x/%04x",
                    original_regs[PT_SP], original_regs[PT_DI]);

        if (pt_c_writes.overflow || pt_uc_writes.overflow ||
            pt_c_writes.count != pt_uc_writes.count)
            PT_FAIL("write-set size: original %u translated %u; overflow %d/%d",
                    pt_uc_writes.count, pt_c_writes.count,
                    pt_uc_writes.overflow, pt_c_writes.overflow);
        for (unsigned i = 0; i < pt_uc_writes.count; ++i) {
            uint32_t address = pt_uc_writes.address[i];
            unsigned j;
            for (j = 0; j < pt_c_writes.count; ++j)
                if (pt_c_writes.address[j] == address)
                    break;
            if (j == pt_c_writes.count)
                PT_FAIL("missing translated write at linear 0x%x", address);
            if (mem[address] != pt_uc_writes.value[i])
                PT_FAIL("memory[0x%x] original=0x%02x translated=0x%02x",
                        address, pt_uc_writes.value[i], mem[address]);
        }
        /* Also inspect final memory rather than trusting write-hook values,
         * including canaries around the output and the entire active stack. */
        PT_UC(uc_mem_read(uc, output_address, actual_guard, sizeof output));
        if (memcmp(actual_guard, mem + output_address, sizeof output))
            PT_FAIL("final output buffer or surrounding canaries differ");
        PT_UC(uc_mem_read(uc, stack_address, actual_guard, sizeof stack));
        if (memcmp(actual_guard, mem + stack_address, sizeof stack))
            PT_FAIL("final stack or surrounding canaries differ");
    }
    result = (int)count;

cleanup:
    if (uc)
        uc_close(uc);
    if (result < 0)
        snprintf(message, capacity,
                 "PutTime time=0x%04x format=%u loadseg=0x%04x CS_delta=0x%x "
                 "DF=%u separator=0x%02x case=%u: %s",
                 time, config->format, config->loadseg, config->cs_delta,
                 config->reverse, separator, index, detail);
    else
        snprintf(message, capacity,
                 "%u PutTime inputs passed: format=%u, CS_delta=0x%x, DF=%u; "
                 "registers, defined flags, write sets, output and stack match",
                 count, config->format, config->cs_delta, config->reverse);
    return result;
#undef PT_UC
#undef PT_FAIL
}
