/* A plain `rogue` resumes the single save in the DOS current directory. */
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
extern int rogue_main(int argc, char **argv, char **envp);
const char *rogue_recovery_notice;

int main(int argc, char **argv, char **envp)
{
    const char *resume[3];
    int status;
    _fmode = O_BINARY;
    if (argc == 1 && access("rogue.sav", 0) == 0)
    {
        resume[0] = argv[0];
        resume[1] = "rogue.sav";
        resume[2] = 0;
        /* A failed restore may have already replaced globals and the pack.
         * Keep this process pristine; DOS reclaims the failed child's state. */
        status = spawnve(P_WAIT, argv[0], resume, (const char *const *)envp);
        if (status == -1)
        {
            perror("Cannot start saved game");
            return 1;
        }
        if (status != 1)
            return status;
        if (rename("rogue.sav", "rogue.bad") == 0)
            rogue_recovery_notice = "Save renamed to rogue.bad; starting a new game.";
        else
            rogue_recovery_notice = "Cannot rename rogue.sav to rogue.bad; starting a new game.";
    }
    return rogue_main(argc, argv, envp);
}
