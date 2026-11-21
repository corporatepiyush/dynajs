#include "mathx-internal.h"
#include "mathx-nd.h"
#include "core/dyn-snappend.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

JSClassID dyna_nd_class_id;
static _Atomic(JSClassID) nd_ta_cid[DYN_ND_NTYPES];
static _Atomic(JSClassID) nd_u16_cid;

typedef struct {
    JSValue ta;
    dyn_nd_t v;
} nd_obj_t;

static const char* const nd_dtype_names[DYN_ND_NTYPES] = {
    "float64", "float32", "float16", "bfloat16", "int32", "int16", "int8", "uint8", "bool"
};

static const JSTypedArrayEnum nd_ta_kind[DYN_ND_NTYPES] = {
    JS_TYPED_ARRAY_FLOAT64, JS_TYPED_ARRAY_FLOAT32, JS_TYPED_ARRAY_FLOAT16, JS_TYPED_ARRAY_UINT16,
    JS_TYPED_ARRAY_INT32, JS_TYPED_ARRAY_INT16, JS_TYPED_ARRAY_INT8, JS_TYPED_ARRAY_UINT8, JS_TYPED_ARRAY_UINT8
};

const char* dyna_nd_dtype_name(int dtype)
{
    return (unsigned)dtype < DYN_ND_NTYPES ? nd_dtype_names[dtype] : "unknown";
}

int dyna_nd_parse_dtype(JSContext* ctx, JSValueConst v, int dflt)
{
    const char* s;
    int i;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return dflt;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "NDArray: dtype must be one of float64, float32, float16, bfloat16, int32, int16, int8, uint8, bool");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < DYN_ND_NTYPES; i++)
        if (!strcmp(s, nd_dtype_names[i])) {
            JS_FreeCString(ctx, s);
            return i;
        }
    JS_ThrowTypeError(ctx, "NDArray: unknown dtype \"%s\" (float64, float32, float16, bfloat16, int32, int16, int8, uint8, bool)", s);
    JS_FreeCString(ctx, s);
    return -1;
}

static void nd_finalizer(JSRuntime* rt, JSValue val)
{
    nd_obj_t* o = JS_GetOpaque(val, dyna_nd_class_id);
    if (!o)
        return;
    JS_FreeValueRT(rt, o->ta);
    free(o);
}

static void nd_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    nd_obj_t* o = JS_GetOpaque(val, dyna_nd_class_id);
    if (o)
        JS_MarkValue(rt, o->ta, mark_func);
}

static JSClassDef nd_class = { "NDArray", .finalizer = nd_finalizer, .gc_mark = nd_mark };

static nd_obj_t* nd_get(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, dyna_nd_class_id);
}

static int nd_resolve(JSContext* ctx, const nd_obj_t* o, dyn_nd_t* out)
{
    size_t boff = 0, blen = 0, bpe = 0, sz = 0;
    JSValue ab;
    uint8_t* p;
    *out = o->v;
    ab = JS_GetTypedArrayBuffer(ctx, o->ta, &boff, &blen, &bpe);
    if (JS_IsException(ab))
        return -1;
    p = JS_GetArrayBuffer(ctx, &sz, ab);
    JS_FreeValue(ctx, ab);
    if (!p && blen) {
        JS_ThrowTypeError(ctx, "NDArray: its ArrayBuffer is detached");
        return -1;
    }
    if (!p || boff > sz)
        blen = 0;
    else if (blen > sz - boff)
        blen = sz - boff;
    out->base = p ? p + boff : NULL;
    out->base_len = (int64_t)(blen / dyn_nd_esize(o->v.dtype));
    if (!dyn_nd_bounds_ok(out)) {
        JS_ThrowRangeError(ctx, "NDArray: the view reaches past its buffer (was the ArrayBuffer detached or resized?)");
        return -1;
    }
    return 0;
}

int dyna_nd_view(JSContext* ctx, JSValueConst v, dyn_nd_t* out)
{
    nd_obj_t* o = nd_get(ctx, v);
    if (!o)
        return -1;
    return nd_resolve(ctx, o, out);
}

int dyna_nd_writable(JSContext* ctx, JSValueConst v)
{
    nd_obj_t* o = nd_get(ctx, v);
    if (!o)
        return -1;
    return JS_CheckWritableBuffer(ctx, o->ta);
}

static JSClassID nd_capture_cid(JSContext* ctx, JSTypedArrayEnum type)
{
    JSValueConst args[1];
    JSValue ta;
    JSClassID cid = 0;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, type);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return cid;
}

static JSValue nd_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
static const JSCFunctionListEntry nd_proto_funcs[];
static const JSCFunctionListEntry nd_static_funcs[];
static int nd_proto_count(void);
static int nd_static_count(void);

int dyna_nd_ensure_class(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    int i;
    JS_NewClassID(&dyna_nd_class_id);
    if (!JS_IsRegisteredClass(rt, dyna_nd_class_id) && JS_NewClass(rt, dyna_nd_class_id, &nd_class) < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyna_nd_class_id);
    if (JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return 0;
    }
    JS_FreeValue(ctx, proto);
    for (i = 0; i < DYN_ND_NTYPES; i++)
        if (!nd_ta_cid[i])
            nd_ta_cid[i] = nd_capture_cid(ctx, nd_ta_kind[i]);
    if (!nd_u16_cid)
        nd_u16_cid = nd_capture_cid(ctx, JS_TYPED_ARRAY_UINT16);
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, nd_proto_funcs, nd_proto_count());
    ctor = JS_NewCFunction2(ctx, (JSCFunction*)nd_ctor, "NDArray", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetPropertyFunctionList(ctx, ctor, nd_static_funcs, nd_static_count());
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, dyna_nd_class_id, proto);
    JS_FreeValue(ctx, ctor);
    return 0;
}

static JSValue nd_class_ctor(JSContext* ctx)
{
    JSValue proto, ctor;
    if (dyna_nd_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    proto = JS_GetClassProto(ctx, dyna_nd_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    return ctor;
}

static JSValue nd_make(JSContext* ctx, JSValueConst proto_from, JSValueConst ta, const dyn_nd_t* v)
{
    JSValue proto, obj;
    nd_obj_t* o;
    if (dyna_nd_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    if (JS_IsObject(proto_from)) {
        proto = JS_GetPropertyStr(ctx, proto_from, "prototype");
        if (JS_IsException(proto))
            return proto;
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            proto = JS_GetClassProto(ctx, dyna_nd_class_id);
        }
    } else
        proto = JS_GetClassProto(ctx, dyna_nd_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, dyna_nd_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    o = malloc(sizeof(*o));
    if (!o) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    o->ta = JS_DupValue(ctx, ta);
    o->v = *v;
    o->v.base = NULL;
    o->v.base_len = 0;
    JS_SetOpaque(obj, o);
    return obj;
}

static JSValue nd_typed_over(JSContext* ctx, JSValueConst ab, int dtype, int64_t byte_off, int64_t len)
{
    JSValueConst args[3];
    JSValue r;
    args[0] = ab;
    args[1] = JS_NewInt64(ctx, byte_off);
    args[2] = JS_NewInt64(ctx, len);
    r = JS_NewTypedArray(ctx, 3, args, nd_ta_kind[dtype]);
    return r;
}

JSValue dyna_nd_take(JSContext* ctx, int dtype, int ndim, const int64_t* shape, void* data)
{
    dyn_nd_t v;
    int64_t size = dyn_nd_shape_size(ndim, shape);
    size_t bytes;
    JSValue ab, ta, r;
    memset(&v, 0, sizeof(v));
    if (size < 0 || (uint64_t)size > (uint64_t)1 << 48) {
        free(data);
        return JS_ThrowRangeError(ctx, "NDArray: shape too large");
    }
    bytes = (size_t)size * dyn_nd_esize(dtype);
    if (!data) {
        data = calloc(bytes ? bytes : 1, 1);
        if (!data)
            return JS_ThrowOutOfMemory(ctx);
    }
    ab = dyn_nat_ab_take(ctx, data, bytes);
    if (JS_IsException(ab))
        return ab;
    ta = nd_typed_over(ctx, ab, dtype, 0, size);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(ta))
        return ta;
    v.dtype = dtype;
    v.ndim = ndim;
    v.size = size;
    memcpy(v.shape, shape, sizeof(int64_t) * (size_t)ndim);
    dyn_nd_contig_strides(ndim, shape, v.strides);
    r = nd_make(ctx, JS_UNDEFINED, ta, &v);
    JS_FreeValue(ctx, ta);
    return r;
}

JSValue dyna_nd_new(JSContext* ctx, int dtype, int ndim, const int64_t* shape, void** data)
{
    JSValue r = dyna_nd_take(ctx, dtype, ndim, shape, NULL);
    dyn_nd_t v;
    if (JS_IsException(r))
        return r;
    if (data) {
        if (dyna_nd_view(ctx, r, &v) < 0) {
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
        *data = v.base;
    }
    return r;
}

static int nd_ta_dtype(JSContext* ctx, JSValueConst v, int want)
{
    JSClassID cid = JS_GetClassID(v);
    int i;
    if (dyna_nd_ensure_class(ctx) < 0)
        return -1;
    if (cid == nd_u16_cid)
        return want == DYN_ND_BF16 ? DYN_ND_BF16 : -2;
    for (i = 0; i < DYN_ND_NTYPES; i++)
        if (cid == nd_ta_cid[i] && i != DYN_ND_BF16 && i != DYN_ND_BOOL) {
            if (i == DYN_ND_U8 && want == DYN_ND_BOOL)
                return DYN_ND_BOOL;
            if (want >= 0 && want != i)
                return -2;
            return i;
        }
    return -3;
}

JSValue dyna_nd_wrap(JSContext* ctx, JSValueConst buffer, const dyn_nd_t* view)
{
    JSValue ta, r;
    dyn_nd_t v = *view;
    if (dyna_nd_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    if (JS_GetBufferKind(buffer) == JS_BUFFER_KIND_BUFFER) {
        size_t sz = 0;
        if (!JS_GetArrayBuffer(ctx, &sz, buffer))
            return JS_ThrowTypeError(ctx, "NDArray: the ArrayBuffer is detached");
        ta = nd_typed_over(ctx, buffer, v.dtype, 0, (int64_t)(sz / dyn_nd_esize(v.dtype)));
    } else {
        int dt = nd_ta_dtype(ctx, buffer, v.dtype);
        if (dt < 0)
            return dt == -1 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "NDArray: the typed array does not hold %s elements", dyna_nd_dtype_name(v.dtype));
        ta = JS_DupValue(ctx, buffer);
    }
    if (JS_IsException(ta))
        return ta;
    r = nd_make(ctx, JS_UNDEFINED, ta, &v);
    JS_FreeValue(ctx, ta);
    if (!JS_IsException(r) && dyna_nd_view(ctx, r, &v) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

typedef struct {
    int ndim;
    int64_t shape[DYN_ND_MAXDIM];
    int64_t n;
    double* buf;
    int64_t cap;
} nd_nest_t;

static int nd_nest_shape(JSContext* ctx, JSValueConst v, nd_nest_t* ns, int depth)
{
    int64_t len;
    JSValue e;
    if (!JS_IsArray(ctx, v))
        return 0;
    if (depth >= DYN_ND_MAXDIM) {
        JS_ThrowRangeError(ctx, "NDArray: more than %d dimensions", DYN_ND_MAXDIM);
        return -1;
    }
    e = JS_GetPropertyStr(ctx, v, "length");
    if (JS_ToInt64(ctx, &len, e)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    JS_FreeValue(ctx, e);
    ns->shape[depth] = len;
    ns->ndim = depth + 1;
    if (len == 0)
        return 0;
    e = JS_GetPropertyUint32(ctx, v, 0);
    if (JS_IsException(e))
        return -1;
    if (nd_nest_shape(ctx, e, ns, depth + 1) < 0) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    JS_FreeValue(ctx, e);
    return 0;
}

static int nd_nest_fill(JSContext* ctx, JSValueConst v, nd_nest_t* ns, int depth)
{
    int64_t len, i;
    JSValue e;
    if (depth == ns->ndim) {
        double d;
        if (JS_IsArray(ctx, v)) {
            JS_ThrowTypeError(ctx, "NDArray: nested arrays are ragged");
            return -1;
        }
        if (!JS_IsNumber(v) && !JS_IsBool(v)) {
            JS_ThrowTypeError(ctx, "NDArray: elements must be numbers");
            return -1;
        }
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        ns->buf[ns->n++] = d;
        return 0;
    }
    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "NDArray: nested arrays are ragged");
        return -1;
    }
    e = JS_GetPropertyStr(ctx, v, "length");
    if (JS_ToInt64(ctx, &len, e)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    JS_FreeValue(ctx, e);
    if (len != ns->shape[depth]) {
        JS_ThrowTypeError(ctx, "NDArray: nested arrays are ragged (axis %d has lengths %lld and %lld)", depth,
            (long long)ns->shape[depth], (long long)len);
        return -1;
    }
    for (i = 0; i < len; i++) {
        e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        if (JS_IsException(e))
            return -1;
        if (nd_nest_fill(ctx, e, ns, depth + 1) < 0) {
            JS_FreeValue(ctx, e);
            return -1;
        }
        JS_FreeValue(ctx, e);
    }
    return 0;
}

static JSValue nd_from_nested(JSContext* ctx, JSValueConst v, int dtype)
{
    nd_nest_t ns;
    JSValue r;
    dyn_nd_t out;
    memset(&ns, 0, sizeof(ns));
    if (JS_IsNumber(v) || JS_IsBool(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return JS_EXCEPTION;
        r = dyna_nd_take(ctx, dtype, 0, ns.shape, NULL);
        if (JS_IsException(r) || dyna_nd_view(ctx, r, &out) < 0)
            goto fail;
        dyn_nd_set(&out, 0, d);
        return r;
    }
    if (!JS_IsArray(ctx, v))
        return JS_ThrowTypeError(ctx, "NDArray: expected an NDArray, a typed array, a number or a (nested) array of numbers");
    if (nd_nest_shape(ctx, v, &ns, 0) < 0)
        return JS_EXCEPTION;
    ns.cap = dyn_nd_shape_size(ns.ndim, ns.shape);
    if (ns.cap < 0 || ns.cap > ((int64_t)1 << 40))
        return JS_ThrowRangeError(ctx, "NDArray: shape too large");
    ns.buf = malloc(sizeof(double) * (size_t)(ns.cap ? ns.cap : 1));
    if (!ns.buf)
        return JS_ThrowOutOfMemory(ctx);
    if (nd_nest_fill(ctx, v, &ns, 0) < 0) {
        free(ns.buf);
        return JS_EXCEPTION;
    }
    if (dtype == DYN_ND_F64)
        return dyna_nd_take(ctx, dtype, ns.ndim, ns.shape, ns.buf);
    r = dyna_nd_take(ctx, dtype, ns.ndim, ns.shape, NULL);
    if (JS_IsException(r) || dyna_nd_view(ctx, r, &out) < 0) {
        free(ns.buf);
        goto fail;
    }
    dyn_nd_from_f64(&out, ns.buf);
    free(ns.buf);
    return r;
fail:
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

JSValue dyna_nd_from_value(JSContext* ctx, JSValueConst v, int dtype, dyn_nd_t* out)
{
    JSValue r;
    if (dyna_nd_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    if (JS_GetClassID(v) == dyna_nd_class_id) {
        if (dyna_nd_view(ctx, v, out) < 0)
            return JS_EXCEPTION;
        return JS_DupValue(ctx, v);
    }
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW) {
        dyn_nd_t t;
        int dt = nd_ta_dtype(ctx, v, -1);
        size_t boff, blen, bpe;
        JSValue ab;
        if (dt < 0)
            return dt == -1 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "NDArray: unsupported typed array (use Float64Array, Float32Array, Float16Array, Int32Array, Int16Array, Int8Array or Uint8Array)");
        ab = JS_GetTypedArrayBuffer(ctx, v, &boff, &blen, &bpe);
        if (JS_IsException(ab))
            return ab;
        JS_FreeValue(ctx, ab);
        memset(&t, 0, sizeof(t));
        t.dtype = dt;
        t.ndim = 1;
        t.shape[0] = (int64_t)(blen / bpe);
        t.strides[0] = 1;
        t.size = t.shape[0];
        r = nd_make(ctx, JS_UNDEFINED, v, &t);
    } else
        r = nd_from_nested(ctx, v, dtype < 0 ? DYN_ND_F64 : dtype);
    if (JS_IsException(r))
        return r;
    if (dyna_nd_view(ctx, r, out) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

static int nd_shape_arg(JSContext* ctx, JSValueConst v, int* ndim, int64_t* shape)
{
    int64_t len, i;
    JSValue e;
    if (JS_IsNumber(v)) {
        if (JS_ToInt64(ctx, &shape[0], v))
            return -1;
        *ndim = 1;
        goto check;
    }
    {
        int isarr = JS_IsArray(ctx, v);
        if (isarr <= 0) {
            if (!isarr)
                JS_ThrowTypeError(ctx, "NDArray: shape must be an array of non-negative integers");
            return -1;
        }
    }
    e = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(e) || JS_ToInt64(ctx, &len, e)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    JS_FreeValue(ctx, e);
    if (len > DYN_ND_MAXDIM) {
        JS_ThrowRangeError(ctx, "NDArray: at most %d dimensions", DYN_ND_MAXDIM);
        return -1;
    }
    for (i = 0; i < len; i++) {
        double d;
        e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        if (JS_IsException(e))
            return -1;
        if (!JS_IsNumber(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "NDArray: shape must be an array of non-negative integers");
            return -1;
        }
        JS_ToFloat64(ctx, &d, e);
        JS_FreeValue(ctx, e);
        if (d != floor(d) || d < 0 || d > 9007199254740991.0) {
            JS_ThrowRangeError(ctx, "NDArray: shape entries must be non-negative integers");
            return -1;
        }
        shape[i] = (int64_t)d;
    }
    *ndim = (int)len;
check:
    for (i = 0; i < *ndim; i++)
        if (shape[i] < 0) {
            JS_ThrowRangeError(ctx, "NDArray: shape entries must be non-negative integers");
            return -1;
        }
    if (dyn_nd_shape_size(*ndim, shape) < 0) {
        JS_ThrowRangeError(ctx, "NDArray: shape too large");
        return -1;
    }
    return 0;
}

static int nd_int_arg(JSContext* ctx, JSValueConst v, int64_t* out, const char* what)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "NDArray: %s must be an integer", what);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    if (d != floor(d) || fabs(d) > 9007199254740991.0) {
        JS_ThrowRangeError(ctx, "NDArray: %s must be an integer", what);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static JSValue nd_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst data = argc > 0 ? argv[0] : JS_UNDEFINED, shp = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue ta = JS_UNDEFINED, r, e;
    dyn_nd_t v, chk;
    int dtype = -1, i;
    int64_t len = 0;
    if (dyna_nd_ensure_class(ctx) < 0)
        return JS_EXCEPTION;
    memset(&v, 0, sizeof(v));
    if (!JS_IsUndefined(opts) && !JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "NDArray: options must be an object");
    if (JS_IsObject(opts)) {
        static const char* const keys[] = { "dtype", "strides", "offset" };
        if (dyn_opts_strict(ctx, opts, keys, 3))
            return JS_EXCEPTION;
        e = JS_GetPropertyStr(ctx, opts, "dtype");
        dtype = dyna_nd_parse_dtype(ctx, e, -1);
        JS_FreeValue(ctx, e);
        if (dtype == -1 && JS_HasException(ctx))
            return JS_EXCEPTION;
    }
    if (JS_GetBufferKind(data) == JS_BUFFER_KIND_VIEW) {
        size_t boff, blen, bpe;
        JSValue ab;
        int dt = nd_ta_dtype(ctx, data, dtype);
        if (dt == -1)
            return JS_EXCEPTION;
        if (dt == -2)
            return JS_ThrowTypeError(ctx, "NDArray: the typed array does not hold %s elements", dyna_nd_dtype_name(dtype));
        if (dt < 0)
            return JS_ThrowTypeError(ctx, "NDArray: unsupported typed array (Uint16Array needs dtype bfloat16)");
        dtype = dt;
        ab = JS_GetTypedArrayBuffer(ctx, data, &boff, &blen, &bpe);
        if (JS_IsException(ab))
            return ab;
        JS_FreeValue(ctx, ab);
        len = (int64_t)(blen / bpe);
        ta = JS_DupValue(ctx, data);
    } else if (JS_GetBufferKind(data) == JS_BUFFER_KIND_BUFFER) {
        size_t sz = 0;
        if (dtype < 0)
            dtype = DYN_ND_F64;
        if (!JS_GetArrayBuffer(ctx, &sz, data))
            return JS_ThrowTypeError(ctx, "NDArray: the ArrayBuffer is detached");
        len = (int64_t)(sz / dyn_nd_esize(dtype));
        if (sz % dyn_nd_esize(dtype)) {
            ta = nd_typed_over(ctx, data, dtype, 0, len);
        } else {
            JSValueConst targs[3];
            targs[0] = data;
            targs[1] = JS_NewInt32(ctx, 0);
            targs[2] = JS_UNDEFINED;
            ta = JS_NewTypedArray(ctx, 3, targs, nd_ta_kind[dtype]);
        }
        if (JS_IsException(ta))
            return ta;
    } else if (JS_IsArray(ctx, data) || JS_IsNumber(data)) {
        dyn_nd_t src;
        JSValue tmp = nd_from_nested(ctx, data, dtype < 0 ? DYN_ND_F64 : dtype);
        nd_obj_t* o;
        if (JS_IsException(tmp))
            return tmp;
        o = JS_GetOpaque(tmp, dyna_nd_class_id);
        if (JS_IsObject(opts)) {
            JSValue so = JS_GetPropertyStr(ctx, opts, "strides"), oo = JS_GetPropertyStr(ctx, opts, "offset");
            int bad = !JS_IsUndefined(so) || !JS_IsUndefined(oo);
            JS_FreeValue(ctx, so);
            JS_FreeValue(ctx, oo);
            if (bad) {
                JS_FreeValue(ctx, tmp);
                return JS_ThrowTypeError(ctx, "NDArray: strides/offset need a typed array or an ArrayBuffer");
            }
        }
        if (JS_IsUndefined(shp)) {
            r = nd_make(ctx, new_target, o->ta, &o->v);
            JS_FreeValue(ctx, tmp);
            return r;
        }
        if (dyna_nd_view(ctx, tmp, &src) < 0) {
            JS_FreeValue(ctx, tmp);
            return JS_EXCEPTION;
        }
        dtype = src.dtype;
        len = src.size;
        ta = JS_DupValue(ctx, o->ta);
        JS_FreeValue(ctx, tmp);
    } else
        return JS_ThrowTypeError(ctx, "NDArray: data must be a typed array, an ArrayBuffer, a number or a (nested) array of numbers");
    v.dtype = dtype;
    if (JS_IsUndefined(shp)) {
        v.ndim = 1;
        v.shape[0] = len;
    } else if (nd_shape_arg(ctx, shp, &v.ndim, v.shape) < 0)
        goto fail;
    v.size = dyn_nd_shape_size(v.ndim, v.shape);
    dyn_nd_contig_strides(v.ndim, v.shape, v.strides);
    if (JS_IsObject(opts)) {
        e = JS_GetPropertyStr(ctx, opts, "offset");
        if (!JS_IsUndefined(e) && nd_int_arg(ctx, e, &v.offset, "offset") < 0) {
            JS_FreeValue(ctx, e);
            goto fail;
        }
        JS_FreeValue(ctx, e);
        e = JS_GetPropertyStr(ctx, opts, "strides");
        if (!JS_IsUndefined(e)) {
            int nd;
            int64_t st[DYN_ND_MAXDIM];
            if (!JS_IsArray(ctx, e)) {
                JS_FreeValue(ctx, e);
                JS_ThrowTypeError(ctx, "NDArray: strides must be an array of integers");
                goto fail;
            }
            {
                int64_t ln;
                if (dyn_js_len(ctx, e, &ln)) {
                    JS_FreeValue(ctx, e);
                    goto fail;
                }
                nd = (int)ln;
                if (ln != v.ndim) {
                    JS_FreeValue(ctx, e);
                    JS_ThrowRangeError(ctx, "NDArray: strides has %lld entries but shape has %d", (long long)ln, v.ndim);
                    goto fail;
                }
            }
            for (i = 0; i < nd; i++) {
                JSValue x = JS_GetPropertyUint32(ctx, e, (uint32_t)i);
                int bad = nd_int_arg(ctx, x, &st[i], "a stride");
                JS_FreeValue(ctx, x);
                if (bad < 0) {
                    JS_FreeValue(ctx, e);
                    goto fail;
                }
                v.strides[i] = st[i];
            }
        }
        JS_FreeValue(ctx, e);
    }
    r = nd_make(ctx, new_target, ta, &v);
    JS_FreeValue(ctx, ta);
    if (JS_IsException(r))
        return r;
    if (dyna_nd_view(ctx, r, &chk) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
fail:
    JS_FreeValue(ctx, ta);
    return JS_EXCEPTION;
}

static JSValue nd_copy_contig(JSContext* ctx, const dyn_nd_t* src, int dtype)
{
    dyn_nd_t dst;
    JSValue r = dyna_nd_take(ctx, dtype, src->ndim, src->shape, NULL);
    if (JS_IsException(r))
        return r;
    if (dyna_nd_view(ctx, r, &dst) < 0 || dyn_nd_copy(&dst, src) < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

static JSValue nd_get_dtype(JSContext* ctx, JSValueConst this_val)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    return o ? JS_NewString(ctx, nd_dtype_names[o->v.dtype]) : JS_EXCEPTION;
}

static JSValue nd_get_int(JSContext* ctx, JSValueConst this_val, int magic)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    if (!o)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewInt32(ctx, o->v.ndim);
    case 1:
        return JS_NewInt64(ctx, o->v.size);
    case 2:
        return JS_NewInt64(ctx, o->v.offset);
    default: {
        dyn_nd_t v;
        if (nd_resolve(ctx, o, &v) < 0)
            return JS_EXCEPTION;
        return JS_NewBool(ctx, dyn_nd_is_contig(&v));
    }
    }
}

static JSValue nd_int_list(JSContext* ctx, int n, const int64_t* a)
{
    JSValue r = JS_NewArray(ctx);
    int i;
    if (JS_IsException(r))
        return r;
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, r, (uint32_t)i, JS_NewInt64(ctx, a[i]), JS_PROP_C_W_E);
    return r;
}

static JSValue nd_get_list(JSContext* ctx, JSValueConst this_val, int magic)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    if (!o)
        return JS_EXCEPTION;
    return nd_int_list(ctx, o->v.ndim, magic ? o->v.strides : o->v.shape);
}

static JSValue nd_get_data(JSContext* ctx, JSValueConst this_val)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    return o ? JS_DupValue(ctx, o->ta) : JS_EXCEPTION;
}

static int nd_axis(JSContext* ctx, JSValueConst v, int ndim, int extra, int* out)
{
    int64_t a;
    if (nd_int_arg(ctx, v, &a, "axis") < 0)
        return -1;
    if (a < 0)
        a += ndim + extra;
    if (a < 0 || a >= ndim + extra) {
        JS_ThrowRangeError(ctx, "NDArray: axis %lld is out of range for %d dimensions", (long long)a, ndim + extra);
        return -1;
    }
    *out = (int)a;
    return 0;
}

static JSValue nd_transpose(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int perm[DYN_ND_MAXDIM], seen[DYN_ND_MAXDIM], i, n;
    JSValue list = JS_UNDEFINED;
    if (!o)
        return JS_EXCEPTION;
    v = o->v;
    n = v.ndim;
    if (argc == 1 && JS_IsArray(ctx, argv[0]))
        list = argv[0];
    if (argc == 0) {
        for (i = 0; i < n; i++)
            perm[i] = n - 1 - i;
    } else {
        int cnt = argc;
        if (!JS_IsUndefined(list)) {
            int64_t ln;
            if (dyn_js_len(ctx, list, &ln))
                return JS_EXCEPTION;
            cnt = (int)(ln > DYN_ND_MAXDIM + 1 ? DYN_ND_MAXDIM + 1 : ln);
        }
        if (cnt != n)
            return JS_ThrowRangeError(ctx, "NDArray.transpose: axes must list all %d axes", n);
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < n; i++) {
            JSValue x = JS_IsUndefined(list) ? JS_DupValue(ctx, argv[i]) : JS_GetPropertyUint32(ctx, list, (uint32_t)i);
            int bad = nd_axis(ctx, x, n, 0, &perm[i]);
            JS_FreeValue(ctx, x);
            if (bad < 0)
                return JS_EXCEPTION;
            if (seen[perm[i]]++)
                return JS_ThrowRangeError(ctx, "NDArray.transpose: repeated axis %d", perm[i]);
        }
    }
    for (i = 0; i < n; i++) {
        v.shape[i] = o->v.shape[perm[i]];
        v.strides[i] = o->v.strides[perm[i]];
    }
    return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
}

static JSValue nd_get_T(JSContext* ctx, JSValueConst this_val)
{
    return nd_transpose(ctx, this_val, 0, NULL);
}

static int nd_index(JSContext* ctx, const dyn_nd_t* v, int argc, JSValueConst* argv, int64_t* off)
{
    int i;
    *off = v->offset;
    if (argc != v->ndim) {
        JS_ThrowRangeError(ctx, "NDArray: %d indices given for %d dimensions", argc, v->ndim);
        return -1;
    }
    for (i = 0; i < argc; i++) {
        int64_t k;
        if (nd_int_arg(ctx, argv[i], &k, "an index") < 0)
            return -1;
        if (k < 0)
            k += v->shape[i];
        if (k < 0 || k >= v->shape[i]) {
            JS_ThrowRangeError(ctx, "NDArray: index out of range on axis %d (size %lld)", i, (long long)v->shape[i]);
            return -1;
        }
        *off += k * v->strides[i];
    }
    return 0;
}

static JSValue nd_get_el(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int64_t off;
    double d;
    if (!o || nd_resolve(ctx, o, &v) < 0 || nd_index(ctx, &v, argc, argv, &off) < 0)
        return JS_EXCEPTION;
    d = dyn_nd_load(v.dtype, (char*)v.base + (size_t)off * dyn_nd_esize(v.dtype));
    if (v.dtype == DYN_ND_BOOL)
        return JS_NewBool(ctx, d != 0);
    return JS_NewFloat64(ctx, d);
}

static JSValue nd_set_el(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int64_t off;
    double d;
    if (!o)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "NDArray.set: expected indices then a value");
    if (!JS_IsNumber(argv[argc - 1]) && !JS_IsBool(argv[argc - 1]))
        return JS_ThrowTypeError(ctx, "NDArray.set: the value must be a number");
    if (JS_ToFloat64(ctx, &d, argv[argc - 1]))
        return JS_EXCEPTION;
    if (JS_CheckWritableBuffer(ctx, o->ta) < 0 || nd_resolve(ctx, o, &v) < 0 || nd_index(ctx, &v, argc - 1, argv, &off) < 0)
        return JS_EXCEPTION;
    dyn_nd_store(v.dtype, (char*)v.base + (size_t)off * dyn_nd_esize(v.dtype), d);
    return JS_DupValue(ctx, this_val);
}

static int nd_parse_slice_str(JSContext* ctx, const char* s, int has[3], int64_t val[3], int* is_index)
{
    int part = 0;
    const char* p = s;
    has[0] = has[1] = has[2] = 0;
    *is_index = 0;
    while (*p == ' ')
        p++;
    for (;;) {
        char* end;
        long long x;
        while (*p == ' ')
            p++;
        if (*p == ':' || *p == 0) {
        } else {
            x = strtoll(p, &end, 10);
            if (end == p)
                goto bad;
            has[part] = 1;
            val[part] = x;
            p = end;
            while (*p == ' ')
                p++;
        }
        if (*p == 0)
            break;
        if (*p != ':' || part == 2)
            goto bad;
        part++;
        p++;
    }
    if (part == 0) {
        if (!has[0])
            goto bad;
        *is_index = 1;
    }
    return 0;
bad:
    JS_ThrowSyntaxError(ctx, "NDArray.slice: bad slice \"%s\" (use \"start:stop:step\")", s);
    return -1;
}

static int nd_apply_slice(JSContext* ctx, int64_t n, const int has[3], const int64_t val[3], int64_t* start, int64_t* step,
    int64_t* len)
{
    int64_t st = has[2] ? val[2] : 1, a, b;
    if (st == 0) {
        JS_ThrowRangeError(ctx, "NDArray.slice: step cannot be zero");
        return -1;
    }
    if (has[0]) {
        a = val[0];
        if (a < 0)
            a += n;
        if (a < 0)
            a = st < 0 ? -1 : 0;
        else if (a >= n)
            a = st < 0 ? n - 1 : n;
    } else
        a = st < 0 ? n - 1 : 0;
    if (has[1]) {
        b = val[1];
        if (b < 0)
            b += n;
        if (b < 0)
            b = st < 0 ? -1 : 0;
        else if (b >= n)
            b = st < 0 ? n - 1 : n;
    } else
        b = st < 0 ? -1 : n;
    if (st < 0)
        *len = b < a ? (a - b - 1) / (-st) + 1 : 0;
    else
        *len = a < b ? (b - a - 1) / st + 1 : 0;
    *start = a;
    *step = st;
    return 0;
}

static JSValue nd_slice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int i, out = 0;
    if (!o)
        return JS_EXCEPTION;
    if (argc > o->v.ndim)
        return JS_ThrowRangeError(ctx, "NDArray.slice: %d specs for %d dimensions", argc, o->v.ndim);
    v = o->v;
    for (i = 0; i < o->v.ndim; i++) {
        JSValueConst sp = i < argc ? argv[i] : JS_UNDEFINED;
        int has[3] = { 0, 0, 0 }, is_index = 0;
        int64_t val[3] = { 0, 0, 0 }, n = o->v.shape[i], start, step, len;
        if (JS_IsNumber(sp)) {
            if (nd_int_arg(ctx, sp, &val[0], "a slice index") < 0)
                return JS_EXCEPTION;
            has[0] = 1;
            is_index = 1;
        } else if (JS_IsString(sp)) {
            const char* s = JS_ToCString(ctx, sp);
            int bad;
            if (!s)
                return JS_EXCEPTION;
            bad = nd_parse_slice_str(ctx, s, has, val, &is_index);
            JS_FreeCString(ctx, s);
            if (bad < 0)
                return JS_EXCEPTION;
        } else if (JS_IsArray(ctx, sp)) {
            int k;
            for (k = 0; k < 3; k++) {
                JSValue x = JS_GetPropertyUint32(ctx, sp, (uint32_t)k);
                if (!JS_IsUndefined(x) && !JS_IsNull(x)) {
                    if (nd_int_arg(ctx, x, &val[k], "a slice bound") < 0) {
                        JS_FreeValue(ctx, x);
                        return JS_EXCEPTION;
                    }
                    has[k] = 1;
                }
                JS_FreeValue(ctx, x);
            }
        } else if (!JS_IsUndefined(sp) && !JS_IsNull(sp))
            return JS_ThrowTypeError(ctx, "NDArray.slice: a spec is an integer, null, \"start:stop:step\" or [start, stop, step]");
        if (is_index) {
            int64_t k = val[0];
            if (k < 0)
                k += n;
            if (k < 0 || k >= n)
                return JS_ThrowRangeError(ctx, "NDArray.slice: index out of range on axis %d (size %lld)", i, (long long)n);
            v.offset += k * o->v.strides[i];
            continue;
        }
        if (nd_apply_slice(ctx, n, has, val, &start, &step, &len) < 0)
            return JS_EXCEPTION;
        if (len > 0)
            v.offset += start * o->v.strides[i];
        v.shape[out] = len;
        v.strides[out] = o->v.strides[i] * step;
        out++;
    }
    v.ndim = out;
    v.size = dyn_nd_shape_size(out, v.shape);
    return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
}

static int nd_shape_from_args(JSContext* ctx, int argc, JSValueConst* argv, int* ndim, int64_t* shape)
{
    int i;
    if (argc == 1 && JS_IsArray(ctx, argv[0])) {
        int64_t ln;
        if (dyn_js_len(ctx, argv[0], &ln))
            return -1;
        if (ln > DYN_ND_MAXDIM) {
            JS_ThrowRangeError(ctx, "NDArray: at most %d dimensions", DYN_ND_MAXDIM);
            return -1;
        }
        for (i = 0; i < ln; i++) {
            JSValue x = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            int bad = nd_int_arg(ctx, x, &shape[i], "a dimension");
            JS_FreeValue(ctx, x);
            if (bad < 0)
                return -1;
        }
        *ndim = (int)ln;
        return 0;
    }
    if (argc > DYN_ND_MAXDIM) {
        JS_ThrowRangeError(ctx, "NDArray: at most %d dimensions", DYN_ND_MAXDIM);
        return -1;
    }
    for (i = 0; i < argc; i++)
        if (nd_int_arg(ctx, argv[i], &shape[i], "a dimension") < 0)
            return -1;
    *ndim = argc;
    return 0;
}

static int nd_nocopy_reshape(const dyn_nd_t* a, int nnd, const int64_t* ndims, int64_t* nstr)
{
    int64_t od[DYN_ND_MAXDIM], os[DYN_ND_MAXDIM];
    int ond = 0, oi, oj, ni, nj, ok, nk, i;
    for (i = 0; i < a->ndim; i++)
        if (a->shape[i] != 1) {
            od[ond] = a->shape[i];
            os[ond] = a->strides[i];
            ond++;
        }
    oi = 0;
    oj = 1;
    ni = 0;
    nj = 1;
    while (ni < nnd && oi < ond) {
        int64_t np = ndims[ni], op = od[oi];
        while (np != op) {
            if (np < op) {
                if (nj >= nnd)
                    return 0;
                np *= ndims[nj++];
            } else {
                if (oj >= ond)
                    return 0;
                op *= od[oj++];
            }
        }
        for (ok = oi; ok < oj - 1; ok++)
            if (os[ok] != od[ok + 1] * os[ok + 1])
                return 0;
        nstr[nj - 1] = os[oj - 1];
        for (nk = nj - 1; nk > ni; nk--)
            nstr[nk - 1] = nstr[nk] * ndims[nk];
        ni = nj++;
        oi = oj++;
    }
    for (nk = ni; nk < nnd; nk++)
        nstr[nk] = 1;
    return 1;
}

static JSValue nd_reshape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v, cur;
    int nnd, i, infer = -1;
    int64_t shape[DYN_ND_MAXDIM], known = 1;
    if (!o || nd_shape_from_args(ctx, argc, argv, &nnd, shape) < 0)
        return JS_EXCEPTION;
    for (i = 0; i < nnd; i++) {
        if (shape[i] == -1) {
            if (infer >= 0)
                return JS_ThrowRangeError(ctx, "NDArray.reshape: only one dimension can be -1");
            infer = i;
        } else if (shape[i] < 0)
            return JS_ThrowRangeError(ctx, "NDArray.reshape: negative dimension %lld", (long long)shape[i]);
        else
            known *= shape[i];
    }
    if (infer >= 0) {
        if (known == 0 || o->v.size % known)
            return JS_ThrowRangeError(ctx, "NDArray.reshape: cannot infer -1 for size %lld", (long long)o->v.size);
        shape[infer] = o->v.size / known;
    }
    if (dyn_nd_shape_size(nnd, shape) != o->v.size)
        return JS_ThrowRangeError(ctx, "NDArray.reshape: cannot reshape size %lld into the given shape", (long long)o->v.size);
    v = o->v;
    v.ndim = nnd;
    memcpy(v.shape, shape, sizeof(int64_t) * (size_t)nnd);
    if (o->v.size == 0) {
        dyn_nd_contig_strides(nnd, shape, v.strides);
        return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
    }
    if (nd_nocopy_reshape(&o->v, nnd, shape, v.strides))
        return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
    if (nd_resolve(ctx, o, &cur) < 0)
        return JS_EXCEPTION;
    {
        JSValue c = nd_copy_contig(ctx, &cur, cur.dtype), r;
        nd_obj_t* co;
        if (JS_IsException(c))
            return c;
        co = JS_GetOpaque(c, dyna_nd_class_id);
        dyn_nd_contig_strides(nnd, shape, v.strides);
        v.offset = 0;
        r = nd_make(ctx, JS_UNDEFINED, co->ta, &v);
        JS_FreeValue(ctx, c);
        return r;
    }
}

static int nd_broadcast_view(JSContext* ctx, const dyn_nd_t* a, int nd, const int64_t* shape, dyn_nd_t* out)
{
    int i;
    *out = *a;
    if (a->ndim > nd) {
        JS_ThrowRangeError(ctx, "NDArray: cannot broadcast %d dimensions to %d", a->ndim, nd);
        return -1;
    }
    for (i = 0; i < nd; i++) {
        int j = i - (nd - a->ndim);
        if (j < 0) {
            out->shape[i] = shape[i];
            out->strides[i] = 0;
        } else if (a->shape[j] == shape[i]) {
            out->shape[i] = shape[i];
            out->strides[i] = a->strides[j];
        } else if (a->shape[j] == 1) {
            out->shape[i] = shape[i];
            out->strides[i] = 0;
        } else {
            JS_ThrowRangeError(ctx, "NDArray: shapes are not broadcastable (axis %d: %lld vs %lld)", i, (long long)a->shape[j],
                (long long)shape[i]);
            return -1;
        }
    }
    out->ndim = nd;
    out->size = dyn_nd_shape_size(nd, shape);
    return 0;
}

static JSValue nd_broadcast_to(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int nd;
    int64_t shape[DYN_ND_MAXDIM];
    if (!o || nd_shape_from_args(ctx, argc, argv, &nd, shape) < 0)
        return JS_EXCEPTION;
    if (nd_broadcast_view(ctx, &o->v, nd, shape, &v) < 0)
        return JS_EXCEPTION;
    return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
}

static JSValue nd_expand_dims(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int ax, i;
    if (!o)
        return JS_EXCEPTION;
    if (o->v.ndim >= DYN_ND_MAXDIM)
        return JS_ThrowRangeError(ctx, "NDArray: at most %d dimensions", DYN_ND_MAXDIM);
    if (nd_axis(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, o->v.ndim, 1, &ax) < 0)
        return JS_EXCEPTION;
    v = o->v;
    for (i = o->v.ndim; i > ax; i--) {
        v.shape[i] = v.shape[i - 1];
        v.strides[i] = v.strides[i - 1];
    }
    v.shape[ax] = 1;
    v.strides[ax] = 1;
    v.ndim++;
    return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
}

static JSValue nd_squeeze(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int ax = -1, i, out = 0;
    if (!o)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (nd_axis(ctx, argv[0], o->v.ndim, 0, &ax) < 0)
            return JS_EXCEPTION;
        if (o->v.shape[ax] != 1)
            return JS_ThrowRangeError(ctx, "NDArray.squeeze: axis %d has size %lld, not 1", ax, (long long)o->v.shape[ax]);
    }
    v = o->v;
    for (i = 0; i < o->v.ndim; i++) {
        if (o->v.shape[i] == 1 && (ax < 0 || ax == i))
            continue;
        v.shape[out] = o->v.shape[i];
        v.strides[out] = o->v.strides[i];
        out++;
    }
    v.ndim = out;
    return nd_make(ctx, JS_UNDEFINED, o->ta, &v);
}

static JSValue nd_contiguous(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    if (!o || nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    if (dyn_nd_is_contig(&v))
        return JS_DupValue(ctx, this_val);
    return nd_copy_contig(ctx, &v, v.dtype);
}

static JSValue nd_copy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    if (!o || nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    return nd_copy_contig(ctx, &v, v.dtype);
}

static JSValue nd_astype(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    int dt;
    if (!o)
        return JS_EXCEPTION;
    dt = dyna_nd_parse_dtype(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, -1);
    if (dt < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "NDArray.astype: a dtype is required");
        return JS_EXCEPTION;
    }
    if (nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    return nd_copy_contig(ctx, &v, dt);
}

static JSValue nd_fill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v, s;
    double d;
    char one[8];
    if (!o)
        return JS_EXCEPTION;
    if (argc < 1 || (!JS_IsNumber(argv[0]) && !JS_IsBool(argv[0])))
        return JS_ThrowTypeError(ctx, "NDArray.fill: the value must be a number");
    JS_ToFloat64(ctx, &d, argv[0]);
    if (JS_CheckWritableBuffer(ctx, o->ta) < 0 || nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    dyn_nd_store(v.dtype, one, d);
    memset(&s, 0, sizeof(s));
    s = v;
    s.base = one;
    s.base_len = 1;
    s.offset = 0;
    memset(s.strides, 0, sizeof(s.strides));
    if (dyn_nd_copy(&v, &s) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue nd_assign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v, src, b;
    JSValue keep;
    if (!o)
        return JS_EXCEPTION;
    if (argc > 0 && (JS_IsNumber(argv[0]) || JS_IsBool(argv[0])))
        return nd_fill(ctx, this_val, argc, argv);
    if (JS_CheckWritableBuffer(ctx, o->ta) < 0)
        return JS_EXCEPTION;
    keep = dyna_nd_from_value(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, o->v.dtype, &src);
    if (JS_IsException(keep))
        return keep;
    if (nd_resolve(ctx, o, &v) < 0 || nd_broadcast_view(ctx, &src, v.ndim, v.shape, &b) < 0) {
        JS_FreeValue(ctx, keep);
        return JS_EXCEPTION;
    }
    if (dyn_nd_copy(&v, &b) < 0) {
        JS_FreeValue(ctx, keep);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_FreeValue(ctx, keep);
    return JS_DupValue(ctx, this_val);
}

static JSValue nd_to_array_rec(JSContext* ctx, const dyn_nd_t* v, int d, int64_t off)
{
    JSValue r;
    int64_t i;
    if (d == v->ndim) {
        double x = dyn_nd_load(v->dtype, (char*)v->base + (size_t)off * dyn_nd_esize(v->dtype));
        return v->dtype == DYN_ND_BOOL ? JS_NewBool(ctx, x != 0) : JS_NewFloat64(ctx, x);
    }
    r = JS_NewArray(ctx);
    if (JS_IsException(r))
        return r;
    for (i = 0; i < v->shape[d]; i++) {
        JSValue e = nd_to_array_rec(ctx, v, d + 1, off + i * v->strides[d]);
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, r);
            return e;
        }
        JS_DefinePropertyValueUint32(ctx, r, (uint32_t)i, e, JS_PROP_C_W_E);
    }
    return r;
}

static JSValue nd_to_array(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    if (!o || nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    return nd_to_array_rec(ctx, &v, 0, v.offset);
}

static JSValue nd_to_typed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    dyn_nd_t v;
    JSValue c, r;
    if (!o || nd_resolve(ctx, o, &v) < 0)
        return JS_EXCEPTION;
    c = nd_copy_contig(ctx, &v, v.dtype);
    if (JS_IsException(c))
        return c;
    r = JS_DupValue(ctx, ((nd_obj_t*)JS_GetOpaque(c, dyna_nd_class_id))->ta);
    JS_FreeValue(ctx, c);
    return r;
}

static JSValue nd_to_string(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    nd_obj_t* o = nd_get(ctx, this_val);
    char buf[256];
    int i, n;
    if (!o)
        return JS_EXCEPTION;
    n = snprintf(buf, sizeof(buf), "NDArray(%s, [", nd_dtype_names[o->v.dtype]);
    for (i = 0; i < o->v.ndim && n < (int)sizeof(buf) - 32; i++)
        n = (int)dyn_snappend(buf, sizeof(buf), (size_t)n, "%s%lld", i ? ", " : "", (long long)o->v.shape[i]);
    dyn_snappend(buf, sizeof(buf), (size_t)n, "])");
    return JS_NewString(ctx, buf);
}

static JSValue nd_s_fill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int nd, dt;
    int64_t shape[DYN_ND_MAXDIM];
    double val = magic == 1 ? 1.0 : 0.0;
    JSValue r;
    dyn_nd_t v;
    if (nd_shape_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &nd, shape) < 0)
        return JS_EXCEPTION;
    if (magic == 2) {
        if (argc < 2 || !JS_IsNumber(argv[1]))
            return JS_ThrowTypeError(ctx, "NDArray.full: the fill value must be a number");
        JS_ToFloat64(ctx, &val, argv[1]);
    }
    dt = dyna_nd_parse_dtype(ctx, argc > (magic == 2 ? 2 : 1) ? argv[magic == 2 ? 2 : 1] : JS_UNDEFINED, DYN_ND_F64);
    if (dt < 0)
        return JS_EXCEPTION;
    r = dyna_nd_take(ctx, dt, nd, shape, NULL);
    if (JS_IsException(r) || val == 0.0)
        return r;
    if (dyna_nd_view(ctx, r, &v) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    {
        int64_t i;
        size_t es = dyn_nd_esize(dt);
        for (i = 0; i < v.size; i++)
            dyn_nd_store(dt, (char*)v.base + (size_t)i * es, val);
    }
    return r;
}

static JSValue nd_s_eye(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t shape[2], i;
    int dt;
    JSValue r;
    dyn_nd_t v;
    if (nd_int_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &shape[0], "n") < 0)
        return JS_EXCEPTION;
    shape[1] = shape[0];
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && nd_int_arg(ctx, argv[1], &shape[1], "m") < 0)
        return JS_EXCEPTION;
    if (shape[0] < 0 || shape[1] < 0)
        return JS_ThrowRangeError(ctx, "NDArray.eye: sizes must be non-negative");
    dt = dyna_nd_parse_dtype(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, DYN_ND_F64);
    if (dt < 0)
        return JS_EXCEPTION;
    r = dyna_nd_take(ctx, dt, 2, shape, NULL);
    if (JS_IsException(r) || dyna_nd_view(ctx, r, &v) < 0)
        goto fail;
    for (i = 0; i < shape[0] && i < shape[1]; i++)
        dyn_nd_store(dt, (char*)v.base + (size_t)(i * shape[1] + i) * dyn_nd_esize(dt), 1.0);
    return r;
fail:
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

static JSValue nd_s_arange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double a = 0, b, st = 1, n;
    int64_t shape[1], i;
    int dt;
    JSValue r;
    dyn_nd_t v;
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "NDArray.arange: start must be a number");
    JS_ToFloat64(ctx, &b, argv[0]);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsNumber(argv[1]))
            return JS_ThrowTypeError(ctx, "NDArray.arange: stop must be a number");
        a = b;
        JS_ToFloat64(ctx, &b, argv[1]);
    }
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (!JS_IsNumber(argv[2]))
            return JS_ThrowTypeError(ctx, "NDArray.arange: step must be a number");
        JS_ToFloat64(ctx, &st, argv[2]);
    }
    if (st == 0 || !isfinite(a) || !isfinite(b) || !isfinite(st))
        return JS_ThrowRangeError(ctx, "NDArray.arange: start, stop and step must be finite and step non-zero");
    n = ceil((b - a) / st);
    if (n < 0)
        n = 0;
    if (n > 1e12)
        return JS_ThrowRangeError(ctx, "NDArray.arange: too many elements");
    shape[0] = (int64_t)n;
    dt = dyna_nd_parse_dtype(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, DYN_ND_F64);
    if (dt < 0)
        return JS_EXCEPTION;
    r = dyna_nd_take(ctx, dt, 1, shape, NULL);
    if (JS_IsException(r) || dyna_nd_view(ctx, r, &v) < 0)
        goto fail;
    for (i = 0; i < shape[0]; i++)
        dyn_nd_store(dt, (char*)v.base + (size_t)i * dyn_nd_esize(dt), a + (double)i * st);
    return r;
fail:
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}

static JSValue nd_s_from(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int dt = dyna_nd_parse_dtype(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, DYN_ND_F64);
    if (dt < 0)
        return JS_EXCEPTION;
    if (argc > 0 && JS_GetClassID(argv[0]) == dyna_nd_class_id) {
        dyn_nd_t v;
        if (dyna_nd_view(ctx, argv[0], &v) < 0)
            return JS_EXCEPTION;
        return nd_copy_contig(ctx, &v, dt);
    }
    if (argc > 0 && JS_GetBufferKind(argv[0]) == JS_BUFFER_KIND_VIEW) {
        dyn_nd_t v;
        JSValue keep = dyna_nd_from_value(ctx, argv[0], -1, &v), r;
        if (JS_IsException(keep))
            return keep;
        r = nd_copy_contig(ctx, &v, argc > 1 && !JS_IsUndefined(argv[1]) ? dt : v.dtype);
        JS_FreeValue(ctx, keep);
        return r;
    }
    return nd_from_nested(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, dt);
}

static const JSCFunctionListEntry nd_proto_funcs[] = {
    JS_CGETSET_DEF("dtype", nd_get_dtype, NULL),
    JS_CGETSET_MAGIC_DEF("ndim", nd_get_int, NULL, 0),
    JS_CGETSET_MAGIC_DEF("size", nd_get_int, NULL, 1),
    JS_CGETSET_MAGIC_DEF("offset", nd_get_int, NULL, 2),
    JS_CGETSET_MAGIC_DEF("isContiguous", nd_get_int, NULL, 3),
    JS_CGETSET_MAGIC_DEF("shape", nd_get_list, NULL, 0),
    JS_CGETSET_MAGIC_DEF("strides", nd_get_list, NULL, 1),
    JS_CGETSET_DEF("data", nd_get_data, NULL),
    JS_CGETSET_DEF("T", nd_get_T, NULL),
    JS_CFUNC_DEF("get", 0, nd_get_el),
    JS_CFUNC_DEF("set", 1, nd_set_el),
    JS_CFUNC_DEF("slice", 0, nd_slice),
    JS_CFUNC_DEF("transpose", 0, nd_transpose),
    JS_CFUNC_DEF("reshape", 1, nd_reshape),
    JS_CFUNC_DEF("broadcastTo", 1, nd_broadcast_to),
    JS_CFUNC_DEF("expandDims", 1, nd_expand_dims),
    JS_CFUNC_DEF("squeeze", 0, nd_squeeze),
    JS_CFUNC_DEF("contiguous", 0, nd_contiguous),
    JS_CFUNC_DEF("copy", 0, nd_copy),
    JS_CFUNC_DEF("astype", 1, nd_astype),
    JS_CFUNC_DEF("fill", 1, nd_fill),
    JS_CFUNC_DEF("assign", 1, nd_assign),
    JS_CFUNC_DEF("toArray", 0, nd_to_array),
    JS_CFUNC_DEF("toTypedArray", 0, nd_to_typed),
    JS_CFUNC_DEF("toString", 0, nd_to_string),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "NDArray", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry nd_static_funcs[] = {
    JS_CFUNC_MAGIC_DEF("zeros", 1, nd_s_fill, 0),
    JS_CFUNC_MAGIC_DEF("ones", 1, nd_s_fill, 1),
    JS_CFUNC_MAGIC_DEF("full", 2, nd_s_fill, 2),
    JS_CFUNC_DEF("eye", 1, nd_s_eye),
    JS_CFUNC_DEF("arange", 1, nd_s_arange),
    JS_CFUNC_DEF("from", 1, nd_s_from),
};

static int nd_proto_count(void)
{
    return (int)countof(nd_proto_funcs);
}

static int nd_static_count(void)
{
    return (int)countof(nd_static_funcs);
}

JSValue dyna_nd_ctor_value(JSContext* ctx)
{
    return nd_class_ctor(ctx);
}

#endif
