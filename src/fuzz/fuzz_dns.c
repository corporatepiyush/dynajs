#include "../../tests/lib/dyn-dns-model.h"

#include <stdio.h>
#include <stdlib.h>

static dm_msg_t g_m;

static void fz_fail(const char* e)
{
    if (e) {
        fprintf(stderr, "oracle: %s\n", e);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    dm_src_t s = { data + 1, size ? size - 1 : 0, NULL };
    if (size < 1 || size > DM_CAP)
        return 0;
    switch (data[0] % 3) {
    case 0:
        fz_fail(dm_check_raw(data + 1, size - 1, &s));
        break;
    case 1: {
        size_t cut;
        dm_gen(&s, &g_m);
        fz_fail(dm_check_gen(&g_m));
        if (g_m.full)
            break;
        dm_mutate(&s, g_m.buf, g_m.len);
        cut = dm_pick(&s, 3) ? g_m.len : dm_pick(&s, (uint32_t)g_m.len + 1);
        fz_fail(dm_check_raw(g_m.buf, cut, &s));
        break;
    }
    default:
        fz_fail(dm_check_enc(&s));
        break;
    }
    return 0;
}
