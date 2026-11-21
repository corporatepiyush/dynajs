#include "structures-internal.h"
#include "core/ds/ds-lru.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_lru_t lru;
    JSRuntime* rt;
    JSValue on_evict;
    int busy;
} dsw_lru_t;

static void dsw_lru_free(void* native)
{
    dsw_lru_t* L = (dsw_lru_t*)native;
    if (!L)
        return;
    dyn_lru_free(&L->lru);
    JS_FreeValueRT(L->rt, L->on_evict);
    dyn_ds_free(L);
}

JSClassID dsw_lru_class_id;

DYN_FINALIZER(dsw_lru)

static void dsw_lru_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dsw_lru_t* L = JS_GetOpaque(val, dsw_lru_class_id);
    dyn_lru_node_t* n;
    if (!L)
        return;
    for (n = L->lru.head; n; n = n->next)
        JS_MarkValue(rt, ds_cell_val(&n->value), mark_func);
    JS_MarkValue(rt, L->on_evict, mark_func);
}

const JSClassDef dsw_lru_class = {
    "LRU",
    .finalizer = dsw_lru_finalizer,
    .gc_mark = dsw_lru_gc_mark,
};

static const char* const dsw_lru_keys[] = { "ttlMs", "onEvict" };

JSValue dsw_lru_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    uint32_t cap;
    if (argc < 1 || JS_ToUint32(ctx, &cap, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "capacity required")
                        : JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be > 0");
    if (cap > DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "capacity must be <= %u",
            DYN_MAX_CAPACITY);
    L = (dsw_lru_t*)dyn_ds_malloc(sizeof(*L));
    if (!L)
        return JS_ThrowOutOfMemory(ctx);
    L->rt = JS_GetRuntime(ctx);
    L->on_evict = JS_UNDEFINED;
    L->busy = 0;
    if (dyn_lru_init(&L->lru, cap, ds_cell_release, L->rt) < 0) {
        dyn_ds_free(L);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        int64_t ms = 0;
        if (dyn_opts_strict(ctx, argv[1], dsw_lru_keys,
                countof(dsw_lru_keys)))
            goto opt_fail;
        v = JS_GetPropertyStr(ctx, argv[1], "ttlMs");
        if (JS_IsException(v))
            goto opt_fail;
        if (!JS_IsUndefined(v)) {
            int rc = JS_ToInt64(ctx, &ms, v);
            JS_FreeValue(ctx, v);
            if (rc < 0)
                goto opt_fail;
            if (ms <= 0) {
                JS_ThrowRangeError(ctx, "LRU: ttlMs must be > 0");
                goto opt_fail;
            }
            L->lru.default_ttl = (uint64_t)ms;
        } else {
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[1], "onEvict");
        if (JS_IsException(v))
            goto opt_fail;
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
        } else if (!JS_IsFunction(ctx, v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "LRU: onEvict must be a function");
            goto opt_fail;
        } else {
            L->on_evict = v;
        }
    }
    return dyn_plain_wrap(ctx, new_target, dsw_lru_class_id, L, dsw_lru_free);
opt_fail:
    JS_FreeValue(ctx, L->on_evict);
    dyn_lru_free(&L->lru);
    dyn_ds_free(L);
    return JS_EXCEPTION;
}

static int dsw_lru_stale(const dyn_lru_node_t* n)
{
    return n->expires != 0 && dyn_lru_expired(n, dyn_lru_now_ms());
}

static int dsw_lru_notify(JSContext* ctx, dsw_lru_t* L, dyn_lru_node_t* n)
{
    JSValue args[2], r;
    int rc = 0;
    dyn_cell_t v;

    if (JS_IsUndefined(L->on_evict) || L->busy) {
        dyn_lru_node_free(&L->lru, n);
        return 0;
    }
    args[0] = JS_NewStringLen(ctx, n->key, n->klen);
    v = dyn_lru_node_take(n);
    args[1] = ds_cell_val(&v);
    L->busy = 1;
    r = JS_Call(ctx, L->on_evict, JS_UNDEFINED, 2, (JSValueConst*)args);
    L->busy = 0;
    if (JS_IsException(r))
        rc = -1;
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return rc;
}

static JSValue dsw_lru_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    const char* key;
    size_t klen;
    dyn_lru_node_t* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    n = dyn_lru_find(&L->lru, key, klen);
    JS_FreeCString(ctx, key);
    if (n && dsw_lru_stale(n)) {
        dyn_lru_detach(&L->lru, n);
        L->lru.expired++;
        if (dsw_lru_notify(ctx, L, n) < 0)
            return JS_EXCEPTION;
        n = NULL;
    }
    if (!n) {
        L->lru.misses++;
        return JS_UNDEFINED;
    }
    L->lru.hits++;
    dyn_lru_touch(&L->lru, n);
    return JS_DupValue(ctx, ds_cell_val(&n->value));
}

static JSValue dsw_lru_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dsw_lru_t* L;
    const char* key;
    size_t klen;
    uint64_t ttl;
    dyn_lru_node_t* victim;
    dyn_cell_t c;
    int64_t ms = 0;
    int rc;

    if (magic == 1) {
        if (argc < 3)
            return JS_ThrowTypeError(ctx,
                "setWithTTL(key, value, ms): ms is required");
        if (JS_ToInt64(ctx, &ms, argv[2]))
            return JS_EXCEPTION;
        if (ms <= 0)
            return JS_ThrowRangeError(ctx, "setWithTTL: ms must be > 0");
    }
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    if (L->busy) {
        JS_FreeCString(ctx, key);
        return JS_ThrowTypeError(ctx, "LRU: onEvict must not modify the cache "
                                      "it is being called from");
    }
    ttl = magic == 1 ? (uint64_t)ms : L->lru.default_ttl;
    c = ds_val_cell(argv[1]);
    rc = dyn_lru_set(&L->lru, key, klen, &c, ttl, &victim);
    JS_FreeCString(ctx, key);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    JS_DupValue(ctx, argv[1]);
    if (victim && dsw_lru_notify(ctx, L, victim) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_lru_purge_expired(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    dyn_lru_node_t *n, *next, *dead = NULL;
    uint64_t now;
    uint32_t removed = 0;
    (void)argc;
    (void)argv;

    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    if (L->busy)
        return JS_ThrowTypeError(ctx, "LRU: onEvict must not modify the cache "
                                      "it is being called from");
    now = dyn_lru_now_ms();
    for (n = L->lru.head; n; n = next) {
        next = n->next;
        if (!dyn_lru_expired(n, now))
            continue;
        dyn_lru_detach(&L->lru, n);
        n->next = dead;
        dead = n;
        removed++;
    }
    L->lru.expired += removed;
    for (n = dead; n; n = next) {
        next = n->next;
        if (dsw_lru_notify(ctx, L, n) < 0) {
            for (n = next; n; n = next) {
                next = n->next;
                dyn_lru_node_free(&L->lru, n);
            }
            return JS_EXCEPTION;
        }
    }
    return JS_NewUint32(ctx, removed);
}

static JSValue dsw_lru_get_stats(JSContext* ctx, JSValueConst this_val)
{
    dsw_lru_t* L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    JSValue o;

    if (!L)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define STAT(name, v)                                                           \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewInt64(ctx, (int64_t)(v)), \
            JS_PROP_C_W_E)                                                      \
        < 0) {                                                                  \
        JS_FreeValue(ctx, o);                                                   \
        return JS_EXCEPTION;                                                    \
    }
    STAT("hits", L->lru.hits)
    STAT("misses", L->lru.misses)
    STAT("evictions", L->lru.evictions)
    STAT("expired", L->lru.expired)
    STAT("size", L->lru.count)
    STAT("capacity", L->lru.cap)
#undef STAT
    return o;
}

static JSValue dsw_lru_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    const char* key;
    size_t klen;
    dyn_lru_node_t* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    n = dyn_lru_find(&L->lru, key, klen);
    JS_FreeCString(ctx, key);
    if (n && dsw_lru_stale(n))
        n = NULL;
    return JS_NewBool(ctx, n != NULL);
}

static JSValue dsw_lru_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    const char* key;
    size_t klen;
    dyn_lru_node_t* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    n = dyn_lru_find(&L->lru, key, klen);
    JS_FreeCString(ctx, key);
    if (!n)
        return JS_NewBool(ctx, 0);
    dyn_lru_detach(&L->lru, n);
    dyn_lru_node_free(&L->lru, n);
    return JS_NewBool(ctx, 1);
}

static JSValue dsw_lru_size(JSContext* ctx, JSValueConst this_val)
{
    dsw_lru_t* L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)L->lru.count);
}

static JSValue dsw_lru_capacity(JSContext* ctx, JSValueConst this_val)
{
    dsw_lru_t* L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)L->lru.cap);
}

static JSValue dsw_lru_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dsw_lru_t* L;
    dyn_lru_node_t* n;
    JSValue arr;
    uint64_t now;
    uint32_t out = 0;
    (void)argc;
    (void)argv;

    L = (dsw_lru_t*)dyn_plain_get(ctx, this_val, dsw_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    now = dyn_lru_now_ms();
    for (n = L->lru.head; n; n = n->next) {
        JSValue pair;
        if (dyn_lru_expired(n, now))
            continue;
        pair = ds2_tuple2(ctx, JS_NewStringLen(ctx, n->key, n->klen),
            JS_DupValue(ctx, ds_cell_val(&n->value)));
        if (JS_IsException(pair)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, out++, pair,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dsw_lru_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

const JSCFunctionListEntry dsw_lru_proto[] = {
    JS_CFUNC_DEF("get", 1, dsw_lru_get),
    JS_CFUNC_MAGIC_DEF("put", 2, dsw_lru_put, 0),
    JS_CFUNC_MAGIC_DEF("set", 2, dsw_lru_put, 0),
    JS_CFUNC_MAGIC_DEF("setWithTTL", 3, dsw_lru_put, 1),
    JS_CFUNC_DEF("purgeExpired", 0, dsw_lru_purge_expired),
    JS_CGETSET_DEF("stats", dsw_lru_get_stats, NULL),
    JS_CFUNC_DEF("has", 1, dsw_lru_has),
    JS_CFUNC_DEF("delete", 1, dsw_lru_delete),
    JS_CFUNC_DEF("entries", 0, dsw_lru_entries),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dsw_lru_iterator),
    JS_CGETSET_DEF("size", dsw_lru_size, NULL),
    JS_CGETSET_DEF("capacity", dsw_lru_capacity, NULL),
};

int dsw_lru_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsw_lru_t* L = JS_GetOpaque(obj, dsw_lru_class_id);
    dyn_lru_node_t* n;
    JSValue arr;
    uint32_t i;
    int r;
    if (!L)
        return -1;

    if (dyn_ser_u32(w, L->lru.cap) < 0 || dyn_ser_u32(w, L->lru.count) < 0)
        return -1;
    for (n = L->lru.tail; n; n = n->prev)
        if (dyn_ser_blob(w, n->key, n->klen) < 0)
            return -1;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return -1;
    i = 0;
    for (n = L->lru.tail; n; n = n->prev)
        if (JS_DefinePropertyValueUint32(ctx, arr, i++,
                JS_DupValue(ctx, ds_cell_val(&n->value)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

JSValue dsw_lru_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t cap = dyn_de_u32(r), n, i;
    JSValue obj, vals, arg;
    const char** keys;
    size_t* klens;
    JSAtom put_atom;
    (void)opts;

    if (!dyn_de_ok(r) || cap == 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_count(r, &n, 4) < 0 || n > cap)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    keys = dyn_ds_calloc(n ? n : 1, sizeof(*keys));
    klens = dyn_ds_calloc(n ? n : 1, sizeof(*klens));
    if (!keys || !klens) {
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        keys[i] = dyn_de_blob(r, &klens[i]);
    if (!dyn_de_ok(r)) {
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = dyn_ds_read_values(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        return vals;
    }
    arg = JS_NewInt64(ctx, (int64_t)cap);
    obj = dyn_ds_construct(ctx, dsw_lru_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(keys);
        dyn_ds_free(klens);
        return obj;
    }
    put_atom = JS_NewAtom(ctx, "put");
    for (i = 0; i < n; i++) {
        JSValue args[2];
        int bad;
        args[0] = JS_NewStringLen(ctx, keys[i], klens[i]);
        args[1] = dyn_ds_value_at(ctx, vals, i);
        bad = put_atom == JS_ATOM_NULL || JS_IsException(args[0]) || JS_IsException(args[1]) || dyn_ds_invoke_atom(ctx, obj, put_atom, 2, (JSValueConst*)args) < 0;
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (bad) {
            JS_FreeAtom(ctx, put_atom);
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            dyn_ds_free(keys);
            dyn_ds_free(klens);
            return JS_EXCEPTION;
        }
    }
    JS_FreeAtom(ctx, put_atom);
    JS_FreeValue(ctx, vals);
    dyn_ds_free(keys);
    dyn_ds_free(klens);
    return obj;
}

DS2_ITERATOR(dsw_lru, "entries")

#endif
