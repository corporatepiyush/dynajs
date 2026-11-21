#include "graphk-int.h"

#define DYN_GK_PR_CHUNK 1024u
#define DYN_GK_PR_UNITS_PER_WORKER 32768u

typedef struct {
    const dyn_gk_csr_t* in;
    const double* inv;
    const double* p;
    double* x[2];
    double* c[2];
    double* err[2];
    double* dang[2];
    const uint64_t* cost;
    uint32_t nchunks;
    uint32_t max_iter;
    double alpha;
    double limit;
    double d0;
    const dyn_gk_ctl_t* ctl;
    int stop[2];
    uint32_t iters;
    int cur;
    int converged;
    int cancelled;
} dyn_gk_pr_t;

static void dyn_gk_pr_chunk(dyn_gk_pr_t* j, uint32_t ch, int cur, double coef, double unif)
{
    const uint32_t* off = j->in->off;
    const uint32_t* tgt = j->in->tgt;
    const double* w = j->in->w;
    const double* c = j->c[cur];
    const double* x = j->x[cur];
    double* xn = j->x[cur ^ 1];
    double* cn = j->c[cur ^ 1];
    const double* inv = j->inv;
    const double* p = j->p;
    double alpha = j->alpha, e_part = 0.0, d_part = 0.0;
    uint32_t v = ch * DYN_GK_PR_CHUNK, end = v + DYN_GK_PR_CHUNK, n = j->in->n;
    if (end > n)
        end = n;
    for (; v < end; v++) {
        uint32_t a = off[v], b = off[v + 1];
        double pull = 0.0, xv;
        if (w)
            for (; a < b; a++)
                pull += c[tgt[a]] * w[a];
        else
            for (; a < b; a++)
                pull += c[tgt[a]];
        xv = alpha * pull + coef * (p ? p[v] : unif);
        e_part += fabs(xv - x[v]);
        xn[v] = xv;
        cn[v] = xv * inv[v];
        if (inv[v] == 0.0)
            d_part += xv;
    }
    j->err[cur][ch] = e_part;
    j->dang[cur][ch] = d_part;
}

static void dyn_gk_pr_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    dyn_gk_pr_t* j = (dyn_gk_pr_t*)arg;
    uint32_t lo, hi, it, ch;
    double d = j->d0, unif = 1.0 / (double)j->in->n;
    int cur = 0;
    dyn_gk_par_span(j->cost, j->nchunks, worker, nworkers, &lo, &hi);
    for (it = 0; it < j->max_iter; it++) {
        double coef = j->alpha * d + (1.0 - j->alpha), e = 0.0, dn = 0.0;
        for (ch = lo; ch < hi; ch++)
            dyn_gk_pr_chunk(j, ch, cur, coef, unif);
        if (worker == 0 && dyn_gk_stopped(j->ctl))
            j->stop[cur] = 1;
        dyn_par_barrier(par);
        for (ch = 0; ch < j->nchunks; ch++) {
            e += j->err[cur][ch];
            dn += j->dang[cur][ch];
        }
        if (e < j->limit) {
            if (worker == 0) {
                j->converged = 1;
                j->iters = it + 1;
                j->cur = cur ^ 1;
            }
            return;
        }
        if (j->stop[cur]) {
            it++;
            cur ^= 1;
            break;
        }
        cur ^= 1;
        d = dn;
    }
    if (worker == 0) {
        j->iters = it;
        j->cur = cur;
        j->cancelled = j->stop[0] | j->stop[1];
    }
}

int dyn_gk_pagerank(const dyn_gk_csr_t* g, dyn_gk_pr_opts_t* o, double* x, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_csr_t in;
    dyn_gk_pr_t j;
    double *inv = NULL, *p = NULL, *buf = NULL, *part = NULL;
    uint64_t* cost = NULL;
    uint32_t n = g->n, u, e, ch;
    unsigned nw;
    int rc;
    o->iters = 0;
    if (n == 0)
        return DYN_GK_OK;
    rc = dyn_gk_csr_transpose(g, 1, &in);
    if (rc < 0)
        return rc;
    memset(&j, 0, sizeof j);
    j.nchunks = (n + DYN_GK_PR_CHUNK - 1) / DYN_GK_PR_CHUNK;
    inv = (double*)malloc((size_t)n * sizeof(double));
    buf = (double*)malloc((size_t)n * 3 * sizeof(double));
    part = (double*)malloc((size_t)j.nchunks * 4 * sizeof(double));
    cost = (uint64_t*)malloc(((size_t)j.nchunks + 1) * sizeof(uint64_t));
    if (o->pers)
        p = (double*)malloc((size_t)n * sizeof(double));
    if (!inv || !buf || !part || !cost || (o->pers && !p)) {
        rc = DYN_GK_E_OOM;
        goto done;
    }
    if (p) {
        double s = 0.0;
        for (u = 0; u < n; u++) {
            if (!(o->pers[u] >= 0.0) || !isfinite(o->pers[u])) {
                rc = DYN_GK_E_BAD_INPUT;
                goto done;
            }
            s += o->pers[u];
        }
        if (!(s > 0.0)) {
            rc = DYN_GK_E_BAD_INPUT;
            goto done;
        }
        for (u = 0; u < n; u++)
            p[u] = o->pers[u] / s;
    }
    for (u = 0; u < n; u++) {
        double s = 0.0;
        if (g->w)
            for (e = g->off[u]; e < g->off[u + 1]; e++)
                s += g->w[e];
        else
            s = (double)(g->off[u + 1] - g->off[u]);
        inv[u] = s > 0.0 ? 1.0 / s : 0.0;
    }
    j.x[0] = x;
    j.x[1] = buf;
    j.c[0] = buf + n;
    j.c[1] = buf + 2 * (size_t)n;
    j.err[0] = part;
    j.err[1] = part + j.nchunks;
    j.dang[0] = part + 2 * (size_t)j.nchunks;
    j.dang[1] = part + 3 * (size_t)j.nchunks;
    for (u = 0; u < n; u++) {
        x[u] = 1.0 / (double)n;
        j.c[0][u] = x[u] * inv[u];
        if (inv[u] == 0.0)
            j.d0 += x[u];
    }
    cost[0] = 0;
    for (ch = 0; ch < j.nchunks; ch++) {
        uint32_t a = ch * DYN_GK_PR_CHUNK, b = a + DYN_GK_PR_CHUNK;
        if (b > n)
            b = n;
        cost[ch + 1] = cost[ch] + (b - a) + (in.off[b] - in.off[a]);
    }
    j.in = &in;
    j.inv = inv;
    j.p = p;
    j.cost = cost;
    j.alpha = o->alpha;
    j.limit = (double)n * o->tol;
    j.max_iter = o->max_iter;
    j.ctl = ctl;
    nw = dyn_par_plan((size_t)cost[j.nchunks], DYN_GK_PR_UNITS_PER_WORKER, 0);
    if (nw > j.nchunks)
        nw = j.nchunks;
    dyn_par_run(nw, dyn_gk_pr_worker, &j);
    if (j.cur == 1)
        memcpy(x, buf, (size_t)n * sizeof(double));
    o->iters = j.iters;
    if (j.cancelled)
        rc = DYN_GK_E_CANCELLED;
    else
        rc = j.converged ? DYN_GK_OK : DYN_GK_E_NO_CONVERGE;
done:
    free(inv);
    free(buf);
    free(part);
    free(cost);
    free(p);
    dyn_gk_csr_free(&in);
    return rc;
}
