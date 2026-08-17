/*
 * dyna:csv -- RFC 4180 reader/writer with a streaming file reader.
 *
 * Field scanning is scalar on purpose: a SIMD find_first_of measured 15-20%
 * SLOWER because fields are too short to amortise the dispatch call.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dyna-simd-kernels.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ============================ growable byte buffer ============================ */
typedef struct { char *p; size_t len, cap; } Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra) nc *= 2;
    char *np = (char *)realloc(b->p, nc);
    if (!np) return -1;
    b->p = np; b->cap = nc;
    return 0;
}
static int buf_put(Buf *b, const char *s, size_t n) {
    if (buf_reserve(b, n)) return -1;
    memcpy(b->p + b->len, s, n); b->len += n; return 0;
}
static int buf_putc(Buf *b, char c) {
    if (buf_reserve(b, 1)) return -1;
    b->p[b->len++] = c; return 0;
}
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ============================ CSV table (jagged) ============================ */
/* Row 0 is the header. A cell is a nul-terminated string owned by the table's
 * ARENA, not individually: parsing allocated one malloc per cell (10M for a
 * 1M x 10 file), which dominated ingest. Cells are carved from a block chain;
 * replaced/removed cells stay in the arena until table_free, so mutations
 * must NOT free() them. A NULL cell pointer denotes the empty string. */
typedef struct csv_ablock {
    struct csv_ablock *next;
    size_t off, cap;
    char data[];
} csv_ablock;
typedef struct { char **f; size_t n, cap; } Row;
typedef struct { Row *r; size_t n, cap; csv_ablock *arena; } Table;

/* Carve a nul-terminated copy of s[0..n) from the arena; NULL for n == 0. */
#define CSV_ARENA_BLOCK 65536
static char *tcell_dup(Table *t, const char *s, size_t n) {
    if (n == 0) return NULL;
    if (n + 1 > CSV_ARENA_BLOCK) {
        /* An exact-fit block is FULL on arrival, so making it the head would
           orphan whatever room the current head still has and force a fresh
           64 KiB block for the next short cell. Splice it behind the head
           instead: measured 46 MB of waste on a 140 MB file with one blob
           column and one short one. */
        csv_ablock *b = (csv_ablock *)malloc(sizeof(*b) + n + 1);
        char *d;
        if (!b) return NULL;
        b->cap = n + 1;
        b->off = n + 1;
        if (t->arena) { b->next = t->arena->next; t->arena->next = b; }
        else          { b->next = NULL; t->arena = b; }
        d = b->data;
        memcpy(d, s, n); d[n] = 0;
        return d;
    }
    if (!t->arena || t->arena->off + n + 1 > t->arena->cap) {
        csv_ablock *b = (csv_ablock *)malloc(sizeof(*b) + CSV_ARENA_BLOCK);
        if (!b) return NULL;
        b->next = t->arena; b->off = 0; b->cap = CSV_ARENA_BLOCK;
        t->arena = b;
    }
    char *d = t->arena->data + t->arena->off;
    memcpy(d, s, n); d[n] = 0;
    t->arena->off += n + 1;
    return d;
}

static int row_push(Row *row, char *s) {
    if (row->n == row->cap) {
        size_t nc = row->cap ? row->cap * 2 : 8;
        char **nv = (char **)realloc(row->f, nc * sizeof(char *));
        if (!nv) return -1;
        row->f = nv; row->cap = nc;
    }
    row->f[row->n++] = s; return 0;
}
static int table_push(Table *t, Row row) {
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        Row *nr = (Row *)realloc(t->r, nc * sizeof(Row));
        if (!nr) return -1;
        t->r = nr; t->cap = nc;
    }
    t->r[t->n++] = row; return 0;
}
static void table_free(Table *t) {
    for (size_t i = 0; i < t->n; i++) free(t->r[i].f);   /* the row arrays, not the cells */
    free(t->r);
    while (t->arena) { csv_ablock *b = t->arena; t->arena = b->next; free(b); }
    t->r = NULL; t->n = t->cap = 0;
}
/* header column count */
static size_t table_ncols(const Table *t) { return t->n ? t->r[0].n : 0; }
/* a cell as a C string ("" for missing/NULL) */
static const char *cell(const Table *t, size_t r, size_t c) {
    if (r >= t->n || c >= t->r[r].n || !t->r[r].f[c]) return "";
    return t->r[r].f[c];
}

/* ============================ RFC-4180 parser (SIMD) ============================ */
/* Parse `buf`/`len` into `t`. Returns 0, or -1 on OOM (t is freed). */
static int csv_parse(const uint8_t *buf, size_t len, Table *t) {
    static const uint8_t stop[] = { ',', '\n', '\r' }; /* unquoted field terminators */
    size_t i = 0;
    memset(t, 0, sizeof(*t));
    if (len == 0) return 0;

    /* pre-size the row vector from a SIMD newline count (an upper bound; embedded
     * newlines in quoted fields only make it an over-estimate, which is fine). */
    size_t est = simd.count_u8(buf, '\n', len) + 1;
    t->cap = est < 64 ? 64 : est;
    t->r = (Row *)malloc(t->cap * sizeof(Row));
    if (!t->r) return -1;

    Row row; memset(&row, 0, sizeof(row));
    Buf fld; memset(&fld, 0, sizeof(fld));

    for (;;) {
        /* --- one field --- */
        fld.len = 0;
        if (i < len && buf[i] == '"') {           /* quoted field */
            i++;
            while (i < len) {
                uint8_t c = buf[i];
                if (c == '"') {
                    if (i + 1 < len && buf[i + 1] == '"') { if (buf_putc(&fld, '"')) goto oom; i += 2; }
                    else { i++; break; }           /* closing quote */
                } else { if (buf_putc(&fld, (char)c)) goto oom; i++; }
            }
        } else {                                    /* unquoted field: SIMD jump to next terminator */
            size_t p = simd.find_first_of(buf + i, len - i, stop, countof(stop));
            if (p == SIZE_MAX) p = len - i;
            if (buf_put(&fld, (const char *)(buf + i), p)) goto oom;
            i += p;
        }
        /* commit the field (NULL for empty to avoid tiny allocs) */
        {
            char *s = NULL;
            if (fld.len) { s = tcell_dup(t, fld.p, fld.len); if (!s) goto oom; }
            if (row_push(&row, s)) goto oom;
        }
        /* --- delimiter / record end / EOF --- */
        if (i >= len) { if (table_push(t, row)) goto oom; memset(&row, 0, sizeof(row)); break; }
        {
            uint8_t c = buf[i];
            if (c == ',') { i++; continue; }
            /* record terminator (handle CRLF) */
            if (c == '\r' && i + 1 < len && buf[i + 1] == '\n') i += 2; else i++;
            if (table_push(t, row)) goto oom;
            memset(&row, 0, sizeof(row));
            if (i >= len) break;                    /* trailing newline: no spurious empty row */
        }
    }
    buf_free(&fld);
    return 0;
oom:
    buf_free(&fld);
    free(row.f);            /* cells are arena-owned: table_free covers them */
    table_free(t);
    return -1;
}

/* ============================ serializer ============================ */
static int field_needs_quote(const char *s) {
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') return 1;
    return 0;
}
static int emit_field(Buf *b, const char *s) {
    if (!field_needs_quote(s)) return buf_put(b, s, strlen(s));
    if (buf_putc(b, '"')) return -1;
    for (const char *p = s; *p; p++) {
        if (*p == '"' && buf_putc(b, '"')) return -1;
        if (buf_putc(b, *p)) return -1;
    }
    return buf_putc(b, '"');
}
static int csv_serialize(const Table *t, Buf *out) {
    for (size_t r = 0; r < t->n; r++) {
        size_t nc = t->r[r].n;
        for (size_t c = 0; c < nc; c++) {
            if (c && buf_putc(out, ',')) return -1;
            if (emit_field(out, t->r[r].f[c] ? t->r[r].f[c] : "")) return -1;
        }
        if (buf_putc(out, '\n')) return -1;
    }
    return 0;
}

/* ==================== file I/O (shared dyn_io_* primitives) ====================
 * Reads go through dyn_io_slurp (a zero-copy mmap view for large files, an
 * advise-hinted heap read otherwise); writes go through dyn_io_write_whole_atomic
 * (temp file + durable fsync + rename) -- the shared io-core primitives that
 * dyna:file and the engine's own loader use. */

/* Create parent directories of `path` (mkdir -p of the dirname). */
static void csv_mkparents(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    free(tmp);
}
/* Atomic durable write via the shared io core (temp file + durable fsync +
 * rename over `path`). */
static int csv_write_atomic(const char *path, const char *data, size_t len) {
    return dyn_io_write_whole_atomic(path, data, len, /*durable=*/1);
}

/* ============================ JS option helpers ============================ */
/* obj[key] as an owned C string, or NULL (absent/undefined). *present set. */
static char *opt_str(JSContext *ctx, JSValueConst obj, const char *key, int *present) {
    if (present) *present = 0;
    if (!JS_IsObject(obj)) return NULL;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return NULL; }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return NULL;
    char *d = strdup(s);
    JS_FreeCString(ctx, s);
    if (present) *present = 1;
    return d;
}
/* obj[key] as an int with a default; sets *present if the key was given. */
static int opt_int(JSContext *ctx, JSValueConst obj, const char *key, int64_t def,
                   int64_t *out, int *present) {
    if (present) *present = 0;
    if (!JS_IsObject(obj)) { *out = def; return 0; }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); *out = def; return 0; }
    int r = JS_ToInt64(ctx, out, v);
    JS_FreeValue(ctx, v);
    if (r) return -1;
    if (present) *present = 1;
    return 0;
}
static int opt_bool(JSContext *ctx, JSValueConst obj, const char *key) {
    if (!JS_IsObject(obj)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}
/* obj[key] as an owned array of owned C strings. Returns count, or -1 (throwing).
 * *arr NULL if the key is absent (returns 0). */
static int opt_str_array(JSContext *ctx, JSValueConst obj, const char *key, char ***arr) {
    *arr = NULL;
    if (!JS_IsObject(obj)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return 0; }
    if (!JS_IsArray(ctx, v)) { JS_FreeValue(ctx, v); JS_ThrowTypeError(ctx, "csv: '%s' must be an array", key); return -1; }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    char **out = n ? (char **)calloc(n, sizeof(char *)) : NULL;
    if (n && !out) { JS_FreeValue(ctx, v); JS_ThrowOutOfMemory(ctx); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        const char *s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s) { for (uint32_t j = 0; j < i; j++) free(out[j]); free(out); JS_FreeValue(ctx, v); return -1; }
        out[i] = strdup(s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    *arr = out;
    return (int)n;
}
static void free_str_array(char **a, int n) { if (a) { for (int i = 0; i < n; i++) free(a[i]); free(a); } }

/* find a header column index by name, or -1 */
static int header_index(const Table *t, const char *name) {
    if (t->n == 0) return -1;
    for (size_t c = 0; c < t->r[0].n; c++)
        if (strcmp(t->r[0].f[c] ? t->r[0].f[c] : "", name) == 0) return (int)c;
    return -1;
}

/* Load + parse a file into `t`. Returns 0, or -1 with a thrown exception. */
static int csv_load(JSContext *ctx, const char *path, Table *t) {
    dyn_iobuf_t src;
    if (dyn_io_slurp(path, &src, 0) < 0) { JS_ThrowTypeError(ctx, "csv: cannot read '%s': %s", path, strerror(errno)); return -1; }
    /* parse straight from the buffer (an mmap view for large files: zero copy) */
    int r = csv_parse(dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src), t);
    dyn_iobuf_free(&src);
    if (r < 0) { JS_ThrowOutOfMemory(ctx); return -1; }
    if (t->n == 0) { table_free(t); JS_ThrowTypeError(ctx, "csv: '%s' is empty (no header)", path); return -1; }
    return 0;
}
/* Serialize + atomically write `t`. Returns 0, or -1 with a thrown exception. */
static int csv_store(JSContext *ctx, const char *path, const Table *t) {
    Buf out; memset(&out, 0, sizeof(out));
    if (csv_serialize(t, &out)) { buf_free(&out); JS_ThrowOutOfMemory(ctx); return -1; }
    int r = csv_write_atomic(path, out.p ? out.p : "", out.len);
    buf_free(&out);
    if (r < 0) { JS_ThrowTypeError(ctx, "csv: cannot write '%s': %s", path, strerror(errno)); return -1; }
    return 0;
}

/* build a JS array from a table row's cells [c0,c1) */
static JSValue row_to_js(JSContext *ctx, const Table *t, size_t r, size_t ncols) {
    JSValue a = JS_NewArray(ctx);
    for (size_t c = 0; c < ncols; c++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)c, JS_NewString(ctx, cell(t, r, c)));
    return a;
}

/* ============================ CSVFile class ============================ */
/* The instance owns nothing but its file path; every method load-modify-stores
 * that file. `path` is immutable after construction. */
typedef struct { char *path; } DynCsvFile;

static JSClassID dyn_csvfile_class_id;

static void dyn_csvfile_dispose(void *native) {
    DynCsvFile *f = (DynCsvFile *)native;
    if (!f) return;
    free(f->path);
    free(f);
}

static const JSClassDef dyn_csvfile_class = {
    "CSVFile",
    .finalizer = dyn_res_finalizer,
};

/* new CSVFile(path) -- binds a path; does not touch the disk. */
static JSValue js_csvfile_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    (void)new_target;
    if (argc < 1) return JS_ThrowTypeError(ctx, "new CSVFile(path) requires a Path");
    /* Borrowed, not coerced: a Path has no close(), so nothing can invalidate
     * these bytes mid-call. The strdup below is still required -- the instance
     * outlives the constructor argument. */
    const char *s = dyn_path_borrow(ctx, argv[0], "CSVFile(path)", NULL);
    if (!s) return JS_EXCEPTION;
    DynCsvFile *f = (DynCsvFile *)calloc(1, sizeof(*f));
    if (!f) { return JS_ThrowOutOfMemory(ctx); }
    f->path = strdup(s);
    if (!f->path) { free(f); return JS_ThrowOutOfMemory(ctx); }
    return dyn_res_wrap(ctx, dyn_csvfile_class_id, f, dyn_csvfile_dispose);
}

/* Resolve `this` to a PRIVATE owned copy of its path (caller frees), or NULL
 * (throwing). Copying up front makes the method reentrancy-safe: a later
 * argument coercion may close `this` and free the instance, but not our copy. */
static char *csvfile_path(JSContext *ctx, JSValueConst this_val) {
    DynResource *r = dyn_res_get(ctx, this_val, dyn_csvfile_class_id);
    if (!r) return NULL;
    char *p = strdup(((DynCsvFile *)r->native)->path);
    if (!p) { JS_ThrowOutOfMemory(ctx); return NULL; }
    return p;
}

/* the options object (may be absent -- QuickJS pads argv to the method's
 * declared arity, so argv[0] is `undefined` when omitted, and the opt_* helpers
 * treat a non-object as "all keys absent"); data methods validate it via
 * need_obj. */
#define OPTS argv[0]
/* require an options object (for methods whose data lives in it) */
static int need_obj(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) { JS_ThrowTypeError(ctx, "csv: expected an options object"); return -1; }
    return 0;
}

/* ============================ create ============================ */
static JSValue js_csv_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    char **headers = NULL; int nh = opt_str_array(ctx, OPTS, "headers", &headers);
    JSValue ret = JS_EXCEPTION;
    if (nh < 0) { free(path); return JS_EXCEPTION; }
    if (nh == 0) { JS_ThrowTypeError(ctx, "csv.create: 'headers' must have at least one entry"); goto done; }

    int overwrite = opt_bool(ctx, OPTS, "overwrite");
    if (!overwrite) { struct stat st; if (stat(path, &st) == 0) { JS_ThrowTypeError(ctx, "csv.create: '%s' already exists (set overwrite:true)", path); goto done; } }

    Buf out; memset(&out, 0, sizeof(out));
    for (int c = 0; c < nh; c++) { if (c) buf_putc(&out, ','); emit_field(&out, headers[c]); }
    buf_putc(&out, '\n');

    /* optional initial rows */
    JSValue rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    uint32_t nrows = 0;
    if (JS_IsArray(ctx, rows)) {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length"); JS_ToUint32(ctx, &nrows, lv); JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < nrows; i++) {
            JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
            uint32_t rc = 0; JSValue rl = JS_GetPropertyStr(ctx, rv, "length"); JS_ToUint32(ctx, &rc, rl); JS_FreeValue(ctx, rl);
            if (!JS_IsArray(ctx, rv) || rc != (uint32_t)nh) {
                JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows); buf_free(&out);
                JS_ThrowTypeError(ctx, "csv.create: row %u must have exactly %d values", i, nh); goto done;
            }
            for (uint32_t c = 0; c < rc; c++) {
                JSValue cv = JS_GetPropertyUint32(ctx, rv, c);
                const char *s = JS_ToCString(ctx, cv); JS_FreeValue(ctx, cv);
                if (c) buf_putc(&out, ',');
                emit_field(&out, s ? s : ""); if (s) JS_FreeCString(ctx, s);
            }
            buf_putc(&out, '\n');
            JS_FreeValue(ctx, rv);
        }
    }
    JS_FreeValue(ctx, rows);

    csv_mkparents(path);
    if (csv_write_atomic(path, out.p ? out.p : "", out.len) < 0) {
        buf_free(&out); JS_ThrowTypeError(ctx, "csv.create: cannot write '%s': %s", path, strerror(errno)); goto done;
    }
    buf_free(&out);
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, ret, "rows", JS_NewInt64(ctx, nrows));
done:
    free_str_array(headers, nh); free(path);
    return ret;
}

/* ============================ read ============================ */
static JSValue js_csv_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc;
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }

    int64_t offset = 0, limit = -1; int has_limit = 0;
    opt_int(ctx, OPTS, "offset", 0, &offset, NULL);
    opt_int(ctx, OPTS, "limit", -1, &limit, &has_limit);
    if (offset < 0) offset = 0;
    char **cols = NULL; int ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    JSValue ret = JS_EXCEPTION;
    if (ncsel < 0) goto done;

    size_t ncols = table_ncols(&t);
    size_t total = t.n - 1;                        /* data rows */

    /* resolve the projected column indices (all columns if none requested) */
    size_t nproj = ncsel > 0 ? (size_t)ncsel : ncols;
    int *idx = (int *)malloc((nproj ? nproj : 1) * sizeof(int));
    if (!idx) { JS_ThrowOutOfMemory(ctx); goto done; }
    JSValue hjs = JS_NewArray(ctx);
    for (size_t k = 0; k < nproj; k++) {
        if (ncsel > 0) {
            int ci = header_index(&t, cols[k]);
            if (ci < 0) { free(idx); JS_FreeValue(ctx, hjs); JS_ThrowTypeError(ctx, "csv.read: no such column '%s'", cols[k]); goto done; }
            idx[k] = ci;
        } else idx[k] = (int)k;
        JS_SetPropertyUint32(ctx, hjs, (uint32_t)k, JS_NewString(ctx, cell(&t, 0, (size_t)idx[k])));
    }

    size_t start = (size_t)offset;
    size_t end = total;
    if (has_limit && limit >= 0 && start + (size_t)limit < end) end = start + (size_t)limit;
    if (start > total) start = total;

    JSValue rjs = JS_NewArray(ctx);
    uint32_t out_i = 0;
    for (size_t r = start; r < end; r++) {
        JSValue a = JS_NewArray(ctx);
        for (size_t k = 0; k < nproj; k++)
            JS_SetPropertyUint32(ctx, a, (uint32_t)k, JS_NewString(ctx, cell(&t, r + 1, (size_t)idx[k])));
        JS_SetPropertyUint32(ctx, rjs, out_i++, a);
    }
    free(idx);

    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "headers", hjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)total));
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    table_free(&t); free(path);
    return ret;
}

/* ============================ addRow ============================ */
static JSValue js_csv_add_row(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    size_t ncols = table_ncols(&t);

    JSValue rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    JSValue ret = JS_EXCEPTION;
    if (!JS_IsArray(ctx, rows)) { JS_FreeValue(ctx, rows); JS_ThrowTypeError(ctx, "csv.addRow: 'rows' must be an array"); goto done; }
    uint32_t nr = 0; { JSValue lv = JS_GetPropertyStr(ctx, rows, "length"); JS_ToUint32(ctx, &nr, lv); JS_FreeValue(ctx, lv); }

    for (uint32_t i = 0; i < nr; i++) {
        JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
        Row row; memset(&row, 0, sizeof(row));
        if (JS_IsArray(ctx, rv)) {                 /* positional */
            uint32_t rc = 0; JSValue rl = JS_GetPropertyStr(ctx, rv, "length"); JS_ToUint32(ctx, &rc, rl); JS_FreeValue(ctx, rl);
            for (size_t c = 0; c < ncols; c++) {
                char *s = NULL;
                if (c < rc) { JSValue cv = JS_GetPropertyUint32(ctx, rv, (uint32_t)c); const char *cs = JS_ToCString(ctx, cv); JS_FreeValue(ctx, cv); if (cs) { s = tcell_dup(&t, cs, strlen(cs)); JS_FreeCString(ctx, cs); } }
                row_push(&row, s);      /* cells are arena-owned; never free one */
            }
        } else if (JS_IsObject(rv)) {              /* named: map by header */
            for (size_t c = 0; c < ncols; c++) {
                JSValue cv = JS_GetPropertyStr(ctx, rv, cell(&t, 0, c));
                char *s = NULL;
                if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) { const char *cs = JS_ToCString(ctx, cv); if (cs) { s = tcell_dup(&t, cs, strlen(cs)); JS_FreeCString(ctx, cs); } }
                JS_FreeValue(ctx, cv);
                row_push(&row, s);
            }
        } else {
            JS_FreeValue(ctx, rv); JS_FreeValue(ctx, rows);
            free(row.f);
            JS_ThrowTypeError(ctx, "csv.addRow: each row must be an array or an object"); goto done;
        }
        JS_FreeValue(ctx, rv);
        if (table_push(&t, row)) { free(row.f); JS_FreeValue(ctx, rows); JS_ThrowOutOfMemory(ctx); goto done; }
    }
    JS_FreeValue(ctx, rows);
    if (csv_store(ctx, path, &t)) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "added", JS_NewInt64(ctx, nr));
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(t.n - 1)));
done:
    table_free(&t); free(path);
    return ret;
}

/* resolve a column selector (column name or columnIndex) to an index, or -1 (throwing) */
static int resolve_column(JSContext *ctx, JSValueConst obj, const Table *t) {
    int present; char *name = opt_str(ctx, obj, "column", &present);
    if (name) { int ci = header_index(t, name); if (ci < 0) JS_ThrowTypeError(ctx, "csv: no such column '%s'", name); free(name); return ci; }
    if (JS_HasException(ctx)) return -1;
    int64_t ix; int has;
    if (opt_int(ctx, obj, "columnIndex", 0, &ix, &has)) return -1;
    if (has) { if (ix < 0 || (size_t)ix >= table_ncols(t)) { JS_ThrowRangeError(ctx, "csv: columnIndex %lld out of range", (long long)ix); return -1; } return (int)ix; }
    JS_ThrowTypeError(ctx, "csv: provide 'column' or 'columnIndex'");
    return -1;
}

/* ============================ updateCell ============================ */
static JSValue js_csv_update_cell(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;

    int64_t rowi; int hasr;
    if (opt_int(ctx, OPTS, "row", 0, &rowi, &hasr) || !hasr) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.updateCell: 'row' is required"); goto done; }
    if (rowi < 0 || (size_t)rowi >= t.n - 1) { JS_ThrowRangeError(ctx, "csv.updateCell: row %lld out of range (0..%zu)", (long long)rowi, t.n - 2); goto done; }
    int ci = resolve_column(ctx, OPTS, &t);
    if (ci < 0) goto done;
    int present; char *value = opt_str(ctx, OPTS, "value", &present);
    if (!present) { free(value); if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.updateCell: 'value' is required"); goto done; }

    Row *row = &t.r[(size_t)rowi + 1];
    while (row->n <= (size_t)ci) { if (row_push(row, NULL)) { free(value); JS_ThrowOutOfMemory(ctx); goto done; } }  /* pad short rows */
    /* the old cell is arena-owned: dropping the pointer is the whole removal */
    row->f[ci] = (value && *value) ? tcell_dup(&t, value, strlen(value)) : NULL;
    if (value && *value && !row->f[ci]) { free(value); JS_ThrowOutOfMemory(ctx); goto done; }
    free(value);

    if (csv_store(ctx, path, &t)) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "row", JS_NewInt64(ctx, rowi));
    JS_SetPropertyStr(ctx, ret, "column", JS_NewString(ctx, cell(&t, 0, (size_t)ci)));
    JS_SetPropertyStr(ctx, ret, "value", JS_NewString(ctx, row->f[ci] ? row->f[ci] : ""));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ removeRow ============================ */
static JSValue js_csv_remove_row(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int64_t rowi; int hasr;
    if (opt_int(ctx, OPTS, "row", 0, &rowi, &hasr) || !hasr) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.removeRow: 'row' is required"); goto done; }
    if (rowi < 0 || (size_t)rowi >= t.n - 1) { JS_ThrowRangeError(ctx, "csv.removeRow: row %lld out of range", (long long)rowi); goto done; }
    size_t r = (size_t)rowi + 1;
    free(t.r[r].f);                     /* the row array; cells are arena-owned */
    memmove(&t.r[r], &t.r[r + 1], (t.n - r - 1) * sizeof(Row));
    t.n--;
    if (csv_store(ctx, path, &t)) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "removed", JS_NewInt64(ctx, rowi));
    JS_SetPropertyStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(t.n - 1)));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ addColumn ============================ */
static JSValue js_csv_add_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int present; char *col = opt_str(ctx, OPTS, "column", &present);
    if (!col) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.addColumn: 'column' is required"); goto done; }
    if (header_index(&t, col) >= 0) { JS_ThrowTypeError(ctx, "csv.addColumn: column '%s' already exists", col); goto done; }
    char *def = opt_str(ctx, OPTS, "defaultValue", NULL);   /* NULL => "" */

    size_t ncols = table_ncols(&t);
    for (size_t r = 0; r < t.n; r++) {
        Row *row = &t.r[r];
        while (row->n < ncols) { if (row_push(row, NULL)) { free(def); JS_ThrowOutOfMemory(ctx); goto done; } }
        /* NULL is tcell_dup's SUCCESS value for an empty cell, so an empty
           name must not be routed through it or it reads as an OOM. */
        char *v = (r == 0) ? (*col ? tcell_dup(&t, col, strlen(col)) : NULL)
                           : (def && *def ? tcell_dup(&t, def, strlen(def)) : NULL);
        if (r == 0 && *col && !v) { free(def); JS_ThrowOutOfMemory(ctx); goto done; }
        if (row_push(row, v)) { free(def); JS_ThrowOutOfMemory(ctx); goto done; }
    }
    free(def);
    if (csv_store(ctx, path, &t)) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "column", JS_NewString(ctx, col));
    JS_SetPropertyStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)table_ncols(&t)));
done:
    free(col); table_free(&t); free(path);
    return ret;
}

/* ============================ removeColumn ============================ */
static JSValue js_csv_remove_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    int ci = resolve_column(ctx, OPTS, &t);
    if (ci < 0) goto done;
    for (size_t r = 0; r < t.n; r++) {
        Row *row = &t.r[r];
        if ((size_t)ci >= row->n) continue;
        /* the dropped cell stays in the arena until table_free */
        memmove(&row->f[ci], &row->f[ci + 1], (row->n - ci - 1) * sizeof(char *));
        row->n--;
    }
    if (csv_store(ctx, path, &t)) goto done;
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "removedIndex", JS_NewInt64(ctx, ci));
    JS_SetPropertyStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)table_ncols(&t)));
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ renameColumn ============================ */
static JSValue js_csv_rename_column(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    char *oldn = opt_str(ctx, OPTS, "oldName", NULL);
    char *newn = opt_str(ctx, OPTS, "newName", NULL);
    if (!oldn || !newn) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.renameColumn: 'oldName' and 'newName' are required"); goto done; }
    int oi = header_index(&t, oldn);
    if (oi < 0) { JS_ThrowTypeError(ctx, "csv.renameColumn: no such column '%s'", oldn); goto done; }
    if (strcmp(oldn, newn) != 0) {
        if (header_index(&t, newn) >= 0) { JS_ThrowTypeError(ctx, "csv.renameColumn: column '%s' already exists", newn); goto done; }
        /* NULL means "empty cell", not "allocation failed": an empty newName
           must not be reported as OOM. */
        char *nv = *newn ? tcell_dup(&t, newn, strlen(newn)) : NULL;
        if (*newn && !nv) { JS_ThrowOutOfMemory(ctx); goto done; }
        t.r[0].f[oi] = nv;              /* the old header cell is arena-owned */
        if (csv_store(ctx, path, &t)) goto done;
    }
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "oldName", JS_NewString(ctx, oldn));
    JS_SetPropertyStr(ctx, ret, "newName", JS_NewString(ctx, newn));
done:
    free(oldn); free(newn); table_free(&t); free(path);
    return ret;
}

/* clamp a [start,end) window over `total` data rows with a max span; returns 0 or -1 (throwing) */
static int range_window(JSContext *ctx, JSValueConst obj, size_t total, size_t maxspan, size_t *ps, size_t *pe) {
    int64_t start, end; int has_end;
    opt_int(ctx, obj, "start", 0, &start, NULL);
    opt_int(ctx, obj, "end", 0, &end, &has_end);
    if (start < 0) start = 0;
    /* the cap is on the REQUESTED window when end is explicit; an omitted end
     * means "to the end" and is not capped. */
    if (has_end && end > start && (uint64_t)(end - start) > (uint64_t)maxspan) {
        JS_ThrowRangeError(ctx, "csv: requested window %lld exceeds the maximum of %zu rows", (long long)(end - start), maxspan);
        return -1;
    }
    size_t s = (size_t)start > total ? total : (size_t)start;
    size_t e = has_end ? ((end < 0) ? 0 : (size_t)end) : total;
    if (e > total) e = total;
    if (e < s) e = s;
    *ps = s; *pe = e;
    return 0;
}

/* ============================ readColumnValuesRange ============================ */
static JSValue js_csv_read_column_values_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    char *col = opt_str(ctx, OPTS, "column", NULL);
    if (!col) { if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "csv.readColumnValuesRange: 'column' is required"); goto done; }
    int ci = header_index(&t, col);
    if (ci < 0) { JS_ThrowTypeError(ctx, "csv: no such column '%s'", col); goto done; }
    size_t s, e;
    if (range_window(ctx, OPTS, t.n - 1, 1000, &s, &e)) goto done;
    JSValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    for (size_t r = s; r < e; r++) JS_SetPropertyUint32(ctx, a, k++, JS_NewString(ctx, cell(&t, r + 1, (size_t)ci)));
    ret = a;
done:
    free(col); table_free(&t); free(path);
    return ret;
}

/* ============================ readRowRange ============================ */
static JSValue js_csv_read_row_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    /* options are OPTIONAL here (readRowRange() means row 0), so need_obj is
       too strict -- but a POSITIONAL call was silently ignored and returned
       row 0. Refuse a given non-object; keep the documented no-arg form. */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "csv.readRowRange: expected an options object "
                               "like { start, end } -- positional arguments are "
                               "not read");
        free(path);
        return JS_EXCEPTION;
    }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    size_t ncols = table_ncols(&t);
    /* default: a single row (start..start+1) */
    int64_t start; int has_end; int64_t end;
    opt_int(ctx, OPTS, "start", 0, &start, NULL);
    opt_int(ctx, OPTS, "end", 0, &end, &has_end);
    if (start < 0) start = 0;
    if (has_end && end > start && (uint64_t)(end - start) > 100) { JS_ThrowRangeError(ctx, "csv.readRowRange: requested window %lld exceeds the maximum of 100 rows", (long long)(end - start)); goto done; }
    size_t total = t.n - 1;
    size_t s = (size_t)start > total ? total : (size_t)start;
    size_t e = has_end ? (end < 0 ? 0 : (size_t)end) : s + 1;
    if (e > total) e = total;
    if (e < s) e = s;
    JSValue hjs = JS_NewArray(ctx);
    for (size_t c = 0; c < ncols; c++) JS_SetPropertyUint32(ctx, hjs, (uint32_t)c, JS_NewString(ctx, cell(&t, 0, c)));
    JSValue rjs = JS_NewArray(ctx);
    uint32_t k = 0;
    for (size_t r = s; r < e; r++) JS_SetPropertyUint32(ctx, rjs, k++, row_to_js(ctx, &t, r + 1, ncols));
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "headers", hjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
done:
    table_free(&t); free(path);
    return ret;
}

/* ============================ selectColumnRange ============================ */
static JSValue js_csv_select_column_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char *path = csvfile_path(ctx, this_val);
    if (!path) return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) { free(path); return JS_EXCEPTION; }
    Table t;
    if (csv_load(ctx, path, &t)) { free(path); return JS_EXCEPTION; }
    char **cols = NULL; int ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    JSValue ret = JS_EXCEPTION;
    if (ncsel < 0) { table_free(&t); free(path); return JS_EXCEPTION; }
    if (ncsel == 0) { JS_ThrowTypeError(ctx, "csv.selectColumnRange: 'columns' must be non-empty"); goto done; }
    int *idx = (int *)malloc(ncsel * sizeof(int));
    if (!idx) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (int k = 0; k < ncsel; k++) { int ci = header_index(&t, cols[k]); if (ci < 0) { free(idx); JS_ThrowTypeError(ctx, "csv.selectColumnRange: no such column '%s'", cols[k]); goto done; } idx[k] = ci; }
    size_t s, e;
    if (range_window(ctx, OPTS, t.n - 1, 100, &s, &e)) { free(idx); goto done; }
    JSValue cjs = JS_NewArray(ctx);
    for (int k = 0; k < ncsel; k++) JS_SetPropertyUint32(ctx, cjs, (uint32_t)k, JS_NewString(ctx, cols[k]));
    JSValue rjs = JS_NewArray(ctx);
    uint32_t out_i = 0;
    for (size_t r = s; r < e; r++) {
        JSValue a = JS_NewArray(ctx);
        for (int k = 0; k < ncsel; k++) JS_SetPropertyUint32(ctx, a, (uint32_t)k, JS_NewString(ctx, cell(&t, r + 1, (size_t)idx[k])));
        JS_SetPropertyUint32(ctx, rjs, out_i++, a);
    }
    free(idx);
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "columns", cjs);
    JS_SetPropertyStr(ctx, ret, "rows", rjs);
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    table_free(&t); free(path);
    return ret;
}

/* ============================ registration ============================ */
/* the CSVFile prototype methods (all operate on the instance path) */
static const JSCFunctionListEntry csvfile_methods[] = {
    JS_CFUNC_DEF("create", 1, js_csv_create),
    JS_CFUNC_DEF("read", 1, js_csv_read),
    JS_CFUNC_DEF("addRow", 1, js_csv_add_row),
    JS_CFUNC_DEF("updateCell", 1, js_csv_update_cell),
    JS_CFUNC_DEF("removeRow", 1, js_csv_remove_row),
    JS_CFUNC_DEF("addColumn", 1, js_csv_add_column),
    JS_CFUNC_DEF("removeColumn", 1, js_csv_remove_column),
    JS_CFUNC_DEF("renameColumn", 1, js_csv_rename_column),
    JS_CFUNC_DEF("readColumnValuesRange", 1, js_csv_read_column_values_range),
    JS_CFUNC_DEF("readRowRange", 1, js_csv_read_row_range),
    JS_CFUNC_DEF("selectColumnRange", 1, js_csv_select_column_range),
};

static int csv_module_init(JSContext *ctx, JSModuleDef *m) {
    return dyn_register_class(ctx, m, &dyn_csvfile_class_id, &dyn_csvfile_class,
                              csvfile_methods, countof(csvfile_methods),
                              js_csvfile_ctor, "CSVFile");
}

int js_nat_init_csv(JSContext *ctx) {
    simd_init(); /* select the best find_first_of / count_u8 kernels */
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:csv", csv_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "CSVFile");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CSV */
