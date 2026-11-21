#include "structures-internal.h"
#include "core/ds/ds-cuckoo-filter.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_cuckoo_t c;
} sdb_cf_w;

JSClassID sdb_cf_class_id;

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

const JSClassDef sdb_cf_class = {
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

JSValue sdb_cf_ctor(JSContext* ctx, JSValueConst new_target,
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

const JSCFunctionListEntry sdb_cf_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_cf_op, 0),
    JS_CFUNC_MAGIC_DEF("mayContain", 1, sdb_cf_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_cf_op, 2),
    JS_CGETSET_MAGIC_DEF("size", sdb_cf_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("capacity", sdb_cf_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("loadFactor", sdb_cf_getter, NULL, 2),
};

int sdb_cf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
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

JSValue sdb_cf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
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

#endif
