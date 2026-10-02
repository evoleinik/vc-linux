/* DOS process, date, BSD random/string, and segmented-save support. */
#include <dos.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include "hack.h"
#include "extern.h"

int getuid(void) { return 1; }
char *getlogin(void) { return "Hacker"; }

size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t len = strlen(in), count;
    if (size) {
        count = len < size - 1 ? len : size - 1;
        memcpy(out, in, count);
        out[count] = 0;
    }
    return len;
}
size_t strlcat(char *out, const char *in, size_t size)
{
    size_t old = 0;
    while (old < size && out[old]) ++old;
    return old + strlcpy(out + old, in, size - old);
}

/* The BSD degree-31 additive generator. Use explicit 32-bit arithmetic;
 * Watcom's 15-bit rand() would bias Hack's original random() >> 3 calls. */
static unsigned long rng_state[31];
static unsigned rng_front = 3, rng_rear;
long random(void)
{
    unsigned long result;
    rng_state[rng_front] += rng_state[rng_rear];
    result = (rng_state[rng_front] >> 1) & 0x7fffffffUL;
    rng_front = (rng_front + 1) % 31;
    rng_rear = (rng_rear + 1) % 31;
    return (long)result;
}
void srandom(unsigned long seed)
{
    unsigned i;
    long word = seed & 0x7fffffffUL;
    if (!word) word = 1;
    rng_state[0] = word;
    for (i = 1; i < 31; ++i) {
        word = 16807L * (word % 127773L) - 2836L * (word / 127773L);
        if (word < 0) word += 2147483647L;
        rng_state[i] = word;
    }
    rng_front = 3;
    rng_rear = 0;
    for (i = 0; i < 310; ++i) random();
}
void setrandom(void) { srandom((unsigned long)time(NULL)); }

static struct tm *getlt(void)
{
    time_t now = time(NULL);
    return localtime(&now);
}
int getyear(void) { return 1900 + getlt()->tm_year; }
char *getdatestr(void)
{
    static char result[7];
    struct tm *lt = getlt();
    snprintf(result, sizeof(result), "%02d%02d%02d", lt->tm_year % 100,
             lt->tm_mon + 1, lt->tm_mday);
    return result;
}
int phase_of_the_moon(void)
{
    struct tm *lt = getlt();
    int golden = (lt->tm_year % 19) + 1;
    int epact = (11 * golden + 18) % 30;
    if ((epact == 25 && golden > 11) || epact == 24) ++epact;
    return (((lt->tm_yday + epact) * 6 + 11) % 177) / 22 & 7;
}
int night(void)
{
    int hour = getlt()->tm_hour;
    return hour < 6 || hour > 21;
}
int midnight(void) { return getlt()->tm_hour == 0; }
/* Save freshness uses the linked-image identity, not DOS timestamp precision
 * or the file's installation time. save_io.c checks it before any recovery. */
void gethdate(char *name) { (void)name; }

void getlock(void)
{
    int fd;
    /* Original Unix lock names embed user names and are not DOS 8.3 names.
     * Only one program runs in this private DOS session at any instant. */
    strcpy(lock, "HACK");
    glo(0);
    fd = creat(lock, FMASK);
    if (fd < 0) error("Cannot write in the Hack directory.");
    if (write(fd, &hackpid, sizeof(hackpid)) != sizeof(hackpid))
        error("Cannot write Hack level lock.");
    close(fd);
}
void regularize(char *name)
{
    while (*name) {
        if (*name == '.' || *name == '/' || *name == '\\' || *name == ':')
            *name = '_';
        ++name;
    }
}

const struct permonst *hack_monster_rebase(const struct permonst *saved,
                                          const struct permonst *oldbase)
{
    /* All EXE segments move by the same paragraph displacement. Preserve
     * the original offset as well as relative segment of static monsters. */
    unsigned segment = FP_SEG(saved) + FP_SEG(mons) - FP_SEG(oldbase);
    return (const struct permonst *)MK_FP(segment, FP_OFF(saved));
}

void hack_save_you(int fd)
{
    unsigned length = u.usick_cause ? strlen(u.usick_cause) + 1 : 0;
    bwrite(fd, &length, sizeof(length));
    if (length) bwrite(fd, u.usick_cause, length);
}
void hack_restore_you(int fd)
{
    unsigned length;
    int i;
    char *cause = NULL;
    mread(fd, &length, sizeof(length));
    if (length) {
        if (length > BUFSZ) error("Invalid saved sickness description.");
        cause = alloc(length);
        mread(fd, cause, length);
        cause[length - 1] = 0;
    }
    u.usick_cause = cause;
    /* Hack has exactly one timeout callback: levitation's float_down. A
     * pointer from an earlier EXE placement must not be called after EXEC. */
    for (i = 0; i < LAST_RING + 10; ++i)
        u.uprops[i].p_tofn = NULL;
    if (u.uprops[PROP(RIN_LEVITATION)].p_flgs & TIMEOUT)
        u.uprops[PROP(RIN_LEVITATION)].p_tofn = float_down;
}
