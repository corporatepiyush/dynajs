/*
 * dyn-snappy -- Snappy compressed format (google format description,
 * 2011-10-05). PURE C: no JSValue, no JSContext.
 *
 *   encode: literal + copy(2-byte-offset) greedy hash-table encoder. Only
 *           the 2-byte-offset copy form is emitted (offsets <= 65535); the
 *           decoder accepts ALL FOUR element types so any conforming stream
 *           decodes. Encoder scope: a simple greedy matcher, not a ratio
 *           champion -- correct snappy, nothing more.
 *   decode: full format, every read bounds-checked, output capped at `cap`,
 *           declared length enforced. UNTRUSTED SURFACE.
 */
#ifndef DYN_SNAPPY_H
#define DYN_SNAPPY_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-compress.h"   /* dyn_outbuf_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Compress `src` into a fresh malloc'd buffer (free(*pout) by the caller).
 * Returns 0 on success; -1 on allocation failure only -- any input
 * compresses, because a literal-only stream is always legal. */
int dyn_snappy_compress(const uint8_t *src, size_t len,
                        uint8_t **pout, size_t *pout_len);

/* Decompress `src` into `o`. Returns 0, or -1 on malformed input, an output
 * larger than `cap`, a declared length that exceeds `cap`, or allocation
 * failure. The caller frees o->buf either way. */
int dyn_snappy_decompress(const uint8_t *src, size_t len, size_t cap,
                          dyn_outbuf_t *o);

#ifdef __cplusplus
}
#endif

#endif /* DYN_SNAPPY_H */