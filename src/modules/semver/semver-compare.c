#include "semver-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

int sv_two(JSContext* ctx, int argc, JSValueConst* argv, int* cmp)
{
    const char *as, *bs;
    size_t an, bn;
    SemVer a, b;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "dyna:semver: expected two version strings");
        return -1;
    }
    as = JS_ToCStringLen(ctx, &an, argv[0]);
    if (!as)
        return -1;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) {
        JS_FreeCString(ctx, as);
        return -1;
    }
    if (semver_parse(as, an, &a) || semver_parse(bs, bn, &b)) {
        JS_FreeCString(ctx, as);
        JS_FreeCString(ctx, bs);
        JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
        return -1;
    }
    *cmp = semver_compare(&a, &b);
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return 0;
}

JSValue dyn_semver_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int c;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, c);
}

int sv_two_versions(JSContext* ctx, int argc, JSValueConst* argv,
    SemVer* a, SemVer* b,
    const char** pas, const char** pbs)
{
    const char *as, *bs;
    size_t an, bn;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx, "dyna:semver: expected two version strings");
        return -1;
    }
    as = JS_ToCStringLen(ctx, &an, argv[0]);
    if (!as)
        return -1;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) {
        JS_FreeCString(ctx, as);
        return -1;
    }
    if (semver_parse(as, an, a) || semver_parse(bs, bn, b)) {
        JS_FreeCString(ctx, as);
        JS_FreeCString(ctx, bs);
        JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
        return -1;
    }
    *pas = as;
    *pbs = bs;
    return 0;
}

JSValue dyn_semver_compare_build(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    SemVer a, b;
    const char *as, *bs;
    int c;
    (void)this_val;
    if (sv_two_versions(ctx, argc, argv, &a, &b, &as, &bs))
        return JS_EXCEPTION;
    c = semver_compare(&a, &b);
    if (c == 0) {
        if (a.n_build == 0 && b.n_build == 0)
            c = 0;
        else if (a.n_build == 0)
            c = -1;
        else if (b.n_build == 0)
            c = 1;
        else
            c = sv_cmp_pre(a.build, a.n_build, b.build, b.n_build);
    }
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return JS_NewInt32(ctx, c);
}

int sv_read_versions(JSContext* ctx, JSValueConst arr, JSValue** pelems,
    const char*** pcstrs, SemVer** psvs, uint32_t* pcount,
    int allow_invalid)
{
    JSValue lval, *elems = NULL;
    const char** cstrs = NULL;
    SemVer* svs = NULL;
    uint32_t len, i, filled = 0, cap = 0;
    int isarr = JS_IsArray(ctx, arr);

    if (isarr < 0)
        return -1;
    if (!isarr) {
        JS_ThrowTypeError(ctx, "dyna:semver: expected an Array of versions");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lval))
        return -1;
    if (JS_ToUint32(ctx, &len, lval)) {
        JS_FreeValue(ctx, lval);
        return -1;
    }
    JS_FreeValue(ctx, lval);

#if UINTPTR_MAX == UINT32_MAX
    if (len > 0 && (size_t)len > ((size_t)-1) / sizeof(SemVer)) {
        JS_ThrowRangeError(ctx, "dyna:semver: array too large");
        return -1;
    }
#endif
    for (i = 0; i < len; i++) {
        size_t slen;
        JSValue e;
        if (i == cap) {
            uint32_t ncap = cap ? (cap > len - cap ? len : cap * 2) : (len < 16 ? len : 16);
            JSValue* ne = js_realloc(ctx, elems, (size_t)ncap * sizeof(*elems));
            const char** nc;
            SemVer* ns;
            if (!ne)
                goto fail;
            elems = ne;
            nc = js_realloc(ctx, cstrs, (size_t)ncap * sizeof(*cstrs));
            if (!nc)
                goto fail;
            cstrs = nc;
            ns = js_realloc(ctx, svs, (size_t)ncap * sizeof(*svs));
            if (!ns)
                goto fail;
            svs = ns;
            cap = ncap;
        }
        e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e))
            goto fail;
        elems[i] = e;
        cstrs[i] = NULL;
        filled = i + 1;
        if (!JS_IsString(e)) {
            JS_ThrowTypeError(ctx,
                "dyna:semver: every array element must be a string");
            goto fail;
        }
        cstrs[i] = JS_ToCStringLen(ctx, &slen, e);
        if (!cstrs[i])
            goto fail;
        if (semver_parse(cstrs[i], slen, &svs[i])) {
            if (!allow_invalid) {
                JS_ThrowTypeError(ctx, "dyna:semver: invalid version in array "
                                       "at index %u",
                    i);
                goto fail;
            }
            svs[i].n_pre = -1;
        }
    }
    *pelems = elems;
    *pcstrs = cstrs;
    *psvs = svs;
    *pcount = len;
    return 0;

fail:
    for (i = 0; i < filled; i++) {
        if (cstrs && cstrs[i])
            JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems);
    js_free(ctx, cstrs);
    js_free(ctx, svs);
    return -1;
}

JSValue dyn_semver_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue *elems, out;
    const char** cstrs;
    SemVer* svs;
    uint32_t count, *idx, *tmp, i;

    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: sort expects an Array of versions");
    }
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 0))
        return JS_EXCEPTION;

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        return JS_EXCEPTION;
    }
    if (count > 0) {
        idx = js_malloc(ctx, (size_t)count * sizeof(*idx));
        tmp = js_malloc(ctx, (size_t)count * sizeof(*tmp));
        if (!idx || !tmp) {
            js_free(ctx, idx);
            js_free(ctx, tmp);
            JS_FreeValue(ctx, out);
            sv_free_versions(ctx, elems, cstrs, svs, count);
            return JS_EXCEPTION;
        }
        for (i = 0; i < count; i++)
            idx[i] = i;
        semverk_sort_indices(svs, idx, tmp, count);

        for (i = 0; i < count; i++) {
            JSValue e = elems[idx[i]];
            elems[idx[i]] = JS_UNDEFINED;
            if (JS_DefinePropertyValueUint32(ctx, out, i, e,
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, e);
                js_free(ctx, idx);
                js_free(ctx, tmp);
                JS_FreeValue(ctx, out);
                sv_free_versions(ctx, elems, cstrs, svs, count);
                return JS_EXCEPTION;
            }
        }
        js_free(ctx, idx);
        js_free(ctx, tmp);
    }
    sv_free_versions(ctx, elems, cstrs, svs, count);
    return out;
}

JSValue dyn_semver_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max)
{
    JSValue *elems, out;
    const char** cstrs;
    SemVer* svs;
    const char* rs;
    size_t rn;
    Range* rg;
    uint32_t count, i;
    long best = -1;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: expected (Array, rangeString)");
    }
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs)
        return JS_EXCEPTION;
    if (sv_read_versions(ctx, argv[0], &elems, &cstrs, &svs, &count, 1)) {
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    rg = js_malloc(ctx, sizeof(*rg));
    if (!rg) {
        sv_free_versions(ctx, elems, cstrs, svs, count);
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    if (semverk_parse_range(rs, rn, rg)) {
        js_free(ctx, rg);
        sv_free_versions(ctx, elems, cstrs, svs, count);
        JS_FreeCString(ctx, rs);
        return JS_NULL;
    }

    for (i = 0; i < count; i++) {
        if (!semverk_range_test(&svs[i], rg))
            continue;
        if (best < 0) {
            best = (long)i;
            continue;
        }
        {
            int c = semver_compare(&svs[i], &svs[best]);
            if (want_max ? (c > 0) : (c < 0))
                best = (long)i;
        }
    }
    out = (best < 0) ? JS_NULL : JS_DupValue(ctx, elems[best]);
    sv_range_release(rg);

    js_free(ctx, rg);
    sv_free_versions(ctx, elems, cstrs, svs, count);
    JS_FreeCString(ctx, rs);
    return out;
}

JSValue dyn_semver_coerce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i;
    JSValue result = JS_NULL;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;

    for (i = 0; i < n; i++) {
        size_t j, k;
        int hm = 0, hp = 0;
        size_t ms = 0, ml = 0, ps = 0, pl = 0;
        char buf[64];
        size_t o = 0;
        SemVer sv;

        if (!sv_is_digit(s[i]) || (i > 0 && sv_is_digit(s[i - 1])))
            continue;
        j = i;
        while (j < n && sv_is_digit(s[j]))
            j++;
        if (j - i > 16) {
            i = j - 1;
            continue;
        }

        k = j;
        if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
            size_t a = k + 1, b = a;
            while (b < n && sv_is_digit(s[b]))
                b++;
            if (b - a <= 16) {
                hm = 1;
                ms = a;
                ml = b - a;
                k = b;
                if (k < n && s[k] == '.' && k + 1 < n && sv_is_digit(s[k + 1])) {
                    size_t c = k + 1, d = c;
                    while (d < n && sv_is_digit(s[d]))
                        d++;
                    if (d - c <= 16) {
                        hp = 1;
                        ps = c;
                        pl = d - c;
                    }
                }
            }
        }

        memcpy(buf + o, s + i, j - i);
        o += j - i;
        buf[o++] = '.';
        if (hm) {
            memcpy(buf + o, s + ms, ml);
            o += ml;
        } else
            buf[o++] = '0';
        buf[o++] = '.';
        if (hp) {
            memcpy(buf + o, s + ps, pl);
            o += pl;
        } else
            buf[o++] = '0';

        if (semver_parse(buf, o, &sv) == 0)
            result = JS_NewStringLen(ctx, buf, o);
        break;
    }
    JS_FreeCString(ctx, s);
    return result;
}

#endif
