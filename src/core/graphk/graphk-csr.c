#include "graphk-int.h"

int dyn_gk_csr_alloc(dyn_gk_csr_t* c, uint32_t n, uint64_t m, int with_w)
{
    c->n = 0;
    c->m = 0;
    c->off = NULL;
    c->tgt = NULL;
    c->w = NULL;
    if (m > (uint64_t)UINT32_MAX || n == UINT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    c->off = (uint32_t*)calloc((size_t)n + 1, sizeof(uint32_t));
    c->tgt = (uint32_t*)malloc((m ? (size_t)m : 1) * sizeof(uint32_t));
    if (with_w)
        c->w = (double*)malloc((m ? (size_t)m : 1) * sizeof(double));
    if (!c->off || !c->tgt || (with_w && !c->w)) {
        dyn_gk_csr_free(c);
        return DYN_GK_E_OOM;
    }
    c->n = n;
    c->m = (uint32_t)m;
    return DYN_GK_OK;
}

void dyn_gk_csr_free(dyn_gk_csr_t* c)
{
    if (!c)
        return;
    free(c->off);
    free(c->tgt);
    free(c->w);
    c->off = NULL;
    c->tgt = NULL;
    c->w = NULL;
    c->n = c->m = 0;
}

int dyn_gk_csr_of_graph(const dyn_graph_t* g, int with_w, dyn_gk_csr_t* out)
{
    uint64_t m = 0;
    uint32_t u, i, at = 0;
    int rc;
    for (u = 0; u < g->n; u++)
        m += g->adj[u].n;
    rc = dyn_gk_csr_alloc(out, g->n, m, with_w);
    if (rc < 0)
        return rc;
    for (u = 0; u < g->n; u++) {
        const dyn_edge_t* e = g->adj[u].e;
        uint32_t d = g->adj[u].n;
        out->off[u] = at;
        if (with_w)
            for (i = 0; i < d; i++) {
                out->tgt[at + i] = (uint32_t)e[i].to;
                out->w[at + i] = e[i].w;
            }
        else
            for (i = 0; i < d; i++)
                out->tgt[at + i] = (uint32_t)e[i].to;
        at += d;
    }
    out->off[g->n] = at;
    return DYN_GK_OK;
}

int dyn_gk_csr_validate(const dyn_gk_csr_t* c)
{
    uint32_t u, e;
    if (c->off[0] != 0 || c->off[c->n] != c->m)
        return DYN_GK_E_BAD_INPUT;
    for (u = 0; u < c->n; u++)
        if (c->off[u] > c->off[u + 1])
            return DYN_GK_E_BAD_INPUT;
    for (e = 0; e < c->m; e++)
        if (c->tgt[e] >= c->n)
            return DYN_GK_E_BAD_INPUT;
    if (c->w)
        for (e = 0; e < c->m; e++)
            if (!isfinite(c->w[e]))
                return DYN_GK_E_BAD_INPUT;
    return DYN_GK_OK;
}

int dyn_gk_has_negative_w(const dyn_gk_csr_t* c, int allow_zero)
{
    uint32_t e;
    if (!c->w)
        return 0;
    for (e = 0; e < c->m; e++)
        if (allow_zero ? c->w[e] < 0.0 : !(c->w[e] > 0.0))
            return 1;
    return 0;
}

int dyn_gk_csr_transpose(const dyn_gk_csr_t* in, int with_w, dyn_gk_csr_t* out)
{
    uint32_t u, e, n = in->n, *pos;
    int rc;
    with_w = with_w && in->w;
    rc = dyn_gk_csr_alloc(out, n, in->m, with_w);
    if (rc < 0)
        return rc;
    pos = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!pos) {
        dyn_gk_csr_free(out);
        return DYN_GK_E_OOM;
    }
    for (e = 0; e < in->m; e++)
        out->off[in->tgt[e] + 1]++;
    for (u = 0; u < n; u++)
        out->off[u + 1] += out->off[u];
    memcpy(pos, out->off, ((size_t)n + 1) * sizeof(uint32_t));
    for (u = 0; u < n; u++) {
        uint32_t a = in->off[u], b = in->off[u + 1];
        if (with_w)
            for (e = a; e < b; e++) {
                uint32_t p = pos[in->tgt[e]]++;
                out->tgt[p] = u;
                out->w[p] = in->w[e];
            }
        else
            for (e = a; e < b; e++)
                out->tgt[pos[in->tgt[e]]++] = u;
    }
    free(pos);
    return DYN_GK_OK;
}

void dyn_gk_arcs_free(dyn_gk_arcs_t* a)
{
    free(a->src);
    free(a->dst);
    free(a->w);
}

int dyn_gk_arcs_alloc(dyn_gk_arcs_t* a, uint64_t m, int with_w)
{
    a->m = m;
    a->src = (uint32_t*)malloc((m ? (size_t)m : 1) * sizeof(uint32_t));
    a->dst = (uint32_t*)malloc((m ? (size_t)m : 1) * sizeof(uint32_t));
    a->w = with_w ? (double*)malloc((m ? (size_t)m : 1) * sizeof(double)) : NULL;
    if (!a->src || !a->dst || (with_w && !a->w)) {
        dyn_gk_arcs_free(a);
        return DYN_GK_E_OOM;
    }
    return DYN_GK_OK;
}

int dyn_gk_sort_merge(uint32_t n, const dyn_gk_arcs_t* a, int merge_min, dyn_gk_csr_t* out)
{
    uint32_t *doff, *pos, *ts, u, v, k;
    uint64_t i, p;
    double* tw = NULL;
    int with_w = a->w != NULL, rc;
    rc = dyn_gk_csr_alloc(out, n, a->m, with_w);
    if (rc < 0)
        return rc;
    doff = (uint32_t*)calloc((size_t)n + 1, sizeof(uint32_t));
    pos = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    ts = (uint32_t*)malloc((a->m ? (size_t)a->m : 1) * sizeof(uint32_t));
    if (with_w)
        tw = (double*)malloc((a->m ? (size_t)a->m : 1) * sizeof(double));
    if (!doff || !pos || !ts || (with_w && !tw)) {
        free(doff);
        free(pos);
        free(ts);
        free(tw);
        dyn_gk_csr_free(out);
        return DYN_GK_E_OOM;
    }
    for (i = 0; i < a->m; i++) {
        doff[a->dst[i] + 1]++;
        out->off[a->src[i] + 1]++;
    }
    for (u = 0; u < n; u++) {
        doff[u + 1] += doff[u];
        out->off[u + 1] += out->off[u];
    }
    memcpy(pos, doff, ((size_t)n + 1) * sizeof(uint32_t));
    for (i = 0; i < a->m; i++) {
        uint32_t q = pos[a->dst[i]]++;
        ts[q] = a->src[i];
        if (with_w)
            tw[q] = a->w[i];
    }
    memcpy(pos, out->off, ((size_t)n + 1) * sizeof(uint32_t));
    for (v = 0, p = 0; v < n; v++)
        for (; p < doff[v + 1]; p++) {
            uint32_t q = pos[ts[p]]++;
            out->tgt[q] = v;
            if (with_w)
                out->w[q] = tw[p];
        }
    free(doff);
    free(pos);
    free(ts);
    free(tw);
    for (u = 0, k = 0; u < n; u++) {
        uint32_t a0 = out->off[u], b0 = out->off[u + 1], e, start = k;
        out->off[u] = k;
        for (e = a0; e < b0; e++) {
            if (k > start && out->tgt[k - 1] == out->tgt[e]) {
                if (with_w) {
                    if (!merge_min)
                        out->w[k - 1] += out->w[e];
                    else if (out->w[e] < out->w[k - 1])
                        out->w[k - 1] = out->w[e];
                }
                continue;
            }
            out->tgt[k] = out->tgt[e];
            if (with_w)
                out->w[k] = out->w[e];
            k++;
        }
    }
    out->off[n] = k;
    out->m = k;
    return DYN_GK_OK;
}

int dyn_gk_csr_sym(const dyn_gk_csr_t* in, int keep_loops, int with_w, dyn_gk_csr_t* out)
{
    dyn_gk_arcs_t a;
    uint64_t m = 0, at = 0;
    uint32_t u, e;
    int rc;
    with_w = with_w && in->w;
    for (u = 0; u < in->n; u++)
        for (e = in->off[u]; e < in->off[u + 1]; e++)
            if (keep_loops || in->tgt[e] != u)
                m += 2;
    if (m > (uint64_t)UINT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    rc = dyn_gk_arcs_alloc(&a, m, with_w);
    if (rc < 0)
        return rc;
    for (u = 0; u < in->n; u++)
        for (e = in->off[u]; e < in->off[u + 1]; e++) {
            uint32_t v = in->tgt[e];
            if (!keep_loops && v == u)
                continue;
            a.src[at] = u;
            a.dst[at] = v;
            a.src[at + 1] = v;
            a.dst[at + 1] = u;
            if (with_w)
                a.w[at] = a.w[at + 1] = keep_loops == 2 && v == u ? 2.0 * in->w[e] : in->w[e];
            at += 2;
        }
    rc = dyn_gk_sort_merge(in->n, &a, 0, out);
    dyn_gk_arcs_free(&a);
    return rc;
}

int dyn_gk_csr_dedup(const dyn_gk_csr_t* in, int with_w, dyn_gk_csr_t* out)
{
    dyn_gk_arcs_t a;
    uint64_t m = 0, at = 0;
    uint32_t u, e;
    int rc;
    with_w = with_w && in->w;
    for (u = 0; u < in->n; u++)
        for (e = in->off[u]; e < in->off[u + 1]; e++)
            if (in->tgt[e] != u)
                m++;
    rc = dyn_gk_arcs_alloc(&a, m, with_w);
    if (rc < 0)
        return rc;
    for (u = 0; u < in->n; u++)
        for (e = in->off[u]; e < in->off[u + 1]; e++) {
            if (in->tgt[e] == u)
                continue;
            a.src[at] = u;
            a.dst[at] = in->tgt[e];
            if (with_w)
                a.w[at] = in->w[e];
            at++;
        }
    rc = dyn_gk_sort_merge(in->n, &a, 1, out);
    dyn_gk_arcs_free(&a);
    return rc;
}
