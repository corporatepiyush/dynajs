#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-roaring-bitmap.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#define DS_R4(x) x, x, x, x

#define DS_ROAR_GALLOP 6
static const uint64_t dyn_roar_ones[DYN_ROAR_WORDS] = { DS_R4(DS_R4(DS_R4(DS_R4(DS_R4(~0ULL))))) };

#undef DS_R4

static const uint64_t* dyn_roar_words(const dyn_roar_ct* c)
{
    return c->type == DYN_ROAR_FULL ? dyn_roar_ones : (const uint64_t*)c->data;
}

static void dyn_roar_ct_free(dyn_roar_ct* c)
{
    if (c->type != DYN_ROAR_FULL)
        DYN_DS_FREE(c->data);
    c->data = NULL;
    c->type = 0;
    c->card = 0;
}

void dyn_roar_clear(dyn_roar_t* r)
{
    uint32_t i;
    for (i = 0; i < r->n; i++)
        dyn_roar_ct_free(&r->c[i]);
    DYN_DS_FREE(r->c);
    r->c = NULL;
    r->n = r->cap = 0;
}

static int dyn_roar_find(const dyn_roar_t* r, uint16_t key, uint32_t* pos)
{
    uint32_t lo = 0, hi = r->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (r->c[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos = lo;
    return lo < r->n && r->c[lo].key == key;
}

int dyn_roar_reserve(dyn_roar_t* r, uint32_t need)
{
    uint32_t ncap;
    dyn_roar_ct* nc;
    if (need <= r->cap)
        return 0;
    ncap = r->cap ? r->cap * 2 : 4;
    while (ncap < need)
        ncap *= 2;
    nc = (dyn_roar_ct*)DYN_DS_REALLOC(r->c, sizeof(dyn_roar_ct) * ncap);
    if (!nc)
        return -1;
    r->c = nc;
    r->cap = ncap;
    return 0;
}

static dyn_roar_ct* dyn_roar_insert_at(dyn_roar_t* r, uint32_t pos, uint16_t key)
{
    if (dyn_roar_reserve(r, r->n + 1) < 0)
        return NULL;
    memmove(&r->c[pos + 1], &r->c[pos], sizeof(dyn_roar_ct) * (r->n - pos));
    r->n++;
    memset(&r->c[pos], 0, sizeof(dyn_roar_ct));
    r->c[pos].key = key;
    return &r->c[pos];
}

static void dyn_roar_remove_at(dyn_roar_t* r, uint32_t pos)
{
    dyn_roar_ct_free(&r->c[pos]);
    memmove(&r->c[pos], &r->c[pos + 1], sizeof(dyn_roar_ct) * (r->n - pos - 1));
    r->n--;
}

uint32_t dyn_roar_popcnt(const uint64_t* w)
{
    uint32_t i, c = 0;
    for (i = 0; i < DYN_ROAR_WORDS; i++)
        c += (uint32_t)__builtin_popcountll(w[i]);
    return c;
}

static void dyn_roar_arr_to_words(const dyn_roar_ct* c, uint64_t* w)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t i;
    memset(w, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
    for (i = 0; i < c->card; i++)
        w[a[i] >> 6] |= 1ULL << (a[i] & 63);
}

static const uint64_t* dyn_roar_words_of(const dyn_roar_ct* c, uint64_t* tmp)
{
    if (!c) {
        memset(tmp, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
        return tmp;
    }
    if (c->type == DYN_ROAR_ARR) {
        dyn_roar_arr_to_words(c, tmp);
        return tmp;
    }
    return dyn_roar_words(c);
}

static int dyn_roar_adopt_words(dyn_roar_ct* out, uint64_t* w, uint32_t card)
{
    out->card = card;
    if (card == 0) {
        DYN_DS_FREE(w);
        out->type = 0;
        out->data = NULL;
        return 0;
    }
    if (card == 65536) {
        DYN_DS_FREE(w);
        out->type = DYN_ROAR_FULL;
        out->data = NULL;
        return 0;
    }
    if (card <= DYN_ROAR_ARR_MAX) {
        uint16_t* a = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * card);
        uint32_t i, k = 0;
        if (!a) {
            DYN_DS_FREE(w);
            return -1;
        }
        for (i = 0; i < DYN_ROAR_WORDS; i++) {
            uint64_t x = w[i];
            while (x) {
                a[k++] = (uint16_t)(i * 64 + __builtin_ctzll(x));
                x &= x - 1;
            }
        }
        DYN_DS_FREE(w);
        out->type = DYN_ROAR_ARR;
        out->data = a;
        out->cap = card;
        return 0;
    }
    out->type = DYN_ROAR_BMP;
    out->data = w;
    out->cap = 0;
    return 0;
}

static int dyn_roar_ct_copy(dyn_roar_ct* out, const dyn_roar_ct* src)
{
    out->key = src->key;
    out->type = src->type;
    out->card = src->card;
    out->cap = 0;
    out->data = NULL;
    if (src->type == DYN_ROAR_ARR) {
        out->data = DYN_DS_MALLOC(sizeof(uint16_t) * src->card);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint16_t) * src->card);
        out->cap = src->card;
    } else if (src->type == DYN_ROAR_BMP) {
        out->data = DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint64_t) * DYN_ROAR_WORDS);
    }
    return 0;
}

static int dyn_roar_arr_has(const dyn_roar_ct* c, uint16_t lo, uint32_t* pos)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t l = 0, h = c->card;
    while (l < h) {
        uint32_t m = (l + h) >> 1;
        if (a[m] < lo)
            l = m + 1;
        else
            h = m;
    }
    if (pos)
        *pos = l;
    return l < c->card && a[l] == lo;
}

static int dyn_roar_ct_has(const dyn_roar_ct* c, uint16_t lo)
{
    if (c->type == DYN_ROAR_ARR)
        return dyn_roar_arr_has(c, lo, NULL);
    return (dyn_roar_words(c)[lo >> 6] >> (lo & 63)) & 1;
}

int dyn_roar_add(dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos;
    dyn_roar_ct* c;
    if (!dyn_roar_find(r, hi, &pos)) {
        c = dyn_roar_insert_at(r, pos, hi);
        if (!c)
            return -1;
        c->data = DYN_DS_MALLOC(sizeof(uint16_t) * 4);
        if (!c->data) {
            memmove(&r->c[pos], &r->c[pos + 1], sizeof(dyn_roar_ct) * (r->n - pos - 1));
            r->n--;
            return -1;
        }
        c->type = DYN_ROAR_ARR;
        c->cap = 4;
        c->card = 1;
        ((uint16_t*)c->data)[0] = lo;
        return 1;
    }
    c = &r->c[pos];
    if (c->type == DYN_ROAR_FULL)
        return 0;
    if (c->type == DYN_ROAR_BMP) {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (w[lo >> 6] & b)
            return 0;
        w[lo >> 6] |= b;
        if (++c->card == 65536) {
            DYN_DS_FREE(c->data);
            c->data = NULL;
            c->type = DYN_ROAR_FULL;
        }
        return 1;
    }
    {
        uint16_t* a = (uint16_t*)c->data;
        uint32_t p;
        if (c->card && a[c->card - 1] < lo)
            p = c->card;
        else if (dyn_roar_arr_has(c, lo, &p))
            return 0;
        if (c->card == DYN_ROAR_ARR_MAX) {
            uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
            if (!w)
                return -1;
            dyn_roar_arr_to_words(c, w);
            w[lo >> 6] |= 1ULL << (lo & 63);
            DYN_DS_FREE(c->data);
            c->data = w;
            c->type = DYN_ROAR_BMP;
            c->card++;
            return 1;
        }
        if (c->card == c->cap) {
            uint32_t ncap = c->cap * 2 > DYN_ROAR_ARR_MAX ? DYN_ROAR_ARR_MAX : c->cap * 2;
            uint16_t* na = (uint16_t*)DYN_DS_REALLOC(a, sizeof(uint16_t) * ncap);
            if (!na)
                return -1;
            c->data = a = na;
            c->cap = ncap;
        }
        memmove(&a[p + 1], &a[p], sizeof(uint16_t) * (c->card - p));
        a[p] = lo;
        c->card++;
        return 1;
    }
}

int dyn_roar_remove(dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos, p;
    dyn_roar_ct* c;
    if (!dyn_roar_find(r, hi, &pos))
        return 0;
    c = &r->c[pos];
    if (c->type == DYN_ROAR_ARR) {
        uint16_t* a = (uint16_t*)c->data;
        if (!dyn_roar_arr_has(c, lo, &p))
            return 0;
        memmove(&a[p], &a[p + 1], sizeof(uint16_t) * (c->card - p - 1));
        if (--c->card == 0)
            dyn_roar_remove_at(r, pos);
        return 1;
    }
    if (c->type == DYN_ROAR_FULL) {
        uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        memcpy(w, dyn_roar_ones, sizeof(dyn_roar_ones));
        c->data = w;
        c->type = DYN_ROAR_BMP;
    }
    {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (!(w[lo >> 6] & b))
            return 0;
        w[lo >> 6] &= ~b;
        c->card--;
        if (c->card <= DYN_ROAR_ARR_MAX) {
            dyn_roar_ct t = *c;
            c->data = NULL;
            if (dyn_roar_adopt_words(&t, w, c->card) < 0) {
                c->data = NULL;
                dyn_roar_remove_at(r, pos);
                return -1;
            }
            *c = t;
        }
        return 1;
    }
}

int dyn_roar_has(const dyn_roar_t* r, uint32_t x)
{
    uint32_t pos;
    if (!dyn_roar_find(r, (uint16_t)(x >> 16), &pos))
        return 0;
    return dyn_roar_ct_has(&r->c[pos], (uint16_t)x);
}

int dyn_roar_range(dyn_roar_t* r, uint64_t lo, uint64_t hi, int add)
{
    uint64_t k;
    if (lo >= hi)
        return 0;
    for (k = lo >> 16; k <= (hi - 1) >> 16; k++) {
        uint32_t a = k == (lo >> 16) ? (uint32_t)(lo & 0xFFFF) : 0;
        uint32_t b = k == ((hi - 1) >> 16) ? (uint32_t)((hi - 1) & 0xFFFF) : 0xFFFF;
        uint32_t pos;
        int found = dyn_roar_find(r, (uint16_t)k, &pos);
        dyn_roar_ct* c;
        uint64_t* w;
        uint32_t i;
        if (a == 0 && b == 0xFFFF) {
            if (!add) {
                if (found)
                    dyn_roar_remove_at(r, pos);
                continue;
            }
            if (!found && !dyn_roar_insert_at(r, pos, (uint16_t)k))
                return -1;
            dyn_roar_ct_free(&r->c[pos]);
            r->c[pos].type = DYN_ROAR_FULL;
            r->c[pos].card = 65536;
            continue;
        }
        if (!found && !add)
            continue;
        w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        if (found) {
            const uint64_t* src = dyn_roar_words_of(&r->c[pos], w);
            if (src != w)
                memcpy(w, src, sizeof(uint64_t) * DYN_ROAR_WORDS);
        } else {
            memset(w, 0, sizeof(uint64_t) * DYN_ROAR_WORDS);
        }
        for (i = a; i <= b;) {
            if ((i & 63) == 0 && i + 63 <= b) {
                w[i >> 6] = add ? ~0ULL : 0;
                i += 64;
            } else {
                if (add)
                    w[i >> 6] |= 1ULL << (i & 63);
                else
                    w[i >> 6] &= ~(1ULL << (i & 63));
                i++;
            }
        }
        if (!found) {
            c = dyn_roar_insert_at(r, pos, (uint16_t)k);
            if (!c) {
                DYN_DS_FREE(w);
                return -1;
            }
        } else {
            c = &r->c[pos];
            dyn_roar_ct_free(c);
        }
        if (dyn_roar_adopt_words(c, w, dyn_roar_popcnt(w)) < 0) {
            dyn_roar_remove_at(r, pos);
            return -1;
        }
        if (c->card == 0)
            dyn_roar_remove_at(r, pos);
    }
    return 0;
}

uint64_t dyn_roar_card(const dyn_roar_t* r)
{
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n; i++)
        s += r->c[i].card;
    return s;
}

static int dyn_roar_ct_words_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp);

static uint32_t ds_roar_gallop(const uint16_t* y, uint32_t q, uint32_t nb, uint16_t v)
{
    uint32_t step = 1, lo = q, hi;
    while (q + step < nb && y[q + step] < v) {
        lo = q + step;
        step <<= 1;
    }
    hi = q + step < nb ? q + step : nb;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (y[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static uint32_t ds_roar_gallop_and(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o, int keep_hits)
{
    uint32_t p, q = 0, card = 0;
    for (p = 0; p < na; p++) {
        int hit;
        q = q < nb && y[q] < x[p] ? ds_roar_gallop(y, q, nb, x[p]) : q;
        hit = q < nb && y[q] == x[p];
        if (hit == keep_hits) {
            if (o)
                o[card] = x[p];
            card++;
        }
    }
    return card;
}

#if defined(__aarch64__) && defined(__ARM_NEON)
#define DS_ROAR_SIMD 1
static inline uint32_t ds_roar_blk_mask(const uint16_t* x, const uint16_t* y)
{
    static const uint16_t bits[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    uint16x8_t a = vld1q_u16(x), b = vld1q_u16(y), m = vceqq_u16(a, b);
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 1)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 2)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 3)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 4)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 5)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 6)));
    m = vorrq_u16(m, vceqq_u16(a, vextq_u16(b, b, 7)));
    return vaddvq_u16(vandq_u16(m, vld1q_u16(bits)));
}
#elif defined(__SSE2__)
#define DS_ROAR_SIMD 1
static inline uint32_t ds_roar_blk_mask(const uint16_t* x, const uint16_t* y)
{
    __m128i a = _mm_loadu_si128((const __m128i*)x), b = _mm_loadu_si128((const __m128i*)y);
    __m128i m = _mm_cmpeq_epi16(a, b);
    int k;
    for (k = 1; k < 8; k++) {
        b = _mm_or_si128(_mm_srli_si128(b, 2), _mm_slli_si128(b, 14));
        m = _mm_or_si128(m, _mm_cmpeq_epi16(a, b));
    }
    return (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(m, _mm_setzero_si128()));
}
#endif

static inline int ds_roar_and_gallops(uint32_t small, uint32_t big)
{
    if ((uint64_t)small * DS_ROAR_GALLOP >= big)
        return 0;
#ifdef DS_ROAR_SIMD
    if (small >= 128 && (uint64_t)small * 16 >= big)
        return 0;
#endif
    return 1;
}

static uint32_t ds_roar_merge_and(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o)
{
    uint32_t p = 0, q = 0, card = 0;
    if (na <= nb && ds_roar_and_gallops(na, nb))
        return ds_roar_gallop_and(x, na, y, nb, o, 1);
    if (nb < na && ds_roar_and_gallops(nb, na))
        return ds_roar_gallop_and(y, nb, x, na, o, 1);
#ifdef DS_ROAR_SIMD
    while (p + 8 <= na && q + 8 <= nb) {
        uint32_t m = ds_roar_blk_mask(x + p, y + q);
        uint16_t xm = x[p + 7], ym = y[q + 7];
        if (m) {
            if (o) {
                uint16_t t[8];
                uint32_t k, c = 0;
                for (k = 0; k < 8; k++) {
                    t[c] = x[p + k];
                    c += (m >> k) & 1;
                }
                memcpy(o + card, t, c * sizeof *t);
                card += c;
            } else {
                card += (uint32_t)__builtin_popcount(m);
            }
        }
        p += xm <= ym ? 8 : 0;
        q += ym <= xm ? 8 : 0;
    }
#endif
    if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)xv;
            card += xv == yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv == yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    return card;
}

static uint32_t ds_roar_merge_andnot(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o)
{
    uint32_t p = 0, q = 0, card = 0;
    if ((uint64_t)na * DS_ROAR_GALLOP < nb)
        return ds_roar_gallop_and(x, na, y, nb, o, 0);
    if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)xv;
            card += xv < yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv < yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    if (o && p < na)
        memcpy(o + card, x + p, sizeof(uint16_t) * (na - p));
    return card + (na - p);
}

static uint32_t ds_roar_merge_or(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, uint16_t* o, int xor_)
{
    uint32_t p = 0, q = 0, card = 0;
    if (o && xor_)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)(xv < yv ? xv : yv);
            card += xv != yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else if (o)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            o[card] = (uint16_t)(xv < yv ? xv : yv);
            card++;
            p += xv <= yv;
            q += yv <= xv;
        }
    else if (xor_)
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card += xv != yv;
            p += xv <= yv;
            q += yv <= xv;
        }
    else
        while (p < na && q < nb) {
            uint32_t xv = x[p], yv = y[q];
            card++;
            p += xv <= yv;
            q += yv <= xv;
        }
    if (o && p < na)
        memcpy(o + card, x + p, sizeof(uint16_t) * (na - p));
    if (o && q < nb)
        memcpy(o + card, y + q, sizeof(uint16_t) * (nb - q));
    return card + (na - p) + (nb - q);
}

static uint32_t ds_roar_merge(const uint16_t* x, uint32_t na, const uint16_t* y, uint32_t nb, int op, uint16_t* o)
{
    if (op == DYN_ROAR_AND)
        return ds_roar_merge_and(x, na, y, nb, o);
    if (op == DYN_ROAR_ANDNOT)
        return ds_roar_merge_andnot(x, na, y, nb, o);
    return ds_roar_merge_or(x, na, y, nb, o, op == DYN_ROAR_XOR);
}

static int dyn_roar_ct_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp)
{
    uint32_t i, card = 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->key = a ? a->key : b->key;
    }
    if (!a || !b) {
        const dyn_roar_ct* s = NULL;
        if (op == DYN_ROAR_AND || (op == DYN_ROAR_ANDNOT && !a))
            s = NULL;
        else
            s = a ? a : b;
        if (!s) {
            *cardp = 0;
            return 0;
        }
        *cardp = s->card;
        return out ? dyn_roar_ct_copy(out, s) : 0;
    }
    if (a->type == DYN_ROAR_ARR && b->type == DYN_ROAR_ARR) {
        const uint16_t *x = (const uint16_t*)a->data, *y = (const uint16_t*)b->data;
        uint32_t cap = op == DYN_ROAR_AND ? (a->card < b->card ? a->card : b->card) : op == DYN_ROAR_ANDNOT ? a->card
                                                                                                            : a->card + b->card;
        uint16_t* o = NULL;
        if (out && cap) {
            o = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * cap);
            if (!o)
                return -1;
        }
        card = ds_roar_merge(x, a->card, y, b->card, op, o);
        *cardp = card;
        if (!out)
            return 0;
        if (card == 0) {
            DYN_DS_FREE(o);
            return 0;
        }
        if (card <= DYN_ROAR_ARR_MAX) {
            out->type = DYN_ROAR_ARR;
            out->data = o;
            out->cap = cap;
            out->card = card;
            return 0;
        }
        {
            uint64_t* w = (uint64_t*)DYN_DS_CALLOC(DYN_ROAR_WORDS, sizeof(uint64_t));
            if (!w) {
                DYN_DS_FREE(o);
                return -1;
            }
            for (i = 0; i < card; i++)
                w[o[i] >> 6] |= 1ULL << (o[i] & 63);
            DYN_DS_FREE(o);
            return dyn_roar_adopt_words(out, w, card);
        }
    }
    if ((op == DYN_ROAR_AND || op == DYN_ROAR_ANDNOT) && a->type == DYN_ROAR_ARR) {
        const uint16_t* x = (const uint16_t*)a->data;
        const uint64_t* yw = dyn_roar_words(b);
        uint16_t* o = NULL;
        if (out) {
            o = (uint16_t*)DYN_DS_MALLOC(sizeof(uint16_t) * a->card);
            if (!o)
                return -1;
        }
        {
            uint32_t want = op == DYN_ROAR_AND;
            if (o)
                for (i = 0; i < a->card; i++) {
                    o[card] = x[i];
                    card += (uint32_t)((yw[x[i] >> 6] >> (x[i] & 63)) & 1) == want;
                }
            else
                for (i = 0; i < a->card; i++)
                    card += (uint32_t)((yw[x[i] >> 6] >> (x[i] & 63)) & 1) == want;
        }
        *cardp = card;
        if (!out)
            return 0;
        if (!card) {
            DYN_DS_FREE(o);
            return 0;
        }
        out->type = DYN_ROAR_ARR;
        out->data = o;
        out->cap = a->card;
        out->card = card;
        return 0;
    }
    if (op == DYN_ROAR_AND && b->type == DYN_ROAR_ARR)
        return dyn_roar_ct_op(b, a, op, out, cardp);
    return dyn_roar_ct_words_op(a, b, op, out, cardp);
}

#define DS_ROAR_WFN(NAME, EXPR)                                                                                 \
    static uint32_t NAME##_card(const uint64_t* restrict wa, const uint64_t* restrict wb)                       \
    {                                                                                                           \
        uint32_t i, card = 0;                                                                                   \
        for (i = 0; i < DYN_ROAR_WORDS; i++)                                                                    \
            card += (uint32_t)__builtin_popcountll(EXPR);                                                       \
        return card;                                                                                            \
    }                                                                                                           \
    static uint32_t NAME##_into(const uint64_t* restrict wa, const uint64_t* restrict wb, uint64_t* restrict w) \
    {                                                                                                           \
        uint32_t i, card = 0;                                                                                   \
        for (i = 0; i < DYN_ROAR_WORDS; i++) {                                                                  \
            uint64_t v = EXPR;                                                                                  \
            w[i] = v;                                                                                           \
            card += (uint32_t)__builtin_popcountll(v);                                                          \
        }                                                                                                       \
        return card;                                                                                            \
    }
DS_ROAR_WFN(ds_roar_w_and, wa[i] & wb[i])
DS_ROAR_WFN(ds_roar_w_or, wa[i] | wb[i])
DS_ROAR_WFN(ds_roar_w_xor, wa[i] ^ wb[i])
DS_ROAR_WFN(ds_roar_w_andnot, wa[i] & ~wb[i])
#undef DS_ROAR_WFN

static int dyn_roar_ct_words_op(const dyn_roar_ct* a, const dyn_roar_ct* b, int op, dyn_roar_ct* out, uint64_t* cardp)
{
    uint64_t ta[DYN_ROAR_WORDS] DS_SCRATCH, tb[DYN_ROAR_WORDS] DS_SCRATCH;
    const uint64_t *wa, *wb;
    uint32_t card;
    wa = dyn_roar_words_of(a, ta);
    wb = dyn_roar_words_of(b, tb);
    if (!out) {
        card = op == DYN_ROAR_AND ? ds_roar_w_and_card(wa, wb)
            : op == DYN_ROAR_OR   ? ds_roar_w_or_card(wa, wb)
            : op == DYN_ROAR_XOR  ? ds_roar_w_xor_card(wa, wb)
                                  : ds_roar_w_andnot_card(wa, wb);
        *cardp = card;
        return 0;
    }
    {
        uint64_t* w = (uint64_t*)DYN_DS_MALLOC(sizeof(uint64_t) * DYN_ROAR_WORDS);
        if (!w)
            return -1;
        card = op == DYN_ROAR_AND ? ds_roar_w_and_into(wa, wb, w)
            : op == DYN_ROAR_OR   ? ds_roar_w_or_into(wa, wb, w)
            : op == DYN_ROAR_XOR  ? ds_roar_w_xor_into(wa, wb, w)
                                  : ds_roar_w_andnot_into(wa, wb, w);
        *cardp = card;
        return dyn_roar_adopt_words(out, w, card);
    }
}

int dyn_roar_op(dyn_roar_t* a, const dyn_roar_t* b, int op)
{
    dyn_roar_t nr = { NULL, 0, 0 };
    uint32_t i = 0, j = 0;
    if (a == b) {
        if (op == DYN_ROAR_XOR || op == DYN_ROAR_ANDNOT)
            dyn_roar_clear(a);
        return 0;
    }
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        dyn_roar_ct out;
        uint64_t card;
        if (op == DYN_ROAR_AND && (i >= a->n || j >= b->n))
            break;
        if (op == DYN_ROAR_ANDNOT && i >= a->n)
            break;
        if (ka < kb && op != DYN_ROAR_AND) {
            if (dyn_roar_reserve(&nr, nr.n + 1) < 0)
                goto oom;
            nr.c[nr.n++] = a->c[i];
            a->c[i].data = NULL;
            a->c[i].type = 0;
            i++;
            continue;
        }
        if (ka < kb) {
            i++;
            continue;
        }
        if (kb < ka && (op == DYN_ROAR_AND || op == DYN_ROAR_ANDNOT)) {
            j++;
            continue;
        }
        if (dyn_roar_ct_op(ka == kb ? &a->c[i] : NULL, &b->c[j], op, &out, &card) < 0)
            goto oom;
        if (ka == kb)
            i++;
        j++;
        if (card) {
            if (dyn_roar_reserve(&nr, nr.n + 1) < 0) {
                dyn_roar_ct_free(&out);
                goto oom;
            }
            nr.c[nr.n++] = out;
        }
    }
    dyn_roar_clear(a);
    *a = nr;
    return 0;
oom:
    dyn_roar_clear(&nr);
    return -1;
}

static void dyn_roar_or_into(uint64_t* w, const dyn_roar_ct* c)
{
    uint32_t i;
    if (c->type == DYN_ROAR_ARR) {
        const uint16_t* a = (const uint16_t*)c->data;
        for (i = 0; i < c->card; i++)
            w[a[i] >> 6] |= 1ULL << (a[i] & 63);
    } else {
        const uint64_t* x = dyn_roar_words(c);
        for (i = 0; i < DYN_ROAR_WORDS; i++)
            w[i] |= x[i];
    }
}

int dyn_roar_or_many(dyn_roar_t* out, const dyn_roar_t* const* in, uint32_t n)
{
    dyn_roar_t nr = { NULL, 0, 0 };
    uint32_t* cur = (uint32_t*)DYN_DS_CALLOC(n ? n : 1, sizeof(uint32_t));
    uint32_t i, io = 0;
    if (!cur)
        return -1;
    for (;;) {
        uint32_t key = io < out->n ? out->c[io].key : 0x10000u, card;
        uint64_t* w;
        dyn_roar_ct ct;
        for (i = 0; i < n; i++)
            if (cur[i] < in[i]->n && in[i]->c[cur[i]].key < key)
                key = in[i]->c[cur[i]].key;
        if (key == 0x10000u)
            break;
        w = (uint64_t*)DYN_DS_CALLOC(DYN_ROAR_WORDS, sizeof(uint64_t));
        if (!w)
            goto oom;
        if (io < out->n && out->c[io].key == key)
            dyn_roar_or_into(w, &out->c[io++]);
        for (i = 0; i < n; i++)
            if (cur[i] < in[i]->n && in[i]->c[cur[i]].key == key)
                dyn_roar_or_into(w, &in[i]->c[cur[i]++]);
        card = dyn_roar_popcnt(w);
        memset(&ct, 0, sizeof ct);
        ct.key = (uint16_t)key;
        if (dyn_roar_adopt_words(&ct, w, card) < 0)
            goto oom;
        if (!card) {
            dyn_roar_ct_free(&ct);
            continue;
        }
        if (dyn_roar_reserve(&nr, nr.n + 1) < 0) {
            dyn_roar_ct_free(&ct);
            goto oom;
        }
        nr.c[nr.n++] = ct;
    }
    DYN_DS_FREE(cur);
    dyn_roar_clear(out);
    *out = nr;
    return 0;
oom:
    DYN_DS_FREE(cur);
    dyn_roar_clear(&nr);
    return -1;
}

uint64_t dyn_roar_op_card(const dyn_roar_t* a, const dyn_roar_t* b, int op)
{
    uint32_t i = 0, j = 0;
    uint64_t s = 0;
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        uint64_t card = 0;
        if (ka == kb) {
            dyn_roar_ct_op(&a->c[i], &b->c[j], op, NULL, &card);
            i++;
            j++;
        } else if (ka < kb) {
            dyn_roar_ct_op(&a->c[i], NULL, op, NULL, &card);
            i++;
        } else {
            dyn_roar_ct_op(NULL, &b->c[j], op, NULL, &card);
            j++;
        }
        s += card;
    }
    return s;
}

uint64_t dyn_roar_rank(const dyn_roar_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n && r->c[i].key < hi; i++)
        s += r->c[i].card;
    if (i < r->n && r->c[i].key == hi) {
        const dyn_roar_ct* c = &r->c[i];
        if (c->type == DYN_ROAR_FULL)
            s += (uint32_t)lo + 1;
        else if (c->type == DYN_ROAR_ARR) {
            uint32_t p;
            s += dyn_roar_arr_has(c, lo, &p) ? p + 1 : p;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t k;
            for (k = 0; k < (uint32_t)(lo >> 6); k++)
                s += (uint64_t)__builtin_popcountll(w[k]);
            s += (uint64_t)__builtin_popcountll(w[lo >> 6] & (((lo & 63) == 63) ? ~0ULL : ((2ULL << (lo & 63)) - 1)));
        }
    }
    return s;
}

int dyn_roar_select(const dyn_roar_t* r, uint64_t i, uint32_t* out)
{
    uint32_t k;
    for (k = 0; k < r->n; k++) {
        const dyn_roar_ct* c = &r->c[k];
        if (i >= c->card) {
            i -= c->card;
            continue;
        }
        if (c->type == DYN_ROAR_FULL)
            *out = ((uint32_t)c->key << 16) | (uint32_t)i;
        else if (c->type == DYN_ROAR_ARR)
            *out = ((uint32_t)c->key << 16) | ((const uint16_t*)c->data)[i];
        else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < DYN_ROAR_WORDS; j++) {
                uint32_t pc = (uint32_t)__builtin_popcountll(w[j]);
                if (i < pc) {
                    uint64_t x = w[j];
                    while (i--)
                        x &= x - 1;
                    *out = ((uint32_t)c->key << 16) | (j * 64 + (uint32_t)__builtin_ctzll(x));
                    return 1;
                }
                i -= pc;
            }
            return 0;
        }
        return 1;
    }
    return 0;
}

void dyn_roar_fill(const dyn_roar_t* r, uint32_t* out)
{
    uint32_t k, n = 0;
    for (k = 0; k < r->n; k++) {
        const dyn_roar_ct* c = &r->c[k];
        uint32_t base = (uint32_t)c->key << 16, i;
        if (c->type == DYN_ROAR_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            for (i = 0; i < c->card; i++)
                out[n++] = base | a[i];
        } else if (c->type == DYN_ROAR_FULL) {
            for (i = 0; i < 65536; i++)
                out[n++] = base | i;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            for (i = 0; i < DYN_ROAR_WORDS; i++) {
                uint64_t x = w[i];
                while (x) {
                    out[n++] = base | (i * 64 + (uint32_t)__builtin_ctzll(x));
                    x &= x - 1;
                }
            }
        }
    }
}

int dyn_roar_clone(dyn_roar_t* dst, const dyn_roar_t* src)
{
    uint32_t i;
    dst->c = NULL;
    dst->n = dst->cap = 0;
    if (dyn_roar_reserve(dst, src->n) < 0)
        return -1;
    for (i = 0; i < src->n; i++) {
        if (dyn_roar_ct_copy(&dst->c[i], &src->c[i]) < 0) {
            dst->n = i;
            dyn_roar_clear(dst);
            return -1;
        }
        dst->n = i + 1;
    }
    return 0;
}

int dyn_roar_u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
