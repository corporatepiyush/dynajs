#ifndef DYN_DECNUM_DECNUM_H
#define DYN_DECNUM_DECNUM_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    void* (*alloc)(size_t n);
    void* (*zalloc)(size_t n, size_t size);
    void* (*resize)(void* p, size_t n);
    void (*release)(void* p);
} dyn_dec_allocator_t;

extern _Atomic(const dyn_dec_allocator_t*) dyn_dec_allocator;

void dyn_dec_set_allocator(const dyn_dec_allocator_t* a);

static inline const dyn_dec_allocator_t* dyn_dec_alloc_get(void)
{
    return atomic_load_explicit(&dyn_dec_allocator, memory_order_relaxed);
}

static inline void* dyn_dec_malloc(size_t n)
{
    return dyn_dec_alloc_get()->alloc(n);
}

static inline void* dyn_dec_calloc(size_t n, size_t size)
{
    return dyn_dec_alloc_get()->zalloc(n, size);
}

static inline void* dyn_dec_realloc(void* p, size_t n)
{
    return dyn_dec_alloc_get()->resize(p, n);
}

static inline void dyn_dec_free(void* p)
{
    dyn_dec_alloc_get()->release(p);
}

#define DEC_E_INVALID (-4)
#define DEC_E_OVERFLOW (-5)
#define DEC_E_BUDGET (-6)
#define DEC_SERIES_CELLS 268435456.0
#define DEC_INLINE 40
#define DEC_MAX_DIGITS 100000u
#define DEC_MAX_MUL_CELLS (1u << 26)
#define DEC_MAX_DIV_CELLS (1u << 23)
#define DEC_E_TOOSLOW (-3)
#define DEC_MAX_TEXT 1000000

enum { RND_UP,
    RND_DOWN,
    RND_CEIL,
    RND_FLOOR,
    RND_HALF_UP,
    RND_HALF_DOWN,
    RND_HALF_EVEN,
    RND_HALF_ODD };

typedef struct {
    int32_t exp;
    uint32_t nd, cap;
    uint8_t sign;
    uint8_t* d;
    uint8_t inl[DEC_INLINE];
} dec_t;

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom, too_long;
} db_t;

int dec_grow(dec_t* x, uint32_t need);
void dec_trim(dec_t* x);
int dec_add(dec_t* r, const dec_t* a, const dec_t* b);
int dec_div(dec_t* r, const dec_t* a, const dec_t* b, uint32_t prec, int mode);
int dec_parse(dec_t* x, const char* s, size_t n);
int dec_atanh2(dec_t* r, const dec_t* u, uint32_t w);
int dec_small(dec_t* x, int64_t v);
int dec_const_ln2(dec_t* r, uint32_t w);
int dec_const_ln10(dec_t* r, uint32_t w);
int dec_sub_abs(dec_t* r, const dec_t* a, const dec_t* b);
int dec_sub(dec_t* r, const dec_t* a, const dec_t* b);
int dec_mul(dec_t* r, const dec_t* a, const dec_t* b);
int dec_round_at(dec_t* x, int32_t target, int mode, int sticky);
int dec_round_sig(dec_t* x, uint32_t prec, int mode, int sticky);
uint32_t dec_sub_into(uint8_t* rem, uint32_t rn, const uint8_t* b, uint32_t bn);
int dec_divmod_int(uint8_t* q, uint32_t* qn, uint8_t* rem, uint32_t* rn, const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn);
int dec_mul_into(dec_t* acc, const dec_t* m);

uint8_t money_minor(const char* code);
int dec_int_divmod(dec_t* qout, dec_t* r, const dec_t* a, const dec_t* b);
void dec_write_fixed(db_t* b, const dec_t* x, int32_t dp, int neg);
int dec_exp(dec_t* r, const dec_t* a, uint32_t prec);
int dec_ln(dec_t* r, const dec_t* a, uint32_t prec);
int dec_log10(dec_t* r, const dec_t* a, uint32_t prec);
int dec_pow_uint(dec_t* acc, dec_t* base, uint64_t e);
int dec_sqrt(dec_t* r, const dec_t* a, uint32_t prec);

static inline void dec_init(dec_t* x)
{
    x->exp = 0;
    x->nd = 0;
    x->sign = 0;
    x->d = x->inl;
    x->cap = DEC_INLINE;
}

static inline void dec_free(dec_t* x)
{
    if (!x)
        return;
    if (x->d != x->inl)
        dyn_dec_free(x->d);
    x->d = x->inl;
    x->cap = DEC_INLINE;
    x->nd = 0;
}

static inline int dec_is_zero(const dec_t* x) { return x->nd == 0; }

static inline int dec_at(const dec_t* x, int32_t p)
{
    if (p < x->exp || p >= x->exp + (int32_t)x->nd)
        return 0;
    return x->d[p - x->exp];
}

static inline int dec_cmp_abs(const dec_t* a, const dec_t* b)
{
    int32_t ha, hb, lo, p;

    if (a->nd == 0)
        return b->nd == 0 ? 0 : -1;
    if (b->nd == 0)
        return 1;
    ha = a->exp + (int32_t)a->nd;
    hb = b->exp + (int32_t)b->nd;
    if (ha != hb)
        return ha < hb ? -1 : 1;
    lo = a->exp < b->exp ? a->exp : b->exp;
    for (p = ha - 1; p >= lo; p--) {
        int da = dec_at(a, p), db = dec_at(b, p);
        if (da != db)
            return da < db ? -1 : 1;
    }
    return 0;
}

static inline int dec_cmp(const dec_t* a, const dec_t* b)
{
    int c;
    if (a->nd == 0 && b->nd == 0)
        return 0;
    if (a->sign != b->sign)
        return a->sign ? -1 : 1;
    c = dec_cmp_abs(a, b);
    return a->sign ? -c : c;
}

static inline int dec_copy(dec_t* r, const dec_t* a)
{
    if (r == a)
        return 0;
    if (dec_grow(r, a->nd ? a->nd : 1) < 0)
        return -1;
    memcpy(r->d, a->d, a->nd);
    r->nd = a->nd;
    r->exp = a->exp;
    r->sign = a->sign;
    return 0;
}

static inline int dec_ge(const uint8_t* rem, uint32_t rn, const uint8_t* b, uint32_t bn)
{
    uint32_t k;
    if (rn != bn)
        return rn > bn;
    for (k = bn; k-- > 0;)
        if (rem[k] != b[k])
            return rem[k] > b[k];
    return 1;
}

static inline void db_init(db_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = b->too_long = 0;
}

static inline void db_free(db_t* b)
{
    dyn_dec_free(b->p);
    b->p = NULL;
}

static inline void db_write(db_t* b, const void* p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        uint8_t* np;
        while (nc < b->n + n)
            nc += nc / 2 + 8;
        np = (uint8_t*)dyn_dec_realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static inline void db_put(db_t* b, uint8_t c) { db_write(b, &c, 1); }

static inline int32_t dec_adjusted(const dec_t* x)
{
    return x->nd ? x->exp + (int32_t)x->nd - 1 : 0;
}

static inline void dec_write(db_t* b, const dec_t* x)
{
    int32_t hi, lo, p;

    if (x->nd == 0) {
        db_put(b, '0');
        return;
    }
    if (x->sign)
        db_put(b, '-');
    hi = x->exp + (int32_t)x->nd - 1;
    lo = x->exp;
    if (hi < 0)
        hi = 0;
    if (lo > 0)
        lo = 0;
    if ((int64_t)hi - lo >= DEC_MAX_TEXT) {
        b->too_long = 1;
        return;
    }
    for (p = hi; p >= lo; p--) {
        if (p == -1)
            db_put(b, '.');
        db_put(b, (uint8_t)('0' + dec_at(x, p)));
    }
}

static inline int dec_mod(dec_t* r, const dec_t* a, const dec_t* b)
{
    return dec_int_divmod(NULL, r, a, b);
}

#endif
