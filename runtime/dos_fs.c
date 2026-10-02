#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "cpu.h"
#include "hle.h"
#include "dos_fs.h"
#include "cp866.h"
#include "guest_mem.h"
#ifdef __EMSCRIPTEN__
#include "web_files.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifndef __EMSCRIPTEN__
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>

/* A DOS path is held separately from the process cwd. All host operations
 * receive absolute paths; in particular, this module never calls chdir(). */
#define DOS_HANDLES DOS_MAX_HANDLES
#define SEARCH_SLOTS 128
#define DOS_PATH_MAX 4096
#define DOS_NAME_MAX 255
#define LFN_PATH_MAX 260
#define DOS_DRIVES 8
#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20

enum { DEV_NONE, DEV_IN, DEV_OUT, DEV_AUX, DEV_PRN, DEV_CON, DEV_NUL };
enum { DRIVE_C = 2, DRIVE_H = 7 };
typedef struct {
    /* Empty root means absent. Both paths are absolute host spellings; cwd
     * must remain at or below root, even after a rename through C:. */
    char root[PATH_MAX], cwd[PATH_MAX];
} DosDrive;

typedef struct {
    int fd, device, access;
    unsigned mode, drive;
    unsigned references; /* System file, shared by inherited PSP job tables. */
    uint32_t fcb_id; /* nonzero only for an FCB open; guards copied/stale FCBs */
    uint16_t fcb_owner;
    char path[PATH_MAX];
} DosHandle;

typedef struct {
    char *host;
    char dos[DOS_NAME_MAX + 1];
    char alias[13];
    struct stat st;
    struct timespec birth;
    unsigned attr;
    bool have_stat, lossless, ambiguous;
} Entry;

typedef struct {
    Entry *entries;
    size_t count, next;
    uint32_t generation;
    uint16_t handle, mask, owner;
    unsigned drive;
    bool active, lfn, fcb;
    char pattern[DOS_NAME_MAX + 1];
} Search;

/* Linux birth times cannot be changed with utimensat. DOS creation-time
 * writes are therefore remembered by inode for this process's lifetime.
 * No sidecar files or extended attributes are added to the user's tree. */
typedef struct BirthTime {
    dev_t dev;
    ino_t ino;
    struct timespec time;
    struct BirthTime *next;
} BirthTime;

/* A lease is deliberately sparse: just the components of one selected
 * canonical path, not a cache of the root drive's short-name assignments.
 * Component offsets refer to the two immutable absolute spellings below. */
typedef struct {
    uint16_t parent_end, host_start, host_end, dos_start;
    uint8_t dos_length;
    dev_t dev, entry_dev;
    ino_t ino, entry_ino;
    mode_t type, entry_type;
} LeasePart;

struct DosPathLease {
    struct DosPathLease *next;
    int fd; /* Hold the selected inode until release or an owner rename. */
    uint16_t psp;
    bool bound;
    bool recreate; /* Owner moved the leaf; retain only its pathname reservation. */
#ifndef __EMSCRIPTEN__
    bool guard_directory; /* Native playground: also validate relative-name opens. */
#endif
    unsigned count;
    char dos[128], host[PATH_MAX];
    LeasePart parts[64];
};

static DosDrive drives[DOS_DRIVES] = {[DRIVE_C] = {"/", "/"}};
static unsigned current_drive = DRIVE_C;
static DosHandle handles[DOS_HANDLES];
static Search searches[SEARCH_SLOTS];
static BirthTime *birth_times;
static DosPathLease *path_leases;
static uint16_t dta_seg, dta_off = 0x80;
static uint16_t last_error;
static uint32_t search_generation;
static unsigned temp_sequence;
static uint32_t fcb_sequence;
static uint16_t fcb_process;
static uint16_t closed_file_owner;
static char closed_file_path[PATH_MAX];
static struct timespec clock_delta;
static bool initialized;
static int door_root_fd = -1;
static uint64_t door_quota, door_bytes, door_page = 4096;
static unsigned door_entries;
static char pipe_directory[PATH_MAX];
static bool pipe_cleanup_registered;

static int disk_space(bool extended);
static int filesystem_info(void);
static int country_info(void);
static int handle_info(void);
static int convert_filetime(void);
static int generate_shortname(void);
static void forget_birth(const struct stat *st);
static int quota_truncate(int fd, off_t size);
static void quota_release_entry(const struct stat *st);
static bool inode_is_open(const struct stat *st);
static int rename_paths(const char *old, char *target, const char *dos);

static void cleanup_command_pipes(void)
{
    if (!*pipe_directory) return;
    /* Only these exact runtime-owned names may be removed. The 0700
     * mkdtemp directory is neither HOME nor a guest-supplied path. */
    for (unsigned drive = 0; drive < DOS_DRIVES; drive++)
        for (unsigned number = 1; number <= 2; number++) {
            char file[PATH_MAX];
            int n = snprintf(file, sizeof file, "%s/%c-PIPE%u", pipe_directory, 'A' + drive, number);
            if (n > 0 && (size_t)n < sizeof file) (void)unlink(file);
        }
    (void)rmdir(pipe_directory);
    pipe_directory[0] = 0;
}

/* COMMAND 2 writes these two fixed names at the drive root. Translate only
 * those exact root names; ordinary files in subdirectories stay ordinary.
 * A door already has a private root, and must never open anything in /tmp. */
static int command_pipe_path(const char *name, unsigned drive, bool missing, char out[PATH_MAX])
{
    if (door_root_fd >= 0) return 0;
    bool rooted = *name == '\\' || *name == '/';
    if (!rooted && strcmp(drives[drive].cwd, drives[drive].root)) return 0;
    while (*name == '\\' || *name == '/') name++;
    unsigned number = !strcasecmp(name, "%PIPE1.$$$") ? 1 :
                      !strcasecmp(name, "%PIPE2.$$$") ? 2 : 0;
    if (!number) return 0;
    if (!*pipe_directory) {
        if (!missing) return -2;
        char directory[] = "/tmp/vc-dos-pipe-XXXXXX";
        if (!mkdtemp(directory)) return -5;
        strcpy(pipe_directory, directory);
        if (!pipe_cleanup_registered) {
            if (atexit(cleanup_command_pipes)) { cleanup_command_pipes(); return -8; }
            pipe_cleanup_registered = true;
        }
    }
    int n = snprintf(out, PATH_MAX, "%s/%c-PIPE%u", pipe_directory, 'A' + drive, number);
    return n > 0 && n < PATH_MAX ? 1 : -3;
}

static bool drive_present(unsigned drive)
{
    return drive < DOS_DRIVES && *drives[drive].root;
}

/* Query functions number drives from one, with zero meaning current. The
 * select/current-drive functions instead use zero-based drive numbers. */
static DosDrive *query_drive(unsigned number)
{
    unsigned drive = number ? number - 1 : current_drive;
    return drive_present(drive) ? drives + drive : NULL;
}

static bool path_below(const char *path, const char *root)
{
    size_t n = strlen(root);
    return n && (!strcmp(root, "/") ||
                 (!strncmp(path, root, n) && (!path[n] || path[n] == '/')));
}

/* Door operations use directory descriptors, not a checked path followed by
 * an ordinary host open. Every parent is opened beneath the held H: root with
 * O_NOFOLLOW, so replacing a component with a symlink cannot escape H: between
 * resolution and a mutation. Guest-created entries are never symlinks. */
static int door_parent(const char *path, char leaf[NAME_MAX + 1])
{
    const char *root = drives[DRIVE_H].root;
    if (!path_below(path, root)) { errno = EACCES; return -1; }
    char relative[PATH_MAX];
    size_t used = 0;
    const char *p = path + strlen(root);
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - start);
        if (n == 1 && start[0] == '.') continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (used && relative[used - 1] != '/') --used;
            if (used) --used;
            continue;
        }
        if (n > NAME_MAX || used + n + 1 >= sizeof(relative)) { errno = ENAMETOOLONG; return -1; }
        if (used) relative[used++] = '/';
        memcpy(relative + used, start, n);
        used += n;
    }
    relative[used] = 0;
    int fd = openat(door_root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = relative, *slash;
    while ((slash = strchr(part, '/'))) {
        *slash = 0;
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        if (next < 0) { errno = saved == ELOOP ? EACCES : saved; return -1; }
        fd = next;
        part = slash + 1;
    }
    strcpy(leaf, *part ? part : ".");
    return fd;
}

static bool door_file_type(const struct stat *st)
{
    /* A hardlink could name an inode outside H: even though its path is below
     * the root. DOS cannot create either kind of link; refuse them both. */
    return S_ISDIR(st->st_mode) || (S_ISREG(st->st_mode) && st->st_nlink <= 1);
}

static int fs_stat(const char *path, struct stat *st, bool nofollow)
{
    if (door_root_fd < 0) return nofollow ? lstat(path, st) : stat(path, st);
    char leaf[NAME_MAX + 1];
    int parent = door_parent(path, leaf);
    if (parent < 0) return -1;
    int result = fstatat(parent, leaf, st, AT_SYMLINK_NOFOLLOW);
    int saved = errno;
    close(parent);
    if (!result && !door_file_type(st)) { result = -1; saved = EACCES; }
    errno = saved;
    return result;
}

static int fs_open(const char *path, int flags, mode_t mode)
{
#ifdef __EMSCRIPTEN__
    if (door_root_fd < 0) return web_files_open(path, flags, mode);
#endif
    if (door_root_fd < 0) return open(path, flags, mode);
    char leaf[NAME_MAX + 1];
    int parent = door_parent(path, leaf);
    if (parent < 0) return -1;
    struct stat st;
    int probe = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    if ((!probe && !door_file_type(&st)) || (probe && errno != ENOENT)) {
        int saved = !probe ? EACCES : errno;
        close(parent); errno = saved; return -1;
    }
    /* Defer truncation until the opened inode has passed the type check too. */
    int open_flags = (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK;
    bool creating = (flags & O_CREAT) && probe;
    if (flags & O_CREAT) {
        if (!probe && (flags & O_EXCL)) {
            close(parent); errno = EEXIST; return -1;
        }
        if (creating) {
            if (door_entries >= DOS_DOOR_ENTRY_LIMIT) {
                close(parent); errno = EDQUOT; return -1;
            }
            /* Only an atomic new allocation may charge a new entry. */
            open_flags |= O_EXCL;
        } else {
            /* If an external actor removes an existing file after the probe,
             * do not recreate an unaccounted inode through the same open. */
            open_flags &= ~O_CREAT;
        }
    }
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY)
        open_flags = (open_flags & ~O_ACCMODE) | O_RDWR;
    int fd = openat(parent, leaf, open_flags, mode);
    int saved = errno;
    close(parent);
    if (fd < 0) { errno = saved == ELOOP ? EACCES : saved; return -1; }
    if (creating) ++door_entries;
    int checked = fstat(fd, &st);
    if (checked || !door_file_type(&st) ||
        ((flags & O_TRUNC) && quota_truncate(fd, 0))) {
        saved = !checked && !door_file_type(&st) ? EACCES : errno;
        close(fd); errno = saved; return -1;
    }
    return fd;
}

static DIR *fs_opendir(const char *path)
{
    if (door_root_fd < 0) return opendir(path);
    int fd = fs_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0) return NULL;
    DIR *dp = fdopendir(fd);
    if (!dp) { int saved = errno; close(fd); errno = saved; }
    return dp;
}

int dos_fs_open_readonly(const char *host)
{
    if (!initialized) dos_fs_init();
    if (!host || *host != '/') { errno = EACCES; return -1; }
    return fs_open(host, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
}

static int fs_mkdir(const char *path, mode_t mode)
{
    if (door_root_fd < 0) return mkdir(path, mode);
    char leaf[NAME_MAX + 1];
    int parent = door_parent(path, leaf);
    if (parent < 0) return -1;
    if (door_entries >= DOS_DOOR_ENTRY_LIMIT) {
        struct stat st;
        int probe = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
        int saved = !probe ? EEXIST : errno == ENOENT ? EDQUOT : errno;
        close(parent); errno = saved; return -1;
    }
    int result = mkdirat(parent, leaf, mode), saved = errno;
    if (!result) ++door_entries;
    close(parent); errno = saved;
    return result;
}

static int fs_unlink(const char *path, bool directory)
{
    if (door_root_fd < 0) return directory ? rmdir(path) : unlink(path);
    char leaf[NAME_MAX + 1];
    int parent = door_parent(path, leaf);
    if (parent < 0) return -1;
    struct stat st;
    if (fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW)) {
        int saved = errno; close(parent); errno = saved; return -1;
    }
    int result = unlinkat(parent, leaf, directory ? AT_REMOVEDIR : 0), saved = errno;
    /* Removed-but-open inodes still occupy memory and an inode on tmpfs.
     * Their last tracked descriptor release owns the eventual reclamation. */
    if (!result && !inode_is_open(&st)) quota_release_entry(&st);
    close(parent); errno = saved;
    return result;
}

static int fs_chmod(const char *path, mode_t mode)
{
    if (door_root_fd < 0) return chmod(path, mode);
    int fd = fs_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return -1;
    int result = fchmod(fd, mode), saved = errno;
    close(fd); errno = saved;
    return result;
}

static int fs_utimens(const char *path, const struct timespec times[2])
{
    if (door_root_fd < 0) return utimensat(AT_FDCWD, path, times, 0);
    int fd = fs_open(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return -1;
    int result = futimens(fd, times), saved = errno;
    close(fd); errno = saved;
    return result;
}

#ifndef __EMSCRIPTEN__
static int fs_rename(const char *old, const char *target)
{
    int old_parent = AT_FDCWD, target_parent = AT_FDCWD;
    char old_leaf[NAME_MAX + 1], target_leaf[NAME_MAX + 1];
    if (door_root_fd >= 0) {
        old_parent = door_parent(old, old_leaf);
        if (old_parent < 0) return -1;
        target_parent = door_parent(target, target_leaf);
        if (target_parent < 0) { int saved = errno; close(old_parent); errno = saved; return -1; }
        old = old_leaf; target = target_leaf;
    }
    /* musl exposes the Linux syscall but not glibc's renameat2 wrapper. */
    int result = (int)syscall(SYS_renameat2, old_parent, old, target_parent, target, 1u);
    int saved = errno; /* Linux RENAME_NOREPLACE = 1. */
    if (old_parent != AT_FDCWD) close(old_parent);
    if (target_parent != AT_FDCWD) close(target_parent);
    errno = saved;
    return result;
}
#endif

/* The private tree is populated before initialization. DOS is its only writer
 * afterwards, so accounting changes at the actual write/truncate/unlink boundary
 * is both exact and independent of the number of seeded files.
 * The door's H: is tmpfs, which gives every non-empty file at least one whole
 * page. Charge a file its logical size rounded up to whole pages, so the
 * quota bounds the memory really used. Directories take no tmpfs pages. */
static uint64_t quota_charge(off_t size)
{
    if (size <= 0) return 0;
    return ((uint64_t)size + door_page - 1) / door_page * door_page;
}

static void quota_release_entry(const struct stat *st)
{
    if (S_ISREG(st->st_mode) && st->st_nlink <= 1)
        door_bytes -= quota_charge(st->st_size);
    --door_entries;
}

static int quota_prepare(int fd, uint64_t end, uint64_t *previous)
{
    if (door_root_fd < 0) { *previous = 0; return 0; }
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if (!S_ISREG(st.st_mode) || !door_file_type(&st)) { errno = EACCES; return -1; }
    *previous = quota_charge(st.st_size);
    uint64_t charge = end > INT64_MAX ? UINT64_MAX : quota_charge((off_t)end);
    if (charge > *previous && (door_bytes > door_quota || charge - *previous > door_quota - door_bytes)) {
        errno = EDQUOT;
        return -1;
    }
    return 0;
}

static void quota_changed(int fd, uint64_t previous)
{
    if (door_root_fd < 0) return;
    struct stat st;
    if (fstat(fd, &st)) return;
    door_bytes = door_bytes - previous + quota_charge(st.st_size);
}

static int quota_truncate(int fd, off_t size)
{
    uint64_t previous;
    if (size < 0) { errno = EINVAL; return -1; }
    if (quota_prepare(fd, (uint64_t)size, &previous)) return -1;
    int result = ftruncate(fd, size);
    if (!result) quota_changed(fd, previous);
    return result;
}

static ssize_t quota_write(int fd, const void *bytes, size_t count, off_t position, bool positioned)
{
    uint64_t previous = 0;
    if (door_root_fd >= 0) {
        if (!positioned) position = lseek(fd, 0, SEEK_CUR);
        if (position < 0 || quota_prepare(fd, (uint64_t)position + count, &previous)) return -1;
    }
    ssize_t result = positioned ? pwrite(fd, bytes, count, position) : write(fd, bytes, count);
    if (result >= 0) quota_changed(fd, previous);
    return result;
}

static int quota_scan(int fd, unsigned depth)
{
    if (depth > 128) return 3;
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copy < 0) return 5;
    DIR *dp = fdopendir(copy);
    if (!dp) { close(copy); return 5; }
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) { if (errno) error = 5; break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (door_entries >= DOS_DOOR_ENTRY_LIMIT) { error = 39; break; }
        ++door_entries;
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) { error = 5; break; }
        if (S_ISREG(st.st_mode)) {
            uint64_t size = quota_charge(st.st_size);
            if (size > door_quota - door_bytes) { error = 39; break; }
            door_bytes += size;
        } else if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { error = 5; break; }
            error = quota_scan(child, depth + 1);
            close(child);
            if (error) break;
        }
    }
    closedir(dp);
    return error;
}

static unsigned host_drive(const char *path)
{
    return drive_present(DRIVE_H) && path_below(path, drives[DRIVE_H].root) ?
           DRIVE_H : DRIVE_C;
}

static bool drive_root(const char *path)
{
    for (unsigned i = 0; i < DOS_DRIVES; ++i)
        if (drive_present(i) && !strcmp(path, drives[i].root)) return true;
    return false;
}

static bool drive_ancestor(const char *path)
{
    for (unsigned i = 0; i < DOS_DRIVES; ++i)
        if (drive_present(i) && path_below(drives[i].root, path)) return true;
    return false;
}

static int path_drive(const char **path, unsigned *drive)
{
    const char *p = *path;
    *drive = current_drive;
    if (*p && p[1] == ':') {
        *drive = (unsigned)(cp866_upper((uint8_t)*p) - 'A');
        p += 2;
    }
    if (!drive_present(*drive)) return 15;
    if (strchr(p, ':')) return 3;
    *path = p;
    return 0;
}

static int dos_errno(int e)
{
    switch (e) {
    case ENOENT: return 2;
    case ENOTDIR: case ENAMETOOLONG: case ELOOP: return 3;
    case EMFILE: case ENFILE: return 4;
    case EACCES: case EPERM: case EROFS: case EISDIR:
    case ENOTEMPTY: case EBUSY: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case ENODEV: case ENXIO: return 15;
    case EXDEV: return 17;
    case ENOSPC: case EDQUOT: return 39;
    case EEXIST: return 80;
    default: return 1;
    }
}

static void fail(int error)
{
    last_error = (uint16_t)error;
    cpu.a.x = (uint16_t)error;
    cpu.cf = 1;
}

static void succeed(void) { cpu.cf = 0; }
static void result(int error) { if (error) fail(error); else succeed(); }

static uint32_t get32(uint16_t seg, uint16_t off)
{
    return rd16(seg, off) | (uint32_t)rd16(seg, (uint16_t)(off + 2)) << 16;
}

static uint64_t get64(uint16_t seg, uint16_t off)
{
    return get32(seg, off) | (uint64_t)get32(seg, (uint16_t)(off + 4)) << 32;
}

static void put32(uint16_t seg, uint16_t off, uint32_t v)
{
    wr16(seg, off, (uint16_t)v);
    wr16(seg, (uint16_t)(off + 2), (uint16_t)(v >> 16));
}

static void put64(uint16_t seg, uint16_t off, uint64_t v)
{
    put32(seg, off, (uint32_t)v);
    put32(seg, (uint16_t)(off + 4), (uint32_t)(v >> 32));
}

static void zero_mem(uint16_t seg, uint16_t off, size_t n)
{
    for (size_t i = 0; i < n; ++i) wr8(seg, (uint16_t)(off + i), 0);
}

static int read_string(uint16_t seg, uint16_t off, char *s, size_t cap)
{
    for (size_t i = 0; i < cap; ++i) {
        s[i] = (char)rd8(seg, (uint16_t)(off + i));
        if (!s[i]) return 0;
    }
    s[cap - 1] = 0;
    return 3;
}

static int write_string(uint16_t seg, uint16_t off, const char *s, size_t cap)
{
    size_t n = strlen(s) + 1;
    if (n > cap) return 3;
    for (size_t i = 0; i < n; ++i) wr8(seg, (uint16_t)(off + i), (uint8_t)s[i]);
    return 0;
}

static int cp_to_utf8(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        uint32_t u = cp866_to_ucs((uint8_t)*s++);
        unsigned bytes = u < 0x80 ? 1 : u < 0x800 ? 2 : 3;
        if (n + bytes >= cap) return 3;
        if (bytes == 1) out[n++] = (char)u;
        else if (bytes == 2) {
            out[n++] = (char)(0xc0 | (u >> 6));
            out[n++] = (char)(0x80 | (u & 63));
        } else {
            out[n++] = (char)(0xe0 | (u >> 12));
            out[n++] = (char)(0x80 | ((u >> 6) & 63));
            out[n++] = (char)(0x80 | (u & 63));
        }
    }
    out[n] = 0;
    return 0;
}

/* Unrepresentable characters and invalid UTF-8 bytes use CP866 FEh (■),
 * never a DOS wildcard. Invalid bytes cannot consume following ASCII.
 * The return value distinguishes native spellings from lossy conversions. */
static bool utf8_to_cp(const char *s, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    bool lossless = true;
    while (*p && n + 1 < cap) {
        uint32_t u = *p;
        unsigned count = 1;
        if (*p >= 0xc2 && *p <= 0xdf) { u = *p & 31; count = 2; }
        else if (*p >= 0xe0 && *p <= 0xef) { u = *p & 15; count = 3; }
        else if (*p >= 0xf0 && *p <= 0xf4) { u = *p & 7; count = 4; }
        else if (*p >= 0x80) u = UINT32_MAX;
        bool valid = true;
        for (unsigned j = 1; j < count; ++j) {
            if ((p[j] & 0xc0) != 0x80) { valid = false; break; }
            u = (u << 6) | (p[j] & 63);
        }
        if (!valid || (count == 2 && u < 0x80) ||
            (count == 3 && u < 0x800) || (count == 4 && u < 0x10000) ||
            (u >= 0xd800 && u <= 0xdfff) || u > 0x10ffff) {
            u = UINT32_MAX;
            count = 1;
        }
        int c = ucs_to_cp866(u);
        /* Characters DOS forbids in a name are unrepresentable too: a \\ or
         * / would split the name into a path, * and ? are wildcards. */
        if (u < 0x20 || (u < 0x80 && strchr("\\/:*?\"<>|", (int)u))) c = -1;
        if (c < 0) lossless = false;
        out[n++] = c < 0 ? (char)0xfe : (char)c;
        p += count;
    }
    out[n] = 0;
    /* DOS and Windows drop trailing spaces and dots from a name, so VC cannot
     * name "note.txt " apart from "note.txt": it would act on the wrong file.
     * Such a trailing run is unrepresentable too. "." and ".." are names. */
    if (strcmp(out, ".") && strcmp(out, "..")) {
        size_t end = n;
        while (end && (out[end - 1] == ' ' || out[end - 1] == '.')) out[--end] = (char)0xfe;
        if (end != n) lossless = false;
    }
    return lossless && !*p;
}

static bool cp_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (cp866_upper((uint8_t)*a++) != cp866_upper((uint8_t)*b++)) return false;
    }
    return !*a && !*b;
}

static bool short_char(uint8_t c)
{
    return c > 32 && c != 127 && !strchr("\"*+,./:;<=>?[\\]|", c);
}

static bool valid_short(const char *name, char out[13])
{
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        strcpy(out, name);
        return true;
    }
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (!base || base > 8 || (dot && (!ext || ext > 3 || strchr(dot + 1, '.')))) return false;
    for (size_t i = 0; name[i]; ++i) {
        if (name[i] != '.' && !short_char((uint8_t)name[i])) return false;
        out[i] = (char)cp866_upper((uint8_t)name[i]);
    }
    out[strlen(name)] = 0;
    return true;
}

static void lease_alias(const DosPathLease *lease, const LeasePart *part, char out[13])
{
    memcpy(out, lease->dos + part->dos_start, part->dos_length);
    out[part->dos_length] = 0;
}

static bool lease_host_name(const DosPathLease *lease, const LeasePart *part,
                            const char *name)
{
    size_t n = part->host_end - part->host_start;
    return strlen(name) == n && !memcmp(lease->host + part->host_start, name, n);
}

static int lease_identity(const DosPathLease *lease, const LeasePart *part)
{
    char path[PATH_MAX];
    memcpy(path, lease->host, part->host_end);
    path[part->host_end] = 0;
    struct stat st, entry;
    if (fs_stat(path, &entry, true)) {
        /* Only the owner can deliberately make its reserved basename vacant.
         * An external deletion still cannot recreate a stale literal alias. */
        return errno == ENOENT && part->type == S_IFREG && lease->recreate ? 0 : 5;
    }
    if (fs_stat(path, &st, false) || (st.st_mode & S_IFMT) != part->type ||
        (entry.st_mode & S_IFMT) != part->entry_type) return 5;
    /* Directories and symlink entries must stay attached to their own identity.
     * A regular file, however, is a saved pathname: editors, git and sync tools
     * may replace its inode without changing which file the user is editing. */
    if ((part->type == S_IFDIR && (st.st_dev != part->dev || st.st_ino != part->ino)) ||
        ((part->type == S_IFDIR || part->entry_type == S_IFLNK) &&
         (entry.st_dev != part->entry_dev || entry.st_ino != part->entry_ino))) return 5;
    return 0;
}

static int lease_alias_conflict(const DosPathLease *lease, const LeasePart *part)
{
    char parent[PATH_MAX], alias[13];
    memcpy(parent, lease->host, part->parent_end);
    parent[part->parent_end] = 0;
    lease_alias(lease, part, alias);
    DIR *dp = fs_opendir(parent);
    if (!dp) return 5;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) { if (errno) error = 5; break; }
        if (lease_host_name(lease, part, de->d_name)) continue;
        char cp[DOS_NAME_MAX + 1], short_name[13];
        if (utf8_to_cp(de->d_name, cp, sizeof(cp)) && valid_short(cp, short_name) &&
            cp_equal(short_name, alias)) { error = 5; break; }
    }
    closedir(dp);
    return error;
}

/* A directory lookup validates only that prefix. A moved, replaced or missing
 * descendant must not prevent unrelated opens, backup lookup or Save As. */
static int lease_validate(const DosPathLease *lease, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (lease_identity(lease, lease->parts + i) ||
            lease_alias_conflict(lease, lease->parts + i)) return 5;
    return 0;
}

void dos_fs_release_path(DosPathLease *lease)
{
    DosPathLease **p = &path_leases;
    while (*p && *p != lease) p = &(*p)->next;
    if (!*p) return;
    *p = lease->next;
    struct stat st;
    bool unlinked = door_root_fd >= 0 && lease->fd >= 0 &&
                    !fstat(lease->fd, &st) && !st.st_nlink;
    if (lease->fd >= 0) close(lease->fd);
    if (unlinked && !inode_is_open(&st)) quota_release_entry(&st);
    free(lease);
}

void dos_fs_bind_path(DosPathLease *lease, uint16_t psp)
{
    if (lease) { lease->psp = psp; lease->bound = true; }
}

static bool active_lease(const DosPathLease *lease)
{
    return lease->bound && lease->psp == fcb_process;
}

static bool lease_parent(const DosPathLease *lease, const LeasePart *part, const char *dir)
{
    return strlen(dir) == part->parent_end && !memcmp(dir, lease->host, part->parent_end);
}

#ifndef __EMSCRIPTEN__
static int guarded_directory(const char *dir)
{
    for (DosPathLease *lease = path_leases; lease; lease = lease->next)
        if (active_lease(lease) && lease->guard_directory && path_below(dir, lease->host) &&
            lease_validate(lease, lease->count)) return 5;
    return 0;
}
#endif

/* Consult leases before exact native lookup: a newly created literal 8.3
 * name must cause refusal, not steal the selected file's saved spelling. */
static int resolve_leased_name(const char *dir, const char *name, char out[PATH_MAX],
                               bool *claimed)
{
    *claimed = false;
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease)) continue;
        for (unsigned i = 0; i < lease->count; ++i) {
            const LeasePart *part = lease->parts + i;
            if (!lease_parent(lease, part, dir)) continue;
            char alias[13];
            lease_alias(lease, part, alias);
            if (!cp_equal(alias, name)) continue;
            if (lease_validate(lease, i + 1)) return 5;
            if (*claimed && (strlen(out) != part->host_end ||
                             memcmp(out, lease->host, part->host_end))) return 5;
            memcpy(out, lease->host, part->host_end);
            out[part->host_end] = 0;
            *claimed = true;
        }
    }
    return 0;
}

static int reserve_leased_aliases(const char *dir, Entry *entries, size_t count)
{
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease)) continue;
        for (unsigned i = 0; i < lease->count; ++i) {
            const LeasePart *part = lease->parts + i;
            if (!lease_parent(lease, part, dir)) continue;
            /* Reserve the leaf's spelling even if it is currently missing or
             * unsafe to open. Its own identity cannot poison this directory's
             * other entries; opening the leaf validates it separately. */
            if (lease_validate(lease, i) || lease_alias_conflict(lease, part) ||
                (part->type == S_IFDIR && lease_identity(lease, part))) return 5;
            char alias[13];
            lease_alias(lease, part, alias);
            size_t index;
            for (index = 0; index < count; ++index)
                if (lease_host_name(lease, part, entries[index].host)) break;
            if (index == count) continue;
            if (*entries[index].alias && strcmp(entries[index].alias, alias)) return 5;
            for (size_t j = 0; j < count; ++j)
                if (j != index && !strcmp(entries[j].alias, alias)) return 5;
            strcpy(entries[index].alias, alias);
        }
    }
    return 0;
}

static int leased_host(const char *path, DosPathLease **out)
{
    *out = NULL;
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease) || strcmp(lease->host, path)) continue;
        if (lease_validate(lease, lease->count)) return 5;
        *out = lease;
    }
    return 0;
}

static void alias_parts(const char *name, char base[256], char ext[4])
{
    const char *start = name;
    while (*start == '.') ++start;
    const char *dot = strrchr(start, '.');
    size_t n = 0;
    for (const char *p = start; *p && p != dot; ++p) {
        if (*p == '.' || *p == ' ') continue;
        base[n++] = short_char((uint8_t)*p) ? (char)cp866_upper((uint8_t)*p) : '_';
    }
    if (!n) base[n++] = '_';
    base[n] = 0;
    n = 0;
    if (dot) for (const char *p = dot + 1; *p && n < 3; ++p) {
        if (*p == ' ' || *p == '.') continue;
        ext[n++] = short_char((uint8_t)*p) ? (char)cp866_upper((uint8_t)*p) : '_';
    }
    ext[n] = 0;
}

static void numbered_alias(const char *name, unsigned number, char out[13])
{
    char base[256], ext[4], tail[12];
    alias_parts(name, base, ext);
    snprintf(tail, sizeof(tail), "~%u", number);
    size_t n = strlen(tail);
    size_t prefix = n < 8 ? 8 - n : 0;
    if (prefix > strlen(base)) prefix = strlen(base);
    memcpy(out, base, prefix);
    memcpy(out + prefix, tail, n);
    size_t pos = prefix + n;
    if (*ext) { out[pos++] = '.'; memcpy(out + pos, ext, strlen(ext)); pos += strlen(ext); }
    out[pos] = 0;
}

static bool reserved_alias(const char *dir, const char *alias)
{
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease)) continue;
        for (unsigned i = 0; i < lease->count; ++i) {
            const LeasePart *part = lease->parts + i;
            char reserved[13];
            if (!lease_parent(lease, part, dir)) continue;
            lease_alias(lease, part, reserved);
            if (cp_equal(alias, reserved)) return true;
        }
    }
    return false;
}

static bool reserved_host_alias(const char *dir, const char *host, char out[13])
{
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease)) continue;
        for (unsigned i = 0; i < lease->count; ++i) {
            const LeasePart *part = lease->parts + i;
            if (!lease_parent(lease, part, dir) || !lease_host_name(lease, part, host)) continue;
            lease_alias(lease, part, out);
            return true;
        }
    }
    return false;
}

static bool alias_used(const char *dir, const Entry *entries, size_t count, const char *alias)
{
    for (size_t i = 0; i < count; ++i)
        if (*entries[i].alias && !strcmp(entries[i].alias, alias)) return true;
    /* A vacant owner-renamed leaf has no Entry, but its spelling must not be
     * handed to a neighbor before VZ recreates the selected long host name. */
    return reserved_alias(dir, alias);
}

static void hashed_name(const char *host, const char *name, char out[DOS_NAME_MAX + 1])
{
    /* FNV-1a over the host basename's bytes, XOR-folded to 16 bits. The
     * suffix must not depend on which other entries happen to exist. */
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p) {
        hash ^= *p;
        hash *= UINT32_C(16777619);
    }
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL; /* A leading dot alone is not an extension. */
    char tail[6];
    snprintf(tail, sizeof(tail), "~%04X", (unsigned)((hash ^ (hash >> 16)) & 0xffff));
    size_t suffix = strlen(tail), ext = dot ? strlen(dot) : 0;
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    /* Keep the extension when possible, with room for a base byte and ~XXXX.
     * CP866 is single-byte, so truncation cannot split a character. */
    if (ext > DOS_NAME_MAX - suffix - 1) ext = DOS_NAME_MAX - suffix - 1;
    if (base > DOS_NAME_MAX - suffix - ext) base = DOS_NAME_MAX - suffix - ext;
    memcpy(out, name, base);
    memcpy(out + base, tail, suffix);
    if (ext) memcpy(out + base + suffix, dot, ext);
    out[base + suffix + ext] = 0;
}

static void assign_dos_names(Entry *entries, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].lossless) continue;
        char candidate[DOS_NAME_MAX + 1];
        hashed_name(entries[i].host, entries[i].dos, candidate);
        strcpy(entries[i].dos, candidate);
    }
    /* A hash collision must never move either name onto a saved selection's
     * spelling. Keep listing both, but refuse the ambiguous lossy spelling.
     * Native names retain priority, even over case-insensitive collisions. */
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].lossless) continue;
        for (size_t j = 0; j < count; ++j)
            if (i != j && cp_equal(entries[i].dos, entries[j].dos)) {
                entries[i].ambiguous = true;
                break;
            }
    }
}

/* Count distinct matching entries, not spellings: one entry can match both
 * its long and short name. Refuse ambiguity instead of choosing a victim. */
static unsigned matching_entry(const Entry *entries, size_t count, const char *name, size_t *index)
{
    unsigned matches = 0;
    /* Preserve exact native spelling here too: short-name conversion calls
     * this helper without going through resolve_name's exact-host check. */
    for (size_t i = 0; i < count; ++i)
        if (entries[i].lossless && !strcmp(name, entries[i].dos)) {
            *index = i;
            return 1;
        }
    /* Not an exact native spelling. If an ambiguous generated name also
     * matches, the input could mean either entry, so refuse rather than
     * hand back the native one: F8 on the generated row would delete it. */
    for (size_t i = 0; i < count; ++i)
        if (entries[i].ambiguous && cp_equal(name, entries[i].dos)) return 2;
    /* Real long names win over generated spellings. Native case twins keep
     * their existing unique short alias as a tie-break for nonexact input. */
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].lossless || !cp_equal(name, entries[i].dos)) continue;
        *index = i;
        if (cp_equal(name, entries[i].alias)) return 1;
        ++matches;
    }
    if (matches) return matches;
    /* Do not let a blocked long name fall through to an unrelated alias or
     * to allow_missing creation of a literal replacement-character name. */
    for (size_t i = 0; i < count; ++i)
        if (entries[i].ambiguous && cp_equal(name, entries[i].dos)) return 2;
    for (size_t i = 0; i < count; ++i) {
        if (!cp_equal(name, entries[i].dos) && !cp_equal(name, entries[i].alias)) continue;
        if (++matches > 1) return matches;
        *index = i;
    }
    return matches;
}

static int entry_cmp(const void *va, const void *vb)
{
    return strcmp(((const Entry *)va)->host, ((const Entry *)vb)->host);
}

static void free_entries(Entry *entries, size_t count)
{
    for (size_t i = 0; i < count; ++i) free(entries[i].host);
    free(entries);
}

static int join_path(const char *dir, const char *name, char *out, size_t cap)
{
    size_t n = strlen(dir), m = strlen(name);
    bool slash = n && dir[n - 1] != '/';
    if (n + slash + m >= cap) return 3;
    memmove(out, dir, n);
    if (slash) out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 0;
}

static int list_directory(const char *dir, Entry **out, size_t *out_count)
{
    DIR *dp = fs_opendir(dir);
    if (!dp) return errno == ENOENT ? 3 : dos_errno(errno);
    Entry *entries = NULL;
    size_t count = 0, cap = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) { if (errno) error = dos_errno(errno); break; }
        if (!strcmp(dir, "/") && (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))) continue;
        if (count == cap) {
            size_t next = cap ? cap * 2 : 32;
            Entry *new_entries = realloc(entries, next * sizeof(*entries));
            if (!new_entries) { error = 8; break; }
            entries = new_entries;
            cap = next;
        }
        memset(entries + count, 0, sizeof(*entries));
        entries[count].host = strdup(de->d_name);
        if (!entries[count].host) { error = 8; break; }
        entries[count].lossless = utf8_to_cp(de->d_name, entries[count].dos, sizeof(entries[count].dos));
        ++count;
    }
    closedir(dp);
    if (error) { free_entries(entries, count); return error; }
    if (count) qsort(entries, count, sizeof(*entries), entry_cmp);
    assign_dos_names(entries, count);
    error = reserve_leased_aliases(dir, entries, count);
    if (error) { free_entries(entries, count); return error; }
    /* Reserve every real 8.3 name first, including lexically later names.
     * Case-colliding native short names after the first also need an alias. */
    for (size_t i = 0; i < count; ++i) {
        if (*entries[i].alias) continue;
        char candidate[13];
        if (valid_short(entries[i].dos, candidate) && !alias_used(dir, entries, count, candidate))
            strcpy(entries[i].alias, candidate);
    }
    for (size_t i = 0; i < count; ++i) {
        if (*entries[i].alias) continue;
        char candidate[13];
        unsigned n;
        for (n = 1; n <= 9999999; ++n) {
            numbered_alias(entries[i].dos, n, candidate);
            if (!alias_used(dir, entries, count, candidate)) break;
        }
        if (n > 9999999) { free_entries(entries, count); return 4; }
        strcpy(entries[i].alias, candidate);
    }
    *out = entries;
    *out_count = count;
    return 0;
}

static unsigned file_attr(const char *path, const struct stat *st)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    unsigned attr = S_ISDIR(st->st_mode) ? ATTR_DIR : S_ISREG(st->st_mode) ? ATTR_ARCHIVE : 0;
    if (!(st->st_mode & S_IWUSR)) attr |= ATTR_RO;
    if (*name == '.' && strcmp(name, ".") && strcmp(name, "..")) attr |= ATTR_HIDDEN;
    return attr;
}

static int file_stat(const char *path, struct stat *st)
{
    if (fs_stat(path, st, false) == 0) return 0;
    int saved = errno;
    if ((saved == ENOENT || saved == ENOTDIR) && fs_stat(path, st, true) == 0 && S_ISLNK(st->st_mode)) {
        st->st_mode = (st->st_mode & 07777) | S_IFREG;
        st->st_size = 0;
        return 0;
    }
    return dos_errno(saved);
}

static struct timespec birth_time(const char *path, int fd, const struct stat *st)
{
    for (BirthTime *b = birth_times; b; b = b->next)
        if (b->dev == st->st_dev && b->ino == st->st_ino) return b->time;
#ifndef __EMSCRIPTEN__
    struct statx sx;
    int held = -1;
    if (door_root_fd >= 0 && fd < 0) {
        held = fs_open(path, O_RDONLY | O_CLOEXEC, 0);
        if (held < 0) return st->st_ctim;
        fd = held;
    }
    int ok = fd >= 0 ? statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &sx) :
                      statx(AT_FDCWD, path, 0, STATX_BTIME, &sx);
    if (held >= 0) close(held);
    if (!ok && (sx.stx_mask & STATX_BTIME))
        return (struct timespec){(time_t)sx.stx_btime.tv_sec, (long)sx.stx_btime.tv_nsec};
#else
    /* MEMFS exposes no statx birth time. Preserve explicit DOS overrides
     * above, otherwise use the same ctime fallback as a Linux FS without it. */
    (void)path;
    (void)fd;
#endif
    return st->st_ctim;
}

static int set_birth(const struct stat *st, struct timespec time)
{
    BirthTime *b;
    for (b = birth_times; b; b = b->next)
        if (b->dev == st->st_dev && b->ino == st->st_ino) break;
    if (!b) {
        b = malloc(sizeof(*b));
        if (!b) return 8;
        b->dev = st->st_dev;
        b->ino = st->st_ino;
        b->next = birth_times;
        birth_times = b;
    }
    b->time = time;
    return 0;
}

static int resolve_name(const char *dir, const char *name, char *out, bool allow_missing)
{
#ifndef __EMSCRIPTEN__
    if (guarded_directory(dir)) return 5;
#endif
    char utf8[4 * (DOS_NAME_MAX + 1)], exact[PATH_MAX];
    if (strlen(name) > DOS_NAME_MAX) return 3;
    bool claimed;
    int error = resolve_leased_name(dir, name, out, &claimed);
    if (error || claimed) return error;
    error = cp_to_utf8(name, utf8, sizeof(utf8));
    if (error || (error = join_path(dir, utf8, exact, sizeof(exact)))) return error;
    /* A name that exists exactly as spelled is always that entry: real names
     * take priority over every generated alias and collision suffix. This
     * skips listing the directory, which is slow in big ones and impossible
     * in execute-only ones. */
    struct stat exact_st;
    if (fs_stat(exact, &exact_st, true) == 0) {
        strcpy(out, exact);
        return 0;
    }
    if (door_root_fd >= 0 && errno != ENOENT) return dos_errno(errno);
    Entry *entries;
    size_t count;
    error = list_directory(dir, &entries, &count);
    if (error) return error;
    size_t index = 0;
    unsigned matches = matching_entry(entries, count, name, &index);
    if (matches == 1) error = join_path(dir, entries[index].host, out, PATH_MAX);
    else if (!matches && allow_missing && !strpbrk(name, "\"<>|:")) strcpy(out, exact);
    else error = 2;
    free_entries(entries, count);
    return error;
}

static void parent_path(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) strcpy(path, "/");
    else *slash = 0;
}

static int resolve_path(const char *dos, char out[PATH_MAX], bool allow_missing)
{
    if (!*dos) return 3;
    const char *p = dos;
    unsigned drive;
    int error = path_drive(&p, &drive);
    if (error) return error;
    int pipe = command_pipe_path(p, drive, allow_missing, out);
    if (pipe) return pipe < 0 ? -pipe : 0;
    const DosDrive *d = drives + drive;
    strcpy(out, (*p == '/' || *p == '\\') ? d->root : d->cwd);
#ifndef __EMSCRIPTEN__
    if (guarded_directory(out)) return 5;
#endif
    if (door_root_fd >= 0) {
        struct stat st;
        if (fs_stat(out, &st, false)) return dos_errno(errno);
        if (!S_ISDIR(st.st_mode)) return 3;
    }
    while (*p) {
        while (*p == '/' || *p == '\\') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        size_t n = (size_t)(p - start);
        if (n > DOS_NAME_MAX) return 3;
        char part[DOS_NAME_MAX + 1];
        memcpy(part, start, n);
        part[n] = 0;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            if (strcmp(out, d->root)) parent_path(out);
            continue;
        }
        char resolved[PATH_MAX];
        error = resolve_name(out, part, resolved, allow_missing && !*p);
        if (error) return error == 2 && *p ? 3 : error;
        if (*p || door_root_fd >= 0) {
            struct stat st;
            error = file_stat(resolved, &st);
            if (error == 2 && allow_missing && !*p) { strcpy(out, resolved); break; }
            if (error) return error == 2 ? 3 : error;
            if (*p && !S_ISDIR(st.st_mode)) return 3;
        }
        strcpy(out, resolved);
    }
    return 0;
}

int dos_fs_pin_path(const char *dos, const char *host, DosPathLease **out)
{
    if (!out) return 5;
    *out = NULL;
    if (!dos || !host || strlen(dos) >= 128 || strlen(host) >= PATH_MAX ||
        strlen(dos) < 4 || dos[1] != ':' || (dos[2] != '\\' && dos[2] != '/') ||
        *host != '/') return 5;
    const char *input = dos;
    unsigned drive;
    if (path_drive(&input, &drive) || !path_below(host, drives[drive].root) ||
        !strcmp(host, drives[drive].root)) return 5;
    char resolved[PATH_MAX];
    if (resolve_path(dos, resolved, false) || strcmp(resolved, host)) return 5;
    DosPathLease *lease = calloc(1, sizeof(*lease));
    if (!lease) return 8;
    lease->fd = -1;
    strcpy(lease->host, host);
    for (size_t i = 0; dos[i]; ++i)
        lease->dos[i] = dos[i] == '/' ? '\\' : (char)cp866_upper((uint8_t)dos[i]);
    size_t d = 3, parent = strlen(drives[drive].root);
    size_t h = parent + (host[parent - 1] != '/');
    int error = 0;
    while (lease->dos[d] && host[h]) {
        if (lease->count == sizeof(lease->parts) / sizeof(*lease->parts)) { error = 5; break; }
        size_t dend = d, hend = h;
        while (lease->dos[dend] && lease->dos[dend] != '\\') ++dend;
        while (host[hend] && host[hend] != '/') ++hend;
        char name[13], candidate[13];
        size_t n = dend - d;
        if (!n || n >= sizeof(name)) { error = 5; break; }
        memcpy(name, lease->dos + d, n); name[n] = 0;
        if (!valid_short(name, candidate) || !strcmp(name, ".") || !strcmp(name, "..")) {
            error = 5; break;
        }
        lease->parts[lease->count++] = (LeasePart){
            .parent_end = (uint16_t)parent, .host_start = (uint16_t)h,
            .host_end = (uint16_t)hend, .dos_start = (uint16_t)d, .dos_length = (uint8_t)n};
        parent = hend;
        d = dend + !!lease->dos[dend];
        h = hend + !!host[hend];
    }
    if (lease->dos[d] || host[h] || !lease->count) error = 5;
    struct stat st, entry;
    for (unsigned i = 0; !error && i < lease->count; ++i) {
        LeasePart *part = lease->parts + i;
        memcpy(resolved, host, part->host_end);
        resolved[part->host_end] = 0;
        if (fs_stat(resolved, &st, false) || fs_stat(resolved, &entry, true) ||
            (i + 1 < lease->count ? !S_ISDIR(st.st_mode) :
             (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))) { error = 5; break; }
        part->dev = st.st_dev; part->ino = st.st_ino;
        part->entry_dev = entry.st_dev; part->entry_ino = entry.st_ino;
        part->type = st.st_mode & S_IFMT;
        part->entry_type = entry.st_mode & S_IFMT;
    }
    if (!error) {
        do { lease->fd = fs_open(host, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0); }
        while (lease->fd < 0 && errno == EINTR);
        if (lease->fd < 0 || fstat(lease->fd, &st) || fs_stat(host, &entry, true) ||
            (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) error = 5;
    }
    if (!error) {
        const LeasePart *leaf = lease->parts + lease->count - 1;
        if (st.st_dev != leaf->dev || st.st_ino != leaf->ino ||
            entry.st_dev != leaf->entry_dev || entry.st_ino != leaf->entry_ino) error = 5;
        else error = lease_validate(lease, lease->count);
    }
    if (error) {
        if (lease->fd >= 0) close(lease->fd);
        free(lease);
        return error;
    }
    lease->next = path_leases;
    path_leases = lease;
    *out = lease;
    return 0;
}

#ifndef __EMSCRIPTEN__
int dos_fs_guard_directory(const char *dos, const char *host, DosPathLease **out)
{
    int error = dos_fs_pin_path(dos, host, out);
    if (error) return error;
    DosPathLease *lease = *out;
    if (lease->parts[lease->count - 1].type != S_IFDIR) {
        dos_fs_release_path(lease);
        *out = NULL;
        return 5;
    }
    lease->guard_directory = true;
    return 0;
}
#endif

static int memory_path(uint16_t seg, uint16_t off, char out[PATH_MAX], bool allow_missing)
{
    char dos[DOS_PATH_MAX];
    int error = read_string(seg, off, dos, sizeof(dos));
    return error ? error : resolve_path(dos, out, allow_missing);
}

int dos_fs_to_host(uint16_t seg, uint16_t off, char *out, size_t cap)
{
    if (!initialized) dos_fs_init();
    char host[PATH_MAX];
    int error = memory_path(seg, off, host, true);
    if (!error && strlen(host) >= cap) error = 3;
    if (error) {
        errno = error == 15 ? ENODEV : error == 5 ? EACCES : error == 8 ? ENOMEM :
                error == 2 ? ENOENT : ENOTDIR;
        return -1;
    }
    memcpy(out, host, strlen(host) + 1);
    return 0;
}

static int host_to_dos_on_drive(const char *host, unsigned drive, bool short_names,
                                char *out, size_t cap)
{
    const char *root = drives[drive].root;
    if (!path_below(host, root)) return 3;
    char dir[PATH_MAX];
    strcpy(dir, root);
    size_t used = 3;
    if (cap < 4) return 3;
    memcpy(out, "C:\\", 4);
    out[0] = (char)('A' + drive);
    const char *p = host + strlen(root);
    while (*p == '/') ++p;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - start);
        char part[NAME_MAX + 1], dos[DOS_NAME_MAX + 1];
        if (n > NAME_MAX) return 3;
        memcpy(part, start, n); part[n] = 0;
        bool lossless = utf8_to_cp(part, dos, sizeof(dos));
        if (short_names || !lossless) {
            Entry *entries;
            size_t count;
            int error = list_directory(dir, &entries, &count);
            if (error) return error;
            bool found = false;
            for (size_t i = 0; i < count; ++i) if (!strcmp(entries[i].host, part)) {
                strcpy(dos, short_names ? entries[i].alias : entries[i].dos); found = true; break;
            }
            if (!found && short_names) found = reserved_host_alias(dir, part, dos);
            if (!found && short_names) {
                char candidate[13];
                if (!valid_short(dos, candidate) || alias_used(dir, entries, count, candidate)) {
                    unsigned i;
                    for (i = 1; i <= 9999999; ++i) {
                        numbered_alias(dos, i, candidate);
                        if (!alias_used(dir, entries, count, candidate)) break;
                    }
                    if (i > 9999999) { free_entries(entries, count); return 4; }
                }
                strcpy(dos, candidate);
            }
            free_entries(entries, count);
        }
        size_t len = strlen(dos);
        if (used + len + (*p != 0) >= cap) return 3;
        memcpy(out + used, dos, len); used += len;
        if (*p) out[used++] = '\\';
        out[used] = 0;
        char next[PATH_MAX];
        int error = join_path(dir, part, next, sizeof(next));
        if (error) return error;
        strcpy(dir, next);
        while (*p == '/') ++p;
    }
    return 0;
}

static int host_to_dos(const char *host, bool short_names, char *out, size_t cap)
{
    return host_to_dos_on_drive(host, host_drive(host), short_names, out, cap);
}

#ifndef __EMSCRIPTEN__
int dos_fs_host_short_path(const char *host, char *out, size_t cap)
{
    if (!host || *host != '/' || !out) return 5;
    return host_to_dos(host, true, out, cap);
}
#endif

static bool pack_time(struct timespec ts, uint16_t *time, uint16_t *date)
{
    struct tm tm;
    if (!localtime_r(&ts.tv_sec, &tm) || tm.tm_year < 80) {
        *time = 0; *date = 0x0021;
        return false;
    }
    if (tm.tm_year > 207 || (tm.tm_year == 207 && tm.tm_mon == 11 && tm.tm_mday == 31 &&
        tm.tm_hour == 23 && tm.tm_min == 59 && (tm.tm_sec > 58 || (tm.tm_sec == 58 && ts.tv_nsec)))) {
        *time = 0xbf7d; *date = 0xff9f;
        return false;
    }
    *time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    return true;
}

static int unpack_time(uint16_t time, uint16_t date, unsigned hundredths,
                       struct timespec *out)
{
    struct tm tm = {0};
    int year = (date >> 9) + 1980, month = (date >> 5) & 15, day = date & 31;
    int hour = time >> 11, min = (time >> 5) & 63, sec = (time & 31) * 2;
    if (hundredths > 199 || month < 1 || month > 12 || !day ||
        hour > 23 || min > 59 || sec > 58) return 13;
    sec += (int)(hundredths / 100);
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec; tm.tm_isdst = -1;
    time_t value = mktime(&tm);
    if (value == (time_t)-1 || tm.tm_year != year - 1900 || tm.tm_mon != month - 1 ||
        tm.tm_mday != day || tm.tm_hour != hour || tm.tm_min != min || tm.tm_sec != sec) return 13;
    out->tv_sec = value;
    out->tv_nsec = (long)(hundredths % 100) * 10000000;
    return 0;
}

static uint64_t to_filetime(struct timespec ts)
{
    const int64_t epoch = INT64_C(11644473600);
    if (ts.tv_sec < -epoch) return 0;
    uint64_t seconds = (uint64_t)ts.tv_sec + (uint64_t)epoch;
    uint64_t part = (uint64_t)ts.tv_nsec / 100;
    if (seconds > (UINT64_MAX - part) / 10000000) return UINT64_MAX;
    return seconds * 10000000 + part;
}

static struct timespec from_filetime(uint64_t ft)
{
    return (struct timespec){(time_t)(ft / 10000000) - INT64_C(11644473600),
                             (long)(ft % 10000000) * 100};
}

static unsigned time_hundredths(struct timespec ts)
{
    struct tm tm;
    if (!localtime_r(&ts.tv_sec, &tm)) return 0;
    return (unsigned)(tm.tm_sec & 1) * 100 + (unsigned)(ts.tv_nsec / 10000000);
}

static struct timespec normalized_time(struct timespec ts)
{
    if (ts.tv_nsec >= 1000000000) { ++ts.tv_sec; ts.tv_nsec -= 1000000000; }
    if (ts.tv_nsec < 0) { --ts.tv_sec; ts.tv_nsec += 1000000000; }
    return ts;
}

static struct timespec dos_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    now.tv_sec += clock_delta.tv_sec;
    now.tv_nsec += clock_delta.tv_nsec;
    return normalized_time(now);
}

static int set_clock(bool date_call)
{
    struct timespec now = dos_now();
    struct tm tm;
    if (!localtime_r(&now.tv_sec, &tm)) return 13;
    int year = date_call ? cpu.c.x : tm.tm_year + 1900;
    int month = date_call ? cpu.d.h : tm.tm_mon + 1;
    int day = date_call ? cpu.d.l : tm.tm_mday;
    int hour = date_call ? tm.tm_hour : cpu.c.h;
    int min = date_call ? tm.tm_min : cpu.c.l;
    int sec = date_call ? tm.tm_sec : cpu.d.h;
    int sub = date_call ? (int)(now.tv_nsec / 10000000) : cpu.d.l;
    if (year < 1980 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59 || sub > 99) return 13;
    uint16_t dt = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    uint16_t tt = (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
    struct timespec target, real;
    int error = unpack_time(tt, dt, (unsigned)((sec & 1) * 100 + sub), &target);
    if (error) return error;
    clock_gettime(CLOCK_REALTIME, &real);
    clock_delta.tv_sec = target.tv_sec - real.tv_sec;
    clock_delta.tv_nsec = target.tv_nsec - real.tv_nsec;
    clock_delta = normalized_time(clock_delta);
    cpu.a.l = 0;
    return 0;
}

static bool wildcard_core(const char *pattern, const char *name)
{
    size_t len = strlen(name);
    bool prev[DOS_NAME_MAX + 1] = {true}, next[DOS_NAME_MAX + 1];
    for (const char *p = pattern; *p; ++p) {
        memset(next, 0, sizeof(next));
        for (size_t n = 0; n <= len; ++n) {
            if (*p == '*') next[n] = prev[n] || (n && next[n - 1]);
            else if (*p == '?') {
                if (n == len || name[n] == '.') next[n] = prev[n];
                if (n && name[n - 1] != '.') next[n] |= prev[n - 1];
            } else if (n && cp866_upper((uint8_t)*p) == cp866_upper((uint8_t)name[n - 1]))
                next[n] = prev[n - 1];
        }
        memcpy(prev, next, sizeof(prev));
    }
    return prev[len];
}

static bool long_match(const char *pattern, const char *name)
{
    if (!strcmp(pattern, "*.*")) return true;
    if (wildcard_core(pattern, name)) return true;
    size_t n = strlen(pattern);
    if (n >= 2 && !strcmp(pattern + n - 2, ".*")) {
        char shorter[DOS_NAME_MAX + 1];
        memcpy(shorter, pattern, n - 2); shorter[n - 2] = 0;
        return wildcard_core(shorter, name);
    }
    if (n && pattern[n - 1] == '.' && !strchr(name, '.')) {
        char shorter[DOS_NAME_MAX + 1];
        memcpy(shorter, pattern, n - 1); shorter[n - 1] = 0;
        return wildcard_core(shorter, name);
    }
    return false;
}

static bool classic_field(const char *pattern, size_t pn, const char *name, size_t nn, size_t width)
{
    char field[8];
    memset(field, ' ', width);
    size_t n = 0;
    while (n < pn && n < width) {
        if (pattern[n] == '*') { memset(field + n, '?', width - n); break; }
        field[n] = (char)cp866_upper((uint8_t)pattern[n]);
        ++n;
    }
    if (n == width && n < pn && pattern[n] != '*') return false;
    for (size_t i = 0; i < width; ++i) {
        uint8_t c = i < nn ? cp866_upper((uint8_t)name[i]) : ' ';
        if (field[i] != '?' && (uint8_t)field[i] != c) return false;
    }
    return true;
}

static bool classic_match(const char *pattern, const char *name)
{
    if (!strcmp(pattern, "*") || !strcmp(pattern, "*.*")) return true;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return cp_equal(pattern, name);
    const char *pd = strchr(pattern, '.'), *nd = strchr(name, '.');
    size_t pn = pd ? (size_t)(pd - pattern) : strlen(pattern);
    size_t nn = nd ? (size_t)(nd - name) : strlen(name);
    return classic_field(pattern, pn, name, nn, 8) &&
           classic_field(pd ? pd + 1 : "", pd ? strlen(pd + 1) : 0,
                         nd ? nd + 1 : "", nd ? strlen(nd + 1) : 0, 3);
}

static bool attributes_match(unsigned attr, uint16_t mask, bool lfn)
{
    unsigned allowed = mask & 0xff, required = lfn ? (mask >> 8) & 0x3f : 0;
    if ((allowed & 0x3f) == ATTR_VOLUME || (required & ATTR_VOLUME)) return false;
    if ((attr & (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_DIR)) & ~allowed) return false;
    return (attr & required) == required;
}

static int split_pattern(const char *dos, char dir[PATH_MAX], char pattern[DOS_NAME_MAX + 1])
{
    size_t n = strlen(dos), split = 0;
    for (size_t i = 0; i < n; ++i) if (dos[i] == '/' || dos[i] == '\\' || dos[i] == ':') split = i + 1;
    const char *pat = dos + split;
    /* A missing filespec is not an implicit wildcard, especially for 7141:
     * interpreting an empty/delete-directory argument as '*' loses data. */
    if (!*pat) return 3;
    if (strlen(pat) > DOS_NAME_MAX) return 3;
    strcpy(pattern, pat);
    if (!split) { strcpy(dir, drives[current_drive].cwd); return 0; }
    char parent[DOS_PATH_MAX];
    memcpy(parent, dos, split); parent[split] = 0;
    int error = resolve_path(parent, dir, false);
    if (error) return error == 2 ? 3 : error;
    struct stat st;
    error = file_stat(dir, &st);
    if (error) return error == 2 ? 3 : error;
    return S_ISDIR(st.st_mode) ? 0 : 3;
}

/* A symlink to a directory that contains it (/proc/1/root points at /) would
 * send a recursive walk, such as VC's directory tree, round in a circle. Such
 * a link is listed as a plain file. Opening a path through it still works. */
static bool links_to_ancestor(const char *dir_real, const char *path, const struct stat *st)
{
    struct stat lst;
    if (door_root_fd >= 0 || !S_ISDIR(st->st_mode) || fs_stat(path, &lst, true) != 0 ||
        !S_ISLNK(lst.st_mode) || !dir_real)
        return false;
    char *target = realpath(path, NULL);
    if (!target) return false;
    size_t n = strlen(target);
    bool ancestor = !strcmp(target, "/") ||
                    (!strncmp(dir_real, target, n) && (dir_real[n] == '/' || dir_real[n] == 0));
    free(target);
    return ancestor;
}

static int snapshot(const char *dir, unsigned drive, Entry **entries, size_t *count)
{
    int error = list_directory(dir, entries, count);
    if (error) return error;
    bool at_root = !strcmp(dir, drives[drive].root);
    char *dir_real = door_root_fd >= 0 ? NULL : realpath(dir, NULL);
    for (size_t i = 0; i < *count; ++i) {
        Entry *e = *entries + i;
        /* Do not even form an H-root/.. host path. The same directory can
         * still expose dot entries when enumerated through its C: name. */
        if (at_root && (!strcmp(e->host, ".") || !strcmp(e->host, ".."))) continue;
        char path[PATH_MAX];
        if (join_path(dir, e->host, path, sizeof(path)) || file_stat(path, &e->st)) continue;
        if (links_to_ancestor(dir_real, path, &e->st)) {
            e->st.st_mode = (e->st.st_mode & 07777) | S_IFREG;
            e->st.st_size = 0;
        }
        e->attr = file_attr(path, &e->st);
        e->birth = birth_time(path, -1, &e->st);
        e->have_stat = true;
    }
    free(dir_real);
    return 0;
}

static void release_search(Search *s)
{
    free_entries(s->entries, s->count);
    s->entries = NULL; s->count = s->next = 0;
    s->active = false;
}

static Search *classic_search(void)
{
    unsigned slot = rd16(dta_seg, dta_off);
    if (!slot || slot > SEARCH_SLOTS) return NULL;
    Search *s = searches + slot - 1;
    uint32_t check = get32(dta_seg, (uint16_t)(dta_off + 2));
    if (!s->active || s->lfn || s->fcb || check != (s->generation ^ UINT32_C(0x44544121)) ||
        get32(dta_seg, (uint16_t)(dta_off + 6)) != ~check) return NULL;
    return s;
}

static Search *lfn_search(uint16_t handle)
{
    Search *s = searches + (handle & (SEARCH_SLOTS - 1));
    return s->active && s->lfn && s->handle == handle ? s : NULL;
}

static void put_find_time(uint16_t seg, uint16_t off, struct timespec ts, bool packed)
{
    if (packed) {
        uint16_t tt, dt;
        pack_time(ts, &tt, &dt);
        put64(seg, off, tt | (uint32_t)dt << 16);
    } else put64(seg, off, to_filetime(ts));
}

static void fill_find(const Entry *e, bool lfn, bool packed)
{
    uint64_t size = e->st.st_size > 0 && !S_ISDIR(e->st.st_mode) ? (uint64_t)e->st.st_size : 0;
    if (!lfn) {
        uint16_t tt, dt;
        pack_time(e->st.st_mtim, &tt, &dt);
        wr8(dta_seg, (uint16_t)(dta_off + 21), (uint8_t)e->attr);
        wr16(dta_seg, (uint16_t)(dta_off + 22), tt);
        wr16(dta_seg, (uint16_t)(dta_off + 24), dt);
        put32(dta_seg, (uint16_t)(dta_off + 26), size > UINT32_MAX ? UINT32_MAX : (uint32_t)size);
        zero_mem(dta_seg, (uint16_t)(dta_off + 30), 14);
        write_string(dta_seg, (uint16_t)(dta_off + 30), e->alias, 13);
    } else {
        uint16_t seg = cpu.es, off = cpu.di;
        zero_mem(seg, off, 318);
        put32(seg, off, e->attr);
        put_find_time(seg, (uint16_t)(off + 4), e->birth, packed);
        put_find_time(seg, (uint16_t)(off + 12), e->st.st_atim, packed);
        put_find_time(seg, (uint16_t)(off + 20), e->st.st_mtim, packed);
        put32(seg, (uint16_t)(off + 28), (uint32_t)(size >> 32));
        put32(seg, (uint16_t)(off + 32), (uint32_t)size);
        write_string(seg, (uint16_t)(off + 44), e->dos, 260);
        /* Like Windows NT: no short name when the long name already is one,
         * ignoring case. VC copies under the short name when it has one, so
         * hello.txt would otherwise arrive as HELLO.TXT. */
        if (strcasecmp(e->dos, e->alias))
            write_string(seg, (uint16_t)(off + 304), e->alias, 14);
    }
}

static int advance_search(Search *s, bool packed)
{
    while (s->next < s->count) {
        Entry *e = s->entries + s->next++;
        if (!e->have_stat || !attributes_match(e->attr, s->mask, s->lfn)) continue;
        bool matches = s->lfn ? (long_match(s->pattern, e->dos) || long_match(s->pattern, e->alias)) :
                               classic_match(s->pattern, e->alias);
        if (!matches) continue;
        fill_find(e, s->lfn, packed);
        return 0;
    }
    if (!s->lfn) release_search(s);
    return 18;
}

static int find_first(bool lfn)
{
    if (lfn && cpu.si > 1) return 1;
    char dos[DOS_PATH_MAX], dir[PATH_MAX], pattern[DOS_NAME_MAX + 1];
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    const char *name = dos;
    unsigned drive;
    error = path_drive(&name, &drive);
    if (error || (error = split_pattern(dos, dir, pattern))) return error;
    if (!lfn) {
        Search *old = classic_search();
        if (old) release_search(old);
    }
    unsigned slot;
    for (slot = 0; slot < SEARCH_SLOTS && searches[slot].active; ++slot) {}
    if (slot == SEARCH_SLOTS) return 4;
    Search *s = searches + slot;
    error = snapshot(dir, drive, &s->entries, &s->count);
    if (error) return error;
    s->next = 0; s->mask = cpu.c.x; s->lfn = lfn; s->fcb = false; s->active = true;
    s->generation = ++search_generation;
    s->handle = (uint16_t)(0x8000 | ((s->generation & 0x7f) << 7) | slot);
    if (s->handle == 0xffff) {
        s->generation = ++search_generation;
        s->handle = (uint16_t)(0x8000 | ((s->generation & 0x7f) << 7) | slot);
    }
    strcpy(s->pattern, pattern);
    if (!lfn) {
        zero_mem(dta_seg, dta_off, 21);
        wr16(dta_seg, dta_off, (uint16_t)(slot + 1));
        uint32_t check = s->generation ^ UINT32_C(0x44544121);
        put32(dta_seg, (uint16_t)(dta_off + 2), check);
        put32(dta_seg, (uint16_t)(dta_off + 6), ~check);
    }
    error = advance_search(s, cpu.si == 1);
    if (error) { release_search(s); return error; }
    cpu.a.x = lfn ? s->handle : 0;
    return 0;
}

static int find_next(bool lfn)
{
    Search *s = lfn ? lfn_search(cpu.b.x) : classic_search();
    if (!s) return lfn ? 6 : 18;
    if (lfn && cpu.si > 1) return 1;
    int error = advance_search(s, cpu.si == 1);
    if (!error) cpu.a.x = 0;
    return error;
}

static DosHandle *get_handle(unsigned h)
{
    if (h >= DOS_HANDLES || (handles[h].fd < 0 && handles[h].device == DEV_NONE)) return NULL;
    return handles + h;
}

static int unused_handle(void)
{
    for (int i = 5; i < DOS_HANDLES; ++i) if (!get_handle((unsigned)i)) return i;
    return -1;
}

static bool inode_is_open(const struct stat *st)
{
    for (unsigned i = 0; i < DOS_HANDLES; ++i) {
        DosHandle *h = get_handle(i);
        struct stat held;
        if (h && !h->device && !fstat(h->fd, &held) &&
            held.st_dev == st->st_dev && held.st_ino == st->st_ino) return true;
    }
    if (door_root_fd >= 0) {
        for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
            struct stat held;
            if (lease->fd >= 0 && !fstat(lease->fd, &held) &&
                held.st_dev == st->st_dev && held.st_ino == st->st_ino) return true;
        }
    }
    return false;
}

static int close_handle(unsigned n)
{
    DosHandle *h = get_handle(n);
    if (!h) return 6;
    if (h->references > 1) { --h->references; return 0; }
    struct stat st;
    bool unlinked = h->fd >= 0 && !fstat(h->fd, &st) && !st.st_nlink;
    int error = h->fd >= 0 && close(h->fd) < 0 ? dos_errno(errno) : 0;
    memset(h, 0, sizeof(*h)); h->fd = -1;
    if (unlinked && !inode_is_open(&st)) {
        if (door_root_fd >= 0) quota_release_entry(&st);
        forget_birth(&st);
    }
    return error;
}

/* DOS 2's COMMAND.COM moves SFT indices inside PSP:18h to implement
 * redirection. Always read those bytes, not a cached host-side mapping.
 * The standalone file tests (and DOS 1 FCB clients) need no PSP. */
static bool has_jft(uint16_t psp)
{
    return psp && rd16(psp, 0) == 0x20CD;
}

static uint32_t jft_entry(uint16_t psp, unsigned number)
{
    unsigned count = rd16(psp, 0x32);
    uint32_t base = lin(rd16(psp, 0x36), rd16(psp, 0x34));
    if (count > DOS_HANDLES || number >= count || !guest_span(base, count)) return UINT32_MAX;
    return base + number;
}

static unsigned system_handle(unsigned number)
{
    if (!has_jft(fcb_process)) return number;
    uint32_t at = jft_entry(fcb_process, number);
    return at == UINT32_MAX ? DOS_HANDLES : mem[at];
}

static DosHandle *dos_handle(unsigned number)
{
    return get_handle(system_handle(number));
}

static int unused_jfn(void)
{
    if (!has_jft(fcb_process)) return unused_handle();
    for (unsigned i = 0; i < DOS_HANDLES; ++i) {
        uint32_t at = jft_entry(fcb_process, i);
        if (at == UINT32_MAX) break;
        if (mem[at] == 0xFF) return (int)i;
    }
    return -1;
}

static int publish_handle(unsigned slot)
{
    if (!has_jft(fcb_process)) return 0;
    int number = unused_jfn();
    if (number < 0) { close_handle(slot); return 4; }
    mem[jft_entry(fcb_process, (unsigned)number)] = (uint8_t)slot;
    cpu.a.x = (uint16_t)number;
    return 0;
}

static int close_dos_handle(unsigned number)
{
    unsigned slot = system_handle(number);
    DosHandle *h = get_handle(slot);
    if (!h) return 6;
    if (!h->device) {
        closed_file_owner = fcb_process;
        snprintf(closed_file_path, sizeof closed_file_path, "%s", h->path);
    }
    if (has_jft(fcb_process)) mem[jft_entry(fcb_process, number)] = 0xFF;
    return close_handle(slot);
}

int dos_fs_take_closed_file(uint16_t owner, char *path, size_t capacity)
{
    int found = owner == closed_file_owner && *closed_file_path && strlen(closed_file_path) < capacity;
    if (found) memcpy(path, closed_file_path, strlen(closed_file_path) + 1);
    closed_file_path[0] = 0;
    return found;
}

void dos_fs_inherit_process(uint16_t child, uint16_t parent)
{
    if (!parent || !has_jft(parent)) return;
    for (unsigned i = 0; i < 20; ++i) {
        uint32_t source = jft_entry(parent, i), target = jft_entry(child, i);
        unsigned slot = source == UINT32_MAX ? DOS_HANDLES : mem[source];
        DosHandle *h = get_handle(slot);
        if (target == UINT32_MAX) break;
        if (!h || (h->mode & 0x80)) mem[target] = 0xFF;
        else { mem[target] = (uint8_t)slot; ++h->references; }
    }
}

void dos_fs_set_process(uint16_t psp)
{
    fcb_process = psp;
}

void dos_fs_close_process(uint16_t psp)
{
    if (has_jft(psp))
        for (unsigned i = 0; i < DOS_HANDLES; ++i) {
            uint32_t at = jft_entry(psp, i);
            if (at == UINT32_MAX) break;
            if (get_handle(mem[at])) close_handle(mem[at]);
            mem[at] = 0xFF;
        }
    for (unsigned i = 0; i < DOS_HANDLES; i++)
        if (handles[i].fcb_id && handles[i].fcb_owner == psp) close_handle(i);
    for (unsigned i = 0; i < SEARCH_SLOTS; i++)
        if (searches[i].active && searches[i].fcb && searches[i].owner == psp)
            release_search(searches + i);
    dos_fs_release_process_paths(psp);
}

void dos_fs_release_process_paths(uint16_t psp)
{
    DosPathLease *lease = path_leases;
    while (lease) {
        DosPathLease *next = lease->next;
        if (lease->bound && lease->psp == psp) dos_fs_release_path(lease);
        lease = next;
    }
}

static void forget_birth(const struct stat *st)
{
    BirthTime **p = &birth_times;
    while (*p) {
        BirthTime *b = *p;
        if (b->dev == st->st_dev && b->ino == st->st_ino) {
            *p = b->next; free(b);
        } else p = &b->next;
    }
}

static bool valid_access(unsigned mode)
{
    return (mode & 15) <= 2 && ((mode >> 4) & 7) <= 4 && !(mode & 0x1f00);
}

static unsigned access_bits(unsigned mode)
{
    return (mode & 3) == 0 ? 1 : (mode & 3) == 1 ? 2 : 3;
}

static unsigned denied_access(unsigned mode)
{
    switch ((mode >> 4) & 7) {
    case 1: return 3;
    case 2: return 2;
    case 3: return 1;
    /* Compatibility mode permits reopenings within this DOS process. */
    default: return 0;
    }
}

static int check_sharing(const struct stat *st, unsigned mode, bool truncate)
{
    for (unsigned i = 0; i < DOS_HANDLES; ++i) {
        DosHandle *h = get_handle(i);
        struct stat held;
        if (!h || h->device || fstat(h->fd, &held) < 0) continue;
        if (held.st_dev != st->st_dev || held.st_ino != st->st_ino) continue;
        if (((access_bits(mode) | (truncate ? 2u : 0u)) & denied_access(h->mode)) ||
            (access_bits(h->mode) & denied_access(mode))) return 32;
    }
    return 0;
}

static int open_path(const char *path, unsigned drive, unsigned mode,
                     unsigned attributes, unsigned action)
{
    if (!valid_access(mode)) return 12;
    if ((action & ~0x13u) || (action & 15) > 2) return 12;
    if (attributes & ATTR_VOLUME) return 5;
    DosPathLease *lease;
    int error = leased_host(path, &lease);
    if (error) return error;
    struct stat st;
    error = file_stat(path, &st);
    if (lease && error && !(error == 2 && lease->recreate)) return 5;
    if (error && error != 2) return error;
    bool exists = !error;
    unsigned if_exists = action & 15, if_missing = (action >> 4) & 15;
    if (exists && !if_exists) return 80;
    if (!exists && !if_missing) return 2;
    if (exists && (!S_ISREG(st.st_mode) ||
                  (!(st.st_mode & S_IWUSR) && ((mode & 3) != 0 || if_exists == 2)))) return 5;
    if (exists && (error = check_sharing(&st, mode, if_exists == 2))) return error;
    int slot = unused_handle();
    if (slot < 0) return 4;
    int flags = (mode & 3) == 0 ? O_RDONLY : (mode & 3) == 1 ? O_WRONLY : O_RDWR;
    if (lease && if_exists == 2 && (mode & 3) == 0) flags = O_RDWR;
    flags |= O_CLOEXEC;
    if (lease) {
        flags |= O_NONBLOCK;
        if (lease->parts[lease->count - 1].entry_type != S_IFLNK) flags |= O_NOFOLLOW;
    }
    if (!exists) flags |= O_CREAT | O_EXCL;
    if (exists && if_exists == 2 && !lease) flags |= O_TRUNC;
    int fd;
    do { fd = fs_open(path, flags, 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        error = dos_errno(errno);
        return lease && error != 39 ? 5 : error;
    }
    if (fstat(fd, &st) < 0) { error = dos_errno(errno); close(fd); return error; }
    /* Check the opened descriptor against the pathname as it exists now,
     * not the original inode: external atomic replacement is legitimate.
     * Keep truncation deferred until type, path components and aliases pass. */
    if (lease) {
        struct stat current;
        if (lease_validate(lease, lease->count) || fs_stat(path, &current, false) ||
            !S_ISREG(st.st_mode) || st.st_dev != current.st_dev || st.st_ino != current.st_ino ||
            (!(st.st_mode & S_IWUSR) && ((mode & 3) != 0 || if_exists == 2))) {
            close(fd); return 5;
        }
        if ((error = check_sharing(&st, mode, if_exists == 2))) { close(fd); return error; }
    }
    if (lease && if_exists == 2 && quota_truncate(fd, 0)) {
        error = dos_errno(errno); close(fd); return error;
    }
    if (!exists) forget_birth(&st); /* an inode may have been recycled */
    if ((!exists || if_exists == 2) && (attributes & ATTR_RO) &&
        fchmod(fd, st.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)) < 0) {
        error = dos_errno(errno); close(fd); return error;
    }
    DosHandle *h = handles + slot;
    h->fd = fd; h->device = DEV_NONE; h->access = mode & 3; h->mode = mode;
    h->references = 1;
    h->drive = drive;
    strcpy(h->path, path);
    if (lease) lease->recreate = false;
    cpu.a.x = (uint16_t)slot;
    cpu.c.x = !exists ? 2 : if_exists == 2 ? 3 : 1;
    return 0;
}

static unsigned named_device(const char *name)
{
    const char *leaf = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') leaf = p + 1;
    size_t stem = strcspn(leaf, ".");
    return stem == 3 && !strncasecmp(leaf, "NUL", 3) ? DEV_NUL :
           stem == 3 && !strncasecmp(leaf, "CON", 3) ? DEV_CON : DEV_NONE;
}

static int open_file(uint16_t off, unsigned mode, unsigned attributes, unsigned action, bool extended)
{
    if (!valid_access(mode)) return 12;
    if ((action & ~0x13u) || (action & 15) > 2) return 12;
    char dos[DOS_PATH_MAX], path[PATH_MAX];
    int error = read_string(cpu.ds, off, dos, sizeof(dos));
    if (error) return error;
    /* DOS device names are case-insensitive, may have an ignored extension,
     * and can end in a colon. Device recognition precedes file creation, so
     * redirection can never create/truncate a host file named NUL. */
    size_t length = strlen(dos);
    bool device_colon = length > 2 && dos[length - 1] == ':';
    if (device_colon) dos[length - 1] = 0;
    const char *name = dos;
    unsigned drive;
    error = path_drive(&name, &drive);
    if (error) return error;
    unsigned device = named_device(name);
    if (device_colon && !device) return 3;
    /* Reserve capacity before open_path can create or truncate anything.
     * The PSP table can fill while the larger system table still has room. */
    if (unused_jfn() < 0) return 4;
    if (device) {
        int slot = unused_handle();
        if (slot < 0) return 4;
        handles[slot] = (DosHandle){.fd = -1, .device = (int)device,
            .access = (int)(mode & 3), .mode = mode, .drive = drive, .references = 1};
        cpu.a.x = (uint16_t)slot;
        if (extended) cpu.c.x = 1;
        return publish_handle((unsigned)slot);
    }
    if ((error = resolve_path(dos, path, (action & 0x10) != 0))) return error;
    uint16_t saved_cx = cpu.c.x;
    error = open_path(path, drive, mode, attributes, action);
    if (!error) error = publish_handle(cpu.a.x);
    if (!extended) cpu.c.x = saved_cx;
    return error;
}

/* ---- DOS 1.x file control blocks ----------------------------------------- */

static uint16_t fcb_offset(uint16_t seg, uint16_t off)
{
    return rd8(seg, off) == 0xff ? (uint16_t)(off + 7) : off;
}

/* DOS 1.x searches keep continuation state in the caller's FCB, not the
 * output DTA. A generation-tagged token supports copied FCBs without trusting
 * guest pointers, and cannot resume another process's search. */
static Search *fcb_search(uint16_t seg, uint16_t off)
{
    unsigned slot = rd16(seg, (uint16_t)(off + 12));
    if (!slot || slot > SEARCH_SLOTS) return NULL;
    Search *s = searches + slot - 1;
    uint32_t check = get32(seg, (uint16_t)(off + 14));
    if (!s->active || !s->fcb || s->owner != fcb_process ||
        check != (s->generation ^ UINT32_C(0x46434221)) ||
        get32(seg, (uint16_t)(off + 18)) != ~check) return NULL;
    return s;
}

static void fcb_find_entry(const Entry *e, const Search *s, bool extended)
{
    uint16_t out = dta_off;
    zero_mem(dta_seg, out, extended ? 40 : 33);
    if (extended) {
        wr8(dta_seg, out, 0xff);
        wr8(dta_seg, (uint16_t)(out + 6), (uint8_t)s->mask);
        out = (uint16_t)(out + 7);
    }
    wr8(dta_seg, out++, (uint8_t)(s->drive + 1));
    for (unsigned i = 0; i < 11; i++) wr8(dta_seg, (uint16_t)(out + i), ' ');
    const char *dot = e->alias[0] == '.' ? NULL : strchr(e->alias, '.');
    size_t name = dot ? (size_t)(dot - e->alias) : strlen(e->alias);
    for (size_t i = 0; i < name && i < 8; i++)
        wr8(dta_seg, (uint16_t)(out + i), (uint8_t)e->alias[i]);
    if (dot)
        for (size_t i = 0; dot[i + 1] && i < 3; i++)
            wr8(dta_seg, (uint16_t)(out + 8 + i), (uint8_t)dot[i + 1]);
    wr8(dta_seg, (uint16_t)(out + 11), (uint8_t)e->attr);
    uint16_t tt, dt;
    pack_time(e->st.st_mtim, &tt, &dt);
    wr16(dta_seg, (uint16_t)(out + 22), tt);
    wr16(dta_seg, (uint16_t)(out + 24), dt);
    uint64_t size = S_ISDIR(e->st.st_mode) || e->st.st_size < 0 ? 0 : (uint64_t)e->st.st_size;
    put32(dta_seg, (uint16_t)(out + 28), size > UINT32_MAX ? UINT32_MAX : (uint32_t)size);
}

static uint8_t fcb_find(uint16_t seg, uint16_t off, bool first, bool extended)
{
    Search *s = fcb_search(seg, off);
    if (first) {
        if (s) release_search(s);
        unsigned d = rd8(seg, off), drive = d ? d - 1 : current_drive;
        if (!drive_present(drive)) return 0xff;
        char pattern[13];
        size_t n = 0;
        for (unsigned part = 0; part < 2; part++) {
            unsigned width = part ? 3 : 8, begin = part ? 9 : 1, used = width;
            while (used && rd8(seg, (uint16_t)(off + begin + used - 1)) == ' ') used--;
            if (!part && !used) return 0xff;
            if (part && used) pattern[n++] = '.';
            for (unsigned i = 0; i < used; i++) {
                uint8_t c = rd8(seg, (uint16_t)(off + begin + i));
                if (!c || strchr("\\/:\"<>|", c)) return 0xff;
                pattern[n++] = (char)c;
            }
        }
        pattern[n] = 0;
        /* DOS directory lookup recognizes literal device names without
         * making them entries in wildcard directory listings. COMMAND's
         * COPY still does this FCB lookup after opening/IOCTL-ing CON. */
        if (!strpbrk(pattern, "?*") && named_device(pattern)) {
            Entry device = {.attr = 0x40, .have_stat = true, .st = {.st_mode = S_IFCHR}};
            memcpy(device.alias, pattern, n + 1);
            Search result = {.drive = drive,
                .mask = extended ? rd8(seg, (uint16_t)(off - 1)) : 0};
            zero_mem(seg, (uint16_t)(off + 12), 10); /* A device has no find-next. */
            fcb_find_entry(&device, &result, extended);
            return 0;
        }
        unsigned slot;
        for (slot = 0; slot < SEARCH_SLOTS && searches[slot].active; slot++) {}
        if (slot == SEARCH_SLOTS) return 0xff;
        s = searches + slot;
        if (snapshot(drives[drive].cwd, drive, &s->entries, &s->count)) return 0xff;
        s->next = 0; s->active = true; s->fcb = true; s->lfn = false;
        s->owner = fcb_process; s->drive = drive;
        s->mask = extended ? rd8(seg, (uint16_t)(off - 1)) : 0;
        s->generation = ++search_generation;
        strcpy(s->pattern, pattern);
        wr16(seg, (uint16_t)(off + 12), (uint16_t)(slot + 1));
        uint32_t check = s->generation ^ UINT32_C(0x46434221);
        put32(seg, (uint16_t)(off + 14), check);
        put32(seg, (uint16_t)(off + 18), ~check);
    }
    if (!s) return 0xff;
    while (s->next < s->count) {
        Entry *e = s->entries + s->next++;
        if (!e->have_stat || !attributes_match(e->attr, s->mask, false) ||
            !classic_match(s->pattern, e->alias)) continue;
        fcb_find_entry(e, s, extended);
        return 0;
    }
    release_search(s);
    return 0xff;
}

static int fcb_path(uint16_t seg, uint16_t off, char *host, bool missing, unsigned *drive)
{
    unsigned d = rd8(seg, off);
    *drive = d ? d - 1 : current_drive;
    if (!drive_present(*drive)) return 15;
    char path[16];
    size_t n = 0;
    path[n++] = (char)('A' + *drive);
    path[n++] = ':';
    for (unsigned part = 0; part < 2; part++) {
        unsigned width = part ? 3 : 8, begin = part ? 9 : 1;
        unsigned used = width;
        while (used && rd8(seg, (uint16_t)(off + begin + used - 1)) == ' ') used--;
        if (!part && !used) return 2;
        if (part && used) path[n++] = '.';
        for (unsigned i = 0; i < used; i++) {
            uint8_t c = rd8(seg, (uint16_t)(off + begin + i));
            if (!c || strchr("?*\\/:\"<>|", c)) return 2;
            path[n++] = (char)c;
        }
    }
    path[n] = 0;
    return resolve_path(path, host, missing);
}

static DosHandle *fcb_handle(uint16_t seg, uint16_t off, unsigned *slot)
{
    *slot = rd16(seg, (uint16_t)(off + 28));
    DosHandle *h = get_handle(*slot);
    return h && h->fcb_id && h->fcb_id == get32(seg, (uint16_t)(off + 24)) &&
           rd16(seg, (uint16_t)(off + 30)) == 0xfcb1 ? h : NULL;
}

static void fcb_metadata(uint16_t seg, uint16_t off, const struct stat *st)
{
    put32(seg, (uint16_t)(off + 16), st->st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)st->st_size);
    uint16_t time, date;
    pack_time(st->st_mtim, &time, &date);
    wr16(seg, (uint16_t)(off + 20), date);
    wr16(seg, (uint16_t)(off + 22), time);
}

static uint8_t fcb_open(uint16_t seg, uint16_t off, bool create)
{
    char path[PATH_MAX];
    unsigned drive;
    int error = fcb_path(seg, off, path, create, &drive);
    if (error) return 0xff;
    /* FCB OPEN has no access-mode argument. Read-only files must still be
     * readable; an attempted write then reports the FCB write error. */
    error = open_path(path, drive, 2, 0, create ? 0x12 : 1);
    if (error == 5 && !create) error = open_path(path, drive, 0, 0, 1);
    if (error) return 0xff;
    unsigned slot = cpu.a.x;
    DosHandle *h = handles + slot;
    struct stat st;
    if (fstat(h->fd, &st)) { close_handle(slot); return 0xff; }
    h->fcb_id = ++fcb_sequence;
    if (!h->fcb_id) h->fcb_id = ++fcb_sequence;
    h->fcb_owner = fcb_process;
    wr8(seg, off, (uint8_t)(drive + 1));
    wr16(seg, (uint16_t)(off + 12), 0); /* current 128-record block */
    wr16(seg, (uint16_t)(off + 14), 128);
    fcb_metadata(seg, off, &st);
    put32(seg, (uint16_t)(off + 24), h->fcb_id);
    wr16(seg, (uint16_t)(off + 28), (uint16_t)slot);
    wr16(seg, (uint16_t)(off + 30), 0xfcb1);
    wr8(seg, (uint16_t)(off + 32), 0);
    return 0;
}

static uint8_t fcb_close(uint16_t seg, uint16_t off)
{
    unsigned slot;
    DosHandle *h = fcb_handle(seg, off, &slot);
    if (!h) return 0xff;
    struct stat st;
    if (!fstat(h->fd, &st)) fcb_metadata(seg, off, &st);
    zero_mem(seg, (uint16_t)(off + 24), 8);
    return close_handle(slot) ? 0xff : 0;
}

static int fcb_pattern(uint16_t seg, uint16_t off, char pattern[13])
{
    size_t count = 0;
    for (unsigned part = 0; part < 2; part++) {
        unsigned width = part ? 3 : 8, start = part ? 9 : 1;
        unsigned used = width;
        while (used && rd8(seg, (uint16_t)(off + start + used - 1)) == ' ') used--;
        if (!part && !used) return 2;
        if (part && used) pattern[count++] = '.';
        for (unsigned i = 0; i < used; i++) {
            uint8_t c = rd8(seg, (uint16_t)(off + start + i));
            if (!c || strchr("\\/:\"<>|", c)) return 2;
            pattern[count++] = (char)c;
        }
    }
    pattern[count] = 0;
    return 0;
}

static int fcb_rename_target(uint16_t seg, uint16_t off, unsigned drive,
                             const char *alias, char target[PATH_MAX])
{
    char field[11];
    memset(field, ' ', sizeof field);
    const char *dot = strchr(alias, '.');
    size_t name = dot ? (size_t)(dot - alias) : strlen(alias);
    if (name > 8 || (dot && strlen(dot + 1) > 3)) return 2;
    memcpy(field, alias, name);
    if (dot) memcpy(field + 8, dot + 1, strlen(dot + 1));
    for (unsigned i = 0; i < sizeof field; i++) {
        uint8_t c = rd8(seg, (uint16_t)(off + 17 + i));
        if (!c || strchr("*\\/:.\"<>|", c)) return 2;
        /* FCB rename copies exactly this padded 8.3 position, not a
         * variable-length glob capture. In particular '?' can copy blank. */
        if (c != '?') field[i] = (char)c;
    }
    char dos[16] = {(char)('A' + drive), ':'};
    size_t count = 2;
    for (unsigned part = 0; part < 2; part++) {
        unsigned width = part ? 3 : 8, start = part ? 8 : 0, used = width;
        while (used && field[start + used - 1] == ' ') used--;
        if (!part && !used) return 2;
        if (part && used) dos[count++] = '.';
        memcpy(dos + count, field + start, used);
        count += used;
    }
    dos[count] = 0;
    return resolve_path(dos, target, true);
}

static uint8_t fcb_mutate(uint16_t seg, uint16_t off, bool rename, bool extended)
{
    unsigned raw = rd8(seg, off), drive = raw ? raw - 1 : current_drive;
    if (!drive_present(drive)) return 0xff;
    if (rename) {
        raw = rd8(seg, (uint16_t)(off + 16));
        /* An unqualified new name belongs to the old file's drive, even
         * when a drive-qualified source is not on the current drive. */
        if (raw && raw - 1 != drive) return 0xff;
    }
    char pattern[13];
    if (fcb_pattern(seg, off, pattern)) return 0xff;
    Entry *entries;
    size_t count;
    if (snapshot(drives[drive].cwd, drive, &entries, &count)) return 0xff;
    unsigned mask = extended ? rd8(seg, (uint16_t)(off - 1)) : 0;
    uint8_t status = 0xff;
    for (size_t i = 0; i < count; i++) {
        Entry *entry = entries + i;
        if (!entry->have_stat || !S_ISREG(entry->st.st_mode) ||
            !attributes_match(entry->attr, (uint16_t)mask, false) ||
            !classic_match(pattern, entry->alias)) continue;
        char path[PATH_MAX], target[PATH_MAX];
        struct stat st;
        int error = join_path(drives[drive].cwd, entry->host, path, sizeof path);
        if (!error) error = file_stat(path, &st);
        if (!error && (!S_ISREG(st.st_mode) || !(st.st_mode & S_IWUSR))) error = 5;
        if (!error && rename) {
            error = fcb_rename_target(seg, off, drive, entry->alias, target);
            if (!error) error = rename_paths(path, target, "");
        } else if (!error) {
            if (inode_is_open(&st)) error = 5;
            else if (fs_unlink(path, false)) error = dos_errno(errno);
            else forget_birth(&st);
        }
        if (error) { status = 0xff; break; }
        status = 0;
    }
    free_entries(entries, count);
    return status;
}

static uint32_t fcb_record(uint16_t seg, uint16_t off, bool random, uint16_t size)
{
    if (!random) return rd16(seg, (uint16_t)(off + 12)) * 128u + rd8(seg, (uint16_t)(off + 32));
    uint32_t record = get32(seg, (uint16_t)(off + 33));
    return size < 64 ? record : record & 0xffffffu;
}

static void fcb_position(uint16_t seg, uint16_t off, uint32_t record)
{
    wr16(seg, (uint16_t)(off + 12), (uint16_t)(record / 128));
    wr8(seg, (uint16_t)(off + 32), (uint8_t)(record % 128));
}

/* Sequential and random record I/O share the existing DOS handle table and
 * DTA. A short final read is zero-padded (AL=3); no bytes at EOF is AL=1.
 * Single random calls never advance the random-record field: GW-BASIC's
 * ACCFIL advances it itself. Block calls advance it by completed records. */
static uint8_t fcb_transfer(uint16_t seg, uint16_t off, uint8_t function, uint16_t *count)
{
    bool write = function == 0x15 || function == 0x22 || function == 0x28;
    bool random = function >= 0x21, block = function >= 0x27;
    unsigned slot;
    DosHandle *h = fcb_handle(seg, off, &slot);
    if (!h || h->device) { *count = 0; return 0xff; }
    uint16_t size = rd16(seg, (uint16_t)(off + 14));
    if (!size) { *count = 0; return 0xff; }
    uint32_t record = fcb_record(seg, off, random, size);
    uint32_t requested = block ? *count : 1;
    uint64_t pos = (uint64_t)record * size;
    if (write && h->access == 0) { *count = 0; return 1; }
    if (!requested) {
        *count = 0;
        if (write && quota_truncate(h->fd, (off_t)pos)) return 1;
        struct stat st;
        if (!fstat(h->fd, &st)) fcb_metadata(seg, off, &st);
        return 0;
    }
    if (requested * size > 0x10000u - dta_off) { *count = 0; return 2; }
    uint8_t *buffer = malloc(size);
    if (!buffer) { *count = 0; return 0xff; }
    uint32_t done = 0;
    uint8_t status = 0;
    while (done < requested) {
        uint16_t dma = (uint16_t)(dta_off + done * size);
        if (write)
            for (unsigned i = 0; i < size; i++) buffer[i] = rd8(dta_seg, (uint16_t)(dma + i));
        ssize_t n;
        do {
            n = write ? quota_write(h->fd, buffer, size, (off_t)pos, true) :
                        pread(h->fd, buffer, size, (off_t)pos);
        } while (n < 0 && errno == EINTR);
        if (n < 0 || (write && n != size)) { status = 1; break; }
        if (!n) { status = 1; break; }
        if (!write) {
            memset(buffer + n, 0, size - (size_t)n);
            for (unsigned i = 0; i < size; i++) wr8(dta_seg, (uint16_t)(dma + i), buffer[i]);
        }
        done++;
        pos += size;
        if (n < size) { status = 3; break; }
    }
    free(buffer);
    uint32_t next = record + (block || !random ? done : 0);
    fcb_position(seg, off, next);
    if (block) put32(seg, (uint16_t)(off + 33), next);
    *count = (uint16_t)done;
    if (write) {
        struct stat st;
        if (!fstat(h->fd, &st)) fcb_metadata(seg, off, &st);
    }
    return status;
}

static bool fcb_separator(uint8_t c)
{
    return c == ' ' || c == '\t' || c == ':' || c == ';' || c == ',' || c == '=' || c == '+';
}

static bool fcb_end(uint8_t c)
{
    return c <= ' ' || strchr("\"/\\[]:;=,+<>|", c) != NULL;
}

static uint8_t fcb_parse(void)
{
    uint16_t si = cpu.si, off = cpu.di;
    uint8_t options = cpu.a.l, status = 0;
    unsigned scanned = 0;
    if (options & 1)
        while (scanned < 65536 && fcb_separator(rd8(cpu.ds, si))) { si++; scanned++; }
    uint8_t c = rd8(cpu.ds, si);
    if (c && rd8(cpu.ds, (uint16_t)(si + 1)) == ':') {
        unsigned drive = (unsigned)(cp866_upper(c) - 'A');
        if (!drive_present(drive)) { cpu.si = si; return 0xff; }
        wr8(cpu.es, off, (uint8_t)(drive + 1));
        si += 2;
    } else if (!(options & 2)) wr8(cpu.es, off, 0);
    if (!(options & 8))
        for (unsigned i = 0; i < 3; i++) wr8(cpu.es, (uint16_t)(off + 9 + i), ' ');
    for (unsigned part = 0; part < 2; part++) {
        unsigned width = part ? 3 : 8, field = part ? 9 : 1, used = 0;
        c = rd8(cpu.ds, si);
        bool present = !fcb_end(c) && c != '.';
        if (present || part || !(options & 4))
            for (unsigned i = 0; i < width; i++) wr8(cpu.es, (uint16_t)(off + field + i), ' ');
        while (scanned++ < 65536 && !fcb_end(c) && c != '.') {
            if (c == '*' || c == '?') status = 1;
            if (c == '*')
                while (used < width) wr8(cpu.es, (uint16_t)(off + field + used++), '?');
            else if (used < width)
                wr8(cpu.es, (uint16_t)(off + field + used++), cp866_upper(c));
            si++;
            c = rd8(cpu.ds, si);
        }
        if (part || c != '.') break;
        si++;
    }
    wr16(cpu.es, (uint16_t)(off + 12), 0);
    wr16(cpu.es, (uint16_t)(off + 14), 0);
    cpu.si = si;
    return scanned >= 65536 ? 0xff : status;
}

static void fcb_call(uint8_t function)
{
    Cpu saved = cpu;
    uint16_t seg = cpu.ds, off = fcb_offset(seg, cpu.d.x), count = cpu.c.x;
    uint8_t status = 0xff;
    if (function == 0x29) {
        status = fcb_parse();
        saved.si = cpu.si;
    } else if (function == 0x0f || function == 0x16) status = fcb_open(seg, off, function == 0x16);
    else if (function == 0x10) status = fcb_close(seg, off);
    else if (function == 0x11 || function == 0x12)
        status = fcb_find(seg, off, function == 0x11, rd8(seg, cpu.d.x) == 0xff);
    else if (function == 0x13 || function == 0x17)
        status = fcb_mutate(seg, off, function == 0x17, rd8(seg, cpu.d.x) == 0xff);
    else if (function == 0x14 || function == 0x15 || function == 0x21 || function == 0x22 ||
             function == 0x27 || function == 0x28) {
        status = fcb_transfer(seg, off, function, &count);
        if (function >= 0x27) saved.c.x = count;
    } else if (function == 0x24) {
        put32(seg, (uint16_t)(off + 33), fcb_record(seg, off, false, 0));
        status = 0;
    } else if (function == 0x23) {
        char path[PATH_MAX];
        unsigned drive;
        if (!fcb_path(seg, off, path, false, &drive)) {
            struct stat st;
            if (!file_stat(path, &st) && S_ISREG(st.st_mode)) {
                uint16_t size = rd16(seg, (uint16_t)(off + 14));
                if (size) { put32(seg, (uint16_t)(off + 33), (uint32_t)(((uint64_t)st.st_size + size - 1) / size)); status = 0; }
            }
        }
    }
    cpu = saved;
    cpu.a.l = status;
    cpu.cf = 0; /* FCB errors are AL statuses, not CF/AX DOS 2.x errors. */
}

static int read_file(void)
{
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    if (h->access == 1) return 5;
    if (!cpu.c.x) { cpu.a.x = 0; return 0; }
    if (h->device == DEV_IN || h->device == DEV_CON) {
        cpu.cf = 0;
        (void)dos_con_int21();
        return cpu.cf ? cpu.a.x : 0;
    }
    if (h->device != DEV_NONE) { cpu.a.x = 0; return 0; }
    uint8_t buffer[UINT16_MAX];
    ssize_t n;
    do { n = read(h->fd, buffer, cpu.c.x); } while (n < 0 && errno == EINTR);
    if (n < 0) return dos_errno(errno);
    for (ssize_t i = 0; i < n; ++i) wr8(cpu.ds, (uint16_t)(cpu.d.x + i), buffer[i]);
    cpu.a.x = (uint16_t)n;
    return 0;
}

static int write_file(void)
{
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    if (h->access == 0) return 5;
    uint16_t count = cpu.c.x;
    if (!count) {
        if (h->device == DEV_NONE) {
            off_t pos = lseek(h->fd, 0, SEEK_CUR);
            if (pos < 0 || quota_truncate(h->fd, pos) < 0) return dos_errno(errno);
            if ((h->mode & 0x4000) && fsync(h->fd) < 0) return dos_errno(errno);
        }
        cpu.a.x = 0;
        return 0;
    }
    uint8_t buffer[UINT16_MAX];
    for (unsigned i = 0; i < count; ++i) buffer[i] = rd8(cpu.ds, (uint16_t)(cpu.d.x + i));
    if (h->device != DEV_NONE) {
        if (h->device == DEV_OUT || h->device == DEV_CON) con_write(buffer, count);
        cpu.a.x = count;
        return 0;
    }
    ssize_t n;
    do { n = quota_write(h->fd, buffer, count, 0, false); } while (n < 0 && errno == EINTR);
    if (n < 0) return dos_errno(errno);
    if ((h->mode & 0x4000) && fsync(h->fd) < 0) return dos_errno(errno);
    cpu.a.x = (uint16_t)n;
    return 0;
}

static int seek_file(void)
{
    if (cpu.a.l > 2) return 1;
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    if (h->device != DEV_NONE) { cpu.a.x = cpu.d.x = 0; return 0; }
    uint32_t offset = (uint32_t)cpu.c.x << 16 | cpu.d.x;
    off_t value = cpu.a.l == 0 ? (off_t)offset : (off_t)(int32_t)offset;
    int origin = cpu.a.l == 0 ? SEEK_SET : cpu.a.l == 1 ? SEEK_CUR : SEEK_END;
    off_t pos = lseek(h->fd, value, origin);
    if (pos < 0) return errno == EINVAL ? 5 : dos_errno(errno);
    cpu.a.x = (uint16_t)pos;
    cpu.d.x = (uint16_t)((uint64_t)pos >> 16);
    return 0;
}

static int duplicate_handle(bool forced)
{
    DosHandle *source = dos_handle(cpu.b.x);
    if (!source) return 6;
    int number = forced ? cpu.c.x : unused_jfn();
    if (number < 0) return 4;
    if (number >= DOS_HANDLES || (has_jft(fcb_process) &&
        jft_entry(fcb_process, (unsigned)number) == UINT32_MAX)) return 6;
    if (number == cpu.b.x) { cpu.a.x = (uint16_t)number; return 0; }
    int slot = has_jft(fcb_process) ? unused_handle() : number;
    if (slot < 0) return 4;
    int fd = -1;
    if (source->fd >= 0) {
        fd = fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) return dos_errno(errno);
    }
    DosHandle copy = *source;
    if (dos_handle((unsigned)number)) close_dos_handle((unsigned)number);
    handles[slot] = copy;
    handles[slot].fd = fd;
    handles[slot].references = 1;
    if (has_jft(fcb_process)) mem[jft_entry(fcb_process, (unsigned)number)] = (uint8_t)slot;
    cpu.a.x = (uint16_t)number;
    return 0;
}

static int flush_handle(void)
{
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    return h->fd >= 0 && fsync(h->fd) < 0 ? dos_errno(errno) : 0;
}

static int ioctl_call(void)
{
    unsigned sub = cpu.a.l;
    if (sub == 8 || sub == 9 || sub == 0x0e || sub == 0x0f) {
        if (!query_drive(cpu.b.l)) return 15;
        if (sub == 8) cpu.a.x = 1; /* fixed, not removable */
        else if (sub == 9) cpu.d.x = 0; /* local */
        else cpu.a.l = 0; /* no logical-drive remapping */
        return 0;
    }
    if (sub != 0 && sub != 1 && sub != 6 && sub != 7) return 1;
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    if (!sub) {
        if (h->device) cpu.d.x = (uint16_t)(0x80 | (h->mode & 0x20) |
            (h->device == DEV_IN ? 1 : h->device == DEV_OUT ? 2 :
             h->device == DEV_CON ? 3 : h->device == DEV_NUL ? 4 : 0));
        else cpu.d.x = (uint16_t)h->drive;
    } else if (sub == 1) {
        if (!h->device || cpu.d.h) return 1;
        h->mode = (h->mode & ~0x20u) | (cpu.d.x & 0x20);
    } else if (sub == 7) cpu.a.l = 0xff;
    else if (h->device) cpu.a.l = h->device == DEV_IN || h->device == DEV_AUX ? 0 : 0xff;
    else {
        struct stat st;
        off_t pos = lseek(h->fd, 0, SEEK_CUR);
        if (pos < 0 || fstat(h->fd, &st) < 0) return dos_errno(errno);
        cpu.a.l = pos < st.st_size ? 0xff : 0;
    }
    return 0;
}

static int set_attributes(const char *path, const struct stat *st, unsigned attr)
{
    /* An update of the ignored DOS bits must not broaden Unix permissions
     * when the caller has not actually changed the read-only state. */
    if (!!(attr & ATTR_RO) == !(st->st_mode & S_IWUSR)) return 0;
    mode_t mode = st->st_mode & 07777;
    if (attr & ATTR_RO) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    else mode |= S_IWUSR | S_IWGRP | S_IWOTH;
    return fs_chmod(path, mode) < 0 ? dos_errno(errno) : 0;
}

static int attributes_call(unsigned sub, bool lfn)
{
    if (sub > (lfn ? 8u : 1u)) return 1;
    char path[PATH_MAX];
    struct stat st;
    int error = memory_path(cpu.ds, cpu.d.x, path, false);
    if (error || (error = file_stat(path, &st))) return error;
    if (!sub) { cpu.c.x = (uint16_t)file_attr(path, &st); return 0; }
    if (sub == 1) return set_attributes(path, &st, cpu.c.x);
    if (sub == 2) {
        uint64_t size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
        if (size > UINT32_MAX) size = UINT32_MAX;
        cpu.a.x = (uint16_t)size; cpu.d.x = (uint16_t)(size >> 16);
        return 0;
    }
    if (sub & 1) {
        struct timespec ts;
        error = unpack_time(sub == 5 ? 0 : cpu.c.x, cpu.di, sub == 7 ? cpu.si : 0, &ts);
        if (error) return error;
        if (sub == 7) return set_birth(&st, ts);
        struct timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
        times[sub == 5 ? 0 : 1] = ts;
        return fs_utimens(path, times) < 0 ? dos_errno(errno) : 0;
    }
    struct timespec ts = sub == 4 ? st.st_mtim : sub == 6 ? st.st_atim : birth_time(path, -1, &st);
    uint16_t tt, dt;
    bool in_range = pack_time(ts, &tt, &dt);
    cpu.c.x = sub == 6 ? 0 : tt;
    cpu.di = dt;
    if (sub == 8) cpu.si = in_range ? (uint16_t)time_hundredths(ts) : 0;
    return 0;
}

static int handle_time(unsigned sub)
{
    if (sub > 1) return 1; /* includes the explicitly unsupported 5702..5707 */
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h) return 6;
    if (h->device) return 1;
    if (!sub) {
        struct stat st;
        if (fstat(h->fd, &st) < 0) return dos_errno(errno);
        pack_time(st.st_mtim, &cpu.c.x, &cpu.d.x);
        return 0;
    }
    struct timespec times[2] = {{0, UTIME_OMIT}, {0, 0}};
    int error = unpack_time(cpu.c.x, cpu.d.x, 0, &times[1]);
    if (error) return error;
    return futimens(h->fd, times) < 0 ? dos_errno(errno) : 0;
}

static int make_directory(void)
{
    char dos[DOS_PATH_MAX], path[PATH_MAX];
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    size_t n = strlen(dos);
    while (n > 1 && (dos[n - 1] == '/' || dos[n - 1] == '\\') && dos[n - 2] != ':') dos[--n] = 0;
    error = resolve_path(dos, path, true);
    if (error) return error;
    if (fs_mkdir(path, 0777) < 0) return errno == EEXIST ? 5 : dos_errno(errno);
    return 0;
}

static int remove_directory(void)
{
    char dos[DOS_PATH_MAX], path[PATH_MAX];
    struct stat st, cwd_st, link_st;
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    size_t n = strlen(dos);
    while (n > 1 && (dos[n - 1] == '/' || dos[n - 1] == '\\') && dos[n - 2] != ':') dos[--n] = 0;
    error = resolve_path(dos, path, false);
    if (error) return error == 2 ? 3 : error;
    if (fs_stat(path, &link_st, true) < 0) return dos_errno(errno);
    if (S_ISLNK(link_st.st_mode)) {
        if (fs_stat(path, &st, false) < 0) {
            if (errno != ENOENT && errno != ENOTDIR) return dos_errno(errno);
        } else if (!S_ISDIR(st.st_mode)) return 3;
        /* VC recurses after rmdir fails. Succeed by removing only the final
         * symlink, never by letting it enumerate and delete the target. */
        if (fs_unlink(path, false) < 0) return dos_errno(errno);
        if (link_st.st_nlink <= 1 && !inode_is_open(&link_st)) forget_birth(&link_st);
        return 0;
    }
    st = link_st;
    if (!S_ISDIR(st.st_mode)) return 3;
    for (unsigned i = 0; i < DOS_DRIVES; ++i) {
        if (!drive_present(i)) continue;
        const char *cwd = drives[i].cwd;
        if (!strcmp(path, cwd) || (!fs_stat(cwd, &cwd_st, false) &&
            st.st_dev == cwd_st.st_dev && st.st_ino == cwd_st.st_ino)) return 16;
    }
    if (drive_root(path)) return 5;
    if (fs_unlink(path, true) < 0) return dos_errno(errno);
    forget_birth(&st);
    return 0;
}

static int change_directory(void)
{
    char dos[DOS_PATH_MAX], path[PATH_MAX];
    struct stat st;
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    const char *name = dos;
    unsigned drive;
    error = path_drive(&name, &drive);
    if (error) return error;
    error = resolve_path(dos, path, false);
    if (error) return error == 2 ? 3 : error;
    error = file_stat(path, &st);
    if (error) return error == 2 ? 3 : error;
    if (!S_ISDIR(st.st_mode)) return 3;
    strcpy(drives[drive].cwd, path);
    return 0;
}

static int get_directory(bool lfn)
{
    DosDrive *drive = query_drive(cpu.d.l);
    if (!drive) return 15;
    char path[DOS_PATH_MAX];
    /* DOS returns no drive or leading separator. Keep the requested drive's
     * basis so callers can reconstruct C:\\... even below the H: root. */
    int error = host_to_dos_on_drive(drive->cwd, (unsigned)(drive - drives),
                                     !lfn, path, sizeof(path));
    if (error) return error;
    error = write_string(cpu.ds, cpu.si, path + 3, lfn ? LFN_PATH_MAX : 64);
    if (!error) cpu.a.x = 0x0100; /* DOS 3+ documented success value */
    return error;
}

static int unlink_path(const char *path, bool temporary)
{
    struct stat link_st;
    if (fs_stat(path, &link_st, true) < 0) return dos_errno(errno);
    /* A final symlink is the entry being deleted, regardless of its target's
     * type, permissions or existence. In particular, ancestor links are
     * deliberately listed as files and arrive here through 41h/7141h. */
    if (!S_ISLNK(link_st.st_mode) &&
        (S_ISDIR(link_st.st_mode) || (!temporary && !(link_st.st_mode & S_IWUSR)))) return 5;
    if (fs_unlink(path, false) < 0) return dos_errno(errno);
    /* Removing a symlink never removes the followed target's inode. */
    if (link_st.st_nlink <= 1 && !inode_is_open(&link_st)) forget_birth(&link_st);
    return 0;
}

int dos_fs_make_temporary(const char *dos)
{
    if (door_root_fd < 0 || !dos || strlen(dos) < 4 || dos[1] != ':' ||
        (dos[2] != '\\' && dos[2] != '/')) return 5;
    char path[PATH_MAX];
    int error = resolve_path(dos, path, true);
    if (error) return error;
    if (drive_root(path) || drive_ancestor(path)) return 5;
    /* fs_mkdir charges the one new entry; EEXIST lets the caller pick another
     * name, and a full session is DOS disk full like any guest mkdir. */
    if (fs_mkdir(path, 0700) < 0) return errno == EEXIST ? 80 : dos_errno(errno);
    return 0;
}

int dos_fs_remove_temporary(const char *dos)
{
    if (door_root_fd < 0 || !dos || strlen(dos) < 3 || dos[1] != ':' ||
        (dos[2] != '\\' && dos[2] != '/')) return 5;
    char path[PATH_MAX];
    int error = resolve_path(dos, path, false);
    if (error) return error;
    if (drive_ancestor(path)) return 5;
    struct stat st;
    if (fs_stat(path, &st, true)) return dos_errno(errno);
    if (!S_ISDIR(st.st_mode)) {
        error = unlink_path(path, true);
        if (error) return error;
        /* An aborted editor never gets to close its ordinary DOS handles.
         * Only this runtime-owned temporary inode is being retired: generic
         * process handles (and generic TSR handles) keep their old lifetime. */
        for (unsigned i = 0; i < DOS_HANDLES; ++i) {
            DosHandle *h = get_handle(i);
            struct stat held;
            if (!h || h->device || fstat(h->fd, &held) ||
                held.st_dev != st.st_dev || held.st_ino != st.st_ino) continue;
            int closed = close_handle(i);
            if (!error) error = closed;
        }
        return error;
    }
    if (fs_unlink(path, true)) return dos_errno(errno);
    forget_birth(&st);
    return 0;
}

static int delete_file(bool lfn)
{
    if (lfn && cpu.si > 1) return 1;
    char dos[DOS_PATH_MAX], path[PATH_MAX];
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    if (!lfn || !cpu.si) {
        error = resolve_path(dos, path, false);
        return error ? error : unlink_path(path, false);
    }
    char pattern[DOS_NAME_MAX + 1];
    const char *name = dos;
    unsigned drive;
    error = path_drive(&name, &drive);
    if (error || (error = split_pattern(dos, path, pattern))) return error;
    Entry *entries;
    size_t count, removed = 0;
    error = snapshot(path, drive, &entries, &count);
    if (error) return error;
    size_t selected = count;
    if (!strpbrk(pattern, "*?")) {
        /* Wildcard mode can still receive one literal long name. Apply the
         * same ambiguity and native-priority rules as ordinary resolution. */
        unsigned matches = matching_entry(entries, count, pattern, &selected);
        size_t n = strlen(pattern);
        if (!matches && n > 1 && strchr(pattern, '.') == pattern + n - 1) {
            /* Like long_match, a trailing dot can mean no extension, but an
             * ambiguous literal name must never fall through to this case. */
            pattern[n - 1] = 0;
            matches = matching_entry(entries, count, pattern, &selected);
            pattern[n - 1] = '.';
        }
        if (matches != 1) { free_entries(entries, count); return 2; }
    }
    for (size_t i = 0; i < count; ++i) {
        Entry *e = entries + i;
        if (!strcmp(e->host, ".") || !strcmp(e->host, "..")) continue;
        if (!e->have_stat || !attributes_match(e->attr, cpu.c.x, true)) continue;
        if (selected < count ? i != selected :
            !(long_match(pattern, e->dos) || long_match(pattern, e->alias))) continue;
        char file[PATH_MAX];
        error = join_path(path, e->host, file, sizeof(file));
        if (!error) error = unlink_path(file, false);
        if (error) break;
        ++removed;
    }
    free_entries(entries, count);
    return error ? error : removed ? 0 : 2;
}

static void rebase_path(char *path, const char *old, const char *replacement)
{
    size_t n = strlen(old);
    if (strncmp(path, old, n) || (path[n] && path[n] != '/')) return;
    char updated[PATH_MAX];
    if (strlen(replacement) + strlen(path + n) >= sizeof(updated)) return;
    strcpy(updated, replacement); strcat(updated, path + n);
    strcpy(path, updated);
}

static int rename_file(void)
{
    char old[PATH_MAX], target[PATH_MAX], dos[DOS_PATH_MAX];
    int error = memory_path(cpu.ds, cpu.d.x, old, false);
    if (error) return error;
    error = read_string(cpu.es, cpu.di, dos, sizeof(dos));
    if (error || (error = resolve_path(dos, target, true))) return error;
    return rename_paths(old, target, dos);
}

static int rename_paths(const char *old, char *target, const char *dos)
{
    struct stat st;
    int error = file_stat(old, &st);
    if (error) return error;
    if (!(st.st_mode & S_IWUSR)) return 5;
    /* A drive root (or its ancestor reached through C:) must stay in place. */
    if (drive_ancestor(old)) return 5;
    if (S_ISDIR(st.st_mode)) {
        struct stat link_st;
        if (fs_stat(old, &link_st, true) < 0) return dos_errno(errno);
        /* A symlinked parent may give the root another C: spelling. Moving
         * a final symlink itself is safe: its target stays where it was. */
        if (door_root_fd < 0 && !S_ISLNK(link_st.st_mode)) {
            char *real = realpath(old, NULL);
            if (!real) return dos_errno(errno);
            bool protected = drive_ancestor(real);
            free(real);
            if (protected) return 5;
        }
    }
    if (!strcmp(old, target)) {
        /* Preserve a requested case-only change, but do not replace a real
         * long name with its alias or its lossy display spelling. */
        const char *leaf = dos;
        for (const char *p = dos; *p; ++p) if (*p == '/' || *p == '\\' || *p == ':') leaf = p + 1;
        char old_dos[DOS_NAME_MAX + 1], utf8[4 * (DOS_NAME_MAX + 1)], parent[PATH_MAX];
        bool lossless = utf8_to_cp(strrchr(old, '/') + 1, old_dos, sizeof(old_dos));
        if (!lossless || !*leaf || !cp_equal(leaf, old_dos)) return 0;
        error = cp_to_utf8(leaf, utf8, sizeof(utf8));
        strcpy(parent, old); parent_path(parent);
        if (error || (error = join_path(parent, utf8, target, PATH_MAX))) return error;
        if (!strcmp(old, target)) return 0;
    }
    /* Unlike POSIX rename(), DOS must never overwrite an existing name.
     * Linux's atomic NOREPLACE also closes the destination-existence race. */
#ifdef __EMSCRIPTEN__
    /* MEMFS has no competing process, so this check and rename are atomic
     * with respect to VC. lstat also refuses a dangling destination symlink. */
    struct stat target_st;
    if (lstat(target, &target_st) == 0) return 80;
    if (errno != ENOENT) return dos_errno(errno);
    if (rename(old, target) < 0) return dos_errno(errno);
#else
    if (fs_rename(old, target) < 0) return dos_errno(errno);
#endif
    for (DosPathLease *lease = path_leases; lease; lease = lease->next) {
        if (!active_lease(lease) || strcmp(lease->host, old)) continue;
        LeasePart *leaf = lease->parts + lease->count - 1;
        if (leaf->type != S_IFREG) continue;
        /* VZ makes its backup by rename, then recreates the old pathname.
         * Drop the inode lease, not the short-to-long basename reservation:
         * otherwise an alias can move to a neighbor during that vacancy. */
        if (lease->fd >= 0) close(lease->fd);
        lease->fd = -1;
        lease->recreate = true;
        leaf->entry_type = S_IFREG;
    }
    for (unsigned i = 0; i < DOS_DRIVES; ++i) {
        if (!drive_present(i)) continue;
        rebase_path(drives[i].cwd, old, target);
        /* C: can move a saved H: directory outside its drive. Forget that
         * directory rather than letting a later H:relative path escape. */
        if (!path_below(drives[i].cwd, drives[i].root))
            strcpy(drives[i].cwd, drives[i].root);
    }
    for (unsigned i = 0; i < DOS_HANDLES; ++i)
        if (get_handle(i)) rebase_path(handles[i].path, old, target);
    return 0;
}

static int create_temporary(void)
{
    char dos[DOS_PATH_MAX], dir[PATH_MAX], path[PATH_MAX];
    int error = read_string(cpu.ds, cpu.d.x, dos, sizeof(dos));
    if (error) return error;
    const char *name_start = dos;
    unsigned drive;
    error = path_drive(&name_start, &drive);
    if (error || (error = resolve_path(dos, dir, false))) return error;
    struct stat st;
    error = file_stat(dir, &st);
    if (error || !S_ISDIR(st.st_mode)) return error ? error : 3;
    size_t n = strlen(dos);
    bool separator = n && dos[n - 1] != '/' && dos[n - 1] != '\\' && dos[n - 1] != ':';
    if (n + separator + 13 > 128) return 3;
    if (separator) dos[n++] = '\\';
    if (unused_jfn() < 0) return 4;
    unsigned attributes = cpu.c.x;
    for (unsigned tries = 0; tries < 999999; ++tries) {
        temp_sequence = temp_sequence % 999999 + 1;
        char name[13];
        snprintf(name, sizeof(name), "VC%06u.TMP", temp_sequence);
        error = resolve_name(dir, name, path, true);
        if (error) return error;
        error = open_path(path, drive, 2, attributes, 0x10);
        if (error == 80) continue;
        if (error) return error;
        error = publish_handle(cpu.a.x);
        if (error) return error;
        strcpy(dos + n, name);
        cpu.c.x = (uint16_t)attributes;
        return write_string(cpu.ds, cpu.d.x, dos, 128);
    }
    return 80;
}

static int lexical_dos_path(const char *input, char out[DOS_PATH_MAX])
{
    if (!*input) return 3;
    const char *p = input;
    unsigned drive;
    int error = path_drive(&p, &drive);
    if (error) return error;
    if (*p == '/' || *p == '\\') {
        strcpy(out, "C:\\");
        out[0] = (char)('A' + drive);
    }
    else {
        error = host_to_dos_on_drive(drives[drive].cwd, drive, false, out, DOS_PATH_MAX);
        if (error) return error;
    }
    size_t used = strlen(out);
    while (*p) {
        while (*p == '/' || *p == '\\') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        size_t n = (size_t)(p - start);
        if (n == 1 && *start == '.') continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (used > 3) {
                char *slash = strrchr(out, '\\');
                used = slash == out + 2 ? 3 : (size_t)(slash - out);
                out[used] = 0;
            }
            continue;
        }
        if (n > DOS_NAME_MAX || used + n + 1 >= DOS_PATH_MAX) return 3;
        if (used > 3) out[used++] = '\\';
        memcpy(out + used, start, n);
        used += n; out[used] = 0;
    }
    return 0;
}

/* CL=0 and AH=60's missing-parent fallback do not require the named file to
 * exist. Resolve only a possible C: spelling of the home-root prefix, so
 * its aliases/case are recognized without checking any nonexistent suffix. */
static void prefer_home_truename(char path[DOS_PATH_MAX])
{
    if (!drive_present(DRIVE_H) || path[0] != 'C') return;
    unsigned components = 0;
    for (const char *p = drives[DRIVE_H].root; *p; ++p)
        if (*p == '/') ++components;
    char *end = path + 3;
    for (unsigned i = 0; i < components; ++i) {
        if (!*end) return;
        while (*end && *end != '\\') ++end;
        if (i + 1 < components) {
            if (!*end) return;
            ++end;
        }
    }
    char prefix[DOS_PATH_MAX], host[PATH_MAX];
    size_t n = (size_t)(end - path);
    memcpy(prefix, path, n); prefix[n] = 0;
    if (resolve_path(prefix, host, false) || strcmp(host, drives[DRIVE_H].root)) return;
    if (*end) ++end;
    memmove(path + 3, end, strlen(end) + 1);
    path[0] = 'H';
}

static int truename(bool lfn)
{
    unsigned mode = lfn ? cpu.c.l : 1;
    if (mode > 2) return 1;
    char input[DOS_PATH_MAX], canonical[DOS_PATH_MAX], host[PATH_MAX], output[DOS_PATH_MAX];
    int error = read_string(cpu.ds, cpu.si, input, sizeof(input));
    if (error || (error = lexical_dos_path(input, canonical))) return error;
    if (lfn && mode == 0) {
        strcpy(output, canonical);
        prefer_home_truename(output);
    }
    else {
        error = resolve_path(canonical, host, !lfn);
        if (error && !lfn && (error == 2 || error == 3)) {
            /* AH=60 is also valid for not-yet-existing paths. There is no
             * directory alias to query for a nonexistent parent. */
            strcpy(output, canonical);
            prefer_home_truename(output);
            for (char *p = output; *p; ++p) *p = (char)cp866_upper((uint8_t)*p);
        } else {
            if (error) return error;
            error = host_to_dos(host, mode == 1, output, sizeof(output));
            if (error) return error;
        }
    }
    error = write_string(cpu.es, cpu.di, output, lfn ? LFN_PATH_MAX : 128);
    if (!error) cpu.a.x = 0;
    return error;
}

void dos_casemap_upper(void)
{
    if (cpu.a.l >= 0x80) cpu.a.l = cp866_upper(cpu.a.l);
}

void dos_fs_init(void)
{
    while (path_leases) dos_fs_release_path(path_leases);
    if (initialized) {
        for (unsigned i = 0; i < DOS_HANDLES; ++i) if (get_handle(i)) {
            handles[i].references = 1;
            close_handle(i);
        }
        for (unsigned i = 0; i < SEARCH_SLOTS; ++i) release_search(searches + i);
    }
    if (door_root_fd >= 0) close(door_root_fd);
    cleanup_command_pipes();
    door_root_fd = -1;
    door_quota = door_bytes = 0;
    door_entries = 0;
    memset(handles, 0, sizeof(handles));
    for (unsigned i = 0; i < DOS_HANDLES; ++i) handles[i].fd = -1;
    handles[0].device = DEV_IN; handles[0].access = 0;
    handles[1].device = handles[2].device = DEV_OUT;
    handles[1].access = handles[2].access = 1;
    handles[3].device = DEV_AUX; handles[3].access = 2;
    handles[4].device = DEV_PRN; handles[4].access = 1;
    for (unsigned i = 0; i < 5; ++i) handles[i].references = 1;
    while (birth_times) {
        BirthTime *next = birth_times->next;
        free(birth_times); birth_times = next;
    }
    memset(drives, 0, sizeof(drives));
    strcpy(drives[DRIVE_C].root, "/");
    strcpy(drives[DRIVE_C].cwd, "/");
    const char *home = getenv("HOME");
    if (home && *home == '/') {
        char *root = realpath(home, NULL);
        struct stat st;
        if (root && strlen(root) < PATH_MAX && strcmp(root, "/") &&
            !stat(root, &st) && S_ISDIR(st.st_mode)) {
            strcpy(drives[DRIVE_H].root, root);
            strcpy(drives[DRIVE_H].cwd, root);
        }
        free(root);
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    current_drive = host_drive(cwd);
    strcpy(drives[current_drive].cwd, cwd);
    dta_seg = cpu.ds; dta_off = 0x80;
    last_error = 0; temp_sequence = 0;
    fcb_process = 0;
    closed_file_path[0] = 0;
    clock_delta = (struct timespec){0, 0};
    initialized = true;
}

int dos_fs_init_door(const char *root, uint64_t quota)
{
    dos_fs_init();
    /* Fail closed even when an invalid root follows an ordinary session. */
    memset(drives, 0, sizeof(drives));
    current_drive = DRIVE_H;
    if (!root || *root != '/' || !quota) return 5;
    char canonical[PATH_MAX];
    struct stat st;
    if (lstat(root, &st) || !S_ISDIR(st.st_mode) ||
        !realpath(root, canonical) || !strcmp(canonical, "/")) return 5;
    int fd = open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return dos_errno(errno);
    strcpy(drives[DRIVE_H].root, canonical);
    strcpy(drives[DRIVE_H].cwd, canonical);
    door_root_fd = fd;
    door_quota = quota;
    long page = sysconf(_SC_PAGESIZE); /* auxv only: no file or syscall */
    door_page = page > 4096 ? (uint64_t)page : 4096;
    int error = quota_scan(fd, 0);
    if (error) {
        close(door_root_fd);
        door_root_fd = -1;
        door_quota = door_bytes = 0;
        door_entries = 0;
        memset(drives, 0, sizeof(drives));
    }
    return error;
}

static bool owns_function(uint16_t ax)
{
    if (ax == 0x7303 || (ax >> 8) == 0x71) return true;
    switch (ax >> 8) {
    case 0x0d: case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x21: case 0x22:
    case 0x23: case 0x24: case 0x27: case 0x28: case 0x29: return true;
    case 0x0e: case 0x19: case 0x1a: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
    case 0x2f: case 0x36: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c:
    case 0x3d: case 0x3e: case 0x3f: case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: case 0x4e: case 0x4f: case 0x56:
    case 0x57: case 0x59: case 0x5a: case 0x5b: case 0x60: case 0x67: case 0x68:
    case 0x6c: return true;
    default: return false;
    }
}

/* Character DOS I/O is redirectable too. BIOS owns real CON input and its
 * editable line buffer; regular-file handles are serviced here before that
 * fallback. Unlike a host shell, this sees COMMAND's live PSP job table. */
static int redirected_console(void)
{
    unsigned function = cpu.a.h;
    if (!initialized || function > 0x0C || !function) return 0;
    bool writing = function == 2 || function == 9 || (function == 6 && cpu.d.l != 0xFF);
    bool reading = function == 1 || function == 7 || function == 8 || function == 0x0A ||
                   function == 0x0B || function == 0x0C || (function == 6 && cpu.d.l == 0xFF);
    if (!writing && !reading) return 0;
    DosHandle *h = dos_handle(writing ? 1 : 0);
    if (!h || (h->device != DEV_NONE && h->device != DEV_NUL)) return 0;
    if (writing) {
        uint8_t buffer[UINT16_MAX];
        unsigned count = 1;
        buffer[0] = cpu.d.l;
        if (function == 9) {
            for (count = 0; count < sizeof buffer; ++count) {
                uint8_t byte = rd8(cpu.ds, (uint16_t)(cpu.d.x + count));
                if (byte == '$') break;
                buffer[count] = byte;
            }
        }
        size_t done = 0;
        while (h->device != DEV_NUL && done < count) {
            ssize_t n = quota_write(h->fd, buffer + done, count - done, 0, false);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        cpu.a.l = function == 9 ? '$' : cpu.d.l;
        return 1;
    }
    if (function == 0x0C) {
        function = cpu.a.l;
        if (function != 1 && function != 6 && function != 7 && function != 8 && function != 0x0A)
            return 1;
    }
    if (function == 0x0B) {
        struct stat st;
        off_t pos = lseek(h->fd, 0, SEEK_CUR);
        cpu.a.l = h->device != DEV_NUL && pos >= 0 && !fstat(h->fd, &st) && pos < st.st_size ? 0xFF : 0;
        return 1;
    }
    unsigned count = 0, maximum = function == 0x0A ? rd8(cpu.ds, cpu.d.x) : 2;
    uint8_t byte = 0x1A;
    while (count + 1 < maximum) {
        ssize_t n;
        do { n = h->device == DEV_NUL ? 0 : read(h->fd, &byte, 1); } while (n < 0 && errno == EINTR);
        if (n != 1) { byte = 0x1A; break; }
        if (function != 0x0A) { count = 1; break; }
        if (byte == '\n') continue;
        if (byte == '\r') break;
        wr8(cpu.ds, (uint16_t)(cpu.d.x + 2 + count++), byte);
    }
    if (function == 0x0A) {
        wr8(cpu.ds, (uint16_t)(cpu.d.x + 1), (uint8_t)count);
        wr8(cpu.ds, (uint16_t)(cpu.d.x + 2 + count), '\r');
    } else {
        cpu.a.l = byte;
        if (function == 6) { cpu.zf = count == 0; if (!count) cpu.a.l = 0; }
        if (function == 1 && count) con_write(&byte, 1);
    }
    return 1;
}

static void extended_error(void)
{
    unsigned cls = 0, action = 0, locus = 0;
    if (last_error) {
        cls = 7; action = 4; locus = 2;
        switch (last_error) {
        case 2: case 3: case 15: case 18: cls = 8; action = 3; break;
        case 4: case 8: case 39: cls = 1; action = 3; break;
        case 5: cls = 3; action = 3; break;
        case 32: cls = 10; action = 2; break;
        case 80: cls = 12; action = 3; break;
        default: break;
        }
    }
    cpu.a.x = last_error;
    cpu.b.h = (uint8_t)cls; cpu.b.l = (uint8_t)action; cpu.c.h = (uint8_t)locus;
}

int dos_fs_int21(void)
{
    if (redirected_console()) return 1;
    uint16_t function = cpu.a.x;
    /* This test must precede even lazy initialization: unowned interrupts
     * leave every register/flag and every byte of guest memory untouched. */
    if (!owns_function(function)) return 0;
    if (!initialized) dos_fs_init();
    int error = 0;
    if ((function >> 8) == 0x71) {
        switch (function) {
        case 0x7139: error = make_directory(); break;
        case 0x713a: error = remove_directory(); break;
        case 0x713b: error = change_directory(); break;
        case 0x7141: error = delete_file(true); break;
        case 0x7143: error = attributes_call(cpu.b.l, true); break;
        case 0x7147: error = get_directory(true); break;
        case 0x714e: error = find_first(true); break;
        case 0x714f: error = find_next(true); break;
        case 0x7156: error = rename_file(); break;
        case 0x7160: error = truename(true); break;
        case 0x716c: error = open_file(cpu.si, cpu.b.x, cpu.c.x, cpu.d.x, true); break;
        case 0x71a0: error = filesystem_info(); break;
        case 0x71a1: {
            Search *s = lfn_search(cpu.b.x);
            if (!s) error = 6;
            else { release_search(s); cpu.a.x = 0; }
            break;
        }
        case 0x71a6: error = handle_info(); break;
        case 0x71a7: error = convert_filetime(); break;
        case 0x71a8: error = generate_shortname(); break;
        default: error = 0x7100; break;
        }
    } else if (function == 0x7303) error = disk_space(true);
    else switch (function >> 8) {
    case 0x0d:
        for (unsigned i = 0; i < DOS_HANDLES; i++)
            if (handles[i].fd >= 0 && handles[i].access != 0) fsync(handles[i].fd);
        break;
    case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x21: case 0x22: case 0x23: case 0x24: case 0x27:
    case 0x28: case 0x29:
        fcb_call((uint8_t)(function >> 8));
        return 1;
    case 0x0e:
        if (!drive_present(cpu.d.l)) error = 15;
        else { current_drive = cpu.d.l; cpu.a.l = DOS_DRIVES; }
        break;
    case 0x19: cpu.a.l = (uint8_t)current_drive; break;
    case 0x1a: dta_seg = cpu.ds; dta_off = cpu.d.x; break;
    case 0x2f: cpu.es = dta_seg; cpu.b.x = dta_off; break;
    case 0x2a: case 0x2c: {
        struct timespec now = dos_now();
        struct tm tm;
        if (!localtime_r(&now.tv_sec, &tm)) { error = 13; break; }
        if ((function >> 8) == 0x2a) {
            cpu.c.x = (uint16_t)(tm.tm_year + 1900);
            cpu.d.h = (uint8_t)(tm.tm_mon + 1); cpu.d.l = (uint8_t)tm.tm_mday;
            cpu.a.l = (uint8_t)tm.tm_wday;
        } else {
            cpu.c.h = (uint8_t)tm.tm_hour; cpu.c.l = (uint8_t)tm.tm_min;
            cpu.d.h = (uint8_t)tm.tm_sec; cpu.d.l = (uint8_t)(now.tv_nsec / 10000000);
        }
        break;
    }
    /* These three report errors the DOS 1.x way, never with CF: set date and
     * set time return AL=FFh, free space returns AX=FFFFh. VC detects
     * DESQview by setting the impossible date 'DE'-'SQ' and reading AL. */
    case 0x2b: case 0x2d:
        if (set_clock(cpu.a.h == 0x2b)) cpu.a.l = 0xFF;
        cpu.cf = 0;
        return 1;
    case 0x36:
        if (disk_space(false)) cpu.a.x = 0xFFFF;
        cpu.cf = 0;
        return 1;
    case 0x38: error = country_info(); break;
    case 0x39: error = make_directory(); break;
    case 0x3a: error = remove_directory(); break;
    case 0x3b: error = change_directory(); break;
    case 0x3c: error = open_file(cpu.d.x, 2, cpu.c.x, 0x12, false); break;
    case 0x3d: error = open_file(cpu.d.x, cpu.a.l, 0, 1, false); break;
    case 0x3e: error = close_dos_handle(cpu.b.x); break;
    case 0x3f: error = read_file(); break;
    case 0x40: error = write_file(); break;
    case 0x41: error = delete_file(false); break;
    case 0x42: error = seek_file(); break;
    case 0x43: error = attributes_call(cpu.a.l, false); break;
    case 0x44: error = ioctl_call(); break;
    case 0x45: error = duplicate_handle(false); break;
    case 0x46: error = duplicate_handle(true); break;
    case 0x47: error = get_directory(false); break;
    case 0x4e: error = find_first(false); break;
    case 0x4f: error = find_next(false); break;
    case 0x56: error = rename_file(); break;
    case 0x57: error = handle_time(cpu.a.l); break;
    case 0x59: extended_error(); break;
    case 0x5a: error = create_temporary(); break;
    case 0x5b: error = open_file(cpu.d.x, 2, cpu.c.x, 0x10, false); break;
    case 0x60: error = truename(false); break;
    case 0x67:
        if (has_jft(fcb_process)) return 0; /* The kernel allocates the guest table. */
        if (cpu.b.x > DOS_HANDLES) error = 4;
        break;
    case 0x68: error = flush_handle(); break;
    case 0x6c:
        error = cpu.a.l ? 1 : open_file(cpu.si, cpu.b.x, cpu.c.x, cpu.d.x, true);
        break;
    }
    result(error);
    return 1;
}

/* MISC_QUERY_HELPERS: independent query/conversion implementations below. */

static int disk_space(bool extended)
{
    char path[PATH_MAX] = "/";
    if (extended) {
        if (cpu.c.x < 44) return 87;
        int error = memory_path(cpu.ds, cpu.d.x, path, false);
        if (error) return error;
        struct stat st;
        error = file_stat(path, &st);
        if (error) return error;
        if (!S_ISDIR(st.st_mode)) return 3;
    } else {
        const DosDrive *drive = query_drive(cpu.d.l);
        if (!drive) return 15;
        strcpy(path, drive->root);
    }

    uint64_t block_bytes, blocks, available, total_bytes, free_bytes;
    if (door_root_fd >= 0) {
        block_bytes = 512;
        total_bytes = door_quota;
        free_bytes = door_bytes < door_quota ? door_quota - door_bytes : 0;
        blocks = total_bytes / block_bytes;
        available = free_bytes / block_bytes;
    } else {
        struct statvfs fs;
        if (statvfs(path, &fs) != 0) return dos_errno(errno);
        block_bytes = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
        if (!block_bytes) block_bytes = 512;
        blocks = fs.f_blocks; available = fs.f_bavail;
        if (available > blocks) available = blocks;
        total_bytes = blocks > UINT64_MAX / block_bytes ? UINT64_MAX : blocks * block_bytes;
        free_bytes = available > UINT64_MAX / block_bytes ? UINT64_MAX : available * block_bytes;
    }
    uint64_t total_sectors = total_bytes / 512, free_sectors = free_bytes / 512;
    uint64_t sectors_per_cluster = block_bytes / 512;
    if (!sectors_per_cluster) sectors_per_cluster = 1;

    if (!extended) {
        /* Scale the logical allocation unit so ordinary Linux volumes fit
         * DOS's 16-bit cluster counters. Very large volumes are saturated. */
        /* DOS 7 reports at most 2 GB here: 64 sectors of 512 bytes a cluster,
         * 65535 clusters. A larger count would read as AX=FFFFh, "invalid
         * drive". Callers wanting the real size use 7303h. */
        while (sectors_per_cluster < 64 && total_sectors / sectors_per_cluster > 65535)
            sectors_per_cluster *= 2;
        if (sectors_per_cluster > 64) sectors_per_cluster = 64;
        uint64_t total = total_sectors / sectors_per_cluster;
        uint64_t free = free_sectors / sectors_per_cluster;
        cpu.a.x = (uint16_t)sectors_per_cluster;
        cpu.b.x = (uint16_t)(free > 65535 ? 65535 : free);
        cpu.c.x = 512;
        cpu.d.x = (uint16_t)(total > 65535 ? 65535 : total);
        return 0;
    }

    if (sectors_per_cluster > UINT32_MAX) sectors_per_cluster = UINT32_MAX;
    uint64_t fields[8] = {
        sectors_per_cluster, 512,
        free_sectors / sectors_per_cluster, total_sectors / sectors_per_cluster,
        free_sectors, total_sectors, available, blocks
    };
    zero_mem(cpu.es, cpu.di, 44);
    wr16(cpu.es, cpu.di, 44);
    for (unsigned i = 0; i < 8; ++i)
        put32(cpu.es, (uint16_t)(cpu.di + 4 + i * 4),
              fields[i] > UINT32_MAX ? UINT32_MAX : (uint32_t)fields[i]);
    cpu.a.x = 0;
    return 0;
}

static int filesystem_info(void)
{
    char path[PATH_MAX];
    int error = memory_path(cpu.ds, cpu.d.x, path, false);
    if (error) return error;
    struct stat st;
    error = file_stat(path, &st);
    if (error) return error;
    if (!S_ISDIR(st.st_mode)) return 3;
    if (cpu.c.x && cpu.c.x < sizeof("LINUX")) return 122;
    if (cpu.c.x) write_string(cpu.es, cpu.di, "LINUX", cpu.c.x);
    cpu.a.x = 0;
    cpu.b.x = 0x4002; /* Case-preserved names and DOS long-filename support. */
    cpu.c.x = DOS_NAME_MAX;
    cpu.d.x = LFN_PATH_MAX;
    return 0;
}

static int country_info(void)
{
    if (cpu.d.x == 0xffff) return 1; /* Setting country is not supported. */
    unsigned country = cpu.a.l == 0xff ? cpu.b.x : cpu.a.l;
    if (country != 0 && country != 1) return 2;
    uint16_t seg = cpu.ds, off = cpu.d.x;
    zero_mem(seg, off, 32);
    wr16(seg, off, 1); /* date order D.M.Y, chosen by the owner on 2026-10-01 */
    wr8(seg, (uint16_t)(off + 2), '$');
    wr8(seg, (uint16_t)(off + 7), ',');
    wr8(seg, (uint16_t)(off + 9), '.');
    wr8(seg, (uint16_t)(off + 11), '.');
    wr8(seg, (uint16_t)(off + 13), ':');
    wr8(seg, (uint16_t)(off + 16), 2);
    wr8(seg, (uint16_t)(off + 17), 1);
    put32(seg, (uint16_t)(off + 18), UINT32_C(0xf0000100));
    wr8(seg, (uint16_t)(off + 22), ',');
    cpu.a.x = 1;
    cpu.b.x = 1;
    return 0;
}

static int handle_info(void)
{
    DosHandle *h = dos_handle(cpu.b.x);
    if (!h || h->fd < 0 || h->device != DEV_NONE) return 6;
    struct stat st;
    if (fstat(h->fd, &st) != 0) return dos_errno(errno);
    uint16_t seg = cpu.ds, off = cpu.d.x;
    uint64_t size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
    uint64_t inode = (uint64_t)st.st_ino;
    zero_mem(seg, off, 52);
    put32(seg, off, file_attr(h->path, &st));
    put64(seg, (uint16_t)(off + 4), to_filetime(birth_time(h->path, h->fd, &st)));
    put64(seg, (uint16_t)(off + 12), to_filetime(st.st_atim));
    put64(seg, (uint16_t)(off + 20), to_filetime(st.st_mtim));
    put32(seg, (uint16_t)(off + 28), (uint32_t)st.st_dev);
    put32(seg, (uint16_t)(off + 32), (uint32_t)(size >> 32));
    put32(seg, (uint16_t)(off + 36), (uint32_t)size);
    put32(seg, (uint16_t)(off + 40), st.st_nlink > UINT32_MAX ? UINT32_MAX : (uint32_t)st.st_nlink);
    put32(seg, (uint16_t)(off + 44), (uint32_t)(inode >> 32));
    put32(seg, (uint16_t)(off + 48), (uint32_t)inode);
    cpu.a.x = 0;
    return 0;
}

static int convert_filetime(void)
{
    if (cpu.b.l == 0) {
        struct timespec ts = from_filetime(get64(cpu.ds, cpu.si));
        uint16_t time, date;
        bool unclamped = pack_time(ts, &time, &date);
        cpu.c.x = time;
        cpu.d.x = date;
        cpu.b.h = unclamped ? (uint8_t)time_hundredths(ts) : 0;
    } else if (cpu.b.l == 1) {
        struct timespec ts;
        int error = unpack_time(cpu.c.x, cpu.d.x, cpu.b.h, &ts);
        if (error) return error;
        put64(cpu.es, cpu.di, to_filetime(ts));
    } else return 1;
    cpu.a.x = 0;
    return 0;
}

static int generate_shortname(void)
{
    if (cpu.d.l != 0 || cpu.d.h > 1) return 1;
    unsigned format = cpu.d.h;
    char source[DOS_PATH_MAX], dir[PATH_MAX], leaf[DOS_NAME_MAX + 1], alias[13];
    int error = read_string(cpu.ds, cpu.si, source, sizeof(source));
    if (error) return error;
    size_t length = strlen(source);
    if (!length || source[length - 1] == '/' || source[length - 1] == '\\' ||
        source[length - 1] == ':') return 3;
    error = split_pattern(source, dir, leaf);
    if (error) return error;
    Entry *entries;
    size_t count;
    error = list_directory(dir, &entries, &count);
    if (error) return error;

    size_t found = 0;
    unsigned matches = matching_entry(entries, count, leaf, &found);
    if (matches > 1) { free_entries(entries, count); return 2; }
    if (matches == 1) strcpy(alias, entries[found].alias);
    else {
        char host[4 * (DOS_NAME_MAX + 1)];
        bool reserved = !cp_to_utf8(leaf, host, sizeof(host)) && reserved_host_alias(dir, host, alias);
        if (!reserved && valid_short(leaf, alias)) reserved = reserved_alias(dir, alias);
        if (!reserved && (!valid_short(leaf, alias) || alias_used(dir, entries, count, alias))) {
            unsigned n;
            for (n = 1; n <= 9999999; ++n) {
                numbered_alias(leaf, n, alias);
                if (!alias_used(dir, entries, count, alias)) break;
            }
            if (n > 9999999) { free_entries(entries, count); return 4; }
        }
    }
    free_entries(entries, count);
    if (format == 1) error = write_string(cpu.es, cpu.di, alias, 13);
    else {
        const char *dot = strchr(alias, '.');
        if (!strcmp(alias, ".") || !strcmp(alias, "..")) dot = NULL;
        size_t base_len = dot ? (size_t)(dot - alias) : strlen(alias);
        size_t ext_len = dot ? strlen(dot + 1) : 0;
        for (unsigned i = 0; i < 11; ++i) {
            uint8_t byte = ' ';
            if (i < 8 && i < base_len) byte = (uint8_t)alias[i];
            if (i >= 8 && i - 8 < ext_len) byte = (uint8_t)dot[1 + i - 8];
            wr8(cpu.es, (uint16_t)(cpu.di + i), byte);
        }
    }
    if (!error) cpu.a.x = 0;
    return error;
}
