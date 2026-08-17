/*
 * dyn-compress -- gzip/DEFLATE + LZ4. PURE C. See dyn-compress.h.
 *
 * Moved unchanged from dyna-compress.c, where the encoder was checked against
 * system gunzip and the decoder against system gzip's output by
 * tests/test_compress.js. The decode path is an untrusted-input surface: every
 * read is bounds-checked and the output is capped, and that property is the
 * reason this belongs in one place rather than per-caller.
 *
 * ---- PERFORMANCE NOTES -----------------------------------------------------
 *
 * The shape of this file is dictated by two facts about compression code: the
 * inner loops run once per BYTE, and the buffers are proportional to the input.
 * So the rules here are (a) nothing per-byte may be a call, a branch on a
 * capacity check, or a loop over bits, and (b) nothing that scales with the
 * input may be allocated when a fixed-size structure will do.
 *
 *   BIT I/O is 64-bit. The reader refills 8 bytes with one unaligned load and
 *   a branchless pointer bump; the writer drains 8 bytes with one store. Both
 *   used to touch memory one byte at a time, through a function call, with a
 *   capacity check per byte.
 *
 *   HUFFMAN is table-driven. Decoding walks a 9-bit root table (one load for
 *   the overwhelming majority of symbols) and only falls back to the canonical
 *   bit-at-a-time walk for codes longer than the root -- and that fallback is
 *   the original, still validating, still the thing that makes an invalid code
 *   return -1. Encoding reads pre-reversed codes out of .rodata rather than
 *   running a bit-reversal loop per symbol. The fixed tables (RFC 1951 3.2.6)
 *   are const, so a fixed block builds nothing at all.
 *
 *   THE MATCH FINDER is windowed. `prev` used to be int32_t[input_len] -- 4
 *   bytes of heap per input byte, which for a 12 MB payload is a 51 MB
 *   allocation to compress a 1.6 MB result. Because no match may exceed the
 *   32 KiB (DEFLATE) / 64 KiB (LZ4) window, a chain entry only ever needs to
 *   express a GAP to the previous occurrence, and a gap fits in 16 bits. So
 *   `prev` is a fixed 128 KiB ring indexed by position, holding gaps, and the
 *   scratch is 256 KiB flat no matter how large the input is.
 *
 *   THE OUTPUT is written once. gzip framing used to build the DEFLATE body in
 *   one buffer and then memcpy it into a second buffer sized header+body+
 *   trailer. Now the header is written first and the body is deflated directly
 *   after it, so the payload is never copied and peak heap is one buffer.
 *
 *   INCOMPRESSIBLE INPUT EXITS EARLY. The encoder is handed the size of the
 *   STORED representation as a hard ceiling. Passing it means STORED would
 *   have been smaller, which is precisely the condition for falling back --
 *   so the fallback is detected the moment it becomes true instead of after
 *   compressing the whole input and comparing at the end.
 *
 * None of this changes what the decoder ACCEPTS or what the encoder EMITS in
 * format terms. The encoder's parse differs from the previous revision (better
 * hash, deeper filtering), so output bytes are not identical to the old ones;
 * they are valid DEFLATE, they are read by system gunzip, and they are the same
 * size or smaller. Every bounds check on the decode path is preserved.
 */
#include "dyn-compress.h"

#include <stdlib.h>
#include <string.h>

#include "dyn-hash.h"   /* dyn_crc32: the gzip trailer checksum */

/* Max LEN of a single DEFLATE stored block. An encoder detail -- callers
 * never see block structure -- so it stays out of the header. */
#define DYN_STORED_MAX  65535u

/* ---------- compiler hints ------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define DYN_LIKELY(x)    __builtin_expect(!!(x), 1)
#define DYN_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define DYN_INLINE       static inline __attribute__((always_inline))
#define DYN_NOINLINE     __attribute__((noinline))
#else
#define DYN_LIKELY(x)    (x)
#define DYN_UNLIKELY(x)  (x)
#define DYN_INLINE       static inline
#define DYN_NOINLINE
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define DYN_BIG_ENDIAN 1
#else
#define DYN_BIG_ENDIAN 0
#endif

/* Unaligned little-endian 64-bit load/store. memcpy is the portable spelling of
 * an unaligned access -- a cast to uint64_t* is UB on a misaligned address and
 * traps on some targets -- and every compiler that matters emits one
 * instruction for it. */
DYN_INLINE uint64_t dyn_ld64le(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if DYN_BIG_ENDIAN
    v = __builtin_bswap64(v);
#endif
    return v;
}

DYN_INLINE void dyn_st64le(void *p, uint64_t v)
{
#if DYN_BIG_ENDIAN
    v = __builtin_bswap64(v);
#endif
    memcpy(p, &v, 8);
}

/* ---------- growable output buffer (libc-owned) ---------------------------
 *
 * The capacity test is inline and the growth is not: on the hot paths this
 * compiles to one compare against a field already in a register, and the
 * realloc machinery stays out of the loop body's instruction footprint. */

DYN_NOINLINE static int dyn_ob_grow(dyn_outbuf_t *o, size_t extra)
{
    size_t need, ncap;
    uint8_t *nb;

    if (extra > SIZE_MAX - o->len)
        return -1;
    need = o->len + extra;
    if (need > DYN_MAX_OUTPUT)
        return -1;
    ncap = o->cap ? o->cap : 1024;
    while (ncap < need) {
        if (ncap > DYN_MAX_OUTPUT / 2) { ncap = need; break; }
        ncap <<= 1;
    }
    nb = (uint8_t *)realloc(o->buf, ncap);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

DYN_INLINE int dyn_ob_ensure(dyn_outbuf_t *o, size_t extra)
{
    /* cap >= len is an invariant, so the subtraction cannot wrap. */
    if (DYN_LIKELY(extra <= o->cap - o->len))
        return 0;
    return dyn_ob_grow(o, extra);
}

/* Ask for an exact capacity up front. Used where the final size is known (or
 * soundly bounded) so the buffer is allocated once instead of doubled into
 * existence, which for a 12 MB payload is ~14 reallocs and ~24 MB of copying. */
static int dyn_ob_reserve(dyn_outbuf_t *o, size_t want)
{
    uint8_t *nb;
    if (want <= o->cap)
        return 0;
    if (want > DYN_MAX_OUTPUT)
        want = DYN_MAX_OUTPUT;
    if (want <= o->cap)
        return 0;
    nb = (uint8_t *)realloc(o->buf, want);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = want;
    return 0;
}

/* ---------- static code tables (generated, RFC 1951 3.2.5 / 3.2.6) --------
 *
 * dyn_fix_ll     encoder: fixed literal/length codes, PRE-REVERSED, packed as
 *                (reversed_code << 5) | bit_length. Replaces a per-symbol
 *                branch chain plus a bit-at-a-time reversal loop with one load.
 * dyn_len_code   encoder: match length (3..258) -> length code index, direct
 *                indexed by length-3. Replaces a 28-step linear scan.
 * dyn_dist_code  encoder: distance -> distance code index. The low half is
 *                indexed by dist-1 for dist <= 256; above that the index is
 *                256 + ((dist-1) >> 7), which is exact because every distance
 *                base above 256 is a multiple of 128. Replaces a 29-step scan.
 * dyn_fixtab_*   decoder: the fixed block's root tables, (symbol << 4) | length.
 *                Every fixed literal/length code is <= 9 bits and every fixed
 *                distance code is exactly 5, so these are complete -- a fixed
 *                block never touches the slow path and never builds a table.
 */
/* ---- generated by tools/gen-dyn-tables.c; regenerate, do not hand-edit ---- */
static const uint16_t dyn_fix_ll[288] = {
    0x0188,0x1188,0x0988,0x1988,0x0588,0x1588,0x0D88,0x1D88,0x0388,0x1388,0x0B88,0x1B88,
    0x0788,0x1788,0x0F88,0x1F88,0x0048,0x1048,0x0848,0x1848,0x0448,0x1448,0x0C48,0x1C48,
    0x0248,0x1248,0x0A48,0x1A48,0x0648,0x1648,0x0E48,0x1E48,0x0148,0x1148,0x0948,0x1948,
    0x0548,0x1548,0x0D48,0x1D48,0x0348,0x1348,0x0B48,0x1B48,0x0748,0x1748,0x0F48,0x1F48,
    0x00C8,0x10C8,0x08C8,0x18C8,0x04C8,0x14C8,0x0CC8,0x1CC8,0x02C8,0x12C8,0x0AC8,0x1AC8,
    0x06C8,0x16C8,0x0EC8,0x1EC8,0x01C8,0x11C8,0x09C8,0x19C8,0x05C8,0x15C8,0x0DC8,0x1DC8,
    0x03C8,0x13C8,0x0BC8,0x1BC8,0x07C8,0x17C8,0x0FC8,0x1FC8,0x0028,0x1028,0x0828,0x1828,
    0x0428,0x1428,0x0C28,0x1C28,0x0228,0x1228,0x0A28,0x1A28,0x0628,0x1628,0x0E28,0x1E28,
    0x0128,0x1128,0x0928,0x1928,0x0528,0x1528,0x0D28,0x1D28,0x0328,0x1328,0x0B28,0x1B28,
    0x0728,0x1728,0x0F28,0x1F28,0x00A8,0x10A8,0x08A8,0x18A8,0x04A8,0x14A8,0x0CA8,0x1CA8,
    0x02A8,0x12A8,0x0AA8,0x1AA8,0x06A8,0x16A8,0x0EA8,0x1EA8,0x01A8,0x11A8,0x09A8,0x19A8,
    0x05A8,0x15A8,0x0DA8,0x1DA8,0x03A8,0x13A8,0x0BA8,0x1BA8,0x07A8,0x17A8,0x0FA8,0x1FA8,
    0x0269,0x2269,0x1269,0x3269,0x0A69,0x2A69,0x1A69,0x3A69,0x0669,0x2669,0x1669,0x3669,
    0x0E69,0x2E69,0x1E69,0x3E69,0x0169,0x2169,0x1169,0x3169,0x0969,0x2969,0x1969,0x3969,
    0x0569,0x2569,0x1569,0x3569,0x0D69,0x2D69,0x1D69,0x3D69,0x0369,0x2369,0x1369,0x3369,
    0x0B69,0x2B69,0x1B69,0x3B69,0x0769,0x2769,0x1769,0x3769,0x0F69,0x2F69,0x1F69,0x3F69,
    0x00E9,0x20E9,0x10E9,0x30E9,0x08E9,0x28E9,0x18E9,0x38E9,0x04E9,0x24E9,0x14E9,0x34E9,
    0x0CE9,0x2CE9,0x1CE9,0x3CE9,0x02E9,0x22E9,0x12E9,0x32E9,0x0AE9,0x2AE9,0x1AE9,0x3AE9,
    0x06E9,0x26E9,0x16E9,0x36E9,0x0EE9,0x2EE9,0x1EE9,0x3EE9,0x01E9,0x21E9,0x11E9,0x31E9,
    0x09E9,0x29E9,0x19E9,0x39E9,0x05E9,0x25E9,0x15E9,0x35E9,0x0DE9,0x2DE9,0x1DE9,0x3DE9,
    0x03E9,0x23E9,0x13E9,0x33E9,0x0BE9,0x2BE9,0x1BE9,0x3BE9,0x07E9,0x27E9,0x17E9,0x37E9,
    0x0FE9,0x2FE9,0x1FE9,0x3FE9,0x0007,0x0807,0x0407,0x0C07,0x0207,0x0A07,0x0607,0x0E07,
    0x0107,0x0907,0x0507,0x0D07,0x0307,0x0B07,0x0707,0x0F07,0x0087,0x0887,0x0487,0x0C87,
    0x0287,0x0A87,0x0687,0x0E87,0x0068,0x1068,0x0868,0x1868,0x0468,0x1468,0x0C68,0x1C68,
};

static const uint8_t dyn_len_code[256] = {
    0,1,2,3,4,5,6,7,8,8,9,9,10,10,11,11,12,12,12,12,13,13,13,13,
    14,14,14,14,15,15,15,15,16,16,16,16,16,16,16,16,17,17,17,17,17,17,17,17,
    18,18,18,18,18,18,18,18,19,19,19,19,19,19,19,19,20,20,20,20,20,20,20,20,
    20,20,20,20,20,20,20,20,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    22,22,22,22,22,22,22,22,22,22,22,22,22,22,22,22,23,23,23,23,23,23,23,23,
    23,23,23,23,23,23,23,23,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,
    24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,25,25,25,25,25,25,25,25,
    25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
    26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,
    26,26,26,26,26,26,26,26,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,28,
};

static const uint8_t dyn_dist_code[512] = {
    0,1,2,3,4,4,5,5,6,6,6,6,7,7,7,7,8,8,8,8,8,8,8,8,
    9,9,9,9,9,9,9,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,12,12,12,12,12,12,12,12,
    12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,
    13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
    13,13,13,13,13,13,13,13,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
    14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
    14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    0,14,16,17,18,18,19,19,20,20,20,20,21,21,21,21,22,22,22,22,22,22,22,22,
    23,23,23,23,23,23,23,23,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,
    25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,26,26,26,26,26,26,26,26,
    26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,26,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    27,27,27,27,27,27,27,27,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,
    28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,
    28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
};

static const uint16_t dyn_fixtab_ll[512] = {
    0x1007,0x0508,0x0108,0x1188,0x1107,0x0708,0x0308,0x0C09,0x1087,0x0608,0x0208,0x0A09,
    0x0008,0x0808,0x0408,0x0E09,0x1047,0x0588,0x0188,0x0909,0x1147,0x0788,0x0388,0x0D09,
    0x10C7,0x0688,0x0288,0x0B09,0x0088,0x0888,0x0488,0x0F09,0x1027,0x0548,0x0148,0x11C8,
    0x1127,0x0748,0x0348,0x0C89,0x10A7,0x0648,0x0248,0x0A89,0x0048,0x0848,0x0448,0x0E89,
    0x1067,0x05C8,0x01C8,0x0989,0x1167,0x07C8,0x03C8,0x0D89,0x10E7,0x06C8,0x02C8,0x0B89,
    0x00C8,0x08C8,0x04C8,0x0F89,0x1017,0x0528,0x0128,0x11A8,0x1117,0x0728,0x0328,0x0C49,
    0x1097,0x0628,0x0228,0x0A49,0x0028,0x0828,0x0428,0x0E49,0x1057,0x05A8,0x01A8,0x0949,
    0x1157,0x07A8,0x03A8,0x0D49,0x10D7,0x06A8,0x02A8,0x0B49,0x00A8,0x08A8,0x04A8,0x0F49,
    0x1037,0x0568,0x0168,0x11E8,0x1137,0x0768,0x0368,0x0CC9,0x10B7,0x0668,0x0268,0x0AC9,
    0x0068,0x0868,0x0468,0x0EC9,0x1077,0x05E8,0x01E8,0x09C9,0x1177,0x07E8,0x03E8,0x0DC9,
    0x10F7,0x06E8,0x02E8,0x0BC9,0x00E8,0x08E8,0x04E8,0x0FC9,0x1007,0x0518,0x0118,0x1198,
    0x1107,0x0718,0x0318,0x0C29,0x1087,0x0618,0x0218,0x0A29,0x0018,0x0818,0x0418,0x0E29,
    0x1047,0x0598,0x0198,0x0929,0x1147,0x0798,0x0398,0x0D29,0x10C7,0x0698,0x0298,0x0B29,
    0x0098,0x0898,0x0498,0x0F29,0x1027,0x0558,0x0158,0x11D8,0x1127,0x0758,0x0358,0x0CA9,
    0x10A7,0x0658,0x0258,0x0AA9,0x0058,0x0858,0x0458,0x0EA9,0x1067,0x05D8,0x01D8,0x09A9,
    0x1167,0x07D8,0x03D8,0x0DA9,0x10E7,0x06D8,0x02D8,0x0BA9,0x00D8,0x08D8,0x04D8,0x0FA9,
    0x1017,0x0538,0x0138,0x11B8,0x1117,0x0738,0x0338,0x0C69,0x1097,0x0638,0x0238,0x0A69,
    0x0038,0x0838,0x0438,0x0E69,0x1057,0x05B8,0x01B8,0x0969,0x1157,0x07B8,0x03B8,0x0D69,
    0x10D7,0x06B8,0x02B8,0x0B69,0x00B8,0x08B8,0x04B8,0x0F69,0x1037,0x0578,0x0178,0x11F8,
    0x1137,0x0778,0x0378,0x0CE9,0x10B7,0x0678,0x0278,0x0AE9,0x0078,0x0878,0x0478,0x0EE9,
    0x1077,0x05F8,0x01F8,0x09E9,0x1177,0x07F8,0x03F8,0x0DE9,0x10F7,0x06F8,0x02F8,0x0BE9,
    0x00F8,0x08F8,0x04F8,0x0FE9,0x1007,0x0508,0x0108,0x1188,0x1107,0x0708,0x0308,0x0C19,
    0x1087,0x0608,0x0208,0x0A19,0x0008,0x0808,0x0408,0x0E19,0x1047,0x0588,0x0188,0x0919,
    0x1147,0x0788,0x0388,0x0D19,0x10C7,0x0688,0x0288,0x0B19,0x0088,0x0888,0x0488,0x0F19,
    0x1027,0x0548,0x0148,0x11C8,0x1127,0x0748,0x0348,0x0C99,0x10A7,0x0648,0x0248,0x0A99,
    0x0048,0x0848,0x0448,0x0E99,0x1067,0x05C8,0x01C8,0x0999,0x1167,0x07C8,0x03C8,0x0D99,
    0x10E7,0x06C8,0x02C8,0x0B99,0x00C8,0x08C8,0x04C8,0x0F99,0x1017,0x0528,0x0128,0x11A8,
    0x1117,0x0728,0x0328,0x0C59,0x1097,0x0628,0x0228,0x0A59,0x0028,0x0828,0x0428,0x0E59,
    0x1057,0x05A8,0x01A8,0x0959,0x1157,0x07A8,0x03A8,0x0D59,0x10D7,0x06A8,0x02A8,0x0B59,
    0x00A8,0x08A8,0x04A8,0x0F59,0x1037,0x0568,0x0168,0x11E8,0x1137,0x0768,0x0368,0x0CD9,
    0x10B7,0x0668,0x0268,0x0AD9,0x0068,0x0868,0x0468,0x0ED9,0x1077,0x05E8,0x01E8,0x09D9,
    0x1177,0x07E8,0x03E8,0x0DD9,0x10F7,0x06E8,0x02E8,0x0BD9,0x00E8,0x08E8,0x04E8,0x0FD9,
    0x1007,0x0518,0x0118,0x1198,0x1107,0x0718,0x0318,0x0C39,0x1087,0x0618,0x0218,0x0A39,
    0x0018,0x0818,0x0418,0x0E39,0x1047,0x0598,0x0198,0x0939,0x1147,0x0798,0x0398,0x0D39,
    0x10C7,0x0698,0x0298,0x0B39,0x0098,0x0898,0x0498,0x0F39,0x1027,0x0558,0x0158,0x11D8,
    0x1127,0x0758,0x0358,0x0CB9,0x10A7,0x0658,0x0258,0x0AB9,0x0058,0x0858,0x0458,0x0EB9,
    0x1067,0x05D8,0x01D8,0x09B9,0x1167,0x07D8,0x03D8,0x0DB9,0x10E7,0x06D8,0x02D8,0x0BB9,
    0x00D8,0x08D8,0x04D8,0x0FB9,0x1017,0x0538,0x0138,0x11B8,0x1117,0x0738,0x0338,0x0C79,
    0x1097,0x0638,0x0238,0x0A79,0x0038,0x0838,0x0438,0x0E79,0x1057,0x05B8,0x01B8,0x0979,
    0x1157,0x07B8,0x03B8,0x0D79,0x10D7,0x06B8,0x02B8,0x0B79,0x00B8,0x08B8,0x04B8,0x0F79,
    0x1037,0x0578,0x0178,0x11F8,0x1137,0x0778,0x0378,0x0CF9,0x10B7,0x0678,0x0278,0x0AF9,
    0x0078,0x0878,0x0478,0x0EF9,0x1077,0x05F8,0x01F8,0x09F9,0x1177,0x07F8,0x03F8,0x0DF9,
    0x10F7,0x06F8,0x02F8,0x0BF9,0x00F8,0x08F8,0x04F8,0x0FF9,
};

static const uint16_t dyn_fixtab_dist[512] = {
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,
    0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,
    0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,
    0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,
    0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,
    0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,
    0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,
    0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,
    0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,
    0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,
    0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,
    0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,
    0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,
    0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,
    0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,
    0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,
    0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,
    0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,
    0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,
    0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,
    0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,
    0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,
    0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,
    0x0075,0x0175,0x00F5,0x0000,0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,
    0x0025,0x0125,0x00A5,0x01A5,0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,
    0x0055,0x0155,0x00D5,0x01D5,0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
    0x0005,0x0105,0x0085,0x0185,0x0045,0x0145,0x00C5,0x01C5,0x0025,0x0125,0x00A5,0x01A5,
    0x0065,0x0165,0x00E5,0x0000,0x0015,0x0115,0x0095,0x0195,0x0055,0x0155,0x00D5,0x01D5,
    0x0035,0x0135,0x00B5,0x01B5,0x0075,0x0175,0x00F5,0x0000,
};

/* 5-bit reversal: fixed-Huffman distance codes are the 5-bit symbol index
 * MSB-first, so emitting LSB-first needs the reversal. 32 bytes of .rodata
 * instead of a 5-iteration loop per match. */
static const uint8_t dyn_rev5[32] = { 0x00,0x10,0x08,0x18,0x04,0x14,0x0C,0x1C,0x02,0x12,0x0A,0x1A,0x06,0x16,0x0E,0x1E,0x01,0x11,0x09,0x19,0x05,0x15,0x0D,0x1D,0x03,0x13,0x0B,0x1B,0x07,0x17,0x0F,0x1F };
/* ---- end generated tables ---- */

/* Length/distance base values + extra bits (RFC 1951 3.2.5). */
static const short dyn_lens[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
    83, 99, 115, 131, 163, 195, 227, 258
};
static const short dyn_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0
};
static const short dyn_dists[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short dyn_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

/* ---------- bit reader over an untrusted buffer ---------------------------
 *
 * 64-bit accumulator, LSB-first. The invariant is that bits [0, bitcnt) of
 * bitbuf are real input and everything above is zero, which is what lets a
 * decode peek more bits than it has and still be correct: a symbol whose code
 * length exceeds bitcnt is rejected, and one that does not is determined
 * entirely by real bits.
 *
 * Refilling used to be a loop that tested `pos >= len` once per BIT. It is now
 * one 8-byte load whenever 8 bytes are available -- the pointer advances by
 * (63 - bitcnt) >> 3 and the count saturates to 56, both branchless -- and the
 * byte-at-a-time loop survives only for the last few bytes of the stream. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;    /* next unread byte */
    uint64_t bitbuf;
    int bitcnt;
    int error;     /* set once a read runs past the end */
} dyn_bitreader_t;

DYN_INLINE void dyn_refill(dyn_bitreader_t *s)
{
    if (DYN_LIKELY(s->pos + 8 <= s->len)) {
        s->bitbuf |= dyn_ld64le(s->data + s->pos) << s->bitcnt;
        s->pos += (size_t)((63 - s->bitcnt) >> 3);
        s->bitcnt |= 56;
        /* Re-establish "above bitcnt is zero". The bits discarded here are the
         * tail of the load whose bytes `pos` was deliberately not advanced
         * past, so they are re-read by the next refill, not lost. */
        s->bitbuf &= ((uint64_t)1 << s->bitcnt) - 1;
    } else {
        while (s->bitcnt <= 56 && s->pos < s->len) {
            s->bitbuf |= (uint64_t)s->data[s->pos++] << s->bitcnt;
            s->bitcnt += 8;
        }
    }
}

/* Read `need` (0..16) bits LSB-first. On underflow sets error and returns 0. */
DYN_INLINE unsigned dyn_bits(dyn_bitreader_t *s, int need)
{
    unsigned v;
    if (DYN_UNLIKELY(s->bitcnt < need)) {
        dyn_refill(s);
        if (DYN_UNLIKELY(s->bitcnt < need)) {
            s->error = 1;
            return 0;
        }
    }
    v = (unsigned)(s->bitbuf & (((uint64_t)1 << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

/* ---------- Huffman decoding (canonical, RFC 1951 3.2) --------------------- */

#define DYN_MAXBITS   15
#define DYN_MAXLCODES 286
#define DYN_MAXDCODES 30
#define DYN_FIXLCODES 288
#define DYN_MAXCODES  (DYN_MAXLCODES + DYN_MAXDCODES) /* 316 */

/* Root table width. 9 bits covers every fixed literal/length code and the vast
 * majority of dynamic ones; longer codes fall through to the canonical walk. */
#define DYN_ROOT      9
#define DYN_ROOT_SIZE (1 << DYN_ROOT)

typedef struct {
    const uint16_t *fast;              /* (symbol << 4) | len; 0 = not in table */
    int root_complete;                 /* every code fits in the root table */
    short count[DYN_MAXBITS + 1];
    short symbol[DYN_MAXLCODES];
    uint16_t table[DYN_ROOT_SIZE];     /* backing store when built at runtime */
} dyn_huff_t;

/* Build canonical count[]/symbol[] tables from per-symbol code lengths, and the
 * root lookup alongside. Returns 0 for a complete code, >0 for an incomplete
 * code, <0 for an over-subscribed (invalid) code -- the same contract, and the
 * same over-subscription arithmetic, as before. `length[i]` is trusted to be in
 * [0, DYN_MAXBITS]. */
static int dyn_huff_build(dyn_huff_t *h, const short *length, int n)
{
    short offs[DYN_MAXBITS + 1];
    unsigned nextcode[DYN_MAXBITS + 1];
    unsigned code;
    int sym, len, left;

    for (len = 0; len <= DYN_MAXBITS; len++)
        h->count[len] = 0;
    for (sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n) {
        memset(h->table, 0, sizeof h->table);
        h->fast = h->table;
        h->root_complete = 0;
        return 0; /* no codes */
    }
    left = 1;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left; /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < DYN_MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (short)sym;

    /* Canonical first-code per length, then scatter every code short enough to
     * live in the root table across all indices sharing its low bits. */
    memset(h->table, 0, sizeof h->table);
    code = 0;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        code = (code + (unsigned)h->count[len - 1]) << 1;
        nextcode[len] = code;
    }
    for (sym = 0; sym < n; sym++) {
        int l = length[sym];
        if (l == 0 || l > DYN_ROOT) {
            if (l)
                nextcode[l]++;
            continue;
        }
        {
            unsigned c = nextcode[l]++;
            unsigned r = 0, k;
            int b;
            for (b = 0; b < l; b++) { r = (r << 1) | (c & 1u); c >>= 1; }
            for (k = r; k < DYN_ROOT_SIZE; k += (1u << l))
                h->table[k] = (uint16_t)(((unsigned)sym << 4) | (unsigned)l);
        }
    }
    h->fast = h->table;
    /* If the code is complete AND no code is longer than the root, every root
     * index resolves and the canonical walk is unreachable -- so a lookup that
     * fails can only be a truncated stream, and says so without walking. */
    {
        int over = 0;
        for (len = DYN_ROOT + 1; len <= DYN_MAXBITS; len++)
            over |= h->count[len];
        h->root_complete = (left == 0 && over == 0);
    }
    return left;
}

/* The canonical bit-at-a-time walk, unchanged in behaviour: it is the code that
 * makes an invalid or over-long code return -1, and it is kept verbatim in
 * meaning so the root table is purely an accelerator over it. */
DYN_NOINLINE static int dyn_huff_slow(dyn_bitreader_t *s, const dyn_huff_t *h)
{
    int code = 0, first = 0, index = 0, len, cnt;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        if (DYN_UNLIKELY(s->bitcnt < 1)) {
            dyn_refill(s);
            if (s->bitcnt < 1) { s->error = 1; return -1; }
        }
        code |= (int)(s->bitbuf & 1u);
        s->bitbuf >>= 1;
        s->bitcnt--;
        cnt = h->count[len];
        if (code - cnt < first)
            return h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* One symbol. The root table resolves it in a load and a shift; anything longer
 * than DYN_ROOT bits, and anything at a truncated tail, takes the slow walk. */
DYN_INLINE int dyn_huff_decode(dyn_bitreader_t *s, const dyn_huff_t *h)
{
    unsigned e;
    int l;
    if (DYN_UNLIKELY(s->bitcnt < DYN_MAXBITS))
        dyn_refill(s);
    e = h->fast[s->bitbuf & (DYN_ROOT_SIZE - 1)];
    l = (int)(e & 15u);
    if (DYN_LIKELY(l != 0 && l <= s->bitcnt)) {
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return (int)(e >> 4);
    }
    if (h->root_complete) {
        s->error = 1;               /* ran out of input mid-symbol */
        return -1;
    }
    return dyn_huff_slow(s, h);
}

/* ---------- block decoders ------------------------------------------------ */

/* Copy a match that may overlap its own destination. `dist` is the back
 * distance; the caller guarantees at least 8 writable bytes past op + n, which
 * is what licenses the 8-byte granular loops (they may overshoot, never past
 * the reservation, and o->len is set from n regardless).
 *
 * For dist < 8 the run cannot be read 8 bytes at a time from `dist` back, so
 * the first pass materialises d8 bytes -- the smallest multiple of dist that is
 * at least 8 -- after which the sequence is periodic with period d8 and can be
 * copied in 8-byte gulps from a fixed offset. memcpy with dist 1 would
 * replicate garbage; this replicates the run, which is what the format means. */
DYN_INLINE void dyn_copy_match(uint8_t *op, size_t dist, size_t n)
{
    const uint8_t *mp = op - dist;
    size_t k = 0;

    if (DYN_LIKELY(dist >= 8)) {
        do {
            memcpy(op + k, mp + k, 8);
            k += 8;
        } while (k < n);
        return;
    }
    {
        size_t d8 = dist;
        while (d8 < 8)
            d8 += dist;                 /* multiple of dist, 8..14 */
        while (k < n && k < d8) {
            op[k] = mp[k];
            k++;
        }
        while (k < n) {
            memcpy(op + k, op + k - d8, 8);
            k += 8;
        }
    }
}

/* Decode literal/length symbols until the end-of-block (256).
 *
 * The output pointer is held in a register and the capacity is checked once per
 * SYMBOL against a slack window, not once per byte through a function call.
 * DYN_OSLACK is sized so that a maximal match (258) plus the 8-byte overshoot
 * of dyn_copy_match always fits after the check. */
#define DYN_OSLACK 320

static int dyn_inflate_codes(dyn_bitreader_t *s, dyn_outbuf_t *o,
                             const dyn_huff_t *lh, const dyn_huff_t *dh)
{
    uint8_t *op = o->buf + o->len;
    uint8_t *oend = o->buf + o->cap;

    for (;;) {
        int sym;

        if (DYN_UNLIKELY((size_t)(oend - op) < DYN_OSLACK)) {
            o->len = (size_t)(op - o->buf);
            if (dyn_ob_ensure(o, DYN_OSLACK))
                return -1;
            op = o->buf + o->len;
            oend = o->buf + o->cap;
        }

        sym = dyn_huff_decode(s, lh);
        if (DYN_UNLIKELY(sym < 0))
            return -1;
        if (DYN_LIKELY(sym < 256)) {
            *op++ = (uint8_t)sym;
            continue;
        }
        if (sym == 256) {               /* end of block */
            o->len = (size_t)(op - o->buf);
            return 0;
        }
        {
            int isym = sym - 257;
            int dsv;
            size_t length, dist;

            if (DYN_UNLIKELY(isym >= 29))
                return -1;              /* 285 is the last valid length code */
            length = (size_t)dyn_lens[isym] + dyn_bits(s, dyn_lext[isym]);
            dsv = dyn_huff_decode(s, dh);
            if (DYN_UNLIKELY(dsv < 0 || dsv >= 30))
                return -1;
            dist = (size_t)dyn_dists[dsv] + dyn_bits(s, dyn_dext[dsv]);
            if (DYN_UNLIKELY(s->error))
                return -1;
            if (DYN_UNLIKELY(dist > (size_t)(op - o->buf)))
                return -1;              /* reference before the start of output */
            dyn_copy_match(op, dist, length);
            op += length;
        }
    }
}

/* Copy a stored (uncompressed) block. */
static int dyn_inflate_stored(dyn_bitreader_t *s, dyn_outbuf_t *o)
{
    unsigned len, nlen;

    /* Discard bits back to a byte boundary. The reader may hold whole bytes in
     * the accumulator, so rewind `pos` by them rather than dropping them. */
    s->pos -= (size_t)(s->bitcnt >> 3);
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->len) {
        s->error = 1;
        return -1;
    }
    len = (unsigned)s->data[s->pos] | ((unsigned)s->data[s->pos + 1] << 8);
    nlen = (unsigned)s->data[s->pos + 2] | ((unsigned)s->data[s->pos + 3] << 8);
    s->pos += 4;
    if ((len ^ 0xffffu) != nlen)
        return -1; /* LEN/NLEN must be one's complements */
    if (len > s->len - s->pos) {
        s->error = 1;
        return -1;
    }
    if (dyn_ob_ensure(o, len))
        return -1;
    if (len) {
        memcpy(o->buf + o->len, s->data + s->pos, len);
        o->len += len;
        s->pos += len;
    }
    return 0;
}

/* Decode a fixed block. The tables are const .rodata: nothing is built. */
static int dyn_inflate_fixed(dyn_bitreader_t *s, dyn_outbuf_t *o)
{
    dyn_huff_t lh, dh;

    lh.fast = dyn_fixtab_ll;
    lh.root_complete = 1;
    dh.fast = dyn_fixtab_dist;
    dh.root_complete = 1;
    /* count/symbol are only consulted by the slow path, which the fixed root
     * tables never reach: every fixed code is <= 9 bits and both tables are
     * complete, so a lookup always yields len != 0. */
    return dyn_inflate_codes(s, o, &lh, &dh);
}

/* Read the dynamic Huffman header, build the tables, and decode the block. */
static int dyn_inflate_dynamic(dyn_bitreader_t *s, dyn_outbuf_t *o,
                               dyn_huff_t *lh, dyn_huff_t *dh, dyn_huff_t *ch)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    short lengths[DYN_MAXCODES];
    int hlit, hdist, hclen, idx, err, total, sym;

    hlit = (int)dyn_bits(s, 5) + 257;
    hdist = (int)dyn_bits(s, 5) + 1;
    hclen = (int)dyn_bits(s, 4) + 4;
    if (s->error)
        return -1;
    if (hlit > DYN_MAXLCODES || hdist > DYN_MAXDCODES)
        return -1;

    for (idx = 0; idx < 19; idx++)
        lengths[idx] = 0;
    for (idx = 0; idx < hclen; idx++)
        lengths[order[idx]] = (short)dyn_bits(s, 3);
    if (s->error)
        return -1;
    if (dyn_huff_build(ch, lengths, 19) != 0)
        return -1; /* code-length code must be complete */

    total = hlit + hdist;
    idx = 0;
    while (idx < total) {
        sym = dyn_huff_decode(s, ch);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[idx++] = (short)sym;
        } else {
            int rep;
            short val = 0;
            if (sym == 16) {
                if (idx == 0)
                    return -1; /* nothing to repeat */
                val = lengths[idx - 1];
                rep = 3 + (int)dyn_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + (int)dyn_bits(s, 3);
            } else {
                rep = 11 + (int)dyn_bits(s, 7);
            }
            if (s->error)
                return -1;
            if (idx + rep > total)
                return -1; /* would overrun the length list */
            while (rep--)
                lengths[idx++] = val;
        }
    }
    if (lengths[256] == 0)
        return -1; /* no end-of-block code */

    err = dyn_huff_build(lh, lengths, hlit);
    if (err && (err < 0 || hlit != lh->count[0] + lh->count[1]))
        return -1; /* incomplete ok only for a single one-bit code */
    err = dyn_huff_build(dh, lengths + hlit, hdist);
    if (err && (err < 0 || hdist != dh->count[0] + dh->count[1]))
        return -1;
    return dyn_inflate_codes(s, o, lh, dh);
}

/* Inflate a raw DEFLATE stream (RFC 1951) into `o`.
 *
 * The three dynamic-block tables are allocated ONCE for the whole stream rather
 * than per block: they are ~1.4 KiB each, and a multi-block stream used to put
 * all of that on the stack again for every block. */
static int dyn_inflate(const uint8_t *src, size_t src_len, dyn_outbuf_t *o)
{
    dyn_bitreader_t s;
    dyn_huff_t *tabs;
    int last, type, rc = -1;

    s.data = src;
    s.len = src_len;
    s.pos = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.error = 0;

    tabs = (dyn_huff_t *)malloc(sizeof(dyn_huff_t) * 3);
    if (!tabs)
        return -1;

    do {
        last = (int)dyn_bits(&s, 1);
        type = (int)dyn_bits(&s, 2);
        if (s.error)
            goto out;
        if (type == 0)
            rc = dyn_inflate_stored(&s, o);
        else if (type == 1)
            rc = dyn_inflate_fixed(&s, o);
        else if (type == 2)
            rc = dyn_inflate_dynamic(&s, o, &tabs[0], &tabs[1], &tabs[2]);
        else
            goto out; /* reserved block type */
        if (rc)
            goto out;
    } while (!last);
    rc = 0;
out:
    free(tabs);
    return rc;
}

/* ---------- DEFLATE encoder (RFC 1951, fixed-Huffman) ----------------------
 *
 * LSB-first bit writer over a caller-owned span. Codes are pre-reversed in
 * .rodata (see dyn_fix_ll) so emitting them LSB-first reproduces the RFC's
 * MSB-first packing; extra bits are written LSB-first directly.
 *
 * The accumulator is 64 bits and is drained by ONE 8-byte store, so a symbol
 * costs an OR, an add, and an amortised fraction of a store -- where it
 * previously cost a call, a capacity check, and a byte store per output byte.
 * `oend` is a hard ceiling, not a growth trigger: see dyn_deflate_fixed. */
typedef struct {
    uint8_t *op;
    uint8_t *oend;     /* op may not pass this; reaching it means "use stored" */
    uint64_t acc;
    int nbits;
} dyn_bw_t;

/* Add up to 32 bits. Never touches memory; the caller drains. */
DYN_INLINE void dyn_bw_add(dyn_bw_t *w, unsigned value, int nbits)
{
    w->acc |= (uint64_t)value << w->nbits;
    w->nbits += nbits;
}

/* Drain whole bytes with a single unaligned store. Safe to write 8 bytes at
 * `op` because the buffer carries 16 bytes of slack past `oend`. */
DYN_INLINE void dyn_bw_drain(dyn_bw_t *w)
{
    dyn_st64le(w->op, w->acc);
    w->op += (size_t)(w->nbits >> 3);
    w->acc >>= (w->nbits & ~7);
    w->nbits &= 7;
}

/* ---------- LZ77 match finder (hash-chain, greedy) ------------------------- */

#define DYN_MIN_MATCH  3
#define DYN_MAX_MATCH  258
#define DYN_WSIZE      ((size_t)32768)  /* max back-reference distance */
#define DYN_HASH_BITS  15
#define DYN_HASH_SIZE  ((size_t)1 << DYN_HASH_BITS)
#define DYN_MAX_CHAIN  256              /* chain-walk cap: bounds encode time */

#define DYN_LZ4_MIN_MATCH   4
#define DYN_LZ4_LAST_LITERALS 5
#define DYN_LZ4_MF_LIMIT    12          /* no match may start inside this tail */
#define DYN_LZ4_HASH_BITS   14
#define DYN_LZ4_HASH_SIZE   ((size_t)1 << DYN_LZ4_HASH_BITS)
#define DYN_LZ4_MAX_DIST    65535

/* The position ring. A chain entry is the GAP to the previous occurrence of the
 * same hash, and a gap that matters is at most 65535 because that is the widest
 * window either format allows -- so an entry is 16 bits and the ring is a fixed
 * 128 KiB no matter how long the input is. 0 terminates a chain. */
#define DYN_PREV_SIZE  ((size_t)65536)
#define DYN_PREV_MASK  (DYN_PREV_SIZE - 1)

/* A multiplicative hash over the first 3 (DEFLATE) or 4 (LZ4) bytes. The old
 * DEFLATE hash was (p0 << 10) ^ (p1 << 5) ^ p2 masked to 15 bits, which
 * discards the top 3 bits of p0 entirely -- p0 and p0 ^ 0x20 collide by
 * construction. Chain length is what the match finder's cost is made of, so a
 * hash that spreads properly is a CPU optimisation before it is anything else. */
DYN_INLINE uint32_t dyn_hash3(const uint8_t *p, int shift)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> shift;
}

/* The canonical LZ4 hash: a 4-byte read multiplied by a Knuth constant, top
 * bits kept. Reading the 4 bytes explicitly rather than casting keeps it
 * endian-independent and alignment-safe. */
DYN_INLINE uint32_t dyn_lz4_hash_at(const uint8_t *p, int shift)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (v * 2654435761u) >> shift;
}

/* ---------- the reusable match-finder scratch -----------------------------
 *
 * Both tables are now FIXED SIZE, which is the whole point: `prev` used to be
 * 4 bytes of heap per input byte, allocated on every call, so compressing a
 * 12 MB payload allocated 51 MB of chain plus 64 KiB of heads. It is now a
 * 128 KiB ring, and a context holds 256 KiB total, for any input up to the
 * 2 GiB indexing limit.
 *
 * `head` still stores position + base so that a stale entry is detected by one
 * comparison instead of by clearing 128 KiB per call: the base advances past
 * the end of every input, so no entry from an earlier call can be mistaken for
 * a live one, and the table is genuinely wiped only when the base would
 * overflow an int32 -- once per 2 GiB of cumulative input.
 *
 * `prev` needs no clearing at all. A chain is only ever entered from a live
 * head, and is only ever followed while the accumulated distance is inside the
 * window, so every slot read was written by this same call. */
int dyn_comp_ctx_init(dyn_comp_ctx_t *c)
{
    c->head = (int32_t *)calloc(DYN_HASH_SIZE, sizeof(int32_t));
    c->prev = (uint16_t *)malloc(DYN_PREV_SIZE * sizeof(uint16_t));
    c->head_cap = DYN_HASH_SIZE;
    c->prev_cap = DYN_PREV_SIZE;
    c->base = 1;              /* 0 is reserved for "cleared" */
    if (!c->head || !c->prev) {
        free(c->head); free(c->prev);
        c->head = NULL; c->prev = NULL;
        c->head_cap = c->prev_cap = 0;
        return -1;
    }
    return 0;
}

void dyn_comp_ctx_free(dyn_comp_ctx_t *c)
{
    free(c->head);
    free(c->prev);
    c->head = NULL;
    c->prev = NULL;
    c->prev_cap = c->head_cap = 0;
}

/* Scratch for one call. Returns the epoch base positions are stored relative to
 * and the hash shift for the head table actually in use.
 *
 * Without a context there is nothing to keep across calls, so the head table is
 * sized to the input: a 900-byte payload gets a 1024-entry table and clears
 * 4 KiB, where a fixed table would clear 128 KiB to compress 900 bytes. calloc
 * rather than malloc+loop so the allocator can hand back untouched zero pages
 * for the large sizes instead of faulting in every one. */
typedef struct {
    int32_t *head;
    uint16_t *prev;
    int shift;         /* 32 - hash_bits for the head table in use */
    uint32_t base;
    int owned;
} dyn_scratch_t;

/* `need_prev` is 0 when the caller never walks a chain (LZ4 level 1 takes a
 * single candidate per hash, so every read of `prev` is behind its use_chain
 * test). The ring is 128 KiB -- exactly glibc's mmap threshold, so allocating
 * it is an mmap/munmap pair plus first-touch faults, per call, for a table
 * that path provably never touches. It is the DEFAULT level. */
static int dyn_scratch_get(dyn_comp_ctx_t *c, int hash_bits, size_t n,
                           dyn_scratch_t *sc, int need_prev)
{
    int bits = hash_bits;
    size_t heads;

    /* The bucket count is chosen from the FORMAT and the INPUT only -- never
     * from whether a context was supplied. It has to be: the width decides
     * which candidate a hash chain offers, so letting it vary would make the
     * compressed bytes depend on the caller's allocation strategy. (It did, in
     * an earlier draft, and only LZ4 level 1 noticed -- that path takes a
     * single candidate per hash, so it has no chain depth to converge over.) */
    while (bits > 8 && ((size_t)1 << (bits - 1)) >= n + 1)
        bits--;                              /* no more buckets than positions */
    heads = (size_t)1 << bits;

    sc->owned = 0;
    sc->shift = 32 - bits;
    if (c && c->head_cap >= heads && (!need_prev || c->prev_cap >= DYN_PREV_SIZE)) {
        sc->head = c->head;
        sc->prev = c->prev;
        /* Entries from earlier calls are stale by epoch whatever bucket width
         * wrote them, because the base advances past every input -- so a
         * narrower call cannot leave a live-looking entry for a wider one. */
        if (c->base > (uint32_t)INT32_MAX - (uint32_t)(n + 1)) {
            memset(c->head, 0, c->head_cap * sizeof(int32_t));
            c->base = 1;
        }
        sc->base = c->base;
        c->base += (uint32_t)n + 1;
        return 0;
    }
    sc->head = (int32_t *)calloc(heads, sizeof(int32_t));
    sc->prev = need_prev
                 ? (uint16_t *)malloc(DYN_PREV_SIZE * sizeof(uint16_t)) : NULL;
    if (!sc->head || (need_prev && !sc->prev)) {
        free(sc->head);
        free(sc->prev);
        return -1;
    }
    sc->owned = 1;
    sc->base = 1;
    return 0;
}

static void dyn_scratch_put(dyn_scratch_t *sc)
{
    if (sc->owned) {
        free(sc->head);
        free(sc->prev);
    }
}

/* ---------- SWAR match length -------------------------------------------
 *
 * Both encoders' inner loop is "how many bytes do these two positions share?",
 * and it runs for every candidate the hash chain offers, not just the winners.
 *
 * Eight bytes per iteration using a 64-bit load and a count-trailing-zeros on
 * the XOR. Deliberately NOT a `simd.*` dispatch call: on short spans the
 * indirect call costs more than the compare it replaces, and most candidates
 * differ within the first few bytes. This is inline SWAR, which every target
 * compiles to two loads and a CTZ.
 *
 * PORTABILITY, which is the whole reason this is written with memcpy and an
 * #if rather than a cast:
 *   - `memcpy` into a uint64_t is the portable unaligned load. A cast to
 *     `const uint64_t *` is UB on a misaligned address and traps on some
 *     targets; every compiler that matters turns this memcpy into one
 *     instruction.
 *   - the XOR's first differing BYTE is at ctz/8 on little-endian and clz/8 on
 *     big-endian. Getting this wrong produces a wrong match length, which is
 *     still a VALID LZ4/DEFLATE stream -- just a worse or corrupt one -- so it
 *     would not show up as a crash on a big-endian host. Hence both arms. */
#if DYN_BIG_ENDIAN
#define DYN_FIRST_DIFF_BYTE(x) ((unsigned)(__builtin_clzll(x) >> 3))
#else
#define DYN_FIRST_DIFF_BYTE(x) ((unsigned)(__builtin_ctzll(x) >> 3))
#endif

DYN_INLINE size_t dyn_match_len(const uint8_t *a, const uint8_t *b, size_t max_len)
{
    size_t ml = 0;
#if defined(__GNUC__) || defined(__clang__)
    while (ml + 8 <= max_len) {
        uint64_t x = dyn_ld64le(a + ml) ^ dyn_ld64le(b + ml);
        if (x)
            return ml + DYN_FIRST_DIFF_BYTE(x);
        ml += 8;
    }
#endif
    while (ml < max_len && a[ml] == b[ml])
        ml++;
    return ml;
}

/* Emit one fixed-Huffman literal/length symbol from the pre-reversed table. */
DYN_INLINE void dyn_emit_ll(dyn_bw_t *w, unsigned sym)
{
    uint16_t e = dyn_fix_ll[sym];
    dyn_bw_add(w, (unsigned)(e >> 5), (int)(e & 31u));
}

/* Distance -> code index without a scan (see dyn_dist_code). */
DYN_INLINE unsigned dyn_dist_index(unsigned dist)
{
    unsigned d = dist - 1u;
    return d < 256u ? dyn_dist_code[d] : dyn_dist_code[256u + (d >> 7)];
}

/* Encode `src` as a single final fixed-Huffman DEFLATE block into `dst`.
 *
 * `cap` is the size of the STORED representation, and it is a HARD CEILING
 * rather than a growth trigger: the only reason to keep compressing past it is
 * to find out that stored would have been smaller, which is already known. So
 * returning 1 ("would not shrink") happens the instant the ceiling is crossed
 * -- on incompressible input that is the difference between compressing 4 MB
 * and compressing 4 MB and then throwing it away.
 *
 * Returns 0 with *pwritten set, 1 if the output would reach `cap` (caller emits
 * stored), or -1 on an input too large to index / scratch allocation failure.
 * `dst` must have 16 bytes of writable slack past dst + cap. */
static int dyn_deflate_fixed(const uint8_t *src, size_t len,
                             uint8_t *dst, size_t cap, dyn_comp_ctx_t *cx,
                             size_t *pwritten)
{
    dyn_bw_t w;
    dyn_scratch_t sc;
    int32_t *head;
    uint16_t *prev;
    uint32_t base;
    int shift;
    size_t i;
    int rc = 0;

    w.op = dst;
    w.oend = dst + cap;
    w.acc = 0;
    w.nbits = 0;

    /* BFINAL=1, BTYPE=01 (fixed Huffman): the 3 LSB-first bits are 0b011 = 3. */
    dyn_bw_add(&w, 3, 3);

    if (len == 0) {
        dyn_emit_ll(&w, 256);
        dyn_bw_drain(&w);
        if (w.nbits > 0)
            *w.op++ = (uint8_t)(w.acc & 0xffu);
        *pwritten = (size_t)(w.op - dst);
        return *pwritten >= cap ? 1 : 0;
    }
    if (len > (size_t)INT32_MAX)
        return -1; /* positions must fit int32 chain slots */

    if (dyn_scratch_get(cx, DYN_HASH_BITS, len, &sc, 1))
        return -1;
    head = sc.head;
    prev = sc.prev;
    base = sc.base;
    shift = sc.shift;

    i = 0;
    while (i < len) {
        /* Seeded at MIN_MATCH-1, not 0: a match shorter than MIN_MATCH is
         * unusable, so seeding here makes "beat the incumbent" and "is usable
         * at all" the same test, and deletes an unpredictable branch from the
         * innermost loop. best_dist is only read when best_len grew past it. */
        size_t best_len = DYN_MIN_MATCH - 1;
        unsigned best_dist = 0;

        if (DYN_UNLIKELY(w.op >= w.oend)) { rc = 1; goto done; }

        if (DYN_LIKELY(i + DYN_MIN_MATCH <= len)) {
            uint32_t h = dyn_hash3(src + i, shift);
            int32_t hv = head[h];
            head[h] = (int32_t)(i + base);

            if (hv >= (int32_t)base) {
                size_t cur = (size_t)(hv - (int32_t)base);
                size_t dist = i - cur;
                size_t max_len = len - i;
                const uint8_t *a = src + i;
                int chain = DYN_MAX_CHAIN;

                prev[i & DYN_PREV_MASK] =
                    (uint16_t)(dist <= 65535 ? dist : 0);
                if (max_len > DYN_MAX_MATCH)
                    max_len = DYN_MAX_MATCH;

                while (dist <= DYN_WSIZE) {
                    const uint8_t *b = src + cur;
                    /* The next chain link is loaded FIRST. Walking a chain is a
                     * pointer chase, so this load is on the critical path and
                     * everything below is not; issuing it early lets its
                     * latency hide under the comparison work. */
                    uint16_t g = prev[cur & DYN_PREV_MASK];

                    /* Two-byte filter: a candidate cannot beat the incumbent
                     * unless it matches at best_len AND at best_len-1. This is
                     * the cheapest way to not call dyn_match_len, and not
                     * calling it is most of the win on chain-heavy input. */
                    if (b[best_len] == a[best_len] &&
                        b[best_len - 1] == a[best_len - 1]) {
                        size_t ml = dyn_match_len(a, b, max_len);
                        if (ml > best_len) {
                            best_len = ml;
                            best_dist = (unsigned)dist;
                            if (ml >= max_len)
                                break;   /* longest possible here */
                        }
                    }
                    if (g == 0 || --chain <= 0)
                        break;
                    cur -= g;
                    dist += g;
                }
            } else {
                prev[i & DYN_PREV_MASK] = 0;
            }
        }

        if (best_len >= DYN_MIN_MATCH) {
            size_t end = i + best_len;
            size_t k;
            unsigned li = dyn_len_code[best_len - 3];
            unsigned di = dyn_dist_index(best_dist);

            dyn_emit_ll(&w, 257u + li);
            if (dyn_lext[li])
                dyn_bw_add(&w, (unsigned)(best_len - (size_t)dyn_lens[li]),
                           dyn_lext[li]);
            /* Fixed-Huffman distance codes are 5 bits, value == symbol index,
             * MSB-first -- so reverse the 5-bit index for LSB-first emission. */
            dyn_bw_add(&w, dyn_rev5[di], 5);
            if (dyn_dext[di])
                dyn_bw_add(&w, (unsigned)(best_dist - (unsigned)dyn_dists[di]),
                           dyn_dext[di]);
            dyn_bw_drain(&w);

            /* Insert hash entries for the positions the match covered. */
            for (k = i + 1; k < end; k++) {
                if (k + DYN_MIN_MATCH <= len) {
                    uint32_t hh = dyn_hash3(src + k, shift);
                    int32_t pv = head[hh];
                    size_t g = (pv >= (int32_t)base)
                                 ? k - (size_t)(pv - (int32_t)base) : 0;
                    prev[k & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
                    head[hh] = (int32_t)(k + base);
                }
            }
            i = end;
        } else {
            dyn_emit_ll(&w, src[i]);
            dyn_bw_drain(&w);
            i++;
        }
    }

    dyn_emit_ll(&w, 256); /* end-of-block */
    dyn_bw_drain(&w);
    if (w.nbits > 0)
        *w.op++ = (uint8_t)(w.acc & 0xffu);
    if (w.op >= w.oend)
        rc = 1;
    *pwritten = (size_t)(w.op - dst);
done:
    dyn_scratch_put(&sc);
    return rc;
}

/* Encode `src` as DEFLATE stored (uncompressed) blocks into `dst`, which the
 * caller has already sized to exactly the stored representation. Returns the
 * number of bytes written; cannot fail. */
static size_t dyn_deflate_stored(const uint8_t *src, size_t len, uint8_t *dst)
{
    uint8_t *p = dst;
    size_t off;

    if (len == 0) {
        *p++ = 0x01; /* BFINAL=1, BTYPE=00 */
        *p++ = 0x00;
        *p++ = 0x00; /* LEN = 0 */
        *p++ = 0xff;
        *p++ = 0xff; /* NLEN = ~0 */
        return (size_t)(p - dst);
    }
    off = 0;
    while (off < len) {
        size_t chunk = len - off;
        unsigned l, nl;
        if (chunk > DYN_STORED_MAX)
            chunk = DYN_STORED_MAX;
        l = (unsigned)chunk;
        nl = (~l) & 0xffffu;
        *p++ = (uint8_t)(off + chunk == len ? 0x01 : 0x00); /* BFINAL on last */
        *p++ = (uint8_t)(l & 0xff);
        *p++ = (uint8_t)((l >> 8) & 0xff);
        *p++ = (uint8_t)(nl & 0xff);
        *p++ = (uint8_t)((nl >> 8) & 0xff);
        memcpy(p, src + off, chunk);
        p += chunk;
        off += chunk;
    }
    return (size_t)(p - dst);
}

/* ---------- gzip framing --------------------------------------------------- */

/* Build a gzip member around `src`: RFC 1952 header, a real fixed-Huffman
 * DEFLATE body (falling back to stored blocks if that would not shrink the
 * data), then the CRC-32 + ISIZE trailer.
 *
 * ONE allocation, and the payload is never copied. The previous revision built
 * the body into its own growing buffer and then memcpy'd it into a second
 * buffer sized 10 + body + 8, so peak heap held the body twice and every byte
 * of output was written twice. Here the header goes down first, the encoder
 * writes the body directly behind it, and the trailer lands after that; the
 * final realloc only shrinks. */
int dyn_gzip_build_ctx(const uint8_t *src, size_t src_len, dyn_comp_ctx_t *cx,
                       uint8_t **pout, size_t *pout_len)
{
    size_t nblocks, stored_sz, body_len, total;
    uint32_t crc, isize;
    uint8_t *out, *p, *shrunk;
    int rc;

    nblocks = src_len ? (src_len + DYN_STORED_MAX - 1) / DYN_STORED_MAX : 1;
    stored_sz = src_len ? nblocks * 5 + src_len : 5;

    /* 10 header + at most stored_sz body + 8 trailer + 16 bytes of slack for
     * the bit writer's 8-byte drain store. */
    if (stored_sz > SIZE_MAX - 34)
        return -1;
    out = (uint8_t *)malloc(10 + stored_sz + 8 + 16);
    if (!out)
        return -1;
    p = out;

    /* RFC 1952 header: magic, CM=deflate, no FLG, MTIME=0, XFL=0, OS=255. */
    *p++ = 0x1f;
    *p++ = 0x8b;
    *p++ = 0x08;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0xff;

    rc = dyn_deflate_fixed(src, src_len, out + 10, stored_sz, cx, &body_len);
    if (rc != 0) {
        /* rc > 0: fixed Huffman did not shrink it (incompressible input), and
         * the encoder stopped the moment that became true.
         * rc < 0: no compression possible (scratch OOM / oversize). */
        body_len = dyn_deflate_stored(src, src_len, out + 10);
    }

    total = 10 + body_len + 8;
    p = out + 10 + body_len;

    /* RFC 1952 trailer: CRC-32 then ISIZE (mod 2^32), both little-endian. */
    crc = dyn_crc32(src, src_len);
    isize = (uint32_t)(src_len & 0xffffffffu);
    *p++ = (uint8_t)(crc & 0xff);
    *p++ = (uint8_t)((crc >> 8) & 0xff);
    *p++ = (uint8_t)((crc >> 16) & 0xff);
    *p++ = (uint8_t)((crc >> 24) & 0xff);
    *p++ = (uint8_t)(isize & 0xff);
    *p++ = (uint8_t)((isize >> 8) & 0xff);
    *p++ = (uint8_t)((isize >> 16) & 0xff);
    *p++ = (uint8_t)((isize >> 24) & 0xff);

    /* Hand back only what was used. A shrinking realloc is in-place or an
     * mremap; it never copies into a larger region. */
    shrunk = (uint8_t *)realloc(out, total);
    *pout = shrunk ? shrunk : out;
    *pout_len = total;
    return 0;
}

/* Parse a gzip member (RFC 1952), inflate it, and validate the trailer.
 * Returns 0 with `o` filled, or -1 on any malformed/corrupt/truncated input. */
int dyn_gunzip_decode(const uint8_t *src, size_t len, dyn_outbuf_t *o)
{
    size_t start;
    size_t pos = 10;
    uint8_t flg;
    uint32_t crc, isize;

    if (len < 18) /* 10-byte header + 8-byte trailer minimum */
        return -1;
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 0x08)
        return -1;
    flg = src[3];
    if (flg & 0xe0) /* reserved FLG bits must be zero */
        return -1;

    if (flg & 0x04) { /* FEXTRA */
        size_t xlen;
        if (pos + 2 > len)
            return -1;
        xlen = (size_t)src[pos] | ((size_t)src[pos + 1] << 8);
        pos += 2;
        if (xlen > len - pos)
            return -1;
        pos += xlen;
    }
    if (flg & 0x08) { /* FNAME */
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x02) { /* FHCRC */
        if (pos + 2 > len)
            return -1;
        pos += 2;
    }

    if (pos + 8 > len) /* need room for the trailer after the header/fields */
        return -1;

    isize = (uint32_t)src[len - 4] | ((uint32_t)src[len - 3] << 8) |
            ((uint32_t)src[len - 2] << 16) | ((uint32_t)src[len - 1] << 24);

    /* Size the output from ISIZE so an honest member allocates exactly once
     * instead of doubling ~14 times and copying its own contents along the way.
     *
     * ISIZE is attacker-controlled, so it is a HINT, not a promise: it is
     * clamped to what this many input bytes could possibly expand to (DEFLATE's
     * ceiling is 1032:1) and to DYN_MAX_OUTPUT. A 30-byte bomb claiming 1 GiB
     * therefore reserves ~31 KiB, and the real bomb defence -- dyn_ob_ensure
     * refusing to pass DYN_MAX_OUTPUT -- is untouched. */
    {
        size_t hint = isize;
        size_t ceiling = len - 8 - pos;
        if (ceiling > DYN_MAX_OUTPUT / 1032)
            ceiling = DYN_MAX_OUTPUT;
        else
            ceiling = ceiling * 1032 + 4096;
        if (hint > ceiling)
            hint = ceiling;
        if (hint > DYN_MAX_OUTPUT)
            hint = DYN_MAX_OUTPUT;
        if (hint && dyn_ob_reserve(o, o->len + hint + DYN_OSLACK) < 0)
            return -1;
    }

    /* The reserve above APPENDS (o->len + hint), so this member's bytes start
       at `start`, not at 0. Validating the whole buffer made a second member
       decoded into the same outbuf fail its CRC spuriously -- latent only
       because every caller today passes a fresh buffer. Validate the slice. */
    start = o->len;
    if (dyn_inflate(src + pos, len - 8 - pos, o) < 0)
        return -1;

    crc = (uint32_t)src[len - 8] | ((uint32_t)src[len - 7] << 8) |
          ((uint32_t)src[len - 6] << 16) | ((uint32_t)src[len - 5] << 24);
    if (dyn_crc32(o->buf + start, o->len - start) != crc)
        return -1;
    if ((uint32_t)((o->len - start) & 0xffffffffu) != isize)
        return -1;
    return 0;
}

/* ---------- LZ4 block format -----------------------------------------------
 *
 * A sequence is: a token byte, then optional extended lengths, then literals,
 * then a 2-byte little-endian match offset, then optional extended match
 * length. The token's high nibble is the literal count and its low nibble the
 * match length minus 4; a nibble of 15 means "add the following 255-chained
 * bytes". The last sequence in a block is literals only, with no offset.
 *
 * The format's two end-of-block rules are not stylistic -- a decoder is
 * allowed to copy in 8-byte gulps, so an encoder that emits a match too close
 * to the end produces a stream that a conforming fast decoder reads past the
 * end of. The last 5 bytes must be literals, and no match may start within 12
 * bytes of the end.
 *
 * DECODING IS AN UNTRUSTED SURFACE. Every length is accumulated in a size_t
 * with an overflow check, every offset is validated against the bytes written
 * so far, and the overlapping copy respects overlap BECAUSE overlap is legal
 * and load-bearing: `memcpy` with offset 1 would replicate garbage instead of
 * the run the format specifies.
 */

/* Worst-case LZ4 block expansion: one token per 255 literals, plus the token
 * and length bytes of the final sequence. */
#define DYN_LZ4_BOUND(n) ((n) + (n) / 255 + 16)

/* Compress src into o as one LZ4 block.
 *
 * The output is reserved to the format's worst case ONCE, so the emit path is
 * pointer writes with no capacity test at all -- it previously called an
 * ensure-checked put() per token byte and per length byte.
 *
 * `chain` is the match-finder effort: 1 is the classic single-slot LZ4 fast
 * table (one candidate per hash, overwritten), anything larger walks a position
 * chain -- which is what LZ4HC is, in the only sense that matters to the
 * output: a better parse in the same format, decoded by the same decoder.
 *
 * `dict`/`dict_len` seed the window: the dictionary is treated as the bytes
 * immediately preceding src, so a match may reach back into it. That is LZ4's
 * prefix mode, and it is why the decoder needs the identical dictionary. */
static int dyn_lz4_block_compress(const uint8_t *src, size_t len,
                                  const uint8_t *dict, size_t dict_len,
                                  int chain, dyn_comp_ctx_t *cx, dyn_outbuf_t *o)
{
    dyn_scratch_t sc;
    int32_t *head;
    uint16_t *prev;
    uint32_t base;
    int shift, use_chain;
    size_t i, anchor, total;
    const uint8_t *win;
    uint8_t *op;
    uint8_t *tmp = NULL;
    int rc = -1;

    if (dict_len > DYN_LZ4_MAX_DIST) {       /* only the tail is reachable */
        dict = dict + (dict_len - DYN_LZ4_MAX_DIST);
        dict_len = DYN_LZ4_MAX_DIST;
    }
    total = dict_len + len;
    if (total > (size_t)INT32_MAX)
        return -1;

    /* One contiguous window: the dictionary followed by the input, so a match
     * that starts in the dictionary and runs into the input is found by the
     * same loop rather than a second special case. */
    if (dict_len) {
        tmp = (uint8_t *)malloc(total ? total : 1);
        if (!tmp)
            return -1;
        memcpy(tmp, dict, dict_len);
        memcpy(tmp + dict_len, src, len);
        win = tmp;
    } else {
        win = src;
    }

    if (dyn_ob_ensure(o, DYN_LZ4_BOUND(len)))
        goto done_nofree;
    if (dyn_scratch_get(cx, DYN_LZ4_HASH_BITS, total, &sc, chain > 1))
        goto done_nofree;
    head = sc.head;
    prev = sc.prev;
    base = sc.base;
    shift = sc.shift;
    use_chain = chain > 1;

    op = o->buf + o->len;

    /* Index the dictionary so its positions are candidates, without emitting
     * anything for them. */
    if (dict_len) {
        for (i = 0; i + DYN_LZ4_MIN_MATCH <= dict_len; i++) {
            uint32_t h = dyn_lz4_hash_at(win + i, shift);
            int32_t pv = head[h];
            if (use_chain) {
                size_t g = (pv >= (int32_t)base)
                             ? i - (size_t)(pv - (int32_t)base) : 0;
                prev[i & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
            }
            head[h] = (int32_t)(i + base);
        }
    }

    i = dict_len;
    anchor = dict_len;
    while (i + DYN_LZ4_MF_LIMIT <= total) {
        size_t best_len = DYN_LZ4_MIN_MATCH - 1, best_pos = 0;
        uint32_t h = dyn_lz4_hash_at(win + i, shift);
        int32_t hv = head[h];
        size_t max_len = total - i - DYN_LZ4_LAST_LITERALS;

        if (hv >= (int32_t)base) {
            const uint8_t *a = win + i;
            size_t cur = (size_t)(hv - (int32_t)base);
            size_t dist = i - cur;
            int walk = chain;
            while (dist <= DYN_LZ4_MAX_DIST && dist != 0) {
                const uint8_t *b = win + cur;
                uint16_t g = use_chain ? prev[cur & DYN_PREV_MASK] : 0;
                if (b[best_len] == a[best_len] &&
                    b[best_len - 1] == a[best_len - 1]) {
                    size_t ml = dyn_match_len(a, b, max_len);
                    if (ml > best_len) {
                        best_len = ml;
                        best_pos = cur;
                        if (ml >= max_len)
                            break;
                    }
                }
                if (!use_chain || g == 0 || --walk <= 0)
                    break;
                cur -= g;
                dist += g;
            }
            if (use_chain) {
                size_t g = i - (size_t)(hv - (int32_t)base);
                prev[i & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
            }
        } else if (use_chain) {
            prev[i & DYN_PREV_MASK] = 0;
        }
        head[h] = (int32_t)(i + base);

        if (best_len >= DYN_LZ4_MIN_MATCH) {
            size_t k, end = i + best_len;
            size_t lit_len = i - anchor;
            size_t ml = best_len - DYN_LZ4_MIN_MATCH;
            unsigned offset = (unsigned)(i - best_pos);

            *op++ = (uint8_t)(((lit_len >= 15 ? 15u : (unsigned)lit_len) << 4) |
                              (ml >= 15 ? 15u : (unsigned)ml));
            if (lit_len >= 15) {
                size_t r = lit_len - 15;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (uint8_t)r;
            }
            if (lit_len) {
                memcpy(op, win + anchor, lit_len);
                op += lit_len;
            }
            *op++ = (uint8_t)(offset & 0xff);
            *op++ = (uint8_t)(offset >> 8);
            if (ml >= 15) {
                size_t r = ml - 15;
                while (r >= 255) { *op++ = 255; r -= 255; }
                *op++ = (uint8_t)r;
            }

            /* Insert every position the match covered. On compressible input
               this loop runs for nearly every byte of the file, so its body is
               the encode cost rather than the match search: level 12 measured
               1.8x slower than the absolute-position version purely here.
               Split on `use_chain` (loop-invariant) so the level-1 arm carries
               no test and no chain write at all, and compute the gap with one
               subtraction instead of two. Byte-for-byte the same stream. */
            if (use_chain) {
                int32_t bs = (int32_t)base;
                for (k = i + 1; k < end && k + DYN_LZ4_MIN_MATCH <= total; k++) {
                    uint32_t hh = dyn_lz4_hash_at(win + k, shift);
                    int32_t pv = head[hh];
                    int32_t cur = (int32_t)(k + base);
                    uint32_t g = (uint32_t)(cur - pv);
                    prev[k & DYN_PREV_MASK] =
                        (uint16_t)((pv >= bs && g <= 65535) ? g : 0);
                    head[hh] = cur;
                }
            } else {
                for (k = i + 1; k < end && k + DYN_LZ4_MIN_MATCH <= total; k++)
                    head[dyn_lz4_hash_at(win + k, shift)] = (int32_t)(k + base);
            }
            i = end;
            anchor = end;
        } else {
            i++;
        }
    }
    /* The final literals-only sequence. */
    {
        size_t lit_len = total - anchor;
        *op++ = (uint8_t)((lit_len >= 15 ? 15u : (unsigned)lit_len) << 4);
        if (lit_len >= 15) {
            size_t r = lit_len - 15;
            while (r >= 255) { *op++ = 255; r -= 255; }
            *op++ = (uint8_t)r;
        }
        if (lit_len) {
            memcpy(op, win + anchor, lit_len);
            op += lit_len;
        }
    }
    o->len = (size_t)(op - o->buf);
    rc = 0;
    dyn_scratch_put(&sc);
done_nofree:
    free(tmp);
    return rc;
}

/* Decompress one LZ4 block. `dict` is the same window prefix the encoder used;
 * a match may reach back into it. Returns 0, or -1 on any malformed input.
 *
 * Every bounds check of the original is here. What changed is that the literal
 * run is a memcpy instead of a byte loop and the match copy uses the 8-byte
 * granular overlapping copy -- both under an output reservation that carries
 * enough slack to absorb their overshoot. The dictionary-straddling case keeps
 * the byte-at-a-time path, because there the run is spliced from two buffers. */
static int dyn_lz4_block_decompress(const uint8_t *src, size_t len,
                                    const uint8_t *dict, size_t dict_len,
                                    dyn_outbuf_t *o)
{
    size_t p = 0, produced = 0;
    size_t start = o->len;

    /* LZ4 is typically 2-3x; reserving up front removes most of the doubling. */
    if (len && dyn_ob_reserve(o, o->len + len * 3 + DYN_OSLACK) < 0) {
        if (dyn_ob_ensure(o, len))
            return -1;
    }

    while (p < len) {
        uint8_t token = src[p++];
        size_t lit = token >> 4, ml = token & 15, off, k;

        if (DYN_UNLIKELY(lit == 15)) {
            for (;;) {
                if (p >= len)
                    return -1;
                if (lit > SIZE_MAX - 255)
                    return -1;
                lit += src[p];
                if (src[p++] != 255)
                    break;
            }
        }
        if (lit > len - p)
            return -1;
        /* +8 of slack so the literal copy and the match copy that follows may
         * write in 8-byte units without passing the reservation. */
        if (dyn_ob_ensure(o, lit + 8))
            return -1;
        if (lit) {
            memcpy(o->buf + o->len, src + p, lit);
            o->len += lit;
            produced += lit;
            p += lit;
        }
        if (p == len)
            break;                       /* the final literals-only sequence */
        if (p + 2 > len)
            return -1;
        off = (size_t)src[p] | ((size_t)src[p + 1] << 8);
        p += 2;
        if (off == 0 || off > produced + dict_len)
            return -1;                   /* would read before the window */
        if (DYN_UNLIKELY(ml == 15)) {
            for (;;) {
                if (p >= len)
                    return -1;
                if (ml > SIZE_MAX - 255)
                    return -1;
                ml += src[p];
                if (src[p++] != 255)
                    break;
            }
        }
        ml += DYN_LZ4_MIN_MATCH;
        if (dyn_ob_ensure(o, ml + 8))
            return -1;
        if (DYN_LIKELY(off <= produced)) {
            /* Entirely inside what this block has already written. */
            dyn_copy_match(o->buf + o->len, off, ml);
            o->len += ml;
        } else {
            for (k = 0; k < ml; k++) {
                size_t back = produced + k;
                uint8_t b;
                if (off > back) {
                    size_t d = off - back;   /* still inside the dictionary */
                    b = dict[dict_len - d];
                } else {
                    b = o->buf[start + back - off];
                }
                o->buf[o->len++] = b;
            }
        }
        produced += ml;
    }
    return 0;
}

/* ---------- LZ4 frame format, magic 0x184D2204 ----------------------------
 *
 * Verified against the system `lz4` tool in BOTH directions, which is the only
 * check that means anything for an interop format -- a self round-trip agrees
 * with its own bugs.
 *
 * Descriptor: FLG, BD, then the optional 8-byte content size, then a header
 * checksum byte = (XXH32(descriptor, 0) >> 8) & 0xFF. Blocks follow as a
 * 4-byte little-endian size (high bit set = the block is stored uncompressed),
 * then that many bytes; a zero size ends the frame. An optional 4-byte XXH32
 * of the ORIGINAL content follows the end mark.
 *
 * FLG bit 5 (block independence) is set: each block is compressed against
 * itself, so a decoder needs no window from the previous one. Linked blocks
 * compress slightly better and make every block depend on its predecessor,
 * which is the wrong trade for a library whose blocks are whole payloads.
 */

#define DYN_LZ4F_MAGIC     0x184D2204u
#define DYN_LZ4F_BLOCK_MAX 4194304u     /* BD=7: 4 MiB, the largest LZ4 defines */

DYN_INLINE void dyn_put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

DYN_INLINE uint32_t dyn_get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int dyn_lz4_chain_for(int level)
{
    return level <= 1 ? 1 : (level >= 12 ? 4096 : level * 16);
}

int dyn_lz4_compress(const uint8_t *src, size_t len,
                     const uint8_t *dict, size_t dict_len, int level,
                     dyn_comp_ctx_t *cx, uint8_t **pout, size_t *pout_len)
{
    dyn_outbuf_t o = { NULL, 0, 0 };

    if (dyn_lz4_block_compress(src, len, dict, dict_len,
                               dyn_lz4_chain_for(level), cx, &o) < 0) {
        free(o.buf);
        return -1;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
}

int dyn_lz4_decompress(const uint8_t *src, size_t len,
                       const uint8_t *dict, size_t dict_len, dyn_outbuf_t *o)
{
    return dyn_lz4_block_decompress(src, len, dict, dict_len, o);
}

/* Each block is compressed DIRECTLY into the frame buffer behind its own
 * 4-byte size field, which is patched afterwards. The previous revision
 * compressed into a per-block temporary and memcpy'd it in, so a 4 MiB block
 * cost an extra 4 MiB allocation and an extra 4 MiB copy, per block. */
int dyn_lz4_frame_build(const uint8_t *src, size_t len, int level,
                        int content_checksum, dyn_comp_ctx_t *cx,
                        uint8_t **pout, size_t *pout_len)
{
    dyn_outbuf_t o = { NULL, 0, 0 };
    uint8_t desc[3];
    size_t off = 0;
    int chain = dyn_lz4_chain_for(level);

    /* FLG: version 01, block-independent, content checksum optional.
     * BD: block max size 4 MiB (7 << 4). */
    desc[0] = (uint8_t)(0x40 | 0x20 | (content_checksum ? 0x04 : 0));
    desc[1] = 0x70;
    desc[2] = (uint8_t)((dyn_xxh32(desc, 2, 0) >> 8) & 0xff);

    {   /* magic + descriptor + per-block (4-byte size + bound) + end + sum */
        size_t nblk = len ? (len + DYN_LZ4F_BLOCK_MAX - 1) / DYN_LZ4F_BLOCK_MAX : 1;
        size_t want = 7 + nblk * 4 + DYN_LZ4_BOUND(len) + 8;
        if (dyn_ob_reserve(&o, want) < 0 && dyn_ob_ensure(&o, 7) < 0)
            goto fail;
    }
    if (dyn_ob_ensure(&o, 7))
        goto fail;
    dyn_put32le(o.buf + o.len, DYN_LZ4F_MAGIC);
    o.len += 4;
    memcpy(o.buf + o.len, desc, 3);
    o.len += 3;

    while (off < len || len == 0) {
        size_t chunk = len - off;
        size_t hdr, blk_len;

        if (chunk > DYN_LZ4F_BLOCK_MAX)
            chunk = DYN_LZ4F_BLOCK_MAX;
        hdr = o.len;
        if (dyn_ob_ensure(&o, 4))
            goto fail;
        o.len += 4;
        if (dyn_lz4_block_compress(src + off, chunk, NULL, 0, chain, cx, &o) < 0)
            goto fail;
        blk_len = o.len - hdr - 4;

        if (blk_len >= chunk) {
            /* An incompressible block is written STORED with the high bit set,
             * so the frame never expands by more than its own headers. Rewind
             * over the compressed attempt and lay the raw bytes down instead. */
            o.len = hdr + 4;
            if (dyn_ob_ensure(&o, chunk))
                goto fail;
            memcpy(o.buf + o.len, src + off, chunk);
            o.len += chunk;
            dyn_put32le(o.buf + hdr, (uint32_t)chunk | 0x80000000u);
        } else {
            dyn_put32le(o.buf + hdr, (uint32_t)blk_len);
        }
        off += chunk;
        if (len == 0)
            break;
    }

    if (dyn_ob_ensure(&o, 4))
        goto fail;
    dyn_put32le(o.buf + o.len, 0);          /* end mark */
    o.len += 4;
    if (content_checksum) {
        if (dyn_ob_ensure(&o, 4))
            goto fail;
        dyn_put32le(o.buf + o.len, dyn_xxh32(src, len, 0));
        o.len += 4;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
fail:
    free(o.buf);
    return -1;
}

int dyn_lz4_frame_decode(const uint8_t *src, size_t len, dyn_outbuf_t *o)
{
    size_t p = 0;
    uint8_t flg, bd;
    uint32_t max_block;
    int has_checksum, has_content_size, has_dict_id, block_checksum;

    if (len < 7 || dyn_get32le(src) != DYN_LZ4F_MAGIC)
        return -1;
    p = 4;
    flg = src[p++];
    bd = src[p++];
    /* BD bits 4-6 declare the maximum block size (4=64K,5=256K,6=1M,7=4M).
       This was read and thrown away with (void)bd, so the frame's own declared
       cap was never enforced -- DYN_LZ4F_BLOCK_MAX existed but was used only
       on the ENCODE side, which reads as a verified defence and was not one. */
    {
        unsigned bdbits = (bd >> 4) & 7u;
        if (bdbits < 4 || bdbits > 7)
            return -1;                      /* reserved / undefined size */
        max_block = 1u << (2 * bdbits + 8); /* 4->64K 5->256K 6->1M 7->4M */
        if (max_block > DYN_LZ4F_BLOCK_MAX)
            return -1;
    }
    if ((flg & 0xc0) != 0x40)               /* version bits must be 01 */
        return -1;
    /* Bit 5 clear means LINKED blocks: block N's matches may reach into block
     * N-1's output. Decoding one of those as if it were independent produces
     * plausible garbage rather than an error, so refuse it. Supporting it would
     * mean holding a window pointer into an output buffer that reallocates
     * mid-block -- a dangling read, not a feature. */
    if (!(flg & 0x20))
        return -1;
    has_content_size = (flg & 0x08) != 0;
    has_checksum = (flg & 0x04) != 0;
    has_dict_id = (flg & 0x01) != 0;
    block_checksum = (flg & 0x10) != 0;
    if (has_content_size) {
        if (len - p < 8)
            return -1;
        p += 8;
    }
    if (has_dict_id) {
        if (len - p < 4)
            return -1;
        p += 4;
    }
    if (p >= len)
        return -1;
    /* The descriptor checksum covers FLG..(dictID), i.e. everything after the
     * magic and before the byte itself. */
    if (src[p] != (uint8_t)((dyn_xxh32(src + 4, p - 4, 0) >> 8) & 0xff))
        return -1;
    p++;

    for (;;) {
        uint32_t bs;
        size_t bl;
        if (len - p < 4)
            return -1;
        bs = dyn_get32le(src + p);
        p += 4;
        if (bs == 0)
            break;                          /* end mark */
        bl = (size_t)(bs & 0x7fffffffu);
        if (bl > len - p)
            return -1;
        if (bl > max_block)                 /* the frame's OWN declared cap */
            return -1;
        if (bs & 0x80000000u) {
            if (dyn_ob_ensure(o, bl))
                return -1;
            memcpy(o->buf + o->len, src + p, bl);
            o->len += bl;
        } else if (dyn_lz4_block_decompress(src + p, bl, NULL, 0, o) < 0) {
            return -1;
        }
        p += bl;
        if (block_checksum) {
            if (len - p < 4)
                return -1;
            p += 4;                          /* validated by the content sum */
        }
    }
    if (has_checksum) {
        if (len - p < 4)
            return -1;
        if (dyn_get32le(src + p) != dyn_xxh32(o->buf, o->len, 0))
            return -1;
    }
    return 0;
}

/* The context-free forms: identical output, one scratch pair per call. */
int dyn_gzip_build(const uint8_t *src, size_t src_len,
                   uint8_t **pout, size_t *pout_len)
{
    return dyn_gzip_build_ctx(src, src_len, NULL, pout, pout_len);
}
