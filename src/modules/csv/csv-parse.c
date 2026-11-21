#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

int csv_scan_sink(const uint8_t* buf, size_t len, csv_sink* k, int strict,
    int* errrow, uint8_t delim, uint8_t quote)
{
    return csvx_scan(buf, len, k, strict, errrow, delim, quote);
}

int csv_parse_ex(const uint8_t* buf, size_t len, Table* t, int strict,
    int* errrow, uint8_t delim, uint8_t quote)
{
    return csvx_parse_table(buf, len, t, strict, errrow, delim, quote);
}

__attribute__((unused)) static int csv_parse(const uint8_t* buf, size_t len,
    Table* t)
{
    int rr;
    return csv_parse_ex(buf, len, t, 0, &rr, ',', '"') < 0 ? -1 : 0;
}

static const char* const opts_parse_keys[] = { "delimiter", "quote", "hasHeader", "strict" };

static void csv_js_sink_free(csv_js_sink* js)
{
    size_t c;
    for (c = 0; c < js->nv; c++)
        JS_FreeValue(js->ctx, js->vals[c]);
    free(js->vals);
    if (js->last_v)
        for (c = 0; c < CSV_REPEAT_COLS; c++)
            JS_FreeValue(js->ctx, js->last_v[c]);
    free(js->last_v);
    free(js->last_p);
    free(js->last_n);
}

JSValue js_csv_parse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx, "csv.parse: expected CSV text");
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (opts_check(ctx, opts, "csv.parse", opts_parse_keys, countof(opts_parse_keys)))
        return JS_EXCEPTION;
    uint8_t delim, quote;
    if (opts_delim_quote(ctx, opts, "csv.parse", &delim, &quote))
        return JS_EXCEPTION;
    int strict = opt_bool(ctx, opts, "strict");
    int has_header = opt_bool_def(ctx, opts, "hasHeader", 1);

    size_t tlen = 0;
    const char* text = NULL;
    JSValue tstr = JS_ToString(ctx, argv[0]);
    if (JS_IsException(tstr))
        return JS_EXCEPTION;
    const uint8_t* data = csv_narrow_ascii(ctx, tstr, &tlen);
    if (!data) {
        text = JS_ToCStringLen(ctx, &tlen, tstr);
        if (!text) {
            JS_FreeValue(ctx, tstr);
            return JS_EXCEPTION;
        }
        data = (const uint8_t*)text;
    }
    size_t dlen = tlen;
    if (dlen >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        dlen -= 3;
    }
    {
        JSValue fh = JS_UNDEFINED, fr = JS_UNDEFINED;
        int64_t ftotal = 0;
        int fr_rc = csv_parse_fast(ctx, data, dlen, delim, quote, has_header, &fh, &fr, &ftotal);
        if (fr_rc <= 0) {
            if (text)
                JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, tstr);
            if (fr_rc < 0)
                return JS_EXCEPTION;
            JSValue fret = JS_NewObject(ctx);
            if (JS_IsException(fret)) {
                JS_FreeValue(ctx, fh);
                JS_FreeValue(ctx, fr);
                return fret;
            }
            JS_DefinePropertyValueStr(ctx, fret, "headers", JS_IsUndefined(fh) ? JS_NewArray(ctx) : fh, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, fret, "rows", fr, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, fret, "totalRows", JS_NewInt64(ctx, ftotal), JS_PROP_C_W_E);
            return fret;
        }
    }
    csv_js_sink js;
    memset(&js, 0, sizeof(js));
    js.base.field = csv_js_field;
    js.base.row = csv_js_row;
    js.ctx = ctx;
    js.has_header = has_header;
    js.header = JS_UNDEFINED;
    js.rows = JS_NewArray(ctx);
    if (JS_IsException(js.rows)) {
        if (text)
            JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, tstr);
        return JS_EXCEPTION;
    }
    int errrow = 0;
    int r = csvx_scan(data, dlen, &js.base, strict, &errrow, delim, quote);
    csv_js_sink_free(&js);
    if (text)
        JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, tstr);
    if (r != 0) {
        JS_FreeValue(ctx, js.rows);
        JS_FreeValue(ctx, js.header);
        if (r == CSV_SYNTAX_QUOTE)
            return JS_ThrowSyntaxError(ctx, "csv.parse: row %d: unterminated quoted field", errrow);
        if (r == CSV_SYNTAX_GARBAGE)
            return JS_ThrowSyntaxError(ctx, "csv.parse: row %d: unexpected text after a closing quote", errrow);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
    }
    JSValue hjs = JS_IsUndefined(js.header) ? JS_NewArray(ctx) : js.header;
    if (JS_IsException(hjs)) {
        JS_FreeValue(ctx, js.rows);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_NewObject(ctx);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, hjs);
        JS_FreeValue(ctx, js.rows);
        return ret;
    }
    JS_DefinePropertyValueStr(ctx, ret, "headers", hjs, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "rows", js.rows, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ret, "totalRows", JS_NewInt64(ctx, (int64_t)js.k), JS_PROP_C_W_E);
    return ret;
}

#endif
