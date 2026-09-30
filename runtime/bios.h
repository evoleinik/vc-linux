/* Shared state access for the terminal front end. BIOS entry points are in
 * hle.h; none of these helpers changes the translated program's registers. */
#ifndef VC_BIOS_H
#define VC_BIOS_H

#include <stdint.h>

/* Dimensions come from the guest BDA, bounded to the text-video aperture. */
unsigned bios_columns(void);
unsigned bios_rows(void);

/* Insert scan:ASCII into the real BDA keyboard ring. A full ring drops keys. */
int bios_key_push(uint16_t key);
int bios_blink_enabled(void);

/* Terminal coordinates are zero-based character cells; buttons use the
 * INT 33h left/right/middle bitmask. Returns whether the mouse is shown. */
void bios_mouse_event(unsigned column, unsigned row, unsigned buttons);
int bios_mouse_cell(unsigned *column, unsigned *row);

/* Counts interrupts other than 16h. rt.c bumps it; the keyboard poll reads it
 * to tell an idle keyboard spin from a loop that is doing work. */
extern unsigned hle_other_calls;

#endif
