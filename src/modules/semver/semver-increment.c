#include "semver-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

static JSValue inc_format(JSContext* ctx, const IncVer* v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char* buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++)
        cap += (v->pre[i].is_num ? 20 : v->pre[i].slen) + 1;
    buf = js_malloc(ctx, cap);
    if (!buf)
        return JS_EXCEPTION;
    o += sv_u64toa(v->major, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->minor, buf + o);
    buf[o++] = '.';
    o += sv_u64toa(v->patch, buf + o);
    if (v->n_pre > 0) {
        buf[o++] = '-';
        for (i = 0; i < v->n_pre; i++) {
            if (i)
                buf[o++] = '.';
            if (v->pre[i].is_num) {
                o += sv_u64toa(v->pre[i].num, buf + o);
            } else {
                memcpy(buf + o, v->pre[i].str, v->pre[i].slen);
                o += v->pre[i].slen;
            }
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return r;
}

JSValue dyn_semver_inc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *vs, *rel_s, *id = NULL;
    size_t vn, rel_n, idn = 0;
    SemVer sv;
    IncVer iv;
    int rel, i, had_pre;
    JSValue r;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: inc(version, release[, identifier]) expects strings");
    }
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsString(argv[2])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: inc identifier must be a string");
    }
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    rel_s = JS_ToCStringLen(ctx, &rel_n, argv[1]);
    if (!rel_s) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }
    if (argc >= 3 && JS_IsString(argv[2])) {
        id = JS_ToCStringLen(ctx, &idn, argv[2]);
        if (!id) {
            JS_FreeCString(ctx, vs);
            JS_FreeCString(ctx, rel_s);
            return JS_EXCEPTION;
        }
    }

    rel = semverk_inc_release_id(rel_s, rel_n);
    if (rel < 0 || semver_parse(vs, vn, &sv)) {
        JSValue e = JS_ThrowTypeError(ctx, rel < 0 ? "dyna:semver: invalid release type" : "dyna:semver: invalid version");
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return e;
    }

    memset(&iv, 0, sizeof(iv));
    iv.major = sv.major;
    iv.minor = sv.minor;
    iv.patch = sv.patch;
    iv.n_pre = sv.n_pre;
    for (i = 0; i < sv.n_pre; i++) {
        if (sv.pre[i].numeric) {
            uint64_t x = 0;
            size_t k;
            for (k = 0; k < sv.pre[i].len && k < 18; k++)
                x = x * 10 + (uint64_t)(sv.pre[i].s[k] - '0');
            iv.pre[i].is_num = 1;
            iv.pre[i].num = x;
        } else {
            iv.pre[i].is_num = 0;
            iv.pre[i].str = sv.pre[i].s;
            iv.pre[i].slen = sv.pre[i].len;
        }
    }
    had_pre = iv.n_pre > 0;
    semverk_inc_apply(&iv, rel, id, idn);
    if (rel == REL_RELEASE && !had_pre) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return JS_NULL;
    }
    if (iv.major > SV_MAX_SAFE || iv.minor > SV_MAX_SAFE || iv.patch > SV_MAX_SAFE) {
        JSValue e = JS_ThrowRangeError(ctx,
            "dyna:semver: inc result exceeds MAX_SAFE_INTEGER");
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rel_s);
        if (id)
            JS_FreeCString(ctx, id);
        return e;
    }
    for (i = 0; i < iv.n_pre; i++) {
        if (iv.pre[i].is_num && iv.pre[i].num > SV_MAX_SAFE) {
            JSValue e = JS_ThrowRangeError(ctx,
                "dyna:semver: inc result exceeds MAX_SAFE_INTEGER");
            JS_FreeCString(ctx, vs);
            JS_FreeCString(ctx, rel_s);
            if (id)
                JS_FreeCString(ctx, id);
            return e;
        }
    }
    r = inc_format(ctx, &iv);

    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rel_s);
    if (id)
        JS_FreeCString(ctx, id);
    return r;
}

#endif
