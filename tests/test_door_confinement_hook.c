/* Deliberately bypass all DOS path checks. Never linked into a release build. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern pid_t door_test_janitor(void);
extern int door_test_lifetime_fd(void);

int door_confinement_test_failure(const char *stage) {
    const char *failure = getenv("VC_DOOR_TEST_FAILURE");
    return failure && !strcmp(failure, stage);
}

int door_confinement_test_hook(void) {
    const char *hook = getenv("VC_DOOR_TEST_HOOK");
    if (!hook) return -1;
    if (!strcmp(hook, "inside")) {
        static const char contents[] = "native write inside private H";
        char readback[sizeof contents] = {0};
        int fd = open("CONFINED.TXT", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) return 94;
        int good = write(fd, contents, sizeof contents) == sizeof contents &&
            lseek(fd, 0, SEEK_SET) == 0 &&
            read(fd, readback, sizeof readback) == sizeof readback &&
            !memcmp(readback, contents, sizeof contents);
        close(fd);
        if (unlink("CONFINED.TXT")) good = 0;
        dprintf(STDOUT_FILENO, "door-test inside: %s\n", good ? "read/write/delete OK" : "FAILED");
        return good ? 0 : 94;
    }
    if (!strcmp(hook, "outside")) {
        const char *path = getenv("VC_DOOR_TEST_OUTSIDE");
        if (!path) return 90;
        errno = 0;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        int error = errno;
        if (fd >= 0) close(fd);
        dprintf(STDOUT_FILENO, "door-test outside: opened=%d errno=%d\n", fd >= 0, error);
        return fd < 0 && error == EACCES ? 0 : 91;
    }
    if (!strcmp(hook, "execve")) {
        char *const args[] = {"door-test-missing-executable", NULL};
        char *const env[] = {NULL};
        /* A missing file is intentional: without seccomp this safely returns
         * ENOENT, whereas the filter kills even before pathname resolution. */
        syscall(SYS_execve, "/__vc_door_test_missing_executable__", args, env);
        dprintf(STDOUT_FILENO, "door-test execve survived: errno=%d\n", errno);
        return 92;
    }
    if (!strcmp(hook, "terminal")) {
        /* Every terminal ioctl the filter allows must still work, so a wrong
         * hand-counted BPF jump cannot pass by killing the allowed ones too. */
        struct termios mode;
        struct winsize size;
        int queued = 0;
        int good = !ioctl(STDIN_FILENO, TCGETS, &mode) &&
                   !ioctl(STDIN_FILENO, TCSETS, &mode) &&
                   !ioctl(STDIN_FILENO, TCSETSW, &mode) &&
                   !ioctl(STDIN_FILENO, TCSETSF, &mode) &&
                   !ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) &&
                   !ioctl(STDIN_FILENO, FIONREAD, &queued) &&
                   !ioctl(STDIN_FILENO, TCFLSH, TCIFLUSH);
        /* fcntl is allowed except for redirecting file signals (F_SETOWN). */
        int flags = fcntl(STDIN_FILENO, F_GETFL), descriptor = fcntl(STDIN_FILENO, F_GETFD);
        int copy = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
        good = good && flags >= 0 && descriptor >= 0 && copy >= 0 &&
               !fcntl(STDIN_FILENO, F_SETFL, flags) && !fcntl(STDIN_FILENO, F_SETFD, descriptor) &&
               fcntl(STDIN_FILENO, F_GETOWN) >= 0 && !close(copy);
        dprintf(STDOUT_FILENO, "door-test terminal: %s\n", good ? "ioctls OK" : "FAILED");
        return good ? 0 : 96;
    }
    if (!strcmp(hook, "sigio-janitor")) {
        /* Aim SIGIO, as SIGKILL, at the janitor: same uid, so the kernel's
         * own fowner permission check allows it. One input byte fires it. */
        pid_t janitor = door_test_janitor();
        int flags = fcntl(STDIN_FILENO, F_GETFL);
        if (janitor <= 0 || flags < 0 || fcntl(STDIN_FILENO, F_SETOWN, janitor) ||
            fcntl(STDIN_FILENO, F_SETSIG, SIGKILL) ||
            fcntl(STDIN_FILENO, F_SETFL, flags | O_ASYNC)) {
            dprintf(STDOUT_FILENO, "door-test sigio setup failed: errno=%d\n", errno);
            return 97;
        }
        dprintf(STDOUT_FILENO, "door-test sigio armed\n");
        char byte;
        while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) {}
        struct timespec settle = {0, 300000000};
        nanosleep(&settle, NULL);
        dprintf(STDOUT_FILENO, "door-test sigio sent\n");
        return 0;
    }
    if (!strcmp(hook, "signal-janitor")) {
        errno = 0;
        int sent = kill(door_test_janitor(), 0);
        int error = errno;
        dprintf(STDOUT_FILENO, "door-test signal janitor: result=%d errno=%d\n", sent, error);
        return sent < 0 && error == EPERM ? 0 : 98;
    }
    if (!strcmp(hook, "close-lifetime")) {
        /* Drop the janitor's lifetime pipe, then outstay the time limit. */
        close(door_test_lifetime_fd());
        dprintf(STDOUT_FILENO, "door-test lifetime pipe closed\n");
        for (;;) {
            struct timespec second = {1, 0};
            nanosleep(&second, NULL);
        }
    }
    if (!strcmp(hook, "execveat")) syscall(SYS_execveat, -1, "", NULL, NULL, 0);
    else if (!strcmp(hook, "socket")) syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
    else if (!strcmp(hook, "connect")) syscall(SYS_connect, -1, NULL, 0);
    else if (!strcmp(hook, "ptrace")) syscall(SYS_ptrace, PTRACE_PEEKDATA, -1, NULL, NULL);
    else if (!strcmp(hook, "mount")) syscall(SYS_mount, NULL, NULL, NULL, 0, NULL);
    else if (!strcmp(hook, "bpf")) syscall(SYS_bpf, 0, NULL, 0);
    else if (!strcmp(hook, "io_uring")) syscall(SYS_io_uring_setup, 0, NULL);
    else if (!strcmp(hook, "clone3")) syscall(SYS_clone3, NULL, 0);
    else if (!strcmp(hook, "mmap-executable"))
        syscall(SYS_mmap, NULL, 4096, PROT_READ | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    else if (!strcmp(hook, "ioctl-inject")) syscall(SYS_ioctl, STDIN_FILENO, TIOCSTI, NULL);
    else if (!strcmp(hook, "kill-other")) syscall(SYS_kill, 1, 0);
    else if (!strcmp(hook, "utimens-path"))
        syscall(SYS_utimensat, AT_FDCWD, "/__vc_door_test_missing_file__", NULL, 0);
#if defined(__x86_64__)
    else if (!strcmp(hook, "x32")) syscall(UINT32_C(0x40000000) | SYS_getpid);
    else if (!strcmp(hook, "compat-i386")) {
        /* i386 getpid: architecture validation must run before numeric rules. */
        long number = 20;
        __asm__ volatile("int $0x80" : "+a"(number) : : "memory");
    }
#endif
    else return 93;
    dprintf(STDOUT_FILENO, "door-test forbidden syscall survived: %s errno=%d\n", hook, errno);
    return 95;
}
