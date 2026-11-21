#include "json-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_JSON)

#define DYN_JP_MAX_DEPTH 128
#define DYN_JP_MAX_OPS 65536
#define DYN_JP_CLS_OBJECT 1

typedef struct {
    JSValue parent;
    char* scratch;
    size_t tlen;
    uint64_t idx;
} dyn_jp_at_t;

typedef enum {
    DYN_JP_AT_ERR = 0,
    DYN_JP_AT_ARRAY,
    DYN_JP_AT_MEMBER,
} dyn_jp_at_kind_t;

static int dyn_jp_eqs(const char* s, size_t n, const char* lit)
{
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

static int dyn_jp_unescape(const char* s, size_t n, char* out, size_t cap,
    size_t* dlen)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != '~') {
            if (o >= cap)
                return -1;
            out[o++] = (char)c;
        } else {
            if (i + 1 >= n || (s[i + 1] != '0' && s[i + 1] != '1'))
                return -1;
            out[o++] = s[i + 1] == '0' ? '~' : '/';
            i++;
        }
    }
    *dlen = o;
    return 0;
}

static int dyn_jp_parse_index(const char* t, size_t n, uint64_t* idx)
{
    uint64_t v = 0;
    size_t i;
    if (n == 0)
        return -1;
    if (n > 1 && t[0] == '0')
        return -1;
    if (n > 19)
        return -2;
    for (i = 0; i < n; i++) {
        if (t[i] < '0' || t[i] > '9')
            return 0;
        v = v * 10 + (uint64_t)(t[i] - '0');
    }
    *idx = v;
    return 1;
}

static int dyn_jp_get_own(JSContext* ctx, JSValueConst obj, JSAtom a,
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

static uint64_t dyn_jp_arr_len(JSContext* ctx, JSValue arr)
{
    uint64_t len;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToIndex(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return (uint64_t)-1;
    }
    JS_FreeValue(ctx, lv);
    return len;
}

JSValue dyn_jp_clone(JSContext* ctx, JSValueConst v, int depth)
{
    if (depth > DYN_JP_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "dyna:json: value nesting exceeds depth %d",
            DYN_JP_MAX_DEPTH);
        return JS_EXCEPTION;
    }
    if (JS_IsArray(ctx, v)) {
        uint64_t len, i;
        JSValue res, lv;
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_ToIndex(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (len > (1u << 22)) {
            JS_ThrowRangeError(ctx, "dyna:json: array too long to clone "
                                    "(%llu elements; cap is %u)",
                (unsigned long long)len, 1u << 22);
            return JS_EXCEPTION;
        }
        res = JS_NewArray(ctx);
        if (JS_IsException(res))
            return res;
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            JSValue c;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            c = dyn_jp_clone(ctx, el, depth + 1);
            JS_FreeValue(ctx, el);
            if (JS_IsException(c)) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            if (JS_DefinePropertyValueUint32(ctx, res, (uint32_t)i, c, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
        return res;
    }
    if (JS_IsObject(v)) {
        JSValue res;
        JSPropertyEnum* props = NULL;
        uint32_t n, k;
        if (JS_GetClassID(v) != DYN_JP_CLS_OBJECT)
            return JS_DupValue(ctx, v);
        res = JS_NewObject(ctx);
        if (JS_IsException(res))
            return res;
        if (JS_GetOwnPropertyNames(ctx, &props, &n, v,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        for (k = 0; k < n; k++) {
            JSValue el = JS_GetProperty(ctx, v, props[k].atom);
            JSValue c;
            if (JS_IsException(el)) {
                JS_FreePropertyEnum(ctx, props, n);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            c = dyn_jp_clone(ctx, el, depth + 1);
            JS_FreeValue(ctx, el);
            if (JS_IsException(c)) {
                JS_FreePropertyEnum(ctx, props, n);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            if (JS_DefinePropertyValue(ctx, res, props[k].atom, c,
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreePropertyEnum(ctx, props, n);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
        }
        JS_FreePropertyEnum(ctx, props, n);
        return res;
    }
    return JS_DupValue(ctx, v);
}

static int dyn_jp_equal(JSContext* ctx, JSValueConst a, JSValueConst b,
    int depth)
{
    if (depth > DYN_JP_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "dyna:json: value nesting exceeds depth %d",
            DYN_JP_MAX_DEPTH);
        return -1;
    }
    if (JS_IsArray(ctx, a) || JS_IsArray(ctx, b)) {
        uint64_t la, lb, i;
        if (!JS_IsArray(ctx, a) || !JS_IsArray(ctx, b))
            return 0;
        la = dyn_jp_arr_len(ctx, a);
        lb = dyn_jp_arr_len(ctx, b);
        if (la == (uint64_t)-1 || lb == (uint64_t)-1)
            return -1;
        if (la != lb)
            return 0;
        for (i = 0; i < la; i++) {
            JSValue ea = JS_GetPropertyUint32(ctx, a, (uint32_t)i);
            JSValue eb = JS_GetPropertyUint32(ctx, b, (uint32_t)i);
            int r;
            if (JS_IsException(ea) || JS_IsException(eb)) {
                JS_FreeValue(ctx, ea);
                JS_FreeValue(ctx, eb);
                return -1;
            }
            r = dyn_jp_equal(ctx, ea, eb, depth + 1);
            JS_FreeValue(ctx, ea);
            JS_FreeValue(ctx, eb);
            if (r < 0)
                return -1;
            if (r == 0)
                return 0;
        }
        return 1;
    }
    if (JS_IsObject(a) || JS_IsObject(b)) {
        JSPropertyEnum *pa = NULL, *pb = NULL;
        uint32_t na, nb, k;
        if (!JS_IsObject(a) || !JS_IsObject(b))
            return 0;
        if (JS_IsFunction(ctx, a) || JS_IsFunction(ctx, b))
            return JS_StrictEq(ctx, a, b);
        if (JS_GetOwnPropertyNames(ctx, &pa, &na, a,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0)
            return -1;
        if (JS_GetOwnPropertyNames(ctx, &pb, &nb, b,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0) {
            JS_FreePropertyEnum(ctx, pa, na);
            return -1;
        }
        if (na != nb) {
            JS_FreePropertyEnum(ctx, pa, na);
            JS_FreePropertyEnum(ctx, pb, nb);
            return 0;
        }
        for (k = 0; k < na; k++) {
            JSValue va, vb;
            int found, r;
            va = JS_GetProperty(ctx, a, pa[k].atom);
            if (JS_IsException(va)) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            found = dyn_jp_get_own(ctx, b, pa[k].atom, &vb);
            if (found < 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (found == 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
            r = dyn_jp_equal(ctx, va, vb, depth + 1);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
            if (r < 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (r == 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
        }
        JS_FreePropertyEnum(ctx, pa, na);
        JS_FreePropertyEnum(ctx, pb, nb);
        return 1;
    }
    return JS_StrictEq(ctx, a, b);
}

JSValue dyn_jp_throw(JSContext* ctx, const char* pfx, int code,
    const dyn_jp_walk_t* w, const char* p, size_t n)
{
    switch (code) {
    case JP_SYNTAX:
        JS_ThrowTypeError(ctx,
            "%s: pointer must start with '/' or be empty: \"%s\"", pfx, p);
        break;
    case JP_BADESC:
        JS_ThrowTypeError(ctx, "%s: invalid '~' escape in \"%s\"", pfx, p);
        break;
    case JP_LEADZERO:
        JS_ThrowTypeError(ctx,
            "%s: array index has a leading zero in \"%s\"", pfx, p);
        break;
    case JP_TOOBIG:
        JS_ThrowTypeError(ctx,
            "%s: array index in \"%s\" is out of range (over 19 digits)",
            pfx, p);
        break;
    case JP_APPEND:
        JS_ThrowTypeError(ctx,
            "%s: '-' is only valid as the final token of an add path: \"%s\"",
            pfx, p);
        break;
    case JP_NOTNUM:
        JS_ThrowTypeError(ctx,
            "%s: token is not an array index in \"%s\"", pfx, p);
        break;
    case JP_RANGE:
        JS_ThrowTypeError(ctx,
            "%s: array index %llu out of range (length %llu) in \"%s\"", pfx,
            (unsigned long long)w->idx, (unsigned long long)w->alen, p);
        break;
    case JP_SCALAR:
        JS_ThrowTypeError(ctx,
            "%s: cannot descend into a scalar at \"%s\"", pfx, p);
        break;
    case JP_MISSING:
        JS_ThrowTypeError(ctx, "%s: no value at \"%s\"", pfx, p);
        break;
    case JP_TOODEEP:
        JS_ThrowRangeError(ctx, "%s: pointer deeper than %d levels: \"%s\"",
            pfx, DYN_JP_MAX_DEPTH, p);
        break;
    }
    return JS_EXCEPTION;
}

static int dyn_jp_step(JSContext* ctx, JSValue cur, const char* t,
    size_t tlen, dyn_jp_walk_t* w, JSValue* next)
{
    JSAtom a;
    int r;

    if (JS_IsArray(ctx, cur)) {
        uint64_t idx, len;
        if (tlen == 1 && t[0] == '-') {
            w->idx = 0;
            w->alen = 0;
            return JP_APPEND;
        }
        r = dyn_jp_parse_index(t, tlen, &idx);
        if (r == 0)
            return JP_NOTNUM;
        if (r == -1)
            return JP_LEADZERO;
        if (r == -2)
            return JP_TOOBIG;
        len = dyn_jp_arr_len(ctx, cur);
        if (len == (uint64_t)-1)
            return -1;
        if (idx >= len) {
            w->idx = idx;
            w->alen = len;
            return JP_RANGE;
        }
        JSValue el = JS_GetPropertyUint32(ctx, cur, (uint32_t)idx);
        if (JS_IsException(el))
            return -1;
        if (JS_IsUndefined(el)) {
            JSAtom ha = JS_NewAtomUInt32(ctx, (uint32_t)idx);
            JSValue probe;
            int hr;
            if (ha == JS_ATOM_NULL) {
                JS_FreeValue(ctx, el);
                return -1;
            }
            hr = dyn_jp_get_own(ctx, cur, ha, &probe);
            JS_FreeAtom(ctx, ha);
            if (hr < 0) {
                JS_FreeValue(ctx, el);
                return -1;
            }
            if (hr == 0) {
                JS_FreeValue(ctx, el);
                return JP_MISSING;
            }
            JS_FreeValue(ctx, probe);
        }
        *next = el;
        return JP_OK;
    }
    if (!JS_IsObject(cur))
        return JP_SCALAR;
    a = JS_NewAtomLen(ctx, t, tlen);
    if (a == JS_ATOM_NULL)
        return -1;
    r = dyn_jp_get_own(ctx, cur, a, next);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return -1;
    if (r == 0)
        return JP_MISSING;
    return JP_OK;
}

int dyn_jp_walk(JSContext* ctx, JSValueConst root, const char* p,
    size_t n, int keep_last, char* tok, size_t* tlen,
    JSValue* out, dyn_jp_walk_t* w)
{
    char* scratch;
    size_t ntok = 0, k, i, seg;
    JSValue cur, next;
    int code;

    w->idx = 0;
    w->alen = 0;
    if (n == 0) {
        *out = JS_DupValue(ctx, root);
        if (!keep_last) {
            if (tok)
                tok[0] = 0;
            if (tlen)
                *tlen = 0;
        }
        return JP_OK;
    }
    if (p[0] != '/')
        return JP_SYNTAX;
    ntok = 0;
    for (i = 0; i < n; i++)
        if (p[i] == '/')
            ntok++;
    if (ntok > (size_t)DYN_JP_MAX_DEPTH)
        return JP_TOODEEP;
    scratch = js_malloc(ctx, n + 1);
    if (!scratch)
        return -1;
    cur = JS_DupValue(ctx, root);
    seg = 1;
    for (k = 0; k + 1 < ntok; k++) {
        size_t e = seg;
        while (e < n && p[e] != '/')
            e++;
        code = dyn_jp_unescape(p + seg, e - seg, scratch, n + 1, tlen);
        if (code) {
            JS_FreeValue(ctx, cur);
            js_free(ctx, scratch);
            return JP_BADESC;
        }
        code = dyn_jp_step(ctx, cur, scratch, *tlen, w, &next);
        JS_FreeValue(ctx, cur);
        if (code) {
            js_free(ctx, scratch);
            return code;
        }
        cur = next;
        seg = e + 1;
    }
    code = dyn_jp_unescape(p + seg, n - seg, scratch, n + 1, tlen);
    if (code) {
        JS_FreeValue(ctx, cur);
        js_free(ctx, scratch);
        return JP_BADESC;
    }
    if (keep_last) {
        code = dyn_jp_step(ctx, cur, scratch, *tlen, w, &next);
        JS_FreeValue(ctx, cur);
        js_free(ctx, scratch);
        if (code)
            return code;
        *out = next;
        return JP_OK;
    }
    if (tok) {
        memcpy(tok, scratch, *tlen);
        tok[*tlen] = 0;
    }
    js_free(ctx, scratch);
    *out = cur;
    return JP_OK;
}

static int dyn_jp_arr_insert(JSContext* ctx, JSValue arr, uint64_t idx,
    JSValue val)
{
    uint64_t len, i;
    len = dyn_jp_arr_len(ctx, arr);
    if (len == (uint64_t)-1)
        return -1;
    for (i = len; i > idx; i--) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)(i - 1));
        if (JS_IsException(el))
            return -1;
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, el, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            return -1;
    }
    return JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)idx, val, JS_PROP_C_W_E | JS_PROP_THROW);
}

static int dyn_jp_arr_remove(JSContext* ctx, JSValue arr, uint64_t idx)
{
    uint64_t len, i;
    len = dyn_jp_arr_len(ctx, arr);
    if (len == (uint64_t)-1)
        return -1;
    for (i = idx; i + 1 < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)(i + 1));
        if (JS_IsException(el))
            return -1;
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, el, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            return -1;
    }
    return JS_SetPropertyStr(ctx, arr, "length",
        JS_NewInt64(ctx, (int64_t)(len - 1)));
}

static void dyn_jp_at_free(JSContext* ctx, dyn_jp_at_t* at)
{
    JS_FreeValue(ctx, at->parent);
    if (at->scratch)
        js_free(ctx, at->scratch);
    at->parent = JS_UNDEFINED;
    at->scratch = NULL;
    at->tlen = 0;
    at->idx = 0;
}

static dyn_jp_at_kind_t dyn_jp_resolve_at(JSContext* ctx, JSValue* work,
    const char* pfx, const char* label,
    const char* range_tail, int is_add,
    const char* path, size_t plen,
    dyn_jp_at_t* at)
{
    dyn_jp_walk_t w;
    int code;

    at->parent = JS_UNDEFINED;
    at->scratch = NULL;
    at->tlen = 0;
    at->idx = 0;
    at->scratch = js_malloc(ctx, plen + 1);
    if (!at->scratch)
        return DYN_JP_AT_ERR;
    code = dyn_jp_walk(ctx, *work, path, plen, 0, at->scratch, &at->tlen,
        &at->parent, &w);
    if (code) {
        if (is_add && code == JP_MISSING)
            JS_ThrowTypeError(ctx,
                "%s: %sparent does not exist at \"%s\"", pfx, label, path);
        else
            dyn_jp_throw(ctx, pfx, code, &w, path, plen);
        dyn_jp_at_free(ctx, at);
        return DYN_JP_AT_ERR;
    }
    if (JS_IsArray(ctx, at->parent)) {
        uint64_t len, idx;
        int r;
        len = dyn_jp_arr_len(ctx, at->parent);
        if (len == (uint64_t)-1) {
            dyn_jp_at_free(ctx, at);
            return DYN_JP_AT_ERR;
        }
        if (is_add && at->tlen == 1 && at->scratch[0] == '-') {
            at->idx = len;
            return DYN_JP_AT_ARRAY;
        }
        r = dyn_jp_parse_index(at->scratch, at->tlen, &idx);
        if (r == 0)
            JS_ThrowTypeError(ctx,
                "%s: %stoken is not an array index at \"%s\"", pfx, label,
                path);
        else if (r == -1)
            JS_ThrowTypeError(ctx,
                "%s: %sarray index has a leading zero at \"%s\"", pfx, label,
                path);
        else if (r == -2)
            JS_ThrowTypeError(ctx,
                "%s: %sarray index in \"%s\" is out of range (over 19 digits)",
                pfx, label, path);
        else if (idx > len || (!is_add && idx >= len)) {
            if (is_add)
                JS_ThrowTypeError(ctx,
                    "%s: %sarray index %llu out of range (length %llu) at "
                    "\"%s\"",
                    pfx, label, (unsigned long long)idx,
                    (unsigned long long)len, path);
            else
                JS_ThrowTypeError(ctx, "%s: %s%sat \"%s\"", pfx, label,
                    range_tail, path);
            r = -1;
        }
        if (r != 1) {
            dyn_jp_at_free(ctx, at);
            return DYN_JP_AT_ERR;
        }
        at->idx = idx;
        return DYN_JP_AT_ARRAY;
    }
    if (!JS_IsObject(at->parent)) {
        JS_ThrowTypeError(ctx,
            "%s: %scannot descend into a scalar at \"%s\"", pfx, label, path);
        dyn_jp_at_free(ctx, at);
        return DYN_JP_AT_ERR;
    }
    return DYN_JP_AT_MEMBER;
}

int dyn_jp_add_at(JSContext* ctx, JSValue* work, const char* pfx,
    const char* path, size_t plen, JSValue vadd)
{
    dyn_jp_at_t at;
    dyn_jp_at_kind_t kind;

    if (plen == 0) {
        JS_FreeValue(ctx, *work);
        *work = vadd;
        return 0;
    }
    kind = dyn_jp_resolve_at(ctx, work, pfx, "add: ", NULL, 1, path, plen,
        &at);
    if (kind == DYN_JP_AT_ERR) {
        JS_FreeValue(ctx, vadd);
        return -1;
    }
    if (kind == DYN_JP_AT_ARRAY) {
        if (dyn_jp_arr_insert(ctx, at.parent, at.idx, vadd) < 0) {
            dyn_jp_at_free(ctx, &at);
            return -1;
        }
    } else {
        JSAtom a = JS_NewAtomLen(ctx, at.scratch, at.tlen);
        int r;
        if (a == JS_ATOM_NULL) {
            dyn_jp_at_free(ctx, &at);
            JS_FreeValue(ctx, vadd);
            return -1;
        }
        r = JS_DefinePropertyValue(ctx, at.parent, a, vadd, JS_PROP_C_W_E | JS_PROP_THROW);
        JS_FreeAtom(ctx, a);
        if (r < 0) {
            dyn_jp_at_free(ctx, &at);
            return -1;
        }
    }
    dyn_jp_at_free(ctx, &at);
    return 0;
}

int dyn_jp_remove_at(JSContext* ctx, JSValue* work, const char* pfx,
    const char* path, size_t plen)
{
    dyn_jp_at_t at;
    dyn_jp_at_kind_t kind;
    int r = -1;

    if (plen == 0) {
        JS_ThrowTypeError(ctx, "%s: cannot remove the document root", pfx);
        return -1;
    }
    kind = dyn_jp_resolve_at(ctx, work, pfx, "", "array index out of range ",
        0, path, plen, &at);
    if (kind == DYN_JP_AT_ERR)
        return -1;
    if (kind == DYN_JP_AT_ARRAY) {
        r = dyn_jp_arr_remove(ctx, at.parent, at.idx);
    } else {
        JSAtom a = JS_NewAtomLen(ctx, at.scratch, at.tlen);
        JSValue old;
        int found;
        if (a == JS_ATOM_NULL) {
            dyn_jp_at_free(ctx, &at);
            return -1;
        }
        found = dyn_jp_get_own(ctx, at.parent, a, &old);
        if (found == 0) {
            JS_ThrowTypeError(ctx, "%s: no value at \"%s\"", pfx, path);
        } else if (found < 0) {
            ;
        } else {
            JS_FreeValue(ctx, old);
            r = JS_DeleteProperty(ctx, at.parent, a, 0);
        }
        JS_FreeAtom(ctx, a);
    }
    dyn_jp_at_free(ctx, &at);
    return r;
}

static int dyn_jp_op_val(JSContext* ctx, JSValueConst op, const char* pfx,
    const char* name, JSValue* out)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;
    if (a == JS_ATOM_NULL)
        return -1;
    r = dyn_jp_get_own(ctx, op, a, out);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return -1;
    if (r == 0) {
        JS_ThrowTypeError(ctx, "%s: missing \"%s\"", pfx, name);
        return -1;
    }
    return 0;
}

static int dyn_jp_op_str(JSContext* ctx, JSValueConst op, const char* pfx,
    const char* name, const char** s, size_t* n)
{
    JSValue v;
    if (dyn_jp_op_val(ctx, op, pfx, name, &v) < 0)
        return -1;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "%s: \"%s\" must be a string", pfx, name);
        JS_FreeValue(ctx, v);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    JS_FreeValue(ctx, v);
    if (!*s)
        return -1;
    return 0;
}

static int dyn_jp_op_test(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* path, size_t plen)
{
    JSValue have, want;
    size_t tlen;
    dyn_jp_walk_t w;
    int code, eq;

    if (dyn_jp_op_val(ctx, op, pfx, "value", &want) < 0)
        return -1;
    code = dyn_jp_walk(ctx, *work, path, plen, 1, NULL, &tlen, &have, &w);
    if (code) {
        if (code == JP_MISSING)
            JS_ThrowTypeError(ctx, "%s: test: no value at \"%s\"", pfx, path);
        else
            dyn_jp_throw(ctx, pfx, code, &w, path, plen);
        JS_FreeValue(ctx, want);
        return -1;
    }
    eq = dyn_jp_equal(ctx, have, want, 0);
    JS_FreeValue(ctx, have);
    JS_FreeValue(ctx, want);
    if (eq < 0)
        return -1;
    if (!eq) {
        JS_ThrowTypeError(ctx, "%s: test failed at \"%s\"", pfx, path);
        return -1;
    }
    return 0;
}

static int dyn_jp_op_add(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* path, size_t plen)
{
    JSValue val, vadd;
    if (dyn_jp_op_val(ctx, op, pfx, "value", &val) < 0)
        return -1;
    vadd = dyn_jp_clone(ctx, val, 0);
    JS_FreeValue(ctx, val);
    if (JS_IsException(vadd))
        return -1;
    if (dyn_jp_add_at(ctx, work, pfx, path, plen, vadd) < 0)
        return -1;
    return 0;
}

static int dyn_jp_op_remove(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* path, size_t plen)
{
    return dyn_jp_remove_at(ctx, work, pfx, path, plen);
}

static int dyn_jp_op_replace(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* path, size_t plen)
{
    JSValue val, vrep;
    dyn_jp_at_t at;
    dyn_jp_at_kind_t kind;
    int r = -1;

    if (dyn_jp_op_val(ctx, op, pfx, "value", &val) < 0)
        return -1;
    vrep = dyn_jp_clone(ctx, val, 0);
    JS_FreeValue(ctx, val);
    if (JS_IsException(vrep))
        return -1;
    if (plen == 0) {
        JS_FreeValue(ctx, *work);
        *work = vrep;
        return 0;
    }
    kind = dyn_jp_resolve_at(ctx, work, pfx, "replace: ", "no value ", 0,
        path, plen, &at);
    if (kind == DYN_JP_AT_ERR) {
        JS_FreeValue(ctx, vrep);
        return -1;
    }
    if (kind == DYN_JP_AT_ARRAY) {
        r = JS_DefinePropertyValueUint32(ctx, at.parent, (uint32_t)at.idx, vrep, JS_PROP_C_W_E | JS_PROP_THROW);
    } else {
        JSAtom a = JS_NewAtomLen(ctx, at.scratch, at.tlen);
        JSValue old;
        int found;
        if (a == JS_ATOM_NULL) {
            dyn_jp_at_free(ctx, &at);
            JS_FreeValue(ctx, vrep);
            return -1;
        }
        found = dyn_jp_get_own(ctx, at.parent, a, &old);
        if (found == 0) {
            JS_ThrowTypeError(ctx, "%s: replace: no value at \"%s\"", pfx,
                path);
            JS_FreeValue(ctx, vrep);
        } else if (found < 0) {
            JS_FreeValue(ctx, vrep);
        } else {
            JS_FreeValue(ctx, old);
            r = JS_DefinePropertyValue(ctx, at.parent, a, vrep, JS_PROP_C_W_E | JS_PROP_THROW);
        }
        JS_FreeAtom(ctx, a);
    }
    dyn_jp_at_free(ctx, &at);
    return r;
}

static int dyn_jp_is_prefix(JSContext* ctx, const char* from, size_t fn,
    const char* path, size_t pn)
{
    size_t ftok = 0, ptok = 0, i, fseg = 1, pseg = 1;
    char *fb, *pb;
    if (fn == 0)
        return pn > 0;
    if (pn == 0)
        return 0;
    for (i = 0; i < fn; i++)
        if (from[i] == '/')
            ftok++;
    for (i = 0; i < pn; i++)
        if (path[i] == '/')
            ptok++;
    if (ftok >= ptok)
        return 0;
    fb = js_malloc(ctx, fn + 1);
    pb = js_malloc(ctx, pn + 1);
    if (!fb || !pb) {
        js_free(ctx, fb);
        js_free(ctx, pb);
        return -1;
    }
    for (i = 0; i < ftok; i++) {
        size_t fe = fseg, pe = pseg, fl, pl;
        while (fe < fn && from[fe] != '/')
            fe++;
        while (pe < pn && path[pe] != '/')
            pe++;
        if (dyn_jp_unescape(from + fseg, fe - fseg, fb, fn + 1, &fl) || dyn_jp_unescape(path + pseg, pe - pseg, pb, pn + 1, &pl)) {
            js_free(ctx, fb);
            js_free(ctx, pb);
            return 0;
        }
        if (fl != pl || memcmp(fb, pb, fl) != 0) {
            js_free(ctx, fb);
            js_free(ctx, pb);
            return 0;
        }
        fseg = fe + 1;
        pseg = pe + 1;
    }
    js_free(ctx, fb);
    js_free(ctx, pb);
    return 1;
}

static int dyn_jp_op_move(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* from, size_t flen,
    const char* path, size_t plen)
{
    JSValue mval;
    size_t tlen;
    dyn_jp_walk_t w;
    int code, pr;

    pr = dyn_jp_is_prefix(ctx, from, flen, path, plen);
    if (pr < 0)
        return -1;
    if (pr > 0) {
        JS_ThrowTypeError(ctx,
            "%s: move: \"from\" is a prefix of \"path\"", pfx);
        return -1;
    }
    code = dyn_jp_walk(ctx, *work, from, flen, 1, NULL, &tlen, &mval, &w);
    if (code) {
        if (code == JP_MISSING)
            JS_ThrowTypeError(ctx,
                "%s: move: \"from\" location does not exist at \"%s\"", pfx,
                from);
        else
            dyn_jp_throw(ctx, pfx, code, &w, from, flen);
        return -1;
    }
    if (dyn_jp_remove_at(ctx, work, pfx, from, flen) < 0) {
        JS_FreeValue(ctx, mval);
        return -1;
    }
    if (dyn_jp_add_at(ctx, work, pfx, path, plen, mval) < 0)
        return -1;
    return 0;
}

static int dyn_jp_op_copy(JSContext* ctx, JSValue* work, JSValueConst op,
    const char* pfx, const char* from, size_t flen,
    const char* path, size_t plen)
{
    JSValue cval, vcopy;
    size_t tlen;
    dyn_jp_walk_t w;
    int code;

    code = dyn_jp_walk(ctx, *work, from, flen, 1, NULL, &tlen, &cval, &w);
    if (code) {
        if (code == JP_MISSING)
            JS_ThrowTypeError(ctx,
                "%s: copy: \"from\" location does not exist at \"%s\"", pfx,
                from);
        else
            dyn_jp_throw(ctx, pfx, code, &w, from, flen);
        return -1;
    }
    vcopy = dyn_jp_clone(ctx, cval, 0);
    JS_FreeValue(ctx, cval);
    if (JS_IsException(vcopy))
        return -1;
    if (dyn_jp_add_at(ctx, work, pfx, path, plen, vcopy) < 0)
        return -1;
    return 0;
}

static int dyn_jp_do_op(JSContext* ctx, JSValue* work, JSValueConst op,
    int idx)
{
    char pfx[48];
    const char *ostr = NULL, *path = NULL, *from = NULL;
    size_t olen = 0, plen = 0, flen = 0;
    int r = -1;

    snprintf(pfx, sizeof pfx, "Patch.apply[%d]", idx);
    if (dyn_jp_op_str(ctx, op, pfx, "op", &ostr, &olen) < 0)
        return -1;
    if (dyn_jp_op_str(ctx, op, pfx, "path", &path, &plen) < 0)
        goto out;
    if (plen > DYN_JP_MAX_PTR) {
        JS_ThrowRangeError(ctx, "%s: path longer than %d bytes", pfx,
            DYN_JP_MAX_PTR);
        r = -1;
        goto out;
    }
    if (dyn_jp_eqs(ostr, olen, "add"))
        r = dyn_jp_op_add(ctx, work, op, pfx, path, plen);
    else if (dyn_jp_eqs(ostr, olen, "remove"))
        r = dyn_jp_op_remove(ctx, work, op, pfx, path, plen);
    else if (dyn_jp_eqs(ostr, olen, "replace"))
        r = dyn_jp_op_replace(ctx, work, op, pfx, path, plen);
    else if (dyn_jp_eqs(ostr, olen, "test"))
        r = dyn_jp_op_test(ctx, work, op, pfx, path, plen);
    else if (dyn_jp_eqs(ostr, olen, "move")) {
        if (dyn_jp_op_str(ctx, op, pfx, "from", &from, &flen) < 0) {
            r = -1;
            goto out;
        }
        if (flen > DYN_JP_MAX_PTR) {
            JS_ThrowRangeError(ctx, "%s: \"from\" longer than %d bytes", pfx,
                DYN_JP_MAX_PTR);
            r = -1;
            goto out;
        }
        r = dyn_jp_op_move(ctx, work, op, pfx, from, flen, path, plen);
    } else if (dyn_jp_eqs(ostr, olen, "copy")) {
        if (dyn_jp_op_str(ctx, op, pfx, "from", &from, &flen) < 0) {
            r = -1;
            goto out;
        }
        if (flen > DYN_JP_MAX_PTR) {
            JS_ThrowRangeError(ctx, "%s: \"from\" longer than %d bytes", pfx,
                DYN_JP_MAX_PTR);
            r = -1;
            goto out;
        }
        r = dyn_jp_op_copy(ctx, work, op, pfx, from, flen, path, plen);
    } else {
        JS_ThrowTypeError(ctx, "%s: unknown op \"%.*s\"", pfx, (int)olen,
            ostr);
        r = -1;
    }
out:
    if (ostr)
        JS_FreeCString(ctx, ostr);
    if (path)
        JS_FreeCString(ctx, path);
    if (from)
        JS_FreeCString(ctx, from);
    return r;
}

static JSValue dyn_jp_apply(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    JSValue work, lenv, opv;
    uint64_t nops, i;
    int r;

    if (argc < 2 || !JS_IsArray(ctx, argv[1])) {
        JS_ThrowTypeError(ctx,
            "Patch.apply(doc, ops): ops must be an array");
        return JS_EXCEPTION;
    }
    work = dyn_jp_clone(ctx, argv[0], 0);
    if (JS_IsException(work))
        return JS_EXCEPTION;
    lenv = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToIndex(ctx, &nops, lenv)) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, work);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nops > DYN_JP_MAX_OPS) {
        JS_FreeValue(ctx, work);
        JS_ThrowRangeError(ctx,
            "Patch.apply: too many ops (%llu; cap is %d)",
            (unsigned long long)nops, DYN_JP_MAX_OPS);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nops; i++) {
        opv = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        if (JS_IsException(opv)) {
            JS_FreeValue(ctx, work);
            return JS_EXCEPTION;
        }
        if (!JS_IsObject(opv)) {
            JS_ThrowTypeError(ctx, "Patch.apply[%llu]: op must be an object",
                (unsigned long long)i);
            JS_FreeValue(ctx, opv);
            JS_FreeValue(ctx, work);
            return JS_EXCEPTION;
        }
        r = dyn_jp_do_op(ctx, &work, opv, (int)i);
        JS_FreeValue(ctx, opv);
        if (r < 0) {
            JS_FreeValue(ctx, work);
            return JS_EXCEPTION;
        }
    }
    return work;
}

static const JSCFunctionListEntry dyn_jp_patch_funcs[] = {
    JS_CFUNC_MAGIC_DEF("apply", 2, dyn_jp_apply, 0),
};

const JSCFunctionListEntry dyn_jp_funcs[] = {
    JS_OBJECT_DEF("Pointer", dyn_jp_pointer_funcs,
        countof(dyn_jp_pointer_funcs),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("Patch", dyn_jp_patch_funcs, countof(dyn_jp_patch_funcs),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("Ndjson", dyn_ndjson_funcs, countof(dyn_ndjson_funcs),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
};

int dyn_jp_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dyn_jp_funcs, countof(dyn_jp_funcs));
}

#endif
