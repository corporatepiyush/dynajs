#include "graphk-int.h"

int dyn_gk_csr_of_graph_tw(const dyn_graph_t* g, uint32_t mask, const double* tw, dyn_gk_csr_t* out)
{
    uint64_t m = 0;
    uint32_t u, i, at = 0;
    int rc;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            m += (mask >> g->adj[u].e[i].t) & 1u;
    rc = dyn_gk_csr_alloc(out, g->n, m, 1);
    if (rc < 0)
        return rc;
    for (u = 0; u < g->n; u++) {
        const dyn_edge_t* e = g->adj[u].e;
        out->off[u] = at;
        for (i = 0; i < g->adj[u].n; i++) {
            if (!((mask >> e[i].t) & 1u))
                continue;
            out->tgt[at] = (uint32_t)e[i].to;
            out->w[at] = tw ? e[i].w * tw[e[i].t] : e[i].w;
            at++;
        }
    }
    out->off[g->n] = at;
    return DYN_GK_OK;
}

int dyn_gk_topic_pr(const dyn_gk_csr_t* g, uint32_t nt, const double* p, double alpha, double tol, uint32_t max_iter,
    double* out, uint32_t* iters, uint32_t* bad_topic, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_csr_t in;
    uint32_t n = g->n, u, v, t, e, it, active = nt;
    size_t nn = (size_t)n * nt;
    double *inv = NULL, *x = NULL, *xn = NULL, *c = NULL, *cn = NULL, *d = NULL, *dn = NULL, *err = NULL, *pull = NULL;
    uint8_t* live = NULL;
    double limit = (double)n * tol;
    int rc;
    *bad_topic = 0;
    if (n == 0 || nt == 0)
        return DYN_GK_OK;
    rc = dyn_gk_csr_transpose(g, 1, &in);
    if (rc < 0)
        return rc;
    inv = (double*)malloc((size_t)n * sizeof(double));
    x = (double*)malloc(nn * sizeof(double));
    xn = (double*)malloc(nn * sizeof(double));
    c = (double*)malloc(nn * sizeof(double));
    cn = (double*)malloc(nn * sizeof(double));
    d = (double*)calloc(nt, sizeof(double));
    dn = (double*)malloc((size_t)nt * sizeof(double));
    err = (double*)malloc((size_t)nt * sizeof(double));
    pull = (double*)malloc((size_t)nt * sizeof(double));
    live = (uint8_t*)malloc(nt);
    if (!inv || !x || !xn || !c || !cn || !d || !dn || !err || !pull || !live) {
        rc = DYN_GK_E_OOM;
        goto done;
    }
    for (u = 0; u < n; u++) {
        double s = 0.0;
        for (e = g->off[u]; e < g->off[u + 1]; e++)
            s += g->w[e];
        if (!isfinite(s)) {
            rc = DYN_GK_E_BAD_INPUT;
            goto done;
        }
        inv[u] = s > 0.0 ? 1.0 / s : 0.0;
    }
    memset(live, 1, nt);
    for (u = 0; u < n; u++)
        for (t = 0; t < nt; t++) {
            double* xu = x + (size_t)u * nt;
            xu[t] = 1.0 / (double)n;
            c[(size_t)u * nt + t] = xu[t] * inv[u];
            if (inv[u] == 0.0)
                d[t] += xu[t];
        }
    for (it = 0; it < max_iter && active; it++) {
        double* sw;
        if (dyn_gk_stopped(ctl)) {
            rc = DYN_GK_E_CANCELLED;
            goto done;
        }
        for (t = 0; t < nt; t++) {
            err[t] = 0.0;
            dn[t] = 0.0;
        }
        for (v = 0; v < n; v++) {
            const double* xv = x + (size_t)v * nt;
            double* xnv = xn + (size_t)v * nt;
            double* cnv = cn + (size_t)v * nt;
            for (t = 0; t < nt; t++)
                pull[t] = 0.0;
            for (e = in.off[v]; e < in.off[v + 1]; e++) {
                const double* cu = c + (size_t)in.tgt[e] * nt;
                double w = in.w[e];
                for (t = 0; t < nt; t++)
                    pull[t] += cu[t] * w;
            }
            for (t = 0; t < nt; t++) {
                double coef = alpha * d[t] + (1.0 - alpha);
                double y = alpha * pull[t] + coef * p[(size_t)t * n + v];
                err[t] += fabs(y - xv[t]);
                xnv[t] = y;
                cnv[t] = y * inv[v];
                if (inv[v] == 0.0)
                    dn[t] += y;
            }
        }
        for (t = 0; t < nt; t++) {
            if (!live[t] || !(err[t] < limit))
                continue;
            live[t] = 0;
            active--;
            iters[t] = it + 1;
            for (v = 0; v < n; v++)
                out[(size_t)t * n + v] = xn[(size_t)v * nt + t];
        }
        sw = x;
        x = xn;
        xn = sw;
        sw = c;
        c = cn;
        cn = sw;
        sw = d;
        d = dn;
        dn = sw;
    }
    rc = DYN_GK_OK;
    for (t = 0; t < nt; t++)
        if (live[t]) {
            *bad_topic = t;
            iters[t] = it;
            rc = DYN_GK_E_NO_CONVERGE;
            break;
        }
done:
    free(inv);
    free(x);
    free(xn);
    free(c);
    free(cn);
    free(d);
    free(dn);
    free(err);
    free(pull);
    free(live);
    dyn_gk_csr_free(&in);
    return rc;
}
