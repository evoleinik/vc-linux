/* Additional filesystem entry points; the HLE interface lives in hle.h. */
#ifndef VC_DOS_FS_H
#define VC_DOS_FS_H

#include <stdint.h>
#include <stddef.h>

enum { DOS_MAX_HANDLES = 64 };

/* Copy inheritable system-file references into a new PSP's job file table. */
void dos_fs_inherit_process(uint16_t child, uint16_t parent);
/* One-shot provenance for a DOS-hosted loader's close -> create-PSP sequence.
 * The returned path is re-opened and byte-checked by the execution layer. */
int dos_fs_take_closed_file(uint16_t owner, char *path, size_t capacity);

#define DOS_DOOR_ENTRY_LIMIT 4096u

/* dos_fs_init() maps C: to / and, when HOME is an absolute directory other
 * than /, H: to realpath(HOME). Each drive has its own cwd; startup selects
 * H: only when the host cwd is at or below its root. DOS chdir does not
 * select a drive or change the host cwd. H: parents stop at its root;
 * symlinks retain normal host-filesystem behavior. */

/* Reset the filesystem for a door session. Only H: exists, rooted at the
 * supplied private directory; DOS cannot traverse symlinks or open special
 * files. quota bounds the total logical bytes of files, including sparse
 * extents and files still held open after deletion. At most 4096 entries may
 * be allocated, including seeded directories/files and unlinked inodes held
 * by DOS handles or path leases. Returns a DOS error code, or zero. A failure
 * leaves no drive accessible. dos_fs_init() restores the
 * ordinary mappings. Populate root before calling this function. */
int dos_fs_init_door(const char *root, uint64_t quota);

/* Open a resolved absolute host spelling for executable-byte matching.
 * In door mode the actual open is anchored beneath H:, with no symlinks.
 * Returns a read-only CLOEXEC descriptor, or -1 with errno set. */
int dos_fs_open_readonly(const char *absolute_host);

/* Door-only cleanup of an exact runtime-owned temporary file or empty
 * directory, using an absolute DOS path. Ignore the guest read-only bit but
 * retain confinement and quota accounting. Close any orphaned DOS handles to
 * that owned file. Roots and symlinks are refused; no DOS registers or memory
 * change. Returns a DOS error code. */
int dos_fs_remove_temporary(const char *absolute_dos);

/* Door-only creation of an exact runtime-owned private directory (mode 0700)
 * from an absolute DOS path, charged as one entry like any DOS mkdir. Returns
 * a DOS error code: 80 if the name exists, 39 if the session is full. */
int dos_fs_make_temporary(const char *absolute_dos);

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
