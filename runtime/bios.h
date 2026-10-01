/* Shared state access for the terminal front end. BIOS entry points are in
 * hle.h; none of these helpers changes the translated program's registers. */
#ifndef VC_BIOS_H
#define VC_BIOS_H

#include <stdint.h>

/* Dimensions come from the guest BDA, bounded to the text-video aperture. */
unsigned bios_columns(void);
unsigned bios_rows(void);

/* CGA pixels live in the guest's interlaced B800h video memory. Zero width
 * means text mode; graphics modes are 320/640 by 200 with raw indices 0..3.
 * The colour selector is also the BIOS's BDA shadow at 0040:0066. */
unsigned bios_graphics_width(void);
uint8_t bios_graphics_pixel(unsigned x, unsigned y);
uint8_t bios_cga_color(unsigned pixel); /* raw index -> RGBI/VGA index 0..15 */
void bios_cga_color_select(uint8_t value);
uint8_t bios_cga_color_register(void);

/* Insert scan:ASCII into the real BDA keyboard ring. A full ring drops keys. */
int bios_key_push(uint16_t key);
int bios_blink_enabled(void);

/* Ctrl-Break is an interrupt, not Ctrl-C text. The terminal latches it;
 * rt_run consumes the request at a dispatch boundary when IF is set. */
void bios_request_break(void);
int bios_take_break(void);
/* A blocking read must unwind to rt_run for INT 1Bh, then retry after the
 * handler. No synthetic keyboard word is returned to the guest. */
int bios_take_read_retry(void);
void bios_cancel_read(void); /* Discard a forcibly terminated child's read. */

/* Terminal coordinates are zero-based character cells; buttons use the
 * INT 33h left/right/middle bitmask. Returns whether the mouse is shown. */
void bios_mouse_event(unsigned column, unsigned row, unsigned buttons);
int bios_mouse_cell(unsigned *column, unsigned *row);

/* Counts interrupts other than 16h. rt.c bumps it; the keyboard poll reads it
 * to tell an idle keyboard spin from a loop that is doing work. */
extern unsigned hle_other_calls;

#endif
