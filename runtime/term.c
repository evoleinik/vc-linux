/* A small UTF-8/ANSI front end for the real B800 text buffer and BIOS ring.
 * Only the terminal owns host tty state; the BIOS owns all guest state. */
#define _POSIX_C_SOURCE 200809L
#include "term.h"
#include "cp866.h"
#include "bios.h"
#include "cpu.h"
#include "hle.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { SCREEN = 0xb8000, MAX_CELLS = 0x4000, ESC_DELAY = 30 };
enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4, MOD_CAPS = 64,
       MOD_NUM = 128 };

/* Screen glyphs: CP437 pictures for the control bytes 00h-1Fh and 7Fh, as a
 * VGA shows them, CP866 for the rest. NUL is the blank glyph, not a terminal
 * NUL byte. */
static const uint16_t control_glyphs[32] = {
    0x0020, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c,
    0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc,
};

static unsigned screen_ucs(uint8_t byte)
{
    if (byte < 32)
        return control_glyphs[byte];
    if (byte == 0x7f)
        return 0x2302;
    return cp866_to_ucs(byte);
}

static const unsigned char vga_to_ansi[16] = {
    0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15
};
static const unsigned char vga_rgb[16][3] = {
    {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
    {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
    {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
    {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}
};

static TermOutput output_sink;
static void *output_opaque;
static int truecolor, initialized, exit_registered, handlers_installed;
static volatile sig_atomic_t active, kitty_enabled, resize_pending;
static struct termios saved_termios;
static unsigned host_columns, host_rows;
static uint16_t shadow[MAX_CELLS]; /* Always raw guest cells, never mouse XOR. */
static int shadow_valid, cursor_known, old_mouse_visible, old_blink;
static unsigned old_columns, old_rows, old_cursor, old_shape;
static unsigned old_mouse_column, old_mouse_row;
static int old_cursor_visible;
static char output_buffer[8192];
static size_t output_used;
static int output_batch, output_failed;
static uint8_t input_buffer[512];
static size_t input_used;
static uint64_t last_input_ms;
static unsigned held_modifiers, mouse_buttons;
static int input_eof;
static uint64_t monotonic_ms(void);

static const int fatal_signals[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGABRT, SIGSEGV, SIGBUS, SIGFPE,
    SIGILL, SIGPIPE, SIGXCPU, SIGXFSZ
};
#define FATAL_COUNT (sizeof(fatal_signals) / sizeof(fatal_signals[0]))
static struct sigaction saved_fatal[FATAL_COUNT], saved_winch;
static const char leave_modes[] =
    "\033[?1006l\033[?1002l\033>\033[0m\033[?25h\033[?7h\033[?1049l";

static void block_lifecycle_signals(sigset_t *previous)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    for (size_t i = 0; i < FATAL_COUNT; ++i)
        sigaddset(&blocked, fatal_signals[i]);
    sigaddset(&blocked, SIGWINCH);
    sigprocmask(SIG_BLOCK, &blocked, previous);
}

static void write_terminal(const char *data, size_t n)
{
    while (n) {
        ssize_t written = write(STDOUT_FILENO, data, n);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            output_failed = 1;
            return;
        }
        data += written;
        n -= (size_t)written;
    }
}

static void flush_output(void)
{
    if (!output_used)
        return;
    if (output_sink)
        output_sink(output_buffer, output_used, output_opaque);
    else if (active)
        write_terminal(output_buffer, output_used);
    output_used = 0;
}

static void emit(const char *data, size_t n)
{
    while (n) {
        size_t part = sizeof(output_buffer) - output_used;
        if (part > n)
            part = n;
        memcpy(output_buffer + output_used, data, part);
        output_used += part;
        data += part;
        n -= part;
        if (output_used == sizeof(output_buffer))
            flush_output();
    }
    if (!output_batch)
        flush_output();
}

static void emit_string(const char *s)
{
    emit(s, strlen(s));
}

void term_invalidate(void)
{
    shadow_valid = 0;
    cursor_known = 0;
}

void term_set_output(TermOutput output, void *opaque)
{
    flush_output();
    output_sink = output;
    output_opaque = opaque;
    term_invalidate();
}

void term_set_truecolor(int enabled)
{
    truecolor = !!enabled;
    term_invalidate();
}

void term_bell(void)
{
    emit("\a", 1);
}

static void emit_position(unsigned column, unsigned row)
{
    char sequence[40];
    int n = snprintf(sequence, sizeof(sequence), "\033[%u;%uH", row + 1,
                     column + 1);
    emit(sequence, (size_t)n);
}

static void emit_attribute(unsigned attribute, int blink)
{
    unsigned fg = attribute & 15, bg = (attribute >> 4) & (blink ? 7 : 15);
    const char *flashing = blink && (attribute & 0x80) ? ";5" : "";
    char sequence[100];
    int n;
    if (truecolor) {
        n = snprintf(sequence, sizeof(sequence),
                     "\033[0;38;2;%u;%u;%u;48;2;%u;%u;%u%sm",
                     vga_rgb[fg][0], vga_rgb[fg][1], vga_rgb[fg][2],
                     vga_rgb[bg][0], vga_rgb[bg][1], vga_rgb[bg][2], flashing);
    } else {
        unsigned ansi_fg = vga_to_ansi[fg], ansi_bg = vga_to_ansi[bg];
        n = snprintf(sequence, sizeof(sequence), "\033[0;%u;%u%sm",
                     (ansi_fg < 8 ? 30 : 90) + (ansi_fg & 7),
                     (ansi_bg < 8 ? 40 : 100) + (ansi_bg & 7), flashing);
    }
    emit(sequence, (size_t)n);
}

static void emit_glyph(uint8_t byte)
{
    unsigned code = screen_ucs(byte);
    char utf8[3];
    size_t n;
    if (code < 0x80) {
        utf8[0] = (char)code;
        n = 1;
    } else if (code < 0x800) {
        utf8[0] = (char)(0xc0 | (code >> 6));
        utf8[1] = (char)(0x80 | (code & 63));
        n = 2;
    } else {
        utf8[0] = (char)(0xe0 | (code >> 12));
        utf8[1] = (char)(0x80 | ((code >> 6) & 63));
        utf8[2] = (char)(0x80 | (code & 63));
        n = 3;
    }
    emit(utf8, n);
}

static void read_host_size(void)
{
    struct winsize size;
    if (active && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        host_columns = size.ws_col;
        host_rows = size.ws_row;
    }
}

/* VC_SCREEN_DUMP=path: after every render, write the text screen as UTF-8.
 * Tests compare it with what a terminal emulator shows. */
static void dump_screen(void)
{
    static const char *path;
    static int checked;
    if (!checked) {
        path = getenv("VC_SCREEN_DUMP");
        checked = 1;
    }
    if (!path)
        return;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    unsigned columns = bios_columns(), rows = bios_rows();
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned column = 0; column < columns; ++column) {
            unsigned code = screen_ucs(mem[SCREEN + 2 * (row * columns + column)]);
            if (code < 0x80) fputc((int)code, f);
            else if (code < 0x800) { fputc(0xc0 | (code >> 6), f); fputc(0x80 | (code & 63), f); }
            else { fputc(0xe0 | (code >> 12), f); fputc(0x80 | ((code >> 6) & 63), f); fputc(0x80 | (code & 63), f); }
        }
        fputc('\n', f);
    }
    fclose(f);
    rename(tmp, path);
}

void term_render(void)
{
    dump_screen();
    if (!active && !output_sink)
        return;
    if (resize_pending) {
        resize_pending = 0;
        read_host_size();
        term_invalidate();
    }
    unsigned columns = bios_columns(), rows = bios_rows();
    unsigned view_columns = columns, view_rows = rows;
    /* A small host window is clipped, not allowed to scroll the emulated
     * screen. SIGWINCH invalidates the whole shadow when it grows again. */
    if (active && host_columns && view_columns > host_columns)
        view_columns = host_columns;
    if (active && host_rows && view_rows > host_rows)
        view_rows = host_rows;
    int blink = bios_blink_enabled();
    if (columns != old_columns || rows != old_rows || blink != old_blink)
        term_invalidate();
    unsigned mouse_column = 0, mouse_row = 0;
    int mouse_visible = bios_mouse_cell(&mouse_column, &mouse_row);
    int mouse_changed = mouse_visible != old_mouse_visible ||
        (mouse_visible && (mouse_column != old_mouse_column ||
                           mouse_row != old_mouse_row));
    int drew = 0, last_attribute = -1;
    output_failed = 0;
    output_batch = 1;
    if (!shadow_valid)
        emit_string("\033[0m\033[2J");
    for (unsigned row = 0; row < view_rows; ++row) {
        int contiguous = 0;
        for (unsigned column = 0; column < view_columns; ++column) {
            size_t cell = (size_t)row * columns + column;
            uint16_t raw = (uint16_t)(mem[SCREEN + 2 * cell] |
                                     (mem[SCREEN + 2 * cell + 1] << 8));
            int under_mouse = mouse_visible && column == mouse_column &&
                              row == mouse_row;
            int was_mouse = old_mouse_visible && column == old_mouse_column &&
                            row == old_mouse_row;
            if (shadow_valid && shadow[cell] == raw &&
                !(mouse_changed && (under_mouse || was_mouse))) {
                contiguous = 0;
                continue;
            }
            if (!drew)
                emit_string("\033[?25l");
            drew = 1;
            if (!contiguous)
                emit_position(column, row);
            unsigned attribute = raw >> 8;
            if (under_mouse)
                attribute ^= 0x77; /* VC's DOS software-cursor XOR mask. */
            if ((int)attribute != last_attribute) {
                emit_attribute(attribute, blink);
                last_attribute = (int)attribute;
            }
            emit_glyph((uint8_t)raw);
            shadow[cell] = raw;
            contiguous = 1;
        }
    }
    unsigned cursor = rd16(0x40, 0x50), shape = rd16(0x40, 0x60);
    unsigned cursor_column = cursor & 255, cursor_row = cursor >> 8;
    int cursor_visible = !(shape & 0x2000) && cursor_column < view_columns &&
                         cursor_row < view_rows;
    if (drew || !cursor_known || cursor != old_cursor || shape != old_shape ||
        cursor_visible != old_cursor_visible) {
        if (cursor_visible) {
            emit_position(cursor_column, cursor_row);
            emit_string("\033[?25h");
        } else if (!drew) {
            emit_string("\033[?25l");
        }
    }
    flush_output();
    output_batch = 0;
    shadow_valid = cursor_known = !output_failed;
    old_columns = columns;
    old_rows = rows;
    old_blink = blink;
    old_cursor = cursor;
    old_shape = shape;
    old_cursor_visible = cursor_visible;
    old_mouse_visible = mouse_visible;
    old_mouse_column = mouse_column;
    old_mouse_row = mouse_row;
}

static void signal_write(const char *data, size_t n)
{
    while (n) {
        ssize_t written = write(STDOUT_FILENO, data, n);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        data += written;
        n -= (size_t)written;
    }
}

/* Fatal-signal cleanup uses only async-signal-safe operations, no stdio,
 * output callback, renderer, allocation, or mutable output-buffer contents. */
static void fatal_signal(int number)
{
    if (active) {
        if (kitty_enabled)
            signal_write("\033[<u", 4);
        signal_write(leave_modes, sizeof(leave_modes) - 1);
        while (tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) < 0 &&
               errno == EINTR) {}
        active = 0;
    }
    struct sigaction action;
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    sigaction(number, &action, NULL);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, number);
    sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
    kill(getpid(), number);
    _exit(128 + number);
}

static void window_signal(int number)
{
    (void)number;
    resize_pending = 1;
}

static void install_handlers(void)
{
    if (handlers_installed)
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = fatal_signal;
    for (size_t i = 0; i < FATAL_COUNT; ++i)
        sigaddset(&action.sa_mask, fatal_signals[i]);
    for (size_t i = 0; i < FATAL_COUNT; ++i)
        sigaction(fatal_signals[i], &action, &saved_fatal[i]);
    action.sa_handler = window_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, &saved_winch);
    handlers_installed = 1;
}

static void acquire_terminal(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &saved_termios) != 0)
        return;
    struct termios raw = saved_termios;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                     ICRNL | IXON | IXOFF | IXANY);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    install_handlers();
    sigset_t previous;
    block_lifecycle_signals(&previous);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        sigprocmask(SIG_SETMASK, &previous, NULL);
        return;
    }
    active = 1;
    input_eof = 0;
    term_reset_input();
    read_host_size();
    emit_string("\033[?1049h\033[?7l\033=\033[?1002h\033[?1006h\033[?25l\033[?u");
    term_invalidate();
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void term_init(void)
{
    if (initialized)
        return;
    initialized = 1;
    if (!exit_registered) {
        atexit(term_shutdown);
        exit_registered = 1;
    }
    const char *color = getenv("COLORTERM");
    term_set_truecolor(color && (!strcasecmp(color, "truecolor") ||
                                !strcasecmp(color, "24bit")));
    acquire_terminal();
}

void term_suspend(void)
{
    if (!active)
        return;
    sigset_t previous;
    block_lifecycle_signals(&previous);
    flush_output();
    if (kitty_enabled) {
        emit_string("\033[<u");
        kitty_enabled = 0;
    }
    emit(leave_modes, sizeof(leave_modes) - 1);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    active = 0;
    term_reset_input();
    term_invalidate();
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void term_resume(void)
{
    if (!initialized)
        term_init();
    else if (!active)
        acquire_terminal();
    term_invalidate();
    term_render();
}

void term_shutdown(void)
{
    term_suspend();
    if (handlers_installed) {
        for (size_t i = 0; i < FATAL_COUNT; ++i)
            sigaction(fatal_signals[i], &saved_fatal[i], NULL);
        sigaction(SIGWINCH, &saved_winch, NULL);
        handlers_installed = 0;
    }
    initialized = 0;
}

static unsigned ascii_scan(unsigned code)
{
    static const uint8_t letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
        0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
        0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c
    };
    if (code >= 'A' && code <= 'Z')
        code += 'a' - 'A';
    if (code >= 'a' && code <= 'z')
        return letters[code - 'a'];
    if (code >= '1' && code <= '9')
        return code - '1' + 2;
    switch (code) {
    case '!': return 0x02;
    case '@': return 0x03;
    case '#': return 0x04;
    case '$': return 0x05;
    case '%': return 0x06;
    case '^': return 0x07;
    case '&': return 0x08;
    case '*': return 0x09;
    case '(': return 0x0a;
    case '0': case ')': return 0x0b;
    case '-': case '_': return 0x0c;
    case '=': case '+': return 0x0d;
    case '[': case '{': return 0x1a;
    case ']': case '}': return 0x1b;
    case ';': case ':': return 0x27;
    case '\'': case '"': return 0x28;
    case '`': case '~': return 0x29;
    case '\\': case '|': return 0x2b;
    case ',': case '<': return 0x33;
    case '.': case '>': return 0x34;
    case '/': case '?': return 0x35;
    case ' ': return 0x39;
    default: return 0;
    }
}

/* Keys wait here until the 15-key BIOS ring has room, so pasted text or fast
 * typing is never dropped. A real BIOS drops them, but a real keyboard is slow. */
static uint16_t pending[4096];
static size_t pending_head, pending_count;

static void feed_ring(void)
{
    while (pending_count && bios_key_push(pending[pending_head])) {
        pending_head = (pending_head + 1) % (sizeof pending / sizeof pending[0]);
        --pending_count;
    }
}

void term_clear_pending(void)
{
    pending_head = pending_count = 0;
}

static void queue_word(unsigned scan, unsigned ascii)
{
    const size_t cap = sizeof pending / sizeof pending[0];
    if (pending_count == cap)
        return;
    pending[(pending_head + pending_count++) % cap] = (uint16_t)((scan << 8) | ascii);
    feed_ring();
}

static unsigned shifted_ascii(unsigned code, unsigned modifiers)
{
    if (code >= 'a' && code <= 'z') {
        if (!!(modifiers & MOD_SHIFT) != !!(modifiers & MOD_CAPS))
            return code - 'a' + 'A';
    } else if (modifiers & MOD_SHIFT) {
        static const char plain[] = "1234567890-=[];'`,./\\";
        static const char shift[] = "!@#$%^&*()_+{}:\"~<>?|";
        const char *at = code < 128 ? strchr(plain, (int)code) : NULL;
        if (at)
            return (unsigned char)shift[at - plain];
    }
    return code;
}

static void queue_character(unsigned code, unsigned modifiers)
{
    unsigned scan;
    if (code == 13 || code == 10) {
        queue_word(0x1c, modifiers & MOD_ALT ? 0 :
                   ((modifiers & MOD_CTRL) || code == 10 ? 10 : 13));
        return;
    }
    if (code == 9) {
        if (modifiers & MOD_ALT)
            queue_word(0xa5, 0);
        else if (modifiers & MOD_CTRL)
            queue_word(0x94, 0);
        else
            queue_word(0x0f, modifiers & MOD_SHIFT ? 0 : 9);
        return;
    }
    if (code == 8 || code == 127) {
        queue_word(0x0e, modifiers & MOD_ALT ? 0 :
                   (modifiers & MOD_CTRL ? 127 : 8));
        return;
    }
    if (code == 27) {
        queue_word(1, modifiers & MOD_ALT ? 0 : 27);
        return;
    }
    if (code < 32) {
        /* C0 input is ambiguous: use the canonical DOS keys, as VC's own
         * ScanCodes table does. CSI-u retains the real Ctrl-letter scan. */
        static const uint8_t controls[32] = {
            0x03, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22,
            0x0e, 0x0f, 0x1c, 0x25, 0x26, 0x1c, 0x31, 0x18,
            0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11,
            0x2d, 0x15, 0x2c, 0x01, 0x2b, 0x1b, 0x07, 0x0c
        };
        queue_word(controls[code], modifiers & MOD_ALT ? 0 : code);
        return;
    }
    if (code < 128) {
        scan = ascii_scan(code);
        if (code == ' ') {
            queue_word(0x39, ' '); /* IBM BIOS Ctrl/Alt-Space is still Space. */
        } else if (modifiers & MOD_ALT) {
            if (scan >= 2 && scan <= 0x0d)
                scan += 0x76; /* Alt digits, minus and equals: 78h..83h. */
            queue_word(scan, 0);
        } else if (modifiers & MOD_CTRL) {
            unsigned lower = code >= 'A' && code <= 'Z' ? code + 32 : code;
            if (lower >= 'a' && lower <= 'z')
                queue_word(scan, lower - 'a' + 1);
            else if (code == '2' || code == '@')
                queue_word(0x03, 0);
            else if (code == '6' || code == '^')
                queue_word(0x07, 0x1e);
            else if (code == '-' || code == '_')
                queue_word(0x0c, 0x1f);
            else if (code == '[' || code == '{')
                queue_word(0x1a, 0x1b);
            else if (code == '\\' || code == '|')
                queue_word(0x2b, 0x1c);
            else if (code == ']' || code == '}')
                queue_word(0x1b, 0x1d);
            else if (code == '/' || code == '?')
                queue_word(0x95, 0);
            /* The other Ctrl digits/punctuation produce no IBM BIOS key. */
        } else {
            queue_word(scan, shifted_ascii(code, modifiers));
        }
        return;
    }
    if (!!(modifiers & MOD_SHIFT) != !!(modifiers & MOD_CAPS)) {
        if (code >= 0x430 && code <= 0x44f)
            code -= 0x20;
        else if (code == 0x451)
            code = 0x401;
    }
    /* The screen is a single-byte DOS code page, not a Unicode grid. Ignore
     * Unicode that CP866 cannot represent instead of inventing scan codes. */
    int byte = code >= 0x80 ? ucs_to_cp866(code) : -1;
    if (byte >= 0)
        queue_word(0, (unsigned)byte);
}

enum Navigation { NAV_UP, NAV_DOWN, NAV_RIGHT, NAV_LEFT, NAV_HOME, NAV_END,
                  NAV_PGUP, NAV_PGDN, NAV_INSERT, NAV_DELETE, NAV_BEGIN };

static void queue_navigation(enum Navigation key, unsigned modifiers)
{
    static const uint8_t plain[] = {
        0x48, 0x50, 0x4d, 0x4b, 0x47, 0x4f, 0x49, 0x51, 0x52, 0x53, 0x4c
    };
    static const uint8_t control[] = {
        0x8d, 0x91, 0x74, 0x73, 0x77, 0x75, 0x84, 0x76, 0x92, 0x93, 0x8f
    };
    static const uint8_t alternate[] = {
        0x98, 0xa0, 0x9d, 0x9b, 0x97, 0x9f, 0x99, 0xa1, 0xa2, 0xa3, 0x9c
    };
    if (modifiers & MOD_ALT)
        queue_word(alternate[key], 0);
    else
        queue_word(modifiers & MOD_CTRL ? control[key] : plain[key], 0xe0);
}

static void queue_function(unsigned number, unsigned modifiers)
{
    if (number < 1 || number > 12)
        return;
    unsigned kind = modifiers & MOD_ALT ? 3 :
                    (modifiers & MOD_CTRL ? 2 : (modifiers & MOD_SHIFT ? 1 : 0));
    static const unsigned first_ten[] = {0x3b, 0x54, 0x5e, 0x68};
    unsigned scan = number <= 10 ? first_ten[kind] + number - 1 :
                    0x85 + 2 * kind + number - 11;
    queue_word(scan, 0);
}

static void queue_keypad(unsigned code, unsigned modifiers)
{
    if (code == 57414) {
        queue_word(modifiers & MOD_ALT ? 0xa6 : 0xe0,
                   modifiers & MOD_ALT ? 0 : (modifiers & MOD_CTRL ? 10 : 13));
    } else if (code >= 57411 && code <= 57413) {
        static const unsigned scans[] = {0x37, 0x4a, 0x4e};
        static const unsigned controls[] = {0x96, 0x8e, 0x90};
        static const unsigned ascii[] = {'*', '-', '+'};
        unsigned index = code - 57411;
        queue_word(modifiers & MOD_CTRL ? controls[index] : scans[index],
                   modifiers & (MOD_ALT | MOD_CTRL) ? 0 : ascii[index]);
    } else if (code == 57410) {
        queue_word(modifiers & MOD_ALT ? 0xa4 :
                   (modifiers & MOD_CTRL ? 0x95 : 0xe0),
                   modifiers & (MOD_ALT | MOD_CTRL) ? 0 : '/');
    } else if (code >= 57399 && code <= 57408) {
        static const unsigned scans[] = {
            0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49
        };
        queue_word(scans[code - 57399], '0' + code - 57399);
    } else if (code == 57409) {
        queue_word(0x53, '.');
    }
}

/* The six physical modifier keys have separate state so releasing one Shift
 * does not release the other. Snapshot-only reports use the left-hand key. */
static unsigned modifier_bit(unsigned code)
{
    switch (code) {
    case 57441: return 1;  /* Left Shift. */
    case 57447: return 2;  /* Right Shift. */
    case 57442: return 4;  /* Left Ctrl. */
    case 57448: return 8;  /* Right Ctrl. */
    case 57443: return 16; /* Left Alt. */
    case 57449: return 32; /* Right Alt. */
    default: return 0;
    }
}

static void update_modifiers(unsigned modifiers, unsigned code, unsigned event)
{
    unsigned bit = modifier_bit(code);
    if (bit) {
        if (event == 3)
            held_modifiers &= ~bit;
        else
            held_modifiers |= bit;
    }
    static const unsigned groups[] = {3, 12, 48};
    static const unsigned modifier_flags[] = {MOD_SHIFT, MOD_CTRL, MOD_ALT};
    static const unsigned fallback[] = {1, 4, 16};
    for (unsigned i = 0; i < 3; ++i) {
        if (bit & groups[i]) {
            /* A snapshot may have inferred the wrong side before the first
             * explicit modifier event. An all-up release clears that too. */
            if (event == 3 && !(modifiers & modifier_flags[i]))
                held_modifiers &= ~groups[i];
            else if (event == 3 && !(held_modifiers & groups[i]))
                held_modifiers |= groups[i] & ~bit;
            continue;
        }
        if (!(modifiers & modifier_flags[i]))
            held_modifiers &= ~groups[i];
        else if (!(held_modifiers & groups[i]))
            held_modifiers |= fallback[i];
    }
    unsigned flags = ((held_modifiers & 1) ? 2 : 0) |
                     ((held_modifiers & 2) ? 1 : 0) |
                     ((held_modifiers & 12) ? 4 : 0) |
                     ((held_modifiers & 48) ? 8 : 0);
    mem[0x417] = (uint8_t)((mem[0x417] & 0x90) | flags |
                         ((modifiers & MOD_CAPS) ? 0x40 : 0) |
                         ((modifiers & MOD_NUM) ? 0x20 : 0));
    mem[0x418] = (uint8_t)((mem[0x418] & 0xfc) |
                         ((held_modifiers & 4) ? 1 : 0) |
                         ((held_modifiers & 16) ? 2 : 0));
    mem[0x496] = (uint8_t)((mem[0x496] & 0xf3) |
                         ((held_modifiers & 8) ? 4 : 0) |
                         ((held_modifiers & 32) ? 8 : 0));
}

static void queue_protocol_key(unsigned code, unsigned modifiers, unsigned event)
{
    if (kitty_enabled)
        update_modifiers(modifiers, code, event);
    if (event == 3 || modifier_bit(code))
        return;
    if (code >= 57364 && code <= 57375) {
        queue_function(code - 57364 + 1, modifiers);
    } else if (code >= 57399 && code <= 57414) {
        queue_keypad(code, modifiers);
    } else {
        switch (code) {
        case 57344: queue_character(27, modifiers); break;
        case 57345: queue_character(13, modifiers); break;
        case 57346: queue_character(9, modifiers); break;
        case 57347: queue_character(127, modifiers); break;
        case 57348: queue_navigation(NAV_INSERT, modifiers); break;
        case 57349: queue_navigation(NAV_DELETE, modifiers); break;
        case 57350: queue_navigation(NAV_LEFT, modifiers); break;
        case 57351: queue_navigation(NAV_RIGHT, modifiers); break;
        case 57352: queue_navigation(NAV_UP, modifiers); break;
        case 57353: queue_navigation(NAV_DOWN, modifiers); break;
        case 57354: queue_navigation(NAV_PGUP, modifiers); break;
        case 57355: queue_navigation(NAV_PGDN, modifiers); break;
        case 57356: queue_navigation(NAV_HOME, modifiers); break;
        case 57357: queue_navigation(NAV_END, modifiers); break;
        default:
            if (code < 57344 || code > 63743)
                queue_character(code, modifiers);
            break;
        }
    }
}

typedef struct {
    unsigned value[8][4];
    unsigned count;
    unsigned char prefix;
} Parameters;

static int parse_parameters(const uint8_t *bytes, size_t n, Parameters *p)
{
    memset(p, 0, sizeof(*p));
    if (n && bytes[0] >= '<' && bytes[0] <= '?') {
        p->prefix = bytes[0];
        ++bytes;
        --n;
    }
    if (!n)
        return 1;
    p->count = 1;
    unsigned sub = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = bytes[i];
        if (c == ';') {
            if (++p->count > 8)
                return 0;
            sub = 0;
        } else if (c == ':') {
            if (++sub >= 4)
                return 0;
        } else if (c >= '0' && c <= '9') {
            unsigned *value = &p->value[p->count - 1][sub];
            if (*value > (UINT_MAX - (c - '0')) / 10)
                return 0;
            *value = *value * 10 + c - '0';
        } else {
            return 0;
        }
    }
    return 1;
}

static void mouse_sequence(const Parameters *p, unsigned final)
{
    if (p->count != 3 || (final != 'M' && final != 'm'))
        return;
    unsigned button = p->value[0][0], column = p->value[1][0], row = p->value[2][0];
    if (!column || !row || column > 65536 || row > 65536 || (button & ~63u))
        return; /* Wheel/extra buttons are not one of the two DOS buttons. */
    unsigned base = button & 3;
    static const unsigned masks[] = {1, 4, 2, 0};
    if (base == 3)
        mouse_buttons = 0;
    else if (final == 'm')
        mouse_buttons &= ~masks[base];
    else
        mouse_buttons |= masks[base];
    if (kitty_enabled)
        update_modifiers(((button & 4) ? MOD_SHIFT : 0) |
                         ((button & 8) ? MOD_ALT : 0) |
                         ((button & 16) ? MOD_CTRL : 0) |
                         ((mem[0x417] & 0x40) ? MOD_CAPS : 0) |
                         ((mem[0x417] & 0x20) ? MOD_NUM : 0), 0, 1);
    bios_mouse_event(column - 1, row - 1, mouse_buttons);
}

static void control_sequence(const uint8_t *bytes, size_t n, int ss3,
                             unsigned extra_modifiers)
{
    Parameters p;
    unsigned final = bytes[n - 1];
    if (!parse_parameters(bytes + 2, n - 3, &p))
        return;
    if (!ss3 && p.prefix == '<') {
        mouse_sequence(&p, final);
        return;
    }
    if (!ss3 && p.prefix == '?' && final == 'u' && p.count) {
        if (!kitty_enabled) {
            /* Negotiate, do not blindly enable a protocol on older ttys. */
            sigset_t previous;
            block_lifecycle_signals(&previous);
            emit_string("\033[>11u");
            kitty_enabled = 1;
            sigprocmask(SIG_SETMASK, &previous, NULL);
        }
        return;
    }
    if (p.prefix)
        return;
    unsigned modifiers = p.count >= 2 && p.value[1][0] ? p.value[1][0] - 1 : 0;
    modifiers |= extra_modifiers;
    unsigned event = p.count >= 2 && p.value[1][1] ? p.value[1][1] : 1;
    if (event > 3)
        return;
    unsigned first = p.count ? p.value[0][0] : 0;
    if (!ss3 && final == 'u' && p.count) {
        unsigned code = first;
        if ((modifiers & (MOD_CTRL | MOD_ALT)) && p.value[0][2])
            code = p.value[0][2]; /* Base-layout key, when supplied. */
        else if ((modifiers & MOD_SHIFT) && p.value[0][1])
            code = p.value[0][1];
        queue_protocol_key(code, modifiers, event);
        return;
    }
    if (kitty_enabled)
        update_modifiers(modifiers, 0, event);
    if (event == 3)
        return;
    switch (final) {
    case 'A': queue_navigation(NAV_UP, modifiers); break;
    case 'B': queue_navigation(NAV_DOWN, modifiers); break;
    case 'C': queue_navigation(NAV_RIGHT, modifiers); break;
    case 'D': queue_navigation(NAV_LEFT, modifiers); break;
    case 'H': queue_navigation(NAV_HOME, modifiers); break;
    case 'F': queue_navigation(NAV_END, modifiers); break;
    case 'E': queue_navigation(NAV_BEGIN, modifiers); break;
    case 'P': case 'Q': case 'R': case 'S':
        queue_function(final - 'P' + 1, modifiers);
        break;
    case 'Z': queue_character(9, modifiers | MOD_SHIFT); break;
    case '~':
        if (first == 27 && p.count >= 3) {
            queue_protocol_key(p.value[2][0], modifiers, event);
            break; /* xterm modifyOtherKeys, CSI 27;modifier;code~. */
        }
        switch (first) {
        case 1: case 7: queue_navigation(NAV_HOME, modifiers); break;
        case 2: queue_navigation(NAV_INSERT, modifiers); break;
        case 3: queue_navigation(NAV_DELETE, modifiers); break;
        case 4: case 8: queue_navigation(NAV_END, modifiers); break;
        case 5: queue_navigation(NAV_PGUP, modifiers); break;
        case 6: queue_navigation(NAV_PGDN, modifiers); break;
        case 11: case 12: case 13: case 14: case 15:
            queue_function(first - 10, modifiers); break;
        case 17: case 18: case 19: case 20: case 21:
            queue_function(first - 11, modifiers); break;
        case 23: case 24: queue_function(first - 12, modifiers); break;
        case 25: case 26:
            queue_function(first - 24, modifiers | MOD_SHIFT); break;
        case 28: case 29:
            queue_function(first - 25, modifiers | MOD_SHIFT); break;
        case 31: case 32: case 33: case 34:
            queue_function(first - 26, modifiers | MOD_SHIFT); break;
        default: break;
        }
        break;
    default:
        if (ss3) {
            if (final >= 'p' && final <= 'y')
                queue_keypad(57399 + final - 'p', modifiers);
            else if (final == 'n')
                queue_keypad(57409, modifiers);
            else if (final == 'o')
                queue_keypad(57410, modifiers);
            else if (final == 'j')
                queue_keypad(57411, modifiers);
            else if (final == 'm')
                queue_keypad(57412, modifiers);
            else if (final == 'k')
                queue_keypad(57413, modifiers);
            else if (final == 'M')
                queue_keypad(57414, modifiers);
        }
        break;
    }
}

/* 0 means incomplete, positive is a decoded length, -1 an invalid lead or
 * continuation byte. Reject overlong, surrogate and out-of-range encodings. */
static int decode_utf8(const uint8_t *bytes, size_t n, unsigned *code)
{
    if (!n)
        return 0;
    if (bytes[0] < 128) {
        *code = bytes[0];
        return 1;
    }
    unsigned length, value, minimum;
    if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
        length = 2; value = bytes[0] & 31; minimum = 0x80;
    } else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
        length = 3; value = bytes[0] & 15; minimum = 0x800;
    } else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
        length = 4; value = bytes[0] & 7; minimum = 0x10000;
    } else {
        return -1;
    }
    for (unsigned i = 1; i < length; ++i) {
        if (i >= n)
            return 0;
        if ((bytes[i] & 0xc0) != 0x80)
            return -1;
        value = (value << 6) | (bytes[i] & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
        return -1;
    *code = value;
    return (int)length;
}

static size_t parse_input(void)
{
    unsigned code;
    if (input_buffer[0] != 27) {
        int n = decode_utf8(input_buffer, input_used, &code);
        if (n > 0)
            queue_character(code, 0);
        return n < 0 ? 1 : (size_t)n;
    }
    if (input_used < 2)
        return 0;
    size_t prefix = 0;
    if (input_buffer[1] == 27) {
        if (input_used == 2)
            return 0; /* Alt-Esc, or an Alt-prefixed CSI/SS3: wait to know. */
        prefix = 1;
    }
    const uint8_t *bytes = input_buffer + prefix;
    size_t length = input_used - prefix;
    unsigned modifiers = prefix ? MOD_ALT : 0;
    if (bytes[1] == '[' || bytes[1] == 'O') {
        if (length >= 3 && bytes[1] == '[' && bytes[2] == '[') {
            if (length < 4)
                return 0;
            if (bytes[3] >= 'A' && bytes[3] <= 'E')
                queue_function(bytes[3] - 'A' + 1, modifiers);
            return prefix + 4; /* Linux-console F1..F5. */
        }
        for (size_t i = 2; i < length; ++i) {
            if (bytes[i] == 27)
                return prefix + i; /* Resynchronize at a fresh escape. */
            if (bytes[i] >= 0x40 && bytes[i] <= 0x7e) {
                control_sequence(bytes, i + 1, bytes[1] == 'O', modifiers);
                return prefix + i + 1;
            }
        }
        return input_used == sizeof(input_buffer) ? input_used : 0;
    }
    if (prefix) {
        queue_character(27, MOD_ALT);
        return 2;
    }
    int n = decode_utf8(input_buffer + 1, input_used - 1, &code);
    if (n > 0)
        queue_character(code, MOD_ALT);
    return n < 0 ? 2 : (n == 0 ? 0 : (size_t)n + 1);
}

static void consume_input(size_t n)
{
    input_used -= n;
    memmove(input_buffer, input_buffer + n, input_used);
}

static int ambiguous_escape(void)
{
    return input_used && input_buffer[0] == 27 &&
           (input_used == 1 || (input_used == 2 &&
            (input_buffer[1] == 27 || input_buffer[1] == '[' ||
             input_buffer[1] == 'O')));
}

void term_expire_input(uint64_t now_ms)
{
    if (!ambiguous_escape() || now_ms < last_input_ms ||
        now_ms - last_input_ms < ESC_DELAY)
        return;
    if (input_used == 1)
        queue_character(27, 0);
    else
        queue_character(input_buffer[1], MOD_ALT); /* Alt-[ or Alt-O. */
    /* Once CSI parameters or a UTF-8 lead identify an in-progress sequence,
     * retain it across slow reads; only an ambiguous Esc has a 30 ms limit. */
    input_used = 0;
}

void term_feed_input(const uint8_t *data, size_t n, uint64_t now_ms)
{
    term_expire_input(now_ms); /* A late byte must not turn an old Esc into Alt. */
    if (!n)
        return;
    last_input_ms = now_ms;
    for (size_t i = 0; i < n; ++i) {
        if (input_used == sizeof(input_buffer))
            input_used = 0;
        input_buffer[input_used++] = data[i];
        size_t consumed;
        while (input_used && (consumed = parse_input()) != 0)
            consume_input(consumed);
    }
}

static void clear_modifiers(void)
{
    held_modifiers = 0;
    mem[0x417] &= 0xf0;
    mem[0x418] &= 0xfc;
    mem[0x496] &= 0xf3;
}

void term_reset_input(void)
{
    if (kitty_enabled) {
        sigset_t previous;
        block_lifecycle_signals(&previous);
        emit_string("\033[<u");
        kitty_enabled = 0;
        sigprocmask(SIG_SETMASK, &previous, NULL);
    }
    input_used = 0;
    last_input_ms = 0;
    mouse_buttons = 0;
    input_eof = 0;
    clear_modifiers();
}

void term_flush_input(void)
{
    /* DOS flush discards characters, not physical modifier state. Consume
     * the reports already queued in the tty so a flushed release cannot leave
     * Shift/Ctrl/Alt stuck. The caller then empties the resulting BIOS words. */
    int queued = 0;
    if (active && ioctl(STDIN_FILENO, FIONREAD, &queued) == 0) {
        while (queued > 0) {
            uint8_t bytes[1024];
            size_t wanted = (size_t)queued < sizeof(bytes) ?
                            (size_t)queued : sizeof(bytes);
            ssize_t n = read(STDIN_FILENO, bytes, wanted);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            term_feed_input(bytes, (size_t)n, monotonic_ms());
            queued -= (int)n;
        }
    } else if (active) {
        tcflush(STDIN_FILENO, TCIFLUSH);
    }
    input_used = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        return 0;
    return (uint64_t)time.tv_sec * 1000 + (unsigned long)time.tv_nsec / 1000000;
}

void term_idle(int timeout_ms)
{
    if (timeout_ms < 0)
        timeout_ms = 0;
    uint64_t deadline = monotonic_ms() + (unsigned)timeout_ms;
    term_render();
    feed_ring();
    for (;;) {
        uint64_t now = monotonic_ms();
        term_expire_input(now);
        int wait_ms = now < deadline ? (int)(deadline - now) : 0;
        /* Already-buffered keys avoid a wait, not the poll itself: otherwise
         * a full BIOS ring would starve release events and mouse reports. */
        if (rd16(0x40, 0x1a) != rd16(0x40, 0x1c))
            wait_ms = 0;
        if (ambiguous_escape()) {
            uint64_t expires = last_input_ms + ESC_DELAY;
            int esc_wait = expires > now ? (int)(expires - now) : 0;
            if (wait_ms > esc_wait)
                wait_ms = esc_wait;
        }
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        int no_input = input_eof || (initialized && !active);
        int ready = poll(no_input ? NULL : &descriptor, no_input ? 0 : 1, wait_ms);
        if (ready < 0 && errno != EINTR)
            return;
        if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
            uint8_t bytes[4096];
            ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n > 0) {
                term_feed_input(bytes, (size_t)n, monotonic_ms());
                term_render();
                if (!input_used || timeout_ms == 0)
                    return;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                input_eof = 1; /* poll(NULL) below prevents EOF/HUP hot loops. */
            }
        } else if (ready > 0 && (descriptor.revents & (POLLERR | POLLNVAL))) {
            input_eof = 1;
        }
        term_expire_input(monotonic_ms());
        term_render();
        if (monotonic_ms() >= deadline ||
            rd16(0x40, 0x1a) != rd16(0x40, 0x1c))
            return;
    }
}
