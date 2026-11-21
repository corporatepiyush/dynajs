#include "graphk-int.h"

typedef struct {
    dyn_hp_item* a;
    uint32_t n, cap;
} dyn_gk_heap;

typedef struct {
    uint64_t key;
    int u, v;
    double w;
} dyn_gk_kedge;

static int dyn_gk_heap_push(dyn_gk_heap* h, double key, int node)
{
    uint32_t i;
    if (h->n == h->cap) {
        uint32_t nc;
        dyn_hp_item* na;
        if (h->cap > UINT32_MAX / 2)
            return -1;
        nc = h->cap ? h->cap * 2 : 16;
        na = (dyn_hp_item*)realloc(h->a, (size_t)nc * sizeof(*na));
        if (!na)
            return -1;
        h->a = na;
        h->cap = nc;
    }
    i = h->n++;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (h->a[p].key <= key)
            break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i].key = key;
    h->a[i].node = node;
    return 0;
}

static int dyn_gk_heap_pop(dyn_gk_heap* h, dyn_hp_item* out)
{
    uint32_t i = 0;
    dyn_hp_item x;
    if (h->n == 0)
        return 0;
    *out = h->a[0];
    x = h->a[--h->n];
    for (;;) {
        uint32_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        double sk = x.key;
        if (l < h->n && h->a[l].key < sk) {
            s = l;
            sk = h->a[l].key;
        }
        if (r < h->n && h->a[r].key < sk)
            s = r;
        if (s == i)
            break;
        h->a[i] = h->a[s];
        i = s;
    }
    if (h->n)
        h->a[i] = x;
    return 1;
}

int dyn_gk_dijkstra(const dyn_gk_csr_t* g, uint32_t src, double* dist, int32_t* pred)
{
    dyn_gk_heap h = { NULL, 0, 0 };
    dyn_hp_item it;
    uint32_t i, e, n;
    if (g->w)
        for (e = 0; e < g->m; e++)
            if (g->w[e] < 0.0)
                return DYN_GK_E_NEG_WEIGHT;
    n = g->n;
    for (i = 0; i < n; i++)
        dist[i] = DYN_INFINITY;
    if (pred)
        for (i = 0; i < n; i++)
            pred[i] = -1;
    dist[src] = 0.0;
    if (dyn_gk_heap_push(&h, 0.0, (int)src))
        return DYN_GK_E_OOM;
    while (dyn_gk_heap_pop(&h, &it)) {
        uint32_t u = (uint32_t)it.node;
        if (it.key > dist[u])
            continue;
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e];
            double nd = it.key + dyn_gk_wt(g, e);
            if (nd < dist[v]) {
                dist[v] = nd;
                if (pred)
                    pred[v] = (int32_t)u;
                if (dyn_gk_heap_push(&h, nd, (int)v)) {
                    free(h.a);
                    return DYN_GK_E_OOM;
                }
            }
        }
    }
    free(h.a);
    return DYN_GK_OK;
}

int dyn_gk_bellman_ford(const dyn_gk_csr_t* g, uint32_t src, double* dist)
{
    uint32_t iter, u, i, e;
    int changed;
    for (i = 0; i < g->n; i++)
        dist[i] = DYN_INFINITY;
    dist[src] = 0.0;
    for (iter = 0; iter < g->n; iter++) {
        changed = 0;
        for (u = 0; u < g->n; u++) {
            if (dist[u] == DYN_INFINITY)
                continue;
            for (e = g->off[u]; e < g->off[u + 1]; e++) {
                uint32_t v = g->tgt[e];
                double nd = dist[u] + dyn_gk_wt(g, e);
                if (nd == -DYN_INFINITY) {
                    dist[v] = -DYN_INFINITY;
                    changed = 1;
                    continue;
                }
                if (nd < dist[v]) {
                    if (iter == g->n - 1)
                        return DYN_GK_E_NEG_CYCLE;
                    dist[v] = nd;
                    changed = 1;
                }
            }
        }
        if (!changed)
            break;
    }
    return DYN_GK_OK;
}

static void dyn_gk_floyd_row(double* restrict di, const double* restrict dk, double dik, uint32_t n)
{
    uint32_t j;
    for (j = 0; j < n; j++) {
        double nd = dik + dk[j];
        di[j] = nd < di[j] ? nd : di[j];
    }
}

int dyn_gk_floyd(const dyn_gk_csr_t* g, double** out)
{
    double* d;
    uint32_t n = g->n, i, j, k, e;
    *out = NULL;
    if (n > DYN_FLOYD_MAX_N)
        return DYN_GK_E_TOO_LARGE;
    d = (double*)malloc((size_t)n * n * sizeof(double));
    if (n && !d)
        return DYN_GK_E_OOM;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            d[i * n + j] = (i == j) ? 0.0 : DYN_INFINITY;
    for (i = 0; i < n; i++)
        for (e = g->off[i]; e < g->off[i + 1]; e++) {
            uint32_t v = g->tgt[e];
            double w = dyn_gk_wt(g, e);
            if (w < d[i * n + v])
                d[i * n + v] = w;
        }
    for (k = 0; k < n; k++)
        for (i = 0; i < n; i++) {
            double dik = d[(size_t)i * n + k];
            if (dik == DYN_INFINITY)
                continue;
            if (i == k) {
                for (j = 0; j < n; j++) {
                    double nd = d[(size_t)i * n + k] + d[(size_t)k * n + j];
                    if (nd < d[(size_t)i * n + j])
                        d[(size_t)i * n + j] = nd;
                }
            } else
                dyn_gk_floyd_row(d + (size_t)i * n, d + (size_t)k * n, dik, n);
        }
    for (i = 0; i < n; i++)
        if (d[i * n + i] < 0.0) {
            free(d);
            return DYN_GK_E_NEG_CYCLE;
        }
    *out = d;
    return DYN_GK_OK;
}

int dyn_gk_pred_path(const int32_t* pred, uint32_t n, uint32_t dst, int** ppath, uint32_t* plen)
{
    int* rev;
    uint32_t len = 0, k;
    int32_t cur = (int32_t)dst;
    *ppath = NULL;
    *plen = 0;
    rev = (int*)malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!rev)
        return DYN_GK_E_OOM;
    while (cur != -1 && len < n) {
        rev[len++] = cur;
        cur = pred[cur];
    }
    if (cur != -1) {
        free(rev);
        return DYN_GK_E_PRED_CYCLE;
    }
    for (k = 0; k < len / 2; k++) {
        int t = rev[k];
        rev[k] = rev[len - 1 - k];
        rev[len - 1 - k] = t;
    }
    *ppath = rev;
    *plen = len;
    return DYN_GK_OK;
}

int dyn_gk_astar(const dyn_gk_csr_t* g, uint32_t src, uint32_t dst, const double* h, double* pdist,
    int** ppath, uint32_t* plen)
{
    double* gscore;
    int32_t* prev;
    char* closed;
    uint32_t i, e, n0 = g->n;
    size_t pushes, push_cap;
    dyn_gk_heap heap = { NULL, 0, 0 };
    dyn_hp_item it;
    int rc = DYN_GK_OK;

    *ppath = NULL;
    *plen = 0;
    gscore = (double*)malloc((size_t)n0 * sizeof(double));
    prev = (int32_t*)malloc((size_t)n0 * sizeof(int32_t));
    closed = (char*)calloc(n0, 1);
    if (!gscore || !prev || !closed) {
        rc = DYN_GK_E_OOM;
        goto out;
    }
    for (i = 0; i < n0; i++) {
        gscore[i] = DYN_INFINITY;
        prev[i] = -1;
    }
    gscore[src] = 0.0;
    push_cap = (size_t)n0 * 64 + 1024;
    pushes = 1;
    if (dyn_gk_heap_push(&heap, h[src], (int)src)) {
        rc = DYN_GK_E_OOM;
        goto out;
    }
    while (dyn_gk_heap_pop(&heap, &it)) {
        uint32_t u = (uint32_t)it.node;
        if (closed[u])
            continue;
        closed[u] = 1;
        if (u == dst)
            break;
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e];
            double w = dyn_gk_wt(g, e);
            if (gscore[u] + w < gscore[v]) {
                gscore[v] = gscore[u] + w;
                prev[v] = (int32_t)u;
                if (closed[v])
                    closed[v] = 0;
                if (++pushes > push_cap) {
                    rc = DYN_GK_E_BUDGET;
                    goto out;
                }
                if (dyn_gk_heap_push(&heap, gscore[v] + h[v], (int)v)) {
                    rc = DYN_GK_E_OOM;
                    goto out;
                }
            }
        }
    }
    *pdist = gscore[dst];
    if (gscore[dst] != DYN_INFINITY)
        rc = dyn_gk_pred_path(prev, n0, dst, ppath, plen);
out:
    free(heap.a);
    free(gscore);
    free(prev);
    free(closed);
    return rc;
}

int dyn_gk_dag_longest(const dyn_gk_csr_t* g, double* dist, int32_t* pred, uint32_t* pend)
{
    uint32_t *indeg, *q, head = 0, tail = 0, u, e, end = 0;
    *pend = 0;
    indeg = (uint32_t*)calloc(g->n ? g->n : 1, sizeof(uint32_t));
    q = (uint32_t*)malloc((size_t)(g->n ? g->n : 1) * sizeof(uint32_t));
    if (!indeg || !q) {
        free(indeg);
        free(q);
        return DYN_GK_E_OOM;
    }
    for (e = 0; e < g->m; e++)
        indeg[g->tgt[e]]++;
    for (u = 0; u < g->n; u++) {
        dist[u] = -DYN_INFINITY;
        pred[u] = -1;
        if (indeg[u] == 0)
            q[tail++] = u;
    }
    while (head < tail) {
        u = q[head++];
        if (!(dist[u] >= 0.0)) {
            dist[u] = 0.0;
            pred[u] = -1;
        }
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e];
            double c = dist[u] + dyn_gk_wt(g, e);
            if (c > dist[v] || (c == dist[v] && (int32_t)u < pred[v])) {
                dist[v] = c;
                pred[v] = (int32_t)u;
            }
            if (--indeg[v] == 0)
                q[tail++] = v;
        }
    }
    free(indeg);
    free(q);
    if (tail != g->n)
        return DYN_GK_E_CYCLE;
    for (u = 1; u < g->n; u++)
        if (dist[u] > dist[end])
            end = u;
    *pend = end;
    return DYN_GK_OK;
}

static uint64_t dyn_gk_kedge_key(double w)
{
    uint64_t b;
    if (w == 0)
        w = 0.0;
    memcpy(&b, &w, sizeof b);
    return (b >> 63) ? ~b : b | ((uint64_t)1 << 63);
}

static dyn_gk_kedge* dyn_gk_kedge_radix(dyn_gk_kedge* es, uint32_t n)
{
    uint32_t hist[8][256];
    dyn_gk_kedge *src = es, *dst = (dyn_gk_kedge*)malloc((size_t)n * sizeof(dyn_gk_kedge));
    uint32_t i;
    int d;

    if (!dst)
        return NULL;
    memset(hist, 0, sizeof hist);
    for (i = 0; i < n; i++)
        for (d = 0; d < 8; d++)
            hist[d][(src[i].key >> (d * 8)) & 0xff]++;
    for (d = 0; d < 8; d++) {
        uint32_t sum = 0, b, t;
        dyn_gk_kedge* x;
        if (hist[d][(src[0].key >> (d * 8)) & 0xff] == n)
            continue;
        for (b = 0; b < 256; b++) {
            t = hist[d][b];
            hist[d][b] = sum;
            sum += t;
        }
        for (i = 0; i < n; i++)
            dst[hist[d][(src[i].key >> (d * 8)) & 0xff]++] = src[i];
        x = src;
        src = dst;
        dst = x;
    }
    free(dst);
    return src;
}

int dyn_gk_mst(const dyn_gk_csr_t* g, dyn_gk_edge_t** pout, uint32_t* pnout, double* ptotal)
{
    dyn_gk_kedge* es;
    dyn_gk_edge_t* out;
    uint32_t ne = 0, u, i, e, cap = 16, nout = 0;
    int* parent;
    double total = 0.0;

    *pout = NULL;
    *pnout = 0;
    *ptotal = 0.0;
    es = (dyn_gk_kedge*)malloc(cap * sizeof(dyn_gk_kedge));
    parent = (int*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    if (!es || !parent) {
        free(es);
        free(parent);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < g->n; u++)
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            int v = (int)g->tgt[e];
            if ((uint32_t)v < u)
                continue;
            if (ne == cap) {
                dyn_gk_kedge* n2;
                if (cap > UINT32_MAX / 2
#if SIZE_MAX < UINT64_MAX
                    || cap > (SIZE_MAX / sizeof(dyn_gk_kedge)) / 2
#endif
                ) {
                    free(es);
                    free(parent);
                    return DYN_GK_E_OOM;
                }
                n2 = (dyn_gk_kedge*)realloc(es, (cap *= 2) * sizeof(dyn_gk_kedge));
                if (!n2) {
                    free(es);
                    free(parent);
                    return DYN_GK_E_OOM;
                }
                es = n2;
            }
            es[ne].u = (int)u;
            es[ne].v = v;
            es[ne].w = dyn_gk_wt(g, e);
            es[ne].key = dyn_gk_kedge_key(es[ne].w);
            ne++;
        }
    if (ne > DYN_MST_RADIX_MIN) {
        dyn_gk_kedge* sorted = dyn_gk_kedge_radix(es, ne);
        if (!sorted) {
            free(es);
            free(parent);
            return DYN_GK_E_OOM;
        }
        es = sorted;
    } else {
        for (i = 1; i < ne; i++) {
            dyn_gk_kedge key = es[i];
            uint32_t j = i;
            while (j > 0 && es[j - 1].key > key.key) {
                es[j] = es[j - 1];
                j--;
            }
            es[j] = key;
        }
    }
    out = (dyn_gk_edge_t*)malloc((size_t)(g->n ? g->n : 1) * sizeof(dyn_gk_edge_t));
    if (!out) {
        free(es);
        free(parent);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < g->n; u++)
        parent[u] = (int)u;
    for (i = 0; i < ne; i++) {
        int a = es[i].u, b = es[i].v, ra, rb;
        for (ra = a; parent[ra] != ra; ra = parent[ra])
            parent[ra] = parent[parent[ra]];
        for (rb = b; parent[rb] != rb; rb = parent[rb])
            parent[rb] = parent[parent[rb]];
        if (ra == rb)
            continue;
        parent[ra] = rb;
        total += es[i].w;
        out[nout].u = a;
        out[nout].v = b;
        out[nout].w = es[i].w;
        nout++;
    }
    free(es);
    free(parent);
    *pout = out;
    *pnout = nout;
    *ptotal = total;
    return DYN_GK_OK;
}
