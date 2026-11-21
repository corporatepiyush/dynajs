#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "ds-internal.h"
#include "ds-parr.h"

#if defined(_WIN32)
#include <windows.h>
#define PARR_HAVE_VM 1
#elif (defined(__unix__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) && !defined(__wasm__)
#include <sys/mman.h>
#include <unistd.h>
#define PARR_HAVE_VM 1
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#if !defined(MAP_ANONYMOUS) && defined(__linux__)
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#else
#define PARR_HAVE_VM 0
#endif

#define PARR_ONE ((uint64_t)1)
#define PARR_FIRST_SLOTS 1u
#define PARR_LAZY_BYTES 4096u

#if PARR_HAVE_VM && defined(_WIN32)
static size_t parr_page(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096;
}

static uint8_t* parr_vm_reserve(size_t n) { return (uint8_t*)VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }

static int parr_vm_commit(uint8_t* a, size_t n) { return n == 0 || VirtualAlloc(a, n, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1; }

static int parr_vm_decommit(uint8_t* a, size_t n) { return n == 0 || VirtualFree(a, n, MEM_DECOMMIT) ? 0 : -1; }

static void parr_vm_release(uint8_t* a, size_t n)
{
    (void)n;
    if (a)
        VirtualFree(a, 0, MEM_RELEASE);
}
#elif PARR_HAVE_VM
static size_t parr_page(void)
{
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? (size_t)v : 4096;
}

static uint8_t* parr_vm_reserve(size_t n)
{
    void* a = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return a == MAP_FAILED ? NULL : (uint8_t*)a;
}

static int parr_vm_commit(uint8_t* a, size_t n) { return n == 0 ? 0 : mprotect(a, n, PROT_READ | PROT_WRITE); }

static int parr_vm_decommit(uint8_t* a, size_t n)
{
    void* q;
    if (n == 0)
        return 0;
    q = mmap(a, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return q == MAP_FAILED ? -1 : 0;
}

static void parr_vm_release(uint8_t* a, size_t n)
{
    if (a)
        munmap(a, n);
}
#else
static size_t parr_page(void) { return 4096; }

static uint8_t* parr_vm_reserve(size_t n)
{
    (void)n;
    return NULL;
}

static int parr_vm_commit(uint8_t* a, size_t n)
{
    (void)a;
    (void)n;
    return -1;
}

static int parr_vm_decommit(uint8_t* a, size_t n)
{
    (void)a;
    (void)n;
    return -1;
}

static void parr_vm_release(uint8_t* a, size_t n)
{
    (void)a;
    (void)n;
}
#endif

int dyn_parr_vm_supported(void) { return PARR_HAVE_VM; }

void dyn_parr_opts_default(dyn_parr_opts_t* o)
{
    memset(o, 0, sizeof(*o));
    o->partition = 64;
    o->max_capacity = DYN_DS_MAX_ITEMS;
    o->flat_small_bytes = 32768;
    o->grow_step = 1;
    o->key_field = -1;
}

static size_t parr_words(uint64_t bits) { return (size_t)((bits + 63) >> 6); }

static size_t parr_round(size_t n, size_t pg) { return (n + pg - 1) / pg * pg; }

static size_t parr_meta_need(const dyn_parr_t* p, unsigned i, uint32_t np)
{
    uint64_t slots = (uint64_t)np << p->pshift;
    if (i == DYN_PARR_M_GEN)
        return (size_t)slots * 4;
    if (i == DYN_PARR_M_OCC)
        return parr_words(slots) * 8;
    if (i == DYN_PARR_M_TOMB)
        return p->o.soft_delete ? parr_words(slots) * 8 : 0;
    if (i == DYN_PARR_M_FREE)
        return p->lifo ? (size_t)slots * 4 : 0;
    if (i == DYN_PARR_M_OPEN2)
        return parr_words(parr_words(np)) * 8;
    return parr_words(np) * 8;
}

static void parr_sync(dyn_parr_t* p)
{
    unsigned f;
    if (p->flat)
        for (f = 0; f < p->nfields; f++)
            p->base[f] = p->data[f].p;
    p->gen = (uint32_t*)p->meta[DYN_PARR_M_GEN].p;
    p->occ = (uint64_t*)p->meta[DYN_PARR_M_OCC].p;
    p->tomb = (uint64_t*)p->meta[DYN_PARR_M_TOMB].p;
    p->open = (uint64_t*)p->meta[DYN_PARR_M_OPEN].p;
    p->open2 = (uint64_t*)p->meta[DYN_PARR_M_OPEN2].p;
    p->any = (uint64_t*)p->meta[DYN_PARR_M_ANY].p;
    p->fstack = (uint32_t*)p->meta[DYN_PARR_M_FREE].p;
}

static int parr_seg_ensure(dyn_parr_seg_t* s, size_t need)
{
    size_t nc;
    uint8_t* q;
    if (need <= s->cap)
        return 0;
    if (s->res) {
        nc = s->cap * 2;
        if (nc < need)
            nc = need;
        nc = parr_round(nc, parr_page());
        if (nc > s->res)
            nc = s->res;
        if (nc < need || parr_vm_commit(s->p + s->cap, nc - s->cap) < 0)
            return -1;
        s->cap = nc;
        return 0;
    }
    nc = s->cap ? s->cap : 64;
    while (nc < need) {
        if (nc > SIZE_MAX / 2)
            return -1;
        nc *= 2;
    }
    q = (uint8_t*)DYN_DS_REALLOC(s->p, nc);
    if (!q)
        return -1;
    memset(q + s->cap, 0, nc - s->cap);
    s->p = q;
    s->cap = nc;
    return 0;
}

int dyn_parr_init(dyn_parr_t* p, const dyn_parr_opts_t* o, unsigned nfields, const uint32_t* esz)
{
    dyn_parr_opts_t d;
    unsigned f;
    uint32_t P, maxcap, off = 0;
    memset(p, 0, sizeof(*p));
    if (!o) {
        dyn_parr_opts_default(&d);
        o = &d;
    }
    P = o->partition;
    if (nfields < 1 || nfields > DYN_PARR_MAX_FIELDS || !esz)
        return -1;
    if (P < 32 || P > 256 || (P & (P - 1)))
        return -1;
    if (o->mode > DYN_PARR_TABLE || o->growth > DYN_PARR_GROW_LINEAR || o->reuse > DYN_PARR_REUSE_LIFO || !(o->compact_at >= 0 && o->compact_at <= 1))
        return -1;
    if (o->key_field >= (int32_t)nfields || (o->key_field >= 0 && esz[o->key_field] != 8))
        return -1;
    for (f = 0; f < nfields; f++)
        if (esz[f] == 0 || esz[f] > (1u << 20))
            return -1;
    p->o = *o;
    if (!p->o.grow_step)
        p->o.grow_step = 1;
    p->nfields = nfields;
    for (f = 0; f < nfields; f++)
        p->esz[f] = esz[f];
    p->pshift = (uint32_t)__builtin_ctz(P);
    p->pmask = P - 1;
    maxcap = o->max_capacity && o->max_capacity < DYN_DS_MAX_ITEMS ? o->max_capacity : DYN_DS_MAX_ITEMS;
    p->maxpart = (maxcap + P - 1) >> p->pshift;
    p->lifo = o->reuse == DYN_PARR_REUSE_LIFO;
    p->slow = o->soft_delete || o->on_release || o->key_field >= 0;
    if (o->mode == DYN_PARR_AUTO)
        p->flat = PARR_HAVE_VM && !o->no_vm;
    else
        p->flat = o->mode == DYN_PARR_FLAT;
    if (!p->flat) {
        for (f = 0; f < nfields; f++) {
            p->foff[f] = off;
            off += (P * esz[f] + 15u) & ~15u;
        }
        p->block = off;
    }
    if (o->initial_capacity && dyn_parr_reserve(p, o->initial_capacity) < 0) {
        dyn_parr_destroy(p);
        return -1;
    }
    return 0;
}

static size_t parr_part_off(const dyn_parr_t* p, uint32_t slots, unsigned f)
{
    size_t off = 0;
    unsigned g;
    for (g = 0; g < f; g++)
        off += ((size_t)slots * p->esz[g] + 15u) & ~(size_t)15u;
    return off;
}

static uint8_t* parr_block(const dyn_parr_t* p, uint32_t c)
{
    return (uint8_t*)(p->pt[0][c] + ((uintptr_t)c << p->pshift) * p->esz[0]);
}

void dyn_parr_destroy(dyn_parr_t* p)
{
    unsigned f, i;
    uint32_t c;
    if (!p->flat) {
        for (c = 0; c < p->npart; c++)
            DYN_DS_FREE(parr_block(p, c));
        for (f = 0; f < p->nfields; f++)
            DYN_DS_FREE(p->pt[f]);
    }
    for (f = 0; f < p->nfields; f++)
        if (!p->data[f].res)
            DYN_DS_FREE(p->data[f].p);
    for (i = 0; i < DYN_PARR_M_COUNT; i++)
        if (!p->meta[i].res)
            DYN_DS_FREE(p->meta[i].p);
    parr_vm_release(p->vm, p->vmsize);
    DYN_DS_FREE(p->ht);
    memset(p, 0, sizeof(*p));
}

static int parr_go_vm(dyn_parr_t* p, uint32_t np)
{
    dyn_parr_seg_t* old[DYN_PARR_MAX_FIELDS + DYN_PARR_M_COUNT];
    dyn_parr_seg_t nw[DYN_PARR_MAX_FIELDS + DYN_PARR_M_COUNT];
    size_t res[DYN_PARR_MAX_FIELDS + DYN_PARR_M_COUNT], need, keep, total = 0, off = 0, pg = parr_page();
    uint64_t maxslots = (uint64_t)p->maxpart << p->pshift;
    unsigned i, n = p->nfields + DYN_PARR_M_VM;
    uint8_t* vm;
    for (i = 0; i < n; i++) {
        if (i < p->nfields) {
            old[i] = &p->data[i];
            res[i] = parr_round((size_t)(maxslots * p->esz[i]), pg);
        } else {
            old[i] = &p->meta[i - p->nfields];
            res[i] = parr_round(parr_meta_need(p, i - p->nfields, p->maxpart), pg);
        }
        total += res[i];
    }
    if (maxslots * 64 > (uint64_t)(SIZE_MAX / 4)) {
        p->vm_failed = 1;
        return -1;
    }
    vm = parr_vm_reserve(total);
    if (!vm) {
        p->vm_failed = 1;
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!res[i])
            continue;
        nw[i].p = vm + off;
        nw[i].cap = 0;
        nw[i].res = res[i];
        off += res[i];
        need = i < p->nfields ? ((size_t)np << p->pshift) * p->esz[i] : parr_meta_need(p, i - p->nfields, np);
        keep = old[i]->cap < res[i] ? old[i]->cap : res[i];
        if (parr_seg_ensure(&nw[i], need > keep ? need : keep) < 0) {
            parr_vm_release(vm, total);
            p->vm_failed = 1;
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        if (!res[i])
            continue;
        keep = old[i]->cap < res[i] ? old[i]->cap : res[i];
        if (keep)
            memcpy(nw[i].p, old[i]->p, keep);
        DYN_DS_FREE(old[i]->p);
        *old[i] = nw[i];
    }
    p->vm = vm;
    p->vmsize = total;
    parr_sync(p);
    return 0;
}

static void parr_mark_open(dyn_parr_t* p, uint32_t c)
{
    if (!p->open[c >> 6])
        p->open2[c >> 12] |= PARR_ONE << ((c >> 6) & 63);
    p->open[c >> 6] |= PARR_ONE << (c & 63);
}

static void parr_mark_full(dyn_parr_t* p, uint32_t c)
{
    p->open[c >> 6] &= ~(PARR_ONE << (c & 63));
    if (!p->open[c >> 6])
        p->open2[c >> 12] &= ~(PARR_ONE << ((c >> 6) & 63));
}

static uint64_t parr_pm(const dyn_parr_t* p, uint32_t c) { return dyn_parr_pmask(p, c); }

static uint32_t parr_pwords(const dyn_parr_t* p) { return p->pshift >= 6 ? 1u << (p->pshift - 6) : 1u; }

static uint64_t parr_used(const dyn_parr_t* p, uint32_t w) { return p->tomb ? p->occ[w] | p->tomb[w] : p->occ[w]; }

static int parr_part_free(const dyn_parr_t* p, uint32_t c)
{
    uint32_t w = (c << p->pshift) >> 6, k, n = parr_pwords(p);
    uint64_t m = parr_pm(p, c);
    for (k = 0; k < n; k++)
        if (~parr_used(p, w + k) & m)
            return 1;
    return 0;
}

static int parr_part_live(const dyn_parr_t* p, uint32_t c)
{
    uint32_t w = (c << p->pshift) >> 6, k, n = parr_pwords(p);
    uint64_t m = parr_pm(p, c);
    for (k = 0; k < n; k++)
        if (p->occ[w + k] & m)
            return 1;
    return 0;
}

static uint32_t parr_part_slot(const dyn_parr_t* p, uint32_t c)
{
    uint32_t w = (c << p->pshift) >> 6, k, n = parr_pwords(p);
    uint64_t m = parr_pm(p, c), b;
    for (k = 0; k < n; k++) {
        b = ~parr_used(p, w + k) & m;
        if (b)
            return ((w + k) << 6) + (uint32_t)__builtin_ctzll(b);
    }
    return DYN_PARR_NIL;
}

static uint32_t parr_open_from(const dyn_parr_t* p, uint32_t c)
{
    uint32_t w = c >> 6, i, n2;
    uint64_t b;
    if (c >= p->npart)
        return DYN_PARR_NIL;
    b = p->open[w] & (~(uint64_t)0 << (c & 63));
    if (b)
        return (w << 6) + (uint32_t)__builtin_ctzll(b);
    n2 = (uint32_t)parr_words(parr_words(p->npart));
    for (i = (w + 1) >> 6; i < n2; i++) {
        b = p->open2[i];
        if (i == (w + 1) >> 6)
            b &= ~(uint64_t)0 << ((w + 1) & 63);
        if (b) {
            w = (i << 6) + (uint32_t)__builtin_ctzll(b);
            return (w << 6) + (uint32_t)__builtin_ctzll(p->open[w]);
        }
    }
    return DYN_PARR_NIL;
}

static int parr_commit(dyn_parr_t* p, uint32_t slots)
{
    unsigned f;
    if (!p->flat) {
        uint8_t* b = (uint8_t*)DYN_DS_MALLOC(parr_part_off(p, slots, p->nfields));
        if (!b)
            return -1;
        for (f = 0; f < p->nfields; f++)
            memcpy(b + parr_part_off(p, slots, f), (uint8_t*)p->pt[f][0], (size_t)p->cap * p->esz[f]);
        DYN_DS_FREE(parr_block(p, 0));
        for (f = 0; f < p->nfields; f++)
            p->pt[f][0] = (uintptr_t)(b + parr_part_off(p, slots, f));
        p->cap = slots;
        return 0;
    }
    for (f = 0; f < p->nfields; f++)
        if (parr_seg_ensure(&p->data[f], (size_t)slots * p->esz[f]) < 0) {
            parr_sync(p);
            return -1;
        }
    parr_sync(p);
    p->cap = slots;
    return 0;
}

static int parr_grow_to(dyn_parr_t* p, uint32_t np, int lazy)
{
    uint32_t c, f, nc, slots;
    unsigned i;
    size_t big = 0;
    if (np > p->maxpart)
        np = p->maxpart;
    if (np <= p->npart)
        return -1;
    if (p->npart && p->cap < (p->npart << p->pshift) && parr_commit(p, p->npart << p->pshift) < 0)
        return -1;
    lazy = lazy && p->lifo && !p->npart && parr_part_off(p, 1u << p->pshift, p->nfields) > PARR_LAZY_BYTES;
    if (lazy)
        np = 1;
    slots = lazy ? PARR_FIRST_SLOTS : np << p->pshift;
    if (p->flat) {
        for (f = 0; f < p->nfields; f++)
            if (((size_t)np << p->pshift) * p->esz[f] > big)
                big = ((size_t)np << p->pshift) * p->esz[f];
        if (PARR_HAVE_VM && !p->vm && !p->vm_failed && !p->o.no_vm && big > p->o.flat_small_bytes)
            (void)parr_go_vm(p, np);
        for (f = 0; f < p->nfields; f++)
            if (parr_seg_ensure(&p->data[f], (size_t)slots * p->esz[f]) < 0) {
                parr_sync(p);
                return -1;
            }
    }
    for (i = 0; i < DYN_PARR_M_COUNT; i++)
        if (parr_seg_ensure(&p->meta[i], parr_meta_need(p, i, np)) < 0) {
            parr_sync(p);
            return -1;
        }
    parr_sync(p);
    if (!p->flat) {
        if (np > p->ptcap) {
            nc = p->ptcap ? p->ptcap * 2 : 16;
            if (nc < np)
                nc = np;
            for (f = 0; f < p->nfields; f++) {
                uintptr_t* t = (uintptr_t*)DYN_DS_REALLOC(p->pt[f], (size_t)nc * sizeof(uintptr_t));
                if (!t)
                    return -1;
                p->pt[f] = t;
            }
            p->ptcap = nc;
        }
        for (c = p->npart; c < np; c++) {
            uint8_t* b = (uint8_t*)DYN_DS_MALLOC(c || slots == np << p->pshift ? p->block : parr_part_off(p, slots, p->nfields));
            if (!b) {
                if (c == p->npart)
                    return -1;
                np = c;
                break;
            }
            for (f = 0; f < p->nfields; f++)
                p->pt[f][c] = (uintptr_t)(b + (c || slots == np << p->pshift ? p->foff[f] : parr_part_off(p, slots, f))) - ((uintptr_t)c << p->pshift) * p->esz[f];
        }
    }
    for (c = p->npart; c < np && !p->lifo; c++)
        parr_mark_open(p, c);
    if (p->lo > p->npart)
        p->lo = p->npart;
    p->npart = np;
    p->cap = slots;
    return 0;
}

static int parr_grow(dyn_parr_t* p)
{
    uint32_t g = p->o.growth, add, full = p->npart << p->pshift;
    if (p->cap < full)
        return parr_commit(p, p->cap * 2 < full ? p->cap * 2 : full);
    if (g == DYN_PARR_GROW_AUTO)
        g = p->flat && !p->vm ? DYN_PARR_GROW_DOUBLE : DYN_PARR_GROW_LINEAR;
    add = g == DYN_PARR_GROW_DOUBLE ? (p->npart ? p->npart : 1) : p->o.grow_step;
    if (add > p->maxpart - p->npart)
        add = p->maxpart - p->npart;
    return add ? parr_grow_to(p, p->npart + add, 1) : -1;
}

int dyn_parr_reserve(dyn_parr_t* p, uint32_t n)
{
    uint64_t np = ((uint64_t)n + p->pmask) >> p->pshift;
    if (np > p->maxpart)
        return -1;
    if (np <= p->npart)
        return p->cap >= n ? 0 : parr_commit(p, p->npart << p->pshift);
    return parr_grow_to(p, (uint32_t)np, 0);
}

void dyn_parr_note_full(dyn_parr_t* p, uint32_t c, uint32_t k)
{
    uint32_t w = (c << p->pshift) >> 6, n = parr_pwords(p);
    uint64_t m = parr_pm(p, c);
    for (k++; k < n; k++)
        if (~parr_used(p, w + k) & m)
            return;
    parr_mark_full(p, c);
    if (p->lo == c)
        p->lo = c + 1;
}

uint32_t dyn_parr_alloc_slow(dyn_parr_t* p)
{
    uint32_t c;
    if (p->lifo) {
        if (!p->nfree && p->hw >= p->cap && parr_grow(p) < 0)
            return DYN_PARR_NIL;
        return dyn_parr_alloc(p);
    }
    c = parr_open_from(p, p->lo);
    if (c == DYN_PARR_NIL) {
        p->lo = p->npart;
        if (parr_grow(p) < 0)
            return DYN_PARR_NIL;
        c = parr_open_from(p, p->lo);
        if (c == DYN_PARR_NIL)
            return DYN_PARR_NIL;
    }
    p->lo = c;
    if (parr_part_slot(p, c) == DYN_PARR_NIL)
        return DYN_PARR_NIL;
    return dyn_parr_alloc(p);
}

static uint64_t parr_mix(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static uint64_t parr_key(const dyn_parr_t* p, uint32_t s)
{
    uint64_t k;
    memcpy(&k, dyn_parr_at(p, (unsigned)p->o.key_field, s), 8);
    return k;
}

static uint32_t parr_key_pos(const dyn_parr_t* p, uint32_t s)
{
    uint32_t m = p->htcap - 1, i, e;
    uint64_t k = parr_key(p, s);
    for (i = (uint32_t)parr_mix(k) & m;; i = (i + 1) & m) {
        e = p->ht[i];
        if (e == DYN_PARR_NIL)
            return DYN_PARR_NIL;
        if (e == s)
            return i;
        if (parr_key(p, e) == k)
            return DYN_PARR_NIL;
    }
}

static void parr_key_del(dyn_parr_t* p, uint32_t s)
{
    uint32_t m = p->htcap - 1, i = parr_key_pos(p, s), j, k;
    if (i == DYN_PARR_NIL)
        return;
    for (j = i;;) {
        j = (j + 1) & m;
        if (p->ht[j] == DYN_PARR_NIL)
            break;
        k = (uint32_t)parr_mix(parr_key(p, p->ht[j])) & m;
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
            continue;
        p->ht[i] = p->ht[j];
        i = j;
    }
    p->ht[i] = DYN_PARR_NIL;
    p->htn--;
}

static int parr_key_rehash(dyn_parr_t* p, uint32_t nc)
{
    uint32_t* t = (uint32_t*)DYN_DS_MALLOC((size_t)nc * sizeof(uint32_t));
    uint32_t i, j, m = nc - 1;
    if (!t)
        return -1;
    memset(t, 0xff, (size_t)nc * sizeof(uint32_t));
    for (i = 0; i < p->htcap; i++)
        if (p->ht[i] != DYN_PARR_NIL) {
            for (j = (uint32_t)parr_mix(parr_key(p, p->ht[i])) & m; t[j] != DYN_PARR_NIL; j = (j + 1) & m)
                ;
            t[j] = p->ht[i];
        }
    DYN_DS_FREE(p->ht);
    p->ht = t;
    p->htcap = nc;
    return 0;
}

int dyn_parr_key_put(dyn_parr_t* p, uint32_t s)
{
    uint32_t m, i, e;
    uint64_t k;
    if (p->o.key_field < 0 || !dyn_parr_live(p, s))
        return -1;
    if (((uint64_t)p->htn + 1) * 2 > p->htcap && parr_key_rehash(p, p->htcap ? p->htcap * 2 : 16) < 0)
        return -1;
    m = p->htcap - 1;
    k = parr_key(p, s);
    for (i = (uint32_t)parr_mix(k) & m;; i = (i + 1) & m) {
        e = p->ht[i];
        if (e == DYN_PARR_NIL)
            break;
        if (parr_key(p, e) == k)
            return e == s ? 0 : -1;
    }
    p->ht[i] = s;
    p->htn++;
    return 0;
}

uint32_t dyn_parr_key_find(const dyn_parr_t* p, uint64_t key)
{
    uint32_t m, i, e;
    if (!p->ht)
        return DYN_PARR_NIL;
    m = p->htcap - 1;
    for (i = (uint32_t)parr_mix(key) & m;; i = (i + 1) & m) {
        e = p->ht[i];
        if (e == DYN_PARR_NIL || parr_key(p, e) == key)
            return e;
    }
}

int dyn_parr_needs_compact(const dyn_parr_t* p)
{
    return p->tombs && p->o.compact_at > 0 && (double)p->tombs >= p->o.compact_at * (double)(p->live + p->tombs);
}

int dyn_parr_release_slow(dyn_parr_t* p, uint32_t s)
{
    uint32_t c, w;
    uint64_t bit;
    if (!dyn_parr_live(p, s))
        return -1;
    if (p->ht)
        parr_key_del(p, s);
    c = s >> p->pshift;
    w = s >> 6;
    bit = PARR_ONE << (s & 63);
    p->occ[w] &= ~bit;
    p->gen[s]++;
    p->live--;
    if (p->tomb) {
        p->tomb[w] |= bit;
        p->tombs++;
    } else if (p->lifo) {
        p->fstack[p->nfree++] = s;
    } else {
        parr_mark_open(p, c);
        if (c < p->lo)
            p->lo = c;
    }
    if (!(p->occ[w] & parr_pm(p, c)) && !parr_part_live(p, c))
        p->any[c >> 6] &= ~(PARR_ONE << (c & 63));
    if (p->o.on_release) {
        p->depth++;
        p->o.on_release(p->o.ud, s);
        p->depth--;
    }
    if (p->tombs && !p->depth && dyn_parr_needs_compact(p))
        (void)dyn_parr_compact(p);
    return 0;
}

static uint32_t parr_next_any(const dyn_parr_t* p, uint32_t c)
{
    uint32_t w = c >> 6, nw = (uint32_t)parr_words(p->npart);
    uint64_t b;
    if (c >= p->npart)
        return DYN_PARR_NIL;
    b = p->any[w] & (~(uint64_t)0 << (c & 63));
    for (;;) {
        if (b) {
            c = (w << 6) + (uint32_t)__builtin_ctzll(b);
            return c < p->npart ? c : DYN_PARR_NIL;
        }
        if (++w >= nw)
            return DYN_PARR_NIL;
        b = p->any[w];
    }
}

uint32_t dyn_parr_next(const dyn_parr_t* p, uint32_t from)
{
    uint32_t nw, w, c, w2;
    uint64_t b;
    if (from >= p->cap)
        return DYN_PARR_NIL;
    nw = (uint32_t)parr_words(p->cap);
    w = from >> 6;
    b = p->occ[w] & (~(uint64_t)0 << (from & 63));
    for (;;) {
        if (b)
            return (w << 6) + (uint32_t)__builtin_ctzll(b);
        if (++w >= nw)
            return DYN_PARR_NIL;
        b = p->occ[w];
        if (!b) {
            c = parr_next_any(p, (w << 6) >> p->pshift);
            if (c == DYN_PARR_NIL)
                return DYN_PARR_NIL;
            w2 = (c << p->pshift) >> 6;
            if (w2 > w)
                w = w2;
            b = p->occ[w];
        }
    }
}

uint32_t dyn_parr_next_free(const dyn_parr_t* p)
{
    uint32_t c;
    if (p->lifo)
        return p->nfree ? p->fstack[p->nfree - 1] : p->hw < p->cap ? p->hw
                                                                   : DYN_PARR_NIL;
    c = parr_open_from(p, p->lo);
    return c == DYN_PARR_NIL ? DYN_PARR_NIL : parr_part_slot(p, c);
}

static uint32_t parr_scan_hole(const dyn_parr_t* p, uint32_t from, uint32_t lim)
{
    uint32_t w, s;
    uint64_t b;
    while (from < lim) {
        w = from >> 6;
        b = ~p->occ[w] & (~(uint64_t)0 << (from & 63));
        if (b) {
            s = (w << 6) + (uint32_t)__builtin_ctzll(b);
            return s < lim ? s : lim;
        }
        from = (w + 1) << 6;
    }
    return lim;
}

static uint32_t parr_scan_top(const dyn_parr_t* p, uint32_t below)
{
    uint32_t w, r;
    uint64_t b;
    while (below > 0) {
        w = (below - 1) >> 6;
        r = below - (w << 6);
        b = p->occ[w];
        if (r < 64)
            b &= (PARR_ONE << r) - 1;
        if (b)
            return (w << 6) + 63 - (uint32_t)__builtin_clzll(b);
        below = w << 6;
    }
    return DYN_PARR_NIL;
}

int dyn_parr_is_dense(const dyn_parr_t* p)
{
    uint32_t top;
    if (p->tombs)
        return 0;
    if (!p->live)
        return 1;
    top = parr_scan_top(p, p->cap);
    return top != DYN_PARR_NIL && top + 1 == p->live;
}

static void parr_shrink(dyn_parr_t* p, uint32_t keep)
{
    uint32_t c, f;
    size_t from, pg = parr_page();
    if (!p->flat) {
        for (c = keep; c < p->npart; c++)
            DYN_DS_FREE(parr_block(p, c));
        return;
    }
    if (!p->vm)
        return;
    for (f = 0; f < p->nfields; f++) {
        from = parr_round(((size_t)keep << p->pshift) * p->esz[f], pg);
        if (from < p->data[f].cap && parr_vm_decommit(p->data[f].p + from, p->data[f].cap - from) == 0)
            p->data[f].cap = from;
    }
}

int64_t dyn_parr_compact(dyn_parr_t* p)
{
    uint32_t live = p->live, dst, src, f, keep, c, minp;
    int64_t moved = 0;
    size_t nw;
    if (!p->cap)
        return 0;
    dst = parr_scan_hole(p, 0, live);
    src = parr_scan_top(p, p->cap);
    while (dst < live && src != DYN_PARR_NIL && src >= live) {
        for (f = 0; f < p->nfields; f++)
            memcpy(dyn_parr_at(p, f, dst), dyn_parr_at(p, f, src), p->esz[f]);
        p->occ[dst >> 6] |= PARR_ONE << (dst & 63);
        p->occ[src >> 6] &= ~(PARR_ONE << (src & 63));
        p->gen[dst]++;
        p->gen[src]++;
        if (p->ht) {
            uint32_t i = parr_key_pos(p, src);
            if (i != DYN_PARR_NIL)
                p->ht[i] = dst;
        }
        if (p->o.on_move)
            p->o.on_move(p->o.ud, src, dst);
        moved++;
        dst = parr_scan_hole(p, dst + 1, live);
        src = parr_scan_top(p, src);
    }
    nw = parr_words(p->cap);
    if (p->tomb)
        memset(p->tomb, 0, nw * 8);
    p->tombs = 0;
    keep = (live + p->pmask) >> p->pshift;
    minp = (uint32_t)(((uint64_t)p->o.initial_capacity + p->pmask) >> p->pshift);
    if (keep < minp)
        keep = minp;
    if (keep > p->npart)
        keep = p->npart;
    memset(p->open, 0, parr_words(p->npart) * 8);
    memset(p->any, 0, parr_words(p->npart) * 8);
    memset(p->open2, 0, parr_words(parr_words(p->npart)) * 8);
    parr_shrink(p, keep);
    p->npart = keep;
    if ((keep << p->pshift) < p->cap)
        p->cap = keep << p->pshift;
    for (c = 0; c < keep; c++) {
        if (!p->lifo && parr_part_free(p, c))
            parr_mark_open(p, c);
        if (parr_part_live(p, c))
            p->any[c >> 6] |= PARR_ONE << (c & 63);
    }
    p->lo = 0;
    p->nfree = 0;
    p->hw = live;
    return moved;
}

size_t dyn_parr_bytes(const dyn_parr_t* p)
{
    size_t n = sizeof(*p) + (size_t)p->htcap * sizeof(uint32_t);
    unsigned f, i;
    if (p->flat)
        for (f = 0; f < p->nfields; f++)
            n += p->data[f].cap;
    else
        n += (size_t)p->npart * p->block + (size_t)p->ptcap * sizeof(uintptr_t) * p->nfields;
    if (!p->flat && p->npart && p->cap < (p->npart << p->pshift))
        n -= p->block - parr_part_off(p, p->cap, p->nfields);
    for (i = 0; i < DYN_PARR_M_COUNT; i++)
        n += p->meta[i].cap;
    return n;
}
