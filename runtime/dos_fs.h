/* Additional filesystem entry points; the HLE interface lives in hle.h. */
#ifndef VC_DOS_FS_H
#define VC_DOS_FS_H

#include <stdint.h>

/* dos_fs_init() maps C: to / and, when HOME is an absolute directory other
 * than /, H: to realpath(HOME). Each drive has its own cwd; startup selects
 * H: only when the host cwd is at or below its root. DOS chdir does not
 * select a drive or change the host cwd. H: parents stop at its root;
 * symlinks retain normal host-filesystem behavior. */

/* Body of the runtime's far-callable F000:0100 country case-map stub.
 * Only AL changes, and only extended CP866 letters are uppercased. */
void dos_casemap_upper(void);

/* Keep one resolved short path attached to its selected host file/directory while a
 * child is running. Preparing a lease changes no DOS registers or aliases;
 * binding activates it only for that PSP. Both paths must be absolute, and
 * the DOS spelling must be an existing canonical 8.3 path below a drive root.
 * A directory lease also stabilizes paths to new files beneath it. A missing
 * original, changed directory or symlink entry, or a new native name
 * conflicting with its alias fails closed with DOS access denied. Replacing a
 * regular file at its selected pathname is allowed. An owner rename drops its
 * inode identity but reserves the original basename for safe recreation.
 * No directory-wide aliases are cached.
 * close_process releases bound leases; release also handles an unbound one
 * after a failed child launch. Preparation returns a DOS error code. */
typedef struct DosPathLease DosPathLease;
int dos_fs_pin_path(const char *absolute_short_dos, const char *absolute_host,
                    DosPathLease **out);
void dos_fs_bind_path(DosPathLease *lease, uint16_t psp);
void dos_fs_release_path(DosPathLease *lease);
/* Release only a child's path leases, including on TSR exits that retain
 * ordinary DOS file handles. */
void dos_fs_release_process_paths(uint16_t psp);

#endif
