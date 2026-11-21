#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-segment-tree.h"

double dyn_segtree_identity(int op)
{
    if (op == DYN_SEG_MIN)
        return HUGE_VAL;
    if (op == DYN_SEG_MAX)
        return -HUGE_VAL;
    return 0.0;
}

int dyn_segtree_init(dyn_segtree_t* s, uint32_t n, int op)
{
    uint32_t i;
    double idv = dyn_segtree_identity(op);
    s->n = n;
    s->op = op;
    s->tree = (double*)DYN_DS_MALLOC((size_t)2 * n * sizeof(double));
    if (!s->tree)
        return -1;
    for (i = 0; i < 2 * n; i++)
        s->tree[i] = idv;
    return 0;
}

void dyn_segtree_free(dyn_segtree_t* s)
{
    DYN_DS_FREE(s->tree);
    s->tree = NULL;
    s->n = 0;
}

void dyn_segtree_set(dyn_segtree_t* s, uint32_t i, double v)
{
    uint32_t pos = i + s->n;
    s->tree[pos] = v;
    switch (s->op) {
    case DYN_SEG_MIN:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a < b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    case DYN_SEG_MAX:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            double a = s->tree[2 * pos], b2 = s->tree[2 * pos + 1];
            s->tree[pos] = a > b2 ? a : b2;
            if (pos == 1)
                break;
        }
        break;
    default:
        for (pos >>= 1; pos >= 1; pos >>= 1) {
            s->tree[pos] = s->tree[2 * pos] + s->tree[2 * pos + 1];
            if (pos == 1)
                break;
        }
        break;
    }
}

double dyn_segtree_query(const dyn_segtree_t* s, uint32_t lo, uint32_t hi)
{
    double res = dyn_segtree_identity(s->op);
    uint32_t l, r;
    if (lo > hi)
        return res;
    l = lo + s->n;
    r = hi + 1 + s->n;
    switch (s->op) {
    case DYN_SEG_MIN:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res < x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res < x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    case DYN_SEG_MAX:
        while (l < r) {
            if (l & 1) {
                double x = s->tree[l++];
                res = res > x ? res : x;
            }
            if (r & 1) {
                double x = s->tree[--r];
                res = res > x ? res : x;
            }
            l >>= 1;
            r >>= 1;
        }
        break;
    default:
        while (l < r) {
            if (l & 1)
                res += s->tree[l++];
            if (r & 1)
                res += s->tree[--r];
            l >>= 1;
            r >>= 1;
        }
        break;
    }
    return res;
}
