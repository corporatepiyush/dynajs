#include "graphk-int.h"

#define DYN_GK_TRI_CHUNK 256u
#define DYN_GK_TRI_UNITS_PER_WORKER 65536u
#define DYN_GK_LINK_PAR_MIN 4096u
#define DYN_GK_GALLOP_RATIO 32u

typedef struct {
    const uint32_t* fo;
    const uint32_t* ft;
    uint32_t n;
    _Atomic uint64_t* t;
    _Atomic uint32_t next;
    _Atomic int stop;
    const dyn_gk_ctl_t* ctl;
} dyn_gk_tri_t;

static uint32_t dyn_gk_seek(const uint32_t* a, uint32_t lo, uint32_t hi, uint32_t key)
{
    uint32_t step = 1;
    while (lo + step < hi && a[lo + step] < key) {
        lo += step;
        step <<= 1;
    }
    if (lo + step < hi)
        hi = lo + step + 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void dyn_gk_tri_vertex(dyn_gk_tri_t* j, uint32_t u)
{
    const uint32_t* fo = j->fo;
    const uint32_t* ft = j->ft;
    uint32_t a0 = fo[u], a1 = fo[u + 1], e;
    uint64_t own = 0;
    for (e = a0; e < a1; e++) {
        uint32_t v = ft[e], i = a0, k = fo[v], kend = fo[v + 1];
        uint64_t c = 0;
        uint32_t la = a1 - a0, lb = kend - k;
        if (la > DYN_GK_GALLOP_RATIO * lb || lb > DYN_GK_GALLOP_RATIO * la) {
            if (la > lb) {
                for (; k < kend && i < a1; k++) {
                    i = dyn_gk_seek(ft, i, a1, ft[k]);
                    if (i < a1 && ft[i] == ft[k]) {
                        c++;
                        atomic_fetch_add_explicit(&j->t[ft[k]], 1, memory_order_relaxed);
                    }
                }
            } else {
                for (; i < a1 && k < kend; i++) {
                    k = dyn_gk_seek(ft, k, kend, ft[i]);
                    if (k < kend && ft[k] == ft[i]) {
                        c++;
                        atomic_fetch_add_explicit(&j->t[ft[i]], 1, memory_order_relaxed);
                    }
                }
            }
        } else {
            while (i < a1 && k < kend) {
                uint32_t x = ft[i], y = ft[k];
                if (x == y) {
                    c++;
                    atomic_fetch_add_explicit(&j->t[x], 1, memory_order_relaxed);
                }
                i += x <= y;
                k += y <= x;
            }
        }
        if (c) {
            own += c;
            atomic_fetch_add_explicit(&j->t[v], c, memory_order_relaxed);
        }
    }
    if (own)
        atomic_fetch_add_explicit(&j->t[u], own, memory_order_relaxed);
}

static void dyn_gk_tri_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    dyn_gk_tri_t* j = (dyn_gk_tri_t*)arg;
    (void)nworkers;
    (void)par;
    for (;;) {
        uint32_t c = atomic_fetch_add_explicit(&j->next, 1, memory_order_relaxed), u, end;
        uint64_t lo = (uint64_t)c * DYN_GK_TRI_CHUNK;
        if (lo >= j->n || atomic_load_explicit(&j->stop, memory_order_relaxed))
            return;
        if (worker == 0 && dyn_gk_stopped(j->ctl)) {
            atomic_store_explicit(&j->stop, 1, memory_order_relaxed);
            return;
        }
        end = (uint32_t)lo + DYN_GK_TRI_CHUNK;
        if (end > j->n)
            end = j->n;
        for (u = (uint32_t)lo; u < end; u++)
            dyn_gk_tri_vertex(j, u);
    }
}

static int dyn_gk_rank_less(const uint32_t* off, uint32_t a, uint32_t b)
{
    uint32_t da = off[a + 1] - off[a], db = off[b + 1] - off[b];
    return da < db || (da == db && a < b);
}

int dyn_gk_triangles(const dyn_gk_csr_t* s, double* per, double* total, const dyn_gk_ctl_t* ctl)
{
    dyn_gk_tri_t j;
    uint32_t *fo, *ft, u, e, at = 0, n = s->n;
    _Atomic uint64_t* t;
    uint64_t sum = 0;
    unsigned nw;
    *total = 0.0;
    if (n == 0)
        return DYN_GK_OK;
    fo = (uint32_t*)malloc(((size_t)n + 1) * sizeof(uint32_t));
    ft = (uint32_t*)malloc((s->m / 2 + 1) * sizeof(uint32_t));
    t = (_Atomic uint64_t*)malloc((size_t)n * sizeof(*t));
    if (!fo || !ft || !t) {
        free(fo);
        free(ft);
        free(t);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < n; u++) {
        fo[u] = at;
        atomic_init(&t[u], 0);
        for (e = s->off[u]; e < s->off[u + 1]; e++)
            if (dyn_gk_rank_less(s->off, u, s->tgt[e]))
                ft[at++] = s->tgt[e];
    }
    fo[n] = at;
    j.fo = fo;
    j.ft = ft;
    j.n = n;
    j.t = t;
    j.ctl = ctl;
    atomic_init(&j.next, 0);
    atomic_init(&j.stop, 0);
    nw = dyn_par_plan((size_t)s->m + n, DYN_GK_TRI_UNITS_PER_WORKER, 0);
    dyn_par_run(nw, dyn_gk_tri_worker, &j);
    for (u = 0; u < n; u++) {
        uint64_t c = atomic_load_explicit(&t[u], memory_order_relaxed);
        per[u] = (double)c;
        sum += c;
    }
    free(fo);
    free(ft);
    free(t);
    if (atomic_load_explicit(&j.stop, memory_order_relaxed))
        return DYN_GK_E_CANCELLED;
    *total = (double)(sum / 3);
    return DYN_GK_OK;
}

void dyn_gk_clustering(const dyn_gk_csr_t* s, const double* tri, double* local, double* avg, double* trans)
{
    uint32_t u;
    double sum = 0.0, num = 0.0, den = 0.0;
    for (u = 0; u < s->n; u++) {
        double d = (double)(s->off[u + 1] - s->off[u]);
        double c = d > 1.0 ? 2.0 * tri[u] / (d * (d - 1.0)) : 0.0;
        local[u] = c;
        sum += c;
        num += 2.0 * tri[u];
        den += d * (d - 1.0);
    }
    *avg = s->n ? sum / (double)s->n : 0.0;
    *trans = num > 0.0 ? num / den : 0.0;
}

int dyn_gk_core_numbers(const dyn_gk_csr_t* s, int32_t* core)
{
    uint32_t n = s->n, *bin, *pos, *vert, u, i, maxd = 0;
    if (n == 0)
        return DYN_GK_OK;
    for (u = 0; u < n; u++) {
        uint32_t d = s->off[u + 1] - s->off[u];
        core[u] = (int32_t)d;
        if (d > maxd)
            maxd = d;
    }
    bin = (uint32_t*)calloc((size_t)maxd + 2, sizeof(uint32_t));
    pos = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    vert = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!bin || !pos || !vert) {
        free(bin);
        free(pos);
        free(vert);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < n; u++)
        bin[core[u] + 1]++;
    for (i = 0; i <= maxd; i++)
        bin[i + 1] += bin[i];
    for (u = 0; u < n; u++) {
        pos[u] = bin[core[u]]++;
        vert[pos[u]] = u;
    }
    for (i = maxd + 1; i > 0; i--)
        bin[i] = bin[i - 1];
    bin[0] = 0;
    for (i = 0; i < n; i++) {
        uint32_t v = vert[i], e;
        for (e = s->off[v]; e < s->off[v + 1]; e++) {
            uint32_t w = s->tgt[e];
            if (core[w] > core[v]) {
                uint32_t dw = (uint32_t)core[w], pw = pos[w], ps = bin[dw], x = vert[ps];
                if (x != w) {
                    vert[pw] = x;
                    pos[x] = pw;
                    vert[ps] = w;
                    pos[w] = ps;
                }
                bin[dw]++;
                core[w]--;
            }
        }
    }
    free(bin);
    free(pos);
    free(vert);
    return DYN_GK_OK;
}

static uint32_t dyn_gk_common(const dyn_gk_csr_t* s, uint32_t u, uint32_t v, int method, double* acc)
{
    const uint32_t* t = s->tgt;
    uint32_t i = s->off[u], ie = s->off[u + 1], k = s->off[v], ke = s->off[v + 1], c = 0;
    double a = 0.0;
    while (i < ie && k < ke) {
        uint32_t x = t[i], y = t[k];
        if (x == y && x != u && x != v) {
            double d = (double)(s->off[x + 1] - s->off[x]);
            c++;
            if (method == DYN_GK_LINK_ADAMIC_ADAR)
                a += 1.0 / log(d);
            else if (method == DYN_GK_LINK_RESOURCE)
                a += 1.0 / d;
        }
        i += x <= y;
        k += y <= x;
    }
    *acc = a;
    return c;
}

static double dyn_gk_link_one(const dyn_gk_csr_t* s, uint32_t u, uint32_t v, int method)
{
    double du = (double)(s->off[u + 1] - s->off[u]), dv = (double)(s->off[v + 1] - s->off[v]), acc;
    uint32_t c;
    if (method == DYN_GK_LINK_PREFERENTIAL)
        return du * dv;
    c = dyn_gk_common(s, u, v, method, &acc);
    if (method == DYN_GK_LINK_COMMON)
        return (double)c;
    if (method == DYN_GK_LINK_JACCARD) {
        uint32_t i = s->off[u], ie = s->off[u + 1], k = s->off[v], ke = s->off[v + 1], uni = 0;
        while (i < ie || k < ke) {
            if (k >= ke || (i < ie && s->tgt[i] < s->tgt[k]))
                i++;
            else if (i >= ie || s->tgt[k] < s->tgt[i])
                k++;
            else {
                i++;
                k++;
            }
            uni++;
        }
        return uni ? (double)c / (double)uni : 0.0;
    }
    return acc;
}

typedef struct {
    const dyn_gk_csr_t* s;
    const uint32_t* pairs;
    uint32_t np;
    int method;
    double* out;
} dyn_gk_link_t;

static void dyn_gk_link_worker(void* arg, unsigned worker, unsigned nworkers, dyn_par_t* par)
{
    dyn_gk_link_t* j = (dyn_gk_link_t*)arg;
    uint32_t lo = (uint32_t)((uint64_t)j->np * worker / nworkers);
    uint32_t hi = (uint32_t)((uint64_t)j->np * (worker + 1) / nworkers), i;
    (void)par;
    for (i = lo; i < hi; i++)
        j->out[i] = dyn_gk_link_one(j->s, j->pairs[2 * (size_t)i], j->pairs[2 * (size_t)i + 1], j->method);
}

int dyn_gk_link_scores(const dyn_gk_csr_t* s, const uint32_t* pairs, uint32_t np, int method, double* out)
{
    dyn_gk_link_t j;
    uint32_t i;
    for (i = 0; i < 2 * np; i++)
        if (pairs[i] >= s->n)
            return DYN_GK_E_BAD_INPUT;
    j.s = s;
    j.pairs = pairs;
    j.np = np;
    j.method = method;
    j.out = out;
    dyn_par_run(np < DYN_GK_LINK_PAR_MIN ? 1u : dyn_par_plan(np, DYN_GK_LINK_PAR_MIN, 0), dyn_gk_link_worker, &j);
    return DYN_GK_OK;
}

int dyn_gk_bipartition(const dyn_gk_csr_t* s, uint8_t* left)
{
    uint32_t n = s->n, *q, r, head, tail, e;
    uint8_t* seen;
    if (n == 0)
        return DYN_GK_OK;
    q = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    seen = (uint8_t*)calloc(n, 1);
    if (!q || !seen) {
        free(q);
        free(seen);
        return DYN_GK_E_OOM;
    }
    for (r = 0; r < n; r++) {
        if (seen[r])
            continue;
        seen[r] = 1;
        left[r] = 1;
        head = tail = 0;
        q[tail++] = r;
        while (head < tail) {
            uint32_t u = q[head++];
            for (e = s->off[u]; e < s->off[u + 1]; e++) {
                uint32_t v = s->tgt[e];
                if (!seen[v]) {
                    seen[v] = 1;
                    left[v] = (uint8_t)!left[u];
                    q[tail++] = v;
                } else if (left[v] == left[u]) {
                    free(q);
                    free(seen);
                    return DYN_GK_E_NOT_BIPARTITE;
                }
            }
        }
    }
    free(q);
    free(seen);
    return DYN_GK_OK;
}

#define DYN_GK_HK_INF UINT32_MAX

int dyn_gk_matching(const dyn_gk_csr_t* s, const uint8_t* left, int32_t* mate, uint32_t* size)
{
    uint32_t n = s->n, *dist, *q, *it, *stk, *via, u, e, matched = 0;
    *size = 0;
    for (u = 0; u < n; u++) {
        mate[u] = -1;
        for (e = s->off[u]; e < s->off[u + 1]; e++)
            if ((left[u] != 0) == (left[s->tgt[e]] != 0))
                return DYN_GK_E_NOT_BIPARTITE;
    }
    if (n == 0)
        return DYN_GK_OK;
    dist = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    q = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    it = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    stk = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    via = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!dist || !q || !it || !stk || !via) {
        free(dist);
        free(q);
        free(it);
        free(stk);
        free(via);
        return DYN_GK_E_OOM;
    }
    for (;;) {
        uint32_t head = 0, tail = 0, found = 0, before = matched;
        for (u = 0; u < n; u++) {
            dist[u] = DYN_GK_HK_INF;
            if (left[u] && mate[u] < 0) {
                dist[u] = 0;
                q[tail++] = u;
            }
        }
        while (head < tail) {
            uint32_t x = q[head++];
            for (e = s->off[x]; e < s->off[x + 1]; e++) {
                int32_t w = mate[s->tgt[e]];
                if (w < 0)
                    found = 1;
                else if (dist[w] == DYN_GK_HK_INF) {
                    dist[w] = dist[x] + 1;
                    q[tail++] = (uint32_t)w;
                }
            }
        }
        if (!found)
            break;
        for (u = 0; u < n; u++)
            it[u] = s->off[u];
        for (u = 0; u < n; u++) {
            uint32_t top;
            if (!left[u] || mate[u] >= 0)
                continue;
            top = 0;
            stk[0] = u;
            while (top != UINT32_MAX) {
                uint32_t x = stk[top], end = s->off[x + 1];
                int pushed = 0;
                while (it[x] < end) {
                    uint32_t v = s->tgt[it[x]++];
                    int32_t w = mate[v];
                    if (w < 0) {
                        uint32_t l;
                        via[top] = v;
                        for (l = 0; l <= top; l++) {
                            mate[stk[l]] = (int32_t)via[l];
                            mate[via[l]] = (int32_t)stk[l];
                        }
                        matched++;
                        top = UINT32_MAX;
                        pushed = 1;
                        break;
                    }
                    if (dist[w] == dist[x] + 1) {
                        via[top] = v;
                        stk[++top] = (uint32_t)w;
                        pushed = 1;
                        break;
                    }
                }
                if (!pushed) {
                    dist[x] = DYN_GK_HK_INF;
                    top = top == 0 ? UINT32_MAX : top - 1;
                }
            }
        }
        if (matched == before)
            break;
    }
    for (u = 0, matched = 0; u < n; u++)
        if (left[u] && mate[u] >= 0)
            matched++;
    *size = matched;
    free(dist);
    free(q);
    free(it);
    free(stk);
    free(via);
    return DYN_GK_OK;
}
