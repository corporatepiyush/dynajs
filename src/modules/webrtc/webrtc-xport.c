#include "webrtc-js.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modules/libc/libc-internal.h"

JSClassID wjs_dtls_cid, wjs_ice_cid, wjs_dtmf_cid;

enum {
    WJS_XE_STATE,
    WJS_XE_GATHER,
    WJS_XE_PAIR,
    WJS_XE_ERROR,
    WJS_XE_N
};

static const char* const wjs_ice_ev_names[3] = { "statechange", "gatheringstatechange", "selectedcandidatepairchange" };
static const char* const wjs_dtls_ev_names[4] = { "statechange", "", "", "error" };
static const char* const wjs_dtmf_ev_names[1] = { "tonechange" };
static const char* const wjs_ice_state_names[] = { "new", "checking", "connected", "completed", "disconnected", "failed", "closed" };
static const char* const wjs_dtls_state_names[] = { "new", "connecting", "connected", "closed", "failed" };
static const char* const wjs_gather_names[] = { "new", "gathering", "complete" };

typedef struct {
    wjs_et et;
    JSValue pc_obj;
    wjs_pc* p;
    uint8_t last;
    uint8_t last_gather;
} wjs_xport;

typedef struct {
    wjs_et et;
    JSValue tr_obj;
    JSValue fn;
    char* buf;
    uint32_t duration;
    uint32_t gap;
    uint8_t scheduled;
} wjs_dtmf;

static void wjs_xport_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_xport* x = JS_GetOpaque(val, JS_GetClassID(val));
    if (!x)
        return;
    wjs_et_free(rt, &x->et);
    JS_FreeValueRT(rt, x->pc_obj);
    js_free_rt(rt, x);
}

static void wjs_xport_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_xport* x = JS_GetOpaque(val, JS_GetClassID(val));
    if (!x)
        return;
    wjs_et_mark(rt, &x->et, mark);
    JS_MarkValue(rt, x->pc_obj, mark);
}

static JSClassDef wjs_dtls_class = { "RTCDtlsTransport", .finalizer = wjs_xport_finalizer, .gc_mark = wjs_xport_mark };
static JSClassDef wjs_ice_class = { "RTCIceTransport", .finalizer = wjs_xport_finalizer, .gc_mark = wjs_xport_mark };

static JSValue wjs_xport_make(JSContext* ctx, wjs_pc* p, JSClassID cid, int ntypes)
{
    wjs_xport* x = js_mallocz(ctx, sizeof(*x));
    JSValue obj;
    int i;
    if (!x)
        return JS_EXCEPTION;
    x->et.ntypes = ntypes;
    for (i = 0; i < WJS_PE_N; i++)
        x->et.on[i] = JS_NULL;
    x->p = p;
    obj = JS_NewObjectClass(ctx, (int)cid);
    if (JS_IsException(obj)) {
        js_free(ctx, x);
        return obj;
    }
    x->pc_obj = JS_DupValue(ctx, p->self);
    JS_SetOpaque(obj, x);
    return obj;
}

static int wjs_ice_state_now(const wjs_pc* p)
{
    return p->closed ? 6 : p->ice_state % 7;
}

static int wjs_dtls_state_now(const wjs_pc* p)
{
    return p->closed ? WRT_DTLS_CLOSED : p->dtls_st;
}

JSValue wjs_pc_dtls(JSContext* ctx, wjs_pc* p)
{
    if (!JS_IsObject(p->dtls_obj)) {
        JSValue ice = wjs_xport_make(ctx, p, wjs_ice_cid, 3), dtls;
        if (JS_IsException(ice))
            return ice;
        dtls = wjs_xport_make(ctx, p, wjs_dtls_cid, 4);
        if (JS_IsException(dtls)) {
            JS_FreeValue(ctx, ice);
            return dtls;
        }
        ((wjs_xport*)JS_GetOpaque(ice, wjs_ice_cid))->last = (uint8_t)wjs_ice_state_now(p);
        ((wjs_xport*)JS_GetOpaque(ice, wjs_ice_cid))->last_gather = (uint8_t)p->gather_state;
        ((wjs_xport*)JS_GetOpaque(dtls, wjs_dtls_cid))->last = (uint8_t)wjs_dtls_state_now(p);
        p->ice_obj = ice;
        p->dtls_obj = dtls;
    }
    return JS_DupValue(ctx, p->dtls_obj);
}

static void wjs_xfire(JSContext* ctx, JSValueConst obj, wjs_xport* x, int type, JSValue ev)
{
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    wjs_et_fire(ctx, obj, &x->et, type, ev);
    JS_FreeValue(ctx, ev);
}

void wjs_xport_event(JSContext* ctx, wjs_pc* p, int what)
{
    wjs_xport *ix, *dx;
    JSValue ice = p->ice_obj, dtls = p->dtls_obj;
    if (!JS_IsObject(dtls) || p->closed)
        return;
    ix = JS_GetOpaque(ice, wjs_ice_cid);
    dx = JS_GetOpaque(dtls, wjs_dtls_cid);
    if (!ix || !dx)
        return;
    JS_DupValue(ctx, ice);
    JS_DupValue(ctx, dtls);
    if (what == 0 && dx->last != wjs_dtls_state_now(p)) {
        int st = wjs_dtls_state_now(p);
        dx->last = (uint8_t)st;
        if (st == WRT_DTLS_FAILED) {
            JSValue ev = wjs_event(ctx, "error", dtls);
            if (!JS_IsException(ev)) {
                int fpf = p->t && p->t->dtls.fp_failed;
                JS_SetPropertyStr(ctx, ev, "error", wjs_rtc_error(ctx, fpf ? "fingerprint-failure" : "dtls-failure",
                    fpf ? "the peer certificate does not match the signalled a=fingerprint" : "the DTLS handshake failed"));
            }
            wjs_xfire(ctx, dtls, dx, WJS_XE_ERROR, ev);
        }
        wjs_xfire(ctx, dtls, dx, WJS_XE_STATE, wjs_event(ctx, "statechange", dtls));
    } else if (what == 1 && ix->last != wjs_ice_state_now(p)) {
        ix->last = (uint8_t)wjs_ice_state_now(p);
        wjs_xfire(ctx, ice, ix, WJS_XE_STATE, wjs_event(ctx, "statechange", ice));
    } else if (what == 2 && ix->last_gather != p->gather_state) {
        ix->last_gather = (uint8_t)p->gather_state;
        wjs_xfire(ctx, ice, ix, WJS_XE_GATHER, wjs_event(ctx, "gatheringstatechange", ice));
    } else if (what == 3)
        wjs_xfire(ctx, ice, ix, WJS_XE_PAIR, wjs_event(ctx, "selectedcandidatepairchange", ice));
    JS_FreeValue(ctx, ice);
    JS_FreeValue(ctx, dtls);
}

static wjs_xport* wjs_xport_of(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    return JS_GetOpaque2(ctx, v, cid);
}

static JSValue wjs_x_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, magic >= 10 ? wjs_dtls_cid : wjs_ice_cid);
    return x ? wjs_et_on_get(ctx, &x->et, magic % 10) : JS_EXCEPTION;
}

static JSValue wjs_x_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, magic >= 10 ? wjs_dtls_cid : wjs_ice_cid);
    return x ? wjs_et_on_set(ctx, &x->et, magic % 10, v) : JS_EXCEPTION;
}

static JSValue wjs_x_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int dtls = magic >= 10;
    wjs_xport* x = wjs_xport_of(ctx, this_val, dtls ? wjs_dtls_cid : wjs_ice_cid);
    if (!x)
        return JS_EXCEPTION;
    return wjs_et_listen(ctx, &x->et, dtls ? wjs_dtls_ev_names : wjs_ice_ev_names, dtls ? 4 : 3, argc, argv, magic % 10);
}

static JSValue wjs_x_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int dtls = magic >= 10;
    wjs_xport* x = wjs_xport_of(ctx, this_val, dtls ? wjs_dtls_cid : wjs_ice_cid);
    if (!x)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &x->et, dtls ? wjs_dtls_ev_names : wjs_ice_ev_names, dtls ? 4 : 3, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static JSValue wjs_dtls_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_dtls_cid);
    if (!x)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_DupValue(ctx, x->p->ice_obj);
    return JS_NewString(ctx, wjs_dtls_state_names[wjs_dtls_state_now(x->p)]);
}

static JSValue wjs_dtls_certs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_dtls_cid);
    JSValue a;
    uint8_t* der = NULL;
    int n;
    (void)argc;
    (void)argv;
    if (!x)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a) || x->p->closed || !x->p->t || x->p->dtls_st != WRT_DTLS_CONNECTED)
        return a;
    n = wrt_dtls_peer_der(&x->p->t->dtls, &der);
    if (n > 0) {
        JS_SetPropertyUint32(ctx, a, 0, JS_NewArrayBufferCopy(ctx, der, (size_t)n));
        wrt_dtls_der_free(der);
    }
    return a;
}

static const char* wjs_x_mid(const wjs_pc* p, int* ml)
{
    *ml = p->tag_index >= 0 ? p->tag_index : 0;
    if (p->tag_index >= 0 && p->tag_index < p->m.n)
        return p->m.v[p->tag_index].mid;
    return p->pend.n ? p->pend.v[0].mid : "0";
}

static JSValue wjs_x_cand(JSContext* ctx, const wjs_pc* p, const rtc_cand* c, const char* ufrag)
{
    char buf[512];
    int ml;
    const char* mid = wjs_x_mid(p, &ml);
    size_t n = wrt_cand_text(c, buf, sizeof(buf));
    return wjs_cand_new(ctx, buf, n, mid, ml, ufrag);
}

static JSValue wjs_ice_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_ice_cid);
    wjs_pc* p;
    if (!x)
        return JS_EXCEPTION;
    p = x->p;
    switch (magic) {
    case 0:
        if (!p->t || !p->t->ice.started)
            return JS_NewString(ctx, "unknown");
        return JS_NewString(ctx, p->t->ice.controlling ? "controlling" : "controlled");
    case 1:
        return JS_NewString(ctx, "rtp");
    case 2:
        return JS_NewString(ctx, wjs_ice_state_names[wjs_ice_state_now(p)]);
    default:
        return JS_NewString(ctx, wjs_gather_names[p->gather_state % 3]);
    }
}

static JSValue wjs_ice_cands(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_ice_cid);
    JSValue a;
    int i, n;
    uint32_t k = 0;
    (void)argc;
    (void)argv;
    if (!x)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a) || !x->p->t)
        return a;
    n = magic ? x->p->t->ice.nremote : x->p->t->ice.nlocal;
    for (i = 0; i < n; i++) {
        const rtc_cand* c = magic ? &x->p->t->ice.remote[i] : &x->p->t->ice.local[i];
        JSValue v = wjs_x_cand(ctx, x->p, c, magic ? x->p->rufrag : x->p->ufrag);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, a);
            return v;
        }
        JS_SetPropertyUint32(ctx, a, k++, v);
    }
    return a;
}

static JSValue wjs_ice_pair(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_ice_cid);
    const rtc_cand *l, *r;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!x)
        return JS_EXCEPTION;
    if (x->p->closed || !x->p->t || !rtc_ice_get_selected(&x->p->t->ice, &l, &r))
        return JS_NULL;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "local", wjs_x_cand(ctx, x->p, l, x->p->ufrag));
    JS_SetPropertyStr(ctx, o, "remote", wjs_x_cand(ctx, x->p, r, x->p->rufrag));
    return o;
}

static JSValue wjs_ice_params(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_xport* x = wjs_xport_of(ctx, this_val, wjs_ice_cid);
    const char *u, *pw;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!x)
        return JS_EXCEPTION;
    u = magic ? x->p->rufrag : x->p->ufrag;
    pw = magic ? x->p->rpwd : x->p->pwd;
    if (!u[0])
        return JS_NULL;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "usernameFragment", JS_NewString(ctx, u));
    JS_SetPropertyStr(ctx, o, "password", JS_NewString(ctx, pw));
    return o;
}

static const JSCFunctionListEntry wjs_dtls_proto[] = {
    JS_CGETSET_MAGIC_DEF("iceTransport", wjs_dtls_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("state", wjs_dtls_get, NULL, 1),
    JS_CFUNC_DEF("getRemoteCertificates", 0, wjs_dtls_certs),
    JS_CGETSET_MAGIC_DEF("onstatechange", wjs_x_on_get, wjs_x_on_set, 10 + WJS_XE_STATE),
    JS_CGETSET_MAGIC_DEF("onerror", wjs_x_on_get, wjs_x_on_set, 10 + WJS_XE_ERROR),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_x_listen, 10),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_x_listen, 11),
    JS_CFUNC_MAGIC_DEF("dispatchEvent", 1, wjs_x_dispatch, 10),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCDtlsTransport", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry wjs_ice_proto[] = {
    JS_CGETSET_MAGIC_DEF("role", wjs_ice_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("component", wjs_ice_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("state", wjs_ice_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("gatheringState", wjs_ice_get, NULL, 3),
    JS_CFUNC_MAGIC_DEF("getLocalCandidates", 0, wjs_ice_cands, 0),
    JS_CFUNC_MAGIC_DEF("getRemoteCandidates", 0, wjs_ice_cands, 1),
    JS_CFUNC_DEF("getSelectedCandidatePair", 0, wjs_ice_pair),
    JS_CFUNC_MAGIC_DEF("getLocalParameters", 0, wjs_ice_params, 0),
    JS_CFUNC_MAGIC_DEF("getRemoteParameters", 0, wjs_ice_params, 1),
    JS_CGETSET_MAGIC_DEF("onstatechange", wjs_x_on_get, wjs_x_on_set, WJS_XE_STATE),
    JS_CGETSET_MAGIC_DEF("ongatheringstatechange", wjs_x_on_get, wjs_x_on_set, WJS_XE_GATHER),
    JS_CGETSET_MAGIC_DEF("onselectedcandidatepairchange", wjs_x_on_get, wjs_x_on_set, WJS_XE_PAIR),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_x_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_x_listen, 1),
    JS_CFUNC_MAGIC_DEF("dispatchEvent", 1, wjs_x_dispatch, 0),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCIceTransport", JS_PROP_CONFIGURABLE),
};

static void wjs_dtmf_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_dtmf* d = JS_GetOpaque(val, wjs_dtmf_cid);
    if (!d)
        return;
    wjs_et_free(rt, &d->et);
    JS_FreeValueRT(rt, d->tr_obj);
    JS_FreeValueRT(rt, d->fn);
    js_free_rt(rt, d->buf);
    js_free_rt(rt, d);
}

static void wjs_dtmf_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_dtmf* d = JS_GetOpaque(val, wjs_dtmf_cid);
    if (!d)
        return;
    wjs_et_mark(rt, &d->et, mark);
    JS_MarkValue(rt, d->tr_obj, mark);
    JS_MarkValue(rt, d->fn, mark);
}

static JSClassDef wjs_dtmf_class = { "RTCDTMFSender", .finalizer = wjs_dtmf_finalizer, .gc_mark = wjs_dtmf_mark };

JSValue wjs_dtmf_new(JSContext* ctx, JSValueConst tr_obj)
{
    wjs_dtmf* d = js_mallocz(ctx, sizeof(*d));
    JSValue obj;
    int i;
    if (!d)
        return JS_EXCEPTION;
    d->buf = js_mallocz(ctx, 1);
    if (!d->buf) {
        js_free(ctx, d);
        return JS_EXCEPTION;
    }
    d->et.ntypes = 1;
    for (i = 0; i < WJS_PE_N; i++)
        d->et.on[i] = JS_NULL;
    d->duration = 100;
    d->gap = 70;
    d->fn = JS_UNDEFINED;
    obj = JS_NewObjectClass(ctx, (int)wjs_dtmf_cid);
    if (JS_IsException(obj)) {
        js_free(ctx, d->buf);
        js_free(ctx, d);
        return obj;
    }
    d->tr_obj = JS_DupValue(ctx, tr_obj);
    JS_SetOpaque(obj, d);
    return obj;
}

static wjs_dtmf* wjs_dtmf_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_dtmf_cid);
}

static int wjs_dtmf_can(const wjs_dtmf* d)
{
    const wjs_tr* t = wjs_tr_get(d->tr_obj);
    if (!t || t->pc->closed || !wjs_pc_is_connected(t->pc))
        return 0;
    if (t->stopping || JS_IsNull(t->send_track) || !t->has_cur)
        return 0;
    if (t->cur_dir != RTC_DIR_SENDRECV && t->cur_dir != RTC_DIR_SENDONLY)
        return 0;
    if (!t->nenc || !t->enc[0].active)
        return 0;
    return wjs_tr_has_codec(t, "telephone-event");
}

static int wjs_dtmf_schedule(JSContext* ctx, JSValueConst obj, wjs_dtmf* d, double ms);

static int wjs_dtmf_event(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c == '*')
        return 10;
    if (c == '#')
        return 11;
    if (c >= 'A' && c <= 'D')
        return 12 + (c - 'A');
    return -1;
}

static void wjs_dtmf_fire(JSContext* ctx, JSValueConst obj, wjs_dtmf* d, const char* tone)
{
    JSValue ev = wjs_event(ctx, "tonechange", obj);
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    JS_SetPropertyStr(ctx, ev, "tone", JS_NewString(ctx, tone));
    wjs_et_fire(ctx, obj, &d->et, 0, ev);
    JS_FreeValue(ctx, ev);
}

static JSValue wjs_dtmf_task(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    JSValueConst obj = data[0];
    wjs_dtmf* d = JS_GetOpaque(obj, wjs_dtmf_cid);
    char tone[2];
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (!d)
        return JS_UNDEFINED;
    d->scheduled = 0;
    if (!wjs_dtmf_can(d))
        return JS_UNDEFINED;
    if (!d->buf[0]) {
        wjs_dtmf_fire(ctx, obj, d, "");
        return JS_UNDEFINED;
    }
    tone[0] = d->buf[0];
    tone[1] = 0;
    memmove(d->buf, d->buf + 1, strlen(d->buf));
    if (wjs_dtmf_event(tone[0]) >= 0) {
        wjs_tr* t = wjs_tr_get(d->tr_obj);
        uint8_t tp;
        uint32_t tcl;
        if (t && t->pc->t && wjs_tr_te(t, &tp, &tcl) == 0
            && wrt_media_dtmf(t->pc->t, t->id, wjs_dtmf_event(tone[0]), d->duration, tp, tcl, wrt_now_us()) == 0)
            wjs_pc_rearm_later(ctx, t->pc);
    }
    if (wjs_dtmf_schedule(ctx, obj, d, tone[0] == ',' ? 2000.0 : (double)d->duration + (double)d->gap) < 0)
        return JS_EXCEPTION;
    wjs_dtmf_fire(ctx, obj, d, tone);
    return JS_UNDEFINED;
}

static int wjs_dtmf_schedule(JSContext* ctx, JSValueConst obj, wjs_dtmf* d, double ms)
{
    JSValueConst a[2];
    JSValue timer;
    if (JS_IsUndefined(d->fn)) {
        d->fn = JS_NewCFunctionData(ctx, wjs_dtmf_task, 0, 0, 1, &obj);
        if (JS_IsException(d->fn)) {
            d->fn = JS_UNDEFINED;
            return -1;
        }
    }
    a[0] = d->fn;
    a[1] = JS_NewFloat64(ctx, ms);
    timer = js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
    if (JS_IsException(timer))
        return -1;
    JS_FreeValue(ctx, timer);
    d->scheduled = 1;
    return 0;
}

static int wjs_webidl_ulong(JSContext* ctx, JSValueConst v, uint32_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v) < 0)
        return -1;
    if (!isfinite(x)) {
        *out = 0;
        return 0;
    }
    x = trunc(x);
    x = fmod(x, 4294967296.0);
    if (x < 0)
        x += 4294967296.0;
    *out = (uint32_t)x;
    return 0;
}

static JSValue wjs_dtmf_insert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    const char* s;
    size_t n, i;
    uint32_t dur = 100, gap = 70;
    char* nb;
    if (!d)
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argc ? argv[0] : JS_UNDEFINED);
    if (!s)
        return JS_EXCEPTION;
    if ((argc > 1 && !JS_IsUndefined(argv[1]) && wjs_webidl_ulong(ctx, argv[1], &dur) < 0)
        || (argc > 2 && !JS_IsUndefined(argv[2]) && wjs_webidl_ulong(ctx, argv[2], &gap) < 0)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (!wjs_dtmf_can(d)) {
        JS_FreeCString(ctx, s);
        return js_throw_dom_exception(ctx, "InvalidStateError", "insertDTMF: DTMF cannot be sent on this sender now");
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'D') || (c >= 'a' && c <= 'd') || c == '#' || c == '*' || c == ',')) {
            JS_FreeCString(ctx, s);
            return js_throw_dom_exception(ctx, "InvalidCharacterError", "insertDTMF: tones may only hold 0-9, A-D, #, * and ','");
        }
    }
    nb = js_malloc(ctx, n + 1);
    if (!nb) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        nb[i] = (s[i] >= 'a' && s[i] <= 'd') ? (char)(s[i] - 32) : s[i];
    nb[n] = 0;
    JS_FreeCString(ctx, s);
    js_free(ctx, d->buf);
    d->buf = nb;
    d->duration = dur < 40 ? 40 : dur > 6000 ? 6000 : dur;
    d->gap = gap < 30 ? 30 : gap > 6000 ? 6000 : gap;
    if (!n || d->scheduled)
        return JS_UNDEFINED;
    if (wjs_dtmf_schedule(ctx, this_val, d, 0) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue wjs_dtmf_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    return magic ? JS_NewString(ctx, d->buf) : JS_NewBool(ctx, wjs_dtmf_can(d));
}

static JSValue wjs_dtmf_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    return d ? wjs_et_on_get(ctx, &d->et, magic) : JS_EXCEPTION;
}

static JSValue wjs_dtmf_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    return d ? wjs_et_on_set(ctx, &d->et, magic, v) : JS_EXCEPTION;
}

static JSValue wjs_dtmf_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    return d ? wjs_et_listen(ctx, &d->et, wjs_dtmf_ev_names, 1, argc, argv, magic) : JS_EXCEPTION;
}

static JSValue wjs_dtmf_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_dtmf* d = wjs_dtmf_of(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &d->et, wjs_dtmf_ev_names, 1, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static const JSCFunctionListEntry wjs_dtmf_proto[] = {
    JS_CFUNC_DEF("insertDTMF", 1, wjs_dtmf_insert),
    JS_CGETSET_MAGIC_DEF("canInsertDTMF", wjs_dtmf_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("toneBuffer", wjs_dtmf_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("ontonechange", wjs_dtmf_on_get, wjs_dtmf_on_set, 0),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_dtmf_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_dtmf_listen, 1),
    JS_CFUNC_DEF("dispatchEvent", 1, wjs_dtmf_dispatch),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCDTMFSender", JS_PROP_CONFIGURABLE),
};

static JSValue wjs_x_illegal(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

int wjs_xport_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &wjs_dtls_cid, &wjs_dtls_class, wjs_dtls_proto, countof(wjs_dtls_proto), wjs_x_illegal,
            "RTCDtlsTransport")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &wjs_ice_cid, &wjs_ice_class, wjs_ice_proto, countof(wjs_ice_proto), wjs_x_illegal,
            "RTCIceTransport")
        < 0)
        return -1;
    return dyn_register_plain_class(ctx, m, &wjs_dtmf_cid, &wjs_dtmf_class, wjs_dtmf_proto, countof(wjs_dtmf_proto), wjs_x_illegal,
        "RTCDTMFSender");
}

#endif
