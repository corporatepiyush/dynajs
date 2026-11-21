#include "structures-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_zsl_t sl;
} sdb_zset_w;

int sdb_zsl_init(dyn_zsl_t* s, JSContext* ctx)
{
    return dyn_zsl_init(s, ds_cell_release, JS_GetRuntime(ctx));
}

int sdb_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, JSValue val, dyn_zsl_node** out)
{
    dyn_cell_t c = ds_val_cell(val);
    return dyn_zsl_insert(s, score, k, n, &c, out);
}

void sdb_zsl_mark(JSRuntime* rt, const dyn_zsl_t* s, JS_MarkFunc* mark_func)
{
    dyn_zsl_node* x;
    if (!s->head)
        return;
    for (x = s->head->lv[0].fwd; x; x = x->lv[0].fwd)
        JS_MarkValue(rt, ds_cell_val(&x->val), mark_func);
}

JSValue dyn_zsl_keystr(JSContext* ctx, const dyn_zsl_node* x)
{
    return JS_NewStringLen(ctx, x->key, x->klen);
}

static int sdb_norm_range(int64_t start, int64_t stop, uint32_t len, uint32_t* a, uint32_t* b)
{
    if (start < 0)
        start += len;
    if (stop < 0)
        stop += len;
    if (start < 0)
        start = 0;
    if (start > stop || start >= (int64_t)len)
        return 0;
    if (stop >= (int64_t)len)
        stop = (int64_t)len - 1;
    *a = (uint32_t)start;
    *b = (uint32_t)stop;
    return 1;
}

JSClassID sdb_zset_class_id;

static void sdb_zset_free(void* native)
{
    sdb_zset_w* w = (sdb_zset_w*)native;
    if (!w)
        return;
    dyn_zsl_destroy(&w->sl);
    dyn_ds_free(w);
}

static void sdb_zset_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_zset_free(JS_GetOpaque(val, sdb_zset_class_id));
}

const JSClassDef sdb_zset_class = {
    "ZSet",
    .finalizer = sdb_zset_finalizer,
};

static sdb_zset_w* sdb_zset_new(JSContext* ctx)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_ds_malloc(sizeof(*w));
    if (!w)
        return NULL;
    if (sdb_zsl_init(&w->sl, ctx) < 0) {
        dyn_ds_free(w);
        return NULL;
    }
    return w;
}

JSValue sdb_zset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    (void)argc;
    (void)argv;
    w = sdb_zset_new(ctx);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_zset_class_id, w, sdb_zset_free);
}

static int sdb_zset_put(sdb_zset_w* w, const char* k, size_t n, double score, int incr, double* result, int* added)
{
    dyn_zsl_node* x = dyn_zsl_find(&w->sl, k, n);
    *added = 0;
    if (x) {
        double ns = incr ? x->score + score : score;
        if (ns != ns)
            return -2;
        *result = ns;
        if (ns == x->score)
            return 0;
        dyn_zsl_unlink(&w->sl, x);
        x->score = ns;
        dyn_zsl_link(&w->sl, x);
        return 0;
    }
    *result = score;
    if (sdb_zsl_insert(&w->sl, score, k, n, JS_UNDEFINED, NULL) < 0)
        return -1;
    *added = 1;
    return 0;
}

static JSValue sdb_zset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    double sc, res;
    int added, rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (sdb_num_arg(ctx, argv[1], magic ? "delta" : "score", &sc)) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_zset_put(w, k.p, k.n, sc, magic, &res, &added);
    ds2_str_release(ctx, &k);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "incrBy: resulting score is NaN");
    if (rc < 0)
        return ds2_oom(ctx);
    return magic ? JS_NewFloat64(ctx, res) : JS_NewBool(ctx, added);
}

static JSValue sdb_zset_member_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    dyn_zsl_node* x;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    x = dyn_zsl_find(&w->sl, k.p, k.n);
    ds2_str_release(ctx, &k);
    switch (magic) {
    case 0:
        return x ? JS_NewFloat64(ctx, x->score) : JS_UNDEFINED;
    case 1:
        return JS_NewBool(ctx, x != NULL);
    case 2:
        if (!x)
            return JS_FALSE;
        dyn_zsl_drop(&w->sl, x);
        return JS_TRUE;
    case 3:
        return x ? JS_NewInt64(ctx, (int64_t)dyn_zsl_rank(&w->sl, x) - 1) : JS_UNDEFINED;
    default:
        return x ? JS_NewInt64(ctx, (int64_t)w->sl.len - (int64_t)dyn_zsl_rank(&w->sl, x)) : JS_UNDEFINED;
    }
}

static JSValue sdb_zset_entry(JSContext* ctx, const dyn_zsl_node* x)
{
    return sdb_pair(ctx, dyn_zsl_keystr(ctx, x), JS_NewFloat64(ctx, x->score));
}

static JSValue sdb_zset_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t i;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = dyn_zsl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_zset_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_zset_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    int64_t start = 0, stop = -1;
    int reverse = 0;
    uint32_t a, b, i, out = 0;
    dyn_zsl_node* x;
    JSValue arr;
    if (!magic) {
        if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
            return JS_EXCEPTION;
        if (argc > 2 && sdb_opt_bool(ctx, argv[2], "reverse", &reverse))
            return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || !sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return arr;
    x = reverse ? dyn_zsl_at(&w->sl, w->sl.len - a) : dyn_zsl_at(&w->sl, a + 1);
    for (i = a; x && i <= b; i++) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_by_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    double lo, hi;
    uint32_t offset = 0, limit = UINT32_MAX, r1, r2, out = 0, n;
    int reverse = 0;
    dyn_zsl_node* x;
    JSValue arr;
    if (sdb_num_arg(ctx, argv[0], "min", &lo) || sdb_num_arg(ctx, argv[1], "max", &hi))
        return JS_EXCEPTION;
    if (magic == 0 && argc > 2 && (sdb_opt_u32(ctx, argv[2], "offset", &offset) || sdb_opt_u32(ctx, argv[2], "limit", &limit) || sdb_opt_bool(ctx, argv[2], "reverse", &reverse)))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_first_ge_score(&w->sl, lo, &r1);
    dyn_zsl_last_le_score(&w->sl, hi, &r2);
    n = r2 >= r1 ? r2 - r1 + 1 : 0;
    if (magic == 1)
        return JS_NewInt64(ctx, n);
    if (magic == 2) {
        uint32_t k;
        x = n ? dyn_zsl_at(&w->sl, r1) : NULL;
        for (k = 0; k < n && x; k++) {
            dyn_zsl_node* nx = x->lv[0].fwd;
            dyn_zsl_drop(&w->sl, x);
            x = nx;
        }
        return JS_NewInt64(ctx, k);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || offset >= n)
        return arr;
    x = reverse ? dyn_zsl_at(&w->sl, r2 - offset) : dyn_zsl_at(&w->sl, r1 + offset);
    n -= offset;
    while (x && n-- && out < limit) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_remove_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t start, stop;
    uint32_t a, b, k = 0;
    dyn_zsl_node* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return JS_NewInt32(ctx, 0);
    x = dyn_zsl_at(&w->sl, a + 1);
    while (x && k < b - a + 1) {
        dyn_zsl_node* nx = x->lv[0].fwd;
        dyn_zsl_drop(&w->sl, x);
        x = nx;
        k++;
    }
    return JS_NewInt64(ctx, k);
}

static JSValue sdb_zset_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    dyn_zsl_node* x;
    JSValue r;
    (void)argc;
    (void)argv;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    if (!x)
        return JS_UNDEFINED;
    r = sdb_zset_entry(ctx, x);
    if (JS_IsException(r))
        return r;
    dyn_zsl_drop(&w->sl, x);
    return r;
}

static JSValue sdb_zset_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_zset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    dyn_zsl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_zset, "entries")

const JSCFunctionListEntry sdb_zset_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 2, sdb_zset_add, 0),
    JS_CFUNC_MAGIC_DEF("incrBy", 2, sdb_zset_add, 1),
    JS_CFUNC_MAGIC_DEF("score", 1, sdb_zset_member_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_zset_member_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_zset_member_op, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_zset_member_op, 3),
    JS_CFUNC_MAGIC_DEF("revRank", 1, sdb_zset_member_op, 4),
    JS_CFUNC_DEF("at", 1, sdb_zset_at),
    JS_CFUNC_MAGIC_DEF("range", 2, sdb_zset_range, 0),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_zset_range, 1),
    JS_CFUNC_MAGIC_DEF("rangeByScore", 2, sdb_zset_by_score, 0),
    JS_CFUNC_MAGIC_DEF("countByScore", 2, sdb_zset_by_score, 1),
    JS_CFUNC_MAGIC_DEF("removeRangeByScore", 2, sdb_zset_by_score, 2),
    JS_CFUNC_DEF("removeRangeByRank", 2, sdb_zset_remove_rank),
    JS_CFUNC_MAGIC_DEF("popMin", 0, sdb_zset_pop, 0),
    JS_CFUNC_MAGIC_DEF("popMax", 0, sdb_zset_pop, 1),
    JS_CFUNC_DEF("clear", 0, sdb_zset_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_zset_iterator),
    JS_CGETSET_DEF("size", sdb_zset_size, NULL),
};

int sdb_zset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_zset_w* z = JS_GetOpaque(obj, sdb_zset_class_id);
    dyn_zsl_node* x;
    (void)ctx;
    if (dyn_ser_u32(w, z->sl.len) < 0)
        return -1;
    for (x = z->sl.head->lv[0].fwd; x; x = x->lv[0].fwd)
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || dyn_ser_f64(w, x->score) < 0)
            return -1;
    return 0;
}

JSValue sdb_zset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_zset_w* z;
    (void)opts;
    if (dyn_de_count(r, &n, 12) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 96) < 0)
        return sdb_bad(ctx);
    z = sdb_zset_new(ctx);
    if (!z)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        double sc = dyn_de_f64(r);
        if (!dyn_de_ok(r) || sc != sc || dyn_zsl_find(&z->sl, k, kn)) {
            sdb_zset_free(z);
            return sdb_bad(ctx);
        }
        if (sdb_zsl_insert(&z->sl, sc, k, kn, JS_UNDEFINED, NULL) < 0) {
            sdb_zset_free(z);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_zset_class_id, z, sdb_zset_free);
}

#endif
