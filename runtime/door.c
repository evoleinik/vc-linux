/* Native BBS door lifecycle. The original node-pty PID runs DOS; a detached
 * child owns its temporary directory and watches a lifetime pipe. Even a
 * fatal signal (or SIGKILL) in the DOS process therefore leaves a live janitor.
 * Neither process execs a host program. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "door_confinement.h"
#include "door_demo.h"
#include "dos_fs.h"
#include "hle.h"
#include "modem.h"
#include "modem_transport.h"
#include "rt.h"
#include "term.h"

#define DOOR_QUOTA (UINT64_C(8) * 1024 * 1024)

static int lifetime_fd = -1;
static pid_t watchdog_pid = -1;
static volatile sig_atomic_t watchdog_signal;

#ifdef VC_DOOR_TEST_HOOKS
/* Only the confinement test binary can see these, to attack its janitor. */
pid_t door_test_janitor(void) { return watchdog_pid; }
int door_test_lifetime_fd(void) { return lifetime_fd; }
#endif

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) _exit(1);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        size -= (size_t)n;
    }
    return 0;
}

typedef struct { char name[NAME_MAX + 1]; } CleanupPart;

static int directory_at(int root, const CleanupPart *parts, size_t count) {
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    for (size_t i = 0; i < count; ++i) {
        int next = openat(fd, parts[i].name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        if (next < 0) { errno = saved; return -1; }
        fd = next;
    }
    return fd;
}

/* Reopen from the owned root when descending/ascending, retaining only a
 * bounded number of descriptors regardless of the caller's directory depth.
 * Store individual names, never a concatenated pathname: DOS can rename a
 * short subtree repeatedly to build paths longer than the host's PATH_MAX.
 * No symlink or ".." traversal can direct cleanup outside that root. */
static int remove_contents(int root) {
    CleanupPart *parts = NULL;
    size_t count = 0, capacity = 0;
    int result = -1;
    for (;;) {
        int current = directory_at(root, parts, count);
        if (current < 0) break;
        if (fchmod(current, 0700)) { close(current); break; }
        DIR *dir = fdopendir(current);
        if (!dir) { close(current); break; }
        int descended = 0, failed = 0;
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) { if (errno) failed = 1; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            struct stat st;
            if (fstatat(current, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { failed = 1; break; }
            if (S_ISDIR(st.st_mode)) {
                if (count == capacity) {
                    if (capacity > SIZE_MAX / (2 * sizeof(*parts))) { failed = 1; break; }
                    size_t next = capacity ? capacity * 2 : 32;
                    CleanupPart *grown = realloc(parts, next * sizeof(*parts));
                    if (!grown) { failed = 1; break; }
                    parts = grown;
                    capacity = next;
                }
                size_t length = strlen(entry->d_name);
                if (length >= sizeof parts[count].name) { failed = 1; break; }
                memcpy(parts[count++].name, entry->d_name, length + 1);
                descended = 1;
                break;
            }
            if (unlinkat(current, entry->d_name, 0)) { failed = 1; break; }
        }
        closedir(dir);
        if (failed) break;
        if (descended) continue;
        if (!count) { result = 0; break; }
        --count;
        int parent = directory_at(root, parts, count);
        if (parent < 0) break;
        int removed = unlinkat(parent, parts[count].name, AT_REMOVEDIR);
        close(parent);
        if (removed) break;
    }
    free(parts);
    return result;
}

/* An orderly exit has already restored the terminal (term_shutdown runs
 * first, atexit is last-in first-out), so tell the janitor to stay off it.
 * The byte grants nothing else: the pipe closes only when this process
 * exits, and the janitor keeps enforcing the limit until the worker is gone.
 * It must outlive the worker, so the worker never waits for it. */
static void finish_watchdog(void) {
    if (lifetime_fd >= 0) (void)write_all(lifetime_fd, "D", 1);
}

static void watch_signal(int number) { watchdog_signal = number; }

typedef struct { int error; char name[64]; } Ready;

static void watchdog(int rootfd, const char *root, const char *node,
                      int life, int readyfd, uint64_t duration,
                      pid_t worker, const struct termios *tty, int tty_flags,
                      const sigset_t *original_mask) {
    /* Do not receive the BBS's process-group HUP along with the DOS worker.
     * Keep our tty descriptors solely for carrier detection and restoration. */
    Ready ready = {0};
    if (setsid() < 0) ready.error = errno;
    struct sigaction action = {.sa_handler = watch_signal};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM};
    for (size_t i = 0; i < sizeof signals / sizeof *signals; ++i)
        sigaction(signals[i], &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    sigprocmask(SIG_SETMASK, original_mask, NULL);

    char path[PATH_MAX];
    int size = snprintf(path, sizeof path, "%s/node-%s-XXXXXX", root, node);
    if (size < 0 || (size_t)size >= sizeof path) ready.error = ENAMETOOLONG;
    int session = -1;
    if (!ready.error) {
        if (!mkdtemp(path)) ready.error = errno;
        else {
            snprintf(ready.name, sizeof ready.name, "%s", strrchr(path, '/') + 1);
            session = openat(rootfd, ready.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (session < 0 || fchmod(session, 0700)) ready.error = errno;
        }
    }
    (void)write_all(readyfd, &ready, sizeof ready);
    close(readyfd);
    int timed_out = 0, stopping = 0, quiet = 0, life_open = 1;
    uint64_t deadline = monotonic_ms() + duration, stop_at = 0, closed_at = 0;
    if (!ready.error) {
        for (;;) {
            struct pollfd fds[] = {{.fd = life_open ? life : -1, .events = POLLIN},
                                   {.fd = STDIN_FILENO}};
            int polled = poll(fds, 2, 50);
            if (polled < 0 && errno != EINTR) watchdog_signal = SIGTERM;
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                char note[16];
                ssize_t n = read(life, note, sizeof note);
                if (n > 0) quiet |= memchr(note, 'D', (size_t)n) != NULL;
                else if (n == 0 || errno != EINTR) { life_open = 0; closed_at = monotonic_ms(); }
            }
            /* Only reparenting proves the worker is gone: while it is our
             * parent its PID cannot be reused, so signalling it is safe. */
            if (getppid() != worker) break;
            uint64_t now = monotonic_ms();
            /* The pipe closes when the worker exits, and it is reparented
             * microseconds later. A worker still here half a second after
             * closing it did so on purpose, to escape this loop: end it. */
            int abandoned = !life_open && now >= closed_at + 500;
            if (!stopping && (now >= deadline || watchdog_signal || abandoned ||
                              (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)))) {
                timed_out = now >= deadline;
                kill(worker, SIGTERM);
                stopping = 1;
                stop_at = now + 1000;
            }
            if (stopping && now >= stop_at) kill(worker, SIGKILL);
        }
    }
    close(life);
    int failed = 0;
    if (session >= 0) {
        struct stat owned, named;
        if (remove_contents(session)) failed = 1;
        /* If an administrator moved/replaced the session, never delete the
         * replacement merely because it now has the old directory's name. */
        if (fstat(session, &owned) || fstatat(rootfd, ready.name, &named, AT_SYMLINK_NOFOLLOW) ||
            owned.st_dev != named.st_dev || owned.st_ino != named.st_ino ||
            unlinkat(rootfd, ready.name, AT_REMOVEDIR)) failed = 1;
        close(session);
    } else if (ready.name[0] && unlinkat(rootfd, ready.name, AT_REMOVEDIR)) failed = 1;
    close(rootfd);
    /* Cleanup must never wait for a dropped/stalled caller to read output.
     * After deleting H:, restore tty state even after SIGKILL and send only
     * bounded nonblocking final output. A full tty can lose this notice, but
     * cannot keep either the worker or its private files alive. A worker
     * that exited in order already restored the terminal, and the BBS owns
     * it again now: write nothing then. */
    int terminal = !ready.error && !(quiet && !stopping);
    if (terminal) (void)tcsetattr(STDIN_FILENO, TCSANOW, tty);
    int out_flags = terminal ? fcntl(STDOUT_FILENO, F_GETFL) : -1;
    if (out_flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, out_flags | O_NONBLOCK);
    if (terminal) {
        static const char leave[] = "\033[0m\033[?25h\033[?1l\033>\033[?7h\033[?1049l";
        (void)write_all(STDOUT_FILENO, leave, sizeof leave - 1);
    }
    if (timed_out) {
        static const char message[] = "vc: door session time limit reached.\r\n";
        (void)write_all(STDOUT_FILENO, message, sizeof message - 1);
    }
    if (failed) {
        static const char warning[] = "vc: cannot remove door session directory\r\n";
        /* stderr may be a separate, full logging pipe rather than our tty.
         * Skip this best-effort diagnostic if nonblocking mode is unavailable. */
        int err_flags = fcntl(STDERR_FILENO, F_GETFL);
        if (err_flags >= 0 && !fcntl(STDERR_FILENO, F_SETFL, err_flags | O_NONBLOCK)) {
            (void)write_all(STDERR_FILENO, warning, sizeof warning - 1);
            (void)fcntl(STDERR_FILENO, F_SETFL, err_flags);
        }
    }
    if (out_flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, out_flags);
    if (terminal && tty_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, tty_flags);
    _exit(failed || ready.error ? 1 : 0);
}

/* Create all demo paths relative to an already open private root. Only the
 * build-time shared demo generator supplies names/data, never caller input. */
static int install(int root, const EmbeddedFile *file, uint64_t *total) {
    if (file->size > DOOR_QUOTA - *total) { errno = ENOSPC; return -1; }
    char path[PATH_MAX];
    if (strlen(file->name) >= sizeof path) { errno = ENAMETOOLONG; return -1; }
    strcpy(path, file->name);
    int parent = dup(root);
    if (parent < 0) return -1;
    char *part = path;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
            close(parent); errno = EINVAL; return -1;
        }
        if (!slash) break;
        if (mkdirat(parent, part, 0700) && errno != EEXIST) { close(parent); return -1; }
        int next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        if (next < 0) return -1;
        parent = next;
        part = slash + 1;
    }
    int fd = openat(parent, part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(parent);
    if (fd < 0) return -1;
    int result = write_all(fd, file->data, file->size);
    if (close(fd)) result = -1;
    if (!result) *total += file->size;
    return result;
}

static int populate(int session) {
    uint64_t total = 0;
    for (int i = 0; i < door_demo_file_count; ++i)
        if (install(session, door_demo_files + i, &total)) return -1;
    /* The demo list stays shared with the browser. Only VC's own internal
     * program/settings files are additional, hidden on H: instead of exposing
     * the browser's separate /var config directory through a host C: drive. */
    for (int i = 0; i < embedded_file_count; ++i) {
        const EmbeddedFile *file = embedded_files + i;
        if (strncmp(file->name, "VC.", 3) && strcmp(file->name, "VCEDIT.EXT")) continue;
        char name[64];
        int n = snprintf(name, sizeof name, ".VC/%s", file->name);
        if (n < 0 || (size_t)n >= sizeof name) { errno = ENAMETOOLONG; return -1; }
        const EmbeddedFile *contents = !strcmp(file->name, "VC.INI") ? &door_default_ini : file;
        EmbeddedFile hidden = {name, contents->data, contents->size};
        if (install(session, &hidden, &total)) return -1;
    }
    return 0;
}

int door_main(int argc, char **argv) {
    uint64_t duration = UINT64_C(60) * 60 * 1000;
    const char *program = NULL;
    int seen_door = 0, seen_minutes = 0, allow_unconfined = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--door") && !seen_door) { seen_door = 1; continue; }
        if (!strcmp(argv[i], "--door-allow-unconfined") && !allow_unconfined) {
            allow_unconfined = 1;
            continue;
        }
        if (!strcmp(argv[i], "--door-run") && !program && i + 1 < argc && argv[i + 1][0]) {
            program = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--door-minutes") && !seen_minutes && i + 1 < argc) {
            char *end;
            errno = 0;
            double minutes = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end || !isfinite(minutes) ||
                minutes <= 0 || minutes > 1000000) goto arguments;
            duration = (uint64_t)(minutes * 60000);
            if (!duration) goto arguments;
            seen_minutes = 1;
            continue;
        }
        goto arguments;
    }
    if (!seen_door) goto arguments;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("vc: door needs a terminal on stdin and stdout\n", stderr);
        return 2;
    }
    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) || size.ws_col < 80 || size.ws_row < 25) {
        fputs("vc: door requires a terminal of at least 80x25.\n", stderr);
        return 2;
    }
    const char *configured = getenv("VC_DOOR_ROOT"), *node = getenv("VC_DOOR_NODE");
    if (!configured || configured[0] != '/') {
        fputs("vc: --door requires an absolute VC_DOOR_ROOT directory\n", stderr);
        return 2;
    }
    if (!node) node = "0";
    if (!*node || strlen(node) > 9 || strspn(node, "0123456789") != strlen(node)) {
        fputs("vc: VC_DOOR_NODE must contain at most nine digits\n", stderr);
        return 2;
    }
    char root[PATH_MAX];
    if (!realpath(configured, root)) { perror("vc: VC_DOOR_ROOT"); return 1; }
    int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat owner;
    if (rootfd < 0 || fstat(rootfd, &owner) || owner.st_uid != geteuid() || (owner.st_mode & 0077)) {
        if (rootfd >= 0) close(rootfd);
        fputs("vc: VC_DOOR_ROOT must be a private directory owned by the door user\n", stderr);
        return 2;
    }
    struct termios tty;
    int tty_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (tcgetattr(STDIN_FILENO, &tty)) { close(rootfd); perror("vc: door terminal"); return 1; }
    int life[2], handshake[2];
    if (pipe2(life, O_CLOEXEC)) { close(rootfd); perror("vc: door pipe"); return 1; }
    if (pipe2(handshake, O_CLOEXEC)) {
        close(rootfd); close(life[0]); close(life[1]); perror("vc: door pipe"); return 1;
    }
    sigset_t blocked, original_mask;
    sigfillset(&blocked);
    sigprocmask(SIG_BLOCK, &blocked, &original_mask);
    signal(SIGCHLD, SIG_DFL);
    umask(077);
    pid_t worker = getpid();
    watchdog_pid = fork();
    if (!watchdog_pid) {
        close(life[1]); close(handshake[0]);
        watchdog(rootfd, root, node, life[0], handshake[1], duration,
                 worker, &tty, tty_flags, &original_mask);
    }
    close(life[0]); close(handshake[1]);
    lifetime_fd = life[1];
    int registered = watchdog_pid > 0 && !atexit(finish_watchdog);
    sigprocmask(SIG_SETMASK, &original_mask, NULL);
    if (!registered) {
        close(handshake[0]); close(rootfd); finish_watchdog();
        fputs("vc: cannot start door cleanup watchdog\n", stderr);
        return 1;
    }
    Ready ready;
    size_t received = 0;
    while (received < sizeof ready) {
        ssize_t n = read(handshake[0], (char *)&ready + received, sizeof ready - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(handshake[0]);
    int session = received == sizeof ready && !ready.error ?
        openat(rootfd, ready.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    close(rootfd);
    if (session < 0) { fputs("vc: cannot create private door session\n", stderr); return 1; }
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/%s", root, ready.name);
    if (length < 0 || (size_t)length >= sizeof path || fchdir(session)) {
        close(session); fputs("vc: cannot enter door session\n", stderr); return 1;
    }
    rt_set_logging(0);
    embedded_files_init();
    int installed = populate(session);
    if (installed) { close(session); perror("vc: cannot populate door drive"); return 1; }
    dos_core_init();
    dos_core_set_door(1);
    rt_register_supplement(&image_gwbasic, run_gwbasic_graphics);
    bios_init();
    modem_init(modem_transport_ops());
    modem_set_door(1);
    atexit(modem_reset);
    if (dos_fs_init_door(path, DOOR_QUOTA)) {
        close(session);
        fputs("vc: cannot confine door drive\n", stderr); return 1;
    }
    /* Cache the host timezone before Landlock blocks /etc/localtime. All
     * future guest-facing file opens now resolve only beneath the private H:. */
    tzset();
    int confined = door_confine(session, allow_unconfined);
    close(session);
    if (confined) return 1;
#ifdef VC_DOOR_TEST_HOOKS
    /* This hook is linked only into the dedicated confinement test binary. */
    extern int door_confinement_test_hook(void);
    int hook_result = door_confinement_test_hook();
    if (hook_result >= 0) return hook_result;
#endif
    term_set_door(1);
    term_init();
    atexit(term_shutdown);
    int error = dos_door_start(program);
    if (!error) rt_run();
    modem_reset();
    term_shutdown();
    if (error) {
        fprintf(stderr, "vc: cannot start door program (DOS error %d)\n", error);
        return 1;
    }
    return rt_exit_code;

arguments:
    fputs("usage: vc --door [--door-minutes N] [--door-run PROGRAM] [--door-allow-unconfined]\n", stderr);
    return 2;
}
