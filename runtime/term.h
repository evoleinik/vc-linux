/* Terminal front end helpers. The runtime entry points stay in hle.h. */
#ifndef VC_TERM_H
#define VC_TERM_H

#include <stddef.h>
#include <stdint.h>

/* A sink makes rendering usable without a tty (and deterministic in tests).
 * NULL restores output to the terminal. The callback consumes data immediately. */
typedef void (*TermOutput)(const char *data, size_t n, void *opaque);
void term_set_output(TermOutput output, void *opaque);
void term_set_truecolor(int enabled);
void term_invalidate(void);
void term_bell(void);

/* Streaming parser entry points. Times are monotonic milliseconds; an Esc is
 * resolved only after 30 ms without another input byte. */
void term_reset_input(void);
/* Drop keys still waiting for room in the BIOS ring. */
void term_clear_pending(void);
void term_flush_input(void);
void term_feed_input(const uint8_t *data, size_t n, uint64_t now_ms);
void term_expire_input(uint64_t now_ms);

#endif
