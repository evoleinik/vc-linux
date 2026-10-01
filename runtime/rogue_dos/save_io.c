/* Keep a failed DOS save from terminating (and losing) the running game. */
#include <stdio.h>
#include <curses.h>
#include "rogue.h"

int rogue_finish_save(FILE *stream, int state_error)
{
    int failed = state_error || ferror(stream);
    if (fflush(stream) == EOF)
        failed = TRUE;
    if (fclose(stream) == EOF)
        failed = TRUE;
    if (!failed)
        return TRUE;

    /* Only this failed attempt's partial file is removed. The live game
     * remains authoritative, and rs_save_file resets its error on retry. */
    md_unlink(file_name);
    setup();
    playltchars();
    clearok(curscr, TRUE);
    wrefresh(curscr);
    msg("Save failed; the game is still running.");
    return FALSE;
}
