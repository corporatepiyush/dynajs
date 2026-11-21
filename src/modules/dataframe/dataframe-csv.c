#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

typedef struct {
    char* p;
    size_t len, cap;
    int failed;
} DfBuf;

static void df_buf_put(DfBuf* b, const char* s, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char* np;
        while (nc < b->len + n + 1)
            nc *= 2;
        np = realloc(b->p, nc);
        if (!np) {
            b->failed = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void df_csv_field_esc(DfBuf* b, const char* s, int esc_formula)
{
    size_t i, n = strlen(s);
    int need = 0;

    int guard = esc_formula && n && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r');

    for (i = 0; i < n; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\r' || s[i] == '\n') {
            need = 1;
            break;
        }
    if (!need) {
        if (guard)
            df_buf_put(b, "'", 1);
        df_buf_put(b, s, n);
        return;
    }
    df_buf_put(b, "\"", 1);
    if (guard)
        df_buf_put(b, "'", 1);
    for (i = 0; i < n; i++) {
        if (s[i] == '"')
            df_buf_put(b, "\"\"", 2);
        else
            df_buf_put(b, s + i, 1);
    }
    df_buf_put(b, "\"", 1);
}

JSValue dyn_df_to_csv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfBuf b = { 0 };
    DFBound* bs;
    uint32_t r, c, n;
    JSValue res;

    int esc_formula = 0;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "escapeFormulas");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (JS_ToBool(ctx, v) == 1)
            esc_formula = 1;
        JS_FreeValue(ctx, v);
    }
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    for (c = 0; c < df->ncols; c++) {
        if (c)
            df_buf_put(&b, ",", 1);
        df_csv_field_esc(&b, df->cols[c].name, esc_formula);
    }
    df_buf_put(&b, "\n", 1);
    n = df->nrows;
    bs = (DFBound*)malloc((df->ncols ? df->ncols : 1) * sizeof(*bs));
    if (!bs) {
        free(b.p);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (c = 0; c < df->ncols; c++)
        if (dyn_df_bind(ctx, df, (int)c, &bs[c])) {
            free(bs);
            free(b.p);
            return JS_EXCEPTION;
        }
    for (r = 0; r < n; r++) {
        for (c = 0; c < df->ncols; c++) {
            if (c)
                df_buf_put(&b, ",", 1);
            if (df->cols[c].type == DF_STR) {
                int32_t code = ((const int32_t*)bs[c].p)[r];
                if (code < 0 || (uint32_t)code >= df->cols[c].dict_len) {
                    free(bs);
                    free(b.p);
                    return JS_ThrowRangeError(ctx, "TO_CSV: column '%s' code "
                                                   "%d out of range",
                        df->cols[c].name, code);
                }
                df_csv_field_esc(&b, df->cols[c].dict[code], esc_formula);
            } else {
                char tmp[32];
                double v = df_get(bs[c].p, bs[c].type, r);
                if (v != v) {
                    continue;
                } else {
                    df_fmt_double(tmp, sizeof(tmp), v);
                    df_buf_put(&b, tmp, strlen(tmp));
                }
            }
        }
        df_buf_put(&b, "\n", 1);
    }
    free(bs);
    if (b.failed) {
        free(b.p);
        return JS_ThrowOutOfMemory(ctx);
    }
    res = JS_NewStringLen(ctx, b.p ? b.p : "", b.len);
    free(b.p);
    return res;
}

#endif
