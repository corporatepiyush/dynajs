#include "webrtc-js.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modules/libc/libc-internal.h"

JSClassID wjs_dc_cid, wjs_cand_cid, wjs_desc_cid, wjs_cert_cid;

static const char* const wjs_dc_names[WJS_DE_N] = { "open", "message", "bufferedamountlow", "error", "closing", "close" };
static const char* const wjs_dc_states[] = { "connecting", "open", "closing", "closed" };

int wjs_et_add(JSContext* ctx, wjs_et* et, int type, JSValueConst fn, int capture, int once, int handler)
{
    wjs_l* l;
    if (et->n == et->cap) {
        uint32_t nc = et->cap ? et->cap * 2 : 4;
        wjs_l* nl = js_realloc(ctx, et->ls, nc * sizeof(*nl));
        if (!nl)
            return -1;
        et->ls = nl;
        et->cap = nc;
    }
    l = &et->ls[et->n++];
    l->fn = JS_DupValue(ctx, fn);
    l->type = (uint8_t)type;
    l->capture = (uint8_t)capture;
    l->once = (uint8_t)once;
    l->handler = (uint8_t)handler;
    l->id = ++et->next_id;
    return 0;
}

static void wjs_et_drop(JSContext* ctx, wjs_et* et, uint32_t i)
{
    JS_FreeValue(ctx, et->ls[i].fn);
    memmove(&et->ls[i], &et->ls[i + 1], (et->n - i - 1) * sizeof(wjs_l));
    et->n--;
}

static int wjs_type_index(JSContext* ctx, JSValueConst v, const char* const* names, int nn)
{
    const char* s = JS_ToCString(ctx, v);
    int i;
    if (!s)
        return -2;
    for (i = 0; i < nn; i++)
        if (!strcmp(s, names[i]))
            break;
    JS_FreeCString(ctx, s);
    return i < nn ? i : -1;
}

JSValue wjs_et_listen(JSContext* ctx, wjs_et* et, const char* const* names, int nn, int argc, JSValueConst* argv, int remove)
{
    int type, capture = 0, once = 0;
    uint32_t i;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%sEventListener: 2 arguments required", remove ? "remove" : "add");
    type = wjs_type_index(ctx, argv[0], names, nn);
    if (type == -2)
        return JS_EXCEPTION;
    if (argc > 2) {
        if (JS_IsObject(argv[2])) {
            JSValue v = JS_GetPropertyStr(ctx, argv[2], "capture");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            capture = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (!remove) {
                v = JS_GetPropertyStr(ctx, argv[2], "once");
                if (JS_IsException(v))
                    return JS_EXCEPTION;
                once = JS_ToBool(ctx, v);
                JS_FreeValue(ctx, v);
            }
        } else
            capture = JS_ToBool(ctx, argv[2]);
    }
    if (type < 0 || !JS_IsObject(argv[1]))
        return JS_UNDEFINED;
    for (i = 0; i < et->n; i++)
        if (!et->ls[i].handler && et->ls[i].type == type && et->ls[i].capture == capture
            && JS_StrictEq(ctx, et->ls[i].fn, argv[1])) {
            if (remove)
                wjs_et_drop(ctx, et, i);
            return JS_UNDEFINED;
        }
    if (remove)
        return JS_UNDEFINED;
    return wjs_et_add(ctx, et, type, argv[1], capture, once, 0) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue wjs_et_on_get(JSContext* ctx, wjs_et* et, int type)
{
    return JS_DupValue(ctx, et->on[type]);
}

JSValue wjs_et_on_set(JSContext* ctx, wjs_et* et, int type, JSValueConst v)
{
    uint32_t i;
    int had = !JS_IsNull(et->on[type]);
    JS_FreeValue(ctx, et->on[type]);
    et->on[type] = JS_IsObject(v) ? JS_DupValue(ctx, v) : JS_NULL;
    if (JS_IsNull(et->on[type])) {
        for (i = 0; i < et->n; i++)
            if (et->ls[i].handler && et->ls[i].type == type) {
                wjs_et_drop(ctx, et, i);
                break;
            }
    } else if (!had && wjs_et_add(ctx, et, type, JS_UNDEFINED, 0, 0, 1) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

void wjs_et_free(JSRuntime* rt, wjs_et* et)
{
    uint32_t i;
    for (i = 0; i < et->n; i++)
        JS_FreeValueRT(rt, et->ls[i].fn);
    for (i = 0; i < et->ntypes; i++)
        JS_FreeValueRT(rt, et->on[i]);
    js_free_rt(rt, et->ls);
    et->ls = NULL;
    et->n = et->cap = 0;
    for (i = 0; i < et->ntypes; i++)
        et->on[i] = JS_NULL;
}

void wjs_et_mark(JSRuntime* rt, wjs_et* et, JS_MarkFunc* mark)
{
    uint32_t i;
    for (i = 0; i < et->n; i++)
        JS_MarkValue(rt, et->ls[i].fn, mark);
    for (i = 0; i < et->ntypes; i++)
        JS_MarkValue(rt, et->on[i], mark);
}

void wjs_et_fire(JSContext* ctx, JSValueConst self, wjs_et* et, int type, JSValueConst ev)
{
    uint32_t ids_s[16];
    uint32_t *ids = ids_s, i, k = 0, n = et->n;
    if (n > 16) {
        ids = js_malloc(ctx, n * sizeof(uint32_t));
        if (!ids)
            return;
    }
    for (i = 0; i < n; i++)
        if (et->ls[i].type == type)
            ids[k++] = et->ls[i].id;
    for (i = 0; i < k; i++) {
        JSValue r, fn;
        uint32_t j;
        for (j = 0; j < et->n && et->ls[j].id != ids[i]; j++) {
        }
        if (j == et->n)
            continue;
        fn = et->ls[j].handler ? JS_DupValue(ctx, et->on[type]) : JS_DupValue(ctx, et->ls[j].fn);
        if (et->ls[j].once)
            wjs_et_drop(ctx, et, j);
        if (JS_IsFunction(ctx, fn))
            r = JS_Call(ctx, fn, self, 1, &ev);
        else if (!JS_IsObject(fn))
            r = JS_UNDEFINED;
        else {
            JSValue hf = JS_GetPropertyStr(ctx, fn, "handleEvent");
            if (JS_IsException(hf))
                r = JS_EXCEPTION;
            else if (!JS_IsFunction(ctx, hf))
                r = JS_ThrowTypeError(ctx, "handleEvent is not a function");
            else
                r = JS_Call(ctx, hf, fn, 1, &ev);
            JS_FreeValue(ctx, hf);
        }
        JS_FreeValue(ctx, fn);
        if (JS_IsException(r))
            js_std_dump_error(ctx);
        JS_FreeValue(ctx, r);
    }
    if (ids != ids_s)
        js_free(ctx, ids);
}

JSValue wjs_event(JSContext* ctx, const char* type, JSValueConst target)
{
    JSValue ev = JS_NewObject(ctx);
    if (JS_IsException(ev))
        return ev;
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, ev, "srcElement", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, ev, "isTrusted", JS_TRUE);
    JS_SetPropertyStr(ctx, ev, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "cancelable", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "eventPhase", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, ev, "timeStamp", JS_NewFloat64(ctx, (double)wrt_now_us() / 1000.0));
    return ev;
}

int wjs_dispatch(JSContext* ctx, JSValueConst self, wjs_et* et, const char* const* names, int nn, JSValueConst ev)
{
    JSValue t;
    int type;
    if (!JS_IsObject(ev)) {
        JS_ThrowTypeError(ctx, "dispatchEvent: argument is not an Event");
        return -1;
    }
    t = JS_GetPropertyStr(ctx, ev, "type");
    if (JS_IsException(t))
        return -1;
    type = wjs_type_index(ctx, t, names, nn);
    JS_FreeValue(ctx, t);
    if (type == -2)
        return -1;
    if (type >= 0)
        wjs_et_fire(ctx, self, et, type, ev);
    return 0;
}

JSValue wjs_rtc_error(JSContext* ctx, const char* detail, const char* msg)
{
    JSValue e = js_dom_exception(ctx, "OperationError", msg);
    if (JS_IsException(e))
        return e;
    JS_SetPropertyStr(ctx, e, "errorDetail", JS_NewString(ctx, detail));
    JS_SetPropertyStr(ctx, e, "sdpLineNumber", JS_NULL);
    JS_SetPropertyStr(ctx, e, "sctpCauseCode", JS_NULL);
    JS_SetPropertyStr(ctx, e, "receivedAlert", JS_NULL);
    JS_SetPropertyStr(ctx, e, "sentAlert", JS_NULL);
    JS_SetPropertyStr(ctx, e, "httpRequestStatusCode", JS_NULL);
    return e;
}

static wjs_dc* wjs_dc_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_dc_cid);
}

static void wjs_dc_unpin(JSRuntime* rt, wjs_dc* d)
{
    if (!JS_IsUndefined(d->self)) {
        JSValue v = d->self;
        d->self = JS_UNDEFINED;
        JS_FreeValueRT(rt, v);
    }
}

static void wjs_dc_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_dc* d = JS_GetOpaque(val, wjs_dc_cid);
    if (!d)
        return;
    if (d->dc) {
        d->dc->js = NULL;
        if (d->pc && d->pc->t)
            wrt_dc_release(d->pc->t, d->dc);
    }
    wjs_et_free(rt, &d->et);
    JS_FreeValueRT(rt, d->pc_obj);
    js_free_rt(rt, d->label);
    js_free_rt(rt, d->protocol);
    js_free_rt(rt, d);
}

static void wjs_dc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_dc* d = JS_GetOpaque(val, wjs_dc_cid);
    if (!d)
        return;
    wjs_et_mark(rt, &d->et, mark);
    JS_MarkValue(rt, d->pc_obj, mark);
}

static JSClassDef wjs_dc_class = { "RTCDataChannel", .finalizer = wjs_dc_finalizer, .gc_mark = wjs_dc_mark };

static char* wjs_dup(JSContext* ctx, const char* s)
{
    size_t n = s ? strlen(s) : 0;
    char* d = js_malloc(ctx, n + 1);
    if (!d)
        return NULL;
    if (n)
        memcpy(d, s, n);
    d[n] = 0;
    return d;
}

JSValue wjs_dc_new(JSContext* ctx, wjs_pc* p, wrt_dc* dc)
{
    wjs_dc* d;
    JSValue obj;
    int i;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d)
        return JS_EXCEPTION;
    d->ctx = ctx;
    d->et.ntypes = WJS_DE_N;
    for (i = 0; i < WJS_PE_N; i++)
        d->et.on[i] = JS_NULL;
    d->label = wjs_dup(ctx, dc->label);
    d->protocol = wjs_dup(ctx, dc->protocol);
    if (!d->label || !d->protocol) {
        js_free(ctx, d->label);
        js_free(ctx, d->protocol);
        js_free(ctx, d);
        return JS_EXCEPTION;
    }
    d->pc = p;
    d->dc = dc;
    d->state = dc->state;
    d->id = dc->has_id ? dc->id : -1;
    d->ordered = dc->ordered;
    d->negotiated = dc->negotiated;
    d->max_rtx = dc->max_rtx;
    d->max_life = dc->max_life;
    d->pc_obj = JS_DupValue(ctx, p->self);
    obj = JS_NewObjectClass(ctx, (int)wjs_dc_cid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, d->pc_obj);
        js_free(ctx, d->label);
        js_free(ctx, d->protocol);
        js_free(ctx, d);
        return obj;
    }
    JS_SetOpaque(obj, d);
    dc->js = d;
    d->self = JS_DupValue(ctx, obj);
    return obj;
}

void wjs_dc_detach_all(JSRuntime* rt, wjs_pc* p)
{
    uint32_t i;
    if (!p->t)
        return;
    for (i = 0; i < p->t->ndcs; i++) {
        wrt_dc* dc = p->t->dcs[i];
        if (dc && dc->js) {
            wjs_dc* d = (wjs_dc*)dc->js;
            d->dc = NULL;
            d->pc = NULL;
            if (d->state != WRT_DC_CLOSED)
                d->state = WRT_DC_CLOSED;
            dc->js = NULL;
            wjs_dc_unpin(rt, d);
        }
    }
}

static void wjs_dc_fire(JSContext* ctx, wjs_dc* d, JSValueConst obj, int type, JSValue ev)
{
    if (JS_IsException(ev))
        return;
    wjs_et_fire(ctx, obj, &d->et, type, ev);
    JS_FreeValue(ctx, ev);
}

static void wjs_dc_fire_simple(JSContext* ctx, wjs_dc* d, JSValueConst obj, int type)
{
    wjs_dc_fire(ctx, d, obj, type, wjs_event(ctx, wjs_dc_names[type], obj));
}

void wjs_dc_on_event(JSContext* ctx, wjs_pc* p, const wrt_ev* e)
{
    wrt_dc* dc = e->id < p->t->ndcs ? p->t->dcs[e->id] : NULL;
    wjs_dc* d;
    JSValue obj;
    if (!dc || !dc->js)
        return;
    d = (wjs_dc*)dc->js;
    if (JS_IsUndefined(d->self))
        return;
    obj = JS_DupValue(ctx, d->self);
    switch (e->type) {
    case WRT_EV_DC_OPEN:
        if (d->state == WRT_DC_CONNECTING || (e->flag == 1 && d->state == WRT_DC_OPEN)) {
            d->state = WRT_DC_OPEN;
            d->id = dc->id;
            wjs_dc_fire_simple(ctx, d, obj, WJS_DE_OPEN);
        }
        break;
    case WRT_EV_DC_MSG:
        if (d->state == WRT_DC_OPEN) {
            const uint8_t* bytes = p->t->arena + e->off;
            JSValue data, ev;
            if (!e->flag)
                data = JS_NewStringLen(ctx, (const char*)bytes, e->len);
            else {
                data = JS_NewArrayBufferCopy(ctx, bytes, e->len);
                if (d->binary_blob && !JS_IsException(data)) {
                    JSValue b = js_blob_from_bytes(ctx, data, JS_UNDEFINED);
                    JS_FreeValue(ctx, data);
                    data = b;
                }
            }
            if (JS_IsException(data)) {
                js_std_dump_error(ctx);
                break;
            }
            ev = wjs_event(ctx, "message", obj);
            if (JS_IsException(ev)) {
                JS_FreeValue(ctx, data);
                break;
            }
            JS_SetPropertyStr(ctx, ev, "data", data);
            JS_SetPropertyStr(ctx, ev, "origin", JS_NewString(ctx, ""));
            JS_SetPropertyStr(ctx, ev, "lastEventId", JS_NewString(ctx, ""));
            JS_SetPropertyStr(ctx, ev, "source", JS_NULL);
            JS_SetPropertyStr(ctx, ev, "ports", JS_NewArray(ctx));
            wjs_dc_fire(ctx, d, obj, WJS_DE_MESSAGE, ev);
        }
        break;
    case WRT_EV_DC_BUFLOW:
        if (d->state == WRT_DC_OPEN || d->state == WRT_DC_CLOSING)
            wjs_dc_fire_simple(ctx, d, obj, WJS_DE_BUFLOW);
        break;
    case WRT_EV_DC_CLOSING:
        if (d->state == WRT_DC_OPEN || d->state == WRT_DC_CONNECTING) {
            d->state = WRT_DC_CLOSING;
            wjs_dc_fire_simple(ctx, d, obj, WJS_DE_CLOSING);
        }
        break;
    case WRT_EV_DC_ERROR: {
        JSValue ev = wjs_event(ctx, "error", obj);
        if (!JS_IsException(ev)) {
            JS_SetPropertyStr(ctx, ev, "error",
                wjs_rtc_error(ctx, e->flag == 2 ? "sctp-failure" : "data-channel-failure",
                    e->flag == 1 ? "no stream id available" : e->flag == 3 ? "stream id is not below maxChannels" : e->flag == 2 ? "SCTP association aborted" : "data channel failure"));
            wjs_dc_fire(ctx, d, obj, WJS_DE_ERROR, ev);
        }
        break;
    }
    case WRT_EV_DC_CLOSED:
        if (d->state != WRT_DC_CLOSED) {
            d->state = WRT_DC_CLOSED;
            wjs_dc_fire_simple(ctx, d, obj, WJS_DE_CLOSE);
        }
        wjs_dc_unpin(JS_GetRuntime(ctx), d);
        break;
    default:
        break;
    }
    JS_FreeValue(ctx, obj);
}

static JSValue wjs_dc_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, d->label);
    case 1:
        return JS_NewBool(ctx, d->ordered);
    case 2:
        return d->max_life >= 0 ? JS_NewInt64(ctx, d->max_life) : JS_NULL;
    case 3:
        return d->max_rtx >= 0 ? JS_NewInt32(ctx, d->max_rtx) : JS_NULL;
    case 4:
        return JS_NewString(ctx, d->protocol);
    case 5:
        return JS_NewBool(ctx, d->negotiated);
    case 6:
        if (d->id < 0 && d->dc && d->dc->has_id)
            d->id = d->dc->id;
        return d->id >= 0 ? JS_NewInt32(ctx, d->id) : JS_NULL;
    case 7:
        return JS_NewString(ctx, wjs_dc_states[d->state & 3]);
    case 8:
        return JS_NewInt64(ctx, (int64_t)(d->dc ? d->dc->buffered : d->buffered_shadow));
    case 9:
        return JS_NewInt64(ctx, (int64_t)d->low_threshold);
    case 10:
        return JS_NewString(ctx, d->binary_blob ? "blob" : "arraybuffer");
    case 11:
        return JS_NULL;
    default:
        return JS_UNDEFINED;
    }
}

static JSValue wjs_dc_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    if (magic == 9) {
        uint32_t u;
        double x;
        if (JS_ToFloat64(ctx, &x, v))
            return JS_EXCEPTION;
        if (!isfinite(x))
            x = 0;
        x = trunc(fmod(x, 4294967296.0));
        if (x < 0)
            x += 4294967296.0;
        u = (uint32_t)x;
        d->low_threshold = u;
        if (d->dc)
            d->dc->low_threshold = u;
    } else if (magic == 10) {
        const char* s = JS_ToCString(ctx, v);
        if (!s)
            return JS_EXCEPTION;
        if (!strcmp(s, "blob"))
            d->binary_blob = 1;
        else if (!strcmp(s, "arraybuffer"))
            d->binary_blob = 0;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue wjs_dc_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    return d ? wjs_et_on_get(ctx, &d->et, magic) : JS_EXCEPTION;
}

static JSValue wjs_dc_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    return d ? wjs_et_on_set(ctx, &d->et, magic, v) : JS_EXCEPTION;
}

static JSValue wjs_dc_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    return d ? wjs_et_listen(ctx, &d->et, wjs_dc_names, WJS_DE_N, argc, argv, magic) : JS_EXCEPTION;
}

static JSValue wjs_dc_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &d->et, wjs_dc_names, WJS_DE_N, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static JSValue wjs_dc_send(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    JSValueConst v = argc ? argv[0] : JS_UNDEFINED;
    JSValue hold = JS_UNDEFINED;
    const uint8_t* p = NULL;
    size_t n = 0;
    int binary = 1, r;
    const char* s = NULL;
    uint64_t maxm;
    if (!d)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "RTCDataChannel.send: 1 argument required");
    if (d->state != WRT_DC_OPEN || !d->dc || !d->pc || !d->pc->t)
        return js_throw_dom_exception(ctx, "InvalidStateError", "RTCDataChannel.send: readyState is not 'open'");
    if (JS_IsString(v)) {
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return JS_EXCEPTION;
        p = (const uint8_t*)s;
        binary = 0;
    } else if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
        p = JS_GetArrayBuffer(ctx, &n, v);
        if (!p && n)
            return JS_EXCEPTION;
        if (!p)
            JS_FreeValue(ctx, JS_GetException(ctx));
    } else if (JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW) {
        size_t off = 0, len = 0, bpe = 0, bl = 0;
        uint8_t* base;
        hold = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (JS_IsException(hold))
            return hold;
        base = JS_GetArrayBuffer(ctx, &bl, hold);
        if (!base && len) {
            JS_FreeValue(ctx, hold);
            return JS_EXCEPTION;
        }
        p = base ? base + off : NULL;
        n = len;
    } else if (js_blob_is(v)) {
        size_t off = 0, len = 0, bpe = 0, bl = 0;
        uint8_t* base;
        JSValue u8 = js_blob_bytes(ctx, v);
        if (JS_IsException(u8))
            return u8;
        hold = JS_GetArrayBufferView(ctx, u8, &off, &len, &bpe);
        JS_FreeValue(ctx, u8);
        if (JS_IsException(hold))
            return hold;
        base = JS_GetArrayBuffer(ctx, &bl, hold);
        p = base ? base + off : NULL;
        n = len;
    } else {
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return JS_EXCEPTION;
        p = (const uint8_t*)s;
        binary = 0;
    }
    maxm = d->pc->t->local_max_msg;
    if (d->pc->t->remote_max_msg && d->pc->t->remote_max_msg < maxm)
        maxm = d->pc->t->remote_max_msg;
    if (n > maxm) {
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, hold);
        return JS_ThrowTypeError(ctx, "RTCDataChannel.send: message of %zu bytes exceeds maxMessageSize %llu", n,
            (unsigned long long)maxm);
    }
    r = wrt_dc_send(d->pc->t, d->dc, p, n, binary);
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, hold);
    if (r < 0)
        return js_throw_dom_exception(ctx, "OperationError", "RTCDataChannel.send: the message could not be queued");
    wjs_pc_schedule_flush(ctx, d->pc);
    return JS_UNDEFINED;
}

static JSValue wjs_dc_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_dc* d = wjs_dc_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!d)
        return JS_EXCEPTION;
    if (d->state == WRT_DC_CLOSING || d->state == WRT_DC_CLOSED)
        return JS_UNDEFINED;
    d->state = WRT_DC_CLOSING;
    if (d->dc && d->pc && d->pc->t) {
        wrt_dc_close(d->pc->t, d->dc);
        wjs_pc_schedule_flush(ctx, d->pc);
        if (!d->pc->draining)
            wjs_pc_drain(ctx, d->pc);
    }
    return JS_UNDEFINED;
}

static JSValue wjs_illegal(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static const JSCFunctionListEntry wjs_dc_proto[] = {
    JS_CGETSET_MAGIC_DEF("label", wjs_dc_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("ordered", wjs_dc_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("maxPacketLifeTime", wjs_dc_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("maxRetransmits", wjs_dc_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("protocol", wjs_dc_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("negotiated", wjs_dc_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("id", wjs_dc_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("readyState", wjs_dc_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("bufferedAmount", wjs_dc_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("bufferedAmountLowThreshold", wjs_dc_get, wjs_dc_set, 9),
    JS_CGETSET_MAGIC_DEF("binaryType", wjs_dc_get, wjs_dc_set, 10),
    JS_CGETSET_MAGIC_DEF("priority", wjs_dc_get, NULL, 12),
    JS_CGETSET_MAGIC_DEF("onopen", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_OPEN),
    JS_CGETSET_MAGIC_DEF("onmessage", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_MESSAGE),
    JS_CGETSET_MAGIC_DEF("onbufferedamountlow", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_BUFLOW),
    JS_CGETSET_MAGIC_DEF("onerror", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_ERROR),
    JS_CGETSET_MAGIC_DEF("onclosing", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_CLOSING),
    JS_CGETSET_MAGIC_DEF("onclose", wjs_dc_on_get, wjs_dc_on_set, WJS_DE_CLOSE),
    JS_CFUNC_DEF("send", 1, wjs_dc_send),
    JS_CFUNC_DEF("close", 0, wjs_dc_close),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_dc_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_dc_listen, 1),
    JS_CFUNC_DEF("dispatchEvent", 1, wjs_dc_dispatch),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCDataChannel", JS_PROP_CONFIGURABLE),
};

typedef struct {
    char* cand;
    char* mid;
    char* ufrag;
    int mline;
    int ok;
    rtc_sdp_cand c;
} wjs_cand;

static void wjs_cand_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_cand* c = JS_GetOpaque(val, wjs_cand_cid);
    if (!c)
        return;
    js_free_rt(rt, c->cand);
    js_free_rt(rt, c->mid);
    js_free_rt(rt, c->ufrag);
    js_free_rt(rt, c);
}

static JSClassDef wjs_cand_class = { "RTCIceCandidate", .finalizer = wjs_cand_finalizer };

static char* wjs_dupn(JSContext* ctx, const char* s, size_t n)
{
    char* d = js_malloc(ctx, n + 1);
    if (!d)
        return NULL;
    if (n)
        memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static JSValue wjs_cand_make(JSContext* ctx, JSValueConst new_target, const char* cand, size_t n, const char* mid, int mline,
    const char* ufrag)
{
    wjs_cand* c = js_mallocz(ctx, sizeof(*c));
    JSValue obj;
    if (!c)
        return JS_EXCEPTION;
    c->cand = wjs_dupn(ctx, cand, n);
    c->mid = mid ? wjs_dup(ctx, mid) : NULL;
    c->ufrag = ufrag ? wjs_dup(ctx, ufrag) : NULL;
    c->mline = mline;
    if (!c->cand || (mid && !c->mid) || (ufrag && !c->ufrag)) {
        js_free(ctx, c->cand);
        js_free(ctx, c->mid);
        js_free(ctx, c->ufrag);
        js_free(ctx, c);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->ok = n > 0 && rtc_sdp_parse_candidate(cand, n, &c->c) == 0;
    if (JS_IsUndefined(new_target))
        obj = JS_NewObjectClass(ctx, (int)wjs_cand_cid);
    else
        obj = dyn_plain_wrap(ctx, new_target, wjs_cand_cid, NULL, NULL);
    if (JS_IsException(obj)) {
        js_free(ctx, c->cand);
        js_free(ctx, c->mid);
        js_free(ctx, c->ufrag);
        js_free(ctx, c);
        return obj;
    }
    JS_SetOpaque(obj, c);
    return obj;
}

JSValue wjs_cand_new(JSContext* ctx, const char* cand, size_t n, const char* mid, int mline, const char* ufrag)
{
    return wjs_cand_make(ctx, JS_UNDEFINED, cand, n, mid, mline, ufrag);
}

static int wjs_opt_str(JSContext* ctx, JSValueConst o, const char* k, char** out, int nullable)
{
    JSValue v;
    *out = NULL;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || (nullable && JS_IsNull(v))) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        *out = wjs_dupn(ctx, s, n);
        JS_FreeCString(ctx, s);
        if (!*out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    return 1;
}

static JSValue wjs_cand_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst init = argc ? argv[0] : JS_UNDEFINED;
    char *cand = NULL, *mid = NULL, *ufrag = NULL;
    int mline = -1;
    JSValue obj = JS_EXCEPTION, v;
    if (!JS_IsUndefined(init) && !JS_IsNull(init) && !JS_IsObject(init))
        return JS_ThrowTypeError(ctx, "RTCIceCandidate: init must be an object");
    if (wjs_opt_str(ctx, init, "candidate", &cand, 0) < 0 || wjs_opt_str(ctx, init, "sdpMid", &mid, 1) < 0
        || wjs_opt_str(ctx, init, "usernameFragment", &ufrag, 1) < 0)
        goto done;
    if (JS_IsObject(init)) {
        v = JS_GetPropertyStr(ctx, init, "sdpMLineIndex");
        if (JS_IsException(v))
            goto done;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            uint32_t u;
            if (JS_ToUint32(ctx, &u, v)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            mline = (int)(u & 0xFFFF);
        }
        JS_FreeValue(ctx, v);
    }
    if (!mid && mline < 0) {
        JS_ThrowTypeError(ctx, "RTCIceCandidate: sdpMid and sdpMLineIndex are both null");
        goto done;
    }
    obj = wjs_cand_make(ctx, new_target, cand ? cand : "", cand ? strlen(cand) : 0, mid, mline, ufrag);
done:
    js_free(ctx, cand);
    js_free(ctx, mid);
    js_free(ctx, ufrag);
    return obj;
}

static JSValue wjs_ip_value(JSContext* ctx, const rtc_addr* a)
{
    char ip[64];
    if (!rtc_addr_fmt(a, ip, sizeof(ip)))
        return JS_NULL;
    return JS_NewString(ctx, ip);
}

static JSValue wjs_cand_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_cand* c = JS_GetOpaque2(ctx, this_val, wjs_cand_cid);
    if (!c)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, c->cand);
    case 1:
        return c->mid ? JS_NewString(ctx, c->mid) : JS_NULL;
    case 2:
        return c->mline >= 0 ? JS_NewInt32(ctx, c->mline) : JS_NULL;
    case 3:
        return c->ok ? JS_NewString(ctx, c->c.foundation) : JS_NULL;
    case 4:
        return c->ok ? JS_NewString(ctx, c->c.comp == 1 ? "rtp" : "rtcp") : JS_NULL;
    case 5:
        return c->ok ? JS_NewUint32(ctx, c->c.prio) : JS_NULL;
    case 6:
        if (c->ok && !c->c.addr.fam && c->c.host[0])
            return JS_NewString(ctx, c->c.host);
        return c->ok ? wjs_ip_value(ctx, &c->c.addr) : JS_NULL;
    case 7:
        return c->ok ? JS_NewString(ctx, c->c.udp ? "udp" : "tcp") : JS_NULL;
    case 8:
        return c->ok ? JS_NewInt32(ctx, c->c.addr.port) : JS_NULL;
    case 9:
        return c->ok ? JS_NewString(ctx, c->c.typ) : JS_NULL;
    case 10:
        return c->ok && !c->c.udp && c->c.tcptype[0] ? JS_NewString(ctx, c->c.tcptype) : JS_NULL;
    case 11:
        return c->ok && c->c.has_rel ? wjs_ip_value(ctx, &c->c.rel) : JS_NULL;
    case 12:
        return c->ok && c->c.has_rel ? JS_NewInt32(ctx, c->c.rel.port) : JS_NULL;
    case 13:
        return c->ufrag ? JS_NewString(ctx, c->ufrag) : JS_NULL;
    default:
        return JS_NULL;
    }
}

static JSValue wjs_cand_json(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue o;
    int i;
    static const char* const keys[] = { "candidate", "sdpMid", "sdpMLineIndex", "usernameFragment" };
    static const int mg[] = { 0, 1, 2, 13 };
    (void)argc;
    (void)argv;
    if (!JS_GetOpaque2(ctx, this_val, wjs_cand_cid))
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    for (i = 0; i < 4; i++)
        JS_SetPropertyStr(ctx, o, keys[i], wjs_cand_get(ctx, this_val, mg[i]));
    return o;
}

static const JSCFunctionListEntry wjs_cand_proto[] = {
    JS_CGETSET_MAGIC_DEF("candidate", wjs_cand_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("sdpMid", wjs_cand_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("sdpMLineIndex", wjs_cand_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("foundation", wjs_cand_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("component", wjs_cand_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("priority", wjs_cand_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("address", wjs_cand_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("protocol", wjs_cand_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("port", wjs_cand_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("type", wjs_cand_get, NULL, 9),
    JS_CGETSET_MAGIC_DEF("tcpType", wjs_cand_get, NULL, 10),
    JS_CGETSET_MAGIC_DEF("relatedAddress", wjs_cand_get, NULL, 11),
    JS_CGETSET_MAGIC_DEF("relatedPort", wjs_cand_get, NULL, 12),
    JS_CGETSET_MAGIC_DEF("usernameFragment", wjs_cand_get, NULL, 13),
    JS_CGETSET_MAGIC_DEF("relayProtocol", wjs_cand_get, NULL, 14),
    JS_CGETSET_MAGIC_DEF("url", wjs_cand_get, NULL, 15),
    JS_CFUNC_DEF("toJSON", 0, wjs_cand_json),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCIceCandidate", JS_PROP_CONFIGURABLE),
};

typedef struct {
    int type;
    char* sdp;
} wjs_sd;

static const char* const wjs_sdp_types[] = { "offer", "pranswer", "answer", "rollback" };

const char* wjs_sdp_type_name(int t)
{
    return t >= 0 && t < 4 ? wjs_sdp_types[t] : "";
}

int wjs_sdp_type_parse(const char* s)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!strcmp(s, wjs_sdp_types[i]))
            return i;
    return -1;
}

static void wjs_desc_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_sd* d = JS_GetOpaque(val, wjs_desc_cid);
    if (!d)
        return;
    js_free_rt(rt, d->sdp);
    js_free_rt(rt, d);
}

static JSClassDef wjs_desc_class = { "RTCSessionDescription", .finalizer = wjs_desc_finalizer };

static JSValue wjs_desc_make(JSContext* ctx, JSValueConst new_target, int type, const char* sdp, size_t n)
{
    wjs_sd* d = js_mallocz(ctx, sizeof(*d));
    JSValue obj;
    if (!d)
        return JS_EXCEPTION;
    d->type = type;
    d->sdp = wjs_dupn(ctx, sdp, n);
    if (!d->sdp) {
        js_free(ctx, d);
        return JS_ThrowOutOfMemory(ctx);
    }
    obj = JS_IsUndefined(new_target) ? JS_NewObjectClass(ctx, (int)wjs_desc_cid)
                                     : dyn_plain_wrap(ctx, new_target, wjs_desc_cid, NULL, NULL);
    if (JS_IsException(obj)) {
        js_free(ctx, d->sdp);
        js_free(ctx, d);
        return obj;
    }
    JS_SetOpaque(obj, d);
    return obj;
}

JSValue wjs_desc_new(JSContext* ctx, int type, const char* sdp, size_t n)
{
    return wjs_desc_make(ctx, JS_UNDEFINED, type, sdp, n);
}

static JSValue wjs_desc_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst init = argc ? argv[0] : JS_UNDEFINED;
    char *type = NULL, *sdp = NULL;
    int t;
    JSValue r;
    if (!JS_IsObject(init))
        return JS_ThrowTypeError(ctx, "RTCSessionDescription: init.type is required");
    if (wjs_opt_str(ctx, init, "type", &type, 0) < 0)
        return JS_EXCEPTION;
    if (!type)
        return JS_ThrowTypeError(ctx, "RTCSessionDescription: init.type is required");
    t = wjs_sdp_type_parse(type);
    js_free(ctx, type);
    if (t < 0)
        return JS_ThrowTypeError(ctx, "RTCSessionDescription: init.type is not a valid RTCSdpType");
    if (wjs_opt_str(ctx, init, "sdp", &sdp, 0) < 0)
        return JS_EXCEPTION;
    r = wjs_desc_make(ctx, new_target, t, sdp ? sdp : "", sdp ? strlen(sdp) : 0);
    js_free(ctx, sdp);
    return r;
}

static JSValue wjs_desc_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_sd* d = JS_GetOpaque2(ctx, this_val, wjs_desc_cid);
    if (!d)
        return JS_EXCEPTION;
    return magic ? JS_NewString(ctx, d->sdp) : JS_NewString(ctx, wjs_sdp_type_name(d->type));
}

static JSValue wjs_desc_json(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue o;
    (void)argc;
    (void)argv;
    if (!JS_GetOpaque2(ctx, this_val, wjs_desc_cid))
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "type", wjs_desc_get(ctx, this_val, 0));
    JS_SetPropertyStr(ctx, o, "sdp", wjs_desc_get(ctx, this_val, 1));
    return o;
}

static const JSCFunctionListEntry wjs_desc_proto[] = {
    JS_CGETSET_MAGIC_DEF("type", wjs_desc_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("sdp", wjs_desc_get, NULL, 1),
    JS_CFUNC_DEF("toJSON", 0, wjs_desc_json),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCSessionDescription", JS_PROP_CONFIGURABLE),
};

static void wjs_cert_finalizer(JSRuntime* rt, JSValue val)
{
    wrt_cert* c = JS_GetOpaque(val, wjs_cert_cid);
    (void)rt;
    if (c)
        wrt_cert_unref(c);
}

static JSClassDef wjs_cert_class = { "RTCCertificate", .finalizer = wjs_cert_finalizer };

JSValue wjs_cert_new(JSContext* ctx, wrt_cert* c)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)wjs_cert_cid);
    if (JS_IsException(obj))
        return obj;
    wrt_cert_ref(c);
    JS_SetOpaque(obj, c);
    return obj;
}

wrt_cert* wjs_cert_get(JSContext* ctx, JSValueConst v)
{
    (void)ctx;
    return JS_GetOpaque(v, wjs_cert_cid);
}

static JSValue wjs_cert_expires(JSContext* ctx, JSValueConst this_val)
{
    wrt_cert* c = JS_GetOpaque2(ctx, this_val, wjs_cert_cid);
    return c ? JS_NewFloat64(ctx, c->expires_ms) : JS_EXCEPTION;
}

static JSValue wjs_cert_fps(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wrt_cert* c = JS_GetOpaque2(ctx, this_val, wjs_cert_cid);
    char buf[128];
    JSValue arr, o;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    wrt_cert_fp_text(c, buf, sizeof(buf));
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "algorithm", JS_NewString(ctx, "sha-256"));
    JS_SetPropertyStr(ctx, o, "value", JS_NewString(ctx, buf));
    JS_SetPropertyUint32(ctx, arr, 0, o);
    return arr;
}

static const JSCFunctionListEntry wjs_cert_proto[] = {
    JS_CGETSET_DEF("expires", wjs_cert_expires, NULL),
    JS_CFUNC_DEF("getFingerprints", 0, wjs_cert_fps),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCCertificate", JS_PROP_CONFIGURABLE),
};

int wjs_misc_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &wjs_dc_cid, &wjs_dc_class, wjs_dc_proto, countof(wjs_dc_proto), wjs_illegal,
            "RTCDataChannel")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_cand_cid, &wjs_cand_class, wjs_cand_proto, countof(wjs_cand_proto),
            wjs_cand_ctor, "RTCIceCandidate")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_desc_cid, &wjs_desc_class, wjs_desc_proto, countof(wjs_desc_proto),
            wjs_desc_ctor, "RTCSessionDescription")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_cert_cid, &wjs_cert_class, wjs_cert_proto, countof(wjs_cert_proto),
            wjs_illegal, "RTCCertificate")
        < 0)
        return -1;
    return 0;
}

static int wjs_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (wjs_misc_register(ctx, m) < 0 || wjs_media_register(ctx, m) < 0 || wjs_xport_register(ctx, m) < 0)
        return -1;
    return wjs_pc_register(ctx, m);
}

#define WJS_NGLOBALS 13

static const char* const wjs_global_names[WJS_NGLOBALS] = { "RTCPeerConnection", "RTCDataChannel", "RTCIceCandidate", "RTCSessionDescription",
    "RTCCertificate", "RTCRtpTransceiver", "RTCRtpSender", "RTCRtpReceiver", "MediaStream", "MediaStreamTrack", "RTCDtlsTransport", "RTCIceTransport", "RTCDTMFSender" };

static JSValue wjs_global_ns(JSContext* ctx)
{
    JSValue p = JS_LoadModule(ctx, "", "dyna:webrtc");
    JSModuleDef* m;
    if (JS_IsException(p))
        return p;
    if (JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(ctx, p);
        JS_FreeValue(ctx, p);
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, p);
    m = JS_FindLoadedModule(ctx, "dyna:webrtc");
    if (!m)
        return JS_ThrowInternalError(ctx, "dyna:webrtc could not be loaded");
    return JS_GetModuleNamespace(ctx, m);
}

static JSValue wjs_global_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSValue ns = wjs_global_ns(ctx), g, out;
    int i;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(ns))
        return ns;
    g = JS_GetGlobalObject(ctx);
    for (i = 0; i < WJS_NGLOBALS; i++) {
        JSValue cv = JS_GetPropertyStr(ctx, ns, wjs_global_names[i]);
        JS_DefinePropertyValueStr(ctx, g, wjs_global_names[i], cv, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    out = JS_GetPropertyStr(ctx, ns, wjs_global_names[magic]);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, ns);
    return out;
}

static JSValue wjs_global_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSValue g = JS_GetGlobalObject(ctx);
    (void)this_val;
    JS_DefinePropertyValueStr(ctx, g, wjs_global_names[magic], argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED,
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, g);
    return JS_UNDEFINED;
}

void js_webrtc_install_globals(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    int i;
    for (i = 0; i < WJS_NGLOBALS; i++) {
        JSAtom a = JS_NewAtom(ctx, wjs_global_names[i]);
        JSValue get = JS_NewCFunctionMagic(ctx, wjs_global_get, wjs_global_names[i], 0, JS_CFUNC_generic_magic, i);
        JSValue set = JS_NewCFunctionMagic(ctx, wjs_global_set, wjs_global_names[i], 1, JS_CFUNC_generic_magic, i);
        JS_DefinePropertyGetSet(ctx, g, a, get, set, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, g);
}

int js_nat_init_webrtc(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:webrtc", wjs_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "RTCPeerConnection");
    JS_AddModuleExport(ctx, m, "RTCDataChannel");
    JS_AddModuleExport(ctx, m, "RTCIceCandidate");
    JS_AddModuleExport(ctx, m, "RTCSessionDescription");
    JS_AddModuleExport(ctx, m, "RTCCertificate");
    JS_AddModuleExport(ctx, m, "RTCRtpTransceiver");
    JS_AddModuleExport(ctx, m, "RTCRtpSender");
    JS_AddModuleExport(ctx, m, "RTCRtpReceiver");
    JS_AddModuleExport(ctx, m, "MediaStream");
    JS_AddModuleExport(ctx, m, "MediaStreamTrack");
    JS_AddModuleExport(ctx, m, "RTCDtlsTransport");
    JS_AddModuleExport(ctx, m, "RTCIceTransport");
    JS_AddModuleExport(ctx, m, "RTCDTMFSender");
    return 0;
}

#endif
