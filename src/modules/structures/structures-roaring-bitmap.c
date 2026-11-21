#include "structures-internal.h"
#include "core/ds/ds-roaring-bitmap.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_roar_t r;
} sdb_rb_w;

JSClassID sdb_rb_class_id;

static void sdb_rb_free(void* native)
{
    sdb_rb_w* w = (sdb_rb_w*)native;
    if (!w)
        return;
    dyn_roar_clear(&w->r);
    dyn_ds_free(w);
}

static void sdb_rb_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_rb_free(JS_GetOpaque(val, sdb_rb_class_id));
}

const JSClassDef sdb_rb_class = {
    "RoaringBitmap",
    .finalizer = sdb_rb_finalizer,
};

static int sdb_rb_add_list(JSContext* ctx, sdb_rb_w* w, uint32_t* v, uint32_t n)
{
    uint32_t i;
    qsort(v, n, sizeof(uint32_t), dyn_roar_u32_cmp);
    for (i = 0; i < n; i++)
        if ((i == 0 || v[i] != v[i - 1]) && dyn_roar_add(&w->r, v[i]) < 0) {
            ds2_oom(ctx);
            return -1;
        }
    return 0;
}

JSValue sdb_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t* v = NULL;
    uint32_t n = 0;
    JSValue obj;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && sdb_u32_list(ctx, argv[0], "RoaringBitmap", &v, &n))
        return JS_EXCEPTION;
    w = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w) {
        dyn_ds_free(v);
        return ds2_oom(ctx);
    }
    if (n && sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_ds_free(v);
        sdb_rb_free(w);
        return JS_EXCEPTION;
    }
    dyn_ds_free(v);
    obj = dyn_plain_wrap(ctx, new_target, sdb_rb_class_id, w, sdb_rb_free);
    return obj;
}

static sdb_rb_w* sdb_rb_get(JSContext* ctx, JSValueConst v)
{
    return (sdb_rb_w*)dyn_plain_get(ctx, v, sdb_rb_class_id);
}

static JSValue sdb_rb_elem(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint32_t x;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "value", &x))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (dyn_roar_add(&w->r, x) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    case 1:
        rc = dyn_roar_remove(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    case 2:
        return JS_NewBool(ctx, dyn_roar_has(&w->r, x));
    case 3:
        return JS_NewInt64(ctx, (int64_t)dyn_roar_rank(&w->r, x));
    default:
        rc = dyn_roar_add(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    }
}

static JSValue sdb_rb_add_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t *v, n;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "addMany", &v, &n))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w || sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_ds_free(v);
        return JS_EXCEPTION;
    }
    dyn_ds_free(v);
    return JS_DupValue(ctx, this_val);
}

static int sdb_rb_bounds(JSContext* ctx, JSValueConst a, JSValueConst b, uint64_t* lo, uint64_t* hi)
{
    double x, y;
    if (JS_ToFloat64(ctx, &x, a) || JS_ToFloat64(ctx, &y, b))
        return -1;
    if (!(x >= 0 && x <= 4294967296.0) || x != trunc(x) || !(y >= 0 && y <= 4294967296.0) || y != trunc(y)) {
        JS_ThrowRangeError(ctx, "range bounds must be integers in 0..2^32");
        return -1;
    }
    *lo = (uint64_t)x;
    *hi = (uint64_t)y;
    return 0;
}

static JSValue sdb_rb_range_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint64_t lo, hi;
    (void)argc;
    if (sdb_rb_bounds(ctx, argv[0], argv[1], &lo, &hi))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 2) {
        uint64_t a = lo ? dyn_roar_rank(&w->r, (uint32_t)(lo - 1)) : 0;
        uint64_t b = hi ? dyn_roar_rank(&w->r, (uint32_t)(hi - 1)) : 0;
        return JS_NewInt64(ctx, hi > lo ? (int64_t)(b - a) : 0);
    }
    if (dyn_roar_range(&w->r, lo, hi, magic == 0) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_rb_setop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w *w, *o;
    (void)argc;
    o = sdb_rb_get(ctx, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic < 4) {
        if (dyn_roar_op(&w->r, &o->r, magic) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    }
    if (magic < 8)
        return JS_NewInt64(ctx, (int64_t)dyn_roar_op_card(&w->r, &o->r, magic - 4));
    {
        uint64_t inter = dyn_roar_op_card(&w->r, &o->r, DYN_ROAR_AND);
        uint64_t ca = dyn_roar_card(&w->r), cb = dyn_roar_card(&o->r);
        switch (magic) {
        case 8:
            return JS_NewBool(ctx, inter != 0);
        case 9:
            return JS_NewBool(ctx, inter == ca);
        case 10:
            return JS_NewBool(ctx, inter == ca && ca == cb);
        default:
            return JS_NewFloat64(ctx, ca + cb - inter ? (double)inter / (double)(ca + cb - inter) : 1.0);
        }
    }
}

static JSValue sdb_rb_misc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint32_t x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!w->r.n)
            return JS_UNDEFINED;
        return dyn_roar_select(&w->r, 0, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 1:
        if (!w->r.n)
            return JS_UNDEFINED;
        return dyn_roar_select(&w->r, dyn_roar_card(&w->r) - 1, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 2: {
        uint64_t n = dyn_roar_card(&w->r);
        uint32_t* buf;
        JSValue out;
        if (n > DYN_DS_MAX_ITEMS)
            return JS_ThrowRangeError(ctx, "toArray: %llu values exceed the %u materialisation limit", (unsigned long long)n, DYN_DS_MAX_ITEMS);
        buf = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (size_t)(n + 1));
        if (!buf)
            return ds2_oom(ctx);
        dyn_roar_fill(&w->r, buf);
        out = sdb_typed_out(ctx, buf, (size_t)n, JS_TYPED_ARRAY_UINT32);
        dyn_ds_free(buf);
        return out;
    }
    case 3: {
        sdb_rb_w* c = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*c));
        if (!c || dyn_roar_clone(&c->r, &w->r) < 0) {
            dyn_ds_free(c);
            return ds2_oom(ctx);
        }
        return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, c, sdb_rb_free);
    }
    case 4:
        dyn_roar_clear(&w->r);
        return JS_UNDEFINED;
    default:
        return JS_NewBool(ctx, w->r.n == 0);
    }
}

static JSValue sdb_rb_select_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    double d;
    uint32_t x;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= 4294967296.0)
        return JS_UNDEFINED;
    return dyn_roar_select(&w->r, (uint64_t)d, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_rb_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint64_t b = 0;
    uint32_t i;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)dyn_roar_card(&w->r));
    b = sizeof(dyn_roar_ct) * (uint64_t)w->r.cap;
    for (i = 0; i < w->r.n; i++) {
        const dyn_roar_ct* c = &w->r.c[i];
        b += c->type == DYN_ROAR_ARR ? 2u * c->cap : c->type == DYN_ROAR_BMP ? 8u * DYN_ROAR_WORDS
                                                                             : 0;
    }
    return JS_NewInt64(ctx, (int64_t)b);
}

DS2_ITERATOR(sdb_rb, "toArray")

const JSCFunctionListEntry sdb_rb_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_rb_elem, 0),
    JS_CFUNC_MAGIC_DEF("tryAdd", 1, sdb_rb_elem, 4),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_rb_elem, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_rb_elem, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_rb_elem, 3),
    JS_CFUNC_DEF("addMany", 1, sdb_rb_add_many),
    JS_CFUNC_MAGIC_DEF("addRange", 2, sdb_rb_range_op, 0),
    JS_CFUNC_MAGIC_DEF("removeRange", 2, sdb_rb_range_op, 1),
    JS_CFUNC_MAGIC_DEF("rangeCardinality", 2, sdb_rb_range_op, 2),
    JS_CFUNC_MAGIC_DEF("and", 1, sdb_rb_setop, DYN_ROAR_AND),
    JS_CFUNC_MAGIC_DEF("or", 1, sdb_rb_setop, DYN_ROAR_OR),
    JS_CFUNC_MAGIC_DEF("xor", 1, sdb_rb_setop, DYN_ROAR_XOR),
    JS_CFUNC_MAGIC_DEF("andNot", 1, sdb_rb_setop, DYN_ROAR_ANDNOT),
    JS_CFUNC_MAGIC_DEF("andCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_AND),
    JS_CFUNC_MAGIC_DEF("orCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_OR),
    JS_CFUNC_MAGIC_DEF("xorCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_XOR),
    JS_CFUNC_MAGIC_DEF("andNotCardinality", 1, sdb_rb_setop, 4 + DYN_ROAR_ANDNOT),
    JS_CFUNC_MAGIC_DEF("intersects", 1, sdb_rb_setop, 8),
    JS_CFUNC_MAGIC_DEF("isSubsetOf", 1, sdb_rb_setop, 9),
    JS_CFUNC_MAGIC_DEF("equals", 1, sdb_rb_setop, 10),
    JS_CFUNC_MAGIC_DEF("jaccard", 1, sdb_rb_setop, 11),
    JS_CFUNC_DEF("select", 1, sdb_rb_select_m),
    JS_CFUNC_MAGIC_DEF("min", 0, sdb_rb_misc, 0),
    JS_CFUNC_MAGIC_DEF("max", 0, sdb_rb_misc, 1),
    JS_CFUNC_MAGIC_DEF("toArray", 0, sdb_rb_misc, 2),
    JS_CFUNC_MAGIC_DEF("clone", 0, sdb_rb_misc, 3),
    JS_CFUNC_MAGIC_DEF("clear", 0, sdb_rb_misc, 4),
    JS_CFUNC_MAGIC_DEF("isEmpty", 0, sdb_rb_misc, 5),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_rb_iterator),
    JS_CGETSET_MAGIC_DEF("size", sdb_rb_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_rb_getter, NULL, 2),
};

int sdb_rb_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_rb_w* b = JS_GetOpaque(obj, sdb_rb_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, b->r.n) < 0)
        return -1;
    for (i = 0; i < b->r.n; i++) {
        const dyn_roar_ct* c = &b->r.c[i];
        if (dyn_ser_u16(w, c->key) < 0 || dyn_ser_u8(w, c->type) < 0 || dyn_ser_u32(w, c->card) < 0)
            return -1;
        if (c->type == DYN_ROAR_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            uint32_t j;
            for (j = 0; j < c->card; j++)
                if (dyn_ser_u16(w, a[j]) < 0)
                    return -1;
        } else if (c->type == DYN_ROAR_BMP) {
            const uint64_t* x = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < DYN_ROAR_WORDS; j++)
                if (dyn_ser_u64(w, x[j]) < 0)
                    return -1;
        }
    }
    return 0;
}

JSValue sdb_rb_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_rb_w* b;
    (void)opts;
    if (dyn_de_count(r, &n, 7) < 0 || n > 65536)
        return sdb_bad(ctx);
    b = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*b));
    if (!b || dyn_roar_reserve(&b->r, n) < 0) {
        dyn_ds_free(b);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        dyn_roar_ct* c = &b->r.c[i];
        uint16_t key = dyn_de_u16(r);
        uint8_t type = dyn_de_u8(r);
        uint32_t card = dyn_de_u32(r), j;
        memset(c, 0, sizeof(*c));
        if (!dyn_de_ok(r) || (i && key <= b->r.c[i - 1].key))
            goto bad;
        c->key = key;
        if (type == DYN_ROAR_ARR) {
            uint16_t* a;
            if (card == 0 || card > DYN_ROAR_ARR_MAX || dyn_de_left(r) < (size_t)card * 2 || dyn_de_charge(r, (uint64_t)card * 2) < 0)
                goto bad;
            a = (uint16_t*)dyn_ds_malloc(sizeof(uint16_t) * card);
            if (!a)
                goto oom;
            for (j = 0; j < card; j++) {
                a[j] = dyn_de_u16(r);
                if (j && a[j] <= a[j - 1]) {
                    dyn_ds_free(a);
                    goto bad;
                }
            }
            c->type = DYN_ROAR_ARR;
            c->data = a;
            c->cap = card;
            c->card = card;
        } else if (type == DYN_ROAR_BMP) {
            uint64_t* x;
            if (card <= DYN_ROAR_ARR_MAX || card >= 65536 || dyn_de_left(r) < 8u * DYN_ROAR_WORDS || dyn_de_charge(r, 8u * DYN_ROAR_WORDS) < 0)
                goto bad;
            x = (uint64_t*)dyn_ds_malloc(sizeof(uint64_t) * DYN_ROAR_WORDS);
            if (!x)
                goto oom;
            for (j = 0; j < DYN_ROAR_WORDS; j++)
                x[j] = dyn_de_u64(r);
            if (dyn_roar_popcnt(x) != card) {
                dyn_ds_free(x);
                goto bad;
            }
            c->type = DYN_ROAR_BMP;
            c->data = x;
            c->card = card;
        } else if (type == DYN_ROAR_FULL) {
            if (card != 65536)
                goto bad;
            c->type = DYN_ROAR_FULL;
            c->card = 65536;
        } else
            goto bad;
        if (!dyn_de_ok(r))
            goto bad_inc;
        b->r.n = i + 1;
        continue;
    bad_inc:
        b->r.n = i + 1;
        goto bad;
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, b, sdb_rb_free);
bad:
    sdb_rb_free(b);
    return sdb_bad(ctx);
oom:
    sdb_rb_free(b);
    return ds2_oom(ctx);
}

JSValue sdb_rb_wrap_roar(JSContext* ctx, dyn_roar_t* r)
{
    sdb_rb_w* w = (sdb_rb_w*)dyn_ds_calloc(1, sizeof(*w));
    if (!w) {
        dyn_roar_clear(r);
        return ds2_oom(ctx);
    }
    w->r = *r;
    memset(r, 0, sizeof(*r));
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, w, sdb_rb_free);
}

#endif
