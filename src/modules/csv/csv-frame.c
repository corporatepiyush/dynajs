#include "csv-internal.h"
#include "core/dyn-dfk.h"
#include "core/dyn-csvx.h"
#include "core/dyn-prng.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

static const char* const opts_frame_keys[] = { "columns", "dtypes", "offset", "limit", "delimiter", "quote", "strict" };

typedef struct {
    DfkCsvSpec spec;
    char** cols;
    int ncols;
    char** dt_names;
    DfkCsvKind* dt_kinds;
    uint32_t ndt;
} csv_frame_opts;

static void frame_opts_free(JSContext* ctx, csv_frame_opts* o)
{
    uint32_t i;
    (void)ctx;
    free_str_array(o->cols, o->ncols);
    for (i = 0; i < o->ndt; i++)
        free(o->dt_names[i]);
    free(o->dt_names);
    free(o->dt_kinds);
    memset(o, 0, sizeof(*o));
}

static int frame_dtypes(JSContext* ctx, JSValueConst opts, const char* pfx, csv_frame_opts* o)
{
    JSValue v;
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    int rc = -1;

    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "dtypes");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (!JS_IsObject(v) || JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "%s: 'dtypes' must be an object of column name to \"f64\", \"i32\" or \"str\"", pfx);
        goto done;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        goto done;
    o->dt_names = calloc(n ? n : 1, sizeof(*o->dt_names));
    o->dt_kinds = calloc(n ? n : 1, sizeof(*o->dt_kinds));
    if (!o->dt_names || !o->dt_kinds) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetProperty(ctx, v, tab[i].atom);
        const char* key;
        const char* s;
        DfkCsvKind k;
        if (JS_IsException(e))
            goto done;
        key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) {
            JS_FreeValue(ctx, e);
            goto done;
        }
        s = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
        JS_FreeValue(ctx, e);
        k = !s                      ? DFK_CSV_AUTO
            : strcmp(s, "f64") == 0 ? DFK_CSV_F64
            : strcmp(s, "i32") == 0 ? DFK_CSV_I32
            : strcmp(s, "str") == 0 ? DFK_CSV_STR
                                    : DFK_CSV_AUTO;
        if (s)
            JS_FreeCString(ctx, s);
        if (k == DFK_CSV_AUTO) {
            JS_ThrowTypeError(ctx, "%s: dtypes['%s'] must be \"f64\", \"i32\" or \"str\"", pfx, key);
            JS_FreeCString(ctx, key);
            goto done;
        }
        o->dt_names[o->ndt] = strdup(key);
        JS_FreeCString(ctx, key);
        if (!o->dt_names[o->ndt]) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        o->dt_kinds[o->ndt++] = k;
    }
    rc = 0;
done:
    if (tab)
        JS_FreePropertyEnum(ctx, tab, n);
    JS_FreeValue(ctx, v);
    return rc;
}

static int frame_count(JSContext* ctx, JSValueConst opts, const char* pfx, const char* key, uint64_t def, uint64_t* out)
{
    int64_t v;
    int present = 0;
    if (opt_int(ctx, opts, key, -1, &v, &present))
        return -1;
    if (!present) {
        *out = def;
        return 0;
    }
    if (v < 0) {
        JS_ThrowRangeError(ctx, "%s: '%s' must be a non-negative integer", pfx, key);
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int frame_opts(JSContext* ctx, JSValueConst opts, const char* pfx, csv_frame_opts* o)
{
    memset(o, 0, sizeof(*o));
    if (!JS_IsUndefined(opts) && !JS_IsNull(opts) && !JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", pfx);
        return -1;
    }
    if (opts_check(ctx, opts, pfx, opts_frame_keys, countof(opts_frame_keys)))
        return -1;
    if (opts_delim_quote(ctx, opts, pfx, &o->spec.delim, &o->spec.quote))
        return -1;
    o->spec.strict = opt_bool(ctx, opts, "strict");
    if (JS_HasException(ctx))
        return -1;
    if (frame_count(ctx, opts, pfx, "offset", 0, &o->spec.offset) || frame_count(ctx, opts, pfx, "limit", UINT64_MAX, &o->spec.limit))
        return -1;
    o->ncols = 0;
    if (JS_IsObject(opts)) {
        JSValue c = JS_GetPropertyStr(ctx, opts, "columns");
        int has = !JS_IsUndefined(c) && !JS_IsNull(c);
        JS_FreeValue(ctx, c);
        if (has) {
            int n = opt_str_array(ctx, opts, "columns", &o->cols);
            if (n < 0)
                return -1;
            o->ncols = n;
            o->spec.columns = (const char* const*)o->cols;
            o->spec.ncolumns = (uint32_t)n;
            if (!o->cols)
                o->spec.columns = (const char* const*)&o->cols;
        }
    }
    if (frame_dtypes(ctx, opts, pfx, o)) {
        frame_opts_free(ctx, o);
        return -1;
    }
    o->spec.dtype_names = (const char* const*)o->dt_names;
    o->spec.dtype_kinds = o->dt_kinds;
    o->spec.ndtypes = o->ndt;
    return 0;
}

static char* frame_cell_dup(const CsvxIndex* ix, size_t r, uint32_t c)
{
    const uint8_t* p;
    size_t n;
    char* s;
    int q = csvx_field(ix, r, c, &p, &n);
    s = (char*)malloc(n + 1);
    if (!s)
        return NULL;
    if (q) {
        if (csvx_needs_unescape(p, n, ix->quote)) {
            n = csvx_unquote(p, n, ix->quote, (uint8_t*)s);
        } else {
            n = n >= 2 ? n - 2 : 0;
            memcpy(s, p + 1, n);
        }
    } else {
        memcpy(s, p, n);
    }
    s[n] = 0;
    return s;
}

static int frame_find(char** hdr, uint32_t w, const char* name)
{
    uint32_t c;
    for (c = 0; c < w; c++)
        if (strcmp(hdr[c], name) == 0)
            return (int)c;
    return -1;
}

static int frame_column(const CsvxIndex* ix, CsvxColumnJob* job, uint64_t seed)
{
    size_t esz = job->kind == CSVX_COL_F64 ? 8 : 4;
    job->out = malloc(job->nrows ? job->nrows * esz : 8);
    if (!job->out)
        return CSVX_OOM;
    if (job->kind == CSVX_COL_DICT) {
        if (csvx_dict_init(&job->dict, 256))
            return CSVX_OOM;
        job->dict.seed = seed;
    }
    if (csvx_column(ix, job, 0))
        return CSVX_OOM;
    return job->status;
}

static int frame_scan(const uint8_t* buf, size_t len, const csv_frame_opts* o, DfkCsvOut* pout)
{
    CsvxIndex ix;
    DfkCsvOut out;
    char** hdr = NULL;
    int* sel = NULL;
    uint32_t w = 0, nsel, c, i;
    uint64_t data, start, count, seed = 0;
    int rc = 1, st;

    memset(&out, 0, sizeof(out));
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }
    st = csvx_build(&ix, buf, len, o->spec.delim, o->spec.quote, 0);
    if (st == CSVX_OOM)
        return -1;
    if (st != CSVX_OK)
        return 1;
    if (ix.ragged || ix.nrows == 0 || ix.width == 0 || ix.width > 4096)
        goto done;
    w = ix.width;
    hdr = (char**)calloc(w, sizeof(char*));
    if (!hdr)
        goto oom;
    for (c = 0; c < w; c++) {
        hdr[c] = frame_cell_dup(&ix, 0, c);
        if (!hdr[c])
            goto oom;
        if (frame_find(hdr, c, hdr[c]) >= 0)
            goto done;
    }
    nsel = o->spec.columns ? o->spec.ncolumns : w;
    sel = (int*)malloc((nsel ? nsel : 1) * sizeof(int));
    if (!sel)
        goto oom;
    for (i = 0; i < nsel; i++) {
        sel[i] = o->spec.columns ? frame_find(hdr, w, o->spec.columns[i]) : (int)i;
        if (sel[i] < 0)
            goto done;
        for (c = 0; c < i; c++)
            if (sel[c] == sel[i])
                goto done;
    }
    for (i = 0; i < o->spec.ndtypes; i++) {
        int k = frame_find(hdr, w, o->spec.dtype_names[i]);
        uint32_t j;
        if (k < 0)
            goto done;
        for (j = 0; j < nsel && sel[j] != k; j++)
            ;
        if (j == nsel)
            goto done;
    }
    data = ix.nrows - 1;
    start = o->spec.offset < data ? o->spec.offset : data;
    count = data - start < o->spec.limit ? data - start : o->spec.limit;
    if (count > (uint64_t)INT32_MAX)
        goto done;
    if (dyn_os_entropy(&seed, sizeof(seed)) != 0)
        seed = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)&ix;
    out.cols = (DfkCsvCol*)calloc(nsel ? nsel : 1, sizeof(DfkCsvCol));
    if (!out.cols)
        goto oom;
    out.hdr = hdr;
    out.nhdr = w;
    hdr = NULL;
    out.nrows = (uint32_t)count;
    for (i = 0; i < nsel; i++) {
        DfkCsvCol* oc = &out.cols[i];
        DfkCsvKind forced = DFK_CSV_AUTO;
        CsvxColumnJob job;
        uint32_t k;
        out.ncols = i + 1;
        oc->name = out.hdr[sel[i]];
        for (k = 0; k < o->spec.ndtypes; k++)
            if (strcmp(o->spec.dtype_names[k], oc->name) == 0)
                forced = o->spec.dtype_kinds[k];
        memset(&job, 0, sizeof(job));
        job.col = (uint32_t)sel[i];
        job.row0 = 1 + (size_t)start;
        job.nrows = (size_t)count;
        job.kind = forced == DFK_CSV_I32 ? CSVX_COL_I32 : forced == DFK_CSV_STR ? CSVX_COL_DICT
                                                                                : CSVX_COL_F64;
        st = frame_column(&ix, &job, seed);
        if (st == 1 && forced == DFK_CSV_AUTO) {
            free(job.out);
            csvx_dict_free(&job.dict);
            memset(&job, 0, sizeof(job));
            job.col = (uint32_t)sel[i];
            job.row0 = 1 + (size_t)start;
            job.nrows = (size_t)count;
            job.kind = CSVX_COL_DICT;
            st = frame_column(&ix, &job, seed);
        }
        if (st != 0) {
            free(job.out);
            csvx_dict_free(&job.dict);
            if (st == CSVX_OOM)
                goto oom;
            goto done;
        }
        if (job.kind == CSVX_COL_F64) {
            oc->kind = DFK_CSV_F64;
            oc->f64 = (double*)job.out;
        } else if (job.kind == CSVX_COL_I32) {
            oc->kind = DFK_CSV_I32;
            oc->i32 = (int32_t*)job.out;
        } else {
            oc->kind = DFK_CSV_STR;
            oc->codes = (int32_t*)job.out;
            oc->dict = (char**)calloc(job.dict.n ? job.dict.n : 1, sizeof(char*));
            if (!oc->dict) {
                csvx_dict_free(&job.dict);
                goto oom;
            }
            for (k = 0; k < job.dict.n; k++) {
                oc->dict[k] = (char*)malloc(job.dict.len[k] + 1);
                if (!oc->dict[k]) {
                    csvx_dict_free(&job.dict);
                    goto oom;
                }
                memcpy(oc->dict[k], job.dict.bytes + job.dict.off[k], job.dict.len[k]);
                oc->dict[k][job.dict.len[k]] = 0;
                oc->dict_len = k + 1;
            }
            csvx_dict_free(&job.dict);
        }
    }
    csvx_free(&ix);
    free(sel);
    *pout = out;
    return 0;
oom:
    rc = -1;
done:
    dfk_csv_free(&out);
    if (hdr) {
        for (c = 0; c < w; c++)
            free(hdr[c]);
        free(hdr);
    }
    free(sel);
    csvx_free(&ix);
    return rc;
}

static int frame_fast(JSContext* ctx, const char* pfx, const uint8_t* buf, size_t len, const csv_frame_opts* o, JSValue* pret)
{
    DfkCsvOut out;
    int rc;
    if (!dyn_df_csv_frame)
        return 1;
    memset(&out, 0, sizeof(out));
    rc = DYN_OFFLOAD_CPU(frame_scan(buf, len, o, &out));
    if (rc < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (rc > 0)
        return 1;
    *pret = dyn_df_csv_frame(ctx, pfx, &out);
    return JS_IsException(*pret) ? -1 : 0;
}

static JSValue frame_build(JSContext* ctx, const char* pfx, const uint8_t* buf, size_t len, JSValueConst opts)
{
    csv_frame_opts o;
    JSValue ret;
    if (!dyn_df_csv_reader)
        return JS_ThrowTypeError(ctx, "%s: this build has no dyna:dataframe", pfx);
    if (frame_opts(ctx, opts, pfx, &o))
        return JS_EXCEPTION;
    {
        int fr = frame_fast(ctx, pfx, buf, len, &o, &ret);
        if (fr <= 0) {
            frame_opts_free(ctx, &o);
            return fr < 0 ? JS_EXCEPTION : ret;
        }
    }
    ret = dyn_df_csv_reader(ctx, pfx, buf, len, &o.spec);
    frame_opts_free(ctx, &o);
    return ret;
}

JSValue js_csv_parse_dataframe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    size_t tlen = 0;
    const char* text;
    JSValue ret;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "csv.parseDataFrame: expected CSV text (a string)");
    text = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    ret = frame_build(ctx, "csv.parseDataFrame", (const uint8_t*)text, tlen, argc >= 2 ? argv[1] : JS_UNDEFINED);
    JS_FreeCString(ctx, text);
    return ret;
}

JSValue js_csv_to_dataframe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    dyn_iobuf_t src;
    JSValue ret;
    if (!path)
        return JS_EXCEPTION;
    if (dyn_io_slurp(path, &src, 0, DYN_MAX_INPUT) < 0) {
        if (errno == EFBIG)
            JS_ThrowRangeError(ctx, "csv.toDataFrame: file exceeds %u bytes (DYN_MAX_INPUT)", (unsigned)DYN_MAX_INPUT);
        else
            JS_ThrowTypeError(ctx, "csv.toDataFrame: cannot read '%s': %s", path, strerror(errno));
        free(path);
        return JS_EXCEPTION;
    }
    free(path);
    ret = frame_build(ctx, "csv.toDataFrame", (const uint8_t*)dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src), argc >= 1 ? argv[0] : JS_UNDEFINED);
    dyn_iobuf_free(&src);
    return ret;
}

#endif
