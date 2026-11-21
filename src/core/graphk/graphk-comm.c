#include "graphk-int.h"
#include "../dyn-prng.h"

#define DYN_GK_LOUVAIN_MAX_PASSES 4096u
#define DYN_GK_LOUVAIN_MAX_LEVELS 64u

static void dyn_gk_shuffle(dyn_prng_t* r, uint32_t* a, uint32_t n)
{
    uint32_t i;
    for (i = n; i > 1; i--) {
        uint32_t j = (uint32_t)dyn_prng_next_bounded(r, i), t = a[i - 1];
        a[i - 1] = a[j];
        a[j] = t;
    }
}

static uint32_t dyn_gk_compact(int32_t* lab, uint32_t n, uint32_t* map)
{
    uint32_t i, k = 0;
    for (i = 0; i < n; i++)
        map[i] = UINT32_MAX;
    for (i = 0; i < n; i++) {
        uint32_t l = (uint32_t)lab[i];
        if (map[l] == UINT32_MAX)
            map[l] = k++;
        lab[i] = (int32_t)map[l];
    }
    return k;
}

int dyn_gk_lpa(const dyn_gk_csr_t* a, uint64_t seed, uint32_t max_rounds, int32_t* label, uint32_t* ncomm)
{
    uint32_t n = a->n, *order, *tag, *touched, *bests, r, i;
    double* acc;
    dyn_prng_t rng;
    *ncomm = 0;
    if (n == 0)
        return DYN_GK_OK;
    order = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    tag = (uint32_t*)calloc(n, sizeof(uint32_t));
    touched = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    bests = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    acc = (double*)malloc((size_t)n * sizeof(double));
    if (!order || !tag || !touched || !bests || !acc) {
        free(order);
        free(tag);
        free(touched);
        free(bests);
        free(acc);
        return DYN_GK_E_OOM;
    }
    dyn_prng_seed(&rng, seed);
    for (i = 0; i < n; i++) {
        order[i] = i;
        label[i] = (int32_t)i;
    }
    for (r = 0; r < max_rounds; r++) {
        uint32_t changed = 0;
        dyn_gk_shuffle(&rng, order, n);
        for (i = 0; i < n; i++) {
            uint32_t u = order[i], e, nt = 0, nb = 0, t, cur = (uint32_t)label[u], keep = 0;
            double top = 0.0;
            if (a->off[u] == a->off[u + 1])
                continue;
            for (e = a->off[u]; e < a->off[u + 1]; e++) {
                uint32_t l = (uint32_t)label[a->tgt[e]];
                if (tag[l] != u + 1) {
                    tag[l] = u + 1;
                    acc[l] = 0.0;
                    touched[nt++] = l;
                }
                acc[l] += a->w ? a->w[e] : 1.0;
            }
            for (t = 0; t < nt; t++)
                if (acc[touched[t]] > top)
                    top = acc[touched[t]];
            for (t = 0; t < nt; t++)
                if (acc[touched[t]] == top) {
                    bests[nb++] = touched[t];
                    if (touched[t] == cur)
                        keep = 1;
                }
            if (!keep) {
                label[u] = (int32_t)bests[nb == 1 ? 0 : dyn_prng_next_bounded(&rng, nb)];
                changed++;
            }
            for (t = 0; t < nt; t++)
                tag[touched[t]] = 0;
        }
        if (!changed)
            break;
    }
    *ncomm = dyn_gk_compact(label, n, order);
    free(order);
    free(tag);
    free(touched);
    free(bests);
    free(acc);
    return DYN_GK_OK;
}

static double dyn_gk_q_sym(const dyn_gk_csr_t* a, const uint32_t* comm, uint32_t nc, double gamma, double* in, double* tot)
{
    uint32_t u, e, c;
    double m2 = 0.0, q = 0.0;
    for (c = 0; c < nc; c++)
        in[c] = tot[c] = 0.0;
    for (u = 0; u < a->n; u++)
        for (e = a->off[u]; e < a->off[u + 1]; e++) {
            double w = a->w[e];
            tot[comm[u]] += w;
            if (comm[a->tgt[e]] == comm[u])
                in[comm[u]] += w;
            m2 += w;
        }
    if (!(m2 > 0.0))
        return 0.0;
    for (c = 0; c < nc; c++)
        q += in[c] / m2 - gamma * (tot[c] / m2) * (tot[c] / m2);
    return q;
}

typedef struct {
    double* k;
    double* tot;
    double* acc;
    uint32_t* tag;
    uint32_t* touched;
    uint32_t* order;
} dyn_gk_lv_t;

static int dyn_gk_lv_level(const dyn_gk_csr_t* a, double gamma, dyn_prng_t* rng, dyn_gk_lv_t* s, uint32_t* comm, uint32_t* nc, int* improved, const dyn_gk_ctl_t* ctl)
{
    uint32_t n = a->n, i, e, pass;
    double m2 = 0.0;
    *improved = 0;
    for (i = 0; i < n; i++) {
        double d = 0.0;
        for (e = a->off[i]; e < a->off[i + 1]; e++)
            d += a->w[e];
        s->k[i] = d;
        s->tot[i] = d;
        comm[i] = i;
        s->order[i] = i;
        s->tag[i] = 0;
        m2 += d;
    }
    dyn_gk_shuffle(rng, s->order, n);
    if (m2 > 0.0)
        for (pass = 0; pass < DYN_GK_LOUVAIN_MAX_PASSES; pass++) {
            uint32_t moves = 0;
            if (dyn_gk_stopped(ctl))
                return DYN_GK_E_CANCELLED;
            for (i = 0; i < n; i++) {
                uint32_t u = s->order[i], cu = comm[u], best = cu, nt = 0, t;
                double ku = s->k[u], wcu, remove, bestg = 0.0;
                for (e = a->off[u]; e < a->off[u + 1]; e++) {
                    uint32_t v = a->tgt[e], c;
                    if (v == u)
                        continue;
                    c = comm[v];
                    if (s->tag[c] != u + 1) {
                        s->tag[c] = u + 1;
                        s->acc[c] = 0.0;
                        s->touched[nt++] = c;
                    }
                    s->acc[c] += a->w[e];
                }
                wcu = s->tag[cu] == u + 1 ? s->acc[cu] : 0.0;
                s->tot[cu] -= ku;
                remove = -wcu + gamma * s->tot[cu] * ku / m2;
                for (t = 0; t < nt; t++) {
                    uint32_t c = s->touched[t];
                    double g = remove + s->acc[c] - gamma * s->tot[c] * ku / m2;
                    if (g > bestg) {
                        bestg = g;
                        best = c;
                    }
                }
                s->tot[best] += ku;
                for (t = 0; t < nt; t++)
                    s->tag[s->touched[t]] = 0;
                if (best != cu) {
                    comm[u] = best;
                    moves++;
                    *improved = 1;
                }
            }
            if (!moves)
                break;
        }
    *nc = dyn_gk_compact((int32_t*)comm, n, s->order);
    return DYN_GK_OK;
}

static int dyn_gk_lv_aggregate(const dyn_gk_csr_t* a, const uint32_t* comm, uint32_t nc, dyn_gk_csr_t* out)
{
    dyn_gk_arcs_t arcs;
    uint32_t u, e;
    uint64_t at = 0;
    int rc = dyn_gk_arcs_alloc(&arcs, a->m, 1);
    if (rc < 0)
        return rc;
    for (u = 0; u < a->n; u++)
        for (e = a->off[u]; e < a->off[u + 1]; e++) {
            arcs.src[at] = comm[u];
            arcs.dst[at] = comm[a->tgt[e]];
            arcs.w[at] = a->w[e];
            at++;
        }
    rc = dyn_gk_sort_merge(nc, &arcs, 0, out);
    dyn_gk_arcs_free(&arcs);
    return rc;
}

int dyn_gk_louvain(const dyn_gk_csr_t* a, dyn_gk_louvain_opts_t* o, int32_t* comm, uint32_t* ncomm, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_lv_t s;
    dyn_gk_csr_t cur, next;
    dyn_prng_t rng;
    uint32_t n = a->n, i, nc = n, *lc;
    double prev;
    int rc = DYN_GK_OK, owned = 0, improved;
    *ncomm = 0;
    o->levels = 0;
    o->q = 0.0;
    if (n == 0)
        return DYN_GK_OK;
    if (!a->w)
        return DYN_GK_E_BAD_INPUT;
    s.k = (double*)malloc((size_t)n * sizeof(double));
    s.tot = (double*)malloc((size_t)n * sizeof(double));
    s.acc = (double*)malloc((size_t)n * sizeof(double));
    s.tag = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    s.touched = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    s.order = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    lc = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!s.k || !s.tot || !s.acc || !s.tag || !s.touched || !s.order || !lc) {
        rc = DYN_GK_E_OOM;
        goto done;
    }
    dyn_prng_seed(&rng, o->seed);
    for (i = 0; i < n; i++) {
        comm[i] = (int32_t)i;
        lc[i] = i;
    }
    prev = dyn_gk_q_sym(a, lc, n, o->resolution, s.k, s.tot);
    cur = *a;
    for (;;) {
        rc = dyn_gk_lv_level(&cur, o->resolution, &rng, &s, lc, &nc, &improved, ctl);
        if (rc < 0)
            break;
        if (o->levels > 0 && !improved)
            break;
        for (i = 0; i < n; i++)
            comm[i] = (int32_t)lc[comm[i]];
        o->levels++;
        o->q = dyn_gk_q_sym(&cur, lc, nc, o->resolution, s.k, s.tot);
        *ncomm = nc;
        if (o->q - prev <= o->threshold || o->levels >= DYN_GK_LOUVAIN_MAX_LEVELS)
            break;
        prev = o->q;
        rc = dyn_gk_lv_aggregate(&cur, lc, nc, &next);
        if (rc < 0)
            break;
        if (owned)
            dyn_gk_csr_free(&cur);
        cur = next;
        owned = 1;
    }
    if (owned)
        dyn_gk_csr_free(&cur);
done:
    free(s.k);
    free(s.tot);
    free(s.acc);
    free(s.tag);
    free(s.touched);
    free(s.order);
    free(lc);
    return rc;
}

int dyn_gk_modularity(const dyn_gk_csr_t* g, int directed, const int32_t* comm, double resolution, double* q)
{
    uint32_t n = g->n, u, e, c;
    double *kin, *kout, *in, m = 0.0, sum = 0.0;
    *q = 0.0;
    for (u = 0; u < n; u++)
        if (comm[u] < 0 || (uint32_t)comm[u] >= n)
            return DYN_GK_E_BAD_INPUT;
    if (n == 0)
        return DYN_GK_OK;
    kin = (double*)calloc(n, sizeof(double));
    kout = (double*)calloc(n, sizeof(double));
    in = (double*)calloc(n, sizeof(double));
    if (!kin || !kout || !in) {
        free(kin);
        free(kout);
        free(in);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e];
            double w = g->w ? g->w[e] : 1.0, f = 1.0;
            if (!directed && v != u)
                f = 0.5;
            m += w * f;
            if (directed) {
                kout[comm[u]] += w;
                kin[comm[v]] += w;
            } else {
                kout[comm[u]] += v == u ? 2.0 * w : w;
            }
            if (comm[u] == comm[v])
                in[comm[u]] += w * f;
        }
    if (m > 0.0)
        for (c = 0; c < n; c++) {
            if (directed)
                sum += in[c] / m - resolution * kout[c] * kin[c] / (m * m);
            else
                sum += in[c] / m - resolution * (kout[c] / (2.0 * m)) * (kout[c] / (2.0 * m));
        }
    *q = sum;
    free(kin);
    free(kout);
    free(in);
    return DYN_GK_OK;
}
