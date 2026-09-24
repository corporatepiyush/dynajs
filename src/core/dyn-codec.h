/*
 * dyn-codec -- reversible binary<->text codecs. PURE C: no JSValue, no
 * JSContext. Compiles with -Isrc/core plus the allowlisted SIMD kernel table
 * (see tools/core-purity.sh).
 *
 * Hex, base64, base32, Ascii85 and LEB128 var-ints. Everything here is
 * *reversible and keyless* -- that is the module boundary. Digests and
 * checksums live in dyn-hash; anything key-dependent lives in dyna:crypto.
 *
 * ALLOCATION-FREE. Every function writes into a caller-supplied buffer sized by
 * the matching *_len (exact) or *_cap (upper bound) helper, and returns the
 * number of bytes actually written. Nothing here calls malloc, so a caller can
 * work on the stack, in an arena, or straight into a JS string buffer, and the
 * error paths have nothing to unwind.
 *
 * ERROR CONVENTION: a decoder returns DYN_CODEC_BAD on malformed input. It is
 * SIZE_MAX, so it can never collide with a real length.
 *
 * Hex and base64 dispatch to the shared multi-ISA kernel table (`simd`), which
 * the engine initialises once at startup via simd_init(). Calling a codec
 * before simd_init() is a caller error, exactly as it was when this code lived
 * in the binding.
 */
#ifndef DYN_CODEC_H
#define DYN_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by every decoder on malformed input. */
#define DYN_CODEC_BAD ((size_t)-1)

/* Base32 alphabet selector: RFC 4648 standard, or the extended-hex variant. */
typedef enum {
    DYN_BASE32_STD = 0,
    DYN_BASE32_HEX = 1
} dyn_base32_alphabet;

/* ---- output sizing ------------------------------------------------------
 * *_len is exact; *_cap is an upper bound (the decoder returns the true size). */

static inline size_t dyn_codec_hex_encode_len(size_t n)      { return n * 2; }
static inline size_t dyn_codec_hex_decode_cap(size_t n)      { return n / 2; }
static inline size_t dyn_codec_base64_encode_cap(size_t n)   { return 4 * ((n + 2) / 3); }
static inline size_t dyn_codec_base64_decode_cap(size_t n)   { return 3 * (n / 4); }
static inline size_t dyn_codec_base32_encode_cap(size_t n)   { return ((n + 4) / 5) * 8; }
static inline size_t dyn_codec_base32_decode_cap(size_t n)   { return ((n + 7) / 8) * 5; }
static inline size_t dyn_codec_base85_encode_cap(size_t n)   { return ((n + 3) / 4) * 5; }
static inline size_t dyn_codec_base85_decode_cap(size_t n)   { return ((n + 4) / 5) * 4; }
/* A var-int is at most 10 bytes at 64 bits. */
#define DYN_CODEC_VARINT_MAX 10

/* ---- hex (SIMD) --------------------------------------------------------- */

/* Lowercase hex. `out` needs dyn_codec_hex_encode_len(n) bytes. */
void dyn_codec_hex_encode(const uint8_t *data, size_t n, char *out);

/* Returns bytes written, or DYN_CODEC_BAD on an odd length or a non-hex digit.
 * `out` needs dyn_codec_hex_decode_cap(n) bytes. */
size_t dyn_codec_hex_decode(const char *s, size_t n, uint8_t *out);

/* ---- base64 (SIMD) ------------------------------------------------------ */

/* Standard alphabet, '=' padded. Returns bytes written. */
size_t dyn_codec_base64_encode(const uint8_t *data, size_t n, char *out);

/* Standard alphabet. Returns bytes written, or DYN_CODEC_BAD. */
size_t dyn_codec_base64_decode(const char *s, size_t n, uint8_t *out);

/* URL-safe (RFC 4648 sec.5): '-'/'_', unpadded. `out` needs the standard
 * base64 cap; the result is never longer. Returns bytes written. */
size_t dyn_codec_base64url_encode(const uint8_t *data, size_t n, char *out);

/* URL-safe decode. Needs a `scratch` buffer of n + 3 bytes for the translated,
 * re-padded string (the kernel requires a length that is a multiple of 4).
 * Keeping the scratch caller-supplied is what keeps this allocation-free.
 * Returns bytes written, or DYN_CODEC_BAD for a length ≡ 1 (mod 4), a stray
 * '+'/'/' (not in the url-safe alphabet), or
 * any other malformed input. */
size_t dyn_codec_base64url_decode(const char *s, size_t n, uint8_t *out,
                                  char *scratch);

/* ---- base32 (RFC 4648) -------------------------------------------------- */

size_t dyn_codec_base32_encode(const uint8_t *restrict data, size_t n,
                               char *restrict out, dyn_base32_alphabet alpha);
size_t dyn_codec_base32_decode(const char *restrict s, size_t n,
                               uint8_t *restrict out, dyn_base32_alphabet alpha);

/* ---- Ascii85 ------------------------------------------------------------ */

/* Adobe-less ascii85: no <~ ~> wrapper. Decode skips whitespace. */
size_t dyn_codec_base85_encode(const uint8_t *data, size_t n, char *out);
size_t dyn_codec_base85_decode(const char *s, size_t n, uint8_t *out);

/* ---- LEB128 var-ints ---------------------------------------------------- */

/* `out` needs DYN_CODEC_VARINT_MAX bytes. Returns bytes written (1..10). */
size_t dyn_codec_put_uvarint(uint64_t x, uint8_t *out);
size_t dyn_codec_put_varint(int64_t x, uint8_t *out);   /* zig-zag */

/* Read a var-int from buf[0..n). Returns the number of bytes consumed, or 0 on
 * a truncated buffer and a NEGATIVE count on overflow past 64 bits -- three
 * distinct ranges, so a caller can tell the two failures apart. */
int dyn_codec_uvarint(const uint8_t *buf, size_t n, uint64_t *out);
int dyn_codec_varint(const uint8_t *buf, size_t n, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DYN_CODEC_H */
