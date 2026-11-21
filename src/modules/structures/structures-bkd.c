#include "structures-internal.h"
#include "core/ds/ds-bkd.h"
#include "core/ds/ds-roaring-bitmap.h"
#include "core/dyn-netipk.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

enum { SDB_BKD_F64,
    SDB_BKD_I64,
    SDB_BKD_IP,
    SDB_BKD_LATLON,
    SDB_BKD_NTYPES };

static const char* const sdb_bkd_type_names[SDB_BKD_NTYPES] = { "f64", "i64", "ip", "latlon" };
static const uint32_t sdb_bkd_bpd[SDB_BKD_NTYPES] = { 8, 8, 16, 8 };

typedef struct {
    dyn_bkd_t t;
    int type;
} sdb_bkd_w;

typedef struct {
    int type;
    uint32_t dims, leaf, n;
    uint8_t* keys;
    uint32_t* docs;
    sdb_bkd_w* w;
    int rc;
} sdb_bkd_job;

JSClassID sdb_bkd_class_id;

static void sdb_bkd_free(void* native)
{
    sdb_bkd_w* w = (sdb_bkd_w*)native;
    if (!w)
        return;
    dyn_bkd_free(&w->t);
    dyn_ds_free(w);
}

static void sdb_bkd_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_bkd_free(JS_GetOpaque(val, sdb_bkd_class_id));
}

const JSClassDef sdb_bkd_class = {
    "BKDTree",
    .finalizer = sdb_bkd_finalizer,
};

static int sdb_bkd_ip(JSContext* ctx, JSValueConst v, uint8_t* out, const char* what)
{
    NetAddr a;
    size_t n;
    const char* s;
    int bad;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "BKDTree: %s must be an IPv4 or IPv6 address string", what);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    bad = netipk_parse_addr(s, n, &a);
    JS_FreeCString(ctx, s);
    if (bad) {
        JS_ThrowRangeError(ctx, "BKDTree: %s is not an IPv4 or IPv6 address", what);
        return -1;
    }
    if (a.is4) {
        memset(out, 0, 10);
        out[10] = out[11] = 0xFF;
        memcpy(out + 12, a.b, 4);
    } else
        memcpy(out, a.b, 16);
    return 0;
}

static int sdb_bkd_i64(JSContext* ctx, JSValueConst v, int64_t* out, int bound, const char* what)
{
    if (JS_IsBigInt(ctx, v)) {
        JSValue back;
        int same;
        if (JS_ToBigInt64(ctx, out, v))
            return -1;
        back = JS_NewBigInt64(ctx, *out);
        if (JS_IsException(back))
            return -1;
        same = JS_StrictEq(ctx, back, v);
        JS_FreeValue(ctx, back);
        if (!same) {
            JS_ThrowRangeError(ctx, "BKDTree: %s is outside the int64 range", what);
            return -1;
        }
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        if (bound && isinf(d)) {
            *out = d < 0 ? INT64_MIN : INT64_MAX;
            return 0;
        }
        if (d != floor(d) || !(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
            JS_ThrowRangeError(ctx, "BKDTree: %s must be an integer in the int64 range", what);
            return -1;
        }
        *out = (int64_t)d;
        return 0;
    }
    JS_ThrowTypeError(ctx, "BKDTree: %s must be a bigint or an integer number", what);
    return -1;
}

static int sdb_bkd_scalar(JSContext* ctx, int type, JSValueConst v, uint8_t* out, int bound, const char* what)
{
    double d;
    int64_t x;
    switch (type) {
    case SDB_BKD_I64:
        if (sdb_bkd_i64(ctx, v, &x, bound, what))
            return -1;
        dyn_bkd_enc_i64(x, out);
        return 0;
    case SDB_BKD_IP:
        return sdb_bkd_ip(ctx, v, out, what);
    default:
        if (!JS_IsNumber(v)) {
            JS_ThrowTypeError(ctx, "BKDTree: %s must be a number", what);
            return -1;
        }
        JS_ToFloat64(ctx, &d, v);
        dyn_bkd_enc_f64(d, out);
        return 0;
    }
}

static int sdb_bkd_latlon_ok(JSContext* ctx, const uint8_t* key, const char* what)
{
    double lat = dyn_bkd_dec_f64(key), lon = dyn_bkd_dec_f64(key + 8);
    if (!(lat >= -90 && lat <= 90) || !(lon >= -180 && lon <= 180)) {
        JS_ThrowRangeError(ctx, "BKDTree: %s needs latitude in [-90, 90] and longitude in [-180, 180]", what);
        return -1;
    }
    return 0;
}

static int sdb_bkd_len(JSContext* ctx, JSValueConst v, uint32_t* n)
{
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    int r;
    if (JS_IsException(lv))
        return -1;
    r = JS_ToUint32(ctx, n, lv);
    JS_FreeValue(ctx, lv);
    return r;
}

static int sdb_bkd_points(JSContext* ctx, int type, JSValueConst v, uint32_t dims_opt, uint8_t** pkeys, uint32_t* pn, uint32_t* pdims)
{
    uint32_t bpd = sdb_bkd_bpd[type], dims = dims_opt, n = 0, m, i, j, kb;
    int kind = sdb_typed_kind_pub(v), rows = 0;
    uint8_t* keys = NULL;
    *pkeys = NULL;
    if (type == SDB_BKD_LATLON && dims && dims != 2) {
        JS_ThrowRangeError(ctx, "BKDTree: latlon points have 2 dimensions");
        return -1;
    }
    if (type == SDB_BKD_IP && dims && dims != 1) {
        JS_ThrowRangeError(ctx, "BKDTree: ip points have 1 dimension");
        return -1;
    }
    if (type == SDB_BKD_I64 && kind == JS_TYPED_ARRAY_BIG_INT64) {
        size_t len;
        const uint8_t* p = sdb_typed_bytes_pub(ctx, v, &len);
        if (!p)
            return -1;
        if (!dims)
            dims = 1;
        m = (uint32_t)(len / 8 > DYN_DS_MAX_ITEMS ? DYN_DS_MAX_ITEMS + 1u : len / 8);
        if (m > DYN_DS_MAX_ITEMS || m % dims) {
            JS_ThrowRangeError(ctx, "BKDTree: %u values is not a multiple of %u dims or is too many", (unsigned)m, (unsigned)dims);
            return -1;
        }
        keys = (uint8_t*)dyn_ds_malloc((size_t)m * bpd + 1);
        if (!keys) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < m; i++) {
            int64_t x;
            memcpy(&x, p + (size_t)i * 8, 8);
            dyn_bkd_enc_i64(x, keys + (size_t)i * 8);
        }
        *pkeys = keys;
        *pn = m / dims;
        *pdims = dims;
        return 0;
    }
    if ((type == SDB_BKD_F64 || type == SDB_BKD_LATLON) && kind >= 0) {
        double* d;
        if (sdb_f64_list(ctx, v, "BKDTree values", &d, &m))
            return -1;
        if (!dims)
            dims = type == SDB_BKD_LATLON ? 2 : 1;
        if (m % dims) {
            dyn_ds_free(d);
            JS_ThrowRangeError(ctx, "BKDTree: %u values is not a multiple of %u dims", (unsigned)m, (unsigned)dims);
            return -1;
        }
        keys = (uint8_t*)dyn_ds_malloc((size_t)m * bpd + 1);
        if (!keys) {
            dyn_ds_free(d);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < m; i++)
            dyn_bkd_enc_f64(d[i], keys + (size_t)i * 8);
        dyn_ds_free(d);
        n = m / dims;
        if (type == SDB_BKD_LATLON)
            for (i = 0; i < n; i++)
                if (sdb_bkd_latlon_ok(ctx, keys + (size_t)i * 16, "a point")) {
                    dyn_ds_free(keys);
                    return -1;
                }
        *pkeys = keys;
        *pn = n;
        *pdims = dims;
        return 0;
    }
    if (kind >= 0 || JS_IsArray(ctx, v) <= 0) {
        JS_ThrowTypeError(ctx, "BKDTree: values must be an array%s", type == SDB_BKD_I64 ? ", a BigInt64Array" : type == SDB_BKD_IP ? " of strings" : " or a typed array");
        return -1;
    }
    if (sdb_bkd_len(ctx, v, &m))
        return -1;
    if (m > DYN_DS_MAX_ITEMS) {
        JS_ThrowRangeError(ctx, "BKDTree: more than %u values", DYN_DS_MAX_ITEMS);
        return -1;
    }
    if (m) {
        JSValue e = JS_GetPropertyUint32(ctx, v, 0);
        if (JS_IsException(e))
            return -1;
        rows = JS_IsArray(ctx, e) > 0 || sdb_typed_kind_pub(e) >= 0;
        if (rows) {
            uint32_t rl;
            if (sdb_bkd_len(ctx, e, &rl)) {
                JS_FreeValue(ctx, e);
                return -1;
            }
            if (!dims)
                dims = rl;
        }
        JS_FreeValue(ctx, e);
    }
    if (!dims)
        dims = type == SDB_BKD_LATLON ? 2 : 1;
    if (dims < 1 || dims > DYN_BKD_MAX_DIMS || (type == SDB_BKD_LATLON && dims != 2) || (type == SDB_BKD_IP && dims != 1)) {
        JS_ThrowRangeError(ctx, "BKDTree: points need %s dimensions", type == SDB_BKD_LATLON ? "2" : type == SDB_BKD_IP ? "1" : "1..4");
        return -1;
    }
    n = rows ? m : m / dims;
    if (!rows && m % dims) {
        JS_ThrowRangeError(ctx, "BKDTree: %u values is not a multiple of %u dims", (unsigned)m, (unsigned)dims);
        return -1;
    }
    if ((uint64_t)n * dims > DYN_DS_MAX_ITEMS) {
        JS_ThrowRangeError(ctx, "BKDTree: more than %u coordinates", DYN_DS_MAX_ITEMS);
        return -1;
    }
    kb = dims * bpd;
    keys = (uint8_t*)dyn_ds_malloc((size_t)n * kb + 1);
    if (!keys) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue row = JS_UNDEFINED;
        if (rows) {
            uint32_t rl;
            row = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(row))
                goto fail;
            if ((JS_IsArray(ctx, row) <= 0 && sdb_typed_kind_pub(row) < 0) || sdb_bkd_len(ctx, row, &rl) || rl != dims) {
                JS_FreeValue(ctx, row);
                if (!JS_HasException(ctx))
                    JS_ThrowRangeError(ctx, "BKDTree: point %u must be an array of %u coordinates", (unsigned)i, (unsigned)dims);
                goto fail;
            }
        }
        for (j = 0; j < dims; j++) {
            JSValue e = rows ? JS_GetPropertyUint32(ctx, row, j) : JS_GetPropertyUint32(ctx, v, i * dims + j);
            int bad;
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            bad = sdb_bkd_scalar(ctx, type, e, keys + (size_t)i * kb + (size_t)j * bpd, 0, "a value");
            JS_FreeValue(ctx, e);
            if (bad) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
        }
        JS_FreeValue(ctx, row);
        if (type == SDB_BKD_LATLON && sdb_bkd_latlon_ok(ctx, keys + (size_t)i * kb, "a point"))
            goto fail;
    }
    *pkeys = keys;
    *pn = n;
    *pdims = dims;
    return 0;
fail:
    dyn_ds_free(keys);
    return -1;
}

static int sdb_bkd_opts(JSContext* ctx, JSValueConst opts, int* type, uint32_t* dims, uint32_t* leaf, JSValue* docs)
{
    JSValue v;
    *type = SDB_BKD_F64;
    *dims = 0;
    *leaf = DYN_BKD_DEFAULT_LEAF;
    *docs = JS_UNDEFINED;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "BKDTree: options must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, opts, "type");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        int i, found = -1;
        JS_FreeValue(ctx, v);
        if (s) {
            for (i = 0; i < SDB_BKD_NTYPES; i++)
                if (!strcmp(s, sdb_bkd_type_names[i]))
                    found = i;
            JS_FreeCString(ctx, s);
        }
        if (found < 0) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "BKDTree: type must be \"f64\", \"i64\", \"ip\" or \"latlon\"");
            return -1;
        }
        *type = found;
    }
    v = JS_GetPropertyStr(ctx, opts, "dims");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int bad = dyn_size_arg(ctx, v, DYN_BKD_MAX_DIMS, dims);
        JS_FreeValue(ctx, v);
        if (bad)
            return -1;
        if (*dims < 1) {
            JS_ThrowRangeError(ctx, "BKDTree: dims must be 1..%d", DYN_BKD_MAX_DIMS);
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, opts, "leafSize");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int bad = dyn_size_arg(ctx, v, DYN_BKD_MAX_LEAF, leaf);
        JS_FreeValue(ctx, v);
        if (bad)
            return -1;
        if (*leaf < 2) {
            JS_ThrowRangeError(ctx, "BKDTree: leafSize must be 2..%d", DYN_BKD_MAX_LEAF);
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, opts, "docIds");
    if (JS_IsException(v))
        return -1;
    *docs = v;
    return 0;
}

static int sdb_bkd_prepare(JSContext* ctx, int argc, JSValueConst* argv, sdb_bkd_job* j)
{
    JSValue docv;
    uint32_t dims_opt, nd, i;
    memset(j, 0, sizeof(*j));
    if (sdb_bkd_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &j->type, &dims_opt, &j->leaf, &docv))
        return -1;
    if (sdb_bkd_points(ctx, j->type, argc > 0 ? argv[0] : JS_UNDEFINED, dims_opt, &j->keys, &j->n, &j->dims)) {
        JS_FreeValue(ctx, docv);
        return -1;
    }
    if (JS_IsUndefined(docv)) {
        j->docs = (uint32_t*)dyn_ds_malloc((size_t)j->n * 4 + 4);
        if (!j->docs) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < j->n; i++)
            j->docs[i] = i;
        return 0;
    }
    i = (uint32_t)sdb_u32_list(ctx, docv, "BKDTree docIds", &j->docs, &nd);
    JS_FreeValue(ctx, docv);
    if (i)
        return -1;
    if (nd != j->n) {
        JS_ThrowRangeError(ctx, "BKDTree: %u docIds for %u points", (unsigned)nd, (unsigned)j->n);
        return -1;
    }
    return 0;
}

static void sdb_bkd_job_clear(sdb_bkd_job* j)
{
    dyn_ds_free(j->keys);
    dyn_ds_free(j->docs);
    j->keys = NULL;
    j->docs = NULL;
    if (j->w)
        sdb_bkd_free(j->w);
    j->w = NULL;
}

static int sdb_bkd_run(sdb_bkd_job* j)
{
    j->w = (sdb_bkd_w*)dyn_ds_calloc(1, sizeof(*j->w));
    if (!j->w)
        return -1;
    j->w->type = j->type;
    return dyn_bkd_build(&j->w->t, j->dims, sdb_bkd_bpd[j->type], j->leaf, j->keys, j->docs, j->n);
}

static JSValue sdb_bkd_finish(JSContext* ctx, JSValueConst new_target, sdb_bkd_job* j)
{
    sdb_bkd_w* w;
    dyn_ds_free(j->keys);
    dyn_ds_free(j->docs);
    j->keys = NULL;
    j->docs = NULL;
    if (j->rc < 0) {
        sdb_bkd_job_clear(j);
        return ds2_oom(ctx);
    }
    w = j->w;
    j->w = NULL;
    return dyn_plain_wrap(ctx, new_target, sdb_bkd_class_id, w, sdb_bkd_free);
}

JSValue sdb_bkd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_bkd_job j;
    if (sdb_bkd_prepare(ctx, argc, argv, &j)) {
        sdb_bkd_job_clear(&j);
        return JS_EXCEPTION;
    }
    j.rc = sdb_bkd_run(&j);
    return sdb_bkd_finish(ctx, new_target, &j);
}

static JSValue sdb_bkd_build_entry(JSContext* ctx, void* arg)
{
    sdb_bkd_job* j = (sdb_bkd_job*)arg;
    JSValue r;
    j->rc = DYN_OFFLOAD_CPU(sdb_bkd_run(j));
    r = sdb_bkd_finish(ctx, JS_UNDEFINED, j);
    dyn_ds_free(j);
    return r;
}

JSValue sdb_bkd_build_static(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdb_bkd_job* j = (sdb_bkd_job*)dyn_ds_calloc(1, sizeof(*j));
    JSValue promise, funcs[2], e, r;
    (void)this_val;
    if (!j)
        return ds2_oom(ctx);
    if (!sdb_bkd_prepare(ctx, argc, argv, j)) {
        promise = dyn_fiber_start(ctx, sdb_bkd_build_entry, j);
        if (JS_IsException(promise)) {
            sdb_bkd_job_clear(j);
            dyn_ds_free(j);
        }
        return promise;
    }
    sdb_bkd_job_clear(j);
    dyn_ds_free(j);
    e = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, e);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static int sdb_bkd_bound(JSContext* ctx, const sdb_bkd_w* w, JSValueConst v, uint8_t* out, const char* what)
{
    uint32_t d = w->t.dims, bpd = sdb_bkd_bpd[w->type], n, i;
    int arr = JS_IsArray(ctx, v) > 0 || sdb_typed_kind_pub(v) >= 0;
    if (!arr) {
        if (d != 1) {
            JS_ThrowTypeError(ctx, "BKDTree: %s must be an array of %u values", what, (unsigned)d);
            return -1;
        }
        return sdb_bkd_scalar(ctx, w->type, v, out, 1, what);
    }
    if (sdb_bkd_len(ctx, v, &n))
        return -1;
    if (n != d) {
        JS_ThrowRangeError(ctx, "BKDTree: %s has %u values, expected %u", what, (unsigned)n, (unsigned)d);
        return -1;
    }
    for (i = 0; i < d; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        int bad;
        if (JS_IsException(e))
            return -1;
        bad = sdb_bkd_scalar(ctx, w->type, e, out + (size_t)i * bpd, 1, what);
        JS_FreeValue(ctx, e);
        if (bad)
            return -1;
    }
    return 0;
}

static JSValue sdb_bkd_collect(JSContext* ctx, const sdb_bkd_w* w, dyn_bkd_rel_fn rel, dyn_bkd_hit_fn hit, const void* q, int count)
{
    dyn_bkd_sink s = { NULL, 0, 0 };
    dyn_roar_t r;
    memset(&r, 0, sizeof(r));
    if (dyn_bkd_query(&w->t, rel, hit, q, &s) < 0) {
        dyn_bkd_sink_free(&s);
        return ds2_oom(ctx);
    }
    if (count) {
        size_t n = s.n;
        dyn_bkd_sink_free(&s);
        return JS_NewInt64(ctx, (int64_t)n);
    }
    if (dyn_bkd_sink_to_roar(&s, &r) < 0) {
        dyn_bkd_sink_free(&s);
        dyn_roar_clear(&r);
        return ds2_oom(ctx);
    }
    dyn_bkd_sink_free(&s);
    return sdb_rb_wrap_roar(ctx, &r);
}

static JSValue sdb_bkd_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdb_bkd_w* w = (sdb_bkd_w*)dyn_plain_get(ctx, this_val, sdb_bkd_class_id);
    dyn_bkd_range_q q;
    if (!w)
        return JS_EXCEPTION;
    memset(&q, 0, sizeof(q));
    q.dims = w->t.dims;
    q.bpd = sdb_bkd_bpd[w->type];
    q.kb = q.dims * q.bpd;
    if (sdb_bkd_bound(ctx, w, argc > 0 ? argv[0] : JS_UNDEFINED, q.lo, "lo") || sdb_bkd_bound(ctx, w, argc > 1 ? argv[1] : JS_UNDEFINED, q.hi, "hi"))
        return JS_EXCEPTION;
    return sdb_bkd_collect(ctx, w, dyn_bkd_range_rel, dyn_bkd_range_hit, &q, magic);
}

static JSValue sdb_bkd_cidr(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdb_bkd_w* w = (sdb_bkd_w*)dyn_plain_get(ctx, this_val, sdb_bkd_class_id);
    dyn_bkd_range_q q;
    NetAddr a;
    const char* s;
    size_t n;
    int bits, bad, i;
    if (!w)
        return JS_EXCEPTION;
    if (w->type != SDB_BKD_IP)
        return JS_ThrowTypeError(ctx, "BKDTree: cidr() needs a tree of type \"ip\"");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "BKDTree: cidr() expects a prefix string such as \"10.0.0.0/8\"");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    bad = netipk_parse_prefix(s, n, &a, &bits);
    JS_FreeCString(ctx, s);
    if (bad)
        return JS_ThrowRangeError(ctx, "BKDTree: cidr() prefix is not a valid CIDR prefix");
    memset(&q, 0, sizeof(q));
    q.dims = 1;
    q.bpd = 16;
    q.kb = 16;
    if (a.is4) {
        q.lo[10] = q.lo[11] = 0xFF;
        memcpy(q.lo + 12, a.b, 4);
        bits += 96;
    } else
        memcpy(q.lo, a.b, 16);
    for (i = 0; i < 16; i++) {
        int keep = bits - i * 8;
        uint8_t mask = keep >= 8 ? 0xFF : keep <= 0 ? 0 : (uint8_t)(0xFF << (8 - keep));
        q.lo[i] &= mask;
        q.hi[i] = (uint8_t)(q.lo[i] | (uint8_t)~mask);
    }
    return sdb_bkd_collect(ctx, w, dyn_bkd_range_rel, dyn_bkd_range_hit, &q, 0);
}

static int sdb_bkd_deg(JSContext* ctx, JSValueConst v, double lim, const char* what, double* out)
{
    if (sdb_num_arg(ctx, v, what, out))
        return -1;
    if (!(*out >= -lim && *out <= lim)) {
        JS_ThrowRangeError(ctx, "BKDTree: %s must be in [-%g, %g]", what, lim, lim);
        return -1;
    }
    return 0;
}

static sdb_bkd_w* sdb_bkd_geo(JSContext* ctx, JSValueConst this_val, const char* m)
{
    sdb_bkd_w* w = (sdb_bkd_w*)dyn_plain_get(ctx, this_val, sdb_bkd_class_id);
    if (w && w->type != SDB_BKD_LATLON) {
        JS_ThrowTypeError(ctx, "BKDTree: %s() needs a tree of type \"latlon\"", m);
        return NULL;
    }
    return w;
}

static JSValue sdb_bkd_box(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdb_bkd_w* w = sdb_bkd_geo(ctx, this_val, "box");
    dyn_bkd_geo_box_q q;
    (void)argc;
    if (!w)
        return JS_EXCEPTION;
    if (sdb_bkd_deg(ctx, argv[0], 90, "minLat", &q.min_lat) || sdb_bkd_deg(ctx, argv[1], 90, "maxLat", &q.max_lat)
        || sdb_bkd_deg(ctx, argv[2], 180, "minLon", &q.min_lon) || sdb_bkd_deg(ctx, argv[3], 180, "maxLon", &q.max_lon))
        return JS_EXCEPTION;
    return sdb_bkd_collect(ctx, w, dyn_bkd_geo_box_rel, dyn_bkd_geo_box_hit, &q, 0);
}

static JSValue sdb_bkd_distance(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdb_bkd_w* w = sdb_bkd_geo(ctx, this_val, "distance");
    dyn_bkd_geo_dist_q q;
    double lat, lon, m;
    (void)argc;
    if (!w)
        return JS_EXCEPTION;
    if (sdb_bkd_deg(ctx, argv[0], 90, "lat", &lat) || sdb_bkd_deg(ctx, argv[1], 180, "lon", &lon) || sdb_num_arg(ctx, argv[2], "meters", &m))
        return JS_EXCEPTION;
    if (!(m >= 0) || isinf(m))
        return JS_ThrowRangeError(ctx, "BKDTree: meters must be a finite number >= 0");
    dyn_bkd_geo_dist_init(&q, lat, lon, m);
    return sdb_bkd_collect(ctx, w, dyn_bkd_geo_dist_rel, dyn_bkd_geo_dist_hit, &q, 0);
}

static JSValue sdb_bkd_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_bkd_w* w = (sdb_bkd_w*)dyn_plain_get(ctx, this_val, sdb_bkd_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewInt64(ctx, w->t.n);
    case 1:
        return JS_NewInt64(ctx, w->t.dims);
    case 2:
        return JS_NewString(ctx, sdb_bkd_type_names[w->type]);
    case 3:
        return JS_NewInt64(ctx, w->t.leaf);
    default:
        return JS_NewInt64(ctx, w->t.nleaves);
    }
}

const JSCFunctionListEntry sdb_bkd_proto[9] = {
    JS_CFUNC_MAGIC_DEF("range", 2, sdb_bkd_range, 0),
    JS_CFUNC_MAGIC_DEF("rangeCount", 2, sdb_bkd_range, 1),
    JS_CFUNC_DEF("cidr", 1, sdb_bkd_cidr),
    JS_CFUNC_DEF("box", 4, sdb_bkd_box),
    JS_CFUNC_DEF("distance", 3, sdb_bkd_distance),
    JS_CGETSET_MAGIC_DEF("size", sdb_bkd_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("dims", sdb_bkd_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("type", sdb_bkd_getter, NULL, 2),
    JS_CGETSET_MAGIC_DEF("leafSize", sdb_bkd_getter, NULL, 3),
};

int sdb_bkd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_bkd_w* k = JS_GetOpaque(obj, sdb_bkd_class_id);
    (void)ctx;
    if (dyn_ser_u8(w, 1) < 0 || dyn_ser_u8(w, (uint8_t)k->type) < 0 || dyn_ser_u8(w, (uint8_t)k->t.dims) < 0
        || dyn_ser_u32(w, k->t.leaf) < 0 || dyn_ser_u32(w, k->t.n) < 0)
        return -1;
    if (!k->t.n)
        return 0;
    if (dyn_ser_raw(w, k->t.sdim, k->t.nleaves) < 0 || dyn_ser_raw(w, k->t.split, (size_t)k->t.nleaves * k->t.bpd) < 0
        || dyn_ser_raw(w, k->t.keys, (size_t)k->t.n * k->t.kb) < 0)
        return -1;
    return dyn_ser_u32s(w, k->t.docs, k->t.n);
}

JSValue sdb_bkd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t ver = dyn_de_u8(r), type = dyn_de_u8(r), dims = dyn_de_u8(r), leaf = dyn_de_u32(r), n = dyn_de_u32(r);
    uint32_t i, bpd, kb, L = 0;
    uint8_t *keys = NULL, *split = NULL, *sdim = NULL;
    uint32_t* docs = NULL;
    const uint8_t *ps = NULL, *pp = NULL, *pk = NULL;
    sdb_bkd_w* w;
    int rc;
    (void)opts;
    if (!dyn_de_ok(r) || ver != 1 || type >= SDB_BKD_NTYPES || dims < 1 || dims > DYN_BKD_MAX_DIMS
        || (type == SDB_BKD_LATLON && dims != 2) || (type == SDB_BKD_IP && dims != 1)
        || leaf < 2 || leaf > DYN_BKD_MAX_LEAF || n > DYN_DS_MAX_ITEMS / dims)
        return sdb_bad(ctx);
    bpd = sdb_bkd_bpd[type];
    kb = dims * bpd;
    if (n) {
        L = 1;
        while ((uint64_t)L * leaf < n)
            L *= 2;
    }
    if (dyn_de_left(r) < (uint64_t)L * (bpd + 1) + (uint64_t)n * (kb + 4) || dyn_de_charge(r, (uint64_t)L * (bpd + 1) + (uint64_t)n * (kb + 4)) < 0)
        return sdb_bad(ctx);
    if (n) {
        ps = dyn_de_raw(r, L);
        pp = dyn_de_raw(r, (size_t)L * bpd);
        pk = dyn_de_raw(r, (size_t)n * kb);
    }
    keys = (uint8_t*)dyn_ds_malloc((size_t)n * kb + 1);
    docs = (uint32_t*)dyn_ds_malloc((size_t)n * 4 + 4);
    split = (uint8_t*)dyn_ds_malloc((size_t)L * bpd + 1);
    sdim = (uint8_t*)dyn_ds_malloc((size_t)L + 1);
    w = (sdb_bkd_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!keys || !docs || !split || !sdim || !w) {
        dyn_ds_free(keys);
        dyn_ds_free(docs);
        dyn_ds_free(split);
        dyn_ds_free(sdim);
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    if (n && ps && pp && pk) {
        memcpy(sdim, ps, L);
        memcpy(split, pp, (size_t)L * bpd);
        memcpy(keys, pk, (size_t)n * kb);
    }
    for (i = 0; i < n; i++)
        docs[i] = dyn_de_u32(r);
    w->type = (int)type;
    rc = (!dyn_de_ok(r) || (n && (!ps || !pp || !pk))) ? -3 : dyn_bkd_adopt(&w->t, dims, bpd, leaf, n, keys, docs, split, sdim);
    if (rc == -3) {
        dyn_ds_free(keys);
        dyn_ds_free(docs);
        dyn_ds_free(split);
        dyn_ds_free(sdim);
    }
    if (rc == 0 && type == SDB_BKD_LATLON)
        for (i = 0; i < n && rc == 0; i++)
            if (!(dyn_bkd_dec_f64(keys + (size_t)i * kb) >= -90 && dyn_bkd_dec_f64(keys + (size_t)i * kb) <= 90
                    && dyn_bkd_dec_f64(keys + (size_t)i * kb + 8) >= -180 && dyn_bkd_dec_f64(keys + (size_t)i * kb + 8) <= 180))
                rc = -2;
    if (rc < 0) {
        sdb_bkd_free(w);
        return sdb_bad(ctx);
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_bkd_class_id, w, sdb_bkd_free);
}

#endif
