#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-cdefs.h"
#include "../dyn-lak.h"

const uint8_t dyn_nd_esize_tab[DYN_ND_NTYPES] = { 8, 4, 2, 2, 4, 2, 1, 1, 1 };

float dyn_nd_f16_to_f32(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff, u;
    float f;
    if (e == 0) {
        f = ldexpf((float)m, -24);
        return s ? -f : f;
    }
    if (e == 31)
        u = s | 0x7f800000u | (m << 13);
    else
        u = s | ((e + 112) << 23) | (m << 13);
    memcpy(&f, &u, 4);
    return f;
}

static uint16_t nd_round_small(double v, int mant, int bias, int emax_field)
{
    uint16_t sign = signbit(v) ? (uint16_t)(1u << (mant + (emax_field == 31 ? 5 : 8))) : 0;
    uint16_t inf = (uint16_t)(sign | ((unsigned)emax_field << mant));
    double a = fabs(v), r;
    int e;
    if (v != v)
        return (uint16_t)(inf | (1u << (mant - 1)));
    if (a == 0.0)
        return sign;
    if (isinf(a))
        return inf;
    if (a < ldexp(1.0, 1 - bias)) {
        r = nearbyint(ldexp(a, bias - 1 + mant));
        return (uint16_t)(sign | (uint16_t)r);
    }
    (void)frexp(a, &e);
    e -= 1;
    r = nearbyint(ldexp(a, mant - e));
    if (r >= ldexp(1.0, mant + 1)) {
        r = ldexp(1.0, mant);
        e++;
    }
    if (e + bias >= emax_field)
        return inf;
    return (uint16_t)(sign | ((unsigned)(e + bias) << mant) | ((unsigned)r - (1u << mant)));
}

uint16_t dyn_nd_f64_to_f16(double v)
{
    return nd_round_small(v, 10, 15, 31);
}

uint16_t dyn_nd_f64_to_bf16(double v)
{
    return nd_round_small(v, 7, 127, 255);
}

uint16_t dyn_nd_f32_to_f16(float f)
{
    return dyn_nd_f64_to_f16((double)f);
}

float dyn_nd_bf16_to_f32(uint16_t h)
{
    uint32_t u = (uint32_t)h << 16;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

uint16_t dyn_nd_f32_to_bf16(float f)
{
    return dyn_nd_f64_to_bf16((double)f);
}

static double nd_sat(double v, double lo, double hi)
{
    if (v != v)
        return 0;
    v = trunc(v);
    return v < lo ? lo : v > hi ? hi : v;
}

double dyn_nd_load(int dtype, const void* p)
{
    switch (dtype) {
    case DYN_ND_F64: {
        double v;
        memcpy(&v, p, 8);
        return v;
    }
    case DYN_ND_F32: {
        float v;
        memcpy(&v, p, 4);
        return (double)v;
    }
    case DYN_ND_F16:
    case DYN_ND_BF16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return dtype == DYN_ND_F16 ? (double)dyn_nd_f16_to_f32(v) : (double)dyn_nd_bf16_to_f32(v);
    }
    case DYN_ND_I32: {
        int32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    case DYN_ND_I16: {
        int16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    case DYN_ND_I8:
        return *(const int8_t*)p;
    case DYN_ND_U8:
        return *(const uint8_t*)p;
    case DYN_ND_BOOL:
        return *(const uint8_t*)p ? 1 : 0;
    default:
        return (double)NAN;
    }
}

void dyn_nd_store(int dtype, void* p, double v)
{
    switch (dtype) {
    case DYN_ND_F64:
        memcpy(p, &v, 8);
        break;
    case DYN_ND_F32: {
        float f = (float)v;
        memcpy(p, &f, 4);
        break;
    }
    case DYN_ND_F16:
    case DYN_ND_BF16: {
        uint16_t h = dtype == DYN_ND_F16 ? dyn_nd_f64_to_f16(v) : dyn_nd_f64_to_bf16(v);
        memcpy(p, &h, 2);
        break;
    }
    case DYN_ND_I32: {
        int32_t x = (int32_t)nd_sat(v, -2147483648.0, 2147483647.0);
        memcpy(p, &x, 4);
        break;
    }
    case DYN_ND_I16: {
        int16_t x = (int16_t)nd_sat(v, -32768.0, 32767.0);
        memcpy(p, &x, 2);
        break;
    }
    case DYN_ND_I8:
        *(int8_t*)p = (int8_t)nd_sat(v, -128.0, 127.0);
        break;
    case DYN_ND_U8:
        *(uint8_t*)p = (uint8_t)nd_sat(v, 0.0, 255.0);
        break;
    case DYN_ND_BOOL:
        *(uint8_t*)p = (v != 0 && v == v) ? 1 : 0;
        break;
    default:
        break;
    }
}

int64_t dyn_nd_shape_size(int ndim, const int64_t* shape)
{
    int64_t s = 1;
    int i;
    for (i = 0; i < ndim; i++) {
        if (shape[i] < 0)
            return -1;
        if (shape[i] && s > INT64_MAX / shape[i])
            return -1;
        s *= shape[i];
    }
    return s;
}

void dyn_nd_contig_strides(int ndim, const int64_t* shape, int64_t* strides)
{
    int64_t s = 1;
    int i;
    for (i = ndim - 1; i >= 0; i--) {
        strides[i] = s;
        s *= shape[i] > 0 ? shape[i] : 1;
    }
}

int dyn_nd_is_contig(const dyn_nd_t* a)
{
    int64_t s = 1;
    int i;
    for (i = a->ndim - 1; i >= 0; i--) {
        if (a->shape[i] != 1 && a->strides[i] != s)
            return 0;
        s *= a->shape[i];
    }
    return 1;
}

static void nd_extent(const dyn_nd_t* a, int64_t* lo, int64_t* hi)
{
    int64_t l = a->offset, h = a->offset;
    int i;
    for (i = 0; i < a->ndim; i++) {
        int64_t d = (a->shape[i] - 1) * a->strides[i];
        if (d < 0)
            l += d;
        else
            h += d;
    }
    *lo = l;
    *hi = h;
}

int dyn_nd_bounds_ok(const dyn_nd_t* a)
{
    int64_t lo, hi;
    if ((unsigned)a->dtype >= DYN_ND_NTYPES || a->ndim < 0 || a->ndim > DYN_ND_MAXDIM)
        return 0;
    if (dyn_nd_shape_size(a->ndim, a->shape) != a->size)
        return 0;
    if (a->size == 0)
        return 1;
    nd_extent(a, &lo, &hi);
    return lo >= 0 && hi < a->base_len && (a->base != NULL);
}

static char* nd_ptr(const dyn_nd_t* a, int64_t lin)
{
    int64_t off = a->offset;
    int i;
    for (i = a->ndim - 1; i >= 0; i--) {
        int64_t n = a->shape[i];
        off += (lin % n) * a->strides[i];
        lin /= n;
    }
    return (char*)a->base + (size_t)off * dyn_nd_esize(a->dtype);
}

double dyn_nd_get(const dyn_nd_t* a, int64_t lin)
{
    return dyn_nd_load(a->dtype, nd_ptr(a, lin));
}

void dyn_nd_set(const dyn_nd_t* a, int64_t lin, double v)
{
    dyn_nd_store(a->dtype, nd_ptr(a, lin), v);
}

int dyn_nd_overlap(const dyn_nd_t* a, const dyn_nd_t* b)
{
    int64_t alo, ahi, blo, bhi;
    uintptr_t as, ae, bs, be;
    if (a->size == 0 || b->size == 0)
        return 0;
    nd_extent(a, &alo, &ahi);
    nd_extent(b, &blo, &bhi);
    as = (uintptr_t)a->base + (uintptr_t)alo * dyn_nd_esize(a->dtype);
    ae = (uintptr_t)a->base + (uintptr_t)(ahi + 1) * dyn_nd_esize(a->dtype);
    bs = (uintptr_t)b->base + (uintptr_t)blo * dyn_nd_esize(b->dtype);
    be = (uintptr_t)b->base + (uintptr_t)(bhi + 1) * dyn_nd_esize(b->dtype);
    return as < be && bs < ae;
}

typedef struct {
    int nd;
    int64_t idx[DYN_ND_MAXDIM];
    int64_t off;
} nd_it_t;

static void nd_walk(const dyn_nd_t* a, void (*fn)(void* u, int64_t k, char* p, int64_t n, int64_t st), void* u)
{
    nd_it_t it;
    int64_t k = 0, inner, ist;
    size_t es = dyn_nd_esize(a->dtype);
    int d;
    if (a->size == 0)
        return;
    if (a->ndim == 0) {
        fn(u, 0, (char*)a->base + (size_t)a->offset * es, 1, 0);
        return;
    }
    memset(&it, 0, sizeof(it));
    it.nd = a->ndim;
    it.off = a->offset;
    inner = a->shape[a->ndim - 1];
    ist = a->strides[a->ndim - 1];
    for (;;) {
        fn(u, k, (char*)a->base + it.off * (int64_t)es, inner, ist);
        k += inner;
        for (d = a->ndim - 2; d >= 0; d--) {
            it.idx[d]++;
            it.off += a->strides[d];
            if (it.idx[d] < a->shape[d])
                break;
            it.off -= a->strides[d] * a->shape[d];
            it.idx[d] = 0;
        }
        if (d < 0)
            break;
    }
}

typedef struct {
    int dtype;
    double* buf;
} nd_f64_u;

static void nd_to_f64_fn(void* u, int64_t k, char* p, int64_t n, int64_t st)
{
    nd_f64_u* c = u;
    size_t es = dyn_nd_esize(c->dtype);
    int64_t i;
    if (c->dtype == DYN_ND_F64) {
        for (i = 0; i < n; i++)
            memcpy(&c->buf[k + i], p + i * st * 8, 8);
        return;
    }
    for (i = 0; i < n; i++)
        c->buf[k + i] = dyn_nd_load(c->dtype, p + i * st * (int64_t)es);
}

static void nd_from_f64_fn(void* u, int64_t k, char* p, int64_t n, int64_t st)
{
    nd_f64_u* c = u;
    size_t es = dyn_nd_esize(c->dtype);
    int64_t i;
    for (i = 0; i < n; i++)
        dyn_nd_store(c->dtype, p + i * st * (int64_t)es, c->buf[k + i]);
}

void dyn_nd_to_f64(const dyn_nd_t* src, double* dst)
{
    nd_f64_u c = { src->dtype, dst };
    nd_walk(src, nd_to_f64_fn, &c);
}

void dyn_nd_from_f64(const dyn_nd_t* dst, const double* src)
{
    nd_f64_u c = { dst->dtype, (double*)DYN_UNCONST(src) };
    nd_walk(dst, nd_from_f64_fn, &c);
}

typedef struct {
    const dyn_nd_t* src;
    int dt;
    char* raw;
} nd_copy_u;

static void nd_gather_fn(void* u, int64_t k, char* p, int64_t n, int64_t st)
{
    nd_copy_u* c = u;
    size_t es = dyn_nd_esize(c->dt);
    int64_t i;
    for (i = 0; i < n; i++)
        memcpy(c->raw + (size_t)(k + i) * es, p + i * st * (int64_t)es, es);
}

static void nd_scatter_fn(void* u, int64_t k, char* p, int64_t n, int64_t st)
{
    nd_copy_u* c = u;
    size_t es = dyn_nd_esize(c->dt);
    int64_t i;
    for (i = 0; i < n; i++)
        memcpy(p + i * st * (int64_t)es, c->raw + (size_t)(k + i) * es, es);
}

int dyn_nd_copy(const dyn_nd_t* dst, const dyn_nd_t* src)
{
    nd_copy_u c;
    size_t es;
    if (dst->size == 0)
        return 0;
    if (dst->dtype != src->dtype) {
        double* t = malloc((size_t)dst->size * sizeof(double));
        if (!t)
            return -1;
        dyn_nd_to_f64(src, t);
        dyn_nd_from_f64(dst, t);
        free(t);
        return 0;
    }
    es = dyn_nd_esize(src->dtype);
    c.src = src;
    c.dt = src->dtype;
    c.raw = malloc((size_t)src->size * es);
    if (!c.raw)
        return -1;
    nd_walk(src, nd_gather_fn, &c);
    nd_walk(dst, nd_scatter_fn, &c);
    free(c.raw);
    return 0;
}
