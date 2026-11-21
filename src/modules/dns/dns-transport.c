#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

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
        dyn_net_handler_result(ctx, ret, NET_HK_DNS_QUERY);
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

static void dns_chase_cname(const uint8_t* buf, size_t len, size_t start,
    uint16_t ancount, uint16_t qtype, char* want, size_t wantcap)
{
    int hops, i;
    if (qtype == DYN_DNS_T_CNAME)
        return;
    for (hops = 0; hops < 8; hops++) {
        size_t off = start;
        char target[DYN_DNS_MAX_NAME + 1];
        int have_cname = 0;
        for (i = 0; i < (int)ancount; i++) {
            dyn_dns_rr_t rr;
            size_t rdoff;
            int t;
            if (dyn_dns_rr_decode(buf, len, &off, &rr) < 0)
                return;
            if (rr.cls != 1 || !dns_rr_owner_is(&rr, want))
                continue;
            if (rr.type == qtype)
                return;
            if (rr.type != DYN_DNS_T_CNAME || have_cname)
                continue;
            rdoff = (size_t)(rr.rdata - buf);
            if (rdoff >= len || (size_t)rr.rdlen > len - rdoff)
                return;
            t = dyn_dns_name_decode(buf, len, rdoff, target, sizeof(target));
            if (t < 0 || (size_t)t > rdoff + rr.rdlen || strlen(target) >= wantcap)
                return;
            have_cname = 1;
        }
        if (!have_cname)
            return;
        memcpy(want, target, strlen(target) + 1);
    }
}

static void dns_tcp_fallback(dyn_resolver_t* r, dns_pending_t* p);

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
        char want[DYN_DNS_MAX_NAME + 1];
        snprintf(want, sizeof(want), "%s", p->qname);
        dns_chase_cname(buf, len, off, h.ancount, p->qtype, want, sizeof(want));
        for (i = 0; i < (int)h.ancount; i++) {
            dyn_dns_rr_t rr;
            JSValue v;
            if (dyn_dns_rr_decode(buf, len, &off, &rr) < 0)
                break;
            if (!dns_rr_owner_is(&rr, want) || rr.type != p->qtype
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

static void dns_drain_hook(void* udata)
{
    dyn_resolver_t* r = (dyn_resolver_t*)udata;
    int i, busy = 0;
    if (!r->timers)
        return;
    r->in_callback = 1;
    dyn_timer_run(r->timers, dyn_timer_now_ms());
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        busy |= r->q[i].used;
    if (r->pinned && !busy && !r->closing) {
        r->pinned = 0;
        dyn_res_unpin(r->rt, r->self, dyn_resolver_class_id);
    }
    r->in_callback = 0;
    if (r->closing)
        dyn_resolver_teardown(r);
}

JSValue dns_submit(JSContext* ctx, dyn_resolver_t* r, const char* name,
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
    if (!r->pinned && dyn_res_pin(ctx, r->self, dyn_resolver_class_id) == 0)
        r->pinned = 1;
    if (dyn_aio_sendto(r->aio, r->fd, msg, p->qlen, NULL, 0) < 0) {
        JSValue e = JS_NewString(ctx, "send failed");
        dns_settle(r, p, e, JS_UNDEFINED);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

#endif
