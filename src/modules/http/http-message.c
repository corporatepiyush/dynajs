#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

static const char* const http_cookie_keys[] = { "maxAge", "domain", "path", "sameSite", "secure", "httpOnly" };

static int dyn_hm_needs_quote(const char* s, size_t n)
{
    size_t i;
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++)
        if (!httpk_tchar[(unsigned char)s[i]])
            return 1;
    return 0;
}

static void dyn_hm_write_value(dyn_sb_t* b, const char* s, size_t n)
{
    size_t i;
    if (!dyn_hm_needs_quote(s, n)) {
        dyn_sb_put(b, s, n);
        return;
    }
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\')
            dyn_sb_putc(b, '\\');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

static JSValue dyn_hm_ctype_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue tv, sv, pv, ret = JS_EXCEPTION;
    const char *t = NULL, *st = NULL;
    dyn_sb_t b;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ContentTypeFormat(obj): argument must be an object");
    dyn_sb_init(&b);
    tv = JS_GetPropertyStr(ctx, argv[0], "type");
    sv = JS_GetPropertyStr(ctx, argv[0], "subtype");
    pv = JS_GetPropertyStr(ctx, argv[0], "parameters");
    t = JS_IsString(tv) ? JS_ToCString(ctx, tv) : NULL;
    st = JS_IsString(sv) ? JS_ToCString(ctx, sv) : NULL;
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, sv);
    if (!t || !st || !*t || !*st) {
        JS_ThrowTypeError(ctx, "ContentTypeFormat(obj): type and subtype are required");
        goto done;
    }
    if (dyn_mp_ctl(t, strlen(t)) || dyn_mp_ctl(st, strlen(st))) {
        JS_ThrowTypeError(ctx,
            "ContentTypeFormat(obj): type and subtype must not contain "
            "control characters");
        goto done;
    }
    dyn_sb_puts(&b, t);
    dyn_sb_putc(&b, '/');
    dyn_sb_puts(&b, st);
    if (JS_IsObject(pv)
        && JS_GetOwnPropertyNames(ctx, &tab, &len, pv,
               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            == 0) {
        for (i = 0; i < len; i++) {
            JSValue kv = JS_AtomToString(ctx, tab[i].atom);
            JSValue vv = JS_GetProperty(ctx, pv, tab[i].atom);
            const char* ks = JS_IsException(kv) ? NULL : JS_ToCString(ctx, kv);
            size_t vn = 0;
            const char* vs = JS_IsException(vv) ? NULL : JS_ToCStringLen(ctx, &vn, vv);
            JS_FreeValue(ctx, kv);
            JS_FreeValue(ctx, vv);
            if (ks && vs) {
                size_t kk, kn = strlen(ks);
                int bad_name = (kn == 0 || !httpk_tchar[(unsigned char)ks[0]]);
                for (kk = 0; kk < kn && !bad_name; kk++)
                    if (!httpk_tchar[(unsigned char)ks[kk]])
                        bad_name = 1;
                if (bad_name || dyn_mp_ctl(vs, vn)) {
                    JS_FreeCString(ctx, ks);
                    JS_FreeCString(ctx, vs);
                    JS_ThrowTypeError(ctx,
                        "ContentTypeFormat(obj): parameter names must be HTTP "
                        "tokens and values must not contain control characters");
                    goto done;
                }
                dyn_sb_puts(&b, "; ");
                dyn_sb_puts(&b, ks);
                dyn_sb_putc(&b, '=');
                dyn_hm_write_value(&b, vs, vn);
            }
            if (ks)
                JS_FreeCString(ctx, ks);
            if (vs)
                JS_FreeCString(ctx, vs);
        }
    }
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
done:
    if (tab)
        JS_FreePropertyEnum(ctx, tab, len);
    dyn_sb_free(&b);
    JS_FreeValue(ctx, pv);
    if (t)
        JS_FreeCString(ctx, t);
    if (st)
        JS_FreeCString(ctx, st);
    return ret;
}

static double dyn_hm_media_score(const char* range, size_t rn,
    const char* cand, size_t cn, double q,
    int* spec)
{
    size_t rs = 0, cs = 0, i;
    for (i = 0; i < rn; i++)
        if (range[i] == '/') {
            rs = i;
            break;
        }
    for (i = 0; i < cn; i++)
        if (cand[i] == '/') {
            cs = i;
            break;
        }
    if (rn == 3 && strncasecmp(range, "*/*", 3) == 0) {
        *spec = 0;
        return q;
    }
    if (!rs || !cs)
        return -1;
    if (rs != cs || strncasecmp(range, cand, rs) != 0)
        return -1;
    if (rn - rs == 2 && range[rs + 1] == '*') {
        *spec = 1;
        return q;
    }
    if (rn - rs == cn - cs && strncasecmp(range + rs, cand + cs, rn - rs) == 0) {
        *spec = 2;
        return q;
    }
    return -1;
}

static JSValue dyn_hm_negotiate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* h;
    size_t hn, i = 0;
    int64_t ncand = 0, k;
    double best_q = -1;
    int best_spec = -1;
    int64_t best = -1;
    typedef struct {
        const char* s;
        size_t n;
    } hm_cand_t;
    hm_cand_t* cands = NULL;
    const char* fname = magic == 0 ? "Negotiate" : "NegotiateToken";
    int64_t cap = 0;
    JSValue ret = JS_NULL;

    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx,
            "%s(header, candidates): a string and an array are required", fname);
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        int64_t len = 0;
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        for (; ncand < len; ncand++) {
            JSValue cv = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)ncand);
            if (JS_IsException(cv))
                goto fail;
            if (!JS_IsString(cv)) {
                JS_FreeValue(ctx, cv);
                JS_ThrowTypeError(ctx, "%s: candidates[%lld] is not a string", fname, (long long)ncand);
                goto fail;
            }
            if (ncand == cap) {
                int64_t ncap = cap ? cap * 2 : 8;
                hm_cand_t* grown = (hm_cand_t*)realloc(cands, (size_t)ncap * sizeof(*cands));
                if (!grown) {
                    JS_FreeValue(ctx, cv);
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                cands = grown;
                cap = ncap;
            }
            cands[ncand].s = JS_ToCStringLen(ctx, &cands[ncand].n, cv);
            JS_FreeValue(ctx, cv);
            if (!cands[ncand].s)
                goto fail;
        }
    }
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h)
        goto fail;
    {
        size_t t0 = 0, t1 = hn;
        dyn_hm_trim(h, &t0, &t1);
        if (t0 == t1) {
            if (ncand > 0)
                best = 0;
            goto done;
        }
    }
    while (i < hn) {
        size_t eb = i, ee;
        double q;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        {
            size_t rb = eb, re = ee, semi;
            for (semi = rb; semi < re && h[semi] != ';'; semi++)
                ;
            q = httpk_weight(h + rb, re - rb) / 1000.0;
            re = semi;
            dyn_hm_trim(h, &rb, &re);
            if (rb == re || q <= 0 || !cands)
                continue;
            for (k = 0; k < ncand; k++) {
                const char* cs = cands[k].s;
                size_t cn = cands[k].n;
                int spec = 0;
                double sc = -1;

                if (!cs)
                    continue;
                if (magic == 0) {
                    sc = dyn_hm_media_score(h + rb, re - rb, cs, cn, q, &spec);
                } else if (re - rb == 1 && h[rb] == '*') {
                    sc = q;
                    spec = 0;
                } else if (re - rb == cn
                    && strncasecmp(h + rb, cs, cn) == 0) {
                    sc = q;
                    spec = 2;
                } else if (cn > re - rb && cs[re - rb] == '-'
                    && strncasecmp(h + rb, cs, re - rb) == 0) {
                    sc = q;
                    spec = 1;
                } else if (re - rb > cn && h[rb + cn] == '-'
                    && strncasecmp(h + rb, cs, cn) == 0) {

                    sc = q;
                    spec = 1;
                }
                if (sc > 0 && (sc > best_q || (sc == best_q && spec > best_spec))) {
                    best_q = sc;
                    best_spec = spec;
                    best = k;
                }
            }
        }
    }
done:
    if (best >= 0)
        ret = JS_NewStringLen(ctx, cands[best].s, cands[best].n);
    JS_FreeCString(ctx, h);
    for (k = 0; k < ncand; k++)
        JS_FreeCString(ctx, cands[k].s);
    free(cands);
    return ret;
fail:
    for (k = 0; k < ncand; k++)
        JS_FreeCString(ctx, cands[k].s);
    free(cands);
    return JS_EXCEPTION;
}

int dyn_hm_one_range(const char* h, size_t b, size_t e, int64_t size,
    int64_t* start, int64_t* end)
{
    size_t d, p;
    int have_start = 0, have_end = 0;
    *start = -1;
    *end = -1;
    for (d = b; d < e && h[d] != '-'; d++)
        ;
    if (d >= e)
        return -1;
    if (d > b) {
        *start = 0;
        have_start = 1;
        for (p = b; p < d; p++) {
            if (h[p] < '0' || h[p] > '9') {
                have_start = 0;
                break;
            }
            *start = *start * 10 + (h[p] - '0');
            if (*start > size)
                *start = size;
        }
    }
    if (d + 1 < e) {
        *end = 0;
        have_end = 1;
        for (p = d + 1; p < e; p++) {
            if (h[p] < '0' || h[p] > '9') {
                have_end = 0;
                break;
            }
            *end = *end * 10 + (h[p] - '0');
            if (*end > size)
                *end = size;
        }
    }
    if (!have_start && !have_end)
        return -1;
    if (!have_start) {
        if (*end == 0)
            return -1;
        *start = (*end >= size) ? 0 : size - *end;
        *end = size - 1;
    } else if (!have_end || *end >= size) {
        *end = size - 1;
    }
    if (size == 0 || *start >= size || *start > *end)
        return -1;
    return 0;
}

static JSValue dyn_hm_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* h;
    size_t hn, i = 0;
    int64_t size = 0;
    JSValue arr;
    uint32_t out = 0;
    int any_sat = 0;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "RangeParse(header, size): header must be a string");
    if (JS_ToInt64(ctx, &size, argv[1]))
        return JS_EXCEPTION;
    if (size < 0)
        return JS_ThrowRangeError(ctx, "RangeParse(header, size): size must be >= 0");
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    if (hn < 6 || strncasecmp(h, "bytes=", 6) != 0) {
        JS_FreeCString(ctx, h);
        return JS_NULL;
    }
    i = 6;
    {
        size_t b0 = i, e0 = hn;
        dyn_hm_trim(h, &b0, &e0);
        if (b0 == e0) {

            JS_FreeCString(ctx, h);
            return JS_NULL;
        }
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeCString(ctx, h);
        return JS_EXCEPTION;
    }
    while (i < hn) {
        size_t eb = i, ee;
        int64_t start, end;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        dyn_hm_trim(h, &eb, &ee);
        if (dyn_hm_one_range(h, eb, ee, size, &start, &end) < 0)
            continue;
        any_sat = 1;
        {
            JSValue o = JS_NewObject(ctx);
            if (JS_IsException(o)) {
                JS_FreeValue(ctx, arr);
                JS_FreeCString(ctx, h);
                return JS_EXCEPTION;
            }
            JS_DefinePropertyValueStr(ctx, o, "start", JS_NewInt64(ctx, start), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, o, "end", JS_NewInt64(ctx, end), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, arr, out++, o, JS_PROP_C_W_E);
        }
    }
    JS_FreeCString(ctx, h);
    if (!any_sat) {
        JS_FreeValue(ctx, arr);
        return JS_NewString(ctx, "unsatisfiable");
    }
    return arr;
}

static JSValue dyn_hm_cookie_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "CookieParse(header): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    while (i < n) {
        size_t nb, ne, vb, ve;
        while (i < n && (s[i] == ';' || dyn_hm_ows(s[i])))
            i++;
        if (i >= n)
            break;
        nb = i;
        while (i < n && s[i] != '=' && s[i] != ';')
            i++;
        ne = i;
        if (i < n && s[i] == '=') {
            i++;
            vb = i;
            while (i < n && s[i] != ';')
                i++;
            ve = i;
        } else {
            vb = ve = i;
        }
        dyn_hm_trim(s, &nb, &ne);
        dyn_hm_trim(s, &vb, &ve);
        if (ve - vb >= 2 && s[vb] == '"' && s[ve - 1] == '"') {
            vb++;
            ve--;
        }
        if (ne > nb) {
            JSAtom a = JS_NewAtomLen(ctx, s + nb, ne - nb);
            if (a != JS_ATOM_NULL) {
                JS_DefinePropertyValue(ctx, obj, a,
                    JS_NewStringLen(ctx, s + vb, ve - vb), JS_PROP_C_W_E);
                JS_FreeAtom(ctx, a);
            }
        }
    }
    JS_FreeCString(ctx, s);
    return obj;
}

static JSValue dyn_hm_cookie_serialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *nm, *val;
    size_t nn, vn;
    dyn_sb_t b;
    JSValue ret = JS_EXCEPTION, v;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "CookieSerialize(name, value, opts): name must be a string");
    nm = JS_ToCStringLen(ctx, &nn, argv[0]);
    if (!nm)
        return JS_EXCEPTION;
    val = JS_ToCStringLen(ctx, &vn, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, nm);
        return JS_EXCEPTION;
    }
    if (dyn_hm_needs_quote(nm, nn)) {
        JS_ThrowTypeError(ctx, "CookieSerialize(name, value): name must be a token");
        goto done0;
    }
    {
        size_t k;
        for (k = 0; k < vn; k++) {
            unsigned char c = (unsigned char)val[k];
            if (c < 0x21 || c == ';' || c == ',' || c == '\\' || c == '"' || c == 0x7F) {
                JS_ThrowTypeError(ctx,
                    "CookieSerialize(name, value): value must not contain \"%c\"", c);
                goto done0;
            }
        }
    }
    dyn_sb_init(&b);
    dyn_sb_put(&b, nm, nn);
    dyn_sb_putc(&b, '=');
    dyn_sb_put(&b, val, vn);
    if (argc > 2 && JS_IsObject(argv[2])) {
        int32_t age;
        const char* cs;
        if (dyn_opts_strict(ctx, argv[2], http_cookie_keys, 6)) {
            dyn_sb_free(&b);
            goto done0;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "maxAge");
        if (JS_IsException(v)) {
            dyn_sb_free(&b);
            goto done0;
        }
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt32(ctx, &age, v)) {
                JS_FreeValue(ctx, v);
                dyn_sb_free(&b);
                goto done0;
            }
            {
                char num[32];
                snprintf(num, sizeof num, "; Max-Age=%d", age);
                dyn_sb_puts(&b, num);
            }
        }
        JS_FreeValue(ctx, v);
        for (age = 0; age < 3; age++) {
            static const char* const K[3] = { "domain", "path", "sameSite" };
            static const char* const A[3] = { "; Domain=", "; Path=", "; SameSite=" };
            v = JS_GetPropertyStr(ctx, argv[2], K[age]);
            if (JS_IsString(v) && (cs = JS_ToCString(ctx, v)) != NULL) {
                size_t cl = strlen(cs);
                int bad = 0;
                size_t ci;
                for (ci = 0; ci < cl; ci++) {
                    unsigned char ch = (unsigned char)cs[ci];
                    if (ch < 0x21 || ch == ';' || ch == ',' || ch == 0x7F) {
                        bad = 1;
                        break;
                    }
                }
                if (bad) {
                    JS_FreeCString(ctx, cs);
                    JS_FreeValue(ctx, v);
                    dyn_sb_free(&b);
                    JS_ThrowTypeError(ctx,
                        "CookieSerialize: %s must not contain controls, ';' or ','", K[age]);
                    goto done0;
                }
                if (age == 2 && cl > 0) {
                    if (strcasecmp(cs, "Strict") != 0 && strcasecmp(cs, "Lax") != 0 && strcasecmp(cs, "None") != 0) {
                        JS_FreeCString(ctx, cs);
                        JS_FreeValue(ctx, v);
                        dyn_sb_free(&b);
                        JS_ThrowTypeError(ctx,
                            "CookieSerialize: sameSite must be Strict, Lax or None");
                        goto done0;
                    }
                }
                dyn_sb_puts(&b, A[age]);
                dyn_sb_puts(&b, cs);
                JS_FreeCString(ctx, cs);
            }
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "secure");
        {
            int sb = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (sb < 0) {
                dyn_sb_free(&b);
                goto done0;
            }
            if (sb)
                dyn_sb_puts(&b, "; Secure");
        }
        v = JS_GetPropertyStr(ctx, argv[2], "httpOnly");
        {
            int hb = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (hb < 0) {
                dyn_sb_free(&b);
                goto done0;
            }
            if (hb)
                dyn_sb_puts(&b, "; HttpOnly");
        }
    }
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        dyn_sb_free(&b);
        goto done0;
    }
    ret = JS_NewStringLen(ctx, b.p, b.n);
    dyn_sb_free(&b);
done0:
    JS_FreeCString(ctx, nm);
    JS_FreeCString(ctx, val);
    return ret;
}

static JSValue dyn_hm_etag_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *h, *e_owned, *e;
    size_t hn, en, i = 0;
    int hit = 0;

    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "ETagMatch(header, etag): two strings required");
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    e_owned = JS_ToCStringLen(ctx, &en, argv[1]);
    if (!e_owned) {
        JS_FreeCString(ctx, h);
        return JS_EXCEPTION;
    }
    e = e_owned;
    if (en == 0) {
        JS_FreeCString(ctx, h);
        JS_FreeCString(ctx, e_owned);
        return JS_NewBool(ctx, 0);
    }
    if (en > 2 && e[0] == 'W' && e[1] == '/') {
        e += 2;
        en -= 2;
    }
    while (i < hn && !hit) {
        size_t eb = i, ee;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        dyn_hm_trim(h, &eb, &ee);
        if (ee - eb == 1 && h[eb] == '*') {
            hit = 1;
            break;
        }
        if (ee - eb > 2 && h[eb] == 'W' && h[eb + 1] == '/')
            eb += 2;
        if (ee - eb == en && memcmp(h + eb, e, en) == 0)
            hit = 1;
    }
    JS_FreeCString(ctx, h);
    JS_FreeCString(ctx, e_owned);
    return JS_NewBool(ctx, hit);
}

static const JSCFunctionListEntry dyn_httpmsg_funcs[] = {
    JS_CFUNC_DEF("ContentTypeParse", 1, dyn_hm_ctype_parse),
    JS_CFUNC_DEF("ContentTypeFormat", 1, dyn_hm_ctype_format),
    JS_CFUNC_MAGIC_DEF("Negotiate", 2, dyn_hm_negotiate, 0),
    JS_CFUNC_MAGIC_DEF("NegotiateToken", 2, dyn_hm_negotiate, 1),
    JS_CFUNC_DEF("RangeParse", 2, dyn_hm_range),
    JS_CFUNC_DEF("CookieParse", 1, dyn_hm_cookie_parse),
    JS_CFUNC_DEF("CookieSerialize", 2, dyn_hm_cookie_serialize),
    JS_CFUNC_DEF("ETagMatch", 2, dyn_hm_etag_match),
    JS_CFUNC_DEF("MultipartParse", 2, dyn_mp_parse),
    JS_CFUNC_DEF("MultipartFormat", 1, dyn_mp_format),
};

static JSClassID dyn_httpmsg_holder_id;

static const JSClassDef dyn_httpmsg_holder_class = { "HTTPMessageFunctions" };

int dyn_httpmsg_register(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue holder;
    size_t i;
    int rc = 0;

    JS_NewClassID(&dyn_httpmsg_holder_id);
    if (!JS_IsRegisteredClass(rt, dyn_httpmsg_holder_id)
        && JS_NewClass(rt, dyn_httpmsg_holder_id, &dyn_httpmsg_holder_class) < 0)
        return -1;
    holder = JS_GetClassProto(ctx, dyn_httpmsg_holder_id);
    if (!JS_IsObject(holder)) {
        JS_FreeValue(ctx, holder);
        holder = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(holder))
            return -1;
        JS_SetPropertyFunctionList(ctx, holder, dyn_httpmsg_funcs,
            countof(dyn_httpmsg_funcs));
        JS_SetClassProto(ctx, dyn_httpmsg_holder_id, JS_DupValue(ctx, holder));
    }
    for (i = 0; i < countof(dyn_httpmsg_funcs) && rc == 0; i++) {
        JSValue v = JS_GetPropertyStr(ctx, holder, dyn_httpmsg_funcs[i].name);
        if (JS_IsException(v) || JS_SetModuleExport(ctx, m, dyn_httpmsg_funcs[i].name, v) < 0)
            rc = -1;
    }
    JS_FreeValue(ctx, holder);
    return rc;
}

void dyn_httpmsg_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExportList(ctx, m, dyn_httpmsg_funcs, countof(dyn_httpmsg_funcs));
}
#endif
