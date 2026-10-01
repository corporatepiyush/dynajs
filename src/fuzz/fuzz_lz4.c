#include "dyn-compress.h"
#include "dyn-hash.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void put32le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const uint8_t dict[64] = {
        'j', 's', 'o', 'n', 'r', 'p', 'c', '2', '.', '0', 'm', 'e', 't', 'h', 'o', 'd',
        'p', 'a', 'r', 'a', 'm', 's', 'i', 'd', 'r', 'e', 's', 'u', 'l', 't', 'e', 'r',
        'r', 'o', 'r', 'c', 'o', 'd', 'e', 'm', 'e', 's', 's', 'a', 'g', 'e', 'd', 'a',
        't', 'a', 't', 'r', 'u', 'e', 'f', 'a', 'l', 's', 'e', 'n', 'u', 'l', 'l', '!'
    };
    dyn_outbuf_t o;
    uint8_t* frame;
    size_t flen;

    if (size > (1u << 20))
        return 0;

    o.buf = NULL;
    o.len = 0;
    o.cap = 0;
    (void)dyn_lz4_decompress(data, size, NULL, 0, &o);
    free(o.buf);

    o.buf = NULL;
    o.len = 0;
    o.cap = 0;
    (void)dyn_lz4_decompress(data, size, dict, sizeof(dict), &o);
    free(o.buf);

    o.buf = NULL;
    o.len = 0;
    o.cap = 0;
    (void)dyn_lz4_frame_decode(data, size, &o);
    free(o.buf);

    if (size >= 1 && size < (1u << 20) - 32) {
        uint8_t desc[2];
        flen = 4 + 3 + 4 + size + 4;
        frame = (uint8_t*)malloc(flen);
        if (!frame)
            return 0;
        put32le(frame, 0x184D2204u);
        desc[0] = 0x60;
        desc[1] = 0x70;
        frame[4] = desc[0];
        frame[5] = desc[1];
        frame[6] = (uint8_t)((dyn_xxh32(desc, 2, 0) >> 8) & 0xff);
        put32le(frame + 7, (uint32_t)size);
        memcpy(frame + 11, data, size);
        put32le(frame + 11 + size, 0);
        o.buf = NULL;
        o.len = 0;
        o.cap = 0;
        (void)dyn_lz4_frame_decode(frame, flen, &o);
        free(o.buf);
        free(frame);
    }

    {
        int level;
        for (level = 1; level <= 9; level += 8) {
            uint8_t* packed = NULL;
            size_t plen = 0;
            if (dyn_lz4_compress(data, size, NULL, 0, level, NULL, &packed, &plen))
                continue;
            o.buf = NULL;
            o.len = 0;
            o.cap = 0;
            if (dyn_lz4_decompress(packed, plen, NULL, 0, &o) != 0)
                abort();
            if (o.len != size || (size && memcmp(o.buf, data, size) != 0))
                abort();
            free(o.buf);
            free(packed);
        }
    }
    return 0;
}
