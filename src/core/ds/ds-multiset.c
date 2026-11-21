#include "ds-internal.h"
#include "ds-multiset.h"

typedef struct {
    ds_key key;
    uint64_t count;
} ds_mset_rec;

struct dyn_mset {
    ds_mset_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    uint64_t total;
};

dyn_mset_t* dyn_mset_new(void)
{
    dyn_mset_t* m = (dyn_mset_t*)ds_calloc(1, sizeof(*m));
    return m;
}

void dyn_mset_clear(dyn_mset_t* m)
{
    uint32_t i;
    for (i = 0; i < m->len; i++)
        DYN_DS_FREE(m->rec[i].key.p);
    m->len = 0;
    m->total = 0;
    ds_ix_free(&m->ix);
}

void dyn_mset_free(dyn_mset_t* m)
{
    if (!m)
        return;
    dyn_mset_clear(m);
    DYN_DS_FREE(m->rec);
    DYN_DS_FREE(m);
}

static void ds_mset_erase(dyn_mset_t* m, uint32_t i, uint32_t pos)
{
    ds_ix_erase(&m->ix, pos);
    DYN_DS_FREE(m->rec[i].key.p);
    m->len--;
    if (i != m->len) {
        m->rec[i] = m->rec[m->len];
        ds_ix_move(&m->ix, m->rec[i].key.h, m->rec[i].key.p, m->rec[i].key.n, i);
    }
}

int dyn_mset_add(dyn_mset_t* m, const char* k, size_t n, int64_t delta,
    uint64_t* out)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    uint64_t cur = 0;

    if (pos != UINT32_MAX)
        cur = m->rec[m->ix.slot[pos].idx].count;

    if (delta < 0) {
        uint64_t sub = (uint64_t)(-(delta + 1)) + 1;
        uint64_t nv = sub >= cur ? 0 : cur - sub;
        m->total -= cur - nv;
        if (pos != UINT32_MAX) {
            uint32_t idx = m->ix.slot[pos].idx;
            if (nv == 0)
                ds_mset_erase(m, idx, pos);
            else
                m->rec[idx].count = nv;
        }
        if (out)
            *out = nv;
        return 0;
    }
    if (delta == 0) {
        if (out)
            *out = cur;
        return 0;
    }
    if (pos != UINT32_MAX) {
        uint32_t idx = m->ix.slot[pos].idx;
        uint64_t nc = ds_u64_sat_add(cur, (uint64_t)delta);
        m->total = ds_u64_sat_add(m->total, nc - cur);
        m->rec[idx].count = nc;
        if (out)
            *out = nc;
        return 0;
    }
    if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (ds_key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = (uint64_t)delta;
    if (ds_ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        DYN_DS_FREE(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total = ds_u64_sat_add(m->total, (uint64_t)delta);
    if (out)
        *out = (uint64_t)delta;
    return 0;
}

int dyn_mset_set_count(dyn_mset_t* m, const char* k, size_t n, uint64_t c)
{
    uint64_t h = ds_ix_hash(k, n);
    uint32_t pos = ds_ix_find(&m->ix, h, k, n);
    if (pos != UINT32_MAX) {
        uint32_t idx = m->ix.slot[pos].idx;
        m->total = ds_u64_sat_add(m->total - m->rec[idx].count, c);
        if (c == 0)
            ds_mset_erase(m, idx, pos);
        else
            m->rec[idx].count = c;
        return 0;
    }
    if (c == 0)
        return 0;
    if (ds_reserve((void**)&m->rec, &m->cap, m->len + 1, sizeof(*m->rec)) < 0)
        return -1;
    if (ds_key_init(&m->rec[m->len].key, k, n, h) < 0)
        return -1;
    m->rec[m->len].count = c;
    if (ds_ix_insert(&m->ix, h, m->rec[m->len].key.p, n, m->len) < 0) {
        DYN_DS_FREE(m->rec[m->len].key.p);
        return -1;
    }
    m->len++;
    m->total = ds_u64_sat_add(m->total, c);
    return 0;
}

uint64_t dyn_mset_count(const dyn_mset_t* m, const char* k, size_t n)
{
    uint32_t pos = ds_ix_find(&m->ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? 0 : m->rec[m->ix.slot[pos].idx].count;
}

uint64_t dyn_mset_total(const dyn_mset_t* m) { return m->total; }

uint32_t dyn_mset_distinct(const dyn_mset_t* m) { return m->len; }

int dyn_mset_at(const dyn_mset_t* m, uint32_t i, const char** k, size_t* n,
    uint64_t* count)
{
    if (i >= m->len)
        return 0;
    *k = m->rec[i].key.p;
    *n = m->rec[i].key.n;
    *count = m->rec[i].count;
    return 1;
}
