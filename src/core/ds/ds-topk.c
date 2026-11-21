#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-topk.h"

static void dyn_topk_swap(dyn_topk_t* t, uint32_t a, uint32_t b)
{
    uint32_t x = t->heap[a];
    t->heap[a] = t->heap[b];
    t->heap[b] = x;
    t->s[t->heap[a]].hpos = a;
    t->s[t->heap[b]].hpos = b;
}

static int dyn_topk_less(const dyn_topk_t* t, uint32_t a, uint32_t b)
{
    return t->s[t->heap[a]].count < t->s[t->heap[b]].count;
}

static void dyn_topk_down(dyn_topk_t* t, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && dyn_topk_less(t, l, m))
            m = l;
        if (r < t->n && dyn_topk_less(t, r, m))
            m = r;
        if (m == i)
            return;
        dyn_topk_swap(t, i, m);
        i = m;
    }
}

void dyn_topk_up(dyn_topk_t* t, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!dyn_topk_less(t, i, p))
            return;
        dyn_topk_swap(t, i, p);
        i = p;
    }
}

void dyn_topk_free(dyn_topk_t* t)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        DYN_DS_FREE(t->s[i].key);
    DYN_DS_FREE(t->s);
    DYN_DS_FREE(t->heap);
    dyn_ds_smap_free(&t->idx);
}

int dyn_topk_add(dyn_topk_t* t, const char* k, size_t n, double c, char** evicted, size_t* evn)
{
    uint64_t h = dyn_ds_strhash(k, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&t->idx, k, n, h);
    *evicted = NULL;
    t->total += c;
    if (e) {
        dyn_topk_slot* s = &t->s[e->v];
        s->count += c;
        dyn_topk_down(t, s->hpos);
        return 0;
    }
    if (n > UINT32_MAX - 1)
        return -1;
    if (t->n < t->k) {
        uint32_t i = t->n;
        dyn_topk_slot* s = &t->s[i];
        s->key = (char*)dyn_ds_bytes_dup(k, n);
        if (!s->key)
            return -1;
        s->klen = (uint32_t)n;
        s->count = c;
        s->err = 0;
        if (!dyn_ds_smap_insert(&t->idx, s->key, n, h, i)) {
            DYN_DS_FREE(s->key);
            return -1;
        }
        t->heap[i] = i;
        s->hpos = i;
        t->n++;
        dyn_topk_up(t, i);
        return 0;
    }
    {
        uint32_t si = t->heap[0];
        dyn_topk_slot* s = &t->s[si];
        char* nk = (char*)dyn_ds_bytes_dup(k, n);
        dyn_ds_sent* old;
        if (!nk)
            return -1;
        old = dyn_ds_smap_find(&t->idx, s->key, s->klen, dyn_ds_strhash(s->key, s->klen));
        if (old)
            dyn_ds_smap_remove(&t->idx, old);
        if (!dyn_ds_smap_insert(&t->idx, nk, n, h, si)) {
            dyn_ds_smap_insert(&t->idx, s->key, s->klen, dyn_ds_strhash(s->key, s->klen), si);
            DYN_DS_FREE(nk);
            return -1;
        }
        *evicted = s->key;
        *evn = s->klen;
        s->key = nk;
        s->klen = (uint32_t)n;
        s->err = s->count;
        s->count += c;
        dyn_topk_down(t, 0);
        return 0;
    }
}

static _Thread_local const dyn_topk_t* ds_topk_sort_ctx;

static int ds_topk_cmp_desc(const void* a, const void* b)
{
    const dyn_topk_slot* x = &ds_topk_sort_ctx->s[*(const uint32_t*)a];
    const dyn_topk_slot* y = &ds_topk_sort_ctx->s[*(const uint32_t*)b];
    uint32_t m;
    int c;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    m = x->klen < y->klen ? x->klen : y->klen;
    c = m ? memcmp(x->key, y->key, m) : 0;
    return c ? c : (x->klen > y->klen) - (x->klen < y->klen);
}

void dyn_topk_sorted(const dyn_topk_t* t, uint32_t* ord)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        ord[i] = i;
    ds_topk_sort_ctx = t;
    qsort(ord, t->n, sizeof(uint32_t), ds_topk_cmp_desc);
    ds_topk_sort_ctx = NULL;
}

int dyn_topk_init(dyn_topk_t* t, uint32_t k)
{
    memset(t, 0, sizeof(*t));
    t->k = k;
    t->s = (dyn_topk_slot*)DYN_DS_CALLOC(k ? k : 1, sizeof(dyn_topk_slot));
    t->heap = (uint32_t*)DYN_DS_CALLOC(k ? k : 1, sizeof(uint32_t));
    if (!t->s || !t->heap) {
        dyn_topk_free(t);
        return -1;
    }
    return 0;
}
