#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

JSValue dyn_random_get_state(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    uint8_t raw[32];
    JSValue ab, out;
    JSValueConst ta_args[3];
    int i, j;
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    for (i = 0; i < 4; i++) {
        uint64_t w = rng->s[i];
        for (j = 0; j < 8; j++)
            raw[i * 8 + j] = (uint8_t)(w >> (8 * j));
    }
    ab = JS_NewArrayBufferCopy(ctx, raw, sizeof raw);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

JSValue dyn_random_set_state(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue buf_val;
    uint8_t* base;
    size_t byte_off, byte_len, ab_size;
    uint64_t w[4];
    int i, j, all_zero = 1;

    (void)argc;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "setState(state): a 32-byte Uint8Array from getState() is required");
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, NULL);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowTypeError(ctx, "typed array buffer is detached");
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off || byte_len != 32) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx,
            "setState(state): the state is exactly 32 bytes (four little-endian u64 words)");
    }
    for (i = 0; i < 4; i++) {
        w[i] = 0;
        for (j = 7; j >= 0; j--)
            w[i] = (w[i] << 8) | base[byte_off + i * 8 + j];
        if (w[i])
            all_zero = 0;
    }
    JS_FreeValue(ctx, buf_val);
    if (all_zero)
        return JS_ThrowRangeError(ctx,
            "setState(state): the all-zero state is xoshiro256**'s fixed point and would emit zeros forever");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    for (i = 0; i < 4; i++)
        rng->s[i] = w[i];
    return JS_UNDEFINED;
}

#endif
