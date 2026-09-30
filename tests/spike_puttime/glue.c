/* ctypes entry points: run each translation for one input. */
#include <string.h>
#include "x86.h"
reg16 A, B, C, D; uint16_t si, di, ds, es, ss; int zf, cf, df;
uint8_t M[0x110000]; uint16_t stk[256]; int sp;
extern uint16_t OFF_Country;
void PutTime(void);
char *put_time(char *out, uint16_t t, _Bool h24, char sep);

int run_mech(uint16_t t, int fmt, char sep, uint16_t country, uint16_t ent, uint16_t outbuf, uint8_t *out) {
    const uint16_t seg = 0x1000;
    ds = es = ss = seg; df = 0; sp = 0; OFF_Country = country;
    A.x = 0x1111; B.x = 0x2222; C.x = 0x3333; D.x = 0x4444;
    MEM8(seg, country + 17) = fmt; MEM8(seg, country + 13) = sep;
    MEM8(seg, ent + 1) = t & 0xFF; MEM8(seg, ent + 2) = t >> 8;
    memset(&MEM8(seg, outbuf), 0xEE, 16);
    si = ent; di = outbuf;
    PutTime();
    if (A.x != 0x1111 || B.x != 0x2222 || C.x != 0x3333 || D.x != 0x4444 || si != ent) return -1;
    memcpy(out, &MEM8(seg, outbuf), 16);
    return di - outbuf;
}

int run_c(uint16_t t, int fmt, char sep, uint8_t *out) {
    memset(out, 0xEE, 16);
    return put_time((char *)out, t, fmt != 0, sep) - (char *)out;
}
