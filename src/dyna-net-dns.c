/*
 * DNS resolver, part of dyna:net. The RFC 1035 codec is src/core/dyn-dns.c;
 * this is the transport and the anti-spoofing.
 *
 * CACHE POISONING IS THE CLIENT'S PROBLEM, NOT THE PARSER'S. A UDP response is
 * trivially forged by anyone who can guess what to send, so an answer is
 * accepted only when ALL of these match the outstanding query:
 *   - the source address and port (the kernel enforces it: the socket is
 *     connect()ed to the server, so it drops datagrams from anyone else);
 *   - the 16-bit query ID, drawn from the OS CSPRNG rather than a counter;
 *   - the question name and type echoed back.
 * Dropping any one of them is the Kaminsky attack. The source port is random
 * because dyn_aio_udp_bind is given port 0 -- source-port randomisation is half
 * the entropy an off-path attacker has to guess.
 */
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

#define DNS_MAX_INFLIGHT 64

typedef struct dyn_resolver dyn_resolver_t;

typedef struct {
    int used;
    uint16_t id;
    char qname[DYN_DNS_MAX_NAME + 1];
    uint16_t qtype;
    JSValue cb;
    dyn_timer_id timer;
    dyn_resolver_t *owner;
    /* TCP fallback (RFC 1035 4.2.2) for a response that came back with TC set.
     * The stream is length-prefixed, so a partial read is normal and the
     * message is only complete once `have` covers the announced length. */
    int tcp_fd;
    uint8_t qmsg[512];
    uint16_t qlen;
    uint8_t *tcp_buf;
    size_t tcp_cap, have;
    int retried;              /* one fallback per query: TC on TCP is a loop */
} dns_pending_t;

struct dyn_resolver {
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd;
    struct sockaddr_in server;
    uint32_t timeout_ms;
    dyn_prng_t rng;            /* seeded from the OS CSPRNG at construction */
    dns_pending_t q[DNS_MAX_INFLIGHT];
    dyn_timers_t *timers;
    int started;
};

static JSClassID dyn_resolver_class_id;
static const JSClassDef dyn_resolver_class = {
    "DNSResolver", .finalizer = dyn_res_finalizer,
};

static void dns_settle(dyn_resolver_t *r, dns_pending_t *p, JSValue err,
                       JSValue val)
{
    JSContext *ctx = r->ctx;
    JSValue cb = p->cb, ret;
    JSValueConst a[2];

    p->used = 0;                     /* free the slot BEFORE calling out */
    p->cb = JS_UNDEFINED;
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
        a[0] = err; a[1] = val;
        ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, a);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, val);
}

static void dns_on_timeout(void *arg)
{
    dns_pending_t *p = (dns_pending_t *)arg;
    if (!p->used)
        return;
    p->timer = DYN_TIMER_NONE;       /* it just fired; do not cancel it */
    dns_settle(p->owner, p, JS_NewString(p->owner->ctx, "query timed out"),
               JS_UNDEFINED);
}

/* Turn an A/AAAA record into a printable address; other types return the name
 * or NULL, so an unknown type is skipped rather than guessed at. */
static JSValue dns_rr_to_js(JSContext *ctx, const dyn_dns_rr_t *rr)
{
    char ip[64];
    JSValue o;

    if (rr->type == DYN_DNS_T_A && rr->rdlen == 4) {
        if (!inet_ntop(AF_INET, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
    } else if (rr->type == DYN_DNS_T_AAAA && rr->rdlen == 16) {
        if (!inet_ntop(AF_INET6, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
    } else {
        return JS_UNDEFINED;         /* a type we do not decode */
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, rr->name));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, rr->type));
    JS_SetPropertyStr(ctx, o, "ttl", JS_NewInt64(ctx, rr->ttl));
    JS_SetPropertyStr(ctx, o, "address", JS_NewString(ctx, ip));
    return o;
}

static void dns_tcp_fallback(dyn_resolver_t *r, dns_pending_t *p);

/* Match a response to an outstanding query and settle it. `p` may be NULL, in
 * which case the ID is looked up. Shared by the UDP and TCP paths so the
 * anti-spoofing checks cannot drift apart between them. */
static void dns_handle_response(dyn_resolver_t *r, const uint8_t *buf,
                                unsigned len, dns_pending_t *p)
{
    JSContext *ctx = r->ctx;
    dyn_dns_hdr_t h;
    char qname[DYN_DNS_MAX_NAME + 1];
    size_t off;
    JSValue arr;
    uint32_t idx = 0;
    int i;

    if (len < DYN_DNS_HDR_LEN)
        return;
    if (dyn_dns_hdr_decode(buf, len, &h) < 0)
        return;
    if (!(h.flags & 0x8000))
        return;                      /* not a response */
    if (!p) {
        for (i = 0; i < DNS_MAX_INFLIGHT; i++)
            if (r->q[i].used && r->q[i].id == h.id) { p = &r->q[i]; break; }
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
            return;                  /* answers a question we did not ask */
        off = (size_t)after + 4;
    }

    /* TC: the answer did not fit in a datagram. RFC 1035 4.2.2 -- retry over
     * TCP, which has no size limit. Once only: a TC response ON TCP would
     * otherwise loop forever. */
    if ((h.flags & 0x0200) && !p->retried) {
        p->retried = 1;
        dns_tcp_fallback(r, p);
        return;
    }

    if ((h.flags & 0x000f) != 0) {   /* RCODE */
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
    for (i = 0; i < (int)h.ancount; i++) {
        dyn_dns_rr_t rr;
        JSValue v;
        if (dyn_dns_rr_decode(buf, len, &off, &rr) < 0)
            break;
        v = dns_rr_to_js(ctx, &rr);
        if (JS_IsUndefined(v))
            continue;
        JS_DefinePropertyValueUint32(ctx, arr, idx++, v, JS_PROP_C_W_E);
    }
    dns_settle(r, p, JS_NULL, arr);
}

static void dns_on_message(dyn_aio_t *aio, int res, const uint8_t *buf,
                           unsigned len, const struct sockaddr *peer,
                           unsigned peerlen, void *ud)
{
    dyn_resolver_t *r = (dyn_resolver_t *)ud;
    (void)aio; (void)peerlen;

    if (res < 0 || len < DYN_DNS_HDR_LEN)
        return;
    /* The socket is connect()ed, so the kernel has already dropped anything not
     * from the server -- this is belt and braces, and cheap. */
    if (peer && peer->sa_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)peer;
        if (sa->sin_addr.s_addr != r->server.sin_addr.s_addr ||
            sa->sin_port != r->server.sin_port)
            return;
    }
    dns_handle_response(r, buf, len, NULL);
}

/* ---- TCP fallback ------------------------------------------------------ */

static void dns_tcp_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                         unsigned len, void *ud)
{
    dns_pending_t *p = (dns_pending_t *)ud;
    dyn_resolver_t *r = p->owner;
    size_t want;
    (void)aio;

    if (!p->used)
        return;
    if (res <= 0) {
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: connection closed"),
                   JS_UNDEFINED);
        return;
    }
    /* Grow only to what the announced length needs. The prefix is attacker
     * -controlled, so it is bounded before it drives an allocation. */
    if (p->have + len > p->tcp_cap) {
        size_t nc = p->tcp_cap ? p->tcp_cap * 2 : 2048;
        uint8_t *nb;
        while (nc < p->have + len)
            nc *= 2;
        if (nc > 65538) {                 /* 2-byte prefix + 65535 max message */
            dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: oversize"),
                       JS_UNDEFINED);
            return;
        }
        nb = (uint8_t *)realloc(p->tcp_buf, nc);
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
        return;                            /* not even the length yet */
    want = ((size_t)p->tcp_buf[0] << 8) | p->tcp_buf[1];
    /* The prefix EXCLUDES itself (RFC 1035 4.2.2), so the frame is want + 2.
     * Writing `have < want` misparses only while `want <= have < want + 2`;
     * test_net_dns drives exactly that window and the fault returns an address
     * two octets short. */
    if (p->have < want + 2)
        return;                            /* a partial read is normal here */
    dns_handle_response(r, p->tcp_buf + 2, (unsigned)want, p);
}

static void dns_tcp_connected(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *ud)
{
    dns_pending_t *p = (dns_pending_t *)ud;
    dyn_resolver_t *r = p->owner;
    uint8_t *framed;
    (void)aio; (void)buf; (void)len;

    if (!p->used)
        return;
    if (res < 0) {
        dns_settle(r, p, JS_NewString(r->ctx, "TCP fallback: connect failed"),
                   JS_UNDEFINED);
        return;
    }
    framed = (uint8_t *)malloc(p->qlen + 2u);
    if (!framed) {
        dns_settle(r, p, JS_NewString(r->ctx, "out of memory"), JS_UNDEFINED);
        return;
    }
    framed[0] = (uint8_t)(p->qlen >> 8);
    framed[1] = (uint8_t)p->qlen;
    memcpy(framed + 2, p->qmsg, p->qlen);
    {
        /* Ours on every path: dyn_aio_send copies the remainder it could not
         * send inline, it does not adopt this buffer. */
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

static void dns_tcp_fallback(dyn_resolver_t *r, dns_pending_t *p)
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

static void dns_drain_hook(void *udata)
{
    dyn_resolver_t *r = (dyn_resolver_t *)udata;
    if (r->timers)
        dyn_timer_run(r->timers, dyn_timer_now_ms());
}

static void dyn_resolver_dispose(void *native)
{
    dyn_resolver_t *r = (dyn_resolver_t *)native;
    int i;
    if (!r)
        return;
    dyn_net_off_drain(r);
    if (r->timers) {
        dyn_timers_free(r->timers);
        r->timers = NULL;
    }
    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (r->q[i].used)
            JS_FreeValue(r->ctx, r->q[i].cb);
    if (r->aio) {
        if (r->fd >= 0)
            dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(r->ctx);
    }
    free(r);
}

static JSValue dyn_resolver_ctor(JSContext *ctx, JSValueConst new_target,
                                 int argc, JSValueConst *argv)
{
    dyn_resolver_t *r;
    const char *server = NULL;
    int64_t port = 53, timeout = 5000;
    uint64_t seed;
    JSValue v;
    (void)new_target;

    if (argc > 0 && JS_IsObject(argv[0])) {
        v = JS_GetPropertyStr(ctx, argv[0], "server");
        server = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v); if (server) JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "timeoutMs");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &timeout, v)) {
            JS_FreeValue(ctx, v); if (server) JS_FreeCString(ctx, server);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (port < 1 || port > 65535 || timeout < 1) {
        if (server) JS_FreeCString(ctx, server);
        return JS_ThrowRangeError(ctx, "DNSResolver: bad port or timeoutMs");
    }

    r = (dyn_resolver_t *)calloc(1, sizeof(*r));
    if (!r) { if (server) JS_FreeCString(ctx, server); return JS_ThrowOutOfMemory(ctx); }
    r->ctx = ctx;
    r->fd = -1;
    r->timeout_ms = (uint32_t)timeout;
    /* Query IDs come from the KERNEL CSPRNG, not a counter and not time: a
     * predictable ID is the other half of a spoofed answer. */
    dyn_os_entropy(&seed, sizeof(seed));
    dyn_prng_seed(&r->rng, seed);

    memset(&r->server, 0, sizeof(r->server));
    r->server.sin_family = AF_INET;
    r->server.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server ? server : "127.0.0.1",
                  &r->server.sin_addr) != 1) {
        if (server) JS_FreeCString(ctx, server);
        free(r);
        return JS_ThrowTypeError(ctx, "DNSResolver: server must be an IPv4 address");
    }
    if (server) JS_FreeCString(ctx, server);

    r->aio = dyn_net_reactor_acquire(ctx);
    if (!r->aio) { free(r); return JS_ThrowOutOfMemory(ctx); }
    /* Port 0: SOURCE-PORT RANDOMISATION. Half the entropy an off-path attacker
     * must guess lives here, not in the query ID. */
    r->fd = dyn_aio_udp_bind(r->aio, "0.0.0.0", 0);
    if (r->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(r);
        return JS_ThrowInternalError(ctx, "DNSResolver: bind failed");
    }
    /* connect() a UDP socket so the KERNEL drops datagrams from anyone but the
     * server -- the cheapest and most reliable source check there is. */
    if (connect(r->fd, (struct sockaddr *)&r->server, sizeof(r->server)) != 0) {
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
    return dyn_res_wrap(ctx, dyn_resolver_class_id, r, dyn_resolver_dispose);
}

static JSValue dyn_resolver_query(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_resolver_t *r;
    const char *name = NULL;
    int64_t type = DYN_DNS_T_A;
    uint8_t msg[512];
    dns_pending_t *p = NULL;
    int i, n, tries;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "query(name, type, callback)");
    /* Coerce every argument before resolving the handle. */
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &type, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    r = (dyn_resolver_t *)dyn_res_native(ctx, this_val, dyn_resolver_class_id);
    if (!r) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    for (i = 0; i < DNS_MAX_INFLIGHT; i++)
        if (!r->q[i].used) { p = &r->q[i]; break; }
    if (!p) {
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "DNSResolver: too many queries in flight");
    }
    if (strlen(name) > DYN_DNS_MAX_NAME) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "DNSResolver: name too long");
    }

    /* A fresh random ID, and never one already outstanding: a collision would
     * let one answer settle the wrong query. */
    for (tries = 0; tries < 64; tries++) {
        int clash = 0;
        p->id = (uint16_t)(dyn_prng_next(&r->rng) & 0xffff);
        for (i = 0; i < DNS_MAX_INFLIGHT; i++)
            if (r->q[i].used && r->q[i].id == p->id) { clash = 1; break; }
        if (!clash)
            break;
    }
    n = dyn_dns_build_query(p->id, name, (uint16_t)type, msg, sizeof(msg));
    if (n < 0) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "DNSResolver: %s", dyn_dns_strerror(n));
    }
    memcpy(p->qmsg, msg, (size_t)n);   /* kept for a possible TCP retry */
    p->qlen = (uint16_t)n;
    p->tcp_fd = -1;
    p->tcp_buf = NULL;
    p->tcp_cap = p->have = 0;
    p->retried = 0;
    snprintf(p->qname, sizeof(p->qname), "%s", name);
    JS_FreeCString(ctx, name);
    p->qtype = (uint16_t)type;
    p->owner = r;
    p->cb = (argc >= 3) ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    p->used = 1;

    if (!r->started) {
        if (dyn_aio_recvfrom(r->aio, r->fd, dns_on_message, r) < 0 ||
            dyn_net_on_drain(dns_drain_hook, r) < 0) {
            p->used = 0;
            JS_FreeValue(ctx, p->cb);
            return JS_ThrowInternalError(ctx, "DNSResolver: recv failed");
        }
        r->started = 1;
    }
    p->timer = dyn_timer_add(r->timers, dyn_timer_now_ms(), r->timeout_ms,
                             dns_on_timeout, p);
    /* NULL peer: the socket is connect()ed to the server. */
    if (dyn_aio_sendto(r->aio, r->fd, msg, (size_t)n, NULL, 0) < 0) {
        JSValue e = JS_NewString(ctx, "send failed");
        dns_settle(r, p, e, JS_UNDEFINED);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_resolver_proto[] = {
    JS_CFUNC_DEF("query", 3, dyn_resolver_query),
};


/* ---- DNSServer ---------------------------------------------------------
 *
 * A UDP DNS server is a REFLECTION AMPLIFIER by construction: the source
 * address is unverified, so an attacker sends a small query with a forged
 * source and the victim receives the large answer. Two defences here, and
 * neither is optional:
 *
 *   - AMPLIFICATION CAP. A response is never sent if it would be more than
 *     DNS_AMP_FACTOR times the query that provoked it. An attacker gains
 *     nothing by reflecting off us that they could not achieve by sending the
 *     same bytes directly.
 *   - PER-SOURCE RATE LIMIT. A small token bucket per address, so one forged
 *     source cannot be used to flood a victim.
 *
 * TRUNCATION IS THE RFC'S OWN ANSWER: when a response does not fit, we set TC
 * and send the header alone, which tells a real client to retry over TCP and
 * gives an attacker nothing to amplify with.
 */
#define DNS_AMP_FACTOR   4      /* response <= 4x the query, or truncate */
#define DNS_RRL_SLOTS    64
#define DNS_RRL_PER_SEC  20

typedef struct {
    uint32_t addr;              /* network order; 0 = free slot */
    uint32_t tokens;
    uint64_t refilled_ms;
} dns_rrl_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd;
    uint16_t port;
    int started;
    JSValue handler;            /* (name, type) -> address string | null */
    dns_rrl_t rrl[DNS_RRL_SLOTS];
} dyn_dnssrv_t;

static JSClassID dyn_dnssrv_class_id;
static const JSClassDef dyn_dnssrv_class = {
    "DNSServer", .finalizer = dyn_res_finalizer,
};

/* Token bucket keyed on the source address. Returns 1 if the query may be
 * answered. A full table evicts the oldest slot rather than failing open. */
static int dns_rrl_allow(dyn_dnssrv_t *s, uint32_t addr)
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

static void dnssrv_on_query(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, const struct sockaddr *peer,
                            unsigned peerlen, void *ud)
{
    dyn_dnssrv_t *s = (dyn_dnssrv_t *)ud;
    JSContext *ctx = s->ctx;
    dyn_dns_hdr_t h;
    char qname[DYN_DNS_MAX_NAME + 1];
    uint8_t out[512];
    uint16_t qtype;
    int after, off, n_ans = 0;
    JSValue r;
    (void)aio;

    if (res < 0 || len < DYN_DNS_HDR_LEN || !peer)
        return;
    if (peer->sa_family == AF_INET &&
        !dns_rrl_allow(s, ((const struct sockaddr_in *)peer)->sin_addr.s_addr))
        return;                      /* rate limited: answer nothing at all */
    if (dyn_dns_hdr_decode(buf, len, &h) < 0 || (h.flags & 0x8000))
        return;                      /* malformed, or a response sent to us */

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
        r = JS_Call(ctx, s->handler, JS_UNDEFINED, 2, a);
        JS_FreeValue(ctx, (JSValue)a[0]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, r);
            return;
        }
        if (JS_IsString(r)) {
            const char *ip = JS_ToCString(ctx, r);
            uint8_t v4[4], v6[16];
            if (ip) {
                if (qtype == DYN_DNS_T_A && inet_pton(AF_INET, ip, v4) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                                                DYN_DNS_T_A, 60, v4, 4);
                    if (no > 0) { off = no; n_ans = 1; }
                } else if (qtype == DYN_DNS_T_AAAA &&
                           inet_pton(AF_INET6, ip, v6) == 1) {
                    int no = dyn_dns_add_answer(out, sizeof(out), (size_t)off,
                                                DYN_DNS_T_AAAA, 60, v6, 16);
                    if (no > 0) { off = no; n_ans = 1; }
                }
                JS_FreeCString(ctx, ip);
            }
        }
        JS_FreeValue(ctx, r);
    }
    dyn_dns_set_ancount(out, (uint16_t)n_ans);

    /* AMPLIFICATION CAP. Over the factor, drop the answers and set TC: the RFC
     * already says that means "retry over TCP", and TCP requires a handshake
     * the attacker cannot complete with a forged source.
     * NOT REACHABLE TODAY: this handler adds at most one 16-byte answer, so a
     * response is query+16 and can never exceed 4x a real query. It is here for
     * when multi-record answers land, and removing it fails no test -- see
     * NET_PLAN.md rather than assuming it is proven. */
    if ((size_t)off > (size_t)len * DNS_AMP_FACTOR) {
        out[2] |= 0x02;              /* TC */
        dyn_dns_set_ancount(out, 0);
        off = (int)(DYN_DNS_HDR_LEN +
                    ((size_t)after + 4 - DYN_DNS_HDR_LEN));
    }
    dyn_aio_sendto(s->aio, s->fd, out, (size_t)off, peer, peerlen);
}

static void dyn_dnssrv_dispose(void *native)
{
    dyn_dnssrv_t *s = (dyn_dnssrv_t *)native;
    if (!s)
        return;
    if (s->aio) {
        if (s->fd >= 0)
            dyn_aio_close(s->aio, s->fd);
        dyn_net_reactor_release(s->ctx);
    }
    JS_FreeValue(s->ctx, s->handler);
    free(s);
}

static JSValue dyn_dnssrv_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_dnssrv_t *s;
    int64_t port = 0;
    const char *host = NULL;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    JSValue v;
    (void)new_target;

    if (argc > 0 && JS_IsObject(argv[0])) {
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &port, v)) {
            JS_FreeValue(ctx, v); return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        host = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (port < 0 || port > 65535) {
        if (host) JS_FreeCString(ctx, host);
        return JS_ThrowRangeError(ctx, "DNSServer: port must be 0..65535");
    }
    s = (dyn_dnssrv_t *)calloc(1, sizeof(*s));
    if (!s) { if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    s->ctx = ctx;
    s->fd = -1;
    s->handler = JS_UNDEFINED;
    s->aio = dyn_net_reactor_acquire(ctx);
    if (!s->aio) { free(s); if (host) JS_FreeCString(ctx, host); return JS_ThrowOutOfMemory(ctx); }
    s->fd = dyn_aio_udp_bind(s->aio, host ? host : "127.0.0.1", (uint16_t)port);
    if (host) JS_FreeCString(ctx, host);
    if (s->fd < 0) {
        dyn_net_reactor_release(ctx);
        free(s);
        return JS_ThrowInternalError(ctx, "DNSServer: bind failed");
    }
    if (getsockname(s->fd, (struct sockaddr *)&sa, &sl) == 0)
        s->port = ntohs(sa.sin_port);
    return dyn_res_wrap(ctx, dyn_dnssrv_class_id, s, dyn_dnssrv_dispose);
}

static JSValue dyn_dnssrv_start(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_dnssrv_t *s = (dyn_dnssrv_t *)dyn_res_native(ctx, this_val,
                                                     dyn_dnssrv_class_id);
    if (!s) return JS_EXCEPTION;
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

static JSValue dyn_dnssrv_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_dnssrv_t *s = (dyn_dnssrv_t *)dyn_res_native(ctx, this_val,
                                                     dyn_dnssrv_class_id);
    if (!s) return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_dnssrv_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_dnssrv_start),
    JS_CGETSET_DEF("port", dyn_dnssrv_get_port, NULL),
};

int dyn_dns_register(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_resolver_class_id,
                           &dyn_resolver_class, dyn_resolver_proto,
                           countof(dyn_resolver_proto),
                           dyn_resolver_ctor, "DNSResolver") < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_dnssrv_class_id, &dyn_dnssrv_class,
                              dyn_dnssrv_proto, countof(dyn_dnssrv_proto),
                              dyn_dnssrv_ctor, "DNSServer");
}

void dyn_dns_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "DNSResolver");
    JS_AddModuleExport(ctx, m, "DNSServer");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
