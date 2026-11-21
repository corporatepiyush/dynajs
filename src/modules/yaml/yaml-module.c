#include "yaml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

int yml_marker_at(yml_t* p, uint32_t li, const char* m)
{
    const yml_line_t* L;
    size_t k;
    if (li >= p->nln)
        return 0;
    L = &p->ln[li];
    if (L->start != L->raw || L->end - L->start < 3
        || memcmp(p->s + L->start, m, 3) != 0)
        return 0;
    k = L->start + 3;
    return k >= L->end || p->s[k] == ' ' || p->s[k] == '\t';
}

static JSValue yml_bigint_call(JSContext* ctx, JSValueConst ctor, const char* s, size_t n)
{
    JSValue sv = JS_NewStringLen(ctx, s, n), r;
    if (JS_IsException(sv))
        return sv;
    r = JS_Call(ctx, ctor, JS_UNDEFINED, 1, (JSValueConst*)&sv);
    JS_FreeValue(ctx, sv);
    return r;
}

static JSValue yml_wide_int(JSContext* ctx, const char* s, size_t n)
{
    int neg = s[0] == '-';
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    JSValue g = JS_GetGlobalObject(ctx), ctor, r;
    ctor = JS_GetPropertyStr(ctx, g, "BigInt");
    JS_FreeValue(ctx, g);
    if (JS_IsException(ctor))
        return ctor;
    r = yml_bigint_call(ctx, ctor, s + i, n - i);
    if (neg && !JS_IsException(r)) {
        size_t dl;
        const char* d = JS_ToCStringLen(ctx, &dl, r);
        char* t;
        JS_FreeValue(ctx, r);
        if (!d) {
            JS_FreeValue(ctx, ctor);
            return JS_EXCEPTION;
        }
        t = (char*)js_malloc(ctx, dl + 2);
        if (!t) {
            JS_FreeCString(ctx, d);
            JS_FreeValue(ctx, ctor);
            return JS_EXCEPTION;
        }
        t[0] = '-';
        memcpy(t + 1, d, dl);
        JS_FreeCString(ctx, d);
        r = yml_bigint_call(ctx, ctor, t, dl + 1);
        js_free(ctx, t);
    }
    JS_FreeValue(ctx, ctor);
    return r;
}

JSValue yml_scalar(yml_t* p, const char* s, size_t n)
{
    JSContext* ctx = p->ctx;
    int64_t iv;

    if (n != 0) {
        switch (s[0]) {
        case '~':
        case 'n':
        case 'N':
        case 't':
        case 'T':
        case 'f':
        case 'F':
        case '.':
        case '+':
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            break;
        default:
            return JS_NewStringLen(ctx, s, n);
        }
    }

    if (n == 0 || yml_eq(s, n, "~") || yml_eq(s, n, "null")
        || yml_eq(s, n, "Null") || yml_eq(s, n, "NULL"))
        return JS_NULL;
    if (yml_eq(s, n, "true") || yml_eq(s, n, "True") || yml_eq(s, n, "TRUE"))
        return JS_TRUE;
    if (yml_eq(s, n, "false") || yml_eq(s, n, "False") || yml_eq(s, n, "FALSE"))
        return JS_FALSE;
    if (yml_eq(s, n, ".inf") || yml_eq(s, n, ".Inf") || yml_eq(s, n, ".INF")
        || yml_eq(s, n, "+.inf") || yml_eq(s, n, "+.Inf") || yml_eq(s, n, "+.INF"))
        return JS_NewFloat64(ctx, DYN_INFINITY);
    if (yml_eq(s, n, "-.inf") || yml_eq(s, n, "-.Inf") || yml_eq(s, n, "-.INF"))
        return JS_NewFloat64(ctx, -DYN_INFINITY);
    if (yml_eq(s, n, ".nan") || yml_eq(s, n, ".NaN") || yml_eq(s, n, ".NAN"))
        return JS_NewFloat64(ctx, DYN_NAN);
    if (yml_is_int(s, n, &iv)) {
        if (p->bigint && (iv > 9007199254740991LL || iv < -9007199254740991LL))
            return JS_NewBigInt64(ctx, iv);
        return JS_NewInt64(ctx, iv);
    }
    if (n <= 4096 && yml_is_int_syntax(s, n))
        return p->bigint ? yml_wide_int(ctx, s, n) : dyn_bigint_to_number(ctx, yml_wide_int(ctx, s, n));
    if (yml_is_float(s, n)) {
        JSValue sv = JS_NewStringLen(ctx, s, n), r;
        double d;
        if (JS_IsException(sv))
            return sv;
        r = JS_ToFloat64(ctx, &d, sv) < 0 ? JS_EXCEPTION : JS_NewFloat64(ctx, d);
        JS_FreeValue(ctx, sv);
        return r;
    }
    return JS_NewStringLen(ctx, s, n);
}

static int yml_tail(yml_t* p, size_t i, size_t end)
{
    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t'))
        i++;
    if (i >= end || p->s[i] == '#')
        return 0;
    yml_fail(p, "unexpected content after the value");
    return -1;
}

JSValue yml_inline_value(yml_t* p, int indent, size_t st, size_t end)
{
    const char* bad;

    while (st < end && (p->s[st] == ' ' || p->s[st] == '\t'))
        st++;
    if (st >= end)
        return JS_NULL;
    if (p->s[st] == '|' || p->s[st] == '>')
        return yml_block_scalar(p, indent, st, end);
    bad = yml_unsupported(p->s + st, end - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad, (unsigned)(p->li + 1));
        return JS_EXCEPTION;
    }
    if (p->s[st] == '[' || p->s[st] == '{') {
        size_t i = st;
        JSValue v = yml_flow(p, &i, end);
        if (!JS_IsException(v) && yml_tail(p, i, end) < 0) {
            JS_FreeValue(p->ctx, v);
            return JS_EXCEPTION;
        }
        p->li++;
        return v;
    }
    if (p->s[st] == '"' || p->s[st] == '\'') {
        size_t i = st;
        JSValue v = yml_quoted(p, &i, end, 1);
        if (!JS_IsException(v) && yml_tail(p, i, p->ln[p->li].end) < 0) {
            JS_FreeValue(p->ctx, v);
            return JS_EXCEPTION;
        }
        p->li++;
        return v;
    }
    {
        size_t e = yml_plain_end(p->s, st, end);
        JSValue v = yml_scalar(p, p->s + st, e - st);
        p->li++;
        return v;
    }
}

static JSValue yml_seq(yml_t* p, int indent)
{
    JSValue out = JS_NewArray(p->ctx);
    uint32_t k = 0;

    if (JS_IsException(out))
        return out;
    for (;;) {
        size_t st, end, vs;
        JSValue v;

        yml_skip_blank(p);
        if (p->li >= p->nln || p->ln[p->li].indent < indent)
            break;
        if (yml_tab_refuse(p, p->li)) {
            JS_FreeValue(p->ctx, out);
            return JS_EXCEPTION;
        }
        st = p->ln[p->li].start;
        end = p->ln[p->li].end;
        if (p->s[st] != '-' || (st + 1 < end && p->s[st + 1] != ' '))
            break;
        if (p->ln[p->li].indent > indent) {
            JS_FreeValue(p->ctx, out);
            return yml_fail(p, "unexpected indentation");
        }
        vs = st + 1;
        while (vs < end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
            vs++;
        if (vs >= end || p->s[vs] == '#') {
            p->li++;
            v = yml_node(p, indent + 1);
        } else {
            p->ln[p->li].start = vs;
            p->ln[p->li].indent = (int)(vs - p->ln[p->li].raw);
            v = yml_node(p, p->ln[p->li].indent);
        }
        if (JS_IsException(v)
            || JS_DefinePropertyValueUint32(p->ctx, out, k++, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(p->ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

JSValue yml_node(yml_t* p, int min_indent)
{
    int ind;
    JSValue v;

    yml_skip_blank(p);
    if (p->li >= p->nln)
        return JS_NULL;
    if (yml_tab_refuse(p, p->li))
        return JS_EXCEPTION;
    ind = p->ln[p->li].indent;
    if (ind < min_indent)
        return JS_NULL;
    if (yml_marker_at(p, p->li, "---") || yml_marker_at(p, p->li, "..."))
        return JS_NULL;
    if (p->depth >= p->max_depth)
        return yml_fail(p, "nesting exceeds the depth limit");
    p->depth++;
    {
        size_t st = p->ln[p->li].start, end = p->ln[p->li].end;
        if (p->s[st] == '-' && (st + 1 >= end || p->s[st + 1] == ' '))
            v = yml_seq(p, ind);
        else if (yml_key_colon(p->s, st, end) != (size_t)-1)
            v = yml_map(p, ind);
        else
            v = yml_inline_value(p, ind - 1, st, end);
    }
    p->depth--;
    return v;
}

JSValue yml_one(yml_t* p)
{
    yml_skip_blank(p);
    if (yml_at_doc_start(p)) {
        yml_line_t* L = &p->ln[p->li];
        size_t vs = L->start + 3;
        while (vs < L->end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
            vs++;
        if (vs < L->end && p->s[vs] != '#') {
            L->start = vs;
            L->indent = (int)(vs - L->raw);
            return yml_node(p, 0);
        }
        p->li++;
    }
    return yml_node(p, 0);
}

JSClassID ystream_class_id;

static const JSClassDef ystream_class = {
    "YamlDocStream",
    .finalizer = ystream_finalizer,
};

static const JSCFunctionListEntry ystream_proto[] = {
    JS_CFUNC_DEF("next", 0, ystream_next),
    JS_CFUNC_DEF("return", 0, ystream_return),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ystream_iterator),
};

static const JSCFunctionListEntry dyn_yaml_funcs[] = {
    JS_CFUNC_MAGIC_DEF("Parse", 1, yml_parse, 0),
    JS_CFUNC_MAGIC_DEF("ParseAll", 1, yml_parse, 1),
    JS_CFUNC_DEF("ParseStream", 1, yml_parse_stream),
    JS_CFUNC_DEF("Stringify", 1, yml_stringify),
};

static int dyn_yaml_init_module(JSContext* ctx, JSModuleDef* m)
{
    JS_NewClassID(&ystream_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), ystream_class_id, &ystream_class) < 0)
        return -1;
    {
        JSValue proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, ystream_proto,
            countof(ystream_proto));
        JS_SetClassProto(ctx, ystream_class_id, proto);
    }
    return JS_SetModuleExportList(ctx, m, dyn_yaml_funcs, countof(dyn_yaml_funcs));
}

int js_nat_init_yaml(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:yaml", dyn_yaml_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_yaml_funcs, countof(dyn_yaml_funcs));
}

#endif
