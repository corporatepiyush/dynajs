#ifndef CSV_INTERNAL_H
#define CSV_INTERNAL_H

#include "dyna-nat.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "core/dyn-simdk.h"
#include "cutils.h"
#include "core/dyn-csvx.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef CsvxBuf Buf;
typedef CsvxRow Row;
typedef CsvxTable Table;
typedef CsvxSink csv_sink;

static inline int csv_buf_reserve(Buf* b, size_t extra)
{
    return csvx_buf_reserve(b, extra);
}

static inline int buf_put(Buf* b, const char* s, size_t n)
{
    return csvx_buf_put(b, s, n);
}

static inline int buf_putc(Buf* b, char c)
{
    return csvx_buf_putc(b, c);
}

static inline void buf_free(Buf* b)
{
    csvx_buf_free(b);
}

static inline char* tcell_dup(Table* t, const char* s, size_t n)
{
    return csvx_tcell_dup(t, s, n);
}

static inline int row_push(Row* row, char* s)
{
    return csvx_row_push(row, s);
}

static inline int table_push(Table* t, Row row)
{
    return csvx_table_push(t, row);
}

static inline void table_free(Table* t)
{
    csvx_table_free(t);
}

#define CSV_SYNTAX_QUOTE CSVX_SYNTAX_QUOTE
#define CSV_SYNTAX_GARBAGE CSVX_SYNTAX_GARBAGE

int csv_parse_ex(const uint8_t* buf, size_t len, Table* t, int strict,
    int* errrow, uint8_t delim, uint8_t quote);

char* opt_str(JSContext* ctx, JSValueConst obj, const char* key, int* present);

int opt_int(JSContext* ctx, JSValueConst obj, const char* key, int64_t def,
    int64_t* out, int* present);

int opt_bool_def(JSContext* ctx, JSValueConst obj, const char* key, int def);

int opt_bool(JSContext* ctx, JSValueConst obj, const char* key);

int opt_durable(JSContext* ctx, JSValueConst obj);

int opt_str_array(JSContext* ctx, JSValueConst obj, const char* key, char*** arr);

static inline void free_str_array(char** a, int n)
{
    if (a) {
        for (int i = 0; i < n; i++)
            free(a[i]);
        free(a);
    }
}

int opts_check(JSContext* ctx, JSValueConst obj, const char* pfx,
    const char* const* keys, size_t nkeys);

int opts_delim_quote(JSContext* ctx, JSValueConst obj, const char* pfx,
    uint8_t* pdelim, uint8_t* pquote);

typedef struct {
    char* path;
} DynCsvFile;

extern JSClassID dyn_csvfile_class_id;

static inline char* csvfile_path(JSContext* ctx, JSValueConst this_val)
{
    DynResource* r = dyn_res_get(ctx, this_val, dyn_csvfile_class_id);
    if (!r)
        return NULL;
    char* p = strdup(((DynCsvFile*)r->native)->path);
    if (!p) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return p;
}

#define OPTS argv[0]

static inline int need_obj(JSContext* ctx, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "csv: expected an options object");
        return -1;
    }
    return 0;
}

JSValue js_csv_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_add_row(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_remove_row(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_remove_column(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int range_window(JSContext* ctx, JSValueConst obj, size_t total,
    size_t defcap, int single_default, const char* pfx,
    size_t* ps, size_t* pe);

JSValue js_csv_read_column_values_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_read_row_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#define CSV_REPEAT_COLS 256

typedef struct {
    csv_sink base;
    JSContext* ctx;
    JSValue rows;
    JSValue header;
    JSValue* vals;
    size_t nv, cap, width;
    int width_known, has_header;
    uint32_t k;
    const char** last_p;
    size_t* last_n;
    JSValue* last_v;
} csv_js_sink;

int csv_js_field(csv_sink* k, const char* p, size_t n, int stable);

int csv_js_row(csv_sink* k);

JSValue js_csv_parse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

extern JSClassID csv_rows_class_id;

void csv_rows_finalizer(JSRuntime* rt, JSValue val);

JSValue js_csv_rows(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_parse_dataframe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue js_csv_to_dataframe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int csv_scan_sink(const uint8_t* buf, size_t len, csv_sink* k, int strict,
    int* errrow, uint8_t delim, uint8_t quote);

const uint8_t* csv_narrow_ascii(JSContext* ctx, JSValueConst v, size_t* plen);

int csv_parse_fast(JSContext* ctx, const uint8_t* data, size_t dlen, uint8_t delim, uint8_t quote,
    int has_header, JSValue* headers, JSValue* rows, int64_t* total);

JSValue js_csv_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int csv_index_class_init(JSContext* ctx);

int csv_canonicalise(const uint8_t* data, size_t dlen, int strict, int* errrow, uint8_t delim, uint8_t quote, Buf* out);

#define CSV_ROW_INLINE 64

typedef struct {
    CsvxIndex ix;
    dyn_iobuf_t src;
    int has_src;
    uint8_t* own;
    void* map;
    size_t map_len;
    uint8_t* tmp;
    size_t tcap;
} CsvView;

int csv_view_load(JSContext* ctx, const char* path, CsvView* v, int strict, uint8_t delim, uint8_t quote, int map);

void csv_view_free(CsvView* v);

JSValue csv_view_cell(JSContext* ctx, CsvView* v, size_t r, size_t c);

JSValue csv_view_rows(JSContext* ctx, CsvView* v, const int* idx, size_t nproj, size_t start, size_t end);

int csv_view_header_index(CsvView* v, const char* name);

typedef CsvxOverlay CsvOverlay;

int csv_view_field(CsvView* v, size_t r, uint32_t c, const uint8_t** pp, size_t* pn);

int csv_emit_field_n(Buf* b, const uint8_t* s, size_t n);

void csv_overlay_init(CsvOverlay* o);

int csv_view_serialize(CsvView* v, const CsvOverlay* ov, Buf* out);

int csv_write_buf(JSContext* ctx, const char* path, const Buf* out, int durable);

int csv_view_store(JSContext* ctx, const char* path, CsvView* v, const CsvOverlay* ov, int durable);

int csv_view_resolve_column(JSContext* ctx, JSValueConst obj, CsvView* v, const char* pfx);

static inline size_t csv_view_ncols(const CsvView* v) { return v->ix.width; }

static inline size_t csv_view_nrows(const CsvView* v) { return v->ix.nrows; }

#endif

#endif
