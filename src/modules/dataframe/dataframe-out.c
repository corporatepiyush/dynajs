#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

void* df_out_alloc(JSContext* ctx, uint32_t n, size_t esz)
{
    void* p;
    if (esz && n > SIZE_MAX / esz) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    p = malloc(n ? (size_t)n * esz : 1);
    if (!p)
        JS_ThrowOutOfMemory(ctx);
    return p;
}

static int df_out_bag_check(JSContext* ctx, JSValueConst o, const char* op)
{
    static const char* const keys[] = { "out" };
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < (int)countof(keys); k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == (int)countof(keys)) {
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\" (valid: out)",
                op, name);
            JS_FreeCString(ctx, name);
            bad = 1;
            break;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 1;
}

static int df_arg_is_bag(JSContext* ctx, JSValueConst v)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int isbag = 1;

    if (!JS_IsObject(v) || JS_IsArray(ctx, v))
        return 0;
    if (JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE)
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    for (i = 0; i < nprops && isbag; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (name && *name) {
            const char* c;
            int digits = 1;
            for (c = name; *c; c++)
                if (*c < '0' || *c > '9') {
                    digits = 0;
                    break;
                }
            if (digits)
                isbag = 0;
        }
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return isbag;
}

int df_is_out_bag(JSContext* ctx, JSValueConst v)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int has = 0;

    if (!df_arg_is_bag(ctx, v))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    for (i = 0; i < nprops && !has; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (name && strcmp(name, "out") == 0)
            has = 1;
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return has;
}

int df_out_opt(JSContext* ctx, const DataFrame* df, JSValueConst v,
    const char* op, int* pout_idx)
{
    JSValue ov;
    int idx, rc;

    rc = df_out_bag_check(ctx, v, op);
    if (rc <= 0)
        return rc;
    ov = JS_GetPropertyStr(ctx, v, "out");
    if (JS_IsException(ov))
        return -1;
    if (!JS_IsString(ov)) {
        JS_FreeValue(ctx, ov);
        JS_ThrowTypeError(ctx, "%s: the \"out\" option names a column, as a "
                               "string",
            op);
        return -1;
    }
    {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, ov);
        DataFrame* mdf = DYN_UNCONST(df);
        JS_FreeValue(ctx, ov);
        if (!s)
            return -1;
        if (strlen(s) != len) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx, "column name contains a NUL character");
            return -1;
        }
        idx = df_find_col(df, s);
        if (idx < 0) {
            if (df->ncols >= DF_MAX_COLS) {
                JS_ThrowRangeError(ctx, "%s {out}: the frame already has %d columns, the most it can hold",
                    op, DF_MAX_COLS);
                JS_FreeCString(ctx, s);
                return -1;
            }
            free(mdf->out_new);
            mdf->out_new = strdup(s);
            JS_FreeCString(ctx, s);
            if (!mdf->out_new) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            mdf->out_gen++;
            *pout_idx = DF_OUT_NEW | (int)(mdf->out_gen & DF_OUT_GEN_MASK);
            return 1;
        }
        JS_FreeCString(ctx, s);
    }
    if (df->cols[idx].type != DF_F64) {
        JS_ThrowTypeError(ctx, "%s {out}: column '%s' is %s; the in-place "
                               "target must be an existing Float64Array column",
            op, df->cols[idx].name,
            df_type_name(df->cols[idx].type));
        return -1;
    }
    if (df->cols[idx].length != df->nrows) {
        JS_ThrowRangeError(ctx, "%s {out}: column '%s' has %u rows, the frame "
                                "has %u",
            op, df->cols[idx].name,
            df->cols[idx].length, df->nrows);
        return -1;
    }
    *pout_idx = idx;
    return 1;
}

#endif
