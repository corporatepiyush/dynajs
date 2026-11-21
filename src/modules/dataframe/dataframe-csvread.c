#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

static JSValue df_csv_throw(JSContext* ctx, const char* op, int rc, const DfkCsvOut* o)
{
    switch (rc) {
    case DFK_CSV_E_OOM:
        return JS_ThrowOutOfMemory(ctx);
    case DFK_CSV_E_QUOTE:
    case DFK_CSV_E_GARBAGE:
    case DFK_CSV_E_WIDTH:
        return JS_ThrowSyntaxError(ctx, "%s: row %llu: %s", op, (unsigned long long)o->err_record, o->err_text);
    case DFK_CSV_E_NUMBER:
    case DFK_CSV_E_INT:
        return JS_ThrowTypeError(ctx, "%s: row %llu: %s", op, (unsigned long long)o->err_record, o->err_text);
    case DFK_CSV_E_ROWS:
        return JS_ThrowRangeError(ctx, "%s: %s", op, o->err_text);
    default:
        return JS_ThrowTypeError(ctx, "%s: %s", op, o->err_text);
    }
}

JSValue dyn_df_read_csv(JSContext* ctx, const char* op, const uint8_t* buf,
    size_t len, const struct DfkCsvSpec* spec)
{
    DfkCsvSpec s = *spec;
    DfkCsvOut o;
    int rc;

    if (dyn_df_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    dfc_hash_seed_once();
    s.seed = atomic_load_explicit(&dfc_hash_seed, memory_order_relaxed);
    rc = DYN_OFFLOAD_CPU(dfk_csv_read(buf, len, &s, &o));
    if (rc)
        return df_csv_throw(ctx, op, rc, &o);
    return dyn_df_csv_out_frame(ctx, op, &o);
}

JSValue dyn_df_csv_out_frame(JSContext* ctx, const char* op, struct DfkCsvOut* po)
{
    DfkCsvOut o = *po;
    DFBuilder B;
    uint32_t i;
    int rc;
    JSValue ret;

    memset(po, 0, sizeof(*po));
    if (dyn_df_ensure_class(ctx) < 0) {
        dfk_csv_free(&o);
        return JS_EXCEPTION;
    }
    if (dfb_init(ctx, &B, op, o.ncols, o.nrows)) {
        dfk_csv_free(&o);
        return JS_EXCEPTION;
    }
    for (i = 0; i < o.ncols; i++) {
        DfkCsvCol* c = &o.cols[i];
        if (c->kind == DFK_CSV_F64) {
            rc = dfb_add(ctx, &B, c->name, DF_F64, c->f64, o.nrows);
            c->f64 = NULL;
        } else if (c->kind == DFK_CSV_I32) {
            rc = dfb_add(ctx, &B, c->name, DF_I32, c->i32, o.nrows);
            c->i32 = NULL;
        } else {
            rc = dfb_add_str(ctx, &B, c->name, c->codes, c->dict, c->dict_len, o.nrows);
            c->codes = NULL;
            c->dict = NULL;
            c->dict_len = 0;
        }
        if (rc) {
            dfk_csv_free(&o);
            dfb_free(ctx, &B);
            return JS_EXCEPTION;
        }
    }
    ret = dfb_seal(ctx, &B);
    dfk_csv_free(&o);
    return ret;
}

#endif
