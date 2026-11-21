#include "redis-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_REDIS)
#define RD_DEFAULT_MAXBULK (64u * 1024u * 1024u)
#define RD_DEFAULT_PENDING 4096
#define RD_CONNECT_TIMEOUT 10000

static const char* const rd_ctor_keys[] = {
    "tls",
    "ca",
    "host",
    "path",
    "username",
    "password",
    "port",
    "db",
    "binary",
    "bigint",
    "maxReplyBytes",
    "maxPending",
    "connectTimeoutMs",
    "commandTimeoutMs",
};

static JSClassID dyn_redis_class_id;

static void redis_tick(void* udata);

static JSValue redis_release_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

void redis_client_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_redis_t* r = (dyn_redis_t*)opaque;
    JSValue s;

    (void)ctx;
    if (!r || !r->self_held)
        return;
    s = r->self;
    r->self = JS_UNDEFINED;
    r->self_held = 0;
    JS_FreeValueRT(rt, s);
}

static void redis_hold(JSContext* ctx, JSValueConst obj, dyn_redis_t* r)
{
    if (r->self_held)
        return;
    r->self = JS_DupValue(ctx, obj);
    r->self_held = 1;
    JS_AddShutdownSweep(r->rt, redis_client_sweep, r);
}

void redis_release_if_idle(dyn_redis_t* r)
{
    JSValue s;

    if (!r->self_held || r->npending != 0 || r->nwait != 0)
        return;
    s = r->self;
    r->self = JS_UNDEFINED;
    r->self_held = 0;
    JS_RemoveShutdownSweep(r->rt, redis_client_sweep, r);
    if (JS_EnqueueJob(r->ctx, redis_release_job, 1,
            (JSValueConst*)&s)
        < 0) {
        r->self = s;
        r->self_held = 1;
        JS_AddShutdownSweep(r->rt, redis_client_sweep, r);
        return;
    }
    JS_FreeValue(r->ctx, s);
}

int redis_buf_reserve(uint8_t** p, size_t* cap, size_t need)
{
    size_t c = *cap;
    uint8_t* n;
    if (need <= c)
        return 0;
    if (c == 0)
        c = 512;
    while (c < need)
        c = c < (1u << 20) ? c * 2 : c + (c / 4);
    n = (uint8_t*)realloc(*p, c);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

void redis_secure_zero(void* p, size_t n)
{
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--)
        *v++ = 0;
}

void redis_settle(dyn_redis_t* r, dyn_redis_pending_t* p, int reject,
    JSValue v)
{
    JSValue fn = reject ? p->reject : p->resolve;
    if (JS_IsFunction(r->ctx, fn)) {
        JSValueConst a[1] = { v };
        r->cb_depth++;
        JSValue res = JS_Call(r->ctx, fn, JS_UNDEFINED, 1, a);
        r->cb_depth--;
        JS_FreeValue(r->ctx, res);
    }
    JS_FreeValue(r->ctx, v);
    pend_free(r->ctx, p);
    redis_release_if_idle(r);
}

JSValue redis_conn_error(JSContext* ctx, const char* msg)
{
    JSValue e = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, "CONNECTION"),
        JS_PROP_C_W_E);
    return e;
}

static int redis_write(dyn_redis_t* r, const uint8_t* b, size_t n)
{
    if (redis_buf_reserve(&r->obuf, &r->ocap, r->olen + n) < 0)
        return -1;
    memcpy(r->obuf + r->olen, b, n);
    r->olen += n;
    return 0;
}

int redis_arm(dyn_redis_t* r, dyn_redis_pending_t* p)
{
    if (p->bytes) {
        int wrc = redis_write(r, p->bytes, p->nbytes);
        pend_wipe_bytes(p);
        if (wrc < 0)
            return -1;
        free(p->bytes);
        p->bytes = NULL;
        p->nbytes = 0;
    }
    if (r->command_timeout_ms)
        p->deadline_ms = dyn_timer_now_ms() + r->command_timeout_ms;
    pend_push(&r->head, &r->tail, p);
    r->npending++;
    return 0;
}

int redis_send_internal(dyn_redis_t* r, int kind, int argc,
    const char* const* argv, const size_t* lens)
{
    dyn_redis_pending_t* p = pend_new(r->ctx);
    size_t need = dyn_resp_cmd_size(argc, argv, lens);
    uint8_t* b;

    if (!p)
        return -1;
    b = (uint8_t*)malloc(need);
    if (!b) {
        pend_free(r->ctx, p);
        return -1;
    }
    if (dyn_resp_cmd_encode(b, need, argc, argv, lens, NULL) < 0) {
        free(b);
        pend_free(r->ctx, p);
        return -1;
    }
    p->internal = kind;
    p->bytes = b;
    p->nbytes = need;
    return redis_arm(r, p);
}

void redis_sub_recount(dyn_redis_t* r)
{
    if (r->sub_guess < 0)
        r->sub_guess = 0;
    r->subscribed = r->sub_cp + r->sub_shard + r->sub_guess;
}

int redis_gone(dyn_redis_t* r)
{
    return r->closing || r->state == RD_ST_DEAD;
}

void redis_cb_exit(dyn_redis_t* r)
{
    if (r->cb_depth > 0)
        r->cb_depth--;
    if (r->cb_depth == 0 && r->closing)
        dyn_redis_teardown(r);
}

void redis_fail_all(dyn_redis_t* r, const char* msg)
{
    JSContext* ctx = r->ctx;
    dyn_redis_pending_t* p;

    if (r->state == RD_ST_DEAD)
        return;
    r->state = RD_ST_DEAD;
    if (r->fd >= 0) {
        dyn_aio_close(r->aio, r->fd);
        r->fd = -1;
    }
    while ((p = pend_pop(&r->head, &r->tail)) != NULL) {
        r->npending--;
        if (p->internal)
            pend_free(ctx, p);
        else
            redis_settle(r, p, 1, redis_conn_error(ctx, msg));
    }
    while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL) {
        r->nwait--;
        redis_settle(r, p, 1, redis_conn_error(ctx, msg));
    }
    redis_release_if_idle(r);
    if (JS_IsFunction(ctx, r->h_error)) {
        JSValue e = redis_conn_error(ctx, msg);
        r->cb_depth++;
        JSValueConst a[1] = { e };
        JSValue res = JS_Call(ctx, r->h_error, JS_UNDEFINED, 1, a);
        r->cb_depth--;
        if (JS_IsException(res))
            dyn_exc_drop(ctx);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, e);
    }
    if (r->hooked) {
        dyn_net_off_drain(r);
        r->hooked = 0;
    }
    if (r->aio && !r->released) {
        r->released = 1;
        dyn_net_reactor_release(r->ctx);
    }
}

static void redis_tick_inner(void* udata)
{
    dyn_redis_t* r = (dyn_redis_t*)udata;
    uint64_t now;

    if (redis_gone(r))
        return;
    now = dyn_timer_now_ms();
    if (r->connect_deadline_ms && now >= r->connect_deadline_ms) {
        redis_fail_all(r, "Redis: connect timed out");
        return;
    }
    if (r->head && r->head->deadline_ms && now >= r->head->deadline_ms) {
        redis_fail_all(r, "Redis: command timed out");
        return;
    }
}

static void redis_tick(void* udata)
{
    dyn_redis_t* r = (dyn_redis_t*)udata;

    r->cb_depth++;
    redis_tick_inner(udata);
    redis_cb_exit(r);
}

static dyn_redis_t* redis_this(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_redis_t*)dyn_res_native(ctx, this_val, dyn_redis_class_id);
}

const char* const RD_SUB[] = { "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE" };

const char* const RD_UNSUB[] = { "UNSUBSCRIBE", "PUNSUBSCRIBE",
    "SUNSUBSCRIBE" };

int redis_verb_is(const char* cmd, size_t len, const char* const* tab,
    size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (len == strlen(tab[i]) && strncasecmp(cmd, tab[i], len) == 0)
            return 1;
    return 0;
}

static int redis_changes_reply_stream(const redis_args_t* a)
{
    static const char* const hello[] = { "HELLO" };
    static const char* const client[] = { "CLIENT" };
    static const char* const reply[] = { "REPLY" };
    if (a->n < 1)
        return 0;
    if (redis_verb_is(a->argv[0], a->lens[0], hello, 1))
        return 1;
    return a->n >= 2 && redis_verb_is(a->argv[0], a->lens[0], client, 1)
        && redis_verb_is(a->argv[1], a->lens[1], reply, 1);
}

static int redis_allowed_while_subscribed(const char* cmd, size_t len)
{
    static const char* const ok[] = { "PING", "QUIT", "RESET" };
    return redis_verb_is(cmd, len, ok, countof(ok)) || redis_verb_is(cmd, len, RD_SUB, countof(RD_SUB)) || redis_verb_is(cmd, len, RD_UNSUB, countof(RD_UNSUB));
}

static JSValue dyn_redis_command(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    redis_args_t a;
    dyn_redis_t* r;
    JSValue ret;
    int want;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "command: a command name is required");
    if (args_build(ctx, &a, argc, argv) < 0)
        return JS_EXCEPTION;
    r = redis_this(ctx, this_val);
    if (!r) {
        args_free(ctx, &a);
        return JS_EXCEPTION;
    }
    if (r->state == RD_ST_DEAD) {
        args_free(ctx, &a);
        return JS_ThrowInternalError(ctx, "Redis: the connection is closed");
    }
    if (r->state == RD_ST_READY && r->proto == 2 && r->subscribed > 0 && !redis_allowed_while_subscribed(a.argv[0], a.lens[0])) {
        JSValue e = JS_ThrowTypeError(ctx,
            "Redis: '%s' is not allowed while subscribed on RESP2; only PING, "
            "QUIT, RESET and the (un)subscribe commands are",
            a.argv[0]);
        args_free(ctx, &a);
        return e;
    }
    if (redis_changes_reply_stream(&a)) {
        args_free(ctx, &a);
        return JS_ThrowTypeError(ctx,
            "Redis: HELLO and CLIENT REPLY change how many replies the server "
            "sends, so later replies could not be matched to their commands; "
            "the client negotiates the protocol itself when it connects");
    }
    want = redis_reply_count(a.argv[0], a.lens[0], a.n);
    if (want < 0) {
        args_free(ctx, &a);
        return JS_ThrowTypeError(ctx,
            "Redis: unsubscribe by name -- with no channel the reply count "
            "depends on server state and cannot be matched to this command");
    }
    ret = redis_enqueue(ctx, r, a.n, a.argv, a.lens, want,
        want > 1 ? JS_NewArray(ctx) : JS_UNDEFINED);
    if (!JS_IsException(ret))
        redis_hold(ctx, this_val, r);
    args_free(ctx, &a);
    return ret;
}

static JSValue dyn_redis_pipeline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_redis_t* r;
    JSValue acc, ret;
    uint32_t n, i;
    uint8_t* all = NULL;
    size_t alen = 0, acap = 0;
    int replies = 0;
    char* bad_cmd = NULL;
    dyn_redis_pending_t* p;
    JSValue funcs[2], promise;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "pipeline: expects an array of commands");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToUint32(ctx, &n, lv) < 0) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n == 0)
        return JS_ThrowTypeError(ctx, "pipeline: at least one command");

    for (i = 0; i < n; i++) {
        JSValue cmd = JS_GetPropertyUint32(ctx, argv[0], i);
        redis_args_t a;
        uint32_t m, k;
        JSValueConst* parts;
        size_t need;
        if (JS_IsException(cmd)) {
            free(all);
            free(bad_cmd);
            return JS_EXCEPTION;
        }
        if (!JS_IsArray(ctx, cmd)) {
            JS_FreeValue(ctx, cmd);
            free(all);
            free(bad_cmd);
            return JS_ThrowTypeError(ctx, "pipeline: element %u is not an array", i);
        }
        {
            JSValue lv = JS_GetPropertyStr(ctx, cmd, "length");
            if (JS_ToUint32(ctx, &m, lv) < 0) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, cmd);
                free(all);
                free(bad_cmd);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
        }
        if (m == 0) {
            JS_FreeValue(ctx, cmd);
            free(all);
            free(bad_cmd);
            return JS_ThrowTypeError(ctx, "pipeline: element %u is empty", i);
        }
        parts = (JSValueConst*)calloc(m, sizeof(*parts));
        if (!parts) {
            JS_FreeValue(ctx, cmd);
            free(all);
            free(bad_cmd);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (k = 0; k < m; k++) {
            parts[k] = JS_GetPropertyUint32(ctx, cmd, k);
            if (JS_IsException(parts[k]))
                break;
        }
        if (k < m || args_build(ctx, &a, (int)m, parts) < 0) {
            for (k = 0; k < m; k++)
                JS_FreeValue(ctx, parts[k]);
            free(parts);
            JS_FreeValue(ctx, cmd);
            free(all);
            free(bad_cmd);
            return JS_EXCEPTION;
        }
        if (!bad_cmd && a.n > 0
            && !redis_allowed_while_subscribed(a.argv[0], a.lens[0])) {
            bad_cmd = (char*)malloc(a.lens[0] + 1);
            if (bad_cmd) {
                memcpy(bad_cmd, a.argv[0], a.lens[0]);
                bad_cmd[a.lens[0]] = '\0';
            }
        }
        {
            int rc1 = redis_changes_reply_stream(&a)
                    || redis_verb_is(a.argv[0], a.lens[0], RD_SUB, countof(RD_SUB))
                    || redis_verb_is(a.argv[0], a.lens[0], RD_UNSUB, countof(RD_UNSUB))
                ? -1
                : redis_reply_count(a.argv[0], a.lens[0], (int)m);
            if (rc1 < 0) {
                args_free(ctx, &a);
                for (k = 0; k < m; k++)
                    JS_FreeValue(ctx, parts[k]);
                free(parts);
                JS_FreeValue(ctx, cmd);
                free(all);
                free(bad_cmd);
                return JS_ThrowTypeError(ctx,
                    "Redis: pipeline element %u has a reply count that cannot "
                    "be matched (subscribe verbs, HELLO, CLIENT REPLY); use "
                    "command()",
                    i);
            }
            replies += rc1;
        }
        need = dyn_resp_cmd_size((int)m, a.argv, a.lens);
        if (redis_buf_reserve(&all, &acap, alen + need) < 0 || dyn_resp_cmd_encode(all + alen, acap - alen, (int)m, a.argv, a.lens, NULL) < 0) {
            args_free(ctx, &a);
            for (k = 0; k < m; k++)
                JS_FreeValue(ctx, parts[k]);
            free(parts);
            JS_FreeValue(ctx, cmd);
            free(all);
            free(bad_cmd);
            return JS_ThrowOutOfMemory(ctx);
        }
        alen += need;
        args_free(ctx, &a);
        for (k = 0; k < m; k++)
            JS_FreeValue(ctx, parts[k]);
        free(parts);
        JS_FreeValue(ctx, cmd);
    }

    r = redis_this(ctx, this_val);
    if (!r) {
        free(all);
        free(bad_cmd);
        return JS_EXCEPTION;
    }
    if (r->state == RD_ST_DEAD) {
        free(all);
        free(bad_cmd);
        return JS_ThrowInternalError(ctx, "Redis: the connection is closed");
    }
    if (bad_cmd && r->state == RD_ST_READY && r->proto == 2 && r->subscribed > 0) {
        JSValue e = JS_ThrowTypeError(ctx,
            "Redis: '%s' is not allowed while subscribed on RESP2; only PING, "
            "QUIT, RESET and the (un)subscribe commands are",
            bad_cmd);
        free(all);
        free(bad_cmd);
        return e;
    }
    free(bad_cmd);
    if (r->npending + r->nwait >= r->maxpending) {
        free(all);
        return JS_ThrowInternalError(ctx, "Redis: too many commands in flight");
    }
    acc = JS_NewArray(ctx);
    if (JS_IsException(acc)) {
        free(all);
        return acc;
    }
    p = pend_new(ctx);
    if (!p) {
        free(all);
        JS_FreeValue(ctx, acc);
        return JS_ThrowOutOfMemory(ctx);
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(all);
        pend_free(ctx, p);
        JS_FreeValue(ctx, acc);
        return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];
    p->bytes = all;
    p->nbytes = alen;
    p->want = replies;
    p->acc = acc;
    if (r->state == RD_ST_READY) {
        if (redis_arm(r, p) < 0) {
            pend_free(ctx, p);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        redis_flush_soon(r);
    } else {
        pend_push(&r->wq_head, &r->wq_tail, p);
        r->nwait++;
    }
    redis_hold(ctx, this_val, r);
    ret = promise;
    return ret;
}

static JSValue dyn_redis_get_protocol(JSContext* ctx, JSValueConst this_val)
{
    dyn_redis_t* r = redis_this(ctx, this_val);
    return r ? JS_NewInt32(ctx, r->proto) : JS_EXCEPTION;
}

static JSValue dyn_redis_get_ready(JSContext* ctx, JSValueConst this_val)
{
    dyn_redis_t* r = redis_this(ctx, this_val);
    return r ? JS_NewBool(ctx, r->state == RD_ST_READY) : JS_EXCEPTION;
}

static JSValue dyn_redis_get_pending(JSContext* ctx, JSValueConst this_val)
{
    dyn_redis_t* r = redis_this(ctx, this_val);
    return r ? JS_NewInt32(ctx, r->npending + r->nwait) : JS_EXCEPTION;
}

static JSValue dyn_redis_on(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_redis_t* r;
    const char* ev;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, handler)");
    ev = JS_ToCString(ctx, argv[0]);
    if (!ev)
        return JS_EXCEPTION;
    r = redis_this(ctx, this_val);
    if (!r) {
        JS_FreeCString(ctx, ev);
        return JS_EXCEPTION;
    }
    if (strcmp(ev, "push") == 0 || strcmp(ev, "message") == 0) {
        JS_FreeValue(ctx, r->h_push);
        r->h_push = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "error") == 0) {
        JS_FreeValue(ctx, r->h_error);
        r->h_error = JS_DupValue(ctx, argv[1]);
    } else {
        JS_FreeCString(ctx, ev);
        return JS_ThrowRangeError(ctx, "on: unknown event; want 'push' or 'error'");
    }
    JS_FreeCString(ctx, ev);
    return JS_DupValue(ctx, this_val);
}

static void dyn_redis_dispose(void* native)
{
    dyn_redis_t* r = (dyn_redis_t*)native;

    if (!r)
        return;
    if (r->cb_depth) {
        r->closing = 1;
        return;
    }
    dyn_redis_teardown(r);
}

_Thread_local int redis_in_final;

static void dyn_redis_finalizer(JSRuntime* rt, JSValue val)
{
    redis_in_final++;
    dyn_res_finalizer(rt, val);
    redis_in_final--;
}

static int opt_str(JSContext* ctx, JSValueConst o, const char* k, char** out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char* s;
    size_t sl = 0;
    *out = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    s = JS_ToCStringLen(ctx, &sl, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (memchr(s, 0, sl)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "Redis: option '%s' contains a NUL character", k);
        return -1;
    }
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int opt_int(JSContext* ctx, JSValueConst o, const char* k, int dflt, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n;
    int rc;
    *out = dflt;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    rc = JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (rc < 0)
        return -1;
    *out = (int)n;
    return 0;
}

static int opt_bool(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    *out = dyn_get_truthy(ctx, o, k);
    return *out < 0 ? -1 : 0;
}

static int rd_ctor_nums(JSContext* ctx, JSValueConst opt, dyn_redis_t* r)
{
    int mb, mp, ct, cmdt;
    if (opt_int(ctx, opt, "port", 6379, &r->port_i) < 0 || opt_int(ctx, opt, "db", 0, &r->db) < 0
        || opt_bool(ctx, opt, "binary", &r->binary) < 0 || opt_bool(ctx, opt, "bigint", &r->bigint) < 0
        || opt_int(ctx, opt, "maxReplyBytes", 0, &mb) < 0 || opt_int(ctx, opt, "maxPending", 0, &mp) < 0
        || opt_int(ctx, opt, "connectTimeoutMs", RD_CONNECT_TIMEOUT, &ct) < 0
        || opt_int(ctx, opt, "commandTimeoutMs", 0, &cmdt) < 0)
        return -1;
    if (r->port_i >= 0 && r->port_i <= 65535)
        r->port = (uint16_t)r->port_i;
    if (mb > 0)
        r->maxbulk = (size_t)mb;
    if (mp > 0)
        r->maxpending = mp;
    if (cmdt > 0)
        r->command_timeout_ms = (uint64_t)cmdt;
    if (ct > 0)
        r->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)ct;
    return 0;
}

static JSValue dyn_redis_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_redis_t* r;
    JSValueConst opt = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue res;

    if (argc > 0 && !JS_IsObject(opt))
        return JS_ThrowTypeError(ctx, "Redis: expects an options object");

    r = (dyn_redis_t*)calloc(1, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->ctx = ctx;
    r->rt = JS_GetRuntime(ctx);
    r->fd = -1;
    r->proto = 2;
    r->state = RD_ST_CONNECTING;
    r->h_push = r->h_error = JS_UNDEFINED;
    r->self = JS_UNDEFINED;
    dyn_resp_scan_init(&r->rscan);
    r->maxbulk = RD_DEFAULT_MAXBULK;
    r->maxpending = RD_DEFAULT_PENDING;

    if (JS_IsObject(opt)) {
        int tls_b, opt_bad, sbad = 0;
        if (dyn_opts_strict(ctx, opt, rd_ctor_keys, 14)) {
            free(r);
            return JS_EXCEPTION;
        }
        if (opt_bool(ctx, opt, "tls", &tls_b) < 0) {
            free(r);
            return JS_EXCEPTION;
        }
#ifdef CONFIG_TLS
        if (tls_b)
            r->use_tls = 1;
        sbad = opt_str(ctx, opt, "ca", &r->tls_ca);
#else
        if (tls_b) {
            free(r);
            return dyn_tls_missing(ctx, "Redis", "tls");
        }
#endif
        sbad = sbad || opt_str(ctx, opt, "host", &r->host) || opt_str(ctx, opt, "path", &r->path)
            || opt_str(ctx, opt, "username", &r->user) || opt_str(ctx, opt, "password", &r->pass);
        if (!sbad)
            sbad = rd_ctor_nums(ctx, opt, r);
        opt_bad = sbad || JS_HasException(ctx);
        if (opt_bad || r->db < 0 || r->db > 255) {
#ifdef CONFIG_TLS
            if (r->tls_ctx)
                dyn_tls_ctx_free(r->tls_ctx);
            free(r->tls_ca);
#endif
            free(r->host);
            free(r->path);
            free(r->user);
            if (r->pass) {
                memset(r->pass, 0, strlen(r->pass));
                free(r->pass);
            }
            free(r);
            return opt_bad ? JS_EXCEPTION
                           : JS_ThrowRangeError(ctx, "Redis: db must be 0..255");
        }
        if ((r->port_i < 1 || r->port_i > 65535) && !r->path) {
#ifdef CONFIG_TLS
            if (r->tls_ctx)
                dyn_tls_ctx_free(r->tls_ctx);
            free(r->tls_ca);
#endif
            free(r->host);
            free(r->path);
            free(r->user);
            if (r->pass) {
                memset(r->pass, 0, strlen(r->pass));
                free(r->pass);
            }
            free(r);
            return JS_ThrowRangeError(ctx, "Redis: port must be 1..65535");
        }
        if (JS_HasException(ctx)) {
#ifdef CONFIG_TLS
            if (r->tls_ctx)
                dyn_tls_ctx_free(r->tls_ctx);
            free(r->tls_ca);
#endif
            free(r->host);
            free(r->path);
            free(r->user);
            if (r->pass) {
                memset(r->pass, 0, strlen(r->pass));
                free(r->pass);
            }
            free(r);
            return JS_EXCEPTION;
        }
    } else {
        r->port = 6379;
        r->connect_deadline_ms = dyn_timer_now_ms() + RD_CONNECT_TIMEOUT;
    }

    r->aio = dyn_net_reactor_acquire(ctx);
    if (!r->aio) {
#ifdef CONFIG_TLS
        if (r->tls_ctx)
            dyn_tls_ctx_free(r->tls_ctx);
        free(r->tls_ca);
#endif
        free(r->host);
        free(r->path);
        free(r->user);
        if (r->pass) {
            memset(r->pass, 0, strlen(r->pass));
            free(r->pass);
        }
        free(r);
        return dyn_net_throw_no_reactor(ctx, "Redis");
    }

    r->fd = r->path
        ? dyn_aio_unix_connect(r->aio, r->path, redis_on_connect, r)
        : dyn_aio_connect(r->aio, r->host ? r->host : "127.0.0.1",
              r->port, redis_on_connect, r);
    if (r->fd < 0) {
        JSValue e = JS_ThrowInternalError(ctx, "Redis: connect: %s",
            strerror(errno));
        dyn_redis_dispose(r);
        return e;
    }

#ifdef CONFIG_TLS
    if (r->use_tls)
        goto after_hello;
#endif
    if (redis_queue_hello(r) < 0) {
        dyn_redis_dispose(r);
        return JS_ThrowOutOfMemory(ctx);
    }
#ifdef CONFIG_TLS
after_hello:;
#endif

    if (dyn_net_on_drain(redis_tick, r) == 0)
        r->hooked = 1;

    res = dyn_res_wrap(ctx, new_target, dyn_redis_class_id, r, dyn_redis_dispose);
    return res;
}

static const JSCFunctionListEntry dyn_redis_proto[] = {
    JS_CFUNC_DEF("command", 1, dyn_redis_command),
    JS_CFUNC_DEF("pipeline", 1, dyn_redis_pipeline),
    JS_CFUNC_DEF("on", 2, dyn_redis_on),
    JS_CGETSET_DEF("protocol", dyn_redis_get_protocol, NULL),
    JS_CGETSET_DEF("ready", dyn_redis_get_ready, NULL),
    JS_CGETSET_DEF("pending", dyn_redis_get_pending, NULL),
};

static void dyn_redis_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_redis_class_id);
    dyn_redis_t* r;
    dyn_redis_pending_t* p;

    if (!res || res->closed || !res->native)
        return;
    r = (dyn_redis_t*)res->native;
    JS_MarkValue(rt, r->h_push, mark_func);
    JS_MarkValue(rt, r->h_error, mark_func);
    for (p = r->head; p; p = p->next) {
        JS_MarkValue(rt, p->resolve, mark_func);
        JS_MarkValue(rt, p->reject, mark_func);
        JS_MarkValue(rt, p->acc, mark_func);
    }
    for (p = r->wq_head; p; p = p->next) {
        JS_MarkValue(rt, p->resolve, mark_func);
        JS_MarkValue(rt, p->reject, mark_func);
        JS_MarkValue(rt, p->acc, mark_func);
    }
}

static const JSClassDef dyn_redis_class = {
    "Redis",
    .finalizer = dyn_redis_finalizer,
    .gc_mark = dyn_redis_gc_mark,
};

static int dyn_redis_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &dyn_redis_class_id, &dyn_redis_class,
        dyn_redis_proto, countof(dyn_redis_proto),
        dyn_redis_ctor, "Redis");
}

static void dyn_redis_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "Redis");
}

int js_nat_init_redis(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:redis", dyn_redis_register);
    if (!m)
        return -1;
    dyn_redis_add_exports(ctx, m);
    return 0;
}

#endif
