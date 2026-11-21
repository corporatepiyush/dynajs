#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#define DYN_SC_T_NULL 1
#define DYN_SC_T_BOOL 2
#define DYN_SC_T_INTEGER 64
#define DYN_SC_FMT_NAME_MAX 256
#define DYN_SC_FMTREG_NAME "__dyna_schema_formats"

JSClassID dyn_schema_class_id;

dyn_scnode_t* dyn_sc_new_node(dyn_sccomp_t* c)
{
    dyn_scnode_t* n = js_mallocz(c->ctx, sizeof(*n));
    if (!n)
        return NULL;
    n->val = JS_UNDEFINED;
    n->minlen = n->maxlen = -1;
    n->minitems = n->maxitems = -1;
    n->minprops = n->maxprops = -1;
    n->addl_allow = 1;
    if (c->sch->n_nodes >= c->sch->cap_nodes) {
        int ncap = c->sch->cap_nodes ? c->sch->cap_nodes * 2 : 32;
        dyn_scnode_t** nn = js_realloc(c->ctx, c->sch->nodes,
            (size_t)ncap * sizeof(*nn));
        if (!nn) {
            js_free(c->ctx, n);
            return NULL;
        }
        c->sch->nodes = nn;
        c->sch->cap_nodes = ncap;
    }
    c->sch->nodes[c->sch->n_nodes] = n;
    n->idx = c->sch->n_nodes;
    c->sch->n_nodes++;
    return n;
}

int dyn_sc_type_name(const char* s, size_t n, int* mask)
{
    static const struct {
        const char* name;
        int bit;
    } T[] = {
        { "null", DYN_SC_T_NULL },
        { "boolean", DYN_SC_T_BOOL },
        { "object", DYN_SC_T_OBJECT },
        { "array", DYN_SC_T_ARRAY },
        { "number", DYN_SC_T_NUMBER },
        { "string", DYN_SC_T_STRING },
        { "integer", DYN_SC_T_INTEGER },
    };
    int i;
    for (i = 0; i < (int)countof(T); i++)
        if (strlen(T[i].name) == n && memcmp(T[i].name, s, n) == 0) {
            *mask = T[i].bit;
            return 0;
        }
    return -1;
}

int dyn_sc_str_kw(JSContext* ctx, JSValueConst sv, const char* kw, char** out)
{
    JSValue v;
    const char* s;
    size_t n;
    *out = NULL;
    if (dyn_sc_kw(ctx, sv, kw, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be a string", kw);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *out = js_malloc(ctx, n + 1);
    if (*out) {
        memcpy(*out, s, n);
        (*out)[n] = 0;
    }
    JS_FreeCString(ctx, s);
    return *out ? 1 : -1;
}

static void dyn_sc_node_free_rt(JSRuntime* rt, dyn_scnode_t* n)
{
    int i;
    if (!n)
        return;
    if (n->f & SC_F_CONST)
        JS_FreeValueRT(rt, n->val);
    if (n->f & SC_F_ENUM) {
        for (i = 0; i < n->n_enum; i++)
            JS_FreeValueRT(rt, n->enumv[i]);
        js_free_rt(rt, n->enumv);
    }
    js_free_rt(rt, n->re);
    js_free_rt(rt, n->ref);
    js_free_rt(rt, n->dref);
    js_free_rt(rt, n->dref_name);
    js_free_rt(rt, n->fmt);
    for (i = 0; i < n->props.n; i++)
        js_free_rt(rt, n->props.names[i]);
    js_free_rt(rt, n->props.names);
    js_free_rt(rt, n->props.v);
    for (i = 0; i < n->depsch.n; i++)
        js_free_rt(rt, n->depsch.names[i]);
    js_free_rt(rt, n->depsch.names);
    js_free_rt(rt, n->depsch.v);
    for (i = 0; i < n->n_patts; i++) {
        js_free_rt(rt, n->patts[i].pat);
        js_free_rt(rt, n->patts[i].re);
    }
    js_free_rt(rt, n->patts);
    for (i = 0; i < n->required.n; i++)
        js_free_rt(rt, n->required.names[i]);
    js_free_rt(rt, n->required.names);
    js_free_rt(rt, n->required.v);
    for (i = 0; i < n->depreq.n; i++) {
        int j;
        js_free_rt(rt, n->depreq.props[i]);
        for (j = 0; j < n->depreq.nneed[i]; j++)
            js_free_rt(rt, n->depreq.need[i][j]);
        js_free_rt(rt, n->depreq.need[i]);
    }
    js_free_rt(rt, n->depreq.props);
    js_free_rt(rt, n->depreq.need);
    js_free_rt(rt, n->depreq.nneed);
    js_free_rt(rt, n->prefix.v);
    js_free_rt(rt, n->all.v);
    js_free_rt(rt, n->any.v);
    js_free_rt(rt, n->one.v);
    js_free_rt(rt, n);
}

void dyn_sc_schema_free_rt(JSRuntime* rt, dyn_schema_t* sch)
{
    int i;
    if (!sch)
        return;
    for (i = 0; i < sch->n_nodes; i++)
        dyn_sc_node_free_rt(rt, sch->nodes[i]);
    js_free_rt(rt, sch->nodes);
    for (i = 0; i < sch->n_res; i++)
        dyn_sc_res_free_rt(rt, sch->res[i]);
    js_free_rt(rt, sch->res);
    js_free_rt(rt, sch);
}

static void dyn_sc_schema_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_sc_schema_free_rt(rt, JS_GetOpaque(val, dyn_schema_class_id));
}

static void dyn_sc_schema_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_schema_t* sch = JS_GetOpaque(val, dyn_schema_class_id);
    int i, j;
    if (!sch)
        return;
    for (i = 0; i < sch->n_nodes; i++) {
        dyn_scnode_t* n = sch->nodes[i];
        if (n->f & SC_F_CONST)
            JS_MarkValue(rt, n->val, mark_func);
        if (n->f & SC_F_ENUM)
            for (j = 0; j < n->n_enum; j++)
                JS_MarkValue(rt, n->enumv[j], mark_func);
    }
}

int dyn_sc_inst_type(JSContext* ctx, JSValueConst v)
{
    if (JS_IsNull(v))
        return DYN_SC_T_NULL;
    if (JS_IsBool(v))
        return DYN_SC_T_BOOL;
    if (JS_IsString(v))
        return DYN_SC_T_STRING;
    if (JS_IsNumber(v))
        return DYN_SC_T_NUMBER;
    if (JS_IsObject(v)) {
        if (dyn_sc_is_array(ctx, v))
            return DYN_SC_T_ARRAY;
        if (!JS_IsFunction(ctx, v))
            return DYN_SC_T_OBJECT;
    }
    return 0;
}

int dyn_sc_get_own(JSContext* ctx, JSValueConst obj, JSAtom a,
    JSValue* out)
{
    JSPropertyDescriptor desc;
    int r = JS_GetOwnProperty(ctx, &desc, obj, a);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    *out = desc.value;
    return 1;
}

int dyn_sc_utf8_to_utf16(JSContext* ctx, const char* s, size_t n,
    uint16_t** out)
{
    uint16_t* u = js_malloc(ctx, (n + 1) * sizeof(uint16_t));
    size_t i = 0, o = 0;
    uint32_t c;
    if (!u)
        return -1;
    while (i < n) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            u[o++] = b;
            i++;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < n) {
            u[o++] = (uint16_t)(((uint16_t)(b & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F));
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < n) {
            c = ((uint32_t)(b & 0x0F) << 12) | (((uint32_t)(uint8_t)s[i + 1] & 0x3F) << 6) | ((uint32_t)(uint8_t)s[i + 2] & 0x3F);
            i += 3;
            if (c >= 0x10000 && c <= 0x10FFFF) {
                c -= 0x10000;
                u[o++] = (uint16_t)(0xD800 + (c >> 10));
                u[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
            } else {
                u[o++] = (uint16_t)c;
            }
        } else if ((b & 0xF8) == 0xF0 && i + 3 < n) {
            c = ((uint32_t)(b & 0x07) << 18) | (((uint32_t)(uint8_t)s[i + 1] & 0x3F) << 12) | (((uint32_t)(uint8_t)s[i + 2] & 0x3F) << 6) | ((uint32_t)(uint8_t)s[i + 3] & 0x3F);
            i += 4;
            if (c <= 0x10FFFF) {
                c -= 0x10000;
                u[o++] = (uint16_t)(0xD800 + (c >> 10));
                u[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
            } else {
                u[o++] = 0xFFFD;
            }
        } else {
            u[o++] = b;
            i++;
        }
    }
    *out = u;
    return (int)o;
}

int dyn_sc_errs_move(JSContext* ctx, dyn_scerrs_t* dst,
    dyn_scerrs_t* src)
{
    int i, room = DYN_SC_MAX_ERRORS - dst->n;
    if (src->n == 0)
        return 0;
    if (room <= 0) {
        dst->done = 1;
        dst->drops += src->drops;
        src->drops = 0;
        return 0;
    }
    if (src->n < room)
        room = src->n;
    if (dst->n + room > dst->cap) {
        int ncap = dst->cap ? dst->cap : 16;
        dyn_scerr_t* ne;
        while (ncap < dst->n + room)
            ncap *= 2;
        ne = js_realloc(ctx, dst->e, (size_t)ncap * sizeof(*ne));
        if (!ne)
            return -1;
        dst->e = ne;
        dst->cap = ncap;
    }
    for (i = 0; i < room; i++) {
        dst->e[dst->n++] = src->e[i];
        src->e[i].path = NULL;
        src->e[i].message = NULL;
        src->e[i].keyword = NULL;
    }
    dst->drops += src->drops;
    src->drops = 0;
    return 0;
}

int dyn_sc_type_ok(JSContext* ctx, int mask, JSValueConst inst)
{
    int it = dyn_sc_inst_type(ctx, inst);
    int ok = 0;
    if (it == 0)
        return 0;
    if (mask & it)
        ok = 1;
    if (it == DYN_SC_T_NUMBER) {
        double d = dyn_num_value(inst);
        if (!isfinite(d))
            ok = 0;
        if ((mask & DYN_SC_T_INTEGER) && isfinite(d) && d == trunc(d))
            ok = 1;
    }
    return ok;
}

void dyn_sc_type_str(int mask, char* buf, size_t cap)
{
    static const struct {
        int bit;
        const char* name;
    } T[] = {
        { DYN_SC_T_NULL, "null" },
        { DYN_SC_T_BOOL, "boolean" },
        { DYN_SC_T_OBJECT, "object" },
        { DYN_SC_T_ARRAY, "array" },
        { DYN_SC_T_NUMBER, "number" },
        { DYN_SC_T_STRING, "string" },
        { DYN_SC_T_INTEGER, "integer" },
    };
    size_t o = 0;
    int i, n = 0;
    for (i = 0; i < (int)countof(T); i++)
        if (mask & T[i].bit)
            n++;
    if (n == 1) {
        for (i = 0; i < (int)countof(T); i++)
            if (mask & T[i].bit) {
                snprintf(buf, cap, "a %s", T[i].name);
                return;
            }
    }
    o += (size_t)snprintf(buf + o, cap - o, "one of: ");
    for (i = 0; i < (int)countof(T); i++) {
        if (mask & T[i].bit) {
            o += (size_t)snprintf(buf + o, cap - o, "%s%s", T[i].name,
                --n ? ", " : "");
        }
    }
}

static JSValue dyn_sc_proto_validate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_schema_t* sch = (dyn_schema_t*)dyn_plain_get(ctx, this_val,
        dyn_schema_class_id);
    if (!sch)
        return JS_EXCEPTION;
    return dyn_sc_validate_impl(ctx, sch,
        argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue dyn_sc_fmt_registry(JSContext* ctx, int create)
{
    JSValue g, reg;
    JSAtom a = JS_NewAtom(ctx, DYN_SC_FMTREG_NAME);

    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    g = JS_GetGlobalObject(ctx);
    reg = JS_GetProperty(ctx, g, a);
    if (JS_IsException(reg) || !JS_IsObject(reg)) {
        JS_FreeValue(ctx, reg);
        reg = JS_UNDEFINED;
        if (create) {
            reg = JS_NewObject(ctx);
            if (JS_IsException(reg))
                goto out;
            if (JS_DefinePropertyValue(ctx, g, a,
                    JS_DupValue(ctx, reg),
                    JS_PROP_CONFIGURABLE)
                < 0) {
                JS_FreeValue(ctx, reg);
                reg = JS_EXCEPTION;
            }
        }
    }
out:
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, g);
    return reg;
}

JSValue dyn_sc_fmt_lookup(JSContext* ctx, const char* name)
{
    JSValue reg, fn;
    JSAtom a;

    reg = dyn_sc_fmt_registry(ctx, 0);
    if (JS_IsException(reg) || JS_IsUndefined(reg))
        return reg;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, reg);
        return JS_EXCEPTION;
    }
    fn = JS_GetProperty(ctx, reg, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, reg);
    return fn;
}

static int dyn_sc_fmt_has_own(JSContext* ctx, JSValueConst obj, JSAtom a)
{
    return JS_GetOwnProperty(ctx, NULL, obj, a);
}

static JSValue dyn_sc_register_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* name;
    size_t nl;
    JSValue reg;
    JSAtom a = JS_ATOM_NULL;
    JSValue ret = JS_UNDEFINED;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx,
            "Schema.registerFormat: expected (name: string, fn: function)");
    name = JS_ToCStringLen(ctx, &nl, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (nl == 0 || nl > DYN_SC_FMT_NAME_MAX || strlen(name) != nl) {
        JS_ThrowTypeError(ctx,
            "Schema.registerFormat: name must be non-empty, NUL-free, "
            "and at most %d bytes",
            DYN_SC_FMT_NAME_MAX);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    reg = dyn_sc_fmt_registry(ctx, 1);
    if (JS_IsException(reg)) {
        JS_FreeCString(ctx, name);
        return reg;
    }
    a = JS_NewAtomLen(ctx, name, nl);
    if (a == JS_ATOM_NULL) {
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, reg);
        return JS_EXCEPTION;
    }
    {
        int has = dyn_sc_fmt_has_own(ctx, reg, a);
        if (has < 0) {
            ret = JS_EXCEPTION;
        } else if (has) {
            JS_ThrowTypeError(ctx,
                "Schema.registerFormat: format \"%s\" is already registered",
                name);
            ret = JS_EXCEPTION;
        } else if (JS_DefinePropertyValue(ctx, reg, a, JS_DupValue(ctx, argv[1]), JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
            ret = JS_EXCEPTION;
        }
    }
    JS_FreeAtom(ctx, a);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, reg);
    return ret;
}

static const JSCFunctionListEntry dyn_sc_proto_funcs[] = {
    JS_CFUNC_DEF("validate", 1, dyn_sc_proto_validate),
};

static const JSCFunctionListEntry dyn_sc_schema_funcs[] = {
    JS_CFUNC_DEF("compile", 2, dyn_sc_compile_entry),
    JS_CFUNC_DEF("validate", 2, dyn_sc_schema_validate),
    JS_CFUNC_DEF("registerFormat", 2, dyn_sc_register_format),
};

static const JSCFunctionListEntry dyn_sc_module_funcs[] = {
    JS_OBJECT_DEF("Schema", dyn_sc_schema_funcs, countof(dyn_sc_schema_funcs),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
};

static const JSClassDef dyn_sc_class = {
    "CompiledSchema",
    .finalizer = dyn_sc_schema_finalizer,
    .gc_mark = dyn_sc_schema_mark,
};

static int dyn_sc_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dyn_sc_module_funcs,
        countof(dyn_sc_module_funcs));
}

int js_nat_init_schema(JSContext* ctx)
{
    JSModuleDef* m;
    JSValue proto;

    m = JS_NewCModule(ctx, "dyna:schema", dyn_sc_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_sc_module_funcs,
            countof(dyn_sc_module_funcs))
        < 0)
        return -1;

    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), dyn_schema_class_id)) {
        JS_NewClassID(&dyn_schema_class_id);
        if (JS_NewClass(JS_GetRuntime(ctx), dyn_schema_class_id, &dyn_sc_class) < 0)
            return -1;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_sc_proto_funcs,
        countof(dyn_sc_proto_funcs));
    JS_SetClassProto(ctx, dyn_schema_class_id, proto);
    return 0;
}

#endif
