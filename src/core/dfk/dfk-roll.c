#include "dfk.h"
#include "../dyn-cdefs.h"

#define DFK_ROLL_UNROLL 8u

static inline double dfk_pick(uint32_t sel, double v)
{
    uint64_t b, m;

    memcpy(&b, &v, sizeof(b));
    m = (uint64_t)0 - (uint64_t)(sel != 0);
    b = (b & m) | (0ULL & ~m);
    memcpy(&v, &b, sizeof(v));
    return v;
}

int dfk_roll_extreme(const void* bp, DFType bt, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_min)
{
    uint32_t* dq;
    double *wv, *own;
    uint32_t head = 0, tail = 0, i;

    dq = malloc((size_t)span * sizeof(*dq) + 1);
    if (!dq)
        return -1;
    if (bt == DF_F64) {
        wv = DYN_UNCONST(bp);
        own = NULL;
    } else {
        wv = own = malloc((size_t)span * sizeof(*wv) + 1);
        if (!wv) {
            free(dq);
            return -1;
        }
        dfk_widen(bp, bt, wv, span);
    }

    for (i = 0; i < span; i++) {
        double v;
        while (head < tail && dq[head] + w <= i)
            head++;
        if (!(mask && !mask[i])) {
            v = wv[i];
            if (!(v != v)) {
                while (head < tail && (want_min ? (wv[dq[tail - 1]] >= v) : (wv[dq[tail - 1]] <= v)))
                    tail--;
                dq[tail++] = i;
            }
        }
        dst[i] = i < w - 1 ? DYN_NAN
            : head < tail  ? wv[dq[head]]
                           : DYN_NAN;
    }
    free(own);
    free(dq);
    return 0;
}

static int dfk_roll_sum(const void* bp, DFType bt, double* dst, uint32_t span,
    uint32_t w, int want_mean)
{
    const double* x;
    double* own = NULL;
    uint32_t i;

    if (w < DFK_ROLL_UNROLL || span < w)
        return -1;
    if (bt == DF_F64) {
        x = bp;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        dfk_widen(bp, bt, own, span);
    }
    if (w >= DFK_ROLL_SLIDE_MIN) {
        double* suf = malloc((size_t)w * sizeof(double));
        if (suf) {
            uint32_t s;
            for (i = 0; i + 1 < w; i++)
                dst[i] = DYN_NAN;
            for (s = 0; s < span; s += w) {
                uint32_t end = (s + w < span) ? s + w : span;
                uint32_t k, lo, hi;
                double run = 0, pre = 0;
                for (k = end; k-- > s;) {
                    run += x[k];
                    suf[k - s] = run;
                }
                lo = s + w - 1;
                hi = s + 2 * w - 2;
                if (hi >= span)
                    hi = span - 1;
                for (k = lo; k <= hi; k++) {
                    uint32_t st = k + 1 - w;
                    double v;
                    if (k >= end)
                        pre += x[k];
                    v = (st < end ? suf[st - s] : 0.0) + pre;
                    dst[k] = want_mean ? v / (double)w : v;
                }
            }
            free(suf);
            free(own);
            return 0;
        }
    }
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;
    for (i = w - 1; i < span; i++) {
        const double* p = x + (i - (w - 1));
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        uint32_t j, m = w & ~7u;
        for (j = 0; j < m; j += 8) {
            a0 += p[j];
            a1 += p[j + 1];
            a2 += p[j + 2];
            a3 += p[j + 3];
            a4 += p[j + 4];
            a5 += p[j + 5];
            a6 += p[j + 6];
            a7 += p[j + 7];
        }
        for (; j < w; j++)
            a0 += p[j];
        a0 = ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
        dst[i] = want_mean ? a0 / (double)w : a0;
    }
    free(own);
    return 0;
}

static int dfk_roll_sum_masked(const void* bp, DFType bt, const uint8_t* mask,
    double* dst, uint32_t span, uint32_t w,
    int want_mean)
{
    const double* x;
    double *own = NULL, *suf;
    uint32_t i, s, sel = 0;

    if (w < DFK_ROLL_SLIDE_MIN || span < w)
        return -1;
    if (bt == DF_F64) {
        x = bp;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        dfk_widen(bp, bt, own, span);
    }
    suf = malloc((size_t)w * sizeof(double));
    if (!suf) {
        free(own);
        return -1;
    }
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;

    for (s = 0; s < span; s += w) {
        uint32_t end = (s + w < span) ? s + w : span;
        uint32_t k, lo, hi, t;
        double run = 0.0, pre = 0.0;
        for (k = end; k-- > s;) {
            run += dfk_pick((uint32_t)(mask[k] != 0), x[k]);
            suf[k - s] = run;
        }
        lo = s + w - 1;
        hi = s + 2 * w - 2;
        if (hi >= span)
            hi = span - 1;
        for (k = lo; k <= hi; k++) {
            uint32_t st = k + 1 - w;
            double v;
            if (k >= w) {
                sel += (uint32_t)(mask[k] != 0) - (uint32_t)(mask[k - w] != 0);
            } else {
                for (t = 0; t <= k; t++)
                    sel += (uint32_t)(mask[t] != 0);
            }
            if (k >= end)
                pre += dfk_pick((uint32_t)(mask[k] != 0), x[k]);
            v = (st < end ? suf[st - s] : 0.0) + pre;
            dst[k] = want_mean ? (sel ? v / (double)sel : DYN_NAN) : v;
        }
    }
    free(suf);
    free(own);
    return 0;
}

int dfk_roll_sum_sel(const void* bp, DFType bt, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_mean)
{
    if (!mask)
        return dfk_roll_sum(bp, bt, dst, span, w, want_mean);
    return dfk_roll_sum_masked(bp, bt, mask, dst, span, w, want_mean);
}
