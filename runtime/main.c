/* vc: Volkov Commander, translated. Sets up the machine, the config directory
 * and the terminal, starts VC.COM as the first DOS process and runs it. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "rt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "term.h"

EM_JS(void, browser_exit, (int status), {
    if (Module['vcExit']) Module['vcExit'](status);
});
#else
static void usage(void) {
    fputs("usage: vc [DIRECTORY]\n"
          "Volkov Commander 4.99.09, translated from 8086 assembly to native code.\n"
          "Settings live in $XDG_CONFIG_HOME/vc-linux (default ~/.config/vc-linux).\n"
          "A log is written to $VC_LOG (default ~/.cache/vc-linux/vc.log).\n", stdout);
}
#endif

static int mkdirs(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int r = mkdir(path, 0755);
        *p = '/';
        if (r && errno != EEXIST) return -1;
    }
    return mkdir(path, 0755) && errno != EEXIST ? -1 : 0;
}

/* Defaults an earlier build shipped that are unsafe: they paste file names
 * into shell commands. A file still holding exactly one of them was never
 * edited by the user, so it is replaced. */
static const struct { const char *name, *text; } retired[] = {
    {"VCEDIT.EXT", "*: ${EDITOR:-vi} \"!.!\"\r\n"},
    {"VC.EXT", "zip:\tpkunzip -d !.!\r\narj:\tarj x -v -y !.!\r\nlzh:\tlha x !.!\r\n"
               "asm:\ttasm /w0/m9 !;\r\n\ttlink /t !;\r\n"},
    {"VC.EXT", ""},
};

static int holds_retired_default(const char *path, const char *name) {
    for (size_t i = 0; i < sizeof retired / sizeof retired[0]; i++) {
        if (strcmp(retired[i].name, name)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) return 0;
        char buf[512];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (n == strlen(retired[i].text) && !memcmp(buf, retired[i].text, n)) return 1;
    }
    return 0;
}

static int file_matches(const char *path, const EmbeddedFile *file) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != file->size) {
        close(fd);
        return 0;
    }
    FILE *in = fdopen(fd, "rb");
    if (!in) { close(fd); return 0; }
    uint8_t buf[4096];
    int matches = 1;
    for (size_t at = 0; at < file->size;) {
        size_t n = file->size - at;
        if (n > sizeof buf) n = sizeof buf;
        if (fread(buf, 1, n, in) != n || memcmp(buf, file->data + at, n)) {
            matches = 0;
            break;
        }
        at += n;
    }
    if (matches && (fgetc(in) != EOF || ferror(in))) matches = 0;
    if (fclose(in)) matches = 0;
    return matches;
}

/* A concurrent vc, or one whose write fails, must never expose a truncated
 * installed image. Publish a fully closed temporary from the same directory. */
static int install_file(const char *path, const EmbeddedFile *file) {
    struct stat prior;
    mode_t mode;
    if (!stat(path, &prior)) mode = prior.st_mode & 0777;
    else {
        /* Startup is single-threaded; mirror fopen's new-file permissions. */
        mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }
    char temporary[4096 + 16];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof temporary) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    int err = 0;
    FILE *out = fdopen(fd, "wb");
    if (!out) {
        err = errno;
        close(fd);
    } else {
        errno = 0;
        if (fchmod(fd, mode)) err = errno;
        if (!err && fwrite(file->data, 1, file->size, out) != file->size) err = errno ? errno : EIO;
        if (fclose(out) && !err) err = errno ? errno : EIO;
    }
    if (!err && rename(temporary, path)) err = errno;
    if (err) {
        unlink(temporary);
        errno = err;
        return -1;
    }
    return 0;
}

/* Setup files are written once and then belong to the user. Program images
 * are updated to this binary's bytes, leaving matching installations alone. */
static void install_files(const char *dir) {
    for (int i = 0; i < embedded_file_count; i++) {
        const EmbeddedFile *f = &embedded_files[i];
        char path[4096];
#ifdef __EMSCRIPTEN__
        /* H: is the program drive in the browser. Configuration remains out
         * of sight under /var/vc/config, as before. */
        const char *target = !strcmp(f->name, "GWBASIC.EXE") || !strcmp(f->name, "BOOTLOGO.COM")
            ? "/home/vc" : dir;
#else
        const char *target = dir;
#endif
        int length = snprintf(path, sizeof path, "%s/%s", target, f->name);
        if (length < 0 || (size_t)length >= sizeof path) {
            fputs("vc: configuration file path is too long\n", stderr);
            exit(1);
        }
        int program = !strcmp(f->name, "VC.COM") || !strcmp(f->name, "VC.OVL") ||
                      !strcmp(f->name, "GWBASIC.EXE") || !strcmp(f->name, "BOOTLOGO.COM");
        if (!program && access(path, F_OK) == 0 && !holds_retired_default(path, f->name)) continue;
        if (!file_matches(path, f) && install_file(path, f)) {
            fprintf(stderr, "vc: cannot write %s: %s\n", path, strerror(errno));
            exit(1);
        }
        if (!strcmp(f->name, "BOOTLOGO.COM")) {
            /* Retire the old DOS PATH alias only after its replacement is
             * installed. Changed files and user-created symlinks are not ours. */
            length = snprintf(path, sizeof path, "%s/LOGO.COM", target);
            if (length < 0 || (size_t)length >= sizeof path) {
                fputs("vc: legacy program path is too long\n", stderr);
                exit(1);
            }
            struct stat legacy;
            if (!lstat(path, &legacy) && S_ISREG(legacy.st_mode) && file_matches(path, f) &&
                unlink(path) && errno != ENOENT) {
                fprintf(stderr, "vc: cannot retire %s: %s\n", path, strerror(errno));
                exit(1);
            }
        }
    }
}

int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
    (void)argc;
    (void)argv;
    /* Settings and the log live outside H:, so the demo drive shows only
     * the demo files. */
    char cache[] = "/var/vc/cache";
    if (setenv("HOME", "/home/vc", 1) ||
        setenv("XDG_CONFIG_HOME", "/var/vc/config", 1) ||
        setenv("XDG_CACHE_HOME", cache, 1) ||
        mkdirs(cache) || chdir("/home/vc")) {
        fprintf(stderr, "vc: cannot prepare browser home: %s\n", strerror(errno));
        return 1;
    }
#else
    if (argc > 2 || (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        usage();
        return argc > 2 ? 2 : 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("vc: needs a terminal on stdin and stdout\n", stderr);
        return 2;
    }
    /* Shell commands are our children and we wait for them. An inherited
     * SIG_IGN would make the kernel reap them first. */
    signal(SIGCHLD, SIG_DFL);
    if (argc == 2 && chdir(argv[1])) {
        fprintf(stderr, "vc: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
#endif

    char dir[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/vc-linux", xdg);
    else snprintf(dir, sizeof dir, "%s/.config/vc-linux", home ? home : "/tmp");
    if (mkdirs(dir)) {
        fprintf(stderr, "vc: cannot create %s: %s\n", dir, strerror(errno));
        return 1;
    }
    install_files(dir);

    char host_prog[4200];
    snprintf(host_prog, sizeof host_prog, "%s/VC.COM", dir);

    dos_core_init();
    rt_register_supplement(&image_gwbasic, run_gwbasic_graphics);
    bios_init();
    dos_fs_init();
    term_init();
    atexit(term_shutdown);
#ifdef __EMSCRIPTEN__
    /* The browser supplies kitty key reports itself. Feed the query reply
     * after term_init has reset the parser so modifier events stay enabled. */
    static const uint8_t kitty_reply[] = "\033[?0u";
    term_feed_input(kitty_reply, sizeof kitty_reply - 1, 0);
#endif
    rt_log("start: %s", host_prog);
    /* Base-memory mode: no swap file, no EMS or XMS to find. The TSR manager
     * guards against DOS programs that stay resident, which cannot happen here. */
    static const char tail[] = " /std /notsr";
    dos_start(host_prog, (const uint8_t *)tail, (int)sizeof tail - 1);
    rt_run();
    term_shutdown();
#ifdef __EMSCRIPTEN__
    browser_exit(rt_exit_code);
#endif
    return rt_exit_code;
}
