/* Text-mode BIOS and DOS CON, sharing the actual BDA and video memory with
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

enum {
    BDA = 0x40,
    SCREEN = 0xb8000,
    SCREEN_BYTES = 0x8000,
    KEY_START = 0x1e,
    KEY_END = 0x3e
};

static int blink_enabled;
static int console_scan_pending;
static uint8_t console_scan;

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

static void mode_three(int preserve_screen)
{
    mem[0x449] = 3;
    bda_set_word(0x4a, 80);
    bda_set_word(0x4e, 0);
    for (unsigned page = 0; page < 8; ++page)
        bda_set_word(0x50 + page * 2, 0);
    mem[0x462] = 0;
    bda_set_word(0x63, 0x3d4);
    mem[0x487] = (uint8_t)(0x60 | (preserve_screen ? 0x80 : 0));
    mem[0x488] = 0xf9;
    mem[0x489] = 0x51;
    blink_enabled = 0; /* VC's bright backgrounds are the default. */
    font_geometry(25, 16);
    if (!preserve_screen)
        fill_cells(SCREEN, SCREEN_BYTES / 2, 0x07);
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
    bda_set_word(0x10, 0x0020); /* 80-column colour, no invented peripherals. */
    mode_three(0);
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

void bios_int16(void)
{
    uint16_t key;
    unsigned function = cpu.a.h;
    switch (function) {
    case 0x00:
    case 0x10:
        while (!key_read(function == 0x10, 1, &key))
            term_idle(50);
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

static void teletype(uint8_t ch)
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
        /* Text-mode BIOS TTY preserves the destination cell's attribute. */
        mem[screen_cell(column, row)] = ch;
        if (++column == columns) {
            column = 0;
            ++row;
        }
        break;
    }
    if (row >= rows) {
        scroll_window(0, 1, 0x07, 0, 0, columns - 1, rows - 1);
        row = rows - 1;
    }
    cursor_set(column, row);
}

void con_write(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        teletype(buf[i]);
}

void bios_int10(void)
{
    unsigned column, row;
    switch (cpu.a.h) {
    case 0x00:
        /* The native screen implements colour text mode 3, not graphics or
         * the independent monochrome framebuffer at B000:0000. */
        if ((cpu.a.l & 0x7f) == 3)
            mode_three((cpu.a.l & 0x80) != 0);
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
            size_t address = screen_cell(column, row);
            cpu.a.x = (uint16_t)(mem[address] | (mem[address + 1] << 8));
        }
        break;
    case 0x09:
    case 0x0a:
        if (cpu.b.h == 0 && cursor_position(&column, &row)) {
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
    case 0x0e:
        teletype(cpu.a.l);
        break;
    case 0x0f:
        cpu.a.l = mem[0x449];
        cpu.a.h = (uint8_t)bios_columns();
        cpu.b.h = 0;
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
    if (!key_read(1, 1, &key)) {
        if (!blocking) {
            term_idle(0);
            if (!key_read(1, 1, &key))
                return 0;
        } else {
            do {
                term_idle(50);
            } while (!key_read(1, 1, &key));
        }
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
            teletype(' ');
    } else {
        teletype(ch);
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
        mem[screen_cell(column, row)] = ' ';
    }
    cursor_set(column, row);
}

static void console_line(void)
{
    uint16_t segment = cpu.ds, offset = cpu.d.x;
    unsigned maximum = rd8(segment, offset), count = 0;
    uint8_t widths[255];
    if (maximum == 0)
        return;
    for (;;) {
        uint8_t ch;
        int extended;
        (void)console_get(1, &ch, &extended);
        if (extended)
            continue; /* Both halves of an extended key are editing no-ops. */
        if (ch == '\r') {
            wr8(segment, (uint16_t)(offset + 1), (uint8_t)count);
            wr8(segment, (uint16_t)(offset + 2 + count), '\r');
            console_put('\r');
            console_put('\n');
            return;
        }
        if (ch == '\b') {
            if (count != 0)
                console_erase(widths[--count]);
        } else if (ch == 0x1b) {
            while (count != 0)
                console_erase(widths[--count]);
        } else if (ch >= ' ' || ch == '\t') {
            if (count + 1 < maximum) {
                wr8(segment, (uint16_t)(offset + 2 + count), ch);
                widths[count++] = ch == '\t' ?
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
        (void)console_get(1, &ch, &extended);
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
        (void)console_get(1, &ch, &extended);
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
