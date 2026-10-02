#define _GNU_SOURCE
/* Fault injection only: replace VCPLAY.LCK after a successful fcntl lock,
 * before returning to dos_core's required post-lock pathname check. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void replace_lock(int fd) {
    static int replaced;
    if (replaced) return;
    char descriptor[64], path[4096], moved[4096];
    snprintf(descriptor, sizeof descriptor, "/proc/self/fd/%d", fd);
    ssize_t length = readlink(descriptor, path, sizeof path - 1);
    if (length < 11 || (size_t)length + 5 >= sizeof moved) return;
    path[length] = 0;
    if (strcmp(path + length - 11, "/VCPLAY.LCK")) return;
    replaced = 1;
    snprintf(moved, sizeof moved, "%s.OLD", path);
    if (rename(path, moved)) _exit(81);
    int replacement = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (replacement < 0) _exit(82);
    close(replacement);
}

int fcntl(int fd, int command, ...) {
    static int (*original)(int, int, ...);
    if (!original) original = dlsym(RTLD_NEXT, "fcntl");
    if (!original) _exit(83);
    switch (command) {
    case F_GETFD: case F_GETFL: case F_GETOWN: case F_GETSIG:
    case F_GETLEASE: case F_GETPIPE_SZ: case F_GET_SEALS:
        return original(fd, command);
    }
    va_list args;
    va_start(args, command);
    int result;
    switch (command) {
    case F_DUPFD: case F_DUPFD_CLOEXEC: case F_SETFD: case F_SETFL:
    case F_SETOWN: case F_SETSIG: case F_SETLEASE: case F_NOTIFY:
    case F_SETPIPE_SZ: case F_ADD_SEALS:
        result = original(fd, command, va_arg(args, int));
        break;
    case F_OFD_SETLK: {
        struct flock *lock = va_arg(args, struct flock *);
        result = original(fd, command, lock);
        if (!result && lock->l_type == F_WRLCK) replace_lock(fd);
        break;
    }
    default:
        result = original(fd, command, va_arg(args, void *));
        break;
    }
    va_end(args);
    return result;
}
