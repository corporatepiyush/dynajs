#include <stdlib.h>
#include "dd.h"

#define KM_EMPTY 0xFFFFFFFFu
#define KM_TOMB 0xFFFFFFFEu

void dd_kmap_init(dd_kmap* m)
{
    memset(m, 0, sizeof *m);
}

void dd_kmap_free(dd_kmap* m)
{
    free(m->slots);
    free(m->pool);
    memset(m, 0, sizeof *m);
}

const uint8_t* dd_kmap_key(const dd_kmap* m, uint32_t off)
{
    return m->pool + off;
}

static int km_rehash(dd_kmap* m, uint32_t ncap)
{
    dd_kslot* ns = malloc((size_t)ncap * sizeof *ns);
    uint8_t* np = NULL;
    size_t pn = 0, pcap = 0;
    uint32_t i;
    int compact = m->pool_dead > m->pool_n / 2;
    if (!ns)
        return -1;
    for (i = 0; i < ncap; i++)
        ns[i].off = KM_EMPTY;
    if (compact) {
        pcap = m->pool_n - m->pool_dead + 64;
        np = malloc(pcap);
        if (!np) {
            free(ns);
            return -1;
        }
    }
    for (i = 0; i < m->cap; i++) {
        dd_kslot s = m->slots[i];
        uint32_t j;
        if (s.off >= KM_TOMB)
            continue;
        if (compact) {
            memcpy(np + pn, m->pool + s.off, s.len);
            s.off = (uint32_t)pn;
            pn += s.len;
        }
        j = (uint32_t)s.hash & (ncap - 1);
        while (ns[j].off != KM_EMPTY)
            j = (j + 1) & (ncap - 1);
        ns[j] = s;
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    m->tombs = 0;
    if (compact) {
        free(m->pool);
        m->pool = np;
        m->pool_n = pn;
        m->pool_cap = pcap;
        m->pool_dead = 0;
    }
    return 0;
}

int dd_kmap_reserve(dd_kmap* m, uint32_t n)
{
    uint32_t nc = m->cap ? m->cap : 16;
    if ((uint64_t)n * 4 >= (uint64_t)1 << 31)
        return -1;
    while ((n + 1) * 4 >= nc)
        nc *= 2;
    return nc > m->cap ? km_rehash(m, nc) : 0;
}

int dd_kmap_get(const dd_kmap* m, const uint8_t* k, size_t n, uint32_t* val)
{
    uint64_t h;
    uint32_t j;
    if (!m->cap)
        return 0;
    h = dd_hash_bytes(k, n);
    j = (uint32_t)h & (m->cap - 1);
    for (;;) {
        const dd_kslot* s = &m->slots[j];
        if (s->off == KM_EMPTY)
            return 0;
        if (s->off != KM_TOMB && s->hash == h && s->len == n && memcmp(m->pool + s->off, k, n) == 0) {
            if (val)
                *val = s->val;
            return 1;
        }
        j = (j + 1) & (m->cap - 1);
    }
}

int dd_kmap_put(dd_kmap* m, const uint8_t* k, size_t n, uint32_t val, uint32_t* existing)
{
    return dd_kmap_put_h(m, k, n, dd_hash_bytes(k, n), val, existing);
}

int dd_kmap_put_h(dd_kmap* m, const uint8_t* k, size_t n, uint64_t h, uint32_t val, uint32_t* existing)
{
    uint32_t j, tomb = KM_EMPTY;
    if (n > 0x7FFFFFFF)
        return -1;
    if ((m->count + m->tombs + 1) * 2 >= m->cap) {
        uint32_t nc = m->cap ? m->cap : 16;
        while ((m->count + 1) * 4 >= nc)
            nc *= 2;
        if (km_rehash(m, nc) < 0)
            return -1;
    }
    j = (uint32_t)h & (m->cap - 1);
    for (;;) {
        dd_kslot* s = &m->slots[j];
        if (s->off == KM_EMPTY)
            break;
        if (s->off == KM_TOMB) {
            if (tomb == KM_EMPTY)
                tomb = j;
        } else if (s->hash == h && s->len == n && memcmp(m->pool + s->off, k, n) == 0) {
            if (existing)
                *existing = s->val;
            return 1;
        }
        j = (j + 1) & (m->cap - 1);
    }
    if (m->pool_n + n > m->pool_cap) {
        size_t nc = m->pool_cap ? m->pool_cap : 256;
        uint8_t* np;
        while (nc < m->pool_n + n)
            nc *= 2;
        if (nc > 0xFFFFFFF0u)
            return -1;
        np = realloc(m->pool, nc);
        if (!np)
            return -1;
        m->pool = np;
        m->pool_cap = nc;
    }
    if (tomb != KM_EMPTY) {
        j = tomb;
        m->tombs--;
    }
    memcpy(m->pool + m->pool_n, k, n);
    m->slots[j].hash = h;
    m->slots[j].off = (uint32_t)m->pool_n;
    m->slots[j].len = (uint32_t)n;
    m->slots[j].val = val;
    m->pool_n += n;
    m->count++;
    return 0;
}

int dd_kmap_del(dd_kmap* m, const uint8_t* k, size_t n)
{
    uint64_t h;
    uint32_t j;
    if (!m->cap)
        return 0;
    h = dd_hash_bytes(k, n);
    j = (uint32_t)h & (m->cap - 1);
    for (;;) {
        dd_kslot* s = &m->slots[j];
        if (s->off == KM_EMPTY)
            return 0;
        if (s->off != KM_TOMB && s->hash == h && s->len == n && memcmp(m->pool + s->off, k, n) == 0) {
            m->pool_dead += s->len;
            s->off = KM_TOMB;
            m->count--;
            m->tombs++;
            return 1;
        }
        j = (j + 1) & (m->cap - 1);
    }
}
