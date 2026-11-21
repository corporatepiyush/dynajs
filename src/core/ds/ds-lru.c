#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-lru.h"

static uint64_t ds_lru_hash(const char* s, size_t n)
{
    uint64_t h = 14695981039346656037ULL ^ ds_str_seed();
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int dyn_lru_init(dyn_lru_t* L, uint32_t cap, dyn_cell_free_fn release, void* ud)
{
    memset(L, 0, sizeof(*L));
    L->n_buckets = 16;
    L->cap = cap;
    L->release = release;
    L->ud = ud;
    L->buckets = (dyn_lru_node_t**)DYN_DS_CALLOC(L->n_buckets, sizeof(*L->buckets));
    return L->buckets ? 0 : -1;
}

void dyn_lru_node_free(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (L->release)
        L->release(L->ud, &n->value);
    DYN_DS_FREE(n);
}

dyn_cell_t dyn_lru_node_take(dyn_lru_node_t* n)
{
    dyn_cell_t v = n->value;
    DYN_DS_FREE(n);
    return v;
}

void dyn_lru_free(dyn_lru_t* L)
{
    dyn_lru_node_t *n, *next;
    for (n = L->head; n; n = next) {
        next = n->next;
        dyn_lru_node_free(L, n);
    }
    DYN_DS_FREE(L->buckets);
    L->buckets = NULL;
    L->head = L->tail = NULL;
    L->count = 0;
}

uint64_t dyn_lru_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t dyn_lru_expiry(uint64_t ttl)
{
    uint64_t now;
    if (!ttl)
        return 0;
    now = dyn_lru_now_ms();
    if (ttl > (uint64_t)INT64_MAX - now)
        return (uint64_t)INT64_MAX;
    return now + ttl;
}

int dyn_lru_expired(const dyn_lru_node_t* n, uint64_t now)
{
    return n->expires != 0 && now >= n->expires;
}

static void ds_lru_rehash(dyn_lru_t* L, uint32_t nb)
{
    dyn_lru_node_t** nbk = (dyn_lru_node_t**)DYN_DS_CALLOC(nb, sizeof(*nbk));
    dyn_lru_node_t* n;
    if (!nbk)
        return;
    for (n = L->head; n; n = n->next) {
        uint32_t b = (uint32_t)(n->hash & (nb - 1));
        n->hnext = nbk[b];
        nbk[b] = n;
    }
    DYN_DS_FREE(L->buckets);
    L->buckets = nbk;
    L->n_buckets = nb;
}

static dyn_lru_node_t* ds_lru_find_h(const dyn_lru_t* L, const char* key, size_t klen, uint64_t h)
{
    dyn_lru_node_t* n;
    for (n = L->buckets[h & (L->n_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && n->klen == klen && !memcmp(n->key, key, klen))
            return n;
    return NULL;
}

dyn_lru_node_t* dyn_lru_find(const dyn_lru_t* L, const char* key, size_t klen)
{
    return ds_lru_find_h(L, key, klen, ds_lru_hash(key, klen));
}

static void ds_lru_unlink(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        L->head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        L->tail = n->prev;
}

static void ds_lru_push_head(dyn_lru_t* L, dyn_lru_node_t* n)
{
    n->prev = NULL;
    n->next = L->head;
    if (L->head)
        L->head->prev = n;
    else
        L->tail = n;
    L->head = n;
}

void dyn_lru_touch(dyn_lru_t* L, dyn_lru_node_t* n)
{
    if (L->head != n) {
        ds_lru_unlink(L, n);
        ds_lru_push_head(L, n);
    }
}

void dyn_lru_detach(dyn_lru_t* L, dyn_lru_node_t* n)
{
    dyn_lru_node_t** pp = &L->buckets[n->hash & (L->n_buckets - 1)];
    ds_lru_unlink(L, n);
    while (*pp && *pp != n)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = n->hnext;
    L->count--;
}

int dyn_lru_set(dyn_lru_t* L, const char* key, size_t klen, const dyn_cell_t* v, uint64_t ttl, dyn_lru_node_t** evicted)
{
    uint64_t h = ds_lru_hash(key, klen);
    dyn_lru_node_t* n = ds_lru_find_h(L, key, klen, h);
    uint32_t b;
    *evicted = NULL;
    if (n) {
        if (L->release)
            L->release(L->ud, &n->value);
        n->value = *v;
        n->expires = dyn_lru_expiry(ttl);
        dyn_lru_touch(L, n);
        return 0;
    }
    n = (dyn_lru_node_t*)DYN_DS_MALLOC(sizeof(*n) + klen + 1);
    if (!n)
        return -1;
    memcpy(n->key, key, klen);
    n->key[klen] = '\0';
    n->klen = klen;
    n->hash = h;
    n->expires = dyn_lru_expiry(ttl);
    n->value = *v;
    if (L->count + 1 > L->n_buckets && L->n_buckets < DYN_LRU_MAX_BUCKETS)
        ds_lru_rehash(L, L->n_buckets * 2);
    b = (uint32_t)(h & (L->n_buckets - 1));
    n->hnext = L->buckets[b];
    L->buckets[b] = n;
    ds_lru_push_head(L, n);
    L->count++;
    if (L->count > L->cap) {
        dyn_lru_node_t* victim = L->tail;
        dyn_lru_detach(L, victim);
        L->evictions++;
        *evicted = victim;
    }
    return 0;
}
