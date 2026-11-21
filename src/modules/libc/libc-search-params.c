#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-pct.h"
#include "core/dyn-sb.h"
#include "core/dyn-urlrec.h"
#include "cutils.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define DYN_SP_MAX_PAIRS 65536

typedef struct {
    JSValue sp;
    size_t idx;
    int mode;
    int finished;
} dyn_spi_t;

JSClassID dyn_sp_class_id;
static JSClassID dyn_spi_class_id;
static JSClassID dyn_sp_holder_id;

static dyn_sp_t* dyn_sp_of(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_sp_t*)dyn_plain_get(ctx, this_val, dyn_sp_class_id);
}

static int dyn_sp_kv(JSContext* ctx, int argc, JSValueConst* argv,
    const char** ks, size_t* kn,
    const char** vs, size_t* vn)
{
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "URLSearchParams method: a key is required");
        return -1;
    }
    *ks = JS_ToCStringLenUTF8(ctx, kn, argv[0]);
    if (!*ks)
        return -1;
    if (!vs || argc < 2 || JS_IsUndefined(argv[1])) {
        if (vs)
            *vs = NULL;
        if (vn)
            *vn = 0;
    } else {
        const char* v = JS_ToCStringLenUTF8(ctx, vn, argv[1]);
        if (!v) {
            JS_FreeCString(ctx, *ks);
            return -1;
        }
        if (vs)
            *vs = v;
    }
    return 0;
}

static JSValue dyn_sp_append(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    if (sp->list_valid && sp->list_canonical) {
        const char* q = dyn_sp_query_str(sp);
        size_t ql = strlen(q);
        dyn_sb_t out;
        dyn_sb_init(&out);
        dyn_sb_put(&out, q, ql);
        if (ql)
            dyn_sb_putc(&out, '&');
        dyn_pct_encode(&out, ks, kn, 2);
        dyn_sb_putc(&out, '=');
        dyn_pct_encode(&out, vs ? vs : "", vn, 2);
        if (out.oom || dyn_sp_set_query(sp, out.p ? out.p : "", out.n) < 0) {
            dyn_sb_free(&out);
            JS_FreeCString(ctx, ks);
            if (vs)
                JS_FreeCString(ctx, vs);
            return JS_ThrowOutOfMemory(ctx);
        }
        dyn_sb_free(&out);
    } else {
        dyn_sp_list_t* l = dyn_sp_list_ref(sp);
        if (dyn_sp_list_put(l, ks, kn, vs ? vs : "", vn) < 0 || dyn_sp_apply(sp, l) < 0) {
            dyn_sp_cache_drop(sp);
            JS_FreeCString(ctx, ks);
            if (vs)
                JS_FreeCString(ctx, vs);
            return JS_ThrowOutOfMemory(ctx);
        }
        sp->cached_version = dyn_sp_version(sp);
        sp->list_canonical = 1;
    }
    JS_FreeCString(ctx, ks);
    if (vs)
        JS_FreeCString(ctx, vs);
    return JS_UNDEFINED;
}

static JSValue dyn_sp_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    const char* ks = NULL;
    size_t kn = 0, i;
    JSValue r = JS_NULL;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, NULL, NULL) < 0)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    for (i = 0; i < l->n; i++)
        if (dyn_sp_key_is(&l->a[i], ks, kn)) {
            r = JS_NewStringLen(ctx, l->a[i].v, l->a[i].vn);
            break;
        }
    JS_FreeCString(ctx, ks);
    return r;
}

static JSValue dyn_sp_get_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    const char* ks = NULL;
    size_t kn = 0, i;
    JSValue arr;
    uint32_t n = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, NULL, NULL) < 0)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeCString(ctx, ks);
        return JS_EXCEPTION;
    }
    for (i = 0; i < l->n; i++)
        if (dyn_sp_key_is(&l->a[i], ks, kn))
            JS_DefinePropertyValueUint32(ctx, arr, n++, JS_NewStringLen(ctx, l->a[i].v, l->a[i].vn), JS_PROP_C_W_E);
    JS_FreeCString(ctx, ks);
    return arr;
}

static JSValue dyn_sp_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0, i;
    JSValue r = JS_FALSE;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    for (i = 0; i < l->n; i++)
        if (dyn_sp_key_is(&l->a[i], ks, kn) && (!vs || dyn_sp_val_is(&l->a[i], vs, vn))) {
            r = JS_TRUE;
            break;
        }
    JS_FreeCString(ctx, ks);
    if (vs)
        JS_FreeCString(ctx, vs);
    return r;
}

static JSValue dyn_sp_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0, i, w = 0;
    int rc = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    for (i = 0; i < l->n; i++)
        if (!dyn_sp_key_is(&l->a[i], ks, kn) || (vs && !dyn_sp_val_is(&l->a[i], vs, vn))) {
            if (w != i) {
                l->a[w] = l->a[i];
            }
            w++;
        } else {
            free(l->a[i].k);
            free(l->a[i].v);
        }
    l->n = w;
    if (dyn_sp_apply(sp, l) < 0) {
        rc = -1;
        dyn_sp_cache_drop(sp);
    } else {
        sp->cached_version = dyn_sp_version(sp);
        sp->list_canonical = 1;
    }
    JS_FreeCString(ctx, ks);
    if (vs)
        JS_FreeCString(ctx, vs);
    return rc ? JS_ThrowOutOfMemory(ctx) : JS_UNDEFINED;
}

static JSValue dyn_sp_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0, i, w = 0;
    int replaced = 0, rc = -1;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    for (i = 0; i < l->n; i++) {
        if (!replaced && dyn_sp_key_is(&l->a[i], ks, kn)) {
            char* nv = dyn_strndup(vs ? vs : "", vn);
            if (!nv)
                goto done;
            free(l->a[i].v);
            l->a[i].v = nv;
            l->a[i].vn = vn;
            replaced = 1;
            w++;
        } else if (!dyn_sp_key_is(&l->a[i], ks, kn)) {
            if (w != i) {
                l->a[w] = l->a[i];
            }
            w++;
        } else {
            free(l->a[i].k);
            free(l->a[i].v);
        }
    }
    if (!replaced) {
        if (dyn_sp_list_put(l, ks, kn, vs ? vs : "", vn) < 0)
            goto done;
        w++;
    }
    l->n = w;
    if (dyn_sp_apply(sp, l) == 0) {
        rc = 0;
        sp->cached_version = dyn_sp_version(sp);
        sp->list_canonical = 1;
    } else {
        dyn_sp_cache_drop(sp);
    }
done:
    if (rc)
        dyn_sp_cache_drop(sp);
    JS_FreeCString(ctx, ks);
    if (vs)
        JS_FreeCString(ctx, vs);
    return rc ? JS_ThrowOutOfMemory(ctx) : JS_UNDEFINED;
}

static JSValue dyn_sp_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    dyn_sp_sort_ent_t* ent;
    size_t i;
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    ent = (dyn_sp_sort_ent_t*)malloc((l->n ? l->n : 1) * sizeof(*ent));
    if (!ent)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < l->n; i++) {
        ent[i].p = l->a[i];
        ent[i].idx = i;
    }
    qsort(ent, l->n, sizeof(ent[0]), dyn_sp_cmp);
    for (i = 0; i < l->n; i++)
        l->a[i] = ent[i].p;
    free(ent);
    if (dyn_sp_apply(sp, l) < 0) {
        dyn_sp_cache_drop(sp);
        return JS_ThrowOutOfMemory(ctx);
    }
    sp->cached_version = dyn_sp_version(sp);
    sp->list_canonical = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_sp_tostring(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    dyn_sb_t out;
    JSValue r;
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    dyn_sb_init(&out);
    if (dyn_sp_serialize(l, &out) < 0) {
        dyn_sb_free(&out);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
    dyn_sb_free(&out);
    return r;
}

static JSValue dyn_sp_size(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t* l;
    JSValue r;
    (void)magic;
    if (!sp)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    r = JS_NewInt32(ctx, (int32_t)l->n);
    return r;
}

static JSValue dyn_sp_seq(JSContext* ctx, dyn_sp_t* sp, int mode)
{
    dyn_sp_list_t* l;
    size_t i;
    uint32_t n = 0;
    JSValue arr;
    l = dyn_sp_list_ref(sp);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    for (i = 0; i < l->n; i++) {
        JSValue v;
        if (mode == 1) {
            v = JS_NewStringLen(ctx, l->a[i].v, l->a[i].vn);
        } else if (mode == 2) {
            JSValue p = JS_NewArray(ctx);
            if (JS_IsException(p)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            JS_DefinePropertyValueUint32(ctx, p, 0, JS_NewStringLen(ctx, l->a[i].k, l->a[i].kn), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, p, 1, JS_NewStringLen(ctx, l->a[i].v, l->a[i].vn), JS_PROP_C_W_E);
            v = p;
        } else {
            v = JS_NewStringLen(ctx, l->a[i].k, l->a[i].kn);
        }
        JS_DefinePropertyValueUint32(ctx, arr, n++, v, JS_PROP_C_W_E);
    }
    return arr;
}

static void dyn_spi_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spi_t* it = (dyn_spi_t*)JS_GetOpaque(val, dyn_spi_class_id);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->sp);
    free(it);
}

static void dyn_spi_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    dyn_spi_t* it = (dyn_spi_t*)JS_GetOpaque(val, dyn_spi_class_id);
    if (it)
        JS_MarkValue(rt, it->sp, mark);
}

static JSValue dyn_spi_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spi_t* it = (dyn_spi_t*)dyn_plain_get(ctx, this_val, dyn_spi_class_id);
    dyn_sp_t* sp;
    dyn_sp_list_t* l;
    JSValue res, val;
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    sp = (dyn_sp_t*)dyn_plain_get(ctx, it->sp, dyn_sp_class_id);
    if (!sp)
        return JS_EXCEPTION;
    l = dyn_sp_list_ref(sp);
    if (it->finished || it->idx >= l->n) {
        JSValue it_res;
        it->finished = 1;
        it_res = JS_NewObject(ctx);
        if (JS_IsException(it_res))
            return it_res;
        JS_DefinePropertyValueStr(ctx, it_res, "value", JS_UNDEFINED, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, it_res, "done", JS_TRUE, JS_PROP_C_W_E);
        return it_res;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    if (it->mode == 2) {
        JSValue pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) {
            JS_FreeValue(ctx, res);
            return pair;
        }
        JS_DefinePropertyValueUint32(ctx, pair, 0, JS_NewStringLen(ctx, l->a[it->idx].k, l->a[it->idx].kn), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, pair, 1, JS_NewStringLen(ctx, l->a[it->idx].v, l->a[it->idx].vn), JS_PROP_C_W_E);
        val = pair;
    } else if (it->mode == 1) {
        val = JS_NewStringLen(ctx, l->a[it->idx].v, l->a[it->idx].vn);
    } else {
        val = JS_NewStringLen(ctx, l->a[it->idx].k, l->a[it->idx].kn);
    }
    if (JS_IsException(val)) {
        JS_FreeValue(ctx, res);
        return val;
    }
    it->idx++;
    JS_DefinePropertyValueStr(ctx, res, "value", val, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "done", JS_FALSE, JS_PROP_C_W_E);
    return res;
}

static JSValue dyn_spi_self(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_sp_iter_new(JSContext* ctx, JSValueConst this_val, int mode)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_spi_t* it;
    JSValue proto, obj;
    if (!sp)
        return JS_EXCEPTION;
    it = (dyn_spi_t*)calloc(1, sizeof(*it));
    if (!it)
        return JS_ThrowOutOfMemory(ctx);
    it->sp = JS_DupValue(ctx, this_val);
    it->mode = mode;
    proto = JS_GetClassProto(ctx, dyn_spi_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_spi_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValueRT(JS_GetRuntime(ctx), it->sp);
        free(it);
        return obj;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

static JSValue dyn_sp_keys(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_sp_iter_new(ctx, this_val, 0);
}

static JSValue dyn_sp_values_iter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_sp_iter_new(ctx, this_val, 1);
}

static JSValue dyn_sp_entries(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_sp_iter_new(ctx, this_val, 2);
}

static JSValue dyn_sp_symbol_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_sp_iter_new(ctx, this_val, 2);
}

static JSValue dyn_sp_keys_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 0);
}

static JSValue dyn_sp_values_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 1);
}

static JSValue dyn_sp_entries_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 2);
}

static JSValue dyn_sp_for_each(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    size_t i;
    if (!sp)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        JS_ThrowTypeError(ctx, "forEach(callback[, thisArg]): callback must be a function");
        return JS_EXCEPTION;
    }
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    for (i = 0; i < l.n; i++) {
        JSValue args[3], r;
        args[0] = JS_NewStringLen(ctx, l.a[i].v, l.a[i].vn);
        args[1] = JS_NewStringLen(ctx, l.a[i].k, l.a[i].kn);
        args[2] = JS_DupValue(ctx, this_val);
        r = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        if (JS_IsException(r)) {
            dyn_sp_list_free(&l);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
    }
    dyn_sp_list_free(&l);
    return JS_UNDEFINED;
}

static int dyn_sp_enc_pair(JSContext* ctx, dyn_sb_t* out, int* first,
    JSValue k, JSValue v)
{
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        v = JS_NewString(ctx, "");
    } else if (!JS_IsString(v)) {
        JSValue nv = JS_ToString(ctx, v);
        JS_FreeValue(ctx, v);
        v = nv;
    }
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    if (!JS_IsString(k)) {
        JSValue nk = JS_ToString(ctx, k);
        JS_FreeValue(ctx, k);
        k = nk;
    }
    if (JS_IsException(k)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    ks = JS_ToCStringLenUTF8(ctx, &kn, k);
    vs = JS_ToCStringLenUTF8(ctx, &vn, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, k);
    if (!ks || !vs) {
        if (ks)
            JS_FreeCString(ctx, ks);
        if (vs)
            JS_FreeCString(ctx, vs);
        return -1;
    }
    if (!*first)
        dyn_sb_putc(out, '&');
    dyn_pct_encode(out, ks, kn, 2);
    dyn_sb_putc(out, '=');
    dyn_pct_encode(out, vs, vn, 2);
    JS_FreeCString(ctx, ks);
    JS_FreeCString(ctx, vs);
    if (out->oom)
        return -1;
    *first = 0;
    return 0;
}

JSValue dyn_sp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_sp_t* sp;
    (void)new_target;
    if (argc > 0 && (JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])))
        argc = 0;
    sp = (dyn_sp_t*)calloc(1, sizeof(*sp));
    if (!sp)
        return JS_ThrowOutOfMemory(ctx);
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s, *owned;
        size_t n;
        owned = JS_ToCStringLenUTF8(ctx, &n, argv[0]);
        if (!owned) {
            free(sp);
            return JS_EXCEPTION;
        }
        s = owned;
        if (n && s[0] == '?') {
            s++;
            n--;
        }
        if (memchr(s, 0, n)) {
            dyn_sp_list_t pl;
            dyn_sb_t enc;
            dyn_sp_list_init(&pl);
            dyn_sb_init(&enc);
            dyn_sp_parse(s, n, &pl);
            sp->query = dyn_sp_serialize(&pl, &enc) == 0 ? dyn_strndup(enc.p ? enc.p : "", enc.n) : NULL;
            dyn_sb_free(&enc);
            dyn_sp_list_free(&pl);
        } else
            sp->query = dyn_strndup(s, n);
        JS_FreeCString(ctx, owned);
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    } else if (argc > 0 && JS_GetOpaque(argv[0], dyn_sp_class_id) != NULL) {
        dyn_sp_t* src = (dyn_sp_t*)JS_GetOpaque(argv[0], dyn_sp_class_id);
        const char* q = dyn_sp_query_str(src);
        sp->query = dyn_strndup(q, strlen(q));
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    } else if (argc > 0 && JS_IsObject(argv[0])) {
        dyn_sb_t out;
        int first = 1;
        dyn_sb_init(&out);
        if (JS_IsArray(ctx, argv[0])) {
            uint32_t len, i;
            if (dyn_js_len_u32(ctx, argv[0], &len)) {
                dyn_sb_free(&out);
                free(sp);
                return JS_EXCEPTION;
            }
            if (len > DYN_SP_MAX_PAIRS) {
                dyn_sb_free(&out);
                free(sp);
                return JS_ThrowRangeError(ctx, "new URLSearchParams(): too many pairs");
            }
            for (i = 0; i < len; i++) {
                JSValue pr = JS_GetPropertyUint32(ctx, argv[0], i);
                JSValue k, v;
                int64_t prlen = 0;
                if (JS_IsException(pr)) {
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (!JS_IsArray(ctx, pr)) {
                    JS_FreeValue(ctx, pr);
                    continue;
                }
                if (dyn_js_len(ctx, pr, &prlen)) {
                    JS_FreeValue(ctx, pr);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (prlen == 0) {
                    JS_FreeValue(ctx, pr);
                    continue;
                }
                k = JS_GetPropertyUint32(ctx, pr, 0);
                v = prlen >= 2 ? JS_GetPropertyUint32(ctx, pr, 1) : JS_UNDEFINED;
                JS_FreeValue(ctx, pr);
                if (JS_IsException(k) || JS_IsException(v)) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (dyn_sp_enc_pair(ctx, &out, &first, k, v) < 0) {
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
            }
        } else {
            JSPropertyEnum* tab = NULL;
            uint32_t len = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                < 0) {
                dyn_sb_free(&out);
                free(sp);
                return JS_EXCEPTION;
            }
            for (i = 0; i < len; i++) {
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, argv[0], tab[i].atom);
                if (JS_IsException(k) || JS_IsException(v)) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                    JS_FreePropertyEnum(ctx, tab, len);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (dyn_sp_enc_pair(ctx, &out, &first, k, v) < 0) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
        if (out.oom) {
            dyn_sb_free(&out);
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
        sp->query = dyn_strndup(out.p ? out.p : "", out.n);
        dyn_sb_free(&out);
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    {
        JSValue proto, obj;
        if (JS_IsUndefined(new_target)) {
            proto = JS_GetClassProto(ctx, dyn_sp_class_id);
        } else {
            proto = JS_GetPropertyStr(ctx, new_target, "prototype");
            if (JS_IsException(proto)) {
                dyn_sp_free(sp);
                return proto;
            }
        }
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            proto = JS_GetClassProto(ctx, dyn_sp_class_id);
        }
        obj = JS_NewObjectProtoClass(ctx, proto, dyn_sp_class_id);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj)) {
            dyn_sp_free(sp);
            return obj;
        }
        JS_SetOpaque(obj, sp);
        return obj;
    }
}

static void dyn_sp_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_sp_t* sp = (dyn_sp_t*)JS_GetOpaque(val, dyn_sp_class_id);
    (void)rt;
    if (sp)
        dyn_sp_free(sp);
}

static const JSClassDef dyn_sp_class = { "URLSearchParams",
    .finalizer = dyn_sp_finalizer };

static const JSCFunctionListEntry dyn_sp_proto[] = {
    JS_CFUNC_DEF("append", 2, dyn_sp_append),
    JS_CFUNC_DEF("delete", 1, dyn_sp_delete),
    JS_CFUNC_DEF("get", 1, dyn_sp_get),
    JS_CFUNC_DEF("getAll", 1, dyn_sp_get_all),
    JS_CFUNC_DEF("has", 1, dyn_sp_has),
    JS_CFUNC_DEF("set", 2, dyn_sp_set),
    JS_CFUNC_DEF("sort", 0, dyn_sp_sort),
    JS_CFUNC_DEF("toString", 0, dyn_sp_tostring),
    JS_CFUNC_DEF("forEach", 1, dyn_sp_for_each),
    JS_CFUNC_DEF("keys", 0, dyn_sp_keys),
    JS_CFUNC_DEF("values", 0, dyn_sp_values_iter),
    JS_CFUNC_DEF("entries", 0, dyn_sp_entries),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_sp_symbol_iterator),
    JS_CFUNC_DEF("keysArray", 0, dyn_sp_keys_array),
    JS_CFUNC_DEF("valuesArray", 0, dyn_sp_values_array),
    JS_CFUNC_DEF("entriesArray", 0, dyn_sp_entries_array),
    JS_CGETSET_MAGIC_DEF("size", dyn_sp_size, NULL, 0),
};

JSValue dyn_form_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *owned, *s;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "formDecode(text): argument must be a string");
    owned = JS_ToCStringLenUTF8(ctx, &n, argv[0]);
    if (!owned)
        return JS_EXCEPTION;
    s = owned;
    if (n && s[0] == '?') {
        s++;
        n--;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    while (i < n) {
        size_t b = i, eq = (size_t)-1;
        dyn_sb_t k, v;
        JSAtom a;
        while (i < n && s[i] != '&') {
            if (s[i] == '=' && eq == (size_t)-1)
                eq = i;
            i++;
        }
        if (i > b) {
            dyn_sb_init(&k);
            dyn_sb_init(&v);
            if (eq == (size_t)-1) {
                dyn_sb_reserve((void**)&k.p, &k.cap, i - b, i - b);
                dyn_pct_decode(&k, s + b, i - b, 1);
            } else {
                dyn_sb_reserve((void**)&k.p, &k.cap, eq - b, eq - b);
                dyn_sb_reserve((void**)&v.p, &v.cap, i - eq - 1, i - eq - 1);
                dyn_pct_decode(&k, s + b, eq - b, 1);
                dyn_pct_decode(&v, s + eq + 1, i - eq - 1, 1);
            }
            {
                JSValue ks = dyn_nat_new_string_wf(ctx, k.p ? k.p : "", k.n);
                a = JS_IsException(ks) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, ks);
                JS_FreeValue(ctx, ks);
            }
            if (a != JS_ATOM_NULL) {
                JS_DefinePropertyValue(ctx, obj, a,
                    dyn_nat_new_string_wf(ctx, v.p ? v.p : "", v.n), JS_PROP_C_W_E);
                JS_FreeAtom(ctx, a);
            }
            dyn_sb_free(&k);
            dyn_sb_free(&v);
        }
        i++;
    }
    JS_FreeCString(ctx, owned);
    return obj;
}

JSValue dyn_form_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    dyn_sb_t b;
    JSValue ret = JS_EXCEPTION;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "formEncode(obj): argument must be an object");
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    for (i = 0; i < len; i++) {
        JSValue kv = JS_AtomToString(ctx, tab[i].atom);
        JSValue pv = JS_GetProperty(ctx, argv[0], tab[i].atom);
        const char *ks = NULL, *vs = NULL;
        size_t kn = 0, vn = 0;
        if (JS_IsException(kv) || JS_IsException(pv)) {
            JS_FreeValue(ctx, kv);
            JS_FreeValue(ctx, pv);
            goto done;
        }
        if (JS_IsUndefined(pv)) {
            JS_FreeValue(ctx, kv);
            JS_FreeValue(ctx, pv);
            continue;
        }
        ks = JS_ToCStringLenUTF8(ctx, &kn, kv);
        vs = JS_ToCStringLenUTF8(ctx, &vn, pv);
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, pv);
        if (!ks || !vs) {
            if (ks)
                JS_FreeCString(ctx, ks);
            if (vs)
                JS_FreeCString(ctx, vs);
            goto done;
        }
        if (b.n)
            dyn_sb_putc(&b, '&');
        dyn_pct_encode(&b, ks, kn, 1);
        dyn_sb_putc(&b, '=');
        dyn_pct_encode(&b, vs, vn, 1);
        JS_FreeCString(ctx, ks);
        JS_FreeCString(ctx, vs);
    }
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
done:
    dyn_sb_free(&b);
    JS_FreePropertyEnum(ctx, tab, len);
    return ret;
}

static const JSClassDef dyn_spi_class = { "URLSearchParams Iterator",
    .finalizer = dyn_spi_finalizer,
    .gc_mark = dyn_spi_gc_mark };

static const JSCFunctionListEntry dyn_spi_proto[] = {
    JS_CFUNC_DEF("next", 0, dyn_spi_next),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_spi_self),
};

static const JSClassDef dyn_sp_holder_class = { "URLSearchParamsHolder" };

static int dyn_sp_class_ensure(JSRuntime* rt, JSClassID* pid, const JSClassDef* def)
{
    JS_NewClassID(pid);
    if (JS_IsRegisteredClass(rt, *pid))
        return 0;
    return JS_NewClass(rt, *pid, def);
}

static int dyn_spi_proto_install(JSContext* ctx)
{
    JSValue proto = JS_NewObject(ctx), arr, arrproto, values, arriter, aip, iterproto, g, sym, tagatom;
    JSAtom katom;
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_spi_proto, countof(dyn_spi_proto));
    arr = JS_NewArray(ctx);
    arrproto = JS_GetPrototype(ctx, arr);
    values = JS_GetPropertyStr(ctx, arrproto, "values");
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, arrproto);
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, proto);
        return -1;
    }
    arriter = JS_Call(ctx, values, arr, 0, NULL);
    aip = JS_GetPrototype(ctx, arriter);
    iterproto = JS_GetPrototype(ctx, aip);
    if (JS_IsObject(iterproto))
        JS_SetPrototype(ctx, proto, iterproto);
    JS_FreeValue(ctx, iterproto);
    JS_FreeValue(ctx, aip);
    JS_FreeValue(ctx, arriter);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, arrproto);
    JS_FreeValue(ctx, arr);
    g = JS_GetGlobalObject(ctx);
    sym = JS_GetPropertyStr(ctx, g, "Symbol");
    tagatom = JS_IsException(sym) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sym, "toStringTag");
    if (JS_IsException(tagatom)) {
        JS_FreeValue(ctx, sym);
        JS_FreeValue(ctx, g);
        JS_FreeValue(ctx, proto);
        return -1;
    }
    katom = JS_ValueToAtom(ctx, tagatom);
    JS_DefinePropertyValue(ctx, proto, katom, JS_NewString(ctx, "URLSearchParams Iterator"), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, katom);
    JS_FreeValue(ctx, tagatom);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
    JS_SetClassProto(ctx, dyn_spi_class_id, proto);
    return 0;
}

JSValue dyn_sp_constructor(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue holder, proto, ctor;
    if (dyn_sp_class_ensure(rt, &dyn_sp_class_id, &dyn_sp_class) < 0
        || dyn_sp_class_ensure(rt, &dyn_spi_class_id, &dyn_spi_class) < 0
        || dyn_sp_class_ensure(rt, &dyn_sp_holder_id, &dyn_sp_holder_class) < 0)
        return JS_ThrowInternalError(ctx, "URLSearchParams: class registration failed");
    holder = JS_GetClassProto(ctx, dyn_sp_holder_id);
    if (JS_IsObject(holder)) {
        ctor = JS_GetPropertyStr(ctx, holder, "ctor");
        JS_FreeValue(ctx, holder);
        return ctor;
    }
    JS_FreeValue(ctx, holder);
    if (dyn_spi_proto_install(ctx) < 0)
        return JS_EXCEPTION;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return proto;
    JS_SetPropertyFunctionList(ctx, proto, dyn_sp_proto, countof(dyn_sp_proto));
    JS_SetClassProto(ctx, dyn_sp_class_id, JS_DupValue(ctx, proto));
    ctor = JS_NewCFunction2(ctx, dyn_sp_ctor, "URLSearchParams", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return ctor;
    }
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    holder = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(holder)) {
        JS_FreeValue(ctx, ctor);
        return holder;
    }
    JS_SetPropertyStr(ctx, holder, "ctor", JS_DupValue(ctx, ctor));
    JS_SetClassProto(ctx, dyn_sp_holder_id, holder);
    return ctor;
}

void dyn_sp_install(JSContext* ctx, JSValueConst global_obj)
{
    JSValue ctor = dyn_sp_constructor(ctx);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    JS_DefinePropertyValueStr(ctx, global_obj, "URLSearchParams", ctor, JS_PROP_C_W_E);
}
