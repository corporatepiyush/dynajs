#include "ds-internal.h"
#include "ds-multimap.h"

typedef struct {
    ds_key key;
    dyn_cell_t* val;
    uint32_t len, cap;
} ds_mmap_rec;

struct dyn_mmap {
    ds_mmap_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mmap_t* dyn_mmap_new(void)
{
    return (dyn_mmap_t*)ds_calloc(1, sizeof(dyn_mmap_t));
}

void dyn_mmap_free(dyn_mmap_t* m, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i, j;
    if (!m)
        return;
    for (i = 0; i < m->len; i++) {
        if (fn)
            for (j = 0; j < m->rec[i].len; j++)
                fn(ud, &m->rec[i].val[j]);
        DYN_DS_FREE(m->rec[i].val);
        DYN_DS_FREE(m->rec[i].key.p);
    }
    DYN_DS_FREE(m->rec);
    ds_ix_free(&m->ix);
    DYN_DS_FREE(m);
}

static ds_mmap_rec* ds_mmap_lookup(const dyn_mmap_t* m, const char* k, size_t n)
{
    uint32_t pos = ds_ix_find(&m->ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? NULL : &m->rec[m->ix.slot[pos].idx];
}

int dyn_mmap_put(dyn_mmap_t* m, const char* k, size_t n, const dyn_cell_t* v)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    ds_mmap_rec* r;

    if (pos == UINT32_MAX) {
        if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1,
                sizeof(*m->rec))
            < 0)
            return -1;
        r = &m->rec[m->len];
        memset(r, 0, sizeof(*r));
        if (ds_key_init(&r->key, k, n, h) < 0)
            return -1;
        if (ds_reserve((void**)&r->val, &r->cap, 1, sizeof(*r->val)) < 0) {
            DYN_DS_FREE(r->key.p);
            return -1;
        }
        if (ds_ix_insert(&m->ix, h, r->key.p, n, m->len) < 0) {
            DYN_DS_FREE(r->val);
            DYN_DS_FREE(r->key.p);
            return -1;
        }
        m->len++;
    } else {
        r = &m->rec[m->ix.slot[pos].idx];
        if (ds_reserve((void**)&r->val, &r->cap, r->len + 1,
                sizeof(*r->val))
            < 0)
            return -1;
    }
    r->val[r->len++] = *v;
    m->total++;
    return 0;
}

uint32_t dyn_mmap_count(const dyn_mmap_t* m, const char* k, size_t n)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    return r ? r->len : 0;
}

const dyn_cell_t* dyn_mmap_at(const dyn_mmap_t* m, const char* k, size_t n,
    uint32_t i)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    return (r && i < r->len) ? &r->val[i] : NULL;
}

static void ds_mmap_erase(dyn_mmap_t* m, uint32_t idx, uint32_t pos)
{
    ds_ix_erase(&m->ix, pos);
    DYN_DS_FREE(m->rec[idx].val);
    DYN_DS_FREE(m->rec[idx].key.p);
    m->len--;
    if (idx != m->len) {
        m->rec[idx] = m->rec[m->len];
        ds_ix_move(&m->ix, m->rec[idx].key.h, m->rec[idx].key.p,
            m->rec[idx].key.n, idx);
    }
}

int dyn_mmap_remove_at(dyn_mmap_t* m, const char* k, size_t n, uint32_t i,
    dyn_cell_t* out)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n), idx;
    ds_mmap_rec* r;

    if (pos == UINT32_MAX)
        return 0;
    idx = m->ix.slot[pos].idx;
    r = &m->rec[idx];
    if (i >= r->len)
        return 0;
    *out = r->val[i];
    memmove(&r->val[i], &r->val[i + 1],
        (size_t)(r->len - i - 1) * sizeof(*r->val));
    r->len--;
    m->total--;
    if (r->len == 0)
        ds_mmap_erase(m, idx, pos);
    return 1;
}

uint32_t dyn_mmap_remove_key(dyn_mmap_t* m, const char* k, size_t n,
    dyn_cell_free_fn fn, void* ud)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n), idx, i, cnt;

    if (pos == UINT32_MAX)
        return 0;
    idx = m->ix.slot[pos].idx;
    cnt = m->rec[idx].len;
    if (fn)
        for (i = 0; i < cnt; i++)
            fn(ud, &m->rec[idx].val[i]);
    m->total -= cnt;
    ds_mmap_erase(m, idx, pos);
    return cnt;
}

uint32_t dyn_mmap_keys(const dyn_mmap_t* m) { return m->len; }

uint64_t dyn_mmap_size(const dyn_mmap_t* m) { return m->total; }

int dyn_mmap_key_at(const dyn_mmap_t* m, uint32_t i, const char** k, size_t* n,
    uint32_t* count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].len;
    return 1;
}

const dyn_cell_t* dyn_mmap_cells_at(const dyn_mmap_t* m, uint32_t i,
    uint32_t* count)
{
    if (i >= m->len) {
        *count = 0;
        return NULL;
    }
    *count = m->rec[i].len;
    return m->rec[i].val;
}

const dyn_cell_t* dyn_mmap_cells_for(const dyn_mmap_t* m, const char* k,
    size_t n, uint32_t* count)
{
    ds_mmap_rec* r = ds_mmap_lookup(m, k, n);
    *count = r ? r->len : 0;
    return r ? r->val : NULL;
}
