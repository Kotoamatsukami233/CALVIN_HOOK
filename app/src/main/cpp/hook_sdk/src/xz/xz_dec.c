/* xz_dec.c - Simple XZ decompression wrapper using xz-embedded
 * Wraps the Linux kernel xz-embedded library into a single-call interface.
 * License: 0BSD (from xz-embedded by Lasse Collin / Igor Pavlov) */

#include "xz_dec.h"
#include "xz.h"
#include <stdlib.h>
#include <string.h>

size_t xz_decompress(const uint8_t *in, size_t in_size,
                     uint8_t *out, size_t out_cap) {
    xz_crc32_init();

    struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 1 << 26);
    if (!dec) return 0;

    uint8_t *out_buf = out;
    size_t out_size = out_cap;
    int query_mode = (out == NULL);
    size_t total = 0;

    if (query_mode) {
        out_size = 32768;
        out_buf = (uint8_t *)malloc(out_size);
        if (!out_buf) {
            xz_dec_end(dec);
            return 0;
        }
    }

    struct xz_buf buf;
    buf.in = in;
    buf.in_pos = 0;
    buf.in_size = in_size;
    buf.out = out_buf;
    buf.out_pos = 0;
    buf.out_size = out_size;

    enum xz_ret ret = XZ_OK;
    while (1) {
        ret = xz_dec_run(dec, &buf);

        if (ret == XZ_STREAM_END) {
            total += buf.out_pos;
            break;
        }

        if (ret == XZ_OK) {
            total += buf.out_pos;
            if (query_mode) {
                buf.out_pos = 0;
            } else {
                /* Output buffer full but not done - error */
                ret = XZ_BUF_ERROR;
                break;
            }
            continue;
        }

        if (ret == XZ_UNSUPPORTED_CHECK)
            continue;

        break;
    }

    xz_dec_end(dec);

    if (query_mode)
        free(out_buf);

    return (ret == XZ_STREAM_END) ? total : 0;
}
