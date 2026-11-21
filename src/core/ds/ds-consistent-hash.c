#include "ds-internal.h"
#include "ds-consistent-hash.h"
#include "ds-heap.h"

static _Thread_local const dyn_chash_t* ds_ch_sort_ctx;

static int dyn_chash_pt_cmp(const void* a, const void* b)
{
    const dyn_chash_pt *x = (const dyn_chash_pt*)a, *y = (const dyn_chash_pt*)b;
    const dyn_chash_node *nx, *ny;
    uint32_t m;
    int c;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    nx = &ds_ch_sort_ctx->nodes[x->node];
    ny = &ds_ch_sort_ctx->nodes[y->node];
    m = nx->len < ny->len ? nx->len : ny->len;
    c = m ? memcmp(nx->name, ny->name, m) : 0;
    return c ? c : (nx->len > ny->len) - (nx->len < ny->len);
}

static void dyn_chash_hash_node(const dyn_chash_t* c, uint32_t i, char* buf, dyn_chash_pt* out, uint32_t cnt)
{
    size_t len = c->nodes[i].len, nd = 1;
    uint32_t r;
    char* d = buf + len + 1;
    if (len)
        memcpy(buf, c->nodes[i].name, len);
    buf[len] = '#';
    d[0] = '0';
    for (r = 0; r < cnt; r++) {
        size_t k;
        out[r].h = dyn_ds_xxh64((const uint8_t*)buf, len + 1 + nd, 0);
        out[r].node = i;
        for (k = nd; k-- > 0;) {
            if (d[k] != '9') {
                d[k]++;
                break;
            }
            d[k] = '0';
        }
        if (k == (size_t)-1) {
            d[0] = '1';
            d[nd++] = '0';
        }
    }
}

static void dyn_chash_sort(const dyn_chash_t* c, dyn_chash_pt* pts, uint32_t n)
{
    dyn_chash_pt* tmp = NULL;
    uint32_t* cnt = NULL;
    uint32_t i, j;
    if (n >= 4096) {
        tmp = (dyn_chash_pt*)DYN_DS_MALLOC(sizeof(dyn_chash_pt) * (size_t)n);
        cnt = (uint32_t*)DYN_DS_CALLOC(6 * 2048, sizeof(uint32_t));
    }
    if (!tmp || !cnt) {
        DYN_DS_FREE(tmp);
        DYN_DS_FREE(cnt);
        ds_ch_sort_ctx = c;
        qsort(pts, n, sizeof(dyn_chash_pt), dyn_chash_pt_cmp);
        ds_ch_sort_ctx = NULL;
        return;
    }
    for (i = 0; i < n; i++)
        for (j = 0; j < 6; j++)
            cnt[j * 2048 + ((pts[i].h >> (11 * j)) & 2047)]++;
    for (j = 0; j < 6; j++) {
        uint32_t* b = cnt + j * 2048;
        uint32_t s = 0, t;
        dyn_chash_pt *src = (j & 1) ? tmp : pts, *dst = (j & 1) ? pts : tmp;
        for (i = 0; i < 2048; i++) {
            t = b[i];
            b[i] = s;
            s += t;
        }
        for (i = 0; i < n; i++)
            dst[b[(src[i].h >> (11 * j)) & 2047]++] = src[i];
    }
    DYN_DS_FREE(tmp);
    DYN_DS_FREE(cnt);
    ds_ch_sort_ctx = c;
    for (i = 0; i + 1 < n;) {
        j = i + 1;
        while (j < n && pts[j].h == pts[i].h)
            j++;
        if (j - i > 1)
            qsort(pts + i, j - i, sizeof(dyn_chash_pt), dyn_chash_pt_cmp);
        i = j;
    }
    ds_ch_sort_ctx = NULL;
}

static int dyn_chash_rebuild(dyn_chash_t* c)
{
    uint64_t total = 0;
    uint32_t i, r, k = 0;
    dyn_chash_pt* ring;
    for (i = 0; i < c->nn; i++)
        total += (uint64_t)c->replicas * c->nodes[i].weight;
    if (total > DYN_CHASH_MAX_POINTS)
        return -2;
    ring = (dyn_chash_pt*)DYN_DS_MALLOC(sizeof(dyn_chash_pt) * ((size_t)total + 1));
    if (!ring)
        return -1;
    for (i = 0; i < c->nn; i++) {
        size_t len = c->nodes[i].len;
        char* buf = (char*)DYN_DS_MALLOC(len + 16);
        if (!buf) {
            DYN_DS_FREE(ring);
            return -1;
        }
        r = c->replicas * c->nodes[i].weight;
        dyn_chash_hash_node(c, i, buf, ring + k, r);
        k += r;
        DYN_DS_FREE(buf);
    }
    dyn_chash_sort(c, ring, k);
    DYN_DS_FREE(c->ring);
    c->ring = ring;
    c->nr = k;
    c->jdirty = 1;
    return 0;
}

static dyn_chash_pt* dyn_chash_node_points(dyn_chash_t* c, uint32_t i, uint32_t* m)
{
    size_t len = c->nodes[i].len;
    uint32_t cnt = c->replicas * c->nodes[i].weight;
    dyn_chash_pt* pts = (dyn_chash_pt*)DYN_DS_MALLOC(sizeof(dyn_chash_pt) * ((size_t)cnt + 1));
    char* buf = (char*)DYN_DS_MALLOC(len + 16);
    if (!pts || !buf) {
        DYN_DS_FREE(pts);
        DYN_DS_FREE(buf);
        return NULL;
    }
    dyn_chash_hash_node(c, i, buf, pts, cnt);
    DYN_DS_FREE(buf);
    dyn_chash_sort(c, pts, cnt);
    *m = cnt;
    return pts;
}

static int dyn_chash_merge(dyn_chash_t* c, const dyn_chash_pt* add, uint32_t m, uint32_t drop, uint32_t from, uint32_t to)
{
    uint32_t a = 0, b = 0, k = 0;
    dyn_chash_pt* ring = (dyn_chash_pt*)DYN_DS_MALLOC(sizeof(dyn_chash_pt) * ((size_t)c->nr + m + 1));
    if (!ring)
        return -1;
    ds_ch_sort_ctx = c;
    while (a < c->nr || b < m) {
        dyn_chash_pt p;
        if (a < c->nr && c->ring[a].node == drop) {
            a++;
            continue;
        }
        if (a < c->nr) {
            p = c->ring[a];
            if (p.node == from)
                p.node = to;
            if (b < m && dyn_chash_pt_cmp(&add[b], &p) < 0)
                ring[k++] = add[b++];
            else {
                ring[k++] = p;
                a++;
            }
        } else
            ring[k++] = add[b++];
    }
    ds_ch_sort_ctx = NULL;
    DYN_DS_FREE(c->ring);
    c->ring = ring;
    c->nr = k;
    c->jdirty = 1;
    return 0;
}

static int dyn_chash_place(dyn_chash_t* c, uint32_t i, uint32_t drop)
{
    uint32_t m;
    int rc;
    dyn_chash_pt* pts = dyn_chash_node_points(c, i, &m);
    if (!pts)
        return -1;
    rc = dyn_chash_merge(c, pts, m, drop, UINT32_MAX, UINT32_MAX);
    DYN_DS_FREE(pts);
    return rc;
}

static uint64_t dyn_chash_total(const dyn_chash_t* c)
{
    uint64_t total = 0;
    uint32_t i;
    for (i = 0; i < c->nn; i++)
        total += (uint64_t)c->replicas * c->nodes[i].weight;
    return total;
}

uint32_t dyn_chash_locate(const dyn_chash_t* c, uint64_t h)
{
    uint32_t lo = 0, hi = c->nr;
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (c->ring[m].h < h)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == c->nr ? 0 : lo;
}

void dyn_chash_free(dyn_chash_t* c)
{
    uint32_t i;
    for (i = 0; i < c->nn; i++)
        DYN_DS_FREE(c->nodes[i].name);
    DYN_DS_FREE(c->nodes);
    DYN_DS_FREE(c->ring);
    DYN_DS_FREE(c->jt);
    dyn_ds_smap_free(&c->idx);
}

int dyn_chash_set_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight)
{
    uint64_t h = dyn_ds_strhash(name, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&c->idx, name, n, h);
    int rc;
    if (e) {
        uint32_t i = (uint32_t)e->v, old = c->nodes[i].weight;
        c->nodes[i].weight = weight;
        rc = dyn_chash_total(c) > DYN_CHASH_MAX_POINTS ? -2 : dyn_chash_place(c, i, i);
        if (rc < 0)
            c->nodes[i].weight = old;
        return rc;
    }
    rc = dyn_chash_append_node(c, name, n, weight);
    if (rc < 0)
        return rc;
    rc = dyn_chash_total(c) > DYN_CHASH_MAX_POINTS ? -2 : dyn_chash_place(c, c->nn - 1, UINT32_MAX);
    if (rc < 0) {
        c->nn--;
        e = dyn_ds_smap_find(&c->idx, name, n, h);
        if (e)
            dyn_ds_smap_remove(&c->idx, e);
        DYN_DS_FREE(c->nodes[c->nn].name);
    }
    return rc;
}

int dyn_chash_commit(dyn_chash_t* c)
{
    return dyn_chash_rebuild(c);
}

int dyn_chash_append_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight)
{
    uint64_t h = dyn_ds_strhash(name, n);
    if (n > UINT32_MAX - 1)
        return -1;
    if (c->nn == c->ncap) {
        uint32_t nc = c->ncap ? c->ncap * 2 : 8;
        dyn_chash_node* nn = (dyn_chash_node*)DYN_DS_REALLOC(c->nodes, sizeof(dyn_chash_node) * nc);
        if (!nn)
            return -1;
        c->nodes = nn;
        c->ncap = nc;
    }
    c->nodes[c->nn].name = (char*)dyn_ds_bytes_dup(name, n);
    if (!c->nodes[c->nn].name)
        return -1;
    c->nodes[c->nn].len = (uint32_t)n;
    c->nodes[c->nn].weight = weight;
    if (!dyn_ds_smap_insert(&c->idx, c->nodes[c->nn].name, n, h, c->nn)) {
        DYN_DS_FREE(c->nodes[c->nn].name);
        return -1;
    }
    c->nn++;
    return 0;
}

int dyn_chash_remove_node(dyn_chash_t* c, const char* name, size_t n)
{
    uint64_t h = dyn_ds_strhash(name, n);
    dyn_ds_sent* e = dyn_ds_smap_find(&c->idx, name, n, h);
    uint32_t i, last;
    if (!e)
        return 0;
    i = (uint32_t)e->v;
    dyn_ds_smap_remove(&c->idx, e);
    last = c->nn - 1;
    DYN_DS_FREE(c->nodes[i].name);
    if (i != last) {
        c->nodes[i] = c->nodes[last];
        e = dyn_ds_smap_find(&c->idx, c->nodes[i].name, c->nodes[i].len, dyn_ds_strhash(c->nodes[i].name, c->nodes[i].len));
        if (e)
            e->v = i;
    }
    c->nn--;
    if (dyn_chash_merge(c, NULL, 0, i, last, i) < 0) {
        DYN_DS_FREE(c->ring);
        c->ring = NULL;
        c->nr = 0;
        c->jdirty = 1;
        return -1;
    }
    return 1;
}

void dyn_chash_init(dyn_chash_t* c, uint32_t replicas)
{
    memset(c, 0, sizeof(*c));
    c->replicas = replicas;
}

static int dyn_chash_jump_build(dyn_chash_t* c)
{
    uint32_t b = 0, idx, j = 0, nb;
    uint32_t* jt;
    while (b < 24 && ((uint64_t)8 << (b + 1)) <= c->nr)
        b++;
    nb = 1u << b;
    jt = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)nb + 1));
    if (!jt)
        return -1;
    for (idx = 0; idx < nb; idx++) {
        while (j < c->nr && (c->ring[j].h >> (64 - b)) < idx)
            j++;
        jt[idx] = j;
    }
    jt[nb] = c->nr;
    DYN_DS_FREE(c->jt);
    c->jt = jt;
    c->jbits = b;
    c->jdirty = 0;
    return 0;
}

uint32_t dyn_chash_find(dyn_chash_t* c, uint64_t h)
{
    uint32_t lo, hi, idx;
    if (!c->jon || c->nr < 64 || (c->jdirty && dyn_chash_jump_build(c) < 0))
        return dyn_chash_locate(c, h);
    idx = (uint32_t)(h >> (64 - c->jbits));
    lo = c->jt[idx];
    hi = c->jt[idx + 1];
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (c->ring[m].h < h)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == c->nr ? 0 : lo;
}
