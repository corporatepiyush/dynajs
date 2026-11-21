#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

static const char* const opts_read_keys[] = { "offset", "limit", "columns", "strict", "delimiter", "quote" };

static const char* const opts_range_keys[] = { "column", "start", "end", "offset", "limit", "maxRows", "strict" };

static const char* const opts_row_range_keys[] = { "start", "end", "offset", "limit", "maxRows", "strict" };

JSValue js_csv_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (opts_check(ctx, OPTS, "csv.read", opts_read_keys, countof(opts_read_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, OPTS, "csv.read", &delim, &quote)) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), delim, quote, 1)) {
        free(path);
        return JS_EXCEPTION;
    }

    char** cols = NULL;
    int ncsel = 0;
    JSValue ret = JS_EXCEPTION;
    int64_t offset = 0, limit = -1;
    int has_limit = 0;
    if (opt_int(ctx, OPTS, "offset", 0, &offset, NULL))
        goto done;
    if (opt_int(ctx, OPTS, "limit", -1, &limit, &has_limit))
        goto done;
    if (offset < 0)
        offset = 0;
    ncsel = opt_str_array(ctx, OPTS, "columns", &cols);
    if (ncsel < 0)
        goto done;

    size_t ncols = csv_view_ncols(&t);
    size_t total = csv_view_nrows(&t) - 1;

    size_t nproj = ncsel > 0 ? (size_t)ncsel : ncols;
    int* idx = (int*)malloc((nproj ? nproj : 1) * sizeof(int));
    if (!idx) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    JSValue hjs = JS_NewArray(ctx);
    for (size_t k = 0; k < nproj; k++) {
        if (ncsel > 0) {
            int ci = csv_view_header_index(&t, cols[k]);
            if (ci < 0) {
                free(idx);
                JS_FreeValue(ctx, hjs);
                JS_ThrowTypeError(ctx, "csv.read: no such column '%s'", cols[k]);
                goto done;
            }
            idx[k] = ci;
        } else
            idx[k] = (int)k;
        JS_DefinePropertyValueUint32(ctx, hjs, (uint32_t)k, csv_view_cell(ctx, &t, 0, (size_t)idx[k]), JS_PROP_C_W_E);
    }

    size_t start = (size_t)offset;
    if (start > total)
        start = total;
    size_t end = total;
    if (has_limit && limit >= 0 && start + (size_t)limit < end)
        end = start + (size_t)limit;

    JSValue rjs = csv_view_rows(ctx, &t, idx, nproj, start + 1, end + 1);
    free(idx);
    if (JS_IsException(rjs)) {
        JS_FreeValue(ctx, hjs);
        goto done;
    }

    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "headers", hjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "rows", rjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)total), JS_PROP_C_W_E);
done:
    free_str_array(cols, ncsel > 0 ? ncsel : 0);
    csv_view_free(&t);
    free(path);
    return ret;
}

JSValue js_csv_read_column_values_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.readColumnValuesRange", opts_range_keys, countof(opts_range_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EXCEPTION;
    char* col = opt_str(ctx, OPTS, "column", NULL);
    if (!col) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "csv.readColumnValuesRange: 'column' is required");
        goto done;
    }
    int ci = csv_view_header_index(&t, col);
    if (ci < 0) {
        JS_ThrowTypeError(ctx, "csv.readColumnValuesRange: no such column '%s'", col);
        goto done;
    }
    size_t s, e;
    if (range_window(ctx, OPTS, csv_view_nrows(&t) - 1, 1000, 0, "csv.readColumnValuesRange", &s, &e))
        goto done;
    JSValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    for (size_t r = s; r < e; r++)
        JS_DefinePropertyValueUint32(ctx, a, k++, csv_view_cell(ctx, &t, r + 1, (size_t)ci), JS_PROP_C_W_E);
    ret = a;
done:
    free(col);
    csv_view_free(&t);
    free(path);
    return ret;
}

JSValue js_csv_read_row_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsObject(argv[0])) {
        JS_ThrowTypeError(ctx, "csv.readRowRange: expected an options object "
                               "like { start, end } -- positional arguments are "
                               "not read");
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.readRowRange", opts_row_range_keys, countof(opts_row_range_keys))) {
        free(path);
        return JS_EXCEPTION;
    }
    CsvView t;
    if (csv_view_load(ctx, path, &t, opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        free(path);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_EXCEPTION;
    size_t ncols = csv_view_ncols(&t);
    size_t s, e;
    if (range_window(ctx, OPTS, csv_view_nrows(&t) - 1, 100, 1, "csv.readRowRange", &s, &e))
        goto done;
    JSValue hjs = JS_NewArray(ctx);
    for (size_t c = 0; c < ncols; c++)
        JS_DefinePropertyValueUint32(ctx, hjs, (uint32_t)c, csv_view_cell(ctx, &t, 0, c), JS_PROP_C_W_E);
    JSValue rjs = csv_view_rows(ctx, &t, NULL, ncols, s + 1, e + 1);
    if (JS_IsException(rjs)) {
        JS_FreeValue(ctx, hjs);
        goto done;
    }
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "headers", hjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "rows", rjs, JS_PROP_C_W_E);
done:
    csv_view_free(&t);
    free(path);
    return ret;
}

#endif
