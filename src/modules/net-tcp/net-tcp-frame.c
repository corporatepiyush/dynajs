#include "net-tcp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

enum { SPEC_LENGTH = 1, SPEC_DELIM, SPEC_FIXED };

#define SPEC_MAX_DEFAULT (1u << 20)
#define SPEC_2P53 9007199254740992.0

struct dyn_frame_spec {
    int refs;
    int kind;
    dyn_frame_length_cfg len;
    uint8_t** delims;
    size_t* lens;
    size_t nd;
    int strip_delimiter;
    uint64_t max_frame;
    uint64_t fixed;
    int fail_fast;
    int stop_on_error;
    int hold_overlapping;
    int resume_after_error;
};

JSClassID dyn_frame_spec_class_id;

static const char* const lf_keys[] = { "offset", "width", "endian", "adjustment", "initialBytesToStrip", "maxFrameLength", "failFast" };
static const char* const dl_keys[] = { "delimiter", "stripDelimiter", "maxFrameLength", "failFast" };
static const char* const lf_meta_keys[] = { "stopOnError", "rejectUnsafeLength", "resumeAfterError" };
static const char* const dl_meta_keys[] = { "stopOnError", "holdOverlapping", "resumeAfterError" };

void frame_spec_unref(dyn_frame_spec_t* s)
{
    size_t i;
    if (!s || --s->refs > 0)
        return;
    for (i = 0; i < s->nd; i++)
        free(s->delims[i]);
    free(s->delims);
    free(s->lens);
    free(s);
}

dyn_frame_spec_t* frame_spec_ref(dyn_frame_spec_t* s)
{
    if (s)
        s->refs++;
    return s;
}

static void frame_spec_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    frame_spec_unref((dyn_frame_spec_t*)JS_GetOpaque(val, dyn_frame_spec_class_id));
}

static const JSClassDef frame_spec_class = {
    "FrameDecoder",
    .finalizer = frame_spec_finalizer,
};

dyn_frame_spec_t* frame_spec_from(JSContext* ctx, JSValueConst v, const char* fn)
{
    dyn_frame_spec_t* s = JS_IsObject(v) ? (dyn_frame_spec_t*)JS_GetOpaque(v, dyn_frame_spec_class_id) : NULL;
    if (!s)
        JS_ThrowTypeError(ctx, "%s: decoder must be a FrameDecoder from LengthFieldDecoder, DelimiterDecoder or FixedLengthDecoder", fn);
    return s;
}

dyn_frame_t* frame_spec_instantiate(const dyn_frame_spec_t* s)
{
    int err = 0;
    if (s->kind == SPEC_LENGTH)
        return dyn_frame_new_length(&s->len, &err);
    if (s->kind == SPEC_DELIM) {
        dyn_frame_delim_cfg c;
        c.delims = (const uint8_t* const*)s->delims;
        c.lens = s->lens;
        c.count = s->nd;
        c.strip_delimiter = s->strip_delimiter;
        c.max_frame = s->max_frame;
        c.fail_fast = s->fail_fast;
        c.stop_on_error = s->stop_on_error;
        c.hold_overlapping = s->hold_overlapping;
        c.resume_after_error = s->resume_after_error;
        return dyn_frame_new_delim(&c, &err);
    }
    return dyn_frame_new_fixed(s->fixed, &err);
}

static JSValue frame_spec_wrap(JSContext* ctx, dyn_frame_spec_t* s)
{
    JSValue o = JS_NewObjectClass(ctx, (int)dyn_frame_spec_class_id);
    if (JS_IsException(o)) {
        frame_spec_unref(s);
        return o;
    }
    JS_SetOpaque(o, s);
    return o;
}

static int spec_int(JSContext* ctx, JSValueConst o, const char* name, const char* fn,
    double lo, double hi, int64_t* out)
{
    JSValue v;
    double d;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, name);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a number", fn, name);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || d != (double)(int64_t)d) {
        JS_ThrowRangeError(ctx, "%s: %s must be an integer in %.0f..%.0f", fn, name, lo, hi);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static JSValue dyn_length_field_decoder(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "LengthFieldDecoder";
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED, m = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t offset = 0, width = 4, adj = 0, strip = 0, max = SPEC_MAX_DEFAULT;
    int little = 0, fail_fast = 1, stop = 0, reject = 0, resume = 0;
    dyn_frame_spec_t* s;
    dyn_frame_t* probe;
    (void)this_val;
    if (dyn_opts_bag(ctx, argc, argv, 0, lf_keys, (int)countof(lf_keys), fn) < 0
        || dyn_opts_bag(ctx, argc, argv, 1, lf_meta_keys, (int)countof(lf_meta_keys), fn) < 0
        || dyn_opt_bool(ctx, o, "failFast", fn, &fail_fast) < 0
        || dyn_opt_bool(ctx, m, "stopOnError", fn, &stop) < 0
        || dyn_opt_bool(ctx, m, "rejectUnsafeLength", fn, &reject) < 0
        || dyn_opt_bool(ctx, m, "resumeAfterError", fn, &resume) < 0)
        return JS_EXCEPTION;
    if (spec_int(ctx, o, "offset", fn, 0, SPEC_2P53, &offset) < 0
        || spec_int(ctx, o, "width", fn, 1, 8, &width) < 0
        || spec_int(ctx, o, "adjustment", fn, -SPEC_2P53, SPEC_2P53, &adj) < 0
        || spec_int(ctx, o, "initialBytesToStrip", fn, 0, SPEC_2P53, &strip) < 0
        || spec_int(ctx, o, "maxFrameLength", fn, 1, SPEC_2P53, &max) < 0)
        return JS_EXCEPTION;
    if (width != 1 && width != 2 && width != 3 && width != 4 && width != 8)
        return JS_ThrowRangeError(ctx, "%s: width must be 1, 2, 3, 4 or 8", fn);
    if (JS_IsObject(o)) {
        JSValue e = JS_GetPropertyStr(ctx, o, "endian");
        if (JS_IsException(e))
            return e;
        if (!JS_IsUndefined(e)) {
            const char* es = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
            int ok = es && (strcmp(es, "be") == 0 || strcmp(es, "le") == 0);
            little = ok && strcmp(es, "le") == 0;
            if (es)
                JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (!ok)
                return JS_ThrowTypeError(ctx, "%s: endian must be \"be\" or \"le\"", fn);
        }
    }
    s = (dyn_frame_spec_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->refs = 1;
    s->kind = SPEC_LENGTH;
    s->len.offset = (uint64_t)offset;
    s->len.width = (int)width;
    s->len.little_endian = little;
    s->len.adjustment = adj;
    s->len.strip = (uint64_t)strip;
    s->len.max_frame = (uint64_t)max;
    s->len.fail_fast = fail_fast;
    s->len.stop_on_error = stop;
    s->len.reject_unsafe = reject;
    s->len.resume_after_error = resume;
    probe = frame_spec_instantiate(s);
    if (!probe) {
        frame_spec_unref(s);
        return JS_ThrowRangeError(ctx, "%s: offset + width must not exceed maxFrameLength", fn);
    }
    dyn_frame_free(probe);
    return frame_spec_wrap(ctx, s);
}

static int spec_bytes(JSContext* ctx, JSValueConst v, uint8_t** out, size_t* outlen)
{
    size_t len = 0, off = 0, bpe = 0;
    const uint8_t* p = NULL;
    const char* str = NULL;
    JSValue ab = JS_UNDEFINED;
    if (JS_IsString(v)) {
        str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        p = (const uint8_t*)str;
    } else if (JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW) {
        size_t total = 0;
        uint8_t* base;
        ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (JS_IsException(ab))
            return -1;
        base = JS_GetArrayBuffer(ctx, &total, ab);
        if (!base) {
            JS_FreeValue(ctx, ab);
            return -1;
        }
        p = base + off;
    } else if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
        p = JS_GetArrayBuffer(ctx, &len, v);
        if (!p)
            return -1;
    } else {
        return 1;
    }
    *out = NULL;
    *outlen = len;
    if (len > 0 && len <= DYN_FRAME_MAX_DELIM_LEN) {
        *out = (uint8_t*)malloc(len);
        if (*out)
            memcpy(*out, p, len);
    }
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, ab);
    if (len == 0 || len > DYN_FRAME_MAX_DELIM_LEN)
        return 2;
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int spec_add_delim(JSContext* ctx, dyn_frame_spec_t* s, JSValueConst v, const char* fn)
{
    uint8_t* b = NULL;
    size_t n = 0;
    int r;
    if (s->nd >= DYN_FRAME_MAX_DELIMS) {
        JS_ThrowRangeError(ctx, "%s: at most %d delimiters", fn, DYN_FRAME_MAX_DELIMS);
        return -1;
    }
    r = spec_bytes(ctx, v, &b, &n);
    if (r < 0)
        return -1;
    if (r == 1) {
        JS_ThrowTypeError(ctx, "%s: delimiter must be a string, a byte view, an ArrayBuffer or an array of them", fn);
        return -1;
    }
    if (r == 2) {
        JS_ThrowRangeError(ctx, "%s: a delimiter must be 1..%d bytes", fn, DYN_FRAME_MAX_DELIM_LEN);
        return -1;
    }
    s->delims[s->nd] = b;
    s->lens[s->nd] = n;
    s->nd++;
    return 0;
}

static JSValue dyn_delimiter_decoder(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char fn[] = "DelimiterDecoder";
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED, m = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue dv, sv;
    int64_t max = SPEC_MAX_DEFAULT;
    int fail_fast = 1, stop = 0, hold = 0, resume = 0;
    dyn_frame_spec_t* s;
    (void)this_val;
    if (!JS_IsObject(o) || JS_IsFunction(ctx, o))
        return JS_ThrowTypeError(ctx, "%s: options must be an object with a delimiter", fn);
    if (dyn_opts_strict(ctx, o, dl_keys, (int)countof(dl_keys)) < 0
        || dyn_opts_bag(ctx, argc, argv, 1, dl_meta_keys, (int)countof(dl_meta_keys), fn) < 0
        || dyn_opt_bool(ctx, o, "failFast", fn, &fail_fast) < 0
        || dyn_opt_bool(ctx, m, "stopOnError", fn, &stop) < 0
        || dyn_opt_bool(ctx, m, "holdOverlapping", fn, &hold) < 0
        || dyn_opt_bool(ctx, m, "resumeAfterError", fn, &resume) < 0)
        return JS_EXCEPTION;
    if (spec_int(ctx, o, "maxFrameLength", fn, 1, SPEC_2P53, &max) < 0)
        return JS_EXCEPTION;
    s = (dyn_frame_spec_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->refs = 1;
    s->kind = SPEC_DELIM;
    s->max_frame = (uint64_t)max;
    s->strip_delimiter = 1;
    s->fail_fast = fail_fast;
    s->stop_on_error = stop;
    s->hold_overlapping = hold;
    s->resume_after_error = resume;
    s->delims = (uint8_t**)calloc(DYN_FRAME_MAX_DELIMS, sizeof(*s->delims));
    s->lens = (size_t*)calloc(DYN_FRAME_MAX_DELIMS, sizeof(*s->lens));
    if (!s->delims || !s->lens) {
        frame_spec_unref(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    sv = JS_GetPropertyStr(ctx, o, "stripDelimiter");
    if (JS_IsException(sv)) {
        frame_spec_unref(s);
        return sv;
    }
    if (!JS_IsUndefined(sv)) {
        if (!JS_IsBool(sv)) {
            JS_FreeValue(ctx, sv);
            frame_spec_unref(s);
            return JS_ThrowTypeError(ctx, "%s: stripDelimiter must be a boolean", fn);
        }
        s->strip_delimiter = JS_ToBool(ctx, sv);
    }
    dv = JS_GetPropertyStr(ctx, o, "delimiter");
    if (JS_IsException(dv)) {
        frame_spec_unref(s);
        return dv;
    }
    if (JS_IsUndefined(dv)) {
        frame_spec_unref(s);
        return JS_ThrowTypeError(ctx, "%s: delimiter is required", fn);
    }
    if (JS_IsArray(ctx, dv)) {
        int64_t n = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, dv, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
            JS_FreeValue(ctx, lv);
            goto fail;
        }
        JS_FreeValue(ctx, lv);
        if (n < 1 || n > DYN_FRAME_MAX_DELIMS) {
            JS_ThrowRangeError(ctx, "%s: delimiter array must hold 1..%d entries", fn, DYN_FRAME_MAX_DELIMS);
            goto fail;
        }
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, dv, (uint32_t)i);
            int r;
            if (JS_IsException(e))
                goto fail;
            r = spec_add_delim(ctx, s, e, fn);
            JS_FreeValue(ctx, e);
            if (r < 0)
                goto fail;
        }
    } else if (spec_add_delim(ctx, s, dv, fn) < 0) {
        goto fail;
    }
    JS_FreeValue(ctx, dv);
    return frame_spec_wrap(ctx, s);
fail:
    JS_FreeValue(ctx, dv);
    frame_spec_unref(s);
    return JS_EXCEPTION;
}

static JSValue dyn_fixed_length_decoder(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double d;
    dyn_frame_spec_t* s;
    (void)this_val;
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "FixedLengthDecoder: length must be a number");
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    if (!(d >= 1 && d <= SPEC_2P53) || d != (double)(int64_t)d)
        return JS_ThrowRangeError(ctx, "FixedLengthDecoder: length must be an integer in 1..9007199254740992");
    s = (dyn_frame_spec_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->refs = 1;
    s->kind = SPEC_FIXED;
    s->fixed = (uint64_t)d;
    s->max_frame = s->fixed;
    return frame_spec_wrap(ctx, s);
}

static JSValue frame_spec_kind(JSContext* ctx, JSValueConst this_val)
{
    dyn_frame_spec_t* s = (dyn_frame_spec_t*)JS_GetOpaque2(ctx, this_val, dyn_frame_spec_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewString(ctx, s->kind == SPEC_LENGTH ? "length" : s->kind == SPEC_DELIM ? "delimiter" : "fixed");
}

static JSValue frame_spec_max(JSContext* ctx, JSValueConst this_val)
{
    dyn_frame_spec_t* s = (dyn_frame_spec_t*)JS_GetOpaque2(ctx, this_val, dyn_frame_spec_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, (double)(s->kind == SPEC_LENGTH ? s->len.max_frame : s->max_frame));
}

static const JSCFunctionListEntry frame_spec_proto[] = {
    JS_CGETSET_DEF("kind", frame_spec_kind, NULL),
    JS_CGETSET_DEF("maxFrameLength", frame_spec_max, NULL),
};

const JSCFunctionListEntry dyn_frame_funcs[3] = {
    JS_CFUNC_DEF("LengthFieldDecoder", 2, dyn_length_field_decoder),
    JS_CFUNC_DEF("DelimiterDecoder", 2, dyn_delimiter_decoder),
    JS_CFUNC_DEF("FixedLengthDecoder", 1, dyn_fixed_length_decoder),
};

int dyn_frame_spec_register(JSContext* ctx)
{
    JSValue cp;
    JS_NewClassID(&dyn_frame_spec_class_id);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), dyn_frame_spec_class_id)
        && JS_NewClass(JS_GetRuntime(ctx), dyn_frame_spec_class_id, &frame_spec_class) < 0)
        return -1;
    cp = JS_NewObject(ctx);
    if (JS_IsException(cp))
        return -1;
    JS_SetPropertyFunctionList(ctx, cp, frame_spec_proto, countof(frame_spec_proto));
    JS_SetClassProto(ctx, dyn_frame_spec_class_id, cp);
    return 0;
}

#endif
