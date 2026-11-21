#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-indexed-heap.h"

static int dyn_iheap_before(const dyn_iheap_t* q, uint32_t a, uint32_t b)
{
    const dyn_iheap_ent *x = &q->h[a], *y = &q->h[b];
    if (x->p != y->p)
        return q->max ? x->p > y->p : x->p < y->p;
    return x->id < y->id;
}

static void dyn_iheap_swap(dyn_iheap_t* q, uint32_t a, uint32_t b)
{
    dyn_iheap_ent t = q->h[a];
    dyn_ds_ient* s;
    q->h[a] = q->h[b];
    q->h[b] = t;
    s = dyn_ds_imap_find(&q->pos, q->h[a].id);
    if (s)
        s->v = a;
    s = dyn_ds_imap_find(&q->pos, q->h[b].id);
    if (s)
        s->v = b;
}

static uint32_t dyn_iheap_up(dyn_iheap_t* q, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!dyn_iheap_before(q, i, p))
            break;
        dyn_iheap_swap(q, i, p);
        i = p;
    }
    return i;
}

static void dyn_iheap_down(dyn_iheap_t* q, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < q->n && dyn_iheap_before(q, l, m))
            m = l;
        if (r < q->n && dyn_iheap_before(q, r, m))
            m = r;
        if (m == i)
            return;
        dyn_iheap_swap(q, i, m);
        i = m;
    }
}

int dyn_iheap_push(dyn_iheap_t* q, uint32_t id, double p)
{
    dyn_ds_ient* s = dyn_ds_imap_find(&q->pos, id);
    if (s) {
        uint32_t i = s->v;
        if (q->h[i].p == p)
            return 0;
        q->h[i].p = p;
        dyn_iheap_down(q, dyn_iheap_up(q, i));
        return 0;
    }
    if (q->n == q->cap) {
        uint32_t nc = q->cap ? q->cap * 2 : 16;
        dyn_iheap_ent* nh;
        if (nc > DYN_DS_MAX_ITEMS)
            return -1;
        nh = (dyn_iheap_ent*)DYN_DS_REALLOC(q->h, sizeof(dyn_iheap_ent) * nc);
        if (!nh)
            return -1;
        q->h = nh;
        q->cap = nc;
    }
    if (!dyn_ds_imap_put(&q->pos, id, q->n))
        return -1;
    q->h[q->n].p = p;
    q->h[q->n].id = id;
    q->n++;
    dyn_iheap_up(q, q->n - 1);
    return 0;
}

void dyn_iheap_remove_at(dyn_iheap_t* q, uint32_t i)
{
    dyn_ds_ient* s = dyn_ds_imap_find(&q->pos, q->h[i].id);
    uint32_t last = q->n - 1;
    if (s)
        dyn_ds_imap_remove(&q->pos, s);
    q->n--;
    if (i == last)
        return;
    q->h[i] = q->h[last];
    s = dyn_ds_imap_find(&q->pos, q->h[i].id);
    if (s)
        s->v = i;
    dyn_iheap_down(q, dyn_iheap_up(q, i));
}

void dyn_iheap_init(dyn_iheap_t* q, int max)
{
    memset(q, 0, sizeof(*q));
    q->max = max;
    dyn_ds_imap_init(&q->pos);
}

void dyn_iheap_free(dyn_iheap_t* q)
{
    DYN_DS_FREE(q->h);
    q->h = NULL;
    dyn_ds_imap_free(&q->pos);
}
