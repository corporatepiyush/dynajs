#include "graphk-int.h"

#define DYN_GK_BC_MAX_GROUPS 32u
#define DYN_GK_BC_ACC_BYTES ((uint64_t)256 << 20)
#define DYN_GK_CL_CHUNK 32u

typedef struct {
    double* key;
    uint32_t* node;
    uint32_t n;
} dyn_gk_mheap_t;

static void dyn_gk_mheap_push(dyn_gk_mheap_t* h, double key, uint32_t node)
{
    uint32_t i = h->n++;
    while (i > 0) {
        uint32_t p = (i - 1) >> 1;
        if (h->key[p] <= key)
            break;
        h->key[i] = h->key[p];
        h->node[i] = h->node[p];
        i = p;
    }
    h->key[i] = key;
    h->node[i] = node;
}

static uint32_t dyn_gk_mheap_pop(dyn_gk_mheap_t* h, double* key)
{
    uint32_t top = h->node[0], i = 0, n = --h->n, ln;
    double lk;
    *key = h->key[0];
    if (n == 0)
        return top;
    lk = h->key[n];
    ln = h->node[n];
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n && h->key[c + 1] < h->key[c])
            c++;
        if (lk <= h->key[c])
            break;
        h->key[i] = h->key[c];
        h->node[i] = h->node[c];
        i = c;
    }
    h->key[i] = lk;
    h->node[i] = ln;
    return top;
}

typedef struct {
    int32_t* dist;
    double* best;
    double* sigma;
    double* delta;
    uint32_t* order;
    uint8_t* done;
    dyn_gk_mheap_t h;
} dyn_gk_bcs_t;

static void dyn_gk_bcs_free(dyn_gk_bcs_t* st)
{
    free(st->dist);
    free(st->best);
    free(st->sigma);
    free(st->delta);
    free(st->order);
    free(st->done);
    free(st->h.key);
    free(st->h.node);
}

static int dyn_gk_bcs_alloc(dyn_gk_bcs_t* st, const dyn_gk_csr_t* g)
{
    uint32_t n = g->n, v;
    memset(st, 0, sizeof *st);
    st->sigma = (double*)calloc(n, sizeof(double));
    st->delta = (double*)calloc(n, sizeof(double));
    st->order = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (g->w) {
        st->best = (double*)malloc((size_t)n * sizeof(double));
        st->done = (uint8_t*)calloc(n, 1);
        st->h.key = (double*)malloc(((size_t)g->m + 1) * sizeof(double));
        st->h.node = (uint32_t*)malloc(((size_t)g->m + 1) * sizeof(uint32_t));
        if (st->best)
            for (v = 0; v < n; v++)
                st->best[v] = DYN_INFINITY;
    } else {
        st->dist = (int32_t*)malloc((size_t)n * sizeof(int32_t));
        if (st->dist)
            for (v = 0; v < n; v++)
                st->dist[v] = -1;
    }
    if (!st->sigma || !st->delta || !st->order || (g->w ? (!st->best || !st->done || !st->h.key || !st->h.node) : !st->dist)) {
        dyn_gk_bcs_free(st);
        return DYN_GK_E_OOM;
    }
    return DYN_GK_OK;
}

static uint32_t dyn_gk_bc_bfs(dyn_gk_bcs_t* st, const dyn_gk_csr_t* g, uint32_t s)
{
    const uint32_t* off = g->off;
    const uint32_t* tgt = g->tgt;
    int32_t* dist = st->dist;
    double* sigma = st->sigma;
    uint32_t* order = st->order;
    uint32_t head = 0, tail = 1, e;
    order[0] = s;
    dist[s] = 0;
    sigma[s] = 1.0;
    while (head < tail) {
        uint32_t v = order[head++];
        int32_t dv = dist[v] + 1;
        double sv = sigma[v];
        for (e = off[v]; e < off[v + 1]; e++) {
            uint32_t w = tgt[e];
            if (dist[w] < 0) {
                dist[w] = dv;
                order[tail++] = w;
            }
            if (dist[w] == dv)
                sigma[w] += sv;
        }
    }
    return tail;
}

static uint32_t dyn_gk_bc_dijkstra(dyn_gk_bcs_t* st, const dyn_gk_csr_t* g, uint32_t s)
{
    const uint32_t* off = g->off;
    const uint32_t* tgt = g->tgt;
    const double* w = g->w;
    double* best = st->best;
    double* sigma = st->sigma;
    uint8_t* done = st->done;
    uint32_t cnt = 0, e;
    st->h.n = 0;
    best[s] = 0.0;
    sigma[s] = 1.0;
    dyn_gk_mheap_push(&st->h, 0.0, s);
    while (st->h.n) {
        double d;
        uint32_t v = dyn_gk_mheap_pop(&st->h, &d);
        if (done[v])
            continue;
        done[v] = 1;
        st->order[cnt++] = v;
        for (e = off[v]; e < off[v + 1]; e++) {
            uint32_t x = tgt[e];
            double nd = d + w[e];
            if (!done[x] && nd < best[x]) {
                best[x] = nd;
                sigma[x] = sigma[v];
                dyn_gk_mheap_push(&st->h, nd, x);
            } else if (nd == best[x]) {
                sigma[x] += sigma[v];
            }
        }
    }
    return cnt;
}

static void dyn_gk_bc_source(dyn_gk_bcs_t* st, const dyn_gk_csr_t* g, uint32_t s, int endpoints, double* acc)
{
    const uint32_t* off = g->off;
    const uint32_t* tgt = g->tgt;
    double* sigma = st->sigma;
    double* delta = st->delta;
    uint32_t cnt, i, e;
    if (g->w) {
        const double* w = g->w;
        double* best = st->best;
        cnt = dyn_gk_bc_dijkstra(st, g, s);
        for (i = cnt; i-- > 0;) {
            uint32_t v = st->order[i];
            double sum = 0.0, bv = best[v];
            for (e = off[v]; e < off[v + 1]; e++) {
                uint32_t x = tgt[e];
                if (bv + w[e] == best[x] && x != v)
                    sum += (1.0 + delta[x]) / sigma[x];
            }
            delta[v] = sigma[v] * sum;
            if (v != s)
                acc[v] += endpoints ? delta[v] + 1.0 : delta[v];
        }
        for (i = 0; i < cnt; i++) {
            uint32_t v = st->order[i];
            best[v] = DYN_INFINITY;
            sigma[v] = 0.0;
            delta[v] = 0.0;
            st->done[v] = 0;
        }
    } else {
        int32_t* dist = st->dist;
        cnt = dyn_gk_bc_bfs(st, g, s);
        for (i = cnt; i-- > 0;) {
            uint32_t v = st->order[i];
            int32_t dn = dist[v] + 1;
            double sum = 0.0;
            for (e = off[v]; e < off[v + 1]; e++) {
                uint32_t x = tgt[e];
                if (dist[x] == dn)
                    sum += (1.0 + delta[x]) / sigma[x];
            }
            delta[v] = sigma[v] * sum;
            if (v != s)
                acc[v] += endpoints ? delta[v] + 1.0 : delta[v];
        }
        for (i = 0; i < cnt; i++) {
            uint32_t v = st->order[i];
            dist[v] = -1;
            sigma[v] = 0.0;
            delta[v] = 0.0;
        }
    }
    if (endpoints)
        acc[s] += (double)(cnt - 1);
}

typedef struct {
    const dyn_gk_csr_t* g;
    const uint32_t* src;
    uint32_t k;
    uint32_t groups;
    int endpoints;
    double* acc;
    _Atomic uint32_t next;
    _Atomic int stop;
    _Atomic int oom;
    const dyn_gk_ctl_t* ctl;
} dyn_gk_bc_t;

static void dyn_gk_bc_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    dyn_gk_bc_t* j = (dyn_gk_bc_t*)arg;
    dyn_gk_bcs_t st;
    (void)nworkers;
    (void)par;
    if (dyn_gk_bcs_alloc(&st, j->g) < 0) {
        atomic_store_explicit(&j->oom, 1, memory_order_relaxed);
        return;
    }
    for (;;) {
        uint32_t grp = atomic_fetch_add_explicit(&j->next, 1, memory_order_relaxed), i;
        double* acc;
        if (grp >= j->groups)
            break;
        acc = j->acc + (size_t)grp * j->g->n;
        for (i = grp; i < j->k; i += j->groups) {
            if (atomic_load_explicit(&j->stop, memory_order_relaxed))
                break;
            if (worker == 0 && dyn_gk_stopped(j->ctl)) {
                atomic_store_explicit(&j->stop, 1, memory_order_relaxed);
                break;
            }
            dyn_gk_bc_source(&st, j->g, j->src[i], j->endpoints, acc);
        }
    }
    dyn_gk_bcs_free(&st);
}

int dyn_gk_betweenness(const dyn_gk_csr_t* g, const uint32_t* src, uint32_t k, int endpoints, double* bc, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_bc_t j;
    uint32_t n = g->n, grp, v, groups;
    unsigned nw;
    double* acc;
    for (v = 0; v < n; v++)
        bc[v] = 0.0;
    if (n == 0 || k == 0)
        return DYN_GK_OK;
    for (v = 0; v < k; v++)
        if (src[v] >= n)
            return DYN_GK_E_BAD_INPUT;
    groups = k < DYN_GK_BC_MAX_GROUPS ? k : DYN_GK_BC_MAX_GROUPS;
    while (groups > 1 && (uint64_t)groups * n * sizeof(double) > DYN_GK_BC_ACC_BYTES)
        groups--;
    if (groups == 1) {
        acc = bc;
    } else {
        acc = (double*)calloc((size_t)groups * n, sizeof(double));
        if (!acc)
            return DYN_GK_E_OOM;
    }
    j.g = g;
    j.src = src;
    j.k = k;
    j.groups = groups;
    j.endpoints = endpoints;
    j.acc = acc;
    j.ctl = ctl;
    atomic_init(&j.next, 0);
    atomic_init(&j.stop, 0);
    atomic_init(&j.oom, 0);
    nw = dyn_par_plan((size_t)k * ((size_t)n + g->m), (size_t)1 << 16, 0);
    if (nw > groups)
        nw = groups;
    dyn_par_run(nw, dyn_gk_bc_worker, &j);
    if (groups > 1) {
        for (grp = 0; grp < groups; grp++) {
            const double* a = acc + (size_t)grp * n;
            for (v = 0; v < n; v++)
                bc[v] += a[v];
        }
        free(acc);
    }
    if (atomic_load_explicit(&j.stop, memory_order_relaxed))
        return DYN_GK_E_CANCELLED;
    if (atomic_load_explicit(&j.oom, memory_order_relaxed) && atomic_load_explicit(&j.next, memory_order_relaxed) < groups)
        return DYN_GK_E_OOM;
    return DYN_GK_OK;
}

void dyn_gk_betweenness_rescale(double* bc, uint32_t n, int normalized, int directed, int endpoints, const uint32_t* src, uint32_t k, int sampled)
{
    double N = endpoints ? (double)n : (double)n - 1.0, K = sampled ? (double)k : N, corr = directed ? 1.0 : 2.0;
    double scale, s_src, s_non;
    uint32_t v, i;
    uint8_t* mark;
    if (N < 2.0)
        return;
    if (!sampled || endpoints) {
        scale = normalized ? 1.0 / (K * (N - 1.0)) : N / (K * corr);
        if (scale != 1.0)
            for (v = 0; v < n; v++)
                bc[v] *= scale;
        return;
    }
    if (normalized) {
        s_src = K > 1.0 ? 1.0 / ((K - 1.0) * (N - 1.0)) : DYN_NAN;
        s_non = 1.0 / (K * (N - 1.0));
    } else {
        s_src = K > 1.0 ? N / ((K - 1.0) * corr) : DYN_NAN;
        s_non = N / (K * corr);
    }
    mark = (uint8_t*)calloc(n, 1);
    if (mark) {
        for (i = 0; i < k; i++)
            mark[src[i]] = 1;
        for (v = 0; v < n; v++)
            bc[v] *= mark[v] ? s_src : s_non;
        free(mark);
        return;
    }
    for (v = 0; v < n; v++) {
        int in = 0;
        for (i = 0; i < k && !in; i++)
            in = src[i] == v;
        bc[v] *= in ? s_src : s_non;
    }
}

typedef struct {
    const dyn_gk_csr_t* g;
    double* out;
    int wf;
    _Atomic uint32_t next;
    _Atomic int stop;
    _Atomic int oom;
    const dyn_gk_ctl_t* ctl;
} dyn_gk_cl_t;

static void dyn_gk_cl_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    dyn_gk_cl_t* j = (dyn_gk_cl_t*)arg;
    const dyn_gk_csr_t* g = j->g;
    uint32_t n = g->n, *stamp, *q, epoch = 0;
    double* best = NULL;
    dyn_gk_mheap_t h;
    (void)nworkers;
    (void)par;
    memset(&h, 0, sizeof h);
    stamp = (uint32_t*)calloc(n, sizeof(uint32_t));
    q = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (g->w) {
        best = (double*)malloc((size_t)n * sizeof(double));
        h.key = (double*)malloc(((size_t)g->m + 1) * sizeof(double));
        h.node = (uint32_t*)malloc(((size_t)g->m + 1) * sizeof(uint32_t));
    }
    if (!stamp || !q || (g->w && (!best || !h.key || !h.node))) {
        atomic_store_explicit(&j->oom, 1, memory_order_relaxed);
        goto done;
    }
    for (;;) {
        uint32_t c = atomic_fetch_add_explicit(&j->next, 1, memory_order_relaxed), u, end;
        uint64_t lo = (uint64_t)c * DYN_GK_CL_CHUNK;
        if (lo >= n || atomic_load_explicit(&j->stop, memory_order_relaxed))
            break;
        if (worker == 0 && dyn_gk_stopped(j->ctl)) {
            atomic_store_explicit(&j->stop, 1, memory_order_relaxed);
            break;
        }
        end = (uint32_t)lo + DYN_GK_CL_CHUNK;
        if (end > n)
            end = n;
        for (u = (uint32_t)lo; u < end; u++) {
            double tot = 0.0, reach, cc = 0.0;
            uint32_t cnt = 0, e;
            if (++epoch == 0) {
                memset(stamp, 0, (size_t)n * sizeof(uint32_t));
                epoch = 1;
            }
            if (g->w) {
                h.n = 0;
                stamp[u] = epoch;
                best[u] = 0.0;
                dyn_gk_mheap_push(&h, 0.0, u);
                while (h.n) {
                    double d;
                    uint32_t v = dyn_gk_mheap_pop(&h, &d);
                    if (d > best[v])
                        continue;
                    q[cnt++] = v;
                    tot += d;
                    for (e = g->off[v]; e < g->off[v + 1]; e++) {
                        uint32_t x = g->tgt[e];
                        double nd = d + g->w[e];
                        if (stamp[x] != epoch || nd < best[x]) {
                            stamp[x] = epoch;
                            best[x] = nd;
                            dyn_gk_mheap_push(&h, nd, x);
                        }
                    }
                }
            } else {
                uint32_t head = 0, lvl_end = 1;
                uint64_t isum = 0, dv = 1;
                stamp[u] = epoch;
                q[cnt++] = u;
                while (head < cnt) {
                    uint32_t v = q[head++];
                    for (e = g->off[v]; e < g->off[v + 1]; e++) {
                        uint32_t x = g->tgt[e], fresh = stamp[x] != epoch;
                        stamp[x] = epoch;
                        q[cnt] = x;
                        cnt += fresh;
                    }
                    if (head == lvl_end) {
                        isum += dv * (cnt - lvl_end);
                        lvl_end = cnt;
                        dv++;
                    }
                }
                tot = (double)isum;
            }
            reach = (double)cnt - 1.0;
            if (tot > 0.0 && n > 1) {
                cc = reach / tot;
                if (j->wf)
                    cc *= reach / ((double)n - 1.0);
            }
            j->out[u] = cc;
        }
    }
done:
    free(stamp);
    free(q);
    free(best);
    free(h.key);
    free(h.node);
}

int dyn_gk_closeness(const dyn_gk_csr_t* rev, int wf_improved, double* out, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_cl_t j;
    unsigned nw;
    if (rev->n == 0)
        return DYN_GK_OK;
    j.g = rev;
    j.out = out;
    j.wf = wf_improved;
    j.ctl = ctl;
    atomic_init(&j.next, 0);
    atomic_init(&j.stop, 0);
    atomic_init(&j.oom, 0);
    nw = dyn_par_plan((size_t)rev->n * ((size_t)rev->n + rev->m), (size_t)1 << 18, 0);
    dyn_par_run(nw, dyn_gk_cl_worker, &j);
    if (atomic_load_explicit(&j.stop, memory_order_relaxed))
        return DYN_GK_E_CANCELLED;
    if (atomic_load_explicit(&j.oom, memory_order_relaxed) && (uint64_t)atomic_load_explicit(&j.next, memory_order_relaxed) * DYN_GK_CL_CHUNK < rev->n)
        return DYN_GK_E_OOM;
    return DYN_GK_OK;
}
