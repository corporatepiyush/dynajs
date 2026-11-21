#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DYN_DECODE_AMPLIFIED_NUM_MAX (1u << 23)
#define DS_NUM_CONSTANT 1
#define DS_NUM_INT 2
#define DS_NUM_DELTA 3

JSValue dyn_ds_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method,
    int argc, JSValueConst* argv)
{
    JSValue fn, arr, values, iter;

    fn = JS_GetPropertyStr(ctx, this_val, method);
    if (JS_IsException(fn))
        return fn;
    arr = JS_Call(ctx, fn, this_val, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return arr;
    values = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, arr);
        return values;
    }
    iter = JS_Call(ctx, values, arr, 0, NULL);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, arr);
    return iter;
}

int dyn_size_arg(JSContext* ctx, JSValueConst v, uint32_t max,
    uint32_t* out)
{
    double nd;
    if (JS_ToFloat64(ctx, &nd, v))
        return -1;
    if (!(nd >= 0) || nd > (double)max || nd != (double)(uint32_t)nd) {
        JS_ThrowRangeError(ctx, "size must be an integer in 0..%u",
            (unsigned)max);
        return -1;
    }
    *out = (uint32_t)nd;
    return 0;
}

JSValue ds2_tuple3(JSContext* ctx, JSValue a, JSValue b, JSValue c)
{
    JSValue v[3];
    if (JS_IsException(a) || JS_IsException(b) || JS_IsException(c)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, c);
        return JS_EXCEPTION;
    }
    v[0] = a;
    v[1] = b;
    v[2] = c;
    return JS_NewArrayFrom(ctx, 3, v);
}

static JSCFunction* dyn_ds_native_ctor(JSClassID id)
{
    if (id == dsw_bitset_class_id)
        return dsw_bitset_ctor;
    if (id == dsw_uf_class_id)
        return dsw_uf_ctor;
    if (id == dsw_deque_class_id)
        return dsw_deque_ctor;
    if (id == dsw_fenwick_class_id)
        return dsw_fenwick_ctor;
    if (id == dsw_ringbuf_class_id)
        return dsw_ringbuf_ctor;
    if (id == dsw_segtree_class_id)
        return dsw_segtree_ctor;
    if (id == dsw_bloom_class_id)
        return dsw_bloom_ctor;
    if (id == dsw_trie_class_id)
        return dsw_trie_ctor;
    if (id == dsw_lru_class_id)
        return dsw_lru_ctor;
    if (id == dsw_sortedset_class_id)
        return dsw_sortedset_ctor;
    if (id == dsw_sortedmap_class_id)
        return dsw_sortedmap_ctor;
    if (id == dsw_heap_class_id)
        return dsw_heap_ctor;
    if (id == dsw_list_class_id)
        return dsw_list_ctor;
    return NULL;
}

JSValue dyn_ds_construct(JSContext* ctx, JSClassID id, int argc,
    JSValueConst* argv)
{
    JSCFunction* native_ctor = dyn_ds_native_ctor(id);
    JSValue obj;

    if (!native_ctor)
        return JS_ThrowTypeError(ctx, "deserialize: class has no native constructor");
    obj = native_ctor(ctx, JS_UNDEFINED, argc, argv);
    if (!JS_IsException(obj) && !JS_GetOpaque(obj, id)) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "deserialize: constructor produced a foreign object");
    }
    return obj;
}

int dyn_ds_invoke(JSContext* ctx, JSValueConst obj, const char* name,
    int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name), r;

    if (JS_IsException(fn))
        return -1;
    r = JS_Call(ctx, fn, obj, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

int dyn_ds_invoke_atom(JSContext* ctx, JSValueConst obj, JSAtom name,
    int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetProperty(ctx, obj, name), r;

    if (JS_IsException(fn))
        return -1;
    r = JS_Call(ctx, fn, obj, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

JSValue dyn_ds_value_at(JSContext* ctx, JSValueConst vals, uint32_t i)
{
    JSValue* a;
    uint32_t c;

    if (JS_GetFastArray(ctx, vals, &a, &c) && i < c)
        return JS_DupValue(ctx, a[i]);
    return JS_GetPropertyUint32(ctx, vals, i);
}

int dyn_ds_write_values(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, dyn_ds_val_at get)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (JS_IsException(arr))
        return -1;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, get(native, i)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue dyn_ds_read_values(JSContext* ctx, dyn_de_t* r, uint32_t n)
{
    JSValue arr = dyn_codec_read_values(ctx, r), len;
    uint32_t got = 0;

    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &got, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (got != n) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "malformed DYNS record: %u values for %u entries",
            (unsigned)got, (unsigned)n);
    }
    return arr;
}

static int dyn_num_is_int(double v)
{
    static const double neg_zero = -0.0;
    if (v == 0.0)
        return memcmp(&v, &neg_zero, sizeof v) != 0;
    return v == v && v >= -9007199254740992.0 && v <= 9007199254740992.0 && v == (double)(int64_t)v;
}

static uint64_t dyn_num_zig(int64_t x)
{
    return ((uint64_t)x << 1) ^ (uint64_t)(x >> 63);
}

int dyn_num_write(dyn_ser_t* w, const double* v, size_t n)
{
    size_t i;
    uint8_t kind = DS_NUM_CONSTANT;

    if (dyn_ser_u32(w, (uint32_t)n) < 0)
        return -1;
    if (!n)
        return dyn_ser_u8(w, DS_NUM_CONSTANT);

    for (i = 1; i < n; i++)
        if (memcmp(&v[i], &v[0], sizeof(double)) != 0) {
            kind = DS_NUM_INT;
            break;
        }
    if (kind == DS_NUM_INT)
        for (i = 0; i < n; i++)
            if (!dyn_num_is_int(v[i])) {
                kind = DS_NUM_RAW;
                break;
            }

    if (kind == DS_NUM_INT) {
        size_t flat = 0, delta = 0;
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            int64_t x = (int64_t)v[i];
            flat += dyn_varint_len(dyn_num_zig(x));
            delta += dyn_varint_len(dyn_num_zig(x - prev));
            prev = x;
        }
        if (delta < flat)
            kind = DS_NUM_DELTA;
    }

    if (dyn_ser_u8(w, kind) < 0)
        return -1;
    if (kind == DS_NUM_CONSTANT)
        return dyn_ser_f64(w, v[0]);
    if (kind == DS_NUM_INT || kind == DS_NUM_DELTA) {
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            int64_t x = (int64_t)v[i];
            int64_t out = kind == DS_NUM_DELTA ? x - prev : x;
            prev = x;
            if (dyn_sk_uvarint(w, dyn_num_zig(out)) < 0)
                return -1;
        }
        return 0;
    }
    return dyn_ser_f64s(w, v, n);
}

int dyn_num_read_header(dyn_de_t* r, uint32_t* n, uint8_t* kind)
{
    uint32_t c = dyn_de_u32(r);
    uint8_t k;
    if (!dyn_de_ok(r) || c > DS_NUM_MAX)
        return -1;
    k = dyn_de_u8(r);
    if (!dyn_de_ok(r) || k > DS_NUM_DELTA)
        return -1;
    if (k == DS_NUM_CONSTANT && c > DYN_DECODE_AMPLIFIED_NUM_MAX)
        return -1;
    if ((k == DS_NUM_RAW && (uint64_t)c * 8 > (uint64_t)dyn_de_left(r)) || ((k == DS_NUM_INT || k == DS_NUM_DELTA) && (uint64_t)c > (uint64_t)dyn_de_left(r)))
        return -1;
    if (dyn_de_charge(r, (uint64_t)c * 8) < 0)
        return -1;
    *n = c;
    *kind = k;
    return 0;
}

int dyn_num_fill(dyn_de_t* r, uint8_t kind, double* v, uint32_t n)
{
    uint32_t i;
    if (kind == DS_NUM_CONSTANT) {
        double c;
        if (!n)
            return 0;
        c = dyn_de_f64(r);
        if (!dyn_de_ok(r))
            return -1;
        for (i = 0; i < n; i++)
            v[i] = c;
        return 0;
    }
    if (kind == DS_NUM_INT || kind == DS_NUM_DELTA) {
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            uint64_t z;
            int64_t x;
            if (dyn_sk_read_uvarint(r, &z) < 0)
                return -1;
            x = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
            if (kind == DS_NUM_DELTA)
                x += prev;
            prev = x;
            v[i] = (double)x;
        }
        return 0;
    }
    for (i = 0; i < n; i++)
        v[i] = dyn_de_f64(r);
    return dyn_de_ok(r) ? 0 : -1;
}

int dyn_sketch_write_u64(dyn_ser_t* w, const uint64_t* v, size_t n)
{
    size_t i, nz = 0, sparse = 0, prev = 0;

    for (i = 0; i < n; i++) {
        if (!v[i])
            continue;
        sparse += dyn_varint_len(i - prev) + dyn_varint_len(v[i]);
        prev = i;
        nz++;
    }
    sparse += dyn_varint_len(nz);
    if (n > DYN_DECODE_SKETCH_MAX_SPARSE || sparse >= n * 8) {
        if (dyn_ser_u8(w, DS_SKETCH_RAW) < 0)
            return -1;
        for (i = 0; i < n; i++)
            if (dyn_ser_u64(w, v[i]) < 0)
                return -1;
        return 0;
    }
    if (dyn_ser_u8(w, DS_SKETCH_ALT) < 0 || dyn_ser_uvarint(w, nz) < 0)
        return -1;
    prev = 0;
    for (i = 0; i < n; i++) {
        if (!v[i])
            continue;
        if (dyn_ser_uvarint(w, i - prev) < 0 || dyn_ser_uvarint(w, v[i]) < 0)
            return -1;
        prev = i;
    }
    return 0;
}

int dyn_sketch_fits_u64(const dyn_de_t* r, size_t n)
{
    size_t left = dyn_de_left(r);
    uint8_t kind;

    if (!left)
        return -1;
    kind = r->p[r->pos];
    if (kind > DS_SKETCH_ALT)
        return -1;
    if (kind == DS_SKETCH_RAW && (uint64_t)n * 8 > (uint64_t)(left - 1))
        return -1;
    if (kind == DS_SKETCH_ALT && n > DYN_DECODE_SKETCH_MAX_SPARSE)
        return -1;
    return 0;
}

int dyn_sketch_read_u64(dyn_de_t* r, uint64_t* v, size_t n)
{
    uint8_t kind = dyn_de_u8(r);
    uint64_t nz, k, at = 0;
    size_t i;

    if (!dyn_de_ok(r) || kind > DS_SKETCH_ALT)
        return -1;
    if (kind == DS_SKETCH_RAW) {
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return -1;
        for (i = 0; i < n; i++)
            v[i] = dyn_de_u64(r);
        return dyn_de_ok(r) ? 0 : -1;
    }
    memset(v, 0, n * sizeof(*v));
    if (dyn_de_uvarint(r, &nz) < 0 || nz > (uint64_t)n)
        return -1;
    for (k = 0; k < nz; k++) {
        uint64_t d, val;
        if (dyn_de_uvarint(r, &d) < 0 || dyn_de_uvarint(r, &val) < 0)
            return -1;
        at += d;
        if (at >= (uint64_t)n)
            return -1;
        v[at] = val;
    }
    return 0;
}

double* dyn_num_read_pairs(dyn_de_t* r, uint32_t first, uint32_t per,
    uint32_t* count, uint32_t* elems)
{
    uint32_t n, e;
    uint8_t kind;
    double* v;

    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &e, &kind) < 0 || (per > 1 && (e % per)))
            return NULL;
        n = e / per;
    } else {
        uint64_t e64 = (uint64_t)first * per;
        n = first;
        kind = DS_NUM_RAW;
        if (e64 > UINT32_MAX || e64 * 8 > (uint64_t)dyn_de_left(r))
            return NULL;
        e = (uint32_t)e64;
    }
    v = (double*)dyn_ds_calloc(e ? e : 1, sizeof(double));
    if (!v)
        return NULL;
    if (dyn_num_fill(r, kind, v, e) < 0) {
        dyn_ds_free(v);
        return NULL;
    }
    *count = n;
    *elems = e;
    return v;
}

JSValue dyn_ds_read_pushed(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int cap_arg,
    JSValueConst ctor_arg)
{
    uint32_t cap = 0, n, i;
    JSValue obj, vals, arg;

    if (cap_arg) {
        cap = dyn_de_u32(r);
        if (!dyn_de_ok(r) || cap == 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (dyn_de_count(r, &n, 0) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (cap_arg && n > cap)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    vals = dyn_ds_read_values(ctx, r, n);
    if (JS_IsException(vals))
        return vals;
    arg = cap_arg ? JS_NewInt64(ctx, (int64_t)cap) : JS_UNDEFINED;
    if (!JS_IsUndefined(ctor_arg))
        obj = dyn_ds_construct(ctx, id, 1, &ctor_arg);
    else
        obj = dyn_ds_construct(ctx, id, cap_arg ? 1 : 0,
            (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        return obj;
    }
    {
        JSAtom ma = JS_NewAtom(ctx, method);
        if (ma == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++) {
            JSValue v = dyn_ds_value_at(ctx, vals, i);
            int bad = dyn_ds_invoke_atom(ctx, obj, ma, 1, (JSValueConst*)&v) < 0;
            JS_FreeValue(ctx, v);
            if (bad) {
                dyn_ds_rebuild_threw = 1;
                JS_FreeAtom(ctx, ma);
                JS_FreeValue(ctx, obj);
                JS_FreeValue(ctx, vals);
                return JS_EXCEPTION;
            }
        }
        JS_FreeAtom(ctx, ma);
    }
    JS_FreeValue(ctx, vals);
    return obj;
}

JSValue ds2_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method)
{
    JSValue fn, arr, values, iter;

    fn = JS_GetPropertyStr(ctx, this_val, method);
    if (JS_IsException(fn))
        return fn;
    arr = JS_Call(ctx, fn, this_val, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return arr;
    values = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, arr);
        return values;
    }
    iter = JS_Call(ctx, values, arr, 0, NULL);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, arr);
    return iter;
}

int ds2_write_cells(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, ds2_cell_iter next)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (JS_IsException(arr))
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* c = next(native, i);
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, ds_cell_val(c)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue ds2_read_cells(JSContext* ctx, dyn_de_t* r, uint32_t n)
{
    JSValue arr = dyn_codec_read_values(ctx, r);
    uint32_t got = 0;
    JSValue len;

    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &got, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (got != n) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "malformed DYNS record: %u values for %u entries",
            (unsigned)got, (unsigned)n);
    }
    return arr;
}

_Atomic JSClassID sdb_ta_cid[JS_TYPED_ARRAY_FLOAT64 + 1];

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

int sdb_typed_kind_pub(JSValueConst v)
{
    return sdb_typed_kind(v);
}

const uint8_t* sdb_typed_bytes_pub(JSContext* ctx, JSValueConst v, size_t* plen)
{
    return sdb_typed_bytes(ctx, v, plen);
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

int sdb_f64_list(JSContext* ctx, JSValueConst v, const char* what,
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

int sdb_u32_list(JSContext* ctx, JSValueConst v, const char* what,
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

int sdb_opt_u32(JSContext* ctx, JSValueConst opts, const char* name, uint32_t* out)
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

JSValue sdb_typed_out(JSContext* ctx, const void* data, size_t n, JSTypedArrayEnum t)
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

JSValue sdb_pair(JSContext* ctx, JSValue a, JSValue b)
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

int sdb_index_arg(JSContext* ctx, JSValueConst v, const char* what, int64_t* out)
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

JSValue dsx_typed_copy(JSContext* ctx, const void* data, size_t count, size_t esz, JSTypedArrayEnum type)
{
    JSValue buf, args[3], out;
    buf = JS_NewArrayBufferCopy(ctx, (const uint8_t*)data, count * esz);
    if (JS_IsException(buf))
        return buf;
    args[0] = buf;
    args[1] = JS_NewInt32(ctx, 0);
    args[2] = JS_NewInt64(ctx, (int64_t)count);
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, buf);
    return out;
}

int dsx_key_bytes(JSContext* ctx, JSValueConst v, int bytes_mode, dsx_bytes* out)
{
    out->cstr = NULL;
    out->buf = JS_UNDEFINED;
    if (bytes_mode) {
        size_t off, len, bpe;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        size_t ablen;
        if (JS_IsException(ab))
            return -1;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx, "expected a Uint8Array");
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ablen, ab);
        if (!base && len) {
            JS_FreeValue(ctx, ab);
            return -1;
        }
        out->p = base ? base + off : (const uint8_t*)"";
        out->n = len;
        out->buf = ab;
        return 0;
    }
    out->cstr = JS_ToCStringLen(ctx, &out->n, v);
    if (!out->cstr)
        return -1;
    out->p = (const uint8_t*)out->cstr;
    return 0;
}

JSValue dsx_flat_read(JSContext* ctx, dyn_de_t* r, uint32_t group, uint32_t* n)
{
    JSValue arr = dyn_codec_read_values(ctx, r), len;
    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, n, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (*n % group) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: %u values do not form groups of %u", *n, group);
    }
    return arr;
}

JSValue dsx_replay(JSContext* ctx, JSValue obj, JSValue arr, uint32_t n, uint32_t group, dsx_method_fn fn)
{
    uint32_t i, j;
    for (i = 0; i < n; i += group) {
        JSValue args[3], r;
        int vex = 0;
        for (j = 0; j < group; j++) {
            JSValue a = JS_GetPropertyUint32(ctx, arr, i + j);
            vex |= JS_IsException(a);
            args[j] = a;
        }
        r = vex ? JS_EXCEPTION : fn(ctx, obj, (int)group, (JSValueConst*)args);
        for (j = 0; j < group; j++)
            JS_FreeValue(ctx, args[j]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arr);
    return obj;
}

int dsx_opt_keys_ok(JSContext* ctx, JSValueConst obj, const char* allowed, const char* cls, const char* what)
{
    JSPropertyEnum* props;
    uint32_t n, i;
    int ok = 1;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return 0;
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, props[i].atom);
        if (ok && (!k || strcmp(k, allowed) != 0)) {
            if (k)
                JS_ThrowTypeError(ctx, "%s: unknown %s option \"%s\" (known: %s)", cls, what, k, allowed);
            ok = 0;
        }
        JS_FreeCString(ctx, k);
    }
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return ok;
}

int dsx_cache_opt(JSContext* ctx, JSValueConst opts, const char* cls, const char* feature, int* on)
{
    JSValue c, v;
    int r;
    *on = 0;
    if (!JS_IsObject(opts))
        return 0;
    c = JS_GetPropertyStr(ctx, opts, "cache");
    if (JS_IsException(c))
        return -1;
    if (JS_IsUndefined(c) || JS_IsBool(c)) {
        *on = JS_ToBool(ctx, c);
        return 0;
    }
    if (!JS_IsObject(c)) {
        JS_FreeValue(ctx, c);
        JS_ThrowTypeError(ctx, "%s: cache must be a boolean or { %s?: boolean }", cls, feature);
        return -1;
    }
    if (!dsx_opt_keys_ok(ctx, c, feature, cls, "cache")) {
        JS_FreeValue(ctx, c);
        return -1;
    }
    v = JS_GetPropertyStr(ctx, c, feature);
    JS_FreeValue(ctx, c);
    if (JS_IsException(v))
        return -1;
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    *on = r;
    return 0;
}

#endif
