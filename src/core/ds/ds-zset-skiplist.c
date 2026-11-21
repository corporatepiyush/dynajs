#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-zset-skiplist.h"

int dyn_zsl_cmp(const dyn_zsl_node* x, double s, const char* k, uint32_t n)
{
    uint32_t m;
    int c;
    if (x->score < s)
        return -1;
    if (x->score > s)
        return 1;
    m = x->klen < n ? x->klen : n;
    c = m ? memcmp(x->key, k, m) : 0;
    if (c)
        return c;
    return (x->klen > n) - (x->klen < n);
}

static dyn_zsl_node* ds_zsl_new_node(dyn_zsl_t* s, double score, const char* k, size_t n)
{
    uint64_t r = ds_splitmix64(&s->rng);
    uint32_t lvl = 1;
    dyn_zsl_node* x;
    while ((r & 3) == 0 && lvl < DYN_ZSL_MAX) {
        lvl++;
        r >>= 2;
    }
    if (n > UINT32_MAX - 1)
        return NULL;
    x = (dyn_zsl_node*)DYN_DS_MALLOC(sizeof(dyn_zsl_node) + lvl * sizeof(dyn_zsl_level) + n + 1);
    if (!x)
        return NULL;
    x->score = score;
    memset(&x->val, 0, sizeof(x->val));
    x->back = NULL;
    x->lvl = lvl;
    x->klen = (uint32_t)n;
    x->key = (char*)&x->lv[lvl];
    if (n)
        memcpy(x->key, k, n);
    x->key[n] = 0;
    return x;
}

int dyn_zsl_init(dyn_zsl_t* s, dyn_cell_free_fn release, void* ud)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->release = release;
    s->ud = ud;
    s->level = 1;
    s->rng = ds_str_seed() ^ (uint64_t)(uintptr_t)s;
    s->head = (dyn_zsl_node*)DYN_DS_MALLOC(sizeof(dyn_zsl_node) + DYN_ZSL_MAX * sizeof(dyn_zsl_level) + 1);
    if (!s->head)
        return -1;
    memset(s->head, 0, sizeof(dyn_zsl_node));
    memset(&s->head->val, 0, sizeof(s->head->val));
    s->head->lvl = DYN_ZSL_MAX;
    s->head->key = (char*)&s->head->lv[DYN_ZSL_MAX];
    s->head->key[0] = 0;
    for (i = 0; i < DYN_ZSL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    return 0;
}

void dyn_zsl_link(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_zsl_node* update[DYN_ZSL_MAX];
    uint32_t rank[DYN_ZSL_MAX];
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        rank[i] = i == s->level - 1 ? 0 : rank[i + 1];
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0) {
            rank[i] += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        update[i] = p;
    }
    if ((int)x->lvl > s->level) {
        for (i = s->level; i < (int)x->lvl; i++) {
            rank[i] = 0;
            update[i] = s->head;
            update[i]->lv[i].span = s->len;
        }
        s->level = (int)x->lvl;
    }
    for (i = 0; i < (int)x->lvl; i++) {
        x->lv[i].fwd = update[i]->lv[i].fwd;
        update[i]->lv[i].fwd = x;
        x->lv[i].span = update[i]->lv[i].span - (rank[0] - rank[i]);
        update[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = (int)x->lvl; i < s->level; i++)
        update[i]->lv[i].span++;
    x->back = update[0] == s->head ? NULL : update[0];
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x;
    else
        s->tail = x;
    s->len++;
}

void dyn_zsl_unlink(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_zsl_node* update[DYN_ZSL_MAX];
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd != x && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0)
            p = p->lv[i].fwd;
        update[i] = p;
    }
    for (i = 0; i < s->level; i++) {
        if (update[i]->lv[i].fwd == x) {
            update[i]->lv[i].span += x->lv[i].span - 1;
            update[i]->lv[i].fwd = x->lv[i].fwd;
        } else {
            update[i]->lv[i].span -= 1;
        }
    }
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x->back;
    else
        s->tail = x->back;
    while (s->level > 1 && s->head->lv[s->level - 1].fwd == NULL)
        s->level--;
    s->len--;
}

uint32_t dyn_zsl_rank(const dyn_zsl_t* s, const dyn_zsl_node* x)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) <= 0) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (p == x)
            return r;
    }
    return 0;
}

dyn_zsl_node* dyn_zsl_at(const dyn_zsl_t* s, uint32_t rank)
{
    uint32_t t = 0;
    dyn_zsl_node* p = s->head;
    int i;
    if (rank == 0 || rank > s->len)
        return NULL;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && t + p->lv[i].span <= rank) {
            t += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (t == rank)
            return p;
    }
    return NULL;
}

dyn_zsl_node* dyn_zsl_lt(const dyn_zsl_t* s, double sc, const char* k, uint32_t n, int le, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && dyn_zsl_cmp(p->lv[i].fwd, sc, k, n) < le) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

dyn_zsl_node* dyn_zsl_first_ge_score(const dyn_zsl_t* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score < sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r + 1;
    return p->lv[0].fwd;
}

dyn_zsl_node* dyn_zsl_last_le_score(const dyn_zsl_t* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    dyn_zsl_node* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score <= sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

dyn_zsl_node* dyn_zsl_find(const dyn_zsl_t* s, const char* k, size_t n)
{
    dyn_ds_sent* e = dyn_ds_smap_find(&s->idx, k, n, dyn_ds_strhash(k, n));
    return e ? (dyn_zsl_node*)e->v : NULL;
}

void dyn_zsl_drop(dyn_zsl_t* s, dyn_zsl_node* x)
{
    dyn_ds_sent* e = dyn_ds_smap_find(&s->idx, x->key, x->klen, dyn_ds_strhash(x->key, x->klen));
    if (e)
        dyn_ds_smap_remove(&s->idx, e);
    dyn_zsl_unlink(s, x);
    if (s->release)
        s->release(s->ud, &x->val);
    DYN_DS_FREE(x);
}

int dyn_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, const dyn_cell_t* val, dyn_zsl_node** out)
{
    dyn_zsl_node* x = ds_zsl_new_node(s, score, k, n);
    if (!x)
        return -1;
    if (!dyn_ds_smap_insert(&s->idx, x->key, x->klen, dyn_ds_strhash(x->key, x->klen), (uintptr_t)x)) {
        DYN_DS_FREE(x);
        return -1;
    }
    x->val = *val;
    dyn_zsl_link(s, x);
    if (out)
        *out = x;
    return 0;
}

void dyn_zsl_clear(dyn_zsl_t* s)
{
    dyn_zsl_node* x = s->head->lv[0].fwd;
    int i;
    while (x) {
        dyn_zsl_node* nx = x->lv[0].fwd;
        if (s->release)
            s->release(s->ud, &x->val);
        DYN_DS_FREE(x);
        x = nx;
    }
    for (i = 0; i < DYN_ZSL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    s->tail = NULL;
    s->len = 0;
    s->level = 1;
    dyn_ds_smap_free(&s->idx);
}

void dyn_zsl_destroy(dyn_zsl_t* s)
{
    if (!s->head)
        return;
    dyn_zsl_clear(s);
    DYN_DS_FREE(s->head);
    s->head = NULL;
}
