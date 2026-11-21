#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DS_TABLE_EXT 0xFFFFFFFFu

typedef struct {
    uint64_t pfx;
    const char* p;
    size_t n;
    uint32_t id;
} ds2_tbl_key;

static uint64_t ds2_tbl_pfx(const char* p, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | (i < n ? (uint8_t)p[i] : 0);
    return v;
}

static void ds2_table_free(void* native)
{
    ds2_table_w* w = (ds2_table_w*)native;
    if (!w)
        return;
    dyn_table_free(w->c, ds_cell_release, w->rt);
    dyn_ds_free(w);
}

JSClassID ds2_table_class_id;

DS2_FINALIZER(ds2_table)

static void ds2_table_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    ds2_table_w* w = JS_GetOpaque(val, ds2_table_class_id);
    uint32_t i, n;
    if (!w)
        return;
    n = dyn_table_size(w->c);
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        JS_MarkValue(rt, ds_cell_val(v), mark_func);
    }
}

const JSClassDef ds2_table_class = {
    "Table",
    .finalizer = ds2_table_finalizer,
    .gc_mark = ds2_table_gc_mark,
};

JSValue ds2_table_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w;
    (void)argc;
    (void)argv;
    w = (ds2_table_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->rt = JS_GetRuntime(ctx);
    w->c = dyn_table_new();
    if (!w->c) {
        dyn_ds_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, ds2_table_class_id, w, ds2_table_free);
}

static JSValue ds2_table_put(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w;
    ds2_str r, c;
    dyn_cell_t cell, old;
    int rep;
    (void)argc;
    if (ds2_str_get(ctx, &r, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &c, argv[1])) {
        ds2_str_release(ctx, &r);
        return JS_EXCEPTION;
    }
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &r);
        ds2_str_release(ctx, &c);
        return JS_EXCEPTION;
    }
    cell = ds_val_cell(JS_DupValue(ctx, argv[2]));
    rep = dyn_table_put(w->c, r.p, r.n, c.p, c.n, &cell, &old);
    ds2_str_release(ctx, &r);
    ds2_str_release(ctx, &c);
    if (rep < 0) {
        JS_FreeValue(ctx, ds_cell_val(&cell));
        return ds2_oom(ctx);
    }
    if (rep)
        JS_FreeValue(ctx, ds_cell_val(&old));
    return JS_DupValue(ctx, this_val);
}

static JSValue ds2_table_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    ds2_table_w* w;
    ds2_str r, c;
    JSValue out;
    (void)argc;
    if (ds2_str_get(ctx, &r, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &c, argv[1])) {
        ds2_str_release(ctx, &r);
        return JS_EXCEPTION;
    }
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &r);
        ds2_str_release(ctx, &c);
        return JS_EXCEPTION;
    }
    if (magic == 2) {
        dyn_cell_t old;
        int had = dyn_table_remove(w->c, r.p, r.n, c.p, c.n, &old);
        if (had)
            JS_FreeValue(ctx, ds_cell_val(&old));
        out = JS_NewBool(ctx, had);
    } else {
        const dyn_cell_t* v = dyn_table_get(w->c, r.p, r.n, c.p, c.n);
        out = magic == 1 ? JS_NewBool(ctx, v != NULL)
                         : (v ? JS_DupValue(ctx, ds_cell_val(v)) : JS_UNDEFINED);
    }
    ds2_str_release(ctx, &r);
    ds2_str_release(ctx, &c);
    return out;
}

static JSValue ds2_table_slice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int by_column)
{
    ds2_table_w* w;
    ds2_str key;
    JSValue arr;
    uint32_t i, n, out = 0;
    (void)argc;
    if (ds2_str_get(ctx, &key, argv[0]))
        return JS_EXCEPTION;
    w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w) {
        ds2_str_release(ctx, &key);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        ds2_str_release(ctx, &key);
        return arr;
    }
    (void)n;
    for (i = dyn_table_slice_first(w->c, key.p, key.n, by_column);
        i != UINT32_MAX;
        i = dyn_table_slice_next(w->c, i, by_column)) {
        const char *r, *c, *label;
        size_t rn, cn, ln;
        const dyn_cell_t* v;
        JSValue pair;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        label = by_column ? r : c;
        ln = by_column ? rn : cn;
        pair = ds2_tuple2(ctx, ds2_new_str(ctx, label, ln),
            JS_DupValue(ctx, ds_cell_val(v)));
        if (JS_IsException(pair))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, arr, out++, pair,
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    ds2_str_release(ctx, &key);
    return arr;
fail:
    ds2_str_release(ctx, &key);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_table_cells(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_table_w* w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    JSValue arr;
    uint32_t i, n;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    n = dyn_table_size(w->c);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        JSValue t;
        dyn_table_at(w->c, i, &r, &rn, &c, &cn, &v);
        t = ds2_tuple3(ctx, ds2_new_str(ctx, r, rn), ds2_new_str(ctx, c, cn),
            JS_DupValue(ctx, ds_cell_val(v)));
        if (JS_IsException(t))
            goto fail;
        if (JS_DefinePropertyValueUint32(ctx, arr, i, t, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue ds2_table_size(JSContext* ctx, JSValueConst this_val)
{
    ds2_table_w* w = (ds2_table_w*)dyn_plain_get(ctx, this_val, ds2_table_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_table_size(w->c));
}

DS2_ITERATOR(ds2_table, "cells")

const JSCFunctionListEntry ds2_table_proto[] = {
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ds2_table_iterator),
    JS_CFUNC_DEF("put", 3, ds2_table_put),
    JS_CFUNC_MAGIC_DEF("get", 2, ds2_table_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 2, ds2_table_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 2, ds2_table_op, 2),
    JS_CFUNC_MAGIC_DEF("row", 1, ds2_table_slice, 0),
    JS_CFUNC_MAGIC_DEF("column", 1, ds2_table_slice, 1),
    JS_CFUNC_DEF("cells", 0, ds2_table_cells),
    JS_CGETSET_DEF("size", ds2_table_size, NULL),
};

static const dyn_cell_t* ds2_table_cell(void* native, uint32_t i)
{
    const char *r, *c;
    size_t rn, cn;
    const dyn_cell_t* v = NULL;
    dyn_table_at((dyn_table_t*)native, i, &r, &rn, &c, &cn, &v);
    return v;
}

static int ds2_tbl_key_cmp(const ds2_tbl_key* x, const ds2_tbl_key* y)
{
    size_t m = x->n < y->n ? x->n : y->n;
    int c;
    if (x->pfx != y->pfx)
        return x->pfx < y->pfx ? -1 : 1;
    c = m > 8 ? memcmp(x->p + 8, y->p + 8, m - 8) : 0;
    if (c)
        return c;
    return x->n < y->n ? -1 : (x->n > y->n ? 1 : 0);
}

static void ds2_tbl_sort(ds2_tbl_key* v, ds2_tbl_key* tmp, size_t n)
{
    size_t mid, i, j, k;
    if (n < 2)
        return;
    mid = n / 2;
    ds2_tbl_sort(v, tmp, mid);
    ds2_tbl_sort(v + mid, tmp, n - mid);
    i = 0;
    j = mid;
    k = 0;
    while (i < mid && j < n)
        tmp[k++] = ds2_tbl_key_cmp(&v[i], &v[j]) <= 0 ? v[i++] : v[j++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(*v));
}

static int ds2_tbl_dict(dyn_ser_t* w, ds2_tbl_key* v, ds2_tbl_key* tmp,
    uint32_t n, uint32_t* idof)
{
    uint32_t k, distinct = 0;
    const char* prev = NULL;
    size_t prev_len = 0;

    ds2_tbl_sort(v, tmp, n);
    for (k = 0; k < n; k++)
        if (!k || ds2_tbl_key_cmp(&v[k], &v[k - 1]) != 0)
            distinct++;
    if (dyn_ser_uvarint(w, distinct) < 0)
        return -1;
    distinct = 0;
    for (k = 0; k < n; k++) {
        if (!k || ds2_tbl_key_cmp(&v[k], &v[k - 1]) != 0) {
            size_t sh = 0, lim = v[k].n < prev_len ? v[k].n : prev_len;
            while (sh < lim && v[k].p[sh] == prev[sh])
                sh++;
            if (dyn_ser_uvarint(w, sh) < 0 || dyn_ser_uvarint(w, v[k].n - sh) < 0 || dyn_ser_raw(w, v[k].p + sh, v[k].n - sh) < 0)
                return -1;
            prev = v[k].p;
            prev_len = v[k].n;
            distinct++;
        }
        idof[v[k].id] = distinct - 1;
    }
    return 0;
}

int ds2_table_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    ds2_table_w* o = (ds2_table_w*)JS_GetOpaque(obj, ds2_table_class_id);
    uint32_t i, n = dyn_table_size(o->c);
    ds2_tbl_key *rows = NULL, *cols = NULL, *tmp = NULL;
    uint32_t *rid = NULL, *cid = NULL;
    int rc = -1;

    if (dyn_ser_u32(w, DS_TABLE_EXT) < 0 || dyn_ser_u32(w, n) < 0)
        return -1;
    if (!n)
        return ds2_write_cells(ctx, w, o->c, n, ds2_table_cell);

    rows = (ds2_tbl_key*)dyn_ds_malloc(sizeof(*rows) * n);
    cols = (ds2_tbl_key*)dyn_ds_malloc(sizeof(*cols) * n);
    tmp = (ds2_tbl_key*)dyn_ds_malloc(sizeof(*tmp) * n);
    rid = (uint32_t*)dyn_ds_malloc(sizeof(*rid) * n);
    cid = (uint32_t*)dyn_ds_malloc(sizeof(*cid) * n);
    if (!rows || !cols || !tmp || !rid || !cid)
        goto done;
    for (i = 0; i < n; i++) {
        const char *r, *c;
        size_t rn, cn;
        const dyn_cell_t* v;
        dyn_table_at(o->c, i, &r, &rn, &c, &cn, &v);
        rows[i].p = r;
        rows[i].pfx = ds2_tbl_pfx(r, rn);
        rows[i].n = rn;
        rows[i].id = i;
        cols[i].p = c;
        cols[i].pfx = ds2_tbl_pfx(c, cn);
        cols[i].n = cn;
        cols[i].id = i;
    }
    if (ds2_tbl_dict(w, rows, tmp, n, rid) < 0 || ds2_tbl_dict(w, cols, tmp, n, cid) < 0)
        goto done;
    for (i = 0; i < n; i++)
        if (dyn_ser_uvarint(w, rid[i]) < 0 || dyn_ser_uvarint(w, cid[i]) < 0)
            goto done;
    rc = 0;
done:
    dyn_ds_free(rows);
    dyn_ds_free(cols);
    dyn_ds_free(tmp);
    dyn_ds_free(rid);
    dyn_ds_free(cid);
    if (rc < 0)
        return -1;
    return ds2_write_cells(ctx, w, o->c, n, ds2_table_cell);
}

static int ds2_tbl_read_dict(dyn_de_t* r, uint32_t* count, char** blob,
    size_t** off, size_t** len)
{
    uint64_t d;
    uint32_t k;
    size_t cap = 256, used = 0;
    char* b = NULL;
    size_t *o = NULL, *l = NULL;

    *blob = NULL;
    *off = NULL;
    *len = NULL;
    *count = 0;
    if (dyn_de_uvarint(r, &d) < 0 || d > (uint64_t)dyn_de_left(r) + 1)
        return -1;
    *count = (uint32_t)d;
    b = (char*)dyn_ds_malloc(cap);
    o = (size_t*)dyn_ds_malloc(sizeof(*o) * (d ? d : 1));
    l = (size_t*)dyn_ds_malloc(sizeof(*l) * (d ? d : 1));
    if (!b || !o || !l)
        goto fail;
    for (k = 0; k < *count; k++) {
        uint64_t sh, sl;
        const uint8_t* p;
        size_t klen;
        if (dyn_de_uvarint(r, &sh) < 0 || dyn_de_uvarint(r, &sl) < 0)
            goto fail;
        if (k == 0 ? sh != 0 : sh > l[k - 1])
            goto fail;
        if (sl > (uint64_t)dyn_de_left(r))
            goto fail;
        klen = (size_t)(sh + sl);
        while (used + klen > cap) {
            char* nb;
            cap *= 2;
            nb = (char*)dyn_ds_realloc(b, cap);
            if (!nb)
                goto fail;
            b = nb;
        }
        if (sh)
            memcpy(b + used, b + o[k - 1], (size_t)sh);
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto fail;
        memcpy(b + used + sh, p, (size_t)sl);
        o[k] = used;
        l[k] = klen;
        used += klen;
    }
    *blob = b;
    *off = o;
    *len = l;
    return 0;
fail:
    dyn_ds_free(b);
    dyn_ds_free(o);
    dyn_ds_free(l);
    *blob = NULL;
    *off = NULL;
    *len = NULL;
    return -1;
}

JSValue ds2_table_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_table_t* t;
    uint32_t n, i, first;
    JSValue vals;
    const char **rk, **ck;
    size_t *rl, *cl;
    char *rblob = NULL, *cblob = NULL;
    size_t *roff = NULL, *rlen = NULL, *coff = NULL, *clen = NULL;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (first != DS_TABLE_EXT) {
        n = first;
        if ((uint64_t)n * 8 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    } else {
        n = dyn_de_u32(r);
        if (!dyn_de_ok(r) || (uint64_t)n * 2 > (uint64_t)dyn_de_left(r) + 2)
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    rk = (const char**)dyn_ds_calloc(n ? n : 1, sizeof(*rk));
    ck = (const char**)dyn_ds_calloc(n ? n : 1, sizeof(*ck));
    rl = (size_t*)dyn_ds_calloc(n ? n : 1, sizeof(*rl));
    cl = (size_t*)dyn_ds_calloc(n ? n : 1, sizeof(*cl));
    if (!rk || !ck || !rl || !cl) {
        dyn_ds_free(rk);
        dyn_ds_free(ck);
        dyn_ds_free(rl);
        dyn_ds_free(cl);
        return ds2_oom(ctx);
    }
    if (first != DS_TABLE_EXT) {
        for (i = 0; i < n; i++) {
            rk[i] = dyn_de_blob(r, &rl[i]);
            ck[i] = dyn_de_blob(r, &cl[i]);
        }
    } else if (n) {
        uint32_t rn, cn, k;
        if (ds2_tbl_read_dict(r, &rn, &rblob, &roff, &rlen) < 0 || ds2_tbl_read_dict(r, &cn, &cblob, &coff, &clen) < 0) {
            dyn_ds_free(rk);
            dyn_ds_free(ck);
            dyn_ds_free(rl);
            dyn_ds_free(cl);
            dyn_ds_free(rblob);
            dyn_ds_free(roff);
            dyn_ds_free(rlen);
            dyn_ds_free(cblob);
            dyn_ds_free(coff);
            dyn_ds_free(clen);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        for (k = 0; k < n; k++) {
            uint64_t a, b;
            if (dyn_de_uvarint(r, &a) < 0 || dyn_de_uvarint(r, &b) < 0 || a >= rn || b >= cn) {
                dyn_ds_free(rk);
                dyn_ds_free(ck);
                dyn_ds_free(rl);
                dyn_ds_free(cl);
                dyn_ds_free(rblob);
                dyn_ds_free(roff);
                dyn_ds_free(rlen);
                dyn_ds_free(cblob);
                dyn_ds_free(coff);
                dyn_ds_free(clen);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            rk[k] = rblob + roff[a];
            rl[k] = rlen[a];
            ck[k] = cblob + coff[b];
            cl[k] = clen[b];
        }
    }
    if (!dyn_de_ok(r)) {
        dyn_ds_free(rk);
        dyn_ds_free(ck);
        dyn_ds_free(rl);
        dyn_ds_free(cl);
        dyn_ds_free(rblob);
        dyn_ds_free(roff);
        dyn_ds_free(rlen);
        dyn_ds_free(cblob);
        dyn_ds_free(coff);
        dyn_ds_free(clen);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_ds_free(rk);
        dyn_ds_free(ck);
        dyn_ds_free(rl);
        dyn_ds_free(cl);
        dyn_ds_free(rblob);
        dyn_ds_free(roff);
        dyn_ds_free(rlen);
        dyn_ds_free(cblob);
        dyn_ds_free(coff);
        dyn_ds_free(clen);
        return vals;
    }
    t = dyn_table_new();
    if (!t) {
        JS_FreeValue(ctx, vals);
        dyn_ds_free(rk);
        dyn_ds_free(ck);
        dyn_ds_free(rl);
        dyn_ds_free(cl);
        dyn_ds_free(rblob);
        dyn_ds_free(roff);
        dyn_ds_free(rlen);
        dyn_ds_free(cblob);
        dyn_ds_free(coff);
        dyn_ds_free(clen);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, vals, i);
        int vex = JS_IsException(v);
        dyn_cell_t cell = ds_val_cell(v), old;
        int rep = vex ? -1 : dyn_table_put(t, rk[i], rl[i], ck[i], cl[i], &cell, &old);
        if (rep < 0) {
            JS_FreeValue(ctx, v);
            dyn_table_free(t, ds_cell_release, JS_GetRuntime(ctx));
            JS_FreeValue(ctx, vals);
            dyn_ds_free(rk);
            dyn_ds_free(ck);
            dyn_ds_free(rl);
            dyn_ds_free(cl);
            dyn_ds_free(rblob);
            dyn_ds_free(roff);
            dyn_ds_free(rlen);
            dyn_ds_free(cblob);
            dyn_ds_free(coff);
            dyn_ds_free(clen);
            return vex ? JS_EXCEPTION : ds2_oom(ctx);
        }
        if (rep)
            JS_FreeValue(ctx, ds_cell_val(&old));
    }
    JS_FreeValue(ctx, vals);
    dyn_ds_free(rk);
    dyn_ds_free(ck);
    dyn_ds_free(rl);
    dyn_ds_free(cl);
    dyn_ds_free(rblob);
    dyn_ds_free(roff);
    dyn_ds_free(rlen);
    dyn_ds_free(cblob);
    dyn_ds_free(coff);
    dyn_ds_free(clen);
    DS2_WRAP(ds2_table, dyn_table_free(t, ds_cell_release, JS_GetRuntime(ctx)), t);
}

#endif
