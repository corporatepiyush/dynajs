#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <openssl/crypto.h>

static JSClassID ssh_client_cid;
static JSClassID ssh_exec_cid;
static JSClassID ssh_server_cid;
static JSClassID ssh_session_cid;
static _Thread_local int ssh_no_js;

static const char* cipher_name(int k)
{
    return k == SSH_CIPHER_CHACHAPOLY ? "chacha20-poly1305@openssh.com" : k == SSH_CIPHER_AES256GCM ? "aes256-gcm@openssh.com" : "none";
}

static JSValue ssh_error(JSContext* ctx, const char* msg)
{
    JSValue e = JS_NewError(ctx);
    char buf[600];
    if (JS_IsException(e))
        return e;
    snprintf(buf, sizeof buf, "SSH: %s", msg ? msg : "the connection closed");
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, buf),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return e;
}

static void call1(JSContext* ctx, JSValueConst fn, JSValueConst arg)
{
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

static void free_val(JSRuntime* rt, JSValue* v)
{
    JSValue x = *v;
    *v = JS_UNDEFINED;
    JS_FreeValueRT(rt, x);
}

static int ctl_call(JSContext* ctx, JSValueConst ctl, const char* method, int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetPropertyStr(ctx, ctl, method), r;
    int rc = 0;
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    r = JS_Call(ctx, fn, ctl, argc, argv);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        rc = -1;
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
    return rc;
}

static uint64_t ctl_queued(JSContext* ctx, JSValueConst ctl, uint64_t hwm)
{
    JSValue d = JS_GetPropertyStr(ctx, ctl, "desiredSize");
    double v = 0;
    uint64_t q = 0;
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (JS_IsNumber(d) && JS_ToFloat64(ctx, &v, d) == 0 && v == v) {
        double qd = (double)hwm - v;
        q = qd <= 0 ? 0 : qd >= 9007199254740992.0 ? (uint64_t)1 << 53 : (uint64_t)qd;
    }
    JS_FreeValue(ctx, d);
    return q;
}

void ssh_chan_unref(JSRuntime* rt, ssh_chan_t* ch)
{
    int i;
    if (--ch->refs > 0)
        return;
    if (ch->ops && ch->ops->release)
        ch->ops->release(rt, ch);
    free_val(rt, &ch->ctl[0]);
    free_val(rt, &ch->ctl[1]);
    free_val(rt, &ch->open_res);
    free_val(rt, &ch->open_rej);
    free_val(rt, &ch->exit_res);
    for (i = 0; i < ch->nww; i++)
        free_val(rt, &ch->ww[i].resolve);
    free(ch->ww);
    free(ch->cmd);
    free(ch->exit_signal);
    free(ch->exit_msg);
    free(ch->term);
    sshb_free(&ch->inq);
    sshb_free(&ch->errq);
    sshb_free(&ch->env);
    sshb_free(&ch->modes);
    sshb_free(&ch->oextra);
    free(ch);
}

static void ssh_conn_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);
static void srv_free(ssh_srv_t* s);

static void conn_free(ssh_conn_t* c)
{
    if (c->server && c->srv) {
        ssh_srv_t* s = c->srv;
        if (c->sprev)
            c->sprev->snext = c->snext;
        else
            s->conns = c->snext;
        if (c->snext)
            c->snext->sprev = c->sprev;
        s->nconns--;
        c->srv = NULL;
        if (s->dead && s->nconns == 0)
            srv_free(s);
    }
    ssh_fwd_conn_free(c);
    ssh_auth_drop(c);
    free(c->agent_path);
    free_val(c->rt, &c->via_obj);
    free(c->force_cmd);
    sshb_free(&c->ucert);
    if (c->swept) {
        c->swept = 0;
        JS_RemoveShutdownSweep(c->rt, ssh_conn_sweep, c);
    }
    free(c->ch);
    sshb_free(&c->rbuf);
    sshb_free(&c->obuf);
    sshb_free(&c->msg);
    sshb_free(&c->pend);
    sshb_free(&c->ic);
    sshb_free(&c->is);
    sshb_free(&c->hk0);
    ssh_aead_free(&c->ain);
    ssh_aead_free(&c->aout);
    ssh_conn_free_kex(c);
    EVP_PKEY_free(c->ukey);
    free(c->err);
    free(c->host);
    free(c->user);
    free(c->known_hosts);
    free(c->banner);
    free(c->server_sig_algs);
    OPENSSL_cleanse(c, sizeof *c);
    free(c);
}

static void conn_exit(ssh_conn_t* c)
{
    if (--c->busy == 0 && c->closing)
        conn_free(c);
}

void ssh_conn_enter(ssh_conn_t* c)
{
    c->busy++;
}

void ssh_conn_leave(ssh_conn_t* c)
{
    conn_exit(c);
}

JSValue ssh_js_error(JSContext* ctx, const char* msg)
{
    return ssh_error(ctx, msg);
}

static void ssh_conn_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    ssh_conn_t* c = (ssh_conn_t*)opaque;
    (void)ctx;
    c->swept = 0;
    c->busy++;
    ssh_no_js++;
    for (;;) {
        ssh_chan_t* hit = NULL;
        JSValue v;
        int i;
        for (i = 0; i < c->nch && !hit; i++)
            if (c->ch[i] && c->ch[i]->js_held)
                hit = c->ch[i];
        if (!hit)
            break;
        hit->js_held = 0;
        v = hit->jsobj;
        hit->jsobj = JS_UNDEFINED;
        JS_FreeValueRT(rt, v);
    }
    {
        ssh_fwd_t* f;
        for (f = c->fwds; f; f = f->next) {
            free_val(rt, &f->res);
            free_val(rt, &f->rej);
        }
        for (;;) {
            JSValue v;
            for (f = c->fwds; f && JS_IsUndefined(f->obj); f = f->next)
                ;
            if (!f)
                break;
            v = f->obj;
            f->obj = JS_UNDEFINED;
            JS_FreeValueRT(rt, v);
        }
    }
    free_val(rt, &c->via_obj);
    free_val(rt, &c->conn_res);
    free_val(rt, &c->conn_rej);
    ssh_no_js--;
    conn_exit(c);
}

static void ssh_dispose(void* native)
{
    ssh_conn_t* c = (ssh_conn_t*)native;
    if (!c)
        return;
    c->pinned = 0;
    c->busy++;
    ssh_fail(c, SSH_DISC_BY_APPLICATION, "the client was closed");
    c->closing = 1;
    conn_exit(c);
}

void ssh_js_connected(ssh_conn_t* c)
{
    JSContext* ctx = c->ctx;
    if (!JS_IsUndefined(c->conn_res) && !ssh_no_js)
        call1(ctx, c->conn_res, c->self_weak);
    free_val(c->rt, &c->conn_res);
    free_val(c->rt, &c->conn_rej);
}

void ssh_js_died(ssh_conn_t* c)
{
    ssh_fwd_conn_dead(c);
    ssh_auth_drop(c);
    if (c->fd >= 0) {
        dyn_aio_close(c->aio, c->fd);
        c->fd = -1;
    }
    if (c->server) {
        c->closing = 1;
        return;
    }
    if (c->hooked) {
        dyn_net_off_drain(c);
        c->hooked = 0;
    }
    if (c->aio && !c->released) {
        c->released = 1;
        dyn_net_reactor_release_rt(c->rt);
    }
    if (!JS_IsUndefined(c->conn_rej) && !ssh_no_js) {
        JSValue e = ssh_error(c->ctx, c->err);
        call1(c->ctx, c->conn_rej, e);
        JS_FreeValue(c->ctx, e);
    }
    free_val(c->rt, &c->conn_res);
    free_val(c->rt, &c->conn_rej);
    free_val(c->rt, &c->via_obj);
    if (c->pinned) {
        c->pinned = 0;
        dyn_res_unpin(c->rt, c->self_weak, ssh_client_cid);
    }
}

void ssh_js_chan_opened(ssh_chan_t* ch, const char* err)
{
    ssh_conn_t* c = ch->conn;
    JSRuntime* rt = c ? c->rt : NULL;
    if (ch->ops) {
        if (c && ch->ops->opened)
            ch->ops->opened(ch, err);
        return;
    }
    if (!c || JS_IsUndefined(ch->open_res))
        return;
    if (!ssh_no_js) {
        if (err) {
            JSValue e = ssh_error(c->ctx, err);
            call1(c->ctx, ch->open_rej, e);
            JS_FreeValue(c->ctx, e);
        } else
            call1(c->ctx, ch->open_res, ch->jsobj);
    }
    free_val(rt, &ch->open_res);
    free_val(rt, &ch->open_rej);
}

void ssh_js_chan_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n)
{
    ssh_conn_t* c = ch->conn;
    JSContext* ctx;
    JSValue u8;
    if (ch->ops) {
        if (c && ch->ops->data)
            ch->ops->data(ch, which, p, n);
        return;
    }
    if (!c || ch->js_gone || ch->rs_done[which] || JS_IsUndefined(ch->ctl[which]) || ssh_no_js)
        return;
    ctx = c->ctx;
    u8 = dyn_nat_u8_copy(ctx, p, n);
    if (JS_IsException(u8)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory delivering channel data");
        return;
    }
    if (ctl_call(ctx, ch->ctl[which], "enqueue", 1, (JSValueConst*)&u8) < 0) {
        ch->rs_done[which] = 1;
        ch->queued[which] = 0;
        free_val(c->rt, &ch->ctl[which]);
    } else if (!JS_IsUndefined(ch->ctl[which]))
        ch->queued[which] = ctl_queued(ctx, ch->ctl[which], ch->win_size);
    JS_FreeValue(ctx, u8);
}

static void close_streams(ssh_chan_t* ch, JSContext* ctx, JSRuntime* rt, const char* err)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (JS_IsUndefined(ch->ctl[i]))
            continue;
        if (!ssh_no_js && !ch->rs_done[i]) {
            if (err) {
                JSValue e = ssh_error(ctx, err);
                ctl_call(ctx, ch->ctl[i], "error", 1, (JSValueConst*)&e);
                JS_FreeValue(ctx, e);
            } else
                ctl_call(ctx, ch->ctl[i], "close", 0, NULL);
        }
        ch->rs_done[i] = 1;
        ch->queued[i] = 0;
        free_val(rt, &ch->ctl[i]);
    }
}

void ssh_js_chan_eof(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c)
        return;
    if (ch->ops && ch->ops->eof)
        ch->ops->eof(ch);
    if (!ch->conn)
        return;
    close_streams(ch, c->ctx, c->rt, NULL);
}

void ssh_js_chan_written(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    int i, k = 0;
    if (!c)
        return;
    if (ch->ops && ch->ops->written)
        ch->ops->written(ch);
    if (!ch->conn)
        return;
    for (i = 0; i < ch->nww; i++) {
        if (ch->ww[i].mark <= (ch->ww[i].which ? ch->err_sent : ch->in_sent)) {
            if (!ssh_no_js) {
                JSValue fn = JS_GetPropertyStr(c->ctx, ch->ww[i].resolve, "resolve");
                if (JS_IsFunction(c->ctx, fn))
                    call1(c->ctx, fn, JS_UNDEFINED);
                JS_FreeValue(c->ctx, fn);
            }
            free_val(c->rt, &ch->ww[i].resolve);
        } else
            ch->ww[k++] = ch->ww[i];
    }
    ch->nww = k;
}


static JSValue exec_result(JSContext* ctx, ssh_chan_t* ch, const char* err)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "exitCode", ch->has_status == 1 ? JS_NewInt32(ctx, ch->exit_code) : JS_NULL);
    JS_SetPropertyStr(ctx, o, "signal", ch->has_status == 2 && ch->exit_signal ? JS_NewString(ctx, ch->exit_signal) : JS_NULL);
    JS_SetPropertyStr(ctx, o, "coreDumped", JS_NewBool(ctx, ch->has_status == 2 && ch->exit_core));
    JS_SetPropertyStr(ctx, o, "message", JS_NewString(ctx, ch->has_status == 2 && ch->exit_msg ? ch->exit_msg : ""));
    JS_SetPropertyStr(ctx, o, "error", err ? JS_NewString(ctx, err) : JS_NULL);
    return o;
}

static void chan_closed_impl(ssh_chan_t* ch, JSContext* ctx, JSRuntime* rt, const char* err)
{
    int i;
    JSValue self;
    if (!JS_IsUndefined(ch->open_res)) {
        if (!ssh_no_js) {
            JSValue e = ssh_error(ctx, err ? err : "the channel closed before it opened");
            call1(ctx, ch->open_rej, e);
            JS_FreeValue(ctx, e);
        }
        free_val(rt, &ch->open_res);
        free_val(rt, &ch->open_rej);
    }
    close_streams(ch, ctx, rt, err);
    if (!JS_IsUndefined(ch->exit_res)) {
        if (!ssh_no_js) {
            JSValue r = exec_result(ctx, ch, err);
            call1(ctx, ch->exit_res, r);
            JS_FreeValue(ctx, r);
        }
        free_val(rt, &ch->exit_res);
    }
    for (i = 0; i < ch->nww; i++) {
        if (!ssh_no_js) {
            JSValue e = ssh_error(ctx, err ? err : "the channel closed before the data was sent");
            JSValue rej = JS_GetPropertyStr(ctx, ch->ww[i].resolve, "reject");
            if (JS_IsFunction(ctx, rej))
                call1(ctx, rej, e);
            JS_FreeValue(ctx, rej);
            JS_FreeValue(ctx, e);
        }
        free_val(rt, &ch->ww[i].resolve);
    }
    ch->nww = 0;
    if (ch->js_held) {
        ch->js_held = 0;
        self = ch->jsobj;
        ch->jsobj = JS_UNDEFINED;
        JS_FreeValueRT(rt, self);
    }
}

void ssh_js_chan_closed(ssh_chan_t* ch, const char* err)
{
    ssh_conn_t* c = ch->conn;
    if (!c)
        return;
    if (ch->ops && ch->ops->closed)
        ch->ops->closed(ch, err);
    chan_closed_impl(ch, c->ctx, c->rt, err);
}

static ssh_chan_t* exec_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_chan_t*)JS_GetOpaque2(ctx, v, ssh_exec_cid);
}

static ssh_chan_t* chan_of(JSValueConst v)
{
    ssh_chan_t* ch = (ssh_chan_t*)JS_GetOpaque(v, ssh_exec_cid);
    return ch ? ch : (ssh_chan_t*)JS_GetOpaque(v, ssh_session_cid);
}

static JSValue rs_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    ssh_chan_t* ch = chan_of(data[0]);
    (void)this_val;
    if (ch && argc > 0 && magic >= 0 && magic < 2) {
        free_val(JS_GetRuntime(ctx), &ch->ctl[magic]);
        ch->ctl[magic] = JS_DupValue(ctx, argv[0]);
    }
    return JS_UNDEFINED;
}

void ssh_chan_kick(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c)
        return;
    c->busy++;
    ssh_chan_window(ch);
    ssh_chan_pump(ch);
    ssh_flush(c);
    conn_exit(c);
}

static JSValue rs_pull(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    ssh_chan_t* ch = chan_of(data[0]);
    (void)this_val;
    if (ch && argc > 0 && magic >= 0 && magic < 2 && !ch->rs_done[magic]) {
        ch->queued[magic] = ctl_queued(ctx, argv[0], ch->win_size);
        if (ch->conn && !ch->conn->busy)
            ssh_chan_kick(ch);
    }
    return JS_UNDEFINED;
}

static JSValue rs_cancel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    ssh_chan_t* ch = chan_of(data[0]);
    (void)this_val;
    (void)argc;
    (void)argv;
    if (ch && magic >= 0 && magic < 2) {
        ch->rs_done[magic] = 1;
        ch->queued[magic] = 0;
        free_val(JS_GetRuntime(ctx), &ch->ctl[magic]);
        if (ch->conn && !ch->conn->busy)
            ssh_chan_kick(ch);
    }
    return JS_UNDEFINED;
}

static JSValue rs_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_NewInt32(ctx, 1);
    return JS_GetPropertyStr(ctx, argv[0], "byteLength");
}

static JSValue make_rs(JSContext* ctx, JSValueConst obj, int which, uint32_t hwm)
{
    JSValue g = JS_GetGlobalObject(ctx), rs_ctor, src, strat, r = JS_EXCEPTION;
    JSValueConst args[2];
    rs_ctor = JS_GetPropertyStr(ctx, g, "ReadableStream");
    JS_FreeValue(ctx, g);
    if (!JS_IsFunction(ctx, rs_ctor)) {
        JS_FreeValue(ctx, rs_ctor);
        return JS_ThrowTypeError(ctx, "SSH: ReadableStream is not available");
    }
    src = JS_NewObject(ctx);
    strat = JS_NewObject(ctx);
    if (JS_IsException(src) || JS_IsException(strat))
        goto out;
    JS_SetPropertyStr(ctx, src, "start", JS_NewCFunctionData(ctx, rs_start, 1, which, 1, &obj));
    JS_SetPropertyStr(ctx, src, "pull", JS_NewCFunctionData(ctx, rs_pull, 1, which, 1, &obj));
    JS_SetPropertyStr(ctx, src, "cancel", JS_NewCFunctionData(ctx, rs_cancel, 1, which, 1, &obj));
    JS_SetPropertyStr(ctx, strat, "highWaterMark", JS_NewInt64(ctx, hwm));
    JS_SetPropertyStr(ctx, strat, "size", JS_NewCFunction(ctx, rs_size, "size", 1));
    args[0] = src;
    args[1] = strat;
    r = JS_CallConstructor(ctx, rs_ctor, 2, args);
out:
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, strat);
    JS_FreeValue(ctx, rs_ctor);
    return r;
}

static int bytes_of(JSContext* ctx, JSValueConst v, const uint8_t** p, size_t* n, const char** cstr)
{
    *cstr = NULL;
    if (JS_IsString(v)) {
        *cstr = JS_ToCStringLen(ctx, n, v);
        if (!*cstr)
            return -1;
        *p = (const uint8_t*)*cstr;
        return 0;
    }
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
        *p = JS_GetArrayBuffer(ctx, n, v);
        return *p || *n == 0 ? 0 : -1;
    }
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW) {
        size_t off = 0, len = 0, bpe = 0, abn = 0;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return -1;
        base = JS_GetArrayBuffer(ctx, &abn, ab);
        JS_FreeValue(ctx, ab);
        if (!base && len) {
            JS_ThrowTypeError(ctx, "SSH: detached buffer");
            return -1;
        }
        if (off > abn || len > abn - off) {
            JS_ThrowRangeError(ctx, "SSH: view out of bounds");
            return -1;
        }
        *p = base ? base + off : (const uint8_t*)"";
        *n = len;
        return 0;
    }
    JS_ThrowTypeError(ctx, "SSH: expected a string, ArrayBuffer or ArrayBufferView");
    return -1;
}

static int opt_num(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, uint64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SSH: dyna.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || d != (double)(uint64_t)d) {
        JS_ThrowRangeError(ctx, "SSH: dyna.%s must be an integer in %.0f..%.0f", k, lo, hi);
        return -1;
    }
    *out = (uint64_t)d;
    return 0;
}

static char* opt_str(JSContext* ctx, JSValueConst o, const char* k, int* err)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char* s;
    size_t n;
    char* r = NULL;
    *err = 0;
    if (JS_IsException(v)) {
        *err = 1;
        return NULL;
    }
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return NULL;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SSH: option %s must be a string", k);
        *err = 1;
        return NULL;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        *err = 1;
        return NULL;
    }
    if (memchr(s, 0, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "SSH: option %s contains a NUL character", k);
        *err = 1;
        return NULL;
    }
    r = strdup(s);
    JS_FreeCString(ctx, s);
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        *err = 1;
    }
    return r;
}

static const char* const conn_keys[] = { "host", "port", "username", "privateKey", "passphrase", "certificate", "knownHosts", "via", "agent", "dyna" };
static const char* const meta_keys[] = { "hostKeyPolicy", "allowRsaSha2_512", "maxPacketBytes", "windowBytes", "maxChannels", "handshakeTimeoutMs", "rekeyBytes", "rekeySeconds", "shrinkBuffers" };

static int opt_bool(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    JSValue b = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(b))
        return -1;
    if (!JS_IsUndefined(b)) {
        if (!JS_IsBool(b)) {
            JS_FreeValue(ctx, b);
            JS_ThrowTypeError(ctx, "SSH: dyna.%s must be a boolean", k);
            return -1;
        }
        *out = JS_ToBool(ctx, b);
    }
    JS_FreeValue(ctx, b);
    return 0;
}

static int parse_meta(JSContext* ctx, JSValueConst m, ssh_cfg_t* cfg)
{
    uint64_t v;
    int err;
    char* pol;
    JSValue b;
    if (JS_IsUndefined(m))
        return 0;
    if (!JS_IsObject(m)) {
        JS_ThrowTypeError(ctx, "SSH: dyna must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, m, meta_keys, (int)countof(meta_keys)))
        return -1;
    pol = opt_str(ctx, m, "hostKeyPolicy", &err);
    if (err)
        return -1;
    if (pol) {
        if (strcmp(pol, "accept-new") == 0)
            cfg->accept_new = 1;
        else if (strcmp(pol, "strict") != 0) {
            free(pol);
            JS_ThrowTypeError(ctx, "SSH: dyna.hostKeyPolicy must be \"strict\" or \"accept-new\"");
            return -1;
        }
        free(pol);
    }
    b = JS_GetPropertyStr(ctx, m, "allowRsaSha2_512");
    if (JS_IsException(b))
        return -1;
    if (!JS_IsUndefined(b)) {
        if (!JS_IsBool(b)) {
            JS_FreeValue(ctx, b);
            JS_ThrowTypeError(ctx, "SSH: dyna.allowRsaSha2_512 must be a boolean");
            return -1;
        }
        cfg->allow_rsa = JS_ToBool(ctx, b);
    }
    JS_FreeValue(ctx, b);
    v = cfg->max_packet;
    if (opt_num(ctx, m, "maxPacketBytes", 35000, 16777216, &v) < 0)
        return -1;
    cfg->max_packet = (uint32_t)v;
    v = cfg->window;
    if (opt_num(ctx, m, "windowBytes", 65536, 1073741824, &v) < 0)
        return -1;
    cfg->window = (uint32_t)v;
    v = (uint64_t)cfg->max_channels;
    if (opt_num(ctx, m, "maxChannels", 1, 4096, &v) < 0)
        return -1;
    cfg->max_channels = (int)v;
    if (opt_num(ctx, m, "handshakeTimeoutMs", 1, 600000, &cfg->handshake_ms) < 0)
        return -1;
    if (opt_num(ctx, m, "rekeyBytes", 65536, 4294967296.0, &cfg->rekey_bytes) < 0)
        return -1;
    v = cfg->rekey_ms / 1000;
    if (opt_num(ctx, m, "rekeySeconds", 1, 86400, &v) < 0)
        return -1;
    cfg->rekey_ms = v * 1000;
    return opt_bool(ctx, m, "shrinkBuffers", &cfg->shrink);
}

static void tick_hook(void* ud)
{
    ssh_conn_t* c = (ssh_conn_t*)ud;
    c->busy++;
    ssh_tick(c, ssh_now_ms());
    conn_exit(c);
}

static void on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_conn_t* c = (ssh_conn_t*)ud;
    (void)aio;
    if (c->state == SSH_ST_DEAD)
        return;
    c->busy++;
    if (res < 0)
        ssh_fail(c, 0, "connection error: %s", strerror(-res));
    else if (res == 0 && len == 0)
        ssh_fail(c, 0, "the %s closed the connection", c->server ? "client" : "server");
    else
        ssh_input(c, buf, len);
    conn_exit(c);
}

static void on_connect(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_conn_t* c = (ssh_conn_t*)ud;
    (void)buf;
    (void)len;
    if (c->state == SSH_ST_DEAD)
        return;
    c->busy++;
    if (res < 0)
        ssh_fail(c, 0, "cannot connect to %s:%u", c->host, (unsigned)c->port);
    else {
        c->tcp_up = 1;
        if (dyn_aio_recv(aio, c->fd, 0, 1, on_recv, c) < 0)
            ssh_fail(c, 0, "cannot read from the socket");
        else
            ssh_flush(c);
    }
    conn_exit(c);
}

static JSValue ssh_client_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    (void)nt;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "SSH: use SSHClient.connect(options)");
}

static JSValue ssh_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ssh_conn_t* c;
    JSValue key, pass, meta, port, obj, prom, funcs[2];
    const uint8_t* kp;
    size_t kn = 0, pn = 0;
    const char *kstr = NULL, *pstr = NULL;
    char why[200];
    int err;
    uint64_t portv = 22;
    (void)this_val;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SSH: connect expects an options object");
    if (dyn_opts_strict(ctx, o, conn_keys, (int)countof(conn_keys)))
        return JS_EXCEPTION;
    c = (ssh_conn_t*)calloc(1, sizeof *c);
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->fd = -1;
    c->self_weak = c->conn_res = c->conn_rej = c->via_obj = JS_UNDEFINED;
    c->cfg.max_packet = 262144;
    c->cfg.window = 2097152;
    c->cfg.chan_packet = 32768;
    c->cfg.max_channels = 64;
    c->cfg.handshake_ms = 30000;
    c->cfg.rekey_bytes = (uint64_t)1 << 30;
    c->cfg.rekey_ms = 3600000;
    c->host = opt_str(ctx, o, "host", &err);
    if (err)
        goto fail;
    if (!c->host) {
        JS_ThrowTypeError(ctx, "SSH: options.host is required");
        goto fail;
    }
    c->user = opt_str(ctx, o, "username", &err);
    if (err)
        goto fail;
    if (!c->user) {
        JS_ThrowTypeError(ctx, "SSH: options.username is required");
        goto fail;
    }
    port = JS_GetPropertyStr(ctx, o, "port");
    if (!JS_IsUndefined(port)) {
        double d;
        if (!JS_IsNumber(port) || JS_ToFloat64(ctx, &d, port) < 0 || !(d >= 1 && d <= 65535) || d != (double)(int)d) {
            JS_FreeValue(ctx, port);
            JS_ThrowRangeError(ctx, "SSH: options.port must be an integer in 1..65535");
            goto fail;
        }
        portv = (uint64_t)d;
    }
    JS_FreeValue(ctx, port);
    c->port = (uint16_t)portv;
    c->known_hosts = opt_str(ctx, o, "knownHosts", &err);
    if (err)
        goto fail;
    if (!c->known_hosts) {
        const char* home = getenv("HOME");
        if (home) {
            size_t hl = strlen(home) + 32;
            c->known_hosts = (char*)malloc(hl);
            if (c->known_hosts)
                snprintf(c->known_hosts, hl, "%s/.ssh/known_hosts", home);
        }
    }
    meta = JS_GetPropertyStr(ctx, o, "dyna");
    err = parse_meta(ctx, meta, &c->cfg);
    JS_FreeValue(ctx, meta);
    if (err)
        goto fail;
    {
        JSValue ag = JS_GetPropertyStr(ctx, o, "agent");
        if (JS_IsException(ag))
            goto fail;
        if (JS_IsBool(ag) && JS_ToBool(ctx, ag)) {
            const char* sock = getenv("SSH_AUTH_SOCK");
            if (!sock || !sock[0]) {
                JS_ThrowTypeError(ctx, "SSH: options.agent is true but SSH_AUTH_SOCK is not set");
                goto fail;
            }
            c->agent_path = strdup(sock);
        } else if (JS_IsString(ag)) {
            JS_FreeValue(ctx, ag);
            c->agent_path = opt_str(ctx, o, "agent", &err);
            if (err)
                goto fail;
            if (!c->agent_path || !c->agent_path[0]) {
                JS_ThrowTypeError(ctx, "SSH: options.agent must be a non-empty socket path or true");
                goto fail;
            }
        } else if (!JS_IsUndefined(ag) && !(JS_IsBool(ag) && !JS_ToBool(ctx, ag))) {
            JS_FreeValue(ctx, ag);
            JS_ThrowTypeError(ctx, "SSH: options.agent must be a unix socket path or true (SSH_AUTH_SOCK)");
            goto fail;
        }
        if (c->agent_path && strlen(c->agent_path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
            JS_ThrowTypeError(ctx, "SSH: options.agent: the socket path is longer than a unix socket address allows (%zu bytes)", sizeof(((struct sockaddr_un*)0)->sun_path) - 1);
            goto fail;
        }
    }
    key = JS_GetPropertyStr(ctx, o, "privateKey");
    pass = JS_GetPropertyStr(ctx, o, "passphrase");
    if (JS_IsUndefined(key) && c->agent_path) {
        JS_FreeValue(ctx, key);
        if (!JS_IsUndefined(pass)) {
            JS_FreeValue(ctx, pass);
            JS_ThrowTypeError(ctx, "SSH: options.passphrase needs options.privateKey");
            goto fail;
        }
        goto keyed;
    }
    if (JS_IsUndefined(key)) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        JS_ThrowTypeError(ctx, "SSH: options.privateKey (an OpenSSH ssh-ed25519 private key) or options.agent is required");
        goto fail;
    }
    if (!JS_IsUndefined(pass) && !JS_IsString(pass)) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        JS_ThrowTypeError(ctx, "SSH: options.passphrase must be a string");
        goto fail;
    }
    if (bytes_of(ctx, key, &kp, &kn, &kstr) < 0) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        goto fail;
    }
    if (JS_IsString(pass))
        pstr = JS_ToCStringLen(ctx, &pn, pass);
    c->ukey = ssh_parse_private_key(kp, kn, pstr, pn, c->upub, why, sizeof why);
    if (kstr)
        JS_FreeCString(ctx, kstr);
    if (pstr)
        JS_FreeCString(ctx, pstr);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, pass);
    if (!c->ukey) {
        JS_ThrowTypeError(ctx, "SSH: %s", why);
        goto fail;
    }
keyed:
    {
        char* cert = opt_str(ctx, o, "certificate", &err);
        char ty[48];
        ssh_cert_t ct;
        if (err)
            goto fail;
        if (cert && !c->ukey) {
            free(cert);
            JS_ThrowTypeError(ctx, "SSH: options.certificate needs options.privateKey (an agent holds its own certificates)");
            goto fail;
        }
        if (cert) {
            int bad = ssh_parse_pubkey(cert, strlen(cert), &c->ucert, ty, sizeof ty) < 0 || strcmp(ty, "ssh-ed25519-cert-v01@openssh.com") != 0;
            free(cert);
            if (bad) {
                JS_ThrowTypeError(ctx, "SSH: options.certificate must be an ssh-ed25519-cert-v01@openssh.com public key line");
                goto fail;
            }
            if (ssh_cert_parse(c->ucert.p, c->ucert.len, &ct, why, sizeof why) < 0) {
                JS_ThrowTypeError(ctx, "SSH: certificate: %s", why);
                goto fail;
            }
            if (ct.type != SSH_CERT_USER || memcmp(ct.pk, c->upub, 32) != 0) {
                JS_ThrowTypeError(ctx, "SSH: options.certificate is not a user certificate for privateKey");
                goto fail;
            }
        }
    }
    if (c->known_hosts)
        c->offer_hk_cert = ssh_known_hosts_has_ca(c->known_hosts, c->host, c->port);
    {
        JSValue via = JS_GetPropertyStr(ctx, o, "via");
        if (JS_IsException(via))
            goto fail;
        if (!JS_IsUndefined(via)) {
            ssh_conn_t* outer = JS_IsObject(via) ? (ssh_conn_t*)dyn_res_native(ctx, via, ssh_client_cid) : NULL;
            if (!outer) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, via);
                JS_ThrowTypeError(ctx, "SSH: options.via must be an SSHClient");
                goto fail;
            }
            if (outer->state != SSH_ST_READY) {
                JS_FreeValue(ctx, via);
                JS_ThrowTypeError(ctx, "SSH: options.via is not connected");
                goto fail;
            }
            c->via_obj = via;
        }
    }
    c->aio = dyn_net_reactor_acquire(ctx);
    if (!c->aio) {
        dyn_net_throw_no_reactor(ctx, "SSHClient");
        goto fail;
    }
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, ssh_client_cid, c, ssh_dispose);
    if (JS_IsException(obj))
        return obj;
    c->self_weak = obj;
    prom = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(prom)) {
        JS_FreeValue(ctx, obj);
        return prom;
    }
    c->conn_res = funcs[0];
    c->conn_rej = funcs[1];
    if (dyn_res_pin(ctx, obj, ssh_client_cid) == 0)
        c->pinned = 1;
    if (JS_AddShutdownSweep(c->rt, ssh_conn_sweep, c) == 0)
        c->swept = 1;
    if (dyn_net_on_drain(tick_hook, c) == 0)
        c->hooked = 1;
    c->busy++;
    if (ssh_start(c) < 0)
        ssh_fail(c, 0, "cannot start the handshake");
    else {
        if (!JS_IsUndefined(c->via_obj)) {
            ssh_conn_t* outer = (ssh_conn_t*)dyn_res_native(ctx, c->via_obj, ssh_client_cid);
            if (!outer || ssh_via_open(c, outer) < 0)
                ssh_fail(c, 0, "cannot open a channel to %s:%u through the jump host", c->host, (unsigned)c->port);
        } else {
            c->fd = dyn_aio_connect(c->aio, c->host, c->port, on_connect, c);
            if (c->fd < 0)
                ssh_fail(c, 0, "cannot connect to %s:%u: %s", c->host, (unsigned)c->port, strerror(errno));
        }
    }
    conn_exit(c);
    JS_FreeValue(ctx, obj);
    return prom;
fail:
    c->released = 1;
    conn_free(c);
    return JS_EXCEPTION;
}

static ssh_conn_t* client_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_conn_t*)dyn_res_native(ctx, v, ssh_client_cid);
}

static JSValue rejected(JSContext* ctx, JSValue err)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, err);
        return p;
    }
    call1(ctx, f[1], err);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    return p;
}

static JSValue ssh_exec(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    ssh_chan_t* ch;
    const char* cmd;
    size_t cl;
    JSValue obj, rs0, rs1, ep, op, f[2];
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "SSH: exec expects a command string");
    if (c->state != SSH_ST_READY)
        return rejected(ctx, ssh_error(ctx, c->err ? c->err : "the connection is not ready"));
    if (c->nopen >= c->cfg.max_channels) {
        char m[120];
        snprintf(m, sizeof m, "too many open channels (dyna.maxChannels is %d)", c->cfg.max_channels);
        return rejected(ctx, ssh_error(ctx, m));
    }
    cmd = JS_ToCStringLen(ctx, &cl, argv[0]);
    if (!cmd)
        return JS_EXCEPTION;
    ch = (ssh_chan_t*)calloc(1, sizeof *ch);
    if (!ch) {
        JS_FreeCString(ctx, cmd);
        return JS_ThrowOutOfMemory(ctx);
    }
    ch->cmd = (char*)malloc(cl + 1);
    if (ch->cmd)
        memcpy(ch->cmd, cmd, cl + 1);
    ch->cmdlen = cl;
    JS_FreeCString(ctx, cmd);
    ch->ctl[0] = ch->ctl[1] = ch->open_res = ch->open_rej = ch->exit_res = ch->jsobj = JS_UNDEFINED;
    ch->win_size = ch->lwin = c->cfg.window;
    ch->lmax = c->cfg.chan_packet;
    ch->refs = 1;
    if (!ch->cmd) {
        ssh_chan_unref(c->rt, ch);
        return JS_ThrowOutOfMemory(ctx);
    }
    obj = JS_NewObjectClass(ctx, (int)ssh_exec_cid);
    if (JS_IsException(obj)) {
        ssh_chan_unref(c->rt, ch);
        return obj;
    }
    JS_SetOpaque(obj, ch);
    rs0 = make_rs(ctx, obj, 0, c->cfg.window);
    rs1 = JS_IsException(rs0) ? JS_EXCEPTION : make_rs(ctx, obj, 1, c->cfg.window);
    if (JS_IsException(rs1)) {
        JS_FreeValue(ctx, rs0);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, obj, "stdout", rs0, JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, obj, "stderr", rs1, JS_PROP_ENUMERABLE);
    ep = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(ep)) {
        JS_FreeValue(ctx, obj);
        return ep;
    }
    ch->exit_res = f[0];
    JS_FreeValue(ctx, f[1]);
    JS_DefinePropertyValueStr(ctx, obj, "exit", ep, JS_PROP_ENUMERABLE);
    op = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(op)) {
        JS_FreeValue(ctx, obj);
        return op;
    }
    ch->open_res = f[0];
    ch->open_rej = f[1];
    ch->jsobj = obj;
    ch->js_held = 1;
    ch->refs++;
    c->busy++;
    if (ssh_chan_open(c, ch) < 0 && !ch->conn) {
        ch->conn = c;
        chan_closed_impl(ch, ctx, c->rt, "cannot open a channel");
        ch->conn = NULL;
        ssh_chan_unref(c->rt, ch);
    }
    ssh_flush(c);
    conn_exit(c);
    return op;
}

int ssh_js_off(void)
{
    return ssh_no_js;
}

ssh_conn_t* ssh_client_of(JSContext* ctx, JSValueConst v)
{
    return (ssh_conn_t*)dyn_res_native(ctx, v, ssh_client_cid);
}

ssh_chan_t* ssh_session_of(JSValueConst v)
{
    return (ssh_chan_t*)JS_GetOpaque(v, ssh_session_cid);
}

ssh_chan_t* ssh_subsys_open(ssh_conn_t* c, const char* name, const ssh_chan_ops_t* ops, void* opq, JSValueConst holder)
{
    ssh_chan_t* ch;
    if (c->state != SSH_ST_READY || c->nopen >= c->cfg.max_channels)
        return NULL;
    ch = (ssh_chan_t*)calloc(1, sizeof *ch);
    if (!ch)
        return NULL;
    ch->cmd = strdup(name);
    ch->cmdlen = strlen(name);
    ch->ctl[0] = ch->ctl[1] = ch->open_res = ch->open_rej = ch->exit_res = ch->jsobj = JS_UNDEFINED;
    ch->win_size = ch->lwin = c->cfg.window;
    ch->lmax = c->cfg.chan_packet;
    ch->refs = 1;
    ch->subsys = 1;
    if (!ch->cmd) {
        free(ch);
        return NULL;
    }
    ch->jsobj = JS_DupValue(c->ctx, holder);
    ch->js_held = 1;
    ch->refs++;
    c->busy++;
    if (ssh_chan_open(c, ch) < 0 && !ch->conn) {
        ch->conn = c;
        chan_closed_impl(ch, c->ctx, c->rt, "cannot open a channel");
        ch->conn = NULL;
        ssh_chan_unref(c->rt, ch);
        ssh_chan_unref(c->rt, ch);
        conn_exit(c);
        return NULL;
    }
    ch->ops = ops;
    ch->opq = opq;
    ssh_flush(c);
    conn_exit(c);
    return ch;
}

static JSValue ssh_rekey(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (c->state != SSH_ST_READY)
        return JS_ThrowTypeError(ctx, "SSH: rekey() needs an authenticated connection");
    c->busy++;
    (void)ssh_start_rekey(c);
    ssh_flush(c);
    conn_exit(c);
    return JS_UNDEFINED;
}

static JSValue ssh_get_hostkey(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    JSValue o;
    if (!c)
        return JS_EXCEPTION;
    if (!c->hk_type[0])
        return JS_NULL;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, c->hk_type));
    JS_SetPropertyStr(ctx, o, "fingerprint", JS_NewString(ctx, c->hk_fp));
    return o;
}

static JSValue ssh_get_algs(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    JSValue o;
    if (!c)
        return JS_EXCEPTION;
    if (!c->kex_done_once)
        return JS_NULL;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "kex", JS_NewString(ctx, ssh_kex_name(c->kex_alg)));
    JS_SetPropertyStr(ctx, o, "hostKey", JS_NewString(ctx, ssh_hk_name(c->hk_alg)));
    JS_SetPropertyStr(ctx, o, "cipherClientToServer", JS_NewString(ctx, cipher_name(c->cip_cs)));
    JS_SetPropertyStr(ctx, o, "cipherServerToClient", JS_NewString(ctx, cipher_name(c->cip_sc)));
    JS_SetPropertyStr(ctx, o, "strictKex", JS_NewBool(ctx, c->strict));
    return o;
}

static JSValue ssh_get_stats(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    JSValue o;
    if (!c)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "bytesIn", JS_NewFloat64(ctx, (double)c->bytes_in));
    JS_SetPropertyStr(ctx, o, "bytesOut", JS_NewFloat64(ctx, (double)c->bytes_out));
    JS_SetPropertyStr(ctx, o, "packetsIn", JS_NewFloat64(ctx, (double)c->pk_in));
    JS_SetPropertyStr(ctx, o, "packetsOut", JS_NewFloat64(ctx, (double)c->pk_out));
    JS_SetPropertyStr(ctx, o, "rekeys", JS_NewInt32(ctx, c->rekeys));
    JS_SetPropertyStr(ctx, o, "openChannels", JS_NewInt32(ctx, c->nopen));
    {
        size_t held = c->rbuf.cap + c->obuf.cap + c->msg.cap + c->pend.cap;
        int i;
        for (i = 0; i < c->nch; i++)
            if (c->ch[i])
                held += c->ch[i]->inq.cap + c->ch[i]->errq.cap;
        JS_SetPropertyStr(ctx, o, "bufferBytes", JS_NewFloat64(ctx, (double)held));
    }
    return o;
}

static JSValue ssh_get_version(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return c->vs[0] ? JS_NewString(ctx, c->vs) : JS_NULL;
}

static JSValue ssh_get_banner(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return c->banner ? JS_NewString(ctx, c->banner) : JS_NULL;
}

static JSValue ssh_get_connected(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, c->state == SSH_ST_READY);
}

static JSValue chan_write(JSContext* ctx, ssh_chan_t* ch, int which, int argc, JSValueConst* argv)
{
    sshb_t* q = which ? &ch->errq : &ch->inq;
    size_t qoff = which ? ch->errq_off : ch->inq_off;
    const uint8_t* p;
    size_t n;
    const char* cs;
    JSValue f[2], pr, holder;
    ssh_wwait_t* nw;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "SSH: write expects data");
    if (!ch->conn || ch->want_eof || ch->sent_close || ch->state == SSH_CH_CLOSED)
        return rejected(ctx, ssh_error(ctx, ch->srv ? "the session's output is closed" : "the channel's stdin is closed"));
    if (bytes_of(ctx, argv[0], &p, &n, &cs) < 0)
        return JS_EXCEPTION;
    if (q->len - qoff + n > ((size_t)256 << 20)) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return rejected(ctx, ssh_error(ctx, "more than 256 MiB is queued; await earlier writes"));
    }
    if (sshb_put(q, p, n) < 0) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (cs)
        JS_FreeCString(ctx, cs);
    if (which)
        ch->err_total += n;
    else
        ch->in_total += n;
    pr = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(pr))
        return pr;
    holder = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, holder, "resolve", f[0]);
    JS_SetPropertyStr(ctx, holder, "reject", f[1]);
    if (ch->nww == ch->cww) {
        int cap = ch->cww ? ch->cww * 2 : 4;
        nw = (ssh_wwait_t*)realloc(ch->ww, (size_t)cap * sizeof *nw);
        if (!nw) {
            JS_FreeValue(ctx, holder);
            JS_FreeValue(ctx, pr);
            return JS_ThrowOutOfMemory(ctx);
        }
        ch->ww = nw;
        ch->cww = cap;
    }
    ch->ww[ch->nww].mark = which ? ch->err_total : ch->in_total;
    ch->ww[ch->nww].which = which;
    ch->ww[ch->nww].resolve = holder;
    ch->nww++;
    ssh_chan_kick(ch);
    return pr;
}

static JSValue exec_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = exec_get(ctx, this_val);
    if (!ch)
        return JS_EXCEPTION;
    return chan_write(ctx, ch, 0, argc, argv);
}

static JSValue exec_signal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = exec_get(ctx, this_val);
    ssh_conn_t* c;
    const char* sig;
    const char* name;
    size_t i, n;
    if (!ch)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "SSH: signal expects a signal name such as \"TERM\"");
    sig = JS_ToCString(ctx, argv[0]);
    if (!sig)
        return JS_EXCEPTION;
    name = strncmp(sig, "SIG", 3) == 0 ? sig + 3 : sig;
    n = strlen(name);
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9')))
            break;
    if (n == 0 || n > 20 || i != n) {
        JS_FreeCString(ctx, sig);
        return JS_ThrowTypeError(ctx, "SSH: signal expects a signal name such as \"TERM\"");
    }
    c = ch->conn;
    if (c && ch->state == SSH_CH_OPEN && !ch->sent_close && c->state == SSH_ST_READY) {
        c->busy++;
        ssh_msg_begin(c, SSH_MSG_CHANNEL_REQUEST);
        ssh_m_u32(c, ch->rid);
        ssh_m_cstr(c, "signal");
        ssh_m_u8(c, 0);
        ssh_m_cstr(c, name);
        (void)ssh_send_msg(c);
        ssh_flush(c);
        conn_exit(c);
    }
    JS_FreeCString(ctx, sig);
    return JS_UNDEFINED;
}

static JSValue ssh_get_sigalgs(JSContext* ctx, JSValueConst this_val)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    JSValue a;
    const char* p;
    uint32_t k = 0;
    if (!c)
        return JS_EXCEPTION;
    if (!c->server_sig_algs)
        return JS_NULL;
    a = JS_NewArray(ctx);
    p = c->server_sig_algs;
    while (*p) {
        const char* e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l)
            JS_SetPropertyUint32(ctx, a, k++, JS_NewStringLen(ctx, p, l));
        p += l + (e ? 1 : 0);
    }
    return a;
}

static JSValue exec_end(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = exec_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!ch)
        return JS_EXCEPTION;
    ch->want_eof = 1;
    ssh_chan_kick(ch);
    return JS_UNDEFINED;
}

static JSValue exec_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = exec_get(ctx, this_val);
    ssh_conn_t* c;
    (void)argc;
    (void)argv;
    if (!ch)
        return JS_EXCEPTION;
    c = ch->conn;
    if (!c)
        return JS_UNDEFINED;
    c->busy++;
    ssh_chan_close(ch);
    ssh_flush(c);
    conn_exit(c);
    return JS_UNDEFINED;
}

static void exec_finalizer(JSRuntime* rt, JSValue val)
{
    ssh_chan_t* ch = chan_of(val);
    int i;
    if (!ch)
        return;
    ch->js_gone = 1;
    ch->jsobj = JS_UNDEFINED;
    ch->js_held = 0;
    free_val(rt, &ch->ctl[0]);
    free_val(rt, &ch->ctl[1]);
    free_val(rt, &ch->open_res);
    free_val(rt, &ch->open_rej);
    free_val(rt, &ch->exit_res);
    for (i = 0; i < ch->nww; i++)
        free_val(rt, &ch->ww[i].resolve);
    ch->nww = 0;
    if (ch->conn)
        ssh_chan_close(ch);
    ssh_chan_unref(rt, ch);
}

static void exec_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    ssh_chan_t* ch = chan_of(val);
    int i;
    if (!ch)
        return;
    JS_MarkValue(rt, ch->ctl[0], mark);
    JS_MarkValue(rt, ch->ctl[1], mark);
    JS_MarkValue(rt, ch->open_res, mark);
    JS_MarkValue(rt, ch->open_rej, mark);
    JS_MarkValue(rt, ch->exit_res, mark);
    for (i = 0; i < ch->nww; i++)
        JS_MarkValue(rt, ch->ww[i].resolve, mark);
}

static JSClassID ssh_fwd_cid;

void ssh_fwd_drop(ssh_fwd_t* f)
{
    JSValue v = f->obj;
    f->obj = JS_UNDEFINED;
    JS_FreeValueRT(f->rt, v);
}

void ssh_fwd_settle(ssh_fwd_t* f, const char* err)
{
    JSContext* ctx = f->ctx;
    if (JS_IsUndefined(f->res))
        return;
    if (!err)
        f->state = 1;
    if (!ssh_no_js) {
        if (err) {
            JSValue e = ssh_error(ctx, err);
            call1(ctx, f->rej, e);
            JS_FreeValue(ctx, e);
        } else
            call1(ctx, f->res, f->obj);
    }
    free_val(f->rt, &f->res);
    free_val(f->rt, &f->rej);
}

static void fwd_finalizer(JSRuntime* rt, JSValue val)
{
    ssh_fwd_t* f = (ssh_fwd_t*)JS_GetOpaque(val, ssh_fwd_cid);
    (void)rt;
    if (!f)
        return;
    ssh_no_js++;
    f->obj = JS_UNDEFINED;
    if (f->c)
        ssh_fwd_close(f);
    ssh_no_js--;
    free(f->lhost);
    free(f->rhost);
    free(f);
}

static const JSClassDef ssh_fwd_class = {
    "SSHForward",
    .finalizer = fwd_finalizer,
};

static ssh_fwd_t* fwd_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_fwd_t*)JS_GetOpaque2(ctx, v, ssh_fwd_cid);
}

static JSValue fwd_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_fwd_t* f = fwd_get(ctx, this_val);
    ssh_conn_t* c;
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    c = f->c;
    if (!c)
        return JS_UNDEFINED;
    c->busy++;
    ssh_fwd_close(f);
    conn_exit(c);
    return JS_UNDEFINED;
}

static JSValue fwd_get_prop(JSContext* ctx, JSValueConst this_val, int magic)
{
    ssh_fwd_t* f = fwd_get(ctx, this_val);
    int remote;
    if (!f)
        return JS_EXCEPTION;
    remote = f->kind == 1;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, remote ? "remote" : "local");
    case 1:
        return JS_NewString(ctx, remote ? f->rhost : f->lhost);
    case 2:
        return JS_NewInt32(ctx, remote ? f->rport : f->lport);
    case 3:
        return JS_NewString(ctx, remote ? f->lhost : f->rhost);
    case 4:
        return JS_NewInt32(ctx, remote ? f->lport : f->rport);
    case 5:
        return JS_NewFloat64(ctx, (double)f->conns);
    default:
        return JS_NewBool(ctx, f->state == 2);
    }
}

static const JSCFunctionListEntry ssh_fwd_proto[] = {
    JS_CFUNC_DEF("close", 0, fwd_close),
    JS_CGETSET_MAGIC_DEF("type", fwd_get_prop, NULL, 0),
    JS_CGETSET_MAGIC_DEF("listenHost", fwd_get_prop, NULL, 1),
    JS_CGETSET_MAGIC_DEF("listenPort", fwd_get_prop, NULL, 2),
    JS_CGETSET_MAGIC_DEF("targetHost", fwd_get_prop, NULL, 3),
    JS_CGETSET_MAGIC_DEF("targetPort", fwd_get_prop, NULL, 4),
    JS_CGETSET_MAGIC_DEF("connections", fwd_get_prop, NULL, 5),
    JS_CGETSET_MAGIC_DEF("closed", fwd_get_prop, NULL, 6),
};

static const char* const fwd_keys[] = { "host", "port", "localHost", "localPort", "remoteHost", "remotePort" };

static int fwd_port(JSContext* ctx, JSValueConst o, const char* k, int lo, int def)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return def;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= lo && d <= 65535) || d != (double)(int)d) {
        JS_FreeValue(ctx, v);
        JS_ThrowRangeError(ctx, "SSH: options.%s must be an integer in %d..65535", k, lo);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return (int)d;
}

static ssh_fwd_t* fwd_parse(JSContext* ctx, ssh_conn_t* c, JSValueConst o, int remote)
{
    ssh_fwd_t* f;
    int err, tp, bp;
    char *host = NULL, *bind = NULL;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "SSH: %s expects an options object", remote ? "forwardRemote" : "forwardLocal");
        return NULL;
    }
    if (dyn_opts_strict(ctx, o, remote ? fwd_keys : fwd_keys, (int)countof(fwd_keys)))
        return NULL;
    {
        JSValue other = JS_GetPropertyStr(ctx, o, remote ? "localHost" : "remoteHost");
        JSValue other2 = JS_GetPropertyStr(ctx, o, remote ? "localPort" : "remotePort");
        int bad = !JS_IsUndefined(other) || !JS_IsUndefined(other2);
        JS_FreeValue(ctx, other);
        JS_FreeValue(ctx, other2);
        if (bad) {
            JS_ThrowTypeError(ctx, "SSH: %s takes host, port, %s and %s", remote ? "forwardRemote" : "forwardLocal",
                remote ? "remoteHost" : "localHost", remote ? "remotePort" : "localPort");
            return NULL;
        }
    }
    host = opt_str(ctx, o, "host", &err);
    if (err)
        return NULL;
    if (!host || !host[0] || strlen(host) > 255) {
        free(host);
        JS_ThrowTypeError(ctx, "SSH: options.host is required (1..255 bytes)");
        return NULL;
    }
    tp = fwd_port(ctx, o, "port", 1, -2);
    if (tp == -2) {
        free(host);
        JS_ThrowTypeError(ctx, "SSH: options.port is required");
        return NULL;
    }
    if (tp < 0) {
        free(host);
        return NULL;
    }
    bind = opt_str(ctx, o, remote ? "remoteHost" : "localHost", &err);
    if (err) {
        free(host);
        return NULL;
    }
    if (bind && strlen(bind) > 255) {
        free(host);
        free(bind);
        JS_ThrowTypeError(ctx, "SSH: options.%s is longer than 255 bytes", remote ? "remoteHost" : "localHost");
        return NULL;
    }
    bp = fwd_port(ctx, o, remote ? "remotePort" : "localPort", 0, 0);
    if (bp < 0) {
        free(host);
        free(bind);
        return NULL;
    }
    if (!bind)
        bind = strdup("127.0.0.1");
    f = (ssh_fwd_t*)calloc(1, sizeof *f);
    if (!f || !bind) {
        free(f);
        free(host);
        free(bind);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    f->ctx = ctx;
    f->rt = c->rt;
    f->kind = remote;
    f->lfd = -1;
    f->obj = f->res = f->rej = JS_UNDEFINED;
    if (remote) {
        f->lhost = host;
        f->lport = (uint16_t)tp;
        f->rhost = bind;
        f->rport = (uint16_t)bp;
    } else {
        f->lhost = bind;
        f->lport = (uint16_t)bp;
        f->rhost = host;
        f->rport = (uint16_t)tp;
    }
    return f;
}

static JSValue fwd_wrap(JSContext* ctx, ssh_fwd_t* f)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)ssh_fwd_cid);
    if (JS_IsException(obj)) {
        free(f->lhost);
        free(f->rhost);
        free(f);
        return obj;
    }
    JS_SetOpaque(obj, f);
    return obj;
}

static JSValue ssh_forward_local(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    ssh_fwd_t* f;
    JSValue obj;
    char why[200];
    if (!c)
        return JS_EXCEPTION;
    f = fwd_parse(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED, 0);
    if (!f)
        return JS_EXCEPTION;
    obj = fwd_wrap(ctx, f);
    if (JS_IsException(obj))
        return obj;
    if (c->state != SSH_ST_READY) {
        JS_FreeValue(ctx, obj);
        return JS_Throw(ctx, ssh_error(ctx, c->err ? c->err : "the connection is not ready"));
    }
    if (ssh_fwd_listen_local(c, f, why, sizeof why) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_Throw(ctx, ssh_error(ctx, why));
    }
    f->obj = JS_DupValue(ctx, obj);
    return obj;
}

static JSValue ssh_forward_remote(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_conn_t* c = client_get(ctx, this_val);
    ssh_fwd_t* f;
    JSValue obj, p, fns[2];
    if (!c)
        return JS_EXCEPTION;
    f = fwd_parse(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED, 1);
    if (!f)
        return JS_EXCEPTION;
    obj = fwd_wrap(ctx, f);
    if (JS_IsException(obj))
        return obj;
    if (c->state != SSH_ST_READY) {
        JS_FreeValue(ctx, obj);
        return rejected(ctx, ssh_error(ctx, c->err ? c->err : "the connection is not ready"));
    }
    p = JS_NewPromiseCapability(ctx, fns);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, obj);
        return p;
    }
    f->res = fns[0];
    f->rej = fns[1];
    f->obj = obj;
    c->busy++;
    if (ssh_fwd_request_remote(c, f) < 0) {
        ssh_fwd_settle(f, "cannot send the tcpip-forward request");
        f->state = 2;
        ssh_fwd_drop(f);
    }
    conn_exit(c);
    return p;
}

static JSValue fwd_settle_cb(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    DynResource* r = (DynResource*)JS_GetOpaque(data[0], ssh_server_cid);
    ssh_srv_t* s;
    ssh_conn_t* c;
    double cid = 0, tok = 0;
    (void)this_val;
    if (!r || r->closed || !r->native)
        return JS_UNDEFINED;
    s = (ssh_srv_t*)r->native;
    JS_ToFloat64(ctx, &cid, data[1]);
    JS_ToFloat64(ctx, &tok, data[2]);
    for (c = s->conns; c; c = c->snext)
        if (c->cid == (uint64_t)cid)
            break;
    if (!c || c->state != SSH_ST_READY)
        return JS_UNDEFINED;
    ssh_fwd_decided(c, (uint64_t)tok, magic == 1 && argc > 0 && JS_IsBool(argv[0]) && JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

int ssh_js_forward(ssh_conn_t* c, const char* type, const char* host, uint32_t port, const char* ohost, uint32_t oport, uint64_t token)
{
    ssh_srv_t* s = c->srv;
    JSContext* ctx;
    JSValue info, ret;
    int rc = 0;
    if (!s || ssh_no_js || !JS_IsFunction(s->ctx, s->forward))
        return 0;
    ctx = s->ctx;
    info = JS_NewObject(ctx);
    if (JS_IsException(info)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_SetPropertyStr(ctx, info, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, info, "username", JS_NewString(ctx, c->user ? c->user : ""));
    JS_SetPropertyStr(ctx, info, "host", JS_NewString(ctx, host));
    JS_SetPropertyStr(ctx, info, "port", JS_NewInt32(ctx, (int32_t)port));
    JS_SetPropertyStr(ctx, info, "originatorHost", JS_NewString(ctx, ohost));
    JS_SetPropertyStr(ctx, info, "originatorPort", JS_NewInt32(ctx, (int32_t)oport));
    JS_SetPropertyStr(ctx, info, "remoteAddress", JS_NewString(ctx, c->peer));
    JS_SetPropertyStr(ctx, info, "keyFingerprint", JS_NewString(ctx, c->ufp));
    ret = JS_Call(ctx, s->forward, JS_UNDEFINED, 1, (JSValueConst*)&info);
    JS_FreeValue(ctx, info);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (JS_IsBool(ret))
        rc = JS_ToBool(ctx, ret) ? 1 : 0;
    else if (JS_IsObject(ret)) {
        JSValue d[3];
        d[0] = s->self_weak;
        d[1] = JS_NewFloat64(ctx, (double)c->cid);
        d[2] = JS_NewFloat64(ctx, (double)token);
        if (ssh_then2(ctx, ret, fwd_settle_cb, 3, (JSValueConst*)d) == 0)
            rc = 2;
    }
    JS_FreeValue(ctx, ret);
    return rc;
}

static void client_finalizer(JSRuntime* rt, JSValue val)
{
    ssh_no_js++;
    dyn_res_finalizer(rt, val);
    ssh_no_js--;
}

static void client_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, ssh_client_cid);
    ssh_conn_t* c;
    if (!r || r->closed || !r->native)
        return;
    c = (ssh_conn_t*)r->native;
    JS_MarkValue(rt, c->conn_res, mark);
    JS_MarkValue(rt, c->conn_rej, mark);
    JS_MarkValue(rt, c->via_obj, mark);
    {
        ssh_fwd_t* f;
        for (f = c->fwds; f; f = f->next) {
            JS_MarkValue(rt, f->obj, mark);
            JS_MarkValue(rt, f->res, mark);
            JS_MarkValue(rt, f->rej, mark);
        }
    }
}

static const JSClassDef ssh_client_class = {
    "SSHClient",
    .finalizer = client_finalizer,
    .gc_mark = client_mark,
};

static const JSClassDef ssh_exec_class = {
    "SSHExec",
    .finalizer = exec_finalizer,
    .gc_mark = exec_mark,
};

static const JSCFunctionListEntry ssh_client_proto[] = {
    JS_CFUNC_DEF("exec", 1, ssh_exec),
    JS_CFUNC_DEF("rekey", 0, ssh_rekey),
    JS_CGETSET_DEF("hostKey", ssh_get_hostkey, NULL),
    JS_CGETSET_DEF("algorithms", ssh_get_algs, NULL),
    JS_CGETSET_DEF("stats", ssh_get_stats, NULL),
    JS_CGETSET_DEF("serverVersion", ssh_get_version, NULL),
    JS_CGETSET_DEF("banner", ssh_get_banner, NULL),
    JS_CGETSET_DEF("connected", ssh_get_connected, NULL),
    JS_CGETSET_DEF("serverSignatureAlgorithms", ssh_get_sigalgs, NULL),
    JS_CFUNC_DEF("forwardLocal", 1, ssh_forward_local),
    JS_CFUNC_DEF("forwardRemote", 1, ssh_forward_remote),
};

static const JSCFunctionListEntry ssh_exec_proto[] = {
    JS_CFUNC_DEF("write", 1, exec_write),
    JS_CFUNC_DEF("end", 0, exec_end),
    JS_CFUNC_DEF("close", 0, exec_close),
    JS_CFUNC_DEF("signal", 1, exec_signal),
};

static void ssh_srv_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void srv_free(ssh_srv_t* s)
{
    if (s->swept) {
        s->swept = 0;
        JS_RemoveShutdownSweep(s->rt, ssh_srv_sweep, s);
    }
    if (s->hooked) {
        dyn_net_off_drain(s);
        s->hooked = 0;
    }
    if (s->listen_fd >= 0 && s->aio)
        dyn_aio_close(s->aio, s->listen_fd);
    if (s->aio && !s->released) {
        s->released = 1;
        dyn_net_reactor_release_rt(s->rt);
    }
    free_val(s->rt, &s->authorize);
    free_val(s->rt, &s->on_session);
    free_val(s->rt, &s->forward);
    EVP_PKEY_free(s->hkey);
    sshb_free(&s->hblob);
    sshb_free(&s->hcert);
    sshb_free(&s->akeys);
    sshb_free(&s->cakeys);
    free(s->banner);
    free(s->host);
    OPENSSL_cleanse(s, sizeof *s);
    free(s);
}

static int srv_snapshot(ssh_srv_t* s, ssh_conn_t*** out)
{
    ssh_conn_t* c;
    int n = 0, i = 0;
    *out = NULL;
    for (c = s->conns; c; c = c->snext)
        n++;
    if (n == 0)
        return 0;
    *out = (ssh_conn_t**)malloc((size_t)n * sizeof **out);
    if (!*out)
        return 0;
    for (c = s->conns; c; c = c->snext) {
        c->busy++;
        (*out)[i++] = c;
    }
    return n;
}

static void srv_shutdown(ssh_srv_t* s, const char* why)
{
    ssh_conn_t** v;
    int n, i;
    if (s->listen_fd >= 0) {
        dyn_aio_close(s->aio, s->listen_fd);
        s->listen_fd = -1;
    }
    s->closed = 1;
    if (s->hooked) {
        dyn_net_off_drain(s);
        s->hooked = 0;
    }
    n = srv_snapshot(s, &v);
    for (i = 0; i < n; i++)
        ssh_fail(v[i], SSH_DISC_BY_APPLICATION, "%s", why);
    for (i = 0; i < n; i++)
        conn_exit(v[i]);
    free(v);
    if (s->aio && !s->released) {
        s->released = 1;
        dyn_net_reactor_release_rt(s->rt);
    }
}

static void ssh_srv_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    ssh_srv_t* s = (ssh_srv_t*)opaque;
    (void)ctx;
    s->swept = 0;
    ssh_no_js++;
    srv_shutdown(s, "the runtime is shutting down");
    free_val(rt, &s->authorize);
    free_val(rt, &s->on_session);
    free_val(rt, &s->forward);
    ssh_no_js--;
}

static void srv_dispose(void* native)
{
    ssh_srv_t* s = (ssh_srv_t*)native;
    if (!s)
        return;
    s->pinned = 0;
    srv_shutdown(s, "the server was closed");
    s->dead = 1;
    if (s->nconns == 0)
        srv_free(s);
}

static void srv_tick(void* ud)
{
    ssh_srv_t* s = (ssh_srv_t*)ud;
    ssh_conn_t** v;
    uint64_t now = ssh_now_ms();
    int n = srv_snapshot(s, &v), i;
    for (i = 0; i < n; i++)
        ssh_tick(v[i], now);
    for (i = 0; i < n; i++)
        conn_exit(v[i]);
    free(v);
}

static void srv_peer(int fd, char* out, size_t cap)
{
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    out[0] = 0;
    if (getpeername(fd, (struct sockaddr*)&sa, &sl) != 0)
        return;
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in*)&sa)->sin_addr, out, (socklen_t)cap);
    else if (sa.ss_family == AF_INET6) {
        struct in6_addr* a = &((struct sockaddr_in6*)&sa)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a))
            inet_ntop(AF_INET, (const uint8_t*)a + 12, out, (socklen_t)cap);
        else
            inet_ntop(AF_INET6, a, out, (socklen_t)cap);
    }
}

static void srv_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_srv_t* s = (ssh_srv_t*)ud;
    ssh_conn_t* c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (s->closed || s->nconns >= s->cfg.max_conns) {
        s->refused++;
        dyn_aio_close(aio, res);
        return;
    }
    c = (ssh_conn_t*)calloc(1, sizeof *c);
    if (!c) {
        s->refused++;
        dyn_aio_close(aio, res);
        return;
    }
    c->ctx = s->ctx;
    c->rt = s->rt;
    c->aio = aio;
    c->fd = res;
    c->server = 1;
    c->srv = s;
    c->cfg = s->cfg;
    c->self_weak = c->conn_res = c->conn_rej = c->via_obj = JS_UNDEFINED;
    c->cid = ++s->next_cid;
    c->cert_fwd = 1;
    c->fwd_hook = JS_IsFunction(s->ctx, s->forward);
    c->tcp_up = 1;
    c->cert_pty = 1;
    c->released = 1;
    c->srv_hook = JS_IsFunction(s->ctx, s->authorize);
    c->srv_session = JS_IsFunction(s->ctx, s->on_session);
    srv_peer(res, c->peer, sizeof c->peer);
    c->snext = s->conns;
    if (s->conns)
        s->conns->sprev = c;
    s->conns = c;
    s->nconns++;
    s->accepted++;
    c->busy++;
    if (ssh_start(c) < 0)
        ssh_fail(c, 0, "cannot start the handshake");
    else if (dyn_aio_recv(aio, c->fd, 0, 1, on_recv, c) < 0)
        ssh_fail(c, 0, "cannot read from the socket");
    else
        ssh_flush(c);
    conn_exit(c);
}

static JSValue key_line(JSContext* ctx, const uint8_t* blob, size_t n)
{
    sshr_t r;
    size_t tn, cap = n / 3 * 4 + 140, o;
    const uint8_t* t;
    char* buf;
    JSValue v;
    sshr_init(&r, blob, n);
    t = sshr_str(&r, &tn);
    if (r.err || tn > 64)
        return JS_NULL;
    buf = (char*)malloc(cap);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(buf, t, tn);
    o = tn;
    buf[o++] = ' ';
    o += ssh_b64_encode(blob, n, buf + o, 1);
    v = JS_NewStringLen(ctx, buf, o);
    free(buf);
    return v;
}

static JSValue u64_val(JSContext* ctx, uint64_t v)
{
    return v >= ((uint64_t)1 << 53) ? JS_NewFloat64(ctx, (double)INFINITY) : JS_NewFloat64(ctx, (double)v);
}

static JSValue cert_info(JSContext* ctx, const uint8_t* blob, size_t n)
{
    ssh_cert_t ct;
    char why[200], fp[64];
    JSValue o, arr;
    sshr_t r;
    uint32_t i = 0;
    if (ssh_cert_parse(blob, n, &ct, why, sizeof why) < 0)
        return JS_NULL;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, ct.type == SSH_CERT_USER ? "user" : "host"));
    JS_SetPropertyStr(ctx, o, "keyId", JS_NewStringLen(ctx, (const char*)ct.key_id, ct.key_id_len));
    JS_SetPropertyStr(ctx, o, "serial", u64_val(ctx, ct.serial));
    arr = JS_NewArray(ctx);
    sshr_init(&r, ct.principals, ct.principals_len);
    while (r.off < r.len) {
        size_t pn;
        const uint8_t* p = sshr_str(&r, &pn);
        if (r.err)
            break;
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, (const char*)p, pn));
    }
    JS_SetPropertyStr(ctx, o, "principals", arr);
    JS_SetPropertyStr(ctx, o, "validAfter", u64_val(ctx, ct.valid_after));
    JS_SetPropertyStr(ctx, o, "validBefore", u64_val(ctx, ct.valid_before));
    ssh_fingerprint(ct.ca, ct.ca_len, fp);
    JS_SetPropertyStr(ctx, o, "caFingerprint", JS_NewString(ctx, fp));
    return o;
}

static JSValue auth_settle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    DynResource* r = (DynResource*)JS_GetOpaque(data[0], ssh_server_cid);
    ssh_srv_t* s;
    ssh_conn_t* c;
    double cid = 0, seq = 0;
    (void)this_val;
    if (!r || r->closed || !r->native)
        return JS_UNDEFINED;
    s = (ssh_srv_t*)r->native;
    JS_ToFloat64(ctx, &cid, data[1]);
    JS_ToFloat64(ctx, &seq, data[2]);
    for (c = s->conns; c; c = c->snext)
        if (c->cid == (uint64_t)cid)
            break;
    if (!c || c->state == SSH_ST_DEAD)
        return JS_UNDEFINED;
    c->busy++;
    ssh_srv_auth_done(c, (uint32_t)seq, magic == 1 && argc > 0 && JS_IsBool(argv[0]) && JS_ToBool(ctx, argv[0]));
    conn_exit(c);
    return JS_UNDEFINED;
}

int ssh_then2(JSContext* ctx, JSValueConst p, JSCFunctionData* fn, int nd, JSValueConst* d)
{
    JSValue then = JS_GetPropertyStr(ctx, p, "then"), fns[2], r;
    int rc = -1;
    if (JS_IsException(then)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    if (!JS_IsFunction(ctx, then)) {
        JS_FreeValue(ctx, then);
        return -1;
    }
    fns[0] = JS_NewCFunctionData(ctx, fn, 1, 1, nd, d);
    fns[1] = JS_NewCFunctionData(ctx, fn, 1, 0, nd, d);
    r = JS_Call(ctx, then, p, 2, (JSValueConst*)fns);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        rc = 0;
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fns[0]);
    JS_FreeValue(ctx, fns[1]);
    JS_FreeValue(ctx, then);
    return rc;
}

int ssh_js_authorize(ssh_conn_t* c, const char* user, const char* alg, const uint8_t* key,
    size_t keylen, const uint8_t* certblob, size_t certlen, int listed)
{
    ssh_srv_t* s = c->srv;
    JSContext* ctx = s->ctx;
    JSValue info, ret;
    int rc = 0;
    if (ssh_no_js || !JS_IsFunction(ctx, s->authorize))
        return 0;
    info = JS_NewObject(ctx);
    if (JS_IsException(info)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_SetPropertyStr(ctx, info, "username", JS_NewString(ctx, user));
    JS_SetPropertyStr(ctx, info, "keyType", JS_NewString(ctx, alg));
    JS_SetPropertyStr(ctx, info, "fingerprint", JS_NewString(ctx, c->ufp));
    JS_SetPropertyStr(ctx, info, "publicKey", key_line(ctx, key, keylen));
    JS_SetPropertyStr(ctx, info, "listed", JS_NewBool(ctx, listed));
    JS_SetPropertyStr(ctx, info, "certificate", certblob ? cert_info(ctx, certblob, certlen) : JS_NULL);
    JS_SetPropertyStr(ctx, info, "remoteAddress", JS_NewString(ctx, c->peer));
    ret = JS_Call(ctx, s->authorize, JS_UNDEFINED, 1, (JSValueConst*)&info);
    JS_FreeValue(ctx, info);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (JS_IsBool(ret))
        rc = JS_ToBool(ctx, ret) ? 1 : 0;
    else if (JS_IsObject(ret)) {
        JSValue d[3];
        d[0] = s->self_weak;
        d[1] = JS_NewFloat64(ctx, (double)c->cid);
        d[2] = JS_NewFloat64(ctx, (double)++c->auth_seq);
        c->auth_pending = 1;
        if (ssh_then2(ctx, ret, auth_settle, 3, (JSValueConst*)d) == 0)
            rc = 2;
        else
            c->auth_pending = 0;
    }
    JS_FreeValue(ctx, ret);
    return rc;
}

static void sess_kick(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c)
        return;
    c->busy++;
    ssh_chan_window(ch);
    ssh_chan_pump(ch);
    ssh_flush(c);
    conn_exit(c);
}

static void sess_set_exit(ssh_chan_t* ch, uint32_t code)
{
    if (ch->want_exit)
        return;
    ch->want_eof = ch->want_exit = 1;
    ch->exit_kind = 1;
    ch->exit_code = (int)code;
    ch->has_status = 1;
}

static void errq_text(ssh_chan_t* ch, JSContext* ctx, JSValueConst e)
{
    const char* m = JS_ToCString(ctx, e);
    if (!m) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (sshb_put(&ch->errq, m, strlen(m)) == 0 && sshb_u8(&ch->errq, '\n') == 0)
        ch->err_total += strlen(m) + 1;
    JS_FreeCString(ctx, m);
}

static JSValue sess_settle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    ssh_chan_t* ch = (ssh_chan_t*)JS_GetOpaque(data[0], ssh_session_cid);
    (void)this_val;
    if (!ch || !ch->conn || ch->want_exit || ch->sent_close)
        return JS_UNDEFINED;
    if (!magic && argc > 0)
        errq_text(ch, ctx, argv[0]);
    sess_set_exit(ch, magic ? 0 : 1);
    sess_kick(ch);
    return JS_UNDEFINED;
}

static void set_str(JSContext* ctx, JSValueConst o, const char* k, const char* v)
{
    JS_DefinePropertyValueStr(ctx, o, k, v ? JS_NewString(ctx, v) : JS_NULL, JS_PROP_ENUMERABLE);
}

int ssh_js_session(ssh_conn_t* c, ssh_chan_t* ch, const char* kind, const uint8_t* cmd, size_t cmdlen)
{
    ssh_srv_t* s = c->srv;
    JSContext* ctx = s->ctx;
    JSValue obj, rs, env, ep, f[2], ret, cmdv;
    sshr_t r;
    if (ssh_no_js || !JS_IsFunction(ctx, s->on_session))
        return -1;
    obj = JS_NewObjectClass(ctx, (int)ssh_session_cid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JS_SetOpaque(obj, ch);
    ch->refs++;
    ch->jsobj = obj;
    ch->js_held = 1;
    rs = make_rs(ctx, obj, 0, c->cfg.window);
    if (JS_IsException(rs)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JS_DefinePropertyValueStr(ctx, obj, "stdin", rs, JS_PROP_ENUMERABLE);
    set_str(ctx, obj, "user", c->user);
    set_str(ctx, obj, "type", kind);
    cmdv = cmd ? JS_NewStringLen(ctx, (const char*)cmd, cmdlen) : JS_NULL;
    JS_DefinePropertyValueStr(ctx, obj, "command", c->force_cmd ? JS_NewString(ctx, c->force_cmd) : JS_DupValue(ctx, cmdv), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, obj, "requestedCommand", cmdv, JS_PROP_ENUMERABLE);
    env = JS_NewObject(ctx);
    sshr_init(&r, ch->env.p, ch->env.len);
    while (r.off < r.len) {
        size_t nn, vn;
        const uint8_t* n = sshr_str(&r, &nn);
        const uint8_t* v = sshr_str(&r, &vn);
        char key[72];
        if (r.err || nn >= sizeof key)
            break;
        memcpy(key, n, nn);
        key[nn] = 0;
        JS_SetPropertyStr(ctx, env, key, JS_NewStringLen(ctx, (const char*)v, vn));
    }
    JS_DefinePropertyValueStr(ctx, obj, "env", env, JS_PROP_ENUMERABLE);
    if (ch->has_pty) {
        JSValue p = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, p, "term", JS_NewString(ctx, ch->term ? ch->term : ""));
        JS_SetPropertyStr(ctx, p, "cols", JS_NewUint32(ctx, ch->cols));
        JS_SetPropertyStr(ctx, p, "rows", JS_NewUint32(ctx, ch->rows));
        JS_SetPropertyStr(ctx, p, "width", JS_NewUint32(ctx, ch->width));
        JS_SetPropertyStr(ctx, p, "height", JS_NewUint32(ctx, ch->height));
        JS_DefinePropertyValueStr(ctx, obj, "pty", p, JS_PROP_ENUMERABLE);
    } else
        JS_DefinePropertyValueStr(ctx, obj, "pty", JS_NULL, JS_PROP_ENUMERABLE);
    set_str(ctx, obj, "remoteAddress", c->peer);
    set_str(ctx, obj, "keyType", c->ukt);
    set_str(ctx, obj, "keyFingerprint", c->ufp);
    JS_DefinePropertyValueStr(ctx, obj, "certificate", c->ucert.len ? cert_info(ctx, c->ucert.p, c->ucert.len) : JS_NULL, JS_PROP_ENUMERABLE);
    ep = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(ep)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    ch->exit_res = f[0];
    JS_FreeValue(ctx, f[1]);
    JS_DefinePropertyValueStr(ctx, obj, "done", ep, JS_PROP_ENUMERABLE);
    ret = JS_Call(ctx, s->on_session, JS_UNDEFINED, 1, (JSValueConst*)&obj);
    if (JS_IsException(ret)) {
        JSValue e = JS_GetException(ctx);
        if (ch->conn && !ch->want_exit)
            errq_text(ch, ctx, e);
        JS_FreeValue(ctx, e);
        return -1;
    }
    if (JS_IsObject(ret)) {
        JSValueConst d[1] = { obj };
        (void)ssh_then2(ctx, ret, sess_settle, 1, d);
    }
    JS_FreeValue(ctx, ret);
    return 0;
}

static void call_hook(ssh_chan_t* ch, const char* name, JSValue arg)
{
    ssh_conn_t* c = ch->conn;
    JSContext* ctx;
    JSValue fn;
    if (!c || ssh_no_js || !ch->js_held) {
        if (c)
            JS_FreeValue(c->ctx, arg);
        return;
    }
    ctx = c->ctx;
    fn = JS_GetPropertyStr(ctx, ch->jsobj, name);
    if (JS_IsException(fn))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else if (JS_IsFunction(ctx, fn))
        call1(ctx, fn, arg);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arg);
}

void ssh_js_signal(ssh_chan_t* ch, const char* name)
{
    if (ch->conn)
        call_hook(ch, "onsignal", JS_NewString(ch->conn->ctx, name));
}

void ssh_js_resize(ssh_chan_t* ch)
{
    JSContext* ctx;
    JSValue o;
    if (!ch->conn)
        return;
    ctx = ch->conn->ctx;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "cols", JS_NewUint32(ctx, ch->cols));
    JS_SetPropertyStr(ctx, o, "rows", JS_NewUint32(ctx, ch->rows));
    JS_SetPropertyStr(ctx, o, "width", JS_NewUint32(ctx, ch->width));
    JS_SetPropertyStr(ctx, o, "height", JS_NewUint32(ctx, ch->height));
    call_hook(ch, "onresize", o);
}

static ssh_chan_t* sess_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_chan_t*)JS_GetOpaque2(ctx, v, ssh_session_cid);
}

static JSValue sess_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    ssh_chan_t* ch = sess_get(ctx, this_val);
    if (!ch)
        return JS_EXCEPTION;
    return chan_write(ctx, ch, magic, argc, argv);
}

static JSValue sess_end(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = sess_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!ch)
        return JS_EXCEPTION;
    ch->want_eof = 1;
    sess_kick(ch);
    return JS_UNDEFINED;
}

static JSValue sess_exit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = sess_get(ctx, this_val);
    double d = 0;
    if (!ch)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsNumber(argv[0]) || JS_ToFloat64(ctx, &d, argv[0]) < 0 || !(d >= 0 && d <= 4294967295.0) || d != (double)(uint32_t)d)
            return JS_ThrowRangeError(ctx, "SSH: exit code must be an integer in 0..4294967295");
    }
    if (!ch->conn || ch->want_exit || ch->sent_close)
        return JS_UNDEFINED;
    sess_set_exit(ch, (uint32_t)d);
    sess_kick(ch);
    return JS_UNDEFINED;
}

static JSValue sess_kill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = sess_get(ctx, this_val);
    const char* sig = NULL;
    const char* msg = NULL;
    const char* name;
    size_t i, n;
    if (!ch)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "SSH: kill expects a signal name such as \"TERM\"");
        sig = JS_ToCString(ctx, argv[0]);
        if (!sig)
            return JS_EXCEPTION;
    }
    name = sig ? sig : "TERM";
    if (strncmp(name, "SIG", 3) == 0)
        name += 3;
    n = strlen(name);
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9')))
            break;
    if (n == 0 || n > 20 || i != n) {
        if (sig)
            JS_FreeCString(ctx, sig);
        return JS_ThrowTypeError(ctx, "SSH: kill expects a signal name such as \"TERM\"");
    }
    if (ch->conn && !ch->want_exit && !ch->sent_close) {
        if (argc > 1 && JS_IsString(argv[1]))
            msg = JS_ToCString(ctx, argv[1]);
        free(ch->exit_signal);
        free(ch->exit_msg);
        ch->exit_signal = strdup(name);
        ch->exit_msg = strdup(msg ? msg : "");
        if (msg)
            JS_FreeCString(ctx, msg);
        ch->want_eof = ch->want_exit = 1;
        ch->exit_kind = 2;
        ch->has_status = 2;
        sess_kick(ch);
    }
    if (sig)
        JS_FreeCString(ctx, sig);
    return JS_UNDEFINED;
}

static JSValue sess_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch = sess_get(ctx, this_val);
    ssh_conn_t* c;
    (void)argc;
    (void)argv;
    if (!ch)
        return JS_EXCEPTION;
    c = ch->conn;
    if (!c)
        return JS_UNDEFINED;
    c->busy++;
    ssh_chan_close(ch);
    ssh_flush(c);
    conn_exit(c);
    return JS_UNDEFINED;
}

static const char* const srv_keys[] = { "host", "port", "hostKey", "hostKeyPassphrase", "hostCertificate", "authorizedKeys", "trustedUserCAKeys", "authorize", "onSession", "forward", "banner", "dyna" };
static const char* const srv_meta_keys[] = { "maxConnections", "maxAuthTries", "maxChannels", "windowBytes", "maxPacketBytes", "handshakeTimeoutMs", "idleTimeoutMs", "rekeyBytes", "rekeySeconds", "shrinkBuffers", "publicForwardBinds" };

static int parse_srv_meta(JSContext* ctx, JSValueConst m, ssh_cfg_t* cfg)
{
    uint64_t v;
    if (JS_IsUndefined(m))
        return 0;
    if (!JS_IsObject(m)) {
        JS_ThrowTypeError(ctx, "SSH: dyna must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, m, srv_meta_keys, (int)countof(srv_meta_keys)))
        return -1;
    v = (uint64_t)cfg->max_conns;
    if (opt_num(ctx, m, "maxConnections", 1, 65536, &v) < 0)
        return -1;
    cfg->max_conns = (int)v;
    v = (uint64_t)cfg->max_auth_tries;
    if (opt_num(ctx, m, "maxAuthTries", 1, 100, &v) < 0)
        return -1;
    cfg->max_auth_tries = (int)v;
    v = (uint64_t)cfg->max_channels;
    if (opt_num(ctx, m, "maxChannels", 1, 4096, &v) < 0)
        return -1;
    cfg->max_channels = (int)v;
    v = cfg->window;
    if (opt_num(ctx, m, "windowBytes", 65536, 1073741824, &v) < 0)
        return -1;
    cfg->window = (uint32_t)v;
    v = cfg->max_packet;
    if (opt_num(ctx, m, "maxPacketBytes", 35000, 16777216, &v) < 0)
        return -1;
    cfg->max_packet = (uint32_t)v;
    if (opt_num(ctx, m, "handshakeTimeoutMs", 1, 600000, &cfg->handshake_ms) < 0)
        return -1;
    if (opt_num(ctx, m, "idleTimeoutMs", 0, 604800000, &cfg->idle_ms) < 0)
        return -1;
    if (opt_num(ctx, m, "rekeyBytes", 65536, 4294967296.0, &cfg->rekey_bytes) < 0)
        return -1;
    v = cfg->rekey_ms / 1000;
    if (opt_num(ctx, m, "rekeySeconds", 1, 86400, &v) < 0)
        return -1;
    cfg->rekey_ms = v * 1000;
    if (opt_bool(ctx, m, "publicForwardBinds", &cfg->public_binds) < 0)
        return -1;
    return opt_bool(ctx, m, "shrinkBuffers", &cfg->shrink);
}

static int keys_opt(JSContext* ctx, JSValueConst o, const char* k, sshb_t* list)
{
    int err;
    char why[200];
    char* txt = opt_str(ctx, o, k, &err);
    if (err)
        return -1;
    if (!txt)
        return 0;
    if (ssh_parse_pubkey_lines(txt, strlen(txt), list, why, sizeof why) < 0) {
        free(txt);
        JS_ThrowTypeError(ctx, "SSH: %s: %s", k, why);
        return -1;
    }
    free(txt);
    return 0;
}

static JSValue fn_opt(JSContext* ctx, JSValueConst o, const char* k, int* err)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    *err = 0;
    if (JS_IsException(v)) {
        *err = 1;
        return JS_UNDEFINED;
    }
    if (JS_IsUndefined(v))
        return v;
    if (!JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SSH: options.%s must be a function", k);
        *err = 1;
        return JS_UNDEFINED;
    }
    return v;
}

static JSValue ssh_server_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    (void)nt;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "SSH: use SSHServer.listen(options)");
}

static JSValue ssh_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ssh_srv_t* s;
    JSValue key, pass, meta, port, obj;
    const uint8_t* kp;
    size_t kn = 0, pn = 0;
    const char *kstr = NULL, *pstr = NULL;
    char why[200], *cert;
    int err;
    uint64_t portv = 0;
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    (void)this_val;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SSH: SSHServer.listen expects an options object");
    if (dyn_opts_strict(ctx, o, srv_keys, (int)countof(srv_keys)))
        return JS_EXCEPTION;
    s = (ssh_srv_t*)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->ctx = ctx;
    s->rt = JS_GetRuntime(ctx);
    s->listen_fd = -1;
    s->released = 1;
    s->self_weak = s->authorize = s->on_session = s->forward = JS_UNDEFINED;
    s->cfg.max_packet = 262144;
    s->cfg.window = 2097152;
    s->cfg.chan_packet = 32768;
    s->cfg.max_channels = 10;
    s->cfg.handshake_ms = 30000;
    s->cfg.rekey_bytes = (uint64_t)1 << 30;
    s->cfg.rekey_ms = 3600000;
    s->cfg.max_auth_tries = 6;
    s->cfg.max_conns = 64;
    s->host = opt_str(ctx, o, "host", &err);
    if (err)
        goto fail;
    if (!s->host && !(s->host = strdup("127.0.0.1"))) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    port = JS_GetPropertyStr(ctx, o, "port");
    if (!JS_IsUndefined(port)) {
        double d;
        if (!JS_IsNumber(port) || JS_ToFloat64(ctx, &d, port) < 0 || !(d >= 0 && d <= 65535) || d != (double)(int)d) {
            JS_FreeValue(ctx, port);
            JS_ThrowRangeError(ctx, "SSH: options.port must be an integer in 0..65535");
            goto fail;
        }
        portv = (uint64_t)d;
    }
    JS_FreeValue(ctx, port);
    key = JS_GetPropertyStr(ctx, o, "hostKey");
    pass = JS_GetPropertyStr(ctx, o, "hostKeyPassphrase");
    if (JS_IsUndefined(key) || (!JS_IsUndefined(pass) && !JS_IsString(pass))) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        JS_ThrowTypeError(ctx, "SSH: options.hostKey (an OpenSSH ssh-ed25519 private key) is required; hostKeyPassphrase must be a string");
        goto fail;
    }
    if (bytes_of(ctx, key, &kp, &kn, &kstr) < 0) {
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        goto fail;
    }
    if (JS_IsString(pass))
        pstr = JS_ToCStringLen(ctx, &pn, pass);
    s->hkey = ssh_parse_private_key(kp, kn, pstr, pn, s->hpub, why, sizeof why);
    if (kstr)
        JS_FreeCString(ctx, kstr);
    if (pstr)
        JS_FreeCString(ctx, pstr);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, pass);
    if (!s->hkey) {
        JS_ThrowTypeError(ctx, "SSH: hostKey: %s", why);
        goto fail;
    }
    ssh_ed25519_blob(&s->hblob, s->hpub);
    if (!s->hblob.len) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    cert = opt_str(ctx, o, "hostCertificate", &err);
    if (err)
        goto fail;
    if (cert) {
        char ty[48];
        ssh_cert_t ct;
        int bad = ssh_parse_pubkey(cert, strlen(cert), &s->hcert, ty, sizeof ty) < 0 || strcmp(ty, "ssh-ed25519-cert-v01@openssh.com") != 0;
        free(cert);
        if (bad) {
            JS_ThrowTypeError(ctx, "SSH: hostCertificate must be an ssh-ed25519-cert-v01@openssh.com public key line");
            goto fail;
        }
        if (ssh_cert_parse(s->hcert.p, s->hcert.len, &ct, why, sizeof why) < 0) {
            JS_ThrowTypeError(ctx, "SSH: hostCertificate: %s", why);
            goto fail;
        }
        if (ct.type != SSH_CERT_HOST || memcmp(ct.pk, s->hpub, 32) != 0) {
            JS_ThrowTypeError(ctx, "SSH: hostCertificate is not a host certificate for hostKey");
            goto fail;
        }
    }
    if (keys_opt(ctx, o, "authorizedKeys", &s->akeys) < 0 || keys_opt(ctx, o, "trustedUserCAKeys", &s->cakeys) < 0)
        goto fail;
    s->authorize = fn_opt(ctx, o, "authorize", &err);
    if (err)
        goto fail;
    s->forward = fn_opt(ctx, o, "forward", &err);
    if (err)
        goto fail;
    s->on_session = fn_opt(ctx, o, "onSession", &err);
    if (err)
        goto fail;
    if (JS_IsUndefined(s->on_session)) {
        JS_ThrowTypeError(ctx, "SSH: options.onSession (a function receiving each SSHSession) is required");
        goto fail;
    }
    if (!s->akeys.len && !s->cakeys.len && JS_IsUndefined(s->authorize)) {
        JS_ThrowTypeError(ctx, "SSH: pass authorizedKeys, trustedUserCAKeys or authorize; otherwise nobody can log in");
        goto fail;
    }
    s->banner = opt_str(ctx, o, "banner", &err);
    if (err)
        goto fail;
    if (s->banner && strlen(s->banner) > 65536) {
        JS_ThrowRangeError(ctx, "SSH: options.banner is longer than 64 KiB");
        goto fail;
    }
    meta = JS_GetPropertyStr(ctx, o, "dyna");
    err = parse_srv_meta(ctx, meta, &s->cfg);
    JS_FreeValue(ctx, meta);
    if (err)
        goto fail;
    s->aio = dyn_net_reactor_acquire(ctx);
    if (!s->aio) {
        dyn_net_throw_no_reactor(ctx, "SSHServer");
        goto fail;
    }
    s->released = 0;
    s->listen_fd = dyn_aio_listen(s->aio, s->host, (uint16_t)portv, 512);
    if (s->listen_fd < 0) {
        JS_ThrowInternalError(ctx, "SSH: cannot listen on %s:%u", s->host, (unsigned)portv);
        goto fail;
    }
    s->port = (uint16_t)portv;
    if (getsockname(s->listen_fd, (struct sockaddr*)&sa, &sl) == 0)
        s->port = ntohs(sa.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&sa)->sin6_port : ((struct sockaddr_in*)&sa)->sin_port);
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, ssh_server_cid, s, srv_dispose);
    if (JS_IsException(obj))
        return obj;
    s->self_weak = obj;
    if (dyn_res_pin(ctx, obj, ssh_server_cid) == 0)
        s->pinned = 1;
    if (JS_AddShutdownSweep(s->rt, ssh_srv_sweep, s) == 0)
        s->swept = 1;
    if (dyn_net_on_drain(srv_tick, s) == 0)
        s->hooked = 1;
    if (dyn_aio_accept(s->aio, s->listen_fd, srv_on_accept, s) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx, "SSH: cannot accept on %s:%u", s->host, (unsigned)s->port);
    }
    return obj;
fail:
    srv_free(s);
    return JS_EXCEPTION;
}

static ssh_srv_t* server_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_srv_t*)dyn_res_native(ctx, v, ssh_server_cid);
}

static JSValue srv_get_port(JSContext* ctx, JSValueConst this_val)
{
    ssh_srv_t* s = server_get(ctx, this_val);
    return s ? JS_NewInt32(ctx, s->port) : JS_EXCEPTION;
}

static JSValue srv_get_host(JSContext* ctx, JSValueConst this_val)
{
    ssh_srv_t* s = server_get(ctx, this_val);
    return s ? JS_NewString(ctx, s->host) : JS_EXCEPTION;
}

static JSValue srv_get_stats(JSContext* ctx, JSValueConst this_val)
{
    ssh_srv_t* s = server_get(ctx, this_val);
    JSValue o;
    ssh_conn_t* c;
    int sessions = 0, authed = 0;
    if (!s)
        return JS_EXCEPTION;
    for (c = s->conns; c; c = c->snext) {
        sessions += c->nopen;
        authed += c->state == SSH_ST_READY;
    }
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "accepted", JS_NewFloat64(ctx, (double)s->accepted));
    JS_SetPropertyStr(ctx, o, "refused", JS_NewFloat64(ctx, (double)s->refused));
    JS_SetPropertyStr(ctx, o, "connections", JS_NewInt32(ctx, s->nconns));
    JS_SetPropertyStr(ctx, o, "authenticated", JS_NewInt32(ctx, authed));
    JS_SetPropertyStr(ctx, o, "channels", JS_NewInt32(ctx, sessions));
    return o;
}

static void server_finalizer(JSRuntime* rt, JSValue val)
{
    ssh_no_js++;
    dyn_res_finalizer(rt, val);
    ssh_no_js--;
}

static void server_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, ssh_server_cid);
    ssh_srv_t* s;
    if (!r || r->closed || !r->native)
        return;
    s = (ssh_srv_t*)r->native;
    JS_MarkValue(rt, s->authorize, mark);
    JS_MarkValue(rt, s->on_session, mark);
    JS_MarkValue(rt, s->forward, mark);
}

static const JSClassDef ssh_server_class = {
    "SSHServer",
    .finalizer = server_finalizer,
    .gc_mark = server_mark,
};

static const JSClassDef ssh_session_class = {
    "SSHSession",
    .finalizer = exec_finalizer,
    .gc_mark = exec_mark,
};

static const JSCFunctionListEntry ssh_server_proto[] = {
    JS_CGETSET_DEF("port", srv_get_port, NULL),
    JS_CGETSET_DEF("host", srv_get_host, NULL),
    JS_CGETSET_DEF("stats", srv_get_stats, NULL),
};

static const JSCFunctionListEntry ssh_session_proto[] = {
    JS_CFUNC_MAGIC_DEF("write", 1, sess_write, 0),
    JS_CFUNC_MAGIC_DEF("writeStderr", 1, sess_write, 1),
    JS_CFUNC_DEF("end", 0, sess_end),
    JS_CFUNC_DEF("exit", 1, sess_exit),
    JS_CFUNC_DEF("kill", 2, sess_kill),
    JS_CFUNC_DEF("close", 0, sess_close),
};

static JSClassID ssh_agent_cid;

typedef struct {
    ssh_agent_srv_t a;
    JSRuntime* rt;
    int pinned;
    int swept;
    int released;
    JSValue self_weak;
} ssh_agent_js_t;

static void ssh_agent_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void agent_shutdown(ssh_agent_js_t* g)
{
    ssh_agent_srv_close(&g->a);
    if (g->a.aio && !g->released) {
        g->released = 1;
        dyn_net_reactor_release_rt(g->rt);
    }
}

static void agent_dispose(void* native)
{
    ssh_agent_js_t* g = (ssh_agent_js_t*)native;
    if (!g)
        return;
    g->pinned = 0;
    if (g->swept) {
        g->swept = 0;
        JS_RemoveShutdownSweep(g->rt, ssh_agent_sweep, g);
    }
    agent_shutdown(g);
    OPENSSL_cleanse(g, sizeof *g);
    free(g);
}

static void ssh_agent_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    ssh_agent_js_t* g = (ssh_agent_js_t*)opaque;
    (void)ctx;
    (void)rt;
    g->swept = 0;
    agent_shutdown(g);
}

static ssh_agent_js_t* agent_get(JSContext* ctx, JSValueConst v)
{
    return (ssh_agent_js_t*)dyn_res_native(ctx, v, ssh_agent_cid);
}

static const char* const agent_keys[] = { "path", "keys", "dyna" };
static const char* const agent_meta_keys[] = { "maxKeys", "maxConnections" };
static const char* const agent_add_keys[] = { "privateKey", "passphrase", "certificate", "comment", "lifetimeSeconds" };

static JSValue agent_identity(JSContext* ctx, const ssh_akey_t* k)
{
    JSValue o = JS_NewObject(ctx);
    char fp[64];
    static const char* const ty[3] = { "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ssh-ed25519-cert-v01@openssh.com" };
    if (JS_IsException(o))
        return o;
    ssh_fingerprint(k->blob.p, k->blob.len, fp);
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, ty[k->kind]));
    JS_SetPropertyStr(ctx, o, "publicKey", key_line(ctx, k->blob.p, k->blob.len));
    JS_SetPropertyStr(ctx, o, "fingerprint", JS_NewString(ctx, fp));
    JS_SetPropertyStr(ctx, o, "comment", JS_NewString(ctx, k->comment ? k->comment : ""));
    if (k->expires_ms) {
        struct timespec ts;
        uint64_t now = ssh_now_ms();
        clock_gettime(CLOCK_REALTIME, &ts);
        JS_SetPropertyStr(ctx, o, "expiresAt", JS_NewFloat64(ctx, (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000) + (double)(k->expires_ms > now ? k->expires_ms - now : 0)));
    } else
        JS_SetPropertyStr(ctx, o, "expiresAt", JS_NULL);
    JS_SetPropertyStr(ctx, o, "canSign", JS_NewBool(ctx, k->k != NULL));
    return o;
}

static int agent_add_one(JSContext* ctx, ssh_agent_js_t* g, JSValueConst o, JSValue* ident)
{
    JSValue key, pass;
    const uint8_t* kp;
    size_t kn = 0, pn = 0;
    const char *kstr = NULL, *pstr = NULL;
    char why[200], *cert = NULL, *comment = NULL;
    uint8_t pub[32];
    EVP_PKEY* pk;
    sshb_t blob = { 0 };
    ssh_akey_t* k;
    uint64_t life = 0;
    int err, kind = 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "SSH: SSHAgent.add expects { privateKey, passphrase?, certificate?, comment?, lifetimeSeconds? }");
        return -1;
    }
    if (dyn_opts_strict(ctx, o, agent_add_keys, (int)countof(agent_add_keys)))
        return -1;
    if (opt_num(ctx, o, "lifetimeSeconds", 1, 31536000, &life) < 0)
        return -1;
    comment = opt_str(ctx, o, "comment", &err);
    if (err)
        return -1;
    cert = opt_str(ctx, o, "certificate", &err);
    if (err) {
        free(comment);
        return -1;
    }
    key = JS_GetPropertyStr(ctx, o, "privateKey");
    pass = JS_GetPropertyStr(ctx, o, "passphrase");
    if (JS_IsUndefined(key) || (!JS_IsUndefined(pass) && !JS_IsString(pass)) || bytes_of(ctx, key, &kp, &kn, &kstr) < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "SSH: SSHAgent.add: privateKey (OpenSSH ssh-ed25519 key text) is required and passphrase must be a string");
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, pass);
        free(comment);
        free(cert);
        return -1;
    }
    if (JS_IsString(pass))
        pstr = JS_ToCStringLen(ctx, &pn, pass);
    pk = ssh_parse_private_key(kp, kn, pstr, pn, pub, why, sizeof why);
    if (kstr)
        JS_FreeCString(ctx, kstr);
    if (pstr)
        JS_FreeCString(ctx, pstr);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, pass);
    if (!pk) {
        free(comment);
        free(cert);
        JS_ThrowTypeError(ctx, "SSH: SSHAgent.add: %s", why);
        return -1;
    }
    if (cert) {
        char ty[48];
        ssh_cert_t ct;
        int bad = ssh_parse_pubkey(cert, strlen(cert), &blob, ty, sizeof ty) < 0 || strcmp(ty, "ssh-ed25519-cert-v01@openssh.com") != 0
            || ssh_cert_parse(blob.p, blob.len, &ct, why, sizeof why) < 0 || ct.type != SSH_CERT_USER || memcmp(ct.pk, pub, 32) != 0;
        free(cert);
        if (bad) {
            EVP_PKEY_free(pk);
            sshb_free(&blob);
            free(comment);
            JS_ThrowTypeError(ctx, "SSH: SSHAgent.add: certificate is not an ssh-ed25519-cert-v01@openssh.com user certificate for privateKey");
            return -1;
        }
        kind = 2;
    } else
        ssh_ed25519_blob(&blob, pub);
    k = ssh_agent_key_from(kind, pk, blob.p, blob.len, comment, comment ? strlen(comment) : 0);
    sshb_free(&blob);
    free(comment);
    if (!k) {
        EVP_PKEY_free(pk);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    k->expires_ms = life ? ssh_now_ms() + life * 1000 : 0;
    if (g->a.st.locked) {
        EVP_PKEY_free(k->k);
        k->k = NULL;
        sshb_free(&k->blob);
        free(k->comment);
        free(k);
        JS_ThrowTypeError(ctx, "SSH: SSHAgent.add: the agent is locked");
        return -1;
    }
    {
        sshb_t bl = { 0 };
        if (sshb_put(&bl, k->blob.p, k->blob.len) < 0) {
            EVP_PKEY_free(k->k);
            sshb_free(&k->blob);
            free(k->comment);
            free(k);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (ssh_agent_store_put(&g->a.st, k) < 0) {
            sshb_free(&bl);
            EVP_PKEY_free(k->k);
            sshb_free(&k->blob);
            free(k->comment);
            free(k);
            JS_ThrowRangeError(ctx, "SSH: SSHAgent.add: the agent holds dyna.maxKeys (%d) keys", g->a.st.max_keys);
            return -1;
        }
        if (ident) {
            ssh_akey_t* f;
            *ident = JS_UNDEFINED;
            for (f = g->a.st.keys; f; f = f->next)
                if (f->blob.len == bl.len && memcmp(f->blob.p, bl.p, bl.len) == 0)
                    *ident = agent_identity(ctx, f);
        }
        sshb_free(&bl);
    }
    return 0;
}

static JSValue ssh_agent_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    (void)nt;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "SSH: use SSHAgent.listen()");
}

static JSValue ssh_agent_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ssh_agent_js_t* g;
    JSValue obj, keys, meta;
    char *path, why[300];
    int err;
    uint64_t mk = 256, mc = 64;
    (void)this_val;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.listen expects { path, keys?, dyna? }");
    if (dyn_opts_strict(ctx, o, agent_keys, (int)countof(agent_keys)))
        return JS_EXCEPTION;
    meta = JS_GetPropertyStr(ctx, o, "dyna");
    if (JS_IsException(meta))
        return meta;
    if (!JS_IsUndefined(meta)) {
        if (!JS_IsObject(meta)) {
            JS_FreeValue(ctx, meta);
            return JS_ThrowTypeError(ctx, "SSH: dyna must be an object");
        }
        if (dyn_opts_strict(ctx, meta, agent_meta_keys, (int)countof(agent_meta_keys)) || opt_num(ctx, meta, "maxKeys", 1, 65536, &mk) < 0
            || opt_num(ctx, meta, "maxConnections", 1, 4096, &mc) < 0) {
            JS_FreeValue(ctx, meta);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, meta);
    path = opt_str(ctx, o, "path", &err);
    if (err)
        return JS_EXCEPTION;
    if (!path || !path[0]) {
        free(path);
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.listen: options.path (the unix socket to create) is required");
    }
    keys = JS_GetPropertyStr(ctx, o, "keys");
    if (JS_IsException(keys)) {
        free(path);
        return keys;
    }
    if (!JS_IsUndefined(keys) && !JS_IsArray(ctx, keys)) {
        free(path);
        JS_FreeValue(ctx, keys);
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.listen: options.keys must be an array of SSHAgent.add options");
    }
    g = (ssh_agent_js_t*)calloc(1, sizeof *g);
    if (!g) {
        free(path);
        JS_FreeValue(ctx, keys);
        return JS_ThrowOutOfMemory(ctx);
    }
    g->rt = JS_GetRuntime(ctx);
    g->released = 1;
    g->a.lfd = -1;
    g->a.st.max_keys = (int)mk;
    g->a.max_conns = (int)mc;
    if (!JS_IsUndefined(keys)) {
        int64_t n = 0, i;
        if (dyn_js_len(ctx, keys, &n)) {
            JS_FreeValue(ctx, keys);
            free(path);
            ssh_agent_store_clear(&g->a.st);
            free(g);
            return JS_EXCEPTION;
        }
        if (n > 65536)
            n = 65536;
        for (i = 0; i < n; i++) {
            JSValue it = JS_GetPropertyUint32(ctx, keys, (uint32_t)i);
            int rc = agent_add_one(ctx, g, it, NULL);
            JS_FreeValue(ctx, it);
            if (rc < 0) {
                JS_FreeValue(ctx, keys);
                free(path);
                ssh_agent_store_clear(&g->a.st);
                free(g);
                return JS_EXCEPTION;
            }
        }
    }
    JS_FreeValue(ctx, keys);
    g->a.aio = dyn_net_reactor_acquire(ctx);
    if (!g->a.aio) {
        free(path);
        ssh_agent_store_clear(&g->a.st);
        free(g);
        dyn_net_throw_no_reactor(ctx, "SSHAgent");
        return JS_EXCEPTION;
    }
    g->released = 0;
    if (ssh_agent_srv_listen(&g->a, path, why, sizeof why) < 0) {
        free(path);
        agent_shutdown(g);
        free(g);
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.listen: %s", why);
    }
    free(path);
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, ssh_agent_cid, g, agent_dispose);
    if (JS_IsException(obj))
        return obj;
    g->self_weak = obj;
    if (dyn_res_pin(ctx, obj, ssh_agent_cid) == 0)
        g->pinned = 1;
    if (JS_AddShutdownSweep(g->rt, ssh_agent_sweep, g) == 0)
        g->swept = 1;
    return obj;
}

static JSValue agent_add(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_agent_js_t* g = agent_get(ctx, this_val);
    JSValue id = JS_UNDEFINED;
    if (!g)
        return JS_EXCEPTION;
    ssh_agent_store_expire(&g->a.st, ssh_now_ms());
    if (agent_add_one(ctx, g, argc > 0 ? argv[0] : JS_UNDEFINED, &id) < 0)
        return JS_EXCEPTION;
    return id;
}

static JSValue agent_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_agent_js_t* g = agent_get(ctx, this_val);
    char* line;
    char ty[48];
    sshb_t blob = { 0 };
    int err, hit;
    JSValue tmp;
    if (!g)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.remove expects a public key line (\"ssh-ed25519 AAAA...\")");
    tmp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tmp, "k", JS_DupValue(ctx, argv[0]));
    line = opt_str(ctx, tmp, "k", &err);
    JS_FreeValue(ctx, tmp);
    if (err || !line)
        return JS_EXCEPTION;
    if (ssh_parse_pubkey(line, strlen(line), &blob, ty, sizeof ty) < 0) {
        free(line);
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.remove: not a public key line");
    }
    free(line);
    if (g->a.st.locked) {
        sshb_free(&blob);
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.remove: the agent is locked");
    }
    hit = ssh_agent_store_remove(&g->a.st, blob.p, blob.len);
    sshb_free(&blob);
    return JS_NewBool(ctx, hit);
}

static JSValue agent_remove_all(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_agent_js_t* g = agent_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!g)
        return JS_EXCEPTION;
    if (g->a.st.locked)
        return JS_ThrowTypeError(ctx, "SSH: SSHAgent.removeAll: the agent is locked");
    ssh_agent_store_clear(&g->a.st);
    return JS_UNDEFINED;
}

static JSValue agent_list(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_agent_js_t* g = agent_get(ctx, this_val);
    JSValue arr;
    ssh_akey_t* k;
    uint32_t i = 0;
    (void)argc;
    (void)argv;
    if (!g)
        return JS_EXCEPTION;
    ssh_agent_store_expire(&g->a.st, ssh_now_ms());
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || g->a.st.locked)
        return arr;
    for (k = g->a.st.keys; k; k = k->next)
        JS_SetPropertyUint32(ctx, arr, i++, agent_identity(ctx, k));
    return arr;
}

static JSValue agent_get_prop(JSContext* ctx, JSValueConst this_val, int magic)
{
    ssh_agent_js_t* g = agent_get(ctx, this_val);
    JSValue o;
    if (!g)
        return JS_EXCEPTION;
    if (magic == 0)
        return g->a.path ? JS_NewString(ctx, g->a.path) : JS_NULL;
    if (magic == 1)
        return JS_NewBool(ctx, g->a.st.locked);
    ssh_agent_store_expire(&g->a.st, ssh_now_ms());
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "keys", JS_NewInt32(ctx, g->a.st.nkeys));
    JS_SetPropertyStr(ctx, o, "connections", JS_NewInt32(ctx, g->a.nconns));
    JS_SetPropertyStr(ctx, o, "accepted", JS_NewFloat64(ctx, (double)g->a.accepted));
    JS_SetPropertyStr(ctx, o, "refusedConnections", JS_NewFloat64(ctx, (double)g->a.refused_conns));
    JS_SetPropertyStr(ctx, o, "requests", JS_NewFloat64(ctx, (double)g->a.st.requests));
    JS_SetPropertyStr(ctx, o, "signatures", JS_NewFloat64(ctx, (double)g->a.st.signs));
    JS_SetPropertyStr(ctx, o, "offloaded", JS_NewFloat64(ctx, (double)g->a.offloaded));
    JS_SetPropertyStr(ctx, o, "refused", JS_NewFloat64(ctx, (double)g->a.st.refused));
    return o;
}

static void agent_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_res_finalizer(rt, val);
}

static const JSClassDef ssh_agent_class = {
    "SSHAgent",
    .finalizer = agent_finalizer,
};

static const JSCFunctionListEntry ssh_agent_proto[] = {
    JS_CFUNC_DEF("add", 1, agent_add),
    JS_CFUNC_DEF("remove", 1, agent_remove),
    JS_CFUNC_DEF("removeAll", 0, agent_remove_all),
    JS_CFUNC_DEF("list", 0, agent_list),
    JS_CGETSET_MAGIC_DEF("path", agent_get_prop, NULL, 0),
    JS_CGETSET_MAGIC_DEF("locked", agent_get_prop, NULL, 1),
    JS_CGETSET_MAGIC_DEF("stats", agent_get_prop, NULL, 2),
};

static int ssh_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    if (dyn_register_class(ctx, m, &ssh_client_cid, &ssh_client_class, ssh_client_proto,
            (int)countof(ssh_client_proto), ssh_client_ctor, "SSHClient")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, ssh_client_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsFunction(ctx, ctor))
        JS_SetPropertyStr(ctx, ctor, "connect", JS_NewCFunction(ctx, ssh_connect, "connect", 1));
    JS_FreeValue(ctx, ctor);
    JS_NewClassID(&ssh_exec_cid);
    if (JS_NewClass(rt, ssh_exec_cid, &ssh_exec_class) == 0) {
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, ssh_exec_proto, (int)countof(ssh_exec_proto));
        JS_SetClassProto(ctx, ssh_exec_cid, proto);
    }
    if (dyn_register_class(ctx, m, &ssh_server_cid, &ssh_server_class, ssh_server_proto,
            (int)countof(ssh_server_proto), ssh_server_ctor, "SSHServer")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, ssh_server_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsFunction(ctx, ctor))
        JS_SetPropertyStr(ctx, ctor, "listen", JS_NewCFunction(ctx, ssh_listen, "listen", 1));
    JS_FreeValue(ctx, ctor);
    if (dyn_register_class(ctx, m, &ssh_agent_cid, &ssh_agent_class, ssh_agent_proto,
            (int)countof(ssh_agent_proto), ssh_agent_ctor, "SSHAgent")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, ssh_agent_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsFunction(ctx, ctor))
        JS_SetPropertyStr(ctx, ctor, "listen", JS_NewCFunction(ctx, ssh_agent_listen, "listen", 1));
    JS_FreeValue(ctx, ctor);
    JS_NewClassID(&ssh_fwd_cid);
    if (JS_NewClass(rt, ssh_fwd_cid, &ssh_fwd_class) == 0) {
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, ssh_fwd_proto, (int)countof(ssh_fwd_proto));
        JS_SetClassProto(ctx, ssh_fwd_cid, proto);
    }
    JS_NewClassID(&ssh_session_cid);
    if (JS_NewClass(rt, ssh_session_cid, &ssh_session_class) == 0) {
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, ssh_session_proto, (int)countof(ssh_session_proto));
        JS_SetClassProto(ctx, ssh_session_cid, proto);
    }
    return 0;
}

int js_nat_init_ssh(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:ssh", ssh_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "SSHClient");
    JS_AddModuleExport(ctx, m, "SSHServer");
    JS_AddModuleExport(ctx, m, "SSHAgent");
    return 0;
}

#endif
