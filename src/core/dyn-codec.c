/*
 * dyn-codec -- reversible binary<->text codecs. PURE C. See dyn-codec.h.
 *
 * base32, Ascii85 and the LEB128 var-ints are moved here unchanged from
 * dyna-encoding.c, where they were verified against RFC 4648 and against
 * Python's base64.a85encode/a85decode(adobe=False) by tests/test_encoding.js.
 * Hex and base64 are thin wrappers over the shared multi-ISA kernel table, so
 * dyna:encoding, dyna:text, dyna:http and String.prototype can stop each
 * carrying their own sizing and error policy around the same kernels.
 */
#include "dyn-codec.h"

#include <string.h>

#include "dyna-simd-kernels.h"


/* ══════════════════════════════ hex (SIMD kernel) ══════════════════════ */

void dyn_codec_hex_encode(const uint8_t *data, size_t n, char *out)
{
    simd.hex_encode(data, n, out);
}

size_t dyn_codec_hex_decode(const char *s, size_t n, uint8_t *out)
{
    if (n & 1)
        return DYN_CODEC_BAD;   /* an odd length can never be whole bytes */
    return simd.hex_decode(s, n, out); /* DYN_CODEC_BAD on any non-hex digit */
}

/* ══════════════════════════════ base64 (SIMD kernel) ═══════════════════ */

size_t dyn_codec_base64_encode(const uint8_t *data, size_t n, char *out)
{
    return simd.base64_encode(data, n, out);
}

size_t dyn_codec_base64_decode(const char *s, size_t n, uint8_t *out)
{
    return simd.base64_decode(s, n, out);
}

/* base64url: derived from the standard kernel rather than a second kernel --
 * encode then translate and strip padding; decode translates back and re-pads
 * to a multiple of 4, which is what the kernel requires. A stray '+' or '/' is
 * rejected: those bytes are not in the url-safe alphabet, so the reverse
 * table has no entry for them. */
size_t dyn_codec_base64url_encode(const uint8_t *data, size_t n, char *out)
{
    size_t written = simd.base64_encode(data, n, out);
    size_t i, o;

    for (i = 0, o = 0; i < written; i++) {
        char c = out[i];
        if (c == '=')
            break; /* padding is always a contiguous tail run: done */
        out[o++] = (c == '+') ? '-' : (c == '/') ? '_' : c;
    }
    return o;
}

size_t dyn_codec_base64url_decode(const char *s, size_t n, uint8_t *out,
                                  char *scratch)
{
    size_t rem = n % 4, padlen, i;

    if (rem == 1)
        return DYN_CODEC_BAD;   /* no byte count encodes to 4k+1 characters */
    padlen = rem ? 4 - rem : 0;

    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        else if (c == '+' || c == '/')
            return DYN_CODEC_BAD;
        scratch[i] = c;
    }
    for (i = 0; i < padlen; i++)
        scratch[n + i] = '=';

    return simd.base64_decode(scratch, n + padlen, out);
}

/* ══════════════════════════════ base32 (RFC 4648) ══════════════════════ */

static const char dyn_b32_std_alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char dyn_b32_hex_alpha[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

static const char *dyn_codec_b32_alpha(int magic)
{
    return magic == DYN_BASE32_HEX ? dyn_b32_hex_alpha : dyn_b32_std_alpha;
}

/* Encode `len` bytes (5-byte -> 8-char blocks, '='-padded per RFC 4648
 * sec.6) using `alpha` (a 32-byte alphabet table). `out` needs
 * ((len+4)/5)*8 bytes. */
static size_t b32_encode_tab(const uint8_t *restrict data, size_t len,
                             const char *restrict alpha, char *restrict out)
{
    /* nbytes-remaining (0..5) -> count of non-padding chars in the block. */
    static const uint8_t nchars_tab[6] = {0, 2, 4, 5, 7, 8};
    size_t i = 0, o = 0;

    /* Whole blocks, written out. The general path below stages the input
       through a chunk[5] and emits with a VARIABLE shift whose trip count comes
       from a table, so the compiler can neither drop the staging copy nor
       unroll the emit. Every full block takes constant shifts instead; the tail
       keeps the general form, which is the one that has to handle padding. */
    for (; i + 5 <= len; i += 5) {
        uint64_t v = ((uint64_t)data[i] << 32) | ((uint64_t)data[i + 1] << 24) |
                     ((uint64_t)data[i + 2] << 16) | ((uint64_t)data[i + 3] << 8) |
                     (uint64_t)data[i + 4];
        out[o + 0] = alpha[(v >> 35) & 0x1F];
        out[o + 1] = alpha[(v >> 30) & 0x1F];
        out[o + 2] = alpha[(v >> 25) & 0x1F];
        out[o + 3] = alpha[(v >> 20) & 0x1F];
        out[o + 4] = alpha[(v >> 15) & 0x1F];
        out[o + 5] = alpha[(v >> 10) & 0x1F];
        out[o + 6] = alpha[(v >>  5) & 0x1F];
        out[o + 7] = alpha[ v        & 0x1F];
        o += 8;
    }
    for (; i < len; i += 5) {
        uint8_t chunk[5] = {0, 0, 0, 0, 0};
        size_t nb = (len - i < 5) ? len - i : 5;
        size_t j, nc;
        uint64_t v;

        for (j = 0; j < nb; j++)
            chunk[j] = data[i + j];
        v = ((uint64_t)chunk[0] << 32) | ((uint64_t)chunk[1] << 24) |
            ((uint64_t)chunk[2] << 16) | ((uint64_t)chunk[3] << 8) | chunk[4];
        nc = nchars_tab[nb];
        for (j = 0; j < nc; j++)
            out[o++] = alpha[(v >> (35 - 5 * j)) & 0x1F];
        for (; j < 8; j++)
            out[o++] = '=';
    }
    return o;
}

/* Decode an 8-char-block '='-padded base32 string. Returns the byte count,
 * or DYN_CODEC_BAD on any malformed input: length not a multiple of 8, a
 * character outside `alpha`, padding anywhere but the final block, or a
 * padding count that isn't one of RFC 4648's five valid block shapes. `out`
 * needs (slen/8)*5 bytes. */
/* Reverse tables, COMPILE-TIME. These were a 256-byte memset plus 32 scattered
 * stores on EVERY call, before a single character was decoded. base32 decodes
 * short strings -- TOTP secrets, DNS labels, ETags -- where 16 to 32 characters
 * of payload cost less than the setup did. There are exactly two alphabets and
 * both are compile-time constants, so nothing here was ever data-dependent. */
static const int8_t dyn_b32_std_rev[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,26,27,28,29,30,31,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};
static const int8_t dyn_b32_hex_rev[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
    25,26,27,28,29,30,31,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static size_t b32_decode_tab(const char *restrict str, size_t slen,
                             const char *restrict alpha, uint8_t *restrict out)
{
    /* count of non-padding chars in a block (0..8) -> decoded byte count;
     * -1 marks a char-count RFC 4648 never produces (1,3,6, or an all-pad
     * block), which cannot be honest base32. */
    static const int8_t nbytes_tab[9] = { -1, -1, 1, -1, 2, 3, -1, 4, 5 };
    /* The alphabet pointer identifies which of the two it is. */
    const int8_t *rev = (alpha == dyn_b32_hex_alpha) ? dyn_b32_hex_rev
                                                     : dyn_b32_std_rev;
    size_t i, o = 0;

    if (slen % 8 != 0)
        return DYN_CODEC_BAD;

    /* Only the FINAL block may carry padding, so every earlier block decodes
       with no padding scan, no table lookup for the byte count and no staging
       array -- four short loops per block collapse to straight line code. The
       validity test is branchless: a rejected character is -1, whose sign bit
       survives the OR, so one test covers all eight. */
    for (i = 0; i + 8 < slen; i += 8) {
        int a0 = rev[(uint8_t)str[i + 0]], a1 = rev[(uint8_t)str[i + 1]];
        int a2 = rev[(uint8_t)str[i + 2]], a3 = rev[(uint8_t)str[i + 3]];
        int a4 = rev[(uint8_t)str[i + 4]], a5 = rev[(uint8_t)str[i + 5]];
        int a6 = rev[(uint8_t)str[i + 6]], a7 = rev[(uint8_t)str[i + 7]];
        uint64_t v;
        if ((a0 | a1 | a2 | a3 | a4 | a5 | a6 | a7) < 0)
            return DYN_CODEC_BAD;   /* '=' included: padding cannot occur here */
        v = ((uint64_t)a0 << 35) | ((uint64_t)a1 << 30) | ((uint64_t)a2 << 25) |
            ((uint64_t)a3 << 20) | ((uint64_t)a4 << 15) | ((uint64_t)a5 << 10) |
            ((uint64_t)a6 <<  5) | (uint64_t)a7;
        out[o + 0] = (uint8_t)(v >> 32);
        out[o + 1] = (uint8_t)(v >> 24);
        out[o + 2] = (uint8_t)(v >> 16);
        out[o + 3] = (uint8_t)(v >>  8);
        out[o + 4] = (uint8_t)v;
        o += 5;
    }

    for (; i < slen; i += 8) {
        uint8_t c[8], bytes[5];
        int pad, nreal, nbytes, last_group, j;
        uint64_t v = 0;

        last_group = (i + 8 == slen);
        pad = 0;
        for (j = 7; j >= 0; j--) {
            if (str[i + j] != '=')
                break;
            pad++;
        }
        if (pad > 0 && !last_group)
            return DYN_CODEC_BAD; /* padding only valid in the string's last block */
        nreal = 8 - pad;
        nbytes = nbytes_tab[nreal];
        if (nbytes < 0)
            return DYN_CODEC_BAD;

        for (j = 0; j < nreal; j++) {
            int8_t val = rev[(uint8_t)str[i + j]];
            if (val < 0)
                return DYN_CODEC_BAD;
            c[j] = (uint8_t)val;
        }
        for (; j < 8; j++)
            c[j] = 0; /* padding slots contribute zero bits */

        for (j = 0; j < 8; j++)
            v = (v << 5) | c[j];
        bytes[0] = (uint8_t)(v >> 32);
        bytes[1] = (uint8_t)(v >> 24);
        bytes[2] = (uint8_t)(v >> 16);
        bytes[3] = (uint8_t)(v >> 8);
        bytes[4] = (uint8_t)v;
        for (j = 0; j < nbytes; j++)
            out[o++] = bytes[j];
    }
    return o;
}

/* ══════════════════ Ascii85 (Adobe-less, btoa-style) ═══════════════════ */

static int dyn_codec_is_ascii85_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Encode `len` bytes into `out` (needs ((len+3)/4)*5 bytes); returns bytes
 * written. Empty input -> 0 bytes. */
size_t dyn_codec_base85_encode(const uint8_t *restrict data, size_t len,
                                 char *restrict out)
{
    size_t i, o = 0;

    for (i = 0; i < len; i += 4) {
        size_t nb = (len - i < 4) ? len - i : 4;
        uint32_t v = 0;
        char c[5];
        size_t m, j;

        for (j = 0; j < nb; j++)
            v |= (uint32_t)data[i + j] << (24 - 8 * j);

        if (nb == 4 && v == 0) {
            out[o++] = 'z';
            continue;
        }
        for (j = 0; j < 5; j++) {
            c[4 - j] = (char)('!' + (v % 85));
            v /= 85;
        }
        m = (nb < 4) ? nb + 1 : 5; /* a partial final group emits nb+1 chars */
        for (j = 0; j < m; j++)
            out[o++] = c[j];
    }
    return o;
}

/* Decode an ascii85 string into `out` (needs slen*4 bytes -- a safe upper
 * bound: no valid input decodes more than 4 bytes per character, and a
 * lone 'z' hits exactly that ratio). Returns bytes written, or DYN_CODEC_BAD on
 * a corrupt input: a byte outside whitespace/'!'-'u'/'z', a 'z' off a group
 * boundary, or a trailing group of exactly 1 leftover character. */
size_t dyn_codec_base85_decode(const char *restrict str, size_t slen,
                                 uint8_t *restrict out)
{
    uint32_t v = 0;
    unsigned nb = 0;
    size_t i, o = 0;

    for (i = 0; i < slen; i++) {
        unsigned char b = (unsigned char)str[i];
        if (dyn_codec_is_ascii85_space(b))
            continue;
        if (b == 'z' && nb == 0) {
            out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
            continue;
        }
        if (b < '!' || b > 'u')
            return DYN_CODEC_BAD;
        v = v * 85 + (uint32_t)(b - '!');
        nb++;
        if (nb == 5) {
            out[o++] = (uint8_t)(v >> 24);
            out[o++] = (uint8_t)(v >> 16);
            out[o++] = (uint8_t)(v >> 8);
            out[o++] = (uint8_t)v;
            v = 0;
            nb = 0;
        }
    }
    if (nb == 1)
        return DYN_CODEC_BAD; /* a lone leftover digit cannot decode to a byte */
    if (nb > 1) {
        unsigned k;
        for (k = nb; k < 5; k++)
            v = v * 85 + 84; /* missing trailing digits treated as maximal ('u') */
        for (k = 0; k < nb - 1; k++)
            out[o++] = (uint8_t)(v >> (24 - 8 * k));
    }
    return o;
}

/* ══════════════════ var-ints: LEB128 + zigzag ══════════════════════════ */

/* PutUvarint: 1-10 bytes, little-endian base-128 groups, continuation bit
 * (0x80) set on every byte but the last. `out` needs >= 10 bytes. */
size_t dyn_codec_put_uvarint(uint64_t x, uint8_t *out)
{
    size_t i = 0;
    while (x >= 0x80) {
        out[i++] = (uint8_t)(x | 0x80);
        x >>= 7;
    }
    out[i++] = (uint8_t)x;
    return i;
}

/* Uvarint: the inverse of dyn_codec_put_uvarint. Returns bytes
 * consumed (>0) on success; 0 if `buf` ran out before a terminating byte
 * (truncated); or -(i+1) if the value would need more than 64 bits
 * (overflow at byte i). */
int dyn_codec_uvarint(const uint8_t *buf, size_t n, uint64_t *out)
{
    uint64_t x = 0;
    unsigned s = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (i == DYN_CODEC_VARINT_MAX)
            return -(int)(i + 1);
        if (b < 0x80) {
            if (i == DYN_CODEC_VARINT_MAX - 1 && b > 1)
                return -(int)(i + 1); /* final byte can only hold 1 more bit */
            *out = x | ((uint64_t)b << s);
            return (int)(i + 1);
        }
        x |= (uint64_t)(b & 0x7f) << s;
        s += 7;
    }
    *out = 0;
    return 0;
}

/* PutVarint/Varint: zigzag-encode the int64 (0,-1,1,-2,2,... -> 0,1,2,3,4,...)
 * then LEB128 it. */
size_t dyn_codec_put_varint(int64_t x, uint8_t *out)
{
    uint64_t ux = (uint64_t)x << 1;
    if (x < 0)
        ux = ~ux;
    return dyn_codec_put_uvarint(ux, out);
}

int dyn_codec_varint(const uint8_t *buf, size_t n, int64_t *out)
{
    uint64_t ux;
    int nb = dyn_codec_uvarint(buf, n, &ux);
    int64_t x;

    if (nb <= 0) {
        *out = 0;
        return nb;
    }
    x = (int64_t)(ux >> 1);
    if (ux & 1)
        x = ~x;
    *out = x;
    return nb;
}

/* Coerce a JS value to the uint64 magnitude putUvarint encodes: a BigInt (the
 * full 64-bit range; wraps mod 2^64, matching writeBigUint64LE) or a Number
 * that must be an exact non-negative safe integer (<= 2^53-1). A fractional
 * or out-of-range Number throws RangeError rather than silently truncating/
 * wrapping (unlike JS_ToIndex or the fixed-width dyna:bytes accessors):
 * putUvarint encodes an exact value, not an array index or a fixed-width
 * field, so silently accepting e.g. 1.5 would hide a caller bug. Use a
 * BigInt for anything beyond 2^53-1. */

/* Public base32 entry points: resolve the alphabet selector, then run the
 * table-driven codec above. Split this way because the table form is what the
 * codec actually needs and the enum is what a caller wants to say. */
size_t dyn_codec_base32_encode(const uint8_t *restrict data, size_t n,
                               char *restrict out, dyn_base32_alphabet alpha)
{
    return b32_encode_tab(data, n, dyn_codec_b32_alpha((int)alpha), out);
}

size_t dyn_codec_base32_decode(const char *restrict s, size_t n,
                               uint8_t *restrict out, dyn_base32_alphabet alpha)
{
    return b32_decode_tab(s, n, dyn_codec_b32_alpha((int)alpha), out);
}
