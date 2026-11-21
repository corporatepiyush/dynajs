#ifndef STRUCTURES_INTERNAL_H
#define STRUCTURES_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#include "core/ds/ds-alloc.h"
#include "core/ds/ds-bimap.h"
#include "core/ds/ds-bitset.h"
#include "core/ds/ds-btree.h"
#include "core/ds/ds-cell.h"
#include "core/ds/ds-common.h"
#include "core/ds/ds-roaring-bitmap.h"
#include "core/ds/ds-count-min-sketch.h"
#include "core/ds/ds-heap.h"
#include "core/ds/ds-hyperloglog.h"
#include "core/ds/ds-interval-tree.h"
#include "core/ds/ds-min-max-heap.h"
#include "core/ds/ds-multimap.h"
#include "core/ds/ds-multiset.h"
#include "core/ds/ds-range-map.h"
#include "core/ds/ds-range-set.h"
#include "core/ds/ds-table.h"
#include "core/ds/ds-zset-skiplist.h"
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
#define DYN_DECODE_SKETCH_MAX_WORDS (1u << 24)
#define DYN_DECODE_SKETCH_MAX_SPARSE (1u << 22)

#define DYN_FINALIZER(pfx)                                  \
    static void pfx##_finalizer(JSRuntime* rt, JSValue val) \
    {                                                       \
        (void)rt;                                           \
        pfx##_free(JS_GetOpaque(val, pfx##_class_id));      \
    }

_Static_assert(sizeof(JSValue) <= sizeof(dyn_cell_t), "a JSValue must fit a dyn_cell_t");

static inline JSValue ds_cell_val(const dyn_cell_t* c)
{
    JSValue v;
    memcpy(&v, c, sizeof(v));
    return v;
}

static inline dyn_cell_t ds_val_cell(JSValue v)
{
    dyn_cell_t c;
    memset(&c, 0, sizeof(c));
    memcpy(&c, &v, sizeof(v));
    return c;
}

static inline void ds_cell_release(void* ud, const dyn_cell_t* c)
{
    JS_FreeValueRT((JSRuntime*)ud, ds_cell_val(c));
}

JSValue dyn_ds_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method,
    int argc, JSValueConst* argv);

static inline JSValue dyn_ds_iterator_toarray(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_ds_iter_via(ctx, this_val, "toArray", 0, NULL);
}

typedef dyn_bitset_t dsw_bitset_t;

_Static_assert(sizeof(dsw_bitset_t) <= 16, "dsw_bitset_t grew past 16 bytes");

extern JSClassID dsw_bitset_class_id;

extern const JSClassDef dsw_bitset_class;

int dyn_size_arg(JSContext* ctx, JSValueConst v, uint32_t max,
    uint32_t* out);

JSValue dsw_bitset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_bitset_proto[11];

extern JSClassID dsw_uf_class_id;

extern const JSClassDef dsw_uf_class;

#define DYN_UF_MAX_N 67108864u

JSValue dsw_uf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_uf_proto[5];

extern JSClassID dsw_deque_class_id;

extern const JSClassDef dsw_deque_class;

JSValue dsw_deque_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_deque_proto[10];

extern JSClassID dsw_fenwick_class_id;

extern const JSClassDef dsw_fenwick_class;

JSValue dsw_fenwick_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_fenwick_proto[4];

extern JSClassID dsw_ringbuf_class_id;

extern const JSClassDef dsw_ringbuf_class;

JSValue dsw_ringbuf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_ringbuf_proto[7];

extern JSClassID dsw_segtree_class_id;

extern const JSClassDef dsw_segtree_class;

JSValue dsw_segtree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_segtree_proto[3];

extern JSClassID dsw_bloom_class_id;

extern const JSClassDef dsw_bloom_class;

JSValue dsw_bloom_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_bloom_proto[4];

extern JSClassID dsw_trie_class_id;

extern const JSClassDef dsw_trie_class;

JSValue dsw_trie_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_trie_proto[8];

static inline JSValue ds2_tuple2(JSContext* ctx, JSValue a, JSValue b)
{
    JSValue v[2];
    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    v[0] = a;
    v[1] = b;
    return JS_NewArrayFrom(ctx, 2, v);
}

JSValue ds2_tuple3(JSContext* ctx, JSValue a, JSValue b, JSValue c);

extern JSClassID dsw_lru_class_id;

extern const JSClassDef dsw_lru_class;

JSValue dsw_lru_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_lru_proto[12];

typedef struct {
    JSRuntime* rt;
    dyn_btree_t* b;
} dsw_skip_t;

dsw_skip_t* skip_new(JSRuntime* rt);

void skip_free(void* native);

int skip_insert(JSContext* ctx, dsw_skip_t* s, double key,
    JSValueConst val);

int skip_delete(dsw_skip_t* s, double key);

int skip_floor(dsw_skip_t* s, double q, double* out);

int skip_ceil(dsw_skip_t* s, double q, double* out);

extern JSClassID dsw_sortedset_class_id;

extern const JSClassDef dsw_sortedset_class;

JSValue dsw_sortedset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue skip_end_or_undef(JSContext* ctx, dsw_skip_t* s, int last);

JSValue skip_keys_array(JSContext* ctx, dsw_skip_t* s);

extern const JSCFunctionListEntry dsw_sortedset_proto[11];

extern JSClassID dsw_sortedmap_class_id;

extern const JSClassDef dsw_sortedmap_class;

JSValue dsw_sortedmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t i;
    int failed;
} skip_pairs_state;

int skip_pairs_leaf(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n);

extern const JSCFunctionListEntry dsw_sortedmap_proto[12];

typedef struct {
    JSRuntime* rt;
    JSValue cmp;
    dyn_heap_t heap;
    int busy;
    int natural;
} dsw_heap_t;

_Static_assert(sizeof(dsw_heap_t) == 6 * sizeof(void*),
    "dsw_heap_t regained padding: keep the two ints adjacent");

extern JSClassID dsw_heap_class_id;

extern const JSClassDef dsw_heap_class;

JSValue dsw_heap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

typedef struct {
    JSContext* ctx;
    JSValueConst this_val;
    dsw_heap_t* h;
} dsw_heap_cmpctx;

static force_inline int dsw_heap_cmp_js(void* ud, const dyn_cell_t* a, const dyn_cell_t* b)
{
    dsw_heap_cmpctx* c = (dsw_heap_cmpctx*)ud;
    JSContext* ctx = c->ctx;
    JSValueConst args[2];
    JSValue dcmp, da, db, r;
    double d;
    dcmp = JS_DupValue(ctx, c->h->cmp);
    da = JS_DupValue(ctx, ds_cell_val(a));
    db = JS_DupValue(ctx, ds_cell_val(b));
    args[0] = da;
    args[1] = db;
    r = JS_Call(ctx, dcmp, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, dcmp);
    JS_FreeValue(ctx, da);
    JS_FreeValue(ctx, db);
    if (!dyn_plain_get(ctx, c->this_val, dsw_heap_class_id)) {
        JS_FreeValue(ctx, r);
        return DYN_HEAP_ABORT;
    }
    if (JS_IsException(r))
        return DYN_HEAP_ABORT;
    if (JS_ToFloat64(ctx, &d, r)) {
        JS_FreeValue(ctx, r);
        return DYN_HEAP_ABORT;
    }
    JS_FreeValue(ctx, r);
    if (d < 0)
        return -1;
    if (d > 0)
        return 1;
    return 0;
}

extern const JSCFunctionListEntry dsw_heap_proto[5];

extern JSClassID dsw_list_class_id;

extern const JSClassDef dsw_list_class;

JSValue dsw_list_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsw_list_proto[9];

#include "dyna-serialize.h"

JSValue dyn_ds_construct(JSContext* ctx, JSClassID id, int argc,
    JSValueConst* argv);

int dyn_ds_invoke(JSContext* ctx, JSValueConst obj, const char* name,
    int argc, JSValueConst* argv);

int dyn_ds_invoke_atom(JSContext* ctx, JSValueConst obj, JSAtom name,
    int argc, JSValueConst* argv);

JSValue dyn_ds_value_at(JSContext* ctx, JSValueConst vals, uint32_t i);

typedef JSValue (*dyn_ds_val_at)(void* native, uint32_t i);

int dyn_ds_write_values(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, dyn_ds_val_at get);

JSValue dyn_ds_read_values(JSContext* ctx, dyn_de_t* r, uint32_t n);

int dsw_bitset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_bitset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

#define dyn_sk_uvarint(w, v) dyn_ser_uvarint((w), (v))
#define dyn_sk_read_uvarint(r, o) dyn_de_uvarint((r), (o))
#define DS_NUM_RAW 0
#define DS_NUM_MAX (1u << 24)
#define DS_NUM_EXT 0xFFFFFFFFu

int dyn_num_write(dyn_ser_t* w, const double* v, size_t n);

int dyn_num_read_header(dyn_de_t* r, uint32_t* n, uint8_t* kind);

double* dyn_num_read_pairs(dyn_de_t* r, uint32_t first, uint32_t per,
    uint32_t* count, uint32_t* elems);

int dyn_num_fill(dyn_de_t* r, uint8_t kind, double* v, uint32_t n);

int dsw_uf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_uf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_fenwick_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_fenwick_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_segtree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_segtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

#define DS_SKETCH_EXT 0xFFFFFFFFu
#define DS_SKETCH_RAW 0
#define DS_SKETCH_ALT 1

int dyn_sketch_write_u64(dyn_ser_t* w, const uint64_t* v, size_t n);

int dyn_sketch_read_u64(dyn_de_t* r, uint64_t* v, size_t n);
int dyn_sketch_fits_u64(const dyn_de_t* r, size_t n);

int dsw_bloom_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_bloom_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_trie_grow(void** p, size_t* cap, size_t need, size_t elem);

int dsw_trie_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_trie_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_lru_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_lru_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_skip_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj,
    JSClassID id, int with_values);

int dsw_sortedset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

int dsw_sortedmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_skip_read(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int with_values);

JSValue dsw_sortedset_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts);

JSValue dsw_sortedmap_read(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts);

int dsw_deque_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

int dsw_ringbuf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

int dsw_heap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dyn_ds_read_pushed(JSContext* ctx, dyn_de_t* r, JSClassID id,
    const char* method, int cap_arg,
    JSValueConst ctor_arg);

JSValue dsw_deque_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

JSValue dsw_ringbuf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsw_list_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsw_list_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

JSValue dsw_heap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

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

typedef struct {
    JSRuntime* rt;
    dyn_cms_t* c;
    dyn_ds_pstate ps;
    uint64_t* own;
} ds2_cms_w;

typedef struct {
    JSRuntime* rt;
    dyn_hll_t* c;
    dyn_ds_pstate ps;
    uint8_t* own;
} ds2_hll_w;

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

static inline int ds2_str_get(JSContext* ctx, ds2_str* s, JSValueConst v)
{
    s->p = JS_ToCStringLen(ctx, &s->n, v);
    return s->p ? 0 : -1;
}

static inline void ds2_str_release(JSContext* ctx, ds2_str* s)
{
    if (s->p)
        JS_FreeCString(ctx, s->p);
}

static inline JSValue ds2_new_str(JSContext* ctx, const char* p, size_t n)
{
    return JS_NewStringLen(ctx, p, n);
}

static inline JSValue ds2_oom(JSContext* ctx)
{
    return JS_ThrowOutOfMemory(ctx);
}

JSValue ds2_iter_via(JSContext* ctx, JSValueConst this_val,
    const char* method);

#define DS2_ITERATOR(pfx, method)                                        \
    static JSValue pfx##_iterator(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                    \
    {                                                                    \
        (void)argc;                                                      \
        (void)argv;                                                      \
        return ds2_iter_via(ctx, this_val, method);                      \
    }

extern JSClassID ds2_mset_class_id;

extern const JSClassDef ds2_mset_class;

JSValue ds2_mset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

static inline JSValue ds2_u64_js(JSContext* ctx, uint64_t v)
{
    return v > (uint64_t)INT64_MAX ? JS_NewFloat64(ctx, (double)v)
                                   : JS_NewInt64(ctx, (int64_t)v);
}

extern const JSCFunctionListEntry ds2_mset_proto[12];

extern JSClassID ds2_mmap_class_id;

extern const JSClassDef ds2_mmap_class;

JSValue ds2_mmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_mmap_proto[10];

extern JSClassID ds2_bimap_class_id;

extern const JSClassDef ds2_bimap_class;

JSValue ds2_bimap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_bimap_proto[13];

extern JSClassID ds2_table_class_id;

extern const JSClassDef ds2_table_class;

JSValue ds2_table_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_table_proto[9];

extern JSClassID ds2_rset_class_id;

extern const JSClassDef ds2_rset_class;

JSValue ds2_rset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

static inline int ds2_two_bounds(JSContext* ctx, double* lo, double* hi,
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

extern const JSCFunctionListEntry ds2_rset_proto[11];

extern JSClassID ds2_rmap_class_id;

extern const JSClassDef ds2_rmap_class;

JSValue ds2_rmap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_rmap_proto[6];

extern JSClassID ds2_btree_class_id;

extern const JSClassDef ds2_btree_class;

JSValue ds2_btree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_btree_proto[12];

extern JSClassID ds2_itree_class_id;

extern const JSClassDef ds2_itree_class;

JSValue ds2_itree_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_itree_proto[4];

extern JSClassID ds2_mmheap_class_id;

extern const JSClassDef ds2_mmheap_class;

JSValue ds2_mmheap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_mmheap_proto[6];

extern JSClassID ds2_cms_class_id;

extern const JSClassDef ds2_cms_class;

JSValue ds2_cms_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_cms_proto[6];

extern JSClassID ds2_hll_class_id;

extern const JSClassDef ds2_hll_class;

JSValue ds2_hll_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry ds2_hll_proto[5];

#include "dyna-serialize.h"

typedef const dyn_cell_t* (*ds2_cell_iter)(void* native, uint32_t i);

int ds2_write_cells(JSContext* ctx, dyn_ser_t* w, void* native,
    uint32_t n, ds2_cell_iter next);

JSValue ds2_read_cells(JSContext* ctx, dyn_de_t* r, uint32_t n);

#define DS2_WRAP(pfx, corefree, coreptr)                                          \
    do {                                                                          \
        pfx##_w* w_ = (pfx##_w*)dyn_ds_malloc(sizeof(*w_));                       \
        if (!w_) {                                                                \
            corefree;                                                             \
            return ds2_oom(ctx);                                                  \
        }                                                                         \
        memset(w_, 0, sizeof(*w_));                                               \
        w_->rt = JS_GetRuntime(ctx);                                              \
        w_->c = (coreptr);                                                        \
        return dyn_plain_wrap(ctx, JS_UNDEFINED, pfx##_class_id, w_, pfx##_free); \
    } while (0)

int ds2_mset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_mset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_mmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_mmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_bimap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_bimap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_table_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_table_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_rset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_rset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_rmap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_rmap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_itree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_itree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_mmheap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_mmheap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_cms_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

extern const dyn_codec_persist_t ds2_cms_persist;

extern const dyn_codec_persist_t ds2_hll_persist;

extern const dyn_codec_persist_t dsw_bloom_persist;

JSValue ds2_cms_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_hll_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_hll_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int ds2_btree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue ds2_btree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

#include "core/dyn-hash.h"

extern _Atomic(JSClassID) sdb_ta_cid[12];

int sdb_f64_list(JSContext* ctx, JSValueConst v, const char* what,
    double** out, uint32_t* pn);

int sdb_u32_list(JSContext* ctx, JSValueConst v, const char* what,
    uint32_t** out, uint32_t* pn);

static inline int sdb_u32_arg(JSContext* ctx, JSValueConst v, const char* what, uint32_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v))
        return -1;
    if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
        JS_ThrowRangeError(ctx, "%s must be an integer in 0..2^32-1", what);
        return -1;
    }
    *out = (uint32_t)x;
    return 0;
}

static inline int sdb_num_arg(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (*out != *out) {
        JS_ThrowTypeError(ctx, "%s must not be NaN", what);
        return -1;
    }
    return 0;
}

static inline int sdb_opt_get(JSContext* ctx, JSValueConst opts, const char* name, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    *out = JS_GetPropertyStr(ctx, opts, name);
    return JS_IsException(*out) ? -1 : 0;
}

static inline int sdb_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, int* out)
{
    JSValue v;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v))
        *out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return 0;
}

int sdb_opt_u32(JSContext* ctx, JSValueConst opts, const char* name, uint32_t* out);

JSValue sdb_typed_out(JSContext* ctx, const void* data, size_t n, JSTypedArrayEnum t);

JSValue sdb_pair(JSContext* ctx, JSValue a, JSValue b);

static inline int sdb_push(JSContext* ctx, JSValueConst arr, uint32_t* i, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueUint32(ctx, arr, (*i)++, v, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

int sdb_index_arg(JSContext* ctx, JSValueConst v, const char* what, int64_t* out);

int sdb_zsl_init(dyn_zsl_t* s, JSContext* ctx);

int sdb_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, JSValue val, dyn_zsl_node** out);

void sdb_zsl_mark(JSRuntime* rt, const dyn_zsl_t* s, JS_MarkFunc* mark_func);

JSValue dyn_zsl_keystr(JSContext* ctx, const dyn_zsl_node* x);

extern JSClassID sdb_zset_class_id;

extern const JSClassDef sdb_zset_class;

JSValue sdb_zset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_zset_proto[19];

extern JSClassID sdb_omap_class_id;

extern const JSClassDef sdb_omap_class;

JSValue sdb_omap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_omap_proto[20];

extern JSClassID sdb_rb_class_id;

extern const JSClassDef sdb_rb_class;

JSValue sdb_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_rb_proto[31];

extern JSClassID sdb_td_class_id;

extern const JSClassDef sdb_td_class;

JSValue sdb_td_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_td_proto[12];

extern JSClassID sdb_tk_class_id;

extern const JSClassDef sdb_tk_class;

JSValue sdb_tk_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_tk_proto[8];

extern JSClassID sdb_cf_class_id;

extern const JSClassDef sdb_cf_class;

JSValue sdb_cf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_cf_proto[6];

extern JSClassID sdb_ih_class_id;

extern const JSClassDef sdb_ih_class;

JSValue sdb_ih_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_ih_proto[13];

extern JSClassID sdb_kd_class_id;

extern const JSClassDef sdb_kd_class;

JSValue sdb_kd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_kd_proto[8];

extern JSClassID sdb_bkd_class_id;

extern const JSClassDef sdb_bkd_class;

JSValue sdb_bkd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue sdb_bkd_build_static(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_bkd_proto[9];

int sdb_bkd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_bkd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

JSValue sdb_rb_wrap_roar(JSContext* ctx, dyn_roar_t* r);

int sdb_typed_kind_pub(JSValueConst v);

const uint8_t* sdb_typed_bytes_pub(JSContext* ctx, JSValueConst v, size_t* plen);

extern JSClassID sdb_ch_class_id;

extern const JSClassDef sdb_ch_class;

JSValue sdb_ch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue sdb_ch_jump(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_ch_proto[9];

extern JSClassID sdb_ii_class_id;

extern const JSClassDef sdb_ii_class;

JSValue sdb_ii_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue sdb_ii_tokenize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_ii_proto[10];

extern JSClassID sdb_in_class_id;

extern const JSClassDef sdb_in_class;

JSValue sdb_in_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry sdb_in_proto[8];

static inline JSValue sdb_bad(JSContext* ctx)
{
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
}

int sdb_zset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_zset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_omap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_omap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_rb_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_rb_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_td_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_td_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_tk_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_tk_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_cf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_cf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_ih_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_ih_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_kd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_kd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_ch_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_ch_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_ii_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_ii_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int sdb_in_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue sdb_in_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

#include <arpa/inet.h>
#define DSX_MAX_NODES (1u << 26)

static inline int dsx_to_f64_free(JSContext* ctx, double* out, JSValue v)
{
    int r = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r;
}

JSValue dsx_typed_copy(JSContext* ctx, const void* data, size_t count, size_t esz, JSTypedArrayEnum type);

static inline void dsx_val_copy(void* dst, const void* src, void* ud)
{
    JSValue v;
    memcpy(&v, src, sizeof(v));
    v = JS_DupValueRT((JSRuntime*)ud, v);
    memcpy(dst, &v, sizeof(v));
}

static inline void dsx_val_release(void* e, void* ud)
{
    JSValue v;
    memcpy(&v, e, sizeof(v));
    JS_FreeValueRT((JSRuntime*)ud, v);
}

static inline JSValue dsx_val_get(const void* e)
{
    JSValue v;
    memcpy(&v, e, sizeof(v));
    return v;
}

static inline dyn_ds_ops dsx_val_ops(JSRuntime* rt)
{
    dyn_ds_ops o;
    memset(&o, 0, sizeof(o));
    o.size = sizeof(JSValue);
    o.copy = dsx_val_copy;
    o.release = dsx_val_release;
    o.ud = rt;
    return o;
}

static inline void dsx_mark_value(JSRuntime* rt, const void* e, JS_MarkFunc* mark_func)
{
    JS_MarkValue(rt, dsx_val_get(e), mark_func);
}

extern JSClassID dsx_rbtree_class_id;

extern JSClassID dsx_wbtree_class_id;

extern JSClassID dsx_skiplist_class_id;

typedef struct {
    JSRuntime* rt;
    JS_MarkFunc* mark_func;
} dsx_mark_ctx;

extern const JSClassDef dsx_rbtree_class;

extern const JSClassDef dsx_wbtree_class;

extern const JSClassDef dsx_skiplist_class;

JSValue dsx_rbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv);

JSValue dsx_wbtree_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv);

JSValue dsx_skiplist_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsx_rbtree_proto[13];

extern const JSCFunctionListEntry dsx_ranked_proto[15];

extern JSClassID dsx_xarray_class_id;

extern const JSClassDef dsx_xarray_class;

JSValue dsx_xarray_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsx_xarray_proto[11];

extern JSClassID dsx_patricia_class_id;

extern const JSClassDef dsx_patricia_class;

JSValue dsx_patricia_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

typedef struct {
    const uint8_t* p;
    size_t n;
    const char* cstr;
    JSValue buf;
} dsx_bytes;

int dsx_key_bytes(JSContext* ctx, JSValueConst v, int bytes_mode, dsx_bytes* out);

static inline void dsx_key_bytes_free(JSContext* ctx, dsx_bytes* b)
{
    if (b->cstr)
        JS_FreeCString(ctx, b->cstr);
    JS_FreeValue(ctx, b->buf);
}

extern const JSCFunctionListEntry dsx_patricia_proto[10];

extern JSClassID dsx_csr_class_id;

extern const JSClassDef dsx_csr_class;

JSValue dsx_csr_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsx_csr_proto[9];

extern JSClassID dsx_rstar_class_id;

extern const JSClassDef dsx_rstar_class;

JSValue dsx_rstar_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsx_rstar_proto[7];

extern JSClassID dsx_sa_class_id;

extern const JSClassDef dsx_sa_class;

JSValue dsx_sa_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dsx_sa_proto[6];

typedef struct {
    JSContext* ctx;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_flat;

static inline int dsx_flat_push(dsx_flat* f, JSValue v)
{
    if (f->failed || JS_IsException(v) || JS_DefinePropertyValueUint32(f->ctx, f->arr, f->n++, v, JS_PROP_C_W_E) < 0) {
        f->failed = 1;
        return -1;
    }
    return 0;
}

static inline int dsx_flat_open(JSContext* ctx, dsx_flat* f)
{
    f->ctx = ctx;
    f->n = 0;
    f->failed = 0;
    f->arr = JS_NewArray(ctx);
    return JS_IsException(f->arr) ? -1 : 0;
}

static inline int dsx_flat_close(dsx_flat* f, dyn_ser_t* w)
{
    int r = f->failed ? -1 : dyn_codec_write_values(f->ctx, w, f->arr);
    JS_FreeValue(f->ctx, f->arr);
    return r;
}

JSValue dsx_flat_read(JSContext* ctx, dyn_de_t* r, uint32_t group, uint32_t* n);

typedef JSValue (*dsx_method_fn)(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dsx_replay(JSContext* ctx, JSValue obj, JSValue arr, uint32_t n, uint32_t group, dsx_method_fn fn);
int dsx_opt_keys_ok(JSContext* ctx, JSValueConst obj, const char* allowed, const char* cls, const char* what);
int dsx_cache_opt(JSContext* ctx, JSValueConst opts, const char* cls, const char* feature, int* on);

int dsx_tree_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_rbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

JSValue dsx_wbtree_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

JSValue dsx_skiplist_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsx_xarray_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_xarray_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsx_patricia_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_patricia_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsx_csr_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_csr_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsx_rstar_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_rstar_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

int dsx_sa_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj);

JSValue dsx_sa_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts);

#endif

#endif
