/* Configuration for the original Rogue sources, compiled as 16-bit DOS C. */
#ifndef VC_ROGUE_DOS_CONFIG_H
#define VC_ROGUE_DOS_CONFIG_H
#define HAVE_ERASECHAR 1
#define HAVE_KILLCHAR 1
#define SCOREFILE "rogue.scr"
/* DOS runs one program at a time; score-file locks only delay failed writes. */
/* There is no Unix process/signal model in the DOS game. */
#define signal(number, handler) ((void (*)(int))0)
#endif
