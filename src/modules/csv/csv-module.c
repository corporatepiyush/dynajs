#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

#define CSV_TEXT_MAX ((1u << 30) - 1)

static int field_needs_quote(const char* s, size_t n, uint8_t delim, uint8_t quote)
{
    for (const char* p = s; p < s + n; p++)
        if (*p == (char)delim || *p == (char)quote || *p == '\n' || *p == '\r')
            return 1;
    return 0;
}

static int emit_field(Buf* b, const char* s, size_t n, uint8_t delim, uint8_t quote)
{
    if (!field_needs_quote(s, n, delim, quote))
        return buf_put(b, s, n);
    if (buf_putc(b, (char)quote))
        return -1;
    for (const char* p = s; p < s + n; p++) {
        if (*p == (char)quote && buf_putc(b, (char)quote))
            return -1;
        if (buf_putc(b, *p))
            return -1;
    }
    return buf_putc(b, (char)quote);
}

static void emit_classes(uint8_t* cls, uint8_t delim, uint8_t quote)
{
    memset(cls, 0, 128);
    memset(cls + 128, 2, 128);
    cls[0] = 2;
    cls[delim] |= 1;
    cls[quote] |= 1;
    cls['\n'] |= 1;
    cls['\r'] |= 1;
}

static int emit_ascii_field(JSContext* ctx, Buf* b, JSValueConst v, const uint8_t* cls, uint8_t quote)
{
    size_t n = 0, i, run = 0;
    const uint8_t* s = JS_GetNarrowStringBytes(ctx, v, &n);
    uint8_t acc = 0;
    if (!s)
        return 1;
    for (i = 0; i < n; i++)
        acc |= cls[s[i]];
    if (acc & 2)
        return 1;
    if (!acc)
        return buf_put(b, (const char*)s, n) ? -1 : 0;
    if (csv_buf_reserve(b, n + n / 4 + 2) || buf_putc(b, (char)quote))
        return -1;
    for (i = 0; i < n; i++)
        if (s[i] == quote) {
            if (buf_put(b, (const char*)s + run, i + 1 - run))
                return -1;
            run = i;
        }
    if (buf_put(b, (const char*)s + run, n - run))
        return -1;
    return buf_putc(b, (char)quote) ? -1 : 0;
}

static void csv_mkparents(const char* path)
{
    char* tmp = strdup(path);
    if (!tmp)
        return;
    for (char* p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    free(tmp);
}

static int csv_write_atomic(const char* path, const char* data, size_t len, int durable)
{
    return dyn_io_write_whole_atomic(path, data, len, durable);
}

static const char* const opts_create_keys[] = { "headers", "rows", "overwrite" };

static const char* const opts_update_keys[] = { "row", "column", "columnIndex", "value", "strict", "durable" };

static const char* const opts_add_col_keys[] = { "column", "defaultValue", "strict", "durable" };

static const char* const opts_rename_keys[] = { "oldName", "newName", "strict", "durable" };

static const char* const opts_select_keys[] = { "columns", "start", "end", "offset", "limit", "maxRows", "strict" };

static const char* const opts_stringify_keys[] = { "delimiter", "quote", "hasHeader" };

JSClassID dyn_csvfile_class_id;

static void dyn_csvfile_dispose(void* native)
{
    DynCsvFile* f = (DynCsvFile*)native;
    if (!f)
        return;
    free(f->path);
    free(f);
}

static const JSClassDef dyn_csvfile_class = {
    "CSVFile",
    .finalizer = dyn_res_finalizer,
};

static JSValue js_csvfile_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new CSVFile(path) requires a Path");
    const char* s = dyn_path_borrow(ctx, argv[0], "CSVFile(path)", NULL);
    if (!s)
        return JS_EXCEPTION;
    DynCsvFile* f = (DynCsvFile*)calloc(1, sizeof(*f));
    if (!f) {
        return JS_ThrowOutOfMemory(ctx);
    }
    f->path = strdup(s);
    if (!f->path) {
        free(f);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_csvfile_class_id, f, dyn_csvfile_dispose);
}

static JSValue js_csv_create(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.create", opts_create_keys, countof(opts_create_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    char** headers = NULL;
    int nh = opt_str_array(ctx, OPTS, "headers", &headers);
    JSValue ret = JS_EXCEPTION;
    if (nh < 0) {
        free(path);
        return JS_EXCEPTION;
    }
    if (nh == 0) {
        JS_ThrowTypeError(ctx, "csv.create: 'headers' must have at least one entry");
        goto done;
    }

    int overwrite = opt_bool(ctx, OPTS, "overwrite");
    if (!overwrite) {
        struct stat st;
        if (stat(path, &st) == 0) {
            JS_ThrowTypeError(ctx, "csv.create: '%s' already exists (set overwrite:true)", path);
            goto done;
        }
    }

    Buf out;
    memset(&out, 0, sizeof(out));
    for (int c = 0; c < nh; c++) {
        if ((c && buf_putc(&out, ',')) || emit_field(&out, headers[c], strlen(headers[c]), ',', '"')) {
            buf_free(&out);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    if (buf_putc(&out, '\n')) {
        buf_free(&out);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    JSValue rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    uint32_t nrows = 0;
    if (!JS_IsUndefined(rows) && !JS_IsNull(rows) && !JS_IsArray(ctx, rows)) {
        JS_FreeValue(ctx, rows);
        buf_free(&out);
        JS_ThrowTypeError(ctx, "csv.create: 'rows' must be an array");
        goto done;
    }
    if (JS_IsArray(ctx, rows)) {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length");
        if (JS_ToUint32(ctx, &nrows, lv) < 0) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, rows);
            buf_free(&out);
            goto done;
        }
        JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < nrows; i++) {
            JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
            uint32_t rc = 0;
            JSValue rl = JS_GetPropertyStr(ctx, rv, "length");
            if (JS_ToUint32(ctx, &rc, rl) < 0) {
                JS_FreeValue(ctx, rl);
                JS_FreeValue(ctx, rv);
                JS_FreeValue(ctx, rows);
                buf_free(&out);
                goto done;
            }
            JS_FreeValue(ctx, rl);
            if (!JS_IsArray(ctx, rv) || rc != (uint32_t)nh) {
                JS_FreeValue(ctx, rv);
                JS_FreeValue(ctx, rows);
                buf_free(&out);
                JS_ThrowTypeError(ctx, "csv.create: row %u must have exactly %d values", i, nh);
                goto done;
            }
            for (uint32_t c = 0; c < rc; c++) {
                JSValue cv = JS_GetPropertyUint32(ctx, rv, c);
                size_t slen = 0;
                const char* s = JS_ToCStringLen(ctx, &slen, cv);
                JS_FreeValue(ctx, cv);
                if (!s) {
                    JS_FreeValue(ctx, rv);
                    JS_FreeValue(ctx, rows);
                    buf_free(&out);
                    goto done;
                }
                if ((c && buf_putc(&out, ',')) || emit_field(&out, s, slen, ',', '"')) {
                    JS_FreeCString(ctx, s);
                    JS_FreeValue(ctx, rv);
                    JS_FreeValue(ctx, rows);
                    buf_free(&out);
                    JS_ThrowOutOfMemory(ctx);
                    goto done;
                }
                JS_FreeCString(ctx, s);
            }
            if (buf_putc(&out, '\n')) {
                JS_FreeValue(ctx, rv);
                JS_FreeValue(ctx, rows);
                buf_free(&out);
                JS_ThrowOutOfMemory(ctx);
                goto done;
            }
            JS_FreeValue(ctx, rv);
        }
    }
    JS_FreeValue(ctx, rows);

    csv_mkparents(path);
    if (csv_write_atomic(path, out.p ? out.p : "", out.len, 1) < 0) {
        buf_free(&out);
        JS_ThrowTypeError(ctx, "csv.create: cannot write '%s': %s", path, strerror(errno));
        goto done;
    }
    buf_free(&out);
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "path", JS_NewString(ctx, path), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "rows", JS_NewInt64(ctx, nrows), JS_PROP_C_W_E);
done:
    free_str_array(headers, nh);
    free(path);
    return ret;
}

static JSValue js_csv_update_cell(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.updateCell", opts_update_keys, countof(opts_update_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    CsvOverlay ov;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EXCEPTION;

    int64_t rowi;
    int hasr;
    if (opt_int(ctx, OPTS, "row", 0, &rowi, &hasr) || !hasr) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "csv.updateCell: 'row' is required");
        goto done;
    }
    if (rowi < 0 || (size_t)rowi >= csv_view_nrows(&t) - 1) {
        if (csv_view_nrows(&t) < 2)
            JS_ThrowRangeError(ctx, "csv.updateCell: row %lld out of range (the file has no data rows)", (long long)rowi);
        else
            JS_ThrowRangeError(ctx, "csv.updateCell: row %lld out of range (0..%zu)", (long long)rowi, csv_view_nrows(&t) - 2);
        goto done;
    }
    int ci = csv_view_resolve_column(ctx, OPTS, &t, "csv.updateCell");
    if (ci < 0)
        goto done;
    int present;
    char* value = opt_str(ctx, OPTS, "value", &present);
    if (!present) {
        free(value);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "csv.updateCell: 'value' is required");
        goto done;
    }

    csv_overlay_init(&ov);
    ov.set_row = (size_t)rowi + 1;
    ov.set_col = (uint32_t)ci;
    ov.set_val = value ? value : "";
    ov.set_len = strlen(ov.set_val);
    if (csv_view_store(ctx, path, &t, &ov, opt_durable(ctx, OPTS))) {
        free(value);
        goto done;
    }
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "row", JS_NewInt64(ctx, rowi), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "column", csv_view_cell(ctx, &t, 0, (size_t)ci), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "value", JS_NewString(ctx, ov.set_val), JS_PROP_C_W_E);
    free(value);
done:
    csv_view_free(&t);
    free(path);
    return ret;
}

static JSValue js_csv_add_column(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.addColumn", opts_add_col_keys, countof(opts_add_col_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    CsvOverlay ov;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EXCEPTION;
    int present;
    char* col = opt_str(ctx, OPTS, "column", &present);
    if (!col) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "csv.addColumn: 'column' is required");
        goto done;
    }
    if (csv_view_header_index(&t, col) >= 0) {
        JS_ThrowTypeError(ctx, "csv.addColumn: column '%s' already exists", col);
        goto done;
    }
    char* def = opt_str(ctx, OPTS, "defaultValue", NULL);

    if (!def && JS_HasException(ctx))
        goto done;
    csv_overlay_init(&ov);
    ov.add_col = 1;
    ov.add_head = col;
    ov.add_head_len = strlen(col);
    ov.add_def = def ? def : "";
    ov.add_def_len = strlen(ov.add_def);
    int st = csv_view_store(ctx, path, &t, &ov, opt_durable(ctx, OPTS));
    free(def);
    if (st)
        goto done;
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "column", JS_NewString(ctx, col), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)csv_view_ncols(&t) + 1), JS_PROP_C_W_E);
done:
    free(col);
    csv_view_free(&t);
    free(path);
    return ret;
}

static JSValue js_csv_rename_column(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.renameColumn", opts_rename_keys, countof(opts_rename_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    CsvOverlay ov;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EXCEPTION;
    char* oldn = opt_str(ctx, OPTS, "oldName", NULL);
    char* newn = opt_str(ctx, OPTS, "newName", NULL);
    if (!oldn || !newn) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "csv.renameColumn: 'oldName' and 'newName' are required");
        goto done;
    }
    int oi = csv_view_header_index(&t, oldn);
    if (oi < 0) {
        JS_ThrowTypeError(ctx, "csv.renameColumn: no such column '%s'", oldn);
        goto done;
    }
    if (strcmp(oldn, newn) != 0) {
        if (csv_view_header_index(&t, newn) >= 0) {
            JS_ThrowTypeError(ctx, "csv.renameColumn: column '%s' already exists", newn);
            goto done;
        }
        csv_overlay_init(&ov);
        ov.set_row = 0;
        ov.set_col = (uint32_t)oi;
        ov.set_val = newn;
        ov.set_len = strlen(newn);
        if (csv_view_store(ctx, path, &t, &ov, opt_durable(ctx, OPTS)))
            goto done;
    }
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "oldName", JS_NewString(ctx, oldn), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "newName", JS_NewString(ctx, newn), JS_PROP_C_W_E);
done:
    free(oldn);
    free(newn);
    csv_view_free(&t);
    free(path);
    return ret;
}

int range_window(JSContext* ctx, JSValueConst obj, size_t total,
    size_t defcap, int single_default, const char* pfx,
    size_t* ps, size_t* pe)
{
    int64_t start = 0, end = 0, offset = 0, limit = -1, maxrows = -1;
    int has_start, has_end, has_offset, has_limit, has_max;
    if (opt_int(ctx, obj, "start", 0, &start, &has_start))
        return -1;
    if (opt_int(ctx, obj, "end", 0, &end, &has_end))
        return -1;
    if (opt_int(ctx, obj, "offset", 0, &offset, &has_offset))
        return -1;
    if (opt_int(ctx, obj, "limit", -1, &limit, &has_limit))
        return -1;
    if ((has_start || has_end) && (has_offset || has_limit)) {
        JS_ThrowTypeError(ctx, "%s: use either {start, end} or {offset, limit} -- not both", pfx);
        return -1;
    }
    if (has_offset) {
        start = offset;
        has_start = 1;
    }
    if (has_limit) {
        if (limit < 0)
            has_end = 0;
        else {
            has_end = 1;
            end = (uint64_t)limit <= (uint64_t)INT64_MAX - (uint64_t)start
                ? start + limit
                : (int64_t)INT64_MAX;
        }
    }
    if (opt_int(ctx, obj, "maxRows", -1, &maxrows, &has_max))
        return -1;
    if (has_max && maxrows < 0) {
        JS_ThrowRangeError(ctx, "%s: maxRows must not be negative", pfx);
        return -1;
    }
    size_t cap = has_max ? (size_t)maxrows : defcap;
    if (start < 0)
        start = 0;
    if (has_end && end > start && (uint64_t)(end - start) > (uint64_t)cap) {
        JS_ThrowRangeError(ctx, "%s: requested window %lld exceeds the maximum of %zu rows (raise it with maxRows)", pfx, (long long)(end - start), cap);
        return -1;
    }
    size_t s = (size_t)start > total ? total : (size_t)start;
    size_t e;
    if (has_end) {
        e = (end < 0) ? 0 : (size_t)end;
    } else if (single_default)
        e = s + 1;
    else
        e = total;
    if (e > total)
        e = total;
    if (e < s)
        e = s;
    *ps = s;
    *pe = e;
    return 0;
}

static JSValue js_csv_select_column_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.selectColumnRange", opts_select_keys, countof(opts_select_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    char** cols = NULL;
    int ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    JSValue ret = JS_EXCEPTION;
    if (ncsel < 0) {
        csv_view_free(&t);
        free(path);
        return JS_EXCEPTION;
    }
    if (ncsel == 0) {
        JS_ThrowTypeError(ctx, "csv.selectColumnRange: 'columns' must be non-empty");
        goto done;
    }
    int* idx = (int*)malloc(ncsel * sizeof(int));
    if (!idx) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (int k = 0; k < ncsel; k++) {
        int ci = csv_view_header_index(&t, cols[k]);
        if (ci < 0) {
            free(idx);
            JS_ThrowTypeError(ctx, "csv.selectColumnRange: no such column '%s'", cols[k]);
            goto done;
        }
        idx[k] = ci;
    }
    size_t s, e;
    if (range_window(ctx, OPTS, csv_view_nrows(&t) - 1, 100, 0, "csv.selectColumnRange", &s, &e)) {
        free(idx);
        goto done;
    }
    JSValue cjs = JS_NewArray(ctx);
    for (int k = 0; k < ncsel; k++)
        JS_DefinePropertyValueUint32(ctx, cjs, (uint32_t)k, JS_NewString(ctx, cols[k]), JS_PROP_C_W_E);
    JSValue rjs = csv_view_rows(ctx, &t, idx, (size_t)ncsel, s + 1, e + 1);
    free(idx);
    if (JS_IsException(rjs)) {
        JS_FreeValue(ctx, cjs);
        goto done;
    }
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "columns", cjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "rows", rjs, JS_PROP_C_W_E);
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    csv_view_free(&t);
    free(path);
    return ret;
}

static JSValue csv_js_string(csv_js_sink* js, size_t c, const char* p, size_t n, int stable)
{
    JSValue v;
    if (c < CSV_REPEAT_COLS && js->last_v && !JS_IsUndefined(js->last_v[c])
        && js->last_n[c] == n && (n == 0 || memcmp(js->last_p[c], p, n) == 0))
        return JS_DupValue(js->ctx, js->last_v[c]);
    v = JS_NewStringLen(js->ctx, p ? p : "", n);
    if (js->last_v && c < CSV_REPEAT_COLS && !JS_IsException(v)) {
        JS_FreeValue(js->ctx, js->last_v[c]);
        js->last_v[c] = stable ? JS_DupValue(js->ctx, v) : JS_UNDEFINED;
        js->last_p[c] = p;
        js->last_n[c] = n;
    }
    return v;
}

int csv_js_field(csv_sink* k, const char* p, size_t n, int stable)
{
    csv_js_sink* js = (csv_js_sink*)k;
    JSValue v;
    if (js->width_known && js->nv >= js->width)
        return 0;
    if (js->nv == js->cap) {
        size_t nc = js->cap ? js->cap * 2 : 16;
        JSValue* nvals = (JSValue*)realloc(js->vals, nc * sizeof(JSValue));
        if (!nvals)
            return -1;
        js->vals = nvals;
        js->cap = nc;
    }
    v = csv_js_string(js, js->nv, p, n, stable);
    if (JS_IsException(v))
        return -1;
    js->vals[js->nv++] = v;
    return 0;
}

static const char* csv_num_text(JSValueConst v, char* buf, JSDTOATempMem* tmp)
{
    int len;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        len = (int)i32toa(buf, JS_VALUE_GET_INT(v));
    } else if (JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64) {
        len = js_dtoa(buf, JS_VALUE_GET_FLOAT64(v), 10, 0, JS_DTOA_FORMAT_FREE, tmp);
    } else {
        return NULL;
    }
    buf[len] = 0;
    return buf;
}

static JSValue js_csv_stringify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char numbuf[64];
    uint8_t cls[256];
    JSDTOATempMem dtoa_tmp;
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "csv.stringify: 'rows' must be an array");
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (opts_check(ctx, opts, "csv.stringify", opts_stringify_keys, countof(opts_stringify_keys)))
        return JS_EXCEPTION;
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, opts, "csv.stringify", &delim, &quote))
        return JS_EXCEPTION;
    int write_header = opt_bool_def(ctx, opts, "hasHeader", 1);
    emit_classes(cls, delim, quote);

    uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);

    Buf out;
    memset(&out, 0, sizeof(out));
    char** names = NULL;
    JSAtom* col_atoms = NULL;
    JSPropCache* col_pcs = NULL;
    uint32_t ncols = 0;
    int obj_form = 0;
    JSValue ret = JS_EXCEPTION;

    if (n) {
        JSValue r0 = JS_GetPropertyUint32(ctx, argv[0], 0);
        if (JS_IsArray(ctx, r0))
            obj_form = 0;
        else if (JS_IsObject(r0))
            obj_form = 1;
        else {
            JS_FreeValue(ctx, r0);
            buf_free(&out);
            return JS_ThrowTypeError(ctx, "csv.stringify: row 0 must be an array or an object");
        }
        JS_FreeValue(ctx, r0);

        if (obj_form) {
            JSValue first = JS_GetPropertyUint32(ctx, argv[0], 0);
            JSPropertyEnum* tab = NULL;
            uint32_t ntab = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, first, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, first);
                buf_free(&out);
                return JS_EXCEPTION;
            }
            names = ntab ? (char**)calloc(ntab, sizeof(char*)) : NULL;
            col_atoms = ntab ? (JSAtom*)calloc(ntab, sizeof(JSAtom)) : NULL;
            col_pcs = ntab ? (JSPropCache*)calloc(ntab, sizeof(JSPropCache)) : NULL;
            if (ntab && (!names || !col_atoms || !col_pcs)) {
                JS_FreePropertyEnum(ctx, tab, ntab);
                JS_FreeValue(ctx, first);
                JS_ThrowOutOfMemory(ctx);
                goto err;
            }
            for (uint32_t i = 0; i < ntab; i++) {
                const char* nm = JS_AtomToCString(ctx, tab[i].atom);
                if (!nm) {
                    JS_FreePropertyEnum(ctx, tab, ntab);
                    JS_FreeValue(ctx, first);
                    goto err;
                }
                names[ncols] = strdup(nm);
                JS_FreeCString(ctx, nm);
                if (!names[ncols]) {
                    JS_FreePropertyEnum(ctx, tab, ntab);
                    JS_FreeValue(ctx, first);
                    JS_ThrowOutOfMemory(ctx);
                    goto err;
                }
                col_atoms[ncols] = JS_DupAtom(ctx, tab[i].atom);
                ncols++;
            }
            JS_FreePropertyEnum(ctx, tab, ntab);
            JS_FreeValue(ctx, first);
            if (write_header) {
                for (uint32_t c = 0; c < ncols; c++) {
                    if ((c && buf_putc(&out, (char)delim)) || emit_field(&out, names[c], strlen(names[c]), delim, quote)) {
                        JS_ThrowOutOfMemory(ctx);
                        goto err;
                    }
                }
                if (buf_putc(&out, '\n')) {
                    JS_ThrowOutOfMemory(ctx);
                    goto err;
                }
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        JSValue rv = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsArray(ctx, rv)) {
            if (obj_form) {
                JS_FreeValue(ctx, rv);
                JS_ThrowTypeError(ctx, "csv.stringify: row %u: rows must all be objects keyed by header (row 0 was an object)", i);
                goto err;
            }
            uint32_t rc = 0;
            JSValue rl = JS_GetPropertyStr(ctx, rv, "length");
            if (JS_ToUint32(ctx, &rc, rl) < 0) {
                JS_FreeValue(ctx, rl);
                JS_FreeValue(ctx, rv);
                goto err;
            }
            JS_FreeValue(ctx, rl);
            if (rc > CSV_TEXT_MAX) {
                JS_FreeValue(ctx, rv);
                JS_ThrowRangeError(ctx, "csv.stringify: row %u has %u cells, more than any string can hold", i, rc);
                goto err;
            }
            for (uint32_t c = 0; c < rc; c++) {
                JSValue cv = JS_GetPropertyUint32(ctx, rv, c);
                const char* s = NULL;
                size_t slen = 0;
                int owned = 0;
                if (JS_IsException(cv)) {
                    JS_FreeValue(ctx, rv);
                    goto err;
                }
                if (JS_VALUE_GET_TAG(cv) == JS_TAG_STRING) {
                    int fr;
                    if (c && buf_putc(&out, (char)delim))
                        fr = -1;
                    else if ((fr = emit_ascii_field(ctx, &out, cv, cls, quote)) > 0 && c)
                        out.len--;
                    if (fr <= 0) {
                        JS_FreeValue(ctx, cv);
                        if (fr < 0) {
                            JS_FreeValue(ctx, rv);
                            JS_ThrowOutOfMemory(ctx);
                            goto err;
                        }
                        if (out.len > CSV_TEXT_MAX) {
                            JS_FreeValue(ctx, rv);
                            JS_ThrowRangeError(ctx, "csv.stringify: the output exceeds the maximum string length");
                            goto err;
                        }
                        continue;
                    }
                }
                if (!JS_IsUndefined(cv)) {
                    s = csv_num_text(cv, numbuf, &dtoa_tmp);
                    owned = !s;
                    if (owned)
                        s = JS_ToCStringLen(ctx, &slen, cv);
                    else
                        slen = strlen(s);
                }
                JS_FreeValue(ctx, cv);
                if (owned && !s) {
                    JS_FreeValue(ctx, rv);
                    goto err;
                }
                int w = (c && buf_putc(&out, (char)delim)) || (s && emit_field(&out, s, slen, delim, quote));
                if (owned)
                    JS_FreeCString(ctx, s);
                if (w) {
                    JS_FreeValue(ctx, rv);
                    JS_ThrowOutOfMemory(ctx);
                    goto err;
                }
                if (out.len > CSV_TEXT_MAX) {
                    JS_FreeValue(ctx, rv);
                    JS_ThrowRangeError(ctx, "csv.stringify: the output exceeds the maximum string length");
                    goto err;
                }
            }
        } else if (JS_IsObject(rv)) {
            if (!obj_form) {
                JS_FreeValue(ctx, rv);
                JS_ThrowTypeError(ctx, "csv.stringify: row %u: rows must all be arrays (row 0 was an array)", i);
                goto err;
            }
            for (uint32_t c = 0; c < ncols; c++) {
                JSValue v;
                if (JS_GetOwnDataCached(ctx, rv, col_atoms[c], &col_pcs[c], &v) < 0) {
                    JS_FreeValue(ctx, rv);
                    goto err;
                }
                const char* s = NULL;
                size_t slen = 0;
                int owned = 0;

                if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING) {
                    int fr;
                    if (c && buf_putc(&out, (char)delim))
                        fr = -1;
                    else if ((fr = emit_ascii_field(ctx, &out, v, cls, quote)) > 0 && c)
                        out.len--;
                    if (fr <= 0) {
                        JS_FreeValue(ctx, v);
                        if (fr < 0) {
                            JS_FreeValue(ctx, rv);
                            JS_ThrowOutOfMemory(ctx);
                            goto err;
                        }
                        continue;
                    }
                }
                if (!JS_IsUndefined(v)) {
                    s = csv_num_text(v, numbuf, &dtoa_tmp);
                    if (!s) {
                        s = JS_ToCStringLen(ctx, &slen, v);
                        owned = 1;
                    } else
                        slen = strlen(s);
                    if (!s) {
                        JS_FreeValue(ctx, v);
                        JS_FreeValue(ctx, rv);
                        goto err;
                    }
                }
                int w = (c && buf_putc(&out, (char)delim)) || (s && emit_field(&out, s, slen, delim, quote));
                if (owned)
                    JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                if (w) {
                    JS_FreeValue(ctx, rv);
                    JS_ThrowOutOfMemory(ctx);
                    goto err;
                }
            }
        } else {
            JS_FreeValue(ctx, rv);
            JS_ThrowTypeError(ctx, "csv.stringify: row %u must be an array or an object", i);
            goto err;
        }
        JS_FreeValue(ctx, rv);
        if (buf_putc(&out, '\n')) {
            JS_ThrowOutOfMemory(ctx);
            goto err;
        }
        if (out.len > CSV_TEXT_MAX) {
            JS_ThrowRangeError(ctx, "csv.stringify: the output exceeds the maximum string length");
            goto err;
        }
    }

    ret = JS_NewStringLen(ctx, out.p ? out.p : "", out.len);
err:
    for (uint32_t c = 0; c < ncols; c++) {
        JS_FreeAtom(ctx, col_atoms[c]);
        JS_FreePropCache(JS_GetRuntime(ctx), &col_pcs[c]);
    }
    free(col_atoms);
    free(col_pcs);
    free_str_array(names, (int)ncols);
    buf_free(&out);
    return ret;
}

JSClassID csv_rows_class_id;

static const JSClassDef csv_rows_class = { "CsvRows", .finalizer = csv_rows_finalizer };

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
    JS_CFUNC_DEF("rows", 1, js_csv_rows),
    JS_CFUNC_DEF("toDataFrame", 1, js_csv_to_dataframe),
};

static const JSCFunctionListEntry csv_free_funcs[] = {
    JS_CFUNC_DEF("parse", 2, js_csv_parse),
    JS_CFUNC_DEF("stringify", 2, js_csv_stringify),
    JS_CFUNC_DEF("parseDataFrame", 2, js_csv_parse_dataframe),
    JS_CFUNC_DEF("index", 2, js_csv_index),
};

static int csv_module_init(JSContext* ctx, JSModuleDef* m)
{
    JS_NewClassID(&csv_rows_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), (JSClassID)csv_rows_class_id, &csv_rows_class) < 0)
        return -1;
    if (csv_index_class_init(ctx))
        return -1;
    if (dyn_register_class(ctx, m, &dyn_csvfile_class_id, &dyn_csvfile_class,
            csvfile_methods, countof(csvfile_methods),
            js_csvfile_ctor, "CSVFile"))
        return -1;
    return JS_SetModuleExportList(ctx, m, csv_free_funcs, countof(csv_free_funcs));
}

int js_nat_init_csv(JSContext* ctx)
{
    simd_init();
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:csv", csv_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "CSVFile");
    JS_AddModuleExportList(ctx, m, csv_free_funcs, countof(csv_free_funcs));
    return 0;
}

#endif
