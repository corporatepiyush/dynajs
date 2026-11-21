#include "structures-internal.h"
#include "core/ds/ds-consistent-hash.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_chash_t c;
} sdb_ch_w;

JSClassID sdb_ch_class_id;

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

const JSClassDef sdb_ch_class = {
    "ConsistentHash",
    .finalizer = sdb_ch_finalizer,
};

JSValue sdb_ch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    uint32_t rep = 160;
    int jump = 0;
    if (argc >= 1 && sdb_opt_u32(ctx, argv[0], "replicas", &rep))
        return JS_EXCEPTION;
    if (argc >= 1 && dsx_cache_opt(ctx, argv[0], "ConsistentHash", "jump", &jump) < 0)
        return JS_EXCEPTION;
    if (rep < 1 || rep > DYN_CHASH_MAX_REPLICAS)
        return JS_ThrowRangeError(ctx, "replicas must be 1..%u", DYN_CHASH_MAX_REPLICAS);
    w = (sdb_ch_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    dyn_chash_init(&w->c, rep);
    w->c.jon = (uint8_t)jump;
    w->c.jdirty = 1;
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
            const dyn_chash_node* nd = &w->c.nodes[w->c.ring[dyn_chash_find(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0))].node];
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
    pos = dyn_chash_find(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0));
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

JSValue sdb_ch_jump(JSContext* ctx, JSValueConst this_val,
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

const JSCFunctionListEntry sdb_ch_proto[] = {
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

int sdb_ch_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
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

JSValue sdb_ch_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
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
        if (!dyn_de_ok(r) || wt < 1 || wt > DYN_CHASH_MAX_WEIGHT || dyn_ds_smap_find(&c->c.idx, k, kn, dyn_ds_strhash(k, kn))) {
            sdb_ch_free(c);
            return sdb_bad(ctx);
        }
        if (dyn_chash_append_node(&c->c, k, kn, wt) < 0) {
            sdb_ch_free(c);
            return ds2_oom(ctx);
        }
    }
    {
        int rc = dyn_chash_commit(&c->c);
        if (rc < 0) {
            sdb_ch_free(c);
            return rc == -2 ? sdb_bad(ctx) : ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ch_class_id, c, sdb_ch_free);
}

#endif
