/* Plain Linux UAPI: also linked by the static aarch64-linux-musl release. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "door_confinement.h"

/* Build hosts may have older headers than the deployment kernel. These stable
 * UAPI bits are used only after the corresponding ABI has been reported. */
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (UINT64_C(1) << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (UINT64_C(1) << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (UINT64_C(1) << 15)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (UINT64_C(1) << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (UINT64_C(1) << 1)
#endif
#ifndef LANDLOCK_SCOPE_SIGNAL
#define LANDLOCK_SCOPE_SIGNAL (UINT64_C(1) << 1)
#endif

#if defined(__x86_64__) && !defined(__ILP32__)
#define DOOR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define DOOR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "door seccomp policy supports native x86-64 and AArch64 only"
#endif

#ifdef VC_DOOR_TEST_HOOKS
extern int door_confinement_test_failure(const char *stage);
#endif

enum { LANDLOCK_READY, LANDLOCK_UNAVAILABLE, LANDLOCK_FAILED };

static int unavailable_error(void) {
    return errno == ENOSYS || errno == EOPNOTSUPP || errno == EPERM || errno == EACCES;
}

static int confine_filesystem(int session_fd) {
#ifdef VC_DOOR_TEST_HOOKS
    if (door_confinement_test_failure("landlock")) return LANDLOCK_UNAVAILABLE;
#endif
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                          LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return unavailable_error() ? LANDLOCK_UNAVAILABLE : LANDLOCK_FAILED;
    if (!abi) return LANDLOCK_UNAVAILABLE;
    uint64_t handled = (UINT64_C(1) << 13) - 1; /* All ABI 1 filesystem rights. */
    if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5) handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    /* Pass exactly the ABI's own prefix, whatever the build headers hold. No
     * network rule is added: handling bind/connect makes both deny-by-default.
     * ABI 6 signal scoping stops this worker signalling anything outside its
     * domain, above all its unconfined janitor, by kill(2) or by SIGIO. */
    struct { uint64_t handled_access_fs, handled_access_net, scoped; } rules = {
        .handled_access_fs = handled,
        .handled_access_net = abi >= 4 ? LANDLOCK_ACCESS_NET_BIND_TCP |
                                       LANDLOCK_ACCESS_NET_CONNECT_TCP : 0,
        .scoped = abi >= 6 ? LANDLOCK_SCOPE_SIGNAL : 0,
    };
    size_t size = abi >= 6 ? sizeof rules : abi >= 4 ? 2 * sizeof(uint64_t) : sizeof(uint64_t);
    int fd = (int)syscall(SYS_landlock_create_ruleset, &rules, size, 0);
    if (fd < 0) return unavailable_error() ? LANDLOCK_UNAVAILABLE : LANDLOCK_FAILED;
    struct landlock_path_beneath_attr path = {
        .parent_fd = session_fd,
        .allowed_access = handled & (LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE),
    };
    /* All host code is already mapped; no binary/library reopen or execute
     * allowance is needed. Devices, sockets, symlinks and execution stay denied
     * even within H:. The inherited terminal and janitor pipe stay usable. */
    int result = LANDLOCK_FAILED;
    if (!syscall(SYS_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH, &path, 0)) {
        if (!syscall(SYS_landlock_restrict_self, fd, 0)) result = LANDLOCK_READY;
        else if (unavailable_error()) result = LANDLOCK_UNAVAILABLE;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

#define ALLOW_SYSCALL(name) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##name, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define ARG_LOW(index) (offsetof(struct seccomp_data, args[index]))
#define ARG_HIGH(index) (ARG_LOW(index) + sizeof(uint32_t))
#define ALLOW_VALUE(value) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (value), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int confine_syscalls(void) {
#ifdef VC_DOOR_TEST_HOOKS
    if (door_confinement_test_failure("seccomp")) { errno = EPERM; return -1; }
    /* Isolates Landlock in the test binary only; never a production path. */
    if (door_confinement_test_failure("seccomp-off")) return 0;
#endif
    const uint32_t self = (uint32_t)getpid();
    struct sock_filter filter[] = {
        /* x86 int 0x80 and any other compat ABI must not reinterpret numbers. */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DOOR_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#if defined(__x86_64__)
        /* x32 shares AUDIT_ARCH_X86_64 but uses bit 30 in every syscall. */
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, UINT32_C(0x40000000), 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
        /* No executable anonymous mappings or post-startup permission flips. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 0, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LOW(2)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* Only the terminal operations used by term.c, not arbitrary ioctls
         * such as TIOCSTI or device-specific command surfaces. The jump
         * skips 1 + 1 + 1 + 1 + 2 per allowed value + 1 instructions. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 19),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HIGH(1)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LOW(1)),
        ALLOW_VALUE(TCGETS),
        ALLOW_VALUE(TCSETS),
        ALLOW_VALUE(TCSETSW),
        ALLOW_VALUE(TCSETSF),
        ALLOW_VALUE(TIOCGWINSZ),
        ALLOW_VALUE(FIONREAD),
        ALLOW_VALUE(TCFLSH), /* term_flush_input's fallback (tcflush) */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        /* The deployment Landlock ABI does not mediate path-based timestamps.
         * futimens uses utimensat(fd, NULL, ...): reject non-null pathnames. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_utimensat, 0, 7),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HIGH(1)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LOW(1)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* F_SETOWN would let SIGIO (any signal, via F_SETSIG) reach another
         * process, such as the janitor. Kernels before Landlock ABI 6 cannot
         * scope that, so seccomp refuses to redirect file signals at all. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fcntl, 0, 6),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LOW(1)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETOWN, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETOWN_EX, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETSIG, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* Fatal-signal cleanup must re-raise to this worker, never a sibling
         * node or the unconstrained janitor. There is only one worker thread. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_kill, 0, 7),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_HIGH(0)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LOW(0)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, self, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        /* Everything not explicitly needed below kills the process. This
         * includes execve/execveat, all socket operations, ptrace, mount and
         * namespaces, fork/clone, bpf, perf, io_uring, keyrings and new syscalls. */
        ALLOW_SYSCALL(read), ALLOW_SYSCALL(write),
        ALLOW_SYSCALL(readv), ALLOW_SYSCALL(writev),
        ALLOW_SYSCALL(close), ALLOW_SYSCALL(lseek),
        ALLOW_SYSCALL(pread64), ALLOW_SYSCALL(pwrite64),
        ALLOW_SYSCALL(openat), ALLOW_SYSCALL(fstat),
        ALLOW_SYSCALL(newfstatat), ALLOW_SYSCALL(statx),
        ALLOW_SYSCALL(readlinkat), ALLOW_SYSCALL(getdents64),
        ALLOW_SYSCALL(dup), ALLOW_SYSCALL(dup3),
        ALLOW_SYSCALL(getcwd), ALLOW_SYSCALL(mkdirat),
        ALLOW_SYSCALL(unlinkat), ALLOW_SYSCALL(renameat2),
        ALLOW_SYSCALL(ftruncate), ALLOW_SYSCALL(fchmod),
        ALLOW_SYSCALL(fsync), ALLOW_SYSCALL(fdatasync),
        ALLOW_SYSCALL(faccessat),
#ifdef SYS_faccessat2
        ALLOW_SYSCALL(faccessat2),
#endif
        ALLOW_SYSCALL(statfs), ALLOW_SYSCALL(fstatfs),
        ALLOW_SYSCALL(munmap), ALLOW_SYSCALL(mremap),
        ALLOW_SYSCALL(brk), ALLOW_SYSCALL(madvise), ALLOW_SYSCALL(futex),
        ALLOW_SYSCALL(getrandom), ALLOW_SYSCALL(clock_gettime),
        ALLOW_SYSCALL(clock_getres), ALLOW_SYSCALL(gettimeofday),
        ALLOW_SYSCALL(nanosleep), ALLOW_SYSCALL(clock_nanosleep),
        ALLOW_SYSCALL(ppoll), ALLOW_SYSCALL(pselect6),
        ALLOW_SYSCALL(restart_syscall), ALLOW_SYSCALL(sched_yield),
        ALLOW_SYSCALL(rt_sigaction), ALLOW_SYSCALL(rt_sigprocmask),
        ALLOW_SYSCALL(rt_sigreturn), ALLOW_SYSCALL(sigaltstack),
        ALLOW_SYSCALL(getpid), ALLOW_SYSCALL(getppid), ALLOW_SYSCALL(gettid),
        ALLOW_SYSCALL(getuid), ALLOW_SYSCALL(geteuid),
        ALLOW_SYSCALL(getgid), ALLOW_SYSCALL(getegid),
        ALLOW_SYSCALL(wait4), ALLOW_SYSCALL(waitid),
        ALLOW_SYSCALL(exit), ALLOW_SYSCALL(exit_group),
#if defined(__x86_64__)
        /* AArch64 has only *at/new-style calls. x86 glibc/musl may use these. */
        ALLOW_SYSCALL(open), ALLOW_SYSCALL(stat), ALLOW_SYSCALL(lstat),
        ALLOW_SYSCALL(readlink), ALLOW_SYSCALL(access), ALLOW_SYSCALL(dup2),
        /* VZ's private swap directory uses libc mkdtemp/rmdir. */
        ALLOW_SYSCALL(mkdir), ALLOW_SYSCALL(rmdir),
        ALLOW_SYSCALL(unlink), ALLOW_SYSCALL(time),
        ALLOW_SYSCALL(poll), ALLOW_SYSCALL(select),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };
    struct sock_fprog program = {
        .len = (unsigned short)(sizeof filter / sizeof *filter), .filter = filter,
    };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

int door_confine(int session_fd, int allow_unconfined) {
    /* Required by both unprivileged APIs. A failure can never be opted out. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        fputs("vc: cannot enable door no-new-privileges confinement\n", stderr);
        return -1;
    }
    int result = confine_filesystem(session_fd);
    if (result != LANDLOCK_READY) {
        if (result != LANDLOCK_UNAVAILABLE || !allow_unconfined) {
            fputs("vc: door Landlock confinement unavailable; refusing to start\n", stderr);
            return -1;
        }
        fputs("vc: WARNING: Landlock unavailable; --door-allow-unconfined is for tests only\n", stderr);
    }
    if (confine_syscalls()) {
        fputs("vc: cannot install door seccomp confinement; refusing to start\n", stderr);
        return -1;
    }
    return 0;
}
