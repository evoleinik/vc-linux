#ifndef VC_EMBED_LZMA_H
#define VC_EMBED_LZMA_H

#include <stddef.h>
#include <stdint.h>

/* Build-owned raw LZMA1 only: lc=lp=pb=0 and a <=1 MiB dictionary.
 * Return 0 only for a complete stream with exactly dst_size output bytes;
 * reject truncation, trailing data, invalid distances and output overflow.
 * Failure may leave partial output. The caller must not publish it.
 * Raw LZMA has no checksum: verify the build-time Adler-32 before use.
 */
int embed_lzma_decode(uint8_t *dst, size_t dst_size,
                      const uint8_t *src, size_t src_size);
uint32_t embed_adler32(const uint8_t *data, size_t size);

#endif
