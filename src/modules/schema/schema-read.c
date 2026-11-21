#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

int dyn_sc_read_num(JSContext* ctx, JSValueConst v, const char* kw,
    double* out)
{
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, out, v)) {
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be a number", kw);
        return -1;
    }
    if (!isfinite(*out)) {
        JS_ThrowRangeError(ctx, "dyna:schema: \"%s\" must be finite", kw);
        return -1;
    }
    return 0;
}

int dyn_sc_read_int(JSContext* ctx, JSValueConst v, const char* kw,
    int* out)
{
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || d < 0 || d != trunc(d) || d > 2147483647.0 || isnan(d) || isinf(d)) {
        JS_ThrowTypeError(ctx,
            "dyna:schema: \"%s\" must be a non-negative integer", kw);
        return -1;
    }
    *out = (int)d;
    return 0;
}

int dyn_sc_read_map(JSContext* ctx, JSValueConst v, const char* kw,
    char*** pnames, JSValue** pvals, int* pn)
{
    JSPropertyEnum* tab = NULL;
    uint32_t n, i;
    char** names;
    JSValue* vals;

    if (!JS_IsObject(v) || dyn_sc_is_array(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be an object", kw);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
            JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
        < 0)
        return -1;
    names = js_mallocz(ctx, (n ? n : 1) * sizeof(*names));
    vals = js_mallocz(ctx, (n ? n : 1) * sizeof(*vals));
    if (!names || !vals) {
        JS_FreePropertyEnum(ctx, tab, n);
        js_free(ctx, names);
        js_free(ctx, vals);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue name = JS_AtomToString(ctx, tab[i].atom);
        const char* nb;
        size_t nl;
        if (JS_IsException(name))
            goto fail;
        nb = JS_ToCStringLen(ctx, &nl, name);
        if (!nb) {
            JS_FreeValue(ctx, name);
            goto fail;
        }
        names[i] = js_malloc(ctx, nl + 1);
        if (!names[i]) {
            JS_FreeCString(ctx, nb);
            JS_FreeValue(ctx, name);
            goto fail;
        }
        memcpy(names[i], nb, nl);
        names[i][nl] = '\0';
        JS_FreeCString(ctx, nb);
        JS_FreeValue(ctx, name);
        vals[i] = JS_GetProperty(ctx, v, tab[i].atom);
        if (JS_IsException(vals[i]))
            goto fail;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    *pnames = names;
    *pvals = vals;
    *pn = (int)n;
    return 0;
fail:
    JS_FreePropertyEnum(ctx, tab, n);
    for (i = 0; i < n; i++) {
        js_free(ctx, names[i]);
        JS_FreeValue(ctx, vals[i]);
    }
    js_free(ctx, names);
    js_free(ctx, vals);
    return -1;
}

static int dyn_sc_plain_name(const char* s)
{
    const char* p = s;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return 0;
    for (p++; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_'))
            return 0;
    return 1;
}

int dyn_sc_read_anchors(dyn_sccomp_t* c, JSValueConst sv, dyn_scnode_t* n)
{
    static const char* const kws[2] = { "$anchor", "$dynamicAnchor" };
    int k;
    for (k = 0; k < 2; k++) {
        char* name;
        int got = dyn_sc_str_kw(c->ctx, sv, kws[k], &name);
        if (got < 0)
            return -1;
        if (!got)
            continue;
        if (!dyn_sc_plain_name(name)) {
            JS_ThrowTypeError(c->ctx, "dyna:schema: \"%s\" \"%s\" is not a plain name ([A-Za-z_][-A-Za-z0-9._]*)", kws[k], name);
            js_free(c->ctx, name);
            return -1;
        }
        if (dyn_sc_res_anchor_add(c, c->cur, name, n, k)) {
            js_free(c->ctx, name);
            return -1;
        }
        js_free(c->ctx, name);
    }
    return 0;
}

int dyn_sc_read_options(JSContext* ctx, JSValueConst opts, dyn_sccomp_t* c, dyn_schema_t* sch)
{
    static const char* const keys[] = { "resources", "baseUri", "formats" };
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    char* s;
    int got;
    size_t k;

    sch->fmt_mode = SC_FMT_REGISTERED;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "Schema.compile: options must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        int known = 0;
        if (!name) {
            JS_FreePropertyEnum(ctx, props, nprops);
            return -1;
        }
        for (k = 0; k < countof(keys); k++)
            known |= !strcmp(name, keys[k]);
        if (!known) {
            JS_ThrowTypeError(ctx, "Schema.compile: unknown option \"%s\" (valid: resources, baseUri, formats)", name);
            JS_FreeCString(ctx, name);
            JS_FreePropertyEnum(ctx, props, nprops);
            return -1;
        }
        JS_FreeCString(ctx, name);
    }
    JS_FreePropertyEnum(ctx, props, nprops);
    c->resources = JS_GetPropertyStr(ctx, opts, "resources");
    if (JS_IsException(c->resources)) {
        c->resources = JS_UNDEFINED;
        return -1;
    }
    if (!JS_IsUndefined(c->resources) && (!JS_IsObject(c->resources) || dyn_sc_is_array(ctx, c->resources))) {
        JS_ThrowTypeError(ctx, "Schema.compile: resources must be an object mapping URI -> schema");
        return -1;
    }
    got = dyn_sc_str_kw(ctx, opts, "baseUri", &s);
    if (got < 0)
        return -1;
    if (got) {
        if (strchr(s, '#') || !strchr(s, ':')) {
            JS_ThrowTypeError(ctx, "Schema.compile: baseUri must be an absolute URI without a fragment (got \"%s\")", s);
            js_free(ctx, s);
            return -1;
        }
        c->base_owned = s;
        c->base_uri = s;
    }
    got = dyn_sc_str_kw(ctx, opts, "formats", &s);
    if (got < 0)
        return -1;
    if (got) {
        if (!strcmp(s, "annotation"))
            sch->fmt_mode = SC_FMT_ANNOTATION;
        else if (!strcmp(s, "assertion"))
            sch->fmt_mode = SC_FMT_ASSERTION;
        else if (strcmp(s, "registered")) {
            JS_ThrowTypeError(ctx, "Schema.compile: formats must be \"registered\", \"annotation\" or \"assertion\" (got \"%s\")", s);
            js_free(ctx, s);
            return -1;
        }
        js_free(ctx, s);
    }
    return 0;
}

#endif
