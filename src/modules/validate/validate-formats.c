#include "validate-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

static size_t dyn_v_b64len(size_t n)
{
    size_t r = n % 4;
    return n / 4 * 3 + (r == 2 ? 1 : r == 3 ? 2
                                            : 0);
}

int dyn_v_jwt(JSContext* ctx, const char* s, size_t n, JSValueConst algs)
{
    size_t d1 = (size_t)-1, d2 = (size_t)-1, i;
    uint8_t hb[3073], pb[3073], sb[3073];
    char hs[4100], ps[4100], ss[4100];
    size_t hl, pl, sl, hlen, plen;
    JSValue h, v;
    int in_alg = 1;

    for (i = 0; i < n; i++) {
        if (s[i] == '=') {
            return 0;
        } else if (s[i] == '.') {
            if (d1 == (size_t)-1)
                d1 = i;
            else if (d2 == (size_t)-1)
                d2 = i;
            else
                return 0;
        }
    }
    if (d1 == (size_t)-1 || d2 == (size_t)-1)
        return 0;
    if (d1 == 0 || d2 == d1 + 1 || n - d2 - 1 == 0)
        return 0;
    hlen = d1;
    plen = d2 - d1 - 1;

    hl = dyn_codec_base64url_decode(s, hlen, hb, hs);
    if (hl == DYN_CODEC_BAD || hl > dyn_v_b64len(hlen))
        return 0;
    hb[hl] = 0;
    h = JS_ParseJSON(ctx, (const char*)hb, hl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;
    }
    v = JS_GetPropertyStr(ctx, h, "alg");
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, h);
        return 0;
    }
    if (!JS_IsUndefined(algs)) {
        int inlist = 0;
        if (JS_IsArray(ctx, algs)) {
            uint32_t an, ai;
            const char* alg = JS_ToCString(ctx, v);
            if (!alg) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, h);
                return -1;
            }
            if (dyn_js_len_u32(ctx, algs, &an)) {
                if (alg)
                    JS_FreeCString(ctx, alg);
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, h);
                return -1;
            }
            for (ai = 0; ai < an && !inlist; ai++) {
                JSValue e = JS_GetPropertyUint32(ctx, algs, ai);
                const char* ea = JS_ToCString(ctx, e);
                JS_FreeValue(ctx, e);
                if (!ea) {
                    if (alg)
                        JS_FreeCString(ctx, alg);
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, h);
                    return -1;
                }
                if (alg && strcmp(alg, ea) == 0)
                    inlist = 1;
                JS_FreeCString(ctx, ea);
            }
            if (alg)
                JS_FreeCString(ctx, alg);
        }
        in_alg = inlist;
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, h);
    if (!in_alg)
        return 0;

    pl = dyn_codec_base64url_decode(s + d1 + 1, plen, pb, ps);
    if (pl == DYN_CODEC_BAD || pl > dyn_v_b64len(plen))
        return 0;
    pb[pl] = 0;
    h = JS_ParseJSON(ctx, (const char*)pb, pl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;
    }
    JS_FreeValue(ctx, h);

    sl = dyn_codec_base64url_decode(s + d2 + 1, n - d2 - 1, sb, ss);
    if (sl == DYN_CODEC_BAD || sl > dyn_v_b64len(n - d2 - 1))
        return 0;
    return 1;
}

#endif
