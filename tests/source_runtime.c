/* Browser-only inspector gate. Include the real dispatcher so the small
 * fixture can exercise its private address ring and relocation metadata;
 * unused host/runtime services disappear at link time. No test-only path or
 * export is added to the production build. JS also checks the public ABI. */
#include "../runtime/rt.c"
#include <assert.h>

Cpu cpu;
uint8_t mem[MEM_SIZE];
int32_t rt_budget, rt_code_delta;
static const Image fixture_image = {.name = "fixture", .size = 0x4000};
static const Image stale_image = {.name = "stale", .size = 0x4000};
static uint8_t fixture_snap[0x4000], stale_snap[0x4000], guest_before[MEM_SIZE];

static void read_only_snapshot(void) {
    Cpu before = cpu;
    memcpy(guest_before, mem, sizeof mem);
    vc_source_snapshot();
    assert(!memcmp(&before, &cpu, sizeof cpu));
    assert(!memcmp(guest_before, mem, sizeof mem));
}

int main(void) {
    /* Overflow exercises eviction and hash-chain removal. Long repetition
     * of three idle-loop addresses must retain the older distinct entries. */
    for (unsigned i = 0; i < 1200; ++i)
        source_enter(&fixture_image, i, 0x2000, i);
    assert(source_count == SOURCE_HISTORY);
    assert(source_history[source_newest].offset == 1199);
    assert(source_history[source_history[source_newest].newer].offset == 176);
    for (unsigned i = 0; i < 10000; ++i)
        source_enter(&fixture_image, 500 + i % 3, 0x3000, (uint16_t)(500 + i % 3));
    unsigned at = source_newest;
    for (unsigned i = 0; i < SOURCE_HISTORY; ++i) {
        assert(source_history[source_history[at].older].newer == at);
        assert(source_history[source_history[at].newer].older == at);
        at = source_history[at].older;
    }
    assert(at == source_newest);
    unsigned n = 0;
    for (unsigned b = 0; b < SOURCE_HASH; ++b)
        for (unsigned p = source_hash[b]; p; p = source_history[p - 1].hash_next) {
            assert(source_bucket(source_history[p - 1].image, source_history[p - 1].offset) == b);
            assert(++n <= SOURCE_HISTORY);
        }
    assert(n == SOURCE_HISTORY);

    for (unsigned i = 0; i < sizeof fixture_snap; ++i)
        fixture_snap[i] = (uint8_t)(i * 17 + i / 13);
    fixture_snap[0x40] = 0xcd;
    fixture_snap[0x41] = 0x28;
    /* CALL FAR F000:0100 (the casemap stub), plus two near CALLs. The
     * first near return, 0246h, is also an ordinary interrupt FLAGS value.
     * The JS fixture describes these same instruction/CALL boundaries. */
    const uint8_t casemap_call[] = {0x9a, 0x00, 0x01, 0x00, 0xf0};
    const uint8_t near_call[] = {0xe8, 0x00, 0x00};
    memcpy(fixture_snap + 0x100, casemap_call, sizeof casemap_call);
    memcpy(fixture_snap + 0x243, near_call, sizeof near_call);
    memcpy(fixture_snap + 0x350, near_call, sizeof near_call);
    memcpy(mem + 0x20000, fixture_snap, sizeof fixture_snap);
    known[0] = (Known){.img = &fixture_image, .base = 0x20000, .loadseg = 0x2000,
                       .snap = fixture_snap};
    nknown = 1;
    SourceLocation direct = source_location(0x2000, 0x40, 0);
    assert(direct.image == fixture_image.name && direct.offset == 0x40);
    assert(!source_location(0x1000, 0x40, 0).image);

    /* A younger image shares the load address but its approved relocated
     * bytes differ. The live original wins, not the most recent Image. */
    memset(stale_snap, 0xa5, sizeof stale_snap);
    known[1] = (Known){.img = &stale_image, .base = 0x20000, .loadseg = 0x2000,
                       .snap = stale_snap};
    nknown = 2;
    assert(source_location(0x2000, 0x40, 0).image == fixture_image.name);

    memcpy(mem + 0x30000, fixture_snap, sizeof fixture_snap);
    known[0].deltas[known[0].ndeltas++] = 0x10000;
    SourceLocation copy = source_location(0x3000, 0x40, 0);
    assert(copy.image == fixture_image.name && copy.offset == 0x40);
    memset(mem + 0x30043, 0, 6);
    /* Regression: requiring six bytes for a copied tail (the live-image
     * rule) rejected this valid three-byte match accepted by the dispatcher. */
    assert(source_location(0x3000, 0x40, 0).image == fixture_image.name);
    assert(source_location(0x3000, 0x43, 1).image == fixture_image.name);
    assert(!source_location(0x3000, 0x43, 0).image);
    known[0].ndeltas = 0;
    moved[nmoved++] = (Moved){.lin = 0x30040, .img = &fixture_image, .off = 0x40};
    assert(source_location(0x3000, 0x40, 0).image == fixture_image.name);
    known[0].ndeltas = 1;

    cpu.cs = STUB_SEG;
    cpu.ip = 0x28;
    cpu.ss = 0x4000;
    cpu.sp = 0xfff0;
    wr16(cpu.ss, cpu.sp, 0x42);
    wr16(cpu.ss, (uint16_t)(cpu.sp + 2), 0x2000);
    wr16(cpu.ss, (uint16_t)(cpu.sp + 4), 0x246); /* FLAGS, not a caller */
    wr16(cpu.ss, (uint16_t)(cpu.sp + 6), 0x353); /* the genuine caller */
    wr16(cpu.ss, (uint16_t)(cpu.sp + 8), 0xffff);
    read_only_snapshot();
    /* A different interrupt stub must not claim the preceding INT 28h.
     * Retain the honest continuation when the opcode/vector do not agree. */
    cpu.ip = 0x16;
    read_only_snapshot();
    cpu.ip = STUB_INT8_RETURN;
    read_only_snapshot();

    /* A far-callable stub consumes only IP and CS: the genuine caller
     * immediately after those two words must remain in the snapshot. */
    cpu.ip = STUB_CASEMAP;
    cpu.sp = 0xfff8;
    wr16(cpu.ss, cpu.sp, 0x105);
    wr16(cpu.ss, (uint16_t)(cpu.sp + 2), 0x2000);
    wr16(cpu.ss, (uint16_t)(cpu.sp + 4), 0x353);
    wr16(cpu.ss, (uint16_t)(cpu.sp + 6), 0xffff);
    read_only_snapshot();

    cpu.cs = 0x2000;
    cpu.ip = 0x40;
    cpu.sp = 0x8000;
    wr16(cpu.ss, cpu.sp, 0x353);
    read_only_snapshot();

    /* Popping either kind of stub frame can wrap SP to the start of the
     * segment. The caller scan must start at the wrapped post-frame SP. */
    cpu.cs = STUB_SEG;
    cpu.ip = 0x28;
    cpu.sp = 0xfffe;
    wr16(cpu.ss, cpu.sp, 0x42);
    wr16(cpu.ss, 0, 0x2000);
    wr16(cpu.ss, 2, 0x246);
    wr16(cpu.ss, 4, 0x353);
    wr16(cpu.ss, 6, 0xffff);
    read_only_snapshot();
    cpu.ip = STUB_CASEMAP;
    wr16(cpu.ss, cpu.sp, 0x105);
    wr16(cpu.ss, 2, 0x353);
    wr16(cpu.ss, 4, 0xffff);
    read_only_snapshot();
    vc_source_resolve(0x3000, 0x43, 1);
    return 0;
}
