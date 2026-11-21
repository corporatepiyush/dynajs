#include <math.h>
#include "bson.h"
#include "../dyn-cdefs.h"

int dyn_bson_canon_rank(uint8_t type)
{
    switch (type) {
    case DYN_BSON_MINKEY:
        return 0;
    case DYN_BSON_UNDEFINED:
        return 1;
    case DYN_BSON_NULL:
        return 5;
    case DYN_BSON_DOUBLE:
    case DYN_BSON_INT32:
    case DYN_BSON_INT64:
    case DYN_BSON_DEC128:
        return 10;
    case DYN_BSON_STRING:
    case DYN_BSON_SYMBOL:
        return 15;
    case DYN_BSON_DOC:
        return 20;
    case DYN_BSON_ARRAY:
        return 25;
    case DYN_BSON_BINARY:
        return 30;
    case DYN_BSON_OID:
        return 35;
    case DYN_BSON_BOOL:
        return 40;
    case DYN_BSON_DATE:
        return 45;
    case DYN_BSON_TIMESTAMP:
        return 47;
    case DYN_BSON_REGEX:
        return 50;
    case DYN_BSON_DBPOINTER:
        return 55;
    case DYN_BSON_CODE:
        return 60;
    case DYN_BSON_CODE_W_SCOPE:
        return 65;
    case DYN_BSON_MAXKEY:
        return 127;
    }
    return 126;
}

int dyn_bson_is_number(uint8_t type)
{
    return type == DYN_BSON_DOUBLE || type == DYN_BSON_INT32 || type == DYN_BSON_INT64
        || type == DYN_BSON_DEC128;
}

double dyn_bson_num_f64(dyn_bson_val v)
{
    double d;
    switch (v.type) {
    case DYN_BSON_DOUBLE:
        return dyn_bson_rdf64(v.val);
    case DYN_BSON_INT32:
        return (double)(int32_t)dyn_bson_rd32(v.val);
    case DYN_BSON_INT64:
        return (double)(int64_t)dyn_bson_rd64(v.val);
    case DYN_BSON_DEC128:
        if (dyn_bson_dec128_to_f64(v.val, &d) == 0)
            return d;
        return DYN_NAN;
    }
    return DYN_NAN;
}

static int bson_cmp_i64(int64_t a, int64_t b)
{
    return a < b ? -1 : a > b ? 1
                              : 0;
}

static int bson_cmp_f64_i64(double d, int64_t i)
{
    int64_t di;
    if (isnan(d))
        return -1;
    if (d < -9223372036854775808.0)
        return -1;
    if (d >= 9223372036854775808.0)
        return 1;
    di = (int64_t)d;
    if (di != i)
        return di < i ? -1 : 1;
    d -= (double)di;
    return d < 0 ? -1 : d > 0 ? 1
                              : 0;
}

static int bson_cmp_f64(double a, double b)
{
    if (isnan(a))
        return isnan(b) ? 0 : -1;
    if (isnan(b))
        return 1;
    return a < b ? -1 : a > b ? 1
                              : 0;
}

static int bson_is_int(uint8_t t)
{
    return t == DYN_BSON_INT32 || t == DYN_BSON_INT64;
}

static int64_t bson_int_val(dyn_bson_val v)
{
    return v.type == DYN_BSON_INT32 ? (int64_t)(int32_t)dyn_bson_rd32(v.val)
                                    : (int64_t)dyn_bson_rd64(v.val);
}

int dyn_bson_num_cmp(dyn_bson_val a, dyn_bson_val b)
{
    if (bson_is_int(a.type) && bson_is_int(b.type))
        return bson_cmp_i64(bson_int_val(a), bson_int_val(b));
    if (bson_is_int(b.type) && a.type != DYN_BSON_DEC128)
        return bson_cmp_f64_i64(dyn_bson_rdf64(a.val), bson_int_val(b));
    if (bson_is_int(a.type) && b.type != DYN_BSON_DEC128)
        return -bson_cmp_f64_i64(dyn_bson_rdf64(b.val), bson_int_val(a));
    return bson_cmp_f64(dyn_bson_num_f64(a), dyn_bson_num_f64(b));
}

static int bson_cmp_bytes(const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    return an < bn ? -1 : an > bn ? 1
                                  : 0;
}

static int bson_cmp_str(const uint8_t* a, const uint8_t* b)
{
    return bson_cmp_bytes(a + 4, dyn_bson_rd32(a) - 1, b + 4, dyn_bson_rd32(b) - 1);
}

int dyn_bson_doc_cmp(const uint8_t* a, const uint8_t* b)
{
    dyn_bson_iter ia, ib;
    int ra, rb, c;
    dyn_bson_iter_init(&ia, a, dyn_bson_rd32(a));
    dyn_bson_iter_init(&ib, b, dyn_bson_rd32(b));
    for (;;) {
        ra = dyn_bson_iter_next(&ia);
        rb = dyn_bson_iter_next(&ib);
        if (ra <= 0 || rb <= 0)
            return (ra > 0) - (rb > 0);
        c = dyn_bson_canon_rank(ia.type) - dyn_bson_canon_rank(ib.type);
        if (c)
            return c < 0 ? -1 : 1;
        c = bson_cmp_bytes((const uint8_t*)ia.key, ia.klen, (const uint8_t*)ib.key, ib.klen);
        if (c)
            return c;
        c = dyn_bson_cmp(dyn_bson_iter_val(&ia), dyn_bson_iter_val(&ib));
        if (c)
            return c;
    }
}

int dyn_bson_cmp(dyn_bson_val a, dyn_bson_val b)
{
    int ra = dyn_bson_canon_rank(a.type), rb = dyn_bson_canon_rank(b.type), c;
    if (ra != rb)
        return ra < rb ? -1 : 1;
    switch (ra) {
    case 10:
        return dyn_bson_num_cmp(a, b);
    case 15:
    case 60:
        return bson_cmp_str(a.val, b.val);
    case 20:
    case 25:
        return dyn_bson_doc_cmp(a.val, b.val);
    case 30: {
        uint32_t la = dyn_bson_rd32(a.val), lb = dyn_bson_rd32(b.val);
        if (la != lb)
            return la < lb ? -1 : 1;
        if (a.val[4] != b.val[4])
            return a.val[4] < b.val[4] ? -1 : 1;
        return bson_cmp_bytes(a.val + 5, la, b.val + 5, lb);
    }
    case 35:
        return bson_cmp_bytes(a.val, 12, b.val, 12);
    case 40:
        return (a.val[0] != 0) - (b.val[0] != 0);
    case 45:
        return bson_cmp_i64((int64_t)dyn_bson_rd64(a.val), (int64_t)dyn_bson_rd64(b.val));
    case 47: {
        uint64_t x = dyn_bson_rd64(a.val), y = dyn_bson_rd64(b.val);
        return x < y ? -1 : x > y ? 1
                                  : 0;
    }
    case 50: {
        size_t pa = strlen((const char*)a.val), pb = strlen((const char*)b.val);
        c = bson_cmp_bytes(a.val, pa, b.val, pb);
        if (c)
            return c;
        return bson_cmp_bytes(a.val + pa + 1, strlen((const char*)a.val + pa + 1), b.val + pb + 1,
            strlen((const char*)b.val + pb + 1));
    }
    case 65: {
        uint32_t sa = dyn_bson_rd32(a.val + 4), sb = dyn_bson_rd32(b.val + 4);
        c = bson_cmp_str(a.val + 4, b.val + 4);
        if (c)
            return c;
        return dyn_bson_doc_cmp(a.val + 8 + sa, b.val + 8 + sb);
    }
    case 55:
        return bson_cmp_bytes(a.val, a.vlen, b.val, b.vlen);
    }
    return 0;
}

int dyn_bson_equal(dyn_bson_val a, dyn_bson_val b)
{
    return dyn_bson_cmp(a, b) == 0;
}
