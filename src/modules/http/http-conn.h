#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include "dyna-nat.h"
#include "core/dyn-httpk.h"
#include <stdint.h>
#include <sys/types.h>
#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define HC_HEAD_MAX (32 * 1024)
#define HC_HEAD_FIELDS 128
#define HC_MAX_INTERIM 8

typedef struct {
    int fd;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
#endif
    uint64_t deadline_ms;
    int tls_cut;
} hc_conn_t;

typedef struct {
    char* raw;
    httpk_head_t h;
    httpk_hdr_t f[HC_HEAD_FIELDS];
    uint8_t* extra;
    size_t extra_len;
} hc_head_t;

typedef struct {
    hc_conn_t conn;
    int chunked;
    uint64_t clen;
    uint64_t served;
    uint64_t max_body;
    int eof;
    int failed;
    httpk_dechunk_t dc;
    uint8_t* rbuf;
    size_t rn, rcap, rpos;
    char* err;
} hc_stream_t;

int hc_conn_open(hc_conn_t* c, const char* host, uint16_t port,
    int64_t timeout_ms, int (*reject)(int fd, void* ud), void* ud,
    int* reason, char* why, size_t whyn);
void hc_conn_close(hc_conn_t* c);
ssize_t hc_send(hc_conn_t* c, const void* p, size_t n);
ssize_t hc_recv(hc_conn_t* c, void* p, size_t n);
#ifdef CONFIG_TLS
int hc_conn_tls_open(hc_conn_t* conn, dyn_tls_ctx_t* tls_ctx,
    const char* host, char* why, size_t why_n);
dyn_tls_ctx_t* hc_tls_client_ctx(const char* ca, char* why, size_t whyn);
int hc_client_adopt_ca(JSValueConst client, const char* ca, char* why, size_t whyn);
#endif

typedef struct dyn_cookiejar dyn_cookiejar_t;
dyn_cookiejar_t* dyn_cookiejar_ref(dyn_cookiejar_t* j);
void dyn_cookiejar_unref(dyn_cookiejar_t* j);
dyn_cookiejar_t* dyn_cookiejar_of(JSContext* ctx, JSValueConst v,
    const char* who);
int dyn_cookiejar_header(dyn_cookiejar_t* j, const char* url, dyn_sb_t* out);
int dyn_cookiejar_absorb(dyn_cookiejar_t* j, const char* url,
    const char* raw, const httpk_head_t* h);
int dyn_cookiejar_absorb_raw(dyn_cookiejar_t* j, const char* url,
    const char* raw, size_t head_len);
char* dyn_cookiejar_with_header(dyn_cookiejar_t* j, const char* url,
    char* hdr, int* oom);
int hc_client_adopt_jar(JSValueConst client, dyn_cookiejar_t* j);

int hc_read_head(hc_conn_t* conn, hc_head_t* hd, char* why, size_t whyn);
void hc_head_free(hc_head_t* hd);
const char* hc_head_get(const hc_head_t* hd, const char* name, size_t* vlen);
JSValue hc_head_headers(JSContext* ctx, const hc_head_t* hd);

void hc_stream_init(hc_stream_t* s, hc_conn_t* conn, hc_head_t* hd,
    uint64_t max_body);
long long hc_stream_body(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn);
long long hc_stream_some(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn);
void hc_stream_dispose(void* native);

enum {
    HC_CALL_E_URL = -1,
    HC_CALL_E_RESOLVE = -2,
    HC_CALL_E_CONNECT = -3,
    HC_CALL_E_TLS = -4,
    HC_CALL_E_REQUEST = -5,
    HC_CALL_E_SEND = -6,
    HC_CALL_E_RECV = -7,
    HC_CALL_E_PARSE = -8,
    HC_CALL_E_OOM = -9
};

typedef struct {
    const char* method;
    const char* url;
    const char* headers;
    const uint8_t* body;
    size_t body_len;
    int64_t timeout_ms;
    uint64_t max_body;
    int (*on_fd)(void* ud, int fd);
    void* fd_ud;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls;
#endif
} hc_call_req_t;

int hc_call_open(const hc_call_req_t* rq, hc_head_t* hd, hc_stream_t** out,
    char* why, size_t whyn);
int hc_call_read_all(hc_stream_t* s, dyn_sb_t* out, char* why, size_t whyn);
const char* hc_call_strerror(int code);

JSValue hc_promise_resolved(JSContext* ctx, JSValue val);
JSValue hc_promise_rejected(JSContext* ctx, JSValue exc);
uint8_t* hc_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen,
    const char* who);
#endif

#endif
