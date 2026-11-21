#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-tdigest.h"

#define DS_PI 3.14159265358979323846

int dyn_tdigest_init(dyn_tdigest_t* t, double delta)
{
    memset(t, 0, sizeof(*t));
    t->delta = delta;
    t->cap = (uint32_t)(delta * 6) + 64;
    t->c = (dyn_tdigest_cent*)DYN_DS_MALLOC(sizeof(dyn_tdigest_cent) * t->cap);
    t->min = HUGE_VAL;
    t->max = -HUGE_VAL;
    return t->c ? 0 : -1;
}

void dyn_tdigest_free(dyn_tdigest_t* t)
{
    DYN_DS_FREE(t->c);
    DYN_DS_FREE(t->view);
    DYN_DS_FREE(t->vbuf);
    t->c = NULL;
    t->view = NULL;
    t->vbuf = NULL;
}

static int dyn_tdigest_cent_cmp(const void* a, const void* b)
{
    const dyn_tdigest_cent* p = (const dyn_tdigest_cent*)a;
    const dyn_tdigest_cent* q = (const dyn_tdigest_cent*)b;
    if (p->m != q->m)
        return (p->m > q->m) - (p->m < q->m);
    return (p->w > q->w) - (p->w < q->w);
}

static double dyn_tdigest_k(double q, double d)
{
    return d / (2 * DS_PI) * asin(2 * q - 1);
}

static double dyn_tdigest_kinv(double k, double d)
{
    if (k <= -d / 4)
        return 0;
    if (k >= d / 4)
        return 1;
    return (sin(k * 2 * DS_PI / d) + 1) / 2;
}

static uint32_t dyn_tdigest_pass(dyn_tdigest_cent* c, uint32_t n, double delta, double total)
{
    uint32_t i, out = 0, run = 1;
    double so_far = 0, wlimit, sw = c[0].m * c[0].w, w = c[0].w, m0 = c[0].m;
    wlimit = dyn_tdigest_kinv(dyn_tdigest_k(0, delta) + 1, delta) * total;
    for (i = 1; i < n; i++) {
        double nw = c[i].w;
        if (so_far + w + nw <= wlimit) {
            sw += c[i].m * nw;
            w += nw;
            run++;
        } else {
            c[out].m = run == 1 ? m0 : sw / w;
            c[out].w = w;
            out++;
            so_far += w;
            wlimit = dyn_tdigest_kinv(dyn_tdigest_k(so_far / total, delta) + 1, delta) * total;
            m0 = c[i].m;
            sw = m0 * nw;
            w = nw;
            run = 1;
        }
    }
    c[out].m = run == 1 ? m0 : sw / w;
    c[out].w = w;
    return out + 1;
}

static uint32_t dyn_tdigest_pass2(dyn_tdigest_cent* out, const dyn_tdigest_cent* a, uint32_t na,
    const dyn_tdigest_cent* b, uint32_t nb, double delta, double total)
{
    uint32_t i = 0, j = 0, k, o = 0, run = 0, n = na + nb;
    double so_far = 0, wlimit, sw = 0, w = 0, m0 = 0;
    wlimit = dyn_tdigest_kinv(dyn_tdigest_k(0, delta) + 1, delta) * total;
    for (k = 0; k < n; k++) {
        const dyn_tdigest_cent* x;
        double nw;
        if (j < nb && (i >= na || b[j].m < a[i].m || (b[j].m == a[i].m && b[j].w < a[i].w)))
            x = &b[j++];
        else
            x = &a[i++];
        nw = x->w;
        if (run && so_far + w + nw <= wlimit) {
            sw += x->m * nw;
            w += nw;
            run++;
            continue;
        }
        if (run) {
            out[o].m = run == 1 ? m0 : sw / w;
            out[o].w = w;
            o++;
            so_far += w;
            wlimit = dyn_tdigest_kinv(dyn_tdigest_k(so_far / total, delta) + 1, delta) * total;
        }
        m0 = x->m;
        sw = m0 * nw;
        w = nw;
        run = 1;
    }
    if (run) {
        out[o].m = run == 1 ? m0 : sw / w;
        out[o].w = w;
        o++;
    }
    return o;
}

static int dyn_tdigest_sync(dyn_tdigest_t* t)
{
    uint32_t i = t->vbuf_n, j = t->nbuf - t->vbuf_n, k = t->nbuf;
    if (!t->view)
        t->view = (dyn_tdigest_cent*)DYN_DS_MALLOC(sizeof(dyn_tdigest_cent) * t->cap);
    if (!t->vbuf)
        t->vbuf = (dyn_tdigest_cent*)DYN_DS_MALLOC(sizeof(dyn_tdigest_cent) * t->cap);
    if (!t->view || !t->vbuf)
        return -1;
    memcpy(t->view, t->c + t->n + t->vbuf_n, sizeof(dyn_tdigest_cent) * j);
    qsort(t->view, j, sizeof(dyn_tdigest_cent), dyn_tdigest_cent_cmp);
    while (j > 0) {
        if (i > 0 && dyn_tdigest_cent_cmp(&t->vbuf[i - 1], &t->view[j - 1]) > 0)
            t->vbuf[--k] = t->vbuf[--i];
        else
            t->vbuf[--k] = t->view[--j];
    }
    t->vbuf_n = t->nbuf;
    return 0;
}

void dyn_tdigest_compress(dyn_tdigest_t* t)
{
    if (t->nbuf == 0)
        return;
    if (dyn_tdigest_sync(t) == 0) {
        dyn_tdigest_cent* old = t->c;
        uint32_t n = dyn_tdigest_pass2(t->view, t->c, t->n, t->vbuf, t->nbuf, t->delta, t->total);
        t->c = t->view;
        t->view = old;
        t->n = n;
    } else {
        qsort(t->c, t->n + t->nbuf, sizeof(dyn_tdigest_cent), dyn_tdigest_cent_cmp);
        t->n = dyn_tdigest_pass(t->c, t->n + t->nbuf, t->delta, t->total);
    }
    t->nbuf = 0;
    t->view_ok = 0;
    t->vbuf_n = 0;
}

const dyn_tdigest_cent* dyn_tdigest_view(dyn_tdigest_t* t, uint32_t* n, double* total)
{
    if (t->nbuf == 0) {
        *n = t->n;
        *total = t->total;
        return t->c;
    }
    if (!t->view_ok) {
        if (dyn_tdigest_sync(t) < 0) {
            dyn_tdigest_compress(t);
            *n = t->n;
            *total = t->total;
            return t->c;
        }
        t->view_total = t->total;
        t->view_n = dyn_tdigest_pass2(t->view, t->c, t->n, t->vbuf, t->nbuf, t->delta, t->total);
        t->view_ok = 1;
    }
    *n = t->view_n;
    *total = t->view_total;
    return t->view;
}

void dyn_tdigest_add(dyn_tdigest_t* t, double x, double w)
{
    if (t->n + t->nbuf >= t->cap)
        dyn_tdigest_compress(t);
    if (t->n + t->nbuf >= t->cap)
        return;
    t->c[t->n + t->nbuf].m = x;
    t->c[t->n + t->nbuf].w = w;
    t->nbuf++;
    t->view_ok = 0;
    t->total += w;
    t->sum += x * w;
    if (x < t->min)
        t->min = x;
    if (x > t->max)
        t->max = x;
}

static double dyn_tdigest_wavg(double x1, double w1, double x2, double w2)
{
    double lo = x1 < x2 ? x1 : x2, hi = x1 < x2 ? x2 : x1;
    double r;
    if (!(w1 + w2 > 0))
        return (x1 + x2) / 2;
    r = (x1 * w1 + x2 * w2) / (w1 + w2);
    return r < lo ? lo : r > hi ? hi
                                : r;
}

double dyn_tdigest_quantile(dyn_tdigest_t* t, double q)
{
    double W, index, so_far;
    uint32_t i, n;
    const dyn_tdigest_cent* c = dyn_tdigest_view(t, &n, &W);
    if (n == 0)
        return (double)NAN;
    if (n == 1)
        return c[0].m;
    index = q * W;
    if (index < 1)
        return t->min;
    if (c[0].w > 1 && index < c[0].w / 2)
        return t->min + (index - 1) / (c[0].w / 2 - 1) * (c[0].m - t->min);
    if (index > W - 1)
        return t->max;
    if (c[n - 1].w > 1 && W - index <= c[n - 1].w / 2)
        return t->max - (W - index - 1) / (c[n - 1].w / 2 - 1) * (t->max - c[n - 1].m);
    so_far = c[0].w / 2;
    for (i = 0; i + 1 < n; i++) {
        double dw = (c[i].w + c[i + 1].w) / 2;
        if (so_far + dw > index) {
            double lu = 0, ru = 0, z1, z2;
            if (c[i].w == 1) {
                if (index - so_far < 0.5)
                    return c[i].m;
                lu = 0.5;
            }
            if (c[i + 1].w == 1) {
                if (so_far + dw - index <= 0.5)
                    return c[i + 1].m;
                ru = 0.5;
            }
            z1 = index - so_far - lu;
            z2 = so_far + dw - index - ru;
            return dyn_tdigest_wavg(c[i].m, z2, c[i + 1].m, z1);
        }
        so_far += dw;
    }
    {
        double z1 = index - so_far, z2 = c[n - 1].w / 2 - z1;
        if (z1 < 0)
            z1 = 0;
        if (z2 < 0)
            z2 = 0;
        if (z1 + z2 == 0)
            return c[n - 1].m;
        return dyn_tdigest_wavg(c[n - 1].m, z2, t->max, z1);
    }
}

double dyn_tdigest_cdf(dyn_tdigest_t* t, double x)
{
    uint32_t i, n;
    double cum = 0, px, py, total;
    const dyn_tdigest_cent* c = dyn_tdigest_view(t, &n, &total);
    if (n == 0)
        return (double)NAN;
    if (x < t->min)
        return 0;
    if (x >= t->max)
        return 1;
    if (n == 1)
        return t->max - t->min > 0 ? (x - t->min) / (t->max - t->min) : 0.5;
    px = t->min;
    py = 0;
    for (i = 0; i < n; i++) {
        double kx = c[i].m, ky = cum + c[i].w / 2;
        if (x < kx) {
            if (kx <= px)
                return ky / total;
            return (py + (ky - py) * (x - px) / (kx - px)) / total;
        }
        px = kx;
        py = ky;
        cum += c[i].w;
    }
    if (t->max <= px)
        return 1;
    return (py + (total - py) * (x - px) / (t->max - px)) / total;
}
