/* xz_config.h - Standalone build configuration for xz-embedded
 * Based on Linux kernel lib/xz/ (SPDX-License-Identifier: 0BSD)
 * Adapted for Android NDK standalone compilation. */

#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#include "xz.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Only need DYNALLOC mode for streaming decompression */
#define XZ_DEC_DYNALLOC

/* Accept any integrity check type (CRC32, CRC64, SHA256, None) */
#define XZ_DEC_ANY_CHECK

/* Memory allocation wrappers - replace kernel allocators */
#define kmalloc_obj(p) calloc(1, sizeof(p))
#define kfree(ptr) free(ptr)
#define vmalloc(size) malloc(size)
#define vfree(ptr) free(ptr)

/* Kernel utility macros */
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define fallthrough __attribute__((fallthrough))

/* Memory utility macros */
#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

/* Read a 32-bit little-endian unsigned integer */
static inline uint32_t get_le32(const uint8_t *p) {
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

#endif
