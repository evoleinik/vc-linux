/* Browser-only immutable references and metadata-first DOS files. */
#ifndef VC_WEB_FILES_H
#define VC_WEB_FILES_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "rt.h"

typedef struct {
    const char *name, *asset, *sha256;
    size_t size;
    uint32_t checksum;
    const uint8_t *data;
} WebFileReference;

/* Generated alongside embedded_files. Never point at mutable MEMFS bytes. */
extern WebFileReference web_file_references[];
extern const size_t web_file_reference_count;

/* Startup: create a full-size placeholder without fetching. Existing files
 * remain owned by the user. Returns the usual 0/-1 with errno. */
int web_files_install(const char *path, const EmbeddedFile *file);

/* DOS open/EXEC only: may suspend the narrow direct Asyncify chain. The
 * reference remains private and immutable after edits, renames or deletion.
 * Returns a DOS error code (0, 5 or 8). */
int web_files_reference(const EmbeddedFile *file, const uint8_t **out);
int web_files_open(const char *path, int flags, mode_t mode);

#endif
