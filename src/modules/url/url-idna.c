#include "url-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_URL)

static int dyn_idna_js_to_cps(JSContext* ctx, JSValueConst v, dyn_cp_t* out)
{
    size_t n;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    const uint8_t *p, *end;
    if (!s)
        return -1;
    p = (const uint8_t*)s;
    end = p + n;
    while (p < end) {
        const uint8_t* np;
        int c = unicode_from_utf8(p, (int)(end - p), &np);
        if (c < 0 || dyn_cp_put(out, (uint32_t)c)) {
            JS_FreeCString(ctx, s);
            return -1;
        }
        p = np;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue dyn_idna_cps_to_js(JSContext* ctx, const uint32_t* cps, size_t n)
{
    char* buf;
    size_t cap, blen = 0, i;
    JSValue r;

    cap = n * UTF8_CHAR_LEN_MAX + 1;
    buf = (char*)malloc(cap);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        int len = unicode_to_utf8((uint8_t*)buf + blen, cps[i]);
        if (len <= 0) {
            free(buf);
            return JS_ThrowTypeError(ctx,
                "dyna:url: cannot encode code point U+%04X as UTF-8", cps[i]);
        }
        blen += (size_t)len;
    }
    r = JS_NewStringLen(ctx, buf, blen);
    free(buf);
    return r;
}

static int dyn_idna_opts(JSContext* ctx, int argc, JSValueConst* argv, int nkeys, int* strict)
{
    JSValue v;
    int b;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return 0;
    {
        static const char* const keys[] = { "transitional", "strict" };
        JSPropertyEnum* props = NULL;
        uint32_t nprops = 0, i;
        int j, bad = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, argv[1],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        for (i = 0; i < nprops && !bad; i++) {
            const char* name = JS_AtomToCString(ctx, props[i].atom);
            if (!name) {
                bad = 1;
                break;
            }
            for (j = 0; j < nkeys; j++)
                if (strcmp(name, keys[j]) == 0)
                    break;
            if (j == nkeys) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: %s)", name, nkeys > 1 ? "transitional, strict" : "transitional");
                bad = 1;
            }
            JS_FreeCString(ctx, name);
        }
        for (i = 0; i < nprops; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        if (bad)
            return -1;
    }
    if (strict && dyn_opt_bool(ctx, argv[1], "strict", "domainToASCII", strict) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, argv[1], "transitional");
    if (JS_IsException(v))
        return -1;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

JSValue dyn_domain_to_ascii(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cp_t src, out;
    dyn_idna_proc_t p;
    int trans;
    JSValue r;
    size_t slen = 0;
    const char* cstr;
    int strict = 0;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "domainToASCII(domain[, opts]): argument must be a string");
    trans = dyn_idna_opts(ctx, argc, argv, 2, &strict);
    if (trans < 0)
        return JS_EXCEPTION;

    cstr = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!cstr)
        return JS_EXCEPTION;

    if (slen == 0) {
        JS_FreeCString(ctx, cstr);
        return JS_NewStringLen(ctx, "", 0);
    }
    {
        if (dyn_idna_ascii_fast_check(cstr, slen)) {
            char* buf = (char*)malloc(slen + 1);
            if (buf) {
                size_t k;
                for (k = 0; k < slen; k++) {
                    char c = cstr[k];
                    if (c >= 'A' && c <= 'Z')
                        c = (char)(c + 32);
                    buf[k] = c;
                }
                buf[slen] = 0;
                JS_FreeCString(ctx, cstr);
                r = JS_NewStringLen(ctx, buf, slen);
                free(buf);
                return r;
            }
        }
        JS_FreeCString(ctx, cstr);
    }

    dyn_cp_init(&src);
    dyn_cp_init(&out);
    if (dyn_idna_js_to_cps(ctx, argv[0], &src) < 0) {
        dyn_cp_free(&src);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_idna_to_ascii(&p, src.p, (int)src.n, trans, strict, &out);
    dyn_cp_free(&src);
    if (p.oom) {
        dyn_idna_labs_free(&p.labs);
        dyn_cp_free(&p.cps);
        dyn_cp_free(&out);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (p.nerrors) {
        const char* stage = p.stage ? p.stage : "validation";
        dyn_idna_labs_free(&p.labs);
        dyn_cp_free(&p.cps);
        dyn_cp_free(&out);
        return JS_ThrowTypeError(ctx, "domainToASCII: %s error", stage);
    }
    r = dyn_idna_cps_to_js(ctx, out.p, out.n);
    dyn_idna_labs_free(&p.labs);
    dyn_cp_free(&p.cps);
    dyn_cp_free(&out);
    return r;
}

JSValue dyn_domain_to_unicode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cp_t src;
    dyn_idna_proc_t p;
    int trans;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "domainToUnicode(domain[, opts]): argument must be a string");
    trans = dyn_idna_opts(ctx, argc, argv, 1, NULL);
    if (trans < 0)
        return JS_EXCEPTION;
    dyn_cp_init(&src);
    if (dyn_idna_js_to_cps(ctx, argv[0], &src) < 0) {
        dyn_cp_free(&src);
        return JS_ThrowOutOfMemory(ctx);
    }

    if (src.n == 0) {
        dyn_cp_free(&src);
        return JS_NewStringLen(ctx, "", 0);
    }
    p = dyn_idna_process(src.p, (int)src.n, trans, 0, 1);
    dyn_cp_free(&src);
    if (p.oom) {
        dyn_idna_labs_free(&p.labs);
        dyn_cp_free(&p.cps);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (p.nerrors) {
        const char* stage = p.stage ? p.stage : "validation";
        dyn_idna_labs_free(&p.labs);
        dyn_cp_free(&p.cps);
        return JS_ThrowTypeError(ctx, "domainToUnicode: %s error", stage);
    }
    r = dyn_idna_cps_to_js(ctx, p.cps.p, p.cps.n);
    dyn_idna_labs_free(&p.labs);
    dyn_cp_free(&p.cps);
    return r;
}

JSValue dyn_punycode_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cp_t src, out;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "punycodeEncode(text): argument must be a string");
    dyn_cp_init(&src);
    dyn_cp_init(&out);
    if (dyn_idna_js_to_cps(ctx, argv[0], &src) < 0) {
        dyn_cp_free(&src);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (src.n > 1024) {
        dyn_cp_free(&src);
        return JS_ThrowRangeError(ctx, "punycodeEncode: input exceeds 1024 code points");
    }
    if (pc_encode(src.p, (int)src.n, &out) < 0) {
        dyn_cp_free(&src);
        dyn_cp_free(&out);
        return JS_ThrowTypeError(ctx,
            "punycodeEncode: cannot encode (lone surrogate or invalid code point)");
    }
    dyn_cp_free(&src);
    r = dyn_idna_cps_to_js(ctx, out.p, out.n);
    dyn_cp_free(&out);
    return r;
}

JSValue dyn_punycode_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cp_t out;
    JSValue r;
    size_t n;
    const char* s;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "punycodeDecode(text): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > 1024) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "punycodeDecode: input exceeds 1024 octets");
    }
    dyn_cp_init(&out);
    if (pc_decode(s, n, &out) < 0) {
        JS_FreeCString(ctx, s);
        dyn_cp_free(&out);
        return JS_ThrowTypeError(ctx, "punycodeDecode: invalid punycode input");
    }
    JS_FreeCString(ctx, s);
    r = dyn_idna_cps_to_js(ctx, out.p, out.n);
    dyn_cp_free(&out);
    return r;
}

#endif
