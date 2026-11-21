#include "ds-internal.h"
#include "ds-cuckoo-filter.h"

static uint64_t dyn_ds_strhash_fixed(const char* p, size_t n, uint64_t salt)
{
    return dyn_ds_xxh64((const uint8_t*)p, n, DYN_DS_FIXED_SEED ^ salt);
}

static uint32_t dyn_cuckoo_alt(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    return (i ^ (uint32_t)(((uint64_t)fp * 0x5bd1e995u) >> 0)) & c->mask;
}

void dyn_cuckoo_hash(const dyn_cuckoo_t* c, const char* k, size_t n, uint32_t* i, uint16_t* fp)
{
    uint64_t h = dyn_ds_strhash_fixed(k, n, 0xCF);
    uint16_t f = (uint16_t)(h >> 48);
    if (!f)
        f = 1;
    *fp = f;
    *i = (uint32_t)h & c->mask;
}

static int dyn_cuckoo_put(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (!c->t[i * DYN_CUCKOO_SLOTS + s]) {
            c->t[i * DYN_CUCKOO_SLOTS + s] = fp;
            return 1;
        }
    return 0;
}

static int dyn_cuckoo_has_in(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (c->t[i * DYN_CUCKOO_SLOTS + s] == fp)
            return 1;
    return 0;
}

static int dyn_cuckoo_del_in(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < DYN_CUCKOO_SLOTS; s++)
        if (c->t[i * DYN_CUCKOO_SLOTS + s] == fp) {
            c->t[i * DYN_CUCKOO_SLOTS + s] = 0;
            return 1;
        }
    return 0;
}

int dyn_cuckoo_insert(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp), k;
    if (c->has_victim)
        return 0;
    if (dyn_cuckoo_put(c, i, fp) || dyn_cuckoo_put(c, i2, fp)) {
        c->count++;
        return 1;
    }
    i = (ds_splitmix64(&c->rng) & 1) ? i : i2;
    for (k = 0; k < DYN_CUCKOO_MAX_KICKS; k++) {
        uint32_t s = (uint32_t)(ds_splitmix64(&c->rng) % DYN_CUCKOO_SLOTS);
        uint16_t old = c->t[i * DYN_CUCKOO_SLOTS + s];
        c->t[i * DYN_CUCKOO_SLOTS + s] = fp;
        fp = old;
        i = dyn_cuckoo_alt(c, i, fp);
        if (dyn_cuckoo_put(c, i, fp)) {
            c->count++;
            return 1;
        }
    }
    c->victim_fp = fp;
    c->victim_i = i;
    c->has_victim = 1;
    c->count++;
    return 1;
}

int dyn_cuckoo_contains(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp);
    if (dyn_cuckoo_has_in(c, i, fp) || dyn_cuckoo_has_in(c, i2, fp))
        return 1;
    return c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2);
}

int dyn_cuckoo_delete(dyn_cuckoo_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = dyn_cuckoo_alt(c, i, fp);
    if (dyn_cuckoo_del_in(c, i, fp) || dyn_cuckoo_del_in(c, i2, fp)) {
        c->count--;
        if (c->has_victim) {
            uint16_t vf = c->victim_fp;
            uint32_t vi = c->victim_i;
            c->has_victim = 0;
            c->count--;
            dyn_cuckoo_insert(c, vi, vf);
        }
        return 1;
    }
    if (c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2)) {
        c->has_victim = 0;
        c->count--;
        return 1;
    }
    return 0;
}

int dyn_cuckoo_init(dyn_cuckoo_t* c, uint32_t nb)
{
    memset(c, 0, sizeof(*c));
    c->nb = nb;
    c->mask = nb - 1;
    c->rng = DYN_DS_FIXED_SEED;
    c->t = (uint16_t*)DYN_DS_CALLOC((size_t)nb * DYN_CUCKOO_SLOTS, sizeof(uint16_t));
    return c->t ? 0 : -1;
}

void dyn_cuckoo_free(dyn_cuckoo_t* c)
{
    DYN_DS_FREE(c->t);
    c->t = NULL;
}
