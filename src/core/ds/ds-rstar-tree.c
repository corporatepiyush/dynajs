#include "ds-internal.h"
#include "ds-heap.h"

#define DS_RS_MIN 6
#define DS_RS_REINSERT 5

typedef struct {
    double d;
    const ds_rsnode* n;
    uint64_t id;
} ds_rs_qitem;

uint32_t ds_rs_first_hi(const dyn_rset_t* s, double x, int strict)
{
    return ds_cv_first_hi(&s->g, sizeof(ds_span), x, strict);
}

static void ds_rs_box_of(const dyn_rstar_t* t, const ds_rsnode* n, double* b)
{
    unsigned i, d;
    for (d = 0; d < t->dims; d++) {
        b[d] = (double)INFINITY;
        b[t->dims + d] = -(double)INFINITY;
    }
    for (i = 0; i < n->count; i++)
        for (d = 0; d < t->dims; d++) {
            if (n->e[i].b[d] < b[d])
                b[d] = n->e[i].b[d];
            if (n->e[i].b[t->dims + d] > b[t->dims + d])
                b[t->dims + d] = n->e[i].b[t->dims + d];
        }
}

static double ds_rs_area(const dyn_rstar_t* t, const double* b)
{
    double a = 1;
    unsigned d;
    for (d = 0; d < t->dims; d++)
        a *= b[t->dims + d] - b[d];
    return a;
}

static double ds_rs_margin(const dyn_rstar_t* t, const double* b)
{
    double m = 0;
    unsigned d;
    for (d = 0; d < t->dims; d++)
        m += b[t->dims + d] - b[d];
    return m;
}

static double ds_rs_overlap(const dyn_rstar_t* t, const double* a, const double* b)
{
    double o = 1;
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        double lo = a[d] > b[d] ? a[d] : b[d];
        double hi = a[t->dims + d] < b[t->dims + d] ? a[t->dims + d] : b[t->dims + d];
        if (hi <= lo)
            return 0;
        o *= hi - lo;
    }
    return o;
}

static void ds_rs_union(const dyn_rstar_t* t, double* dst, const double* a, const double* b)
{
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        dst[d] = a[d] < b[d] ? a[d] : b[d];
        dst[t->dims + d] = a[t->dims + d] > b[t->dims + d] ? a[t->dims + d] : b[t->dims + d];
    }
}

static int ds_rs_intersects(const dyn_rstar_t* t, const double* a, const double* b)
{
    unsigned d;
    for (d = 0; d < t->dims; d++)
        if (a[t->dims + d] < b[d] || b[t->dims + d] < a[d])
            return 0;
    return 1;
}

static int ds_rs_contains(const dyn_rstar_t* t, const double* outer, const double* inner)
{
    unsigned d;
    for (d = 0; d < t->dims; d++)
        if (inner[d] < outer[d] || inner[t->dims + d] > outer[t->dims + d])
            return 0;
    return 1;
}

dyn_rstar_t* dyn_rstar_new(unsigned dims)
{
    dyn_rstar_t* t;
    if (dims < 1 || dims > DYN_RSTAR_MAX_DIMS)
        return NULL;
    t = (dyn_rstar_t*)DYN_DS_CALLOC(1, sizeof(*t));
    if (!t)
        return NULL;
    t->dims = dims;
    t->root = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
    if (!t->root) {
        DYN_DS_FREE(t);
        return NULL;
    }
    return t;
}

static void ds_rs_free_rec(ds_rsnode* n)
{
    unsigned i;
    if (n->level)
        for (i = 0; i < n->count; i++)
            ds_rs_free_rec(n->e[i].u.child);
    DYN_DS_FREE(n);
}

void dyn_rstar_free(dyn_rstar_t* t)
{
    if (!t)
        return;
    ds_rs_free_rec(t->root);
    DYN_DS_FREE(t);
}

size_t dyn_rstar_size(const dyn_rstar_t* t)
{
    return t->size;
}

unsigned dyn_rstar_height(const dyn_rstar_t* t)
{
    return t->root->level + 1;
}

static unsigned ds_rs_choose(const dyn_rstar_t* t, const ds_rsnode* n, const double* b)
{
    unsigned i, j, best = 0;
    double bu[2 * DYN_RSTAR_MAX_DIMS];
    if (n->level == 1) {
        double best_ov = (double)INFINITY, best_ae = (double)INFINITY, best_a = (double)INFINITY;
        for (i = 0; i < n->count; i++) {
            double ov = 0, a = ds_rs_area(t, n->e[i].b), ae;
            ds_rs_union(t, bu, n->e[i].b, b);
            ae = ds_rs_area(t, bu) - a;
            if (!ds_rs_contains(t, n->e[i].b, b))
                for (j = 0; j < n->count && ov <= best_ov; j++) {
                    double grown;
                    if (j == i)
                        continue;
                    grown = ds_rs_overlap(t, bu, n->e[j].b);
                    if (grown != 0)
                        ov += grown - ds_rs_overlap(t, n->e[i].b, n->e[j].b);
                }
            if (ov < best_ov || (ov == best_ov && (ae < best_ae || (ae == best_ae && a < best_a)))) {
                best_ov = ov;
                best_ae = ae;
                best_a = a;
                best = i;
            }
        }
    } else {
        double best_ae = (double)INFINITY, best_a = (double)INFINITY;
        for (i = 0; i < n->count; i++) {
            double a = ds_rs_area(t, n->e[i].b), ae;
            ds_rs_union(t, bu, n->e[i].b, b);
            ae = ds_rs_area(t, bu) - a;
            if (ae < best_ae || (ae == best_ae && a < best_a)) {
                best_ae = ae;
                best_a = a;
                best = i;
            }
        }
    }
    return best;
}

static void ds_rs_sort(const dyn_rstar_t* t, ds_rsentry* e, unsigned n, unsigned axis, int by_hi)
{
    unsigned i, j;
    unsigned col = by_hi ? t->dims + axis : axis;
    for (i = 1; i < n; i++) {
        ds_rsentry x = e[i];
        double k = x.b[col];
        j = i;
        while (j > 0 && e[j - 1].b[col] > k) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = x;
    }
}

static void ds_rs_group_box(const dyn_rstar_t* t, const ds_rsentry* e, unsigned from, unsigned to, double* b)
{
    unsigned i, d;
    for (d = 0; d < t->dims; d++) {
        b[d] = (double)INFINITY;
        b[t->dims + d] = -(double)INFINITY;
    }
    for (i = from; i < to; i++)
        for (d = 0; d < t->dims; d++) {
            if (e[i].b[d] < b[d])
                b[d] = e[i].b[d];
            if (e[i].b[t->dims + d] > b[t->dims + d])
                b[t->dims + d] = e[i].b[t->dims + d];
        }
}

static ds_rsnode* ds_rs_split(dyn_rstar_t* t, ds_rsnode* n)
{
    unsigned total = n->count, axis, best_axis = 0, k, best_k = DS_RS_MIN;
    int best_hi = 0, hi;
    double best_margin = (double)INFINITY, best_ov = (double)INFINITY, best_area = (double)INFINITY;
    double b1[2 * DYN_RSTAR_MAX_DIMS], b2[2 * DYN_RSTAR_MAX_DIMS];
    ds_rsnode* nn;
    for (axis = 0; axis < t->dims; axis++) {
        double margin = 0;
        for (hi = 0; hi < 2; hi++) {
            ds_rs_sort(t, n->e, total, axis, hi);
            for (k = DS_RS_MIN; k <= total - DS_RS_MIN; k++) {
                ds_rs_group_box(t, n->e, 0, k, b1);
                ds_rs_group_box(t, n->e, k, total, b2);
                margin += ds_rs_margin(t, b1) + ds_rs_margin(t, b2);
            }
        }
        if (margin < best_margin) {
            best_margin = margin;
            best_axis = axis;
        }
    }
    for (hi = 0; hi < 2; hi++) {
        ds_rs_sort(t, n->e, total, best_axis, hi);
        for (k = DS_RS_MIN; k <= total - DS_RS_MIN; k++) {
            double ov, area;
            ds_rs_group_box(t, n->e, 0, k, b1);
            ds_rs_group_box(t, n->e, k, total, b2);
            ov = ds_rs_overlap(t, b1, b2);
            area = ds_rs_area(t, b1) + ds_rs_area(t, b2);
            if (ov < best_ov || (ov == best_ov && area < best_area)) {
                best_ov = ov;
                best_area = area;
                best_k = k;
                best_hi = hi;
            }
        }
    }
    ds_rs_sort(t, n->e, total, best_axis, best_hi);
    nn = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
    if (!nn)
        return NULL;
    nn->level = n->level;
    nn->count = total - best_k;
    memcpy(nn->e, n->e + best_k, sizeof(ds_rsentry) * nn->count);
    n->count = best_k;
    return nn;
}

static int ds_rs_insert_at(dyn_rstar_t* t, const ds_rsentry* in, unsigned level, uint8_t* reinserted);

static int ds_rs_overflow(dyn_rstar_t* t, ds_rsnode** path, unsigned* idx, unsigned depth, uint8_t* reinserted)
{
    for (;;) {
        ds_rsnode* n = path[depth];
        if (n->count <= DS_RS_M)
            break;
        if (n != t->root && !reinserted[n->level]) {
            double center[DYN_RSTAR_MAX_DIMS], nb[2 * DYN_RSTAR_MAX_DIMS], dist[DS_RS_M + 1];
            ds_rsentry out[DS_RS_REINSERT];
            unsigned i, j, d, level = n->level;
            reinserted[level] = 1;
            ds_rs_box_of(t, n, nb);
            for (d = 0; d < t->dims; d++)
                center[d] = (nb[d] + nb[t->dims + d]) / 2;
            for (i = 0; i < n->count; i++) {
                double s = 0;
                for (d = 0; d < t->dims; d++) {
                    double c = (n->e[i].b[d] + n->e[i].b[t->dims + d]) / 2 - center[d];
                    s += c * c;
                }
                dist[i] = s;
            }
            for (j = 0; j < DS_RS_REINSERT; j++) {
                unsigned far = 0;
                for (i = 1; i < n->count; i++)
                    if (dist[i] > dist[far])
                        far = i;
                out[j] = n->e[far];
                n->e[far] = n->e[n->count - 1];
                dist[far] = dist[n->count - 1];
                n->count--;
            }
            for (i = depth; i-- > 0;)
                ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
            for (j = DS_RS_REINSERT; j-- > 0;)
                if (ds_rs_insert_at(t, &out[j], level, reinserted) < 0)
                    return -1;
            return 0;
        } else {
            ds_rsnode* nn = ds_rs_split(t, n);
            if (!nn)
                return -1;
            if (n == t->root) {
                ds_rsnode* r = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
                if (!r) {
                    DYN_DS_FREE(nn);
                    return -1;
                }
                r->level = n->level + 1;
                r->count = 2;
                r->e[0].u.child = n;
                ds_rs_box_of(t, n, r->e[0].b);
                r->e[1].u.child = nn;
                ds_rs_box_of(t, nn, r->e[1].b);
                t->root = r;
                return 0;
            }
            {
                ds_rsnode* p = path[depth - 1];
                ds_rs_box_of(t, n, p->e[idx[depth - 1]].b);
                p->e[p->count].u.child = nn;
                ds_rs_box_of(t, nn, p->e[p->count].b);
                p->count++;
                depth--;
                {
                    unsigned i;
                    for (i = depth; i-- > 0;)
                        ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
                }
            }
        }
    }
    return 0;
}

static int ds_rs_insert_at(dyn_rstar_t* t, const ds_rsentry* in, unsigned level, uint8_t* reinserted)
{
    ds_rsnode* path[64];
    unsigned idx[64], depth = 0, i;
    ds_rsnode* n = t->root;
    path[0] = n;
    while (n->level > level) {
        unsigned c = ds_rs_choose(t, n, in->b);
        idx[depth] = c;
        ds_rs_union(t, n->e[c].b, n->e[c].b, in->b);
        n = n->e[c].u.child;
        path[++depth] = n;
    }
    n->e[n->count++] = *in;
    for (i = depth; i-- > 0;)
        ds_rs_box_of(t, path[i + 1], path[i]->e[idx[i]].b);
    return ds_rs_overflow(t, path, idx, depth, reinserted);
}

int dyn_rstar_insert(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id)
{
    ds_rsentry e;
    uint8_t reinserted[64];
    unsigned d;
    memset(&e, 0, sizeof(e));
    for (d = 0; d < t->dims; d++) {
        if (!(lo[d] <= hi[d]))
            return -1;
        e.b[d] = lo[d];
        e.b[t->dims + d] = hi[d];
    }
    e.u.id = id;
    memset(reinserted, 0, sizeof(reinserted));
    if (ds_rs_insert_at(t, &e, 0, reinserted) < 0)
        return -1;
    t->size++;
    return 0;
}

static int ds_rs_find_leaf(const dyn_rstar_t* t, ds_rsnode* n, const double* b, uint64_t id, ds_rsnode** path, unsigned* idx, unsigned depth, unsigned* out_depth, unsigned* out_i)
{
    unsigned i;
    path[depth] = n;
    if (n->level == 0) {
        for (i = 0; i < n->count; i++) {
            unsigned d, same = 1;
            if (n->e[i].u.id != id)
                continue;
            for (d = 0; d < 2 * t->dims; d++)
                if (n->e[i].b[d] != b[d])
                    same = 0;
            if (same) {
                *out_depth = depth;
                *out_i = i;
                return 1;
            }
        }
        return 0;
    }
    for (i = 0; i < n->count; i++) {
        if (!ds_rs_contains(t, n->e[i].b, b))
            continue;
        idx[depth] = i;
        if (ds_rs_find_leaf(t, n->e[i].u.child, b, id, path, idx, depth + 1, out_depth, out_i))
            return 1;
    }
    return 0;
}

static int ds_rs_collect(ds_rsnode* n, ds_rsentry** buf, unsigned** levels, size_t* cnt, size_t* cap)
{
    unsigned i;
    for (i = 0; i < n->count; i++) {
        if (*cnt == *cap) {
            size_t nc = *cap ? *cap * 2 : 32;
            ds_rsentry* nb = (ds_rsentry*)DYN_DS_REALLOC(*buf, nc * sizeof(ds_rsentry));
            unsigned* nl;
            if (!nb)
                return -1;
            *buf = nb;
            nl = (unsigned*)DYN_DS_REALLOC(*levels, nc * sizeof(unsigned));
            if (!nl)
                return -1;
            *levels = nl;
            *cap = nc;
        }
        (*buf)[*cnt] = n->e[i];
        (*levels)[*cnt] = n->level;
        (*cnt)++;
    }
    return 0;
}

int dyn_rstar_remove(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id)
{
    ds_rsnode* path[64];
    unsigned idx[64], depth = 0, i, d;
    double b[2 * DYN_RSTAR_MAX_DIMS];
    ds_rsentry* orphans = NULL;
    unsigned* levels = NULL;
    size_t norph = 0, cap = 0, k;
    int rc = 1;
    for (d = 0; d < t->dims; d++) {
        b[d] = lo[d];
        b[t->dims + d] = hi[d];
    }
    if (!ds_rs_find_leaf(t, t->root, b, id, path, idx, 0, &depth, &i))
        return 0;
    {
        ds_rsnode* leaf = path[depth];
        leaf->e[i] = leaf->e[leaf->count - 1];
        leaf->count--;
    }
    while (depth > 0) {
        ds_rsnode *n = path[depth], *p = path[depth - 1];
        if (n->count < DS_RS_MIN) {
            if (ds_rs_collect(n, &orphans, &levels, &norph, &cap) < 0) {
                rc = -1;
                break;
            }
            p->e[idx[depth - 1]] = p->e[p->count - 1];
            p->count--;
            DYN_DS_FREE(n);
        } else {
            ds_rs_box_of(t, n, p->e[idx[depth - 1]].b);
        }
        depth--;
    }
    t->size--;
    while (t->root->level > 0 && t->root->count == 1) {
        ds_rsnode* old = t->root;
        t->root = old->e[0].u.child;
        DYN_DS_FREE(old);
    }
    if (t->root->level > 0 && t->root->count == 0) {
        t->root->level = 0;
    }
    for (k = 0; k < norph && rc >= 0; k++) {
        uint8_t reinserted[64];
        memset(reinserted, 1, sizeof(reinserted));
        if (levels[k] == 0 || levels[k] <= t->root->level) {
            if (ds_rs_insert_at(t, &orphans[k], levels[k], reinserted) < 0)
                rc = -1;
        } else {
            ds_rsnode* sub = orphans[k].u.child;
            if (ds_rs_collect(sub, &orphans, &levels, &norph, &cap) < 0)
                rc = -1;
            DYN_DS_FREE(sub);
        }
    }
    DYN_DS_FREE(orphans);
    DYN_DS_FREE(levels);
    return rc;
}

static size_t ds_rs_search_rec(const dyn_rstar_t* t, const ds_rsnode* n, const double* b, dyn_rstar_visit fn, void* ud, int* stop)
{
    size_t hits = 0;
    unsigned i;
    for (i = 0; i < n->count && !*stop; i++) {
        if (!ds_rs_intersects(t, n->e[i].b, b))
            continue;
        if (n->level == 0) {
            hits++;
            if (fn && fn(n->e[i].b, n->e[i].b + t->dims, n->e[i].u.id, ud))
                *stop = 1;
        } else {
            hits += ds_rs_search_rec(t, n->e[i].u.child, b, fn, ud, stop);
        }
    }
    return hits;
}

size_t dyn_rstar_search(const dyn_rstar_t* t, const double* lo, const double* hi, dyn_rstar_visit fn, void* ud)
{
    double b[2 * DYN_RSTAR_MAX_DIMS];
    unsigned d;
    int stop = 0;
    for (d = 0; d < t->dims; d++) {
        b[d] = lo[d];
        b[t->dims + d] = hi[d];
    }
    return ds_rs_search_rec(t, t->root, b, fn, ud, &stop);
}

static double ds_rs_mindist2(const dyn_rstar_t* t, const double* b, const double* p)
{
    double s = 0;
    unsigned d;
    for (d = 0; d < t->dims; d++) {
        double c = 0;
        if (p[d] < b[d])
            c = b[d] - p[d];
        else if (p[d] > b[t->dims + d])
            c = p[d] - b[t->dims + d];
        s += c * c;
    }
    return s;
}

size_t dyn_rstar_nearest(const dyn_rstar_t* t, const double* point, size_t k, uint64_t* ids, double* dist2)
{
    ds_rs_qitem* q;
    size_t qn = 0, qcap = 64, found = 0;
    if (!k || !t->size)
        return 0;
    q = (ds_rs_qitem*)DYN_DS_MALLOC(sizeof(ds_rs_qitem) * qcap);
    if (!q)
        return 0;
    q[qn++] = (ds_rs_qitem) { 0, t->root, 0 };
    while (qn && found < k) {
        ds_rs_qitem top = q[0], last = q[--qn];
        size_t i = 0;
        for (;;) {
            size_t c = 2 * i + 1;
            if (c >= qn)
                break;
            if (c + 1 < qn && q[c + 1].d < q[c].d)
                c++;
            if (last.d <= q[c].d)
                break;
            q[i] = q[c];
            i = c;
        }
        if (qn)
            q[i] = last;
        if (!top.n) {
            ids[found] = top.id;
            if (dist2)
                dist2[found] = top.d;
            found++;
            continue;
        }
        {
            unsigned e;
            for (e = 0; e < top.n->count; e++) {
                ds_rs_qitem it;
                size_t j;
                it.d = ds_rs_mindist2(t, top.n->e[e].b, point);
                it.n = top.n->level ? top.n->e[e].u.child : NULL;
                it.id = top.n->level ? 0 : top.n->e[e].u.id;
                if (qn == qcap) {
                    ds_rs_qitem* nq = (ds_rs_qitem*)DYN_DS_REALLOC(q, sizeof(ds_rs_qitem) * qcap * 2);
                    if (!nq) {
                        DYN_DS_FREE(q);
                        return found;
                    }
                    q = nq;
                    qcap *= 2;
                }
                j = qn++;
                while (j > 0) {
                    size_t p = (j - 1) / 2;
                    if (q[p].d <= it.d)
                        break;
                    q[j] = q[p];
                    j = p;
                }
                q[j] = it;
            }
        }
    }
    DYN_DS_FREE(q);
    return found;
}

static long ds_rs_check_rec(const dyn_rstar_t* t, const ds_rsnode* n, int is_root)
{
    unsigned i, d;
    long total = 0;
    if (!is_root && (n->count < DS_RS_MIN || n->count > DS_RS_M))
        return -1;
    if (n->count > DS_RS_M)
        return -1;
    if (n->level == 0)
        return n->count;
    for (i = 0; i < n->count; i++) {
        double b[2 * DYN_RSTAR_MAX_DIMS];
        long c;
        if (n->e[i].u.child->level != n->level - 1)
            return -1;
        ds_rs_box_of(t, n->e[i].u.child, b);
        for (d = 0; d < 2 * t->dims; d++)
            if (b[d] != n->e[i].b[d])
                return -1;
        c = ds_rs_check_rec(t, n->e[i].u.child, 0);
        if (c < 0)
            return -1;
        total += c;
    }
    return total;
}

int dyn_rstar_check(const dyn_rstar_t* t)
{
    long c = ds_rs_check_rec(t, t->root, 1);
    if (c < 0 || (size_t)c != t->size)
        return -1;
    if (t->root->level > 0 && t->root->count < 2)
        return -1;
    return 0;
}

typedef struct {
    double k;
    uint32_t i;
} ds_rs_key;

static int ds_rs_key_cmp(const void* a, const void* b)
{
    const ds_rs_key* x = (const ds_rs_key*)a;
    const ds_rs_key* y = (const ds_rs_key*)b;
    if (x->k < y->k)
        return -1;
    if (x->k > y->k)
        return 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

static void ds_rs_order(const double* bx, unsigned dims, ds_rs_key* key, uint32_t* perm, size_t from, size_t to, unsigned axis)
{
    size_t j;
    for (j = from; j < to; j++) {
        const double* b = bx + (size_t)perm[j] * 2 * dims;
        key[j].k = b[axis] + b[dims + axis];
        key[j].i = perm[j];
    }
    qsort(key + from, to - from, sizeof(*key), ds_rs_key_cmp);
    for (j = from; j < to; j++)
        perm[j] = key[j].i;
}

static ds_rsnode* ds_rs_node_from(const double* bx, const uint64_t* pay, unsigned dims, const uint32_t* perm, size_t from, size_t to, unsigned level)
{
    ds_rsnode* nd = (ds_rsnode*)DYN_DS_CALLOC(1, sizeof(ds_rsnode));
    size_t j;
    if (!nd)
        return NULL;
    nd->level = level;
    for (j = from; j < to; j++) {
        ds_rsentry* e = &nd->e[nd->count++];
        memcpy(e->b, bx + (size_t)perm[j] * 2 * dims, sizeof(double) * 2 * dims);
        if (level)
            e->u.child = (ds_rsnode*)(uintptr_t)pay[perm[j]];
        else
            e->u.id = pay[perm[j]];
    }
    return nd;
}

static size_t ds_rs_pack(const dyn_rstar_t* t, const double* bx, const uint64_t* pay, size_t n, unsigned level, ds_rs_key* key, uint32_t* perm, double* nbx, uint64_t* npay)
{
    unsigned dims = t->dims;
    size_t k = (n + DS_RS_M - 1) / DS_RS_M, s = 1, si, made = 0, j;
    if (dims > 1)
        while (s * s < k)
            s++;
    for (j = 0; j < n; j++)
        perm[j] = (uint32_t)j;
    ds_rs_order(bx, dims, key, perm, 0, n, 0);
    for (si = 0; si < s; si++) {
        size_t a = n * si / s, b = n * (si + 1) / s, kj, gi;
        if (dims > 1)
            ds_rs_order(bx, dims, key, perm, a, b, 1);
        kj = (b - a + DS_RS_M - 1) / DS_RS_M;
        for (gi = 0; gi < kj; gi++) {
            size_t ga = a + (b - a) * gi / kj, gb = a + (b - a) * (gi + 1) / kj;
            ds_rsnode* nd = ds_rs_node_from(bx, pay, dims, perm, ga, gb, level);
            if (!nd) {
                while (made > 0)
                    ds_rs_free_rec((ds_rsnode*)(uintptr_t)npay[--made]);
                return 0;
            }
            ds_rs_box_of(t, nd, nbx + made * 2 * dims);
            npay[made++] = (uint64_t)(uintptr_t)nd;
        }
    }
    return made;
}

int dyn_rstar_bulk_load(dyn_rstar_t* t, const double* boxes, const uint64_t* ids, size_t n)
{
    unsigned dims = t->dims, level = 0;
    const double* bx = boxes;
    const uint64_t* pay = ids;
    double* own_bx = NULL;
    uint64_t* own_pay = NULL;
    ds_rs_key* key = NULL;
    uint32_t* perm = NULL;
    ds_rsnode* root = NULL;
    size_t j, d;
    size_t total = n;
    if (t->size || t->root->count || n > UINT32_MAX)
        return -1;
    for (j = 0; j < n; j++)
        for (d = 0; d < dims; d++)
            if (!(boxes[j * 2 * dims + d] <= boxes[j * 2 * dims + dims + d]))
                return -1;
    if (!n)
        return 0;
    if (n > DS_RS_M) {
        key = (ds_rs_key*)DYN_DS_MALLOC(sizeof(*key) * n);
        perm = (uint32_t*)DYN_DS_MALLOC(sizeof(*perm) * n);
        if (!key || !perm)
            goto fail;
    }
    for (;;) {
        if (n <= DS_RS_M) {
            for (j = 0; j < n; j++)
                if (perm)
                    perm[j] = (uint32_t)j;
            if (!perm) {
                uint32_t small[DS_RS_M];
                for (j = 0; j < n; j++)
                    small[j] = (uint32_t)j;
                root = ds_rs_node_from(bx, pay, dims, small, 0, n, level);
            } else {
                root = ds_rs_node_from(bx, pay, dims, perm, 0, n, level);
            }
            if (!root)
                goto fail;
            break;
        }
        {
            size_t k = 2 * ((n + DS_RS_M - 1) / DS_RS_M) + 2;
            double* nbx = (double*)DYN_DS_MALLOC(sizeof(double) * 2 * dims * k);
            uint64_t* npay = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * k);
            size_t made;
            if (!nbx || !npay) {
                DYN_DS_FREE(nbx);
                DYN_DS_FREE(npay);
                goto fail;
            }
            made = ds_rs_pack(t, bx, pay, n, level, key, perm, nbx, npay);
            if (!made) {
                DYN_DS_FREE(nbx);
                DYN_DS_FREE(npay);
                goto fail;
            }
            DYN_DS_FREE(own_bx);
            DYN_DS_FREE(own_pay);
            bx = own_bx = nbx;
            pay = own_pay = npay;
            n = made;
            level++;
        }
    }
    DYN_DS_FREE(own_bx);
    DYN_DS_FREE(own_pay);
    DYN_DS_FREE(key);
    DYN_DS_FREE(perm);
    ds_rs_free_rec(t->root);
    t->root = root;
    t->size = total;
    return 0;
fail:
    if (own_pay)
        for (j = 0; j < n; j++)
            ds_rs_free_rec((ds_rsnode*)(uintptr_t)own_pay[j]);
    DYN_DS_FREE(own_bx);
    DYN_DS_FREE(own_pay);
    DYN_DS_FREE(key);
    DYN_DS_FREE(perm);
    return -1;
}
