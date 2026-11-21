#include "../../tests/lib/dyn-httpk-model.h"

static void fz_fail(const char* e)
{
    if (e) {
        fprintf(stderr, "oracle: %s\n", e);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    hm_src_t s = { data + 1, size ? size - 1 : 0, NULL };
    if (size < 1 || size > HM_CAP)
        return 0;
    switch (data[0] % 7) {
    case 0:
        fz_fail(hm_check_head(&s, NULL, 0));
        break;
    case 1:
        fz_fail(hm_check_head(&s, data + 1, size - 1));
        break;
    case 2:
        fz_fail(hm_check_chunk(&s, NULL, 0));
        break;
    case 3:
        fz_fail(hm_check_chunk(&s, data + 1, size - 1));
        break;
    case 4:
        fz_fail(hm_check_sse(&s, NULL, 0));
        break;
    case 5:
        fz_fail(hm_check_sse(&s, data + 1, size - 1));
        break;
    default:
        fz_fail(hm_check_date(&s));
        break;
    }
    return 0;
}
