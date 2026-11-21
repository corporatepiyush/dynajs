#include "../../tests/lib/dyn-pgwire-model.h"

static pm_stream_t g_s;

static void fz_fail(const char* e)
{
    if (e) {
        fprintf(stderr, "oracle: %s\n", e);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    pm_src_t s = { data + 1, size ? size - 1 : 0, NULL };
    if (size < 1 || size > PM_CAP)
        return 0;
    switch (data[0] % 11) {
    case 0:
        fz_fail(pm_check_raw(data + 1, size - 1));
        if (size > 3)
            fz_fail(pm_check_raw(data + 3, size - 3));
        break;
    case 1:
        pm_gen(&s, &g_s);
        fz_fail(pm_check_stream(&g_s, &s));
        break;
    case 2: {
        size_t n, i;
        pm_gen(&s, &g_s);
        if (g_s.full || !g_s.len)
            break;
        n = g_s.len;
        for (i = pm_pick(&s, 4); i; i--)
            g_s.buf[pm_pick(&s, (uint32_t)n)] = (uint8_t)pm_pick(&s, 256);
        for (i = 0; i < g_s.nmsg; i++)
            fz_fail(pm_check_raw(g_s.buf + g_s.msg[i].off, n - g_s.msg[i].off));
        fz_fail(pm_check_raw(g_s.buf, n));
        break;
    }
    case 4:
        fz_fail(pm_check_copyresp(data + 1, size - 1));
        fz_fail(pm_check_copygen(&s));
        break;
    case 5:
        fz_fail(pm_check_npvkey(data + 1, size - 1));
        fz_fail(pm_check_npvgen(&s));
        break;
    case 6:
        fz_fail(pm_check_numraw(data + 1, size - 1));
        fz_fail(pm_check_numgen(&s));
        break;
    case 7:
        fz_fail(pm_check_timeraw(data + 1, size - 1));
        fz_fail(pm_check_tsgen(&s));
        fz_fail(pm_check_ivgen(&s));
        break;
    case 8:
        fz_fail(pm_check_arrtext_raw(data + 1, size - 1));
        fz_fail(pm_check_arrbin_raw(data + 1, size - 1));
        fz_fail(pm_check_arrtext_gen(&s));
        fz_fail(pm_check_arrbin_gen(&s));
        break;
    case 9:
        fz_fail(pm_check_rngtext_raw(data + 1, size - 1));
        fz_fail(pm_check_rngbin_raw(data + 1, size - 1));
        fz_fail(pm_check_rng_gen(&s));
        break;
    case 10:
        fz_fail(pm_check_uri_raw(data + 1, size - 1));
        fz_fail(pm_check_pgpass_raw(data + 1, size - 1));
        fz_fail(pm_check_uri_gen(&s));
        fz_fail(pm_check_pgpass_gen(&s));
        break;
    default:
        fz_fail(pm_check_values(&s));
        break;
    }
    return 0;
}
