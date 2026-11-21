#include "structures-internal.h"
#include "core/ds/ds-union-find.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DYN_DECODE_UF_MAX (1u << 23)

typedef dyn_uf_t dsw_uf_t;

#define DS_UF_EXT 0xFFFFFFFFu
#define DS_UF_IDENTITY 0
#define DS_UF_DELTA 1
#define DS_UF_RAW 2

static void dsw_uf_free(void* native)
{
    dsw_uf_t* u = (dsw_uf_t*)native;
    if (!u)
        return;
    dyn_uf_free(u);
    dyn_ds_free(u);
}

JSClassID dsw_uf_class_id;

DYN_FINALIZER(dsw_uf)

const JSClassDef dsw_uf_class = {
    "UnionFind",
    .finalizer = dsw_uf_finalizer,
};

JSValue dsw_uf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_uf_t* u;
    uint32_t n = 0;

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], DYN_UF_MAX_N, &n))
        return JS_EXCEPTION;
    u = (dsw_uf_t*)dyn_ds_malloc(sizeof(*u));
    if (!u)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_uf_init(u, n) < 0) {
        dyn_ds_free(u);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_uf_class_id, u, dsw_uf_free);
}

static JSValue dsw_uf_find_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_uf_t* u;
    uint32_t x;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    u = (dsw_uf_t*)dyn_plain_get(ctx, this_val, dsw_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    return JS_NewInt64(ctx, (int64_t)dyn_uf_find(u, x));
}

static JSValue dsw_uf_union(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_uf_t* u;
    uint32_t x, y;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &y))
        return JS_EXCEPTION;
    u = (dsw_uf_t*)dyn_plain_get(ctx, this_val, dsw_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n || y >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    return JS_NewBool(ctx, dyn_uf_union(u, x, y));
}

static JSValue dsw_uf_connected(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_uf_t* u;
    uint32_t x, y;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &y))
        return JS_EXCEPTION;
    u = (dsw_uf_t*)dyn_plain_get(ctx, this_val, dsw_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n || y >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    return JS_NewBool(ctx, dyn_uf_find(u, x) == dyn_uf_find(u, y));
}

static JSValue dsw_uf_count(JSContext* ctx, JSValueConst this_val)
{
    dsw_uf_t* u = (dsw_uf_t*)dyn_plain_get(ctx, this_val, dsw_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)u->sets);
}

static JSValue dsw_uf_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_uf_t* u = (dsw_uf_t*)dyn_plain_get(ctx, this_val, dsw_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)u->n);
}

const JSCFunctionListEntry dsw_uf_proto[] = {
    JS_CFUNC_DEF("find", 1, dsw_uf_find_m),
    JS_CFUNC_DEF("union", 2, dsw_uf_union),
    JS_CFUNC_DEF("connected", 2, dsw_uf_connected),
    JS_CGETSET_DEF("count", dsw_uf_count, NULL),
    JS_CGETSET_DEF("size", dsw_uf_size, NULL),
};

int dsw_uf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_uf_t* u = JS_GetOpaque(obj, dsw_uf_class_id);
    uint32_t i;
    uint8_t kind = DS_UF_IDENTITY;
    (void)ctx;
    if (!u)
        return -1;

    if (dyn_ser_u32(w, DS_UF_EXT) < 0 || dyn_ser_u32(w, u->n) < 0 || dyn_ser_u32(w, u->sets) < 0)
        return -1;
    for (i = 0; i < u->n; i++)
        if (u->parent[i] != i || u->rank[i]) {
            kind = DS_UF_DELTA;
            break;
        }
    if (dyn_ser_u8(w, kind) < 0)
        return -1;
    if (kind == DS_UF_IDENTITY)
        return 0;
    for (i = 0; i < u->n; i++) {
        int64_t d = (int64_t)u->parent[i] - (int64_t)i;
        if (dyn_sk_uvarint(w, ((uint64_t)d << 1) ^ (uint64_t)(d >> 63)) < 0)
            return -1;
    }
    for (i = 0; i < u->n;) {
        uint32_t j = i;
        while (j < u->n && u->rank[j] == u->rank[i])
            j++;
        if (dyn_sk_uvarint(w, j - i) < 0 || dyn_ser_u8(w, u->rank[i]) < 0)
            return -1;
        i = j;
    }
    return 0;
}

JSValue dsw_uf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first, n, sets, i;
    uint8_t kind;
    JSValue obj, arg;
    dsw_uf_t* u;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first == DS_UF_EXT) {
        n = dyn_de_u32(r);
        sets = dyn_de_u32(r);
        kind = dyn_de_u8(r);
        if (!dyn_de_ok(r) || kind > DS_UF_DELTA || n > DS_NUM_MAX || (kind == DS_UF_IDENTITY && n > DYN_DECODE_UF_MAX) || (kind == DS_UF_DELTA && (uint64_t)n > (uint64_t)dyn_de_left(r)))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = first;
        sets = dyn_de_u32(r);
        kind = DS_UF_RAW;
        if (!dyn_de_ok(r) || (uint64_t)n * 5 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (dyn_de_charge(r, (uint64_t)n * 5) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    arg = JS_NewInt64(ctx, (int64_t)n);
    obj = dyn_ds_construct(ctx, dsw_uf_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj))
        return obj;
    u = JS_GetOpaque(obj, dsw_uf_class_id);

    if (kind == DS_UF_IDENTITY) {
        for (i = 0; i < n; i++) {
            u->parent[i] = i;
            u->rank[i] = 0;
        }
    } else if (kind == DS_UF_DELTA) {
        for (i = 0; i < n; i++) {
            uint64_t z;
            int64_t p;
            if (dyn_sk_read_uvarint(r, &z) < 0)
                goto bad;
            p = (int64_t)i + ((int64_t)(z >> 1) ^ -(int64_t)(z & 1));
            if (p < 0 || p >= (int64_t)n)
                goto bad;
            u->parent[i] = (uint32_t)p;
        }
        for (i = 0; i < n;) {
            uint64_t run;
            uint8_t rk;
            if (dyn_sk_read_uvarint(r, &run) < 0)
                goto bad;
            rk = dyn_de_u8(r);
            if (!dyn_de_ok(r) || !run || run > (uint64_t)(n - i))
                goto bad;
            while (run--)
                u->rank[i++] = rk;
        }
    } else {
        for (i = 0; i < n; i++) {
            uint32_t p = dyn_de_u32(r);
            uint8_t rk = dyn_de_u8(r);
            if (p >= n)
                goto bad;
            u->parent[i] = p;
            u->rank[i] = rk;
        }
    }
    if (!dyn_de_ok(r) || sets > n)
        goto bad;
    if (kind != DS_UF_IDENTITY) {
        uint32_t roots;
        int crc = dyn_uf_validate(u, &roots);
        if (crc < 0) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (crc > 0)
            goto bad;
        u->sets = roots;
        return obj;
    }
    u->sets = n;
    return obj;
bad:
    JS_FreeValue(ctx, obj);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
}

#endif
