/* xz_dec.h - Simple XZ decompression API for .gnu_debugdata parsing
 * Wraps the xz-embedded library (0BSD license) into a single-call interface. */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decompress XZ-formatted data.
 * Returns decompressed size on success, 0 on failure.
 * If out is NULL, returns required buffer size (query mode). */
size_t xz_decompress(const uint8_t *in, size_t in_size,
                     uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
