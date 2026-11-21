#include <math.h>
#include "veck-int.h"

uint32_t* vk_links(const dyn_vec_t* v, uint32_t slot, int level)
{
    uint32_t* b = v->links[slot];
    if (level == 0)
        return b;
    return b + (1 + v->M0) + (size_t)(level - 1) * (1 + v->M);
}

uint32_t vk_next_tag(dyn_vec_t* v)
{
    if (++v->vtag == 0) {
        memset(v->vis, 0, (size_t)v->cap * sizeof(uint32_t));
        v->vtag = 1;
    }
    return v->vtag;
}

static int vk_level(dyn_vec_t* v)
{
    uint64_t r;
    double u, l;
    v->rng += 0x9E3779B97F4A7C15ull;
    r = v->rng;
    r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ull;
    r = (r ^ (r >> 27)) * 0x94D049BB133111EBull;
    r ^= r >> 31;
    u = ((double)(r >> 11) + 1.0) * 0x1p-53;
    l = -log(u) * v->ml;
    return l >= DYN_VEC_MAX_LEVEL ? DYN_VEC_MAX_LEVEL : (int)l;
}


static uint32_t vk_greedy(const dyn_vec_t* v, dyn_vec_hit_t* cur, int level)
{
    int changed = 1;
    while (changed) {
        uint32_t* L = vk_links(v, cur->s, level);
        uint32_t i;
        changed = 0;
        for (i = 1; i <= L[0]; i++) {
            dyn_vec_hit_t h;
            if (i < L[0])
                vk_prefetch(v, L[i + 1]);
            h.d = vk_qd(v, L[i]);
            h.s = L[i];
            if (vk_lt(h, *cur)) {
                *cur = h;
                changed = 1;
            }
        }
    }
    return cur->s;
}

static uint32_t vk_layer(dyn_vec_t* v, dyn_vec_hit_t ep, uint32_t ef,
    int level, const dyn_vec_filter_t* f, int build)
{
    uint32_t tag = vk_next_tag(v), nc = 0, nr = 0, i;
    dyn_vec_hit_t *C = v->cand, *R = v->res;
    v->vis[ep.s] = tag;
    vk_minpush(C, &nc, ep);
    if (build || vk_admit(v, f, (uint32_t)ep.s))
        vk_maxpush(R, &nr, ep);
    while (nc) {
        dyn_vec_hit_t c = vk_minpop(C, &nc);
        uint32_t* L;
        if (nr >= ef && vk_lt(R[0], c))
            break;
        L = vk_links(v, c.s, level);
        for (i = 1; i <= L[0]; i++) {
            uint32_t u = L[i];
            dyn_vec_hit_t h;
            if (i < L[0] && v->vis[L[i + 1]] != tag)
                vk_prefetch(v, L[i + 1]);
            if (v->vis[u] == tag)
                continue;
            v->vis[u] = tag;
            h.d = vk_qd(v, u);
            h.s = u;
            if (nr < ef || vk_lt(h, R[0])) {
                vk_minpush(C, &nc, h);
                if (build || vk_admit(v, f, u)) {
                    vk_maxpush(R, &nr, h);
                    if (nr > ef)
                        vk_maxpop(R, &nr);
                }
            }
        }
    }
    return nr;
}

static uint32_t vk_select(const dyn_vec_t* v, uint32_t base, const dyn_vec_hit_t* W, uint32_t nw, uint32_t m,
    uint32_t* out)
{
    uint32_t i, j, no = 0;
    int ip = v->metric == DYN_VEC_IP;
    if (nw < m || !v->heuristic) {
        for (i = 0; i < nw && i < m; i++)
            out[i] = W[i].s;
        return i;
    }
    for (i = 0; i < nw && no < m; i++) {
        uint32_t c = (uint32_t)W[i].s;
        double dq = ip ? vk_pl2(v, c, base) : W[i].d;
        int good = 1;
        for (j = 0; j < no; j++)
            if ((ip ? vk_pl2(v, c, out[j]) : vk_pd(v, c, out[j])) < dq) {
                good = 0;
                break;
            }
        if (good)
            out[no++] = c;
    }
    return no;
}

static void vk_sort_hits(dyn_vec_hit_t* a, uint32_t n)
{
    uint32_t i, j;
    for (i = 1; i < n; i++) {
        dyn_vec_hit_t x = a[i];
        for (j = i; j > 0 && vk_lt(x, a[j - 1]); j--)
            a[j] = a[j - 1];
        a[j] = x;
    }
}

static void vk_connect(dyn_vec_t* v, uint32_t e, uint32_t s, int level)
{
    uint32_t* L = vk_links(v, e, level);
    uint32_t mmax = level ? v->M : v->M0, i, cnt;
    uint32_t sel[2 * DYN_VEC_MAX_M];
    dyn_vec_hit_t* B = v->cand;
    if (L[0] < mmax) {
        L[++L[0]] = s;
        return;
    }
    for (i = 0; i < L[0]; i++) {
        B[i].s = L[i + 1];
        B[i].d = vk_pd(v, e, L[i + 1]);
    }
    B[i].s = s;
    B[i].d = vk_pd(v, e, s);
    vk_sort_hits(B, L[0] + 1);
    cnt = vk_select(v, e, B, L[0] + 1, mmax, sel);
    L[0] = cnt;
    for (i = 0; i < cnt; i++)
        L[i + 1] = sel[i];
}

int vk_hnsw_insert(dyn_vec_t* v, uint32_t slot)
{
    int level = vk_level(v), l;
    size_t words = (size_t)(1 + v->M0) + (size_t)level * (1 + v->M);
    uint32_t* blk;
    uint32_t need = slot + 1 > v->M0 + 2 ? slot + 1 : v->M0 + 2;
    uint32_t sel[DYN_VEC_MAX_M];
    dyn_vec_hit_t cur;
    if (vk_grow(&v->cand, &v->ccap, need) || vk_grow(&v->res, &v->rcap, v->efc + 2)
        || vk_grow(&v->tmp, &v->tcap, v->efc + 2))
        return DYN_VEC_ENOMEM;
    blk = (uint32_t*)calloc(words, sizeof(uint32_t));
    if (!blk)
        return DYN_VEC_ENOMEM;
    v->links[slot] = blk;
    v->lvl[slot] = (uint8_t)level;
    if (v->top < 0) {
        v->entry = slot;
        v->top = level;
        return DYN_VEC_OK;
    }
    vk_query_slot(v, slot);
    cur.s = v->entry;
    cur.d = vk_qd(v, (uint32_t)cur.s);
    for (l = v->top; l > level; l--)
        vk_greedy(v, &cur, l);
    for (l = level < v->top ? level : v->top; l >= 0; l--) {
        uint32_t nr = vk_layer(v, cur, v->efc, l, NULL, 1), m = nr, i, cnt;
        uint32_t* L = vk_links(v, slot, l);
        while (nr) {
            uint32_t at = nr - 1;
            v->tmp[at] = vk_maxpop(v->res, &nr);
        }
        cur = v->tmp[0];
        cnt = vk_select(v, slot, v->tmp, m, v->M, sel);
        L[0] = cnt;
        for (i = 0; i < cnt; i++)
            L[i + 1] = sel[i];
        for (i = 0; i < cnt; i++)
            vk_connect(v, sel[i], slot, l);
    }
    if (level > v->top) {
        v->top = level;
        v->entry = slot;
    }
    return DYN_VEC_OK;
}

int vk_hnsw_search(dyn_vec_t* v, uint32_t ef, const dyn_vec_filter_t* f, uint32_t* nres)
{
    dyn_vec_hit_t cur;
    int l;
    if (vk_grow(&v->cand, &v->ccap, v->n + 1) || vk_grow(&v->res, &v->rcap, ef + 2))
        return DYN_VEC_ENOMEM;
    cur.s = v->entry;
    cur.d = vk_qd(v, (uint32_t)cur.s);
    for (l = v->top; l > 0; l--)
        vk_greedy(v, &cur, l);
    *nres = vk_layer(v, cur, ef, 0, f, 0);
    return DYN_VEC_OK;
}
