/* DOS entry point: all mutable Hack files belong beside this executable. */
#include <direct.h>
#include <dos.h>
#include <fcntl.h>
#include <io.h>
#include "hack.h"
#include "extern.h"

extern int hack_main(int, char **);

static int caller_drive, playground_drive;
/* AX=7147 may return 260 bytes without a drive or leading backslash. */
#define DIRECTORY_SIZE 263
static char caller_directory[DIRECTORY_SIZE], playground_directory[DIRECTORY_SIZE];

int hack_remember_directory(int drive, char *directory)
{
    union REGPACK regs;
    memset(&regs, 0, sizeof(regs));
    directory[0] = 'A' + drive - 1;
    directory[1] = ':';
    directory[2] = '\\';
    directory[3] = 0;
    regs.x.ax = 0x7147;
    regs.h.dl = drive;
    regs.x.si = FP_OFF(directory + 3);
    regs.x.flags = INTR_CF;
    regs.x.ds = FP_SEG(directory + 3);
    /* intrf loads the input flags; int86x's cflag is output-only. Old DOS
     * may leave CF untouched when returning an unsupported LFN service. */
    intrf(0x21, &regs);
    if (!(regs.x.flags & INTR_CF)) return 1;
    /* Only a missing LFN service permits falling back to real DOS's stable
     * on-disk 8.3 names. Other errors must not save an unrelated path. */
    if (regs.x.ax != 0x7100 && regs.x.ax != 1) return 0;
    return _getdcwd(drive, directory, DIRECTORY_SIZE) != NULL;
}

int hack_return_directory(const char *directory)
{
    union REGPACK regs;
    memset(&regs, 0, sizeof(regs));
    regs.x.ax = 0x713b;
    regs.x.dx = FP_OFF(directory);
    regs.x.flags = INTR_CF;
    regs.x.ds = FP_SEG(directory);
    intrf(0x21, &regs);
    if (!(regs.x.flags & INTR_CF)) return 0;
    if (regs.x.ax != 0x7100 && regs.x.ax != 1) return -1;
    return chdir(directory);
}

static void restore_directories(void)
{
    /* DOS's current directories are shared with the parent, not private to
     * the child PSP. Restore the changed drive's state as well as the active
     * drive so both Q and S return VC to exactly the caller's playground. */
    if (playground_drive && playground_drive != caller_drive) {
        (void) _chdrive(playground_drive);
        (void) hack_return_directory(playground_directory);
    }
    (void) _chdrive(caller_drive);
    (void) hack_return_directory(caller_directory);
}

int main(int argc, char **argv)
{
    char directory[260];
    char *slash, *forward;
    FILE *file;
    int i;
    const char *writable[] = {"record", "perm"};

    _fmode = O_BINARY;
    caller_drive = _getdrive();
    if (!hack_remember_directory(caller_drive, caller_directory))
        error("Cannot remember the caller's DOS directory.");
    if (atexit(restore_directories) != 0)
        error("Cannot register DOS directory restoration.");
    if (strlcpy(directory, argv[0], sizeof(directory)) >= sizeof(directory))
        error("Hack executable path is too long.");
    slash = strrchr(directory, '\\');
    forward = strrchr(directory, '/');
    if (!slash || (forward && forward > slash))
        slash = forward;
    if (slash) {
        if (slash == directory || (slash == directory + 2 && directory[1] == ':'))
            slash[1] = 0;
        else
            *slash = 0;
        i = caller_drive;
        if (directory[1] == ':') {
            i = directory[0];
            if (i >= 'a' && i <= 'z') i -= 'a' - 'A';
            i = i - 'A' + 1;
        }
        if (i != caller_drive) {
            if (!hack_remember_directory(i, playground_directory))
                error("Cannot remember the Hack drive's DOS directory.");
            playground_drive = i;
        }
        if (_chdrive(i) != 0)
            error("Cannot select the Hack drive.");
        if (hack_return_directory(directory) != 0)
            error("Cannot enter Hack directory: %s", directory);
    }
    if (access("help", 0) || access("data", 0) || access("rumors", 0))
        error("Hack data files must be beside HACK.EXE.");
    for (i = 0; i < SIZE(writable); ++i) {
        file = fopen(writable[i], "ab");
        if (file)
            fclose(file);
    }
    return hack_main(argc, argv);
}
