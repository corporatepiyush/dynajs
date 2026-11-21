/* dyn_parr (src/core/ds/ds-parr.c): the partitioned array with a flat virtual-memory body or a
 * per-partition base-pointer table, generation-checked handles, occupancy/tombstone/summary
 * bitmaps, soft delete + compaction, a unique u64 key index over one column, and a cascading
 * release hook. Every configuration (partition 32..256 x flat-VM / flat-heap / table x hard / soft
 * delete x growth policy) is driven by fixed-seed random operation sequences against a reference
 * model written here (slot -> FREE/LIVE/TOMB + serial + current handle), and after every batch the
 * whole structure is cross-checked: data, occupancy, generations, the summary bitmaps and the
 * search hint, iteration (FOREACH and next), handle resolution of live and stale handles, the key
 * index, lowest-free choice and density. Directed tests cover parameter validation, the capacity
 * limit, cascades, stale handles across compaction + shrink + regrow (generations must survive a
 * decommit), pointer stability across VM commit boundaries, allocation failure at every point of
 * growth, and the lazy first chunk. A LIFO pool whose partition exceeds 4 KB commits partition 0 as
 * 1, 2, 4, 8, 16 slots before whole partitions (flat and table mode), so a 128-entry SkipList
 * holds about 9 KB instead of one 34 KB partition of 1008-byte nodes; smaller partitions are committed whole,
 * since the lazy start cost 1.31x on 16-byte appends at N=128. Reserve, compact and adding a
 * partition while partition 0 is partly committed are covered. A LIFO table pool therefore moves
 * partition 0 while it grows: ds-cbtree.c re-reads node pointers after every allocation. A
 * counting allocator proves nothing leaks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long live_allocs;
static long fail_after = -1;
static int may_fail(void)
{
    if (fail_after < 0)
        return 0;
    if (fail_after == 0)
        return 1;
    fail_after--;
    return 0;
}
static void* t_malloc(size_t n)
{
    void* p = may_fail() ? NULL : malloc(n);
    if (p)
        live_allocs++;
    return p;
}
static void* t_calloc(size_t n, size_t s)
{
    void* p = may_fail() ? NULL : calloc(n, s);
    if (p)
        live_allocs++;
    return p;
}
static void* t_realloc(void* p, size_t n)
{
    void* q = may_fail() ? NULL : realloc(p, n);
    if (q && !p)
        live_allocs++;
    return q;
}
static void t_free(void* p)
{
    if (p)
        live_allocs--;
    free(p);
}
#define DYN_DS_MALLOC(n) t_malloc(n)
#define DYN_DS_CALLOC(n, s) t_calloc(n, s)
#define DYN_DS_REALLOC(p, n) t_realloc(p, n)
#define DYN_DS_FREE(p) t_free(p)
#include "ds/ds-parr.c"

static int failures;
static long checks;
#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            if (++failures > 25)                        \
                exit(1);                                \
        }                                               \
    } while (0)

static uint64_t rng_s;
static uint64_t rnd(void)
{
    rng_s += 0x9e3779b97f4a7c15ULL;
    uint64_t z = rng_s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static uint32_t rn(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

enum { FREE = 0,
    LIVE = 1,
    TOMB = 2 };

typedef struct {
    uint64_t a, b, c;
} blob_t;

#define STALE_MAX 4096
typedef struct {
    dyn_parr_t p;
    uint32_t maxslots;
    uint8_t* st;
    uint64_t* ser;
    uint64_t* hnd;
    uint32_t* lv;
    uint32_t* lpos;
    uint32_t nlive, ntomb;
    uint32_t* fstk;
    uint32_t nfs, hw;
    uint64_t stale[STALE_MAX];
    uint32_t nstale;
    uint64_t next_serial;
    long releases, moves, last_release;
    int in_move;
} model_t;

static void stale_push(model_t* m, uint64_t h)
{
    if (m->nstale < STALE_MAX)
        m->stale[m->nstale++] = h;
    else
        m->stale[rn(STALE_MAX)] = h;
}

static void lv_add(model_t* m, uint32_t s)
{
    m->lpos[s] = m->nlive;
    m->lv[m->nlive++] = s;
}
static void lv_del(model_t* m, uint32_t s)
{
    uint32_t i = m->lpos[s], last = m->lv[--m->nlive];
    m->lv[i] = last;
    m->lpos[last] = i;
}

static void hook_release(void* ud, uint32_t s)
{
    model_t* m = (model_t*)ud;
    m->releases++;
    m->last_release = (long)s;
}

static void hook_move(void* ud, uint32_t from, uint32_t to)
{
    model_t* m = (model_t*)ud;
    m->moves++;
    CHECK(from < m->maxslots && to < m->maxslots, "move in range %u->%u", from, to);
    CHECK(m->st[from] == LIVE, "move source %u was live (state %d)", from, m->st[from]);
    CHECK(m->st[to] != LIVE, "move target %u was not live", to);
    CHECK(to < from, "compaction moves downward (%u->%u)", from, to);
    if (m->st[to] == TOMB)
        m->ntomb--;
    m->st[to] = LIVE;
    m->ser[to] = m->ser[from];
    stale_push(m, m->hnd[from]);
    m->hnd[to] = dyn_parr_handle(&m->p, to);
    m->st[from] = FREE;
    m->lv[m->lpos[from]] = to;
    m->lpos[to] = m->lpos[from];
}

static void write_rec(dyn_parr_t* p, uint32_t s, uint64_t serial)
{
    uint64_t* k = (uint64_t*)dyn_parr_at(p, 0, s);
    uint32_t* c = (uint32_t*)dyn_parr_at(p, 1, s);
    blob_t* b = (blob_t*)dyn_parr_at(p, 2, s);
    *k = serial;
    *c = (uint32_t)(serial ^ 0x9e3779b9u);
    b->a = serial * 3;
    b->b = ~serial;
    b->c = serial << 7;
}

static int rec_ok(const dyn_parr_t* p, uint32_t s, uint64_t serial)
{
    const uint64_t* k = (const uint64_t*)dyn_parr_at(p, 0, s);
    const uint32_t* c = (const uint32_t*)dyn_parr_at(p, 1, s);
    const blob_t* b = (const blob_t*)dyn_parr_at(p, 2, s);
    if (*k != serial || *c != (uint32_t)(serial ^ 0x9e3779b9u))
        return 0;
    if (b->a != serial * 3 || b->b != ~serial || b->c != serial << 7)
        return 0;
    if (p->flat && DYN_PARR_FLAT_REF(p, 0, s, uint64_t) != k)
        return 0;
    if (!p->flat && DYN_PARR_TABLE_REF(p, 2, s, blob_t) != b)
        return 0;
    return DYN_PARR_REF(p, 1, s, uint32_t) == c;
}

static const uint32_t ESZ[3] = { 8, 4, sizeof(blob_t) };

static void model_init(model_t* m, const dyn_parr_opts_t* o)
{
    memset(m, 0, sizeof(*m));
    m->maxslots = (o->max_capacity + o->partition - 1) / o->partition * o->partition;
    m->st = calloc(m->maxslots, 1);
    m->ser = calloc(m->maxslots, sizeof(uint64_t));
    m->hnd = calloc(m->maxslots, sizeof(uint64_t));
    m->lv = calloc(m->maxslots, sizeof(uint32_t));
    m->lpos = calloc(m->maxslots, sizeof(uint32_t));
    m->fstk = calloc(m->maxslots, sizeof(uint32_t));
    m->next_serial = 1;
    dyn_parr_opts_t oo = *o;
    oo.on_release = o->on_release ? hook_release : NULL;
    oo.on_move = hook_move;
    oo.ud = m;
    CHECK(dyn_parr_init(&m->p, &oo, 3, ESZ) == 0, "init");
}

static void model_free(model_t* m)
{
    dyn_parr_destroy(&m->p);
    free(m->st);
    free(m->ser);
    free(m->hnd);
    free(m->lv);
    free(m->lpos);
    free(m->fstk);
}

static uint32_t model_lowest_free(const model_t* m, uint32_t cap)
{
    if (m->p.lifo)
        return m->nfs ? m->fstk[m->nfs - 1] : m->hw < cap ? m->hw
                                                          : DYN_PARR_NIL;
    for (uint32_t s = 0; s < cap; s++)
        if (m->st[s] == FREE)
            return s;
    return DYN_PARR_NIL;
}

static void after_compaction(model_t* m)
{
    for (uint32_t s = 0; s < m->maxslots; s++)
        if (m->st[s] == TOMB)
            m->st[s] = FREE;
    m->ntomb = 0;
    m->nfs = 0;
    m->hw = m->nlive;
    for (uint32_t s = 0; s < m->maxslots; s++)
        CHECK((m->st[s] == LIVE) == (s < m->nlive), "dense after compaction at %u (live %u)", s, m->nlive);
    CHECK(dyn_parr_is_dense(&m->p), "is_dense after compaction");
}

static void validate(model_t* m, const char* tag)
{
    dyn_parr_t* p = &m->p;
    uint32_t cap = p->cap, s, c, P = 1u << p->pshift, nw = (cap + 63) / 64, prev, got;
    uint64_t pc_occ = 0, pc_tomb = 0;
    CHECK(p->live == m->nlive, "%s: live %u vs model %u", tag, p->live, m->nlive);
    CHECK(p->tombs == m->ntomb, "%s: tombs %u vs model %u", tag, p->tombs, m->ntomb);
    CHECK(cap == p->npart * P && cap <= m->maxslots, "%s: cap %u npart %u", tag, cap, p->npart);
    for (s = cap; s < m->maxslots; s++)
        CHECK(m->st[s] == FREE, "%s: model has state %d beyond cap at %u", tag, m->st[s], s);
    for (s = 0; s < nw; s++) {
        uint64_t tw = p->tomb ? p->tomb[s] : 0;
        CHECK(!(p->occ[s] & tw), "%s: occ and tomb overlap in word %u", tag, s);
        pc_occ += (uint64_t)__builtin_popcountll(p->occ[s]);
        pc_tomb += (uint64_t)__builtin_popcountll(tw);
    }
    CHECK(pc_occ == m->nlive && pc_tomb == m->ntomb, "%s: popcounts %llu/%llu", tag, (unsigned long long)pc_occ, (unsigned long long)pc_tomb);
    for (s = 0; s < cap; s++) {
        int lv = (int)((p->occ[s >> 6] >> (s & 63)) & 1), tb = p->tomb ? (int)((p->tomb[s >> 6] >> (s & 63)) & 1) : 0;
        CHECK(lv == (m->st[s] == LIVE) && tb == (m->st[s] == TOMB), "%s: slot %u bits occ=%d tomb=%d model=%d", tag, s, lv, tb, m->st[s]);
        CHECK((int)(p->gen[s] & 1) == lv, "%s: slot %u generation parity %u vs live %d", tag, s, p->gen[s], lv);
        CHECK(dyn_parr_live(p, s) == lv, "%s: dyn_parr_live(%u)", tag, s);
        if (lv) {
            CHECK(rec_ok(p, s, m->ser[s]), "%s: data of slot %u", tag, s);
            CHECK(dyn_parr_resolve(p, m->hnd[s]) == s, "%s: live handle of %u resolves", tag, s);
            CHECK(dyn_parr_handle(p, s) == m->hnd[s], "%s: handle of %u is stable", tag, s);
            if (p->o.key_field >= 0)
                CHECK(dyn_parr_key_find(p, m->ser[s]) == s, "%s: key %llu -> %u", tag, (unsigned long long)m->ser[s], s);
        }
    }
    for (c = 0; c < p->npart; c++) {
        int fr = 0, an = 0;
        for (s = c * P; s < (c + 1) * P; s++) {
            fr |= m->st[s] == FREE;
            an |= m->st[s] == LIVE;
        }
        if (!p->lifo)
            CHECK((int)((p->open[c >> 6] >> (c & 63)) & 1) == fr, "%s: open bit of partition %u", tag, c);
        CHECK((int)((p->any[c >> 6] >> (c & 63)) & 1) == an, "%s: any bit of partition %u", tag, c);
    }
    for (c = 0; c < (p->npart + 63) / 64 && !p->lifo; c++)
        CHECK((int)((p->open2[c >> 6] >> (c & 63)) & 1) == (p->open[c] != 0), "%s: open2 bit %u", tag, c);
    if (p->lifo) {
        int same = p->nfree == m->nfs && p->hw == m->hw;
        for (uint32_t i = 0; same && i < m->nfs; i++)
            same = p->fstack[i] == m->fstk[i] && m->st[m->fstk[i]] == FREE;
        CHECK(same, "%s: free stack (%u entries, hw %u) matches the model (%u, hw %u)", tag, p->nfree, p->hw, m->nfs, m->hw);
        uint32_t holes = 0;
        for (s = 0; s < cap; s++)
            if (s < m->hw ? m->st[s] == FREE : m->st[s] != FREE)
                holes += s < m->hw ? 1 : 1000000;
        CHECK(holes == m->nfs, "%s: every hole below the high-water mark is on the stack once (%u vs %u)", tag, holes, m->nfs);
    } else {
        uint32_t lo = DYN_PARR_NIL;
        for (c = 0; c < p->npart && lo == DYN_PARR_NIL; c++)
            for (s = c * P; s < (c + 1) * P; s++)
                if (m->st[s] == FREE) {
                    lo = c;
                    break;
                }
        CHECK(p->lo <= lo, "%s: search hint %u is above the lowest open partition %u", tag, p->lo, lo);
    }
    CHECK((p->tomb == NULL || p->o.soft_delete) && (p->tomb != NULL || !p->o.soft_delete || cap == 0), "%s: tomb array exists exactly with soft delete", tag);
    prev = 0;
    got = 0;
    {
        uint32_t ok = 1, cnt = 0;
        DYN_PARR_FOREACH(p, it)
        {
            if ((cnt && it <= prev) || m->st[it] != LIVE)
                ok = 0;
            prev = it;
            cnt++;
        }
        CHECK(ok && cnt == m->nlive, "%s: FOREACH visits %u live in order (model %u)", tag, cnt, m->nlive);
    }
    {
        uint32_t cnt = 0, ok = 1, last = 0;
        DYN_PARR_EACH(p, it, {
            if ((cnt && it <= last) || m->st[it] != LIVE)
                ok = 0;
            last = it;
            cnt++;
        });
        CHECK(ok && cnt == m->nlive, "%s: DYN_PARR_EACH visits %u live in order (model %u)", tag, cnt, m->nlive);
        cnt = 0;
        last = 0;
        uint32_t e = 0, runs = 0;
        for (uint32_t a = dyn_parr_run(p, 0, &e); a != DYN_PARR_NIL; a = dyn_parr_run(p, e, &e)) {
            if (e <= a || e > cap || (runs && a <= last) || (a > 0 && m->st[a - 1] == LIVE && runs))
                ok = 0;
            if (e < cap && m->st[e] == LIVE)
                ok = 0;
            for (uint32_t i = a; i < e; i++) {
                if (m->st[i] != LIVE)
                    ok = 0;
                cnt++;
            }
            last = e;
            runs++;
        }
        CHECK(ok && cnt == m->nlive, "%s: dyn_parr_run covers %u live in %u maximal runs (model %u)", tag, cnt, runs, m->nlive);
    }
    {
        uint32_t cnt = 0, ok = 1;
        for (s = dyn_parr_next(p, 0); s != DYN_PARR_NIL; s = dyn_parr_next(p, s + 1)) {
            if (m->st[s] != LIVE)
                ok = 0;
            cnt++;
        }
        CHECK(ok && cnt == m->nlive, "%s: next() chain %u (model %u)", tag, cnt, m->nlive);
        for (int k = 0; k < 16 && cap; k++) {
            uint32_t from = rn(cap + 3), want = DYN_PARR_NIL;
            for (s = from; s < cap; s++)
                if (m->st[s] == LIVE) {
                    want = s;
                    break;
                }
            got = dyn_parr_next(p, from);
            CHECK(got == want, "%s: next(%u) = %u want %u", tag, from, got, want);
        }
    }
    for (uint32_t i = 0; i < m->nstale; i++) {
        uint32_t r = dyn_parr_resolve(p, m->stale[i]);
        CHECK(r == DYN_PARR_NIL, "%s: stale handle %llx resolves to %u", tag, (unsigned long long)m->stale[i], r);
    }
    CHECK(dyn_parr_next_free(p) == model_lowest_free(m, cap), "%s: next free %u vs %u", tag, dyn_parr_next_free(p), model_lowest_free(m, cap));
    {
        int dense = m->ntomb == 0;
        for (s = 0; s < cap && dense; s++)
            if ((m->st[s] == LIVE) != (s < m->nlive))
                dense = 0;
        CHECK(dyn_parr_is_dense(p) == dense, "%s: is_dense %d vs %d", tag, dyn_parr_is_dense(p), dense);
    }
    CHECK(dyn_parr_bytes(p) >= sizeof(*p), "%s: bytes", tag);
}

static void op_alloc(model_t* m, const char* tag)
{
    dyn_parr_t* p = &m->p;
    uint32_t cap0 = p->cap, want = model_lowest_free(m, cap0), s;
    if (want == DYN_PARR_NIL && cap0 < m->maxslots)
        want = cap0;
    s = dyn_parr_alloc(p);
    CHECK(s == want, "%s: alloc returned %u, lowest free is %u (cap %u)", tag, s, want, cap0);
    if (s == DYN_PARR_NIL || s >= m->maxslots)
        return;
    CHECK(m->st[s] == FREE, "%s: alloc handed out a %s slot %u", tag, m->st[s] == LIVE ? "live" : "tombstoned", s);
    if (p->lifo) {
        if (m->nfs && m->fstk[m->nfs - 1] == s)
            m->nfs--;
        else
            m->hw = s + 1;
    }
    m->st[s] = LIVE;
    m->ser[s] = m->next_serial++;
    write_rec(p, s, m->ser[s]);
    m->hnd[s] = dyn_parr_handle(p, s);
    CHECK(dyn_parr_resolve(p, m->hnd[s]) == s, "%s: fresh handle resolves", tag);
    lv_add(m, s);
    if (p->o.key_field >= 0)
        CHECK(dyn_parr_key_put(p, s) == 0, "%s: key put %u", tag, s);
}

static void op_free(model_t* m, const char* tag)
{
    dyn_parr_t* p = &m->p;
    uint32_t s = m->lv[rn(m->nlive)];
    uint64_t h = m->hnd[s], serial = m->ser[s];
    long r0 = m->releases, mv0 = m->moves;
    uint32_t tombs_before = m->ntomb;
    lv_del(m, s);
    if (p->o.soft_delete) {
        m->st[s] = TOMB;
        m->ntomb++;
    } else {
        m->st[s] = FREE;
        if (p->lifo)
            m->fstk[m->nfs++] = s;
    }
    stale_push(m, h);
    CHECK(dyn_parr_free(p, h) == 0, "%s: free live handle", tag);
    if (p->o.on_release)
        CHECK(m->releases == r0 + 1 && m->last_release == (long)s, "%s: release hook once for %u", tag, s);
    else
        CHECK(m->releases == r0, "%s: no release hook configured, none called", tag);
    CHECK(dyn_parr_resolve(p, h) == DYN_PARR_NIL, "%s: freed handle is stale", tag);
    CHECK(dyn_parr_free(p, h) == -1, "%s: double free refused", tag);
    if (p->o.key_field >= 0)
        CHECK(dyn_parr_key_find(p, serial) == DYN_PARR_NIL, "%s: freed key gone", tag);
    if (p->o.soft_delete && p->tombs == 0 && tombs_before + 1 > 0 && p->o.compact_at > 0) {
        after_compaction(m);
        (void)mv0;
    }
}

static void op_compact(model_t* m, const char* tag)
{
    long mv0 = m->moves;
    int64_t r = dyn_parr_compact(&m->p);
    CHECK(r == m->moves - mv0, "%s: compact reports %lld moves, hook saw %ld", tag, (long long)r, m->moves - mv0);
    after_compaction(m);
}

static void op_stale(model_t* m, const char* tag)
{
    dyn_parr_t* p = &m->p;
    uint32_t live0 = p->live;
    if (m->nstale) {
        uint64_t h = m->stale[rn(m->nstale)];
        CHECK(dyn_parr_free(p, h) == -1, "%s: stale free refused", tag);
    }
    if (p->cap) {
        uint32_t s = rn(p->cap);
        uint64_t forged = ((uint64_t)(p->gen[s] & ~1u) << 32) | s;
        CHECK(dyn_parr_resolve(p, forged) == DYN_PARR_NIL, "%s: even generation never resolves", tag);
        CHECK(dyn_parr_resolve(p, ((uint64_t)1 << 32) | (p->cap + rn(5))) == DYN_PARR_NIL, "%s: out-of-range slot", tag);
    }
    CHECK(dyn_parr_resolve(p, DYN_PARR_HNIL) == DYN_PARR_NIL, "%s: HNIL", tag);
    CHECK(dyn_parr_release(p, p->cap + 7) == -1, "%s: release beyond cap", tag);
    CHECK(p->live == live0, "%s: stale ops changed nothing", tag);
}

static void run_random(const dyn_parr_opts_t* o, uint64_t seed, uint32_t nops, uint32_t hi, const char* tag)
{
    model_t m;
    model_init(&m, o);
    CHECK(m.p.slow == (o->soft_delete || o->on_release || o->key_field >= 0), "%s: inline fast path is %s", tag, m.p.slow ? "off" : "on");
    rng_s = seed;
    uint32_t target = hi / 2;
    for (uint32_t i = 1; i <= nops; i++) {
        uint32_t r = rn(1000);
        if (i % 2000 == 0)
            target = rn(hi + 1);
        int want_alloc = m.nlive < target ? r < 700 : r < 300;
        if (r >= 990)
            op_stale(&m, tag);
        else if (r >= 985 && (m.ntomb || m.nlive))
            op_compact(&m, tag);
        else if (r == 984)
            CHECK(dyn_parr_reserve(&m.p, rn(m.maxslots + 1)) == 0 || 0, "%s: reserve", tag);
        else if (want_alloc || !m.nlive)
            op_alloc(&m, tag);
        else
            op_free(&m, tag);
        if (i % 1500 == 0)
            validate(&m, tag);
    }
    validate(&m, tag);
    while (m.nlive)
        op_free(&m, tag);
    op_compact(&m, tag);
    validate(&m, tag);
    model_free(&m);
}

static void test_random_matrix(void)
{
    static const uint32_t parts[] = { 32, 64, 128, 256 };
    static const char* mnames[] = { "flat-vm", "flat-heap", "table" };
    int combos = 0;
    for (int lifo = 0; lifo < 2; lifo++)
        for (int pi = 0; pi < 4; pi++)
            for (int mode = 0; mode < 3; mode++)
                for (int del = 0; del < 3; del++)
                    for (int gp = 0; gp < 3; gp++) {
                        int soft = del == 1, fast = del == 2;
                        dyn_parr_opts_t o;
                        char tag[96];
                        dyn_parr_opts_default(&o);
                        o.partition = parts[pi];
                        o.max_capacity = 9000 + parts[pi] / 2;
                        o.key_field = fast ? -1 : 0;
                        o.on_release = fast ? NULL : hook_release;
                        o.mode = mode == 2 ? DYN_PARR_TABLE : DYN_PARR_FLAT;
                        o.no_vm = mode == 1;
                        o.flat_small_bytes = 512;
                        o.soft_delete = (uint8_t)soft;
                        o.compact_at = soft && gp == 1 ? 0.35 : 0;
                        o.growth = gp == 0 ? DYN_PARR_GROW_AUTO : gp == 1 ? DYN_PARR_GROW_DOUBLE
                                                                          : DYN_PARR_GROW_LINEAR;
                        o.grow_step = 3;
                        o.reuse = (uint8_t)lifo;
                        o.initial_capacity = gp == 2 ? 100 : 0;
                        snprintf(tag, sizeof(tag), "P%u/%s/%s/g%d/%s", parts[pi], mnames[mode], soft ? "soft" : fast ? "hard-fastpath"
                                                                                                                     : "hard-hooked",
                            gp, lifo ? "lifo" : "lowest");
                        run_random(&o, 0x5eed0000ULL + (uint64_t)combos * 7919, 24000, gp == 1 ? 8800 : 2500, tag);
                        combos++;
                    }
    printf("random matrix: %d configurations\n", combos);
}

static void test_params(void)
{
    dyn_parr_t p;
    dyn_parr_opts_t o;
    uint32_t e1[1] = { 8 }, e0[1] = { 0 }, e4[1] = { 4 };
    static const uint32_t bad_parts[] = { 0, 16, 48, 100, 512, 33 };
    for (unsigned i = 0; i < sizeof(bad_parts) / sizeof(bad_parts[0]); i++) {
        dyn_parr_opts_default(&o);
        o.partition = bad_parts[i];
        CHECK(dyn_parr_init(&p, &o, 1, e1) == -1, "partition %u refused", bad_parts[i]);
    }
    for (uint32_t ps = 32; ps <= 256; ps *= 2) {
        dyn_parr_opts_default(&o);
        o.partition = ps;
        CHECK(dyn_parr_init(&p, &o, 1, e1) == 0, "partition %u accepted", ps);
        dyn_parr_destroy(&p);
    }
    CHECK(dyn_parr_init(&p, NULL, 1, e0) == -1, "zero element size refused");
    CHECK(dyn_parr_init(&p, NULL, 0, e1) == -1, "zero fields refused");
    CHECK(dyn_parr_init(&p, NULL, DYN_PARR_MAX_FIELDS + 1, e1) == -1, "too many fields refused");
    dyn_parr_opts_default(&o);
    o.key_field = 0;
    CHECK(dyn_parr_init(&p, &o, 1, e4) == -1, "key column must be 8 bytes");
    o.key_field = 1;
    CHECK(dyn_parr_init(&p, &o, 1, e1) == -1, "key column must exist");
    dyn_parr_opts_default(&o);
    o.compact_at = 1.5;
    CHECK(dyn_parr_init(&p, &o, 1, e1) == -1, "compact_at above 1 refused");
    dyn_parr_opts_default(&o);
    o.mode = 9;
    CHECK(dyn_parr_init(&p, &o, 1, e1) == -1, "unknown mode refused");
    CHECK(dyn_parr_init(&p, NULL, 1, e1) == 0, "defaults");
    CHECK(dyn_parr_capacity(&p) == 0 && dyn_parr_count(&p) == 0, "empty");
    CHECK(dyn_parr_next(&p, 0) == DYN_PARR_NIL && dyn_parr_next_free(&p) == DYN_PARR_NIL, "empty scans");
    CHECK(dyn_parr_resolve(&p, 1ULL << 32) == DYN_PARR_NIL && dyn_parr_compact(&p) == 0, "empty resolve/compact");
    CHECK(dyn_parr_key_put(&p, 0) == -1 && dyn_parr_key_find(&p, 1) == DYN_PARR_NIL, "no key column");
    dyn_parr_destroy(&p);
    printf("params ok\n");
}

static void test_capacity_limit(void)
{
    for (int reuse = 0; reuse < 2; reuse++)
        for (int mode = 1; mode <= 2; mode++)
            for (uint32_t ps = 32; ps <= 256; ps *= 2) {
                dyn_parr_t p;
                dyn_parr_opts_t o;
                uint32_t e[1] = { 8 }, n = 0, want = (3 * ps + 5 + ps - 1) / ps * ps;
                dyn_parr_opts_default(&o);
                o.partition = ps;
                o.mode = (uint8_t)mode;
                o.max_capacity = 3 * ps + 5;
                o.flat_small_bytes = 64;
                o.reuse = (uint8_t)reuse;
                CHECK(dyn_parr_init(&p, &o, 1, e) == 0, "init");
                while (dyn_parr_alloc(&p) != DYN_PARR_NIL)
                    n++;
                CHECK(n == want, "P%u mode %d: capacity limit %u allocs, want %u", ps, mode, n, want);
                CHECK(dyn_parr_reserve(&p, want + 1) == -1, "reserve beyond the limit fails");
                CHECK(dyn_parr_release(&p, want - 1) == 0 && dyn_parr_alloc(&p) == want - 1, "the last slot is reusable");
                CHECK(dyn_parr_release(&p, 0) == 0 && dyn_parr_alloc(&p) == 0, "the first slot is reusable");
                dyn_parr_destroy(&p);
            }
    printf("capacity limit ok\n");
}

typedef struct {
    dyn_parr_t* p;
    long calls;
    uint32_t* seen;
} cascade_t;

static void cascade_hook(void* ud, uint32_t s)
{
    cascade_t* c = (cascade_t*)ud;
    c->calls++;
    c->seen[s]++;
    uint32_t a = (uint32_t)(*DYN_PARR_REF(c->p, 0, s, uint64_t));
    uint32_t b = (uint32_t)(*DYN_PARR_REF(c->p, 0, s, uint64_t) >> 32);
    if (a != DYN_PARR_NIL)
        dyn_parr_release(c->p, a);
    if (b != DYN_PARR_NIL)
        dyn_parr_release(c->p, b);
}

static void test_cascade(void)
{
    for (int soft = 0; soft < 2; soft++)
        for (int mode = 1; mode <= 2; mode++) {
            dyn_parr_t p;
            dyn_parr_opts_t o;
            cascade_t cc;
            uint32_t e[1] = { 8 }, n = 2047, i;
            dyn_parr_opts_default(&o);
            o.partition = 32;
            o.mode = (uint8_t)mode;
            o.soft_delete = (uint8_t)soft;
            o.compact_at = soft ? 0.25 : 0;
            o.on_release = cascade_hook;
            o.ud = &cc;
            cc.p = &p;
            cc.calls = 0;
            cc.seen = calloc(4096, sizeof(uint32_t));
            CHECK(dyn_parr_init(&p, &o, 1, e) == 0, "init");
            for (i = 0; i < n; i++)
                CHECK(dyn_parr_alloc(&p) == i, "dense alloc");
            for (i = 0; i < n; i++) {
                uint64_t l = 2 * i + 1 < n ? 2 * i + 1 : DYN_PARR_NIL, r = 2 * i + 2 < n ? 2 * i + 2 : DYN_PARR_NIL;
                *DYN_PARR_REF(&p, 0, i, uint64_t) = l | (r << 32);
            }
            CHECK(dyn_parr_release(&p, 1) == 0, "release subtree root");
            uint32_t sub = 0;
            for (i = 0; i < n; i++) {
                uint32_t x = i + 1;
                while (x > 2)
                    x /= 2;
                if (i == 0)
                    continue;
                if (x == 2)
                    sub++;
            }
            CHECK(cc.calls == (long)sub, "soft=%d mode=%d cascade released %ld, subtree has %u", soft, mode, cc.calls, sub);
            for (i = 0; i < 4096; i++)
                CHECK(cc.seen[i] <= 1, "slot %u released twice", i);
            CHECK(dyn_parr_count(&p) == n - sub, "count after cascade %u", dyn_parr_count(&p));
            if (soft)
                CHECK(p.tombs == 0 && dyn_parr_is_dense(&p), "auto compaction ran once the cascade finished");
            else
                CHECK(dyn_parr_live(&p, 0) && dyn_parr_live(&p, 2) && !dyn_parr_live(&p, 1) && !dyn_parr_live(&p, 3), "hard cascade left the other subtree");
            dyn_parr_destroy(&p);
            free(cc.seen);
        }
    printf("cascade ok\n");
}

static void test_stale_after_shrink(void)
{
    for (int reuse = 0; reuse < 2; reuse++)
        for (int mode = 0; mode < 3; mode++) {
            dyn_parr_t p;
            dyn_parr_opts_t o;
            uint32_t e[2] = { 16, 8 }, n = 40000, i;
            uint64_t* hs = malloc(n * sizeof(uint64_t));
            dyn_parr_opts_default(&o);
            o.partition = 128;
            o.mode = mode == 2 ? DYN_PARR_TABLE : DYN_PARR_FLAT;
            o.no_vm = mode == 1;
            o.flat_small_bytes = 1024;
            o.reuse = (uint8_t)reuse;
            CHECK(dyn_parr_init(&p, &o, 2, e) == 0, "init");
            for (i = 0; i < n; i++) {
                uint32_t s = dyn_parr_alloc(&p);
                CHECK(s == i, "dense alloc %u", s);
                hs[i] = dyn_parr_handle(&p, s);
            }
            if (mode == 0)
                CHECK(p.vm != NULL, "flat mode moved to a VM reservation");
            if (mode == 1)
                CHECK(p.vm == NULL, "no_vm stays on the heap");
            size_t b0 = dyn_parr_bytes(&p);
            for (i = 100; i < n; i++)
                CHECK(dyn_parr_free(&p, hs[i]) == 0, "free");
            CHECK(dyn_parr_compact(&p) == 0, "nothing to move");
            CHECK(p.cap == 128, "shrunk to one partition (cap %u)", p.cap);
            if (mode != 1)
                CHECK(dyn_parr_bytes(&p) < b0 / 2, "mode %d: shrink returned memory (%zu -> %zu)", mode, b0, dyn_parr_bytes(&p));
            for (i = 100; i < n; i++)
                CHECK(dyn_parr_alloc(&p) == i, "regrow alloc");
            for (i = 100; i < n; i += 37)
                CHECK(dyn_parr_resolve(&p, hs[i]) == DYN_PARR_NIL, "mode %d: handle %u from before the shrink is stale after regrow", mode, i);
            for (i = 0; i < 100; i++)
                CHECK(dyn_parr_resolve(&p, hs[i]) == i, "survivor handle %u", i);
            dyn_parr_destroy(&p);
            free(hs);
        }
    printf("stale after shrink ok\n");
}

static void test_vm_pointer_stability(void)
{
    dyn_parr_t p;
    dyn_parr_opts_t o;
    uint32_t e[1] = { 24 }, n = 300000, i, s;
    if (!dyn_parr_vm_supported())
        return;
    dyn_parr_opts_default(&o);
    o.partition = 256;
    CHECK(dyn_parr_init(&p, &o, 1, e) == 0, "init");
    for (i = 0; i < 2000; i++)
        dyn_parr_alloc(&p);
    CHECK(p.vm != NULL, "past the small threshold");
    blob_t* first = DYN_PARR_FLAT_REF(&p, 0, 5, blob_t);
    first->a = 77;
    size_t commits = 0, lastcap = p.data[0].cap;
    for (i = 2000; i < n; i++) {
        s = dyn_parr_alloc(&p);
        CHECK(s == i, "alloc %u", s);
        DYN_PARR_FLAT_REF(&p, 0, s, blob_t)->a = s;
        if (p.data[0].cap != lastcap) {
            commits++;
            lastcap = p.data[0].cap;
        }
    }
    CHECK(commits >= 5, "crossed %zu commit boundaries", commits);
    CHECK(DYN_PARR_FLAT_REF(&p, 0, 5, blob_t) == first && first->a == 77, "element pointer stable across VM growth");
    for (i = 2000; i < n; i++)
        if (DYN_PARR_FLAT_REF(&p, 0, i, blob_t)->a != i) {
            CHECK(0, "value at %u", i);
            break;
        }
    dyn_parr_destroy(&p);
    printf("vm pointer stability ok (%zu commits)\n", commits);
}

static void test_soft_no_reuse(void)
{
    dyn_parr_t p;
    dyn_parr_opts_t o;
    uint32_t e[1] = { 8 }, i;
    dyn_parr_opts_default(&o);
    o.partition = 32;
    o.soft_delete = 1;
    CHECK(dyn_parr_init(&p, &o, 1, e) == 0, "init");
    for (i = 0; i < 64; i++)
        dyn_parr_alloc(&p);
    for (i = 0; i < 64; i += 2)
        dyn_parr_release(&p, i);
    CHECK(p.tombs == 32 && dyn_parr_needs_compact(&p) == 0, "manual compaction by default");
    CHECK(dyn_parr_alloc(&p) == 64, "tombstones are not reused");
    CHECK(dyn_parr_compact(&p) > 0 && dyn_parr_is_dense(&p) && p.live == 33, "compacted");
    CHECK(dyn_parr_alloc(&p) == 33, "after compaction the next slot is dense");
    dyn_parr_destroy(&p);
    printf("soft delete ok\n");
}

static void test_oom(void)
{
    int fails = 0, runs = 0;
    for (int mode = 0; mode < 6; mode++)
        for (long k = 0; k < 60; k++) {
            dyn_parr_t p;
            dyn_parr_opts_t o;
            uint32_t e[2] = { 8, 8 }, n = 0;
            dyn_parr_opts_default(&o);
            o.partition = 32;
            o.key_field = 0;
            o.mode = mode % 3 == 2 ? DYN_PARR_TABLE : DYN_PARR_FLAT;
            o.no_vm = mode % 3 == 1;
            o.reuse = mode >= 3;
            o.flat_small_bytes = 256;
            o.max_capacity = 4000;
            if (dyn_parr_init(&p, &o, 2, e) != 0)
                continue;
            runs++;
            fail_after = k;
            for (;;) {
                uint32_t s = dyn_parr_alloc(&p);
                if (s == DYN_PARR_NIL)
                    break;
                *DYN_PARR_REF(&p, 0, s, uint64_t) = s + 1;
                if (dyn_parr_key_put(&p, s) != 0) {
                    fails++;
                    CHECK(dyn_parr_release(&p, s) == 0, "release after a failed key put");
                    break;
                }
                n++;
            }
            if (fail_after == 0)
                fails++;
            fail_after = -1;
            uint32_t cnt = 0, ok = 1;
            DYN_PARR_FOREACH(&p, s)
            {
                if (*DYN_PARR_REF(&p, 0, s, uint64_t) != s + 1 || dyn_parr_key_find(&p, s + 1) != s)
                    ok = 0;
                cnt++;
            }
            CHECK(ok && cnt == n && p.live == n, "mode %d fail@%ld: consistent after failure (%u/%u)", mode, k, cnt, n);
            while (dyn_parr_alloc(&p) != DYN_PARR_NIL)
                ;
            CHECK(p.cap == 4000 && p.live == 4000, "growth resumes after failure (cap %u live %u)", p.cap, p.live);
            dyn_parr_destroy(&p);
        }
    printf("oom ok (%d runs, %d injected failures)\n", runs, fails);
}

static int lazy_body_ok(dyn_parr_t* p, uint32_t e)
{
    uint32_t s;
    if (p->flat && p->data[0].cap < (size_t)p->cap * e)
        return 0;
    for (s = 0; s < p->cap; s++)
        if (!dyn_parr_live(p, s))
            memset(dyn_parr_at(p, 0, s), 0xee, e);
    return 1;
}

static void test_lazy_first_chunk(void)
{
    for (int mode = 0; mode < 5; mode++) {
        dyn_parr_t p;
        dyn_parr_opts_t o;
        uint32_t e[2] = { 1000, 24 }, s, i, want, nf = mode == 4 ? 2 : 1;
        int lifo = mode != 3, table = mode == 2 || mode == 4;
        dyn_parr_opts_default(&o);
        o.partition = 32;
        o.reuse = (uint8_t)lifo;
        o.mode = table ? DYN_PARR_TABLE : DYN_PARR_FLAT;
        o.no_vm = mode != 1;
        o.flat_small_bytes = 512;
        CHECK(dyn_parr_init(&p, &o, nf, e) == 0, "lazy init mode %d", mode);
        for (i = 0; i < 100; i++) {
            s = dyn_parr_alloc(&p);
            CHECK(s == i, "mode %d: slot %u comes in order (got %u)", mode, i, s);
            memset(dyn_parr_at(&p, 0, s), (int)(i & 0xff), e[0]);
            if (nf == 2)
                memset(dyn_parr_at(&p, 1, s), (int)((i + 7) & 0xff), e[1]);
            want = i < 32 ? 32 : i < 64 ? 64 : 128;
            if (lifo && i < 32)
                for (want = 1; want < i + 1; want *= 2)
                    ;
            CHECK(p.cap >= i + 1 && (i >= 32 || p.cap == want), "mode %d: %u slots give capacity %u (want %u)", mode, i + 1, p.cap, want);
            CHECK(lazy_body_ok(&p, e[0]), "mode %d: body covers capacity at %u", mode, i);
            if ((mode == 0 || mode == 2) && i == 2)
                CHECK(dyn_parr_bytes(&p) < 8192, "mode %d: a 3-slot pool holds %zu bytes", mode, dyn_parr_bytes(&p));
        }
        for (i = 0; i < 100; i++) {
            const uint8_t* b = (const uint8_t*)dyn_parr_at(&p, 0, i);
            CHECK(b[0] == (i & 0xff) && b[e[0] - 1] == (i & 0xff), "mode %d: slot %u survives growth", mode, i);
            if (nf == 2) {
                b = (const uint8_t*)dyn_parr_at(&p, 1, i);
                CHECK(b[0] == ((i + 7) & 0xff) && b[e[1] - 1] == ((i + 7) & 0xff), "mode %d: field 1 of slot %u survives growth", mode, i);
            }
        }
        dyn_parr_destroy(&p);
        if (!lifo)
            continue;
        CHECK(dyn_parr_init(&p, &o, nf, e) == 0, "lazy init mode %d", mode);
        for (i = 0; i < 3; i++)
            dyn_parr_alloc(&p);
        CHECK(p.cap == 4 && dyn_parr_reserve(&p, 20) == 0 && p.cap >= 20 && lazy_body_ok(&p, e[0]), "mode %d: reserve inside a partly committed partition commits it (cap %u)", mode, p.cap);
        dyn_parr_destroy(&p);
        CHECK(dyn_parr_init(&p, &o, nf, e) == 0, "lazy init mode %d", mode);
        for (i = 0; i < 6; i++)
            dyn_parr_alloc(&p);
        dyn_parr_release(&p, 1);
        dyn_parr_release(&p, 3);
        dyn_parr_compact(&p);
        CHECK(p.cap <= 8 && lazy_body_ok(&p, e[0]) && p.live == 4, "mode %d: compact keeps capacity inside the body (cap %u)", mode, p.cap);
        for (i = 0; i < 60; i++)
            CHECK(dyn_parr_alloc(&p) != DYN_PARR_NIL && lazy_body_ok(&p, e[0]), "mode %d: regrow after compact", mode);
        dyn_parr_destroy(&p);
    }
    for (int mode = 0; mode < 2; mode++) {
        dyn_parr_t p;
        dyn_parr_opts_t o;
        uint32_t e = 16;
        dyn_parr_opts_default(&o);
        o.partition = 64;
        o.reuse = DYN_PARR_REUSE_LIFO;
        o.mode = mode ? DYN_PARR_TABLE : DYN_PARR_FLAT;
        o.no_vm = 1;
        CHECK(dyn_parr_init(&p, &o, 1, &e) == 0 && dyn_parr_alloc(&p) == 0 && p.cap == 64, "mode %d: a 1 KB partition is committed whole (cap %u)", mode, p.cap);
        dyn_parr_destroy(&p);
    }
}

static void test_sparse_iteration(void)
{
    static const uint32_t keep_pat[] = { 0, 63, 64, 127, 640, 641, 4095, 4096, 9000, 12287, 19999 };
    int scenarios = 0;
    for (uint32_t ps = 32; ps <= 256; ps *= 2)
        for (int mode = 1; mode <= 2; mode++) {
            dyn_parr_t p;
            dyn_parr_opts_t o;
            uint32_t e[1] = { 8 }, n = 20000, i, nk = sizeof(keep_pat) / sizeof(keep_pat[0]);
            uint8_t* want = calloc(n, 1);
            dyn_parr_opts_default(&o);
            o.partition = ps;
            o.mode = (uint8_t)mode;
            CHECK(dyn_parr_init(&p, &o, 1, e) == 0, "init");
            for (i = 0; i < n; i++)
                dyn_parr_alloc(&p);
            for (i = 0; i < nk; i++)
                want[keep_pat[i]] = 1;
            for (i = 0; i < n; i++)
                if (!want[i])
                    dyn_parr_release(&p, i);
            for (uint32_t from = 0; from <= n + 1; from += (from % 97) + 1) {
                uint32_t exp = DYN_PARR_NIL;
                for (i = from; i < n; i++)
                    if (want[i]) {
                        exp = i;
                        break;
                    }
                uint32_t got = dyn_parr_next(&p, from);
                CHECK(got == exp, "P%u mode %d sparse next(%u) = %u want %u", ps, mode, from, got, exp);
            }
            uint32_t cnt = 0, ok = 1, prev = 0;
            for (i = dyn_parr_next(&p, 0); i != DYN_PARR_NIL; i = dyn_parr_next(&p, i + 1)) {
                ok &= want[i] && (!cnt || i > prev);
                prev = i;
                cnt++;
            }
            CHECK(ok && cnt == nk, "P%u mode %d sparse next() chain %u of %u", ps, mode, cnt, nk);
            cnt = 0;
            DYN_PARR_EACH(&p, it, cnt += want[it];);
            CHECK(cnt == nk, "P%u mode %d sparse EACH %u of %u", ps, mode, cnt, nk);
            uint32_t end = 0;
            cnt = 0;
            for (uint32_t a = dyn_parr_run(&p, 0, &end); a != DYN_PARR_NIL; a = dyn_parr_run(&p, end, &end))
                for (i = a; i < end; i++)
                    cnt += want[i];
            CHECK(cnt == nk, "P%u mode %d sparse runs %u of %u", ps, mode, cnt, nk);
            dyn_parr_destroy(&p);
            free(want);
            scenarios++;
        }
    printf("sparse iteration ok (%d layouts)\n", scenarios);
}

int main(void)
{
    test_params();
    test_capacity_limit();
    test_soft_no_reuse();
    test_cascade();
    test_stale_after_shrink();
    test_vm_pointer_stability();
    test_sparse_iteration();
    test_lazy_first_chunk();
    test_oom();
    test_random_matrix();
    CHECK(live_allocs == 0, "every allocation was freed (%ld live)", live_allocs);
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
