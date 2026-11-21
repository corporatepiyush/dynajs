#include "structures-internal.h"
#include "core/ds/ds-rstar-tree.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_rstar_t* t;
    unsigned dims;
} dsx_rstar_t;

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_rs_collect;

typedef struct {
    dyn_ser_t* w;
    unsigned dims;
    int failed;
} dsx_rs_ser;

static void dsx_rstar_free(void* native)
{
    dsx_rstar_t* r = (dsx_rstar_t*)native;
    if (!r)
        return;
    dyn_rstar_free(r->t);
    dyn_ds_free(r);
}

JSClassID dsx_rstar_class_id;

DYN_FINALIZER(dsx_rstar)

const JSClassDef dsx_rstar_class = { "RStarTree", .finalizer = dsx_rstar_finalizer };

JSValue dsx_rstar_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int32_t dims = 2;
    dsx_rstar_t* r;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &dims, argv[0]))
        return JS_EXCEPTION;
    if (dims < 1 || dims > DYN_RSTAR_MAX_DIMS)
        return JS_ThrowRangeError(ctx, "RStarTree: dims must be 1..%d", DYN_RSTAR_MAX_DIMS);
    r = (dsx_rstar_t*)dyn_ds_malloc(sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->dims = (unsigned)dims;
    r->t = dyn_rstar_new(r->dims);
    if (!r->t) {
        dyn_ds_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_rstar_class_id, r, dsx_rstar_free);
}

static int dsx_coords(JSContext* ctx, JSValueConst v, unsigned dims, double* out, const char* what)
{
    uint32_t i;
    for (i = 0; i < dims; i++) {
        JSValue x = JS_GetPropertyUint32(ctx, v, i);
        if (JS_IsException(x))
            return -1;
        if (dsx_to_f64_free(ctx, &out[i], x))
            return -1;
        if (out[i] != out[i]) {
            JS_ThrowRangeError(ctx, "RStarTree: %s[%u] is NaN", what, i);
            return -1;
        }
    }
    return 0;
}

static int dsx_rstar_args(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, dsx_rstar_t** pr, double* lo, double* hi, uint64_t* id, int want_id)
{
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    unsigned dims;
    if (!r)
        return -1;
    dims = r->dims;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1])) {
        JS_ThrowTypeError(ctx, "RStarTree: expected (lo: number[], hi: number[])");
        return -1;
    }
    if (dsx_coords(ctx, argv[0], dims, lo, "lo") || dsx_coords(ctx, argv[1], dims, hi, "hi"))
        return -1;
    if (want_id) {
        double d;
        if (JS_ToFloat64(ctx, &d, argc > 2 ? argv[2] : JS_UNDEFINED))
            return -1;
        if (!(d >= 0) || d > 9007199254740991.0 || d != (double)(uint64_t)d) {
            JS_ThrowRangeError(ctx, "RStarTree: id must be a non-negative safe integer");
            return -1;
        }
        *id = (uint64_t)d;
    }
    *pr = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!*pr)
        return -1;
    if ((*pr)->dims != dims) {
        JS_ThrowTypeError(ctx, "RStarTree changed during argument conversion");
        return -1;
    }
    return 0;
}

static JSValue dsx_rstar_insert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    uint64_t id;
    dsx_rstar_t* r;
    unsigned d;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, &id, 1))
        return JS_EXCEPTION;
    for (d = 0; d < r->dims; d++)
        if (lo[d] > hi[d])
            return JS_ThrowRangeError(ctx, "RStarTree.insert: lo[%u] > hi[%u]", d, d);
    if (dyn_rstar_insert(r->t, lo, hi, id) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsx_rstar_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    uint64_t id;
    dsx_rstar_t* r;
    int rc;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, &id, 1))
        return JS_EXCEPTION;
    rc = dyn_rstar_remove(r->t, lo, hi, id);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewBool(ctx, rc);
}

static int dsx_rs_visit(const double* lo, const double* hi, uint64_t id, void* ud)
{
    dsx_rs_collect* c = (dsx_rs_collect*)ud;
    (void)lo;
    (void)hi;
    if (JS_DefinePropertyValueUint32(c->ctx, c->arr, c->n++, JS_NewInt64(c->ctx, (int64_t)id), JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    return 0;
}

static JSValue dsx_rstar_search(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    dsx_rstar_t* r;
    dsx_rs_collect c;
    if (dsx_rstar_args(ctx, this_val, argc, argv, &r, lo, hi, NULL, 0))
        return JS_EXCEPTION;
    c.ctx = ctx;
    c.n = 0;
    c.failed = 0;
    c.arr = JS_NewArray(ctx);
    if (JS_IsException(c.arr))
        return c.arr;
    dyn_rstar_search(r->t, lo, hi, dsx_rs_visit, &c);
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

static JSValue dsx_rstar_nearest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double p[DYN_RSTAR_MAX_DIMS], dk = 1, *dist;
    uint64_t* ids;
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    unsigned dims;
    size_t got, i;
    JSValue arr;
    if (!r)
        return JS_EXCEPTION;
    dims = r->dims;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "RStarTree.nearest: expected (point: number[], k?)");
    if (dsx_coords(ctx, argv[0], dims, p, "point"))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &dk, argv[1]))
        return JS_EXCEPTION;
    if (!(dk >= 1) || dk > 100000 || dk != (double)(size_t)dk)
        return JS_ThrowRangeError(ctx, "RStarTree.nearest: k must be an integer in 1..100000");
    r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!r || r->dims != dims)
        return r ? JS_ThrowTypeError(ctx, "RStarTree changed during argument conversion") : JS_EXCEPTION;
    ids = (uint64_t*)dyn_ds_malloc(sizeof(uint64_t) * (size_t)dk);
    dist = (double*)dyn_ds_malloc(sizeof(double) * (size_t)dk);
    if (!ids || !dist) {
        dyn_ds_free(ids);
        dyn_ds_free(dist);
        return JS_ThrowOutOfMemory(ctx);
    }
    got = dyn_rstar_nearest(r->t, p, (size_t)dk, ids, dist);
    arr = JS_NewArray(ctx);
    for (i = 0; !JS_IsException(arr) && i < got; i++) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        JS_DefinePropertyValueStr(ctx, o, "id", JS_NewInt64(ctx, (int64_t)ids[i]), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "distance", JS_NewFloat64(ctx, sqrt(dist[i])), JS_PROP_C_W_E);
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(ids);
    dyn_ds_free(dist);
    return arr;
}

static JSValue dsx_rstar_info(JSContext* ctx, JSValueConst this_val, int magic)
{
    dsx_rstar_t* r = dyn_plain_get(ctx, this_val, dsx_rstar_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)dyn_rstar_size(r->t));
    if (magic == 1)
        return JS_NewInt64(ctx, dyn_rstar_height(r->t));
    return JS_NewInt64(ctx, r->dims);
}

const JSCFunctionListEntry dsx_rstar_proto[] = {
    JS_CFUNC_DEF("insert", 3, dsx_rstar_insert),
    JS_CFUNC_DEF("remove", 3, dsx_rstar_remove),
    JS_CFUNC_DEF("search", 2, dsx_rstar_search),
    JS_CFUNC_DEF("nearest", 2, dsx_rstar_nearest),
    JS_CGETSET_MAGIC_DEF("size", dsx_rstar_info, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", dsx_rstar_info, NULL, 1),
    JS_CGETSET_MAGIC_DEF("dims", dsx_rstar_info, NULL, 2),
};

static int dsx_rs_ser_visit(const double* lo, const double* hi, uint64_t id, void* ud)
{
    dsx_rs_ser* s = (dsx_rs_ser*)ud;
    unsigned d;
    for (d = 0; d < s->dims; d++)
        if (dyn_ser_f64(s->w, lo[d]) < 0 || dyn_ser_f64(s->w, hi[d]) < 0)
            s->failed = 1;
    if (dyn_ser_u64(s->w, id) < 0)
        s->failed = 1;
    return s->failed;
}

int dsx_rstar_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_rstar_t* r = JS_GetOpaque(obj, dsx_rstar_class_id);
    double lo[DYN_RSTAR_MAX_DIMS], hi[DYN_RSTAR_MAX_DIMS];
    dsx_rs_ser s;
    unsigned d;
    if (!r)
        return -1;
    for (d = 0; d < r->dims; d++) {
        lo[d] = -(double)INFINITY;
        hi[d] = (double)INFINITY;
    }
    s.w = w;
    s.dims = r->dims;
    s.failed = dyn_ser_u8(w, (uint8_t)r->dims) < 0 || dyn_ser_u32(w, (uint32_t)dyn_rstar_size(r->t)) < 0;
    if (!s.failed)
        dyn_rstar_search(r->t, lo, hi, dsx_rs_ser_visit, &s);
    if (s.failed) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

JSValue dsx_rstar_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t dims = dyn_de_u8(r);
    uint32_t n, i;
    unsigned d;
    JSValue obj, dv;
    dsx_rstar_t* t;
    (void)opts;
    if (!dyn_de_ok(r) || dims < 1 || dims > DYN_RSTAR_MAX_DIMS || dyn_de_count(r, &n, (size_t)dims * 16 + 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    dv = JS_NewInt32(ctx, dims);
    obj = dsx_rstar_ctor(ctx, JS_UNDEFINED, 1, (JSValueConst*)&dv);
    if (JS_IsException(obj))
        return obj;
    t = JS_GetOpaque(obj, dsx_rstar_class_id);
    {
        double* bx = (double*)malloc(sizeof(double) * 2 * dims * (n ? n : 1));
        uint64_t* ids = (uint64_t*)malloc(sizeof(uint64_t) * (n ? n : 1));
        int bad = !bx || !ids;
        for (i = 0; i < n && !bad; i++) {
            double* b = bx + (size_t)i * 2 * dims;
            for (d = 0; d < dims; d++) {
                b[d] = dyn_de_f64(r);
                b[dims + d] = dyn_de_f64(r);
                if (!(b[d] <= b[dims + d]))
                    bad = 2;
            }
            ids[i] = dyn_de_u64(r);
            if (!dyn_de_ok(r) || ids[i] > 9007199254740991ULL)
                bad = 2;
        }
        if (!bad && dyn_rstar_bulk_load(t->t, bx, ids, n) < 0)
            bad = 1;
        free(bx);
        free(ids);
        if (bad) {
            JS_FreeValue(ctx, obj);
            return bad == 2 ? dyn_codec_throw(ctx, DYN_DE_TRUNCATED) : JS_ThrowOutOfMemory(ctx);
        }
    }
    return obj;
}

#endif
