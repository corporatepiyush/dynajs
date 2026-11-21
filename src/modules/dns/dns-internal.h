#ifndef DNS_INTERNAL_H
#define DNS_INTERNAL_H

#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-dns.h"
#include "core/dyn-prng.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

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
#define DNS_CACHE_SLOTS 32

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
    int pinned;
    JSValue self;
};

extern JSClassID dyn_resolver_class_id;

extern _Thread_local int dns_in_final;

void dyn_resolver_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

JSValue dns_shape_records(JSContext* ctx, JSValueConst records,
    int shape);

void dns_promise_settle(JSContext* ctx, dns_pending_t* p, JSValue err,
    JSValue val);

int dns_rr_owner_is(const dyn_dns_rr_t* rr, const char* name);

JSValue dns_rr_to_js(JSContext* ctx, const dyn_dns_rr_t* rr,
    const uint8_t* msg, size_t len);

JSValue dns_clone_records(JSContext* ctx, JSValueConst records);

void dns_cache_free_entry_rt(JSRuntime* rt, dns_cache_entry_t* e);

void dns_cache_store(dyn_resolver_t* r, const char* name,
    uint16_t qtype, uint32_t min_ttl, JSValue records);

JSValue dns_cache_fetch(dyn_resolver_t* r, const char* name,
    uint16_t qtype);

int dns_cache_deliver(JSContext* ctx, JSValue records, int shape,
    JSValueConst cb, JSValue resolve, JSValue reject,
    int is_promise);

void dyn_resolver_teardown(dyn_resolver_t* r);

JSValue dyn_resolver_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dns_submit(JSContext* ctx, dyn_resolver_t* r, const char* name,
    int64_t type, int shape, int is_promise,
    JSValueConst cb, JSValueConst resolve,
    JSValueConst reject);

JSValue dyn_resolver_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_resolver_lookup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_resolver_cname(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_resolver_mx(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_resolver_txt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_resolver_ns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_dnssrv_class_id;

void dyn_dnssrv_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

JSValue dyn_dnssrv_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_dnssrv_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_dnssrv_get_port(JSContext* ctx, JSValueConst this_val);

#endif

#endif
