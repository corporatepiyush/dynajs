#include "structures-internal.h"
#include "core/ds/ds-tdigest.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_tdigest_t t;
} sdb_td_w;

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

JSClassID sdb_td_class_id;

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

const JSClassDef sdb_td_class = {
    "TDigest",
    .finalizer = sdb_td_finalizer,
};

JSValue sdb_td_ctor(JSContext* ctx, JSValueConst new_target,
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
    {
        double ototal;
        const dyn_tdigest_cent* oc = dyn_tdigest_view(&o->t, &n, &ototal);
        tmp = (dyn_tdigest_cent*)dyn_ds_malloc(sizeof(dyn_tdigest_cent) * ((size_t)n + 1));
        if (!tmp)
            return ds2_oom(ctx);
        memcpy(tmp, oc, sizeof(dyn_tdigest_cent) * n);
    }
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
    case 4: {
        uint32_t n;
        double total;
        dyn_tdigest_view(&w->t, &n, &total);
        return JS_NewInt64(ctx, n);
    }
    default:
        return JS_NewFloat64(ctx, w->t.delta);
    }
}

const JSCFunctionListEntry sdb_td_proto[] = {
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

int sdb_td_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_td_w* t = JS_GetOpaque(obj, sdb_td_class_id);
    uint32_t i, all = t->t.n + t->t.nbuf;
    (void)ctx;
    if (dyn_ser_f64(w, -t->t.delta) < 0 || dyn_ser_f64(w, t->t.min) < 0 || dyn_ser_f64(w, t->t.max) < 0 || dyn_ser_f64(w, t->t.sum) < 0 || dyn_ser_u32(w, t->t.n) < 0 || dyn_ser_u32(w, t->t.nbuf) < 0)
        return -1;
    for (i = 0; i < all; i++)
        if (dyn_ser_f64(w, t->t.c[i].m) < 0 || dyn_ser_f64(w, t->t.c[i].w) < 0)
            return -1;
    return 0;
}

JSValue sdb_td_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double delta = dyn_de_f64(r), mn = dyn_de_f64(r), mx = dyn_de_f64(r), sum = dyn_de_f64(r);
    uint32_t n, nbuf = 0, i;
    int with_buffer = delta < 0;
    sdb_td_w* t;
    double total = 0;
    (void)opts;
    if (with_buffer)
        delta = -delta;
    if (!dyn_de_ok(r) || !(delta >= DYN_TDIGEST_MIN_DELTA && delta <= DYN_TDIGEST_MAX_DELTA) || dyn_de_count(r, &n, 16) < 0
        || (with_buffer && dyn_de_count(r, &nbuf, 16) < 0))
        return sdb_bad(ctx);
    t = (sdb_td_w*)dyn_ds_calloc(1, sizeof(*t));
    if (!t || dyn_tdigest_init(&t->t, delta) < 0) {
        dyn_ds_free(t);
        return ds2_oom(ctx);
    }
    if (n > t->t.cap || nbuf > t->t.cap - n || (n + nbuf && !(mn <= mx && isfinite(mn) && isfinite(mx))) || !isfinite(sum)) {
        sdb_td_free(t);
        return sdb_bad(ctx);
    }
    for (i = 0; i < n + nbuf; i++) {
        double m = dyn_de_f64(r), wt = dyn_de_f64(r);
        if (!dyn_de_ok(r) || !isfinite(m) || !(wt > 0) || !isfinite(wt) || m < mn || m > mx || (i && i < n && m < t->t.c[i - 1].m)) {
            sdb_td_free(t);
            return sdb_bad(ctx);
        }
        t->t.c[i].m = m;
        t->t.c[i].w = wt;
        total += wt;
    }
    t->t.n = n;
    t->t.nbuf = nbuf;
    t->t.total = total;
    t->t.sum = sum;
    t->t.min = n + nbuf ? mn : HUGE_VAL;
    t->t.max = n + nbuf ? mx : -HUGE_VAL;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_td_class_id, t, sdb_td_free);
}

#endif
