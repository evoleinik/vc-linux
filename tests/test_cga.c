/* CGA BIOS tests inspect physical video memory independently of its decoder.
 * No terminal, translated image, or external graphics library is needed. */
#include "bios.h"
#include "cpu.h"
#include "hle.h"
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Cpu cpu;
uint8_t mem[MEM_SIZE];
static unsigned failures, checks, invalidations, bells;
static const char *group;

void term_invalidate(void) { ++invalidations; }
void term_bell(void) { ++bells; }
void term_idle(int milliseconds) { (void)milliseconds; }
void term_flush_input(void) {}
void term_clear_pending(void) {}

#define CHECK(condition, message) do {                                     \
    ++checks;                                                             \
    if (!(condition)) {                                                   \
        ++failures;                                                       \
        if (failures <= 20)                                               \
            fprintf(stderr, "FAIL [%s] line %d: %s\n", group, __LINE__, message); \
    }                                                                     \
} while (0)

static void video(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{
    cpu.a.x = (uint16_t)ax;
    cpu.b.x = (uint16_t)bx;
    cpu.c.x = (uint16_t)cx;
    cpu.d.x = (uint16_t)dx;
    bios_int10();
}

static void reset(unsigned mode)
{
    memset(mem, 0, sizeof(mem));
    memset(&cpu, 0, sizeof(cpu));
    bios_init();
    video(mode, 0, 0, 0);
}

static unsigned word(unsigned address)
{
    return mem[address] | ((unsigned)mem[address + 1] << 8);
}

static unsigned raw_pixel(unsigned mode, unsigned x, unsigned y)
{
    /* Keep this independent from bios_graphics_pixel/cga_address. */
    unsigned address = 0xb8000 + (y % 2 ? 8192 : 0) + y / 2 * 80;
    return mode == 6 ?
        (mem[address + x / 8] >> (7 - x % 8)) & 1 :
        (mem[address + x / 4] >> (6 - x % 4 * 2)) & 3;
}

static void modes(void)
{
    group = "00h/0Fh modes and BDA";
    for (unsigned mode = 4; mode <= 6; ++mode) {
        reset(3);
        memset(mem + 0xb8000, 0xa5, 0x4001);
        unsigned before = invalidations;
        video(mode, 0, 0, 0);
        CHECK(mem[0x449] == mode, "mode is in guest BDA");
        CHECK(bios_graphics_width() == (mode == 6 ? 640 : 320), "pixel width");
        CHECK(bios_columns() == (mode == 6 ? 80 : 40), "character columns");
        CHECK(bios_rows() == 25 && word(0x485) == 8, "25 rows of 8x8 characters");
        CHECK(word(0x44c) == 0x4000 && word(0x44e) == 0, "16K graphics page");
        CHECK(word(0x450) == 0 && mem[0x462] == 0, "cursor and active page reset");
        CHECK(mem[0x465] == (mode == 4 ? 0x2a : mode == 5 ? 0x2e : 0x1e),
              "CGA mode control shadow");
        CHECK(mem[0x466] == (mode == 6 ? 0x3f : 0x30), "CGA default colour select");
        CHECK(invalidations > before, "mode transition invalidates renderer");
        for (unsigned i = 0; i < 0x4000; ++i)
            CHECK(mem[0xb8000 + i] == 0, "mode change clears exactly 16K");
        CHECK(mem[0xbc000] == 0xa5, "graphics clear stays inside CGA aperture");
        video(0x0f00, 0xffff, 0, 0);
        CHECK(cpu.a.l == mode && cpu.a.h == (mode == 6 ? 80 : 40),
              "0Fh returns mode and text columns");
        CHECK(cpu.b.h == 0 && cpu.b.l == 0xff, "0Fh returns page, preserves BL");
        mem[0xb8123] = 0x87;
        video(0x80 | mode, 0, 0, 0);
        CHECK(mem[0xb8123] == 0x87, "bit7 preserves video memory");
        CHECK(mem[0x449] == mode && (mem[0x487] & 0x80), "preserve bit BDA fields");
        video(3, 0, 0, 0);
        CHECK(!bios_graphics_width() && bios_columns() == 80 && bios_rows() == 25,
              "mode3 restores text dimensions");
        CHECK(mem[0xb8000] == ' ' && mem[0xb8001] == 7, "mode3 clears text cells");
        video(0x0f00, 0, 0, 0);
        CHECK(cpu.a.x == 0x5003 && cpu.b.h == 0, "mode3 query");
    }
}

static void text_modes(void)
{
    group = "SCREEN0 text modes 0/1/2/3";
    for (unsigned mode = 0; mode <= 3; ++mode) {
        reset(4);
        unsigned columns = mode < 2 ? 40 : 80;
        video(mode, 0, 0, 0);
        CHECK(!bios_graphics_width() && mem[0x449] == mode,
              "SCREEN0 selects the requested real text mode");
        CHECK(bios_columns() == columns && bios_rows() == 25,
              "modes0/1 have40 columns, modes2/3 have80");
        CHECK(word(0x44c) == (columns == 40 ? 0x800 : 0x1000),
              "text page size follows character width");
        CHECK(word(0xb8000) == 0x0720, "leaving graphics clears text with attribute7");
        video(0x0f00, 0, 0, 0);
        CHECK(cpu.a.l == mode && cpu.a.h == columns, "0Fh reports truthful text geometry");
        video(0x0200, 0, 0, 0x0203);
        video(0x0941, 0x2e, 1, 0);
        video(0x0800, 0, 0, 0);
        CHECK(cpu.a.x == 0x2e41, "text09h/08h still write/read character and attribute");
        CHECK(word(0xb8000 + (2 * columns + 3) * 2) == 0x2e41,
              "40-column text uses the correct physical row stride");
        video(0x0200, 0, 0, columns - 1);
        video(0x0e58, 0, 0, 0);
        CHECK(word(0x450) == 0x0100, "text TTY wraps at the selected width");
        mem[0xb8000] = 'P';
        video(0x80 | mode, 0, 0, 0);
        CHECK(mem[0xb8000] == 'P' && mem[0x449] == mode,
              "no-clear text mode request preserves the screen");
        video(3, 0, 0, 0);
        CHECK(bios_columns() == 80 && mem[0x449] == 3,
              "VC can restore mode3 after any BASIC text mode");
    }
}

static void pixels(void)
{
    group = "0Ch/0Dh pixels, XOR and physical interlace";
    for (unsigned mode = 4; mode <= 6; ++mode) {
        reset(mode);
        unsigned width = mode == 6 ? 640 : 320, mask = mode == 6 ? 1 : 3;
        /* Exercise both ends of every scanline and all positions in a byte. */
        for (unsigned y = 0; y < 200; ++y)
            for (unsigned x = 0; x < width; ++x) {
                unsigned color = (x + 3 * y + x / 8) & mask;
                video(0x0c00 | color, 0, x, y);
                CHECK(raw_pixel(mode, x, y) == color, "0Ch writes real CGA address/shift");
            }
        for (unsigned y = 0; y < 200; ++y)
            for (unsigned x = 0; x < width; ++x) {
                unsigned color = (x + 3 * y + x / 8) & mask;
                video(0x0dff, 0, x, y);
                CHECK(cpu.a.l == color, "0Dh reads written pixel");
                CHECK(bios_graphics_pixel(x, y) == color, "renderer sees same raw pixel");
            }
        /* An independent direct-memory write must appear at an odd-line
         * coordinate without invoking any BIOS writer or dirty marker. */
        mem[0xba051] = 0xe4;
        video(0x0d00, 0, mode == 6 ? 8 : 4, 3);
        CHECK(cpu.a.l == (mode == 6 ? 1 : 3), "direct B800h write is read back");
        CHECK(bios_graphics_pixel(mode == 6 ? 8 : 4, 3) == (mode == 6 ? 1 : 3),
              "direct B800h write is rendered");
        video(0x0c00 | mask, 0, width - 1, 199);
        CHECK((mem[0xbbf3f] & mask) == mask, "last pixel uses odd bank offset3F3Fh");
        video(0x0c80 | mask, 0, width - 1, 199);
        CHECK((mem[0xbbf3f] & mask) == 0, "bit7 XOR erases matching pixel");
        video(0x0c80 | mask, 0, width - 1, 199);
        CHECK((mem[0xbbf3f] & mask) == mask, "second XOR restores pixel");
        static uint8_t snapshot[0x4000];
        memcpy(snapshot, mem + 0xb8000, sizeof(snapshot));
        video(0x0c03, 0, width, 0);
        video(0x0c03, 0, 0, 200);
        video(0x0cff, 0, 0xffff, 0xffff);
        CHECK(memcmp(snapshot, mem + 0xb8000, sizeof(snapshot)) == 0,
              "out-of-range coordinates do not wrap or overwrite memory");
        video(0x0dff, 0, width, 0);
        CHECK(cpu.a.l == 0, "out-of-range read is zero");
        for (unsigned i = 0x1f40; i < 0x2000; ++i)
            CHECK(mem[0xb8000 + i] == 0 && mem[0xba000 + i] == 0,
                  "scanline-bank padding is untouched");
    }
    reset(4);
    video(0x0c01, 0, 0, 0);
    video(0x0c02, 0, 1, 0);
    video(0x0c03, 0, 2, 0);
    video(0x0c01, 0, 3, 0);
    video(0x0c02, 0, 4, 0);
    video(0x0c03, 0, 0, 1);
    CHECK(mem[0xb8000] == 0x6d && mem[0xb8001] == 0x80, "four MSB-first pixels per byte");
    CHECK(mem[0xba000] == 0xc0, "odd row starts at 2000h, not next scanline");
    video(0x0c83, 0, 1, 0);
    CHECK(mem[0xb8000] == 0x5d, "XOR changes only the selected two bits");
    reset(6);
    video(0x0c01, 0, 0, 0);
    video(0x0c01, 0, 7, 0);
    video(0x0c01, 0, 8, 0);
    CHECK(mem[0xb8000] == 0x81 && mem[0xb8001] == 0x80, "eight MSB-first mode6 pixels");
}

static void palettes(void)
{
    group = "0Bh palette/background and 3D9h colour selector";
    reset(4);
    CHECK(bios_cga_color(0) == 0 && bios_cga_color(1) == 11 &&
          bios_cga_color(2) == 13 && bios_cga_color(3) == 15, "default bright cyan/magenta/white");
    video(0x0b00, 0x0005, 0, 0);
    CHECK(mem[0x466] == 0x25 && bios_cga_color(0) == 5, "BH0 sets background preserving palette");
    CHECK(bios_cga_color(1) == 3 && bios_cga_color(2) == 5 && bios_cga_color(3) == 7,
          "background bit4 controls palette intensity");
    video(0x0b00, 0x0100, 0, 0);
    CHECK(mem[0x466] == 5 && bios_cga_color(1) == 2 &&
          bios_cga_color(2) == 4 && bios_cga_color(3) == 6, "BH1 selects green/red/brown");
    video(0x0b00, 0x001e, 0, 0);
    CHECK(bios_cga_color(0) == 14 && bios_cga_color(1) == 10 &&
          bios_cga_color(2) == 12 && bios_cga_color(3) == 14, "bright palette0 and yellow background");
    video(0x0b00, 0x0101, 0, 0);
    CHECK(mem[0x466] == 0x3e && bios_cga_color(1) == 11, "palette change preserves background/intensity");
    video(0x0b00, 0x0200, 0, 0);
    CHECK(mem[0x466] == 0x3e, "unknown 0Bh subfunction has no effect");
    unsigned before = invalidations;
    bios_cga_color_select(0x02);
    CHECK(bios_cga_color_register() == 2 && mem[0x466] == 2, "port API and BDA share selector");
    CHECK(invalidations > before && bios_cga_color(0) == 2 && bios_cga_color(3) == 6,
          "port palette update invalidates renderer");
    reset(5);
    for (unsigned selection = 0; selection < 2; ++selection) {
        bios_cga_color_select((uint8_t)(selection << 5));
        CHECK(bios_cga_color(1) == 3 && bios_cga_color(2) == 4 && bios_cga_color(3) == 7,
              "mode5 RGBI cyan/red/white ignores palette selection");
    }
    bios_cga_color_select(0x10);
    CHECK(bios_cga_color(1) == 11 && bios_cga_color(2) == 12 && bios_cga_color(3) == 15,
          "mode5 supports high intensity");
    reset(6);
    CHECK(bios_cga_color(0) == 0 && bios_cga_color(1) == 15, "mode6 starts black/white");
    video(0x0b00, 0x000c, 0, 0);
    CHECK(bios_cga_color(0) == 0 && bios_cga_color(1) == 12, "mode6 colour select changes foreground");
    bios_cga_color_select(1);
    CHECK(bios_cga_color(0) == 0 && bios_cga_color(1) == 1, "direct selector changes mode6 foreground");
}

static void glyph_at(unsigned mode, unsigned column, unsigned row,
                     const uint8_t glyph[8], unsigned color)
{
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            CHECK(raw_pixel(mode, column * 8 + x, row * 8 + y) ==
                  ((glyph[y] >> x) & 1 ? color : 0), "glyph bitmap and foreground/background");
}

static void characters(void)
{
    static const uint8_t letter_a[8] = {0x0c, 0x1e, 0x33, 0x33, 0x3f, 0x33, 0x33, 0};
    static const uint8_t blank[8] = {0};
    static const uint8_t full[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    static const uint8_t horizontal[8] = {0, 0, 0, 0, 255, 0, 0, 0};
    static const uint8_t vertical[8] = {8, 8, 8, 8, 8, 8, 8, 8};
    static const uint8_t upper[8] = {255, 255, 255, 255, 0, 0, 0, 0};
    static const uint8_t lower[8] = {0, 0, 0, 0, 255, 255, 255, 255};
    static const uint8_t left[8] = {15, 15, 15, 15, 15, 15, 15, 15};
    static const uint8_t right[8] = {240, 240, 240, 240, 240, 240, 240, 240};
    static const struct { unsigned ch; const uint8_t *bits; } mapped[] = {
        {0xc4, horizontal}, {0xb3, vertical}, {0xdb, full}, {0xdf, upper},
        {0xdc, lower}, {0xdd, left}, {0xde, right}, {0x80, blank}
    };
    group = "02h/03h/09h/0Ah graphics cursor and font8x8 glyphs";
    for (unsigned mode = 4; mode <= 6; ++mode) {
        reset(mode);
        unsigned color = mode == 6 ? 1 : 2, columns = mode == 6 ? 80 : 40;
        video(0x0200, 0, 0, 0x0203);
        video(0x0300, 0, 0, 0);
        CHECK(cpu.d.x == 0x0203 && cpu.c.x == 0x0607, "cursor02h/03h round trip and 8-row shape");
        video(0x0941, color, 2, 0);
        glyph_at(mode, 3, 2, letter_a, color);
        glyph_at(mode, 4, 2, letter_a, color);
        CHECK(word(0x450) == 0x0203, "09h does not advance cursor");
        video(0x0a20, color, 1, 0);
        glyph_at(mode, 3, 2, blank, color);
        glyph_at(mode, 4, 2, letter_a, color);
        video(0x0a41, color, 1, 0);
        glyph_at(mode, 3, 2, letter_a, color);
        CHECK(word(0x450) == 0x0203, "0Ah uses BL colour but does not advance cursor");
        video(0x0941, color | 0x80, 1, 0);
        glyph_at(mode, 3, 2, blank, color);
        video(0x0941, color | 0x80, 1, 0);
        glyph_at(mode, 3, 2, letter_a, color);
        video(0x0200, 0, 0, (4 << 8) | (columns - 1));
        video(0x0941, color, 2, 0);
        glyph_at(mode, columns - 1, 4, letter_a, color);
        glyph_at(mode, 0, 5, letter_a, color);
        CHECK(word(0x450) == ((4 << 8) | (columns - 1)), "repeat crosses line without cursor update");
        for (unsigned i = 0; i < sizeof(mapped) / sizeof(mapped[0]); ++i) {
            video(0x0200, 0, 0, 0);
            video(0x0900 | mapped[i].ch, color, 1, 0);
            glyph_at(mode, 0, 0, mapped[i].bits, color);
        }
        video(0x0200, 0, 0, 0xffff);
        uint8_t before = mem[0xb8000];
        video(0x0941, color, 65535, 0);
        CHECK(mem[0xb8000] == before, "off-screen cursor never wraps into video memory");
    }
}

static void read_characters(void)
{
    group = "08h recognizes live CGA glyphs for BASIC screen editor";
    for (unsigned mode = 4; mode <= 6; ++mode) {
        reset(mode);
        video(0x0200, 0, 0, 0x0203);
        video(0x08ff, 0, 0, 0);
        CHECK(cpu.a.x == ' ', "empty graphics cell reads as a space, not stale AL");
        for (unsigned ch = 32; ch < 127; ++ch) {
            video(0x0900 | ch, mode == 6 ? 1 : 1 + ch % 3, 1, 0);
            video(0x08ff, 0, 0x1234, 0x5678);
            CHECK(cpu.a.x == ch, "08h round-trips every printable BASIC input character");
            CHECK(cpu.c.x == 0x1234 && cpu.d.x == 0x5678 && word(0x450) == 0x0203,
                  "08h preserves cursor and coordinate registers");
        }
        static const uint8_t mapped[] = {0xb3, 0xc4, 0xdb, 0xdc, 0xdd, 0xde, 0xdf};
        for (unsigned i = 0; i < sizeof(mapped); ++i) {
            video(0x0900 | mapped[i], 1, 1, 0);
            video(0x08ff, 0, 0, 0);
            CHECK(cpu.a.l == mapped[i], "08h also recognizes supplied box/block glyphs");
        }
        /* Construct A independently by direct guest-memory writes, with
         * varied ink colours; no BIOS text call or saved-character cache. */
        static const uint8_t letter_a[8] = {0x0c,0x1e,0x33,0x33,0x3f,0x33,0x33,0};
        memset(mem + 0xb8000, 0, 0x4000);
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                unsigned px = 3 * 8 + x, py = 2 * 8 + y;
                unsigned address = 0xb8000 + py % 2 * 8192 + py / 2 * 80;
                unsigned color = (letter_a[y] >> x) & 1 ?
                                 (mode == 6 ? 1 : 1 + (x + y) % 3) : 0;
                if (mode == 6)
                    mem[address + px / 8] |= (uint8_t)(color << (7 - px % 8));
                else
                    mem[address + px / 4] |= (uint8_t)(color << (6 - px % 4 * 2));
            }
        }
        video(0x08ff, 0, 0, 0);
        CHECK(cpu.a.x == 'A', "08h recognizes direct VRAM writes regardless of ink colour");
        bios_cga_color_select(0x1b);
        video(0x08ff, 0, 0, 0);
        CHECK(cpu.a.x == 'A', "palette changes do not alter BIOS glyph identity");
        memset(mem + 0xb8000, 0, 0x4000);
        video(0x0c01, 0, 3 * 8 + 7, 2 * 8 + 7);
        video(0x08ff, 0, 0, 0);
        CHECK(cpu.a.x == 0, "non-font pixels return unknown character0");
    }
}

static void scroll_and_tty(void)
{
    static const uint8_t full[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    static const uint8_t blank[8] = {0};
    group = "06h/07h graphics scroll/clear and 0Eh teletype";
    for (unsigned mode = 4; mode <= 6; ++mode) {
        reset(mode);
        unsigned color = mode == 6 ? 1 : 3, columns = mode == 6 ? 80 : 40;
        video(0x0200, 0, 0, 0x0102);
        video(0x09db, color, 1, 0);
        video(0x0601, 0, 0x0001, 0x0203);
        glyph_at(mode, 2, 0, full, color);
        glyph_at(mode, 2, 1, blank, color);
        video(0x0701, color << 8, 0x0001, 0x0203);
        glyph_at(mode, 2, 1, full, color);
        glyph_at(mode, 1, 0, full, color);
        glyph_at(mode, 0, 0, blank, color);
        video(0x0600, 0, 0x0001, 0x0203);
        glyph_at(mode, 2, 1, blank, color);
        video(0x0700, color << 8, 0x0000, 0xffff);
        CHECK(raw_pixel(mode, columns * 8 - 1, 199) == color, "clear clips window to graphics dimensions");
        video(0x0600, 0, 0, 0xffff);
        video(0x0200, 0, 0, columns - 1);
        video(0x0edb, color, 0, 0);
        glyph_at(mode, columns - 1, 0, full, color);
        CHECK(word(0x450) == 0x0100, "0Eh draws and wraps at character width");
        video(0x0e08, color, 0, 0);
        CHECK(word(0x450) == 0x0100, "backspace stays at left edge");
        video(0x0edb, color, 0, 0);
        video(0x0e08, color, 0, 0);
        CHECK(word(0x450) == 0x0100, "backspace moves cursor without erasing");
        glyph_at(mode, 0, 1, full, color);
        video(0x0e0d, color, 0, 0);
        video(0x0e0a, color, 0, 0);
        CHECK(word(0x450) == 0x0200, "CR/LF cursor controls");
        unsigned before = bells;
        video(0x0e07, color, 0, 0);
        CHECK(bells == before + 1 && word(0x450) == 0x0200, "BEL rings without cursor movement");
        video(0x0200, 0, 0, (24 << 8) | (columns - 1));
        video(0x0edb, color, 0, 0);
        glyph_at(mode, columns - 1, 23, full, color);
        glyph_at(mode, columns - 1, 24, blank, color);
        CHECK(word(0x450) == 0x1800, "TTY scrolls bottom row, clears with background");
        const uint8_t character[] = {0xdb};
        con_write(character, 1);
        glyph_at(mode, 0, 24, full, color);
        CHECK(word(0x450) == 0x1801, "DOS CON also draws graphics text");
    }
}

int main(void)
{
    modes();
    text_modes();
    pixels();
    palettes();
    characters();
    read_characters();
    scroll_and_tty();
    printf("CGA BIOS: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
