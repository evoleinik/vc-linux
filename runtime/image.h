/* A program image translated to C. The translator writes one of these per
 * program (build/gen/vc_com.c, build/gen/vc_ovl.c). The runtime loads the
 * bytes into memory like DOS would and runs the translated code in place of
 * executing the bytes. */
#ifndef VC_IMAGE_H
#define VC_IMAGE_H

#include <stdint.h>

typedef struct Image {
    const char *name;       /* canonical embedded name; only VC.COM/VC.OVL are name-selected */
    int is_exe;             /* 1 = MZ executable, 0 = .COM */
    const uint8_t *bytes;   /* load module exactly as in the file (MZ: after the header), unrelocated */
    uint32_t size;
    const uint32_t *relocs; /* MZ: image offsets of the words that get the load segment added */
    uint32_t nrelocs;
    /* MZ header fields, unused for a .COM */
    uint16_t hdr_cs, hdr_ip, hdr_ss, hdr_sp, min_alloc, max_alloc;
    /* Run translated code from image offset `off`, with image offset 0 at
     * linear address loadseg*16 (for a .COM, loadseg = PSP segment + 10h).
     * Runs through static control flow and returns 0 once it has set
     * cpu.cs:cpu.ip for a transfer it cannot follow statically: RET, RETF,
     * IRET, indirect or far JMP/CALL, INT, or a fault.
     * Returns -1 with no side effects if `off` is not the start of a
     * translated instruction. */
    int (*run)(uint32_t off, uint16_t loadseg);
    /* Sorted load-module offsets of operand bytes deliberately written by
     * the program. Only these bytes may differ when matching copied code;
     * their translations read the live operands. Omitted for immutable VC. */
    const uint32_t *mutable_offsets;
    uint32_t nmutable;
} Image;

extern const Image image_vc_com;
extern const Image image_vc_ovl;
extern const Image image_gwbasic;

#endif
