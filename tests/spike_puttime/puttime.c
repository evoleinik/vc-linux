/* PutTime, rewritten as plain C. Same output, byte for byte.
 * DOS packs a file time into 16 bits: hhhhh mmmmmm sssss (seconds / 2). */
#include <stdbool.h>
#include <stdint.h>

static char *two_digits(char *out, unsigned n) {
    *out++ = '0' + n / 10;
    *out++ = '0' + n % 10;
    return out;
}

/* Writes the time the way VC's file panel does. Returns the end of the
 * text and leaves a NUL there. h24 = the country uses a 24-hour clock. */
char *put_time(char *out, uint16_t t, bool h24, char sep) {
    unsigned sec = (t & 0x1F) * 2, min = (t >> 5) & 0x3F, hour = t >> 11;
    char ampm = hour < 12 ? 'a' : 'p';

    if (h24)
        *out++ = ' ';
    else if ((hour %= 12) == 0)
        hour = 12;
    out = two_digits(out, hour);
    if (out[-2] == '0')
        out[-2] = ' ';
    *out++ = sep;
    out = two_digits(out, min);
    *out++ = sep;
    out = two_digits(out, sec);
    if (!h24)
        *out++ = ampm;
    *out = '\0';
    return out;
}
