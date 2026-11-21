#include "semver-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

enum { FLD_MAJOR,
    FLD_MINOR,
    FLD_PATCH,
    FLD_PRERELEASE };

enum { CMP_GT,
    CMP_GTE,
    CMP_LT,
    CMP_LTE,
    CMP_EQ,
    CMP_NEQ };

JSValue sv_build_version(JSContext* ctx, const SemVer* v)
{
    size_t cap = 3 * 20 + 2 + 1, o = 0;
    int i;
    char* buf;
    JSValue r;
    for (i = 0; i < v->n_pre; i++)
        cap += v->pre[i].len + 1;
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
            memcpy(buf + o, v->pre[i].s, v->pre[i].len);
            o += v->pre[i].len;
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return r;
}

static JSValue sv_id_number(JSContext* ctx, const SemVerId* id)
{
    size_t k;
    if (id->len <= 18) {
        uint64_t x = 0;
        for (k = 0; k < id->len; k++)
            x = x * 10 + (uint64_t)(id->s[k] - '0');
        if (x < SV_MAX_SAFE)
            return JS_NewInt64(ctx, (int64_t)x);
        return JS_NewStringLen(ctx, id->s, id->len);
    }
    {
        double d = 0;
        for (k = 0; k < id->len; k++)
            d = d * 10 + (double)(id->s[k] - '0');
        return JS_NewFloat64(ctx, d);
    }
}

JSValue sv_pre_array(JSContext* ctx, const SemVer* v)
{
    JSValue arr = JS_NewArray(ctx);
    int i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < v->n_pre; i++) {
        JSValue item = v->pre[i].numeric
            ? sv_id_number(ctx, &v->pre[i])
            : JS_NewStringLen(ctx, v->pre[i].s, v->pre[i].len);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, item,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_semver_is_valid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    SemVer v;
    int ok;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewBool(ctx, 0);
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NewBool(ctx, 0);
    }
    ok = semver_parse(s, n, &v) == 0;
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_semver_clean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *s, *p;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    {
        size_t lead = sv_ws_lead(s, n), trail;
        p = s + lead;
        n -= lead;
        trail = sv_ws_trail(p, n);
        n -= trail;
    }
    while (n > 0 && (p[0] == '=' || p[0] == 'v')) {
        p++;
        n--;
    }
    r = (semver_parse(p, n, &v) == 0) ? sv_build_version(ctx, &v) : JS_NULL;
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue dyn_semver_field(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n;
    SemVer v;
    JSValue r;

    (void)this_val;
    s = sv_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (semver_parse(s, n, &v)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
    }
    switch (magic) {
    case FLD_MAJOR:
        r = JS_NewInt64(ctx, (int64_t)v.major);
        break;
    case FLD_MINOR:
        r = JS_NewInt64(ctx, (int64_t)v.minor);
        break;
    case FLD_PATCH:
        r = JS_NewInt64(ctx, (int64_t)v.patch);
        break;
    default:
        r = (v.n_pre > 0) ? sv_pre_array(ctx, &v) : JS_NULL;
        break;
    }
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue dyn_semver_cmpbool(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int c, r;
    (void)this_val;
    if (sv_two(ctx, argc, argv, &c))
        return JS_EXCEPTION;
    switch (magic) {
    case CMP_GT:
        r = c > 0;
        break;
    case CMP_GTE:
        r = c >= 0;
        break;
    case CMP_LT:
        r = c < 0;
        break;
    case CMP_LTE:
        r = c <= 0;
        break;
    case CMP_EQ:
        r = c == 0;
        break;
    default:
        r = c != 0;
        break;
    }
    return JS_NewBool(ctx, r);
}

static JSValue dyn_semver_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    SemVer a, b;
    const char *as, *bs;
    (void)this_val;
    if (sv_two_versions(ctx, argc, argv, &a, &b, &as, &bs))
        return JS_EXCEPTION;
    const char* which = NULL;
    if (a.major != b.major)
        which = "major";
    else if (a.minor != b.minor)
        which = "minor";
    else if (a.patch != b.patch)
        which = "patch";
    else if (semver_compare(&a, &b) != 0) {

        which = "prerelease";
    } else {
        int i;
        int build_eq = a.n_build == b.n_build;
        for (i = 0; build_eq && i < a.n_build; i++)
            build_eq = a.build[i].len == b.build[i].len && memcmp(a.build[i].s, b.build[i].s, a.build[i].len) == 0;
        if (!build_eq)
            which = "build";
    }
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    return which ? JS_NewString(ctx, which) : JS_NULL;
}

static JSValue dyn_semver_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void sv_free_versions(JSContext* ctx, JSValue* elems,
    const char** cstrs, SemVer* svs, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (cstrs && cstrs[i])
            JS_FreeCString(ctx, cstrs[i]);
        JS_FreeValue(ctx, elems[i]);
    }
    js_free(ctx, elems);
    js_free(ctx, cstrs);
    js_free(ctx, svs);
}

static JSValue dyn_semver_satisfies(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *vs, *rs;
    size_t vn, rn;
    SemVer v;
    Range* rg;
    int res;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx,
            "dyna:semver: satisfies(version, range) expects two strings");
    }
    vs = JS_ToCStringLen(ctx, &vn, argv[0]);
    if (!vs)
        return JS_EXCEPTION;
    rs = JS_ToCStringLen(ctx, &rn, argv[1]);
    if (!rs) {
        JS_FreeCString(ctx, vs);
        return JS_EXCEPTION;
    }

    if (semver_parse(vs, vn, &v)) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_ThrowTypeError(ctx, "dyna:semver: invalid version");
    }
    rg = js_malloc(ctx, sizeof(*rg));
    if (!rg) {
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_EXCEPTION;
    }
    if (semverk_parse_range(rs, rn, rg)) {
        js_free(ctx, rg);
        JS_FreeCString(ctx, vs);
        JS_FreeCString(ctx, rs);
        return JS_NewBool(ctx, 0);
    }
    res = semverk_range_test(&v, rg);
    sv_range_release(rg);
    js_free(ctx, rg);
    JS_FreeCString(ctx, vs);
    JS_FreeCString(ctx, rs);
    return JS_NewBool(ctx, res);
}

JSClassID dyn_range_class_id;

static const JSClassDef dyn_range_class = {
    "Range",
    .finalizer = dyn_range_finalizer,
};

static JSValue dyn_semver_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const Range *ra, *rb;
    Range *ha, *hb;
    const char *own_a, *own_b;
    int flag_a, flag_b, res;
    (void)this_val;

    if (sv_range_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            &ra, &own_a, &flag_a, &ha))
        return JS_EXCEPTION;
    if (sv_range_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &rb, &own_b, &flag_b, &hb)) {
        if (flag_a)
            JS_FreeCString(ctx, own_a);
        sv_range_free(ha);
        return JS_EXCEPTION;
    }
    res = sv_range_intersects(ra, rb);
    if (flag_a)
        JS_FreeCString(ctx, own_a);
    if (flag_b)
        JS_FreeCString(ctx, own_b);
    sv_range_free(ha);
    sv_range_free(hb);
    return JS_NewBool(ctx, res);
}

static const JSCFunctionListEntry dyn_range_proto[] = {
    JS_CFUNC_DEF("test", 1, dyn_range_test),
    JS_CFUNC_MAGIC_DEF("maxSatisfying", 1, dyn_range_bestsat, 1),
    JS_CFUNC_MAGIC_DEF("minSatisfying", 1, dyn_range_bestsat, 0),
    JS_CFUNC_DEF("filter", 1, dyn_range_filter),
    JS_CFUNC_DEF("intersects", 1, dyn_range_intersects),
    JS_CGETSET_DEF("source", dyn_range_get_source, NULL),
    JS_CGETSET_DEF("setCount", dyn_range_get_set_count, NULL),
};

static const JSCFunctionListEntry dyn_semver_funcs[] = {
    JS_CFUNC_DEF("parse", 1, dyn_semver_parse),
    JS_CFUNC_DEF("isValid", 1, dyn_semver_is_valid),
    JS_CFUNC_DEF("clean", 1, dyn_semver_clean),
    JS_CFUNC_DEF("compare", 2, dyn_semver_compare),
    JS_CFUNC_MAGIC_DEF("gt", 2, dyn_semver_cmpbool, CMP_GT),
    JS_CFUNC_MAGIC_DEF("gte", 2, dyn_semver_cmpbool, CMP_GTE),
    JS_CFUNC_MAGIC_DEF("lt", 2, dyn_semver_cmpbool, CMP_LT),
    JS_CFUNC_MAGIC_DEF("lte", 2, dyn_semver_cmpbool, CMP_LTE),
    JS_CFUNC_MAGIC_DEF("eq", 2, dyn_semver_cmpbool, CMP_EQ),
    JS_CFUNC_MAGIC_DEF("neq", 2, dyn_semver_cmpbool, CMP_NEQ),
    JS_CFUNC_DEF("sort", 1, dyn_semver_sort),
    JS_CFUNC_MAGIC_DEF("major", 1, dyn_semver_field, FLD_MAJOR),
    JS_CFUNC_MAGIC_DEF("minor", 1, dyn_semver_field, FLD_MINOR),
    JS_CFUNC_MAGIC_DEF("patch", 1, dyn_semver_field, FLD_PATCH),
    JS_CFUNC_MAGIC_DEF("prerelease", 1, dyn_semver_field, FLD_PRERELEASE),
    JS_CFUNC_DEF("inc", 2, dyn_semver_inc),
    JS_CFUNC_DEF("satisfies", 2, dyn_semver_satisfies),
    JS_CFUNC_MAGIC_DEF("maxSatisfying", 2, dyn_semver_bestsat, 1),
    JS_CFUNC_MAGIC_DEF("minSatisfying", 2, dyn_semver_bestsat, 0),
    JS_CFUNC_DEF("coerce", 1, dyn_semver_coerce),
    JS_CFUNC_DEF("diff", 2, dyn_semver_diff),
    JS_CFUNC_DEF("compareBuild", 2, dyn_semver_compare_build),
    JS_CFUNC_DEF("intersects", 2, dyn_semver_intersects),
};

static int dyn_semver_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_range_class_id, &dyn_range_class,
            dyn_range_proto, countof(dyn_range_proto),
            dyn_range_ctor, "Range")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_semver_funcs,
        countof(dyn_semver_funcs));
}

int js_nat_init_semver(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:semver", dyn_semver_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Range");
    return JS_AddModuleExportList(ctx, m, dyn_semver_funcs,
        countof(dyn_semver_funcs));
}

#endif
