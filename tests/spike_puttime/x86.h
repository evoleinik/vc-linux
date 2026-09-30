/* Minimal 8086 state for mechanically translated code.
 * Registers and flags live in globals. DOS memory is one flat 1 MB array,
 * so data keeps the exact addresses and byte layout the original used. */
#include <stdint.h>
#include <stdlib.h>

typedef union { uint16_t x; struct { uint8_t l, h; }; } reg16;

extern reg16 A, B, C, D;
extern uint16_t si, di, ds, es, ss;
extern int zf, cf, df;
extern uint8_t M[0x110000];
extern uint16_t stk[256];
extern int sp;

#define LIN(seg, off) (((uint32_t)(seg) << 4) + (uint16_t)(off))
#define MEM8(seg, off) M[LIN(seg, off)]
static inline uint16_t rd16(uint16_t s, uint16_t o) { return MEM8(s, o) | MEM8(s, o + 1) << 8; }

static inline void push(uint16_t v) { stk[sp++] = v; }
static inline uint16_t pop(void) { return stk[--sp]; }
static inline void stosb(void) { MEM8(es, di) = A.l; di += df ? -1 : 1; }
static inline void stosw(void) { MEM8(es, di) = A.l; MEM8(es, di + 1) = A.h; di += df ? -2 : 2; }
static inline void cmp8(uint8_t a, uint8_t b) { zf = a == b; cf = a < b; }
static inline void flags16(uint16_t r) { zf = r == 0; cf = 0; }
static inline void div8(uint8_t d) {           /* DIV r/m8: AX / d -> AL quot, AH rem */
    if (d == 0 || A.x / d > 0xFF) abort();     /* #DE, divide error */
    uint8_t q = A.x / d, r = A.x % d;
    A.l = q; A.h = r;
}
