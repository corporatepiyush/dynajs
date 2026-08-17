/*
 * dyn-compress -- gzip (RFC 1952) around DEFLATE (RFC 1951). PURE C: no
 * JSValue, no JSContext. Compiles standalone with `-Isrc/core`.
 *
 *   encode: RFC 1952 framing over a real fixed-Huffman DEFLATE stream (LZ77
 *           hash-chain match finding + RFC 1951 sec.3.2.6 fixed codes), falling
 *           back to STORED blocks when compressing would not shrink the input,
 *           so the output never expands. Always valid DEFLATE that any standard
 *           decoder accepts, plus the CRC-32 and ISIZE trailer.
 *   decode: a full inflate (stored / fixed-Huffman / dynamic-Huffman), so it
 *           reads the output of system gzip/zlib, with header parsing and
 *           CRC-32 + ISIZE validation.
 *
 * UNTRUSTED INPUT. Every bit and byte read on the decode path is bounds-checked
 * against the input length, and the output is capped at DYN_MAX_OUTPUT to
 * reject a decompression bomb before it can exhaust memory. Treat any buffer
 * handed to dyn_gunzip_decode as hostile; that is the design assumption.
 *
 * The CRC-32 comes from dyn-hash, so there is one implementation of the gzip
 * trailer's checksum rather than a private copy here.
 */
#ifndef DYN_COMPRESS_H
#define DYN_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap on decompressed size: 1 GiB, far beyond any realistic payload and
 * low enough that a bomb fails fast instead of swapping the machine. */
#define DYN_MAX_OUTPUT ((size_t)1 << 30)

/* Growable output buffer, libc-owned. Zero-initialise before use
 * (`dyn_outbuf_t o = { NULL, 0, 0 };`) and free(o.buf) when done -- including
 * on the error paths, where a partial buffer may already have been allocated. */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} dyn_outbuf_t;

/* Compress `src` into a fresh malloc'd buffer. On success returns 0 with *out
 * and *out_len set and ownership transferred to the caller (free(*out)); on
 * failure returns -1 and *out is untouched. Allocation failure is the only
 * failure mode -- any input compresses, because STORED is always available. */
int dyn_gzip_build(const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len);

/* ---- the reusable match-finder scratch ----------------------------------
 *
 * The hash head table and the position chain are the only per-call allocations
 * either encoder makes. A context owns them across calls, so a compressor
 * reused over a stream of payloads stops allocating after the first one. Pass
 * NULL to any *_ctx function for per-call scratch instead.
 *
 * BOTH TABLES ARE FIXED SIZE -- 256 KiB in total, for any input up to the
 * 2 GiB indexing limit. `prev` holds the GAP to the previous occurrence of a
 * hash rather than an absolute position, and no gap worth following can exceed
 * the 64 KiB window, so 16 bits is enough and the chain is a ring indexed by
 * position instead of an array indexed by it. (It was int32_t[input_len]: 4
 * bytes of heap per input byte, re-allocated on every call.)
 *
 * One context serves one encode at a time. It holds mutable scratch, so it is
 * not shareable across concurrent operations. */
typedef struct {
    int32_t *head;      /* stores position + base; below base means stale */
    uint16_t *prev;     /* gap to the previous occurrence; 0 ends the chain */
    size_t head_cap;
    size_t prev_cap;
    uint32_t base;      /* the current epoch; advances past every input */
} dyn_comp_ctx_t;

int dyn_comp_ctx_init(dyn_comp_ctx_t *c);
void dyn_comp_ctx_free(dyn_comp_ctx_t *c);

int dyn_gzip_build_ctx(const uint8_t *src, size_t src_len, dyn_comp_ctx_t *cx,
                       uint8_t **out, size_t *out_len);

/* Decompress `src` into `o`. Returns 0, or -1 on malformed input, a failed
 * CRC-32/ISIZE check, an output larger than DYN_MAX_OUTPUT, or allocation
 * failure. The caller frees o->buf either way. */
int dyn_gunzip_decode(const uint8_t *src, size_t len, dyn_outbuf_t *o);

/* ---- LZ4 -----------------------------------------------------------------
 *
 * The fast tier: roughly an order of magnitude quicker than DEFLATE at a worse
 * ratio. `level` selects the match-finder effort and NOTHING else -- every
 * level emits the same format and is read by the same decoder, which is what
 * makes "LZ4HC" a better parse rather than a second format. 1 is the classic
 * single-slot table; 2..12 walk a position chain.
 *
 * `dict`/`dict_len` are LZ4 prefix mode: the dictionary is treated as the bytes
 * immediately preceding the input, so matches may reach into it. Decompression
 * REQUIRES the identical dictionary -- the block itself does not record which
 * one, which is why the JS layer stamps an id and refuses a mismatch instead of
 * producing plausible garbage. Pass NULL/0 for no dictionary.
 */

/* Raw block. On success returns 0 and transfers ownership of *pout. */
int dyn_lz4_compress(const uint8_t *src, size_t len,
                     const uint8_t *dict, size_t dict_len, int level,
                     dyn_comp_ctx_t *cx, uint8_t **pout, size_t *pout_len);

/* Raw block, into `o`. Returns -1 on any malformed input; the caller frees
 * o->buf either way. UNTRUSTED SURFACE. */
int dyn_lz4_decompress(const uint8_t *src, size_t len,
                       const uint8_t *dict, size_t dict_len, dyn_outbuf_t *o);

/* LZ4 frame (magic 0x184D2204) -- what the `lz4` command line reads and
 * writes. `content_checksum` appends the XXH32 of the original bytes. */
int dyn_lz4_frame_build(const uint8_t *src, size_t len, int level,
                        int content_checksum, dyn_comp_ctx_t *cx,
                        uint8_t **pout, size_t *pout_len);

/* Decode a frame. Validates the descriptor checksum, every block length
 * against the remaining input, and the content checksum when present.
 * UNTRUSTED SURFACE. */
int dyn_lz4_frame_decode(const uint8_t *src, size_t len, dyn_outbuf_t *o);

#ifdef __cplusplus
}
#endif

#endif /* DYN_COMPRESS_H */
