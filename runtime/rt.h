/* Runtime core: the dispatcher, interrupt stubs, loaded images and the DOS
 * process and memory manager. Internal to runtime/. */
#ifndef VC_RT_H
#define VC_RT_H

#include "cpu.h"
#include "image.h"

/* Stub addresses in the BIOS ROM segment. F000:00nn is the default handler of
 * INT nn. The dispatcher calls C when CS:IP lands on one. */
#define STUB_SEG      0xF000u
#define STUB_CASEMAP  0x0100u   /* far-callable country case-map routine */
#define STUB_EXIT     0x0110u   /* terminate address of the first process */
#define STUB_END      0x0400u

/* Set by a handler that has already moved CS:IP (and maybe SS:SP) somewhere
 * else, such as EXEC or terminate. The stub then skips its normal return. */
extern int hle_redirect;

/* Added to every IP the translated code computes, while the dispatcher runs
 * code that the program copied away from where it was loaded. */
extern int32_t rt_code_delta;

extern int rt_exited;
extern int rt_exit_code;

void rt_log(const char *fmt, ...);
void rt_update_clock(void);
void rt_register_image(const Image *img, uint16_t loadseg);
void rt_run(void);

/* dos_core.c */
void dos_core_init(void);
int dos_core_int21(void);
/* Handle a software interrupt other than 10h, 16h, 21h and 33h.
 * Returns 1 if the stub should return with a full IRET, 0 for RETF 2. */
int dos_int_other(uint8_t n);
/* Load VC.COM as the first process. dos_prog is its DOS path. */
void dos_start(const char *dos_prog, const uint8_t *tail, int tail_len);

/* Default setup files and images embedded in the binary: build/gen/files.c */
typedef struct { const char *name; const uint8_t *data; uint32_t size; } EmbeddedFile;
extern const EmbeddedFile embedded_files[];
extern const int embedded_file_count;

#endif
