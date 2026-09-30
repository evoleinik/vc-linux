/* PutTime + BinDec, translated one instruction per line, as a machine
 * translator would emit it. The original assembly is on the right. */
#include "x86.h"

uint16_t OFF_Country;                          /* data offsets from the original image */
enum { CNTRY_TimeSep = 13, CNTRY_TimeFmt = 17, FILENT_TimeF = 1 };

static void BinDec(void) {
    push(C.x);                                 /* USES CX */
    A.h = 0;                                   /* MOV  AH,0 */
    C.l = 10;                                  /* MOV  CL,10 */
    div8(C.l);                                 /* DIV  CL */
    A.x += 0x3030; flags16(A.x);               /* ADD  AX,'00' */
    C.x = pop();
}                                              /* RET */

void PutTime(void) {
    push(A.x); push(B.x); push(C.x); push(D.x);    /* USES AX BX CX DX */
    cmp8(MEM8(es, OFF_Country + CNTRY_TimeFmt), 0);/* CMP  ES:Country.TimeFmt,0 */
    if (zf) goto Space;                            /* JZ   @@Space */
    A.l = ' ';                                     /* MOV  AL,' ' */
    stosb();                                       /* STOSB */
Space:
    A.x = rd16(ds, si + FILENT_TimeF);             /* MOV  AX,DS:[SI].TimeF */
    push(A.x);                                     /* PUSH AX */
    A.x &= 0x1F; flags16(A.x);                     /* AND  AX,1Fh */
    A.x += A.x; flags16(A.x);                      /* ADD  AX,AX */
    BinDec();                                      /* CALL BinDec */
    B.x = A.x;                                     /* MOV  BX,AX */
    A.x = pop();                                   /* POP  AX */
    C.l = 5;                                       /* MOV  CL,5 */
    A.x >>= C.l; flags16(A.x);                     /* SHR  AX,CL */
    push(A.x);                                     /* PUSH AX */
    A.x &= 0x3F; flags16(A.x);                     /* AND  AX,3Fh */
    BinDec();                                      /* CALL BinDec */
    D.x = A.x;                                     /* MOV  DX,AX */
    A.x = pop();                                   /* POP  AX */
    C.l = 6;                                       /* MOV  CL,6 */
    A.x >>= C.l; flags16(A.x);                     /* SHR  AX,CL */
    A.x &= 0x1F; flags16(A.x);                     /* AND  AX,1Fh */
    cmp8(MEM8(es, OFF_Country + CNTRY_TimeFmt), 0);/* CMP  ES:Country.TimeFmt,0 */
    if (!zf) goto ConvHours;                       /* JNE  @@ConvHours */
    C.l = 12;                                      /* MOV  CL,12 */
    div8(C.l);                                     /* DIV  CL */
    zf = (A.h | A.h) == 0;                         /* OR   AH,AH */
    if (!zf) goto ChkAP;                           /* JNE  @@ChkAP */
    A.h = 12;                                      /* MOV  AH,12 */
ChkAP:
    cmp8(A.l, 0);                                  /* CMP  AL,0 */
    A.l = A.h;                                     /* MOV  AL,AH   (flags kept) */
    C.h = 'a';                                     /* MOV  CH,'a'  (flags kept) */
    if (zf) goto ConvHours;                        /* JE   @@ConvHours */
    C.h = 'p';                                     /* MOV  CH,'p' */
ConvHours:
    BinDec();                                      /* CALL BinDec */
    cmp8(A.l, '0');                                /* CMP  AL,'0' */
    if (!zf) goto Write;                           /* JNE  @@Write */
    A.l = ' ';                                     /* MOV  AL,' ' */
Write:
    stosw();                                       /* STOSW */
    A.l = MEM8(es, OFF_Country + CNTRY_TimeSep);   /* MOV  AL,ES:Country.TimeSep */
    push(A.x);                                     /* PUSH AX */
    stosb();                                       /* STOSB */
    A.x = D.x;                                     /* MOV  AX,DX */
    stosw();                                       /* STOSW */
    A.x = pop();                                   /* POP  AX */
    stosb();                                       /* STOSB */
    A.x = B.x;                                     /* MOV  AX,BX */
    stosw();                                       /* STOSW */
    cmp8(MEM8(es, OFF_Country + CNTRY_TimeFmt), 0);/* CMP  ES:Country.TimeFmt,0 */
    if (!zf) goto Exit;                            /* JNE  @@Exit */
    A.l = C.h;                                     /* MOV  AL,CH */
    stosb();                                       /* STOSB */
Exit:
    MEM8(es, di) = 0;                              /* MOV  BYTE PTR ES:[DI],0 */
    D.x = pop(); C.x = pop(); B.x = pop(); A.x = pop();
}                                                  /* RET */
