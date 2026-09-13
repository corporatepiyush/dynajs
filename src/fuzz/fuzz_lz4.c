// Fuzz target for the LZ4 decoders (src/core/dyn-compress.c).
//
// Both LZ4 decode paths are untrusted surfaces, and they fail differently:
//
//   - the RAW BLOCK has no header, no length and no checksum, so essentially
//     every input reaches the sequence loop. There is nothing to reject on;
//     the only defence is that each literal run is bounded by the remaining
//     input and each match offset is bounded by what has actually been
//     written. That makes this the target where a fuzzer earns its keep.
//   - the FRAME has a magic, a descriptor checksum and per-block lengths, so
//     random bytes are rejected at the gate. To get past it the input is also
//     driven through a REPAIRED frame: a real header wrapped around the
//     fuzzer's bytes as one block payload. Without the repair the target would
//     measure the magic check and nothing else -- the same trap that made the
//     first DYNS sweep report 8811/8811 rejected and prove nothing
//     (STDLIB_OOP_PLAN, W7.3).
//
// A dictionary is supplied on some iterations, because the pre-window branch of
// the match copy (`off > back`) is unreachable without one and is exactly where
// an off-by-one reads before the buffer.
//
// Build with -fsanitize=address,undefined -- the checked-in libfuzzer objects
// are fuzzer-no-link and miss heap-OOB (CLAUDE.md section 7):
//     make fuzz_lz4

#include "dyn-compress.h"
#include "dyn-hash.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t dict[64] = {
        'j','s','o','n','r','p','c','2','.','0','m','e','t','h','o','d',
        'p','a','r','a','m','s','i','d','r','e','s','u','l','t','e','r',
        'r','o','r','c','o','d','e','m','e','s','s','a','g','e','d','a',
        't','a','t','r','u','e','f','a','l','s','e','n','u','l','l','!'
    };
    dyn_outbuf_t o;
    uint8_t *frame;
    size_t flen;

    if (size > (1u << 20))
        return 0;

    /* 1. raw block, no dictionary */
    o.buf = NULL; o.len = 0; o.cap = 0;
    (void)dyn_lz4_decompress(data, size, NULL, 0, &o);
    free(o.buf);

    /* 2. raw block WITH a dictionary: reaches the pre-window copy branch */
    o.buf = NULL; o.len = 0; o.cap = 0;
    (void)dyn_lz4_decompress(data, size, dict, sizeof(dict), &o);
    free(o.buf);

    /* 3. the frame as given -- almost always rejected at the magic, which is
     *    itself worth exercising */
    o.buf = NULL; o.len = 0; o.cap = 0;
    (void)dyn_lz4_frame_decode(data, size, &o);
    free(o.buf);

    /* 4. a REPAIRED frame: valid magic and descriptor, the fuzzer's bytes as a
     *    single compressed block. This is the one that gets past the gate. */
    if (size >= 1 && size < (1u << 20) - 32) {
        uint8_t desc[2];
        flen = 4 + 3 + 4 + size + 4;
        frame = (uint8_t *)malloc(flen);
        if (!frame)
            return 0;
        put32le(frame, 0x184D2204u);
        desc[0] = 0x60;          /* version 01, block independence, no checksum */
        desc[1] = 0x70;          /* 4 MiB blocks */
        frame[4] = desc[0];
        frame[5] = desc[1];
        frame[6] = (uint8_t)((dyn_xxh32(desc, 2, 0) >> 8) & 0xff);
        put32le(frame + 7, (uint32_t)size);
        memcpy(frame + 11, data, size);
        put32le(frame + 11 + size, 0);     /* end mark */
        o.buf = NULL; o.len = 0; o.cap = 0;
        (void)dyn_lz4_frame_decode(frame, flen, &o);
        free(o.buf);
        free(frame);
    }

    /* 5. round trip: whatever the input is, compressing it and decompressing
     *    the result must reproduce it exactly. A decoder that is merely
     *    memory-safe can still be wrong, and this is the assertion that says
     *    so. Levels 1 and 9 take different match-finder paths. */
    {
        int level;
        for (level = 1; level <= 9; level += 8) {
            uint8_t *packed = NULL;
            size_t plen = 0;
            if (dyn_lz4_compress(data, size, NULL, 0, level, NULL, &packed, &plen))
                continue;
            o.buf = NULL; o.len = 0; o.cap = 0;
            if (dyn_lz4_decompress(packed, plen, NULL, 0, &o) != 0)
                abort();                       /* our own output must decode */
            if (o.len != size || (size && memcmp(o.buf, data, size) != 0))
                abort();                       /* and must be identical */
            free(o.buf);
            free(packed);
        }
    }
    return 0;
}
