#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dyna-nat.h"
#include "core/dyn-veck.h"
#include "core/dyn-persist.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VEC)

#ifdef CONFIG_NATIVE_MODULE_STRUCTURES
extern JSClassID sdb_rb_class_id;
extern JSClassID dsw_bitset_class_id;
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define VEC_OFFLOAD_MIN ((uint64_t)1 << 18)

typedef struct {
    dyn_vec_t v;
    uint32_t next_ticket, serving, size;
    int has_idle;
    JSValue idle_p, idle_res;
    dyn_persist_r* map;
} vec_w;

static JSClassID vec_class_id;

static const char* const vec_metric_names[] = { "l2", "cosine", "ip" };
static const char* const vec_kind_names[] = { "flat", "hnsw", "ivfpq" };
static const char* const vec_quant_names[] = { "f32", "sq8", "binary" };

static void vec_dispose(void* p)
{
    vec_w* w = (vec_w*)p;
    dyn_vec_free(&w->v);
    if (w->map)
        dyn_persist_release(w->map);
    free(w);
}

static const JSClassDef vec_class = {
    "VectorIndex",
    .finalizer = dyn_res_finalizer,
};

static int vec_pending(const vec_w* w)
{
    return w->next_ticket != w->serving;
}

static vec_w* vec_sync_get(JSContext* ctx, JSValueConst this_val, const char* fn)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    if (!w)
        return NULL;
    if (vec_pending(w)) {
        JS_ThrowTypeError(ctx, "VectorIndex.%s: an add or search on this index has not settled; await it first", fn);
        return NULL;
    }
    return w;
}

static JSValue vec_throw(JSContext* ctx, int rc, const char* fn)
{
    switch (rc) {
    case DYN_VEC_EDUP:
        return JS_ThrowRangeError(ctx, "VectorIndex.%s: id is already in the index", fn);
    case DYN_VEC_ENONFINITE:
        return JS_ThrowRangeError(ctx, "VectorIndex.%s: vector holds NaN or Infinity", fn);
    case DYN_VEC_EZERO:
        return JS_ThrowRangeError(ctx, "VectorIndex.%s: zero vector has no cosine direction", fn);
    case DYN_VEC_EFULL:
        return JS_ThrowRangeError(ctx, "VectorIndex.%s: index is full", fn);
    default:
        return JS_ThrowOutOfMemory(ctx);
    }
}

static int vec_int(JSContext* ctx, JSValueConst val, const char* fn, const char* name,
    double lo, double hi, double* out)
{
    double d;
    if (!JS_IsNumber(val)) {
        JS_ThrowTypeError(ctx, "%s%s must be a number", fn, name);
        return -1;
    }
    JS_ToFloat64(ctx, &d, val);
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s%s must be an integer in [%.0f, %.0f]", fn, name, lo, hi);
        return -1;
    }
    *out = d;
    return 0;
}

static int vec_opt_int(JSContext* ctx, JSValueConst opts, const char* fn, const char* name,
    double lo, double hi, double* out, int* present)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, name);
    int r;
    if (JS_IsException(v))
        return -1;
    *present = !JS_IsUndefined(v);
    if (!*present)
        return 0;
    r = vec_int(ctx, v, fn, name, lo, hi, out);
    JS_FreeValue(ctx, v);
    return r;
}

static int vec_opt_enum(JSContext* ctx, JSValueConst opts, const char* name,
    const char* const* names, const char* list, uint32_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, name);
    const char* s;
    int i;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "VectorIndex: %s must be a string", name);
        return -1;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < 3; i++)
        if (strcmp(s, names[i]) == 0)
            break;
    JS_FreeCString(ctx, s);
    if (i == 3) {
        JS_ThrowRangeError(ctx, "VectorIndex: %s must be one of %s", name, list);
        return -1;
    }
    *out = (uint32_t)i;
    return 0;
}

static const char* const vec_ctor_keys[] = { "dims", "metric", "index", "quantization", "m",
    "efConstruction", "dyna" };
static const char* const vec_meta_keys[] = { "efSearch", "exactFallback", "heuristic", "seed", "rerank", "rerankFactor" };

static int vec_meta(JSContext* ctx, JSValueConst o, dyn_vec_params_t* p, int* any)
{
    JSValue mv = JS_GetPropertyStr(ctx, o, "dyna");
    double d;
    int has, r = -1;
    if (JS_IsException(mv))
        return -1;
    if (JS_IsUndefined(mv))
        return 0;
    if (!JS_IsObject(mv) || JS_IsFunction(ctx, mv)) {
        JS_FreeValue(ctx, mv);
        JS_ThrowTypeError(ctx, "VectorIndex: dyna must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, mv, vec_meta_keys, countof(vec_meta_keys)))
        goto done;
    if (vec_opt_int(ctx, mv, "VectorIndex: dyna.", "efSearch", 1, DYN_VEC_MAX_EF, &d, &has))
        goto done;
    if (has) {
        p->efs = (uint32_t)d;
        *any |= 1;
    }
    if (vec_opt_int(ctx, mv, "VectorIndex: dyna.", "seed", 0, 9007199254740991.0, &d, &has))
        goto done;
    if (has) {
        p->seed = (uint64_t)d;
        *any |= 2;
    }
    if (vec_opt_int(ctx, mv, "VectorIndex: dyna.", "rerankFactor", 1, DYN_VEC_MAX_POOL, &d, &has))
        goto done;
    if (has) {
        p->pool = (uint32_t)d;
        *any |= 8;
    }
    p->rerank = -1;
    if (dyn_opt_bool(ctx, mv, "rerank", "VectorIndex: dyna", &p->rerank))
        goto done;
    if (p->rerank != -1)
        *any |= 4;
    else
        p->rerank = 1;
    p->exact_fallback = -1;
    p->heuristic = -1;
    if (dyn_opt_bool(ctx, mv, "exactFallback", "VectorIndex: dyna", &p->exact_fallback)
        || dyn_opt_bool(ctx, mv, "heuristic", "VectorIndex: dyna", &p->heuristic))
        goto done;
    if (p->exact_fallback != -1 || p->heuristic != -1)
        *any |= 2;
    if (p->exact_fallback == -1)
        p->exact_fallback = 1;
    if (p->heuristic == -1)
        p->heuristic = 1;
    r = 0;
done:
    JS_FreeValue(ctx, mv);
    return r;
}

static JSValue vec_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    dyn_vec_params_t p;
    double d;
    int has_m = 0, has_efc = 0, any_meta = 0;
    uint32_t quant = 0;
    vec_w* w;
    JSValue r, dv;
    memset(&p, 0, sizeof(p));
    p.metric = DYN_VEC_L2;
    p.kind = DYN_VEC_HNSW;
    p.M = 16;
    p.efc = 200;
    p.efs = 128;
    p.heuristic = 1;
    p.exact_fallback = 1;
    p.rerank = 1;
    if (!JS_IsObject(o) || JS_IsFunction(ctx, o))
        return JS_ThrowTypeError(ctx, "VectorIndex: options must be an object with dims");
    if (dyn_opts_strict(ctx, o, vec_ctor_keys, countof(vec_ctor_keys)))
        return JS_EXCEPTION;
    dv = JS_GetPropertyStr(ctx, o, "dims");
    if (JS_IsException(dv))
        return JS_EXCEPTION;
    if (JS_IsUndefined(dv))
        return JS_ThrowTypeError(ctx, "VectorIndex: dims is required");
    if (vec_int(ctx, dv, "VectorIndex: ", "dims", 1, DYN_VEC_MAX_DIM, &d)) {
        JS_FreeValue(ctx, dv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, dv);
    p.dim = (uint32_t)d;
    if (vec_opt_enum(ctx, o, "metric", vec_metric_names, "\"l2\", \"cosine\", \"ip\"", &p.metric)
        || vec_opt_enum(ctx, o, "index", vec_kind_names, "\"hnsw\", \"flat\", \"ivfpq\"", &p.kind)
        || vec_opt_enum(ctx, o, "quantization", vec_quant_names, "\"f32\", \"sq8\", \"binary\"", &quant))
        return JS_EXCEPTION;
    if (p.kind == 2)
        return JS_ThrowRangeError(ctx, "VectorIndex: index \"ivfpq\" is unsupported in this build");
    p.quant = quant;
    if (vec_opt_int(ctx, o, "VectorIndex: ", "m", DYN_VEC_MIN_M, DYN_VEC_MAX_M, &d, &has_m))
        return JS_EXCEPTION;
    if (has_m)
        p.M = (uint32_t)d;
    if (vec_opt_int(ctx, o, "VectorIndex: ", "efConstruction", 1, DYN_VEC_MAX_EF, &d, &has_efc))
        return JS_EXCEPTION;
    if (has_efc)
        p.efc = (uint32_t)d;
    if (vec_meta(ctx, o, &p, &any_meta))
        return JS_EXCEPTION;
    if ((any_meta & 4) && quant == DYN_VEC_F32)
        return JS_ThrowTypeError(ctx, "VectorIndex: dyna.rerank applies only to quantization \"sq8\" or \"binary\"");
    if ((any_meta & 8) && (quant == DYN_VEC_F32 || !p.rerank))
        return JS_ThrowTypeError(ctx, "VectorIndex: dyna.rerankFactor applies only to quantization \"sq8\" or \"binary\" with dyna.rerank true");
    if (p.kind == DYN_VEC_FLAT && (has_m || has_efc || (any_meta & 2) || ((any_meta & 1) && quant == DYN_VEC_F32)))
        return JS_ThrowTypeError(ctx, "VectorIndex: m, efConstruction and the dyna options apply only to index \"hnsw\" (a quantized flat index takes dyna.efSearch, dyna.rerank and dyna.rerankFactor)");
    w = (vec_w*)calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    w->idle_p = JS_UNDEFINED;
    w->idle_res = JS_UNDEFINED;
    if (dyn_vec_init(&w->v, &p)) {
        vec_dispose(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = dyn_res_wrap(ctx, new_target, vec_class_id, w, vec_dispose);
    if (JS_IsException(r))
        vec_dispose(w);
    return r;
}

static int vec_view(JSContext* ctx, JSValueConst v, const char* fn, const char* what,
    int* type, void** p, size_t* bytes)
{
    JSValue buf;
    double off, bl;
    int vt, r;
    size_t size;
    uint8_t* base;
    r = JS_GetViewInfo(ctx, v, &buf, &off, &bl, &vt);
    if (r < 0)
        return -1;
    if (r == 0 || (vt != JS_TYPED_ARRAY_FLOAT32 && vt != JS_TYPED_ARRAY_UINT32 && vt != JS_TYPED_ARRAY_BIG_UINT64)) {
        if (r > 0)
            JS_FreeValue(ctx, buf);
        *type = -1;
        return 0;
    }
    base = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "VectorIndex.%s: %s is detached", fn, what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "VectorIndex.%s: %s is out of bounds of its buffer", fn, what);
        return -1;
    }
    JS_FreeValue(ctx, buf);
    *type = vt;
    *p = base + (size_t)off;
    *bytes = (size_t)bl;
    return 0;
}

static int vec_f32_arg(JSContext* ctx, JSValueConst v, const char* fn, const char* what,
    const float** p, size_t* n)
{
    int t;
    void* q;
    size_t b;
    if (vec_view(ctx, v, fn, what, &t, &q, &b))
        return -1;
    if (t != JS_TYPED_ARRAY_FLOAT32) {
        JS_ThrowTypeError(ctx, "VectorIndex.%s: %s must be a Float32Array", fn, what);
        return -1;
    }
    *p = (const float*)q;
    *n = b / 4;
    return 0;
}

static int vec_ids_arg(JSContext* ctx, JSValueConst v, const char* fn, uint64_t** out, size_t* n)
{
    int t;
    void* q;
    size_t b, i, c;
    uint64_t* ids;
    if (vec_view(ctx, v, fn, "ids", &t, &q, &b))
        return -1;
    if (t != JS_TYPED_ARRAY_UINT32 && t != JS_TYPED_ARRAY_BIG_UINT64) {
        JS_ThrowTypeError(ctx, "VectorIndex.%s: ids must be a BigUint64Array or Uint32Array", fn);
        return -1;
    }
    c = t == JS_TYPED_ARRAY_UINT32 ? b / 4 : b / 8;
    if (c > DYN_VEC_MAX_SIZE) {
        JS_ThrowRangeError(ctx, "VectorIndex.%s: too many ids", fn);
        return -1;
    }
    ids = (uint64_t*)malloc((c ? c : 1) * sizeof(uint64_t));
    if (!ids) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (t == JS_TYPED_ARRAY_UINT32)
        for (i = 0; i < c; i++)
            ids[i] = ((const uint32_t*)q)[i];
    else if (c)
        memcpy(ids, q, c * sizeof(uint64_t));
    *out = ids;
    *n = c;
    return 0;
}

static int vec_id_value(JSContext* ctx, JSValueConst v, const char* fn, uint64_t* out)
{
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        if (!(d >= 0 && d <= 9007199254740991.0) || d != floor(d))
            return 0;
        *out = (uint64_t)d;
        return 1;
    }
    if (JS_IsBigInt(ctx, v)) {
        int64_t s;
        JSValue back;
        int same;
        if (JS_ToBigInt64(ctx, &s, v))
            return -1;
        back = JS_NewBigUint64(ctx, (uint64_t)s);
        if (JS_IsException(back))
            return -1;
        same = JS_StrictEq(ctx, back, v);
        JS_FreeValue(ctx, back);
        if (!same || (uint64_t)s == DYN_VEC_NOID)
            return 0;
        *out = (uint64_t)s;
        return 1;
    }
    JS_ThrowTypeError(ctx, "VectorIndex.%s: id must be a number or a bigint", fn);
    return -1;
}

static JSValue vec_has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    vec_w* w = vec_sync_get(ctx, this_val, "has");
    uint64_t id;
    int r;
    if (!w)
        return JS_EXCEPTION;
    r = vec_id_value(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "has", &id);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r > 0 && dyn_vec_find(&w->v, id) != DYN_VEC_NONE);
}

static JSValue vec_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    vec_w* w = vec_sync_get(ctx, this_val, "remove");
    uint64_t* ids;
    size_t n, i;
    uint32_t c = 0;
    if (!w)
        return JS_EXCEPTION;
    if (vec_ids_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "remove", &ids, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        c += (uint32_t)dyn_vec_remove(&w->v, ids[i]);
    free(ids);
    w->size = w->v.live;
    return JS_NewUint32(ctx, c);
}

typedef struct {
    int kind, owned, held;
    dyn_roar_t roar;
    dyn_bitset_t bits;
    uint64_t* ids;
    dyn_vec_filter_t f;
    JSValue hold;
} vec_filter_own;

static int vec_filter_arg(JSContext* ctx, JSValueConst fv, const char* fn, int copy,
    vec_filter_own* fo)
{
    if (JS_IsUndefined(fv))
        return 0;
    if (JS_IsObject(fv)) {
        int t;
        void* q;
        size_t b;
        if (vec_view(ctx, fv, fn, "filter", &t, &q, &b))
            return -1;
        if (t == JS_TYPED_ARRAY_BIG_UINT64) {
            size_t c = b / 8;
            fo->ids = (uint64_t*)malloc((c ? c : 1) * sizeof(uint64_t));
            if (!fo->ids) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            if (c)
                memcpy(fo->ids, q, c * sizeof(uint64_t));
            if (!dyn_vec_ids_sorted(fo->ids, c)) {
                JS_ThrowRangeError(ctx, "VectorIndex.%s: a BigUint64Array filter must be sorted ascending with no duplicates", fn);
                return -1;
            }
            fo->kind = 3;
            fo->f.ids = fo->ids;
            fo->f.nids = c;
            return 0;
        }
    }
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES
    if (JS_IsObject(fv)) {
        JSClassID cid = JS_GetClassID(fv);
        if (sdb_rb_class_id && cid == sdb_rb_class_id) {
            const dyn_roar_t* r = (const dyn_roar_t*)JS_GetOpaque(fv, sdb_rb_class_id);
            if (r) {
                if (copy) {
                    if (dyn_roar_clone(&fo->roar, r)) {
                        JS_ThrowOutOfMemory(ctx);
                        return -1;
                    }
                    fo->owned = 1;
                    r = &fo->roar;
                }
                fo->kind = 1;
                fo->f.roar = r;
                return 0;
            }
        }
        if (dsw_bitset_class_id && cid == dsw_bitset_class_id) {
            const dyn_bitset_t* b = (const dyn_bitset_t*)JS_GetOpaque(fv, dsw_bitset_class_id);
            if (b) {
                if (copy) {
                    fo->bits.nwords = b->nwords;
                    fo->bits.cc = b->cc;
                    fo->bits.words = (uint64_t*)malloc((size_t)(b->nwords ? b->nwords : 1) * sizeof(uint64_t));
                    if (!fo->bits.words) {
                        JS_ThrowOutOfMemory(ctx);
                        return -1;
                    }
                    if (b->nwords)
                        memcpy(fo->bits.words, b->words, (size_t)b->nwords * sizeof(uint64_t));
                    b = &fo->bits;
                }
                fo->kind = 2;
                fo->f.bits = b;
                return 0;
            }
        }
    }
#endif
    JS_ThrowTypeError(ctx, "VectorIndex.%s: filter must be a sorted BigUint64Array, or a RoaringBitmap or BitSet from dyna:structures", fn);
    return -1;
}

static void vec_filter_release(JSContext* ctx, vec_filter_own* fo)
{
    if (fo->held)
        JS_FreeValue(ctx, fo->hold);
    if (fo->owned && fo->kind == 1)
        dyn_roar_clear(&fo->roar);
    free(fo->bits.words);
    free(fo->ids);
    memset(fo, 0, sizeof(*fo));
}

enum { VEC_JOB_ADD,
    VEC_JOB_SEARCH,
    VEC_JOB_BATCH };

typedef struct {
    int op, rc, borrow;
    JSValue self;
    DynResource* r;
    vec_w* w;
    uint32_t ticket, nb, k, ef;
    uint64_t* ids;
    float* vecs;
    vec_filter_own fo;
    dyn_vec_hit_t* hits;
    uint32_t* cnt;
} vec_job;

static void vec_job_free(JSContext* ctx, vec_job* j)
{
    JS_FreeValue(ctx, j->self);
    free(j->ids);
    free(j->vecs);
    free(j->hits);
    free(j->cnt);
    vec_filter_release(ctx, &j->fo);
    free(j);
}

static const char* vec_job_name(const vec_job* j)
{
    return j->op == VEC_JOB_ADD ? "add" : j->op == VEC_JOB_SEARCH ? "search" : "searchBatch";
}

static int vec_job_run(vec_job* j)
{
    dyn_vec_t* v = &j->w->v;
    uint32_t i, dim = v->dim;
    int rc;
    if (j->op == VEC_JOB_ADD) {
        rc = dyn_vec_reserve(v, j->nb);
        if (rc)
            return rc;
        rc = dyn_vec_train(v, j->vecs, j->nb);
        if (rc)
            return rc;
        for (i = 0; i < j->nb; i++) {
            rc = dyn_vec_add(v, j->ids[i], j->vecs + (size_t)i * dim);
            if (rc)
                return rc;
        }
        return DYN_VEC_OK;
    }
    for (i = 0; i < j->nb; i++) {
        rc = dyn_vec_search(v, j->vecs + (size_t)i * dim, j->k, j->ef, j->fo.kind ? &j->fo.f : NULL,
            j->hits + (size_t)i * j->k, &j->cnt[i]);
        if (rc)
            return rc;
    }
    return DYN_VEC_OK;
}

static uint64_t vec_job_work(const vec_job* j)
{
    const dyn_vec_t* v = &j->w->v;
    uint64_t per;
    if (j->op == VEC_JOB_ADD)
        per = v->kind == DYN_VEC_HNSW ? v->efc : 1;
    else
        per = v->kind == DYN_VEC_HNSW ? j->ef : (uint64_t)v->live + 1;
    return (uint64_t)j->nb * v->dim * per;
}

static int vec_wait_turn(JSContext* ctx, vec_w* w, uint32_t ticket)
{
    while (w->serving != ticket) {
        JSValue r;
        if (!w->has_idle) {
            JSValue fns[2];
            w->idle_p = JS_NewPromiseCapability(ctx, fns);
            if (JS_IsException(w->idle_p))
                return -1;
            w->idle_res = fns[0];
            JS_FreeValue(ctx, fns[1]);
            w->has_idle = 1;
        }
        r = dyn_fiber_await(ctx, JS_DupValue(ctx, w->idle_p));
        if (JS_IsException(r))
            return -1;
        JS_FreeValue(ctx, r);
    }
    return 0;
}

static void vec_end_turn(JSContext* ctx, vec_w* w)
{
    w->serving++;
    w->size = w->v.live;
    if (w->has_idle) {
        JSValue res = w->idle_res, p = w->idle_p, r;
        w->has_idle = 0;
        w->idle_res = JS_UNDEFINED;
        w->idle_p = JS_UNDEFINED;
        r = JS_Call(ctx, res, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, p);
    }
}

static int vec_u64_cmp(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static int vec_add_present(JSContext* ctx, vec_job* j)
{
    uint32_t i;
    for (i = 0; i < j->nb; i++)
        if (dyn_vec_find(&j->w->v, j->ids[i]) != DYN_VEC_NONE) {
            JS_ThrowRangeError(ctx, "VectorIndex.add: id %llu is already in the index", (unsigned long long)j->ids[i]);
            return -1;
        }
    if ((uint64_t)j->w->v.n + j->nb > DYN_VEC_MAX_SIZE) {
        vec_throw(ctx, DYN_VEC_EFULL, "add");
        return -1;
    }
    return 0;
}

static JSValue vec_new_ta(JSContext* ctx, JSTypedArrayEnum type, uint32_t n, void** p)
{
    JSValue len = JS_NewInt64(ctx, n), ta, ab;
    size_t off, bl, size;
    uint8_t* base;
    ta = JS_NewTypedArray(ctx, 1, (JSValueConst*)&len, type);
    if (JS_IsException(ta))
        return ta;
    ab = JS_GetTypedArrayBuffer(ctx, ta, &off, &bl, NULL);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, ta);
        return ab;
    }
    base = JS_GetArrayBuffer(ctx, &size, ab);
    JS_FreeValue(ctx, ab);
    if (!base && n) {
        JS_FreeValue(ctx, ta);
        return JS_EXCEPTION;
    }
    *p = base ? base + off : NULL;
    return ta;
}

static JSValue vec_job_result(JSContext* ctx, vec_job* j)
{
    uint32_t rows = j->op == VEC_JOB_SEARCH ? j->cnt[0] : j->nb * j->k, i;
    void *pi, *pd, *pc = NULL;
    JSValue o, ids, ds, cs = JS_UNDEFINED;
    ids = vec_new_ta(ctx, JS_TYPED_ARRAY_BIG_UINT64, rows, &pi);
    if (JS_IsException(ids))
        return ids;
    ds = vec_new_ta(ctx, JS_TYPED_ARRAY_FLOAT32, rows, &pd);
    if (JS_IsException(ds)) {
        JS_FreeValue(ctx, ids);
        return ds;
    }
    if (j->op == VEC_JOB_BATCH) {
        cs = vec_new_ta(ctx, JS_TYPED_ARRAY_UINT32, j->nb, &pc);
        if (JS_IsException(cs)) {
            JS_FreeValue(ctx, ids);
            JS_FreeValue(ctx, ds);
            return cs;
        }
    }
    for (i = 0; i < rows; i++) {
        uint32_t q = j->op == VEC_JOB_SEARCH ? 0 : i / j->k, t = j->op == VEC_JOB_SEARCH ? i : i % j->k;
        int live = t < j->cnt[q];
        ((uint64_t*)pi)[i] = live ? j->hits[i].s : DYN_VEC_NOID;
        ((float*)pd)[i] = live ? (float)j->hits[i].d : (float)INFINITY;
    }
    if (pc)
        memcpy(pc, j->cnt, (size_t)j->nb * sizeof(uint32_t));
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, ids);
        JS_FreeValue(ctx, ds);
        JS_FreeValue(ctx, cs);
        return o;
    }
    JS_SetPropertyStr(ctx, o, "ids", ids);
    JS_SetPropertyStr(ctx, o, "distances", ds);
    if (j->op == VEC_JOB_BATCH)
        JS_SetPropertyStr(ctx, o, "counts", cs);
    return o;
}

static JSValue vec_job_entry(JSContext* ctx, void* arg)
{
    vec_job* j = (vec_job*)arg;
    vec_w* w = j->w;
    JSValue r = JS_EXCEPTION;
    if (vec_wait_turn(ctx, w, j->ticket) == 0) {
        if (j->op == VEC_JOB_ADD && vec_add_present(ctx, j)) {
            vec_end_turn(ctx, w);
        } else {
            int off = !j->borrow && vec_job_work(j) >= VEC_OFFLOAD_MIN && dyn_fiber_any();
            j->rc = DYN_OFFLOAD_CPU_IF(off, vec_job_run(j));
            vec_end_turn(ctx, w);
            if (j->rc)
                r = vec_throw(ctx, j->rc, vec_job_name(j));
            else if (j->op == VEC_JOB_ADD)
                r = JS_UNDEFINED;
            else
                r = vec_job_result(ctx, j);
        }
    }
    j->r->async_users--;
    vec_job_free(ctx, j);
    return r;
}

static JSValue vec_rejected(JSContext* ctx)
{
    JSValue e = JS_GetException(ctx), funcs[2], p, r;
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, e);
        return p;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

static int vec_prepare_add(JSContext* ctx, vec_job* j, int argc, JSValueConst* argv)
{
    const float* x;
    size_t n, nv, i;
    uint64_t* s;
    if (vec_ids_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "add", &j->ids, &n))
        return -1;
    j->nb = (uint32_t)n;
    if (vec_f32_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "add", "vectors", &x, &nv))
        return -1;
    if (nv != (size_t)j->nb * j->w->v.dim) {
        JS_ThrowRangeError(ctx, "VectorIndex.add: vectors has length %zu, expected ids.length * dims = %zu", nv, (size_t)j->nb * j->w->v.dim);
        return -1;
    }
    j->vecs = (float*)malloc((nv ? nv : 1) * sizeof(float));
    s = (uint64_t*)malloc((n ? n : 1) * sizeof(uint64_t));
    if (!j->vecs || !s) {
        free(s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(j->vecs, x, nv * sizeof(float));
    memcpy(s, j->ids, n * sizeof(uint64_t));
    qsort(s, n, sizeof(uint64_t), vec_u64_cmp);
    for (i = 0; i < n; i++) {
        if (s[i] == DYN_VEC_NOID) {
            free(s);
            JS_ThrowRangeError(ctx, "VectorIndex.add: id 18446744073709551615 is reserved");
            return -1;
        }
        if (i && s[i] == s[i - 1]) {
            JS_ThrowRangeError(ctx, "VectorIndex.add: id %llu appears twice in the batch", (unsigned long long)s[i]);
            free(s);
            return -1;
        }
    }
    free(s);
    for (i = 0; i < j->nb; i++) {
        int rc = dyn_vec_check(&j->w->v, j->vecs + i * j->w->v.dim);
        if (rc) {
            vec_throw(ctx, rc, "add");
            return -1;
        }
    }
    return 0;
}

static const char* const vec_search_keys[] = { "efSearch", "filter" };

static int vec_prepare_search(JSContext* ctx, vec_job* j, int argc, JSValueConst* argv)
{
    const char* fn = vec_job_name(j);
    const float* q;
    size_t nv, i;
    double d;
    int has;
    char fq[48];
    vec_w* w = j->w;
    snprintf(fq, sizeof(fq), "VectorIndex.%s: ", fn);
    if (vec_int(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fq, "k", 1, DYN_VEC_MAX_K, &d))
        return -1;
    j->k = (uint32_t)d;
    j->ef = w->v.efs;
    if (dyn_opts_bag(ctx, argc, argv, 2, vec_search_keys, countof(vec_search_keys), "VectorIndex"))
        return -1;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue fv;
        if (vec_opt_int(ctx, argv[2], fq, "efSearch", 1, DYN_VEC_MAX_EF, &d, &has))
            return -1;
        if (has)
            j->ef = (uint32_t)d;
        fv = JS_GetPropertyStr(ctx, argv[2], "filter");
        if (JS_IsException(fv))
            return -1;
        j->fo.hold = fv;
        j->fo.held = 1;
    }
    if (vec_f32_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, j->op == VEC_JOB_SEARCH ? "query" : "queries", &q, &nv))
        return -1;
    if (j->op == VEC_JOB_SEARCH ? nv != w->v.dim : nv % w->v.dim != 0) {
        if (j->op == VEC_JOB_SEARCH)
            JS_ThrowRangeError(ctx, "VectorIndex.search: query has length %zu, the index dims is %u", nv, w->v.dim);
        else
            JS_ThrowRangeError(ctx, "VectorIndex.searchBatch: queries has length %zu, not a multiple of dims %u", nv, w->v.dim);
        return -1;
    }
    if (nv / w->v.dim > DYN_VEC_MAX_SIZE || (uint64_t)(nv / w->v.dim) * j->k > 0x7FFFFFFFu) {
        JS_ThrowRangeError(ctx, "VectorIndex.%s: queries * k is too large", fn);
        return -1;
    }
    j->nb = (uint32_t)(nv / w->v.dim);
    j->vecs = (float*)malloc((nv ? nv : 1) * sizeof(float));
    j->hits = (dyn_vec_hit_t*)malloc(((size_t)j->nb * j->k + 1) * sizeof(dyn_vec_hit_t));
    j->cnt = (uint32_t*)calloc((size_t)j->nb + 1, sizeof(uint32_t));
    if (!j->vecs || !j->hits || !j->cnt) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(j->vecs, q, nv * sizeof(float));
    for (i = 0; i < j->nb; i++) {
        int rc = dyn_vec_check(&w->v, j->vecs + i * w->v.dim);
        if (rc) {
            vec_throw(ctx, rc, fn);
            return -1;
        }
    }
    j->borrow = w->serving == j->ticket && vec_job_work(j) < VEC_OFFLOAD_MIN;
    if (j->fo.held) {
        JSValue fv = j->fo.hold;
        if (vec_filter_arg(ctx, fv, fn, !j->borrow, &j->fo))
            return -1;
        if (!j->borrow) {
            j->fo.held = 0;
            JS_FreeValue(ctx, fv);
        }
    }
    return 0;
}

static JSValue vec_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int op)
{
    DynResource* r = dyn_res_get(ctx, this_val, vec_class_id);
    vec_job* j;
    JSValue p;
    int bad;
    if (!r)
        return vec_rejected(ctx);
    j = (vec_job*)calloc(1, sizeof(*j));
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        return vec_rejected(ctx);
    }
    j->op = op;
    j->r = r;
    j->w = (vec_w*)r->native;
    j->self = JS_UNDEFINED;
    j->ticket = j->w->next_ticket;
    r->async_users++;
    bad = op == VEC_JOB_ADD ? vec_prepare_add(ctx, j, argc, argv) : vec_prepare_search(ctx, j, argc, argv);
    if (bad) {
        r->async_users--;
        vec_job_free(ctx, j);
        return vec_rejected(ctx);
    }
    if (j->ticket != j->w->next_ticket) {
        j->ticket = j->w->next_ticket;
        if (j->borrow) {
            JSValue fv = JS_DupValue(ctx, j->fo.hold);
            vec_filter_release(ctx, &j->fo);
            j->borrow = 0;
            bad = vec_filter_arg(ctx, fv, vec_job_name(j), 1, &j->fo);
            JS_FreeValue(ctx, fv);
            if (bad) {
                r->async_users--;
                vec_job_free(ctx, j);
                return vec_rejected(ctx);
            }
        }
    }
    j->self = JS_DupValue(ctx, this_val);
    j->w->next_ticket++;
    p = dyn_fiber_start(ctx, vec_job_entry, j);
    if (JS_IsException(p)) {
        r->async_users--;
        j->w->next_ticket--;
        vec_job_free(ctx, j);
    }
    return p;
}

static JSValue vec_add(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return vec_start(ctx, this_val, argc, argv, VEC_JOB_ADD);
}

static JSValue vec_search(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return vec_start(ctx, this_val, argc, argv, VEC_JOB_SEARCH);
}

static JSValue vec_search_batch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return vec_start(ctx, this_val, argc, argv, VEC_JOB_BATCH);
}

static JSValue vec_get_size(JSContext* ctx, JSValueConst this_val)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    return w ? JS_NewUint32(ctx, w->size) : JS_EXCEPTION;
}

static JSValue vec_get_dims(JSContext* ctx, JSValueConst this_val)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    return w ? JS_NewUint32(ctx, w->v.dim) : JS_EXCEPTION;
}

static JSValue vec_get_metric(JSContext* ctx, JSValueConst this_val)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    return w ? JS_NewString(ctx, vec_metric_names[w->v.metric]) : JS_EXCEPTION;
}

static JSValue vec_get_index(JSContext* ctx, JSValueConst this_val)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    return w ? JS_NewString(ctx, vec_kind_names[w->v.kind]) : JS_EXCEPTION;
}

enum { VEC_S_META = 48,
    VEC_S_DATA,
    VEC_S_EXT,
    VEC_S_DEAD,
    VEC_S_LVL,
    VEC_S_LINKS,
    VEC_S_QPARAM,
    VEC_S_CODES };

#define VEC_LINEAGE 0x5645434958563031ull

static JSValue vec_persist_throw(JSContext* ctx, const char* op, const char* path,
    const dyn_persist_err* e)
{
    JSValue err = JS_NewError(ctx);
    char msg[512];
    if (JS_IsException(err))
        return err;
    snprintf(msg, sizeof msg, "VectorIndex.%s(\"%.200s\"): %s: %s", op, path,
        dyn_persist_strerror(e->code), e->msg);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, dyn_persist_code_name(e->code)), JS_PROP_C_W_E);
    if (e->sys_errno)
        JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e->sys_errno), JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

static int vec_persist_bools(JSContext* ctx, JSValueConst opts, const char* fn,
    const char* a, int* va, const char* b, int* vb)
{
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts) || JS_IsFunction(ctx, opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", fn);
        return -1;
    }
    return dyn_opt_bool(ctx, opts, a, fn, va) || dyn_opt_bool(ctx, opts, b, fn, vb) ? -1 : 0;
}

static JSValue vec_save(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const uint8_t nil = 0;
    vec_w* w = vec_sync_get(ctx, this_val, "save");
    dyn_vec_t* v;
    dyn_persist_w pw;
    dyn_persist_save_opt o;
    dyn_persist_err e;
    uint64_t meta[DYN_VEC_META_WORDS];
    const char* path;
    int do_fsync = 1, incremental = 0, rc = 0;
    uint32_t i;
    JSValue out;
    if (!w)
        return JS_EXCEPTION;
    v = &w->v;
    path = dyn_path_borrow(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "VectorIndex.save(path, opts?): path", NULL);
    if (!path)
        return JS_EXCEPTION;
    if (vec_persist_bools(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "VectorIndex.save", "fsync", &do_fsync,
            "incremental", &incremental))
        return JS_EXCEPTION;
    dyn_vec_meta(v, meta);
    dyn_persist_w_init(&pw);
    dyn_persist_w_begin(&pw, DYN_VEC_PERSIST_TYPE, v->live);
    dyn_persist_w_param(&pw, 0, v->dim);
    dyn_persist_w_param(&pw, 1, v->n);
    rc |= dyn_persist_w_ref(&pw, VEC_S_META, 0, 8, 0, meta, sizeof meta);
    rc |= dyn_persist_w_ref(&pw, VEC_S_DATA, 0, 4, DYN_PERSIST_S_PAGE, v->n && v->keep ? (const void*)v->data : &nil,
        v->keep ? (uint64_t)v->n * v->dim * sizeof(float) : 0);
    if (v->quant == DYN_VEC_SQ8) {
        rc |= dyn_persist_w_open(&pw, VEC_S_QPARAM, 0, 8, 0);
        rc |= dyn_persist_w_put(&pw, v->lo, (size_t)v->dim * sizeof(double));
        rc |= dyn_persist_w_put(&pw, v->hi, (size_t)v->dim * sizeof(double));
        rc |= dyn_persist_w_close(&pw);
    }
    if (v->quant != DYN_VEC_F32)
        rc |= dyn_persist_w_ref(&pw, VEC_S_CODES, 0, v->quant == DYN_VEC_SQ8 ? 1 : 8, DYN_PERSIST_S_PAGE,
            v->n ? (const void*)v->codes : &nil, (uint64_t)v->n * v->cb);
    rc |= dyn_persist_w_ref(&pw, VEC_S_EXT, 0, 8, 0, v->n ? (const void*)v->ext : &nil, (uint64_t)v->n * 8);
    rc |= dyn_persist_w_ref(&pw, VEC_S_DEAD, 0, 1, 0, v->n ? (const void*)v->dead : &nil, v->n);
    if (v->kind == DYN_VEC_HNSW) {
        rc |= dyn_persist_w_ref(&pw, VEC_S_LVL, 0, 1, 0, v->n ? (const void*)v->lvl : &nil, v->n);
        rc |= dyn_persist_w_open(&pw, VEC_S_LINKS, 0, 4, 0);
        for (i = 0; i < v->n && !rc; i++)
            rc |= dyn_persist_w_put(&pw, v->links[i], (size_t)dyn_vec_block_words(v, i) * sizeof(uint32_t));
        rc |= dyn_persist_w_close(&pw);
    }
    if (rc) {
        dyn_persist_w_free(&pw);
        return JS_ThrowOutOfMemory(ctx);
    }
    pw.lineage = VEC_LINEAGE;
    pw.gen = 0;
    memset(&o, 0, sizeof o);
    o.fsync = do_fsync;
    rc = dyn_persist_save(&pw, path, &o, &e);
    dyn_persist_w_free(&pw);
    if (rc)
        return vec_persist_throw(ctx, "save", path, &e);
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        return out;
    JS_DefinePropertyValueStr(ctx, out, "path", JS_DupValue(ctx, argv[0]), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "bytes", JS_NewInt64(ctx, (int64_t)o.file_size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "written", JS_NewInt64(ctx, (int64_t)o.bytes_written), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "incremental", JS_FALSE, JS_PROP_C_W_E);
    return out;
}

static const dyn_persist_rsect* vec_sect(const dyn_persist_r* r, uint32_t kind, uint32_t elem, int need,
    const char** why)
{
    const dyn_persist_rsect* s = dyn_persist_find(r, kind, 0);
    if (!s) {
        if (need)
            *why = "a section is missing";
        return NULL;
    }
    if (s->elem != elem || (s->len && ((uintptr_t)s->p % elem))) {
        *why = "a section has the wrong element size";
        return NULL;
    }
    return s;
}

static JSValue vec_load(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    const dyn_persist_rsect *sm, *sd, *se, *sx, *sl = NULL, *sk = NULL, *sq = NULL, *sc = NULL;
    dyn_persist_r* r = NULL;
    dyn_persist_err e;
    dyn_vec_image_t img;
    const char* path;
    const char* why = NULL;
    int use_mmap = 0, verify = 1, rc;
    vec_w* w;
    JSValue out;
    (void)this_val;
    path = dyn_path_borrow(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "VectorIndex.load(path, opts?): path", NULL);
    if (!path)
        return JS_EXCEPTION;
    if (vec_persist_bools(ctx, opts, "VectorIndex.load", "mmap", &use_mmap, "verify", &verify))
        return JS_EXCEPTION;
    rc = dyn_persist_open(path, use_mmap ? DYN_PERSIST_MODE_MMAP : DYN_PERSIST_MODE_COPY, verify,
        DYN_VEC_PERSIST_TYPE, &r, &e);
    if (rc) {
        if (rc == DYN_PERSIST_E_TYPE)
            snprintf(e.msg, sizeof e.msg, "the file holds another dyna persist type, not a VectorIndex");
        return vec_persist_throw(ctx, "load", path, &e);
    }
    memset(&img, 0, sizeof img);
    sm = vec_sect(r, VEC_S_META, 8, 1, &why);
    sd = vec_sect(r, VEC_S_DATA, 4, 1, &why);
    se = vec_sect(r, VEC_S_EXT, 8, 1, &why);
    sx = vec_sect(r, VEC_S_DEAD, 1, 1, &why);
    if (!why && sm->len != DYN_VEC_META_WORDS * 8)
        why = "metadata has the wrong size";
    if (!why) {
        sl = vec_sect(r, VEC_S_LVL, 1, 0, &why);
        sk = vec_sect(r, VEC_S_LINKS, 4, 0, &why);
        sq = vec_sect(r, VEC_S_QPARAM, 8, 0, &why);
        sc = dyn_persist_find(r, VEC_S_CODES, 0);
        if (sc && sm->len == DYN_VEC_META_WORDS * 8)
            sc = vec_sect(r, VEC_S_CODES, ((const uint64_t*)sm->p)[8] == DYN_VEC_SQ8 ? 1 : 8, 0, &why);
    }
    if (!why && (!sl) != (!sk))
        why = "graph sections are incomplete";
    if (!why) {
        img.meta = (const uint64_t*)sm->p;
        img.data = (const float*)sd->p;
        img.data_bytes = sd->len;
        img.ext = (const uint64_t*)se->p;
        img.ext_bytes = se->len;
        img.dead = sx->p;
        img.dead_bytes = sx->len;
        img.lvl = sl ? sl->p : NULL;
        img.lvl_bytes = sl ? sl->len : 0;
        img.links = sk ? (const uint32_t*)sk->p : NULL;
        img.links_bytes = sk ? sk->len : 0;
        img.qparam = sq ? (const double*)sq->p : NULL;
        img.qparam_bytes = sq ? sq->len : 0;
        img.codes = sc ? sc->p : NULL;
        img.codes_bytes = sc ? sc->len : 0;
        img.borrow_data = use_mmap && (sd->flags & DYN_PERSIST_S_PAGE);
        if (img.meta[3] == DYN_VEC_HNSW && !sl)
            why = "graph sections are missing";
    }
    w = why ? NULL : (vec_w*)calloc(1, sizeof(*w));
    if (!why && !w) {
        dyn_persist_release(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (!why) {
        rc = dyn_vec_restore(&w->v, &img, &why);
        if (rc && !why) {
            free(w);
            dyn_persist_release(r);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (why) {
        free(w);
        e.code = DYN_PERSIST_E_CORRUPT;
        e.sys_errno = 0;
        snprintf(e.msg, sizeof e.msg, "%s", why);
        dyn_persist_release(r);
        return vec_persist_throw(ctx, "load", path, &e);
    }
    w->idle_p = JS_UNDEFINED;
    w->idle_res = JS_UNDEFINED;
    w->size = w->v.live;
    if (w->v.data_borrowed)
        w->map = r;
    else
        dyn_persist_release(r);
    out = dyn_res_wrap(ctx, JS_UNDEFINED, vec_class_id, w, vec_dispose);
    if (JS_IsException(out))
        vec_dispose(w);
    return out;
}

static JSValue vec_get_quant(JSContext* ctx, JSValueConst this_val)
{
    vec_w* w = (vec_w*)dyn_res_native(ctx, this_val, vec_class_id);
    return w ? JS_NewString(ctx, vec_quant_names[w->v.quant]) : JS_EXCEPTION;
}

static const JSCFunctionListEntry vec_proto[] = {
    JS_CFUNC_DEF("add", 2, vec_add),
    JS_CFUNC_DEF("search", 3, vec_search),
    JS_CFUNC_DEF("searchBatch", 3, vec_search_batch),
    JS_CFUNC_DEF("remove", 1, vec_remove),
    JS_CFUNC_DEF("has", 1, vec_has),
    JS_CFUNC_DEF("save", 1, vec_save),
    JS_CGETSET_DEF("size", vec_get_size, NULL),
    JS_CGETSET_DEF("dims", vec_get_dims, NULL),
    JS_CGETSET_DEF("metric", vec_get_metric, NULL),
    JS_CGETSET_DEF("index", vec_get_index, NULL),
    JS_CGETSET_DEF("quantization", vec_get_quant, NULL),
};

static int vec_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;
    int rc;
    if (dyn_register_class(ctx, m, &vec_class_id, &vec_class, vec_proto,
            countof(vec_proto), vec_ctor, "VectorIndex")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, vec_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    rc = JS_DefinePropertyValueStr(ctx, ctor, "load", JS_NewCFunction(ctx, vec_load, "load", 1),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, ctor);
    return rc < 0 ? -1 : 0;
}

int js_nat_init_vec(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:vec", vec_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExport(ctx, m, "VectorIndex");
}

#endif
