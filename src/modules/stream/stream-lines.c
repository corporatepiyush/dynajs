#include "stream-internal.h"
#include "dyna-libc.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

#define LINE_MODE_LINES 0
#define LINES_BATCH_MAX 65536u

typedef struct {
    JSValue src;
    JSValue reader, pend_err;
    uint8_t* buf;
    size_t buf_len, buf_cap, buf_pos;
    uint64_t lineno;
    int mode;
    int eof;
    int finished;
    int busy;
    int batch;
    int ascii;
    uint8_t hi;
    size_t scan;
} lines_iter_t;

static void lines_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void lines_op_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    lines_op_t* op = (lines_op_t*)opaque;
    stream_park_fail(ctx, rt, &op->jresolve, &op->jreject,
        "stream.lines: next() aborted at engine shutdown");
    stream_op_sweep_tail(ctx, rt, &op->cps, &op->it_obj, 3);
}

static void lines_op_retire(JSRuntime* rt, lines_op_t* op)
{
    if (op->cps.dead)
        return;
    JS_RemoveShutdownSweep(rt, lines_op_sweep, op);
    JS_ShutdownUndeferFree(rt, op);
}

void lines_iter_finalizer(JSRuntime* rt, JSValue val)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(val, lines_iter_class_id);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->src);
    JS_FreeValueRT(rt, it->reader);
    JS_FreeValueRT(rt, it->pend_err);
    free(it->buf);
    free(it);
}

void lines_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(val, lines_iter_class_id);
    if (!it)
        return;
    JS_MarkValue(rt, it->src, mark);
    JS_MarkValue(rt, it->reader, mark);
    JS_MarkValue(rt, it->pend_err, mark);
}

static JSValue lines_done_value(JSContext* ctx)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result))
        return result;
    JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
    return result;
}

static void lines_op_done(JSContext* ctx, lines_iter_t* it, lines_op_t* op)
{
    JSValue result, r;

    if (op->cps.dead)
        return;
    lines_op_retire(JS_GetRuntime(ctx), op);
    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), &op->cps);
    if (it)
        it->busy = 0;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) {
        stream_drop_exc(ctx);
        r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 0, NULL);
    } else {
        JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
        r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
            (JSValueConst*)&result);
        JS_FreeValue(ctx, result);
    }
    if (JS_IsException(r))
        stream_drop_exc(ctx);
    else
        JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static void lines_op_settle(JSContext* ctx, lines_iter_t* it, lines_op_t* op,
    JSValue value, int failed)
{
    if (op->cps.dead) {
        JS_FreeValue(ctx, value);
        return;
    }
    lines_op_retire(JS_GetRuntime(ctx), op);
    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), &op->cps);
    if (it)
        it->busy = 0;
    if (failed) {
        JSValue r = JS_IsException(value) ? JS_EXCEPTION : JS_Call(ctx, op->jreject, JS_UNDEFINED, 1, (JSValueConst*)&value);
        JS_FreeValue(ctx, value);
        if (JS_IsException(r))
            stream_drop_exc(ctx);
        else
            JS_FreeValue(ctx, r);
    } else {
        JSValue result = JS_NewObject(ctx);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, value);
            value = JS_GetException(ctx);
            result = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1,
                (JSValueConst*)&value);
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, value);
        } else {
            JSValue r;
            JS_DefinePropertyValueStr(ctx, result, "value", value,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, op->jresolve, JS_UNDEFINED, 1,
                (JSValueConst*)&result);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                stream_drop_exc(ctx);
            else
                JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static const uint8_t* lines_find_nl(const uint8_t* p, const uint8_t* e, uint8_t* hi)
{
    uint64_t acc = *hi;
    if (p < e && *p == '\n')
        return p;
#if defined(__aarch64__) && defined(__ARM_NEON)
    if (e - p >= 16) {
        static const uint8_t iota[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        uint8x16_t nlv = vdupq_n_u8('\n'), va = vdupq_n_u8(0);
        do {
            uint8x16_t v = vld1q_u8(p);
            uint64_t m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(v, nlv)), 4)), 0);
            if (m) {
                unsigned k = (unsigned)__builtin_ctzll(m) >> 2;
                va = vorrq_u8(va, vandq_u8(v, vcltq_u8(vld1q_u8(iota), vdupq_n_u8((uint8_t)k))));
                *hi = (uint8_t)(acc | vmaxvq_u8(va));
                return p + k;
            }
            va = vorrq_u8(va, v);
            p += 16;
        } while (e - p >= 16);
        acc |= vmaxvq_u8(va);
    }
#elif defined(__SSE2__)
    if (e - p >= 16) {
        __m128i nlv = _mm_set1_epi8('\n'), va = _mm_setzero_si128();
        do {
            __m128i v = _mm_loadu_si128((const __m128i*)(const void*)p);
            unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, nlv));
            if (m) {
                unsigned k = (unsigned)__builtin_ctz(m);
                if ((unsigned)_mm_movemask_epi8(_mm_or_si128(va, v)) & ((1u << k) - 1u))
                    acc |= 0x80;
                if (_mm_movemask_epi8(va))
                    acc |= 0x80;
                *hi = (uint8_t)acc;
                return p + k;
            }
            va = _mm_or_si128(va, v);
            p += 16;
        } while (e - p >= 16);
        if (_mm_movemask_epi8(va))
            acc |= 0x80;
    }
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    while (e - p >= 8) {
        uint64_t v, x, z;
        memcpy(&v, p, 8);
        x = v ^ 0x0a0a0a0a0a0a0a0aULL;
        z = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
        if (z) {
            unsigned k = (unsigned)__builtin_ctzll(z) >> 3;
            if (k)
                acc |= v & (~0ULL >> (64 - 8 * k));
            *hi = (acc & 0x8080808080808080ULL) ? 0x80 : 0;
            return p + k;
        }
        acc |= v;
        p += 8;
    }
#endif
    for (; p < e; p++) {
        if (*p == '\n') {
            *hi = (acc & 0x8080808080808080ULL) ? 0x80 : 0;
            return p;
        }
        acc |= *p;
    }
    *hi = (acc & 0x8080808080808080ULL) ? 0x80 : 0;
    return NULL;
}

static int lines_extract_bytes(lines_iter_t* it, const uint8_t** out,
    size_t* out_len)
{
    const uint8_t* nl;
    size_t from;

    if (it->buf_pos >= it->buf_len)
        return 0;
    from = it->scan > it->buf_pos ? it->scan : it->buf_pos;
    nl = lines_find_nl(it->buf + from, it->buf + it->buf_len, &it->hi);
    if (!nl) {
        it->scan = it->buf_len;
        return 0;
    }
    it->ascii = !(it->hi & 0x80);
    it->hi = 0;
    *out = it->buf + it->buf_pos;
    *out_len = (size_t)(nl - *out);
    it->buf_pos = (size_t)(nl - it->buf) + 1;
    it->scan = it->buf_pos;
    if (*out_len > 0 && (*out)[*out_len - 1] == '\r')
        (*out_len)--;
    return 1;
}

static int lines_buf_reserve(lines_iter_t* it, JSContext* ctx, size_t need)
{
    if (it->buf_pos > 0) {
        if (it->buf_pos == it->buf_len) {
            it->buf_pos = it->buf_len = 0;
            it->scan = 0;
            it->hi = 0;
        } else if (it->buf_pos >= it->buf_cap / 2) {
            memmove(it->buf, it->buf + it->buf_pos, it->buf_len - it->buf_pos);
            it->buf_len -= it->buf_pos;
            it->scan -= it->buf_pos < it->scan ? it->buf_pos : it->scan;
            it->buf_pos = 0;
        }
    }
    if (it->buf_len + need + 1 > it->buf_cap) {
        size_t ncap = it->buf_cap ? it->buf_cap * 2 : 4096;
        uint8_t* nb;
        while (ncap < it->buf_len + need + 1)
            ncap *= 2;
        if (ncap > DYN_MAX_INPUT) {
            JS_ThrowRangeError(ctx, "stream.lines: line exceeds %u bytes"
                                    " (DYN_MAX_INPUT)",
                (unsigned)DYN_MAX_INPUT);
            return -1;
        }
        nb = (uint8_t*)realloc(it->buf, ncap);
        if (!nb) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        it->buf = nb;
        it->buf_cap = ncap;
    }
    return 0;
}

static void lines_step_drive(JSContext* ctx, lines_op_t* op);

static int lines_take(JSContext* ctx, lines_iter_t* it, JSValue* out)
{
    for (;;) {
        const uint8_t* lb = NULL;
        size_t lb_len = 0;
        int have = lines_extract_bytes(it, &lb, &lb_len);
        if (!have) {
            if (!it->eof)
                return 0;
            if (it->buf_pos >= it->buf_len)
                return 2;
            lb = it->buf + it->buf_pos;
            lb_len = it->buf_len - it->buf_pos;
            if (lb_len > 0 && lb[lb_len - 1] == '\r')
                lb_len--;
            it->buf_pos = it->buf_len;
            it->ascii = 0;
        }
        it->lineno++;
        if (it->mode != LINE_MODE_NDJSON) {
            if (it->ascii && lb_len) {
                char* d;
                *out = JS_NewString8Buf(ctx, lb_len, &d);
                if (d)
                    memcpy(d, lb, lb_len);
            } else {
                *out = JS_NewStringLen(ctx, (const char*)lb, lb_len);
            }
            return JS_IsException(*out) ? -1 : 1;
        }
        {
            size_t i = 0;
            uint8_t* endp;
            uint8_t saved;
            JSValue value;
            while (i < lb_len && (lb[i] == ' ' || lb[i] == '\t' || lb[i] == '\r'))
                i++;
            if (i == lb_len)
                continue;
            endp = (uint8_t*)DYN_UNCONST(lb) + lb_len;
            saved = *endp;
            *endp = '\0';
            value = JS_ParseJSON(ctx, (const char*)lb, lb_len, "<dyna:stream:ndjson>");
            *endp = saved;
            if (JS_IsException(value)) {
                JSValue exc;
                if (JS_HasUncatchableException(ctx))
                    return -1;
                exc = JS_GetException(ctx);
                JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
                const char* mstr = NULL;
                char mbuf[288];
                if (!JS_IsException(msg) && !JS_IsUndefined(msg))
                    mstr = JS_ToCString(ctx, msg);
                snprintf(mbuf, sizeof(mbuf), "ndjson: line %llu: %s",
                    (unsigned long long)it->lineno, mstr ? mstr : "invalid JSON");
                if (mstr)
                    JS_FreeCString(ctx, mstr);
                JS_FreeValue(ctx, msg);
                JS_FreeValue(ctx, exc);
                JS_ThrowSyntaxError(ctx, "%s", mbuf);
                return -1;
            }
            *out = value;
            return 1;
        }
    }
}

static void lines_step_rs_done(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed)
{
    lines_op_t* op = (lines_op_t*)cp;
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(op->it_obj, lines_iter_class_id);
    JSValue dv, v;
    const uint8_t* p;
    const char* cs = NULL;
    size_t n = 0;
    if (failed || !it) {
        lines_op_settle(ctx, it, op, failed ? JS_DupValue(ctx, val) : JS_UNDEFINED, 1);
        return;
    }
    dv = JS_GetPropertyStr(ctx, val, "done");
    if (JS_IsException(dv)) {
        lines_op_settle(ctx, it, op, stream_take_exc(ctx), 1);
        return;
    }
    if (JS_ToBool(ctx, dv)) {
        JS_FreeValue(ctx, dv);
        it->eof = 1;
        js_streams_reader_close(ctx, it->reader, 0);
        lines_step_drive(ctx, op);
        return;
    }
    JS_FreeValue(ctx, dv);
    v = JS_GetPropertyStr(ctx, val, "value");
    if (JS_IsString(v)) {
        cs = JS_ToCStringLen(ctx, &n, v);
        p = (const uint8_t*)cs;
    } else {
        p = stream_view_bytes(ctx, v, &n, 0);
    }
    if (!p) {
        JS_FreeValue(ctx, v);
        if (JS_HasUncatchableException(ctx)) {
            lines_op_settle(ctx, it, op, JS_EXCEPTION, 1);
            return;
        }
        stream_drop_exc(ctx);
        JS_ThrowTypeError(ctx, "%s: ReadableStream chunks must be BufferSources or strings",
            it->mode == LINE_MODE_NDJSON ? "ndjson" : "lines");
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
        return;
    }
    if (n && lines_buf_reserve(it, ctx, n) < 0) {
        if (cs)
            JS_FreeCString(ctx, cs);
        JS_FreeValue(ctx, v);
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
        return;
    }
    if (n)
        memcpy(it->buf + it->buf_len, p, n);
    it->buf_len += n;
    if (cs)
        JS_FreeCString(ctx, cs);
    JS_FreeValue(ctx, v);
    lines_step_drive(ctx, op);
}

static void lines_settle_batch(JSContext* ctx, lines_iter_t* it, lines_op_t* op, JSValue* vec, uint32_t n)
{
    JSValue arr = JS_NewArrayFrom(ctx, (int)n, vec);
    if (JS_IsException(arr))
        lines_op_settle(ctx, it, op, JS_GetException(ctx), 1);
    else
        lines_op_settle(ctx, it, op, arr, 0);
}

static void lines_step_drive(JSContext* ctx, lines_op_t* op)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque(op->it_obj, lines_iter_class_id);
    JSValue* vec = NULL;
    uint32_t n = 0, cap = 0;
    JSValue v, p;
    int r;
    if (!it) {
        lines_op_settle(ctx, NULL, op, JS_UNDEFINED, 1);
        return;
    }
    for (;;) {
        r = lines_take(ctx, it, &v);
        if (r == 1 && !it->batch) {
            lines_op_settle(ctx, it, op, v, 0);
            return;
        }
        if (r == 1) {
            if (n == cap) {
                uint32_t nc = cap ? cap * 2 : 64;
                JSValue* nv = (JSValue*)realloc(vec, nc * sizeof(JSValue));
                if (!nv) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowOutOfMemory(ctx);
                    r = -1;
                } else {
                    vec = nv;
                    cap = nc;
                }
            }
            if (r == 1) {
                vec[n++] = v;
                if (n < LINES_BATCH_MAX)
                    continue;
                break;
            }
        }
        if (r == -1) {
            JSValue e = stream_take_exc(ctx);
            if (JS_IsException(e)) {
                while (n)
                    JS_FreeValue(ctx, vec[--n]);
            }
            if (n) {
                JS_FreeValue(ctx, it->pend_err);
                it->pend_err = e;
                break;
            }
            free(vec);
            lines_op_settle(ctx, it, op, e, 1);
            return;
        }
        if (r == 2) {
            it->finished = 1;
            if (n)
                break;
            free(vec);
            lines_op_done(ctx, it, op);
            return;
        }
        if (n)
            break;
        free(vec);
        p = js_streams_reader_read(ctx, it->reader);
        if (JS_IsException(p)) {
            lines_op_settle(ctx, it, op, stream_take_exc(ctx), 1);
            return;
        }
        dyn_cps_await(ctx, &op->cps, lines_step_rs_done, p);
        return;
    }
    lines_settle_batch(ctx, it, op, vec, n);
    free(vec);
}

JSValue lines_iter_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque2(ctx, this_val,
        lines_iter_class_id);
    lines_op_t* op;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise, v;
    int r;
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    if (it->busy)
        return JS_ThrowTypeError(ctx,
            "stream iterator: next() called while a previous next() is"
            " still pending");
    if (!JS_IsUndefined(it->pend_err)) {
        JSValue e = it->pend_err;
        it->pend_err = JS_UNDEFINED;
        return stream_promise_rejected(ctx, e);
    }
    if (it->finished)
        return stream_promise_resolved(ctx, lines_done_value(ctx));
    if (!it->batch) {
        r = lines_take(ctx, it, &v);
        if (r == 1)
            return stream_promise_resolved(ctx, JS_NewIterResult(ctx, v, 0));
        if (r == -1)
            return stream_promise_rejected(ctx, JS_GetException(ctx));
        if (r == 2) {
            it->finished = 1;
            return stream_promise_resolved(ctx, lines_done_value(ctx));
        }
    }
    op = (lines_op_t*)calloc(1, sizeof(*op));
    if (!op)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        free(op);
        return promise;
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    JS_AddShutdownSweep(JS_GetRuntime(ctx), lines_op_sweep, op);
    it->busy = 1;
    lines_step_drive(ctx, op);
    return stream_ret(ctx, promise);
}

JSValue lines_iter_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    lines_iter_t* it = (lines_iter_t*)JS_GetOpaque2(ctx, this_val,
        lines_iter_class_id);
    (void)argc;
    (void)argv;
    if (!it)
        return JS_EXCEPTION;
    if (!it->finished) {
        it->finished = 1;
        js_streams_reader_close(ctx, it->reader, 1);
    }
    JS_FreeValue(ctx, it->pend_err);
    it->pend_err = JS_UNDEFINED;
    return stream_promise_resolved(ctx, lines_done_value(ctx));
}

JSValue lines_iter_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

JSValue stream_lines_common(JSContext* ctx, int argc,
    JSValueConst* argv, int mode)
{
    lines_iter_t* it;
    JSValue obj, proto, src, reader;
    int batch = 0;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            mode == LINE_MODE_NDJSON
                ? "ndjson(src[, {batch}]) requires a ByteSource or a ReadableStream"
                : "lines(src[, {encoding, batch}]) requires a ByteSource or a ReadableStream");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue e = mode == LINE_MODE_LINES ? JS_GetPropertyStr(ctx, argv[1], "encoding") : JS_UNDEFINED;
        if (JS_IsException(e))
            return e;
        if (!JS_IsUndefined(e) && !JS_IsNull(e)) {
            const char* es = JS_ToCString(ctx, e);
            int ok;
            if (!es) {
                JS_FreeValue(ctx, e);
                return JS_EXCEPTION;
            }
            ok = !strcmp(es, "utf-8") || !strcmp(es, "utf8");
            JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (!ok)
                return JS_ThrowTypeError(ctx,
                    "lines: only encoding \"utf-8\" is supported");
        } else {
            JS_FreeValue(ctx, e);
        }
        e = JS_GetPropertyStr(ctx, argv[1], "batch");
        if (JS_IsException(e))
            return e;
        batch = JS_ToBool(ctx, e);
        JS_FreeValue(ctx, e);
    }
    if (js_streams_kind(argv[0]) == 1) {
        src = JS_DupValue(ctx, argv[0]);
    } else {
        JSValue rf = JS_GetPropertyStr(ctx, argv[0], "read");
        if (!JS_IsFunction(ctx, rf)) {
            JS_FreeValue(ctx, rf);
            return JS_ThrowTypeError(ctx, "lines: src.read(buf) is not a function");
        }
        JS_FreeValue(ctx, rf);
        src = js_streams_call(ctx, "toReadableStream", 1, argv);
        if (JS_IsException(src))
            return src;
    }
    reader = js_streams_reader_open(ctx, src);
    if (JS_IsException(reader)) {
        JS_FreeValue(ctx, src);
        return reader;
    }

    it = (lines_iter_t*)calloc(1, sizeof(*it));
    if (!it) {
        js_streams_reader_close(ctx, reader, 0);
        JS_FreeValue(ctx, reader);
        JS_FreeValue(ctx, src);
        return JS_ThrowOutOfMemory(ctx);
    }
    it->mode = mode;
    it->batch = batch;
    it->pend_err = JS_UNDEFINED;
    it->reader = reader;
    it->src = src;
    proto = JS_GetClassProto(ctx, lines_iter_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, lines_iter_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        js_streams_reader_close(ctx, it->reader, 0);
        JS_FreeValue(ctx, it->src);
        JS_FreeValue(ctx, it->reader);
        free(it);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

JSValue stream_lines(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return stream_lines_common(ctx, argc, argv, LINE_MODE_LINES);
}

#endif
