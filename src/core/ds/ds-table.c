#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-table.h"

typedef struct {
    ds_key row;
    ds_key col;
    ds_key pair;
    dyn_cell_t val;
} ds_table_rec;

typedef struct {
    uint32_t head, tail;
} ds_table_chain;

struct dyn_table {
    ds_table_rec* rec;
    uint32_t len, cap;
    dyn_ix ix;
    dyn_ix row_ix, col_ix;
    ds_table_chain *row_chain, *col_chain;
    uint32_t row_chains, row_chain_cap, col_chains, col_chain_cap;
    uint32_t *row_next, *col_next;
    uint32_t slice_cap;
    uint32_t sliced;
    int slices_valid;
};

#define DS_TABLE_PAIR_INLINE 192

static void ds_table_drop_slices(dyn_table_t* t)
{
    ds_ix_free(&t->row_ix);
    ds_ix_free(&t->col_ix);
    DYN_DS_FREE(t->row_chain);
    DYN_DS_FREE(t->col_chain);
    DYN_DS_FREE(t->row_next);
    DYN_DS_FREE(t->col_next);
    t->row_chain = NULL;
    t->col_chain = NULL;
    t->row_next = NULL;
    t->col_next = NULL;
    t->row_chains = t->col_chains = 0;
    t->row_chain_cap = t->col_chain_cap = 0;
    t->slice_cap = 0;
    t->sliced = 0;
    t->slices_valid = 0;
}

static int ds_table_chain_add(dyn_ix* ix, ds_table_chain** chain, uint32_t* nchain,
    uint32_t* cap, uint32_t* next, const ds_key* k,
    uint32_t i)
{
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n);
    next[i] = UINT32_MAX;
    if (pos != UINT32_MAX) {
        ds_table_chain* c = &(*chain)[ix->slot[pos].idx];
        next[c->tail] = i;
        c->tail = i;
        return 0;
    }
    if (ds_reserve((void**)chain, cap, *nchain + 1, sizeof(**chain)) < 0)
        return -1;
    (*chain)[*nchain].head = i;
    (*chain)[*nchain].tail = i;
    if (ds_ix_insert(ix, k->h, k->p, k->n, *nchain) < 0)
        return -1;
    (*nchain)++;
    return 0;
}

static void ds_table_chain_unlink(dyn_table_t* t, int by_column, uint32_t i)
{
    dyn_ix* ix = by_column ? &t->col_ix : &t->row_ix;
    ds_table_chain* chain = by_column ? t->col_chain : t->row_chain;
    uint32_t* next = by_column ? t->col_next : t->row_next;
    const ds_key* k = by_column ? &t->rec[i].col : &t->rec[i].row;
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n), p;
    ds_table_chain* c;

    if (pos == UINT32_MAX)
        return;
    c = &chain[ix->slot[pos].idx];
    if (c->head == i) {
        if (c->tail == i) {
            ds_ix_erase(ix, pos);
            return;
        }
        c->head = next[i];
    } else {
        for (p = c->head; p != UINT32_MAX && next[p] != i; p = next[p])
            ;
        if (p == UINT32_MAX)
            return;
        next[p] = next[i];
        if (c->tail == i)
            c->tail = p;
    }
    ix->slot[pos].key = by_column ? t->rec[c->head].col.p
                                  : t->rec[c->head].row.p;
}

static void ds_table_chain_move(dyn_table_t* t, int by_column, uint32_t from,
    uint32_t to)
{
    dyn_ix* ix = by_column ? &t->col_ix : &t->row_ix;
    ds_table_chain* chain = by_column ? t->col_chain : t->row_chain;
    uint32_t* next = by_column ? t->col_next : t->row_next;
    const ds_key* k = by_column ? &t->rec[to].col : &t->rec[to].row;
    uint32_t pos = ds_ix_find(ix, k->h, k->p, k->n), p;
    ds_table_chain* c;

    if (pos == UINT32_MAX)
        return;
    c = &chain[ix->slot[pos].idx];
    next[to] = next[from];
    if (c->head == from) {
        c->head = to;
    } else {
        for (p = c->head; p != UINT32_MAX && next[p] != from; p = next[p])
            ;
        if (p != UINT32_MAX)
            next[p] = to;
    }
    if (c->tail == from)
        c->tail = to;
}

static int ds_table_build_slices(dyn_table_t* t)
{
    uint32_t i;

    if (t->len > t->slice_cap) {
        uint32_t cap = t->slice_cap ? t->slice_cap : 16;
        uint32_t *rn, *cn;
        while (cap < t->len)
            cap *= 2;
        rn = (uint32_t*)DYN_DS_REALLOC(t->row_next, (size_t)cap * sizeof(uint32_t));
        if (!rn)
            return -1;
        t->row_next = rn;
        cn = (uint32_t*)DYN_DS_REALLOC(t->col_next, (size_t)cap * sizeof(uint32_t));
        if (!cn)
            return -1;
        t->col_next = cn;
        t->slice_cap = cap;
    }
    for (i = t->sliced; i < t->len; i++) {
        if (ds_table_chain_add(&t->row_ix, &t->row_chain, &t->row_chains,
                &t->row_chain_cap, t->row_next,
                &t->rec[i].row, i)
                < 0
            || ds_table_chain_add(&t->col_ix, &t->col_chain, &t->col_chains,
                   &t->col_chain_cap, t->col_next,
                   &t->rec[i].col, i)
                < 0) {
            ds_table_drop_slices(t);
            return -1;
        }
    }
    t->sliced = t->len;
    t->slices_valid = 1;
    return 0;
}

uint32_t dyn_table_slice_first(dyn_table_t* t, const char* k, size_t n,
    int by_column)
{
    const dyn_ix* ix;
    const ds_table_chain* chain;
    uint32_t pos;
    if ((!t->slices_valid || t->sliced != t->len) && ds_table_build_slices(t) < 0)
        return UINT32_MAX;
    ix = by_column ? &t->col_ix : &t->row_ix;
    chain = by_column ? t->col_chain : t->row_chain;
    pos = ds_ix_find(ix, ds_ix_hash(k, n), k, n);
    return pos == UINT32_MAX ? UINT32_MAX : chain[ix->slot[pos].idx].head;
}

uint32_t dyn_table_slice_next(const dyn_table_t* t, uint32_t i, int by_column)
{
    const uint32_t* next = by_column ? t->col_next : t->row_next;
    if (!t->slices_valid || !next || i >= t->len)
        return UINT32_MAX;
    return next[i];
}

dyn_table_t* dyn_table_new(void)
{
    return (dyn_table_t*)ds_calloc(1, sizeof(dyn_table_t));
}

void dyn_table_free(dyn_table_t* t, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < t->len; i++) {
        if (fn)
            fn(ud, &t->rec[i].val);
        DYN_DS_FREE(t->rec[i].row.p);
        DYN_DS_FREE(t->rec[i].col.p);
        DYN_DS_FREE(t->rec[i].pair.p);
    }
    DYN_DS_FREE(t->rec);
    ds_ix_free(&t->ix);
    ds_table_drop_slices(t);
    DYN_DS_FREE(t);
}

static int ds_table_pair_into(char* inl, char** out, size_t* outn, const char* r,
    size_t rn, const char* c, size_t cn)
{
    size_t n;
    char* p;
    int i;
    if (rn > SIZE_MAX - 8 - cn)
        return -1;
    n = 8 + rn + cn;
    p = (inl && n <= DS_TABLE_PAIR_INLINE) ? inl : (char*)DYN_DS_MALLOC(n);
    if (!p)
        return -1;
    for (i = 0; i < 8; i++)
        p[i] = (char)((uint64_t)rn >> (i * 8));
    memcpy(p + 8, r, rn);
    memcpy(p + 8 + rn, c, cn);
    *out = p;
    *outn = n;
    return 0;
}

static int ds_table_pair(char** out, size_t* outn, const char* r, size_t rn,
    const char* c, size_t cn)
{
    return ds_table_pair_into(NULL, out, outn, r, rn, c, cn);
}

static void ds_table_pair_release(char* inl, char* p)
{
    if (p != inl)
        DYN_DS_FREE(p);
}

int dyn_table_put(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, const dyn_cell_t* v,
    dyn_cell_t* old)
{
    char* pk;
    size_t pn;
    uint64_t h;
    uint32_t pos;
    ds_table_rec* rec;

    if (ds_table_pair(&pk, &pn, r, rn, c, cn) < 0)
        return -1;
    h = ds_ix_hash(pk, pn);
    pos = ds_ix_find(&t->ix, h, pk, pn);
    if (pos != UINT32_MAX) {
        uint32_t idx = t->ix.slot[pos].idx;
        DYN_DS_FREE(pk);
        *old = t->rec[idx].val;
        t->rec[idx].val = *v;
        return 1;
    }
    if (ds_reserve((void**)&t->rec, &t->cap, t->len + 1,
            sizeof(*t->rec))
        < 0) {
        DYN_DS_FREE(pk);
        return -1;
    }
    rec = &t->rec[t->len];
    memset(rec, 0, sizeof(*rec));
    if (ds_key_init(&rec->row, r, rn, ds_ix_hash(r, rn)) < 0 || ds_key_init(&rec->col, c, cn, ds_ix_hash(c, cn)) < 0) {
        DYN_DS_FREE(rec->row.p);
        DYN_DS_FREE(pk);
        return -1;
    }
    rec->pair.p = pk;
    rec->pair.n = pn;
    rec->pair.h = h;
    rec->val = *v;
    if (ds_ix_insert(&t->ix, h, pk, pn, t->len) < 0) {
        DYN_DS_FREE(rec->row.p);
        DYN_DS_FREE(rec->col.p);
        DYN_DS_FREE(pk);
        return -1;
    }
    t->len++;
    if (t->slices_valid && ds_table_build_slices(t) < 0)
        ds_table_drop_slices(t);
    return 0;
}

const dyn_cell_t* dyn_table_get(const dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn)
{
    char pk_inl[DS_TABLE_PAIR_INLINE];
    char* pk;
    size_t pn;
    uint32_t pos;
    const dyn_cell_t* out = NULL;

    if (ds_table_pair_into(pk_inl, &pk, &pn, r, rn, c, cn) < 0)
        return NULL;
    pos = ds_ix_find(&t->ix, ds_ix_hash(pk, pn), pk, pn);
    if (pos != UINT32_MAX)
        out = &t->rec[t->ix.slot[pos].idx].val;
    ds_table_pair_release(pk_inl, pk);
    return out;
}

int dyn_table_remove(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, dyn_cell_t* out)
{
    char pk_inl[DS_TABLE_PAIR_INLINE];
    char* pk;
    size_t pn;
    uint32_t pos, idx;

    if (ds_table_pair_into(pk_inl, &pk, &pn, r, rn, c, cn) < 0)
        return 0;
    pos = ds_ix_find(&t->ix, ds_ix_hash(pk, pn), pk, pn);
    ds_table_pair_release(pk_inl, pk);
    if (pos == UINT32_MAX)
        return 0;
    idx = t->ix.slot[pos].idx;
    *out = t->rec[idx].val;
    ds_ix_erase(&t->ix, pos);
    if (t->slices_valid) {
        ds_table_chain_unlink(t, 0, idx);
        ds_table_chain_unlink(t, 1, idx);
    }
    DYN_DS_FREE(t->rec[idx].row.p);
    DYN_DS_FREE(t->rec[idx].col.p);
    DYN_DS_FREE(t->rec[idx].pair.p);
    t->len--;
    if (idx != t->len) {
        t->rec[idx] = t->rec[t->len];
        ds_ix_move(&t->ix, t->rec[idx].pair.h, t->rec[idx].pair.p,
            t->rec[idx].pair.n, idx);
        if (t->slices_valid) {
            ds_table_chain_move(t, 0, t->len, idx);
            ds_table_chain_move(t, 1, t->len, idx);
        }
    }
    if (t->slices_valid)
        t->sliced = t->len;
    return 1;
}

uint32_t dyn_table_size(const dyn_table_t* t) { return t->len; }

int dyn_table_at(const dyn_table_t* t, uint32_t i, const char** r, size_t* rn,
    const char** c, size_t* cn, const dyn_cell_t** v)
{
    if (i >= t->len)
        return 0;
    *r = t->rec[i].row.p;
    *rn = t->rec[i].row.n;
    *c = t->rec[i].col.p;
    *cn = t->rec[i].col.n;
    *v = &t->rec[i].val;
    return 1;
}
