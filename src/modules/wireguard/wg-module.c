#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-mod.h"
#include "../../dyna-libc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

JSClassID wgm_class_id;
_Thread_local int wgm_in_final;

static const char* const create_keys[] = { "privateKey", "listenPort", "address", "peers", "tun", "mtu" };
static const char* const peer_keys[] = { "publicKey", "presharedKey", "endpoint", "allowedIPs", "persistentKeepalive" };

typedef struct {
    int v6;
    uint8_t a[16];
    unsigned cidr;
} wgm_aip_t;

typedef struct {
    uint8_t pub[32], psk[32];
    int has_psk;
    struct sockaddr_in ep;
    int has_ep;
    unsigned keep, naip;
    wgm_aip_t* aip;
} wgm_pspec_t;

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    JSValue res, rej;
    uint8_t priv[32];
    wg_static_t s;
    int rc;
    uint16_t port;
    wg_ipaddr_t* addr;
    unsigned naddr, npeers;
    wgm_pspec_t* peers;
    wg_tun_t* tun;
    char tun_name[16];
} wgm_cjob_t;

wgm_t* wgm_of(JSValueConst obj)
{
    DynResource* r = (DynResource*)JS_GetOpaque(obj, wgm_class_id);
    return r && !r->closed ? (wgm_t*)r->native : NULL;
}

JSValue wgm_err(JSContext* ctx, const char* fmt, ...)
{
    va_list ap;
    JSValue m, e = JS_NewError(ctx);
    va_start(ap, fmt);
    m = JS_NewStringVF(ctx, fmt, ap);
    va_end(ap);
    JS_DefinePropertyValueStr(ctx, e, "message", m, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return e;
}

static void call_fn(JSContext* ctx, JSValueConst fn, JSValueConst v)
{
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &v);
    JS_FreeValue(ctx, r);
}

void wgm_settle(wgm_t* w, JSValue* res, JSValue* rej, int reject, JSValue v)
{
    JSValue fn = reject ? *rej : *res;
    if (!wgm_in_final && JS_IsFunction(w->ctx, fn))
        call_fn(w->ctx, fn, v);
    JS_FreeValueRT(w->rt, *res);
    JS_FreeValueRT(w->rt, *rej);
    *res = *rej = JS_UNDEFINED;
    JS_FreeValueRT(w->rt, v);
}

void wgm_call_error(wgm_t* w)
{
    js_std_set_job_error_rt(w->rt, JS_GetException(w->ctx));
}

int wgm_slot_add(wgm_t* w, int kind, void* p, uint32_t* id)
{
    uint32_t i;
    for (i = 0; i < w->nslots; i++)
        if (!w->slots[i].p)
            break;
    if (i == w->nslots) {
        if (w->nslots == w->cap) {
            uint32_t nc = w->cap ? w->cap * 2 : 16;
            wgm_slot_t* ns = (wgm_slot_t*)realloc(w->slots, nc * sizeof *ns);
            if (!ns)
                return -1;
            memset(ns + w->cap, 0, (nc - w->cap) * sizeof *ns);
            w->slots = ns;
            w->cap = nc;
        }
        w->nslots++;
    }
    w->slots[i].p = p;
    w->slots[i].kind = kind;
    w->slots[i].gen = (w->slots[i].gen + 1) & 0xFFFu;
    *id = (w->slots[i].gen << 20) | i;
    return 0;
}

void* wgm_slot_get(wgm_t* w, int kind, uint32_t id)
{
    uint32_t i = id & 0xFFFFFu;
    if (!w || i >= w->nslots || !w->slots[i].p || w->slots[i].kind != kind || (w->slots[i].gen << 20) != (id & 0xFFF00000u))
        return NULL;
    return w->slots[i].p;
}

void wgm_slot_del(wgm_t* w, uint32_t id)
{
    uint32_t i = id & 0xFFFFFu;
    if (i < w->nslots)
        w->slots[i].p = NULL;
}

int wgm_bytes(JSContext* ctx, JSValueConst v, const uint8_t** p, size_t* n, const char** cstr, const char* who)
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
        if ((!base && len) || off > abn || len > abn - off) {
            JS_ThrowTypeError(ctx, "%s: detached or out-of-bounds view", who);
            return -1;
        }
        *p = base ? base + off : (const uint8_t*)"";
        *n = len;
        return 0;
    }
    JS_ThrowTypeError(ctx, "%s: expected a string, ArrayBuffer or ArrayBufferView", who);
    return -1;
}

int wgm_ip_arg(JSContext* ctx, JSValueConst v, wg_ipaddr_t* a, const char* who)
{
    size_t n;
    const char* s;
    int rc;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "%s: host must be an IP literal string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    memset(a, 0, sizeof *a);
    rc = wg_conf_ip(s, n, &a->v6, a->a);
    if (rc < 0)
        JS_ThrowTypeError(ctx, "%s: '%s' is not an IP literal", who, s);
    JS_FreeCString(ctx, s);
    return rc;
}

int wgm_port_arg(JSContext* ctx, JSValueConst v, int lo, uint16_t* port, const char* who)
{
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= lo && d <= 65535) || d != (double)(int)d) {
        JS_ThrowTypeError(ctx, "%s: port must be an integer in %d..65535", who, lo);
        return -1;
    }
    *port = (uint16_t)d;
    return 0;
}

JSValue wgm_ip_str(JSContext* ctx, const wg_ipaddr_t* a)
{
    char b[48];
    wg_conf_fmt_ip(a->v6, a->a, b);
    return JS_NewString(ctx, b);
}

void wgm_kick(wgm_t* w)
{
    if (w && w->g && !w->closing)
        wg_glue_kick(w->g);
}

static int str_prop(JSContext* ctx, JSValueConst o, const char* k, const char** s, size_t* n, int required, const char* who)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    *s = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        if (required) {
            JS_ThrowTypeError(ctx, "%s: %s is required", who, k);
            return -1;
        }
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a string", who, k);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    JS_FreeValue(ctx, v);
    return *s ? 1 : -1;
}

static int key_prop(JSContext* ctx, JSValueConst o, const char* k, uint8_t out[32], int required, const char* who)
{
    const char* s;
    size_t n;
    int rc = str_prop(ctx, o, k, &s, &n, required, who);
    if (rc <= 0)
        return rc;
    if (wg_conf_key(s, n, out) < 0) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "%s: %s must be base64 of 32 bytes", who, k);
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 1;
}

static int uint_prop(JSContext* ctx, JSValueConst o, const char* k, unsigned lo, unsigned hi, unsigned* out, const char* who)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= lo && d <= hi) || d != (double)(unsigned)d) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be an integer in %u..%u", who, k, lo, hi);
        return -1;
    }
    *out = (unsigned)d;
    return 1;
}

static int str_list(JSContext* ctx, JSValueConst o, const char* k, int required, const char* who, int (*each)(JSContext*, const char*, size_t, void*, const char*), void* ud)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k), e;
    uint32_t i, len = 0;
    int rc = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) && !required)
        return 0;
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be an array of strings", who, k);
        return -1;
    }
    if (dyn_js_len_u32(ctx, v, &len)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (required && !len) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must not be empty", who, k);
        return -1;
    }
    for (i = 0; i < len && !rc; i++) {
        const char* s;
        size_t n;
        e = JS_GetPropertyUint32(ctx, v, i);
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "%s: %s[%u] must be a string", who, k, i);
            rc = -1;
            break;
        }
        s = JS_ToCStringLen(ctx, &n, e);
        JS_FreeValue(ctx, e);
        if (!s) {
            rc = -1;
            break;
        }
        rc = each(ctx, s, n, ud, who);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static int each_aip(JSContext* ctx, const char* s, size_t n, void* ud, const char* who)
{
    wgm_pspec_t* ps = (wgm_pspec_t*)ud;
    wgm_aip_t* na = (wgm_aip_t*)realloc(ps->aip, (ps->naip + 1) * sizeof *na);
    if (!na) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    ps->aip = na;
    if (wg_conf_cidr(s, n, &na[ps->naip].v6, na[ps->naip].a, &na[ps->naip].cidr) < 0) {
        JS_ThrowTypeError(ctx, "%s: allowedIPs entry '%.*s' is not a CIDR", who, (int)(n > 64 ? 64 : n), s);
        return -1;
    }
    ps->naip++;
    return 0;
}

static int parse_peer(JSContext* ctx, JSValueConst o, wgm_pspec_t* ps, const char* who)
{
    const char* s;
    size_t n;
    int rc;
    memset(ps, 0, sizeof *ps);
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "%s: a peer must be an object", who);
        return -1;
    }
    if (dyn_opts_strict(ctx, o, peer_keys, (int)countof(peer_keys)))
        return -1;
    if (key_prop(ctx, o, "publicKey", ps->pub, 1, who) < 0)
        return -1;
    rc = key_prop(ctx, o, "presharedKey", ps->psk, 0, who);
    if (rc < 0)
        return -1;
    ps->has_psk = rc;
    rc = str_prop(ctx, o, "endpoint", &s, &n, 0, who);
    if (rc < 0)
        return -1;
    if (rc) {
        int v6;
        uint8_t a[16];
        uint16_t port;
        if (wg_conf_endpoint(s, n, &v6, a, &port) < 0 || v6) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "%s: endpoint must be \"a.b.c.d:port\" (an IPv4 literal; IPv6 underlay is not supported)", who);
            return -1;
        }
        JS_FreeCString(ctx, s);
        ps->ep.sin_family = AF_INET;
        ps->ep.sin_port = htons(port);
        memcpy(&ps->ep.sin_addr, a, 4);
        ps->has_ep = 1;
    }
    if (uint_prop(ctx, o, "persistentKeepalive", 0, 65535, &ps->keep, who) < 0)
        return -1;
    if (str_list(ctx, o, "allowedIPs", 1, who, each_aip, ps) < 0) {
        free(ps->aip);
        ps->aip = NULL;
        return -1;
    }
    return 0;
}

static void pspec_free(wgm_pspec_t* ps)
{
    free(ps->aip);
    wg_wipe(ps, sizeof *ps);
}

static wgm_peer_t* peer_find(wgm_t* w, const uint8_t pub[32])
{
    wg_dpeer_t* p = wg_dev_peer(wg_glue_dev(w->g), pub);
    return p ? (wgm_peer_t*)wg_dev_peer_ud(p) : NULL;
}

static void peer_unlink(wgm_t* w, wgm_peer_t* m)
{
    if (!m->linked)
        return;
    m->linked = 0;
    if (m->prev)
        m->prev->next = m->next;
    else
        w->peers = m->next;
    if (m->next)
        m->next->prev = m->prev;
    m->next = m->prev = NULL;
    w->npeers--;
}

static void create_fail(wgm_t* w, JSValue err);
static void create_step(wgm_t* w);

static void peer_settle(wgm_t* w, wgm_peer_t* m, int rc)
{
    if (rc < 0) {
        char b[48];
        wg_conf_key_b64(m->pub, b);
        peer_unlink(w, m);
        if (m->initial)
            create_fail(w, wgm_err(w->ctx, "WireGuard: peer %s: low-order public key (X25519 result is zero)", b));
        else
            wgm_settle(w, &m->res, &m->rej, 1, wgm_err(w->ctx, "WireGuard: peer %s: low-order public key (X25519 result is zero)", b));
        free(m);
        return;
    }
    if (m->initial) {
        m->initial = 0;
        create_step(w);
    } else
        wgm_settle(w, &m->res, &m->rej, 0, JS_UNDEFINED);
}

static void on_peer_ready(void* ud, wg_dpeer_t* p, int rc)
{
    wgm_peer_t* m = (wgm_peer_t*)ud;
    m->pending = 0;
    m->p = rc < 0 ? NULL : p;
    if (m->initial < 0 || !m->w || m->w->closing)
        return;
    peer_settle(m->w, m, rc);
}

static int peer_add(wgm_t* w, wgm_pspec_t* ps, wgm_peer_t* m)
{
    unsigned i;
    wg_dpeer_t* p;
    int initial = m->initial;
    m->w = w;
    memcpy(m->pub, ps->pub, 32);
    m->pending = 1;
    m->initial = -1;
    m->prev = NULL;
    m->next = w->peers;
    if (w->peers)
        w->peers->prev = m;
    w->peers = m;
    m->linked = 1;
    w->npeers++;
    p = wg_dev_add_peer(wg_glue_dev(w->g), ps->pub, ps->has_psk ? ps->psk : NULL, ps->keep, on_peer_ready, m);
    m->initial = initial;
    if (!p) {
        if (!m->pending)
            return 1;
        peer_unlink(w, m);
        return -1;
    }
    if (!m->pending) {
        m->p = p;
        for (i = 0; i < ps->naip; i++)
            wg_dev_allow(wg_glue_dev(w->g), p, ps->aip[i].v6, ps->aip[i].a, ps->aip[i].cidr);
        if (ps->has_ep)
            wg_dev_set_endpoint(p, (const struct sockaddr*)&ps->ep, sizeof ps->ep);
        return 2;
    }
    m->p = p;
    if (ps->has_ep)
        wg_dev_set_endpoint(p, (const struct sockaddr*)&ps->ep, sizeof ps->ep);
    for (i = 0; i < ps->naip; i++)
        wg_dev_allow(wg_glue_dev(w->g), p, ps->aip[i].v6, ps->aip[i].a, ps->aip[i].cidr);
    return 0;
}

static void reject_all(wgm_t* w, const char* msg)
{
    wgm_peer_t* m;
    for (m = w->peers; m; m = m->next)
        if (m->pending && !m->initial && !JS_IsUndefined(m->rej))
            wgm_settle(w, &m->res, &m->rej, 1, wgm_err(w->ctx, "%s", msg));
    if (w->in_create) {
        w->in_create = 0;
        wgm_settle(w, &w->cres, &w->crej, 1, wgm_err(w->ctx, "%s", msg));
    }
}

static void wgm_free(wgm_t* w)
{
    wgm_peer_t* m;
    while ((m = w->peers) != NULL) {
        w->peers = m->next;
        JS_FreeValueRT(w->rt, m->res);
        JS_FreeValueRT(w->rt, m->rej);
        free(m);
    }
    JS_FreeValueRT(w->rt, w->cres);
    JS_FreeValueRT(w->rt, w->crej);
    free(w->slots);
    if (w->holds_reactor)
        dyn_net_reactor_release_rt(w->rt);
    free(w);
}

static void on_glue_freed(void* ud)
{
    wgm_free((wgm_t*)ud);
}

static void wgm_shutdown(wgm_t* w)
{
    if (w->closing)
        return;
    reject_all(w, "WireGuard: closed");
    wgm_conns_shutdown(w);
    w->closing = 1;
    if (wgm_in_final) {
        wg_glue_free(w->g);
        w->g = NULL;
        wgm_free(w);
        return;
    }
    wg_glue_release(w->g, on_glue_freed, w);
}

static void wgm_dispose(void* native)
{
    if (native)
        wgm_shutdown((wgm_t*)native);
}

static void create_fail(wgm_t* w, JSValue err)
{
    JSRuntime* rt = w->rt;
    JSValue obj = JS_DupValue(w->ctx, w->obj);
    DynResource* r = (DynResource*)JS_GetOpaque(obj, wgm_class_id);
    w->in_create = 0;
    wgm_settle(w, &w->cres, &w->crej, 1, err);
    if (r && !r->closed) {
        r->closed = 1;
        r->native = NULL;
        dyn_res_unpin(rt, obj, wgm_class_id);
        wgm_shutdown(w);
    }
    JS_FreeValueRT(rt, obj);
}

static void create_step(wgm_t* w)
{
    if (w->in_create && --w->create_left == 0) {
        w->in_create = 0;
        wgm_settle(w, &w->cres, &w->crej, 0, JS_DupValue(w->ctx, w->obj));
    }
}

static void cjob_free(wgm_cjob_t* j)
{
    unsigned i;
    for (i = 0; i < j->npeers; i++)
        pspec_free(&j->peers[i]);
    free(j->peers);
    free(j->addr);
    wg_wipe(j->priv, sizeof j->priv);
    free(j);
}

static void cjob_work(void* arg)
{
    wgm_cjob_t* j = (wgm_cjob_t*)arg;
    j->rc = wg_static_init(&j->s, j->priv);
    wg_wipe(j->priv, sizeof j->priv);
}

static void cjob_done(void* arg)
{
    wgm_cjob_t* j = (wgm_cjob_t*)arg;
    JSContext* ctx = j->ctx;
    JSRuntime* rt = JS_GetRuntime(ctx);
    wgm_t* w;
    JSValue obj;
    unsigned i;
    int e;
    if (j->rc < 0) {
        call_fn(ctx, j->rej, wgm_err(ctx, "WireGuard: privateKey is not a usable X25519 key"));
        goto fail;
    }
    w = (wgm_t*)calloc(1, sizeof *w);
    if (!w) {
        wg_static_free(&j->s);
        call_fn(ctx, j->rej, wgm_err(ctx, "WireGuard: out of memory"));
        goto fail;
    }
    w->ctx = ctx;
    w->rt = rt;
    w->aio = j->aio;
    w->obj = w->cres = w->crej = JS_UNDEFINED;
    errno = 0;
    w->g = wg_glue_new_static(j->aio, &j->s, "0.0.0.0", j->port, j->tun);
    e = errno;
    if (!w->g) {
        free(w);
        call_fn(ctx, j->rej, wgm_err(ctx, "WireGuard: cannot bind UDP port %u: %s", j->port, e ? strerror(e) : "failed"));
        goto fail;
    }
    j->tun = NULL;
    w->holds_reactor = 1;
    j->aio = NULL;
    memcpy(w->pub, wg_dev_pub(wg_glue_dev(w->g)), 32);
    w->port = (uint16_t)wg_dev_port(wg_glue_dev(w->g));
    memcpy(w->tun, j->tun_name, sizeof w->tun);
    for (i = 0; i < j->naddr; i++) {
        wg_ip_add_addr(wg_glue_ip(w->g), &j->addr[i]);
        if (j->addr[i].v6)
            w->has6 = 1;
        else
            w->has4 = 1;
    }
    obj = dyn_res_wrap(ctx, JS_UNDEFINED, wgm_class_id, w, wgm_dispose);
    if (JS_IsException(obj)) {
        JSValue ex = JS_GetException(ctx);
        call_fn(ctx, j->rej, ex);
        JS_FreeValue(ctx, ex);
        wg_glue_free(w->g);
        w->g = NULL;
        wgm_free(w);
        goto out;
    }
    w->obj = obj;
    dyn_res_pin(ctx, obj, wgm_class_id);
    w->cres = j->res;
    w->crej = j->rej;
    j->res = j->rej = JS_UNDEFINED;
    w->in_create = 1;
    w->create_left = j->npeers + 1;
    for (i = 0; i < j->npeers && w->in_create; i++) {
        wgm_peer_t* m = (wgm_peer_t*)calloc(1, sizeof *m);
        int rc;
        if (!m) {
            create_fail(w, wgm_err(ctx, "WireGuard: out of memory"));
            break;
        }
        m->res = m->rej = JS_UNDEFINED;
        m->initial = 1;
        rc = peer_add(w, &j->peers[i], m);
        if (rc < 0) {
            free(m);
            create_fail(w, wgm_err(ctx, "WireGuard: peers[%u]: rejected by the device", i));
            break;
        }
        if (rc > 0)
            peer_settle(w, m, rc == 2 ? 0 : -1);
    }
    create_step(w);
    JS_FreeValue(ctx, obj);
    goto out;
fail:
    if (j->tun)
        wg_tun_close(j->tun);
out:
    if (j->aio)
        dyn_net_reactor_release_rt(rt);
    JS_FreeValue(ctx, j->res);
    JS_FreeValue(ctx, j->rej);
    cjob_free(j);
}

static int each_addr(JSContext* ctx, const char* s, size_t n, void* ud, const char* who)
{
    wgm_cjob_t* j = (wgm_cjob_t*)ud;
    wg_ipaddr_t* na = (wg_ipaddr_t*)realloc(j->addr, (j->naddr + 1) * sizeof *na);
    unsigned cidr;
    if (!na) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    j->addr = na;
    memset(&na[j->naddr], 0, sizeof *na);
    if (wg_conf_cidr(s, n, &na[j->naddr].v6, na[j->naddr].a, &cidr) < 0) {
        JS_ThrowTypeError(ctx, "%s: address entry '%.*s' is not an IP address", who, (int)(n > 64 ? 64 : n), s);
        return -1;
    }
    j->naddr++;
    return 0;
}

static JSValue rejected_exc(JSContext* ctx)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f), e = JS_GetException(ctx);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, e);
        return p;
    }
    call_fn(ctx, f[1], e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    return p;
}

static int parse_create(JSContext* ctx, JSValueConst o, wgm_cjob_t* j)
{
    static const char who[] = "WireGuard.create";
    JSValue pv;
    const char* s;
    size_t n;
    unsigned v = 0, mtu = WG_TUN_MTU_DEFAULT;
    uint32_t i, len = 0;
    int rc;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "%s: expects an options object with privateKey", who);
        return -1;
    }
    if (dyn_opts_strict(ctx, o, create_keys, (int)countof(create_keys)))
        return -1;
    if (key_prop(ctx, o, "privateKey", j->priv, 1, who) < 0)
        return -1;
    if (uint_prop(ctx, o, "listenPort", 0, 65535, &v, who) < 0)
        return -1;
    j->port = (uint16_t)v;
    if (uint_prop(ctx, o, "mtu", WG_TUN_MTU_MIN, WG_TUN_MTU_MAX, &mtu, who) < 0)
        return -1;
    if (str_list(ctx, o, "address", 0, who, each_addr, j) < 0)
        return -1;
    pv = JS_GetPropertyStr(ctx, o, "peers");
    if (JS_IsException(pv))
        return -1;
    if (!JS_IsUndefined(pv)) {
        if (!JS_IsArray(ctx, pv)) {
            JS_FreeValue(ctx, pv);
            JS_ThrowTypeError(ctx, "%s: peers must be an array", who);
            return -1;
        }
        if (dyn_js_len_u32(ctx, pv, &len)) {
            JS_FreeValue(ctx, pv);
            return -1;
        }
        j->peers = (wgm_pspec_t*)calloc(len ? len : 1, sizeof *j->peers);
        if (!j->peers) {
            JS_FreeValue(ctx, pv);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, pv, i);
            char lab[40];
            unsigned k;
            snprintf(lab, sizeof lab, "WireGuard.create: peers[%u]", i);
            rc = parse_peer(ctx, e, &j->peers[i], lab);
            JS_FreeValue(ctx, e);
            if (rc < 0) {
                JS_FreeValue(ctx, pv);
                return -1;
            }
            j->npeers++;
            for (k = 0; k < i; k++)
                if (!memcmp(j->peers[k].pub, j->peers[i].pub, 32)) {
                    JS_FreeValue(ctx, pv);
                    JS_ThrowTypeError(ctx, "%s: duplicate publicKey", lab);
                    return -1;
                }
        }
    }
    JS_FreeValue(ctx, pv);
    rc = str_prop(ctx, o, "tun", &s, &n, 0, who);
    if (rc < 0)
        return -1;
    if (rc) {
        wg_tun_err_t te;
        char rb[160];
        if (n >= sizeof j->tun_name) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "%s: tun name longer than 15 bytes", who);
            return -1;
        }
        memcpy(j->tun_name, s, n);
        j->tun_name[n] = 0;
        JS_FreeCString(ctx, s);
        memset(&te, 0, sizeof te);
        j->tun = wg_tun_open(j->tun_name, mtu, &te);
        if (!j->tun) {
            JS_ThrowTypeError(ctx, "WireGuard: tun: %s", wg_tun_reason(&te, rb, sizeof rb));
            return -1;
        }
        snprintf(j->tun_name, sizeof j->tun_name, "%s", wg_tun_name(j->tun));
    } else if (!j->naddr) {
        JS_ThrowTypeError(ctx, "%s: address is required without tun", who);
        return -1;
    }
    return 0;
}

static JSValue wgm_create(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_cjob_t* j = (wgm_cjob_t*)calloc(1, sizeof *j);
    JSValue f[2], p;
    (void)this_val;
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->ctx = ctx;
    j->res = j->rej = JS_UNDEFINED;
    if (parse_create(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, j) < 0) {
        if (j->tun)
            wg_tun_close(j->tun);
        cjob_free(j);
        return rejected_exc(ctx);
    }
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        if (j->tun)
            wg_tun_close(j->tun);
        cjob_free(j);
        return p;
    }
    j->res = f[0];
    j->rej = f[1];
    j->aio = dyn_net_reactor_acquire(ctx);
    if (!j->aio) {
        JSValue e;
        dyn_net_throw_no_reactor(ctx, "WireGuard");
        e = JS_GetException(ctx);
        call_fn(ctx, j->rej, e);
        JS_FreeValue(ctx, e);
        if (j->tun)
            wg_tun_close(j->tun);
        JS_FreeValue(ctx, j->res);
        JS_FreeValue(ctx, j->rej);
        cjob_free(j);
        return p;
    }
    if (dyn_aio_offload_cpu(j->aio, cjob_work, cjob_done, j) != 0) {
        cjob_work(j);
        cjob_done(j);
    }
    return p;
}

static JSValue wgm_this_err(JSContext* ctx, JSValueConst this_val, wgm_t** w)
{
    *w = wgm_of(this_val);
    if (!*w || (*w)->closing)
        return JS_ThrowTypeError(ctx, "WireGuard: closed");
    return JS_UNDEFINED;
}

static JSValue wgm_add_peer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w;
    wgm_pspec_t ps;
    wgm_peer_t* m;
    JSValue f[2], p;
    int rc;
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)) || parse_peer(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &ps, "WireGuard.addPeer") < 0)
        return rejected_exc(ctx);
    if (peer_find(w, ps.pub)) {
        pspec_free(&ps);
        JS_ThrowTypeError(ctx, "WireGuard.addPeer: duplicate publicKey");
        return rejected_exc(ctx);
    }
    m = (wgm_peer_t*)calloc(1, sizeof *m);
    p = m ? JS_NewPromiseCapability(ctx, f) : JS_ThrowOutOfMemory(ctx);
    if (JS_IsException(p)) {
        free(m);
        pspec_free(&ps);
        return p;
    }
    m->res = f[0];
    m->rej = f[1];
    rc = peer_add(w, &ps, m);
    pspec_free(&ps);
    if (rc < 0) {
        wgm_settle(w, &m->res, &m->rej, 1, wgm_err(ctx, "WireGuard.addPeer: rejected by the device"));
        free(m);
    } else if (rc > 0)
        peer_settle(w, m, rc == 2 ? 0 : -1);
    wgm_kick(w);
    return p;
}

static JSValue wgm_remove_peer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w;
    uint8_t pub[32];
    const char* s;
    size_t n;
    wgm_peer_t* m;
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)))
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "WireGuard.removePeer: publicKey must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (wg_conf_key(s, n, pub) < 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "WireGuard.removePeer: publicKey must be base64 of 32 bytes");
    }
    JS_FreeCString(ctx, s);
    m = peer_find(w, pub);
    if (!m)
        return JS_FALSE;
    peer_unlink(w, m);
    if (m->p)
        wg_dev_remove_peer(wg_glue_dev(w->g), m->p);
    if (m->pending && !JS_IsUndefined(m->rej))
        wgm_settle(w, &m->res, &m->rej, 1, wgm_err(ctx, "WireGuard.addPeer: removed before it was ready"));
    JS_FreeValue(ctx, m->res);
    JS_FreeValue(ctx, m->rej);
    free(m);
    wgm_kick(w);
    return JS_TRUE;
}

static JSValue wgm_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_t* w;
    wg_dev_stats_t d;
    wg_glue_stats_t g;
    wg_tcp_stats_t t;
    JSValue o;
    (void)argc;
    (void)argv;
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)))
        return JS_EXCEPTION;
    wg_dev_stats(wg_glue_dev(w->g), &d);
    wg_glue_stats(w->g, &g);
    memset(&t, 0, sizeof t);
    if (wg_glue_tcp(w->g))
        wg_tcp_stats(wg_glue_tcp(w->g), &t);
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "peers", JS_NewUint32(ctx, w->npeers));
    JS_SetPropertyStr(ctx, o, "handshakesSent", JS_NewFloat64(ctx, (double)(d.hs_init_sent + d.hs_resp_sent)));
    JS_SetPropertyStr(ctx, o, "handshakesCompleted", JS_NewFloat64(ctx, (double)d.hs_done));
    JS_SetPropertyStr(ctx, o, "rxPackets", JS_NewFloat64(ctx, (double)d.rx_data));
    JS_SetPropertyStr(ctx, o, "txPackets", JS_NewFloat64(ctx, (double)d.tx_data));
    JS_SetPropertyStr(ctx, o, "rxDropped", JS_NewFloat64(ctx, (double)d.rx_drop));
    JS_SetPropertyStr(ctx, o, "txDropped", JS_NewFloat64(ctx, (double)d.tx_drop));
    JS_SetPropertyStr(ctx, o, "checksumsVerified", JS_NewFloat64(ctx, (double)d.csum_verified));
    JS_SetPropertyStr(ctx, o, "poolJobs", JS_NewFloat64(ctx, (double)d.jobs_offloaded));
    JS_SetPropertyStr(ctx, o, "inlineJobs", JS_NewFloat64(ctx, (double)d.inline_ops));
    JS_SetPropertyStr(ctx, o, "tunRx", JS_NewFloat64(ctx, (double)g.tun_rx));
    JS_SetPropertyStr(ctx, o, "tunTx", JS_NewFloat64(ctx, (double)g.tun_tx));
    JS_SetPropertyStr(ctx, o, "tcpConnections", JS_NewFloat64(ctx, (double)t.live));
    JS_SetPropertyStr(ctx, o, "timerFires", JS_NewFloat64(ctx, (double)g.timer_fires));
    return o;
}

static JSValue wgm_get_pub(JSContext* ctx, JSValueConst this_val)
{
    wgm_t* w;
    char b[48];
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)))
        return JS_EXCEPTION;
    wg_conf_key_b64(w->pub, b);
    return JS_NewString(ctx, b);
}

static JSValue wgm_get_port(JSContext* ctx, JSValueConst this_val)
{
    wgm_t* w;
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->port);
}

static JSValue wgm_get_tun(JSContext* ctx, JSValueConst this_val)
{
    wgm_t* w;
    if (JS_IsException(wgm_this_err(ctx, this_val, &w)))
        return JS_EXCEPTION;
    return w->tun[0] ? JS_NewString(ctx, w->tun) : JS_NULL;
}

typedef struct {
    JSContext* ctx;
    JSValue res, rej;
    uint8_t priv[32], pub[32];
    int rc;
} wgm_kjob_t;

static void kjob_work(void* arg)
{
    wgm_kjob_t* k = (wgm_kjob_t*)arg;
    k->rc = wg_keygen(k->priv, k->pub);
}

static void kjob_done(void* arg)
{
    wgm_kjob_t* k = (wgm_kjob_t*)arg;
    JSContext* ctx = k->ctx;
    if (k->rc < 0)
        call_fn(ctx, k->rej, wgm_err(ctx, "WireGuard.generateKeyPair: no randomness"));
    else {
        char a[48], b[48];
        JSValue o = JS_NewObject(ctx);
        wg_conf_key_b64(k->priv, a);
        wg_conf_key_b64(k->pub, b);
        JS_SetPropertyStr(ctx, o, "privateKey", JS_NewString(ctx, a));
        JS_SetPropertyStr(ctx, o, "publicKey", JS_NewString(ctx, b));
        wg_wipe(a, sizeof a);
        call_fn(ctx, k->res, o);
        JS_FreeValue(ctx, o);
    }
    JS_FreeValue(ctx, k->res);
    JS_FreeValue(ctx, k->rej);
    wg_wipe(k, sizeof *k);
    free(k);
    dyn_net_reactor_release(ctx);
}

static JSValue wgm_keypair(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_kjob_t* k = (wgm_kjob_t*)calloc(1, sizeof *k);
    JSValue f[2], p;
    dyn_aio_t* aio;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!k)
        return JS_ThrowOutOfMemory(ctx);
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        free(k);
        return p;
    }
    k->ctx = ctx;
    k->res = f[0];
    k->rej = f[1];
    aio = dyn_net_reactor_acquire(ctx);
    if (!aio || dyn_aio_offload_cpu(aio, kjob_work, kjob_done, k) != 0) {
        if (!aio) {
            JSValue e;
            dyn_net_throw_no_reactor(ctx, "WireGuard");
            e = JS_GetException(ctx);
            call_fn(ctx, k->rej, e);
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, k->res);
            JS_FreeValue(ctx, k->rej);
            free(k);
            return p;
        }
        kjob_work(k);
        kjob_done(k);
    }
    return p;
}

typedef struct {
    JSContext* ctx;
    JSValue o, peer, addr, peers, aips;
    wg_conf_err_t* e;
    uint32_t na, np, nai;
} wgm_pc_t;

static int pc_num(wgm_pc_t* c, JSValue obj, const char* k, const char* v, size_t vn, unsigned max)
{
    unsigned x;
    if (wg_conf_uint(v, vn, max, &x) < 0) {
        c->e->msg = "bad number";
        return -1;
    }
    JS_SetPropertyStr(c->ctx, obj, k, JS_NewUint32(c->ctx, x));
    return 0;
}

static void pc_list(wgm_pc_t* c, JSValue arr, uint32_t* n, const char* v, size_t vn)
{
    size_t off = 0, in;
    const char* it;
    while (wg_conf_next(v, vn, &off, &it, &in))
        JS_SetPropertyUint32(c->ctx, arr, (*n)++, JS_NewStringLen(c->ctx, it, in));
}

static int pc_cb(void* ud, int sec, int key, const char* v, size_t vn, unsigned line)
{
    wgm_pc_t* c = (wgm_pc_t*)ud;
    JSContext* ctx = c->ctx;
    (void)line;
    if (key == WG_CK_SECTION) {
        if (sec != WG_CONF_PEER)
            return 0;
        JS_FreeValue(ctx, c->peer);
        JS_FreeValue(ctx, c->aips);
        c->peer = JS_NewObject(ctx);
        c->aips = JS_NewArray(ctx);
        c->nai = 0;
        JS_SetPropertyStr(ctx, c->peer, "allowedIPs", JS_DupValue(ctx, c->aips));
        JS_SetPropertyUint32(ctx, c->peers, c->np++, JS_DupValue(ctx, c->peer));
        return 0;
    }
    switch (key) {
    case WG_CK_PRIVATEKEY:
        JS_SetPropertyStr(ctx, c->o, "privateKey", JS_NewStringLen(ctx, v, vn));
        return 0;
    case WG_CK_LISTENPORT:
        return pc_num(c, c->o, "listenPort", v, vn, 65535);
    case WG_CK_MTU:
        return pc_num(c, c->o, "mtu", v, vn, 65535);
    case WG_CK_ADDRESS:
        pc_list(c, c->addr, &c->na, v, vn);
        return 0;
    case WG_CK_PUBLICKEY:
        JS_SetPropertyStr(ctx, c->peer, "publicKey", JS_NewStringLen(ctx, v, vn));
        return 0;
    case WG_CK_PRESHAREDKEY:
        JS_SetPropertyStr(ctx, c->peer, "presharedKey", JS_NewStringLen(ctx, v, vn));
        return 0;
    case WG_CK_ENDPOINT:
        JS_SetPropertyStr(ctx, c->peer, "endpoint", JS_NewStringLen(ctx, v, vn));
        return 0;
    case WG_CK_ALLOWEDIPS:
        pc_list(c, c->aips, &c->nai, v, vn);
        return 0;
    case WG_CK_KEEPALIVE:
        if (vn == 3 && (v[0] | 32) == 'o' && (v[1] | 32) == 'f' && (v[2] | 32) == 'f') {
            JS_SetPropertyStr(ctx, c->peer, "persistentKeepalive", JS_NewInt32(ctx, 0));
            return 0;
        }
        return pc_num(c, c->peer, "persistentKeepalive", v, vn, 65535);
    default:
        return 0;
    }
}

static JSValue wgm_parse_config(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wgm_pc_t c;
    wg_conf_err_t e;
    const char* s;
    size_t n;
    int rc;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "WireGuard.parseConfig: text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    memset(&c, 0, sizeof c);
    memset(&e, 0, sizeof e);
    c.ctx = ctx;
    c.e = &e;
    c.o = JS_NewObject(ctx);
    c.addr = JS_NewArray(ctx);
    c.peers = JS_NewArray(ctx);
    c.peer = c.aips = JS_UNDEFINED;
    rc = wg_conf_scan(s, n, pc_cb, &c, &e);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, c.peer);
    JS_FreeValue(ctx, c.aips);
    if (rc < 0) {
        JS_FreeValue(ctx, c.o);
        JS_FreeValue(ctx, c.addr);
        JS_FreeValue(ctx, c.peers);
        return JS_ThrowSyntaxError(ctx, "WireGuard.parseConfig: line %u: %s", e.line, e.msg ? e.msg : "invalid");
    }
    JS_SetPropertyStr(ctx, c.o, "address", c.addr);
    JS_SetPropertyStr(ctx, c.o, "peers", c.peers);
    return c.o;
}

static JSValue wgm_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "WireGuard: use WireGuard.create(options)");
}

static void wgm_finalizer(JSRuntime* rt, JSValue val)
{
    wgm_in_final++;
    dyn_res_finalizer(rt, val);
    wgm_in_final--;
}

static void wgm_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, wgm_class_id);
    wgm_t* w;
    wgm_peer_t* m;
    if (!r || r->closed || !r->native)
        return;
    w = (wgm_t*)r->native;
    JS_MarkValue(rt, w->cres, mf);
    JS_MarkValue(rt, w->crej, mf);
    for (m = w->peers; m; m = m->next) {
        JS_MarkValue(rt, m->res, mf);
        JS_MarkValue(rt, m->rej, mf);
    }
    wgm_conns_mark(rt, w, mf);
}

static const JSCFunctionListEntry wgm_proto[] = {
    JS_CFUNC_DEF("dial", 2, wgm_dial),
    JS_CFUNC_DEF("listen", 2, wgm_listen),
    JS_CFUNC_DEF("udp", 2, wgm_udp),
    JS_CFUNC_DEF("addPeer", 1, wgm_add_peer),
    JS_CFUNC_DEF("removePeer", 1, wgm_remove_peer),
    JS_CFUNC_DEF("stats", 0, wgm_stats),
    JS_CGETSET_DEF("publicKey", wgm_get_pub, NULL),
    JS_CGETSET_DEF("listenPort", wgm_get_port, NULL),
    JS_CGETSET_DEF("tun", wgm_get_tun, NULL),
};

static const JSCFunctionListEntry wgm_static[] = {
    JS_CFUNC_DEF("create", 1, wgm_create),
    JS_CFUNC_DEF("parseConfig", 1, wgm_parse_config),
    JS_CFUNC_DEF("generateKeyPair", 0, wgm_keypair),
};

static const JSClassDef wgm_class = {
    "WireGuard",
    .finalizer = wgm_finalizer,
    .gc_mark = wgm_gc_mark,
};

static int wgm_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;
    if (dyn_register_class(ctx, m, &wgm_class_id, &wgm_class, wgm_proto, countof(wgm_proto), wgm_ctor, "WireGuard") < 0)
        return -1;
    proto = JS_GetClassProto(ctx, wgm_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    JS_SetPropertyFunctionList(ctx, ctor, wgm_static, countof(wgm_static));
    JS_FreeValue(ctx, ctor);
    return 0;
}

int js_nat_init_wireguard(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:wireguard", wgm_register);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "WireGuard");
    return 0;
}

#endif
