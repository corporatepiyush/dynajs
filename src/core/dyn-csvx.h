#ifndef DYN_CSVX_H
#define DYN_CSVX_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CSVX_OK 0
#define CSVX_IRREGULAR 1
#define CSVX_TOO_LARGE 2
#define CSVX_OOM (-1)

#define CSVX_MAX_LEN ((size_t)UINT32_MAX - 1)

typedef struct {
    const uint8_t* buf;
    size_t len;
    uint8_t delim, quote;
    uint32_t* sep;
    size_t nsep;
    uint32_t* row_first;
    uint32_t* row_start;
    size_t nrows;
    uint32_t width;
    uint32_t ragged;
} CsvxIndex;

int csvx_build(CsvxIndex* ix, const uint8_t* buf, size_t len, uint8_t delim,
    uint8_t quote, unsigned max_threads);

void csvx_free(CsvxIndex* ix);

static inline uint32_t csvx_row_fields(const CsvxIndex* ix, size_t r)
{
    return ix->row_first[r + 1] - ix->row_first[r];
}

static inline int csvx_field(const CsvxIndex* ix, size_t r, uint32_t c,
    const uint8_t** pp, size_t* pn)
{
    uint32_t f, s, e;
    if (c >= csvx_row_fields(ix, r)) {
        *pp = ix->buf;
        *pn = 0;
        return 0;
    }
    f = ix->row_first[r] + c;
    s = c ? ix->sep[f - 1] + 1 : ix->row_start[r];
    e = ix->sep[f];
    *pp = ix->buf + s;
    *pn = e - s;
    return *pn && ix->buf[s] == ix->quote;
}

size_t csvx_unquote(const uint8_t* p, size_t n, uint8_t quote, uint8_t* out);

static inline int csvx_needs_unescape(const uint8_t* p, size_t n, uint8_t quote)
{
    return n > 2 && memchr(p + 1, quote, n - 2) != NULL;
}

int csvx_parse_f64(const uint8_t* p, size_t n, double* out);

int csvx_parse_i64(const uint8_t* p, size_t n, int64_t* out);

typedef struct {
    uint32_t* slot;
    uint32_t mask;
    uint32_t n, cap;
    uint32_t* off;
    uint32_t* len;
    uint64_t* hash;
    uint8_t* bytes;
    size_t nbytes, bcap;
    uint64_t seed;
} CsvxDict;

int csvx_dict_init(CsvxDict* d, uint32_t hint);

int csvx_dict_code(CsvxDict* d, const uint8_t* p, size_t n, int32_t* code);

void csvx_dict_free(CsvxDict* d);

#define CSVX_COL_F64 1
#define CSVX_COL_I32 2
#define CSVX_COL_I64 3
#define CSVX_COL_DICT 4

typedef struct {
    int kind;
    uint32_t col;
    size_t row0, nrows;
    void* out;
    CsvxDict dict;
    size_t bad_row;
    int status;
} CsvxColumnJob;

int csvx_column(const CsvxIndex* ix, CsvxColumnJob* job, unsigned max_threads);

#define CSVX_ARENA_BLOCK 65536
#define CSVX_SYNTAX_QUOTE 1
#define CSVX_SYNTAX_GARBAGE 2

typedef struct {
    char* p;
    size_t len, cap;
} CsvxBuf;

int csvx_buf_reserve(CsvxBuf* b, size_t extra);

static inline int csvx_buf_put(CsvxBuf* b, const char* s, size_t n)
{
    if (csvx_buf_reserve(b, n))
        return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 0;
}

static inline int csvx_buf_putc(CsvxBuf* b, char c)
{
    if (csvx_buf_reserve(b, 1))
        return -1;
    b->p[b->len++] = c;
    return 0;
}

static inline void csvx_buf_free(CsvxBuf* b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

typedef struct {
    size_t off, cap;
    char data[];
} CsvxBlock;

typedef struct {
    char** f;
    size_t n, cap;
} CsvxRow;

typedef struct {
    CsvxRow* r;
    size_t n, cap;
    CsvxBlock** blk;
    size_t nblk, capblk, cur;
} CsvxTable;

typedef struct CsvxSink {
    int (*field)(struct CsvxSink* k, const char* p, size_t n, int stable);
    int (*row)(struct CsvxSink* k);
    size_t rows;
} CsvxSink;

char* csvx_tcell_dup(CsvxTable* t, const char* s, size_t n);
int csvx_row_push(CsvxRow* row, char* s);
int csvx_table_push(CsvxTable* t, CsvxRow row);
void csvx_table_free(CsvxTable* t);
int csvx_scan(const uint8_t* buf, size_t len, CsvxSink* k, int strict,
    int* errrow, uint8_t delim, uint8_t quote);
int csvx_parse_table(const uint8_t* buf, size_t len, CsvxTable* t, int strict,
    int* errrow, uint8_t delim, uint8_t quote);

typedef struct {
    const CsvxIndex* ix;
    uint8_t* tmp;
    size_t tcap;
} CsvxView;

typedef struct {
    size_t drop_row, set_row;
    uint32_t set_col, drop_col;
    const char* set_val;
    size_t set_len;
    int add_col;
    const char* add_head;
    size_t add_head_len;
    const char* add_def;
    size_t add_def_len;
    const char* add_def_q;
    size_t add_def_qlen;
} CsvxOverlay;

void csvx_overlay_init(CsvxOverlay* o);
int csvx_emit_field_n(CsvxBuf* b, const uint8_t* s, size_t n);
int csvx_view_field(CsvxView* v, size_t r, uint32_t c, const uint8_t** pp, size_t* pn);
int csvx_view_serialize(CsvxView* v, const CsvxOverlay* ov, CsvxBuf* out);

#endif
