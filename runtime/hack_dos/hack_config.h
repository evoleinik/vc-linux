/* DOS configuration for the unmodified Hack gameplay sources. */
#ifndef CONFIG
#define CONFIG
/* No UNIX, CHDIR, SECURE, SHELL, SUSPEND, or MAIL: startup owns the directory. */
#define WIZARD "bruno"
#define RECORD "record"
#define HELP "help"
#define SHELP "hh"
#define RUMORFILE "rumors"
#define DATAFILE "data"
#define FMASK 0660
#define HLOCK "perm"
#define LLOCK "SAFELOCK"
#define GOLD_ON_BOTL
#define EXP_ON_BOTL
#define COLNO 80
#define ROWNO 22
typedef short int schar;
typedef unsigned char uchar;
typedef schar xchar;
typedef xchar boolean;
#define TRUE 1
#define FALSE 0
#define Bitfield(x,n) unsigned x:n
#define SIZE(x) (int)(sizeof(x) / sizeof(x[0]))
#endif
