#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-dns.h"
#include "core/dyn-prng.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const dns_resolver_keys[] = { "server", "port", "timeoutMs", "ttl" };
static const char* const dns_server_keys[] = { "port", "host" };

#define DNS_MAX_INFLIGHT 64

#define DNS_CACHE_SLOTS 32
#define DNS_CACHE_TTL_CAP 86400

typedef struct dyn_resolver dyn_resolver_t;

typedef struct {
    int used;
    uint16_t id;
    char qname[DYN_DNS_MAX_NAME + 1];
    uint16_t qtype;
    JSValue cb;
    int is_promise;
    int shape;
    JSValue resolve, reject;
    dyn_timer_id timer;
    dyn_resolver_t* owner;
    int tcp_fd;
    uint8_t qmsg[512];
    uint16_t qlen;
    uint8_t* tcp_buf;
    size_t tcp_cap, have;
    int retried;
} dns_pending_t;

#define DNS_SHAPE_RECORDS 0
#define DNS_SHAPE_STRINGS 1
#define DNS_SHAPE_MX 2
#define DNS_SHAPE_TXT 3

typedef struct {
    int used;
    char name[DYN_DNS_MAX_NAME + 1];
    uint16_t qtype;
    uint64_t expire_ms;
    uint64_t stamp;
    JSValue records;
} dns_cache_entry_t;

struct dyn_resolver {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    struct sockaddr_in server;
    uint32_t timeout_ms;
    dns_pending_t q[DNS_MAX_INFLIGHT];
    dns_cache_entry_t cache[DNS_CACHE_SLOTS];
    uint32_t ttl_cap_s;
    uint64_t cache_seq;
    dyn_timers_t* timers;
    int started;
    int in_callback;
    int closing;
};

static JSClassID dyn_resolver_class_id;

static _Thread_local int dns_in_final;

static void dyn_dns_finalizer(JSRuntime* rt, JSValue val)
{
    dns_in_final++;
    dyn_res_finalizer(rt, val);
    dns_in_final--;
}

static void dyn_resolver_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_resolver_class_id);
    dyn_resolver_t* r;
    int i;

    if (!res || res->closed || !res->native)
        return;
    r = (dyn_resolver_t*)res->native;
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (r->q[i].used) {
            JS_MarkValue(rt, r->q[i].cb, mark_func);
            JS_MarkValue(rt, r->q[i].resolve, mark_func);
            JS_MarkValue(rt, r->q[i].reject, mark_func);
        }
    for (i = 0; i < DNS_CACHE_SLOTS; i++)
        if (r->cache[i].used)
            JS_MarkValue(rt, r->cache[i].records, mark_func);
}

static const JSClassDef dyn_resolver_class = {
    "DNSResolver",
    .finalizer = dyn_dns_finalizer,
    .gc_mark = dyn_resolver_gc_mark,
};

static JSValue dns_shape_records(JSContext* ctx, JSValueConst records,
    int shape)
{
    JSValue arr;
    uint32_t n = 0, i, out = 0;
    JSValue lv;

    if (shape == DNS_SHAPE_RECORDS)
        return JS_DupValue(ctx, records);
    lv = JS_GetPropertyStr(ctx, records, "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, i), v;
        if (JS_IsException(rec))
            goto fail;
        switch (shape) {
        case DNS_SHAPE_STRINGS:
            v = JS_GetPropertyStr(ctx, rec, "target");
            break;
        case DNS_SHAPE_TXT:
            v = JS_GetPropertyStr(ctx, rec, "chunks");
            break;
        case DNS_SHAPE_MX: {
            JSValue prio = JS_GetPropertyStr(ctx, rec, "priority");
            JSValue exch;
            if (JS_IsException(prio)) {
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            exch = JS_GetPropertyStr(ctx, rec, "exchange");
            if (JS_IsException(exch)) {
                JS_FreeValue(ctx, prio);
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            v = JS_NewObject(ctx);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, prio);
                JS_FreeValue(ctx, exch);
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            JS_DefinePropertyValueStr(ctx, v, "priority", prio, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, v, "exchange", exch, JS_PROP_C_W_E);
            break;
        }
        default:
            v = JS_DupValue(ctx, rec);
            break;
        }
        JS_FreeValue(ctx, rec);
        if (JS_IsException(v))
            goto fail;
        JS_DefinePropertyValueUint32(ctx, arr, out++, v, JS_PROP_C_W_E);
    }
    return arr;

fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static void dns_promise_settle(JSContext* ctx, dns_pending_t* p, JSValue err,
    JSValue val)
{
    JSValue r;

    if (!JS_IsNull(err)) {
        JSValue e = JS_NewError(ctx);
        JSValueConst a[1];
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message", JS_DupValue(ctx, err), JS_PROP_C_W_E);
            a[0] = e;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            a[0] = err;
        }
        r = JS_Call(ctx, p->reject, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, e);
    } else {
        JSValue shaped = dns_shape_records(ctx, val, p->shape);
        JSValueConst a[1];
        if (JS_IsException(shaped)) {
            shaped = JS_GetException(ctx);
            r = JS_Call(ctx, p->reject, JS_UNDEFINED, 1,
                (JSValueConst*)&shaped);
        } else {
            a[0] = shaped;
            r = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, shaped);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, r);
}

static void dns_settle(dyn_resolver_t* r, dns_pending_t* p, JSValue err,
    JSValue val)
{
    JSContext* ctx = r->ctx;
    JSValue cb = p->cb, ret;
    JSValueConst a[2];

    p->used = 0;
    p->cb = JS_UNDEFINED;
    if (p->is_promise) {
        JSValue resolve = p->resolve, reject = p->reject;
        dns_pending_t tmp;
        if (p->tcp_fd >= 0) {
            dyn_aio_close(r->aio, p->tcp_fd);
            p->tcp_fd = -1;
        }
        free(p->tcp_buf);
        p->tcp_buf = NULL;
        p->tcp_cap = p->have = 0;
        if (p->timer != DYN_TIMER_NONE) {
            dyn_timer_cancel(r->timers, p->timer);
            p->timer = DYN_TIMER_NONE;
        }
        memset(&tmp, 0, sizeof(tmp));
        tmp.is_promise = 1;
        tmp.shape = p->shape;
        tmp.resolve = resolve;
        tmp.reject = reject;
        p->resolve = p->reject = JS_UNDEFINED;
        dns_promise_settle(ctx, &tmp, err, val);
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, reject);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, val);
        return;
    }
    if (p->tcp_fd >= 0) {
        dyn_aio_close(r->aio, p->tcp_fd);
        p->tcp_fd = -1;
    }
    free(p->tcp_buf);
    p->tcp_buf = NULL;
    p->tcp_cap = p->have = 0;
    if (p->timer != DYN_TIMER_NONE) {
        dyn_timer_cancel(r->timers, p->timer);
        p->timer = DYN_TIMER_NONE;
    }
    if (JS_IsFunction(ctx, cb)) {
        a[0] = err;
        a[1] = val;
        ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, a);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, val);
}

static void dns_on_timeout(void* arg)
{
    dns_pending_t* p = (dns_pending_t*)arg;
    if (!p->used)
        return;
    p->timer = DYN_TIMER_NONE;
    dns_settle(p->owner, p, JS_NewString(p->owner->ctx, "query timed out"),
        JS_UNDEFINED);
}

static int dns_rr_matches_query(const dyn_dns_rr_t* rr, const dns_pending_t* p)
{
    return strcasecmp(rr->name, p->qname) == 0;
}

static JSValue dns_rr_to_js(JSContext* ctx, const dyn_dns_rr_t* rr,
    const uint8_t* msg, size_t len)
{
    char ip[64], name[DYN_DNS_MAX_NAME + 1];
    JSValue o;
    size_t rdoff;
    int t;

    rdoff = (size_t)(rr->rdata - msg);
    switch (rr->type) {
    case DYN_DNS_T_A:
        if (rr->rdlen != 4 || !inet_ntop(AF_INET, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_AAAA:
        if (rr->rdlen != 16 || !inet_ntop(AF_INET6, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_CNAME:
    case DYN_DNS_T_NS:
    case DYN_DNS_T_PTR:
        if (rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        t = dyn_dns_name_decode(msg, len, rdoff, name, sizeof(name));
        if (t < 0 || (size_t)t > rdoff + rr->rdlen)
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_MX:
        if (rr->rdlen < 3 || rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        t = dyn_dns_name_decode(msg, len, rdoff + 2, name, sizeof(name));
        if (t < 0 || (size_t)t > rdoff + rr->rdlen)
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_TXT:
        if (rr->rdlen < 1 || rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        break;
    default:
        return JS_UNDEFINED;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, rr->name),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "type", JS_NewInt32(ctx, rr->type),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ttl", JS_NewInt64(ctx, rr->ttl),
        JS_PROP_C_W_E);
    switch (rr->type) {
    case DYN_DNS_T_A:
    case DYN_DNS_T_AAAA:
        JS_DefinePropertyValueStr(ctx, o, "address", JS_NewString(ctx, ip),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_CNAME:
    case DYN_DNS_T_NS:
    case DYN_DNS_T_PTR:
        JS_DefinePropertyValueStr(ctx, o, "target", JS_NewString(ctx, name),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_MX:
        JS_DefinePropertyValueStr(ctx, o, "priority",
            JS_NewInt32(ctx, (rr->rdata[0] << 8) | rr->rdata[1]),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "exchange", JS_NewString(ctx, name),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_TXT: {
        JSValue chunks = JS_NewArray(ctx);
        size_t at = 0;
        uint32_t ci = 0;
        if (JS_IsException(chunks))
            return chunks;
        while (at < rr->rdlen) {
            size_t cl = rr->rdata[at];
            if (cl > rr->rdlen - at - 1)
                break;
            JS_DefinePropertyValueUint32(ctx, chunks, ci++,
                JS_NewStringLen(ctx, (const char*)rr->rdata + at + 1, cl),
                JS_PROP_C_W_E);
            at += 1 + cl;
        }
        JS_DefinePropertyValueStr(ctx, o, "chunks", chunks, JS_PROP_C_W_E);
        break;
    }
    }
    return o;
}

static void dns_tcp_fallback(dyn_resolver_t* r, dns_pending_t* p);

static void dns_name_fold(char* s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static JSValue dns_clone_records(JSContext* ctx, JSValueConst records);

static void dns_cache_free_entry(JSContext* ctx, dns_cache_entry_t* e)
{
    if (e->used) {
        JS_FreeValue(ctx, e->records);
        e->records = JS_UNDEFINED;
        e->used = 0;
    }
}

static void dns_cache_free_entry_rt(JSRuntime* rt, dns_cache_entry_t* e)
{
    if (e->used) {
        JS_FreeValueRT(rt, e->records);
        e->records = JS_UNDEFINED;
        e->used = 0;
    }
}

static void dns_cache_store(dyn_resolver_t* r, const char* name,
    uint16_t qtype, uint32_t min_ttl, JSValue records)
{
    JSContext* ctx = r->ctx;
    dns_cache_entry_t *slot = NULL, *victim = NULL;
    int i;
    uint64_t ttl_ms;

    if (!r->ttl_cap_s || !min_ttl)
        return;
    ttl_ms = 1000ull * (min_ttl < r->ttl_cap_s ? min_ttl : r->ttl_cap_s);
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        dns_cache_entry_t* e = &r->cache[i];
        if (!e->used) {
            slot = e;
            break;
        }
        if (e->expire_ms <= dyn_timer_now_ms()) {
            slot = e;
            break;
        }
        if (!victim || e->stamp < victim->stamp)
            victim = e;
    }
    if (!slot)
        slot = victim;
    dns_cache_free_entry(ctx, slot);
    slot->used = 1;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    dns_name_fold(slot->name);
    slot->qtype = qtype;
    slot->expire_ms = dyn_timer_now_ms() + ttl_ms;
    slot->stamp = ++r->cache_seq;
    slot->records = dns_clone_records(ctx, records);
    if (JS_IsException(slot->records)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        slot->records = JS_UNDEFINED;
        slot->used = 0;
    }
}

static JSValue dns_clone_record(JSContext* ctx, JSValueConst rec)
{
    static const char* const keys[] = { "name", "type", "ttl", "address",
        "target", "priority", "exchange",
        "chunks" };
    JSValue o = JS_NewObject(ctx);
    int k;

    if (JS_IsException(o))
        return o;
    for (k = 0; k < (int)countof(keys); k++) {
        JSValue v = JS_GetPropertyStr(ctx, rec, keys[k]);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (k == (int)countof(keys) - 1 && JS_IsArray(ctx, v)) {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t n = 0, i;
            JSValue copy = JS_NewArray(ctx);
            if (!JS_IsException(lv) && !JS_IsException(copy) && !JS_ToUint32(ctx, &n, lv)) {
                for (i = 0; i < n; i++) {
                    JSValue el = JS_GetPropertyUint32(ctx, v, i);
                    if (JS_IsException(el)) {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        el = JS_UNDEFINED;
                    }
                    JS_DefinePropertyValueUint32(ctx, copy, i, el,
                        JS_PROP_C_W_E);
                }
            }
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, v);
            v = copy;
        }
        JS_DefinePropertyValueStr(ctx, o, keys[k], v, JS_PROP_C_W_E);
    }
    return o;
}

static JSValue dns_clone_records(JSContext* ctx, JSValueConst records)
{
    JSValue lv = JS_GetPropertyStr(ctx, records, "length");
    uint32_t n = 0, i;
    JSValue arr = JS_NewArray(ctx);

    if (JS_IsException(lv))
        return lv;
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, i), c;
        if (JS_IsException(rec)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        c = dns_clone_record(ctx, rec);
        JS_FreeValue(ctx, rec);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, arr);
            return c;
        }
        JS_DefinePropertyValueUint32(ctx, arr, i, c, JS_PROP_C_W_E);
    }
    return arr;
}

static JSValue dns_cache_fetch(dyn_resolver_t* r, const char* name,
    uint16_t qtype)
{
    JSContext* ctx = r->ctx;
    char key[DYN_DNS_MAX_NAME + 1];
    int i;

    if (!r->ttl_cap_s)
        return JS_UNDEFINED;
    snprintf(key, sizeof(key), "%s", name);
    dns_name_fold(key);
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        dns_cache_entry_t* e = &r->cache[i];
        if (!e->used || e->qtype != qtype || strcmp(e->name, key) != 0)
            continue;
        if (e->expire_ms <= dyn_timer_now_ms()) {
            dns_cache_free_entry(ctx, e);
            return JS_UNDEFINED;
        }
        return dns_clone_records(ctx, e->records);
    }
    return JS_UNDEFINED;
}

typedef struct {
    JSValue cb, resolve, reject, records;
    int is_promise, shape;
} dns_hit_t;

static void dns_hit_deliver(JSContext* ctx, dns_hit_t* h)
{
    if (h->is_promise) {
        dns_pending_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.is_promise = 1;
        tmp.shape = h->shape;
        tmp.resolve = h->resolve;
        tmp.reject = h->reject;
        dns_promise_settle(ctx, &tmp, JS_NULL, h->records);
    } else if (JS_IsFunction(ctx, h->cb)) {
        JSValueConst a[2] = { JS_NULL, h->records };
        JSValue r = JS_Call(ctx, h->cb, JS_UNDEFINED, 2, a);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    }
}

static JSValue dns_hit_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    dns_hit_t* h;
    int64_t p = 0;

    (void)argc;
    if (JS_ToInt64(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    h = (dns_hit_t*)(uintptr_t)p;
    dns_hit_deliver(ctx, h);
    JS_FreeValue(ctx, h->cb);
    JS_FreeValue(ctx, h->resolve);
    JS_FreeValue(ctx, h->reject);
    JS_FreeValue(ctx, h->records);
    free(h);
    return JS_UNDEFINED;
}

static int dns_cache_deliver(JSContext* ctx, JSValue records, int shape,
    JSValueConst cb, JSValue resolve, JSValue reject,
    int is_promise)
{
    dns_hit_t* h = (dns_hit_t*)calloc(1, sizeof(*h));
    JSValue dptr, a[2];
    int rc;

    if (!h)
        return -1;
    h->is_promise = is_promise;
    h->shape = shape;
    h->cb = JS_IsFunction(ctx, cb) ? JS_DupValue(ctx, cb) : JS_UNDEFINED;
    h->resolve = is_promise ? JS_DupValue(ctx, resolve) : JS_UNDEFINED;
    h->reject = is_promise ? JS_DupValue(ctx, reject) : JS_UNDEFINED;
    h->records = JS_DupValue(ctx, records);
    dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)h);
    a[0] = dptr;
    a[1] = records;
    rc = JS_EnqueueJob(ctx, dns_hit_job, 2, a);
    JS_FreeValue(ctx, dptr);
    if (rc < 0) {
        JS_FreeValue(ctx, h->cb);
        JS_FreeValue(ctx, h->resolve);
        JS_FreeValue(ctx, h->reject);
        JS_FreeValue(ctx, h->records);
        free(h);
        return -1;
    }
    return 0;
}

static void dns_handle_response(dyn_resolver_t* r, const uint8_t* buf,
    unsigned len, dns_pending_t* p)
{
    JSContext* ctx = r->ctx;
    dyn_dns_hdr_t h;
    char qname[DYN_DNS_MAX_NAME + 1];
    size_t off;
    JSValue arr;
    int i;

    if (len < DYN_DNS_HDR_LEN)
        return;
    if (dyn_dns_hdr_decode(buf, len, &h) < 0)
        return;
    if (!(h.flags & 0x8000))
        return;
    if (!p) {
        for (i = 0; i < DNS_MAX_INFLIGHT; i++)
            if (r->q[i].used && r->q[i].id == h.id) {
                p = &r->q[i];
                break;
            }
    } else if (p->used && p->id != h.id) {
        return;
    }
    if (!p || !p->used)
        return;

    off = DYN_DNS_HDR_LEN;
    if (h.qdcount != 1)
        return;
    {
        int after = dyn_dns_name_decode(buf, len, off, qname, sizeof(qname));
        uint16_t qtype;
        if (after < 0 || (size_t)after + 4 > len)
            return;
        qtype = (uint16_t)((buf[after] << 8) | buf[after + 1]);
        if (qtype != p->qtype || strcasecmp(qname, p->qname) != 0)
            return;
        off = (size_t)after + 4;
    }

    if ((h.flags & 0x0200) && !p->retried) {
        p->retried = 1;
        dns_tcp_fallback(r, p);
        return;
    }

    if ((h.flags & 0x000f) != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "DNS error, rcode %u",
            (unsigned)(h.flags & 0x000f));
        dns_settle(r, p, JS_NewString(ctx, msg), JS_UNDEFINED);
        return;
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dns_settle(r, p, JS_NewString(ctx, "out of memory"), JS_UNDEFINED);
        return;
    }
    {
        uint32_t idx = 0;
        uint32_t min_ttl = 0xffffffffu;
        int kept = 0;
        for (i = 0; i < (int)h.ancount; i++) {
            dyn_dns_rr_t rr;
            JSValue v;
            if (dyn_dns_rr_decode(buf, len, &off, &rr) < 0)
                break;
            if (!dns_rr_matches_query(&rr, p) || rr.type != p->qtype
                || rr.cls != 1)
                continue;
            v = dns_rr_to_js(ctx, &rr, buf, len);
            if (JS_IsUndefined(v))
                continue;
            JS_DefinePropertyValueUint32(ctx, arr, idx++, v, JS_PROP_C_W_E);
            if (rr.ttl < min_ttl)
                min_ttl = rr.ttl;
            kept++;
        }
        if (kept > 0)
            dns_cache_store(r, p->qname, p->qtype, min_ttl, arr);
    }
    dns_settle(r, p, JS_NULL, arr);
}

static void dns_on_message(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, const struct sockaddr* peer,
    unsigned peerlen, void* ud)
{
    dyn_resolver_t* r = (dyn_resolver_t*)ud;
    (void)aio;
    (void)peerlen;

    if (res < 0 || len < DYN_DNS_HDR_LEN)
        return;
    if (peer && peer->sa_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)peer;
        if (sa->sin_addr.s_addr != r->server.sin_addr.s_addr || sa->sin_port != r->server.sin_port)
            return;
    }
    dns_handle_response(r, buf, len, NULL);
}

static void dns_tcp_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dns_pending_t* p = (dns_pending_t*)ud;
    dyn_resolver_t* r = p->owner;
    size_t want;
    (void)aio;

    if (!p->used)
        return;
    if (res <= 0) {
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: connection closed"),
            JS_UNDEFINED);
        return;
    }
    if (p->have + len > p->tcp_cap) {
        size_t nc = p->tcp_cap ? p->tcp_cap * 2 : 2048;
        uint8_t* nb;
        while (nc < p->have + len)
            nc *= 2;
        if (nc > 65538) {
            dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: oversize"),
                JS_UNDEFINED);
            return;
        }
        nb = (uint8_t*)realloc(p->tcp_buf, nc);
        if (!nb) {
            dns_settle(r, p, JS_NewString(r->ctx, "out of memory"), JS_UNDEFINED);
            return;
        }
        p->tcp_buf = nb;
        p->tcp_cap = nc;
    }
    memcpy(p->tcp_buf + p->have, buf, len);
    p->have += len;

    if (p->have < 2)
        return;
    want = ((size_t)p->tcp_buf[0] << 8) | p->tcp_buf[1];
    if (p->have < want + 2)
        return;
    dns_handle_response(r, p->tcp_buf + 2, (unsigned)want, p);
}

static void dns_tcp_connected(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dns_pending_t* p = (dns_pending_t*)ud;
    dyn_resolver_t* r = p->owner;
    uint8_t* framed;
    (void)aio;
    (void)buf;
    (void)len;

    if (!p->used)
        return;
    if (res < 0) {
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: connect failed"),
            JS_UNDEFINED);
        return;
    }
    framed = (uint8_t*)malloc(p->qlen + 2u);
    if (!framed) {
        dns_settle(r, p, JS_NewString(r->ctx, "out of memory"), JS_UNDEFINED);
        return;
    }
    framed[0] = (uint8_t)(p->qlen >> 8);
    framed[1] = (uint8_t)p->qlen;
    memcpy(framed + 2, p->qmsg, p->qlen);
    {
        int rc = dyn_aio_send(r->aio, p->tcp_fd, framed, p->qlen + 2u, 0,
            NULL, NULL);
        free(framed);
        if (rc < 0) {
            dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: send failed"),
                JS_UNDEFINED);
            return;
        }
    }
    dyn_aio_recv(r->aio, p->tcp_fd, 0, 1, dns_tcp_recv, p);
}

static void dns_tcp_fallback(dyn_resolver_t* r, dns_pending_t* p)
{
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &r->server.sin_addr, ip, sizeof(ip))) {
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: bad server"),
            JS_UNDEFINED);
        return;
    }
    p->tcp_fd = dyn_aio_connect(r->aio, ip, ntohs(r->server.sin_port),
        dns_tcp_connected, p);
    if (p->tcp_fd < 0)
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: connect failed"),
            JS_UNDEFINED);
}

static void dyn_resolver_teardown(dyn_resolver_t* r);

static void dyn_resolver_dispose(void* native)
{
    dyn_resolver_t* r = (dyn_resolver_t*)native;
    if (!r)
        return;
    if (r->in_callback) {
        r->closing = 1;
        return;
    }
    dyn_resolver_teardown(r);
}

static void dyn_resolver_teardown(dyn_resolver_t* r)
{
    JSContext* ctx = r->ctx;
    int i;
    dyn_net_off_drain(r);
    if (r->timers) {
        dyn_timers_free(r->timers);
        r->timers = NULL;
    }
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (r->q[i].used) {
            if (r->q[i].tcp_buf) {
                free(r->q[i].tcp_buf);
                r->q[i].tcp_buf = NULL;
            }
            if (r->q[i].tcp_fd >= 0) {
                dyn_aio_close(r->aio, r->q[i].tcp_fd);
                r->q[i].tcp_fd = -1;
            }
            if (!dns_in_final && r->q[i].is_promise && !JS_IsUndefined(r->q[i].reject)) {
                JSValue e = JS_NewError(ctx), rr;
                if (!JS_IsException(e)) {
                    JS_DefinePropertyValueStr(ctx, e, "message",
                        JS_NewString(ctx, "DNSResolver: closed"), JS_PROP_C_W_E);
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    e = JS_NewString(ctx, "DNSResolver: closed");
                }
                rr = JS_Call(ctx, r->q[i].reject, JS_UNDEFINED, 1,
                    (JSValueConst*)&e);
                if (JS_IsException(rr))
                    JS_FreeValue(ctx, JS_GetException(ctx));
                else
                    JS_FreeValue(ctx, rr);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValueRT(r->rt, r->q[i].cb);
            JS_FreeValueRT(r->rt, r->q[i].resolve);
            JS_FreeValueRT(r->rt, r->q[i].reject);
            r->q[i].used = 0;
        }
    for (i = 0; i < DNS_CACHE_SLOTS; i++)
        dns_cache_free_entry_rt(r->rt, &r->cache[i]);
    if (r->aio) {
        if (r->fd >= 0)
            dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release_rt(r->rt);
    }
    free(r);
}

static void dns_drain_hook(void* udata)
{
    dyn_resolver_t* r = (dyn_resolver_t*)udata;
    if (!r->timers)
        return;
    r->in_callback = 1;
    dyn_timer_run(r->timers, dyn_timer_now_ms());
    r->in_callback = 0;
    if (r->closing)
        dyn_resolver_teardown(r);
}

static JSValue dyn_resolver_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* server = NULL;
    int64_t port = 53, timeout = 5000, ttl = 0;
    JSValue v;

    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], dns_resolver_keys, 4))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "server");
        server = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "timeoutMs");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &timeout, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "ttl");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &ttl, v)) {
            JS_FreeValue(ctx, v);
            if (server)
                JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (port < 1 || port > 65535 || timeout < 1 || timeout > 4294967295LL) {
        if (server)
            JS_FreeCString(ctx, server);
        return JS_ThrowRangeError(ctx, "DNSResolver: bad port or timeoutMs");
    }
    if (ttl < 0)
        ttl = 0;
    if (ttl > DNS_CACHE_TTL_CAP)
        ttl = DNS_CACHE_TTL_CAP;

    r = (dyn_resolver_t*)calloc(1, sizeof(*r));
    if (!r) {
        if (server)
            JS_FreeCString(ctx, server);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->ctx = ctx;
    r->rt = JS_GetRuntime(ctx);
    r->fd = -1;
    r->timeout_ms = (uint32_t)timeout;
    r->ttl_cap_s = (uint32_t)ttl;
    memset(&r->server, 0, sizeof(r->server));
    r->server.sin_family = AF_INET;
    r->server.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server ? server : "127.0.0.1",
            &r->server.sin_addr)
        != 1) {
        if (server)
            JS_FreeCString(ctx, server);
        free(r);
        return JS_ThrowTypeError(ctx, "DNSResolver: server must be an IPv4 address");
    }
    if (server)
        JS_FreeCString(ctx, server);

    r->aio = dyn_net_reactor_acquire(ctx);
    if (!r->aio) {
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->fd = dyn_aio_udp_bind(r->aio, "0.0.0.0", 0);
    if (r->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowInternalError(ctx, "DNSResolver: bind failed");
    }
    if (connect(r->fd, (struct sockaddr*)&r->server, sizeof(r->server)) != 0) {
        dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowInternalError(ctx, "DNSResolver: connect failed");
    }
    r->timers = dyn_timers_new();
    if (!r->timers) {
        dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_resolver_class_id, r, dyn_resolver_dispose);
}

static JSValue dns_submit(JSContext* ctx, dyn_resolver_t* r, const char* name,
    int64_t type, int shape, int is_promise,
    JSValueConst cb, JSValueConst resolve,
    JSValueConst reject)
{
    uint8_t msg[512];
    dns_pending_t* p = NULL;
    int i, tries;
    JSValue hit;

    if (strlen(name) > DYN_DNS_MAX_NAME) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "DNSResolver: name too long");
    }
    if (type < 1 || type > 0xffff) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "DNSResolver: type out of range");
    }

    hit = dns_cache_fetch(r, name, (uint16_t)type);
    if (!JS_IsUndefined(hit)) {
        if (dns_cache_deliver(ctx, hit, shape, cb, resolve, reject,
                is_promise)
            == 0) {
            JS_FreeValue(ctx, hit);
            JS_FreeCString(ctx, name);
            return JS_UNDEFINED;
        }
        JS_FreeValue(ctx, hit);
    }

    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (!r->q[i].used) {
            p = &r->q[i];
            break;
        }
    if (!p) {
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "DNSResolver: too many queries in flight");
    }

    for (tries = 0; tries < 64; tries++) {
        int clash = 0;
        uint16_t rid;
        if (dyn_os_entropy(&rid, sizeof(rid)) < 0) {
            JS_FreeCString(ctx, name);
            return JS_ThrowInternalError(ctx,
                "DNSResolver: OS entropy unavailable");
        }
        p->id = rid;
        for (i = 0; i < DNS_MAX_INFLIGHT; i++)
            if (r->q[i].used && r->q[i].id == p->id) {
                clash = 1;
                break;
            }
        if (!clash)
            break;
    }
    if (tries == 64) {
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "DNSResolver: query ID collision");
    }
    {
        int n = dyn_dns_build_query(p->id, name, (uint16_t)type, msg, sizeof(msg));
        if (n < 0) {
            JS_FreeCString(ctx, name);
            return JS_ThrowTypeError(ctx, "DNSResolver: %s", dyn_dns_strerror(n));
        }
        memcpy(p->qmsg, msg, (size_t)n);
        p->qlen = (uint16_t)n;
    }
    p->tcp_fd = -1;
    p->tcp_buf = NULL;
    p->tcp_cap = p->have = 0;
    p->retried = 0;
    snprintf(p->qname, sizeof(p->qname), "%s", name);
    JS_FreeCString(ctx, name);
    p->qtype = (uint16_t)type;
    p->owner = r;
    p->is_promise = is_promise;
    p->shape = shape;
    p->cb = (!is_promise && JS_IsFunction(ctx, cb)) ? JS_DupValue(ctx, cb)
                                                    : JS_UNDEFINED;
    p->resolve = is_promise ? JS_DupValue(ctx, resolve) : JS_UNDEFINED;
    p->reject = is_promise ? JS_DupValue(ctx, reject) : JS_UNDEFINED;
    p->used = 1;

    if (!r->started) {
        if (dyn_aio_recvfrom(r->aio, r->fd, dns_on_message, r) < 0 || dyn_net_on_drain(dns_drain_hook, r) < 0) {
            p->used = 0;
            JS_FreeValue(ctx, p->cb);
            JS_FreeValue(ctx, p->resolve);
            JS_FreeValue(ctx, p->reject);
            return JS_ThrowInternalError(ctx, "DNSResolver: recv failed");
        }
        r->started = 1;
    }
    p->timer = dyn_timer_add(r->timers, dyn_timer_now_ms(), r->timeout_ms,
        dns_on_timeout, p);
    if (p->timer == DYN_TIMER_NONE) {
        p->used = 0;
        JS_FreeValue(ctx, p->cb);
        JS_FreeValue(ctx, p->resolve);
        JS_FreeValue(ctx, p->reject);
        p->cb = p->resolve = p->reject = JS_UNDEFINED;
        return JS_ThrowInternalError(ctx, "DNSResolver: out of memory");
    }
    if (dyn_aio_sendto(r->aio, r->fd, msg, p->qlen, NULL, 0) < 0) {
        JSValue e = JS_NewString(ctx, "send failed");
        dns_settle(r, p, e, JS_UNDEFINED);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static void dns_reject_pending(JSContext* ctx, JSValueConst reject)
{
    JSValue exc = JS_GetException(ctx);
    JSValueConst a[1] = { exc };
    JSValue r = JS_Call(ctx, reject, JS_UNDEFINED, 1, a);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, exc);
}

static JSValue dyn_resolver_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    int64_t type = DYN_DNS_T_A;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "query(name, type, callback)");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &type, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    return dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 0,
        argc >= 3 ? argv[2] : JS_UNDEFINED,
        JS_UNDEFINED, JS_UNDEFINED);
}

static JSValue dyn_resolver_lookup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    int64_t type = DYN_DNS_T_A;
    JSValueConst cb = JS_UNDEFINED;
    int ncoerce = argc;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "lookup(name, type?, callback?)");
    if (argc >= 2 && JS_IsFunction(ctx, argv[argc - 1])) {
        cb = argv[argc - 1];
        ncoerce = argc - 1;
    }
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (ncoerce >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &type, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    if (!JS_IsUndefined(cb)) {
        return dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 0, cb,
            JS_UNDEFINED, JS_UNDEFINED);
    }
    {
        JSValue funcs[2], promise, sub;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        sub = dns_submit(ctx, r, name, type, DNS_SHAPE_RECORDS, 1, JS_UNDEFINED,
            funcs[0], funcs[1]);
        if (JS_IsException(sub))
            dns_reject_pending(ctx, funcs[1]);
        JS_FreeValue(ctx, sub);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
}

static JSValue dns_resolve_typed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int64_t type,
    int shape, const char* what)
{
    dyn_resolver_t* r;
    const char* name = NULL;
    JSValue funcs[2], promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(name)", what);
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    r = (dyn_resolver_t*)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, name);
        return promise;
    }
    {
        JSValue sub = dns_submit(ctx, r, name, type, shape, 1, JS_UNDEFINED,
            funcs[0], funcs[1]);
        if (JS_IsException(sub))
            dns_reject_pending(ctx, funcs[1]);
        JS_FreeValue(ctx, sub);
    }
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue dyn_resolver_cname(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_CNAME,
        DNS_SHAPE_STRINGS, "resolveCname");
}
static JSValue dyn_resolver_mx(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_MX,
        DNS_SHAPE_MX, "resolveMx");
}
static JSValue dyn_resolver_txt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_TXT,
        DNS_SHAPE_TXT, "resolveTxt");
}
static JSValue dyn_resolver_ns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dns_resolve_typed(ctx, this_val, argc, argv, DYN_DNS_T_NS,
        DNS_SHAPE_STRINGS, "resolveNs");
}

static const JSCFunctionListEntry dyn_resolver_proto[] = {
    JS_CFUNC_DEF("query", 2, dyn_resolver_query),
    JS_CFUNC_DEF("lookup", 1, dyn_resolver_lookup),
    JS_CFUNC_DEF("resolveCname", 1, dyn_resolver_cname),
    JS_CFUNC_DEF("resolveMx", 1, dyn_resolver_mx),
    JS_CFUNC_DEF("resolveTxt", 1, dyn_resolver_txt),
    JS_CFUNC_DEF("resolveNs", 1, dyn_resolver_ns),
};

#define DNS_AMP_FACTOR 4
#define DNS_RRL_SLOTS 64
#define DNS_RRL_PER_SEC 20

typedef struct {
    uint32_t addr;
    uint32_t tokens;
    uint64_t refilled_ms;
} dns_rrl_t;

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    uint16_t port;
    int started;
    JSValue handler;
    dns_rrl_t rrl[DNS_RRL_SLOTS];
    uint8_t in_cb, closing;
} dyn_dnssrv_t;

static void dyn_dnssrv_teardown(dyn_dnssrv_t* s);

static JSClassID dyn_dnssrv_class_id;

static void dyn_dnssrv_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_dnssrv_class_id);
    dyn_dnssrv_t* s;

    if (!res || res->closed || !res->native)
        return;
    s = (dyn_dnssrv_t*)res->native;
    JS_MarkValue(rt, s->handler, mark_func);
}

static const JSClassDef dyn_dnssrv_class = {
    "DNSServer",
    .finalizer = dyn_dns_finalizer,
    .gc_mark = dyn_dnssrv_gc_mark,
};

static uint32_t dns_rrl_key(const struct sockaddr* peer)
{
    if (peer->sa_family == AF_INET) {
        uint32_t a = ((const struct sockaddr_in*)peer)->sin_addr.s_addr;
        return a ? a : 1u;
    }
    if (peer->sa_family == AF_INET6) {
        const uint8_t* p = ((const struct sockaddr_in6*)peer)->sin6_addr.s6_addr;
        uint32_t h = 2166136261u;
        int i;
        for (i = 0; i < 16; i++) {
            h ^= p[i];
            h *= 16777619u;
        }
        return h ? h : 1u;
    }
    return 0;
}

static int dns_rrl_allow(dyn_dnssrv_t* s, uint32_t addr)
{
    uint64_t now = dyn_timer_now_ms();
    int i, free_slot = -1, oldest = 0;

    for (i = 0; i < DNS_RRL_SLOTS; i++) {
        if (s->rrl[i].addr == addr) {
            uint64_t elapsed = now - s->rrl[i].refilled_ms;
            if (elapsed >= 1000) {
                s->rrl[i].tokens = DNS_RRL_PER_SEC;
                s->rrl[i].refilled_ms = now;
            }
            if (s->rrl[i].tokens == 0)
                return 0;
            s->rrl[i].tokens--;
            return 1;
        }
        if (!s->rrl[i].addr && free_slot < 0)
            free_slot = i;
        if (s->rrl[i].refilled_ms < s->rrl[oldest].refilled_ms)
            oldest = i;
    }
    i = (free_slot >= 0) ? free_slot : oldest;
    s->rrl[i].addr = addr;
    s->rrl[i].tokens = DNS_RRL_PER_SEC - 1;
    s->rrl[i].refilled_ms = now;
    return 1;
}

static void dnssrv_on_query(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, const struct sockaddr* peer,
    unsigned peerlen, void* ud)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)ud;
    JSContext* ctx = s->ctx;
    dyn_dns_hdr_t h;
    char qname[DYN_DNS_MAX_NAME + 1];
    uint8_t out[512];
    uint16_t qtype;
    int after, off, n_ans = 0;
    JSValue r;
    (void)aio;

    if (res < 0 || len < DYN_DNS_HDR_LEN || !peer)
        return;
    {
        uint32_t k = dns_rrl_key(peer);
        if (k && !dns_rrl_allow(s, k))
            return;
    }
    if (dyn_dns_hdr_decode(buf, len, &h) < 0 || (h.flags & 0x8000))
        return;

    after = dyn_dns_name_decode(buf, len, DYN_DNS_HDR_LEN, qname, sizeof(qname));
    if (after < 0 || (size_t)after + 4 > len)
        return;
    qtype = (uint16_t)((buf[after] << 8) | buf[after + 1]);

    off = dyn_dns_begin_response(buf, len, 0, out, sizeof(out));
    if (off < 0)
        return;

    if (JS_IsFunction(ctx, s->handler)) {
        JSValueConst a[2];
        a[0] = JS_NewString(ctx, qname);
        a[1] = JS_NewInt32(ctx, qtype);
        s->in_cb = 1;
        r = JS_Call(ctx, s->handler, JS_UNDEFINED, 2, a);
        s->in_cb = 0;
        if (s->closing) {
            JS_FreeValue(ctx, (JSValue)a[0]);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            dyn_dnssrv_teardown(s);
            return;
        }
        JS_FreeValue(ctx, (JSValue)a[0]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            return;
        }
        if (JS_IsString(r)) {
            const char* ip = JS_ToCString(ctx, r);
            uint8_t v4[4], v6[16];
            if (ip) {
                if (qtype == DYN_DNS_T_A && inet_pton(AF_INET, ip, v4) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                        DYN_DNS_T_A, 60, v4, 4);
                    if (no > 0) {
                        off = no;
                        n_ans = 1;
                    }
                } else if (qtype == DYN_DNS_T_AAAA && inet_pton(AF_INET6, ip, v6) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                        DYN_DNS_T_AAAA, 60, v6, 16);
                    if (no > 0) {
                        off = no;
                        n_ans = 1;
                    }
                }
                JS_FreeCString(ctx, ip);
            }
        }
        JS_FreeValue(ctx, r);
    }
    dyn_dns_set_ancount(out, (uint16_t)n_ans);

    if ((size_t)off > (size_t)len * DNS_AMP_FACTOR) {
        out[2] |= 0x02;
        dyn_dns_set_ancount(out, 0);
        off = (int)(DYN_DNS_HDR_LEN + ((size_t)after + 4 - DYN_DNS_HDR_LEN));
    }
    dyn_aio_sendto(s->aio, s->fd, out, (size_t)off, peer, peerlen);
}

static void dyn_dnssrv_teardown(dyn_dnssrv_t* s)
{
    if (!s)
        return;
    if (s->aio) {
        if (s->fd >= 0)
            dyn_aio_close(s->aio, s->fd);
        dyn_net_reactor_release_rt(s->rt);
    }
    JS_FreeValueRT(s->rt, s->handler);
    free(s);
}

static void dyn_dnssrv_dispose(void* native)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)native;
    if (!s)
        return;
    if (s->in_cb) {
        s->closing = 1;
        return;
    }
    dyn_dnssrv_teardown(s);
}

static JSValue dyn_dnssrv_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_dnssrv_t* s;
    int64_t port = 0;
    const char* host = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    JSValue v;

    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], dns_server_keys, 2))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        host = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (port < 0 || port > 65535) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "DNSServer: port must be 0..65535");
    }
    s = (dyn_dnssrv_t*)calloc(1, sizeof(*s));
    if (!s) {
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->ctx = ctx;
    s->rt = JS_GetRuntime(ctx);
    s->fd = -1;
    s->handler = JS_UNDEFINED;
    s->aio = dyn_net_reactor_acquire(ctx);
    if (!s->aio) {
        free(s);
        if (host)
            JS_FreeCString(ctx, host);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->fd = dyn_aio_udp_bind(s->aio, host ? host : "127.0.0.1", (uint16_t)port);
    if (host)
        JS_FreeCString(ctx, host);
    if (s->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(s);
        return JS_ThrowInternalError(ctx, "DNSServer: bind failed");
    }
    if (getsockname(s->fd, (struct sockaddr*)&sa, &sl) == 0)
        s->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, new_target, dyn_dnssrv_class_id, s, dyn_dnssrv_dispose);
}

static JSValue dyn_dnssrv_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)dyn_res_native(ctx, this_val,
        dyn_dnssrv_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (argc > 0) {
        JS_FreeValue(ctx, s->handler);
        s->handler = JS_DupValue(ctx, argv[0]);
    }
    if (!s->started) {
        if (dyn_aio_recvfrom(s->aio, s->fd, dnssrv_on_query, s) < 0)
            return JS_ThrowInternalError(ctx, "DNSServer: recv failed");
        s->started = 1;
    }
    return JS_UNDEFINED;
}

static JSValue dyn_dnssrv_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_dnssrv_t* s = (dyn_dnssrv_t*)dyn_res_native(ctx, this_val,
        dyn_dnssrv_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_dnssrv_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_dnssrv_start),
    JS_CGETSET_DEF("port", dyn_dnssrv_get_port, NULL),
};

int dyn_dns_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_resolver_class_id,
            &dyn_resolver_class, dyn_resolver_proto,
            countof(dyn_resolver_proto),
            dyn_resolver_ctor, "DNSResolver")
        < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_dnssrv_class_id, &dyn_dnssrv_class,
        dyn_dnssrv_proto, countof(dyn_dnssrv_proto),
        dyn_dnssrv_ctor, "DNSServer");
}

void dyn_dns_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "DNSResolver");
    JS_AddModuleExport(ctx, m, "DNSServer");
}

#endif
