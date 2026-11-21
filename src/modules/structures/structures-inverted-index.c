#include "structures-internal.h"
#include "core/ds/ds-inverted-index.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_invidx_t x;
} sdb_ii_w;

typedef struct {
    char buf[DYN_INVIDX_TOK_MAX];
    size_t n;
} ii_first_ctx;

typedef struct {
    uint32_t df;
    uint32_t ti;
} ii_sugg;

static int sdb_opt_f64(JSContext* ctx, JSValueConst opts, const char* name, double* out)
{
    JSValue v;
    int rc = 0;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v) && JS_ToFloat64(ctx, out, v))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

JSClassID sdb_ii_class_id;

static void sdb_ii_free(void* native)
{
    sdb_ii_w* w = (sdb_ii_w*)native;
    if (!w)
        return;
    dyn_invidx_free(&w->x);
    dyn_ds_free(w);
}

static void sdb_ii_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ii_free(JS_GetOpaque(val, sdb_ii_class_id));
}

const JSClassDef sdb_ii_class = {
    "InvertedIndex",
    .finalizer = sdb_ii_finalizer,
};

static sdb_ii_w* sdb_ii_new(double k1, double b)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    dyn_invidx_init(&w->x, k1, b);
    return w;
}

JSValue sdb_ii_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    double k1 = 1.2, b = 0.75;
    JSValue sw = JS_UNDEFINED;
    JSValueConst opts = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (sdb_opt_f64(ctx, opts, "k1", &k1) || sdb_opt_f64(ctx, opts, "b", &b))
        return JS_EXCEPTION;
    if (!(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1))
        return JS_ThrowRangeError(ctx, "InvertedIndex: k1 must be 0..100 and b 0..1");
    if (sdb_opt_get(ctx, opts, "stopWords", &sw))
        return JS_EXCEPTION;
    w = sdb_ii_new(k1, b);
    if (!w) {
        JS_FreeValue(ctx, sw);
        return ds2_oom(ctx);
    }
    if (!JS_IsUndefined(sw)) {
        JSValue lv;
        uint32_t n, i;
        if (JS_IsArray(ctx, sw) <= 0) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowTypeError(ctx, "stopWords must be an array of strings");
        }
        lv = JS_GetPropertyStr(ctx, sw, "length");
        if (JS_ToUint32(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (n > 100000) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowRangeError(ctx, "stopWords: at most 100000 entries");
        }
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sw, i);
            ds2_str s;
            int rc;
            if (JS_IsException(e) || !JS_IsString(e)) {
                if (!JS_IsException(e))
                    JS_ThrowTypeError(ctx, "stopWords[%u] is not a string", (unsigned)i);
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = ds2_str_get(ctx, &s, e);
            JS_FreeValue(ctx, e);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = dyn_invidx_tokenize(s.p, s.n, dyn_invidx_stop_tok, &w->x);
            ds2_str_release(ctx, &s);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return ds2_oom(ctx);
            }
        }
    }
    JS_FreeValue(ctx, sw);
    return dyn_plain_wrap(ctx, new_target, sdb_ii_class_id, w, sdb_ii_free);
}

static JSValue sdb_ii_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    uint32_t id;
    ds2_str t;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    rc = dyn_invidx_add(&w->x, id, t.p, t.n);
    ds2_str_release(ctx, &t);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ii_id_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ii_w* w;
    uint32_t id;
    dyn_ds_ient* e;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, dyn_invidx_remove(&w->x, id));
    e = dyn_ds_imap_find(&w->x.didx, id);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    return e ? JS_NewInt64(ctx, w->x.docs[e->v].len) : JS_UNDEFINED;
}

static JSValue sdb_ii_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str q;
    uint32_t limit = 10, n, i, out = 0;
    int prefix = 0, and_mode = 0;
    dyn_invidx_hit* h;
    JSValue arr, mode = JS_UNDEFINED;
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || sdb_opt_bool(ctx, opts, "prefix", &prefix) || sdb_opt_get(ctx, opts, "mode", &mode))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(mode)) {
        const char* m = JS_ToCString(ctx, mode);
        JS_FreeValue(ctx, mode);
        if (!m)
            return JS_EXCEPTION;
        if (!strcmp(m, "and"))
            and_mode = 1;
        else if (strcmp(m, "or")) {
            JS_FreeCString(ctx, m);
            return JS_ThrowRangeError(ctx, "mode must be \"and\" or \"or\"");
        }
        JS_FreeCString(ctx, m);
    }
    if (limit > DYN_DS_MAX_ITEMS)
        limit = DYN_DS_MAX_ITEMS;
    if (ds2_str_get(ctx, &q, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &q);
        return JS_EXCEPTION;
    }
    if (dyn_invidx_search(&w->x, q.p, q.n, and_mode, prefix, limit, &h, &n) < 0) {
        ds2_str_release(ctx, &q);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &q);
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++)
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewInt64(ctx, h[i].id), JS_NewFloat64(ctx, h[i].s))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    dyn_ds_free(h);
    return arr;
}

static int ii_first_tok(void* ud, const char* t, size_t n)
{
    ii_first_ctx* c = (ii_first_ctx*)ud;
    memcpy(c->buf, t, n);
    c->n = n;
    return 1;
}

static JSValue sdb_ii_doc_freq(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    int64_t ti;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    dyn_invidx_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    if (!c.n)
        return JS_NewInt32(ctx, 0);
    ti = dyn_invidx_term_get(&w->x, c.buf, c.n, 0);
    return JS_NewInt64(ctx, ti >= 0 ? w->x.terms[ti].n : 0);
}

static _Thread_local const dyn_invidx_t* ii_sort_ctx;

static int ii_sugg_cmp(const void* a, const void* b)
{
    const ii_sugg *x = (const ii_sugg*)a, *y = (const ii_sugg*)b;
    const dyn_invidx_term *tx, *ty;
    uint32_t m;
    int c;
    if (x->df != y->df)
        return x->df > y->df ? -1 : 1;
    tx = &ii_sort_ctx->terms[x->ti];
    ty = &ii_sort_ctx->terms[y->ti];
    m = tx->len < ty->len ? tx->len : ty->len;
    c = m ? memcmp(tx->s, ty->s, m) : 0;
    return c ? c : (tx->len > ty->len) - (tx->len < ty->len);
}

static JSValue sdb_ii_suggest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    uint32_t limit = 10, i, n = 0, out = 0;
    ii_sugg* s;
    JSValue arr;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && dyn_size_arg(ctx, argv[1], DYN_DS_MAX_ITEMS, &limit))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    dyn_invidx_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    s = (ii_sugg*)dyn_ds_malloc(sizeof(ii_sugg) * ((size_t)w->x.nterms + 1));
    if (!s)
        return ds2_oom(ctx);
    for (i = 0; i < w->x.nterms; i++) {
        const dyn_invidx_term* tm = &w->x.terms[i];
        if (tm->n && tm->len >= c.n && (c.n == 0 || memcmp(tm->s, c.buf, c.n) == 0)) {
            s[n].df = tm->n;
            s[n].ti = i;
            n++;
        }
    }
    ii_sort_ctx = &w->x;
    qsort(s, n, sizeof(ii_sugg), ii_sugg_cmp);
    ii_sort_ctx = NULL;
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && i < limit && !JS_IsException(arr); i++) {
        const dyn_invidx_term* tm = &w->x.terms[s[i].ti];
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewStringLen(ctx, tm->s, tm->len), JS_NewInt64(ctx, tm->n))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_ds_free(s);
    return arr;
}

static int ii_collect_tok(void* ud, const char* t, size_t n)
{
    JSContext* ctx = ((JSContext**)ud)[0];
    JSValue* arr = (JSValue*)((void**)ud)[1];
    uint32_t* i = (uint32_t*)((void**)ud)[2];
    return sdb_push(ctx, *arr, i, JS_NewStringLen(ctx, t, n)) < 0 ? -1 : 0;
}

JSValue sdb_ii_tokenize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_str t;
    JSValue arr;
    uint32_t i = 0;
    void* ud[3];
    (void)this_val;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        ud[0] = ctx;
        ud[1] = &arr;
        ud[2] = &i;
        if (dyn_invidx_tokenize(t.p, t.n, ii_collect_tok, ud)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    ds2_str_release(ctx, &t);
    return arr;
}

static JSValue sdb_ii_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    uint32_t i, n = 0;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->x.live);
    if (magic == 1) {
        for (i = 0; i < w->x.nterms; i++)
            n += w->x.terms[i].n != 0;
        return JS_NewInt64(ctx, n);
    }
    return JS_NewFloat64(ctx, w->x.live ? (double)w->x.total_len / w->x.live : 0);
}

const JSCFunctionListEntry sdb_ii_proto[] = {
    JS_CFUNC_DEF("add", 2, sdb_ii_add),
    JS_CFUNC_MAGIC_DEF("remove", 1, sdb_ii_id_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_ii_id_op, 1),
    JS_CFUNC_MAGIC_DEF("docLength", 1, sdb_ii_id_op, 2),
    JS_CFUNC_DEF("search", 1, sdb_ii_search),
    JS_CFUNC_DEF("docFreq", 1, sdb_ii_doc_freq),
    JS_CFUNC_DEF("suggest", 1, sdb_ii_suggest),
    JS_CGETSET_MAGIC_DEF("size", sdb_ii_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("termCount", sdb_ii_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("avgLength", sdb_ii_getter, NULL, 2),
};

int sdb_ii_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ii_w* ii = JS_GetOpaque(obj, sdb_ii_class_id);
    dyn_invidx_t* x = &ii->x;
    uint32_t i, j;
    (void)ctx;
    if (dyn_ser_f64(w, x->k1) < 0 || dyn_ser_f64(w, x->b) < 0 || dyn_ser_u32(w, x->nstop) < 0)
        return -1;
    for (i = 0; i < x->nstop; i++)
        if (dyn_ser_blob(w, x->stopw[i], strlen(x->stopw[i])) < 0)
            return -1;
    if (dyn_ser_u32(w, x->nterms) < 0)
        return -1;
    for (i = 0; i < x->nterms; i++)
        if (dyn_ser_blob(w, x->terms[i].s, x->terms[i].len) < 0)
            return -1;
    if (dyn_ser_u32(w, x->live) < 0)
        return -1;
    for (i = 0; i < x->nslots; i++) {
        const dyn_invidx_doc* d = &x->docs[i];
        if (!d->alive)
            continue;
        if (dyn_ser_u32(w, d->id) < 0 || dyn_ser_u32(w, d->len) < 0 || dyn_ser_u32(w, d->nt) < 0)
            return -1;
        for (j = 0; j < d->nt; j++)
            if (dyn_ser_u32(w, d->t[j].term) < 0 || dyn_ser_u32(w, d->t[j].tf) < 0)
                return -1;
    }
    return 0;
}

JSValue sdb_ii_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double k1 = dyn_de_f64(r), b = dyn_de_f64(r);
    uint32_t ns, nt, nd, i, j;
    sdb_ii_w* ii;
    dyn_invidx_t* x;
    (void)opts;
    if (!dyn_de_ok(r) || !(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1) || dyn_de_count(r, &ns, 4) < 0)
        return sdb_bad(ctx);
    ii = sdb_ii_new(k1, b);
    if (!ii)
        return ds2_oom(ctx);
    x = &ii->x;
    for (i = 0; i < ns; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        if (!dyn_de_ok(r) || n == 0 || n > DYN_INVIDX_TOK_MAX || memchr(s, 0, n))
            goto bad;
        if (dyn_invidx_add_stop(x, s, n) < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nt, 4) < 0 || dyn_de_charge(r, (uint64_t)nt * 96) < 0)
        goto bad;
    for (i = 0; i < nt; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        int64_t ti;
        if (!dyn_de_ok(r) || n == 0 || n > DYN_INVIDX_TOK_MAX || dyn_invidx_term_get(x, s, n, 0) >= 0)
            goto bad;
        ti = dyn_invidx_term_get(x, s, n, 1);
        if (ti < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nd, 12) < 0 || dyn_de_charge(r, (uint64_t)nd * 64) < 0)
        goto bad;
    for (i = 0; i < nd; i++) {
        uint32_t id = dyn_de_u32(r), len = dyn_de_u32(r), cnt;
        uint64_t sumtf = 0;
        dyn_invidx_dt* list;
        if (!dyn_de_ok(r) || dyn_ds_imap_find(&x->didx, id) || dyn_de_count(r, &cnt, 8) < 0 || dyn_de_charge(r, (uint64_t)cnt * 24) < 0)
            goto bad;
        list = (dyn_invidx_dt*)dyn_ds_malloc(sizeof(dyn_invidx_dt) * ((size_t)cnt + 1));
        if (!list)
            goto oom;
        if (dyn_invidx_tscratch(x) < 0) {
            dyn_ds_free(list);
            goto oom;
        }
        dyn_invidx_bump_tepoch(x);
        for (j = 0; j < cnt; j++) {
            list[j].term = dyn_de_u32(r);
            list[j].tf = dyn_de_u32(r);
            if (!dyn_de_ok(r) || list[j].term >= x->nterms || list[j].tf == 0 || x->tstamp[list[j].term] == x->tepoch) {
                dyn_ds_free(list);
                goto bad;
            }
            x->tstamp[list[j].term] = x->tepoch;
            sumtf += list[j].tf;
        }
        if (sumtf != len) {
            dyn_ds_free(list);
            goto bad;
        }
        if (dyn_invidx_install(x, id, list, cnt, len) < 0) {
            dyn_ds_free(list);
            goto oom;
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ii_class_id, ii, sdb_ii_free);
bad:
    sdb_ii_free(ii);
    return sdb_bad(ctx);
oom:
    sdb_ii_free(ii);
    return ds2_oom(ctx);
}

#endif
