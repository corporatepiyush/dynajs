#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

typedef struct {
    dyn_dict_t* d;
} dyn_jsdict_t;

static void dyn_jsdict_dispose(void* native)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)native;
    if (!j)
        return;
    dyn_dict_free(j->d);
    free(j);
}

JSValue dyn_jsdict_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_jsdict_t* j;
    const uint8_t** phrases = NULL;
    size_t* lens = NULL;
    const char** owned = NULL;
    int64_t n = 0;
    int64_t i;
    int n_owned = 0;
    JSValue ret = JS_EXCEPTION;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new Dictionary(phrases[])");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n < 1)
        return JS_ThrowRangeError(ctx, "at least one phrase is required");
    if (n > DYN_DICT_MAX_PHRASES)
        return JS_ThrowRangeError(ctx, "at most %d phrases", DYN_DICT_MAX_PHRASES);

    phrases = (const uint8_t**)calloc((size_t)n, sizeof(*phrases));
    lens = (size_t*)calloc((size_t)n, sizeof(*lens));
    owned = (const char**)calloc((size_t)n, sizeof(*owned));
    if (!phrases || !lens || !owned) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t sl;
        const char* cs;
        if (JS_IsException(e))
            goto done;
        cs = JS_ToCStringLen(ctx, &sl, e);
        JS_FreeValue(ctx, e);
        if (!cs)
            goto done;
        owned[n_owned++] = cs;
        if (sl == 0) {
            JS_ThrowRangeError(ctx, "an empty phrase matches everywhere and "
                                    "encodes nothing; it is not a phrase");
            goto done;
        }
        phrases[i] = (const uint8_t*)cs;
        lens[i] = sl;
    }

    j = (dyn_jsdict_t*)calloc(1, sizeof(*j));
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    j->d = dyn_dict_new(phrases, lens, (size_t)n);
    if (!j->d) {
        free(j);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = dyn_res_wrap(ctx, new_target, dyn_jsdict_class_id, j, dyn_jsdict_dispose);

done:
    for (i = 0; i < n_owned; i++)
        JS_FreeCString(ctx, owned[i]);
    free(owned);
    free(phrases);
    free(lens);
    return ret;
}

JSValue dyn_jsdict_run(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_jsdict_t* j;
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    JSValue result;
    int rc;

    if (magic) {
        static const char* const dict_dec_keys[] = { "maxOutputBytes" };
        if (argc > 1 && JS_IsObject(argv[1])
            && dyn_opts_strict(ctx, argv[1], dict_dec_keys, 1) < 0)
            return JS_EXCEPTION;
        if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
            return JS_EXCEPTION;
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val, dyn_jsdict_class_id);
    if (!j) {
        free(src);
        return JS_EXCEPTION;
    }
    rc = magic ? dyn_dict_decompress(j->d, src, src_len, &o)
               : dyn_dict_compress(j->d, src, src_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        if (magic && o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "Dictionary.decompress");
        return magic
            ? JS_ThrowTypeError(ctx, "not a record from this dictionary, or "
                                     "the record is malformed")
            : JS_ThrowOutOfMemory(ctx);
    }
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

JSValue dyn_jsdict_get_id(JSContext* ctx, JSValueConst this_val)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val,
        dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_dict_id(j->d));
}

JSValue dyn_jsdict_get_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_jsdict_t* j = (dyn_jsdict_t*)dyn_res_native(ctx, this_val,
        dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_dict_count(j->d));
}

#endif
