/* Exercise the actual janitor with a small descriptor budget. Dead runtime
 * sections are removed by the linker, so this needs no fake DOS translation. */
#include "../runtime/door.c"
#include <sys/prctl.h>
#include <sys/resource.h>

Cpu cpu;
uint8_t mem[MEM_SIZE];
void con_write(const uint8_t *bytes, size_t size) { (void)bytes; (void)size; }
int dos_con_int21(void) { cpu.a.x = 0; cpu.cf = 0; return 1; }

static int dos_path_call(uint16_t ax, const char *source, const char *target) {
    memset(&cpu, 0, sizeof cpu);
    cpu.a.x = ax;
    cpu.ds = 0x1000; cpu.es = 0x2000;
    cpu.d.x = cpu.di = 0x100;
    for (size_t i = 0; i <= strlen(source); ++i)
        wr8(cpu.ds, (uint16_t)(cpu.d.x + i), (uint8_t)source[i]);
    if (target)
        for (size_t i = 0; i <= strlen(target); ++i)
            wr8(cpu.es, (uint16_t)(cpu.di + i), (uint8_t)target[i]);
    if (dos_fs_int21() && !cpu.cf) return 0;
    fprintf(stderr, "cleanup fixture: DOS %04x %s failed with %u\n", ax, source, cpu.a.x);
    return -1;
}

/* The regression leaves the old implementation's over-deep tree intact.
 * Lift its one known child to a short root name to reclaim that fixture even
 * on a red run, without relying on the production cleanup being tested. */
static int rescue_dos_tree(int root) {
    for (;;) {
        int dir = openat(root, "DEEPNEST", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir < 0) return errno == ENOENT ? 0 : -1;
        (void)unlinkat(dir, "outside", 0);
        (void)unlinkat(dir, "LEAF.DAT", 0);
        int lifted = renameat(dir, "DEEPNEST", root, "NEXTROOT");
        int saved = errno;
        close(dir);
        if (lifted && saved != ENOENT) return -1;
        if (unlinkat(root, "DEEPNEST", AT_REMOVEDIR)) return -1;
        if (lifted) return 0;
        if (renameat(root, "NEXTROOT", root, "DEEPNEST")) return -1;
    }
}

static int dos_rename_deep_cleanup(void) {
    enum { DEPTH = 512, BYTES = DEPTH * (sizeof "DEEPNEST" - 1) + DEPTH - 1 };
    char path[] = "/tmp/vc-door-dos-depth-XXXXXX";
    char outside[] = "/tmp/vc-door-outside-XXXXXX";
    if (!mkdtemp(path)) return 2;
    int sentinel = mkstemp(outside);
    int root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sentinel < 0 || root < 0 || write_all(sentinel, "untouched", 9)) return 2;
    if (dos_fs_init_door(path, UINT64_C(8) * 1024 * 1024) ||
        dos_path_call(0x3900, "H:\\DEEPNEST", NULL) ||
        dos_path_call(0x3c00, "H:\\DEEPNEST\\LEAF.DAT", NULL)) return 2;
    cpu.b.x = cpu.a.x; cpu.a.x = 0x3e00;
    if (!dos_fs_int21() || cpu.cf) return 2;
    int deepest = openat(root, "DEEPNEST", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (deepest < 0 || symlinkat(outside, deepest, "outside")) return 2;
    for (unsigned i = 1; i < DEPTH; ++i) {
        /* Every caller-controlled operation has a short DOS pathname. Moving
         * the existing subtree builds a 4607-byte relative path nonetheless. */
        if (dos_path_call(0x3900, "H:\\NEWROOT", NULL) ||
            dos_path_call(0x5600, "H:\\DEEPNEST", "H:\\NEWROOT\\DEEPNEST") ||
            dos_path_call(0x5600, "H:\\NEWROOT", "H:\\DEEPNEST")) return 2;
    }
    if (fchmod(deepest, 0500)) return 2;
    close(deepest);
    dos_fs_init();
    struct rlimit previous, low;
    if (getrlimit(RLIMIT_NOFILE, &previous)) return 2;
    low = previous; low.rlim_cur = 32;
    if (setrlimit(RLIMIT_NOFILE, &low)) return 2;
    int result = remove_contents(root);
    if (setrlimit(RLIMIT_NOFILE, &previous)) return 2;
    char bytes[9];
    int safe = pread(sentinel, bytes, sizeof bytes, 0) == sizeof bytes &&
               !memcmp(bytes, "untouched", sizeof bytes) && !access(outside, F_OK);
    /* The rescued deepest directory was made read-only, so restore it through
     * short descriptor steps if the deliberately red cleanup did not reach it. */
    if (result) {
        int cursor = openat(root, "DEEPNEST", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        while (cursor >= 0) {
            if (fchmod(cursor, 0700)) return 2;
            int child = openat(cursor, "DEEPNEST", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(cursor); cursor = child;
        }
        if (rescue_dos_tree(root)) return 2;
    }
    close(root); close(sentinel);
    if (rmdir(path) || unlink(outside)) return 2;
    printf("door cleanup: DOS mkdir/rename, %u levels / %u bytes, 32 descriptors, outside symlink: %s\n",
           DEPTH, (unsigned)BYTES, result || !safe ? "FAILED" : "passed");
    return result || !safe ? 1 : 0;
}

static int deep_tree_cleanup(void) {
    char path[] = "/tmp/vc-door-cleanup-XXXXXX";
    if (!mkdtemp(path)) return 2;
    int root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int cursor = dup(root);
    for (unsigned i = 0; i < 80; ++i) {
        if (mkdirat(cursor, "d", 0700)) return 2;
        int child = openat(cursor, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) return 2;
        close(cursor);
        cursor = child;
    }
    /* A symlink in the deepest directory must not direct deletion outside
     * the owned tree. The linked target is the original test root itself. */
    if (symlinkat(path, cursor, "outside")) return 2;
    close(cursor);
    struct rlimit previous, low;
    if (getrlimit(RLIMIT_NOFILE, &previous)) return 2;
    low = previous;
    low.rlim_cur = 32;
    if (setrlimit(RLIMIT_NOFILE, &low)) return 2;
    int result = remove_contents(root);
    if (setrlimit(RLIMIT_NOFILE, &previous)) return 2;
    /* Restore resources and clean up even when checking the old defect. */
    int restored = remove_contents(root);
    close(root);
    if (rmdir(path)) return 2;
    printf("door cleanup: 80 levels, 32 descriptors, symlink: %s\n",
           result || restored ? "FAILED" : "passed");
    return result || restored ? 1 : 0;
}

static int blocked_stderr_cleanup(void) {
    char path[] = "/tmp/vc-door-stderr-XXXXXX";
    if (!mkdtemp(path)) return 2;
    int root = -1, session = -1, nullfd = -1;
    int life[2] = {-1, -1}, ready_pipe[2] = {-1, -1}, errors[2] = {-1, -1}, go[2] = {-1, -1};
    int result = 2, status = 0, flags = -1;
    pid_t child = -1, janitor = -1;
    const char *reason = "fixture error";
    root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (root < 0 || nullfd < 0 || pipe2(life, O_CLOEXEC) ||
        pipe2(ready_pipe, O_CLOEXEC) || pipe2(errors, O_CLOEXEC) || pipe2(go, O_CLOEXEC)) goto done;

    /* The warning pipe has its own open description: changing stdout's flags
     * cannot make stderr nonblocking. Keep its reader open but never drain it. */
    flags = fcntl(errors[1], F_GETFL);
    if (flags < 0 || fcntl(errors[1], F_SETFL, flags | O_NONBLOCK)) goto done;
    char padding[4096];
    memset(padding, 'x', sizeof padding);
    size_t filled = 0;
    for (;;) {
        ssize_t n = write(errors[1], padding, sizeof padding);
        if (n > 0) { filled += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN && filled) break;
        goto done;
    }
    if (fcntl(errors[1], F_SETFL, flags)) goto done;

    /* As in production, the janitor is the worker's child and outlives it.
     * This fixture is the BBS above them: a subreaper, so it can still reap
     * the janitor once the worker has gone. */
    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR || prctl(PR_SET_CHILD_SUBREAPER, 1)) goto done;
    child = fork();
    if (child < 0) goto done;
    if (!child) {
        pid_t worker = getpid(), own = fork();
        if (own < 0) _exit(2);
        if (!own) {
            close(life[1]); close(ready_pipe[0]); close(errors[0]); close(go[0]); close(go[1]);
            if (dup2(nullfd, STDIN_FILENO) < 0 || dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(errors[1], STDERR_FILENO) < 0) _exit(2);
            close(nullfd); close(errors[1]);
            struct termios tty = {0};
            sigset_t mask;
            sigemptyset(&mask);
            watchdog(root, path, "7", life[0], ready_pipe[1], UINT64_C(60000),
                     worker, &tty, fcntl(STDIN_FILENO, F_GETFL), &mask);
        }
        /* The worker ends abruptly, as a crash would: no orderly 'D'. */
        close(go[1]);
        char byte;
        while (read(go[0], &byte, 1) < 0 && errno == EINTR) {}
        _exit(0);
    }
    close(life[0]); life[0] = -1;
    close(life[1]); life[1] = -1;
    close(ready_pipe[1]); ready_pipe[1] = -1;
    close(go[0]); go[0] = -1;
    struct pollfd response = {.fd = ready_pipe[0], .events = POLLIN};
    if (poll(&response, 1, 750) != 1 || !(response.revents & POLLIN)) goto done;
    Ready ready;
    if (read(ready_pipe[0], &ready, sizeof ready) != sizeof ready || ready.error) goto done;
    session = openat(root, ready.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (session < 0) goto done;
    int file = openat(session, "must-go", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) goto done;
    int written = write_all(file, "x", 1);
    close(file);
    if (written || renameat(root, ready.name, root, "moved")) goto done;
    /* The held session descriptor still cleans its contents, but the original
     * name no longer exists: this drives the real watchdog's warning branch. */
    close(go[1]); go[1] = -1;
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    child = -1;
    uint64_t deadline = monotonic_ms() + 1000;
    for (;;) {
        pid_t ended = waitpid(-1, &status, WNOHANG);
        if (ended > 0) break;
        if (ended < 0 && errno != EINTR) goto done;
        if (monotonic_ms() >= deadline) {
            result = 1;
            reason = "FAILED: watchdog blocked on full stderr";
            goto done;
        }
        (void)poll(NULL, 0, 5);
    }
    struct stat st;
    int removed = fstatat(session, "must-go", &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
    result = !WIFEXITED(status) || WEXITSTATUS(status) != 1 || !removed ||
             fcntl(errors[1], F_GETFL) != flags;
    reason = result ? "FAILED: status, content cleanup, or restored flags" : "passed";

done:
    /* The red test must leave no stuck process or fixture. child is either our
     * unreaped fork PID or -1, never a reused or externally discovered PID. */
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
    /* An orphaned janitor is now this subreaper's child: never leave it. */
    while ((janitor = waitpid(-1, NULL, WNOHANG)) > 0) {}
    (void)prctl(PR_SET_CHILD_SUBREAPER, 0);
    if (session >= 0) close(session);
    if (nullfd >= 0) close(nullfd);
    for (unsigned i = 0; i < 2; ++i) {
        if (life[i] >= 0) close(life[i]);
        if (ready_pipe[i] >= 0) close(ready_pipe[i]);
        if (errors[i] >= 0) close(errors[i]);
        if (go[i] >= 0) close(go[i]);
    }
    if (root >= 0) {
        if (remove_contents(root)) result = 2;
        close(root);
    }
    if (rmdir(path)) result = 2;
    printf("door cleanup: full separate stderr, moved session: %s\n", reason);
    return result;
}

int main(void) {
    int deep = deep_tree_cleanup();
    int dos_deep = dos_rename_deep_cleanup();
    int stderr_result = blocked_stderr_cleanup();
    return deep ? deep : dos_deep ? dos_deep : stderr_result;
}
