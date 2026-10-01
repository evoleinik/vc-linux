/* DOS machine-dependent shim for Rogue 5.4.4.  Gameplay remains upstream. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <curses.h>
#include "extern.h"

void md_init(void) { _fmode = O_BINARY; }
void md_onsignal_default(void) {}
void md_onsignal_exit(void) {}
void md_onsignal_autosave(void) {}
void md_ignoreallsignals(void) {}
void md_tstphold(void) {}
void md_tstpresume(void) {}
void md_tstpsignal(void) {}
void md_normaluser(void) {}
void md_raw_standout(void) {}
void md_raw_standend(void) {}
int md_hasclreol(void) { return TRUE; }
int md_getuid(void) { return 42; }
int md_getpid(void) { return 42; }
char *md_getusername(void) { return "Rogue"; }
char *md_getrealname(int uid) { (void)uid; return "Rogue"; }
char *md_gethomedir(void) { return ""; }
char *md_getshell(void) { return ""; }
int md_shellescape(void) { return 0; }
int md_chmod(char *file, int mode) { (void)file; (void)mode; return 0; }
int md_unlink(char *file) { return unlink(file); }
int md_unlink_open_file(char *file, FILE *stream)
{
    /* DOS cannot unlink an open file.  Upstream has finished reading it. */
    fclose(stream);
    return unlink(file);
}
int md_erasechar(void) { return erasechar(); }
int md_killchar(void) { return killchar(); }
int md_dsuspchar(void) { return 0; }
int md_suspchar(void) { return 0; }
int md_setdsuspchar(int c) { (void)c; return 0; }
int md_setsuspchar(int c) { (void)c; return 0; }
void md_sleep(int seconds) { napms(seconds * 1000); }
void md_loadav(double *average) { average[0] = average[1] = average[2] = 0; }
void md_putchar(int c) { putchar(c); }
int directory_exists(char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && (info.st_mode & S_IFDIR);
}

/* PDCurses has already decoded the BIOS key.  These are Rogue's own
 * standard vi/numeric-keypad commands, including shifted run commands. */
int md_readchar(void)
{
    int c = getch();
    switch (c)
    {
    case KEY_LEFT: case KEY_B1: c = 'h'; break;
    case KEY_DOWN: case KEY_C2: c = 'j'; break;
    case KEY_UP: case KEY_A2: c = 'k'; break;
    case KEY_RIGHT: case KEY_B3: c = 'l'; break;
    case KEY_HOME: case KEY_A1: c = 'y'; break;
    case KEY_PPAGE: case KEY_A3: c = 'u'; break;
    case KEY_END: case KEY_C1: c = 'b'; break;
    case KEY_NPAGE: case KEY_C3: c = 'n'; break;
    case KEY_B2: c = '.'; break;
    case KEY_SLEFT: case CTL_LEFT: c = CTRL('H'); break;
    case KEY_SDOWN: case CTL_DOWN: c = CTRL('J'); break;
    case KEY_SUP: case CTL_UP: c = CTRL('K'); break;
    case KEY_SRIGHT: case CTL_RIGHT: c = CTRL('L'); break;
    case KEY_SHOME: case CTL_HOME: c = CTRL('Y'); break;
    case KEY_SPREVIOUS: case CTL_PGUP: c = CTRL('U'); break;
    case KEY_SEND: case CTL_END: c = CTRL('B'); break;
    case KEY_SNEXT: case CTL_PGDN: c = CTRL('N'); break;
    case KEY_BACKSPACE: c = CTRL('H'); break;
    }
    return c & 0x7f;
}
