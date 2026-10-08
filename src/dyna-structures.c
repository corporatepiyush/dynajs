#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#include "core/dyn-ds.h"
#include "core/dyn-prng.h"

#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_MAX_CAPACITY (1u << 24)
#define DYN_DECODE_CHUNK_MAX_WORDS (1u << 22)
#define DYN_DECODE_SKETCH_MAX_WORDS (1u << 22)
#define DYN_DECODE_AMPLIFIED_NUM_MAX (1u << 23)
#define DYN_DECODE_UF_MAX (1u << 23)

#define DYN_FINALIZER(pfx)                                  \
    static void pfx##_finalizer(JSRuntime* rt, JSValue val) \
    {                                                       \
        (void)rt;                                           \
        pfx##_free(JS_GetOpaque(val, pfx##_class_id));      \
    }

static JSValue dyn_ds_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method,
    int argc, JSValueConst* argv)
{
    JSValue fn, arr, values, iter;

    fn = JS_GetPropertyStr(ctx, this_val, method);
    if (JS_IsException(fn))
        return fn;
    arr = JS_Call(ctx, fn, this_val, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return arr;
    values = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, arr);
        return values;
    }
    iter = JS_Call(ctx, values, arr, 0, NULL);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, arr);
    return iter;
}

static JSValue dyn_ds_iterator_toarray(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_ds_iter_via(ctx, this_val, "toArray", 0, NULL);
}

static JSValue dyn_ds_iterator_trie(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue empty = JS_NewString(ctx, "");
    JSValueConst args[1];
    JSValue r;
    (void)argc;
    (void)argv;
    if (JS_IsException(empty))
        return empty;
    args[0] = empty;
    r = dyn_ds_iter_via(ctx, this_val, "keysWithPrefix", 1, args);
    JS_FreeValue(ctx, empty);
    return r;
}

typedef struct {
    uint64_t* words;
    uint32_t nwords;
} dyn_bitset_t;

_Static_assert(sizeof(dyn_bitset_t) <= 16, "dyn_bitset_t grew past 16 bytes");

static void dyn_bitset_free(void* native)
{
    dyn_bitset_t* b = (dyn_bitset_t*)native;
    if (!b)
        return;
    dyn_nat_free(b->words);
    dyn_nat_free(b);
}

static JSClassID dyn_bitset_class_id;
DYN_FINALIZER(dyn_bitset)
static const JSClassDef dyn_bitset_class = {
    "BitSet",
    .finalizer = dyn_bitset_finalizer,
};

static int dyn_size_arg(JSContext* ctx, JSValueConst v, uint32_t max,
    uint32_t* out)
{
    double nd;
    if (JS_ToFloat64(ctx, &nd, v))
        return -1;
    if (!(nd >= 0) || nd > (double)max || nd != (double)(uint32_t)nd) {
        JS_ThrowRangeError(ctx, "size must be an integer in 0..%u",
            (unsigned)max);
        return -1;
    }
    *out = (uint32_t)nd;
    return 0;
}

static int dyn_bitset_ensure(dyn_bitset_t* b, uint32_t wi)
{
    uint32_t ncap;
    uint64_t* nw;

    if (wi < b->nwords)
        return 0;
    ncap = b->nwords ? b->nwords : 4;
    while (ncap <= wi) {
        if (ncap > UINT32_MAX / 2)
            return -1;
        ncap *= 2;
    }
    nw = (uint64_t*)dyn_nat_realloc(b->words, (size_t)ncap * sizeof(uint64_t));
    if (!nw)
        return -1;
    memset(nw + b->nwords, 0, (size_t)(ncap - b->nwords) * sizeof(uint64_t));
    b->words = nw;
    b->nwords = ncap;
    return 0;
}

static JSValue dyn_bitset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t nbits = 0;

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], 1u << 30, &nbits))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_nat_malloc(sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    b->words = NULL;
    b->nwords = 0;
    if (nbits && dyn_bitset_ensure(b, (nbits - 1) >> 6)) {
        dyn_bitset_free(b);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_bitset_class_id, b, dyn_bitset_free);
}

static JSValue dyn_bitset_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if ((i >> 6) >= DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "bit index too large (max %u)",
            (unsigned)(DYN_MAX_CAPACITY << 6) - 1);
    if (dyn_bitset_ensure(b, i >> 6))
        return JS_ThrowOutOfMemory(ctx);
    b->words[i >> 6] |= (uint64_t)1 << (i & 63);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_bitset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if ((i >> 6) < b->nwords)
        b->words[i >> 6] &= ~((uint64_t)1 << (i & 63));
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_bitset_flip(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    if ((i >> 6) >= DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "bit index too large (max %u)",
            (unsigned)(DYN_MAX_CAPACITY << 6) - 1);
    if (dyn_bitset_ensure(b, i >> 6))
        return JS_ThrowOutOfMemory(ctx);
    b->words[i >> 6] ^= (uint64_t)1 << (i & 63);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_bitset_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t i;
    int bit;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    bit = (i >> 6) < b->nwords && (b->words[i >> 6] & ((uint64_t)1 << (i & 63))) != 0;
    return JS_NewBool(ctx, bit);
}

static JSValue dyn_bitset_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_bitset_t* b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    uint64_t total = 0;
    uint32_t i;
    if (!b)
        return JS_EXCEPTION;
    for (i = 0; i < b->nwords; i++)
        total += (uint64_t)__builtin_popcountll(b->words[i]);
    return JS_NewInt64(ctx, (int64_t)total);
}

static JSValue dyn_bitset_next_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    uint32_t from, wi;
    uint64_t w;
    (void)argc;
    if (JS_ToUint32(ctx, &from, argv[0]))
        return JS_EXCEPTION;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    wi = from >> 6;
    if (wi >= b->nwords)
        return JS_NewInt64(ctx, -1);
    w = b->words[wi] & (~(uint64_t)0 << (from & 63));
    for (;;) {
        if (w)
            return JS_NewInt64(ctx,
                (int64_t)wi * 64 + __builtin_ctzll(w));
        if (++wi >= b->nwords)
            return JS_NewInt64(ctx, -1);
        w = b->words[wi];
    }
}

static JSValue dyn_bitset_bulk(JSContext* ctx, JSValueConst this_val,
    JSValueConst other_val, int op)
{
    dyn_bitset_t *a, *o;
    uint32_t i;

    o = (dyn_bitset_t*)dyn_plain_get(ctx, other_val, dyn_bitset_class_id);
    if (!o)
        return JS_EXCEPTION;
    a = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!a)
        return JS_EXCEPTION;
    if (op != 0 && o->nwords > a->nwords) {
        if (dyn_bitset_ensure(a, o->nwords - 1))
            return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < a->nwords; i++) {
        uint64_t ow = i < o->nwords ? o->words[i] : 0;
        if (op == 0)
            a->words[i] &= ow;
        else if (op == 1)
            a->words[i] |= ow;
        else
            a->words[i] ^= ow;
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_bitset_and(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dyn_bitset_bulk(ctx, t, argv[0], 0);
}
static JSValue dyn_bitset_or(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dyn_bitset_bulk(ctx, t, argv[0], 1);
}
static JSValue dyn_bitset_xor(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    (void)argc;
    return dyn_bitset_bulk(ctx, t, argv[0], 2);
}

static JSValue dyn_bitset_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bitset_t* b;
    JSValue arr;
    uint32_t wi, out = 0;
    (void)argc;
    (void)argv;
    b = (dyn_bitset_t*)dyn_plain_get(ctx, this_val, dyn_bitset_class_id);
    if (!b)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (wi = 0; wi < b->nwords; wi++) {
        uint64_t w = b->words[wi];
        while (w) {
            int bit = __builtin_ctzll(w);
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    JS_NewInt64(ctx, (int64_t)wi * 64 + bit),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            w &= w - 1;
        }
    }
    return arr;
}

static const JSCFunctionListEntry dyn_bitset_proto[] = {
    JS_CFUNC_DEF("set", 1, dyn_bitset_set),
    JS_CFUNC_DEF("clear", 1, dyn_bitset_clear),
    JS_CFUNC_DEF("flip", 1, dyn_bitset_flip),
    JS_CFUNC_DEF("get", 1, dyn_bitset_get),
    JS_CFUNC_DEF("nextSet", 1, dyn_bitset_next_set),
    JS_CFUNC_DEF("and", 1, dyn_bitset_and),
    JS_CFUNC_DEF("or", 1, dyn_bitset_or),
    JS_CFUNC_DEF("xor", 1, dyn_bitset_xor),
    JS_CFUNC_DEF("toArray", 0, dyn_bitset_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("count", dyn_bitset_count, NULL),
};

typedef struct {
    uint32_t* parent;
    uint8_t* rank;
    uint32_t n;
    uint32_t sets;
} dyn_uf_t;

static void dyn_uf_free(void* native)
{
    dyn_uf_t* u = (dyn_uf_t*)native;
    if (!u)
        return;
    dyn_nat_free(u->parent);
    dyn_nat_free(u->rank);
    dyn_nat_free(u);
}

static JSClassID dyn_uf_class_id;
DYN_FINALIZER(dyn_uf)
static const JSClassDef dyn_uf_class = {
    "UnionFind",
    .finalizer = dyn_uf_finalizer,
};

#define DYN_UF_MAX_N 67108864u

static JSValue dyn_uf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_uf_t* u;
    uint32_t n = 0, i;

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], DYN_UF_MAX_N, &n))
        return JS_EXCEPTION;
    u = (dyn_uf_t*)dyn_nat_malloc(sizeof(*u));
    if (!u)
        return JS_ThrowOutOfMemory(ctx);
    u->n = n;
    u->sets = n;
    u->parent = n ? (uint32_t*)dyn_nat_malloc((size_t)n * sizeof(uint32_t)) : NULL;
    u->rank = n ? (uint8_t*)dyn_nat_calloc(n, 1) : NULL;
    if (n && (!u->parent || !u->rank)) {
        dyn_uf_free(u);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        u->parent[i] = i;
    return dyn_plain_wrap(ctx, new_target, dyn_uf_class_id, u, dyn_uf_free);
}

static uint32_t dyn_uf_find(dyn_uf_t* u, uint32_t x)
{
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]];
        x = u->parent[x];
    }
    return x;
}

static JSValue dyn_uf_find_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_uf_t* u;
    uint32_t x;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    u = (dyn_uf_t*)dyn_plain_get(ctx, this_val, dyn_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    return JS_NewInt64(ctx, (int64_t)dyn_uf_find(u, x));
}

static JSValue dyn_uf_union(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_uf_t* u;
    uint32_t x, y, rx, ry;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &y))
        return JS_EXCEPTION;
    u = (dyn_uf_t*)dyn_plain_get(ctx, this_val, dyn_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n || y >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    rx = dyn_uf_find(u, x);
    ry = dyn_uf_find(u, y);
    if (rx == ry)
        return JS_NewBool(ctx, 0);
    if (u->rank[rx] < u->rank[ry]) {
        uint32_t t = rx;
        rx = ry;
        ry = t;
    }
    u->parent[ry] = rx;
    if (u->rank[rx] == u->rank[ry])
        u->rank[rx]++;
    u->sets--;
    return JS_NewBool(ctx, 1);
}

static JSValue dyn_uf_connected(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_uf_t* u;
    uint32_t x, y;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &x))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &y))
        return JS_EXCEPTION;
    u = (dyn_uf_t*)dyn_plain_get(ctx, this_val, dyn_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    if (x >= u->n || y >= u->n)
        return JS_ThrowRangeError(ctx, "element out of range");
    return JS_NewBool(ctx, dyn_uf_find(u, x) == dyn_uf_find(u, y));
}

static JSValue dyn_uf_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_uf_t* u = (dyn_uf_t*)dyn_plain_get(ctx, this_val, dyn_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)u->sets);
}

static JSValue dyn_uf_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_uf_t* u = (dyn_uf_t*)dyn_plain_get(ctx, this_val, dyn_uf_class_id);
    if (!u)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)u->n);
}

static const JSCFunctionListEntry dyn_uf_proto[] = {
    JS_CFUNC_DEF("find", 1, dyn_uf_find_m),
    JS_CFUNC_DEF("union", 2, dyn_uf_union),
    JS_CFUNC_DEF("connected", 2, dyn_uf_connected),
    JS_CGETSET_DEF("count", dyn_uf_count, NULL),
    JS_CGETSET_DEF("size", dyn_uf_size, NULL),
};

typedef struct {
    JSRuntime* rt;
    JSValue* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
} dyn_deque_t;

static inline uint32_t dyn_deque_idx(const dyn_deque_t* d, uint32_t i)
{
    return (d->head + i) & (d->cap - 1);
}

static void dyn_deque_free(void* native)
{
    dyn_deque_t* d = (dyn_deque_t*)native;
    uint32_t i, idx;
    if (!d)
        return;
    for (i = 0, idx = d->head; i < d->count; i++) {
        JS_FreeValueRT(d->rt, d->buf[idx]);
        idx = (idx + 1) & (d->cap - 1);
    }
    dyn_nat_free(d->buf);
    dyn_nat_free(d);
}

static JSClassID dyn_deque_class_id;
DYN_FINALIZER(dyn_deque)
static void dyn_deque_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_deque_t* d = JS_GetOpaque(val, dyn_deque_class_id);
    uint32_t i, idx;
    if (!d)
        return;
    for (i = 0, idx = d->head; i < d->count; i++) {
        JS_MarkValue(rt, d->buf[idx], mark_func);
        idx = (idx + 1) & (d->cap - 1);
    }
}
static const JSClassDef dyn_deque_class = {
    "Deque",
    .finalizer = dyn_deque_finalizer,
    .gc_mark = dyn_deque_gc_mark,
};

static int dyn_deque_grow(dyn_deque_t* d)
{
    uint32_t ncap = d->cap ? d->cap * 2 : 8;
    JSValue* nb;

    if (d->count < d->cap)
        return 0;
    if (d->cap > UINT32_MAX / 2 || d->cap > UINT32_MAX / (uint32_t)sizeof(JSValue) / 2)
        return -1;
    nb = (JSValue*)dyn_nat_malloc((size_t)ncap * sizeof(JSValue));
    if (!nb)
        return -1;
    if (d->count) {
        uint32_t first = d->cap - d->head;
        if (first > d->count)
            first = d->count;
        memcpy(nb, d->buf + d->head, (size_t)first * sizeof(JSValue));
        if (first < d->count)
            memcpy(nb + first, d->buf,
                (size_t)(d->count - first) * sizeof(JSValue));
    }
    dyn_nat_free(d->buf);
    d->buf = nb;
    d->cap = ncap;
    d->head = 0;
    return 0;
}

static void dyn_deque_maybe_shrink(dyn_deque_t* d)
{
    uint32_t ncap;
    JSValue* nb;

    if (d->cap < 64 || d->count > d->cap / 4)
        return;
    ncap = d->cap / 2;
    nb = (JSValue*)dyn_nat_malloc((size_t)ncap * sizeof(JSValue));
    if (!nb)
        return;
    if (d->count) {
        uint32_t first = d->cap - d->head;
        if (first > d->count)
            first = d->count;
        memcpy(nb, d->buf + d->head, (size_t)first * sizeof(JSValue));
        if (first < d->count)
            memcpy(nb + first, d->buf,
                (size_t)(d->count - first) * sizeof(JSValue));
    }
    dyn_nat_free(d->buf);
    d->buf = nb;
    d->cap = ncap;
    d->head = 0;
}

static JSValue dyn_deque_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_nat_malloc(sizeof(*d));
    if (!d)
        return JS_ThrowOutOfMemory(ctx);
    d->rt = JS_GetRuntime(ctx);
    d->buf = NULL;
    d->cap = d->head = d->count = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_deque_class_id, d, dyn_deque_free);
}

static JSValue dyn_deque_push_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (dyn_deque_grow(d))
        return JS_ThrowOutOfMemory(ctx);
    d->buf[dyn_deque_idx(d, d->count)] = JS_DupValue(ctx, argv[0]);
    d->count++;
    return JS_NewInt64(ctx, (int64_t)d->count);
}

static JSValue dyn_deque_push_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (dyn_deque_grow(d))
        return JS_ThrowOutOfMemory(ctx);
    d->head = (d->head - 1) & (d->cap - 1);
    d->buf[d->head] = JS_DupValue(ctx, argv[0]);
    d->count++;
    return JS_NewInt64(ctx, (int64_t)d->count);
}

static JSValue dyn_deque_pop_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    JSValue v;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->count == 0)
        return JS_UNDEFINED;
    v = d->buf[d->head];
    d->head = (d->head + 1) & (d->cap - 1);
    d->count--;
    dyn_deque_maybe_shrink(d);
    return v;
}

static JSValue dyn_deque_pop_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->count == 0)
        return JS_UNDEFINED;
    d->count--;
    {
        JSValue v = d->buf[dyn_deque_idx(d, d->count)];
        dyn_deque_maybe_shrink(d);
        return v;
    }
}

static JSValue dyn_deque_peek_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, d->buf[d->head]);
}

static JSValue dyn_deque_peek_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (d->count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, d->buf[dyn_deque_idx(d, d->count - 1)]);
}

static JSValue dyn_deque_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    if (i >= d->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, d->buf[dyn_deque_idx(d, i)]);
}

static JSValue dyn_deque_length(JSContext* ctx, JSValueConst this_val)
{
    dyn_deque_t* d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)d->count);
}

static JSValue dyn_deque_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_deque_t* d;
    JSValue arr;
    uint32_t i;
    (void)argc;
    (void)argv;
    d = (dyn_deque_t*)dyn_plain_get(ctx, this_val, dyn_deque_class_id);
    if (!d)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < d->count; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, d->buf[dyn_deque_idx(d, i)]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static const JSCFunctionListEntry dyn_deque_proto[] = {
    JS_CFUNC_DEF("pushBack", 1, dyn_deque_push_back),
    JS_CFUNC_DEF("pushFront", 1, dyn_deque_push_front),
    JS_CFUNC_DEF("popFront", 0, dyn_deque_pop_front),
    JS_CFUNC_DEF("popBack", 0, dyn_deque_pop_back),
    JS_CFUNC_DEF("peekFront", 0, dyn_deque_peek_front),
    JS_CFUNC_DEF("peekBack", 0, dyn_deque_peek_back),
    JS_CFUNC_DEF("get", 1, dyn_deque_get),
    JS_CFUNC_DEF("toArray", 0, dyn_deque_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("length", dyn_deque_length, NULL),
};

typedef struct {
    double* tree;
    uint32_t n;
} dyn_fenwick_t;

static void dyn_fenwick_free(void* native)
{
    dyn_fenwick_t* f = (dyn_fenwick_t*)native;
    if (!f)
        return;
    dyn_nat_free(f->tree);
    dyn_nat_free(f);
}

static JSClassID dyn_fenwick_class_id;
DYN_FINALIZER(dyn_fenwick)
static const JSClassDef dyn_fenwick_class = {
    "Fenwick",
    .finalizer = dyn_fenwick_finalizer,
};

static JSValue dyn_fenwick_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_fenwick_t* f;
    uint32_t n = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && dyn_size_arg(ctx, argv[0], DYN_UF_MAX_N, &n))
        return JS_EXCEPTION;
    f = (dyn_fenwick_t*)dyn_nat_malloc(sizeof(*f));
    if (!f)
        return JS_ThrowOutOfMemory(ctx);
    f->n = n;
    f->tree = (double*)dyn_nat_calloc((size_t)n + 1, sizeof(double));
    if (!f->tree) {
        dyn_nat_free(f);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_fenwick_class_id, f, dyn_fenwick_free);
}

static JSValue dyn_fenwick_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_fenwick_t* f;
    uint32_t i;
    double delta;
    uint32_t idx;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &delta, argv[1]))
        return JS_EXCEPTION;
    f = (dyn_fenwick_t*)dyn_plain_get(ctx, this_val, dyn_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (i >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    for (idx = i + 1; idx <= f->n; idx += idx & (~idx + 1))
        f->tree[idx] += delta;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_fenwick_prefix_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_fenwick_t* f;
    uint32_t i;
    uint32_t idx;
    double s = 0.0;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    f = (dyn_fenwick_t*)dyn_plain_get(ctx, this_val, dyn_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (i >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    for (idx = i + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    return JS_NewFloat64(ctx, s);
}

static JSValue dyn_fenwick_range_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_fenwick_t* f;
    uint32_t lo, hi;
    uint32_t idx;
    double s = 0.0;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &lo))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &hi))
        return JS_EXCEPTION;
    f = (dyn_fenwick_t*)dyn_plain_get(ctx, this_val, dyn_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    if (lo >= f->n || hi >= f->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    if (lo > hi)
        return JS_NewFloat64(ctx, 0.0);
    for (idx = hi + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    for (idx = lo; idx > 0; idx -= idx & (~idx + 1))
        s -= f->tree[idx];
    return JS_NewFloat64(ctx, s);
}

static JSValue dyn_fenwick_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_fenwick_t* f = (dyn_fenwick_t*)dyn_plain_get(ctx, this_val, dyn_fenwick_class_id);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)f->n);
}

static const JSCFunctionListEntry dyn_fenwick_proto[] = {
    JS_CFUNC_DEF("update", 2, dyn_fenwick_update),
    JS_CFUNC_DEF("prefixSum", 1, dyn_fenwick_prefix_sum),
    JS_CFUNC_DEF("rangeQuery", 2, dyn_fenwick_range_query),
    JS_CGETSET_DEF("size", dyn_fenwick_size, NULL),
};

typedef struct {
    JSRuntime* rt;
    JSValue* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
} dyn_ringbuf_t;

static inline uint32_t dyn_ringbuf_idx(const dyn_ringbuf_t* r, uint32_t i)
{
    uint32_t idx = r->head + i;
    return idx >= r->cap ? idx - r->cap : idx;
}

static void dyn_ringbuf_free(void* native)
{
    dyn_ringbuf_t* r = (dyn_ringbuf_t*)native;
    uint32_t i, idx;
    if (!r)
        return;
    for (i = 0, idx = r->head; i < r->count; i++) {
        JS_FreeValueRT(r->rt, r->buf[idx]);
        if (++idx == r->cap)
            idx = 0;
    }
    dyn_nat_free(r->buf);
    dyn_nat_free(r);
}

static JSClassID dyn_ringbuf_class_id;
DYN_FINALIZER(dyn_ringbuf)
static void dyn_ringbuf_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_ringbuf_t* r = JS_GetOpaque(val, dyn_ringbuf_class_id);
    uint32_t i, idx;
    if (!r)
        return;
    for (i = 0, idx = r->head; i < r->count; i++) {
        JS_MarkValue(rt, r->buf[idx], mark_func);
        if (++idx == r->cap)
            idx = 0;
    }
}
static const JSClassDef dyn_ringbuf_class = {
    "RingBuffer",
    .finalizer = dyn_ringbuf_finalizer,
    .gc_mark = dyn_ringbuf_gc_mark,
};

static JSValue dyn_ringbuf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_ringbuf_t* r;
    uint32_t cap;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "capacity required");
    if (dyn_size_arg(ctx, argv[0], DYN_MAX_CAPACITY, &cap))
        return JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be > 0");
    r = (dyn_ringbuf_t*)dyn_nat_malloc(sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->rt = JS_GetRuntime(ctx);
    r->buf = (JSValue*)dyn_nat_malloc((size_t)cap * sizeof(JSValue));
    if (!r->buf) {
        dyn_nat_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->cap = cap;
    r->head = r->count = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_ringbuf_class_id, r, dyn_ringbuf_free);
}

static JSValue dyn_ringbuf_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ringbuf_t* r;
    (void)argc;
    r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (r->count < r->cap) {
        r->buf[dyn_ringbuf_idx(r, r->count)] = JS_DupValue(ctx, argv[0]);
        r->count++;
    } else {
        JS_FreeValueRT(r->rt, r->buf[r->head]);
        r->buf[r->head] = JS_DupValue(ctx, argv[0]);
        if (++r->head == r->cap)
            r->head = 0;
    }
    return JS_NewInt64(ctx, (int64_t)r->count);
}

static JSValue dyn_ringbuf_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ringbuf_t* r;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    if (i >= r->count)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, r->buf[dyn_ringbuf_idx(r, i)]);
}

static JSValue dyn_ringbuf_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ringbuf_t* r;
    JSValue arr;
    uint32_t i;
    (void)argc;
    (void)argv;
    r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < r->count; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, r->buf[dyn_ringbuf_idx(r, i)]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_ringbuf_length(JSContext* ctx, JSValueConst this_val)
{
    dyn_ringbuf_t* r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->count);
}

static JSValue dyn_ringbuf_capacity(JSContext* ctx, JSValueConst this_val)
{
    dyn_ringbuf_t* r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->cap);
}

static JSValue dyn_ringbuf_full(JSContext* ctx, JSValueConst this_val)
{
    dyn_ringbuf_t* r = (dyn_ringbuf_t*)dyn_plain_get(ctx, this_val, dyn_ringbuf_class_id);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r->count == r->cap);
}

static const JSCFunctionListEntry dyn_ringbuf_proto[] = {
    JS_CFUNC_DEF("push", 1, dyn_ringbuf_push),
    JS_CFUNC_DEF("get", 1, dyn_ringbuf_get),
    JS_CFUNC_DEF("toArray", 0, dyn_ringbuf_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("length", dyn_ringbuf_length, NULL),
    JS_CGETSET_DEF("capacity", dyn_ringbuf_capacity, NULL),
    JS_CGETSET_DEF("full", dyn_ringbuf_full, NULL),
};

enum { SEG_SUM = 0,
    SEG_MIN = 1,
    SEG_MAX = 2 };

typedef struct {
    double* tree;
    uint32_t n;
    int op;
} dyn_segtree_t;

static double dyn_seg_identity(int op)
{
    if (op == SEG_MIN)
        return DYN_INFINITY;
    if (op == SEG_MAX)
        return -DYN_INFINITY;
    return 0.0;
}

static void dyn_segtree_free(void* native)
{
    dyn_segtree_t* s = (dyn_segtree_t*)native;
    if (!s)
        return;
    dyn_nat_free(s->tree);
    dyn_nat_free(s);
}

static JSClassID dyn_segtree_class_id;
DYN_FINALIZER(dyn_segtree)
static const JSClassDef dyn_segtree_class = {
    "SegTree",
    .finalizer = dyn_segtree_finalizer,
};

#define DYN_SEG_MAX_N 67108864u

static JSValue dyn_segtree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_segtree_t* s;
    uint32_t n = 0, i;
    int op = SEG_SUM;
    double idv;

    {
        double nd;
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "size required");
        if (JS_ToFloat64(ctx, &nd, argv[0]))
            return JS_EXCEPTION;
        if (!(nd >= 1) || nd > (double)DYN_SEG_MAX_N || nd != (double)(uint32_t)nd)
            return JS_ThrowRangeError(ctx,
                "size must be an integer in 1..%u", (unsigned)DYN_SEG_MAX_N);
        n = (uint32_t)nd;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char* o = JS_ToCString(ctx, argv[1]);
        if (!o)
            return JS_EXCEPTION;
        if (!strcmp(o, "sum"))
            op = SEG_SUM;
        else if (!strcmp(o, "min"))
            op = SEG_MIN;
        else if (!strcmp(o, "max"))
            op = SEG_MAX;
        else {
            JS_FreeCString(ctx, o);
            return JS_ThrowRangeError(ctx, "op must be sum|min|max");
        }
        JS_FreeCString(ctx, o);
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "size must be > 0");
    s = (dyn_segtree_t*)dyn_nat_malloc(sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->n = n;
    s->op = op;
    s->tree = (double*)dyn_nat_malloc((size_t)2 * n * sizeof(double));
    if (!s->tree) {
        dyn_nat_free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    idv = dyn_seg_identity(op);
    for (i = 0; i < 2 * n; i++)
        s->tree[i] = idv;
    return dyn_plain_wrap(ctx, new_target, dyn_segtree_class_id, s, dyn_segtree_free);
}

static JSValue dyn_segtree_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_segtree_t* s;
    uint32_t i;
    double v;
    uint32_t pos;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v, argv[1]))
        return JS_EXCEPTION;
    s = (dyn_segtree_t*)dyn_plain_get(ctx, this_val, dyn_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (i >= s->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    pos = i + s->n;
    s->tree[pos] = v;
    switch (s->op) {
    case SEG_MIN:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a < b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    case SEG_MAX:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a > b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    default:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            s->tree[pos] = s->tree[2 * pos] + s->tree[2 * pos + 1];
            if (pos == 1)
                break;
        }
        break;
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_segtree_range_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_segtree_t* s;
    uint32_t lo, hi;
    uint32_t l, r;
    double res;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &lo))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &hi))
        return JS_EXCEPTION;
    s = (dyn_segtree_t*)dyn_plain_get(ctx, this_val, dyn_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (lo >= s->n || hi >= s->n)
        return JS_ThrowRangeError(ctx, "index out of range");
    if (lo > hi)
        return JS_NewFloat64(ctx, dyn_seg_identity(s->op));
    res = dyn_seg_identity(s->op);
    l = lo + s->n;
    r = hi + 1 + s->n;
    switch (s->op) {
    case SEG_MIN:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res < x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res < x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    case SEG_MAX:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res > x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res > x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    default:
        while (l < r) {
            if (l & 1)
                res += s->tree[l++];
            if (r & 1)
                res += s->tree[--r];
            l >>= 1;
            r >>= 1;
        }
        break;
    }
    return JS_NewFloat64(ctx, res);
}

static JSValue dyn_segtree_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_segtree_t* s = (dyn_segtree_t*)dyn_plain_get(ctx, this_val, dyn_segtree_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)s->n);
}

static const JSCFunctionListEntry dyn_segtree_proto[] = {
    JS_CFUNC_DEF("update", 2, dyn_segtree_update),
    JS_CFUNC_DEF("rangeQuery", 2, dyn_segtree_range_query),
    JS_CGETSET_DEF("size", dyn_segtree_size, NULL),
};

typedef struct {
    uint64_t* words;
    uint32_t nbits;
    uint32_t nwords;
    uint32_t k;
} dyn_bloom_t;

static void dyn_bloom_free(void* native)
{
    dyn_bloom_t* b = (dyn_bloom_t*)native;
    if (!b)
        return;
    dyn_nat_free(b->words);
    dyn_nat_free(b);
}

static JSClassID dyn_bloom_class_id;
DYN_FINALIZER(dyn_bloom)
static const JSClassDef dyn_bloom_class = {
    "BloomFilter",
    .finalizer = dyn_bloom_finalizer,
};

static JSValue dyn_bloom_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_bloom_t* b;
    uint32_t bits, k = 3;
    if (argc < 1 || dyn_size_arg(ctx, argv[0], 1u << 30, &bits))
        return argc < 1 ? JS_ThrowTypeError(ctx, "bits required") : JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (bits == 0)
        return JS_ThrowRangeError(ctx, "bits must be > 0");
    if (k == 0)
        k = 1;
    if (k > 32)
        k = 32;
    b = (dyn_bloom_t*)dyn_nat_malloc(sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    b->nbits = bits;
    b->k = k;
    b->nwords = (bits + 63) >> 6;
    b->words = (uint64_t*)dyn_nat_calloc(b->nwords, sizeof(uint64_t));
    if (!b->words) {
        dyn_nat_free(b);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_bloom_class_id, b, dyn_bloom_free);
}

static atomic_uint_least64_t dyn_strhash_seed = 0x9E3779B97F4A7C15ULL;

static uint64_t dyn_str_seed(void)
{
    static atomic_flag seeded = ATOMIC_FLAG_INIT;
    uint64_t s[2] = { 0, 0 };
    if (!atomic_flag_test_and_set_explicit(&seeded, memory_order_relaxed)) {
        if (dyn_os_entropy(s, sizeof s) == 0)
            atomic_store_explicit(&dyn_strhash_seed, s[0],
                memory_order_relaxed);
    }
    return atomic_load_explicit(&dyn_strhash_seed, memory_order_relaxed);
}

static void dyn_bloom_hashes(const char* s, size_t n, uint64_t* h1, uint64_t* h2)
{
    uint64_t a = 14695981039346656037ULL ^ dyn_str_seed();
    uint64_t c = ~a;
    size_t i;
    for (i = 0; i < n; i++) {
        a ^= (uint64_t)(unsigned char)s[i];
        a *= 1099511628211ULL;
        c = (c ^ (uint64_t)(unsigned char)s[i]) * 1099511628211ULL + 0x9E3779B9ULL;
    }
    *h1 = a;
    *h2 = c | 1;
}

static inline void dyn_bloom_probe_init(uint32_t m, uint64_t h1, uint64_t h2,
    uint32_t* bit, uint32_t* step,
    uint32_t* wmod)
{
    uint64_t w;
    *bit = (uint32_t)(h1 % m);
    *step = (uint32_t)(h2 % m);
    if (!*step)
        *step = 1;
    w = (UINT64_MAX % m) + 1;
    *wmod = (uint32_t)(w == m ? 0 : w);
}

static inline uint32_t dyn_bloom_probe_next(uint32_t m, uint64_t* acc,
    uint64_t h2, uint32_t bit,
    uint32_t step, uint32_t wmod)
{
    uint64_t next = *acc + h2;
    uint64_t r = (uint64_t)bit + step;
    if (r >= m)
        r -= m;
    if (next < *acc) {
        r = r + m - wmod;
        if (r >= m)
            r -= m;
    }
    *acc = next;
    return (uint32_t)r;
}

static JSValue dyn_bloom_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bloom_t* b;
    const char* key;
    size_t klen;
    uint64_t h1, h2;
    uint32_t i;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    b = (dyn_bloom_t*)dyn_plain_get(ctx, this_val, dyn_bloom_class_id);
    if (!b) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    dyn_bloom_hashes(key, klen, &h1, &h2);
    JS_FreeCString(ctx, key);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        dyn_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
            bit = dyn_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_bloom_may_contain(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_bloom_t* b;
    const char* key;
    size_t klen;
    uint64_t h1, h2;
    uint32_t i;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    b = (dyn_bloom_t*)dyn_plain_get(ctx, this_val, dyn_bloom_class_id);
    if (!b) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    dyn_bloom_hashes(key, klen, &h1, &h2);
    JS_FreeCString(ctx, key);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return JS_NewBool(ctx, 0);
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        dyn_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return JS_NewBool(ctx, 0);
            bit = dyn_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
    return JS_NewBool(ctx, 1);
}

static JSValue dyn_bloom_bits(JSContext* ctx, JSValueConst this_val)
{
    dyn_bloom_t* b = (dyn_bloom_t*)dyn_plain_get(ctx, this_val, dyn_bloom_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)b->nbits);
}

static JSValue dyn_bloom_hashes_getter(JSContext* ctx, JSValueConst this_val)
{
    dyn_bloom_t* b = (dyn_bloom_t*)dyn_plain_get(ctx, this_val, dyn_bloom_class_id);
    if (!b)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)b->k);
}

static const JSCFunctionListEntry dyn_bloom_proto[] = {
    JS_CFUNC_DEF("add", 1, dyn_bloom_add),
    JS_CFUNC_DEF("mayContain", 1, dyn_bloom_may_contain),
    JS_CGETSET_DEF("bits", dyn_bloom_bits, NULL),
    JS_CGETSET_DEF("hashes", dyn_bloom_hashes_getter, NULL),
};

typedef struct DynTrieNode {
    struct DynTrieNode* child;
    struct DynTrieNode* sibling;
    uint8_t* tail;
    uint32_t tail_len;
    uint8_t byte;
    uint8_t is_end;
} DynTrieNode;

typedef struct DynTrieChunk {
    struct DynTrieChunk* next;
    uint32_t used, cap;
} DynTrieChunk;

typedef struct DynTrieBytes {
    struct DynTrieBytes* next;
    uint32_t used, cap;
} DynTrieBytes;

#define DYN_TRIE_BYTES(b) ((uint8_t*)((b) + 1))
#define DYN_TRIE_BYTES_MIN 512

#define DYN_TRIE_CHUNK_NODES(c) ((DynTrieNode*)((c) + 1))
#define DYN_TRIE_CHUNK_MAX 8192

typedef struct {
    DynTrieNode* root;
    DynTrieChunk* chunks;
    DynTrieBytes* bytes;
    uint32_t count;
} dyn_trie_t;

static uint8_t* dyn_trie_bytes(dyn_trie_t* t, const uint8_t* src, uint32_t n)
{
    DynTrieBytes* b = t->bytes;
    uint8_t* p;
    if (!n)
        return NULL;
    if (!b || b->cap - b->used < n) {
        uint32_t cap = n > DYN_TRIE_BYTES_MIN ? n : DYN_TRIE_BYTES_MIN;
        b = (DynTrieBytes*)dyn_nat_malloc(sizeof(*b) + cap);
        if (!b)
            return NULL;
        b->next = t->bytes;
        b->used = 0;
        b->cap = cap;
        t->bytes = b;
    }
    p = DYN_TRIE_BYTES(b) + b->used;
    memcpy(p, src, n);
    b->used += n;
    return p;
}

static DynTrieNode* dyn_trie_node(dyn_trie_t* t)
{
    DynTrieChunk* c = t->chunks;
    DynTrieNode* n;
    if (!c || c->used == c->cap) {
        uint32_t cap = c ? (c->cap < DYN_TRIE_CHUNK_MAX ? c->cap * 2
                                                        : DYN_TRIE_CHUNK_MAX)
                         : 32;
        c = (DynTrieChunk*)dyn_nat_malloc(sizeof(*c) + (size_t)cap * sizeof(DynTrieNode));
        if (!c)
            return NULL;
        c->next = t->chunks;
        c->used = 0;
        c->cap = cap;
        t->chunks = c;
    }
    n = DYN_TRIE_CHUNK_NODES(c) + c->used++;
    memset(n, 0, sizeof(*n));
    return n;
}

static void dyn_trie_free(void* native)
{
    dyn_trie_t* t = (dyn_trie_t*)native;
    DynTrieChunk *c, *nx;
    if (!t)
        return;
    for (c = t->chunks; c; c = nx) {
        nx = c->next;
        dyn_nat_free(c);
    }
    {
        DynTrieBytes *b, *bn;
        for (b = t->bytes; b; b = bn) {
            bn = b->next;
            dyn_nat_free(b);
        }
    }
    dyn_nat_free(t);
}

static JSClassID dyn_trie_class_id;
DYN_FINALIZER(dyn_trie)
static const JSClassDef dyn_trie_class = {
    "Trie",
    .finalizer = dyn_trie_finalizer,
};

static JSValue dyn_trie_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    (void)argc;
    (void)argv;
    t = (dyn_trie_t*)dyn_nat_malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    t->chunks = NULL;
    t->bytes = NULL;
    t->count = 0;
    t->root = dyn_trie_node(t);
    if (!t->root) {
        dyn_nat_free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_trie_class_id, t, dyn_trie_free);
}

static DynTrieNode* dyn_trie_child(DynTrieNode* node, uint8_t b)
{
    DynTrieNode* c;
    for (c = node->child; c; c = c->sibling)
        if (c->byte == b)
            return c;
    return NULL;
}

static DynTrieNode* dyn_trie_walk(dyn_trie_t* t, const char* key, size_t klen,
    int* on_tail)
{
    DynTrieNode* node = t->root;
    size_t i = 0;
    *on_tail = 0;
    while (i < klen) {
        if (node->tail) {
            size_t rem = klen - i;
            if (rem == node->tail_len && memcmp(node->tail, key + i, rem) == 0) {
                *on_tail = 1;
                return node;
            }
            return NULL;
        }
        node = dyn_trie_child(node, (uint8_t)key[i]);
        if (!node)
            return NULL;
        i++;
    }
    return node;
}

static int dyn_trie_expand(dyn_trie_t* t, DynTrieNode** pnode, uint32_t keep)
{
    DynTrieNode *node = *pnode, *c;
    uint8_t* tail = node->tail;
    uint32_t tlen = node->tail_len, j;
    DynTrieNode** tmp;
    uint32_t nneed = keep + (keep < tlen ? 1 : 0);

    tmp = (DynTrieNode**)dyn_nat_malloc((size_t)nneed * sizeof(*tmp));
    if (!tmp)
        return -1;
    for (j = 0; j < nneed; j++) {
        tmp[j] = dyn_trie_node(t);
        if (!tmp[j]) {
            dyn_nat_free(tmp);
            return -1;
        }
    }
    node->tail = NULL;
    node->tail_len = 0;
    for (j = 0; j < keep; j++) {
        c = tmp[j];
        c->byte = tail[j];
        c->sibling = node->child;
        node->child = c;
        node = c;
    }
    if (keep == tlen) {
        node->is_end = 1;
    } else {
        c = tmp[keep];
        c->byte = tail[keep];
        c->sibling = node->child;
        node->child = c;
        if (keep + 1 == tlen) {
            c->is_end = 1;
        } else {
            c->tail = tail + keep + 1;
            c->tail_len = tlen - keep - 1;
        }
    }
    *pnode = node;
    dyn_nat_free(tmp);
    return 0;
}

static int dyn_trie_insert_at(dyn_trie_t* t, DynTrieNode* node,
    const char* key, size_t klen, size_t from,
    DynTrieNode** path, size_t* plen)
{
    size_t i = from;

    for (;;) {
        if (path)
            *plen = i;
        if (i == klen) {
            if (!node->is_end) {
                node->is_end = 1;
                t->count++;
            }
            return 0;
        }
        if (node->tail) {
            uint32_t rem = (uint32_t)(klen - i), lim, L = 0;
            lim = rem < node->tail_len ? rem : node->tail_len;
            while (L < lim && node->tail[L] == (uint8_t)key[i + L])
                L++;
            if (L == node->tail_len && L == rem)
                return 0;
            if (path) {
                DynTrieNode* walk = node;
                uint32_t j;
                if (dyn_trie_expand(t, &node, L) < 0)
                    return -1;
                for (j = 0; j < L; j++) {
                    walk = walk->child;
                    path[i + j] = walk;
                }
                i += L;
                continue;
            }
            if (dyn_trie_expand(t, &node, L) < 0)
                return -1;
            i += L;
            continue;
        }
        if (!node->child) {
            node->tail = dyn_trie_bytes(t, (const uint8_t*)key + i,
                (uint32_t)(klen - i));
            if (!node->tail)
                return -1;
            node->tail_len = (uint32_t)(klen - i);
            t->count++;
            return 0;
        }
        {
            DynTrieNode* c = dyn_trie_child(node, (uint8_t)key[i]);
            if (!c) {
                c = dyn_trie_node(t);
                if (!c)
                    return -1;
                c->byte = (uint8_t)key[i];
                c->sibling = node->child;
                node->child = c;
            }
            if (path)
                path[i] = c;
            node = c;
            i++;
        }
    }
}

static int dyn_trie_insert_bytes(dyn_trie_t* t, const char* key, size_t klen)
{
    return dyn_trie_insert_at(t, t->root, key, klen, 0, NULL, NULL);
}

static JSValue dyn_trie_insert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    int rc;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    rc = dyn_trie_insert_bytes(t, key, klen);
    JS_FreeCString(ctx, key);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_trie_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    DynTrieNode* node;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    {
        int on_tail;
        node = dyn_trie_walk(t, key, klen, &on_tail);
        JS_FreeCString(ctx, key);
        return JS_NewBool(ctx, node && (on_tail || node->is_end));
    }
}

static JSValue dyn_trie_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    DynTrieNode* node;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    {
        int on_tail;
        node = dyn_trie_walk(t, key, klen, &on_tail);
        JS_FreeCString(ctx, key);
        if (node && on_tail) {
            node->tail = NULL;
            node->tail_len = 0;
            t->count--;
            return JS_NewBool(ctx, 1);
        }
        if (node && node->is_end) {
            node->is_end = 0;
            t->count--;
            return JS_NewBool(ctx, 1);
        }
        return JS_NewBool(ctx, 0);
    }
}

static int dyn_trie_emit(JSContext* ctx, JSValue arr, uint32_t* out, char** kb,
    size_t* kbcap, size_t base, const uint8_t* extra,
    size_t n)
{
    size_t need = base + n;
    if (need + 1 > *kbcap) {
        size_t nc = *kbcap ? *kbcap : 64;
        char* nk;
        while (nc <= need)
            nc *= 2;
        nk = (char*)dyn_nat_realloc(*kb, nc);
        if (!nk)
            return -1;
        *kb = nk;
        *kbcap = nc;
    }
    if (n)
        memcpy(*kb + base, extra, n);
    return JS_DefinePropertyValueUint32(ctx, arr, (*out)++,
        JS_NewStringLen(ctx, *kb, need),
        JS_PROP_C_W_E);
}

static JSValue dyn_trie_keys_with_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* pfx;
    size_t plen;
    DynTrieNode* base;
    JSValue arr;
    uint32_t out = 0;
    char* kb = NULL;
    size_t kbcap = 0;
    DynTrieNode** nstk = NULL;
    size_t *dstk = NULL, slen = 0, scap = 0;
    (void)argc;

    pfx = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pfx)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, pfx);
        return JS_EXCEPTION;
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeCString(ctx, pfx);
        return arr;
    }
    {
        size_t i = 0;
        base = t->root;
        while (i < plen) {
            if (base->tail) {
                size_t rem = plen - i;
                if (rem <= base->tail_len && memcmp(base->tail, pfx + i, rem) == 0) {
                    kbcap = i + base->tail_len + 1;
                    kb = (char*)dyn_nat_malloc(kbcap);
                    if (!kb)
                        goto fail;
                    memcpy(kb, pfx, i);
                    if (dyn_trie_emit(ctx, arr, &out, &kb, &kbcap, i,
                            base->tail, base->tail_len)
                        < 0)
                        goto fail;
                }
                goto done;
            }
            base = dyn_trie_child(base, (uint8_t)pfx[i]);
            if (!base)
                goto done;
            i++;
        }
    }

    kbcap = plen + 16;
    kb = (char*)dyn_nat_malloc(kbcap);
    if (!kb)
        goto fail;
    if (plen)
        memcpy(kb, pfx, plen);

    if (base->is_end) {
        if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                JS_NewStringLen(ctx, pfx, plen), JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    if (base->tail && dyn_trie_emit(ctx, arr, &out, &kb, &kbcap, plen, base->tail, base->tail_len) < 0)
        goto fail;
    if (base->child) {
        if (scap < 8) {
            void* nn = dyn_nat_realloc(nstk, 8 * sizeof(*nstk));
            void* dd = dyn_nat_realloc(dstk, 8 * sizeof(*dstk));
            if (!nn || !dd) {
                dyn_nat_free(nn ? nn : nstk);
                dyn_nat_free(dd ? dd : dstk);
                nstk = NULL;
                dstk = NULL;
                goto fail;
            }
            nstk = (DynTrieNode**)nn;
            dstk = (size_t*)dd;
            scap = 8;
        }
        nstk[slen] = base->child;
        dstk[slen] = 0;
        slen++;
    }
    while (slen) {
        DynTrieNode* n = nstk[--slen];
        size_t depth = dstk[slen];
        if (plen + depth >= kbcap) {
            size_t nc = kbcap * 2;
            char* nk;
            while (nc <= plen + depth)
                nc *= 2;
            nk = (char*)dyn_nat_realloc(kb, nc);
            if (!nk)
                goto fail;
            kb = nk;
            kbcap = nc;
        }
        kb[plen + depth] = (char)n->byte;
        if (n->is_end && dyn_trie_emit(ctx, arr, &out, &kb, &kbcap, plen + depth + 1, NULL, 0) < 0)
            goto fail;
        if (n->tail && dyn_trie_emit(ctx, arr, &out, &kb, &kbcap, plen + depth + 1, n->tail, n->tail_len) < 0)
            goto fail;
        if (slen + 2 > scap) {
            size_t nc = scap * 2;
            void* nn = dyn_nat_realloc(nstk, nc * sizeof(*nstk));
            void* dd = dyn_nat_realloc(dstk, nc * sizeof(*dstk));
            if (!nn || !dd) {
                if (nn)
                    nstk = nn;
                if (dd)
                    dstk = dd;
                goto fail;
            }
            nstk = (DynTrieNode**)nn;
            dstk = (size_t*)dd;
            scap = nc;
        }
        if (n->sibling) {
            nstk[slen] = n->sibling;
            dstk[slen] = depth;
            slen++;
        }
        if (n->child) {
            nstk[slen] = n->child;
            dstk[slen] = depth + 1;
            slen++;
        }
    }
done:
    JS_FreeCString(ctx, pfx);
    dyn_nat_free(kb);
    dyn_nat_free(nstk);
    dyn_nat_free(dstk);
    return arr;
fail:
    JS_FreeCString(ctx, pfx);
    dyn_nat_free(kb);
    dyn_nat_free(nstk);
    dyn_nat_free(dstk);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue dyn_trie_longest_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* s;
    size_t slen, i, best = 0;
    DynTrieNode* node;
    JSValue out;
    (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    node = t->root;
    if (node->is_end)
        best = 0;
    for (i = 0; i < slen; i++) {
        if (node->tail) {
            size_t rem = slen - i;
            if (rem >= node->tail_len && memcmp(node->tail, s + i, node->tail_len) == 0)
                best = i + node->tail_len;
            break;
        }
        node = dyn_trie_child(node, (uint8_t)s[i]);
        if (!node)
            break;
        if (node->is_end)
            best = i + 1;
    }
    if (node && node->tail && slen >= i && best < slen) {
        size_t rem = slen - i;
        if (rem == node->tail_len && memcmp(node->tail, s + i, rem) == 0)
            best = slen;
    }
    out = JS_NewStringLen(ctx, s, best);
    JS_FreeCString(ctx, s);
    return out;
}

static JSValue dyn_trie_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_trie_t* t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dyn_trie_class_id);
    if (!t)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)t->count);
}

static const JSCFunctionListEntry dyn_trie_proto[] = {
    JS_CFUNC_DEF("insert", 1, dyn_trie_insert),
    JS_CFUNC_DEF("has", 1, dyn_trie_has),
    JS_CFUNC_DEF("delete", 1, dyn_trie_delete),
    JS_CFUNC_DEF("keysWithPrefix", 1, dyn_trie_keys_with_prefix),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_trie),
    JS_CFUNC_DEF("longestPrefix", 1, dyn_trie_longest_prefix),
    JS_CGETSET_DEF("size", dyn_trie_size, NULL),
};

typedef struct DynLRUNode {
    struct DynLRUNode *prev, *next;
    struct DynLRUNode* hnext;
    JSValue value;
    uint64_t hash;
    uint64_t expires;
    size_t klen;
    char key[];
} DynLRUNode;

typedef struct {
    JSRuntime* rt;
    DynLRUNode** buckets;
    DynLRUNode *head, *tail;
    JSValue on_evict;
    uint64_t default_ttl;
    uint64_t hits, misses, evictions, expired;
    uint32_t n_buckets;
    uint32_t count;
    uint32_t cap;
    int busy;
} dyn_lru_t;

static uint64_t dyn_lru_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t dyn_lru_expiry(uint64_t ttl)
{
    uint64_t now = dyn_lru_now_ms();
    if (!ttl)
        return 0;
    if (ttl > (uint64_t)INT64_MAX - now)
        return (uint64_t)INT64_MAX;
    return now + ttl;
}

static int dyn_lru_expired(const DynLRUNode* n, uint64_t now)
{
    return n->expires != 0 && now >= n->expires;
}

static uint64_t dyn_str_hash(const char* s, size_t n)
{
    uint64_t h = 14695981039346656037ULL ^ dyn_str_seed();
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void dyn_lru_free(void* native)
{
    dyn_lru_t* L = (dyn_lru_t*)native;
    DynLRUNode *n, *next;
    if (!L)
        return;
    for (n = L->head; n; n = next) {
        next = n->next;
        JS_FreeValueRT(L->rt, n->value);
        dyn_nat_free(n);
    }
    dyn_nat_free(L->buckets);
    JS_FreeValueRT(L->rt, L->on_evict);
    dyn_nat_free(L);
}

static JSClassID dyn_lru_class_id;
DYN_FINALIZER(dyn_lru)
static void dyn_lru_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_lru_t* L = JS_GetOpaque(val, dyn_lru_class_id);
    DynLRUNode* n;
    if (!L)
        return;
    for (n = L->head; n; n = n->next)
        JS_MarkValue(rt, n->value, mark_func);
    JS_MarkValue(rt, L->on_evict, mark_func);
}
static const JSClassDef dyn_lru_class = {
    "LRU",
    .finalizer = dyn_lru_finalizer,
    .gc_mark = dyn_lru_gc_mark,
};

static int dyn_lru_opts_check(JSContext* ctx, JSValueConst o,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const dyn_lru_keys[] = { "ttlMs", "onEvict" };

static JSValue dyn_lru_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    uint32_t cap, nb;
    if (argc < 1 || JS_ToUint32(ctx, &cap, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "capacity required")
                        : JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be > 0");
    if (cap > DYN_MAX_CAPACITY)
        return JS_ThrowRangeError(ctx, "capacity must be <= %u",
            DYN_MAX_CAPACITY);
    L = (dyn_lru_t*)dyn_nat_malloc(sizeof(*L));
    if (!L)
        return JS_ThrowOutOfMemory(ctx);
    nb = 16;
    L->rt = JS_GetRuntime(ctx);
    L->n_buckets = nb;
    L->count = 0;
    L->cap = cap;
    L->head = L->tail = NULL;
    L->on_evict = JS_UNDEFINED;
    L->default_ttl = 0;
    L->hits = L->misses = L->evictions = L->expired = 0;
    L->busy = 0;
    L->buckets = (DynLRUNode**)dyn_nat_calloc(nb, sizeof(*L->buckets));
    if (!L->buckets) {
        dyn_nat_free(L);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_lru_opts_check(ctx, argv[1], dyn_lru_keys,
                countof(dyn_lru_keys))) {
            dyn_nat_free(L->buckets);
            dyn_nat_free(L);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "ttlMs");
        int64_t ms = 0;
        if (JS_IsException(v))
            goto opt_fail;
        if (!JS_IsUndefined(v)) {
            int rc = JS_ToInt64(ctx, &ms, v);
            JS_FreeValue(ctx, v);
            if (rc < 0)
                goto opt_fail;
            if (ms <= 0) {
                dyn_nat_free(L->buckets);
                dyn_nat_free(L);
                return JS_ThrowRangeError(ctx, "LRU: ttlMs must be > 0");
            }
            L->default_ttl = (uint64_t)ms;
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
            dyn_nat_free(L->buckets);
            dyn_nat_free(L);
            return JS_ThrowTypeError(ctx, "LRU: onEvict must be a function");
        } else {
            L->on_evict = v;
        }
    }
    return dyn_plain_wrap(ctx, new_target, dyn_lru_class_id, L, dyn_lru_free);
opt_fail:
    JS_FreeValue(ctx, L->on_evict);
    dyn_nat_free(L->buckets);
    dyn_nat_free(L);
    return JS_EXCEPTION;
}

static void dyn_lru_rehash(dyn_lru_t* L, uint32_t nb)
{
    DynLRUNode** nbk = (DynLRUNode**)dyn_nat_calloc(nb, sizeof(*nbk));
    DynLRUNode* n;
    if (!nbk)
        return;
    for (n = L->head; n; n = n->next) {
        uint32_t b = (uint32_t)(n->hash & (nb - 1));
        n->hnext = nbk[b];
        nbk[b] = n;
    }
    dyn_nat_free(L->buckets);
    L->buckets = nbk;
    L->n_buckets = nb;
}

static DynLRUNode* dyn_lru_find(dyn_lru_t* L, const char* key, size_t klen,
    uint64_t h)
{
    DynLRUNode* n;
    for (n = L->buckets[h & (L->n_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && n->klen == klen && !memcmp(n->key, key, klen))
            return n;
    return NULL;
}

static void dyn_lru_unlink(dyn_lru_t* L, DynLRUNode* n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        L->head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        L->tail = n->prev;
}

static void dyn_lru_push_head(dyn_lru_t* L, DynLRUNode* n)
{
    n->prev = NULL;
    n->next = L->head;
    if (L->head)
        L->head->prev = n;
    else
        L->tail = n;
    L->head = n;
}

static void dyn_lru_bucket_remove(dyn_lru_t* L, DynLRUNode* n)
{
    DynLRUNode** pp = &L->buckets[n->hash & (L->n_buckets - 1)];
    while (*pp && *pp != n)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = n->hnext;
}

static DynLRUNode* dyn_lru_detach(dyn_lru_t* L, DynLRUNode* n)
{
    dyn_lru_unlink(L, n);
    dyn_lru_bucket_remove(L, n);
    L->count--;
    return n;
}

static int dyn_lru_notify(JSContext* ctx, dyn_lru_t* L, DynLRUNode* n)
{
    JSValue args[2], r;
    int rc = 0;

    if (JS_IsUndefined(L->on_evict) || L->busy) {
        JS_FreeValueRT(L->rt, n->value);
        dyn_nat_free(n);
        return 0;
    }
    args[0] = JS_NewStringLen(ctx, n->key, n->klen);
    args[1] = n->value;
    dyn_nat_free(n);
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

static JSValue dyn_lru_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    const char* key;
    size_t klen;
    uint64_t h;
    DynLRUNode* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    h = dyn_str_hash(key, klen);
    n = dyn_lru_find(L, key, klen, h);
    JS_FreeCString(ctx, key);
    if (n && dyn_lru_expired(n, dyn_lru_now_ms())) {
        dyn_lru_detach(L, n);
        L->expired++;
        if (dyn_lru_notify(ctx, L, n) < 0)
            return JS_EXCEPTION;
        n = NULL;
    }
    if (!n) {
        L->misses++;
        return JS_UNDEFINED;
    }
    L->hits++;
    if (L->head != n) {
        dyn_lru_unlink(L, n);
        dyn_lru_push_head(L, n);
    }
    return JS_DupValue(ctx, n->value);
}

static JSValue dyn_lru_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_lru_t* L;
    const char* key;
    size_t klen;
    uint64_t h, ttl;
    DynLRUNode* n;
    uint32_t b;
    int64_t ms = 0;

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
    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    if (L->busy) {
        JS_FreeCString(ctx, key);
        return JS_ThrowTypeError(ctx, "LRU: onEvict must not modify the cache "
                                      "it is being called from");
    }
    ttl = L->default_ttl;
    if (magic == 1)
        ttl = (uint64_t)ms;
    h = dyn_str_hash(key, klen);
    n = dyn_lru_find(L, key, klen, h);
    if (n) {
        JSValue nv = JS_DupValue(ctx, argv[1]);
        JS_FreeValueRT(L->rt, n->value);
        n->value = nv;
        n->expires = dyn_lru_expiry(ttl);
        if (L->head != n) {
            dyn_lru_unlink(L, n);
            dyn_lru_push_head(L, n);
        }
        JS_FreeCString(ctx, key);
        return JS_DupValue(ctx, this_val);
    }
    n = (DynLRUNode*)dyn_nat_malloc(sizeof(*n) + klen + 1);
    if (!n) {
        JS_FreeCString(ctx, key);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(n->key, key, klen);
    n->key[klen] = '\0';
    n->klen = klen;
    n->hash = h;
    n->expires = dyn_lru_expiry(ttl);
    n->value = JS_DupValue(ctx, argv[1]);
    JS_FreeCString(ctx, key);
    if (L->count + 1 > L->n_buckets && L->n_buckets < DYN_MAX_CAPACITY)
        dyn_lru_rehash(L, L->n_buckets * 2);
    b = h & (L->n_buckets - 1);
    n->hnext = L->buckets[b];
    L->buckets[b] = n;
    dyn_lru_push_head(L, n);
    L->count++;
    if (L->count > L->cap) {
        DynLRUNode* victim = dyn_lru_detach(L, L->tail);
        L->evictions++;
        if (dyn_lru_notify(ctx, L, victim) < 0)
            return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_lru_purge_expired(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    DynLRUNode *n, *next, *dead = NULL;
    uint64_t now;
    uint32_t removed = 0;
    (void)argc;
    (void)argv;

    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    if (L->busy)
        return JS_ThrowTypeError(ctx, "LRU: onEvict must not modify the cache "
                                      "it is being called from");
    now = dyn_lru_now_ms();
    for (n = L->head; n; n = next) {
        next = n->next;
        if (!dyn_lru_expired(n, now))
            continue;
        dyn_lru_detach(L, n);
        n->next = dead;
        dead = n;
        removed++;
    }
    L->expired += removed;
    for (n = dead; n; n = next) {
        next = n->next;
        if (dyn_lru_notify(ctx, L, n) < 0) {
            for (n = next; n; n = next) {
                next = n->next;
                JS_FreeValueRT(L->rt, n->value);
                dyn_nat_free(n);
            }
            return JS_EXCEPTION;
        }
    }
    return JS_NewUint32(ctx, removed);
}

static JSValue dyn_lru_get_stats(JSContext* ctx, JSValueConst this_val)
{
    dyn_lru_t* L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
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
    STAT("hits", L->hits)
    STAT("misses", L->misses)
    STAT("evictions", L->evictions)
    STAT("expired", L->expired)
    STAT("size", L->count)
    STAT("capacity", L->cap)
#undef STAT
    return o;
}

static JSValue dyn_lru_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    const char* key;
    size_t klen;
    DynLRUNode* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    n = dyn_lru_find(L, key, klen, dyn_str_hash(key, klen));
    JS_FreeCString(ctx, key);
    if (n && dyn_lru_expired(n, dyn_lru_now_ms()))
        n = NULL;
    return JS_NewBool(ctx, n != NULL);
}

static JSValue dyn_lru_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    const char* key;
    size_t klen;
    DynLRUNode* n;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    n = dyn_lru_find(L, key, klen, dyn_str_hash(key, klen));
    JS_FreeCString(ctx, key);
    if (!n)
        return JS_NewBool(ctx, 0);
    dyn_lru_unlink(L, n);
    dyn_lru_bucket_remove(L, n);
    JS_FreeValueRT(L->rt, n->value);
    dyn_nat_free(n);
    L->count--;
    return JS_NewBool(ctx, 1);
}

static JSValue dyn_lru_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_lru_t* L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)L->count);
}

static JSValue dyn_lru_capacity(JSContext* ctx, JSValueConst this_val)
{
    dyn_lru_t* L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)L->cap);
}

static JSValue dyn_lru_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_lru_t* L;
    DynLRUNode* n;
    JSValue arr;
    uint64_t now;
    uint32_t out = 0;
    (void)argc;
    (void)argv;

    L = (dyn_lru_t*)dyn_plain_get(ctx, this_val, dyn_lru_class_id);
    if (!L)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    now = dyn_lru_now_ms();
    for (n = L->head; n; n = n->next) {
        JSValue pair;
        if (dyn_lru_expired(n, now))
            continue;
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, pair, 0,
                JS_NewStringLen(ctx, n->key, n->klen), JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, pair, 1,
                   JS_DupValue(ctx, n->value), JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, pair);
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

static JSValue dyn_lru_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static const JSCFunctionListEntry dyn_lru_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_lru_get),
    JS_CFUNC_MAGIC_DEF("put", 2, dyn_lru_put, 0),
    JS_CFUNC_MAGIC_DEF("set", 2, dyn_lru_put, 0),
    JS_CFUNC_MAGIC_DEF("setWithTTL", 3, dyn_lru_put, 1),
    JS_CFUNC_DEF("purgeExpired", 0, dyn_lru_purge_expired),
    JS_CGETSET_DEF("stats", dyn_lru_get_stats, NULL),
    JS_CFUNC_DEF("has", 1, dyn_lru_has),
    JS_CFUNC_DEF("delete", 1, dyn_lru_delete),
    JS_CFUNC_DEF("entries", 0, dyn_lru_entries),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_lru_iterator),
    JS_CGETSET_DEF("size", dyn_lru_size, NULL),
    JS_CGETSET_DEF("capacity", dyn_lru_capacity, NULL),
};

typedef char dyn_ds2_cell_fits[sizeof(JSValue) <= sizeof(dyn_cell_t) ? 1 : -1];

static dyn_cell_t ds2_cell(JSValue v)
{
    dyn_cell_t c;
    memset(&c, 0, sizeof(c));
    memcpy(&c, &v, sizeof(v));
    return c;
}

static JSValue ds2_value(const dyn_cell_t* c)
{
    JSValue v;
    memcpy(&v, c, sizeof(v));
    return v;
}

static void ds2_free_cell(void* ud, const dyn_cell_t* c)
{
    JS_FreeValueRT((JSRuntime*)ud, ds2_value(c));
}

static void ds2_dup_cell(void* ud, const dyn_cell_t* in, dyn_cell_t* out)
{
    *out = ds2_cell(JS_DupValueRT((JSRuntime*)ud, ds2_value(in)));
}

typedef struct {
    JSRuntime* rt;
    dyn_btree_t* b;
} dyn_skip_t;

static dyn_skip_t* skip_new(JSRuntime* rt)
{
    dyn_skip_t* s = (dyn_skip_t*)dyn_nat_malloc(sizeof(*s));
    if (!s)
        return NULL;
    s->b = dyn_btree_new();
    if (!s->b) {
        dyn_nat_free(s);
        return NULL;
    }
    s->rt = rt;
    return s;
}

static void skip_free(void* native)
{
    dyn_skip_t* s = (dyn_skip_t*)native;
    if (!s)
        return;
    dyn_btree_free(s->b, ds2_free_cell, s->rt);
    dyn_nat_free(s);
}

static int skip_insert(JSContext* ctx, dyn_skip_t* s, double key,
    JSValueConst val)
{
    JSValue dup = JS_DupValue(ctx, val);
    dyn_cell_t cell = ds2_cell(dup), old;
    int replaced = 0;

    if (dyn_btree_set(s->b, key, &cell, &old, &replaced) < 0) {
        JS_FreeValue(ctx, dup);
        return -1;
    }
    if (replaced)
        ds2_free_cell(s->rt, &old);
    return 0;
}

static int skip_delete(dyn_skip_t* s, double key)
{
    dyn_cell_t out;
    if (!dyn_btree_del(s->b, key, &out))
        return 0;
    ds2_free_cell(s->rt, &out);
    return 1;
}

static int skip_floor(dyn_skip_t* s, double q, double* out)
{
    return dyn_btree_floor(s->b, q, out);
}

static int skip_ceil(dyn_skip_t* s, double q, double* out)
{
    return dyn_btree_ceil(s->b, q, out);
}

static JSClassID dyn_sortedset_class_id;
static void dyn_sortedset_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    skip_free(JS_GetOpaque(val, dyn_sortedset_class_id));
}
static const JSClassDef dyn_sortedset_class = {
    "SortedSet",
    .finalizer = dyn_sortedset_finalizer,
};

static JSValue dyn_sortedset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = skip_new(JS_GetRuntime(ctx));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, new_target, dyn_sortedset_class_id, s, skip_free);
}

static dyn_skip_t* sortedset_get(JSContext* ctx, JSValueConst t)
{
    return (dyn_skip_t*)dyn_plain_get(ctx, t, dyn_sortedset_class_id);
}

static JSValue dyn_sortedset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_ThrowRangeError(ctx, "NaN is not a valid key");
    if (skip_insert(ctx, s, x, JS_UNDEFINED))
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_sortedset_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(x) && dyn_btree_get(s->b, x) != NULL);
}

static JSValue dyn_sortedset_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(x) && skip_delete(s, x));
}

static JSValue skip_end_or_undef(JSContext* ctx, dyn_skip_t* s, int last)
{
    double k;
    int ok = last ? dyn_btree_last(s->b, &k) : dyn_btree_first(s->b, &k);
    return ok ? JS_NewFloat64(ctx, k) : JS_UNDEFINED;
}

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int failed;
} skip_keys_state;

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

static JSValue skip_keys_array(JSContext* ctx, dyn_skip_t* s)
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

static JSValue dyn_sortedset_first(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 0);
}

static JSValue dyn_sortedset_last(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 1);
}

static JSValue dyn_sortedset_floor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_floor(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dyn_sortedset_ceil(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_ceil(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dyn_sortedset_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double lo, hi;
    dyn_btree_iter it;
    JSValue arr;
    uint32_t out = 0;
    (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[1]))
        return JS_EXCEPTION;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (isnan(lo) || isnan(hi) || lo > hi)
        return arr;
    if (dyn_btree_iter_seek(s->b, lo, &it)) {
        double k;
        const dyn_cell_t* v;
        do {
            if (!dyn_btree_iter_get(&it, &k, &v) || k > hi)
                break;
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    JS_NewFloat64(ctx, k), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        } while (dyn_btree_iter_next(&it));
    }
    return arr;
}

static JSValue dyn_sortedset_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_keys_array(ctx, s);
}

static JSValue dyn_sortedset_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_skip_t* s = sortedset_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(s->b));
}

static const JSCFunctionListEntry dyn_sortedset_proto[] = {
    JS_CFUNC_DEF("add", 1, dyn_sortedset_add),
    JS_CFUNC_DEF("has", 1, dyn_sortedset_has),
    JS_CFUNC_DEF("delete", 1, dyn_sortedset_delete),
    JS_CFUNC_DEF("first", 0, dyn_sortedset_first),
    JS_CFUNC_DEF("last", 0, dyn_sortedset_last),
    JS_CFUNC_DEF("floor", 1, dyn_sortedset_floor),
    JS_CFUNC_DEF("ceil", 1, dyn_sortedset_ceil),
    JS_CFUNC_DEF("rangeQuery", 2, dyn_sortedset_range),
    JS_CFUNC_DEF("toArray", 0, dyn_sortedset_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_toarray),
    JS_CGETSET_DEF("size", dyn_sortedset_size, NULL),
};

static JSClassID dyn_sortedmap_class_id;
static void dyn_sortedmap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    skip_free(JS_GetOpaque(val, dyn_sortedmap_class_id));
}
static void dyn_sortedmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_skip_t* s = JS_GetOpaque(val, dyn_sortedmap_class_id);
    dyn_btree_iter it;
    if (!s)
        return;
    if (dyn_btree_iter_begin(s->b, &it)) {
        double k;
        const dyn_cell_t* v;
        do {
            if (dyn_btree_iter_get(&it, &k, &v))
                JS_MarkValue(rt, ds2_value(v), mark_func);
        } while (dyn_btree_iter_next(&it));
    }
}
static const JSClassDef dyn_sortedmap_class = {
    "SortedMap",
    .finalizer = dyn_sortedmap_finalizer,
    .gc_mark = dyn_sortedmap_gc_mark,
};

static JSValue dyn_sortedmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = skip_new(JS_GetRuntime(ctx));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, new_target, dyn_sortedmap_class_id, s, skip_free);
}

static dyn_skip_t* sortedmap_get(JSContext* ctx, JSValueConst t)
{
    return (dyn_skip_t*)dyn_plain_get(ctx, t, dyn_sortedmap_class_id);
}

static JSValue dyn_sortedmap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(k))
        return JS_ThrowRangeError(ctx, "NaN is not a valid key");
    if (skip_insert(ctx, s, k, argv[1]))
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_sortedmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double k;
    const dyn_cell_t* v;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(k))
        return JS_UNDEFINED;
    v = dyn_btree_get(s->b, k);
    return v ? JS_DupValue(ctx, ds2_value(v)) : JS_UNDEFINED;
}

static JSValue dyn_sortedmap_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(k) && dyn_btree_get(s->b, k) != NULL);
}

static JSValue dyn_sortedmap_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double k;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, !isnan(k) && skip_delete(s, k));
}

static JSValue dyn_sortedmap_first_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 0);
}

static JSValue dyn_sortedmap_last_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_end_or_undef(ctx, s, 1);
}

static JSValue dyn_sortedmap_floor_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_floor(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dyn_sortedmap_ceil_key(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double x, out;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_UNDEFINED;
    return skip_ceil(s, x, &out) ? JS_NewFloat64(ctx, out) : JS_UNDEFINED;
}

static JSValue dyn_sortedmap_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    double lo, hi;
    dyn_btree_iter it;
    JSValue arr;
    uint32_t out = 0;
    (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[1]))
        return JS_EXCEPTION;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (isnan(lo) || isnan(hi) || lo > hi)
        return arr;
    if (dyn_btree_iter_seek(s->b, lo, &it)) {
        double k;
        const dyn_cell_t* v;
        do {
            if (!dyn_btree_iter_get(&it, &k, &v) || k > hi)
                break;
            JSValue pair = JS_NewArray(ctx);
            if (JS_IsException(pair) || JS_DefinePropertyValueUint32(ctx, pair, 0, JS_NewFloat64(ctx, k), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, pair, 1, JS_DupValue(ctx, ds2_value(v)), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, arr, out++, pair, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        } while (dyn_btree_iter_next(&it));
    }
    return arr;
}

static JSValue dyn_sortedmap_keys(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return skip_keys_array(ctx, s);
}

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int failed;
} skip_pairs_state;

static int skip_pairs_leaf(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n)
{
    skip_pairs_state* st = (skip_pairs_state*)ud;
    uint32_t j;
    for (j = 0; j < n; j++) {
        JSValue pair = JS_NewArray(st->ctx);
        if (JS_IsException(pair)) {
            st->failed = 1;
            return 0;
        }
        if (JS_DefinePropertyValueUint32(st->ctx, pair, 0,
                JS_NewFloat64(st->ctx, keys[j]), JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(st->ctx, pair, 1,
                   JS_DupValue(st->ctx, ds2_value(&vals[j])), JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(st->ctx, pair);
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

static JSValue dyn_sortedmap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_skip_t* s;
    skip_pairs_state st;
    (void)argc;
    (void)argv;
    s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    st.ctx = ctx;
    st.i = 0;
    st.failed = 0;
    st.arr = JS_NewArray(ctx);
    if (JS_IsException(st.arr))
        return st.arr;
    dyn_btree_walk_leaves(s->b, skip_pairs_leaf, &st);
    if (st.failed == 2)
        return JS_EXCEPTION;
    if (st.failed) {
        JS_FreeValue(ctx, st.arr);
        return JS_EXCEPTION;
    }
    return st.arr;
}

static JSValue dyn_sortedmap_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_skip_t* s = sortedmap_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(s->b));
}

static const JSCFunctionListEntry dyn_sortedmap_proto[] = {
    JS_CFUNC_DEF("set", 2, dyn_sortedmap_set),
    JS_CFUNC_DEF("get", 1, dyn_sortedmap_get),
    JS_CFUNC_DEF("has", 1, dyn_sortedmap_has),
    JS_CFUNC_DEF("delete", 1, dyn_sortedmap_delete),
    JS_CFUNC_DEF("firstKey", 0, dyn_sortedmap_first_key),
    JS_CFUNC_DEF("lastKey", 0, dyn_sortedmap_last_key),
    JS_CFUNC_DEF("floorKey", 1, dyn_sortedmap_floor_key),
    JS_CFUNC_DEF("ceilKey", 1, dyn_sortedmap_ceil_key),
    JS_CFUNC_DEF("rangeQuery", 2, dyn_sortedmap_range),
    JS_CFUNC_DEF("keys", 0, dyn_sortedmap_keys),
    JS_CFUNC_DEF("entries", 0, dyn_sortedmap_entries),
    JS_CGETSET_DEF("size", dyn_sortedmap_size, NULL),
};

typedef struct {
    JSRuntime* rt;
    JSValue cmp;
    JSValue* items;
    uint32_t count;
    uint32_t cap;
    int busy;
    int natural;
} dyn_heap_t;

_Static_assert(sizeof(dyn_heap_t) == 6 * sizeof(void*),
    "dyn_heap_t regained padding: keep the two ints adjacent");

static int dyn_heap_reserve(dyn_heap_t* h, uint32_t need)
{
    uint32_t ncap;
    JSValue* ni;

    if (need <= h->cap)
        return 0;
    if (need > UINT32_MAX / (uint32_t)sizeof(JSValue))
        return -1;
    ncap = h->cap ? h->cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2 || ncap > UINT32_MAX / (uint32_t)sizeof(JSValue) / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    ni = (JSValue*)dyn_nat_realloc(h->items, (size_t)ncap * sizeof(JSValue));
    if (!ni)
        return -1;
    h->items = ni;
    h->cap = ncap;
    return 0;
}

static void dyn_heap_free(void* native)
{
    dyn_heap_t* h = (dyn_heap_t*)native;
    uint32_t i;

    if (!h)
        return;
    for (i = 0; i < h->count; i++)
        JS_FreeValueRT(h->rt, h->items[i]);
    JS_FreeValueRT(h->rt, h->cmp);
    dyn_nat_free(h->items);
    dyn_nat_free(h);
}

static JSClassID dyn_heap_class_id;

static void dyn_heap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_heap_free(JS_GetOpaque(val, dyn_heap_class_id));
}
static void dyn_heap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_heap_t* h = JS_GetOpaque(val, dyn_heap_class_id);
    uint32_t i;
    if (!h)
        return;
    JS_MarkValue(rt, h->cmp, mark_func);
    for (i = 0; i < h->count; i++)
        JS_MarkValue(rt, h->items[i], mark_func);
}
static const JSClassDef dyn_heap_class = {
    "Heap",
    .finalizer = dyn_heap_finalizer,
    .gc_mark = dyn_heap_gc_mark,
};

static JSValue dyn_heap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_heap_t* h;

    int natural = (argc < 1 || JS_IsUndefined(argv[0]));

    if (!natural && !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Heap(comparator) requires a comparator function");
    h = (dyn_heap_t*)dyn_nat_malloc(sizeof(*h));
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    h->rt = JS_GetRuntime(ctx);
    h->natural = natural;
    h->cmp = natural ? JS_UNDEFINED : JS_DupValue(ctx, argv[0]);
    h->items = NULL;
    h->count = 0;
    h->cap = 0;
    h->busy = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_heap_class_id, h, dyn_heap_free);
}

static int dyn_heap_cmp_call(JSContext* ctx, JSValueConst cmp,
    JSValueConst a, JSValueConst b, int* err)
{
    JSValueConst args[2];
    JSValue dcmp, da, db, r;
    double d;

    if (*err)
        return 0;
    dcmp = JS_DupValue(ctx, cmp);
    da = JS_DupValue(ctx, a);
    db = JS_DupValue(ctx, b);
    args[0] = da;
    args[1] = db;
    r = JS_Call(ctx, dcmp, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, dcmp);
    JS_FreeValue(ctx, da);
    JS_FreeValue(ctx, db);
    if (JS_IsException(r)) {
        *err = 1;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, r)) {
        JS_FreeValue(ctx, r);
        *err = 1;
        return 0;
    }
    JS_FreeValue(ctx, r);
    if (d < 0)
        return -1;
    if (d > 0)
        return 1;
    return 0;
}

static int dyn_heap_number(JSValueConst v, double* out)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) {
        *out = (double)JS_VALUE_GET_INT(v);
        return 1;
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        *out = JS_VALUE_GET_FLOAT64(v);
        return 1;
    }
    return 0;
}

static int dyn_heap_cmp_nat(JSContext* ctx, JSValueConst a, JSValueConst b,
    int* err)
{
    double x, y;
    if (!dyn_heap_number(a, &x) || !dyn_heap_number(b, &y)) {
        JS_ThrowTypeError(ctx, "a Heap built without a comparator orders "
                               "numbers only; pass a comparator for other values");
        *err = 1;
        return 0;
    }
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int dyn_heap_sift_up(JSContext* ctx, JSValueConst this_val,
    dyn_heap_t* h, uint32_t idx)
{
    int err = 0;

    if (h->natural) {
        while (idx > 0) {
            uint32_t parent = (idx - 1) / 2;
            JSValue tmp;
            if (dyn_heap_cmp_nat(ctx, h->items[idx], h->items[parent], &err) >= 0)
                break;
            if (err)
                return -1;
            tmp = h->items[idx];
            h->items[idx] = h->items[parent];
            h->items[parent] = tmp;
            idx = parent;
        }
        return err ? -1 : 0;
    }

    h->busy = 1;
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        int c = dyn_heap_cmp_call(ctx, h->cmp, h->items[idx], h->items[parent],
            &err);
        h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
        if (!h)
            return -1;
        if (err) {
            h->busy = 0;
            return -1;
        }
        if (c >= 0)
            break;
        {
            JSValue tmp = h->items[idx];
            h->items[idx] = h->items[parent];
            h->items[parent] = tmp;
        }
        idx = parent;
    }
    h->busy = 0;
    return 0;
}

static int dyn_heap_sift_down(JSContext* ctx, JSValueConst this_val,
    dyn_heap_t* h, uint32_t idx)
{
    int err = 0;

    if (h->natural) {
        for (;;) {
            uint32_t l = 2 * idx + 1, r = 2 * idx + 2, smallest = idx;
            JSValue tmp;
            if (l < h->count && dyn_heap_cmp_nat(ctx, h->items[l], h->items[smallest], &err) < 0)
                smallest = l;
            if (err)
                return -1;
            if (r < h->count && dyn_heap_cmp_nat(ctx, h->items[r], h->items[smallest], &err) < 0)
                smallest = r;
            if (err)
                return -1;
            if (smallest == idx)
                return 0;
            tmp = h->items[idx];
            h->items[idx] = h->items[smallest];
            h->items[smallest] = tmp;
            idx = smallest;
        }
    }

    h->busy = 1;
    for (;;) {
        uint32_t l = 2 * idx + 1, r = 2 * idx + 2, smallest = idx;
        int c;

        if (l < h->count) {
            c = dyn_heap_cmp_call(ctx, h->cmp, h->items[l], h->items[smallest],
                &err);
            h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
            if (!h)
                return -1;
            if (err) {
                h->busy = 0;
                return -1;
            }
            if (c < 0)
                smallest = l;
        }
        if (r < h->count) {
            c = dyn_heap_cmp_call(ctx, h->cmp, h->items[r], h->items[smallest],
                &err);
            h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
            if (!h)
                return -1;
            if (err) {
                h->busy = 0;
                return -1;
            }
            if (c < 0)
                smallest = r;
        }
        if (smallest == idx)
            break;
        {
            JSValue tmp = h->items[idx];
            h->items[idx] = h->items[smallest];
            h->items[smallest] = tmp;
        }
        idx = smallest;
    }
    h->busy = 0;
    return 0;
}

static JSValue dyn_heap_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_heap_t* h;
    uint32_t idx;

    (void)argc;
    h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (h->natural) {
        double d;
        if (!dyn_heap_number(argv[0], &d))
            return JS_ThrowTypeError(ctx,
                "a Heap built without a comparator orders numbers only; "
                "pass a comparator for other values");
    }
    if (dyn_heap_reserve(h, h->count + 1))
        return JS_ThrowOutOfMemory(ctx);
    h->items[h->count] = JS_DupValue(ctx, argv[0]);
    idx = h->count;
    h->count++;
    if (dyn_heap_sift_up(ctx, this_val, h, idx)) {
        h->count--;
        JS_FreeValue(ctx, h->items[h->count]);
        return JS_EXCEPTION;
    }
    return JS_NewInt64(ctx, (int64_t)h->count);
}

static JSValue dyn_heap_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_heap_t* h;
    JSValue top;

    (void)argc;
    (void)argv;
    h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->busy)
        return JS_ThrowTypeError(ctx, "Heap: comparator must not push/pop its own heap");
    if (h->count == 0)
        return JS_UNDEFINED;
    top = h->items[0];
    h->count--;
    if (h->count > 0) {
        h->items[0] = h->items[h->count];
        if (dyn_heap_sift_down(ctx, this_val, h, 0)) {
            JS_FreeValue(ctx, top);
            return JS_EXCEPTION;
        }
    }
    return top;
}

static JSValue dyn_heap_peek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_heap_t* h;

    (void)argc;
    (void)argv;
    h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->count == 0)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, h->items[0]);
}

static JSValue dyn_heap_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_heap_t* h = (dyn_heap_t*)dyn_plain_get(ctx, this_val, dyn_heap_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)h->count);
}

static const JSCFunctionListEntry dyn_heap_proto[] = {
    JS_CFUNC_DEF("push", 1, dyn_heap_push),
    JS_CFUNC_DEF("pop", 0, dyn_heap_pop),
    JS_CFUNC_DEF("peek", 0, dyn_heap_peek),
    JS_CGETSET_DEF("size", dyn_heap_size, NULL),
    JS_CGETSET_DEF("length", dyn_heap_size, NULL),
};

typedef struct DynListNode {
    struct DynListNode *prev, *next;
    JSValue value;
} DynListNode;

typedef struct {
    JSRuntime* rt;
    DynListNode *head, *tail;
    uint32_t length;
} dyn_list_t;

static void dyn_list_free(void* native)
{
    dyn_list_t* l = (dyn_list_t*)native;
    DynListNode *n, *next;

    if (!l)
        return;
    for (n = l->head; n; n = next) {
        next = n->next;
        JS_FreeValueRT(l->rt, n->value);
        dyn_nat_free(n);
    }
    dyn_nat_free(l);
}

static JSClassID dyn_list_class_id;

static void dyn_list_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_list_free(JS_GetOpaque(val, dyn_list_class_id));
}
static void dyn_list_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_list_t* l = JS_GetOpaque(val, dyn_list_class_id);
    DynListNode* n;
    if (!l)
        return;
    for (n = l->head; n; n = n->next)
        JS_MarkValue(rt, n->value, mark_func);
}
static const JSClassDef dyn_list_class = {
    "List",
    .finalizer = dyn_list_finalizer,
    .gc_mark = dyn_list_gc_mark,
};

static JSValue dyn_list_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_nat_malloc(sizeof(*l));
    if (!l)
        return JS_ThrowOutOfMemory(ctx);
    l->rt = JS_GetRuntime(ctx);
    l->head = NULL;
    l->tail = NULL;
    l->length = 0;
    return dyn_plain_wrap(ctx, new_target, dyn_list_class_id, l, dyn_list_free);
}

static JSValue dyn_list_push_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;
    DynListNode* n;

    (void)argc;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    n = (DynListNode*)dyn_nat_malloc(sizeof(*n));
    if (!n)
        return JS_ThrowOutOfMemory(ctx);
    n->value = JS_DupValue(ctx, argv[0]);
    n->prev = NULL;
    n->next = l->head;
    if (l->head)
        l->head->prev = n;
    else
        l->tail = n;
    l->head = n;
    l->length++;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dyn_list_push_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;
    DynListNode* n;

    (void)argc;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    n = (DynListNode*)dyn_nat_malloc(sizeof(*n));
    if (!n)
        return JS_ThrowOutOfMemory(ctx);
    n->value = JS_DupValue(ctx, argv[0]);
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
    l->length++;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dyn_list_pop_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;
    DynListNode* n;
    JSValue v;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->head)
        return JS_UNDEFINED;
    n = l->head;
    l->head = n->next;
    if (l->head)
        l->head->prev = NULL;
    else
        l->tail = NULL;
    l->length--;
    v = n->value;
    dyn_nat_free(n);
    return v;
}

static JSValue dyn_list_pop_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;
    DynListNode* n;
    JSValue v;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->tail)
        return JS_UNDEFINED;
    n = l->tail;
    l->tail = n->prev;
    if (l->tail)
        l->tail->next = NULL;
    else
        l->head = NULL;
    l->length--;
    v = n->value;
    dyn_nat_free(n);
    return v;
}

static JSValue dyn_list_front(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->head)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, l->head->value);
}

static JSValue dyn_list_back(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    if (!l->tail)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, l->tail->value);
}

static JSValue dyn_list_length(JSContext* ctx, JSValueConst this_val)
{
    dyn_list_t* l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)l->length);
}

static JSValue dyn_list_to_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_list_t* l;
    DynListNode* n;
    JSValue arr;
    uint32_t i;

    (void)argc;
    (void)argv;
    l = (dyn_list_t*)dyn_plain_get(ctx, this_val, dyn_list_class_id);
    if (!l)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    i = 0;
    for (n = l->head; n; n = n->next) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i++, JS_DupValue(ctx, n->value),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_list_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue arr, fn, iter;

    arr = dyn_list_to_array(ctx, this_val, argc, argv);
    if (JS_IsException(arr))
        return arr;
    fn = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, arr);
        return fn;
    }
    iter = JS_Call(ctx, fn, arr, 0, NULL);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arr);
    return iter;
}

static const JSCFunctionListEntry dyn_list_proto[] = {
    JS_CFUNC_DEF("pushFront", 1, dyn_list_push_front),
    JS_CFUNC_DEF("pushBack", 1, dyn_list_push_back),
    JS_CFUNC_DEF("popFront", 0, dyn_list_pop_front),
    JS_CFUNC_DEF("popBack", 0, dyn_list_pop_back),
    JS_CFUNC_DEF("front", 0, dyn_list_front),
    JS_CFUNC_DEF("back", 0, dyn_list_back),
    JS_CFUNC_DEF("toArray", 0, dyn_list_to_array),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_list_iterator),
    JS_CGETSET_DEF("length", dyn_list_length, NULL),
};

#include "dyna-serialize.h"

static JSCFunction* dyn_ds_native_ctor(JSClassID id)
{
    if (id == dyn_bitset_class_id)
        return dyn_bitset_ctor;
    if (id == dyn_uf_class_id)
        return dyn_uf_ctor;
    if (id == dyn_deque_class_id)
        return dyn_deque_ctor;
    if (id == dyn_fenwick_class_id)
        return dyn_fenwick_ctor;
    if (id == dyn_ringbuf_class_id)
        return dyn_ringbuf_ctor;
    if (id == dyn_segtree_class_id)
        return dyn_segtree_ctor;
    if (id == dyn_bloom_class_id)
        return dyn_bloom_ctor;
    if (id == dyn_trie_class_id)
        return dyn_trie_ctor;
    if (id == dyn_lru_class_id)
        return dyn_lru_ctor;
    if (id == dyn_sortedset_class_id)
        return dyn_sortedset_ctor;
    if (id == dyn_sortedmap_class_id)
        return dyn_sortedmap_ctor;
    if (id == dyn_heap_class_id)
        return dyn_heap_ctor;
    if (id == dyn_list_class_id)
        return dyn_list_ctor;
    return NULL;
}

static JSValue dyn_ds_construct(JSContext* ctx, JSClassID id, int argc,
    JSValueConst* argv)
{
    JSCFunction* native_ctor = dyn_ds_native_ctor(id);
    JSValue obj;

    if (!native_ctor)
        return JS_ThrowTypeError(ctx, "deserialize: class has no native constructor");
    obj = native_ctor(ctx, JS_UNDEFINED, argc, argv);
    if (!JS_IsException(obj) && !JS_GetOpaque(obj, id)) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "deserialize: constructor produced a foreign object");
    }
    return obj;
}

static int dyn_ds_invoke(JSContext* ctx, JSValueConst obj, const char* name,
    int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name), r;

    if (JS_IsException(fn))
        return -1;
    r = JS_Call(ctx, fn, obj, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

typedef JSValue (*dyn_ds_val_at)(void* native, uint32_t i);

static int dyn_ds_write_values(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, dyn_ds_val_at get)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (JS_IsException(arr))
        return -1;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, get(native, i)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue dyn_ds_read_values(JSContext* ctx, dyn_de_t* r, uint32_t n)
{
    JSValue arr = dyn_codec_read_values(ctx, r), len;
    uint32_t got = 0;

    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &got, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (got != n) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "malformed DYNS record: %u values for %u entries",
            (unsigned)got, (unsigned)n);
    }
    return arr;
}

#define DS_BITSET_CHUNKED 0xFFFFFFFFu
#define DS_BITSET_CHUNKW 1024u
#define DS_BS_EMPTY 0
#define DS_BS_ARRAY 1
#define DS_BS_BITMAP 2
#define DS_BS_RUN 3

static void dyn_bs_chunk_stats(const uint64_t* w, uint32_t nw,
    uint32_t* bits, uint32_t* runs)
{
    uint32_t i, b = 0, r = 0;
    int prev = 0;
    for (i = 0; i < nw; i++) {
        uint64_t v = w[i];
        b += (uint32_t)__builtin_popcountll(v);
        r += (uint32_t)__builtin_popcountll(v & ~((v << 1) | (uint64_t)prev));
        prev = (int)(v >> 63);
    }
    *bits = b;
    *runs = r;
}

static int dyn_bs_write_chunk(dyn_ser_t* w, const uint64_t* words, uint32_t nw)
{
    uint32_t bits = 0, runs = 0, i;
    dyn_bs_chunk_stats(words, nw, &bits, &runs);

    if (bits == 0)
        return dyn_ser_u8(w, DS_BS_EMPTY);

    {
        size_t sz_arr = (size_t)bits * 2, sz_run = (size_t)runs * 4,
               sz_map = (size_t)nw * 8;
        if (sz_run <= sz_arr && sz_run < sz_map) {
            uint32_t start = 0, len = 0;
            if (dyn_ser_u8(w, DS_BS_RUN) < 0 || dyn_ser_u16(w, (uint16_t)runs) < 0)
                return -1;
            for (i = 0; i < nw; i++) {
                uint64_t v = words[i];
                uint32_t base = i * 64;
                if (v == 0) {
                    if (len) {
                        if (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0)
                            return -1;
                        len = 0;
                    }
                    continue;
                }
                if (v == ~(uint64_t)0) {
                    if (!len)
                        start = base;
                    len += 64;
                    continue;
                }
                {
                    uint32_t bit = 0;
                    while (bit < 64) {
                        uint64_t rest = v >> bit;
                        uint32_t skip;
                        if (!(rest & 1)) {
                            if (len) {
                                if (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0)
                                    return -1;
                                len = 0;
                            }
                            skip = rest ? (uint32_t)__builtin_ctzll(rest) : 64 - bit;
                        } else {
                            uint64_t inv = ~rest;
                            skip = inv ? (uint32_t)__builtin_ctzll(inv) : 64 - bit;
                            if (!len)
                                start = base + bit;
                            len += skip;
                        }
                        bit += skip;
                    }
                }
            }
            if (len && (dyn_ser_u16(w, (uint16_t)start) < 0 || dyn_ser_u16(w, (uint16_t)(len - 1)) < 0))
                return -1;
            return 0;
        }
        if (sz_arr < sz_map) {
            if (dyn_ser_u8(w, DS_BS_ARRAY) < 0 || dyn_ser_u16(w, (uint16_t)bits) < 0)
                return -1;
            for (i = 0; i < nw; i++) {
                uint64_t v = words[i];
                while (v) {
                    uint32_t off = (uint32_t)(i * 64 + (uint32_t)__builtin_ctzll(v));
                    if (dyn_ser_u16(w, (uint16_t)off) < 0)
                        return -1;
                    v &= v - 1;
                }
            }
            return 0;
        }
    }
    if (dyn_ser_u8(w, DS_BS_BITMAP) < 0)
        return -1;
    for (i = 0; i < nw; i++)
        if (dyn_ser_u64(w, words[i]) < 0)
            return -1;
    return 0;
}

static int dyn_bitset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_bitset_t* b = JS_GetOpaque(obj, dyn_bitset_class_id);
    uint32_t nchunks, c;
    (void)ctx;
    if (!b)
        return -1;

    nchunks = (b->nwords + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW;
    if (dyn_ser_u32(w, DS_BITSET_CHUNKED) < 0 || dyn_ser_u32(w, b->nwords) < 0 || dyn_ser_u32(w, nchunks) < 0)
        return -1;
    for (c = 0; c < nchunks; c++) {
        uint32_t off = c * DS_BITSET_CHUNKW;
        uint32_t nw = b->nwords - off;
        if (nw > DS_BITSET_CHUNKW)
            nw = DS_BITSET_CHUNKW;
        if (dyn_bs_write_chunk(w, b->words + off, nw) < 0)
            return -1;
    }
    return 0;
}

static int dyn_bs_read_chunk(dyn_de_t* r, uint64_t* words, uint32_t nw)
{
    uint32_t kind = dyn_de_u8(r), i, cnt;
    uint32_t maxbit = nw * 64;

    switch (kind) {
    case DS_BS_EMPTY:
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_ARRAY:
        cnt = dyn_de_u16(r);
        if (!dyn_de_ok(r) || (size_t)cnt * 2 > dyn_de_left(r))
            return -1;
        for (i = 0; i < cnt; i++) {
            uint32_t off = dyn_de_u16(r);
            if (off >= maxbit)
                return -1;
            words[off >> 6] |= (uint64_t)1 << (off & 63);
        }
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_RUN:
        cnt = dyn_de_u16(r);
        if (!dyn_de_ok(r) || (size_t)cnt * 4 > dyn_de_left(r))
            return -1;
        for (i = 0; i < cnt; i++) {
            uint32_t start = dyn_de_u16(r);
            uint32_t len = (uint32_t)dyn_de_u16(r) + 1;
            uint32_t j;
            if (start >= maxbit || len > maxbit - start)
                return -1;
            {
                uint32_t end = start + len;
                uint32_t wfirst = (start + 63) >> 6, wlast = end >> 6;
                for (j = start; j < end && (j & 63); j++)
                    words[j >> 6] |= (uint64_t)1 << (j & 63);
                for (j = wfirst; j < wlast; j++)
                    words[j] = ~(uint64_t)0;
                for (j = wlast * 64 > start ? wlast * 64 : start; j < end; j++)
                    words[j >> 6] |= (uint64_t)1 << (j & 63);
            }
        }
        return dyn_de_ok(r) ? 0 : -1;
    case DS_BS_BITMAP:
        if ((size_t)nw * 8 > dyn_de_left(r))
            return -1;
        for (i = 0; i < nw; i++)
            words[i] = dyn_de_u64(r);
        return dyn_de_ok(r) ? 0 : -1;
    default:
        return -1;
    }
}

static JSValue dyn_bitset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i, first;
    JSValue obj;
    dyn_bitset_t* b;
    int chunked;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    chunked = (first == DS_BITSET_CHUNKED);
    if (chunked) {
        uint32_t nchunks, want;
        n = dyn_de_u32(r);
        nchunks = dyn_de_u32(r);
        want = (n + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW;
        if (!dyn_de_ok(r) || n > DYN_MAX_CAPACITY || n > DYN_DECODE_CHUNK_MAX_WORDS || nchunks != want)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        if (dyn_de_charge(r, (uint64_t)n * 8) < 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = first;
        if ((size_t)n * 8 > dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }

    obj = dyn_ds_construct(ctx, dyn_bitset_class_id, 0, NULL);
    if (JS_IsException(obj))
        return obj;
    b = JS_GetOpaque(obj, dyn_bitset_class_id);
    if (n && dyn_bitset_ensure(b, n - 1) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (!chunked) {
        for (i = 0; i < n; i++)
            b->words[i] = dyn_de_u64(r);
        return obj;
    }
    {
        uint32_t nchunks = (n + DS_BITSET_CHUNKW - 1) / DS_BITSET_CHUNKW, c;
        for (c = 0; c < nchunks; c++) {
            uint32_t off = c * DS_BITSET_CHUNKW;
            uint32_t nw = n - off;
            if (nw > DS_BITSET_CHUNKW)
                nw = DS_BITSET_CHUNKW;
            if (dyn_bs_read_chunk(r, b->words + off, nw) < 0) {
                JS_FreeValue(ctx, obj);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
        }
    }
    return obj;
}

#define dyn_sk_uvarint(w, v) dyn_ser_uvarint((w), (v))
#define dyn_sk_read_uvarint(r, o) dyn_de_uvarint((r), (o))

#define DS_NUM_RAW 0
#define DS_NUM_CONSTANT 1
#define DS_NUM_INT 2
#define DS_NUM_DELTA 3
#define DS_NUM_MAX (1u << 24)
#define DS_NUM_EXT 0xFFFFFFFFu

static int dyn_num_is_int(double v)
{
    static const double neg_zero = -0.0;
    if (v == 0.0)
        return memcmp(&v, &neg_zero, sizeof v) != 0;
    return v == v && v >= -9007199254740992.0 && v <= 9007199254740992.0 && v == (double)(int64_t)v;
}

static uint64_t dyn_num_zig(int64_t x)
{
    return ((uint64_t)x << 1) ^ (uint64_t)(x >> 63);
}

static int dyn_num_write(dyn_ser_t* w, const double* v, size_t n)
{
    size_t i;
    uint8_t kind = DS_NUM_CONSTANT;

    if (dyn_ser_u32(w, (uint32_t)n) < 0)
        return -1;
    if (!n)
        return dyn_ser_u8(w, DS_NUM_CONSTANT);

    for (i = 1; i < n; i++)
        if (memcmp(&v[i], &v[0], sizeof(double)) != 0) {
            kind = DS_NUM_INT;
            break;
        }
    if (kind == DS_NUM_INT)
        for (i = 0; i < n; i++)
            if (!dyn_num_is_int(v[i])) {
                kind = DS_NUM_RAW;
                break;
            }

    if (kind == DS_NUM_INT) {
        size_t flat = 0, delta = 0;
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            int64_t x = (int64_t)v[i];
            flat += dyn_varint_len(dyn_num_zig(x));
            delta += dyn_varint_len(dyn_num_zig(x - prev));
            prev = x;
        }
        if (delta < flat)
            kind = DS_NUM_DELTA;
    }

    if (dyn_ser_u8(w, kind) < 0)
        return -1;
    if (kind == DS_NUM_CONSTANT)
        return dyn_ser_f64(w, v[0]);
    if (kind == DS_NUM_INT || kind == DS_NUM_DELTA) {
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            int64_t x = (int64_t)v[i];
            int64_t out = kind == DS_NUM_DELTA ? x - prev : x;
            prev = x;
            if (dyn_sk_uvarint(w, dyn_num_zig(out)) < 0)
                return -1;
        }
        return 0;
    }
    for (i = 0; i < n; i++)
        if (dyn_ser_f64(w, v[i]) < 0)
            return -1;
    return 0;
}

static int dyn_num_read_header(dyn_de_t* r, uint32_t* n, uint8_t* kind)
{
    uint32_t c = dyn_de_u32(r);
    uint8_t k;
    if (!dyn_de_ok(r) || c > DS_NUM_MAX)
        return -1;
    k = dyn_de_u8(r);
    if (!dyn_de_ok(r) || k > DS_NUM_DELTA)
        return -1;
    if (k == DS_NUM_CONSTANT && c > DYN_DECODE_AMPLIFIED_NUM_MAX)
        return -1;
    if ((k == DS_NUM_RAW && (uint64_t)c * 8 > (uint64_t)dyn_de_left(r)) || ((k == DS_NUM_INT || k == DS_NUM_DELTA) && (uint64_t)c > (uint64_t)dyn_de_left(r)))
        return -1;
    if (dyn_de_charge(r, (uint64_t)c * 8) < 0)
        return -1;
    *n = c;
    *kind = k;
    return 0;
}

static double* dyn_num_read_pairs(dyn_de_t* r, uint32_t first, uint32_t per,
    uint32_t* count, uint32_t* elems);

static int dyn_num_fill(dyn_de_t* r, uint8_t kind, double* v, uint32_t n)
{
    uint32_t i;
    if (kind == DS_NUM_CONSTANT) {
        double c;
        if (!n)
            return 0;
        c = dyn_de_f64(r);
        if (!dyn_de_ok(r))
            return -1;
        for (i = 0; i < n; i++)
            v[i] = c;
        return 0;
    }
    if (kind == DS_NUM_INT || kind == DS_NUM_DELTA) {
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            uint64_t z;
            int64_t x;
            if (dyn_sk_read_uvarint(r, &z) < 0)
                return -1;
            x = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
            if (kind == DS_NUM_DELTA)
                x += prev;
            prev = x;
            v[i] = (double)x;
        }
        return 0;
    }
    for (i = 0; i < n; i++)
        v[i] = dyn_de_f64(r);
    return dyn_de_ok(r) ? 0 : -1;
}

#define DS_UF_EXT 0xFFFFFFFFu
#define DS_UF_IDENTITY 0
#define DS_UF_DELTA 1
#define DS_UF_RAW 2

static int dyn_uf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_uf_t* u = JS_GetOpaque(obj, dyn_uf_class_id);
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

static int dyn_uf_forest_ok(dyn_uf_t* u, uint32_t* nroots)
{
    uint32_t n = u->n, i, roots = 0;
    uint8_t* st;

    st = (uint8_t*)dyn_nat_calloc(n ? n : 1, 1);
    if (!st)
        return -1;
    for (i = 0; i < n; i++) {
        uint32_t x = i, y;
        int cyc = 0;
        while (st[x] == 0 && u->parent[x] != x) {
            st[x] = 1;
            x = u->parent[x];
        }
        if (st[x] == 1 && u->parent[x] != x)
            cyc = 1;
        y = i;
        while (st[y] == 1) {
            st[y] = cyc ? 0 : 2;
            y = u->parent[y];
        }
        if (cyc) {
            dyn_nat_free(st);
            return 1;
        }
    }
    for (i = 0; i < n; i++)
        if (u->parent[i] == i)
            roots++;
    dyn_nat_free(st);
    *nroots = roots;
    return 0;
}

static JSValue dyn_uf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first, n, sets, i;
    uint8_t kind;
    JSValue obj, arg;
    dyn_uf_t* u;
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
    obj = dyn_ds_construct(ctx, dyn_uf_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj))
        return obj;
    u = JS_GetOpaque(obj, dyn_uf_class_id);

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
        int crc = dyn_uf_forest_ok(u, &roots);
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

static int dyn_fenwick_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_fenwick_t* f = JS_GetOpaque(obj, dyn_fenwick_class_id);
    (void)ctx;
    if (!f)
        return -1;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    return dyn_num_write(w, f->tree + 1, f->n);
}

static JSValue dyn_fenwick_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i = 0, first;
    uint8_t kind;
    JSValue obj, arg;
    dyn_fenwick_t* f;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &n, &kind) < 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = first;
        kind = DS_NUM_RAW;
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    arg = JS_NewInt64(ctx, (int64_t)n);
    obj = dyn_ds_construct(ctx, dyn_fenwick_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj))
        return obj;
    f = JS_GetOpaque(obj, dyn_fenwick_class_id);
    if (dyn_num_fill(r, kind, f->tree + 1, n) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    (void)i;
    return obj;
}

static int dyn_segtree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_segtree_t* s = JS_GetOpaque(obj, dyn_segtree_class_id);
    (void)ctx;
    if (!s)
        return -1;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0 || dyn_ser_u32(w, (uint32_t)s->op) < 0)
        return -1;
    return dyn_num_write(w, s->tree + 1, (size_t)2 * s->n - 1);
}

static JSValue dyn_segtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), op = dyn_de_u32(r), n, elems;
    uint8_t kind;
    JSValue obj, args[2];
    dyn_segtree_t* s;
    static const char* const opnames[] = { "sum", "min", "max" };
    (void)opts;

    if (!dyn_de_ok(r) || op >= countof(opnames))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &elems, &kind) < 0 || !elems || !(elems & 1))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        n = (elems + 1) / 2;
    } else {
        n = first;
        kind = DS_NUM_RAW;
        if ((uint64_t)n * 16 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    args[0] = JS_NewInt64(ctx, (int64_t)n);
    args[1] = JS_NewString(ctx, opnames[op]);
    obj = dyn_ds_construct(ctx, dyn_segtree_class_id, 2,
        (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(obj))
        return obj;
    s = JS_GetOpaque(obj, dyn_segtree_class_id);
    if (dyn_num_fill(r, kind, s->tree + 1, 2 * n - 1) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    return obj;
}

#define DS_SKETCH_EXT 0xFFFFFFFFu
#define DS_SKETCH_RAW 0
#define DS_SKETCH_ALT 1

static int dyn_sketch_write_u64(dyn_ser_t* w, const uint64_t* v, size_t n)
{
    size_t i, nz = 0, sparse = 0, prev = 0;

    for (i = 0; i < n; i++) {
        if (!v[i])
            continue;
        sparse += dyn_varint_len(i - prev) + dyn_varint_len(v[i]);
        prev = i;
        nz++;
    }
    sparse += dyn_varint_len(nz);
    if (sparse >= n * 8) {
        if (dyn_ser_u8(w, DS_SKETCH_RAW) < 0)
            return -1;
        for (i = 0; i < n; i++)
            if (dyn_ser_u64(w, v[i]) < 0)
                return -1;
        return 0;
    }
    if (dyn_ser_u8(w, DS_SKETCH_ALT) < 0 || dyn_ser_uvarint(w, nz) < 0)
        return -1;
    prev = 0;
    for (i = 0; i < n; i++) {
        if (!v[i])
            continue;
        if (dyn_ser_uvarint(w, i - prev) < 0 || dyn_ser_uvarint(w, v[i]) < 0)
            return -1;
        prev = i;
    }
    return 0;
}

static int dyn_sketch_read_u64(dyn_de_t* r, uint64_t* v, size_t n)
{
    uint8_t kind = dyn_de_u8(r);
    uint64_t nz, k, at = 0;
    size_t i;

    if (!dyn_de_ok(r) || kind > DS_SKETCH_ALT)
        return -1;
    if (kind == DS_SKETCH_RAW) {
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return -1;
        for (i = 0; i < n; i++)
            v[i] = dyn_de_u64(r);
        return dyn_de_ok(r) ? 0 : -1;
    }
    memset(v, 0, n * sizeof(*v));
    if (dyn_de_uvarint(r, &nz) < 0 || nz > (uint64_t)n)
        return -1;
    for (k = 0; k < nz; k++) {
        uint64_t d, val;
        if (dyn_de_uvarint(r, &d) < 0 || dyn_de_uvarint(r, &val) < 0)
            return -1;
        at += d;
        if (at >= (uint64_t)n)
            return -1;
        v[at] = val;
    }
    return 0;
}

static int dyn_sketch_write_u8(dyn_ser_t* w, const uint8_t* v, size_t n)
{
    size_t i, rle = 0;

    for (i = 0; i < n;) {
        size_t j = i;
        while (j < n && v[j] == v[i])
            j++;
        rle += dyn_varint_len(j - i) + 1;
        i = j;
    }
    if (rle >= n) {
        if (dyn_ser_u8(w, DS_SKETCH_RAW) < 0)
            return -1;
        return dyn_ser_raw(w, v, n);
    }
    if (dyn_ser_u8(w, DS_SKETCH_ALT) < 0)
        return -1;
    for (i = 0; i < n;) {
        size_t j = i;
        while (j < n && v[j] == v[i])
            j++;
        if (dyn_ser_uvarint(w, j - i) < 0 || dyn_ser_u8(w, v[i]) < 0)
            return -1;
        i = j;
    }
    return 0;
}

static int dyn_sketch_read_u8(dyn_de_t* r, uint8_t* v, size_t n)
{
    uint8_t kind = dyn_de_u8(r);
    size_t at = 0;

    if (!dyn_de_ok(r) || kind > DS_SKETCH_ALT)
        return -1;
    if (kind == DS_SKETCH_RAW) {
        const uint8_t* p = dyn_de_raw(r, n);
        if (!p)
            return -1;
        memcpy(v, p, n);
        return 0;
    }
    while (at < n) {
        uint64_t run;
        uint8_t val;
        if (dyn_de_uvarint(r, &run) < 0)
            return -1;
        val = dyn_de_u8(r);
        if (!dyn_de_ok(r) || !run || run > (uint64_t)(n - at))
            return -1;
        memset(v + at, val, (size_t)run);
        at += (size_t)run;
    }
    return 0;
}

static int dyn_bloom_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_bloom_t* b = JS_GetOpaque(obj, dyn_bloom_class_id);
    uint32_t i;
    (void)ctx;
    (void)i;
    if (!b)
        return -1;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, b->nbits) < 0 || dyn_ser_u32(w, b->k) < 0)
        return -1;
    return dyn_sketch_write_u64(w, b->words, b->nwords);
}

static JSValue dyn_bloom_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t nbits, k, nwords, i, first;
    int ext;
    JSValue obj, args[2];
    dyn_bloom_t* b;
    (void)opts;

    first = dyn_de_u32(r);
    ext = (first == DS_SKETCH_EXT);
    nbits = ext ? dyn_de_u32(r) : first;
    k = dyn_de_u32(r);
    if (!dyn_de_ok(r) || nbits == 0 || k == 0 || k > 32)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    nwords = (nbits + 63) >> 6;
    if (!ext && (uint64_t)nwords * 8 > (uint64_t)dyn_de_left(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (ext && nwords > DYN_DECODE_SKETCH_MAX_WORDS)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, (uint64_t)nwords * 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    args[0] = JS_NewInt64(ctx, (int64_t)nbits);
    args[1] = JS_NewInt64(ctx, (int64_t)k);
    obj = dyn_ds_construct(ctx, dyn_bloom_class_id, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(obj))
        return obj;
    b = JS_GetOpaque(obj, dyn_bloom_class_id);
    if (ext) {
        if (dyn_sketch_read_u64(r, b->words, nwords) < 0) {
            JS_FreeValue(ctx, obj);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        for (i = 0; i < nwords; i++)
            b->words[i] = dyn_de_u64(r);
    }
    return obj;
}

#define DS_TRIE_FRONTCODED 0xFFFFFFFFu

typedef struct {
    DynTrieNode* n;
    size_t depth;
} dyn_trie_frame;

static void dyn_trie_order(DynTrieNode** v, size_t k)
{
    size_t i, j;
    if (k <= 32) {
        for (i = 1; i < k; i++) {
            DynTrieNode* x = v[i];
            for (j = i; j && v[j - 1]->byte > x->byte; j--)
                v[j] = v[j - 1];
            v[j] = x;
        }
    } else {
        DynTrieNode* slot[256];
        memset(slot, 0, sizeof(slot));
        for (i = 0; i < k; i++)
            slot[v[i]->byte] = v[i];
        for (i = 0, k = 0; i < 256; i++)
            if (slot[i])
                v[k++] = slot[i];
    }
}

static int dyn_trie_grow(void** p, size_t* cap, size_t need, size_t elem)
{
    size_t nc = *cap ? *cap : 64;
    void* np;
    if (need <= *cap)
        return 0;
    while (nc < need)
        nc *= 2;
    np = dyn_nat_realloc(*p, nc * elem);
    if (!np)
        return -1;
    *p = np;
    *cap = nc;
    return 0;
}

static int dyn_trie_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_trie_t* t = JS_GetOpaque(obj, dyn_trie_class_id);
    dyn_trie_frame* frames = NULL;
    size_t nframes = 0, fcap = 0;
    uint8_t *path = NULL, *prev = NULL;
    size_t pcap = 0, prevcap = 0, prev_len = 0;
    DynTrieNode* kids[256];
    uint32_t emitted = 0;
    if (!t)
        return -1;
    int rc = -1;

    (void)ctx;
    if (dyn_ser_u32(w, DS_TRIE_FRONTCODED) < 0 || dyn_ser_u32(w, t->count) < 0)
        goto done;

    if (t->root->is_end) {
        if (dyn_sk_uvarint(w, 0) < 0 || dyn_sk_uvarint(w, 0) < 0)
            goto done;
        emitted++;
    }
    if (t->root->tail) {
        if (dyn_sk_uvarint(w, 0) < 0 || dyn_sk_uvarint(w, t->root->tail_len) < 0 || dyn_ser_raw(w, t->root->tail, t->root->tail_len) < 0)
            goto done;
        if (dyn_trie_grow((void**)&prev, &prevcap, t->root->tail_len, 1) < 0)
            goto done;
        memcpy(prev, t->root->tail, t->root->tail_len);
        prev_len = t->root->tail_len;
        emitted++;
    }
    {
        DynTrieNode* c;
        size_t k = 0;
        for (c = t->root->child; c; c = c->sibling) {
            if (k == 256)
                goto done;
            kids[k++] = c;
        }
        dyn_trie_order(kids, k);
        if (dyn_trie_grow((void**)&frames, &fcap, k, sizeof(*frames)) < 0)
            goto done;
        while (k--) {
            frames[nframes].n = kids[k];
            frames[nframes].depth = 0;
            nframes++;
        }
    }
    while (nframes) {
        DynTrieNode *n = frames[nframes - 1].n, *c;
        size_t depth = frames[nframes - 1].depth, k = 0;
        nframes--;

        if (dyn_trie_grow((void**)&path, &pcap, depth + 1, 1) < 0)
            goto done;
        path[depth] = n->byte;

        {
            size_t which;
            for (which = 0; which < 2; which++) {
                size_t klen, sh = 0, lim;
                if (which == 0) {
                    if (!n->is_end)
                        continue;
                    klen = depth + 1;
                } else {
                    if (!n->tail)
                        continue;
                    klen = depth + 1 + n->tail_len;
                    if (dyn_trie_grow((void**)&path, &pcap, klen, 1) < 0)
                        goto done;
                    memcpy(path + depth + 1, n->tail, n->tail_len);
                }
                lim = klen < prev_len ? klen : prev_len;
                while (sh < lim && path[sh] == prev[sh])
                    sh++;
                if (dyn_sk_uvarint(w, sh) < 0 || dyn_sk_uvarint(w, klen - sh) < 0 || dyn_ser_raw(w, path + sh, klen - sh) < 0)
                    goto done;
                if (dyn_trie_grow((void**)&prev, &prevcap, klen, 1) < 0)
                    goto done;
                memcpy(prev, path, klen);
                prev_len = klen;
                emitted++;
            }
        }

        for (c = n->child; c; c = c->sibling) {
            if (k == 256)
                goto done;
            kids[k++] = c;
        }
        dyn_trie_order(kids, k);
        if (dyn_trie_grow((void**)&frames, &fcap, nframes + k,
                sizeof(*frames))
            < 0)
            goto done;
        while (k--) {
            frames[nframes].n = kids[k];
            frames[nframes].depth = depth + 1;
            nframes++;
        }
    }
    if (emitted != t->count)
        goto done;
    rc = 0;
done:
    dyn_nat_free(frames);
    dyn_nat_free(path);
    dyn_nat_free(prev);
    return rc;
}

static JSValue dyn_trie_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i, first;
    JSValue obj;
    dyn_trie_t* t;
    uint8_t* cur = NULL;
    size_t curcap = 0, cur_len = 0;
    DynTrieNode** path = NULL;
    size_t pathcap = 0, path_len = 0;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);

    if (first != DS_TRIE_FRONTCODED) {
        if ((uint64_t)first * 4 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        obj = dyn_ds_construct(ctx, dyn_trie_class_id, 0, NULL);
        if (JS_IsException(obj))
            return obj;
        t = JS_GetOpaque(obj, dyn_trie_class_id);
        for (i = 0; i < first; i++) {
            size_t kl;
            const char* k = dyn_de_blob(r, &kl);
            if (!dyn_de_ok(r)) {
                JS_FreeValue(ctx, obj);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            if (dyn_trie_insert_bytes(t, k, kl) < 0) {
                JS_FreeValue(ctx, obj);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        return obj;
    }

    if (dyn_de_count(r, &n, 2) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    obj = dyn_ds_construct(ctx, dyn_trie_class_id, 0, NULL);
    if (JS_IsException(obj))
        return obj;
    t = JS_GetOpaque(obj, dyn_trie_class_id);
    for (i = 0; i < n; i++) {
        uint64_t sh, sl;
        const uint8_t* p;
        if (dyn_sk_read_uvarint(r, &sh) < 0 || dyn_sk_read_uvarint(r, &sl) < 0)
            goto truncated;
        if (sh > cur_len || sl > (uint64_t)dyn_de_left(r))
            goto truncated;
        if (dyn_trie_grow((void**)&cur, &curcap, (size_t)(sh + sl), 1) < 0)
            goto oom;
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto truncated;
        memcpy(cur + sh, p, (size_t)sl);
        cur_len = (size_t)(sh + sl);
        if (dyn_trie_grow((void**)&path, &pathcap,
                (cur_len + 1) * sizeof(*path), 1)
            < 0)
            goto oom;
        {
            size_t from = sh < path_len ? (size_t)sh : path_len;
            DynTrieNode* start = from ? path[from - 1] : t->root;
            if (dyn_trie_insert_at(t, start, (const char*)cur, cur_len, from,
                    path, &path_len)
                < 0)
                goto oom;
        }
    }
    dyn_nat_free(cur);
    dyn_nat_free(path);
    return obj;
truncated:
    dyn_nat_free(cur);
    dyn_nat_free(path);
    JS_FreeValue(ctx, obj);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
oom:
    dyn_nat_free(cur);
    dyn_nat_free(path);
    JS_FreeValue(ctx, obj);
    return JS_ThrowOutOfMemory(ctx);
}

static int dyn_lru_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_lru_t* L = JS_GetOpaque(obj, dyn_lru_class_id);
    DynLRUNode* n;
    JSValue arr;
    uint32_t i;
    int r;
    if (!L)
        return -1;

    if (dyn_ser_u32(w, L->cap) < 0 || dyn_ser_u32(w, L->count) < 0)
        return -1;
    for (n = L->tail; n; n = n->prev)
        if (dyn_ser_blob(w, n->key, n->klen) < 0)
            return -1;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return -1;
    i = 0;
    for (n = L->tail; n; n = n->prev)
        if (JS_DefinePropertyValueUint32(ctx, arr, i++,
                JS_DupValue(ctx, n->value),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue dyn_lru_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t cap = dyn_de_u32(r), n, i;
    JSValue obj, vals, arg;
    const char** keys;
    size_t* klens;
    (void)opts;

    if (!dyn_de_ok(r) || cap == 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_count(r, &n, 4) < 0 || n > cap)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    keys = dyn_nat_calloc(n ? n : 1, sizeof(*keys));
    klens = dyn_nat_calloc(n ? n : 1, sizeof(*klens));
    if (!keys || !klens) {
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        keys[i] = dyn_de_blob(r, &klens[i]);
    if (!dyn_de_ok(r)) {
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = dyn_ds_read_values(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        return vals;
    }
    arg = JS_NewInt64(ctx, (int64_t)cap);
    obj = dyn_ds_construct(ctx, dyn_lru_class_id, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        return obj;
    }
    for (i = 0; i < n; i++) {
        JSValue args[2];
        int bad;
        args[0] = JS_NewStringLen(ctx, keys[i], klens[i]);
        args[1] = JS_GetPropertyUint32(ctx, vals, i);
        bad = JS_IsException(args[0]) || JS_IsException(args[1]) || dyn_ds_invoke(ctx, obj, "put", 2, (JSValueConst*)args) < 0;
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (bad) {
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            dyn_nat_free(keys);
            dyn_nat_free(klens);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(keys);
    dyn_nat_free(klens);
    return obj;
}

#define DS_SKIP_EXT 0xFFFFFFFFu
#define DS_SK_RAW 0
#define DS_SK_ARITHMETIC 1
#define DS_SK_DELTA 2
#define DS_SK_MAX_ARITH (1u << 22)
#define DS_SK_ARITH_KEY_COST 128u

static int dyn_sk_is_int(double v)
{
    return v == v && v >= -9007199254740992.0 && v <= 9007199254740992.0 && v == (double)(int64_t)v;
}

static double* dyn_num_read_pairs(dyn_de_t* r, uint32_t first, uint32_t per,
    uint32_t* count, uint32_t* elems)
{
    uint32_t n, e;
    uint8_t kind;
    double* v;

    if (first == DS_NUM_EXT) {
        if (dyn_num_read_header(r, &e, &kind) < 0 || (per > 1 && (e % per)))
            return NULL;
        n = e / per;
    } else {
        uint64_t e64 = (uint64_t)first * per;
        n = first;
        kind = DS_NUM_RAW;
        if (e64 > UINT32_MAX || e64 * 8 > (uint64_t)dyn_de_left(r))
            return NULL;
        e = (uint32_t)e64;
    }
    v = (double*)dyn_nat_calloc(e ? e : 1, sizeof(double));
    if (!v)
        return NULL;
    if (dyn_num_fill(r, kind, v, e) < 0) {
        dyn_nat_free(v);
        return NULL;
    }
    *count = n;
    *elems = e;
    return v;
}

static int dyn_skip_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj,
    JSClassID id, int with_values)
{
    dyn_skip_t* s = JS_GetOpaque(obj, id);
    JSValue arr;
    uint32_t n, i;
    double* keys;
    int r;

    if (!s)
        return -1;
    n = dyn_btree_size(s->b);
    keys = (double*)dyn_nat_malloc((size_t)(n ? n : 1) * sizeof(double));
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
                dyn_nat_free(keys);
                return -1;
            }
        } else if (all_int && n >= 1) {
            if (dyn_ser_u32(w, DS_SKIP_EXT) < 0 || dyn_ser_u8(w, DS_SK_DELTA) < 0 || dyn_ser_u32(w, n) < 0) {
                dyn_nat_free(keys);
                return -1;
            }
            for (i = 0; i < n; i++) {
                if (i == 0) {
                    if (dyn_ser_f64(w, keys[0]) < 0) {
                        dyn_nat_free(keys);
                        return -1;
                    }
                } else {
                    double d = keys[i] - keys[i - 1];
                    if (d < 0) {
                        dyn_nat_free(keys);
                        return -1;
                    }
                    if (dyn_sk_uvarint(w, (uint64_t)d) < 0) {
                        dyn_nat_free(keys);
                        return -1;
                    }
                }
            }
        } else {
            if (dyn_ser_u32(w, DS_SKIP_EXT) < 0 || dyn_ser_u8(w, DS_SK_RAW) < 0 || dyn_ser_u32(w, n) < 0) {
                dyn_nat_free(keys);
                return -1;
            }
            for (i = 0; i < n; i++)
                if (dyn_ser_f64(w, keys[i]) < 0) {
                    dyn_nat_free(keys);
                    return -1;
                }
        }
    }
    dyn_nat_free(keys);
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
                        JS_DupValue(ctx, ds2_value(v)),
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

static int dyn_sortedset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    return dyn_skip_write(ctx, w, obj, dyn_sortedset_class_id, 0);
}

static int dyn_sortedmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    return dyn_skip_write(ctx, w, obj, dyn_sortedmap_class_id, 1);
}

static JSValue dyn_skip_read(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int with_values)
{
    uint32_t n, i;
    JSValue obj, vals = JS_UNDEFINED;
    double* keys;

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
        keys = dyn_nat_calloc(n ? n : 1, sizeof(double));
        if (!keys)
            return JS_ThrowOutOfMemory(ctx);
        switch (kind) {
        case DS_SK_ARITHMETIC: {
            double k0 = dyn_de_f64(r), step = dyn_de_f64(r);
            if (!dyn_de_ok(r)) {
                dyn_nat_free(keys);
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
                        dyn_nat_free(keys);
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
        dyn_nat_free(keys);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (with_values) {
        vals = dyn_ds_read_values(ctx, r, n);
        if (JS_IsException(vals)) {
            dyn_nat_free(keys);
            return vals;
        }
    }
    obj = dyn_ds_construct(ctx, id, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(keys);
        return obj;
    }
    for (i = 0; i < n; i++) {
        JSValue args[2];
        int bad;
        args[0] = JS_NewFloat64(ctx, keys[i]);
        args[1] = with_values ? JS_GetPropertyUint32(ctx, vals, i)
                              : JS_UNDEFINED;
        bad = dyn_ds_invoke(ctx, obj, method, with_values ? 2 : 1,
                  (JSValueConst*)args)
            < 0;
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (bad) {
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            dyn_nat_free(keys);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(keys);
    return obj;
}

static JSValue dyn_sortedset_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts)
{
    (void)opts;
    return dyn_skip_read(ctx, r, dyn_sortedset_class_id, "add", 0);
}

static JSValue dyn_sortedmap_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts)
{
    (void)opts;
    return dyn_skip_read(ctx, r, dyn_sortedmap_class_id, "set", 1);
}

static JSValue dyn_deque_val_at(void* native, uint32_t i)
{
    dyn_deque_t* d = (dyn_deque_t*)native;
    return d->buf[dyn_deque_idx(d, i)];
}

static int dyn_deque_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_deque_t* d = JS_GetOpaque(obj, dyn_deque_class_id);
    if (!d || dyn_ser_u32(w, d->count) < 0)
        return -1;
    return dyn_ds_write_values(ctx, w, d, d->count, dyn_deque_val_at);
}

static JSValue dyn_ringbuf_val_at(void* native, uint32_t i)
{
    dyn_ringbuf_t* rb = (dyn_ringbuf_t*)native;
    return rb->buf[dyn_ringbuf_idx(rb, i)];
}

static int dyn_ringbuf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_ringbuf_t* rb = JS_GetOpaque(obj, dyn_ringbuf_class_id);
    if (!rb || dyn_ser_u32(w, rb->cap) < 0 || dyn_ser_u32(w, rb->count) < 0)
        return -1;
    return dyn_ds_write_values(ctx, w, rb, rb->count, dyn_ringbuf_val_at);
}

static int dyn_heap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_heap_t* h = JS_GetOpaque(obj, dyn_heap_class_id);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (!h || JS_IsException(arr))
        return -1;
    if (dyn_ser_u32(w, h->count) < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    for (i = 0; i < h->count; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, h->items[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue dyn_ds_read_pushed(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int cap_arg,
    JSValueConst ctor_arg)
{
    uint32_t cap = 0, n, i;
    JSValue obj, vals, arg;

    if (cap_arg) {
        cap = dyn_de_u32(r);
        if (!dyn_de_ok(r) || cap == 0)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (dyn_de_count(r, &n, 0) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (cap_arg && n > cap)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    vals = dyn_ds_read_values(ctx, r, n);
    if (JS_IsException(vals))
        return vals;
    arg = cap_arg ? JS_NewInt64(ctx, (int64_t)cap) : JS_UNDEFINED;
    if (!JS_IsUndefined(ctor_arg))
        obj = dyn_ds_construct(ctx, id, 1, &ctor_arg);
    else
        obj = dyn_ds_construct(ctx, id, cap_arg ? 1 : 0,
            (JSValueConst*)&arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, vals);
        return obj;
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int bad = dyn_ds_invoke(ctx, obj, method, 1, (JSValueConst*)&v) < 0;
        JS_FreeValue(ctx, v);
        if (bad) {
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, vals);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, vals);
    return obj;
}

static JSValue dyn_deque_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dyn_deque_class_id, "pushBack", 0,
        JS_UNDEFINED);
}

static JSValue dyn_ringbuf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dyn_ringbuf_class_id, "push", 1,
        JS_UNDEFINED);
}

static int dyn_list_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    JSValue arr, len, fn;
    uint32_t n = 0;
    int r;

    fn = JS_GetPropertyStr(ctx, obj, "toArray");
    if (JS_IsException(fn))
        return -1;
    arr = JS_Call(ctx, fn, obj, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return -1;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &n, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return -1;
    }
    JS_FreeValue(ctx, len);
    if (dyn_ser_u32(w, n) < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue dyn_list_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    (void)opts;
    return dyn_ds_read_pushed(ctx, r, dyn_list_class_id, "pushBack", 0,
        JS_UNDEFINED);
}

static JSValue dyn_heap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    if (!JS_IsUndefined(opts) && !JS_IsFunction(ctx, opts))
        return JS_ThrowTypeError(ctx,
            "decoding a Heap takes its comparator or nothing: "
            "Heap.deserialize(bytes, (a, b) => a - b), or "
            "Heap.deserialize(bytes) for natural order");
    return dyn_ds_read_pushed(ctx, r, dyn_heap_class_id, "push", 0, opts);
}

static const dyn_codec_t dyn_ds_codecs[] = {
    { 0, DYN_TID_BITSET, "BitSet", dyn_bitset_write, dyn_bitset_read },
    { 0, DYN_TID_UNIONFIND, "UnionFind", dyn_uf_write, dyn_uf_read },
    { 0, DYN_TID_DEQUE, "Deque", dyn_deque_write, dyn_deque_read },
    { 0, DYN_TID_FENWICK, "Fenwick", dyn_fenwick_write, dyn_fenwick_read },
    { 0, DYN_TID_RINGBUFFER, "RingBuffer", dyn_ringbuf_write, dyn_ringbuf_read },
    { 0, DYN_TID_SEGTREE, "SegTree", dyn_segtree_write, dyn_segtree_read },
    { 0, DYN_TID_BLOOMFILTER, "BloomFilter", dyn_bloom_write, dyn_bloom_read },
    { 0, DYN_TID_TRIE, "Trie", dyn_trie_write, dyn_trie_read },
    { 0, DYN_TID_LRU, "LRU", dyn_lru_write, dyn_lru_read },
    { 0, DYN_TID_SORTEDSET, "SortedSet", dyn_sortedset_write, dyn_sortedset_read },
    { 0, DYN_TID_SORTEDMAP, "SortedMap", dyn_sortedmap_write, dyn_sortedmap_read },
    { 0, DYN_TID_HEAP, "Heap", dyn_heap_write, dyn_heap_read },
    { 0, DYN_TID_LIST, "List", dyn_list_write, dyn_list_read },
};

static int dyn_ds_register_codecs(void)
{
    static const JSClassID* const ids[] = {
        &dyn_bitset_class_id,
        &dyn_uf_class_id,
        &dyn_deque_class_id,
        &dyn_fenwick_class_id,
        &dyn_ringbuf_class_id,
        &dyn_segtree_class_id,
        &dyn_bloom_class_id,
        &dyn_trie_class_id,
        &dyn_lru_class_id,
        &dyn_sortedset_class_id,
        &dyn_sortedmap_class_id,
        &dyn_heap_class_id,
        &dyn_list_class_id,
    };
    size_t i;
    for (i = 0; i < countof(dyn_ds_codecs); i++) {
        dyn_codec_t c = dyn_ds_codecs[i];
        c.class_id = *ids[i];
        if (dyn_codec_register(&c) < 0)
            return -1;
    }
    return 0;
}

#define DS2_WRAPPER(name, coretype) \
    typedef struct {                \
        JSRuntime* rt;              \
        coretype* c;                \
    } name

DS2_WRAPPER(ds2_mset_w, dyn_mset_t);
DS2_WRAPPER(ds2_mmap_w, dyn_mmap_t);
DS2_WRAPPER(ds2_bimap_w, dyn_bimap_t);
DS2_WRAPPER(ds2_table_w, dyn_table_t);
DS2_WRAPPER(ds2_rset_w, dyn_rset_t);
DS2_WRAPPER(ds2_rmap_w, dyn_rmap_t);
DS2_WRAPPER(ds2_itree_w, dyn_itree_t);
DS2_WRAPPER(ds2_mmheap_w, dyn_mmheap_t);
DS2_WRAPPER(ds2_cms_w, dyn_cms_t);
DS2_WRAPPER(ds2_hll_w, dyn_hll_t);
DS2_WRAPPER(ds2_btree_w, dyn_btree_t);

#define DS2_FINALIZER(pfx)                                  \
    static void pfx##_finalizer(JSRuntime* rt, JSValue val) \
    {                                                       \
        (void)rt;                                           \
        pfx##_free(JS_GetOpaque(val, pfx##_class_id));      \
    }

typedef struct {
    const char* p;
    size_t n;
} ds2_str;

static int ds2_str_get(JSContext* ctx, ds2_str* s, JSValueConst v)
{
    s->p = JS_ToCStringLen(ctx, &s->n, v);
    return s->p ? 0 : -1;
}

static void ds2_str_release(JSContext* ctx, ds2_str* s)
{
    if (s->p)
        JS_FreeCString(ctx, s->p);
}

static JSValue ds2_new_str(JSContext* ctx, const char* p, size_t n)
{
    return JS_NewStringLen(ctx, p, n);
}

static JSValue ds2_oom(JSContext* ctx)
{
    return JS_ThrowOutOfMemory(ctx);
}

static JSValue ds2_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method)
{
    JSValue fn, arr, values, iter;

    fn = JS_GetPropertyStr(ctx, this_val, method);
    if (JS_IsException(fn))
        return fn;
    arr = JS_Call(ctx, fn, this_val, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(arr))
        return arr;
    values = JS_GetPropertyStr(ctx, arr, "values");
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, arr);
        return values;
    }
    iter = JS_Call(ctx, values, arr, 0, NULL);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, arr);
    return iter;
}

#define DS2_ITERATOR(pfx, method)                                        \
    static JSValue pfx##_iterator(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                    \
    {                                                                    \
        (void)argc;                                                      \
        (void)argv;                                                      \
        return ds2_iter_via(ctx, this_val, method);                      \
    }

DS2_ITERATOR(dyn_lru, "entries")

static void ds2_mset_free(void* native)
{
    ds2_mset_w* w = (ds2_mset_w*)native;
    if (!w)
        return;
    dyn_mset_free(w->c);
    dyn_nat_free(w);
}

static JSClassID ds2_mset_class_id;
DS2_FINALIZER(ds2_mset)
static const JSClassDef ds2_mset_class = {
    "Multiset",
    .finalizer = ds2_mset_finalizer,
};

static JSValue ds2_mset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mset_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mset_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mset_class_id, w, ds2_mset_free);
}

static JSValue ds2_u64_js(JSContext* ctx, uint64_t v)
{
    return v > (uint64_t)INT64_MAX ? JS_NewFloat64(ctx, (double)v)
                                   : JS_NewInt64(ctx, (int64_t)v);
}

static JSValue ds2_mset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_mset_w* w;
    ds2_str k;
    int64_t n = 1;
    uint64_t out = 0;

    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx, "count must be >= 0");
    }
    if (magic)
        n = -n;
    if (dyn_mset_add(w->c, k.p, k.n, n, &out) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, out);
}

static JSValue ds2_mset_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_mset_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, c);
}

static JSValue ds2_mset_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_mset_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, c > 0);
}

static JSValue ds2_mset_set_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    int64_t n;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setCount(key, count): count is required");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx, "count must be >= 0");
    }
    if (dyn_mset_set_count(w->c, k.p, k.n, (uint64_t)n) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mset_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w;
    ds2_str k;
    int had;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    had = dyn_mset_count(w->c, k.p, k.n) > 0;
    if (had)
        dyn_mset_set_count(w->c, k.p, k.n, 0);
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, had);
}

static JSValue ds2_mset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_mset_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_mset_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int entries)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_mset_distinct(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char* k;
        size_t kl;
        uint64_t c;
        JSValue item;
        dyn_mset_at(w->c, i, &k, &kl, &c);
        if (entries) {
            item = JS_NewArray(ctx);
            if (JS_IsException(item))
                goto fail;
            if (JS_DefinePropertyValueUint32(ctx, item, 0,
                    ds2_new_str(ctx, k, kl),
                    JS_PROP_C_W_E)
                    < 0
                || JS_DefinePropertyValueUint32(ctx, item, 1,
                       ds2_u64_js(ctx, c),
                       JS_PROP_C_W_E)
                    < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
        } else {
            item = ds2_new_str(ctx, k, kl);
            if (JS_IsException(item))
                goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, item,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_mset_total(JSContext* ctx, JSValueConst this_val)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return ds2_u64_js(ctx, dyn_mset_total(w->c));
}

static JSValue ds2_mset_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mset_w* w = (ds2_mset_w*)dyn_plain_get(ctx, this_val, ds2_mset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mset_distinct(w->c));
}

DS2_ITERATOR(ds2_mset, "entrySet")

static const JSCFunctionListEntry ds2_mset_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_mset_iterator),
    JS_CFUNC_MAGIC_DEF("add", 1, ds2_mset_add, 0),
    JS_CFUNC_MAGIC_DEF("remove", 1, ds2_mset_add, 1),
    JS_CFUNC_DEF("count", 1, ds2_mset_count),
    JS_CFUNC_DEF("has", 1, ds2_mset_has),
    JS_CFUNC_DEF("setCount", 2, ds2_mset_set_count),
    JS_CFUNC_DEF("delete", 1, ds2_mset_delete),
    JS_CFUNC_DEF("clear", 0, ds2_mset_clear),
    JS_CFUNC_MAGIC_DEF("elementSet", 0, ds2_mset_set, 0),
    JS_CFUNC_MAGIC_DEF("entrySet", 0, ds2_mset_set, 1),
    JS_CGETSET_DEF("size", ds2_mset_size, NULL),
    JS_CGETSET_DEF("totalSize", ds2_mset_total, NULL),
};

static void ds2_mmap_free(void* native)
{
    ds2_mmap_w* w = (ds2_mmap_w*)native;
    if (!w)
        return;
    dyn_mmap_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_mmap_class_id;
DS2_FINALIZER(ds2_mmap)
static void ds2_mmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_mmap_w* w = JS_GetOpaque(val, ds2_mmap_class_id);
    uint32_t i, j, nk, nv;
    if (!w)
        return;
    nk = dyn_mmap_keys(w->c);
    for (i = 0; i < nk; i++) {
        const dyn_cell_t* cells = dyn_mmap_cells_at(w->c, i, &nv);
        for (j = 0; j < nv; j++)
            JS_MarkValue(rt, ds2_value(&cells[j]), mark_func);
    }
}
static const JSClassDef ds2_mmap_class = {
    "Multimap",
    .finalizer = ds2_mmap_finalizer,
    .gc_mark = ds2_mmap_gc_mark,
};

static JSValue ds2_mmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mmap_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mmap_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mmap_class_id, w, ds2_mmap_free);
}

static JSValue ds2_mmap_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    cell = ds2_cell(JS_DupValue(ctx, argv[1]));
    if (dyn_mmap_put(w->c, k.p, k.n, &cell) < 0) {
        JS_FreeValue(ctx, ds2_value(&cell));
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    const dyn_cell_t* cells;
    uint32_t n, i;
    JSValue arr;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    cells = dyn_mmap_cells_for(w->c, k.p, k.n, &n);
    ds2_str_release(ctx, &k);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx,
                    ds2_value(&cells[i])),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static JSValue ds2_mmap_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t n;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    n = dyn_mmap_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_NewInt64(ctx, (int64_t)n);
}

static JSValue ds2_mmap_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t n;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    n = dyn_mmap_remove_key(w->c, k.p, k.n, ds2_free_cell, w->rt);
    ds2_str_release(ctx, &k);
    return JS_NewInt64(ctx, (int64_t)n);
}

static JSValue ds2_mmap_remove_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmap_w* w;
    ds2_str k;
    uint32_t idx;
    dyn_cell_t out;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "removeAt(key, index): index is required");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &idx, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (!dyn_mmap_remove_at(w->c, k.p, k.n, idx, &out)) {
        ds2_str_release(ctx, &k);
        return JS_UNDEFINED;
    }
    ds2_str_release(ctx, &k);
    return ds2_value(&out);
}

static JSValue ds2_mmap_view(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int entries)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    JSValue arr;
    uint32_t i, nk, out = 0;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    nk = dyn_mmap_keys(w->c);
    for (i = 0; i < nk; i++) {
        const char* k;
        size_t kl;
        uint32_t nv, j;
        const dyn_cell_t* cells;
        dyn_mmap_key_at(w->c, i, &k, &kl, &nv);
        if (!entries) {
            if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                    ds2_new_str(ctx, k, kl),
                    JS_PROP_C_W_E)
                < 0)
                goto fail;
            continue;
        }
        cells = dyn_mmap_cells_at(w->c, i, &nv);
        for (j = 0; j < nv; j++) {
            JSValue pair = JS_NewArray(ctx);
            if (JS_IsException(pair))
                goto fail;
            if (JS_DefinePropertyValueUint32(ctx, pair, 0,
                    ds2_new_str(ctx, k, kl),
                    JS_PROP_C_W_E)
                    < 0
                || JS_DefinePropertyValueUint32(
                       ctx, pair, 1, JS_DupValue(ctx, ds2_value(&cells[j])),
                       JS_PROP_C_W_E)
                    < 0) {
                JS_FreeValue(ctx, pair);
                goto fail;
            }
            if (JS_DefinePropertyValueUint32(ctx, arr, out++, pair,
                    JS_PROP_C_W_E)
                < 0)
                goto fail;
        }
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_mmap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmap_size(w->c));
}

static JSValue ds2_mmap_keycount(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmap_w* w = (ds2_mmap_w*)dyn_plain_get(ctx, this_val, ds2_mmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmap_keys(w->c));
}

DS2_ITERATOR(ds2_mmap, "entries")

static const JSCFunctionListEntry ds2_mmap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_mmap_iterator),
    JS_CFUNC_DEF("put", 2, ds2_mmap_put),
    JS_CFUNC_DEF("get", 1, ds2_mmap_get),
    JS_CFUNC_DEF("count", 1, ds2_mmap_count),
    JS_CFUNC_DEF("delete", 1, ds2_mmap_delete),
    JS_CFUNC_DEF("removeAt", 2, ds2_mmap_remove_at),
    JS_CFUNC_MAGIC_DEF("keys", 0, ds2_mmap_view, 0),
    JS_CFUNC_MAGIC_DEF("entries", 0, ds2_mmap_view, 1),
    JS_CGETSET_DEF("size", ds2_mmap_size, NULL),
    JS_CGETSET_DEF("keyCount", ds2_mmap_keycount, NULL),
};

static void ds2_bimap_free(void* native)
{
    ds2_bimap_w* w = (ds2_bimap_w*)native;
    if (!w)
        return;
    dyn_bimap_free(w->c);
    dyn_nat_free(w);
}

static JSClassID ds2_bimap_class_id;
DS2_FINALIZER(ds2_bimap)
static const JSClassDef ds2_bimap_class = {
    "BiMap",
    .finalizer = ds2_bimap_finalizer,
};

static JSValue ds2_bimap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_bimap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_bimap_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_bimap_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_bimap_class_id, w, ds2_bimap_free);
}

static JSValue ds2_bimap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int force)
{
    ds2_bimap_w* w;
    ds2_str k, v;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &v, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        ds2_str_release(ctx, &v);
        return JS_EXCEPTION;
    }
    r = dyn_bimap_put(w->c, k.p, k.n, v.p, v.n, force);
    ds2_str_release(ctx, &k);
    ds2_str_release(ctx, &v);
    if (r == DYN_BIMAP_VALUE_TAKEN)
        return JS_ThrowTypeError(ctx,
            "value is already bound to another key; use forceSet to rebind");
    if (r < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_bimap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int inverse)
{
    ds2_bimap_w* w;
    ds2_str k;
    const char* r;
    size_t rn = 0;
    JSValue out;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    r = inverse ? dyn_bimap_key_for(w->c, k.p, k.n, &rn)
                : dyn_bimap_get(w->c, k.p, k.n, &rn);
    out = r ? ds2_new_str(ctx, r, rn) : JS_UNDEFINED;
    ds2_str_release(ctx, &k);
    return out;
}

static JSValue ds2_bimap_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_bimap_w* w;
    ds2_str k;
    size_t rn = 0;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        r = dyn_bimap_get(w->c, k.p, k.n, &rn) != NULL;
        break;
    case 1:
        r = dyn_bimap_key_for(w->c, k.p, k.n, &rn) != NULL;
        break;
    case 2:
        r = dyn_bimap_remove(w->c, k.p, k.n);
        break;
    default:
        r = dyn_bimap_remove_value(w->c, k.p, k.n);
        break;
    }
    ds2_str_release(ctx, &k);
    return JS_NewBool(ctx, r);
}

static JSValue ds2_bimap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int inverse)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_bimap_size(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char *k, *v;
        size_t kl, vl;
        JSValue pair;
        dyn_bimap_at(w->c, i, &k, &kl, &v, &vl);
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair))
            goto fail;
        if (JS_DefinePropertyValueUint32(
                ctx, pair, 0,
                inverse ? ds2_new_str(ctx, v, vl) : ds2_new_str(ctx, k, kl),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(
                   ctx, pair, 1,
                   inverse ? ds2_new_str(ctx, k, kl) : ds2_new_str(ctx, v, vl),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, pair);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, pair,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_bimap_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_bimap_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_bimap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_bimap_w* w = (ds2_bimap_w*)dyn_plain_get(ctx, this_val, ds2_bimap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_bimap_size(w->c));
}

DS2_ITERATOR(ds2_bimap, "entries")

static const JSCFunctionListEntry ds2_bimap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_bimap_iterator),
    JS_CFUNC_MAGIC_DEF("set", 2, ds2_bimap_set, 0),
    JS_CFUNC_MAGIC_DEF("forceSet", 2, ds2_bimap_set, 1),
    JS_CFUNC_MAGIC_DEF("get", 1, ds2_bimap_get, 0),
    JS_CFUNC_MAGIC_DEF("keyOf", 1, ds2_bimap_get, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, ds2_bimap_op, 0),
    JS_CFUNC_MAGIC_DEF("hasValue", 1, ds2_bimap_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, ds2_bimap_op, 2),
    JS_CFUNC_MAGIC_DEF("deleteValue", 1, ds2_bimap_op, 3),
    JS_CFUNC_MAGIC_DEF("entries", 0, ds2_bimap_entries, 0),
    JS_CFUNC_MAGIC_DEF("inverseEntries", 0, ds2_bimap_entries, 1),
    JS_CFUNC_DEF("clear", 0, ds2_bimap_clear),
    JS_CGETSET_DEF("size", ds2_bimap_size, NULL),
};

static void ds2_table_free(void* native)
{
    ds2_table_w* w = (ds2_table_w*)native;
    if (!w)
        return;
    dyn_table_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_table_class_id;
DS2_FINALIZER(ds2_table)
static void ds2_table_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_table_w* w = JS_GetOpaque(val, ds2_table_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_table_size(w->c);
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        JS_MarkValue(rt, ds2_value(v), mark_func);
    }
}
static const JSClassDef ds2_table_class = {
    "Table",
    .finalizer = ds2_table_finalizer,
    .gc_mark = ds2_table_gc_mark,
};

static JSValue ds2_table_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_table_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_table_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_table_class_id, w, ds2_table_free);
}

static JSValue ds2_table_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w;
    ds2_str r, c;
    dyn_cell_t cell, old;
    int rep;
    (void)argc;
    if (ds2_str_get(ctx, &r, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &c, argv[1])) {
        ds2_str_release(ctx, &r);
        return JS_EXCEPTION;
    }
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &r);
        ds2_str_release(ctx, &c);
        return JS_EXCEPTION;
    }
    cell = ds2_cell(JS_DupValue(ctx, argv[2]));
    rep = dyn_table_put(w->c, r.p, r.n, c.p, c.n, &cell, &old);
    ds2_str_release(ctx, &r);
    ds2_str_release(ctx, &c);
    if (rep < 0) {
        JS_FreeValue(ctx, ds2_value(&cell));
        return ds2_oom(ctx);
    }
    if (rep)
        JS_FreeValue(ctx, ds2_value(&old));
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_table_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_table_w* w;
    ds2_str r, c;
    JSValue out;
    (void)argc;
    if (ds2_str_get(ctx, &r, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &c, argv[1])) {
        ds2_str_release(ctx, &r);
        return JS_EXCEPTION;
    }
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &r);
        ds2_str_release(ctx, &c);
        return JS_EXCEPTION;
    }
    if (magic == 2) {
        dyn_cell_t old;
        int had = dyn_table_remove(w->c, r.p, r.n, c.p, c.n, &old);
        if (had)
            JS_FreeValue(ctx, ds2_value(&old));
        out = JS_NewBool(ctx, had);
    } else {
        const dyn_cell_t* v = dyn_table_get(w->c, r.p, r.n, c.p, c.n);
        out = magic == 1 ? JS_NewBool(ctx, v != NULL)
                         : (v ? JS_DupValue(ctx, ds2_value(v)) : JS_UNDEFINED);
    }
    ds2_str_release(ctx, &r);
    ds2_str_release(ctx, &c);
    return out;
}

static JSValue ds2_table_slice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int by_column)
{
    ds2_table_w* w;
    ds2_str key;
    JSValue arr;
    uint32_t i, n, out = 0;
    (void)argc;
    if (ds2_str_get(ctx, &key, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &key);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        ds2_str_release(ctx, &key);
        return arr;
    }
    (void)n;
    for (i = dyn_table_slice_first(w->c, key.p, key.n, by_column);
        i != UINT32_MAX;
        i = dyn_table_slice_next(w->c, i, by_column)) {
        const char *r, *c, *label;
        size_t rn, cn, ln;
        const dyn_cell_t* v;
        JSValue pair;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        label = by_column ? r : c;
        ln = by_column ? rn : cn;
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, pair, 0,
                ds2_new_str(ctx, label, ln),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, pair, 1,
                   JS_DupValue(ctx, ds2_value(v)),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, pair);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, out++, pair,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    ds2_str_release(ctx, &key);
    return arr;
fail:
    ds2_str_release(ctx, &key);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_table_cells(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_table_size(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        JSValue t;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        t = JS_NewArray(ctx);
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, t, 0, ds2_new_str(ctx, r, rn),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 1, ds2_new_str(ctx, c, cn),
                   JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 2,
                   JS_DupValue(ctx, ds2_value(v)),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, t);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_table_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_table_w* w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_table_size(w->c));
}

DS2_ITERATOR(ds2_table, "cells")

static const JSCFunctionListEntry ds2_table_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_table_iterator),
    JS_CFUNC_DEF("put", 3, ds2_table_put),
    JS_CFUNC_MAGIC_DEF("get", 2, ds2_table_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 2, ds2_table_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 2, ds2_table_op, 2),
    JS_CFUNC_MAGIC_DEF("row", 1, ds2_table_slice, 0),
    JS_CFUNC_MAGIC_DEF("column", 1, ds2_table_slice, 1),
    JS_CFUNC_DEF("cells", 0, ds2_table_cells),
    JS_CGETSET_DEF("size", ds2_table_size, NULL),
};

static void ds2_rset_free(void* native)
{
    ds2_rset_w* w = (ds2_rset_w*)native;
    if (!w)
        return;
    dyn_rset_free(w->c);
    dyn_nat_free(w);
}

static JSClassID ds2_rset_class_id;
DS2_FINALIZER(ds2_rset)
static const JSClassDef ds2_rset_class = {
    "RangeSet",
    .finalizer = ds2_rset_finalizer,
};

static JSValue ds2_rset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_rset_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_rset_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_rset_class_id, w, ds2_rset_free);
}

static int ds2_two_bounds(JSContext* ctx, double* lo, double* hi,
    JSValueConst a, JSValueConst b)
{
    if (JS_ToFloat64(ctx, lo, a) || JS_ToFloat64(ctx, hi, b))
        return -1;
    if (isnan(*lo) || isnan(*hi)) {
        JS_ThrowRangeError(ctx, "interval bounds must not be NaN");
        return -1;
    }
    return 0;
}

static JSValue ds2_rset_edit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int remove)
{
    ds2_rset_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if ((remove ? dyn_rset_remove(w->c, lo, hi)
                : dyn_rset_add(w->c, lo, hi))
        < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rset_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_rset_contains(w->c, x));
}

static JSValue ds2_rset_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int intersects)
{
    ds2_rset_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, intersects ? dyn_rset_intersects(w->c, lo, hi) : dyn_rset_encloses(w->c, lo, hi));
}

static JSValue ds2_spans_array(JSContext* ctx, const dyn_rset_t* s)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i, n;
    if (JS_IsException(arr))
        return arr;
    n = dyn_rset_count(s);
    for (i = 0; i < n; i++) {
        double lo, hi;
        JSValue pair;
        dyn_rset_at(s, i, &lo, &hi);
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, pair, 0, JS_NewFloat64(ctx, lo),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, pair, 1, JS_NewFloat64(ctx, hi),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, pair);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, pair,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_rset_ranges(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    return ds2_spans_array(ctx, w->c);
}

static JSValue ds2_rset_complement(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w;
    dyn_rset_t* out;
    double lo, hi;
    JSValue r;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    out = dyn_rset_new();
    if (!out)
        return ds2_oom(ctx);
    if (dyn_rset_complement(w->c, lo, hi, out) < 0) {
        dyn_rset_free(out);
        return ds2_oom(ctx);
    }
    r = ds2_spans_array(ctx, out);
    dyn_rset_free(out);
    return r;
}

static JSValue ds2_rset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_rset_clear(w->c);
    return JS_UNDEFINED;
}

static JSValue ds2_rset_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_rset_count(w->c));
}

static JSValue ds2_rset_measure(JSContext* ctx, JSValueConst this_val)
{
    ds2_rset_w* w = (ds2_rset_w*)dyn_plain_get(ctx, this_val, ds2_rset_class_id);
    double total = 0;
    uint32_t i, n;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_rset_count(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        dyn_rset_at(w->c, i, &lo, &hi);
        total += hi - lo;
    }
    return JS_NewFloat64(ctx, total);
}

DS2_ITERATOR(ds2_rset, "ranges")

static const JSCFunctionListEntry ds2_rset_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_rset_iterator),
    JS_CFUNC_MAGIC_DEF("add", 2, ds2_rset_edit, 0),
    JS_CFUNC_MAGIC_DEF("remove", 2, ds2_rset_edit, 1),
    JS_CFUNC_DEF("contains", 1, ds2_rset_contains),
    JS_CFUNC_MAGIC_DEF("encloses", 2, ds2_rset_query, 0),
    JS_CFUNC_MAGIC_DEF("intersects", 2, ds2_rset_query, 1),
    JS_CFUNC_DEF("ranges", 0, ds2_rset_ranges),
    JS_CFUNC_DEF("complement", 2, ds2_rset_complement),
    JS_CFUNC_DEF("clear", 0, ds2_rset_clear),
    JS_CGETSET_DEF("size", ds2_rset_size, NULL),
    JS_CGETSET_DEF("measure", ds2_rset_measure, NULL),
};

static void ds2_rmap_free(void* native)
{
    ds2_rmap_w* w = (ds2_rmap_w*)native;
    if (!w)
        return;
    dyn_rmap_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_rmap_class_id;
DS2_FINALIZER(ds2_rmap)
static void ds2_rmap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_rmap_w* w = JS_GetOpaque(val, ds2_rmap_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_rmap_count(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        dyn_rmap_at(w->c, i, &lo, &hi, &v);
        JS_MarkValue(rt, ds2_value(v), mark_func);
    }
}
static const JSClassDef ds2_rmap_class = {
    "RangeMap",
    .finalizer = ds2_rmap_finalizer,
    .gc_mark = ds2_rmap_gc_mark,
};

static JSValue ds2_rmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_rmap_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_rmap_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_rmap_class_id, w, ds2_rmap_free);
}

static JSValue ds2_rmap_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    double lo, hi;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(hi > lo))
        return JS_DupValue(ctx, this_val);
    cell = ds2_cell(JS_DupValue(ctx, argv[2]));
    if (dyn_rmap_put(w->c, lo, hi, &cell, ds2_dup_cell, ds2_free_cell,
            w->rt)
        < 0) {
        JS_FreeValue(ctx, ds2_value(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rmap_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    const dyn_cell_t* v;
    double x;
    (void)argc;
    if (JS_ToFloat64(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    v = dyn_rmap_get(w->c, x);
    return v ? JS_DupValue(ctx, ds2_value(v)) : JS_UNDEFINED;
}

static JSValue ds2_rmap_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w;
    double lo, hi;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (dyn_rmap_remove(w->c, lo, hi, ds2_dup_cell, ds2_free_cell, w->rt) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_rmap_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_rmap_w* w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_rmap_count(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        JSValue t;
        dyn_rmap_at(w->c, i, &lo, &hi, &v);
        t = JS_NewArray(ctx);
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, t, 0, JS_NewFloat64(ctx, lo),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 1, JS_NewFloat64(ctx, hi),
                   JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 2,
                   JS_DupValue(ctx, ds2_value(v)),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, t);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_rmap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_rmap_w* w = (ds2_rmap_w*)dyn_plain_get(ctx, this_val, ds2_rmap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_rmap_count(w->c));
}

DS2_ITERATOR(ds2_rmap, "entries")

static const JSCFunctionListEntry ds2_rmap_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_rmap_iterator),
    JS_CFUNC_DEF("put", 3, ds2_rmap_put),
    JS_CFUNC_DEF("get", 1, ds2_rmap_get),
    JS_CFUNC_DEF("remove", 2, ds2_rmap_remove),
    JS_CFUNC_DEF("entries", 0, ds2_rmap_entries),
    JS_CGETSET_DEF("size", ds2_rmap_size, NULL),
};

static void ds2_btree_free(void* native)
{
    ds2_btree_w* w = (ds2_btree_w*)native;
    if (!w)
        return;
    dyn_btree_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_btree_class_id;
DS2_FINALIZER(ds2_btree)
static void ds2_btree_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_btree_w* w = JS_GetOpaque(val, ds2_btree_class_id);
    dyn_btree_iter it;
    if (!w)
        return;
    if (dyn_btree_iter_begin(w->c, &it))
        do {
            double k;
            const dyn_cell_t* v;
            if (dyn_btree_iter_get(&it, &k, &v))
                JS_MarkValue(rt, ds2_value(v), mark_func);
        } while (dyn_btree_iter_next(&it));
}

static const JSClassDef ds2_btree_class = {
    "BTree",
    .finalizer = ds2_btree_finalizer,
    .gc_mark = ds2_btree_gc_mark,
};

static JSValue ds2_btree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_btree_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_btree_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_btree_class_id, w, ds2_btree_free);
}

static int ds2_btree_key(JSContext* ctx, JSValueConst v, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (*out != *out) {
        JS_ThrowTypeError(ctx, "BTree keys must be ordered numbers, not NaN");
        return -1;
    }
    return 0;
}

static JSValue ds2_btree_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    JSValue dup;
    dyn_cell_t cell, old;
    int replaced = 0;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    dup = JS_DupValue(ctx, argv[1]);
    cell = ds2_cell(dup);
    if (dyn_btree_set(w->c, k, &cell, &old, &replaced) < 0) {
        JS_FreeValue(ctx, dup);
        return ds2_oom(ctx);
    }
    if (replaced)
        ds2_free_cell(w->rt, &old);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_btree_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    const dyn_cell_t* v;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    v = dyn_btree_get(w->c, k);
    return v ? JS_DupValue(ctx, ds2_value(v)) : JS_UNDEFINED;
}

static JSValue ds2_btree_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_btree_get(w->c, k) != NULL);
}

static JSValue ds2_btree_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_btree_w* w;
    double k;
    dyn_cell_t out;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!dyn_btree_del(w->c, k, &out))
        return JS_NewBool(ctx, 0);
    ds2_free_cell(w->rt, &out);
    return JS_NewBool(ctx, 1);
}

static JSValue ds2_btree_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double k;
    (void)argc;
    (void)argv;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(magic ? dyn_btree_last(w->c, &k) : dyn_btree_first(w->c, &k)))
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, k);
}

static JSValue ds2_btree_near(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double k, out;
    (void)argc;
    if (ds2_btree_key(ctx, argv[0], &k))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(magic ? dyn_btree_ceil(w->c, k, &out) : dyn_btree_floor(w->c, k, &out)))
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, out);
}

static JSValue ds2_btree_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_btree_w* w;
    double lo = 0, hi = 0;
    JSValue arr;
    dyn_btree_iter it;
    uint32_t out = 0;
    int bounded = !magic && argc >= 2;

    if (bounded && (ds2_btree_key(ctx, argv[0], &lo) || ds2_btree_key(ctx, argv[1], &hi)))
        return JS_EXCEPTION;
    w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (!(bounded ? dyn_btree_iter_seek(w->c, lo, &it)
                  : dyn_btree_iter_begin(w->c, &it)))
        return arr;
    do {
        double k;
        const dyn_cell_t* v;
        JSValue item;
        if (!dyn_btree_iter_get(&it, &k, &v))
            break;
        if (bounded && k > hi)
            break;
        if (magic) {
            item = JS_NewFloat64(ctx, k);
        } else {
            item = JS_NewArray(ctx);
            if (JS_IsException(item))
                goto fail;
            if (JS_DefinePropertyValueUint32(ctx, item, 0, JS_NewFloat64(ctx, k),
                    JS_PROP_C_W_E)
                    < 0
                || JS_DefinePropertyValueUint32(ctx, item, 1,
                       JS_DupValue(ctx, ds2_value(v)),
                       JS_PROP_C_W_E)
                    < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, out++, item,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    } while (dyn_btree_iter_next(&it));
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_btree_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_btree_w* w = (ds2_btree_w*)dyn_plain_get(ctx, this_val, ds2_btree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_btree_size(w->c));
}

DS2_ITERATOR(ds2_btree, "rangeQuery")

static const JSCFunctionListEntry ds2_btree_proto[] = {
    JS_CFUNC_DEF("set", 2, ds2_btree_set),
    JS_CFUNC_DEF("get", 1, ds2_btree_get),
    JS_CFUNC_DEF("has", 1, ds2_btree_has),
    JS_CFUNC_DEF("delete", 1, ds2_btree_delete),
    JS_CFUNC_MAGIC_DEF("firstKey", 0, ds2_btree_end, 0),
    JS_CFUNC_MAGIC_DEF("lastKey", 0, ds2_btree_end, 1),
    JS_CFUNC_MAGIC_DEF("floorKey", 1, ds2_btree_near, 0),
    JS_CFUNC_MAGIC_DEF("ceilKey", 1, ds2_btree_near, 1),
    JS_CFUNC_MAGIC_DEF("rangeQuery", 2, ds2_btree_range, 0),
    JS_CFUNC_MAGIC_DEF("keys", 0, ds2_btree_range, 1),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_btree_iterator),
    JS_CGETSET_DEF("size", ds2_btree_size, NULL),
};

static void ds2_itree_free(void* native)
{
    ds2_itree_w* w = (ds2_itree_w*)native;
    if (!w)
        return;
    dyn_itree_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_itree_class_id;
DS2_FINALIZER(ds2_itree)
static void ds2_itree_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_itree_w* w = JS_GetOpaque(val, ds2_itree_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_itree_size(w->c);
    for (i = 0; i < n; i++) {
        double lo, hi;
        const dyn_cell_t* v;
        dyn_itree_at(w->c, i, &lo, &hi, &v);
        JS_MarkValue(rt, ds2_value(v), mark_func);
    }
}
static const JSClassDef ds2_itree_class = {
    "IntervalTree",
    .finalizer = ds2_itree_finalizer,
    .gc_mark = ds2_itree_gc_mark,
};

static JSValue ds2_itree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_itree_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_itree_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_itree_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_itree_class_id, w, ds2_itree_free);
}

static JSValue ds2_itree_insert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_itree_w* w;
    double lo, hi;
    dyn_cell_t cell;
    (void)argc;
    if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1]))
        return JS_EXCEPTION;
    if (lo > hi)
        return JS_ThrowRangeError(ctx, "IntervalTree: lo must not exceed hi");
    w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    cell = ds2_cell(JS_DupValue(ctx, argv[2]));
    if (dyn_itree_insert(w->c, lo, hi, &cell) < 0) {
        JS_FreeValue(ctx, ds2_value(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_itree_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int point)
{
    ds2_itree_w* w;
    double lo, hi;
    uint32_t *idx = NULL, n, i, cap;
    JSValue arr;

    if (point) {
        if (JS_ToFloat64(ctx, &lo, argv[0]))
            return JS_EXCEPTION;
        if (isnan(lo))
            return JS_ThrowRangeError(ctx, "interval bounds must not be NaN");
        hi = lo;
    } else if (ds2_two_bounds(ctx, &lo, &hi, argv[0], argv[1])) {
        return JS_EXCEPTION;
    }
    (void)argc;
    w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(hi >= lo)) {
        JSValue res = JS_NewArray(ctx);
        if (JS_IsException(res))
            return res;
        return res;
    }
    cap = dyn_itree_size(w->c);
    if (cap) {
        idx = (uint32_t*)dyn_nat_malloc((size_t)cap * sizeof(uint32_t));
        if (!idx)
            return ds2_oom(ctx);
    }
    n = dyn_itree_query(w->c, lo, hi, idx, cap);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_nat_free(idx);
        return arr;
    }
    for (i = 0; i < n; i++) {
        double a, b;
        const dyn_cell_t* v;
        JSValue t;
        dyn_itree_at(w->c, idx[i], &a, &b, &v);
        t = JS_NewArray(ctx);
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, t, 0, JS_NewFloat64(ctx, a),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 1, JS_NewFloat64(ctx, b),
                   JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueUint32(ctx, t, 2,
                   JS_DupValue(ctx, ds2_value(v)),
                   JS_PROP_C_W_E)
                < 0) {
            JS_FreeValue(ctx, t);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    dyn_nat_free(idx);
    return arr;
fail:
    dyn_nat_free(idx);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_itree_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_itree_w* w = (ds2_itree_w*)dyn_plain_get(ctx, this_val, ds2_itree_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_itree_size(w->c));
}

static const JSCFunctionListEntry ds2_itree_proto[] = {
    JS_CFUNC_DEF("insert", 3, ds2_itree_insert),
    JS_CFUNC_MAGIC_DEF("overlapping", 2, ds2_itree_query, 0),
    JS_CFUNC_MAGIC_DEF("at", 1, ds2_itree_query, 1),
    JS_CGETSET_DEF("size", ds2_itree_size, NULL),
};

static void ds2_mmheap_free(void* native)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)native;
    if (!w)
        return;
    dyn_mmheap_free(w->c, ds2_free_cell, w->rt);
    dyn_nat_free(w);
}

static JSClassID ds2_mmheap_class_id;
DS2_FINALIZER(ds2_mmheap)
static void ds2_mmheap_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_mmheap_w* w = JS_GetOpaque(val, ds2_mmheap_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_mmheap_size(w->c);
    for (i = 0; i < n; i++)
        JS_MarkValue(rt, ds2_value(dyn_mmheap_cell_at(w->c, i)), mark_func);
}
static const JSClassDef ds2_mmheap_class = {
    "MinMaxHeap",
    .finalizer = ds2_mmheap_finalizer,
    .gc_mark = ds2_mmheap_gc_mark,
};

static JSValue ds2_mmheap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_mmheap_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_mmheap_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_mmheap_new();
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_mmheap_class_id, w, ds2_mmheap_free);
}

static JSValue ds2_mmheap_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_mmheap_w* w;
    double pri;
    dyn_cell_t cell;

    if (JS_ToFloat64(ctx, &pri, argv[0]))
        return JS_EXCEPTION;
    if (isnan(pri))
        return JS_ThrowRangeError(ctx, "priority must not be NaN");
    w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    if (!w)
        return JS_EXCEPTION;
    cell = ds2_cell(JS_DupValue(ctx, argc >= 2 ? argv[1] : argv[0]));
    if (dyn_mmheap_push(w->c, pri, &cell) < 0) {
        JS_FreeValue(ctx, ds2_value(&cell));
        return ds2_oom(ctx);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_mmheap_take(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    double pri;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (magic < 2) {
        dyn_cell_t out;
        int ok = magic ? dyn_mmheap_pop_max(w->c, &pri, &out)
                       : dyn_mmheap_pop_min(w->c, &pri, &out);
        return ok ? ds2_value(&out) : JS_UNDEFINED;
    } else {
        const dyn_cell_t* v;
        int ok = magic == 3 ? dyn_mmheap_peek_max(w->c, &pri, &v)
                            : dyn_mmheap_peek_min(w->c, &pri, &v);
        return ok ? JS_DupValue(ctx, ds2_value(v)) : JS_UNDEFINED;
    }
}

static JSValue ds2_mmheap_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_mmheap_w* w = (ds2_mmheap_w*)dyn_plain_get(ctx, this_val, ds2_mmheap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_mmheap_size(w->c));
}

static const JSCFunctionListEntry ds2_mmheap_proto[] = {
    JS_CFUNC_DEF("push", 1, ds2_mmheap_push),
    JS_CFUNC_MAGIC_DEF("popMin", 0, ds2_mmheap_take, 0),
    JS_CFUNC_MAGIC_DEF("popMax", 0, ds2_mmheap_take, 1),
    JS_CFUNC_MAGIC_DEF("peekMin", 0, ds2_mmheap_take, 2),
    JS_CFUNC_MAGIC_DEF("peekMax", 0, ds2_mmheap_take, 3),
    JS_CGETSET_DEF("size", ds2_mmheap_size, NULL),
};

static void ds2_cms_free(void* native)
{
    ds2_cms_w* w = (ds2_cms_w*)native;
    if (!w)
        return;
    dyn_cms_free(w->c);
    dyn_nat_free(w);
}

static JSClassID ds2_cms_class_id;
DS2_FINALIZER(ds2_cms)
static const JSClassDef ds2_cms_class = {
    "CountMinSketch",
    .finalizer = ds2_cms_finalizer,
};

static JSValue ds2_cms_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    uint32_t width, depth = 5;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "width required");
    if (JS_ToUint32(ctx, &width, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &depth, argv[1]))
        return JS_EXCEPTION;
    if (width == 0 || depth == 0)
        return JS_ThrowRangeError(ctx, "width and depth must be > 0");
    if (depth > 64)
        return JS_ThrowRangeError(ctx, "depth must be <= 64");
    w = (ds2_cms_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_cms_new(width, depth);
    if (!w->c) {
        dyn_nat_free(w);
        return JS_ThrowRangeError(ctx, "width * depth is too large");
    }
    return dyn_plain_wrap(ctx, new_target, ds2_cms_class_id, w, ds2_cms_free);
}

static JSValue ds2_cms_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    ds2_str k;
    int64_t n = 1;

    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &n, argv[1])) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (n < 0) {
        ds2_str_release(ctx, &k);
        return JS_ThrowRangeError(ctx,
            "count must be >= 0: a sketch cannot be decremented");
    }
    dyn_cms_add(w->c, k.p, k.n, (uint64_t)n);
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_cms_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w* w;
    ds2_str k;
    uint64_t c;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    c = dyn_cms_count(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return ds2_u64_js(ctx, c);
}

static JSValue ds2_cms_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_cms_w *w, *o;
    (void)argc;
    o = (ds2_cms_w*)JS_GetOpaque2(ctx, argv[0], ds2_cms_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (w == o)
        return JS_ThrowTypeError(ctx, "cannot merge a sketch into itself");
    if (dyn_cms_merge(w->c, o->c) < 0)
        return JS_ThrowTypeError(ctx,
            "merge requires identical width and depth");
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_cms_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    ds2_cms_w* w = (ds2_cms_w*)dyn_plain_get(ctx, this_val, ds2_cms_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewInt64(ctx, (int64_t)dyn_cms_width(w->c));
    case 1:
        return JS_NewInt64(ctx, (int64_t)dyn_cms_depth(w->c));
    default:
        return ds2_u64_js(ctx, dyn_cms_total(w->c));
    }
}

static const JSCFunctionListEntry ds2_cms_proto[] = {
    JS_CFUNC_DEF("add", 1, ds2_cms_add),
    JS_CFUNC_DEF("count", 1, ds2_cms_count),
    JS_CFUNC_DEF("merge", 1, ds2_cms_merge),
    JS_CGETSET_MAGIC_DEF("width", ds2_cms_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("depth", ds2_cms_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("totalCount", ds2_cms_getter, NULL, 2),
};

static void ds2_hll_free(void* native)
{
    ds2_hll_w* w = (ds2_hll_w*)native;
    if (!w)
        return;
    dyn_hll_free(w->c);
    dyn_nat_free(w);
}

static JSClassID ds2_hll_class_id;
DS2_FINALIZER(ds2_hll)
static const JSClassDef ds2_hll_class = {
    "HyperLogLog",
    .finalizer = ds2_hll_finalizer,
};

static JSValue ds2_hll_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w;
    uint32_t p = 14;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToUint32(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    if (p < DYN_HLL_MIN_PRECISION || p > DYN_HLL_MAX_PRECISION)
        return JS_ThrowRangeError(ctx, "precision must be %d..%d",
            DYN_HLL_MIN_PRECISION,
            DYN_HLL_MAX_PRECISION);
    w = (ds2_hll_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_hll_new(p);
    if (!w->c) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_hll_class_id, w, ds2_hll_free);
}

static JSValue ds2_hll_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w;
    ds2_str k;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    dyn_hll_add(w->c, k.p, k.n);
    ds2_str_release(ctx, &k);
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_hll_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w* w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_hll_count(w->c));
}

static JSValue ds2_hll_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_hll_w *w, *o;
    (void)argc;
    o = (ds2_hll_w*)JS_GetOpaque2(ctx, argv[0], ds2_hll_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (w == o)
        return JS_DupValue(ctx, this_val);
    if (dyn_hll_merge(w->c, o->c) < 0)
        return JS_ThrowTypeError(ctx, "merge requires equal precision");
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_hll_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    ds2_hll_w* w = (ds2_hll_w*)dyn_plain_get(ctx, this_val, ds2_hll_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic ? (int64_t)dyn_hll_registers(w->c) : (int64_t)dyn_hll_precision(w->c));
}

static const JSCFunctionListEntry ds2_hll_proto[] = {
    JS_CFUNC_DEF("add", 1, ds2_hll_add),
    JS_CFUNC_DEF("count", 0, ds2_hll_count),
    JS_CFUNC_DEF("merge", 1, ds2_hll_merge),
    JS_CGETSET_MAGIC_DEF("precision", ds2_hll_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("registers", ds2_hll_getter, NULL, 1),
};

#include "dyna-serialize.h"

typedef const dyn_cell_t* (*ds2_cell_iter)(void* native, uint32_t i);

static int ds2_write_cells(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, ds2_cell_iter next)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    int r;

    if (JS_IsException(arr))
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* c = next(native, i);
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_DupValue(ctx, ds2_value(c)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
    }
    r = dyn_codec_write_values(ctx, w, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue ds2_read_cells(JSContext* ctx, dyn_de_t* r, uint32_t n)
{
    JSValue arr = dyn_codec_read_values(ctx, r);
    uint32_t got = 0;
    JSValue len;

    if (JS_IsException(arr))
        return arr;
    len = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &got, len)) {
        JS_FreeValue(ctx, len);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (got != n) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "malformed DYNS record: %u values for %u entries",
            (unsigned)got, (unsigned)n);
    }
    return arr;
}

#define DS2_WRAP(pfx, corefree, coreptr)                                          \
    do {                                                                          \
        pfx##_w* w_ = (pfx##_w*)dyn_nat_malloc(sizeof(*w_));                      \
        if (!w_) {                                                                \
            corefree;                                                             \
            return ds2_oom(ctx);                                                  \
        }                                                                         \
        w_->rt = JS_GetRuntime(ctx);                                              \
        w_->c = (coreptr);                                                        \
        return dyn_plain_wrap(ctx, JS_UNDEFINED, pfx##_class_id, w_, pfx##_free); \
    } while (0)

#define DS_MSET_EXT 0xFFFFFFFFu

typedef struct {
    const char* k;
    size_t kl;
    uint64_t c;
} ds2_mset_ent;

static int ds2_mset_cmp(const ds2_mset_ent* x, const ds2_mset_ent* y)
{
    size_t n = x->kl < y->kl ? x->kl : y->kl;
    int c = n ? memcmp(x->k, y->k, n) : 0;
    if (c)
        return c;
    return x->kl < y->kl ? -1 : (x->kl > y->kl ? 1 : 0);
}

static void ds2_mset_sort(ds2_mset_ent* v, ds2_mset_ent* tmp, size_t n)
{
    size_t mid, i, j, k;
    if (n < 2)
        return;
    mid = n / 2;
    ds2_mset_sort(v, tmp, mid);
    ds2_mset_sort(v + mid, tmp, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n)
        tmp[k++] = ds2_mset_cmp(&v[i], &v[j]) <= 0 ? v[i++] : v[j++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(*v));
}

static int ds2_mset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mset_w* o = (ds2_mset_w*)JS_GetOpaque(obj, ds2_mset_class_id);
    uint32_t i, n = dyn_mset_distinct(o->c);
    ds2_mset_ent *ents = NULL, *tmp = NULL;
    const char* prev = NULL;
    size_t prev_len = 0;
    int rc = -1;
    (void)ctx;

    if (dyn_ser_u32(w, DS_MSET_EXT) < 0 || dyn_ser_u32(w, n) < 0)
        return -1;
    if (!n)
        return 0;
    ents = (ds2_mset_ent*)dyn_nat_malloc(sizeof(*ents) * n);
    tmp = (ds2_mset_ent*)dyn_nat_malloc(sizeof(*tmp) * n);
    if (!ents || !tmp)
        goto done;
    for (i = 0; i < n; i++)
        dyn_mset_at(o->c, i, &ents[i].k, &ents[i].kl, &ents[i].c);
    ds2_mset_sort(ents, tmp, n);
    for (i = 0; i < n; i++) {
        size_t sh = 0, lim = ents[i].kl < prev_len ? ents[i].kl : prev_len;
        while (sh < lim && ents[i].k[sh] == prev[sh])
            sh++;
        if (dyn_sk_uvarint(w, sh) < 0 || dyn_sk_uvarint(w, ents[i].kl - sh) < 0 || dyn_ser_raw(w, ents[i].k + sh, ents[i].kl - sh) < 0 || dyn_sk_uvarint(w, ents[i].c) < 0)
            goto done;
        prev = ents[i].k;
        prev_len = ents[i].kl;
    }
    rc = 0;
done:
    dyn_nat_free(ents);
    dyn_nat_free(tmp);
    return rc;
}

static JSValue ds2_mset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mset_t* m;
    uint32_t n, i, first;
    uint8_t* cur = NULL;
    size_t curcap = 0, cur_len = 0;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first != DS_MSET_EXT) {
        n = first;
        if ((uint64_t)n * 12 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        m = dyn_mset_new();
        if (!m)
            return ds2_oom(ctx);
        for (i = 0; i < n; i++) {
            size_t kl;
            const char* k = dyn_de_blob(r, &kl);
            uint64_t c = dyn_de_u64(r);
            if (!dyn_de_ok(r) || dyn_mset_set_count(m, k, kl, c) < 0) {
                dyn_mset_free(m);
                return dyn_de_ok(r) ? ds2_oom(ctx)
                                    : dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
        }
        DS2_WRAP(ds2_mset, dyn_mset_free(m), m);
    }
    if (dyn_de_count(r, &n, 3) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    m = dyn_mset_new();
    if (!m)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        uint64_t sh, sl, c;
        const uint8_t* p;
        if (dyn_sk_read_uvarint(r, &sh) < 0 || dyn_sk_read_uvarint(r, &sl) < 0)
            goto bad;
        if (sh > cur_len || sl > (uint64_t)dyn_de_left(r))
            goto bad;
        if (dyn_trie_grow((void**)&cur, &curcap, (size_t)(sh + sl), 1) < 0)
            goto oom;
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto bad;
        memcpy(cur + sh, p, (size_t)sl);
        cur_len = (size_t)(sh + sl);
        if (dyn_sk_read_uvarint(r, &c) < 0)
            goto bad;
        if (dyn_mset_set_count(m, (const char*)cur, cur_len, c) < 0)
            goto oom;
    }
    dyn_nat_free(cur);
    DS2_WRAP(ds2_mset, dyn_mset_free(m), m);
bad:
    dyn_nat_free(cur);
    dyn_mset_free(m);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
oom:
    dyn_nat_free(cur);
    dyn_mset_free(m);
    return ds2_oom(ctx);
}

static const dyn_cell_t* ds2_mmap_flat(void* native, uint32_t i)
{
    dyn_mmap_t* m = (dyn_mmap_t*)native;
    uint32_t k, nv;
    for (k = 0; k < dyn_mmap_keys(m); k++) {
        const dyn_cell_t* cells = dyn_mmap_cells_at(m, k, &nv);
        if (i < nv)
            return &cells[i];
        i -= nv;
    }
    return NULL;
}

static int ds2_mmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mmap_w* o = (ds2_mmap_w*)JS_GetOpaque(obj, ds2_mmap_class_id);
    uint32_t i, nk = dyn_mmap_keys(o->c);
    if (dyn_ser_u32(w, nk) < 0)
        return -1;
    for (i = 0; i < nk; i++) {
        const char* k;
        size_t kl;
        uint32_t nv;
        dyn_mmap_key_at(o->c, i, &k, &kl, &nv);
        if (dyn_ser_blob(w, k, kl) < 0 || dyn_ser_u32(w, nv) < 0)
            return -1;
    }
    return ds2_write_cells(ctx, w, o->c, (uint32_t)dyn_mmap_size(o->c),
        ds2_mmap_flat);
}

static JSValue ds2_mmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mmap_t* m;
    uint32_t nk, i, j, at;
    uint64_t total = 0;
    JSValue vals;
    uint32_t* counts;
    const char** keys;
    size_t* klens;
    (void)opts;

    if (dyn_de_count(r, &nk, 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    keys = (const char**)dyn_nat_calloc(nk ? nk : 1, sizeof(*keys));
    klens = (size_t*)dyn_nat_calloc(nk ? nk : 1, sizeof(*klens));
    counts = (uint32_t*)dyn_nat_calloc(nk ? nk : 1, sizeof(*counts));
    if (!keys || !klens || !counts) {
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        dyn_nat_free(counts);
        return ds2_oom(ctx);
    }
    for (i = 0; i < nk; i++) {
        keys[i] = dyn_de_blob(r, &klens[i]);
        counts[i] = dyn_de_u32(r);
        if (!dyn_de_ok(r) || counts[i] > dyn_de_left(r)) {
            dyn_nat_free(keys);
            dyn_nat_free(klens);
            dyn_nat_free(counts);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        total += counts[i];
        if (total > DYN_MAX_CAPACITY) {
            dyn_nat_free(keys);
            dyn_nat_free(klens);
            dyn_nat_free(counts);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    vals = ds2_read_cells(ctx, r, (uint32_t)total);
    if (JS_IsException(vals)) {
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        dyn_nat_free(counts);
        return vals;
    }
    m = dyn_mmap_new();
    if (!m) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(keys);
        dyn_nat_free(klens);
        dyn_nat_free(counts);
        return ds2_oom(ctx);
    }
    at = 0;
    for (i = 0; i < nk; i++)
        for (j = 0; j < counts[i]; j++) {
            JSValue v = JS_GetPropertyUint32(ctx, vals, at++);
            dyn_cell_t cell = ds2_cell(v);
            if (dyn_mmap_put(m, keys[i], klens[i], &cell) < 0) {
                JS_FreeValue(ctx, v);
                dyn_mmap_free(m, ds2_free_cell, JS_GetRuntime(ctx));
                JS_FreeValue(ctx, vals);
                dyn_nat_free(keys);
                dyn_nat_free(klens);
                dyn_nat_free(counts);
                return ds2_oom(ctx);
            }
        }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(keys);
    dyn_nat_free(klens);
    dyn_nat_free(counts);
    DS2_WRAP(ds2_mmap, dyn_mmap_free(m, ds2_free_cell, JS_GetRuntime(ctx)), m);
}

static int ds2_bimap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_bimap_w* o = (ds2_bimap_w*)JS_GetOpaque(obj, ds2_bimap_class_id);
    uint32_t i, n = dyn_bimap_size(o->c);
    (void)ctx;
    if (dyn_ser_u32(w, n) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char *k, *v;
        size_t kl, vl;
        dyn_bimap_at(o->c, i, &k, &kl, &v, &vl);
        if (dyn_ser_blob(w, k, kl) < 0 || dyn_ser_blob(w, v, vl) < 0)
            return -1;
    }
    return 0;
}

static JSValue ds2_bimap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_bimap_t* b;
    uint32_t n, i;
    (void)opts;
    if (dyn_de_count(r, &n, 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    b = dyn_bimap_new();
    if (!b)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kl, vl;
        const char* k = dyn_de_blob(r, &kl);
        const char* v = dyn_de_blob(r, &vl);
        if (!dyn_de_ok(r)) {
            dyn_bimap_free(b);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (dyn_bimap_put(b, k, kl, v, vl, 1) < 0) {
            dyn_bimap_free(b);
            return ds2_oom(ctx);
        }
    }
    DS2_WRAP(ds2_bimap, dyn_bimap_free(b), b);
}

static const dyn_cell_t* ds2_table_cell(void* native, uint32_t i)
{
    const char *r, *c;
    size_t rn, cn;
    const dyn_cell_t* v = NULL;
    dyn_table_at((dyn_table_t*)native, i, &r, &rn, &c, &cn, &v);
    return v;
}

#define DS_TABLE_EXT 0xFFFFFFFFu

typedef struct {
    const char* p;
    size_t n;
    uint32_t id;
} ds2_tbl_key;

static int ds2_tbl_key_cmp(const ds2_tbl_key* x, const ds2_tbl_key* y)
{
    size_t m = x->n < y->n ? x->n : y->n;
    int c = m ? memcmp(x->p, y->p, m) : 0;
    if (c)
        return c;
    return x->n < y->n ? -1 : (x->n > y->n ? 1 : 0);
}

static void ds2_tbl_sort(ds2_tbl_key* v, ds2_tbl_key* tmp, size_t n)
{
    size_t mid, i, j, k;
    if (n < 2)
        return;
    mid = n / 2;
    ds2_tbl_sort(v, tmp, mid);
    ds2_tbl_sort(v + mid, tmp, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n)
        tmp[k++] = ds2_tbl_key_cmp(&v[i], &v[j]) <= 0 ? v[i++] : v[j++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(*v));
}

static int ds2_tbl_dict(dyn_ser_t* w, ds2_tbl_key* v, ds2_tbl_key* tmp,
    uint32_t n, uint32_t* idof)
{
    uint32_t k, distinct = 0;
    const char* prev = NULL;
    size_t prev_len = 0;

    ds2_tbl_sort(v, tmp, n);
    for (k = 0; k < n; k++)
        if (!k || ds2_tbl_key_cmp(&v[k], &v[k - 1]) != 0)
            distinct++;
    if (dyn_ser_uvarint(w, distinct) < 0)
        return -1;
    distinct = 0;
    for (k = 0; k < n; k++) {
        if (!k || ds2_tbl_key_cmp(&v[k], &v[k - 1]) != 0) {
            size_t sh = 0, lim = v[k].n < prev_len ? v[k].n : prev_len;
            while (sh < lim && v[k].p[sh] == prev[sh])
                sh++;
            if (dyn_ser_uvarint(w, sh) < 0 || dyn_ser_uvarint(w, v[k].n - sh) < 0 || dyn_ser_raw(w, v[k].p + sh, v[k].n - sh) < 0)
                return -1;
            prev = v[k].p;
            prev_len = v[k].n;
            distinct++;
        }
        idof[v[k].id] = distinct - 1;
    }
    return 0;
}

static int ds2_table_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_table_w* o = (ds2_table_w*)JS_GetOpaque(obj, ds2_table_class_id);
    uint32_t i, n = dyn_table_size(o->c);
    ds2_tbl_key *rows = NULL, *cols = NULL, *tmp = NULL;
    uint32_t *rid = NULL, *cid = NULL;
    int rc = -1;

    if (dyn_ser_u32(w, DS_TABLE_EXT) < 0 || dyn_ser_u32(w, n) < 0)
        return -1;
    if (!n)
        return ds2_write_cells(ctx, w, o->c, n, ds2_table_cell);

    rows = (ds2_tbl_key*)dyn_nat_malloc(sizeof(*rows) * n);
    cols = (ds2_tbl_key*)dyn_nat_malloc(sizeof(*cols) * n);
    tmp = (ds2_tbl_key*)dyn_nat_malloc(sizeof(*tmp) * n);
    rid = (uint32_t*)dyn_nat_malloc(sizeof(*rid) * n);
    cid = (uint32_t*)dyn_nat_malloc(sizeof(*cid) * n);
    if (!rows || !cols || !tmp || !rid || !cid)
        goto done;
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        dyn_table_at(o->c, i, &r, &rn, &c, &cn, &v);
        rows[i].p = r;
        rows[i].n = rn;
        rows[i].id = i;
        cols[i].p = c;
        cols[i].n = cn;
        cols[i].id = i;
    }
    if (ds2_tbl_dict(w, rows, tmp, n, rid) < 0 || ds2_tbl_dict(w, cols, tmp, n, cid) < 0)
        goto done;
    for (i = 0; i < n; i++)
        if (dyn_ser_uvarint(w, rid[i]) < 0 || dyn_ser_uvarint(w, cid[i]) < 0)
            goto done;
    rc = 0;
done:
    dyn_nat_free(rows);
    dyn_nat_free(cols);
    dyn_nat_free(tmp);
    dyn_nat_free(rid);
    dyn_nat_free(cid);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_table_cell);
}

static int ds2_tbl_read_dict(dyn_de_t* r, uint32_t* count, char** blob,
    size_t** off, size_t** len)
{
    uint64_t d;
    uint32_t k;
    size_t cap = 256, used = 0;
    char* b = NULL;
    size_t *o = NULL, *l = NULL;

    *blob = NULL;
    *off = NULL;
    *len = NULL;
    *count = 0;
    if (dyn_de_uvarint(r, &d) < 0 || d > (uint64_t)dyn_de_left(r) + 1)
        return -1;
    *count = (uint32_t)d;
    b = (char*)dyn_nat_malloc(cap);
    o = (size_t*)dyn_nat_malloc(sizeof(*o) * (d ? d : 1));
    l = (size_t*)dyn_nat_malloc(sizeof(*l) * (d ? d : 1));
    if (!b || !o || !l)
        goto fail;
    for (k = 0; k < *count; k++) {
        uint64_t sh, sl;
        const uint8_t* p;
        size_t klen;
        if (dyn_de_uvarint(r, &sh) < 0 || dyn_de_uvarint(r, &sl) < 0)
            goto fail;
        if (k == 0 ? sh != 0 : sh > l[k - 1])
            goto fail;
        if (sl > (uint64_t)dyn_de_left(r))
            goto fail;
        klen = (size_t)(sh + sl);
        while (used + klen > cap) {
            char* nb;
            cap *= 2;
            nb = (char*)dyn_nat_realloc(b, cap);
            if (!nb)
                goto fail;
            b = nb;
        }
        if (sh)
            memcpy(b + used, b + o[k - 1], (size_t)sh);
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto fail;
        memcpy(b + used + sh, p, (size_t)sl);
        o[k] = used;
        l[k] = klen;
        used += klen;
    }
    *blob = b;
    *off = o;
    *len = l;
    return 0;
fail:
    dyn_nat_free(b);
    dyn_nat_free(o);
    dyn_nat_free(l);
    *blob = NULL;
    *off = NULL;
    *len = NULL;
    return -1;
}

static JSValue ds2_table_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_table_t* t;
    uint32_t n, i, first;
    JSValue vals;
    const char **rk, **ck;
    size_t *rl, *cl;
    char *rblob = NULL, *cblob = NULL;
    size_t *roff = NULL, *rlen = NULL, *coff = NULL, *clen = NULL;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first != DS_TABLE_EXT) {
        n = first;
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = dyn_de_u32(r);
        if (!dyn_de_ok(r) || (uint64_t)n * 2 > (uint64_t)dyn_de_left(r) + 2)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    rk = (const char**)dyn_nat_calloc(n ? n : 1, sizeof(*rk));
    ck = (const char**)dyn_nat_calloc(n ? n : 1, sizeof(*ck));
    rl = (size_t*)dyn_nat_calloc(n ? n : 1, sizeof(*rl));
    cl = (size_t*)dyn_nat_calloc(n ? n : 1, sizeof(*cl));
    if (!rk || !ck || !rl || !cl) {
        dyn_nat_free(rk);
        dyn_nat_free(ck);
        dyn_nat_free(rl);
        dyn_nat_free(cl);
        return ds2_oom(ctx);
    }
    if (first != DS_TABLE_EXT) {
        for (i = 0; i < n; i++) {
            rk[i] = dyn_de_blob(r, &rl[i]);
            ck[i] = dyn_de_blob(r, &cl[i]);
        }
    } else if (n) {
        uint32_t rn, cn, k;
        if (ds2_tbl_read_dict(r, &rn, &rblob, &roff, &rlen) < 0 || ds2_tbl_read_dict(r, &cn, &cblob, &coff, &clen) < 0) {
            dyn_nat_free(rk);
            dyn_nat_free(ck);
            dyn_nat_free(rl);
            dyn_nat_free(cl);
            dyn_nat_free(rblob);
            dyn_nat_free(roff);
            dyn_nat_free(rlen);
            dyn_nat_free(cblob);
            dyn_nat_free(coff);
            dyn_nat_free(clen);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        for (k = 0; k < n; k++) {
            uint64_t a, b;
            if (dyn_de_uvarint(r, &a) < 0 || dyn_de_uvarint(r, &b) < 0 || a >= rn || b >= cn) {
                dyn_nat_free(rk);
                dyn_nat_free(ck);
                dyn_nat_free(rl);
                dyn_nat_free(cl);
                dyn_nat_free(rblob);
                dyn_nat_free(roff);
                dyn_nat_free(rlen);
                dyn_nat_free(cblob);
                dyn_nat_free(coff);
                dyn_nat_free(clen);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            rk[k] = rblob + roff[a];
            rl[k] = rlen[a];
            ck[k] = cblob + coff[b];
            cl[k] = clen[b];
        }
    }
    if (!dyn_de_ok(r)) {
        dyn_nat_free(rk);
        dyn_nat_free(ck);
        dyn_nat_free(rl);
        dyn_nat_free(cl);
        dyn_nat_free(rblob);
        dyn_nat_free(roff);
        dyn_nat_free(rlen);
        dyn_nat_free(cblob);
        dyn_nat_free(coff);
        dyn_nat_free(clen);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(rk);
        dyn_nat_free(ck);
        dyn_nat_free(rl);
        dyn_nat_free(cl);
        dyn_nat_free(rblob);
        dyn_nat_free(roff);
        dyn_nat_free(rlen);
        dyn_nat_free(cblob);
        dyn_nat_free(coff);
        dyn_nat_free(clen);
        return vals;
    }
    t = dyn_table_new();
    if (!t) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(rk);
        dyn_nat_free(ck);
        dyn_nat_free(rl);
        dyn_nat_free(cl);
        dyn_nat_free(rblob);
        dyn_nat_free(roff);
        dyn_nat_free(rlen);
        dyn_nat_free(cblob);
        dyn_nat_free(coff);
        dyn_nat_free(clen);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        dyn_cell_t cell = ds2_cell(v), old;
        int rep = dyn_table_put(t, rk[i], rl[i], ck[i], cl[i], &cell, &old);
        if (rep < 0) {
            JS_FreeValue(ctx, v);
            dyn_table_free(t, ds2_free_cell, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_nat_free(rk);
            dyn_nat_free(ck);
            dyn_nat_free(rl);
            dyn_nat_free(cl);
            dyn_nat_free(rblob);
            dyn_nat_free(roff);
            dyn_nat_free(rlen);
            dyn_nat_free(cblob);
            dyn_nat_free(coff);
            dyn_nat_free(clen);
            return ds2_oom(ctx);
        }
        if (rep)
            JS_FreeValue(ctx, ds2_value(&old));
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(rk);
    dyn_nat_free(ck);
    dyn_nat_free(rl);
    dyn_nat_free(cl);
    dyn_nat_free(rblob);
    dyn_nat_free(roff);
    dyn_nat_free(rlen);
    dyn_nat_free(cblob);
    dyn_nat_free(coff);
    dyn_nat_free(clen);
    DS2_WRAP(ds2_table, dyn_table_free(t, ds2_free_cell, JS_GetRuntime(ctx)), t);
}

static int ds2_rset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_rset_w* o = (ds2_rset_w*)JS_GetOpaque(obj, ds2_rset_class_id);
    uint32_t i, n = dyn_rset_count(o->c);
    double* v;
    int rc;
    (void)ctx;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    v = (double*)dyn_nat_malloc(sizeof(*v) * ((size_t)n * 2 + 1));
    if (!v)
        return -1;
    for (i = 0; i < n; i++)
        dyn_rset_at(o->c, i, &v[i * 2], &v[i * 2 + 1]);
    rc = dyn_num_write(w, v, (size_t)n * 2);
    dyn_nat_free(v);
    return rc;
}

static JSValue ds2_rset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_rset_t* s;
    uint32_t n, i, e, first;
    double* b;
    (void)opts;
    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    b = dyn_num_read_pairs(r, first, 2, &n, &e);
    if (!b)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    s = dyn_rset_new();
    if (!s) {
        dyn_nat_free(b);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        if (dyn_rset_add(s, b[i * 2], b[i * 2 + 1]) < 0) {
            dyn_nat_free(b);
            dyn_rset_free(s);
            return ds2_oom(ctx);
        }
    }
    dyn_nat_free(b);
    if (!dyn_de_ok(r)) {
        dyn_rset_free(s);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    DS2_WRAP(ds2_rset, dyn_rset_free(s), s);
}

static const dyn_cell_t* ds2_rmap_cell(void* native, uint32_t i)
{
    double lo, hi;
    const dyn_cell_t* v = NULL;
    dyn_rmap_at((dyn_rmap_t*)native, i, &lo, &hi, &v);
    return v;
}

static int ds2_rmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_rmap_w* o = (ds2_rmap_w*)JS_GetOpaque(obj, ds2_rmap_class_id);
    uint32_t i, n = dyn_rmap_count(o->c);
    double* b;
    int rc;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    b = (double*)dyn_nat_malloc(sizeof(*b) * ((size_t)n * 2 + 1));
    if (!b)
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* v;
        dyn_rmap_at(o->c, i, &b[i * 2], &b[i * 2 + 1], &v);
    }
    rc = dyn_num_write(w, b, (size_t)n * 2);
    dyn_nat_free(b);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_rmap_cell);
}

static JSValue ds2_rmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_rmap_t* m;
    uint32_t n, i;
    JSValue vals;
    double *lo, *hi;
    (void)opts;

    {
        uint32_t e, first = dyn_de_u32(r);
        double* b;
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        b = dyn_num_read_pairs(r, first, 2, &n, &e);
        if (!b)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        lo = (double*)dyn_nat_calloc(n ? n : 1, sizeof(double));
        hi = (double*)dyn_nat_calloc(n ? n : 1, sizeof(double));
        if (!lo || !hi) {
            dyn_nat_free(lo);
            dyn_nat_free(hi);
            dyn_nat_free(b);
            return ds2_oom(ctx);
        }
        for (i = 0; i < n; i++) {
            lo[i] = b[i * 2];
            hi[i] = b[i * 2 + 1];
        }
        dyn_nat_free(b);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(lo);
        dyn_nat_free(hi);
        return vals;
    }
    m = dyn_rmap_new();
    if (!m) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(lo);
        dyn_nat_free(hi);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        dyn_cell_t cell = ds2_cell(v);
        if (!(hi[i] > lo[i])) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (dyn_rmap_put(m, lo[i], hi[i], &cell, ds2_dup_cell, ds2_free_cell,
                JS_GetRuntime(ctx))
            < 0) {
            JS_FreeValue(ctx, v);
            dyn_rmap_free(m, ds2_free_cell, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_nat_free(lo);
            dyn_nat_free(hi);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(lo);
    dyn_nat_free(hi);
    DS2_WRAP(ds2_rmap, dyn_rmap_free(m, ds2_free_cell, JS_GetRuntime(ctx)), m);
}

static const dyn_cell_t* ds2_itree_cell(void* native, uint32_t i)
{
    double lo, hi;
    const dyn_cell_t* v = NULL;
    dyn_itree_at((dyn_itree_t*)native, i, &lo, &hi, &v);
    return v;
}

static int ds2_itree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_itree_w* o = (ds2_itree_w*)JS_GetOpaque(obj, ds2_itree_class_id);
    uint32_t i, n = dyn_itree_size(o->c);
    double* b;
    int rc;
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    b = (double*)dyn_nat_malloc(sizeof(*b) * ((size_t)n * 2 + 1));
    if (!b)
        return -1;
    for (i = 0; i < n; i++) {
        const dyn_cell_t* v;
        dyn_itree_at(o->c, i, &b[i * 2], &b[i * 2 + 1], &v);
    }
    rc = dyn_num_write(w, b, (size_t)n * 2);
    dyn_nat_free(b);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_itree_cell);
}

static JSValue ds2_itree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_itree_t* t;
    uint32_t n, i;
    JSValue vals;
    double *lo, *hi;
    (void)opts;

    {
        uint32_t e, first = dyn_de_u32(r);
        double* b;
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        b = dyn_num_read_pairs(r, first, 2, &n, &e);
        if (!b)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        lo = (double*)dyn_nat_calloc(n ? n : 1, sizeof(double));
        hi = (double*)dyn_nat_calloc(n ? n : 1, sizeof(double));
        if (!lo || !hi) {
            dyn_nat_free(lo);
            dyn_nat_free(hi);
            dyn_nat_free(b);
            return ds2_oom(ctx);
        }
        for (i = 0; i < n; i++) {
            lo[i] = b[i * 2];
            hi[i] = b[i * 2 + 1];
        }
        dyn_nat_free(b);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(lo);
        dyn_nat_free(hi);
        return vals;
    }
    t = dyn_itree_new();
    if (!t) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(lo);
        dyn_nat_free(hi);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        dyn_cell_t cell = ds2_cell(v);
        if (!(lo[i] >= lo[i]) || !(hi[i] >= hi[i]) || lo[i] > hi[i]) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (dyn_itree_insert(t, lo[i], hi[i], &cell) < 0) {
            JS_FreeValue(ctx, v);
            dyn_itree_free(t, ds2_free_cell, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_nat_free(lo);
            dyn_nat_free(hi);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(lo);
    dyn_nat_free(hi);
    DS2_WRAP(ds2_itree, dyn_itree_free(t, ds2_free_cell, JS_GetRuntime(ctx)), t);
}

static const dyn_cell_t* ds2_mmheap_cell(void* native, uint32_t i)
{
    return dyn_mmheap_cell_at((dyn_mmheap_t*)native, i);
}

static int ds2_mmheap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_mmheap_w* o = (ds2_mmheap_w*)JS_GetOpaque(obj, ds2_mmheap_class_id);
    uint32_t i, n = dyn_mmheap_size(o->c);
    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    {
        double* b = (double*)dyn_nat_malloc(sizeof(*b) * ((size_t)n + 1));
        int rc;
        if (!b)
            return -1;
        for (i = 0; i < n; i++)
            b[i] = dyn_mmheap_pri_at(o->c, i);
        rc = dyn_num_write(w, b, n);
        dyn_nat_free(b);
        if (rc < 0)
            return -1;
    }
    return ds2_write_cells(ctx, w, o->c, n, ds2_mmheap_cell);
}

static JSValue ds2_mmheap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_mmheap_t* h;
    uint32_t n, i;
    JSValue vals;
    double* pri;
    (void)opts;

    {
        uint32_t e, first = dyn_de_u32(r);
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        pri = dyn_num_read_pairs(r, first, 1, &n, &e);
        if (!pri)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(pri);
        return vals;
    }
    h = dyn_mmheap_new();
    if (!h) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(pri);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        dyn_cell_t cell = ds2_cell(v);
        if (dyn_mmheap_push(h, pri[i], &cell) < 0) {
            JS_FreeValue(ctx, v);
            dyn_mmheap_free(h, ds2_free_cell, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_nat_free(pri);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(pri);
    DS2_WRAP(ds2_mmheap, dyn_mmheap_free(h, ds2_free_cell, JS_GetRuntime(ctx)),
        h);
}

static int ds2_cms_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_cms_w* o = (ds2_cms_w*)JS_GetOpaque(obj, ds2_cms_class_id);
    uint32_t width = dyn_cms_width(o->c), depth = dyn_cms_depth(o->c), i;
    const uint64_t* c = dyn_cms_counters(o->c);
    (void)ctx;
    (void)i;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, width) < 0 || dyn_ser_u32(w, depth) < 0 || dyn_ser_u64(w, dyn_cms_total(o->c)) < 0)
        return -1;
    return dyn_sketch_write_u64(w, c, (size_t)width * depth);
}

static JSValue ds2_cms_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), width, depth, i, n;
    int ext = (first == DS_SKETCH_EXT);
    uint64_t total;
    dyn_cms_t* s;
    uint64_t* c;
    (void)opts;

    width = ext ? dyn_de_u32(r) : first;
    depth = dyn_de_u32(r);
    total = dyn_de_u64(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (width == 0 || depth == 0 || depth > 64 || (!ext && (uint64_t)width * depth * 8 > (uint64_t)dyn_de_left(r)))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (ext && (uint64_t)width * depth > DYN_DECODE_SKETCH_MAX_WORDS)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, (uint64_t)width * depth * 8) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    s = dyn_cms_new(width, depth);
    if (!s)
        return ds2_oom(ctx);
    c = dyn_cms_counters_mut(s);
    n = width * depth;
    if (ext) {
        if (dyn_sketch_read_u64(r, c, n) < 0) {
            dyn_cms_free(s);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        for (i = 0; i < n; i++)
            c[i] = dyn_de_u64(r);
    }
    dyn_cms_set_total(s, total);
    DS2_WRAP(ds2_cms, dyn_cms_free(s), s);
}

static int ds2_hll_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_hll_w* o = (ds2_hll_w*)JS_GetOpaque(obj, ds2_hll_class_id);
    (void)ctx;
    if (dyn_ser_u32(w, DS_SKETCH_EXT) < 0 || dyn_ser_u32(w, dyn_hll_precision(o->c)) < 0)
        return -1;
    return dyn_sketch_write_u8(w, dyn_hll_data(o->c),
        dyn_hll_registers(o->c));
}

static JSValue ds2_hll_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t first = dyn_de_u32(r), p;
    int ext;
    size_t n = 0;
    const char* data;
    dyn_hll_t* h;
    (void)opts;

    ext = (first == DS_SKETCH_EXT);
    p = ext ? dyn_de_u32(r) : first;
    if (!dyn_de_ok(r) || p < DYN_HLL_MIN_PRECISION || p > DYN_HLL_MAX_PRECISION)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (dyn_de_charge(r, (uint64_t)1 << p) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    h = dyn_hll_new(p);
    if (!h)
        return ds2_oom(ctx);
    if (ext) {
        if (dyn_sketch_read_u8(r, dyn_hll_data_mut(h), (size_t)1 << p) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (dyn_hll_regs_valid(h) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    } else {
        data = dyn_de_blob(r, &n);
        if (!dyn_de_ok(r) || n != (size_t)1 << p) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        memcpy(dyn_hll_data_mut(h), data, n);
        if (dyn_hll_regs_valid(h) < 0) {
            dyn_hll_free(h);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    DS2_WRAP(ds2_hll, dyn_hll_free(h), h);
}

#define DS2_REGISTER(pfx, name)                                         \
    if (dyn_register_plain_class(ctx, m, &pfx##_class_id, &pfx##_class, \
            pfx##_proto, countof(pfx##_proto),                          \
            pfx##_ctor, name)                                           \
        < 0)                                                            \
        return -1;

static int ds2_btree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_btree_w* o = (ds2_btree_w*)JS_GetOpaque(obj, ds2_btree_class_id);
    uint32_t n = dyn_btree_size(o->c), i = 0;
    double* k;
    dyn_btree_iter it;
    JSValue vals;
    int rc;

    if (dyn_ser_u32(w, DS_NUM_EXT) < 0)
        return -1;
    k = (double*)dyn_nat_malloc(sizeof(*k) * ((size_t)n + 1));
    if (!k)
        return -1;
    vals = JS_NewArray(ctx);
    if (JS_IsException(vals)) {
        dyn_nat_free(k);
        return -1;
    }
    if (n && dyn_btree_iter_begin(o->c, &it))
        do {
            const dyn_cell_t* v;
            if (!dyn_btree_iter_get(&it, &k[i], &v) || i >= n)
                break;
            if (JS_DefinePropertyValueUint32(ctx, vals, i,
                    JS_DupValue(ctx, ds2_value(v)),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, vals);
                dyn_nat_free(k);
                return -1;
            }
            i++;
        } while (dyn_btree_iter_next(&it));
    rc = dyn_num_write(w, k, i);
    dyn_nat_free(k);
    if (rc == 0)
        rc = dyn_codec_write_values(ctx, w, vals);
    JS_FreeValue(ctx, vals);
    return rc;
}

static JSValue ds2_btree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_btree_t* b;
    uint32_t n, e, i, first;
    double* k;
    JSValue vals;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    k = dyn_num_read_pairs(r, first, 1, &n, &e);
    if (!k)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(k);
        return vals;
    }
    b = dyn_btree_new();
    if (!b) {
        JS_FreeValue(ctx, vals);
        dyn_nat_free(k);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        dyn_cell_t cell = ds2_cell(v), old;
        int replaced = 0;
        if (k[i] != k[i] || dyn_btree_set(b, k[i], &cell, &old, &replaced) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, vals);
            dyn_btree_free(b, ds2_free_cell, JS_GetRuntime(ctx));
            dyn_nat_free(k);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (replaced)
            ds2_free_cell(JS_GetRuntime(ctx), &old);
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(k);
    DS2_WRAP(ds2_btree, dyn_btree_free(b, ds2_free_cell, JS_GetRuntime(ctx)), b);
}

static const dyn_codec_t ds2_codecs[] = {
    { 0, DYN_TID_MULTISET, "Multiset", ds2_mset_write, ds2_mset_read },
    { 0, DYN_TID_MULTIMAP, "Multimap", ds2_mmap_write, ds2_mmap_read },
    { 0, DYN_TID_BIMAP, "BiMap", ds2_bimap_write, ds2_bimap_read },
    { 0, DYN_TID_TABLE, "Table", ds2_table_write, ds2_table_read },
    { 0, DYN_TID_RANGESET, "RangeSet", ds2_rset_write, ds2_rset_read },
    { 0, DYN_TID_RANGEMAP, "RangeMap", ds2_rmap_write, ds2_rmap_read },
    { 0, DYN_TID_INTERVALTREE, "IntervalTree", ds2_itree_write, ds2_itree_read },
    { 0, DYN_TID_MINMAXHEAP, "MinMaxHeap", ds2_mmheap_write, ds2_mmheap_read },
    { 0, DYN_TID_COUNTMIN, "CountMinSketch", ds2_cms_write, ds2_cms_read },
    { 0, DYN_TID_HYPERLOGLOG, "HyperLogLog", ds2_hll_write, ds2_hll_read },
    { 0, DYN_TID_BTREE, "BTree", ds2_btree_write, ds2_btree_read },
};

static int dyn_guava_register_codecs(void)
{
    static const JSClassID* const ids[] = {
        &ds2_mset_class_id,
        &ds2_mmap_class_id,
        &ds2_bimap_class_id,
        &ds2_table_class_id,
        &ds2_rset_class_id,
        &ds2_rmap_class_id,
        &ds2_itree_class_id,
        &ds2_mmheap_class_id,
        &ds2_cms_class_id,
        &ds2_hll_class_id,
        &ds2_btree_class_id,
    };
    size_t i;
    for (i = 0; i < countof(ds2_codecs); i++) {
        dyn_codec_t c = ds2_codecs[i];
        c.class_id = *ids[i];
        if (dyn_codec_register(&c) < 0)
            return -1;
    }
    return 0;
}

static int dyn_guava_register(JSContext* ctx, JSModuleDef* m)
{
    DS2_REGISTER(ds2_mset, "Multiset")
    DS2_REGISTER(ds2_mmap, "Multimap")
    DS2_REGISTER(ds2_bimap, "BiMap")
    DS2_REGISTER(ds2_table, "Table")
    DS2_REGISTER(ds2_rset, "RangeSet")
    DS2_REGISTER(ds2_rmap, "RangeMap")
    DS2_REGISTER(ds2_itree, "IntervalTree")
    DS2_REGISTER(ds2_mmheap, "MinMaxHeap")
    DS2_REGISTER(ds2_cms, "CountMinSketch")
    DS2_REGISTER(ds2_hll, "HyperLogLog")
    DS2_REGISTER(ds2_btree, "BTree")
    return 0;
}

static void dyn_guava_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = {
        "Multiset",
        "Multimap",
        "BiMap",
        "Table",
        "RangeSet",
        "RangeMap",
        "IntervalTree",
        "MinMaxHeap",
        "CountMinSketch",
        "HyperLogLog",
        "BTree",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}

static int dyn_structures_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_guava_register(ctx, m) < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_bitset_class_id, &dyn_bitset_class,
            dyn_bitset_proto, countof(dyn_bitset_proto),
            dyn_bitset_ctor, "BitSet")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_uf_class_id, &dyn_uf_class,
            dyn_uf_proto, countof(dyn_uf_proto),
            dyn_uf_ctor, "UnionFind")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_deque_class_id, &dyn_deque_class,
            dyn_deque_proto, countof(dyn_deque_proto),
            dyn_deque_ctor, "Deque")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_fenwick_class_id, &dyn_fenwick_class,
            dyn_fenwick_proto, countof(dyn_fenwick_proto),
            dyn_fenwick_ctor, "Fenwick")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ringbuf_class_id, &dyn_ringbuf_class,
            dyn_ringbuf_proto, countof(dyn_ringbuf_proto),
            dyn_ringbuf_ctor, "RingBuffer")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_segtree_class_id, &dyn_segtree_class,
            dyn_segtree_proto, countof(dyn_segtree_proto),
            dyn_segtree_ctor, "SegTree")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_bloom_class_id, &dyn_bloom_class,
            dyn_bloom_proto, countof(dyn_bloom_proto),
            dyn_bloom_ctor, "BloomFilter")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_trie_class_id, &dyn_trie_class,
            dyn_trie_proto, countof(dyn_trie_proto),
            dyn_trie_ctor, "Trie")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_lru_class_id, &dyn_lru_class,
            dyn_lru_proto, countof(dyn_lru_proto),
            dyn_lru_ctor, "LRU")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_sortedset_class_id, &dyn_sortedset_class,
            dyn_sortedset_proto, countof(dyn_sortedset_proto),
            dyn_sortedset_ctor, "SortedSet")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_sortedmap_class_id, &dyn_sortedmap_class,
            dyn_sortedmap_proto, countof(dyn_sortedmap_proto),
            dyn_sortedmap_ctor, "SortedMap")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_heap_class_id, &dyn_heap_class,
            dyn_heap_proto, countof(dyn_heap_proto),
            dyn_heap_ctor, "Heap")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_list_class_id, &dyn_list_class,
            dyn_list_proto, countof(dyn_list_proto),
            dyn_list_ctor, "List")
        < 0)
        return -1;
    if (dyn_graph_register(ctx, m) < 0)
        return -1;
    if (dyn_ds_register_codecs() < 0 || dyn_guava_register_codecs() < 0 || dyn_serializer_register(ctx, m) < 0)
        return -1;
    if (dyn_codec_install_methods(ctx) < 0)
        return -1;
    return 0;
}

int js_nat_init_structures(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:structures",
        dyn_structures_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "BitSet");
    JS_AddModuleExport(ctx, m, "UnionFind");
    JS_AddModuleExport(ctx, m, "Deque");
    JS_AddModuleExport(ctx, m, "Fenwick");
    JS_AddModuleExport(ctx, m, "RingBuffer");
    JS_AddModuleExport(ctx, m, "SegTree");
    JS_AddModuleExport(ctx, m, "BloomFilter");
    JS_AddModuleExport(ctx, m, "Trie");
    JS_AddModuleExport(ctx, m, "LRU");
    JS_AddModuleExport(ctx, m, "SortedSet");
    JS_AddModuleExport(ctx, m, "SortedMap");
    JS_AddModuleExport(ctx, m, "Heap");
    JS_AddModuleExport(ctx, m, "List");
    dyn_guava_add_exports(ctx, m);
    dyn_graph_add_exports(ctx, m);
    dyn_serializer_add_exports(ctx, m);
    return 0;
}

int js_nat_init_structures_ext(JSContext* ctx)
{
    (void)ctx;
    return 0;
}

#endif
