/* Native-only immutable copy of the shared browser/door demo drive.
 * Paths are relative UTF-8 host names; data retains its original DOS encoding. */
#ifndef VC_DOOR_DEMO_H
#define VC_DOOR_DEMO_H

#include "rt.h"

extern const EmbeddedFile door_demo_files[];
extern const int door_demo_file_count;
/* Door-only VC setup, not part of the shared demo drive's file mapping. */
extern const EmbeddedFile door_default_ini;

#endif
