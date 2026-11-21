#include "graphk-int.h"

#define DYN_GK_UNSEEN UINT32_MAX

int dyn_gk_scc(const dyn_gk_csr_t* g, int32_t* comp, uint32_t* ncomp)
{
    uint32_t n = g->n, *idx, *low, *cur, *stk, *frame, r, counter = 0, sp = 0, nc = 0;
    *ncomp = 0;
    if (n == 0)
        return DYN_GK_OK;
    idx = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    low = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    cur = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    stk = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    frame = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!idx || !low || !cur || !stk || !frame) {
        free(idx);
        free(low);
        free(cur);
        free(stk);
        free(frame);
        return DYN_GK_E_OOM;
    }
    for (r = 0; r < n; r++) {
        idx[r] = DYN_GK_UNSEEN;
        comp[r] = -1;
    }
    for (r = 0; r < n; r++) {
        uint32_t fp = 0;
        if (idx[r] != DYN_GK_UNSEEN)
            continue;
        idx[r] = low[r] = counter++;
        cur[r] = g->off[r];
        stk[sp++] = r;
        frame[fp++] = r;
        while (fp) {
            uint32_t v = frame[fp - 1];
            if (cur[v] < g->off[v + 1]) {
                uint32_t w = g->tgt[cur[v]++];
                if (idx[w] == DYN_GK_UNSEEN) {
                    idx[w] = low[w] = counter++;
                    cur[w] = g->off[w];
                    stk[sp++] = w;
                    frame[fp++] = w;
                } else if (comp[w] < 0 && idx[w] < low[v]) {
                    low[v] = idx[w];
                }
                continue;
            }
            fp--;
            if (fp && low[v] < low[frame[fp - 1]])
                low[frame[fp - 1]] = low[v];
            if (low[v] == idx[v]) {
                uint32_t x;
                do {
                    x = stk[--sp];
                    comp[x] = (int32_t)nc;
                } while (x != v);
                nc++;
            }
        }
    }
    *ncomp = nc;
    free(idx);
    free(low);
    free(cur);
    free(stk);
    free(frame);
    return DYN_GK_OK;
}

static uint32_t dyn_gk_uf_find(uint32_t* p, uint32_t x)
{
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static void dyn_gk_uf_union(uint32_t* p, uint32_t a, uint32_t b)
{
    a = dyn_gk_uf_find(p, a);
    b = dyn_gk_uf_find(p, b);
    if (a < b)
        p[b] = a;
    else if (b < a)
        p[a] = b;
}

static void dyn_gk_uf_label(uint32_t* p, uint32_t n, int32_t* comp, uint32_t* ncomp)
{
    uint32_t i, k = 0;
    for (i = 0; i < n; i++) {
        uint32_t r = dyn_gk_uf_find(p, i);
        if (r == i)
            comp[i] = (int32_t)k++;
        else
            comp[i] = comp[r];
    }
    *ncomp = k;
}

int dyn_gk_wcc(const dyn_gk_csr_t* g, int32_t* comp, uint32_t* ncomp)
{
    uint32_t *p, u, e;
    *ncomp = 0;
    if (g->n == 0)
        return DYN_GK_OK;
    p = (uint32_t*)malloc((size_t)g->n * sizeof(uint32_t));
    if (!p)
        return DYN_GK_E_OOM;
    for (u = 0; u < g->n; u++)
        p[u] = u;
    for (u = 0; u < g->n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++)
            dyn_gk_uf_union(p, u, g->tgt[e]);
    dyn_gk_uf_label(p, g->n, comp, ncomp);
    free(p);
    return DYN_GK_OK;
}

int dyn_gk_components(const dyn_gk_csr_t* g, int* comp)
{
    uint32_t *p, u, e, k;
    if (g->n == 0)
        return DYN_GK_OK;
    p = (uint32_t*)malloc((size_t)g->n * sizeof(uint32_t));
    if (!p)
        return DYN_GK_E_OOM;
    for (u = 0; u < g->n; u++)
        p[u] = u;
    for (u = 0; u < g->n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++)
            dyn_gk_uf_union(p, u, g->tgt[e]);
    _Static_assert(sizeof(int) == sizeof(int32_t), "int is 32 bits");
    dyn_gk_uf_label(p, g->n, (int32_t*)comp, &k);
    free(p);
    return DYN_GK_OK;
}

int dyn_gk_topo_levels(const dyn_gk_csr_t* g, int32_t* level, uint32_t* nlevels)
{
    uint32_t n = g->n, *indeg, *q, head = 0, tail = 0, u, e, top = 0;
    *nlevels = 0;
    if (n == 0)
        return DYN_GK_OK;
    indeg = (uint32_t*)calloc(n, sizeof(uint32_t));
    q = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!indeg || !q) {
        free(indeg);
        free(q);
        return DYN_GK_E_OOM;
    }
    for (e = 0; e < g->m; e++)
        indeg[g->tgt[e]]++;
    for (u = 0; u < n; u++) {
        level[u] = 0;
        if (!indeg[u])
            q[tail++] = u;
    }
    while (head < tail) {
        uint32_t x = q[head++];
        int32_t lx = level[x] + 1;
        if ((uint32_t)level[x] + 1 > top)
            top = (uint32_t)level[x] + 1;
        for (e = g->off[x]; e < g->off[x + 1]; e++) {
            uint32_t v = g->tgt[e];
            if (level[v] < lx)
                level[v] = lx;
            if (--indeg[v] == 0)
                q[tail++] = v;
        }
    }
    free(indeg);
    free(q);
    if (tail != n)
        return DYN_GK_E_CYCLE;
    *nlevels = top;
    return DYN_GK_OK;
}

typedef struct {
    uint32_t* off;
    uint32_t* to;
    uint32_t* twin;
    double* cap;
    uint32_t* fwd;
    uint32_t* level;
    uint32_t* it;
    uint32_t* q;
    uint32_t* pa;
    uint32_t* pu;
} dyn_gk_flow_t;

static void dyn_gk_flow_free(dyn_gk_flow_t* f)
{
    free(f->off);
    free(f->to);
    free(f->twin);
    free(f->cap);
    free(f->fwd);
    free(f->level);
    free(f->it);
    free(f->q);
    free(f->pa);
    free(f->pu);
}

static int dyn_gk_flow_bfs(const dyn_gk_flow_t* f, uint32_t n, uint32_t s, uint32_t t)
{
    uint32_t head = 0, tail = 0, u, a;
    for (u = 0; u < n; u++)
        f->level[u] = DYN_GK_UNSEEN;
    f->level[s] = 0;
    f->q[tail++] = s;
    while (head < tail) {
        uint32_t x = f->q[head++];
        for (a = f->off[x]; a < f->off[x + 1]; a++) {
            uint32_t v = f->to[a];
            if (f->cap[a] > 0.0 && f->level[v] == DYN_GK_UNSEEN) {
                f->level[v] = f->level[x] + 1;
                f->q[tail++] = v;
            }
        }
    }
    return f->level[t] != DYN_GK_UNSEEN;
}

int dyn_gk_maxflow(const dyn_gk_csr_t* g, uint32_t s, uint32_t t, double* value, double* flow, uint8_t* side, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_flow_t f;
    uint32_t n = g->n, u, e, *pos;
    uint64_t r2 = 2 * (uint64_t)g->m;
    double total = 0.0;
    int rc = DYN_GK_OK;
    *value = 0.0;
    if (s >= n || t >= n || s == t)
        return DYN_GK_E_BAD_INPUT;
    if (r2 > UINT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    memset(&f, 0, sizeof f);
    f.off = (uint32_t*)calloc((size_t)n + 1, sizeof(uint32_t));
    f.to = (uint32_t*)malloc((r2 ? (size_t)r2 : 1) * sizeof(uint32_t));
    f.twin = (uint32_t*)malloc((r2 ? (size_t)r2 : 1) * sizeof(uint32_t));
    f.cap = (double*)malloc((r2 ? (size_t)r2 : 1) * sizeof(double));
    f.fwd = (uint32_t*)malloc((g->m ? (size_t)g->m : 1) * sizeof(uint32_t));
    f.level = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    f.it = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    f.q = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    f.pa = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    f.pu = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    pos = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!f.off || !f.to || !f.twin || !f.cap || !f.fwd || !f.level || !f.it || !f.q || !f.pa || !f.pu || !pos) {
        free(pos);
        dyn_gk_flow_free(&f);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            f.off[u + 1]++;
            f.off[g->tgt[e] + 1]++;
        }
    for (u = 0; u < n; u++)
        f.off[u + 1] += f.off[u];
    memcpy(pos, f.off, ((size_t)n + 1) * sizeof(uint32_t));
    for (u = 0; u < n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e], a = pos[u]++, b = pos[v]++;
            f.to[a] = v;
            f.cap[a] = g->w ? g->w[e] : 1.0;
            f.twin[a] = b;
            f.to[b] = u;
            f.cap[b] = 0.0;
            f.twin[b] = a;
            f.fwd[e] = a;
        }
    free(pos);
    while (dyn_gk_flow_bfs(&f, n, s, t)) {
        uint32_t top = 0, x = s;
        if (dyn_gk_stopped(ctl)) {
            rc = DYN_GK_E_CANCELLED;
            break;
        }
        memcpy(f.it, f.off, (size_t)n * sizeof(uint32_t));
        for (;;) {
            if (x == t) {
                double b = f.cap[f.pa[0]];
                uint32_t i, cut = top;
                for (i = 1; i < top; i++)
                    if (f.cap[f.pa[i]] < b)
                        b = f.cap[f.pa[i]];
                for (i = 0; i < top; i++) {
                    uint32_t a = f.pa[i];
                    f.cap[a] -= b;
                    f.cap[f.twin[a]] += b;
                    if (cut == top && !(f.cap[a] > 0.0))
                        cut = i;
                }
                total += b;
                top = cut;
                x = f.pu[cut];
                continue;
            }
            while (f.it[x] < f.off[x + 1]) {
                uint32_t a = f.it[x], v = f.to[a];
                if (f.cap[a] > 0.0 && f.level[v] == f.level[x] + 1)
                    break;
                f.it[x]++;
            }
            if (f.it[x] < f.off[x + 1]) {
                f.pa[top] = f.it[x];
                f.pu[top] = x;
                top++;
                x = f.to[f.it[x]];
                continue;
            }
            if (x == s)
                break;
            f.level[x] = DYN_GK_UNSEEN;
            top--;
            x = f.pu[top];
            f.it[x]++;
        }
    }
    if (rc == DYN_GK_OK) {
        if (flow)
            for (e = 0; e < g->m; e++) {
                double c = g->w ? g->w[e] : 1.0, r = f.cap[f.fwd[e]];
                flow[e] = c > r ? c - r : 0.0;
            }
        if (side) {
            dyn_gk_flow_bfs(&f, n, s, t);
            for (u = 0; u < n; u++)
                side[u] = f.level[u] != DYN_GK_UNSEEN;
        }
        *value = total;
    }
    dyn_gk_flow_free(&f);
    return rc;
}
