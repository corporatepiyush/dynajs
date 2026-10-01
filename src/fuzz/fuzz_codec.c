#include "dyn-codec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void simd_init(void);

static int inited;

static void decode_exact(const char* s, size_t n, dyn_base32_alphabet alpha)
{
    size_t cap = dyn_codec_base32_decode_cap(n);
    uint8_t* out = (uint8_t*)malloc(cap ? cap : 1);
    size_t got;

    if (!out)
        return;
    got = dyn_codec_base32_decode(s, n, out, alpha);
    if (got != DYN_CODEC_BAD && got > cap)
        abort();
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    int a;

    if (!inited) {
        simd_init();
        inited = 1;
    }
    if (size > 65536)
        return 0;

    decode_exact((const char*)data, size, DYN_BASE32_STD);
    decode_exact((const char*)data, size, DYN_BASE32_HEX);

    {
        size_t n8 = size & ~(size_t)7;
        if (n8) {
            decode_exact((const char*)data, n8, DYN_BASE32_STD);
            decode_exact((const char*)data, n8, DYN_BASE32_HEX);
        }
    }

    for (a = 0; a < 2; a++) {
        dyn_base32_alphabet alpha = a ? DYN_BASE32_HEX : DYN_BASE32_STD;
        size_t ecap = dyn_codec_base32_encode_cap(size);
        char* enc = (char*)malloc(ecap ? ecap : 1);
        uint8_t* dec;
        size_t elen, dcap, dlen;

        if (!enc)
            return 0;
        elen = dyn_codec_base32_encode(data, size, enc, alpha);
        if (elen > ecap)
            abort();
        dcap = dyn_codec_base32_decode_cap(elen);
        dec = (uint8_t*)malloc(dcap ? dcap : 1);
        if (!dec) {
            free(enc);
            return 0;
        }
        dlen = dyn_codec_base32_decode(enc, elen, dec, alpha);
        if (dlen == DYN_CODEC_BAD)
            abort();
        if (dlen != size || (size && memcmp(dec, data, size) != 0))
            abort();
        free(dec);
        free(enc);
    }
    return 0;
}
