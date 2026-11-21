#include "structures-internal.h"
#include "core/ds/ds-kdtree.h"
#include "core/ds/ds-roaring-bitmap.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_kdtree_t t;
} sdb_kd_w;

JSClassID sdb_kd_class_id;

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

const JSClassDef sdb_kd_class = {
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

JSValue sdb_kd_ctor(JSContext* ctx, JSValueConst new_target,
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

const JSCFunctionListEntry sdb_kd_proto[] = {
    JS_CFUNC_MAGIC_DEF("knn", 2, sdb_kd_knn, 0),
    JS_CFUNC_MAGIC_DEF("nearest", 1, sdb_kd_knn, 1),
    JS_CFUNC_MAGIC_DEF("knnMany", 2, sdb_kd_knn, 2),
    JS_CFUNC_MAGIC_DEF("radius", 2, sdb_kd_region, 0),
    JS_CFUNC_MAGIC_DEF("box", 2, sdb_kd_region, 1),
    JS_CFUNC_DEF("point", 1, sdb_kd_point_m),
    JS_CGETSET_MAGIC_DEF("size", sdb_kd_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("dims", sdb_kd_getter, NULL, 1),
};

int sdb_kd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_kd_w* k = JS_GetOpaque(obj, sdb_kd_class_id);
    size_t total = (size_t)k->t.n * k->t.d;
    (void)ctx;
    if (dyn_ser_u32(w, k->t.d) < 0 || dyn_ser_u32(w, k->t.n) < 0)
        return -1;
    return dyn_ser_f64s(w, k->t.pts, total);
}

JSValue sdb_kd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
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

#endif
