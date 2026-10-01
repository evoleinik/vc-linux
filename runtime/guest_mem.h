/* Native copies between host buffers and guest memory.
 *
 * A guest program, and in door mode an anonymous caller, controls every byte
 * of `mem`: MCB chains, PSPs, segment registers and lengths. A native copy
 * must never trust them. guest_span is the one bounds check: it fails when
 * start plus length passes MEM_SIZE. Every helper below goes through it and
 * fails the whole copy, touching nothing, rather than copying a prefix.
 * Copies use memmove, so a guest source and destination may overlap.
 * Single-byte rd8/wr8 accesses stay inside `mem` by construction (cpu.h). */
#ifndef VC_GUEST_MEM_H
#define VC_GUEST_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"

/* The guest bytes [address, address + length), or NULL past MEM_SIZE. */
static inline uint8_t *guest_span(uint32_t address, size_t length) {
    if (address > MEM_SIZE || length > MEM_SIZE - address) return NULL;
    return mem + address;
}

/* Host to guest. Returns 0, or -1 with nothing written. */
static inline int guest_write(uint32_t address, const void *source, size_t length) {
    uint8_t *target = guest_span(address, length);
    if (!target) return -1;
    if (length) memmove(target, source, length);
    return 0;
}

/* Guest to host. Returns 0, or -1 with nothing read. */
static inline int guest_read(void *target, uint32_t address, size_t length) {
    const uint8_t *source = guest_span(address, length);
    if (!source) return -1;
    if (length) memmove(target, source, length);
    return 0;
}

/* Guest to guest; the ranges may overlap. Returns 0, or -1 with nothing moved. */
static inline int guest_move(uint32_t target, uint32_t source, size_t length) {
    uint8_t *to = guest_span(target, length);
    const uint8_t *from = guest_span(source, length);
    if (!to || !from) return -1;
    if (length) memmove(to, from, length);
    return 0;
}

/* Returns 0, or -1 with nothing filled. */
static inline int guest_fill(uint32_t address, uint8_t value, size_t length) {
    uint8_t *target = guest_span(address, length);
    if (!target) return -1;
    if (length) memset(target, value, length);
    return 0;
}

#endif
