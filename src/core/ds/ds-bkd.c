#include "ds-internal.h"
#include "ds-alloc.h"
#include "ds-bkd.h"
#include <math.h>


typedef struct {
    uint8_t* rec;
    uint32_t rs, kb, bpd, dims;
    uint64_t rng;
    uint8_t tmp[DYN_BKD_MAX_KEY + 4];
    uint8_t piv[DYN_BKD_MAX_BPD];
} dyn_bkd_bctx;

static void dyn_bkd_swap(dyn_bkd_bctx* b, uint32_t i, uint32_t j)
{
    if (i == j)
        return;
    memcpy(b->tmp, b->rec + (size_t)i * b->rs, b->rs);
    memcpy(b->rec + (size_t)i * b->rs, b->rec + (size_t)j * b->rs, b->rs);
    memcpy(b->rec + (size_t)j * b->rs, b->tmp, b->rs);
}

static const uint8_t* dyn_bkd_rk(const dyn_bkd_bctx* b, uint32_t i, uint32_t dim)
{
    return b->rec + (size_t)i * b->rs + (size_t)dim * b->bpd;
}

static void dyn_bkd_select(dyn_bkd_bctx* b, uint32_t lo, uint32_t hi, uint32_t k, uint32_t dim)
{
    while (hi - lo > 1) {
        uint32_t p = lo + (uint32_t)(ds_splitmix64(&b->rng) % (hi - lo));
        uint32_t lt = lo, i = lo, gt = hi;
        memcpy(b->piv, dyn_bkd_rk(b, p, dim), b->bpd);
        while (i < gt) {
            int c = memcmp(dyn_bkd_rk(b, i, dim), b->piv, b->bpd);
            if (c < 0)
                dyn_bkd_swap(b, lt++, i++);
            else if (c > 0)
                dyn_bkd_swap(b, i, --gt);
            else
                i++;
        }
        if (k < lt)
            hi = lt;
        else if (k >= gt)
            lo = gt;
        else
            return;
    }
}

static void dyn_bkd_span(const uint8_t* lo, const uint8_t* hi, uint32_t n, uint8_t* out)
{
    int borrow = 0;
    uint32_t i = n;
    while (i-- > 0) {
        int v = (int)hi[i] - (int)lo[i] - borrow;
        borrow = v < 0;
        out[i] = (uint8_t)(v + (borrow ? 256 : 0));
    }
}

static uint32_t dyn_bkd_pick_dim(const dyn_bkd_bctx* b, uint32_t lo, uint32_t hi)
{
    uint8_t mn[DYN_BKD_MAX_KEY], mx[DYN_BKD_MAX_KEY], best[DYN_BKD_MAX_BPD], cur[DYN_BKD_MAX_BPD];
    uint32_t d, i, bd = 0;
    if (b->dims == 1)
        return 0;
    memcpy(mn, b->rec + (size_t)lo * b->rs, b->kb);
    memcpy(mx, mn, b->kb);
    for (i = lo + 1; i < hi; i++)
        for (d = 0; d < b->dims; d++) {
            const uint8_t* k = dyn_bkd_rk(b, i, d);
            uint32_t off = d * b->bpd;
            if (memcmp(k, mn + off, b->bpd) < 0)
                memcpy(mn + off, k, b->bpd);
            else if (memcmp(k, mx + off, b->bpd) > 0)
                memcpy(mx + off, k, b->bpd);
        }
    for (d = 0; d < b->dims; d++) {
        dyn_bkd_span(mn + d * b->bpd, mx + d * b->bpd, b->bpd, cur);
        if (d == 0 || memcmp(cur, best, b->bpd) > 0) {
            memcpy(best, cur, b->bpd);
            bd = d;
        }
    }
    return bd;
}

static void dyn_bkd_build_node(dyn_bkd_t* t, dyn_bkd_bctx* b, uint32_t node, uint32_t la, uint32_t lz)
{
    uint32_t dim, lo, hi, mid, lm;
    if (node >= t->nleaves)
        return;
    lo = dyn_bkd_leaf_start(t, la);
    hi = dyn_bkd_leaf_start(t, lz);
    lm = la + (lz - la) / 2;
    mid = dyn_bkd_leaf_start(t, lm);
    dim = dyn_bkd_pick_dim(b, lo, hi);
    dyn_bkd_select(b, lo, hi, mid, dim);
    t->sdim[node] = (uint8_t)dim;
    memcpy(t->split + (size_t)node * t->bpd, dyn_bkd_rk(b, mid, dim), t->bpd);
    dyn_bkd_build_node(t, b, 2 * node, la, lm);
    dyn_bkd_build_node(t, b, 2 * node + 1, lm, lz);
}

uint32_t dyn_bkd_leaf_start(const dyn_bkd_t* t, uint32_t leaf)
{
    return (uint32_t)(((uint64_t)t->n * leaf) / t->nleaves);
}

void dyn_bkd_free(dyn_bkd_t* t)
{
    dyn_ds_free(t->keys);
    dyn_ds_free(t->docs);
    dyn_ds_free(t->split);
    dyn_ds_free(t->sdim);
    t->keys = t->split = t->sdim = NULL;
    t->docs = NULL;
    t->n = t->nleaves = 0;
}

static void dyn_bkd_bounds(dyn_bkd_t* t)
{
    uint32_t i, d, bpd = t->bpd;
    memcpy(t->minp, t->keys, t->kb);
    memcpy(t->maxp, t->keys, t->kb);
    for (i = 1; i < t->n; i++)
        for (d = 0; d < t->dims; d++) {
            const uint8_t* k = t->keys + (size_t)i * t->kb + (size_t)d * bpd;
            if (memcmp(k, t->minp + d * bpd, bpd) < 0)
                memcpy(t->minp + d * bpd, k, bpd);
            if (memcmp(k, t->maxp + d * bpd, bpd) > 0)
                memcpy(t->maxp + d * bpd, k, bpd);
        }
}

static uint32_t dyn_bkd_nleaves(uint32_t n, uint32_t leaf)
{
    uint32_t L = 1;
    if (!n)
        return 0;
    while ((uint64_t)L * leaf < n)
        L *= 2;
    return L;
}

static int dyn_bkd_check(const dyn_bkd_t* t, uint32_t node, uint8_t* cmin, uint8_t* cmax)
{
    uint8_t save[DYN_BKD_MAX_BPD];
    const uint8_t* s;
    uint32_t dim, off;
    if (node >= t->nleaves) {
        uint32_t i, d, a = dyn_bkd_leaf_start(t, node - t->nleaves), z = dyn_bkd_leaf_start(t, node - t->nleaves + 1);
        for (i = a; i < z; i++)
            for (d = 0; d < t->dims; d++) {
                const uint8_t* k = t->keys + (size_t)i * t->kb + (size_t)d * t->bpd;
                if (memcmp(k, cmin + d * t->bpd, t->bpd) < 0 || memcmp(k, cmax + d * t->bpd, t->bpd) > 0)
                    return -1;
            }
        return 0;
    }
    dim = t->sdim[node];
    if (dim >= t->dims)
        return -1;
    off = dim * t->bpd;
    s = t->split + (size_t)node * t->bpd;
    memcpy(save, cmax + off, t->bpd);
    memcpy(cmax + off, s, t->bpd);
    if (dyn_bkd_check(t, 2 * node, cmin, cmax) < 0)
        return -1;
    memcpy(cmax + off, save, t->bpd);
    memcpy(save, cmin + off, t->bpd);
    memcpy(cmin + off, s, t->bpd);
    if (dyn_bkd_check(t, 2 * node + 1, cmin, cmax) < 0)
        return -1;
    memcpy(cmin + off, save, t->bpd);
    return 0;
}

int dyn_bkd_adopt(dyn_bkd_t* t, uint32_t dims, uint32_t bpd, uint32_t leaf, uint32_t n, uint8_t* keys, uint32_t* docs, uint8_t* split, uint8_t* sdim)
{
    uint8_t cmin[DYN_BKD_MAX_KEY], cmax[DYN_BKD_MAX_KEY];
    memset(t, 0, sizeof(*t));
    t->keys = keys;
    t->docs = docs;
    t->split = split;
    t->sdim = sdim;
    if (dims < 1 || dims > DYN_BKD_MAX_DIMS || bpd < 1 || bpd > DYN_BKD_MAX_BPD || leaf < 2 || leaf > DYN_BKD_MAX_LEAF || n > DYN_DS_MAX_ITEMS) {
        dyn_bkd_free(t);
        return -2;
    }
    t->dims = dims;
    t->bpd = bpd;
    t->kb = dims * bpd;
    t->leaf = leaf;
    t->n = n;
    t->nleaves = dyn_bkd_nleaves(n, leaf);
    if (!n)
        return 0;
    dyn_bkd_bounds(t);
    memcpy(cmin, t->minp, t->kb);
    memcpy(cmax, t->maxp, t->kb);
    if (dyn_bkd_check(t, 1, cmin, cmax) < 0) {
        dyn_bkd_free(t);
        return -2;
    }
    return 0;
}

int dyn_bkd_build(dyn_bkd_t* t, uint32_t dims, uint32_t bpd, uint32_t leaf, const uint8_t* keys, const uint32_t* docs, uint32_t n)
{
    dyn_bkd_bctx b;
    uint32_t i, L;
    memset(t, 0, sizeof(*t));
    if (dims < 1 || dims > DYN_BKD_MAX_DIMS || bpd < 1 || bpd > DYN_BKD_MAX_BPD || leaf < 2 || leaf > DYN_BKD_MAX_LEAF || n > DYN_DS_MAX_ITEMS)
        return -2;
    t->dims = dims;
    t->bpd = bpd;
    t->kb = dims * bpd;
    t->leaf = leaf;
    t->n = n;
    if (!n)
        return 0;
    L = dyn_bkd_nleaves(n, leaf);
    t->nleaves = L;
    memset(&b, 0, sizeof(b));
    b.rs = t->kb + 4;
    b.kb = t->kb;
    b.bpd = bpd;
    b.dims = dims;
    b.rng = 0x9E3779B97F4A7C15ull;
    b.rec = (uint8_t*)dyn_ds_malloc((size_t)n * b.rs);
    t->keys = (uint8_t*)dyn_ds_malloc((size_t)n * t->kb);
    t->docs = (uint32_t*)dyn_ds_malloc((size_t)n * 4);
    t->split = (uint8_t*)dyn_ds_calloc((size_t)L, bpd);
    t->sdim = (uint8_t*)dyn_ds_calloc((size_t)L, 1);
    if (!b.rec || !t->keys || !t->docs || !t->split || !t->sdim) {
        dyn_ds_free(b.rec);
        dyn_bkd_free(t);
        return -1;
    }
    for (i = 0; i < n; i++) {
        memcpy(b.rec + (size_t)i * b.rs, keys + (size_t)i * t->kb, t->kb);
        memcpy(b.rec + (size_t)i * b.rs + t->kb, &docs[i], 4);
    }
    dyn_bkd_build_node(t, &b, 1, 0, L);
    for (i = 0; i < n; i++) {
        memcpy(t->keys + (size_t)i * t->kb, b.rec + (size_t)i * b.rs, t->kb);
        memcpy(&t->docs[i], b.rec + (size_t)i * b.rs + t->kb, 4);
    }
    dyn_ds_free(b.rec);
    dyn_bkd_bounds(t);
    return 0;
}

static int dyn_bkd_push(dyn_bkd_sink* s, const uint32_t* v, size_t n)
{
    if (s->n + n > s->cap) {
        size_t c = s->cap ? s->cap : 256;
        uint32_t* p;
        while (c < s->n + n)
            c *= 2;
        p = (uint32_t*)dyn_ds_realloc(s->v, c * 4);
        if (!p)
            return -1;
        s->v = p;
        s->cap = c;
    }
    memcpy(s->v + s->n, v, n * 4);
    s->n += n;
    return 0;
}

typedef struct {
    const dyn_bkd_t* t;
    dyn_bkd_rel_fn rel;
    dyn_bkd_hit_fn hit;
    const void* q;
    dyn_bkd_sink* out;
    uint8_t cmin[DYN_BKD_MAX_KEY];
    uint8_t cmax[DYN_BKD_MAX_KEY];
} dyn_bkd_qctx;

static int dyn_bkd_visit(dyn_bkd_qctx* c, uint32_t node)
{
    const dyn_bkd_t* t = c->t;
    int r = c->rel(c->q, c->cmin, c->cmax);
    uint8_t save[DYN_BKD_MAX_BPD];
    uint32_t dim, off, a, z;
    if (r == DYN_BKD_OUTSIDE)
        return 0;
    if (r == DYN_BKD_INSIDE) {
        a = node;
        z = node;
        while (a < t->nleaves) {
            a = 2 * a;
            z = 2 * z + 1;
        }
        a = dyn_bkd_leaf_start(t, a - t->nleaves);
        z = dyn_bkd_leaf_start(t, z - t->nleaves + 1);
        return dyn_bkd_push(c->out, t->docs + a, z - a);
    }
    if (node >= t->nleaves) {
        uint32_t i;
        a = dyn_bkd_leaf_start(t, node - t->nleaves);
        z = dyn_bkd_leaf_start(t, node - t->nleaves + 1);
        for (i = a; i < z; i++)
            if (c->hit(c->q, t->keys + (size_t)i * t->kb) && dyn_bkd_push(c->out, &t->docs[i], 1) < 0)
                return -1;
        return 0;
    }
    dim = t->sdim[node];
    off = dim * t->bpd;
    memcpy(save, c->cmax + off, t->bpd);
    memcpy(c->cmax + off, t->split + (size_t)node * t->bpd, t->bpd);
    if (dyn_bkd_visit(c, 2 * node) < 0)
        return -1;
    memcpy(c->cmax + off, save, t->bpd);
    memcpy(save, c->cmin + off, t->bpd);
    memcpy(c->cmin + off, t->split + (size_t)node * t->bpd, t->bpd);
    if (dyn_bkd_visit(c, 2 * node + 1) < 0)
        return -1;
    memcpy(c->cmin + off, save, t->bpd);
    return 0;
}

int dyn_bkd_query(const dyn_bkd_t* t, dyn_bkd_rel_fn rel, dyn_bkd_hit_fn hit, const void* q, dyn_bkd_sink* out)
{
    dyn_bkd_qctx c;
    if (!t->n)
        return 0;
    c.t = t;
    c.rel = rel;
    c.hit = hit;
    c.q = q;
    c.out = out;
    memcpy(c.cmin, t->minp, t->kb);
    memcpy(c.cmax, t->maxp, t->kb);
    return dyn_bkd_visit(&c, 1);
}

void dyn_bkd_sink_free(dyn_bkd_sink* s)
{
    dyn_ds_free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

int dyn_bkd_sink_to_roar(dyn_bkd_sink* s, dyn_roar_t* out)
{
    uint32_t *tmp, *src, *dst, *sw;
    size_t i, cnt[65537];
    int pass;
    if (!s->n)
        return 0;
    tmp = (uint32_t*)dyn_ds_malloc(s->n * 4);
    if (!tmp)
        return -1;
    src = s->v;
    dst = tmp;
    for (pass = 0; pass < 2; pass++) {
        int sh = pass * 16;
        memset(cnt, 0, sizeof(cnt));
        for (i = 0; i < s->n; i++)
            cnt[((src[i] >> sh) & 0xFFFFu) + 1]++;
        for (i = 1; i <= 65536; i++)
            cnt[i] += cnt[i - 1];
        for (i = 0; i < s->n; i++)
            dst[cnt[(src[i] >> sh) & 0xFFFFu]++] = src[i];
        sw = src;
        src = dst;
        dst = sw;
    }
    for (i = 0; i < s->n; i++)
        if ((i == 0 || src[i] != src[i - 1]) && dyn_roar_add(out, src[i]) < 0) {
            dyn_ds_free(tmp);
            return -1;
        }
    dyn_ds_free(tmp);
    return 0;
}

int dyn_bkd_range_rel(const void* qv, const uint8_t* cmin, const uint8_t* cmax)
{
    const dyn_bkd_range_q* q = (const dyn_bkd_range_q*)qv;
    uint32_t d, off;
    int cross = 0;
    for (d = 0; d < q->dims; d++) {
        off = d * q->bpd;
        if (memcmp(cmax + off, q->lo + off, q->bpd) < 0 || memcmp(cmin + off, q->hi + off, q->bpd) > 0)
            return DYN_BKD_OUTSIDE;
        if (memcmp(cmin + off, q->lo + off, q->bpd) < 0 || memcmp(cmax + off, q->hi + off, q->bpd) > 0)
            cross = 1;
    }
    return cross ? DYN_BKD_CROSSES : DYN_BKD_INSIDE;
}

int dyn_bkd_range_hit(const void* qv, const uint8_t* key)
{
    const dyn_bkd_range_q* q = (const dyn_bkd_range_q*)qv;
    uint32_t d, off;
    for (d = 0; d < q->dims; d++) {
        off = d * q->bpd;
        if (memcmp(key + off, q->lo + off, q->bpd) < 0 || memcmp(key + off, q->hi + off, q->bpd) > 0)
            return 0;
    }
    return 1;
}

void dyn_bkd_enc_f64(double x, uint8_t* out)
{
    uint64_t u;
    int i;
    if (x != x)
        u = 0x7FF8000000000000ull;
    else
        memcpy(&u, &x, 8);
    u = (u >> 63) ? ~u : (u ^ 0x8000000000000000ull);
    for (i = 7; i >= 0; i--) {
        out[i] = (uint8_t)u;
        u >>= 8;
    }
}

void dyn_bkd_enc_i64(int64_t x, uint8_t* out)
{
    uint64_t u = (uint64_t)x ^ 0x8000000000000000ull;
    int i;
    for (i = 7; i >= 0; i--) {
        out[i] = (uint8_t)u;
        u >>= 8;
    }
}

double dyn_bkd_dec_f64(const uint8_t* p)
{
    uint64_t u = 0;
    double x;
    int i;
    for (i = 0; i < 8; i++)
        u = (u << 8) | p[i];
    u = (u >> 63) ? (u ^ 0x8000000000000000ull) : ~u;
    memcpy(&x, &u, 8);
    return x;
}

int64_t dyn_bkd_dec_i64(const uint8_t* p)
{
    uint64_t u = 0;
    int i;
    for (i = 0; i < 8; i++)
        u = (u << 8) | p[i];
    return (int64_t)(u ^ 0x8000000000000000ull);
}

static int dyn_bkd_lon_rel(double a, double b, double mn, double mx)
{
    if (mn <= mx) {
        if (b < mn || a > mx)
            return DYN_BKD_OUTSIDE;
        return (a >= mn && b <= mx) ? DYN_BKD_INSIDE : DYN_BKD_CROSSES;
    }
    if (a > mx && b < mn)
        return DYN_BKD_OUTSIDE;
    return (a >= mn || b <= mx) ? DYN_BKD_INSIDE : DYN_BKD_CROSSES;
}

static int dyn_bkd_lon_in(double x, double mn, double mx)
{
    return mn <= mx ? (x >= mn && x <= mx) : (x >= mn || x <= mx);
}

int dyn_bkd_geo_box_rel(const void* qv, const uint8_t* cmin, const uint8_t* cmax)
{
    const dyn_bkd_geo_box_q* q = (const dyn_bkd_geo_box_q*)qv;
    double la = dyn_bkd_dec_f64(cmin), lb = dyn_bkd_dec_f64(cmax);
    int r;
    if (lb < q->min_lat || la > q->max_lat)
        return DYN_BKD_OUTSIDE;
    r = dyn_bkd_lon_rel(dyn_bkd_dec_f64(cmin + 8), dyn_bkd_dec_f64(cmax + 8), q->min_lon, q->max_lon);
    if (r == DYN_BKD_OUTSIDE)
        return r;
    if (la < q->min_lat || lb > q->max_lat)
        return DYN_BKD_CROSSES;
    return r;
}

int dyn_bkd_geo_box_hit(const void* qv, const uint8_t* key)
{
    const dyn_bkd_geo_box_q* q = (const dyn_bkd_geo_box_q*)qv;
    double lat = dyn_bkd_dec_f64(key), lon = dyn_bkd_dec_f64(key + 8);
    return lat >= q->min_lat && lat <= q->max_lat && dyn_bkd_lon_in(lon, q->min_lon, q->max_lon);
}

double dyn_bkd_haversine(double lat1, double lon1, double lat2, double lon2)
{
    double r = M_PI / 180.0;
    double p1 = lat1 * r, p2 = lat2 * r;
    double s1 = sin((p2 - p1) / 2.0), s2 = sin((lon2 - lon1) * r / 2.0);
    double h = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
    if (h > 1.0)
        h = 1.0;
    return 2.0 * DYN_BKD_EARTH_M * asin(sqrt(h));
}

void dyn_bkd_geo_dist_init(dyn_bkd_geo_dist_q* q, double lat, double lon, double meters)
{
    double r = meters / DYN_BKD_EARTH_M + 1e-9, deg = 180.0 / M_PI;
    double phi = lat / deg, lam = lon / deg, mnl = phi - r, mxl = phi + r, dl, a, b;
    memset(q, 0, sizeof(*q));
    q->lat = lat;
    q->lon = lon;
    q->meters = meters;
    if (mnl > -M_PI / 2 && mxl < M_PI / 2) {
        double s = sin(r) / cos(phi);
        dl = s >= 1.0 ? M_PI : asin(s);
        if (dl >= M_PI) {
            a = -M_PI;
            b = M_PI;
        } else {
            a = lam - dl;
            b = lam + dl;
            if (a < -M_PI)
                a += 2 * M_PI;
            if (b > M_PI)
                b -= 2 * M_PI;
        }
    } else {
        if (mnl < -M_PI / 2)
            mnl = -M_PI / 2;
        if (mxl > M_PI / 2)
            mxl = M_PI / 2;
        a = -M_PI;
        b = M_PI;
    }
    q->bmin_lat = mnl * deg;
    q->bmax_lat = mxl * deg;
    q->bmin_lon = a * deg;
    q->bmax_lon = b * deg;
    q->wrap = q->bmin_lon > q->bmax_lon;
}

static int dyn_bkd_lon_near(double a, double b, double c)
{
    double da = a - c, db = b - c;
    return (da >= -90 && db <= 90) || (da + 360 >= -90 && db + 360 <= 90) || (da - 360 >= -90 && db - 360 <= 90);
}

int dyn_bkd_geo_dist_rel(const void* qv, const uint8_t* cmin, const uint8_t* cmax)
{
    const dyn_bkd_geo_dist_q* q = (const dyn_bkd_geo_dist_q*)qv;
    double la = dyn_bkd_dec_f64(cmin), lb = dyn_bkd_dec_f64(cmax);
    double oa = dyn_bkd_dec_f64(cmin + 8), ob = dyn_bkd_dec_f64(cmax + 8);
    double lim = q->meters * (1 - 1e-9);
    if (lb < q->bmin_lat || la > q->bmax_lat || dyn_bkd_lon_rel(oa, ob, q->bmin_lon, q->bmax_lon) == DYN_BKD_OUTSIDE)
        return DYN_BKD_OUTSIDE;
    if (dyn_bkd_lon_near(oa, ob, q->lon)
        && dyn_bkd_haversine(q->lat, q->lon, la, oa) <= lim && dyn_bkd_haversine(q->lat, q->lon, la, ob) <= lim
        && dyn_bkd_haversine(q->lat, q->lon, lb, oa) <= lim && dyn_bkd_haversine(q->lat, q->lon, lb, ob) <= lim)
        return DYN_BKD_INSIDE;
    return DYN_BKD_CROSSES;
}

int dyn_bkd_geo_dist_hit(const void* qv, const uint8_t* key)
{
    const dyn_bkd_geo_dist_q* q = (const dyn_bkd_geo_dist_q*)qv;
    return dyn_bkd_haversine(q->lat, q->lon, dyn_bkd_dec_f64(key), dyn_bkd_dec_f64(key + 8)) <= q->meters;
}
