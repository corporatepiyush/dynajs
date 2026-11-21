#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

static int dyn_random_arr_swap(JSContext* ctx, JSValueConst arr,
    uint32_t i, uint32_t j)
{
    JSAtom ai = JS_NewAtomUInt32(ctx, i);
    JSAtom aj = JS_NewAtomUInt32(ctx, j);
    JSPropertyDescriptor di, dj;
    int has_i, has_j, r = -1;

    if (ai == JS_ATOM_NULL || aj == JS_ATOM_NULL)
        goto out;
    has_i = JS_GetOwnProperty(ctx, &di, arr, ai);
    if (has_i < 0)
        goto out;
    has_j = JS_GetOwnProperty(ctx, &dj, arr, aj);
    if (has_j < 0) {
        if (has_i) {
            JS_FreeValue(ctx, di.value);
            JS_FreeValue(ctx, di.getter);
            JS_FreeValue(ctx, di.setter);
        }
        goto out;
    }
    if (has_i) {
        JS_FreeValue(ctx, di.value);
        JS_FreeValue(ctx, di.getter);
        JS_FreeValue(ctx, di.setter);
    }
    if (has_j) {
        JS_FreeValue(ctx, dj.value);
        JS_FreeValue(ctx, dj.getter);
        JS_FreeValue(ctx, dj.setter);
    }

    if (has_i && has_j) {
        JSValue vi = JS_GetPropertyUint32(ctx, arr, i);
        JSValue vj;
        if (JS_IsException(vi))
            goto out;
        vj = JS_GetPropertyUint32(ctx, arr, j);
        if (JS_IsException(vj)) {
            JS_FreeValue(ctx, vi);
            goto out;
        }
        r = JS_SetPropertyUint32(ctx, arr, j, vi);
        if (r < 0) {
            JS_FreeValue(ctx, vj);
            goto out;
        }
        r = JS_SetPropertyUint32(ctx, arr, i, vj);
    } else if (has_i) {
        JSValue vi = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(vi))
            goto out;
        r = JS_SetPropertyUint32(ctx, arr, j, vi);
        if (r < 0)
            goto out;
        r = JS_DeleteProperty(ctx, arr, ai, 0);
    } else if (has_j) {
        JSValue vj = JS_GetPropertyUint32(ctx, arr, j);
        if (JS_IsException(vj))
            goto out;
        r = JS_SetPropertyUint32(ctx, arr, i, vj);
        if (r < 0)
            goto out;
        r = JS_DeleteProperty(ctx, arr, aj, 0);
    } else {
        r = 0;
    }
out:
    if (ai != JS_ATOM_NULL)
        JS_FreeAtom(ctx, ai);
    if (aj != JS_ATOM_NULL)
        JS_FreeAtom(ctx, aj);
    return r < 0 ? -1 : 0;
}

static int dyn_random_fy_array(JSContext* ctx, JSValueConst arr,
    dyn_prng_t* rng, uint32_t len)
{
    JSValue* vals;
    uint32_t i, count;
    if (JS_GetFastArray(ctx, arr, &vals, &count) && count == len) {
        for (i = len - 1; i >= 1; i--) {
            uint32_t j = (uint32_t)dyn_prng_next_bounded(rng, (uint64_t)i + 1);
            JSValue t = vals[i];
            vals[i] = vals[j];
            vals[j] = t;
        }
        return 0;
    }
    for (i = len - 1; i >= 1; i--) {
        uint32_t j = (uint32_t)dyn_prng_next_bounded(rng, (uint64_t)i + 1);
        if (i == j)
            continue;
        if (dyn_random_arr_swap(ctx, arr, i, j))
            return -1;
    }
    return 0;
}

#define DYN_RANDOM_FY_FIXED(S)                              \
    do {                                                    \
        uint8_t* p = base + byte_off;                       \
        for (i = len - 1; i >= 1; i--) {                    \
            uint64_t j = dyn_prng_next_bounded(rng, i + 1); \
            uint8_t t[S];                                   \
            if (i == j)                                     \
                continue;                                   \
            memcpy(t, p + i * (S), (S));                    \
            memcpy(p + i * (S), p + j * (S), (S));          \
            memcpy(p + j * (S), t, (S));                    \
        }                                                   \
        return;                                             \
    } while (0)

static void dyn_random_fy_ta(dyn_prng_t* rng, uint8_t* base,
    size_t byte_off, size_t bpe, uint64_t len)
{
    uint8_t tmp[8];
    uint64_t i;
    if (len < 2)
        return;
    switch (bpe) {
    case 1:
        DYN_RANDOM_FY_FIXED(1);
    case 2:
        DYN_RANDOM_FY_FIXED(2);
    case 4:
        DYN_RANDOM_FY_FIXED(4);
    case 8:
        DYN_RANDOM_FY_FIXED(8);
    default:
        break;
    }
    for (i = len - 1; i >= 1; i--) {
        uint64_t j = dyn_prng_next_bounded(rng, i + 1);
        if (i == j)
            continue;
        memcpy(tmp, base + byte_off + i * bpe, bpe);
        memmove(base + byte_off + i * bpe, base + byte_off + j * bpe, bpe);
        memcpy(base + byte_off + j * bpe, tmp, bpe);
    }
}

JSValue dyn_random_shuffle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue buf_val;
    size_t byte_off, byte_len, bpe;
    double len;
    int is_ta;

    (void)argc;
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;

    if (JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        uint8_t* base;
        size_t ab_size;
        if (!JS_IsExtensible(ctx, argv[0])) {
            JS_FreeValue(ctx, buf_val);
            return JS_ThrowTypeError(ctx,
                "shuffle: refusing a non-extensible typed array (frozen or sealed)");
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
        if (!base) {
            JS_FreeValue(ctx, buf_val);
            return JS_ThrowTypeError(ctx, "typed array buffer is detached");
        }
        if (byte_off > ab_size || byte_len > ab_size - byte_off) {
            JS_FreeValue(ctx, buf_val);
            return JS_ThrowRangeError(ctx, "typed array out of bounds");
        }
        dyn_random_fy_ta(rng, base, byte_off, bpe, byte_len / bpe);
        JS_FreeValue(ctx, buf_val);
    } else {
        if (dyn_random_array_len(ctx, argv[0], &len))
            return JS_EXCEPTION;
        if (!JS_IsExtensible(ctx, argv[0]))
            return JS_ThrowTypeError(ctx,
                "shuffle: refusing a non-extensible array (frozen or sealed)");
        if (len >= 2.0 && dyn_random_fy_array(ctx, argv[0], rng, (uint32_t)len))
            return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, this_val);
}

#endif
