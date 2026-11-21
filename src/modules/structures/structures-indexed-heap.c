#include "structures-internal.h"
#include "core/ds/ds-indexed-heap.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_iheap_t q;
} sdb_ih_w;

JSClassID sdb_ih_class_id;

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

const JSClassDef sdb_ih_class = {
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

JSValue sdb_ih_ctor(JSContext* ctx, JSValueConst new_target,
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

const JSCFunctionListEntry sdb_ih_proto[] = {
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

int sdb_ih_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
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

JSValue sdb_ih_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
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

#endif
