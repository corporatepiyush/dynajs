#ifndef DYN_DS_H
#define DYN_DS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dyn_ds_hash_seed(uint64_t seed0, uint64_t seed1);

uint64_t dyn_ds_xxh64(const void* data, size_t len, uint64_t seed);
void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*));
void* dyn_ds_malloc(size_t n);
void* dyn_ds_calloc(size_t n, size_t size);
void* dyn_ds_realloc(void* p, size_t n);
void dyn_ds_free(void* p);

typedef struct {
    uint64_t w[2];
} dyn_cell_t;

typedef void (*dyn_cell_free_fn)(void* ud, const dyn_cell_t* cell);

typedef void (*dyn_cell_dup_fn)(void* ud, const dyn_cell_t* in,
    dyn_cell_t* out);

typedef struct dyn_mset dyn_mset_t;

dyn_mset_t* dyn_mset_new(void);
void dyn_mset_free(dyn_mset_t* m);

int dyn_mset_add(dyn_mset_t* m, const char* k, size_t n, int64_t delta,
    uint64_t* out);
int dyn_mset_set_count(dyn_mset_t* m, const char* k, size_t n, uint64_t c);
uint64_t dyn_mset_count(const dyn_mset_t* m, const char* k, size_t n);
uint64_t dyn_mset_total(const dyn_mset_t* m);
uint32_t dyn_mset_distinct(const dyn_mset_t* m);
void dyn_mset_clear(dyn_mset_t* m);

int dyn_mset_at(const dyn_mset_t* m, uint32_t i, const char** k, size_t* n,
    uint64_t* count);

typedef struct dyn_mmap dyn_mmap_t;

dyn_mmap_t* dyn_mmap_new(void);
void dyn_mmap_free(dyn_mmap_t* m, dyn_cell_free_fn fn, void* ud);

int dyn_mmap_put(dyn_mmap_t* m, const char* k, size_t n, const dyn_cell_t* v);
uint32_t dyn_mmap_count(const dyn_mmap_t* m, const char* k, size_t n);
const dyn_cell_t* dyn_mmap_at(const dyn_mmap_t* m, const char* k, size_t n,
    uint32_t i);
int dyn_mmap_remove_at(dyn_mmap_t* m, const char* k, size_t n, uint32_t i,
    dyn_cell_t* out);
uint32_t dyn_mmap_remove_key(dyn_mmap_t* m, const char* k, size_t n,
    dyn_cell_free_fn fn, void* ud);
uint32_t dyn_mmap_keys(const dyn_mmap_t* m);
uint64_t dyn_mmap_size(const dyn_mmap_t* m);
int dyn_mmap_key_at(const dyn_mmap_t* m, uint32_t i, const char** k, size_t* n,
    uint32_t* count);
const dyn_cell_t* dyn_mmap_cells_at(const dyn_mmap_t* m, uint32_t i,
    uint32_t* count);
const dyn_cell_t* dyn_mmap_cells_for(const dyn_mmap_t* m, const char* k,
    size_t n, uint32_t* count);

typedef struct dyn_bimap dyn_bimap_t;

dyn_bimap_t* dyn_bimap_new(void);
void dyn_bimap_free(dyn_bimap_t* b);

#define DYN_BIMAP_VALUE_TAKEN 1

int dyn_bimap_put(dyn_bimap_t* b, const char* k, size_t kn,
    const char* v, size_t vn, int force);
const char* dyn_bimap_get(const dyn_bimap_t* b, const char* k, size_t kn,
    size_t* vn);
const char* dyn_bimap_key_for(const dyn_bimap_t* b, const char* v, size_t vn,
    size_t* kn);
int dyn_bimap_remove(dyn_bimap_t* b, const char* k, size_t kn);
int dyn_bimap_remove_value(dyn_bimap_t* b, const char* v, size_t vn);
uint32_t dyn_bimap_size(const dyn_bimap_t* b);
void dyn_bimap_clear(dyn_bimap_t* b);
int dyn_bimap_at(const dyn_bimap_t* b, uint32_t i, const char** k, size_t* kn,
    const char** v, size_t* vn);

typedef struct dyn_table dyn_table_t;

dyn_table_t* dyn_table_new(void);
void dyn_table_free(dyn_table_t* t, dyn_cell_free_fn fn, void* ud);

int dyn_table_put(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, const dyn_cell_t* v,
    dyn_cell_t* old);
const dyn_cell_t* dyn_table_get(const dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn);
int dyn_table_remove(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, dyn_cell_t* out);
uint32_t dyn_table_size(const dyn_table_t* t);
uint32_t dyn_table_slice_first(dyn_table_t* t, const char* k, size_t n,
    int by_column);
uint32_t dyn_table_slice_next(const dyn_table_t* t, uint32_t i, int by_column);

int dyn_table_at(const dyn_table_t* t, uint32_t i, const char** r, size_t* rn,
    const char** c, size_t* cn, const dyn_cell_t** v);

typedef struct dyn_rset dyn_rset_t;

dyn_rset_t* dyn_rset_new(void);
void dyn_rset_free(dyn_rset_t* s);

int dyn_rset_add(dyn_rset_t* s, double lo, double hi);
int dyn_rset_remove(dyn_rset_t* s, double lo, double hi);
int dyn_rset_contains(const dyn_rset_t* s, double x);
int dyn_rset_encloses(const dyn_rset_t* s, double lo, double hi);
int dyn_rset_intersects(const dyn_rset_t* s, double lo, double hi);
uint32_t dyn_rset_count(const dyn_rset_t* s);
int dyn_rset_at(const dyn_rset_t* s, uint32_t i, double* lo, double* hi);
void dyn_rset_clear(dyn_rset_t* s);
int dyn_rset_complement(const dyn_rset_t* s, double lo, double hi,
    dyn_rset_t* out);

typedef struct dyn_rmap dyn_rmap_t;

dyn_rmap_t* dyn_rmap_new(void);
void dyn_rmap_free(dyn_rmap_t* m, dyn_cell_free_fn fn, void* ud);

int dyn_rmap_put(dyn_rmap_t* m, double lo, double hi, const dyn_cell_t* v,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud);
const dyn_cell_t* dyn_rmap_get(const dyn_rmap_t* m, double x);
int dyn_rmap_remove(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud);
uint32_t dyn_rmap_count(const dyn_rmap_t* m);
int dyn_rmap_at(const dyn_rmap_t* m, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v);

typedef struct dyn_itree dyn_itree_t;

dyn_itree_t* dyn_itree_new(void);
void dyn_itree_free(dyn_itree_t* t, dyn_cell_free_fn fn, void* ud);

int dyn_itree_insert(dyn_itree_t* t, double lo, double hi,
    const dyn_cell_t* v);
uint32_t dyn_itree_size(const dyn_itree_t* t);
uint32_t dyn_itree_query(dyn_itree_t* t, double lo, double hi,
    uint32_t* out, uint32_t cap);
int dyn_itree_at(const dyn_itree_t* t, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v);
int dyn_itree_remove_at(dyn_itree_t* t, uint32_t i, dyn_cell_t* out);

typedef struct dyn_mmheap dyn_mmheap_t;

dyn_mmheap_t* dyn_mmheap_new(void);
void dyn_mmheap_free(dyn_mmheap_t* h, dyn_cell_free_fn fn, void* ud);

int dyn_mmheap_push(dyn_mmheap_t* h, double pri, const dyn_cell_t* v);
int dyn_mmheap_pop_min(dyn_mmheap_t* h, double* pri, dyn_cell_t* out);
int dyn_mmheap_pop_max(dyn_mmheap_t* h, double* pri, dyn_cell_t* out);
int dyn_mmheap_peek_min(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v);
int dyn_mmheap_peek_max(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v);
uint32_t dyn_mmheap_size(const dyn_mmheap_t* h);
const dyn_cell_t* dyn_mmheap_cell_at(const dyn_mmheap_t* h, uint32_t i);
double dyn_mmheap_pri_at(const dyn_mmheap_t* h, uint32_t i);

typedef struct dyn_cms dyn_cms_t;

dyn_cms_t* dyn_cms_new(uint32_t width, uint32_t depth);
void dyn_cms_free(dyn_cms_t* s);
void dyn_cms_add(dyn_cms_t* s, const char* k, size_t n, uint64_t count);
uint64_t dyn_cms_count(const dyn_cms_t* s, const char* k, size_t n);
uint64_t dyn_cms_total(const dyn_cms_t* s);
uint32_t dyn_cms_width(const dyn_cms_t* s);
uint32_t dyn_cms_depth(const dyn_cms_t* s);
int dyn_cms_merge(dyn_cms_t* a, const dyn_cms_t* b);
const uint64_t* dyn_cms_counters(const dyn_cms_t* s);
uint64_t* dyn_cms_counters_mut(dyn_cms_t* s);
void dyn_cms_set_total(dyn_cms_t* s, uint64_t total);

typedef struct dyn_hll dyn_hll_t;

#define DYN_HLL_MIN_PRECISION 4
#define DYN_HLL_MAX_PRECISION 18

dyn_hll_t* dyn_hll_new(uint32_t precision);
void dyn_hll_free(dyn_hll_t* h);
void dyn_hll_add(dyn_hll_t* h, const char* k, size_t n);
void dyn_hll_add_hash(dyn_hll_t* h, uint64_t hash);
double dyn_hll_count(dyn_hll_t* h);
uint32_t dyn_hll_precision(const dyn_hll_t* h);
uint32_t dyn_hll_registers(const dyn_hll_t* h);
int dyn_hll_merge(dyn_hll_t* a, const dyn_hll_t* b);
const uint8_t* dyn_hll_data(const dyn_hll_t* h);
uint8_t* dyn_hll_data_mut(dyn_hll_t* h);
int dyn_hll_regs_valid(const dyn_hll_t* h);

typedef struct dyn_btree dyn_btree_t;

#define DYN_BTREE_ORDER 32

dyn_btree_t* dyn_btree_new(void);
void dyn_btree_free(dyn_btree_t* t, dyn_cell_free_fn fn, void* ud);
uint32_t dyn_btree_size(const dyn_btree_t* t);
int dyn_btree_set(dyn_btree_t* t, double k, const dyn_cell_t* v,
    dyn_cell_t* old, int* replaced);
const dyn_cell_t* dyn_btree_get(const dyn_btree_t* t, double k);
int dyn_btree_del(dyn_btree_t* t, double k, dyn_cell_t* out);
int dyn_btree_ceil(const dyn_btree_t* t, double k, double* out);
int dyn_btree_floor(const dyn_btree_t* t, double k, double* out);
int dyn_btree_first(const dyn_btree_t* t, double* out);
int dyn_btree_last(const dyn_btree_t* t, double* out);
uint32_t dyn_btree_get_keys(const dyn_btree_t* t, double* out);
typedef int (*dyn_btree_leaf_fn)(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n);
uint32_t dyn_btree_walk_leaves(const dyn_btree_t* t, dyn_btree_leaf_fn fn,
    void* ud);
typedef struct {
    void* leaf;
    uint32_t i;
} dyn_btree_iter;
int dyn_btree_iter_begin(const dyn_btree_t* t, dyn_btree_iter* it);
int dyn_btree_iter_seek(const dyn_btree_t* t, double k, dyn_btree_iter* it);
int dyn_btree_iter_get(const dyn_btree_iter* it, double* k,
    const dyn_cell_t** v);
int dyn_btree_iter_next(dyn_btree_iter* it);

typedef struct dyn_ds_ops {
    size_t size;
    uint64_t (*hash)(const void* elem, void* ud);
    int (*eq)(const void* a, const void* b, void* ud);
    int (*cmp)(const void* a, const void* b, void* ud);
    void (*release)(void* elem, void* ud);
    void (*copy)(void* dst, const void* src, void* ud);
    void* ud;
} dyn_ds_ops;

typedef struct dyn_swiss dyn_swiss_t;
dyn_swiss_t* dyn_swiss_new(const dyn_ds_ops* key, const dyn_ds_ops* val);
void dyn_swiss_free(dyn_swiss_t* m);
size_t dyn_swiss_size(const dyn_swiss_t* m);
void* dyn_swiss_get(const dyn_swiss_t* m, const void* key);
int dyn_swiss_put(dyn_swiss_t* m, const void* key, const void* val);
void* dyn_swiss_upsert(dyn_swiss_t* m, const void* key, int* created);
int dyn_swiss_remove(dyn_swiss_t* m, const void* key);
void dyn_swiss_clear(dyn_swiss_t* m);
int dyn_swiss_reserve(dyn_swiss_t* m, size_t n);
size_t dyn_swiss_next(const dyn_swiss_t* m, size_t pos, const void** key, void** val);
const char* dyn_swiss_group_impl(void);

typedef struct dyn_rbtree dyn_rbtree_t;
typedef struct dyn_rbnode dyn_rbnode;
dyn_rbtree_t* dyn_rbtree_new(const dyn_ds_ops* key, const dyn_ds_ops* val);
void dyn_rbtree_free(dyn_rbtree_t* t);
size_t dyn_rbtree_size(const dyn_rbtree_t* t);
void* dyn_rbtree_get(const dyn_rbtree_t* t, const void* key);
int dyn_rbtree_put(dyn_rbtree_t* t, const void* key, const void* val);
int dyn_rbtree_remove(dyn_rbtree_t* t, const void* key);
dyn_rbnode* dyn_rbtree_first(const dyn_rbtree_t* t);
dyn_rbnode* dyn_rbtree_last(const dyn_rbtree_t* t);
dyn_rbnode* dyn_rbtree_next(const dyn_rbnode* n);
dyn_rbnode* dyn_rbtree_prev(const dyn_rbnode* n);
dyn_rbnode* dyn_rbtree_lower_bound(const dyn_rbtree_t* t, const void* key);
dyn_rbnode* dyn_rbtree_upper_bound(const dyn_rbtree_t* t, const void* key);
const void* dyn_rbtree_key(const dyn_rbtree_t* t, const dyn_rbnode* n);
void* dyn_rbtree_val(const dyn_rbtree_t* t, const dyn_rbnode* n);
int dyn_rbtree_check(const dyn_rbtree_t* t);

typedef struct dyn_wbtree dyn_wbtree_t;
dyn_wbtree_t* dyn_wbtree_new(const dyn_ds_ops* key, const dyn_ds_ops* val);
void dyn_wbtree_free(dyn_wbtree_t* t);
size_t dyn_wbtree_size(const dyn_wbtree_t* t);
void* dyn_wbtree_get(const dyn_wbtree_t* t, const void* key);
int dyn_wbtree_put(dyn_wbtree_t* t, const void* key, const void* val);
int dyn_wbtree_remove(dyn_wbtree_t* t, const void* key);
size_t dyn_wbtree_rank(const dyn_wbtree_t* t, const void* key);
int dyn_wbtree_select(const dyn_wbtree_t* t, size_t i, const void** key, void** val);
int dyn_wbtree_check(const dyn_wbtree_t* t);
typedef int (*dyn_wbtree_visit)(const void* key, void* val, void* ud);
int dyn_wbtree_walk(const dyn_wbtree_t* t, dyn_wbtree_visit fn, void* ud);

typedef struct dyn_skiplist dyn_skiplist_t;
typedef struct dyn_slnode dyn_slnode;
dyn_skiplist_t* dyn_skiplist_new(const dyn_ds_ops* key, const dyn_ds_ops* val, uint64_t seed);
void dyn_skiplist_free(dyn_skiplist_t* l);
size_t dyn_skiplist_size(const dyn_skiplist_t* l);
void* dyn_skiplist_get(const dyn_skiplist_t* l, const void* key);
int dyn_skiplist_put(dyn_skiplist_t* l, const void* key, const void* val);
int dyn_skiplist_remove(dyn_skiplist_t* l, const void* key);
size_t dyn_skiplist_rank(const dyn_skiplist_t* l, const void* key);
dyn_slnode* dyn_skiplist_at(const dyn_skiplist_t* l, size_t i);
dyn_slnode* dyn_skiplist_first(const dyn_skiplist_t* l);
dyn_slnode* dyn_skiplist_seek(const dyn_skiplist_t* l, const void* key);
dyn_slnode* dyn_skiplist_next(const dyn_slnode* n);
const void* dyn_skiplist_key(const dyn_skiplist_t* l, const dyn_slnode* n);
void* dyn_skiplist_val(const dyn_skiplist_t* l, const dyn_slnode* n);
int dyn_skiplist_check(const dyn_skiplist_t* l);

typedef struct dyn_xarray dyn_xarray_t;
dyn_xarray_t* dyn_xarray_new(const dyn_ds_ops* val);
void dyn_xarray_free(dyn_xarray_t* x);
size_t dyn_xarray_count(const dyn_xarray_t* x);
void* dyn_xarray_load(const dyn_xarray_t* x, uint64_t index);
int dyn_xarray_store(dyn_xarray_t* x, uint64_t index, const void* val);
int dyn_xarray_erase(dyn_xarray_t* x, uint64_t index);
int dyn_xarray_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, void** val);
int dyn_xarray_set_mark(dyn_xarray_t* x, uint64_t index, unsigned mark, int on);
int dyn_xarray_get_mark(const dyn_xarray_t* x, uint64_t index, unsigned mark);
int dyn_xarray_find_marked(const dyn_xarray_t* x, uint64_t* index, uint64_t max, unsigned mark, void** val);

typedef struct dyn_sa dyn_sa_t;
dyn_sa_t* dyn_sa_build(const uint8_t* text, size_t n);
void dyn_sa_free(dyn_sa_t* sa);
size_t dyn_sa_len(const dyn_sa_t* sa);
const uint32_t* dyn_sa_array(const dyn_sa_t* sa);
const uint8_t* dyn_sa_text(const dyn_sa_t* sa);
const uint32_t* dyn_sa_lcp(dyn_sa_t* sa);
size_t dyn_sa_find(const dyn_sa_t* sa, const uint8_t* pat, size_t m, size_t* first);
size_t dyn_sa_longest_repeat(dyn_sa_t* sa, size_t* pos);

typedef struct dyn_patricia dyn_patricia_t;
typedef int (*dyn_patricia_visit)(const uint8_t* key, uint32_t bits, void* val, void* ud);
dyn_patricia_t* dyn_patricia_new(const dyn_ds_ops* val);
void dyn_patricia_free(dyn_patricia_t* t);
size_t dyn_patricia_size(const dyn_patricia_t* t);
int dyn_patricia_put(dyn_patricia_t* t, const uint8_t* key, uint32_t bits, const void* val);
void* dyn_patricia_get(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits);
void* dyn_patricia_longest_prefix(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits, uint32_t* matched_bits);
int dyn_patricia_remove(dyn_patricia_t* t, const uint8_t* key, uint32_t bits);
int dyn_patricia_walk(const dyn_patricia_t* t, const uint8_t* prefix, uint32_t bits, dyn_patricia_visit fn, void* ud);

typedef struct dyn_csr dyn_csr_t;
typedef struct {
    uint32_t src, dst;
    double w;
} dyn_csr_edge;
dyn_csr_t* dyn_csr_build(uint32_t nodes, const dyn_csr_edge* edges, size_t nedges, int directed);
void dyn_csr_free(dyn_csr_t* g);
uint32_t dyn_csr_nodes(const dyn_csr_t* g);
size_t dyn_csr_edges(const dyn_csr_t* g);
uint32_t dyn_csr_degree(const dyn_csr_t* g, uint32_t v);
const uint32_t* dyn_csr_neighbors(const dyn_csr_t* g, uint32_t v, const double** w, uint32_t* count);
int dyn_csr_bfs(const dyn_csr_t* g, uint32_t src, int32_t* dist);
int dyn_csr_dijkstra(const dyn_csr_t* g, uint32_t src, double* dist, int32_t* pred);
int dyn_csr_pagerank(const dyn_csr_t* g, double damping, uint32_t iters, double tol, double* rank);
uint32_t dyn_csr_components(const dyn_csr_t* g, uint32_t* comp);

#define DYN_RSTAR_MAX_DIMS 8
typedef struct dyn_rstar dyn_rstar_t;
typedef int (*dyn_rstar_visit)(const double* lo, const double* hi, uint64_t id, void* ud);
dyn_rstar_t* dyn_rstar_new(unsigned dims);
void dyn_rstar_free(dyn_rstar_t* t);
size_t dyn_rstar_size(const dyn_rstar_t* t);
unsigned dyn_rstar_height(const dyn_rstar_t* t);
int dyn_rstar_insert(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id);
int dyn_rstar_remove(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id);
size_t dyn_rstar_search(const dyn_rstar_t* t, const double* lo, const double* hi, dyn_rstar_visit fn, void* ud);
size_t dyn_rstar_nearest(const dyn_rstar_t* t, const double* point, size_t k, uint64_t* ids, double* dist2);
int dyn_rstar_check(const dyn_rstar_t* t);

#ifndef DYN_DS_NO_THREADS
typedef int (*dyn_park_validate)(void* ud);
int dyn_park_wait(const void* addr, dyn_park_validate validate, void* ud, int64_t timeout_ns);
unsigned dyn_park_wake(const void* addr, unsigned count);
unsigned dyn_park_waiters(const void* addr);

typedef struct dyn_ring dyn_ring_t;
dyn_ring_t* dyn_ring_new(size_t elem_size, size_t capacity);
void dyn_ring_free(dyn_ring_t* r);
size_t dyn_ring_capacity(const dyn_ring_t* r);
int dyn_ring_try_push(dyn_ring_t* r, const void* elem);
int dyn_ring_try_pop(dyn_ring_t* r, void* out);
size_t dyn_ring_size_approx(const dyn_ring_t* r);

typedef struct dyn_wsdeque dyn_wsdeque_t;
dyn_wsdeque_t* dyn_wsdeque_new(size_t initial_capacity);
void dyn_wsdeque_free(dyn_wsdeque_t* q);
int dyn_wsdeque_push(dyn_wsdeque_t* q, uint64_t item);
int dyn_wsdeque_pop(dyn_wsdeque_t* q, uint64_t* item);
int dyn_wsdeque_steal(dyn_wsdeque_t* q, uint64_t* item);
size_t dyn_wsdeque_size_approx(const dyn_wsdeque_t* q);
#endif

#define DYN_DS_FIXED_SEED 0x5DB1E995C3A5C85CULL
#define DYN_DS_MAX_ITEMS (1u << 28)

typedef struct {
    uint64_t h;
    const char* k;
    uint32_t n;
    uint32_t used;
    uintptr_t v;
} dyn_ds_sent;

typedef struct {
    dyn_ds_sent* e;
    uint32_t cap, n;
} dyn_ds_smap;

typedef struct {
    uint32_t k;
    uint32_t used;
    uint32_t v;
} dyn_ds_ient;

typedef struct {
    dyn_ds_ient* e;
    uint32_t cap, n;
    uint64_t seed;
} dyn_ds_imap;

#define DYN_ROAR_ARR 1
#define DYN_ROAR_BMP 2
#define DYN_ROAR_FULL 3
#define DYN_ROAR_ARR_MAX 4096u
#define DYN_ROAR_WORDS 1024
#define DYN_ROAR_AND 0
#define DYN_ROAR_OR 1
#define DYN_ROAR_XOR 2
#define DYN_ROAR_ANDNOT 3
typedef struct {
    uint16_t key;
    uint8_t type;
    uint32_t card;
    uint32_t cap;
    void* data;
} dyn_roar_ct;

typedef struct {
    dyn_roar_ct* c;
    uint32_t n, cap;
} dyn_roar_t;

typedef struct {
    double m, w;
} dyn_tdigest_cent;

typedef struct {
    double delta;
    dyn_tdigest_cent* c;
    uint32_t n, nbuf, cap;
    double total, min, max, sum;
} dyn_tdigest_t;

#define DYN_TDIGEST_MIN_DELTA 10.0
#define DYN_TDIGEST_MAX_DELTA 10000.0
typedef struct {
    char* key;
    uint32_t klen;
    uint32_t hpos;
    double count;
    double err;
} dyn_topk_slot;

typedef struct {
    uint32_t k, n;
    dyn_topk_slot* s;
    uint32_t* heap;
    dyn_ds_smap idx;
    double total;
} dyn_topk_t;

#define DYN_TOPK_MAX_K (1u << 20)
#define DYN_CUCKOO_SLOTS 4
#define DYN_CUCKOO_MAX_KICKS 500
#define DYN_CUCKOO_MAX_CAPACITY (1u << 26)
typedef struct {
    uint16_t* t;
    uint32_t nb, mask;
    uint32_t count;
    uint16_t victim_fp;
    uint32_t victim_i;
    int has_victim;
    uint64_t rng;
} dyn_cuckoo_t;

typedef struct {
    double p;
    uint32_t id;
} dyn_iheap_ent;

typedef struct {
    dyn_iheap_ent* h;
    uint32_t n, cap;
    int max;
    dyn_ds_imap pos;
} dyn_iheap_t;

#define DYN_KDTREE_LEAF 12
#define DYN_KDTREE_MAX_DIMS 64
typedef struct {
    uint32_t lo, hi;
    int32_t l, r;
    double split;
    uint32_t dim;
} dyn_kdtree_node;

typedef struct {
    uint32_t d, n;
    double* pts;
    uint32_t* perm;
    dyn_kdtree_node* nodes;
    uint32_t nn, ncap;
    uint64_t rng;
} dyn_kdtree_t;

typedef struct {
    double d2;
    uint32_t i;
} dyn_kdtree_hit;

typedef struct {
    const dyn_kdtree_t* t;
    const double* q;
    dyn_kdtree_hit* h;
    uint32_t k, n;
} dyn_kdtree_knn_ctx;

typedef struct {
    dyn_kdtree_hit* h;
    uint32_t n, cap;
    int oom;
} dyn_kdtree_list;

#define DYN_CHASH_MAX_REPLICAS 1000u
#define DYN_CHASH_MAX_WEIGHT 1000u
#define DYN_CHASH_MAX_POINTS (1u << 22)
typedef struct {
    char* name;
    uint32_t len;
    uint32_t weight;
} dyn_chash_node;

typedef struct {
    uint64_t h;
    uint32_t node;
} dyn_chash_pt;

typedef struct {
    dyn_chash_node* nodes;
    uint32_t nn, ncap;
    dyn_chash_pt* ring;
    uint32_t nr;
    uint32_t replicas;
    dyn_ds_smap idx;
} dyn_chash_t;

#define DYN_INVIDX_TOK_MAX 128
#define DYN_INVIDX_MAX_GROUPS 32
typedef struct {
    uint32_t doc;
    uint32_t tf;
} dyn_invidx_post;

typedef struct {
    char* s;
    uint32_t len;
    uint32_t n, cap;
    dyn_invidx_post* p;
} dyn_invidx_term;

typedef struct {
    uint32_t term;
    uint32_t tf;
} dyn_invidx_dt;

typedef struct {
    uint32_t id;
    uint32_t len;
    uint32_t nt;
    uint32_t alive;
    dyn_invidx_dt* t;
} dyn_invidx_doc;

typedef struct {
    dyn_invidx_term* terms;
    uint32_t nterms, tcap;
    dyn_ds_smap tidx;
    dyn_invidx_doc* docs;
    uint32_t nslots, dcap;
    uint32_t* freel;
    uint32_t nfree, fcap;
    dyn_ds_imap didx;
    uint32_t live;
    uint64_t total_len;
    double k1, b;
    dyn_ds_smap stop;
    char** stopw;
    uint32_t nstop;
    double* acc;
    uint32_t* stamp;
    uint32_t* mask;
    uint32_t* touched;
    uint32_t scap, epoch;
    uint32_t* tstamp;
    uint32_t* tcount;
    uint32_t tscap, tepoch;
} dyn_invidx_t;

typedef int (*ii_tok_fn)(void* ud, const char* tok, size_t n);

typedef struct {
    dyn_invidx_t* x;
    dyn_invidx_dt* list;
    uint32_t n, cap;
    uint32_t len;
    int oom;
} dyn_invidx_add_ctx;

typedef struct {
    dyn_invidx_t* x;
    char toks[DYN_INVIDX_MAX_GROUPS][DYN_INVIDX_TOK_MAX];
    uint32_t lens[DYN_INVIDX_MAX_GROUPS];
    uint32_t n;
} dyn_invidx_q_ctx;

typedef struct {
    double s;
    uint32_t id;
} dyn_invidx_hit;

#define DYN_INTERNER_CHUNK (64u * 1024u)
typedef struct dyn_interner_chunk {
    struct dyn_interner_chunk* next;
    size_t used, cap;
    char data[];
} dyn_interner_chunk;

typedef struct {
    const char* p;
    uint32_t len;
} dyn_interner_str;

typedef struct {
    dyn_interner_chunk* chunks;
    dyn_interner_str* s;
    uint32_t n, cap;
    dyn_ds_smap idx;
    uint64_t bytes;
} dyn_interner_t;

uint64_t dyn_ds_strhash(const char* p, size_t n);
void dyn_ds_smap_free(dyn_ds_smap* m);
dyn_ds_sent* dyn_ds_smap_find(const dyn_ds_smap* m, const char* k, size_t n, uint64_t h);
dyn_ds_sent* dyn_ds_smap_insert(dyn_ds_smap* m, const char* k, size_t n, uint64_t h, uintptr_t v);
void dyn_ds_smap_remove(dyn_ds_smap* m, dyn_ds_sent* s);
void dyn_ds_imap_init(dyn_ds_imap* m);
void dyn_ds_imap_free(dyn_ds_imap* m);
dyn_ds_ient* dyn_ds_imap_find(const dyn_ds_imap* m, uint32_t k);
uint8_t* dyn_ds_bytes_dup(const char* p, size_t n);
void dyn_roar_clear(dyn_roar_t* r);
int dyn_roar_reserve(dyn_roar_t* r, uint32_t need);
uint32_t dyn_roar_popcnt(const uint64_t* w);
int dyn_roar_add(dyn_roar_t* r, uint32_t x);
int dyn_roar_remove(dyn_roar_t* r, uint32_t x);
int dyn_roar_has(const dyn_roar_t* r, uint32_t x);
int dyn_roar_range(dyn_roar_t* r, uint64_t lo, uint64_t hi, int add);
uint64_t dyn_roar_card(const dyn_roar_t* r);
int dyn_roar_op(dyn_roar_t* a, const dyn_roar_t* b, int op);
uint64_t dyn_roar_op_card(const dyn_roar_t* a, const dyn_roar_t* b, int op);
uint64_t dyn_roar_rank(const dyn_roar_t* r, uint32_t x);
int dyn_roar_select(const dyn_roar_t* r, uint64_t i, uint32_t* out);
void dyn_roar_fill(const dyn_roar_t* r, uint32_t* out);
int dyn_roar_clone(dyn_roar_t* dst, const dyn_roar_t* src);
int dyn_roar_u32_cmp(const void* a, const void* b);
int dyn_tdigest_init(dyn_tdigest_t* t, double delta);
void dyn_tdigest_free(dyn_tdigest_t* t);
void dyn_tdigest_compress(dyn_tdigest_t* t);
void dyn_tdigest_add(dyn_tdigest_t* t, double x, double w);
double dyn_tdigest_quantile(dyn_tdigest_t* t, double q);
double dyn_tdigest_cdf(dyn_tdigest_t* t, double x);
void dyn_topk_up(dyn_topk_t* t, uint32_t i);
void dyn_topk_free(dyn_topk_t* t);
int dyn_topk_add(dyn_topk_t* t, const char* k, size_t n, double c, char** evicted, size_t* evn);
void dyn_cuckoo_hash(const dyn_cuckoo_t* c, const char* k, size_t n, uint32_t* i, uint16_t* fp);
int dyn_cuckoo_insert(dyn_cuckoo_t* c, uint32_t i, uint16_t fp);
int dyn_cuckoo_contains(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp);
int dyn_cuckoo_delete(dyn_cuckoo_t* c, uint32_t i, uint16_t fp);
int dyn_iheap_push(dyn_iheap_t* q, uint32_t id, double p);
void dyn_iheap_remove_at(dyn_iheap_t* q, uint32_t i);
void dyn_kdtree_free(dyn_kdtree_t* t);
int dyn_kdtree_init(dyn_kdtree_t* t, double* pts, uint32_t n, uint32_t d);
void dyn_kdtree_knn(dyn_kdtree_knn_ctx* c, int32_t ni);
int dyn_kdtree_hit_cmp(const void* a, const void* b);
void dyn_kdtree_radius(const dyn_kdtree_t* t, int32_t ni, const double* q, double r2, dyn_kdtree_list* out);
void dyn_kdtree_box(const dyn_kdtree_t* t, int32_t ni, const double* lo, const double* hi, dyn_kdtree_list* out);
uint32_t dyn_chash_locate(const dyn_chash_t* c, uint64_t h);
void dyn_chash_free(dyn_chash_t* c);
int dyn_chash_set_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight);
int dyn_chash_remove_node(dyn_chash_t* c, const char* name, size_t n);
int dyn_invidx_tokenize(const char* str, size_t n, ii_tok_fn fn, void* ud);
int64_t dyn_invidx_term_get(dyn_invidx_t* x, const char* t, size_t n, int create);
int dyn_invidx_tscratch(dyn_invidx_t* x);
void dyn_invidx_bump_tepoch(dyn_invidx_t* x);
int dyn_invidx_remove(dyn_invidx_t* x, uint32_t id);
int dyn_invidx_install(dyn_invidx_t* x, uint32_t id, dyn_invidx_dt* list, uint32_t nt, uint32_t len);
int dyn_invidx_add(dyn_invidx_t* x, uint32_t id, const char* text, size_t n);
void dyn_invidx_free(dyn_invidx_t* x);
int dyn_invidx_add_stop(dyn_invidx_t* x, const char* t, size_t n);
int dyn_invidx_stop_tok(void* ud, const char* t, size_t n);
int dyn_invidx_search(dyn_invidx_t* x, const char* q, size_t qn, int and_mode, int prefix, uint32_t limit, dyn_invidx_hit** out, uint32_t* nout);
int64_t dyn_interner_intern(dyn_interner_t* t, const char* p, size_t n, int create);
void dyn_interner_free(dyn_interner_t* t);

#define DYN_ZSL_MAX 32
typedef struct dyn_zsl_node dyn_zsl_node;

typedef struct {
    dyn_zsl_node* fwd;
    uint32_t span;
} dyn_zsl_level;

struct dyn_zsl_node {
    double score;
    dyn_cell_t val;
    dyn_zsl_node* back;
    char* key;
    uint32_t klen;
    uint32_t lvl;
    dyn_zsl_level lv[];
};

typedef struct {
    dyn_cell_free_fn release;
    void* ud;
    dyn_zsl_node* head;
    dyn_zsl_node* tail;
    uint32_t len;
    int level;
    uint64_t rng;
    dyn_ds_smap idx;
} dyn_zsl_t;

int dyn_zsl_cmp(const dyn_zsl_node* x, double s, const char* k, uint32_t n);
int dyn_zsl_init(dyn_zsl_t* s, dyn_cell_free_fn release, void* ud);
void dyn_zsl_link(dyn_zsl_t* s, dyn_zsl_node* x);
void dyn_zsl_unlink(dyn_zsl_t* s, dyn_zsl_node* x);
uint32_t dyn_zsl_rank(const dyn_zsl_t* s, const dyn_zsl_node* x);
dyn_zsl_node* dyn_zsl_at(const dyn_zsl_t* s, uint32_t rank);
dyn_zsl_node* dyn_zsl_lt(const dyn_zsl_t* s, double sc, const char* k, uint32_t n, int le, uint32_t* prank);
dyn_zsl_node* dyn_zsl_first_ge_score(const dyn_zsl_t* s, double sc, uint32_t* prank);
dyn_zsl_node* dyn_zsl_last_le_score(const dyn_zsl_t* s, double sc, uint32_t* prank);
dyn_zsl_node* dyn_zsl_find(const dyn_zsl_t* s, const char* k, size_t n);
void dyn_zsl_drop(dyn_zsl_t* s, dyn_zsl_node* x);
int dyn_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, const dyn_cell_t* val, dyn_zsl_node** out);
void dyn_zsl_clear(dyn_zsl_t* s);
void dyn_zsl_destroy(dyn_zsl_t* s);
void dyn_topk_sorted(const dyn_topk_t* t, uint32_t* ord);
int dyn_topk_init(dyn_topk_t* t, uint32_t k);
int dyn_cuckoo_init(dyn_cuckoo_t* c, uint32_t nb);
void dyn_cuckoo_free(dyn_cuckoo_t* c);
void dyn_iheap_init(dyn_iheap_t* q, int max);
void dyn_iheap_free(dyn_iheap_t* q);
void dyn_invidx_init(dyn_invidx_t* x, double k1, double b);
void dyn_chash_init(dyn_chash_t* c, uint32_t replicas);

typedef struct {
    uint64_t* words;
    uint32_t nwords;
} dyn_bitset_t;

enum { DYN_BITSET_AND,
    DYN_BITSET_OR,
    DYN_BITSET_XOR };

void dyn_bitset_init(dyn_bitset_t* b);
void dyn_bitset_free(dyn_bitset_t* b);
int dyn_bitset_reserve(dyn_bitset_t* b, uint32_t word_index);
uint64_t dyn_bitset_count(const dyn_bitset_t* b);
int64_t dyn_bitset_next_set(const dyn_bitset_t* b, uint32_t from);
int dyn_bitset_combine(dyn_bitset_t* a, const dyn_bitset_t* o, int op);

typedef struct {
    uint32_t* parent;
    uint8_t* rank;
    uint32_t n;
    uint32_t sets;
} dyn_uf_t;

int dyn_uf_init(dyn_uf_t* u, uint32_t n);
void dyn_uf_free(dyn_uf_t* u);
int dyn_uf_union(dyn_uf_t* u, uint32_t x, uint32_t y);
int dyn_uf_validate(const dyn_uf_t* u, uint32_t* nroots);

typedef struct {
    double* tree;
    uint32_t n;
} dyn_fenwick_t;

int dyn_fenwick_init(dyn_fenwick_t* f, uint32_t n);
void dyn_fenwick_free(dyn_fenwick_t* f);
double dyn_fenwick_range(const dyn_fenwick_t* f, uint32_t lo, uint32_t hi);

enum { DYN_SEG_SUM,
    DYN_SEG_MIN,
    DYN_SEG_MAX };

typedef struct {
    double* tree;
    uint32_t n;
    int op;
} dyn_segtree_t;

double dyn_segtree_identity(int op);
int dyn_segtree_init(dyn_segtree_t* s, uint32_t n, int op);
void dyn_segtree_free(dyn_segtree_t* s);
void dyn_segtree_set(dyn_segtree_t* s, uint32_t i, double v);
double dyn_segtree_query(const dyn_segtree_t* s, uint32_t lo, uint32_t hi);

typedef struct {
    dyn_cell_t* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
    dyn_cell_free_fn release;
    void* ud;
} dyn_circbuf_t;

int dyn_circbuf_init(dyn_circbuf_t* c, uint32_t cap, dyn_cell_free_fn release, void* ud);
void dyn_circbuf_free(dyn_circbuf_t* c);

typedef struct {
    dyn_cell_t* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
    dyn_cell_free_fn release;
    void* ud;
} dyn_deque_t;

void dyn_deque_init(dyn_deque_t* d, dyn_cell_free_fn release, void* ud);
void dyn_deque_free(dyn_deque_t* d);

typedef struct {
    uint64_t* words;
    uint32_t nbits;
    uint32_t nwords;
    uint32_t k;
} dyn_bloom_t;

int dyn_bloom_init(dyn_bloom_t* b, uint32_t nbits, uint32_t k);
void dyn_bloom_free(dyn_bloom_t* b);
void dyn_bloom_add(dyn_bloom_t* b, const void* key, size_t n);
int dyn_bloom_may_contain(const dyn_bloom_t* b, const void* key, size_t n);

typedef struct dyn_trie_node {
    struct dyn_trie_node* child;
    struct dyn_trie_node* sibling;
    uint8_t* tail;
    uint32_t tail_len;
    uint8_t byte;
    uint8_t is_end;
} dyn_trie_node_t;

typedef struct dyn_trie_chunk {
    struct dyn_trie_chunk* next;
    uint32_t used, cap;
} dyn_trie_chunk_t;

typedef struct dyn_trie_bytes {
    struct dyn_trie_bytes* next;
    uint32_t used, cap;
} dyn_trie_bytes_t;

#define DYN_TRIE_BYTES(b) ((uint8_t*)((b) + 1))
#define DYN_TRIE_BYTES_MIN 512

#define DYN_TRIE_CHUNK_NODES(c) ((dyn_trie_node_t*)((c) + 1))
#define DYN_TRIE_CHUNK_MAX 8192

typedef struct {
    dyn_trie_node_t* root;
    dyn_trie_chunk_t* chunks;
    dyn_trie_bytes_t* bytes;
    uint32_t count;
} dyn_trie_t;

int dyn_trie_init(dyn_trie_t* t);
void dyn_trie_free(dyn_trie_t* t);
dyn_trie_node_t* dyn_trie_child(dyn_trie_node_t* node, uint8_t b);
dyn_trie_node_t* dyn_trie_walk(dyn_trie_t* t, const char* key, size_t klen, int* on_tail);
int dyn_trie_insert_at(dyn_trie_t* t, dyn_trie_node_t* node, const char* key, size_t klen, size_t from, dyn_trie_node_t** path, size_t* plen);
int dyn_trie_insert(dyn_trie_t* t, const char* key, size_t klen);
int dyn_trie_has(const dyn_trie_t* t, const char* key, size_t klen);
int dyn_trie_delete(dyn_trie_t* t, const char* key, size_t klen);
size_t dyn_trie_longest_prefix(const dyn_trie_t* t, const char* s, size_t slen);
void dyn_trie_order_children(dyn_trie_node_t** v, size_t k);

#define DYN_HEAP_ABORT (-2147483647 - 1)

typedef int (*dyn_heap_cmp_fn)(void* ud, const dyn_cell_t* a, const dyn_cell_t* b);

typedef struct {
    dyn_cell_t* items;
    uint32_t count;
    uint32_t cap;
} dyn_heap_t;

void dyn_heap_init(dyn_heap_t* h);
void dyn_heap_free(dyn_heap_t* h, dyn_cell_free_fn release, void* ud);
int dyn_heap_reserve(dyn_heap_t* h, uint32_t need);

typedef struct dyn_list_node {
    struct dyn_list_node *prev, *next;
    dyn_cell_t value;
} dyn_list_node_t;

typedef struct {
    dyn_list_node_t *head, *tail;
    uint32_t length;
    dyn_cell_free_fn release;
    void* ud;
} dyn_list_t;

void dyn_list_init(dyn_list_t* l, dyn_cell_free_fn release, void* ud);
void dyn_list_free(dyn_list_t* l);
int dyn_list_push_front(dyn_list_t* l, const dyn_cell_t* v);
int dyn_list_push_back(dyn_list_t* l, const dyn_cell_t* v);
int dyn_list_pop_front(dyn_list_t* l, dyn_cell_t* out);
int dyn_list_pop_back(dyn_list_t* l, dyn_cell_t* out);

#define DYN_LRU_MAX_BUCKETS (1u << 24)

typedef struct dyn_lru_node {
    struct dyn_lru_node *prev, *next;
    struct dyn_lru_node* hnext;
    dyn_cell_t value;
    uint64_t hash;
    uint64_t expires;
    size_t klen;
    char key[];
} dyn_lru_node_t;

typedef struct {
    dyn_lru_node_t** buckets;
    dyn_lru_node_t *head, *tail;
    uint64_t default_ttl;
    uint64_t hits, misses, evictions, expired;
    uint32_t n_buckets;
    uint32_t count;
    uint32_t cap;
    dyn_cell_free_fn release;
    void* ud;
} dyn_lru_t;

int dyn_lru_init(dyn_lru_t* L, uint32_t cap, dyn_cell_free_fn release, void* ud);
void dyn_lru_free(dyn_lru_t* L);
uint64_t dyn_lru_now_ms(void);
uint64_t dyn_lru_expiry(uint64_t ttl);
int dyn_lru_expired(const dyn_lru_node_t* n, uint64_t now);
dyn_lru_node_t* dyn_lru_find(const dyn_lru_t* L, const char* key, size_t klen);
void dyn_lru_touch(dyn_lru_t* L, dyn_lru_node_t* n);
void dyn_lru_detach(dyn_lru_t* L, dyn_lru_node_t* n);
int dyn_lru_set(dyn_lru_t* L, const char* key, size_t klen, const dyn_cell_t* v, uint64_t ttl, dyn_lru_node_t** evicted);
void dyn_lru_node_free(dyn_lru_t* L, dyn_lru_node_t* n);
dyn_cell_t dyn_lru_node_take(dyn_lru_node_t* n);

int dyn_deque_grow(dyn_deque_t* d);
void dyn_deque_shrink(dyn_deque_t* d);

static inline int dyn_bitset_set(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) >= b->nwords && dyn_bitset_reserve(b, i >> 6) < 0)
        return -1;
    b->words[i >> 6] |= (uint64_t)1 << (i & 63);
    return 0;
}

static inline int dyn_bitset_flip(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) >= b->nwords && dyn_bitset_reserve(b, i >> 6) < 0)
        return -1;
    b->words[i >> 6] ^= (uint64_t)1 << (i & 63);
    return 0;
}

static inline void dyn_bitset_clear(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) < b->nwords)
        b->words[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

static inline int dyn_bitset_get(const dyn_bitset_t* b, uint32_t i)
{
    return (i >> 6) < b->nwords && (b->words[i >> 6] & ((uint64_t)1 << (i & 63))) != 0;
}

static inline uint32_t dyn_uf_find(dyn_uf_t* u, uint32_t x)
{
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]];
        x = u->parent[x];
    }
    return x;
}

static inline void dyn_fenwick_add(dyn_fenwick_t* f, uint32_t i, double delta)
{
    uint32_t idx;
    for (idx = i + 1; idx <= f->n; idx += idx & (~idx + 1))
        f->tree[idx] += delta;
}

static inline double dyn_fenwick_prefix(const dyn_fenwick_t* f, uint32_t i)
{
    uint32_t idx;
    double s = 0.0;
    for (idx = i + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    return s;
}

static inline dyn_cell_t* dyn_circbuf_at(const dyn_circbuf_t* c, uint32_t i)
{
    uint32_t idx;
    if (i >= c->count)
        return NULL;
    idx = c->head + i;
    return &c->buf[idx >= c->cap ? idx - c->cap : idx];
}

static inline int dyn_circbuf_push(dyn_circbuf_t* c, const dyn_cell_t* v, dyn_cell_t* evicted)
{
    if (c->count < c->cap) {
        uint32_t idx = c->head + c->count;
        c->buf[idx >= c->cap ? idx - c->cap : idx] = *v;
        c->count++;
        return 0;
    }
    if (evicted)
        *evicted = c->buf[c->head];
    else if (c->release)
        c->release(c->ud, &c->buf[c->head]);
    c->buf[c->head] = *v;
    if (++c->head == c->cap)
        c->head = 0;
    return 1;
}

static inline dyn_cell_t* dyn_deque_at(const dyn_deque_t* d, uint32_t i)
{
    if (i >= d->count)
        return NULL;
    return &d->buf[(d->head + i) & (d->cap - 1)];
}

static inline int dyn_deque_push_back(dyn_deque_t* d, const dyn_cell_t* v)
{
    if ((d->count < d->cap ? 0 : dyn_deque_grow(d)) < 0)
        return -1;
    d->buf[(d->head + d->count) & (d->cap - 1)] = *v;
    d->count++;
    return 0;
}

static inline int dyn_deque_push_front(dyn_deque_t* d, const dyn_cell_t* v)
{
    if ((d->count < d->cap ? 0 : dyn_deque_grow(d)) < 0)
        return -1;
    d->head = (d->head - 1) & (d->cap - 1);
    d->buf[d->head] = *v;
    d->count++;
    return 0;
}

static inline int dyn_deque_pop_front(dyn_deque_t* d, dyn_cell_t* out)
{
    if (d->count == 0)
        return -1;
    *out = d->buf[d->head];
    d->head = (d->head + 1) & (d->cap - 1);
    d->count--;
    if (d->cap >= 64 && d->count <= d->cap / 4)
        dyn_deque_shrink(d);
    return 0;
}

static inline int dyn_deque_pop_back(dyn_deque_t* d, dyn_cell_t* out)
{
    if (d->count == 0)
        return -1;
    d->count--;
    *out = d->buf[(d->head + d->count) & (d->cap - 1)];
    if (d->cap >= 64 && d->count <= d->cap / 4)
        dyn_deque_shrink(d);
    return 0;
}

static inline int dyn_heap_sift_up(dyn_heap_t* h, uint32_t idx, dyn_heap_cmp_fn cmp, void* ud)
{
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        int c = cmp(ud, &h->items[idx], &h->items[parent]);
        dyn_cell_t tmp;
        if (c == DYN_HEAP_ABORT)
            return -1;
        if (c >= 0)
            break;
        tmp = h->items[idx];
        h->items[idx] = h->items[parent];
        h->items[parent] = tmp;
        idx = parent;
    }
    return 0;
}

static inline int dyn_heap_sift_down(dyn_heap_t* h, uint32_t idx, dyn_heap_cmp_fn cmp, void* ud)
{
    uint32_t path[64];
    int depth = 0, m, c;
    dyn_cell_t x = h->items[idx];
    path[0] = idx;
    while (2 * path[depth] + 1 < h->count && depth < 63) {
        uint32_t l = 2 * path[depth] + 1, r = l + 1, pick = l;
        if (r < h->count) {
            c = cmp(ud, &h->items[r], &h->items[l]);
            if (c == DYN_HEAP_ABORT)
                return -1;
            if (c < 0)
                pick = r;
        }
        path[++depth] = pick;
    }
    m = depth;
    while (m > 0) {
        c = cmp(ud, &x, &h->items[path[m]]);
        if (c == DYN_HEAP_ABORT)
            return -1;
        if (c >= 0)
            break;
        m--;
    }
    for (c = 0; c < m; c++)
        h->items[path[c]] = h->items[path[c + 1]];
    h->items[path[m]] = x;
    return 0;
}

dyn_csr_t* dyn_csr_alloc(uint32_t nodes, size_t arcs, uint32_t** row, uint32_t** col, double** w);

#ifdef __cplusplus
}
#endif

#endif
