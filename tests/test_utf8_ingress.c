#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dynajs.h"

static JSContext *ctx;
static JSRuntime *rt;
static unsigned long line_no;

static void dump(const char *what, const uint8_t *buf, size_t len)
{
    JSValue s, lv;
    uint32_t n, i;

    s = JS_NewStringLen(ctx, (const char *)buf, len);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        printf("%6lu %-14s len=%-5zu EXCEPTION\n", ++line_no, what, len);
        return;
    }
    lv = JS_GetPropertyStr(ctx, s, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);

    printf("%6lu %-14s in=%-5zu out=%-5u", ++line_no, what, len, n);
    for (i = 0; i < n; i++) {
        JSValue cv = JS_GetPropertyUint32(ctx, s, i);
        uint32_t c;
        JS_ToUint32(ctx, &c, cv);
        JS_FreeValue(ctx, cv);
        printf(" %04X", c);
    }
    printf("\n");
    JS_FreeValue(ctx, s);
}

static uint32_t st = 0x12345678;
static uint32_t rnd(void)
{
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

int main(void)
{
    uint8_t buf[4096];
    size_t i, n;
    int round;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    {
        static const char *units[] = {
            "a",
            "\xC3\xA9",
            "\xC2\x80",
            "\xC3\xBF",
            "\xC4\x80",
            "\xE6\x97\xA5",
            "\xE2\x9C\x93",
            "\xF0\x9F\x98\x80",
            "\xEF\xBF\xBD",
            "\xFF",
            "\xFE",
            "\x80",
            "\xBF",
            "\xC3",
            "\xE6\x97",
            "\xF0\x9F\x98",
            "\xC0\x80",
            "\xC1\xBF",
            "\xE0\x80\x80",
            "\xF0\x80\x80\x80",
            "\xED\xA0\x80",
            "\xED\xBF\xBF",
            "\xF4\x90\x80\x80",
            "\xF5\x80\x80\x80",
            "\xF8\x88\x80\x80\x80",
            "\xC3\x28",
            "\xE6\x28\xA5",
            "\x00",
        };
        size_t u;
        for (u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
            size_t ul = (units[u][0] == '\0') ? 1 : strlen(units[u]);
            size_t reps[] = { 1, 2, 20, 40, 100 };
            size_t r;
            for (r = 0; r < sizeof(reps) / sizeof(reps[0]); r++) {
                n = 0;
                for (i = 0; i < reps[r] && n + ul < sizeof(buf); i++) {
                    memcpy(buf + n, units[u], ul);
                    n += ul;
                }
                dump("unit", buf, n);

                for (i = 0; i < 5; i++) {
                    size_t pre = (size_t[]){ 0, 1, 63, 64, 65 }[i];
                    size_t m = 0;
                    if (pre + ul * reps[r] + 8 >= sizeof(buf)) continue;
                    memset(buf, 'a', pre); m = pre;
                    { size_t k; for (k = 0; k < reps[r]; k++) { memcpy(buf + m, units[u], ul); m += ul; } }
                    memcpy(buf + m, "TAILtail", 8); m += 8;
                    dump("prefixed", buf, m);
                }
            }
        }
    }

    for (round = 0; round < 20000; round++) {
        n = rnd() % 300;
        for (i = 0; i < n; i++) {
            uint32_t r = rnd();
            switch (r % 5) {
            case 0: buf[i] = (uint8_t)(r >> 8) & 0x7f; break;
            case 1: buf[i] = 0xC0 | ((r >> 8) & 0x1f); break;
            case 2: buf[i] = 0x80 | ((r >> 8) & 0x3f); break;
            case 3: buf[i] = 0xE0 | ((r >> 8) & 0x0f); break;
            default: buf[i] = (uint8_t)(r >> 8); break;
            }
        }
        dump("soup", buf, n);
    }

    for (round = 0; round < 20000; round++) {
        static const char *cps[] = { "x", "\xC3\xA9", "\xC4\x80", "\xE6\x97\xA5",
                                     "\xF0\x9F\x98\x80", " ", "\xC2\xA0" };
        n = 0;
        while (n < 40 + rnd() % 240) {
            const char *c = cps[rnd() % (sizeof(cps) / sizeof(cps[0]))];
            size_t cl = strlen(c);
            if (n + cl >= sizeof(buf)) break;
            memcpy(buf + n, c, cl);
            n += cl;
        }
        dump("valid", buf, n);
    }

    printf("TOTAL %lu\n", line_no);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
