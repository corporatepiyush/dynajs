#include "dyn-scram.h"
#include "dyna-simd-kernels.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTCAP 1024

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    dyn_scram_t s;
    char out[OUTCAP];
    uint8_t* exact;
    int rc;

    simd_init();

    if (size > 65536)
        return 0;

    exact = (uint8_t*)malloc(size ? size : 1);
    if (!exact)
        return 0;
    memcpy(exact, data, size);

    memset(&s, 0, sizeof s);
    if (dyn_scram_client_first(&s, out, sizeof out) > 0) {
        rc = dyn_scram_server_first(&s, (const char*)exact, size,
            "hunter2", out, sizeof out);
        if (rc > 0)
            dyn_scram_server_final(&s, (const char*)exact, size);
    }
    dyn_scram_free(&s);

    memset(&s, 0, sizeof s);
    if (dyn_scram_client_first(&s, out, sizeof out) > 0) {
        size_t cap = size * 2 + sizeof s.nonce + 128;
        char* msg = (char*)malloc(cap);
        if (msg) {
            size_t n = 0, i;
            n += (size_t)snprintf(msg + n, cap - n, "r=%s", s.nonce);
            for (i = 0; i < size && n + 1 < cap; i++) {
                char c = (char)exact[i];
                if (c == ',' || c == '\0')
                    c = 'x';
                msg[n++] = c;
            }
            n += (size_t)snprintf(msg + n, cap - n, ",s=");
            for (i = 0; i < size && n + 1 < cap; i++)
                msg[n++] = (char)exact[i];
            n += (size_t)snprintf(msg + n, cap - n, ",i=4096");
            rc = dyn_scram_server_first(&s, msg, n, "hunter2", out, sizeof out);
            if (rc > 0)
                dyn_scram_server_final(&s, msg, n);
            free(msg);
        }
    }
    dyn_scram_free(&s);

    free(exact);
    return 0;
}
