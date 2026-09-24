/*
 * test_utf8_ingress.c -- differential oracle for the SIMD UTF-8 ingress path in
 * JS_NewStringLen() (src/runtime/class.inc.c, CONFIG_UTF8_SIMD).
 *
 * The hazard this exists to catch: the simd.utf8_to_latin1 / utf8_to_utf16le
 * kernels are STRICT -- they REJECT ill-formed UTF-8 -- while JS_NewStringLen's
 * contract is LOSSY: an invalid sequence becomes U+FFFD, and the number of bytes
 * it swallows follows a specific (idiosyncratic) rule. A fast path that quietly
 * substitutes different replacement characters, or resynchronises at a different
 * byte, produces a *valid-looking* string that is simply wrong -- no crash, no
 * assertion, just corrupted text. So the only adequate proof is that the exact
 * code units are identical with the optimisation compiled in and out.
 *
 * This is a C test, not JS, because malformed UTF-8 cannot be constructed from
 * JS source: every route into the engine from JS has already been through a
 * decoder. Here the bytes go straight to JS_NewStringLen.
 *
 * Build + run BOTH ways and diff (the whole point -- one build proves nothing):
 *
 *   make CONFIG_NATIVE_MODULES=y
 *   clang -I. -Isrc -O2 -o /tmp/u8a tests/test_utf8_ingress.c libdynajs.a -lm -lpthread
 *   /tmp/u8a > /tmp/a.txt
 *   rm .obj/dynajs.o && make CONFIG_NATIVE_MODULES=y \
 *       CFLAGS_EXTRA=-DCONFIG_UTF8_SIMD=0            # (or edit the #define)
 *   clang ... -o /tmp/u8b tests/test_utf8_ingress.c libdynajs.a -lm -lpthread
 *   /tmp/u8b > /tmp/b.txt
 *   cmp /tmp/a.txt /tmp/b.txt
 *
 * Runs on the arm64 host and under docker linux/amd64 (tools/xplat-verify.sh),
 * which is what proves the x86 kernels agree with NEON -- they never execute on
 * the dev host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dynajs.h"

static JSContext *ctx;
static JSRuntime *rt;
static unsigned long line_no;

/* Print the exact code units of the string JS_NewStringLen produced. Comparing
   rendered text would hide a differing-but-printable substitution. */
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

/* deterministic PRNG so both builds see the identical corpus */
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

    /* ---- hand-built cases, each straddling JS_UTF8_SIMD_MIN_LEN (64) ---- */
    {
        static const char *units[] = {
            "a",                    /* ASCII */
            "\xC3\xA9",             /* U+00E9, latin1-representable */
            "\xC2\x80",             /* U+0080, lowest 2-byte */
            "\xC3\xBF",             /* U+00FF, highest latin1 */
            "\xC4\x80",             /* U+0100, first NON-latin1 -> forces wide */
            "\xE6\x97\xA5",         /* U+65E5 CJK, 3-byte */
            "\xE2\x9C\x93",         /* U+2713, 3-byte */
            "\xF0\x9F\x98\x80",     /* U+1F600, 4-byte -> surrogate pair */
            "\xEF\xBF\xBD",         /* U+FFFD itself, must not be confused with
                                       a substituted one */
            /* --- malformed: every class the lossy path has a rule for --- */
            "\xFF",                 /* invalid lead */
            "\xFE",                 /* invalid lead */
            "\x80",                 /* stray continuation */
            "\xBF",                 /* stray continuation */
            "\xC3",                 /* truncated 2-byte */
            "\xE6\x97",             /* truncated 3-byte */
            "\xF0\x9F\x98",         /* truncated 4-byte */
            "\xC0\x80",             /* overlong NUL */
            "\xC1\xBF",             /* overlong */
            "\xE0\x80\x80",         /* overlong 3-byte */
            "\xF0\x80\x80\x80",     /* overlong 4-byte */
            "\xED\xA0\x80",         /* CESU-8 high surrogate U+D800 */
            "\xED\xBF\xBF",         /* CESU-8 low surrogate U+DFFF */
            "\xF4\x90\x80\x80",     /* > U+10FFFF */
            "\xF5\x80\x80\x80",     /* > U+10FFFF lead */
            "\xF8\x88\x80\x80\x80", /* 5-byte (never legal) */
            "\xC3\x28",             /* bad continuation after valid lead */
            "\xE6\x28\xA5",         /* bad continuation mid 3-byte */
            "\x00",                 /* embedded NUL */
        };
        size_t u;
        for (u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
            size_t ul = (units[u][0] == '\0') ? 1 : strlen(units[u]);
            /* repeat to cross the 64-byte threshold in both directions, and
               place the unit at the front, middle and end of an ASCII filler so
               the count_ascii prefix length varies */
            size_t reps[] = { 1, 2, 20, 40, 100 };
            size_t r;
            for (r = 0; r < sizeof(reps) / sizeof(reps[0]); r++) {
                n = 0;
                for (i = 0; i < reps[r] && n + ul < sizeof(buf); i++) {
                    memcpy(buf + n, units[u], ul);
                    n += ul;
                }
                dump("unit", buf, n);

                /* ASCII prefix + unit + ASCII suffix, several prefix lengths */
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

    /* ---- random byte soup: the sequences nobody thinks to hand-write ---- */
    for (round = 0; round < 20000; round++) {
        n = rnd() % 300;
        for (i = 0; i < n; i++) {
            uint32_t r = rnd();
            switch (r % 5) {
            case 0: buf[i] = (uint8_t)(r >> 8) & 0x7f; break;  /* ASCII */
            case 1: buf[i] = 0xC0 | ((r >> 8) & 0x1f); break;  /* 2-byte lead */
            case 2: buf[i] = 0x80 | ((r >> 8) & 0x3f); break;  /* continuation */
            case 3: buf[i] = 0xE0 | ((r >> 8) & 0x0f); break;  /* 3-byte lead */
            default: buf[i] = (uint8_t)(r >> 8); break;        /* anything */
            }
        }
        dump("soup", buf, n);
    }

    /* ---- valid-UTF-8 soup: the fast path must actually be taken here ---- */
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
