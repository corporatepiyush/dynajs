#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

#define DYN_RANDOM_POISSON_MAX 2147483647.0
#define DYN_RANDOM_BYTES_MAX 1073741824.0

JSClassID dyn_random_class_id;

static void dyn_random_dispose(void* native)
{
    free(native);
}

static void dyn_random_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_random_dispose(JS_GetOpaque(val, dyn_random_class_id));
}

static const JSClassDef dyn_random_class = {
    "Random",
    .finalizer = dyn_random_finalizer,
};

static JSValue dyn_random_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    uint64_t seed = 0;
    int has_seed = 1;

    if (argc < 1 || JS_IsUndefined(argv[0])) {
        has_seed = 0;
    } else if (JS_IsBigInt(ctx, argv[0])) {
        int64_t s;
        if (JS_ToBigInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    } else {
        int64_t s;
        if (JS_ToInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    }

    rng = (dyn_prng_t*)malloc(sizeof(*rng));
    if (!rng)
        return JS_ThrowOutOfMemory(ctx);
    if (!has_seed) {
        if (dyn_prng_seed_random(rng) < 0) {
            free(rng);
            return JS_ThrowInternalError(ctx,
                "new Random(): OS entropy unavailable");
        }
    } else {
        dyn_prng_seed(rng, seed);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_random_class_id, rng, dyn_random_dispose);
}

static int dyn_random_arg_num(JSContext* ctx, JSValueConst v, int has_arg,
    double def, double lo, double hi,
    double* out, const char* what)
{
    double d;

    if (!has_arg || JS_IsUndefined(v)) {
        *out = def;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo) || d > hi || !isfinite(d)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number in [%#g, %#g]",
            what, lo, hi);
        return -1;
    }
    *out = d;
    return 0;
}

int dyn_random_arg_real(JSContext* ctx, JSValueConst v, int has_arg,
    double def, double* out, const char* what)
{
    double d = def;

    if (!has_arg || JS_IsUndefined(v)) {
        *out = def;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number", what);
        return -1;
    }
    *out = d;
    return 0;
}

int dyn_random_arg_int(JSContext* ctx, JSValueConst v, int has_arg,
    double def, double lo, double hi,
    double* out, const char* what)
{
    double d = *out;

    if (dyn_random_arg_num(ctx, v, has_arg, def, lo, hi, &d, what))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s must be an integer", what);
        return -1;
    }
    *out = d;
    return 0;
}

static JSValue dyn_random_normal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double mu = 0.0, sigma = 1.0;

    if (dyn_random_arg_real(ctx, argv[0], argc > 0, 0.0,
            &mu, "normal(mu, sigma): mu")
        || dyn_random_arg_real(ctx, argv[1], argc > 1, 1.0,
            &sigma, "normal(mu, sigma): sigma"))
        return JS_EXCEPTION;
    if (sigma < 0.0)
        return JS_ThrowRangeError(ctx,
            "normal(mu, sigma): sigma must be >= 0");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    if (sigma == 0.0)
        return JS_NewFloat64(ctx, mu);
    return JS_NewFloat64(ctx, dyn_prng_normal(rng, mu, sigma));
}

static JSValue dyn_random_exponential(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double lambda = 1.0;

    if (dyn_random_arg_real(ctx, argv[0], argc > 0, 1.0,
            &lambda, "exponential(lambda): lambda"))
        return JS_EXCEPTION;
    if (!(lambda > 0.0))
        return JS_ThrowRangeError(ctx,
            "exponential(lambda): lambda must be a finite number > 0");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_exponential(rng, lambda));
}

static JSValue dyn_random_poisson(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double lambda;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "poisson(lambda): lambda is required");
    if (dyn_random_arg_num(ctx, argv[0], 1, 0.0,
            0.0, DYN_RANDOM_POISSON_MAX,
            &lambda, "poisson(lambda): lambda"))
        return JS_EXCEPTION;

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, dyn_prng_poisson(rng, lambda));
}

static JSValue dyn_random_gamma(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double shape, scale = 1.0;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "gamma(shape, scale): shape is required");
    if (dyn_random_arg_real(ctx, argv[0], 1, 0.0, &shape, "gamma(shape, scale): shape")
        || dyn_random_arg_real(ctx, argv[1], argc > 1, 1.0, &scale, "gamma(shape, scale): scale"))
        return JS_EXCEPTION;
    if (!(shape > 0.0))
        return JS_ThrowRangeError(ctx, "gamma(shape, scale): shape must be a finite number > 0");
    if (!(scale > 0.0))
        return JS_ThrowRangeError(ctx, "gamma(shape, scale): scale must be a finite number > 0");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_gamma(rng, shape, scale));
}

static JSValue dyn_random_beta(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double a, b;

    if (argc < 2 || JS_IsUndefined(argv[0]) || JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(ctx, "beta(a, b): a and b are required");
    if (dyn_random_arg_real(ctx, argv[0], 1, 0.0, &a, "beta(a, b): a")
        || dyn_random_arg_real(ctx, argv[1], 1, 0.0, &b, "beta(a, b): b"))
        return JS_EXCEPTION;
    if (!(a > 0.0) || !(b > 0.0))
        return JS_ThrowRangeError(ctx, "beta(a, b): a and b must be finite numbers > 0");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_beta(rng, a, b));
}

static JSValue dyn_random_binomial(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    double n, p;

    if (argc < 2 || JS_IsUndefined(argv[0]) || JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(ctx, "binomial(n, p): n and p are required");
    if (dyn_random_arg_int(ctx, argv[0], 1, 0.0, 0.0, DYN_RANDOM_POISSON_MAX, &n, "binomial(n, p): n")
        || dyn_random_arg_num(ctx, argv[1], 1, 0.0, 0.0, 1.0, &p, "binomial(n, p): p"))
        return JS_EXCEPTION;
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, dyn_prng_binomial(rng, (int64_t)n, p));
}

static JSValue dyn_random_jump(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    dyn_prng_jump(rng);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_random_long_jump(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    dyn_prng_long_jump(rng);
    return JS_DupValue(ctx, this_val);
}

_Atomic JSClassID dyn_random_cid_f32;

_Atomic JSClassID dyn_random_cid_f64;

static JSClassID dyn_random_capture_cid(JSContext* ctx, JSTypedArrayEnum type)
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

static void dyn_random_ab_free(JSRuntime* rt, void* opaque, void* ptr)
{
    (void)opaque;
    js_free_rt(rt, ptr);
}

static JSValue dyn_random_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue ab, out, ta_args[3];
    uint8_t* buf;
    double dn;
    size_t n;

    (void)argc;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "bytes(n): n is required");
    if (JS_ToFloat64(ctx, &dn, argv[0]))
        return JS_EXCEPTION;
    if (!(dn >= 0.0) || dn > DYN_RANDOM_BYTES_MAX || dn != floor(dn))
        return JS_ThrowRangeError(ctx,
            "bytes(n): n must be an integer in [0, 2^30] (1 GiB cap)");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    n = (size_t)dn;
    buf = (uint8_t*)js_malloc(ctx, n ? n : 1);
    if (!buf)
        return JS_EXCEPTION;
    dyn_prng_fill(rng, buf, n);
    ab = JS_NewArrayBuffer(ctx, buf, n, dyn_random_ab_free, NULL, 0);
    if (JS_IsException(ab)) {
        js_free(ctx, buf);
        return ab;
    }
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

int dyn_random_array_len(JSContext* ctx, JSValueConst arr, double* len)
{
    JSValue lv;
    double d;

    if (!JS_IsObject(arr)) {
        JS_ThrowTypeError(ctx, "expected an Array or a TypedArray");
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lv))
        return -1;
    if (JS_ToFloat64(ctx, &d, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (!(d >= 0.0) || d > 4294967295.0 || d != floor(d)) {
        JS_ThrowTypeError(ctx, "expected an Array or a TypedArray");
        return -1;
    }
    *len = d;
    return 0;
}

static JSValue dyn_random_elem_at(JSContext* ctx, JSValueConst arr,
    uint32_t pick)
{
    return JS_GetPropertyUint32(ctx, arr, pick);
}

static JSValue dyn_random_choice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue buf_val;
    size_t byte_off, byte_len, bpe;
    double len;
    uint64_t pick;
    int is_ta;

    (void)argc;
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        JS_FreeValue(ctx, buf_val);
        len = (double)(byte_len / bpe);
    } else if (dyn_random_array_len(ctx, argv[0], &len)) {
        return JS_EXCEPTION;
    }
    if (len < 1.0)
        return JS_ThrowRangeError(ctx, "choice: the array is empty");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    pick = dyn_prng_next_bounded(rng, (uint64_t)len);
    return dyn_random_elem_at(ctx, argv[0], (uint32_t)pick);
}

static const JSCFunctionListEntry dyn_random_proto[] = {
    JS_CFUNC_DEF("nextU64", 0, dyn_random_next_u64),
    JS_CFUNC_DEF("nextU53", 0, dyn_random_next_u53),
    JS_CFUNC_DEF("nextFloat", 0, dyn_random_next_float),
    JS_CFUNC_DEF("nextBounded", 1, dyn_random_next_bounded),
    JS_CFUNC_DEF("normal", 2, dyn_random_normal),
    JS_CFUNC_DEF("exponential", 1, dyn_random_exponential),
    JS_CFUNC_DEF("poisson", 1, dyn_random_poisson),
    JS_CFUNC_DEF("gamma", 2, dyn_random_gamma),
    JS_CFUNC_DEF("beta", 2, dyn_random_beta),
    JS_CFUNC_DEF("binomial", 2, dyn_random_binomial),
    JS_CFUNC_DEF("shuffle", 1, dyn_random_shuffle),
    JS_CFUNC_DEF("sample", 2, dyn_random_sample),
    JS_CFUNC_DEF("choice", 1, dyn_random_choice),
    JS_CFUNC_DEF("fill", 3, dyn_random_fill),
    JS_CFUNC_MAGIC_DEF("fillNormal", 3, dyn_random_fill_dist, DYN_FILL_NORMAL),
    JS_CFUNC_MAGIC_DEF("fillUniform", 3, dyn_random_fill_dist, DYN_FILL_UNIFORM),
    JS_CFUNC_MAGIC_DEF("fillExponential", 2, dyn_random_fill_dist, DYN_FILL_EXPONENTIAL),
    JS_CFUNC_DEF("bytes", 1, dyn_random_bytes),
    JS_CFUNC_DEF("jump", 0, dyn_random_jump),
    JS_CFUNC_DEF("longJump", 0, dyn_random_long_jump),
    JS_CFUNC_DEF("getState", 0, dyn_random_get_state),
    JS_CFUNC_DEF("setState", 1, dyn_random_set_state),
};

static int dyn_random_init_module(JSContext* ctx, JSModuleDef* m)
{
    dyn_random_cid_f32 = dyn_random_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT32);
    dyn_random_cid_f64 = dyn_random_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT64);
    if (dyn_register_plain_class(ctx, m, &dyn_random_class_id, &dyn_random_class,
            dyn_random_proto, countof(dyn_random_proto),
            dyn_random_ctor, "Random")
        < 0)
        return -1;
    return 0;
}

int js_nat_init_random(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:random", dyn_random_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Random");
    return 0;
}

#endif
