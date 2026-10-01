#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/* The DOS file layer is tested as an interrupt client, not through its private
 * implementation.  This translation unit owns the CPU and RAM deliberately:
 * runtime/cpu.c is another worker's responsibility and must not be linked. */
#include "cpu.h"
#include "hle.h"
#include "dos_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

Cpu cpu;
uint8_t mem[MEM_SIZE];
int32_t rt_budget;

static unsigned checks, failures;
static char fixture[PATH_MAX], original_cwd[PATH_MAX], home_root[PATH_MAX];
static unsigned char console_bytes[1024];
static size_t console_size;
static uint16_t called[256];
static size_t called_count;

enum { DS = 0x1200, ES = 0x2400, ARG = 0x100, ARG2 = 0x800,
       DATA = 0x1000, OUT = 0x2000, DTA1 = 0x3000, DTA2 = 0x3080 };
enum { A_RO = 1, A_HIDDEN = 2, A_SYSTEM = 4, A_LABEL = 8,
       A_DIR = 0x10, A_ARCHIVE = 0x20 };

static void check_at(int condition, int line, const char *fmt, ...)
{
    va_list args;
    ++checks;
    if (condition) return;
    ++failures;
    fprintf(stderr, "FAIL tests/test_dos_fs.c:%d: ", line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
#define CHECK(c, ...) check_at(!!(c), __LINE__, __VA_ARGS__)

void con_write(const uint8_t *buf, size_t n)
{
    size_t room = sizeof console_bytes - console_size;
    if (n > room) n = room;
    memcpy(console_bytes + console_size, buf, n);
    console_size += n;
}
uint8_t port_in8(uint16_t port) { (void)port; return 0; }
uint16_t port_in16(uint16_t port) { (void)port; return 0; }
void port_out8(uint16_t port, uint8_t value) { (void)port; (void)value; }
void port_out16(uint16_t port, uint16_t value) { (void)port; (void)value; }
void rt_yield(void) { rt_budget = 1000; }
_Noreturn void rt_fault(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

static void host_require(int ok, const char *what)
{
    if (!ok) { perror(what); exit(2); }
}

static void host_path(char *out, size_t cap, const char *relative)
{
    int n = snprintf(out, cap, "%s/%s", fixture, relative);
    host_require(n >= 0 && (size_t)n < cap, "fixture path");
}

static void host_file(const char *name, const char *contents, mode_t mode)
{
    char path[PATH_MAX];
    host_path(path, sizeof path, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    host_require(fd >= 0, "create fixture file");
    size_t n = strlen(contents);
    host_require(write(fd, contents, n) == (ssize_t)n, "write fixture file");
    host_require(fchmod(fd, mode) == 0, "chmod fixture file");
    host_require(close(fd) == 0, "close fixture file");
}

static int host_stat(const char *name, struct stat *st)
{
    char path[PATH_MAX];
    host_path(path, sizeof path, name);
    return stat(path, st);
}

static void host_times(const char *name, time_t seconds, long ns)
{
    char path[PATH_MAX];
    struct timespec ts[2] = {{seconds, ns}, {seconds, ns}};
    host_path(path, sizeof path, name);
    host_require(utimensat(AT_FDCWD, path, ts, 0) == 0, "set fixture timestamp");
}

static void putstr(uint16_t seg, uint16_t off, const char *s)
{
    do { wr8(seg, off++, (uint8_t)*s); } while (*s++);
}

static void getstr(uint16_t seg, uint16_t off, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i < cap; ++i) {
        out[i] = (char)rd8(seg, off++);
        if (!out[i]) return;
    }
    if (cap) out[cap - 1] = 0;
    CHECK(0, "DOS output at %04x:%04x is not NUL terminated", seg, off);
}

static uint32_t get32(uint16_t seg, uint16_t off)
{
    return rd16(seg, off) | ((uint32_t)rd16(seg, (uint16_t)(off + 2)) << 16);
}

static uint64_t get64(uint16_t seg, uint16_t off)
{
    return get32(seg, off) | ((uint64_t)get32(seg, (uint16_t)(off + 4)) << 32);
}

static void put64(uint16_t seg, uint16_t off, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) wr8(seg, off++, (uint8_t)(value >> (i * 8)));
}

static void begin(uint16_t ax)
{
    memset(&cpu, 0, sizeof cpu);
    cpu.a.x = ax;
    cpu.ds = DS;
    cpu.es = ES;
    cpu.d.x = ARG;
    cpu.si = ARG;
    cpu.di = OUT;
    cpu.cf = 1; /* Success must clear a pre-existing carry. */
}

static void path_begin(uint16_t ax, const char *path)
{
    begin(ax);
    putstr(DS, ARG, path);
}

static int invoke(void)
{
    uint16_t code = cpu.a.x;
    if ((code >> 8) != 0x71 && (code >> 8) != 0x73 &&
        (code >> 8) != 0x43 && (code >> 8) != 0x44 && (code >> 8) != 0x57)
        code &= 0xff00;
    size_t i;
    for (i = 0; i < called_count; ++i) if (called[i] == code) break;
    if (i == called_count && called_count < sizeof called / sizeof called[0])
        called[called_count++] = code;
    return dos_fs_int21();
}

static int ok(const char *what)
{
    uint16_t fn = cpu.a.x;
    int handled = invoke();
    int success = handled == 1 && cpu.cf == 0;
    CHECK(success, "%s: INT21 %04x expected success, handled=%d CF=%u AX=%04x",
          what, fn, handled, cpu.cf, cpu.a.x);
    return success;
}

static int error(unsigned code, const char *what)
{
    uint16_t fn = cpu.a.x;
    cpu.cf = 0; /* Failure must set carry, not just leave it unchanged. */
    int handled = invoke();
    int correct = handled == 1 && cpu.cf == 1 && cpu.a.x == code;
    CHECK(correct, "%s: INT21 %04x expected DOS error %u, handled=%d CF=%u AX=%u",
          what, fn, code, handled, cpu.cf, cpu.a.x);
    return correct;
}

/* Set date, set time and free space report errors without CF: AL=FFh for the
 * clock calls, AX=FFFFh for free space. VC's DESQview probe depends on it. */
static void sentinel(uint16_t mask, uint16_t value, const char *what)
{
    uint16_t fn = cpu.a.x;
    int handled = invoke();
    CHECK(handled == 1 && (cpu.a.x & mask) == value && cpu.cf == 0,
          "%s: INT21 %04x expected AX&%04x=%04x with CF clear, handled=%d CF=%u AX=%04x",
          what, fn, mask, value, handled, cpu.cf, cpu.a.x);
}

static uint16_t open_file(const char *path, unsigned mode)
{
    path_begin((uint16_t)(0x3d00 | mode), path);
    if (!ok("open file")) return 0xffff;
    CHECK(cpu.a.x >= 5 && cpu.a.x < 64, "file handle %u is in fixed table", cpu.a.x);
    return cpu.a.x;
}

static void close_file(uint16_t handle)
{
    begin(0x3e00); cpu.b.x = handle; ok("close file");
}

static uint32_t seek_file(uint16_t handle, unsigned origin, int32_t offset)
{
    begin((uint16_t)(0x4200 | origin));
    cpu.b.x = handle;
    cpu.c.x = (uint16_t)((uint32_t)offset >> 16);
    cpu.d.x = (uint16_t)offset;
    if (!ok("seek file")) return UINT32_MAX;
    return ((uint32_t)cpu.d.x << 16) | cpu.a.x;
}

static void read_equals(uint16_t handle, const char *expected, const char *why)
{
    begin(0x3f00); cpu.b.x = handle; cpu.c.x = 128; cpu.d.x = DATA;
    if (!ok(why)) return;
    size_t n = strlen(expected);
    CHECK(cpu.a.x == n, "%s: read count %u, expected %zu", why, cpu.a.x, n);
    CHECK(memcmp(mem + lin(DS, DATA), expected, n) == 0, "%s: file contents", why);
}

static void write_bytes(uint16_t handle, const char *bytes)
{
    size_t n = strlen(bytes);
    putstr(DS, DATA, bytes);
    begin(0x4000); cpu.b.x = handle; cpu.c.x = (uint16_t)n; cpu.d.x = DATA;
    if (ok("write file")) CHECK(cpu.a.x == n, "write reports exact count");
}

typedef struct {
    char name[260], alias[14];
    uint32_t attrs;
    uint64_t size, mtime, ctime, atime;
} Found;

static void set_dta(uint16_t off)
{
    begin(0x1a00); cpu.d.x = off; ok("set DTA");
}

static void classic_result(Found *out, uint16_t off)
{
    memset(out, 0, sizeof *out);
    getstr(DS, (uint16_t)(off + 30), out->name, 13);
    memcpy(out->alias, out->name, strlen(out->name) + 1);
    out->attrs = rd8(DS, (uint16_t)(off + 21));
    out->mtime = get32(DS, (uint16_t)(off + 22));
    out->size = get32(DS, (uint16_t)(off + 26));
}

static void long_result(Found *out)
{
    memset(out, 0, sizeof *out);
    getstr(ES, OUT + 44, out->name, sizeof out->name);
    getstr(ES, OUT + 304, out->alias, sizeof out->alias);
    out->attrs = get32(ES, OUT);
    out->ctime = get64(ES, OUT + 4);
    out->atime = get64(ES, OUT + 12);
    out->mtime = get64(ES, OUT + 20);
    out->size = ((uint64_t)get32(ES, OUT + 28) << 32) | get32(ES, OUT + 32);
}

static size_t find_entries(int lfn, const char *pattern, uint16_t mask,
                           unsigned time_mode, Found *out, size_t cap)
{
    size_t count = 0;
    uint16_t handle = 0xffff;
    if (!lfn) set_dta(DTA1);
    wr8(lfn ? ES : DS, lfn ? OUT + 318 : DTA1 + 44, 0xa5);
    path_begin(lfn ? 0x714e : 0x4e00, pattern);
    cpu.c.x = mask;
    cpu.si = (uint16_t)time_mode;
    for (;;) {
        int handled = invoke();
        CHECK(handled == 1, "%s find handled", lfn ? "LFN" : "classic");
        if (cpu.cf) {
            CHECK(cpu.a.x == 18, "%s find end/error must be 18, got %u for '%s'",
                  lfn ? "LFN" : "classic", cpu.a.x, pattern);
            break;
        }
        if (lfn && handle == 0xffff) handle = cpu.a.x;
        if (count >= cap) { CHECK(0, "find produced over %zu entries", cap); break; }
        if (lfn) long_result(&out[count]); else classic_result(&out[count], DTA1);
        CHECK(rd8(lfn ? ES : DS, lfn ? OUT + 318 : DTA1 + 44) == 0xa5,
              "%s find record does not overwrite adjacent memory", lfn ? "FDR" : "DTA");
        ++count;
        begin(lfn ? 0x714f : 0x4f00);
        cpu.b.x = handle;
        cpu.si = (uint16_t)time_mode;
    }
    if (lfn && handle != 0xffff) {
        begin(0x71a1); cpu.b.x = handle; ok("close LFN search");
        begin(0x714f); cpu.b.x = handle; error(6, "closed LFN search handle");
    }
    return count;
}

static const Found *found_name(const Found *list, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(list[i].name, name) == 0) return &list[i];
    return NULL;
}

static void check_alias(const Found *list, size_t count, const char *name,
                        const char *expected)
{
    const Found *entry = found_name(list, count, name);
    CHECK(entry != NULL, "ALIAS REGRESSION: find must return host name '%s'", name);
    if (entry)
        CHECK(strcmp(entry->alias, expected) == 0,
              "ALIAS REGRESSION: '%s' expected '%s', got '%s'", name, expected, entry->alias);
}

static void test_names_and_finds(void)
{
    Found list[128], again[128];
    size_t n = find_entries(1, "*.*", A_DIR | A_HIDDEN | A_SYSTEM, 1, list, 128);
    CHECK(n >= 17, "LFN *.* returns fixture and dot entries (got %zu)", n);
    check_alias(list, n, "A very long name.txt", "AVERYL~1.TXT");
    check_alias(list, n, "Base name alpha.txt", "BASENA~2.TXT");
    check_alias(list, n, "Base name beta.txt", "BASENA~3.TXT");
    /* A long name that already is 8.3, ignoring case, has no short name in
     * the long-name record (as on Windows NT). VC copies under the short name
     * when there is one, so plain.txt would arrive as PLAIN.TXT. */
    check_alias(list, n, "basena~1.txt", "");
    check_alias(list, n, "plain.txt", "");
    {
        Found classic[128];
        size_t cn = find_entries(0, "*.*", A_DIR | A_HIDDEN | A_SYSTEM, 1, classic, 128);
        const Found *p = found_name(classic, cn, "PLAIN.TXT");
        CHECK(p != NULL, "classic find still names plain.txt by its alias PLAIN.TXT");
    }
    check_alias(list, n, "\x92\xa5\xe1\xe2.txt", "\x92\x85\x91\x92.TXT");
    CHECK(found_name(list, n, "face-\xfe~0791.txt") != NULL,
          "unrepresentable emoji gets one CP866 FEh square and its own FNV hash suffix");
    CHECK(found_name(list, n, ".") && found_name(list, n, ".."), "LFN directories contain dot and dot-dot");
    const Found *ent = found_name(list, n, "dangling");
    CHECK(ent && ent->size == 0 && ent->attrs == A_ARCHIVE, "dangling symlink is regular zero-length file");
    ent = found_name(list, n, "link.txt");
    CHECK(ent && ent->size == 10 && ent->attrs == A_ARCHIVE, "stat follows file symlink");
    ent = found_name(list, n, "dirlink");
    CHECK(ent && (ent->attrs & A_DIR), "stat follows directory symlink");
    ent = found_name(list, n, ".secret");
    CHECK(ent && ent->attrs == (A_HIDDEN | A_ARCHIVE), "dotfile hidden+archive attributes");
    ent = found_name(list, n, "readonly.bin");
    CHECK(ent && ent->attrs == (A_RO | A_ARCHIVE), "owner write bit controls read-only attribute");

    size_t m = find_entries(1, "*", A_DIR | A_HIDDEN | A_SYSTEM, 1, again, 128);
    CHECK(n == m, "Windows '*' and '*.*' enumerate same set");
    for (size_t i = 0; i < n && i < m; ++i) {
        CHECK(strcmp(list[i].name, again[i].name) == 0, "stable bytewise host order at index %zu", i);
        CHECK(strcmp(list[i].alias, again[i].alias) == 0, "stable aliases at index %zu", i);
    }
    const char *ordered[] = {".", "..", ".secret", "A very long name.txt", "Base name alpha.txt",
                            "Base name beta.txt", "Long directory", "README", "basena~1.txt"};
    for (size_t i = 0; i < sizeof ordered / sizeof ordered[0] && i < n; ++i)
        CHECK(strcmp(list[i].name, ordered[i]) == 0, "host bytewise sort[%zu]: expected '%s', got '%s'",
              i, ordered[i], list[i].name);

    n = find_entries(0, "*.*", 0, 0, list, 128);
    CHECK(found_name(list, n, "README") != NULL, "classic *.* matches extensionless name");
    CHECK(found_name(list, n, "AVERYL~1.TXT") != NULL, "classic find returns 8.3 long-name alias");
    CHECK(found_name(list, n, "READONLY.BIN") != NULL, "classic normal mask includes read-only files");
    for (size_t i = 0; i < n; ++i)
        CHECK(!(list[i].attrs & (A_HIDDEN | A_DIR | A_SYSTEM)), "default classic mask filters special attrs");
    n = find_entries(0, "*.*", A_DIR, 0, list, 128);
    CHECK(found_name(list, n, ".") && found_name(list, n, ".."), "classic directory mask includes dots");
    CHECK(found_name(list, n, "SUBDIR") != NULL, "classic directory mask includes directories");
    for (size_t i = 0; i < n; ++i) CHECK(!(list[i].attrs & A_HIDDEN), "classic mask excludes hidden");
    n = find_entries(0, "*.*", A_HIDDEN, 0, list, 128);
    unsigned hidden = 0;
    for (size_t i = 0; i < n; ++i) if (list[i].attrs & A_HIDDEN) ++hidden;
    CHECK(hidden == 1, "classic hidden mask includes the one hidden dotfile");
    CHECK(find_entries(0, "*.*", A_LABEL, 0, list, 128) == 0, "no classic volume label");
    CHECK(find_entries(1, "*.*", A_LABEL, 1, list, 128) == 0, "no LFN volume label");
    n = find_entries(1, "*", 0, 1, list, 128);
    CHECK(found_name(list, n, "README") != NULL, "LFN normal mask includes extensionless files");
    CHECK(found_name(list, n, ".secret") == NULL && found_name(list, n, "subdir") == NULL,
          "LFN default excludes hidden and directories");
    n = find_entries(1, "base NAME *.TxT", 0, 1, list, 128);
    CHECK(n == 2, "LFN wildcard matching is case insensitive");
    n = find_entries(0, "BASENA~?.TXT", 0, 0, list, 128);
    CHECK(n == 3, "classic '?' matches the alias collision digit");
    n = find_entries(0, "PLAIN???.TXT", 0, 0, list, 128);
    CHECK(n == 1 && !strcmp(list[0].name, "PLAIN.TXT"), "classic '?' also matches space padding in8.3 fields");
    n = find_entries(0, "README.???", 0, 0, list, 128);
    CHECK(n == 1 && !strcmp(list[0].name, "README"), "classic extension wildcards match padded empty extension");
    n = find_entries(1, "face-?~????.txt", 0, 1, list, 128);
    CHECK(n == 1 && !strcmp(list[0].name, "face-\xfe~0791.txt"), "LFN wildcard over lossy CP866 name");
    CHECK(find_entries(0, "NOSUCH.*", 0, 0, list, 128) == 0, "classic no-match error18");
    CHECK(find_entries(1, "NOSUCH.*", 0, 1, list, 128) == 0, "LFN no-match error18");

    const char *paths[] = {"AVERYL~1.TXT", "averyl~1.txt", "A VERY LONG NAME.TXT",
                           "./subdir/../A very long name.txt"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        uint16_t handle = open_file(paths[i], 0);
        read_equals(handle, "long payload", "resolve long-name or alias input");
        close_file(handle);
    }
    uint16_t handle = open_file("BASENA~2.TXT", 0);
    read_equals(handle, "alpha alias", "ALIAS REGRESSION: alias must open the assigned host file");
    close_file(handle);
    handle = open_file("\xe2\xa5\xe1\xe2.TxT", 0);
    read_equals(handle, "Cyrillic", "CP866 Cyrillic case-insensitive open"); close_file(handle);
    handle = open_file("face-\xfe~0791.txt", 0);
    read_equals(handle, "emoji", "lossy CP866 FEh name opens host emoji file"); close_file(handle);
    path_begin(0x3d00, "face-?.txt"); error(2, "non-wildcard open treats question mark literally");
    host_file("face-?.txt", "literal question", 0644);
    handle = open_file("face-?.txt", 0);
    read_equals(handle, "literal question", "literal question mark is independent of the lossy name"); close_file(handle);
    path_begin(0x7141, "face-?.txt"); cpu.si = 0;
    ok("LFN literal unlink distinguishes real question mark from wildcard mode");
    path_begin(0x3d00, "face-?.txt"); error(2, "removed literal question mark does not match emoji");
    handle = open_file("face-\xfe~0791.txt", 0);
    read_equals(handle, "emoji", "lossy emoji file survives exact-name deletion"); close_file(handle);
    handle = open_file("link.txt", 0);
    read_equals(handle, "plain-data", "symlink open follows target"); close_file(handle);

    /* The DOS current directory is not the process current directory. */
    path_begin(0x713b, "C:\\"); ok("LFN chdir root");
    n = find_entries(1, "*", A_DIR, 1, list, 128);
    CHECK(found_name(list, n, ".") == NULL && found_name(list, n, "..") == NULL,
          "root must not enumerate dot entries");
    char dosroot[PATH_MAX + 3];
    snprintf(dosroot, sizeof dosroot, "C:%s", fixture);
    path_begin(0x3b00, dosroot); ok("restore fixture DOS cwd");
}

static void test_dta_state(void)
{
    Found first, second;
    uint8_t saved[44];
    set_dta(DTA1);
    begin(0x2f00); if (ok("get DTA"))
        CHECK(cpu.es == DS && cpu.b.x == DTA1, "2F returns DTA in ES:BX");
    path_begin(0x4e00, "BASENA~?.TXT");
    if (ok("first interleaved classic search")) classic_result(&first, DTA1);
    else memset(&first, 0, sizeof first);
    for (size_t i = 0; i < sizeof saved; ++i) saved[i] = rd8(DS, DTA1 + (uint16_t)i);
    set_dta(DTA2);
    path_begin(0x4e00, "README"); ok("second interleaved classic search");
    classic_result(&second, DTA2);
    CHECK(!strcmp(second.name, "README"), "second DTA search has independent selection");
    set_dta(DTA1);
    begin(0x4f00); if (ok("resume first search after switching DTA")) {
        classic_result(&second, DTA1);
        CHECK(strcmp(first.name, second.name) != 0, "first DTA retains its own search cursor");
    }
    /* DOS clients can move a complete DTA; its reserved bytes identify state. */
    for (size_t i = 0; i < sizeof saved; ++i) wr8(DS, DTA2 + (uint16_t)i, saved[i]);
    set_dta(DTA2);
    begin(0x4f00); ok("copied DTA reserved bytes retain search identity");
    for (unsigned i = 0; i < 21; ++i) wr8(DS, DTA2 + (uint16_t)i, 0);
    begin(0x4f00); error(18, "uninitialized/tampered DTA search cookie");
    begin(0x714f); cpu.b.x = 0xffff; error(6, "invalid LFN find handle");
    begin(0x71a1); cpu.b.x = 0xffff; error(6, "invalid LFN find-close handle");
    /* Drain the still-live first search before later capacity tests. */
    set_dta(DTA1);
    begin(0x4f00);
    int handled = invoke();
    CHECK(handled == 1 && cpu.cf && cpu.a.x == 18, "original search exhausted after copied-DTA advance");
}

static void test_alias_collision_scale(void)
{
    char path[PATH_MAX], name[100], contents[32], alias[14];
    host_path(path, sizeof path, "Alias scale");
    host_require(mkdir(path, 0755) == 0, "mkdir alias scale fixture");
    for (int i = 12; i >= 1; --i) {
        snprintf(name, sizeof name, "Alias scale/Prefix example %02d.txt", i);
        snprintf(contents, sizeof contents, "number%02d", i);
        host_file(name, contents, 0644);
    }
    host_file("Alias scale/prefix~1.txt", "reserved", 0644);
    Found list[32];
    size_t n = find_entries(1, "Alias scale\\*", 0, 1, list, 32);
    CHECK(n == 13, "scaled alias fixture contains twelve long names and reserved8.3 name");
    for (unsigned i = 1; i <= 12; ++i) {
        snprintf(name, sizeof name, "Prefix example %02u.txt", i);
        unsigned ordinal = i + 1;
        snprintf(alias, sizeof alias, ordinal < 10 ? "PREFIX~%u.TXT" : "PREFI~%u.TXT", ordinal);
        check_alias(list, n, name, alias);
        snprintf(name, sizeof name, "Alias scale\\%s", alias);
        snprintf(contents, sizeof contents, "number%02u", i);
        uint16_t h = open_file(name, 0);
        read_equals(h, contents, "ALIAS REGRESSION: multi-digit alias opens its assigned file");
        close_file(h);
    }
}

static void test_io_and_handles(void)
{
    path_begin(0x3c00, "io.bin"); cpu.c.x = 0;
    if (!ok("classic create")) return;
    uint16_t h = cpu.a.x;
    write_bytes(h, "abcdef");
    CHECK(seek_file(h, 0, 0) == 0, "absolute seek to start");
    read_equals(h, "abcdef", "read created file");
    CHECK(seek_file(h, 2, -2) == 4, "signed relative seek from EOF");
    write_bytes(h, "XY");
    CHECK(seek_file(h, 1, -3) == 3, "signed seek from current position");
    begin(0x4000); cpu.b.x = h; cpu.c.x = 0; cpu.d.x = DATA;
    if (ok("CX=0 truncates at current position")) CHECK(cpu.a.x == 0, "zero-byte truncate count");
    struct stat st;
    CHECK(host_stat("io.bin", &st) == 0 && st.st_size == 3, "CX=0 host file size is current offset");
    CHECK(seek_file(h, 0, 0) == 0, "rewind truncated file");
    read_equals(h, "abc", "truncated content");
    CHECK(seek_file(h, 0, 8) == 8, "seek beyond EOF");
    begin(0x4000); cpu.b.x = h; cpu.c.x = 0; ok("CX=0 extends at a beyond-EOF offset");
    CHECK(host_stat("io.bin", &st) == 0 && st.st_size == 8, "zero write also extends file");
    CHECK(seek_file(h, 0, 0) == 0, "rewind for duplicate handle");
    begin(0x4500); cpu.b.x = h;
    uint16_t duplicate = 0xffff;
    if (ok("duplicate handle")) duplicate = cpu.a.x;
    CHECK(duplicate != h && duplicate >= 5 && duplicate < 64, "dup allocates another DOS handle");
    begin(0x3f00); cpu.b.x = duplicate; cpu.c.x = 1; cpu.d.x = DATA;
    if (ok("read through duplicate")) CHECK(cpu.a.x == 1 && rd8(DS, DATA) == 'a', "duplicate reads first byte");
    CHECK(seek_file(h, 1, 0) == 1, "duplicate descriptors share the file position");
    uint16_t replaced = open_file("plain.txt", 0);
    begin(0x4600); cpu.b.x = h; cpu.c.x = replaced; ok("force duplicate over existing handle");
    begin(0x3f00); cpu.b.x = replaced; cpu.c.x = 1; cpu.d.x = DATA;
    if (ok("force-duplicated handle reads source")) CHECK(rd8(DS, DATA) == 'b', "dup2 shares position");
    begin(0x4600); cpu.b.x = h; cpu.c.x = h; ok("force dup handle onto itself");
    begin(0x6800); cpu.b.x = h; ok("flush/commit file");
    close_file(duplicate); close_file(replaced); close_file(h);
    begin(0x3e00); cpu.b.x = h; error(6, "close already closed handle");

    h = open_file("plain.txt", 0);
    begin(0x4000); cpu.b.x = h; cpu.c.x = 1; cpu.d.x = DATA;
    error(5, "write denied on a read-only-open handle");
    close_file(h);
    h = open_file("io.bin", 1);
    begin(0x3f00); cpu.b.x = h; cpu.c.x = 1; cpu.d.x = DATA;
    error(5, "read denied on a write-only-open handle"); close_file(h);
    path_begin(0x3d03, "plain.txt"); error(12, "invalid open access mode");
    path_begin(0x3d70, "plain.txt"); error(12, "invalid open share mode");
    path_begin(0x3d02, "readonly.bin"); error(5, "read-only file denies write even under privileged test user");
    path_begin(0x3d00, "subdir"); error(5, "directories are not regular DOS handles");
    path_begin(0x3d00, "missing.txt"); error(2, "missing leaf open");
    path_begin(0x3d00, "no-parent/file.txt"); error(3, "missing parent open");
    path_begin(0x3d00, "Z:\\plain.txt"); error(15, "invalid-drive open");
    begin(0x4203); cpu.b.x = 5; error(1, "invalid seek origin");

    const uint16_t bad_handle_calls[] = {0x3e00, 0x3f00, 0x4000, 0x4200, 0x4400,
                                      0x4500, 0x4600, 0x5700, 0x5701, 0x6800, 0x71a6};
    for (size_t i = 0; i < sizeof bad_handle_calls / sizeof bad_handle_calls[0]; ++i) {
        begin(bad_handle_calls[i]); cpu.b.x = 64; cpu.c.x = 10;
        error(6, "invalid handle outside the global 64-entry table");
    }
    begin(0x6700); cpu.b.x = 64; ok("set handle count to fixed capacity");
    begin(0x6700); cpu.b.x = 65; error(4, "handle count cannot exceed fixed 64-entry table");

    uint16_t handles[59];
    size_t used = 0;
    for (; used < sizeof handles / sizeof handles[0]; ++used) {
        path_begin(0x3d00, "plain.txt");
        if (!ok("fill DOS handle table")) break;
        handles[used] = cpu.a.x;
    }
    CHECK(used == 59, "handles 5 through 63 provide exactly 59 file slots");
    path_begin(0x3d00, "plain.txt"); error(4, "full DOS handle table");
    if (used) { begin(0x4500); cpu.b.x = handles[0]; error(4, "dup on full DOS handle table"); }
    for (size_t i = 0; i < used; ++i) close_file(handles[i]);
    h = open_file("plain.txt", 0); CHECK(h == 5, "lowest freed DOS handle is reusable"); close_file(h);
}

static void rename_file(uint16_t function, const char *from, const char *to, unsigned expected)
{
    path_begin(function, from);
    putstr(ES, OUT, to);
    if (expected) error(expected, "rename failure"); else ok("rename file");
}

static void check_name_roundtrip(const char *dir, const char *dos_name,
                                 const char *host_name, const char *contents)
{
    char path[PATH_MAX], relative[PATH_MAX], expected[PATH_MAX], resolved[PATH_MAX], output[PATH_MAX];
    snprintf(path, sizeof path, "%s\\%s", dir, dos_name);
    snprintf(relative, sizeof relative, "%s/%s", dir, host_name);
    host_path(expected, sizeof expected, relative);
    putstr(DS, ARG, path);
    CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) == 0 && !strcmp(resolved, expected),
          "NAME COLLISION: DOS spelling resolves to its one exact host entry (%s -> %s)", path, resolved);
    uint16_t h = open_file(path, 0);
    read_equals(h, contents, "NAME COLLISION: classic open reads the assigned entry"); close_file(h);
    path_begin(0x716c, path); cpu.b.x = 0; cpu.d.x = 1;
    if (ok("NAME COLLISION: LFN open of assigned entry")) {
        h = cpu.a.x;
        read_equals(h, contents, "NAME COLLISION: LFN open reads the assigned entry"); close_file(h);
    }
    for (unsigned mode = 1; mode <= 2; ++mode) {
        path_begin(0x7160, path); cpu.c.x = (uint16_t)mode;
        if (!ok("NAME COLLISION: truename of assigned entry")) continue;
        getstr(ES, OUT, output, sizeof output);
        putstr(DS, ARG, output);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) == 0 && !strcmp(resolved, expected),
              "NAME COLLISION: long/short truename round trips to the same host entry");
        if (mode == 2) {
            const char *leaf = strrchr(output, '\\');
            CHECK(leaf && !strcmp(leaf + 1, dos_name), "NAME COLLISION: long truename retains the assigned suffix");
        }
    }
    path_begin(0x71a8, path); cpu.d.h = 1; cpu.d.l = 0;
    if (ok("NAME COLLISION: generate assigned entry's short name")) {
        char alias[13];
        getstr(ES, OUT, alias, sizeof alias);
        snprintf(path, sizeof path, "%s\\%s", dir, alias);
        putstr(DS, ARG, path);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) == 0 && !strcmp(resolved, expected),
              "NAME COLLISION: generated short name selects the same host entry");
    }
}

/* Independent test oracle: FNV-1a hashes every unsigned byte of the entire
 * host basename, including the extension, and XOR-folds the result to 16 bits. */
static uint16_t name_hash16(const char *host)
{
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    return (uint16_t)(hash ^ (hash >> 16));
}

static void test_converted_name_collisions(void)
{
    const uint16_t deletions[] = {0x4100, 0x7141};
    const struct { const char *host, *dos, *contents; uint16_t hash; } names[] = {
        {"face-\xf0\x9f\x98\x80.txt", "face-\xfe~0791.txt", "first emoji", 0x0791},
        {"face-\xf0\x9f\x98\x83.txt", "face-\xfe~911A.txt", "second emoji", 0x911a},
        {"face-\xf0\x9f\x98\x84.txt", "face-\xfe~0E94.txt", "third emoji", 0x0e94},
    };
    for (size_t i = 0; i < 3; ++i)
        CHECK(name_hash16(names[i].host) == names[i].hash,
              "STABLE LOSSY NAME: fixed FNV-1a32 folded16 test vector %zu", i);
    char dir[40], path[PATH_MAX], relative[PATH_MAX], pattern[80];
    struct stat st;
    for (size_t call = 0; call < sizeof deletions / sizeof deletions[0]; ++call) {
        for (size_t removed = 0; removed < 3; ++removed) {
            snprintf(dir, sizeof dir, "Emoji names %zu %zu", call, removed);
            host_path(path, sizeof path, dir);
            host_require(mkdir(path, 0755) == 0, "mkdir emoji stability fixture");
            /* Neither creation order, directory name, nor the surviving
             * neighbors may contribute to the displayed long name. */
            for (int i = 2; i >= 0; --i) {
                snprintf(relative, sizeof relative, "%s/%s", dir, names[i].host);
                host_file(relative, names[i].contents, 0644);
            }
            Found list[8], again[8];
            snprintf(pattern, sizeof pattern, "%s\\*", dir);
            size_t n = find_entries(1, pattern, 0, 1, list, 8);
            size_t m = find_entries(1, pattern, 0, 1, again, 8);
            CHECK(n == 3 && m == 3, "STABLE LOSSY NAME: all three emoji entries are listed");
            if (n != 3 || m != 3) continue;
            for (size_t i = 0; i < 3; ++i) {
                CHECK(!strcmp(list[i].name, names[i].dos),
                      "STABLE LOSSY NAME: entry %zu gets its own ~%04X suffix", i, names[i].hash);
                CHECK(!strcmp(list[i].name, again[i].name) && !strcmp(list[i].alias, again[i].alias),
                      "STABLE LOSSY NAME: repeated enumeration yields deterministic names and aliases");
                CHECK(!strpbrk(list[i].name, "*?"), "STABLE LOSSY NAME: display contains no wildcard");
                check_name_roundtrip(dir, list[i].name, names[i].host, names[i].contents);
                snprintf(path, sizeof path, "%s\\%s", dir, list[i].name);
                rename_file(0x7156, path, path, 0);
                snprintf(relative, sizeof relative, "%s/%s", dir, names[i].host);
                CHECK(host_stat(relative, &st) == 0, "STABLE LOSSY NAME: rename-to-self preserves the host spelling");
            }
            snprintf(path, sizeof path, "%s\\%s", dir, list[removed].name);
            path_begin(deletions[call], path); cpu.si = 0;
            ok("STABLE LOSSY NAME: delete one saved displayed name");
            snprintf(relative, sizeof relative, "%s/%s", dir, names[removed].host);
            CHECK(host_stat(relative, &st) < 0 && errno == ENOENT,
                  "STABLE LOSSY NAME: deletion removes exactly its host file");
            m = find_entries(1, pattern, 0, 1, again, 8);
            CHECK(m == 2, "STABLE LOSSY NAME: exactly two files survive");
            for (size_t i = 0; i < 3; ++i) if (i != removed) {
                CHECK(found_name(again, m, list[i].name) != NULL &&
                      found_name(again, m, names[i].dos) != NULL,
                      "STABLE LOSSY NAME: deleting entry %zu must not rename survivor %zu", removed, i);
                check_name_roundtrip(dir, list[i].name, names[i].host, names[i].contents);
            }
            path_begin(deletions[call], path); cpu.si = 0;
            error(2, "STABLE LOSSY NAME: deleted name cannot select another file");
        }

        /* VC saves two selected long names from one find snapshot. Do not
         * refresh that snapshot between the first and second deletion. */
        snprintf(dir, sizeof dir, "Emoji selection %zu", call);
        host_path(path, sizeof path, dir);
        host_require(mkdir(path, 0755) == 0, "mkdir saved-selection fixture");
        for (size_t i = 0; i < 3; ++i) {
            snprintf(relative, sizeof relative, "%s/%s", dir, names[i].host);
            host_file(relative, names[i].contents, 0644);
        }
        Found selected[8];
        snprintf(pattern, sizeof pattern, "%s\\*", dir);
        size_t n = find_entries(1, pattern, 0, 1, selected, 8);
        CHECK(n == 3, "SAVED SELECTION: find returns the three emoji files");
        if (n != 3) continue;
        for (size_t i = 0; i < 2; ++i) {
            snprintf(path, sizeof path, "%s\\%s", dir, selected[i].name);
            path_begin(deletions[call], path); cpu.si = 0;
            ok("SAVED SELECTION: delete the next saved long name");
            for (size_t j = 0; j < 3; ++j) {
                snprintf(relative, sizeof relative, "%s/%s", dir, names[j].host);
                int present = host_stat(relative, &st) == 0;
                CHECK(j <= i ? !present && errno == ENOENT : present,
                      "SAVED SELECTION: after deletion %zu, host file %zu has the intended existence", i, j);
            }
        }
        check_name_roundtrip(dir, selected[2].name, names[2].host, names[2].contents);
    }

    strcpy(dir, "Name priority");
    host_path(path, sizeof path, dir);
    host_require(mkdir(path, 0755) == 0, "mkdir native-name priority fixture");
    const struct { const char *host, *dos, *contents; int ambiguous; } priority[] = {
        {"Face-\xf0\x9f\x98\x80.txt", "Face-\xfe~AEB1.txt", "first converted", 1},
        {"face-\xf0\x9f\x98\x83.txt", "face-\xfe~911A.txt", "second converted", 1},
        {"face-\xe2\x96\xa0~AEB1.txt", "face-\xfe~AEB1.txt", "native after converted", 0},
        {"FACE-\xe2\x96\xa0~911A.TXT", "FACE-\xfe~911A.TXT", "native before converted", 0},
        {"face-\xe2\x96\xa0.txt", "face-\xfe.txt", "native square", 0},
        {"FACE-\xe2\x96\xa0~1.TXT", "FACE-\xfe~1.TXT", "native suffix one", 0},
        {"face-\xf0\x9f\x98\x81~2.txt", "face-\xfe~2~DEFB.txt", "converted suffix two", 0},
    };
    /* Both host sort directions are deliberate: real names keep their exact
     * spelling even when a lossy name has the same case-folded hash spelling. */
    for (size_t i = 0; i < sizeof priority / sizeof priority[0]; ++i) {
        snprintf(relative, sizeof relative, "%s/%s", dir, priority[i].host);
        host_file(relative, priority[i].contents, 0644);
    }
    Found list[12];
    size_t n = find_entries(1, "Name priority\\*", 0, 1, list, 12);
    CHECK(n == 7, "NAME COLLISION: native and converted collision fixture is complete");
    for (size_t i = 0; i < sizeof priority / sizeof priority[0]; ++i) {
        CHECK(found_name(list, n, priority[i].dos) != NULL,
              "NAME COLLISION: neither real nor hash-suffixed colliding name may be renamed");
        if (!priority[i].ambiguous)
            check_name_roundtrip(dir, priority[i].dos, priority[i].host, priority[i].contents);
    }
    /* A spelling that is not a native name exactly, but matches a native
     * name and an ambiguous converted one case-insensitively, is refused:
     * it is how VC names the converted row, and giving back the native file
     * let F8 on that row delete it. The exact native spelling still works. */
    const char *case_inputs[] = {"Name priority\\FACE-\xfe~aeb1.TXT", "Name priority\\face-\xfe~911a.txt"};
    for (size_t i = 0; i < 2; ++i) {
        char resolved[PATH_MAX];
        putstr(DS, ARG, case_inputs[i]);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) != 0,
              "NAME COLLISION: a case-insensitive spelling shared with a converted name is refused");
    }
    path_begin(0x7141, "Name priority\\Face-\xfe~AEB1.txt"); cpu.si = 0; cpu.c.x = 0;
    error(2, "NAME COLLISION: F8 on the converted row cannot reach the native file");
    snprintf(relative, sizeof relative, "%s/%s", dir, priority[2].host);
    CHECK(host_stat(relative, &st) == 0, "NAME COLLISION: the native file survives a delete of the converted row");
    snprintf(relative, sizeof relative, "%s/%s", dir, priority[0].host);
    CHECK(host_stat(relative, &st) == 0, "NAME COLLISION: the converted file is not deleted either");
    check_name_roundtrip(dir, priority[2].dos, priority[2].host, priority[2].contents);
}

static void test_lossy_hash_collision(void)
{
    /* These distinct one-emoji basenames have different 32-bit FNV hashes
     * (CD009184 and 7D0C2188), both folding to 5C84. Their entire converted
     * DOS names, not merely their suffixes, therefore collide. */
    const char *hosts[] = {"face-\xf0\x9f\x8d\x8b.txt", "face-\xf0\x9f\x90\x8c.txt"};
    const char *contents[] = {"lemon must survive", "snail must survive"};
    const char *dos = "face-\xfe~5C84.txt";
    char path[PATH_MAX], relative[PATH_MAX], resolved[PATH_MAX];
    host_path(path, sizeof path, "Hash collision");
    host_require(mkdir(path, 0755) == 0, "mkdir forced full-name hash collision");
    struct stat before[2], st;
    for (size_t i = 0; i < 2; ++i) {
        CHECK(name_hash16(hosts[i]) == 0x5c84, "HASH COLLISION: fixture has the intended folded FNV value");
        snprintf(relative, sizeof relative, "Hash collision/%s", hosts[i]);
        host_file(relative, contents[i], 0644);
        host_require(host_stat(relative, &before[i]) == 0, "stat hash-collision fixture");
    }
    Found list[4];
    size_t n = find_entries(1, "Hash collision\\*", 0, 1, list, 4);
    CHECK(n == 2 && !strcmp(list[0].name, dos) && !strcmp(list[1].name, dos),
          "HASH COLLISION: both ambiguous entries retain the same own-host-derived displayed name");
    const char *inputs[] = {"Hash collision\\face-\xfe~5C84.txt", "Hash collision\\FACE-\xfe~5c84.TXT"};
    for (size_t i = 0; i < 2; ++i) {
        putstr(DS, ARG, inputs[i]);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) < 0 && errno == ENOENT,
              "HASH COLLISION: allow-missing public resolution must reject ambiguous names");
        path_begin(0x3d00, inputs[i]); error(2, "HASH COLLISION: classic open never selects either file");
        path_begin(0x716c, inputs[i]); cpu.b.x = 0; cpu.d.x = 1;
        error(2, "HASH COLLISION: LFN open never selects either file");
        path_begin(0x3c00, inputs[i]); error(2, "HASH COLLISION: create cannot turn ambiguity into a real name");
        path_begin(0x716c, inputs[i]); cpu.b.x = 2; cpu.d.x = 0x11;
        error(2, "HASH COLLISION: LFN open-or-create must preserve ambiguity");
        path_begin(0x7160, inputs[i]); cpu.c.x = 2;
        error(2, "HASH COLLISION: long truename must not select either file");
        path_begin(0x4100, inputs[i]); error(2, "HASH COLLISION: classic unlink never removes either file");
        path_begin(0x7141, inputs[i]); cpu.si = 0;
        error(2, "HASH COLLISION: LFN literal unlink never removes either file");
        path_begin(0x7141, inputs[i]); cpu.si = 1;
        error(2, "HASH COLLISION: wildcard-mode literal unlink must not bypass ambiguity");
    }
    n = find_entries(1, "Hash collision\\*", 0, 1, list, 4);
    CHECK(n == 2, "HASH COLLISION: rejected creates/deletes preserve exactly the two original entries");
    for (size_t i = 0; i < 2; ++i) {
        snprintf(relative, sizeof relative, "Hash collision/%s", hosts[i]);
        CHECK(host_stat(relative, &st) == 0 && st.st_dev == before[i].st_dev &&
              st.st_ino == before[i].st_ino && st.st_size == before[i].st_size,
              "HASH COLLISION: rejected operations preserve each original inode and size");
        host_path(path, sizeof path, relative);
        int fd = open(path, O_RDONLY);
        host_require(fd >= 0, "open guarded hash-collision fixture");
        char buffer[64] = {0};
        ssize_t got = read(fd, buffer, sizeof buffer);
        CHECK(got == (ssize_t)strlen(contents[i]) && !memcmp(buffer, contents[i], strlen(contents[i])),
              "HASH COLLISION: neither payload is truncated or replaced");
        host_require(close(fd) == 0, "close guarded hash-collision fixture");
    }
}

static void test_literal_wildcard_names(void)
{
    host_file("literal-a.txt", "wildcard guard", 0644);
    const char *names[] = {"literal-?.txt", "literal-*.txt"};
    const char *renamed[] = {"renamed-?.txt", "renamed-*.txt"};
    const uint16_t deletions[] = {0x4100, 0x7141};
    struct stat st;
    for (size_t i = 0; i < 2; ++i) {
        path_begin(deletions[i], names[i]); cpu.si = 0;
        error(2, "literal wildcard deletion with no exact entry is file-not-found");
        path_begin(0x3c00, names[i]);
        if (ok("create accepts a literal wildcard byte")) {
            uint16_t h = cpu.a.x;
            write_bytes(h, "literal contents"); close_file(h);
        }
        CHECK(host_stat(names[i], &st) == 0, "literal wildcard create uses exact host spelling");
        uint16_t h = open_file(names[i], 0);
        read_equals(h, "literal contents", "open treats wildcard bytes literally"); close_file(h);
        path_begin(0x4300, names[i]);
        if (ok("attributes treat wildcard bytes literally")) CHECK(cpu.c.x == A_ARCHIVE, "literal wildcard file attributes");
        rename_file(0x7156, names[i], renamed[i], 0);
        CHECK(host_stat(names[i], &st) < 0 && errno == ENOENT, "literal wildcard rename removes only exact source");
        CHECK(host_stat(renamed[i], &st) == 0, "literal wildcard rename keeps wildcard in exact destination");
        path_begin(deletions[i], renamed[i]); cpu.si = 0;
        ok("non-wildcard deletion removes a literal wildcard name");
        CHECK(host_stat(renamed[i], &st) < 0 && errno == ENOENT, "literal wildcard unlink removes exact target");
        h = open_file("literal-a.txt", 0);
        read_equals(h, "wildcard guard", "literal wildcard operations preserve the matching neighbor"); close_file(h);
    }
}

static void test_name_boundaries(void)
{
    char path[PATH_MAX], relative[PATH_MAX], expected[PATH_MAX], resolved[PATH_MAX];
    host_path(path, sizeof path, "Name edges");
    host_require(mkdir(path, 0755) == 0, "mkdir name-boundary fixture");
    const struct { const char *host, *dos, *contents; } cases[] = {
        {"bad-\xff.txt", "bad-\xfe~059A.txt", "invalid later byte"},
        {"bad-\xfe.txt", "bad-\xfe~D0D3.txt", "invalid earlier byte"},
        {"bad-\xff" "a.txt", "bad-\xfe" "a~F1E9.txt", "ASCII after invalid byte"},
        {"accent-\xc3\xa9.txt", "accent-\xfe~FF3E.txt", "unrepresentable accent"},
        {"Case.txt", "Case.txt", "upper native name"},
        {"case.txt", "case.txt", "lower native name"},
        {"\xd0\x96.txt", "\x86.txt", "upper Cyrillic name"},
        {"\xd0\xb6.txt", "\xa6.txt", "lower Cyrillic name"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        snprintf(relative, sizeof relative, "Name edges/%s", cases[i].host);
        host_file(relative, cases[i].contents, 0644);
    }
    Found list[16];
    size_t n = find_entries(1, "Name edges\\*", 0, 1, list, 16);
    CHECK(n == 8, "NAME COLLISION: invalid UTF-8 and case-collision fixture is complete");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        CHECK(found_name(list, n, cases[i].dos) != NULL, "NAME COLLISION: invalid UTF-8 and CP866 case collisions get safe names");
        check_name_roundtrip("Name edges", cases[i].dos, cases[i].host, cases[i].contents);
    }

    /* Even at the 255-byte limit, case-only lossless neighbors keep their
     * own spelling and remain reachable by an exact native path. */
    for (unsigned extension = 0; extension < 2; ++extension) {
        char host[256], upper[256], dos[256];
        if (!extension) {
            memset(host, 'q', 251); memcpy(host + 251, ".txt", 5);
        } else {
            memcpy(host, "r.", 2); memset(host + 2, 'x', 253); host[255] = 0;
        }
        strcpy(dos, host);
        strcpy(upper, host); upper[0] = (char)(host[0] - ('a' - 'A'));
        snprintf(relative, sizeof relative, "Name edges/%s", host);
        host_file(relative, "lower boundary name", 0644);
        host_path(expected, sizeof expected, relative);
        snprintf(relative, sizeof relative, "Name edges/%s", upper);
        host_file(relative, "upper boundary name", 0644);
        n = find_entries(1, "Name edges\\*", 0, 1, list, 16);
        const Found *entry = found_name(list, n, dos);
        CHECK(entry && strlen(entry->name) == 255 && found_name(list, n, upper),
              "NAME COLLISION: maximum-length lossless case neighbors retain both complete spellings");
        snprintf(path, sizeof path, "Name edges\\%s", dos);
        putstr(DS, ARG, path);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) == 0 && !strcmp(resolved, expected),
              "NAME COLLISION: full-length lossless name resolves to its exact host spelling");
        uint16_t h = open_file(path, 0);
        read_equals(h, "lower boundary name", "NAME COLLISION: full-length collision opens the correct file"); close_file(h);
    }
    /* Lossy names at the same limit still need all five suffix characters,
     * including when an unusually long extension consumes the entire name. */
    for (unsigned extension = 0; extension < 2; ++extension) {
        char host[256], dos[256], suffix[6];
        if (!extension) {
            memset(host, 'q', 250); host[250] = (char)0xff; memcpy(host + 251, ".txt", 5);
        } else {
            memcpy(host, "r.", 2); memset(host + 2, 'x', 252); host[254] = (char)0xff; host[255] = 0;
        }
        snprintf(suffix, sizeof suffix, "~%04X", name_hash16(host));
        if (!extension) {
            memset(dos, 'q', 246); memcpy(dos + 246, suffix, 5); memcpy(dos + 251, ".txt", 5);
        } else {
            dos[0] = 'r'; memcpy(dos + 1, suffix, 5); dos[6] = '.';
            memset(dos + 7, 'x', 248); dos[255] = 0;
        }
        snprintf(relative, sizeof relative, "Name edges/%s", host);
        host_file(relative, "lossy boundary name", 0644);
        host_path(expected, sizeof expected, relative);
        n = find_entries(1, "Name edges\\*", 0, 1, list, 16);
        const Found *entry = found_name(list, n, dos);
        CHECK(entry && strlen(entry->name) == 255,
              "STABLE LOSSY NAME: full-length base/extension retains its complete hash within 255 bytes");
        snprintf(path, sizeof path, "Name edges\\%s", dos);
        putstr(DS, ARG, path);
        CHECK(dos_fs_to_host(DS, ARG, resolved, sizeof resolved) == 0 && !strcmp(resolved, expected),
              "STABLE LOSSY NAME: bounded hash spelling resolves to the full-length host name");
        uint16_t h = open_file(path, 0);
        read_equals(h, "lossy boundary name", "STABLE LOSSY NAME: full-length hash spelling opens its own file");
        close_file(h);
    }
}

/* A symlink to an ancestor directory lists as a file, so a recursive walk
 * cannot loop through it. A symlink elsewhere stays a directory. */
static void test_ancestor_symlink_lists_as_file(void)
{
    char path[PATH_MAX];
    host_path(path, sizeof path, "loopdir");
    host_require(mkdir(path, 0755) == 0, "mkdir loop dir");
    host_path(path, sizeof path, "loopdir/up");
    host_require(symlink("..", path) == 0, "symlink to parent");
    host_path(path, sizeof path, "loopdir/elsewhere");
    host_require(symlink("../xonly", path) == 0, "symlink to a sibling");
    Found list[16];
    size_t n = find_entries(1, "loopdir\\*.*", A_DIR | A_HIDDEN | A_SYSTEM, 1, list, 16);
    const Found *up = found_name(list, n, "up"), *other = found_name(list, n, "elsewhere");
    CHECK(up && !(up->attrs & A_DIR), "a link to an ancestor is listed as a file");
    CHECK(other && (other->attrs & A_DIR), "a link to a sibling directory stays a directory");
}

/* DOS drops trailing spaces and dots, so such names convert like
 * unrepresentable ones and get a stable suffix; the plain name stays itself. */
static void test_trailing_space_and_dot_names(void)
{
    host_file("tail.txt", "plain", 0644);
    host_file("tail.txt ", "spaced", 0644);
    host_file("dotted.", "dotted", 0644);
    Found list[128];
    size_t n = find_entries(1, "*.*", A_DIR | A_HIDDEN | A_SYSTEM, 1, list, 128);
    int plain = 0, spaced = 0, dotted = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *nm = list[i].name;
        if (!strcmp(nm, "tail.txt")) plain++;
        else if (!strncmp(nm, "tail", 4) && strchr(nm, '\xfe') && strchr(nm, '~')) spaced++;
        else if (!strncmp(nm, "dotted", 6) && strchr(nm, '\xfe') && strchr(nm, '~')) dotted++;
    }
    CHECK(plain == 1 && spaced == 1, "tail.txt and 'tail.txt ' get distinct DOS names");
    CHECK(dotted == 1, "a trailing dot converts like an unrepresentable character");
    for (size_t i = 0; i < n; ++i) {
        const char *nm = list[i].name;
        if (strncmp(nm, "tail", 4) || !strchr(nm, '\xfe')) continue;
        path_begin(0x7141, list[i].name);
        cpu.si = 0; /* no wildcards */
        cpu.c.x = 0;
        ok("delete the spaced name by its DOS name");
    }
    struct stat st;
    CHECK(host_stat("tail.txt", &st) == 0, "the plain file is untouched");
    CHECK(host_stat("tail.txt ", &st) < 0, "the spaced file is the one deleted");
}

/* Characters DOS forbids in a name make it unrepresentable. A literal
 * backslash must never act as a path separator. */
static void test_forbidden_characters(void)
{
    char path[PATH_MAX];
    host_path(path, sizeof path, "forbid");
    host_require(mkdir(path, 0755) == 0, "mkdir forbidden-character fixture");
    host_path(path, sizeof path, "forbid/dir");
    host_require(mkdir(path, 0755) == 0, "mkdir forbid/dir");
    host_file("forbid/dir/report.txt", "inside the directory", 0644);
    host_file("forbid/dir\\report.txt", "backslash in the name", 0644);
    host_file("forbid/a*b.txt", "star in the name", 0644);
    Found list[16];
    size_t n = find_entries(1, "forbid\\*.*", A_DIR | A_HIDDEN | A_SYSTEM, 1, list, 16);
    const char *slashed = NULL;
    int starred = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!strncmp(list[i].name, "dir\xfe", 4)) slashed = list[i].name;
        if (!strncmp(list[i].name, "a\xfe", 2)) starred = 1;
        CHECK(!strpbrk(list[i].name, "\\/*?"), "no listed name carries a forbidden character");
    }
    CHECK(slashed != NULL && starred, "backslash and star become the unrepresentable marker");
    if (slashed) {
        char dos[300];
        snprintf(dos, sizeof dos, "forbid\\%s", slashed);
        path_begin(0x7141, dos); cpu.si = 0; cpu.c.x = 0;
        ok("delete the backslash-named file by its DOS name");
    }
    struct stat st;
    CHECK(host_stat("forbid/dir\\report.txt", &st) < 0, "the backslash-named file is the one deleted");
    CHECK(host_stat("forbid/dir/report.txt", &st) == 0, "the file inside dir/ is untouched");
}

static void test_execute_only_directory(void)
{
    char path[PATH_MAX], output[PATH_MAX], expected[PATH_MAX + 40], dos_fixture[PATH_MAX + 3];
    host_path(path, sizeof path, "xonly");
    host_require(mkdir(path, 0755) == 0, "mkdir execute-only directory");
    host_file("xonly/known.txt", "reachable by name", 0644);
    host_path(path, sizeof path, "xonly/\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82 child");
    host_require(mkdir(path, 0755) == 0, "mkdir lossless Cyrillic child of execute-only parent");
    host_file("xonly/\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82 child/known.txt", "reachable child", 0644);
    host_path(path, sizeof path, "xonly");
    host_require(chmod(path, 0111) == 0, "make directory execute-only");
    errno = 0;
    DIR *dir = opendir(path);
    int saved_errno = errno;
    CHECK(dir == NULL && saved_errno == EACCES,
          "EXECUTE-ONLY: listing really fails with EACCES (euid=%lu); run without permission bypass",
          (unsigned long)geteuid());
    if (dir) host_require(closedir(dir) == 0, "close unexpected execute-only listing");
    struct stat st;
    CHECK(host_stat("xonly/known.txt", &st) == 0, "EXECUTE-ONLY: exact child remains reachable by name");
    path_begin(0x3d00, "xonly\\known.txt");
    if (ok("open a file by exact name in an execute-only directory"))
        close_file(cpu.a.x);

    const char *inputs[] = {"xonly\\known.txt", "xonly\\\x92\xa5\xe1\xe2 child\\known.txt"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        path_begin(0x7160, inputs[i]); cpu.c.x = 2;
        if (ok("EXECUTE-ONLY: long truename must not list a lossless component's parent")) {
            getstr(ES, OUT, output, sizeof output);
            snprintf(expected, sizeof expected, "C:%s\\%s", fixture, inputs[i]);
            for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
            CHECK(!strcmp(output, expected), "EXECUTE-ONLY: long truename preserves lossless ASCII/Cyrillic spelling");
        }
    }
    path_begin(0x713b, "xonly\\\x92\xa5\xe1\xe2 child");
    ok("EXECUTE-ONLY: chdir to a reachable child of the unreadable parent");
    begin(0x7147); cpu.d.l = 3; cpu.si = OUT;
    if (ok("EXECUTE-ONLY: LFN getcwd must not list lossless parents")) {
        getstr(DS, OUT, output, sizeof output);
        snprintf(expected, sizeof expected, "%s\\xonly\\\x92\xa5\xe1\xe2 child", fixture + 1);
        for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
        CHECK(!strcmp(output, expected), "EXECUTE-ONLY: LFN getcwd returns the complete lossless CP866 path");
    }
    snprintf(dos_fixture, sizeof dos_fixture, "C:%s", fixture);
    path_begin(0x713b, dos_fixture); ok("restore DOS cwd after execute-only conversion");
    host_require(chmod(path, 0755) == 0, "restore execute-only directory");
}

static void test_unlink_directory_symlinks(void)
{
    const uint16_t calls[] = {0x4100, 0x7141};
    const char *targets[] = {".", "unlink-target"};
    const char *links[] = {"..", "../unlink-target"};
    const char *guards[] = {"unlink-ancestor-keep.txt", "unlink-target/keep.txt"};
    char path[PATH_MAX], name[100], pattern[100];
    host_path(path, sizeof path, "unlink-target");
    host_require(mkdir(path, 0755) == 0, "mkdir unlink symlink target");
    host_path(path, sizeof path, "unlink-links");
    host_require(mkdir(path, 0755) == 0, "mkdir unlink symlink parent");
    host_file(guards[0], "untouched unlink target", 0644);
    host_file(guards[1], "untouched unlink target", 0644);
    struct stat target_before[2], guard_before[2], st;
    for (size_t kind = 0; kind < 2; ++kind) {
        host_require(host_stat(targets[kind], &target_before[kind]) == 0, "stat unlink directory target");
        host_require(host_stat(guards[kind], &guard_before[kind]) == 0, "stat unlink target guard");
        for (size_t call = 0; call < sizeof calls / sizeof calls[0]; ++call) {
            snprintf(name, sizeof name, "unlink-links/link-%zu-%zu", kind, call);
            host_path(path, sizeof path, name);
            host_require(symlink(links[kind], path) == 0, "make unlink directory symlink");
            snprintf(pattern, sizeof pattern, "unlink-links\\link-%zu-%zu", kind, call);
            Found list[4];
            size_t n = find_entries(1, pattern, A_DIR, 1, list, 4);
            CHECK(n == 1 && !!(list[0].attrs & A_DIR) == (kind == 1),
                  "SYMLINK UNLINK: ancestor is listed as a file; ordinary directory link remains a directory");
            path_begin(calls[call], pattern); cpu.si = 0;
            ok("SYMLINK UNLINK: 41h/7141h removes the final directory symlink itself");
            CHECK(lstat(path, &st) < 0 && errno == ENOENT, "SYMLINK UNLINK: only the link itself is gone");
            CHECK(host_stat(targets[kind], &st) == 0 && S_ISDIR(st.st_mode) &&
                  st.st_dev == target_before[kind].st_dev && st.st_ino == target_before[kind].st_ino &&
                  st.st_mtim.tv_sec == target_before[kind].st_mtim.tv_sec &&
                  st.st_mtim.tv_nsec == target_before[kind].st_mtim.tv_nsec,
                  "SYMLINK UNLINK: target directory inode and contents timestamp remain intact");
            CHECK(host_stat(guards[kind], &st) == 0 && st.st_dev == guard_before[kind].st_dev &&
                  st.st_ino == guard_before[kind].st_ino && st.st_size == guard_before[kind].st_size,
                  "SYMLINK UNLINK: target's guard file inode and size remain intact");
            uint16_t h = open_file(guards[kind], 0);
            read_equals(h, "untouched unlink target", "SYMLINK UNLINK: target file contents are unchanged");
            close_file(h);
            path_begin(calls[call], pattern); cpu.si = 0;
            error(2, "SYMLINK UNLINK: repeated deletion cannot remove the target");
        }
    }
}

static void test_rmdir_symlinks(void)
{
    char path[PATH_MAX], name[80];
    struct stat target_before, file_before, st;
    host_path(path, sizeof path, "rmdir-target");
    host_require(mkdir(path, 0755) == 0, "mkdir symlink-rmdir target");
    host_file("rmdir-target/keep.txt", "untouched directory target", 0644);
    host_require(host_stat("rmdir-target", &target_before) == 0, "stat symlink-rmdir target");
    host_require(host_stat("rmdir-target/keep.txt", &file_before) == 0, "stat symlink-rmdir guard file");
    const uint16_t calls[] = {0x3a00, 0x713a};
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; ++i) {
        snprintf(name, sizeof name, "rmdir-dirlink-%zu", i);
        host_path(path, sizeof path, name);
        host_require(symlink("rmdir-target", path) == 0, "make nonempty-directory symlink");
        path_begin(calls[i], name);
        ok("SYMLINK RMDIR: remove a link to a nonempty directory");
        CHECK(lstat(path, &st) < 0 && errno == ENOENT, "SYMLINK RMDIR: the directory link itself is gone");
        CHECK(host_stat("rmdir-target", &st) == 0 && S_ISDIR(st.st_mode) &&
              st.st_dev == target_before.st_dev && st.st_ino == target_before.st_ino &&
              st.st_mtim.tv_sec == target_before.st_mtim.tv_sec && st.st_mtim.tv_nsec == target_before.st_mtim.tv_nsec,
              "SYMLINK RMDIR: target directory inode and contents timestamp are untouched");
        CHECK(host_stat("rmdir-target/keep.txt", &st) == 0 && st.st_dev == file_before.st_dev &&
              st.st_ino == file_before.st_ino && st.st_size == file_before.st_size,
              "SYMLINK RMDIR: target's file inode and size are untouched");
        uint16_t h = open_file("rmdir-target/keep.txt", 0);
        read_equals(h, "untouched directory target", "SYMLINK RMDIR: target file content is untouched"); close_file(h);
        const char *dangling[] = {"rmdir-no-such-target", "plain.txt/no-directory"};
        for (size_t j = 0; j < 2; ++j) {
            snprintf(name, sizeof name, "rmdir-dangling-%zu-%zu", i, j);
            host_path(path, sizeof path, name);
            host_require(symlink(dangling[j], path) == 0, "make dangling rmdir symlink");
            if (j) strcat(name, "\\"); /* Still the final component, not an intermediate directory. */
            path_begin(calls[i], name); ok("SYMLINK RMDIR: remove a dangling symlink");
            CHECK(lstat(path, &st) < 0 && errno == ENOENT, "SYMLINK RMDIR: dangling link itself is gone");
        }
        snprintf(name, sizeof name, "rmdir-filelink-%zu", i);
        host_path(path, sizeof path, name);
        host_require(symlink("plain.txt", path) == 0, "make file rmdir symlink");
        path_begin(calls[i], name); error(3, "SYMLINK RMDIR: link to a regular file still fails");
        CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode), "SYMLINK RMDIR: rejected file link remains");
        h = open_file("plain.txt", 0);
        read_equals(h, "plain-data", "SYMLINK RMDIR: rejected link leaves the file target untouched"); close_file(h);
        snprintf(name, sizeof name, "rmdir-cwdlink-%zu", i);
        host_path(path, sizeof path, name);
        host_require(symlink(".", path) == 0, "make link to current directory");
        path_begin(calls[i], name); ok("SYMLINK RMDIR: a link to cwd is not cwd itself");
        CHECK(lstat(path, &st) < 0 && errno == ENOENT, "SYMLINK RMDIR: link to cwd is removed without recursing");
    }
}

static void test_paths_and_mutations(void)
{
    char out[PATH_MAX], expected[PATH_MAX + 32], dos_fixture[PATH_MAX + 3];
    struct stat st;
    snprintf(dos_fixture, sizeof dos_fixture, "C:%s", fixture);
    path_begin(0x3900, "made"); ok("classic mkdir");
    CHECK(host_stat("made", &st) == 0 && S_ISDIR(st.st_mode), "mkdir creates host directory");
    path_begin(0x3900, "made"); error(5, "mkdir existing directory");
    path_begin(0x7139, "Made long directory"); ok("LFN mkdir");
    path_begin(0x713b, "Made long directory"); ok("LFN chdir");
    host_require(getcwd(out, sizeof out) != NULL, "getcwd after DOS chdir");
    CHECK(strcmp(out, fixture) == 0, "DOS chdir never changes host process cwd");
    begin(0x7147); cpu.d.l = 3; cpu.si = OUT;
    if (ok("LFN getcwd")) {
        getstr(DS, OUT, out, sizeof out);
        snprintf(expected, sizeof expected, "%s\\Made long directory", fixture + 1);
        for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
        CHECK(strcmp(out, expected) == 0, "LFN getcwd has no drive/leading slash and preserves long spelling");
    }
    begin(0x4700); cpu.d.l = 0; cpu.si = OUT;
    if (ok("classic getcwd")) {
        getstr(DS, OUT, out, sizeof out);
        size_t len = strlen(out);
        CHECK(len >= 8 && !strcmp(out + len - 8, "MADELO~1"), "classic getcwd uses directory aliases: '%s'", out);
        CHECK(out[0] != '\\' && strchr(out, ':') == NULL, "classic getcwd strips drive and leading separator");
    }
    path_begin(0x3a00, "."); error(16, "cannot remove current directory");
    path_begin(0x713a, "."); error(16, "LFN cannot remove current directory");
    path_begin(0x3b00, ".."); ok("classic parent chdir");
    path_begin(0x3b00, "MADELO~1"); ok("chdir through generated directory alias");
    path_begin(0x3b00, dos_fixture); ok("restore DOS cwd by absolute C path");
    path_begin(0x3a00, "made"); ok("classic rmdir");
    path_begin(0x713a, "Made long directory"); ok("LFN rmdir");
    CHECK(host_stat("made", &st) < 0 && errno == ENOENT, "rmdir removes host directory");
    path_begin(0x3a00, "Long directory"); error(5, "nonempty directory removal denied");
    path_begin(0x3b00, "no-such-directory"); error(3, "missing chdir path");
    path_begin(0x7139, "no-parent/child"); error(3, "mkdir missing parent");
    path_begin(0x713b, "A:\\"); error(15, "LFN invalid-drive chdir");
    begin(0x4700); cpu.d.l = 4; cpu.si = OUT; error(15, "classic invalid-drive getcwd");
    begin(0x7147); cpu.d.l = 2; cpu.si = OUT; error(15, "LFN invalid-drive getcwd");

    path_begin(0x5b00, "new-only.bin"); cpu.c.x = 0;
    if (ok("create-new file")) close_file(cpu.a.x);
    path_begin(0x5b00, "new-only.bin"); error(80, "create-new existing file");
    rename_file(0x5600, "new-only.bin", "renamed.bin", 0);
    rename_file(0x7156, "renamed.bin", "Renamed long file.bin", 0);
    CHECK(host_stat("Renamed long file.bin", &st) == 0, "LFN rename writes long host spelling");
    rename_file(0x7156, "Renamed long file.bin", "lower83.bin", 0);
    CHECK(host_stat("lower83.bin", &st) == 0, "LFN rename to an 8.3 name keeps its lowercase spelling");
    rename_file(0x7156, "lower83.bin", "Renamed long file.bin", 0);
    rename_file(0x5600, "Renamed long file.bin", "plain.txt", 80);
    rename_file(0x7156, "absent", "target", 2);
    rename_file(0x5600, "Renamed long file.bin", "Z:\\target", 15);
    path_begin(0x4100, "Renamed long file.bin"); ok("classic unlink");
    path_begin(0x4100, "Renamed long file.bin"); error(2, "unlink missing file");
    path_begin(0x4100, "readonly.bin"); error(5, "unlink read-only file denied");
    path_begin(0x4100, "subdir"); error(5, "unlink cannot remove directory");
    host_file("delete-a.tmp", "a", 0644);
    host_file("delete-b.tmp", "b", 0644);
    host_file(".delete-c.tmp", "hidden", 0644);
    path_begin(0x7141, "delete-?.tmp"); cpu.si = 1; cpu.c.x = 0;
    ok("LFN wildcard unlink");
    CHECK(host_stat("delete-a.tmp", &st) < 0 && errno == ENOENT, "wildcard removed first file");
    CHECK(host_stat("delete-b.tmp", &st) < 0 && errno == ENOENT, "wildcard removed second file");
    CHECK(host_stat(".delete-c.tmp", &st) == 0, "wildcard leaves nonmatching hidden file");
    path_begin(0x7141, ".delete-*.tmp"); cpu.si = 1; cpu.c.x = 0;
    error(2, "wildcard delete honors hidden exclusion");
    path_begin(0x7141, ".delete-*.tmp"); cpu.si = 1; cpu.c.x = A_HIDDEN;
    ok("wildcard delete includes hidden with explicit mask");
    path_begin(0x7141, "none-*.tmp"); cpu.si = 1;
    error(2, "no-match wildcard unlink");
    host_file("delete-exact.tmp", "exact", 0644);
    path_begin(0x7141, "delete-exact.tmp"); cpu.si = 0; ok("LFN literal unlink");
    host_file(".delete-literal.tmp", "hidden literal", 0644);
    path_begin(0x7141, ".delete-literal.tmp"); cpu.si = 1; cpu.c.x = 0;
    error(2, "wildcard-mode literal deletion still excludes hidden files without a mask");
    CHECK(host_stat(".delete-literal.tmp", &st) == 0, "wildcard-mode literal hidden exclusion preserves the file");
    path_begin(0x7141, ".delete-literal.tmp"); cpu.si = 1; cpu.c.x = A_HIDDEN;
    ok("wildcard-mode literal deletion honors the explicit hidden mask");
    CHECK(host_stat(".delete-literal.tmp", &st) < 0 && errno == ENOENT,
          "wildcard-mode literal deletion removes the included hidden file");
    host_file("delete-extensionless", "extensionless", 0644);
    host_file("delete-extensionless.txt", "extension guard", 0644);
    path_begin(0x7141, "delete-extensionless."); cpu.si = 1; cpu.c.x = 0;
    ok("wildcard-mode literal trailing dot still matches the extensionless file");
    CHECK(host_stat("delete-extensionless", &st) < 0 && errno == ENOENT,
          "trailing-dot literal deletion removes the extensionless file");
    CHECK(host_stat("delete-extensionless.txt", &st) == 0,
          "trailing-dot literal deletion preserves a neighbor with an extension");

    /* 5A appends its generated name to an input directory path. */
    path_begin(0x5a00, "subdir\\"); cpu.c.x = 0;
    if (ok("create unique temporary file")) {
        uint16_t h = cpu.a.x;
        getstr(DS, ARG, out, sizeof out);
        CHECK(strncmp(out, "subdir\\", 7) == 0 && strlen(out) > 7,
              "temporary create appends an 8.3 filename to DS:DX");
        write_bytes(h, "temp"); close_file(h);
        path_begin(0x4100, out); ok("unlink generated temporary file");
    }
    path_begin(0x3d00, "Long directory\\Mixed name.dat");
    if (ok("nested long directory resolution")) {
        uint16_t h = cpu.a.x;
        read_equals(h, "nested", "nested contents"); close_file(h);
    }
    path_begin(0x3d00, "LONGDI~1\\MIXEDN~1.DAT");
    if (ok("nested alias path resolution")) {
        uint16_t h = cpu.a.x;
        read_equals(h, "nested", "nested alias contents"); close_file(h);
    }
    putstr(DS, ARG, "C:subdir/../plain.txt");
    int resolved = dos_fs_to_host(DS, ARG, out, sizeof out);
    host_path(expected, sizeof expected, "plain.txt");
    CHECK(resolved == 0 && strcmp(out, expected) == 0, "public resolver handles drive-relative paths and dots");
    putstr(DS, ARG, "plain.txt");
    CHECK(dos_fs_to_host(DS, ARG, out, 2) != 0, "public resolver refuses undersized output buffer");
}

static void test_extended_open(void)
{
    struct stat st;
    path_begin(0x6c00, "extended.bin"); cpu.b.x = 2; cpu.c.x = 0; cpu.d.x = 0x11;
    if (ok("6C create-if-missing/open-if-existing")) {
        uint16_t h = cpu.a.x;
        CHECK(cpu.c.x == 2, "6C returns action2 for created file");
        write_bytes(h, "extended"); close_file(h);
    }
    path_begin(0x6c00, "extended.bin"); cpu.b.x = 0; cpu.d.x = 0x01;
    if (ok("6C open existing")) {
        uint16_t h = cpu.a.x;
        CHECK(cpu.c.x == 1, "6C returns action1 for opened file");
        read_equals(h, "extended", "6C does not truncate open-existing"); close_file(h);
    }
    path_begin(0x6c00, "extended.bin"); cpu.b.x = 2; cpu.d.x = 0x12;
    if (ok("6C replace existing")) {
        CHECK(cpu.c.x == 3, "6C returns action3 for replaced file");
        close_file(cpu.a.x);
    }
    CHECK(host_stat("extended.bin", &st) == 0 && st.st_size == 0, "6C replacement truncates");
    path_begin(0x716c, "Extended long created.dat"); cpu.b.x = 2; cpu.d.x = 0x10;
    if (ok("716C create long filename")) {
        CHECK(cpu.c.x == 2, "716C returns created action2");
        close_file(cpu.a.x);
    }
    path_begin(0x716c, "Extended long created.dat"); cpu.b.x = 0; cpu.d.x = 0x10;
    error(80, "716C fail-if-existing");
    path_begin(0x6c00, "no-extended-file"); cpu.b.x = 0; cpu.d.x = 1;
    error(2, "6C fail-if-missing");
    path_begin(0x716c, "plain.txt"); cpu.b.x = 3; cpu.d.x = 1;
    error(12, "716C invalid access mode");
    path_begin(0x6c00, "plain.txt"); cpu.b.x = 0; cpu.d.x = 0x33;
    error(12, "6C invalid action flags");
}

static uint16_t extended_open(const char *name, uint16_t mode, uint16_t action,
                              unsigned expected, const char *why)
{
    path_begin(0x716c, name);
    cpu.b.x = mode;
    cpu.d.x = action;
    if (expected) { error(expected, why); return 0xffff; }
    return ok(why) ? cpu.a.x : 0xffff;
}

static void test_share_modes(void)
{
    host_file("Shared long file.bin", "shared-content", 0644);
    uint16_t h = extended_open("Shared long file.bin", 0x12, 1, 0, "open RW deny-all");
    extended_open("Shared long file.bin", 0x40, 1, 32, "existing deny-all rejects another reader");
    extended_open("SHARED~1.BIN", 0x40, 1, 32, "sharing follows8.3 alias to the same file");
    extended_open("Shared long file.bin", 0x42, 0x12, 32, "denied replacement cannot truncate");
    read_equals(h, "shared-content", "sharing failure preserves original content");
    char source[PATH_MAX], linked[PATH_MAX];
    host_path(source, sizeof source, "Shared long file.bin");
    host_path(linked, sizeof linked, "share-hardlink.bin");
    host_require(link(source, linked) == 0, "create sharing identity hardlink");
    extended_open("share-hardlink.bin", 0x40, 1, 32, "sharing checks device/inode through hardlink");
    begin(0x4500); cpu.b.x = h;
    uint16_t duplicate = ok("duplicate retains sharing restriction") ? cpu.a.x : 0xffff;
    close_file(h);
    extended_open("Shared long file.bin", 0x40, 1, 32, "duplicate keeps deny-all after original closes");
    close_file(duplicate);
    h = extended_open("Shared long file.bin", 0x40, 1, 0, "last close releases sharing restriction");
    extended_open("Shared long file.bin", 0x12, 1, 32, "new deny-all conflicts with already-open reader");
    close_file(h);

    h = extended_open("Shared long file.bin", 0x20, 1, 0, "read with deny-write");
    extended_open("Shared long file.bin", 0x40, 2, 32,
                  "replacement counts as writing even when returned access is read-only");
    read_equals(h, "shared-content", "read-only replacement denial preserves content");
    uint16_t other = extended_open("Shared long file.bin", 0x40, 1, 0, "deny-write permits another reader");
    close_file(other);
    extended_open("Shared long file.bin", 0x41, 1, 32, "deny-write rejects a writer");
    close_file(h);
    h = extended_open("Shared long file.bin", 0x41, 1, 0, "write with deny-none");
    extended_open("Shared long file.bin", 0x20, 1, 32, "new deny-write conflicts with old writer");
    close_file(h);
    h = extended_open("Shared long file.bin", 0x31, 1, 0, "write with deny-read");
    other = extended_open("Shared long file.bin", 0x41, 1, 0, "deny-read permits another writer");
    close_file(other);
    extended_open("Shared long file.bin", 0x40, 1, 32, "deny-read rejects a reader");
    close_file(h);
    h = extended_open("Shared long file.bin", 0x40, 1, 0, "read with deny-none");
    extended_open("Shared long file.bin", 0x31, 1, 32, "new deny-read conflicts with old reader");
    close_file(h);
    h = open_file("Shared long file.bin", 2);
    other = open_file("SHARED~1.BIN", 0);
    CHECK(h != other && h < 64 && other < 64, "single-process compatibility-mode opens stay compatible");
    close_file(other); close_file(h);
}

static void test_empty_file_specs(void)
{
    host_file("empty-guard.bin", "cwd guard", 0644);
    host_file("subdir/empty-guard.bin", "subdir guard", 0644);
    const char *specs[] = {"", "subdir\\"};
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; ++i) {
        path_begin(0x7141, specs[i]); cpu.si = 1;
        error(3, "empty wildcard deletion filespec must not mean delete-all");
        path_begin(0x4e00, specs[i]); cpu.c.x = A_DIR;
        error(3, "empty classic find filespec rejected");
        path_begin(0x714e, specs[i]); cpu.c.x = A_DIR; cpu.si = 1;
        error(3, "empty LFN find filespec rejected");
        uint16_t h = open_file("empty-guard.bin", 0);
        read_equals(h, "cwd guard", "empty filespec preserves cwd files"); close_file(h);
        h = open_file("subdir/empty-guard.bin", 0);
        read_equals(h, "subdir guard", "empty filespec preserves subdirectory files"); close_file(h);
    }
}

static void test_cross_device_rename(void)
{
    struct stat source_mount, target_mount, st;
    char target_dir[PATH_MAX], target_file[PATH_MAX], dos_target[PATH_MAX + 3];
    int n = snprintf(target_dir, sizeof target_dir, "%s/build/dos-fs-cross-XXXXXX", original_cwd);
    host_require(n >= 0 && (size_t)n < sizeof target_dir, "cross-device fixture template");
    char build_dir[PATH_MAX];
    n = snprintf(build_dir, sizeof build_dir, "%s/build", original_cwd);
    host_require(n >= 0 && (size_t)n < sizeof build_dir, "build directory path");
    if (stat(fixture, &source_mount) != 0 || stat(build_dir, &target_mount) != 0 ||
        source_mount.st_dev == target_mount.st_dev || access(build_dir, W_OK) != 0) {
        puts("SKIP optional real EXDEV test: /tmp and writable build/ are not separate filesystems.");
        return;
    }
    host_require(mkdtemp(target_dir) != NULL, "mkdtemp cross-device fixture");
    host_file("cross-source.bin", "preserve on EXDEV", 0644);
    n = snprintf(target_file, sizeof target_file, "%s/destination.bin", target_dir);
    host_require(n >= 0 && (size_t)n < sizeof target_file, "cross-device target path");
    snprintf(dos_target, sizeof dos_target, "C:%s", target_file);
    rename_file(0x7156, "cross-source.bin", dos_target, 17);
    CHECK(host_stat("cross-source.bin", &st) == 0, "EXDEV preserves original file");
    CHECK(lstat(target_file, &st) < 0 && errno == ENOENT, "EXDEV creates no destination file");
    /* Both paths are exact descendants of our fresh, private mkdtemp directory. */
    if (lstat(target_file, &st) == 0) host_require(unlink(target_file) == 0, "cleanup unexpected rename destination");
    host_require(rmdir(target_dir) == 0, "remove private cross-device directory");
}

static void test_devices_and_ioctl(void)
{
    for (unsigned h = 0; h < 5; ++h) {
        begin(0x4400); cpu.b.x = (uint16_t)h;
        if (ok("standard device information")) CHECK(cpu.d.x & 0x80, "handle%u is a device", h);
    }
    console_size = 0;
    write_bytes(1, "stdout"); write_bytes(2, "stderr");
    CHECK(console_size == 12 && !memcmp(console_bytes, "stdoutstderr", 12),
          "stdout and stderr dispatch through con_write");
    begin(0x3f00); cpu.b.x = 0; cpu.c.x = 10; cpu.d.x = DATA;
    if (ok("stdin returns immediate EOF")) CHECK(cpu.a.x == 0, "stdin read reports zero bytes");
    begin(0x4401); cpu.b.x = 1; cpu.d.x = 0; ok("set device information");
    begin(0x4406); cpu.b.x = 0;
    if (ok("device input status")) CHECK(cpu.a.l == 0, "stdin input status agrees with EOF");
    begin(0x4407); cpu.b.x = 1;
    if (ok("device output status")) CHECK(cpu.a.l == 0xff, "console output is ready");
    uint16_t h = open_file("plain.txt", 0);
    begin(0x4400); cpu.b.x = h;
    if (ok("disk file information")) CHECK(!(cpu.d.x & 0x80), "regular handle is not a character device");
    begin(0x4406); cpu.b.x = h;
    if (ok("regular file input status")) CHECK(cpu.a.l == 0xff, "nonempty file is input-ready");
    seek_file(h, 2, 0);
    begin(0x4406); cpu.b.x = h;
    if (ok("file input status at EOF")) CHECK(cpu.a.l == 0, "EOF status is not ready");
    close_file(h);
    begin(0x4408); cpu.b.l = 3;
    if (ok("C removable status")) CHECK(cpu.a.x == 1, "C is fixed, not removable");
    begin(0x4409); cpu.b.l = 3;
    if (ok("C remote status")) CHECK(!(cpu.d.x & 0x9000), "C is local and not SUBST");
    begin(0x440e); cpu.b.l = 3;
    if (ok("get logical drive map")) CHECK(cpu.a.l == 0, "C has a single logical mapping");
    begin(0x440f); cpu.b.l = 3;
    if (ok("set logical drive map")) CHECK(cpu.a.l == 0, "single drive mapping is fixed");
    const uint16_t drive_calls[] = {0x4408, 0x4409, 0x440e, 0x440f};
    for (size_t i = 0; i < sizeof drive_calls / sizeof drive_calls[0]; ++i) {
        begin(drive_calls[i]); cpu.b.l = 1; error(15, "IOCTL invalid drive");
    }
    begin(0x4402); cpu.b.x = 1; error(1, "unsupported IOCTL subfunction");
    begin(0x44ff); cpu.b.x = 1; error(1, "unknown IOCTL subfunction");
    begin(0x0e00); cpu.d.l = 2;
    if (ok("select C drive")) CHECK(cpu.a.l == 8, "0E returns LASTDRIVE H (eight drive positions)");
    begin(0x1900);
    if (ok("current drive")) CHECK(cpu.a.l == 2, "19 returns C index2");
    for (unsigned drive = 0; drive < 26; ++drive) {
        if (drive == 2 || drive == 7) continue;
        begin(0x0e00); cpu.d.l = (uint8_t)drive; error(15, "only C and H are valid");
    }
    begin(0x1900);
    if (ok("drive after failed change")) CHECK(cpu.a.l == 2, "invalid drive selection preserves C");
}

static uint16_t packed_date(unsigned year, unsigned month, unsigned day)
{
    return (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

static uint16_t packed_time(unsigned hour, unsigned minute, unsigned second)
{
    return (uint16_t)((hour << 11) | (minute << 5) | (second / 2));
}

static time_t timestamp(unsigned year, unsigned month, unsigned day,
                         unsigned hour, unsigned minute, unsigned second)
{
    struct tm tm = {0};
    tm.tm_year = (int)year - 1900;
    tm.tm_mon = (int)month - 1;
    tm.tm_mday = (int)day;
    tm.tm_hour = (int)hour;
    tm.tm_min = (int)minute;
    tm.tm_sec = (int)second;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static void test_attributes_and_times(void)
{
    struct stat st;
    host_file("clock.bin", "clock", 0644);
    uint16_t h = open_file("clock.bin", 2);
    uint16_t date = packed_date(2024, 2, 29), time = packed_time(12, 34, 56);
    time_t seconds = timestamp(2024, 2, 29, 12, 34, 56);
    begin(0x5701); cpu.b.x = h; cpu.c.x = time; cpu.d.x = date;
    ok("set DOS handle modification timestamp");
    begin(0x5700); cpu.b.x = h;
    if (ok("get DOS handle modification timestamp"))
        CHECK(cpu.c.x == time && cpu.d.x == date, "5701/5700 packed timestamp round trip");
    CHECK(host_stat("clock.bin", &st) == 0 && st.st_mtime == seconds, "DOS timestamp reaches host mtime");
    begin(0x5701); cpu.b.x = h; cpu.c.x = 0; cpu.d.x = packed_date(2024, 2, 30);
    error(13, "reject normalized-invalid file date");
    begin(0x5701); cpu.b.x = h; cpu.c.x = packed_time(24, 0, 0); cpu.d.x = date;
    error(13, "reject invalid file time");
    for (unsigned sub = 2; sub <= 7; ++sub) {
        begin((uint16_t)(0x5700 | sub)); cpu.b.x = h;
        error(1, "5702-5707 unsupported extended attributes/time operation");
    }
    host_times("clock.bin", 0, 0);
    begin(0x5700); cpu.b.x = h;
    if (ok("timestamp before DOS epoch"))
        CHECK(cpu.c.x == 0 && cpu.d.x == packed_date(1980, 1, 1), "old timestamp clamps to start1980");
    host_times("clock.bin", timestamp(2200, 1, 1, 0, 0, 0), 0);
    begin(0x5700); cpu.b.x = h;
    if (ok("timestamp after DOS ceiling"))
        CHECK(cpu.c.x == packed_time(23, 59, 58) && cpu.d.x == packed_date(2107, 12, 31),
              "future timestamp clamps to final even second2107");
    host_times("clock.bin", seconds, 0);

    path_begin(0x4300, "clock.bin");
    if (ok("classic get attributes")) CHECK(cpu.c.x == A_ARCHIVE, "regular file has archive attribute");
    path_begin(0x4301, "clock.bin"); cpu.c.x = A_ARCHIVE | A_HIDDEN | A_SYSTEM;
    ok("setting ignored attribute bits on already-writable file");
    CHECK(host_stat("clock.bin", &st) == 0 && (st.st_mode & 0777) == 0644,
          "ignored hidden/system/archive bits do not grant group/other write permissions");
    path_begin(0x4301, "clock.bin"); cpu.c.x = A_RO; ok("set read-only attribute");
    CHECK(host_stat("clock.bin", &st) == 0 && !(st.st_mode & 0222), "set read-only clears all three write bits");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 0;
    if (ok("LFN get attributes")) CHECK(cpu.c.x == (A_RO | A_ARCHIVE), "7143 BL0 returns attrs in CX");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 1; cpu.c.x = A_HIDDEN | A_SYSTEM | A_ARCHIVE;
    ok("LFN clear readonly; unsupported attribute bits succeed");
    CHECK(host_stat("clock.bin", &st) == 0 && (st.st_mode & 0222) == 0222,
          "clearing readonly enables owner/group/other writes together");
    path_begin(0x4300, "clock.bin");
    if (ok("get attrs after ignored hidden/system")) CHECK(cpu.c.x == A_ARCHIVE, "hidden/system/archive setting has no effect");
    path_begin(0x4301, ".secret"); cpu.c.x = 0; ok("cannot clear name-derived hidden flag by chmod");
    path_begin(0x4300, ".secret");
    if (ok("get dotfile attributes")) CHECK(cpu.c.x == (A_HIDDEN | A_ARCHIVE), "dotfile stays hidden");
    host_file("owner-ro.bin", "owner cannot write", 0466);
    path_begin(0x4300, "owner-ro.bin");
    if (ok("read-only classification tests owner bit specifically"))
        CHECK(cpu.c.x == (A_RO | A_ARCHIVE), "group/other write permission does not cancel owner read-only");
    path_begin(0x3d02, "owner-ro.bin"); error(5, "owner read-only denies write despite group/other bits");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 2;
    if (ok("LFN compressed-size query"))
        CHECK((((uint32_t)cpu.d.x << 16) | cpu.a.x) == 5, "uncompressed-size query returns logical size");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 3; cpu.c.x = time; cpu.di = date;
    ok("7143 BL3 sets mtime in CX:DI");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 4;
    if (ok("7143 BL4 gets mtime")) CHECK(cpu.c.x == time && cpu.di == date, "path mtime round trip");
    uint16_t access_date = packed_date(2023, 6, 15);
    path_begin(0x7143, "clock.bin"); cpu.b.l = 5; cpu.di = access_date;
    ok("7143 BL5 sets access date");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 6;
    if (ok("7143 BL6 gets access date")) CHECK(cpu.di == access_date, "path access-date round trip");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 7; cpu.c.x = time; cpu.di = date; cpu.si = 125;
    ok("7143 BL7 sets creation timestamp and hundredths");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 8;
    if (ok("7143 BL8 gets creation timestamp"))
        CHECK(cpu.c.x == time && cpu.di == date && cpu.si == 125,
              "creation time preserves odd-second hundredths 125 (got %u)", cpu.si);
    path_begin(0x7143, "clock.bin"); cpu.b.l = 7; cpu.c.x = time; cpu.di = date; cpu.si = 200;
    error(13, "creation hundredths must be below200");
    path_begin(0x7143, "clock.bin"); cpu.b.l = 9; error(1, "invalid 7143 attribute/time operation");
    path_begin(0x4300, "missing-parent/file"); error(3, "attribute query missing parent");
    path_begin(0x7143, "absent"); cpu.b.l = 0; error(2, "LFN attribute query missing leaf");

    /* LFN FILETIME output is UTC 100 ns units, not a local DOS word pair. */
    uint64_t filetime = ((uint64_t)seconds + UINT64_C(11644473600)) * UINT64_C(10000000);
    uint64_t access_filetime = ((uint64_t)timestamp(2023, 6, 15, 0, 0, 0) + UINT64_C(11644473600))
                               * UINT64_C(10000000);
    begin(0x71a6); cpu.b.x = h; cpu.d.x = OUT;
    for (unsigned i = 0; i < 54; ++i) wr8(DS, OUT + (uint16_t)i, 0xa5);
    if (ok("LFN file information by handle")) {
        CHECK(get32(DS, OUT) == A_ARCHIVE, "71A6 attrs at offset0");
        CHECK(get64(DS, OUT + 4) == filetime + UINT64_C(12500000), "71A6 creation FILETIME includes125 hundredths");
        CHECK(get64(DS, OUT + 12) == access_filetime, "71A6 access FILETIME uses requested date");
        CHECK(get64(DS, OUT + 20) == filetime, "71A6 modification FILETIME at offset20");
        CHECK(get32(DS, OUT + 32) == 0 && get32(DS, OUT + 36) == 5, "71A6 split size at offsets32/36");
        CHECK(get32(DS, OUT + 40) >= 1, "71A6 link count");
        CHECK(get32(DS, OUT + 44) || get32(DS, OUT + 48), "71A6 stable nonzero file identity");
        CHECK(rd8(DS, OUT + 52) == 0xa5 && rd8(DS, OUT + 53) == 0xa5, "71A6 writes exactly52 bytes");
    }
    Found entries[4];
    size_t n = find_entries(1, "clock.bin", 0, 0, entries, 4);
    CHECK(n == 1 && entries[0].mtime == filetime, "LFN SI0 FDR carries FILETIME");
    CHECK(n == 1 && entries[0].ctime == filetime + UINT64_C(12500000) && entries[0].atime == access_filetime,
          "LFN SI0 FDR creation and access timestamps occupy correct QWORD fields");
    n = find_entries(1, "clock.bin", 0, 1, entries, 4);
    CHECK(n == 1 && entries[0].mtime == ((uint32_t)date << 16 | time), "LFN SI1 FDR carries DOS words");
    n = find_entries(0, "clock.bin", 0, 0, entries, 4);
    CHECK(n == 1 && entries[0].mtime == ((uint32_t)date << 16 | time), "classic DTA time/date layout");
    close_file(h);
}

static void test_filetime_conversion(void)
{
    uint16_t date = packed_date(2024, 2, 29), time = packed_time(12, 34, 56);
    uint64_t base = ((uint64_t)timestamp(2024, 2, 29, 12, 34, 56) + UINT64_C(11644473600))
                    * UINT64_C(10000000);
    begin(0x71a7); cpu.b.l = 1; cpu.b.h = 123; cpu.c.x = time; cpu.d.x = date;
    if (ok("DOS date/time to FILETIME"))
        CHECK(get64(ES, OUT) == base + UINT64_C(12300000), "71A7 preserves hundredths past even second");
    put64(DS, DATA, base + UINT64_C(12300000));
    begin(0x71a7); cpu.b.l = 0; cpu.si = DATA;
    if (ok("FILETIME to DOS date/time"))
        CHECK(cpu.c.x == time && cpu.d.x == date && cpu.b.h == 123, "71A7 inverse returns CX/DX/BH");
    put64(DS, DATA, 0);
    begin(0x71a7); cpu.b.l = 0; cpu.si = DATA;
    if (ok("FILETIME before DOS epoch clamps"))
        CHECK(cpu.c.x == 0 && cpu.d.x == packed_date(1980, 1, 1), "FILETIME1601 becomes DOS1980");
    put64(DS, DATA, UINT64_MAX);
    begin(0x71a7); cpu.b.l = 0; cpu.si = DATA;
    if (ok("maximum unsigned FILETIME clamps safely"))
        CHECK(cpu.c.x == packed_time(23, 59, 58) && cpu.d.x == packed_date(2107, 12, 31),
              "UINT64_MAX FILETIME clamps without signed overflow");
    begin(0x71a7); cpu.b.l = 1; cpu.c.x = time; cpu.d.x = packed_date(2024, 0, 1);
    error(13, "invalid date for FILETIME conversion");
    begin(0x71a7); cpu.b.l = 2; error(1, "invalid FILETIME conversion selector");

    /* A fractional-hour offset distinguishes local DOS time from UTC FILETIME. */
    host_require(setenv("TZ", "UTC-5:30", 1) == 0, "set non-UTC test timezone");
    tzset();
    put64(DS, DATA, base);
    begin(0x71a7); cpu.b.l = 0; cpu.si = DATA;
    if (ok("FILETIME converts through local timezone"))
        CHECK(cpu.c.x == packed_time(18, 4, 56) && cpu.d.x == date,
              "UTC12:34:56 is local18:04:56 at UTC+05:30");
    begin(0x71a7); cpu.b.l = 1; cpu.c.x = packed_time(18, 4, 56); cpu.d.x = date;
    if (ok("local DOS timestamp converts back to UTC FILETIME"))
        CHECK(get64(ES, OUT) == base, "timezone-adjusted FILETIME round trip");
    uint16_t h = open_file("clock.bin", 0);
    begin(0x5700); cpu.b.x = h;
    if (ok("file mtime uses local timezone"))
        CHECK(cpu.c.x == packed_time(18, 4, 56) && cpu.d.x == date, "file packed mtime is local, not UTC");
    close_file(h);
    host_require(setenv("TZ", "UTC0", 1) == 0, "restore UTC test timezone");
    tzset();
}

static void test_creation_time_symlink_identity(void)
{
    host_file("creation-target.bin", "target", 0644);
    uint16_t date = packed_date(2022, 7, 14), time = packed_time(3, 4, 6);
    const uint16_t functions[] = {0x4100, 0x7141};
    char relative[64], path[PATH_MAX];
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; ++i) {
        snprintf(relative, sizeof relative, "creation-link-%zu.bin", i);
        host_path(path, sizeof path, relative);
        host_require(symlink("creation-target.bin", path) == 0, "create creation-time symlink fixture");
        path_begin(0x7143, "creation-target.bin");
        cpu.b.l = 7; cpu.c.x = time; cpu.di = date; cpu.si = 175;
        ok("set target creation override before unlinking symlink");
        path_begin(functions[i], relative); cpu.si = 0;
        ok("delete symlink rather than its target identity");
        struct stat st;
        CHECK(lstat(path, &st) < 0 && errno == ENOENT, "symlink itself was removed");
        CHECK(host_stat("creation-target.bin", &st) == 0 && st.st_size == 6,
              "symlink unlink preserves target file");
        path_begin(0x7143, "creation-target.bin"); cpu.b.l = 8;
        if (ok("get target creation time after unlinking symlink"))
            CHECK(cpu.c.x == time && cpu.di == date && cpu.si == 175,
                  "unlink of symlink preserves target's inode creation override");
    }
    uint16_t h = open_file("creation-target.bin", 0);
    path_begin(0x4100, "creation-target.bin"); ok("unlink last pathname while file handle stays open");
    begin(0x71a6); cpu.b.x = h; cpu.d.x = OUT;
    if (ok("file information from an unlinked live handle")) {
        uint64_t expected = ((uint64_t)timestamp(2022, 7, 14, 3, 4, 6) + UINT64_C(11644473600))
                            * UINT64_C(10000000) + UINT64_C(17500000);
        CHECK(get64(DS, OUT + 4) == expected, "birth override survives last unlink until final handle close");
    }
    close_file(h);
}

static void test_truename_and_shortname(void)
{
    char out[PATH_MAX], host[PATH_MAX], expected[PATH_MAX];
    const uint16_t calls[] = {0x6000, 0x7160, 0x7160, 0x7160};
    const unsigned modes[] = {0, 0, 1, 2};
    for (size_t i = 0; i < 4; ++i) {
        path_begin(calls[i], ".\\subdir\\..\\averyl~1.txt");
        cpu.c.x = (uint16_t)modes[i];
        if (!ok("truename mode")) continue;
        getstr(ES, OUT, out, sizeof out);
        CHECK(strncmp(out, "C:\\", 3) == 0, "truename yields absolute DOS path: '%s'", out);
        CHECK(strstr(out, "\\..\\") == NULL && strstr(out, "\\.\\") == NULL,
              "truename resolves dot components");
        const char *leaf = strrchr(out, '\\');
        leaf = leaf ? leaf + 1 : out;
        if (i == 0 || modes[i] == 1)
            CHECK(strcmp(leaf, "AVERYL~1.TXT") == 0, "short truename must expose correct 8.3 alias: '%s'", leaf);
        if (modes[i] == 2)
            CHECK(strcmp(leaf, "A very long name.txt") == 0, "long truename restores preserved host spelling");
        putstr(DS, ARG, out);
        host_path(expected, sizeof expected, "A very long name.txt");
        CHECK(dos_fs_to_host(DS, ARG, host, sizeof host) == 0 && strcmp(host, expected) == 0,
              "every truename representation resolves back to the same host path");
    }
    path_begin(0x7160, "plain.txt"); cpu.c.x = 0x8002;
    ok("truename high CH flag accepted with CL2");
    path_begin(0x7160, "plain.txt"); cpu.c.l = 3; error(1, "invalid truename mode");
    path_begin(0x7160, "B:\\plain.txt"); cpu.c.l = 2; error(15, "invalid truename drive");
    path_begin(0x71a8, "A very long name.txt"); cpu.d.h = 0; cpu.d.l = 0;
    for (unsigned i = 0; i < 13; ++i) wr8(ES, OUT + (uint16_t)i, 0xa5);
    if (ok("71A8 generates fixed-width 8+3 name")) {
        CHECK(memcmp(mem + lin(ES, OUT), "AVERYL~1TXT", 11) == 0, "DH0 short-name format is padded11 bytes");
        CHECK(rd8(ES, OUT + 11) == 0xa5, "DH0 format does not append NUL");
    }
    path_begin(0x71a8, "A very long name.txt"); cpu.d.h = 1; cpu.d.l = 0;
    if (ok("71A8 generates dotted ASCIIZ name")) {
        getstr(ES, OUT, out, sizeof out);
        CHECK(!strcmp(out, "AVERYL~1.TXT"), "DH1 short-name format contains dot and NUL");
    }
    path_begin(0x71a8, "README"); cpu.d.h = 0; cpu.d.l = 0;
    if (ok("71A8 pads extensionless short name"))
        CHECK(!memcmp(mem + lin(ES, OUT), "README     ", 11), "extensionless11-byte short-name padding");
}

static void test_free_space_and_country(void)
{
    begin(0x3600); cpu.d.l = 3;
    if (ok("classic free space on C")) {
        CHECK(cpu.a.x > 0 && cpu.c.x >= 128, "classic sectors-per-cluster and bytes-per-sector are nonzero");
        CHECK(cpu.a.x <= 64, "classic sectors-per-cluster stays a DOS value, never FFFFh (invalid drive)");
        CHECK(cpu.b.x <= cpu.d.x && cpu.d.x > 0, "classic free clusters <= total clusters");
    }
    begin(0x3600); cpu.d.l = 0; ok("classic free space on current drive");
    begin(0x3600); cpu.d.l = 1; sentinel(0xFFFF, 0xFFFF, "classic free space invalid drive returns AX=FFFFh");
    path_begin(0x7303, "C:\\"); cpu.c.x = 60;
    for (unsigned i = 0; i < 62; ++i) wr8(ES, OUT + (uint16_t)i, 0xa5);
    if (ok("extended free space")) {
        CHECK(rd16(ES, OUT) == 44 && rd16(ES, OUT + 2) == 0, "7303 layout length44 and version0");
        CHECK(get32(ES, OUT + 4) > 0 && get32(ES, OUT + 8) >= 128, "7303 cluster geometry");
        CHECK(get32(ES, OUT + 12) <= get32(ES, OUT + 16) && get32(ES, OUT + 16) > 0,
              "7303 cluster counts are sensible");
        CHECK(get32(ES, OUT + 20) <= get32(ES, OUT + 24), "7303 sector counts are sensible");
        CHECK(rd8(ES, OUT + 60) == 0xa5 && rd8(ES, OUT + 61) == 0xa5, "7303 respects caller capacity");
    }
    path_begin(0x7303, "C:\\"); cpu.c.x = 43; error(87, "extended free space rejects short output buffer");
    path_begin(0x7303, "D:\\"); cpu.c.x = 60; error(15, "extended free space invalid drive");
    path_begin(0x71a0, "C:\\"); cpu.c.x = 16;
    if (ok("LFN filesystem information")) {
        char name[20]; getstr(ES, OUT, name, sizeof name);
        CHECK(!strcmp(name, "LINUX"), "filesystem name is LINUX");
        CHECK((cpu.b.x & 0x4002) == 0x4002, "filesystem flags preserve case and advertise LFN bit14");
        CHECK(cpu.c.x == 255 && cpu.d.x == 260, "filesystem name/path limits255/260");
    }
    path_begin(0x71a0, "C:\\"); cpu.c.x = 0; ok("VC's zero-capacity filesystem query");
    path_begin(0x71a0, "C:\\"); cpu.c.x = 5; error(122, "filesystem name requires room for NUL");
    path_begin(0x71a0, "F:\\"); cpu.c.x = 16; error(15, "filesystem info invalid drive");

    begin(0x3800); cpu.d.x = OUT;
    for (unsigned i = 0; i < 34; ++i) wr8(DS, OUT + (uint16_t)i, 0xa5);
    if (ok("country information")) {
        CHECK(rd16(DS, OUT) == 1, "country date order is D.M.Y");
        CHECK(rd8(DS, OUT + 2) == '$' && rd8(DS, OUT + 3) == 0, "country currency is dollar");
        CHECK(rd8(DS, OUT + 7) == ',' && rd8(DS, OUT + 9) == '.', "country thousands and decimal separators");
        CHECK(rd8(DS, OUT + 11) == '.' && rd8(DS, OUT + 13) == ':', "country date and time separators");
        CHECK(rd8(DS, OUT + 17) == 1, "country uses24-hour time");
        CHECK(get32(DS, OUT + 18) == UINT32_C(0xf0000100), "country CaseMap points at F000:0100");
        CHECK(rd8(DS, OUT + 32) == 0xa5 && rd8(DS, OUT + 33) == 0xa5, "CNTRY writes exactly32 bytes");
    }
    for (unsigned value = 0; value < 256; ++value) {
        unsigned upper = value;
        if (value >= 0xa0 && value <= 0xaf) upper -= 0x20;
        else if (value >= 0xe0 && value <= 0xef) upper -= 0x50;
        else if (value == 0xf1 || value == 0xf3 || value == 0xf5 || value == 0xf7) --upper;
        begin(0x5a00); cpu.a.l = (uint8_t)value; cpu.b.x = 0x1234; cpu.cf = 1;
        Cpu expected_cpu = cpu; expected_cpu.a.l = (uint8_t)upper;
        dos_casemap_upper();
        CHECK(memcmp(&cpu, &expected_cpu, sizeof cpu) == 0,
              "country casemap byte%02x -> %02x and preserves all other registers/flags", value, upper);
    }
}

static void test_calendar(void)
{
    begin(0x2a00);
    if (ok("get current date")) {
        CHECK(cpu.c.x >= 1980 && cpu.c.x <= 2107 && cpu.d.h >= 1 && cpu.d.h <= 12 &&
              cpu.d.l >= 1 && cpu.d.l <= 31 && cpu.a.l <= 6,
              "current date fields are in DOS range");
    }
    begin(0x2c00);
    if (ok("get current time"))
        CHECK(cpu.c.h < 24 && cpu.c.l < 60 && cpu.d.h < 60 && cpu.d.l < 100,
              "current time fields include hundredths");
    begin(0x2b00); cpu.c.x = 2023; cpu.d.h = 2; cpu.d.l = 29;
    sentinel(0x00FF, 0x00FF, "set date rejects non-leap February29");
    begin(0x2b00); cpu.c.x = 1979; cpu.d.h = 12; cpu.d.l = 31;
    sentinel(0x00FF, 0x00FF, "set date rejects pre-DOS year");
    begin(0x2B01); cpu.c.x = 0x4445; cpu.d.x = 0x5351; /* 'DE' 'SQ' */
    sentinel(0x00FF, 0x00FF, "DESQview probe date is rejected, so VC sees no DESQview");
    begin(0x2d00); cpu.c.h = 24; cpu.c.l = 0; cpu.d.h = 0; cpu.d.l = 0;
    sentinel(0x00FF, 0x00FF, "set time rejects hour24");
    begin(0x2d00); cpu.c.h = 0; cpu.c.l = 0; cpu.d.h = 0; cpu.d.l = 100;
    sentinel(0x00FF, 0x00FF, "set time rejects100 hundredths");
    time_t before = time(NULL);
    begin(0x2b00); cpu.c.x = 2024; cpu.d.h = 2; cpu.d.l = 29;
    if (ok("set process-local DOS date")) CHECK(cpu.a.l == 0, "set date AL0");
    begin(0x2d00); cpu.c.h = 12; cpu.c.l = 34; cpu.d.h = 56; cpu.d.l = 0;
    if (ok("set process-local DOS time")) CHECK(cpu.a.l == 0, "set time AL0");
    begin(0x2a00);
    if (ok("get process-local DOS date"))
        CHECK(cpu.c.x == 2024 && cpu.d.h == 2 && cpu.d.l == 29 && cpu.a.l == 4,
              "virtual DOS date is Thursday2024-02-29");
    begin(0x2c00);
    if (ok("get process-local DOS time"))
        CHECK(cpu.c.h == 12 && cpu.c.l == 34 && cpu.d.h >= 56 && cpu.d.h <= 59,
              "virtual DOS time follows requested12:34:56");
    time_t after = time(NULL);
    CHECK(after >= before && after - before < 10, "DOS date/time setters do not change host system clock");
}

static void test_errors_bounds_and_dispatch(void)
{
    path_begin(0x3d00, "missing.txt"); error(2, "record extended last error");
    begin(0x5900); cpu.b.x = 0;
    if (ok("extended error information")) {
        CHECK(cpu.a.x == 2, "59 preserves exact preceding DOS error");
        CHECK(cpu.b.h != 0 && cpu.b.l != 0 && cpu.c.h != 0, "59 reports class, action and locus");
    }
    const uint16_t unknown[] = {0x2500, 0x4c00, 0x7304, 0xff00};
    uint8_t *saved_mem = malloc(sizeof mem);
    host_require(saved_mem != NULL, "snapshot DOS memory");
    for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; ++i) {
        memset(&cpu, 0x5a, sizeof cpu);
        cpu.a.x = unknown[i];
        Cpu saved_cpu = cpu;
        memcpy(saved_mem, mem, sizeof mem);
        int handled = dos_fs_int21();
        CHECK(handled == 0, "unknown INT21 %04x is not handled", unknown[i]);
        CHECK(memcmp(&cpu, &saved_cpu, sizeof cpu) == 0,
              "unknown INT21 %04x leaves entire CPU untouched", unknown[i]);
        CHECK(memcmp(mem, saved_mem, sizeof mem) == 0,
              "unknown INT21 %04x leaves RAM untouched", unknown[i]);
    }
    free(saved_mem);
    begin(0x71ff); error(0x7100, "unknown71xx requests classic fallback");

    /* Both strings and byte buffers wrap a 16-bit offset within their segment. */
    begin(0x3d00); cpu.d.x = 0xfffc; putstr(DS, 0xfffc, "plain.txt");
    if (ok("filename crossing FFFF offset boundary")) {
        uint16_t h = cpu.a.x;
        begin(0x3f00); cpu.b.x = h; cpu.c.x = 10; cpu.d.x = 0xfffc;
        if (ok("read buffer crossing FFFF offset boundary")) {
            const char *s = "plain-data";
            for (unsigned i = 0; i < 10; ++i)
                CHECK(rd8(DS, (uint16_t)(0xfffc + i)) == (uint8_t)s[i], "wrapped read byte%u", i);
        }
        close_file(h);
    }
    begin(0x3d00);
    /* No NUL exists anywhere in the input segment; the reader must terminate. */
    for (unsigned i = 0; i <= 0xffff; ++i) wr8(DS, (uint16_t)i, 'x');
    int handled = invoke();
    CHECK(handled == 1 && cpu.cf, "unterminated DOS pathname fails safely");
    memset(mem + lin(DS, 0), 0, 0x10000);
    path_begin(0x714e, "plain.txt"); cpu.si = 2;
    error(1, "unsupported LFN find time format");
    path_begin(0x7141, "plain.txt"); cpu.si = 2;
    error(1, "unsupported LFN unlink mode");
    path_begin(0x71a8, "plain.txt"); cpu.d.h = 2; cpu.d.l = 0;
    error(1, "unsupported short-name output format");
    path_begin(0x4300, "C:plain.txt");
    if (ok("drive-relative path after malformed request")) CHECK(cpu.c.x == A_ARCHIVE, "error does not damage filesystem state");
    Found list[128];
    size_t n = find_entries(1, "*", 0x1010, 1, list, 128);
    CHECK(n >= 4, "LFN required-directory mask finds directories and dots");
    for (size_t i = 0; i < n; ++i)
        CHECK(list[i].attrs & A_DIR, "LFN high-byte required attribute mask excludes files");
    CHECK(find_entries(1, "*", 0x0404, 1, list, 128) == 0,
          "LFN required-system mask finds nothing because system is never set");
}

static void test_function_coverage(void)
{
    const uint16_t required[] = {
        0x0e00,0x1900,0x1a00,0x2f00,0x2a00,0x2b00,0x2c00,0x2d00,
        0x3600,0x3800,0x3900,0x3a00,0x3b00,0x3c00,0x3d00,0x3e00,
        0x3f00,0x4000,0x4100,0x4200,0x4300,0x4301,
        0x4400,0x4401,0x4406,0x4407,0x4408,0x4409,0x440e,0x440f,
        0x4500,0x4600,0x4700,0x4e00,0x4f00,0x5600,
        0x5700,0x5701,0x5702,0x5703,0x5704,0x5705,0x5706,0x5707,
        0x5900,0x5a00,0x5b00,0x6000,0x6700,0x6800,0x6c00,
        0x7139,0x713a,0x713b,0x7141,0x7143,0x7147,0x714e,0x714f,
        0x7156,0x7160,0x716c,0x71a0,0x71a1,0x71a6,0x71a7,0x71a8,0x7303
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        size_t j;
        for (j = 0; j < called_count; ++j) if (called[j] == required[i]) break;
        CHECK(j < called_count, "advertised INT21 function%04x has a register-call test", required[i]);
    }
    printf("Function coverage: %zu advertised INT21 functions/subfunctions exercised.\n",
           sizeof required / sizeof required[0]);
}

static unsigned host_fd_count(void)
{
    unsigned total = 0;
    /* The runtime has64 handles; this bound includes standard/inherited FDs. */
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) ++total;
    return total;
}

static void test_reinitialization(void)
{
    dos_fs_init();
    unsigned before = host_fd_count();
    uint16_t h = extended_open("plain.txt", 0x12, 1, 0, "open denied-shared handle before reinit");
    begin(0x4500); cpu.b.x = h;
    uint16_t duplicate = ok("duplicate before reinit") ? cpu.a.x : 0xffff;
    path_begin(0x714e, "plain.txt"); cpu.si = 1;
    uint16_t find_handle = ok("LFN search before reinit") ? cpu.a.x : 0xffff;
    set_dta(DTA1);
    path_begin(0x4e00, "BASENA~?.TXT"); ok("classic search before reinit");
    CHECK(host_fd_count() > before, "open DOS handle owns a host descriptor");
    dos_fs_init();
    CHECK(host_fd_count() == before, "reinitialization releases all opened host descriptors");
    begin(0x3e00); cpu.b.x = h; error(6, "reinit invalidates original file handle");
    begin(0x3e00); cpu.b.x = duplicate; error(6, "reinit invalidates duplicated file handle");
    begin(0x714f); cpu.b.x = find_handle; cpu.si = 1; error(6, "reinit invalidates LFN search handle");
    set_dta(DTA1);
    begin(0x4f00); error(18, "reinit rejects stale classic DTA state");
    h = extended_open("plain.txt", 0x40, 1, 0, "reinit removes stale sharing reservations");
    CHECK(h == 5, "reinit restores the first allocatable handle");
    close_file(h);
}

static void check_current_drive(unsigned drive, const char *why)
{
    begin(0x1900);
    if (ok(why)) CHECK(cpu.a.l == drive, "%s: drive index %u, got %u", why, drive, cpu.a.l);
}

static void select_drive(unsigned drive)
{
    begin(0x0e00); cpu.d.l = (uint8_t)drive;
    if (ok("select valid drive")) CHECK(cpu.a.l == 8, "valid selection returns LASTDRIVE H");
    check_current_drive(drive, "current drive follows selection");
}

static void check_drive_cwd(int lfn, unsigned drive, const char *expected)
{
    char out[PATH_MAX];
    begin(lfn ? 0x7147 : 0x4700); cpu.d.l = (uint8_t)drive; cpu.si = OUT;
    if (!ok("get drive-specific current directory")) return;
    getstr(DS, OUT, out, sizeof out);
    CHECK(!strcmp(out, expected), "%s getcwd DL=%u: expected '%s', got '%s'",
          lfn ? "LFN" : "classic", drive, expected, out);
    CHECK(out[0] != '\\' && strchr(out, ':') == NULL,
          "47h/7147h preserve DOS's no-drive/no-leading-slash output contract");
}

static int check_host_resolution(const char *dos, const char *expected)
{
    char out[PATH_MAX] = "";
    putstr(DS, ARG, dos);
    int resolved = dos_fs_to_host(DS, ARG, out, sizeof out);
    int matches = resolved == 0 && !strcmp(out, expected);
    CHECK(matches, "HOME DRIVE: '%s' resolves to '%s', got result %d '%s'",
          dos, expected, resolved, out);
    return matches;
}

static void check_file_drive(uint16_t handle, unsigned drive)
{
    begin(0x4400); cpu.b.x = handle;
    if (ok("file-handle drive information"))
        CHECK(!(cpu.d.x & 0x80) && (cpu.d.x & 0x1f) == drive,
              "file handle %u retains opened drive %u, got DX=%04x", handle, drive, cpu.d.x);
}

static void test_home_drive_paths(void)
{
    char path[PATH_MAX], expected[PATH_MAX], out[PATH_MAX], dos_path[PATH_MAX + 3];
    host_file("home/x.bin", "home-data", 0644);
    host_path(path, sizeof path, "home/Work directory");
    host_require(mkdir(path, 0755) == 0, "mkdir H current-directory fixture");
    host_file("home/Work directory/x.bin", "home-subdir", 0644);
    host_file("subdir/cdrive.bin", "C-current-directory", 0644);
    host_path(path, sizeof path, "home/outside");
    host_require(symlink("..", path) == 0, "H symlink to outside its root");
    dos_fs_init();
    check_current_drive(2, "host cwd outside HOME starts on C");
    check_drive_cwd(0, 8, ""); check_drive_cwd(1, 8, "");
    check_host_resolution("C:\\", "/");
    const char *roots[] = {"H:\\", "h:/", "H:", "H:\\..", "H:\\..\\..\\..",
                           "H:/./../Work directory/../../.."};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i)
        check_host_resolution(roots[i], home_root);
    host_path(expected, sizeof expected, "home/x.bin");
    check_host_resolution("H:\\..\\..\\x.bin", expected);

    path_begin(0x3b00, "H:\\Work directory"); ok("classic chdir changes inactive H directory");
    check_current_drive(2, "classic H chdir does not select H");
    check_drive_cwd(0, 8, "WORKDI~1"); check_drive_cwd(1, 8, "Work directory");
    host_path(expected, sizeof expected, "home/Work directory/x.bin");
    check_host_resolution("H:x.bin", expected);
    host_path(path, sizeof path, "subdir");
    snprintf(dos_path, sizeof dos_path, "C:%s", path);
    path_begin(0x713b, dos_path); ok("LFN chdir changes C directory");
    select_drive(7);
    check_drive_cwd(0, 0, "WORKDI~1"); check_drive_cwd(1, 0, "Work directory");
    check_host_resolution("x.bin", expected);
    host_path(expected, sizeof expected, "subdir/cdrive.bin");
    check_host_resolution("C:cdrive.bin", expected);
    host_path(expected, sizeof expected, "home/x.bin");
    check_host_resolution("\\x.bin", expected);
    path_begin(0x713b, "C:.."); ok("LFN chdir changes inactive C directory");
    check_current_drive(7, "LFN C chdir does not select C");
    host_path(expected, sizeof expected, "plain.txt");
    check_host_resolution("C:plain.txt", expected);
    select_drive(2);
    check_drive_cwd(0, 8, "WORKDI~1"); check_drive_cwd(1, 8, "Work directory");
    select_drive(7);
    check_drive_cwd(1, 0, "Work directory");
    select_drive(2);

    const uint16_t chdirs[] = {0x3b00, 0x713b};
    for (size_t i = 0; i < sizeof chdirs / sizeof chdirs[0]; ++i) {
        path_begin(chdirs[i], "H:\\"); ok("change H to its drive root");
        path_begin(chdirs[i], "H:..\\..\\.."); ok("H parent at root remains at root");
        check_drive_cwd(0, 8, ""); check_drive_cwd(1, 8, "");
        check_current_drive(2, "H parent chdir leaves current drive unchanged");
        path_begin(chdirs[i], "H:Work directory"); ok("drive-relative H chdir");
        path_begin(chdirs[i], "H:no-such-directory"); error(3, "failed H chdir");
        check_drive_cwd(1, 8, "Work directory");
    }

    for (unsigned lfn = 0; lfn < 2; ++lfn) {
        Found list[8];
        size_t n = find_entries(lfn, "H:*.bin", 0, 1, list, 8);
        CHECK(n == 1 && list[0].size == strlen("home-subdir") &&
              !strcmp(list[0].name, lfn ? "x.bin" : "X.BIN"),
              "drive-relative find uses inactive H's saved directory");
        n = find_entries(lfn, "H:\\*.bin", 0, 1, list, 8);
        CHECK(n == 1 && list[0].size == strlen("home-data"),
              "absolute H find starts at HOME, not its saved directory");
        n = find_entries(lfn, "H:\\*", A_DIR, 1, list, 8);
        CHECK(!found_name(list, n, ".") && !found_name(list, n, ".."),
              "H root find omits dot entries, just as C root does");
        char c_pattern[PATH_MAX + 5];
        snprintf(c_pattern, sizeof c_pattern, "C:%s\\*", home_root);
        n = find_entries(lfn, c_pattern, A_DIR, 1, list, 8);
        CHECK(found_name(list, n, ".") && found_name(list, n, ".."),
              "C listing of HOME retains dot entries because HOME is not C's root");
    }

    const uint16_t true_calls[] = {0x6000, 0x7160, 0x7160, 0x7160};
    const unsigned modes[] = {0, 0, 1, 2};
    host_path(expected, sizeof expected, "home/Work directory/x.bin");
    snprintf(dos_path, sizeof dos_path, "C:%s", expected);
    const char *names[] = {"H:\\Work directory\\x.bin", dos_path};
    for (size_t n = 0; n < sizeof names / sizeof names[0]; ++n) {
        for (size_t i = 0; i < sizeof true_calls / sizeof true_calls[0]; ++i) {
            path_begin(true_calls[i], names[n]); cpu.c.x = (uint16_t)modes[i];
            if (!ok("truename chooses H for a host path under HOME")) continue;
            getstr(ES, OUT, out, sizeof out);
            CHECK(!strncmp(out, "H:\\", 3), "HOME DRIVE: canonical letter is H: '%s'", out);
            if (i == 0 || modes[i] == 1)
                CHECK(!strcmp(out, "H:\\WORKDI~1\\X.BIN"), "H short truename has drive-local aliases: '%s'", out);
            if (modes[i] == 2)
                CHECK(!strcmp(out, "H:\\Work directory\\x.bin"), "H long truename preserves spelling: '%s'", out);
            check_host_resolution(out, expected);
        }
    }
    host_path(path, sizeof path, "home/missing/leaf.dat");
    snprintf(dos_path, sizeof dos_path, "C:%s", path);
    for (unsigned lfn = 0; lfn < 2; ++lfn) {
        path_begin(lfn ? 0x7160 : 0x6000, dos_path); cpu.c.x = 0;
        if (ok("nonexistent-parent truename still chooses HOME's H letter")) {
            getstr(ES, OUT, out, sizeof out);
            CHECK(!strcmp(out, lfn ? "H:\\missing\\leaf.dat" : "H:\\MISSING\\LEAF.DAT"),
                  "lexical truename retains a nonexistent suffix below H: '%s'", out);
        }
    }

    /* C's current directory remains relative to C even when that host path
     * also has an H name. In particular C:.. may leave HOME; H:.. may not. */
    snprintf(dos_path, sizeof dos_path, "C:%s", home_root);
    path_begin(0x713b, dos_path); ok("C chdir to a host directory also named H root");
    snprintf(expected, sizeof expected, "%s", home_root + 1);
    for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
    check_drive_cwd(1, 3, expected);
    check_drive_cwd(1, 8, "Work directory");
    check_host_resolution("C:..", fixture);
    path_begin(0x7160, "C:.."); cpu.c.x = 2;
    if (ok("C parent leaves HOME before canonical drive is chosen")) {
        getstr(ES, OUT, out, sizeof out);
        CHECK(!strncmp(out, "C:\\", 3), "parent outside HOME keeps C letter: '%s'", out);
        check_host_resolution(out, fixture);
    }
    path_begin(0x7160, "C:x.bin"); cpu.c.x = 2;
    if (ok("drive-relative C truename below HOME")) {
        getstr(ES, OUT, out, sizeof out);
        CHECK(!strcmp(out, "H:\\x.bin"), "C input under HOME displays with H letter");
    }
    snprintf(dos_path, sizeof dos_path, "C:%s", fixture);
    path_begin(0x3b00, dos_path); ok("restore C fixture directory");

    /* Both spellings identify the same inode; 4400h describes the drive used
     * to open the handle, not the preferred letter used by truename. */
    host_path(path, sizeof path, "home/x.bin");
    snprintf(dos_path, sizeof dos_path, "C:%s", path);
    const char *same_file[] = {"H:\\x.bin", dos_path};
    struct stat st;
    host_require(stat(path, &st) == 0, "stat dual-drive file");
    for (unsigned lfn = 0; lfn < 2; ++lfn) {
        for (size_t i = 0; i < sizeof same_file / sizeof same_file[0]; ++i) {
            path_begin(lfn ? 0x716c : 0x3d00, same_file[i]);
            if (lfn) { cpu.b.x = 0x40; cpu.d.x = 1; }
            if (!ok("open the same file by C and H, classic and LFN")) continue;
            uint16_t h = cpu.a.x;
            unsigned drive = i ? 2 : 7;
            read_equals(h, "home-data", "dual-drive open reads the same payload");
            check_file_drive(h, drive);
            begin(0x71a6); cpu.b.x = h; cpu.d.x = OUT;
            if (ok("identity of dual-drive file handle"))
                CHECK(get32(DS, OUT + 28) == (uint32_t)st.st_dev &&
                      (((uint64_t)get32(DS, OUT + 44) << 32) | get32(DS, OUT + 48)) == (uint64_t)st.st_ino,
                      "C and H opens refer to exactly the fixture inode");
            begin(0x4500); cpu.b.x = h;
            if (ok("duplicate drive-tagged handle")) {
                uint16_t duplicate = cpu.a.x;
                check_file_drive(duplicate, drive); close_file(duplicate);
            }
            begin(0x4600); cpu.b.x = h; cpu.c.x = 60;
            if (ok("force-duplicate drive-tagged handle")) {
                check_file_drive(60, drive); close_file(60);
            }
            close_file(h);
        }
    }
    host_path(expected, sizeof expected, "home/outside/plain.txt");
    check_host_resolution("H:\\outside\\plain.txt", expected);
    uint16_t h = open_file("H:\\outside\\plain.txt", 0);
    if (h != 0xffff) { read_equals(h, "plain-data", "H may follow a filesystem symlink outside its root"); close_file(h); }
    host_path(expected, sizeof expected, "home/x.bin");
    check_host_resolution("H:\\outside\\..\\x.bin", expected);

    /* Keep the required H->/ planted-defect run read-only outside the private
     * fixture: do not create anything unless the H root is verified first. */
    if (check_host_resolution("H:\\", home_root)) {
        const char *temp_dirs[] = {"H:\\", "H:"};
        for (size_t i = 0; i < sizeof temp_dirs / sizeof temp_dirs[0]; ++i) {
            path_begin(0x5a00, temp_dirs[i]);
            if (!ok("create temporary file on absolute or drive-relative H")) continue;
            uint16_t temp = cpu.a.x;
            getstr(DS, ARG, out, sizeof out);
            CHECK(!strncmp(out, temp_dirs[i], strlen(temp_dirs[i])),
                  "temporary filename retains its H drive prefix");
            check_file_drive(temp, 7); close_file(temp);
            const char *leaf = out + strlen(temp_dirs[i]);
            char relative[PATH_MAX];
            int n = snprintf(relative, sizeof relative, "home/%s%s", i ? "Work directory/" : "", leaf);
            host_require(n >= 0 && (size_t)n < sizeof relative, "H temporary fixture path");
            host_path(expected, sizeof expected, relative);
            if (check_host_resolution(out, expected)) {
                CHECK(stat(expected, &st) == 0 && S_ISREG(st.st_mode), "H temporary file exists in its own drive directory");
                path_begin(0x4100, out); ok("delete private H temporary file");
            }
        }
    }
    host_require(getcwd(out, sizeof out) != NULL, "getcwd after home-drive calls");
    CHECK(!strcmp(out, fixture), "home-drive changes never change the host process cwd");
    dos_fs_init();
}

static void test_home_drive_cwd_rebases(void)
{
    /* Renames use explicit C paths bounded to our fixture; the guard also
     * makes the planted H-root defect skip all directory mutations here. */
    if (!check_host_resolution("H:\\", home_root)) return;
    char path[PATH_MAX], target[PATH_MAX], expected[PATH_MAX];
    char source_dos[PATH_MAX + 3], target_dos[PATH_MAX + 3];
    host_path(path, sizeof path, "home-parent-alias");
    host_require(symlink(".", path) == 0, "symlink to HOME's parent inside the fixture");
    struct stat home_before, st;
    host_require(stat(home_root, &home_before) == 0, "record protected HOME root identity");
    const uint16_t renames[] = {0x5600, 0x7156};
    for (size_t i = 0; i < sizeof renames / sizeof renames[0]; ++i) {
        host_path(path, sizeof path, "home-parent-alias/home");
        host_path(target, sizeof target, "home-root-renamed");
        snprintf(source_dos, sizeof source_dos, "C:%s", path);
        snprintf(target_dos, sizeof target_dos, "C:%s", target);
        rename_file(renames[i], source_dos, target_dos, 5);
        CHECK(lstat(home_root, &st) == 0 && st.st_dev == home_before.st_dev && st.st_ino == home_before.st_ino,
              "HOME root cannot be renamed through a symlinked parent");
        /* Recover the exact private inode if a regression allowed the move,
         * so following tests and fixture cleanup still operate on HOME. */
        if (lstat(target, &st) == 0 && st.st_dev == home_before.st_dev && st.st_ino == home_before.st_ino) {
            struct stat home_state;
            if (lstat(home_root, &home_state) < 0 && errno == ENOENT)
                host_require(rename(target, home_root) == 0, "restore an incorrectly renamed private HOME");
        }

        char relative[64];
        snprintf(relative, sizeof relative, "home-final-link-%zu", i);
        host_path(path, sizeof path, relative);
        host_require(symlink("home", path) == 0, "create final symlink to HOME");
        snprintf(relative, sizeof relative, "home-final-link-renamed-%zu", i);
        host_path(target, sizeof target, relative);
        snprintf(source_dos, sizeof source_dos, "C:%s", path);
        snprintf(target_dos, sizeof target_dos, "C:%s", target);
        rename_file(renames[i], source_dos, target_dos, 0);
        CHECK(lstat(path, &st) < 0 && errno == ENOENT, "renaming final HOME symlink removes the link's old name");
        CHECK(lstat(target, &st) == 0 && S_ISLNK(st.st_mode), "a final HOME symlink may itself be renamed");
        CHECK(stat(home_root, &st) == 0 && st.st_dev == home_before.st_dev && st.st_ino == home_before.st_ino,
              "renaming a final symlink never renames HOME itself");
    }
    dos_fs_init();
    host_path(path, sizeof path, "home/saved");
    host_require(mkdir(path, 0755) == 0, "mkdir inactive H cwd");
    path_begin(0x3b00, "H:\\saved"); ok("save inactive H cwd before C-side mutations");
    check_current_drive(2, "H cwd for mutation tests is inactive");
    snprintf(source_dos, sizeof source_dos, "C:%s", path);
    path_begin(0x3a00, source_dos); error(16, "classic rmdir protects another drive's cwd");
    path_begin(0x713a, source_dos); error(16, "LFN rmdir protects another drive's cwd");
    host_file("home/saved/marker.bin", "saved-directory", 0644);
    host_path(target, sizeof target, "home/renamed");
    snprintf(target_dos, sizeof target_dos, "C:%s", target);
    rename_file(0x7156, source_dos, target_dos, 0);
    check_drive_cwd(0, 8, "RENAMED"); check_drive_cwd(1, 8, "renamed");
    host_path(expected, sizeof expected, "home/renamed/marker.bin");
    check_host_resolution("H:marker.bin", expected);
    path_begin(0x713b, target_dos); ok("save C cwd at the same renamed directory");
    strcpy(source_dos, target_dos);
    host_path(target, sizeof target, "moved-outside-home");
    snprintf(target_dos, sizeof target_dos, "C:%s", target);
    rename_file(0x5600, source_dos, target_dos, 0);
    check_current_drive(2, "C-side rename leaves C selected");
    check_drive_cwd(0, 8, ""); check_drive_cwd(1, 8, "");
    host_path(expected, sizeof expected, "moved-outside-home/marker.bin");
    check_host_resolution("C:marker.bin", expected);
    host_path(expected, sizeof expected, "home/marker.bin");
    check_host_resolution("H:marker.bin", expected);
    check_host_resolution("H:..\\..", home_root);
    dos_fs_init();
}

static void test_home_drive_queries(void)
{
    select_drive(7);
    for (unsigned i = 0; i < 2; ++i) {
        begin(0x3600); cpu.d.l = i ? 8 : 0;
        if (ok("classic free space on H or current H")) {
            CHECK(cpu.a.x > 0 && cpu.a.x <= 64 && cpu.c.x == 512,
                  "H classic free-space geometry is representable by DOS");
            CHECK(cpu.b.x <= cpu.d.x && cpu.d.x > 0, "H free clusters do not exceed total");
        }
    }
    path_begin(0x7303, "H:\\"); cpu.c.x = 44;
    if (ok("extended free space on H")) {
        struct statvfs fs;
        host_require(statvfs(home_root, &fs) == 0, "statvfs HOME fixture");
        uint64_t blocks = fs.f_blocks;
        CHECK(rd16(ES, OUT) == 44 && get32(ES, OUT + 8) == 512,
              "H extended free space has the documented layout");
        CHECK(get32(ES, OUT + 32) == (blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks),
              "H extended free space is measured on HOME's filesystem");
    }
    path_begin(0x71a0, "H:\\"); cpu.c.x = 16;
    if (ok("LFN volume info on H")) {
        char out[20]; getstr(ES, OUT, out, sizeof out);
        CHECK(!strcmp(out, "LINUX") && (cpu.b.x & 0x4002) == 0x4002,
              "H volume exposes Linux long-name support");
    }
    const uint16_t ioctls[] = {0x4408, 0x4409, 0x440e, 0x440f};
    for (size_t i = 0; i < sizeof ioctls / sizeof ioctls[0]; ++i) {
        for (unsigned current = 0; current < 2; ++current) {
            begin(ioctls[i]); cpu.b.l = current ? 0 : 8;
            if (!ok("IOCTL on drive 8 and current H")) continue;
            if (ioctls[i] == 0x4408) CHECK(cpu.a.x == 1, "H is a fixed disk");
            else if (ioctls[i] == 0x4409) CHECK(!(cpu.d.x & 0x9000), "H is local and not SUBST");
            else CHECK(cpu.a.l == 0, "H has a single logical mapping");
        }
    }
    const unsigned bad_drives[] = {3, 25}; /* D and Z, zero based. */
    for (size_t b = 0; b < sizeof bad_drives / sizeof bad_drives[0]; ++b) {
        unsigned drive = bad_drives[b];
        char name[] = "D:\\", out[PATH_MAX];
        name[0] = (char)('A' + drive);
        begin(0x0e00); cpu.d.l = (uint8_t)drive; error(15, "D/Z selection is invalid");
        check_current_drive(7, "invalid selection preserves H");
        for (size_t i = 0; i < sizeof ioctls / sizeof ioctls[0]; ++i) {
            begin(ioctls[i]); cpu.b.l = (uint8_t)(drive + 1); error(15, "D/Z IOCTL is invalid");
        }
        const uint16_t getcwds[] = {0x4700, 0x7147};
        const uint16_t path_calls[] = {0x3b00, 0x713b, 0x6000, 0x7160, 0x71a0, 0x7303};
        for (size_t i = 0; i < sizeof getcwds / sizeof getcwds[0]; ++i) {
            begin(getcwds[i]); cpu.d.l = (uint8_t)(drive + 1); cpu.si = OUT;
            error(15, "D/Z getcwd is invalid");
        }
        for (size_t i = 0; i < sizeof path_calls / sizeof path_calls[0]; ++i) {
            path_begin(path_calls[i], name); cpu.c.x = path_calls[i] == 0x7160 ? 2 : 60;
            error(15, "D/Z pathname-based drive query is invalid");
        }
        begin(0x3600); cpu.d.l = (uint8_t)(drive + 1);
        sentinel(0xffff, 0xffff, "D/Z classic free space uses AX=FFFFh");
        putstr(DS, ARG, name);
        CHECK(dos_fs_to_host(DS, ARG, out, sizeof out) < 0 && errno == ENODEV,
              "D/Z public resolution reports ENODEV");
    }
    select_drive(2);
}

static void test_home_drive_initialization(void)
{
    char path[PATH_MAX], expected[PATH_MAX], out[PATH_MAX], link_home[PATH_MAX];
    host_require(chdir(home_root) == 0, "enter HOME root before init");
    dos_fs_init();
    check_current_drive(7, "host cwd at HOME root starts on H");
    check_drive_cwd(0, 0, ""); check_drive_cwd(1, 0, "");
    check_drive_cwd(0, 3, ""); check_drive_cwd(1, 3, "");
    check_host_resolution("\\", home_root);
    host_path(path, sizeof path, "home/Work directory");
    host_require(chdir(path) == 0, "enter HOME descendant before init");
    dos_fs_init();
    check_current_drive(7, "host cwd below HOME starts on H");
    check_drive_cwd(0, 8, "WORKDI~1"); check_drive_cwd(1, 8, "Work directory");
    check_drive_cwd(1, 3, "");

    host_path(path, sizeof path, "home-sibling");
    host_require(mkdir(path, 0755) == 0, "mkdir HOME-prefix sibling");
    host_require(chdir(path) == 0, "enter HOME-prefix sibling before init");
    dos_fs_init();
    check_current_drive(2, "HOME prefix without path boundary is outside H");
    snprintf(expected, sizeof expected, "%s", path + 1);
    for (char *p = expected; *p; ++p) if (*p == '/') *p = '\\';
    check_drive_cwd(1, 3, expected); check_drive_cwd(1, 8, "");
    path_begin(0x7160, "."); cpu.c.x = 2;
    if (ok("truename for a HOME-prefix sibling")) {
        getstr(ES, OUT, out, sizeof out);
        CHECK(!strncmp(out, "C:\\", 3), "prefix sibling canonical name stays C: '%s'", out);
        check_host_resolution(out, path);
    }
    host_require(chdir(fixture) == 0, "restore host fixture for HOME configuration checks");
    host_path(link_home, sizeof link_home, "home-link");
    host_require(symlink("home", link_home) == 0, "symlink HOME configuration");
    host_require(setenv("HOME", link_home, 1) == 0, "set HOME through symlink");
    host_require(chdir(home_root) == 0, "enter canonical HOME for symlink init");
    dos_fs_init();
    check_current_drive(7, "HOME is canonicalized before start-drive classification");
    check_host_resolution("H:\\", home_root);
    check_drive_cwd(1, 0, "");
    host_require(chdir(fixture) == 0, "restore fixture before invalid HOME cases");

    char missing[PATH_MAX], not_directory[PATH_MAX], root_link[PATH_MAX];
    host_path(missing, sizeof missing, "missing-home");
    host_path(not_directory, sizeof not_directory, "plain.txt");
    host_path(root_link, sizeof root_link, "root-home-link");
    host_require(symlink("/", root_link) == 0, "symlink HOME to filesystem root");
    const char *invalid[] = {NULL, "", "home", "/", missing, not_directory, root_link};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        if (invalid[i]) host_require(setenv("HOME", invalid[i], 1) == 0, "set invalid HOME fixture");
        else host_require(unsetenv("HOME") == 0, "unset HOME fixture");
        dos_fs_init();
        check_current_drive(2, "invalid HOME starts on C");
        begin(0x0e00); cpu.d.l = 7; error(15, "invalid HOME disables H selection");
        check_current_drive(2, "failed H selection leaves C selected");
        putstr(DS, ARG, "H:\\");
        CHECK(dos_fs_to_host(DS, ARG, out, sizeof out) < 0 && errno == ENODEV,
              "invalid HOME disables public H resolution (case %zu)", i);
        begin(0x4700); cpu.d.l = 8; cpu.si = OUT; error(15, "invalid HOME disables classic H getcwd");
        begin(0x7147); cpu.d.l = 8; cpu.si = OUT; error(15, "invalid HOME disables LFN H getcwd");
        begin(0x3600); cpu.d.l = 8; sentinel(0xffff, 0xffff, "invalid HOME disables H free space");
        path_begin(0x7303, "H:\\"); cpu.c.x = 44; error(15, "invalid HOME disables extended H free space");
        path_begin(0x71a0, "H:\\"); cpu.c.x = 16; error(15, "invalid HOME disables H volume info");
        begin(0x4408); cpu.b.l = 8; error(15, "invalid HOME disables H IOCTL");
        check_host_resolution("C:\\", "/");
    }
    char trailing_home[PATH_MAX + 3];
    snprintf(trailing_home, sizeof trailing_home, "%s/", home_root);
    host_require(setenv("HOME", trailing_home, 1) == 0, "set HOME with trailing separator");
    dos_fs_init();
    check_host_resolution("H:\\", home_root);
    host_require(setenv("HOME", home_root, 1) == 0, "restore valid HOME fixture");
    dos_fs_init();
    check_current_drive(2, "restored host fixture starts on C");
    check_drive_cwd(1, 8, "");
}

static int remove_fixture_entry(const char *path, const struct stat *st, int type, struct FTW *walk)
{
    (void)st; (void)walk;
    return type == FTW_DP || type == FTW_D ? rmdir(path) : unlink(path);
}

static void setup_fixture(void)
{
    host_require(getcwd(original_cwd, sizeof original_cwd) != NULL, "save original cwd");
    strcpy(fixture, "/tmp/vc-dos-fs-test-XXXXXX");
    host_require(mkdtemp(fixture) != NULL, "mkdtemp");
    host_file("A very long name.txt", "long payload", 0644);
    host_file("Base name beta.txt", "beta alias", 0644); /* Creation order is intentionally reversed. */
    host_file("Base name alpha.txt", "alpha alias", 0644);
    host_file("basena~1.txt", "reserved", 0644); /* Must reserve before sorted long names. */
    host_file("plain.txt", "plain-data", 0644);
    host_file("README", "no extension", 0644);
    host_file("\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82.txt", "Cyrillic", 0644);
    host_file("face-\xf0\x9f\x98\x80.txt", "emoji", 0644);
    host_file(".secret", "hidden", 0644);
    host_file("readonly.bin", "protected", 0444);
    char path[PATH_MAX];
    host_path(path, sizeof path, "subdir"); host_require(mkdir(path, 0755) == 0, "mkdir subdir fixture");
    host_path(path, sizeof path, "Long directory"); host_require(mkdir(path, 0755) == 0, "mkdir long fixture");
    host_file("Long directory/Mixed name.dat", "nested", 0644);
    host_path(path, sizeof path, "link.txt"); host_require(symlink("plain.txt", path) == 0, "file symlink fixture");
    host_path(path, sizeof path, "dirlink"); host_require(symlink("subdir", path) == 0, "directory symlink fixture");
    host_path(path, sizeof path, "dangling"); host_require(symlink("no-such-target", path) == 0, "dangling symlink fixture");
    host_path(home_root, sizeof home_root, "home");
    host_require(mkdir(home_root, 0755) == 0, "mkdir HOME inside fixture");
    host_require(setenv("HOME", home_root, 1) == 0, "set deterministic fixture HOME before init");
    host_require(chdir(fixture) == 0, "enter fixture before dos_fs_init");
    host_require(setenv("TZ", "UTC0", 1) == 0, "set deterministic test timezone");
    tzset();
    dos_fs_init();
}

int main(void)
{
    setup_fixture();
    test_names_and_finds();
    test_alias_collision_scale();
    test_dta_state();
    test_io_and_handles();
    test_paths_and_mutations();
    test_converted_name_collisions();
    test_lossy_hash_collision();
    test_literal_wildcard_names();
    test_name_boundaries();
    test_unlink_directory_symlinks();
    test_rmdir_symlinks();
    test_execute_only_directory();
    test_forbidden_characters();
    test_trailing_space_and_dot_names();
    test_ancestor_symlink_lists_as_file();
    test_extended_open();
    test_share_modes();
    test_cross_device_rename();
    test_devices_and_ioctl();
    test_attributes_and_times();
    test_filetime_conversion();
    test_creation_time_symlink_identity();
    test_truename_and_shortname();
    test_free_space_and_country();
    test_calendar();
    test_errors_bounds_and_dispatch();
    test_empty_file_specs();
    test_reinitialization();
    test_home_drive_paths();
    test_home_drive_cwd_rebases();
    test_home_drive_queries();
    test_home_drive_initialization();
    test_function_coverage();
    /* Reinitialization also exercises releasing live classic-search state. */
    dos_fs_init();
    host_require(chdir(original_cwd) == 0, "restore test process cwd");
    host_require(nftw(fixture, remove_fixture_entry, 32, FTW_DEPTH | FTW_PHYS) == 0, "remove private fixture tree");
    printf("DOS filesystem tests: %u checks, %u failures.\n", checks, failures);
    return failures ? 1 : 0;
}
