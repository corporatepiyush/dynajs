#ifndef HTTP_INTERNAL_H
#define HTTP_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "cutils.h"
#include "dyna-tls.h"
#include "dyna-evloop.h"
#include "dyna-aio.h"
#include "dyna-libc.h"
#include "core/dyn-pool.h"
#include "core/dyn-timer.h"
#include "core/dyn-thread-name.h"
#include "http-scan.h"
#include "http-static-open.h"
#include "http-conn.h"

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#if defined(__linux__) && defined(CONFIG_IO_URING)
#include <liburing.h>
#define DYN_HTTP_HAVE_URING 1
#endif

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-compress.h"
#include "core/dyn-prng.h"
#include "core/dyn-ds.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include "core/dyn-simdk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_HTTP_ERR_URL 1
#define DYN_HTTP_ERR_RESOLVE 2
#define DYN_HTTP_ERR_CONNECT 3
#define DYN_HTTP_ERR_SEND 4
#define DYN_HTTP_ERR_RECV 5
#define DYN_HTTP_ERR_PARSE 6
#define DYN_HTTP_ERR_OOM 7
#define DYN_HTTP_ERR_TOO_LARGE 8
#define DYN_HTTP_ERR_TLS 9
#define DYN_HTTP_ERR_CANCEL 10
#define DYN_HTTP_ERR_TRUNC 11
#define DYN_HTTP_DEFAULT_TIMEOUT_MS 15000
#define DYN_HTTP_CONN_QUEUE_CAP 256
#define DYN_APP_OUT_LOW (256 * 1024)
#define DYN_APP_OUT_TOTAL (64 * 1024 * 1024)
#define DYN_APP_UP_RATE_MS 1000

extern const char* const http_env_keys[4];

extern const char* const http_ws_handler_keys[4];

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} dyn_bytes_t;

int dyn_bytes_reserve(dyn_bytes_t* b, size_t extra);

int dyn_bytes_append(dyn_bytes_t* b, const char* src, size_t n);

const char* dyn_memfind(const char* hay, size_t hlen,
    const char* needle, size_t nlen);

#include "core/dyn-sb.h"

int dyn_hm_one_range(const char* h, size_t b, size_t e, int64_t size,
    int64_t* start, int64_t* end);

int dyn_httpmsg_register(JSContext* ctx, JSModuleDef* m);

void dyn_httpmsg_add_exports(JSContext* ctx, JSModuleDef* m);

#define DYN_HTTP_FETCH_NAMES_N 7

extern const char* const dyn_http_fetch_names[DYN_HTTP_FETCH_NAMES_N];

extern JSClassID dyn_http_client_class_id;

typedef struct hc_job hc_job_t;

typedef struct {
    int64_t timeout_ms;
    size_t max_body;
    struct hc_job* jobs;
    JSValue self_pending;
    int n_async;
    JSContext* ctx;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls;
#endif
    struct dyn_cookiejar* jar;
    char* ca;
    unsigned origin_cap;
} dyn_http_client_t;

struct dyn_cookiejar {
    int refs;
    httpk_jar_t jar;
};

extern JSClassID dyn_cookiejar_class_id;
JSValue dyn_http_client_redirect_plan(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_http_client_use_cookies(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
int dyn_cookiejar_register(JSContext* ctx, JSModuleDef* m,
    int (*reg)(JSContext*, JSModuleDef*, JSClassID*, const JSClassDef*,
        const JSCFunctionListEntry*, int, JSCFunction*, const char*));

void dyn_http_client_unpin(dyn_http_client_t* cl);

void hc_jobs_orphan(dyn_http_client_t* cl);

void hc_pool_disown(const void* owner, int close_them);

#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
JSValue dyn_http_client_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

int dyn_method_valid(const char* m, size_t n);

void dyn_call_drop(JSContext* ctx, JSValueConst fn,
    JSValueConst this_val, int argc, JSValueConst* argv);

typedef struct {
    const char* path;
    size_t path_len;
    uint16_t port;
    uint8_t tls;
    char host[256];
} dyn_url_t;

_Static_assert(sizeof(dyn_url_t) <= 288,
    "dyn_url_t is a request-stack local: keep it near one page");

int dyn_parse_url(const char* url, dyn_url_t* out);

const char* dyn_host_bare(const char* host, char* buf, size_t cap);

char* dyn_headers_to_string(JSContext* ctx, JSValueConst headers,
    int* perr);

const char* hc_err_name(int code);

JSValue hc_error_value_named(JSContext* ctx, int code, const char* method,
    const char* url, const char* what);

JSValue hc_error_value(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy);

JSValue dyn_http_throw(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy);

JSValue dyn_http_throw_named(JSContext* ctx, int code,
    const char* method, const char* url,
    const char* what);

int dyn_head_parse(const char* raw, size_t head_len, httpk_head_t* h,
    httpk_hdr_t* small, size_t nsmall, httpk_hdr_t** heap);
JSValue dyn_headers_js(JSContext* ctx, const char* raw, const httpk_head_t* h);
void dyn_head_why(const char* raw, const httpk_head_t* h, int rc, char* why,
    size_t whyn);

#define HC_CANCELLED(cancel) \
    ((cancel) && atomic_load_explicit((cancel), memory_order_relaxed))

#define HC_RESP_MORE (-1)

typedef struct {
    const char* method;
    size_t max_body;
    size_t hdr_end;
    size_t scanned;
    size_t want;
    size_t content_length;
    size_t chunk_pos;
    size_t out_pos;
    size_t dropped;
    httpk_dechunk_t dc;
    unsigned interims;
    int phase;
    int chunked;
    int keepalive;
    int peer_closes;
    int reusable;
} hc_resp_t;

void hc_resp_init(hc_resp_t* st, const char* method, size_t max_body);

size_t hc_resp_room(const hc_resp_t* st, const dyn_bytes_t* resp);

int hc_resp_step(hc_resp_t* st, dyn_bytes_t* resp, int eof, int tls_cut,
    char* truncwhy, size_t truncwhy_n);

int dyn_read_response(hc_conn_t* conn, const char* method,
    size_t max_body, dyn_bytes_t* resp,
    size_t* phdr_end, int* pchunked,
    const _Atomic int* cancel,
    char* truncwhy, size_t truncwhy_n, int* preusable);

JSValue dyn_build_response_ex(JSContext* ctx, const char* raw, size_t len,
    size_t hdr_end, const char* method, const char* url, int bytes_only);
JSValue dyn_build_response(JSContext* ctx, const char* raw, size_t len,
    size_t hdr_end, const char* method, const char* url);

typedef struct {
    const char* method;
    char *url, *hdr, *body;
    size_t body_len;
    int64_t timeout_ms;
    size_t max_body;
    dyn_url_t u;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls_ctx;
#endif
    dyn_bytes_t resp;
    size_t hdr_end;
    int chunked;
    int err;
    char tlswhy[192];
    char truncwhy[128];
    const _Atomic int* cancel;
    int (*on_connect)(const char* ip, void* ud);
    void* on_connect_ud;
    char peer_ip[64];
    int reusable;
} hc_exch_t;

void hc_exchange(hc_exch_t* x);
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#ifdef CONFIG_TLS
int hc_tls_ensure(JSContext* ctx, dyn_http_client_t* cl, char* terr,
    size_t terr_n);
#endif
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
typedef struct {
    JSContext* ctx;
    JSValue fn;
} hc_onconn_t;

int hc_js_on_connect(const char* ip, void* ud);

JSValue dyn_http_client_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_client_post(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_client_request(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void dyn_http_async_hook(JSContext* ctx);

void dyn_http_async_release(JSContext* ctx);

struct hc_job {
    hc_exch_t x;
    char* ca;
    hc_conn_t conn;
    JSContext* ctx;
    dyn_http_client_t* cl;
    JSValue resolve, reject;
    JSValue on_connect_fn;
    hc_onconn_t onconn;
    char* method_own;
    struct dyn_aio* aio;
    int offloaded;
    struct hc_job* jnext;
    _Atomic int cancelled;
    int on_reactor;
    int astage;
    int a_reused;
    int a_retried;
    struct hc_pconn* pc;
    hc_resp_t arst;
    uint64_t a_idle_deadline_ms;
    uint64_t a_total_deadline_ms;
    struct hc_job* anext;
    int bytes_only;
    int stream;
    struct hc_sresp* sr;
    struct hc_origin* org;
    struct hc_job* qnext;
    int queued;
    unsigned cap;
    struct dyn_cookiejar* jar;
};

typedef struct hc_pconn {
    struct dyn_aio* aio;
    hc_job_t* job;
    void* owner;
    struct hc_pconn* next;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    dyn_tls_ctx_t* tls_ctx;
#endif
    uint64_t idle_deadline_ms;
    int fd;
    int is_tls;
    int sending;
    int send_failed;
    int closing;
    size_t sent_len;
    uint16_t port;
    char* ca;
    char host[256];
    char peer_ip[64];
} hc_pconn_t;

int wsc_hs_any_pending(void);

void wsc_hs_sweep(uint64_t now);

void hc_async_hook_sync(void);

JSValue hc_submit_async_ex(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val, int bytes_only);
JSValue hc_submit_async(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val);

JSValue dyn_http_client_request_pending(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_client_set_timeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_hc_stream_class_id;

JSValue dyn_hc_stream_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_client_get_stream(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_client_disconnect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_http_server_class_id;

typedef struct {
    char* path;
    int status;
    char* content_type;
    char* body;
    size_t body_len;
} dyn_route_t;

typedef struct dyn_route_idx dyn_route_idx_t;

typedef struct dyn_http_async dyn_http_async_t;

#define DYN_ACONN_MAX_CONNS_DEFAULT 8192

typedef struct dyn_aconn_s {
    dyn_http_async_t* srv;
    int fd;
    dyn_bytes_t in;
    dyn_bytes_t out;
    size_t out_off;
    size_t hdr_scan_from;
    int closing;
    int nreq;
    uint64_t last_ms;
    struct dyn_aconn_s *lnext, *lprev;
} dyn_aconn_t;

struct dyn_http_async {
    int listen_fd;
    uint16_t port;
    int backlog;

    dyn_route_t* routes;
    size_t n_routes;
    dyn_route_idx_t* route_idx;

    uint64_t idle_ms;
    int max_conns;
    size_t max_req;
    dyn_aconn_t* live;
    int nconns;
    uint64_t last_sweep_ms;
    atomic_ullong n_refused;

    dyn_evloop_t* loop;
    void* uring;
    pthread_t reactor;
    int started;
    atomic_int stop_flag;
    atomic_int spawn_ok;
    JSContext* ctx;
    int reactor_held;
    int borrowed;
    int accept_paused;
    int accept_errs;
    uint64_t accept_resume_ms;
    int nworkers;
    int reuseport;
    struct dyn_http_async** extra;
    int nextra;
};

void dyn_http_async_stop_internal(struct dyn_http_async* s);

int dyn_http_async_spawn(struct dyn_http_async* s);

const char* dyn_reason_phrase(int status);

dyn_route_idx_t* dyn_route_index_build(const dyn_route_t* routes, size_t n_routes);

void dyn_route_index_free(dyn_route_idx_t* idx);

const dyn_route_t* dyn_route_lookup(const dyn_route_t* routes,
    size_t n_routes, const dyn_route_idx_t* idx, const char* path);

const char* dyn_req_header(const char* buf, size_t len,
    const char* name, size_t* vlen);

#define DYN_HTTP_CT_MAX 256

int dyn_route_copy(JSContext* ctx, const char* path, JSValueConst val,
    dyn_route_t* r);

int dyn_http_bind(const char* host, uint16_t* pport, int backlog);
int dyn_http_bind_ex(const char* host, uint16_t* pport, int backlog, int reuseport);
int dyn_http_listen_clone(int lfd, int backlog);
int dyn_http_reuseport_for(int workers);

JSValue dyn_http_server_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_http_server_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_server_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_server_get_port(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_http_async_class_id;

#define DYN_ACONN_MAX_REQ (1 * 1024 * 1024)
#define DYN_WS_MAX_FRAGMENTS 4096
#define DYN_WS_CTL_BUDGET 64
#define DYN_HTTP_OUTBOUND_MAX (4 << 20)
#define DYN_APP_MAX_PARAMS 256

static inline const char* dyn_http_date(const char* extra, size_t n)
{
    const char* d;
    if (extra && n && httpk_block_has(extra, n, "date"))
        return "";
    d = httpk_date_line(dyn_wall_sec());
    return d ? d : "";
}

int dyn_http_format(dyn_bytes_t* out, int status, const char* ct,
    const char* body, size_t body_len, int keep_alive, int head_only);

int dyn_http_pump(dyn_bytes_t* in, dyn_bytes_t* out, int* pnreq,
    size_t* pscan,
    const dyn_route_t* routes, size_t n_routes,
    const dyn_route_idx_t* route_idx, size_t max_req);

JSValue dyn_http_async_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_http_async_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_async_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_http_async_get_port(JSContext* ctx, JSValueConst this_val);

enum { APP_RPC = 1,
    APP_STATIC,
    APP_UPLOAD,
    APP_WS,
    APP_PROXY,
    APP_SSE,
    APP_DYN };

typedef struct {
    JSValue handler;
    char* path;
    char* dir;
    char** allow;
    union {
        char* up_host;
        struct dyn_static_root* sroot;
    };
    char* dyn_method;
    char* dyn_pattern;
    int64_t max_file;
    size_t n_allow;
    int type;
    uint16_t up_port;
} dyn_app_route_t;

_Static_assert(sizeof(dyn_app_route_t) <= 96,
    "dyn_app_route_t regained padding: reorder largest-first");

#define DYN_APP_HDR_TPLS 4

typedef struct {
    JSObjectTemplate* tpl;
    JSAtom* atoms;
    uint32_t n;
} dyn_app_hdr_tpl_t;

typedef struct dyn_app_ridx dyn_app_ridx_t;

typedef struct {
    uint32_t next;
    uint32_t g;
    int lin;
} dyn_app_dyn_it_t;

typedef struct dyn_app {
    JSContext* ctx;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    dyn_app_route_t* routes;
    size_t n_routes;
    dyn_app_ridx_t* ridx;
    int started;
    int n_conns;
    int dispose_called;
    void* conns;
    uint64_t idle_ms;
    int max_conns;
    dyn_timers_t* timers;
    dyn_timers_t* timers_orphan;
    int in_sweep;
    dyn_timer_id sweep;
    int compress;
    int metrics_http;
    JSObjectTemplate* req_tpl;
    dyn_app_hdr_tpl_t hdr_tpl[DYN_APP_HDR_TPLS];
    uint32_t hdr_tpl_next;
    uint32_t hdr_last_miss;
    char* listen_host;
    int backlog;
    int workers;
    size_t out_bytes;
    int n_sse;
    JSValue* mw;
    size_t n_mw, cap_mw;
} dyn_app_t;

void dyn_app_ridx_free(dyn_app_t* app);
const dyn_app_route_t* dyn_app_route_find(dyn_app_t* app, const char* path,
    const dyn_app_route_t** pmount);
void dyn_app_dyn_first(dyn_app_t* app, const char* path, dyn_app_dyn_it_t* it);
long dyn_app_dyn_next(const dyn_app_t* app, dyn_app_dyn_it_t* it);

typedef struct dyn_ws dyn_ws_t;

typedef struct dyn_sse dyn_sse_t;

typedef struct dyn_app_upload dyn_app_upload_t;

typedef struct dyn_app_stream dyn_app_stream_t;

typedef struct {
    dyn_app_t* app;
    int fd;
    dyn_iobuf_t in;
    dyn_app_upload_t* up;
    int refs;
    int closed;
    int close_code;
    int is_ws;
    JSValue ws_handlers;
    JSValue ws_this;
    dyn_ws_t* ws_native;
    dyn_iobuf_t ws_frag;
    int ws_frag_op;
    int ws_frag_frames;
    int ws_ctl_budget;
    int is_sse;
    JSValue sse_handlers;
    JSValue sse_this;
    dyn_sse_t* sse_native;
    int accept_gzip;
    int identity_refused;
    int close_after;
    int head_only;
    int resp_parked;
    int in_process;
    uint64_t req_start_ms;
    int http11;
    dyn_app_stream_t* st;
    size_t hdr_scan_from;
    size_t hdr_counted;
    uint32_t hdr_lines;
    uint64_t last_ms;
    size_t out_q;
    size_t stall_q;
    size_t idle_q;
    int out_stalled;
    int file_inflight;
    int linger;
    uint64_t stall_ms;
    uint64_t up_win_ms;
    int64_t up_win_bytes;
    int64_t linger_left;
    uint64_t linger_until;
    void *lprev, *lnext;
    void* proxy;
} dyn_app_conn_t;

struct dyn_app_upload {
    int fd;
    int64_t remaining;
    int64_t size;
    char* path;
    char ctype[128];
    JSValue handler;
    int64_t off;
    int wbusy;
    int err;
};

void dyn_app_conn_unlink(dyn_app_conn_t* c);

void dyn_app_conn_unref(dyn_app_conn_t* c);

void dyn_app_conn_close(dyn_app_conn_t* c);

void dyn_app_shutdown_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

void dyn_app_send_err(dyn_app_conn_t* c, int status, const char* msg);

void dyn_app_pump_resume(dyn_app_conn_t* c);

void dyn_app_out_account(dyn_app_conn_t* c);

int dyn_app_out_blocked(dyn_app_conn_t* c);
static inline const char* dyn_app_conn_token(const dyn_app_conn_t* c)
{
    return (c->close_after || c->linger) ? "close" : "keep-alive";
}

void dyn_app_out_unstall(dyn_app_conn_t* c);

void dyn_app_out_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

void dyn_app_out_file_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);
void dyn_app_out_last(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

void dyn_app_met_response(dyn_app_conn_t* c, double bytes);

JSValue dyn_app_own_get(JSContext* ctx, JSValueConst obj, const char* key);

struct dyn_ws {
    dyn_app_conn_t* conn;
    int64_t after_close;
};

extern JSClassID dyn_ws_class_id;

void dws_accept(const char* key, size_t klen, char out[32]);

void dyn_ws_finalizer(JSRuntime* rt, JSValue val);

uint8_t* dyn_ws_bytes_view(JSContext* ctx, JSValueConst v, size_t* plen,
    int* pbinary);

JSValue dyn_ws_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_ws_get_buffered(JSContext* ctx, JSValueConst this_val);
JSValue dyn_ws_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dyn_app_ws_handshake(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len);

void dyn_app_ws_process(dyn_app_conn_t* c);

struct dyn_sse {
    dyn_app_conn_t* conn;
    int64_t after_close;
};

extern JSClassID dyn_sse_class_id;

void dyn_sse_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_sse_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_sse_get_buffered(JSContext* ctx, JSValueConst this_val);
JSValue dyn_sse_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void dyn_app_sse_handshake(dyn_app_conn_t* c,
    const dyn_app_route_t* rt);

extern JSClassID dyn_app_class_id;

void dyn_app_send_built(dyn_app_conn_t* c, int status,
    const char* ctype, const char* body,
    size_t body_len, const char* extra, uint8_t* gz, size_t gz_len);

int dyn_app_gz_wanted(const dyn_app_conn_t* c, int status, const char* ctype,
    size_t body_len);

int dyn_app_gz_offload(dyn_app_conn_t* c, int status, const char* ctype,
    const char* body, size_t body_len, const char* extra);

void dyn_app_send_body(dyn_app_conn_t* c, int status, const char* ctype,
    const char* body, size_t body_len);

void dyn_app_send_json(dyn_app_conn_t* c, int status, const char* body,
    size_t body_len);

void dyn_json_escape_n_into(char* buf, size_t limit, size_t* pos,
    const char* s, size_t n);

size_t dyn_rpc_err_compose(char* buf, size_t cap, int code,
    const char* msg, const char* id_json);

void dyn_app_rpc_error(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, const char* id_json);

void dyn_app_rpc_error_n(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, size_t msg_len,
    const char* id_json);

struct dyn_app_stream {
    JSValue src;
    JSValue readfn;
    JSValue chunk;
    JSValue pending;
    uint8_t* frame;
    size_t frame_cap;
    uint64_t total;
    int status;
    char* ct;
    int done;
    int busy;
    int in_send;
    int depth;
    int retiring;
};

#define DYN_APP_STREAM_CHUNK (64 * 1024)

void dyn_app_stream_resume(dyn_app_conn_t* c);

void dyn_app_stream_sent(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len, void* udata);

int dyn_value_is_byte_source(JSContext* ctx, JSValueConst v);

void dyn_app_stream_enter(dyn_app_stream_t* st);

void dyn_app_stream_retire(dyn_app_conn_t* c);

void dyn_app_stream_leave(dyn_app_conn_t* c);

void dyn_app_stream_abort(dyn_app_conn_t* c);

int dyn_app_read_attach(JSContext* ctx, JSValueConst pr,
    JSValueConst onres, JSValueConst onrej);

void dyn_app_read_abandon(JSContext* ctx, JSValueConst pr);

const char* dyn_app_env_headers(JSContext* ctx, JSValueConst env,
    const char* const* keys, int nkeys, dyn_iobuf_t* out);

int dyn_app_try_stream_result(dyn_app_conn_t* c, JSValueConst result);

void dyn_app_send_rpc_result(dyn_app_conn_t* c, JSValueConst result,
    const char* id_s);

void dyn_app_send_rpc_throw(dyn_app_conn_t* c, JSValueConst exc,
    const char* id_s);

typedef struct {
    dyn_app_conn_t* conn;
    char* id;
    int accept_gzip;
    int identity_refused;
    int counted;
} dyn_app_pend_t;

extern JSClassID dyn_pend_class_id;

void dyn_app_pend_release(dyn_app_pend_t* pd, int abandoned);

dyn_app_pend_t* dyn_app_pend_claim(JSValueConst obj);

JSValue dyn_app_rpc_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data);

void dyn_app_build_error(int code, const char* msg, const char* id_s,
    dyn_iobuf_t* o);

void dyn_app_build_result(JSContext* ctx, JSValueConst result,
    const char* id_s, dyn_iobuf_t* o);

int dyn_app_rpc_build_one(dyn_app_conn_t* c, JSValueConst methods,
    JSValueConst elem, dyn_iobuf_t* out, int comma);

const char* dyn_app_content_type(const char* path);

void dyn_app_reject_linger(dyn_app_conn_t* c, int status, const char* msg,
    int64_t clen);

void dyn_app_send_err_allow(dyn_app_conn_t* c, int status,
    const char* msg, const char* allow);

void dyn_app_serve_static(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* reqpath,
    const char* base, size_t head_len);

void dyn_app_upload_drain(dyn_app_conn_t* c);

typedef struct {
    dyn_app_conn_t* c;
    int fd;
    uint8_t* req;
    size_t req_len;
    dyn_iobuf_t resp;
    int sent;
} dyn_app_proxy_t;

void dyn_proxy_fail(dyn_app_proxy_t* p, int status, const char* msg);

void dyn_app_proxy_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len,
    const char* path, int64_t clen);

void dyn_app_upload_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len, int64_t clen);

size_t dyn_req_header_count(const char* buf, size_t len);

void dyn_app_process(dyn_app_conn_t* c);

JSValue dyn_app_rpc(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

char* dyn_pct_decode(const char* s, size_t n, int plus_space, size_t* outn);
static inline int dyn_pct_plain(const char* s, size_t n, int plus_space)
{
    return !memchr(s, '%', n) && !(plus_space && memchr(s, '+', n));
}

void dyn_app_req_method(const char* base, char* out, size_t cap);

void dyn_app_req_target(const char* base, size_t head_len,
    const char** tgt, size_t* tlen);

JSValue dyn_app_build_query(JSContext* ctx, const char* qs, size_t qn);

JSValue dyn_app_build_headers(JSContext* ctx, dyn_app_t* app, const char* base, size_t head_len);
void dyn_app_hdr_templates_free(JSContext* ctx, dyn_app_t* app);
void dyn_app_hdr_templates_mark(JSRuntime* rt, dyn_app_t* app, JS_MarkFunc* mark_func);

void dyn_app_dyn_err_json_n(dyn_app_conn_t* c, int status,
    const char* msg, size_t msg_len);

void dyn_app_dyn_send_or_await(dyn_app_conn_t* c, JSValue res);

int dyn_app_parse_allow(JSContext* ctx, JSValueConst opts, char*** pout, size_t* pn);

JSValue dyn_app_proxy(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_app_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_app_upload(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_app_sse_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_app_ws_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_app_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

extern JSClassID dyn_wsc_class_id;

JSValue dyn_wsc_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_wsc_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);
#endif

#endif
