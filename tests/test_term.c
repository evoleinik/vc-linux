/* Independent BIOS/terminal tests: no CPU core or DOS file layer. Unit tests
 * use an output hook; isolated lifecycle tests create their own real PTYs. */
#define _XOPEN_SOURCE 700
#include "cpu.h"
#include "hle.h"
#include "bios.h"
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

Cpu cpu;
uint8_t mem[MEM_SIZE];

static unsigned checks, failures, groups;
static const char *test_name;
static char output[262144];
static size_t output_size;

#define CHECK(condition, message) do {                                      \
    ++checks;                                                             \
    if (!(condition)) {                                                   \
        ++failures;                                                       \
        fprintf(stderr, "FAIL [%s] line %d: %s\n", test_name, __LINE__,    \
                (message));                                               \
    }                                                                     \
} while (0)

static void check_number(unsigned actual, unsigned expected,
                         const char *description)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "FAIL [%s] %s: expected 0x%04X, got 0x%04X\n",
                test_name, description, expected, actual);
    }
}

static unsigned word(unsigned address)
{
    return mem[address] | ((unsigned)mem[address + 1] << 8);
}

static void put_word(unsigned address, unsigned value)
{
    mem[address] = (uint8_t)value;
    mem[address + 1] = (uint8_t)(value >> 8);
}

static unsigned cell(unsigned row, unsigned column)
{
    return 0xb8000u + 2u * (row * word(0x44a) + column);
}

static void capture(const char *data, size_t length, void *opaque)
{
    (void)opaque;
    if (length >= sizeof(output) - output_size) {
        fprintf(stderr, "FAIL [%s]: output capture overflow\n", test_name);
        exit(2);
    }
    memcpy(output + output_size, data, length);
    output_size += length;
    output[output_size] = 0;
}

static void clear_output(void)
{
    output_size = 0;
    output[0] = 0;
}

static void clear_input(void)
{
    put_word(0x41a, 0x1e);
    put_word(0x41c, 0x1e);
    mem[0x417] = mem[0x418] = 0;
    term_reset_input();
    term_clear_pending();
}

static void reset(void)
{
    memset(&cpu, 0, sizeof(cpu));
    memset(mem, 0, sizeof(mem));
    bios_init();
    term_set_output(capture, NULL);
    term_set_truecolor(0);
    term_reset_input();
    term_invalidate();
    clear_output();
}

/* Read the real ring independently of INT 16h, to isolate parser failures. */
static unsigned pop_raw(void)
{
    unsigned head = word(0x41a);
    unsigned key;
    if (head == word(0x41c))
        return 0x10000;
    key = word(0x400 + head);
    head += 2;
    if (head == word(0x482))
        head = word(0x480);
    put_word(0x41a, head);
    return key;
}

static void feed(const char *bytes, uint64_t milliseconds)
{
    term_feed_input((const uint8_t *)bytes, strlen(bytes), milliseconds);
}

static void expect_sequence(const char *description, const char *sequence,
                            unsigned expected)
{
    clear_input();
    feed(sequence, 100);
    term_expire_input(131);
    check_number(pop_raw(), expected, description);
    check_number(pop_raw(), 0x10000, "sequence must produce only one key");
}

static void video(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{
    cpu.a.x = (uint16_t)ax;
    cpu.b.x = (uint16_t)bx;
    cpu.c.x = (uint16_t)cx;
    cpu.d.x = (uint16_t)dx;
    bios_int10();
}

static void mouse(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{
    cpu.a.x = (uint16_t)ax;
    cpu.b.x = (uint16_t)bx;
    cpu.c.x = (uint16_t)cx;
    cpu.d.x = (uint16_t)dx;
    bios_int33();
}

static unsigned read_key(unsigned function)
{
    /* Avoid a hanging test if the expected producer failed. */
    if ((function == 0 || function == 0x10) && word(0x41a) == word(0x41c)) {
        CHECK(0, "blocking INT 16h read was about to use an empty ring");
        return 0x10000;
    }
    cpu.a.h = (uint8_t)function;
    bios_int16();
    return cpu.a.x;
}

static int console(unsigned function)
{
    cpu.a.h = (uint8_t)function;
    return dos_con_int21();
}

static void test_bda(void)
{
    unsigned page;
    reset();
    put_word(0x46c, 0x1234);
    put_word(0x46e, 0x5678);
    bios_init();
    check_number(word(0x410) & 0x30, 0x20, "equipment colour display bits");
    check_number(mem[0x449], 3, "video mode");
    check_number(word(0x44a), 80, "columns");
    check_number(word(0x44c), 0x1000, "25-row BIOS page allocation");
    check_number(word(0x44e), 0, "page offset");
    for (page = 0; page < 8; ++page)
        check_number(word(0x450 + 2 * page), 0, "initial page cursor");
    CHECK(!(mem[0x461] & 0x20), "initial cursor is visible");
    check_number(mem[0x462], 0, "active page");
    check_number(word(0x463), 0x3d4, "colour CRTC port");
    check_number(mem[0x484], 24, "rows minus one");
    check_number(word(0x485), 16, "character height");
    CHECK(mem[0x487] != 0 || mem[0x489] != 0, "EGA/VGA information initialized");
    check_number(mem[0x417], 0, "keyboard flags");
    check_number(mem[0x418], 0, "extended keyboard flags");
    check_number(word(0x41a), 0x1e, "keyboard head");
    check_number(word(0x41c), 0x1e, "keyboard tail");
    check_number(word(0x480), 0x1e, "keyboard buffer start");
    check_number(word(0x482), 0x3e, "keyboard buffer end");
    check_number(word(0x46c), 0x1234, "timer low word must survive init");
    check_number(word(0x46e), 0x5678, "timer high word must survive init");
    check_number(bios_columns(), 80, "column helper");
    check_number(bios_rows(), 25, "row helper");
    check_number((unsigned)bios_blink_enabled(), 0, "default bright backgrounds");
}

static void test_ascii_keys(void)
{
    static const uint8_t scans[95] = {
        0x39,0x02,0x28,0x04,0x05,0x06,0x08,0x28,
        0x0a,0x0b,0x09,0x0d,0x33,0x0c,0x34,0x35,
        0x0b,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x27,0x27,0x33,0x0d,0x34,0x35,
        0x03,0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,
        0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,
        0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,
        0x2d,0x15,0x2c,0x1a,0x2b,0x1b,0x07,0x0c,
        0x29,0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,
        0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,
        0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,
        0x2d,0x15,0x2c,0x1a,0x2b,0x1b,0x29
    };
    unsigned ch;
    reset();
    for (ch = 32; ch < 127; ++ch) {
        char input[2] = {(char)ch, 0};
        char label[64];
        (void)snprintf(label, sizeof(label), "ASCII 0x%02X scan mapping", ch);
        expect_sequence(label, input, ((unsigned)scans[ch - 32] << 8) | ch);
    }
    expect_sequence("Enter", "\r", 0x1c0d);
    expect_sequence("Ctrl-Enter / LF", "\n", 0x1c0a);
    expect_sequence("Backspace", "\b", 0x0e08);
    expect_sequence("terminal DEL Backspace", "\177", 0x0e08);
    expect_sequence("Tab", "\t", 0x0f09);
    expect_sequence("Shift-Tab", "\033[Z", 0x0f00);
    expect_sequence("keypad Enter", "\033OM", 0xe00d);
    expect_sequence("keypad plus", "\033Ok", 0x4e2b);
    expect_sequence("keypad minus", "\033Om", 0x4a2d);
    expect_sequence("keypad asterisk", "\033Oj", 0x372a);
}

static void test_control_alt_keys(void)
{
    static const uint8_t scans[26] = {
        0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,
        0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,
        0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
    };
    unsigned i;
    reset();
    for (i = 0; i < 26; ++i) {
        char sequence[48], label[64];
        (void)snprintf(sequence, sizeof(sequence), "\033[%u;5u", 'a' + i);
        (void)snprintf(label, sizeof(label), "Ctrl-%c CSI-u", 'A' + (int)i);
        expect_sequence(label, sequence, ((unsigned)scans[i] << 8) | (i + 1));
        sequence[0] = '\033';
        sequence[1] = (char)('a' + i);
        sequence[2] = 0;
        (void)snprintf(label, sizeof(label), "Alt-%c ESC prefix", 'A' + (int)i);
        expect_sequence(label, sequence, (unsigned)scans[i] << 8);
        (void)snprintf(sequence, sizeof(sequence), "\033[%u;3u", 'a' + i);
        expect_sequence("Alt letter CSI-u", sequence, (unsigned)scans[i] << 8);
    }
    for (i = 0; i < 10; ++i) {
        char sequence[24], label[64];
        unsigned digit = (i + 1) % 10;
        sequence[0] = '\033';
        sequence[1] = (char)('0' + digit);
        sequence[2] = 0;
        (void)snprintf(label, sizeof(label), "Alt-%u digit", digit);
        expect_sequence(label, sequence, (0x78 + i) << 8);
        (void)snprintf(sequence, sizeof(sequence), "\033[%u;5u", '0' + digit);
        clear_input();
        feed(sequence, 100);
        /* IBM BIOS has no words for the remaining Ctrl-digit combinations. */
        check_number(pop_raw(), digit == 2 ? 0x0300 : digit == 6 ? 0x071e : 0x10000,
                     "Ctrl-digit BIOS-defined word or no key");
    }
    expect_sequence("Alt minus", "\033-", 0x8200);
    expect_sequence("Alt equals", "\033=", 0x8300);
    expect_sequence("Ctrl-minus", "\033[45;5u", 0x0c1f);
    expect_sequence("CSI-u Ctrl-Enter", "\033[13;5u", 0x1c0a);
    expect_sequence("modifyOtherKeys Ctrl-Enter", "\033[27;5;13~", 0x1c0a);
    clear_input();
    {
        static const uint8_t controls[] = {0x00,0x01,0x02,0x03,0x1c,0x1d,0x1e,0x1f};
        static const unsigned expected[] = {0x0300,0x1e01,0x3002,0x2e03,
                                             0x2b1c,0x1b1d,0x071e,0x0c1f};
        term_feed_input(controls, sizeof(controls), 100);
        for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
            check_number(pop_raw(), expected[i], "raw control-byte canonical scan");
    }
}

static void test_function_keys(void)
{
    static const unsigned tilde[12] = {11,12,13,14,15,17,18,19,20,21,23,24};
    static const unsigned modifiers[4] = {1,2,5,3};
    static const unsigned expected[4][12] = {
        {0x3b00,0x3c00,0x3d00,0x3e00,0x3f00,0x4000,0x4100,0x4200,0x4300,0x4400,0x8500,0x8600},
        {0x5400,0x5500,0x5600,0x5700,0x5800,0x5900,0x5a00,0x5b00,0x5c00,0x5d00,0x8700,0x8800},
        {0x5e00,0x5f00,0x6000,0x6100,0x6200,0x6300,0x6400,0x6500,0x6600,0x6700,0x8900,0x8a00},
        {0x6800,0x6900,0x6a00,0x6b00,0x6c00,0x6d00,0x6e00,0x6f00,0x7000,0x7100,0x8b00,0x8c00}
    };
    unsigned modifier, key;
    reset();
    for (modifier = 0; modifier < 4; ++modifier) {
        for (key = 0; key < 12; ++key) {
            char sequence[32], label[64];
            (void)snprintf(sequence, sizeof(sequence), "\033[%u;%u~",
                           tilde[key], modifiers[modifier]);
            (void)snprintf(label, sizeof(label), "F%u CSI tilde modifier %u",
                           key + 1, modifiers[modifier]);
            expect_sequence(label, sequence, expected[modifier][key]);
            (void)snprintf(sequence, sizeof(sequence), "\033[%u;%uu",
                           57364 + key, modifiers[modifier]);
            expect_sequence("kitty function key", sequence, expected[modifier][key]);
            if (key < 4) {
                (void)snprintf(sequence, sizeof(sequence), "\033[1;%u%c",
                               modifiers[modifier], 'P' + (int)key);
                expect_sequence("F1-F4 CSI final modifier", sequence,
                                expected[modifier][key]);
            }
        }
    }
    for (key = 0; key < 4; ++key) {
        char sequence[4] = {'\033','O',(char)('P' + key),0};
        expect_sequence("F1-F4 SS3", sequence, expected[0][key]);
    }
    for (key = 0; key < 12; ++key) {
        char sequence[16];
        (void)snprintf(sequence, sizeof(sequence), "\033[%u~", tilde[key]);
        expect_sequence("unmodified F key CSI tilde", sequence, expected[0][key]);
    }
}

static void test_navigation_keys(void)
{
    static const struct {
        const char *name;
        char final;
        unsigned tilde, plain, control, alt;
    } nav[] = {
        {"Up", 'A',0,0x48e0,0x8de0,0x9800},
        {"Down",'B',0,0x50e0,0x91e0,0xa000},
        {"Right",'C',0,0x4de0,0x74e0,0x9d00},
        {"Left",'D',0,0x4be0,0x73e0,0x9b00},
        {"Home",'H',1,0x47e0,0x77e0,0x9700},
        {"End",'F',4,0x4fe0,0x75e0,0x9f00},
        {"Page Up",0,5,0x49e0,0x84e0,0x9900},
        {"Page Down",0,6,0x51e0,0x76e0,0xa100},
        {"Insert",0,2,0x52e0,0x92e0,0xa200},
        {"Delete",0,3,0x53e0,0x93e0,0xa300}
    };
    static const unsigned modifiers[] = {1,2,5,3};
    unsigned key, modifier;
    reset();
    for (key = 0; key < sizeof(nav) / sizeof(nav[0]); ++key) {
        for (modifier = 0; modifier < 4; ++modifier) {
            char sequence[32], label[64];
            unsigned expected = modifier == 2 ? nav[key].control :
                                modifier == 3 ? nav[key].alt : nav[key].plain;
            (void)snprintf(label, sizeof(label), "%s modifier %u",
                           nav[key].name, modifiers[modifier]);
            if (nav[key].final) {
                (void)snprintf(sequence, sizeof(sequence), "\033[1;%u%c",
                               modifiers[modifier], nav[key].final);
                expect_sequence(label, sequence, expected);
            }
            if (nav[key].tilde) {
                (void)snprintf(sequence, sizeof(sequence), "\033[%u;%u~",
                               nav[key].tilde, modifiers[modifier]);
                expect_sequence(label, sequence, expected);
            }
        }
        if (nav[key].final) {
            char sequence[4] = {'\033','[',nav[key].final,0};
            expect_sequence("unmodified CSI navigation", sequence, nav[key].plain);
            sequence[1] = 'O';
            expect_sequence("SS3 navigation", sequence, nav[key].plain);
        }
    }
    expect_sequence("Home tilde 7 alias", "\033[7~", 0x47e0);
    expect_sequence("End tilde 8 alias", "\033[8~", 0x4fe0);
}

static void test_utf8_keys(void)
{
    static const struct { const char *utf8; unsigned cp866; } examples[] = {
        {"\320\220",0x80}, {"\320\257",0x9f}, {"\320\260",0xa0},
        {"\320\277",0xaf}, {"\321\200",0xe0}, {"\321\217",0xef},
        {"\320\201",0xf0}, {"\321\221",0xf1}, {"\320\204",0xf2},
        {"\321\224",0xf3}, {"\320\207",0xf4}, {"\321\227",0xf5},
        {"\320\216",0xf6}, {"\321\236",0xf7}, {"\302\260",0xf8},
        {"\302\267",0xfa}, {"\342\204\226",0xfc}, {"\302\244",0xfd},
        {"\342\226\240",0xfe}, {"\302\240",0xff}, {"\342\224\200",0xc4}
    };
    unsigned i;
    reset();
    for (i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i)
        expect_sequence("UTF-8 to CP866 with zero scan", examples[i].utf8,
                        examples[i].cp866);
    clear_input();
    term_feed_input((const uint8_t *)"\320", 1, 100);
    check_number(pop_raw(), 0x10000, "split UTF-8 lead byte waits");
    term_feed_input((const uint8_t *)"\220", 1, 101);
    check_number(pop_raw(), 0x0080, "split UTF-8 completed");
    clear_input();
    feed("\360\237\231\202", 100);
    check_number(pop_raw(), 0x10000, "unrepresentable Unicode does not fabricate a BIOS key");
    feed("x", 101);
    check_number(pop_raw(), 0x2d78, "parser recovers after unsupported Unicode");
}

static void test_streaming_and_escape(void)
{
    static const struct { const char *bytes; unsigned expected; } sequences[] = {
        {"\033[1;5A",0x8de0}, {"\033[23;3~",0x8b00},
        {"\033OP",0x3b00}, {"\033[97;5u",0x1e01},
        {"\342\224\200",0x00c4}, {"\033a",0x1e00},
        {"\033\033[D",0x9b00}, {"\033\033OP",0x6800}
    };
    unsigned i;
    reset();
    for (i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
        size_t split, length = strlen(sequences[i].bytes);
        for (split = 1; split < length; ++split) {
            clear_input();
            term_feed_input((const uint8_t *)sequences[i].bytes, split, 100);
            check_number(pop_raw(), 0x10000, "partial sequence queues nothing");
            term_feed_input((const uint8_t *)sequences[i].bytes + split,
                            length - split, 110);
            check_number(pop_raw(), sequences[i].expected, "all possible split points");
            check_number(pop_raw(), 0x10000, "split key emitted once");
        }
    }
    clear_input();
    feed("\033", 100);
    term_expire_input(129);
    check_number(pop_raw(), 0x10000, "Esc remains pending at 29 ms");
    term_expire_input(130);
    check_number(pop_raw(), 0x011b, "Esc resolves at 30 ms");
    term_expire_input(150);
    check_number(pop_raw(), 0x10000, "Esc timeout emits once");
    clear_input();
    feed("\033", 200);
    feed("a", 229);
    check_number(pop_raw(), 0x1e00, "29 ms Esc+letter becomes Alt");
    clear_input();
    feed("\033", 300);
    feed("a", 330);
    check_number(pop_raw(), 0x011b, "new input first expires old Esc");
    check_number(pop_raw(), 0x1e61, "30 ms quiet gap leaves next letter unmodified");
    clear_input();
    feed("abc\033[A", 400);
    check_number(pop_raw(), 0x1e61, "first of batched keys");
    check_number(pop_raw(), 0x3062, "second of batched keys");
    check_number(pop_raw(), 0x2e63, "third of batched keys");
    check_number(pop_raw(), 0x48e0, "escape after batched printable keys");
    clear_input();
    feed("\033[1;", 100);
    term_expire_input(199);
    check_number(pop_raw(), 0x10000, "recognized partial CSI survives a long input gap");
    feed("5A", 200);
    check_number(pop_raw(), 0x8de0, "slow split CSI still produces Ctrl-Up");
    check_number(pop_raw(), 0x10000, "slow split CSI produces exactly one key");
    clear_input();
    feed("\033\320", 100);
    term_expire_input(199);
    check_number(pop_raw(), 0x10000, "recognized Alt UTF-8 lead survives a long input gap");
    feed("\220", 200);
    check_number(pop_raw(), 0x0080, "slow Alt UTF-8 completion produces CP866 uppercase A");
    check_number(pop_raw(), 0x10000, "slow Alt UTF-8 does not leak Esc or continuation byte");
}

static void test_kitty_modifiers(void)
{
    reset();
    feed("\033[?1u", 100);
    CHECK(strstr(output, "\033[>11u") != NULL,
          "kitty negotiation enables exactly flags 1|2|8 after reply");
    check_number(pop_raw(), 0x10000, "negotiation reply is not a key");
    feed("\033[57441;2:1u", 101);
    CHECK(mem[0x417] & 2, "left Shift press updates BDA bit 1");
    check_number(pop_raw(), 0x10000, "modifier press queues no key");
    feed("\033[65;2:1u", 102);
    check_number(pop_raw(), 0x1e41, "kitty shifted letter press");
    feed("\033[65;2:2u", 103);
    check_number(pop_raw(), 0x1e41, "kitty repeat emits key");
    feed("\033[65;2:3u", 104);
    check_number(pop_raw(), 0x10000, "kitty release emits no key");
    feed("\033[57441;1:3u", 105);
    check_number(mem[0x417] & 3, 0, "Shift release clears flags");
    feed("\033[57442;5:1u", 106);
    CHECK(mem[0x417] & 4, "Ctrl press updates basic BDA");
    CHECK(mem[0x418] & 1, "left Ctrl press updates secondary BDA");
    feed("\033[57442;1:3u", 107);
    check_number(mem[0x417] & 4, 0, "Ctrl release clears basic BDA");
    check_number(mem[0x418] & 1, 0, "Ctrl release clears secondary BDA");
    feed("\033[57443;3:1u", 108);
    CHECK(mem[0x417] & 8, "Alt press updates basic BDA");
    CHECK(mem[0x418] & 2, "left Alt press updates secondary BDA");
    feed("\033[57443;1:3u", 109);
    check_number(mem[0x417] & 8, 0, "Alt release clears basic BDA");
    check_number(mem[0x418] & 2, 0, "Alt release clears secondary BDA");
    feed("\033[57447;2:1u", 110);
    CHECK(mem[0x417] & 1, "right Shift distinguished from left Shift");
    feed("\033[57447;1:3u", 111);
    check_number(mem[0x417] & 3, 0, "right Shift release");
    feed("\033[57449;3:1u", 112);
    CHECK(mem[0x417] & 8, "right Alt is Alt");
    check_number(mem[0x418] & 2, 0, "right Alt is not left Alt");
    feed("\033[57449;1:3u", 113);
    clear_input();
    clear_output();
    feed("\033[?0u", 200);
    CHECK(strstr(output, "\033[>11u") != NULL,
          "zero current kitty flags still advertise protocol support");
    feed("\033[97;193u", 201);
    check_number(mem[0x417] & 0x60, 0x60, "kitty reports CapsLock and NumLock flags");
    feed("\033[<0;2;2M", 202);
    check_number(mem[0x417] & 0x60, 0x60,
                 "SGR mouse lacking lock bits must preserve kitty lock flags");
    feed("\033[57441;2:1u", 203);
    feed("\033[57447;2:1u", 204);
    check_number(mem[0x417] & 3, 3, "simultaneously held left and right Shift");
    feed("\033[57441;2:3u", 205);
    check_number(mem[0x417] & 3, 1, "left Shift release keeps held right Shift");
    feed("\033[57447;1:3u", 206);
    check_number(mem[0x417] & 3, 0, "last Shift release clears both bits");
    feed("\033[57448;5:1u", 207);
    CHECK(mem[0x417] & 4, "right Ctrl sets aggregate Ctrl bit");
    check_number(mem[0x418] & 1, 0, "right Ctrl does not set left-Ctrl bit");
    CHECK(mem[0x496] & 4, "right Ctrl updates enhanced BDA bit");
    feed("\033[57448;1:3u", 208);
    check_number(mem[0x417] & 4, 0, "right Ctrl release clears aggregate bit");
    check_number(mem[0x496] & 4, 0, "right Ctrl release clears enhanced bit");
    feed("\033[65;2u", 209);
    CHECK(mem[0x417] & 3, "ordinary kitty key infers held Shift from snapshot");
    feed("\033[57447;1:3u", 210);
    check_number(mem[0x417] & 3, 0,
                 "right-Shift release clears previously inferred left-Shift state");
    feed("\033[97;5u", 211);
    CHECK(mem[0x417] & 4, "ordinary kitty key infers held Ctrl from snapshot");
    feed("\033[57448;1:3u", 212);
    check_number(mem[0x417] & 4, 0, "right-Ctrl release clears inferred aggregate Ctrl");
    check_number(mem[0x418] & 1, 0, "right-Ctrl release clears inferred left-Ctrl state");
    check_number(mem[0x496] & 4, 0, "right-Ctrl release clears enhanced Ctrl bit");
    feed("\033[97;3u", 213);
    CHECK(mem[0x417] & 8, "ordinary kitty key infers held Alt from snapshot");
    feed("\033[57449;1:3u", 214);
    check_number(mem[0x417] & 8, 0, "right-Alt release clears inferred aggregate Alt");
    check_number(mem[0x418] & 2, 0, "right-Alt release clears inferred left-Alt state");
    check_number(mem[0x496] & 8, 0, "right-Alt release clears enhanced Alt bit");
}

static void test_keyboard_ring(void)
{
    unsigned i;
    reset();
    for (i = 0; i < 15; ++i)
        check_number((unsigned)bios_key_push((uint16_t)(0x1000 + i)), 1,
                     "BIOS ring accepts fifteen words");
    check_number((unsigned)bios_key_push(0xffff), 0, "full BIOS ring drops key");
    check_number(word(0x41c), 0x3c, "tail lives in real BDA");
    check_number(read_key(0x11), 0x1000, "enhanced poll reads first key");
    check_number(cpu.zf, 0, "enhanced poll nonempty ZF");
    check_number(word(0x41a), 0x1e, "enhanced poll is non-consuming");
    for (i = 0; i < 15; ++i)
        check_number(read_key(0x10), 0x1000 + i, "ring FIFO read");
    for (i = 0; i < 8; ++i) {
        CHECK(bios_key_push((uint16_t)(0x2000 + i)), "ring wrap insertion");
        check_number(read_key(0x10), 0x2000 + i, "ring wrap read");
    }
    cpu.a.h = 5;
    cpu.c.x = 0x2e63;
    bios_int16();
    check_number(cpu.a.l, 0, "AH05 returns success");
    check_number(pop_raw(), 0x2e63, "AH05 pushes CX into BDA ring");
    for (i = 0; i < 15; ++i)
        (void)bios_key_push(0x1e61);
    cpu.a.h = 5;
    cpu.c.x = 0x3062;
    bios_int16();
    check_number(cpu.a.l, 1, "AH05 returns full");
    clear_input();
    feed("abcdefghijklmnopq", 100);
    for (i = 0; i < 15; ++i)
        CHECK(pop_raw() != 0x10000, "parser fills fifteen-word ring");
    check_number(pop_raw(), 0x10000, "parser drops excess keys on full ring");
    mem[0x417] = 0x5b;
    mem[0x418] = 0x03;
    (void)read_key(2);
    check_number(cpu.a.l, 0x5b, "legacy modifier flags");
    (void)read_key(0x12);
    check_number(cpu.a.l, 0x5b, "extended modifier flags low byte");
    check_number(cpu.a.h & 3, 3, "extended modifier flags left Ctrl/Alt");
}

/* Keys beyond the 15-slot BIOS ring wait in the terminal layer, in order. */
static void test_typeahead_survives_full_ring(void)
{
    reset();
    clear_input();
    static const char text[] = "echo made by vc > made.txt";
    term_feed_input((const uint8_t *)text, sizeof text - 1, 0);
    char got[64];
    size_t n = 0;
    for (;;) {
        unsigned word = read_key(0x11);
        if (cpu.zf) break;
        (void)read_key(0x10);
        got[n++] = (char)(word & 0xff);
        if (n == sizeof got - 1) break;
    }
    got[n] = 0;
    CHECK(strcmp(got, text) == 0, "every typed key arrives, in order");
}

static void test_legacy_keyboard(void)
{
    static const struct { unsigned enhanced, legacy; } keys[] = {
        {0x48e0,0x4800}, {0x4be0,0x4b00}, {0x73e0,0x7300},
        {0x74e0,0x7400}, {0x77e0,0x7700}, {0x75e0,0x7500},
        {0x84e0,0x8400}, {0x76e0,0x7600}, {0xe00d,0x1c0d},
        {0xe00a,0x1c0a}, {0x00e0,0x00e0}, {0x00f0,0x00f0},
        {0x3b00,0x3b00}, {0x1e61,0x1e61}
    };
    static const unsigned hidden[] = {
        0x8500,0x8600,0x8700,0x8800,0x8900,0x8a00,0x8b00,0x8c00,
        0x8de0,0x91e0,0x9700,0x9800,0x9900,0x9b00,0x9d00,
        0x9f00,0xa000,0xa100,0xa200,0xa300
    };
    unsigned i;
    reset();
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        clear_input();
        CHECK(bios_key_push((uint16_t)keys[i].enhanced), "enqueue legacy sample");
        check_number(read_key(0x11), keys[i].enhanced, "enhanced poll exact word");
        check_number(read_key(1), keys[i].legacy, "legacy poll normalization");
        check_number(cpu.zf, 0, "legacy poll found compatible key");
        check_number(read_key(0), keys[i].legacy, "legacy read normalization");
        check_number(pop_raw(), 0x10000, "legacy read consumed word");
    }
    for (i = 0; i < sizeof(hidden) / sizeof(hidden[0]); ++i) {
        clear_input();
        (void)bios_key_push((uint16_t)hidden[i]);
        (void)bios_key_push(0x1e61);
        check_number(read_key(0x11), hidden[i], "extended-only key visible to enhanced BIOS");
        check_number(read_key(1), 0x1e61, "legacy poll hides enhanced-only key");
        check_number(read_key(0), 0x1e61, "legacy read skips hidden head");
    }
    clear_input();
    (void)read_key(0x11);
    check_number(cpu.zf, 1, "empty enhanced poll sets ZF");
    (void)read_key(1);
    check_number(cpu.zf, 1, "empty legacy poll sets ZF");
}

static void test_video_functions(void)
{
    unsigned i;
    reset();
    video(0x0100, 0, 0x0506, 0);
    check_number(word(0x460), 0x0506, "set cursor shape");
    video(0x0200, 0, 0, 0x0407);
    check_number(word(0x450), 0x0407, "set cursor position");
    video(0x0300, 0, 0, 0);
    check_number(cpu.d.x, 0x0407, "get cursor position");
    check_number(cpu.c.x, 0x0506, "get cursor shape");
    video(0x0941, 0x001e, 3, 0);
    for (i = 7; i < 10; ++i)
        check_number(word(cell(4,i)), 0x1e41, "AH09 character and attribute repeat");
    check_number(word(0x450), 0x0407, "AH09 does not advance cursor");
    video(0x0800, 0, 0, 0);
    check_number(cpu.a.x, 0x1e41, "AH08 reads character and attribute");
    video(0x0a42, 0x0002, 2, 0);
    check_number(word(cell(4,7)), 0x1e42, "AH0A preserves attributes");
    check_number(word(cell(4,8)), 0x1e42, "AH0A repeats character");
    check_number(word(cell(4,9)), 0x1e41, "AH0A stops at requested count");
    video(0x0e43, 0, 0, 0);
    check_number(mem[cell(4,7)], 'C', "AH0E teletype writes character");
    check_number(word(0x450), 0x0408, "AH0E advances cursor");
    video(0x0503, 0, 0, 0);
    check_number(mem[0x462], 0, "only video page zero exists");
    video(0x0f00, 0xffff, 0, 0);
    check_number(cpu.a.l, 3, "AH0F returns mode");
    check_number(cpu.a.h, 80, "AH0F returns columns");
    check_number(cpu.b.h, 0, "AH0F returns active page zero");
    video(0x1003, 1, 0, 0);
    check_number((unsigned)bios_blink_enabled(), 1, "enable blink");
    video(0x1003, 0, 0, 0);
    check_number((unsigned)bios_blink_enabled(), 0, "disable blink for bright backgrounds");
    video(0x1212, 0x0010, 0, 0);
    check_number(cpu.b.h, 0, "EGA/VGA is colour");
    check_number(cpu.b.l, 3, "EGA/VGA reports 256K");
    video(0x1a00, 0, 0, 0);
    check_number(cpu.a.l, 0x1a, "display-combination call supported");
    check_number(cpu.b.l, 8, "VGA colour display combination");
    video(0x4f00, 0, 0, 0);
    check_number(cpu.a.x, 0x014f, "VESA explicitly reports not supported");
    video(0x4f02, 0, 0, 0);
    check_number(cpu.a.x, 0x014f, "all VESA calls fail rather than false success");
    video(0x0003, 0, 0, 0);
    check_number(mem[0x484], 24, "mode 3 returns to 25 rows");
    check_number(word(0x485), 16, "mode 3 returns 16-line font");
    check_number(word(0x450), 0, "mode 3 resets cursor");
    for (i = 0; i < 80 * 25; ++i)
        check_number(word(0xb8000 + 2*i), 0x0720, "mode 3 clears screen with attribute 07");
}

static void test_font_switches(void)
{
    static const struct { unsigned function, rows, height; } fonts[] = {
        {0x1112,50,8}, {0x1111,28,14}, {0x1114,25,16}
    };
    unsigned i;
    reset();
    for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        video(fonts[i].function, 0, 0, 0);
        check_number(mem[0x484], fonts[i].rows - 1, "font switch updates row-count BDA");
        check_number(word(0x485), fonts[i].height, "font switch updates height BDA");
        check_number(bios_rows(), fonts[i].rows, "font switch affects row helper");
        video(0x1130, 0, 0, 0);
        check_number(cpu.c.x, fonts[i].height, "font query character height");
        check_number(cpu.d.l, fonts[i].rows - 1, "font query rows minus one");
    }
    video(0x1112, 0, 0, 0);
    put_word(cell(49,79), 0x1f58);
    term_render();
    CHECK(strstr(output, "X") != NULL, "renderer reaches fiftieth row");
    video(0x0003, 0, 0, 0);
    check_number(mem[0x484], 24, "mode 3 resets a 50-row font");
}

static void seed_scroll(void)
{
    unsigned row, column;
    for (row = 0; row < 5; ++row)
        for (column = 0; column < 8; ++column)
            put_word(cell(row,column), ((0x10 + row) << 8) | ('A' + row));
}

static void test_scroll_windows(void)
{
    unsigned row, column;
    reset();
    seed_scroll();
    video(0x0601, 0x2e00, 0x0102, 0x0305);
    for (row = 1; row <= 3; ++row) {
        for (column = 2; column <= 5; ++column)
            check_number(word(cell(row,column)), row == 3 ? 0x2e20 :
                         ((0x11 + row) << 8) | ('B' + row), "scroll-up window");
        check_number(word(cell(row,1)), ((0x10+row)<<8)|('A'+row),
                     "scroll-up preserves left edge outside window");
        check_number(word(cell(row,6)), ((0x10+row)<<8)|('A'+row),
                     "scroll-up preserves right edge outside window");
    }
    check_number(word(cell(0,2)), 0x1041, "scroll-up preserves row above");
    check_number(word(cell(4,2)), 0x1445, "scroll-up preserves row below");
    seed_scroll();
    video(0x0702, 0x4f00, 0x0102, 0x0305);
    for (row = 1; row <= 3; ++row)
        for (column = 2; column <= 5; ++column)
            check_number(word(cell(row,column)), row < 3 ? 0x4f20 : 0x1142,
                         "scroll-down two rows with overlap");
    check_number(word(cell(3,6)), 0x1344, "scroll-down preserves outside cells");
    seed_scroll();
    video(0x0600, 0x0700, 0x0102, 0x0305);
    for (row = 1; row <= 3; ++row)
        for (column = 2; column <= 5; ++column)
            check_number(word(cell(row,column)), 0x0720, "AL zero clears only window");
    seed_scroll();
    video(0x07ff, 0x1f00, 0x0102, 0x0305);
    check_number(word(cell(1,2)), 0x1f20, "oversized downward scroll clears window");
    check_number(word(cell(3,5)), 0x1f20, "oversized scroll clears last cell");
}

static void test_teletype(void)
{
    static const uint8_t controls[] = {'A','\b','B','\r','C','\n','D',7};
    unsigned column;
    reset();
    con_write(controls, sizeof(controls));
    check_number(mem[cell(0,0)], 'C', "CR and BS affect next character position");
    check_number(mem[cell(1,1)], 'D', "LF preserves column");
    check_number(word(0x450), 0x0102, "teletype control cursor");
    CHECK(strchr(output, 7) != NULL, "BEL reaches terminal output hook");
    video(0x0200, 0, 0, 0x004f);
    con_write((const uint8_t *)"XY", 2);
    check_number(mem[cell(0,79)], 'X', "last column receives character");
    check_number(mem[cell(1,0)], 'Y', "teletype wraps to next row");
    check_number(word(0x450), 0x0101, "wrapped teletype cursor");
    for (column = 0; column < 80; ++column)
        put_word(cell(24,column), 0x1e5a);
    video(0x0200, 0, 0, 0x184f);
    con_write((const uint8_t *)"XY", 2);
    check_number(mem[cell(23,79)], 'X', "bottom-right output scrolls up");
    check_number(mem[cell(24,0)], 'Y', "next character is on fresh bottom line");
    check_number(mem[cell(24,0) + 1], 7, "new line starts with attribute 07");
    for (column = 1; column < 80; ++column)
        check_number(word(cell(24,column)), 0x0720, "scroll blanks bottom with attribute 07");
    check_number(word(0x450), 0x1801, "scrolling cursor stays on bottom row");
}

static void test_renderer_palette(void)
{
    static const unsigned ansi_fg[16] = {
        30,34,32,36,31,35,33,37,90,94,92,96,91,95,93,97
    };
    static const unsigned ansi_bg[16] = {
        40,44,42,46,41,45,43,47,100,104,102,106,101,105,103,107
    };
    unsigned index;
    reset();
    for (index = 0; index < 16; ++index) {
        char expected[64], label[100];
        put_word(cell(0,0), (index << 8) | 'X');
        term_invalidate();
        clear_output();
        term_render();
        (void)snprintf(expected, sizeof(expected), "\033[0;%u;40mX", ansi_fg[index]);
        (void)snprintf(label, sizeof(label),
                       "VGA foreground colour index %u must map to ANSI %u",
                       index, ansi_fg[index]);
        CHECK(strstr(output, expected) != NULL, label);
        put_word(cell(0,0), (index << 12) | 0x0758);
        term_invalidate();
        clear_output();
        term_render();
        (void)snprintf(expected, sizeof(expected), "\033[0;37;%umX", ansi_bg[index]);
        (void)snprintf(label, sizeof(label),
                       "VGA background colour index %u must map to ANSI %u",
                       index, ansi_bg[index]);
        CHECK(strstr(output, expected) != NULL, label);
    }
    term_set_truecolor(1);
    put_word(cell(0,0), 0x0658);
    clear_output();
    term_invalidate();
    term_render();
    CHECK(strstr(output, "\033[0;38;2;170;85;0;48;2;0;0;0mX") != NULL,
          "truecolor VGA brown must be exact #AA5500, not dark yellow");
    put_word(cell(0,0), 0x9c58);
    clear_output();
    term_render();
    CHECK(strstr(output, "38;2;255;85;85;48;2;85;85;255mX") != NULL,
          "truecolor bright red foreground and bright blue background");
}

static void test_renderer_diffs_and_glyphs(void)
{
    static const uint8_t glyphs[] = {1,0x10,0x11,0x18,0x19,0x1e,0x1f,0x7f,0x80,0xe0,0xc4};
    static const char *unicode[] = {
        "\342\230\272", "\342\226\272", "\342\227\204", "\342\206\221",
        "\342\206\223", "\342\226\262", "\342\226\274", "\342\214\202",
        "\320\220", "\321\200", "\342\224\200"
    };
    unsigned i;
    reset();
    for (i = 0; i < sizeof(glyphs); ++i)
        put_word(cell(0,i), 0x0700 | glyphs[i]);
    term_render();
    for (i = 0; i < sizeof(unicode) / sizeof(unicode[0]); ++i)
        CHECK(strstr(output, unicode[i]) != NULL, "CP437 controls and CP866 glyphs render as UTF-8");
    clear_output();
    term_render();
    check_number((unsigned)output_size, 0, "unchanged second frame emits zero bytes");
    put_word(cell(2,3), 0x0458);
    clear_output();
    term_render();
    CHECK(strstr(output, "\033[3;4H") != NULL, "dirty cell uses correct one-based CUP position");
    CHECK(strstr(output, "\033[0;31;40mX") != NULL, "dirty cell emits red VGA colour and character");
    CHECK(output_size < 160, "single-cell change must not repaint whole screen");
    clear_output();
    term_render();
    check_number((unsigned)output_size, 0, "shadow updated after dirty cell");
    term_invalidate();
    term_render();
    CHECK(output_size > 2000, "explicit invalidation forces full frame");
    clear_output();
    video(0x0200, 0, 0, 0x0305);
    term_render();
    CHECK(strstr(output, "\033[4;6H") != NULL, "cursor comes from BDA page-zero position");
    CHECK(output_size < 160, "cursor-only change does not repaint cells");
    clear_output();
    video(0x0100, 0, 0x2000, 0);
    term_render();
    CHECK(strstr(output, "\033[?25l") != NULL, "cursor start bit 5 hides cursor");
    clear_output();
    video(0x0100, 0, 0x0607, 0);
    term_render();
    CHECK(strstr(output, "\033[?25h") != NULL, "visible BIOS cursor is restored");
}

static void test_renderer_mouse_and_blink(void)
{
    unsigned original;
    reset();
    put_word(cell(0,0), 0x1758);
    original = word(cell(0,0));
    term_render();
    clear_output();
    mouse(1, 0, 0, 0);
    term_render();
    CHECK(output_size != 0, "showing mouse changes rendered cell");
    check_number(word(cell(0,0)), original, "mouse inversion never modifies B800 memory");
    clear_output();
    term_render();
    check_number((unsigned)output_size, 0, "unchanged visible mouse does not trigger perpetual repaint");
    mouse(2, 0, 0, 0);
    term_render();
    CHECK(strstr(output, "\033[0;37;44mX") != NULL,
          "hiding mouse restores original uninverted shadow attribute");
    check_number(word(cell(0,0)), original, "hiding mouse leaves screen memory intact");
    clear_output();
    put_word(cell(0,0), 0x8758);
    video(0x1003, 1, 0, 0);
    term_render();
    CHECK(strstr(output, ";100mX") == NULL,
          "attribute bit 7 is not bright background when blink enabled");
    CHECK(strstr(output, ";5;") != NULL || strstr(output, ";5m") != NULL,
          "blink attribute enables terminal blink SGR");
    video(0x1003, 0, 0, 0);
    clear_output();
    term_render();
    CHECK(strstr(output, "\033[0;37;100mX") != NULL,
          "disabling blink redraws bit-7 attribute as bright background");
}

/* A click delivered in one read (press then release) must still show the
 * button down to one function 03h poll, then up. */
/* A press stays reported down for a minimum click time; wait it out. */
static void click_settles(void)
{
    struct timespec wait = {0, 100 * 1000000L};
    nanosleep(&wait, NULL);
}

static void test_mouse_quick_click(void)
{
    reset();
    cpu.a.x = 0; bios_int33();
    bios_mouse_event(5, 3, 1);
    bios_mouse_event(5, 3, 0);
    cpu.a.x = 3; bios_int33();
    check_number(cpu.b.x, 1, "quick click is seen by a poll");
    check_number(cpu.c.x, 40, "click column");
    cpu.a.x = 3; bios_int33();
    check_number(cpu.b.x, 1, "and by the next poll in the same pass");
    struct timespec wait = {0, 120 * 1000000L};
    nanosleep(&wait, NULL);
    cpu.a.x = 3; bios_int33();
    check_number(cpu.b.x, 0, "then the button is up");
}

static void test_mouse(void)
{
    unsigned column = 99, row = 99;
    reset();
    mouse(0, 0, 0, 0);
    check_number(cpu.a.x, 0xffff, "mouse installed");
    check_number(cpu.b.x, 2, "two-button mouse");
    check_number((unsigned)bios_mouse_cell(&column, &row), 0, "mouse initially hidden");
    mouse(1, 0, 0, 0);
    CHECK(bios_mouse_cell(&column,&row), "first show reaches visible counter zero");
    mouse(1, 0, 0, 0);
    mouse(2, 0, 0, 0);
    CHECK(bios_mouse_cell(&column,&row), "balanced nested show/hide still visible");
    mouse(2, 0, 0, 0);
    check_number((unsigned)bios_mouse_cell(&column,&row), 0, "second hide goes below zero");
    (void)bios_mouse_event(7, 11, 1);
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 1, "mouse left-button status");
    check_number(cpu.c.x, 56, "mouse X column times eight");
    check_number(cpu.d.x, 88, "mouse Y row times eight");
    mouse(5, 0, 0, 0);
    check_number(cpu.b.x, 1, "left-button press count");
    check_number(cpu.c.x, 56, "last press X");
    check_number(cpu.d.x, 88, "last press Y");
    mouse(5, 0, 0, 0);
    check_number(cpu.b.x, 0, "press count resets after reading");
    (void)bios_mouse_event(8, 12, 0);
    mouse(6, 0, 0, 0);
    check_number(cpu.b.x, 1, "left-button release count");
    check_number(cpu.c.x, 64, "last release X");
    check_number(cpu.d.x, 96, "last release Y");
    mouse(4, 0, 40, 56);
    mouse(3, 0, 0, 0);
    check_number(cpu.c.x, 40, "set mouse X in pixel units");
    check_number(cpu.d.x, 56, "set mouse Y in pixel units");
    mouse(7, 0, 16, 40);
    mouse(8, 0, 8, 24);
    (void)bios_mouse_event(79, 24, 0);
    mouse(3, 0, 0, 0);
    check_number(cpu.c.x, 40, "mouse horizontal upper range");
    check_number(cpu.d.x, 24, "mouse vertical upper range");
    (void)bios_mouse_event(0, 0, 0);
    mouse(3, 0, 0, 0);
    check_number(cpu.c.x, 16, "mouse horizontal lower range");
    check_number(cpu.d.x, 8, "mouse vertical lower range");
    mouse(0x21, 0, 0, 0);
    check_number(cpu.a.x, 0xffff, "mouse software reset installed");
    check_number(cpu.b.x, 2, "mouse software reset buttons");
    (void)bios_mouse_event(3, 2, 0);
    mouse(0x0b, 0, 0, 0);
    check_number(cpu.c.x, 24, "mouse horizontal motion counter");
    check_number(cpu.d.x, 16, "mouse vertical motion counter");
    mouse(0x0b, 0, 0, 0);
    check_number(cpu.c.x, 0, "reading clears horizontal motion counter");
    check_number(cpu.d.x, 0, "reading clears vertical motion counter");
    (void)bios_mouse_event(1, 1, 0);
    mouse(0x0b, 0, 0, 0);
    check_number(cpu.c.x, (uint16_t)-16, "negative horizontal mouse motion");
    check_number(cpu.d.x, (uint16_t)-8, "negative vertical mouse motion");
}

static void test_sgr_mouse_input(void)
{
    reset();
    feed("\033[<0;8;12M", 100);
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 1, "SGR mouse press maps left button");
    check_number(cpu.c.x, 56, "SGR coordinates are one-based columns");
    check_number(cpu.d.x, 88, "SGR coordinates are one-based rows");
    check_number(pop_raw(), 0x10000, "mouse event is not a keyboard key");
    feed("\033[<32;9;13M", 101);
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 1, "SGR drag preserves pressed button");
    check_number(cpu.c.x, 64, "SGR drag updates X");
    feed("\033[<0;9;13m", 102);
    click_settles();
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 0, "SGR release clears button");
    feed("\033[<2;10;14M", 103);
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 2, "SGR right button maps DOS button bit one");
    mouse(5, 1, 0, 0);
    check_number(cpu.b.x, 1, "SGR right press increments its counter");
    clear_input();
    feed("\033[<2;10;", 200);
    check_number(pop_raw(), 0x10000, "split mouse sequence queues no key");
    feed("14m", 210);
    click_settles();
    mouse(3, 0, 0, 0);
    check_number(cpu.b.x, 0, "split SGR release completes");
}

static void test_console_io(void)
{
    unsigned cursor;
    reset();
    (void)bios_key_push(0x1e61);
    CHECK(console(1), "DOS AH01 handled");
    check_number(cpu.a.l, 'a', "DOS AH01 returns character");
    check_number(mem[cell(0,0)], 'a', "DOS AH01 echoes character");
    cursor = word(0x450);
    (void)bios_key_push(0x3062);
    CHECK(console(7), "DOS AH07 handled");
    check_number(cpu.a.l, 'b', "DOS AH07 returns character");
    check_number(word(0x450), cursor, "DOS AH07 does not echo");
    (void)bios_key_push(0x2e63);
    CHECK(console(8), "DOS AH08 handled");
    check_number(cpu.a.l, 'c', "DOS AH08 returns character");
    check_number(word(0x450), cursor, "DOS AH08 does not echo");
    (void)bios_key_push(0x48e0);
    (void)console(8);
    check_number(cpu.a.l, 0, "extended console key first returns zero");
    (void)console(0x0b);
    check_number(cpu.a.l, 0xff, "pending scan byte counts as input available");
    (void)console(8);
    check_number(cpu.a.l, 0x48, "extended console key next returns scan");
    (void)console(0x0b);
    check_number(cpu.a.l, 0, "DOS input status empty");
    cpu.d.l = 0xff;
    CHECK(console(6), "DOS AH06 direct input handled");
    check_number(cpu.zf, 1, "DOS direct input empty ZF");
    check_number(cpu.a.l, 0, "DOS direct input empty AL");
    (void)bios_key_push(0x2074);
    cpu.d.l = 0xff;
    (void)console(6);
    check_number(cpu.zf, 0, "DOS direct input nonempty ZF");
    check_number(cpu.a.l, 't', "DOS direct input value");
    cpu.d.l = 'Q';
    CHECK(console(2), "DOS AH02 output handled");
    check_number(cpu.a.l, 'Q', "DOS output returns last character");
    check_number(mem[cell(0,1)], 'Q', "DOS AH02 writes at BIOS cursor");
    cpu.d.l = 'R';
    (void)console(6);
    check_number(mem[cell(0,2)], 'R', "DOS AH06 direct output");
    cpu.ds = 0x1000;
    cpu.d.x = 0x100;
    memcpy(mem + lin(cpu.ds,cpu.d.x), "hi$ignored", 10);
    CHECK(console(9), "DOS AH09 string output handled");
    check_number(mem[cell(0,3)], 'h', "DOS dollar string first byte");
    check_number(mem[cell(0,4)], 'i', "DOS dollar string second byte");
    check_number(word(0x450), 5, "DOS dollar terminator stops output");
    check_number(cpu.a.l, '$', "DOS AH09 returns dollar terminator");
}

static void test_console_line_and_flush(void)
{
    unsigned address;
    reset();
    cpu.ds = 0x1000;
    cpu.d.x = 0x120;
    address = lin(cpu.ds,cpu.d.x);
    mem[address] = 8;
    feed("abc\bD\033", 100);
    term_expire_input(130);
    feed("xy\r", 150);
    CHECK(console(0x0a), "DOS buffered line input handled");
    check_number(mem[address + 1], 2, "Backspace and Esc line editing length");
    CHECK(memcmp(mem + address + 2, "xy\r", 3) == 0,
          "Esc clears line and Enter appends CR outside counted length");
    clear_input();
    mem[address] = 3;
    mem[address + 1] = 0;
    feed("abcd\r", 200);
    (void)console(0x0a);
    check_number(mem[address + 1], 2, "DOS line capacity reserves final CR");
    CHECK(memcmp(mem + address + 2, "ab\r", 3) == 0,
          "DOS buffered line input does not exceed maximum");
    check_number(pop_raw(), 0x10000, "overflow characters consumed through Enter");
    (void)bios_key_push(0x1e61);
    (void)bios_key_push(0x3062);
    cpu.a.l = 6;
    cpu.d.l = 0xff;
    CHECK(console(0x0c), "DOS flush-and-read handled");
    check_number(word(0x41a), word(0x41c), "DOS flush clears real BIOS buffer");
    check_number(cpu.zf, 1, "DOS flush invokes requested direct read");
    (void)bios_key_push(0x48e0);
    (void)console(8);
    check_number(cpu.a.l, 0, "pending extended byte established");
    cpu.a.l = 6;
    cpu.d.l = 0xff;
    (void)console(0x0c);
    check_number(cpu.zf, 1, "DOS flush also clears pending extended scan byte");
    (void)console(0x0b);
    check_number(cpu.a.l, 0, "pending console input remains flushed");
    feed("\033[?1u\033[57441;2:1u", 300);
    CHECK(mem[0x417] & 2, "held Shift established before DOS flush");
    (void)bios_key_push(0x1e61);
    cpu.a.l = 6;
    cpu.d.l = 0xff;
    (void)console(0x0c);
    CHECK(mem[0x417] & 2, "DOS flush preserves held modifier state");
    check_number(word(0x41a), word(0x41c), "DOS flush removes queued words with held Shift");
    feed("\033[57441;1:3u", 301);
    check_number(mem[0x417] & 3, 0, "held Shift still releases normally after DOS flush");
}

static void test_noops(void)
{
    static const unsigned video_noops[] = {0x1000,0x1001,0x1010,0x1100,0xfe00,0xff00,0xee00};
    static const unsigned keyboard_noops[] = {0x0300,0x0400,0x0600,0x1300,0xff00};
    static const unsigned mouse_noops[] = {0x000a,0x000c,0x0014,0x00ff};
    static const unsigned console_unknown[] = {0x00,0x03,0x04,0x05,0x0d,0x30,0xff};
    unsigned i;
    reset();
    for (i = 0; i < sizeof(video_noops) / sizeof(video_noops[0]); ++i) {
        Cpu before;
        memset(&cpu, 0x55, sizeof(cpu));
        cpu.a.x = (uint16_t)video_noops[i];
        before = cpu;
        bios_int10();
        CHECK(memcmp(&cpu,&before,sizeof(cpu)) == 0,
              "video unsupported/TopView no-op preserves all registers including ES:DI");
    }
    for (i = 0; i < sizeof(keyboard_noops) / sizeof(keyboard_noops[0]); ++i) {
        Cpu before;
        memset(&cpu, 0x55, sizeof(cpu));
        cpu.a.x = (uint16_t)keyboard_noops[i];
        before = cpu;
        bios_int16();
        CHECK(memcmp(&cpu,&before,sizeof(cpu)) == 0, "unsupported INT 16h is no-op");
    }
    for (i = 0; i < sizeof(mouse_noops) / sizeof(mouse_noops[0]); ++i) {
        Cpu before;
        memset(&cpu, 0x55, sizeof(cpu));
        cpu.a.x = (uint16_t)mouse_noops[i];
        before = cpu;
        bios_int33();
        CHECK(memcmp(&cpu,&before,sizeof(cpu)) == 0, "mouse shape/handlers/unknown are no-ops");
    }
    for (i = 0; i < sizeof(console_unknown) / sizeof(console_unknown[0]); ++i) {
        Cpu before;
        memset(&cpu, 0x55, sizeof(cpu));
        cpu.a.h = (uint8_t)console_unknown[i];
        before = cpu;
        check_number((unsigned)dos_con_int21(), 0, "unhandled console function returns zero");
        CHECK(memcmp(&cpu,&before,sizeof(cpu)) == 0, "unhandled console function never touches cpu");
    }
}

/* PTY lifecycle tests use glibc's POSIX PTY interface, not libutil. Every
 * action is acknowledged over a separate pipe after its terminal writes;
 * poll drains output while waiting, so a repaint cannot fill the PTY and
 * deadlock the test. Deadlines turn broken child behavior into named failures. */
typedef struct {
    int master, slave, command, response;
    pid_t child;
    struct termios original;
} PtyCase;

static uint64_t monotonic_ms(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
        perror("test_term: clock_gettime");
        exit(2);
    }
    return (uint64_t)stamp.tv_sec * 1000 + (uint64_t)stamp.tv_nsec / 1000000;
}

static int write_bytes(int descriptor, const void *bytes, size_t length)
{
    const uint8_t *position = bytes;
    uint64_t deadline = monotonic_ms() + 3000;
    while (length) {
        ssize_t written = write(descriptor, position, length);
        if (written > 0) {
            position += (size_t)written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            monotonic_ms() < deadline) {
            struct pollfd descriptor_poll = {descriptor, POLLOUT, 0};
            if (poll(&descriptor_poll, 1, 100) < 0 && errno != EINTR)
                return 0;
            continue;
        }
        return 0;
    }
    return 1;
}

static void close_descriptor(int *descriptor)
{
    if (*descriptor >= 0) {
        (void)close(*descriptor);
        *descriptor = -1;
    }
}

static void pty_dispose(PtyCase *terminal)
{
    if (terminal->child > 0) {
        int status;
        (void)kill(terminal->child, SIGKILL);
        while (waitpid(terminal->child, &status, 0) < 0 && errno == EINTR)
            ;
        terminal->child = -1;
    }
    close_descriptor(&terminal->master);
    close_descriptor(&terminal->slave);
    close_descriptor(&terminal->command);
    close_descriptor(&terminal->response);
}

static int pty_drain(PtyCase *terminal)
{
    char buffer[4096];
    for (;;) {
        ssize_t received = read(terminal->master, buffer, sizeof(buffer));
        if (received > 0) {
            capture(buffer, (size_t)received, NULL);
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        if (received == 0 || (received < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)))
            return 1;
        CHECK(0, "reading PTY output failed");
        return 0;
    }
}

static int pty_response(PtyCase *terminal, uint8_t *bytes, size_t length)
{
    uint64_t deadline = monotonic_ms() + 3000;
    while (length) {
        struct pollfd descriptors[2] = {
            {terminal->master, POLLIN, 0},
            {terminal->response, POLLIN, 0}
        };
        if (!pty_drain(terminal))
            return 0;
        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            CHECK(0, "PTY child acknowledgement timed out after 3 seconds");
            return 0;
        }
        int timeout = (int)(deadline - now);
        if (timeout > 1000)
            timeout = 1000;
        int ready = poll(descriptors, 2, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            CHECK(0, "polling PTY child failed");
            return 0;
        }
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            ssize_t received = read(terminal->response, bytes, length);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0) {
                CHECK(0, "PTY child exited before acknowledging the requested action");
                return 0;
            }
            bytes += (size_t)received;
            length -= (size_t)received;
        }
    }
    return pty_drain(terminal);
}

static int pty_acknowledged(PtyCase *terminal, uint8_t command)
{
    uint8_t acknowledgement;
    if (!pty_response(terminal, &acknowledgement, 1))
        return 0;
    check_number(acknowledgement, command, "PTY child completed requested action");
    return acknowledgement == command;
}

static int pty_command(PtyCase *terminal, uint8_t command)
{
    if (!write_bytes(terminal->command, &command, 1)) {
        CHECK(0, "sending command to PTY child failed");
        return 0;
    }
    return pty_acknowledged(terminal, command);
}

static int pty_input(PtyCase *terminal, const char *input)
{
    if (!write_bytes(terminal->master, input, strlen(input))) {
        CHECK(0, "injecting terminal input into PTY failed");
        return 0;
    }
    return pty_command(terminal, 'K');
}

static int pty_keyboard_state(PtyCase *terminal, uint8_t state[4])
{
    return pty_command(terminal, 'G') && pty_response(terminal, state, 4);
}

static int pty_blocking_key(PtyCase *terminal, uint8_t command,
                            const char *input, unsigned expected,
                            const char *description)
{
    uint8_t key[2];
    /* The first acknowledgement is a readiness barrier before the blocking
     * BIOS call; only then does the parent inject input into the actual tty. */
    if (!pty_command(terminal, command))
        return 0;
    if (!write_bytes(terminal->master, input, strlen(input))) {
        CHECK(0, "injecting key for blocking BIOS read failed");
        return 0;
    }
    if (!pty_acknowledged(terminal, command) || !pty_response(terminal, key, 2))
        return 0;
    unsigned actual = key[0] | ((unsigned)key[1] << 8);
    check_number(actual, expected, description);
    return actual == expected;
}

static void pty_child(int commands, int responses)
{
    uint8_t acknowledgement = 'I';
    reset();
    term_set_output(NULL, NULL);
    put_word(cell(0,0), 0x0750);
    term_init();
    if (!write_bytes(responses, &acknowledgement, 1))
        _exit(110);
    for (;;) {
        uint8_t command;
        ssize_t received = read(commands, &command, 1);
        if (received < 0 && errno == EINTR)
            continue;
        if (received != 1) {
            term_shutdown();
            _exit(111);
        }
        switch (command) {
        case 'R': term_render(); break;
        case 'K': term_idle(0); break;
        case 'S': term_suspend(); break;
        case 'U': term_resume(); break;
        case 'W':
            if (raise(SIGWINCH) != 0)
                _exit(112);
            term_render();
            break;
        case 'D': term_shutdown(); break;
        case 'C':
            cpu.a.x = 0x0c06;
            cpu.d.l = 0xff;
            (void)dos_con_int21();
            break;
        case 'E':
            put_word(0x41a, 0x1e);
            put_word(0x41c, 0x1e);
            break;
        case 'F':
            put_word(0x41a, 0x1e);
            put_word(0x41c, 0x1e);
            for (unsigned i = 0; i < 15; ++i)
                (void)bios_key_push(0x1e61);
            break;
        case 'H':
        case 'L': {
            /* Acknowledge readiness before entering an initially empty-ring
             * blocking read, then acknowledge completion with its BIOS word. */
            if (!write_bytes(responses, &command, 1))
                _exit(120);
            cpu.a.h = command == 'H' ? 0x10 : 0x00;
            bios_int16();
            uint8_t result[] = {command,cpu.a.l,cpu.a.h};
            if (!write_bytes(responses, result, sizeof(result)))
                _exit(121);
            continue;
        }
        case 'G': {
            uint8_t state[] = {'G',mem[0x417],mem[0x418],mem[0x496],
                (uint8_t)(((word(0x41c) + 32 - word(0x41a)) % 32) / 2)};
            if (!write_bytes(responses, state, sizeof(state)))
                _exit(113);
            continue;
        }
        case 'A': exit(0); /* Deliberately rely on term_init's atexit hook. */
        case 'T':
            (void)raise(SIGTERM); /* Must terminate by signal, not _exit. */
            _exit(114);
        case 'X':
            term_shutdown();
            if (!write_bytes(responses, &command, 1))
                _exit(115);
            _exit(0);
        default:
            _exit(116);
        }
        if (!write_bytes(responses, &command, 1))
            _exit(117);
    }
}

static int pty_open(PtyCase *terminal)
{
    int commands[2] = {-1,-1}, responses[2] = {-1,-1};
    *terminal = (PtyCase){.master=-1,.slave=-1,.command=-1,.response=-1,.child=-1};
    terminal->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (terminal->master < 0) {
        CHECK(0, "posix_openpt unavailable: a real PTY is required, not skipped");
        fprintf(stderr, "PTY setup: %s\n", strerror(errno));
        goto failure;
    }
    if (grantpt(terminal->master) != 0 || unlockpt(terminal->master) != 0) {
        CHECK(0, "grantpt/unlockpt failed");
        goto failure;
    }
    char *slave_name = ptsname(terminal->master);
    if (!slave_name || (terminal->slave = open(slave_name, O_RDWR | O_NOCTTY)) < 0) {
        CHECK(0, "opening PTY slave failed");
        goto failure;
    }
    if (tcgetattr(terminal->slave, &terminal->original) != 0) {
        CHECK(0, "reading original PTY termios failed");
        goto failure;
    }
    terminal->original.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
    terminal->original.c_iflag |= IXON | ICRNL;
    terminal->original.c_oflag |= OPOST;
    terminal->original.c_cc[VMIN] = 1;
    terminal->original.c_cc[VTIME] = 0;
    if (tcsetattr(terminal->slave, TCSANOW, &terminal->original) != 0) {
        CHECK(0, "establishing canonical PTY baseline failed");
        goto failure;
    }
    struct winsize size = {.ws_row=25,.ws_col=80};
    if (ioctl(terminal->slave, TIOCSWINSZ, &size) != 0 ||
        pipe(commands) != 0 || pipe(responses) != 0) {
        CHECK(0, "creating PTY geometry/handshake pipes failed");
        goto failure;
    }
    (void)fflush(NULL);
    terminal->child = fork();
    if (terminal->child < 0) {
        CHECK(0, "forking PTY test child failed");
        goto failure;
    }
    if (terminal->child == 0) {
        (void)close(terminal->master);
        (void)close(commands[1]);
        (void)close(responses[0]);
        if (dup2(terminal->slave, STDIN_FILENO) < 0 ||
            dup2(terminal->slave, STDOUT_FILENO) < 0)
            _exit(118);
        if (terminal->slave > STDERR_FILENO)
            (void)close(terminal->slave);
        pty_child(commands[0], responses[1]);
        _exit(119);
    }
    close_descriptor(&commands[0]);
    close_descriptor(&responses[1]);
    terminal->command = commands[1];
    terminal->response = responses[0];
    commands[1] = responses[0] = -1;
    if (!pty_acknowledged(terminal, 'I'))
        goto failure;
    return 1;

failure:
    close_descriptor(&commands[0]);
    close_descriptor(&commands[1]);
    close_descriptor(&responses[0]);
    close_descriptor(&responses[1]);
    pty_dispose(terminal);
    return 0;
}

static int pty_exit(PtyCase *terminal, int *status)
{
    uint64_t deadline = monotonic_ms() + 3000;
    for (;;) {
        if (!pty_drain(terminal))
            return 0;
        pid_t waited = waitpid(terminal->child, status, WNOHANG);
        if (waited == terminal->child) {
            terminal->child = -1;
            return pty_drain(terminal);
        }
        if (waited < 0 && errno != EINTR) {
            CHECK(0, "waiting for PTY child exit failed");
            return 0;
        }
        if (monotonic_ms() >= deadline) {
            CHECK(0, "PTY child did not exit within 3 seconds");
            return 0;
        }
        struct pollfd descriptors[2] = {
            {terminal->master,POLLIN,0},{terminal->response,POLLIN,0}
        };
        if (poll(descriptors, 2, 100) < 0 && errno != EINTR) {
            CHECK(0, "polling PTY child exit failed");
            return 0;
        }
    }
}

static void check_raw(PtyCase *terminal)
{
    struct termios current;
    if (tcgetattr(terminal->slave, &current) != 0) {
        CHECK(0, "reading active PTY termios failed");
        return;
    }
    check_number(current.c_lflag & (ECHO | ICANON | ISIG | IEXTEN), 0,
                 "raw mode disables echo, canonical input, signals and extensions");
    check_number(current.c_iflag & (ICRNL | IXON), 0,
                 "raw mode disables CR translation and software flow control");
    check_number(current.c_oflag & OPOST, 0, "raw mode disables output translation");
    check_number(current.c_cc[VMIN], 0, "raw mode VMIN");
    check_number(current.c_cc[VTIME], 0, "raw mode VTIME");
}

static void check_restored(PtyCase *terminal)
{
    struct termios current;
    if (tcgetattr(terminal->slave, &current) != 0) {
        CHECK(0, "reading restored PTY termios failed");
        return;
    }
    CHECK(current.c_iflag == terminal->original.c_iflag &&
          current.c_oflag == terminal->original.c_oflag &&
          current.c_cflag == terminal->original.c_cflag &&
          current.c_lflag == terminal->original.c_lflag &&
          cfgetispeed(&current) == cfgetispeed(&terminal->original) &&
          cfgetospeed(&current) == cfgetospeed(&terminal->original) &&
          memcmp(current.c_cc, terminal->original.c_cc, sizeof(current.c_cc)) == 0,
          "cleanup restores every saved termios flag, speed and control character");
}

static void check_enter_modes(void)
{
    CHECK(strstr(output, "\033[?1049h") != NULL, "PTY enters alternate screen");
    CHECK(strstr(output, "\033=") != NULL, "PTY enters application keypad mode");
    CHECK(strstr(output, "\033[?1002h") != NULL, "PTY enables button-event mouse mode");
    CHECK(strstr(output, "\033[?1006h") != NULL, "PTY enables SGR mouse coordinates");
    CHECK(strstr(output, "\033[?u") != NULL, "PTY queries kitty support");
    CHECK(strstr(output, "\033[>11u") == NULL,
          "PTY must not enable kitty before an affirmative query reply");
}

static void check_leave_modes(int kitty)
{
    CHECK(strstr(output, "\033[?1049l") != NULL, "cleanup leaves alternate screen");
    CHECK(strstr(output, "\033>") != NULL, "cleanup leaves application keypad mode");
    CHECK(strstr(output, "\033[?1002l") != NULL, "cleanup disables button-event mouse mode");
    CHECK(strstr(output, "\033[?1006l") != NULL, "cleanup disables SGR mouse coordinates");
    CHECK(strstr(output, "\033[?25h") != NULL, "cleanup makes host cursor visible");
    CHECK(strstr(output, "\033[0m") != NULL, "cleanup resets character attributes");
    CHECK(strstr(output, "\033[?7h") != NULL, "cleanup restores terminal autowrap");
    if (kitty)
        CHECK(strstr(output, "\033[<u") != NULL, "cleanup pops negotiated kitty flags");
}

static void test_pty_lifecycle(void)
{
    PtyCase terminal;
    uint8_t state[4];
    int status;
    reset();
    if (!pty_open(&terminal))
        return;
    check_enter_modes();
    check_raw(&terminal);
    clear_output();
    if (!pty_command(&terminal, 'R'))
        goto done;
    CHECK(strstr(output, "\033[2J") != NULL && output_size > 2000,
          "first actual PTY frame draws complete screen");
    clear_output();
    if (!pty_command(&terminal, 'R'))
        goto done;
    check_number((unsigned)output_size, 0, "unchanged actual PTY frame emits nothing");
    if (!pty_input(&terminal, "\033[?1u"))
        goto done;
    CHECK(strstr(output, "\033[>11u") != NULL,
          "actual terminal reply enables exactly kitty flags 1|2|8");
    if (!pty_command(&terminal, 'F') ||
        !pty_input(&terminal, "\033[57441;2:1u") ||
        !pty_keyboard_state(&terminal, state))
        goto done;
    CHECK(state[0] & 2, "term_idle(0) ingests modifier press even with full BIOS ring");
    check_number(state[3], 15, "modifier press leaves full ring intact");
    if (!pty_command(&terminal, 'C') || !pty_keyboard_state(&terminal, state))
        goto done;
    CHECK(state[0] & 2, "actual DOS flush keeps physically held Shift");
    check_number(state[3], 0, "actual DOS flush empties ring without losing Shift");
    if (!pty_command(&terminal, 'F'))
        goto done;
    if (!pty_input(&terminal, "\033[57441;1:3u") ||
        !pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[0] & 3, 0, "term_idle(0) ingests modifier release with full BIOS ring");
    if (!pty_command(&terminal, 'E') || !pty_input(&terminal, "a") ||
        !pty_input(&terminal, "b") || !pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[3], 2, "term_idle(0) ingests new input with existing queued key");
    if (!pty_input(&terminal, "\033[57441;2:1u"))
        goto done;
    static const char release[] = "\033[57441;1:3u";
    if (!write_bytes(terminal.master, release, sizeof(release) - 1)) {
        CHECK(0, "queuing modifier release before DOS flush failed");
        goto done;
    }
    if (!pty_command(&terminal, 'C') || !pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[0] & 3, 0, "DOS flush ingests already queued terminal modifier releases");
    check_number(state[3], 0, "DOS flush removes keys after ingesting pending releases");
    if (!pty_command(&terminal, 'E'))
        goto done;
    clear_output();
    if (!pty_command(&terminal, 'S'))
        goto done;
    check_leave_modes(1);
    check_restored(&terminal);
    clear_output();
    if (!pty_command(&terminal, 'S'))
        goto done;
    check_number((unsigned)output_size, 0, "suspend is idempotent");
    if (!pty_command(&terminal, 'U'))
        goto done;
    check_enter_modes();
    check_raw(&terminal);
    CHECK(strstr(output, "\033[2J") != NULL && output_size > 2000,
          "resume redraws entire screen even when guest memory did not change");
    clear_output();
    if (!pty_command(&terminal, 'R'))
        goto done;
    check_number((unsigned)output_size, 0, "resume updates renderer shadow");
    struct winsize smaller = {.ws_row=24,.ws_col=80};
    CHECK(ioctl(terminal.slave, TIOCSWINSZ, &smaller) == 0,
          "test changes real host PTY geometry");
    if (!pty_command(&terminal, 'W'))
        goto done;
    CHECK(strstr(output, "\033[2J") != NULL && output_size > 1900,
          "SIGWINCH invalidates unchanged screen and redraws resized viewport");
    clear_output();
    if (!pty_input(&terminal, "\033[?0u"))
        goto done;
    CHECK(strstr(output, "\033[>11u") != NULL, "resume can negotiate kitty anew");
    clear_output();
    if (!pty_command(&terminal, 'D'))
        goto done;
    check_leave_modes(1);
    check_restored(&terminal);
    clear_output();
    if (!pty_command(&terminal, 'D'))
        goto done;
    check_number((unsigned)output_size, 0, "shutdown is idempotent");
    if (pty_command(&terminal, 'X') && pty_exit(&terminal, &status))
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "PTY lifecycle child exits cleanly");
done:
    pty_dispose(&terminal);
}

static void test_pty_atexit(void)
{
    PtyCase terminal;
    int status;
    uint8_t command = 'A';
    reset();
    if (!pty_open(&terminal))
        return;
    clear_output();
    if (!pty_input(&terminal, "\033[?1u"))
        goto done;
    CHECK(strstr(output, "\033[>11u") != NULL, "atexit child negotiated kitty");
    clear_output();
    if (!write_bytes(terminal.command, &command, 1)) {
        CHECK(0, "sending atexit command failed");
        goto done;
    }
    if (pty_exit(&terminal, &status)) {
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "exit() without explicit shutdown completes normally");
        check_leave_modes(1);
        check_restored(&terminal);
    }
done:
    pty_dispose(&terminal);
}

static void test_pty_blocking_keyboard(void)
{
    PtyCase terminal;
    uint8_t state[4];
    int status;
    reset();
    if (!pty_open(&terminal))
        return;
    if (!pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[3], 0, "blocking enhanced read begins with an empty BIOS ring");
    if (!pty_blocking_key(&terminal, 'H', "\033[A", 0x48e0,
                          "blocking AH10 ingests actual tty input and preserves E0"))
        goto done;
    if (!pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[3], 0, "blocking enhanced read consumes its only key");
    if (!pty_blocking_key(&terminal, 'L', "\033[23~a", 0x1e61,
                          "blocking AH00 hides actual tty F11 and returns following letter"))
        goto done;
    if (!pty_keyboard_state(&terminal, state))
        goto done;
    check_number(state[3], 0, "legacy blocking read discards F11 and consumes compatible key");
    if (pty_command(&terminal, 'X') && pty_exit(&terminal, &status))
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "blocking-keyboard PTY child exits cleanly");
done:
    pty_dispose(&terminal);
}

static void test_pty_fatal_signal(void)
{
    PtyCase terminal;
    int status;
    uint8_t command = 'T';
    reset();
    if (!pty_open(&terminal))
        return;
    clear_output();
    if (!pty_input(&terminal, "\033[?1u"))
        goto done;
    CHECK(strstr(output, "\033[>11u") != NULL, "signal child negotiated kitty");
    clear_output();
    if (!write_bytes(terminal.command, &command, 1)) {
        CHECK(0, "sending fatal-signal command failed");
        goto done;
    }
    if (pty_exit(&terminal, &status)) {
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
              "fatal cleanup preserves SIGTERM termination status");
        check_leave_modes(1);
        check_restored(&terminal);
    }
done:
    pty_dispose(&terminal);
}

#define RUN(function) do { ++groups; test_name = #function; function(); } while (0)

int main(void)
{
    RUN(test_bda);
    RUN(test_ascii_keys);
    RUN(test_control_alt_keys);
    RUN(test_function_keys);
    RUN(test_navigation_keys);
    RUN(test_utf8_keys);
    RUN(test_streaming_and_escape);
    RUN(test_kitty_modifiers);
    RUN(test_keyboard_ring);
    RUN(test_typeahead_survives_full_ring);
    RUN(test_legacy_keyboard);
    RUN(test_video_functions);
    RUN(test_font_switches);
    RUN(test_scroll_windows);
    RUN(test_teletype);
    RUN(test_renderer_palette);
    RUN(test_renderer_diffs_and_glyphs);
    RUN(test_renderer_mouse_and_blink);
    RUN(test_mouse_quick_click);
    RUN(test_mouse);
    RUN(test_sgr_mouse_input);
    RUN(test_console_io);
    RUN(test_console_line_and_flush);
    RUN(test_noops);
    RUN(test_pty_lifecycle);
    RUN(test_pty_atexit);
    RUN(test_pty_blocking_keyboard);
    RUN(test_pty_fatal_signal);
    term_set_output(NULL, NULL);
    printf("test_term: %u groups, %u checks, %u failures\n", groups, checks, failures);
    return failures ? 1 : 0;
}
