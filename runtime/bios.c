/* Text/CGA BIOS and DOS CON, sharing the actual BDA and video memory with
 * translated code. No private queue or screen buffer can go stale when VC
 * updates either region directly. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "bios.h"
#include "cpu.h"
#include "hle.h"
#include "term.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* The upstream public-domain bitmaps use plain char for byte constants.
 * Keep their files unchanged and suppress only that signed-char diagnostic;
 * glyph reads below explicitly convert back to uint8_t before shifting. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#endif
#include "../third_party/font8x8/font8x8_basic.h"
#include "../third_party/font8x8/font8x8_box.h"
#include "../third_party/font8x8/font8x8_block.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum {
    BDA = 0x40,
    SCREEN = 0xb8000,
    SCREEN_BYTES = 0x8000,
    CGA_BYTES = 0x4000,
    CGA_HEIGHT = 200,
    KEY_START = 0x1e,
    KEY_END = 0x3e
};

static int blink_enabled;
static int console_scan_pending;
static uint8_t console_scan;
static int break_pending;
static int read_retry;
static struct {
    uint16_t ss, sp, segment, offset;
    unsigned count;
    uint8_t widths[255];
    int active;
} line_read;

typedef struct {
    int show;
    unsigned x, y, xmin, xmax, ymin, ymax, buttons;
    uint64_t pressed_ms[3]; /* when each button last went down */
    uint16_t dx, dy;
    uint16_t presses[3], releases[3];
    uint16_t press_x[3], press_y[3], release_x[3], release_y[3];
} Mouse;

static Mouse mouse;

static uint16_t bda_word(unsigned offset)
{
    return rd16(BDA, (uint16_t)offset);
}

static void bda_set_word(unsigned offset, uint16_t value)
{
    wr16(BDA, (uint16_t)offset, value);
}

unsigned bios_columns(void)
{
    unsigned columns = bda_word(0x4a);
    if (columns == 0)
        columns = 80;
    return columns > 256 ? 256 : columns;
}

unsigned bios_rows(void)
{
    unsigned rows = (unsigned)mem[0x484] + 1;
    unsigned maximum = SCREEN_BYTES / (2 * bios_columns());
    return rows > maximum ? maximum : rows;
}

unsigned bios_graphics_width(void)
{
    unsigned mode = mem[0x449];
    return mode == 6 ? 640 : (mode == 4 || mode == 5 ? 320 : 0);
}

uint8_t bios_cga_color_register(void)
{
    return mem[0x466];
}

void bios_cga_color_select(uint8_t value)
{
    mem[0x466] = value;
    term_invalidate();
}

uint8_t bios_cga_color(unsigned pixel)
{
    uint8_t select = bios_cga_color_register();
    if (mem[0x449] == 6)
        return (pixel & 1) ? select & 15 : 0;
    pixel &= 3;
    if (pixel == 0)
        return select & 15;
    unsigned intensity = (select & 0x10) >> 1;
    /* Disabling the colour burst (mode 5) selects cyan/red/white on an RGBI
     * display, independently of the palette-select bit. Composite artefact
     * colours and scanline palette changes are not emulated. */
    static const uint8_t palettes[3][3] = {
        {2, 4, 6}, {3, 5, 7}, {3, 4, 7}
    };
    unsigned palette = mem[0x449] == 5 ? 2 : (select >> 5) & 1;
    return (uint8_t)(palettes[palette][pixel - 1] | intensity);
}

static size_t cga_address(unsigned x, unsigned y)
{
    unsigned pixels_per_byte = bios_graphics_width() == 640 ? 8 : 4;
    return SCREEN + (y & 1) * 0x2000 + (y >> 1) * 80 + x / pixels_per_byte;
}

uint8_t bios_graphics_pixel(unsigned x, unsigned y)
{
    unsigned width = bios_graphics_width();
    if (x >= width || y >= CGA_HEIGHT)
        return 0;
    unsigned shift = width == 640 ? 7 - (x & 7) : 6 - 2 * (x & 3);
    unsigned mask = width == 640 ? 1 : 3;
    return (uint8_t)((mem[cga_address(x, y)] >> shift) & mask);
}

static void graphics_pixel(unsigned x, unsigned y, uint8_t color)
{
    unsigned width = bios_graphics_width();
    if (x >= width || y >= CGA_HEIGHT)
        return;
    unsigned shift = width == 640 ? 7 - (x & 7) : 6 - 2 * (x & 3);
    unsigned mask = width == 640 ? 1 : 3;
    size_t address = cga_address(x, y);
    uint8_t bits = (uint8_t)((color & mask) << shift);
    if (color & 0x80)
        mem[address] ^= bits;
    else
        mem[address] = (uint8_t)((mem[address] & ~(mask << shift)) | bits);
}

static const char *graphics_glyph(uint8_t ch)
{
    /* CP437 and CP866 share the complete B0h-DFh box/block range. The
     * remaining non-ASCII characters have no supplied font and stay blank. */
    static const uint16_t boxes[48] = {
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
        0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
        0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
        0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580
    };
    if (ch < 128)
        return font8x8_basic[ch];
    if (ch >= 0xb0 && ch <= 0xdf) {
        unsigned codepoint = boxes[ch - 0xb0];
        return codepoint < 0x2580 ? font8x8_box[codepoint - 0x2500] :
                                 font8x8_block[codepoint - 0x2580];
    }
    return font8x8_basic[' '];
}

static void graphics_character(unsigned column, unsigned row,
                               uint8_t ch, uint8_t color)
{
    const char *glyph = graphics_glyph(ch);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            /* Upstream font bits run left-to-right, unlike CGA byte bits. */
            uint8_t ink = ((uint8_t)glyph[y] >> x) & 1 ? color : color & 0x80;
            graphics_pixel(column * 8 + x, row * 8 + y, ink);
        }
}

static uint8_t graphics_read_character(unsigned column, unsigned row)
{
    uint8_t bitmap[8] = {0};
    /* The CGA BIOS compares the eight scanlines against its font, not a
     * remembered character. GW-BASIC's editor reads text back this way and
     * guest programs may have changed any of these pixels directly. */
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            if (bios_graphics_pixel(column * 8 + x, row * 8 + y))
                bitmap[y] |= (uint8_t)(1u << x);
    /* Prefer space to the many blank control/unsupported glyphs. */
    for (unsigned ch = 32; ch < 127; ++ch)
        if (!memcmp(bitmap, graphics_glyph((uint8_t)ch), sizeof(bitmap)))
            return (uint8_t)ch;
    for (unsigned ch = 0xb0; ch <= 0xdf; ++ch)
        if (!memcmp(bitmap, graphics_glyph((uint8_t)ch), sizeof(bitmap)))
            return (uint8_t)ch;
    return 0; /* No matching glyph, as with arbitrary turtle graphics. */
}

static size_t screen_cell(unsigned column, unsigned row)
{
    return SCREEN + 2 * ((size_t)row * bios_columns() + column);
}

static void fill_cells(size_t first, size_t count, uint8_t attribute)
{
    for (size_t i = 0; i < count; ++i) {
        mem[first + 2 * i] = ' ';
        mem[first + 2 * i + 1] = attribute;
    }
}

static void font_geometry(unsigned rows, unsigned height)
{
    unsigned bytes = bios_columns() * rows * 2;
    mem[0x484] = (uint8_t)(rows - 1);
    bda_set_word(0x85, (uint16_t)height);
    bda_set_word(0x4c, (uint16_t)((bytes + 0xfffu) & ~0xfffu));
    bda_set_word(0x60, height == 8 ? 0x0607 :
                 (uint16_t)(((height - 3) << 8) | (height - 2)));
    term_invalidate();
}

static void mode_text(unsigned mode, int preserve_screen)
{
    unsigned columns = mode < 2 ? 40 : 80;
    mem[0x449] = (uint8_t)mode;
    bda_set_word(0x4a, (uint16_t)columns);
    bda_set_word(0x4e, 0);
    for (unsigned page = 0; page < 8; ++page)
        bda_set_word(0x50 + page * 2, 0);
    mem[0x462] = 0;
    bda_set_word(0x63, 0x3d4);
    mem[0x487] = (uint8_t)(0x60 | (preserve_screen ? 0x80 : 0));
    mem[0x488] = 0xf9;
    mem[0x489] = 0x51;
    mem[0x465] = (uint8_t)(0x28 | (mode >= 2 ? 1 : 0) | (mode & 1 ? 0 : 4));
    mem[0x466] = 0x30;
    blink_enabled = 0; /* VC's bright backgrounds are the default. */
    font_geometry(25, 16);
    if (columns == 40)
        bda_set_word(0x4c, 0x0800);
    if (!preserve_screen)
        fill_cells(SCREEN, SCREEN_BYTES / 2, 0x07);
}

static void mode_graphics(unsigned mode, int preserve_screen)
{
    mem[0x449] = (uint8_t)mode;
    bda_set_word(0x4a, mode == 6 ? 80 : 40);
    bda_set_word(0x4c, CGA_BYTES);
    bda_set_word(0x4e, 0);
    for (unsigned page = 0; page < 8; ++page)
        bda_set_word(0x50 + page * 2, 0);
    bda_set_word(0x60, 0x0607);
    mem[0x462] = 0;
    bda_set_word(0x63, 0x3d4);
    mem[0x465] = (uint8_t)(mode == 4 ? 0x2a : mode == 5 ? 0x2e : 0x1e);
    mem[0x466] = (uint8_t)(mode == 6 ? 0x3f : 0x30);
    mem[0x484] = 24;
    bda_set_word(0x85, 8);
    mem[0x487] = (uint8_t)(0x60 | (preserve_screen ? 0x80 : 0));
    blink_enabled = 0;
    if (!preserve_screen)
        memset(mem + SCREEN, 0, CGA_BYTES);
    term_invalidate();
}

static void mouse_reset(void)
{
    memset(&mouse, 0, sizeof(mouse));
    mouse.show = -1;
    mouse.xmax = bios_columns() * 8 - 1;
    mouse.ymax = bios_rows() * 8 - 1;
}

unsigned hle_other_calls;

void bios_init(void)
{
    bda_set_word(0x00, 0x03f8); /* One 8250 serial port: COM1. */
    bda_set_word(0x02, 0);
    bda_set_word(0x04, 0);
    bda_set_word(0x06, 0);
    bda_set_word(0x10, 0x0220); /* 80-column colour, one serial port. */
    mem[0x47c] = 1; /* BIOS serial timeout in seconds. */
    mode_text(3, 0);
    mem[0x417] = 0;
    mem[0x418] = 0;
    mem[0x496] = 0x10; /* Enhanced keyboard installed; right modifiers clear. */
    bda_set_word(0x1a, KEY_START);
    bda_set_word(0x1c, KEY_START);
    bda_set_word(0x80, KEY_START);
    bda_set_word(0x82, KEY_END);
    memset(mem + 0x400 + KEY_START, 0, KEY_END - KEY_START);
    console_scan_pending = 0;
    console_scan = 0;
    break_pending = 0;
    read_retry = 0;
    line_read.active = 0;
    mem[0x471] = 0;
    mouse_reset();
    /* The runtime owns the timer at 046Ch: it is deliberately untouched. */
}

int bios_blink_enabled(void)
{
    return blink_enabled;
}

/* A guest may clear or advance the ring by writing its BDA pointers itself.
 * Honour valid pointers, including a smaller ring, but never follow malformed
 * offsets into arbitrary guest memory. */
static void key_bounds(unsigned *start, unsigned *end)
{
    unsigned first = bda_word(0x80), limit = bda_word(0x82);
    unsigned head = bda_word(0x1a), tail = bda_word(0x1c);
    if (first < KEY_START || limit > KEY_END || limit < first + 4 ||
        ((first | limit) & 1) || head < first || head >= limit ||
        tail < first || tail >= limit || ((head | tail) & 1)) {
        first = KEY_START;
        limit = KEY_END;
        bda_set_word(0x80, (uint16_t)first);
        bda_set_word(0x82, (uint16_t)limit);
        bda_set_word(0x1a, (uint16_t)first);
        bda_set_word(0x1c, (uint16_t)first);
    }
    *start = first;
    *end = limit;
}

static unsigned key_advance(unsigned offset, unsigned start, unsigned end)
{
    return offset + 2 >= end ? start : offset + 2;
}

int bios_key_push(uint16_t key)
{
    unsigned start, end;
    key_bounds(&start, &end);
    unsigned tail = bda_word(0x1c);
    unsigned next = key_advance(tail, start, end);
    if (next == bda_word(0x1a))
        return 0;
    bda_set_word(tail, key);
    bda_set_word(0x1c, (uint16_t)next);
    return 1;
}

void bios_request_break(void)
{
    /* Discard old typeahead, but do not turn the interrupt into a NUL key.
     * A blocked read explicitly unwinds to the dispatcher below instead. */
    unsigned start, end;
    key_bounds(&start, &end);
    bda_set_word(0x1c, bda_word(0x1a));
    console_scan_pending = 0;
    mem[0x471] |= 0x80;
    break_pending = 1;
}

int bios_take_break(void)
{
    int pending = break_pending;
    break_pending = 0;
    return pending;
}

int bios_take_read_retry(void)
{
    int retry = read_retry;
    read_retry = 0;
    return retry;
}

void bios_cancel_read(void)
{
    read_retry = 0;
    console_scan_pending = 0;
    line_read.active = 0;
}

static int legacy_key(uint16_t *key)
{
    unsigned scan = *key >> 8, ascii = *key & 0xff;
    if (scan == 0xe0) {
        if (ascii == 0x0d || ascii == 0x0a)
            *key = (uint16_t)(0x1c00 | ascii); /* Enhanced keypad Enter. */
        else if (ascii == '/')
            *key = 0x352f;
        else
            return 0;
    } else if (scan >= 0x85 || (scan != 0 && ascii == 0xf0)) {
        return 0; /* F11/F12 and the new enhanced-keyboard combinations. */
    } else if (scan != 0 && ascii == 0xe0) {
        *key &= 0xff00; /* Do not change scan-zero CP866 character E0h. */
    }
    return 1;
}

static int key_read(int extended, int remove, uint16_t *key)
{
    unsigned start, end;
    key_bounds(&start, &end);
    unsigned head = bda_word(0x1a);
    while (head != bda_word(0x1c)) {
        uint16_t value = bda_word(head);
        unsigned next = key_advance(head, start, end);
        if (extended || legacy_key(&value)) {
            if (remove)
                bda_set_word(0x1a, (uint16_t)next);
            *key = value;
            return 1;
        }
        /* Legacy peek must discard enhanced-only entries too, or the
         * unreadable entry would permanently block every following key. */
        bda_set_word(0x1a, (uint16_t)next);
        head = next;
    }
    return 0;
}

static int key_wait(int extended, uint16_t *key)
{
    for (;;) {
        if (break_pending) {
            read_retry = 1;
            return 0;
        }
        if (key_read(extended, 1, key))
            return 1;
        term_idle(50);
    }
}

void bios_int16(void)
{
    uint16_t key;
    unsigned function = cpu.a.h;
    switch (function) {
    case 0x00:
    case 0x10:
        if (!key_wait(function == 0x10, &key))
            return;
        cpu.a.x = key;
        break;
    case 0x01:
    case 0x11:
        if (!key_read(function == 0x11, 0, &key)) {
            /* Programs also poll here between units of real work, as VC's
             * tree scan does per directory entry, so an empty poll must not
             * sleep. Idle loops say so through INT 28h or INT 2Fh AX=1680h,
             * which sleep instead. Only a loop that does nothing but poll
             * the keyboard, with no other interrupt in between, is throttled. */
            static unsigned last_other, spins;
            int wait = 0;
            if (hle_other_calls != last_other) {
                last_other = hle_other_calls;
                spins = 0;
            } else if (++spins > 200) {
                wait = 10;
            }
            term_idle(wait);
            if (!key_read(function == 0x11, 0, &key)) {
                cpu.zf = 1;
                break;
            }
        }
        cpu.zf = 0;
        cpu.a.x = key;
        break;
    case 0x02:
        cpu.a.l = mem[0x417];
        break;
    case 0x05:
        cpu.a.l = (uint8_t)!bios_key_push(cpu.c.x);
        break;
    case 0x12:
        cpu.a.l = mem[0x417];
        cpu.a.h = (uint8_t)((mem[0x418] & 0x73) |
                          ((mem[0x418] & 0x04) << 5) |
                          (mem[0x496] & 0x0c));
        break;
    default:
        break;
    }
}

static void scroll_window(int down, unsigned count, uint8_t attribute,
                          unsigned left, unsigned top,
                          unsigned right, unsigned bottom)
{
    unsigned columns = bios_columns(), rows = bios_rows();
    if (left >= columns || top >= rows || left > right || top > bottom)
        return;
    if (right >= columns)
        right = columns - 1;
    if (bottom >= rows)
        bottom = rows - 1;
    unsigned height = bottom - top + 1, width = right - left + 1;
    if (count == 0 || count >= height)
        count = height;
    if (bios_graphics_width()) {
        unsigned first = top * 8, limit = (bottom + 1) * 8, shift = count * 8;
        unsigned bytes = width * (bios_graphics_width() == 640 ? 1 : 2);
        uint8_t fill = bios_graphics_width() == 640 ?
                       ((attribute & 1) ? 0xff : 0) : (attribute & 3) * 0x55;
        /* Copy whole scanlines in direction order, keeping the two physical
         * banks interlaced. Both ends are aligned to 8-pixel character cells. */
        if (down) {
            for (unsigned y = limit; y-- > first + shift;)
                memmove(mem + cga_address(left * 8, y),
                        mem + cga_address(left * 8, y - shift), bytes);
            for (unsigned y = first; y < first + shift; ++y)
                memset(mem + cga_address(left * 8, y), fill, bytes);
        } else {
            for (unsigned y = first; y + shift < limit; ++y)
                memmove(mem + cga_address(left * 8, y),
                        mem + cga_address(left * 8, y + shift), bytes);
            for (unsigned y = limit - shift; y < limit; ++y)
                memset(mem + cga_address(left * 8, y), fill, bytes);
        }
        return;
    }
    if (down) {
        for (unsigned row = bottom + 1; row-- > top + count;)
            memmove(mem + screen_cell(left, row),
                    mem + screen_cell(left, row - count), width * 2);
        for (unsigned row = top; row < top + count; ++row)
            fill_cells(screen_cell(left, row), width, attribute);
    } else {
        for (unsigned row = top; row + count <= bottom; ++row)
            memmove(mem + screen_cell(left, row),
                    mem + screen_cell(left, row + count), width * 2);
        for (unsigned row = bottom + 1 - count; row <= bottom; ++row)
            fill_cells(screen_cell(left, row), width, attribute);
    }
}

static int cursor_position(unsigned *column, unsigned *row)
{
    uint16_t position = bda_word(0x50);
    *column = position & 0xff;
    *row = position >> 8;
    return *column < bios_columns() && *row < bios_rows();
}

static void cursor_set(unsigned column, unsigned row)
{
    bda_set_word(0x50, (uint16_t)((row << 8) | column));
}

static void teletype(uint8_t ch, uint8_t color)
{
    unsigned column, row, columns = bios_columns(), rows = bios_rows();
    (void)cursor_position(&column, &row);
    if (column >= columns)
        column = columns - 1;
    if (row >= rows)
        row = rows - 1;
    switch (ch) {
    case '\a':
        term_bell();
        return;
    case '\r':
        column = 0;
        break;
    case '\n':
        ++row;
        break;
    case '\b':
        if (column != 0)
            --column;
        break;
    default:
        if (bios_graphics_width())
            graphics_character(column, row, ch, color);
        else
            /* Text-mode BIOS TTY preserves the destination cell's attribute. */
            mem[screen_cell(column, row)] = ch;
        if (++column == columns) {
            column = 0;
            ++row;
        }
        break;
    }
    if (row >= rows) {
        scroll_window(0, 1, bios_graphics_width() ? 0 : 0x07,
                      0, 0, columns - 1, rows - 1);
        row = rows - 1;
    }
    cursor_set(column, row);
}

void con_write(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        teletype(buf[i], 3);
}

void bios_int10(void)
{
    unsigned column, row;
    switch (cpu.a.h) {
    case 0x00:
        if ((cpu.a.l & 0x7f) <= 3)
            mode_text(cpu.a.l & 0x7f, (cpu.a.l & 0x80) != 0);
        else if ((cpu.a.l & 0x7f) >= 4 && (cpu.a.l & 0x7f) <= 6)
            mode_graphics(cpu.a.l & 0x7f, (cpu.a.l & 0x80) != 0);
        break;
    case 0x01:
        bda_set_word(0x60, cpu.c.x);
        break;
    case 0x02:
        if (cpu.b.h < 8)
            bda_set_word(0x50 + cpu.b.h * 2, cpu.d.x);
        /* Off-screen positions deliberately survive: VC uses row 127 to
         * hide the cursor without changing its shape. */
        break;
    case 0x03:
        cpu.c.x = bda_word(0x60);
        if (cpu.b.h < 8)
            cpu.d.x = bda_word(0x50 + cpu.b.h * 2);
        break;
    case 0x05:
        if (cpu.a.l == 0) {
            mem[0x462] = 0;
            bda_set_word(0x4e, 0);
        }
        break;
    case 0x06:
    case 0x07:
        scroll_window(cpu.a.h == 7, cpu.a.l, cpu.b.h,
                      cpu.c.l, cpu.c.h, cpu.d.l, cpu.d.h);
        break;
    case 0x08:
        if (cpu.b.h == 0 && cursor_position(&column, &row)) {
            if (bios_graphics_width())
                cpu.a.x = graphics_read_character(column, row);
            else {
                size_t address = screen_cell(column, row);
                cpu.a.x = (uint16_t)(mem[address] | (mem[address + 1] << 8));
            }
        }
        break;
    case 0x09:
    case 0x0a:
        if (cpu.b.h == 0 && cursor_position(&column, &row)) {
            if (bios_graphics_width()) {
                unsigned count = cpu.c.x;
                while (count-- != 0 && row < bios_rows()) {
                    graphics_character(column, row, cpu.a.l, cpu.b.l);
                    if (++column == bios_columns()) {
                        column = 0;
                        ++row;
                    }
                }
                break;
            }
            size_t address = screen_cell(column, row);
            size_t limit = screen_cell(0, bios_rows());
            unsigned count = cpu.c.x;
            while (count-- != 0 && address < limit) {
                mem[address] = cpu.a.l;
                if (cpu.a.h == 0x09)
                    mem[address + 1] = cpu.b.l;
                address += 2;
            }
        }
        break;
    case 0x0b: {
        uint8_t select = bios_cga_color_register();
        if (cpu.b.h == 0)
            bios_cga_color_select((uint8_t)((select & 0xe0) | (cpu.b.l & 0x1f)));
        else if (cpu.b.h == 1)
            bios_cga_color_select((uint8_t)((select & 0xdf) | ((cpu.b.l & 1) << 5)));
        break;
    }
    case 0x0c:
        graphics_pixel(cpu.c.x, cpu.d.x, cpu.a.l);
        break;
    case 0x0d:
        cpu.a.l = bios_graphics_pixel(cpu.c.x, cpu.d.x);
        break;
    case 0x0e:
        teletype(cpu.a.l, cpu.b.l);
        break;
    case 0x0f:
        cpu.a.l = mem[0x449];
        cpu.a.h = (uint8_t)bios_columns();
        cpu.b.h = mem[0x462];
        break;
    case 0x10:
        if (cpu.a.l == 3) {
            blink_enabled = cpu.b.l != 0;
            term_invalidate();
        }
        /* Other palette subfunctions have no effect on the fixed VGA map. */
        break;
    case 0x11:
        switch (cpu.a.l) {
        case 0x11: font_geometry(28, 14); break;
        case 0x12: font_geometry(50, 8); break;
        case 0x14: font_geometry(25, 16); break;
        case 0x30:
            cpu.c.x = bda_word(0x85);
            cpu.d.l = (uint8_t)(bios_rows() - 1);
            break;
        default: break;
        }
        break;
    case 0x12:
        if (cpu.b.l == 0x10) {
            cpu.b.h = 0; /* Colour display. */
            cpu.b.l = 3; /* 256 KiB VGA memory. */
            cpu.c.x = 0x0009;
        }
        break;
    case 0x1a:
        if (cpu.a.l == 0) {
            cpu.a.l = 0x1a;
            cpu.b.x = 0x0008; /* VGA colour, no second display. */
        }
        break;
    case 0x4f:
        cpu.a.x = 0x014f; /* VESA request failed, never claim success. */
        break;
    case 0xfe:
    case 0xff:
        /* No TopView shadow buffer: ES:DI and every other register stay put. */
        break;
    default:
        break;
    }
}

static unsigned bounded_coordinate(unsigned value, unsigned low, unsigned high)
{
    if (value < low)
        value = low;
    if (value > high)
        value = high;
    return value & ~7u; /* INT 33h text coordinates are character-cell aligned. */
}

static uint64_t mouse_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* A terminal may deliver a click's press and release in one read. VC polls
 * the buttons, several times per pass of its input loop, so a button is
 * reported down for at least as long as a physical click lasts. */
enum { MIN_CLICK_MS = 80 };
static unsigned mouse_held(void)
{
    unsigned held = mouse.buttons;
    uint64_t now = mouse_now_ms();
    for (unsigned i = 0; i < 3; ++i)
        if (mouse.presses[i] && now - mouse.pressed_ms[i] < MIN_CLICK_MS)
            held |= 1u << i;
    return held;
}

void bios_mouse_event(unsigned column, unsigned row, unsigned buttons)
{
    if (column >= bios_columns())
        column = bios_columns() - 1;
    if (row >= bios_rows())
        row = bios_rows() - 1;
    unsigned x = bounded_coordinate(column * 8, mouse.xmin, mouse.xmax);
    unsigned y = bounded_coordinate(row * 8, mouse.ymin, mouse.ymax);
    /* A terminal supplies cells, not hardware mickeys: motion is approximated
     * as eight units per visible cell and cannot measure sub-cell movement. */
    mouse.dx = (uint16_t)(mouse.dx + (int)x - (int)mouse.x);
    mouse.dy = (uint16_t)(mouse.dy + (int)y - (int)mouse.y);
    mouse.x = x;
    mouse.y = y;
    buttons &= 7;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned mask = 1u << i;
        if ((buttons & mask) != 0 && (mouse.buttons & mask) == 0) {
            ++mouse.presses[i];
            mouse.pressed_ms[i] = mouse_now_ms();
            mouse.press_x[i] = (uint16_t)x;
            mouse.press_y[i] = (uint16_t)y;
        } else if ((buttons & mask) == 0 && (mouse.buttons & mask) != 0) {
            ++mouse.releases[i];
            mouse.release_x[i] = (uint16_t)x;
            mouse.release_y[i] = (uint16_t)y;
        }
    }
    mouse.buttons = buttons;
}

int bios_mouse_cell(unsigned *column, unsigned *row)
{
    *column = mouse.x / 8;
    *row = mouse.y / 8;
    return mouse.show >= 0 && *column < bios_columns() && *row < bios_rows();
}

void bios_int33(void)
{
    unsigned function = cpu.a.x;
    switch (function) {
    case 0x00:
    case 0x21:
        mouse_reset();
        cpu.a.x = 0xffff;
        cpu.b.x = 2;
        break;
    case 0x01:
        if (mouse.show < INT_MAX)
            ++mouse.show;
        break;
    case 0x02:
        if (mouse.show > INT_MIN)
            --mouse.show;
        break;
    case 0x03:
        cpu.b.x = (uint16_t)mouse_held();
        cpu.c.x = (uint16_t)mouse.x;
        cpu.d.x = (uint16_t)mouse.y;
        break;
    case 0x04:
        /* A software warp is not physical motion and does not add mickeys. */
        mouse.x = bounded_coordinate(cpu.c.x, mouse.xmin, mouse.xmax);
        mouse.y = bounded_coordinate(cpu.d.x, mouse.ymin, mouse.ymax);
        break;
    case 0x05:
    case 0x06: {
        unsigned button = cpu.b.x;
        cpu.a.x = (uint16_t)mouse.buttons;
        cpu.b.x = 0;
        cpu.c.x = (uint16_t)mouse.x;
        cpu.d.x = (uint16_t)mouse.y;
        if (button < 3) {
            if (function == 5) {
                cpu.b.x = mouse.presses[button];
                mouse.presses[button] = 0;
                cpu.c.x = mouse.press_x[button];
                cpu.d.x = mouse.press_y[button];
            } else {
                cpu.b.x = mouse.releases[button];
                mouse.releases[button] = 0;
                cpu.c.x = mouse.release_x[button];
                cpu.d.x = mouse.release_y[button];
            }
        }
        break;
    }
    case 0x07:
    case 0x08: {
        unsigned low = cpu.c.x, high = cpu.d.x;
        if (low > high) {
            unsigned temporary = low;
            low = high;
            high = temporary;
        }
        if (function == 7) {
            mouse.xmin = low;
            mouse.xmax = high;
            mouse.x = bounded_coordinate(mouse.x, low, high);
        } else {
            mouse.ymin = low;
            mouse.ymax = high;
            mouse.y = bounded_coordinate(mouse.y, low, high);
        }
        break;
    }
    case 0x0b:
        cpu.c.x = mouse.dx;
        cpu.d.x = mouse.dy;
        mouse.dx = mouse.dy = 0;
        break;
    case 0x0a:
        /* A fixed inverted-attribute text cursor replaces user cursor masks. */
        break;
    case 0x0c:
    case 0x14:
        /* Event-handler installation/exchange is intentionally unsupported:
         * this polling front end does not invoke real-mode mouse callbacks. */
        break;
    default:
        break;
    }
}

static int console_get(int blocking, uint8_t *ch, int *extended)
{
    if (console_scan_pending) {
        console_scan_pending = 0;
        *ch = console_scan;
        *extended = 1;
        return 1;
    }
    uint16_t key;
    if (blocking) {
        if (!key_wait(1, &key))
            return 0;
    } else if (!key_read(1, 1, &key)) {
        term_idle(0);
        if (!key_read(1, 1, &key))
            return 0;
    }
    unsigned ascii = key & 0xff, scan = key >> 8;
    *extended = ascii == 0 || (ascii == 0xe0 && scan != 0);
    if (*extended) {
        console_scan = (uint8_t)scan;
        console_scan_pending = 1;
        *ch = 0;
    } else {
        *ch = (uint8_t)ascii;
    }
    return 1;
}

static void console_put(uint8_t ch)
{
    if (ch == '\t') {
        unsigned count = 8 - (mem[0x450] & 7);
        while (count-- != 0)
            teletype(' ', 3);
    } else {
        teletype(ch, 3);
    }
}

/* Erase an edited character even after it wrapped to the following row.
 * Text which has already scrolled off the top cannot be recovered. */
static void console_erase(unsigned cells)
{
    unsigned column, row;
    if (!cursor_position(&column, &row))
        return;
    while (cells-- != 0) {
        if (column != 0) {
            --column;
        } else if (row != 0) {
            column = bios_columns() - 1;
            --row;
        } else {
            break;
        }
        if (bios_graphics_width())
            graphics_character(column, row, ' ', 3);
        else
            mem[screen_cell(column, row)] = ' ';
    }
    cursor_set(column, row);
}

static void console_line(void)
{
    uint16_t segment = cpu.ds, offset = cpu.d.x;
    unsigned maximum = rd8(segment, offset);
    if (maximum == 0)
        return;
    /* Preserve an edited line across the dispatcher round trip to INT 1Bh.
     * The stack and buffer identify the interrupted DOS call; a later call
     * must not accidentally resume another program's editing state. */
    if (!line_read.active || line_read.ss != cpu.ss || line_read.sp != cpu.sp ||
        line_read.segment != segment || line_read.offset != offset) {
        line_read.ss = cpu.ss;
        line_read.sp = cpu.sp;
        line_read.segment = segment;
        line_read.offset = offset;
        line_read.count = 0;
        line_read.active = 1;
    }
    for (;;) {
        uint8_t ch;
        int extended;
        if (!console_get(1, &ch, &extended))
            return;
        if (extended)
            continue; /* Both halves of an extended key are editing no-ops. */
        if (ch == '\r') {
            wr8(segment, (uint16_t)(offset + 1), (uint8_t)line_read.count);
            wr8(segment, (uint16_t)(offset + 2 + line_read.count), '\r');
            console_put('\r');
            console_put('\n');
            line_read.active = 0;
            return;
        }
        if (ch == '\b') {
            if (line_read.count != 0)
                console_erase(line_read.widths[--line_read.count]);
        } else if (ch == 0x1b) {
            while (line_read.count != 0)
                console_erase(line_read.widths[--line_read.count]);
        } else if (ch >= ' ' || ch == '\t') {
            if (line_read.count + 1 < maximum) {
                wr8(segment, (uint16_t)(offset + 2 + line_read.count), ch);
                line_read.widths[line_read.count++] = ch == '\t' ?
                    (uint8_t)(8 - (mem[0x450] & 7)) : 1;
                console_put(ch);
            } else {
                term_bell();
            }
        }
    }
}

int dos_con_int21(void)
{
    uint8_t ch;
    int extended;
    switch (cpu.a.h) {
    case 0x01:
        if (!console_get(1, &ch, &extended))
            return 1;
        cpu.a.l = ch;
        if (!extended)
            console_put(ch);
        return 1;
    case 0x02:
        console_put(cpu.d.l);
        cpu.a.l = cpu.d.l == '\t' ? ' ' : cpu.d.l;
        return 1;
    case 0x06:
        if (cpu.d.l != 0xff) {
            console_put(cpu.d.l);
            cpu.a.l = cpu.d.l;
        } else if (console_get(0, &ch, &extended)) {
            cpu.a.l = ch;
            cpu.zf = 0;
        } else {
            cpu.a.l = 0;
            cpu.zf = 1;
        }
        return 1;
    case 0x07:
    case 0x08:
        /* Ctrl-C remains a byte: this layer does not dispatch DOS INT 23h. */
        if (!console_get(1, &ch, &extended))
            return 1;
        cpu.a.l = ch;
        return 1;
    case 0x09:
        /* Cap a missing '$' terminator at one offset-space traversal. */
        for (unsigned i = 0; i < 0x10000; ++i) {
            ch = rd8(cpu.ds, (uint16_t)(cpu.d.x + i));
            if (ch == '$')
                break;
            console_put(ch);
        }
        cpu.a.l = '$';
        return 1;
    case 0x0a:
        console_line();
        return 1;
    case 0x0b: {
        uint16_t key;
        if (!console_scan_pending && !key_read(1, 0, &key))
            term_idle(0);
        cpu.a.l = console_scan_pending || key_read(1, 0, &key) ? 0xff : 0;
        return 1;
    }
    case 0x0c: {
        unsigned function = cpu.a.l;
        unsigned start, end;
        term_flush_input();
        term_clear_pending();
        key_bounds(&start, &end);
        bda_set_word(0x1a, bda_word(0x1c));
        console_scan_pending = 0;
        if (function == 1 || function == 6 || function == 7 ||
            function == 8 || function == 0x0a) {
            cpu.a.h = (uint8_t)function;
            (void)dos_con_int21();
        }
        return 1;
    }
    default:
        return 0;
    }
}
