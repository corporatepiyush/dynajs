#include <stdlib.h>
#include "bson.h"

void dyn_bson_w_init(dyn_bson_w* w)
{
    w->p = NULL;
    w->n = w->cap = 0;
    w->oom = 0;
    w->depth = 0;
}

void dyn_bson_w_free(dyn_bson_w* w)
{
    free(w->p);
    dyn_bson_w_init(w);
}

void dyn_bson_w_reset(dyn_bson_w* w)
{
    w->n = 0;
    w->oom = 0;
    w->depth = 0;
}

__attribute__((noinline)) static int bson_w_grow(dyn_bson_w* w, size_t extra)
{
    size_t need, nc;
    uint8_t* np;
    if (w->oom)
        return 0;
    if (extra > DYN_BSON_MAX_SIZE || w->n > DYN_BSON_MAX_SIZE - extra) {
        w->oom = 1;
        return 0;
    }
    need = w->n + extra;
    nc = w->cap ? w->cap : 256;
    while (nc < need)
        nc = nc < (1u << 20) ? nc * 2 : nc + nc / 2;
    np = realloc(w->p, nc);
    if (!np) {
        w->oom = 1;
        return 0;
    }
    w->p = np;
    w->cap = nc;
    return 1;
}

int dyn_bson_w_reserve(dyn_bson_w* w, size_t extra)
{
    if (w->cap - w->n >= extra && !w->oom)
        return 1;
    return bson_w_grow(w, extra);
}

void dyn_bson_w_bytes(dyn_bson_w* w, const void* p, size_t n)
{
    if (!dyn_bson_w_reserve(w, n))
        return;
    if (n)
        memcpy(w->p + w->n, p, n);
    w->n += n;
}

static void bson_w_u8(dyn_bson_w* w, uint8_t b)
{
    if (!dyn_bson_w_reserve(w, 1))
        return;
    w->p[w->n++] = b;
}

static void bson_w_u32(dyn_bson_w* w, uint32_t v)
{
    if (!dyn_bson_w_reserve(w, 4))
        return;
    dyn_bson_wr32(w->p + w->n, v);
    w->n += 4;
}

static void bson_w_u64(dyn_bson_w* w, uint64_t v)
{
    if (!dyn_bson_w_reserve(w, 8))
        return;
    dyn_bson_wr64(w->p + w->n, v);
    w->n += 8;
}

void dyn_bson_w_doc_begin(dyn_bson_w* w)
{
    if (w->depth > DYN_BSON_MAX_DEPTH) {
        w->oom = 1;
        return;
    }
    w->stack[w->depth++] = (uint32_t)w->n;
    bson_w_u32(w, 0);
}

void dyn_bson_w_doc_end(dyn_bson_w* w)
{
    uint32_t at;
    bson_w_u8(w, 0);
    if (w->oom || w->depth <= 0)
        return;
    at = w->stack[--w->depth];
    dyn_bson_wr32(w->p + at, (uint32_t)(w->n - at));
}

void dyn_bson_w_key(dyn_bson_w* w, uint8_t type, const char* key, size_t klen)
{
    if (!dyn_bson_w_reserve(w, klen + 2))
        return;
    w->p[w->n++] = type;
    if (klen)
        memcpy(w->p + w->n, key, klen);
    w->n += klen;
    w->p[w->n++] = 0;
}

size_t dyn_bson_u32_key(char out[12], uint32_t idx)
{
    char rev[12];
    size_t n = 0, i;
    if (idx < 10) {
        out[0] = (char)('0' + idx);
        out[1] = 0;
        return 1;
    }
    do {
        rev[n++] = (char)('0' + idx % 10);
        idx /= 10;
    } while (idx);
    for (i = 0; i < n; i++)
        out[i] = rev[n - 1 - i];
    out[n] = 0;
    return n;
}

void dyn_bson_w_index_key(dyn_bson_w* w, uint8_t type, uint32_t idx)
{
    char buf[12];
    size_t n = dyn_bson_u32_key(buf, idx);
    dyn_bson_w_key(w, type, buf, n);
}

void dyn_bson_w_sub_begin(dyn_bson_w* w, uint8_t type, const char* key, size_t klen)
{
    dyn_bson_w_key(w, type, key, klen);
    dyn_bson_w_doc_begin(w);
}

void dyn_bson_w_sub_end(dyn_bson_w* w)
{
    dyn_bson_w_doc_end(w);
}

void dyn_bson_w_double(dyn_bson_w* w, const char* key, size_t klen, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    dyn_bson_w_key(w, DYN_BSON_DOUBLE, key, klen);
    bson_w_u64(w, u);
}

void dyn_bson_w_int32(dyn_bson_w* w, const char* key, size_t klen, int32_t v)
{
    dyn_bson_w_key(w, DYN_BSON_INT32, key, klen);
    bson_w_u32(w, (uint32_t)v);
}

void dyn_bson_w_int64(dyn_bson_w* w, const char* key, size_t klen, int64_t v)
{
    dyn_bson_w_key(w, DYN_BSON_INT64, key, klen);
    bson_w_u64(w, (uint64_t)v);
}

void dyn_bson_w_string(dyn_bson_w* w, const char* key, size_t klen, const char* s, size_t n)
{
    dyn_bson_w_key(w, DYN_BSON_STRING, key, klen);
    if (n >= DYN_BSON_MAX_SIZE) {
        w->oom = 1;
        return;
    }
    bson_w_u32(w, (uint32_t)n + 1);
    dyn_bson_w_bytes(w, s, n);
    bson_w_u8(w, 0);
}

void dyn_bson_w_bool(dyn_bson_w* w, const char* key, size_t klen, int b)
{
    dyn_bson_w_key(w, DYN_BSON_BOOL, key, klen);
    bson_w_u8(w, b ? 1 : 0);
}

void dyn_bson_w_null(dyn_bson_w* w, const char* key, size_t klen)
{
    dyn_bson_w_key(w, DYN_BSON_NULL, key, klen);
}

void dyn_bson_w_date(dyn_bson_w* w, const char* key, size_t klen, int64_t ms)
{
    dyn_bson_w_key(w, DYN_BSON_DATE, key, klen);
    bson_w_u64(w, (uint64_t)ms);
}

void dyn_bson_w_oid(dyn_bson_w* w, const char* key, size_t klen, const uint8_t oid[12])
{
    dyn_bson_w_key(w, DYN_BSON_OID, key, klen);
    dyn_bson_w_bytes(w, oid, 12);
}

void dyn_bson_w_binary(dyn_bson_w* w, const char* key, size_t klen, uint8_t subtype,
    const uint8_t* p, size_t n)
{
    dyn_bson_w_key(w, DYN_BSON_BINARY, key, klen);
    if (n >= DYN_BSON_MAX_SIZE - 9) {
        w->oom = 1;
        return;
    }
    if (subtype == 2) {
        bson_w_u32(w, (uint32_t)n + 4);
        bson_w_u8(w, 2);
        bson_w_u32(w, (uint32_t)n);
    } else {
        bson_w_u32(w, (uint32_t)n);
        bson_w_u8(w, subtype);
    }
    dyn_bson_w_bytes(w, p, n);
}

void dyn_bson_w_regex(dyn_bson_w* w, const char* key, size_t klen, const char* pat, size_t plen,
    const char* opts, size_t olen)
{
    dyn_bson_w_key(w, DYN_BSON_REGEX, key, klen);
    dyn_bson_w_bytes(w, pat, plen);
    bson_w_u8(w, 0);
    dyn_bson_w_bytes(w, opts, olen);
    bson_w_u8(w, 0);
}

void dyn_bson_w_timestamp(dyn_bson_w* w, const char* key, size_t klen, uint32_t t, uint32_t i)
{
    dyn_bson_w_key(w, DYN_BSON_TIMESTAMP, key, klen);
    bson_w_u32(w, i);
    bson_w_u32(w, t);
}

void dyn_bson_w_dec128(dyn_bson_w* w, const char* key, size_t klen, const uint8_t v[16])
{
    dyn_bson_w_key(w, DYN_BSON_DEC128, key, klen);
    dyn_bson_w_bytes(w, v, 16);
}

void dyn_bson_w_value(dyn_bson_w* w, const char* key, size_t klen, dyn_bson_val v)
{
    dyn_bson_w_key(w, v.type, key, klen);
    dyn_bson_w_bytes(w, v.val, v.vlen);
}
