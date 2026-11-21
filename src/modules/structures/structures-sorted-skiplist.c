#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int failed;
} skip_keys_state;

#define DS_SKIP_EXT 0xFFFFFFFFu
#define DS_SK_RAW 0
#define DS_SK_ARITHMETIC 1
#define DS_SK_DELTA 2
#define DS_SK_MAX_ARITH (1u << 22)
#define DS_SK_ARITH_KEY_COST 128u

dsw_skip_t* skip_new(JSRuntime* rt)
{
    dsw_skip_t* s = (dsw_skip_t*)dyn_ds_malloc(sizeof(*s));
    if (!s)
        return NULL;
    s->b = dyn_btree_new();
    if (!s->b) {
        dyn_ds_free(s);
        return NULL;
    }
    s->rt = rt;
    return s;
}

void skip_free(void* native)
{
    dsw_skip_t* s = (dsw_skip_t*)native;
    if (!s)
        return;
    dyn_btree_free(s->b, ds_cell_release, s->rt);
    dyn_ds_free(s);
}

int skip_insert(JSContext* ctx, dsw_skip_t* s, double key,
    JSValueConst val)
{
    JSValue dup = JS_DupValue(ctx, val);
    dyn_cell_t cell = ds_val_cell(dup), old;
    int replaced = 0;

    if (dyn_btree_set(s->b, key, &cell, &old, &replaced) < 0) {
        JS_FreeValue(ctx, dup);
        return -1;
    }
    if (replaced)
        ds_cell_release(s->rt, &old);
    return 0;
}

int skip_delete(dsw_skip_t* s, double key)
{
    dyn_cell_t out;
    if (!dyn_btree_del(s->b, key, &out))
        return 0;
    ds_cell_release(s->rt, &out);
    return 1;
}

int skip_floor(dsw_skip_t* s, double q, double* out)
{
    return dyn_btree_floor(s->b, q, out);
}

int skip_ceil(dsw_skip_t* s, double q, double* out)
{
    return dyn_btree_ceil(s->b, q, out);
}

JSValue skip_end_or_undef(JSContext* ctx, dsw_skip_t* s, int last)
{
    double k;
    int ok = last ? dyn_btree_last(s->b, &k) : dyn_btree_first(s->b, &k);
    return ok ? JS_NewFloat64(ctx, k) : JS_UNDEFINED;
}

static int skip_keys_leaf(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n)
{
    skip_keys_state* st = (skip_keys_state*)ud;
    uint32_t j;
    (void)vals;
    for (j = 0; j < n; j++) {
        if (JS_DefinePropertyValueUint32(st->ctx, st->arr, st->i++,
                JS_NewFloat64(st->ctx, keys[j]), JS_PROP_C_W_E)
            < 0) {
            st->failed = 1;
            return 0;
        }
    }
    return 1;
}

JSValue skip_keys_array(JSContext* ctx, dsw_skip_t* s)
{
    skip_keys_state st;
    st.ctx = ctx;
    st.i = 0;
    st.failed = 0;
    st.arr = JS_NewArray(ctx);
    if (JS_IsException(st.arr))
        return st.arr;
    dyn_btree_walk_leaves(s->b, skip_keys_leaf, &st);
    if (st.failed) {
        JS_FreeValue(ctx, st.arr);
        return JS_EXCEPTION;
    }
    return st.arr;
}

int skip_pairs_leaf(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n)
{
    skip_pairs_state* st = (skip_pairs_state*)ud;
    uint32_t j;
    for (j = 0; j < n; j++) {
        JSValue pair = ds2_tuple2(st->ctx, JS_NewFloat64(st->ctx, keys[j]),
            JS_DupValue(st->ctx, ds_cell_val(&vals[j])));
        if (JS_IsException(pair)) {
            st->failed = 1;
            return 0;
        }
        if (JS_DefinePropertyValueUint32(st->ctx, st->arr, st->i++, pair,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(st->ctx, st->arr);
            st->failed = 2;
            return 0;
        }
    }
    return 1;
}

static int dyn_sk_is_int(double v)
{
    return v == v && v >= -9007199254740992.0 && v <= 9007199254740992.0 && v == (double)(int64_t)v;
}

int dsw_skip_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj,
    JSClassID id, int with_values)
{
    dsw_skip_t* s = JS_GetOpaque(obj, id);
    JSValue arr;
    uint32_t n, i;
    double* keys;
    int r;

    if (!s)
        return -1;
    n = dyn_btree_size(s->b);
    keys = (double*)dyn_ds_malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!keys)
        return -1;
    dyn_btree_get_keys(s->b, keys);

    {
        int all_int = 1, arithmetic = 1;
        double step = 0;
        for (i = 0; i < n; i++) {
            if (!dyn_sk_is_int(keys[i])) {
                all_int = 0;
                arithmetic = 0;
                break;
            }
            if (i == 1)
                step = keys[1] - keys[0];
            else if (i > 1 && keys[i] - keys[i - 1] != step)
                arithmetic = 0;
        }
        if (all_int && n >= 2 && arithmetic) {
            if (dyn_ser_u32(w, DS_SKIP_EXT) < 0 || dyn_ser_u8(w, DS_SK_ARITHMETIC) < 0 || dyn_ser_u32(w, n) < 0 || dyn_ser_f64(w, keys[0]) < 0 || dyn_ser_f64(w, step) < 0) {
                dyn_ds_free(keys);
                return -1;
            }
        } else if (all_int && n >= 1) {
            if (dyn_ser_u32(w, DS_SKIP_EXT) < 0 || dyn_ser_u8(w, DS_SK_DELTA) < 0 || dyn_ser_u32(w, n) < 0) {
                dyn_ds_free(keys);
                return -1;
            }
            for (i = 0; i < n; i++) {
                if (i == 0) {
                    if (dyn_ser_f64(w, keys[0]) < 0) {
                        dyn_ds_free(keys);
                        return -1;
                    }
                } else {
                    double d = keys[i] - keys[i - 1];
                    if (d < 0) {
                        dyn_ds_free(keys);
                        return -1;
                    }
                    if (dyn_sk_uvarint(w, (uint64_t)d) < 0) {
                        dyn_ds_free(keys);
                        return -1;
                    }
                }
            }
        } else {
            if (dyn_ser_u32(w, DS_SKIP_EXT) < 0 || dyn_ser_u8(w, DS_SK_RAW) < 0 || dyn_ser_u32(w, n) < 0) {
                dyn_ds_free(keys);
                return -1;
            }
            if (dyn_ser_f64s(w, keys, n) < 0) {
                dyn_ds_free(keys);
                return -1;
            }
        }
    }
    dyn_ds_free(keys);
    if (!with_values)
        return 0;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return -1;
    i = 0;
    {
        dyn_btree_iter it;
        if (dyn_btree_iter_begin(s->b, &it)) {
            double k;
            const dyn_cell_t* v;
            do {
                if (!dyn_btree_iter_get(&it, &k, &v))
                    break;
                if (JS_DefinePropertyValueUint32(ctx, arr, i++,
                        JS_DupValue(ctx, ds_cell_val(v)),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } while (dyn_btree_iter_next(&it));
        }
    }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue dsw_skip_read(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int with_values)
{
    uint32_t n, i;
    JSValue obj, vals = JS_UNDEFINED;
    double* keys;
    JSAtom ma;

    {
        uint32_t first = dyn_de_u32(r);
        uint32_t kind = DS_SK_RAW;
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        if (first == DS_SKIP_EXT) {
            kind = dyn_de_u8(r);
            if (!dyn_de_ok(r) || kind > DS_SK_DELTA)
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            if (kind == DS_SK_ARITHMETIC) {
                n = dyn_de_u32(r);
                if (!dyn_de_ok(r) || n > DS_SK_MAX_ARITH)
                    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
                if (dyn_de_charge(r, (uint64_t)n * DS_SK_ARITH_KEY_COST) < 0)
                    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            } else if (dyn_de_count(r, &n, kind == DS_SK_RAW ? 8 : 0) < 0) {
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
        } else {
            n = first;
            if ((size_t)n * 8 > dyn_de_left(r))
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        keys = dyn_ds_calloc(n ? n : 1, sizeof(double));
        if (!keys)
            return JS_ThrowOutOfMemory(ctx);
        switch (kind) {
        case DS_SK_ARITHMETIC: {
            double k0 = dyn_de_f64(r), step = dyn_de_f64(r);
            if (!dyn_de_ok(r)) {
                dyn_ds_free(keys);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            if (n)
                keys[0] = k0;
            for (i = 1; i < n; i++)
                keys[i] = k0 + (double)i * step;
            break;
        }
        case DS_SK_DELTA: {
            double prev = 0;
            for (i = 0; i < n; i++) {
                if (i == 0) {
                    prev = dyn_de_f64(r);
                } else {
                    uint64_t d;
                    if (dyn_sk_read_uvarint(r, &d) < 0) {
                        dyn_ds_free(keys);
                        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
                    }
                    prev += (double)d;
                }
                keys[i] = prev;
            }
            break;
        }
        default:
            for (i = 0; i < n; i++)
                keys[i] = dyn_de_f64(r);
            break;
        }
    }
    if (!dyn_de_ok(r)) {
        dyn_ds_free(keys);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (with_values) {
        vals = dyn_ds_read_values(ctx, r, n);
        if (JS_IsException(vals)) {
            dyn_ds_free(keys);
            return vals;
        }
    }
    obj = dyn_ds_construct(ctx, id, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(keys);
        return obj;
    }
    ma = JS_NewAtom(ctx, method);
    for (i = 0; i < n; i++) {
        JSValue args[2];
        int bad;
        args[0] = JS_NewFloat64(ctx, keys[i]);
        args[1] = with_values ? dyn_ds_value_at(ctx, vals, i)
                              : JS_UNDEFINED;
        bad = ma == JS_ATOM_NULL
            || dyn_ds_invoke_atom(ctx, obj, ma, with_values ? 2 : 1,
                   (JSValueConst*)args)
                < 0;
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (bad) {
            JS_FreeAtom(ctx, ma);
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(keys);
            return JS_EXCEPTION;
        }
    }
    JS_FreeAtom(ctx, ma);
    JS_FreeValue(ctx, vals);
    dyn_ds_free(keys);
    return obj;
}

#endif
