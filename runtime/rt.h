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
#define STUB_INT8_RETURN 0x0120u /* IRET after the BIOS's INT 1Ch call */
#define STUB_END      0x0400u

/* Set by a handler that has already moved CS:IP (and maybe SS:SP) somewhere
 * else, such as EXEC or terminate. The stub then skips its normal return. */
extern int hle_redirect;

extern int rt_exited;
extern int rt_exit_code;

void rt_log(const char *fmt, ...);
void rt_update_clock(void);
void rt_register_image(const Image *img, uint16_t loadseg);
/* A separately generated set of listing-proved entry points may supplement
 * an unchanged image translation. The dispatcher still checks that image's
 * bytes before either runner, including when the guest copies code. */
typedef int (*RtImageRunner)(uint32_t off, uint16_t loadseg);
void rt_register_supplement(const Image *img, RtImageRunner run);
void rt_run(void);
/* Effective PIT channel-2 frequency, or zero when its gate/speaker is off.
 * Linux stays silent; the browser forwards changes to Module.vcSpeaker. */
double rt_speaker_hz(void);

/* Preserve the parent's interrupt vectors and PIT/PIC/speaker state while
 * a DOS child runs. Normal exits discard this snapshot; a forced exit must
 * restore it because the child cannot execute its own cleanup routines. */
typedef struct RtProcessState RtProcessState;
RtProcessState *rt_save_process_state(void);
void rt_finish_process_state(RtProcessState *state, int restore);

/* dos_core.c */
void dos_core_init(void);
int dos_core_int21(void);
/* Execute only the exact kernel-installed entry thunks in the current PSP. */
int dos_run_psp(void);
/* Stop a non-VC child after an untranslated transfer. Return 0 for VC itself,
 * including its overlay child, so the dispatcher keeps its fatal diagnostic. */
int dos_abort_untranslated(void);
/* Ctrl-Break stops only a non-VC child that has not installed INT 1Bh.
 * Return 0 when the dispatcher should deliver the guest's own interrupt. */
int dos_abort_break(void);
/* Handle a software interrupt other than 10h, 16h, 21h and 33h.
 * Returns 1 if the stub should return with a full IRET, 0 for RETF 2. */
int dos_int_other(uint8_t n);
/* Load VC.COM as the first process. host_prog is where it lives on Linux. */
void dos_start(const char *host_prog, const uint8_t *tail, int tail_len);

/* Default setup files and images embedded in the binary: build/gen/files.c */
typedef struct { const char *name; const uint8_t *data; uint32_t size; } EmbeddedFile;
extern const EmbeddedFile embedded_files[];
extern const int embedded_file_count;

#endif
