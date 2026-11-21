#include "csv-internal.h"
#include "core/dyn-csvx.h"
#include "core/dyn-prng.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

#include <sys/mman.h>

#define CSV_CACHE_SLOTS 4096
#define CSV_CACHE_MAXLEN 48
#define CSV_CACHE_COLS 64
#define CSV_VIEW_CACHE_MIN 1024
#define CSV_CACHE_PROBE_WINDOW 4096

typedef struct {
    uint64_t h;
    const uint8_t* p;
    uint32_t n;
    JSValue v;
} CsvCacheEnt;

typedef struct {
    CsvCacheEnt* e;
    uint32_t used, slots;
    uint32_t lookups, hits;
    int off;
} CsvCache;

typedef struct {
    JSRuntime* rt;
    JSValue pin;
    uint8_t* owned;
    void* map;
    size_t map_len;
    CsvxIndex ix;
    int has_header;
    uint32_t width;
    size_t row0, nrows;
    uint64_t seed;
    JSValue headers;
} CsvIndexObj;

JSClassID csv_index_class_id;

static inline uint64_t csv_hash(const uint8_t* p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (n * 0xBF58476D1CE4E5B9ULL);
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x94D049BB133111EBULL;
        h ^= h >> 29;
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t w = 0;
        memcpy(&w, p, n);
        h = (h ^ w) * 0x94D049BB133111EBULL;
    }
    h ^= h >> 32;
    return h;
}

static void cache_clear(JSContext* ctx, CsvCache* c)
{
    uint32_t k;
    if (!c->e)
        return;
    for (k = 0; k < c->slots; k++)
        if (c->e[k].p)
            JS_FreeValue(ctx, c->e[k].v);
    free(c->e);
    c->e = NULL;
    c->used = 0;
}

static void cache_size(CsvCache* c, uint32_t n, size_t rows)
{
    uint32_t i, s = 16;
    while (s < CSV_CACHE_SLOTS && (size_t)s < rows * 2)
        s <<= 1;
    for (i = 0; i < n; i++)
        c[i].slots = s;
}

static void cache_free(JSContext* ctx, CsvCache* c, uint32_t n)
{
    uint32_t i;
    if (!c)
        return;
    for (i = 0; i < n; i++)
        cache_clear(ctx, &c[i]);
    free(c);
}

static JSValue cell_string(JSContext* ctx, const CsvxIndex* ix, size_t r, uint32_t c, CsvCache* cache, uint8_t** tmp, size_t* tcap)
{
    const uint8_t* p;
    size_t n;
    if (csvx_field(ix, r, c, &p, &n)) {
        if (csvx_needs_unescape(p, n, ix->quote)) {
            if (n > *tcap) {
                uint8_t* t = (uint8_t*)realloc(*tmp, n);
                if (!t)
                    return JS_ThrowOutOfMemory(ctx);
                *tmp = t;
                *tcap = n;
            }
            n = csvx_unquote(p, n, ix->quote, *tmp);
            return JS_NewStringLen(ctx, (const char*)*tmp, n);
        }
        p++;
        n = n >= 2 ? n - 2 : 0;
    }
    if (n == 0)
        return JS_NewStringLen(ctx, "", 0);
    if (cache && !cache->off && n <= CSV_CACHE_MAXLEN) {
        uint64_t h;
        if (++cache->lookups == CSV_CACHE_PROBE_WINDOW) {
            if (cache->hits * 4 < cache->lookups) {
                cache->off = 1;
                return JS_NewStringLen(ctx, (const char*)p, n);
            }
            cache->lookups = cache->hits = 0;
        }
        h = csv_hash(p, n);
        uint32_t pos = (uint32_t)h & (cache->slots - 1), probes;
        if (!cache->e) {
            cache->e = (CsvCacheEnt*)calloc(cache->slots, sizeof(CsvCacheEnt));
            if (!cache->e)
                return JS_NewStringLen(ctx, (const char*)p, n);
        }
        for (probes = 0; probes < 8; probes++) {
            CsvCacheEnt* e = &cache->e[pos];
            if (!e->p) {
                JSValue v;
                if (cache->used * 2 >= cache->slots)
                    break;
                v = JS_NewStringLen(ctx, (const char*)p, n);
                if (JS_IsException(v))
                    return v;
                e->h = h;
                e->p = p;
                e->n = (uint32_t)n;
                e->v = JS_DupValue(ctx, v);
                cache->used++;
                return v;
            }
            if (e->h == h && e->n == n && memcmp(e->p, p, n) == 0) {
                cache->hits++;
                return JS_DupValue(ctx, e->v);
            }
            pos = (pos + 1) & (cache->slots - 1);
        }
    }
    return JS_NewStringLen(ctx, (const char*)p, n);
}

static JSValue row_array(JSContext* ctx, const CsvxIndex* ix, size_t r, uint32_t width, JSValue* vals, CsvCache* cache, uint8_t** tmp, size_t* tcap)
{
    uint32_t c;
    for (c = 0; c < width; c++) {
        vals[c] = cell_string(ctx, ix, r, c, cache && c < CSV_CACHE_COLS ? &cache[c] : NULL, tmp, tcap);
        if (JS_IsException(vals[c])) {
            while (c--)
                JS_FreeValue(ctx, vals[c]);
            return JS_EXCEPTION;
        }
    }
    return JS_NewArrayFrom(ctx, (int)width, vals);
}

const uint8_t* csv_narrow_ascii(JSContext* ctx, JSValueConst v, size_t* plen)
{
    size_t n = 0, i = 0;
    const uint8_t* p = JS_GetNarrowStringBytes(ctx, v, &n);
    uint64_t acc = 0;
    if (!p)
        return NULL;
    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8);
        memcpy(&b, p + i + 8, 8);
        memcpy(&c, p + i + 16, 8);
        memcpy(&d, p + i + 24, 8);
        acc |= a | b | c | d;
    }
    for (; i < n; i++)
        acc |= p[i];
    if (acc & 0x8080808080808080ULL)
        return NULL;
    *plen = n;
    return p;
}

int csv_parse_fast(JSContext* ctx, const uint8_t* data, size_t dlen, uint8_t delim, uint8_t quote,
    int has_header, JSValue* headers, JSValue* rows, int64_t* total)
{
    CsvxIndex ix;
    int rc = csvx_build(&ix, data, dlen, delim, quote, 0);
    size_t r, first, nout;
    uint32_t width;
    JSValue *vals = NULL, *rowv = NULL;
    CsvCache* cache = NULL;
    uint8_t* tmp = NULL;
    size_t tcap = 0;
    int ret = -1;
    uint32_t ncache = 0;

    if (rc == CSVX_IRREGULAR || rc == CSVX_TOO_LARGE)
        return 1;
    if (rc != CSVX_OK) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    width = ix.width;
    *headers = JS_UNDEFINED;
    *rows = JS_UNDEFINED;
    if (width > (uint32_t)INT32_MAX) {
        csvx_free(&ix);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    first = has_header && ix.nrows ? 1 : 0;
    nout = ix.nrows - first;
    if (nout > (size_t)INT32_MAX) {
        csvx_free(&ix);
        JS_ThrowRangeError(ctx, "csv.parse: too many rows");
        return -1;
    }
    vals = (JSValue*)malloc((width ? width : 1) * sizeof(JSValue));
    rowv = (JSValue*)malloc((nout ? nout : 1) * sizeof(JSValue));
    ncache = width < CSV_CACHE_COLS ? (width ? width : 1) : CSV_CACHE_COLS;
    cache = (CsvCache*)calloc(ncache, sizeof(CsvCache));
    if (!vals || !rowv || !cache) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    cache_size(cache, ncache, nout);
    if (first) {
        *headers = row_array(ctx, &ix, 0, width, vals, NULL, &tmp, &tcap);
        if (JS_IsException(*headers))
            goto done;
    }
    for (r = 0; r < nout; r++) {
        rowv[r] = row_array(ctx, &ix, r + first, width, vals, cache, &tmp, &tcap);
        if (JS_IsException(rowv[r])) {
            while (r--)
                JS_FreeValue(ctx, rowv[r]);
            goto done;
        }
    }
    *rows = JS_NewArrayFrom(ctx, (int)nout, rowv);
    if (JS_IsException(*rows))
        goto done;
    *total = (int64_t)nout;
    ret = 0;
done:
    if (ret) {
        JS_FreeValue(ctx, *headers);
        *headers = JS_UNDEFINED;
    }
    cache_free(ctx, cache, ncache);
    free(tmp);
    free(vals);
    free(rowv);
    csvx_free(&ix);
    return ret;
}

typedef struct {
    Buf out;
    uint8_t delim, quote;
    int first;
} canon_sink;

typedef struct {
    csv_sink base;
    canon_sink* cs;
} canon_sink_wrap;

static int canon_field(csv_sink* k, const char* p, size_t n, int stable)
{
    canon_sink* cs = ((canon_sink_wrap*)k)->cs;
    size_t i;
    (void)stable;
    if (!cs->first && buf_putc(&cs->out, (char)cs->delim))
        return -1;
    cs->first = 0;
    if (buf_putc(&cs->out, (char)cs->quote))
        return -1;
    for (i = 0; i < n; i++) {
        if ((uint8_t)p[i] == cs->quote && buf_putc(&cs->out, (char)cs->quote))
            return -1;
        if (buf_putc(&cs->out, p[i]))
            return -1;
    }
    return buf_putc(&cs->out, (char)cs->quote);
}

static int canon_row(csv_sink* k)
{
    canon_sink* cs = ((canon_sink_wrap*)k)->cs;
    cs->first = 1;
    return buf_putc(&cs->out, '\n');
}

int csv_canonicalise(const uint8_t* data, size_t dlen, int strict, int* errrow, uint8_t delim, uint8_t quote, Buf* out)
{
    canon_sink cs;
    canon_sink_wrap w;
    int r;
    memset(&cs, 0, sizeof(cs));
    memset(&w, 0, sizeof(w));
    cs.delim = delim;
    cs.quote = quote;
    cs.first = 1;
    w.base.field = canon_field;
    w.base.row = canon_row;
    w.cs = &cs;
    r = csv_scan_sink(data, dlen, &w.base, strict, errrow, delim, quote);
    if (r != 0) {
        buf_free(&cs.out);
        memset(out, 0, sizeof(*out));
        return r;
    }
    *out = cs.out;
    return 0;
}

static int view_map(CsvView* v, const char* path)
{
    struct stat st;
    void* m;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    if ((uint64_t)st.st_size > (uint64_t)DYN_MAX_INPUT) {
        close(fd);
        return -2;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    v->map = m;
    v->map_len = (size_t)st.st_size;
    return 0;
}

int csv_view_load(JSContext* ctx, const char* path, CsvView* v, int strict, uint8_t delim, uint8_t quote, int map)
{
    const uint8_t* data;
    size_t dlen;
    int rc, mr;
    memset(v, 0, sizeof(*v));
    mr = map ? view_map(v, path) : -1;
    if (mr == 0) {
        data = (const uint8_t*)v->map;
        dlen = v->map_len;
    } else {
        if (mr == -2 || dyn_io_slurp(path, &v->src, 0, DYN_MAX_INPUT) < 0) {
            if (mr == -2 || errno == EFBIG)
                JS_ThrowRangeError(ctx, "csv: file exceeds %u bytes (DYN_MAX_INPUT)", (unsigned)DYN_MAX_INPUT);
            else
                JS_ThrowTypeError(ctx, "csv: cannot read '%s': %s", path, strerror(errno));
            return -1;
        }
        v->has_src = 1;
        data = (const uint8_t*)dyn_iobuf_rdata(&v->src);
        dlen = dyn_iobuf_rlen(&v->src);
    }
    if (dlen >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        dlen -= 3;
    }
    rc = csvx_build(&v->ix, data ? data : (const uint8_t*)"", dlen, delim, quote, 0);
    if (rc == CSVX_IRREGULAR) {
        Buf cb;
        int errrow = 0, r2;
        csvx_free(&v->ix);
        r2 = csv_canonicalise(data, dlen, strict, &errrow, delim, quote, &cb);
        if (r2 == CSV_SYNTAX_QUOTE || r2 == CSV_SYNTAX_GARBAGE) {
            JS_ThrowSyntaxError(ctx, r2 == CSV_SYNTAX_QUOTE ? "csv: row %d: unterminated quoted field" : "csv: row %d: unexpected text after a closing quote", errrow);
            csv_view_free(v);
            return -1;
        }
        if (r2 != 0) {
            JS_ThrowOutOfMemory(ctx);
            csv_view_free(v);
            return -1;
        }
        if (v->has_src)
            dyn_iobuf_free(&v->src);
        v->has_src = 0;
        if (v->map)
            munmap(v->map, v->map_len);
        v->map = NULL;
        v->own = (uint8_t*)cb.p;
        rc = csvx_build(&v->ix, v->own ? v->own : (const uint8_t*)"", cb.len, delim, quote, 0);
    }
    if (rc != CSVX_OK) {
        if (rc == CSVX_TOO_LARGE)
            JS_ThrowRangeError(ctx, "csv: '%s' is larger than 4 GiB", path);
        else
            JS_ThrowOutOfMemory(ctx);
        csv_view_free(v);
        return -1;
    }
    if (v->ix.nrows == 0) {
        csv_view_free(v);
        JS_ThrowTypeError(ctx, "csv: '%s' is empty (no header)", path);
        return -1;
    }
    return 0;
}

void csv_view_free(CsvView* v)
{
    csvx_free(&v->ix);
    if (v->has_src)
        dyn_iobuf_free(&v->src);
    v->has_src = 0;
    if (v->map)
        munmap(v->map, v->map_len);
    v->map = NULL;
    free(v->own);
    v->own = NULL;
    free(v->tmp);
    v->tmp = NULL;
    v->tcap = 0;
}

JSValue csv_view_cell(JSContext* ctx, CsvView* v, size_t r, size_t c)
{
    if (r >= v->ix.nrows || c >= UINT32_MAX)
        return JS_NewStringLen(ctx, "", 0);
    return cell_string(ctx, &v->ix, r, (uint32_t)c, NULL, &v->tmp, &v->tcap);
}

JSValue csv_view_rows(JSContext* ctx, CsvView* v, const int* idx, size_t nproj, size_t start, size_t end)
{
    size_t nout = end > start ? end - start : 0, r, k;
    JSValue* vals = NULL;
    JSValue* rowv = NULL;
    CsvCache* cache = NULL;
    JSValue ret = JS_EXCEPTION;
    if (nout > (size_t)INT32_MAX || nproj > (size_t)INT32_MAX)
        return JS_ThrowRangeError(ctx, "csv: too many rows");
    vals = (JSValue*)malloc((nproj ? nproj : 1) * sizeof(JSValue));
    rowv = (JSValue*)malloc((nout ? nout : 1) * sizeof(JSValue));
    if (nout >= CSV_VIEW_CACHE_MIN)
        cache = (CsvCache*)calloc(nproj < CSV_CACHE_COLS ? nproj + 1 : CSV_CACHE_COLS, sizeof(CsvCache));
    if (!vals || !rowv || (nout >= CSV_VIEW_CACHE_MIN && !cache)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (cache)
        cache_size(cache, nproj < CSV_CACHE_COLS ? (uint32_t)nproj + 1 : CSV_CACHE_COLS, nout);
    for (r = 0; r < nout; r++) {
        for (k = 0; k < nproj; k++) {
            size_t c = idx ? (size_t)idx[k] : k;
            vals[k] = cell_string(ctx, &v->ix, start + r, (uint32_t)c, cache && k < CSV_CACHE_COLS ? &cache[k] : NULL, &v->tmp, &v->tcap);
            if (JS_IsException(vals[k])) {
                while (k--)
                    JS_FreeValue(ctx, vals[k]);
                goto fail_rows;
            }
        }
        rowv[r] = JS_NewArrayFrom(ctx, (int)nproj, vals);
        if (JS_IsException(rowv[r]))
            goto fail_rows;
    }
    ret = JS_NewArrayFrom(ctx, (int)nout, rowv);
    goto done;
fail_rows:
    while (r--)
        JS_FreeValue(ctx, rowv[r]);
done:
    cache_free(ctx, cache, nproj < CSV_CACHE_COLS ? (uint32_t)nproj + 1 : CSV_CACHE_COLS);
    free(vals);
    free(rowv);
    return ret;
}

int csv_view_header_index(CsvView* v, const char* name)
{
    size_t want = strlen(name), c;
    for (c = 0; c < v->ix.width; c++) {
        const uint8_t* p;
        size_t n;
        if (csvx_field(&v->ix, 0, (uint32_t)c, &p, &n)) {
            if (n > v->tcap) {
                uint8_t* t = (uint8_t*)realloc(v->tmp, n);
                if (!t)
                    return -1;
                v->tmp = t;
                v->tcap = n;
            }
            n = csvx_unquote(p, n, v->ix.quote, v->tmp);
            p = v->tmp;
        }
        if (n == want && memcmp(p, name, n) == 0)
            return (int)c;
    }
    return -1;
}

static void csv_index_finalizer(JSRuntime* rt, JSValue val)
{
    CsvIndexObj* o = (CsvIndexObj*)JS_GetOpaque(val, csv_index_class_id);
    if (!o)
        return;
    csvx_free(&o->ix);
    if (o->map)
        munmap(o->map, o->map_len);
    free(o->owned);
    JS_FreeValueRT(rt, o->pin);
    JS_FreeValueRT(rt, o->headers);
    free(o);
}

static void csv_index_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    CsvIndexObj* o = (CsvIndexObj*)JS_GetOpaque(val, csv_index_class_id);
    if (o) {
        JS_MarkValue(rt, o->pin, mark_func);
        JS_MarkValue(rt, o->headers, mark_func);
    }
}

static const JSClassDef csv_index_class = { "CsvIndex", .finalizer = csv_index_finalizer, .gc_mark = csv_index_mark };

static int index_source(JSContext* ctx, JSValueConst src, CsvIndexObj* o, const uint8_t** pdata, size_t* plen)
{
    size_t n = 0;
    if (JS_IsString(src)) {
        const uint8_t* p = csv_narrow_ascii(ctx, src, &n);
        if (p) {
            o->pin = JS_DupValue(ctx, src);
            *pdata = p;
            *plen = n;
            return 0;
        }
        {
            const char* s = JS_ToCStringLen(ctx, &n, src);
            if (!s)
                return -1;
            o->owned = (uint8_t*)malloc(n ? n : 1);
            if (!o->owned) {
                JS_FreeCString(ctx, s);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            memcpy(o->owned, s, n);
            JS_FreeCString(ctx, s);
            *pdata = o->owned;
            *plen = n;
            return 0;
        }
    }
    if (JS_GetBufferKind(src) != JS_BUFFER_KIND_NONE) {
        const uint8_t* p;
        size_t off = 0, blen = 0, bpe = 0;
        if (JS_GetBufferKind(src) == JS_BUFFER_KIND_BUFFER) {
            p = JS_GetArrayBuffer(ctx, &blen, src);
            if (!p)
                return -1;
        } else {
            JSValue ab = JS_GetTypedArrayBuffer(ctx, src, &off, &blen, &bpe);
            if (JS_IsException(ab))
                return -1;
            p = JS_GetArrayBuffer(ctx, &n, ab);
            JS_FreeValue(ctx, ab);
            if (!p)
                return -1;
            p += off;
        }
        o->owned = (uint8_t*)malloc(blen ? blen : 1);
        if (!o->owned) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(o->owned, p, blen);
        *pdata = o->owned;
        *plen = blen;
        return 0;
    }
    if (dyn_value_is_path(src)) {
        const char* path = dyn_path_borrow(ctx, src, "csv.index", NULL);
        struct stat st;
        int fd;
        if (!path)
            return -1;
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            JS_ThrowTypeError(ctx, "csv.index: cannot read '%s': %s", path, strerror(errno));
            return -1;
        }
        if (fstat(fd, &st) != 0) {
            int e = errno;
            close(fd);
            JS_ThrowTypeError(ctx, "csv.index: cannot read '%s': %s", path, strerror(e));
            return -1;
        }
        if ((uint64_t)st.st_size > CSVX_MAX_LEN) {
            close(fd);
            JS_ThrowRangeError(ctx, "csv.index: %s is larger than 4 GiB", path);
            return -1;
        }
        if (st.st_size > 0) {
            void* m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) {
                int e = errno;
                close(fd);
                JS_ThrowTypeError(ctx, "csv.index: cannot read '%s': %s", path, strerror(e));
                return -1;
            }
            o->map = m;
            o->map_len = (size_t)st.st_size;
        }
        close(fd);
        *pdata = (const uint8_t*)o->map;
        *plen = o->map_len;
        return 0;
    }
    JS_ThrowTypeError(ctx, "csv.index: expected a string, Uint8Array, ArrayBuffer or Path");
    return -1;
}

static const char* const opts_index_keys[] = { "delimiter", "quote", "hasHeader", "strict" };

static CsvIndexObj* index_get(JSContext* ctx, JSValueConst this_val)
{
    return (CsvIndexObj*)JS_GetOpaque2(ctx, this_val, csv_index_class_id);
}

JSValue js_csv_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    uint8_t delim, quote;
    int strict, has_header, rc;
    const uint8_t* data = NULL;
    size_t dlen = 0;
    CsvIndexObj* o;
    JSValue obj;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "csv.index: expected a string, Uint8Array, ArrayBuffer or Path");
    if (opts_check(ctx, opts, "csv.index", opts_index_keys, countof(opts_index_keys)))
        return JS_EXCEPTION;
    if (opts_delim_quote(ctx, opts, "csv.index", &delim, &quote))
        return JS_EXCEPTION;
    strict = opt_bool(ctx, opts, "strict");
    has_header = opt_bool_def(ctx, opts, "hasHeader", 1);
    o = (CsvIndexObj*)calloc(1, sizeof(*o));
    if (!o)
        return JS_ThrowOutOfMemory(ctx);
    o->rt = JS_GetRuntime(ctx);
    o->pin = JS_UNDEFINED;
    o->headers = JS_UNDEFINED;
    o->has_header = has_header;
    obj = JS_NewObjectClass(ctx, (int)csv_index_class_id);
    if (JS_IsException(obj)) {
        free(o);
        return obj;
    }
    JS_SetOpaque(obj, o);
    if (index_source(ctx, argv[0], o, &data, &dlen))
        goto fail;
    if (dlen >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        dlen -= 3;
    }
    rc = csvx_build(&o->ix, data, dlen, delim, quote, 0);
    if (rc == CSVX_IRREGULAR) {
        Buf cb;
        int errrow = 0, r2 = csv_canonicalise(data, dlen, strict, &errrow, delim, quote, &cb);
        if (r2 == CSV_SYNTAX_QUOTE || r2 == CSV_SYNTAX_GARBAGE) {
            JS_ThrowSyntaxError(ctx, r2 == CSV_SYNTAX_QUOTE ? "csv.index: row %d: unterminated quoted field" : "csv.index: row %d: unexpected text after a closing quote", errrow);
            goto fail;
        }
        if (r2 != 0) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (o->map) {
            munmap(o->map, o->map_len);
            o->map = NULL;
        }
        free(o->owned);
        JS_FreeValue(ctx, o->pin);
        o->pin = JS_UNDEFINED;
        o->owned = (uint8_t*)cb.p;
        data = o->owned;
        dlen = cb.len;
        rc = csvx_build(&o->ix, data ? data : (const uint8_t*)"", dlen, delim, quote, 0);
    }
    if (rc == CSVX_TOO_LARGE) {
        JS_ThrowRangeError(ctx, "csv.index: input is larger than 4 GiB");
        goto fail;
    }
    if (rc != CSVX_OK) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    o->width = o->ix.width;
    o->row0 = has_header && o->ix.nrows ? 1 : 0;
    o->nrows = o->ix.nrows - o->row0;
    if (dyn_os_entropy(&o->seed, sizeof(o->seed)) != 0)
        o->seed = (uint64_t)(uintptr_t)o ^ 0x9E3779B97F4A7C15ULL;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static int index_row_arg(JSContext* ctx, CsvIndexObj* o, JSValueConst v, const char* op, size_t* out)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "CsvIndex.%s: row must be a number", op);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0) || d != (double)(size_t)d || (size_t)d >= o->nrows) {
        JS_ThrowRangeError(ctx, "CsvIndex.%s: row %g is outside 0..%zu", op, d, o->nrows);
        return -1;
    }
    *out = (size_t)d + o->row0;
    return 0;
}

static int index_col_arg(JSContext* ctx, CsvIndexObj* o, JSValueConst v, const char* op, uint32_t* out)
{
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (!(d >= 0) || d != (double)(uint32_t)d || (uint32_t)d >= o->width) {
            JS_ThrowRangeError(ctx, "CsvIndex.%s: column %g is outside 0..%u", op, d, o->width);
            return -1;
        }
        *out = (uint32_t)d;
        return 0;
    }
    if (JS_IsString(v)) {
        size_t n = 0;
        uint32_t c;
        const char* name;
        if (!o->has_header || o->ix.nrows == 0) {
            JS_ThrowTypeError(ctx, "CsvIndex.%s: a column name needs a header row", op);
            return -1;
        }
        name = JS_ToCStringLen(ctx, &n, v);
        if (!name)
            return -1;
        for (c = 0; c < o->width; c++) {
            const uint8_t* p;
            size_t fn;
            uint8_t* tmp = NULL;
            int match;
            if (csvx_field(&o->ix, 0, c, &p, &fn)) {
                if (csvx_needs_unescape(p, fn, o->ix.quote)) {
                    tmp = (uint8_t*)malloc(fn);
                    if (!tmp) {
                        JS_FreeCString(ctx, name);
                        JS_ThrowOutOfMemory(ctx);
                        return -1;
                    }
                    fn = csvx_unquote(p, fn, o->ix.quote, tmp);
                    p = tmp;
                } else {
                    p++;
                    fn = fn >= 2 ? fn - 2 : 0;
                }
            }
            match = fn == n && memcmp(p, name, n) == 0;
            free(tmp);
            if (match) {
                JS_FreeCString(ctx, name);
                *out = c;
                return 0;
            }
        }
        JS_ThrowTypeError(ctx, "CsvIndex.%s: no column named '%s'", op, name);
        JS_FreeCString(ctx, name);
        return -1;
    }
    JS_ThrowTypeError(ctx, "CsvIndex.%s: column must be a number or a header name", op);
    return -1;
}

static JSValue csv_index_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    size_t r;
    uint32_t c;
    uint8_t* tmp = NULL;
    size_t tcap = 0;
    JSValue v;
    if (!o)
        return JS_EXCEPTION;
    if (index_row_arg(ctx, o, argc > 0 ? argv[0] : JS_UNDEFINED, "get", &r)
        || index_col_arg(ctx, o, argc > 1 ? argv[1] : JS_UNDEFINED, "get", &c))
        return JS_EXCEPTION;
    v = cell_string(ctx, &o->ix, r, c, NULL, &tmp, &tcap);
    free(tmp);
    return v;
}

static JSValue csv_index_row(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    size_t r, tcap = 0;
    uint8_t* tmp = NULL;
    JSValue *vals, a;
    if (!o)
        return JS_EXCEPTION;
    if (index_row_arg(ctx, o, argc > 0 ? argv[0] : JS_UNDEFINED, "row", &r))
        return JS_EXCEPTION;
    vals = (JSValue*)malloc((o->width ? o->width : 1) * sizeof(JSValue));
    if (!vals)
        return JS_ThrowOutOfMemory(ctx);
    a = row_array(ctx, &o->ix, r, o->width, vals, NULL, &tmp, &tcap);
    free(vals);
    free(tmp);
    return a;
}

static JSValue typed_take(JSContext* ctx, void* data, size_t bytes, JSTypedArrayEnum kind)
{
    JSValue ab = dyn_nat_ab_take(ctx, data, bytes);
    JSValue ta;
    if (JS_IsException(ab))
        return ab;
    {
        JSValueConst args[3];
        args[0] = ab;
        args[1] = JS_UNDEFINED;
        args[2] = JS_UNDEFINED;
        ta = JS_NewTypedArray(ctx, 3, args, kind);
    }
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue column_strings(JSContext* ctx, CsvIndexObj* o, uint32_t c)
{
    JSValue* vals = (JSValue*)malloc((o->nrows ? o->nrows : 1) * sizeof(JSValue));
    CsvCache cache;
    uint8_t* tmp = NULL;
    size_t tcap = 0, r;
    JSValue a;
    if (!vals)
        return JS_ThrowOutOfMemory(ctx);
    if (o->nrows > (size_t)INT32_MAX) {
        free(vals);
        return JS_ThrowRangeError(ctx, "CsvIndex.column: too many rows for an array");
    }
    memset(&cache, 0, sizeof(cache));
    cache_size(&cache, 1, o->nrows);
    for (r = 0; r < o->nrows; r++) {
        vals[r] = cell_string(ctx, &o->ix, r + o->row0, c, &cache, &tmp, &tcap);
        if (JS_IsException(vals[r])) {
            while (r--)
                JS_FreeValue(ctx, vals[r]);
            free(vals);
            free(tmp);
            cache_clear(ctx, &cache);
            return JS_EXCEPTION;
        }
    }
    a = JS_NewArrayFrom(ctx, (int)o->nrows, vals);
    free(vals);
    free(tmp);
    cache_clear(ctx, &cache);
    return a;
}

static JSValue column_error(JSContext* ctx, CsvIndexObj* o, CsvxColumnJob* job, const char* what)
{
    const uint8_t* p;
    size_t n;
    csvx_field(&o->ix, job->row0 + job->bad_row, job->col, &p, &n);
    if (n > 40)
        n = 40;
    return JS_ThrowTypeError(ctx, "CsvIndex.column: row %zu, column %u: '%.*s' is not %s",
        job->bad_row, job->col, (int)n, (const char*)p, what);
}

static JSValue csv_index_column(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    CsvxColumnJob job;
    uint32_t c;
    const char* dt = NULL;
    size_t esz = 0;
    JSValue ret = JS_EXCEPTION;
    if (!o)
        return JS_EXCEPTION;
    if (index_col_arg(ctx, o, argc > 0 ? argv[0] : JS_UNDEFINED, "column", &c))
        return JS_EXCEPTION;
    if (argc < 2 || JS_IsUndefined(argv[1]))
        return column_strings(ctx, o, c);
    if (!JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "CsvIndex.column: dtype must be \"f64\", \"i32\", \"i64\" or \"dict\"");
    dt = JS_ToCString(ctx, argv[1]);
    if (!dt)
        return JS_EXCEPTION;
    memset(&job, 0, sizeof(job));
    job.col = c;
    job.row0 = o->row0;
    job.nrows = o->nrows;
    if (strcmp(dt, "f64") == 0) {
        job.kind = CSVX_COL_F64;
        esz = 8;
    } else if (strcmp(dt, "i32") == 0) {
        job.kind = CSVX_COL_I32;
        esz = 4;
    } else if (strcmp(dt, "i64") == 0) {
        job.kind = CSVX_COL_I64;
        esz = 8;
    } else if (strcmp(dt, "dict") == 0) {
        job.kind = CSVX_COL_DICT;
        esz = 4;
    }
    JS_FreeCString(ctx, dt);
    if (!job.kind)
        return JS_ThrowTypeError(ctx, "CsvIndex.column: dtype must be \"f64\", \"i32\", \"i64\" or \"dict\"");
    if (o->nrows > SIZE_MAX / 8 - 1)
        return JS_ThrowRangeError(ctx, "CsvIndex.column: too many rows");
    job.out = malloc(o->nrows ? o->nrows * esz : 8);
    if (!job.out)
        return JS_ThrowOutOfMemory(ctx);
    if (job.kind == CSVX_COL_DICT) {
        if (csvx_dict_init(&job.dict, 1024)) {
            free(job.out);
            return JS_ThrowOutOfMemory(ctx);
        }
        job.dict.seed = o->seed;
    }
    if (csvx_column(&o->ix, &job, 0) != 0) {
        JS_ThrowOutOfMemory(ctx);
        goto out;
    }
    if (job.status == 1) {
        column_error(ctx, o, &job, job.kind == CSVX_COL_F64 ? "a number" : job.kind == CSVX_COL_I32 ? "a 32-bit integer"
                                                                                                    : "a 64-bit integer");
        goto out;
    }
    if (job.kind == CSVX_COL_DICT) {
        JSValue codes, values, *vv;
        uint32_t k;
        if (job.dict.n > (uint32_t)INT32_MAX) {
            JS_ThrowRangeError(ctx, "CsvIndex.column: too many distinct values");
            goto out;
        }
        vv = (JSValue*)malloc((job.dict.n ? job.dict.n : 1) * sizeof(JSValue));
        if (!vv) {
            JS_ThrowOutOfMemory(ctx);
            goto out;
        }
        for (k = 0; k < job.dict.n; k++) {
            vv[k] = JS_NewStringLen(ctx, (const char*)job.dict.bytes + job.dict.off[k], job.dict.len[k]);
            if (JS_IsException(vv[k])) {
                while (k--)
                    JS_FreeValue(ctx, vv[k]);
                free(vv);
                goto out;
            }
        }
        values = JS_NewArrayFrom(ctx, (int)job.dict.n, vv);
        free(vv);
        if (JS_IsException(values))
            goto out;
        codes = typed_take(ctx, job.out, o->nrows * 4, JS_TYPED_ARRAY_INT32);
        job.out = NULL;
        if (JS_IsException(codes)) {
            JS_FreeValue(ctx, values);
            goto out;
        }
        ret = JS_NewObject(ctx);
        if (JS_IsException(ret)) {
            JS_FreeValue(ctx, codes);
            JS_FreeValue(ctx, values);
            goto out;
        }
        JS_DefinePropertyValueStr(ctx, ret, "codes", codes, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ret, "values", values, JS_PROP_C_W_E);
        goto out;
    }
    ret = typed_take(ctx, job.out, o->nrows * esz,
        job.kind == CSVX_COL_F64 ? JS_TYPED_ARRAY_FLOAT64 : job.kind == CSVX_COL_I32 ? JS_TYPED_ARRAY_INT32
                                                                                     : JS_TYPED_ARRAY_BIG_INT64);
    job.out = NULL;
out:
    free(job.out);
    csvx_dict_free(&job.dict);
    return ret;
}

static JSValue csv_index_headers(JSContext* ctx, JSValueConst this_val)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    if (!o)
        return JS_EXCEPTION;
    if (JS_IsUndefined(o->headers)) {
        JSValue h;
        if (o->row0) {
            JSValue* vals = (JSValue*)malloc((o->width ? o->width : 1) * sizeof(JSValue));
            uint8_t* tmp = NULL;
            size_t tcap = 0;
            if (!vals)
                return JS_ThrowOutOfMemory(ctx);
            h = row_array(ctx, &o->ix, 0, o->width, vals, NULL, &tmp, &tcap);
            free(vals);
            free(tmp);
        } else {
            h = JS_NewArray(ctx);
        }
        if (JS_IsException(h))
            return h;
        o->headers = h;
    }
    return JS_DupValue(ctx, o->headers);
}

static JSValue csv_index_row_count(JSContext* ctx, JSValueConst this_val)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    if (!o)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)o->nrows);
}

static JSValue csv_index_column_count(JSContext* ctx, JSValueConst this_val)
{
    CsvIndexObj* o = index_get(ctx, this_val);
    if (!o)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)o->width);
}

static const JSCFunctionListEntry csv_index_proto[] = {
    JS_CGETSET_DEF("headers", csv_index_headers, NULL),
    JS_CGETSET_DEF("rowCount", csv_index_row_count, NULL),
    JS_CGETSET_DEF("columnCount", csv_index_column_count, NULL),
    JS_CFUNC_DEF("get", 2, csv_index_get),
    JS_CFUNC_DEF("row", 1, csv_index_row),
    JS_CFUNC_DEF("column", 2, csv_index_column),
};

int csv_index_class_init(JSContext* ctx)
{
    JSValue proto;
    JS_NewClassID(&csv_index_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), csv_index_class_id, &csv_index_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, csv_index_proto, countof(csv_index_proto)) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, csv_index_class_id, proto);
    return 0;
}

#endif
