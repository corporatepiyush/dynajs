#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

static const char* const opts_add_row_keys[] = { "rows", "strict", "durable" };

static JSValue csv_own_get(JSContext* ctx, JSValueConst obj, const char* key)
{
    JSAtom a = JS_NewAtom(ctx, key);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &desc, obj, a);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

JSValue js_csv_add_row(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    char* path = csvfile_path(ctx, this_val);
    if (!path)
        return JS_EXCEPTION;
    int bare = argc >= 1 && JS_IsArray(ctx, argv[0]);
    JSValue rows;
    if (bare) {
        rows = JS_NewArray(ctx);
        if (JS_IsException(rows)) {
            free(path);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueUint32(ctx, rows, 0, JS_DupValue(ctx, argv[0]), JS_PROP_C_W_E);
    } else {
        if (need_obj(ctx, argc, argv)) {
            free(path);
            return JS_EXCEPTION;
        }
        if (opts_check(ctx, OPTS, "csv.addRow", opts_add_row_keys, countof(opts_add_row_keys))) {
            free(path);
            return JS_EXCEPTION;
        }
        rows = JS_GetPropertyStr(ctx, OPTS, "rows");
    }
    CsvView t;
    CsvOverlay ov;
    Buf out;
    char** hn = NULL;
    size_t ncols, nold, c;
    JSValue ret = JS_EXCEPTION;
    uint32_t nr = 0;
    memset(&out, 0, sizeof(out));
    if (csv_view_load(ctx, path, &t, bare ? 0 : opt_bool(ctx, OPTS, "strict"), ',', '"', 1)) {
        JS_FreeValue(ctx, rows);
        free(path);
        return JS_EXCEPTION;
    }
    ncols = csv_view_ncols(&t);
    nold = csv_view_nrows(&t) - 1;
    csv_overlay_init(&ov);
    hn = (char**)calloc(ncols ? ncols : 1, sizeof(char*));
    if (!hn || csv_view_serialize(&t, &ov, &out)) {
        csv_view_free(&t);
        JS_FreeValue(ctx, rows);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (c = 0; c < ncols; c++) {
        const uint8_t* p;
        size_t n;
        if (csv_view_field(&t, 0, (uint32_t)c, &p, &n) == 0) {
            hn[c] = (char*)malloc(n + 1);
            if (hn[c]) {
                memcpy(hn[c], p, n);
                hn[c][n] = 0;
            }
        }
        if (!hn[c]) {
            csv_view_free(&t);
            JS_FreeValue(ctx, rows);
            if (!JS_HasException(ctx))
                JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    csv_view_free(&t);

    if (!JS_IsArray(ctx, rows)) {
        JS_FreeValue(ctx, rows);
        JS_ThrowTypeError(ctx, "csv.addRow: 'rows' must be an array");
        goto done;
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length");
        int urc = JS_ToUint32(ctx, &nr, lv);
        JS_FreeValue(ctx, lv);
        if (urc < 0) {
            JS_FreeValue(ctx, rows);
            goto done;
        }
    }

    for (uint32_t i = 0; i < nr; i++) {
        JSValue rv = JS_GetPropertyUint32(ctx, rows, i);
        int isarr = JS_IsArray(ctx, rv);
        uint32_t rc = 0;
        if (!isarr && !JS_IsObject(rv)) {
            JS_FreeValue(ctx, rv);
            JS_FreeValue(ctx, rows);
            JS_ThrowTypeError(ctx, "csv.addRow: each row must be an array or an object");
            goto done;
        }
        if (isarr) {
            JSValue rl = JS_GetPropertyStr(ctx, rv, "length");
            if (JS_ToUint32(ctx, &rc, rl) < 0) {
                JS_FreeValue(ctx, rl);
                JS_FreeValue(ctx, rv);
                JS_FreeValue(ctx, rows);
                goto done;
            }
            JS_FreeValue(ctx, rl);
        }
        for (c = 0; c < ncols; c++) {
            JSValue cv = JS_UNDEFINED;
            const char* cs = NULL;
            size_t cslen = 0;
            int bad;
            if (isarr) {
                if (c < rc) {
                    cv = JS_GetPropertyUint32(ctx, rv, (uint32_t)c);
                    int nested = JS_IsException(cv) ? -1 : bare ? JS_IsArray(ctx, cv)
                                                                : 0;
                    if (nested) {
                        JS_FreeValue(ctx, cv);
                        if (nested > 0)
                            JS_ThrowTypeError(ctx, "csv.addRow: a bare array is one positional row -- pass {rows: [...]} to add several");
                        JS_FreeValue(ctx, rv);
                        JS_FreeValue(ctx, rows);
                        goto done;
                    }
                    cs = JS_ToCStringLen(ctx, &cslen, cv);
                    JS_FreeValue(ctx, cv);
                    if (!cs) {
                        JS_FreeValue(ctx, rv);
                        JS_FreeValue(ctx, rows);
                        goto done;
                    }
                }
            } else {
                cv = csv_own_get(ctx, rv, hn[c]);
                if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                    cs = JS_ToCStringLen(ctx, &cslen, cv);
                    if (!cs) {
                        JS_FreeValue(ctx, cv);
                        JS_FreeValue(ctx, rv);
                        JS_FreeValue(ctx, rows);
                        goto done;
                    }
                }
                JS_FreeValue(ctx, cv);
            }
            bad = (c && buf_putc(&out, ',')) || (cs && csv_emit_field_n(&out, (const uint8_t*)cs, cslen));
            if (cs)
                JS_FreeCString(ctx, cs);
            if (bad) {
                JS_FreeValue(ctx, rv);
                JS_FreeValue(ctx, rows);
                JS_ThrowOutOfMemory(ctx);
                goto done;
            }
        }
        JS_FreeValue(ctx, rv);
        if (buf_putc(&out, '\n')) {
            JS_FreeValue(ctx, rows);
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    JS_FreeValue(ctx, rows);
    if (csv_write_buf(ctx, path, &out, bare ? 0 : opt_durable(ctx, OPTS)))
        goto done;
    ret = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ret, "added", JS_NewInt64(ctx, nr), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)(nold + nr)), JS_PROP_C_W_E);
done:
    if (hn)
        for (c = 0; c < ncols; c++)
            free(hn[c]);
    free(hn);
    buf_free(&out);
    free(path);
    return ret;
}

int csv_js_row(csv_sink* k)
{
    csv_js_sink* js = (csv_js_sink*)k;
    JSValue a;
    size_t c;
    if (!js->width_known) {
        js->width = js->nv;
        js->width_known = 1;
        if (js->width > (size_t)INT32_MAX)
            return -1;
        js->last_v = (JSValue*)malloc(CSV_REPEAT_COLS * sizeof(JSValue));
        js->last_p = (const char**)malloc(CSV_REPEAT_COLS * sizeof(char*));
        js->last_n = (size_t*)malloc(CSV_REPEAT_COLS * sizeof(size_t));
        if (!js->last_v || !js->last_p || !js->last_n) {
            free(js->last_v);
            free(js->last_p);
            free(js->last_n);
            js->last_v = NULL;
            js->last_p = NULL;
            js->last_n = NULL;
        } else {
            for (c = 0; c < CSV_REPEAT_COLS; c++)
                js->last_v[c] = JS_UNDEFINED;
        }
    }
    while (js->nv < js->width)
        if (csv_js_field(k, NULL, 0, 0))
            return -1;
    a = JS_NewArrayFrom(js->ctx, (int)js->width, js->vals);
    js->nv = 0;
    if (JS_IsException(a))
        return -1;
    if (js->has_header && k->rows == 0) {
        js->header = a;
        return 0;
    }
    return JS_DefinePropertyValueUint32(js->ctx, js->rows, js->k++, a, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

#endif
