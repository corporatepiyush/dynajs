/*
 * dyn-ds -- data structures the language lacks, as a pure C library.
 *
 * No JSValue, no JSContext, no dynajs.h: this compiles standalone with
 * -I src/core and is checked by tools/core-purity.sh. The JS bindings in
 * src/dyna-structures*.c are coerce -> call -> wrap and nothing else.
 *
 * ---- The element-ownership contract, and why there is no hot-path vtable ----
 *
 * Containers that hold engine values store them as an opaque `dyn_cell_t`. The
 * core never interprets a cell; it copies cells and hands them back.
 *
 * STDLIB_OOP_PLAN's W1.6 sketch proposed a `dyn_elem_ops_t {dup,free,mark,cmp}`
 * vtable consulted per element. That is unnecessary and it is the expensive
 * part: measurement (tests/bench_structures.js) puts Deque.pushBack at 24.3 ns
 * against a 20.2 ns native-call floor, so the per-element work is ~4 ns and an
 * indirect call would be a double-digit percentage of it.
 *
 * Instead OWNERSHIP TRANSFERS at the boundary: the caller dups before pushing,
 * and owns (and frees) whatever it pops. The core needs a callback in exactly
 * one place -- teardown, where it must hand back the cells still inside -- and
 * that is one indirect call per element per container destruction, never on the
 * hot path.
 *
 * ---- Keys ----
 *
 * String-keyed containers take (bytes, len) and COPY the key. Keys are compared
 * as byte strings, never interpreted, so an embedded NUL is a legal key byte.
 * This matches the existing LRU/Trie/BloomFilter convention in this tree.
 *
 * ---- Failure ----
 *
 * Every allocating call returns 0 on success and -1 on out-of-memory, leaving
 * the container unchanged. Nothing aborts, nothing partially applies.
 */
#ifndef DYN_DS_H
#define DYN_DS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque engine value. Sized and aligned for the widest JSValue
 * representation (16 bytes when JS_NAN_BOXING is off, which is the case on
 * every 64-bit host: dynajs.h defines JS_NAN_BOXING only when JS_PTR64 is not).
 * The binding static-asserts sizeof(JSValue) <= sizeof(dyn_cell_t). */
typedef struct { uint64_t w[2]; } dyn_cell_t;

/* Teardown callback: called once per cell still held when a container is
 * freed. `ud` is the binding's context. */
typedef void (*dyn_cell_free_fn)(void *ud, const dyn_cell_t *cell);

/* Copy callback. Needed by exactly one operation -- splitting a RangeMap span
 * in two, where one cell must become two owned references. Never on a hot
 * path. */
typedef void (*dyn_cell_dup_fn)(void *ud, const dyn_cell_t *in,
                                dyn_cell_t *out);

/* ===================================================================== *
 *  Multiset -- Guava Multiset / Commons Bag: byte-string keys to counts
 * ===================================================================== */

typedef struct dyn_mset dyn_mset_t;

dyn_mset_t *dyn_mset_new(void);
void dyn_mset_free(dyn_mset_t *m);

/* Add `delta` (may be negative) to the count of `k`. A count that reaches 0 is
 * removed. Returns 0, or -1 on OOM. `*out` (optional) receives the new count.
 * A negative delta larger than the current count clamps at 0. */
int dyn_mset_add(dyn_mset_t *m, const char *k, size_t n, int64_t delta,
                 uint64_t *out);
int dyn_mset_set_count(dyn_mset_t *m, const char *k, size_t n, uint64_t c);
uint64_t dyn_mset_count(const dyn_mset_t *m, const char *k, size_t n);
uint64_t dyn_mset_total(const dyn_mset_t *m);     /* sum of all counts */
uint32_t dyn_mset_distinct(const dyn_mset_t *m);  /* number of distinct keys */
void dyn_mset_clear(dyn_mset_t *m);

/* Dense iteration: 0 .. dyn_mset_distinct()-1, insertion order. The returned
 * key pointer is owned by the multiset and is invalidated by any mutation. */
int dyn_mset_at(const dyn_mset_t *m, uint32_t i, const char **k, size_t *n,
                uint64_t *count);

/* ===================================================================== *
 *  Multimap -- one byte-string key to many cells
 * ===================================================================== */

typedef struct dyn_mmap dyn_mmap_t;

dyn_mmap_t *dyn_mmap_new(void);
void dyn_mmap_free(dyn_mmap_t *m, dyn_cell_free_fn fn, void *ud);

/* Append `v` under `k`. Takes ownership of the cell. */
int dyn_mmap_put(dyn_mmap_t *m, const char *k, size_t n, const dyn_cell_t *v);
uint32_t dyn_mmap_count(const dyn_mmap_t *m, const char *k, size_t n);
/* Borrowed pointer to the i'th cell under `k`, or NULL. Invalidated by any
 * mutation of that key's list. */
const dyn_cell_t *dyn_mmap_at(const dyn_mmap_t *m, const char *k, size_t n,
                              uint32_t i);
/* Remove the i'th cell under `k`, moving it to *out (caller now owns it).
 * Preserves the order of the remaining values. Returns 1 if removed, 0 if the
 * key or index does not exist. */
int dyn_mmap_remove_at(dyn_mmap_t *m, const char *k, size_t n, uint32_t i,
                       dyn_cell_t *out);
/* Remove a whole key; each removed cell is passed to `fn`. Returns the count
 * removed. */
uint32_t dyn_mmap_remove_key(dyn_mmap_t *m, const char *k, size_t n,
                             dyn_cell_free_fn fn, void *ud);
uint32_t dyn_mmap_keys(const dyn_mmap_t *m);  /* distinct keys */
uint64_t dyn_mmap_size(const dyn_mmap_t *m);  /* total values */
/* Dense key iteration in insertion order. */
int dyn_mmap_key_at(const dyn_mmap_t *m, uint32_t i, const char **k, size_t *n,
                    uint32_t *count);
/* Borrowed pointer to the i'th key's contiguous value block. Lets a caller
 * project or gc-mark a whole key without one hash lookup per element. */
const dyn_cell_t *dyn_mmap_cells_at(const dyn_mmap_t *m, uint32_t i,
                                    uint32_t *count);
/* Same, addressed by key. */
const dyn_cell_t *dyn_mmap_cells_for(const dyn_mmap_t *m, const char *k,
                                     size_t n, uint32_t *count);

/* ===================================================================== *
 *  BiMap -- byte-string <-> byte-string, both directions unique
 * ===================================================================== */

typedef struct dyn_bimap dyn_bimap_t;

dyn_bimap_t *dyn_bimap_new(void);
void dyn_bimap_free(dyn_bimap_t *b);

#define DYN_BIMAP_VALUE_TAKEN 1   /* returned by put() when !force */

/* Bind k <-> v. If `v` is already bound to a different key: with force=0 this
 * returns DYN_BIMAP_VALUE_TAKEN and changes nothing (Guava's put); with
 * force=1 the old pair is evicted (Guava's forcePut). */
int dyn_bimap_put(dyn_bimap_t *b, const char *k, size_t kn,
                  const char *v, size_t vn, int force);
const char *dyn_bimap_get(const dyn_bimap_t *b, const char *k, size_t kn,
                          size_t *vn);
const char *dyn_bimap_key_for(const dyn_bimap_t *b, const char *v, size_t vn,
                              size_t *kn);
int dyn_bimap_remove(dyn_bimap_t *b, const char *k, size_t kn);
int dyn_bimap_remove_value(dyn_bimap_t *b, const char *v, size_t vn);
uint32_t dyn_bimap_size(const dyn_bimap_t *b);
void dyn_bimap_clear(dyn_bimap_t *b);
int dyn_bimap_at(const dyn_bimap_t *b, uint32_t i, const char **k, size_t *kn,
                 const char **v, size_t *vn);

/* ===================================================================== *
 *  Table -- Guava Table: (row, column) -> cell
 * ===================================================================== */

typedef struct dyn_table dyn_table_t;

dyn_table_t *dyn_table_new(void);
void dyn_table_free(dyn_table_t *t, dyn_cell_free_fn fn, void *ud);

/* Insert or replace. On replace the previous cell is moved to *old (caller now
 * owns it) and 1 is returned; on insert 0 is returned. -1 on OOM. */
int dyn_table_put(dyn_table_t *t, const char *r, size_t rn,
                  const char *c, size_t cn, const dyn_cell_t *v,
                  dyn_cell_t *old);
const dyn_cell_t *dyn_table_get(const dyn_table_t *t, const char *r, size_t rn,
                                const char *c, size_t cn);
int dyn_table_remove(dyn_table_t *t, const char *r, size_t rn,
                     const char *c, size_t cn, dyn_cell_t *out);
uint32_t dyn_table_size(const dyn_table_t *t);
/* Dense cell iteration in insertion order (with removals filling from the
 * end, so the order is not stable across removals -- documented). */
/* Walk the records sharing one row (by_column=0) or column (by_column=1).
 * Returns a record index for dyn_table_at, or UINT32_MAX at the end. The
 * chains are built on first use and dropped by any put/remove, so a slice
 * right after a mutation pays one O(n) rebuild and every later slice is
 * O(matches). _first may allocate and returns UINT32_MAX if that fails. */
uint32_t dyn_table_slice_first(dyn_table_t *t, const char *k, size_t n,
                               int by_column);
uint32_t dyn_table_slice_next(const dyn_table_t *t, uint32_t i, int by_column);

int dyn_table_at(const dyn_table_t *t, uint32_t i, const char **r, size_t *rn,
                 const char **c, size_t *cn, const dyn_cell_t **v);

/* ===================================================================== *
 *  RangeSet -- coalescing set of half-open double intervals [lo, hi)
 * ===================================================================== */

typedef struct dyn_rset dyn_rset_t;

dyn_rset_t *dyn_rset_new(void);
void dyn_rset_free(dyn_rset_t *s);

/* An empty or inverted interval (hi <= lo) is a no-op, not an error. */
int dyn_rset_add(dyn_rset_t *s, double lo, double hi);
int dyn_rset_remove(dyn_rset_t *s, double lo, double hi);
int dyn_rset_contains(const dyn_rset_t *s, double x);
int dyn_rset_encloses(const dyn_rset_t *s, double lo, double hi);
int dyn_rset_intersects(const dyn_rset_t *s, double lo, double hi);
uint32_t dyn_rset_count(const dyn_rset_t *s);   /* number of disjoint spans */
int dyn_rset_at(const dyn_rset_t *s, uint32_t i, double *lo, double *hi);
void dyn_rset_clear(dyn_rset_t *s);
/* `out` receives the complement of `s` within [lo, hi). */
int dyn_rset_complement(const dyn_rset_t *s, double lo, double hi,
                        dyn_rset_t *out);

/* ===================================================================== *
 *  RangeMap -- non-coalescing [lo, hi) -> cell, disjoint after every put
 * ===================================================================== */

typedef struct dyn_rmap dyn_rmap_t;

dyn_rmap_t *dyn_rmap_new(void);
void dyn_rmap_free(dyn_rmap_t *m, dyn_cell_free_fn fn, void *ud);

/* Guava's RangeMap.put: the new range wins, overlapping parts of existing
 * ranges are trimmed away, and a range fully covered is dropped. Every cell
 * that stops being reachable is handed to `fn`. */
int dyn_rmap_put(dyn_rmap_t *m, double lo, double hi, const dyn_cell_t *v,
                 dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void *ud);
const dyn_cell_t *dyn_rmap_get(const dyn_rmap_t *m, double x);
int dyn_rmap_remove(dyn_rmap_t *m, double lo, double hi,
                    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void *ud);
uint32_t dyn_rmap_count(const dyn_rmap_t *m);
int dyn_rmap_at(const dyn_rmap_t *m, uint32_t i, double *lo, double *hi,
                const dyn_cell_t **v);

/* ===================================================================== *
 *  IntervalTree -- closed [lo, hi] intervals, overlap ENUMERATION
 * ===================================================================== */

typedef struct dyn_itree dyn_itree_t;

dyn_itree_t *dyn_itree_new(void);
void dyn_itree_free(dyn_itree_t *t, dyn_cell_free_fn fn, void *ud);

int dyn_itree_insert(dyn_itree_t *t, double lo, double hi,
                     const dyn_cell_t *v);
uint32_t dyn_itree_size(const dyn_itree_t *t);
/* Collect the indices of every interval overlapping [lo, hi] into `out`
 * (capacity `cap`). Returns the TOTAL number of matches, which may exceed
 * `cap`: call once with cap=0 to size the buffer, or size it from
 * dyn_itree_size(). Amortises the index build, so the first query after a
 * mutation is O(n log n) and later ones are O(log n + matches). */
uint32_t dyn_itree_query(dyn_itree_t *t, double lo, double hi,
                         uint32_t *out, uint32_t cap);
int dyn_itree_at(const dyn_itree_t *t, uint32_t i, double *lo, double *hi,
                 const dyn_cell_t **v);
/* Remove by index (as returned by a query). Order is not preserved. */
int dyn_itree_remove_at(dyn_itree_t *t, uint32_t i, dyn_cell_t *out);

/* ===================================================================== *
 *  MinMaxHeap -- double-priority interval heap: O(log n) at BOTH ends
 * ===================================================================== */

typedef struct dyn_mmheap dyn_mmheap_t;

/* Priority is a plain double rather than a user comparator, deliberately: a
 * comparator would put a JS call inside the sift loop, which is the whole cost
 * of the existing Heap (measured 725 ns/op for push+pop against 24 ns for a
 * comparator-free Deque op) AND would reintroduce the reentrancy hazard that
 * Heap needs its `busy` flag for. */
dyn_mmheap_t *dyn_mmheap_new(void);
void dyn_mmheap_free(dyn_mmheap_t *h, dyn_cell_free_fn fn, void *ud);

int dyn_mmheap_push(dyn_mmheap_t *h, double pri, const dyn_cell_t *v);
int dyn_mmheap_pop_min(dyn_mmheap_t *h, double *pri, dyn_cell_t *out);
int dyn_mmheap_pop_max(dyn_mmheap_t *h, double *pri, dyn_cell_t *out);
int dyn_mmheap_peek_min(const dyn_mmheap_t *h, double *pri,
                        const dyn_cell_t **v);
int dyn_mmheap_peek_max(const dyn_mmheap_t *h, double *pri,
                        const dyn_cell_t **v);
uint32_t dyn_mmheap_size(const dyn_mmheap_t *h);
/* Raw index-order access, for serialization: the pair at slot i. The order is
 * the heap array's, not sorted -- a record that stored sorted order would
 * depend on the sift algorithm. */
const dyn_cell_t *dyn_mmheap_cell_at(const dyn_mmheap_t *h, uint32_t i);
double dyn_mmheap_pri_at(const dyn_mmheap_t *h, uint32_t i);

/* ===================================================================== *
 *  CountMinSketch -- sublinear frequency estimate, never under-counts
 * ===================================================================== */

typedef struct dyn_cms dyn_cms_t;

/* width = counters per row, depth = number of rows. Both clamped to sane
 * bounds by the caller; this returns NULL on OOM or a zero dimension. */
dyn_cms_t *dyn_cms_new(uint32_t width, uint32_t depth);
void dyn_cms_free(dyn_cms_t *s);
void dyn_cms_add(dyn_cms_t *s, const char *k, size_t n, uint64_t count);
uint64_t dyn_cms_count(const dyn_cms_t *s, const char *k, size_t n);
uint64_t dyn_cms_total(const dyn_cms_t *s);
uint32_t dyn_cms_width(const dyn_cms_t *s);
uint32_t dyn_cms_depth(const dyn_cms_t *s);
/* Merge `b` into `a`; both must have identical dimensions. 0 ok, -1 mismatch. */
int dyn_cms_merge(dyn_cms_t *a, const dyn_cms_t *b);
/* Raw counter access, for serialization. */
const uint64_t *dyn_cms_counters(const dyn_cms_t *s);
uint64_t *dyn_cms_counters_mut(dyn_cms_t *s);
/* Restore the exact total after loading raw counters, which cannot be derived
 * from them (the rows each sum to it, but a merged sketch's rows need not). */
void dyn_cms_set_total(dyn_cms_t *s, uint64_t total);

/* ===================================================================== *
 *  HyperLogLog -- distinct-count estimate in 2^precision bytes
 * ===================================================================== */

typedef struct dyn_hll dyn_hll_t;

#define DYN_HLL_MIN_PRECISION 4
#define DYN_HLL_MAX_PRECISION 18

dyn_hll_t *dyn_hll_new(uint32_t precision);
void dyn_hll_free(dyn_hll_t *h);
void dyn_hll_add(dyn_hll_t *h, const char *k, size_t n);
void dyn_hll_add_hash(dyn_hll_t *h, uint64_t hash);
double dyn_hll_count(const dyn_hll_t *h);
uint32_t dyn_hll_precision(const dyn_hll_t *h);
uint32_t dyn_hll_registers(const dyn_hll_t *h);
int dyn_hll_merge(dyn_hll_t *a, const dyn_hll_t *b);
const uint8_t *dyn_hll_data(const dyn_hll_t *h);
uint8_t *dyn_hll_data_mut(dyn_hll_t *h);
/* 0 if every register is a rank add() could have produced, else -1.
 * A decoder must call this: the registers come off the wire. */
int dyn_hll_regs_valid(const dyn_hll_t *h);

/* =====================================================================
 *  B+tree -- an ordered map on double keys
 *
 * A skiplist chases one pointer per level and lands somewhere new each time.
 * A B+tree reads a whole node of keys per level, so one cache line answers
 * several comparisons and the depth is log_B(n) rather than log_2(n). Values
 * live only in the leaves and the leaves are linked, so a range scan and the
 * iterator walk memory in order instead of descending per element.
 * ===================================================================== */

typedef struct dyn_btree dyn_btree_t;

/* Keys per leaf. 32 doubles is 256 bytes -- four cache lines of keys, read
 * linearly, which measured faster than bisecting them. */
#define DYN_BTREE_ORDER 32

dyn_btree_t *dyn_btree_new(void);
void dyn_btree_free(dyn_btree_t *t, dyn_cell_free_fn fn, void *ud);
uint32_t dyn_btree_size(const dyn_btree_t *t);
/* Returns 1 if an existing key was replaced (its old cell in `old`), 0 if
 * inserted, -1 on allocation failure. */
int dyn_btree_set(dyn_btree_t *t, double k, const dyn_cell_t *v,
                  dyn_cell_t *old, int *replaced);
const dyn_cell_t *dyn_btree_get(const dyn_btree_t *t, double k);
int dyn_btree_del(dyn_btree_t *t, double k, dyn_cell_t *out);
/* Smallest key >= k (ceil) or largest <= k (floor); 0 if there is none. */
int dyn_btree_ceil(const dyn_btree_t *t, double k, double *out);
int dyn_btree_floor(const dyn_btree_t *t, double k, double *out);
int dyn_btree_first(const dyn_btree_t *t, double *out);
int dyn_btree_last(const dyn_btree_t *t, double *out);
/* Ordered iteration. `pos` is opaque; start with dyn_btree_iter_begin and
 * stop when it returns 0. Seek positions the cursor at the first key >= k. */
typedef struct { void *leaf; uint32_t i; } dyn_btree_iter;
int dyn_btree_iter_begin(const dyn_btree_t *t, dyn_btree_iter *it);
int dyn_btree_iter_seek(const dyn_btree_t *t, double k, dyn_btree_iter *it);
int dyn_btree_iter_get(const dyn_btree_iter *it, double *k,
                       const dyn_cell_t **v);
int dyn_btree_iter_next(dyn_btree_iter *it);

#ifdef __cplusplus
}
#endif

#endif /* DYN_DS_H */
