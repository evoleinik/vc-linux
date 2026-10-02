/* Forced include: expose the small DOS replacement surface to upstream C. */
#ifndef HACK_DOS_COMPAT_H
#define HACK_DOS_COMPAT_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

typedef void (*sig_t)(int);
#undef min
#undef max

int getuid(void);
char *getlogin(void);
long random(void);
void srandom(unsigned long);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
int hack_getchar(void);
int hack_putchar(int);
int hack_printf(const char *, ...);
int hack_vprintf(const char *, va_list);
int hack_puts(const char *);
int hack_fputs(const char *, FILE *);
int hack_mapchar(int, int, int);
void hack_save_you(int);
void hack_restore_you(int);
int hack_creat(const char *, int);
void hack_write_header(int);
void hack_read_header(int);
int hack_finish_file(int);
int hack_resume_valid(int);
struct permonst;
const struct permonst *hack_monster_rebase(const struct permonst *, const struct permonst *);

/* There is no Unix process/signal model inside a DOS invocation. */
#define signal(number, handler) ((void (*)(int))0)
/* io.h exports lock(); Hack's unrelated level-name buffer has this name. */
#define lock hack_lock
#define creat hack_creat
#undef getchar
#undef putchar
#define getchar hack_getchar
#define putchar hack_putchar
#define printf hack_printf
#define vprintf hack_vprintf
#define puts hack_puts
#define fputs hack_fputs
#endif
