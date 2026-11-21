#include "libc-internal.h"
#include "core/dyn-textdec.h"

typedef struct {
    uint8_t brand;
} JSTextEncoder;

#define JS_TEXTDECODER_WHOLE_MIN 32

typedef struct {
    BOOL fatal;
    BOOL ignore_bom;
    int enc;
    dyn_textdec_t ld;
    JSUtf8DecodeState st;
    BOOL stream_active;
    BOOL bom_handled;
} JSTextDecoder;

static JSClassID js_textencoder_class_id;

static JSClassID js_textdecoder_class_id;

static void js_utf8_decode_state_init(JSUtf8DecodeState* st)
{
    memset(st, 0, sizeof(*st));
    st->lower = 0x80;
    st->upper = 0xBF;
}

static const uint8_t utf8_replacement[3] = { 0xEF, 0xBF, 0xBD };

static const uint8_t utf8_bom[3] = { 0xEF, 0xBB, 0xBF };

static void js_textencoder_finalizer(JSRuntime* rt, JSValue val)
{
    JSTextEncoder* te = JS_GetOpaque(val, js_textencoder_class_id);
    if (te)
        js_free_rt(rt, te);
}

static void js_textdecoder_finalizer(JSRuntime* rt, JSValue val)
{
    JSTextDecoder* td = JS_GetOpaque(val, js_textdecoder_class_id);
    if (td)
        js_free_rt(rt, td);
}

static JSClassDef js_textencoder_class = {
    "TextEncoder",
    .finalizer = js_textencoder_finalizer,
};

static JSClassDef js_textdecoder_class = {
    "TextDecoder",
    .finalizer = js_textdecoder_finalizer,
};

static int js_textcodec_get_bytes(JSContext* ctx, JSValueConst obj,
    uint8_t** pbuf, size_t* plen)
{
    JSValue buffer;
    uint8_t *ptr, *out;
    size_t offset, length, abuf_size;

    *pbuf = NULL;
    *plen = 0;

    if ((JS_GetBufferKind(obj) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &abuf_size, obj) : NULL)) {
        buffer = JS_DupValue(ctx, obj);
        offset = 0;
        length = abuf_size;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buffer = JS_GetBufferKind(obj) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, obj, &offset, &length, NULL) : JS_EXCEPTION;
        if (JS_IsException(buffer)) {
            JSValue v;
            int64_t v_off, v_len;
            JS_FreeValue(ctx, JS_GetException(ctx));
            buffer = JS_GetPropertyStr(ctx, obj, "buffer");
            if (JS_IsException(buffer))
                return -1;
            if (!JS_GetArrayBuffer(ctx, &abuf_size, buffer)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, buffer);
                JS_ThrowTypeError(ctx, "decode() argument is not a BufferSource");
                return -1;
            }
            v = JS_GetPropertyStr(ctx, obj, "byteOffset");
            if (JS_ToInt64(ctx, &v_off, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, buffer);
                return -1;
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, obj, "byteLength");
            if (JS_ToInt64(ctx, &v_len, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, buffer);
                return -1;
            }
            JS_FreeValue(ctx, v);
            offset = (v_off > 0) ? (size_t)v_off : 0;
            length = (v_len > 0) ? (size_t)v_len : 0;
        }
    }

    ptr = JS_GetArrayBuffer(ctx, &abuf_size, buffer);
    if (!ptr) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    if (offset > abuf_size || length > abuf_size - offset) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowRangeError(ctx, "buffer source out of bounds");
        return -1;
    }
    out = js_malloc(ctx, length + 1);
    if (!out) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    memcpy(out, ptr + offset, length);
    JS_FreeValue(ctx, buffer);
    *pbuf = out;
    *plen = length;
    return 0;
}

static int js_textcodec_check_options(JSContext* ctx, JSValueConst opts,
    const char* const* valid,
    const char* valid_desc)
{
    JSPropertyEnum* props;
    uint32_t nprops, i;
    int j, ret = 0;

    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < nprops; i++) {
        const char* key = JS_AtomToCString(ctx, props[i].atom);
        if (!key) {
            ret = -1;
            break;
        }
        for (j = 0; valid[j]; j++) {
            if (!strcmp(key, valid[j]))
                break;
        }
        if (!valid[j]) {
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                key, valid_desc);
            ret = -1;
        }
        JS_FreeCString(ctx, key);
        if (ret)
            break;
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return ret;
}

static int js_utf8_decode_whatwg(JSContext* ctx, DynBuf* dbuf,
    const uint8_t* buf, size_t len, BOOL fatal,
    JSUtf8DecodeState* st, BOOL final)
{
    size_t i = 0;
    uint32_t codepoint = st->codepoint;
    int bytes_needed = st->bytes_needed, bytes_seen = st->bytes_seen;
    uint8_t lower = st->lower, upper = st->upper;
    uint8_t cbuf[UTF8_CHAR_LEN_MAX];

    while (i < len) {
        uint8_t b = buf[i];
        if (bytes_needed == 0) {
            if (b <= 0x7F) {
                dbuf_putc(dbuf, b);
                i++;
            } else if (b >= 0xC2 && b <= 0xDF) {
                bytes_needed = 1;
                codepoint = b & 0x1F;
                i++;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0)
                    lower = 0xA0;
                if (b == 0xED)
                    upper = 0x9F;
                bytes_needed = 2;
                codepoint = b & 0x0F;
                i++;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0)
                    lower = 0x90;
                if (b == 0xF4)
                    upper = 0x8F;
                bytes_needed = 3;
                codepoint = b & 0x07;
                i++;
            } else {
                if (fatal)
                    goto fatal_error;
                dbuf_put(dbuf, utf8_replacement, 3);
                i++;
            }
        } else if (b < lower || b > upper) {
            codepoint = 0;
            bytes_needed = 0;
            bytes_seen = 0;
            lower = 0x80;
            upper = 0xBF;
            if (fatal)
                goto fatal_error;
            dbuf_put(dbuf, utf8_replacement, 3);
        } else {
            lower = 0x80;
            upper = 0xBF;
            codepoint = (codepoint << 6) | (b & 0x3F);
            bytes_seen++;
            i++;
            if (bytes_seen == bytes_needed) {
                dbuf_put(dbuf, cbuf, unicode_to_utf8(cbuf, codepoint));
                codepoint = 0;
                bytes_needed = 0;
                bytes_seen = 0;
            }
        }
    }
    if (bytes_needed != 0) {
        if (final) {
            if (fatal)
                goto fatal_error;
            dbuf_put(dbuf, utf8_replacement, 3);
            bytes_needed = 0;
            bytes_seen = 0;
            codepoint = 0;
            lower = 0x80;
            upper = 0xBF;
        }
    }
    st->bytes_needed = bytes_needed;
    st->bytes_seen = bytes_seen;
    st->codepoint = codepoint;
    st->lower = lower;
    st->upper = upper;
    if (dbuf->error) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
fatal_error:
    JS_ThrowTypeError(ctx, "invalid UTF-8 sequence");
    return -1;
}

JSValue dyn_utf8_decode_bytes(JSContext* ctx, const uint8_t* p, size_t n, int strip_bom)
{
    JSUtf8DecodeState st;
    DynBuf dbuf;
    JSValue ret;
    if (strip_bom && n >= 3 && !memcmp(p, utf8_bom, 3)) {
        p += 3;
        n -= 3;
    }
    if (simd.validate_utf8(p, n) == n)
        return JS_NewStringLen(ctx, (const char*)p, n);
    js_utf8_decode_state_init(&st);
    js_std_dbuf_init(ctx, &dbuf);
    if (js_utf8_decode_whatwg(ctx, &dbuf, p, n, FALSE, &st, TRUE)) {
        dbuf_free(&dbuf);
        return JS_EXCEPTION;
    }
    ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return ret;
}

static JSValue js_textencoder_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextEncoder* te;

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_textencoder_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_textencoder_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    te = js_mallocz(ctx, sizeof(*te));
    if (!te)
        goto fail;
    JS_SetOpaque(obj, te);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_textencoder_get_encoding(JSContext* ctx, JSValueConst this_val)
{
    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    return JS_NewString(ctx, "utf-8");
}

static char* dyn_wtf8_lone_surrogates_to_fffd(const char* s, size_t n, size_t* pn)
{
    size_t i = 0;
    int found = 0;
    char* out;

    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c == 0xED && i + 2 < n && (uint8_t)s[i + 1] >= 0xA0 && (uint8_t)s[i + 1] <= 0xBF
            && ((uint8_t)s[i + 2] & 0xC0) == 0x80) {
            found = 1;
            break;
        }
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2
            : (c < 0xF0)                 ? 3
                                         : 4;
    }
    if (!found)
        return NULL;
    out = (char*)malloc(n ? n : 1);
    if (!out)
        return NULL;
    memcpy(out, s, n);
    i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)out[i];
        if (c == 0xED && i + 2 < n && (uint8_t)out[i + 1] >= 0xA0 && (uint8_t)out[i + 1] <= 0xBF
            && ((uint8_t)out[i + 2] & 0xC0) == 0x80) {
            out[i] = (char)0xEF;
            out[i + 1] = (char)0xBF;
            out[i + 2] = (char)0xBD;
            i += 3;
            continue;
        }
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2
            : (c < 0xF0)                 ? 3
                                         : 4;
    }
    *pn = n;
    return out;
}

static JSValue js_textencoder_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str = NULL;
    size_t len = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str)
            return JS_EXCEPTION;
    }
    {
        size_t flen = len;
        char* fixed = str && memchr(str, 0xED, len) ? dyn_wtf8_lone_surrogates_to_fffd(str, len, &flen) : NULL;
        ab = JS_NewArrayBufferCopy(ctx,
            (const uint8_t*)(fixed ? fixed : (str ? str : "")),
            fixed ? flen : len);
        if (fixed)
            free(fixed);
        if (str)
            JS_FreeCString(ctx, str);
    }
    if (JS_IsException(ab))
        return JS_EXCEPTION;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static JSValue js_textencoder_encodeInto(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* src;
    size_t src_len, offset, dst_len, abuf_size, bpe, si = 0, di = 0;
    int64_t read = 0, written = 0;
    uint8_t *abuf, *dst;
    JSValue buffer, res;

    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &src_len, argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (!src)
        return JS_EXCEPTION;
    buffer = JS_GetTypedArrayBuffer(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED,
        &offset, &dst_len, &bpe);
    if (JS_IsException(buffer)) {
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_ThrowTypeError(ctx, "encodeInto() destination must be a Uint8Array");
    }
    if (JS_CheckWritableBuffer(ctx, argv[1]) < 0) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    abuf = JS_GetArrayBuffer(ctx, &abuf_size, buffer);
    if (!abuf) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    if (offset > abuf_size || dst_len > abuf_size - offset) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_ThrowRangeError(ctx, "destination out of bounds");
    }
    dst = abuf + offset;
    while (si < src_len) {
        const uint8_t* p = (const uint8_t*)src + si;
        size_t run = 0, room = dst_len - di, lim = src_len - si < room ? src_len - si : room;
        while (run + 8 <= lim) {
            uint64_t w;
            memcpy(&w, p + run, 8);
            if (w & 0x8080808080808080ULL)
                break;
            run += 8;
        }
        while (run < lim && p[run] < 0x80)
            run++;
        if (run) {
            memcpy(dst + di, p, run);
            di += run;
            si += run;
            written += (int64_t)run;
            read += (int64_t)run;
            continue;
        }
        if (di >= dst_len)
            break;
        const uint8_t* pnext;
        int max_len = (src_len - si) > UTF8_CHAR_LEN_MAX ? UTF8_CHAR_LEN_MAX : (int)(src_len - si);
        int c = unicode_from_utf8(p, max_len, &pnext);
        int clen;
        int lone_surrogate = (c >= 0xD800 && c <= 0xDFFF);
        if (c < 0) {
            c = 0xFFFD;
            clen = 1;
        } else if (lone_surrogate) {
            clen = 3;
        } else {
            clen = (int)(pnext - p);
        }
        if (di + (size_t)clen > dst_len)
            break;
        if (lone_surrogate) {
            dst[di] = (uint8_t)0xEF;
            dst[di + 1] = (uint8_t)0xBF;
            dst[di + 2] = (uint8_t)0xBD;
        } else {
            memcpy(dst + di, p, clen);
        }
        di += clen;
        si += clen;
        written += clen;
        read += (c >= 0x10000) ? 2 : 1;
    }
    JS_FreeValue(ctx, buffer);
    JS_FreeCString(ctx, src);

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, res, "read", JS_NewInt64(ctx, read), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "written", JS_NewInt64(ctx, written), JS_PROP_C_W_E);
    return res;
}

static JSValue js_textdecoder_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextDecoder* td;
    BOOL fatal = FALSE, ignore_bom = FALSE;
    int kind = DYN_TD_ENC_UTF8;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        size_t label_len;
        const char* label = JS_ToCStringLen(ctx, &label_len, argv[0]);
        if (!label)
            return JS_EXCEPTION;
        kind = dyn_textdec_label(label, label_len);
        JS_FreeCString(ctx, label);
        if (kind < 0 || kind == DYN_TD_ENC_REPLACEMENT)
            return JS_ThrowRangeError(ctx, "unsupported encoding label");
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const valid[] = { "fatal", "ignoreBOM", NULL };
        JSValue v;
        int b;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "TextDecoder options must be an object");
        if (js_textcodec_check_options(ctx, argv[1], valid,
                "fatal, ignoreBOM"))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "fatal");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        fatal = b;
        v = JS_GetPropertyStr(ctx, argv[1], "ignoreBOM");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        ignore_bom = b;
    }

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_textdecoder_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_textdecoder_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    td = js_mallocz(ctx, sizeof(*td));
    if (!td)
        goto fail;
    td->fatal = fatal;
    td->ignore_bom = ignore_bom;
    td->enc = kind;
    dyn_textdec_init(&td->ld, kind);
    js_utf8_decode_state_init(&td->st);
    JS_SetOpaque(obj, td);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_textdecoder_get_encoding(JSContext* ctx, JSValueConst this_val)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewString(ctx, dyn_textdec_name(td->enc));
}

static JSValue js_textdecoder_get_fatal(JSContext* ctx, JSValueConst this_val)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->fatal);
}

static JSValue js_textdecoder_get_ignoreBOM(JSContext* ctx, JSValueConst this_val)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->ignore_bom);
}

static JSValue js_textdecoder_legacy_string(JSContext* ctx, const uint16_t* u, size_t n)
{
    JSValue ret;
    size_t i;
    uint16_t acc = 0;
    for (i = 0; i < n; i++)
        acc |= u[i];
    if (acc < 0x100) {
        char* p;
        ret = JS_NewString8Buf(ctx, n, &p);
        if (p)
            for (i = 0; i < n; i++)
                p[i] = (char)u[i];
    } else {
        uint16_t* p;
        ret = JS_NewString16Buf(ctx, n, &p);
        if (p)
            memcpy(p, u, n * 2);
    }
    return ret;
}

static JSValue js_textdecoder_decode_legacy(JSContext* ctx, JSTextDecoder* td,
    int argc, JSValueConst* argv, BOOL stream)
{
    uint8_t* buf = NULL;
    const uint8_t* in = NULL;
    uint16_t* out;
    size_t len = 0;
    int strip = !td->ignore_bom && (td->enc == DYN_TD_ENC_UTF16BE || td->enc == DYN_TD_ENC_UTF16LE);
    JSValue ab = JS_UNDEFINED, ret;
    long w;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int kind = JS_GetBufferKind(argv[0]);
        size_t voff = 0, vlen = 0, asize = 0;
        const uint8_t* base = NULL;
        if (kind == JS_BUFFER_KIND_BUFFER) {
            base = JS_GetArrayBuffer(ctx, &asize, argv[0]);
            vlen = asize;
        } else if (kind == JS_BUFFER_KIND_VIEW) {
            ab = JS_GetArrayBufferView(ctx, argv[0], &voff, &vlen, NULL);
            base = JS_IsException(ab) ? NULL : JS_GetArrayBuffer(ctx, &asize, ab);
        }
        if (base && voff <= asize && vlen <= asize - voff) {
            in = base + voff;
            len = vlen;
        } else {
            if (kind != JS_BUFFER_KIND_NONE)
                JS_FreeValue(ctx, JS_GetException(ctx));
            if (!JS_IsException(ab))
                JS_FreeValue(ctx, ab);
            ab = JS_UNDEFINED;
            if (js_textcodec_get_bytes(ctx, argv[0], &buf, &len))
                return JS_EXCEPTION;
            in = buf;
        }
    }
    out = js_malloc(ctx, dyn_textdec_max_out(len) * sizeof(uint16_t));
    if (!out) {
        js_free(ctx, buf);
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    w = dyn_textdec_run(&td->ld, in, len, !stream, td->fatal, strip, out);
    js_free(ctx, buf);
    JS_FreeValue(ctx, ab);
    if (w < 0) {
        js_free(ctx, out);
        dyn_textdec_init(&td->ld, td->enc);
        return JS_ThrowTypeError(ctx, "The encoded data was not valid for encoding %s", dyn_textdec_name(td->enc));
    }
    if (!stream)
        dyn_textdec_init(&td->ld, td->enc);
    ret = js_textdecoder_legacy_string(ctx, out, (size_t)w);
    js_free(ctx, out);
    return ret;
}

static JSValue js_textdecoder_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    BOOL fatal, ignore_bom, stream = FALSE, was_active;
    const uint8_t* p;
    uint8_t* buf;
    size_t len, n;
    DynBuf dbuf;
    JSValue ret;

    if (!td)
        return JS_EXCEPTION;
    fatal = td->fatal;
    ignore_bom = td->ignore_bom;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const valid[] = { "stream", NULL };
        JSValue v;
        int b;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "decode() options must be an object");
        if (js_textcodec_check_options(ctx, argv[1], valid, "stream"))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "stream");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "option \"stream\" must be a boolean");
        }
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        stream = b;
    }

    if (td->enc != DYN_TD_ENC_UTF8)
        return js_textdecoder_decode_legacy(ctx, td, argc, argv, stream);

    if (!stream && !td->stream_active && td->st.bytes_needed == 0 && argc >= 1
        && JS_GetBufferKind(argv[0]) == JS_BUFFER_KIND_VIEW) {
        size_t voff, vlen, asize;
        JSValue ab = JS_GetArrayBufferView(ctx, argv[0], &voff, &vlen, NULL);
        const uint8_t* base = JS_IsException(ab) ? NULL : JS_GetArrayBuffer(ctx, &asize, ab);
        if (base && voff <= asize && vlen <= asize - voff) {
            const uint8_t* q = base + voff;
            size_t m = vlen;
            if (!ignore_bom && m >= 3 && !memcmp(q, utf8_bom, 3)) {
                q += 3;
                m -= 3;
            }
            if (simd.validate_utf8(q, m) == m) {
                ret = JS_NewStringLen(ctx, (const char*)q, m);
                JS_FreeValue(ctx, ab);
                return ret;
            }
        }
        if (!base)
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (!JS_IsException(ab))
            JS_FreeValue(ctx, ab);
    }

    if (argc < 1 || JS_IsUndefined(argv[0])) {
        if (!stream && !td->stream_active)
            return JS_NewStringLen(ctx, "", 0);
        buf = NULL;
        len = 0;
    } else {
        if (js_textcodec_get_bytes(ctx, argv[0], &buf, &len))
            return JS_EXCEPTION;
    }

    was_active = td->stream_active;
    p = buf;
    n = len;
    if (!was_active) {
        if (!ignore_bom && n >= 3 && !memcmp(p, utf8_bom, 3)) {
            p += 3;
            n -= 3;
            td->bom_handled = TRUE;
        }
    }
    if (n >= JS_TEXTDECODER_WHOLE_MIN && td->st.bytes_needed == 0 && (!was_active || td->bom_handled)
        && simd.validate_utf8(p, n) == n) {
        td->bom_handled = TRUE;
        ret = JS_NewStringLen(ctx, (const char*)p, n);
        js_free(ctx, buf);
    } else {
        js_std_dbuf_init(ctx, &dbuf);
        if (js_utf8_decode_whatwg(ctx, &dbuf, p, n, fatal, &td->st, !stream)) {
            dbuf_free(&dbuf);
            js_free(ctx, buf);
            js_utf8_decode_state_init(&td->st);
            td->stream_active = FALSE;
            td->bom_handled = FALSE;
            return JS_EXCEPTION;
        }
        if (!ignore_bom && was_active && !td->bom_handled && dbuf.size >= 3 && !memcmp(dbuf.buf, utf8_bom, 3)) {
            memmove(dbuf.buf, dbuf.buf + 3, dbuf.size - 3);
            dbuf.size -= 3;
        }
        if (dbuf.size > 0)
            td->bom_handled = TRUE;
        ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
        dbuf_free(&dbuf);
        js_free(ctx, buf);
    }
    if (stream) {
        td->stream_active = TRUE;
    } else {
        js_utf8_decode_state_init(&td->st);
        td->stream_active = FALSE;
        td->bom_handled = FALSE;
    }
    return ret;
}

static const JSCFunctionListEntry js_textencoder_proto_funcs[] = {
    JS_CGETSET_DEF("encoding", js_textencoder_get_encoding, NULL),
    JS_CFUNC_DEF("encode", 1, js_textencoder_encode),
    JS_CFUNC_DEF("encodeInto", 2, js_textencoder_encodeInto),
};

static const JSCFunctionListEntry js_textdecoder_proto_funcs[] = {
    JS_CGETSET_DEF("encoding", js_textdecoder_get_encoding, NULL),
    JS_CGETSET_DEF("fatal", js_textdecoder_get_fatal, NULL),
    JS_CGETSET_DEF("ignoreBOM", js_textdecoder_get_ignoreBOM, NULL),
    JS_CFUNC_DEF("decode", 1, js_textdecoder_decode),
};

void js_textcodec_install(JSContext* ctx, JSValueConst global_obj)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(&js_textencoder_class_id);
    JS_NewClass(rt, js_textencoder_class_id, &js_textencoder_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_textencoder_proto_funcs,
        countof(js_textencoder_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_textencoder_ctor, "TextEncoder", 0,
        JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, js_textencoder_class_id, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, "TextEncoder", ctor, JS_PROP_C_W_E);

    JS_NewClassID(&js_textdecoder_class_id);
    JS_NewClass(rt, js_textdecoder_class_id, &js_textdecoder_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_textdecoder_proto_funcs,
        countof(js_textdecoder_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_textdecoder_ctor, "TextDecoder", 0,
        JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, js_textdecoder_class_id, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, "TextDecoder", ctor, JS_PROP_C_W_E);
}

void js_textcodec_utf8_init(JSUtf8DecodeState* st)
{
    js_utf8_decode_state_init(st);
}

int js_textcodec_utf8_feed(JSContext* ctx, DynBuf* dbuf, const uint8_t* buf, size_t len, JSUtf8DecodeState* st, int final)
{
    return js_utf8_decode_whatwg(ctx, dbuf, buf, len, FALSE, st, final);
}
