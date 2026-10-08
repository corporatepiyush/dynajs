#ifndef DYN_DS_H
#define DYN_DS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dyn_ds_hash_seed(uint64_t seed0, uint64_t seed1);

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

#ifdef __cplusplus
}
#endif

#endif
