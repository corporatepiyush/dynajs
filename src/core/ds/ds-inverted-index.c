#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-inverted-index.h"

static inline uint64_t ds_ix_mix(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    uint64_t lo = (a & 0xFFFFFFFFu) * (b & 0xFFFFFFFFu);
    uint64_t hi = (a >> 32) * (b >> 32);
    uint64_t m1 = (a >> 32) * (b & 0xFFFFFFFFu);
    uint64_t m2 = (a & 0xFFFFFFFFu) * (b >> 32);
    return (lo + (m1 << 32) + (m2 << 32)) ^ (hi + (m1 >> 32) + (m2 >> 32));
#endif
}

uint64_t ds_ix_hash(const char* k, size_t n)
{
    const uint8_t* p = (const uint8_t*)k;
    uint64_t a, b, h;
    uint64_t s0 = atomic_load_explicit(&ds_seed0, memory_order_relaxed);
    uint64_t s1 = atomic_load_explicit(&ds_seed1, memory_order_relaxed);

    if (n >= 16) {
        h = dyn_ds_xxh64(p, n, s1);
        return h ? h : 1;
    }
    if (n >= 8) {
        memcpy(&a, p, 8);
        memcpy(&b, p + n - 8, 8);
    } else if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, p + n - 4, 4);
        a = x;
        b = y;
    } else if (n) {
        a = ((uint64_t)p[0] << 16) | ((uint64_t)p[n >> 1] << 8) | p[n - 1];
        b = 0;
    } else {
        a = b = 0;
    }
    h = ds_ix_mix(a ^ s0, b ^ s1 ^ (uint64_t)n);
    return h ? h : 1;
}

void ds_ix_free(dyn_ix* ix)
{
    DYN_DS_FREE(ix->slot);
    ix->slot = NULL;
    ix->cap = ix->used = 0;
}

static void ds_ix_put_raw(dyn_ixslot* slot, uint32_t mask, uint64_t h,
    const char* k, size_t n, uint32_t idx)
{
    uint32_t i = (uint32_t)h & mask;
    while (slot[i].h)
        i = (i + 1) & mask;
    slot[i].h = h;
    slot[i].key = k;
    slot[i].klen = n;
    slot[i].idx = idx;
}

static int ds_ix_grow(dyn_ix* ix)
{
    uint32_t nc = ix->cap ? ix->cap * 2 : DS_MIN_CAP;
    dyn_ixslot* ns;
    uint32_t i;
    if (ix->cap > 0x40000000u)
        return -1;
    ns = (dyn_ixslot*)ds_calloc(nc, sizeof(*ns));
    if (!ns)
        return -1;
    for (i = 0; i < ix->cap; i++)
        if (ix->slot[i].h)
            ds_ix_put_raw(ns, nc - 1, ix->slot[i].h, ix->slot[i].key,
                ix->slot[i].klen, ix->slot[i].idx);
    DYN_DS_FREE(ix->slot);
    ix->slot = ns;
    ix->cap = nc;
    return 0;
}

uint32_t ds_ix_find(const dyn_ix* ix, uint64_t h, const char* k, size_t n)
{
    uint32_t mask, i;
    if (!ix->cap)
        return UINT32_MAX;
    mask = ix->cap - 1;
    i = (uint32_t)h & mask;
    while (ix->slot[i].h) {
        if (ix->slot[i].h == h && ix->slot[i].klen == n && memcmp(ix->slot[i].key, k, n) == 0)
            return i;
        i = (i + 1) & mask;
    }
    return UINT32_MAX;
}

int ds_ix_reserve(dyn_ix* ix)
{
    if ((ix->used + 1) * 10 >= ix->cap * 7)
        return ds_ix_grow(ix);
    return 0;
}

int ds_ix_insert(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx)
{
    if (ds_ix_reserve(ix) < 0)
        return -1;
    ds_ix_put_raw(ix->slot, ix->cap - 1, h, k, n, idx);
    ix->used++;
    return 0;
}

void ds_ix_erase(dyn_ix* ix, uint32_t pos)
{
    uint32_t mask = ix->cap - 1, i = pos, j;
    ix->slot[i].h = 0;
    ix->used--;
    j = i;
    for (;;) {
        uint32_t k;
        j = (j + 1) & mask;
        if (!ix->slot[j].h)
            break;
        k = (uint32_t)ix->slot[j].h & mask;
        if ((j > i) ? (k <= i || k > j) : (k <= i && k > j)) {
            ix->slot[i] = ix->slot[j];
            ix->slot[j].h = 0;
            i = j;
        }
    }
}

void ds_ix_move(dyn_ix* ix, uint64_t h, const char* k, size_t n,
    uint32_t idx)
{
    uint32_t pos = ds_ix_find(ix, h, k, n);
    if (pos != UINT32_MAX)
        ix->slot[pos].idx = idx;
}

static const uint8_t dyn_invidx_class[256] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    1,
    0,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
};

static int dyn_invidx_word_byte(const unsigned char* s, size_t n, size_t i, size_t* skip)
{
    unsigned char c = s[i];
    *skip = 1;
    if (dyn_invidx_class[c] < 2)
        return dyn_invidx_class[c];
    if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF && s[i + 1] != 0xAA && s[i + 1] != 0xB5 && s[i + 1] != 0xBA) {
        *skip = 2;
        return 0;
    }
    if (c == 0xE2 && i + 2 < n && (s[i + 1] == 0x80 || s[i + 1] == 0x81)) {
        *skip = 3;
        return 0;
    }
    if (c == 0xE3 && i + 2 < n && s[i + 1] == 0x80) {
        *skip = 3;
        return 0;
    }
    if (c == 0xEF && i + 2 < n && ((s[i + 1] == 0xBC && s[i + 2] >= 0x81 && s[i + 2] <= 0x8F) || (s[i + 1] == 0xBD && s[i + 2] >= 0x9B))) {
        *skip = 3;
        return 0;
    }
    return 1;
}

int dyn_invidx_tokenize(const char* str, size_t n, ii_tok_fn fn, void* ud)
{
    const unsigned char* s = (const unsigned char*)str;
    char buf[DYN_INVIDX_TOK_MAX];
    size_t i = 0, skip;
    while (i < n) {
        size_t len = 0;
        int trunc = 0;
        while (i < n && !dyn_invidx_word_byte(s, n, i, &skip))
            i += skip;
        while (i < n && dyn_invidx_word_byte(s, n, i, &skip)) {
            unsigned char c = s[i];
            if (len < DYN_INVIDX_TOK_MAX)
                buf[len++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            else
                trunc = 1;
            i++;
        }
        if (trunc) {
            size_t k = len;
            while (k > 0 && ((unsigned char)buf[k - 1] & 0xC0) == 0x80)
                k--;
            if (k > 0) {
                unsigned char lead = (unsigned char)buf[k - 1];
                size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                    : lead >= 0xC0                            ? 2
                                                              : 1;
                if (len - (k - 1) < need)
                    len = k - 1;
            }
        }
        if (len) {
            int rc = fn(ud, buf, len);
            if (rc)
                return rc;
        }
    }
    return 0;
}

static int dyn_invidx_is_stop(const dyn_invidx_t* x, const char* t, size_t n)
{
    return x->nstop && dyn_ds_smap_find(&x->stop, t, n, dyn_ds_strhash(t, n)) != NULL;
}

int64_t dyn_invidx_term_get(dyn_invidx_t* x, const char* t, size_t n, int create)
{
    uint64_t h = dyn_ds_strhash(t, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&x->tidx, t, n, h);
    dyn_invidx_term* tm;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (x->nterms == x->tcap) {
        uint32_t nc = x->tcap ? x->tcap * 2 : 64;
        dyn_invidx_term* nt;
        if (nc > DYN_DS_MAX_ITEMS)
            return -2;
        nt = (dyn_invidx_term*)DYN_DS_REALLOC(x->terms, sizeof(dyn_invidx_term) * nc);
        if (!nt)
            return -2;
        x->terms = nt;
        x->tcap = nc;
    }
    tm = &x->terms[x->nterms];
    memset(tm, 0, sizeof(*tm));
    tm->s = (char*)dyn_ds_bytes_dup(t, n);
    if (!tm->s)
        return -2;
    tm->len = (uint32_t)n;
    if (!dyn_ds_smap_insert(&x->tidx, tm->s, n, h, x->nterms)) {
        DYN_DS_FREE(tm->s);
        return -2;
    }
    return (int64_t)x->nterms++;
}

int dyn_invidx_tscratch(dyn_invidx_t* x)
{
    if (x->tscap < x->nterms) {
        uint32_t nc = x->tcap;
        uint32_t* a = (uint32_t*)DYN_DS_REALLOC(x->tstamp, sizeof(uint32_t) * nc);
        uint32_t* b;
        if (!a)
            return -1;
        x->tstamp = a;
        b = (uint32_t*)DYN_DS_REALLOC(x->tcount, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->tcount = b;
        memset(x->tstamp + x->tscap, 0, sizeof(uint32_t) * (nc - x->tscap));
        x->tscap = nc;
    }
    return 0;
}

static int dyn_invidx_add_tok(void* ud, const char* t, size_t n)
{
    dyn_invidx_add_ctx* c = (dyn_invidx_add_ctx*)ud;
    dyn_invidx_t* x = c->x;
    int64_t ti;
    if (dyn_invidx_is_stop(x, t, n))
        return 0;
    ti = dyn_invidx_term_get(x, t, n, 1);
    if (ti < 0 || dyn_invidx_tscratch(x) < 0) {
        c->oom = 1;
        return 1;
    }
    c->len++;
    if (x->tstamp[ti] == x->tepoch) {
        c->list[x->tcount[ti]].tf++;
        return 0;
    }
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 16;
        dyn_invidx_dt* nl = (dyn_invidx_dt*)DYN_DS_REALLOC(c->list, sizeof(dyn_invidx_dt) * nc);
        if (!nl) {
            c->oom = 1;
            return 1;
        }
        c->list = nl;
        c->cap = nc;
    }
    x->tstamp[ti] = x->tepoch;
    x->tcount[ti] = c->n;
    c->list[c->n].term = (uint32_t)ti;
    c->list[c->n].tf = 1;
    c->n++;
    return 0;
}

void dyn_invidx_bump_tepoch(dyn_invidx_t* x)
{
    if (++x->tepoch == 0) {
        if (x->tstamp)
            memset(x->tstamp, 0, sizeof(uint32_t) * x->tscap);
        x->tepoch = 1;
    }
}

static int dyn_invidx_post_push(dyn_invidx_term* tm, uint32_t slot, uint32_t tf)
{
    if (tm->n == tm->cap) {
        uint32_t nc = tm->cap ? tm->cap * 2 : 4;
        dyn_invidx_post* np = (dyn_invidx_post*)DYN_DS_REALLOC(tm->p, sizeof(dyn_invidx_post) * nc);
        if (!np)
            return -1;
        tm->p = np;
        tm->cap = nc;
    }
    tm->p[tm->n].doc = slot;
    tm->p[tm->n].tf = tf;
    tm->n++;
    return 0;
}

static void dyn_invidx_remove_slot(dyn_invidx_t* x, uint32_t slot)
{
    dyn_invidx_doc* d = &x->docs[slot];
    uint32_t i, j;
    for (i = 0; i < d->nt; i++) {
        dyn_invidx_term* tm = &x->terms[d->t[i].term];
        for (j = 0; j < tm->n; j++)
            if (tm->p[j].doc == slot) {
                tm->p[j] = tm->p[--tm->n];
                break;
            }
    }
    x->total_len -= d->len;
    DYN_DS_FREE(d->t);
    d->t = NULL;
    d->nt = 0;
    d->alive = 0;
    x->live--;
    if (x->nfree == x->fcap) {
        uint32_t nc = x->fcap ? x->fcap * 2 : 16;
        uint32_t* nf = (uint32_t*)DYN_DS_REALLOC(x->freel, sizeof(uint32_t) * nc);
        if (!nf)
            return;
        x->freel = nf;
        x->fcap = nc;
    }
    x->freel[x->nfree++] = slot;
}

int dyn_invidx_remove(dyn_invidx_t* x, uint32_t id)
{
    dyn_ds_ient* e = dyn_ds_imap_find(&x->didx, id);
    uint32_t slot;
    if (!e)
        return 0;
    slot = e->v;
    dyn_ds_imap_remove(&x->didx, e);
    dyn_invidx_remove_slot(x, slot);
    return 1;
}

static int dyn_invidx_slot_alloc(dyn_invidx_t* x, uint32_t* out)
{
    if (x->nfree) {
        *out = x->freel[--x->nfree];
        return 0;
    }
    if (x->nslots == x->dcap) {
        uint32_t nc = x->dcap ? x->dcap * 2 : 64;
        dyn_invidx_doc* nd;
        if (nc > DYN_DS_MAX_ITEMS)
            return -1;
        nd = (dyn_invidx_doc*)DYN_DS_REALLOC(x->docs, sizeof(dyn_invidx_doc) * nc);
        if (!nd)
            return -1;
        x->docs = nd;
        x->dcap = nc;
    }
    *out = x->nslots++;
    return 0;
}

int dyn_invidx_install(dyn_invidx_t* x, uint32_t id, dyn_invidx_dt* list, uint32_t nt, uint32_t len)
{
    uint32_t slot, i;
    dyn_invidx_doc* d;
    if (dyn_invidx_slot_alloc(x, &slot) < 0)
        return -1;
    if (!dyn_ds_imap_put(&x->didx, id, slot)) {
        if (x->nfree < x->fcap)
            x->freel[x->nfree++] = slot;
        else if (slot == x->nslots - 1)
            x->nslots--;
        return -1;
    }
    d = &x->docs[slot];
    d->id = id;
    d->len = len;
    d->nt = nt;
    d->t = list;
    d->alive = 1;
    x->live++;
    x->total_len += len;
    for (i = 0; i < nt; i++)
        if (dyn_invidx_post_push(&x->terms[list[i].term], slot, list[i].tf) < 0) {
            uint32_t j;
            for (j = 0; j < i; j++)
                x->terms[list[j].term].n--;
            d->nt = 0;
            d->t = NULL;
            if (dyn_ds_imap_find(&x->didx, id))
                dyn_ds_imap_remove(&x->didx, dyn_ds_imap_find(&x->didx, id));
            d->alive = 0;
            x->live--;
            x->total_len -= len;
            if (x->nfree < x->fcap)
                x->freel[x->nfree++] = slot;
            return -1;
        }
    return 0;
}

int dyn_invidx_add(dyn_invidx_t* x, uint32_t id, const char* text, size_t n)
{
    dyn_invidx_add_ctx c;
    memset(&c, 0, sizeof(c));
    c.x = x;
    dyn_invidx_remove(x, id);
    dyn_invidx_bump_tepoch(x);
    dyn_invidx_tokenize(text, n, dyn_invidx_add_tok, &c);
    if (c.oom || dyn_invidx_install(x, id, c.list, c.n, c.len) < 0) {
        DYN_DS_FREE(c.list);
        return -1;
    }
    return 0;
}

void dyn_invidx_free(dyn_invidx_t* x)
{
    uint32_t i;
    for (i = 0; i < x->nterms; i++) {
        DYN_DS_FREE(x->terms[i].s);
        DYN_DS_FREE(x->terms[i].p);
    }
    DYN_DS_FREE(x->terms);
    for (i = 0; i < x->nslots; i++)
        DYN_DS_FREE(x->docs[i].t);
    DYN_DS_FREE(x->docs);
    DYN_DS_FREE(x->freel);
    dyn_ds_smap_free(&x->tidx);
    dyn_ds_imap_free(&x->didx);
    for (i = 0; i < x->nstop; i++)
        DYN_DS_FREE(x->stopw[i]);
    DYN_DS_FREE(x->stopw);
    dyn_ds_smap_free(&x->stop);
    DYN_DS_FREE(x->acc);
    DYN_DS_FREE(x->stamp);
    DYN_DS_FREE(x->mask);
    DYN_DS_FREE(x->touched);
    DYN_DS_FREE(x->tstamp);
    DYN_DS_FREE(x->tcount);
}

int dyn_invidx_add_stop(dyn_invidx_t* x, const char* t, size_t n)
{
    char** ns;
    char* s;
    if (dyn_ds_smap_find(&x->stop, t, n, dyn_ds_strhash(t, n)))
        return 0;
    ns = (char**)DYN_DS_REALLOC(x->stopw, sizeof(char*) * (x->nstop + 1));
    if (!ns)
        return -1;
    x->stopw = ns;
    s = (char*)dyn_ds_bytes_dup(t, n);
    if (!s)
        return -1;
    if (!dyn_ds_smap_insert(&x->stop, s, n, dyn_ds_strhash(t, n), x->nstop)) {
        DYN_DS_FREE(s);
        return -1;
    }
    x->stopw[x->nstop++] = s;
    return 0;
}

int dyn_invidx_stop_tok(void* ud, const char* t, size_t n)
{
    return dyn_invidx_add_stop((dyn_invidx_t*)ud, t, n) < 0 ? -1 : 0;
}

static int dyn_invidx_q_tok(void* ud, const char* t, size_t n)
{
    dyn_invidx_q_ctx* q = (dyn_invidx_q_ctx*)ud;
    uint32_t i;
    if (dyn_invidx_is_stop(q->x, t, n))
        return 0;
    for (i = 0; i < q->n; i++)
        if (q->lens[i] == n && memcmp(q->toks[i], t, n) == 0)
            return 0;
    if (q->n == DYN_INVIDX_MAX_GROUPS)
        return 1;
    memcpy(q->toks[q->n], t, n);
    q->lens[q->n++] = (uint32_t)n;
    return 0;
}

static int dyn_invidx_scratch(dyn_invidx_t* x)
{
    if (x->scap < x->nslots) {
        uint32_t nc = x->dcap;
        double* a = (double*)DYN_DS_REALLOC(x->acc, sizeof(double) * nc);
        uint32_t *b, *c, *d;
        if (!a)
            return -1;
        x->acc = a;
        b = (uint32_t*)DYN_DS_REALLOC(x->stamp, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->stamp = b;
        c = (uint32_t*)DYN_DS_REALLOC(x->mask, sizeof(uint32_t) * nc);
        if (!c)
            return -1;
        x->mask = c;
        d = (uint32_t*)DYN_DS_REALLOC(x->touched, sizeof(uint32_t) * nc);
        if (!d)
            return -1;
        x->touched = d;
        memset(x->stamp + x->scap, 0, sizeof(uint32_t) * (nc - x->scap));
        x->scap = nc;
    }
    if (++x->epoch == 0) {
        memset(x->stamp, 0, sizeof(uint32_t) * x->scap);
        x->epoch = 1;
    }
    return 0;
}

static int dyn_invidx_hit_worse(const dyn_invidx_hit* a, const dyn_invidx_hit* b)
{
    return a->s < b->s || (a->s == b->s && a->id > b->id);
}

static int dyn_invidx_hit_cmp(const void* a, const void* b)
{
    const dyn_invidx_hit *x = (const dyn_invidx_hit*)a, *y = (const dyn_invidx_hit*)b;
    if (x->s != y->s)
        return x->s > y->s ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

static void dyn_invidx_heap_push(dyn_invidx_hit* h, uint32_t* n, uint32_t k, dyn_invidx_hit v)
{
    uint32_t i;
    if (*n < k) {
        i = (*n)++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!dyn_invidx_hit_worse(&v, &h[p]))
                break;
            h[i] = h[p];
            i = p;
        }
        h[i] = v;
        return;
    }
    if (!dyn_invidx_hit_worse(&h[0], &v))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const dyn_invidx_hit* mv = &v;
        if (l < *n && dyn_invidx_hit_worse(&h[l], mv)) {
            m = l;
            mv = &h[l];
        }
        if (r < *n && dyn_invidx_hit_worse(&h[r], mv))
            m = r;
        if (m == i)
            break;
        h[i] = h[m];
        i = m;
    }
    h[i] = v;
}

static void dyn_invidx_score_term(dyn_invidx_t* x, uint32_t ti, uint32_t g, double avg, uint32_t* ntouched)
{
    dyn_invidx_term* tm = &x->terms[ti];
    double N = x->live, df = tm->n, idf, k1 = x->k1, b = x->b;
    uint32_t j;
    if (!tm->n)
        return;
    idf = log(1 + (N - df + 0.5) / (df + 0.5));
    for (j = 0; j < tm->n; j++) {
        uint32_t slot = tm->p[j].doc;
        double tf = tm->p[j].tf, dl = x->docs[slot].len;
        double s = idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * (avg > 0 ? dl / avg : 0)));
        if (x->stamp[slot] != x->epoch) {
            x->stamp[slot] = x->epoch;
            x->acc[slot] = 0;
            x->mask[slot] = 0;
            x->touched[(*ntouched)++] = slot;
        }
        x->acc[slot] += s;
        x->mask[slot] |= 1u << g;
    }
}

int dyn_invidx_search(dyn_invidx_t* x, const char* q, size_t qn, int and_mode, int prefix, uint32_t limit, dyn_invidx_hit** out, uint32_t* nout)
{
    dyn_invidx_q_ctx* qc;
    uint32_t g, ntouched = 0, i, nh = 0, full;
    double avg;
    dyn_invidx_hit* h;
    *out = NULL;
    *nout = 0;
    qc = (dyn_invidx_q_ctx*)DYN_DS_MALLOC(sizeof(*qc));
    if (!qc)
        return -1;
    qc->x = x;
    qc->n = 0;
    dyn_invidx_tokenize(q, qn, dyn_invidx_q_tok, qc);
    if (!qc->n || !x->live || !limit) {
        DYN_DS_FREE(qc);
        return 0;
    }
    if (dyn_invidx_scratch(x) < 0) {
        DYN_DS_FREE(qc);
        return -1;
    }
    avg = (double)x->total_len / x->live;
    for (g = 0; g < qc->n; g++) {
        if (prefix && g == qc->n - 1) {
            for (i = 0; i < x->nterms; i++)
                if (x->terms[i].len >= qc->lens[g] && memcmp(x->terms[i].s, qc->toks[g], qc->lens[g]) == 0)
                    dyn_invidx_score_term(x, i, g, avg, &ntouched);
        } else {
            int64_t ti = dyn_invidx_term_get(x, qc->toks[g], qc->lens[g], 0);
            if (ti >= 0)
                dyn_invidx_score_term(x, (uint32_t)ti, g, avg, &ntouched);
        }
    }
    full = qc->n == 32 ? 0xFFFFFFFFu : (1u << qc->n) - 1;
    DYN_DS_FREE(qc);
    if (limit > ntouched)
        limit = ntouched;
    h = (dyn_invidx_hit*)DYN_DS_MALLOC(sizeof(dyn_invidx_hit) * ((size_t)limit + 1));
    if (!h)
        return -1;
    for (i = 0; i < ntouched && limit; i++) {
        uint32_t slot = x->touched[i];
        dyn_invidx_hit v;
        if (and_mode && x->mask[slot] != full)
            continue;
        v.s = x->acc[slot];
        v.id = x->docs[slot].id;
        dyn_invidx_heap_push(h, &nh, limit, v);
    }
    qsort(h, nh, sizeof(dyn_invidx_hit), dyn_invidx_hit_cmp);
    *out = h;
    *nout = nh;
    return 0;
}

void dyn_invidx_init(dyn_invidx_t* x, double k1, double b)
{
    memset(x, 0, sizeof(*x));
    x->k1 = k1;
    x->b = b;
    x->tepoch = 1;
    dyn_ds_imap_init(&x->didx);
}
