#include "webrtc-js.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "modules/libc/libc-internal.h"
#include "core/dyn-prng.h"

JSClassID wjs_track_cid, wjs_stream_cid, wjs_tr_cid, wjs_sender_cid, wjs_receiver_cid;

enum {
    WJS_TE_ENDED,
    WJS_TE_MUTE,
    WJS_TE_UNMUTE,
    WJS_TE_N
};

enum {
    WJS_SE_ADD,
    WJS_SE_REMOVE,
    WJS_SE_N
};

#define WJS_DIR_STOPPED 4
#define WJS_MAX_SIMULCAST 4

static const char* const wjs_track_names[WJS_TE_N] = { "ended", "mute", "unmute" };
static const char* const wjs_stream_names[WJS_SE_N] = { "addtrack", "removetrack" };
static const char* const wjs_dir_names[] = { "sendrecv", "sendonly", "recvonly", "inactive", "stopped" };
static const char* const wjs_prio_names[] = { "very-low", "low", "medium", "high" };
static const char* const wjs_hint_names[] = { "", "speech", "speech-recognition", "music", "motion", "detail", "text" };

typedef struct {
    JSValue tr_obj;
} wjs_side;

static int wjs_len(JSContext* ctx, JSValueConst v, int64_t* out)
{
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    int r;
    if (JS_IsException(l))
        return -1;
    r = JS_ToInt64(ctx, out, l);
    if (r == 0 && *out < 0)
        *out = 0;
    if (r == 0 && *out > 0xFFFFFFFFll)
        *out = 0xFFFFFFFFll;
    JS_FreeValue(ctx, l);
    return r;
}

static JSValue wjs_idx(JSContext* ctx, JSValueConst v, int64_t i)
{
    return JS_GetPropertyUint32(ctx, v, (uint32_t)i);
}

static JSValue wjs_type_err(JSContext* ctx, const char* msg)
{
    JS_ThrowTypeError(ctx, "%s", msg);
    return JS_GetException(ctx);
}

static void wjs_uuid(char* out)
{
    static const char hx[] = "0123456789abcdef";
    uint8_t r[16];
    int i, k = 0;
    if (dyn_os_entropy(r, sizeof(r)) < 0) {
        uintptr_t a = (uintptr_t)out;
        for (i = 0; i < 16; i++)
            r[i] = (uint8_t)(a >> ((i % 8) * 8)) ^ (uint8_t)(i * 37);
    }
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[k++] = '-';
        out[k++] = hx[r[i] >> 4];
        out[k++] = hx[r[i] & 15];
    }
    out[k] = 0;
}

static uint32_t wjs_rand32(void)
{
    uint32_t v = 0;
    while (!v)
        if (dyn_os_entropy(&v, sizeof(v)) < 0)
            v = (uint32_t)(uintptr_t)&v ^ 0x9E3779B9u;
    return v;
}

static int wjs_is_sending(int d)
{
    return d == RTC_DIR_SENDRECV || d == RTC_DIR_SENDONLY;
}

static int wjs_is_receiving(int d)
{
    return d == RTC_DIR_SENDRECV || d == RTC_DIR_RECVONLY;
}

static int wjs_dir_reverse(int d)
{
    return d == RTC_DIR_SENDONLY ? RTC_DIR_RECVONLY : d == RTC_DIR_RECVONLY ? RTC_DIR_SENDONLY : d;
}

static int wjs_enum_index(JSContext* ctx, JSValueConst v, const char* const* names, int n, int* out)
{
    const char* s;
    int i;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "expected a string enum value");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < n; i++)
        if (!strcmp(s, names[i])) {
            JS_FreeCString(ctx, s);
            *out = i;
            return 0;
        }
    JS_ThrowTypeError(ctx, "'%s' is not a valid enum value", s);
    JS_FreeCString(ctx, s);
    return -1;
}

static JSValue wjs_settled(JSContext* ctx, JSValue v, int ok)
{
    JSValue res[2], prom, r;
    prom = JS_NewPromiseCapability(ctx, res);
    if (JS_IsException(prom)) {
        JS_FreeValue(ctx, v);
        return prom;
    }
    r = JS_Call(ctx, res[ok ? 0 : 1], JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, res[0]);
    JS_FreeValue(ctx, res[1]);
    return prom;
}

JSValue wjs_reject_pending(JSContext* ctx)
{
    return wjs_settled(ctx, JS_GetException(ctx), 0);
}

static JSValue wjs_reject_dom(JSContext* ctx, const char* name, const char* msg)
{
    return wjs_settled(ctx, js_dom_exception(ctx, name, msg), 0);
}

static JSValue wjs_illegal_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

wjs_track* wjs_track_get(JSValueConst v)
{
    return JS_GetOpaque(v, wjs_track_cid);
}

static wjs_track* wjs_track_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_track_cid);
}

static void wjs_track_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_track* t = JS_GetOpaque(val, wjs_track_cid);
    if (!t)
        return;
    wrt_relay_unref(t->relay);
    wjs_et_free(rt, &t->et);
    js_free_rt(rt, t->label);
    js_free_rt(rt, t);
}

static void wjs_track_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_track* t = JS_GetOpaque(val, wjs_track_cid);
    if (t)
        wjs_et_mark(rt, &t->et, mark);
}

static JSClassDef wjs_track_class = { "MediaStreamTrack", .finalizer = wjs_track_finalizer, .gc_mark = wjs_track_mark };

static JSValue wjs_track_make(JSContext* ctx, int kind, const char* label, int remote)
{
    wjs_track* t = js_mallocz(ctx, sizeof(*t));
    JSValue obj;
    size_t n = label ? strlen(label) : 0;
    int i;
    if (!t)
        return JS_EXCEPTION;
    t->label = js_malloc(ctx, n + 1);
    if (!t->label) {
        js_free(ctx, t);
        return JS_EXCEPTION;
    }
    if (n)
        memcpy(t->label, label, n);
    t->label[n] = 0;
    t->et.ntypes = WJS_TE_N;
    for (i = 0; i < WJS_PE_N; i++)
        t->et.on[i] = JS_NULL;
    t->kind = (uint8_t)kind;
    t->enabled = 1;
    t->remote = (uint8_t)remote;
    t->muted = (uint8_t)remote;
    wjs_uuid(t->id);
    if (remote && !(t->relay = wrt_relay_new())) {
        js_free(ctx, t->label);
        js_free(ctx, t);
        return JS_ThrowOutOfMemory(ctx);
    }
    obj = JS_NewObjectClass(ctx, (int)wjs_track_cid);
    if (JS_IsException(obj)) {
        wrt_relay_unref(t->relay);
        js_free(ctx, t->label);
        js_free(ctx, t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    return obj;
}

static void wjs_track_fire(JSContext* ctx, JSValueConst obj, int type)
{
    wjs_track* t = wjs_track_get(obj);
    JSValue ev;
    if (!t)
        return;
    ev = wjs_event(ctx, wjs_track_names[type], obj);
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    wjs_et_fire(ctx, obj, &t->et, type, ev);
    JS_FreeValue(ctx, ev);
}


static void wjs_track_end(JSContext* ctx, JSValueConst obj, int fire)
{
    wjs_track* t = wjs_track_get(obj);
    if (!t || t->ended)
        return;
    t->ended = 1;
    wrt_relay_end(t->relay);
    if (fire)
        wjs_track_fire(ctx, obj, WJS_TE_ENDED);
}

static JSValue wjs_track_get_prop(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, t->kind ? "video" : "audio");
    case 1:
        return JS_NewString(ctx, t->id);
    case 2:
        return JS_NewString(ctx, t->label);
    case 3:
        return JS_NewBool(ctx, t->enabled);
    case 4:
        return JS_NewBool(ctx, t->muted);
    case 5:
        return JS_NewString(ctx, t->ended ? "ended" : "live");
    default:
        return JS_NewString(ctx, wjs_hint_names[t->hint]);
    }
}

static JSValue wjs_track_set_prop(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 3) {
        t->enabled = (uint8_t)JS_ToBool(ctx, v);
        return JS_UNDEFINED;
    }
    {
        const char* s = JS_ToCString(ctx, v);
        int i, lo = t->kind ? 4 : 1, hi = t->kind ? 6 : 3;
        if (!s)
            return JS_EXCEPTION;
        if (!s[0])
            t->hint = 0;
        for (i = lo; i <= hi; i++)
            if (!strcmp(s, wjs_hint_names[i]))
                t->hint = (uint8_t)i;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue wjs_track_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    return t ? wjs_et_on_get(ctx, &t->et, magic) : JS_EXCEPTION;
}

static JSValue wjs_track_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    return t ? wjs_et_on_set(ctx, &t->et, magic, v) : JS_EXCEPTION;
}

static JSValue wjs_track_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    return t ? wjs_et_listen(ctx, &t->et, wjs_track_names, WJS_TE_N, argc, argv, magic) : JS_EXCEPTION;
}

static JSValue wjs_track_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &t->et, wjs_track_names, WJS_TE_N, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static JSValue wjs_track_clone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_track *t = wjs_track_of(ctx, this_val), *c;
    JSValue obj;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    obj = wjs_track_make(ctx, t->kind, t->label, t->remote);
    if (JS_IsException(obj))
        return obj;
    c = wjs_track_get(obj);
    c->enabled = t->enabled;
    c->muted = t->muted;
    c->ended = t->ended;
    if (c->ended)
        wrt_relay_end(c->relay);
    c->hint = t->hint;
    return obj;
}

static JSValue wjs_track_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    t->ended = 1;
    wrt_relay_end(t->relay);
    return JS_UNDEFINED;
}

static JSValue wjs_track_dict(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (magic == 2 && !JS_IsException(o))
        JS_SetPropertyStr(ctx, o, "deviceId", JS_NewString(ctx, t->id));
    return o;
}

static JSValue wjs_track_apply(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_track* t = wjs_track_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (argc && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) && !JS_IsObject(argv[0]))
        return wjs_settled(ctx, wjs_type_err(ctx, "applyConstraints: constraints must be an object"), 0);
    return wjs_settled(ctx, JS_UNDEFINED, 1);
}

static const JSCFunctionListEntry wjs_track_proto[] = {
    JS_CGETSET_MAGIC_DEF("kind", wjs_track_get_prop, NULL, 0),
    JS_CGETSET_MAGIC_DEF("id", wjs_track_get_prop, NULL, 1),
    JS_CGETSET_MAGIC_DEF("label", wjs_track_get_prop, NULL, 2),
    JS_CGETSET_MAGIC_DEF("enabled", wjs_track_get_prop, wjs_track_set_prop, 3),
    JS_CGETSET_MAGIC_DEF("muted", wjs_track_get_prop, NULL, 4),
    JS_CGETSET_MAGIC_DEF("readyState", wjs_track_get_prop, NULL, 5),
    JS_CGETSET_MAGIC_DEF("contentHint", wjs_track_get_prop, wjs_track_set_prop, 6),
    JS_CGETSET_MAGIC_DEF("onended", wjs_track_on_get, wjs_track_on_set, WJS_TE_ENDED),
    JS_CGETSET_MAGIC_DEF("onmute", wjs_track_on_get, wjs_track_on_set, WJS_TE_MUTE),
    JS_CGETSET_MAGIC_DEF("onunmute", wjs_track_on_get, wjs_track_on_set, WJS_TE_UNMUTE),
    JS_CFUNC_DEF("clone", 0, wjs_track_clone),
    JS_CFUNC_DEF("stop", 0, wjs_track_stop),
    JS_CFUNC_MAGIC_DEF("getCapabilities", 0, wjs_track_dict, 0),
    JS_CFUNC_MAGIC_DEF("getConstraints", 0, wjs_track_dict, 1),
    JS_CFUNC_MAGIC_DEF("getSettings", 0, wjs_track_dict, 2),
    JS_CFUNC_DEF("applyConstraints", 0, wjs_track_apply),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_track_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_track_listen, 1),
    JS_CFUNC_DEF("dispatchEvent", 1, wjs_track_dispatch),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "MediaStreamTrack", JS_PROP_CONFIGURABLE),
};

static wjs_stream* wjs_stream_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_stream_cid);
}

static void wjs_stream_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_stream* s = JS_GetOpaque(val, wjs_stream_cid);
    int i;
    if (!s)
        return;
    for (i = 0; i < s->n; i++)
        JS_FreeValueRT(rt, s->tracks[i]);
    js_free_rt(rt, s->tracks);
    wjs_et_free(rt, &s->et);
    js_free_rt(rt, s->id);
    js_free_rt(rt, s);
}

static void wjs_stream_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_stream* s = JS_GetOpaque(val, wjs_stream_cid);
    int i;
    if (!s)
        return;
    for (i = 0; i < s->n; i++)
        JS_MarkValue(rt, s->tracks[i], mark);
    wjs_et_mark(rt, &s->et, mark);
}

static JSClassDef wjs_stream_class = { "MediaStream", .finalizer = wjs_stream_finalizer, .gc_mark = wjs_stream_mark };

static int wjs_stream_find(const wjs_stream* s, JSValueConst track)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (JS_VALUE_GET_PTR(s->tracks[i]) == JS_VALUE_GET_PTR(track))
            return i;
    return -1;
}

static int wjs_stream_push(JSContext* ctx, wjs_stream* s, JSValueConst track)
{
    if (wjs_stream_find(s, track) >= 0)
        return 0;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        JSValue* nv = js_realloc(ctx, s->tracks, sizeof(JSValue) * (size_t)nc);
        if (!nv)
            return -1;
        s->tracks = nv;
        s->cap = nc;
    }
    s->tracks[s->n++] = JS_DupValue(ctx, track);
    return 1;
}

static int wjs_stream_drop(JSContext* ctx, wjs_stream* s, JSValueConst track)
{
    int i = wjs_stream_find(s, track);
    if (i < 0)
        return 0;
    JS_FreeValue(ctx, s->tracks[i]);
    memmove(&s->tracks[i], &s->tracks[i + 1], sizeof(JSValue) * (size_t)(s->n - i - 1));
    s->n--;
    return 1;
}

static void wjs_stream_fire(JSContext* ctx, JSValueConst obj, int type, JSValueConst track)
{
    wjs_stream* s = JS_GetOpaque(obj, wjs_stream_cid);
    JSValue ev;
    if (!s)
        return;
    ev = wjs_event(ctx, wjs_stream_names[type], obj);
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    JS_SetPropertyStr(ctx, ev, "track", JS_DupValue(ctx, track));
    wjs_et_fire(ctx, obj, &s->et, type, ev);
    JS_FreeValue(ctx, ev);
}

static JSValue wjs_stream_make(JSContext* ctx, JSValueConst new_target, const char* id)
{
    wjs_stream* s = js_mallocz(ctx, sizeof(*s));
    JSValue obj;
    int i;
    if (!s)
        return JS_EXCEPTION;
    s->et.ntypes = WJS_SE_N;
    for (i = 0; i < WJS_PE_N; i++)
        s->et.on[i] = JS_NULL;
    s->id = js_malloc(ctx, id ? strlen(id) + 1 : 40);
    if (!s->id) {
        js_free(ctx, s);
        return JS_EXCEPTION;
    }
    if (id)
        memcpy(s->id, id, strlen(id) + 1);
    else
        wjs_uuid(s->id);
    obj = JS_IsUndefined(new_target) ? JS_NewObjectClass(ctx, (int)wjs_stream_cid)
                                     : dyn_plain_wrap(ctx, new_target, wjs_stream_cid, NULL, NULL);
    if (JS_IsException(obj)) {
        js_free(ctx, s->id);
        js_free(ctx, s);
        return obj;
    }
    JS_SetOpaque(obj, s);
    return obj;
}

static JSValue wjs_stream_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValue obj;
    wjs_stream* s;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "MediaStream: constructor requires 'new'");
    obj = wjs_stream_make(ctx, new_target, NULL);
    if (JS_IsException(obj))
        return obj;
    s = JS_GetOpaque(obj, wjs_stream_cid);
    if (argc && !JS_IsUndefined(argv[0])) {
        wjs_stream* src = JS_GetOpaque(argv[0], wjs_stream_cid);
        if (src) {
            int i;
            for (i = 0; i < src->n; i++)
                if (wjs_stream_push(ctx, s, src->tracks[i]) < 0)
                    goto fail;
        } else if (JS_IsArray(ctx, argv[0])) {
            int64_t n, i;
            if (wjs_len(ctx, argv[0], &n) < 0)
                goto fail;
            for (i = 0; i < n; i++) {
                JSValue v = wjs_idx(ctx, argv[0], i);
                if (JS_IsException(v))
                    goto fail;
                if (!wjs_track_get(v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "MediaStream: every element must be a MediaStreamTrack");
                    goto fail;
                }
                if (wjs_stream_push(ctx, s, v) < 0) {
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                JS_FreeValue(ctx, v);
            }
        } else {
            JS_ThrowTypeError(ctx, "MediaStream: argument must be a MediaStream or a sequence of MediaStreamTrack");
            goto fail;
        }
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue wjs_stream_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    int i;
    if (!s)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewString(ctx, s->id);
    for (i = 0; i < s->n; i++) {
        wjs_track* t = wjs_track_get(s->tracks[i]);
        if (t && !t->ended)
            return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue wjs_stream_tracks(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    JSValue a;
    int i;
    uint32_t k = 0;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < s->n; i++) {
        wjs_track* t = wjs_track_get(s->tracks[i]);
        if (magic == 2 || (t && t->kind == (uint8_t)magic))
            JS_SetPropertyUint32(ctx, a, k++, JS_DupValue(ctx, s->tracks[i]));
    }
    return a;
}

static JSValue wjs_stream_by_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    const char* id;
    int i;
    if (!s)
        return JS_EXCEPTION;
    id = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!id)
        return JS_EXCEPTION;
    for (i = 0; i < s->n; i++) {
        wjs_track* t = wjs_track_get(s->tracks[i]);
        if (t && !strcmp(t->id, id)) {
            JS_FreeCString(ctx, id);
            return JS_DupValue(ctx, s->tracks[i]);
        }
    }
    JS_FreeCString(ctx, id);
    return JS_NULL;
}

static JSValue wjs_stream_change(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    JSValueConst tr = argc ? argv[0] : JS_UNDEFINED;
    if (!s)
        return JS_EXCEPTION;
    if (!wjs_track_get(tr))
        return JS_ThrowTypeError(ctx, "MediaStream.%s: argument is not a MediaStreamTrack", magic ? "removeTrack" : "addTrack");
    if (magic)
        wjs_stream_drop(ctx, s, tr);
    else if (wjs_stream_push(ctx, s, tr) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue wjs_stream_clone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_stream *s = wjs_stream_of(ctx, this_val), *c;
    JSValue obj;
    int i;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    obj = wjs_stream_make(ctx, JS_UNDEFINED, NULL);
    if (JS_IsException(obj))
        return obj;
    c = JS_GetOpaque(obj, wjs_stream_cid);
    for (i = 0; i < s->n; i++) {
        JSValue t = wjs_track_clone(ctx, s->tracks[i], 0, NULL);
        if (JS_IsException(t) || wjs_stream_push(ctx, c, t) < 0) {
            JS_FreeValue(ctx, t);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, t);
    }
    return obj;
}

static JSValue wjs_stream_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    return s ? wjs_et_on_get(ctx, &s->et, magic) : JS_EXCEPTION;
}

static JSValue wjs_stream_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    return s ? wjs_et_on_set(ctx, &s->et, magic, v) : JS_EXCEPTION;
}

static JSValue wjs_stream_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    return s ? wjs_et_listen(ctx, &s->et, wjs_stream_names, WJS_SE_N, argc, argv, magic) : JS_EXCEPTION;
}

static JSValue wjs_stream_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_stream* s = wjs_stream_of(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &s->et, wjs_stream_names, WJS_SE_N, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static const JSCFunctionListEntry wjs_stream_proto[] = {
    JS_CGETSET_MAGIC_DEF("id", wjs_stream_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("active", wjs_stream_get, NULL, 1),
    JS_CFUNC_MAGIC_DEF("getAudioTracks", 0, wjs_stream_tracks, 0),
    JS_CFUNC_MAGIC_DEF("getVideoTracks", 0, wjs_stream_tracks, 1),
    JS_CFUNC_MAGIC_DEF("getTracks", 0, wjs_stream_tracks, 2),
    JS_CFUNC_DEF("getTrackById", 1, wjs_stream_by_id),
    JS_CFUNC_MAGIC_DEF("addTrack", 1, wjs_stream_change, 0),
    JS_CFUNC_MAGIC_DEF("removeTrack", 1, wjs_stream_change, 1),
    JS_CFUNC_DEF("clone", 0, wjs_stream_clone),
    JS_CGETSET_MAGIC_DEF("onaddtrack", wjs_stream_on_get, wjs_stream_on_set, WJS_SE_ADD),
    JS_CGETSET_MAGIC_DEF("onremovetrack", wjs_stream_on_get, wjs_stream_on_set, WJS_SE_REMOVE),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_stream_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_stream_listen, 1),
    JS_CFUNC_DEF("dispatchEvent", 1, wjs_stream_dispatch),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "MediaStream", JS_PROP_CONFIGURABLE),
};

wjs_tr* wjs_tr_get(JSValueConst v)
{
    return JS_GetOpaque(v, wjs_tr_cid);
}

static void wjs_codecs_clear(JSRuntime* rt, wjs_codecs* c)
{
    js_free_rt(rt, c->v);
    js_free_rt(rt, c->x);
    js_free_rt(rt, c->blob);
    memset(c, 0, sizeof(*c));
}

static void wjs_enc_one_free(JSRuntime* rt, wjs_enc* e)
{
    js_free_rt(rt, e->rid);
    if (e->codec)
        js_free_rt(rt, e->codec->fmtp);
    js_free_rt(rt, e->codec);
    e->rid = NULL;
    e->codec = NULL;
}

static void wjs_enc_clear(JSRuntime* rt, wjs_tr* t)
{
    int i;
    for (i = 0; i < t->nenc; i++)
        wjs_enc_one_free(rt, &t->enc[i]);
    js_free_rt(rt, t->enc);
    t->enc = NULL;
    t->nenc = 0;
}

static void wjs_sids_clear(JSRuntime* rt, wjs_tr* t)
{
    int i;
    for (i = 0; i < t->nsids; i++)
        js_free_rt(rt, t->sids[i]);
    js_free_rt(rt, t->sids);
    t->sids = NULL;
    t->nsids = 0;
}

static void wjs_tr_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_tr* t = JS_GetOpaque(val, wjs_tr_cid);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->pc_obj);
    JS_FreeValueRT(rt, t->sender);
    JS_FreeValueRT(rt, t->receiver);
    JS_FreeValueRT(rt, t->send_track);
    JS_FreeValueRT(rt, t->recv_track);
    JS_FreeValueRT(rt, t->rstreams);
    JS_FreeValueRT(rt, t->clear_fn);
    JS_FreeValueRT(rt, t->dtmf);
    JS_FreeValueRT(rt, t->snap_streams);
    wjs_sids_clear(rt, t);
    wjs_enc_clear(rt, t);
    wjs_codecs_clear(rt, &t->prefs);
    wjs_codecs_clear(rt, &t->neg);
    js_free_rt(rt, t);
}

static void wjs_tr_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_tr* t = JS_GetOpaque(val, wjs_tr_cid);
    if (!t)
        return;
    JS_MarkValue(rt, t->pc_obj, mark);
    JS_MarkValue(rt, t->sender, mark);
    JS_MarkValue(rt, t->receiver, mark);
    JS_MarkValue(rt, t->send_track, mark);
    JS_MarkValue(rt, t->recv_track, mark);
    JS_MarkValue(rt, t->rstreams, mark);
    JS_MarkValue(rt, t->clear_fn, mark);
    JS_MarkValue(rt, t->dtmf, mark);
    JS_MarkValue(rt, t->snap_streams, mark);
}

static JSClassDef wjs_tr_class = { "RTCRtpTransceiver", .finalizer = wjs_tr_finalizer, .gc_mark = wjs_tr_mark };

static void wjs_side_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_side* s = JS_GetOpaque(val, JS_GetClassID(val));
    if (!s)
        return;
    JS_FreeValueRT(rt, s->tr_obj);
    js_free_rt(rt, s);
}

static void wjs_side_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_side* s = JS_GetOpaque(val, JS_GetClassID(val));
    if (s)
        JS_MarkValue(rt, s->tr_obj, mark);
}

static JSClassDef wjs_sender_class = { "RTCRtpSender", .finalizer = wjs_side_finalizer, .gc_mark = wjs_side_mark };
static JSClassDef wjs_receiver_class = { "RTCRtpReceiver", .finalizer = wjs_side_finalizer, .gc_mark = wjs_side_mark };

static wjs_tr* wjs_side_tr(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    wjs_side* s = JS_GetOpaque2(ctx, v, cid);
    return s ? wjs_tr_get(s->tr_obj) : NULL;
}

static wjs_tr* wjs_tr_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_tr_cid);
}

static JSValue wjs_side_new(JSContext* ctx, JSClassID cid, JSValueConst tr_obj)
{
    wjs_side* s = js_mallocz(ctx, sizeof(*s));
    JSValue obj;
    if (!s)
        return JS_EXCEPTION;
    obj = JS_NewObjectClass(ctx, (int)cid);
    if (JS_IsException(obj)) {
        js_free(ctx, s);
        return obj;
    }
    s->tr_obj = JS_DupValue(ctx, tr_obj);
    JS_SetOpaque(obj, s);
    return obj;
}

wjs_tr* wjs_tr_by_id(wjs_pc* p, uint32_t id)
{
    int i;
    for (i = 0; i < p->ntr && id; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t && t->id == id && !t->removed)
            return t;
    }
    return NULL;
}

wjs_tr* wjs_tr_by_mid(wjs_pc* p, const char* mid)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t && t->has_mid && !t->removed && !strcmp(t->mid, mid))
            return t;
    }
    return NULL;
}

static int wjs_enc_default(JSContext* ctx, wjs_tr* t)
{
    t->enc = js_mallocz(ctx, sizeof(wjs_enc));
    if (!t->enc)
        return -1;
    t->nenc = 1;
    t->enc[0].active = 1;
    t->enc[0].priority = 1;
    t->enc[0].net_priority = 1;
    if (t->kind) {
        t->enc[0].has_scale = 1;
        t->enc[0].scale = 1.0;
    }
    return 0;
}

JSValue wjs_tr_create(JSContext* ctx, wjs_pc* p, int kind, int dir, JSValueConst track)
{
    wjs_tr* t = js_mallocz(ctx, sizeof(*t));
    JSValue obj;
    if (!t)
        return JS_EXCEPTION;
    t->ctx = ctx;
    t->pc = p;
    t->kind = (uint8_t)kind;
    t->dir = (uint8_t)dir;
    t->fired_dir = RTC_DIR_INACTIVE;
    t->id = ++p->next_trid;
    t->ssrc = wjs_rand32();
    do
        t->rtx_ssrc = wjs_rand32();
    while (t->rtx_ssrc == t->ssrc);
    t->pc_obj = JS_DupValue(ctx, p->self);
    t->sender = t->receiver = t->recv_track = t->rstreams = JS_UNDEFINED;
    t->clear_fn = JS_UNDEFINED;
    t->dtmf = t->snap_streams = JS_UNDEFINED;
    t->send_track = JS_IsUndefined(track) ? JS_NULL : JS_DupValue(ctx, track);
    obj = JS_NewObjectClass(ctx, (int)wjs_tr_cid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, t->pc_obj);
        JS_FreeValue(ctx, t->send_track);
        js_free(ctx, t);
        return obj;
    }
    JS_SetOpaque(obj, t);
    t->self = obj;
    if (wjs_enc_default(ctx, t) < 0)
        goto fail;
    t->sender = wjs_side_new(ctx, wjs_sender_cid, obj);
    t->receiver = wjs_side_new(ctx, wjs_receiver_cid, obj);
    t->recv_track = wjs_track_make(ctx, kind, "", 1);
    t->rstreams = JS_NewArray(ctx);
    if (JS_IsException(t->sender) || JS_IsException(t->receiver) || JS_IsException(t->recv_track) || JS_IsException(t->rstreams))
        goto fail;
    if (p->ntr == p->cap_tr) {
        int nc = p->cap_tr ? p->cap_tr * 2 : 4;
        JSValue* nv = js_realloc(ctx, p->trs, sizeof(JSValue) * (size_t)nc);
        if (!nv)
            goto fail;
        p->trs = nv;
        p->cap_tr = nc;
    }
    p->trs[p->ntr++] = JS_DupValue(ctx, obj);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static uint32_t wjs_tr_msid_hash(const wjs_tr* t)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < t->nsids; i++) {
        const char* s = t->sids[i];
        for (; *s; s++)
            h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;
    }
    return h;
}

static int wjs_set_sids(JSContext* ctx, wjs_tr* t, int argc, JSValueConst* argv)
{
    char** v = NULL;
    int n = 0, i, j;
    if (argc) {
        v = js_mallocz(ctx, sizeof(char*) * (size_t)argc);
        if (!v)
            return -1;
    }
    for (i = 0; i < argc; i++) {
        wjs_stream* s = JS_GetOpaque(argv[i], wjs_stream_cid);
        int dup = 0;
        if (!s)
            continue;
        for (j = 0; j < n; j++)
            if (!strcmp(v[j], s->id))
                dup = 1;
        if (dup)
            continue;
        v[n] = js_malloc(ctx, strlen(s->id) + 1);
        if (!v[n]) {
            for (j = 0; j < n; j++)
                js_free(ctx, v[j]);
            js_free(ctx, v);
            return -1;
        }
        memcpy(v[n], s->id, strlen(s->id) + 1);
        n++;
    }
    wjs_sids_clear(JS_GetRuntime(ctx), t);
    t->sids = v;
    t->nsids = n;
    return 0;
}

static int wjs_check_streams(JSContext* ctx, int argc, JSValueConst* argv)
{
    int i;
    for (i = 0; i < argc; i++)
        if (!JS_GetOpaque(argv[i], wjs_stream_cid)) {
            JS_ThrowTypeError(ctx, "argument %d is not a MediaStream", i + 1);
            return -1;
        }
    return 0;
}

static const rtc_sdp_codec* wjs_caps(int kind, int* n)
{
    const rtc_jsep_caps* c = rtc_jsep_default_caps();
    *n = kind ? c->nvideo : c->naudio;
    return kind ? c->video : c->audio;
}

static int wjs_is_aux(const char* name)
{
    static const char* const aux[] = { "rtx", "red", "ulpfec", "flexfec-03", "CN" };
    size_t i;
    for (i = 0; i < sizeof(aux) / sizeof(aux[0]); i++)
        if (!strcasecmp(name, aux[i]))
            return 1;
    return 0;
}

static JSValue wjs_codec_dict(JSContext* ctx, int kind, const rtc_sdp_codec* c, int with_pt, int fmtp)
{
    JSValue o = JS_NewObject(ctx);
    char mt[96];
    if (JS_IsException(o))
        return o;
    if (with_pt)
        JS_SetPropertyStr(ctx, o, "payloadType", JS_NewInt32(ctx, c->pt));
    snprintf(mt, sizeof(mt), "%s/%s", kind ? "video" : "audio", c->name ? c->name : "");
    JS_SetPropertyStr(ctx, o, "mimeType", JS_NewString(ctx, mt));
    JS_SetPropertyStr(ctx, o, "clockRate", JS_NewUint32(ctx, c->clock));
    if (!kind)
        JS_SetPropertyStr(ctx, o, "channels", JS_NewInt32(ctx, c->channels ? c->channels : 1));
    if (fmtp && c->fmtp && c->fmtp[0])
        JS_SetPropertyStr(ctx, o, "sdpFmtpLine", JS_NewString(ctx, c->fmtp));
    return o;
}

static JSValue wjs_capabilities(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const rtc_jsep_caps* caps = rtc_jsep_default_caps();
    const rtc_sdp_codec* cs;
    const char* k;
    int kind, n, i, rtx = 0;
    uint32_t a = 0;
    JSValue o, codecs, ext;
    (void)this_val;
    k = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!k)
        return JS_EXCEPTION;
    kind = !strcmp(k, "audio") ? 0 : !strcmp(k, "video") ? 1 : -1;
    JS_FreeCString(ctx, k);
    if (kind < 0)
        return JS_NULL;
    cs = wjs_caps(kind, &n);
    o = JS_NewObject(ctx);
    codecs = JS_NewArray(ctx);
    ext = JS_NewArray(ctx);
    for (i = 0; i < n; i++) {
        int is_rtx = !strcasecmp(cs[i].name, "rtx");
        if (is_rtx && rtx++)
            continue;
        JS_SetPropertyUint32(ctx, codecs, a++, wjs_codec_dict(ctx, kind, &cs[i], 0, !is_rtx));
    }
    a = 0;
    for (i = 0; i < caps->next; i++)
        if (!caps->ext_kinds || (caps->ext_kinds[i] & (kind ? 2 : 1))) {
            JSValue e = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, e, "uri", JS_NewString(ctx, caps->ext[i]));
            JS_SetPropertyUint32(ctx, ext, a++, e);
        }
    JS_SetPropertyStr(ctx, o, "codecs", codecs);
    JS_SetPropertyStr(ctx, o, "headerExtensions", ext);
    return o;
}

static int wjs_read_cpref(JSContext* ctx, JSValueConst o, wjs_cpref* c)
{
    JSValue v;
    const char* s;
    memset(c, 0, sizeof(*c));
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "setCodecPreferences: every codec must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, o, "mimeType");
    if (JS_IsUndefined(v)) {
        JS_ThrowTypeError(ctx, "setCodecPreferences: mimeType is required");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    snprintf(c->mime, sizeof(c->mime), "%s", s);
    JS_FreeCString(ctx, s);
    v = JS_GetPropertyStr(ctx, o, "clockRate");
    if (JS_IsUndefined(v)) {
        JS_ThrowTypeError(ctx, "setCodecPreferences: clockRate is required");
        return -1;
    }
    if (JS_ToFloat64(ctx, &c->clock, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "channels");
    if (!JS_IsUndefined(v)) {
        int32_t ch;
        if (JS_ToInt32(ctx, &ch, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        c->channels = ch;
        c->has_ch = 1;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "sdpFmtpLine");
    if (!JS_IsUndefined(v)) {
        s = JS_ToCString(ctx, v);
        if (!s) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        c->fmtp = js_malloc(ctx, strlen(s) + 1);
        if (c->fmtp)
            memcpy(c->fmtp, s, strlen(s) + 1);
        JS_FreeCString(ctx, s);
        c->has_fmtp = 1;
        if (!c->fmtp) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int wjs_cpref_match(const wjs_cpref* c, int kind, const rtc_sdp_codec* cap, int rtx_dedup)
{
    char mt[96];
    int cap_ch = kind ? -1 : (cap->channels ? cap->channels : 1);
    int cap_has_fmtp = cap->fmtp && cap->fmtp[0] && !(rtx_dedup && !strcasecmp(cap->name, "rtx"));
    snprintf(mt, sizeof(mt), "%s/%s", kind ? "video" : "audio", cap->name);
    if (strcasecmp(mt, c->mime) || c->clock != (double)cap->clock)
        return 0;
    if (kind ? c->has_ch : (!c->has_ch || c->channels != cap_ch))
        return 0;
    if (c->has_fmtp != cap_has_fmtp || (cap_has_fmtp && strcmp(c->fmtp, cap->fmtp)))
        return 0;
    return 1;
}

static int wjs_enc_codec_in(const wjs_cpref* c, int kind, const rtc_sdp_codec* v, int n, int caps)
{
    int k;
    for (k = 0; k < n; k++)
        if (wjs_cpref_match(c, kind, &v[k], caps))
            return 1;
    return 0;
}

static JSValue wjs_cpref_dict(JSContext* ctx, const wjs_cpref* c)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "mimeType", JS_NewString(ctx, c->mime));
    JS_SetPropertyStr(ctx, o, "clockRate", JS_NewFloat64(ctx, c->clock));
    if (c->has_ch)
        JS_SetPropertyStr(ctx, o, "channels", JS_NewInt32(ctx, c->channels));
    if (c->has_fmtp)
        JS_SetPropertyStr(ctx, o, "sdpFmtpLine", JS_NewString(ctx, c->fmtp ? c->fmtp : ""));
    return o;
}

static JSValue wjs_tr_set_prefs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_tr_of(ctx, this_val);
    const rtc_sdp_codec* cs;
    int ncs, j, k, primaries = 0, want_rtx = 0, nout = 0;
    int64_t n = 0, i;
    uint8_t* used = NULL;
    rtc_sdp_codec* out = NULL;
    JSValue r = JS_EXCEPTION;
    if (!t)
        return JS_EXCEPTION;
    if (!argc || !JS_IsObject(argv[0]) || wjs_len(ctx, argv[0], &n) < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "setCodecPreferences: codecs must be a sequence");
        return JS_EXCEPTION;
    }
    if (n == 0) {
        wjs_codecs_clear(JS_GetRuntime(ctx), &t->prefs);
        return JS_UNDEFINED;
    }
    cs = wjs_caps(t->kind, &ncs);
    used = js_mallocz(ctx, (size_t)ncs);
    out = js_mallocz(ctx, sizeof(*out) * (size_t)ncs);
    if (!used || !out)
        goto done;
    for (i = 0; i < n; i++) {
        wjs_cpref pr;
        JSValue v = wjs_idx(ctx, argv[0], i);
        int rc = JS_IsException(v) ? -1 : wjs_read_cpref(ctx, v, &pr), found = 0;
        JS_FreeValue(ctx, v);
        if (rc < 0)
            goto done;
        for (k = 0; k < ncs; k++)
            if (wjs_cpref_match(&pr, t->kind, &cs[k], 1)) {
                found = 1;
                if (!used[k] && !wjs_is_aux(cs[k].name)) {
                    used[k] = 1;
                    out[nout++] = cs[k];
                    primaries++;
                }
            }
        if (!found) {
            js_throw_dom_exception(ctx, "InvalidModificationError", "setCodecPreferences: %s is not in RTCRtpReceiver.getCapabilities('%s')",
                pr.mime, t->kind ? "video" : "audio");
            js_free(ctx, pr.fmtp);
            goto done;
        }
        if (!strcasecmp(pr.mime + 6, "rtx"))
            want_rtx = 1;
        js_free(ctx, pr.fmtp);
    }
    if (!primaries) {
        js_throw_dom_exception(ctx, "InvalidModificationError",
            "setCodecPreferences: only RTX, RED, FEC or comfort-noise codecs are listed");
        goto done;
    }
    if (want_rtx) {
        int base = nout;
        for (j = 0; j < base; j++)
            for (k = 0; k < ncs; k++)
                if (!used[k] && cs[k].rtx_apt_set && cs[k].rtx_apt == out[j].pt) {
                    used[k] = 1;
                    out[nout++] = cs[k];
                }
    }
    wjs_codecs_clear(JS_GetRuntime(ctx), &t->prefs);
    t->prefs.v = out;
    t->prefs.n = nout;
    out = NULL;
    r = JS_UNDEFINED;
done:
    js_free(ctx, used);
    js_free(ctx, out);
    return r;
}

static void wjs_tr_stop_local(JSContext* ctx, wjs_tr* t)
{
    t->stopping = 1;
    t->dir = WJS_DIR_STOPPED;
    wjs_track_end(ctx, t->recv_track, 0);
    t->receptive = 0;
    if (t->pc)
        wjs_media_sync(ctx, t->pc);
}

static JSValue wjs_ended_cb(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    wjs_track_fire(ctx, data[0], WJS_TE_ENDED);
    return JS_UNDEFINED;
}

static JSValue wjs_tr_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_tr_of(ctx, this_val);
    wjs_track* rt;
    int fire;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    if (t->pc->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "RTCRtpTransceiver.stop: the RTCPeerConnection is closed");
    if (t->stopping)
        return JS_UNDEFINED;
    rt = wjs_track_get(t->recv_track);
    fire = rt && !rt->ended;
    wjs_tr_stop_local(ctx, t);
    if (fire) {
        JSValueConst a[2];
        JSValue fn = JS_NewCFunctionData(ctx, wjs_ended_cb, 0, 0, 1, (JSValueConst*)&t->recv_track), timer;
        if (JS_IsException(fn))
            return fn;
        a[0] = fn;
        a[1] = JS_NewInt32(ctx, 0);
        timer = js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(timer))
            return timer;
        JS_FreeValue(ctx, timer);
    }
    wjs_pc_nneeded(ctx, t->pc);
    return JS_UNDEFINED;
}

static JSValue wjs_tr_get_prop(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_tr* t = wjs_tr_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return t->has_mid ? JS_NewString(ctx, t->mid) : JS_NULL;
    case 1:
        return JS_DupValue(ctx, t->sender);
    case 2:
        return JS_DupValue(ctx, t->receiver);
    case 3:
        return JS_NewString(ctx, wjs_dir_names[t->dir]);
    default:
        return t->has_cur ? JS_NewString(ctx, wjs_dir_names[t->cur_dir]) : JS_NULL;
    }
}

static JSValue wjs_tr_set_dir(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_tr* t = wjs_tr_of(ctx, this_val);
    const char* s;
    int i, d = -1;
    (void)magic;
    if (!t)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, v);
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < 5; i++)
        if (!strcmp(s, wjs_dir_names[i]))
            d = i;
    JS_FreeCString(ctx, s);
    if (d < 0)
        return JS_UNDEFINED;
    if (t->stopping)
        return js_throw_dom_exception(ctx, "InvalidStateError", "RTCRtpTransceiver.direction: the transceiver is stopping or stopped");
    if (d == WJS_DIR_STOPPED)
        return JS_ThrowTypeError(ctx, "RTCRtpTransceiver.direction: 'stopped' cannot be set; call stop()");
    if (d == t->dir)
        return JS_UNDEFINED;
    t->dir = (uint8_t)d;
    wjs_pc_nneeded(ctx, t->pc);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry wjs_tr_proto[] = {
    JS_CGETSET_MAGIC_DEF("mid", wjs_tr_get_prop, NULL, 0),
    JS_CGETSET_MAGIC_DEF("sender", wjs_tr_get_prop, NULL, 1),
    JS_CGETSET_MAGIC_DEF("receiver", wjs_tr_get_prop, NULL, 2),
    JS_CGETSET_MAGIC_DEF("direction", wjs_tr_get_prop, wjs_tr_set_dir, 3),
    JS_CGETSET_MAGIC_DEF("currentDirection", wjs_tr_get_prop, NULL, 4),
    JS_CFUNC_DEF("stop", 0, wjs_tr_stop),
    JS_CFUNC_DEF("setCodecPreferences", 1, wjs_tr_set_prefs),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCRtpTransceiver", JS_PROP_CONFIGURABLE),
};

static JSValue wjs_neg_params(JSContext* ctx, wjs_tr* t, JSValue o)
{
    JSValue codecs = JS_NewArray(ctx), ext = JS_NewArray(ctx), rtcp = JS_NewObject(ctx);
    int i;
    for (i = 0; i < t->neg.n; i++)
        JS_SetPropertyUint32(ctx, codecs, (uint32_t)i, wjs_codec_dict(ctx, t->kind, &t->neg.v[i], 1, 1));
    for (i = 0; i < t->neg.nx; i++) {
        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, e, "uri", JS_NewString(ctx, t->neg.x[i].uri));
        JS_SetPropertyStr(ctx, e, "id", JS_NewInt32(ctx, t->neg.x[i].id));
        JS_SetPropertyStr(ctx, e, "encrypted", JS_FALSE);
        JS_SetPropertyUint32(ctx, ext, (uint32_t)i, e);
    }
    JS_SetPropertyStr(ctx, rtcp, "cname", JS_NewString(ctx, t->pc->cname));
    JS_SetPropertyStr(ctx, rtcp, "reducedSize", JS_NewBool(ctx, t->rsize));
    JS_SetPropertyStr(ctx, o, "headerExtensions", ext);
    JS_SetPropertyStr(ctx, o, "rtcp", rtcp);
    JS_SetPropertyStr(ctx, o, "codecs", codecs);
    return o;
}

static JSValue wjs_clear_last(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    wjs_tr* t = wjs_tr_get(data[0]);
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (t)
        t->has_last = 0;
    (void)ctx;
    return JS_UNDEFINED;
}

static void wjs_tid(const wjs_tr* t, char* out, size_t cap)
{
    snprintf(out, cap, "%u.%u", (unsigned)t->id, (unsigned)t->tid_seq);
}

static JSValue wjs_sender_params(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_sender_cid);
    JSValue o, encs;
    char tid[32];
    int i;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    if (!t->has_last) {
        JSValueConst a[2];
        JSValue timer;
        t->tid_seq++;
        t->has_last = 1;
        if (JS_IsUndefined(t->clear_fn)) {
            t->clear_fn = JS_NewCFunctionData(ctx, wjs_clear_last, 0, 0, 1, (JSValueConst*)&t->self);
            if (JS_IsException(t->clear_fn)) {
                t->clear_fn = JS_UNDEFINED;
                return JS_EXCEPTION;
            }
        }
        a[0] = t->clear_fn;
        a[1] = JS_NewInt32(ctx, 0);
        timer = js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
        if (JS_IsException(timer))
            return timer;
        JS_FreeValue(ctx, timer);
    }
    wjs_tid(t, tid, sizeof(tid));
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "transactionId", JS_NewString(ctx, tid));
    encs = JS_NewArray(ctx);
    for (i = 0; i < t->nenc; i++) {
        const wjs_enc* e = &t->enc[i];
        JSValue x = JS_NewObject(ctx);
        if (e->rid)
            JS_SetPropertyStr(ctx, x, "rid", JS_NewString(ctx, e->rid));
        JS_SetPropertyStr(ctx, x, "active", JS_NewBool(ctx, e->active));
        if (e->has_bitrate)
            JS_SetPropertyStr(ctx, x, "maxBitrate", JS_NewFloat64(ctx, e->max_bitrate));
        if (t->kind && e->has_framerate)
            JS_SetPropertyStr(ctx, x, "maxFramerate", JS_NewFloat64(ctx, e->max_framerate));
        if (t->kind && e->has_scale)
            JS_SetPropertyStr(ctx, x, "scaleResolutionDownBy", JS_NewFloat64(ctx, e->scale));
        JS_SetPropertyStr(ctx, x, "priority", JS_NewString(ctx, wjs_prio_names[e->priority & 3]));
        JS_SetPropertyStr(ctx, x, "networkPriority", JS_NewString(ctx, wjs_prio_names[e->net_priority & 3]));
        if (e->codec)
            JS_SetPropertyStr(ctx, x, "codec", wjs_cpref_dict(ctx, e->codec));
        JS_SetPropertyUint32(ctx, encs, (uint32_t)i, x);
    }
    JS_SetPropertyStr(ctx, o, "encodings", encs);
    return wjs_neg_params(ctx, t, o);
}

static JSValue wjs_receiver_params(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_receiver_cid);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    return wjs_neg_params(ctx, t, o);
}

static int wjs_get_member(JSContext* ctx, JSValueConst o, const char* k, JSValue* out, int required, const char* where)
{
    *out = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(*out))
        return -1;
    if (required && JS_IsUndefined(*out)) {
        JS_ThrowTypeError(ctx, "%s: '%s' is required", where, k);
        return -1;
    }
    return 0;
}

static int wjs_num_member(JSContext* ctx, JSValueConst o, const char* k, double* out, uint8_t* has)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int r = 0;
    if (JS_IsException(v))
        return -1;
    *has = 0;
    if (!JS_IsUndefined(v)) {
        r = JS_ToFloat64(ctx, out, v);
        if (r == 0 && !isfinite(*out)) {
            JS_ThrowTypeError(ctx, "%s must be a finite number", k);
            r = -1;
        }
        *has = r == 0;
    }
    JS_FreeValue(ctx, v);
    return r;
}

static int wjs_enum_member(JSContext* ctx, JSValueConst o, const char* k, const char* const* names, int n, uint8_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int r = 0, idx;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        r = wjs_enum_index(ctx, v, names, n, &idx);
        if (r == 0)
            *out = (uint8_t)idx;
    }
    JS_FreeValue(ctx, v);
    return r;
}

static int wjs_rid_ok(const char* s)
{
    size_t n = strlen(s), i;
    if (!n || n > 255)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int wjs_read_enc(JSContext* ctx, JSValueConst o, wjs_enc* e, int with_rid)
{
    JSValue v;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "encodings: every encoding must be an object");
        return -1;
    }
    e->active = 1;
    e->priority = 1;
    e->net_priority = 1;
    if (with_rid) {
        v = JS_GetPropertyStr(ctx, o, "rid");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            const char* s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s)
                return -1;
            e->rid = js_malloc(ctx, strlen(s) + 1);
            if (e->rid)
                memcpy(e->rid, s, strlen(s) + 1);
            JS_FreeCString(ctx, s);
            if (!e->rid)
                return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, o, "active");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v))
        e->active = (uint8_t)JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "codec");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        wjs_cpref* c = js_mallocz(ctx, sizeof(*c));
        if (!c || wjs_read_cpref(ctx, v, c) < 0) {
            if (c)
                js_free(ctx, c->fmtp);
            js_free(ctx, c);
            JS_FreeValue(ctx, v);
            return -1;
        }
        e->codec = c;
    }
    JS_FreeValue(ctx, v);
    if (wjs_num_member(ctx, o, "maxBitrate", &e->max_bitrate, &e->has_bitrate) < 0
        || wjs_num_member(ctx, o, "maxFramerate", &e->max_framerate, &e->has_framerate) < 0
        || wjs_num_member(ctx, o, "scaleResolutionDownBy", &e->scale, &e->has_scale) < 0
        || wjs_enum_member(ctx, o, "priority", wjs_prio_names, 4, &e->priority) < 0
        || wjs_enum_member(ctx, o, "networkPriority", wjs_prio_names, 4, &e->net_priority) < 0)
        return -1;
    if (e->has_bitrate && e->max_bitrate < 0) {
        JS_ThrowTypeError(ctx, "maxBitrate must be an unsigned long");
        return -1;
    }
    return 0;
}

static int wjs_enc_ranges(JSContext* ctx, int kind, wjs_enc* e, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!kind) {
            e[i].has_scale = 0;
            e[i].has_framerate = 0;
            continue;
        }
        if (e[i].has_scale && e[i].scale < 1.0) {
            JS_ThrowRangeError(ctx, "scaleResolutionDownBy must be 1.0 or more");
            return -1;
        }
        if (e[i].has_framerate && e[i].max_framerate < 0.0) {
            JS_ThrowRangeError(ctx, "maxFramerate must be 0 or more");
            return -1;
        }
    }
    return 0;
}

static void wjs_enc_free(JSContext* ctx, wjs_enc* e, int n)
{
    int i;
    for (i = 0; i < n; i++)
        wjs_enc_one_free(JS_GetRuntime(ctx), &e[i]);
    js_free(ctx, e);
}

static int wjs_send_encodings(JSContext* ctx, JSValueConst arr, int kind, wjs_enc** out, int* nout)
{
    int64_t n, i;
    int j, k, with_rid = 0, any_scale = 0;
    wjs_enc* e;
    *out = NULL;
    *nout = 0;
    if (!JS_IsObject(arr) || wjs_len(ctx, arr, &n) < 0) {
        JS_ThrowTypeError(ctx, "addTransceiver: sendEncodings must be a sequence");
        return -1;
    }
    if (n == 0)
        return 0;
    if (n > 1024) {
        JS_ThrowRangeError(ctx, "addTransceiver: too many sendEncodings");
        return -1;
    }
    e = js_mallocz(ctx, sizeof(*e) * (size_t)n);
    if (!e)
        return -1;
    for (i = 0; i < n; i++) {
        JSValue v = wjs_idx(ctx, arr, i);
        int rc = JS_IsException(v) ? -1 : wjs_read_enc(ctx, v, &e[i], 1);
        JS_FreeValue(ctx, v);
        if (rc < 0)
            goto fail;
        if (e[i].rid)
            with_rid++;
    }
    for (j = 0; j < n; j++) {
        if (e[j].rid && !wjs_rid_ok(e[j].rid)) {
            JS_ThrowTypeError(ctx, "addTransceiver: rid '%s' does not follow RFC 8851 section 10", e[j].rid);
            goto fail;
        }
        for (k = 0; k < j; k++)
            if (e[j].rid && e[k].rid && !strcmp(e[j].rid, e[k].rid)) {
                JS_ThrowTypeError(ctx, "addTransceiver: duplicate rid '%s'", e[j].rid);
                goto fail;
            }
    }
    if (with_rid && with_rid != n) {
        JS_ThrowTypeError(ctx, "addTransceiver: some but not all sendEncodings have a rid");
        goto fail;
    }
    for (j = 0; j < n; j++)
        if (e[j].codec) {
            int nc;
            const rtc_sdp_codec* cs = wjs_caps(kind, &nc);
            if (!wjs_enc_codec_in(e[j].codec, kind, cs, nc, 1)) {
                js_throw_dom_exception(ctx, "OperationError", "addTransceiver: encoding codec %s is not in RTCRtpSender.getCapabilities()", e[j].codec->mime);
                goto fail;
            }
        }
    if (wjs_enc_ranges(ctx, kind, e, (int)n) < 0)
        goto fail;
    k = kind ? WJS_MAX_SIMULCAST : 1;
    while (n > k)
        wjs_enc_one_free(JS_GetRuntime(ctx), &e[--n]);
    if (n == 1) {
        js_free(ctx, e[0].rid);
        e[0].rid = NULL;
    }
    for (j = 0; j < n; j++)
        any_scale |= e[j].has_scale;
    if (kind)
        for (j = 0; j < n; j++)
            if (!e[j].has_scale) {
                e[j].has_scale = 1;
                e[j].scale = any_scale ? 1.0 : ldexp(1.0, (int)n - j - 1);
            }
    *out = e;
    *nout = (int)n;
    return 0;
fail:
    wjs_enc_free(ctx, e, (int)n);
    return -1;
}

static int wjs_same_str(JSContext* ctx, JSValueConst v, const char* want)
{
    const char* s;
    int r;
    if (!want)
        return JS_IsUndefined(v);
    if (JS_IsUndefined(v))
        return 0;
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    r = !strcmp(s, want);
    JS_FreeCString(ctx, s);
    return r;
}

static int wjs_ro_changed(JSContext* ctx, wjs_tr* t, JSValueConst codecs, JSValueConst ext, JSValueConst rtcp)
{
    int64_t n;
    int i;
    if (wjs_len(ctx, codecs, &n) < 0)
        return -1;
    if (n != t->neg.n)
        return 1;
    for (i = 0; i < n; i++) {
        JSValue c = wjs_idx(ctx, codecs, i), v;
        int32_t pt = -1;
        if (JS_IsException(c))
            return -1;
        v = JS_GetPropertyStr(ctx, c, "payloadType");
        JS_FreeValue(ctx, c);
        if (JS_ToInt32(ctx, &pt, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        if (pt != t->neg.v[i].pt)
            return 1;
    }
    if (wjs_len(ctx, ext, &n) < 0)
        return -1;
    if (n != t->neg.nx)
        return 1;
    for (i = 0; i < n; i++) {
        JSValue x = wjs_idx(ctx, ext, i), v;
        int32_t id = -1;
        int same;
        if (JS_IsException(x))
            return -1;
        v = JS_GetPropertyStr(ctx, x, "id");
        if (JS_ToInt32(ctx, &id, v) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, x);
            return -1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, x, "uri");
        same = wjs_same_str(ctx, v, t->neg.x[i].uri);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, x);
        if (same < 0)
            return -1;
        if (!same || id != t->neg.x[i].id)
            return 1;
    }
    if (JS_IsObject(rtcp)) {
        JSValue v = JS_GetPropertyStr(ctx, rtcp, "cname");
        int same = JS_IsUndefined(v) ? 1 : wjs_same_str(ctx, v, t->pc->cname);
        JS_FreeValue(ctx, v);
        if (same <= 0)
            return same < 0 ? -1 : 1;
        v = JS_GetPropertyStr(ctx, rtcp, "reducedSize");
        if (!JS_IsUndefined(v) && JS_ToBool(ctx, v) != t->rsize) {
            JS_FreeValue(ctx, v);
            return 1;
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static JSValue wjs_sender_set_params(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_sender_cid);
    JSValueConst prm = argc ? argv[0] : JS_UNDEFINED;
    JSValue tid = JS_UNDEFINED, encs = JS_UNDEFINED, codecs = JS_UNDEFINED, ext = JS_UNDEFINED, rtcp = JS_UNDEFINED;
    wjs_enc* ne = NULL;
    int64_t n = 0;
    int i, ch, nne = 0;
    char cur[32];
    JSValue r;
    if (!t)
        return JS_EXCEPTION;
    if (!JS_IsObject(prm)) {
        JS_ThrowTypeError(ctx, "setParameters: parameters must be an object");
        return wjs_reject_pending(ctx);
    }
    if (wjs_get_member(ctx, prm, "codecs", &codecs, 1, "setParameters") < 0
        || wjs_get_member(ctx, prm, "encodings", &encs, 1, "setParameters") < 0
        || wjs_get_member(ctx, prm, "headerExtensions", &ext, 1, "setParameters") < 0
        || wjs_get_member(ctx, prm, "rtcp", &rtcp, 1, "setParameters") < 0
        || wjs_get_member(ctx, prm, "transactionId", &tid, 1, "setParameters") < 0)
        goto exc;
    if (!JS_IsObject(encs) || wjs_len(ctx, encs, &n) < 0 || !JS_IsObject(codecs) || !JS_IsObject(ext)) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "setParameters: encodings, codecs and headerExtensions must be sequences");
        goto exc;
    }
    if (n > 0 && n <= 1024) {
        ne = js_mallocz(ctx, sizeof(*ne) * (size_t)n);
        if (!ne)
            goto exc;
        nne = (int)n;
        for (i = 0; i < n; i++) {
            JSValue v = wjs_idx(ctx, encs, i);
            int rc = JS_IsException(v) ? -1 : wjs_read_enc(ctx, v, &ne[i], 1);
            JS_FreeValue(ctx, v);
            if (rc < 0)
                goto exc;
        }
    }
    if (t->stopping) {
        r = wjs_reject_dom(ctx, "InvalidStateError", "setParameters: the transceiver is stopping or stopped");
        goto out;
    }
    if (!t->has_last) {
        r = wjs_reject_dom(ctx, "InvalidStateError", "setParameters: call getParameters() first, in the same task");
        goto out;
    }
    wjs_tid(t, cur, sizeof(cur));
    ch = wjs_same_str(ctx, tid, cur);
    if (ch < 0)
        goto exc;
    if (!ch) {
        r = wjs_reject_dom(ctx, "InvalidModificationError", "setParameters: transactionId does not match the last getParameters()");
        goto out;
    }
    if (n != t->nenc) {
        r = wjs_reject_dom(ctx, "InvalidModificationError", "setParameters: the number of encodings cannot change");
        goto out;
    }
    for (i = 0; i < nne; i++) {
        int same = ne[i].rid && t->enc[i].rid ? !strcmp(ne[i].rid, t->enc[i].rid) : ne[i].rid == t->enc[i].rid;
        if (!same) {
            r = wjs_reject_dom(ctx, "InvalidModificationError", "setParameters: an encoding rid is read-only");
            goto out;
        }
    }
    ch = wjs_ro_changed(ctx, t, codecs, ext, rtcp);
    if (ch < 0)
        goto exc;
    if (ch) {
        r = wjs_reject_dom(ctx, "InvalidModificationError", "setParameters: codecs, headerExtensions and rtcp are read-only");
        goto out;
    }
    for (i = 0; i < nne; i++)
        if (ne[i].codec) {
            int ok, nc;
            const rtc_sdp_codec* cs;
            if (t->neg.n)
                ok = wjs_enc_codec_in(ne[i].codec, t->kind, t->neg.v, t->neg.n, 0);
            else if (t->prefs.n) {
                int k;
                ok = 0;
                for (k = 0; k < t->prefs.n && !ok; k++)
                    ok = !wjs_is_aux(t->prefs.v[k].name) && wjs_cpref_match(ne[i].codec, t->kind, &t->prefs.v[k], 1);
            } else {
                cs = wjs_caps(t->kind, &nc);
                ok = wjs_enc_codec_in(ne[i].codec, t->kind, cs, nc, 1);
            }
            if (!ok) {
                r = wjs_reject_dom(ctx, "InvalidModificationError", "setParameters: an encoding codec is not one of the negotiated, preferred or capable codecs");
                goto out;
            }
        }
    if (wjs_enc_ranges(ctx, t->kind, ne, nne) < 0)
        goto exc;
    for (i = 0; i < nne; i++) {
        wjs_enc* d = &t->enc[i];
        d->active = ne[i].active;
        d->has_bitrate = ne[i].has_bitrate;
        d->max_bitrate = ne[i].max_bitrate;
        d->has_framerate = ne[i].has_framerate;
        d->max_framerate = ne[i].max_framerate;
        if (t->kind) {
            d->has_scale = 1;
            d->scale = ne[i].has_scale ? ne[i].scale : 1.0;
        }
        d->priority = ne[i].priority;
        d->net_priority = ne[i].net_priority;
        if (d->codec)
            js_free(ctx, d->codec->fmtp);
        js_free(ctx, d->codec);
        d->codec = ne[i].codec;
        ne[i].codec = NULL;
    }
    t->has_last = 0;
    r = wjs_settled(ctx, JS_UNDEFINED, 1);
    goto out;
exc:
    r = wjs_reject_pending(ctx);
out:
    if (ne)
        wjs_enc_free(ctx, ne, nne);
    JS_FreeValue(ctx, tid);
    JS_FreeValue(ctx, encs);
    JS_FreeValue(ctx, codecs);
    JS_FreeValue(ctx, ext);
    JS_FreeValue(ctx, rtcp);
    return r;
}

JSValue wjs_tr_replace_op(JSContext* ctx, wjs_pc* p, JSValueConst arg)
{
    JSValue tro = JS_GetPropertyUint32(ctx, arg, 0), track = JS_GetPropertyUint32(ctx, arg, 1);
    wjs_tr* t = wjs_tr_get(tro);
    JSValue r = JS_UNDEFINED;
    (void)p;
    if (!t)
        r = JS_ThrowTypeError(ctx, "replaceTrack: lost transceiver");
    else if (t->stopping)
        r = js_throw_dom_exception(ctx, "InvalidStateError", "replaceTrack: the transceiver is stopping or stopped");
    else {
        JS_FreeValue(ctx, t->send_track);
        t->send_track = JS_DupValue(ctx, track);
        wjs_media_sync(ctx, t->pc);
    }
    JS_FreeValue(ctx, tro);
    JS_FreeValue(ctx, track);
    return r;
}

static JSValue wjs_sender_replace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_sender_cid);
    JSValueConst tv = argc ? argv[0] : JS_NULL;
    wjs_track* nt;
    JSValue arr, r;
    if (!t)
        return JS_EXCEPTION;
    if (JS_IsUndefined(tv))
        tv = JS_NULL;
    nt = JS_IsNull(tv) ? NULL : wjs_track_get(tv);
    if (!JS_IsNull(tv) && !nt)
        return wjs_settled(ctx, wjs_type_err(ctx, "replaceTrack: argument must be a MediaStreamTrack or null"), 0);
    if (nt && nt->kind != t->kind)
        return wjs_settled(ctx, wjs_type_err(ctx, "replaceTrack: the track kind does not match the transceiver kind"), 0);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, t->self));
    JS_SetPropertyUint32(ctx, arr, 1, JS_DupValue(ctx, tv));
    r = wjs_pc_op_push(ctx, t->pc, -1, arr);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue wjs_sender_set_streams(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_sender_cid);
    if (!t)
        return JS_EXCEPTION;
    if (wjs_check_streams(ctx, argc, argv) < 0)
        return JS_EXCEPTION;
    if (t->pc->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "setStreams: the RTCPeerConnection is closed");
    if (wjs_set_sids(ctx, t, argc, argv) < 0)
        return JS_EXCEPTION;
    wjs_pc_nneeded(ctx, t->pc);
    return JS_UNDEFINED;
}

static JSValue wjs_empty_map(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx), mc = JS_GetPropertyStr(ctx, g, "Map"), m;
    JS_FreeValue(ctx, g);
    m = JS_IsException(mc) ? mc : JS_CallConstructor(ctx, mc, 0, NULL);
    JS_FreeValue(ctx, mc);
    return m;
}

static JSValue wjs_side_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, magic ? wjs_receiver_cid : wjs_sender_cid);
    JSValue m;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    m = wjs_empty_map(ctx);
    if (JS_IsException(m))
        return wjs_reject_pending(ctx);
    return wjs_settled(ctx, m, 1);
}

static JSValue wjs_side_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, magic >= 10 ? wjs_receiver_cid : wjs_sender_cid);
    if (!t)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_DupValue(ctx, t->send_track);
    case 10:
        return JS_DupValue(ctx, t->recv_track);
    case 11:
        return t->has_jbt ? JS_NewFloat64(ctx, t->jbt) : JS_NULL;
    case 1:
    case 12:
        return t->has_mid ? wjs_pc_dtls(ctx, t->pc) : JS_NULL;
    case 2:
        if (t->kind)
            return JS_NULL;
        if (JS_IsUndefined(t->dtmf)) {
            t->dtmf = wjs_dtmf_new(ctx, t->self);
            if (JS_IsException(t->dtmf)) {
                t->dtmf = JS_UNDEFINED;
                return JS_EXCEPTION;
            }
        }
        return JS_DupValue(ctx, t->dtmf);
    default:
        return JS_NULL;
    }
}

static JSValue wjs_receiver_set_jbt(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_receiver_cid);
    double d;
    (void)magic;
    if (!t)
        return JS_EXCEPTION;
    if (JS_IsNull(v)) {
        t->has_jbt = 0;
        return JS_UNDEFINED;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0)
        return JS_EXCEPTION;
    if (!isfinite(d))
        return JS_ThrowTypeError(ctx, "jitterBufferTarget must be a finite number or null");
    if (d < 0 || d > 4000)
        return JS_ThrowRangeError(ctx, "jitterBufferTarget must be within 0..4000 ms");
    t->jbt = d;
    t->has_jbt = 1;
    return JS_UNDEFINED;
}

static JSValue wjs_receiver_sources(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_tr* t = wjs_side_tr(ctx, this_val, wjs_receiver_cid);
    wrt_src s[WRT_RX_CSRC_MAX];
    JSValue a;
    int n = 0, i;
    (void)argc;
    (void)argv;
    if (!t)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    if (t->pc && t->pc->t)
        n = wrt_media_sources(t->pc->t, t->id, magic, wrt_now_us(), s, WRT_RX_CSRC_MAX);
    for (i = 0; i < n; i++) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, a);
            return o;
        }
        JS_SetPropertyStr(ctx, o, "timestamp", JS_NewFloat64(ctx, wrt_wall_ms(s[i].at_us)));
        JS_SetPropertyStr(ctx, o, "source", JS_NewUint32(ctx, s[i].src));
        if (s[i].level != WRT_LEVEL_NONE)
            JS_SetPropertyStr(ctx, o, "audioLevel", JS_NewFloat64(ctx, s[i].level == 127 ? 0.0 : pow(10.0, -(double)s[i].level / 20.0)));
        JS_SetPropertyStr(ctx, o, "rtpTimestamp", JS_NewUint32(ctx, s[i].rtp_ts));
        JS_SetPropertyUint32(ctx, a, (uint32_t)i, o);
    }
    return a;
}

static const JSCFunctionListEntry wjs_sender_proto[] = {
    JS_CGETSET_MAGIC_DEF("track", wjs_side_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("transport", wjs_side_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("dtmf", wjs_side_get, NULL, 2),
    JS_CFUNC_DEF("getParameters", 0, wjs_sender_params),
    JS_CFUNC_DEF("setParameters", 1, wjs_sender_set_params),
    JS_CFUNC_DEF("replaceTrack", 1, wjs_sender_replace),
    JS_CFUNC_DEF("setStreams", 0, wjs_sender_set_streams),
    JS_CFUNC_MAGIC_DEF("getStats", 0, wjs_side_stats, 0),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCRtpSender", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry wjs_receiver_proto[] = {
    JS_CGETSET_MAGIC_DEF("track", wjs_side_get, NULL, 10),
    JS_CGETSET_MAGIC_DEF("transport", wjs_side_get, NULL, 12),
    JS_CGETSET_MAGIC_DEF("jitterBufferTarget", wjs_side_get, wjs_receiver_set_jbt, 11),
    JS_CFUNC_DEF("getParameters", 0, wjs_receiver_params),
    JS_CFUNC_MAGIC_DEF("getContributingSources", 0, wjs_receiver_sources, 1),
    JS_CFUNC_MAGIC_DEF("getSynchronizationSources", 0, wjs_receiver_sources, 0),
    JS_CFUNC_MAGIC_DEF("getStats", 0, wjs_side_stats, 1),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCRtpReceiver", JS_PROP_CONFIGURABLE),
};

JSValue wjs_pc_media_list(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_pc* p = wjs_pc_get_opaque(ctx, this_val);
    JSValue a;
    uint32_t k = 0;
    int i;
    (void)argc;
    (void)argv;
    if (!p)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (!t || t->removed || (magic < 2 && t->stopped))
            continue;
        JS_SetPropertyUint32(ctx, a, k++, JS_DupValue(ctx, magic == 0 ? t->sender : magic == 1 ? t->receiver : p->trs[i]));
    }
    return a;
}

JSValue wjs_pc_add_track(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_get_opaque(ctx, this_val);
    JSValueConst track = argc ? argv[0] : JS_UNDEFINED;
    wjs_track* tk;
    wjs_tr* reuse = NULL;
    JSValue obj = JS_UNDEFINED;
    int i;
    if (!p)
        return JS_EXCEPTION;
    tk = wjs_track_get(track);
    if (!tk)
        return JS_ThrowTypeError(ctx, "addTrack: argument 1 is not a MediaStreamTrack");
    if (wjs_check_streams(ctx, argc - 1, argv + 1) < 0)
        return JS_EXCEPTION;
    if (p->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "addTrack: the RTCPeerConnection is closed");
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t && !t->stopped && JS_VALUE_GET_PTR(t->send_track) == JS_VALUE_GET_PTR(track) && JS_IsObject(t->send_track))
            return js_throw_dom_exception(ctx, "InvalidAccessError", "addTrack: the track already has a sender");
    }
    for (i = 0; i < p->ntr && !reuse; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t && !t->removed && !t->stopping && JS_IsNull(t->send_track) && t->kind == tk->kind && !t->ever_sent)
            reuse = t;
    }
    if (reuse) {
        reuse->send_track = JS_DupValue(ctx, track);
        if (reuse->dir == RTC_DIR_RECVONLY)
            reuse->dir = RTC_DIR_SENDRECV;
        else if (reuse->dir == RTC_DIR_INACTIVE)
            reuse->dir = RTC_DIR_SENDONLY;
        if (wjs_set_sids(ctx, reuse, argc - 1, argv + 1) < 0)
            return JS_EXCEPTION;
        wjs_pc_nneeded(ctx, p);
        return JS_DupValue(ctx, reuse->sender);
    }
    obj = wjs_tr_create(ctx, p, tk->kind, RTC_DIR_SENDRECV, track);
    if (JS_IsException(obj))
        return obj;
    {
        wjs_tr* t = wjs_tr_get(obj);
        JSValue s;
        t->by_addtrack = 1;
        if (wjs_set_sids(ctx, t, argc - 1, argv + 1) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        s = JS_DupValue(ctx, t->sender);
        JS_FreeValue(ctx, obj);
        wjs_pc_nneeded(ctx, p);
        return s;
    }
}

JSValue wjs_pc_remove_track(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_get_opaque(ctx, this_val);
    wjs_side* s;
    wjs_tr* t;
    if (!p)
        return JS_EXCEPTION;
    s = argc ? JS_GetOpaque(argv[0], wjs_sender_cid) : NULL;
    if (!s)
        return JS_ThrowTypeError(ctx, "removeTrack: argument 1 is not an RTCRtpSender");
    if (p->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "removeTrack: the RTCPeerConnection is closed");
    t = wjs_tr_get(s->tr_obj);
    if (!t || t->pc != p)
        return js_throw_dom_exception(ctx, "InvalidAccessError", "removeTrack: the sender was not created by this connection");
    if (t->removed || t->stopped || JS_IsNull(t->send_track))
        return JS_UNDEFINED;
    JS_FreeValue(ctx, t->send_track);
    t->send_track = JS_NULL;
    if (t->dir == RTC_DIR_SENDRECV)
        t->dir = RTC_DIR_RECVONLY;
    else if (t->dir == RTC_DIR_SENDONLY)
        t->dir = RTC_DIR_INACTIVE;
    wjs_pc_nneeded(ctx, p);
    return JS_UNDEFINED;
}

JSValue wjs_pc_add_transceiver(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_get_opaque(ctx, this_val);
    JSValueConst a0 = argc ? argv[0] : JS_UNDEFINED, init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue track = JS_UNDEFINED, obj, v;
    wjs_enc* enc = NULL;
    int nenc = 0, kind, dir = RTC_DIR_SENDRECV;
    JSValue* sv = NULL;
    int64_t ns = 0, i;
    wjs_tr* t;
    if (!p)
        return JS_EXCEPTION;
    if (JS_IsString(a0)) {
        const char* s = JS_ToCString(ctx, a0);
        if (!s)
            return JS_EXCEPTION;
        kind = !strcmp(s, "audio") ? 0 : !strcmp(s, "video") ? 1 : -1;
        JS_FreeCString(ctx, s);
        if (kind < 0)
            return JS_ThrowTypeError(ctx, "addTransceiver: kind must be 'audio' or 'video'");
    } else {
        wjs_track* tk = wjs_track_get(a0);
        if (!tk)
            return JS_ThrowTypeError(ctx, "addTransceiver: argument 1 must be a MediaStreamTrack or 'audio'/'video'");
        kind = tk->kind;
        track = a0;
    }
    if (!JS_IsUndefined(init) && !JS_IsNull(init) && !JS_IsObject(init))
        return JS_ThrowTypeError(ctx, "addTransceiver: init must be an object");
    if (JS_IsObject(init)) {
        int idx;
        v = JS_GetPropertyStr(ctx, init, "direction");
        if (JS_IsException(v))
            return v;
        if (!JS_IsUndefined(v)) {
            int rc = wjs_enum_index(ctx, v, wjs_dir_names, 5, &idx);
            JS_FreeValue(ctx, v);
            if (rc < 0)
                return JS_EXCEPTION;
            if (idx == WJS_DIR_STOPPED)
                return JS_ThrowTypeError(ctx, "addTransceiver: direction 'stopped' is not allowed");
            dir = idx;
        }
        v = JS_GetPropertyStr(ctx, init, "streams");
        if (JS_IsException(v))
            return v;
        if (!JS_IsUndefined(v)) {
            if (!JS_IsObject(v) || wjs_len(ctx, v, &ns) < 0 || ns > 1024) {
                JS_FreeValue(ctx, v);
                return JS_IsException(v) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "addTransceiver: streams must be a sequence of MediaStream");
            }
            sv = js_mallocz(ctx, sizeof(JSValue) * (size_t)(ns ? ns : 1));
            if (!sv) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            for (i = 0; i < ns; i++)
                sv[i] = wjs_idx(ctx, v, i);
            JS_FreeValue(ctx, v);
            if (wjs_check_streams(ctx, (int)ns, sv) < 0)
                goto fail;
        }
        v = JS_GetPropertyStr(ctx, init, "sendEncodings");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            int rc = wjs_send_encodings(ctx, v, kind, &enc, &nenc);
            JS_FreeValue(ctx, v);
            if (rc < 0)
                goto fail;
        }
    }
    if (p->closed) {
        js_throw_dom_exception(ctx, "InvalidStateError", "addTransceiver: the RTCPeerConnection is closed");
        goto fail;
    }
    obj = wjs_tr_create(ctx, p, kind, dir, track);
    if (JS_IsException(obj))
        goto fail;
    t = wjs_tr_get(obj);
    if (nenc) {
        wjs_enc_clear(JS_GetRuntime(ctx), t);
        t->enc = enc;
        t->nenc = nenc;
        enc = NULL;
    }
    if (wjs_set_sids(ctx, t, (int)ns, sv) < 0) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    for (i = 0; i < ns; i++)
        JS_FreeValue(ctx, sv[i]);
    js_free(ctx, sv);
    wjs_pc_nneeded(ctx, p);
    return obj;
fail:
    if (sv) {
        for (i = 0; i < ns; i++)
            JS_FreeValue(ctx, sv[i]);
        js_free(ctx, sv);
    }
    if (enc)
        wjs_enc_free(ctx, enc, nenc);
    return JS_EXCEPTION;
}

static int wjs_codecs_from(JSContext* ctx, wjs_codecs* c, const rtc_sdp_media* md)
{
    size_t need = 0, at = 0;
    int i;
    wjs_codecs n = { 0 };
    for (i = 0; i < md->ncodecs; i++)
        need += strlen(md->codecs[i].name) + strlen(md->codecs[i].fmtp) + 2;
    for (i = 0; i < md->next; i++)
        need += strlen(md->ext[i].uri) + 1;
    n.blob = js_malloc(ctx, need + 1);
    n.v = js_mallocz(ctx, sizeof(rtc_sdp_codec) * (size_t)(md->ncodecs + 1));
    n.x = js_mallocz(ctx, sizeof(rtc_sdp_ext) * (size_t)(md->next + 1));
    if (!n.blob || !n.v || !n.x) {
        wjs_codecs_clear(JS_GetRuntime(ctx), &n);
        return -1;
    }
    for (i = 0; i < md->ncodecs; i++) {
        size_t a = strlen(md->codecs[i].name) + 1, b = strlen(md->codecs[i].fmtp) + 1;
        n.v[i] = md->codecs[i];
        memcpy(n.blob + at, md->codecs[i].name, a);
        n.v[i].name = n.blob + at;
        at += a;
        memcpy(n.blob + at, md->codecs[i].fmtp, b);
        n.v[i].fmtp = n.blob + at;
        at += b;
    }
    n.n = md->ncodecs;
    for (i = 0; i < md->next; i++) {
        size_t a = strlen(md->ext[i].uri) + 1;
        n.x[i] = md->ext[i];
        memcpy(n.blob + at, md->ext[i].uri, a);
        n.x[i].uri = n.blob + at;
        n.x[i].attrs = "";
        at += a;
    }
    n.nx = md->next;
    wjs_codecs_clear(JS_GetRuntime(ctx), c);
    *c = n;
    return 0;
}

static void wjs_fmtp_val(const char* f, const char* key, char* out, size_t cap)
{
    size_t kl = strlen(key), i = 0;
    out[0] = 0;
    while (f && *f) {
        while (*f == ' ' || *f == ';')
            f++;
        if (!strncasecmp(f, key, kl) && f[kl] == '=') {
            f += kl + 1;
            while (*f && *f != ';' && i + 1 < cap) {
                char c = *f++;
                out[i++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            out[i] = 0;
            return;
        }
        while (*f && *f != ';')
            f++;
    }
}

static int wjs_codec_same(const rtc_sdp_codec* a, const rtc_sdp_codec* b)
{
    char x[16], y[16];
    if (!a->name || !b->name || strcasecmp(a->name, b->name) || a->clock != b->clock)
        return 0;
    if ((a->channels > 1 ? a->channels : 1) != (b->channels > 1 ? b->channels : 1))
        return 0;
    if (strcasecmp(a->name, "H264"))
        return 1;
    if (a->fmtp && b->fmtp && !strcmp(a->fmtp, b->fmtp))
        return 1;
    wjs_fmtp_val(a->fmtp, "profile-level-id", x, sizeof(x));
    wjs_fmtp_val(b->fmtp, "profile-level-id", y, sizeof(y));
    if (strncmp(x[0] ? x : "42000a", y[0] ? y : "42000a", 4))
        return 0;
    wjs_fmtp_val(a->fmtp, "packetization-mode", x, sizeof(x));
    wjs_fmtp_val(b->fmtp, "packetization-mode", y, sizeof(y));
    return !strcmp(x[0] ? x : "0", y[0] ? y : "0");
}

typedef struct {
    const rtc_sdp_codec* pt[128];
    const char* uri[256];
    const rtc_sdp_codec* list[128];
    uint8_t xid[255];
    int nlist;
    int nx;
    uint8_t used[128];
    uint8_t ids[256];
} wjs_negmap;

static void wjs_negmap_build(wjs_pc* p, wjs_negmap* g)
{
    int i, k;
    memset(g, 0, sizeof(*g));
    for (i = 0; i < p->ntr; i++) {
        const wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (!t)
            continue;
        for (k = 0; k < t->neg.n; k++) {
            int pt = t->neg.v[k].pt & 127;
            g->used[pt] = 1;
            if (!g->pt[pt]) {
                g->pt[pt] = &t->neg.v[k];
                g->list[g->nlist++] = g->pt[pt];
            }
        }
        for (k = 0; k < t->neg.nx; k++)
            if (t->neg.x[k].id < 256) {
                g->ids[t->neg.x[k].id] = 1;
                if (!g->uri[t->neg.x[k].id] && t->neg.x[k].id) {
                    g->uri[t->neg.x[k].id] = t->neg.x[k].uri;
                    g->xid[g->nx++] = (uint8_t)t->neg.x[k].id;
                }
            }
    }
}

static int wjs_is_rtx(const rtc_sdp_codec* c)
{
    return c->rtx_apt_set && c->name && !strcasecmp(c->name, "rtx");
}

static int wjs_neg_hit(const rtc_sdp_codec* n, const rtc_sdp_codec* c, int rtx_apt)
{
    if (!n->name)
        return 0;
    if (rtx_apt >= 0)
        return n->rtx_apt == rtx_apt && wjs_is_rtx(n);
    return !wjs_is_rtx(n) && wjs_codec_same(n, c);
}

static const rtc_sdp_codec* wjs_neg_find(const wjs_negmap* g, const wjs_tr* self, const rtc_sdp_codec* c, int rtx_apt)
{
    int k;
    const rtc_sdp_codec* q = g->pt[c->pt & 127];
    for (k = 0; k < self->neg.n; k++)
        if (self->neg.v[k].pt == c->pt && wjs_neg_hit(&self->neg.v[k], c, rtx_apt))
            return &self->neg.v[k];
    if (q && wjs_neg_hit(q, c, rtx_apt))
        return q;
    for (k = 0; k < self->neg.n; k++)
        if (wjs_neg_hit(&self->neg.v[k], c, rtx_apt))
            return &self->neg.v[k];
    for (k = 0; k < g->nlist; k++)
        if (wjs_neg_hit(g->list[k], c, rtx_apt))
            return g->list[k];
    return NULL;
}

static int wjs_ext_find(const wjs_negmap* g, const wjs_tr* self, const char* uri, const uint8_t* idt, int dflt)
{
    int k;
    if (dflt < 256 && g->uri[dflt] && !idt[dflt] && !strcmp(g->uri[dflt], uri))
        return dflt;
    for (k = 0; k < self->neg.nx; k++)
        if (self->neg.x[k].uri && !strcmp(self->neg.x[k].uri, uri) && self->neg.x[k].id < 256 && !idt[self->neg.x[k].id])
            return self->neg.x[k].id;
    for (k = 0; k < g->nx; k++)
        if (!idt[g->xid[k]] && g->uri[g->xid[k]] && !strcmp(g->uri[g->xid[k]], uri))
            return g->xid[k];
    return 0;
}

static int wjs_pt_alloc(const uint8_t* taken, const uint8_t* used, const uint8_t* resv)
{
    int pass, pt;
    for (pass = 0; pass < 2; pass++) {
        for (pt = 96; pt < 128; pt++)
            if (!taken[pt] && !used[pt] && (pass || !resv[pt]))
                return pt;
        for (pt = 35; pt < 64; pt++)
            if (!taken[pt] && !used[pt] && (pass || !resv[pt]))
                return pt;
    }
    return -1;
}

static int wjs_offer_remap(JSContext* ctx, const wjs_negmap* g, wjs_tr* t, rtc_jsep_offer_m* o)
{
    const rtc_jsep_caps* caps = rtc_jsep_default_caps();
    int n = o->ncodecs, k, j;
    uint8_t taken[128] = { 0 }, resv[128] = { 0 }, idt[256] = { 0 };
    const uint8_t *used = g->used, *ids = g->ids;
    size_t sz = sizeof(rtc_sdp_codec) * (size_t)n + sizeof(uint16_t) * (size_t)(caps->next + 1) + 16 * (size_t)n;
    char* blk = js_malloc(ctx, sz);
    rtc_sdp_codec* out;
    uint16_t* xid;
    char* txt;
    if (!blk)
        return -1;
    out = (rtc_sdp_codec*)(void*)blk;
    xid = (uint16_t*)(void*)(blk + sizeof(rtc_sdp_codec) * (size_t)n);
    txt = blk + sizeof(rtc_sdp_codec) * (size_t)n + sizeof(uint16_t) * (size_t)(caps->next + 1);
    for (k = 0; k < n; k++) {
        out[k] = o->codecs[k];
        resv[o->codecs[k].pt & 127] = 1;
    }
    for (k = 0; k < n; k++) {
        const rtc_sdp_codec* f;
        if (wjs_is_rtx(&out[k]))
            continue;
        f = wjs_neg_find(g, t, &out[k], -1);
        if (f && !taken[f->pt & 127]) {
            out[k].pt = f->pt;
            taken[f->pt & 127] = 2;
        } else
            out[k].pt = 255;
    }
    for (k = 0; k < n; k++) {
        int pt;
        if (out[k].pt != 255 || wjs_is_rtx(&out[k]))
            continue;
        pt = o->codecs[k].pt & 127;
        if (taken[pt] || used[pt])
            pt = wjs_pt_alloc(taken, used, resv);
        out[k].pt = (uint8_t)(pt < 0 ? o->codecs[k].pt : pt);
        taken[out[k].pt & 127] = 1;
    }
    for (k = 0; k < n; k++) {
        const rtc_sdp_codec* f;
        int apt = -1, pt;
        if (!wjs_is_rtx(&o->codecs[k]))
            continue;
        for (j = 0; j < n; j++)
            if (o->codecs[j].pt == o->codecs[k].rtx_apt && !wjs_is_rtx(&o->codecs[j]))
                apt = out[j].pt;
        if (apt < 0)
            apt = o->codecs[k].rtx_apt;
        f = wjs_neg_find(g, t, &out[k], apt);
        pt = f && !taken[f->pt & 127] ? f->pt : o->codecs[k].pt & 127;
        if (!(f && pt == f->pt) && (taken[pt] || used[pt]))
            pt = wjs_pt_alloc(taken, used, resv);
        if (pt < 0)
            pt = o->codecs[k].pt;
        out[k].pt = (uint8_t)pt;
        taken[pt & 127] = 1;
        out[k].rtx_apt = (uint8_t)apt;
        snprintf(txt + 16 * k, 16, "apt=%d", apt);
        out[k].fmtp = txt + 16 * k;
    }
    for (k = 0; k < caps->next; k++) {
        int id = wjs_ext_find(g, t, caps->ext[k], idt, k + 1);
        if (!id) {
            id = k + 1;
            if (ids[id] || idt[id])
                for (id = 1; id < 15 && (ids[id] || idt[id] || id <= caps->next); id++)
                    ;
            if (id >= 15)
                for (id = 1; id < 15 && (ids[id] || idt[id]); id++)
                    ;
            if (id >= 15)
                id = k + 1;
        }
        idt[id] = 1;
        xid[k] = (uint16_t)id;
    }
    o->codecs = out;
    o->ext_ids = xid;
    return 0;
}

int wjs_media_offer(JSContext* ctx, wjs_pc* p, const wjs_ml* ms, rtc_jsep_offer_m* out)
{
    int i, j;
    wjs_negmap g;
    wjs_negmap_build(p, &g);
    for (i = 0; i < ms->n; i++) {
        wjs_m* m = &ms->v[i];
        rtc_jsep_offer_m* o = &out[i];
        wjs_tr* t;
        o->kind = m->app ? "application" : m->kind;
        o->mid = m->mid;
        o->rejected = m->rejected;
        o->bundle_only = m->bundle_only;
        if (!m->media)
            continue;
        t = wjs_tr_by_id(p, m->trid);
        o->codecs = wjs_caps(!strcmp(m->kind, "video"), &o->ncodecs);
        if (!t || m->rejected) {
            o->rejected = 1;
            continue;
        }
        if (t->prefs.n) {
            o->codecs = t->prefs.v;
            o->ncodecs = t->prefs.n;
        }
        if ((g.nlist || g.nx) && wjs_offer_remap(ctx, &g, t, o) < 0)
            return -1;
        o->dir = t->dir;
        m->ldir = t->dir;
        m->msid_hash = wjs_tr_msid_hash(t);
        if (wjs_is_sending(t->dir)) {
            int nm = t->nsids ? t->nsids : 1;
            wjs_track* tk = wjs_track_get(t->send_track);
            rtc_sdp_msid* mv = js_mallocz(ctx, sizeof(*mv) * (size_t)nm);
            if (!mv)
                return -1;
            for (j = 0; j < nm; j++) {
                mv[j].stream = t->nsids ? t->sids[j] : "-";
                mv[j].track = tk ? tk->id : wjs_track_get(t->recv_track)->id;
            }
            o->msid = mv;
            o->nmsid = nm;
            o->ssrc = t->ssrc;
            o->rtx_ssrc = t->rtx_ssrc;
            o->cname = p->cname;
            if (t->kind && t->nenc > 1) {
                const char** rv = js_mallocz(ctx, sizeof(char*) * (size_t)t->nenc);
                if (!rv)
                    return -1;
                for (j = 0; j < t->nenc; j++)
                    rv[j] = t->enc[j].rid ? t->enc[j].rid : "";
                o->rids = rv;
                o->nrids = t->nenc;
            }
        }
    }
    return 0;
}

void wjs_media_offer_free(JSContext* ctx, rtc_jsep_offer_m* out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        void *a, *b;
        memcpy(&a, &out[i].msid, sizeof(a));
        memcpy(&b, &out[i].rids, sizeof(b));
        js_free(ctx, a);
        js_free(ctx, b);
        if (out[i].ext_ids) {
            memcpy(&a, &out[i].codecs, sizeof(a));
            js_free(ctx, a);
        }
    }
    js_free(ctx, out);
}


int wjs_media_answer_local(JSContext* ctx, wjs_pc* p, const rtc_sdp* offer, rtc_jsep_local* out)
{
    int i, j;
    for (i = 0; i < offer->nmedia; i++) {
        const rtc_sdp_media* md = &offer->media[i];
        rtc_jsep_local* lo = &out[i];
        wjs_tr* t;
        if (strcmp(md->kind, "audio") && strcmp(md->kind, "video"))
            continue;
        t = i < p->pend.n ? wjs_tr_by_id(p, p->pend.v[i].trid) : NULL;
        if (!t || t->stopping || t->stopped) {
            lo->stopped = 1;
            continue;
        }
        lo->dir = t->dir;
        if (t->prefs.n) {
            lo->codecs = t->prefs.v;
            lo->ncodecs = t->prefs.n;
        }
        if (wjs_is_sending(t->dir)) {
            wjs_track* tk = wjs_track_get(t->send_track);
            const char* tid = tk ? tk->id : wjs_track_get(t->recv_track)->id;
            int nm = t->nsids ? t->nsids : 1;
            rtc_sdp_msid* mv = js_mallocz(ctx, sizeof(*mv) * (size_t)nm);
            if (!mv)
                return -1;
            for (j = 0; j < nm; j++) {
                mv[j].stream = t->nsids ? t->sids[j] : "-";
                mv[j].track = tid;
            }
            lo->msids = mv;
            lo->nmsid = nm;
            lo->msid = mv[0];
            lo->ssrc = t->ssrc;
            lo->rtx_ssrc = t->rtx_ssrc;
            lo->cname = p->cname;
        }
    }
    return 0;
}

void wjs_media_answer_local_free(JSContext* ctx, rtc_jsep_local* out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        void* a;
        memcpy(&a, &out[i].msids, sizeof(a));
        js_free(ctx, a);
    }
    js_free(ctx, out);
}

static JSValue wjs_rstream_get(JSContext* ctx, wjs_pc* p, const char* id)
{
    JSValue s;
    if (JS_IsUndefined(p->rstreams)) {
        p->rstreams = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(p->rstreams)) {
            p->rstreams = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
    }
    s = JS_GetPropertyStr(ctx, p->rstreams, id);
    if (!JS_IsUndefined(s))
        return s;
    s = wjs_stream_make(ctx, JS_UNDEFINED, id);
    if (!JS_IsException(s))
        JS_SetPropertyStr(ctx, p->rstreams, id, JS_DupValue(ctx, s));
    return s;
}

static int wjs_has_stream(JSContext* ctx, JSValueConst arr, JSValueConst s)
{
    int64_t n, i;
    if (wjs_len(ctx, arr, &n) < 0)
        return 0;
    for (i = 0; i < n; i++) {
        JSValue v = wjs_idx(ctx, arr, i);
        int same = JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(s);
        JS_FreeValue(ctx, v);
        if (same)
            return 1;
    }
    return 0;
}

static int wjs_q(JSContext* ctx, wjs_pc* p, int code, JSValueConst target, JSValueConst arg)
{
    int64_t n = 0;
    JSValue e;
    if (JS_IsUndefined(p->track_evs)) {
        p->track_evs = JS_NewArray(ctx);
        if (JS_IsException(p->track_evs)) {
            p->track_evs = JS_UNDEFINED;
            return -1;
        }
    }
    e = JS_NewArray(ctx);
    if (JS_IsException(e))
        return -1;
    JS_SetPropertyUint32(ctx, e, 0, JS_NewInt32(ctx, code));
    JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, target));
    JS_SetPropertyUint32(ctx, e, 2, JS_DupValue(ctx, arg));
    wjs_len(ctx, p->track_evs, &n);
    return JS_SetPropertyInt64(ctx, p->track_evs, n, e) < 0 ? -1 : 0;
}

static void wjs_mute_q(JSContext* ctx, wjs_pc* p, JSValueConst obj)
{
    wjs_track* t = wjs_track_get(obj);
    if (!t || t->ended || t->muted)
        return;
    t->muted = 1;
    wjs_q(ctx, p, 1, obj, JS_UNDEFINED);
}
static int wjs_set_rstreams(JSContext* ctx, wjs_pc* p, wjs_tr* t, const rtc_sdp_media* md)
{
    JSValue nw = JS_NewArray(ctx);
    int64_t n, i;
    uint32_t k = 0;
    int j;
    if (JS_IsException(nw))
        return -1;
    for (j = 0; md && j < md->nmsid; j++) {
        const char* id = md->msid[j].stream;
        JSValue s;
        if (!id[0] || !strcmp(id, "-"))
            continue;
        s = wjs_rstream_get(ctx, p, id);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, nw);
            return -1;
        }
        if (wjs_has_stream(ctx, nw, s))
            JS_FreeValue(ctx, s);
        else
            JS_SetPropertyUint32(ctx, nw, k++, s);
    }
    if (md && !md->nmsid)
        for (j = 0; j < md->nssrc; j++) {
            const char* id = md->ssrcs[j].msid.stream;
            JSValue s;
            if (!id[0] || !strcmp(id, "-"))
                continue;
            s = wjs_rstream_get(ctx, p, id);
            if (JS_IsException(s)) {
                JS_FreeValue(ctx, nw);
                return -1;
            }
            if (wjs_has_stream(ctx, nw, s))
                JS_FreeValue(ctx, s);
            else
                JS_SetPropertyUint32(ctx, nw, k++, s);
        }
    if (wjs_len(ctx, t->rstreams, &n) == 0)
        for (i = 0; i < n; i++) {
            JSValue s = wjs_idx(ctx, t->rstreams, i);
            wjs_stream* st = JS_GetOpaque(s, wjs_stream_cid);
            if (st && !wjs_has_stream(ctx, nw, s) && wjs_stream_drop(ctx, st, t->recv_track))
                wjs_q(ctx, p, 2, s, t->recv_track);
            JS_FreeValue(ctx, s);
        }
    for (i = 0; i < k; i++) {
        JSValue s = wjs_idx(ctx, nw, i);
        wjs_stream* st = JS_GetOpaque(s, wjs_stream_cid);
        if (st && wjs_stream_push(ctx, st, t->recv_track) > 0)
            wjs_q(ctx, p, 3, s, t->recv_track);
        JS_FreeValue(ctx, s);
    }
    JS_FreeValue(ctx, t->rstreams);
    t->rstreams = nw;
    return 0;
}


static wjs_tr* wjs_srd_assoc(JSContext* ctx, wjs_pc* p, wjs_m* m)
{
    int kind = !strcmp(m->kind, "video"), i;
    wjs_tr* t = wjs_tr_by_mid(p, m->mid);
    JSValue obj;
    if (t || m->rejected)
        return t;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* c = wjs_tr_get(p->trs[i]);
        if (c && !c->removed && !c->has_mid && c->by_addtrack && c->kind == kind && !c->stopping && !c->stopped) {
            t = c;
            break;
        }
    }
    if (!t) {
        obj = wjs_tr_create(ctx, p, kind, RTC_DIR_RECVONLY, JS_UNDEFINED);
        if (JS_IsException(obj))
            return NULL;
        t = wjs_tr_get(obj);
        JS_FreeValue(ctx, obj);
        t->pend_created = 1;
    }
    snprintf(t->mid, sizeof(t->mid), "%s", m->mid);
    t->has_mid = 1;
    t->pend_assoc = 1;
    return t;
}

static void wjs_neg_stop(JSContext* ctx, wjs_pc* p, wjs_tr* t)
{
    wjs_track* rt = wjs_track_get(t->recv_track);
    if (t->stopped)
        return;
    if (!t->stopping && rt && !rt->ended)
        wjs_q(ctx, p, 0, t->recv_track, JS_UNDEFINED);
    wjs_tr_stop_local(ctx, t);
    t->stopped = 1;
    t->cur_dir = WJS_DIR_STOPPED;
    t->has_cur = 1;
}

int wjs_media_srd(JSContext* ctx, wjs_pc* p, const rtc_sdp* s, int offer, wjs_ml* ms)
{
    int i;
    for (i = 0; i < ms->n; i++) {
        wjs_m* m = &ms->v[i];
        const rtc_sdp_media* md = &s->media[i];
        wjs_tr* t;
        int ours;
        if (!m->media)
            continue;
        if (offer) {
            t = wjs_srd_assoc(ctx, p, m);
            if (!t && JS_HasException(ctx))
                return -1;
        } else {
            t = wjs_tr_by_id(p, m->trid);
            if (!t)
                t = wjs_tr_by_mid(p, m->mid);
        }
        m->trid = t ? t->id : 0;
        if (!t)
            continue;
        if (offer && m->rejected) {
            wjs_neg_stop(ctx, p, t);
            continue;
        }
        ours = m->rejected ? RTC_DIR_INACTIVE : wjs_dir_reverse(m->rdir);
        {
            int q;
            t->nrssrc = 0;
            for (q = 0; q < md->nssrc && t->nrssrc < 4; q++)
                t->rssrc[t->nrssrc++] = md->ssrcs[q].id;
        }
        if (!wjs_is_receiving(ours))
            t->receptive = 0;
        if (wjs_is_receiving(ours)) {
            if (wjs_set_rstreams(ctx, p, t, md) < 0)
                return -1;
            if (!wjs_is_receiving(t->fired_dir) && wjs_q(ctx, p, 4, t->self, JS_UNDEFINED) < 0)
                return -1;
        } else if (wjs_is_receiving(t->fired_dir)) {
            if (wjs_set_rstreams(ctx, p, t, NULL) < 0)
                return -1;
            wjs_mute_q(ctx, p, t->recv_track);
        }
        t->fired_dir = (uint8_t)ours;
    }
    wjs_media_sync(ctx, p);
    return 0;
}

void wjs_media_sld_offer(JSContext* ctx, wjs_pc* p, wjs_ml* ms)
{
    int i;
    (void)ctx;
    for (i = 0; i < ms->n; i++) {
        wjs_tr* t;
        if (!ms->v[i].media)
            continue;
        t = wjs_tr_by_id(p, ms->v[i].trid);
        if (t && !t->has_mid) {
            snprintf(t->mid, sizeof(t->mid), "%s", ms->v[i].mid);
            t->has_mid = 1;
            t->pend_assoc = 1;
        }
        if (t && !t->stopping && !t->stopped)
            t->receptive = (uint8_t)wjs_is_receiving(ms->v[i].ldir);
    }
    wjs_media_sync(ctx, p);
}

static int wjs_sim_has(const char* l, const char* rid)
{
    size_t n = strlen(rid);
    while (*l) {
        const char* a;
        while (*l == ';' || *l == ',' || *l == '~')
            l++;
        a = l;
        while (*l && *l != ';' && *l != ',' && *l != ' ')
            l++;
        if ((size_t)(l - a) == n && !strncmp(a, rid, n))
            return 1;
        if (*l == ' ')
            break;
    }
    return 0;
}

static const char* wjs_sim_recv(const rtc_sdp_media* md)
{
    int i;
    for (i = 0; i < md->nextra; i++) {
        const char* l = md->extra[i];
        const char* r;
        if (strncmp(l, "a=simulcast:", 12))
            continue;
        r = strstr(l + 12, "recv ");
        if (r && (r == l + 12 || r[-1] == ' '))
            return r + 5;
    }
    return NULL;
}

static void wjs_prune_encodings(JSContext* ctx, wjs_tr* t, const rtc_sdp_media* md)
{
    const char* list = wjs_sim_recv(md);
    int i, k = 0;
    if (t->nenc < 2)
        return;
    for (i = 0; i < t->nenc; i++) {
        if (list && t->enc[i].rid && wjs_sim_has(list, t->enc[i].rid))
            t->enc[k++] = t->enc[i];
        else if (!list && i == 0)
            t->enc[k++] = t->enc[i];
        else
            wjs_enc_one_free(JS_GetRuntime(ctx), &t->enc[i]);
    }
    if (!k) {
        t->enc[0].rid = NULL;
        k = 1;
    }
    t->nenc = k;
}

void wjs_media_apply_answer(JSContext* ctx, wjs_pc* p, const wjs_ml* ms, const rtc_sdp* ans, int remote)
{
    int i;
    for (i = 0; i < ms->n && i < ans->nmedia; i++) {
        wjs_m* m = &ms->v[i];
        const rtc_sdp_media* md = &ans->media[i];
        wjs_tr* t;
        if (!m->media)
            continue;
        t = wjs_tr_by_id(p, m->trid);
        if (!t)
            t = wjs_tr_by_mid(p, m->mid);
        if (!t)
            continue;
        if (m->rejected || rtc_sdp_media_rejected(md)) {
            wjs_neg_stop(ctx, p, t);
            continue;
        }
        {
            int d = md->has_dir ? md->dir : RTC_DIR_SENDRECV;
            if (remote)
                d = wjs_dir_reverse(d);
            else {
                t->receptive = (uint8_t)wjs_is_receiving(d);
                m->ldir = (uint8_t)d;
                m->msid_hash = wjs_tr_msid_hash(t);
            }
            t->cur_dir = (uint8_t)d;
            t->has_cur = 1;
            if (wjs_is_sending(d))
                t->ever_sent = 1;
        }
        t->rsize = md->rtcp_rsize;
        wjs_codecs_from(ctx, &t->neg, md);
        if (remote && t->kind)
            wjs_prune_encodings(ctx, t, md);
        {
            int q;
            for (q = 0; q < t->nenc; q++)
                if (t->enc[q].codec && !wjs_enc_codec_in(t->enc[q].codec, t->kind, t->neg.v, t->neg.n, 0)) {
                    js_free(ctx, t->enc[q].codec->fmtp);
                    js_free(ctx, t->enc[q].codec);
                    t->enc[q].codec = NULL;
                }
        }
    }
    wjs_media_sync(ctx, p);
}

void wjs_media_commit_pending(wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t) {
            t->pend_created = t->pend_assoc = 0;
            t->snap_valid = 0;
            JS_FreeValue(t->ctx, t->snap_streams);
            t->snap_streams = JS_UNDEFINED;
        }
    }
}

void wjs_media_stable(JSContext* ctx, wjs_pc* p)
{
    int i, k = 0;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t && t->stopped)
            t->removed = 1;
        if (!t || t->removed)
            JS_FreeValue(ctx, p->trs[i]);
        else
            p->trs[k++] = p->trs[i];
    }
    p->ntr = k;
    wjs_media_sync(ctx, p);
}

int wjs_tr_has_codec(const wjs_tr* t, const char* name)
{
    int i;
    for (i = 0; i < t->neg.n; i++)
        if (t->neg.v[i].name && !strcasecmp(t->neg.v[i].name, name))
            return 1;
    return 0;
}

void wjs_media_snapshot(JSContext* ctx, wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (!t)
            continue;
        t->snap_valid = 1;
        t->snap_dir = t->dir;
        t->snap_cur = t->cur_dir;
        t->snap_has_cur = t->has_cur;
        t->snap_stopping = t->stopping;
        t->snap_stopped = t->stopped;
        t->snap_fired = t->fired_dir;
        JS_FreeValue(ctx, t->snap_streams);
        t->snap_streams = JS_DupValue(ctx, t->rstreams);
    }
}

static void wjs_snap_drop(JSContext* ctx, wjs_tr* t)
{
    t->snap_valid = 0;
    JS_FreeValue(ctx, t->snap_streams);
    t->snap_streams = JS_UNDEFINED;
}

void wjs_media_snap_clear(JSContext* ctx, wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (t)
            wjs_snap_drop(ctx, t);
    }
}

static void wjs_snap_restore(JSContext* ctx, wjs_pc* p, wjs_tr* t)
{
    int64_t n = 0, i;
    JSValue old = t->rstreams;
    if (JS_VALUE_GET_PTR(old) != JS_VALUE_GET_PTR(t->snap_streams)) {
        if (wjs_len(ctx, old, &n) == 0)
            for (i = 0; i < n; i++) {
                JSValue s = wjs_idx(ctx, old, i);
                wjs_stream* st = JS_GetOpaque(s, wjs_stream_cid);
                if (st && !wjs_has_stream(ctx, t->snap_streams, s) && wjs_stream_drop(ctx, st, t->recv_track))
                    wjs_q(ctx, p, 2, s, t->recv_track);
                JS_FreeValue(ctx, s);
            }
        if (wjs_len(ctx, t->snap_streams, &n) == 0)
            for (i = 0; i < n; i++) {
                JSValue s = wjs_idx(ctx, t->snap_streams, i);
                wjs_stream* st = JS_GetOpaque(s, wjs_stream_cid);
                if (st && wjs_stream_push(ctx, st, t->recv_track) > 0)
                    wjs_q(ctx, p, 3, s, t->recv_track);
                JS_FreeValue(ctx, s);
            }
        t->rstreams = JS_DupValue(ctx, t->snap_streams);
        JS_FreeValue(ctx, old);
    }
    if (wjs_is_receiving(t->fired_dir) && !wjs_is_receiving(t->snap_fired))
        wjs_mute_q(ctx, p, t->recv_track);
    t->fired_dir = t->snap_fired;
    if (t->stopped && !t->snap_stopped) {
        t->dir = t->snap_dir;
        t->cur_dir = t->snap_cur;
        t->has_cur = t->snap_has_cur;
        t->stopping = t->snap_stopping;
        t->stopped = 0;
    }
}

void wjs_media_rollback(JSContext* ctx, wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (!t)
            continue;
        if (t->pend_assoc && wjs_is_receiving(t->fired_dir)) {
            wjs_set_rstreams(ctx, p, t, NULL);
            wjs_mute_q(ctx, p, t->recv_track);
        }
        if (t->pend_created && JS_IsNull(t->send_track)) {
            wjs_tr_stop_local(ctx, t);
            t->stopped = 1;
            t->removed = 1;
        }
        if (t->snap_valid && !t->pend_assoc && !t->pend_created)
            wjs_snap_restore(ctx, p, t);
        if (t->pend_assoc) {
            t->has_mid = 0;
            t->mid[0] = 0;
            t->fired_dir = RTC_DIR_INACTIVE;
        }
        t->pend_created = t->pend_assoc = 0;
        wjs_snap_drop(ctx, t);
    }
    wjs_media_sync(ctx, p);
}

int wjs_media_nneeded(wjs_pc* p)
{
    int i, j;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        const wjs_m* m = NULL;
        if (!t || t->removed)
            continue;
        if (t->stopping && !t->stopped)
            return 1;
        if (t->stopped)
            continue;
        if (!t->has_mid)
            return 1;
        for (j = 0; j < p->m.n; j++)
            if (!strcmp(p->m.v[j].mid, t->mid))
                m = &p->m.v[j];
        if (!m)
            return 1;
        if (p->local_offer) {
            if (t->dir != m->ldir && t->dir != wjs_dir_reverse(m->rdir))
                return 1;
        } else if (rtc_jsep_dir_answer(m->rdir, t->dir) != m->ldir)
            return 1;
        if (wjs_is_sending(t->dir) && wjs_tr_msid_hash(t) != m->msid_hash)
            return 1;
    }
    return 0;
}

static void wjs_fire_track_ev(JSContext* ctx, wjs_pc* p, JSValueConst tro)
{
    wjs_tr* t = wjs_tr_get(tro);
    JSValue ev, streams;
    int64_t k, ns = 0;
    if (!t || t->removed)
        return;
    ev = wjs_event(ctx, "track", p->self);
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    streams = JS_NewArray(ctx);
    wjs_len(ctx, t->rstreams, &ns);
    for (k = 0; k < ns; k++)
        JS_SetPropertyInt64(ctx, streams, k, wjs_idx(ctx, t->rstreams, k));
    JS_SetPropertyStr(ctx, ev, "receiver", JS_DupValue(ctx, t->receiver));
    JS_SetPropertyStr(ctx, ev, "track", JS_DupValue(ctx, t->recv_track));
    JS_SetPropertyStr(ctx, ev, "streams", streams);
    JS_SetPropertyStr(ctx, ev, "transceiver", JS_DupValue(ctx, tro));
    wjs_et_fire(ctx, p->self, &p->et, WJS_PE_TRACK, ev);
    JS_FreeValue(ctx, ev);
}

void wjs_media_fire_tracks(JSContext* ctx, wjs_pc* p)
{
    JSValue evs = p->track_evs;
    int64_t n = 0, i;
    int pass;
    if (JS_IsUndefined(evs))
        return;
    p->track_evs = JS_UNDEFINED;
    wjs_len(ctx, evs, &n);
    for (pass = 0; pass < 5; pass++)
        for (i = 0; i < n && !p->closed; i++) {
            JSValue e = wjs_idx(ctx, evs, i), tgt, arg, cv;
            int32_t c = -1;
            cv = JS_GetPropertyUint32(ctx, e, 0);
            c = dyn_int_or(cv, -1);
            JS_FreeValue(ctx, cv);
            if (c != pass) {
                JS_FreeValue(ctx, e);
                continue;
            }
            tgt = JS_GetPropertyUint32(ctx, e, 1);
            arg = JS_GetPropertyUint32(ctx, e, 2);
            JS_FreeValue(ctx, e);
            if (c == 0 || c == 1)
                wjs_track_fire(ctx, tgt, c ? WJS_TE_MUTE : WJS_TE_ENDED);
            else if (c == 2 || c == 3)
                wjs_stream_fire(ctx, tgt, c == 3 ? WJS_SE_ADD : WJS_SE_REMOVE, arg);
            else
                wjs_fire_track_ev(ctx, p, tgt);
            JS_FreeValue(ctx, tgt);
            JS_FreeValue(ctx, arg);
        }
    JS_FreeValue(ctx, evs);
}

void wjs_media_drop_events(JSContext* ctx, wjs_pc* p)
{
    JS_FreeValue(ctx, p->track_evs);
    p->track_evs = JS_UNDEFINED;
}

void wjs_media_close(JSContext* ctx, wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        if (!t || t->stopped)
            continue;
        wjs_tr_stop_local(ctx, t);
        t->stopped = 1;
        t->cur_dir = WJS_DIR_STOPPED;
        t->has_cur = 1;
    }
    JS_FreeValue(ctx, p->track_evs);
    p->track_evs = JS_UNDEFINED;
}

void wjs_media_mark(JSRuntime* rt, wjs_pc* p, JS_MarkFunc* mark)
{
    int i;
    for (i = 0; i < p->ntr; i++)
        JS_MarkValue(rt, p->trs[i], mark);
    JS_MarkValue(rt, p->rstreams, mark);
    JS_MarkValue(rt, p->track_evs, mark);
}

void wjs_media_free(JSRuntime* rt, wjs_pc* p)
{
    int i;
    for (i = 0; i < p->ntr; i++)
        JS_FreeValueRT(rt, p->trs[i]);
    js_free_rt(rt, p->trs);
    p->trs = NULL;
    p->ntr = 0;
    JS_FreeValueRT(rt, p->rstreams);
    JS_FreeValueRT(rt, p->track_evs);
    p->rstreams = p->track_evs = JS_UNDEFINED;
}

static const char* const wjs_meta_keys[] = { "sdpLimits" };
static const char* const wjs_lim_keys[] = { "maxInputBytes", "maxLineBytes", "maxMediaSections", "maxCodecsPerSection",
    "maxHeaderExtensions", "maxCandidatesPerSection", "maxSsrcsPerSection", "maxMsidsPerSection",
    "maxSsrcGroups", "maxSsrcsPerGroup", "maxUnknownLines" };

static int wjs_lim_value(JSContext* ctx, JSValueConst o, const char* k, double cap, double* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "RTCPeerConnection dyna options: sdpLimits.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (isinf(d) && d > 0) {
        *out = cap;
        return 1;
    }
    if (!(d >= 1) || d != floor(d) || d > cap) {
        JS_ThrowRangeError(ctx, "RTCPeerConnection dyna options: sdpLimits.%s must be an integer in 1..%.0f, or Infinity", k, cap);
        return -1;
    }
    *out = d;
    return 1;
}

int wjs_parse_meta(JSContext* ctx, wjs_pc* p, JSValueConst meta)
{
    JSValue l;
    rtc_sdp_limits lim = p->lim;
    double v;
    int r, i;
    if (!JS_IsObject(meta)) {
        JS_ThrowTypeError(ctx, "RTCPeerConnection: dyna options must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, meta, wjs_meta_keys, 1) < 0)
        return -1;
    l = JS_GetPropertyStr(ctx, meta, "sdpLimits");
    if (JS_IsException(l))
        return -1;
    if (JS_IsUndefined(l))
        return 0;
    if (!JS_IsObject(l)) {
        JS_FreeValue(ctx, l);
        JS_ThrowTypeError(ctx, "RTCPeerConnection dyna options: sdpLimits must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, l, wjs_lim_keys, (int)(sizeof(wjs_lim_keys) / sizeof(wjs_lim_keys[0]))) < 0) {
        JS_FreeValue(ctx, l);
        return -1;
    }
    for (i = 0; i < (int)(sizeof(wjs_lim_keys) / sizeof(wjs_lim_keys[0])); i++) {
        double cap = i < 2 ? (double)(1u << 30) : (double)(1 << 24);
        r = wjs_lim_value(ctx, l, wjs_lim_keys[i], cap, &v);
        if (r < 0) {
            JS_FreeValue(ctx, l);
            return -1;
        }
        if (!r)
            continue;
        switch (i) {
        case 0:
            lim.max_input = (size_t)v;
            break;
        case 1:
            lim.line_max = (size_t)v;
            break;
        case 2:
            lim.media = (int)v;
            break;
        case 3:
            lim.codecs = (int)v;
            break;
        case 4:
            lim.ext = (int)v;
            break;
        case 5:
            lim.cands = (int)v;
            break;
        case 6:
            lim.ssrc = (int)v;
            break;
        case 7:
            lim.msid = (int)v;
            break;
        case 8:
            lim.groups = (int)v;
            break;
        case 9:
            lim.group_ssrcs = (int)v;
            break;
        default:
            lim.extra = (int)v;
            break;
        }
    }
    JS_FreeValue(ctx, l);
    p->lim = lim;
    return 0;
}

int wjs_media_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;
    JSClassID cids[2];
    int i;
    if (dyn_register_plain_class(ctx, m, &wjs_track_cid, &wjs_track_class, wjs_track_proto, countof(wjs_track_proto), wjs_illegal_ctor,
            "MediaStreamTrack")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_stream_cid, &wjs_stream_class, wjs_stream_proto, countof(wjs_stream_proto), wjs_stream_ctor,
            "MediaStream")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_tr_cid, &wjs_tr_class, wjs_tr_proto, countof(wjs_tr_proto), wjs_illegal_ctor,
            "RTCRtpTransceiver")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_sender_cid, &wjs_sender_class, wjs_sender_proto, countof(wjs_sender_proto), wjs_illegal_ctor,
            "RTCRtpSender")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_receiver_cid, &wjs_receiver_class, wjs_receiver_proto, countof(wjs_receiver_proto),
            wjs_illegal_ctor, "RTCRtpReceiver")
        < 0)
        return -1;
    cids[0] = wjs_sender_cid;
    cids[1] = wjs_receiver_cid;
    for (i = 0; i < 2; i++) {
        proto = JS_GetClassProto(ctx, cids[i]);
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsFunction(ctx, ctor))
            JS_SetPropertyStr(ctx, ctor, "getCapabilities", JS_NewCFunction(ctx, wjs_capabilities, "getCapabilities", 1));
        JS_FreeValue(ctx, ctor);
    }
    return 0;
}

static int wjs_ext_id(const wjs_tr* t, const char* uri)
{
    int i;
    for (i = 0; i < t->neg.nx; i++)
        if (t->neg.x[i].uri && !strcmp(t->neg.x[i].uri, uri) && t->neg.x[i].id >= 1 && t->neg.x[i].id <= 14)
            return t->neg.x[i].id;
    return 0;
}

static int wjs_codec_media(const char* n)
{
    return n && strcasecmp(n, "rtx") && strcasecmp(n, "red") && strcasecmp(n, "ulpfec") && strcasecmp(n, "flexfec-03")
        && strcasecmp(n, "telephone-event") && strcasecmp(n, "CN");
}

int wjs_tr_te(const wjs_tr* t, uint8_t* pt, uint32_t* clock)
{
    int k, first = -1;
    *pt = WRT_PT_NONE;
    *clock = 0;
    for (k = 0; k < t->neg.n && first < 0; k++)
        if (wjs_codec_media(t->neg.v[k].name))
            first = k;
    for (k = 0; k < t->neg.n; k++) {
        const rtc_sdp_codec* c = &t->neg.v[k];
        if (c->name && !strcasecmp(c->name, "telephone-event")
            && (*pt == WRT_PT_NONE || (first >= 0 && c->clock == t->neg.v[first].clock))) {
            *pt = (uint8_t)c->pt;
            *clock = c->clock;
        }
    }
    return *pt == WRT_PT_NONE ? -1 : 0;
}

void wjs_media_sync(JSContext* ctx, wjs_pc* p)
{
    wrt_pc* w = p->t;
    int i;
    (void)ctx;
    if (!w || (!p->ntr && !w->media) || wrt_media_sync_begin(w) < 0)
        return;
    for (i = 0; i < p->ntr; i++) {
        wjs_tr* t = wjs_tr_get(p->trs[i]);
        wrt_rx_cfg rc;
        wrt_tx_cfg tc;
        wjs_track* rt;
        wjs_track* st;
        wrt_codec_desc cd[WRT_RELAY_CODECS];
        int k, first = -1, nd = 0, nte = 0;
        if (!t || t->removed || t->stopped || t->stopping || !t->has_mid || !t->neg.n)
            continue;
        memset(&rc, 0, sizeof(rc));
        memset(&tc, 0, sizeof(tc));
        memset(rc.te_pt, WRT_PT_NONE, sizeof(rc.te_pt));
        for (k = 0; k < t->neg.n; k++) {
            const rtc_sdp_codec* c = &t->neg.v[k];
            uint64_t* set = c->name && !strcasecmp(c->name, "rtx") ? rc.rtx_pts : rc.pts;
            set[(c->pt & 127) >> 6] |= 1ull << (c->pt & 63);
            if (c->name && !strcasecmp(c->name, "telephone-event") && nte < 2)
                rc.te_pt[nte++] = (uint8_t)c->pt;
            if (nd < WRT_RELAY_CODECS && c->name && strcasecmp(c->name, "rtx")) {
                cd[nd].pt = c->pt;
                cd[nd].clock = c->clock;
                cd[nd].channels = c->channels;
                snprintf(cd[nd].name, sizeof(cd[nd].name), "%s", c->name);
                nd++;
            }
            if (first < 0 && wjs_codec_media(c->name))
                first = k;
        }
        if (first < 0)
            continue;
        tc.clock = t->neg.v[first].clock;
        rt = wjs_track_get(t->recv_track);
        rc.id = tc.id = t->id;
        rc.mid = tc.mid = t->mid;
        rc.clock = t->neg.v[first].clock;
        rc.ssrcs = t->rssrc;
        rc.nssrc = t->nrssrc;
        rc.receptive = (uint8_t)(t->receptive && !t->stopping);
        rc.muted = (uint8_t)(rt && rt->muted && !rt->ended);
        rc.relay = rt ? rt->relay : NULL;
        rc.codecs = cd;
        rc.ncodec = nd;
        rc.mid_ext = tc.mid_ext = (uint8_t)wjs_ext_id(t, "urn:ietf:params:rtp-hdrext:sdes:mid");
        if (!t->kind) {
            rc.alevel_ext = tc.alevel_ext = (uint8_t)wjs_ext_id(t, "urn:ietf:params:rtp-hdrext:ssrc-audio-level");
            rc.clevel_ext = tc.clevel_ext = (uint8_t)wjs_ext_id(t, "urn:ietf:params:rtp-hdrext:csrc-audio-level");
        }
        tc.ssrc = t->ssrc;
        tc.pt = t->neg.v[first].pt;
        tc.live = (uint8_t)(t->has_cur && wjs_is_sending(t->cur_dir) && !t->stopping && !JS_IsNull(t->send_track)
            && !JS_IsUndefined(t->send_track));
        st = wjs_track_get(t->send_track);
        if (st && st->relay) {
            tc.relay = st->relay;
            tc.codecs = cd;
            tc.ncodec = nd;
        }
        wrt_media_rx_set(w, &rc);
        wrt_media_tx_set(w, &tc);
    }
    wrt_media_sync_end(w);
}

void wjs_media_rtp_event(JSContext* ctx, wjs_pc* p, int unmute, uint32_t id)
{
    wjs_tr* t = wjs_tr_by_id(p, id);
    wjs_track* rt;
    if (!t || t->removed || p->closed)
        return;
    rt = wjs_track_get(t->recv_track);
    if (!rt || rt->ended)
        return;
    if (unmute) {
        if (!rt->muted)
            return;
        rt->muted = 0;
        wjs_track_fire(ctx, t->recv_track, WJS_TE_UNMUTE);
    } else {
        if (rt->muted)
            return;
        rt->muted = 1;
        wjs_track_fire(ctx, t->recv_track, WJS_TE_MUTE);
    }
}

wrt_pc* wjs_side_route(JSValueConst v, uint32_t* id)
{
    wjs_side* s = JS_GetOpaque(v, wjs_sender_cid);
    wjs_tr* t;
    if (!s)
        s = JS_GetOpaque(v, wjs_receiver_cid);
    t = s ? wjs_tr_get(s->tr_obj) : NULL;
    if (!t || !t->pc || !t->pc->t)
        return NULL;
    *id = t->id;
    return t->pc->t;
}

void wjs_side_rearm(JSContext* ctx, JSValueConst v)
{
    wjs_side* s = JS_GetOpaque(v, wjs_sender_cid);
    wjs_tr* t;
    if (!s)
        s = JS_GetOpaque(v, wjs_receiver_cid);
    t = s ? wjs_tr_get(s->tr_obj) : NULL;
    if (t && t->pc)
        wjs_pc_rearm_later(ctx, t->pc);
}

#endif
