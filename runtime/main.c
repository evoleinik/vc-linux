/* vc: Volkov Commander, translated. Sets up the machine, the config directory
 * and the terminal, starts VC.COM as the first DOS process and runs it. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "rt.h"

static void usage(void) {
    fputs("usage: vc [DIRECTORY]\n"
          "Volkov Commander 4.99.09, translated from 8086 assembly to native code.\n"
          "Settings live in $XDG_CONFIG_HOME/vc-linux (default ~/.config/vc-linux).\n"
          "A log is written to $VC_LOG (default ~/.cache/vc-linux/vc.log).\n", stdout);
}

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

/* Setup files are written once and then belong to the user. The program
 * images are rewritten every run, since they are this binary. */
static void install_files(const char *dir) {
    for (int i = 0; i < embedded_file_count; i++) {
        const EmbeddedFile *f = &embedded_files[i];
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, f->name);
        int program = !strcmp(f->name, "VC.COM") || !strcmp(f->name, "VC.OVL");
        if (!program && access(path, F_OK) == 0) continue;
        FILE *out = fopen(path, "wb");
        if (!out || fwrite(f->data, 1, f->size, out) != f->size) {
            fprintf(stderr, "vc: cannot write %s: %s\n", path, strerror(errno));
            exit(1);
        }
        fclose(out);
    }
}

int main(int argc, char **argv) {
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
    bios_init();
    dos_fs_init();
    term_init();
    atexit(term_shutdown);
    rt_log("start: %s", host_prog);
    /* Base-memory mode: no swap file, no EMS or XMS to find. The TSR manager
     * guards against DOS programs that stay resident, which cannot happen here. */
    static const char tail[] = " /std /notsr";
    dos_start(host_prog, (const uint8_t *)tail, (int)sizeof tail - 1);
    rt_run();
    term_shutdown();
    return rt_exit_code;
}
