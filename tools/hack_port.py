"""Guarded, build-directory-only adaptations of NetBSD's Hack 1.0.3.

The four Unix platform modules are replaced outright by runtime/hack_dos.
Everything below is an ABI, platform, or 16-bit integer adaptation; vendored
source bytes are never written and every replacement is counted.
"""
from pathlib import Path


REPLACED = {"hack.unix.c", "hack.tty.c", "hack.ioctl.c", "hack.terminfo.c"}

CHANGES = {
    "hack.h": [('#include "config.h"', '#include "hack_config.h"', 1)],
    "def.objects.h": [('#include "config.h"', '#include "hack_config.h"', 1)],
    "hack.main.c": [
        ("int main(int, char *[]);", "int hack_main(int, char *[]);", 1),
        ("main(int argc, char *argv[])", "hack_main(int argc, char *argv[])", 1),
        ('open("/dev/null", O_RDONLY)', 'open("NUL", O_RDONLY)', 1),
        ('= "save/";', '= "HACK.SAV";', 1),
        ('\t(void) snprintf(SAVEF, sizeof(SAVEF), "save/%d%s", getuid(), plname);\n'
         '\tregularize(SAVEF + 5);',
         '\t(void) strcpy(SAVEF, "HACK.SAV");\n\t/* Fixed DOS 8.3 save name. */', 1),
        ('(uptodate(fd) || unlink(SAVEF) == 666)', 'hack_resume_valid(fd)', 1),
        ('static void chdirx(const char *, boolean);',
         '#ifdef CHDIR\nstatic void chdirx(const char *, boolean);\n#endif', 1),
    ],
    "hack.end.c": [
        ("itoa(", "hack_itoa(", 3),
        ('"record_lock"', '"RECORD.LCK"', 1),
        # DOS is single-process. An absent/read-only record must not create
        # the Unix 300-second hardlink retry loop.
        ('while (link(recfile, reclock) == -1)', 'while (0)', 1),
    ],
    "hack.fight.c": [
        ('boolean         far =', 'boolean         distant =', 1),
        ('if (far != far_noise', 'if (distant != far_noise', 1),
        ('far_noise = far;', 'far_noise = distant;', 1),
        ('far ? " in the distance"', 'distant ? " in the distance"', 1),
    ],
    "hack.pri.c": [
        ('\t(void) putchar(ch);\n\tcurx++;',
         '\t(void) putchar(hack_mapchar(x, y - 2, ch));\n\tcurx++;', 1),
    ],
    "hack.lev.c": [
        ('%zu', '%u', 2), ('%zd', '%d', 1),
        ('\tbwrite(fd, &hackpid, sizeof(hackpid));',
         '\thack_write_header(fd);\n\tbwrite(fd, &hackpid, sizeof(hackpid));', 1),
        ('\t/* First some sanity checks */',
         '\t/* First some sanity checks */\n\thack_read_header(fd);', 1),
    ],
    "hack.bones.c": [
        ('\tsavelev(fd, dlevel);\n\t(void) close(fd);',
         '\tsavelev(fd, dlevel);\n\t(void) hack_finish_file(fd);\n\t(void) close(fd);', 1),
        # Debugging preserves usable bones, never an incompatible build's
        # pointer-bearing file. Keep normal upstream deletion otherwise.
        ('\tif (!wizard)\t\t/* duvel!frans:',
         '\tif (!wizard || !ok)\t/* duvel!frans:', 1),
    ],
    "hack.save.c": [
        ('\tlong            differ;', '/* Far pointers need a segment-aware rebase. */', 1),
        ('\tdiffer = (const char *) (&mons[0]) - (const char *) (monbegin);', '', 1),
        ('\t\tmtmp->data = (const struct permonst *)\n'
         '\t\t\t((const char *) mtmp->data + differ);',
         '\t\tmtmp->data = hack_monster_rebase(mtmp->data, monbegin);', 1),
        ('\tbwrite(fd, &u, sizeof(struct you));',
         '\tbwrite(fd, &u, sizeof(struct you));\n\thack_save_you(fd);', 1),
        ('\tmread(fd, &u, sizeof(struct you));',
         '\tmread(fd, &u, sizeof(struct you));\n\thack_restore_you(fd);', 1),
        ('\t(void) close(fd);\n\tglo(dlevel);',
         '\tif (!hack_finish_file(fd)) {\n'
         '\t\t(void) close(fd);\n\t\t(void) unlink(SAVEF);\n'
         '\t\terror("Cannot finish saved game.");\n\t}\n'
         '\t(void) close(fd);\n\tglo(dlevel);', 1),
    ],
}


def prepare(source: Path, target: Path) -> None:
    missing = set(CHANGES) - {path.name for path in source.iterdir()}
    if missing:
        raise ValueError(f"missing adapted Hack inputs: {', '.join(sorted(missing))}")
    target.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.glob("*")):
        if path.suffix not in (".c", ".h") or path.name in REPLACED:
            continue
        content = path.read_text()
        for before, after, count in CHANGES.get(path.name, ()):
            actual = content.count(before)
            if actual != count:
                raise ValueError(f"{path.name}: expected {count} instances of {before!r}, got {actual}")
            content = content.replace(before, after)
        if any(macro in content for macro in ("__DATE__", "__TIME__", "__TIMESTAMP__")):
            raise ValueError(f"{path.name}: unpinned compiler clock dependency")
        (target / path.name).write_text(content)
