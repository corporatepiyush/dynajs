#include "modules/webrtc/webrtc-internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static long fz_tag;
volatile uint64_t fz_dtls_sink;
static wrt_cert* fz_cert;

void wrt_pc_send_dtls(wrt_pc* pc, const uint8_t* p, size_t n)
{
    (void)pc;
    fz_dtls_sink += n + (n ? p[0] : 0);
}

void wrt_pc_dtls_data(wrt_pc* pc, const uint8_t* p, size_t n)
{
    (void)pc;
    fz_dtls_sink += n + (n ? p[n - 1] : 0);
}

void wrt_pc_dtls_state(wrt_pc* pc)
{
    (void)pc;
    fz_dtls_sink++;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    wrt_dtls d;
    const char* err = NULL;
    if (size < 1 || size > 65536)
        return 0;
    if (!fz_cert)
        fz_cert = wrt_cert_generate(0, 0, 86400000.0, &err);
    if (!fz_cert)
        return 0;
    memset(&d, 0, sizeof(d));
    if (wrt_dtls_init(&d, (wrt_pc*)(void*)&fz_tag, fz_cert, data[0] & 1) < 0)
        return 0;
    d.remote_fp_alg = RTC_FP_SHA256;
    d.remote_fp_len = 32;
    wrt_dtls_start(&d);
    data++;
    size--;
    while (size >= 2) {
        size_t l = ((size_t)data[0] << 8 | data[1]) % 1600;
        data += 2;
        size -= 2;
        if (l > size)
            l = size;
        wrt_dtls_input(&d, data, l);
        if (l && (data[0] & 0x40))
            wrt_dtls_timeout(&d);
        wrt_dtls_write(&d, data, l);
        data += l;
        size -= l;
    }
    wrt_dtls_deadline(&d, 0);
    wrt_dtls_close(&d);
    wrt_dtls_free(&d);
    return 0;
}
