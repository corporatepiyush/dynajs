#include <math.h>
#include <stdlib.h>
#include "bson.h"

void dyn_ks_init(dyn_ks_buf* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

void dyn_ks_free(dyn_ks_buf* b)
{
    free(b->p);
    dyn_ks_init(b);
}

__attribute__((noinline)) static int ks_grow(dyn_ks_buf* b, size_t extra)
{
    size_t nc = b->cap ? b->cap : 64;
    uint8_t* np;
    if (b->oom)
        return 0;
    while (nc < b->n + extra)
        nc *= 2;
    np = realloc(b->p, nc);
    if (!np) {
        b->oom = 1;
        return 0;
    }
    b->p = np;
    b->cap = nc;
    return 1;
}

static inline int ks_reserve(dyn_ks_buf* b, size_t extra)
{
    return (b->cap - b->n >= extra && !b->oom) || ks_grow(b, extra);
}

void dyn_ks_raw(dyn_ks_buf* b, const uint8_t* p, size_t n)
{
    if (!ks_reserve(b, n))
        return;
    if (n)
        memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void ks_u8(dyn_ks_buf* b, uint8_t v)
{
    if (!ks_reserve(b, 1))
        return;
    b->p[b->n++] = v;
}

static void ks_be64(dyn_ks_buf* b, uint64_t v)
{
    int i;
    if (!ks_reserve(b, 8))
        return;
    for (i = 7; i >= 0; i--) {
        b->p[b->n + (size_t)i] = (uint8_t)v;
        v >>= 8;
    }
    b->n += 8;
}

static void ks_bytes_escaped(dyn_ks_buf* b, const uint8_t* s, size_t n)
{
    size_t i;
    if (!ks_reserve(b, n + 2))
        return;
    for (i = 0; i < n; i++) {
        if (s[i] == 0) {
            if (!ks_reserve(b, n - i + 3))
                return;
            b->p[b->n++] = 0;
            b->p[b->n++] = 1;
        } else {
            b->p[b->n++] = s[i];
        }
    }
    b->p[b->n++] = 0;
    b->p[b->n++] = 0;
}

static uint64_t ks_f64_bits(double d)
{
    uint64_t u;
    if (isnan(d))
        return 0;
    if (d == 0)
        d = 0;
    memcpy(&u, &d, 8);
    return (u & 0x8000000000000000ull) ? ~u : u | 0x8000000000000000ull;
}

static void ks_number(dyn_ks_buf* b, dyn_bson_val v)
{
    double d;
    int64_t tb = 0;
    if (v.type == DYN_BSON_INT64) {
        int64_t i = (int64_t)dyn_bson_rd64(v.val);
        d = (double)i;
        if (d >= 9223372036854775808.0)
            tb = i - INT64_MAX - 1;
        else
            tb = i - (int64_t)d;
    } else {
        d = dyn_bson_num_f64(v);
    }
    ks_be64(b, ks_f64_bits(d));
    ks_be64(b, (uint64_t)tb ^ 0x8000000000000000ull);
}

static void ks_doc(dyn_ks_buf* b, const uint8_t* doc, int with_keys);

static void ks_value_body(dyn_ks_buf* b, dyn_bson_val v)
{
    switch (dyn_bson_canon_rank(v.type)) {
    case 10:
        ks_number(b, v);
        break;
    case 15:
    case 60:
        ks_bytes_escaped(b, v.val + 4, dyn_bson_rd32(v.val) - 1);
        break;
    case 20:
        ks_doc(b, v.val, 1);
        break;
    case 25:
        ks_doc(b, v.val, 0);
        break;
    case 30: {
        uint32_t l = dyn_bson_rd32(v.val);
        ks_be64(b, l);
        ks_u8(b, v.val[4]);
        dyn_ks_raw(b, v.val + 5, l);
        break;
    }
    case 35:
        dyn_ks_raw(b, v.val, 12);
        break;
    case 40:
        ks_u8(b, v.val[0] ? 1 : 0);
        break;
    case 45:
        ks_be64(b, dyn_bson_rd64(v.val) ^ 0x8000000000000000ull);
        break;
    case 47:
        ks_be64(b, dyn_bson_rd64(v.val));
        break;
    case 50: {
        size_t pl = strlen((const char*)v.val);
        ks_bytes_escaped(b, v.val, pl);
        ks_bytes_escaped(b, v.val + pl + 1, strlen((const char*)v.val + pl + 1));
        break;
    }
    case 65: {
        uint32_t sl = dyn_bson_rd32(v.val + 4);
        ks_bytes_escaped(b, v.val + 8, sl - 1);
        ks_doc(b, v.val + 8 + sl, 1);
        break;
    }
    case 55:
        ks_bytes_escaped(b, v.val, v.vlen);
        break;
    default:
        break;
    }
}

static void ks_doc(dyn_ks_buf* b, const uint8_t* doc, int with_keys)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        ks_u8(b, (uint8_t)(dyn_bson_canon_rank(it.type) + 1));
        if (with_keys)
            ks_bytes_escaped(b, (const uint8_t*)it.key, it.klen);
        ks_value_body(b, dyn_bson_iter_val(&it));
    }
    ks_u8(b, 0);
}

static void ks_invert_from(dyn_ks_buf* b, size_t from)
{
    size_t i;
    if (b->oom)
        return;
    for (i = from; i < b->n; i++)
        b->p[i] = (uint8_t)~b->p[i];
}

void dyn_ks_value(dyn_ks_buf* b, dyn_bson_val v, int desc)
{
    size_t from = b->n;
    if (v.type == DYN_BSON_UNDEFINED)
        v.type = DYN_BSON_NULL;
    ks_u8(b, (uint8_t)(dyn_bson_canon_rank(v.type) + 1));
    ks_value_body(b, v);
    if (desc)
        ks_invert_from(b, from);
}

void dyn_ks_missing(dyn_ks_buf* b, int desc)
{
    ks_u8(b, (uint8_t)(desc ? ~(5 + 1) : (5 + 1)));
}

void dyn_ks_type_floor(dyn_ks_buf* b, int rank, int desc)
{
    ks_u8(b, (uint8_t)(desc ? ~(rank + 1) : (rank + 1)));
}

void dyn_ks_type_ceil(dyn_ks_buf* b, int rank, int desc)
{
    ks_u8(b, (uint8_t)(desc ? ~(rank + 2) : (rank + 2)));
}
