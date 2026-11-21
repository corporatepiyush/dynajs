#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#define DYN_DETECT_MIN_BYTES 16

static const char* dyn_detect_cjk(const uint8_t* p, size_t n)
{
    size_t i;
    int gbk_hits = 0, big5_hits = 0, sjis_hits = 0, eucjp_hits = 0, euckr_hits = 0;
    int sjis_kana_hits = 0;
    int eucjp_specials = 0;
    int total_multibyte = 0;

    for (i = 0; i < n; i++) {
        uint8_t c1 = p[i];
        if (c1 < 0x80)
            continue;
        total_multibyte++;

        if (i + 1 < n) {
            uint8_t c2 = p[i + 1];

            if (c1 == 0x8E && c2 >= 0xA1 && c2 <= 0xDF) {
                eucjp_hits += 2;
                eucjp_specials++;
                i++;
                continue;
            }
            if (c1 == 0x8F && i + 2 < n) {
                uint8_t c3 = p[i + 2];
                if (c2 >= 0xA1 && c2 <= 0xFE && c3 >= 0xA1 && c3 <= 0xFE) {
                    eucjp_hits += 3;
                    eucjp_specials += 2;
                    i += 2;
                    continue;
                }
            }

            if (((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC)) && ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC))) {
                sjis_hits += 2;
                if (c1 == 0x82 || c1 == 0x83)
                    sjis_kana_hits++;
            }

            if (c1 >= 0x81 && c1 <= 0xFE && c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                gbk_hits += 2;
            }

            if (c1 >= 0x81 && c1 <= 0xFE && ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0xA1 && c2 <= 0xFE))) {
                big5_hits += 2;
            }

            if (c1 >= 0xA1 && c1 <= 0xFE && c2 >= 0xA1 && c2 <= 0xFE) {
                eucjp_hits += 2;
                euckr_hits += 2;
            }
            i++;
        }
    }

    if (total_multibyte == 0)
        return "utf-8";

    if (eucjp_specials > 0 && eucjp_hits >= sjis_hits && eucjp_hits >= gbk_hits)
        return "euc-jp";
    if (sjis_kana_hits > 0 && sjis_hits >= gbk_hits)
        return "shift_jis";
    if (sjis_hits > gbk_hits && sjis_hits > eucjp_hits && sjis_hits > big5_hits)
        return "shift_jis";
    if (big5_hits > gbk_hits && big5_hits > sjis_hits)
        return "big5";
    if (gbk_hits > 0 && gbk_hits >= sjis_hits && gbk_hits >= eucjp_hits)
        return "gbk";
    if (euckr_hits > 0 && euckr_hits >= gbk_hits)
        return "euc-kr";
    if (eucjp_hits > 0)
        return "euc-jp";

    return NULL;
}

static void detect_allow_free(char** list, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

static int detect_allow_read(JSContext* ctx, JSValueConst arr, char*** plist, uint32_t* pn)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t len = 0, i;
    char** list;
    if (JS_IsException(lv) || JS_ToUint32(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    uint32_t cap = 16;
    list = (char**)calloc(cap, sizeof(char*));
    if (!list) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, arr, i);
        const char* str;
        if (JS_IsException(item)) {
            detect_allow_free(list, i);
            return -1;
        }
        if (!JS_IsString(item)) {
            JS_FreeValue(ctx, item);
            detect_allow_free(list, i);
            JS_ThrowTypeError(ctx, "detectEncoding: allowList[%u] must be a string", (unsigned)i);
            return -1;
        }
        if (i == cap) {
            char** grown = (char**)realloc(list, (size_t)cap * 2 * sizeof(char*));
            if (!grown) {
                JS_FreeValue(ctx, item);
                detect_allow_free(list, i);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            list = grown;
            cap *= 2;
        }
        str = JS_ToCString(ctx, item);
        JS_FreeValue(ctx, item);
        if (!str) {
            detect_allow_free(list, i);
            return -1;
        }
        list[i] = strdup(str);
        JS_FreeCString(ctx, str);
        if (!list[i]) {
            detect_allow_free(list, i);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    *plist = list;
    *pn = len;
    return 0;
}

static int detect_allow_has(char* const* list, uint32_t n, const char* name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (strcasecmp(list[i], name) == 0)
            return 1;
    return 0;
}

static const char* const detect_keys[] = { "fallback", "allowList" };

JSValue dyn_enc_detect_encoding(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* p;
    size_t n;
    const char* verdict = NULL;
    const char* fallback = "utf-8";
    char* fallback_alloc = NULL;
    char** allow = NULL;
    uint32_t nallow = 0;
    int has_allow = 0;
    JSValue ret;

    (void)this_val;
    if (dyn_enc_view(ctx, argv[0], &p, &n) < 0)
        return JS_ThrowTypeError(ctx, "detectEncoding requires a TypedArray or ArrayBuffer");
    if (argc > 1 && dyn_opts_strict(ctx, argv[1], detect_keys, countof(detect_keys)) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue fb = JS_GetPropertyStr(ctx, argv[1], "fallback");
        JSValue al;
        if (JS_IsException(fb))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(fb) && !JS_IsNull(fb)) {
            const char* fs;
            if (!JS_IsString(fb)) {
                JS_FreeValue(ctx, fb);
                return JS_ThrowTypeError(ctx, "detectEncoding: fallback must be a string");
            }
            fs = JS_ToCString(ctx, fb);
            if (!fs) {
                JS_FreeValue(ctx, fb);
                return JS_EXCEPTION;
            }
            fallback_alloc = strdup(fs);
            JS_FreeCString(ctx, fs);
            if (!fallback_alloc) {
                JS_FreeValue(ctx, fb);
                return JS_ThrowOutOfMemory(ctx);
            }
            fallback = fallback_alloc;
        }
        JS_FreeValue(ctx, fb);
        al = JS_GetPropertyStr(ctx, argv[1], "allowList");
        if (JS_IsException(al)) {
            free(fallback_alloc);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(al) && !JS_IsNull(al)) {
            if (!JS_IsArray(ctx, al)) {
                JS_FreeValue(ctx, al);
                free(fallback_alloc);
                return JS_ThrowTypeError(ctx, "detectEncoding: allowList must be an array of strings");
            }
            if (detect_allow_read(ctx, al, &allow, &nallow)) {
                JS_FreeValue(ctx, al);
                free(fallback_alloc);
                return JS_EXCEPTION;
            }
            has_allow = 1;
        }
        JS_FreeValue(ctx, al);
    }

    if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
        verdict = "utf-32be";
    } else if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
        verdict = "utf-32le";
    } else if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        verdict = "utf-8";
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        verdict = "utf-16be";
    } else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        verdict = "utf-16le";
    }

    if (!verdict) {
        if (simd.validate_utf8 && simd.validate_utf8(p, n) == n) {
            verdict = "utf-8";
        }
    }

    if (!verdict && n >= DYN_DETECT_MIN_BYTES) {
        verdict = dyn_detect_cjk(p, n);
    }

    if (!verdict)
        verdict = fallback;

    if (has_allow && !detect_allow_has(allow, nallow, verdict)) {
        if (fallback && strcasecmp(fallback, verdict) != 0 && detect_allow_has(allow, nallow, fallback)) {
            verdict = fallback;
        } else {
            detect_allow_free(allow, nallow);
            free(fallback_alloc);
            return JS_ThrowTypeError(ctx, "detectEncoding: detected encoding not in allowList and no valid fallback");
        }
    }
    ret = JS_NewString(ctx, verdict);
    detect_allow_free(allow, nallow);
    free(fallback_alloc);
    return ret;
}

#endif
