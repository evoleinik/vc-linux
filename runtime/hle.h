/* The DOS and BIOS services, written in C against Linux.
 *
 * Every handler below runs as the body of an interrupt handler. The caller's
 * FLAGS, CS and IP are already on the 8086 stack. A handler reads its inputs
 * from `cpu`, writes results back to `cpu` (registers, cf, zf and so on) and
 * returns. The runtime then returns to the caller the way real DOS does with
 * RETF 2: the live flags are kept, IF is restored from the stacked FLAGS. */
#ifndef VC_HLE_H
#define VC_HLE_H

#include <stddef.h>
#include <stdint.h>

/* ---- DOS file system: runtime/dos_fs.c ---------------------------------- */

void dos_fs_init(void);
/* Track the active PSP job table and FCB owner. Exiting closes only that
 * process's references; inherited files stay open in its parent. */
void dos_fs_set_process(uint16_t psp);
void dos_fs_close_process(uint16_t psp);
/* INT 21h functions for files, directories, drives, handles, date and time,
 * country info and IOCTL, both classic and the 71xxh long-file-name family.
 * Returns 1 if it handled the function in cpu.a.h (or cpu.a.x), 0 if the
 * function is not one of its own. */
int dos_fs_int21(void);
/* Resolve the NUL-terminated DOS path at seg:off, relative to the current
 * drive and directory, to an absolute host path. Returns 0 on success. */
int dos_fs_to_host(uint16_t seg, uint16_t off, char *out, size_t cap);

/* ---- Console, video, keyboard, mouse: runtime/bios.c, runtime/term.c ---- */

void bios_init(void);       /* fill the BIOS data area video and keyboard fields */
void bios_int10(void);      /* video */
void bios_int16(void);      /* keyboard */
void bios_int33(void);      /* mouse */
/* INT 21h console functions 01h-0Ch, plus cooked AH=3Fh CON input delegated
 * by the filesystem after it resolves the handle. Returns 1 if handled. */
int dos_con_int21(void);
/* Write bytes to the CON device: teletype into the text screen at the BIOS
 * cursor, with CR, LF, BS, BEL, DOS TAB stops and scrolling. */
void con_write(const uint8_t *buf, size_t n);

void term_init(void);
void term_shutdown(void);
void term_suspend(void);    /* hand the terminal back for a shell command */
void term_resume(void);     /* take it again and redraw everything */
void term_render(void);     /* draw changed cells of the text screen */
/* Render, then wait up to timeout_ms for terminal input and move any keys
 * into the BIOS keyboard buffer. timeout_ms = 0 polls. */
void term_idle(int timeout_ms);

#endif
