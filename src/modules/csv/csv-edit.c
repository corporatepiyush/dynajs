#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

static const char* const opts_remove_row_keys[] = { "row", "strict", "durable" };

static const char* const opts_remove_col_keys[] = { "column", "columnIndex", "strict", "durable" };

static CsvxView csv_xview(CsvView* v)
{
    CsvxView x;
    x.ix = &v->ix;
    x.tmp = v->tmp;
    x.tcap = v->tcap;
    return x;
}

static void csv_xview_back(CsvView* v, const CsvxView* x)
{
    v->tmp = x->tmp;
    v->tcap = x->tcap;
}

int csv_emit_field_n(Buf* b, const uint8_t* s, size_t n)
{
    return csvx_emit_field_n(b, s, n);
}

int csv_view_field(CsvView* v, size_t r, uint32_t c, const uint8_t** pp, size_t* pn)
{
    CsvxView x = csv_xview(v);
    int rc = csvx_view_field(&x, r, c, pp, pn);
    csv_xview_back(v, &x);
    return rc;
}

void csv_overlay_init(CsvOverlay* o)
{
    csvx_overlay_init(o);
}

int csv_view_serialize(CsvView* v, const CsvOverlay* ov, Buf* out)
{
    CsvxView x = csv_xview(v);
    int rc = csvx_view_serialize(&x, ov, out);
    csv_xview_back(v, &x);
    return rc;
}

int csv_write_buf(JSContext* ctx, const char* path, const Buf* out, int durable)
{
    if (dyn_io_write_whole_atomic(path, out->p ? out->p : "", out->len, durable) < 0) {
        JS_ThrowTypeError(ctx, "csv: cannot write '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int csv_view_store(JSContext* ctx, const char* path, CsvView* v, const CsvOverlay* ov, int durable)
{
    Buf out;
    int r;
    memset(&out, 0, sizeof(out));
    if (csv_view_serialize(v, ov, &out)) {
        buf_free(&out);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    r = csv_write_buf(ctx, path, &out, durable);
    buf_free(&out);
    return r;
}

int csv_view_resolve_column(JSContext* ctx, JSValueConst obj, CsvView* v, const char* pfx)
{
    int present;
    char* name = opt_str(ctx, obj, "column", &present);
    if (name) {
        int ci = csv_view_header_index(v, name);
        if (ci < 0)
            JS_ThrowTypeError(ctx, "%s: no such column '%s'", pfx, name);
        free(name);
        return ci;
    }
    if (JS_HasException(ctx))
        return -1;
    int64_t ix;
    int has;
    if (opt_int(ctx, obj, "columnIndex", 0, &ix, &has))
        return -1;
    if (has) {
        if (ix < 0 || (size_t)ix >= csv_view_ncols(v)) {
            JS_ThrowRangeError(ctx, "%s: columnIndex %lld out of range", pfx, (long long)ix);
            return -1;
        }
        return (int)ix;
    }
    JS_ThrowTypeError(ctx, "%s: provide 'column' or 'columnIndex'", pfx);
    return -1;
}

JSValue js_csv_remove_row(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.removeRow", opts_remove_row_keys, countof(opts_remove_row_keys))) {
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
            JS_ThrowTypeError(ctx, "csv.removeRow: 'row' is required");
        goto done;
    }
    if (rowi < 0 || (size_t)rowi >= csv_view_nrows(&t) - 1) {
        JS_ThrowRangeError(ctx, "csv.removeRow: row %lld out of range", (long long)rowi);
        goto done;
    }
    csv_overlay_init(&ov);
    ov.drop_row = (size_t)rowi + 1;
    if (csv_view_store(ctx, path, &t, &ov, opt_durable(ctx, OPTS)))
        goto done;
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "removed", JS_NewInt64(ctx, rowi), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(csv_view_nrows(&t) - 2)), JS_PROP_C_W_E);
done:
    csv_view_free(&t);
    free(path);
    return ret;
}

JSValue js_csv_remove_column(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    if (need_obj(ctx, argc, argv)) {
        free(path);
        return JS_EXCEPTION;
    }
    if (opts_check(ctx, OPTS, "csv.removeColumn", opts_remove_col_keys, countof(opts_remove_col_keys))) {
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
    int ci = csv_view_resolve_column(ctx, OPTS, &t, "csv.removeColumn");
    if (ci < 0)
        goto done;
    csv_overlay_init(&ov);
    ov.drop_col = (uint32_t)ci;
    if (csv_view_store(ctx, path, &t, &ov, opt_durable(ctx, OPTS)))
        goto done;
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "removedIndex", JS_NewInt64(ctx, ci), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalColumns", JS_NewInt64(ctx, (int64_t)csv_view_ncols(&t) - 1), JS_PROP_C_W_E);
done:
    csv_view_free(&t);
    free(path);
    return ret;
}

#endif
