/* Load unchanged generated translations on DOS EXEC, not at page startup.
 * SIDE_MODULE code shares the main module's memory and indirect-call table.
 * Its Image.run never waits: only this direct DOS-handler path may Asyncify. */
#include <dlfcn.h>
#include <emscripten.h>
#include <emscripten/emmalloc.h>
#include <emscripten/heap.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"
#include "web_programs.h"
#include "web_program_names.h"

typedef struct {
    const char *filename, *module, *symbol;
    const Image *image;
} WebImage;

static WebImage images[WEB_IMAGE_COUNT] = {
    [WEB_VC_COM] = {"VC.COM", NULL, NULL, &image_vc_com},
    [WEB_VC_OVL] = {"VC.OVL", NULL, NULL, &image_vc_ovl},
    [WEB_GWBASIC] = {"GWBASIC.EXE", WEB_MODULE_GWBASIC, "image_gwbasic", NULL},
    [WEB_BOOTLOGO] = {"BOOTLOGO.COM", WEB_MODULE_BOOTLOGO, "image_bootlogo", NULL},
    [WEB_ROGUE] = {"ROGUE.EXE", WEB_MODULE_ROGUE, "image_rogue", NULL},
    [WEB_VZ] = {"VZ.COM", WEB_MODULE_VZ, "image_vz", NULL},
    [WEB_KERMIT] = {"KERMIT.EXE", WEB_MODULE_KERMIT, "image_kermit", NULL},
    [WEB_COMMAND] = {"COMMAND.COM", WEB_MODULE_COMMAND, "image_command", NULL},
    [WEB_EDLIN] = {"EDLIN.COM", WEB_MODULE_EDLIN, "image_edlin", NULL},
    [WEB_DEBUG] = {"DEBUG.COM", WEB_MODULE_DEBUG, "image_debug", NULL},
    [WEB_FIND] = {"FIND.EXE", WEB_MODULE_FIND, "image_find", NULL},
    [WEB_MORE] = {"MORE.COM", WEB_MODULE_MORE, "image_more", NULL},
    [WEB_SORT] = {"SORT.EXE", WEB_MODULE_SORT, "image_sort", NULL},
    [WEB_FC] = {"FC.EXE", WEB_MODULE_FC, "image_fc", NULL},
    [WEB_VC405] = {"VC405.COM", WEB_MODULE_VC405, "image_vc405", NULL},
    [WEB_VCSETUP405] = {"VC405/VCSETUP.COM", WEB_MODULE_VCSETUP405, "image_vcsetup405", NULL},
};

EM_ASYNC_JS(int, fetch_program, (const char *name, const char *path, const char *sha256), {
    const controller = new AbortController();
    // Tests may shorten the deadline; no configuration may remove the cap.
    const configured = Module['vcProgramFetchTimeoutMs'];
    const timeoutMs = Number.isFinite(configured) && configured > 0
        ? Math.min(configured, 30000) : 30000;
    let timer;
    try {
        // The name itself pins the asset bytes; locateFile also stamps the URL.
        // Tests replace just the transport with asynchronous local reads.
        const url = locateFile(UTF8ToString(name));
        const expectedHash = sha256 ? UTF8ToString(sha256) : null;
        const deadline = new Promise((_, reject) => {
            timer = setTimeout(() => {
                controller.abort();
                reject(new Error('asset download timed out'));
            }, timeoutMs);
        });
        const download = async () => {
            const options = { signal: controller.signal };
            let bytes;
            if (Module['vcFetchProgram']) bytes = await Module['vcFetchProgram'](url, options);
            else {
                const response = await fetch(url, options);
                if (!response.ok) throw new Error('HTTP ' + response.status);
                bytes = new Uint8Array(await response.arrayBuffer());
            }
            if (expectedHash) {
                // Snapshot before the asynchronous hash: a transport-owned
                // buffer must not change between verification and staging.
                bytes = new Uint8Array(bytes);
                const digest = new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes));
                const actualHash = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join("");
                if (actualHash !== expectedHash) throw new Error('file content hash mismatch');
            }
            return bytes;
        };
        // Race the body AND content verification, not just response headers.
        // A transport or digest that ignores abort cannot stage late bytes.
        const bytes = await Promise.race([download(), deadline]);
        FS.writeFile(UTF8ToString(path), bytes);
        return 1;
    } catch {
        // No loading UI, console warning, or guest screen output. DOS will
        // receive an ordinary file/EXEC error and keep its process alive.
        try { FS.unlink(UTF8ToString(path)); } catch {}
        return 0;
    } finally {
        clearTimeout(timer);
    }
});

int web_fetch_asset(const char *name, const char *path, const char *sha256) {
    return fetch_program(name, path, sha256);
}

const char *web_image_filename(size_t index) {
    return images[index].filename;
}

int web_image_is_vz(const Image *image) {
    return image && image == images[WEB_VZ].image;
}

static int enough_module_memory(const char *path) {
    struct stat info;
    if (stat(path, &info) || info.st_size <= 0) return 5;
    // Emscripten retains one raw file copy, plus dylink static data. web_modules.mjs
    // gates statics+alignment <= file size. 2x covers both, and 64 KiB covers
    // allocator/DSO bookkeeping and Asyncify's small direct wait stack.
    const size_t overhead = 64 * 1024;
    if ((uint64_t)info.st_size > (SIZE_MAX - overhead) / 2) return 8;
    size_t required = (size_t)info.st_size * 2 + overhead;
    size_t available = emscripten_get_heap_size() - (uintptr_t)sbrk(0)
        + emmalloc_free_dynamic_memory();
    if (available < required) return 8;
    // Also require one contiguous region: counting fragmented free blocks
    // alone cannot protect dylink's unchecked calloc. No guest code runs
    // between this reservation and dlopen on the single-threaded web build.
    // Use the explicit allocator API: Clang can eliminate an unused
    // malloc/free pair, including its allocation-failure check.
    void *reservation = emmalloc_malloc(required);
    if (!reservation) return 8;
    emmalloc_free(reservation);
    return 0;
}

int web_load_image(size_t index, const Image **out) {
    WebImage *entry = &images[index];
    if (entry->image) {
        *out = entry->image;
        return 0;
    }
    // A rejected dlopen leaves its path in Emscripten's internal registry.
    // A fresh staging path lets a later EXEC retry even a damaged response.
    static unsigned attempt;
    char path[80];
    snprintf(path, sizeof path, "/var/vc/%u-%s", ++attempt, entry->module);
    if (!fetch_program(entry->module, path, NULL)) return 5; /* DOS access denied */
    int error = enough_module_memory(path);
    if (error) {
        unlink(path);
        return error; /* DOS 8: refuse before dylink can overwrite address 0 */
    }
    // Emscripten's dlopen also awaits compilation under Asyncify. The
    // translation is never on this C stack, so IGNORE_INDIRECT stays safe.
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    // Drop MEMFS's JS-side copy. Emscripten still retains a raw heap copy
    // for the session, even after a failed dlopen; the guard budgets for it.
    unlink(path);
    if (!handle) {
        rt_log("cannot link browser translation %s", entry->module);
        return 5; /* the verified DOS file is valid; its translation is unavailable */
    }
    const Image *image = dlsym(handle, entry->symbol);
    RtImageRunner supplement = NULL;
    if (index == WEB_GWBASIC) supplement = (RtImageRunner)dlsym(handle, "run_gwbasic_graphics");
    if (!image || (index == WEB_GWBASIC && !supplement)) {
        rt_log("browser translation %s lacks its image/runner", entry->module);
        dlclose(handle);
        return 5;
    }
    if (supplement) rt_register_supplement(image, supplement);
    // Deliberately keep the dlopen reference for the session. Image pointers
    // and their runners may remain registered after a DOS child terminates.
    entry->image = image;
    *out = image;
    return 0;
}
