#ifndef DYN_BSON_BSON_H
#define DYN_BSON_BSON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DYN_BSON_DOUBLE 0x01
#define DYN_BSON_STRING 0x02
#define DYN_BSON_DOC 0x03
#define DYN_BSON_ARRAY 0x04
#define DYN_BSON_BINARY 0x05
#define DYN_BSON_UNDEFINED 0x06
#define DYN_BSON_OID 0x07
#define DYN_BSON_BOOL 0x08
#define DYN_BSON_DATE 0x09
#define DYN_BSON_NULL 0x0A
#define DYN_BSON_REGEX 0x0B
#define DYN_BSON_DBPOINTER 0x0C
#define DYN_BSON_CODE 0x0D
#define DYN_BSON_SYMBOL 0x0E
#define DYN_BSON_CODE_W_SCOPE 0x0F
#define DYN_BSON_INT32 0x10
#define DYN_BSON_TIMESTAMP 0x11
#define DYN_BSON_INT64 0x12
#define DYN_BSON_DEC128 0x13
#define DYN_BSON_MINKEY 0xFF
#define DYN_BSON_MAXKEY 0x7F

#define DYN_BSON_MAX_DEPTH 200
#define DYN_BSON_MAX_SIZE 0x7FFFFFFFu

enum {
    DYN_BSON_OK = 0,
    DYN_BSON_E_SHORT = -1,
    DYN_BSON_E_LENGTH = -2,
    DYN_BSON_E_TERM = -3,
    DYN_BSON_E_TYPE = -4,
    DYN_BSON_E_KEY = -5,
    DYN_BSON_E_UTF8 = -6,
    DYN_BSON_E_DEPTH = -7,
    DYN_BSON_E_VALUE = -8,
    DYN_BSON_E_TRAILING = -9,
    DYN_BSON_E_OOM = -10,
};

typedef struct {
    const uint8_t* p;
    uint32_t n;
    uint32_t off;
    uint8_t type;
    const char* key;
    uint32_t klen;
    const uint8_t* val;
    uint32_t vlen;
} dyn_bson_iter;

typedef struct {
    uint8_t type;
    const uint8_t* val;
    uint32_t vlen;
} dyn_bson_val;

static inline uint32_t dyn_bson_rd32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static inline uint64_t dyn_bson_rd64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static inline double dyn_bson_rdf64(const uint8_t* p)
{
    uint64_t u = dyn_bson_rd64(p);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline void dyn_bson_wr32(uint8_t* p, uint32_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    memcpy(p, &v, 4);
}

static inline void dyn_bson_wr64(uint8_t* p, uint64_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    memcpy(p, &v, 8);
}

static inline uint32_t dyn_bson_doc_len(const uint8_t* doc)
{
    return dyn_bson_rd32(doc);
}

int dyn_bson_iter_init(dyn_bson_iter* it, const uint8_t* doc, size_t avail);

int dyn_bson_iter_next(dyn_bson_iter* it);

int dyn_bson_value_size(uint8_t type, const uint8_t* v, size_t avail, uint32_t* out);

static inline dyn_bson_val dyn_bson_iter_val(const dyn_bson_iter* it)
{
    dyn_bson_val v;
    v.type = it->type;
    v.val = it->val;
    v.vlen = it->vlen;
    return v;
}

int dyn_bson_find(const uint8_t* doc, const char* key, size_t klen, dyn_bson_val* out);

typedef struct {
    int code;
    uint32_t offset;
} dyn_bson_err;

int dyn_bson_validate(const uint8_t* doc, size_t n, int max_depth, dyn_bson_err* err);

const char* dyn_bson_strerror(int code);

size_t dyn_bson_utf8_valid_prefix(const uint8_t* p, size_t n);

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
    int depth;
    uint32_t stack[DYN_BSON_MAX_DEPTH + 2];
} dyn_bson_w;

void dyn_bson_w_init(dyn_bson_w* w);
void dyn_bson_w_free(dyn_bson_w* w);
void dyn_bson_w_reset(dyn_bson_w* w);
int dyn_bson_w_reserve(dyn_bson_w* w, size_t extra);
void dyn_bson_w_doc_begin(dyn_bson_w* w);
void dyn_bson_w_doc_end(dyn_bson_w* w);
void dyn_bson_w_sub_begin(dyn_bson_w* w, uint8_t type, const char* key, size_t klen);
void dyn_bson_w_sub_end(dyn_bson_w* w);
void dyn_bson_w_key(dyn_bson_w* w, uint8_t type, const char* key, size_t klen);
void dyn_bson_w_bytes(dyn_bson_w* w, const void* p, size_t n);
void dyn_bson_w_double(dyn_bson_w* w, const char* key, size_t klen, double d);
void dyn_bson_w_int32(dyn_bson_w* w, const char* key, size_t klen, int32_t v);
void dyn_bson_w_int64(dyn_bson_w* w, const char* key, size_t klen, int64_t v);
void dyn_bson_w_string(dyn_bson_w* w, const char* key, size_t klen, const char* s, size_t n);
void dyn_bson_w_bool(dyn_bson_w* w, const char* key, size_t klen, int b);
void dyn_bson_w_null(dyn_bson_w* w, const char* key, size_t klen);
void dyn_bson_w_date(dyn_bson_w* w, const char* key, size_t klen, int64_t ms);
void dyn_bson_w_oid(dyn_bson_w* w, const char* key, size_t klen, const uint8_t oid[12]);
void dyn_bson_w_binary(dyn_bson_w* w, const char* key, size_t klen, uint8_t subtype, const uint8_t* p, size_t n);
void dyn_bson_w_regex(dyn_bson_w* w, const char* key, size_t klen, const char* pat, size_t plen, const char* opts, size_t olen);
void dyn_bson_w_timestamp(dyn_bson_w* w, const char* key, size_t klen, uint32_t t, uint32_t i);
void dyn_bson_w_dec128(dyn_bson_w* w, const char* key, size_t klen, const uint8_t v[16]);
void dyn_bson_w_value(dyn_bson_w* w, const char* key, size_t klen, dyn_bson_val v);
size_t dyn_bson_u32_key(char out[12], uint32_t idx);
void dyn_bson_w_index_key(dyn_bson_w* w, uint8_t type, uint32_t idx);

int dyn_bson_type_valid(uint8_t type);

int dyn_bson_canon_rank(uint8_t type);

int dyn_bson_is_number(uint8_t type);

double dyn_bson_num_f64(dyn_bson_val v);

int dyn_bson_num_cmp(dyn_bson_val a, dyn_bson_val b);

int dyn_bson_cmp(dyn_bson_val a, dyn_bson_val b);

int dyn_bson_doc_cmp(const uint8_t* a, const uint8_t* b);

int dyn_bson_equal(dyn_bson_val a, dyn_bson_val b);

void dyn_bson_oid_generate(uint8_t out[12]);

int dyn_bson_dec128_to_string(const uint8_t v[16], char* out, size_t cap);

int dyn_bson_dec128_from_string(const char* s, size_t n, uint8_t out[16]);

int dyn_bson_dec128_to_f64(const uint8_t v[16], double* out);

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} dyn_ks_buf;

void dyn_ks_init(dyn_ks_buf* b);
void dyn_ks_free(dyn_ks_buf* b);
void dyn_ks_value(dyn_ks_buf* b, dyn_bson_val v, int desc);
void dyn_ks_missing(dyn_ks_buf* b, int desc);
void dyn_ks_type_floor(dyn_ks_buf* b, int rank, int desc);
void dyn_ks_type_ceil(dyn_ks_buf* b, int rank, int desc);
void dyn_ks_raw(dyn_ks_buf* b, const uint8_t* p, size_t n);

#endif
