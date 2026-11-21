#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-patricia.h"
#include "ds-chunk.h"

#define PT_NIL UINT32_MAX
#define PT_INL 16u
#define PT_PATH 64u
#define PT_CS 10u
#define PT_CM ((1u << PT_CS) - 1)

typedef struct {
    uint32_t c[2];
    uint32_t bits;
    uint32_t v;
} ds_ptn;

struct dyn_patricia {
    dyn_ds_ops vops;
    dyn_chunk_t na, sa;
    uint32_t root;
    size_t size;
};

#define PN(t, x) ((ds_ptn*)((uint32_t)(x) >> PT_CS ? (t)->na.ch[(uint32_t)(x) >> PT_CS] : (t)->na.c0) + ((uint32_t)(x) & PT_CM))
#define PT_ID(p) ((uint32_t)(uintptr_t)(p))
#define PT_PTR(i) ((void*)(uintptr_t)(i))

static int pt_bit(const uint8_t* key, uint32_t i)
{
    return (key[i >> 3] >> (7 - (i & 7))) & 1;
}

static uint32_t pt_common(const uint8_t* a, uint32_t abits, const uint8_t* b, uint32_t bbits)
{
    uint32_t lim = abits < bbits ? abits : bbits, i = 0;
    while (i + 64 <= lim) {
        uint64_t x, y;
        memcpy(&x, a + (i >> 3), 8);
        memcpy(&y, b + (i >> 3), 8);
        if (x != y)
            break;
        i += 64;
    }
    while (i + 8 <= lim && a[i >> 3] == b[i >> 3])
        i += 8;
    while (i < lim && pt_bit(a, i) == pt_bit(b, i))
        i++;
    return i;
}

static uint8_t* pt_slot(const dyn_patricia_t* t, uint32_t v)
{
    return dyn_chunk_at(&t->sa, v);
}

static const uint8_t* pt_key(const dyn_patricia_t* t, uint32_t x)
{
    uint8_t* s = pt_slot(t, PN(t, x)->v);
    uint8_t* p;
    if (((size_t)PN(t, x)->bits + 7) / 8 <= PT_INL)
        return s;
    memcpy(&p, s, sizeof(p));
    return p;
}

static void* pt_val(const dyn_patricia_t* t, uint32_t x)
{
    return pt_slot(t, PN(t, x)->v) + PT_INL;
}

static int pt_eq(const uint8_t* a, const uint8_t* b, uint32_t bits)
{
    size_t full = bits >> 3;
    if (memcmp(a, b, full) != 0)
        return 0;
    if (bits & 7) {
        uint8_t m = (uint8_t)(0xFF << (8 - (bits & 7)));
        return ((a[full] ^ b[full]) & m) == 0;
    }
    return 1;
}

static uint32_t pt_node(dyn_patricia_t* t, uint32_t bits)
{
    uint32_t id = dyn_chunk_alloc(&t->na);
    if (id == PT_NIL)
        return PT_NIL;
    PN(t, id)->c[0] = PN(t, id)->c[1] = PT_NIL;
    PN(t, id)->bits = bits;
    PN(t, id)->v = PT_NIL;
    return id;
}

static void pt_node_free(dyn_patricia_t* t, uint32_t id)
{
    PN(t, id)->v = PT_NIL;
    dyn_chunk_release(&t->na, id);
}

static int pt_attach(dyn_patricia_t* t, uint32_t id, const uint8_t* key, const void* val)
{
    uint32_t bits = PN(t, id)->bits, v;
    size_t kb = ((size_t)bits + 7) / 8;
    uint8_t *heap = NULL, *s, *kp;
    if (kb > PT_INL && !(heap = (uint8_t*)DYN_DS_MALLOC(kb)))
        return -1;
    v = dyn_chunk_alloc(&t->sa);
    if (v == PT_NIL) {
        DYN_DS_FREE(heap);
        return -1;
    }
    s = pt_slot(t, v);
    kp = s;
    if (heap) {
        memcpy(s, &heap, sizeof(heap));
        kp = heap;
    }
    if (kb) {
        memcpy(kp, key, kb);
        if (bits & 7)
            kp[kb - 1] &= (uint8_t)(0xFF << (8 - (bits & 7)));
    }
    ds_ops_copy(&t->vops, s + PT_INL, val);
    PN(t, id)->v = v;
    return 0;
}

static void pt_detach(dyn_patricia_t* t, uint32_t id)
{
    uint32_t v = PN(t, id)->v;
    uint8_t* s = pt_slot(t, v);
    ds_ops_release(&t->vops, s + PT_INL);
    if (((size_t)PN(t, id)->bits + 7) / 8 > PT_INL) {
        uint8_t* p;
        memcpy(&p, s, sizeof(p));
        DYN_DS_FREE(p);
    }
    dyn_chunk_release(&t->sa, v);
    PN(t, id)->v = PT_NIL;
}

static uint32_t* pt_link(dyn_patricia_t* t, uint32_t p, int side)
{
    return p == PT_NIL ? &t->root : &PN(t, p)->c[side];
}

dyn_patricia_t* dyn_patricia_new(const dyn_ds_ops* val)
{
    dyn_patricia_t* t = (dyn_patricia_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    ds_ops_init(&t->vops, val);
    dyn_chunk_init(&t->sa, PT_INL + ds_align8(t->vops.size), PT_CS, 8, 1u << PT_CS);
    dyn_chunk_init(&t->na, sizeof(ds_ptn), PT_CS, 8, 1u << PT_CS);
    t->root = PT_NIL;
    return t;
}

static void pt_clear(dyn_patricia_t* t)
{
    uint32_t i;
    for (i = 0; i < t->na.len; i++)
        if (PN(t, i)->v != PT_NIL)
            pt_detach(t, i);
    dyn_chunk_destroy(&t->na);
    dyn_chunk_destroy(&t->sa);
    t->root = PT_NIL;
    t->size = 0;
}

void dyn_patricia_free(dyn_patricia_t* t)
{
    if (!t)
        return;
    pt_clear(t);
    DYN_DS_FREE(t);
}

size_t dyn_patricia_size(const dyn_patricia_t* t)
{
    return t->size;
}

static int pt_push(dyn_patricia_cursor* cur, uint32_t x)
{
    if (!cur)
        return 0;
    if (cur->n == cur->cap) {
        uint32_t nc = cur->cap ? cur->cap * 2 : 64;
        void** np = (void**)DYN_DS_REALLOC(cur->path, sizeof(void*) * nc);
        if (!np)
            return -1;
        cur->path = np;
        cur->cap = nc;
    }
    cur->path[cur->n++] = PT_PTR(x);
    return 0;
}

static int pt_put_from(dyn_patricia_t* t, uint32_t p, int side, const uint8_t* key, uint32_t bits, const void* val, dyn_patricia_cursor* cur)
{
    uint32_t x = *pt_link(t, p, side), r = PT_NIL, cpl, pth[PT_PATH], d = 0, j;
    if (x == PT_NIL) {
        cpl = bits;
    } else {
        r = x;
        for (;;) {
            uint32_t c;
            if (d < PT_PATH)
                pth[d] = r;
            d++;
            if (PN(t, r)->bits >= bits)
                break;
            c = PN(t, r)->c[pt_bit(key, PN(t, r)->bits)];
            if (c == PT_NIL)
                break;
            r = c;
        }
        while (PN(t, r)->v == PT_NIL)
            r = PN(t, r)->c[0];
        cpl = pt_common(pt_key(t, r), PN(t, r)->bits, key, bits);
    }
    if (d && d <= PT_PATH) {
        for (j = 0; j < d && PN(t, pth[j])->bits <= cpl && PN(t, pth[j])->bits < bits; j++)
            if (pt_push(cur, pth[j]) < 0)
                return -1;
        if (j) {
            p = pth[j - 1];
            side = pt_bit(key, PN(t, p)->bits);
        }
    }
    for (;;) {
        x = *pt_link(t, p, side);
        if (x == PT_NIL) {
            uint32_t id = pt_node(t, bits);
            if (id == PT_NIL)
                return -1;
            if (pt_attach(t, id, key, val) < 0) {
                pt_node_free(t, id);
                return -1;
            }
            *pt_link(t, p, side) = id;
            t->size++;
            return pt_push(cur, id) < 0 ? -1 : 1;
        }
        if (PN(t, x)->bits > cpl) {
            int ob = pt_bit(pt_key(t, r), cpl);
            uint32_t mid = pt_node(t, cpl), leaf = PT_NIL;
            if (mid == PT_NIL)
                return -1;
            if (cpl == bits) {
                if (pt_attach(t, mid, key, val) < 0) {
                    pt_node_free(t, mid);
                    return -1;
                }
            } else {
                leaf = pt_node(t, bits);
                if (leaf == PT_NIL || pt_attach(t, leaf, key, val) < 0) {
                    if (leaf != PT_NIL)
                        pt_node_free(t, leaf);
                    pt_node_free(t, mid);
                    return -1;
                }
                PN(t, mid)->c[!ob] = leaf;
            }
            PN(t, mid)->c[ob] = x;
            *pt_link(t, p, side) = mid;
            t->size++;
            if (pt_push(cur, mid) < 0 || (leaf != PT_NIL && pt_push(cur, leaf) < 0))
                return -1;
            return 1;
        }
        if (PN(t, x)->bits == bits) {
            int added = PN(t, x)->v == PT_NIL;
            if (added) {
                if (pt_attach(t, x, key, val) < 0)
                    return -1;
            } else {
                void* dv = pt_val(t, x);
                ds_ops_release(&t->vops, dv);
                ds_ops_copy(&t->vops, dv, val);
            }
            t->size += (size_t)added;
            return pt_push(cur, x) < 0 ? -1 : added;
        }
        if (pt_push(cur, x) < 0)
            return -1;
        p = x;
        side = pt_bit(key, PN(t, x)->bits);
    }
}

int dyn_patricia_put(dyn_patricia_t* t, const uint8_t* key, uint32_t bits, const void* val)
{
    return pt_put_from(t, PT_NIL, 0, key, bits, val, NULL);
}

int dyn_patricia_put_cursor(dyn_patricia_t* t, dyn_patricia_cursor* cur, const uint8_t* key, uint32_t bits, const void* val)
{
    uint32_t p = PT_NIL;
    int side = 0, r;
    size_t kb = ((size_t)bits + 7) / 8;
    if (cur->n) {
        uint32_t c = pt_common(cur->key, cur->kbits, key, bits);
        while (cur->n) {
            uint32_t x = PT_ID(cur->path[cur->n - 1]), nb = PN(t, x)->bits;
            if (nb <= c) {
                if (nb == bits) {
                    cur->n--;
                    if (cur->n) {
                        p = PT_ID(cur->path[cur->n - 1]);
                        side = pt_bit(key, PN(t, p)->bits);
                    }
                } else {
                    p = x;
                    side = pt_bit(key, nb);
                }
                break;
            }
            cur->n--;
        }
    }
    r = pt_put_from(t, p, side, key, bits, val, cur);
    if (r < 0) {
        cur->n = 0;
        return r;
    }
    if (kb > cur->kcap) {
        uint8_t* nk = (uint8_t*)DYN_DS_REALLOC(cur->key, kb);
        if (!nk) {
            cur->n = 0;
            return r;
        }
        cur->key = nk;
        cur->kcap = (uint32_t)kb;
    }
    if (kb)
        memcpy(cur->key, key, kb);
    cur->kbits = bits;
    return r;
}

void dyn_patricia_cursor_free(dyn_patricia_cursor* cur)
{
    DYN_DS_FREE(cur->path);
    DYN_DS_FREE(cur->key);
    memset(cur, 0, sizeof(*cur));
}

void* dyn_patricia_get(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits)
{
    uint32_t x = t->root;
    while (x != PT_NIL) {
        const ds_ptn* n = PN(t, x);
        if (n->bits >= bits) {
            if (n->bits != bits || n->v == PT_NIL || !pt_eq(pt_key(t, x), key, bits))
                return NULL;
            return pt_val(t, x);
        }
        x = n->c[pt_bit(key, n->bits)];
    }
    return NULL;
}

void* dyn_patricia_longest_prefix(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits, uint32_t* matched_bits)
{
    uint32_t x = t->root, last = PT_NIL, best, cpl, cand[PT_PATH], nc = 0;
    while (x != PT_NIL) {
        const ds_ptn* n = PN(t, x);
        if (n->bits > bits)
            break;
        if (n->v != PT_NIL) {
            last = x;
            if (nc < PT_PATH)
                cand[nc] = x;
            nc++;
        }
        if (n->bits == bits)
            break;
        x = n->c[pt_bit(key, n->bits)];
    }
    if (last == PT_NIL)
        return NULL;
    cpl = pt_common(pt_key(t, last), PN(t, last)->bits, key, bits);
    best = last;
    if (cpl < PN(t, last)->bits && nc <= PT_PATH) {
        best = PT_NIL;
        for (; nc > 1 && best == PT_NIL; nc--)
            if (PN(t, cand[nc - 2])->bits <= cpl)
                best = cand[nc - 2];
        if (best == PT_NIL)
            return NULL;
    } else if (cpl < PN(t, last)->bits) {
        best = PT_NIL;
        x = t->root;
        while (x != PT_NIL && PN(t, x)->bits <= cpl) {
            if (PN(t, x)->v != PT_NIL)
                best = x;
            x = PN(t, x)->c[pt_bit(key, PN(t, x)->bits)];
        }
        if (best == PT_NIL)
            return NULL;
    }
    if (matched_bits)
        *matched_bits = PN(t, best)->bits;
    return pt_val(t, best);
}

int dyn_patricia_remove(dyn_patricia_t* t, const uint8_t* key, uint32_t bits)
{
    uint32_t x = t->root, p = PT_NIL, gp = PT_NIL, child;
    int ps = 0, gs = 0;
    while (x != PT_NIL && PN(t, x)->bits < bits) {
        gp = p;
        gs = ps;
        p = x;
        ps = pt_bit(key, PN(t, x)->bits);
        x = PN(t, x)->c[ps];
    }
    if (x == PT_NIL || PN(t, x)->bits != bits || PN(t, x)->v == PT_NIL || !pt_eq(pt_key(t, x), key, bits))
        return 0;
    pt_detach(t, x);
    t->size--;
    if (t->size == 0) {
        pt_clear(t);
        return 1;
    }
    if (PN(t, x)->c[0] != PT_NIL && PN(t, x)->c[1] != PT_NIL)
        return 1;
    child = PN(t, x)->c[0] != PT_NIL ? PN(t, x)->c[0] : PN(t, x)->c[1];
    *pt_link(t, p, ps) = child;
    pt_node_free(t, x);
    if (child == PT_NIL && p != PT_NIL && PN(t, p)->v == PT_NIL) {
        *pt_link(t, gp, gs) = PN(t, p)->c[!ps];
        pt_node_free(t, p);
    }
    return 1;
}

static int pt_walk_rec(const dyn_patricia_t* t, uint32_t x, dyn_patricia_visit fn, void* ud)
{
    int r;
    while (x != PT_NIL) {
        if (PN(t, x)->v != PT_NIL && (r = fn(pt_key(t, x), PN(t, x)->bits, pt_val(t, x), ud)) != 0)
            return r;
        if (PN(t, x)->c[0] == PT_NIL) {
            x = PN(t, x)->c[1];
            continue;
        }
        if (PN(t, x)->c[1] != PT_NIL && (r = pt_walk_rec(t, PN(t, x)->c[0], fn, ud)) != 0)
            return r;
        x = PN(t, x)->c[1] != PT_NIL ? PN(t, x)->c[1] : PN(t, x)->c[0];
    }
    return 0;
}

int dyn_patricia_walk(const dyn_patricia_t* t, const uint8_t* prefix, uint32_t bits, dyn_patricia_visit fn, void* ud)
{
    uint32_t x = t->root;
    while (x != PT_NIL) {
        if (PN(t, x)->bits >= bits) {
            uint32_t r = x;
            while (PN(t, r)->v == PT_NIL)
                r = PN(t, r)->c[0];
            if (pt_common(pt_key(t, r), PN(t, r)->bits, prefix, bits) < bits)
                return 0;
            return pt_walk_rec(t, x, fn, ud);
        }
        x = PN(t, x)->c[pt_bit(prefix, PN(t, x)->bits)];
    }
    return 0;
}
