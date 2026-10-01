/* Additional filesystem entry points; the HLE interface lives in hle.h. */
#ifndef VC_DOS_FS_H
#define VC_DOS_FS_H

/* dos_fs_init() maps C: to / and, when HOME is an absolute directory other
 * than /, H: to realpath(HOME). Each drive has its own cwd; startup selects
 * H: only when the host cwd is at or below its root. DOS chdir does not
 * select a drive or change the host cwd. H: parents stop at its root;
 * symlinks retain normal host-filesystem behavior. */

/* Body of the runtime's far-callable F000:0100 country case-map stub.
 * Only AL changes, and only extended CP866 letters are uppercased. */
void dos_casemap_upper(void);

#endif
