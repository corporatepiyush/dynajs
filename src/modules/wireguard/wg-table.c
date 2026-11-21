#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"

#include <stdlib.h>
#include <string.h>

static uint32_t idx_hash(uint32_t k)
{
    k ^= k >> 16;
    k *= 0x7feb352du;
    k ^= k >> 15;
    return k;
}

int wg_idx_init(wg_idx_t* t, uint32_t cap)
{
    uint32_t c = 16;
    while (c < cap && c < (1u << 30))
        c <<= 1;
    t->s = (wg_idx_slot_t*)calloc(c, sizeof *t->s);
    t->mask = c - 1;
    t->n = 0;
    return t->s ? 0 : -1;
}

void wg_idx_free(wg_idx_t* t)
{
    free(t->s);
    memset(t, 0, sizeof *t);
}

static int idx_grow(wg_idx_t* t)
{
    wg_idx_t g;
    uint32_t i;
    if (t->mask >= (1u << 30) || wg_idx_init(&g, (t->mask + 1) * 2) < 0)
        return -1;
    for (i = 0; i <= t->mask; i++)
        if (t->s[i].used) {
            uint32_t j = idx_hash(t->s[i].key) & g.mask;
            while (g.s[j].used)
                j = (j + 1) & g.mask;
            g.s[j] = t->s[i];
            g.n++;
        }
    free(t->s);
    *t = g;
    return 0;
}

static wg_idx_slot_t* idx_find(const wg_idx_t* t, uint32_t key)
{
    uint32_t j = idx_hash(key) & t->mask;
    while (t->s[j].used) {
        if (t->s[j].key == key)
            return &t->s[j];
        j = (j + 1) & t->mask;
    }
    return NULL;
}

int wg_idx_put(wg_idx_t* t, uint32_t key, void* val)
{
    uint32_t j;
    if (idx_find(t, key))
        return -1;
    if ((uint64_t)(t->n + 1) * 4 > (uint64_t)(t->mask + 1) * 3 && idx_grow(t) < 0)
        return -1;
    j = idx_hash(key) & t->mask;
    while (t->s[j].used)
        j = (j + 1) & t->mask;
    t->s[j].key = key;
    t->s[j].used = 1;
    t->s[j].val = val;
    t->n++;
    return 0;
}

int wg_idx_alloc(wg_idx_t* t, void* val, uint32_t* key)
{
    int tries;
    for (tries = 0; tries < 64; tries++) {
        uint8_t r[4];
        if (wg_random(r, sizeof r) < 0)
            return -1;
        *key = wg_le32(r);
        if (wg_idx_put(t, *key, val) == 0)
            return 0;
    }
    return -1;
}

void* wg_idx_get(const wg_idx_t* t, uint32_t key)
{
    wg_idx_slot_t* s = idx_find(t, key);
    return s ? s->val : NULL;
}

int wg_idx_del(wg_idx_t* t, uint32_t key)
{
    wg_idx_slot_t* s = idx_find(t, key);
    uint32_t i, j;
    if (!s)
        return -1;
    i = (uint32_t)(s - t->s);
    j = i;
    for (;;) {
        uint32_t h;
        j = (j + 1) & t->mask;
        if (!t->s[j].used)
            break;
        h = idx_hash(t->s[j].key) & t->mask;
        if (((j - h) & t->mask) >= ((j - i) & t->mask)) {
            t->s[i] = t->s[j];
            i = j;
        }
    }
    memset(&t->s[i], 0, sizeof t->s[i]);
    t->n--;
    return 0;
}

static uint32_t pub_hash(const wg_pubtab_t* t, const uint8_t pub[WG_KEY])
{
    uint64_t z = wg_le64(pub) ^ wg_le64(pub + 8) ^ t->seed;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (uint32_t)(z ^ (z >> 31));
}

int wg_pubtab_init(wg_pubtab_t* t, const uint8_t* (*pub_of)(const void* v))
{
    uint8_t r[8];
    memset(t, 0, sizeof *t);
    t->pub_of = pub_of;
    if (wg_random(r, sizeof r) < 0)
        return -1;
    t->seed = wg_le64(r);
    t->s = (void**)calloc(16, sizeof *t->s);
    t->mask = 15;
    return t->s ? 0 : -1;
}

void wg_pubtab_free(wg_pubtab_t* t)
{
    free(t->s);
    memset(t, 0, sizeof *t);
}

static uint32_t pub_slot(const wg_pubtab_t* t, const uint8_t pub[WG_KEY], int* found)
{
    uint32_t j = pub_hash(t, pub) & t->mask;
    *found = 0;
    while (t->s[j]) {
        if (memcmp(t->pub_of(t->s[j]), pub, WG_KEY) == 0) {
            *found = 1;
            break;
        }
        j = (j + 1) & t->mask;
    }
    return j;
}

int wg_pubtab_put(wg_pubtab_t* t, void* v)
{
    int found;
    uint32_t j;
    if ((uint64_t)(t->n + 1) * 4 > (uint64_t)(t->mask + 1) * 3) {
        void** old = t->s;
        uint32_t om = t->mask, i;
        if (om >= (1u << 30))
            return -1;
        t->s = (void**)calloc((size_t)(om + 1) * 2, sizeof *t->s);
        if (!t->s) {
            t->s = old;
            return -1;
        }
        t->mask = om * 2 + 1;
        for (i = 0; i <= om; i++)
            if (old[i])
                t->s[pub_slot(t, t->pub_of(old[i]), &found)] = old[i];
        free(old);
    }
    j = pub_slot(t, t->pub_of(v), &found);
    if (found)
        return -1;
    t->s[j] = v;
    t->n++;
    return 0;
}

void* wg_pubtab_get(const wg_pubtab_t* t, const uint8_t pub[WG_KEY])
{
    int found;
    uint32_t j = pub_slot(t, pub, &found);
    return found ? t->s[j] : NULL;
}

int wg_pubtab_del(wg_pubtab_t* t, const uint8_t pub[WG_KEY])
{
    int found;
    uint32_t i = pub_slot(t, pub, &found), j = i;
    if (!found)
        return -1;
    for (;;) {
        uint32_t h;
        j = (j + 1) & t->mask;
        if (!t->s[j])
            break;
        h = pub_hash(t, t->pub_of(t->s[j])) & t->mask;
        if (((j - h) & t->mask) >= ((j - i) & t->mask)) {
            t->s[i] = t->s[j];
            i = j;
        }
    }
    t->s[i] = NULL;
    t->n--;
    return 0;
}

static unsigned bit_at(const uint8_t* k, unsigned i)
{
    return (k[i >> 3] >> (7 - (i & 7))) & 1u;
}

static unsigned common_bits(const uint8_t* a, const uint8_t* b, unsigned max)
{
    unsigned i = 0;
    while (i + 8 <= max && a[i >> 3] == b[i >> 3])
        i += 8;
    while (i < max && bit_at(a, i) == bit_at(b, i))
        i++;
    return i;
}

static void mask_key(uint8_t out[16], const uint8_t* addr, unsigned cidr, unsigned bytes)
{
    uint8_t tmp[16] = { 0 };
    unsigned i;
    memcpy(tmp, addr, bytes);
    memcpy(out, tmp, 16);
    for (i = cidr; i < bytes * 8; i++)
        out[i >> 3] &= (uint8_t)~(0x80u >> (i & 7));
}

static wg_aip_node_t* aip_node(wg_aip_t* t, const uint8_t key[16], unsigned cidr, void* peer)
{
    wg_aip_node_t* n = (wg_aip_node_t*)calloc(1, sizeof *n);
    if (!n)
        return NULL;
    memcpy(n->key, key, 16);
    n->cidr = (uint8_t)cidr;
    n->peer = peer;
    t->nodes++;
    return n;
}

int wg_aip_insert(wg_aip_t* t, int v6, const uint8_t* addr, unsigned cidr, void* peer)
{
    unsigned bytes = v6 ? 16 : 4;
    wg_aip_node_t** slot = v6 ? &t->root6 : &t->root4;
    uint8_t key[16];
    if (cidr > bytes * 8 || !peer)
        return -1;
    mask_key(key, addr, cidr, bytes);
    for (;;) {
        wg_aip_node_t *n = *slot, *leaf, *mid;
        unsigned c;
        if (!n) {
            *slot = aip_node(t, key, cidr, peer);
            return *slot ? 0 : -1;
        }
        c = common_bits(n->key, key, n->cidr < cidr ? n->cidr : cidr);
        if (c == n->cidr && c == cidr) {
            n->peer = peer;
            return 0;
        }
        if (c == n->cidr) {
            slot = &n->c[bit_at(key, n->cidr)];
            continue;
        }
        leaf = aip_node(t, key, cidr, peer);
        if (!leaf)
            return -1;
        if (c == cidr) {
            leaf->c[bit_at(n->key, cidr)] = n;
            *slot = leaf;
            return 0;
        }
        mask_key(key, key, c, bytes);
        mid = aip_node(t, key, c, NULL);
        if (!mid) {
            free(leaf);
            t->nodes--;
            return -1;
        }
        mid->c[bit_at(leaf->key, c)] = leaf;
        mid->c[bit_at(n->key, c)] = n;
        *slot = mid;
        return 0;
    }
}

int wg_aip_remove(wg_aip_t* t, int v6, const uint8_t* addr, unsigned cidr)
{
    unsigned bytes = v6 ? 16 : 4;
    wg_aip_node_t** stack[130];
    wg_aip_node_t** slot = v6 ? &t->root6 : &t->root4;
    uint8_t key[16];
    int d = 0, i;
    if (cidr > bytes * 8)
        return -1;
    mask_key(key, addr, cidr, bytes);
    for (;;) {
        wg_aip_node_t* n = *slot;
        if (!n || n->cidr > cidr || common_bits(n->key, key, n->cidr) < n->cidr)
            return -1;
        stack[d++] = slot;
        if (n->cidr == cidr)
            break;
        slot = &n->c[bit_at(key, n->cidr)];
    }
    if (!(*stack[d - 1])->peer)
        return -1;
    (*stack[d - 1])->peer = NULL;
    for (i = d - 1; i >= 0; i--) {
        wg_aip_node_t* n = *stack[i];
        if (n->peer || (n->c[0] && n->c[1]))
            break;
        *stack[i] = n->c[0] ? n->c[0] : n->c[1];
        free(n);
        t->nodes--;
    }
    return 0;
}

static void prune(wg_aip_node_t** slot, void* peer, size_t* nodes)
{
    wg_aip_node_t* n = *slot;
    if (!n)
        return;
    prune(&n->c[0], peer, nodes);
    prune(&n->c[1], peer, nodes);
    if (peer && n->peer == peer)
        n->peer = NULL;
    if (!peer || (!n->peer && !(n->c[0] && n->c[1]))) {
        *slot = peer ? (n->c[0] ? n->c[0] : n->c[1]) : NULL;
        free(n);
        (*nodes)--;
    }
}

void wg_aip_remove_peer(wg_aip_t* t, void* peer)
{
    if (!peer)
        return;
    prune(&t->root4, peer, &t->nodes);
    prune(&t->root6, peer, &t->nodes);
}

void* wg_aip_lookup(const wg_aip_t* t, int v6, const uint8_t* addr)
{
    const wg_aip_node_t* n = v6 ? t->root6 : t->root4;
    unsigned maxc = v6 ? 128 : 32;
    void* found = NULL;
    while (n) {
        if (common_bits(n->key, addr, n->cidr) < n->cidr)
            break;
        if (n->peer)
            found = n->peer;
        if (n->cidr >= maxc)
            break;
        n = n->c[bit_at(addr, n->cidr)];
    }
    return found;
}

void wg_aip_free(wg_aip_t* t)
{
    prune(&t->root4, NULL, &t->nodes);
    prune(&t->root6, NULL, &t->nodes);
}

#endif
