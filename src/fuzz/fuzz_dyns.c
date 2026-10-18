#include "dyn-serial.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drive(dyn_de_t* r, const uint8_t* script, size_t nscript)
{
    size_t i;
    for (i = 0; i < nscript && i < 4096; i++) {
        switch (script[i] % 11) {
        case 0:
            (void)dyn_de_u8(r);
            break;
        case 1:
            (void)dyn_de_u16(r);
            break;
        case 2:
            (void)dyn_de_u32(r);
            break;
        case 3:
            (void)dyn_de_u64(r);
            break;
        case 4:
            (void)dyn_de_f64(r);
            break;
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
        if (!dyn_de_ok(r)) {
            (void)dyn_de_u64(r);
            if (dyn_de_ok(r))
                abort();
            break;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    dyn_de_t r;
    uint16_t tid = 0;
    uint32_t flags = 0;
    dyn_ser_t w;

    if (size > (1u << 20))
        return 0;

    if (dyn_de_open(&r, data, size, &tid, &flags, 0) == DYN_DE_OK)
        drive(&r, data, size);

    dyn_ser_init(&w);
    if (dyn_ser_begin(&w, size ? data[0] : 0, 0) == 0 && dyn_ser_raw(&w, data, size) == 0 && dyn_ser_finish(&w) == 0) {
        if (dyn_de_open(&r, w.buf, w.len, &tid, &flags, 0) != DYN_DE_OK)
            abort();
        drive(&r, data, size);
    }
    dyn_ser_free(&w);
    return 0;
}
