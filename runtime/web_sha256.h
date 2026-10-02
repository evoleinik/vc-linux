#ifndef VC_WEB_SHA256_H
#define VC_WEB_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 for immutable lazy H: files, including plain-HTTP hosting where
 * WebCrypto is unavailable. No allocation, platform crypto or guest state. */
void web_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);

#endif
