/* The cores moved into dyn-ds.c out of the dyna:structures bindings (BitSet, UnionFind, Fenwick,
 * SegTree, circular buffer, Deque, Bloom, Trie, Heap, List, LRU, the scored skip list behind
 * ZSet/OrderedMap, and the init/free entry points of the dyna:structures-db cores), each driven by
 * random operation sequences against a brute-force model written here. The library is included as
 * one translation unit with a counting allocator, and value cells carry a serial number whose
 * release is counted, so every test also proves that nothing leaks and nothing is released twice. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ds_live_allocs;
static void* t_malloc(size_t n)
{
    void* p = malloc(n);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_calloc(size_t n, size_t s)
{
    void* p = calloc(n, s);
    if (p)
        ds_live_allocs++;
    return p;
}
static void* t_realloc(void* p, size_t n)
{
    void* q = realloc(p, n);
    if (q && !p)
        ds_live_allocs++;
    return q;
}
static void t_free(void* p)
{
    if (p)
        ds_live_allocs--;
    free(p);
}
#define DYN_DS_MALLOC(n) t_malloc(n)
#define DYN_DS_CALLOC(n, s) t_calloc(n, s)
#define DYN_DS_REALLOC(p, n) t_realloc(p, n)
#define DYN_DS_FREE(p) t_free(p)
#include "dyn-ds.c"

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

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* a cell holds a serial; live[serial] says it has not been released yet */
#define MAXSER 400000
static unsigned char live[MAXSER];
static uint32_t next_serial = 1;
static long released;
static dyn_cell_t mk(void)
{
    dyn_cell_t c;
    memset(&c, 0, sizeof(c));
    c.w[0] = next_serial;
    live[next_serial++] = 1;
    return c;
}
static void rel(void* ud, const dyn_cell_t* c)
{
    (void)ud;
    if (c->w[0] >= MAXSER || !live[c->w[0]]) {
        printf("FAIL: released cell %llu twice or never made\n", (unsigned long long)c->w[0]);
        failures++;
        return;
    }
    live[c->w[0]] = 0;
    released++;
}
static void drop(const dyn_cell_t* c)
{
    rel(NULL, c);
}
static long cells_live(void)
{
    long n = 0;
    uint32_t i;
    for (i = 1; i < next_serial; i++)
        n += live[i];
    return n;
}

static void test_bitset(void)
{
    dyn_bitset_t b;
    static unsigned char ref[70000];
    int step;
    memset(ref, 0, sizeof(ref));
    dyn_bitset_init(&b);
    for (step = 0; step < 200000; step++) {
        uint32_t i = (uint32_t)(rnd() % 70000);
        switch (rnd() % 4) {
        case 0:
            CHECK(dyn_bitset_set(&b, i) == 0, "set");
            ref[i] = 1;
            break;
        case 1:
            CHECK(dyn_bitset_flip(&b, i) == 0, "flip");
            ref[i] ^= 1;
            break;
        case 2:
            dyn_bitset_clear(&b, i);
            ref[i] = 0;
            break;
        default:
            CHECK(dyn_bitset_get(&b, i) == ref[i], "get %u", i);
        }
    }
    {
        uint64_t cnt = 0;
        uint32_t i;
        int64_t nx = dyn_bitset_next_set(&b, 0);
        for (i = 0; i < 70000; i++) {
            cnt += ref[i];
            if (ref[i]) {
                CHECK(nx == (int64_t)i, "nextSet walk %u got %lld", i, (long long)nx);
                nx = dyn_bitset_next_set(&b, i + 1);
            }
        }
        CHECK(nx == -1, "nextSet ends with -1");
        CHECK(dyn_bitset_count(&b) == cnt, "count");
        CHECK(dyn_bitset_next_set(&b, 1u << 30) == -1, "nextSet past the end");
    }
    {
        dyn_bitset_t o;
        uint32_t i;
        static unsigned char ro[70000];
        int op;
        for (op = 0; op < 3; op++) {
            dyn_bitset_t a;
            dyn_bitset_init(&a);
            dyn_bitset_init(&o);
            memset(ro, 0, sizeof(ro));
            for (i = 0; i < 2000; i++) {
                uint32_t x = (uint32_t)(rnd() % 40000), y = (uint32_t)(rnd() % 70000);
                dyn_bitset_set(&a, x);
                dyn_bitset_set(&o, y);
                ro[y] = 1;
            }
            {
                unsigned char ra[70000];
                for (i = 0; i < 70000; i++)
                    ra[i] = (unsigned char)dyn_bitset_get(&a, i);
                CHECK(dyn_bitset_combine(&a, &o, op) == 0, "combine");
                for (i = 0; i < 70000; i++) {
                    int want = op == DYN_BITSET_AND ? (ra[i] & ro[i]) : op == DYN_BITSET_OR ? (ra[i] | ro[i])
                                                                                            : (ra[i] ^ ro[i]);
                    if (dyn_bitset_get(&a, i) != want) {
                        CHECK(0, "combine op %d bit %u", op, i);
                        break;
                    }
                }
            }
            dyn_bitset_free(&a);
            dyn_bitset_free(&o);
        }
    }
    dyn_bitset_free(&b);
    printf("bitset ok\n");
}

static void test_uf(void)
{
    enum { N = 3000 };
    static uint32_t lab[N];
    dyn_uf_t u;
    uint32_t i, sets = N, roots;
    int step;
    CHECK(dyn_uf_init(&u, N) == 0, "uf init");
    for (i = 0; i < N; i++)
        lab[i] = i;
    for (step = 0; step < 6000; step++) {
        uint32_t x = (uint32_t)(rnd() % N), y = (uint32_t)(rnd() % N);
        int want = lab[x] != lab[y];
        CHECK(dyn_uf_union(&u, x, y) == want, "union %u %u", x, y);
        if (want) {
            uint32_t from = lab[y], to = lab[x], k;
            for (k = 0; k < N; k++)
                if (lab[k] == from)
                    lab[k] = to;
            sets--;
        }
        if (step % 50 == 0) {
            uint32_t a = (uint32_t)(rnd() % N), b = (uint32_t)(rnd() % N);
            CHECK((dyn_uf_find(&u, a) == dyn_uf_find(&u, b)) == (lab[a] == lab[b]), "connected");
        }
    }
    CHECK(u.sets == sets, "set count %u vs %u", u.sets, sets);
    CHECK(dyn_uf_validate(&u, &roots) == 0 && roots == sets, "validate: a forest with one root per set");
    u.parent[0] = 1;
    u.parent[1] = 0;
    CHECK(dyn_uf_validate(&u, &roots) == 1, "validate refuses a cycle");
    dyn_uf_free(&u);
    CHECK(dyn_uf_init(&u, 0) == 0 && u.sets == 0, "empty union-find");
    dyn_uf_free(&u);
    printf("unionfind ok\n");
}

static void test_fenwick_segtree(void)
{
    enum { N = 777 };
    double ref[N];
    dyn_fenwick_t f;
    dyn_segtree_t s[3];
    int step, op;
    uint32_t i;
    CHECK(dyn_fenwick_init(&f, N) == 0, "fenwick init");
    for (op = 0; op < 3; op++)
        CHECK(dyn_segtree_init(&s[op], N, op) == 0, "segtree init");
    for (i = 0; i < N; i++)
        ref[i] = 0;
    for (op = 0; op < 3; op++)
        for (i = 0; i < N; i++)
            dyn_segtree_set(&s[op], i, 0);
    for (step = 0; step < 20000; step++) {
        uint32_t a = (uint32_t)(rnd() % N), b = (uint32_t)(rnd() % N);
        double d = (double)(int)(rnd() % 2001) - 1000;
        if (rnd() % 2) {
            dyn_fenwick_add(&f, a, d);
            ref[a] += d;
            for (op = 0; op < 3; op++)
                dyn_segtree_set(&s[op], a, ref[a]);
        } else {
            double sum = 0, mn = HUGE_VAL, mx = -HUGE_VAL;
            uint32_t lo = a < b ? a : b, hi = a < b ? b : a, k;
            for (k = lo; k <= hi; k++) {
                sum += ref[k];
                mn = ref[k] < mn ? ref[k] : mn;
                mx = ref[k] > mx ? ref[k] : mx;
            }
            CHECK(dyn_fenwick_range(&f, lo, hi) == sum, "fenwick range");
            CHECK(dyn_segtree_query(&s[DYN_SEG_SUM], lo, hi) == sum, "seg sum");
            CHECK(dyn_segtree_query(&s[DYN_SEG_MIN], lo, hi) == mn, "seg min");
            CHECK(dyn_segtree_query(&s[DYN_SEG_MAX], lo, hi) == mx, "seg max");
            {
                double p = 0;
                for (k = 0; k <= hi; k++)
                    p += ref[k];
                CHECK(dyn_fenwick_prefix(&f, hi) == p, "fenwick prefix");
            }
        }
    }
    CHECK(dyn_fenwick_range(&f, 5, 4) == 0.0, "an inverted range is empty");
    CHECK(dyn_segtree_query(&s[DYN_SEG_MIN], 5, 4) == HUGE_VAL, "an inverted range is the identity");
    dyn_fenwick_free(&f);
    for (op = 0; op < 3; op++)
        dyn_segtree_free(&s[op]);
    printf("fenwick/segtree ok\n");
}

static void test_circbuf(void)
{
    dyn_circbuf_t c;
    uint32_t cap = 37, i;
    uint64_t ref[64];
    uint32_t rn = 0;
    int step;
    CHECK(dyn_circbuf_init(&c, cap, rel, NULL) == 0, "circbuf init");
    for (step = 0; step < 5000; step++) {
        dyn_cell_t v = mk(), old;
        if (step % 2) {
            if (dyn_circbuf_push(&c, &v, &old))
                drop(&old);
        } else {
            CHECK(dyn_circbuf_push(&c, &v, NULL) == (rn == cap), "push reports an overwrite");
        }
        if (rn < cap)
            ref[rn++] = v.w[0];
        else {
            memmove(ref, ref + 1, (cap - 1) * sizeof(ref[0]));
            ref[cap - 1] = v.w[0];
        }
        CHECK(c.count == rn, "count");
        for (i = 0; i < rn; i += 7)
            CHECK(dyn_circbuf_at(&c, i)->w[0] == ref[i], "at %u", i);
        CHECK(dyn_circbuf_at(&c, rn) == NULL, "past the end");
    }
    dyn_circbuf_free(&c);
    CHECK(cells_live() == 0, "an overwritten slot and the remainder are released exactly once");
    printf("circbuf ok\n");
}

static void test_deque_list(void)
{
    static uint64_t ref[20000];
    long head = 10000, tail = 10000;
    dyn_deque_t d;
    dyn_list_t l;
    int step;
    dyn_deque_init(&d, rel, NULL);
    dyn_list_init(&l, rel, NULL);
    for (step = 0; step < 40000; step++) {
        int op = (int)(rnd() % 5);
        if (op < 2 && tail - head < 9000) {
            dyn_cell_t v = mk(), w = mk();
            w.w[1] = v.w[0];
            if (op == 0) {
                CHECK(dyn_deque_push_back(&d, &v) == 0 && dyn_list_push_back(&l, &w) == 0, "push back");
                ref[tail++] = v.w[0];
            } else {
                CHECK(dyn_deque_push_front(&d, &v) == 0 && dyn_list_push_front(&l, &w) == 0, "push front");
                ref[--head] = v.w[0];
            }
        } else if (op < 4) {
            dyn_cell_t a, b;
            int front = op == 2;
            int rd = front ? dyn_deque_pop_front(&d, &a) : dyn_deque_pop_back(&d, &a);
            int rl = front ? dyn_list_pop_front(&l, &b) : dyn_list_pop_back(&l, &b);
            if (head == tail) {
                CHECK(rd == -1 && rl == -1, "pop on empty");
            } else {
                uint64_t want = front ? ref[head++] : ref[--tail];
                CHECK(rd == 0 && a.w[0] == want, "deque pop");
                CHECK(rl == 0 && b.w[1] == want, "list pop");
                drop(&a);
                drop(&b);
            }
        } else if (tail > head) {
            uint32_t i = (uint32_t)(rnd() % (uint64_t)(tail - head));
            CHECK(dyn_deque_at(&d, i)->w[0] == ref[head + i], "deque at");
        }
        CHECK(d.count == (uint32_t)(tail - head) && l.length == (uint32_t)(tail - head), "lengths");
        if (head < 100 || tail > 19900) {
            long n = tail - head;
            memmove(ref + 10000 - n / 2, ref + head, (size_t)n * sizeof(ref[0]));
            head = 10000 - n / 2;
            tail = head + n;
        }
    }
    CHECK(d.cap >= d.count && (d.cap & (d.cap - 1)) == 0, "deque capacity stays a power of two");
    dyn_deque_free(&d);
    dyn_list_free(&l);
    CHECK(cells_live() == 0, "deque and list release what they hold");
    printf("deque/list ok\n");
}

static void test_bloom(void)
{
    dyn_bloom_t b;
    char key[32];
    int i, k, fp = 0;
    for (k = 1; k <= 12; k += 11) {
        CHECK(dyn_bloom_init(&b, 100000, (uint32_t)k) == 0, "bloom init");
        for (i = 0; i < 8000; i++) {
            int n = snprintf(key, sizeof key, "key-%d", i);
            dyn_bloom_add(&b, key, (size_t)n);
        }
        for (i = 0; i < 8000; i++) {
            int n = snprintf(key, sizeof key, "key-%d", i);
            CHECK(dyn_bloom_may_contain(&b, key, (size_t)n), "no false negative (k=%d)", k);
        }
        fp = 0;
        for (i = 0; i < 20000; i++) {
            int n = snprintf(key, sizeof key, "other-%d", i);
            fp += dyn_bloom_may_contain(&b, key, (size_t)n);
        }
        {
            double theory = pow(1.0 - exp(-(double)k * 8000.0 / 100000.0), (double)k);
            printf("bloom k=%d: false positives %d of 20000 (%.2f%%), theory %.2f%%\n", k, fp, fp / 200.0, theory * 100);
            CHECK(fp / 20000.0 < 2.0 * theory + 0.001, "false-positive rate within 2x of theory (k=%d)", k);
        }
        dyn_bloom_free(&b);
    }
    printf("bloom ok\n");
}

static void test_trie(void)
{
    enum { NK = 3000 };
    static char keys[NK][12];
    static size_t lens[NK];
    static unsigned char in[NK];
    dyn_trie_t t;
    int i, step;
    CHECK(dyn_trie_init(&t) == 0, "trie init");
    for (i = 0; i < NK; i++) {
        size_t n = (size_t)(rnd() % 11), j;
        for (j = 0; j < n; j++)
            keys[i][j] = (char)("abc\xff\x01"[rnd() % 5]);
        lens[i] = n;
        in[i] = 0;
    }
    for (step = 0; step < 30000; step++) {
        int k = (int)(rnd() % NK), j, want;
        uint32_t cnt = 0;
        switch (rnd() % 3) {
        case 0:
            CHECK(dyn_trie_insert(&t, keys[k], lens[k]) == 0, "insert");
            for (j = 0; j < NK; j++)
                if (lens[j] == lens[k] && !memcmp(keys[j], keys[k], lens[k]))
                    in[j] = 1;
            break;
        case 1:
            want = in[k];
            CHECK(dyn_trie_delete(&t, keys[k], lens[k]) == want, "delete");
            for (j = 0; j < NK; j++)
                if (lens[j] == lens[k] && !memcmp(keys[j], keys[k], lens[k]))
                    in[j] = 0;
            break;
        default:
            CHECK(dyn_trie_has(&t, keys[k], lens[k]) == in[k], "has");
            {
                size_t best = 0, got = dyn_trie_longest_prefix(&t, keys[k], lens[k]);
                for (j = 0; j < NK; j++)
                    if (in[j] && lens[j] <= lens[k] && lens[j] > best && !memcmp(keys[j], keys[k], lens[j]))
                        best = lens[j];
                CHECK(got == best, "longest prefix %zu vs %zu", got, best);
            }
        }
        if (step % 5000 == 0) {
            int u;
            for (j = 0; j < NK; j++) {
                for (u = 0; u < j; u++)
                    if (lens[u] == lens[j] && !memcmp(keys[u], keys[j], lens[j]))
                        break;
                if (u == j && in[j])
                    cnt++;
            }
            CHECK(t.count == cnt, "count %u vs %u", t.count, cnt);
        }
    }
    dyn_trie_free(&t);
    printf("trie ok\n");
}

static int cmp_calls, abort_at;
static int cmp_cell(void* ud, const dyn_cell_t* a, const dyn_cell_t* b)
{
    (void)ud;
    if (abort_at && ++cmp_calls == abort_at)
        return DYN_HEAP_ABORT;
    return a->w[1] < b->w[1] ? -1 : a->w[1] > b->w[1];
}
static int u64cmp(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static void test_heap(void)
{
    dyn_heap_t h;
    static uint64_t ref[5000];
    size_t rn = 0;
    int step;
    dyn_heap_init(&h);
    for (step = 0; step < 30000; step++) {
        if (rnd() % 3 && rn < 5000) {
            dyn_cell_t v = mk();
            v.w[1] = rnd() % 1000;
            CHECK(dyn_heap_reserve(&h, h.count + 1) == 0, "reserve");
            h.items[h.count++] = v;
            CHECK(dyn_heap_sift_up(&h, h.count - 1, cmp_cell, NULL) == 0, "sift up");
            ref[rn++] = v.w[1];
        } else if (rn) {
            dyn_cell_t top = h.items[0];
            size_t i, mi = 0;
            for (i = 1; i < rn; i++)
                if (ref[i] < ref[mi])
                    mi = i;
            CHECK(top.w[1] == ref[mi], "pop yields the minimum");
            ref[mi] = ref[--rn];
            h.items[0] = h.items[--h.count];
            if (h.count)
                CHECK(dyn_heap_sift_down(&h, 0, cmp_cell, NULL) == 0, "sift down");
            drop(&top);
        }
    }
    {
        dyn_cell_t snap[64];
        uint32_t i;
        while (h.count > 40) {
            dyn_cell_t top = h.items[0];
            h.items[0] = h.items[--h.count];
            dyn_heap_sift_down(&h, 0, cmp_cell, NULL);
            drop(&top);
        }
        memcpy(snap, h.items, h.count * sizeof(dyn_cell_t));
        snap[0].w[1] = 5000;
        h.items[0] = snap[0];
        cmp_calls = 0;
        abort_at = 2;
        CHECK(dyn_heap_sift_down(&h, 0, cmp_cell, NULL) == -1, "an aborted comparator stops the sift");
        CHECK(memcmp(snap, h.items, h.count * sizeof(dyn_cell_t)) == 0, "and an aborted sift-down moved nothing");
        abort_at = 0;
        CHECK(dyn_heap_sift_down(&h, 0, cmp_cell, NULL) == 0, "the same sift completes without the abort");
        {
            uint64_t keys[64], got[64];
            for (i = 0; i < h.count; i++)
                keys[i] = h.items[i].w[1];
            qsort(keys, h.count, sizeof(uint64_t), u64cmp);
            for (i = 0; h.count; i++) {
                dyn_cell_t top = h.items[0];
                got[i] = top.w[1];
                h.items[0] = h.items[--h.count];
                if (h.count)
                    dyn_heap_sift_down(&h, 0, cmp_cell, NULL);
                drop(&top);
            }
            CHECK(memcmp(keys, got, i * sizeof(uint64_t)) == 0, "a heap drains in order after the abort");
        }
    }
    dyn_heap_free(&h, rel, NULL);
    CHECK(cells_live() == 0, "heap releases what it holds");
    printf("heap ok\n");
}

static void test_lru(void)
{
    enum { CAP = 50,
        KEYS = 200 };
    dyn_lru_t L;
    int order[KEYS], on = 0, step, i;
    char key[16];
    CHECK(dyn_lru_init(&L, CAP, rel, NULL) == 0, "lru init");
    for (step = 0; step < 50000; step++) {
        int k = (int)(rnd() % KEYS), n = snprintf(key, sizeof key, "k%d", k), pos;
        for (pos = 0; pos < on && order[pos] != k; pos++)
            ;
        if (rnd() % 2) {
            dyn_cell_t v = mk();
            dyn_lru_node_t* victim;
            CHECK(dyn_lru_set(&L, key, (size_t)n, &v, 0, &victim) == 0, "set");
            if (pos < on) {
                memmove(order + 1, order, (size_t)pos * sizeof(int));
                order[0] = k;
                CHECK(!victim, "a replace evicts nothing");
            } else {
                memmove(order + 1, order, (size_t)on * sizeof(int));
                order[0] = k;
                if (++on > CAP) {
                    int want = order[--on];
                    char wk[16];
                    int wn = snprintf(wk, sizeof wk, "k%d", want);
                    CHECK(victim && victim->klen == (size_t)wn && !memcmp(victim->key, wk, (size_t)wn), "the least recent key is evicted");
                } else
                    CHECK(!victim, "under capacity evicts nothing");
            }
            if (victim)
                dyn_lru_node_free(&L, victim);
        } else {
            dyn_lru_node_t* f = dyn_lru_find(&L, key, (size_t)n);
            CHECK((f != NULL) == (pos < on), "find");
            if (f && rnd() % 2) {
                dyn_lru_touch(&L, f);
                memmove(order + 1, order, (size_t)pos * sizeof(int));
                order[0] = k;
            } else if (f) {
                dyn_lru_detach(&L, f);
                dyn_lru_node_free(&L, f);
                memmove(order + pos, order + pos + 1, (size_t)(on - pos - 1) * sizeof(int));
                on--;
            }
        }
        CHECK(L.count == (uint32_t)on, "count");
    }
    {
        dyn_lru_node_t* n = L.head;
        for (i = 0; i < on && n; i++, n = n->next) {
            char wk[16];
            int wn = snprintf(wk, sizeof wk, "k%d", order[i]);
            CHECK(n->klen == (size_t)wn && !memcmp(n->key, wk, (size_t)wn), "recency order at %d", i);
        }
        CHECK(i == on && !n, "the list holds exactly the live keys");
    }
    {
        dyn_lru_node_t* f = L.head;
        dyn_cell_t v;
        f->expires = 1;
        CHECK(dyn_lru_expired(f, dyn_lru_now_ms()), "a past expiry is expired");
        CHECK(!dyn_lru_expired(L.tail, dyn_lru_now_ms()) || L.tail == f, "no TTL never expires");
        CHECK(dyn_lru_expiry(0) == 0, "ttl 0 means none");
        CHECK(dyn_lru_expiry(UINT64_MAX) == (uint64_t)INT64_MAX, "a huge ttl saturates");
        dyn_lru_detach(&L, f);
        v = dyn_lru_node_take(f);
        drop(&v);
    }
    dyn_lru_free(&L);
    CHECK(cells_live() == 0, "lru releases what it holds");
    printf("lru ok\n");
}

typedef struct {
    double s;
    char k[8];
    uint32_t kn;
} zref;
static int zcmp(const void* a, const void* b)
{
    const zref *x = (const zref*)a, *y = (const zref*)b;
    uint32_t m;
    int c;
    if (x->s != y->s)
        return x->s < y->s ? -1 : 1;
    m = x->kn < y->kn ? x->kn : y->kn;
    c = memcmp(x->k, y->k, m);
    return c ? c : (int)x->kn - (int)y->kn;
}

static void test_zsl(void)
{
    enum { NK = 400 };
    dyn_zsl_t z;
    static zref ref[NK];
    int rn = 0, step;
    CHECK(dyn_zsl_init(&z, rel, NULL) == 0, "zsl init");
    for (step = 0; step < 20000; step++) {
        char k[8];
        uint32_t kn = (uint32_t)snprintf(k, sizeof k, "m%d", (int)(rnd() % NK));
        dyn_zsl_node* x = dyn_zsl_find(&z, k, kn);
        int i, at = -1;
        for (i = 0; i < rn; i++)
            if (ref[i].kn == kn && !memcmp(ref[i].k, k, kn))
                at = i;
        CHECK((x != NULL) == (at >= 0), "find");
        if (rnd() % 3 == 0 && x) {
            dyn_zsl_drop(&z, x);
            ref[at] = ref[--rn];
        } else if (!x) {
            dyn_cell_t v = mk();
            double s = (double)(rnd() % 50);
            CHECK(dyn_zsl_insert(&z, s, k, kn, &v, NULL) == 0, "insert");
            ref[rn].s = s;
            memcpy(ref[rn].k, k, kn);
            ref[rn].kn = kn;
            rn++;
        }
        if (step % 97 == 0 && rn) {
            int r;
            qsort(ref, (size_t)rn, sizeof(zref), zcmp);
            CHECK(z.len == (uint32_t)rn, "length");
            for (r = 0; r < rn; r += 11) {
                dyn_zsl_node* n = dyn_zsl_at(&z, (uint32_t)r + 1);
                CHECK(n && n->klen == ref[r].kn && !memcmp(n->key, ref[r].k, ref[r].kn) && n->score == ref[r].s, "rank %d", r + 1);
                CHECK(n && dyn_zsl_rank(&z, n) == (uint32_t)r + 1, "rank of the node at %d", r + 1);
            }
        }
    }
    dyn_zsl_destroy(&z);
    CHECK(cells_live() == 0, "the scored skip list releases what it holds");
    printf("zsl ok\n");
}

static void test_db_cores(void)
{
    dyn_topk_t tk;
    dyn_cuckoo_t cf;
    dyn_iheap_t ih;
    dyn_invidx_t ii;
    dyn_chash_t ch;
    CHECK(dyn_topk_init(&tk, 16) == 0 && tk.k == 16, "topk init");
    dyn_topk_free(&tk);
    CHECK(dyn_cuckoo_init(&cf, 64) == 0 && cf.mask == 63, "cuckoo init");
    dyn_cuckoo_free(&cf);
    dyn_iheap_init(&ih, 0);
    dyn_iheap_free(&ih);
    dyn_invidx_init(&ii, 1.2, 0.75);
    CHECK(ii.k1 == 1.2 && ii.b == 0.75 && ii.tepoch == 1, "invidx init");
    dyn_invidx_free(&ii);
    dyn_chash_init(&ch, 8);
    CHECK(ch.replicas == 8, "chash init");
    dyn_chash_free(&ch);
    printf("db cores ok\n");
}

int main(void)
{
    test_bitset();
    test_uf();
    test_fenwick_segtree();
    test_circbuf();
    test_deque_list();
    test_bloom();
    test_trie();
    test_heap();
    test_lru();
    test_zsl();
    test_db_cores();
    CHECK(ds_live_allocs == 0, "every allocation was freed (%ld live)", ds_live_allocs);
    CHECK(cells_live() == 0, "every cell was released (%ld live)", cells_live());
    CHECK(released == (long)next_serial - 1, "releases match cells made (%ld of %ld)", released, (long)next_serial - 1);
    printf("%s: %ld checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
