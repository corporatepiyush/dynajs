#include "webrtc-js.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modules/libc/libc-internal.h"
#include "core/dyn-prng.h"
#include "../../dyna-aio.h"

JSClassID wjs_pc_cid;

static void wjs_sdp_del(JSRuntime* rt, rtc_sdp** s);

static const char* const wjs_pc_names[WJS_PE_N] = { "icecandidate", "icecandidateerror", "signalingstatechange",
    "iceconnectionstatechange", "icegatheringstatechange", "connectionstatechange", "datachannel", "negotiationneeded",
    "track" };
static const char* const wjs_sig_names[] = { "stable", "have-local-offer", "have-remote-offer", "have-local-pranswer",
    "have-remote-pranswer", "closed" };
static const char* const wjs_ice_names[] = { "new", "checking", "connected", "completed", "disconnected", "failed",
    "closed" };
static const char* const wjs_conn_names[] = { "new", "connecting", "connected", "disconnected", "failed", "closed" };
static const char* const wjs_gather_names[] = { "new", "gathering", "complete" };

enum {
    WJS_CONN_NEW,
    WJS_CONN_CONNECTING,
    WJS_CONN_CONNECTED,
    WJS_CONN_DISCONNECTED,
    WJS_CONN_FAILED,
    WJS_CONN_CLOSED
};

enum {
    WJS_OP_OFFER,
    WJS_OP_ANSWER,
    WJS_OP_SLD,
    WJS_OP_SRD,
    WJS_OP_ADDCAND,
    WJS_OP_STATS,
    WJS_OP_REPLACE
};

typedef struct wjs_op {
    int kind;
    JSValue arg;
    JSValue res[2];
    struct wjs_op* next;
} wjs_op;

static wjs_pc* wjs_pc_of(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, wjs_pc_cid);
}

static void wjs_desc_clear(JSRuntime* rt, wjs_desc* d)
{
    js_free_rt(rt, d->sdp);
    d->sdp = NULL;
}

static void wjs_pc_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void wjs_pc_unpin(JSRuntime* rt, wjs_pc* p)
{
    if (!JS_IsUndefined(p->pin)) {
        JSValue v = p->pin;
        p->pin = JS_UNDEFINED;
        JS_RemoveShutdownSweep(rt, wjs_pc_sweep, p);
        JS_FreeValueRT(rt, v);
    }
}

static void wjs_pc_pin(JSContext* ctx, wjs_pc* p)
{
    if (JS_IsUndefined(p->pin) && !p->closed) {
        p->pin = JS_DupValue(ctx, p->self);
        JS_AddShutdownSweep(JS_GetRuntime(ctx), wjs_pc_sweep, p);
    }
}

static int wjs_dcs_live(const wjs_pc* p)
{
    uint32_t i;
    for (i = 0; p->t && i < p->t->ndcs; i++) {
        const wrt_dc* dc = p->t->dcs[i];
        if (dc && dc->js && ((const wjs_dc*)dc->js)->state != WRT_DC_CLOSED)
            return 1;
    }
    return 0;
}

static void wjs_timer_clear(JSContext* ctx, wjs_pc* p)
{
    if (!JS_IsUndefined(p->timer) && !JS_IsNull(p->timer)) {
        if (JS_GetRuntimeOpaque(JS_GetRuntime(ctx))) {
            JSValue r = js_os_clearTimeout(ctx, JS_UNDEFINED, 1, (JSValueConst*)&p->timer);
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, p->timer);
    }
    p->timer = JS_UNDEFINED;
    p->timer_at = 0;
}

static void wjs_pc_shutdown(JSContext* ctx, wjs_pc* p)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    wjs_timer_clear(ctx, p);
    if (p->t) {
        wrt_pc_close(p->t);
        p->t->notify = NULL;
    }
    if (p->reactor) {
        p->reactor = 0;
        dyn_net_reactor_release_rt(rt);
    }
    wjs_dc_detach_all(rt, p);
}

static void wjs_pc_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    wjs_pc* p = (wjs_pc*)opaque;
    (void)rt;
    p->closed = 1;
    p->signaling = WJS_SIG_CLOSED;
    wjs_pc_shutdown(ctx, p);
    if (!JS_IsUndefined(p->pin)) {
        JSValue v = p->pin;
        p->pin = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static void wjs_pc_finalizer(JSRuntime* rt, JSValue val)
{
    wjs_pc* p = JS_GetOpaque(val, wjs_pc_cid);
    if (!p)
        return;
    if (p->reactor) {
        p->reactor = 0;
        dyn_net_reactor_release_rt(rt);
    }
    wjs_dc_detach_all(rt, p);
    if (p->t) {
        p->t->notify = NULL;
        p->t->js = NULL;
        wrt_pc_free(p->t);
        p->t = NULL;
    }
    while (p->op_head) {
        wjs_op* o = p->op_head;
        p->op_head = o->next;
        JS_FreeValueRT(rt, o->arg);
        JS_FreeValueRT(rt, o->res[0]);
        JS_FreeValueRT(rt, o->res[1]);
        js_free_rt(rt, o);
    }
    wjs_et_free(rt, &p->et);
    JS_FreeValueRT(rt, p->timer);
    JS_FreeValueRT(rt, p->tick_fn);
    JS_FreeValueRT(rt, p->config);
    JS_FreeValueRT(rt, p->sctp_obj);
    JS_FreeValueRT(rt, p->dtls_obj);
    JS_FreeValueRT(rt, p->ice_obj);
    JS_FreeValueRT(rt, p->certs);
    wjs_desc_clear(rt, &p->cur_local);
    wjs_desc_clear(rt, &p->pend_local);
    wjs_desc_clear(rt, &p->cur_remote);
    wjs_desc_clear(rt, &p->pend_remote);
    js_free_rt(rt, p->last_offer);
    js_free_rt(rt, p->scands);
    js_free_rt(rt, p->last_answer);
    wjs_media_free(rt, p);
    js_free_rt(rt, p->m.v);
    js_free_rt(rt, p->pend.v);
    js_free_rt(rt, p->offer.v);
    wjs_sdp_del(rt, &p->roffer);
    wjs_sdp_del(rt, &p->lanswer);
    if (p->cert)
        wrt_cert_unref(p->cert);
    js_free_rt(rt, p);
}

static void wjs_pc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    wjs_pc* p = JS_GetOpaque(val, wjs_pc_cid);
    wjs_op* o;
    if (!p)
        return;
    wjs_et_mark(rt, &p->et, mark);
    JS_MarkValue(rt, p->tick_fn, mark);
    JS_MarkValue(rt, p->config, mark);
    JS_MarkValue(rt, p->sctp_obj, mark);
    JS_MarkValue(rt, p->dtls_obj, mark);
    JS_MarkValue(rt, p->ice_obj, mark);
    JS_MarkValue(rt, p->certs, mark);
    wjs_media_mark(rt, p, mark);
    for (o = p->op_head; o; o = o->next) {
        JS_MarkValue(rt, o->arg, mark);
        JS_MarkValue(rt, o->res[0], mark);
        JS_MarkValue(rt, o->res[1], mark);
    }
}

static JSClassDef wjs_pc_class = { "RTCPeerConnection", .finalizer = wjs_pc_finalizer, .gc_mark = wjs_pc_mark };

static void wjs_fire(JSContext* ctx, wjs_pc* p, int type, JSValue ev)
{
    if (JS_IsException(ev)) {
        js_std_dump_error(ctx);
        return;
    }
    wjs_et_fire(ctx, p->self, &p->et, type, ev);
    JS_FreeValue(ctx, ev);
}

static void wjs_fire_simple(JSContext* ctx, wjs_pc* p, int type)
{
    wjs_fire(ctx, p, type, wjs_event(ctx, wjs_pc_names[type], p->self));
}

static void wjs_clear_last(JSContext* ctx, wjs_pc* p)
{
    js_free(ctx, p->last_offer);
    js_free(ctx, p->last_answer);
    p->last_offer = NULL;
    p->last_answer = NULL;
}

static void wjs_set_signaling(JSContext* ctx, wjs_pc* p, int s)
{
    if (p->signaling == s)
        return;
    p->signaling = s;
    wjs_fire_simple(ctx, p, WJS_PE_SIGNALING);
}

static int wjs_conn_compute(wjs_pc* p)
{
    int ice = p->ice_state, dtls = p->t ? p->t->dtls.state : WRT_DTLS_NEW;
    if (p->closed)
        return WJS_CONN_CLOSED;
    if (ice == RTC_ICE_FAILED || dtls == WRT_DTLS_FAILED)
        return WJS_CONN_FAILED;
    if (ice == RTC_ICE_DISCONNECTED)
        return WJS_CONN_DISCONNECTED;
    if ((ice == RTC_ICE_NEW || ice == RTC_ICE_CLOSED) && (dtls == WRT_DTLS_NEW || dtls == WRT_DTLS_CLOSED))
        return WJS_CONN_NEW;
    if ((ice == RTC_ICE_CONNECTED || ice == RTC_ICE_COMPLETED) && (dtls == WRT_DTLS_CONNECTED || dtls == WRT_DTLS_CLOSED))
        return WJS_CONN_CONNECTED;
    return WJS_CONN_CONNECTING;
}

static void wjs_update_conn(JSContext* ctx, wjs_pc* p)
{
    int c = wjs_conn_compute(p);
    if (c == p->conn_state)
        return;
    p->conn_state = c;
    wjs_fire_simple(ctx, p, WJS_PE_CONN);
}

int wjs_pc_is_connected(const wjs_pc* p)
{
    return !p->closed && p->conn_state == WJS_CONN_CONNECTED;
}

static void wjs_sctp_obj_update(JSContext* ctx, wjs_pc* p)
{
    static const char* const st[] = { "connecting", "connected", "closed" };
    double mm;
    if (!JS_IsObject(p->sctp_obj))
        return;
    JS_SetPropertyStr(ctx, p->sctp_obj, "state", JS_NewString(ctx, st[p->sctp_state]));
    mm = p->t && p->t->remote_max_msg ? (double)(p->t->remote_max_msg < p->t->local_max_msg ? p->t->remote_max_msg
                                                                                          : p->t->local_max_msg)
                                      : (p->t ? (double)p->t->local_max_msg : 65536.0);
    JS_SetPropertyStr(ctx, p->sctp_obj, "maxMessageSize", JS_NewFloat64(ctx, mm));
    if (p->sctp_state == 1 && p->t && p->t->sctp)
        JS_SetPropertyStr(ctx, p->sctp_obj, "maxChannels",
            JS_NewInt32(ctx, (int32_t)rtc_sctp_max_channels(p->t->sctp)));
}

static void wjs_fire_cand(JSContext* ctx, wjs_pc* p, int idx)
{
    char buf[512];
    size_t n = idx >= 0 ? wrt_pc_cand_text(p->t, idx, buf, sizeof(buf)) : 0;
    JSValue ev = wjs_event(ctx, "icecandidate", p->self), c;
    const char* mid = p->tag_index >= 0 && p->tag_index < p->m.n ? p->m.v[p->tag_index].mid : (p->pend.n ? p->pend.v[0].mid : "0");
    int ml = p->tag_index >= 0 ? p->tag_index : 0;
    if (JS_IsException(ev))
        return;
    if (p->pend.n && p->tag_index < 0) {
        int i;
        for (i = 0; i < p->pend.n; i++)
            if (!p->pend.v[i].rejected) {
                mid = p->pend.v[i].mid;
                ml = i;
                break;
            }
    }
    if (idx >= 0 && !n) {
        JS_FreeValue(ctx, ev);
        return;
    }
    c = idx >= 0 ? wjs_cand_new(ctx, buf, n, mid, ml, p->lrestart ? p->pend_ufrag : p->ufrag)
        : idx == -2 ? wjs_cand_new(ctx, "", 0, mid, ml, p->lrestart ? p->pend_ufrag : p->ufrag)
                    : JS_NULL;
    if (JS_IsException(c)) {
        JS_FreeValue(ctx, ev);
        js_std_dump_error(ctx);
        return;
    }
    JS_SetPropertyStr(ctx, ev, "candidate", c);
    JS_SetPropertyStr(ctx, ev, "url", JS_NULL);
    wjs_fire(ctx, p, WJS_PE_ICECANDIDATE, ev);
}

static void wjs_handle_ev(JSContext* ctx, wjs_pc* p, const wrt_ev* e)
{
    switch (e->type) {
    case WRT_EV_CANDIDATE:
        if (p->gather_state != 1) {
            p->gather_state = 1;
            wjs_xport_event(ctx, p, 2);
            wjs_fire_simple(ctx, p, WJS_PE_GATHERING);
        }
        wjs_fire_cand(ctx, p, (int)e->a);
        break;
    case WRT_EV_GATHER_DONE:
        wjs_fire_cand(ctx, p, -2);
        if (p->gather_state != 2) {
            p->gather_state = 2;
            wjs_xport_event(ctx, p, 2);
            wjs_fire_simple(ctx, p, WJS_PE_GATHERING);
        }
        wjs_fire_cand(ctx, p, -1);
        break;
    case WRT_EV_CAND_ERROR: {
        JSValue ev = wjs_event(ctx, "icecandidateerror", p->self);
        if (!JS_IsException(ev) && e->id < p->t->nservers) {
            int code = e->a ? (int)e->a : 701;
            JS_SetPropertyStr(ctx, ev, "address", JS_NULL);
            JS_SetPropertyStr(ctx, ev, "port", JS_NULL);
            JS_SetPropertyStr(ctx, ev, "url", JS_NewString(ctx, p->t->servers[e->id].url));
            JS_SetPropertyStr(ctx, ev, "errorCode", JS_NewInt32(ctx, code));
            JS_SetPropertyStr(ctx, ev, "errorText",
                JS_NewString(ctx, e->flag ? "STUN/TURN session limit (64) reached" : code == 701 ? "STUN/TURN server unreachable" : code == 401 ? "Unauthorized" : "Allocation failed"));
        }
        wjs_fire(ctx, p, WJS_PE_ICECANDIDATEERROR, ev);
        break;
    }
    case WRT_EV_ICE_STATE:
        if (p->ice_state != (int)e->a && !p->closed) {
            p->ice_state = (int)e->a;
            wjs_xport_event(ctx, p, 1);
            wjs_fire_simple(ctx, p, WJS_PE_ICECONN);
            wjs_update_conn(ctx, p);
        }
        break;
    case WRT_EV_DTLS_STATE:
        p->dtls_st = (uint8_t)e->a;
        wjs_xport_event(ctx, p, 0);
        if ((int)e->a == WRT_DTLS_CLOSED || (int)e->a == WRT_DTLS_FAILED) {
            p->sctp_state = 2;
            wjs_sctp_obj_update(ctx, p);
        }
        wjs_update_conn(ctx, p);
        break;
    case WRT_EV_PAIR:
        wjs_xport_event(ctx, p, 3);
        break;
    case WRT_EV_RTP_UNMUTE:
    case WRT_EV_RTP_MUTE:
        wjs_media_rtp_event(ctx, p, e->type == WRT_EV_RTP_UNMUTE, e->id);
        break;
    case WRT_EV_SCTP_UP:
        p->sctp_state = 1;
        wjs_sctp_obj_update(ctx, p);
        break;
    case WRT_EV_SCTP_DOWN:
        p->sctp_state = 2;
        wjs_sctp_obj_update(ctx, p);
        break;
    case WRT_EV_DC_NEW: {
        wrt_dc* dc = e->id < p->t->ndcs ? p->t->dcs[e->id] : NULL;
        JSValue obj, ev;
        if (!dc || dc->js)
            break;
        obj = wjs_dc_new(ctx, p, dc);
        if (JS_IsException(obj)) {
            js_std_dump_error(ctx);
            break;
        }
        ((wjs_dc*)dc->js)->state = dc->state == WRT_DC_OPEN ? WRT_DC_OPEN : WRT_DC_CONNECTING;
        ev = wjs_event(ctx, "datachannel", p->self);
        if (!JS_IsException(ev))
            JS_SetPropertyStr(ctx, ev, "channel", JS_DupValue(ctx, obj));
        wjs_fire(ctx, p, WJS_PE_DATACHANNEL, ev);
        JS_FreeValue(ctx, obj);
        if (dc->js && dc->state == WRT_DC_OPEN) {
            wrt_ev oe;
            memset(&oe, 0, sizeof(oe));
            oe.type = WRT_EV_DC_OPEN;
            oe.id = e->id;
            oe.flag = 1;
            wjs_dc_on_event(ctx, p, &oe);
        }
        break;
    }
    case WRT_EV_DC_OPEN:
    case WRT_EV_DC_MSG:
    case WRT_EV_DC_CLOSING:
    case WRT_EV_DC_CLOSED:
    case WRT_EV_DC_BUFLOW:
    case WRT_EV_DC_ERROR:
        wjs_dc_on_event(ctx, p, e);
        if (e->type == WRT_EV_DC_CLOSED && !p->reactor && !wjs_dcs_live(p))
            wjs_pc_unpin(JS_GetRuntime(ctx), p);
        break;
    default:
        break;
    }
}

static void wjs_checkpoint(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSContext* jc = NULL;
    int err;
    if (js_std_job_error_pending_rt(rt))
        return;
    while ((err = JS_ExecutePendingJob(rt, &jc)) > 0) {
    }
    if (err < 0)
        js_std_set_job_error_rt(rt, jc ? JS_GetException(jc) : JS_UNINITIALIZED);
}

void wjs_pc_drain(JSContext* ctx, wjs_pc* p)
{
    uint32_t i;
    JSValue hold;
    if (!p->t || p->draining)
        return;
    hold = JS_DupValue(ctx, p->self);
    p->draining = 1;
    p->t->in_input++;
    for (i = 0; p->t && i < p->t->nev; i++) {
        wrt_ev e = p->t->evs[i];
        wjs_handle_ev(ctx, p, &e);
        if (p->checkpoint && p->t && i + 1 < p->t->nev)
            wjs_checkpoint(ctx);
    }
    if (p->t) {
        wrt_pc_events_done(p->t);
        p->t->in_input--;
        if (!p->t->in_input && p->t->free_pending) {
            wrt_pc_free(p->t);
            p->t = NULL;
        }
    }
    p->draining = 0;
    JS_FreeValue(ctx, hold);
}

static JSValue wjs_tick_cb(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    wjs_pc* p = JS_GetOpaque(data[0], wjs_pc_cid);
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (!p)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, p->timer);
    p->timer = JS_UNDEFINED;
    p->timer_at = 0;
    p->checkpoint = 1;
    wjs_pc_rearm(ctx, p);
    p->checkpoint = 0;
    return JS_UNDEFINED;
}

static void wjs_pc_arm(JSContext* ctx, wjs_pc* p, int drain_now);

void wjs_pc_rearm(JSContext* ctx, wjs_pc* p)
{
    wjs_pc_arm(ctx, p, 1);
}

void wjs_pc_rearm_later(JSContext* ctx, wjs_pc* p)
{
    wjs_pc_arm(ctx, p, 0);
}

static void wjs_pc_arm(JSContext* ctx, wjs_pc* p, int drain_now)
{
    uint64_t now, next;
    JSValueConst a[2];
    double ms;
    if (!p->t || p->closed)
        return;
    now = wrt_now_us();
    next = wrt_pc_tick(p->t, now);
    if (drain_now)
        wjs_pc_drain(ctx, p);
    else if (p->t->nev)
        next = now;
    if (!p->t || p->closed)
        return;
    if (p->t->sctp)
        wrt_pc_flush(p->t);
    if (next == UINT64_MAX)
        return;
    if (!JS_IsUndefined(p->timer) && p->timer_at && p->timer_at <= next + 1000)
        return;
    wjs_timer_clear(ctx, p);
    if (JS_IsUndefined(p->tick_fn)) {
        JSValue d = p->self;
        p->tick_fn = JS_NewCFunctionData(ctx, wjs_tick_cb, 0, 0, 1, &d);
        if (JS_IsException(p->tick_fn)) {
            p->tick_fn = JS_UNDEFINED;
            return;
        }
    }
    ms = next > now ? (double)((next - now + 999) / 1000) : 0;
    a[0] = p->tick_fn;
    a[1] = JS_NewFloat64(ctx, ms);
    p->timer = js_os_setTimeout(ctx, JS_UNDEFINED, 2, a, 0);
    if (JS_IsException(p->timer)) {
        p->timer = JS_UNDEFINED;
        js_std_dump_error(ctx);
        return;
    }
    p->timer_at = next;
}

static void wjs_notify(wrt_pc* t)
{
    wjs_pc* p = (wjs_pc*)t->js;
    if (!p)
        return;
    {
        JSContext* ctx = p->ctx;
        JSValue hold = JS_DupValue(ctx, p->self);
        p->checkpoint = 1;
        wjs_pc_drain(ctx, p);
        p->checkpoint = 0;
        if (p->t)
            wjs_pc_rearm(ctx, p);
        JS_FreeValue(ctx, hold);
    }
}

static JSValue wjs_flush_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    wjs_pc* p = JS_GetOpaque(argv[0], wjs_pc_cid);
    (void)argc;
    if (!p)
        return JS_UNDEFINED;
    p->flush_queued = 0;
    if (p->t && !p->closed) {
        wrt_pc_flush(p->t);
        wjs_pc_rearm(ctx, p);
    }
    return JS_UNDEFINED;
}

void wjs_pc_schedule_flush(JSContext* ctx, wjs_pc* p)
{
    if (p->flush_queued || p->closed)
        return;
    p->flush_queued = 1;
    if (JS_EnqueueJob(ctx, wjs_flush_job, 1, (JSValueConst*)&p->self) < 0)
        p->flush_queued = 0;
}

static void wjs_rand_str(wjs_pc* p, char* out, size_t n)
{
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t r[32];
    size_t i;
    (void)p;
    if (dyn_os_entropy(r, n) < 0)
        rtc_rng_bytes(&p->t->rng, r, n);
    for (i = 0; i < n; i++)
        out[i] = al[r[i] & 63];
    out[n] = 0;
}

static void wjs_new_creds(wjs_pc* p, char* uf, char* pw)
{
    wjs_rand_str(p, uf, 4);
    wjs_rand_str(p, pw, 24);
}

static int wjs_ml_reserve(JSContext* ctx, wjs_ml* l, int n)
{
    wjs_m* v;
    int c;
    if (n <= l->cap)
        return 0;
    c = l->cap ? l->cap : 4;
    while (c < n)
        c *= 2;
    v = js_realloc(ctx, l->v, sizeof(wjs_m) * (size_t)c);
    if (!v)
        return -1;
    l->v = v;
    l->cap = c;
    return 0;
}

static int wjs_ml_copy(JSContext* ctx, wjs_ml* d, const wjs_ml* s)
{
    if (d == s)
        return 0;
    if (wjs_ml_reserve(ctx, d, s->n) < 0)
        return -1;
    if (s->n)
        memcpy(d->v, s->v, sizeof(wjs_m) * (size_t)s->n);
    d->n = s->n;
    return 0;
}

static wjs_m* wjs_ml_push(JSContext* ctx, wjs_ml* l)
{
    if (wjs_ml_reserve(ctx, l, l->n + 1) < 0)
        return NULL;
    memset(&l->v[l->n], 0, sizeof(wjs_m));
    return &l->v[l->n++];
}

static int wjs_has_app(const wjs_ml* l)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->v[i].app && !l->v[i].rejected)
            return 1;
    return 0;
}

static int wjs_first_live(const wjs_ml* l)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!l->v[i].rejected)
            return i;
    return -1;
}

static int wjs_have_dcs(wjs_pc* p)
{
    uint32_t i;
    if (!p->t)
        return 0;
    for (i = 0; i < p->t->ndcs; i++)
        if (p->t->dcs[i])
            return 1;
    return 0;
}

static int wjs_hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static size_t wjs_fp_bytes(const char* t, uint8_t* out, size_t cap)
{
    size_t n = 0;
    while (t[0] && t[1] && n < cap) {
        int a = wjs_hexv(t[0]), b = wjs_hexv(t[1]);
        if (a < 0 || b < 0)
            break;
        out[n++] = (uint8_t)(a * 16 + b);
        t += 2;
        if (*t == ':')
            t++;
    }
    return n;
}

static void wjs_jsep_params(wjs_pc* p, rtc_jsep_params* prm, int setup, const char* uf, const char* pw, uint8_t* fp)
{
    memset(prm, 0, sizeof(*prm));
    prm->sess_id = p->sess_id;
    prm->sess_ver = p->sess_ver;
    prm->ufrag = uf;
    prm->pwd = pw;
    prm->fp = fp;
    prm->fp_len = (uint8_t)wjs_fp_bytes(p->fp_text, fp, 64);
    prm->fp_alg = RTC_FP_SHA256;
    prm->setup = (uint8_t)setup;
    prm->trickle = 1;
    prm->sctp_port = p->t->sctp_port;
    prm->max_msg = (uint32_t)p->t->local_max_msg;
}

static char* wjs_sdp_text(JSContext* ctx, rtc_sdp* s)
{
    rtc_sb b;
    char* out = NULL;
    rtc_sb_init(&b);
    if (rtc_sdp_write(s, &b) == 0 && !b.err) {
        out = js_malloc(ctx, b.len + 1);
        if (out) {
            memcpy(out, b.buf, b.len);
            out[b.len] = 0;
        }
    } else
        JS_ThrowOutOfMemory(ctx);
    rtc_sb_free(&b);
    return out;
}

static rtc_sdp* wjs_sdp_new(JSContext* ctx, wjs_pc* p)
{
    rtc_sdp* s = js_malloc(ctx, sizeof(*s));
    if (s) {
        rtc_sdp_init(s);
        s->lim = p->lim;
    }
    return s;
}

static void wjs_sdp_del(JSRuntime* rt, rtc_sdp** s)
{
    if (*s) {
        rtc_sdp_free(*s);
        js_free_rt(rt, *s);
        *s = NULL;
    }
}

static JSValue wjs_desc_with_cands(JSContext* ctx, wjs_pc* p, const wjs_desc* d)
{
    rtc_sb b;
    const wjs_ml* l = d == &p->pend_local && p->pend.n ? &p->pend : &p->m;
    int first;
    const char *s = d->sdp, *ins = NULL;
    JSValue r;
    char key[64];
    size_t kl;
    if (!s)
        return JS_NULL;
    first = wjs_first_live(l);
    if (first < 0 || !p->t || (!p->t->ice.nlocal && !p->t->gather_done))
        return wjs_desc_new(ctx, d->type, s, strlen(s));
    kl = (size_t)snprintf(key, sizeof(key), "a=mid:%s\r\n", l->v[first].mid);
    ins = strstr(s, key);
    if (!ins)
        return wjs_desc_new(ctx, d->type, s, strlen(s));
    ins += kl;
    {
        const char* nx = strstr(ins, "\r\nm=");
        ins = nx ? nx + 2 : s + strlen(s);
    }
    rtc_sb_init(&b);
    rtc_sb_put(&b, s, (size_t)(ins - s));
    {
        int i;
        for (i = 0; i < p->t->ice.nlocal; i++) {
            char buf[512];
            size_t n = wrt_pc_local_shown(p->t, i) ? wrt_pc_cand_text(p->t, i, buf, sizeof(buf)) : 0;
            if (n) {
                rtc_sb_str(&b, "a=");
                rtc_sb_put(&b, buf, n);
                rtc_sb_str(&b, "\r\n");
            }
        }
    }
    if (p->t->gather_done)
        rtc_sb_str(&b, "a=end-of-candidates\r\n");
    rtc_sb_str(&b, ins);
    r = b.err ? JS_ThrowOutOfMemory(ctx) : wjs_desc_new(ctx, d->type, b.buf, b.len);
    rtc_sb_free(&b);
    return r;
}

static JSValue wjs_make_init(JSContext* ctx, int type, const char* sdp)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, wjs_sdp_type_name(type)));
    JS_SetPropertyStr(ctx, o, "sdp", JS_NewString(ctx, sdp));
    return o;
}

static void wjs_next_mid(wjs_pc* p, const wjs_ml* l, char* out)
{
    int k;
    for (k = p->mid_floor;; k++) {
        int i, used = 0;
        char tmp[RTC_SDP_MID_MAX];
        snprintf(tmp, sizeof(tmp), "%d", k);
        for (i = 0; i < l->n; i++)
            if (!strcmp(l->v[i].mid, tmp))
                used = 1;
        for (i = 0; i < p->ntr && !used; i++) {
            wjs_tr* t = wjs_tr_get(p->trs[i]);
            if (t && t->has_mid && !strcmp(t->mid, tmp))
                used = 1;
        }
        if (!used) {
            memcpy(out, tmp, strlen(tmp) + 1);
            p->mid_floor = k;
            return;
        }
    }
}

static void wjs_app_m(wjs_m* m, const char* mid)
{
    memset(m, 0, sizeof(*m));
    memcpy(m->kind, "application", 12);
    memcpy(m->proto, "UDP/DTLS/SCTP", 14);
    memcpy(m->fmt, "webrtc-datachannel", 19);
    snprintf(m->mid, sizeof(m->mid), "%s", mid);
    m->app = 1;
}

static void wjs_cert_adopt(wjs_pc* p)
{
    wrt_cert_fp_text(p->cert, p->fp_text, sizeof(p->fp_text));
    p->t->cert = p->cert;
    wrt_cert_ref(p->cert);
}

static int wjs_ensure_cert(JSContext* ctx, wjs_pc* p)
{
    const char* err = NULL;
    if (p->cert)
        return 0;
    if (p->cert_err) {
        err = p->cert_err;
        p->cert_err = NULL;
        js_throw_dom_exception(ctx, "OperationError", "certificate generation failed: %s", err);
        return -1;
    }
    p->cert = wrt_cert_generate(0, 0, (double)time(NULL) * 1000.0 + 30.0 * 86400000.0, &err);
    if (!p->cert) {
        js_throw_dom_exception(ctx, "OperationError", "certificate generation failed: %s", err ? err : "unknown");
        return -1;
    }
    wjs_cert_adopt(p);
    return 0;
}

static int wjs_offer_lines(JSContext* ctx, wjs_pc* p, wjs_ml* ms)
{
    int i, base, k;
    base = ms->n;
    for (i = 0; i < ms->n; i++) {
        wjs_m* m = &ms->v[i];
        wjs_tr* t;
        if (!m->media)
            continue;
        t = wjs_tr_by_mid(p, m->mid);
        if (!t && m->trid)
            t = wjs_tr_by_id(p, m->trid);
        m->trid = t ? t->id : 0;
        m->rejected = !t || t->stopping || t->stopped;
    }
    for (k = 0; k < p->ntr; k++) {
        wjs_tr* t = wjs_tr_get(p->trs[k]);
        wjs_m* m;
        int j, seen = 0;
        if (!t || t->has_mid || t->stopping || t->stopped || t->removed)
            continue;
        for (j = 0; j < ms->n; j++)
            if (ms->v[j].trid == t->id)
                seen = 1;
        if (seen)
            continue;
        for (j = 0, m = NULL; j < base && !m; j++) {
            wjs_m* r = &ms->v[j];
            wjs_tr* o = r->media && r->rejected && !r->app ? wjs_tr_by_id(p, r->trid) : NULL;
            if (r->media && r->rejected && !r->app && (!o || o->stopped) && !strcmp(r->kind, t->kind ? "video" : "audio"))
                m = r;
        }
        if (m) {
            wjs_next_mid(p, ms, m->mid);
            m->rejected = 0;
            m->bundle_only = 0;
            m->trid = t->id;
            continue;
        }
        if (!(m = wjs_ml_push(ctx, ms)))
            return -1;
        snprintf(m->kind, sizeof(m->kind), "%s", t->kind ? "video" : "audio");
        memcpy(m->proto, "UDP/TLS/RTP/SAVPF", 18);
        wjs_next_mid(p, ms, m->mid);
        m->media = 1;
        m->trid = t->id;
    }
    if (wjs_have_dcs(p) && !wjs_has_app(ms)) {
        char mid[RTC_SDP_MID_MAX];
        wjs_m* m;
        wjs_next_mid(p, ms, mid);
        if (!(m = wjs_ml_push(ctx, ms)))
            return -1;
        wjs_app_m(m, mid);
    }
    for (i = base; i < ms->n; i++) {
        int j;
        for (j = 0; j < i; j++) {
            const wjs_m* a = &ms->v[j];
            if (a->rejected || a->bundle_only)
                continue;
            if (p->bundle_policy == 2 || (p->bundle_policy == 0 && !strcmp(a->kind, ms->v[i].kind)))
                ms->v[i].bundle_only = 1;
        }
    }
    return 0;
}

static JSValue wjs_op_offer(JSContext* ctx, wjs_pc* p)
{
    wjs_ml ms = { 0 };
    const wjs_ml* base = &p->m;
    rtc_jsep_offer_m* om = NULL;
    rtc_jsep_params prm;
    uint8_t fp[64];
    rtc_sdp* s = NULL;
    char* sdp = NULL;
    const char *uf = p->ufrag, *pw = p->pwd, *err = NULL;
    JSValue r = JS_EXCEPTION;
    if (p->signaling != WJS_SIG_STABLE && p->signaling != WJS_SIG_HAVE_LOCAL_OFFER)
        return js_throw_dom_exception(ctx, "InvalidStateError", "createOffer: signalingState is '%s'",
            wjs_sig_names[p->signaling]);
    if (wjs_ensure_cert(ctx, p) < 0)
        return JS_EXCEPTION;
    if (p->signaling == WJS_SIG_HAVE_LOCAL_OFFER && p->pend.n)
        base = &p->pend;
    if (wjs_ml_copy(ctx, &ms, base) < 0 || wjs_offer_lines(ctx, p, &ms) < 0)
        goto oom;
    if (p->restart_wanted || !p->ufrag[0]) {
        if (!p->pend_ufrag[0])
            wjs_new_creds(p, p->pend_ufrag, p->pend_pwd);
        uf = p->pend_ufrag;
        pw = p->pend_pwd;
    }
    om = js_mallocz(ctx, sizeof(*om) * (size_t)(ms.n ? ms.n : 1));
    if (!om || wjs_media_offer(ctx, p, &ms, om) < 0)
        goto oom;
    p->sess_ver++;
    wjs_jsep_params(p, &prm, RTC_SETUP_ACTPASS, uf, pw, fp);
    if (!(s = wjs_sdp_new(ctx, p)))
        goto oom;
    if (rtc_jsep_offer(om, ms.n, rtc_jsep_default_caps(), &prm, s, &err) < 0) {
        js_throw_dom_exception(ctx, "OperationError", "createOffer: %s", err ? err : "failed");
        goto done;
    }
    if (!(sdp = wjs_sdp_text(ctx, s)))
        goto done;
    js_free(ctx, p->last_offer);
    p->last_offer = sdp;
    p->offer_restart = p->restart_wanted && p->ufrag[0];
    js_free(ctx, p->offer.v);
    p->offer = ms;
    ms.v = NULL;
    r = wjs_make_init(ctx, WJS_SDP_OFFER, sdp);
    goto done;
oom:
    JS_ThrowOutOfMemory(ctx);
done:
    if (om)
        wjs_media_offer_free(ctx, om, ms.n);
    js_free(ctx, ms.v);
    wjs_sdp_del(JS_GetRuntime(ctx), &s);
    return r;
}

static int wjs_answer_pass(JSContext* ctx, wjs_pc* p, rtc_jsep_local* lo, const char* uf, const char* pw, rtc_sdp* s)
{
    rtc_jsep_params prm;
    uint8_t fp[64];
    const char* err = NULL;
    wjs_jsep_params(p, &prm, p->local_setup, uf, pw, fp);
    if (rtc_jsep_answer(p->roffer, rtc_jsep_default_caps(), lo, &prm, s, &err) < 0) {
        js_throw_dom_exception(ctx, "OperationError", "createAnswer: %s", err ? err : "failed");
        return -1;
    }
    return 0;
}

static int wjs_in_bundle(const rtc_sdp* s, const char* mid)
{
    int i;
    for (i = 0; i < s->nbundle; i++)
        if (!strcmp(s->bundle[i], mid))
            return 1;
    return 0;
}

static JSValue wjs_op_answer(JSContext* ctx, wjs_pc* p)
{
    char* sdp;
    const char *uf = p->ufrag, *pw = p->pwd;
    rtc_jsep_local* lo = NULL;
    rtc_sdp* s = NULL;
    int n, i, tag = -1, again = 0;
    JSValue r = JS_EXCEPTION;
    if (p->signaling != WJS_SIG_HAVE_REMOTE_OFFER && p->signaling != WJS_SIG_HAVE_LOCAL_PRANSWER)
        return js_throw_dom_exception(ctx, "InvalidStateError", "createAnswer: signalingState is '%s'",
            wjs_sig_names[p->signaling]);
    if (!p->roffer)
        return js_throw_dom_exception(ctx, "InvalidStateError", "createAnswer: no remote offer");
    if (wjs_ensure_cert(ctx, p) < 0)
        return JS_EXCEPTION;
    if (p->rrestart || !p->ufrag[0]) {
        if (!p->pend_ufrag[0])
            wjs_new_creds(p, p->pend_ufrag, p->pend_pwd);
        uf = p->pend_ufrag;
        pw = p->pend_pwd;
    }
    n = p->roffer->nmedia;
    lo = js_mallocz(ctx, sizeof(*lo) * (size_t)(n ? n : 1));
    if (!lo || wjs_media_answer_local(ctx, p, p->roffer, lo) < 0 || !(s = wjs_sdp_new(ctx, p))) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    p->sess_ver++;
    if (wjs_answer_pass(ctx, p, lo, uf, pw, s) < 0)
        goto done;
    for (i = 0; i < n; i++) {
        if (!s->media[i].port)
            continue;
        if (tag < 0 && (!s->has_bundle || wjs_in_bundle(s, s->media[i].mid))) {
            tag = i;
            continue;
        }
        if (tag >= 0 && s->has_bundle && wjs_in_bundle(s, s->media[i].mid))
            continue;
        lo[i].stopped = 1;
        again = 1;
    }
    if (again && wjs_answer_pass(ctx, p, lo, uf, pw, s) < 0)
        goto done;
    if (!(sdp = wjs_sdp_text(ctx, s)))
        goto done;
    js_free(ctx, p->last_answer);
    p->last_answer = sdp;
    wjs_sdp_del(JS_GetRuntime(ctx), &p->lanswer);
    p->lanswer = s;
    s = NULL;
    r = wjs_make_init(ctx, WJS_SDP_ANSWER, sdp);
done:
    if (lo)
        wjs_media_answer_local_free(ctx, lo, n);
    wjs_sdp_del(JS_GetRuntime(ctx), &s);
    return r;
}

static int wjs_desc_args(JSContext* ctx, JSValueConst d, int* type, char** sdp)
{
    JSValue v;
    *type = -1;
    *sdp = NULL;
    if (JS_IsUndefined(d) || JS_IsNull(d))
        return 0;
    if (!JS_IsObject(d)) {
        JS_ThrowTypeError(ctx, "description must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, d, "type");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        *type = wjs_sdp_type_parse(s);
        JS_FreeCString(ctx, s);
        if (*type < 0) {
            JS_ThrowTypeError(ctx, "description.type is not a valid RTCSdpType");
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, d, "sdp");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        *sdp = js_malloc(ctx, n + 1);
        if (*sdp) {
            memcpy(*sdp, s, n);
            (*sdp)[n] = 0;
        }
        JS_FreeCString(ctx, s);
        if (!*sdp)
            return -1;
    }
    return 0;
}

static void wjs_desc_set(JSContext* ctx, wjs_desc* d, int type, const char* sdp)
{
    js_free(ctx, d->sdp);
    d->sdp = NULL;
    if (sdp) {
        size_t n = strlen(sdp);
        d->sdp = js_malloc(ctx, n + 1);
        if (d->sdp)
            memcpy(d->sdp, sdp, n + 1);
    }
    d->type = type;
}

static void wjs_desc_move(JSContext* ctx, wjs_desc* to, wjs_desc* from)
{
    js_free(ctx, to->sdp);
    *to = *from;
    from->sdp = NULL;
}

static void wjs_ensure_transport(JSContext* ctx, wjs_pc* p)
{
    if (!p->reactor) {
        struct dyn_aio* aio = dyn_net_reactor_acquire(ctx);
        if (aio) {
            p->reactor = 1;
            p->t->aio = aio;
        }
    }
    wjs_pc_pin(ctx, p);
}

static void wjs_take_pend_creds(wjs_pc* p)
{
    memcpy(p->ufrag, p->pend_ufrag, sizeof(p->ufrag));
    memcpy(p->pwd, p->pend_pwd, sizeof(p->pwd));
    p->pend_ufrag[0] = 0;
    p->pend_pwd[0] = 0;
    p->restart_wanted = 0;
}

static void wjs_apply_local_creds(wjs_pc* p)
{
    if (!p->pend_ufrag[0])
        return;
    rtc_ice_set_local_creds(&p->t->ice, p->pend_ufrag, p->pend_pwd);
    wjs_take_pend_creds(p);
}

static void wjs_ice_apply_restart(wjs_pc* p)
{
    rtc_ice_restart(&p->t->ice, p->pend_ufrag, p->pend_pwd);
    p->t->ice.local_done = p->t->gather_done;
    wjs_take_pend_creds(p);
}

static void wjs_drop_rstage(JSContext* ctx, wjs_pc* p)
{
    p->rrestart = 0;
    p->nscands = 0;
    p->seoc = 0;
    js_free(ctx, p->scands);
    p->scands = NULL;
}

static void wjs_push_scand(JSContext* ctx, wjs_pc* p, const rtc_sdp_cand* c)
{
    if (!p->scands)
        p->scands = js_malloc(ctx, RTC_ICE_MAX_REMOTE * sizeof(rtc_sdp_cand));
    if (p->scands && p->nscands < RTC_ICE_MAX_REMOTE)
        p->scands[p->nscands++] = *c;
}

static void wjs_rollback_local(JSContext* ctx, wjs_pc* p)
{
    wjs_desc_clear(JS_GetRuntime(ctx), &p->pend_local);
    wjs_media_rollback(ctx, p);
    p->pend.n = 0;
    if (p->lrestart) {
        rtc_ice_stage_local(&p->t->ice, "", "");
        p->lrestart = 0;
    }
    wjs_set_signaling(ctx, p, WJS_SIG_STABLE);
}

static void wjs_rollback_remote(JSContext* ctx, wjs_pc* p)
{
    wjs_desc_clear(JS_GetRuntime(ctx), &p->pend_remote);
    wjs_media_rollback(ctx, p);
    p->pend.n = 0;
    wjs_sdp_del(JS_GetRuntime(ctx), &p->roffer);
    wjs_sdp_del(JS_GetRuntime(ctx), &p->lanswer);
    wjs_drop_rstage(ctx, p);
    p->local_setup = p->snap_setup;
    p->remote_lite = p->snap_lite;
    p->can_trickle = p->snap_trickle;
    wjs_set_signaling(ctx, p, WJS_SIG_STABLE);
}

static void wjs_commit_m(JSContext* ctx, wjs_pc* p, int keep)
{
    int i;
    if (wjs_ml_copy(ctx, &p->m, &p->pend) < 0)
        return;
    if (!keep)
        p->pend.n = 0;
    p->app_index = -1;
    p->tag_index = wjs_first_live(&p->m);
    for (i = 0; i < p->m.n; i++)
        if (p->m.v[i].app && !p->m.v[i].rejected) {
            p->app_index = i;
            break;
        }
    wjs_media_commit_pending(p);
}

static void wjs_start_transport(JSContext* ctx, wjs_pc* p)
{
    int remote_has_app = p->app_index >= 0;
    p->t->has_sctp = remote_has_app;
    if (remote_has_app && !JS_IsObject(p->sctp_obj)) {
        p->sctp_obj = JS_NewObject(ctx);
        if (!JS_IsException(p->sctp_obj)) {
            JS_SetPropertyStr(ctx, p->sctp_obj, "maxChannels", JS_NULL);
            JS_SetPropertyStr(ctx, p->sctp_obj, "transport", wjs_pc_dtls(ctx, p));
            JS_SetPropertyStr(ctx, p->sctp_obj, "onstatechange", JS_NULL);
            wjs_sctp_obj_update(ctx, p);
        } else
            p->sctp_obj = JS_NULL;
    }
    wrt_pc_start(p->t);
    p->started = 1;
}

static int wjs_check_nneeded(wjs_pc* p)
{
    if (p->closed || p->signaling != WJS_SIG_STABLE)
        return 0;
    if (wjs_have_dcs(p) && p->app_index < 0)
        return 1;
    if (p->restart_wanted)
        return 1;
    return wjs_media_nneeded(p);
}

static JSValue wjs_nneeded_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    wjs_pc* p = JS_GetOpaque(argv[0], wjs_pc_cid);
    (void)argc;
    if (!p)
        return JS_UNDEFINED;
    p->nneeded_queued = 0;
    if (p->op_head || p->op_running)
        return JS_UNDEFINED;
    if (wjs_check_nneeded(p) && p->nneeded) {
        p->nneeded = 0;
        wjs_fire_simple(ctx, p, WJS_PE_NEGOTIATION);
    }
    return JS_UNDEFINED;
}

static void wjs_update_nneeded(JSContext* ctx, wjs_pc* p)
{
    if (!wjs_check_nneeded(p)) {
        if (p->signaling == WJS_SIG_STABLE)
            p->nneeded = 0;
        return;
    }
    if (p->nneeded)
        return;
    p->nneeded = 1;
    if (!p->nneeded_queued && JS_EnqueueJob(ctx, wjs_nneeded_job, 1, (JSValueConst*)&p->self) == 0)
        p->nneeded_queued = 1;
}

void wjs_pc_nneeded(JSContext* ctx, wjs_pc* p)
{
    if (p->op_head || p->op_running)
        return;
    wjs_update_nneeded(ctx, p);
}

static void wjs_answer_to_pend(wjs_pc* p, const rtc_sdp* ans)
{
    int i;
    for (i = 0; i < p->pend.n && i < ans->nmedia; i++) {
        const rtc_sdp_media* md = &ans->media[i];
        wjs_m* m = &p->pend.v[i];
        m->rejected = m->rejected || rtc_sdp_media_rejected(md);
        m->ldir = md->has_dir ? md->dir : RTC_DIR_SENDRECV;
    }
}

static JSValue wjs_op_sld(JSContext* ctx, wjs_pc* p, JSValueConst arg)
{
    int type;
    char* sdp = NULL;
    JSValue r = JS_UNDEFINED;
    if (wjs_desc_args(ctx, arg, &type, &sdp) < 0)
        return JS_EXCEPTION;
    if (type == WJS_SDP_ROLLBACK) {
        js_free(ctx, sdp);
        if (p->signaling != WJS_SIG_HAVE_LOCAL_OFFER)
            return js_throw_dom_exception(ctx, "InvalidStateError", "setLocalDescription(rollback): no local offer");
        wjs_rollback_local(ctx, p);
        wjs_update_nneeded(ctx, p);
        wjs_media_fire_tracks(ctx, p);
        return JS_UNDEFINED;
    }
    if (type < 0) {
        type = (p->signaling == WJS_SIG_STABLE || p->signaling == WJS_SIG_HAVE_LOCAL_OFFER
                   || p->signaling == WJS_SIG_HAVE_REMOTE_PRANSWER)
            ? WJS_SDP_OFFER
            : WJS_SDP_ANSWER;
    }
    if (!sdp || !*sdp) {
        JSValue g;
        js_free(ctx, sdp);
        sdp = NULL;
        if (type == WJS_SDP_OFFER) {
            if (!p->last_offer || p->signaling != WJS_SIG_HAVE_LOCAL_OFFER) {
                g = wjs_op_offer(ctx, p);
                if (JS_IsException(g))
                    return g;
                JS_FreeValue(ctx, g);
            }
        } else {
            g = wjs_op_answer(ctx, p);
            if (JS_IsException(g))
                return g;
            JS_FreeValue(ctx, g);
        }
    } else {
        const char* last = type == WJS_SDP_OFFER ? p->last_offer : p->last_answer;
        int same = last && !strcmp(last, sdp);
        js_free(ctx, sdp);
        if (!same)
            return js_throw_dom_exception(ctx, "InvalidModificationError",
                "setLocalDescription: the SDP does not match the last createOffer/createAnswer result");
    }
    if (type == WJS_SDP_OFFER) {
        int regather = 0;
        if (p->signaling != WJS_SIG_STABLE && p->signaling != WJS_SIG_HAVE_LOCAL_OFFER)
            return js_throw_dom_exception(ctx, "InvalidStateError", "setLocalDescription(offer): signalingState is '%s'",
                wjs_sig_names[p->signaling]);
        if (wjs_ml_copy(ctx, &p->pend, &p->offer) < 0)
            return JS_ThrowOutOfMemory(ctx);
        wjs_desc_set(ctx, &p->pend_local, WJS_SDP_OFFER, p->last_offer);
        wjs_media_sld_offer(ctx, p, &p->pend);
        p->is_offerer = 1;
        if (!p->t->ice.started)
            rtc_ice_set_role(&p->t->ice, 1);
        wjs_ensure_transport(ctx, p);
        if (p->offer_restart && p->t->ice.started) {
            if (!p->lrestart) {
                p->lrestart = 1;
                rtc_ice_stage_local(&p->t->ice, p->pend_ufrag, p->pend_pwd);
                regather = 1;
            }
        } else if (p->offer_restart || !p->ufrag[0])
            wjs_apply_local_creds(p);
        if (wjs_first_live(&p->pend) >= 0) {
            if (regather)
                wrt_pc_regather(p->t);
            else if (p->t->aio)
                wrt_pc_gather(p->t);
        }
        wjs_set_signaling(ctx, p, WJS_SIG_HAVE_LOCAL_OFFER);
    } else {
        int is_server, regather = 0;
        if (p->signaling != WJS_SIG_HAVE_REMOTE_OFFER && p->signaling != WJS_SIG_HAVE_LOCAL_PRANSWER)
            return js_throw_dom_exception(ctx, "InvalidStateError", "setLocalDescription(answer): signalingState is '%s'",
                wjs_sig_names[p->signaling]);
        wjs_ensure_transport(ctx, p);
        if (p->rrestart) {
            int k;
            wjs_ice_apply_restart(p);
            snprintf(p->rufrag, sizeof(p->rufrag), "%s", p->srufrag);
            snprintf(p->rpwd, sizeof(p->rpwd), "%s", p->srpwd);
            rtc_ice_set_remote_creds(&p->t->ice, p->rufrag, p->rpwd);
            for (k = 0; k < p->nscands; k++)
                wrt_pc_add_remote_cand(p->t, &p->scands[k]);
            if (p->seoc)
                wrt_pc_remote_eoc(p->t);
            wjs_drop_rstage(ctx, p);
            regather = 1;
        } else if (!p->ufrag[0])
            wjs_apply_local_creds(p);
        if (p->lanswer)
            wjs_answer_to_pend(p, p->lanswer);
        if (p->lanswer)
            wjs_media_apply_answer(ctx, p, &p->pend, p->lanswer, 0);
        if (type == WJS_SDP_PRANSWER)
            wjs_desc_set(ctx, &p->pend_local, WJS_SDP_PRANSWER, p->last_answer);
        else {
            wjs_desc_set(ctx, &p->cur_local, WJS_SDP_ANSWER, p->last_answer);
            wjs_desc_clear(JS_GetRuntime(ctx), &p->pend_local);
            if (p->pend_remote.sdp)
                wjs_desc_move(ctx, &p->cur_remote, &p->pend_remote);
            wjs_clear_last(ctx, p);
        }
        p->local_offer = 0;
        wjs_commit_m(ctx, p, type == WJS_SDP_PRANSWER);
        if (type != WJS_SDP_PRANSWER) {
            wjs_sdp_del(JS_GetRuntime(ctx), &p->roffer);
            wjs_sdp_del(JS_GetRuntime(ctx), &p->lanswer);
        }
        is_server = p->local_setup == RTC_SETUP_PASSIVE;
        if (p->tag_index >= 0) {
            wrt_pc_set_dtls_role(p->t, is_server, p->t->dtls.remote_fp, p->t->dtls.remote_fp_len, p->t->dtls.remote_fp_alg);
            if (regather)
                wrt_pc_regather(p->t);
            else if (p->t->aio)
                wrt_pc_gather(p->t);
            wjs_start_transport(ctx, p);
        }
        wjs_set_signaling(ctx, p, type == WJS_SDP_PRANSWER ? WJS_SIG_HAVE_LOCAL_PRANSWER : WJS_SIG_STABLE);
        if (type != WJS_SDP_PRANSWER)
            wjs_media_stable(ctx, p);
        wjs_update_nneeded(ctx, p);
    }
    wjs_media_fire_tracks(ctx, p);
    wjs_pc_rearm_later(ctx, p);
    return r;
}

static int wjs_media_fmt(const rtc_sdp_media* md, char* out, size_t cap)
{
    size_t n = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < md->ncodecs && n + 5 < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s%u", i ? " " : "", (unsigned)md->codecs[i].pt);
    if (!n)
        snprintf(out, cap, "%s", strstr(md->proto, "SCTP") ? "webrtc-datachannel" : "0");
    return 0;
}

static int wjs_m_supported(const rtc_sdp_media* md)
{
    if (!strcmp(md->kind, "application"))
        return strstr(md->proto, "DTLS/SCTP") != NULL;
    return (!strcmp(md->kind, "audio") || !strcmp(md->kind, "video")) && strstr(md->proto, "RTP/") != NULL;
}

static int wjs_mid_index(const rtc_sdp* s, const char* mid)
{
    int i;
    for (i = 0; i < s->nmedia; i++)
        if (s->media[i].mid[0] && !strcmp(s->media[i].mid, mid))
            return i;
    return -1;
}

static int wjs_srd_tag(const rtc_sdp* s, const wjs_ml* ms)
{
    int i;
    if (s->has_bundle && s->nbundle) {
        int k = wjs_mid_index(s, s->bundle[0]);
        if (k >= 0 && k < ms->n && !ms->v[k].rejected && s->media[k].port)
            return k;
    }
    for (i = 0; i < ms->n; i++)
        if (!ms->v[i].rejected && s->media[i].port)
            return i;
    return -1;
}

static JSValue wjs_op_srd(JSContext* ctx, wjs_pc* p, JSValueConst arg)
{
    int type, i, tag = -1, app = -1, setup = 0, fp_alg = 0, want = 0, sig, implicit, cred_changed = 0;
    char* sdp = NULL;
    rtc_sdp* s;
    const char* err = NULL;
    const char *uf = NULL, *pw = NULL;
    const uint8_t* fp = NULL;
    size_t fpl = 0;
    wjs_ml ms = { 0 };
    int nm;
    JSValue ex = JS_UNDEFINED;
    if (wjs_desc_args(ctx, arg, &type, &sdp) < 0)
        return JS_EXCEPTION;
    if (type < 0) {
        js_free(ctx, sdp);
        return JS_ThrowTypeError(ctx, "setRemoteDescription: description.type is required");
    }
    if (type == WJS_SDP_ROLLBACK) {
        js_free(ctx, sdp);
        if (p->signaling != WJS_SIG_HAVE_REMOTE_OFFER)
            return js_throw_dom_exception(ctx, "InvalidStateError", "setRemoteDescription(rollback): no remote offer");
        wjs_rollback_remote(ctx, p);
        wjs_update_nneeded(ctx, p);
        wjs_media_fire_tracks(ctx, p);
        return JS_UNDEFINED;
    }
    implicit = type == WJS_SDP_OFFER && p->signaling == WJS_SIG_HAVE_LOCAL_OFFER;
    sig = implicit ? WJS_SIG_STABLE : p->signaling;
    if (type == WJS_SDP_OFFER && sig != WJS_SIG_STABLE && sig != WJS_SIG_HAVE_REMOTE_OFFER) {
        js_free(ctx, sdp);
        return js_throw_dom_exception(ctx, "InvalidStateError", "setRemoteDescription(offer): signalingState is '%s'",
            wjs_sig_names[p->signaling]);
    }
    if (type != WJS_SDP_OFFER && sig != WJS_SIG_HAVE_LOCAL_OFFER && sig != WJS_SIG_HAVE_REMOTE_PRANSWER) {
        js_free(ctx, sdp);
        return js_throw_dom_exception(ctx, "InvalidStateError", "setRemoteDescription(%s): signalingState is '%s'",
            wjs_sdp_type_name(type), wjs_sig_names[p->signaling]);
    }
    if (!sdp)
        return JS_ThrowTypeError(ctx, "setRemoteDescription: description.sdp is required");
    s = wjs_sdp_new(ctx, p);
    if (!s) {
        js_free(ctx, sdp);
        return JS_EXCEPTION;
    }
    if (rtc_sdp_parse(s, sdp, strlen(sdp), &err) < 0) {
        ex = wjs_rtc_error(ctx, "sdp-syntax-error", err ? err : "invalid SDP");
        goto reject;
    }
    nm = s->nmedia;
    if (type != WJS_SDP_OFFER && nm != p->pend.n) {
        js_throw_dom_exception(ctx, "InvalidAccessError", "setRemoteDescription: answer has %d m-sections, offer has %d", nm, p->pend.n);
        ex = JS_GetException(ctx);
        goto reject;
    }
    if (wjs_ml_reserve(ctx, &ms, nm) < 0) {
        ex = JS_ThrowOutOfMemory(ctx);
        ex = JS_GetException(ctx);
        goto reject;
    }
    for (i = 0; i < nm; i++) {
        rtc_sdp_media* md = &s->media[i];
        wjs_m* m = wjs_ml_push(ctx, &ms);
        snprintf(m->kind, sizeof(m->kind), "%s", md->kind);
        snprintf(m->proto, sizeof(m->proto), "%s", md->proto);
        snprintf(m->mid, sizeof(m->mid), "%s", md->mid[0] ? md->mid : "");
        if (!m->mid[0])
            snprintf(m->mid, sizeof(m->mid), "%d", i);
        wjs_media_fmt(md, m->fmt, sizeof(m->fmt));
        m->app = !strcmp(md->kind, "application") && strstr(md->proto, "DTLS/SCTP") != NULL;
        m->media = !m->app && wjs_m_supported(md);
        m->rejected = rtc_sdp_media_rejected(md) || (!m->app && !m->media);
        m->has_rdir = 1;
        m->rdir = md->has_dir ? md->dir : RTC_DIR_SENDRECV;
        if (type != WJS_SDP_OFFER && i < p->pend.n) {
            if (strcmp(p->pend.v[i].mid, m->mid)) {
                js_throw_dom_exception(ctx, "InvalidAccessError", "setRemoteDescription: answer mid '%s' does not match the offer", m->mid);
                ex = JS_GetException(ctx);
                goto reject;
            }
            m->trid = p->pend.v[i].trid;
            m->ldir = p->pend.v[i].ldir;
            m->msid_hash = p->pend.v[i].msid_hash;
            m->bundle_only = p->pend.v[i].bundle_only;
            if (p->pend.v[i].rejected)
                m->rejected = 1;
        }
        if (m->app && !m->rejected && app < 0)
            app = i;
    }
    tag = wjs_srd_tag(s, &ms);
    if (tag >= 0) {
        rtc_sdp_media* md = &s->media[tag];
        int rs;
        uf = md->ufrag[0] ? md->ufrag : s->ufrag;
        pw = md->pwd[0] ? md->pwd : s->pwd;
        fp = md->fp_len ? md->fp : s->fp;
        fpl = md->fp_len ? md->fp_len : s->fp_len;
        fp_alg = md->fp_len ? md->fp_alg : s->fp_alg;
        setup = md->setup ? md->setup : s->setup;
        if (!uf[0] || !pw[0]) {
            ex = js_dom_exception(ctx, "InvalidAccessError", "setRemoteDescription: missing ice-ufrag or ice-pwd");
            goto reject;
        }
        if (!fpl) {
            ex = js_dom_exception(ctx, "InvalidAccessError", "setRemoteDescription: missing DTLS fingerprint");
            goto reject;
        }
        rs = setup;
        if (rs == RTC_SETUP_NONE)
            rs = type == WJS_SDP_OFFER ? RTC_SETUP_ACTIVE : RTC_SETUP_PASSIVE;
        if (type != WJS_SDP_OFFER && rs == RTC_SETUP_ACTPASS) {
            ex = js_dom_exception(ctx, "InvalidAccessError", "setRemoteDescription: answer must not use setup:actpass");
            goto reject;
        }
        if (type == WJS_SDP_OFFER)
            want = rs == RTC_SETUP_ACTIVE ? RTC_SETUP_PASSIVE : RTC_SETUP_ACTIVE;
        else
            want = rs == RTC_SETUP_PASSIVE ? RTC_SETUP_ACTIVE : RTC_SETUP_PASSIVE;
        if (p->t && p->t->dtls.ssl && fpl == p->t->dtls.remote_fp_len && !memcmp(fp, p->t->dtls.remote_fp, fpl)) {
            int cur = p->t->dtls.is_server ? RTC_SETUP_PASSIVE : RTC_SETUP_ACTIVE;
            if (rs == RTC_SETUP_ACTPASS)
                want = cur;
            if (want != cur) {
                ex = js_dom_exception(ctx, "InvalidAccessError",
                    "setRemoteDescription: the DTLS roles would change, which needs a new DTLS association (RFC 8842 3.1)");
                goto reject;
            }
        }
        if (p->t && p->t->dtls.ssl && p->t->dtls.state != WRT_DTLS_NEW
            && (fpl != p->t->dtls.remote_fp_len || memcmp(fp, p->t->dtls.remote_fp, fpl))) {
            ex = js_dom_exception(ctx, "InvalidModificationError", "setRemoteDescription: the DTLS fingerprint changed on an established transport");
            goto reject;
        }
        cred_changed = p->rufrag[0] && (strcmp(p->rufrag, uf) || strcmp(p->rpwd, pw));
        if (type != WJS_SDP_OFFER && cred_changed && !p->lrestart) {
            ex = js_dom_exception(ctx, "InvalidAccessError",
                "setRemoteDescription: the answer changes the ICE credentials but the offer did not restart ICE (JSEP 5.10)");
            goto reject;
        }
    }
    if (implicit)
        wjs_rollback_local(ctx, p);
    if (type == WJS_SDP_OFFER && p->signaling == WJS_SIG_STABLE) {
        p->snap_setup = p->local_setup;
        p->snap_lite = p->remote_lite;
        p->snap_trickle = p->can_trickle;
    }
    if (type == WJS_SDP_OFFER)
        wjs_drop_rstage(ctx, p);
    if (tag >= 0) {
        int staged;
        p->local_setup = want;
        if (type == WJS_SDP_OFFER) {
            wjs_ensure_transport(ctx, p);
            if (!p->t->ice.started || s->ice_lite != p->remote_lite)
                rtc_ice_set_role(&p->t->ice, s->ice_lite ? 1 : 0);
            p->is_offerer = 0;
        } else if (s->ice_lite)
            rtc_ice_set_role(&p->t->ice, 1);
        staged = type == WJS_SDP_OFFER && cred_changed && p->t->ice.started;
        if (staged) {
            p->rrestart = 1;
            snprintf(p->srufrag, sizeof(p->srufrag), "%s", uf);
            snprintf(p->srpwd, sizeof(p->srpwd), "%s", pw);
        } else {
            snprintf(p->rufrag, sizeof(p->rufrag), "%s", uf);
            snprintf(p->rpwd, sizeof(p->rpwd), "%s", pw);
        }
        p->remote_lite = s->ice_lite;
        if (fpl > sizeof(p->t->dtls.remote_fp))
            fpl = sizeof(p->t->dtls.remote_fp);
        memcpy(p->t->dtls.remote_fp, fp, fpl);
        p->t->dtls.remote_fp_len = (uint8_t)fpl;
        p->t->dtls.remote_fp_alg = (uint8_t)fp_alg;
    }
    if (app >= 0) {
        rtc_sdp_media* md = &s->media[app];
        p->t->remote_sctp_port = md->has_sctp_port ? md->sctp_port : 5000;
        p->t->remote_max_msg = md->has_max_msg ? md->max_msg : 65536;
    }
    if (type == WJS_SDP_OFFER) {
        int k, apps = 0;
        for (k = 0; k < nm; k++)
            if (ms.v[k].app && !ms.v[k].rejected && apps++)
                ms.v[k].rejected = 1;
        if (p->signaling == WJS_SIG_STABLE)
            wjs_media_snapshot(ctx, p);
        if (wjs_media_srd(ctx, p, s, 1, &ms) < 0) {
            ex = JS_GetException(ctx);
            goto reject_late;
        }
        if (wjs_ml_copy(ctx, &p->pend, &ms) < 0) {
            ex = JS_ThrowOutOfMemory(ctx);
            ex = JS_GetException(ctx);
            goto reject_late;
        }
        wjs_desc_set(ctx, &p->pend_remote, WJS_SDP_OFFER, sdp);
        if (tag >= 0 && !p->rrestart)
            rtc_ice_set_remote_creds(&p->t->ice, p->rufrag, p->rpwd);
        wjs_sdp_del(JS_GetRuntime(ctx), &p->roffer);
        wjs_sdp_del(JS_GetRuntime(ctx), &p->lanswer);
        p->roffer = s;
        p->local_offer = 0;
    } else {
        int is_server = p->local_setup == RTC_SETUP_PASSIVE;
        if (wjs_media_srd(ctx, p, s, 0, &ms) < 0) {
            ex = JS_GetException(ctx);
            goto reject_late;
        }
        wjs_media_apply_answer(ctx, p, &ms, s, 1);
        if (wjs_ml_copy(ctx, &p->pend, &ms) < 0) {
            ex = JS_ThrowOutOfMemory(ctx);
            ex = JS_GetException(ctx);
            goto reject_late;
        }
        if (type == WJS_SDP_PRANSWER)
            wjs_desc_set(ctx, &p->pend_remote, type, sdp);
        else {
            wjs_desc_set(ctx, &p->cur_remote, type, sdp);
            wjs_desc_move(ctx, &p->cur_local, &p->pend_local);
            wjs_desc_clear(JS_GetRuntime(ctx), &p->pend_remote);
            wjs_clear_last(ctx, p);
        }
        p->local_offer = 1;
        wjs_commit_m(ctx, p, type == WJS_SDP_PRANSWER);
        if (p->tag_index >= 0) {
            if (p->lrestart) {
                wjs_ice_apply_restart(p);
                p->lrestart = 0;
            }
            rtc_ice_set_remote_creds(&p->t->ice, p->rufrag, p->rpwd);
            wrt_pc_set_dtls_role(p->t, is_server, p->t->dtls.remote_fp, p->t->dtls.remote_fp_len, p->t->dtls.remote_fp_alg);
        }
    }
    if (tag >= 0) {
        rtc_sdp_media* md = &s->media[tag];
        int k;
        for (k = 0; k < md->ncands; k++) {
            if (p->rrestart)
                wjs_push_scand(ctx, p, &md->cands[k]);
            else
                wrt_pc_add_remote_cand(p->t, &md->cands[k]);
        }
        if (md->eoc) {
            if (p->rrestart)
                p->seoc = 1;
            else
                wrt_pc_remote_eoc(p->t);
        }
    }
    p->can_trickle = (tag >= 0 && (s->trickle || s->media[tag].trickle)) ? 1 : 0;
    if (p->roffer != s) {
        rtc_sdp_free(s);
        js_free(ctx, s);
    }
    js_free(ctx, sdp);
    js_free(ctx, ms.v);
    if (type == WJS_SDP_OFFER)
        wjs_set_signaling(ctx, p, WJS_SIG_HAVE_REMOTE_OFFER);
    else {
        if (p->tag_index >= 0)
            wjs_start_transport(ctx, p);
        wjs_set_signaling(ctx, p, type == WJS_SDP_PRANSWER ? WJS_SIG_HAVE_REMOTE_PRANSWER : WJS_SIG_STABLE);
        if (type != WJS_SDP_PRANSWER)
            wjs_media_stable(ctx, p);
        wjs_update_nneeded(ctx, p);
    }
    wjs_media_fire_tracks(ctx, p);
    wjs_pc_rearm_later(ctx, p);
    return JS_UNDEFINED;
reject_late:
    wjs_media_drop_events(ctx, p);
    wjs_media_snap_clear(ctx, p);
reject:
    rtc_sdp_free(s);
    js_free(ctx, s);
    js_free(ctx, sdp);
    js_free(ctx, ms.v);
    return JS_Throw(ctx, ex);
}

static const char* const wjs_cand_keys[] = { "candidate", "sdpMLineIndex", "sdpMid", "usernameFragment" };

static JSValue wjs_cand_init(JSContext* ctx, JSValueConst arg)
{
    JSValue o, v;
    int i, text = 0, nulls = 0;
    if (!JS_IsUndefined(arg) && !JS_IsNull(arg) && !JS_IsObject(arg))
        return JS_ThrowTypeError(ctx, "addIceCandidate: candidate must be an object");
    o = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(o))
        return o;
    for (i = 0; i < 4; i++) {
        v = JS_IsObject(arg) ? JS_GetPropertyStr(ctx, arg, wjs_cand_keys[i]) : JS_UNDEFINED;
        if (JS_IsException(v))
            goto fail;
        if (JS_IsUndefined(v) || (i && JS_IsNull(v))) {
            v = i ? JS_NULL : JS_NewString(ctx, "");
            nulls += i == 1 || i == 2;
        } else if (i == 1) {
            uint32_t u;
            int rc = JS_ToUint32(ctx, &u, v);
            JS_FreeValue(ctx, v);
            if (rc)
                goto fail;
            v = JS_NewInt32(ctx, (int32_t)(u & 0xFFFF));
        } else {
            JSValue s = JS_ToString(ctx, v);
            JS_FreeValue(ctx, v);
            v = s;
            if (!i && JS_IsString(v)) {
                size_t n;
                const char* t = JS_ToCStringLen(ctx, &n, v);
                if (!t) {
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                text = n != 0;
                JS_FreeCString(ctx, t);
            }
        }
        if (JS_IsException(v) || JS_DefinePropertyValueStr(ctx, o, wjs_cand_keys[i], v, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (text && nulls == 2) {
        JS_ThrowTypeError(ctx, "addIceCandidate: sdpMid and sdpMLineIndex are both null");
        goto fail;
    }
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static int wjs_init_str(JSContext* ctx, JSValueConst o, int k, const char** out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, wjs_cand_keys[k]);
    *out = NULL;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsString(v))
        return 0;
    *out = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return *out ? 0 : -1;
}

static JSValue wjs_op_addcand(JSContext* ctx, wjs_pc* p, JSValueConst arg)
{
    const char *text = NULL, *mid = NULL, *uf = NULL;
    int mline, i, target = -1, staged;
    rtc_sdp_cand c;
    JSValue r = JS_UNDEFINED, v;
    if (!p->cur_remote.sdp && !p->pend_remote.sdp)
        return js_throw_dom_exception(ctx, "InvalidStateError", "addIceCandidate: remote description is not set");
    if (wjs_init_str(ctx, arg, 0, &text) || wjs_init_str(ctx, arg, 2, &mid) || wjs_init_str(ctx, arg, 3, &uf))
        goto fail;
    if (!text)
        goto done;
    v = JS_GetPropertyStr(ctx, arg, wjs_cand_keys[1]);
    mline = dyn_int_or(v, -1);
    JS_FreeValue(ctx, v);
    {
        const wjs_ml* l = p->pend.n ? &p->pend : &p->m;
        if (mid) {
            for (i = 0; i < l->n; i++)
                if (!strcmp(l->v[i].mid, mid))
                    target = i;
        } else if (mline >= 0 && mline < l->n)
            target = mline;
        if (text[0] && target < 0) {
            r = js_throw_dom_exception(ctx, "OperationError", "addIceCandidate: unknown sdpMid/sdpMLineIndex");
            goto done;
        }
        if (target >= 0 && l->v[target].rejected)
            goto done;
    }
    staged = p->rrestart;
    if (uf && uf[0]) {
        staged = p->rrestart && !strcmp(uf, p->srufrag);
        if (!staged && strcmp(uf, p->rufrag)) {
            r = js_throw_dom_exception(ctx, "OperationError", "addIceCandidate: usernameFragment does not match");
            goto done;
        }
    }
    if (!text[0]) {
        if (staged)
            p->seoc = 1;
        else if (p->t)
            wrt_pc_remote_eoc(p->t);
        goto done;
    }
    if (rtc_sdp_parse_candidate(text, strlen(text), &c) < 0) {
        r = js_throw_dom_exception(ctx, "OperationError", "addIceCandidate: candidate could not be parsed");
        goto done;
    }
    if (strstr(text, ".local "))
        goto done;
    if (staged)
        wjs_push_scand(ctx, p, &c);
    else
        wrt_pc_add_remote_cand(p->t, &c);
    wjs_pc_rearm(ctx, p);
    goto done;
fail:
    r = JS_EXCEPTION;
done:
    if (text)
        JS_FreeCString(ctx, text);
    if (mid)
        JS_FreeCString(ctx, mid);
    if (uf)
        JS_FreeCString(ctx, uf);
    return r;
}

static void wjs_stats_put(JSContext* ctx, JSValueConst map, JSValueConst set, JSValue o)
{
    JSValue id = JS_GetPropertyStr(ctx, o, "id");
    JSValueConst a[2];
    JSValue r;
    a[0] = id;
    a[1] = o;
    r = JS_Call(ctx, set, map, 2, a);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, o);
}

static JSValue wjs_stat_obj(JSContext* ctx, const char* type, const char* id, double ts)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id", JS_NewString(ctx, id));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, o, "timestamp", JS_NewFloat64(ctx, ts));
    return o;
}

static void wjs_stats_cand(JSContext* ctx, JSValueConst map, JSValueConst set, const rtc_cand* c, int remote, int i, double ts)
{
    static const char* const typ[] = { "host", "srflx", "prflx", "relay" };
    char id[32], ip[64];
    JSValue o;
    snprintf(id, sizeof(id), "%s%d", remote ? "RC" : "LC", i);
    o = wjs_stat_obj(ctx, remote ? "remote-candidate" : "local-candidate", id, ts);
    rtc_addr_fmt(&c->addr, ip, sizeof(ip));
    JS_SetPropertyStr(ctx, o, "transportId", JS_NewString(ctx, "T0"));
    JS_SetPropertyStr(ctx, o, "address", JS_NewString(ctx, ip));
    JS_SetPropertyStr(ctx, o, "port", JS_NewInt32(ctx, c->addr.port));
    JS_SetPropertyStr(ctx, o, "protocol", JS_NewString(ctx, "udp"));
    JS_SetPropertyStr(ctx, o, "candidateType", JS_NewString(ctx, typ[c->type & 3]));
    JS_SetPropertyStr(ctx, o, "priority", JS_NewUint32(ctx, c->prio));
    if (c->type == RTC_CAND_RELAY)
        JS_SetPropertyStr(ctx, o, "relayProtocol", JS_NewString(ctx, "udp"));
    wjs_stats_put(ctx, map, set, o);
}

static JSValue wjs_op_stats(JSContext* ctx, wjs_pc* p)
{
    JSValue g = JS_GetGlobalObject(ctx), mc, map, set, o;
    double ts = (double)time(NULL) * 1000.0;
    char id[48];
    uint32_t i;
    mc = JS_GetPropertyStr(ctx, g, "Map");
    JS_FreeValue(ctx, g);
    if (JS_IsException(mc))
        return mc;
    map = JS_CallConstructor(ctx, mc, 0, NULL);
    JS_FreeValue(ctx, mc);
    if (JS_IsException(map))
        return map;
    set = JS_GetPropertyStr(ctx, map, "set");
    o = wjs_stat_obj(ctx, "peer-connection", "P", ts);
    JS_SetPropertyStr(ctx, o, "dataChannelsOpened", JS_NewInt64(ctx, p->t ? (int64_t)p->t->dc_opened : 0));
    JS_SetPropertyStr(ctx, o, "dataChannelsClosed", JS_NewInt64(ctx, p->t ? (int64_t)p->t->dc_closed : 0));
    wjs_stats_put(ctx, map, set, o);
    if (p->t) {
        wrt_pc* t = p->t;
        const rtc_cand *sl, *sr;
        int have_sel = rtc_ice_get_selected(&t->ice, &sl, &sr) != 0;
        for (i = 0; i < t->ndcs; i++) {
            wrt_dc* dc = t->dcs[i];
            if (!dc)
                continue;
            snprintf(id, sizeof(id), "D%u", i);
            o = wjs_stat_obj(ctx, "data-channel", id, ts);
            JS_SetPropertyStr(ctx, o, "label", JS_NewString(ctx, dc->label));
            JS_SetPropertyStr(ctx, o, "protocol", JS_NewString(ctx, dc->protocol));
            if (dc->has_id)
                JS_SetPropertyStr(ctx, o, "dataChannelIdentifier", JS_NewInt32(ctx, dc->id));
            JS_SetPropertyStr(ctx, o, "state",
                JS_NewString(ctx, dc->state == WRT_DC_OPEN ? "open" : dc->state == WRT_DC_CLOSING ? "closing" : dc->state == WRT_DC_CLOSED ? "closed" : "connecting"));
            JS_SetPropertyStr(ctx, o, "messagesSent", JS_NewInt64(ctx, (int64_t)dc->msgs_tx));
            JS_SetPropertyStr(ctx, o, "bytesSent", JS_NewInt64(ctx, (int64_t)dc->bytes_tx));
            JS_SetPropertyStr(ctx, o, "messagesReceived", JS_NewInt64(ctx, (int64_t)dc->msgs_rx));
            JS_SetPropertyStr(ctx, o, "bytesReceived", JS_NewInt64(ctx, (int64_t)dc->bytes_rx));
            wjs_stats_put(ctx, map, set, o);
        }
        o = wjs_stat_obj(ctx, "transport", "T0", ts);
        JS_SetPropertyStr(ctx, o, "bytesSent", JS_NewInt64(ctx, (int64_t)t->bytes_tx));
        JS_SetPropertyStr(ctx, o, "bytesReceived", JS_NewInt64(ctx, (int64_t)t->bytes_rx));
        JS_SetPropertyStr(ctx, o, "packetsSent", JS_NewInt64(ctx, (int64_t)t->pkts_tx));
        JS_SetPropertyStr(ctx, o, "packetsReceived", JS_NewInt64(ctx, (int64_t)t->pkts_rx));
        JS_SetPropertyStr(ctx, o, "receiveWakeups", JS_NewInt64(ctx, (int64_t)t->rx_drains));
        JS_SetPropertyStr(ctx, o, "iceRole", JS_NewString(ctx, t->ice.controlling ? "controlling" : "controlled"));
        JS_SetPropertyStr(ctx, o, "iceLocalUsernameFragment", JS_NewString(ctx, p->ufrag));
        JS_SetPropertyStr(ctx, o, "iceState", JS_NewString(ctx, wjs_ice_names[p->ice_state % 7]));
        JS_SetPropertyStr(ctx, o, "dtlsState",
            JS_NewString(ctx, t->dtls.state == WRT_DTLS_CONNECTED ? "connected" : t->dtls.state == WRT_DTLS_CONNECTING ? "connecting" : t->dtls.state == WRT_DTLS_FAILED ? "failed" : t->dtls.state == WRT_DTLS_CLOSED ? "closed" : "new"));
        if (t->dtls.ssl)
            JS_SetPropertyStr(ctx, o, "dtlsRole", JS_NewString(ctx, t->dtls.is_server ? "server" : "client"));
        if (t->dtls.state == WRT_DTLS_CONNECTED) {
            const char* c = wrt_dtls_cipher(&t->dtls);
            const char* sp = wrt_dtls_srtp_name(&t->dtls);
            JS_SetPropertyStr(ctx, o, "tlsVersion", JS_NewString(ctx, "FEFD"));
            if (c)
                JS_SetPropertyStr(ctx, o, "dtlsCipher", JS_NewString(ctx, c));
            if (sp)
                JS_SetPropertyStr(ctx, o, "srtpCipher", JS_NewString(ctx, sp));
            JS_SetPropertyStr(ctx, o, "remoteCertificateId", JS_NewString(ctx, "CR"));
        }
        if (p->cert)
            JS_SetPropertyStr(ctx, o, "localCertificateId", JS_NewString(ctx, "CL"));
        if (have_sel) {
            snprintf(id, sizeof(id), "CP%d", t->ice.selected >= 0 ? t->ice.selected : 0);
            JS_SetPropertyStr(ctx, o, "selectedCandidatePairId", JS_NewString(ctx, id));
        }
        wjs_stats_put(ctx, map, set, o);
        if (t->sctp) {
            rtc_sctp* s = t->sctp;
            o = wjs_stat_obj(ctx, "sctp-transport", "S0", ts);
            JS_SetPropertyStr(ctx, o, "transportId", JS_NewString(ctx, "T0"));
            JS_SetPropertyStr(ctx, o, "smoothedRoundTripTime", JS_NewFloat64(ctx, s->srtt_us / 1e6));
            JS_SetPropertyStr(ctx, o, "congestionWindow", JS_NewInt64(ctx, s->cwnd));
            JS_SetPropertyStr(ctx, o, "receiverWindow", JS_NewInt64(ctx, s->peer_rwnd));
            JS_SetPropertyStr(ctx, o, "mtu", JS_NewInt64(ctx, s->mtu));
            JS_SetPropertyStr(ctx, o, "unackData", JS_NewInt64(ctx, s->flight));
            JS_SetPropertyStr(ctx, o, "retransmissionTimeout", JS_NewFloat64(ctx, s->rto_us / 1e6));
            JS_SetPropertyStr(ctx, o, "retransmittedChunks", JS_NewInt64(ctx, (int64_t)s->st.rtx_chunks));
            JS_SetPropertyStr(ctx, o, "t3Expiries", JS_NewInt64(ctx, (int64_t)s->st.t3_expiries));
            JS_SetPropertyStr(ctx, o, "fastRetransmits", JS_NewInt64(ctx, (int64_t)s->st.fast_rtx));
            JS_SetPropertyStr(ctx, o, "forwardTsnSent", JS_NewInt64(ctx, (int64_t)s->st.fwd_tsn_tx));
            JS_SetPropertyStr(ctx, o, "initChunksSent", JS_NewInt64(ctx, (int64_t)s->st.init_tx));
            JS_SetPropertyStr(ctx, o, "zeroChecksumNegotiated", JS_NewBool(ctx, s->peer_zcsum && s->zcsum_local));
            JS_SetPropertyStr(ctx, o, "zeroChecksumPacketsSent", JS_NewInt64(ctx, (int64_t)s->st.zcsum_tx));
            JS_SetPropertyStr(ctx, o, "zeroChecksumPacketsReceived", JS_NewInt64(ctx, (int64_t)s->st.zcsum_rx));
            wjs_stats_put(ctx, map, set, o);
        }
        for (i = 0; i < (uint32_t)t->ice.nlocal; i++)
            wjs_stats_cand(ctx, map, set, &t->ice.local[i], 0, (int)i, ts);
        for (i = 0; i < (uint32_t)t->ice.nremote; i++)
            wjs_stats_cand(ctx, map, set, &t->ice.remote[i], 1, (int)i, ts);
        for (i = 0; i < (uint32_t)t->ice.npairs; i++) {
            static const char* const ps[] = { "frozen", "waiting", "in-progress", "succeeded", "failed" };
            rtc_pair* pr = &t->ice.pairs[i];
            snprintf(id, sizeof(id), "CP%u", i);
            o = wjs_stat_obj(ctx, "candidate-pair", id, ts);
            JS_SetPropertyStr(ctx, o, "transportId", JS_NewString(ctx, "T0"));
            snprintf(id, sizeof(id), "LC%u", pr->l);
            JS_SetPropertyStr(ctx, o, "localCandidateId", JS_NewString(ctx, id));
            snprintf(id, sizeof(id), "RC%u", pr->r);
            JS_SetPropertyStr(ctx, o, "remoteCandidateId", JS_NewString(ctx, id));
            JS_SetPropertyStr(ctx, o, "state", JS_NewString(ctx, ps[pr->state % 5]));
            JS_SetPropertyStr(ctx, o, "nominated", JS_NewBool(ctx, (pr->flags & RTC_PF_NOMINATED) != 0));
            JS_SetPropertyStr(ctx, o, "priority", JS_NewFloat64(ctx, (double)pr->prio));
            JS_SetPropertyStr(ctx, o, "requestsSent", JS_NewInt64(ctx, (int64_t)pr->req_tx));
            JS_SetPropertyStr(ctx, o, "requestsReceived", JS_NewInt64(ctx, (int64_t)pr->req_rx));
            JS_SetPropertyStr(ctx, o, "responsesSent", JS_NewInt64(ctx, (int64_t)pr->rsp_tx));
            JS_SetPropertyStr(ctx, o, "responsesReceived", JS_NewInt64(ctx, (int64_t)pr->rsp_rx));
            if (pr->rtt_us)
                JS_SetPropertyStr(ctx, o, "currentRoundTripTime", JS_NewFloat64(ctx, pr->rtt_us / 1e6));
            JS_SetPropertyStr(ctx, o, "totalRoundTripTime", JS_NewFloat64(ctx, pr->rtt_total_us / 1e6));
            if ((int)i == t->ice.selected) {
                JS_SetPropertyStr(ctx, o, "bytesSent", JS_NewInt64(ctx, (int64_t)t->bytes_tx));
                JS_SetPropertyStr(ctx, o, "bytesReceived", JS_NewInt64(ctx, (int64_t)t->bytes_rx));
                JS_SetPropertyStr(ctx, o, "packetsSent", JS_NewInt64(ctx, (int64_t)t->pkts_tx));
                JS_SetPropertyStr(ctx, o, "packetsReceived", JS_NewInt64(ctx, (int64_t)t->pkts_rx));
            }
            wjs_stats_put(ctx, map, set, o);
        }
        if (p->cert) {
            char fpb[128];
            o = wjs_stat_obj(ctx, "certificate", "CL", ts);
            wrt_cert_fp_text(p->cert, fpb, sizeof(fpb));
            JS_SetPropertyStr(ctx, o, "fingerprint", JS_NewString(ctx, fpb));
            JS_SetPropertyStr(ctx, o, "fingerprintAlgorithm", JS_NewString(ctx, "sha-256"));
            wjs_stats_put(ctx, map, set, o);
        }
        if (t->dtls.state == WRT_DTLS_CONNECTED) {
            uint8_t rfp[32];
            char fpb[128];
            if (wrt_dtls_peer_fp(&t->dtls, rfp)) {
                rtc_hex_upper_colon(rfp, 32, fpb, sizeof(fpb));
                o = wjs_stat_obj(ctx, "certificate", "CR", ts);
                JS_SetPropertyStr(ctx, o, "fingerprint", JS_NewString(ctx, fpb));
                JS_SetPropertyStr(ctx, o, "fingerprintAlgorithm", JS_NewString(ctx, "sha-256"));
                wjs_stats_put(ctx, map, set, o);
            }
        }
    }
    JS_FreeValue(ctx, set);
    return map;
}

static JSValue wjs_ops_job(JSContext* ctx, int argc, JSValueConst* argv);

static void wjs_ops_kick(JSContext* ctx, wjs_pc* p)
{
    if (p->op_job || !p->op_head)
        return;
    if (JS_EnqueueJob(ctx, wjs_ops_job, 1, (JSValueConst*)&p->self) == 0)
        p->op_job = 1;
}

typedef struct {
    JSRuntime* rt;
    JSValue self;
    int orphan;
    double expires_ms;
    wrt_cert* c;
    const char* err;
} wjs_dcert_job;

static void wjs_dcert_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    wjs_dcert_job* j = (wjs_dcert_job*)opaque;
    JSValue v = j->self;
    (void)ctx;
    j->orphan = 1;
    j->self = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
    dyn_net_reactor_release_rt(rt);
}

static void wjs_dcert_work(void* arg)
{
    wjs_dcert_job* j = (wjs_dcert_job*)arg;
    j->c = wrt_cert_generate(0, 0, j->expires_ms, &j->err);
}

static void wjs_dcert_done(void* arg)
{
    wjs_dcert_job* j = (wjs_dcert_job*)arg;
    wjs_pc* p;
    JSContext* ctx;
    if (j->orphan) {
        if (j->c)
            wrt_cert_unref(j->c);
        free(j);
        return;
    }
    JS_RemoveShutdownSweep(j->rt, wjs_dcert_sweep, j);
    p = JS_GetOpaque(j->self, wjs_pc_cid);
    ctx = p->ctx;
    p->cert_wait = 0;
    if (p->closed || p->cert) {
        if (j->c)
            wrt_cert_unref(j->c);
    } else if (!j->c)
        p->cert_err = j->err ? j->err : "unknown";
    else {
        p->cert = j->c;
        wjs_cert_adopt(p);
    }
    wjs_ops_kick(ctx, p);
    JS_FreeValue(ctx, j->self);
    free(j);
    dyn_net_reactor_release(ctx);
}

static int wjs_dcert_offload(JSContext* ctx, wjs_pc* p)
{
    wjs_dcert_job* j;
    struct dyn_aio* aio;
    if (p->cert_wait)
        return 1;
    if (p->cert || p->cert_err)
        return 0;
    j = (wjs_dcert_job*)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->rt = JS_GetRuntime(ctx);
    j->expires_ms = (double)time(NULL) * 1000.0 + 30.0 * 86400000.0;
    aio = dyn_net_reactor_acquire(ctx);
    if (aio && JS_AddShutdownSweep(j->rt, wjs_dcert_sweep, j) == 0) {
        j->self = JS_DupValue(ctx, p->self);
        if (dyn_aio_offload_cpu(aio, wjs_dcert_work, wjs_dcert_done, j) == 0) {
            p->cert_wait = 1;
            return 1;
        }
        JS_RemoveShutdownSweep(j->rt, wjs_dcert_sweep, j);
        JS_FreeValue(ctx, j->self);
    }
    if (aio)
        dyn_net_reactor_release(ctx);
    free(j);
    return 0;
}

static JSValue wjs_ops_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    wjs_pc* p = JS_GetOpaque(argv[0], wjs_pc_cid);
    wjs_op* o;
    JSValue v, r;
    int ok;
    (void)argc;
    if (!p)
        return JS_UNDEFINED;
    p->op_job = 0;
    o = p->op_head;
    if (!o)
        return JS_UNDEFINED;
    if (!p->closed && (o->kind == WJS_OP_OFFER || o->kind == WJS_OP_ANSWER || o->kind == WJS_OP_SLD) && wjs_dcert_offload(ctx, p))
        return JS_UNDEFINED;
    p->op_head = o->next;
    if (!p->op_head)
        p->op_tail = NULL;
    if (p->closed && o->kind != WJS_OP_STATS) {
        JS_FreeValue(ctx, o->arg);
        JS_FreeValue(ctx, o->res[0]);
        JS_FreeValue(ctx, o->res[1]);
        js_free(ctx, o);
        wjs_ops_kick(ctx, p);
        return JS_UNDEFINED;
    }
    p->op_running = 1;
    switch (o->kind) {
    case WJS_OP_OFFER:
        v = wjs_op_offer(ctx, p);
        break;
    case WJS_OP_ANSWER:
        v = wjs_op_answer(ctx, p);
        break;
    case WJS_OP_SLD:
        v = wjs_op_sld(ctx, p, o->arg);
        break;
    case WJS_OP_SRD:
        v = wjs_op_srd(ctx, p, o->arg);
        break;
    case WJS_OP_ADDCAND:
        v = wjs_op_addcand(ctx, p, o->arg);
        break;
    case WJS_OP_REPLACE:
        v = wjs_tr_replace_op(ctx, p, o->arg);
        break;
    default:
        v = wjs_op_stats(ctx, p);
        break;
    }
    p->op_running = 0;
    ok = !JS_IsException(v);
    if (!ok)
        v = JS_GetException(ctx);
    if (!p->closed || o->kind == WJS_OP_STATS) {
        r = JS_Call(ctx, o->res[ok ? 0 : 1], JS_UNDEFINED, 1, (JSValueConst*)&v);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, o->arg);
    JS_FreeValue(ctx, o->res[0]);
    JS_FreeValue(ctx, o->res[1]);
    js_free(ctx, o);
    wjs_ops_kick(ctx, p);
    if (!p->op_head && p->nneeded && !p->nneeded_queued && wjs_check_nneeded(p)
        && JS_EnqueueJob(ctx, wjs_nneeded_job, 1, (JSValueConst*)&p->self) == 0)
        p->nneeded_queued = 1;
    return JS_UNDEFINED;
}

static JSValue wjs_op_push(JSContext* ctx, wjs_pc* p, int kind, JSValueConst arg)
{
    if (kind < 0)
        kind = WJS_OP_REPLACE;
    wjs_op* o;
    JSValue prom;
    if (p->closed && kind != WJS_OP_STATS) {
        JSValue res[2], e, r;
        prom = JS_NewPromiseCapability(ctx, res);
        if (JS_IsException(prom))
            return prom;
        e = js_dom_exception(ctx, "InvalidStateError", "RTCPeerConnection is closed");
        r = JS_Call(ctx, res[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, res[0]);
        JS_FreeValue(ctx, res[1]);
        return prom;
    }
    o = js_mallocz(ctx, sizeof(*o));
    if (!o)
        return JS_EXCEPTION;
    prom = JS_NewPromiseCapability(ctx, o->res);
    if (JS_IsException(prom)) {
        js_free(ctx, o);
        return prom;
    }
    o->kind = kind;
    o->arg = JS_DupValue(ctx, arg);
    if (p->op_tail)
        p->op_tail->next = o;
    else
        p->op_head = o;
    p->op_tail = o;
    wjs_ops_kick(ctx, p);
    return prom;
}

JSValue wjs_pc_op_push(JSContext* ctx, wjs_pc* p, int kind, JSValueConst arg)
{
    return wjs_op_push(ctx, p, kind, arg);
}

wjs_pc* wjs_pc_get_opaque(JSContext* ctx, JSValueConst v)
{
    return wjs_pc_of(ctx, v);
}

static JSValue wjs_pc_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (magic == WJS_OP_OFFER && argc && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "iceRestart");
        if (JS_IsException(v))
            return v;
        if (JS_ToBool(ctx, v))
            p->restart_wanted = 1;
        JS_FreeValue(ctx, v);
    }
    if (magic == WJS_OP_ADDCAND) {
        JSValue init = wjs_cand_init(ctx, argc ? argv[0] : JS_UNDEFINED), r;
        if (JS_IsException(init))
            return wjs_reject_pending(ctx);
        r = wjs_op_push(ctx, p, magic, init);
        JS_FreeValue(ctx, init);
        return r;
    }
    return wjs_op_push(ctx, p, magic, argc ? argv[0] : JS_UNDEFINED);
}

static int wjs_parse_ice_servers(JSContext* ctx, wjs_pc* p, JSValueConst cfg);
static int wjs_parse_policy(JSContext* ctx, wjs_pc* p, JSValueConst cfg);
static int wjs_parse_local_addrs(JSContext* ctx, wjs_pc* p, JSValueConst cfg);

static const char* const wjs_bundle_names[] = { "balanced", "max-compat", "max-bundle" };

static int wjs_parse_bundle(JSContext* ctx, JSValueConst cfg, int* bundle)
{
    JSValue v = JS_GetPropertyStr(ctx, cfg, "bundlePolicy");
    int b = 0;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        for (b = 0; b < 3 && strcmp(s, wjs_bundle_names[b]); b++)
            ;
        JS_FreeCString(ctx, s);
        if (b == 3) {
            JS_ThrowTypeError(ctx, "RTCPeerConnection: invalid bundlePolicy");
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, cfg, "rtcpMuxPolicy");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        int ok;
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        ok = !strcmp(s, "require");
        JS_FreeCString(ctx, s);
        if (!ok) {
            JS_ThrowTypeError(ctx, "RTCPeerConnection: invalid rtcpMuxPolicy");
            return -1;
        }
    }
    *bundle = b;
    return 0;
}

static int wjs_parse_servers(JSContext* ctx, wjs_pc* p, JSValueConst cfg)
{
    JSValue v;
    if (!JS_IsObject(cfg))
        return 0;
    if (wjs_parse_policy(ctx, p, cfg) < 0 || wjs_parse_bundle(ctx, cfg, &p->bundle_policy) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, cfg, "certificates");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        JSValue c0;
        wrt_cert* c;
        if (!JS_IsArray(ctx, v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: certificates must be a sequence");
            return -1;
        }
        c0 = JS_GetPropertyUint32(ctx, v, 0);
        c = wjs_cert_get(ctx, c0);
        JS_FreeValue(ctx, c0);
        if (!JS_IsUndefined(c0) && !c) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: certificates[0] is not an RTCCertificate");
            return -1;
        }
        if (c) {
            if (c->expires_ms <= (double)time(NULL) * 1000.0) {
                JS_FreeValue(ctx, v);
                js_throw_dom_exception(ctx, "InvalidAccessError", "RTCPeerConnection: certificate has expired");
                return -1;
            }
            p->cert = c;
            wrt_cert_ref(c);
            p->t->cert = c;
            wrt_cert_ref(c);
            wrt_cert_fp_text(c, p->fp_text, sizeof(p->fp_text));
        }
        p->certs = v;
    }
    if (wjs_parse_local_addrs(ctx, p, cfg) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, cfg, "sctpZeroChecksum");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v))
        p->t->sctp_zcsum_off = !JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, cfg, "sctpStreams");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        double d = -1;
        if (JS_ToFloat64(ctx, &d, v) < 0 || !(d >= 1 && d <= 65535) || d != (double)(int32_t)d) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: sctpStreams must be an integer in 1..65535");
            return -1;
        }
        p->t->sctp_streams = (uint16_t)d;
    }
    JS_FreeValue(ctx, v);
    return wjs_parse_ice_servers(ctx, p, cfg);
}

static int wjs_parse_policy(JSContext* ctx, wjs_pc* p, JSValueConst cfg)
{
    JSValue v = JS_GetPropertyStr(ctx, cfg, "iceTransportPolicy");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        if (!strcmp(s, "relay"))
            p->t->relay_next = 1;
        else if (!strcmp(s, "all"))
            p->t->relay_next = 0;
        else {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: invalid iceTransportPolicy");
            return -1;
        }
        JS_FreeCString(ctx, s);
    }
    return 0;
}

static int wjs_parse_local_addrs(JSContext* ctx, wjs_pc* p, JSValueConst cfg)
{
    JSValue v = JS_GetPropertyStr(ctx, cfg, "iceLocalAddresses");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        uint32_t k, na = 0;
        rtc_addr list[WRT_MAX_SOCKS];
        int nl = 0;
        if (!JS_IsArray(ctx, v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: iceLocalAddresses must be an array of IP literals");
            return -1;
        }
        if (dyn_js_len_u32(ctx, v, &na)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        for (k = 0; k < na; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, k);
            size_t sl;
            const char* s = JS_ToCStringLen(ctx, &sl, e);
            rtc_addr ad;
            int bad;
            JS_FreeValue(ctx, e);
            if (!s) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            bad = rtc_addr_parse(s, sl, 0, &ad) < 0 || nl >= WRT_MAX_SOCKS;
            if (bad) {
                JS_ThrowTypeError(ctx, "RTCPeerConnection: iceLocalAddresses[%u] \"%s\" is not an IP literal (or more than %d given)", k, s, WRT_MAX_SOCKS);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                return -1;
            }
            JS_FreeCString(ctx, s);
            list[nl++] = ad;
        }
        memcpy(p->t->bind_addrs, list, (size_t)nl * sizeof(rtc_addr));
        p->t->bind_n = nl;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int wjs_parse_ice_servers(JSContext* ctx, wjs_pc* p, JSValueConst cfg)
{
    JSValue servers, v;
    uint32_t i, n = 0;
    servers = JS_GetPropertyStr(ctx, cfg, "iceServers");
    if (JS_IsException(servers))
        return -1;
    if (JS_IsUndefined(servers) || JS_IsNull(servers))
        return 0;
    v = JS_GetPropertyStr(ctx, servers, "length");
    if (JS_IsException(v) || JS_ToUint32(ctx, &n, v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, servers);
        return -1;
    }
    JS_FreeValue(ctx, v);
    for (i = 0; i < n; i++) {
        JSValue sv = JS_GetPropertyUint32(ctx, servers, i), urls, uv, cv;
        const char *user = NULL, *cred = NULL;
        uint32_t k, nu = 1;
        int arr;
        if (JS_IsException(sv) || !JS_IsObject(sv)) {
            JS_FreeValue(ctx, sv);
            JS_FreeValue(ctx, servers);
            JS_ThrowTypeError(ctx, "RTCPeerConnection: iceServers entries must be objects");
            return -1;
        }
        urls = JS_GetPropertyStr(ctx, sv, "urls");
        if (JS_IsUndefined(urls)) {
            JS_FreeValue(ctx, urls);
            urls = JS_GetPropertyStr(ctx, sv, "url");
        }
        uv = JS_GetPropertyStr(ctx, sv, "username");
        cv = JS_GetPropertyStr(ctx, sv, "credential");
        if (!JS_IsUndefined(uv))
            user = JS_ToCString(ctx, uv);
        if (!JS_IsUndefined(cv))
            cred = JS_ToCString(ctx, cv);
        arr = JS_IsArray(ctx, urls);
        if (JS_IsUndefined(urls)) {
            nu = 0;
            JS_ThrowTypeError(ctx, "RTCPeerConnection: iceServers[%u].urls is required", i);
        } else if (arr) {
            if (dyn_js_len_u32(ctx, urls, &nu))
                nu = 0;
            else if (!nu)
                js_throw_dom_exception(ctx, "SyntaxError", "RTCPeerConnection: iceServers[%u].urls is empty", i);
        }
        for (k = 0; k < nu; k++) {
            JSValue u = arr ? JS_GetPropertyUint32(ctx, urls, k) : JS_DupValue(ctx, urls);
            const char* us = JS_ToCString(ctx, u);
            const char* err = NULL;
            JS_FreeValue(ctx, u);
            if (!us)
                break;
            if (wrt_pc_add_server(p->t, us, user, cred, &err) < 0) {
                if (!strcmp(err, "SyntaxError"))
                    js_throw_dom_exception(ctx, "SyntaxError", "RTCPeerConnection: invalid ICE server URL '%s'", us);
                else if (!strcmp(err, "InvalidAccessError"))
                    js_throw_dom_exception(ctx, "InvalidAccessError", "RTCPeerConnection: TURN server '%s' needs username and credential", us);
                else
                    js_throw_dom_exception(ctx, "NotSupportedError", "RTCPeerConnection: ICE server URL '%s' is not supported", us);
                JS_FreeCString(ctx, us);
                nu = 0;
                break;
            }
            JS_FreeCString(ctx, us);
        }
        if (user)
            JS_FreeCString(ctx, user);
        if (cred)
            JS_FreeCString(ctx, cred);
        JS_FreeValue(ctx, uv);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, urls);
        JS_FreeValue(ctx, sv);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, servers);
            return -1;
        }
    }
    JS_FreeValue(ctx, servers);
    return 0;
}

static JSValue wjs_pc_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    wjs_pc* p;
    JSValue obj;
    uint8_t seed[32];
    int i;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "RTCPeerConnection: constructor requires 'new'");
    if (argc && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) && !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "RTCPeerConnection: configuration must be an object");
    p = js_mallocz(ctx, sizeof(*p));
    if (!p)
        return JS_EXCEPTION;
    p->ctx = ctx;
    p->pin = JS_UNDEFINED;
    p->timer = JS_UNDEFINED;
    p->tick_fn = JS_UNDEFINED;
    p->config = JS_UNDEFINED;
    p->sctp_obj = JS_NULL;
    p->certs = JS_UNDEFINED;
    p->dtls_obj = JS_NULL;
    p->ice_obj = JS_NULL;
    p->app_index = -1;
    p->tag_index = -1;
    p->can_trickle = -1;
    p->rstreams = JS_UNDEFINED;
    p->track_evs = JS_UNDEFINED;
    rtc_sdp_limits_default(&p->lim);
    p->et.ntypes = WJS_PE_N;
    for (i = 0; i < WJS_PE_N; i++)
        p->et.on[i] = JS_NULL;
    if (dyn_os_entropy(seed, sizeof(seed)) < 0) {
        js_free(ctx, p);
        return JS_ThrowInternalError(ctx, "RTCPeerConnection: no OS entropy");
    }
    p->t = wrt_pc_new(NULL, NULL, seed);
    if (!p->t) {
        js_free(ctx, p);
        return JS_ThrowOutOfMemory(ctx);
    }
    p->t->js = p;
    p->t->notify = wjs_notify;
    p->sess_id = ((uint64_t)rtc_rng_u32(&p->t->rng) << 31) ^ rtc_rng_u32(&p->t->rng);
    p->sess_id &= 0x3FFFFFFFFFFFFFFFull;
    p->sess_ver = 1;
    wjs_rand_str(p, p->cname, 16);
    obj = dyn_plain_wrap(ctx, new_target, wjs_pc_cid, NULL, NULL);
    if (JS_IsException(obj)) {
        wrt_pc_free(p->t);
        js_free(ctx, p);
        return obj;
    }
    JS_SetOpaque(obj, p);
    p->self = obj;
    if (argc && JS_IsObject(argv[0])) {
        if (wjs_parse_servers(ctx, p, argv[0]) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        p->config = JS_DupValue(ctx, argv[0]);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && wjs_parse_meta(ctx, p, argv[1]) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue wjs_pc_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return wjs_desc_with_cands(ctx, p, p->pend_local.sdp ? &p->pend_local : &p->cur_local);
    case 1:
        return wjs_desc_with_cands(ctx, p, &p->cur_local);
    case 2:
        return wjs_desc_with_cands(ctx, p, &p->pend_local);
    case 3: {
        wjs_desc* d = p->pend_remote.sdp ? &p->pend_remote : &p->cur_remote;
        return d->sdp ? wjs_desc_new(ctx, d->type, d->sdp, strlen(d->sdp)) : JS_NULL;
    }
    case 4:
        return p->cur_remote.sdp ? wjs_desc_new(ctx, p->cur_remote.type, p->cur_remote.sdp, strlen(p->cur_remote.sdp)) : JS_NULL;
    case 5:
        return p->pend_remote.sdp ? wjs_desc_new(ctx, p->pend_remote.type, p->pend_remote.sdp, strlen(p->pend_remote.sdp))
                                  : JS_NULL;
    case 6:
        return JS_NewString(ctx, wjs_sig_names[p->signaling]);
    case 7:
        return JS_NewString(ctx, wjs_gather_names[p->gather_state]);
    case 8:
        return JS_NewString(ctx, wjs_ice_names[p->closed ? 6 : p->ice_state % 7]);
    case 9:
        return JS_NewString(ctx, wjs_conn_names[p->closed ? WJS_CONN_CLOSED : p->conn_state]);
    case 10:
        return p->can_trickle < 0 ? JS_NULL : JS_NewBool(ctx, p->can_trickle);
    case 11:
        return JS_DupValue(ctx, p->sctp_obj);
    default:
        return JS_UNDEFINED;
    }
}

static JSValue wjs_pc_on_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    return p ? wjs_et_on_get(ctx, &p->et, magic) : JS_EXCEPTION;
}

static JSValue wjs_pc_on_set(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    return p ? wjs_et_on_set(ctx, &p->et, magic, v) : JS_EXCEPTION;
}

static JSValue wjs_pc_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    return p ? wjs_et_listen(ctx, &p->et, wjs_pc_names, WJS_PE_N, argc, argv, magic) : JS_EXCEPTION;
}

static JSValue wjs_pc_dispatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (wjs_dispatch(ctx, this_val, &p->et, wjs_pc_names, WJS_PE_N, argc ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static int wjs_opt_u16(JSContext* ctx, JSValueConst o, const char* k, int32_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    *out = -1;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!isfinite(d) || d < 0 || d > 65535) {
        JS_ThrowTypeError(ctx, "createDataChannel: %s must be an unsigned short", k);
        return -1;
    }
    *out = (int32_t)d;
    return 0;
}

static JSValue wjs_pc_create_dc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    const char *label = NULL, *proto = NULL;
    size_t ll = 0, pl = 0;
    int ordered = 1, negotiated = 0;
    int32_t max_rtx = -1, max_life = -1, id = -1;
    JSValue protov = JS_UNDEFINED, obj = JS_EXCEPTION;
    wrt_dc* dc;
    if (!p)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "createDataChannel: 1 argument required");
    if (p->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "createDataChannel: RTCPeerConnection is closed");
    label = JS_ToCStringLen(ctx, &ll, argv[0]);
    if (!label)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue v;
        if (!JS_IsObject(argv[1])) {
            JS_ThrowTypeError(ctx, "createDataChannel: dataChannelDict must be an object");
            goto done;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "ordered");
        if (JS_IsException(v))
            goto done;
        if (!JS_IsUndefined(v))
            ordered = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (wjs_opt_u16(ctx, argv[1], "maxPacketLifeTime", &max_life) < 0)
            goto done;
        if (max_life < 0 && wjs_opt_u16(ctx, argv[1], "maxRetransmitTime", &max_life) < 0)
            goto done;
        if (wjs_opt_u16(ctx, argv[1], "maxRetransmits", &max_rtx) < 0)
            goto done;
        protov = JS_GetPropertyStr(ctx, argv[1], "protocol");
        if (JS_IsException(protov))
            goto done;
        if (!JS_IsUndefined(protov)) {
            proto = JS_ToCStringLen(ctx, &pl, protov);
            if (!proto)
                goto done;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "negotiated");
        if (JS_IsException(v))
            goto done;
        negotiated = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (wjs_opt_u16(ctx, argv[1], "id", &id) < 0)
            goto done;
    }
    if (ll > 65535) {
        JS_ThrowTypeError(ctx, "createDataChannel: label is longer than 65535 bytes");
        goto done;
    }
    if (pl > 65535) {
        JS_ThrowTypeError(ctx, "createDataChannel: protocol is longer than 65535 bytes");
        goto done;
    }
    if (max_life >= 0 && max_rtx >= 0) {
        JS_ThrowTypeError(ctx, "createDataChannel: maxPacketLifeTime and maxRetransmits are mutually exclusive");
        goto done;
    }
    if (negotiated && id < 0) {
        JS_ThrowTypeError(ctx, "createDataChannel: negotiated channels need an id");
        goto done;
    }
    if (!negotiated)
        id = -1;
    if (id == 65535) {
        JS_ThrowTypeError(ctx, "createDataChannel: id 65535 is reserved");
        goto done;
    }
    if (id >= 0 && wrt_dc_id_taken(p->t, id)) {
        js_throw_dom_exception(ctx, "OperationError", "createDataChannel: id %d is already in use", id);
        goto done;
    }
    if (id >= wrt_dc_id_limit(p->t)) {
        js_throw_dom_exception(ctx, "OperationError", "createDataChannel: id %d is not below maxChannels", id);
        goto done;
    }
    if (id < 0 && p->t->role_known && p->t->sctp && p->t->sctp->state == RTC_SCTP_ESTABLISHED && wrt_dc_free_id(p->t) < 0) {
        js_throw_dom_exception(ctx, "OperationError", "createDataChannel: no stream id available");
        goto done;
    }
    dc = wrt_dc_create(p->t, label, ll, proto ? proto : "", pl, ordered, max_rtx, max_life, negotiated, id);
    if (!dc) {
        js_throw_dom_exception(ctx, "OperationError", "createDataChannel: no channel could be allocated");
        goto done;
    }
    obj = wjs_dc_new(ctx, p, dc);
    if (JS_IsException(obj)) {
        wrt_dc_release(p->t, dc);
        goto done;
    }
    wjs_pc_pin(ctx, p);
    wjs_update_nneeded(ctx, p);
    if (p->t->nev) {
        if (!p->draining)
            wjs_pc_drain(ctx, p);
    }
    wjs_pc_schedule_flush(ctx, p);
done:
    JS_FreeCString(ctx, label);
    if (proto)
        JS_FreeCString(ctx, proto);
    JS_FreeValue(ctx, protov);
    return obj;
}

static JSValue wjs_pc_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    uint32_t i;
    (void)argc;
    (void)argv;
    if (!p)
        return JS_EXCEPTION;
    if (p->closed)
        return JS_UNDEFINED;
    p->closed = 1;
    p->signaling = WJS_SIG_CLOSED;
    p->ice_state = RTC_ICE_CLOSED;
    p->conn_state = WJS_CONN_CLOSED;
    p->sctp_state = 2;
    wjs_sctp_obj_update(ctx, p);
    if (p->t) {
        for (i = 0; i < p->t->ndcs; i++) {
            wrt_dc* dc = p->t->dcs[i];
            if (dc && dc->js)
                ((wjs_dc*)dc->js)->state = WRT_DC_CLOSED;
        }
    }
    wjs_media_close(ctx, p);
    wjs_pc_shutdown(ctx, p);
    if (p->t)
        wrt_pc_events_done(p->t);
    wjs_pc_unpin(JS_GetRuntime(ctx), p);
    return JS_UNDEFINED;
}

static JSValue wjs_pc_restart(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!p)
        return JS_EXCEPTION;
    if (!p->closed && !p->restart_wanted) {
        p->restart_wanted = 1;
        wjs_update_nneeded(ctx, p);
    }
    return JS_UNDEFINED;
}

static JSValue wjs_pc_getconfig(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!p)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_IsObject(p->config)) {
        JSValue s = JS_GetPropertyStr(ctx, p->config, "iceServers");
        JS_SetPropertyStr(ctx, o, "iceServers", JS_IsUndefined(s) ? JS_NewArray(ctx) : s);
    } else
        JS_SetPropertyStr(ctx, o, "iceServers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, o, "iceTransportPolicy", JS_NewString(ctx, p->t && p->t->relay_next ? "relay" : "all"));
    JS_SetPropertyStr(ctx, o, "bundlePolicy", JS_NewString(ctx, wjs_bundle_names[p->bundle_policy]));
    JS_SetPropertyStr(ctx, o, "rtcpMuxPolicy", JS_NewString(ctx, "require"));
    JS_SetPropertyStr(ctx, o, "iceCandidatePoolSize", JS_NewInt32(ctx, 0));
    if (p->cert) {
        JSValue a = JS_NewArray(ctx);
        if (JS_IsArray(ctx, p->certs)) {
            JS_FreeValue(ctx, a);
            a = JS_DupValue(ctx, p->certs);
        } else
            JS_SetPropertyUint32(ctx, a, 0, wjs_cert_new(ctx, p->cert));
        JS_SetPropertyStr(ctx, o, "certificates", a);
    }
    return o;
}

static JSValue wjs_pc_setconfig(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_pc* p = wjs_pc_of(ctx, this_val);
    (void)argc;
    if (!p)
        return JS_EXCEPTION;
    if (p->closed)
        return js_throw_dom_exception(ctx, "InvalidStateError", "setConfiguration: RTCPeerConnection is closed");
    if (argc && JS_IsObject(argv[0])) {
        JSValue c;
        int changed, bundle;
        if (wjs_parse_bundle(ctx, argv[0], &bundle) < 0)
            return JS_EXCEPTION;
        c = JS_GetPropertyStr(ctx, argv[0], "certificates");
        if (JS_IsException(c))
            return c;
        changed = !JS_IsUndefined(c) && !(JS_IsArray(ctx, c) && JS_IsArray(ctx, p->certs));
        if (!changed && !JS_IsUndefined(c)) {
            JSValue a = JS_GetPropertyUint32(ctx, c, 0);
            changed = wjs_cert_get(ctx, a) != p->cert;
            JS_FreeValue(ctx, a);
        }
        JS_FreeValue(ctx, c);
        if (changed)
            return js_throw_dom_exception(ctx, "InvalidModificationError", "setConfiguration: certificates cannot change");
        if (bundle != p->bundle_policy)
            return js_throw_dom_exception(ctx, "InvalidModificationError", "setConfiguration: bundlePolicy cannot change (it is '%s')",
                wjs_bundle_names[p->bundle_policy]);
        if (p->t) {
            wrt_server* saved = (wrt_server*)js_malloc(ctx, sizeof(p->t->servers));
            int nsaved = p->t->nservers, relay = p->t->relay_next, nb = p->t->bind_n, k;
            rtc_addr bsaved[WRT_MAX_SOCKS];
            if (!saved)
                return JS_EXCEPTION;
            memcpy(saved, p->t->servers, sizeof(p->t->servers));
            memcpy(bsaved, p->t->bind_addrs, sizeof(bsaved));
            p->t->nservers = 0;
            p->t->relay_next = 0;
            if (wjs_parse_policy(ctx, p, argv[0]) < 0 || wjs_parse_local_addrs(ctx, p, argv[0]) < 0
                || wjs_parse_ice_servers(ctx, p, argv[0]) < 0) {
                memcpy(p->t->servers, saved, sizeof(p->t->servers));
                p->t->nservers = nsaved;
                p->t->relay_next = relay;
                memcpy(p->t->bind_addrs, bsaved, sizeof(bsaved));
                p->t->bind_n = nb;
                js_free(ctx, saved);
                return JS_EXCEPTION;
            }
            for (k = 0; k < p->t->nturns; k++) {
                int o = p->t->turn_server[k], i, to = 0xFF;
                for (i = 0; o < nsaved && i < p->t->nservers; i++) {
                    const wrt_server* a = &saved[o];
                    const wrt_server* b = &p->t->servers[i];
                    if (!strcmp(a->url, b->url) && !strcmp(a->user, b->user) && !strcmp(a->pass, b->pass)) {
                        to = i;
                        break;
                    }
                }
                p->t->turn_server[k] = (uint8_t)to;
            }
            js_free(ctx, saved);
        }
        JS_FreeValue(ctx, p->config);
        p->config = JS_DupValue(ctx, argv[0]);
    }
    return JS_UNDEFINED;
}

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    int orphan;
    JSValue res[2];
    int rsa;
    uint32_t modulus;
    double expires_ms;
    wrt_cert* c;
    const char* err;
} wjs_cert_job;

static void wjs_cert_work(void* arg)
{
    wjs_cert_job* j = (wjs_cert_job*)arg;
    j->c = wrt_cert_generate(j->rsa, j->modulus, j->expires_ms, &j->err);
}

static void wjs_cert_settle(wjs_cert_job* j)
{
    JSContext* ctx = j->ctx;
    JSValue r;
    if (!j->c) {
        JSValue e = js_dom_exception(ctx, "OperationError", j->err ? j->err : "generateCertificate failed");
        r = JS_Call(ctx, j->res[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, e);
    } else {
        JSValue cv = wjs_cert_new(ctx, j->c);
        wrt_cert_unref(j->c);
        r = JS_Call(ctx, j->res[0], JS_UNDEFINED, 1, (JSValueConst*)&cv);
        JS_FreeValue(ctx, cv);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, j->res[0]);
    JS_FreeValue(ctx, j->res[1]);
}

static void wjs_cert_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    wjs_cert_job* j = (wjs_cert_job*)opaque;
    (void)ctx;
    j->orphan = 1;
    JS_FreeValueRT(rt, j->res[0]);
    JS_FreeValueRT(rt, j->res[1]);
    j->res[0] = j->res[1] = JS_UNDEFINED;
    dyn_net_reactor_release_rt(rt);
}

static void wjs_cert_done(void* arg)
{
    wjs_cert_job* j = (wjs_cert_job*)arg;
    JSContext* ctx = j->ctx;
    if (j->orphan) {
        if (j->c)
            wrt_cert_unref(j->c);
        free(j);
        return;
    }
    JS_RemoveShutdownSweep(j->rt, wjs_cert_sweep, j);
    wjs_cert_settle(j);
    free(j);
    dyn_net_reactor_release(ctx);
}

static JSValue wjs_gen_cert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst a = argc ? argv[0] : JS_UNDEFINED;
    JSValue res[2], prom, v, r;
    int rsa = 0, ok = 1;
    uint32_t modulus = 2048;
    double expires = 30.0 * 86400000.0;
    (void)this_val;
    prom = JS_NewPromiseCapability(ctx, res);
    if (JS_IsException(prom))
        return prom;
    if (JS_IsString(a)) {
        const char* s = JS_ToCString(ctx, a);
        ok = 0;
        if (s)
            JS_FreeCString(ctx, s);
    } else if (JS_IsObject(a)) {
        JSValue n = JS_GetPropertyStr(ctx, a, "name");
        const char* s = JS_IsUndefined(n) ? NULL : JS_ToCString(ctx, n);
        JS_FreeValue(ctx, n);
        if (s && !strcasecmp(s, "ECDSA")) {
            JSValue cv = JS_GetPropertyStr(ctx, a, "namedCurve");
            const char* cs = JS_IsUndefined(cv) ? NULL : JS_ToCString(ctx, cv);
            JS_FreeValue(ctx, cv);
            ok = cs && !strcmp(cs, "P-256");
            if (cs)
                JS_FreeCString(ctx, cs);
        } else if (s && !strcasecmp(s, "RSASSA-PKCS1-v1_5")) {
            JSValue mv = JS_GetPropertyStr(ctx, a, "modulusLength");
            uint32_t m = 2048;
            if (!JS_IsUndefined(mv) && JS_ToUint32(ctx, &m, mv))
                m = 0;
            JS_FreeValue(ctx, mv);
            rsa = 1;
            modulus = m;
            ok = m >= 1024 && m <= 8192;
        } else
            ok = 0;
        if (s)
            JS_FreeCString(ctx, s);
        v = JS_GetPropertyStr(ctx, a, "expires");
        if (!JS_IsUndefined(v)) {
            double d;
            if (!JS_ToFloat64(ctx, &d, v) && d >= 0 && isfinite(d))
                expires = d < 365.0 * 86400000.0 ? d : 365.0 * 86400000.0;
        }
        JS_FreeValue(ctx, v);
    } else
        ok = 0;
    if (!ok || JS_HasException(ctx)) {
        JSValue e = JS_HasException(ctx) ? JS_GetException(ctx) : js_dom_exception(ctx, "NotSupportedError", "generateCertificate: unsupported keygenAlgorithm");
        r = JS_Call(ctx, res[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, e);
    } else {
        wjs_cert_job* j = (wjs_cert_job*)calloc(1, sizeof(*j));
        struct dyn_aio* aio;
        if (!j) {
            JS_FreeValue(ctx, res[0]);
            JS_FreeValue(ctx, res[1]);
            JS_FreeValue(ctx, prom);
            return JS_EXCEPTION;
        }
        j->ctx = ctx;
        j->rt = JS_GetRuntime(ctx);
        j->res[0] = res[0];
        j->res[1] = res[1];
        j->rsa = rsa;
        j->modulus = modulus;
        j->expires_ms = (double)time(NULL) * 1000.0 + expires;
        aio = dyn_net_reactor_acquire(ctx);
        if (aio && JS_AddShutdownSweep(j->rt, wjs_cert_sweep, j) == 0) {
            if (dyn_aio_offload_cpu(aio, wjs_cert_work, wjs_cert_done, j) == 0)
                return prom;
            JS_RemoveShutdownSweep(j->rt, wjs_cert_sweep, j);
        }
        if (aio)
            dyn_net_reactor_release(ctx);
        wjs_cert_work(j);
        wjs_cert_settle(j);
        free(j);
        return prom;
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, res[0]);
    JS_FreeValue(ctx, res[1]);
    return prom;
}

static const JSCFunctionListEntry wjs_pc_proto[] = {
    JS_CFUNC_MAGIC_DEF("createOffer", 0, wjs_pc_op, WJS_OP_OFFER),
    JS_CFUNC_MAGIC_DEF("createAnswer", 0, wjs_pc_op, WJS_OP_ANSWER),
    JS_CFUNC_MAGIC_DEF("setLocalDescription", 0, wjs_pc_op, WJS_OP_SLD),
    JS_CFUNC_MAGIC_DEF("setRemoteDescription", 1, wjs_pc_op, WJS_OP_SRD),
    JS_CFUNC_MAGIC_DEF("addIceCandidate", 0, wjs_pc_op, WJS_OP_ADDCAND),
    JS_CFUNC_MAGIC_DEF("getStats", 0, wjs_pc_op, WJS_OP_STATS),
    JS_CFUNC_DEF("createDataChannel", 1, wjs_pc_create_dc),
    JS_CFUNC_DEF("close", 0, wjs_pc_close),
    JS_CFUNC_DEF("restartIce", 0, wjs_pc_restart),
    JS_CFUNC_DEF("getConfiguration", 0, wjs_pc_getconfig),
    JS_CFUNC_DEF("setConfiguration", 1, wjs_pc_setconfig),
    JS_CFUNC_MAGIC_DEF("getSenders", 0, wjs_pc_media_list, 0),
    JS_CFUNC_MAGIC_DEF("getReceivers", 0, wjs_pc_media_list, 1),
    JS_CFUNC_MAGIC_DEF("getTransceivers", 0, wjs_pc_media_list, 2),
    JS_CFUNC_DEF("addTrack", 1, wjs_pc_add_track),
    JS_CFUNC_DEF("removeTrack", 1, wjs_pc_remove_track),
    JS_CFUNC_DEF("addTransceiver", 1, wjs_pc_add_transceiver),
    JS_CGETSET_MAGIC_DEF("localDescription", wjs_pc_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("currentLocalDescription", wjs_pc_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("pendingLocalDescription", wjs_pc_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("remoteDescription", wjs_pc_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("currentRemoteDescription", wjs_pc_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("pendingRemoteDescription", wjs_pc_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("signalingState", wjs_pc_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("iceGatheringState", wjs_pc_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("iceConnectionState", wjs_pc_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("connectionState", wjs_pc_get, NULL, 9),
    JS_CGETSET_MAGIC_DEF("canTrickleIceCandidates", wjs_pc_get, NULL, 10),
    JS_CGETSET_MAGIC_DEF("sctp", wjs_pc_get, NULL, 11),
    JS_CGETSET_MAGIC_DEF("onicecandidate", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_ICECANDIDATE),
    JS_CGETSET_MAGIC_DEF("onicecandidateerror", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_ICECANDIDATEERROR),
    JS_CGETSET_MAGIC_DEF("onsignalingstatechange", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_SIGNALING),
    JS_CGETSET_MAGIC_DEF("oniceconnectionstatechange", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_ICECONN),
    JS_CGETSET_MAGIC_DEF("onicegatheringstatechange", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_GATHERING),
    JS_CGETSET_MAGIC_DEF("onconnectionstatechange", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_CONN),
    JS_CGETSET_MAGIC_DEF("ondatachannel", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_DATACHANNEL),
    JS_CGETSET_MAGIC_DEF("onnegotiationneeded", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_NEGOTIATION),
    JS_CGETSET_MAGIC_DEF("ontrack", wjs_pc_on_get, wjs_pc_on_set, WJS_PE_TRACK),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, wjs_pc_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, wjs_pc_listen, 1),
    JS_CFUNC_DEF("dispatchEvent", 1, wjs_pc_dispatch),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RTCPeerConnection", JS_PROP_CONFIGURABLE),
};

int wjs_pc_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;
    if (dyn_register_plain_class(ctx, m, &wjs_pc_cid, &wjs_pc_class, wjs_pc_proto, countof(wjs_pc_proto), wjs_pc_ctor,
            "RTCPeerConnection")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, wjs_pc_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsFunction(ctx, ctor))
        JS_SetPropertyStr(ctx, ctor, "generateCertificate", JS_NewCFunction(ctx, wjs_gen_cert, "generateCertificate", 1));
    JS_FreeValue(ctx, ctor);
    return 0;
}

#endif
