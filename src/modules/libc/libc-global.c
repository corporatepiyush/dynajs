#include "libc-internal.h"

JSValue js_global_btoa(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len, i, o;
    char* out;
    JSValue res;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "btoa: a string is required");
    const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    {
        unsigned char* bytes = (unsigned char*)malloc(len + 1);
        size_t bi = 0;
        if (!bytes) {
            JS_FreeCString(ctx, s);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < len;) {
            unsigned char c = (unsigned char)s[i];
            uint32_t cp;
            size_t adv;
            if (c < 0x80) {
                cp = c;
                adv = 1;
            } else if ((c & 0xe0) == 0xc0 && i + 1 < len
                && ((unsigned char)s[i + 1] & 0xc0) == 0x80) {
                cp = ((uint32_t)(c & 0x1f) << 6)
                    | ((unsigned char)s[i + 1] & 0x3f);
                adv = 2;
            } else if ((c & 0xf0) == 0xe0 && i + 2 < len
                && ((unsigned char)s[i + 1] & 0xc0) == 0x80
                && ((unsigned char)s[i + 2] & 0xc0) == 0x80) {
                cp = ((uint32_t)(c & 0x0f) << 12)
                    | (((unsigned char)s[i + 1] & 0x3f) << 6)
                    | ((unsigned char)s[i + 2] & 0x3f);
                adv = 3;
            } else {
                cp = 0x110000;
                adv = 1;
            }
            if (cp > 0xff) {
                free(bytes);
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx,
                    "btoa: string contains characters outside Latin-1");
            }
            bytes[bi++] = (unsigned char)cp;
            i += adv;
        }
        out = (char*)malloc(bi / 3 * 4 + 5);
        if (!out) {
            free(bytes);
            JS_FreeCString(ctx, s);
            return JS_ThrowOutOfMemory(ctx);
        }
        {
            size_t src_len = bi;
            const unsigned char* bs = bytes;
            o = 0;
            for (i = 0; i + 2 < src_len; i += 3) {
                uint32_t v = ((uint32_t)bs[i] << 16)
                    | ((uint32_t)bs[i + 1] << 8)
                    | (uint32_t)bs[i + 2];
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = enc[(v >> 6) & 63];
                out[o++] = enc[v & 63];
            }
            if (src_len - i == 1) {
                uint32_t v = (uint32_t)bs[i] << 16;
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = '=';
                out[o++] = '=';
            } else if (src_len - i == 2) {
                uint32_t v = ((uint32_t)bs[i] << 16)
                    | ((uint32_t)bs[i + 1] << 8);
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = enc[(v >> 6) & 63];
                out[o++] = '=';
            }
            free(bytes);
            JS_FreeCString(ctx, s);
            res = JS_NewStringLen(ctx, out, o);
            free(out);
            return res;
        }
    }
}

JSValue js_global_atob(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static int rev_init = 0;
    static int8_t rev[256];
    size_t len, i, o, npad, need;
    uint32_t acc = 0;
    int nbits = 0;
    uint8_t *raw, *out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "atob: a string is required");
    const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (!rev_init) {
        int j;
        memset(rev, -1, sizeof rev);
        for (j = 0; j < 26; j++) {
            rev['A' + j] = (int8_t)j;
            rev['a' + j] = (int8_t)(j + 26);
        }
        for (j = 0; j < 10; j++)
            rev['0' + j] = (int8_t)(j + 52);
        rev['+'] = 62;
        rev['/'] = 63;
        rev_init = 1;
    }
    if (len / 2 > (SIZE_MAX - 2) / 3) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    raw = (uint8_t*)malloc(len + 1);
    need = len / 2 * 3 + 2;
    out = (uint8_t*)malloc(need);
    if (!raw || !out) {
        free(raw);
        free(out);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    o = 0;
    npad = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int8_t d;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0c)
            continue;
        if (c == '=') {
            npad++;
            continue;
        }
        if (npad) {
        bad:
            free(raw);
            free(out);
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx,
                "atob: invalid character in base64 data");
        }
        d = rev[c];
        if (d < 0)
            goto bad;
        acc = (acc << 6) | (uint32_t)d;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            raw[o++] = (uint8_t)((acc >> nbits) & 0xff);
        }
    }
    if (nbits >= 6 || npad > 2) {
        free(raw);
        free(out);
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "atob: base64 length is a non-multiple "
                                      "of 4 characters");
    }
    JS_FreeCString(ctx, s);
    {
        size_t uo = 0;
        for (i = 0; i < o; i++) {
            if (raw[i] < 0x80) {
                out[uo++] = raw[i];
            } else {
                out[uo++] = (uint8_t)(0xc2 | (raw[i] >> 6));
                out[uo++] = (uint8_t)(0x80 | (raw[i] & 0x3f));
            }
        }
        JSValue res = JS_NewStringLen(ctx, (const char*)out, uo);
        free(raw);
        free(out);
        return res;
    }
}

#if defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_HTTP)
void dyn_global_install_from_module(JSContext* ctx,
    const char* module,
    const char* holder,
    const char* const* pairs,
    int npairs)
{
    char src[192];
    JSValue r, v, g, hobj;
    JSPromiseStateEnum st;
    int i;

    snprintf(src, sizeof src,
        "import * as m from \"%s\";\n"
        "globalThis.%s = m;\n",
        module, holder);
    r = JS_Eval(ctx, src, strlen(src), "<dyna:globals>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        goto skip_exc;
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v))
        goto skip_exc;
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(ctx, v);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, err);
        return;
    }
    g = JS_GetGlobalObject(ctx);
    hobj = JS_GetPropertyStr(ctx, g, holder);
    if (JS_IsException(hobj)) {
        hobj = JS_UNDEFINED;
    } else if (JS_IsObject(hobj)) {
        for (i = 0; i + 1 < 2 * npairs; i += 2) {
            JSValue cv = JS_GetPropertyStr(ctx, hobj, pairs[i]);
            if (JS_IsException(cv))
                break;
            if (!JS_IsUndefined(cv))
                JS_DefinePropertyValueStr(ctx, g, pairs[i + 1], cv, JS_PROP_C_W_E);
            else
                JS_FreeValue(ctx, cv);
        }
        {
            JSAtom hatom = JS_NewAtom(ctx, holder);
            JS_DeleteProperty(ctx, g, hatom, 0);
            JS_FreeAtom(ctx, hatom);
        }
    }
    JS_FreeValue(ctx, hobj);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, v);
    if (JS_HasException(ctx)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
    }
    return;
skip_exc:
    {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
    }
}
#endif
