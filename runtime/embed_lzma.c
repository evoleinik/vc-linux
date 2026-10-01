/* Fixed-profile raw LZMA1 decoder for tools/embed.py, not a general archive API.
 * lc=lp=pb=0, dictionary <= 1 MiB, required end marker, exact input/output sizes.
 * No allocation, runtime downloads, or guest/interpreter changes are involved.
 */
#include "embed_lzma.h"
#include <string.h>

typedef struct {
    const uint8_t *src;
    size_t size, at;
    uint32_t code, range;
    int failed;
} Range;

typedef struct {
    uint16_t choice[2], low[8], middle[8], high[256];
} Length;

typedef struct {
    uint16_t match[12], rep[12], rep0[12], rep1[12], rep2[12], shortrep[12];
    uint16_t slot[4][64], special[114], align[16], literal[768];
    Length length, rep_length;
} Models;

static void normalize(Range *r) {
    if (r->range < (1u << 24)) {
        if (r->at == r->size) { r->failed = 1; return; }
        r->range <<= 8;
        r->code = (r->code << 8) | r->src[r->at++];
    }
}

static unsigned bit(Range *r, uint16_t *probability) {
    normalize(r);
    uint32_t bound = (r->range >> 11) * *probability;
    if (r->code < bound) {
        r->range = bound;
        *probability += (uint16_t)((2048u - *probability) >> 5);
        return 0;
    }
    r->range -= bound;
    r->code -= bound;
    *probability -= (uint16_t)(*probability >> 5);
    return 1;
}

static unsigned tree(Range *r, uint16_t *p, unsigned bits) {
    unsigned symbol = 1;
    for (unsigned n = 0; n < bits; ++n) symbol = (symbol << 1) | bit(r, &p[symbol]);
    return symbol - (1u << bits);
}

static unsigned reverse_tree(Range *r, uint16_t *p, int base, unsigned bits) {
    unsigned symbol = 1, value = 0;
    for (unsigned n = 0; n < bits; ++n) {
        unsigned b = bit(r, &p[base + (int)symbol]);
        symbol = (symbol << 1) | b;
        value |= b << n;
    }
    return value;
}

static uint32_t direct(Range *r, unsigned bits) {
    uint32_t value = 0;
    for (unsigned n = 0; n < bits; ++n) {
        normalize(r);
        r->range >>= 1;
        unsigned b = r->code >= r->range;
        if (b) r->code -= r->range;
        value = (value << 1) | b;
    }
    return value;
}

static unsigned length(Range *r, Length *p) {
    if (!bit(r, &p->choice[0])) return 2 + tree(r, p->low, 3);
    if (!bit(r, &p->choice[1])) return 10 + tree(r, p->middle, 3);
    return 18 + tree(r, p->high, 8);
}

int embed_lzma_decode(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size) {
    if (!src || (!dst && dst_size) || src_size < 5 || src[0]) return -1;
    Range r = {.src = src, .size = src_size, .at = 5, .range = UINT32_MAX};
    for (unsigned i = 1; i < 5; ++i) r.code = (r.code << 8) | src[i];
    Models p;
    /* Initialize through character storage, without aliasing across members. */
    const uint16_t initial = 1024;
    for (size_t i = 0; i < sizeof p; i += sizeof initial)
        memcpy((uint8_t *)&p + i, &initial, sizeof initial);
    uint32_t reps[4] = {0, 0, 0, 0}; /* Distance minus one. */
    unsigned state = 0;
    size_t at = 0;
    while (!r.failed) {
        if (!bit(&r, &p.match[state])) {
            if (at == dst_size) return -1;
            unsigned symbol = 1;
            if (state >= 7) {
                if (reps[0] >= at) return -1;
                unsigned match = dst[at - reps[0] - 1];
                do {
                    unsigned match_bit = (match >> 7) & 1;
                    match <<= 1;
                    unsigned b = bit(&r, &p.literal[((1 + match_bit) << 8) + symbol]);
                    symbol = (symbol << 1) | b;
                    if (b != match_bit) break;
                } while (symbol < 256);
            }
            while (symbol < 256) symbol = (symbol << 1) | bit(&r, &p.literal[symbol]);
            dst[at++] = (uint8_t)symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned count;
        if (bit(&r, &p.rep[state])) {
            if (!bit(&r, &p.rep0[state])) {
                if (!bit(&r, &p.shortrep[state])) {
                    state = state < 7 ? 9 : 11;
                    count = 1;
                    goto copy;
                }
            } else {
                uint32_t distance;
                if (!bit(&r, &p.rep1[state])) distance = reps[1];
                else {
                    if (!bit(&r, &p.rep2[state])) distance = reps[2];
                    else { distance = reps[3]; reps[3] = reps[2]; }
                    reps[2] = reps[1];
                }
                reps[1] = reps[0];
                reps[0] = distance;
            }
            count = length(&r, &p.rep_length);
            state = state < 7 ? 8 : 11;
        } else {
            reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0];
            count = length(&r, &p.length);
            state = state < 7 ? 7 : 10;
            unsigned slot = tree(&r, p.slot[count < 6 ? count - 2 : 3], 6);
            reps[0] = slot;
            if (slot >= 4) {
                unsigned bits = (slot >> 1) - 1;
                reps[0] = (2u | (slot & 1)) << bits;
                if (slot < 14)
                    reps[0] += reverse_tree(&r, p.special, (int)reps[0] - (int)slot - 1, bits);
                else {
                    reps[0] += direct(&r, bits - 4) << 4;
                    reps[0] += reverse_tree(&r, p.align, 0, 4);
                    if (reps[0] == UINT32_MAX) {
                        normalize(&r);
                        return !r.failed && !r.code && at == dst_size &&
                               count == 2 && r.at == src_size ? 0 : -1;
                    }
                }
            }
        }
copy:
        if (r.failed || reps[0] >= (1u << 20) || reps[0] >= at ||
            count > dst_size - at) return -1;
        for (unsigned i = 0; i < count; ++i) { dst[at] = dst[at - reps[0] - 1]; ++at; }
    }
    return -1;
}

uint32_t embed_adler32(const uint8_t *data, size_t size) {
    uint32_t a = 1, b = 0;
    /* 5552 bytes is the largest chunk whose sums cannot overflow uint32_t. */
    while (size) {
        size_t n = size < 5552 ? size : 5552;
        size -= n;
        while (n--) { a += *data++; b += a; }
        a %= 65521u; b %= 65521u;
    }
    return (b << 16) | a;
}

void embed_x86_16_restore(uint8_t *data, size_t size) {
    if (size < 3) return;
    for (size_t at = 0; at <= size - 3;) {
        if (data[at] != 0xe8 && data[at] != 0xe9) {
            ++at;
            continue;
        }
        uint16_t absolute = (uint16_t)(data[at + 1] | (unsigned)data[at + 2] << 8);
        uint16_t relative = (uint16_t)(absolute - (uint16_t)(at + 3));
        data[at + 1] = (uint8_t)relative;
        data[at + 2] = (uint8_t)(relative >> 8);
        /* Operand bytes can themselves be E8/E9. The encoder skipped them,
         * so the inverse must too; incomplete final operands stay untouched. */
        at += 3;
    }
}
