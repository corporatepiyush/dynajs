#include "smtp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>

static const struct {
    int code;
    const char *esc, *text;
} deny[HK_N] = {
    { 550, "5.7.1", "Rejected" }, { 535, "5.7.8", "Authentication credentials invalid" }, { 550, "5.7.1", "Sender rejected" },
    { 550, "5.1.1", "Recipient rejected" }, { 0, NULL, NULL }, { 550, "5.7.1", "Message rejected" },
};

static JSClassID ssrv_class_id;
static _Thread_local int ssrv_in_final;

static void ssrv_free(ssrv_t* sv)
{
    int i;
    if (sv->hooked)
        dyn_net_off_drain(sv);
    if (sv->aio && !sv->released)
        dyn_net_reactor_release_rt(sv->rt);
#ifdef CONFIG_TLS
    if (sv->tls_ctx)
        dyn_tls_ctx_free(sv->tls_ctx);
#endif
    for (i = 0; i < HK_N; i++)
        JS_FreeValueRT(sv->rt, sv->hooks[i]);
    free(sv->host);
    free(sv->name);
    free(sv->cert);
    free(sv->key);
    free(sv);
}

void ssrv_unref(ssrv_t* sv)
{
    if (--sv->refs == 0 && sv->cb_depth == 0)
        ssrv_free(sv);
}

static void set_str(JSContext* ctx, JSValueConst o, const char* k, const char* p, size_t n)
{
    JS_SetPropertyStr(ctx, o, k, p ? JS_NewStringLen(ctx, p, n) : JS_NULL);
}

int ssrv_session_new(ssrv_conn_t* c)
{
    ssrv_t* sv = c->sv;
    JSContext* ctx = sv->ctx;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    char host[INET6_ADDRSTRLEN] = "";
    int port = 0;
    JSValue o;
    if (getpeername(c->fd, (struct sockaddr*)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, host, sizeof host);
            port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)&ss)->sin6_addr, host, sizeof host);
            port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
        }
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JS_SetPropertyStr(ctx, o, "id", JS_NewFloat64(ctx, (double)sv->sessions));
    set_str(ctx, o, "remoteAddress", host, strlen(host));
    JS_SetPropertyStr(ctx, o, "remotePort", JS_NewInt32(ctx, port));
    JS_SetPropertyStr(ctx, o, "tls", JS_NewBool(ctx, sv->tls == MAILK_TLS_IMPLICIT));
    set_str(ctx, o, "helo", NULL, 0);
    set_str(ctx, o, "user", NULL, 0);
    c->session = o;
    return 0;
}

static JSValue take_body(JSContext* ctx, mailk_srv_t* e)
{
    char* p = e->body.p;
    size_t n = e->body.n;
    JSValue ab, u8, ta[3];
    dyn_sb_init(&e->body);
    if (!n) {
        free(p);
        ab = JS_NewArrayBufferCopy(ctx, NULL, 0);
    } else
        ab = dyn_nat_ab_take(ctx, p, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_NewInt32(ctx, 0);
    ta[2] = JS_NewFloat64(ctx, (double)n);
    u8 = JS_NewTypedArray(ctx, 3, (JSValueConst*)ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static void dec_set(ssrv_conn_t* c, int code, const char* esc, const char* text)
{
    free(c->dec.text);
    memset(&c->dec, 0, sizeof c->dec);
    c->dec.code = code;
    if (esc)
        snprintf(c->dec.esc, sizeof c->dec.esc, "%s", esc);
    c->dec.text = text ? strdup(text) : NULL;
    c->has_dec = 1;
}

static int key_bytes(JSContext* ctx, JSValueConst o, const char* k, uint8_t* dst, size_t lo, size_t hi, size_t* got)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    size_t n = 0;
    uint8_t* p;
    int ok;
    if (JS_IsException(v)) {
        *got = 0;
        return 0;
    }
    p = smtp_bytes_of(ctx, v, &n);
    ok = p && n >= lo && n <= hi;
    if (ok)
        memcpy(dst, p, n);
    JS_FreeValue(ctx, v);
    *got = n;
    return ok;
}

static int scram_verdict(ssrv_conn_t* c, JSValueConst v)
{
    JSContext* ctx = c->sv->ctx;
    size_t dl = c->eng.mech == (int)MAILK_MECH_SCRAM_SHA1 ? 20 : c->eng.mech == (int)MAILK_MECH_SCRAM_SHA256 ? 32 : 64, a, b;
    int64_t it = 0;
    JSValue iv = JS_GetPropertyStr(ctx, v, "iterations");
    int ok = !JS_IsException(iv) && JS_ToInt64(ctx, &it, iv) == 0 && it >= MAILK_SCRAM_ITER_MIN && it <= MAILK_SCRAM_ITER_MAX;
    JS_FreeValue(ctx, iv);
    dec_set(c, 0, NULL, NULL);
    c->dec.scram = 1;
    c->dec.iters = (uint32_t)it;
    ok = ok && key_bytes(ctx, v, "salt", c->dec.salt, 1, sizeof c->dec.salt, &c->dec.saltn)
        && key_bytes(ctx, v, "storedKey", c->dec.stored, dl, dl, &a) && key_bytes(ctx, v, "serverKey", c->dec.server, dl, dl, &b);
    return ok;
}

static int obj_verdict(ssrv_conn_t* c, JSValueConst v)
{
    JSContext* ctx = c->sv->ctx;
    JSValue cv = JS_GetPropertyStr(ctx, v, "code"), mv = JS_UNDEFINED, ev = JS_UNDEFINED;
    const char *m = NULL, *e = NULL;
    int32_t code = 0;
    int ok;
    if (!JS_IsException(cv))
        mv = JS_GetPropertyStr(ctx, v, "message");
    if (!JS_IsException(mv))
        ev = JS_GetPropertyStr(ctx, v, "enhanced");
    ok = !JS_IsException(cv) && !JS_IsException(mv) && !JS_IsException(ev) && JS_IsNumber(cv)
        && JS_ToInt32(ctx, &code, cv) == 0 && code >= 200 && code <= 599 && JS_IsString(mv)
        && (JS_IsUndefined(ev) || JS_IsString(ev));
    if (ok) {
        m = JS_ToCString(ctx, mv);
        e = JS_IsString(ev) ? JS_ToCString(ctx, ev) : NULL;
        ok = m && (!JS_IsString(ev) || (e && strlen(e) < sizeof c->dec.esc));
    }
    if (ok)
        dec_set(c, code, e, m);
    JS_FreeCString(ctx, m);
    JS_FreeCString(ctx, e);
    JS_FreeValue(ctx, cv);
    JS_FreeValue(ctx, mv);
    JS_FreeValue(ctx, ev);
    return ok;
}

static void verdict(ssrv_conn_t* c, int hk, int rejected, JSValueConst v)
{
    int ok = 1;
    if (rejected)
        ok = 0;
    else if (JS_IsUndefined(v) || (JS_IsBool(v) && JS_ToBool(c->sv->ctx, v)))
        dec_set(c, 0, NULL, NULL);
    else if (JS_IsBool(v))
        dec_set(c, deny[hk].code, deny[hk].esc, deny[hk].text);
    else if (JS_IsObject(v) && hk == HK_AUTH && (unsigned)c->eng.mech >= MAILK_MECH_SCRAM_SHA1)
        ok = scram_verdict(c, v);
    else
        ok = JS_IsObject(v) && obj_verdict(c, v);
    if (JS_HasException(c->sv->ctx))
        dyn_exc_drop(c->sv->ctx);
    if (!ok) {
        c->sv->hook_errors++;
        dec_set(c, hk == HK_AUTH ? 454 : 451, hk == HK_AUTH ? "4.7.0" : "4.3.0", hk == HK_AUTH ? "Temporary authentication failure" : "Local error in processing");
    }
}

static void settle(ssrv_conn_t* c, int hk, int rejected, JSValueConst v)
{
    c->waiting = 0;
    if (c->dead || c->closing)
        return;
    if (hk == HK_CHUNK) {
        if (rejected) {
            c->chunk_err = 1;
            c->sv->hook_errors++;
        }
        c->eng.ev &= ~MAILK_SEV_BODY;
        if (c->eng.pend == MAILK_PEND_DATA)
            ssrv_dispatch(c);
        else
            ssrv_kick(c);
        return;
    }
    verdict(c, hk, rejected, v);
    if (hk == HK_DATA)
        c->chunk_err = c->tail_sent = 0;
    ssrv_kick(c);
}

static JSValue on_react(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    DynResource* r = (DynResource*)JS_GetOpaque(data, ssrv_class_id);
    ssrv_t* sv;
    ssrv_conn_t* c;
    (void)ctx;
    if (!r || r->closed || !r->native)
        return JS_UNDEFINED;
    sv = (ssrv_t*)r->native;
    for (c = sv->conns; c; c = c->next)
        if (c->waiting && c->wait_seq == (uint32_t)magic) {
            uint64_t t0 = smtp_now_ns();
            c->refs++;
            settle(c, c->wait_hk, is_reject, arg);
            c->main_ns += smtp_now_ns() - t0 - c->hook_ns;
            c->hook_ns = 0;
            ssrv_conn_unref(c);
            break;
        }
    if (sv->cb_depth == 0) {
        sv->cb_depth++;
        ssrv_run_parked(sv);
        sv->cb_depth--;
        sv->refs++;
        ssrv_unref(sv);
    }
    return JS_UNDEFINED;
}

static void call_hook(ssrv_conn_t* c, int hk, JSValue arg)
{
    ssrv_t* sv = c->sv;
    JSContext* ctx = sv->ctx;
    JSValueConst argv[2] = { c->session, arg };
    uint64_t t0 = smtp_now_ns();
    JSValue r;
    c->refs++;
    r = JS_Call(ctx, sv->hooks[hk], JS_UNDEFINED, 2, argv);
    c->hook_ns += smtp_now_ns() - t0;
    JS_FreeValue(ctx, arg);
    if (JS_IsException(r)) {
        int unc = JS_HasUncatchableException(ctx);
        JSValue e = JS_GetException(ctx);
        if (unc)
            js_std_set_job_error_rt(JS_GetRuntime(ctx), JS_DupValue(ctx, e));
        settle(c, hk, 1, e);
        JS_FreeValue(ctx, e);
    } else if ((int)JS_PromiseState(ctx, r) >= 0) {
        JSValue rx = JS_NewPromiseReaction(ctx, on_react, sv->self, (int)++sv->seq);
        c->waiting = 1;
        c->wait_hk = hk;
        c->wait_seq = sv->seq;
        if (JS_IsException(rx) || JS_PromiseReact(ctx, r, rx) < 0) {
            dyn_exc_drop(ctx);
            settle(c, hk, 1, JS_UNDEFINED);
        }
        JS_FreeValue(ctx, rx);
    } else
        settle(c, hk, 0, r);
    JS_FreeValue(ctx, r);
    ssrv_conn_unref(c);
}

static JSValue rcpt_obj(JSContext* ctx, const char* p, size_t n, size_t* used)
{
    JSValue o = JS_NewObject(ctx);
    const char *a = p, *nt = a + strlen(a) + 1, *oc = nt + strlen(nt) + 1;
    set_str(ctx, o, "address", a, strlen(a));
    set_str(ctx, o, "notify", *nt ? nt : NULL, strlen(nt));
    set_str(ctx, o, "orcpt", *oc ? oc : NULL, strlen(oc));
    *used = (size_t)(oc + strlen(oc) + 1 - p);
    (void)n;
    return o;
}

static void envelope(JSContext* ctx, JSValueConst o, const mailk_srv_t* e)
{
    static const char* const bodies[] = { "7BIT", "8BITMIME", "BINARYMIME" };
    JS_SetPropertyStr(ctx, o, "body", e->body8 >= 1 && e->body8 <= 2 ? JS_NewString(ctx, bodies[e->body8]) : JS_UNDEFINED);
    JS_SetPropertyStr(ctx, o, "smtputf8", JS_NewBool(ctx, e->utf8));
    JS_SetPropertyStr(ctx, o, "ret", e->ret ? JS_NewString(ctx, e->ret == 1 ? "FULL" : "HDRS") : JS_UNDEFINED);
    set_str(ctx, o, "envid", e->envid.n ? e->envid.p : NULL, e->envid.n);
}

static JSValue pend_arg(ssrv_conn_t* c)
{
    JSContext* ctx = c->sv->ctx;
    mailk_srv_t* e = &c->eng;
    JSValue o = JS_NewObject(ctx);
    size_t off = 0, u;
    switch (e->pend) {
    case MAILK_PEND_HELO:
        set_str(ctx, o, "name", e->helo_arg.p, e->helo_arg.n);
        JS_SetPropertyStr(ctx, o, "ehlo", JS_NewBool(ctx, e->pend_ehlo));
        break;
    case MAILK_PEND_AUTH: {
        static const char* const mn[] = { "PLAIN", "LOGIN", "CRAM-MD5", "XOAUTH2", "OAUTHBEARER", "SCRAM-SHA-1", "SCRAM-SHA-256", "SCRAM-SHA-512" };
        int b = 0;
        while (b < 7 && !((unsigned)e->mech & (1u << b)))
            b++;
        JS_SetPropertyStr(ctx, o, "mechanism", JS_NewString(ctx, mn[b]));
        set_str(ctx, o, "username", e->user.p ? e->user.p : "", e->user.n);
        set_str(ctx, o, "authzid", e->authz.n ? e->authz.p : NULL, e->authz.n);
        if (e->secret.n)
            set_str(ctx, o, e->mech == (int)MAILK_MECH_PLAIN || e->mech == (int)MAILK_MECH_LOGIN ? "password" : "token", e->secret.p, e->secret.n);
        break;
    }
    case MAILK_PEND_MAIL:
        set_str(ctx, o, "address", e->from.p ? e->from.p : "", e->from.n);
        envelope(ctx, o, e);
        break;
    case MAILK_PEND_RCPT:
        JS_FreeValue(ctx, o);
        return rcpt_obj(ctx, e->rcpt.p, e->rcpt.n, &u);
    default: {
        JSValue to = JS_NewArray(ctx);
        uint32_t i = 0;
        set_str(ctx, o, "from", e->from.p ? e->from.p : "", e->from.n);
        while (off < e->rcpts.n) {
            JS_SetPropertyUint32(ctx, to, i++, rcpt_obj(ctx, e->rcpts.p + off, e->rcpts.n - off, &u));
            off += u;
        }
        JS_SetPropertyStr(ctx, o, "to", to);
        JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, (double)e->msg_bytes));
        envelope(ctx, o, e);
        if (JS_IsUndefined(c->sv->hooks[HK_CHUNK]))
            JS_SetPropertyStr(ctx, o, "data", take_body(ctx, e));
        break;
    }
    }
    return o;
}

void ssrv_dispatch(ssrv_conn_t* c)
{
    ssrv_t* sv = c->sv;
    JSContext* ctx = sv->ctx;
    mailk_srv_t* e = &c->eng;
    int hk;
    if (ssrv_in_final || c->dead || sv->closing)
        return;
    JS_SetPropertyStr(ctx, c->session, "tls", JS_NewBool(ctx, e->tls_on));
    set_str(ctx, c->session, "helo", e->helo_arg.n ? e->helo_arg.p : NULL, e->helo_arg.n);
    if (e->authed && !c->user_set) {
        c->user_set = 1;
        set_str(ctx, c->session, "user", e->user.p ? e->user.p : "", e->user.n);
    }
    if ((e->ev & MAILK_SEV_BODY) || (e->pend == MAILK_PEND_DATA && e->body.n && !c->tail_sent && !JS_IsUndefined(sv->hooks[HK_CHUNK]))) {
        c->tail_sent = e->pend == MAILK_PEND_DATA;
        call_hook(c, HK_CHUNK, take_body(ctx, e));
        return;
    }
    for (hk = 0; hk < HK_N && ssrv_hk_pend[hk] != e->pend; hk++)
        ;
    if (hk == HK_N || (hk == HK_DATA && c->chunk_err) || JS_IsUndefined(sv->hooks[hk])) {
        if (hk == HK_DATA && c->chunk_err)
            dec_set(c, 451, "4.3.0", "Local error in processing");
        else
            dec_set(c, 0, NULL, NULL);
        c->chunk_err = c->tail_sent = 0;
        ssrv_kick(c);
        return;
    }
    call_hook(c, hk, pend_arg(c));
}

static void ssrv_shutdown(ssrv_t* sv, int bye)
{
    ssrv_conn_t* c;
    if (sv->closing)
        return;
    sv->closing = 1;
    if (sv->listen_fd >= 0) {
        dyn_aio_close(sv->aio, sv->listen_fd);
        sv->listen_fd = -1;
    }
    for (;;) {
        for (c = sv->conns; c && (c->dead || c->closing); c = c->next)
            ;
        if (!c)
            break;
        ssrv_kill(c, bye);
    }
    ssrv_run_parked(sv);
    if (sv->hooked) {
        dyn_net_off_drain(sv);
        sv->hooked = 0;
    }
}

static void ssrv_dispose(void* native)
{
    ssrv_t* sv = (ssrv_t*)native;
    if (!sv)
        return;
    sv->cb_depth++;
    ssrv_shutdown(sv, !ssrv_in_final);
    sv->cb_depth--;
    ssrv_unref(sv);
}

static void ssrv_finalizer(JSRuntime* rt, JSValue val)
{
    ssrv_in_final++;
    dyn_res_finalizer(rt, val);
    ssrv_in_final--;
}

static JSValue ssrv_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ssrv_t* sv;
    int i;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SmtpServer: expects an options object");
    sv = (ssrv_t*)calloc(1, sizeof *sv);
    if (!sv)
        return JS_ThrowOutOfMemory(ctx);
    sv->ctx = ctx;
    sv->rt = JS_GetRuntime(ctx);
    sv->refs = 1;
    sv->listen_fd = -1;
    sv->self = JS_UNDEFINED;
    for (i = 0; i < HK_N; i++)
        sv->hooks[i] = JS_UNDEFINED;
    sv->released = 1;
    if (ssrv_opts(ctx, sv, o) < 0) {
        ssrv_free(sv);
        return JS_EXCEPTION;
    }
#ifdef CONFIG_TLS
    if (sv->tls != MAILK_TLS_NONE) {
        dyn_tls_srv_opts_t to;
        char terr[192];
        memset(&to, 0, sizeof to);
        to.cert = sv->cert;
        to.key = sv->key;
        sv->tls_ctx = dyn_tls_ctx_server(&to, terr, sizeof terr);
        if (!sv->tls_ctx) {
            ssrv_free(sv);
            return JS_ThrowTypeError(ctx, "SmtpServer: %s", terr);
        }
    }
#else
    if (sv->tls != MAILK_TLS_NONE) {
        ssrv_free(sv);
        return dyn_tls_missing(ctx, "SmtpServer", "tls");
    }
#endif
    sv->aio = dyn_net_reactor_acquire(ctx);
    if (!sv->aio) {
        ssrv_free(sv);
        return dyn_net_throw_no_reactor(ctx, "SmtpServer");
    }
    sv->released = 0;
    return dyn_res_wrap(ctx, new_target, ssrv_class_id, sv, ssrv_dispose);
}

static ssrv_t* ssrv_this(JSContext* ctx, JSValueConst this_val)
{
    return (ssrv_t*)dyn_res_native(ctx, this_val, ssrv_class_id);
}

static JSValue ssrv_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssrv_t* sv = ssrv_this(ctx, this_val);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    (void)argc;
    (void)argv;
    if (!sv)
        return JS_EXCEPTION;
    if (sv->listening || sv->closing)
        return JS_ThrowTypeError(ctx, "SmtpServer: already listening or closed");
    sv->listen_fd = dyn_aio_listen(sv->aio, sv->host ? sv->host : "127.0.0.1", sv->port, 512);
    if (sv->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "SmtpServer: cannot listen on %s:%u", sv->host ? sv->host : "127.0.0.1", sv->port);
    if (getsockname(sv->listen_fd, (struct sockaddr*)&ss, &sl) == 0)
        sv->port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&ss)->sin6_port : ((struct sockaddr_in*)&ss)->sin_port);
    if (dyn_net_on_drain(ssrv_tick, sv) == 0)
        sv->hooked = 1;
    if (dyn_aio_accept(sv->aio, sv->listen_fd, ssrv_on_accept, sv) < 0 || dyn_res_pin(ctx, this_val, ssrv_class_id) < 0) {
        ssrv_shutdown(sv, 0);
        return JS_ThrowInternalError(ctx, "SmtpServer: accept failed");
    }
    sv->self = this_val;
    sv->listening = 1;
    return JS_NewInt32(ctx, sv->port);
}

static JSValue ssrv_get_port(JSContext* ctx, JSValueConst this_val)
{
    ssrv_t* sv = ssrv_this(ctx, this_val);
    return sv ? JS_NewInt32(ctx, sv->port) : JS_EXCEPTION;
}

static JSValue ssrv_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssrv_t* sv = ssrv_this(ctx, this_val);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!sv)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "connections", JS_NewInt32(ctx, sv->nconns));
    JS_SetPropertyStr(ctx, o, "sessions", JS_NewFloat64(ctx, (double)sv->sessions));
    JS_SetPropertyStr(ctx, o, "refused", JS_NewFloat64(ctx, (double)sv->refused));
    JS_SetPropertyStr(ctx, o, "messages", JS_NewFloat64(ctx, (double)sv->msgs));
    JS_SetPropertyStr(ctx, o, "jobs", JS_NewFloat64(ctx, (double)sv->jobs));
    JS_SetPropertyStr(ctx, o, "hookErrors", JS_NewFloat64(ctx, (double)sv->hook_errors));
    JS_SetPropertyStr(ctx, o, "handoffP50Us", JS_NewFloat64(ctx, smtp_pct(sv->hs, sv->hsn, 0.5)));
    JS_SetPropertyStr(ctx, o, "handoffP99Us", JS_NewFloat64(ctx, smtp_pct(sv->hs, sv->hsn, 0.99)));
    JS_SetPropertyStr(ctx, o, "mainP50Us", JS_NewFloat64(ctx, smtp_pct(sv->ms, sv->msn, 0.5)));
    JS_SetPropertyStr(ctx, o, "mainP99Us", JS_NewFloat64(ctx, smtp_pct(sv->ms, sv->msn, 0.99)));
    return o;
}

static const JSCFunctionListEntry ssrv_proto[] = {
    JS_CFUNC_DEF("listen", 0, ssrv_listen),
    JS_CFUNC_DEF("stats", 0, ssrv_stats),
    JS_CGETSET_DEF("port", ssrv_get_port, NULL),
};

static void ssrv_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, ssrv_class_id);
    ssrv_t* sv;
    ssrv_conn_t* c;
    int i;
    if (!res || res->closed || !res->native)
        return;
    sv = (ssrv_t*)res->native;
    for (i = 0; i < HK_N; i++)
        JS_MarkValue(rt, sv->hooks[i], mark_func);
    for (c = sv->conns; c; c = c->next)
        JS_MarkValue(rt, c->session, mark_func);
}

static const JSClassDef ssrv_class = {
    "SmtpServer",
    .finalizer = ssrv_finalizer,
    .gc_mark = ssrv_gc_mark,
};

int ssrv_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &ssrv_class_id, &ssrv_class, ssrv_proto, countof(ssrv_proto), ssrv_ctor, "SmtpServer");
}
#endif
