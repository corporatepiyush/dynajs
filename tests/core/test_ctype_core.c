#include <limits.h>
#include <stdio.h>

#include "dyn-ctype.h"

static int bad;

static int ref_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int ref_ws_xml(unsigned c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int ref_ws_html(unsigned c)
{
    return ref_ws_xml(c) || c == '\f';
}

static int ref_ws_c(unsigned c)
{
    return ref_ws_html(c) || c == '\v';
}

static int ref_ws_date(unsigned c)
{
    return c == ' ' || c == '\t' || c == ',' || c == '.';
}

static void check_hex(int c)
{
    int got = dyn_hex_digit(c), want = ref_hex(c);
    if (got != want) {
        printf("FAIL: dyn_hex_digit(%d) = %d, want %d\n", c, got, want);
        bad++;
    }
}

static void check_set(const char* name, uint64_t set, int (*ref)(unsigned), unsigned c)
{
    int got = dyn_in_set64(c, set), want = ref(c);
    if (got != want) {
        printf("FAIL: dyn_in_set64(%u, %s) = %d, want %d\n", c, name, got, want);
        bad++;
    }
}

int main(void)
{
    static const unsigned far[] = { 256u, 300u, 511u, 576u, 1000u, 0x10000u, 0x7fffffffu, 0xffffffffu };
    static const int farhex[] = { INT_MIN, -1000, -256, -129, -128, -1, 256, 304, 358, 0x110000, INT_MAX };
    struct {
        const char* name;
        uint64_t set;
        int (*ref)(unsigned);
    } sets[] = {
        { "DYN_WS_XML", DYN_WS_XML, ref_ws_xml },
        { "DYN_WS_HTML", DYN_WS_HTML, ref_ws_html },
        { "DYN_WS_C", DYN_WS_C, ref_ws_c },
        { "DYN_WS_DATE", DYN_WS_DATE, ref_ws_date },
    };
    unsigned c, k;
    int i;
    for (i = -300; i <= 600; i++)
        check_hex(i);
    for (k = 0; k < sizeof(farhex) / sizeof(farhex[0]); k++)
        check_hex(farhex[k]);
    for (k = 0; k < sizeof(sets) / sizeof(sets[0]); k++) {
        for (c = 0; c < 256u; c++)
            check_set(sets[k].name, sets[k].set, sets[k].ref, c);
        for (c = 0; c < sizeof(far) / sizeof(far[0]); c++)
            check_set(sets[k].name, sets[k].set, sets[k].ref, far[c]);
        check_set(sets[k].name, sets[k].set, sets[k].ref, (unsigned)-1);
    }
    if (bad) {
        printf("test_ctype_core: %d mismatch(es)\n", bad);
        return 1;
    }
    printf("test_ctype_core: ok (hex -300..600 + extremes, 4 whitespace sets x 256 bytes + out-of-range)\n");
    return 0;
}
