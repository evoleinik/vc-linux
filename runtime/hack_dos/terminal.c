/* ANSI-free IBM PC BIOS screen and keyboard for Hack 1.0.3.
 * Coordinates in Hack's terminal contract are one-based.
 */
#include <dos.h>
#include "hack.h"
#include "extern.h"

int CO = COLNO, LI = ROWNO + 2;
char *CD = "BIOS";
char morc;
static int screen_x, screen_y;
static unsigned char attribute = 7;

static void cursor(void)
{
    union REGS regs;
    memset(&regs, 0, sizeof(regs));
    regs.h.ah = 2;
    regs.h.dl = screen_x;
    regs.h.dh = screen_y;
    int86(0x10, &regs, &regs);
}

static void erase(int x1, int y1, int x2, int y2)
{
    union REGS regs;
    memset(&regs, 0, sizeof(regs));
    regs.h.ah = 6;
    regs.h.bh = 7;
    regs.h.cl = x1;
    regs.h.ch = y1;
    regs.h.dl = x2;
    regs.h.dh = y2;
    int86(0x10, &regs, &regs);
}

void curs(int x, int y)
{
    if (x < 1) x = 1;
    if (x > CO) x = CO;
    if (y < 1) y = 1;
    if (y > LI) y = LI;
    curx = x;
    cury = y;
    screen_x = x - 1;
    screen_y = y - 1;
    cursor();
}

void home(void) { curs(1, 1); }
void clearscreen(void) { erase(0, 0, 79, 24); home(); }
void cl_end(void) { erase(screen_x, screen_y, CO - 1, screen_y); }
void cl_eos(void)
{
    cl_end();
    if (screen_y + 1 < LI)
        erase(0, screen_y + 1, CO - 1, LI - 1);
}
void standoutbeg(void) { attribute = 0x70; }
void standoutend(void) { attribute = 7; }
void backsp(void)
{
    if (screen_x) --screen_x;
    if (curx > 1) --curx;
    cursor();
}
void sound_bell(void) { }
void delay_output(void) { }
void startscreen(void) { }
void endscreen(void) { attribute = 7; }
void getioctls(void) { }
void setioctls(void) { }
int dosuspend(void) { return 0; }
void gettty(void) { flags.echo = OFF; flags.cbreak = ON; }
void setftty(void) { gettty(); }
void startup(void)
{
    union REGS regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 3;
    int86(0x10, &regs, &regs);
    CO = COLNO;
    LI = ROWNO + 2;
    screen_x = screen_y = 0;
    set_whole_screen();
}
void settty(const char *s)
{
    endscreen();
    clearscreen();
    if (s) hack_fputs(s, stdout);
}

int hack_putchar(int ch)
{
    union REGS regs;
    int spaces;
    ch = (unsigned char)ch;
    if (ch == '\r') {
        screen_x = 0;
    } else if (ch == '\n') {
        screen_x = 0;
        ++screen_y;
    } else if (ch == '\b') {
        if (screen_x) --screen_x;
    } else if (ch == '\t') {
        spaces = 8 - screen_x % 8;
        while (spaces--) hack_putchar(' ');
        return ch;
    } else if (ch != '\a') {
        memset(&regs, 0, sizeof(regs));
        regs.h.ah = 9;
        regs.h.al = ch;
        regs.h.bl = attribute;
        regs.x.cx = 1;
        int86(0x10, &regs, &regs);
        if (++screen_x >= CO) {
            screen_x = 0;
            ++screen_y;
        }
    }
    if (screen_y >= LI) {
        memset(&regs, 0, sizeof(regs));
        regs.x.ax = 0x0601;
        regs.h.bh = 7;
        regs.h.dl = CO - 1;
        regs.h.dh = LI - 1;
        int86(0x10, &regs, &regs);
        screen_y = LI - 1;
    }
    cursor();
    return ch;
}

int hack_fputs(const char *s, FILE *stream)
{
    if (stream != stdout && stream != stderr)
        return fwrite(s, 1, strlen(s), stream) == strlen(s) ? 0 : EOF;
    while (*s) hack_putchar(*s++);
    return 0;
}
int hack_puts(const char *s)
{
    hack_fputs(s, stdout);
    return hack_putchar('\n');
}
int hack_vprintf(const char *format, va_list args)
{
    char text[2048];
    int result = vsnprintf(text, sizeof(text), format, args);
    text[sizeof(text) - 1] = 0;
    hack_fputs(text, stdout);
    return result;
}
int hack_printf(const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = hack_vprintf(format, args);
    va_end(args);
    return result;
}

int hack_mapchar(int x, int y, int ch)
{
    if (x >= 0 && x < COLNO && y >= 0 && y < ROWNO) {
        if (levl[x][y].typ == HWALL && ch == '-') return 196;
        if (levl[x][y].typ == VWALL && ch == '|') return 179;
    }
    return ch;
}

int hack_getchar(void)
{
    union REGS regs;
    int ch;
    do {
        memset(&regs, 0, sizeof(regs));
        int86(0x16, &regs, &regs);
        ch = regs.h.al;
        if (!ch) {
            switch (regs.h.ah) {
            case 0x47: ch = 'y'; break;
            case 0x48: ch = 'k'; break;
            case 0x49: ch = 'u'; break;
            case 0x4b: ch = 'h'; break;
            case 0x4d: ch = 'l'; break;
            case 0x4f: ch = 'b'; break;
            case 0x50: ch = 'j'; break;
            case 0x51: ch = 'n'; break;
            }
        }
    } while (!ch);
    return ch == '\r' ? '\n' : ch;
}

char readchar(void)
{
    int ch = hack_getchar();
    if (flags.toplin == 1) flags.toplin = 2;
    return ch;
}

void getlin(char *line)
{
    unsigned used = 0;
    int ch;
    flags.toplin = 2;
    for (;;) {
        ch = hack_getchar();
        if (ch == '\033') {
            line[0] = ch;
            line[1] = 0;
            return;
        }
        if (ch == '\n') {
            line[used] = 0;
            return;
        }
        if (ch == '\b') {
            if (used) { --used; putstr("\b \b"); }
        } else if (ch == 21 || ch == 127) {
            while (used) { --used; putstr("\b \b"); }
        } else if (ch >= ' ' && ch < 127 && used < COLNO - 1) {
            line[used++] = ch;
            putsym(ch);
        }
    }
}

void xwaitforspace(const char *extra)
{
    int ch;
    morc = 0;
    for (;;) {
        ch = readchar();
        if (ch == '\n' || ch == ' ') return;
        if (extra && strchr(extra, ch)) { morc = ch; return; }
    }
}
void cgetret(const char *extra)
{
    putsym('\n');
    putstr("Hit space to continue: ");
    xwaitforspace(extra);
}
void getret(void) { cgetret(""); }

char *parse(void)
{
    static char command[4];
    int ch;
    flags.move = 1;
    if (!Invisible) curs_on_u(); else home();
    while ((ch = readchar()) >= '0' && ch <= '9') {
        if (multi < 3276 || (multi == 3276 && ch <= '7'))
            multi = 10 * multi + ch - '0';
        else
            multi = 32767;
    }
    if (multi) { --multi; save_cm = command; }
    command[0] = ch;
    command[1] = 0;
    if (strchr("fFmM", ch)) {
        command[1] = hack_getchar();
        command[2] = 0;
    }
    clrlin();
    return command;
}

void error(const char *format, ...)
{
    va_list args;
    settty(NULL);
    va_start(args, format);
    hack_vprintf(format, args);
    va_end(args);
    hack_putchar('\n');
    exit(1);
}
void end_of_input(void)
{
    settty("End of input?\n");
    clearlocks();
    exit(0);
}
