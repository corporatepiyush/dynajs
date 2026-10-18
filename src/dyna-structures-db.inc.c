#include "core/dyn-hash.h"

static _Atomic JSClassID sdb_ta_cid[JS_TYPED_ARRAY_FLOAT64 + 1];

static void sdb_capture_typed_cids(JSContext* ctx)
{
    int t;
    for (t = 0; t <= JS_TYPED_ARRAY_FLOAT64; t++) {
        JSValueConst args[1];
        JSValue ta;
        args[0] = JS_NewInt32(ctx, 0);
        ta = JS_NewTypedArray(ctx, 1, args, (JSTypedArrayEnum)t);
        if (JS_IsException(ta)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        atomic_store_explicit(&sdb_ta_cid[t], JS_GetClassID(ta), memory_order_relaxed);
        JS_FreeValue(ctx, ta);
    }
}

static int sdb_typed_kind(JSValueConst v)
{
    JSClassID cid;
    int t;
    if (!JS_IsObject(v))
        return -1;
    cid = JS_GetClassID(v);
    for (t = 0; t <= JS_TYPED_ARRAY_FLOAT64; t++)
        if (cid == atomic_load_explicit(&sdb_ta_cid[t], memory_order_relaxed))
            return t;
    return -1;
}

static const uint8_t* sdb_typed_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return NULL;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "typed array buffer is detached");
        return NULL;
    }
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return NULL;
    }
    *plen = len;
    return base + off;
}

static double sdb_typed_at(const uint8_t* p, int kind, size_t i)
{
    switch (kind) {
    case JS_TYPED_ARRAY_INT8: {
        int8_t x;
        memcpy(&x, p + i, 1);
        return x;
    }
    case JS_TYPED_ARRAY_UINT8:
    case JS_TYPED_ARRAY_UINT8C:
        return p[i];
    case JS_TYPED_ARRAY_INT16: {
        int16_t x;
        memcpy(&x, p + i * 2, 2);
        return x;
    }
    case JS_TYPED_ARRAY_UINT16: {
        uint16_t x;
        memcpy(&x, p + i * 2, 2);
        return x;
    }
    case JS_TYPED_ARRAY_INT32: {
        int32_t x;
        memcpy(&x, p + i * 4, 4);
        return x;
    }
    case JS_TYPED_ARRAY_UINT32: {
        uint32_t x;
        memcpy(&x, p + i * 4, 4);
        return x;
    }
    case JS_TYPED_ARRAY_FLOAT32: {
        float x;
        memcpy(&x, p + i * 4, 4);
        return (double)x;
    }
    case JS_TYPED_ARRAY_FLOAT64: {
        double x;
        memcpy(&x, p + i * 8, 8);
        return x;
    }
    default:
        return (double)NAN;
    }
}

static size_t sdb_typed_bpe(int kind)
{
    switch (kind) {
    case JS_TYPED_ARRAY_INT8:
    case JS_TYPED_ARRAY_UINT8:
    case JS_TYPED_ARRAY_UINT8C:
        return 1;
    case JS_TYPED_ARRAY_INT16:
    case JS_TYPED_ARRAY_UINT16:
    case JS_TYPED_ARRAY_FLOAT16:
        return 2;
    case JS_TYPED_ARRAY_FLOAT64:
    case JS_TYPED_ARRAY_BIG_INT64:
    case JS_TYPED_ARRAY_BIG_UINT64:
        return 8;
    default:
        return 4;
    }
}

static int sdb_f64_list(JSContext* ctx, JSValueConst v, const char* what,
    double** out, uint32_t* pn)
{
    int kind = sdb_typed_kind(v);
    double* d;
    uint32_t n, i;

    *out = NULL;
    *pn = 0;
    if (kind >= 0) {
        size_t len, bpe = sdb_typed_bpe(kind);
        const uint8_t* p;
        if (kind == JS_TYPED_ARRAY_BIG_INT64 || kind == JS_TYPED_ARRAY_BIG_UINT64 || kind == JS_TYPED_ARRAY_FLOAT16) {
            JS_ThrowTypeError(ctx, "%s: BigInt and Float16 arrays are not accepted", what);
            return -1;
        }
        p = sdb_typed_bytes(ctx, v, &len);
        if (!p)
            return -1;
        if (len / bpe > DYN_DS_MAX_ITEMS) {
            JS_ThrowRangeError(ctx, "%s: more than %u values", what, DYN_DS_MAX_ITEMS);
            return -1;
        }
        n = (uint32_t)(len / bpe);
        d = (double*)dyn_ds_malloc(sizeof(double) * ((size_t)n + 1));
        if (!d) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < n; i++)
            d[i] = sdb_typed_at(p, kind, i);
        *out = d;
        *pn = n;
        return 0;
    }
    if (JS_IsArray(ctx, v) > 0) {
        JSValue* arr;
        uint32_t cnt;
        if (JS_GetFastArray(ctx, v, &arr, &cnt)) {
            if (cnt > DYN_DS_MAX_ITEMS) {
                JS_ThrowRangeError(ctx, "%s: more than %u values", what, DYN_DS_MAX_ITEMS);
                return -1;
            }
            d = (double*)dyn_ds_malloc(sizeof(double) * ((size_t)cnt + 1));
            if (!d) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            for (i = 0; i < cnt; i++) {
                int tag = JS_VALUE_GET_TAG(arr[i]);
                if (tag == JS_TAG_INT)
                    d[i] = JS_VALUE_GET_INT(arr[i]);
                else if (JS_TAG_IS_FLOAT64(tag))
                    d[i] = JS_VALUE_GET_FLOAT64(arr[i]);
                else {
                    dyn_ds_free(d);
                    JS_ThrowTypeError(ctx, "%s: element %u is not a number", what, (unsigned)i);
                    return -1;
                }
            }
            *out = d;
            *pn = cnt;
            return 0;
        } else {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t len;
            if (JS_ToUint32(ctx, &len, lv)) {
                JS_FreeValue(ctx, lv);
                return -1;
            }
            JS_FreeValue(ctx, lv);
            if (len > DYN_DS_MAX_ITEMS) {
                JS_ThrowRangeError(ctx, "%s: more than %u values", what, DYN_DS_MAX_ITEMS);
                return -1;
            }
            d = (double*)dyn_ds_malloc(sizeof(double) * 16);
            if (!d) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            n = 0;
            for (i = 0; i < len; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, i);
                if (JS_IsException(e)) {
                    dyn_ds_free(d);
                    return -1;
                }
                if (!JS_IsNumber(e)) {
                    JS_FreeValue(ctx, e);
                    dyn_ds_free(d);
                    JS_ThrowTypeError(ctx, "%s: element %u is not a number", what, (unsigned)i);
                    return -1;
                }
                if ((n & (n - 1)) == 0 && n >= 16) {
                    double* nd = (double*)dyn_ds_realloc(d, sizeof(double) * (size_t)n * 2);
                    if (!nd) {
                        dyn_ds_free(d);
                        JS_ThrowOutOfMemory(ctx);
                        return -1;
                    }
                    d = nd;
                }
                JS_ToFloat64(ctx, &d[n++], e);
            }
            *out = d;
            *pn = n;
            return 0;
        }
    }
    JS_ThrowTypeError(ctx, "%s: expected an array or a typed array of numbers", what);
    return -1;
}

static int sdb_u32_list(JSContext* ctx, JSValueConst v, const char* what,
    uint32_t** out, uint32_t* pn)
{
    int kind = sdb_typed_kind(v);
    double* d;
    uint32_t* u;
    uint32_t n, i;

    if (kind == JS_TYPED_ARRAY_UINT32 || kind == JS_TYPED_ARRAY_INT32) {
        size_t len;
        const uint8_t* p = sdb_typed_bytes(ctx, v, &len);
        if (!p)
            return -1;
        if (len / 4 > DYN_DS_MAX_ITEMS) {
            JS_ThrowRangeError(ctx, "%s: more than %u values", what, DYN_DS_MAX_ITEMS);
            return -1;
        }
        n = (uint32_t)(len / 4);
        u = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)n + 1));
        if (!u) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(u, p, (size_t)n * 4);
        if (kind == JS_TYPED_ARRAY_INT32)
            for (i = 0; i < n; i++)
                if ((int32_t)u[i] < 0) {
                    dyn_ds_free(u);
                    JS_ThrowRangeError(ctx, "%s: element %u is negative", what, (unsigned)i);
                    return -1;
                }
        *out = u;
        *pn = n;
        return 0;
    }
    if (sdb_f64_list(ctx, v, what, &d, &n))
        return -1;
    u = (uint32_t*)d;
    for (i = 0; i < n; i++) {
        double x = d[i];
        if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
            dyn_ds_free(d);
            JS_ThrowRangeError(ctx, "%s: element %u is not an integer in 0..2^32-1", what, (unsigned)i);
            return -1;
        }
        u[i] = (uint32_t)x;
    }
    *out = u;
    *pn = n;
    return 0;
}

static int sdb_u32_arg(JSContext* ctx, JSValueConst v, const char* what, uint32_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v))
        return -1;
    if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
        JS_ThrowRangeError(ctx, "%s must be an integer in 0..2^32-1", what);
        return -1;
    }
    *out = (uint32_t)x;
    return 0;
}

static int sdb_num_arg(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (*out != *out) {
        JS_ThrowTypeError(ctx, "%s must not be NaN", what);
        return -1;
    }
    return 0;
}

static int sdb_finite_arg(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (!isfinite(*out)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number", what);
        return -1;
    }
    return 0;
}

static int sdb_opt_get(JSContext* ctx, JSValueConst opts, const char* name, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    *out = JS_GetPropertyStr(ctx, opts, name);
    return JS_IsException(*out) ? -1 : 0;
}

static int sdb_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, int* out)
{
    JSValue v;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v))
        *out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return 0;
}

static int sdb_opt_u32(JSContext* ctx, JSValueConst opts, const char* name, uint32_t* out)
{
    JSValue v;
    int rc = 0;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        double x;
        if (JS_ToFloat64(ctx, &x, v))
            rc = -1;
        else if (x == HUGE_VAL)
            *out = UINT32_MAX;
        else if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
            JS_ThrowRangeError(ctx, "%s must be a non-negative integer", name);
            rc = -1;
        } else
            *out = (uint32_t)x;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static int sdb_opt_f64(JSContext* ctx, JSValueConst opts, const char* name, double* out)
{
    JSValue v;
    int rc = 0;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v) && JS_ToFloat64(ctx, out, v))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

static JSValue sdb_typed_out(JSContext* ctx, const void* data, size_t n, JSTypedArrayEnum t)
{
    static const uint64_t zero = 0;
    JSValue ab, out;
    JSValueConst a[3];
    size_t bpe = sdb_typed_bpe((int)t);
    ab = JS_NewArrayBufferCopy(ctx, n ? (const uint8_t*)data : (const uint8_t*)&zero, n * bpe);
    if (JS_IsException(ab))
        return ab;
    a[0] = ab;
    a[1] = JS_NewInt32(ctx, 0);
    a[2] = JS_NewInt64(ctx, (int64_t)n);
    out = JS_NewTypedArray(ctx, 3, a, t);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue sdb_pair(JSContext* ctx, JSValue a, JSValue b)
{
    JSValue p = JS_NewArray(ctx);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return p;
    }
    if (JS_DefinePropertyValueUint32(ctx, p, 0, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, p, 1, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static int sdb_push(JSContext* ctx, JSValueConst arr, uint32_t* i, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueUint32(ctx, arr, (*i)++, v, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

static int sdb_index_arg(JSContext* ctx, JSValueConst v, const char* what, int64_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v))
        return -1;
    if (x != x || x != trunc(x)) {
        JS_ThrowRangeError(ctx, "%s must be an integer", what);
        return -1;
    }
    if (x > 9007199254740992.0)
        x = 9007199254740992.0;
    if (x < -9007199254740992.0)
        x = -9007199254740992.0;
    *out = (int64_t)x;
    return 0;
}

static int sdb_zsl_init(dyn_zsl_t* s, JSContext* ctx)
{
    return dyn_zsl_init(s, ds_cell_release, JS_GetRuntime(ctx));
}

static int sdb_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, JSValue val, dyn_zsl_node** out)
{
    dyn_cell_t c = ds_val_cell(val);
    return dyn_zsl_insert(s, score, k, n, &c, out);
}

static void sdb_zsl_mark(JSRuntime* rt, const dyn_zsl_t* s, JS_MarkFunc* mark_func)
{
    dyn_zsl_node* x;
    if (!s->head)
        return;
    for (x = s->head->lv[0].fwd; x; x = x->lv[0].fwd)
        JS_MarkValue(rt, ds_cell_val(&x->val), mark_func);
}

static JSValue dyn_zsl_keystr(JSContext* ctx, const dyn_zsl_node* x)
{
    return JS_NewStringLen(ctx, x->key, x->klen);
}

static int sdb_norm_range(int64_t start, int64_t stop, uint32_t len, uint32_t* a, uint32_t* b)
{
    if (start < 0)
        start += len;
    if (stop < 0)
        stop += len;
    if (start < 0)
        start = 0;
    if (start > stop || start >= (int64_t)len)
        return 0;
    if (stop >= (int64_t)len)
        stop = (int64_t)len - 1;
    *a = (uint32_t)start;
    *b = (uint32_t)stop;
    return 1;
}

typedef struct {
    dyn_zsl_t sl;
} sdb_zset_w;

static JSClassID sdb_zset_class_id;

static void sdb_zset_free(void* native)
{
    sdb_zset_w* w = (sdb_zset_w*)native;
    if (!w)
        return;
    dyn_zsl_destroy(&w->sl);
    dyn_ds_free(w);
}

static void sdb_zset_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_zset_free(JS_GetOpaque(val, sdb_zset_class_id));
}

static const JSClassDef sdb_zset_class = {
    "ZSet",
    .finalizer = sdb_zset_finalizer,
};

static sdb_zset_w* sdb_zset_new(JSContext* ctx)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return NULL;
    if (sdb_zsl_init(&w->sl, ctx) < 0) {
        dyn_ds_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_zset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    (void)argc;
    (void)argv;
    w = sdb_zset_new(ctx);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_zset_class_id, w, sdb_zset_free);
}

static int sdb_zset_put(sdb_zset_w* w, const char* k, size_t n, double score, int incr, double* result, int* added)
{
    dyn_zsl_node* x = dyn_zsl_find(&w->sl, k, n);
    *added = 0;
    if (x) {
        double ns = incr ? x->score + score : score;
        if (ns != ns)
            return -2;
        *result = ns;
        if (ns == x->score)
            return 0;
        dyn_zsl_unlink(&w->sl, x);
        x->score = ns;
        dyn_zsl_link(&w->sl, x);
        return 0;
    }
    *result = score;
    if (sdb_zsl_insert(&w->sl, score, k, n, JS_UNDEFINED, NULL) < 0)
        return -1;
    *added = 1;
    return 0;
}

static JSValue sdb_zset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    double sc, res;
    int added, rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (sdb_num_arg(ctx, argv[1], magic ? "delta" : "score", &sc)) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_zset_put(w, k.p, k.n, sc, magic, &res, &added);
    ds2_str_release(ctx, &k);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "incrBy: resulting score is NaN");
    if (rc < 0)
        return ds2_oom(ctx);
    return magic ? JS_NewFloat64(ctx, res) : JS_NewBool(ctx, added);
}

static JSValue sdb_zset_member_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    dyn_zsl_node* x;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    x = dyn_zsl_find(&w->sl, k.p, k.n);
    ds2_str_release(ctx, &k);
    switch (magic) {
    case 0:
        return x ? JS_NewFloat64(ctx, x->score) : JS_UNDEFINED;
    case 1:
        return JS_NewBool(ctx, x != NULL);
    case 2:
        if (!x)
            return JS_FALSE;
        dyn_zsl_drop(&w->sl, x);
        return JS_TRUE;
    case 3:
        return x ? JS_NewInt64(ctx, (int64_t)dyn_zsl_rank(&w->sl, x) - 1) : JS_UNDEFINED;
    default:
        return x ? JS_NewInt64(ctx, (int64_t)w->sl.len - (int64_t)dyn_zsl_rank(&w->sl, x)) : JS_UNDEFINED;
    }
}

static JSValue sdb_zset_entry(JSContext* ctx, const dyn_zsl_node* x)
{
    return sdb_pair(ctx, dyn_zsl_keystr(ctx, x), JS_NewFloat64(ctx, x->score));
}

static JSValue sdb_zset_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t i;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = dyn_zsl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_zset_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_zset_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    int64_t start = 0, stop = -1;
    int reverse = 0;
    uint32_t a, b, i, out = 0;
    dyn_zsl_node* x;
    JSValue arr;
    if (!magic) {
        if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
            return JS_EXCEPTION;
        if (argc > 2 && sdb_opt_bool(ctx, argv[2], "reverse", &reverse))
            return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || !sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return arr;
    x = reverse ? dyn_zsl_at(&w->sl, w->sl.len - a) : dyn_zsl_at(&w->sl, a + 1);
    for (i = a; x && i <= b; i++) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_by_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    double lo, hi;
    uint32_t offset = 0, limit = UINT32_MAX, r1, r2, out = 0, n;
    int reverse = 0;
    dyn_zsl_node* x;
    JSValue arr;
    if (sdb_num_arg(ctx, argv[0], "min", &lo) || sdb_num_arg(ctx, argv[1], "max", &hi))
        return JS_EXCEPTION;
    if (magic == 0 && argc > 2 && (sdb_opt_u32(ctx, argv[2], "offset", &offset) || sdb_opt_u32(ctx, argv[2], "limit", &limit) || sdb_opt_bool(ctx, argv[2], "reverse", &reverse)))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_first_ge_score(&w->sl, lo, &r1);
    dyn_zsl_last_le_score(&w->sl, hi, &r2);
    n = r2 >= r1 ? r2 - r1 + 1 : 0;
    if (magic == 1)
        return JS_NewInt64(ctx, n);
    if (magic == 2) {
        uint32_t k;
        x = n ? dyn_zsl_at(&w->sl, r1) : NULL;
        for (k = 0; k < n && x; k++) {
            dyn_zsl_node* nx = x->lv[0].fwd;
            dyn_zsl_drop(&w->sl, x);
            x = nx;
        }
        return JS_NewInt64(ctx, k);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || offset >= n)
        return arr;
    x = reverse ? dyn_zsl_at(&w->sl, r2 - offset) : dyn_zsl_at(&w->sl, r1 + offset);
    n -= offset;
    while (x && n-- && out < limit) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_remove_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t start, stop;
    uint32_t a, b, k = 0;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return JS_NewInt32(ctx, 0);
    x = dyn_zsl_at(&w->sl, a + 1);
    while (x && k < b - a + 1) {
        dyn_zsl_node* nx = x->lv[0].fwd;
        dyn_zsl_drop(&w->sl, x);
        x = nx;
        k++;
    }
    return JS_NewInt64(ctx, k);
}

static JSValue sdb_zset_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    dyn_zsl_node* x;
    JSValue r;
    (void)argc;
    (void)argv;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    if (!x)
        return JS_UNDEFINED;
    r = sdb_zset_entry(ctx, x);
    if (JS_IsException(r))
        return r;
    dyn_zsl_drop(&w->sl, x);
    return r;
}

static JSValue sdb_zset_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_zset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_zset, "entries")

static const JSCFunctionListEntry sdb_zset_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 2, sdb_zset_add, 0),
    JS_CFUNC_MAGIC_DEF("incrBy", 2, sdb_zset_add, 1),
    JS_CFUNC_MAGIC_DEF("score", 1, sdb_zset_member_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_zset_member_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_zset_member_op, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_zset_member_op, 3),
    JS_CFUNC_MAGIC_DEF("revRank", 1, sdb_zset_member_op, 4),
    JS_CFUNC_DEF("at", 1, sdb_zset_at),
    JS_CFUNC_MAGIC_DEF("range", 2, sdb_zset_range, 0),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_zset_range, 1),
    JS_CFUNC_MAGIC_DEF("rangeByScore", 2, sdb_zset_by_score, 0),
    JS_CFUNC_MAGIC_DEF("countByScore", 2, sdb_zset_by_score, 1),
    JS_CFUNC_MAGIC_DEF("removeRangeByScore", 2, sdb_zset_by_score, 2),
    JS_CFUNC_DEF("removeRangeByRank", 2, sdb_zset_remove_rank),
    JS_CFUNC_MAGIC_DEF("popMin", 0, sdb_zset_pop, 0),
    JS_CFUNC_MAGIC_DEF("popMax", 0, sdb_zset_pop, 1),
    JS_CFUNC_DEF("clear", 0, sdb_zset_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_zset_iterator),
    JS_CGETSET_DEF("size", sdb_zset_size, NULL),
};

typedef struct {
    dyn_zsl_t sl;
} sdb_omap_w;

static JSClassID sdb_omap_class_id;

static void sdb_omap_free(void* native)
{
    sdb_omap_w* w = (sdb_omap_w*)native;
    if (!w)
        return;
    dyn_zsl_destroy(&w->sl);
    dyn_ds_free(w);
}

static void sdb_omap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_omap_free(JS_GetOpaque(val, sdb_omap_class_id));
}

static void sdb_omap_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    sdb_omap_w* w = JS_GetOpaque(val, sdb_omap_class_id);
    if (w)
        sdb_zsl_mark(rt, &w->sl, mark_func);
}

static const JSClassDef sdb_omap_class = {
    "OrderedMap",
    .finalizer = sdb_omap_finalizer,
    .gc_mark = sdb_omap_gc_mark,
};

static JSValue sdb_omap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_omap_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    if (sdb_zsl_init(&w->sl, ctx) < 0) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_omap_class_id, w, sdb_omap_free);
}

static int sdb_omap_put(JSContext* ctx, sdb_omap_w* w, const char* k, size_t n, JSValueConst v)
{
    dyn_zsl_node* x = dyn_zsl_find(&w->sl, k, n);
    if (x) {
        JSValue old = ds_cell_val(&x->val);
        x->val = ds_val_cell(JS_DupValue(ctx, v));
        JS_FreeValue(ctx, old);
        return 0;
    }
    {
        JSValue d = JS_DupValue(ctx, v);
        if (sdb_zsl_insert(&w->sl, 0, k, n, d, NULL) < 0) {
            JS_FreeValue(ctx, d);
            return -1;
        }
    }
    return 0;
}

static JSValue sdb_omap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str k;
    int rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_omap_put(ctx, w, k.p, k.n, argv[1]);
    ds2_str_release(ctx, &k);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_omap_entry(JSContext* ctx, const dyn_zsl_node* x)
{
    return sdb_pair(ctx, dyn_zsl_keystr(ctx, x), JS_DupValue(ctx, ds_cell_val(&x->val)));
}

static JSValue sdb_omap_key_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str k;
    dyn_zsl_node* x = NULL;
    uint32_t r = 0;
    JSValue res = JS_UNDEFINED;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        x = dyn_zsl_find(&w->sl, k.p, k.n);
        res = x ? JS_DupValue(ctx, ds_cell_val(&x->val)) : JS_UNDEFINED;
        break;
    case 1:
        res = JS_NewBool(ctx, dyn_zsl_find(&w->sl, k.p, k.n) != NULL);
        break;
    case 2:
        x = dyn_zsl_find(&w->sl, k.p, k.n);
        if (x)
            dyn_zsl_drop(&w->sl, x);
        res = JS_NewBool(ctx, x != NULL);
        break;
    case 3:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 4:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 5:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 6:
        x = dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    default:
        dyn_zsl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, &r);
        res = JS_NewInt64(ctx, r);
        break;
    }
    ds2_str_release(ctx, &k);
    return res;
}

static JSValue sdb_omap_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    dyn_zsl_node* x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    return x ? dyn_zsl_keystr(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str lo = { 0 }, hi = { 0 };
    uint32_t limit = UINT32_MAX, out = 0;
    int reverse = 0, keys_only = magic == 3;
    dyn_zsl_node* x;
    JSValue arr;
    JSValueConst opts = JS_UNDEFINED;

    if (magic == 0 || magic == 1) {
        if (ds2_str_get(ctx, &lo, argv[0]))
            return JS_EXCEPTION;
    }
    if (magic == 0) {
        if (ds2_str_get(ctx, &hi, argv[1])) {
            ds2_str_release(ctx, &lo);
            return JS_EXCEPTION;
        }
        if (argc > 2)
            opts = argv[2];
    } else if (magic == 1 && argc > 1)
        opts = argv[1];
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || (magic == 0 && sdb_opt_bool(ctx, opts, "reverse", &reverse))) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    arr = w ? JS_NewArray(ctx) : JS_EXCEPTION;
    if (JS_IsException(arr)) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return arr;
    }
    if (magic >= 2) {
        x = w->sl.head->lv[0].fwd;
    } else if (reverse) {
        x = dyn_zsl_lt(&w->sl, 0, hi.p, (uint32_t)hi.n, 1, NULL);
    } else {
        x = dyn_zsl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
    }
    while (x && out < limit) {
        JSValue item;
        if (magic == 0) {
            if (reverse ? dyn_zsl_cmp(x, 0, lo.p, (uint32_t)lo.n) < 0 : dyn_zsl_cmp(x, 0, hi.p, (uint32_t)hi.n) > 0)
                break;
        } else if (magic == 1) {
            if (x->klen < lo.n || (lo.n && memcmp(x->key, lo.p, lo.n) != 0))
                break;
        }
        item = keys_only ? dyn_zsl_keystr(ctx, x) : sdb_omap_entry(ctx, x);
        if (sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return arr;
}

static JSValue sdb_omap_delete_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str lo, hi;
    dyn_zsl_node* x;
    uint32_t k = 0;
    (void)argc;
    if (ds2_str_get(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &hi, argv[1])) {
        ds2_str_release(ctx, &lo);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (w) {
        x = dyn_zsl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        while (x && dyn_zsl_cmp(x, 0, hi.p, (uint32_t)hi.n) <= 0) {
            dyn_zsl_node* nx = x->lv[0].fwd;
            dyn_zsl_drop(&w->sl, x);
            x = nx;
            k++;
        }
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return w ? JS_NewInt64(ctx, k) : JS_EXCEPTION;
}

static JSValue sdb_omap_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    int64_t i;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = dyn_zsl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_omap_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_omap_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_omap, "entries")

static const JSCFunctionListEntry sdb_omap_proto[] = {
    JS_CFUNC_DEF("set", 2, sdb_omap_set),
    JS_CFUNC_MAGIC_DEF("get", 1, sdb_omap_key_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_omap_key_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_omap_key_op, 2),
    JS_CFUNC_MAGIC_DEF("floorKey", 1, sdb_omap_key_op, 3),
    JS_CFUNC_MAGIC_DEF("ceilKey", 1, sdb_omap_key_op, 4),
    JS_CFUNC_MAGIC_DEF("lowerKey", 1, sdb_omap_key_op, 5),
    JS_CFUNC_MAGIC_DEF("higherKey", 1, sdb_omap_key_op, 6),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_omap_key_op, 7),
    JS_CFUNC_MAGIC_DEF("firstKey", 0, sdb_omap_end, 0),
    JS_CFUNC_MAGIC_DEF("lastKey", 0, sdb_omap_end, 1),
    JS_CFUNC_MAGIC_DEF("rangeQuery", 2, sdb_omap_range, 0),
    JS_CFUNC_MAGIC_DEF("prefix", 1, sdb_omap_range, 1),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_omap_range, 2),
    JS_CFUNC_MAGIC_DEF("keys", 0, sdb_omap_range, 3),
    JS_CFUNC_DEF("deleteRange", 2, sdb_omap_delete_range),
    JS_CFUNC_DEF("at", 1, sdb_omap_at),
    JS_CFUNC_DEF("clear", 0, sdb_omap_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_omap_iterator),
    JS_CGETSET_DEF("size", sdb_omap_size, NULL),
};
typedef struct {
    dyn_roar_t r;
} sdb_rb_w;

static JSClassID sdb_rb_class_id;

static void sdb_rb_free(void* native)
{
    sdb_rb_w* w = (sdb_rb_w*)native;
    if (!w)
        return;
    dyn_roar_clear(&w->r);
    dyn_ds_free(w);
}

static void sdb_rb_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_rb_free(JS_GetOpaque(val, sdb_rb_class_id));
}

static const JSClassDef sdb_rb_class = {
    "RoaringBitmap",
    .finalizer = sdb_rb_finalizer,
};

static int sdb_rb_add_list(JSContext* ctx, sdb_rb_w* w, uint32_t* v, uint32_t n)
{
    uint32_t i;
    qsort(v, n, sizeof(uint32_t), dyn_roar_u32_cmp);
    for (i = 0; i < n; i++)
        if ((i == 0 || v[i] != v[i - 1]) && dyn_roar_add(&w->r, v[i]) < 0) {
            ds2_oom(ctx);
            return -1;
        }
    return 0;
}

static JSValue sdb_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t* v = NULL;
    uint32_t n = 0;
    JSValue obj;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && sdb_u32_list(ctx, argv[0], "RoaringBitmap", &v, &n))
        return JS_EXCEPTION;
    w = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w) {
        dyn_ds_free(v);
        return ds2_oom(ctx);
    }
    if (n && sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_ds_free(v);
        sdb_rb_free(w);
        return JS_EXCEPTION;
    }
    dyn_ds_free(v);
    obj = dyn_plain_wrap(ctx, new_target, sdb_rb_class_id, w, sdb_rb_free);
    return obj;
}

static sdb_rb_w* sdb_rb_get(JSContext* ctx, JSValueConst v)
{
    return (sdb_rb_w*)dyn_plain_get(ctx, v, sdb_rb_class_id);
}

static JSValue sdb_rb_elem(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint32_t x;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "value", &x))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (dyn_roar_add(&w->r, x) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    case 1:
        rc = dyn_roar_remove(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    case 2:
        return JS_NewBool(ctx, dyn_roar_has(&w->r, x));
    case 3:
        return JS_NewInt64(ctx, (int64_t)dyn_roar_rank(&w->r, x));
    default:
        rc = dyn_roar_add(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    }
}

static JSValue sdb_rb_add_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t *v, n;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "addMany", &v, &n))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w || sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_ds_free(v);
        return JS_EXCEPTION;
    }
    dyn_ds_free(v);
    return JS_DupValue(ctx, this_val);
}

static int sdb_rb_bounds(JSContext* ctx, JSValueConst a, JSValueConst b, uint64_t* lo, uint64_t* hi)
{
    double x, y;
    if (JS_ToFloat64(ctx, &x, a) || JS_ToFloat64(ctx, &y, b))
        return -1;
    if (!(x >= 0 && x <= 4294967296.0) || x != trunc(x) || !(y >= 0 && y <= 4294967296.0) || y != trunc(y)) {
        JS_ThrowRangeError(ctx, "range bounds must be integers in 0..2^32");
        return -1;
    }
    *lo = (uint64_t)x;
    *hi = (uint64_t)y;
    return 0;
}

static JSValue sdb_rb_range_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint64_t lo, hi;
    (void)argc;
    if (sdb_rb_bounds(ctx, argv[0], argv[1], &lo, &hi))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 2) {
        uint64_t a = lo ? dyn_roar_rank(&w->r, (uint32_t)(lo - 1)) : 0;
        uint64_t b = hi ? dyn_roar_rank(&w->r, (uint32_t)(hi - 1)) : 0;
        return JS_NewInt64(ctx, hi > lo ? (int64_t)(b - a) : 0);
    }
    if (dyn_roar_range(&w->r, lo, hi, magic == 0) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_rb_setop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w *w, *o;
    (void)argc;
    o = sdb_rb_get(ctx, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic < 4) {
        if (dyn_roar_op(&w->r, &o->r, magic) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    }
    if (magic < 8)
        return JS_NewInt64(ctx, (int64_t)dyn_roar_op_card(&w->r, &o->r, magic - 4));
    {
        uint64_t inter = dyn_roar_op_card(&w->r, &o->r, DYN_ROAR_AND);
        uint64_t ca = dyn_roar_card(&w->r), cb = dyn_roar_card(&o->r);
        switch (magic) {
        case 8:
            return JS_NewBool(ctx, inter != 0);
        case 9:
            return JS_NewBool(ctx, inter == ca);
        case 10:
            return JS_NewBool(ctx, inter == ca && ca == cb);
        default:
            return JS_NewFloat64(ctx, ca + cb - inter ? (double)inter / (double)(ca + cb - inter) : 1.0);
        }
    }
}

static JSValue sdb_rb_misc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint32_t x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!w->r.n)
            return JS_UNDEFINED;
        return dyn_roar_select(&w->r, 0, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 1:
        if (!w->r.n)
            return JS_UNDEFINED;
        return dyn_roar_select(&w->r, dyn_roar_card(&w->r) - 1, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 2: {
        uint64_t n = dyn_roar_card(&w->r);
        uint32_t* buf;
        JSValue out;
        if (n > DYN_DS_MAX_ITEMS)
            return JS_ThrowRangeError(ctx, "toArray: %llu values exceed the %u materialisation limit", (unsigned long long)n, DYN_DS_MAX_ITEMS);
        buf = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (size_t)(n + 1));
        if (!buf)
            return ds2_oom(ctx);
        dyn_roar_fill(&w->r, buf);
        out = sdb_typed_out(ctx, buf, (size_t)n, JS_TYPED_ARRAY_UINT32);
        dyn_ds_free(buf);
        return out;
    }
    case 3: {
        sdb_rb_w* c = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*c));
        if (!c || dyn_roar_clone(&c->r, &w->r) < 0) {
            dyn_ds_free(c);
            return ds2_oom(ctx);
        }
        return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, c, sdb_rb_free);
    }
    case 4:
        dyn_roar_clear(&w->r);
        return JS_UNDEFINED;
    default:
        return JS_NewBool(ctx, w->r.n == 0);
    }
}

static JSValue sdb_rb_select_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    double d;
    uint32_t x;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= 4294967296.0)
        return JS_UNDEFINED;
    return dyn_roar_select(&w->r, (uint64_t)d, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_rb_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint64_t b = 0;
    uint32_t i;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)dyn_roar_card(&w->r));
    b = sizeof(dyn_roar_ct) * (uint64_t)w->r.cap;
    for (i = 0; i < w->r.n; i++) {
        const dyn_roar_ct* c = &w->r.c[i];
        b += c->type == DYN_ROAR_ARR ? 2u * c->cap : c->type == DYN_ROAR_BMP ? 8u * DYN_ROAR_WORDS
                                                                             : 0;
    }
    return JS_NewInt64(ctx, (int64_t)b);
}

DS2_ITERATOR(sdb_rb, "toArray")

static const JSCFunctionListEntry sdb_rb_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_rb_elem, 0),
    JS_CFUNC_MAGIC_DEF("tryAdd", 1, sdb_rb_elem, 4),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_rb_elem, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_rb_elem, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_rb_elem, 3),
    JS_CFUNC_DEF("addMany", 1, sdb_rb_add_many),
    JS_CFUNC_MAGIC_DEF("addRange", 2, sdb_rb_range_op, 0),
    JS_CFUNC_MAGIC_DEF("removeRange", 2, sdb_rb_range_op, 1),
    JS_CFUNC_MAGIC_DEF("rangeCardinality", 2, sdb_rb_range_op, 2),
    JS_CFUNC_MAGIC_DEF("and", 1, sdb_rb_setop, DYN_ROAR_AND),
    JS_CFUNC_MAGIC_DEF("or", 1, sdb_rb_setop, DYN_ROAR_OR),
    JS_CFUNC_MAGIC_DEF("xor", 1, sdb_rb_setop, DYN_ROAR_XOR),
    JS_CFUNC_MAGIC_DEF("andNot", 1, sdb_rb_setop, DYN_ROAR_ANDNOT),
    JS_CFUNC_MAGIC_DEF("andCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_AND),
    JS_CFUNC_MAGIC_DEF("orCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_OR),
    JS_CFUNC_MAGIC_DEF("xorCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_XOR),
    JS_CFUNC_MAGIC_DEF("andNotCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_ANDNOT),
    JS_CFUNC_MAGIC_DEF("intersects", 1, sdb_rb_setop, 8),
    JS_CFUNC_MAGIC_DEF("isSubsetOf", 1, sdb_rb_setop, 9),
    JS_CFUNC_MAGIC_DEF("equals", 1, sdb_rb_setop, 10),
    JS_CFUNC_MAGIC_DEF("jaccard", 1, sdb_rb_setop, 11),
    JS_CFUNC_DEF("select", 1, sdb_rb_select_m),
    JS_CFUNC_MAGIC_DEF("min", 0, sdb_rb_misc, 0),
    JS_CFUNC_MAGIC_DEF("max", 0, sdb_rb_misc, 1),
    JS_CFUNC_MAGIC_DEF("toArray", 0, sdb_rb_misc, 2),
    JS_CFUNC_MAGIC_DEF("clone", 0, sdb_rb_misc, 3),
    JS_CFUNC_MAGIC_DEF("clear", 0, sdb_rb_misc, 4),
    JS_CFUNC_MAGIC_DEF("isEmpty", 0, sdb_rb_misc, 5),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_rb_iterator),
    JS_CGETSET_MAGIC_DEF("size", sdb_rb_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_rb_getter, NULL, 2),
};
typedef struct {
    dyn_tdigest_t t;
} sdb_td_w;

static JSClassID sdb_td_class_id;

static void sdb_td_free(void* native)
{
    sdb_td_w* w = (sdb_td_w*)native;
    if (!w)
        return;
    dyn_tdigest_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_td_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_td_free(JS_GetOpaque(val, sdb_td_class_id));
}

static const JSClassDef sdb_td_class = {
    "TDigest",
    .finalizer = sdb_td_finalizer,
};

static JSValue sdb_td_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double d = 100;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (!(d >= DYN_TDIGEST_MIN_DELTA && d <= DYN_TDIGEST_MAX_DELTA))
            return JS_ThrowRangeError(ctx, "compression must be in %g..%g", DYN_TDIGEST_MIN_DELTA, DYN_TDIGEST_MAX_DELTA);
    }
    w = (sdb_td_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    if (dyn_tdigest_init(&w->t, d) < 0) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_td_class_id, w, sdb_td_free);
}

static JSValue sdb_td_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double x, wt = 1;
    if (sdb_finite_arg(ctx, argv[0], "value", &x))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &wt, argv[1]))
            return JS_EXCEPTION;
        if (!(wt > 0) || !isfinite(wt))
            return JS_ThrowRangeError(ctx, "weight must be a positive finite number");
    }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_tdigest_add(&w->t, x, wt);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_add_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double* v;
    uint32_t n, i;
    (void)argc;
    if (sdb_f64_list(ctx, argv[0], "addMany", &v, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        if (!isfinite(v[i])) {
            dyn_ds_free(v);
            return JS_ThrowRangeError(ctx, "addMany: element %u is not finite", (unsigned)i);
        }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w) {
        dyn_ds_free(v);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        dyn_tdigest_add(&w->t, v[i], 1);
    dyn_ds_free(v);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_td_w* w;
    double x;
    (void)argc;
    if (sdb_num_arg(ctx, argv[0], magic ? "value" : "q", &x))
        return JS_EXCEPTION;
    if (!magic && !(x >= 0 && x <= 1))
        return JS_ThrowRangeError(ctx, "q must be in 0..1");
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, magic ? dyn_tdigest_cdf(&w->t, x) : dyn_tdigest_quantile(&w->t, x));
}

static JSValue sdb_td_quantiles(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double* q;
    uint32_t n, i;
    JSValue out;
    (void)argc;
    if (sdb_f64_list(ctx, argv[0], "quantiles", &q, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        if (!(q[i] >= 0 && q[i] <= 1)) {
            dyn_ds_free(q);
            return JS_ThrowRangeError(ctx, "quantiles: element %u is not in 0..1", (unsigned)i);
        }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w) {
        dyn_ds_free(q);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        q[i] = dyn_tdigest_quantile(&w->t, q[i]);
    out = sdb_typed_out(ctx, q, n, JS_TYPED_ARRAY_FLOAT64);
    dyn_ds_free(q);
    return out;
}

static JSValue sdb_td_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w *w, *o;
    uint32_t i, n;
    dyn_tdigest_cent* tmp;
    (void)argc;
    o = (sdb_td_w*)dyn_plain_get(ctx, argv[0], sdb_td_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_tdigest_compress(&o->t);
    n = o->t.n;
    tmp = (dyn_tdigest_cent*)dyn_ds_malloc(sizeof(dyn_tdigest_cent) * ((size_t)n + 1));
    if (!tmp)
        return ds2_oom(ctx);
    memcpy(tmp, o->t.c, sizeof(dyn_tdigest_cent) * n);
    {
        double omin = o->t.min, omax = o->t.max, osum = o->t.sum;
        for (i = 0; i < n; i++)
            dyn_tdigest_add(&w->t, tmp[i].m, tmp[i].w);
        if (n) {
            w->t.sum += osum;
            for (i = 0; i < n; i++)
                w->t.sum -= tmp[i].m * tmp[i].w;
            if (omin < w->t.min)
                w->t.min = omin;
            if (omax > w->t.max)
                w->t.max = omax;
        }
    }
    dyn_ds_free(tmp);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_td_w* w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, w->t.total);
    case 1:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.min) : JS_UNDEFINED;
    case 2:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.max) : JS_UNDEFINED;
    case 3:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.sum / w->t.total) : JS_UNDEFINED;
    case 4:
        dyn_tdigest_compress(&w->t);
        return JS_NewInt64(ctx, w->t.n);
    default:
        return JS_NewFloat64(ctx, w->t.delta);
    }
}

static const JSCFunctionListEntry sdb_td_proto[] = {
    JS_CFUNC_DEF("add", 1, sdb_td_add),
    JS_CFUNC_DEF("addMany", 1, sdb_td_add_many),
    JS_CFUNC_MAGIC_DEF("quantile", 1, sdb_td_query, 0),
    JS_CFUNC_MAGIC_DEF("cdf", 1, sdb_td_query, 1),
    JS_CFUNC_DEF("quantiles", 1, sdb_td_quantiles),
    JS_CFUNC_DEF("merge", 1, sdb_td_merge),
    JS_CGETSET_MAGIC_DEF("count", sdb_td_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("min", sdb_td_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("max", sdb_td_getter, NULL, 2),
    JS_CGETSET_MAGIC_DEF("mean", sdb_td_getter, NULL, 3),
    JS_CGETSET_MAGIC_DEF("centroids", sdb_td_getter, NULL, 4),
    JS_CGETSET_MAGIC_DEF("compression", sdb_td_getter, NULL, 5),
};
typedef struct {
    dyn_topk_t t;
} sdb_tk_w;

static JSClassID sdb_tk_class_id;

static void sdb_tk_free(void* native)
{
    sdb_tk_w* w = (sdb_tk_w*)native;
    if (!w)
        return;
    dyn_topk_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_tk_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_tk_free(JS_GetOpaque(val, sdb_tk_class_id));
}

static const JSClassDef sdb_tk_class = {
    "TopK",
    .finalizer = sdb_tk_finalizer,
};

static sdb_tk_w* sdb_tk_new(uint32_t k)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    if (dyn_topk_init(&w->t, k) < 0) {
        sdb_tk_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_tk_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    uint32_t k;
    (void)argc;
    if (dyn_size_arg(ctx, argv[0], DYN_TOPK_MAX_K, &k))
        return JS_EXCEPTION;
    if (k == 0)
        return JS_ThrowRangeError(ctx, "k must be at least 1");
    w = sdb_tk_new(k);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_tk_class_id, w, sdb_tk_free);
}

static JSValue sdb_tk_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    ds2_str k;
    double c = 1;
    char* ev;
    size_t evn = 0;
    JSValue r;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &c, argv[1]))
            return JS_EXCEPTION;
        if (!(c > 0) || !isfinite(c))
            return JS_ThrowRangeError(ctx, "count must be a positive finite number");
    }
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (dyn_topk_add(&w->t, k.p, k.n, c, &ev, &evn) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    if (!ev)
        return JS_UNDEFINED;
    r = JS_NewStringLen(ctx, ev, evn);
    dyn_ds_free(ev);
    return r;
}

static JSValue sdb_tk_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_tk_w* w;
    ds2_str k;
    dyn_ds_sent* e;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    e = dyn_ds_smap_find(&w->t.idx, k.p, k.n, dyn_ds_strhash(k.p, k.n));
    ds2_str_release(ctx, &k);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    if (magic == 2)
        return e ? JS_NewFloat64(ctx, w->t.s[e->v].err) : JS_UNDEFINED;
    return JS_NewFloat64(ctx, e ? w->t.s[e->v].count : 0);
}

static JSValue sdb_tk_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    uint32_t *ord, i, out = 0;
    JSValue arr;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    ord = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)w->t.n + 1));
    if (!ord)
        return ds2_oom(ctx);
    dyn_topk_sorted(&w->t, ord);
    arr = JS_NewArray(ctx);
    for (i = 0; i < w->t.n && !JS_IsException(arr); i++) {
        const dyn_topk_slot* s = &w->t.s[ord[i]];
        JSValue item = JS_NewArray(ctx);
        uint32_t j = 0;
        if (sdb_push(ctx, item, &j, JS_NewStringLen(ctx, s->key, s->klen)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->count)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->err)) < 0
            || sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ord);
    return arr;
}

static JSValue sdb_tk_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->t.k);
    if (magic == 1)
        return JS_NewInt64(ctx, w->t.n);
    return JS_NewFloat64(ctx, w->t.total);
}

static const JSCFunctionListEntry sdb_tk_proto[] = {
    JS_CFUNC_DEF("add", 1, sdb_tk_add),
    JS_CFUNC_MAGIC_DEF("count", 1, sdb_tk_query, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_tk_query, 1),
    JS_CFUNC_MAGIC_DEF("error", 1, sdb_tk_query, 2),
    JS_CFUNC_DEF("list", 0, sdb_tk_list),
    JS_CGETSET_MAGIC_DEF("k", sdb_tk_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("size", sdb_tk_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("total", sdb_tk_getter, NULL, 2),
};
typedef struct {
    dyn_cuckoo_t c;
} sdb_cf_w;

static JSClassID sdb_cf_class_id;

static void sdb_cf_free(void* native)
{
    sdb_cf_w* w = (sdb_cf_w*)native;
    if (!w)
        return;
    dyn_cuckoo_free(&w->c);
    dyn_ds_free(w);
}

static void sdb_cf_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_cf_free(JS_GetOpaque(val, sdb_cf_class_id));
}

static const JSClassDef sdb_cf_class = {
    "CuckooFilter",
    .finalizer = sdb_cf_finalizer,
};

static sdb_cf_w* sdb_cf_new(uint32_t nb)
{
    sdb_cf_w* w = (sdb_cf_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    if (dyn_cuckoo_init(&w->c, nb) < 0) {
        dyn_ds_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_cf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_cf_w* w;
    uint32_t cap, nb = 1;
    (void)argc;
    if (dyn_size_arg(ctx, argv[0], DYN_CUCKOO_MAX_CAPACITY, &cap))
        return JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be at least 1");
    while ((uint64_t)nb * DYN_CUCKOO_SLOTS * 95 < (uint64_t)cap * 100)
        nb <<= 1;
    w = sdb_cf_new(nb);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_cf_class_id, w, sdb_cf_free);
}

static JSValue sdb_cf_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_cf_w* w;
    ds2_str k;
    uint32_t i;
    uint16_t fp;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_cf_w*)dyn_plain_get(ctx, this_val, sdb_cf_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    dyn_cuckoo_hash(&w->c, k.p, k.n, &i, &fp);
    ds2_str_release(ctx, &k);
    r = magic == 0 ? dyn_cuckoo_insert(&w->c, i, fp) : magic == 1 ? dyn_cuckoo_contains(&w->c, i, fp)
                                                                  : dyn_cuckoo_delete(&w->c, i, fp);
    return JS_NewBool(ctx, r);
}

static JSValue sdb_cf_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_cf_w* w = (sdb_cf_w*)dyn_plain_get(ctx, this_val, sdb_cf_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->c.count);
    if (magic == 1)
        return JS_NewInt64(ctx, (int64_t)w->c.nb * DYN_CUCKOO_SLOTS);
    return JS_NewFloat64(ctx, (double)w->c.count / ((double)w->c.nb * DYN_CUCKOO_SLOTS));
}

static const JSCFunctionListEntry sdb_cf_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_cf_op, 0),
    JS_CFUNC_MAGIC_DEF("mayContain", 1, sdb_cf_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_cf_op, 2),
    JS_CGETSET_MAGIC_DEF("size", sdb_cf_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("capacity", sdb_cf_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("loadFactor", sdb_cf_getter, NULL, 2),
};
typedef struct {
    dyn_iheap_t q;
} sdb_ih_w;

static JSClassID sdb_ih_class_id;

static void sdb_ih_free(void* native)
{
    sdb_ih_w* w = (sdb_ih_w*)native;
    if (!w)
        return;
    dyn_iheap_free(&w->q);
    dyn_ds_free(w);
}

static void sdb_ih_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ih_free(JS_GetOpaque(val, sdb_ih_class_id));
}

static const JSClassDef sdb_ih_class = {
    "IndexedHeap",
    .finalizer = sdb_ih_finalizer,
};

static sdb_ih_w* sdb_ih_new(int max)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    dyn_iheap_init(&w->q, max);
    return w;
}

static JSValue sdb_ih_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    int max = 0;
    if (argc >= 1 && sdb_opt_bool(ctx, argv[0], "max", &max))
        return JS_EXCEPTION;
    w = sdb_ih_new(max);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_ih_class_id, w, sdb_ih_free);
}

static JSValue sdb_ih_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    uint32_t id;
    double p;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id) || sdb_num_arg(ctx, argv[1], "priority", &p))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (dyn_iheap_push(&w->q, id, p) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ih_push_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    uint32_t *ids, n, np, i;
    double* ps;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "pushMany ids", &ids, &n))
        return JS_EXCEPTION;
    if (sdb_f64_list(ctx, argv[1], "pushMany priorities", &ps, &np)) {
        dyn_ds_free(ids);
        return JS_EXCEPTION;
    }
    if (n != np) {
        dyn_ds_free(ids);
        dyn_ds_free(ps);
        return JS_ThrowRangeError(ctx, "pushMany: %u ids but %u priorities", (unsigned)n, (unsigned)np);
    }
    for (i = 0; i < n; i++)
        if (ps[i] != ps[i]) {
            dyn_ds_free(ids);
            dyn_ds_free(ps);
            return JS_ThrowTypeError(ctx, "pushMany: priority %u is NaN", (unsigned)i);
        }
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    for (i = 0; w && i < n; i++)
        if (dyn_iheap_push(&w->q, ids[i], ps[i]) < 0) {
            dyn_ds_free(ids);
            dyn_ds_free(ps);
            return ds2_oom(ctx);
        }
    dyn_ds_free(ids);
    dyn_ds_free(ps);
    return w ? JS_DupValue(ctx, this_val) : JS_EXCEPTION;
}

static JSValue sdb_ih_id_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ih_w* w;
    uint32_t id;
    dyn_ds_ient* s;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    s = dyn_ds_imap_find(&w->q.pos, id);
    if (magic == 0)
        return s ? JS_NewFloat64(ctx, w->q.h[s->v].p) : JS_UNDEFINED;
    if (magic == 1)
        return JS_NewBool(ctx, s != NULL);
    if (!s)
        return JS_FALSE;
    dyn_iheap_remove_at(&w->q, s->v);
    return JS_TRUE;
}

static JSValue sdb_ih_top(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    dyn_iheap_ent e;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (!w->q.n)
        return JS_UNDEFINED;
    e = w->q.h[0];
    if (magic == 1)
        return JS_NewInt64(ctx, e.id);
    if (magic == 2)
        return JS_NewFloat64(ctx, e.p);
    dyn_iheap_remove_at(&w->q, 0);
    if (magic == 3)
        return sdb_pair(ctx, JS_NewInt64(ctx, e.id), JS_NewFloat64(ctx, e.p));
    return JS_NewInt64(ctx, e.id);
}

static JSValue sdb_ih_drain(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    double lim;
    uint32_t cap = UINT32_MAX, n = 0, *buf = NULL, bcap = 0;
    JSValue out;
    if (sdb_num_arg(ctx, argv[0], "limit", &lim))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && sdb_u32_arg(ctx, argv[1], "max", &cap))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    while (w->q.n && n < cap && (w->q.max ? w->q.h[0].p >= lim : w->q.h[0].p <= lim)) {
        if (n == bcap) {
            uint32_t nc = bcap ? bcap * 2 : 16;
            uint32_t* nb = (uint32_t*)dyn_ds_realloc(buf, sizeof(uint32_t) * nc);
            if (!nb) {
                dyn_ds_free(buf);
                return ds2_oom(ctx);
            }
            buf = nb;
            bcap = nc;
        }
        buf[n++] = w->q.h[0].id;
        dyn_iheap_remove_at(&w->q, 0);
    }
    out = sdb_typed_out(ctx, buf, n, JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(buf);
    return out;
}

static JSValue sdb_ih_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    w->q.n = 0;
    dyn_ds_imap_free(&w->q.pos);
    return JS_UNDEFINED;
}

static JSValue sdb_ih_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    return magic ? JS_NewBool(ctx, w->q.max) : JS_NewInt64(ctx, w->q.n);
}

static const JSCFunctionListEntry sdb_ih_proto[] = {
    JS_CFUNC_DEF("push", 2, sdb_ih_push),
    JS_CFUNC_DEF("pushMany", 2, sdb_ih_push_many),
    JS_CFUNC_MAGIC_DEF("priority", 1, sdb_ih_id_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_ih_id_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_ih_id_op, 2),
    JS_CFUNC_MAGIC_DEF("pop", 0, sdb_ih_top, 0),
    JS_CFUNC_MAGIC_DEF("peek", 0, sdb_ih_top, 1),
    JS_CFUNC_MAGIC_DEF("peekPriority", 0, sdb_ih_top, 2),
    JS_CFUNC_MAGIC_DEF("popEntry", 0, sdb_ih_top, 3),
    JS_CFUNC_DEF("popUntil", 1, sdb_ih_drain),
    JS_CFUNC_DEF("clear", 0, sdb_ih_clear),
    JS_CGETSET_MAGIC_DEF("size", sdb_ih_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("isMax", sdb_ih_getter, NULL, 1),
};
typedef struct {
    dyn_kdtree_t t;
} sdb_kd_w;

static JSClassID sdb_kd_class_id;

static void sdb_kd_free(void* native)
{
    sdb_kd_w* w = (sdb_kd_w*)native;
    if (!w)
        return;
    dyn_kdtree_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_kd_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_kd_free(JS_GetOpaque(val, sdb_kd_class_id));
}

static const JSClassDef sdb_kd_class = {
    "KDTree",
    .finalizer = sdb_kd_finalizer,
};

static int sdb_kd_rows(JSContext* ctx, JSValueConst v, double** out, uint32_t* pn, uint32_t* pd)
{
    JSValue lv, row;
    uint32_t n, i, d = 0, rd;
    double *pts = NULL, *r;
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    for (i = 0; i < n; i++) {
        row = JS_GetPropertyUint32(ctx, v, i);
        if (JS_IsException(row)) {
            dyn_ds_free(pts);
            return -1;
        }
        if (sdb_f64_list(ctx, row, "KDTree point", &r, &rd)) {
            JS_FreeValue(ctx, row);
            dyn_ds_free(pts);
            return -1;
        }
        JS_FreeValue(ctx, row);
        if (i == 0) {
            d = rd;
            if (d == 0 || d > DYN_KDTREE_MAX_DIMS || (uint64_t)n * d > DYN_DS_MAX_ITEMS) {
                dyn_ds_free(r);
                JS_ThrowRangeError(ctx, "KDTree: points need 1..%d dimensions and at most %u coordinates", DYN_KDTREE_MAX_DIMS, DYN_DS_MAX_ITEMS);
                return -1;
            }
            pts = (double*)dyn_ds_malloc(sizeof(double) * (size_t)n * d);
            if (!pts) {
                dyn_ds_free(r);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        } else if (rd != d) {
            dyn_ds_free(r);
            dyn_ds_free(pts);
            JS_ThrowRangeError(ctx, "KDTree: point %u has %u coordinates, expected %u", (unsigned)i, (unsigned)rd, (unsigned)d);
            return -1;
        }
        memcpy(&pts[(size_t)i * d], r, sizeof(double) * d);
        dyn_ds_free(r);
    }
    *out = pts;
    *pn = n;
    *pd = d;
    return 0;
}

static JSValue sdb_kd_wrap(JSContext* ctx, JSValueConst new_target, double* pts, uint32_t n, uint32_t d)
{
    sdb_kd_w* w;
    uint32_t i;
    for (i = 0; i < n * d; i++)
        if (!isfinite(pts[i])) {
            dyn_ds_free(pts);
            return JS_ThrowRangeError(ctx, "KDTree: coordinate %u of point %u is not finite", (unsigned)(i % d), (unsigned)(i / d));
        }
    w = (sdb_kd_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w) {
        dyn_ds_free(pts);
        return ds2_oom(ctx);
    }
    if (dyn_kdtree_init(&w->t, pts, n, d) < 0) {
        sdb_kd_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_kd_class_id, w, sdb_kd_free);
}

static JSValue sdb_kd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    double* pts;
    uint32_t n, d;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        uint32_t total;
        if (dyn_size_arg(ctx, argv[1], DYN_KDTREE_MAX_DIMS, &d))
            return JS_EXCEPTION;
        if (d == 0)
            return JS_ThrowRangeError(ctx, "KDTree: dims must be 1..%d", DYN_KDTREE_MAX_DIMS);
        if (sdb_f64_list(ctx, argv[0], "KDTree points", &pts, &total))
            return JS_EXCEPTION;
        if (total % d) {
            dyn_ds_free(pts);
            return JS_ThrowRangeError(ctx, "KDTree: %u coordinates is not a multiple of %u dims", (unsigned)total, (unsigned)d);
        }
        n = total / d;
    } else {
        if (JS_IsArray(ctx, argv[0]) <= 0)
            return JS_ThrowTypeError(ctx, "KDTree: expected number[][] or a flat array with dims");
        if (sdb_kd_rows(ctx, argv[0], &pts, &n, &d))
            return JS_EXCEPTION;
        if (n == 0)
            return JS_ThrowRangeError(ctx, "KDTree: an empty point list needs an explicit dims argument");
    }
    return sdb_kd_wrap(ctx, new_target, pts, n, d);
}

static int sdb_kd_point(JSContext* ctx, JSValueConst v, uint32_t d, double** out)
{
    uint32_t n, i;
    if (sdb_f64_list(ctx, v, "KDTree query", out, &n))
        return -1;
    if (n != d) {
        dyn_ds_free(*out);
        JS_ThrowRangeError(ctx, "KDTree: query has %u coordinates, expected %u", (unsigned)n, (unsigned)d);
        return -1;
    }
    for (i = 0; i < n; i++)
        if ((*out)[i] != (*out)[i]) {
            dyn_ds_free(*out);
            JS_ThrowRangeError(ctx, "KDTree: query coordinate %u is NaN", (unsigned)i);
            return -1;
        }
    return 0;
}

static JSValue sdb_kd_result(JSContext* ctx, const dyn_kdtree_hit* h, uint32_t n, int sq)
{
    int32_t* idx = (int32_t*)dyn_ds_malloc(sizeof(int32_t) * ((size_t)n + 1));
    double* dist = (double*)dyn_ds_malloc(sizeof(double) * ((size_t)n + 1));
    JSValue o, a, b;
    uint32_t i;
    if (!idx || !dist) {
        dyn_ds_free(idx);
        dyn_ds_free(dist);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        idx[i] = (int32_t)h[i].i;
        dist[i] = sq ? sqrt(h[i].d2) : h[i].d2;
    }
    a = sdb_typed_out(ctx, idx, n, JS_TYPED_ARRAY_INT32);
    b = sdb_typed_out(ctx, dist, n, JS_TYPED_ARRAY_FLOAT64);
    dyn_ds_free(idx);
    dyn_ds_free(dist);
    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return o;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "indices", a, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, o, "distances", b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdb_kd_knn(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_kd_w* w;
    double* q = NULL;
    uint32_t k = 1, m = 1, i, total;
    dyn_kdtree_knn_ctx c;
    dyn_kdtree_hit* all;
    JSValue r;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic != 1 && dyn_size_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, DYN_DS_MAX_ITEMS, &k))
        return JS_EXCEPTION;
    if (magic == 2) {
        uint32_t nq;
        if (sdb_f64_list(ctx, argv[0], "knnMany queries", &q, &nq))
            return JS_EXCEPTION;
        if (nq % w->t.d) {
            dyn_ds_free(q);
            return JS_ThrowRangeError(ctx, "knnMany: %u coordinates is not a multiple of %u dims", (unsigned)nq, (unsigned)w->t.d);
        }
        m = nq / w->t.d;
        for (i = 0; i < nq; i++)
            if (q[i] != q[i]) {
                dyn_ds_free(q);
                return JS_ThrowRangeError(ctx, "knnMany: query coordinate %u is NaN", (unsigned)i);
            }
    } else if (sdb_kd_point(ctx, argv[0], w->t.d, &q))
        return JS_EXCEPTION;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w) {
        dyn_ds_free(q);
        return JS_EXCEPTION;
    }
    if (k > w->t.n)
        k = w->t.n;
    if (magic == 1) {
        if (!w->t.n) {
            dyn_ds_free(q);
            return JS_NewInt32(ctx, -1);
        }
        k = 1;
    }
    total = k * m;
    if ((uint64_t)k * m > DYN_DS_MAX_ITEMS) {
        dyn_ds_free(q);
        return JS_ThrowRangeError(ctx, "knn: result too large");
    }
    all = (dyn_kdtree_hit*)dyn_ds_malloc(sizeof(dyn_kdtree_hit) * ((size_t)total + 1));
    if (!all) {
        dyn_ds_free(q);
        return ds2_oom(ctx);
    }
    for (i = 0; i < m && k; i++) {
        c.t = &w->t;
        c.q = q + (size_t)i * w->t.d;
        c.h = all + (size_t)i * k;
        c.k = k;
        c.n = 0;
        dyn_kdtree_knn(&c, 0);
        qsort(c.h, c.n, sizeof(dyn_kdtree_hit), dyn_kdtree_hit_cmp);
    }
    dyn_ds_free(q);
    if (magic == 1) {
        r = JS_NewInt64(ctx, all[0].i);
        dyn_ds_free(all);
        return r;
    }
    r = sdb_kd_result(ctx, all, total, 1);
    dyn_ds_free(all);
    return r;
}

static JSValue sdb_kd_region(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_kd_w* w;
    double *a = NULL, *b = NULL, r = 0;
    dyn_kdtree_list l = { NULL, 0, 0, 0 };
    JSValue out;
    uint32_t i;
    (void)argc;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (sdb_kd_point(ctx, argv[0], w->t.d, &a))
        return JS_EXCEPTION;
    if (magic == 0) {
        if (sdb_num_arg(ctx, argv[1], "radius", &r)) {
            dyn_ds_free(a);
            return JS_EXCEPTION;
        }
    } else if (sdb_kd_point(ctx, argv[1], w->t.d, &b)) {
        dyn_ds_free(a);
        return JS_EXCEPTION;
    }
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (w && w->t.n) {
        if (magic == 0) {
            if (r >= 0)
                dyn_kdtree_radius(&w->t, 0, a, r * r, &l);
            qsort(l.h, l.n, sizeof(dyn_kdtree_hit), dyn_kdtree_hit_cmp);
        } else
            dyn_kdtree_box(&w->t, 0, a, b, &l);
    }
    dyn_ds_free(a);
    dyn_ds_free(b);
    if (!w) {
        dyn_ds_free(l.h);
        return JS_EXCEPTION;
    }
    if (l.oom) {
        dyn_ds_free(l.h);
        return ds2_oom(ctx);
    }
    if (magic == 1) {
        uint32_t* ids = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)l.n + 1));
        if (!ids) {
            dyn_ds_free(l.h);
            return ds2_oom(ctx);
        }
        for (i = 0; i < l.n; i++)
            ids[i] = l.h[i].i;
        qsort(ids, l.n, sizeof(uint32_t), dyn_roar_u32_cmp);
        out = sdb_typed_out(ctx, ids, l.n, JS_TYPED_ARRAY_INT32);
        dyn_ds_free(ids);
    } else
        out = sdb_kd_result(ctx, l.h, l.n, 1);
    dyn_ds_free(l.h);
    return out;
}

static JSValue sdb_kd_point_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_kd_w* w;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i >= w->t.n)
        return JS_UNDEFINED;
    return sdb_typed_out(ctx, &w->t.pts[(size_t)i * w->t.d], w->t.d, JS_TYPED_ARRAY_FLOAT64);
}

static JSValue sdb_kd_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_kd_w* w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic ? w->t.d : w->t.n);
}

static const JSCFunctionListEntry sdb_kd_proto[] = {
    JS_CFUNC_MAGIC_DEF("knn", 2, sdb_kd_knn, 0),
    JS_CFUNC_MAGIC_DEF("nearest", 1, sdb_kd_knn, 1),
    JS_CFUNC_MAGIC_DEF("knnMany", 2, sdb_kd_knn, 2),
    JS_CFUNC_MAGIC_DEF("radius", 2, sdb_kd_region, 0),
    JS_CFUNC_MAGIC_DEF("box", 2, sdb_kd_region, 1),
    JS_CFUNC_DEF("point", 1, sdb_kd_point_m),
    JS_CGETSET_MAGIC_DEF("size", sdb_kd_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("dims", sdb_kd_getter, NULL, 1),
};
typedef struct {
    dyn_chash_t c;
} sdb_ch_w;

static JSClassID sdb_ch_class_id;

static void sdb_ch_free(void* native)
{
    sdb_ch_w* w = (sdb_ch_w*)native;
    if (!w)
        return;
    dyn_chash_free(&w->c);
    dyn_ds_free(w);
}

static void sdb_ch_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ch_free(JS_GetOpaque(val, sdb_ch_class_id));
}

static const JSClassDef sdb_ch_class = {
    "ConsistentHash",
    .finalizer = sdb_ch_finalizer,
};

static JSValue sdb_ch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    uint32_t rep = 160;
    if (argc >= 1 && sdb_opt_u32(ctx, argv[0], "replicas", &rep))
        return JS_EXCEPTION;
    if (rep < 1 || rep > DYN_CHASH_MAX_REPLICAS)
        return JS_ThrowRangeError(ctx, "replicas must be 1..%u", DYN_CHASH_MAX_REPLICAS);
    w = (sdb_ch_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    dyn_chash_init(&w->c, rep);
    return dyn_plain_wrap(ctx, new_target, sdb_ch_class_id, w, sdb_ch_free);
}

static JSValue sdb_ch_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    ds2_str k;
    uint32_t weight = 1;
    int rc;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (dyn_size_arg(ctx, argv[1], DYN_CHASH_MAX_WEIGHT, &weight))
            return JS_EXCEPTION;
        if (weight == 0)
            return JS_ThrowRangeError(ctx, "weight must be 1..%u", DYN_CHASH_MAX_WEIGHT);
    }
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = dyn_chash_set_node(&w->c, k.p, k.n, weight);
    ds2_str_release(ctx, &k);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "ConsistentHash: more than %u ring points", DYN_CHASH_MAX_POINTS);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ch_node_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ch_w* w;
    ds2_str k;
    JSValue r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (magic == 0) {
        int rc = dyn_chash_remove_node(&w->c, k.p, k.n);
        r = rc < 0 ? ds2_oom(ctx) : JS_NewBool(ctx, rc);
    } else if (magic == 1) {
        r = JS_NewBool(ctx, dyn_ds_smap_find(&w->c.idx, k.p, k.n, dyn_ds_strhash(k.p, k.n)) != NULL);
    } else {
        if (!w->c.nr)
            r = JS_UNDEFINED;
        else {
            const dyn_chash_node* nd = &w->c.nodes[w->c.ring[dyn_chash_locate(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0))].node];
            r = JS_NewStringLen(ctx, nd->name, nd->len);
        }
    }
    ds2_str_release(ctx, &k);
    return r;
}

static JSValue sdb_ch_get_n(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    ds2_str k;
    uint32_t want, got = 0, i, pos, out = 0;
    uint8_t* seen;
    JSValue arr;
    (void)argc;
    if (dyn_size_arg(ctx, argv[1], 1u << 20, &want))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || !w->c.nr || !want) {
        ds2_str_release(ctx, &k);
        return arr;
    }
    if (want > w->c.nn)
        want = w->c.nn;
    seen = (uint8_t*)dyn_ds_calloc(w->c.nn, 1);
    if (!seen) {
        ds2_str_release(ctx, &k);
        JS_FreeValue(ctx, arr);
        return ds2_oom(ctx);
    }
    pos = dyn_chash_locate(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0));
    ds2_str_release(ctx, &k);
    for (i = 0; i < w->c.nr && got < want; i++) {
        uint32_t nd = w->c.ring[(pos + i) % w->c.nr].node;
        if (seen[nd])
            continue;
        seen[nd] = 1;
        got++;
        if (sdb_push(ctx, arr, &out, JS_NewStringLen(ctx, w->c.nodes[nd].name, w->c.nodes[nd].len)) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
    }
    dyn_ds_free(seen);
    return arr;
}

static JSValue sdb_ch_nodes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    JSValue arr;
    uint32_t i, out = 0;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    for (i = 0; i < w->c.nn && !JS_IsException(arr); i++)
        if (sdb_push(ctx, arr, &out, JS_NewStringLen(ctx, w->c.nodes[i].name, w->c.nodes[i].len)) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    return arr;
}

static JSValue sdb_ch_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ch_w* w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic == 0 ? w->c.nn : magic == 1 ? w->c.replicas
                                                              : w->c.nr);
}

static JSValue sdb_ch_jump(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_str k;
    uint32_t buckets;
    uint64_t key;
    int64_t b = -1, j = 0;
    (void)this_val;
    (void)argc;
    if (dyn_size_arg(ctx, argv[1], 0x7FFFFFFFu, &buckets))
        return JS_EXCEPTION;
    if (buckets == 0)
        return JS_ThrowRangeError(ctx, "buckets must be at least 1");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    key = dyn_xxh64((const uint8_t*)k.p, k.n, 0);
    ds2_str_release(ctx, &k);
    while (j < (int64_t)buckets) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = (int64_t)((double)(b + 1) * ((double)(1LL << 31) / (double)((key >> 33) + 1)));
    }
    return JS_NewInt64(ctx, b);
}

static const JSCFunctionListEntry sdb_ch_proto[] = {
    JS_CFUNC_DEF("addNode", 1, sdb_ch_add),
    JS_CFUNC_MAGIC_DEF("removeNode", 1, sdb_ch_node_op, 0),
    JS_CFUNC_MAGIC_DEF("hasNode", 1, sdb_ch_node_op, 1),
    JS_CFUNC_MAGIC_DEF("get", 1, sdb_ch_node_op, 2),
    JS_CFUNC_DEF("getN", 2, sdb_ch_get_n),
    JS_CFUNC_DEF("nodes", 0, sdb_ch_nodes),
    JS_CGETSET_MAGIC_DEF("size", sdb_ch_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("replicas", sdb_ch_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("points", sdb_ch_getter, NULL, 2),
};
typedef struct {
    dyn_invidx_t x;
} sdb_ii_w;

static JSClassID sdb_ii_class_id;

static void sdb_ii_free(void* native)
{
    sdb_ii_w* w = (sdb_ii_w*)native;
    if (!w)
        return;
    dyn_invidx_free(&w->x);
    dyn_ds_free(w);
}

static void sdb_ii_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ii_free(JS_GetOpaque(val, sdb_ii_class_id));
}

static const JSClassDef sdb_ii_class = {
    "InvertedIndex",
    .finalizer = sdb_ii_finalizer,
};

static sdb_ii_w* sdb_ii_new(double k1, double b)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    dyn_invidx_init(&w->x, k1, b);
    return w;
}

static JSValue sdb_ii_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    double k1 = 1.2, b = 0.75;
    JSValue sw = JS_UNDEFINED;
    JSValueConst opts = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (sdb_opt_f64(ctx, opts, "k1", &k1) || sdb_opt_f64(ctx, opts, "b", &b))
        return JS_EXCEPTION;
    if (!(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1))
        return JS_ThrowRangeError(ctx, "InvertedIndex: k1 must be 0..100 and b 0..1");
    if (sdb_opt_get(ctx, opts, "stopWords", &sw))
        return JS_EXCEPTION;
    w = sdb_ii_new(k1, b);
    if (!w) {
        JS_FreeValue(ctx, sw);
        return ds2_oom(ctx);
    }
    if (!JS_IsUndefined(sw)) {
        JSValue lv;
        uint32_t n, i;
        if (JS_IsArray(ctx, sw) <= 0) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowTypeError(ctx, "stopWords must be an array of strings");
        }
        lv = JS_GetPropertyStr(ctx, sw, "length");
        if (JS_ToUint32(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (n > 100000) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowRangeError(ctx, "stopWords: at most 100000 entries");
        }
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sw, i);
            ds2_str s;
            int rc;
            if (JS_IsException(e) || !JS_IsString(e)) {
                if (!JS_IsException(e))
                    JS_ThrowTypeError(ctx, "stopWords[%u] is not a string", (unsigned)i);
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = ds2_str_get(ctx, &s, e);
            JS_FreeValue(ctx, e);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = dyn_invidx_tokenize(s.p, s.n, dyn_invidx_stop_tok, &w->x);
            ds2_str_release(ctx, &s);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return ds2_oom(ctx);
            }
        }
    }
    JS_FreeValue(ctx, sw);
    return dyn_plain_wrap(ctx, new_target, sdb_ii_class_id, w, sdb_ii_free);
}

static JSValue sdb_ii_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    uint32_t id;
    ds2_str t;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    rc = dyn_invidx_add(&w->x, id, t.p, t.n);
    ds2_str_release(ctx, &t);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ii_id_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ii_w* w;
    uint32_t id;
    dyn_ds_ient* e;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, dyn_invidx_remove(&w->x, id));
    e = dyn_ds_imap_find(&w->x.didx, id);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    return e ? JS_NewInt64(ctx, w->x.docs[e->v].len) : JS_UNDEFINED;
}

static JSValue sdb_ii_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str q;
    uint32_t limit = 10, n, i, out = 0;
    int prefix = 0, and_mode = 0;
    dyn_invidx_hit* h;
    JSValue arr, mode = JS_UNDEFINED;
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || sdb_opt_bool(ctx, opts, "prefix", &prefix) || sdb_opt_get(ctx, opts, "mode", &mode))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(mode)) {
        const char* m = JS_ToCString(ctx, mode);
        JS_FreeValue(ctx, mode);
        if (!m)
            return JS_EXCEPTION;
        if (!strcmp(m, "and"))
            and_mode = 1;
        else if (strcmp(m, "or")) {
            JS_FreeCString(ctx, m);
            return JS_ThrowRangeError(ctx, "mode must be \"and\" or \"or\"");
        }
        JS_FreeCString(ctx, m);
    }
    if (limit > DYN_DS_MAX_ITEMS)
        limit = DYN_DS_MAX_ITEMS;
    if (ds2_str_get(ctx, &q, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &q);
        return JS_EXCEPTION;
    }
    if (dyn_invidx_search(&w->x, q.p, q.n, and_mode, prefix, limit, &h, &n) < 0) {
        ds2_str_release(ctx, &q);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &q);
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++)
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewInt64(ctx, h[i].id), JS_NewFloat64(ctx, h[i].s))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    dyn_ds_free(h);
    return arr;
}

typedef struct {
    char buf[DYN_INVIDX_TOK_MAX];
    size_t n;
} ii_first_ctx;

static int ii_first_tok(void* ud, const char* t, size_t n)
{
    ii_first_ctx* c = (ii_first_ctx*)ud;
    memcpy(c->buf, t, n);
    c->n = n;
    return 1;
}

static JSValue sdb_ii_doc_freq(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    int64_t ti;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    dyn_invidx_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    if (!c.n)
        return JS_NewInt32(ctx, 0);
    ti = dyn_invidx_term_get(&w->x, c.buf, c.n, 0);
    return JS_NewInt64(ctx, ti >= 0 ? w->x.terms[ti].n : 0);
}

typedef struct {
    uint32_t df;
    uint32_t ti;
} ii_sugg;

static _Thread_local const dyn_invidx_t* ii_sort_ctx;

static int ii_sugg_cmp(const void* a, const void* b)
{
    const ii_sugg *x = (const ii_sugg*)a, *y = (const ii_sugg*)b;
    const dyn_invidx_term *tx, *ty;
    uint32_t m;
    int c;
    if (x->df != y->df)
        return x->df > y->df ? -1 : 1;
    tx = &ii_sort_ctx->terms[x->ti];
    ty = &ii_sort_ctx->terms[y->ti];
    m = tx->len < ty->len ? tx->len : ty->len;
    c = m ? memcmp(tx->s, ty->s, m) : 0;
    return c ? c : (tx->len > ty->len) - (tx->len < ty->len);
}

static JSValue sdb_ii_suggest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    uint32_t limit = 10, i, n = 0, out = 0;
    ii_sugg* s;
    JSValue arr;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && dyn_size_arg(ctx, argv[1], DYN_DS_MAX_ITEMS, &limit))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    dyn_invidx_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    s = (ii_sugg*)dyn_ds_malloc(sizeof(ii_sugg) * ((size_t)w->x.nterms + 1));
    if (!s)
        return ds2_oom(ctx);
    for (i = 0; i < w->x.nterms; i++) {
        const dyn_invidx_term* tm = &w->x.terms[i];
        if (tm->n && tm->len >= c.n && (c.n == 0 || memcmp(tm->s, c.buf, c.n) == 0)) {
            s[n].df = tm->n;
            s[n].ti = i;
            n++;
        }
    }
    ii_sort_ctx = &w->x;
    qsort(s, n, sizeof(ii_sugg), ii_sugg_cmp);
    ii_sort_ctx = NULL;
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && i < limit && !JS_IsException(arr); i++) {
        const dyn_invidx_term* tm = &w->x.terms[s[i].ti];
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewStringLen(ctx, tm->s, tm->len), JS_NewInt64(ctx, tm->n))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(s);
    return arr;
}

static int ii_collect_tok(void* ud, const char* t, size_t n)
{
    JSContext* ctx = ((JSContext**)ud)[0];
    JSValue* arr = (JSValue*)((void**)ud)[1];
    uint32_t* i = (uint32_t*)((void**)ud)[2];
    return sdb_push(ctx, *arr, i, JS_NewStringLen(ctx, t, n)) < 0 ? -1 : 0;
}

static JSValue sdb_ii_tokenize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_str t;
    JSValue arr;
    uint32_t i = 0;
    void* ud[3];
    (void)this_val;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        ud[0] = ctx;
        ud[1] = &arr;
        ud[2] = &i;
        if (dyn_invidx_tokenize(t.p, t.n, ii_collect_tok, ud)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    ds2_str_release(ctx, &t);
    return arr;
}

static JSValue sdb_ii_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    uint32_t i, n = 0;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->x.live);
    if (magic == 1) {
        for (i = 0; i < w->x.nterms; i++)
            n += w->x.terms[i].n != 0;
        return JS_NewInt64(ctx, n);
    }
    return JS_NewFloat64(ctx, w->x.live ? (double)w->x.total_len / w->x.live : 0);
}

static const JSCFunctionListEntry sdb_ii_proto[] = {
    JS_CFUNC_DEF("add", 2, sdb_ii_add),
    JS_CFUNC_MAGIC_DEF("remove", 1, sdb_ii_id_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_ii_id_op, 1),
    JS_CFUNC_MAGIC_DEF("docLength", 1, sdb_ii_id_op, 2),
    JS_CFUNC_DEF("search", 1, sdb_ii_search),
    JS_CFUNC_DEF("docFreq", 1, sdb_ii_doc_freq),
    JS_CFUNC_DEF("suggest", 1, sdb_ii_suggest),
    JS_CGETSET_MAGIC_DEF("size", sdb_ii_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("termCount", sdb_ii_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("avgLength", sdb_ii_getter, NULL, 2),
};
typedef struct {
    dyn_interner_t t;
} sdb_in_w;

static JSClassID sdb_in_class_id;

static void sdb_in_free(void* native)
{
    sdb_in_w* w = (sdb_in_w*)native;
    if (!w)
        return;
    dyn_interner_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_in_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_in_free(JS_GetOpaque(val, sdb_in_class_id));
}

static const JSClassDef sdb_in_class = {
    "Interner",
    .finalizer = sdb_in_finalizer,
};

static JSValue sdb_in_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_in_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_in_class_id, w, sdb_in_free);
}

static JSValue sdb_in_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_in_w* w;
    ds2_str k;
    int64_t id;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    id = dyn_interner_intern(&w->t, k.p, k.n, magic == 0);
    ds2_str_release(ctx, &k);
    if (id == -2)
        return ds2_oom(ctx);
    if (magic == 2)
        return JS_NewBool(ctx, id >= 0);
    return id >= 0 ? JS_NewInt64(ctx, id) : JS_UNDEFINED;
}

static JSValue sdb_in_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    double d;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= (double)w->t.n)
        return JS_UNDEFINED;
    return JS_NewStringLen(ctx, w->t.s[(uint32_t)d].p, w->t.s[(uint32_t)d].len);
}

static JSValue sdb_in_intern_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    JSValue lv;
    uint32_t n, i;
    uint32_t* ids;
    JSValue out;
    (void)argc;
    if (JS_IsArray(ctx, argv[0]) <= 0)
        return JS_ThrowTypeError(ctx, "internMany: expected an array of strings");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n > DYN_DS_MAX_ITEMS)
        return JS_ThrowRangeError(ctx, "internMany: more than %u strings", DYN_DS_MAX_ITEMS);
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    ids = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * ((size_t)n + 1));
    if (!ids)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        JSValue* fast;
        uint32_t cnt;
        JSValue e;
        ds2_str k;
        int64_t id;
        if (JS_GetFastArray(ctx, argv[0], &fast, &cnt) && i < cnt)
            e = JS_DupValue(ctx, fast[i]);
        else
            e = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(e)) {
            dyn_ds_free(ids);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            dyn_ds_free(ids);
            return JS_ThrowTypeError(ctx, "internMany: element %u is not a string", (unsigned)i);
        }
        if (ds2_str_get(ctx, &k, e)) {
            JS_FreeValue(ctx, e);
            dyn_ds_free(ids);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
        id = w ? dyn_interner_intern(&w->t, k.p, k.n, 1) : -3;
        ds2_str_release(ctx, &k);
        if (id < 0) {
            dyn_ds_free(ids);
            return id == -3 ? JS_EXCEPTION : ds2_oom(ctx);
        }
        ids[i] = (uint32_t)id;
    }
    out = sdb_typed_out(ctx, ids, n, JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(ids);
    return out;
}

static JSValue sdb_in_get_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    uint32_t *ids, n, i, out = 0;
    JSValue arr;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "getMany", &ids, &n))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        dyn_ds_free(ids);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        JSValue v = ids[i] < w->t.n ? JS_NewStringLen(ctx, w->t.s[ids[i]].p, w->t.s[ids[i]].len) : JS_UNDEFINED;
        if (sdb_push(ctx, arr, &out, v) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ids);
    return arr;
}

static JSValue sdb_in_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_in_w* w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    return magic ? JS_NewInt64(ctx, (int64_t)w->t.bytes) : JS_NewInt64(ctx, w->t.n);
}

static const JSCFunctionListEntry sdb_in_proto[] = {
    JS_CFUNC_MAGIC_DEF("intern", 1, sdb_in_op, 0),
    JS_CFUNC_MAGIC_DEF("lookup", 1, sdb_in_op, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_in_op, 2),
    JS_CFUNC_DEF("get", 1, sdb_in_get),
    JS_CFUNC_DEF("internMany", 1, sdb_in_intern_many),
    JS_CFUNC_DEF("getMany", 1, sdb_in_get_many),
    JS_CGETSET_MAGIC_DEF("size", sdb_in_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_in_getter, NULL, 1),
};

#define DYN_TID_ZSET 31
#define DYN_TID_ORDEREDMAP 32
#define DYN_TID_ROARING 33
#define DYN_TID_TDIGEST 34
#define DYN_TID_TOPK 35
#define DYN_TID_CUCKOO 36
#define DYN_TID_INDEXEDHEAP 37
#define DYN_TID_KDTREE 38
#define DYN_TID_CONSISTENTHASH 39
#define DYN_TID_INVERTEDINDEX 40
#define DYN_TID_INTERNER 41

static JSValue sdb_bad(JSContext* ctx)
{
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
}

static int sdb_zset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_zset_w* z = JS_GetOpaque(obj, sdb_zset_class_id);
    dyn_zsl_node* x;
    (void)ctx;
    if (dyn_ser_u32(w, z->sl.len) < 0)
        return -1;
    for (x = z->sl.head->lv[0].fwd; x; x = x->lv[0].fwd)
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || dyn_ser_f64(w, x->score) < 0)
            return -1;
    return 0;
}

static JSValue sdb_zset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_zset_w* z;
    (void)opts;
    if (dyn_de_count(r, &n, 12) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 96) < 0)
        return sdb_bad(ctx);
    z = sdb_zset_new(ctx);
    if (!z)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        double sc = dyn_de_f64(r);
        if (!dyn_de_ok(r) || sc != sc || dyn_zsl_find(&z->sl, k, kn)) {
            sdb_zset_free(z);
            return sdb_bad(ctx);
        }
        if (sdb_zsl_insert(&z->sl, sc, k, kn, JS_UNDEFINED, NULL) < 0) {
            sdb_zset_free(z);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_zset_class_id, z, sdb_zset_free);
}

static int sdb_omap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_omap_w* o = JS_GetOpaque(obj, sdb_omap_class_id);
    dyn_zsl_node* x;
    JSValue vals;
    uint32_t i = 0;
    int rc;
    if (dyn_ser_u32(w, o->sl.len) < 0)
        return -1;
    vals = JS_NewArray(ctx);
    if (JS_IsException(vals))
        return -1;
    for (x = o->sl.head->lv[0].fwd; x; x = x->lv[0].fwd) {
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || sdb_push(ctx, vals, &i, JS_DupValue(ctx, ds_cell_val(&x->val))) < 0) {
            JS_FreeValue(ctx, vals);
            return -1;
        }
    }
    rc = dyn_codec_write_values(ctx, w, vals);
    JS_FreeValue(ctx, vals);
    return rc;
}

static JSValue sdb_omap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_omap_w* o;
    JSValue vals, obj;
    const char** ks;
    size_t* kl;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 112) < 0)
        return sdb_bad(ctx);
    ks = (const char**)dyn_ds_malloc(sizeof(char*) * ((size_t)n + 1));
    kl = (size_t*)dyn_ds_malloc(sizeof(size_t) * ((size_t)n + 1));
    if (!ks || !kl) {
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        ks[i] = dyn_de_blob(r, &kl[i]);
        if (!dyn_de_ok(r)) {
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return sdb_bad(ctx);
        }
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return vals;
    }
    o = (sdb_omap_w*)dyn_ds_malloc(sizeof(*o));
    if (!o || sdb_zsl_init(&o->sl, ctx) < 0) {
        dyn_ds_free(o);
        JS_FreeValue(ctx, vals);
        dyn_ds_free(ks);
        dyn_ds_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v;
        if (dyn_zsl_find(&o->sl, ks[i], kl[i])) {
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return sdb_bad(ctx);
        }
        v = JS_GetPropertyUint32(ctx, vals, i);
        if (JS_IsException(v) || sdb_zsl_insert(&o->sl, 0, ks[i], kl[i], v, NULL) < 0) {
            JS_FreeValue(ctx, v);
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(ks);
            dyn_ds_free(kl);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(ks);
    dyn_ds_free(kl);
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_omap_class_id, o, sdb_omap_free);
    return obj;
}

static int sdb_rb_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_rb_w* b = JS_GetOpaque(obj, sdb_rb_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, b->r.n) < 0)
        return -1;
    for (i = 0; i < b->r.n; i++) {
        const dyn_roar_ct* c = &b->r.c[i];
        if (dyn_ser_u16(w, c->key) < 0 || dyn_ser_u8(w, c->type) < 0 || dyn_ser_u32(w, c->card) < 0)
            return -1;
        if (c->type == DYN_ROAR_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            uint32_t j;
            for (j = 0; j < c->card; j++)
                if (dyn_ser_u16(w, a[j]) < 0)
                    return -1;
        } else if (c->type == DYN_ROAR_BMP) {
            const uint64_t* x = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < DYN_ROAR_WORDS; j++)
                if (dyn_ser_u64(w, x[j]) < 0)
                    return -1;
        }
    }
    return 0;
}

static JSValue sdb_rb_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_rb_w* b;
    (void)opts;
    if (dyn_de_count(r, &n, 7) < 0 || n > 65536)
        return sdb_bad(ctx);
    b = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*b));
    if (!b || dyn_roar_reserve(&b->r, n) < 0) {
        dyn_ds_free(b);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        dyn_roar_ct* c = &b->r.c[i];
        uint16_t key = dyn_de_u16(r);
        uint8_t type = dyn_de_u8(r);
        uint32_t card = dyn_de_u32(r), j;
        memset(c, 0, sizeof(*c));
        if (!dyn_de_ok(r) || (i && key <= b->r.c[i - 1].key))
            goto bad;
        c->key = key;
        if (type == DYN_ROAR_ARR) {
            uint16_t* a;
            if (card == 0 || card > DYN_ROAR_ARR_MAX || dyn_de_left(r) < (size_t)card * 2 || dyn_de_charge(r, (uint64_t)card * 2) < 0)
                goto bad;
            a = (uint16_t*)dyn_ds_malloc(sizeof(uint16_t) * card);
            if (!a)
                goto oom;
            for (j = 0; j < card; j++) {
                a[j] = dyn_de_u16(r);
                if (j && a[j] <= a[j - 1]) {
                    dyn_ds_free(a);
                    goto bad;
                }
            }
            c->type = DYN_ROAR_ARR;
            c->data = a;
            c->cap = card;
            c->card = card;
        } else if (type == DYN_ROAR_BMP) {
            uint64_t* x;
            if (card <= DYN_ROAR_ARR_MAX || card >= 65536 || dyn_de_left(r) < 8u * DYN_ROAR_WORDS || dyn_de_charge(r, 8u * DYN_ROAR_WORDS) < 0)
                goto bad;
            x = (uint64_t*)dyn_ds_malloc(sizeof(uint64_t) * DYN_ROAR_WORDS);
            if (!x)
                goto oom;
            for (j = 0; j < DYN_ROAR_WORDS; j++)
                x[j] = dyn_de_u64(r);
            if (dyn_roar_popcnt(x) != card) {
                dyn_ds_free(x);
                goto bad;
            }
            c->type = DYN_ROAR_BMP;
            c->data = x;
            c->card = card;
        } else if (type == DYN_ROAR_FULL) {
            if (card != 65536)
                goto bad;
            c->type = DYN_ROAR_FULL;
            c->card = 65536;
        } else
            goto bad;
        if (!dyn_de_ok(r))
            goto bad_inc;
        b->r.n = i + 1;
        continue;
    bad_inc:
        b->r.n = i + 1;
        goto bad;
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, b, sdb_rb_free);
bad:
    sdb_rb_free(b);
    return sdb_bad(ctx);
oom:
    sdb_rb_free(b);
    return ds2_oom(ctx);
}

static int sdb_td_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_td_w* t = JS_GetOpaque(obj, sdb_td_class_id);
    uint32_t i;
    (void)ctx;
    dyn_tdigest_compress(&t->t);
    if (dyn_ser_f64(w, t->t.delta) < 0 || dyn_ser_f64(w, t->t.min) < 0 || dyn_ser_f64(w, t->t.max) < 0 || dyn_ser_f64(w, t->t.sum) < 0 || dyn_ser_u32(w, t->t.n) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++)
        if (dyn_ser_f64(w, t->t.c[i].m) < 0 || dyn_ser_f64(w, t->t.c[i].w) < 0)
            return -1;
    return 0;
}

static JSValue sdb_td_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double delta = dyn_de_f64(r), mn = dyn_de_f64(r), mx = dyn_de_f64(r), sum = dyn_de_f64(r);
    uint32_t n, i;
    sdb_td_w* t;
    double total = 0;
    (void)opts;
    if (!dyn_de_ok(r) || !(delta >= DYN_TDIGEST_MIN_DELTA && delta <= DYN_TDIGEST_MAX_DELTA) || dyn_de_count(r, &n, 16) < 0)
        return sdb_bad(ctx);
    t = (sdb_td_w*)dyn_ds_calloc(1, sizeof(*t));
    if (!t || dyn_tdigest_init(&t->t, delta) < 0) {
        dyn_ds_free(t);
        return ds2_oom(ctx);
    }
    if (n > t->t.cap || (n && !(mn <= mx && isfinite(mn) && isfinite(mx))) || !isfinite(sum)) {
        sdb_td_free(t);
        return sdb_bad(ctx);
    }
    for (i = 0; i < n; i++) {
        double m = dyn_de_f64(r), wt = dyn_de_f64(r);
        if (!dyn_de_ok(r) || !isfinite(m) || !(wt > 0) || !isfinite(wt) || m < mn || m > mx || (i && m < t->t.c[i - 1].m)) {
            sdb_td_free(t);
            return sdb_bad(ctx);
        }
        t->t.c[i].m = m;
        t->t.c[i].w = wt;
        total += wt;
    }
    t->t.n = n;
    t->t.total = total;
    t->t.sum = sum;
    t->t.min = n ? mn : HUGE_VAL;
    t->t.max = n ? mx : -HUGE_VAL;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_td_class_id, t, sdb_td_free);
}

static int sdb_tk_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_tk_w* t = JS_GetOpaque(obj, sdb_tk_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.k) < 0 || dyn_ser_u32(w, t->t.n) < 0 || dyn_ser_f64(w, t->t.total) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++) {
        const dyn_topk_slot* s = &t->t.s[i];
        if (dyn_ser_blob(w, s->key, s->klen) < 0 || dyn_ser_f64(w, s->count) < 0 || dyn_ser_f64(w, s->err) < 0)
            return -1;
    }
    return 0;
}

static JSValue sdb_tk_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t k = dyn_de_u32(r), n, i;
    double total;
    sdb_tk_w* t;
    (void)opts;
    if (!dyn_de_ok(r) || k == 0 || k > DYN_TOPK_MAX_K || dyn_de_count(r, &n, 20) < 0 || n > k)
        return sdb_bad(ctx);
    total = dyn_de_f64(r);
    if (!dyn_de_ok(r) || !(total >= 0) || !isfinite(total) || dyn_de_charge(r, (uint64_t)k * 48) < 0)
        return sdb_bad(ctx);
    t = sdb_tk_new(k);
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* key = dyn_de_blob(r, &kn);
        double c = dyn_de_f64(r), e = dyn_de_f64(r);
        dyn_topk_slot* s = &t->t.s[i];
        uint64_t h;
        if (!dyn_de_ok(r) || !(c > 0) || !isfinite(c) || !(e >= 0) || e > c || kn > UINT32_MAX - 1) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        h = dyn_ds_strhash(key, kn);
        if (dyn_ds_smap_find(&t->t.idx, key, kn, h)) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        s->key = (char*)dyn_ds_bytes_dup(key, kn);
        if (!s->key || !dyn_ds_smap_insert(&t->t.idx, s->key, kn, h, i)) {
            dyn_ds_free(s->key);
            s->key = NULL;
            sdb_tk_free(t);
            return ds2_oom(ctx);
        }
        s->klen = (uint32_t)kn;
        s->count = c;
        s->err = e;
        t->t.heap[i] = i;
        s->hpos = i;
        t->t.n = i + 1;
        dyn_topk_up(&t->t, i);
    }
    t->t.total = total;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_tk_class_id, t, sdb_tk_free);
}

static int sdb_cf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_cf_w* f = JS_GetOpaque(obj, sdb_cf_class_id);
    uint64_t i, total = (uint64_t)f->c.nb * DYN_CUCKOO_SLOTS;
    (void)ctx;
    if (dyn_ser_u32(w, f->c.nb) < 0 || dyn_ser_u32(w, f->c.count) < 0 || dyn_ser_u8(w, (uint8_t)f->c.has_victim) < 0 || dyn_ser_u16(w, f->c.victim_fp) < 0 || dyn_ser_u32(w, f->c.victim_i) < 0)
        return -1;
    for (i = 0; i < total; i++)
        if (dyn_ser_u16(w, f->c.t[i]) < 0)
            return -1;
    return 0;
}

static JSValue sdb_cf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t nb = dyn_de_u32(r), count = dyn_de_u32(r), vi, used = 0;
    uint8_t hv = dyn_de_u8(r);
    uint16_t vf = dyn_de_u16(r);
    uint64_t i, total;
    sdb_cf_w* f;
    (void)opts;
    vi = dyn_de_u32(r);
    if (!dyn_de_ok(r) || nb == 0 || (nb & (nb - 1)) || (uint64_t)nb * DYN_CUCKOO_SLOTS > 2ull * DYN_CUCKOO_MAX_CAPACITY || hv > 1 || (hv && (!vf || vi >= nb)))
        return sdb_bad(ctx);
    total = (uint64_t)nb * DYN_CUCKOO_SLOTS;
    if (dyn_de_left(r) < total * 2 || dyn_de_charge(r, total * 2) < 0)
        return sdb_bad(ctx);
    f = sdb_cf_new(nb);
    if (!f)
        return ds2_oom(ctx);
    for (i = 0; i < total; i++) {
        f->c.t[i] = dyn_de_u16(r);
        used += f->c.t[i] != 0;
    }
    if (!dyn_de_ok(r) || used + hv != count) {
        sdb_cf_free(f);
        return sdb_bad(ctx);
    }
    f->c.count = count;
    f->c.has_victim = hv;
    f->c.victim_fp = vf;
    f->c.victim_i = vi;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_cf_class_id, f, sdb_cf_free);
}

static int sdb_ih_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ih_w* h = JS_GetOpaque(obj, sdb_ih_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u8(w, (uint8_t)h->q.max) < 0 || dyn_ser_u32(w, h->q.n) < 0)
        return -1;
    for (i = 0; i < h->q.n; i++)
        if (dyn_ser_u32(w, h->q.h[i].id) < 0 || dyn_ser_f64(w, h->q.h[i].p) < 0)
            return -1;
    return 0;
}

static JSValue sdb_ih_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t mx = dyn_de_u8(r);
    uint32_t n, i;
    sdb_ih_w* h;
    (void)opts;
    if (!dyn_de_ok(r) || mx > 1 || dyn_de_count(r, &n, 12) < 0 || dyn_de_charge(r, (uint64_t)n * 40) < 0)
        return sdb_bad(ctx);
    h = sdb_ih_new(mx);
    if (!h)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        uint32_t id = dyn_de_u32(r);
        double p = dyn_de_f64(r);
        if (!dyn_de_ok(r) || p != p || dyn_ds_imap_find(&h->q.pos, id)) {
            sdb_ih_free(h);
            return sdb_bad(ctx);
        }
        if (dyn_iheap_push(&h->q, id, p) < 0) {
            sdb_ih_free(h);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ih_class_id, h, sdb_ih_free);
}

static int sdb_kd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_kd_w* k = JS_GetOpaque(obj, sdb_kd_class_id);
    size_t total = (size_t)k->t.n * k->t.d;
    (void)ctx;
    if (dyn_ser_u32(w, k->t.d) < 0 || dyn_ser_u32(w, k->t.n) < 0)
        return -1;
    return dyn_ser_f64s(w, k->t.pts, total);
}

static JSValue sdb_kd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t d = dyn_de_u32(r), n = dyn_de_u32(r);
    uint64_t total, i;
    double* pts;
    (void)opts;
    if (!dyn_de_ok(r) || d == 0 || d > DYN_KDTREE_MAX_DIMS)
        return sdb_bad(ctx);
    total = (uint64_t)n * d;
    if (total > DYN_DS_MAX_ITEMS || dyn_de_left(r) < total * 8 || dyn_de_charge(r, total * 24) < 0)
        return sdb_bad(ctx);
    pts = (double*)dyn_ds_malloc(sizeof(double) * (size_t)(total + 1));
    if (!pts)
        return ds2_oom(ctx);
    for (i = 0; i < total; i++)
        pts[i] = dyn_de_f64(r);
    if (!dyn_de_ok(r)) {
        dyn_ds_free(pts);
        return sdb_bad(ctx);
    }
    return sdb_kd_wrap(ctx, JS_UNDEFINED, pts, n, d);
}

static int sdb_ch_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ch_w* c = JS_GetOpaque(obj, sdb_ch_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, c->c.replicas) < 0 || dyn_ser_u32(w, c->c.nn) < 0)
        return -1;
    for (i = 0; i < c->c.nn; i++)
        if (dyn_ser_blob(w, c->c.nodes[i].name, c->c.nodes[i].len) < 0 || dyn_ser_u32(w, c->c.nodes[i].weight) < 0)
            return -1;
    return 0;
}

static JSValue sdb_ch_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t rep = dyn_de_u32(r), n, i;
    sdb_ch_w* c;
    (void)opts;
    if (!dyn_de_ok(r) || rep < 1 || rep > DYN_CHASH_MAX_REPLICAS || dyn_de_count(r, &n, 8) < 0)
        return sdb_bad(ctx);
    c = (sdb_ch_w*)dyn_ds_calloc(1, sizeof(*c));
    if (!c)
        return ds2_oom(ctx);
    dyn_chash_init(&c->c, rep);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        uint32_t wt = dyn_de_u32(r);
        int rc;
        if (!dyn_de_ok(r) || wt < 1 || wt > DYN_CHASH_MAX_WEIGHT || dyn_ds_smap_find(&c->c.idx, k, kn, dyn_ds_strhash(k, kn))) {
            sdb_ch_free(c);
            return sdb_bad(ctx);
        }
        rc = dyn_chash_set_node(&c->c, k, kn, wt);
        if (rc < 0) {
            sdb_ch_free(c);
            return rc == -2 ? sdb_bad(ctx) : ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ch_class_id, c, sdb_ch_free);
}

static int sdb_ii_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ii_w* ii = JS_GetOpaque(obj, sdb_ii_class_id);
    dyn_invidx_t* x = &ii->x;
    uint32_t i, j;
    (void)ctx;
    if (dyn_ser_f64(w, x->k1) < 0 || dyn_ser_f64(w, x->b) < 0 || dyn_ser_u32(w, x->nstop) < 0)
        return -1;
    for (i = 0; i < x->nstop; i++)
        if (dyn_ser_blob(w, x->stopw[i], strlen(x->stopw[i])) < 0)
            return -1;
    if (dyn_ser_u32(w, x->nterms) < 0)
        return -1;
    for (i = 0; i < x->nterms; i++)
        if (dyn_ser_blob(w, x->terms[i].s, x->terms[i].len) < 0)
            return -1;
    if (dyn_ser_u32(w, x->live) < 0)
        return -1;
    for (i = 0; i < x->nslots; i++) {
        const dyn_invidx_doc* d = &x->docs[i];
        if (!d->alive)
            continue;
        if (dyn_ser_u32(w, d->id) < 0 || dyn_ser_u32(w, d->len) < 0 || dyn_ser_u32(w, d->nt) < 0)
            return -1;
        for (j = 0; j < d->nt; j++)
            if (dyn_ser_u32(w, d->t[j].term) < 0 || dyn_ser_u32(w, d->t[j].tf) < 0)
                return -1;
    }
    return 0;
}

static JSValue sdb_ii_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double k1 = dyn_de_f64(r), b = dyn_de_f64(r);
    uint32_t ns, nt, nd, i, j;
    sdb_ii_w* ii;
    dyn_invidx_t* x;
    (void)opts;
    if (!dyn_de_ok(r) || !(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1) || dyn_de_count(r, &ns, 4) < 0)
        return sdb_bad(ctx);
    ii = sdb_ii_new(k1, b);
    if (!ii)
        return ds2_oom(ctx);
    x = &ii->x;
    for (i = 0; i < ns; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        if (!dyn_de_ok(r) || n == 0 || n > DYN_INVIDX_TOK_MAX || memchr(s, 0, n))
            goto bad;
        if (dyn_invidx_add_stop(x, s, n) < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nt, 4) < 0 || dyn_de_charge(r, (uint64_t)nt * 96) < 0)
        goto bad;
    for (i = 0; i < nt; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        int64_t ti;
        if (!dyn_de_ok(r) || n == 0 || n > DYN_INVIDX_TOK_MAX || dyn_invidx_term_get(x, s, n, 0) >= 0)
            goto bad;
        ti = dyn_invidx_term_get(x, s, n, 1);
        if (ti < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nd, 12) < 0 || dyn_de_charge(r, (uint64_t)nd * 64) < 0)
        goto bad;
    for (i = 0; i < nd; i++) {
        uint32_t id = dyn_de_u32(r), len = dyn_de_u32(r), cnt;
        uint64_t sumtf = 0;
        dyn_invidx_dt* list;
        if (!dyn_de_ok(r) || dyn_ds_imap_find(&x->didx, id) || dyn_de_count(r, &cnt, 8) < 0 || dyn_de_charge(r, (uint64_t)cnt * 24) < 0)
            goto bad;
        list = (dyn_invidx_dt*)dyn_ds_malloc(sizeof(dyn_invidx_dt) * ((size_t)cnt + 1));
        if (!list)
            goto oom;
        if (dyn_invidx_tscratch(x) < 0) {
            dyn_ds_free(list);
            goto oom;
        }
        dyn_invidx_bump_tepoch(x);
        for (j = 0; j < cnt; j++) {
            list[j].term = dyn_de_u32(r);
            list[j].tf = dyn_de_u32(r);
            if (!dyn_de_ok(r) || list[j].term >= x->nterms || list[j].tf == 0 || x->tstamp[list[j].term] == x->tepoch) {
                dyn_ds_free(list);
                goto bad;
            }
            x->tstamp[list[j].term] = x->tepoch;
            sumtf += list[j].tf;
        }
        if (sumtf != len) {
            dyn_ds_free(list);
            goto bad;
        }
        if (dyn_invidx_install(x, id, list, cnt, len) < 0) {
            dyn_ds_free(list);
            goto oom;
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ii_class_id, ii, sdb_ii_free);
bad:
    sdb_ii_free(ii);
    return sdb_bad(ctx);
oom:
    sdb_ii_free(ii);
    return ds2_oom(ctx);
}

static int sdb_in_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_in_w* t = JS_GetOpaque(obj, sdb_in_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.n) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++)
        if (dyn_ser_blob(w, t->t.s[i].p, t->t.s[i].len) < 0)
            return -1;
    return 0;
}

static JSValue sdb_in_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_in_w* t;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0 || dyn_de_charge(r, (uint64_t)n * 48) < 0)
        return sdb_bad(ctx);
    t = (sdb_in_w*)dyn_ds_calloc(1, sizeof(*t));
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        int64_t id;
        if (!dyn_de_ok(r) || dyn_interner_intern(&t->t, k, kn, 0) >= 0) {
            sdb_in_free(t);
            return sdb_bad(ctx);
        }
        id = dyn_interner_intern(&t->t, k, kn, 1);
        if (id < 0) {
            sdb_in_free(t);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_in_class_id, t, sdb_in_free);
}

static const dyn_codec_t sdb_codecs[] = {
    { 0, DYN_TID_ZSET, "ZSet", sdb_zset_write, sdb_zset_read },
    { 0, DYN_TID_ORDEREDMAP, "OrderedMap", sdb_omap_write, sdb_omap_read },
    { 0, DYN_TID_ROARING, "RoaringBitmap", sdb_rb_write, sdb_rb_read },
    { 0, DYN_TID_TDIGEST, "TDigest", sdb_td_write, sdb_td_read },
    { 0, DYN_TID_TOPK, "TopK", sdb_tk_write, sdb_tk_read },
    { 0, DYN_TID_CUCKOO, "CuckooFilter", sdb_cf_write, sdb_cf_read },
    { 0, DYN_TID_INDEXEDHEAP, "IndexedHeap", sdb_ih_write, sdb_ih_read },
    { 0, DYN_TID_KDTREE, "KDTree", sdb_kd_write, sdb_kd_read },
    { 0, DYN_TID_CONSISTENTHASH, "ConsistentHash", sdb_ch_write, sdb_ch_read },
    { 0, DYN_TID_INVERTEDINDEX, "InvertedIndex", sdb_ii_write, sdb_ii_read },
    { 0, DYN_TID_INTERNER, "Interner", sdb_in_write, sdb_in_read },
};

static JSClassID* const sdb_class_ids[] = {
    &sdb_zset_class_id,
    &sdb_omap_class_id,
    &sdb_rb_class_id,
    &sdb_td_class_id,
    &sdb_tk_class_id,
    &sdb_cf_class_id,
    &sdb_ih_class_id,
    &sdb_kd_class_id,
    &sdb_ch_class_id,
    &sdb_ii_class_id,
    &sdb_in_class_id,
};

static int sdb_register_codecs(void)
{
    size_t i;
    for (i = 0; i < countof(sdb_codecs); i++) {
        dyn_codec_t c = sdb_codecs[i];
        c.class_id = *sdb_class_ids[i];
        if (dyn_codec_register(&c) < 0)
            return -1;
    }
    return 0;
}

static int sdb_static(JSContext* ctx, JSClassID id, const char* name, JSCFunction* fn, int len)
{
    JSValue proto = JS_GetClassProto(ctx, id), ctor;
    int rc = -1;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    if (JS_IsFunction(ctx, ctor))
        rc = JS_DefinePropertyValueStr(ctx, ctor, name, JS_NewCFunction(ctx, fn, name, len), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, proto);
    return rc;
}

#define SDB_REGISTER(pfx, name)                                         \
    if (dyn_register_plain_class(ctx, m, &pfx##_class_id, &pfx##_class, \
            pfx##_proto, countof(pfx##_proto),                          \
            pfx##_ctor, name)                                           \
        < 0)                                                            \
        return -1;

static int sdb_register(JSContext* ctx, JSModuleDef* m)
{
    sdb_capture_typed_cids(ctx);
    SDB_REGISTER(sdb_zset, "ZSet")
    SDB_REGISTER(sdb_omap, "OrderedMap")
    SDB_REGISTER(sdb_rb, "RoaringBitmap")
    SDB_REGISTER(sdb_td, "TDigest")
    SDB_REGISTER(sdb_tk, "TopK")
    SDB_REGISTER(sdb_cf, "CuckooFilter")
    SDB_REGISTER(sdb_ih, "IndexedHeap")
    SDB_REGISTER(sdb_kd, "KDTree")
    SDB_REGISTER(sdb_ch, "ConsistentHash")
    SDB_REGISTER(sdb_ii, "InvertedIndex")
    SDB_REGISTER(sdb_in, "Interner")
    if (sdb_static(ctx, sdb_ch_class_id, "jump", sdb_ch_jump, 2) < 0 || sdb_static(ctx, sdb_ii_class_id, "tokenize", sdb_ii_tokenize, 1) < 0)
        return -1;
    return 0;
}

static void sdb_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = {
        "ZSet",
        "OrderedMap",
        "RoaringBitmap",
        "TDigest",
        "TopK",
        "CuckooFilter",
        "IndexedHeap",
        "KDTree",
        "ConsistentHash",
        "InvertedIndex",
        "Interner",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}
