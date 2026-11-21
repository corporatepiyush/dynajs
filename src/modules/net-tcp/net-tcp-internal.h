#ifndef NET_TCP_INTERNAL_H
#define NET_TCP_INTERNAL_H

#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-timer.h"
#include "core/dyn-frame.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "dyna-tls.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);

typedef struct dyn_tcp dyn_tcp_t;

typedef struct dyn_frame_spec dyn_frame_spec_t;

#define TCP_NHANDLERS 6

typedef struct eb_attempt {
    dyn_tcp_t* owner;
    int idx;
    int fd;
} eb_attempt_t;

typedef struct dyn_tcp_conn {
    dyn_tcp_t* owner;
    int fd;
    int closed;
    int refs;
    int drain_armed;
    uint64_t hwm;
    JSValue jsobj;
    struct dyn_tcp_conn *lnext, *lprev;
    uint64_t last_ms;
    int paused;
    int eof;
    int delivering;
    JSValue tap;
    dyn_frame_t* dec;
    JSValue* fq;
    size_t fq_head;
    size_t fq_len;
    size_t fq_cap;
    int frame_err;
    int frame_err_told;
    int eof_pending;
    int pumping;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    int tls_up;
#endif
} dyn_tcp_conn_t;

struct dyn_tcp {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    int started;
    int client;
    char* path;
    int path_bound;
    dev_t path_dev;
    ino_t path_ino;
    JSValue h_connect, h_data, h_close, h_drain, h_frame, h_error;
    dyn_frame_spec_t* spec;
    uint64_t hwm;
    JSValue self_pending;
    JSValue self_listen;
    JSValue self_weak;

    dyn_tcp_conn_t* pending_conn;

    dyn_tcp_conn_t* conns;
    int nconns;
    int max_conns;
    uint64_t idle_ms;
    uint64_t connect_deadline_ms;
    int hooked;
    int released;
    uint64_t n_refused;
    uint64_t n_idle_closed;
    eb_attempt_t eb[2];
    int in_cb;
    int closing;
    uint64_t eb_deadline_ms;
    int eb_done;
    int eb_err;
    struct eb_dns* eb_dns;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls_ctx;
    char* tls_servername;
#endif
};

extern JSClassID dyn_tcp_class_id;

extern JSClassID dyn_tcp_conn_class_id;

void dyn_tcp_teardown(dyn_tcp_t* t);

static inline int tcp_gone(dyn_tcp_t* t)
{
    if (t->closing && t->in_cb == 0) {
        dyn_tcp_teardown(t);
        return 1;
    }
    return 0;
}

void dyn_tcp_conn_finalizer(JSRuntime* rt, JSValue val);

void tcp_conn_drop(JSRuntime* rt, dyn_tcp_conn_t* c);

static inline void tcp_release_pending(JSRuntime* rt, dyn_tcp_t* t);

void tcp_client_idle(dyn_tcp_t* t);

void tcp_hold_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

#ifdef CONFIG_TLS
int tls_flush(dyn_tcp_conn_t* c);
#endif

void tcp_list_unlink(dyn_tcp_conn_t* c);

void tcp_conn_close(dyn_tcp_conn_t* c);

void tcp_conn_sent(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

JSValue dyn_tcp_conn_buffered(JSContext* ctx, JSValueConst this_val);

JSValue dyn_tcp_conn_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_tcp_conn_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_tcp_conn_pause(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_tcp_conn_paused(JSContext* ctx, JSValueConst this_val);

JSValue dyn_tcp_conn_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_tcp_conn_streams(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void dyn_tcp_conn_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

void tcp_report_connect(dyn_tcp_t* t, JSValueConst obj, const char* err);

#ifdef CONFIG_TLS
void tls_fail(dyn_tcp_conn_t* c, const char* fallback);

int tcp_setup_tls_server(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts);

int tcp_setup_tls(JSContext* ctx, dyn_tcp_t* t, JSValueConst opts,
    const char* host);
#endif

int tcp_conn_start(dyn_tcp_t* t, dyn_tcp_conn_t* c);

void tcp_sweep(void* udata);

static inline void tcp_release_pending(JSRuntime* rt, dyn_tcp_t* t)
{
    JSValue v = t->self_pending;
    t->self_pending = JS_UNDEFINED;
    JS_RemoveShutdownSweep(rt, tcp_hold_sweep, t);
    JS_FreeValueRT(rt, v);
}

void tcp_listen_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static inline void tcp_release_listen(JSRuntime* rt, dyn_tcp_t* t)
{
    JSValue v = t->self_listen;
    if (JS_IsUndefined(v))
        return;
    t->self_listen = JS_UNDEFINED;
    JS_RemoveShutdownSweep(rt, tcp_listen_sweep, t);
    JS_FreeValueRT(rt, v);
}

void tcp_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

int eb_resolve(const char* host, uint16_t port,
    char out[2][INET6_ADDRSTRLEN], int numeric_only);

void eb_sweep(void* udata);

typedef struct eb_dns {
    dyn_tcp_t* owner;
    char* host;
    uint16_t port;
    int failed;
    int sync;
    int held;
    JSRuntime* rt;
    uint64_t fallback;
    char ips[2][INET6_ADDRSTRLEN];
} eb_dns_t;

int eb_arm(dyn_tcp_t* t, char ips[2][INET6_ADDRSTRLEN], uint16_t port,
    uint64_t fallback);

void eb_dns_free(eb_dns_t* d);

void eb_dns_work(void* p);

void eb_dns_done(void* p);

void dyn_tcp_dispose(void* native);

dyn_tcp_t* tcp_new(JSContext* ctx);

static inline int tcp_arm_sweep(dyn_tcp_t* t)
{
    if (t->hooked || (!t->idle_ms && !t->connect_deadline_ms))
        return 0;
    if (dyn_net_on_drain(tcp_sweep, t) < 0)
        return -1;
    t->hooked = 1;
    return 0;
}

int tcp_read_bounds(JSContext* ctx, dyn_tcp_t* t, JSValueConst o,
    int is_client);

const char* tcp_unix_path(JSContext* ctx, JSValueConst v);

int tcp_collect_handlers(JSContext* ctx, JSValueConst h, JSValue out[TCP_NHANDLERS]);

void tcp_install_handlers(dyn_tcp_t* t, JSValue v[TCP_NHANDLERS]);

int tcp_check_decoder(JSContext* ctx, dyn_tcp_t* t, const char* fn);

extern JSClassID dyn_frame_spec_class_id;

extern const JSCFunctionListEntry dyn_frame_funcs[3];

void frame_spec_unref(dyn_frame_spec_t* s);

dyn_frame_spec_t* frame_spec_ref(dyn_frame_spec_t* s);

dyn_frame_spec_t* frame_spec_from(JSContext* ctx, JSValueConst v, const char* fn);

dyn_frame_t* frame_spec_instantiate(const dyn_frame_spec_t* s);

int dyn_frame_spec_register(JSContext* ctx);

JSValue dyn_tcp_conn_set_decoder(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void tcp_conn_release(JSRuntime* rt, dyn_tcp_conn_t* c);

JSValue dyn_tcp_connect_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_tcp_connect_happy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct dyn_udp {
    JSValue h_message;
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    int started;
    int dead;
    uint16_t port;
    uint8_t v6;
} dyn_udp_t;

_Static_assert(sizeof(dyn_udp_t) <= 56,
    "dyn_udp_t regained padding: reorder largest-first");

extern JSClassID dyn_udp_class_id;

void dyn_udp_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

JSValue dyn_udp_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_udp_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_udp_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_udp_get_port(JSContext* ctx, JSValueConst this_val);

#endif

#endif
