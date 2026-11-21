#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

static inline double dfg_pick(uint32_t sel, double v)
{
    uint64_t b, m;

    memcpy(&b, &v, sizeof(b));
    m = (uint64_t)0 - (uint64_t)(sel != 0);
    b = (b & m) | (0ULL & ~m);
    memcpy(&v, &b, sizeof(v));
    return v;
}

#define DFG_ROLL_VAR_WORK_MAX 100000000.0
#define DFG_ROLL_VAR_TOL 1e-10
#define DFG_ROLL_VAR_CADENCE 131072u
#define DFG_ROLL_VAR_XPASS 16u

const char* dfg_agg_verb(int magic)
{
    switch (magic) {
    case DF_MIN:
        return "take the min of";
    case DF_MAX:
        return "take the max of";
    default:
        return "take the mean of";
    }
}

const char* dfg_roll_name(int magic)
{
    switch (magic) {
    case DF_MIN:
        return "ROLLING_MIN";
    case DF_MAX:
        return "ROLLING_MAX";
    case DF_MEAN:
        return "ROLLING_MEAN";
    default:
        return "ROLLING_SUM";
    }
}

uint32_t dfg_roll_win2(const double* x, const uint8_t* mask,
    uint32_t lo, uint32_t hi, double* pm, double* pq,
    double* ps)
{
    double mean = 0.0, m2 = 0.0, dev = 0.0, sum = 0.0, err = 0.0;
    uint32_t c = 0, j;

    for (j = lo; j <= hi; j++) {
        double t, xv;
        if (mask && !mask[j])
            continue;
        xv = x[j];
        t = sum + xv;
        if (fabs(sum) >= fabs(xv))
            err += (sum - t) + xv;
        else
            err += (xv - t) + sum;
        sum = t;
        c++;
    }
    if (c == 0) {
        *pm = 0.0;
        *pq = 0.0;
        *ps = 0.0;
        return 0;
    }
    mean = (sum + err) / (double)c;
    for (j = lo; j <= hi; j++) {
        double d;
        if (mask && !mask[j])
            continue;
        d = x[j] - mean;
        m2 += d * d;
        dev += d;
    }
    *pm = mean;
    *pq = m2;
    *ps = dev;
    return c;
}

#define DFG_ROLL_VAR_SLIDE(SLIDED)                                         \
    do {                                                                   \
        for (i = w; i < span; i++) {                                       \
            uint32_t mi, mo;                                               \
            double din, dout, a, m2, bnd, t2;                              \
            if (SLIDED) {                                                  \
                mi = (uint32_t)(mask[i] != 0);                             \
                mo = (uint32_t)(mask[i - w] != 0);                         \
            } else {                                                       \
                mi = 1;                                                    \
                mo = 1;                                                    \
            }                                                              \
            if (mi && !isfinite(x[i]))                                     \
                nf++;                                                      \
            if (mo && !isfinite(x[i - w])) {                               \
                nf--;                                                      \
                if (nf == 0) {                                             \
                    c = dfg_roll_win2(x, mask, i + 1 - w, i, &K, &Q, &S);  \
                    eq = 0.0;                                              \
                    es = 0.0;                                              \
                    t = 0;                                                 \
                    dst[i] = c < 2 ? DYN_NAN                               \
                                   : (want_std ? sqrt(Q / (double)(c - 1)) \
                                               : Q / (double)(c - 1));     \
                    continue;                                              \
                }                                                          \
            }                                                              \
            if (nf != 0) {                                                 \
                dst[i] = DYN_NAN;                                          \
                continue;                                                  \
            }                                                              \
            din = dfg_pick(mi, x[i] - K);                                  \
            dout = dfg_pick(mo, x[i - w] - K);                             \
            S += din - dout;                                               \
            Q += din * din - dout * dout;                                  \
            c += (int32_t)mi - (int32_t)mo;                                \
            if (c < 2) {                                                   \
                dst[i] = DYN_NAN;                                          \
                continue;                                                  \
            }                                                              \
            a = S / (double)c;                                             \
            m2 = Q - S * a;                                                \
            t2 = din * din;                                                \
            if (dout * dout > t2)                                          \
                t2 = dout * dout;                                          \
                                                                           \
            eq += 4.0 * DBL_EPSILON * (Q > t2 ? Q : t2);                   \
            t2 = fabs(din) > fabs(dout) ? fabs(din) : fabs(dout);          \
            es += 2.0 * DBL_EPSILON * (fabs(S) > t2 ? fabs(S) : t2);       \
            bnd = eq + 2.0 * fabs(S) * es / (double)c                      \
                + 2.0 * DBL_EPSILON * (Q > fabs(S * a) ? Q : fabs(S * a)); \
            t++;                                                           \
            if (t >= cadence || !(m2 >= 0.0)                               \
                || !(bnd <= DFG_ROLL_VAR_TOL * m2)) {                      \
                if (rc * (uint64_t)w * 2u                                  \
                    > (uint64_t)DFG_ROLL_VAR_XPASS * span)                 \
                    goto decline;                                          \
                rc++;                                                      \
                c = dfg_roll_win2(x, mask, i + 1 - w, i, &K, &Q, &S);      \
                eq = 0.0;                                                  \
                t = 0;                                                     \
                es = 0.0;                                                  \
                m2 = Q;                                                    \
            }                                                              \
            dst[i] = want_std ? sqrt(m2 / (double)(c - 1))                 \
                              : m2 / (double)(c - 1);                      \
        }                                                                  \
    } while (0)

int dfg_roll_var_slide(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_std)
{
    const double* x;
    double* own = NULL;
    double K = 0.0, S = 0.0, Q = 0.0, eq = 0.0, es = 0.0;
    uint32_t i, c = 0, rc = 0, t = 0, cadence, nf = 0;

    if (w < 2 || span < w)
        return -1;
    if ((double)span * (double)w <= DFG_ROLL_VAR_WORK_MAX)
        return -1;
    if (b->type == DF_F64) {
        x = b->p;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        df_widen(b, own, span);
    }

    cadence = w < DFG_ROLL_VAR_CADENCE ? DFG_ROLL_VAR_CADENCE : w;
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;
    for (i = 0; i < w; i++)
        if ((!mask || mask[i]) && !isfinite(x[i]))
            nf++;
    c = dfg_roll_win2(x, mask, 0, w - 1, &K, &Q, &S);
    if (c < 2 || nf != 0) {
        dst[w - 1] = DYN_NAN;
    } else {
        dst[w - 1] = want_std ? sqrt(Q / (double)(c - 1))
                              : Q / (double)(c - 1);
    }
    if (mask)
        DFG_ROLL_VAR_SLIDE(1);
    else
        DFG_ROLL_VAR_SLIDE(0);
    free(own);
    return 0;
decline:
    free(own);
    return -1;
}

#undef DFG_ROLL_VAR_SLIDE

#endif
