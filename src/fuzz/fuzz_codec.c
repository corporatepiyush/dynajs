// Fuzz target for the base32 codec (src/core/dyn-codec.c).
//
// b32_decode_tab is pure length arithmetic -- a block loop, a padding scan and
// a table-driven byte count -- the shape where an off-by-one yields a plausible
// wrong answer rather than a crash. So this is a VALUE oracle as much as a
// memory-safety one: encode(x) must decode back to x byte-for-byte.
//
// simd_init() FIRST. dyn-codec.c dispatches through a function-pointer table
// the engine fills at module load; a standalone target never does, so the first
// call goes through a NULL slot and segfaults before a byte of input is read --
// which reads as "crashes on every input", not as a missing initialiser. See
// fuzz_scram.c:46-50, where exactly this was hit for real.
//
// INPUT: raw pointer, NO +1 NUL slack. Both entry points take an explicit
// length, unlike JS_Eval/JS_ParseJSON in fuzz_eval/fuzz_json which require
// buf[len] == '\0'. libFuzzer's buffer is exactly `size` bytes, so a read one
// past the end is a reported heap overflow. Every OUTPUT buffer is an exact
// malloc of the documented cap, for the same reason.

#include "dyn-codec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void simd_init(void);

static int inited;

/* Decode `n` bytes under one alphabet into an exactly-sized buffer. */
static void decode_exact(const char *s, size_t n, dyn_base32_alphabet alpha)
{
    size_t cap = dyn_codec_base32_decode_cap(n);
    uint8_t *out = (uint8_t *)malloc(cap ? cap : 1);
    size_t got;

    if (!out)
        return;
    got = dyn_codec_base32_decode(s, n, out, alpha);
    if (got != DYN_CODEC_BAD && got > cap)
        abort();                     /* claimed more bytes than its own cap */
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int a;

    if (!inited) { simd_init(); inited = 1; }
    if (size > 65536)
        return 0;

    /* 1. raw bytes as base32 text. Most die on the "multiple of 8" gate, which
     *    is itself the check worth exercising. */
    decode_exact((const char *)data, size, DYN_BASE32_STD);
    decode_exact((const char *)data, size, DYN_BASE32_HEX);

    /* 2. the same bytes with the length REPAIRED to a multiple of 8, so the
     *    block loop, the final-block padding scan and nbytes_tab[] are reached
     *    on nearly every input instead of one in eight. */
    {
        size_t n8 = size & ~(size_t)7;
        if (n8) {
            decode_exact((const char *)data, n8, DYN_BASE32_STD);
            decode_exact((const char *)data, n8, DYN_BASE32_HEX);
        }
    }

    /* 3. round trip, both alphabets: whatever the input is, encoding it and
     *    decoding the result must reproduce it exactly. A decoder that is
     *    merely memory-safe can still be wrong; this is what says so. */
    for (a = 0; a < 2; a++) {
        dyn_base32_alphabet alpha = a ? DYN_BASE32_HEX : DYN_BASE32_STD;
        size_t ecap = dyn_codec_base32_encode_cap(size);
        char *enc = (char *)malloc(ecap ? ecap : 1);
        uint8_t *dec;
        size_t elen, dcap, dlen;

        if (!enc)
            return 0;
        elen = dyn_codec_base32_encode(data, size, enc, alpha);
        if (elen > ecap)
            abort();                        /* encoder passed its own cap */
        dcap = dyn_codec_base32_decode_cap(elen);
        dec = (uint8_t *)malloc(dcap ? dcap : 1);
        if (!dec) { free(enc); return 0; }
        dlen = dyn_codec_base32_decode(enc, elen, dec, alpha);
        if (dlen == DYN_CODEC_BAD)
            abort();                        /* our own output must decode */
        if (dlen != size || (size && memcmp(dec, data, size) != 0))
            abort();                        /* and must be identical */
        free(dec);
        free(enc);
    }
    return 0;
}
