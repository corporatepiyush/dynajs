#include "mlk.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#define DYN_TNODE_MISSING_LEFT 1
#define DYN_TREE_RADIX_MIN 512

typedef struct {
    const uint32_t* lo;
    size_t n;
    int tied;
} dyn_tree_tie_t;

typedef struct {
    const double* X;
    const double* y;
    size_t cols;
    const double* classes;
    size_t n_classes;
    double* cnt_left;
    double* cnt_right;
    double* cnt_tot;
    const uint8_t* cls_of;
    const double* w;
    uint32_t* feat;
    size_t n_cand;
    uint32_t* scratch;
    dyn_tree_kv_t* kv;
    uint32_t* best_order;
    const dyn_hist_t* hist;
    double* hbuf;
    uint32_t* hnext;
    size_t hstride;
    const double* grad;
    const double* hess;
    dyn_tree_opts_t opt;
    uint64_t rng;
    int (*poll)(void*);
    void* poll_ud;
    int interrupted;
    int any_order;
    dyn_tree_tie_t* tie;
    uint8_t* tiefree;
    size_t rows;
} dyn_tree_ctx_t;

void dyn_tree_free(dyn_tree_t* t)
{
    free(t->node);
    free(t->improve);
    free(t->nsamp);
    free(t->proba);
    t->node = NULL;
    t->improve = t->nsamp = t->proba = NULL;
    t->n_nodes = t->cap = t->n_classes = 0;
}

static size_t dyn_tree_push(dyn_tree_t* t)
{
    if (t->n_nodes == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 32, k = t->n_classes;
        dyn_tnode_t* p = (dyn_tnode_t*)realloc(t->node, cap * sizeof(*p));
        double *im, *ns, *pr = NULL;
        if (!p)
            return SIZE_MAX;
        t->node = p;
        im = (double*)realloc(t->improve, cap * sizeof(double));
        if (!im)
            return SIZE_MAX;
        t->improve = im;
        ns = (double*)realloc(t->nsamp, cap * sizeof(double));
        if (!ns)
            return SIZE_MAX;
        t->nsamp = ns;
        if (k) {
            pr = (double*)realloc(t->proba, cap * k * sizeof(double));
            if (!pr)
                return SIZE_MAX;
            t->proba = pr;
        }
        t->cap = cap;
    }
    memset(&t->node[t->n_nodes], 0, sizeof(t->node[0]));
    t->improve[t->n_nodes] = 0.0;
    t->nsamp[t->n_nodes] = 0.0;
    if (t->n_classes)
        memset(t->proba + t->n_nodes * t->n_classes, 0,
            t->n_classes * sizeof(double));
    return t->n_nodes++;
}

static void dyn_tree_set_proba(dyn_tree_t* t, size_t self, const double* cnt,
    size_t n)
{
    size_t i, k = t->n_classes;
    double tot = 0.0;
    if (!k || n == 0)
        return;
    for (i = 0; i < k; i++)
        tot += cnt[i];
    if (tot <= 0.0)
        return;
    for (i = 0; i < k; i++)
        t->proba[self * k + i] = cnt[i] / tot;
}

static void dyn_tree_qsort_kv(dyn_tree_kv_t* a, size_t n)
{
    size_t limit = 0, t;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_kv_sort_r(a, n, 0, 2 * limit + 1);
}

static void dyn_tree_sort_kv(dyn_tree_kv_t* a, size_t n)
{
    if (n >= DYN_TREE_RADIX_MIN && dyn_ml_kv_radix(a, n) == 0)
        return;
    dyn_tree_qsort_kv(a, n);
}

static void dyn_tree_kv_fill(const double* X, size_t cols, int32_t f,
    const uint32_t* idx, size_t n, dyn_tree_kv_t* kv)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t r = idx[i];
        kv[i].row = r;
        kv[i].key = X[(size_t)r * cols + f];
    }
}

static int dyn_tree_kv_tied(const dyn_tree_kv_t* kv, size_t n)
{
    size_t i;
    for (i = 1; i < n; i++)
        if (kv[i].key == kv[i - 1].key && kv[i].row != kv[i - 1].row)
            return 1;
    return 0;
}

static uint8_t dyn_tree_col_ties(const dyn_tree_ctx_t* c, int32_t f)
{
    const uint64_t empty = 0x7ff8000000000001ull;
    size_t cap = 16, mask, i;
    unsigned shift = 60;
    uint64_t* tab;
    uint8_t res = 1;
    while (cap < 2 * c->rows && cap < ((size_t)1 << 62)) {
        cap <<= 1;
        shift--;
    }
    tab = (uint64_t*)malloc(cap * sizeof(uint64_t));
    if (!tab)
        return 2;
    for (i = 0; i < cap; i++)
        tab[i] = empty;
    mask = cap - 1;
    for (i = 0; i < c->rows; i++) {
        double v = c->X[i * c->cols + f];
        uint64_t u = 0;
        size_t h;
        if (v != 0.0)
            memcpy(&u, &v, 8);
        h = (size_t)((u * 0x9e3779b97f4a7c15ull) >> shift);
        while (tab[h] != empty && tab[h] != u)
            h = (h + 1) & mask;
        if (tab[h] == u) {
            res = 2;
            break;
        }
        tab[h] = u;
    }
    free(tab);
    return res;
}

static void dyn_tree_sort_idx(dyn_tree_ctx_t* c, int32_t f, const uint32_t* node,
    size_t n)
{
    uint32_t* idx = c->scratch;
    dyn_tree_kv_t* kv = c->kv;
    dyn_tree_tie_t* tf = c->tie ? c->tie + f : NULL;
    int inside = tf && tf->lo && node >= tf->lo && node + n <= tf->lo + tf->n;
    uint8_t* g = c->tiefree ? c->tiefree + f : NULL;
    size_t i;
    if (g && *g == 0 && n >= DYN_TREE_RADIX_MIN && !c->any_order)
        *g = dyn_tree_col_ties(c, f);
    dyn_tree_kv_fill(c->X, c->cols, f, idx, n, kv);
    if (n < DYN_TREE_RADIX_MIN || (!c->any_order && !(g && *g == 1) && (!tf || (inside && tf->tied)))) {
        dyn_tree_qsort_kv(kv, n);
    } else if (!c->any_order && !(g && *g == 1) && !inside) {
        dyn_tree_qsort_kv(kv, n);
        tf->lo = node;
        tf->n = n;
        tf->tied = dyn_tree_kv_tied(kv, n);
    } else if (dyn_ml_kv_radix(kv, n) != 0) {
        dyn_tree_qsort_kv(kv, n);
    }
    for (i = 0; i < n; i++)
        idx[i] = kv[i].row;
}

void dyn_hist_free(dyn_hist_t* h)
{
    free(h->code);
    free(h->vmin);
    free(h->vmax);
    free(h->nbin);
    memset(h, 0, sizeof(*h));
}

static double dyn_hist_mid(double lo, double hi)
{
    double t = lo + (hi - lo) * 0.5;
    return (t >= hi) ? lo : t;
}

int dyn_hist_build(dyn_hist_t* h, const double* X, size_t rows,
    size_t cols, size_t max_bins, dyn_tree_kv_t* kv,
    double* vals, const double* w)
{
    size_t f, i, b, m, ndist, nb;

    memset(h, 0, sizeof(*h));
    if (max_bins < 2)
        max_bins = 2;
    if (max_bins > DYN_HIST_MAX_BINS)
        max_bins = DYN_HIST_MAX_BINS;
    if (rows == 0 || cols == 0 || cols > SIZE_MAX / rows)
        return -1;
    if (cols > (SIZE_MAX / sizeof(double)) / max_bins || cols > SIZE_MAX / sizeof(uint32_t))
        return -1;
    h->rows = rows;
    h->cols = cols;
    h->max_bins = max_bins;
    h->exact = 1;
    h->code = (uint8_t*)malloc(rows * cols);
    h->vmin = (double*)malloc(cols * max_bins * sizeof(double));
    h->vmax = (double*)malloc(cols * max_bins * sizeof(double));
    h->nbin = (uint32_t*)malloc(cols * sizeof(uint32_t));
    if (!h->code || !h->vmin || !h->vmax || !h->nbin) {
        dyn_hist_free(h);
        return -1;
    }

    for (f = 0; f < cols; f++) {
        double *lo_v = h->vmin + f * max_bins, *hi_v = h->vmax + f * max_bins;
        m = 0;
        for (i = 0; i < rows; i++) {
            double v = X[i * cols + f];
            if (w && w[i] == 0.0)
                continue;
            if (!isnan(v)) {
                kv[m].key = v;
                kv[m].row = (uint32_t)i;
                m++;
            }
        }
        dyn_tree_sort_kv(kv, m);
        ndist = 0;
        for (i = 0; i < m; i++)
            if (ndist == 0 || kv[i].key != vals[ndist - 1])
                vals[ndist++] = kv[i].key;

        if (ndist == 0) {
            nb = 1;
            lo_v[0] = hi_v[0] = 0.0;
        } else if (ndist <= max_bins) {
            nb = ndist;
            for (b = 0; b < ndist; b++)
                lo_v[b] = hi_v[b] = vals[b];
        } else {
            size_t start = 0;
            h->exact = 0;
            nb = max_bins;
            for (b = 0; b < nb; b++) {
                size_t end = (b + 1 == nb)
                    ? ndist
                    : (size_t)(((double)(b + 1) * (double)ndist) / (double)nb);
                if (end <= start)
                    end = start + 1;
                if (end > ndist)
                    end = ndist;
                lo_v[b] = vals[start];
                hi_v[b] = vals[end - 1];
                start = end;
            }
        }
        h->nbin[f] = (uint32_t)nb;

        for (i = 0; i < rows; i++) {
            double v = X[i * cols + f];
            size_t lo = 0, hi = nb - 1;
            if (isnan(v)) {
                h->code[f * rows + i] = (uint8_t)nb;
                continue;
            }
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (v <= hi_v[mid])
                    hi = mid;
                else
                    lo = mid + 1;
            }
            h->code[f * rows + i] = (uint8_t)lo;
        }
    }
    return 0;
}

static double dyn_newton_leaf(double G, double H, double lambda, double alpha)
{
    double g = G, d = H + lambda;
    if (alpha > 0.0) {
        if (g > alpha)
            g -= alpha;
        else if (g < -alpha)
            g += alpha;
        else
            return 0.0;
    }
    return (d > 0.0) ? -g / d : 0.0;
}

static double dyn_newton_score(double G, double H, double lambda, double alpha)
{
    double g = fabs(G) - alpha, d = H + lambda;
    if (g < 0.0)
        g = 0.0;
    return (d > 0.0) ? g * g / d : 0.0;
}

static size_t dyn_tree_class_of(const dyn_tree_ctx_t* c, uint32_t r)
{
    return c->cls_of ? c->cls_of[r] : 0;
}

static double dyn_tree_leaf_value(dyn_tree_ctx_t* c, const uint32_t* idx,
    size_t n)
{
    size_t i;
    if (c->opt.newton) {
        double G = 0.0, H = 0.0;
        for (i = 0; i < n; i++) {
            G += c->grad[idx[i]];
            H += c->hess[idx[i]];
        }
        return dyn_newton_leaf(G, H, c->opt.lambda, c->opt.alpha);
    }
    if (!c->opt.classifier) {
        double s = 0.0;
        if (c->w) {
            double sw = 0.0;
            for (i = 0; i < n; i++) {
                double wi = c->w[idx[i]];
                s += wi * c->y[idx[i]];
                sw += wi;
            }
            return sw > 0.0 ? s / sw : 0.0;
        }
        for (i = 0; i < n; i++)
            s += c->y[idx[i]];
        return s / (double)n;
    }
    memset(c->cnt_left, 0, c->n_classes * sizeof(double));
    if (c->w) {
        for (i = 0; i < n; i++)
            c->cnt_left[dyn_tree_class_of(c, idx[i])] += c->w[idx[i]];
    } else {
        for (i = 0; i < n; i++)
            c->cnt_left[dyn_tree_class_of(c, idx[i])] += 1.0;
    }
    {
        size_t best = 0;
        for (i = 1; i < c->n_classes; i++)
            if (c->cnt_left[i] > c->cnt_left[best])
                best = i;
        return c->classes[best];
    }
}

static double dyn_tree_gini(const double* cnt, size_t k, double tot)
{
    double s = 0.0;
    size_t i;
    if (tot <= 0.0)
        return 0.0;
    for (i = 0; i < k; i++)
        s += cnt[i] * cnt[i];
    return tot - s / tot;
}

static void dyn_split_hist(dyn_tree_ctx_t* c, const uint32_t* idx, size_t n,
    size_t nf, int32_t* out_f, double* out_thr,
    double* out_gain, uint32_t* out_bin, int* out_mleft)
{
    const dyn_hist_t* H = c->hist;
    const uint8_t* code = H->code;
    size_t stride = c->hstride, ncls = c->n_classes;
    size_t k, b, i, j, msl = c->opt.min_samples_leaf;
    double lambda = c->opt.lambda, alpha = c->opt.alpha, gam = c->opt.gamma;
    double mcw = c->opt.min_child_weight;
    int cls = c->opt.classifier, newton = c->opt.newton;
    size_t cslot = c->w ? stride - 1 : 0;

    for (k = 0; k < nf; k++) {
        int32_t f = (int32_t)c->feat[k];
        const uint8_t* cf = code + (size_t)f * H->rows;
        size_t nb = H->nbin[f], miss = nb;
        const double* lo_v = H->vmin + (size_t)f * H->max_bins;
        const double* hi_v = H->vmax + (size_t)f * H->max_bins;
        double* hb = c->hbuf;
        double tot_c = 0.0, tot_1 = 0.0, tot_2 = 0.0, parent = 0.0;
        double tot_n = 0.0;
        size_t lo_b = nb, hi_b = 0;
        int dir;

        if (newton) {
            const double *g = c->grad, *hh = c->hess;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += 1.0;
                s[1] += g[r];
                s[2] += hh[r];
            }
        } else if (c->w && cls) {
            const double* tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += tw[r];
                s[1 + dyn_tree_class_of(c, r)] += tw[r];
                s[cslot] += 1.0;
            }
        } else if (c->w) {
            const double *ty = c->y, *tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                double v = ty[r], wi = tw[r];
                s[0] += wi;
                s[1] += wi * v;
                s[2] += wi * v * v;
                s[cslot] += 1.0;
            }
        } else if (cls) {
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += 1.0;
                s[1 + dyn_tree_class_of(c, r)] += 1.0;
            }
        } else {
            const double* ty = c->y;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                double v = ty[r];
                s[0] += 1.0;
                s[1] += v;
                s[2] += v * v;
            }
        }

        if (cls && !newton) {
            memset(c->cnt_tot, 0, ncls * sizeof(double));
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double* s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            {
                const double* s = hb + nb * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            parent = dyn_tree_gini(c->cnt_tot, ncls, tot_c);
        } else {
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double* s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                tot_1 += s[1];
                tot_2 += s[2];
            }
            {
                const double* s = hb + nb * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                tot_1 += s[1];
                tot_2 += s[2];
            }
            parent = newton ? dyn_newton_score(tot_1, tot_2, lambda, alpha)
                            : tot_2 - tot_1 * tot_1 / tot_c;
        }
        if (tot_c <= 0.0)
            continue;

        if (lo_b <= hi_b) {
            uint32_t nxt = (uint32_t)nb;
            c->hnext[hi_b + 1] = nxt;
            for (b = hi_b + 1; b-- > lo_b;) {
                if (hb[b * stride] > 0.0)
                    nxt = (uint32_t)b;
                c->hnext[b] = nxt;
            }
        }

        for (dir = 0; dir < 2; dir++) {
            double cl = 0.0, s1 = 0.0, s2 = 0.0, nl_cnt = 0.0;
            const double* ms = hb + miss * stride;
            size_t prev_nz = nb;
            if (dir == 1 && ms[0] == 0.0)
                break;
            if (cls && !newton)
                memset(c->cnt_left, 0, ncls * sizeof(double));
            if (dir == 1) {
                cl = ms[0];
                nl_cnt = ms[cslot];
                if (cls && !newton) {
                    for (j = 0; j < ncls; j++)
                        c->cnt_left[j] += ms[1 + j];
                } else {
                    s1 = ms[1];
                    s2 = ms[2];
                }
            }
            for (b = 0; b <= (lo_b <= hi_b ? hi_b + 1 : 0); b = (b ? b + 1 : lo_b + 1)) {
                double gain, cr, thr;
                size_t br;
                if (b > 0) {
                    const double* s = hb + (b - 1) * stride;
                    cl += s[0];
                    nl_cnt += s[cslot];
                    if (s[0] > 0.0)
                        prev_nz = b - 1;
                    if (cls && !newton) {
                        for (j = 0; j < ncls; j++)
                            c->cnt_left[j] += s[1 + j];
                    } else {
                        s1 += s[1];
                        s2 += s[2];
                    }
                }
                cr = tot_c - cl;
                if (nl_cnt < (double)msl || tot_n - nl_cnt < (double)msl)
                    continue;
                if (cl <= 0.0 || cr <= 0.0)
                    continue;
                if (newton) {
                    double gr = tot_1 - s1, hr = tot_2 - s2;
                    if (s2 < mcw || hr < mcw)
                        continue;
                    gain = 0.5 * (dyn_newton_score(s1, s2, lambda, alpha) + dyn_newton_score(gr, hr, lambda, alpha) - parent) - gam;
                } else if (cls) {
                    for (j = 0; j < ncls; j++)
                        c->cnt_right[j] = c->cnt_tot[j] - c->cnt_left[j];
                    gain = parent - (dyn_tree_gini(c->cnt_left, ncls, cl) + dyn_tree_gini(c->cnt_right, ncls, cr)) - gam;
                } else {
                    double r1 = tot_1 - s1, r2 = tot_2 - s2;
                    double sse_l = s2 - s1 * s1 / cl;
                    double sse_r = r2 - r1 * r1 / cr;
                    gain = parent - (sse_l + sse_r) - gam;
                }
                if (gain > *out_gain) {
                    br = (b >= lo_b && b <= hi_b + 1) ? c->hnext[b] : (uint32_t)lo_b;
                    if (prev_nz == nb)
                        thr = -DYN_INFINITY;
                    else if (br < nb)
                        thr = dyn_hist_mid(hi_v[prev_nz], lo_v[br]);
                    else
                        thr = hi_v[prev_nz];
                    *out_gain = gain;
                    *out_f = f;
                    *out_thr = thr;
                    *out_bin = (uint32_t)b;
                    *out_mleft = dir;
                }
            }
        }
        if (lo_b <= hi_b)
            memset(hb + lo_b * stride, 0, (hi_b - lo_b + 1) * stride * sizeof(double));
        memset(hb + nb * stride, 0, stride * sizeof(double));
    }
}

static size_t dyn_tree_depth_limit(size_t requested)
{
    return (requested && requested < DYN_TREE_MAX_DEPTH)
        ? requested
        : DYN_TREE_MAX_DEPTH;
}

static size_t dyn_tree_build(dyn_tree_ctx_t* c, dyn_tree_t* t, uint32_t* idx,
    size_t n, size_t depth)
{
    size_t self, i, k, nf, nl, left, right, ncand;
    int32_t best_f = -1;
    double best_thr = 0.0, best_gain = 0.0;
    uint32_t best_bin = 0;
    int best_mleft = 0;
    int pure = 1;

    if (c->interrupted || (c->poll && c->poll(c->poll_ud))) {
        c->interrupted = 1;
        return SIZE_MAX;
    }
    self = dyn_tree_push(t);
    if (self == SIZE_MAX)
        return SIZE_MAX;
    t->node[self].left = t->node[self].right = DYN_TREE_LEAF;
    t->node[self].feature = -1;
    if (c->w) {
        double sw = 0.0;
        for (i = 0; i < n; i++)
            sw += c->w[idx[i]];
        t->nsamp[self] = sw;
    } else {
        t->nsamp[self] = (double)n;
    }

    for (i = 1; i < n; i++)
        if (c->y[idx[i]] != c->y[idx[0]]) {
            pure = 0;
            break;
        }
    if (pure || n < c->opt.min_samples_split || n < 2 * c->opt.min_samples_leaf || depth >= dyn_tree_depth_limit(c->opt.max_depth)) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }

    ncand = c->n_cand ? c->n_cand : c->cols;
    nf = c->opt.max_features ? c->opt.max_features : ncand;
    if (nf > ncand)
        nf = ncand;
    if (nf < ncand) {
        for (i = 0; i < nf; i++) {
            size_t j = i + (size_t)(dyn_splitmix64(&c->rng) % (uint64_t)(ncand - i));
            uint32_t tmp = c->feat[i];
            c->feat[i] = c->feat[j];
            c->feat[j] = tmp;
        }
    }

    if (c->hist) {
        dyn_split_hist(c, idx, n, nf, &best_f, &best_thr, &best_gain, &best_bin,
            &best_mleft);
        goto chosen;
    }

    for (k = 0; k < nf; k++) {
        int32_t f = (int32_t)c->feat[k];
        double sum_all = 0.0, sq_all = 0.0, parent, sum_l = 0.0, sq_l = 0.0;
        double wt_all = 0.0;
        double gain_at_entry = best_gain;

        memcpy(c->scratch, idx, n * sizeof(uint32_t));
        dyn_tree_sort_idx(c, f, idx, n);

        if (c->w) {
            wt_all = 0.0;
            if (!c->opt.classifier) {
                for (i = 0; i < n; i++) {
                    uint32_t r = c->scratch[i];
                    double v = c->y[r], wi = c->w[r];
                    sum_all += wi * v;
                    sq_all += wi * v * v;
                    wt_all += wi;
                }
                parent = wt_all > 0.0 ? sq_all - sum_all * sum_all / wt_all : 0.0;
            } else {
                memset(c->cnt_left, 0, c->n_classes * sizeof(double));
                memset(c->cnt_right, 0, c->n_classes * sizeof(double));
                for (i = 0; i < n; i++) {
                    uint32_t r = c->scratch[i];
                    c->cnt_right[dyn_tree_class_of(c, r)] += c->w[r];
                    wt_all += c->w[r];
                }
                parent = dyn_tree_gini(c->cnt_right, c->n_classes, wt_all);
            }
        } else if (!c->opt.classifier) {
            for (i = 0; i < n; i++) {
                double v = c->y[c->scratch[i]];
                sum_all += v;
                sq_all += v * v;
            }
            parent = sq_all - sum_all * sum_all / (double)n;
        } else {
            memset(c->cnt_left, 0, c->n_classes * sizeof(double));
            memset(c->cnt_right, 0, c->n_classes * sizeof(double));
            for (i = 0; i < n; i++)
                c->cnt_right[dyn_tree_class_of(c, c->scratch[i])] += 1.0;
            parent = dyn_tree_gini(c->cnt_right, c->n_classes, (double)n);
        }

        {
            const double* TX = c->X;
            const double* ty = c->y;
            const uint32_t* scr = c->scratch;
            size_t cols = c->cols, msl = c->opt.min_samples_leaf;
            double xprev = TX[(size_t)scr[0] * cols + f];

            if (c->w) {
                const double* tw = c->w;
                double wt_l = 0.0;
                double *cl_l = c->cnt_left, *cl_r = c->cnt_right;
                size_t ncls = c->n_classes;
                int cls = c->opt.classifier;
                for (i = 1; i < n; i++) {
                    uint32_t prev = scr[i - 1];
                    double xcur = TX[(size_t)scr[i] * cols + f];
                    double wi = tw[prev];
                    if (cls) {
                        cl_l[dyn_tree_class_of(c, prev)] += wi;
                        cl_r[dyn_tree_class_of(c, prev)] -= wi;
                    } else {
                        double v = ty[prev];
                        sum_l += wi * v;
                        sq_l += wi * v * v;
                    }
                    wt_l += wi;
                    if (xcur != xprev && i >= msl && n - i >= msl) {
                        double wt_r = wt_all - wt_l, gain;
                        if (wt_l <= 0.0 || wt_r <= 0.0) {
                            xprev = xcur;
                            continue;
                        }
                        if (cls) {
                            gain = parent - (dyn_tree_gini(cl_l, ncls, wt_l) + dyn_tree_gini(cl_r, ncls, wt_r));
                        } else {
                            double sum_r = sum_all - sum_l, sq_r = sq_all - sq_l;
                            gain = parent - ((sq_l - sum_l * sum_l / wt_l) + (sq_r - sum_r * sum_r / wt_r));
                        }
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_f = f;
                            best_thr = xprev + (xcur - xprev) * 0.5;
                            if (best_thr >= xcur)
                                best_thr = xprev;
                        }
                    }
                    xprev = xcur;
                }
            } else if (!c->opt.classifier) {
                for (i = 1; i < n; i++) {
                    uint32_t prev = scr[i - 1];
                    double xcur = TX[(size_t)scr[i] * cols + f];
                    double v = ty[prev];
                    sum_l += v;
                    sq_l += v * v;
                    if (xcur != xprev && i >= msl && n - i >= msl) {
                        double sum_r = sum_all - sum_l, sq_r = sq_all - sq_l;
                        double sse_l = sq_l - sum_l * sum_l / (double)i;
                        double sse_r = sq_r - sum_r * sum_r / (double)(n - i);
                        double gain = parent - (sse_l + sse_r);
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_f = f;
                            best_thr = xprev + (xcur - xprev) * 0.5;
                            if (best_thr >= xcur)
                                best_thr = xprev;
                        }
                    }
                    xprev = xcur;
                }
            } else {
                double *cl_l = c->cnt_left, *cl_r = c->cnt_right;
                size_t ncls = c->n_classes;
                for (i = 1; i < n; i++) {
                    uint32_t prev = scr[i - 1];
                    double xcur = TX[(size_t)scr[i] * cols + f];
                    size_t cl = dyn_tree_class_of(c, prev);
                    cl_l[cl] += 1.0;
                    cl_r[cl] -= 1.0;
                    if (xcur != xprev && i >= msl && n - i >= msl) {
                        double gain = parent - (dyn_tree_gini(cl_l, ncls, (double)i) + dyn_tree_gini(cl_r, ncls, (double)(n - i)));
                        if (gain > best_gain) {
                            best_gain = gain;
                            best_f = f;
                            best_thr = xprev + (xcur - xprev) * 0.5;
                            if (best_thr >= xcur)
                                best_thr = xprev;
                        }
                    }
                    xprev = xcur;
                }
            }
        }
        if (best_gain > gain_at_entry)
            memcpy(c->best_order, c->scratch, n * sizeof(uint32_t));
    }

chosen:
    if (best_f < 0) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }
    if (c->hist) {
        const dyn_hist_t* H = c->hist;
        const uint8_t* cf = H->code + (size_t)best_f * H->rows;
        uint8_t miss = (uint8_t)H->nbin[best_f];
        uint32_t* lft = c->scratch;
        uint32_t* rgt = c->best_order;
        size_t nr = 0;
        nl = 0;
        for (i = 0; i < n; i++) {
            uint32_t r = idx[i];
            uint8_t code = cf[r];
            size_t gl = (size_t)((code == miss) ? best_mleft : (code < best_bin));
            lft[nl] = r;
            rgt[nr] = r;
            nl += gl;
            nr += gl ^ 1;
        }
        memcpy(idx, lft, nl * sizeof(uint32_t));
        memcpy(idx + nl, rgt, nr * sizeof(uint32_t));
        t->node[self].flags = best_mleft ? DYN_TNODE_MISSING_LEFT : 0;
    } else {
        memcpy(idx, c->best_order, n * sizeof(uint32_t));
        nl = 0;
        while (nl < n && c->X[(size_t)idx[nl] * c->cols + best_f] <= best_thr)
            nl++;
    }
    if (nl == 0 || nl == n) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }
    t->improve[self] = best_gain;
    left = dyn_tree_build(c, t, idx, nl, depth + 1);
    if (left == SIZE_MAX)
        return SIZE_MAX;
    right = dyn_tree_build(c, t, idx + nl, n - nl, depth + 1);
    if (right == SIZE_MAX)
        return SIZE_MAX;
    t->node[self].feature = best_f;
    t->node[self].threshold = best_thr;
    t->node[self].left = (uint32_t)left;
    t->node[self].right = (uint32_t)right;
    return self;
}

double dyn_tree_predict_row(const dyn_tree_t* t, const double* x)
{
    uint32_t at = 0;
    while (t->node[at].feature >= 0) {
        at = (x[t->node[at].feature] <= t->node[at].threshold)
            ? t->node[at].left
            : t->node[at].right;
    }
    return t->node[at].value;
}

double dyn_tree_predict_row_missing(const dyn_tree_t* t, const double* x)
{
    uint32_t at = 0;
    while (t->node[at].feature >= 0) {
        double v = x[t->node[at].feature];
        int left = isnan(v) ? (t->node[at].flags & DYN_TNODE_MISSING_LEFT)
                            : (v <= t->node[at].threshold);
        at = left ? t->node[at].left : t->node[at].right;
    }
    return t->node[at].value;
}

uint32_t dyn_tree_leaf_index(const dyn_tree_t* t, const double* x,
    size_t cols)
{
    uint32_t i = 0;
    (void)cols;
    if (t->n_nodes == 0)
        return 0;
    while (t->node[i].feature >= 0)
        i = x[t->node[i].feature] <= t->node[i].threshold ? t->node[i].left
                                                          : t->node[i].right;
    return i;
}

uint32_t dyn_tree_leaf_index_missing(const dyn_tree_t* t, const double* x)
{
    uint32_t i = 0;
    if (t->n_nodes == 0)
        return 0;
    while (t->node[i].feature >= 0) {
        double v = x[t->node[i].feature];
        int left = isnan(v) ? (t->node[i].flags & DYN_TNODE_MISSING_LEFT)
                            : (v <= t->node[i].threshold);
        i = left ? t->node[i].left : t->node[i].right;
    }
    return i;
}

int dyn_tree_fit(dyn_tree_t* t, const double* X, const double* y, size_t rows, size_t cols,
    const double* classes, size_t n_classes, const dyn_tree_opts_t* opt, uint64_t* rng,
    uint32_t* idx, size_t n, const dyn_tree_work_t* w, int (*poll)(void*), void* poll_ud,
    int* interrupted)
{
    dyn_tree_ctx_t c;
    size_t i;

    c.poll = poll;
    c.poll_ud = poll_ud;
    c.interrupted = 0;
    c.X = X;
    c.y = y;
    c.cols = cols;
    c.classes = classes;
    c.n_classes = n_classes;
    c.cnt_left = w->cnt_l;
    c.cnt_right = w->cnt_r;
    c.cnt_tot = w->cnt_tot;
    c.feat = w->feat;
    c.n_cand = w->n_cand;
    c.scratch = w->scratch;
    c.kv = w->kv;
    c.best_order = w->best_order;
    c.hist = w->hist;
    c.hbuf = w->hbuf;
    c.hnext = w->hnext;
    c.hstride = w->hstride;
    c.grad = w->grad;
    c.hess = w->hess;
    c.w = w->w;
    c.cls_of = w->cls_of;
    c.opt = *opt;
    c.rng = *rng;
    c.rows = rows;
    c.tiefree = w->tiefree;
    c.any_order = opt->classifier && !w->w;
    c.tie = (!c.any_order && !w->hist && n >= DYN_TREE_RADIX_MIN) ? (dyn_tree_tie_t*)calloc(cols, sizeof(dyn_tree_tie_t)) : NULL;
    if (!w->n_cand)
        for (i = 0; i < cols; i++)
            c.feat[i] = (uint32_t)i;
    t->n_classes = opt->classifier ? n_classes : 0;
    if (dyn_tree_build(&c, t, idx, n, 0) == SIZE_MAX) {
        *interrupted = c.interrupted;
        free(c.tie);
        return -1;
    }
    free(c.tie);
    *rng = c.rng;
    return 0;
}
