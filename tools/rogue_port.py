"""Checked, build-directory-only adaptations of Rogue's 32-bit Unix C.

The vendored files are never written.  Each replacement must occur exactly the
recorded number of times so an upstream change cannot silently lose a shim.
"""
from pathlib import Path


CHANGES = {
    "rogue.h": [
        ("int s_exp;", "long s_exp;", 1),
        ("inv_type, lastscore, level", "inv_type, level", 1),
        ("noscore, ntraps, purse,", "noscore, ntraps,", 1),
        ("extern int\tdnum, e_levels[], seed;",
         "extern int dnum;\nextern long e_levels[], seed, purse, lastscore;", 1),
        ("score(int amount, int flags, char monst)",
         "score(long amount, int flags, char monst)", 1),
    ],
    "score.h": [
        ("int sc_score;", "long sc_score;", 1),
    ],
    "extern.c": [
        ("int seed;", "long seed;", 1),
        ("int e_levels[]", "long e_levels[]", 1),
        ("int purse = 0;", "long purse = 0;", 1),
        ("int lastscore = -1;", "long lastscore = -1;", 1),
    ],
    "main.c": [
        ('#include "rogue.h"',
         '#include "rogue.h"\nextern const char *rogue_recovery_notice;', 1),
        ("main(int argc, char **argv, char **envp)",
         "rogue_main(int argc, char **argv, char **envp)", 1),
        ('"rogue.save"', '"rogue.sav"', 1),
        ("abs((int) RN) % range", "(unsigned long) RN % (unsigned) range", 1),
        ("You quit with %d gold pieces", "You quit with %ld gold pieces", 1),
        ("    playit();",
         "    if (rogue_recovery_notice != NULL)\n"
         '        msg("%s", rogue_recovery_notice);\n    playit();', 1),
    ],
    "io.c": [
        ("static int s_exp = 0;", "static long s_exp = 0;", 1),
        ("static int s_pur = -1;", "static long s_pur = -1;", 1),
        ("Exp: %d/%d", "Exp: %d/%ld", 1),
        ("Gold: %-5d", "Gold: %-5ld", 2),
    ],
    "fight.c": [
        ("register int lastpurse;", "register long lastpurse;", 1),
    ],
    "save.c": [
        # Watcom's stdlib.h declares this with the correct near/far attribute.
        ("    extern char **environ;", "    /* environ comes from stdlib.h. */", 1),
        ("static STAT sbuf;", "static STAT sbuf;\nextern int rogue_finish_save(FILE *, int);", 1),
        ("    rs_save_file(savef);\n    fflush(savef);\n    fclose(savef);",
         "    if (!rogue_finish_save(savef, rs_save_file(savef)))\n        return;", 1),
        ("    rs_restore_file(inf);",
         '    if (rs_restore_file(inf) != 0)\n    {\n'
         '        endwin();\n        fclose(inf);\n'
         '        printf("Saved game is damaged; the file was not removed.\\n");\n'
         '        return FALSE;\n    }', 1),
        (" %u %d %u %hu %d %x ", " %u %ld %u %hu %d %x ", 2),
    ],
    "rip.c": [
        # Q,y returns directly; deaths/wins retain their acknowledgement.
        ("        wgetnstr(stdscr,prbuf,80);",
         "        if (flags != 1) wgetnstr(stdscr,prbuf,80);", 1),
        ("score(int amount, int flags, char monst)",
         "score(long amount, int flags, char monst)", 1),
        ("int worth = 0;", "long worth = 0;", 1),
        ("int oldpurse;", "long oldpurse;", 1),
        ("worth = 2 * obj->o_count", "worth = 2L * obj->o_count", 1),
        ("worth *= 3 *", "worth *= 3L *", 1),
        ("(9 - obj->o_arm) * 100", "(9L - obj->o_arm) * 100", 1),
        ("(10 * (a_class", "(10L * (a_class", 1),
        ("worth += obj->o_arm * 100", "worth += obj->o_arm * 100L", 1),
        ("worth += 20 * obj->o_charges", "worth += 20L * obj->o_charges", 1),
        ("%2d %5d %s: %s on level %d", "%2d %5ld %s: %s on level %d", 1),
        ("%s with %d gold", "%s with %ld gold", 1),
        ("%d Au", "%ld Au", 1),
        ("%c) %5d  %s", "%c) %5ld  %s", 1),
        ("%5d  Gold Pieces", "%5ld  Gold Pieces", 1),
    ],
    "state.c": [
        ("static int endian =", "static long endian =", 1),
        ("rs_write_int(FILE *savef, int c)\n{\n    unsigned char bytes[4];\n"
         "    unsigned char *buf = (unsigned char *) &c;",
         "rs_write_int(FILE *savef, int c)\n{\n    unsigned char bytes[4];\n"
         "    long converted = c;\n"
         "    unsigned char *buf = (unsigned char *) &converted;", 1),
        ("    int input = 0;", "    long input = 0;", 1),
        ("rs_write_uint(FILE *savef, unsigned int c)\n{\n    unsigned char bytes[4];\n"
         "    unsigned char *buf = (unsigned char *) &c;",
         "rs_write_uint(FILE *savef, unsigned int c)\n{\n    unsigned char bytes[4];\n"
         "    unsigned long converted = c;\n"
         "    unsigned char *buf = (unsigned char *) &converted;", 1),
        ("    int  input;", "    unsigned long input = 0;", 1),
        ("int\nrs_write_marker(FILE *savef, int id)",
         '#include "state_long.h"\n\nint\nrs_write_marker(FILE *savef, long id)', 1),
        ("    rs_write_int(savef, id);", "    rs_write_long(savef, id);", 1),
        ("rs_read_marker(FILE *inf, int id)\n{\n    int nid;",
         "rs_read_marker(FILE *inf, long id)\n{\n    long nid;", 1),
        ("    if (rs_read_int(inf, &nid) == 0)",
         "    if (rs_read_long(inf, &nid) == 0)", 1),
        ("rs_write_int(savef, mvwinch(win,row,col))",
         "rs_write_long(savef, (long) mvwinch(win,row,col))", 1),
        ("int row,col,maxlines,maxcols,value,width,height;",
         "int row,col,maxlines,maxcols,width,height;\n    long value;", 1),
        ("            if (rs_read_int(inf, &value) != 0)",
         "            if (rs_read_long(inf, &value) != 0)", 1),
        ("rs_write_int(savef, s->s_exp)", "rs_write_long(savef, s->s_exp)", 1),
        ("rs_read_int(inf,&s->s_exp)", "rs_read_long(inf,&s->s_exp)", 1),
        ("rs_write_int(savef, seed)", "rs_write_long(savef, seed)", 1),
        ("rs_read_int(inf, &seed)", "rs_read_long(inf, &seed)", 1),
        ("rs_write_int(savef, purse)", "rs_write_long(savef, purse)", 1),
        ("rs_read_int(inf, &purse)", "rs_read_long(inf, &purse)", 1),
        ("rs_write_int(savef, lastscore)", "rs_write_long(savef, lastscore)", 1),
        ("rs_read_int(inf, &lastscore)", "rs_read_long(inf, &lastscore)", 1),
        ("rs_write_ints(savef, e_levels, 21)",
         "rs_write_longs(savef, e_levels, 21)", 1),
        ("rs_read_ints(inf,e_levels,21)", "rs_read_longs(inf,e_levels,21)", 1),
        ("rs_save_file(FILE *savef)\n{\n    if (write_error)\n        return(WRITESTAT);",
         "rs_save_file(FILE *savef)\n{\n    write_error = FALSE; /* a previous failed save can be retried */", 1),
    ],
}


def prepare(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.iterdir()):
        if path.suffix not in (".c", ".h"):
            continue
        content = path.read_text()
        for old, new, expected in CHANGES.get(path.name, []):
            actual = content.count(old)
            if actual != expected:
                raise ValueError(f"{path.name}: expected {expected} instances of {old!r}; got {actual}")
            content = content.replace(old, new)
        output = destination / path.name
        if not output.exists() or output.read_text() != content:
            output.write_text(content)
