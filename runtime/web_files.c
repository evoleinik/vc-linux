/* Only DOS opens materialize these files. Directory enumeration/stat sees
 * complete metadata at startup, and translated code never waits itself. */
#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "embed_lzma.h"
#include "web_files.h"
#include "web_programs.h"

/* The marker belongs to the MEMFS inode, not its pathname: renaming a file
 * or its parent keeps it lazy, while deleting/recreating it cannot resurrect
 * old bytes. MEMFS operations clear the marker on writes/truncation, including
 * an application-side FS.writeFile while a download is suspended. */
EM_JS(int, install_placeholder, (const char *path, size_t size, WebFileReference *reference,
                                 int exclusive, int missing, int io_error), {
    try {
        const name = UTF8ToString(path);
        try { FS.lookupPath(name); return 0; } catch (error) {
            if (error.errno !== missing) return 5;
        }
        const slash = name.lastIndexOf('/');
        FS.mkdirTree(name.slice(0, slash));
        const stream = FS.open(name, exclusive, 0o666);
        const node = stream.node;
        try {
            // Reserve the real length without putting these zeros in wasm.
            // A failed DOS fetch never exposes them: its open fails first.
            node.contents = new Uint8Array(size);
            node.usedBytes = size;
            node.vcLazyFile = reference;
            const streams = node.stream_ops;
            node.stream_ops = {
                ...streams,
                read(stream, buffer, offset, length, position) {
                    if (stream.node.vcLazyFile && length) throw new FS.ErrnoError(io_error);
                    return streams.read(stream, buffer, offset, length, position);
                },
                write(stream, buffer, offset, length, position, canOwn) {
                    const result = streams.write(stream, buffer, offset, length, position, canOwn);
                    if (result) delete stream.node.vcLazyFile;
                    return result;
                },
                allocate(stream, offset, length) {
                    const result = streams.allocate(stream, offset, length);
                    if (length) delete stream.node.vcLazyFile;
                    return result;
                },
            };
            const nodes = node.node_ops;
            node.node_ops = {
                ...nodes,
                setattr(node, attributes) {
                    const result = nodes.setattr(node, attributes);
                    if (attributes.size !== undefined) delete node.vcLazyFile;
                    return result;
                },
            };
        } finally {
            FS.close(stream);
        }
        return 0;
    } catch (error) {
        return error instanceof RangeError ? 8 : 5;
    }
});

EM_JS(WebFileReference *, pending_reference, (const char *path, unsigned *inode), {
    try {
        const node = FS.lookupPath(UTF8ToString(path), { follow: true }).node;
        HEAPU32[inode >>> 2] = node.id;
        return node.vcLazyFile || 0;
    } catch {
        return 0;
    }
});

/* Allocate before publishing, then swap only the inode's contents. This is
 * one synchronous operation, preserves open leases/metadata, and cannot leave
 * a truncated placeholder on allocation failure. The private reference is
 * copied: editing the guest's file must not change EXEC's trusted bytes. */
EM_JS(int, fill_placeholder, (const char *path, unsigned inode,
                             WebFileReference *reference, const uint8_t *data, size_t size), {
    try {
        const node = FS.lookupPath(UTF8ToString(path), { follow: true }).node;
        if (node.id !== inode || node.vcLazyFile !== reference) return 5;
        const contents = new Uint8Array(size);
        contents.set(HEAPU8.subarray(data, data + size));
        node.contents = contents;
        node.usedBytes = size;
        delete node.vcLazyFile;
        return 0;
    } catch (error) {
        return error instanceof RangeError ? 8 : 5;
    }
});

static WebFileReference *find_reference(const EmbeddedFile *file)
{
    for (size_t index = 0; index < web_file_reference_count; index++) {
        WebFileReference *reference = web_file_references + index;
        if (!strcmp(reference->name, file->name) && reference->size == file->size)
            return reference;
    }
    return NULL;
}

int web_files_install(const char *path, const EmbeddedFile *file)
{
    WebFileReference *reference = find_reference(file);
    int error = reference ? install_placeholder(path, reference->size, reference,
                                               O_WRONLY | O_CREAT | O_EXCL, ENOENT, EIO) : 5;
    if (!error) return 0;
    errno = error == 8 ? ENOMEM : EACCES;
    return -1;
}

static int load_reference(WebFileReference *reference)
{
    if (!reference->sha256) return 5;
    if (reference->data) return 0;
    /* Identical files/aliases share one successful download, but never the
     * mutable contents of either installed DOS file. */
    for (size_t index = 0; index < web_file_reference_count; index++) {
        WebFileReference *other = web_file_references + index;
        if (other->data && other->sha256 && other->size == reference->size &&
            !strcmp(other->sha256, reference->sha256) &&
            other->checksum == reference->checksum && !strcmp(other->asset, reference->asset)) {
            reference->data = other->data;
            return 0;
        }
    }
    uint8_t *bytes = malloc(reference->size ? reference->size : 1);
    if (!bytes) return 8;
    static unsigned attempt;
    char stage[80];
    snprintf(stage, sizeof stage, "/var/vc/file-%u.bin", ++attempt);
    int error = 5;
    if (web_fetch_asset(reference->asset, stage, reference->sha256)) {
        FILE *stream = fopen(stage, "rb");
        if (stream) {
            size_t got = fread(bytes, 1, reference->size, stream);
            int extra = fgetc(stream);
            if (got == reference->size && extra == EOF && !ferror(stream) &&
                embed_adler32(bytes, reference->size) == reference->checksum) error = 0;
            if (fclose(stream)) error = 5;
        }
    }
    unlink(stage);
    if (error) free(bytes);
    else reference->data = bytes;
    return error;
}

int web_files_reference(const EmbeddedFile *file, const uint8_t **out)
{
    *out = NULL;
    if (file->data) { *out = file->data; return 0; }
    WebFileReference *reference = find_reference(file);
    if (!reference) return 5;
    int error = load_reference(reference);
    if (!error) *out = reference->data;
    return error;
}

int web_files_open(const char *path, int flags, mode_t mode)
{
    unsigned inode;
    WebFileReference *reference = pending_reference(path, &inode);
    /* Truncation intentionally discards the old contents. O_EXCL must fail
     * without downloading, and ordinary OS access checks precede a fetch. */
    int fd = open(path, flags, mode);
    if (fd < 0 || !reference || (flags & O_TRUNC)) return fd;
    int error = load_reference(reference);
    if (!error) error = fill_placeholder(path, inode, reference, reference->data, reference->size);
    if (!error) return fd;
    close(fd);
    errno = error == 8 ? ENOMEM : EACCES;
    return -1;
}
