#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DS2_REGISTER(pfx, name)                                         \
    if (dyn_register_plain_class(ctx, m, &pfx##_class_id, &pfx##_class, \
            pfx##_proto, countof(pfx##_proto),                          \
            pfx##_ctor, name)                                           \
        < 0)                                                            \
        return -1;

#define DYN_TID_ZSET 31
#define DYN_TID_ORDEREDMAP 32
#define DYN_TID_ROARING 33
#define DYN_TID_TDIGEST 34
#define DYN_TID_TOPK 35
#define DYN_TID_CUCKOO 36
#define DYN_TID_INDEXEDHEAP 37
#define DYN_TID_KDTREE 38
#define DYN_TID_CONSISTENTHASH 39
#define DYN_TID_INVERTEDINDEX 40
#define DYN_TID_INTERNER 41
#define DYN_TID_BKDTREE 51

#define SDB_REGISTER(pfx, name)                                         \
    if (dyn_register_plain_class(ctx, m, &pfx##_class_id, &pfx##_class, \
            pfx##_proto, countof(pfx##_proto),                          \
            pfx##_ctor, name)                                           \
        < 0)                                                            \
        return -1;

#define DYN_TID_RBTREE 43
#define DYN_TID_WBTREE 44
#define DYN_TID_SKIPLIST 45
#define DYN_TID_XARRAY 46
#define DYN_TID_PATRICIA 47
#define DYN_TID_CSRGRAPH 48
#define DYN_TID_RSTAR 49
#define DYN_TID_SUFFIXARRAY 50

static const dyn_codec_t dyn_ds_codecs[] = {
    { 0, DYN_TID_BITSET, "BitSet", dsw_bitset_write, dsw_bitset_read },
    { 0, DYN_TID_UNIONFIND, "UnionFind", dsw_uf_write, dsw_uf_read },
    { 0, DYN_TID_DEQUE, "Deque", dsw_deque_write, dsw_deque_read },
    { 0, DYN_TID_FENWICK, "Fenwick", dsw_fenwick_write, dsw_fenwick_read },
    { 0, DYN_TID_RINGBUFFER, "RingBuffer", dsw_ringbuf_write, dsw_ringbuf_read },
    { 0, DYN_TID_SEGTREE, "SegTree", dsw_segtree_write, dsw_segtree_read },
    { 0, DYN_TID_BLOOMFILTER, "BloomFilter", dsw_bloom_write, dsw_bloom_read },
    { 0, DYN_TID_TRIE, "Trie", dsw_trie_write, dsw_trie_read },
    { 0, DYN_TID_LRU, "LRU", dsw_lru_write, dsw_lru_read },
    { 0, DYN_TID_SORTEDSET, "SortedSet", dsw_sortedset_write, dsw_sortedset_read },
    { 0, DYN_TID_SORTEDMAP, "SortedMap", dsw_sortedmap_write, dsw_sortedmap_read },
    { 0, DYN_TID_HEAP, "Heap", dsw_heap_write, dsw_heap_read },
    { 0, DYN_TID_LIST, "List", dsw_list_write, dsw_list_read },
};

static int dyn_ds_register_codecs(void)
{
    static const JSClassID* const ids[] = {
        &dsw_bitset_class_id,
        &dsw_uf_class_id,
        &dsw_deque_class_id,
        &dsw_fenwick_class_id,
        &dsw_ringbuf_class_id,
        &dsw_segtree_class_id,
        &dsw_bloom_class_id,
        &dsw_trie_class_id,
        &dsw_lru_class_id,
        &dsw_sortedset_class_id,
        &dsw_sortedmap_class_id,
        &dsw_heap_class_id,
        &dsw_list_class_id,
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

static void sdb_capture_typed_cids(JSContext* ctx)
{
    int t;
    for (t = 0; t <= JS_TYPED_ARRAY_FLOAT64; t++) {
        JSValueConst args[1];
        JSValue ta;
        args[0] = JS_NewInt32(ctx, 0);
        ta = JS_NewTypedArray(ctx, 1, args, (JSTypedArrayEnum)t);
        if (JS_IsException(ta)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        atomic_store_explicit(&sdb_ta_cid[t], JS_GetClassID(ta), memory_order_relaxed);
        JS_FreeValue(ctx, ta);
    }
}

static const dyn_codec_t sdb_codecs[] = {
    { 0, DYN_TID_ZSET, "ZSet", sdb_zset_write, sdb_zset_read },
    { 0, DYN_TID_ORDEREDMAP, "OrderedMap", sdb_omap_write, sdb_omap_read },
    { 0, DYN_TID_ROARING, "RoaringBitmap", sdb_rb_write, sdb_rb_read },
    { 0, DYN_TID_TDIGEST, "TDigest", sdb_td_write, sdb_td_read },
    { 0, DYN_TID_TOPK, "TopK", sdb_tk_write, sdb_tk_read },
    { 0, DYN_TID_CUCKOO, "CuckooFilter", sdb_cf_write, sdb_cf_read },
    { 0, DYN_TID_INDEXEDHEAP, "IndexedHeap", sdb_ih_write, sdb_ih_read },
    { 0, DYN_TID_KDTREE, "KDTree", sdb_kd_write, sdb_kd_read },
    { 0, DYN_TID_CONSISTENTHASH, "ConsistentHash", sdb_ch_write, sdb_ch_read },
    { 0, DYN_TID_INVERTEDINDEX, "InvertedIndex", sdb_ii_write, sdb_ii_read },
    { 0, DYN_TID_INTERNER, "Interner", sdb_in_write, sdb_in_read },
    { 0, DYN_TID_BKDTREE, "BKDTree", sdb_bkd_write, sdb_bkd_read },
};

static JSClassID* const sdb_class_ids[] = {
    &sdb_zset_class_id,
    &sdb_omap_class_id,
    &sdb_rb_class_id,
    &sdb_td_class_id,
    &sdb_tk_class_id,
    &sdb_cf_class_id,
    &sdb_ih_class_id,
    &sdb_kd_class_id,
    &sdb_ch_class_id,
    &sdb_ii_class_id,
    &sdb_in_class_id,
    &sdb_bkd_class_id,
};

static int sdb_register_codecs(void)
{
    size_t i;
    for (i = 0; i < countof(sdb_codecs); i++) {
        dyn_codec_t c = sdb_codecs[i];
        c.class_id = *sdb_class_ids[i];
        if (dyn_codec_register(&c) < 0)
            return -1;
    }
    return 0;
}

static int sdb_static(JSContext* ctx, JSClassID id, const char* name, JSCFunction* fn, int len)
{
    JSValue proto = JS_GetClassProto(ctx, id), ctor;
    int rc = -1;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    if (!JS_IsException(ctor) && JS_IsFunction(ctx, ctor))
        rc = JS_DefinePropertyValueStr(ctx, ctor, name, JS_NewCFunction(ctx, fn, name, len), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, proto);
    return rc;
}

static int sdb_register(JSContext* ctx, JSModuleDef* m)
{
    sdb_capture_typed_cids(ctx);
    SDB_REGISTER(sdb_zset, "ZSet")
    SDB_REGISTER(sdb_omap, "OrderedMap")
    SDB_REGISTER(sdb_rb, "RoaringBitmap")
    SDB_REGISTER(sdb_td, "TDigest")
    SDB_REGISTER(sdb_tk, "TopK")
    SDB_REGISTER(sdb_cf, "CuckooFilter")
    SDB_REGISTER(sdb_ih, "IndexedHeap")
    SDB_REGISTER(sdb_kd, "KDTree")
    SDB_REGISTER(sdb_ch, "ConsistentHash")
    SDB_REGISTER(sdb_ii, "InvertedIndex")
    SDB_REGISTER(sdb_in, "Interner")
    SDB_REGISTER(sdb_bkd, "BKDTree")
    if (sdb_static(ctx, sdb_ch_class_id, "jump", sdb_ch_jump, 2) < 0 || sdb_static(ctx, sdb_ii_class_id, "tokenize", sdb_ii_tokenize, 1) < 0
        || sdb_static(ctx, sdb_bkd_class_id, "build", sdb_bkd_build_static, 2) < 0)
        return -1;
    return 0;
}

static void sdb_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = {
        "ZSet",
        "OrderedMap",
        "RoaringBitmap",
        "TDigest",
        "TopK",
        "CuckooFilter",
        "IndexedHeap",
        "KDTree",
        "ConsistentHash",
        "InvertedIndex",
        "Interner",
        "BKDTree",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}

static int dsx_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dsx_rbtree_class_id, &dsx_rbtree_class, dsx_rbtree_proto, countof(dsx_rbtree_proto), dsx_rbtree_ctor, "RBTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_wbtree_class_id, &dsx_wbtree_class, dsx_ranked_proto, countof(dsx_ranked_proto), dsx_wbtree_ctor, "WBTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_skiplist_class_id, &dsx_skiplist_class, dsx_ranked_proto, countof(dsx_ranked_proto), dsx_skiplist_ctor, "SkipList") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_xarray_class_id, &dsx_xarray_class, dsx_xarray_proto, countof(dsx_xarray_proto), dsx_xarray_ctor, "XArray") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_patricia_class_id, &dsx_patricia_class, dsx_patricia_proto, countof(dsx_patricia_proto), dsx_patricia_ctor, "PatriciaTrie") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_csr_class_id, &dsx_csr_class, dsx_csr_proto, countof(dsx_csr_proto), dsx_csr_ctor, "CSRGraph") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_rstar_class_id, &dsx_rstar_class, dsx_rstar_proto, countof(dsx_rstar_proto), dsx_rstar_ctor, "RStarTree") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsx_sa_class_id, &dsx_sa_class, dsx_sa_proto, countof(dsx_sa_proto), dsx_sa_ctor, "SuffixArray") < 0)
        return -1;
    return 0;
}

static void dsx_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = { "RBTree", "WBTree", "SkipList", "XArray", "PatriciaTrie", "CSRGraph", "RStarTree", "SuffixArray" };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}

static int dsx_register_codecs(void)
{
    const dyn_codec_t codecs[] = {
        { dsx_rbtree_class_id, DYN_TID_RBTREE, "RBTree", dsx_tree_write, dsx_rbtree_read },
        { dsx_wbtree_class_id, DYN_TID_WBTREE, "WBTree", dsx_tree_write, dsx_wbtree_read },
        { dsx_skiplist_class_id, DYN_TID_SKIPLIST, "SkipList", dsx_tree_write, dsx_skiplist_read },
        { dsx_xarray_class_id, DYN_TID_XARRAY, "XArray", dsx_xarray_write, dsx_xarray_read },
        { dsx_patricia_class_id, DYN_TID_PATRICIA, "PatriciaTrie", dsx_patricia_write, dsx_patricia_read },
        { dsx_csr_class_id, DYN_TID_CSRGRAPH, "CSRGraph", dsx_csr_write, dsx_csr_read },
        { dsx_rstar_class_id, DYN_TID_RSTAR, "RStarTree", dsx_rstar_write, dsx_rstar_read },
        { dsx_sa_class_id, DYN_TID_SUFFIXARRAY, "SuffixArray", dsx_sa_write, dsx_sa_read },
    };
    size_t i;
    for (i = 0; i < countof(codecs); i++)
        if (dyn_codec_register(&codecs[i]) < 0)
            return -1;
    return 0;
}

static int dyn_structures_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_guava_register(ctx, m) < 0)
        return -1;
    if (sdb_register(ctx, m) < 0)
        return -1;
    if (dsx_register(ctx, m) < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_bitset_class_id, &dsw_bitset_class,
            dsw_bitset_proto, countof(dsw_bitset_proto),
            dsw_bitset_ctor, "BitSet")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_uf_class_id, &dsw_uf_class,
            dsw_uf_proto, countof(dsw_uf_proto),
            dsw_uf_ctor, "UnionFind")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_deque_class_id, &dsw_deque_class,
            dsw_deque_proto, countof(dsw_deque_proto),
            dsw_deque_ctor, "Deque")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_fenwick_class_id, &dsw_fenwick_class,
            dsw_fenwick_proto, countof(dsw_fenwick_proto),
            dsw_fenwick_ctor, "Fenwick")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_ringbuf_class_id, &dsw_ringbuf_class,
            dsw_ringbuf_proto, countof(dsw_ringbuf_proto),
            dsw_ringbuf_ctor, "RingBuffer")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_segtree_class_id, &dsw_segtree_class,
            dsw_segtree_proto, countof(dsw_segtree_proto),
            dsw_segtree_ctor, "SegTree")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_bloom_class_id, &dsw_bloom_class,
            dsw_bloom_proto, countof(dsw_bloom_proto),
            dsw_bloom_ctor, "BloomFilter")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_trie_class_id, &dsw_trie_class,
            dsw_trie_proto, countof(dsw_trie_proto),
            dsw_trie_ctor, "Trie")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_lru_class_id, &dsw_lru_class,
            dsw_lru_proto, countof(dsw_lru_proto),
            dsw_lru_ctor, "LRU")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_sortedset_class_id, &dsw_sortedset_class,
            dsw_sortedset_proto, countof(dsw_sortedset_proto),
            dsw_sortedset_ctor, "SortedSet")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_sortedmap_class_id, &dsw_sortedmap_class,
            dsw_sortedmap_proto, countof(dsw_sortedmap_proto),
            dsw_sortedmap_ctor, "SortedMap")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_heap_class_id, &dsw_heap_class,
            dsw_heap_proto, countof(dsw_heap_proto),
            dsw_heap_ctor, "Heap")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dsw_list_class_id, &dsw_list_class,
            dsw_list_proto, countof(dsw_list_proto),
            dsw_list_ctor, "List")
        < 0)
        return -1;
    if (dyn_ds_register_codecs() < 0 || dyn_guava_register_codecs() < 0 || sdb_register_codecs() < 0 || dsx_register_codecs() < 0 || dyn_serializer_register(ctx, m) < 0
        || dyn_codec_persist_register(DYN_TID_BLOOMFILTER, &dsw_bloom_persist) < 0
        || dyn_codec_persist_register(DYN_TID_COUNTMIN, &ds2_cms_persist) < 0
        || dyn_codec_persist_register(DYN_TID_HYPERLOGLOG, &ds2_hll_persist) < 0)
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
    sdb_add_exports(ctx, m);
    dsx_add_exports(ctx, m);
    dyn_serializer_add_exports(ctx, m);
    return 0;
}

int js_nat_init_structures_ext(JSContext* ctx)
{
    (void)ctx;
    return 0;
}

#endif
