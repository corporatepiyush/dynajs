#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-kdtree.h"

static double dyn_kdtree_coord(const dyn_kdtree_t* t, uint32_t idx, uint32_t dim)
{
    return t->pts[(size_t)idx * t->d + dim];
}

static void dyn_kdtree_select(dyn_kdtree_t* t, uint32_t lo, uint32_t hi, uint32_t k, uint32_t dim)
{
    uint32_t* p = t->perm;
    while (hi - lo > 1) {
        uint32_t piv = lo + (uint32_t)(ds_splitmix64(&t->rng) % (hi - lo));
        double pv = dyn_kdtree_coord(t, p[piv], dim);
        uint32_t lt = lo, i = lo, gt = hi;
        while (i < gt) {
            double v = dyn_kdtree_coord(t, p[i], dim);
            if (v < pv) {
                uint32_t x = p[lt];
                p[lt++] = p[i];
                p[i++] = x;
            } else if (v > pv) {
                uint32_t x = p[--gt];
                p[gt] = p[i];
                p[i] = x;
            } else
                i++;
        }
        if (k < lt)
            hi = lt;
        else if (k >= gt)
            lo = gt;
        else
            return;
    }
}

static int32_t dyn_kdtree_build(dyn_kdtree_t* t, uint32_t lo, uint32_t hi)
{
    int32_t id;
    dyn_kdtree_node* nd;
    if (t->nn == t->ncap) {
        uint32_t nc = t->ncap ? t->ncap * 2 : 64;
        dyn_kdtree_node* nn = (dyn_kdtree_node*)DYN_DS_REALLOC(t->nodes, sizeof(dyn_kdtree_node) * nc);
        if (!nn)
            return -2;
        t->nodes = nn;
        t->ncap = nc;
    }
    id = (int32_t)t->nn++;
    nd = &t->nodes[id];
    nd->lo = lo;
    nd->hi = hi;
    nd->l = nd->r = -1;
    nd->dim = 0;
    nd->split = 0;
    if (hi - lo > DYN_KDTREE_LEAF) {
        uint32_t best = 0, dim, i, mid = lo + (hi - lo) / 2;
        double spread = -1;
        int32_t l, r;
        for (dim = 0; dim < t->d; dim++) {
            double mn = HUGE_VAL, mx = -HUGE_VAL;
            for (i = lo; i < hi; i++) {
                double v = dyn_kdtree_coord(t, t->perm[i], dim);
                if (v < mn)
                    mn = v;
                if (v > mx)
                    mx = v;
            }
            if (mx - mn > spread) {
                spread = mx - mn;
                best = dim;
            }
        }
        if (spread <= 0)
            return id;
        dyn_kdtree_select(t, lo, hi, mid, best);
        t->nodes[id].dim = best;
        t->nodes[id].split = dyn_kdtree_coord(t, t->perm[mid], best);
        l = dyn_kdtree_build(t, lo, mid);
        if (l == -2)
            return -2;
        r = dyn_kdtree_build(t, mid, hi);
        if (r == -2)
            return -2;
        t->nodes[id].l = l;
        t->nodes[id].r = r;
    }
    return id;
}

void dyn_kdtree_free(dyn_kdtree_t* t)
{
    DYN_DS_FREE(t->pts);
    DYN_DS_FREE(t->perm);
    DYN_DS_FREE(t->nodes);
    memset(t, 0, sizeof(*t));
}

int dyn_kdtree_init(dyn_kdtree_t* t, double* pts, uint32_t n, uint32_t d)
{
    uint32_t i;
    memset(t, 0, sizeof(*t));
    t->d = d;
    t->n = n;
    t->pts = pts;
    t->rng = DYN_DS_FIXED_SEED;
    t->perm = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)n + 1));
    if (!t->perm)
        return -1;
    for (i = 0; i < n; i++)
        t->perm[i] = i;
    if (n && dyn_kdtree_build(t, 0, n) == -2)
        return -1;
    return 0;
}

static int dyn_kdtree_hit_worse(const dyn_kdtree_hit* a, const dyn_kdtree_hit* b)
{
    return a->d2 > b->d2 || (a->d2 == b->d2 && a->i > b->i);
}

static void dyn_kdtree_knn_push(dyn_kdtree_knn_ctx* c, double d2, uint32_t idx)
{
    dyn_kdtree_hit x = { d2, idx };
    uint32_t i;
    if (c->n < c->k) {
        i = c->n++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!dyn_kdtree_hit_worse(&x, &c->h[p]))
                break;
            c->h[i] = c->h[p];
            i = p;
        }
        c->h[i] = x;
        return;
    }
    if (!dyn_kdtree_hit_worse(&c->h[0], &x))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const dyn_kdtree_hit* mv = &x;
        if (l < c->n && dyn_kdtree_hit_worse(&c->h[l], mv)) {
            m = l;
            mv = &c->h[l];
        }
        if (r < c->n && dyn_kdtree_hit_worse(&c->h[r], mv))
            m = r;
        if (m == i)
            break;
        c->h[i] = c->h[m];
        i = m;
    }
    c->h[i] = x;
}

static double dyn_kdtree_d2(const dyn_kdtree_t* t, const double* q, uint32_t idx)
{
    const double* p = &t->pts[(size_t)idx * t->d];
    double s = 0;
    uint32_t j;
    for (j = 0; j < t->d; j++) {
        double x = p[j] - q[j];
        s += x * x;
    }
    return s;
}

void dyn_kdtree_knn(dyn_kdtree_knn_ctx* c, int32_t ni)
{
    const dyn_kdtree_node* nd = &c->t->nodes[ni];
    double diff;
    int32_t first, second;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            uint32_t idx = c->t->perm[i];
            dyn_kdtree_knn_push(c, dyn_kdtree_d2(c->t, c->q, idx), idx);
        }
        return;
    }
    diff = c->q[nd->dim] - nd->split;
    first = diff < 0 ? nd->l : nd->r;
    second = diff < 0 ? nd->r : nd->l;
    dyn_kdtree_knn(c, first);
    if (c->n < c->k || diff * diff <= c->h[0].d2)
        dyn_kdtree_knn(c, second);
}

int dyn_kdtree_hit_cmp(const void* a, const void* b)
{
    const dyn_kdtree_hit *x = (const dyn_kdtree_hit*)a, *y = (const dyn_kdtree_hit*)b;
    if (x->d2 != y->d2)
        return x->d2 < y->d2 ? -1 : 1;
    return (x->i > y->i) - (x->i < y->i);
}

static void dyn_kdtree_list_add(dyn_kdtree_list* l, double d2, uint32_t i)
{
    if (l->oom)
        return;
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 32;
        dyn_kdtree_hit* nh = (dyn_kdtree_hit*)DYN_DS_REALLOC(l->h, sizeof(dyn_kdtree_hit) * nc);
        if (!nh) {
            l->oom = 1;
            return;
        }
        l->h = nh;
        l->cap = nc;
    }
    l->h[l->n].d2 = d2;
    l->h[l->n].i = i;
    l->n++;
}

void dyn_kdtree_radius(const dyn_kdtree_t* t, int32_t ni, const double* q, double r2, dyn_kdtree_list* out)
{
    const dyn_kdtree_node* nd = &t->nodes[ni];
    double diff;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            double d2 = dyn_kdtree_d2(t, q, t->perm[i]);
            if (d2 <= r2)
                dyn_kdtree_list_add(out, d2, t->perm[i]);
        }
        return;
    }
    diff = q[nd->dim] - nd->split;
    if (diff <= 0 || diff * diff <= r2)
        dyn_kdtree_radius(t, nd->l, q, r2, out);
    if (diff >= 0 || diff * diff <= r2)
        dyn_kdtree_radius(t, nd->r, q, r2, out);
}

void dyn_kdtree_box(const dyn_kdtree_t* t, int32_t ni, const double* lo, const double* hi, dyn_kdtree_list* out)
{
    const dyn_kdtree_node* nd = &t->nodes[ni];
    if (nd->l < 0) {
        uint32_t i, j;
        for (i = nd->lo; i < nd->hi; i++) {
            const double* p = &t->pts[(size_t)t->perm[i] * t->d];
            for (j = 0; j < t->d; j++)
                if (p[j] < lo[j] || p[j] > hi[j])
                    break;
            if (j == t->d)
                dyn_kdtree_list_add(out, (double)t->perm[i], t->perm[i]);
        }
        return;
    }
    if (lo[nd->dim] <= nd->split)
        dyn_kdtree_box(t, nd->l, lo, hi, out);
    if (hi[nd->dim] >= nd->split)
        dyn_kdtree_box(t, nd->r, lo, hi, out);
}
