// Fuzz target for the DYNS record reader (src/core/dyn-serial.c).
//
// Every dyna:structures and dyna:ml record deserialiser sits on this cursor, so
// a bug here is a bug in all of them at once. The reader is pure C and needs no
// engine, which is why this target links only the core: it runs orders of
// magnitude more executions per second than one that spins up a JSRuntime, and
// the per-type codecs are covered end-to-end by tests/test_structures_serialize.js
// instead.
//
// Two shapes are driven per input, because they reach different code:
//
//   1. the RAW buffer, which almost always fails dyn_de_open -- that is the
//      magic/version/length/CRC gate, and it must reject rather than read;
//   2. the buffer REWRAPPED as a valid record (correct length, correct CRC),
//      which gets past the gate and drives the cursor over attacker-controlled
//      payload bytes. Without this, the fuzzer would spend all its time being
//      rejected by the checksum and would never test the thing that matters.
//
// After opening, the payload drives its own read sequence: the first byte of
// each step selects the accessor. Any read past the end must set the sticky
// error and return a zero -- never a wild pointer -- so the whole target is an
// assertion that the cursor cannot be walked out of bounds.
//
// Build with -fsanitize=address,undefined. The checked-in libfuzzer objects are
// fuzzer-no-link and miss heap-OOB (CLAUDE.md section 7):
//     make fuzz_dyns

#include "dyn-serial.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drive(dyn_de_t *r, const uint8_t *script, size_t nscript)
{
    size_t i;
    for (i = 0; i < nscript && i < 4096; i++) {
        switch (script[i] % 11) {
        case 0: (void)dyn_de_u8(r); break;
        case 1: (void)dyn_de_u16(r); break;
        case 2: (void)dyn_de_u32(r); break;
        case 3: (void)dyn_de_u64(r); break;
        case 4: (void)dyn_de_f64(r); break;
        case 5: {
            size_t n = 0;
            (void)dyn_de_blob(r, &n);
            break;
        }
        case 6: {
            uint32_t c = 0;
            (void)dyn_de_count(r, &c, 8);
            break;
        }
        case 7: {
            uint32_t c = 0;
            (void)dyn_de_count(r, &c, 0);
            break;
        }
        case 8: {
            /* The varint readers are the newest attacker-facing primitive: a
             * continuation run must terminate rather than shift past 64. */
            uint64_t v = 0;
            (void)dyn_de_uvarint(r, &v);
            break;
        }
        case 9: {
            int64_t v = 0;
            (void)dyn_de_svarint(r, &v);
            break;
        }
        default:
            (void)dyn_de_raw(r, (size_t)script[i]);
            break;
        }
        /* A sticky error must stay stuck: once set, no accessor may clear it
         * and start reading again. */
        if (!dyn_de_ok(r)) {
            (void)dyn_de_u64(r);
            if (dyn_de_ok(r))
                abort();
            break;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    dyn_de_t r;
    uint16_t tid = 0;
    uint32_t flags = 0;
    dyn_ser_t w;

    if (size > (1u << 20))
        return 0;

    /* 1. raw bytes: the gate must reject or open safely. */
    if (dyn_de_open(&r, data, size, &tid, &flags, 0) == DYN_DE_OK)
        drive(&r, data, size);

    /* 2. the same bytes wrapped in a well-formed envelope, so the payload is
     *    reached. */
    dyn_ser_init(&w);
    if (dyn_ser_begin(&w, size ? data[0] : 0, 0) == 0 &&
        dyn_ser_raw(&w, data, size) == 0 &&
        dyn_ser_finish(&w) == 0) {
        if (dyn_de_open(&r, w.buf, w.len, &tid, &flags, 0) != DYN_DE_OK)
            abort();          /* a record we just wrote must always open */
        drive(&r, data, size);
    }
    dyn_ser_free(&w);
    return 0;
}
