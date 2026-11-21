#include "config-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#define DYN_TOML_MAX_DEPTH 64

typedef struct {
    void** v;
    size_t n, cap;
} dyn_tml_set_t;

typedef struct {
    JSContext* ctx;
    dyn_tml_lex_t x;
    dyn_tml_set_t marks;
    dyn_tml_set_t closed;
    int bigint;
} dyn_tml_t;

static void dyn_tml_set_free(dyn_tml_set_t* set)
{
    free(set->v);
    set->v = NULL;
    set->n = set->cap = 0;
}

static size_t dyn_tml_set_hash(const void* p)
{
    uintptr_t x = (uintptr_t)p >> 3;
    x *= (uintptr_t)0x9E3779B97F4A7C15ULL;
    return (size_t)(x >> (sizeof(uintptr_t) * 4)) ^ (size_t)x;
}

static void dyn_tml_sets_free(dyn_tml_t* t)
{
    dyn_tml_set_free(&t->marks);
    dyn_tml_set_free(&t->closed);
}

static int dyn_tml_set_grow(dyn_tml_set_t* set)
{
    size_t ncap = set->cap ? set->cap * 2 : 64;
    void** nv = (void**)calloc(ncap, sizeof *nv);
    size_t k;
    if (!nv)
        return -1;
    for (k = 0; k < set->cap; k++) {
        void* p = set->v[k];
        size_t h;
        if (!p)
            continue;
        h = dyn_tml_set_hash(p) & (ncap - 1);
        while (nv[h])
            h = (h + 1) & (ncap - 1);
        nv[h] = p;
    }
    free(set->v);
    set->v = nv;
    set->cap = ncap;
    return 0;
}

static int dyn_tml_set_add(dyn_tml_set_t* set, JSValue v)
{
    void* p = JS_VALUE_GET_PTR(v);
    size_t h;
    if (set->n * 2 >= set->cap && dyn_tml_set_grow(set) < 0)
        return -1;
    h = dyn_tml_set_hash(p) & (set->cap - 1);
    while (set->v[h]) {
        if (set->v[h] == p)
            return 0;
        h = (h + 1) & (set->cap - 1);
    }
    set->v[h] = p;
    set->n++;
    return 0;
}

static int dyn_tml_set_has(dyn_tml_set_t* set, JSValueConst v)
{
    void* p = JS_VALUE_GET_PTR(v);
    size_t h;
    if (!set->cap)
        return 0;
    h = dyn_tml_set_hash(p) & (set->cap - 1);
    while (set->v[h]) {
        if (set->v[h] == p)
            return 1;
        h = (h + 1) & (set->cap - 1);
    }
    return 0;
}

static int dyn_tml_mark(dyn_tml_t* t, JSValue v)
{
    return dyn_tml_set_add(&t->marks, v);
}

static int dyn_tml_marked(dyn_tml_t* t, JSValueConst v)
{
    return dyn_tml_set_has(&t->marks, v);
}

static JSValue __attribute__((format(printf, 2, 3))) dyn_tml_err(dyn_tml_t* t, const char* fmt, ...)
{
    va_list ap;
    char buf[192], msg[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(msg, sizeof msg, "TOML.parse: line %d col %d: %s", t->x.line, t->x.col, buf);
    return JS_ThrowSyntaxError(t->ctx, "%s", msg);
}

static double dyn_tml_atod(const char* buf, const char** end)
{
    JSATODTempMem tmp;
    return js_atod(buf, end, 10, 0, &tmp);
}

static JSValue dyn_tml_throw(dyn_tml_t* t)
{
    char msg[256];
    if (t->x.ecode == DYN_TML_E_OOM)
        return JS_ThrowOutOfMemory(t->ctx);
    dyn_tml_format_error(&t->x, msg, sizeof msg);
    return JS_ThrowSyntaxError(t->ctx, "TOML.parse: %s", msg);
}

static int dyn_tml_define(dyn_tml_t* t, JSValue obj, const char* key, size_t klen,
    JSValue v, int dup)
{
    JSAtom a = JS_NewAtomLen(t->ctx, key, klen);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(t->ctx, v);
        return -1;
    }
    r = JS_GetOwnProperty(t->ctx, &desc, obj, a);
    if (r < 0) {
        JS_FreeAtom(t->ctx, a);
        JS_FreeValue(t->ctx, v);
        return -1;
    }
    if (r && dup) {
        JS_FreeValue(t->ctx, desc.getter);
        JS_FreeValue(t->ctx, desc.setter);
        JS_FreeValue(t->ctx, desc.value);
        JS_FreeAtom(t->ctx, a);
        JS_FreeValue(t->ctx, v);
        JS_ThrowSyntaxError(t->ctx, "TOML.parse: line %d col %d: duplicate key", t->x.line, t->x.col);
        return -1;
    }
    r = JS_DefinePropertyValue(t->ctx, obj, a, v, JS_PROP_C_W_E);
    JS_FreeAtom(t->ctx, a);
    return r < 0 ? -1 : 0;
}

static JSValue dyn_tml_value(dyn_tml_t* t, int depth);

static int dyn_tml_walk_key(dyn_tml_t* t, JSValue root, const dyn_tml_key_t* key,
    JSValue* parent, size_t* pseg);

static JSValue dyn_tml_array(dyn_tml_t* t, int depth)
{
    JSValue a = JS_NewArray(t->ctx);
    uint64_t n = 0;
    if (JS_IsException(a))
        return a;
    if (depth >= DYN_TOML_MAX_DEPTH) {
        JS_FreeValue(t->ctx, a);
        return dyn_tml_err(t, "nesting exceeds %d", DYN_TOML_MAX_DEPTH);
    }
    dyn_tml_adv(&t->x);
    dyn_tml_blank(&t->x);
    if (dyn_tml_cur(&t->x) == ']') {
        dyn_tml_adv(&t->x);
        return a;
    }
    for (;;) {
        JSValue v = dyn_tml_value(t, depth + 1);
        if (JS_IsException(v)) {
            JS_FreeValue(t->ctx, a);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(t->ctx, a, (uint32_t)n++, v,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(t->ctx, a);
            return JS_EXCEPTION;
        }
        dyn_tml_blank(&t->x);
        if (dyn_tml_cur(&t->x) == ']') {
            dyn_tml_adv(&t->x);
            return a;
        }
        if (dyn_tml_cur(&t->x) != ',') {
            dyn_tml_err(t, "expected ',' or ']' in an array");
            JS_FreeValue(t->ctx, a);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(&t->x);
        dyn_tml_blank(&t->x);
        if (dyn_tml_cur(&t->x) == ']') {
            dyn_tml_adv(&t->x);
            return a;
        }
    }
}

static JSValue dyn_tml_inline_table(dyn_tml_t* t, int depth)
{
    JSValue o = JS_NewObject(t->ctx);
    if (JS_IsException(o))
        return o;
    if (depth >= DYN_TOML_MAX_DEPTH) {
        JS_FreeValue(t->ctx, o);
        return dyn_tml_err(t, "nesting exceeds %d", DYN_TOML_MAX_DEPTH);
    }
    if (dyn_tml_mark(t, o) < 0 || dyn_tml_set_add(&t->closed, o) < 0) {
        JS_FreeValue(t->ctx, o);
        JS_ThrowOutOfMemory(t->ctx);
        return JS_EXCEPTION;
    }
    dyn_tml_adv(&t->x);
    dyn_tml_ws(&t->x);
    if (dyn_tml_cur(&t->x) == '}') {
        dyn_tml_adv(&t->x);
        return o;
    }
    for (;;) {
        dyn_tml_key_t key;
        JSValue v, parent;
        size_t seg;
        dyn_tml_ws(&t->x);
        dyn_tml_key_init(&key);
        {
            int c = dyn_tml_cur(&t->x);
            if (c != '"' && c != '\'' && !dyn_tml_key_char(c)) {
                dyn_tml_err(t, "expected a key in an inline table");
                JS_FreeValue(t->ctx, o);
                return JS_EXCEPTION;
            }
            if (dyn_tml_key_parse(&t->x, &key) < 0) {
                dyn_tml_throw(t);
                dyn_tml_key_free(&key);
                JS_FreeValue(t->ctx, o);
                return JS_EXCEPTION;
            }
        }
        if (dyn_tml_cur(&t->x) != '=') {
            dyn_tml_err(t, "expected '=' in an inline table");
            dyn_tml_key_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(&t->x);
        dyn_tml_ws(&t->x);
        v = dyn_tml_value(t, depth + 1);
        if (JS_IsException(v)) {
            dyn_tml_key_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        if (dyn_tml_walk_key(t, o, &key, &parent, &seg) < 0) {
            dyn_tml_key_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        if (dyn_tml_define(t, parent, key.p + key.seg[seg].off,
                key.seg[seg].len, v, 1)
            < 0) {
            JS_FreeValue(t->ctx, parent);
            dyn_tml_key_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        JS_FreeValue(t->ctx, parent);
        dyn_tml_key_free(&key);
        dyn_tml_ws(&t->x);
        if (dyn_tml_cur(&t->x) == '}') {
            dyn_tml_adv(&t->x);
            return o;
        }
        if (dyn_tml_cur(&t->x) != ',') {
            dyn_tml_err(t, "expected ',' or '}' in an inline table");
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(&t->x);
        dyn_tml_ws(&t->x);
    }
}

static JSValue dyn_tml_value(dyn_tml_t* t, int depth)
{
    int c = dyn_tml_cur(&t->x);
    dyn_tml_scalar_t v;
    if (c == '"' || c == '\'') {
        dyn_tml_sb_t sb;
        JSValue r;
        dyn_tml_sb_init(&sb);
        if (dyn_tml_string(&t->x, &sb) < 0) {
            dyn_tml_sb_free(&sb);
            return dyn_tml_throw(t);
        }
        r = JS_NewStringLen(t->ctx, sb.p ? sb.p : "", sb.n);
        dyn_tml_sb_free(&sb);
        return r;
    }
    if (c == '[')
        return dyn_tml_array(t, depth);
    if (c == '{')
        return dyn_tml_inline_table(t, depth);
    if (dyn_tml_scalar(&t->x, &v, dyn_tml_atod) < 0)
        return dyn_tml_throw(t);
    switch (v.kind) {
    case DYN_TML_V_BOOL:
        return JS_NewBool(t->ctx, v.b);
    case DYN_TML_V_INT:
        if (t->bigint && (v.i > 9007199254740991LL || v.i < -9007199254740991LL))
            return JS_NewBigInt64(t->ctx, v.i);
        return JS_NewInt64(t->ctx, v.i);
    case DYN_TML_V_FLOAT:
        return __JS_NewFloat64(t->ctx, v.d);
    default:
        return JS_NewStringLen(t->ctx, t->x.src + v.off, v.len);
    }
}
static int dyn_tml_walk_key(dyn_tml_t* t, JSValue root, const dyn_tml_key_t* key,
    JSValue* parent, size_t* pseg)
{
    JSValue cur = JS_DupValue(t->ctx, root);
    size_t seg = 0;
    if (key->nseg > DYN_TOML_MAX_DEPTH) {
        JS_FreeValue(t->ctx, cur);
        JS_ThrowSyntaxError(t->ctx,
            "TOML.parse: line %d col %d: key nesting exceeds %d",
            t->x.line, t->x.col, DYN_TOML_MAX_DEPTH);
        return -1;
    }
    for (;;) {
        int is_last;
        JSAtom a;
        is_last = (seg + 1 >= key->nseg);
        a = JS_NewAtomLen(t->ctx, key->p + key->seg[seg].off, key->seg[seg].len);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(t->ctx, cur);
            return -1;
        }
        if (is_last) {
            JS_FreeAtom(t->ctx, a);
            *parent = cur;
            *pseg = seg;
            return 0;
        }
        {
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (!JS_IsObject(nxt) || JS_IsArray(t->ctx, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a dotted key crosses a value",
                        t->x.line, t->x.col);
                    return -1;
                }
                if (dyn_tml_set_has(&t->closed, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a dotted key cannot add to "
                        "a closed inline table",
                        t->x.line, t->x.col);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            } else {
                nxt = JS_NewObject(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (dyn_tml_mark(t, nxt) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowOutOfMemory(t->ctx);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            }
        }
        JS_FreeAtom(t->ctx, a);
        seg++;
    }
}

static int dyn_tml_header(dyn_tml_t* t, JSValue root, const dyn_tml_key_t* key,
    int is_array, JSValue* out)
{
    JSValue cur = JS_DupValue(t->ctx, root);
    size_t seg = 0;
    for (;;) {
        JSAtom a;
        a = JS_NewAtomLen(t->ctx, key->p + key->seg[seg].off, key->seg[seg].len);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(t->ctx, cur);
            return -1;
        }
        if (seg + 1 >= key->nseg) {
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (is_array) {
                    if (!JS_IsArray(t->ctx, nxt)) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        JS_ThrowSyntaxError(t->ctx,
                            "TOML.parse: line %d col %d: a table conflicts with "
                            "an array-of-tables",
                            t->x.line, t->x.col);
                        return -1;
                    }
                    if (!dyn_tml_marked(t, nxt)) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        JS_ThrowSyntaxError(t->ctx,
                            "TOML.parse: line %d col %d: an array-of-tables cannot "
                            "extend a statically defined array",
                            t->x.line, t->x.col);
                        return -1;
                    }
                    {
                        JSValue el = JS_NewObject(t->ctx);
                        JSValue lv;
                        uint64_t ln = 0;
                        if (JS_IsException(el)) {
                            JS_FreeValue(t->ctx, nxt);
                            JS_FreeAtom(t->ctx, a);
                            JS_FreeValue(t->ctx, cur);
                            return -1;
                        }
                        lv = JS_GetPropertyStr(t->ctx, nxt, "length");
                        if (JS_ToIndex(t->ctx, &ln, lv) < 0) {
                            JS_FreeValue(t->ctx, lv);
                            JS_FreeValue(t->ctx, el);
                            JS_FreeValue(t->ctx, nxt);
                            JS_FreeAtom(t->ctx, a);
                            JS_FreeValue(t->ctx, cur);
                            return -1;
                        }
                        JS_FreeValue(t->ctx, lv);
                        if (JS_DefinePropertyValueUint32(t->ctx, nxt, (uint32_t)ln,
                                JS_DupValue(t->ctx, el),
                                JS_PROP_C_W_E)
                            < 0) {
                            JS_FreeValue(t->ctx, nxt);
                            JS_FreeAtom(t->ctx, a);
                            JS_FreeValue(t->ctx, cur);
                            return -1;
                        }
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        *out = el;
                        return 0;
                    }
                }
                if (!JS_IsObject(nxt) || JS_IsArray(t->ctx, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a table conflicts with a value",
                        t->x.line, t->x.col);
                    return -1;
                }
                if (dyn_tml_marked(t, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: table already defined",
                        t->x.line, t->x.col);
                    return -1;
                }
                if (dyn_tml_mark(t, nxt) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowOutOfMemory(t->ctx);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                JS_FreeAtom(t->ctx, a);
                *out = nxt;
                return 0;
            }
            if (is_array) {
                nxt = JS_NewArray(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (dyn_tml_mark(t, nxt) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowOutOfMemory(t->ctx);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                {
                    JSValue el = JS_NewObject(t->ctx);
                    if (JS_IsException(el)) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        return -1;
                    }
                    if (JS_DefinePropertyValueUint32(t->ctx, nxt, 0,
                            JS_DupValue(t->ctx, el),
                            JS_PROP_C_W_E)
                        < 0) {
                        JS_FreeValue(t->ctx, el);
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        return -1;
                    }
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    *out = el;
                    return 0;
                }
            }
            nxt = JS_NewObject(t->ctx);
            if (JS_IsException(nxt)) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(t->ctx, nxt);
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (dyn_tml_mark(t, nxt) < 0) {
                JS_FreeValue(t->ctx, nxt);
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                JS_ThrowOutOfMemory(t->ctx);
                return -1;
            }
            JS_FreeValue(t->ctx, cur);
            JS_FreeAtom(t->ctx, a);
            *out = nxt;
            return 0;
        }
        {
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (JS_IsArray(t->ctx, nxt)) {
                    JSValue lv;
                    uint64_t ln = 0;
                    JSValue el;
                    lv = JS_GetPropertyStr(t->ctx, nxt, "length");
                    r = JS_ToIndex(t->ctx, &ln, lv);
                    JS_FreeValue(t->ctx, lv);
                    if (r < 0 || ln == 0) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        JS_ThrowSyntaxError(t->ctx,
                            "TOML.parse: line %d col %d: a table header cannot "
                            "extend an empty array-of-tables",
                            t->x.line, t->x.col);
                        return -1;
                    }
                    el = JS_GetPropertyUint32(t->ctx, nxt, (uint32_t)(ln - 1));
                    JS_FreeValue(t->ctx, nxt);
                    if (JS_IsException(el) || !JS_IsObject(el)) {
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        JS_FreeValue(t->ctx, el);
                        return -1;
                    }
                    JS_FreeValue(t->ctx, cur);
                    cur = el;
                    JS_FreeAtom(t->ctx, a);
                    seg++;
                    continue;
                }
                if (!JS_IsObject(nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a table header crosses a value",
                        t->x.line, t->x.col);
                    return -1;
                }
                if (dyn_tml_set_has(&t->closed, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a table header cannot extend "
                        "a closed inline table",
                        t->x.line, t->x.col);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            } else {
                nxt = JS_NewObject(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            }
        }
        JS_FreeAtom(t->ctx, a);
        seg++;
    }
}

static const char* const dyn_tml_parse_keys[] = { "bigint" };

JSValue dyn_tml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tml_t t;
    JSValue root, active;
    const char* s;
    size_t n;
    int bigint = 0;
    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "TOML.parse(text): text must be a string");
    if (dyn_opts_bag(ctx, argc, argv, 1, dyn_tml_parse_keys, countof(dyn_tml_parse_keys), "TOML.parse") < 0
        || (argc > 1 && dyn_opt_bool(ctx, argv[1], "bigint", "TOML.parse", &bigint) < 0))
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    memset(&t, 0, sizeof t);
    t.ctx = ctx;
    t.bigint = bigint;
    {
        const char* body = s;
        dyn_cfg_bom(&body, &n);
        dyn_tml_lex_init(&t.x, body, n);
    }
    root = JS_NewObject(ctx);
    if (JS_IsException(root)) {
        JS_FreeCString(ctx, s);
        return root;
    }
    active = JS_DupValue(ctx, root);

    dyn_tml_blank(&t.x);
    while (t.x.i < t.x.n) {
        int c = dyn_tml_cur(&t.x);
        if (c == '[') {
            int is_array = dyn_tml_peek(&t.x, 1) == '[';
            dyn_tml_key_t path;
            JSValue nact;
            dyn_tml_key_init(&path);
            t.x.i += is_array ? 2 : 1;
            t.x.col += is_array ? 2 : 1;
            if (dyn_tml_key_parse(&t.x, &path) < 0) {
                dyn_tml_throw(&t);
                dyn_tml_key_free(&path);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            dyn_tml_ws(&t.x);
            c = dyn_tml_cur(&t.x);
            if ((!is_array && c != ']') || (is_array && (c != ']' || dyn_tml_peek(&t.x, 1) != ']'))) {
                dyn_tml_err(&t, "malformed table header");
                dyn_tml_key_free(&path);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            t.x.i += is_array ? 2 : 1;
            t.x.col += is_array ? 2 : 1;
            if (dyn_tml_header(&t, root, &path, is_array, &nact) < 0) {
                dyn_tml_key_free(&path);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            dyn_tml_key_free(&path);
            c = dyn_tml_cur(&t.x);
            while (c == ' ' || c == '\t') {
                dyn_tml_adv(&t.x);
                c = dyn_tml_cur(&t.x);
            }
            if (c != -1 && c != '\n' && c != '\r' && c != '#') {
                dyn_tml_err(&t, "junk after a table header");
                JS_FreeValue(ctx, nact);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            while (c != -1 && c != '\n') {
                dyn_tml_adv(&t.x);
                c = dyn_tml_cur(&t.x);
            }
            JS_FreeValue(ctx, active);
            active = nact;
            dyn_tml_blank(&t.x);
            continue;
        }
        if (c == '#' || c == '\n' || c == '\r') {
            dyn_tml_blank(&t.x);
            continue;
        }
        {
            dyn_tml_key_t key;
            JSValue v, parent;
            size_t seg;
            if (c != '"' && c != '\'' && !dyn_tml_key_char(c)) {
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return dyn_tml_err(&t, "expected key = value");
            }
            dyn_tml_key_init(&key);
            if (dyn_tml_key_parse(&t.x, &key) < 0) {
                dyn_tml_throw(&t);
                dyn_tml_key_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            if (dyn_tml_cur(&t.x) != '=') {
                dyn_tml_key_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return dyn_tml_err(&t, "expected '='");
            }
            dyn_tml_adv(&t.x);
            dyn_tml_ws(&t.x);
            v = dyn_tml_value(&t, 0);
            if (JS_IsException(v)) {
                dyn_tml_key_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return v;
            }
            if (dyn_tml_walk_key(&t, active, &key, &parent, &seg) < 0) {
                dyn_tml_key_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            if (dyn_tml_define(&t, parent, key.p + key.seg[seg].off,
                    key.seg[seg].len, v, 1)
                < 0) {
                JS_FreeValue(ctx, parent);
                dyn_tml_key_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, parent);
            dyn_tml_key_free(&key);
            c = dyn_tml_cur(&t.x);
            while (c == ' ' || c == '\t') {
                dyn_tml_adv(&t.x);
                c = dyn_tml_cur(&t.x);
            }
            if (c == '#')
                while (c != -1 && c != '\n') {
                    dyn_tml_adv(&t.x);
                    c = dyn_tml_cur(&t.x);
                }
            if (c != -1 && c != '\n' && c != '\r') {
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_sets_free(&t);
                return dyn_tml_err(&t, "expected a newline after a key-value pair");
            }
            dyn_tml_blank(&t.x);
            continue;
        }
    }
    JS_FreeValue(ctx, active);
    JS_FreeCString(ctx, s);
    dyn_tml_sets_free(&t);
    return root;
}

static const uint8_t toml_esc_map[256] = {
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
};

static void dyn_tml_sb_escape(dyn_tml_sb_t* b, const char* s, size_t n)
{
    size_t i = 0, start = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (!toml_esc_map[c]) {
            i++;
            continue;
        }
        if (i > start)
            dyn_tml_sb_put(b, s + start, i - start);
        switch (c) {
        case '"':
            dyn_tml_sb_put(b, "\\\"", 2);
            break;
        case '\\':
            dyn_tml_sb_put(b, "\\\\", 2);
            break;
        case '\n':
            dyn_tml_sb_put(b, "\\n", 2);
            break;
        case '\t':
            dyn_tml_sb_put(b, "\\t", 2);
            break;
        case '\r':
            dyn_tml_sb_put(b, "\\r", 2);
            break;
        case '\b':
            dyn_tml_sb_put(b, "\\b", 2);
            break;
        case '\f':
            dyn_tml_sb_put(b, "\\f", 2);
            break;
        default: {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            dyn_tml_sb_put(b, esc, 6);
            break;
        }
        }
        i++;
        start = i;
    }
    if (i > start)
        dyn_tml_sb_put(b, s + start, i - start);
}

static int dyn_tml_emit(JSContext* ctx, dyn_tml_sb_t* b, JSValueConst v, int depth);

static int dyn_tml_emit_string(JSContext* ctx, dyn_tml_sb_t* b, JSValueConst v)
{
    const char* s;
    size_t n;
    if (!JS_IsString(v))
        return -1;
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    dyn_tml_sb_putc(b, '"');
    dyn_tml_sb_escape(b, s, n);
    dyn_tml_sb_putc(b, '"');
    JS_FreeCString(ctx, s);
    return 0;
}

static int dyn_tml_emit(JSContext* ctx, dyn_tml_sb_t* b, JSValueConst v, int depth)
{
    if (depth > DYN_TOML_MAX_DEPTH)
        return -1;
    if (JS_IsString(v))
        return dyn_tml_emit_string(ctx, b, v);
    if (JS_IsBool(v)) {
        dyn_tml_sb_put(b, JS_ToBool(ctx, v) ? "true" : "false",
            JS_ToBool(ctx, v) ? 4 : 5);
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        if (isnan(d)) {
            dyn_tml_sb_put(b, "nan", 3);
            return 0;
        }
        if (isinf(d)) {
            if (d > 0)
                dyn_tml_sb_put(b, "inf", 3);
            else
                dyn_tml_sb_put(b, "-inf", 4);
            return 0;
        }
        {
            char num[64];
            int64_t iv;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)) && d == floor(d)
                && d >= -2147483648.0 && d < 2147483648.0) {
                if (d == 0 && signbit(d))
                    memcpy(num, "-0.0", 5);
                else
                    memcpy(num + i32toa(num, (int32_t)d), ".0", 3);
            } else if (d >= -9007199254740991.0 && d <= 9007199254740991.0
                && JS_ToInt64(ctx, &iv, v) == 0 && (double)iv == d) {
                snprintf(num, sizeof num, "%lld", (long long)iv);
            } else {
                size_t nl = dyn_cfg_dtoa(num, d);
                if (!memchr(num, '.', nl) && !memchr(num, 'e', nl))
                    memcpy(num + nl, ".0", 3);
            }
            dyn_tml_sb_put(b, num, strlen(num));
            return 0;
        }
    }
    if (JS_IsBigInt(ctx, v)) {
        char num[32];
        int64_t iv;
        int same;
        JSValue back;
        if (JS_ToBigInt64(ctx, &iv, v))
            return -1;
        back = JS_NewBigInt64(ctx, iv);
        if (JS_IsException(back))
            return -1;
        same = JS_StrictEq(ctx, back, v);
        JS_FreeValue(ctx, back);
        if (!same)
            return -1;
        snprintf(num, sizeof num, "%lld", (long long)iv);
        dyn_tml_sb_put(b, num, strlen(num));
        return 0;
    }
    if (JS_IsArray(ctx, v)) {
        uint64_t len, i;
        int64_t ln;
        if (dyn_js_len(ctx, v, &ln))
            return -1;
        len = ln > 0 ? (uint64_t)ln : 0;
        dyn_tml_sb_putc(b, '[');
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            if (i)
                dyn_tml_sb_put(b, ", ", 2);
            if (JS_IsException(el) || dyn_tml_emit(ctx, b, el, depth + 1) < 0) {
                JS_FreeValue(ctx, el);
                return -1;
            }
            JS_FreeValue(ctx, el);
        }
        dyn_tml_sb_putc(b, ']');
        return 0;
    }
    if (JS_IsObject(v)) {
        JSPropertyEnum* tab = NULL;
        uint32_t n = 0, k;
        int is_root = (depth == 0);
        dyn_tml_sb_putc(b, is_root ? '\n' : '{');
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            return -1;
        for (k = 0; k < n; k++) {
            JSValue el = JS_GetProperty(ctx, v, tab[k].atom);
            size_t klen = 0;
            const char* ks = JS_AtomToCStringLen(ctx, &klen, tab[k].atom);
            if (!ks || JS_IsException(el)) {
                if (ks)
                    JS_FreeCString(ctx, ks);
                JS_FreeValue(ctx, el);
                JS_FreePropertyEnum(ctx, tab, n);
                return -1;
            }
            size_t m;
            int bare = klen > 0;
            for (m = 0; m < klen; m++) {
                char ch = ks[m];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
                    bare = 0;
            }
            if (k)
                dyn_tml_sb_put(b, is_root ? "\n" : ", ", is_root ? 1 : 2);
            if (bare) {
                dyn_tml_sb_put(b, ks, klen);
            } else {
                dyn_tml_sb_putc(b, '"');
                dyn_tml_sb_escape(b, ks, klen);
                dyn_tml_sb_putc(b, '"');
            }
            dyn_tml_sb_put(b, " = ", 3);
            if (dyn_tml_emit(ctx, b, el, depth + 1) < 0) {
                JS_FreeValue(ctx, el);
                JS_FreeCString(ctx, ks);
                JS_FreePropertyEnum(ctx, tab, n);
                return -1;
            }
            JS_FreeValue(ctx, el);
            JS_FreeCString(ctx, ks);
        }
        JS_FreePropertyEnum(ctx, tab, n);
        if (!is_root)
            dyn_tml_sb_putc(b, '}');
        return 0;
    }
    return -1;
}

JSValue dyn_tml_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_tml_sb_t b;
    JSValue r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "TOML.stringify(value): a value is required");
    if (!JS_IsObject(argv[0]) || JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "TOML.stringify(value): the document root must be an object");
    dyn_tml_sb_init(&b);
    if (dyn_tml_emit(ctx, &b, argv[0], 0) < 0 || b.oom) {
        dyn_tml_sb_free(&b);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_ThrowTypeError(ctx, "TOML.stringify: unsupported value");
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_tml_sb_free(&b);
    return r;
}

#endif
