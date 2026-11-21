#include "smtp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)

static const char* const smtp_ctor_keys[] = {
    "host", "port", "tls", "ca", "servername", "name", "user", "pass", "token", "authzid", "mechanisms",
    "insecureAuth", "connectTimeoutMs", "commandTimeoutMs", "idleTimeoutMs", "maxConnections",
    "maxMessagesPerConnection", "scramMaxIterations",
};

static const char* const smtp_send_keys[] = { "from", "to", "data", "bdat", "dsn" };
static const char* const smtp_dsn_keys[] = { "ret", "envid", "notify", "orcpt" };

static JSClassID smtp_class_id;
static _Thread_local int smtp_in_final;

static void smtp_strong(smtp_t* s, int on)
{
    if (!s->aio || s->released || on == s->strong)
        return;
    s->strong = on;
    if (on)
        dyn_net_reactor_hold();
    else
        dyn_net_reactor_release_rt(s->rt);
}

static void smtp_free(smtp_t* s)
{
    if (s->hooked)
        dyn_net_off_drain(s);
    if (s->aio && !s->released) {
        smtp_strong(s, 0);
        dyn_net_reactor_weak_drop(s->rt);
    }
#ifdef CONFIG_TLS
    if (s->tls_ctx)
        dyn_tls_ctx_free(s->tls_ctx);
#endif
    free(s->host);
    free(s->ca);
    free(s->servername);
    free(s->name);
    free(s->user);
    free(s->authzid);
    if (s->pass) {
        volatile char* v = s->pass;
        while (*v)
            *v++ = 0;
        free(s->pass);
    }
    if (s->token) {
        volatile char* v = s->token;
        while (*v)
            *v++ = 0;
        free(s->token);
    }
    free(s);
}

void smtp_enter(smtp_t* s)
{
    s->cb_depth++;
}

void smtp_leave(smtp_t* s)
{
    if (s->cb_depth == 1)
        smtp_run_parked(s);
    if (--s->cb_depth == 0 && s->refs == 0)
        smtp_free(s);
}

void smtp_unref(smtp_t* s)
{
    if (--s->refs == 0 && s->cb_depth == 0)
        smtp_free(s);
}

static void smtp_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    smtp_t* s = (smtp_t*)opaque;
    JSValue v;
    (void)ctx;
    if (!s->self_held)
        return;
    v = s->self;
    s->self = JS_UNDEFINED;
    s->self_held = 0;
    smtp_strong(s, 0);
    JS_FreeValueRT(rt, v);
}

static void smtp_hold(JSContext* ctx, JSValueConst obj, smtp_t* s)
{
    if (s->self_held)
        return;
    s->self = JS_DupValue(ctx, obj);
    s->self_held = 1;
    smtp_strong(s, 1);
    JS_AddShutdownSweep(s->rt, smtp_sweep, s);
}

void smtp_release_if_idle(smtp_t* s)
{
    JSValue v;
    if (s->quitting && !s->nconns && !s->qh && JS_IsFunction(s->ctx, s->quit_resolve)) {
        JSValue f = s->quit_resolve, r;
        s->quit_resolve = JS_UNDEFINED;
        if (!smtp_in_final) {
            r = JS_Call(s->ctx, f, JS_UNDEFINED, 0, NULL);
            JS_FreeValue(s->ctx, r);
        }
        JS_FreeValueRT(s->rt, f);
    }
    if (!s->self_held || s->nconns || s->qh || smtp_in_final)
        return;
    v = s->self;
    s->self = JS_UNDEFINED;
    s->self_held = 0;
    smtp_strong(s, 0);
    JS_RemoveShutdownSweep(s->rt, smtp_sweep, s);
    JS_FreeValueRT(s->rt, v);
}

void smtp_msg_free(JSRuntime* rt, smtp_msg_t* m)
{
    size_t i;
    JS_FreeValueRT(rt, m->resolve);
    JS_FreeValueRT(rt, m->reject);
    free(m->from);
    for (i = 0; i < m->nto; i++)
        free(m->to[i]);
    free(m->to);
    free(m->body);
    free(m->ret);
    free(m->envid);
    free(m->notify);
    free(m);
}

void smtp_settle_msg(smtp_t* s, smtp_msg_t* m, int reject, JSValue v)
{
    JSValue fn = reject ? m->reject : m->resolve;
    s->msgs++;
    s->ms[s->msn % SMTP_SAMPLES] = (uint32_t)(m->main_ns / 1000 > 0xFFFFFFFFull ? 0xFFFFFFFFu : m->main_ns / 1000);
    s->msn++;
    if (!smtp_in_final && JS_IsFunction(s->ctx, fn)) {
        JSValueConst a[1] = { v };
        JSValue r = JS_Call(s->ctx, fn, JS_UNDEFINED, 1, a);
        JS_FreeValue(s->ctx, r);
    }
    JS_FreeValueRT(s->rt, v);
    smtp_msg_free(s->rt, m);
}

static JSValue res_text(JSContext* ctx, const mailk_cli_t* e, const mailk_res_t* r)
{
    return JS_NewStringLen(ctx, e->texts.p ? e->texts.p + r->off : "", r->len);
}

static JSValue res_enh(JSContext* ctx, const mailk_res_t* r)
{
    char b[48];
    if (r->cls < 0 || !r->code)
        return JS_NULL;
    snprintf(b, sizeof b, "%d.%d.%d", r->cls, r->sub, r->det);
    return JS_NewString(ctx, b);
}

static JSValue rejected_list(JSContext* ctx, const smtp_msg_t* m, const mailk_cli_t* e)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    size_t i;
    for (i = 0; m && e && i < e->nrcpt && i < m->nto; i++) {
        const mailk_res_t* r = &e->rcpt[i];
        JSValue o;
        if (r->code / 100 == 2)
            continue;
        o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "recipient", JS_NewString(ctx, m->to[i]));
        JS_SetPropertyStr(ctx, o, "code", JS_NewInt32(ctx, r->code));
        JS_SetPropertyStr(ctx, o, "enhanced", res_enh(ctx, r));
        JS_SetPropertyStr(ctx, o, "text", r->code ? res_text(ctx, e, r) : JS_NewString(ctx, ""));
        JS_SetPropertyUint32(ctx, a, k++, o);
    }
    return a;
}

JSValue smtp_error(JSContext* ctx, const char* code, const char* msg, const mailk_cli_t* e, const mailk_res_t* r)
{
    JSValue err = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, code), JS_PROP_C_W_E);
    if (e && r && r->code) {
        JS_DefinePropertyValueStr(ctx, err, "smtpCode", JS_NewInt32(ctx, r->code), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, err, "enhanced", res_enh(ctx, r), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, err, "response", res_text(ctx, e, r), JS_PROP_C_W_E);
    }
    return err;
}

void smtp_add_rejected(JSContext* ctx, JSValueConst err, const smtp_msg_t* m, const mailk_cli_t* e)
{
    JS_DefinePropertyValueStr(ctx, err, "rejected", rejected_list(ctx, m, e), JS_PROP_C_W_E);
}

JSValue smtp_result(JSContext* ctx, smtp_msg_t* m, const mailk_cli_t* e)
{
    JSValue o = JS_NewObject(ctx), acc = JS_NewArray(ctx);
    uint32_t k = 0;
    size_t i;
    for (i = 0; i < e->nrcpt && i < m->nto; i++)
        if (e->rcpt[i].code / 100 == 2)
            JS_SetPropertyUint32(ctx, acc, k++, JS_NewString(ctx, m->to[i]));
    JS_SetPropertyStr(ctx, o, "accepted", acc);
    JS_SetPropertyStr(ctx, o, "rejected", rejected_list(ctx, m, e));
    JS_SetPropertyStr(ctx, o, "code", JS_NewInt32(ctx, e->final.code));
    JS_SetPropertyStr(ctx, o, "enhanced", res_enh(ctx, &e->final));
    JS_SetPropertyStr(ctx, o, "text", res_text(ctx, e, &e->final));
    return o;
}

static void smtp_shutdown(smtp_t* s, const char* why)
{
    smtp_conn_t* c;
    smtp_msg_t* m;
    if (s->closing)
        return;
    s->closing = 1;
    for (;;) {
        for (c = s->conns; c && c->dead; c = c->next)
            ;
        if (!c)
            break;
        c->quitting = 1;
        c->ready = 1;
        smtp_conn_kill(c, "SMTP_CLOSED", why);
    }
    while ((m = s->qh) != NULL) {
        s->qh = m->next;
        s->nq--;
        smtp_settle_msg(s, m, 1, smtp_error(s->ctx, "SMTP_CLOSED", why, NULL, NULL));
    }
    s->qt = NULL;
    s->quitting = 1;
    smtp_release_if_idle(s);
    if (s->self_held) {
        JSValue v = s->self;
        s->self = JS_UNDEFINED;
        s->self_held = 0;
        smtp_strong(s, 0);
        JS_RemoveShutdownSweep(s->rt, smtp_sweep, s);
        JS_FreeValueRT(s->rt, v);
    }
    JS_FreeValueRT(s->rt, s->quit_resolve);
    s->quit_resolve = JS_UNDEFINED;
    if (s->hooked) {
        dyn_net_off_drain(s);
        s->hooked = 0;
    }
}

static void smtp_dispose(void* native)
{
    smtp_t* s = (smtp_t*)native;
    if (!s)
        return;
    smtp_enter(s);
    smtp_shutdown(s, "SMTP: client closed");
    s->refs--;
    smtp_leave(s);
}

static void smtp_finalizer(JSRuntime* rt, JSValue val)
{
    smtp_in_final++;
    dyn_res_finalizer(rt, val);
    smtp_in_final--;
}

char* smtp_opt_str(JSContext* ctx, JSValueConst o, const char* k, int* bad)
{
    JSValue v;
    const char* s;
    size_t n;
    char* out = NULL;
    if (*bad)
        return NULL;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v)) {
        *bad = 1;
        return NULL;
    }
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SMTP: option '%s' must be a string", k);
        *bad = 1;
        return NULL;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        *bad = 1;
        return NULL;
    }
    if (memchr(s, 0, n) || memchr(s, '\r', n) || memchr(s, '\n', n)) {
        JS_ThrowTypeError(ctx, "SMTP: option '%s' contains NUL, CR or LF", k);
        *bad = 1;
    } else {
        out = strdup(s);
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            *bad = 1;
        }
    }
    JS_FreeCString(ctx, s);
    return out;
}

int64_t smtp_opt_num(JSContext* ctx, JSValueConst o, const char* k, int64_t dflt, int64_t lo, int64_t hi, int* bad)
{
    JSValue v;
    double d;
    if (*bad)
        return dflt;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v)) {
        *bad = 1;
        return dflt;
    }
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return dflt;
    }
    if (JS_ToFloat64(ctx, &d, v) < 0 || !(d >= (double)lo && d <= (double)hi) || d != (double)(int64_t)d) {
        JS_FreeValue(ctx, v);
        if (!JS_HasException(ctx))
            JS_ThrowRangeError(ctx, "SMTP: option '%s' must be an integer in %lld..%lld", k, (long long)lo, (long long)hi);
        *bad = 1;
        return dflt;
    }
    JS_FreeValue(ctx, v);
    return (int64_t)d;
}

unsigned smtp_opt_mechs(JSContext* ctx, JSValueConst o, int* bad)
{
    static const struct {
        const char* n;
        unsigned b;
    } t[] = { { "PLAIN", MAILK_MECH_PLAIN }, { "LOGIN", MAILK_MECH_LOGIN }, { "CRAM-MD5", MAILK_MECH_CRAM_MD5 },
        { "XOAUTH2", MAILK_MECH_XOAUTH2 }, { "OAUTHBEARER", MAILK_MECH_OAUTHBEARER }, { "SCRAM-SHA-1", MAILK_MECH_SCRAM_SHA1 },
        { "SCRAM-SHA-256", MAILK_MECH_SCRAM_SHA256 }, { "SCRAM-SHA-512", MAILK_MECH_SCRAM_SHA512 } };
    JSValue v = JS_GetPropertyStr(ctx, o, "mechanisms");
    unsigned m = 0;
    uint32_t n = 0, i;
    if (JS_IsException(v)) {
        *bad = 1;
        return 0;
    }
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SMTP: mechanisms must be an array of SASL mechanism names");
        *bad = 1;
        return 0;
    }
    if (dyn_js_len_u32(ctx, v, &n)) {
        JS_FreeValue(ctx, v);
        *bad = 1;
        return 0;
    }
    for (i = 0; i < n && !*bad; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        const char* s = JS_ToCString(ctx, e);
        size_t k, hit = 0;
        JS_FreeValue(ctx, e);
        if (!s) {
            *bad = 1;
            break;
        }
        for (k = 0; k < sizeof t / sizeof t[0]; k++)
            if (!strcmp(s, t[k].n)) {
                m |= t[k].b;
                hit = 1;
            }
        if (!hit) {
            JS_ThrowRangeError(ctx, "SMTP: unknown SASL mechanism '%s'", s);
            *bad = 1;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    if (!m && !*bad) {
        JS_ThrowRangeError(ctx, "SMTP: mechanisms is empty");
        *bad = 1;
    }
    return m;
}

static JSValue smtp_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    smtp_t* s;
    char* tls = NULL;
    int bad = 0;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SmtpClient: expects an options object with at least host");
    if (dyn_opts_strict(ctx, o, smtp_ctor_keys, (int)countof(smtp_ctor_keys)))
        return JS_EXCEPTION;
    s = (smtp_t*)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->ctx = ctx;
    s->rt = JS_GetRuntime(ctx);
    s->refs = 1;
    s->self = JS_UNDEFINED;
    s->quit_resolve = JS_UNDEFINED;
    s->host = smtp_opt_str(ctx, o, "host", &bad);
    tls = smtp_opt_str(ctx, o, "tls", &bad);
    s->ca = smtp_opt_str(ctx, o, "ca", &bad);
    s->servername = smtp_opt_str(ctx, o, "servername", &bad);
    s->name = smtp_opt_str(ctx, o, "name", &bad);
    s->user = smtp_opt_str(ctx, o, "user", &bad);
    s->pass = smtp_opt_str(ctx, o, "pass", &bad);
    s->token = smtp_opt_str(ctx, o, "token", &bad);
    s->authzid = smtp_opt_str(ctx, o, "authzid", &bad);
    if (!bad)
        s->mechs = smtp_opt_mechs(ctx, o, &bad);
    if (!bad) {
        int ia = dyn_get_truthy(ctx, o, "insecureAuth");
        if (ia < 0)
            bad = 1;
        else
            s->insecure_auth = ia;
    }
    s->tls = MAILK_TLS_STARTTLS;
    if (!bad && tls) {
        if (!strcmp(tls, "starttls"))
            s->tls = MAILK_TLS_STARTTLS;
        else if (!strcmp(tls, "opportunistic"))
            s->tls = MAILK_TLS_OPPORTUNISTIC;
        else if (!strcmp(tls, "implicit"))
            s->tls = MAILK_TLS_IMPLICIT;
        else if (!strcmp(tls, "none"))
            s->tls = MAILK_TLS_NONE;
        else {
            JS_ThrowRangeError(ctx, "SMTP: tls must be 'starttls', 'opportunistic', 'implicit' or 'none'");
            bad = 1;
        }
    }
    free(tls);
#ifndef CONFIG_TLS
    if (!bad && s->tls != MAILK_TLS_NONE) {
        smtp_free(s);
        return dyn_tls_missing(ctx, "SmtpClient", "tls");
    }
#endif
    if (!bad) {
        s->port = (uint16_t)smtp_opt_num(ctx, o, "port", s->tls == MAILK_TLS_IMPLICIT ? 465 : 587, 1, 65535, &bad);
        s->connect_ms = (uint64_t)smtp_opt_num(ctx, o, "connectTimeoutMs", 10000, 1, 3600000, &bad);
        s->command_ms = (uint64_t)smtp_opt_num(ctx, o, "commandTimeoutMs", 300000, 1, 3600000, &bad);
        s->idle_timeout_ms = (uint64_t)smtp_opt_num(ctx, o, "idleTimeoutMs", 200, 0, 3600000, &bad);
        s->max_conns = (int)smtp_opt_num(ctx, o, "maxConnections", 1, 1, 64, &bad);
        s->max_per_conn = (uint32_t)smtp_opt_num(ctx, o, "maxMessagesPerConnection", 100, 0, 1000000, &bad);
        s->scram_max_iters = (uint32_t)smtp_opt_num(ctx, o, "scramMaxIterations", 0, 4096, MAILK_SCRAM_ITER_MAX, &bad);
    }
    if (!bad && !s->host) {
        JS_ThrowTypeError(ctx, "SmtpClient: host is required");
        bad = 1;
    }
    if (!bad && ((s->user && !s->pass && !s->token) || ((s->pass || s->token) && !s->user))) {
        JS_ThrowTypeError(ctx, "SmtpClient: AUTH needs user with pass or token");
        bad = 1;
    }
    if (!bad) {
        mailk_cli_t probe;
        mailk_cli_opts_t po;
        memset(&po, 0, sizeof po);
        po.name = s->name;
        po.tls = s->tls;
        if (mailk_cli_init(&probe, &po) != MAILK_OK) {
            JS_ThrowTypeError(ctx, "SmtpClient: name must be a domain or an address literal");
            bad = 1;
        }
        mailk_cli_free(&probe);
    }
    if (bad) {
        smtp_free(s);
        return JS_EXCEPTION;
    }
    s->aio = dyn_net_reactor_acquire(ctx);
    if (!s->aio) {
        s->released = 1;
        smtp_free(s);
        return dyn_net_throw_no_reactor(ctx, "SmtpClient");
    }
    dyn_net_reactor_weak_hold();
    dyn_net_reactor_release_rt(s->rt);
    if (dyn_net_on_drain(smtp_tick, s) == 0)
        s->hooked = 1;
    return dyn_res_wrap(ctx, new_target, smtp_class_id, s, smtp_dispose);
}

static smtp_t* smtp_this(JSContext* ctx, JSValueConst this_val)
{
    return (smtp_t*)dyn_res_native(ctx, this_val, smtp_class_id);
}

static char* addr_str(JSContext* ctx, JSValueConst v, const char* what)
{
    const char* s;
    size_t n;
    char* out;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "SmtpClient.send: %s must be a string address", what);
        return NULL;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return NULL;
    if (memchr(s, 0, n) || memchr(s, '\r', n) || memchr(s, '\n', n) || n > MAILK_PATH_MAX) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "SmtpClient.send: %s has NUL, CR, LF or is over 256 octets", what);
        return NULL;
    }
    out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!out)
        JS_ThrowOutOfMemory(ctx);
    return out;
}

uint8_t* smtp_bytes_of(JSContext* ctx, JSValueConst v, size_t* n)
{
    int k = JS_GetBufferKind(v);
    static uint8_t empty[1];
    if (k == JS_BUFFER_KIND_BUFFER) {
        uint8_t* p = JS_GetArrayBuffer(ctx, n, v);
        return p ? p : (*n == 0 ? empty : NULL);
    }
    if (k == JS_BUFFER_KIND_VIEW) {
        size_t off = 0, len = 0, bpe = 0, bl = 0;
        JSValue b = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        uint8_t* p;
        if (JS_IsException(b)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return NULL;
        }
        p = JS_GetArrayBuffer(ctx, &bl, b);
        JS_FreeValue(ctx, b);
        if (!p || off + len > bl)
            return len == 0 ? empty : NULL;
        *n = len;
        return p + off;
    }
    return NULL;
}

static int msg_parse(JSContext* ctx, JSValueConst o, smtp_msg_t* m)
{
    JSValue v;
    int bad = 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "SmtpClient.send: expects { from, to, data }");
        return -1;
    }
    if (dyn_opts_strict(ctx, o, smtp_send_keys, (int)countof(smtp_send_keys)))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "from");
    if (JS_IsException(v))
        return -1;
    m->from = addr_str(ctx, v, "from");
    JS_FreeValue(ctx, v);
    if (!m->from)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "to");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        m->to = (char**)calloc(1, sizeof(char*));
        if (!m->to) {
            JS_ThrowOutOfMemory(ctx);
            bad = 1;
        } else if ((m->to[0] = addr_str(ctx, v, "to")) != NULL)
            m->nto = 1;
        else
            bad = 1;
    } else if (JS_IsArray(ctx, v)) {
        uint32_t n = 0, i;
        if (dyn_js_len_u32(ctx, v, &n)) {
            bad = 1;
        } else if (!n || n > MAILK_RCPT_MAX) {
            JS_ThrowRangeError(ctx, "SmtpClient.send: to needs 1..1000 addresses");
            bad = 1;
        } else if (!(m->to = (char**)calloc(n, sizeof(char*)))) {
            JS_ThrowOutOfMemory(ctx);
            bad = 1;
        }
        for (i = 0; !bad && i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(e)) {
                bad = 1;
                break;
            }
            m->to[i] = addr_str(ctx, e, "to[]");
            JS_FreeValue(ctx, e);
            if (!m->to[i])
                bad = 1;
            else
                m->nto = i + 1;
        }
    } else {
        JS_ThrowTypeError(ctx, "SmtpClient.send: to must be a string or an array of strings");
        bad = 1;
    }
    JS_FreeValue(ctx, v);
    if (bad)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "data");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (s) {
            m->body = (uint8_t*)malloc(n ? n : 1);
            if (m->body) {
                memcpy(m->body, s, n);
                m->bodyn = n;
            }
            JS_FreeCString(ctx, s);
        }
    } else {
        size_t n = 0;
        uint8_t* p = smtp_bytes_of(ctx, v, &n);
        if (!p) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "SmtpClient.send: data must be a string, Uint8Array or ArrayBuffer");
            return -1;
        }
        m->body = (uint8_t*)malloc(n ? n : 1);
        if (m->body) {
            memcpy(m->body, p, n);
            m->bodyn = n;
        }
    }
    JS_FreeValue(ctx, v);
    if (!m->body) {
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    m->bdat = dyn_get_truthy(ctx, o, "bdat");
    if (m->bdat < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "dsn");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        JSValue n;
        if (!JS_IsObject(v) || dyn_opts_strict(ctx, v, smtp_dsn_keys, (int)countof(smtp_dsn_keys))) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "SmtpClient.send: dsn must be an object");
            JS_FreeValue(ctx, v);
            return -1;
        }
        m->ret = smtp_opt_str(ctx, v, "ret", &bad);
        m->envid = smtp_opt_str(ctx, v, "envid", &bad);
        n = bad ? JS_UNDEFINED : JS_GetPropertyStr(ctx, v, "notify");
        if (JS_IsException(n)) {
            bad = 1;
        } else if (!bad && JS_IsArray(ctx, n)) {
            dyn_sb_t b;
            uint32_t k = 0, i;
            if (dyn_js_len_u32(ctx, n, &k))
                bad = 1;
            dyn_sb_init(&b);
            for (i = 0; i < k && i < 4; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, n, i);
                const char* es = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
                JS_FreeValue(ctx, e);
                if (!es) {
                    bad = 1;
                    break;
                }
                if (i)
                    dyn_sb_putc(&b, ',');
                dyn_sb_puts(&b, es);
                JS_FreeCString(ctx, es);
            }
            dyn_sb_putc(&b, 0);
            m->notify = b.oom ? NULL : b.p;
            if (b.oom) {
                dyn_sb_free(&b);
                JS_ThrowOutOfMemory(ctx);
                bad = 1;
            }
        } else if (!bad && !JS_IsUndefined(n)) {
            JS_ThrowTypeError(ctx, "SmtpClient.send: dsn.notify must be an array of 'NEVER', 'SUCCESS', 'FAILURE', 'DELAY'");
            bad = 1;
        }
        JS_FreeValue(ctx, n);
        if (!bad) {
            m->orcpt = dyn_get_truthy(ctx, v, "orcpt");
            bad = m->orcpt < 0;
        }
    }
    JS_FreeValue(ctx, v);
    return bad ? -1 : 0;
}

static JSValue smtp_send(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    smtp_t* s = smtp_this(ctx, this_val);
    smtp_msg_t* m;
    JSValue funcs[2], p;
    if (!s)
        return JS_EXCEPTION;
    if (s->closing || s->quitting)
        return JS_ThrowInternalError(ctx, "SmtpClient.send: the client is closed or quitting");
    m = (smtp_msg_t*)calloc(1, sizeof *m);
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->resolve = m->reject = JS_UNDEFINED;
    if (msg_parse(ctx, argc ? argv[0] : JS_UNDEFINED, m) < 0) {
        smtp_msg_free(s->rt, m);
        return JS_EXCEPTION;
    }
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
        smtp_msg_free(s->rt, m);
        return p;
    }
    m->resolve = funcs[0];
    m->reject = funcs[1];
    if (s->qt)
        s->qt->next = m;
    else
        s->qh = m;
    s->qt = m;
    s->nq++;
    smtp_hold(ctx, this_val, s);
    smtp_enter(s);
    smtp_pump(s);
    smtp_leave(s);
    return p;
}

static JSValue smtp_quit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    smtp_t* s = smtp_this(ctx, this_val);
    JSValue funcs[2], p;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p))
        return p;
    if (s->closing || (!s->nconns && !s->qh)) {
        JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        s->quitting = 1;
        return p;
    }
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, s->quit_resolve);
    s->quit_resolve = funcs[0];
    s->quitting = 1;
    smtp_hold(ctx, this_val, s);
    smtp_enter(s);
    smtp_pump(s);
    smtp_leave(s);
    return p;
}

static int u32cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

double smtp_pct(const uint32_t* ring, uint64_t n, double q)
{
    uint32_t tmp[SMTP_SAMPLES];
    size_t k = n < SMTP_SAMPLES ? (size_t)n : SMTP_SAMPLES;
    if (!k)
        return 0;
    memcpy(tmp, ring, k * sizeof *tmp);
    qsort(tmp, k, sizeof *tmp, u32cmp);
    return tmp[(size_t)(q * (double)(k - 1))];
}

static JSValue smtp_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    smtp_t* s = smtp_this(ctx, this_val);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "messages", JS_NewFloat64(ctx, (double)s->msgs));
    JS_SetPropertyStr(ctx, o, "jobs", JS_NewFloat64(ctx, (double)s->jobs));
    JS_SetPropertyStr(ctx, o, "connections", JS_NewInt32(ctx, s->nconns));
    JS_SetPropertyStr(ctx, o, "queued", JS_NewInt32(ctx, s->nq));
    JS_SetPropertyStr(ctx, o, "handoffP50Us", JS_NewFloat64(ctx, smtp_pct(s->hs, s->hsn, 0.5)));
    JS_SetPropertyStr(ctx, o, "handoffP99Us", JS_NewFloat64(ctx, smtp_pct(s->hs, s->hsn, 0.99)));
    JS_SetPropertyStr(ctx, o, "mainP50Us", JS_NewFloat64(ctx, smtp_pct(s->ms, s->msn, 0.5)));
    JS_SetPropertyStr(ctx, o, "mainP99Us", JS_NewFloat64(ctx, smtp_pct(s->ms, s->msn, 0.99)));
    return o;
}

static const JSCFunctionListEntry smtp_proto[] = {
    JS_CFUNC_DEF("send", 1, smtp_send),
    JS_CFUNC_DEF("quit", 0, smtp_quit),
    JS_CFUNC_DEF("stats", 0, smtp_stats),
};

static void smtp_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, smtp_class_id);
    smtp_t* s;
    smtp_msg_t* m;
    smtp_conn_t* c;
    if (!res || res->closed || !res->native)
        return;
    s = (smtp_t*)res->native;
    JS_MarkValue(rt, s->quit_resolve, mark_func);
    for (m = s->qh; m; m = m->next) {
        JS_MarkValue(rt, m->resolve, mark_func);
        JS_MarkValue(rt, m->reject, mark_func);
    }
    for (c = s->conns; c; c = c->next)
        if (c->cur) {
            JS_MarkValue(rt, c->cur->resolve, mark_func);
            JS_MarkValue(rt, c->cur->reject, mark_func);
        }
}

static const JSClassDef smtp_class = {
    "SmtpClient",
    .finalizer = smtp_finalizer,
    .gc_mark = smtp_gc_mark,
};

static int smtp_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &smtp_class_id, &smtp_class, smtp_proto, countof(smtp_proto), smtp_ctor, "SmtpClient") < 0)
        return -1;
    return ssrv_register(ctx, m);
}

int js_nat_init_smtp(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:smtp", smtp_register);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "SmtpClient");
    JS_AddModuleExport(ctx, m, "SmtpServer");
    return 0;
}
#endif
