#include "dfk.h"
#include "../dyn-cdefs.h"

#define DFK_UNROLL_MIN 64u

#define DFK_WIDEN_ONE(CTYPE)       \
    do {                           \
        const CTYPE* x = p;        \
        for (i = 0; i < n; i++)    \
            dst[i] = (double)x[i]; \
    } while (0)

void dfk_widen(const void* p, DFType t, double* dst, uint32_t n)
{
    uint32_t i;

    if (n == 0)
        return;
    switch ((int)(t)) {
    case DF_F64:
        memcpy(dst, p, (size_t)n * sizeof(double));
        return;
    case DF_F32:
        DFK_WIDEN_ONE(float);
        break;
    case DF_I32:
        DFK_WIDEN_ONE(int32_t);
        break;
    case DF_U32:
        DFK_WIDEN_ONE(uint32_t);
        break;
    case DF_I16:
        DFK_WIDEN_ONE(int16_t);
        break;
    case DF_U16:
        DFK_WIDEN_ONE(uint16_t);
        break;
    case DF_I8:
        DFK_WIDEN_ONE(int8_t);
        break;
    case DF_U8:
        DFK_WIDEN_ONE(uint8_t);
        break;
    default:
        DFK_WIDEN_ONE(int32_t);
        break;
    }
}

#undef DFK_WIDEN_ONE

static void dfk_recenter(DfkMoments* o, double sdx, double sdy, int pair)
{
    double n = o->n, cx, cy;
    if (!(n > 0.0))
        return;
    cx = sdx / n;
    if (pair) {
        cy = sdy / n;
        o->m2x -= cx * sdx;
        o->m2y -= cy * sdy;
        o->cxy -= cx * sdy;
        o->my += cy;
    } else {
        double m2 = o->m2x, m3 = o->m3x;
        o->m2x = m2 - cx * sdx;
        o->m3x = m3 - 3.0 * cx * m2 + 2.0 * n * cx * cx * cx;
        o->m4x = o->m4x - 4.0 * cx * m3 + 6.0 * cx * cx * m2 - 3.0 * n * cx * cx * cx * cx;
    }
    o->mx += cx;
    if (o->m2x < 0.0)
        o->m2x = 0.0;
    if (pair && o->m2y < 0.0)
        o->m2y = 0.0;
}

static void dfk_unrolled(const double* x, const double* y, uint32_t n,
    DfkMoments* o)
{
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0;
    double lo0 = DYN_INFINITY, lo1 = DYN_INFINITY, lo2 = DYN_INFINITY, lo3 = DYN_INFINITY;
    double hi0 = -DYN_INFINITY, hi1 = -DYN_INFINITY, hi2 = -DYN_INFINITY, hi3 = -DYN_INFINITY;
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    double c0 = 0, c1 = 0, c2 = 0, c3 = 0, mx, my = 0.0, sx, sy = 0.0;
    double g0 = 0, g1 = 0, g2 = 0, g3 = 0, h0 = 0;
    uint32_t i, q = n & ~7u, r = n & ~3u;

    for (i = 0; i < q; i += 8) {
        s0 += x[i];
        s1 += x[i + 1];
        s2 += x[i + 2];
        s3 += x[i + 3];
        s4 += x[i + 4];
        s5 += x[i + 5];
        s6 += x[i + 6];
        s7 += x[i + 7];
    }
    for (; i < n; i++)
        s0 += x[i];
    sx = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));

    for (i = 0; i < r; i += 4) {
        if (x[i] < lo0)
            lo0 = x[i];
        if (x[i] > hi0)
            hi0 = x[i];
        if (x[i + 1] < lo1)
            lo1 = x[i + 1];
        if (x[i + 1] > hi1)
            hi1 = x[i + 1];
        if (x[i + 2] < lo2)
            lo2 = x[i + 2];
        if (x[i + 2] > hi2)
            hi2 = x[i + 2];
        if (x[i + 3] < lo3)
            lo3 = x[i + 3];
        if (x[i + 3] > hi3)
            hi3 = x[i + 3];
    }
    for (; i < n; i++) {
        if (x[i] < lo0)
            lo0 = x[i];
        if (x[i] > hi0)
            hi0 = x[i];
    }
    if (lo1 < lo0)
        lo0 = lo1;
    if (lo3 < lo2)
        lo2 = lo3;
    if (lo2 < lo0)
        lo0 = lo2;
    if (hi1 > hi0)
        hi0 = hi1;
    if (hi3 > hi2)
        hi2 = hi3;
    if (hi2 > hi0)
        hi0 = hi2;

    if (y) {
        for (i = 0; i < q; i += 8) {
            t0 += y[i];
            t1 += y[i + 1];
            t2 += y[i + 2];
            t3 += y[i + 3];
            t4 += y[i + 4];
            t5 += y[i + 5];
            t6 += y[i + 6];
            t7 += y[i + 7];
        }
        for (; i < n; i++)
            t0 += y[i];
        sy = ((t0 + t1) + (t2 + t3)) + ((t4 + t5) + (t6 + t7));
        my = sy / (double)n;
    }
    mx = sx / (double)n;

    if (y) {
        for (i = 0; i < r; i += 4) {
            double d0 = x[i] - mx, d1 = x[i + 1] - mx, d2 = x[i + 2] - mx, d3 = x[i + 3] - mx;
            double e0 = y[i] - my, e1 = y[i + 1] - my, e2 = y[i + 2] - my, e3 = y[i + 3] - my;
            g0 += (d0 + d1) + (d2 + d3);
            h0 += (e0 + e1) + (e2 + e3);
            a0 += d0 * d0;
            a1 += d1 * d1;
            a2 += d2 * d2;
            a3 += d3 * d3;
            b0 += e0 * e0;
            b1 += e1 * e1;
            b2 += e2 * e2;
            b3 += e3 * e3;
            c0 += d0 * e0;
            c1 += d1 * e1;
            c2 += d2 * e2;
            c3 += d3 * e3;
        }
        for (; i < n; i++) {
            double d = x[i] - mx, e = y[i] - my;
            g0 += d;
            h0 += e;
            a0 += d * d;
            b0 += e * e;
            c0 += d * e;
        }
        o->m2x = (a0 + a1) + (a2 + a3);
        o->m2y = (b0 + b1) + (b2 + b3);
        o->cxy = (c0 + c1) + (c2 + c3);
        o->m3x = DYN_NAN;
        o->m4x = DYN_NAN;
    } else {
        for (i = 0; i < r; i += 4) {
            double d0 = x[i] - mx, d1 = x[i + 1] - mx, d2 = x[i + 2] - mx, d3 = x[i + 3] - mx;
            g0 += d0;
            g1 += d1;
            g2 += d2;
            g3 += d3;
            a0 += d0 * d0;
            a1 += d1 * d1;
            a2 += d2 * d2;
            a3 += d3 * d3;
            b0 += d0 * d0 * d0;
            b1 += d1 * d1 * d1;
            b2 += d2 * d2 * d2;
            b3 += d3 * d3 * d3;
            c0 += d0 * d0 * d0 * d0;
            c1 += d1 * d1 * d1 * d1;
            c2 += d2 * d2 * d2 * d2;
            c3 += d3 * d3 * d3 * d3;
        }
        for (; i < n; i++) {
            double d = x[i] - mx;
            g0 += d;
            a0 += d * d;
            b0 += d * d * d;
            c0 += d * d * d * d;
        }
        o->m2x = (a0 + a1) + (a2 + a3);
        o->m3x = (b0 + b1) + (b2 + b3);
        o->m4x = (c0 + c1) + (c2 + c3);
        o->m2y = 0.0;
        o->cxy = 0.0;
    }
    o->n = (double)n;
    o->sx = sx;
    o->sy = sy;
    o->mx = mx;
    o->my = my;
    o->lo = lo0;
    o->hi = hi0;
    dfk_recenter(o, (g0 + g1) + (g2 + g3), h0, y != NULL);
}

void dfk_moments(const void* px, DFType tx,
    const void* py, DFType ty,
    const uint8_t* m, uint32_t n, DfkMoments* o)
{
    double cnt = 0.0, sx = 0.0, sy = 0.0, lo = DYN_INFINITY, hi = -DYN_INFINITY;
    double m2x = 0.0, m3x = 0.0, m4x = 0.0, m2y = 0.0, cxy = 0.0;
    double mx = 0.0, my = 0.0, v, w, d, e, sdx = 0.0, sdy = 0.0;
    uint32_t i;
    double *wx = NULL, *wy = NULL;
    double *own_x = NULL, *own_y = NULL;

    if (n) {
        if (tx == DF_F64) {
            wx = DYN_UNCONST(px);
        } else {
            wx = own_x = malloc((size_t)n * sizeof(double));
            if (wx) {
                dfk_widen(px, tx, wx, n);
            }
        }
        if (py) {
            if (ty == DF_F64) {
                wy = DYN_UNCONST(py);
            } else {
                wy = own_y = malloc((size_t)n * sizeof(double));
                if (wy) {
                    dfk_widen(py, ty, wy, n);
                }
            }
            if (!wy) {
                free(own_x);
                wx = own_x = NULL;
            }
        }
    }

#define DFK_X(i) (wx ? wx[i] : df_get(px, tx, i))
#define DFK_Y(i) (wy ? wy[i] : df_get(py, ty, i))

    if (!m && wx && n >= DFK_UNROLL_MIN && (!py || wy)) {
        dfk_unrolled(wx, wy, n, o);
        free(own_x);
        free(own_y);
        return;
    }

    if (py) {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFK_X(i);
            w = DFK_Y(i);
            cnt += 1.0;
            sx += v;
            sy += w;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFK_X(i);
            cnt += 1.0;
            sx += v;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
    }

    if (cnt > 0.0) {
        mx = sx / cnt;
        my = sy / cnt;
        if (py) {
            m3x = DYN_NAN;
            m4x = DYN_NAN;
            for (i = 0; i < n; i++) {
                if (m && !m[i])
                    continue;
                d = DFK_X(i) - mx;
                e = DFK_Y(i) - my;
                sdx += d;
                sdy += e;
                m2x += d * d;
                m2y += e * e;
                cxy += d * e;
            }
        } else {
            for (i = 0; i < n; i++) {
                if (m && !m[i])
                    continue;
                d = DFK_X(i) - mx;
                sdx += d;
                m2x += d * d;
                m3x += d * d * d;
                m4x += d * d * d * d;
            }
        }
    } else if (py) {
        m3x = DYN_NAN;
        m4x = DYN_NAN;
    }
#undef DFK_X
#undef DFK_Y
    free(own_x);
    free(own_y);

    o->n = cnt;
    o->sx = sx;
    o->sy = sy;
    o->mx = mx;
    o->my = my;
    o->m2x = m2x;
    o->m3x = m3x;
    o->m4x = m4x;
    o->m2y = m2y;
    o->cxy = cxy;
    o->lo = lo;
    o->hi = hi;
    dfk_recenter(o, sdx, sdy, py != NULL);
}
