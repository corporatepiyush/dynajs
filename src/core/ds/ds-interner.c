#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-interner.h"

static const char* dyn_interner_store(dyn_interner_t* t, const char* p, size_t n)
{
    dyn_interner_chunk* c = t->chunks;
    char* d;
    if (!c || c->cap - c->used < n + 1) {
        size_t cap = n + 1 > DYN_INTERNER_CHUNK ? n + 1 : DYN_INTERNER_CHUNK;
        dyn_interner_chunk* nc = (dyn_interner_chunk*)DYN_DS_MALLOC(sizeof(dyn_interner_chunk) + cap);
        if (!nc)
            return NULL;
        nc->used = 0;
        nc->cap = cap;
        if (c && n + 1 > DYN_INTERNER_CHUNK) {
            nc->next = c->next;
            c->next = nc;
        } else {
            nc->next = c;
            t->chunks = nc;
        }
        c = nc;
    }
    d = c->data + c->used;
    if (n)
        memcpy(d, p, n);
    d[n] = 0;
    c->used += n + 1;
    t->bytes += n + 1;
    return d;
}

int64_t dyn_interner_intern(dyn_interner_t* t, const char* p, size_t n, int create)
{
    uint64_t h = dyn_ds_strhash(p, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&t->idx, p, n, h);
    const char* d;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (n > UINT32_MAX - 1 || t->n >= 0x7FFFFFFFu)
        return -2;
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 64;
        dyn_interner_str* ns = (dyn_interner_str*)DYN_DS_REALLOC(t->s, sizeof(dyn_interner_str) * nc);
        if (!ns)
            return -2;
        t->s = ns;
        t->cap = nc;
    }
    d = dyn_interner_store(t, p, n);
    if (!d)
        return -2;
    if (!dyn_ds_smap_insert(&t->idx, d, n, h, t->n))
        return -2;
    t->s[t->n].p = d;
    t->s[t->n].len = (uint32_t)n;
    return (int64_t)t->n++;
}

void dyn_interner_free(dyn_interner_t* t)
{
    dyn_interner_chunk* c = t->chunks;
    while (c) {
        dyn_interner_chunk* nx = c->next;
        DYN_DS_FREE(c);
        c = nx;
    }
    DYN_DS_FREE(t->s);
    dyn_ds_smap_free(&t->idx);
    memset(t, 0, sizeof(*t));
}
