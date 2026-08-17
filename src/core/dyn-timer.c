/* dyn-timer -- see dyn-timer.h. */
#include "dyn-timer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <pthread.h>

/* The timebase is process-constant but a syscall to learn: check-then-act on
   a plain static here was a data race (a thread could read denom != 0 while
   numer was still 0 and silently compute 0). */
static mach_timebase_info_data_t dyn_tb;
static void dyn_tb_init(void) { mach_timebase_info(&dyn_tb); }
static pthread_once_t dyn_tb_once = PTHREAD_ONCE_INIT;
#endif

typedef struct {
    uint64_t deadline;
    uint64_t seq;          /* FIFO tiebreaker for equal deadlines */
    dyn_timer_cb cb;
    void *arg;
    dyn_timer_id id;
} tnode_t;

/* Slot for an id: where it sits in the heap, plus a generation so a stale id
 * from a freed slot cannot cancel whatever reused it. */
typedef struct {
    uint32_t pos;          /* heap index, or NO_POS when free */
    uint32_t gen;
    uint32_t next_free;
} tslot_t;

#define NO_POS 0xffffffffu

struct dyn_timers {
    tnode_t *h;
    size_t n, cap;
    tslot_t *slots;
    uint32_t nslots, slot_cap, free_head;
    uint64_t seq;
};

uint64_t dyn_timer_now_ms(void)
{
#if defined(__APPLE__)
    pthread_once(&dyn_tb_once, dyn_tb_init);
    return (mach_absolute_time() * dyn_tb.numer / dyn_tb.denom) / 1000000ull;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
#else
    return (uint64_t)time(NULL) * 1000ull;
#endif
}

/* id is (index+1) in the low 20 bits and the generation above, so a stale id
 * is detectable rather than silently aliasing a reused slot. */
#define ID_MAKE(idx, gen) (((dyn_timer_id)((gen) & 0xfffu) << 20) | ((idx) + 1u))
#define ID_INDEX(id)      (((id) & 0xfffffu) - 1u)
#define ID_GEN(id)        (((id) >> 20) & 0xfffu)

dyn_timers_t *dyn_timers_new(void)
{
    dyn_timers_t *t = (dyn_timers_t *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->free_head = NO_POS;
    return t;
}

void dyn_timers_free(dyn_timers_t *t)
{
    if (!t)
        return;
    free(t->h);
    free(t->slots);
    free(t);
}

size_t dyn_timers_count(const dyn_timers_t *t) { return t ? t->n : 0; }

static int node_less(const tnode_t *a, const tnode_t *b)
{
    if (a->deadline != b->deadline)
        return a->deadline < b->deadline;
    return a->seq < b->seq;
}

static void place(dyn_timers_t *t, size_t i, const tnode_t *v)
{
    t->h[i] = *v;
    t->slots[ID_INDEX(v->id)].pos = (uint32_t)i;
}

static void sift_up(dyn_timers_t *t, size_t i)
{
    tnode_t v = t->h[i];
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!node_less(&v, &t->h[p]))
            break;
        place(t, i, &t->h[p]);
        i = p;
    }
    place(t, i, &v);
}

static void sift_down(dyn_timers_t *t, size_t i)
{
    tnode_t v = t->h[i];
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && node_less(&t->h[l], &v)) m = l;
        if (r < t->n && node_less(&t->h[r], (m == i) ? &v : &t->h[l])) m = r;
        if (m == i)
            break;
        place(t, i, &t->h[m]);
        i = m;
    }
    place(t, i, &v);
}

static int grow_heap(dyn_timers_t *t)
{
    size_t nc = t->cap ? t->cap * 2 : 16;
    tnode_t *nh = (tnode_t *)realloc(t->h, nc * sizeof(tnode_t));
    if (!nh)
        return -1;
    t->h = nh;
    t->cap = nc;
    return 0;
}

static uint32_t slot_alloc(dyn_timers_t *t)
{
    uint32_t idx;
    if (t->free_head != NO_POS) {
        idx = t->free_head;
        t->free_head = t->slots[idx].next_free;
        return idx;
    }
    if (t->nslots == t->slot_cap) {
        uint32_t nc = t->slot_cap ? t->slot_cap * 2u : 16u;
        tslot_t *ns = (tslot_t *)realloc(t->slots, (size_t)nc * sizeof(tslot_t));
        if (!ns)
            return NO_POS;
        memset(ns + t->slot_cap, 0, (size_t)(nc - t->slot_cap) * sizeof(tslot_t));
        t->slots = ns;
        t->slot_cap = nc;
    }
    idx = t->nslots++;
    return idx;
}

static void slot_release(dyn_timers_t *t, uint32_t idx)
{
    t->slots[idx].pos = NO_POS;
    t->slots[idx].gen = (t->slots[idx].gen + 1u) & 0xfffu;
    t->slots[idx].next_free = t->free_head;
    t->free_head = idx;
}

dyn_timer_id dyn_timer_add(dyn_timers_t *t, uint64_t now, uint64_t delay_ms,
                           dyn_timer_cb cb, void *arg)
{
    uint32_t idx;
    tnode_t v;

    if (!t || !cb)
        return DYN_TIMER_NONE;
    if (t->n == t->cap && grow_heap(t) < 0)
        return DYN_TIMER_NONE;
    idx = slot_alloc(t);
    if (idx == NO_POS)
        return DYN_TIMER_NONE;

    v.deadline = (now + delay_ms < now) ? UINT64_MAX : now + delay_ms;
    v.seq = t->seq++;
    v.cb = cb;
    v.arg = arg;
    v.id = ID_MAKE(idx, t->slots[idx].gen);

    t->n++;
    place(t, t->n - 1, &v);
    sift_up(t, t->n - 1);
    return v.id;
}

/* Remove heap position `i`, keeping the heap and the slot table consistent. */
static void heap_remove_at(dyn_timers_t *t, size_t i)
{
    tnode_t last;
    slot_release(t, ID_INDEX(t->h[i].id));
    t->n--;
    if (i == t->n)
        return;                    /* removed the tail */
    last = t->h[t->n];
    place(t, i, &last);
    sift_down(t, i);
    if (t->slots[ID_INDEX(t->h[i].id)].pos == i)
        sift_up(t, i);             /* sift_down did nothing; may need to rise */
}

int dyn_timer_cancel(dyn_timers_t *t, dyn_timer_id id)
{
    uint32_t idx;
    if (!t || id == DYN_TIMER_NONE)
        return 0;
    idx = ID_INDEX(id);
    if (idx >= t->nslots)
        return 0;
    if (t->slots[idx].pos == NO_POS)
        return 0;                  /* already fired or cancelled */
    if (t->slots[idx].gen != ID_GEN(id))
        return 0;                  /* stale id from a previous generation */
    heap_remove_at(t, t->slots[idx].pos);
    return 1;
}

int dyn_timer_next_timeout(const dyn_timers_t *t, uint64_t now)
{
    uint64_t d;
    if (!t || t->n == 0)
        return -1;
    if (t->h[0].deadline <= now)
        return 0;
    d = t->h[0].deadline - now;
    return (d > (uint64_t)INT32_MAX) ? INT32_MAX : (int)d;
}

int dyn_timer_run(dyn_timers_t *t, uint64_t now)
{
    int ran = 0;
    if (!t)
        return 0;
    /* Re-read the root each pass: a callback may arm or cancel timers, so the
     * heap can change shape under us. */
    while (t->n > 0 && t->h[0].deadline <= now) {
        dyn_timer_cb cb = t->h[0].cb;
        void *arg = t->h[0].arg;
        heap_remove_at(t, 0);
        cb(arg);
        ran++;
    }
    return ran;
}
