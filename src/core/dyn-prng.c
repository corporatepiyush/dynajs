#include "dyn-prng.h"
#include "dyn-common.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
int dyn_os_entropy(void* buf, size_t n)
{
    arc4random_buf(buf, n);
    return 0;
}
#else
#include <sys/random.h>
#include <errno.h>
int dyn_os_entropy(void* buf, size_t n)
{
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += got;
        n -= (size_t)got;
    }
    return 0;
}
#endif

uint64_t dyn_splitmix64(uint64_t* x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void dyn_prng_seed(dyn_prng_t* r, uint64_t seed)
{
    uint64_t sm = seed;
    r->s[0] = dyn_splitmix64(&sm);
    r->s[1] = dyn_splitmix64(&sm);
    r->s[2] = dyn_splitmix64(&sm);
    r->s[3] = dyn_splitmix64(&sm);
}

uint64_t dyn_prng_next(dyn_prng_t* r)
{
    uint64_t result = dyn_rotl64(r->s[1] * 5, 7) * 9;
    dyn_prng_step(r);
    return result;
}

double dyn_prng_next_double(dyn_prng_t* r)
{
    return (double)(dyn_prng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t dyn_prng_next_bounded(dyn_prng_t* r, uint64_t bound)
{
    uint64_t threshold;
    if (bound == 0)
        return 0;
    threshold = (0 - bound) % bound;
    for (;;) {
        uint64_t v = dyn_prng_next(r);
        if (v >= threshold)
            return v % bound;
    }
}

void dyn_prng_fill(dyn_prng_t* restrict r, uint8_t* restrict dst, size_t n)
{
    while (n >= 8) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, 8);
        dst += 8;
        n -= 8;
    }
    if (n > 0) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, n);
    }
}

int dyn_prng_seed_random(dyn_prng_t* r)
{
    if (dyn_os_entropy(r->s, sizeof(r->s)) < 0)
        return -1;
    if (!(r->s[0] | r->s[1] | r->s[2] | r->s[3]))
        r->s[0] = 0x9e3779b97f4a7c15ULL;
    return 0;
}

static const uint64_t dyn_jump_table[4] = {
    0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
    0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL
};
static const uint64_t dyn_long_jump_table[4] = {
    0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL,
    0x77710069854ee241ULL, 0x39109bb02acbe635ULL
};

static void dyn_jump(dyn_prng_t* r, const uint64_t table[4])
{
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int w, b;

    for (w = 0; w < 4; w++) {
        for (b = 0; b < 64; b++) {
            if ((table[w] >> b) & 1) {
                s0 ^= r->s[0];
                s1 ^= r->s[1];
                s2 ^= r->s[2];
                s3 ^= r->s[3];
            }
            dyn_prng_step(r);
        }
    }
    r->s[0] = s0;
    r->s[1] = s1;
    r->s[2] = s2;
    r->s[3] = s3;
}

void dyn_prng_jump(dyn_prng_t* r)
{
    dyn_jump(r, dyn_jump_table);
}

void dyn_prng_long_jump(dyn_prng_t* r)
{
    dyn_jump(r, dyn_long_jump_table);
}

#include <math.h>

double dyn_prng_normal(dyn_prng_t* r, double mu, double sigma)
{
    for (;;) {
        double u = 2.0 * dyn_prng_next_double(r) - 1.0;
        double v = 2.0 * dyn_prng_next_double(r) - 1.0;
        double s = u * u + v * v;
        if (s >= 1.0 || s == 0.0)
            continue;
        return mu + sigma * (u * sqrt(-2.0 * log(s) / s));
    }
}

double dyn_prng_exponential(dyn_prng_t* r, double lambda)
{
    double u = dyn_prng_next_double(r);
    return -log1p(-u) / lambda;
}

typedef struct {
    uint64_t key;
    double pm, sum;
} dyn_prng_memo_t;

static _Thread_local dyn_prng_memo_t dyn_prng_pois_memo;

static void dyn_prng_poisson_head(double lambda, int64_t m, double* ppm, double* psum)
{
    dyn_prng_memo_t* c = &dyn_prng_pois_memo;
    uint64_t key;
    double pm, sum_down, term;
    int64_t k;
    memcpy(&key, &lambda, sizeof key);
    if (c->key == key && key != 0) {
        *ppm = c->pm;
        *psum = c->sum;
        return;
    }
    pm = exp(-lambda + (double)m * log(lambda) - lgamma((double)m + 1.0));
    sum_down = pm;
    term = pm;
    for (k = m; k > 0; k--) {
        term = term * (double)k / lambda;
        if (term < 1e-20)
            break;
        sum_down += term;
    }
    c->key = key;
    c->pm = pm;
    c->sum = sum_down;
    *ppm = pm;
    *psum = sum_down;
}

int64_t dyn_prng_poisson(dyn_prng_t* r, double lambda)
{
    double u;

    if (lambda <= 0.0)
        return 0;

    if (lambda < 30.0) {
        double L = exp(-lambda), p = 1.0;
        int64_t k = 0;
        do {
            k++;
            p *= dyn_prng_next_double(r);
        } while (p > L);
        return k - 1;
    }

    u = dyn_prng_next_double(r);
    {
        int64_t m = (int64_t)floor(lambda);
        double pm, sum_down, term;
        int64_t k;
        dyn_prng_poisson_head(lambda, m, &pm, &sum_down);
        if (u < sum_down) {
            double D = sum_down - pm;
            term = pm;
            for (k = m;; k--) {
                if (u >= D)
                    return k;
                if (k == 0)
                    return 0;
                term = term * (double)k / lambda;
                D -= term;
            }
        }
        {
            double D = sum_down;
            term = pm;
            for (k = m + 1;; k++) {
                term = term * lambda / (double)k;
                D += term;
                if (u < D)
                    return k;
                if (term < 1e-20)
                    return k;
            }
        }
    }
}

static double dyn_prng_gamma_mt(dyn_prng_t* r, double shape)
{
    double d = shape - 1.0 / 3.0, c = 1.0 / sqrt(9.0 * d);
    for (;;) {
        double x = dyn_prng_normal(r, 0.0, 1.0), v = 1.0 + c * x, u;
        if (v <= 0.0)
            continue;
        v = v * v * v;
        u = 1.0 - dyn_prng_next_double(r);
        if (u < 1.0 - 0.0331 * (x * x) * (x * x))
            return d * v;
        if (log(u) < 0.5 * x * x + d * (1.0 - v + log(v)))
            return d * v;
    }
}

double dyn_prng_log_gamma(dyn_prng_t* r, double shape)
{
    if (shape >= 1.0)
        return log(dyn_prng_gamma_mt(r, shape));
    {
        double g = dyn_prng_gamma_mt(r, shape + 1.0);
        double u = 1.0 - dyn_prng_next_double(r);
        return log(g) + log(u) / shape;
    }
}

double dyn_prng_gamma(dyn_prng_t* r, double shape, double scale)
{
    if (shape >= 1.0)
        return dyn_prng_gamma_mt(r, shape) * scale;
    return exp(dyn_prng_log_gamma(r, shape)) * scale;
}

double dyn_prng_beta(dyn_prng_t* r, double a, double b)
{
    if (a >= 1.0 && b >= 1.0) {
        double x = dyn_prng_gamma_mt(r, a), y = dyn_prng_gamma_mt(r, b);
        return x / (x + y);
    }
    {
        double la = dyn_prng_log_gamma(r, a), lb = dyn_prng_log_gamma(r, b);
        return la >= lb ? 1.0 / (1.0 + exp(lb - la)) : exp(la - lb) / (1.0 + exp(la - lb));
    }
}

#define DYN_PRNG_BINOM_CAP 512

typedef struct {
    int64_t n, lo, hi;
    uint64_t p;
    double pm, plo, phi, acc;
    int side, cnt, warm;
    double a[DYN_PRNG_BINOM_CAP];
    int32_t v[DYN_PRNG_BINOM_CAP];
} dyn_prng_binom_t;

typedef struct {
    int64_t lo, hi;
    double plo, phi, acc;
    int side;
} dyn_prng_binom_walk_t;

static _Thread_local dyn_prng_binom_t dyn_prng_binom_memo;

static int dyn_prng_binom_step(dyn_prng_binom_walk_t* w, int64_t n, double ratio_up,
    double ratio_down, int64_t* out)
{
    int tries;
    for (tries = 0; tries < 2; tries++) {
        if (w->side == 0) {
            w->side = 1;
            if (w->hi < n && w->phi > 0.0) {
                w->phi *= (double)(n - w->hi) / (double)(w->hi + 1) * ratio_up;
                w->hi++;
                w->acc += w->phi;
                *out = w->hi;
                return 1;
            }
        } else {
            w->side = 0;
            if (w->lo > 0 && w->plo > 0.0) {
                w->plo *= (double)w->lo / (double)(n - w->lo + 1) * ratio_down;
                w->lo--;
                w->acc += w->plo;
                *out = w->lo;
                return 1;
            }
        }
    }
    return 0;
}

int64_t dyn_prng_binomial(dyn_prng_t* r, int64_t n, double p)
{
    double q = 1.0 - p, u, ratio_up, ratio_down;
    dyn_prng_binom_t* c = &dyn_prng_binom_memo;
    dyn_prng_binom_walk_t w;
    int64_t m, val;
    uint64_t pk;
    if (n <= 0 || p <= 0.0)
        return 0;
    if (p >= 1.0)
        return n;
    m = (int64_t)floor((double)(n + 1) * p);
    if (m > n)
        m = n;
    memcpy(&pk, &p, sizeof pk);
    if (c->n != n || c->p != pk) {
        c->pm = exp(lgamma((double)n + 1.0) - lgamma((double)m + 1.0) - lgamma((double)(n - m) + 1.0)
            + (double)m * log(p) + (double)(n - m) * log1p(-p));
        c->n = n;
        c->p = pk;
        c->lo = c->hi = m;
        c->plo = c->phi = c->acc = c->pm;
        c->side = 0;
        c->cnt = 0;
        c->warm = 0;
    } else {
        c->warm = 1;
    }
    u = dyn_prng_next_double(r);
    if (u < c->pm)
        return m;
    if (c->cnt > 0 && u < c->a[c->cnt - 1]) {
        int lo = 0, hi = c->cnt - 1;
        while (lo < hi) {
            int mid = (lo + hi) >> 1;
            if (u < c->a[mid])
                hi = mid;
            else
                lo = mid + 1;
        }
        return m + c->v[lo];
    }
    ratio_up = p / q;
    ratio_down = q / p;
    if (!c->warm) {
        double acc = c->pm, plo = c->pm, phi = c->pm;
        int64_t lo = m, hi = m;
        for (;;) {
            if (hi < n && phi > 0.0) {
                phi *= (double)(n - hi) / (double)(hi + 1) * ratio_up;
                hi++;
                acc += phi;
                if (u < acc)
                    return hi;
            }
            if (lo > 0 && plo > 0.0) {
                plo *= (double)lo / (double)(n - lo + 1) * ratio_down;
                lo--;
                acc += plo;
                if (u < acc)
                    return lo;
            }
            if ((hi >= n || !(phi > 0.0)) && (lo <= 0 || !(plo > 0.0)))
                return m;
        }
    }
    w.lo = c->lo;
    w.hi = c->hi;
    w.plo = c->plo;
    w.phi = c->phi;
    w.acc = c->acc;
    w.side = c->side;
    val = m;
    while (c->cnt < DYN_PRNG_BINOM_CAP) {
        if (!dyn_prng_binom_step(&w, n, ratio_up, ratio_down, &val)) {
            val = m;
            break;
        }
        c->a[c->cnt] = w.acc;
        c->v[c->cnt] = (int32_t)(val - m);
        c->cnt++;
        if (u < w.acc)
            break;
    }
    c->lo = w.lo;
    c->hi = w.hi;
    c->plo = w.plo;
    c->phi = w.phi;
    c->acc = w.acc;
    c->side = w.side;
    if (c->cnt < DYN_PRNG_BINOM_CAP || u < w.acc)
        return val;
    for (;;) {
        if (!dyn_prng_binom_step(&w, n, ratio_up, ratio_down, &val))
            return m;
        if (u < w.acc)
            return val;
    }
}
