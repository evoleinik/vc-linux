/* Code page 866 character conversion shared by DOS paths and the terminal. */
#ifndef VC_CP866_H
#define VC_CP866_H

#include <stdint.h>

/* ASCII, including its control characters, maps to the same Unicode value. */
uint32_t cp866_to_ucs(uint8_t ch);

/* Return the CP866 byte, or -1 if the Unicode code point is not representable. */
int ucs_to_cp866(uint32_t ch);

/* Locale-independent uppercase for ASCII and all CP866 Cyrillic letters. */
uint8_t cp866_upper(uint8_t ch);

#endif
