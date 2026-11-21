#ifndef DYN_PGWIRE_H
#define DYN_PGWIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_PGW_E_LEN -1
#define DYN_PGW_E_TRUNC -2
#define DYN_PGW_E_PAST -3
#define DYN_PGW_E_FORMAT -4
#define DYN_PGW_NULL 0xffffffffu

typedef struct {
    const uint8_t* b;
    size_t n, at;
    uint16_t count, i;
} dyn_pgw_iter_t;

typedef struct {
    const uint8_t* name;
    size_t nlen;
    uint32_t table_oid, type_oid;
    uint16_t column, format;
} dyn_pgw_field_t;

uint32_t dyn_pgw_u32(const uint8_t* p);
uint16_t dyn_pgw_u16(const uint8_t* p);

long dyn_pgw_frame(const uint8_t* b, size_t n, size_t maxmsg, char* type,
    const uint8_t** body, size_t* blen);

int dyn_pgw_rowdesc_begin(dyn_pgw_iter_t* it, const uint8_t* b, size_t n);
int dyn_pgw_rowdesc_next(dyn_pgw_iter_t* it, dyn_pgw_field_t* f);

int dyn_pgw_copy_response(const uint8_t* b, size_t n, uint8_t* fmt, uint16_t* ncols);
int dyn_pgw_negotiate_version(const uint8_t* b, size_t n, uint32_t* minor, uint32_t* nopt);
int dyn_pgw_backend_key(const uint8_t* b, size_t n, int minor, uint32_t* pid);

#define DYN_PGW_EPOCH_DAYS 10957
int dyn_pgw_numeric_bin(const uint8_t* p, size_t n, char* out, size_t cap, size_t* need);
int64_t dyn_pgw_days_from_civil(int64_t y, unsigned m, unsigned d);
void dyn_pgw_civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d);
int dyn_pgw_ts_text(const char* s, size_t n, int kind, int64_t* us);
int dyn_pgw_ts_format(int64_t us, int kind, char* out, size_t cap);
#define DYN_PGW_MAXDIM 6
typedef struct {
    uint32_t off;
    int32_t len;
} dyn_pgw_span_t;
typedef struct {
    int32_t ndim, hasnull;
    uint32_t elem;
    int32_t dims[DYN_PGW_MAXDIM], lb[DYN_PGW_MAXDIM];
    size_t nel;
    const uint8_t* data;
    size_t dlen;
} dyn_pgw_arrbin_t;
int dyn_pgw_array_text(const char* s, size_t n, int* ndim, int32_t dims[DYN_PGW_MAXDIM],
    dyn_pgw_span_t* sp, size_t cap, char* out, size_t* nel);
int dyn_pgw_array_bin(const uint8_t* p, size_t n, dyn_pgw_arrbin_t* h);
int dyn_pgw_array_bin_next(dyn_pgw_arrbin_t* h, size_t* at, const uint8_t** e, int32_t* len);
uint32_t dyn_pgw_array_elem(uint32_t oid);
#define DYN_PGW_RANGE_EMPTY 0x01
#define DYN_PGW_RANGE_LB_INC 0x02
#define DYN_PGW_RANGE_UB_INC 0x04
#define DYN_PGW_RANGE_LB_INF 0x08
#define DYN_PGW_RANGE_UB_INF 0x10
typedef struct {
    int flags;
    const uint8_t *lo, *hi;
    int32_t lo_len, hi_len;
} dyn_pgw_range_t;
int dyn_pgw_range_text(const char* s, size_t n, dyn_pgw_range_t* r, char* out);
int dyn_pgw_range_bin(const uint8_t* p, size_t n, dyn_pgw_range_t* r);
int dyn_pgw_multirange_text(const char* s, size_t n, size_t* at, dyn_pgw_range_t* r, char* out);
int dyn_pgw_multirange_bin(const uint8_t* p, size_t n, size_t* at, dyn_pgw_range_t* r);
uint32_t dyn_pgw_range_sub(uint32_t oid);
uint32_t dyn_pgw_multirange_range(uint32_t oid);
#define DYN_PGW_URI_SCHEME (-1)
#define DYN_PGW_URI_BRACKET (-2)
#define DYN_PGW_URI_EMPTY6 (-3)
#define DYN_PGW_URI_AFTER6 (-4)
#define DYN_PGW_DEC_HEX (-1)
#define DYN_PGW_DEC_NUL (-2)
#define DYN_PGW_DEC_SPACE (-3)
typedef struct {
    size_t user, user_n, pass, pass_n, net, net_end, db, db_n, q, q_n;
    int has_user, has_pass, has_db, has_q;
} dyn_pgw_uri_t;
int dyn_pgw_uri_split(const char* s, size_t n, dyn_pgw_uri_t* u);
int dyn_pgw_uri_host(const char* s, size_t end, size_t* at, size_t* h, size_t* hn, size_t* p, size_t* pn, int* has_port);
int dyn_pgw_uri_param(const char* s, size_t end, size_t* at, size_t* k, size_t* kn, size_t* v, size_t* vn);
long dyn_pgw_uri_decode(const char* s, size_t n, char* out);
long dyn_pgw_pgpass_find(const char* f, size_t n, const char* host, const char* port, const char* db, const char* user, char* out);
int dyn_pgw_interval_text(const char* s, size_t n, int64_t* us, int32_t* days, int32_t* months);

int dyn_pgw_datarow_begin(dyn_pgw_iter_t* it, const uint8_t* b, size_t n);
int dyn_pgw_datarow_next(dyn_pgw_iter_t* it, const uint8_t** col, uint32_t* clen);

size_t dyn_pgw_cstr(const uint8_t* b, size_t n, size_t* at, const uint8_t** s);
int dyn_pgw_field_next(const uint8_t* b, size_t n, size_t* at, char* code,
    const uint8_t** s, size_t* slen);

long dyn_pgw_bytea_hex(const uint8_t* p, size_t n, uint8_t* out);
void dyn_pgw_hex(const uint8_t* p, size_t n, char* out);
int dyn_pgw_int8_text(const uint8_t* p, size_t n, int64_t* v);
void dyn_pgw_uuid(const uint8_t* p, char* out);

#ifdef __cplusplus
}
#endif

#endif
