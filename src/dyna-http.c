#include "dyna-nat.h"
#include "cutils.h"
#include "dyna-tls.h"
#include "dyna-evloop.h"
#include "dyna-aio.h"
#include "core/dyn-pool.h"
#include "core/dyn-timer.h"

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata);

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

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
#include <limits.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

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
#define DYN_HTTP_ERR_TOOBIG 8
#define DYN_HTTP_ERR_TLS 9
#define DYN_HTTP_ERR_CANCEL 10
#define DYN_HTTP_ERR_TRUNC 11

#define DYN_HTTP_DEFAULT_TIMEOUT_MS 15000
#define DYN_HTTP_DEFAULT_MAX_BODY (16 * 1024 * 1024)
#define DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT 30000
#define DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT 5000
#define DYN_HTTP_MAX_RESP_HEADER (64 * 1024)
#define DYN_HTTP_SERVER_FRAME_MAX (16 * 1024 - 1)
#define DYN_HTTP_CONN_QUEUE_CAP 256

#define DYN_APP_MAX_HEADER (64 * 1024)
#define DYN_APP_MAX_HEADER_COUNT 256
#define DYN_APP_OUT_HIGH (1024 * 1024)
#define DYN_APP_OUT_LOW (256 * 1024)
#define DYN_APP_OUT_TOTAL (64 * 1024 * 1024)
#define DYN_APP_OUT_STALL_MS 10000
#define DYN_APP_SSE_IDLE_MULT 10
#define DYN_APP_SSE_MAX 1024
#define DYN_APP_UP_IN_MAX (4 * 1024 * 1024)
#define DYN_APP_UP_RATE_BYTES (16 * 1024)
#define DYN_APP_UP_RATE_MS 1000

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

static const char* const http_srv_keys[] = { "port", "workers", "backlog", "requestTimeoutMs", "host", "routes" };
static const char* const http_async_keys[] = { "port", "backlog", "host", "idleTimeoutMs", "maxConns", "routes" };
static const char* const http_app_keys[] = { "port", "idleTimeoutMs", "maxConns", "host", "compress", "metrics",
    "workers", "backlog" };
static const char* const http_route_val_keys[] = { "status", "contentType", "body" };
static const char* const http_static_keys[] = { "maxFileSize", "allow" };
static const char* const http_upload_keys[] = { "dir", "maxFileSize", "allow" };
static const char* const http_proxy_keys[] = { "host", "port" };
static const char* const http_ws_handler_keys[] = { "open", "message", "close", "upgrade" };
static const char* const http_sse_handler_keys[] = { "open", "close" };
static const char* const http_cookie_keys[] = { "maxAge", "domain", "path", "sameSite", "secure", "httpOnly" };

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} dyn_bytes_t;

static int dyn_bytes_reserve(dyn_bytes_t* b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;
    char* nd;

    if (need < b->len)
        return -1;
    if (need <= b->cap)
        return 0;
    nc = b->cap ? b->cap * 2 : 4096;
    while (nc < need) {
        size_t doubled = nc * 2;
        if (doubled < nc)
            return -1;
        nc = doubled;
    }
    nd = (char*)realloc(b->data, nc);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int dyn_bytes_append(dyn_bytes_t* b, const char* src, size_t n)
{
    if (dyn_bytes_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int dyn_req_headers_valid(const char* buf, size_t len);

static const char* dyn_memfind(const char* hay, size_t hlen,
    const char* needle, size_t nlen)
{
    size_t idx;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    idx = simd.strfind((const uint8_t*)hay, hlen,
        (const uint8_t*)needle, nlen);
    return idx == SIZE_MAX ? NULL : hay + idx;
}

static int dyn_ci_equal(const char* a, size_t alen, const char* b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        int ca = a[i], cb = b[i];
        if (cb == '\0')
            return 0;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
    }
    return b[i] == '\0';
}

static void dyn_set_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

#include "dyna-httpmsg.inc.c"

static JSClassID dyn_http_client_class_id;

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
} dyn_http_client_t;

static void dyn_http_client_unpin(dyn_http_client_t* cl)
{
    JSContext* ctx = cl->ctx;
    if (--cl->n_async > 0)
        return;
    {
        JSValue v = cl->self_pending;
        cl->self_pending = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static void hc_jobs_orphan(dyn_http_client_t* cl);

static void dyn_http_client_dispose(void* native)
{
    dyn_http_client_t* c = (dyn_http_client_t*)native;
    if (c) {
        JSValue v = c->self_pending;
        c->self_pending = JS_UNDEFINED;
        hc_jobs_orphan(c);
        JS_FreeValue(c->ctx, v);
#ifdef CONFIG_TLS
        dyn_tls_ctx_free(c->tls);
#endif
    }
    free(native);
}

typedef struct {
    int fd;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
#endif
    uint64_t deadline_ms;
    int tls_cut;
} hc_conn_t;

#define DYN_HTTP_TOTAL_TIMEOUT_FACTOR 20

static ssize_t hc_raw_send(int fd, const void* p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t s = send(fd, (const char*)p + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)n;
}

#ifdef CONFIG_TLS
static int hc_tls_flush(hc_conn_t* c)
{
    uint8_t out[16384];
    int n;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0)
        if (hc_raw_send(c->fd, out, (size_t)n) < 0)
            return -1;
    return 0;
}

static int hc_tls_handshake(hc_conn_t* c)
{
    uint8_t buf[16384];
    for (;;) {
        int st = dyn_tls_handshake(c->tls);
        ssize_t r;
        if (hc_tls_flush(c) < 0)
            return -1;
        if (st < 0)
            return -1;
        if (st == 1)
            return 0;
        r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0)
            return -1;
        if (dyn_tls_feed(c->tls, buf, (size_t)r) != 0)
            return -1;
    }
}
#endif

static ssize_t hc_send(hc_conn_t* c, const void* p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        size_t off = 0;
        while (off < n) {
            int w = dyn_tls_write(c->tls, (const uint8_t*)p + off, n - off);
            if (w <= 0)
                return -1;
            off += (size_t)w;
            if (hc_tls_flush(c) != 0)
                return -1;
        }
        return (ssize_t)n;
    }
#endif
    return hc_raw_send(c->fd, p, n);
}

static ssize_t hc_recv(hc_conn_t* c, void* p, size_t n)
{
    if (c->deadline_ms && dyn_timer_now_ms() >= c->deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
#ifdef CONFIG_TLS
    if (c->tls) {
        for (;;) {
            uint8_t cipher[16384];
            ssize_t r;
            int got = dyn_tls_read(c->tls, (uint8_t*)p, n);
            if (got > 0)
                return got;
            if (got < 0)
                return dyn_tls_peer_closed(c->tls) ? 0 : -1;
            r = recv(c->fd, cipher, sizeof cipher, 0);
            if (r == 0)
                c->tls_cut = 1;
            if (r <= 0)
                return r;
            if (dyn_tls_feed(c->tls, cipher, (size_t)r) != 0)
                return -1;
        }
    }
#endif
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static const JSClassDef dyn_http_client_class = {
    "HTTPClient",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_http_client_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    int64_t max_body = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt64(ctx, &max_body, argv[0]))
            return JS_EXCEPTION;
        if (max_body < 0)
            max_body = 0;
    }
    cl = (dyn_http_client_t*)calloc(1, sizeof(*cl));
    if (!cl)
        return JS_ThrowOutOfMemory(ctx);
    cl->timeout_ms = DYN_HTTP_DEFAULT_TIMEOUT_MS;
    cl->max_body = max_body ? (size_t)max_body : DYN_HTTP_DEFAULT_MAX_BODY;
    cl->self_pending = JS_UNDEFINED;
    cl->ctx = ctx;
    return dyn_res_wrap(ctx, new_target, dyn_http_client_class_id, cl,
        dyn_http_client_dispose);
}

static int dyn_method_valid(const char* m, size_t n)
{
    size_t i;
    if (!m || n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (!DYN_TCHAR[(unsigned char)m[i]])
            return 0;
    return 1;
}

static void dyn_call_drop(JSContext* ctx, JSValueConst fn,
    JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r = JS_Call(ctx, fn, this_val, argc, argv);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

#define DYN_URL_MAX_PATH 2048

typedef struct {
    const char* path;
    size_t path_len;
    uint16_t port;
    uint8_t tls;
    char host[256];
} dyn_url_t;

_Static_assert(sizeof(dyn_url_t) <= 288,
    "dyn_url_t is a request-stack local: keep it near one page");

static int dyn_parse_url(const char* url, dyn_url_t* out)
{
    const char* p = url;
    const char *host_start, *host_end;
    size_t host_len, path_len;

    out->tls = 0;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
#ifdef CONFIG_TLS
    else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        out->tls = 1;
    }
#else
    else if (strncmp(p, "https://", 8) == 0)
        return -1;
#endif
    else
        return -1;

    host_start = p;
    if (*p == '[') {
        const char* close = strchr(p, ']');
        if (!close)
            return -1;
        p = close + 1;
    } else {
        while (*p && *p != ':' && *p != '/')
            p++;
    }
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return -1;
    {
        const char* h;
        for (h = host_start; h < host_end; h++)
            if ((unsigned char)*h < 0x21 || (unsigned char)*h > 0x7e)
                return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    out->port = out->tls ? 443 : 80;
    if (*p == ':') {
        long port = 0;
        p++;
        if (*p < '0' || *p > '9')
            return -1;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            if (port > 65535)
                return -1;
            p++;
        }
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        out->path = "/";
        out->path_len = 1;
        return 0;
    }
    if (*p != '/')
        return -1;
    path_len = strlen(p);
    if (path_len >= DYN_URL_MAX_PATH)
        return -1;
    {
        const char* q;
        for (q = p; q < p + path_len; q++)
            if ((unsigned char)*q < 0x21 || (unsigned char)*q > 0x7e)
                return -1;
    }
    {
        const char* frag = (const char*)memchr(p, '#', path_len);
        if (frag)
            path_len = (size_t)(frag - p);
    }
    if (path_len == 0) {
        out->path = "/";
        out->path_len = 1;
        return 0;
    }
    out->path = p;
    out->path_len = path_len;
    return 0;
}

static const char* dyn_host_bare(const char* host, char* buf, size_t cap)
{
    const char* close_;
    size_t n;
    if (host[0] != '[')
        return host;
    close_ = strchr(host, ']');
    if (!close_)
        return host;
    n = (size_t)(close_ - host) - 1;
    if (n == 0 || n >= cap)
        return host;
    memcpy(buf, host + 1, n);
    buf[n] = '\0';
    return buf;
}

static int dyn_sock_cloexec(int domain, int type, int protocol)
{
    int fd;
#ifdef SOCK_CLOEXEC
    fd = socket(domain, type | SOCK_CLOEXEC, protocol);
    if (fd < 0 && (errno == EINVAL || errno == EPROTOTYPE))
        fd = socket(domain, type, protocol);
#else
    fd = socket(domain, type, protocol);
#endif
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int dyn_accept_cloexec(int lfd)
{
    int fd;
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd >= 0)
        return fd;
    if (errno != ENOSYS && errno != EINVAL && errno != EPROTOTYPE)
        return -1;
#endif
    fd = accept(lfd, NULL, NULL);
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int dyn_tcp_connect(const char* host, uint16_t port, int64_t timeout_ms,
    int* perr)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        *perr = DYN_HTTP_ERR_RESOLVE;
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        int flags, rc;
        fd = dyn_sock_cloexec(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
#ifdef SO_NOSIGPIPE
        {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        }
#endif
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            dyn_set_nodelay(fd);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            int pr;
            int pt = timeout_ms > INT_MAX ? INT_MAX
                : timeout_ms < -1         ? -1
                                          : (int)timeout_ms;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            pr = poll(&pfd, 1, pt);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                    fcntl(fd, F_SETFL, flags);
                    dyn_set_nodelay(fd);
                    break;
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        *perr = DYN_HTTP_ERR_CONNECT;
    return fd;
}

static int dyn_ci_eq(const char* a, const char* b, size_t n);
static int dyn_hdr_is_token(const char* n, size_t len)
{
    size_t i;
    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL)))
            return 0;
    }
    return 1;
}

static int dyn_hdr_is_client_framing_name(const char* n, size_t len)
{
    static const char* const names[] = {
        "host",
        "content-length",
        "transfer-encoding",
        "connection",
        "keep-alive",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        if (strlen(names[i]) == len && dyn_ci_eq(n, names[i], len))
            return 1;
    return 0;
}

static char* dyn_headers_to_string(JSContext* ctx, JSValueConst headers,
    int* perr)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    dyn_bytes_t buf = { 0 };

    *perr = 0;
    if (JS_IsUndefined(headers) || JS_IsNull(headers))
        return NULL;

    if (JS_IsString(headers)) {
        const char* s = JS_ToCString(ctx, headers);
        dyn_bytes_t b = { 0 };
        size_t hi, n;
        if (!s) {
            *perr = 1;
            return NULL;
        }
        n = strlen(s);
        {
            size_t vlen = n;
            if (vlen >= 2 && s[vlen - 2] == '\r' && s[vlen - 1] == '\n')
                vlen -= 2;
            if (memchr(s, '\r', vlen) || memchr(s, '\n', vlen)) {
                JS_FreeCString(ctx, s);
                *perr = 1;
                JS_ThrowTypeError(ctx,
                    "header name/value must not contain CR or LF");
                return NULL;
            }
        }
        hi = 0;
        while (hi < n) {
            size_t ls = hi, le;
            const char* colon;
            size_t nlen;
            while (hi + 1 < n && !(s[hi] == '\r' && s[hi + 1] == '\n'))
                hi++;
            if (hi + 1 < n) {
                le = hi;
                hi += 2;
            } else {
                le = n;
                hi = n;
            }
            if (le > ls) {
                colon = memchr(s + ls, ':', le - ls);
                nlen = colon ? (size_t)(colon - (s + ls)) : 0;
                if (!colon || !dyn_hdr_is_token(s + ls, nlen)) {
                    JS_FreeCString(ctx, s);
                    free(b.data);
                    *perr = 1;
                    JS_ThrowTypeError(ctx,
                        "a header string must be \"Name: value\" with an HTTP token name");
                    return NULL;
                }
                if (!dyn_hdr_is_client_framing_name(s + ls, nlen)) {
                    if (dyn_bytes_append(&b, s + ls, le - ls) < 0 || dyn_bytes_append(&b, "\r\n", 2) < 0)
                        goto strform_oom;
                }
            }
        }
        if (dyn_bytes_append(&b, "\0", 1) < 0)
            goto strform_oom;
        JS_FreeCString(ctx, s);
        return b.data;
    strform_oom:
        JS_FreeCString(ctx, s);
        free(b.data);
        *perr = 1;
        return NULL;
    }

    if (!JS_IsObject(headers))
        return NULL;
    if (JS_IsArray(ctx, headers)) {
        *perr = 1;
        JS_ThrowTypeError(ctx,
            "headers must be an object of name/value pairs or a "
            "pre-formatted string, not an array");
        return NULL;
    }

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        const char* name = JS_AtomToCString(ctx, tab[i].atom);
        const char* val;
        JSValue v;
        size_t nl, vl;

        if (!name)
            goto fail;
        v = JS_GetProperty(ctx, headers, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        val = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!val) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        nl = strlen(name);
        vl = strlen(val);
        if (memchr(name, '\r', nl) || memchr(name, '\n', nl) || memchr(val, '\r', vl) || memchr(val, '\n', vl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                "header name/value must not contain CR or LF");
            return NULL;
        }
        if (!dyn_hdr_is_token(name, nl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                "header name must be a valid HTTP token "
                "(no spaces, controls or separators)");
            return NULL;
        }
        if (dyn_hdr_is_client_framing_name(name, nl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            continue;
        }
        if (dyn_bytes_append(&buf, name, nl) < 0 || dyn_bytes_append(&buf, ": ", 2) < 0 || dyn_bytes_append(&buf, val, vl) < 0 || dyn_bytes_append(&buf, "\r\n", 2) < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            goto fail;
        }
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (buf.data) {
        if (dyn_bytes_append(&buf, "\0", 1) < 0) {
            free(buf.data);
            return NULL;
        }
    }
    return buf.data;

fail:
    free(buf.data);
    JS_FreePropertyEnum(ctx, tab, len);
    *perr = 1;
    return NULL;
}

static const char* hc_err_name(int code)
{
    static const char* const names[] = {
        "ok", "bad URL", "DNS resolution failed", "connection failed",
        "send failed", "receive failed", "malformed response",
        "out of memory", "response too large",
        "TLS handshake failed", "aborted", "truncated body"
    };
    return (code >= 0 && code < (int)countof(names)) ? names[code] : "error";
}

static void hc_redact_put(char* out, size_t cap, size_t* n, const char* s)
{
    size_t left, l;

    if (*n >= cap)
        return;
    left = cap - 1 - *n;
    l = strlen(s);
    if (l > left) {
        memcpy(out + *n, s, left);
        *n = cap - 1;
        out[*n] = 0;
        return;
    }
    memcpy(out + *n, s, l);
    *n += l;
    out[*n] = 0;
}

static void hc_url_redact(const char* url, char* out, size_t cap)
{
    const char* p = url ? url : "";
    const char* scheme = strstr(p, "://");
    size_t n = 0;

    if (cap == 0)
        return;
    out[0] = 0;
    if (scheme) {
        const char* auth = scheme + 3;
        const char* q = auth;
        const char* at;
        size_t pre;
        while (*q && *q != '/' && *q != '?' && *q != '#')
            q++;
        at = (const char*)memchr(auth, '@', (size_t)(q - auth));
        pre = (size_t)(auth - p);
        if (pre > cap - 1)
            pre = cap - 1;
        memcpy(out, p, pre);
        out[pre] = 0;
        n = pre;
        if (at)
            hc_redact_put(out, cap, &n, "***@");
        hc_redact_put(out, cap, &n, at ? at + 1 : auth);
    } else {
        hc_redact_put(out, cap, &n, p);
    }
    {
        char* qm = strchr(out, '?');
        if (qm)
            snprintf(qm, cap - (size_t)(qm - out), "?[redacted]");
    }
}

static JSValue hc_error_value_named(JSContext* ctx, int code, const char* method,
    const char* url, const char* what)
{
    JSValue e;
    char msg[640];
    char rurl[256];

    hc_url_redact(url, rurl, sizeof rurl);
    snprintf(msg, sizeof(msg), "HTTP %s %s failed: %s", method,
        rurl, what);
    e = JS_NewError(ctx);
    if (JS_IsException(e))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "dynajsError", JS_NewInt32(ctx, code),
        JS_PROP_C_W_E);
    return e;
}

static JSValue hc_error_value(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy)
{
    const char* what = hc_err_name(code);

    if (code == DYN_HTTP_ERR_TLS && tlswhy && *tlswhy)
        what = tlswhy;
    return hc_error_value_named(ctx, code, method, url, what);
}

static JSValue dyn_http_throw(JSContext* ctx, int code, const char* method,
    const char* url, const char* tlswhy)
{
    JSValue e = hc_error_value(ctx, code, method, url, tlswhy);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

static JSValue dyn_http_throw_named(JSContext* ctx, int code,
    const char* method, const char* url,
    const char* what)
{
    JSValue e = hc_error_value_named(ctx, code, method, url, what);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

static JSValue dyn_headers_object(JSContext* ctx, const char* p, const char* end)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    while (p < end) {
        const char* eol = dyn_memfind(p, (size_t)(end - p), "\r\n", 2);
        const char* line_end = eol ? eol : end;
        const char* colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char* val = colon + 1;
            size_t val_len = (size_t)(line_end - val);
            JSAtom key;
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }
            if (name_len > 0) {
                key = JS_NewAtomLen(ctx, p, name_len);
                if (key != JS_ATOM_NULL) {
                    JSPropertyDescriptor prev;
                    JSValue v = JS_UNDEFINED;
                    int have = JS_GetOwnProperty(ctx, &prev, obj, key);
                    if (have > 0) {
                        size_t ol = 0;
                        const char* old = JS_IsString(prev.value)
                            ? JS_ToCStringLen(ctx, &ol, prev.value)
                            : NULL;
                        char* both = old ? (char*)malloc(ol + 2 + val_len + 1) : NULL;
                        if (both) {
                            memcpy(both, old, ol);
                            memcpy(both + ol, ", ", 2);
                            memcpy(both + ol + 2, val, val_len);
                            v = JS_NewStringLen(ctx, both, ol + 2 + val_len);
                            free(both);
                        }
                        if (old)
                            JS_FreeCString(ctx, old);
                        JS_FreeValue(ctx, prev.value);
                        JS_FreeValue(ctx, prev.getter);
                        JS_FreeValue(ctx, prev.setter);
                    }
                    if (JS_IsUndefined(v))
                        v = JS_NewStringLen(ctx, val, val_len);
                    if (JS_IsException(v))
                        JS_FreeValue(ctx, JS_GetException(ctx));
                    else
                        JS_DefinePropertyValue(ctx, obj, key, v, JS_PROP_C_W_E);
                    JS_FreeAtom(ctx, key);
                }
            }
        }
        if (!eol)
            break;
        p = eol + 2;
    }
    return obj;
}

enum { DECHUNK_OK = 0,
    DECHUNK_TRUNCATED = 1,
    DECHUNK_MALFORMED = 2,
    DECHUNK_OOM = 3 };

static int dyn_dechunk(const char* p, size_t len, char** out, size_t* out_len)
{
    dyn_bytes_t o = { 0 };
    size_t i = 0;

    *out = NULL;
    *out_len = 0;
    for (;;) {
        size_t chunk = 0, j = i;
        int digits = 0;
        int sig = 0;
        if (i >= len)
            goto truncated;
        while (j < len && p[j] != '\r' && p[j] != ';') {
            int c = p[j];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            if (sig || d)
                sig++;
            chunk = chunk * 16 + (size_t)d;
            digits++;
            j++;
        }
        if (!digits)
            goto malformed;
        if (sig > 13)
            goto malformed;
        if (j >= len)
            goto truncated;
        if (p[j] != '\r' && p[j] != ';')
            goto malformed;
        while (j < len && p[j] != '\n')
            j++;
        if (j >= len)
            goto truncated;
        i = j + 1;
        if (chunk == 0) {
            for (;;) {
                const char* eol = dyn_memfind(p + i, len - i, "\r\n", 2);
                if (!eol)
                    goto truncated;
                if (eol == p + i) {
                    *out = o.data;
                    *out_len = o.len;
                    return DECHUNK_OK;
                }
                i = (size_t)(eol - p) + 2;
            }
        }
        if (chunk > len - i)
            goto truncated;
        if (i + chunk + 2 > len)
            goto truncated;
        if (p[i + chunk] != '\r' || p[i + chunk + 1] != '\n')
            goto malformed;
        if (dyn_bytes_append(&o, p + i, chunk) < 0)
            goto oom;
        i += chunk + 2;
    }
truncated:
    free(o.data);
    return DECHUNK_TRUNCATED;
malformed:
    free(o.data);
    return DECHUNK_MALFORMED;
oom:
    free(o.data);
    return DECHUNK_OOM;
}

#define HC_CANCELLED(cancel) \
    ((cancel) && atomic_load_explicit((cancel), memory_order_relaxed))

static int hc_te_chunked(const char* v, size_t vl)
{
    size_t i = 0;
    while (i < vl) {
        size_t j;
        while (i < vl && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
            i++;
        j = i;
        while (j < vl && v[j] != ',')
            j++;
        while (j > i && (v[j - 1] == ' ' || v[j - 1] == '\t'))
            j--;
        if (j - i == 7 && strncasecmp(v + i, "chunked", 7) == 0)
            return 1;
        i = j;
    }
    return 0;
}

static int dyn_read_response(hc_conn_t* conn, const char* method,
    size_t max_body, dyn_bytes_t* resp,
    size_t* phdr_end, int* pchunked,
    const _Atomic int* cancel,
    char* truncwhy, size_t truncwhy_n)
{
    const char* hdr_marker;
    size_t hdr_end = 0;
    size_t scanned = 0;
    unsigned interims = 0;

    for (;;) {
        ssize_t r;
        if (HC_CANCELLED(cancel))
            return DYN_HTTP_ERR_CANCEL;
        if (dyn_bytes_reserve(resp, 8192) < 0)
            return DYN_HTTP_ERR_OOM;
        hdr_marker = dyn_memfind(resp->data + scanned, resp->len - scanned,
            "\r\n\r\n", 4);
        if (!hdr_marker) {
            scanned = resp->len > 3 ? resp->len - 3 : 0;
            r = hc_recv(conn, resp->data + resp->len, resp->cap - resp->len);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                return DYN_HTTP_ERR_RECV;
            }
            if (r == 0)
                return DYN_HTTP_ERR_PARSE;
            resp->len += (size_t)r;
            if (resp->len > max_body)
                return DYN_HTTP_ERR_TOOBIG;
            if (resp->len > DYN_HTTP_MAX_RESP_HEADER)
                return DYN_HTTP_ERR_TOOBIG;
            continue;
        }
        {
            hdr_end = (size_t)(hdr_marker - resp->data) + 4;
            const char* sp = memchr(resp->data, ' ',
                hdr_end < 32 ? hdr_end : 32);
            if (sp && resp->len >= hdr_end && memcmp(resp->data, "HTTP/", 5) == 0 && (size_t)(sp - resp->data) <= 12 && sp + 5 <= resp->data + hdr_end && sp[1] == '1' && sp[2] >= '0' && sp[2] <= '9' && sp[3] >= '0' && sp[3] <= '9' && !(sp[4] >= '0' && sp[4] <= '9') && !(sp[1] == '1' && sp[2] == '0' && sp[3] == '1')) {
                if (++interims > 8)
                    return DYN_HTTP_ERR_PARSE;
                memmove(resp->data, resp->data + hdr_end,
                    resp->len - hdr_end);
                resp->len -= hdr_end;
                hdr_end = 0;
                scanned = 0;
                continue;
            }
        }
        break;
    }
    *phdr_end = hdr_end;

    {
        const char* hstart = dyn_memfind(resp->data, hdr_end, "\r\n", 2);
        const char* hp = hstart ? hstart + 2 : resp->data;
        const char* hend = resp->data + hdr_end - 2;
        size_t content_length = 0;
        int have_cl = 0, chunked = 0;

        if (!dyn_req_headers_valid(resp->data, hdr_end))
            return DYN_HTTP_ERR_PARSE;

        while (hp < hend) {
            const char* eol = dyn_memfind(hp, (size_t)(hend - hp), "\r\n", 2);
            const char* line_end = eol ? eol : hend;
            const char* colon = memchr(hp, ':', (size_t)(line_end - hp));
            if (colon) {
                size_t nlen = (size_t)(colon - hp);
                const char* val = colon + 1;
                size_t vlen = (size_t)(line_end - val);
                while (vlen > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    vlen--;
                }
                while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
                    vlen--;
                if (dyn_ci_equal(hp, nlen, "content-length")) {
                    size_t k, parsed_cl = 0;
                    if (vlen == 0)
                        return DYN_HTTP_ERR_PARSE;
                    for (k = 0; k < vlen; k++) {
                        if (val[k] < '0' || val[k] > '9')
                            return DYN_HTTP_ERR_PARSE;
                        if (parsed_cl > (SIZE_MAX - 9) / 10)
                            return DYN_HTTP_ERR_TOOBIG;
                        parsed_cl = parsed_cl * 10 + (size_t)(val[k] - '0');
                    }
                    if (have_cl && parsed_cl != content_length)
                        return DYN_HTTP_ERR_PARSE;
                    content_length = parsed_cl;
                    have_cl = 1;
                } else if (dyn_ci_equal(hp, nlen, "transfer-encoding")) {
                    if (hc_te_chunked(val, vlen))
                        chunked = 1;
                }
            }
            if (!eol)
                break;
            hp = eol + 2;
        }
        if (chunked && have_cl)
            return DYN_HTTP_ERR_PARSE;
        *pchunked = chunked;
        {
            int bodyless;
            const char* spl = (const char*)memchr(resp->data, ' ',
                hdr_end < 32 ? hdr_end : 32);
            int status = 0;
            if (spl && spl + 3 <= resp->data + hdr_end && spl[1] >= '0' && spl[1] <= '9' && spl[2] >= '0' && spl[2] <= '9' && spl[3] >= '0' && spl[3] <= '9')
                status = (spl[1] - '0') * 100 + (spl[2] - '0') * 10 + (spl[3] - '0');
            bodyless = status == 204 || status == 304
                || (method && dyn_ci_equal(method, strlen(method), "HEAD"));
            if (bodyless)
                return 0;
        }

        if (chunked || !have_cl) {
            for (;;) {
                ssize_t r;
                if (HC_CANCELLED(cancel))
                    return DYN_HTTP_ERR_CANCEL;
                if (dyn_bytes_reserve(resp, 8192) < 0)
                    return DYN_HTTP_ERR_OOM;
                r = hc_recv(conn, resp->data + resp->len, resp->cap - resp->len);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    return DYN_HTTP_ERR_RECV;
                }
                if (r == 0) {
                    if (!chunked && conn->tls_cut) {
                        snprintf(truncwhy, truncwhy_n,
                            "truncated body: the TLS stream ended without "
                            "close_notify and the response declares no length");
                        return DYN_HTTP_ERR_TRUNC;
                    }
                    break;
                }
                resp->len += (size_t)r;
                if (resp->len > max_body)
                    return DYN_HTTP_ERR_TOOBIG;
            }
        } else {
            size_t want = hdr_end + content_length;
            if (want > max_body)
                return DYN_HTTP_ERR_TOOBIG;
            while (resp->len < want) {
                ssize_t r;
                if (HC_CANCELLED(cancel))
                    return DYN_HTTP_ERR_CANCEL;
                if (dyn_bytes_reserve(resp, want - resp->len) < 0)
                    return DYN_HTTP_ERR_OOM;
                r = hc_recv(conn, resp->data + resp->len, resp->cap - resp->len);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    return DYN_HTTP_ERR_RECV;
                }
                if (r == 0) {
                    snprintf(truncwhy, truncwhy_n,
                        "truncated body: connection ended after %llu of "
                        "%llu declared bytes",
                        (unsigned long long)(resp->len - hdr_end),
                        (unsigned long long)content_length);
                    return DYN_HTTP_ERR_TRUNC;
                }
                resp->len += (size_t)r;
            }
            if (resp->len > want)
                resp->len = want;
        }
    }
    return 0;
}

static JSValue dyn_build_response(JSContext* ctx, const char* raw, size_t len,
    size_t hdr_end, int chunked,
    const char* method, const char* url)
{
    JSValue obj, headers;
    const char* line_end = dyn_memfind(raw, len, "\r\n", 2);
    const char *sp1, *sp2;
    int status = 0;
    const char* status_text = "";
    size_t status_text_len = 0;
    const char* body;
    size_t body_len;
    char* dechunked = NULL;

    if (!line_end)
        return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);

    if (line_end - raw < 9 || memcmp(raw, "HTTP/", 5) != 0)
        return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
    {
        const char* d = raw + 5;
        if (!(*d >= '0' && *d <= '9'))
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
        d++;
        if (*d == '.') {
            d++;
            if (!(*d >= '0' && *d <= '9'))
                return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
            d++;
        }
        if (*d != ' ')
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
        d++;
        if (!(line_end - d >= 3 && d[0] >= '0' && d[0] <= '9' && d[1] >= '0' && d[1] <= '9' && d[2] >= '0' && d[2] <= '9'))
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
        if (line_end - d > 3 && d[3] >= '0' && d[3] <= '9')
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
    }

    sp1 = memchr(raw, ' ', (size_t)(line_end - raw));
    if (sp1) {
        const char* code = sp1 + 1;
        unsigned acc = 0;
        int nd = 0;
        while (code < line_end && *code >= '0' && *code <= '9') {
            acc = acc * 10 + (unsigned)(*code - '0');
            nd++;
            code++;
        }
        status = nd ? (int)acc : 0;
        if (status > 599)
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
        sp2 = (code < line_end && *code == ' ') ? code + 1 : code;
        status_text = sp2;
        status_text_len = (size_t)(line_end - sp2);
    }

    body = raw + hdr_end;
    body_len = len - hdr_end;
    {
        int may_have_body = status != 204 && status != 304 && !(method && dyn_ci_equal(method, strlen(method), "HEAD"));
        if (chunked && may_have_body) {
            size_t dl = 0;
            int dr = dyn_dechunk(body, body_len, &dechunked, &dl);
            if (dr != DECHUNK_OK) {
                free(dechunked);
                return dyn_http_throw_named(ctx,
                    dr == DECHUNK_OOM ? DYN_HTTP_ERR_OOM : DYN_HTTP_ERR_PARSE,
                    method, url,
                    dr == DECHUNK_TRUNCATED
                        ? "truncated chunked body: connection ended before the "
                          "terminal chunk"
                        : dr == DECHUNK_MALFORMED
                        ? "malformed chunked body"
                        : "out of memory");
            }
            body = dechunked;
            body_len = dl;
        } else if (chunked) {
            body = "";
            body_len = 0;
        }
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        free(dechunked);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "status", JS_NewInt32(ctx, status),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
        JS_NewStringLen(ctx, status_text, status_text_len),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok",
        JS_NewBool(ctx, status >= 200 && status < 300),
        JS_PROP_C_W_E);
    headers = dyn_headers_object(ctx, line_end + 2, raw + hdr_end - 2);
    if (JS_IsException(headers)) {
        free(dechunked);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, obj, "headers", headers, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "body",
        JS_NewStringLen(ctx, body ? body : "", body_len),
        JS_PROP_C_W_E);
    {
        JSValue bb = JS_NewArrayBufferCopy(ctx,
            (const uint8_t*)(body ? body : ""),
            body_len);
        if (JS_IsException(bb)) {
            free(dechunked);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, obj, "bodyBytes", bb, JS_PROP_C_W_E);
    }
    free(dechunked);
    return obj;
}

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
} hc_exch_t;

static int hc_exchange_open(hc_exch_t* x, hc_conn_t* conn)
{
    int err = 0;
    char hbuf[256];

    conn->fd = -1;
    conn->deadline_ms = 0;
    conn->tls_cut = 0;
#ifdef CONFIG_TLS
    conn->tls = NULL;
#endif
    x->peer_ip[0] = 0;
    conn->fd = dyn_tcp_connect(dyn_host_bare(x->u.host, hbuf, sizeof hbuf),
        x->u.port, x->timeout_ms, &err);
    if (conn->fd < 0)
        return err != 0 ? err : DYN_HTTP_ERR_CONNECT;
    if (x->on_connect) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        if (getpeername(conn->fd, (struct sockaddr*)&ss, &sl) == 0) {
            char ipbuf[INET6_ADDRSTRLEN];
            const void* ap = NULL;
            if (ss.ss_family == AF_INET6)
                ap = &((struct sockaddr_in6*)&ss)->sin6_addr;
            else if (ss.ss_family == AF_INET)
                ap = &((struct sockaddr_in*)&ss)->sin_addr;
            if (ap && inet_ntop(ss.ss_family, ap, ipbuf, sizeof ipbuf))
                snprintf(x->peer_ip, sizeof x->peer_ip, "%s", ipbuf);
        }
    }
    return 0;
}

static void hc_conn_close(hc_conn_t* conn)
{
#ifdef CONFIG_TLS
    dyn_tls_conn_free(conn->tls);
    conn->tls = NULL;
#endif
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

static int hc_exchange_run(hc_exch_t* x, hc_conn_t* conn)
{
    dyn_bytes_t req = { 0 };
    char line[512];
    __maybe_unused char hbuf[256];

    if (conn->fd < 0)
        return DYN_HTTP_ERR_CONNECT;
    x->tlswhy[0] = 0;
    conn->deadline_ms = x->timeout_ms > 0
        ? dyn_timer_now_ms() + (uint64_t)x->timeout_ms * DYN_HTTP_TOTAL_TIMEOUT_FACTOR
        : 0;
    if (x->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = x->timeout_ms / 1000;
        tv.tv_usec = (x->timeout_ms % 1000) * 1000;
        setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#ifdef CONFIG_TLS
    if (x->u.tls) {
        char terr[192];
        if (!x->tls_ctx) {
            snprintf(x->tlswhy, sizeof x->tlswhy, "no TLS context");
            return DYN_HTTP_ERR_TLS;
        }
        conn->tls = dyn_tls_conn_new(x->tls_ctx,
            dyn_host_bare(x->u.host, hbuf, sizeof hbuf), terr, sizeof terr);
        if (!conn->tls || hc_tls_handshake(conn) != 0) {
            const char* why = conn->tls ? dyn_tls_error(conn->tls) : terr;
            snprintf(x->tlswhy, sizeof x->tlswhy, "%s",
                why ? why : "handshake failed");
            return DYN_HTTP_ERR_TLS;
        }
    }
#endif

    if (x->u.port == 80)
        snprintf(line, sizeof(line), "Host: %s\r\n", x->u.host);
    else
        snprintf(line, sizeof(line), "Host: %s:%u\r\n", x->u.host,
            (unsigned)x->u.port);
    if (dyn_bytes_append(&req, x->method, strlen(x->method)) < 0
        || dyn_bytes_append(&req, " ", 1) < 0
        || dyn_bytes_append(&req, x->u.path, x->u.path_len) < 0
        || dyn_bytes_append(&req, " HTTP/1.1\r\n", 11) < 0
        || dyn_bytes_append(&req, line, strlen(line)) < 0) {
        free(req.data);
        return DYN_HTTP_ERR_OOM;
    }
    if (x->hdr && dyn_bytes_append(&req, x->hdr, strlen(x->hdr)) < 0) {
        free(req.data);
        return DYN_HTTP_ERR_OOM;
    }
    if (x->body) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", x->body_len);
        if (dyn_bytes_append(&req, line, strlen(line)) < 0) {
            free(req.data);
            return DYN_HTTP_ERR_OOM;
        }
    }
    if (dyn_bytes_append(&req, "Connection: close\r\n\r\n", 21) < 0 || (x->body && dyn_bytes_append(&req, x->body, x->body_len) < 0)) {
        free(req.data);
        return DYN_HTTP_ERR_OOM;
    }

    if (hc_send(conn, req.data, req.len) < 0) {
        free(req.data);
        return DYN_HTTP_ERR_SEND;
    }
    free(req.data);
    return dyn_read_response(conn, x->method, x->max_body, &x->resp,
        &x->hdr_end, &x->chunked, x->cancel,
        x->truncwhy, sizeof x->truncwhy);
}

static void hc_exchange(hc_exch_t* x)
{
    hc_conn_t conn;

    memset(&conn, 0, sizeof conn);
    conn.fd = -1;
    x->err = hc_exchange_open(x, &conn);
    if (x->err == 0 && x->on_connect && x->peer_ip[0]
        && x->on_connect(x->peer_ip, x->on_connect_ud) != 0)
        x->err = DYN_HTTP_ERR_CONNECT;
    if (x->err == 0)
        x->err = hc_exchange_run(x, &conn);
    hc_conn_close(&conn);
}

#ifdef CONFIG_TLS
static int hc_tls_ensure(JSContext* ctx, dyn_http_client_t* cl, char* terr,
    size_t terr_n)
{
    if (!cl->tls) {
        dyn_tls_opts_t o;
        (void)ctx;
        memset(&o, 0, sizeof o);
        o.min_version = 12;
        cl->tls = dyn_tls_ctx_client(&o, terr, terr_n);
        if (!cl->tls)
            return -1;
    }
    return 0;
}
#endif

typedef struct {
    JSContext* ctx;
    JSValue fn;
} hc_onconn_t;

static int hc_js_on_connect(const char* ip, void* ud)
{
    hc_onconn_t* h = (hc_onconn_t*)ud;
    JSValueConst argv[1];
    JSValue r;
    int refuse;

    argv[0] = JS_NewString(h->ctx, ip);
    r = JS_Call(h->ctx, h->fn, JS_UNDEFINED, 1, argv);
    JS_FreeValue(h->ctx, argv[0]);
    if (JS_IsException(r)) {
        JS_FreeValue(h->ctx, JS_GetException(h->ctx));
        return 1;
    }
    refuse = !JS_ToBool(h->ctx, r);
    JS_FreeValue(h->ctx, r);
    return refuse;
}

static JSValue dyn_http_perform(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val)
{
    dyn_http_client_t* cl;
    const char* url = NULL;
    const char* body = NULL;
    size_t body_len = 0;
    char* raw_body = NULL;
    size_t raw_body_len = 0;
    char* hdr = NULL;
    int hdr_err = 0;
    hc_exch_t x;
    hc_onconn_t onconn = { NULL, JS_UNDEFINED };
    JSValue result;

    memset(&x, 0, sizeof x);
    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");
    if (!dyn_method_valid(method, strlen(method)))
        return JS_ThrowTypeError(ctx, "invalid HTTP method");

    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        if (JS_IsString(body_val)) {
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        } else {
            size_t boff = 0, blen = 0, bpe = 0, ab = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, body_val, &boff, &blen, &bpe);
            if (!JS_IsException(buf)) {
                uint8_t* base;
                if (bpe != 1) {
                    JS_FreeValue(ctx, buf);
                    JS_ThrowTypeError(ctx, "body must be a byte view (Uint8Array, "
                                           "Uint8ClampedArray, Int8Array, DataView) "
                                           "or a string; wider views are not a byte body");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                base = JS_GetArrayBuffer(ctx, &ab, buf);
                JS_FreeValue(ctx, buf);
                if (!base) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                if (boff > ab || blen > ab - boff) {
                    JS_ThrowRangeError(ctx, "body view out of bounds");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                raw_body = (char*)malloc(blen + 1);
                if (!raw_body) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                memcpy(raw_body, base + boff, blen);
                raw_body[blen] = 0;
                raw_body_len = blen;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                body = JS_ToCStringLen(ctx, &body_len, body_val);
            }
        }
        if (!body && !raw_body) {
            if (raw_body)
                free(raw_body);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }
    hdr = dyn_headers_to_string(ctx, headers_val, &hdr_err);
    if (hdr_err) {
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl) {
        free(hdr);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    if (dyn_parse_url(url, &x.u) < 0) {
        free(hdr);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        result = dyn_http_throw(ctx, DYN_HTTP_ERR_URL, method, url, NULL);
        JS_FreeCString(ctx, url);
        return result;
    }
#ifdef CONFIG_TLS
    if (x.u.tls) {
        char terr[192];
        if (hc_tls_ensure(ctx, cl, terr, sizeof terr) < 0) {
            free(hdr);
            if (body)
                JS_FreeCString(ctx, body);
            if (raw_body)
                free(raw_body);
            result = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, method, url, terr);
            JS_FreeCString(ctx, url);
            return result;
        }
        x.tls_ctx = cl->tls;
    }
#endif
    x.method = method;
    x.url = DYN_UNCONST(url);
    x.hdr = hdr;
    x.body = raw_body ? raw_body : DYN_UNCONST(body);
    x.body_len = raw_body_len ? raw_body_len : body_len;
    x.timeout_ms = cl->timeout_ms;
    x.max_body = cl->max_body;
    {
        JSValue oc = JS_GetPropertyStr(ctx, this_val, "onConnect");
        if (JS_IsFunction(ctx, oc)) {
            onconn.ctx = ctx;
            onconn.fn = oc;
            x.on_connect = hc_js_on_connect;
            x.on_connect_ud = &onconn;
        } else {
            JS_FreeValue(ctx, oc);
        }
    }
    hc_exchange(&x);

    free(hdr);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    if (x.err != 0) {
        free(x.resp.data);
        result = (x.err == DYN_HTTP_ERR_TRUNC && x.truncwhy[0])
            ? dyn_http_throw_named(ctx, x.err, method, url, x.truncwhy)
            : dyn_http_throw(ctx, x.err, method, url, x.tlswhy);
    } else {
        result = dyn_build_response(ctx, x.resp.data ? (char*)x.resp.data : "",
            x.resp.len, x.hdr_end, x.chunked,
            method, url);
        free(x.resp.data);
    }
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, onconn.fn);
    return result;
}

static JSValue dyn_http_client_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_http_perform(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue dyn_http_client_post(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_http_perform(ctx, this_val, "POST", argv[0],
        argc > 1 ? argv[1] : JS_UNDEFINED,
        argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue dyn_http_client_request(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "request(method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = dyn_http_perform(ctx, this_val, method, argv[1],
        argc > 2 ? argv[2] : JS_UNDEFINED,
        argc > 3 ? argv[3] : JS_UNDEFINED);
    JS_FreeCString(ctx, method);
    return res;
}

static long dyn_http_async_pending;
static JSContext* dyn_http_async_ctx;
static int dyn_http_async_hooked;

static void dyn_http_async_reap(void* unused)
{
    (void)unused;
    while (dyn_http_async_pending > 0 && dyn_http_async_ctx) {
        dyn_http_async_pending--;
        dyn_net_reactor_release(dyn_http_async_ctx);
    }
    if (dyn_http_async_pending == 0 && dyn_http_async_hooked) {
        dyn_http_async_hooked = 0;
        dyn_net_off_drain(&dyn_http_async_hooked);
        dyn_http_async_ctx = NULL;
    }
}

static void dyn_http_async_hook(JSContext* ctx)
{
    if (!dyn_http_async_hooked
        && dyn_net_on_drain(dyn_http_async_reap, &dyn_http_async_hooked) >= 0) {
        dyn_http_async_hooked = 1;
        dyn_http_async_ctx = ctx;
    }
}

static void dyn_http_async_release(JSContext* ctx)
{
    if (dyn_http_async_hooked)
        dyn_http_async_pending++;
    else
        dyn_net_reactor_release(ctx);
}

struct hc_job {
    hc_exch_t x;
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
};

static void hc_jobs_orphan(dyn_http_client_t* cl)
{
    hc_job_t* j;
    for (j = cl->jobs; j; j = j->jnext)
        j->cl = NULL;
    cl->jobs = NULL;
}

static void hc_job_free(hc_job_t* j)
{
    JSContext* ctx = j->ctx;
    if (ctx && !JS_IsUndefined(j->on_connect_fn))
        JS_FreeValue(ctx, j->on_connect_fn);
    j->on_connect_fn = JS_UNDEFINED;
    free(j->x.resp.data);
    free(j->method_own);
    free(j->x.url);
    free(j->x.hdr);
    free(j->x.body);
    free(j);
}

static void hc_job_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    hc_exchange(&j->x);
}

static void hc_job_run_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    j->x.err = hc_exchange_run(&j->x, &j->conn);
    hc_conn_close(&j->conn);
}

static void hc_job_connect_work(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    j->x.err = hc_exchange_open(&j->x, &j->conn);
}

static void hc_job_unlink(hc_job_t* j)
{
    hc_job_t** pp;
    if (!j->cl)
        return;
    pp = &j->cl->jobs;
    while (*pp && *pp != j)
        pp = &(*pp)->jnext;
    if (*pp)
        *pp = j->jnext;
    j->jnext = NULL;
}

static void hc_job_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    hc_job_t* j = (hc_job_t*)opaque;
    dyn_http_client_t* cl = j->cl;

    if (cl) {
        JSValue v = JS_UNDEFINED;
        hc_job_unlink(j);
        j->cl = NULL;
        if (--cl->n_async <= 0) {
            v = cl->self_pending;
            cl->self_pending = JS_UNDEFINED;
        }
        if (ctx)
            JS_FreeValue(ctx, v);
        else
            JS_FreeValueRT(rt, v);
    }
    if (ctx) {
        JSValue e = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message",
                JS_NewString(ctx, "HTTP exchange aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            e = JS_GetException(ctx);
        }
        r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst*)&e);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }
    if (ctx) {
        JS_FreeValue(ctx, j->resolve);
        JS_FreeValue(ctx, j->reject);
    } else {
        JS_FreeValueRT(rt, j->resolve);
        JS_FreeValueRT(rt, j->reject);
    }
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
}

static void hc_job_done(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;
    JSContext* ctx = j->ctx;
    JSValue v, r;
    JSValueConst a1[1];

    JS_RemoveShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
    if (!j->cl && JS_IsUndefined(j->resolve)) {
        hc_job_free(j);
        return;
    }

    if (j->cl) {
        hc_job_unlink(j);
        dyn_http_client_unpin(j->cl);
        j->cl = NULL;
    }

    {
        int reject_it = j->x.err != 0;
        if (j->x.err) {
            v = (j->x.err == DYN_HTTP_ERR_TRUNC && j->x.truncwhy[0])
                ? hc_error_value_named(ctx, j->x.err, j->x.method, j->x.url,
                      j->x.truncwhy)
                : hc_error_value(ctx, j->x.err, j->x.method, j->x.url,
                      j->x.tlswhy[0] ? j->x.tlswhy : NULL);
        } else {
            v = dyn_build_response(ctx, j->x.resp.data ? (char*)j->x.resp.data : "",
                j->x.resp.len, j->x.hdr_end, j->x.chunked,
                j->x.method, j->x.url);
        }
        if (JS_IsException(v)) {
            v = JS_GetException(ctx);
            reject_it = 1;
        }
        a1[0] = v;
        r = JS_Call(ctx, reject_it ? j->reject : j->resolve, JS_UNDEFINED, 1, a1);
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    if (j->offloaded)
        dyn_http_async_release(j->ctx);
    hc_job_free(j);
}

static void hc_job_connect_done(void* arg)
{
    hc_job_t* j = (hc_job_t*)arg;

    if (j->x.err == 0 && j->x.on_connect && j->x.peer_ip[0]
        && j->x.on_connect(j->x.peer_ip, j->x.on_connect_ud) != 0)
        j->x.err = DYN_HTTP_ERR_CONNECT;
    if (j->x.err != 0) {
        hc_conn_close(&j->conn);
        hc_job_done(j);
        return;
    }
    if (j->offloaded && j->aio) {
        dyn_aio_offload(j->aio, hc_job_run_work, hc_job_done, j);
        return;
    }
    j->x.err = hc_exchange_run(&j->x, &j->conn);
    hc_conn_close(&j->conn);
    hc_job_done(j);
}

static JSValue hc_submit_async(JSContext* ctx, JSValueConst this_val,
    const char* method, JSValueConst url_val,
    JSValueConst body_val, JSValueConst headers_val)
{
    dyn_http_client_t* cl;
    const char* url = NULL;
    const char* body = NULL;
    size_t body_len = 0;
    char* raw_body = NULL;
    size_t raw_body_len = 0;
    char* hdr = NULL;
    int hdr_err = 0;
    hc_job_t* j;
    JSValue funcs[2], promise;
    struct dyn_aio* aio;

    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");
    if (!dyn_method_valid(method, strlen(method)))
        return JS_ThrowTypeError(ctx, "invalid HTTP method");
    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        if (JS_IsString(body_val)) {
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        } else {
            size_t boff = 0, blen = 0, bpe = 0, ab = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, body_val, &boff, &blen, &bpe);
            if (!JS_IsException(buf)) {
                uint8_t* base;
                if (bpe != 1) {
                    JS_FreeValue(ctx, buf);
                    JS_ThrowTypeError(ctx, "body must be a byte view (Uint8Array, "
                                           "Uint8ClampedArray, Int8Array, DataView) "
                                           "or a string; wider views are not a byte body");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                base = JS_GetArrayBuffer(ctx, &ab, buf);
                JS_FreeValue(ctx, buf);
                if (!base) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                if (boff > ab || blen > ab - boff) {
                    JS_ThrowRangeError(ctx, "body view out of bounds");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                raw_body = (char*)malloc(blen + 1);
                if (!raw_body) {
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                memcpy(raw_body, base + boff, blen);
                raw_body[blen] = 0;
                raw_body_len = blen;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                body = JS_ToCStringLen(ctx, &body_len, body_val);
            }
        }
        if (!body && !raw_body) {
            if (raw_body)
                free(raw_body);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }
    hdr = dyn_headers_to_string(ctx, headers_val, &hdr_err);
    if (hdr_err) {
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl) {
        free(hdr);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    j = (hc_job_t*)calloc(1, sizeof(*j));
    if (!j)
        goto oom;
    j->ctx = ctx;
    j->on_connect_fn = JS_UNDEFINED;
    j->x.hdr = hdr;
    hdr = NULL;
    j->method_own = strdup(method);
    j->x.url = strdup(url);
    j->x.body = NULL;
    if (body || raw_body) {
        j->x.body = (char*)malloc((body ? body_len : raw_body_len) + 1);
        if (j->x.body) {
            if (body) {
                memcpy(j->x.body, body, body_len);
            } else {
                memcpy(j->x.body, raw_body, raw_body_len);
            }
            j->x.body[body ? body_len : raw_body_len] = 0;
        }
    }
    if (!j->method_own || !j->x.url || ((body || raw_body) && !j->x.body)) {
        hc_job_free(j);
        if (raw_body)
            free(raw_body);
        goto oom;
    }
    j->x.method = j->method_own;
    j->x.body_len = body ? body_len : raw_body_len;
    if (dyn_parse_url(j->x.url, &j->x.u) < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_URL, method, url, NULL);
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return e;
    }
#ifdef CONFIG_TLS
    if (j->x.u.tls) {
        char terr[192];
        if (hc_tls_ensure(ctx, cl, terr, sizeof terr) < 0) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, method, url, terr);
            hc_job_free(j);
            if (body)
                JS_FreeCString(ctx, body);
            if (raw_body)
                free(raw_body);
            JS_FreeCString(ctx, url);
            return e;
        }
        j->x.tls_ctx = cl->tls;
    }
#endif
    j->x.timeout_ms = cl->timeout_ms;
    j->x.max_body = cl->max_body;
    {
        JSValue oc = JS_GetPropertyStr(ctx, this_val, "onConnect");
        if (JS_IsFunction(ctx, oc)) {
            j->on_connect_fn = oc;
            j->onconn.ctx = ctx;
            j->onconn.fn = oc;
            j->x.on_connect = hc_js_on_connect;
            j->x.on_connect_ud = &j->onconn;
        } else {
            JS_FreeValue(ctx, oc);
        }
    }

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        hc_job_free(j);
        if (body)
            JS_FreeCString(ctx, body);
        if (raw_body)
            free(raw_body);
        JS_FreeCString(ctx, url);
        return promise;
    }
    j->resolve = funcs[0];
    j->reject = funcs[1];
    atomic_store_explicit(&j->cancelled, 0, memory_order_relaxed);
    j->x.cancel = &j->cancelled;
    j->jnext = cl->jobs;
    cl->jobs = j;
    j->cl = cl;
    cl->n_async++;
    if (cl->n_async == 1)
        cl->self_pending = JS_DupValue(ctx, this_val);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);

    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) {
        if (j->x.on_connect)
            hc_job_connect_done(j);
        else {
            hc_job_work(j);
            hc_job_done(j);
        }
        return promise;
    }
    dyn_http_async_hook(ctx);
    j->offloaded = 1;
    j->aio = aio;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
    if (j->x.on_connect)
        dyn_aio_offload(aio, hc_job_connect_work, hc_job_connect_done, j);
    else
        dyn_aio_offload(aio, hc_job_work, hc_job_done, j);
    return promise;

oom:
    free(hdr);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);
    return JS_ThrowOutOfMemory(ctx);
}

static JSValue dyn_http_client_request_pending(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "HTTPClient: the pending request needs (method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = hc_submit_async(ctx, this_val, method, argv[1],
        argc > 2 ? argv[2] : JS_UNDEFINED,
        argc > 3 ? argv[3] : JS_UNDEFINED);
    JS_FreeCString(ctx, method);
    return res;
}

static JSValue dyn_http_client_set_timeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    int64_t ms;

    (void)argc;
    if (JS_ToInt64(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    cl->timeout_ms = ms;
    return JS_UNDEFINED;
}

typedef struct {
    hc_conn_t conn;
    int chunked;
    uint64_t clen;
    uint64_t served;
    uint64_t max_body;
    int eof;
    int failed;
    uint64_t chunk_left;
    int need_size;
    uint8_t* rbuf;
    size_t rn, rcap, rpos;
    char* err;
} hc_stream_t;

static JSClassID dyn_hc_stream_class_id;

static void hc_stream_dispose(void* native)
{
    hc_stream_t* s = (hc_stream_t*)native;
    if (!s)
        return;
#ifdef CONFIG_TLS
    dyn_tls_conn_free(s->conn.tls);
    s->conn.tls = NULL;
#endif
    if (s->conn.fd >= 0)
        close(s->conn.fd);
    free(s->rbuf);
    free(s->err);
    free(s);
}

static const JSClassDef dyn_hc_stream_class = {
    "HTTPBodyStream",
    .finalizer = dyn_res_finalizer,
};

static void hc_stream_fail(hc_stream_t* s, const char* msg)
{
    if (!s->failed) {
        s->failed = 1;
        free(s->err);
        s->err = strdup(msg);
    }
}

static int hc_stream_fill(hc_stream_t* s, char* why, size_t whyn)
{
    ssize_t r;
    if (s->rn == s->rcap) {
        size_t nc = s->rcap ? s->rcap * 2 : 8192;
        uint8_t* nb = (uint8_t*)realloc(s->rbuf, nc);
        if (!nb) {
            snprintf(why, whyn, "out of memory");
            return -1;
        }
        s->rbuf = nb;
        s->rcap = nc;
    }
    r = hc_recv(&s->conn, s->rbuf + s->rn, s->rcap - s->rn);
    if (r < 0) {
        snprintf(why, whyn, "receiving body failed");
        return -1;
    }
    if (r == 0) {
        s->eof = 1;
        return 0;
    }
    s->rn += (size_t)r;
    return 0;
}

static int hc_stream_line(hc_stream_t* s, char* line, size_t cap,
    char* why, size_t whyn)
{
    size_t o = 0;
    for (;;) {
        if (s->rpos >= s->rn) {
            s->rpos = s->rn = 0;
            if (hc_stream_fill(s, why, whyn) < 0)
                return -1;
            if (s->eof) {
                snprintf(why, whyn, "truncated chunked body: connection ended mid-line");
                return -1;
            }
            continue;
        }
        {
            uint8_t c = s->rbuf[s->rpos++];
            if (c == '\n') {
                if (o && line[o - 1] == '\r')
                    o--;
                line[o] = 0;
                return 0;
            }
            if (o + 1 >= cap) {
                snprintf(why, whyn, "chunk line exceeds %zu bytes", cap - 1);
                return -1;
            }
            line[o++] = (char)c;
        }
    }
}

static size_t hc_stream_take(hc_stream_t* s, uint8_t* dst, size_t want)
{
    size_t have = s->rn - s->rpos;
    if (have > want)
        have = want;
    memcpy(dst, s->rbuf + s->rpos, have);
    s->rpos += have;
    if (s->rpos == s->rn)
        s->rpos = s->rn = 0;
    return have;
}

#define HC_CHUNK_SIZE_MAX_DIGITS 15

static long long hc_stream_body(hc_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn)
{
    size_t got = 0;
    if (s->eof)
        return 0;
    while (got < want) {
        if (s->chunked) {
            if (s->need_size) {
                char line[64];
                if (hc_stream_line(s, line, sizeof line, why, whyn) < 0)
                    return -1;
                {
                    const char* endp = line;
                    long long sz = 0;
                    int digits = 0;
                    for (;; endp++, digits++) {
                        int hv = *endp >= '0' && *endp <= '9' ? *endp - '0'
                            : *endp >= 'a' && *endp <= 'f' ? *endp - 'a' + 10
                            : *endp >= 'A' && *endp <= 'F' ? *endp - 'A' + 10
                                                           : -1;
                        if (hv < 0)
                            break;
                        if (digits >= HC_CHUNK_SIZE_MAX_DIGITS) {
                            digits = 0;
                            break;
                        }
                        sz = sz * 16 + hv;
                    }
                    if (digits == 0 || (*endp && *endp != ';' && *endp != ' ')) {
                        snprintf(why, whyn, "malformed chunk size \"%.16s\"", line);
                        return -1;
                    }
                    if (sz == 0) {
                        uint64_t budget = s->max_body > s->served
                            ? s->max_body - s->served
                            : 0;
                        uint64_t trailer = 0;
                        for (;;) {
                            if (hc_stream_line(s, line, sizeof line, why, whyn) < 0)
                                return -1;
                            if (!line[0])
                                break;
                            trailer += (uint64_t)strlen(line) + 2;
                            if (trailer > budget) {
                                snprintf(why, whyn, "body exceeds maxBodyBytes");
                                return -1;
                            }
                        }
                        s->eof = 1;
                        return (long long)got;
                    }
                    s->chunk_left = (uint64_t)sz;
                    s->need_size = 0;
                }
            }
            if (s->chunk_left == 0) {
                char line[64];
                if (hc_stream_line(s, line, sizeof line, why, whyn) < 0)
                    return -1;
                if (line[0]) {
                    snprintf(why, whyn, "malformed chunk terminator");
                    return -1;
                }
                s->need_size = 1;
                continue;
            }
            {
                size_t room = (size_t)(s->chunk_left < (uint64_t)(want - got)
                        ? s->chunk_left
                        : (uint64_t)(want - got));
                size_t moved = hc_stream_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                s->chunk_left -= moved;
                if (moved < room) {
                    if (hc_stream_fill(s, why, whyn) < 0)
                        return -1;
                    if (s->eof) {
                        snprintf(why, whyn, "truncated chunked body: connection ended mid-chunk");
                        return -1;
                    }
                }
            }
        } else {
            uint64_t left = s->clen == UINT64_MAX
                ? (uint64_t)(want - got)
                : (s->clen > s->served ? s->clen - s->served : 0);
            if (left == 0) {
                s->eof = 1;
                return (long long)got;
            }
            {
                size_t room = (size_t)((uint64_t)(want - got) < left
                        ? (uint64_t)(want - got)
                        : left);
                size_t moved = hc_stream_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                if (moved < room) {
                    if (hc_stream_fill(s, why, whyn) < 0)
                        return -1;
                    if (s->eof) {
                        if (s->clen != UINT64_MAX && s->served < s->clen) {
                            snprintf(why, whyn,
                                "truncated body: connection ended after "
                                "%llu of %llu declared bytes",
                                (unsigned long long)s->served,
                                (unsigned long long)s->clen);
                            return -1;
                        }
                        s->eof = 1;
                        return (long long)got;
                    }
                }
            }
        }
        if (s->served > s->max_body) {
            snprintf(why, whyn, "body exceeds maxBodyBytes");
            return -1;
        }
    }
    return (long long)got;
}

static JSValue hc_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue hc_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;
    if (JS_IsException(exc))
        exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static uint8_t* hc_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
{
    size_t n = 0;
    uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
    *plen = 0;
    if (p) {
        *plen = n;
        return p;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return NULL;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx,
                "HTTPBodyStream.read: buf must be a byte-wide view "
                "(Uint8Array/DataView/ArrayBuffer)");
            return NULL;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return NULL;
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}

static JSValue dyn_hc_stream_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t len = 0;
    uint8_t* base;
    hc_stream_t* s;
    char why[160];

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "HTTPBodyStream.read: buf must be a byte-wide view (Uint8Array)");
    base = hc_view_bytes(ctx, argv[0], &len);
    if (!base)
        return JS_EXCEPTION;
    s = (hc_stream_t*)dyn_res_native(ctx, this_val, dyn_hc_stream_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (len == 0)
        return hc_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (s->failed)
        return hc_promise_rejected(ctx,
            JS_ThrowRangeError(ctx, "HTTPBodyStream: %s",
                s->err ? s->err : "body read failed"));
    if (s->eof)
        return hc_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    {
        long long n = hc_stream_body(s, base, len, why, sizeof why);
        if (n < 0) {
            JSValue exc;
            hc_stream_fail(s, why);
#ifdef CONFIG_TLS
            dyn_tls_conn_free(s->conn.tls);
            s->conn.tls = NULL;
#endif
            if (s->conn.fd >= 0) {
                close(s->conn.fd);
                s->conn.fd = -1;
            }
            exc = JS_ThrowRangeError(ctx, "HTTPBodyStream: %s", why);
            return hc_promise_rejected(ctx, exc);
        }
        if (n == 0)
            s->eof = 1;
        return hc_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
}

static const JSCFunctionListEntry dyn_hc_stream_proto[] = {
    JS_CFUNC_DEF("read", 1, dyn_hc_stream_read),
};

#define HC_STREAM_HDR_MAX (32 * 1024)
static char* hc_read_head(hc_conn_t* conn, size_t* rawlen,
    uint8_t** pextra, size_t* pextra_len,
    char* why, size_t whyn)
{
    char* buf = (char*)malloc(HC_STREAM_HDR_MAX + 1);
    size_t n = 0, scanned = 0;
    int skips = 0;
    *pextra = NULL;
    *pextra_len = 0;
    if (!buf) {
        snprintf(why, whyn, "out of memory");
        return NULL;
    }
    for (;;) {
        const char* marker = dyn_memfind(buf + scanned, n - scanned,
            "\r\n\r\n", 4);
        while (!marker && n < HC_STREAM_HDR_MAX) {
            ssize_t r;
            scanned = n > 3 ? n - 3 : 0;
            r = hc_recv(conn, buf + n, HC_STREAM_HDR_MAX - n);
            if (r < 0) {
                snprintf(why, whyn, "receiving response head failed");
                goto bad;
            }
            if (r == 0) {
                snprintf(why, whyn, "connection closed before response head");
                goto bad;
            }
            n += (size_t)r;
            marker = dyn_memfind(buf + scanned, n - scanned, "\r\n\r\n", 4);
        }
        if (!marker) {
            snprintf(why, whyn, "response head exceeds %d bytes",
                HC_STREAM_HDR_MAX);
            goto bad;
        }
        {
            size_t head_end = (size_t)(marker - buf) + 4;
            const char* p;
            int status;
            if (head_end < 12 || memcmp(buf, "HTTP/", 5) != 0) {
                snprintf(why, whyn, "not an HTTP/1.x response");
                goto bad;
            }
            p = buf + 5;
            if (*p >= '0' && *p <= '9') {
                p++;
                if (*p == '.' && p[1] >= '0' && p[1] <= '9')
                    p += 2;
            }
            if (*p != ' ' || !(p - buf >= 8)) {
                snprintf(why, whyn, "malformed status line");
                goto bad;
            }
            p++;
            if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9')) {
                snprintf(why, whyn, "malformed status code");
                goto bad;
            }
            status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
            if (status >= 100 && status < 200 && status != 101) {
                if (++skips > 8) {
                    snprintf(why, whyn, "too many interim responses");
                    goto bad;
                }
                memmove(buf, buf + head_end, n - head_end);
                n -= head_end;
                scanned = 0;
                continue;
            }
            buf[n] = 0;
            *rawlen = head_end;
            if (n > head_end) {
                *pextra = (uint8_t*)malloc(n - head_end);
                if (!*pextra) {
                    snprintf(why, whyn, "out of memory");
                    goto bad;
                }
                memcpy(*pextra, buf + head_end, n - head_end);
                *pextra_len = n - head_end;
            }
            return buf;
        }
    }
bad:
    free(buf);
    return NULL;
}

static char* hc_head_value(const char* raw, size_t n, const char* want)
{
    const char* p = raw;
    const char* end = raw + n;
    char* out = NULL;
    size_t wl = strlen(want);
    while (p < end) {
        const char* eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        const char* colon;
        if (ll && p[ll - 1] == '\r')
            ll--;
        colon = (const char*)memchr(p, ':', ll);
        if (colon) {
            size_t kl = (size_t)(colon - p);
            const char* v = colon + 1;
            size_t vl = ll - kl - 1;
            while (vl && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t'))
                vl--;
            if (kl == wl && strncasecmp(p, want, kl) == 0) {
                free(out);
                out = (char*)malloc(vl + 1);
                if (out) {
                    memcpy(out, v, vl);
                    out[vl] = 0;
                }
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    return out;
}

static int hc_head_framing(const char* raw, size_t n, int* pchunked,
    uint64_t* pclen, char* why, size_t whyn)
{
    const char* p = raw;
    const char* end = raw + n;
    int have_cl = 0, chunked = 0;
    uint64_t cl = 0;
    while (p < end) {
        const char* eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        const char* colon;
        if (ll && p[ll - 1] == '\r')
            ll--;
        if (ll == 0)
            break;
        colon = (const char*)memchr(p, ':', ll);
        if (colon) {
            size_t kl = (size_t)(colon - p);
            const char* v = colon + 1;
            size_t vl = ll - kl - 1;
            while (vl && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t'))
                vl--;
            if (kl == 14 && strncasecmp(p, "content-length", 14) == 0) {
                uint64_t parsed = 0;
                size_t k;
                if (vl == 0) {
                    snprintf(why, whyn, "empty Content-Length");
                    return -1;
                }
                for (k = 0; k < vl; k++) {
                    if (v[k] < '0' || v[k] > '9') {
                        snprintf(why, whyn, "malformed Content-Length");
                        return -1;
                    }
                    if (parsed > (UINT64_MAX - 9) / 10) {
                        snprintf(why, whyn, "malformed Content-Length");
                        return -1;
                    }
                    parsed = parsed * 10 + (uint64_t)(v[k] - '0');
                }
                if (have_cl && parsed != cl) {
                    snprintf(why, whyn,
                        "conflicting duplicate Content-Length");
                    return -1;
                }
                cl = parsed;
                have_cl = 1;
            } else if (kl == 17 && strncasecmp(p, "transfer-encoding", 17) == 0) {
                if (hc_te_chunked(v, vl))
                    chunked = 1;
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    if (chunked && have_cl) {
        snprintf(why, whyn,
            "Content-Length with Transfer-Encoding: chunked");
        return -1;
    }
    *pchunked = chunked;
    *pclen = have_cl ? cl : UINT64_MAX;
    return 0;
}

static JSValue dyn_http_client_get_stream(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    const char* url0;
    char *url = NULL, *hdr = NULL;
    int hdr_err = 0;
    dyn_url_t u;
    hc_conn_t conn = { -1,
#ifdef CONFIG_TLS
        NULL
#endif
    };
    char* raw = NULL;
    size_t rawlen = 0;
    char why[160];
    JSValue ret = JS_EXCEPTION;
    int status = 0;
    char status_text[64] = "";
    char* ctv = NULL;
    hc_stream_t* s = NULL;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getStream(url[, headers])");
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0)
        return JS_EXCEPTION;
    url = strdup(url0);
    JS_FreeCString(ctx, url0);
    if (!url)
        return JS_ThrowOutOfMemory(ctx);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        hdr = dyn_headers_to_string(ctx, argv[1], &hdr_err);
    if (hdr_err) {
        free(url);
        return JS_EXCEPTION;
    }
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl) {
        free(url);
        free(hdr);
        return JS_EXCEPTION;
    }

    if (dyn_parse_url(url, &u) < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_URL, "GET", url, NULL);
        free(url);
        free(hdr);
        return e;
    }
#ifdef CONFIG_TLS
    if (u.tls) {
        char terr[192];
        if (hc_tls_ensure(ctx, cl, terr, sizeof terr) < 0) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, "GET", url, terr);
            free(url);
            free(hdr);
            return e;
        }
    }
#endif
    char hbuf2[256];
    conn.fd = dyn_tcp_connect(dyn_host_bare(u.host, hbuf2, sizeof hbuf2),
        u.port, cl->timeout_ms, &hdr_err);
    if (conn.fd < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "GET", url, NULL);
        free(url);
        free(hdr);
        return e;
    }
    {
        JSValue oc = JS_GetPropertyStr(ctx, this_val, "onConnect");
        if (JS_IsFunction(ctx, oc)) {
            hc_onconn_t onconn = { ctx, oc };
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            int refused = 0;
            if (getpeername(conn.fd, (struct sockaddr*)&ss, &sl) == 0) {
                char ipbuf[INET6_ADDRSTRLEN];
                const void* ap = NULL;
                if (ss.ss_family == AF_INET6)
                    ap = &((struct sockaddr_in6*)&ss)->sin6_addr;
                else if (ss.ss_family == AF_INET)
                    ap = &((struct sockaddr_in*)&ss)->sin_addr;
                if (ap && inet_ntop(ss.ss_family, ap, ipbuf, sizeof ipbuf))
                    refused = hc_js_on_connect(ipbuf, &onconn) != 0;
            }
            JS_FreeValue(ctx, oc);
            if (refused) {
                JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "GET",
                    url, "onConnect refused the connection");
                free(url);
                free(hdr);
                close(conn.fd);
                return e;
            }
        } else {
            JS_FreeValue(ctx, oc);
        }
    }
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl) {
        free(url);
        free(hdr);
        close(conn.fd);
        return JS_EXCEPTION;
    }
    if (cl->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = cl->timeout_ms / 1000;
        tv.tv_usec = (cl->timeout_ms % 1000) * 1000;
        (void)setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(conn.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    dyn_set_nodelay(conn.fd);
#ifdef CONFIG_TLS
    if (u.tls) {
        char terr[192];
        conn.tls = dyn_tls_conn_new(cl->tls,
            dyn_host_bare(u.host, hbuf2, sizeof hbuf2), terr, sizeof terr);
        if (!conn.tls || hc_tls_handshake(&conn) != 0) {
            const char* w = conn.tls ? dyn_tls_error(conn.tls) : terr;
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, "GET", url,
                w ? w : "handshake failed");
            free(url);
            free(hdr);
            dyn_tls_conn_free(conn.tls);
            close(conn.fd);
            return e;
        }
    }
#endif
    {
        dyn_bytes_t req = { 0 };
        char line[512];
        int bad = dyn_bytes_append(&req, "GET ", 4) < 0 || dyn_bytes_append(&req, u.path, u.path_len) < 0 || dyn_bytes_append(&req, " HTTP/1.1\r\n", 11) < 0;
        if (!bad) {
            if (u.port == 80)
                snprintf(line, sizeof(line), "Host: %s\r\n", u.host);
            else
                snprintf(line, sizeof(line), "Host: %s:%u\r\n", u.host,
                    (unsigned)u.port);
            bad = dyn_bytes_append(&req, line, strlen(line)) < 0 || (hdr && dyn_bytes_append(&req, hdr, strlen(hdr)) < 0) || dyn_bytes_append(&req, "Connection: close\r\n\r\n", 21) < 0;
        }
        if (!bad && hc_send(&conn, req.data, req.len) < 0)
            bad = 2;
        free(req.data);
        if (bad) {
            JSValue e = dyn_http_throw(ctx,
                bad == 2 ? DYN_HTTP_ERR_SEND
                         : DYN_HTTP_ERR_OOM,
                "GET", url, NULL);
            free(url);
            free(hdr);
#ifdef CONFIG_TLS
            dyn_tls_conn_free(conn.tls);
#endif
            close(conn.fd);
            return e;
        }
    }
    {
        uint8_t* extra = NULL;
        size_t extra_len = 0;
        raw = hc_read_head(&conn, &rawlen, &extra, &extra_len, why,
            sizeof why);
        if (!raw) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_RECV, "GET", url,
                why);
            free(extra);
            free(url);
            free(hdr);
#ifdef CONFIG_TLS
            dyn_tls_conn_free(conn.tls);
#endif
            close(conn.fd);
            return e;
        }
        {
            const char* sp = raw + 5;
            while (*sp != ' ')
                sp++;
            sp++;
            status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
            {
                const char* rs = sp + 3;
                const char* eol = (const char*)memchr(rs, '\r', 64);
                size_t rn = eol ? (size_t)(eol - rs) : 0;
                if (rn >= sizeof status_text)
                    rn = sizeof status_text - 1;
                if (rn && *rs == ' ') {
                    rs++;
                    rn--;
                }
                memcpy(status_text, rs, rn);
            }
        }
        s = (hc_stream_t*)calloc(1, sizeof *s);
        if (!s) {
            JS_ThrowOutOfMemory(ctx);
            free(extra);
            goto out;
        }
        if (extra_len) {
            s->rbuf = extra;
            s->rcap = extra_len;
            s->rn = extra_len;
        }
    }
    ctv = hc_head_value(raw, rawlen, "content-type");
    {
        int chunked = 0;
        uint64_t cl64 = UINT64_MAX;
        if (hc_head_framing(raw, rawlen, &chunked, &cl64, why,
                sizeof why)
            < 0) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "GET", url,
                why);
            hc_stream_dispose(s);
            free(ctv);
            free(url);
            free(hdr);
#ifdef CONFIG_TLS
            dyn_tls_conn_free(conn.tls);
#endif
            close(conn.fd);
            return e;
        }
        s->chunked = chunked;
        s->clen = chunked ? UINT64_MAX : cl64;
    }
    s->conn = conn;
    conn.fd = -1;
#ifdef CONFIG_TLS
    conn.tls = NULL;
#endif
    s->max_body = cl->max_body;
    s->need_size = s->chunked;
    if (!s->chunked && s->clen != UINT64_MAX && s->clen > s->max_body) {
        JS_ThrowRangeError(ctx,
            "HTTPClient.getStream: declared Content-Length %llu exceeds the "
            "client's max body",
            (unsigned long long)s->clen);
        hc_stream_dispose(s);
        s = NULL;
        goto out;
    }
    {
        JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_hc_stream_class_id,
            s, hc_stream_dispose);
        if (JS_IsException(obj)) {
            hc_stream_dispose(s);
            s = NULL;
            goto out;
        }
        JS_DefinePropertyValueStr(ctx, obj, "status",
            JS_NewInt32(ctx, status), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "statusText",
            JS_NewString(ctx, status_text), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ok",
            JS_NewBool(ctx, status >= 200 && status < 300),
            JS_PROP_C_W_E);
        {
            const char* hstart = (const char*)memchr(raw, '\n', rawlen);
            JSValue hh = hstart
                ? dyn_headers_object(ctx, hstart + 1, raw + rawlen - 2)
                : JS_EXCEPTION;
            if (JS_IsException(hh)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                hh = JS_NewObject(ctx);
            }
            JS_DefinePropertyValueStr(ctx, obj, "headers", hh, JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, obj, "url", JS_NewString(ctx, url),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "contentType",
            JS_NewString(ctx, ctv ? ctv : ""),
            JS_PROP_C_W_E);
        ret = obj;
        s = NULL;
    }
out:
    if (s)
        hc_stream_dispose(s);
    free(raw);
    free(ctv);
    free(url);
    free(hdr);
#ifdef CONFIG_TLS
    dyn_tls_conn_free(conn.tls);
#endif
    if (conn.fd >= 0)
        close(conn.fd);
    return ret;
}

static JSValue dyn_http_client_disconnect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_client_t* cl;
    (void)argc;
    (void)argv;
    cl = (dyn_http_client_t*)dyn_res_native(ctx, this_val,
        dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    {
        hc_job_t* j;
        for (j = cl->jobs; j; j = j->jnext)
            atomic_store_explicit(&j->cancelled, 1, memory_order_relaxed);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_http_client_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_http_client_get),
    JS_CFUNC_DEF("post", 1, dyn_http_client_post),
    JS_CFUNC_DEF("request", 2, dyn_http_client_request),
    JS_CFUNC_DEF("setTimeout", 1, dyn_http_client_set_timeout),
    JS_CFUNC_DEF("disconnect", 0, dyn_http_client_disconnect),
    JS_CFUNC_DEF("getStream", 1, dyn_http_client_get_stream),
};

static JSClassID dyn_http_server_class_id;

typedef struct {
    char* path;
    int status;
    char* content_type;
    char* body;
    size_t body_len;
} dyn_route_t;

typedef struct dyn_http_async dyn_http_async_t;

#define DYN_ACONN_IDLE_MS_DEFAULT 30000
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
};

static int dyn_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void dyn_http_async_stop_internal(struct dyn_http_async* s);
static int dyn_http_async_spawn(struct dyn_http_async* s);

typedef struct {
    int listen_fd;
    uint16_t port;
    int backlog;
    int num_workers;
    int req_timeout_ms;

    dyn_route_t* routes;
    size_t n_routes;

    struct dyn_http_async* async;
    int started;
    JSContext* ctx;
} dyn_http_server_t;

static const char* dyn_reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 415:
        return "Unsupported Media Type";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return status < 200 ? "Continue"
            : status < 300  ? "OK"
            : status < 400  ? "Found"
            : status < 500  ? "Bad Request"
                            : "Internal Server Error";
    }
}

static const dyn_route_t* dyn_route_lookup(const dyn_route_t* routes,
    size_t n_routes, const char* path)
{
    size_t i;
    for (i = 0; i < n_routes; i++) {
        if (strcmp(routes[i].path, path) == 0)
            return &routes[i];
    }
    return NULL;
}

static int dyn_ci_eq(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

static const char* dyn_req_header(const char* buf, size_t len,
    const char* name, size_t* vlen)
{
    size_t nlen = strlen(name), i;
    for (i = 0; i + 2 + nlen + 1 <= len; i++) {
        const char *v, *ve, *end;
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;
        if (!dyn_ci_eq(buf + i + 2, name, nlen) || buf[i + 2 + nlen] != ':')
            continue;
        v = buf + i + 2 + nlen + 1;
        end = buf + len;
        while (v < end && (*v == ' ' || *v == '\t'))
            v++;
        ve = v;
        while (ve < end && *ve != '\r' && *ve != '\n')
            ve++;
        *vlen = (size_t)(ve - v);
        return v;
    }
    return NULL;
}

static int dyn_hdr_token(const char* v, size_t vlen, const char* tok)
{
    size_t tlen = strlen(tok), i = 0;
    if (!v)
        return 0;
    while (i <= vlen) {
        size_t e = i, b = i;
        while (e < vlen && v[e] != ',')
            e++;
        while (b < e && (v[b] == ' ' || v[b] == '\t'))
            b++;
        while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t'))
            e--;
        if (e - b == tlen && dyn_ci_eq(v + b, tok, tlen))
            return 1;
        if (e >= vlen)
            break;
        i = e + 1;
    }
    return 0;
}

static int dyn_req_headers_valid(const char* buf, size_t len)
{
    size_t i;
    for (i = 0; i + 2 < len; i++) {
        const char *line, *c, *end;
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;
        line = buf + i + 2;
        if (line[0] == '\r')
            return 1;
        end = buf + len;
        for (c = line; c < end && *c != '\r' && *c != '\n' && *c != ':'; c++) {
            if (*c == ' ' || *c == '\t' || (unsigned char)*c < 0x20 || (unsigned char)*c == 0x7f)
                return 0;
        }
        if (c == line || c >= end || *c != ':')
            return 0;
        if (c[-1] == ' ' || c[-1] == '\t')
            return 0;
        {
            const char* v;
            for (v = c + 1; v < end; v++) {
                unsigned char ch = (unsigned char)*v;
                if (ch == '\r' || ch == '\n') {
                    if (ch != '\r' || v + 1 >= end || v[1] != '\n')
                        return 0;
                    break;
                }
                if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
                    return 0;
            }
        }
    }
    return 1;
}

typedef enum {
    DYN_HEAD_OK = 0,
    DYN_HEAD_ERR_LINE = 1,
    DYN_HEAD_ERR_DUP = 2,
} dyn_head_err;

typedef struct {
    const char* cl;
    size_t cl_len;
    int cl_count;
    int has_te;
    int host_count;
    int cl_set;
    int dup;
    const char *ae, *conn, *ct;
    size_t ae_len, conn_len, ct_len;
    int http11;
    const char* method;
    size_t method_len;
    const char* target;
    size_t target_len;
    int bad_target;
    size_t head_len;
    size_t n_lines;
} dyn_reqinfo_t;

static int dyn_head_dup_ok(const char* n, size_t len)
{
    static const struct {
        const char* s;
        size_t l;
    } ok[] = {
        { "cookie", 6 },
        { "set-cookie", 10 },
        { "connection", 10 },
        { "accept", 6 },
        { "accept-encoding", 15 },
        { "accept-language", 15 },
        { "cache-control", 13 },
        { "vary", 4 },
    };
    size_t i;
    for (i = 0; i < countof(ok); i++)
        if (ok[i].l == len && dyn_ci_eq(n, ok[i].s, len))
            return 1;
    return 0;
}

static int dyn_ci_lit(const char* p, size_t len, const char* lit, size_t ll)
{
    size_t i;
    if (len != ll)
        return 0;
    for (i = 0; i < ll; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != lit[i])
            return 0;
    }
    return 1;
}

static int dyn_scan_head(const char* buf, size_t avail, dyn_reqinfo_t* ri)
{
    const char *base = buf, *end = buf + avail;
    const char *line, *le = NULL;
    const char* reqline_end = NULL;
    const char* seen[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t seen_len[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t nseen = 0;

    memset(ri, 0, sizeof(*ri));
    ri->n_lines = 0;

    line = base;
    for (;;) {
        const char* p = line;
        while (p + 1 < end && !(p[0] == '\r' && p[1] == '\n'))
            p++;
        if (p + 1 >= end) {
            return -1;
        }
        le = p;
        if (!reqline_end) {
            const char* sp1 = (const char*)memchr(line, ' ', (size_t)(le - line));
            const char* sp2 = sp1
                ? (const char*)memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1))
                : NULL;
            if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1) {
                ri->method = line;
                ri->method_len = 0;
                ri->target = le;
                ri->target_len = 0;
                ri->bad_target = 1;
            } else {
                const char* t = sp1 + 1;
                size_t tlen = (size_t)(sp2 - t);
                int in_query = 0;
                ri->method = line;
                ri->method_len = (size_t)(sp1 - line);
                ri->target = t;
                ri->bad_target = 0;
                while (tlen > 0) {
                    unsigned char ch = (unsigned char)*t;
                    if (ch < 0x21 || ch == 0x7f) {
                        ri->bad_target = 1;
                        break;
                    }
                    if (!in_query) {
                        if (ch == '?')
                            in_query = 1;
                        else
                            ri->target_len++;
                    }
                    t++;
                    tlen--;
                }
                if (le - (sp2 + 1) != 8
                    || (memcmp(sp2 + 1, "HTTP/1.1", 8) != 0
                        && memcmp(sp2 + 1, "HTTP/1.0", 8) != 0))
                    ri->bad_target = 1;
                {
                    const char* mc;
                    for (mc = line; mc < sp1; mc++) {
                        unsigned char ch = (unsigned char)*mc;
                        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                                || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
                                || ch == '!' || ch == '#' || ch == '$' || ch == '%'
                                || ch == '&' || ch == '\'' || ch == '*' || ch == '+'
                                || ch == '.' || ch == '^' || ch == '`' || ch == '|'
                                || ch == '~')) {
                            ri->bad_target = 1;
                            break;
                        }
                    }
                }
            }
            ri->http11 = (le - line) >= 8 && memcmp(le - 8, "HTTP/1.1", 8) == 0;
            reqline_end = le;
        } else {
            const char* c = line;
            if (c == le) {
                ri->head_len = (size_t)(le + 2 - base);
                break;
            }
            ri->n_lines++;
            while (c < le && *c != ':' && *c != ' ' && *c != '\t' && (unsigned char)*c >= 0x20 && (unsigned char)*c != 0x7f)
                c++;
            if (c == line)
                return DYN_HEAD_ERR_LINE;
            if (c >= le || *c != ':')
                return DYN_HEAD_ERR_LINE;
            if (c[-1] == ' ' || c[-1] == '\t')
                return DYN_HEAD_ERR_LINE;
            {
                const char* name = line;
                size_t nlen = (size_t)(c - name);
                const char *v = c + 1, *ve = le;
                while (v < ve && (*v == ' ' || *v == '\t'))
                    v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                    ve--;
                {
                    const char* q;
                    for (q = v; q < ve; q++)
                        if (((unsigned char)*q < 0x20 && *q != '\t') || (unsigned char)*q == 0x7f)
                            return DYN_HEAD_ERR_LINE;
                }
                if (!dyn_head_dup_ok(name, nlen)) {
                    size_t i;
                    int dup = 0;
                    for (i = 0; i < nseen; i++)
                        if (seen_len[i] == nlen && dyn_ci_eq(name, seen[i], nlen)) {
                            dup = 1;
                            break;
                        }
                    if (dup)
                        ri->dup = 1;
                    else if (nseen < countof(seen)) {
                        seen[nseen] = name;
                        seen_len[nseen] = nlen;
                        nseen++;
                    }
                }
                if (dyn_ci_lit(name, nlen, "content-length", 14)) {
                    if (!ri->cl_set) {
                        ri->cl = v;
                        ri->cl_len = (size_t)(ve - v);
                        ri->cl_set = 1;
                    }
                    ri->cl_count++;
                } else if (dyn_ci_lit(name, nlen, "transfer-encoding", 17))
                    ri->has_te = 1;
                else if (dyn_ci_lit(name, nlen, "host", 4))
                    ri->host_count++;
                if (!ri->ae && dyn_ci_lit(name, nlen, "accept-encoding", 15)) {
                    ri->ae = v;
                    ri->ae_len = (size_t)(ve - v);
                }
                if (!ri->conn && dyn_ci_lit(name, nlen, "connection", 10)) {
                    ri->conn = v;
                    ri->conn_len = (size_t)(ve - v);
                }
                if (!ri->ct && dyn_ci_lit(name, nlen, "content-type", 12)) {
                    ri->ct = v;
                    ri->ct_len = (size_t)(ve - v);
                }
            }
        }
        line = le + 2;
        if (line >= end) {
            return -1;
        }
    }
    return 0;
}

static void dyn_http_server_stop_internal(dyn_http_server_t* s)
{
    if (!s->started)
        return;
    if (s->async) {
        dyn_http_async_stop_internal(s->async);
        free(s->async);
        s->async = NULL;
    }
    s->started = 0;
}

static void dyn_http_server_dispose(void* native)
{
    dyn_http_server_t* s = (dyn_http_server_t*)native;
    size_t i;

    dyn_http_server_stop_internal(s);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    if (s->routes) {
        for (i = 0; i < s->n_routes; i++) {
            free(s->routes[i].path);
            free(s->routes[i].content_type);
            free(s->routes[i].body);
        }
        free(s->routes);
    }
    free(s);
}

static int dyn_hdr_value_ok(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f)
            return -1;
    }
    return 0;
}

static int dyn_hdr_name_ok(const char* s, size_t n)
{
    size_t i;
    if (!s || n == 0)
        return -1;
    for (i = 0; i < n; i++)
        if (!DYN_TCHAR[(unsigned char)s[i]])
            return -1;
    return 0;
}

#define DYN_HTTP_CT_MAX 256

static int dyn_route_copy(JSContext* ctx, const char* path, JSValueConst val,
    dyn_route_t* r)
{
    const char *body = NULL, *ct = NULL;
    size_t body_len = 0;
    int32_t status = 200;
    int body_owned = 0;

    r->path = NULL;
    r->content_type = NULL;
    r->body = NULL;
    r->body_len = 0;
    r->status = 200;

    {
        const char* p;
        for (p = path; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) {
                JS_ThrowTypeError(ctx,
                    "route path must not contain control characters");
                return -1;
            }
    }
    if (JS_IsString(val)) {
        body = JS_ToCStringLen(ctx, &body_len, val);
        if (!body)
            return -1;
        ct = NULL;
    } else if (JS_IsFunction(ctx, val)) {
        JS_ThrowTypeError(ctx,
            "route value must be a string or {status, contentType, body}; "
            "dyna:http servers serve static routes only -- use App.rpc/App "
            "handlers for dynamic responses");
        return -1;
    } else if (JS_IsObject(val)) {
        if (dyn_opts_strict(ctx, val, http_route_val_keys, 3))
            return -1;
        JSValue vs = JS_GetPropertyStr(ctx, val, "status");
        JSValue vc = JS_GetPropertyStr(ctx, val, "contentType");
        JSValue vb = JS_GetPropertyStr(ctx, val, "body");
        if (!JS_IsUndefined(vs) && !JS_IsNull(vs)) {
            if (JS_ToInt32(ctx, &status, vs)) {
                JS_FreeValue(ctx, vs);
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
        }
        JS_FreeValue(ctx, vs);
        if (status < 100 || status > 599) {
            if (ct)
                JS_FreeCString(ctx, ct);
            if (body)
                JS_FreeCString(ctx, body);
            JS_FreeValue(ctx, vc);
            JS_FreeValue(ctx, vb);
            JS_ThrowRangeError(ctx, "http.route: status must be in [100, 599]");
            return -1;
        }
        if (!JS_IsUndefined(vc) && !JS_IsNull(vc)) {
            size_t ct_len = 0;
            ct = JS_ToCStringLen(ctx, &ct_len, vc);
            if (!ct) {
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
            {
                if (dyn_hdr_value_ok(ct, ct_len) != 0) {
                    JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vc);
                    JS_FreeValue(ctx, vb);
                    JS_ThrowTypeError(ctx,
                        "route contentType must not contain control "
                        "characters");
                    return -1;
                }
                if (ct_len > DYN_HTTP_CT_MAX) {
                    JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vc);
                    JS_FreeValue(ctx, vb);
                    JS_ThrowRangeError(ctx,
                        "route contentType must be at most %d bytes",
                        DYN_HTTP_CT_MAX);
                    return -1;
                }
            }
        }
        JS_FreeValue(ctx, vc);
        if (!JS_IsUndefined(vb) && !JS_IsNull(vb)) {
            if (JS_IsString(vb)) {
                body = JS_ToCStringLen(ctx, &body_len, vb);
                if (!body) {
                    if (ct)
                        JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vb);
                    return -1;
                }
            } else {
                size_t off = 0, total = 0, bpe = 0;
                uint8_t* base;
                uint8_t* nb;
                JSValue ab = JS_GetArrayBufferView(ctx, vb, &off, &body_len,
                    &bpe);
                if (JS_IsException(ab)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    base = JS_GetArrayBuffer(ctx, &total, vb);
                    if (!base) {
                        JS_FreeValue(ctx, vb);
                        if (ct)
                            JS_FreeCString(ctx, ct);
                        JS_ThrowTypeError(ctx,
                            "route body must be a string or a byte view "
                            "(Uint8Array, ArrayBuffer)");
                        return -1;
                    }
                    off = 0;
                    body_len = total;
                } else {
                    base = JS_GetArrayBuffer(ctx, &total, ab);
                    JS_FreeValue(ctx, ab);
                    if (!base) {
                        JS_FreeValue(ctx, vb);
                        if (ct)
                            JS_FreeCString(ctx, ct);
                        return -1;
                    }
                }
                nb = (uint8_t*)malloc(body_len > 0 ? body_len : 1);
                if (!nb) {
                    JS_FreeValue(ctx, vb);
                    if (ct)
                        JS_FreeCString(ctx, ct);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                memcpy(nb, base + off, body_len);
                body = (const char*)nb;
                body_owned = 1;
            }
        }
        JS_FreeValue(ctx, vb);
    } else {
        JS_ThrowTypeError(ctx, "route value must be a string or an object");
        return -1;
    }

    r->status = status;
    r->path = strdup(path);
    r->content_type = strdup(ct ? ct : "text/plain");
    r->body = (char*)malloc(body_len + 1);
    if (!r->path || !r->content_type || !r->body) {
        if (body) {
            if (body_owned)
                free(DYN_UNCONST(body));
            else
                JS_FreeCString(ctx, body);
        }
        if (ct)
            JS_FreeCString(ctx, ct);
        free(r->path);
        free(r->content_type);
        free(r->body);
        r->path = r->content_type = r->body = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (body_len > 0)
        memcpy(r->body, body, body_len);
    r->body[body_len] = '\0';
    r->body_len = body_len;
    if (body) {
        if (body_owned)
            free(DYN_UNCONST(body));
        else
            JS_FreeCString(ctx, body);
    }
    if (ct)
        JS_FreeCString(ctx, ct);
    return 0;
}

static int dyn_http_bind(const char* host, uint16_t* pport, int backlog)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, on = 1;

    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
#ifdef IPV6_V6ONLY
        int v6only = 0;
#endif
        {
            const char* h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf))
                hl = sizeof(hbuf) - 1;
            if (h[0] == '[') {
                h++;
                if (hl > 0)
                    hl--;
                if (hl > 0 && h[hl - 1] == ']')
                    hl--;
            }
            memcpy(hbuf, h, hl);
            hbuf[hl] = '\0';
        }
        fd = dyn_sock_cloexec(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef IPV6_V6ONLY
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(*pport);
        if (inet_pton(AF_INET6, hbuf, &sa6.sin6_addr) != 1 || bind(fd, (struct sockaddr*)&sa6, sizeof(sa6)) < 0 || listen(fd, backlog > 0 ? backlog : SOMAXCONN) < 0) {
            close(fd);
            return -1;
        }
        goto resolved;
    }

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)*pport);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? 0 : AI_PASSIVE;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = dyn_sock_cloexec(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

resolved:
    if (*pport == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr*)&sin, &sl) == 0)
            *pport = ntohs(sin.sin_port);
    }
    return fd;
}

static JSValue dyn_http_server_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char* host_c = NULL;
    char* host_dup = NULL;
    int32_t port = 0, workers = (int32_t)dyn_pool_default_threads(), backlog = 0;
    int32_t req_timeout_ms = DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT;
    JSPropertyEnum* tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        if (dyn_opts_strict(ctx, opts, http_srv_keys, 6)) {
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &workers, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "requestTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &req_timeout_ms, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            host_c = JS_ToCString(ctx, v);
            if (!host_c) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(host_c);
            JS_FreeCString(ctx, host_c);
        }
        JS_FreeValue(ctx, v);

        routes_val = JS_GetPropertyStr(ctx, opts, "routes");
        if (JS_IsException(routes_val)) {
            free(host_dup);
            return JS_EXCEPTION;
        }
    }

    if (port < 0 || port > 65535) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowRangeError(ctx, "port must be in [0, 65535]");
    }
    if (workers < 1)
        workers = 1;
    if (workers > 64)
        workers = 64;
    if (backlog <= 0)
        backlog = SOMAXCONN;
    if (req_timeout_ms < 0)
        req_timeout_ms = 0;

    s = (dyn_http_server_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->num_workers = workers;
    s->backlog = backlog;
    s->req_timeout_ms = req_timeout_ms;
    s->ctx = ctx;

    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t*)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char* path = JS_AtomToCString(ctx, tab[i].atom);
            JSValue rv;
            if (!path)
                goto fail_enum;
            rv = JS_GetProperty(ctx, routes_val, tab[i].atom);
            if (JS_IsException(rv)) {
                JS_FreeCString(ctx, path);
                goto fail_enum;
            }
            if (dyn_route_copy(ctx, path, rv, &s->routes[s->n_routes]) < 0) {
                JS_FreeCString(ctx, path);
                JS_FreeValue(ctx, rv);
                goto fail_enum;
            }
            s->n_routes++;
            JS_FreeCString(ctx, path);
            JS_FreeValue(ctx, rv);
        }
        JS_FreePropertyEnum(ctx, tab, n_routes);
        tab = NULL;
    }
    JS_FreeValue(ctx, routes_val);
    routes_val = JS_UNDEFINED;

    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind(host_dup, &bound_port, backlog);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        dyn_http_server_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, new_target, dyn_http_server_class_id, s,
        dyn_http_server_dispose);

oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_server_dispose(s);
    return JS_EXCEPTION;
}

static JSValue dyn_http_server_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    {
        dyn_http_async_t* a = (dyn_http_async_t*)calloc(1, sizeof(*a));
        if (!a)
            return JS_ThrowOutOfMemory(ctx);
        a->ctx = s->ctx;
        a->listen_fd = s->listen_fd;
        a->routes = s->routes;
        a->n_routes = s->n_routes;
        a->borrowed = 1;
        a->max_req = DYN_HTTP_SERVER_FRAME_MAX;
        a->idle_ms = s->req_timeout_ms
            ? (uint64_t)(s->req_timeout_ms < DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT
                      ? s->req_timeout_ms
                      : DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT)
            : 0;
        a->max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
        atomic_init(&a->stop_flag, 0);
        atomic_init(&a->spawn_ok, 0);
        atomic_init(&a->n_refused, 0);
        dyn_set_nonblock(s->listen_fd);
        if (dyn_http_async_spawn(a) < 0) {
            free(a);
            return JS_ThrowInternalError(
                ctx, "HTTPServer.start: failed to start the reactor thread");
        }
        if (!dyn_net_reactor_acquire(ctx)) {
            dyn_http_async_stop_internal(a);
            free(a);
            return JS_ThrowOutOfMemory(ctx);
        }
        a->reactor_held = 1;
        s->async = a;
    }
    s->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_http_server_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_server_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_server_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_http_server_t* s = (dyn_http_server_t*)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_http_server_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_server_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_server_stop),
    JS_CGETSET_DEF("port", dyn_http_server_get_port, NULL),
};

static const JSClassDef dyn_http_server_class = {
    "HTTPServer",
    .finalizer = dyn_res_finalizer,
};

static JSClassID dyn_http_async_class_id;

#define DYN_ACONN_MAX_REQ (1 * 1024 * 1024)
#define DYN_ACONN_MAX_REQS 100000
#define DYN_WS_MAX_FRAGMENTS 4096
#define DYN_WS_CTL_BUDGET 64
#define DYN_HTTP_OUTBOUND_MAX (4 << 20)
#define DYN_APP_MAX_BATCH 256
#define DYN_APP_MAX_PARAMS 256

static inline size_t dyn_put_u64(char* p, uint64_t v)
{
    char tmp[20];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++)
        p[i] = tmp[n - 1 - i];
    return (size_t)n;
}

static int dyn_http_format(dyn_bytes_t* out, int status, const char* ct,
    const char* body, size_t body_len, int keep_alive, int head_only)
{
    char head[512];
    int n;
    if ((status >= 100 && status < 200) || status == 204 || status == 304) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\nConnection: %s\r\n\r\n",
            status, dyn_reason_phrase(status),
            keep_alive ? "keep-alive" : "close");
        if (n < 0 || n >= (int)sizeof(head))
            return -1;
        if (dyn_bytes_append(out, head, (size_t)n) < 0)
            return -1;
        return 0;
    }
    if (status == 200 && keep_alive && ct && strcmp(ct, "text/plain") == 0) {
        static const char pre[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
        static const char post[] = "\r\nConnection: keep-alive\r\n\r\n";
        size_t k = sizeof(pre) - 1;
        memcpy(head, pre, k);
        k += (size_t)dyn_put_u64(head + k, (uint64_t)body_len);
        memcpy(head + k, post, sizeof(post) - 1);
        n = (int)(k + sizeof(post) - 1);
    } else {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            status, dyn_reason_phrase(status), ct ? ct : "text/plain",
            body_len, keep_alive ? "keep-alive" : "close");
        if (n < 0 || n >= (int)sizeof(head))
            return -1;
    }
    if (dyn_bytes_append(out, head, (size_t)n) < 0)
        return -1;
    if (!head_only && body_len > 0 && dyn_bytes_append(out, body, body_len) < 0)
        return -1;
    return 0;
}

static void dyn_aconn_free(dyn_aconn_t* c)
{
    free(c->in.data);
    free(c->out.data);
    free(c);
}

static void dyn_aconn_link(dyn_http_async_t* s, dyn_aconn_t* c)
{
    c->lnext = s->live;
    if (s->live)
        s->live->lprev = c;
    s->live = c;
    s->nconns++;
}

static void dyn_aconn_unlink(dyn_http_async_t* s, dyn_aconn_t* c)
{
    if (c->lprev)
        c->lprev->lnext = c->lnext;
    else if (s->live == c)
        s->live = c->lnext;
    else
        return;
    if (c->lnext)
        c->lnext->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
    if (s->nconns > 0)
        s->nconns--;
}

static void dyn_aconn_close(dyn_evloop_t* lp, dyn_aconn_t* c)
{
    dyn_aconn_unlink(c->srv, c);
    dyn_evloop_del(lp, c->fd);
    close(c->fd);
    dyn_aconn_free(c);
}

static void dyn_aconn_sweep(dyn_evloop_t* lp, dyn_http_async_t* s, uint64_t now)
{
    dyn_aconn_t *c = s->live, *next;
    while (c) {
        next = c->lnext;
        if (now - c->last_ms >= s->idle_ms)
            dyn_aconn_close(lp, c);
        c = next;
    }
}

static int dyn_http_pump(dyn_bytes_t* in, dyn_bytes_t* out, int* pnreq,
    size_t* pscan,
    const dyn_route_t* routes, size_t n_routes,
    size_t max_req)
{
    char path[2048];

    for (;;) {
        const char* base = in->data;
        size_t avail = in->len;
        size_t from = *pscan < avail ? *pscan : avail;
        const char* hdr_end;
        size_t head_len, body_len = 0, req_total, clv_len = 0, connv_len = 0;
        const char *cl, *conn;
        int http11, keep_alive, head_only = 0;
        const dyn_route_t* route;
        dyn_reqinfo_t ri;

        if (out->len > DYN_HTTP_OUTBOUND_MAX)
            return 0;

        hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        if (!hdr_end) {
            *pscan = avail >= 3 ? avail - 3 : 0;
            if (avail > max_req) {
                dyn_http_format(out, 431, "text/plain",
                    "Request Header Fields Too Large", 31, 0, head_only);
                return 1;
            }
            return 0;
        }
        head_len = (size_t)(hdr_end - base) + 4;

        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src != 0 || ri.head_len == 0 || ri.head_len > (size_t)(hdr_end - base) + 4) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
        }
        head_len = ri.head_len;
        http11 = ri.http11;
        head_only = ri.method_len == 4 && memcmp(ri.method, "HEAD", 4) == 0;

        if (head_len > max_req) {
            dyn_http_format(out, 431, "text/plain",
                "Request Header Fields Too Large", 31, 0, head_only);
            return 1;
        }

        if (ri.has_te) {
            dyn_http_format(out, 501, "text/plain", "Not Implemented", 15, 0, head_only);
            return 1;
        }
        {
            if (ri.cl_count > 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
            if (http11 && ri.host_count != 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
        }
        cl = ri.cl;
        clv_len = ri.cl_len;
        if (cl) {
            size_t j;
            if (clv_len == 0 || clv_len > 19) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                return 1;
            }
            for (j = 0; j < clv_len; j++) {
                if (cl[j] < '0' || cl[j] > '9') {
                    dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
                    return 1;
                }
                if (body_len > (SIZE_MAX - 9) / 10) {
                    dyn_http_format(out, 413, "text/plain", "Content Too Large", 17, 0, head_only);
                    return 1;
                }
                body_len = body_len * 10 + (size_t)(cl[j] - '0');
            }
        }
        req_total = head_len + body_len;
        if (req_total > max_req) {
            dyn_http_format(out, 413, "text/plain", "Content Too Large", 17, 0, head_only);
            return 1;
        }
        if (avail < req_total)
            return 0;

        conn = ri.conn;
        connv_len = ri.conn_len;
        keep_alive = http11;
        if (dyn_hdr_token(conn, connv_len, "close"))
            keep_alive = 0;
        else if (dyn_hdr_token(conn, connv_len, "keep-alive"))
            keep_alive = 1;
        if (++(*pnreq) >= DYN_ACONN_MAX_REQS)
            keep_alive = 0;

        if (ri.bad_target) {
            dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0, head_only);
            return 1;
        }
        if (ri.target_len >= sizeof(path)) {
            dyn_http_format(out, 414, "text/plain", "URI Too Long", 12, 0, head_only);
            return 1;
        }
        {
            size_t plen = ri.target_len;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
        }
        route = dyn_route_lookup(routes, n_routes, path);
        if (route) {
            if (dyn_http_format(out, route->status, route->content_type,
                    route->body, route->body_len, keep_alive, head_only)
                < 0)
                return 1;
        } else {
            if (dyn_http_format(out, 404, "text/plain", "Not Found", 9,
                    keep_alive, head_only)
                < 0)
                return 1;
        }

        memmove(in->data, in->data + req_total, in->len - req_total);
        in->len -= req_total;
        *pscan = 0;
        if (!keep_alive)
            return 1;
    }
}

static int dyn_aconn_process(dyn_aconn_t* c)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
        &c->hdr_scan_from,
        c->srv->routes, c->srv->n_routes,
        c->srv->max_req);
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

static int dyn_aconn_flush(dyn_evloop_t* lp, dyn_aconn_t* c)
{
    for (;;) {
        while (c->out_off < c->out.len) {
            ssize_t w = send(c->fd, c->out.data + c->out_off,
                c->out.len - c->out_off, 0);
            if (w > 0) {
                c->out_off += (size_t)w;
                continue;
            }
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                dyn_evloop_mod(lp, c->fd, DYN_EV_WRITE);
                return 0;
            }
            dyn_aconn_close(lp, c);
            return 1;
        }
        c->out.len = 0;
        c->out_off = 0;
        if (c->closing) {
            dyn_aconn_close(lp, c);
            return 1;
        }
        if (c->in.len > 0) {
            size_t before = c->in.len;
            if (dyn_aconn_process(c)) {
                c->closing = 1;
                continue;
            }
            if (c->in.len == before && c->out.len == 0)
                break;
            continue;
        }
        break;
    }
    dyn_evloop_mod(lp, c->fd, DYN_EV_READ);
    return 0;
}

static void dyn_aconn_cb(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_aconn_t* c = (dyn_aconn_t*)udata;
    (void)fd;

    if (events & DYN_EV_WRITE) {
        if (dyn_aconn_flush(lp, c))
            return;
    }
    if (events & DYN_EV_READ) {
        for (;;) {
            ssize_t r;
            if (dyn_bytes_reserve(&c->in, 16384) < 0) {
                dyn_aconn_close(lp, c);
                return;
            }
            r = recv(fd, c->in.data + c->in.len, c->in.cap - c->in.len, 0);
            if (r > 0) {
                c->in.len += (size_t)r;
                if (c->in.len > c->srv->max_req + 16384) {
                    dyn_aconn_close(lp, c);
                    return;
                }
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            c->closing = 1;
            break;
        }
        if (dyn_aconn_process(c))
            c->closing = 1;
        if (c->out.len > c->out_off) {
            if (dyn_aconn_flush(lp, c))
                return;
        } else if (c->closing) {
            dyn_aconn_close(lp, c);
            return;
        }
    }
    if (events & DYN_EV_ERROR) {
        if (c->out_off >= c->out.len)
            dyn_aconn_close(lp, c);
    }
}

static void dyn_alisten_cb(dyn_evloop_t* lp, int fd, int events, void* udata)
{
    dyn_http_async_t* s = (dyn_http_async_t*)udata;
    (void)events;
    for (;;) {
        dyn_aconn_t* c;
        int cfd = dyn_accept_cloexec(fd);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || ++s->accept_errs >= 32) {
                dyn_evloop_del(lp, fd);
                s->accept_paused = 1;
                s->accept_resume_ms = dyn_timer_now_ms() + 50;
                s->accept_errs = 0;
            }
            break;
        }
        if (s->max_conns && s->nconns >= s->max_conns) {
            atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
            close(cfd);
            continue;
        }
        s->accept_errs = 0;
        if (dyn_set_nonblock(cfd) < 0) {
            close(cfd);
            continue;
        }
        dyn_set_nodelay(cfd);
        c = (dyn_aconn_t*)calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        c->srv = s;
        c->fd = cfd;
        c->last_ms = dyn_timer_now_ms();
        if (dyn_evloop_add(lp, cfd, DYN_EV_READ, dyn_aconn_cb, c) < 0) {
            close(cfd);
            dyn_aconn_free(c);
            continue;
        }
        dyn_aconn_link(s, c);
    }
}

#ifdef DYN_HTTP_HAVE_URING
#define DYN_URING_ENTRIES 8192
#define DYN_URING_NBUFS 4096
#define DYN_URING_BUFSZ 2048
#define DYN_URING_BGID 1
#define DYN_URING_ACCEPT_UD 0ULL

enum { DYN_OP_ACCEPT = 0,
    DYN_OP_RECV = 1,
    DYN_OP_SEND = 2,
    DYN_OP_CLOSE = 3 };
#define DYN_UD_TAG(ud) ((int)((ud) & 7))
#define DYN_UD_CONN(ud) ((dyn_uconn_t*)(uintptr_t)((ud) & ~(uint64_t)7))

typedef struct dyn_uconn {
    int fd;
    dyn_bytes_t in;
    dyn_bytes_t out;
    size_t out_off;
    size_t hdr_scan_from;
    int nreq;
    unsigned recv_armed : 1;
    unsigned send_inflight : 1;
    unsigned closing : 1;
    uint64_t last_ms;
    struct dyn_uconn *next, *prev;
} dyn_uconn_t;

typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring* br;
    unsigned char* buf_base;
    int nbufs, bufsz;
    int accept_errs;
    int accept_needs_arm;
    uint64_t accept_resume_ms;
    int nconns;
    dyn_uconn_t* live;
} dyn_uring_ctx;

static struct io_uring_sqe* dyn_ur_sqe(dyn_uring_ctx* u)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&u->ring);
    if (!sqe) {
        io_uring_submit(&u->ring);
        sqe = io_uring_get_sqe(&u->ring);
    }
    if (!sqe) {
        io_uring_submit_and_wait(&u->ring, 1);
        sqe = io_uring_get_sqe(&u->ring);
    }
    return sqe;
}

static int dyn_ur_arm_accept(dyn_uring_ctx* u, int listen_fd)
{
    struct io_uring_sqe* sqe = dyn_ur_sqe(u);
    if (!sqe)
        return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, DYN_URING_ACCEPT_UD);
    return 0;
}

static void dyn_ur_arm_recv(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    struct io_uring_sqe* sqe;
    if (c->fd < 0) {
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        c->closing = 1;
        return;
    }
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = DYN_URING_BGID;
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_RECV);
    c->recv_armed = 1;
}

static void dyn_ur_submit_send(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    struct io_uring_sqe* sqe;
    if (c->fd < 0) {
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        c->closing = 1;
        return;
    }
    io_uring_prep_send(sqe, c->fd, c->out.data + c->out_off,
        c->out.len - c->out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_SEND);
    c->send_inflight = 1;
}

static void dyn_ur_recycle(dyn_uring_ctx* u, int bid)
{
    io_uring_buf_ring_add(u->br, u->buf_base + (size_t)bid * u->bufsz, u->bufsz,
        bid, io_uring_buf_ring_mask(u->nbufs), 0);
    io_uring_buf_ring_advance(u->br, 1);
}

static dyn_uconn_t* dyn_uconn_new(dyn_uring_ctx* u, int fd)
{
    dyn_uconn_t* c = (dyn_uconn_t*)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->last_ms = dyn_timer_now_ms();
    c->next = u->live;
    if (u->live)
        u->live->prev = c;
    u->live = c;
    u->nconns++;
    return c;
}

static void dyn_uconn_destroy(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        u->live = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (u->nconns > 0)
        u->nconns--;
    if (c->fd >= 0)
        close(c->fd);
    free(c->in.data);
    free(c->out.data);
    free(c);
}

static void dyn_uconn_close_step(dyn_uring_ctx* u, dyn_uconn_t* c)
{
    if (!c->closing || c->send_inflight)
        return;
    if (c->recv_armed) {
        if (c->fd >= 0) {
            struct io_uring_sqe* sqe = dyn_ur_sqe(u);
            if (sqe) {
                io_uring_prep_cancel_fd(sqe, c->fd, IORING_ASYNC_CANCEL_ALL);
                io_uring_sqe_set_data64(sqe, (uint64_t)DYN_OP_CLOSE);
            }
            io_uring_submit(&u->ring);
            close(c->fd);
            c->fd = -1;
        }
        return;
    }
    dyn_uconn_destroy(u, c);
}

static void dyn_ur_on_accept(dyn_uring_ctx* u, dyn_http_async_t* s,
    struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE) && res >= 0) {
        if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
            u->accept_needs_arm = 1;
            return;
        }
    }
    if (res < 0) {
        if (++u->accept_errs >= 32) {
            u->accept_resume_ms = dyn_timer_now_ms() + 50;
            u->accept_needs_arm = 1;
            u->accept_errs = 0;
        }
        return;
    }
    u->accept_errs = 0;
    if (s->max_conns && u->nconns >= s->max_conns) {
        atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
        close(res);
        return;
    }
    {
        dyn_uconn_t* c = dyn_uconn_new(u, res);
        if (!c) {
            close(res);
            return;
        }
        (void)fcntl(res, F_SETFD, FD_CLOEXEC);
        dyn_set_nodelay(res);
        dyn_ur_arm_recv(u, c);
    }
}

static int dyn_ur_pump(dyn_uconn_t* c, dyn_http_async_t* s)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
        &c->hdr_scan_from,
        s->routes, s->n_routes, s->max_req);
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

static void dyn_ur_on_recv(dyn_uring_ctx* u, dyn_http_async_t* s,
    dyn_uconn_t* c, struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    int bid = (cqe->flags & IORING_CQE_F_BUFFER)
        ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)
        : -1;

    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->recv_armed = 0;

    if (res > 0) {
        int oom = 0;
        if (bid >= 0) {
            if (dyn_bytes_append(&c->in,
                    (char*)(u->buf_base + (size_t)bid * u->bufsz),
                    (size_t)res)
                < 0)
                oom = 1;
            dyn_ur_recycle(u, bid);
        }
        if (oom || c->in.len > DYN_ACONN_MAX_REQ + 16384) {
            c->closing = 1;
        } else if (!c->send_inflight) {
            if (dyn_ur_pump(c, s))
                c->closing = 1;
            if (c->out.len > c->out_off)
                dyn_ur_submit_send(u, c);
        }
        if (!c->closing && !c->recv_armed)
            dyn_ur_arm_recv(u, c);
    } else {
        if (bid >= 0)
            dyn_ur_recycle(u, bid);
        if (res == -ENOBUFS) {
            if (!c->closing && !c->recv_armed)
                dyn_ur_arm_recv(u, c);
        } else {
            if (!c->send_inflight) {
                if (dyn_ur_pump(c, s))
                    c->closing = 1;
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
            }
            c->closing = 1;
        }
    }
    dyn_uconn_close_step(u, c);
}

static void dyn_ur_on_send(dyn_uring_ctx* u, dyn_http_async_t* s,
    dyn_uconn_t* c, struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    c->send_inflight = 0;
    if (res > 0) {
        c->out_off += (size_t)res;
        if (c->out_off < c->out.len) {
            dyn_ur_submit_send(u, c);
        } else {
            c->out.len = 0;
            c->out_off = 0;
            if (!c->closing) {
                if (dyn_ur_pump(c, s))
                    c->closing = 1;
                if (c->out.len > c->out_off)
                    dyn_ur_submit_send(u, c);
                else if (!c->recv_armed && !c->closing)
                    dyn_ur_arm_recv(u, c);
            }
        }
    } else {
        c->closing = 1;
    }
    dyn_uconn_close_step(u, c);
}

static void dyn_ur_sweep(dyn_uring_ctx* u, dyn_http_async_t* s, uint64_t now)
{
    dyn_uconn_t *c = u->live, *next;
    while (c) {
        next = c->next;
        if (now - c->last_ms >= s->idle_ms) {
            c->closing = 1;
            dyn_uconn_close_step(u, c);
        }
        c = next;
    }
}

static int dyn_http_uring_try_run(dyn_http_async_t* s)
{
    dyn_uring_ctx* u = (dyn_uring_ctx*)calloc(1, sizeof(*u));
    struct io_uring_params p;
    int ret = 0, i;

    if (!u)
        return 0;
    u->nbufs = DYN_URING_NBUFS;
    u->bufsz = DYN_URING_BUFSZ;

    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
        memset(&p, 0, sizeof(p));
        p.flags = IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
            memset(&p, 0, sizeof(p));
            if (io_uring_queue_init_params(DYN_URING_ENTRIES, &u->ring, &p) < 0) {
                free(u);
                return 0;
            }
        }
    }

    u->buf_base = (unsigned char*)malloc((size_t)u->nbufs * u->bufsz);
    if (!u->buf_base) {
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    u->br = io_uring_setup_buf_ring(&u->ring, u->nbufs, DYN_URING_BGID, 0, &ret);
    if (!u->br) {
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    for (i = 0; i < u->nbufs; i++)
        io_uring_buf_ring_add(u->br, u->buf_base + (size_t)i * u->bufsz, u->bufsz,
            i, io_uring_buf_ring_mask(u->nbufs), i);
    io_uring_buf_ring_advance(u->br, u->nbufs);

    if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
        io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
        free(u->buf_base);
        io_uring_queue_exit(&u->ring);
        free(u);
        return 0;
    }
    io_uring_submit(&u->ring);

    s->uring = u;
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
        struct io_uring_cqe* cqe;
        unsigned head, count = 0;

        {
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_ur_sweep(u, s, now);
            }
            if (u->accept_needs_arm && now >= u->accept_resume_ms
                && dyn_ur_arm_accept(u, s->listen_fd) == 0)
                u->accept_needs_arm = 0;
        }
        ret = io_uring_submit_and_wait_timeout(&u->ring, &cqe, 1, &ts, NULL);
        if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -ETIMEDOUT)
            break;
        io_uring_for_each_cqe(&u->ring, head, cqe)
        {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            count++;
            if (ud == DYN_URING_ACCEPT_UD) {
                dyn_ur_on_accept(u, s, cqe);
            } else if (DYN_UD_TAG(ud) == DYN_OP_CLOSE) {
            } else {
                dyn_uconn_t* c = DYN_UD_CONN(ud);
                if (DYN_UD_TAG(ud) == DYN_OP_RECV)
                    dyn_ur_on_recv(u, s, c, cqe);
                else if (DYN_UD_TAG(ud) == DYN_OP_SEND)
                    dyn_ur_on_send(u, s, c, cqe);
            }
        }
        io_uring_cq_advance(&u->ring, count);
    }

    while (u->live)
        dyn_uconn_destroy(u, u->live);
    io_uring_free_buf_ring(&u->ring, u->br, u->nbufs, DYN_URING_BGID);
    free(u->buf_base);
    io_uring_queue_exit(&u->ring);
    free(u);
    s->uring = NULL;
    return 1;
}
#endif

static void* dyn_http_async_reactor(void* arg)
{
    dyn_http_async_t* s = (dyn_http_async_t*)arg;

#ifdef DYN_HTTP_HAVE_URING
    if (dyn_http_uring_try_run(s))
        return NULL;
#endif

    s->loop = dyn_evloop_new();
    if (!s->loop || dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ, dyn_alisten_cb, s) < 0) {
        atomic_store_explicit(&s->spawn_ok, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        {
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_aconn_sweep(s->loop, s, now);
            }
            if (s->accept_paused && now >= s->accept_resume_ms) {
                s->accept_paused = 0;
                s->accept_errs = 0;
                dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ,
                    dyn_alisten_cb, s);
            }
        }
        if (dyn_evloop_poll(s->loop, 200) < 0)
            break;
    }
    return NULL;
}

static void dyn_http_async_stop_internal(dyn_http_async_t* s)
{
    if (!s->started)
        return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    pthread_join(s->reactor, NULL);
    if (s->loop) {
        dyn_evloop_free(s->loop);
        s->loop = NULL;
    }
    if (s->reactor_held) {
        dyn_net_reactor_release(s->ctx);
        s->reactor_held = 0;
    }
    s->started = 0;
}

static void dyn_http_async_dispose(void* native)
{
    dyn_http_async_t* s = (dyn_http_async_t*)native;
    size_t i;

    dyn_http_async_stop_internal(s);
    if (!s->borrowed) {
        if (s->listen_fd >= 0)
            close(s->listen_fd);
        if (s->routes) {
            for (i = 0; i < s->n_routes; i++) {
                free(s->routes[i].path);
                free(s->routes[i].content_type);
                free(s->routes[i].body);
            }
            free(s->routes);
        }
    }
    free(s);
}

static JSValue dyn_http_async_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char* host_c = NULL;
    char* host_dup = NULL;
    int32_t port = 0, backlog = 0, max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
    int64_t idle_ms = DYN_ACONN_IDLE_MS_DEFAULT;
    JSPropertyEnum* tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        if (dyn_opts_strict(ctx, opts, http_async_keys, 6))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            host_c = JS_ToCString(ctx, v);
            if (!host_c) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(host_c);
            JS_FreeCString(ctx, host_c);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &idle_ms, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (idle_ms < 0)
            idle_ms = 0;
        v = JS_GetPropertyStr(ctx, opts, "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &max_conns, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (max_conns < 0)
            max_conns = 0;
        routes_val = JS_GetPropertyStr(ctx, opts, "routes");
        if (JS_IsException(routes_val)) {
            free(host_dup);
            return JS_EXCEPTION;
        }
    }

    if (port < 0 || port > 65535) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowRangeError(ctx, "port must be in [0, 65535]");
    }
    if (backlog <= 0)
        backlog = SOMAXCONN;

    s = (dyn_http_async_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(host_dup);
        JS_FreeValue(ctx, routes_val);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->listen_fd = -1;
    s->backlog = backlog;
    s->ctx = ctx;
    atomic_init(&s->stop_flag, 0);
    atomic_init(&s->spawn_ok, 0);
    atomic_init(&s->n_refused, 0);
    s->idle_ms = (uint64_t)idle_ms;
    s->max_conns = max_conns;
    s->max_req = DYN_ACONN_MAX_REQ;

    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t*)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char* path = JS_AtomToCString(ctx, tab[i].atom);
            JSValue rv;
            if (!path)
                goto fail_enum;
            rv = JS_GetProperty(ctx, routes_val, tab[i].atom);
            if (JS_IsException(rv)) {
                JS_FreeCString(ctx, path);
                goto fail_enum;
            }
            if (dyn_route_copy(ctx, path, rv, &s->routes[s->n_routes]) < 0) {
                JS_FreeCString(ctx, path);
                JS_FreeValue(ctx, rv);
                goto fail_enum;
            }
            s->n_routes++;
            JS_FreeCString(ctx, path);
            JS_FreeValue(ctx, rv);
        }
        JS_FreePropertyEnum(ctx, tab, n_routes);
        tab = NULL;
    }
    JS_FreeValue(ctx, routes_val);
    routes_val = JS_UNDEFINED;

    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind(host_dup, &bound_port, backlog);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    if (dyn_set_nonblock(listen_fd) < 0) {
        close(listen_fd);
        dyn_http_async_dispose(s);
        return dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "bind", NULL, NULL);
    }
    s->listen_fd = listen_fd;
    s->port = bound_port;

    return dyn_res_wrap(ctx, new_target, dyn_http_async_class_id, s,
        dyn_http_async_dispose);

oom_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_ThrowOutOfMemory(ctx);

fail_enum:
    JS_FreePropertyEnum(ctx, tab, n_routes);
    tab = NULL;
fail_pending:
    free(host_dup);
    JS_FreeValue(ctx, routes_val);
    dyn_http_async_dispose(s);
    return JS_EXCEPTION;
}

static int dyn_http_async_spawn(dyn_http_async_t* s)
{
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    atomic_store_explicit(&s->spawn_ok, 0, memory_order_relaxed);
    if (pthread_create(&s->reactor, NULL, dyn_http_async_reactor, s) != 0)
        return -1;
    for (;;) {
        int ok = atomic_load_explicit(&s->spawn_ok, memory_order_acquire);
        if (ok != 0) {
            if (ok < 0) {
                pthread_join(s->reactor, NULL);
                return -1;
            }
            break;
        }
        sched_yield();
    }
    s->started = 1;
    return 0;
}

static JSValue dyn_http_async_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    if (dyn_http_async_spawn(s) < 0)
        return JS_ThrowInternalError(ctx, "failed to start reactor thread");
    if (!dyn_net_reactor_acquire(ctx)) {
        dyn_http_async_stop_internal(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->reactor_held = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_async_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_http_async_t* s = (dyn_http_async_t*)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, s->port);
}

static const JSCFunctionListEntry dyn_http_async_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_async_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_async_stop),
    JS_CGETSET_DEF("port", dyn_http_async_get_port, NULL),
};

static const JSClassDef dyn_http_async_class = {
    "HTTPServerAsync",
    .finalizer = dyn_res_finalizer,
};

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
    char* up_host;
    char* dyn_method;
    char* dyn_pattern;
    int64_t max_file;
    size_t n_allow;
    int type;
    uint16_t up_port;
} dyn_app_route_t;

_Static_assert(sizeof(dyn_app_route_t) <= 96,
    "dyn_app_route_t regained padding: reorder largest-first");

typedef struct dyn_app {
    JSContext* ctx;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    dyn_app_route_t* routes;
    size_t n_routes;
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
    char* listen_host;
    int backlog;
    int workers;
    size_t out_bytes;
    int n_sse;
    JSValue* mw;
    size_t n_mw, cap_mw;
} dyn_app_t;

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
    int out_stalled;
    uint64_t stall_ms;
    uint64_t up_win_ms;
    int64_t up_win_bytes;
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

static void dyn_app_conn_unlink(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    if (c->lprev)
        ((dyn_app_conn_t*)c->lprev)->lnext = c->lnext;
    else if (app->conns == c)
        app->conns = c->lnext;
    else
        return;
    if (c->lnext)
        ((dyn_app_conn_t*)c->lnext)->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
}

static void dyn_app_conn_unref(dyn_app_conn_t* c)
{
    if (--c->refs == 0) {
        dyn_app_t* app = c->app;
        dyn_app_conn_unlink(c);
        if (c->up) {
            close(c->up->fd);
            unlink(c->up->path);
            free(c->up->path);
            JS_FreeValue(app->ctx, c->up->handler);
            free(c->up);
        }
        JS_FreeValue(app->ctx, c->ws_handlers);
        JS_FreeValue(app->ctx, c->ws_this);
        JS_FreeValue(app->ctx, c->sse_handlers);
        JS_FreeValue(app->ctx, c->sse_this);
        dyn_iobuf_free(&c->ws_frag);
        dyn_iobuf_free(&c->in);
        app->out_bytes -= c->out_q;
        if (c->is_sse && app->n_sse > 0)
            app->n_sse--;
        free(c);
        if (--app->n_conns == 0 && app->dispose_called && !app->in_sweep) {
            if (app->timers_orphan) {
                dyn_timers_free(app->timers_orphan);
                app->timers_orphan = NULL;
            }
            dyn_net_off_drain(app);
            free(app);
        }
    }
}

static void dyn_app_conn_close(dyn_app_conn_t* c);
static void dyn_app_send_err(dyn_app_conn_t* c, int status, const char* msg);
static void dyn_app_pump_resume(dyn_app_conn_t* c);

static void dyn_app_out_account(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    size_t q = dyn_aio_queued(app->aio, c->fd);
    app->out_bytes += q;
    app->out_bytes -= c->out_q;
    c->out_q = q;
}

static int dyn_app_out_blocked(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;

    dyn_app_out_account(c);
    if (c->out_q < DYN_APP_OUT_HIGH && app->out_bytes < DYN_APP_OUT_TOTAL)
        return 0;
    if (!c->out_stalled) {
        c->out_stalled = 1;
        c->stall_ms = dyn_timer_now_ms();
        c->stall_q = c->out_q;
    }
    return 1;
}

static void dyn_app_out_unstall(dyn_app_conn_t* c)
{
    c->out_stalled = 0;
    c->stall_ms = 0;
    c->stall_q = 0;
}

static int dyn_app_out_ready(dyn_app_conn_t* c)
{
    return c->out_q < DYN_APP_OUT_LOW && c->app->out_bytes < DYN_APP_OUT_TOTAL;
}

static void dyn_app_out_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    (void)buf;
    (void)len;
    dyn_app_out_account(c);
    if (res < 0)
        dyn_app_conn_close(c);
    else if (c->out_stalled && !c->closed && dyn_app_out_ready(c)) {
        dyn_app_out_unstall(c);
        dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

static void dyn_app_out_last(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_out_account(c);
    if (res < 0 || c->close_after)
        dyn_app_conn_close(c);
    else if (c->out_stalled && !c->closed && dyn_app_out_ready(c)) {
        dyn_app_out_unstall(c);
        dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

static void dyn_app_met_response(dyn_app_conn_t* c, double bytes)
{
    dyn_metrics_c_record(DYN_MET_COUNTER, "dyn_http_responses_total", 1);
    dyn_metrics_c_record(DYN_MET_COUNTER, "dyn_http_response_bytes_total",
        bytes);
    if (c->req_start_ms) {
        dyn_metrics_c_record(
            DYN_MET_HISTOGRAM, "dyn_http_request_duration_seconds",
            (double)(dyn_timer_now_ms() - c->req_start_ms) / 1000.0);
        c->req_start_ms = 0;
    }
}

static JSValue dyn_app_own_get(JSContext* ctx, JSValueConst obj, const char* key)
{
    JSAtom a = JS_NewAtom(ctx, key);
    JSPropertyDescriptor d;
    JSValue v;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &d, obj, a);
    if (r != 0) {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    if (r < 0) {
        JS_FreeAtom(ctx, a);
        return JS_EXCEPTION;
    }
    if (r == 0) {
        JS_FreeAtom(ctx, a);
        return JS_UNDEFINED;
    }
    v = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return v;
}

struct dyn_ws {
    dyn_app_conn_t* conn;
};
static JSClassID dyn_ws_class_id;

static void dws_accept(const char* key, size_t klen, char out[32])
{
    uint8_t buf[64], d[20];
    size_t kn = klen < 24 ? klen : 24, n;

    memcpy(buf, key, kn);
    memcpy(buf + kn, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    dyn_sha1(buf, kn + 36, d);
    n = dyn_codec_base64_encode(d, sizeof(d), out);
    out[n] = '\0';
}

static void dyn_ws_send_frame(dyn_app_conn_t* c, int opcode, const uint8_t* data,
    size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10];
    size_t hn;
    int i;
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        h[1] = (uint8_t)len;
        hn = 2;
    } else if (len <= 0xffff) {
        h[1] = 126;
        h[2] = (len >> 8) & 0xff;
        h[3] = len & 0xff;
        hn = 4;
    } else {
        h[1] = 127;
        for (i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
        hn = 10;
    }
    dyn_iobuf_append(&f, h, hn);
    if (len)
        dyn_iobuf_append(&f, data, len);
    if (dyn_aio_queued(c->app->aio, c->fd) + f.len > DYN_HTTP_OUTBOUND_MAX) {
        dyn_iobuf_free(&f);
        dyn_app_conn_close(c);
        return;
    }
    dyn_aio_send(c->app->aio, c->fd, f.data, f.len, 0, NULL, NULL);
    dyn_iobuf_free(&f);
}

static void dyn_ws_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(val, dyn_ws_class_id);
    (void)rt;
    free(w);
}
static const JSClassDef dyn_ws_class = { "WsConn", .finalizer = dyn_ws_finalizer };

static uint8_t* dyn_ws_bytes_view(JSContext* ctx, JSValueConst v, size_t* plen,
    int* pbinary)
{
    uint8_t* base;
    size_t off = 0, blen = 0, ab = 0;
    JSValue buf;

    *pbinary = 0;
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE)
        return NULL;
    buf = JS_GetArrayBufferView(ctx, v, &off, &blen, NULL);
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return NULL;
        }
        if (off > ab)
            off = ab;
        if (blen > ab - off)
            blen = ab - off;
        *plen = blen;
        *pbinary = 1;
        return base + off;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        *plen = ab;
        *pbinary = 1;
        return base;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
}

static JSValue dyn_ws_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(this_val, dyn_ws_class_id);
    const char* str = NULL;
    uint8_t* abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w)
        return JS_ThrowTypeError(ctx, "not a WsConn");
    if (argc < 1)
        return JS_UNDEFINED;
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str)
            return JS_EXCEPTION;
    } else {
        abuf = dyn_ws_bytes_view(ctx, argv[0], &len, &binary);
        if (!abuf && !binary) {
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str)
                return JS_EXCEPTION;
        }
    }
    if (w->conn && !w->conn->closed)
        dyn_ws_send_frame(w->conn, binary ? 2 : 1,
            str ? (const uint8_t*)str : abuf, len);
    if (str)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue dyn_ws_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_ws_t* w = (dyn_ws_t*)JS_GetOpaque(this_val, dyn_ws_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_ThrowTypeError(ctx, "not a WsConn");
    if (w->conn && !w->conn->closed) {
        dyn_ws_send_frame(w->conn, 8, NULL, 0);
        dyn_app_conn_close(w->conn);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_ws_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_ws_send),
    JS_CFUNC_DEF("close", 0, dyn_ws_close_method),
};

static int dyn_ws_handshake_valid(const char* base, size_t head_len)
{
    const char *key, *upg, *conn, *ver;
    size_t keylen = 0, upglen = 0, connlen = 0, verlen = 0, b;

    if (head_len < 5 || memcmp(base, "GET ", 4) != 0)
        return 0;
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    upg = dyn_req_header(base, head_len, "upgrade", &upglen);
    conn = dyn_req_header(base, head_len, "connection", &connlen);
    ver = dyn_req_header(base, head_len, "sec-websocket-version", &verlen);
    if (!key || keylen != 24)
        return 0;
    for (b = 0; b < keylen; b++) {
        char ch = key[b];
        int okc = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!okc)
            return 0;
    }
    if (!dyn_hdr_token(upg, upglen, "websocket"))
        return 0;
    if (!dyn_hdr_token(conn, connlen, "upgrade"))
        return 0;
    while (verlen > 0 && (*ver == ' ' || *ver == '\t')) {
        ver++;
        verlen--;
    }
    while (verlen > 0 && (ver[verlen - 1] == ' ' || ver[verlen - 1] == '\t'))
        verlen--;
    return verlen == 2 && ver[0] == '1' && ver[1] == '3';
}

static int dyn_app_ws_handshake(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len)
{
    JSContext* ctx = c->app->ctx;
    const char* key;
    size_t keylen = 0;
    char accept[32], resp[256];
    int rn;
    dyn_ws_t* w;
    JSValue obj, oh;

    if (!dyn_ws_handshake_valid(base, head_len))
        return 0;
    oh = JS_GetPropertyStr(ctx, rt->handler, "upgrade");
    if (JS_IsFunction(ctx, oh)) {
        static const char* const hdr[] = { "origin", "host", "cookie", "sec-websocket-protocol" };
        static const char* const prop[] = { "origin", "host", "cookie", "protocol" };
        JSValue info = JS_NewObject(ctx), r;
        int allow, i;
        for (i = 0; i < 4 && !JS_IsException(info); i++) {
            size_t vlen = 0;
            const char* v = dyn_req_header(base, head_len, hdr[i], &vlen);
            JS_DefinePropertyValueStr(ctx, info, prop[i],
                v ? JS_NewStringLen(ctx, v, vlen) : JS_NULL, JS_PROP_C_W_E);
        }
        r = JS_Call(ctx, oh, JS_UNDEFINED, 1, (JSValueConst*)&info);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            allow = 0;
        } else {
            allow = JS_ToBool(ctx, r) > 0;
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, info);
        if (!allow) {
            JS_FreeValue(ctx, oh);
            return -1;
        }
    }
    JS_FreeValue(ctx, oh);
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    dws_accept(key, keylen, accept);
    rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    dyn_aio_send(c->app->aio, c->fd, resp, (size_t)rn, 0, NULL, NULL);

    c->is_ws = 1;
    c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
    c->ws_frag_frames = 0;
    c->ws_handlers = JS_DupValue(ctx, rt->handler);
    w = (dyn_ws_t*)malloc(sizeof(*w));
    if (!w)
        return 1;
    w->conn = c;
    c->ws_native = w;
    obj = JS_NewObjectClass(ctx, dyn_ws_class_id);
    JS_SetOpaque(obj, w);
    c->ws_this = obj;

    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
    }
    JS_FreeValue(ctx, oh);
    return 1;
}

static void dyn_app_ws_dispatch_msg(dyn_app_conn_t* c, int opcode,
    const uint8_t* payload, size_t plen)
{
    JSContext* ctx = c->app->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, c->ws_handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        JS_FreeValue(ctx, mh);
        {
            uint8_t cl[2] = { 0x03, 0xef };
            dyn_ws_send_frame(c, 8, cl, 2);
        }
        c->close_code = 1007;
        dyn_app_conn_close(c);
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? JS_NewArrayBufferCopy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char*)payload, plen);
        JSValueConst args[3] = { c->ws_this, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

static void dyn_app_ws_process(dyn_app_conn_t* c)
{
    for (;;) {
        uint8_t* p = dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        int opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t *mask, *payload;

        int fin;
        if (avail < 2)
            return;
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        plen = p[1] & 0x7f;
        hdr = 2;
        if (plen == 126) {
            if (avail < 4)
                return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10)
                return;
            plen = 0;
            for (i = 0; i < 8; i++)
                plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (!masked) {
            dyn_app_conn_close(c);
            return;
        }
        if (opcode >= 0x8 && (plen > 125 || !fin)) {
            dyn_app_conn_close(c);
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) {
            dyn_app_conn_close(c);
            return;
        }
        if (avail < hdr + 4 + plen)
            return;
        mask = p + hdr;
        payload = p + hdr + 4;
        {
            uint32_t m32;
            uint64_t m64;
            size_t k = 0, n8 = (size_t)plen & ~(size_t)7;
            memcpy(&m32, mask, 4);
            m64 = ((uint64_t)m32 << 32) | m32;
            for (; k < n8; k += 8) {
                uint64_t v;
                memcpy(&v, payload + k, 8);
                v ^= m64;
                memcpy(payload + k, &v, 8);
            }
            for (i = (uint32_t)k; i < plen; i++)
                payload[i] ^= mask[i & 3];
        }
        frame_total = hdr + 4 + (size_t)plen;

        if (opcode == 0x8) {
            dyn_ws_send_frame(c, 8, NULL, 0);
            dyn_iobuf_consume(&c->in, frame_total);
            dyn_app_conn_close(c);
            return;
        } else if (opcode == 0x9) {
            if (--c->ws_ctl_budget < 0) {
                dyn_app_conn_close(c);
                return;
            }
            dyn_ws_send_frame(c, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {
        } else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
            int frag_active = c->ws_frag_op != 0;
            if ((opcode == 0x0 && !frag_active) || (opcode != 0x0 && frag_active)) {
                dyn_app_conn_close(c);
                return;
            }
            if (opcode != 0x0 && fin) {
                c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_app_ws_dispatch_msg(c, opcode, payload, (size_t)plen);
            } else {
                if (opcode != 0x0) {
                    c->ws_frag_op = opcode;
                    c->ws_frag_frames = 0;
                }
                if (++c->ws_frag_frames > DYN_WS_MAX_FRAGMENTS || dyn_iobuf_rlen(&c->ws_frag) + plen > DYN_ACONN_MAX_REQ || dyn_iobuf_append(&c->ws_frag, payload, (size_t)plen) < 0) {
                    dyn_app_conn_close(c);
                    return;
                }
                if (fin) {
                    c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
                    dyn_app_ws_dispatch_msg(c, c->ws_frag_op,
                        dyn_iobuf_rdata(&c->ws_frag),
                        dyn_iobuf_rlen(&c->ws_frag));
                    dyn_iobuf_reset(&c->ws_frag);
                    c->ws_frag_op = 0;
                    c->ws_frag_frames = 0;
                }
            }
        }
        dyn_iobuf_consume(&c->in, frame_total);
        if (c->closed)
            return;
    }
}

struct dyn_sse {
    dyn_app_conn_t* conn;
};
static JSClassID dyn_sse_class_id;

static void dyn_sse_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(val, dyn_sse_class_id);
    (void)rt;
    free(s);
}
static const JSClassDef dyn_sse_class = { "SseConn", .finalizer = dyn_sse_finalizer };

static int dyn_sse_frame(dyn_iobuf_t* f, const char* data, size_t dlen,
    const char* event, size_t elen)
{
    size_t i = 0;

    if (event) {
        if (dyn_iobuf_append(f, "event: ", 7) < 0
            || dyn_iobuf_append(f, event, elen) < 0
            || dyn_iobuf_append(f, "\n", 1) < 0)
            return -1;
    }
    for (;;) {
        size_t ls = i;
        while (i < dlen && data[i] != '\n')
            i++;
        if (dyn_iobuf_append(f, "data: ", 6) < 0
            || dyn_iobuf_append(f, data + ls, i - ls) < 0
            || dyn_iobuf_append(f, "\n", 1) < 0)
            return -1;
        if (i >= dlen)
            break;
        i++;
    }
    return dyn_iobuf_append(f, "\n", 1);
}

static JSValue dyn_sse_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(this_val, dyn_sse_class_id);
    const char *data = NULL, *event = NULL;
    size_t dlen = 0, elen = 0;
    dyn_iobuf_t f;

    if (!s)
        return JS_ThrowTypeError(ctx, "not an SseConn");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "send(data[, event])");
    data = JS_ToCStringLen(ctx, &dlen, argv[0]);
    if (!data)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        event = JS_ToCStringLen(ctx, &elen, argv[1]);
        if (!event) {
            JS_FreeCString(ctx, data);
            return JS_EXCEPTION;
        }
        if (memchr(event, '\n', elen) || memchr(event, '\r', elen)) {
            JS_FreeCString(ctx, event);
            JS_FreeCString(ctx, data);
            return JS_ThrowTypeError(ctx, "SseConn.send: an event name cannot "
                                          "contain a newline");
        }
    }
    if (memchr(data, '\r', dlen)) {
        if (event)
            JS_FreeCString(ctx, event);
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "SseConn.send: data cannot contain \\r");
    }
    if (s->conn && !s->conn->closed) {
        dyn_iobuf_init(&f);
        if (dyn_sse_frame(&f, data, dlen, event, elen) < 0) {
            dyn_app_conn_close(s->conn);
        } else if (dyn_aio_queued(s->conn->app->aio, s->conn->fd) + f.len
            > DYN_HTTP_OUTBOUND_MAX) {
            dyn_app_conn_close(s->conn);
        } else {
            dyn_aio_send(s->conn->app->aio, s->conn->fd, f.data, f.len, 0,
                NULL, NULL);
            s->conn->last_ms = dyn_timer_now_ms();
        }
        dyn_iobuf_free(&f);
    }
    if (event)
        JS_FreeCString(ctx, event);
    JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static JSValue dyn_sse_close_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sse_t* s = (dyn_sse_t*)JS_GetOpaque(this_val, dyn_sse_class_id);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_ThrowTypeError(ctx, "not an SseConn");
    if (s->conn && !s->conn->closed)
        dyn_app_conn_close(s->conn);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_sse_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_sse_send),
    JS_CFUNC_DEF("close", 0, dyn_sse_close_method),
};

static void dyn_app_sse_handshake(dyn_app_conn_t* c,
    const dyn_app_route_t* rt)
{
    JSContext* ctx = c->app->ctx;
    static const char hdr[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
    dyn_sse_t* s;
    JSValue obj, oh;

    if (c->app->n_sse >= DYN_APP_SSE_MAX) {
        dyn_app_send_err(c, 503, "{\"error\":\"too many event streams\"}");
        dyn_app_conn_close(c);
        return;
    }
    s = (dyn_sse_t*)malloc(sizeof(*s));
    if (!s) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    s->conn = c;
    c->sse_native = s;
    obj = JS_NewObjectClass(ctx, dyn_sse_class_id);
    JS_SetOpaque(obj, s);
    c->sse_this = obj;
    dyn_aio_send(c->app->aio, c->fd, hdr, sizeof(hdr) - 1, 0, NULL, NULL);
    c->is_sse = 1;
    c->app->n_sse++;
    c->sse_handlers = JS_DupValue(ctx, rt->handler);
    dyn_app_met_response(c, 0);
    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
    }
    JS_FreeValue(ctx, oh);
}

static JSClassID dyn_app_class_id;

#define DYN_APP_GZIP_MIN 256

static int dyn_ae_q(const char* v, size_t n, const char* tok, size_t tl)
{
    size_t i = 0;
    while (i < n) {
        size_t ts = i, te, tok_end, name_end, nl;
        int q1000 = 1000;
        const char *pp, *pe;
        while (ts < n && (v[ts] == ' ' || v[ts] == '\t' || v[ts] == ','))
            ts++;
        te = ts;
        while (te < n && v[te] != ',')
            te++;
        tok_end = te;
        while (tok_end > ts && (v[tok_end - 1] == ' ' || v[tok_end - 1] == '\t'))
            tok_end--;
        name_end = ts;
        while (name_end < tok_end && v[name_end] != ';')
            name_end++;
        nl = name_end - ts;
        while (nl && (v[ts + nl - 1] == ' ' || v[ts + nl - 1] == '\t'))
            nl--;
        if (nl == tl && dyn_ci_equal(v + ts, (int)tl, tok)) {
            pp = v + name_end;
            pe = v + tok_end;
            while (pp + 1 < pe) {
                if (*pp == ';') {
                    const char *pn = pp + 1, *eq;
                    while (pn < pe && (*pn == ' ' || *pn == '\t'))
                        pn++;
                    eq = (*pn == 'q' || *pn == 'Q') ? pn + 1 : NULL;
                    if (eq) {
                        while (eq < pe && (*eq == ' ' || *eq == '\t'))
                            eq++;
                    }
                    if (eq && *eq == '=') {
                        const char* q = eq + 1;
                        int seen = 0, is_zero = 1;
                        while (q < pe && (*q == ' ' || *q == '\t'))
                            q++;
                        if (q < pe && *q >= '0' && *q <= '9') {
                            seen = 1;
                            if (*q != '0')
                                is_zero = 0;
                            q++;
                        }
                        while (q < pe && *q >= '0' && *q <= '9') {
                            if (*q != '0')
                                is_zero = 0;
                            q++;
                        }
                        if (q < pe && *q == '.') {
                            q++;
                            while (q < pe && *q >= '0' && *q <= '9') {
                                if (*q != '0')
                                    is_zero = 0;
                                q++;
                            }
                        }
                        if (seen && is_zero)
                            q1000 = 0;
                    }
                }
                pp++;
            }
            return q1000;
        }
        i = te + 1;
    }
    return -1;
}

#define DYN_APP_SMALL_RESP 1024

static void dyn_app_send_body_x(dyn_app_conn_t* c, int status,
    const char* ctype, const char* body,
    size_t body_len, const char* extra)
{
    static const char hdr200_a[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
    static const char hdr_b[] = "\r\nConnection: keep-alive\r\n\r\n";
    dyn_iobuf_t out;
    char head[512];
    const char* sbody = body;
    size_t slen = body_len;
    size_t wlen;
    uint8_t* gz = NULL;
    size_t gz_len = 0;
    int n, compressed = 0;
    int no_body = (status >= 100 && status < 200) || status == 204 || status == 304;

    if (no_body) {
        sbody = "";
        slen = 0;
    } else if (c->app->compress && c->accept_gzip
        && (body_len >= DYN_APP_GZIP_MIN || c->identity_refused)
        && dyn_gzip_build((const uint8_t*)body, body_len, &gz,
               &gz_len)
            == 0) {
        if (gz && (gz_len < body_len || c->identity_refused)) {
            compressed = 1;
            sbody = (const char*)gz;
            slen = gz_len;
        } else {
            free(gz);
            gz = NULL;
        }
    }
    if (!compressed && !no_body && c->identity_refused) {
        c->identity_refused = 0;
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        return;
    }

    wlen = (no_body || c->head_only) ? 0 : slen;
    dyn_iobuf_init(&out);
    if (no_body) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n%sConnection: keep-alive\r\n\r\n",
            status, dyn_reason_phrase(status), extra ? extra : "");
    } else if (compressed) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n%sContent-Type: %s\r\n"
            "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"
            "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
            status, dyn_reason_phrase(status), extra ? extra : "",
            ctype, slen);
    } else if (!extra && status == 200 && strcmp(ctype, "application/json") == 0) {
        n = (int)(sizeof(hdr200_a) - 1);
        memcpy(head, hdr200_a, (size_t)n);
        n += dyn_put_u64(head + n, (uint64_t)body_len);
        memcpy(head + n, hdr_b, sizeof(hdr_b) - 1);
        n += (int)(sizeof(hdr_b) - 1);
    } else {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n%sContent-Type: %s\r\n"
            "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
            status, dyn_reason_phrase(status), extra ? extra : "",
            ctype, slen);
    }
    if (n > 0 && (size_t)n >= sizeof(head)) {
        free(gz);
        dyn_app_send_err(c, 500, "{\"error\":\"response head too large\"}");
        return;
    }
    if (n > 0) {
        if ((size_t)n + wlen <= DYN_APP_SMALL_RESP) {
            char small[DYN_APP_SMALL_RESP];
            memcpy(small, head, (size_t)n);
            if (wlen)
                memcpy(small + n, sbody, wlen);
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, small, (size_t)n + wlen, 0,
                    dyn_app_out_last, c)
                < 0)
                dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
            free(gz);
            dyn_app_met_response(c, (double)wlen);
            return;
        }
        dyn_iobuf_init(&out);
        dyn_iobuf_append(&out, head, (size_t)n);
        if (wlen)
            dyn_iobuf_append(&out, sbody, wlen);
        c->refs++;
        if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                dyn_app_out_last, c)
            < 0)
            dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
        dyn_iobuf_free(&out);
    }
    free(gz);
    dyn_app_met_response(c, (double)wlen);
}

static void dyn_app_send_body(dyn_app_conn_t* c, int status, const char* ctype,
    const char* body, size_t body_len)
{
    dyn_app_send_body_x(c, status, ctype, body, body_len, NULL);
}

static void dyn_app_send_json(dyn_app_conn_t* c, int status, const char* body,
    size_t body_len)
{
    dyn_app_send_body(c, status, "application/json", body, body_len);
}

static void dyn_json_escape_n_into(char* buf, size_t limit, size_t* pos,
    const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n && *pos + 7 < limit; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = '"';
        } else if (ch == '\\') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = '\\';
        } else if (ch == '\n') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'n';
        } else if (ch == '\r') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'r';
        } else if (ch == '\t') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 't';
        } else if (ch < 0x20)
            *pos += (size_t)snprintf(buf + *pos, 7, "\\u%04x", ch);
        else
            buf[(*pos)++] = (char)ch;
    }
}

static size_t dyn_rpc_err_compose_n(char* buf, size_t cap, int code,
    const char* msg, size_t msg_len,
    const char* id_json)
{
    const char* idsafe = id_json && *id_json ? id_json : "null";
    size_t idlen = strlen(idsafe);
    size_t tail_room, pos = 0;
    int n;

    if (idlen > 100) {
        idsafe = "null";
        idlen = 4;
    }
    tail_room = idlen + 16;
    if (tail_room >= cap)
        return 0;
    n = snprintf(buf, cap,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"",
        code);
    if (n < 0 || (size_t)n >= cap - tail_room)
        return 0;
    pos = (size_t)n;
    dyn_json_escape_n_into(buf, cap - tail_room, &pos, msg ? msg : "", msg_len);
    n = snprintf(buf + pos, cap - pos, "\"},\"id\":%s}", idsafe);
    return n < 0 ? 0 : pos + (size_t)n;
}

static size_t dyn_rpc_err_compose(char* buf, size_t cap, int code,
    const char* msg, const char* id_json)
{
    return dyn_rpc_err_compose_n(buf, cap, code, msg,
        msg ? strlen(msg) : 0, id_json);
}

static void dyn_app_rpc_error(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, const char* id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

static void dyn_app_rpc_error_n(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, size_t msg_len,
    const char* id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose_n(buf, sizeof(buf), code, msg, msg_len,
        id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

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

static void dyn_app_stream_resume(dyn_app_conn_t* c);
static void dyn_app_stream_stop(dyn_app_conn_t* c);
static void dyn_app_pump_resume(dyn_app_conn_t* c);
static void dyn_app_stream_sent(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len, void* udata);
static void dyn_app_stream_finished(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len,
    void* udata);

static int dyn_value_is_byte_source(JSContext* ctx, JSValueConst v)
{
    JSValue rf;
    int is;
    if (!JS_IsObject(v))
        return 0;
    rf = JS_GetPropertyStr(ctx, v, "read");
    is = JS_IsFunction(ctx, rf);
    JS_FreeValue(ctx, rf);
    return is;
}

static JSValue dyn_value_stream_member(JSContext* ctx, JSValueConst v)
{
    JSValue sv;
    if (!JS_IsObject(v) || dyn_value_is_byte_source(ctx, v))
        return JS_UNDEFINED;
    sv = dyn_app_own_get(ctx, v, "stream");
    if (!dyn_value_is_byte_source(ctx, sv)) {
        JS_FreeValue(ctx, sv);
        return JS_UNDEFINED;
    }
    return sv;
}
static void dyn_app_stream_stop(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;
    if (!st)
        return;
    c->st = NULL;
    JS_FreeValue(ctx, st->src);
    JS_FreeValue(ctx, st->readfn);
    JS_FreeValue(ctx, st->chunk);
    JS_FreeValue(ctx, st->pending);
    free(st->frame);
    free(st->ct);
    free(st);
    if (!c->closed)
        dyn_app_pump_resume(c);
    dyn_app_conn_unref(c);
}

static void dyn_app_stream_enter(dyn_app_stream_t* st) { st->depth++; }

static void dyn_app_stream_reap(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st || !st->retiring || st->depth > 0 || !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_stop(c);
}

static void dyn_app_stream_retire(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st)
        return;
    st->done = 1;
    st->retiring = 1;
    dyn_app_stream_reap(c);
}

static void dyn_app_stream_leave(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    if (!st || --st->depth > 0)
        return;
    dyn_app_stream_reap(c);
}

static void dyn_app_stream_abort(dyn_app_conn_t* c)
{
    if (!c->st)
        return;
    dyn_app_stream_retire(c);
}

static int dyn_app_stream_on_count(dyn_app_conn_t* c, double n)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;

    if (n < 0 || n > (double)DYN_APP_STREAM_CHUNK)
        return -1;
    if (n == 0) {
        JSValue cf = JS_GetPropertyStr(ctx, st->src, "close");
        if (JS_IsFunction(ctx, cf))
            dyn_call_drop(ctx, cf, st->src, 0, NULL);
        JS_FreeValue(ctx, cf);
        if (st->done || c->closed)
            return 0;
        st->done = 1;
        memcpy(st->frame, "0\r\n\r\n", 5);
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame, 5, 0,
                dyn_app_stream_finished, c)
            < 0) {
            st->in_send = 0;
            dyn_app_stream_finished(NULL, -ECONNRESET, NULL, 0, c);
            return 0;
        }
        return (st->in_send || st->retiring) ? 0 : 1;
    }
    {
        size_t un = (size_t)n;
        size_t voff, vlen, vbpe, ab_size = 0;
        int hl;
        JSValue ab;
        uint8_t* base;
        ab = JS_GetArrayBufferView(ctx, st->chunk, &voff, &vlen, &vbpe);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return -1;
        if (un > vlen || voff > ab_size || un > ab_size - voff)
            return -1;
        hl = snprintf((char*)st->frame, 16, "%zx\r\n", un);
        if (hl <= 0 || (size_t)hl + un + 2 > st->frame_cap)
            return -1;
        memcpy(st->frame + hl, base + voff, un);
        st->frame[hl + un] = '\r';
        st->frame[hl + un + 1] = '\n';
        st->total += un;
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame,
                (unsigned)(hl + un + 2), 0,
                dyn_app_stream_sent, c)
            < 0) {
            st->in_send = 0;
            return -1;
        }
        return (st->in_send || st->retiring) ? 0 : 1;
    }
}

static void dyn_app_stream_fail(dyn_app_conn_t* c)
{
    if (!c->st || c->st->done || c->closed)
        return;
    c->st->done = 1;
    dyn_app_conn_close(c);
    dyn_app_stream_retire(c);
}

static JSValue dyn_app_stream_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    int64_t ptr = 0;
    dyn_app_conn_t* c;
    dyn_app_stream_t* st;
    double n = -1;
    (void)this_val;
    if (JS_ToInt64(ctx, &ptr, data[0]))
        return JS_EXCEPTION;
    c = (dyn_app_conn_t*)(uintptr_t)ptr;
    st = c->st;
    if (!st)
        return JS_UNDEFINED;
    dyn_app_stream_enter(st);
    JS_FreeValue(ctx, st->pending);
    st->pending = JS_UNDEFINED;
    if (st->done || c->closed) {
        dyn_app_stream_retire(c);
    } else {
        st->busy = 0;
        if (!magic && argc > 0 && JS_IsNumber(argv[0])) {
            if (JS_ToFloat64(ctx, &n, argv[0]) < 0) {
                dyn_app_stream_leave(c);
                return JS_EXCEPTION;
            }
        }
        {
            int keep = magic ? -1 : dyn_app_stream_on_count(c, n);
            if (keep == 1)
                dyn_app_stream_resume(c);
            else if (keep != 0)
                dyn_app_stream_fail(c);
        }
    }
    dyn_app_stream_leave(c);
    return JS_UNDEFINED;
}

static void dyn_app_stream_sent(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len, void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_stream_t* st = c->st;
    int inline_cb;
    (void)aio;
    (void)buf;
    (void)len;
    if (!st)
        return;
    inline_cb = st->depth > 0;
    dyn_app_stream_enter(st);
    if (res < 0) {
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
    } else {
        st->in_send = 0;
        if (!inline_cb)
            dyn_app_stream_resume(c);
    }
    dyn_app_stream_leave(c);
}

static void dyn_app_stream_finished(dyn_aio_t* aio, int res,
    const uint8_t* buf, unsigned len,
    void* udata)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)udata;
    dyn_app_stream_t* st = c->st;
    (void)aio;
    (void)buf;
    (void)len;
    if (!st)
        return;
    dyn_app_stream_enter(st);
    if (res < 0) {
        dyn_app_conn_close(c);
    } else {
        st->in_send = 0;
        st->done = 1;
        dyn_app_met_response(c, (double)st->total);
        if (c->close_after)
            dyn_app_conn_close(c);
    }
    dyn_app_stream_retire(c);
    dyn_app_stream_leave(c);
}

static JSValue dyn_app_read_noop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)data;
    return JS_UNDEFINED;
}

static int dyn_read_is_promise(JSContext* ctx, JSValueConst pr)
{
    JSPromiseStateEnum ps = JS_PromiseState(ctx, pr);
    return ps == JS_PROMISE_PENDING || ps == JS_PROMISE_FULFILLED || ps == JS_PROMISE_REJECTED;
}

static int dyn_app_proto_then(JSContext* ctx, JSValueConst pr,
    JSValueConst onres, JSValueConst onrej)
{
    JSValue g, pcons, pproto, pthen, tr;
    JSValueConst a[2];
    int ret = -1;

    a[0] = onres;
    a[1] = onrej;
    g = JS_GetGlobalObject(ctx);
    pcons = JS_GetPropertyStr(ctx, g, "Promise");
    JS_FreeValue(ctx, g);
    if (JS_IsException(pcons)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    pproto = JS_GetPropertyStr(ctx, pcons, "prototype");
    if (JS_IsException(pproto)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, pcons);
        return -1;
    }
    pthen = JS_GetPropertyStr(ctx, pproto, "then");
    JS_FreeValue(ctx, pproto);
    if (JS_IsException(pthen)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, pcons);
        return -1;
    }
    if (JS_IsFunction(ctx, pthen)) {
        tr = JS_Call(ctx, pthen, pr, 2, a);
        if (!JS_IsException(tr)) {
            JS_FreeValue(ctx, tr);
            ret = 0;
            goto out;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, tr);
        if (JS_DefinePropertyValueStr(ctx, pr, "constructor",
                JS_DupValue(ctx, pcons),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)
            >= 0) {
            tr = JS_Call(ctx, pthen, pr, 2, a);
            if (!JS_IsException(tr)) {
                JS_FreeValue(ctx, tr);
                ret = 0;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, tr);
            }
        }
    }
out:
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, pcons);
    return ret;
}

static int dyn_app_read_attach(JSContext* ctx, JSValueConst pr,
    JSValueConst onres, JSValueConst onrej)
{
    JSValue thenf, tr;
    JSValueConst a[2];

    if (!JS_IsObject(pr))
        return 0;
    a[0] = onres;
    a[1] = onrej;
    if (dyn_read_is_promise(ctx, pr)) {
        if (dyn_app_proto_then(ctx, pr, onres, onrej) == 0)
            return 0;
    }
    thenf = JS_GetPropertyStr(ctx, pr, "then");
    if (JS_IsException(thenf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    if (!JS_IsFunction(ctx, thenf)) {
        JS_FreeValue(ctx, thenf);
        return -1;
    }
    tr = JS_Call(ctx, thenf, pr, 2, a);
    JS_FreeValue(ctx, thenf);
    if (JS_IsException(tr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JS_FreeValue(ctx, tr);
    return 0;
}

static void dyn_app_read_abandon(JSContext* ctx, JSValueConst pr)
{
    JSValue onres, onrej;
    if (!JS_IsObject(pr))
        return;
    onres = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 0, 0, NULL);
    onrej = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 1, 0, NULL);
    if (!JS_IsException(onres) && !JS_IsException(onrej))
        dyn_app_read_attach(ctx, pr, onres, onrej);
    else {
        if (JS_IsException(onres))
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (JS_IsException(onrej))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, onres);
    JS_FreeValue(ctx, onrej);
}

static void dyn_app_stream_resume(dyn_app_conn_t* c)
{
    dyn_app_stream_t* st = c->st;
    JSContext* ctx = c->app->ctx;

    if (!st || st->busy || !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_enter(st);
    st->busy = 1;
    while (!st->done && !c->closed) {
        JSValue pr, v;
        JSPromiseStateEnum ps;
        double n = -1;
        int keep;

        pr = JS_Call(ctx, st->readfn, st->src, 1, (JSValueConst*)&st->chunk);
        if (JS_IsException(pr)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;
        }
        if (st->done || c->closed) {
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;
        }
        ps = JS_PromiseState(ctx, pr);
        if (ps == JS_PROMISE_REJECTED) {
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;
        }
        if (ps == JS_PROMISE_PENDING) {
            JSValue funcs[2], promise, pthen, dptr, onres, onrej, tr;
            JSValueConst thenargs[2];
            promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(promise)) {
                dyn_app_read_abandon(ctx, pr);
                JS_FreeValue(ctx, pr);
                break;
            }
            dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)c);
            onres = JS_NewCFunctionData(ctx, dyn_app_stream_settle, 1, 0,
                1, &dptr);
            onrej = JS_NewCFunctionData(ctx, dyn_app_stream_settle, 1, 1,
                1, &dptr);
            JS_FreeValue(ctx, dptr);
            pthen = JS_GetPropertyStr(ctx, promise, "then");
            thenargs[0] = onres;
            thenargs[1] = onrej;
            tr = JS_Call(ctx, pthen, promise, 2, thenargs);
            JS_FreeValue(ctx, pthen);
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            if (JS_IsException(tr)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, funcs[0]);
                JS_FreeValue(ctx, funcs[1]);
                JS_FreeValue(ctx, promise);
                dyn_app_read_abandon(ctx, pr);
                JS_FreeValue(ctx, pr);
                break;
            }
            JS_FreeValue(ctx, tr);
            if (dyn_app_read_attach(ctx, pr, funcs[0], funcs[1]) < 0) {
                JSValue undef = JS_UNDEFINED;
                tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &undef);
                JS_FreeValue(ctx, tr);
            }
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            st->pending = pr;
            st->busy = 0;
            goto out;
        }
        v = JS_PromiseResult(ctx, pr);
        JS_FreeValue(ctx, pr);
        if (JS_IsNumber(v)) {
            if (JS_ToFloat64(ctx, &n, v) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, v);
                break;
            }
        }
        JS_FreeValue(ctx, v);
        keep = dyn_app_stream_on_count(c, n);
        if (keep == 1)
            continue;
        if (keep == 0) {
            st->busy = 0;
            goto out;
        }
        break;
    }
    st->busy = 0;
    dyn_app_stream_fail(c);
out:
    dyn_app_stream_leave(c);
}

static int dyn_app_try_stream_result(dyn_app_conn_t* c, JSValueConst result)
{
    JSContext* ctx = c->app->ctx;
    JSValue src = JS_UNDEFINED;
    JSValue sv, ctv;
    const char* cts = NULL;
    size_t cts_len = 0;
    int status = 200;
    dyn_app_stream_t* st;
    char head[512];
    int hn;

    if (c->st) {
        dyn_app_conn_close(c);
        return 1;
    }
    if (dyn_value_is_byte_source(ctx, result)) {
        src = JS_DupValue(ctx, result);
    } else {
        src = dyn_value_stream_member(ctx, result);
        if (JS_IsUndefined(src))
            return 0;
        sv = dyn_app_own_get(ctx, result, "status");
        if (JS_IsException(sv)) {
            JS_FreeValue(ctx, sv);
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (JS_IsNumber(sv)) {
            int32_t s32 = 200;
            if (JS_ToInt32(ctx, &s32, sv) == 0 && s32 >= 100 && s32 < 600)
                status = s32;
        }
        JS_FreeValue(ctx, sv);
        ctv = dyn_app_own_get(ctx, result, "contentType");
        if (JS_IsException(ctv)) {
            JS_FreeValue(ctx, ctv);
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (JS_IsString(ctv)) {
            cts = JS_ToCStringLen(ctx, &cts_len, ctv);
        }
        JS_FreeValue(ctx, ctv);
    }
    if (cts) {
        int ctl = dyn_hdr_value_ok(cts, cts_len) != 0;
        if (ctl || cts_len > DYN_HTTP_CT_MAX) {
            JS_FreeCString(ctx, cts);
            JS_FreeValue(ctx, src);
            dyn_app_send_err(c, 500, ctl ? "{\"error\":\"stream contentType must not contain control "
                                           "characters\"}"
                                         : "{\"error\":\"stream contentType too long\"}");
            return 1;
        }
    }
    if (c->closed) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        return 1;
    }
    if (!c->http11) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500,
            "{\"error\":\"streaming bodies require an HTTP/1.1 peer\"}");
        return 1;
    }
    if (c->identity_refused) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        c->identity_refused = 0;
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        return 1;
    }
    st = (dyn_app_stream_t*)calloc(1, sizeof *st);
    if (!st) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
        return 1;
    }
    st->status = status;
    st->ct = strdup(cts && *cts ? cts : "application/octet-stream");
    if (cts)
        JS_FreeCString(ctx, cts);
    st->frame_cap = DYN_APP_STREAM_CHUNK + 32;
    st->frame = (uint8_t*)malloc(st->frame_cap);
    st->pending = JS_UNDEFINED;
    if (st->frame) {
        JSValue dim = JS_NewInt64(ctx, DYN_APP_STREAM_CHUNK);
        st->chunk = JS_NewTypedArray(ctx, 1, &dim, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, dim);
        st->readfn = JS_GetPropertyStr(ctx, src, "read");
    }
    if (!st->frame || !st->ct || JS_IsException(st->chunk) || !JS_IsFunction(ctx, st->readfn)) {
        if (JS_IsException(st->chunk))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, src);
        JS_FreeValue(ctx, st->readfn);
        JS_FreeValue(ctx, st->chunk);
        free(st->frame);
        free(st->ct);
        free(st);
        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
        return 1;
    }
    st->src = src;
    c->st = st;
    c->refs++;
    dyn_app_stream_enter(st);
    hn = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
        st->status, dyn_reason_phrase(st->status), st->ct,
        c->close_after ? "close" : "keep-alive");
    if (hn <= 0 || (size_t)hn >= sizeof head) {
        dyn_app_conn_close(c);
        dyn_app_send_err(c, 500, "{\"error\":\"stream start failed\"}");
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);
        return 1;
    }
    st->in_send = 1;
    if (dyn_aio_send(c->app->aio, c->fd, (const uint8_t*)head,
            (unsigned)hn, 0, dyn_app_stream_sent, c)
        < 0) {
        st->in_send = 0;
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);
        return 1;
    }
    if (!st->in_send && !st->retiring)
        dyn_app_stream_resume(c);
    dyn_app_stream_leave(c);
    return 1;
}

static void dyn_app_send_rpc_result(dyn_app_conn_t* c, JSValueConst result,
    const char* id_s)
{
    JSContext* ctx = c->app->ctx;
    size_t out_len;
    if (dyn_app_try_stream_result(c, result))
        return;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    int res_undef = !JS_IsException(jstr) && JS_IsUndefined(jstr);
    const char* out_s = (JS_IsException(jstr) || res_undef)
        ? NULL
        : JS_ToCStringLen(ctx, &out_len, jstr);
    if (res_undef) {
        out_s = "null";
        out_len = 4;
    }
    if (out_s) {
        dyn_iobuf_t o;
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(&o, out_s, out_len);
        dyn_iobuf_append(&o, ",\"id\":", 6);
        dyn_iobuf_append(&o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char*)o.data, o.len);
        dyn_iobuf_free(&o);
        if (!res_undef)
            JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr))
            JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 500, -32603, "Internal error", id_s);
    }
    JS_FreeValue(ctx, jstr);
}

static void dyn_app_send_rpc_throw(dyn_app_conn_t* c, JSValueConst exc,
    const char* id_s)
{
    JSContext* ctx = c->app->ctx;
    size_t em_len = 0;
    const char* em = JS_ToCStringLen(ctx, &em_len, exc);
    dyn_app_rpc_error_n(c, 500, -32000, em ? em : "Server error",
        em ? em_len : strlen("Server error"), id_s);
    if (em)
        JS_FreeCString(ctx, em);
}

typedef struct {
    dyn_app_conn_t* conn;
    char* id;
    int accept_gzip;
    int identity_refused;
    int counted;
} dyn_app_pend_t;

static JSClassID dyn_pend_class_id;

static void dyn_app_pump_resume(dyn_app_conn_t* c);

static void dyn_app_pend_release(dyn_app_pend_t* pd, int abandoned)
{
    dyn_app_conn_t* c = pd->conn;
    int parked = pd->counted;
    if (parked)
        c->resp_parked--;
    free(pd->id);
    free(pd);
    if (parked) {
        if (abandoned)
            dyn_app_conn_close(c);
        else
            dyn_app_pump_resume(c);
    }
    dyn_app_conn_unref(c);
}

static void dyn_pend_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_app_pend_t* pd = (dyn_app_pend_t*)JS_GetOpaque(val, dyn_pend_class_id);
    (void)rt;
    if (pd) {
        JS_SetOpaque(val, NULL);
        dyn_app_pend_release(pd, 1);
    }
}

static const JSClassDef dyn_pend_class = {
    "PendingResponse",
    .finalizer = dyn_pend_finalizer,
};

static dyn_app_pend_t* dyn_app_pend_claim(JSValueConst obj)
{
    dyn_app_pend_t* pd = (dyn_app_pend_t*)JS_GetOpaque(obj, dyn_pend_class_id);
    if (pd)
        JS_SetOpaque(obj, NULL);
    return pd;
}

static JSValue dyn_app_rpc_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    dyn_app_pend_t* pd;
    dyn_app_conn_t* c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd)
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        int saved_g = c->accept_gzip, saved_i = c->identity_refused;
        c->accept_gzip = pd->accept_gzip;
        c->identity_refused = pd->identity_refused;
        if (magic)
            dyn_app_send_rpc_throw(c, v, pd->id);
        else
            dyn_app_send_rpc_result(c, v, pd->id);
        c->accept_gzip = saved_g;
        c->identity_refused = saved_i;
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

static void dyn_app_build_error(int code, const char* msg, const char* id_s,
    dyn_iobuf_t* o)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_s);
    if (n > 0)
        dyn_iobuf_append(o, buf, n);
}
static void dyn_app_build_result(JSContext* ctx, JSValueConst result,
    const char* id_s, dyn_iobuf_t* o)
{
    size_t out_len;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    const char* out_s = JS_IsException(jstr) ? NULL : JS_ToCStringLen(ctx, &out_len, jstr);
    if (out_s) {
        dyn_iobuf_append(o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(o, out_s, out_len);
        dyn_iobuf_append(o, ",\"id\":", 6);
        dyn_iobuf_append(o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(o, "}", 1);
        JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr))
            JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_build_error(-32603, "Internal error", id_s, o);
    }
    JS_FreeValue(ctx, jstr);
}

static int dyn_app_rpc_build_one(dyn_app_conn_t* c, JSValueConst methods,
    JSValueConst elem, dyn_iobuf_t* out, int comma)
{
    JSContext* ctx = c->app->ctx;
    JSValue method_v = JS_GetPropertyStr(ctx, elem, "method");
    JSValue id = JS_GetPropertyStr(ctx, elem, "id");
    JSValue params = JS_GetPropertyStr(ctx, elem, "params");
    int has_id = !JS_IsUndefined(id);
    const char* method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    const char* id_s;
    int wrote = 0;
    dyn_iobuf_t tmp;
    dyn_iobuf_init(&tmp);
    {
        JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
        id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
            ? NULL
            : JS_ToCString(ctx, idj);
        JS_FreeValue(ctx, idj);
    }

    if (!method_s) {
        if (has_id) {
            dyn_app_build_error(-32600, "Invalid Request", id_s, &tmp);
            wrote = 1;
        }
    } else {
        JSValue fn = dyn_app_own_get(ctx, methods, method_s);
        if (!JS_IsFunction(ctx, fn)) {
            if (has_id) {
                dyn_app_build_error(-32601, "Method not found", id_s, &tmp);
                wrote = 1;
            }
        } else {
            JSValue res = JS_UNDEFINED;
            int arg_err = 0;
            if (JS_IsUndefined(params)) {
                res = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
            } else if (JS_IsArray(ctx, params)) {
                JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
                uint32_t len = 0, i;
                JSValueConst* argv = NULL;
                int badlen = 0;
                if (JS_ToUint32(ctx, &len, lenv)) {
                    dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
                    badlen = 1;
                }
                if (badlen) {
                    arg_err = 1;
                    len = 0;
                }
                JS_FreeValue(ctx, lenv);
                if (!badlen) {
                    if (len > DYN_APP_MAX_PARAMS) {
                        if (has_id) {
                            dyn_app_build_error(-32602, "Invalid params", id_s, &tmp);
                            wrote = 1;
                        }
                        arg_err = 1;
                    } else if (len > 0) {
                        argv = (JSValueConst*)malloc(len * sizeof(*argv));
                        if (!argv) {
                            if (has_id) {
                                dyn_app_build_error(-32603, "Internal error", id_s, &tmp);
                                wrote = 1;
                            }
                            arg_err = 1;
                        } else {
                            for (i = 0; i < len; i++)
                                argv[i] = JS_GetPropertyUint32(ctx, params, i);
                            res = JS_Call(ctx, fn, JS_UNDEFINED, len, argv);
                            for (i = 0; i < len; i++)
                                JS_FreeValue(ctx, argv[i]);
                            free(argv);
                        }
                    } else {
                        res = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
                    }
                }
            } else {
                JSValueConst a[1] = { params };
                res = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
            }
            if (!arg_err && JS_IsException(res)) {
                JSValue exc = JS_GetException(ctx);
                if (has_id) {
                    const char* em = JS_ToCString(ctx, exc);
                    dyn_app_build_error(-32000, em ? em : "Server error", id_s, &tmp);
                    if (em)
                        JS_FreeCString(ctx, em);
                    wrote = 1;
                }
                JS_FreeValue(ctx, exc);
            } else if (!arg_err) {
                int thenable = 0;
                if (JS_IsObject(res)) {
                    JSValue th = JS_GetPropertyStr(ctx, res, "then");
                    thenable = JS_IsFunction(ctx, th);
                    JS_FreeValue(ctx, th);
                }
                if (thenable) {
                    if (has_id) {
                        dyn_app_build_error(-32000, "async handler not allowed in batch", id_s, &tmp);
                        wrote = 1;
                    }
                } else if (has_id && dyn_value_is_byte_source(ctx, res)) {
                    dyn_app_build_error(-32603,
                        "streaming body not allowed in a batch", id_s, &tmp);
                    wrote = 1;
                } else if (has_id) {
                    dyn_app_build_result(ctx, res, id_s, &tmp);
                    wrote = 1;
                }
                JS_FreeValue(ctx, res);
            }
        }
        JS_FreeValue(ctx, fn);
    }
    if (wrote) {
        if (comma)
            dyn_iobuf_append(out, ",", 1);
        dyn_iobuf_append(out, tmp.data, tmp.len);
    }
    dyn_iobuf_free(&tmp);
    if (method_s)
        JS_FreeCString(ctx, method_s);
    if (id_s)
        JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    return wrote;
}

static void dyn_app_dispatch_batch(dyn_app_conn_t* c, JSValueConst methods,
    JSValueConst arr)
{
    JSContext* ctx = c->app->ctx;
    uint32_t len = 0, i;
    int nresp = 0;
    dyn_iobuf_t out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    JS_FreeValue(ctx, lv);
    if (len == 0) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    if (len > DYN_APP_MAX_BATCH) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    dyn_iobuf_init(&out);
    dyn_iobuf_append(&out, "[", 1);
    for (i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
        if (dyn_app_rpc_build_one(c, methods, elem, &out, nresp > 0))
            nresp++;
        JS_FreeValue(ctx, elem);
    }
    dyn_iobuf_append(&out, "]", 1);
    if (nresp > 0)
        dyn_app_send_json(c, 200, (const char*)out.data, out.len);
    else
        dyn_app_send_json(c, 200, "", 0);
    dyn_iobuf_free(&out);
}

static void dyn_app_dispatch_rpc(dyn_app_conn_t* c, JSValueConst methods,
    const char* body, size_t body_len)
{
    JSContext* ctx = c->app->ctx;
    JSValue req, method_v, params, id, fn, result;
    const char *method_s, *id_s;

    {
        char* m = DYN_UNCONST(body);
        char save = m[body_len];
        m[body_len] = 0;
        req = JS_ParseJSON(ctx, body, body_len, "<rpc>");
        m[body_len] = save;
    }
    if (JS_IsException(req)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 400, -32700, "Parse error", "null");
        return;
    }
    if (JS_IsArray(ctx, req)) {
        JSValue m = JS_DupValue(ctx, methods);
        dyn_app_dispatch_batch(c, m, req);
        JS_FreeValue(ctx, m);
        JS_FreeValue(ctx, req);
        return;
    }
    methods = JS_DupValue(ctx, methods);
    method_v = JS_GetPropertyStr(ctx, req, "method");
    id = JS_GetPropertyStr(ctx, req, "id");
    params = JS_GetPropertyStr(ctx, req, "params");
    {
        JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
        id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
            ? NULL
            : JS_ToCString(ctx, idj);
        JS_FreeValue(ctx, idj);
    }

    if (JS_IsUndefined(id)) {
        goto cleanup;
    }

    method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    if (!method_s) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", id_s);
        goto cleanup;
    }
    fn = dyn_app_own_get(ctx, methods, method_s);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        dyn_app_rpc_error(c, 404, -32601, "Method not found", id_s);
        JS_FreeCString(ctx, method_s);
        goto cleanup;
    }
    if (JS_IsUndefined(params)) {
        result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    } else if (JS_IsArray(ctx, params)) {
        JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
        uint32_t len = 0, i;
        JSValueConst argv_stack[8];
        JSValueConst* argv = NULL;
        if (JS_ToUint32(ctx, &len, lenv)) {
            JS_FreeValue(ctx, lenv);
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, method_s);
            dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
            goto cleanup;
        }
        JS_FreeValue(ctx, lenv);
        if (len > DYN_APP_MAX_PARAMS) {
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, method_s);
            dyn_app_rpc_error(c, 400, -32602, "Invalid params", id_s);
            goto cleanup;
        }
        if (len > 0) {
            if (len <= countof(argv_stack)) {
                argv = argv_stack;
            } else {
                argv = (JSValueConst*)malloc(len * sizeof(*argv));
                if (!argv) {
                    JS_FreeValue(ctx, fn);
                    JS_FreeCString(ctx, method_s);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    goto cleanup;
                }
            }
            for (i = 0; i < len; i++)
                argv[i] = JS_GetPropertyUint32(ctx, params, i);
            result = JS_Call(ctx, fn, JS_UNDEFINED, len, argv);
            for (i = 0; i < len; i++)
                JS_FreeValue(ctx, argv[i]);
            if (argv != argv_stack)
                free(argv);
        } else {
            result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        }
    } else {
        JSValueConst argv[1] = { params };
        result = JS_Call(ctx, fn, JS_UNDEFINED, 1, argv);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeCString(ctx, method_s);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        dyn_app_send_rpc_throw(c, exc, id_s);
        JS_FreeValue(ctx, exc);
        goto cleanup;
    }
    if (JS_IsObject(result)) {
        JSValue then = JS_GetPropertyStr(ctx, result, "then");
        if (JS_IsFunction(ctx, then)) {
            dyn_app_pend_t* pd = (dyn_app_pend_t*)calloc(1, sizeof(*pd));
            if (pd) {
                JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
                JSValue promise, pthen, pobj, onres, onrej, tr;
                JSValueConst thenargs[2], settleargs[2];

                pd->conn = c;
                c->refs++;
                pd->id = strdup(id_s ? id_s : "null");
                pd->accept_gzip = c->accept_gzip;
                pd->identity_refused = c->identity_refused;
                pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
                if (JS_IsException(pobj)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_pend_release(pd, 0);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_SetOpaque(pobj, pd);
                promise = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(promise) || !pd->id) {
                    if (!JS_IsException(promise))
                        JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, pobj);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                onres = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 0, 1, &pobj);
                onrej = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 1, 1, &pobj);
                pthen = JS_GetPropertyStr(ctx, promise, "then");
                settleargs[0] = onres;
                settleargs[1] = onrej;
                tr = JS_IsException(pthen) ? pthen
                                           : JS_Call(ctx, pthen, promise, 2, settleargs);
                if (JS_IsException(tr)) {
                    JS_FreeValue(ctx, tr);
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, pthen);
                    JS_FreeValue(ctx, onres);
                    JS_FreeValue(ctx, onrej);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, pobj);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_FreeValue(ctx, tr);
                JS_FreeValue(ctx, pthen);
                JS_FreeValue(ctx, onres);
                JS_FreeValue(ctx, onrej);
                pd->counted = 1;
                c->resp_parked++;
                thenargs[0] = funcs[0];
                thenargs[1] = funcs[1];
                tr = JS_Call(ctx, then, result, 2, thenargs);
                if (JS_IsException(tr)) {
                    JSValue exc = JS_GetException(ctx);
                    JS_FreeValue(ctx, tr);
                    tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
                    JS_FreeValue(ctx, tr);
                    JS_FreeValue(ctx, exc);
                } else {
                    JS_FreeValue(ctx, tr);
                }
                JS_FreeValue(ctx, funcs[0]);
                JS_FreeValue(ctx, funcs[1]);
                JS_FreeValue(ctx, promise);
                JS_FreeValue(ctx, pobj);
                JS_FreeValue(ctx, then);
                JS_FreeValue(ctx, result);
                goto cleanup;
            }
        }
        JS_FreeValue(ctx, then);
    }
    dyn_app_send_rpc_result(c, result, id_s);
    JS_FreeValue(ctx, result);
cleanup:
    if (id_s)
        JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, methods);
}

static const char* dyn_app_content_type(const char* path)
{
    const char* d = strrchr(path, '.');
    if (!d)
        return "application/octet-stream";
    if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm"))
        return "text/html";
    if (!strcasecmp(d, ".js") || !strcasecmp(d, ".mjs"))
        return "text/javascript";
    if (!strcasecmp(d, ".css"))
        return "text/css";
    if (!strcasecmp(d, ".json"))
        return "application/json";
    if (!strcasecmp(d, ".txt"))
        return "text/plain";
    if (!strcasecmp(d, ".csv"))
        return "text/csv";
    if (!strcasecmp(d, ".png"))
        return "image/png";
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(d, ".gif"))
        return "image/gif";
    if (!strcasecmp(d, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(d, ".ico"))
        return "image/x-icon";
    if (!strcasecmp(d, ".wasm"))
        return "application/wasm";
    return "application/octet-stream";
}

static int dyn_app_allowed(const dyn_app_route_t* rt, const char* path)
{
    size_t k;
    const char *dot, *ct;
    if (!rt->allow)
        return 1;
    dot = strrchr(path, '.');
    ct = dyn_app_content_type(path);
    for (k = 0; k < rt->n_allow; k++) {
        if (dot && !strcasecmp(rt->allow[k], dot))
            return 1;
        if (!strcasecmp(rt->allow[k], ct))
            return 1;
    }
    return 0;
}

static void dyn_app_send_err(dyn_app_conn_t* c, int status, const char* msg)
{
    dyn_app_send_json(c, status, msg, strlen(msg));
}

static void dyn_app_send_err_allow(dyn_app_conn_t* c, int status,
    const char* msg, const char* allow)
{
    char extra[256];
    size_t al = strlen(allow);
    size_t ml = strlen(msg);
    int n;
    if (al > 200)
        al = 200;
    n = snprintf(extra, sizeof(extra), "Allow: %.*s\r\n", (int)al, allow);
    if (n < 0 || (size_t)n >= sizeof(extra))
        dyn_app_send_json(c, status, msg, ml);
    else
        dyn_app_send_body_x(c, status, "application/json", msg, ml, extra);
}

static int dyn_app_static_segments(const char* sub)
{
    const char* p = sub;
    while (*p) {
        const char* seg;
        size_t len;
        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        if (len == 0)
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return 403;
        if (seg[0] == '.' && !(len == 11 && memcmp(seg, ".well-known", 11) == 0))
            return 404;
    }
    return 0;
}

static int dyn_app_open_beneath(const char* root, const char* rel)
{
    int fd = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    const char* p = rel;
    char comp[NAME_MAX + 1];
    if (fd < 0)
        return -1;
    for (;;) {
        const char* seg;
        size_t len;
        int next, last;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        seg = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        if (len > NAME_MAX) {
            close(fd);
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(comp, seg, len);
        comp[len] = 0;
        {
            const char* q = p;
            while (*q == '/')
                q++;
            last = !*q;
        }
        next = openat(fd, comp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? 0 : O_DIRECTORY));
        {
            int saved = errno;
            close(fd);
            errno = saved;
        }
        if (next < 0)
            return -1;
        fd = next;
    }
    return fd;
}

static void dyn_app_serve_static(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* reqpath,
    const char* base, size_t head_len)
{
    const char* sub = reqpath + strlen(rt->path);
    char fpath[2048];
    struct stat st;
    int64_t cap;
    dyn_iobuf_t out;
    char head[512];
    int n, ffd;
    int head_only;
    int64_t rstart = -1, rend = -1;
    int range_unsat = 0;

    head_only = (head_len >= 5 && memcmp(base, "HEAD ", 5) == 0);

    {
        int seg_verdict = dyn_app_static_segments(sub);
        if (seg_verdict) {
            dyn_app_send_err(c, seg_verdict, seg_verdict == 403 ? "{\"error\":\"forbidden\"}" : "{\"error\":\"not found\"}");
            return;
        }
    }
    while (*sub == '/')
        sub++;
    snprintf(fpath, sizeof(fpath), "%s/%s", rt->dir, *sub ? sub : "index.html");

    {
        char rdir[PATH_MAX], rfile[PATH_MAX];
        size_t dl;
        if (!realpath(rt->dir, rdir) || !realpath(fpath, rfile)) {
            dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
            return;
        }
        dl = strlen(rdir);
        if (strncmp(rfile, rdir, dl) != 0 || (rfile[dl] != '/' && rfile[dl] != '\0')) {
            dyn_app_send_err(c, 403, "{\"error\":\"forbidden\"}");
            return;
        }
        ffd = dyn_app_open_beneath(rdir, rfile + dl);
    }
    if (ffd < 0) {
        dyn_app_send_err(c, errno == ELOOP ? 403 : (errno == ENOENT ? 404 : 500),
            errno == ELOOP ? "{\"error\":\"forbidden\"}"
                           : "{\"error\":\"not found\"}");
        return;
    }
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    cap = rt->max_file > 0 ? rt->max_file : (int64_t)(32 * 1024 * 1024);
    if ((int64_t)st.st_size > cap) {
        close(ffd);
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        return;
    }
    if (!dyn_app_allowed(rt, fpath)) {
        close(ffd);
        dyn_app_send_err(c, 403, "{\"error\":\"type not allowed\"}");
        return;
    }
    {
        size_t rv_len = 0;
        const char* rv = dyn_req_header(base, head_len, "range", &rv_len);
        if (rv && rv_len > 6 && strncasecmp(rv, "bytes=", 6) == 0 && !memchr(rv, ',', rv_len)) {
            size_t b = 6, e = rv_len;
            int64_t S = -1, E = -1;
            while (b < e && (rv[b] == ' ' || rv[b] == '\t'))
                b++;
            while (e > b && (rv[e - 1] == ' ' || rv[e - 1] == '\t'))
                e--;
            if (dyn_hm_one_range(rv, b, e, (int64_t)st.st_size, &S, &E) == 0) {
                rstart = S;
                rend = E;
            } else {
                range_unsat = 1;
            }
        }
    }
    if (range_unsat) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 416 Range Not Satisfiable\r\n"
            "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n"
            "Connection: keep-alive\r\n\r\n",
            (long long)st.st_size);
        close(ffd);
        if (n > 0) {
            dyn_iobuf_init(&out);
            dyn_iobuf_append(&out, head, (size_t)n);
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                    dyn_app_out_done, c)
                < 0)
                dyn_app_out_done(NULL, -ECONNRESET, NULL, 0, c);
            dyn_iobuf_free(&out);
            dyn_app_met_response(c, 0);
        }
        return;
    }
    if (rstart >= 0)
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
            "Connection: keep-alive\r\n\r\n",
            dyn_app_content_type(fpath), (long long)rstart,
            (long long)rend, (long long)st.st_size,
            (long long)(rend - rstart + 1));
    else
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
            "Connection: keep-alive\r\n\r\n",
            dyn_app_content_type(fpath), (long long)st.st_size);
    dyn_iobuf_init(&out);
    if (n > 0) {
        dyn_iobuf_append(&out, head, (size_t)n);
        c->refs++;
        if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                dyn_app_out_done, c)
            < 0)
            dyn_app_out_done(NULL, -ECONNRESET, NULL, 0, c);
    }
    dyn_iobuf_free(&out);
    if (head_only) {
        close(ffd);
        dyn_app_met_response(c, 0);
        return;
    }
    c->refs++;
    {
        int rc;
        if (rstart >= 0)
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, (off_t)rstart,
                (size_t)(rend - rstart + 1), dyn_app_out_last, c);
        else
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, 0, (size_t)st.st_size,
                dyn_app_out_last, c);
        if (rc < 0) {
            dyn_app_conn_close(c);
            dyn_app_conn_unref(c);
        }
    }
    dyn_app_met_response(c, (double)(rstart >= 0 ? rend - rstart + 1 : st.st_size));
}

static unsigned long dyn_app_upload_seq;

static void dyn_app_upload_finish(dyn_app_conn_t* c)
{
    dyn_app_upload_t* u = c->up;
    JSContext* ctx = c->app->ctx;
    JSValue pathv, meta, r;
    JSValueConst args[2];
    c->up = NULL;
    close(u->fd);
    pathv = JS_NewString(ctx, u->path);
    meta = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, meta, "size", JS_NewInt64(ctx, u->size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "contentType", JS_NewString(ctx, u->ctype), JS_PROP_C_W_E);
    args[0] = pathv;
    args[1] = meta;
    r = JS_Call(ctx, u->handler, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"handler failed\"}");
    } else {
        dyn_iobuf_t o;
        JSValue pj = JS_JSONStringify(ctx, pathv, JS_UNDEFINED, JS_UNDEFINED);
        const char* ps = JS_IsException(pj) ? NULL : JS_ToCString(ctx, pj);
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"ok\":true,\"path\":", 18);
        dyn_iobuf_append(&o, ps ? ps : "\"\"", strlen(ps ? ps : "\"\""));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char*)o.data, o.len);
        dyn_iobuf_free(&o);
        if (ps)
            JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, pj);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, pathv);
    JS_FreeValue(ctx, meta);
    JS_FreeValue(ctx, u->handler);
    free(u->path);
    free(u);
}

#define DYN_APP_UPLOAD_CHUNK (256u * 1024u)
#define DYN_APP_UPLOAD_MAXW 4

typedef struct dyn_app_upload_w {
    dyn_app_upload_t* u;
    dyn_app_conn_t* c;
    uint8_t* base;
    uint8_t* buf;
    int64_t off;
    size_t len;
} dyn_app_upload_w_t;

static void dyn_app_upload_wdone(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

static void dyn_app_upload_resume(dyn_app_conn_t* c)
{
    dyn_app_upload_t* u = c->up;
    struct dyn_aio* aio = c->app->aio;

    if (u->err) {
        dyn_app_conn_close(c);
        return;
    }
    while (u->remaining > 0 && u->wbusy < DYN_APP_UPLOAD_MAXW) {
        size_t take = dyn_iobuf_rlen(&c->in);
        dyn_app_upload_w_t* w;
        if (take == 0)
            return;
        if ((int64_t)take > u->remaining)
            take = (size_t)u->remaining;
        if (take > DYN_APP_UPLOAD_CHUNK)
            take = DYN_APP_UPLOAD_CHUNK;
        w = (dyn_app_upload_w_t*)malloc(sizeof(*w));
        if (w)
            w->base = (uint8_t*)malloc(take);
        if (!w || !w->base) {
            if (w)
                free(w->base);
            free(w);
            u->err = -ENOMEM;
            dyn_app_conn_close(c);
            return;
        }
        w->buf = w->base;
        memcpy(w->base, dyn_iobuf_rdata(&c->in), take);
        w->u = u;
        w->c = c;
        w->off = u->off;
        w->len = take;
        u->off += take;
        u->remaining -= take;
        dyn_iobuf_consume(&c->in, take);
        u->wbusy++;
        c->refs++;
        dyn_aio_write(aio, u->fd, w->buf, take, w->off,
            dyn_app_upload_wdone, w);
        if (!c->up || c->closed)
            return;
    }
    if (u->remaining == 0 && u->wbusy == 0)
        dyn_app_upload_finish(c);
}

static void dyn_app_upload_wdone(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_upload_w_t* w = (dyn_app_upload_w_t*)ud;
    dyn_app_upload_t* u = w->u;
    dyn_app_conn_t* c = w->c;
    (void)aio;
    (void)buf;
    (void)len;

    if (res == -EINTR) {
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                dyn_app_upload_wdone, w)
            == 0)
            return;
        res = -EIO;
    }
    if (res > 0 && (size_t)res < w->len) {
        w->buf += res;
        w->off += res;
        w->len -= (size_t)res;
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                dyn_app_upload_wdone, w)
            == 0)
            return;
        res = -EIO;
    }
    if (res == 0)
        res = -EIO;
    free(w->base);
    free(w);
    u->wbusy--;
    if (res < 0 && !u->err)
        u->err = res;
    if (c->closed) {
        dyn_app_conn_unref(c);
        return;
    }
    dyn_app_upload_resume(c);
    dyn_app_conn_unref(c);
}

static void dyn_app_upload_drain(dyn_app_conn_t* c)
{
    dyn_app_upload_resume(c);
}

#define DYN_PROXY_HEAD_MAX 8192

static int dyn_proxy_hop_by_hop(const char* n, size_t len)
{
    static const char* const hop[] = {
        "connection",
        "keep-alive",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
        "proxy-authenticate",
        "proxy-authorization",
        "content-length",
        "host",
        "x-forwarded-for",
        "x-forwarded-proto",
        "x-forwarded-host",
        "x-real-ip",
        "via",
    };
    size_t i;
    for (i = 0; i < countof(hop); i++)
        if (strlen(hop[i]) == len && dyn_ci_eq(n, hop[i], len))
            return 1;
    return 0;
}

typedef struct {
    dyn_app_conn_t* c;
    int fd;
    uint8_t* req;
    size_t req_len;
    dyn_iobuf_t resp;
    int sent;
} dyn_app_proxy_t;

static void dyn_proxy_fail(dyn_app_proxy_t* p, int status, const char* msg)
{
    dyn_app_conn_t* c = p->c;
    c->proxy = NULL;
    if (p->fd >= 0) {
        int fd = p->fd;
        p->fd = -1;
        dyn_aio_close(c->app->aio, fd);
    }
    if (!p->sent) {
        p->sent = 1;
        dyn_app_send_err(c, status, msg);
    }
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_unref(c);
    free(p);
}

typedef struct {
    size_t head_len;
    int have_te;
    int have_cl;
    int cl_ok;
    int64_t clen;
} dyn_proxy_frame_t;

static int dyn_proxy_scan_frame(const char* buf, size_t len,
    dyn_proxy_frame_t* f)
{
    size_t i, ls, first = 1;

    f->head_len = 0;
    f->have_te = 0;
    f->have_cl = 0;
    f->cl_ok = 0;
    f->clen = 0;
    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            f->head_len = i + 4;
            break;
        }
    if (!f->head_len)
        return 1;

    for (ls = 0; ls + 1 < f->head_len; first = 0) {
        size_t le = ls, colon, nlen;
        while (le + 1 < f->head_len && !(buf[le] == '\r' && buf[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        for (colon = ls; colon < le && buf[colon] != ':'; colon++)
            ;
        if (!first && colon < le) {
            const char* v = buf + colon + 1;
            size_t vlen = le - colon - 1;
            nlen = colon - ls;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vlen--;
            }
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
                vlen--;
            if (dyn_ci_equal(buf + ls, nlen, "transfer-encoding") && vlen) {
                f->have_te = 1;
                return 0;
            }
            if (dyn_ci_equal(buf + ls, nlen, "content-length")) {
                int64_t v2 = 0;
                f->have_cl = 1;
                if (vlen == 0)
                    return 0;
                for (i = 0; i < vlen; i++) {
                    if (v[i] < '0' || v[i] > '9')
                        return 0;
                    v2 = v2 * 10 + (v[i] - '0');
                    if (v2 > DYN_ACONN_MAX_REQ)
                        return 0;
                }
                if (f->cl_ok && v2 != f->clen) {
                    f->cl_ok = 0;
                    return 0;
                }
                f->clen = v2;
                f->cl_ok = 1;
            }
        }
        ls = le + 2;
    }
    return 0;
}

static int dyn_proxy_status_ok(const char* line, size_t len)
{
    if (len < 12)
        return 0;
    if (line[7] < '0' || line[7] > '9')
        return 0;
    if (line[8] != ' ')
        return 0;
    if (line[9] < '0' || line[9] > '9')
        return 0;
    if (line[10] < '0' || line[10] > '9')
        return 0;
    if (line[11] < '0' || line[11] > '9')
        return 0;
    if (len > 12 && line[12] != ' ')
        return 0;
    return 1;
}

static int dyn_proxy_status_digits(const char* buf, size_t len)
{
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0)
        return 0;
    if (buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' || buf[11] < '0' || buf[11] > '9')
        return 0;
    return (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
}

static int dyn_proxy_relay_response(dyn_app_proxy_t* p)
{
    dyn_app_conn_t* c = p->c;
    const char* buf = (const char*)p->resp.data;
    size_t len = p->resp.len, eol, body, n = 0;
    dyn_proxy_frame_t f;
    char out[DYN_PROXY_HEAD_MAX];
    int k, status, no_body;

    for (;;) {
        if (dyn_proxy_scan_frame(buf, len, &f))
            return 1;
        status = dyn_proxy_status_digits(buf, len);
        if (status >= 100 && status < 200 && status != 101) {
            memmove(p->resp.data, p->resp.data + f.head_len,
                p->resp.len - f.head_len);
            p->resp.len -= f.head_len;
            buf = (const char*)p->resp.data;
            len = p->resp.len;
            continue;
        }
        break;
    }

    if (f.have_te) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream Transfer-Encoding is "
                               "not proxied\"}");
        return 0;
    }
    if (f.have_cl && !f.cl_ok) {
        dyn_proxy_fail(p, 502, "{\"error\":\"bad upstream Content-Length\"}");
        return 0;
    }
    body = len - f.head_len;
    no_body = (status >= 100 && status < 200) || status == 204 || status == 304;
    if (!no_body && !c->head_only && f.have_cl && (int64_t)body != f.clen) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream body length does not "
                               "match Content-Length\"}");
        return 0;
    }

    for (eol = 0; eol + 1 < f.head_len; eol++)
        if (buf[eol] == '\r' && buf[eol + 1] == '\n')
            break;
    if (eol + 1 >= f.head_len || eol < 12 || memcmp(buf, "HTTP/1.", 7) != 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"bad upstream response\"}");
        return 0;
    }
    if (eol + 2 > sizeof(out) || dyn_hdr_value_ok(buf, eol) != 0 || !dyn_proxy_status_ok(buf, eol)) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream status line rejected\"}");
        return 0;
    }
    memcpy(out, buf, eol);
    n = eol;
    n += (size_t)snprintf(out + n, sizeof(out) - n, "\r\n");

    {
        size_t ls = eol + 2;
        while (ls + 1 < f.head_len) {
            size_t le = ls, colon;
            while (le + 1 < f.head_len && !(buf[le] == '\r' && buf[le + 1] == '\n'))
                le++;
            if (le == ls)
                break;
            for (colon = ls; colon < le && buf[colon] != ':'; colon++)
                ;
            if (colon < le) {
                if (dyn_hdr_name_ok(buf + ls, colon - ls) != 0 || dyn_hdr_value_ok(buf + colon, le - colon) != 0) {
                    dyn_proxy_fail(p, 502, "{\"error\":\"upstream header "
                                           "rejected\"}");
                    return 0;
                }
            }
            if (colon < le && !dyn_proxy_hop_by_hop(buf + ls, colon - ls)) {
                if (n + (le - ls) + 2 >= sizeof(out)) {
                    dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
                    return 0;
                }
                memcpy(out + n, buf + ls, le - ls);
                n += le - ls;
                out[n++] = '\r';
                out[n++] = '\n';
            }
            ls = le + 2;
        }
    }
    if (no_body) {
        k = snprintf(out + n, sizeof(out) - n, "Connection: close\r\n\r\n");
    } else if (c->head_only) {
        if (f.have_cl && f.cl_ok)
            k = snprintf(out + n, sizeof(out) - n,
                "Content-Length: %llu\r\nConnection: close\r\n\r\n",
                (unsigned long long)f.clen);
        else
            k = snprintf(out + n, sizeof(out) - n,
                "Connection: close\r\n\r\n");
    } else {
        k = snprintf(out + n, sizeof(out) - n,
            "Content-Length: %llu\r\nConnection: close\r\n\r\n",
            (unsigned long long)body);
    }
    if (k < 0 || n + (size_t)k >= sizeof(out)) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
        return 0;
    }
    n += (size_t)k;
    p->sent = 1;
    dyn_aio_send(c->app->aio, c->fd, out, n, 0, NULL, NULL);
    if (body && !no_body && !c->head_only)
        dyn_aio_send(c->app->aio, c->fd, buf + f.head_len, body, 0, NULL, NULL);

    {
        int fd = p->fd;
        p->fd = -1;
        if (fd >= 0)
            dyn_aio_close(c->app->aio, fd);
    }
    c->proxy = NULL;
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_close(c);
    dyn_app_conn_unref(c);
    free(p);
    return 0;
}

static int dyn_proxy_response_complete(dyn_app_proxy_t* p)
{
    dyn_proxy_frame_t f;
    int status;
    if (dyn_proxy_scan_frame((const char*)p->resp.data, p->resp.len, &f))
        return 0;
    status = dyn_proxy_status_digits((const char*)p->resp.data, p->resp.len);
    if (status >= 100 && status < 200 && status != 101)
        return 0;
    if (p->c->head_only)
        return 1;
    if (f.have_te || (f.have_cl && !f.cl_ok))
        return 1;
    if (!f.have_cl)
        return 0;
    return p->resp.len >= f.head_len + (size_t)f.clen;
}

static void dyn_proxy_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_proxy_t* p = (dyn_app_proxy_t*)udata;
    (void)aio;

    if (p->fd < 0)
        return;
    if (res < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream read failed\"}");
        return;
    }
    if (res == 0) {
        if (dyn_proxy_relay_response(p))
            dyn_proxy_fail(p, 502, "{\"error\":\"truncated upstream response\"}");
        return;
    }
    if (p->resp.len + len > (size_t)DYN_ACONN_MAX_REQ) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream response too large\"}");
        return;
    }
    if (dyn_iobuf_append(&p->resp, buf, len) < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"out of memory\"}");
        return;
    }
    if (dyn_proxy_response_complete(p))
        dyn_proxy_relay_response(p);
}

static void dyn_proxy_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_app_proxy_t* p = (dyn_app_proxy_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;

    if (res < 0 || p->fd < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
        return;
    }
    if (dyn_aio_send(p->c->app->aio, p->fd, p->req, p->req_len, 0, NULL, NULL) < 0 || dyn_aio_recv(p->c->app->aio, p->fd, 0, 1, dyn_proxy_on_recv, p) < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream write failed\"}");
}

static void dyn_app_proxy_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len,
    const char* path, int64_t clen)
{
    dyn_app_proxy_t* p;
    char head[DYN_PROXY_HEAD_MAX];
    size_t n = 0, ls, sp1 = 0, i, sub_len = 0;
    const char* sub;
    int k;

    for (i = 0; i < head_len && base[i] != ' '; i++)
        ;
    sp1 = i;
    if (sp1 == 0 || sp1 > 24 || !dyn_method_valid(base, sp1)) {
        dyn_app_send_err(c, 400, "{\"error\":\"bad method\"}");
        dyn_app_conn_close(c);
        return;
    }
    {
        const char* tstart = base + sp1 + 1;
        size_t tlen = 0, plen = strlen(rt->path);
        while (tstart + tlen < base + head_len && tstart[tlen] != ' '
            && tstart[tlen] != '\r')
            tlen++;
        if (plen > tlen)
            plen = tlen;
        sub = tstart + plen;
        sub_len = tlen - plen;
    }
    if (sub_len == 0) {
        sub = "/";
        sub_len = 1;
    }

    k = snprintf(head, sizeof(head), "%.*s %s%.*s HTTP/1.1\r\nHost: %s:%u\r\n",
        (int)sp1, base, sub[0] == '/' ? "" : "/", (int)sub_len, sub,
        rt->up_host, (unsigned)rt->up_port);
    if (k < 0 || (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n = (size_t)k;

    for (ls = 0; ls + 1 < head_len;) {
        if (base[ls] == '\r' && base[ls + 1] == '\n') {
            ls += 2;
            break;
        }
        ls++;
    }
    while (ls + 1 < head_len) {
        size_t le = ls, colon;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        for (colon = ls; colon < le && base[colon] != ':'; colon++)
            ;
        if (colon < le && dyn_hdr_name_ok(base + ls, colon - ls) != 0) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad header name\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (colon < le && !dyn_proxy_hop_by_hop(base + ls, colon - ls)) {
            if (n + (le - ls) + 2 >= sizeof(head)) {
                dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
                dyn_app_conn_close(c);
                return;
            }
            memcpy(head + n, base + ls, le - ls);
            n += le - ls;
            head[n++] = '\r';
            head[n++] = '\n';
        }
        ls = le + 2;
    }

    {
        char peer[INET6_ADDRSTRLEN];
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        snprintf(peer, sizeof peer, "%s", "127.0.0.1");
        if (getpeername(c->fd, (struct sockaddr*)&ss, &sl) == 0) {
            const void* ap = NULL;
            if (ss.ss_family == AF_INET6)
                ap = &((struct sockaddr_in6*)&ss)->sin6_addr;
            else if (ss.ss_family == AF_INET)
                ap = &((struct sockaddr_in*)&ss)->sin_addr;
            if (ap)
                inet_ntop(ss.ss_family, ap, peer, sizeof peer);
        }
        k = snprintf(head + n, sizeof(head) - n,
            "X-Forwarded-For: %s\r\nX-Forwarded-Proto: http\r\n"
            "Via: 1.1 dynajs\r\nContent-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            peer, (long long)clen);
    }
    if (k < 0 || n + (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n += (size_t)k;

    p = (dyn_app_proxy_t*)calloc(1, sizeof(*p));
    if (!p) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    p->c = c;
    p->fd = -1;
    dyn_iobuf_init(&p->resp);
    p->req_len = n + (size_t)clen;
    p->req = (uint8_t*)malloc(p->req_len ? p->req_len : 1);
    if (!p->req) {
        free(p);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    memcpy(p->req, head, n);
    if (clen > 0)
        memcpy(p->req + n, base + head_len, (size_t)clen);

    c->refs++;
    c->proxy = p;
    p->fd = dyn_aio_connect(c->app->aio, rt->up_host, rt->up_port,
        dyn_proxy_on_connect, p);
    if (p->fd < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
}

static void dyn_app_upload_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len, int64_t clen)
{
    JSContext* ctx = c->app->ctx;
    const char* ct;
    size_t ctlen = 0;
    int64_t cap = rt->max_file > 0 ? rt->max_file : (int64_t)(16 * 1024 * 1024);
    char ctbuf[128], fpath[2048];
    dyn_app_upload_t* u;
    int fd;

    if (clen < 0 || clen > cap) {
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    ct = dyn_req_header(base, head_len, "content-type", &ctlen);
    {
        size_t n = ctlen < sizeof(ctbuf) - 1 ? ctlen : sizeof(ctbuf) - 1;
        char* sc;
        memcpy(ctbuf, ct ? ct : "", n);
        ctbuf[n] = 0;
        if ((sc = strchr(ctbuf, ';')) != NULL)
            *sc = 0;
        {
            size_t cl = strlen(ctbuf);
            while (cl && ctbuf[cl - 1] == ' ')
                ctbuf[--cl] = 0;
        }
    }
    if (rt->allow) {
        size_t k;
        int ok = 0;
        for (k = 0; k < rt->n_allow; k++)
            if (!strcasecmp(rt->allow[k], ctbuf)) {
                ok = 1;
                break;
            }
        if (!ok) {
            dyn_app_send_err(c, 415, "{\"error\":\"type not allowed\"}");
            dyn_app_conn_close(c);
            return;
        }
    }
    fd = -1;
    for (int attempt = 0; attempt < 64 && fd < 0; attempt++) {
        snprintf(fpath, sizeof(fpath), "%s/up_%ld_%lu", rt->dir, (long)getpid(),
            ++dyn_app_upload_seq);
        fd = open(fpath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) {
        dyn_app_send_err(c, 500, "{\"error\":\"open failed\"}");
        dyn_app_conn_close(c);
        return;
    }
    u = (dyn_app_upload_t*)calloc(1, sizeof(*u));
    if (!u) {
        close(fd);
        dyn_app_conn_close(c);
        return;
    }
    u->fd = fd;
    u->remaining = clen;
    u->size = clen;
    u->handler = JS_DupValue(ctx, rt->handler);
    u->path = strdup(fpath);
    if (!u->path) {
        JS_FreeValue(ctx, u->handler);
        free(u);
        close(fd);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    snprintf(u->ctype, sizeof(u->ctype), "%s", ctbuf);
    c->up = u;
    dyn_iobuf_consume(&c->in, head_len);
    c->hdr_scan_from = 0;
    c->hdr_counted = 0;
    c->hdr_lines = 0;
    dyn_app_upload_drain(c);
}

static size_t dyn_req_header_count(const char* buf, size_t len)
{
    size_t i, n = 0;
    for (i = 0; i + 2 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] != '\r')
            n++;
    return n;
}

static int dyn_app_try_dyn(dyn_app_conn_t* c, const char* base, size_t head_len,
    const char* path, const char* body, size_t body_len);

static void dyn_app_process_loop(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    for (;;) {
        const char* base = (const char*)dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        size_t from = c->hdr_scan_from < avail ? c->hdr_scan_from : avail;
        const char* hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        size_t head_len, body_len, req_total, clv_len = 0, i, r;
        const char* cl;
        char path[1024];
        int64_t clen = 0;
        const dyn_app_route_t* route = NULL;
        dyn_reqinfo_t ri;

        if (dyn_app_out_blocked(c))
            return;

        if (!hdr_end) {
            size_t resume = c->hdr_counted >= 1 ? c->hdr_counted - 1 : 0;
            c->hdr_scan_from = avail >= 3 ? avail - 3 : 0;
            if (resume <= avail) {
                c->hdr_lines += (uint32_t)dyn_req_header_count(
                    base + resume, avail - resume);
                c->hdr_counted = avail;
            }
            if (avail > DYN_APP_MAX_HEADER || c->hdr_lines > DYN_APP_MAX_HEADER_COUNT) {
                dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
                dyn_app_conn_close(c);
            }
            return;
        }

        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src == DYN_HEAD_ERR_LINE) {
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
            if (src != 0 || ri.head_len == 0 || ri.head_len > (size_t)(hdr_end - base) + 4) {
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
        }
        head_len = ri.head_len;

        if (head_len > DYN_APP_MAX_HEADER || ri.n_lines > DYN_APP_MAX_HEADER_COUNT) {
            dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
            dyn_app_conn_close(c);
            return;
        }

        if (ri.has_te) {
            dyn_app_send_err(c, 501, "{\"error\":\"transfer-encoding unsupported\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.cl_count > 1) {
            dyn_app_send_err(c, 400, "{\"error\":\"duplicate content-length\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.http11 && ri.host_count != 1) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad host\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.dup) {
            dyn_app_send_err(c, 400, "{\"error\":\"duplicate header\"}");
            dyn_app_conn_close(c);
            return;
        }
        cl = ri.cl;
        clv_len = ri.cl_len;
        if (cl) {
            if (clv_len == 0) {
                dyn_app_send_err(c, 400, "{\"error\":\"bad content-length\"}");
                dyn_app_conn_close(c);
                return;
            }
            for (i = 0; i < clv_len; i++) {
                if (cl[i] < '0' || cl[i] > '9') {
                    dyn_app_send_err(c, 400, "{\"error\":\"bad content-length\"}");
                    dyn_app_conn_close(c);
                    return;
                }
                clen = clen * 10 + (cl[i] - '0');
                if (clen > (int64_t)(1LL << 40)) {
                    clen = (int64_t)(1LL << 40);
                    break;
                }
            }
        }

        if (!c->req_start_ms)
            c->req_start_ms = dyn_timer_now_ms();
        {
            c->accept_gzip = 0;
            c->identity_refused = 0;
            if (ri.ae) {
                int qg = dyn_ae_q(ri.ae, ri.ae_len, "gzip", 4);
                if (qg < 0)
                    qg = dyn_ae_q(ri.ae, ri.ae_len, "*", 1);
                c->accept_gzip = qg > 0;
                {
                    int qi = dyn_ae_q(ri.ae, ri.ae_len, "identity", 8);
                    if (qi < 0 && dyn_ae_q(ri.ae, ri.ae_len, "*", 1) == 0)
                        qi = 0;
                    c->identity_refused = (qi == 0);
                }
            }
        }

        {
            int keep = ri.http11;
            if (dyn_hdr_token(ri.conn, ri.conn_len, "close"))
                keep = 0;
            else if (dyn_hdr_token(ri.conn, ri.conn_len, "keep-alive"))
                keep = 1;
            c->close_after = !keep;
            c->http11 = ri.http11;
            c->head_only = ri.method_len == 4 && memcmp(ri.method, "HEAD", 4) == 0;
        }

        if (ri.bad_target) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad request\"}");
            dyn_app_conn_close(c);
            return;
        }
        {
            size_t plen = ri.target_len < sizeof(path) - 1
                ? ri.target_len
                : sizeof(path) - 1;
            size_t r2, w2;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
            for (r2 = 0, w2 = 0; r2 < plen; r2++) {
                path[w2++] = path[r2];
                if (path[r2] == '/')
                    while (r2 + 1 < plen && path[r2 + 1] == '/')
                        r2++;
            }
            path[w2] = '\0';
        }
        for (r = 0; r < app->n_routes; r++) {
            const dyn_app_route_t* rt = &app->routes[r];
            if (rt->type == APP_DYN)
                continue;
            if (rt->type == APP_STATIC || rt->type == APP_PROXY) {
                size_t plen = strlen(rt->path);
                int slash_end = plen > 0 && rt->path[plen - 1] == '/';
                if (strncmp(path, rt->path, plen) == 0
                    && (slash_end || path[plen] == '/' || path[plen] == '\0')) {
                    route = rt;
                    break;
                }
            } else if (strcmp(rt->path, path) == 0) {
                route = rt;
                break;
            }
        }

        if (clen > (int64_t)DYN_ACONN_MAX_REQ && !(route && route->type == APP_UPLOAD)) {
            dyn_app_send_err(c, 413, "{\"error\":\"payload too large\"}");
            dyn_app_conn_close(c);
            return;
        }

        if (route && route->type == APP_UPLOAD) {
            dyn_app_upload_start(c, route, base, head_len, clen);
            if (c->up || c->closed)
                return;
            continue;
        }

        body_len = clen > DYN_ACONN_MAX_REQ ? (size_t)DYN_ACONN_MAX_REQ + 1 : (size_t)clen;
        req_total = head_len + body_len;
        if (avail < req_total)
            return;

        if (!route) {
            int dyn_rc = dyn_app_try_dyn(c, base, head_len, path,
                base + head_len, body_len);
            if (dyn_rc == 0) {
                if (app->metrics_http && strcmp(path, "/metrics") == 0) {
                    size_t ml = 0;
                    char* m = dyn_metrics_c_scrape(&ml);
                    if (m) {
                        dyn_app_send_body(c, 200, "text/plain; version=0.0.4",
                            m, ml);
                        free(m);
                    } else {
                        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                    }
                } else if (app->metrics_http && strcmp(path, "/healthz") == 0) {
                    dyn_app_send_json(c, 200, "{\"ok\":true}", 11);
                } else {
                    dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
                }
            }
        } else if (route->type == APP_RPC) {
            {
                const char* rct = ri.ct;
                size_t rctlen = ri.ct_len;
                if (rct) {
                    char typebuf[64];
                    size_t n = rctlen < sizeof(typebuf) - 1 ? rctlen : sizeof(typebuf) - 1;
                    char* sc;
                    memcpy(typebuf, rct, n);
                    typebuf[n] = 0;
                    if ((sc = strchr(typebuf, ';')) != NULL)
                        *sc = 0;
                    {
                        size_t tlen = strlen(typebuf);
                        while (tlen && (typebuf[tlen - 1] == ' ' || typebuf[tlen - 1] == '\t'))
                            typebuf[--tlen] = 0;
                    }
                    if (strcasecmp(typebuf, "application/json") != 0) {
                        dyn_app_send_err(c, 415, "{\"error\":\"unsupported media type\"}");
                        dyn_app_conn_close(c);
                        return;
                    }
                }
            }
            dyn_app_dispatch_rpc(c, route->handler, base + head_len, body_len);
        } else if (route->type == APP_STATIC) {
            dyn_app_serve_static(c, route, path, base, head_len);
        } else if (route->type == APP_PROXY) {
            dyn_app_proxy_start(c, route, base, head_len, path, (int64_t)body_len);
        } else if (route->type == APP_WS) {
            int upgraded = dyn_app_ws_handshake(c, route, base, head_len);
            if (upgraded > 0) {
                dyn_iobuf_consume(&c->in, req_total);
                c->hdr_scan_from = 0;
                c->hdr_counted = 0;
                c->hdr_lines = 0;
                return;
            }
            if (upgraded < 0)
                dyn_app_send_err(c, 403, "{\"error\":\"websocket upgrade refused\"}");
            else
                dyn_app_send_err(c, 400, "{\"error\":\"expected websocket upgrade\"}");
        } else if (route->type == APP_SSE) {
            dyn_app_sse_handshake(c, route);
            dyn_iobuf_consume(&c->in, req_total);
            c->hdr_scan_from = 0;
            c->hdr_counted = 0;
            c->hdr_lines = 0;
            return;
        }
        dyn_iobuf_consume(&c->in, req_total);
        c->hdr_scan_from = 0;
        c->hdr_counted = 0;
        c->hdr_lines = 0;
        if (c->closed)
            return;
        if (c->resp_parked || c->st)
            return;
        if (c->close_after)
            return;
    }
}

static void dyn_app_process(dyn_app_conn_t* c)
{
    if (c->in_process || c->resp_parked || c->st)
        return;
    c->in_process = 1;
    c->refs++;
    dyn_app_process_loop(c);
    c->in_process = 0;
    dyn_app_conn_unref(c);
}

static void dyn_app_pump_resume(dyn_app_conn_t* c)
{
    if (c->closed || c->in_process || c->close_after || c->resp_parked || c->st || c->up || c->is_ws || c->is_sse)
        return;
    if (dyn_iobuf_rlen(&c->in) == 0)
        return;
    dyn_iobuf_ensure_nul(&c->in);
    dyn_app_process(c);
    if (!c->closed)
        dyn_iobuf_compact(&c->in);
}

static void dyn_app_conn_close(dyn_app_conn_t* c)
{
    if (c->closed)
        return;
    c->closed = 1;
    dyn_app_stream_abort(c);
    if (c->proxy) {
        dyn_app_proxy_t* px = (dyn_app_proxy_t*)c->proxy;
        px->sent = 1;
        dyn_proxy_fail(px, 0, "");
    }
    if (c->is_ws) {
        JSContext* ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->ws_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[3] = { c->ws_this,
                JS_NewInt32(ctx, c->close_code ? c->close_code : 1000),
                JS_NewStringLen(ctx, "", 0) };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 3, a);
            JS_FreeValue(ctx, a[2]);
        }
        JS_FreeValue(ctx, ch);
        if (c->ws_native)
            c->ws_native->conn = NULL;
    }
    if (c->is_sse) {
        JSContext* ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->sse_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[1] = { c->sse_this };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, ch);
        if (c->sse_native)
            c->sse_native->conn = NULL;
    }
    dyn_aio_close(c->app->aio, c->fd);
    dyn_app_conn_unref(c);
}

static void dyn_app_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_conn_t* c = (dyn_app_conn_t*)ud;
    (void)aio;
    if (res <= 0) {
        dyn_app_conn_close(c);
        return;
    }
    c->refs++;
    if (dyn_iobuf_append(&c->in, buf, len) < 0) {
        dyn_app_conn_close(c);
    } else if (dyn_iobuf_rlen(&c->in)
        > (c->up ? (size_t)DYN_APP_UP_IN_MAX : (size_t)DYN_ACONN_MAX_REQ)) {
        dyn_app_conn_close(c);
    } else {
        size_t before = dyn_iobuf_rlen(&c->in);
        if (c->up) {
            dyn_app_upload_drain(c);
            c->up_win_bytes += (int64_t)len;
            if (c->up_win_bytes >= DYN_APP_UP_RATE_BYTES) {
                c->up_win_bytes = 0;
                c->last_ms = dyn_timer_now_ms();
            }
            while (!c->closed && !c->up && dyn_iobuf_rlen(&c->in) > 0) {
                size_t had = dyn_iobuf_rlen(&c->in);
                dyn_iobuf_ensure_nul(&c->in);
                dyn_app_process(c);
                if (!c->closed && !c->up)
                    dyn_iobuf_compact(&c->in);
                if (dyn_iobuf_rlen(&c->in) >= had)
                    break;
            }
        } else if (c->is_ws) {
            dyn_app_ws_process(c);
        } else if (c->is_sse) {
            dyn_iobuf_reset(&c->in);
        } else {
            dyn_iobuf_ensure_nul(&c->in);
            dyn_app_process(c);
            if (!c->closed && c->is_ws)
                dyn_app_ws_process(c);
        }
        if (!c->closed) {
            dyn_iobuf_compact(&c->in);
            if (!c->up && dyn_iobuf_rlen(&c->in) < before)
                c->last_ms = dyn_timer_now_ms();
        }
    }
    dyn_app_conn_unref(c);
}

static void dyn_app_idle_sweep(void* arg)
{
    dyn_app_t* app = (dyn_app_t*)arg;
    uint64_t now = dyn_timer_now_ms();
    dyn_app_conn_t *c, *next;

    app->in_sweep = 1;
    c = (dyn_app_conn_t*)app->conns;
    if (c)
        c->refs++;
    while (c) {
        uint64_t idle_ms;
        next = (dyn_app_conn_t*)c->lnext;
        if (next)
            next->refs++;
        if (!c->closed)
            dyn_app_out_account(c);
        idle_ms = c->is_sse ? app->idle_ms * DYN_APP_SSE_IDLE_MULT
                            : app->idle_ms;
        if (!c->closed && idle_ms && now - c->last_ms >= idle_ms) {
            dyn_app_conn_unlink(c);
            dyn_app_conn_close(c);
        } else if (!c->closed && c->out_stalled) {
            if (c->out_q && c->out_q < c->stall_q) {
                c->stall_q = c->out_q;
                c->stall_ms = now;
            } else if (now - c->stall_ms >= DYN_APP_OUT_STALL_MS) {
                dyn_app_conn_unlink(c);
                dyn_app_conn_close(c);
            } else if (c->out_q < DYN_APP_OUT_LOW
                && app->out_bytes < DYN_APP_OUT_TOTAL) {
                dyn_app_out_unstall(c);
                dyn_app_pump_resume(c);
            }
        }
        dyn_app_conn_unref(c);
        c = next;
    }
    app->in_sweep = 0;
    if (!app->dispose_called && app->timers && app->idle_ms)
        app->sweep = dyn_timer_add(app->timers, now, 1000,
            dyn_app_idle_sweep, app);
}

static void dyn_app_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_t* app = (dyn_app_t*)ud;
    dyn_app_conn_t* c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (app->max_conns && app->n_conns >= app->max_conns) {
        dyn_aio_close(aio, res);
        return;
    }
    c = (dyn_app_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        dyn_aio_close(aio, res);
        return;
    }
    c->app = app;
    c->fd = res;
    c->refs = 1;
    c->ws_handlers = JS_UNDEFINED;
    c->ws_this = JS_UNDEFINED;
    c->sse_handlers = JS_UNDEFINED;
    c->sse_this = JS_UNDEFINED;
    dyn_iobuf_init(&c->ws_frag);
    dyn_iobuf_init(&c->in);
    c->last_ms = dyn_timer_now_ms();
    c->lnext = app->conns;
    if (app->conns)
        ((dyn_app_conn_t*)app->conns)->lprev = c;
    app->conns = c;
    app->n_conns++;
    dyn_aio_recv(aio, res, 0, 1, dyn_app_on_recv, c);
}

static void dyn_app_dispose(void* native)
{
    dyn_app_t* app = (dyn_app_t*)native;
    size_t i;
    while (app->conns) {
        dyn_app_conn_t* c = (dyn_app_conn_t*)app->conns;
        dyn_app_conn_unlink(c);
        dyn_app_conn_close(c);
    }
    if (app->timers) {
        if (app->in_sweep)
            app->timers_orphan = app->timers;
        else
            dyn_timers_free(app->timers);
        app->timers = NULL;
        app->idle_ms = 0;
    }
    if (app->aio) {
        if (!app->in_sweep)
            dyn_net_off_drain(app);
        if (app->listen_fd >= 0)
            dyn_aio_close(app->aio, app->listen_fd);
        dyn_net_reactor_release(app->ctx);
    }
    for (i = 0; i < app->n_routes; i++) {
        size_t k;
        free(app->routes[i].path);
        free(app->routes[i].dir);
        free(app->routes[i].up_host);
        free(app->routes[i].dyn_method);
        free(app->routes[i].dyn_pattern);
        for (k = 0; k < app->routes[i].n_allow; k++)
            free(app->routes[i].allow[k]);
        free(app->routes[i].allow);
        JS_FreeValue(app->ctx, app->routes[i].handler);
    }
    free(app->routes);
    app->routes = NULL;
    app->n_routes = 0;
    for (i = 0; i < app->n_mw; i++)
        JS_FreeValue(app->ctx, app->mw[i]);
    free(app->mw);
    app->mw = NULL;
    app->n_mw = 0;
    app->cap_mw = 0;
    free(app->listen_host);
    app->dispose_called = 1;
    if (app->n_conns == 0 && !app->in_sweep)
        free(app);
}

static void dyn_app_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_app_class_id);
    dyn_app_t* app = (r && !r->closed) ? (dyn_app_t*)r->native : NULL;
    size_t i;

    if (!app)
        return;
    for (i = 0; i < app->n_routes; i++)
        JS_MarkValue(rt, app->routes[i].handler, mark_func);
    for (i = 0; i < app->n_mw; i++)
        JS_MarkValue(rt, app->mw[i], mark_func);
}

static const JSClassDef dyn_app_class = {
    "App",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_app_gc_mark,
};

static JSValue dyn_app_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    int64_t port = 0;
    int64_t workers = 0, backlog = 0;
    char* host_dup = NULL;
    int64_t idle_ms = 30000;
    int32_t max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
    int compress_opt = 1;
    int metrics_opt = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        if (dyn_opts_strict(ctx, argv[0], http_app_keys, 8))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &port, v))
                return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &idle_ms, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (idle_ms < 0)
                idle_ms = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt32(ctx, &max_conns, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (max_conns < 0)
                max_conns = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* h = JS_ToCString(ctx, v);
            if (!h) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(h);
            JS_FreeCString(ctx, h);
            if (!host_dup) {
                JS_FreeValue(ctx, v);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "compress");
        if (!JS_IsUndefined(v)) {
            compress_opt = JS_ToBool(ctx, v);
            if (compress_opt < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "metrics");
        if (!JS_IsUndefined(v)) {
            metrics_opt = JS_ToBool(ctx, v);
            if (metrics_opt < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if ((int64_t)d != d || d < 1 || d > 64) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "App: workers must be an integer from 1 to 64 (the same "
                    "range the thread-pool HTTPServer accepts)");
            }
            workers = (int64_t)d;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if ((int64_t)d != d || d < 1 || d > 65535) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "App: backlog must be an integer from 1 to 65535");
            }
            backlog = (int64_t)d;
        }
        JS_FreeValue(ctx, v);
    }
    app = (dyn_app_t*)calloc(1, sizeof(*app));
    if (!app)
        return JS_ThrowOutOfMemory(ctx);
    app->ctx = ctx;
    app->listen_fd = -1;
    app->port = (uint16_t)port;
    app->idle_ms = (uint64_t)idle_ms;
    app->max_conns = max_conns;
    app->compress = compress_opt;
    app->metrics_http = metrics_opt;
    app->listen_host = host_dup;
    app->workers = (int)workers;
    app->backlog = (int)backlog;
    app->aio = dyn_net_reactor_acquire(ctx);
    if (!app->aio) {
        free(host_dup);
        free(app);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_app_class_id, app, dyn_app_dispose);
}

static JSValue dyn_app_rpc(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "rpc(path, methods) requires a methods object");
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_RPC;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static int dyn_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char* dyn_pct_decode(const char* s, size_t n, int plus_space, size_t* outn)
{
    char* o = (char*)malloc(n + 1);
    size_t w = 0, i = 0;
    if (!o)
        return NULL;
    while (i < n) {
        if (s[i] == '%' && i + 2 < n + 1) {
            int h, l;
            if (i + 2 < n && ((h = dyn_hexval(s[i + 1])) >= 0) && ((l = dyn_hexval(s[i + 2])) >= 0)) {
                o[w++] = (char)((h << 4) | l);
                i += 3;
                continue;
            }
            o[w++] = s[i++];
        } else if (plus_space && s[i] == '+') {
            o[w++] = ' ';
            i++;
        } else {
            o[w++] = s[i++];
        }
    }
    o[w] = '\0';
    if (outn)
        *outn = w;
    return o;
}

static int dyn_is_pname(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int dyn_app_dyn_validate(JSContext* ctx, const char* pat)
{
    size_t n, seg_start;
    int wild = 0;
    char names[16][64];
    int nnames = 0;
    if (!pat || !*pat)
        return JS_ThrowTypeError(ctx, "dynamic route pattern must be a non-empty string"), -1;
    n = strlen(pat);
    if (n > 1024)
        return JS_ThrowTypeError(ctx, "dynamic route pattern exceeds 1024 bytes"), -1;
    if (pat[0] != '/')
        return JS_ThrowTypeError(ctx, "dynamic route pattern must start with '/'"), -1;
    if (strchr(pat, '?') || strchr(pat, '#'))
        return JS_ThrowTypeError(ctx, "dynamic route pattern is a path only (no query or fragment)"), -1;
    seg_start = 1;
    while (1) {
        size_t seg_end = seg_start;
        while (seg_end < n && pat[seg_end] != '/')
            seg_end++;
        {
            size_t slen = seg_end - seg_start;
            if (slen == 0 && seg_end < n) {
                JS_ThrowTypeError(ctx, "dynamic route pattern has an empty segment");
                return -1;
            }
            if (slen > 0) {
                const char* seg = pat + seg_start;
                if (seg[0] == ':') {
                    size_t k;
                    if (slen < 2) {
                        JS_ThrowTypeError(ctx, "dynamic route ':param' needs a name");
                        return -1;
                    }
                    for (k = 1; k < slen; k++)
                        if (!dyn_is_pname(seg[k])) {
                            JS_ThrowTypeError(ctx, "dynamic route ':param' names match [A-Za-z0-9_]+");
                            return -1;
                        }
                    if (slen - 1 >= sizeof names[0]) {
                        JS_ThrowTypeError(ctx, "dynamic route param name too long");
                        return -1;
                    }
                    for (k = 0; k < (size_t)nnames; k++)
                        if (strlen(names[k]) == slen - 1 && !memcmp(names[k], seg + 1, slen - 1)) {
                            JS_ThrowTypeError(ctx, "dynamic route has a duplicate param name");
                            return -1;
                        }
                    if (nnames >= 16) {
                        JS_ThrowTypeError(ctx,
                            "dynamic route has more than 16 named captures");
                        return -1;
                    }
                    memcpy(names[nnames], seg + 1, slen - 1);
                    names[nnames][slen - 1] = '\0';
                    nnames++;
                    if (wild) {
                        JS_ThrowTypeError(ctx, "dynamic route wildcard must be the last segment");
                        return -1;
                    }
                } else if (seg[0] == '*') {
                    size_t k;
                    if (wild) {
                        JS_ThrowTypeError(ctx, "dynamic route has more than one wildcard");
                        return -1;
                    }
                    if (slen < 2) {
                        JS_ThrowTypeError(ctx, "dynamic route '*rest' needs a name");
                        return -1;
                    }
                    for (k = 1; k < slen; k++)
                        if (!dyn_is_pname(seg[k])) {
                            JS_ThrowTypeError(ctx, "dynamic route '*rest' names match [A-Za-z0-9_]+");
                            return -1;
                        }
                    for (k = 0; k < (size_t)nnames; k++)
                        if (strlen(names[k]) == slen - 1 && !memcmp(names[k], seg + 1, slen - 1)) {
                            JS_ThrowTypeError(ctx, "dynamic route has a duplicate param name");
                            return -1;
                        }
                    if (seg_end < n) {
                        JS_ThrowTypeError(ctx, "dynamic route wildcard must be the last segment");
                        return -1;
                    }
                    wild = 1;
                } else {
                    size_t k;
                    for (k = 0; k < slen; k++)
                        if (seg[k] == ':' || seg[k] == '*') {
                            JS_ThrowTypeError(ctx, "dynamic route ':' and '*' start a segment");
                            return -1;
                        }
                }
            }
        }
        if (seg_end >= n)
            break;
        seg_start = seg_end + 1;
    }
    return 0;
}

static int dyn_app_dyn_match(JSContext* ctx, const char* pat, const char* path,
    JSValue params)
{
    const char *pp = pat, *qp = path;
    while (1) {
        const char *pe, *qe;
        size_t plen, qlen, qrem;
        while (*pp == '/')
            pp++;
        while (*qp == '/')
            qp++;
        if (!*pp && !*qp)
            return 1;
        if (!*pp || !*qp) {
            if (*pp == '*' || (pp[0] == '*')) {
                const char* nm = pp + 1;
                size_t nl = strlen(nm);
                char* dec;
                size_t dn;
                if (nl == 0)
                    return 0;
                dec = dyn_pct_decode("", 0, 0, &dn);
                if (!dec) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                {
                    JSValue v = JS_NewStringLen(ctx, dec, dn);
                    free(dec);
                    if (JS_IsException(v))
                        return -1;
                    if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                            JS_PROP_C_W_E)
                        < 0)
                        return -1;
                }
                return 1;
            }
            return 0;
        }
        pe = strchr(pp, '/');
        qe = strchr(qp, '/');
        plen = pe ? (size_t)(pe - pp) : strlen(pp);
        qrem = strlen(qp);
        qlen = qe ? (size_t)(qe - qp) : qrem;
        if (pp[0] == ':') {
            char nm[64];
            char* dec;
            size_t dn;
            if (qlen == 0)
                return 0;
            if (plen - 1 >= sizeof nm)
                return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            dec = dyn_pct_decode(qp, qlen, 0, &dn);
            if (!dec) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v))
                    return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                        JS_PROP_C_W_E)
                    < 0)
                    return -1;
            }
        } else if (pp[0] == '*') {
            char nm[64];
            size_t rlen = qrem;
            char* dec;
            size_t dn;
            if (plen - 1 >= sizeof nm)
                return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            dec = dyn_pct_decode(qp, rlen, 0, &dn);
            if (!dec) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v))
                    return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                        JS_PROP_C_W_E)
                    < 0)
                    return -1;
            }
            return 1;
        } else {
            if (plen != qlen || memcmp(pp, qp, plen) != 0)
                return 0;
        }
        pp += plen;
        qp += qlen;
    }
}

static void dyn_app_req_method(const char* base, char* out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap && base[i] && base[i] != ' ' && base[i] != '\r') {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

static void dyn_app_req_target(const char* base, size_t head_len,
    const char** tgt, size_t* tlen)
{
    const char* s = strchr(base, ' ');
    const char* e;
    if (!s) {
        *tgt = "";
        *tlen = 0;
        return;
    }
    s++;
    e = strchr(s, ' ');
    if (!e || (size_t)(e - base) > head_len) {
        *tgt = "";
        *tlen = 0;
        return;
    }
    *tgt = s;
    *tlen = (size_t)(e - s);
}

static JSValue dyn_app_build_query(JSContext* ctx, const char* qs, size_t qn)
{
    JSValue o = JS_NewObject(ctx);
    size_t i = 0;
    if (JS_IsException(o))
        return o;
    while (i < qn) {
        size_t ks = i, ke, vs, ve;
        while (i < qn && qs[i] != '&')
            i++;
        ke = i;
        if (i < qn && qs[i] == '&')
            i++;
        vs = ke;
        ve = ke;
        {
            size_t eq = ks;
            while (eq < ke && qs[eq] != '=')
                eq++;
            if (eq < ke) {
                vs = eq + 1;
                ve = ke;
                ke = eq;
            } else {
                vs = ve = ke;
            }
        }
        if (ke > ks) {
            char* kdec = dyn_pct_decode(qs + ks, ke - ks, 1, NULL);
            char* vdec = dyn_pct_decode(qs + vs, ve - vs, 1, NULL);
            if (!kdec || !vdec) {
                free(kdec);
                free(vdec);
                JS_FreeValue(ctx, o);
                return JS_ThrowOutOfMemory(ctx);
            }
            {
                JSValue v = JS_NewString(ctx, vdec);
                int rc;
                free(vdec);
                if (JS_IsException(v)) {
                    free(kdec);
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                }
                rc = JS_DefinePropertyValueStr(ctx, o, kdec, v, JS_PROP_C_W_E);
                free(kdec);
                if (rc < 0) {
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                }
            }
        }
    }
    return o;
}

static JSValue dyn_app_build_headers(JSContext* ctx, const char* base, size_t head_len)
{
    JSValue o = JS_NewObject(ctx);
    size_t ls, le;
    if (JS_IsException(o))
        return o;
    ls = 0;
    while (ls + 1 < head_len && !(base[ls] == '\r' && base[ls + 1] == '\n'))
        ls++;
    ls += 2;
    while (ls + 1 < head_len) {
        size_t colon, vs, ve, ns, ne;
        char nbuf[128];
        size_t k;
        le = ls;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le + 1] == '\n'))
            le++;
        if (le == ls)
            break;
        colon = ls;
        while (colon < le && base[colon] != ':')
            colon++;
        if (colon >= le) {
            ls = le + 2;
            continue;
        }
        ns = ls;
        ne = colon;
        while (ne > ns && (base[ne - 1] == ' ' || base[ne - 1] == '\t'))
            ne--;
        while (ns < ne && (base[ns] == ' ' || base[ns] == '\t'))
            ns++;
        vs = colon + 1;
        ve = le;
        while (vs < ve && (base[vs] == ' ' || base[vs] == '\t'))
            vs++;
        while (ve > vs && (base[ve - 1] == ' ' || base[ve - 1] == '\t'))
            ve--;
        if (ne - ns >= sizeof nbuf) {
            ls = le + 2;
            continue;
        }
        for (k = 0; k < ne - ns; k++) {
            unsigned char c = (unsigned char)base[ns + k];
            nbuf[k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        nbuf[ne - ns] = '\0';
        {
            JSValue cur = JS_GetPropertyStr(ctx, o, nbuf);
            JSValue nv = JS_NewStringLen(ctx, base + vs, ve - vs);
            if (JS_IsException(nv)) {
                JS_FreeValue(ctx, cur);
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
            if (JS_IsString(cur)) {
                size_t a, b;
                const char* as = JS_ToCStringLen(ctx, &a, cur);
                const char* bs = JS_ToCStringLen(ctx, &b, nv);
                const char* sep = ", ";
                size_t sepl = 2;
                char* joined;
                JSValue jv;
                if (!strcmp(nbuf, "cookie")) {
                    sep = "; ";
                    sepl = 2;
                }
                JS_FreeValue(ctx, cur);
                JS_FreeValue(ctx, nv);
                if (!strcmp(nbuf, "set-cookie")) {
                    if (as)
                        JS_FreeCString(ctx, as);
                    if (bs)
                        JS_FreeCString(ctx, bs);
                } else if (!as || !bs) {
                    if (as)
                        JS_FreeCString(ctx, as);
                    if (bs)
                        JS_FreeCString(ctx, bs);
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                } else {
                    joined = (char*)malloc(a + sepl + b + 1);
                    if (!joined) {
                        JS_FreeCString(ctx, as);
                        JS_FreeCString(ctx, bs);
                        JS_FreeValue(ctx, o);
                        return JS_ThrowOutOfMemory(ctx);
                    }
                    memcpy(joined, as, a);
                    memcpy(joined + a, sep, sepl);
                    memcpy(joined + a + sepl, bs, b);
                    joined[a + sepl + b] = '\0';
                    JS_FreeCString(ctx, as);
                    JS_FreeCString(ctx, bs);
                    jv = JS_NewString(ctx, joined);
                    free(joined);
                    if (JS_IsException(jv)) {
                        JS_FreeValue(ctx, o);
                        return JS_EXCEPTION;
                    }
                    if (JS_DefinePropertyValueStr(ctx, o, nbuf, jv, JS_PROP_C_W_E) < 0) {
                        JS_FreeValue(ctx, o);
                        return JS_EXCEPTION;
                    }
                }
            } else {
                JS_FreeValue(ctx, cur);
                if (JS_DefinePropertyValueStr(ctx, o, nbuf, nv, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                }
            }
        }
        ls = le + 2;
    }
    return o;
}

static int dyn_value_is_bytes(JSContext* ctx, JSValueConst v)
{
    size_t n = 0;
    uint8_t* p;
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE)
        return 0;
    p = JS_GetArrayBuffer(ctx, &n, v);
    if (p)
        return 1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            JS_FreeValue(ctx, ab);
            return bpe == 1;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return 0;
}

static int dyn_bytes_to_c(JSContext* ctx, JSValueConst v,
    const uint8_t** pp, size_t* pn,
    JSValue* keep)
{
    size_t n = 0;
    uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
    if (p) {
        *pp = p;
        *pn = n;
        *keep = JS_UNDEFINED;
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, absz;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return -1;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &absz, ab);
        *keep = ab;
        if (!base) {
            JS_FreeValue(ctx, ab);
            *keep = JS_UNDEFINED;
            return -1;
        }
        if (off > absz || len > absz - off) {
            JS_FreeValue(ctx, ab);
            *keep = JS_UNDEFINED;
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
}

static void dyn_app_dyn_send(dyn_app_conn_t* c, JSValueConst val);

static void dyn_app_dyn_send_json_c(dyn_app_conn_t* c, JSContext* ctx, JSValueConst v)
{
    JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jstr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"unserializable response\"}");
        return;
    }
    {
        size_t n = 0;
        const char* s = JS_ToCStringLen(ctx, &n, jstr);
        JS_FreeValue(ctx, jstr);
        if (!s) {
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        dyn_app_send_body(c, 200, "application/json", s, n);
        JS_FreeCString(ctx, s);
    }
}

static void dyn_app_dyn_send(dyn_app_conn_t* c, JSValueConst val)
{
    JSContext* ctx = c->app->ctx;
    if (JS_IsString(val)) {
        size_t n = 0;
        const char* s = JS_ToCStringLen(ctx, &n, val);
        if (!s) {
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        dyn_app_send_body(c, 200, "text/plain", s, n);
        JS_FreeCString(ctx, s);
        return;
    }
    if (dyn_value_is_bytes(ctx, val)) {
        const uint8_t* p = NULL;
        size_t n = 0;
        JSValue keep = JS_UNDEFINED;
        if (dyn_bytes_to_c(ctx, val, &p, &n, &keep) == 0) {
            dyn_app_send_body(c, 200, "application/octet-stream",
                (const char*)p, n);
            JS_FreeValue(ctx, keep);
        } else {
            JS_FreeValue(ctx, keep);
            dyn_app_send_err(c, 500, "{\"error\":\"bad bytes response\"}");
        }
        return;
    }
    if (JS_IsObject(val)) {
        JSValue has_status = dyn_app_own_get(ctx, val, "status");
        JSValue has_body = dyn_app_own_get(ctx, val, "body");
        int is_env = 0;
        int status = 200;
        if (!JS_IsException(has_status) && !JS_IsUndefined(has_status) && JS_IsNumber(has_status))
            is_env = 1;
        if (!JS_IsException(has_body) && !JS_IsUndefined(has_body))
            is_env = 1;
        if (!is_env) {
            JS_FreeValue(ctx, has_status);
            JS_FreeValue(ctx, has_body);
            dyn_app_dyn_send_json_c(c, ctx, val);
            return;
        }
        if (!JS_IsUndefined(has_status)) {
            int32_t st = 200;
            if (JS_ToInt32(ctx, &st, has_status) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, has_status);
                JS_FreeValue(ctx, has_body);
                dyn_app_send_err(c, 500, "{\"error\":\"bad response status\"}");
                return;
            }
            status = st;
            if (status < 100 || status > 599) {
                JS_FreeValue(ctx, has_status);
                JS_FreeValue(ctx, has_body);
                dyn_app_send_err(c, 500, "{\"error\":\"bad response status\"}");
                return;
            }
        }
        JS_FreeValue(ctx, has_status);
        {
            JSValue ctv = dyn_app_own_get(ctx, val, "contentType");
            char ct_owned[DYN_HTTP_CT_MAX + 1];
            const char* ct = "application/json";
            int ct_set = 0;
            if (!JS_IsException(ctv) && JS_IsString(ctv)) {
                size_t cn = 0;
                const char* cs = JS_ToCStringLen(ctx, &cn, ctv);
                if (cs) {
                    int bad = dyn_hdr_value_ok(cs, cn) != 0;
                    if (!bad && cn > DYN_HTTP_CT_MAX)
                        bad = 2;
                    if (bad) {
                        JS_FreeCString(ctx, cs);
                        JS_FreeValue(ctx, ctv);
                        JS_FreeValue(ctx, has_body);
                        dyn_app_send_err(c, 500, bad == 2 ? "{\"error\":\"response contentType too long\"}" : "{\"error\":\"response contentType must not "
                                                                                                              "contain control characters\"}");
                        return;
                    }
                    memcpy(ct_owned, cs, cn);
                    ct_owned[cn] = '\0';
                    ct = ct_owned;
                    ct_set = 1;
                    JS_FreeCString(ctx, cs);
                }
            }
            JS_FreeValue(ctx, ctv);
            if (JS_IsUndefined(has_body)) {
                JS_FreeValue(ctx, has_body);
                dyn_app_send_body(c, status, ct, "", 0);
                return;
            }
            if (JS_IsString(has_body)) {
                size_t n = 0;
                const char* s = JS_ToCStringLen(ctx, &n, has_body);
                const char* use_ct = ct_set ? ct : "text/plain";
                JS_FreeValue(ctx, has_body);
                if (!s) {
                    dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                    return;
                }
                dyn_app_send_body(c, status, use_ct, s, n);
                JS_FreeCString(ctx, s);
                return;
            }
            if (dyn_value_is_bytes(ctx, has_body)) {
                const uint8_t* p = NULL;
                size_t n = 0;
                JSValue keep = JS_UNDEFINED;
                const char* use_ct = ct_set ? ct : "application/octet-stream";
                if (dyn_bytes_to_c(ctx, has_body, &p, &n, &keep) == 0) {
                    dyn_app_send_body(c, status, use_ct, (const char*)p, n);
                    JS_FreeValue(ctx, keep);
                } else {
                    JS_FreeValue(ctx, keep);
                    dyn_app_send_err(c, 500, "{\"error\":\"bad response body\"}");
                }
                JS_FreeValue(ctx, has_body);
                return;
            }
            {
                JSValue jstr = JS_JSONStringify(ctx, has_body, JS_UNDEFINED, JS_UNDEFINED);
                JS_FreeValue(ctx, has_body);
                if (JS_IsException(jstr)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_send_err(c, 500, "{\"error\":\"unserializable response body\"}");
                    return;
                }
                {
                    size_t n = 0;
                    const char* s = JS_ToCStringLen(ctx, &n, jstr);
                    JS_FreeValue(ctx, jstr);
                    if (!s) {
                        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                        return;
                    }
                    dyn_app_send_body(c, status, ct, s, n);
                    JS_FreeCString(ctx, s);
                }
                return;
            }
        }
    }
    if (JS_IsUndefined(val)) {
        dyn_app_send_err(c, 500, "{\"error\":\"handler returned undefined\"}");
        return;
    }
    if (JS_IsNull(val)) {
        dyn_app_send_body(c, 200, "application/json", "null", 4);
        return;
    }
    dyn_app_dyn_send_json_c(c, ctx, val);
}

static void dyn_app_dyn_err_json_n(dyn_app_conn_t* c, int status,
    const char* msg, size_t msg_len)
{
    char buf[256];
    size_t pos = 0;
    buf[pos++] = '{';
    buf[pos++] = '"';
    buf[pos++] = 'e';
    buf[pos++] = 'r';
    buf[pos++] = 'r';
    buf[pos++] = 'o';
    buf[pos++] = 'r';
    buf[pos++] = '"';
    buf[pos++] = ':';
    buf[pos++] = '"';
    dyn_json_escape_n_into(buf, sizeof buf - 3, &pos,
        msg ? msg : "Server error",
        msg ? msg_len : strlen("Server error"));
    buf[pos++] = '"';
    buf[pos++] = '}';
    dyn_app_send_json(c, status, buf, pos);
}

static JSValue dyn_app_dyn_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    dyn_app_pend_t* pd;
    dyn_app_conn_t* c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd)
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        if (magic) {
            const char* em = NULL;
            size_t em_len = 0;
            JSValue m = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "message") : JS_UNDEFINED;
            if (JS_IsString(m))
                em = JS_ToCStringLen(ctx, &em_len, m);
            JS_FreeValue(ctx, m);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
        } else {
            dyn_app_dyn_send(c, v);
        }
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

static void dyn_app_dyn_send_or_await(dyn_app_conn_t* c, JSValue res)
{
    JSContext* ctx = c->app->ctx;
    int thenable = 0;
    if (JS_IsObject(res)) {
        JSValue th = JS_GetPropertyStr(ctx, res, "then");
        thenable = JS_IsFunction(ctx, th);
        JS_FreeValue(ctx, th);
    }
    if (!thenable) {
        dyn_app_dyn_send(c, res);
        JS_FreeValue(ctx, res);
        return;
    }
    {
        dyn_app_pend_t* pd = (dyn_app_pend_t*)calloc(1, sizeof *pd);
        JSValue pobj, thenv, onres, onrej;
        if (!pd) {
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        pd->conn = c;
        c->refs++;
        pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
        if (JS_IsException(pobj)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_app_pend_release(pd, 0);
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        JS_SetOpaque(pobj, pd);
        onres = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 0, 1, &pobj);
        onrej = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 1, 1, &pobj);
        if (JS_IsException(onres) || JS_IsException(onrej)) {
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        thenv = JS_GetPropertyStr(ctx, res, "then");
        if (JS_IsException(thenv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj);
            dyn_app_send_err(c, 500, "{\"error\":\"bad thenable\"}");
            return;
        }
        {
            JSValueConst a[2] = { onres, onrej };
            JSValue r;
            pd->counted = 1;
            c->resp_parked++;
            r = JS_Call(ctx, thenv, res, 2, a);
            JS_FreeValue(ctx, thenv);
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                dyn_app_pend_t* p2 = dyn_app_pend_claim(pobj);
                if (p2) {
                    if (!p2->conn->closed)
                        dyn_app_send_json(p2->conn, 500,
                            "{\"error\":\"Server error\"}", 24);
                    dyn_app_pend_release(p2, 0);
                }
            } else {
                JS_FreeValue(ctx, r);
            }
            JS_FreeValue(ctx, pobj);
        }
    }
}

static void dyn_app_dispatch_dyn(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    JSValue ctx_obj)
{
    JSContext* ctx = c->app->ctx;
    dyn_app_t* app = c->app;
    size_t i;
    JSValue handler = JS_DupValue(ctx, rt->handler);
    for (i = 0; i < app->n_mw; i++) {
        JSValueConst a[1] = { ctx_obj };
        JSValue r = JS_Call(ctx, app->mw[i], JS_UNDEFINED, 1, a);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char* em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, handler);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
            return;
        }
        if (JS_IsObject(r)) {
            JSValue th = JS_GetPropertyStr(ctx, r, "then");
            int is_then = JS_IsFunction(ctx, th);
            JS_FreeValue(ctx, th);
            if (is_then) {
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 500, "{\"error\":\"async middleware not supported\"}");
                return;
            }
        }
        if (JS_IsObject(r)) {
            JSValue rv = dyn_app_own_get(ctx, r, "response");
            int has = !JS_IsException(rv) && !JS_IsUndefined(rv);
            JS_FreeValue(ctx, r);
            if (JS_IsException(rv)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 500, "{\"error\":\"middleware response read failed\"}");
                return;
            }
            if (has) {
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_dyn_send_or_await(c, rv);
                return;
            }
            JS_FreeValue(ctx, rv);
            if (app->dispose_called) {
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
                return;
            }
            continue;
        }
        JS_FreeValue(ctx, r);
        if (app->dispose_called) {
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, handler);
            dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
            return;
        }
    }
    if (app->dispose_called) {
        JS_FreeValue(ctx, ctx_obj);
        JS_FreeValue(ctx, handler);
        dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
        return;
    }
    {
        JSValueConst a[1] = { ctx_obj };
        JSValue res = JS_Call(ctx, handler, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, ctx_obj);
        JS_FreeValue(ctx, handler);
        if (JS_IsException(res)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char* em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
            return;
        }
        dyn_app_dyn_send_or_await(c, res);
    }
}

static JSValue dyn_app_dyn_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    const char* method)
{
    dyn_app_t* app;
    const char* pat = NULL;
    dyn_app_route_t* nr;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern and handler are required",
            method);
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern must be a string", method);
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): handler must be a function", method);
    pat = JS_ToCString(ctx, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    if (dyn_app_dyn_validate(ctx, pat) < 0) {
        JS_FreeCString(ctx, pat);
        return JS_EXCEPTION;
    }
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, pat);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->type = APP_DYN;
    nr->handler = JS_DupValue(ctx, argv[1]);
    nr->dyn_method = strdup(method);
    nr->dyn_pattern = strdup(pat);
    nr->path = strdup(pat);
    JS_FreeCString(ctx, pat);
    if (!nr->dyn_method || !nr->dyn_pattern || !nr->path) {
        free(nr->dyn_method);
        free(nr->dyn_pattern);
        free(nr->path);
        JS_FreeValue(ctx, nr->handler);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->n_routes++;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_app_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "GET");
}
static JSValue dyn_app_post(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "POST");
}
static JSValue dyn_app_put(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "PUT");
}
static JSValue dyn_app_patch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "PATCH");
}
static JSValue dyn_app_del(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "DELETE");
}

static JSValue dyn_app_use(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    JSValue* nm;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "use(fn): fn must be a function");
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    if (app->n_mw == app->cap_mw) {
        size_t nc = app->cap_mw ? app->cap_mw * 2 : 4;
        nm = (JSValue*)realloc(app->mw, nc * sizeof *nm);
        if (!nm)
            return JS_ThrowOutOfMemory(ctx);
        app->mw = nm;
        app->cap_mw = nc;
    }
    app->mw[app->n_mw++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static int dyn_app_try_dyn(dyn_app_conn_t* c, const char* base, size_t head_len,
    const char* path, const char* body, size_t body_len)
{
    dyn_app_t* app = c->app;
    JSContext* ctx = app->ctx;
    char method[16];
    const char* tgt;
    size_t tlen, i;
    int pat_hit = 0;

    dyn_app_req_method(base, method, sizeof method);
    dyn_app_req_target(base, head_len, &tgt, &tlen);
    for (i = 0; i < app->n_routes; i++) {
        const dyn_app_route_t* rt = &app->routes[i];
        JSValue params, ctx_obj, qv, hv, bv, mv, pv;
        const char* qs;
        size_t qn;
        int m;
        if (rt->type != APP_DYN)
            continue;
        params = JS_NewObject(ctx);
        if (JS_IsException(params))
            goto fail_oom;
        m = dyn_app_dyn_match(ctx, rt->dyn_pattern, path, params);
        if (m < 0) {
            JS_FreeValue(ctx, params);
            goto fail_oom;
        }
        if (m == 0) {
            JS_FreeValue(ctx, params);
            continue;
        }
        pat_hit = 1;
        if (strcmp(rt->dyn_method, method) != 0) {
            JS_FreeValue(ctx, params);
            continue;
        }
        qs = (const char*)memchr(tgt, '?', tlen);
        qn = qs ? (size_t)(tgt + tlen - (qs + 1)) : 0;
        qv = qs ? dyn_app_build_query(ctx, qs + 1, qn) : JS_NewObject(ctx);
        hv = dyn_app_build_headers(ctx, base, head_len);
        bv = JS_NewStringLen(ctx, body, body_len);
        mv = JS_NewString(ctx, method);
        pv = JS_NewString(ctx, path);
        ctx_obj = JS_NewObject(ctx);
        if (JS_IsException(qv) || JS_IsException(hv) || JS_IsException(bv) || JS_IsException(mv) || JS_IsException(pv) || JS_IsException(ctx_obj)) {
            JS_FreeValue(ctx, qv);
            JS_FreeValue(ctx, hv);
            JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, mv);
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, params);
            goto fail_oom;
        }
        JS_DefinePropertyValueStr(ctx, ctx_obj, "method", mv, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctx_obj, "path", pv, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctx_obj, "params", params, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctx_obj, "query", qv, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctx_obj, "headers", hv, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ctx_obj, "body", bv, JS_PROP_C_W_E);
        dyn_app_dispatch_dyn(c, rt, ctx_obj);
        return 1;
    }
    if (pat_hit) {
        char allow[256];
        size_t al = 0, a;
        for (i = 0; i < app->n_routes; i++) {
            const dyn_app_route_t* rt = &app->routes[i];
            JSValue scratch;
            size_t ml;
            int m2, dup = 0;
            if (rt->type != APP_DYN)
                continue;
            scratch = JS_NewObject(ctx);
            if (JS_IsException(scratch))
                goto fail_oom;
            m2 = dyn_app_dyn_match(ctx, rt->dyn_pattern, path, scratch);
            JS_FreeValue(ctx, scratch);
            if (m2 < 0)
                goto fail_oom;
            if (m2 == 0)
                continue;
            ml = strlen(rt->dyn_method);
            a = 0;
            while (a + 1 < al) {
                size_t e = a;
                while (e < al && allow[e] != ',')
                    e++;
                if (e - a == ml && memcmp(allow + a, rt->dyn_method, ml) == 0) {
                    dup = 1;
                    break;
                }
                a = e + 2;
            }
            if (dup || al + ml + 3 >= sizeof allow)
                continue;
            if (al) {
                allow[al++] = ',';
                allow[al++] = ' ';
            }
            memcpy(allow + al, rt->dyn_method, ml);
            al += ml;
        }
        allow[al] = '\0';
        dyn_app_send_err_allow(c, 405, "{\"error\":\"method not allowed\"}",
            allow);
        return -1;
    }
    return 0;
fail_oom:
    dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
    return -1;
}

static char** dyn_app_parse_allow(JSContext* ctx, JSValueConst opts, size_t* pn)
{
    char** out = NULL;
    JSValue av = JS_GetPropertyStr(ctx, opts, "allow");
    *pn = 0;
    if (JS_IsArray(ctx, av)) {
        uint32_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, av, "length");
        if (JS_ToUint32(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return NULL;
        }
        JS_FreeValue(ctx, lv);
        if (len)
            out = (char**)calloc(len, sizeof(char*));
        for (i = 0; out && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, av, i);
            const char* s = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (s) {
                out[*pn] = strdup(s);
                if (out[*pn])
                    (*pn)++;
                JS_FreeCString(ctx, s);
            }
        }
    }
    JS_FreeValue(ctx, av);
    return out;
}

static JSValue dyn_app_proxy(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *prefix = NULL, *host = NULL;
    dyn_app_route_t* nr;
    JSValue jh, jp;
    int32_t port = 0;

    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "proxy(prefix, { host, port })");
    if (dyn_opts_strict(ctx, argv[1], http_proxy_keys, 2))
        return JS_EXCEPTION;
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix)
        return JS_EXCEPTION;
    jh = JS_GetPropertyStr(ctx, argv[1], "host");
    jp = JS_GetPropertyStr(ctx, argv[1], "port");
    if (JS_IsException(jh) || JS_IsException(jp))
        goto fail;
    host = JS_IsUndefined(jh) ? js_strdup(ctx, "127.0.0.1") : JS_ToCString(ctx, jh);
    if (!host)
        goto fail;
    if (JS_ToInt32(ctx, &port, jp) < 0)
        goto fail;
    if (port <= 0 || port > 65535) {
        JS_ThrowRangeError(ctx, "proxy: upstream port out of range");
        goto fail;
    }

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto fail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_PROXY;
    nr->handler = JS_UNDEFINED;
    nr->up_host = strdup(host);
    nr->up_port = (uint16_t)port;
    if (!nr->path || !nr->up_host) {
        free(nr->path);
        free(nr->up_host);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->n_routes++;
    JS_FreeValue(ctx, jh);
    JS_FreeValue(ctx, jp);
    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_DupValue(ctx, this_val);
fail:
    JS_FreeValue(ctx, jh);
    JS_FreeValue(ctx, jp);
    if (host)
        JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}

static JSValue dyn_app_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *prefix, *dir;
    dyn_app_route_t* nr;
    char** allow = NULL;
    size_t n_allow = 0;
    int64_t maxf = 0;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "static(prefix, dir[, opts])");
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix)
        return JS_EXCEPTION;
    dir = dyn_path_borrow(ctx, argv[1], "static(prefix, root)", NULL);
    if (!dir) {
        JS_FreeCString(ctx, prefix);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], http_static_keys, 2)) {
            JS_FreeCString(ctx, prefix);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxFileSize");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &maxf, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, prefix);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        allow = dyn_app_parse_allow(ctx, argv[2], &n_allow);
    }

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto fail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_STATIC;
    nr->handler = JS_UNDEFINED;
    nr->dir = strdup(dir);
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, prefix);
    return JS_UNDEFINED;
fail:
    {
        size_t k;
        for (k = 0; k < n_allow; k++)
            free(allow[k]);
        free(allow);
    }
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}

static JSValue dyn_app_upload(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *path = NULL, *dir = NULL;
    dyn_app_route_t* nr;
    char** allow = NULL;
    size_t n_allow = 0, k;
    int64_t maxf = 0;
    JSValue handler = JS_UNDEFINED, v;

    if (argc < 3 || !JS_IsObject(argv[1]) || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "upload(path, {dir,...}, handler)");
    if (dyn_opts_strict(ctx, argv[1], http_upload_keys, 3))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[1], "dir");
    dir = JS_IsUndefined(v) ? NULL
                            : dyn_path_borrow(ctx, v, "upload opts.dir", NULL);
    JS_FreeValue(ctx, v);
    if (!dir) {
        JS_FreeCString(ctx, path);
        if (JS_IsUndefined(v))
            return JS_ThrowTypeError(ctx, "upload: opts.dir required");
        return JS_EXCEPTION;
    }
    v = JS_GetPropertyStr(ctx, argv[1], "maxFileSize");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &maxf, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, v);
    allow = dyn_app_parse_allow(ctx, argv[1], &n_allow);
    handler = JS_DupValue(ctx, argv[2]);

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto ufail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto ufail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_UPLOAD;
    nr->handler = handler;
    nr->dir = strdup(dir);
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
ufail:
    for (k = 0; k < n_allow; k++)
        free(allow[k]);
    free(allow);
    JS_FreeValue(ctx, handler);
    JS_FreeCString(ctx, path);
    return JS_EXCEPTION;
}

static JSValue dyn_app_sse_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "sse(path, {open,close})");
    if (dyn_opts_strict(ctx, argv[1], http_sse_handler_keys, 2))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_SSE;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_app_ws_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "ws(path, {open,message,close})");
    if (dyn_opts_strict(ctx, argv[1], http_ws_handler_keys, 4))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_WS;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static void dyn_app_drain(void* udata)
{
    dyn_app_t* app = (dyn_app_t*)udata;
    if (app->timers)
        dyn_timer_run(app->timers, dyn_timer_now_ms());
    if (app->timers_orphan) {
        dyn_timers_free(app->timers_orphan);
        app->timers_orphan = NULL;
    }
    if (app->dispose_called && !app->in_sweep && app->n_conns == 0) {
        dyn_net_off_drain(app);
        free(app);
    }
}

static JSValue dyn_app_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    (void)argc;
    (void)argv;
    if (!app)
        return JS_EXCEPTION;
    if (app->started)
        return JS_UNDEFINED;
    app->listen_fd = dyn_aio_listen(app->aio,
        app->listen_host ? app->listen_host
                         : "0.0.0.0",
        app->port,
        app->backlog > 0 ? app->backlog : 1024);
    if (app->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "App: listen failed");
    if (app->port == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(app->listen_fd, (struct sockaddr*)&sin, &sl) == 0)
            app->port = ntohs(sin.sin_port);
    }
    dyn_aio_accept(app->aio, app->listen_fd, dyn_app_on_accept, app);
    if (app->idle_ms) {
        app->timers = dyn_timers_new();
        if (!app->timers)
            return JS_ThrowOutOfMemory(ctx);
        app->sweep = dyn_timer_add(app->timers, dyn_timer_now_ms(), 1000,
            dyn_app_idle_sweep, app);
    }
    {
        int rc = dyn_net_on_drain(dyn_app_drain, app);
        if (rc == -2)
            return JS_ThrowInternalError(ctx,
                "App: this reactor backend cannot arm a periodic timer, so the "
                "idle-timeout sweep would never run");
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
    }
    app->started = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_app_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->port);
}

static JSValue dyn_app_get_workers(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->workers ? app->workers : 1);
}

static JSValue dyn_app_get_backlog(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->backlog ? app->backlog : 1024);
}

typedef struct wsc_hs wsc_hs_t;

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    int fd;
    JSValue handlers;
    JSValue self;
    dyn_iobuf_t in;
    dyn_iobuf_t frag;
    int frag_op, frag_frames, ctl_budget;
    int refs;
    int closed;
    int close_sent;
    wsc_hs_t* hs;
    int dispose_deferred;
    int release_deferred;
} dyn_wsc_t;

struct wsc_hs {
    dyn_wsc_t* w;
    char* url;
    dyn_url_t u;
    char* http_url;
    char key_b64[32], accept[32];
    dyn_bytes_t req;
    int fd;
    char head[16384];
    size_t got, hdr_end;
    int err;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
    dyn_tls_ctx_t* tls_ctx;
    char tls_err[192];
#endif
};

static JSClassID dyn_wsc_class_id;

static void dyn_wsc_close_internal(dyn_wsc_t* w, int code, const char* reason);

static void dyn_wsc_unref(dyn_wsc_t* w)
{
    if (--w->refs != 0)
        return;
    if (w->hs) {
        w->dispose_deferred = 1;
        return;
    }
    dyn_iobuf_free(&w->in);
    dyn_iobuf_free(&w->frag);
    free(w);
}

static void dyn_wsc_send_frame(dyn_wsc_t* w, int opcode, const uint8_t* data,
    size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10], mask[4];
    size_t hn, i, off;

    if (w->closed)
        return;
    if (dyn_aio_queued(w->aio, w->fd) + len + 16 > DYN_HTTP_OUTBOUND_MAX) {
        dyn_wsc_close_internal(w, 1008, "output backpressure");
        return;
    }
    if (dyn_os_entropy(mask, sizeof mask) < 0) {
        w->closed = 1;
        return;
    }
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (uint8_t)(opcode & 0x0f);
    if (len < 126) {
        h[1] = 0x80 | (uint8_t)len;
        hn = 2;
    } else if (len <= 0xffff) {
        h[1] = 0x80 | 126;
        h[2] = (len >> 8) & 0xff;
        h[3] = len & 0xff;
        hn = 4;
    } else {
        h[1] = 0x80 | 127;
        for (i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
        hn = 10;
    }
    if (dyn_iobuf_append(&f, h, hn) < 0 || dyn_iobuf_append(&f, mask, 4) < 0) {
        dyn_iobuf_free(&f);
        dyn_wsc_close_internal(w, 1011, "out of memory while sending");
        return;
    }
    off = f.len;
    if (len > 0 && dyn_iobuf_append(&f, data, len) < 0) {
        dyn_iobuf_free(&f);
        dyn_wsc_close_internal(w, 1011, "out of memory while sending");
        return;
    }
    if (len) {
        uint8_t* p;
        uint32_t m32;
        uint64_t m64;
        size_t k = 0, n8 = len & ~(size_t)7;
        p = f.data + off;
        memcpy(&m32, mask, 4);
        m64 = ((uint64_t)m32 << 32) | m32;
        for (; k < n8; k += 8) {
            uint64_t v;
            memcpy(&v, p + k, 8);
            v ^= m64;
            memcpy(p + k, &v, 8);
        }
        for (i = k; i < len; i++)
            p[i] ^= mask[i & 3];
    }
    dyn_aio_send(w->aio, w->fd, f.data, f.len, 0, NULL, NULL);
    dyn_iobuf_free(&f);
}

static void dyn_wsc_dispatch_msg(dyn_wsc_t* w, int opcode,
    const uint8_t* payload, size_t plen)
{
    JSContext* ctx = w->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, w->handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        JS_FreeValue(ctx, mh);
        dyn_wsc_close_internal(w, 1007, "invalid UTF-8 in text message");
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? JS_NewArrayBufferCopy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char*)payload, plen);
        JSValueConst args[3] = { w->self, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

static void dyn_wsc_process(dyn_wsc_t* w)
{
    if (w->closed)
        return;
    for (;;) {
        uint8_t* p = dyn_iobuf_rdata(&w->in);
        size_t avail = dyn_iobuf_rlen(&w->in);
        int fin, opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t* payload;

        if (avail < 2)
            return;
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        plen = p[1] & 0x7f;
        hdr = 2;
        if (plen == 126) {
            if (avail < 4)
                return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10)
                return;
            plen = 0;
            for (i = 0; i < 8; i++)
                plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (masked || (opcode >= 0x8 && (plen > 125 || !fin))) {
            dyn_wsc_close_internal(w, 1002, "protocol error");
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) {
            dyn_wsc_close_internal(w, 1009, "too big");
            return;
        }
        if (avail < hdr + plen)
            return;
        payload = p + hdr;
        frame_total = hdr + (size_t)plen;

        if (opcode == 0x8) {
            int code = 1000;
            if (plen >= 2)
                code = (payload[0] << 8) | payload[1];
            if (!w->close_sent) {
                w->close_sent = 1;
                dyn_wsc_send_frame(w, 8, payload, (size_t)plen);
            }
            dyn_iobuf_consume(&w->in, frame_total);
            dyn_wsc_close_internal(w, code, "");
            return;
        } else if (opcode == 0x9) {
            if (--w->ctl_budget < 0) {
                dyn_wsc_close_internal(w, 1002, "control flood");
                return;
            }
            dyn_wsc_send_frame(w, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {
        } else if (opcode <= 0x2) {
            int frag_active = w->frag_op != 0;
            if ((opcode == 0x0 && !frag_active)
                || (opcode != 0x0 && frag_active)) {
                dyn_wsc_close_internal(w, 1002, "protocol error");
                return;
            }
            if (opcode != 0x0 && fin) {
                w->ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_wsc_dispatch_msg(w, opcode, payload, (size_t)plen);
            } else {
                if (opcode != 0x0) {
                    w->frag_op = opcode;
                    w->frag_frames = 0;
                }
                if (++w->frag_frames > DYN_WS_MAX_FRAGMENTS || dyn_iobuf_rlen(&w->frag) + plen > DYN_ACONN_MAX_REQ || dyn_iobuf_append(&w->frag, payload, (size_t)plen) < 0) {
                    dyn_wsc_close_internal(w, 1009, "too big");
                    return;
                }
                if (fin) {
                    w->ctl_budget = DYN_WS_CTL_BUDGET;
                    dyn_wsc_dispatch_msg(w, w->frag_op,
                        dyn_iobuf_rdata(&w->frag),
                        dyn_iobuf_rlen(&w->frag));
                    dyn_iobuf_reset(&w->frag);
                    w->frag_op = 0;
                    w->frag_frames = 0;
                }
            }
        }
        dyn_iobuf_consume(&w->in, frame_total);
        if (w->closed)
            return;
    }
}

static void dyn_wsc_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_wsc_t* w = (dyn_wsc_t*)ud;
    (void)aio;
    if (res <= 0) {
        dyn_wsc_close_internal(w, 1006, "abnormal closure");
        return;
    }
    w->refs++;
    if (dyn_iobuf_append(&w->in, buf, len) < 0
        || dyn_iobuf_rlen(&w->in) > DYN_ACONN_MAX_REQ) {
        dyn_wsc_close_internal(w, 1009, "too big");
        dyn_wsc_unref(w);
        return;
    }
    dyn_wsc_process(w);
    dyn_wsc_unref(w);
}

static void dyn_wsc_close_internal(dyn_wsc_t* w, int code, const char* reason)
{
    JSContext* ctx;
    JSValue ch;

    if (w->closed)
        return;
    w->closed = 1;
    w->refs++;
    ctx = w->ctx;
    if (w->fd >= 0) {
        dyn_aio_close(w->aio, w->fd);
        w->fd = -1;
    }
    ch = JS_GetPropertyStr(ctx, w->handlers, "close");
    if (JS_IsFunction(ctx, ch)) {
        JSValueConst a[3] = { w->self, JS_NewInt32(ctx, code),
            JS_NewString(ctx, reason) };
        dyn_call_drop(ctx, ch, JS_UNDEFINED, 3, a);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, a[2]);
    }
    JS_FreeValue(ctx, ch);
    JS_FreeValue(ctx, w->handlers);
    w->handlers = JS_UNDEFINED;
    JS_FreeValue(ctx, w->self);
    w->self = JS_UNDEFINED;
    if (w->hs) {
        w->release_deferred = 1;
    } else {
        dyn_http_async_release(w->ctx);
    }
    dyn_wsc_unref(w);
}

static JSValue dyn_wsc_send(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_wsc_t* w = (dyn_wsc_t*)dyn_res_native(ctx, this_val,
        dyn_wsc_class_id);
    const char* str = NULL;
    uint8_t* abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_UNDEFINED;
    w->refs++;
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) {
            dyn_wsc_unref(w);
            return JS_EXCEPTION;
        }
    } else {
        abuf = dyn_ws_bytes_view(ctx, argv[0], &len, &binary);
        if (!abuf && !binary) {
            str = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!str) {
                dyn_wsc_unref(w);
                return JS_EXCEPTION;
            }
        }
    }
    if (!w->closed)
        dyn_wsc_send_frame(w, binary ? 2 : 1,
            str ? (const uint8_t*)str : abuf, len);
    dyn_wsc_unref(w);
    if (str)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_wsc_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_wsc_send),
};

static void dyn_wsc_dispose(void* native)
{
    dyn_wsc_t* w = (dyn_wsc_t*)native;
    if (!w)
        return;
    w->refs++;
    if (!w->closed) {
        if (!w->close_sent && w->fd >= 0) {
            w->close_sent = 1;
            dyn_wsc_send_frame(w, 8, NULL, 0);
        }
        dyn_wsc_close_internal(w, 1000, "");
    }
    dyn_wsc_unref(w);
    dyn_wsc_unref(w);
}

static void wsc_hs_work(void* arg)
{
    wsc_hs_t* hs = (wsc_hs_t*)arg;
    uint8_t key_raw[16];
    char hhost[320];
    char hbuf3[256];
    size_t klen;
    int err = 0;

    if (dyn_os_entropy(key_raw, sizeof key_raw) < 0) {
        hs->err = -1;
        return;
    }
    klen = dyn_codec_base64_encode(key_raw, sizeof key_raw, hs->key_b64);
    hs->key_b64[klen] = '\0';

    hs->fd = dyn_tcp_connect(dyn_host_bare(hs->u.host, hbuf3, sizeof hbuf3),
        hs->u.port,
        DYN_HTTP_DEFAULT_TIMEOUT_MS, &err);
    if (hs->fd < 0) {
        hs->err = err;
        return;
    }
    {
        struct timeval tv;
        tv.tv_sec = DYN_HTTP_DEFAULT_TIMEOUT_MS / 1000;
        tv.tv_usec = (DYN_HTTP_DEFAULT_TIMEOUT_MS % 1000) * 1000;
        setsockopt(hs->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(hs->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#ifdef CONFIG_TLS
    if (hs->tls) {
        uint8_t buf[16384];
        int n, st;
        for (;;) {
            st = dyn_tls_handshake(hs->tls);
            while ((n = dyn_tls_pull(hs->tls, buf, sizeof buf)) > 0) {
                if (hc_raw_send(hs->fd, buf, (size_t)n) < 0) {
                    hs->err = DYN_HTTP_ERR_SEND;
                    return;
                }
            }
            if (st != 0)
                break;
            n = (int)recv(hs->fd, buf, sizeof buf, 0);
            if (n <= 0 || dyn_tls_feed(hs->tls, buf, (size_t)n) != 0) {
                hs->err = DYN_HTTP_ERR_RECV;
                snprintf(hs->tls_err, sizeof hs->tls_err, "%s",
                    dyn_tls_error(hs->tls));
                return;
            }
        }
        if (st < 0) {
            hs->err = DYN_HTTP_ERR_TLS;
            snprintf(hs->tls_err, sizeof hs->tls_err, "%s",
                dyn_tls_error(hs->tls));
            return;
        }
    }
#endif
    if (hs->u.port == 80)
        snprintf(hhost, sizeof hhost, "%s", hs->u.host);
    else
        snprintf(hhost, sizeof hhost, "%s:%u", hs->u.host, (unsigned)hs->u.port);
    if (dyn_bytes_append(&hs->req, "GET ", 4) < 0
        || dyn_bytes_append(&hs->req, hs->u.path, hs->u.path_len) < 0
        || dyn_bytes_append(&hs->req, " HTTP/1.1\r\nHost: ", 17) < 0
        || dyn_bytes_append(&hs->req, hhost, strlen(hhost)) < 0
        || dyn_bytes_append(&hs->req, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                      "Sec-WebSocket-Key: ",
               62)
            < 0
        || dyn_bytes_append(&hs->req, hs->key_b64, klen) < 0
        || dyn_bytes_append(&hs->req, "\r\nSec-WebSocket-Version: 13\r\n\r\n", 31) < 0) {
        hs->err = DYN_HTTP_ERR_OOM;
        return;
    }
#ifdef CONFIG_TLS
    if (hs->tls) {
        uint8_t ob[16384];
        size_t off = 0;
        int n;
        while (off < hs->req.len) {
            n = dyn_tls_write(hs->tls, (const uint8_t*)hs->req.data + off,
                hs->req.len - off);
            if (n <= 0) {
                hs->err = DYN_HTTP_ERR_SEND;
                return;
            }
            off += (size_t)n;
            while ((n = dyn_tls_pull(hs->tls, ob, sizeof ob)) > 0) {
                if (hc_raw_send(hs->fd, ob, (size_t)n) < 0) {
                    hs->err = DYN_HTTP_ERR_SEND;
                    return;
                }
            }
        }
    } else
#endif
        if (hc_raw_send(hs->fd, hs->req.data, hs->req.len) < 0) {
        hs->err = DYN_HTTP_ERR_SEND;
        return;
    }

    for (;;) {
        const char* m;
        ssize_t r;
        if (hs->got >= sizeof hs->head) {
            hs->err = -1;
            return;
        }
#ifdef CONFIG_TLS
        if (hs->tls) {
            uint8_t rb[16384], pt[16384];
            int got;
            r = recv(hs->fd, rb, sizeof rb, 0);
            if (r <= 0 || dyn_tls_feed(hs->tls, rb, (size_t)r) != 0) {
                hs->err = DYN_HTTP_ERR_RECV;
                return;
            }
            while ((got = dyn_tls_read(hs->tls, pt, sizeof pt)) > 0) {
                size_t take = (size_t)got;
                if (hs->got + take > sizeof hs->head)
                    take = sizeof hs->head - hs->got;
                memcpy(hs->head + hs->got, pt, take);
                hs->got += take;
                if (hs->got >= sizeof hs->head)
                    break;
            }
            m = dyn_memfind(hs->head, hs->got, "\r\n\r\n", 4);
            if (m) {
                hs->hdr_end = (size_t)(m - hs->head) + 4;
                return;
            }
            continue;
        }
#endif
        r = recv(hs->fd, hs->head + hs->got, sizeof hs->head - hs->got, 0);
        if (r <= 0) {
            hs->err = DYN_HTTP_ERR_RECV;
            return;
        }
        hs->got += (size_t)r;
        m = dyn_memfind(hs->head, hs->got, "\r\n\r\n", 4);
        if (m) {
            hs->hdr_end = (size_t)(m - hs->head) + 4;
            return;
        }
    }
}

static void wsc_hs_done(void* arg)
{
    wsc_hs_t* hs = (wsc_hs_t*)arg;
    dyn_wsc_t* w = hs->w;
    JSContext* ctx = w->ctx;
    char why[384];
    int fail = 1, code = 1006;
    int rel_deferred;
    w->refs++;

    rel_deferred = w->release_deferred;
    w->release_deferred = 0;

    w->hs = NULL;
    why[0] = 0;
    if (hs->err == 0) {
        const char* sp = memchr(hs->head, ' ', hs->hdr_end);
        const char* acc;
        size_t acclen = 0;
        int status = 0;
        if (!sp) {
            snprintf(why, sizeof why, "handshake failed: no status line");
        } else {
            while (hs->head + hs->hdr_end > ++sp && *sp >= '0' && *sp <= '9')
                status = status * 10 + (*sp - '0');
            if (status != 101) {
                snprintf(why, sizeof why, "handshake failed: status %d", status);
            } else {
                acc = dyn_req_header(hs->head, hs->hdr_end,
                    "sec-websocket-accept", &acclen);
                dws_accept(hs->key_b64, strlen(hs->key_b64), hs->accept);
                if (!acc || acclen != strlen(hs->accept)
                    || memcmp(acc, hs->accept, acclen) != 0) {
                    snprintf(why, sizeof why,
                        "handshake failed: bad Sec-WebSocket-Accept");
                } else {
                    fail = 0;
                }
            }
        }
    } else if (hs->err == -1) {
        snprintf(why, sizeof why, "handshake failed: response headers too large");
    } else {
#ifdef CONFIG_TLS
        if (hs->tls_err[0])
            snprintf(why, sizeof why, "HTTP WS %s failed: %s (%s)",
                hs->url ? hs->url : "", hc_err_name(hs->err),
                hs->tls_err);
        else
#endif
            snprintf(why, sizeof why, "HTTP WS %s failed: %s",
                hs->url ? hs->url : "", hc_err_name(hs->err));
    }

    if (!fail && !w->closed) {
        JSValue oh;
        w->fd = hs->fd;
        {
            int fl = fcntl(hs->fd, F_GETFL, 0);
            if (fl >= 0)
                (void)fcntl(hs->fd, F_SETFL, fl | O_NONBLOCK);
            struct timeval tv;
            memset(&tv, 0, sizeof tv);
            setsockopt(hs->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(hs->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        if (hs->got > hs->hdr_end)
            dyn_iobuf_append(&w->in, hs->head + hs->hdr_end,
                hs->got - hs->hdr_end);
#ifdef CONFIG_TLS
        if (hs->tls)
            dyn_aio_tls_attach(w->aio, hs->fd, hs->tls, NULL, NULL);
#endif
        dyn_aio_recv(w->aio, hs->fd, 0, 1, dyn_wsc_on_recv, w);
        oh = JS_GetPropertyStr(ctx, w->handlers, "open");
        if (JS_IsFunction(ctx, oh)) {
            JSValueConst a[1] = { w->self };
            dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, oh);
        dyn_wsc_process(w);
    } else {
        if (hs->fd >= 0)
            close(hs->fd);
        if (!w->closed) {
            dyn_res_mark_closed(JS_GetRuntime(ctx), w->self, dyn_wsc_class_id);
            dyn_wsc_close_internal(w, code, why);
            dyn_wsc_unref(w);
        }
    }
    free(hs->req.data);
    free(hs->http_url);
    free(hs->url);
#ifdef CONFIG_TLS
    if (hs->tls_ctx)
        dyn_tls_ctx_free(hs->tls_ctx);
    if (fail && hs->tls)
        dyn_tls_conn_free(hs->tls);
#endif
    free(hs);
    dyn_wsc_unref(w);
    if (rel_deferred) {
        dyn_http_async_release(ctx);
    }
}

static JSValue dyn_wsc_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_wsc_t* w = NULL;
    wsc_hs_t* hs = NULL;
    const char* url = NULL;
    dyn_url_t u;
    __maybe_unused char hbuf3[256];
    JSValue obj;
    int is_wss = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    if (dyn_opts_strict(ctx, argv[1], http_ws_handler_keys, 3))
        return JS_EXCEPTION;
    url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    is_wss = (strncmp(url, "wss://", 6) == 0);
    if (!is_wss && strncmp(url, "ws://", 5) != 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "WsClient: the URL scheme must be ws://");
    }

    w = (dyn_wsc_t*)calloc(1, sizeof(*w));
    if (!w)
        goto oom;
    w->ctx = ctx;
    w->fd = -1;
    w->refs = 1;
    w->aio = dyn_net_reactor_acquire(ctx);
    if (!w->aio) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    w->handlers = JS_DupValue(ctx, argv[1]);
    w->ctl_budget = DYN_WS_CTL_BUDGET;
    dyn_iobuf_init(&w->in);
    dyn_iobuf_init(&w->frag);

    hs = (wsc_hs_t*)calloc(1, sizeof(*hs));
    if (!hs)
        goto oom;
    hs->w = w;
    hs->url = strdup(url);
    hs->http_url = (char*)malloc(strlen(url) + 3);
    if (!hs->url || !hs->http_url)
        goto oom;
    snprintf(hs->http_url, strlen(url) + 3,
        is_wss ? "https://%s" : "http://%s",
        url + (is_wss ? 6 : 5));
    if (dyn_parse_url(hs->http_url, &u) < 0) {
        dyn_http_throw(ctx, DYN_HTTP_ERR_URL, "WS", url, NULL);
        goto fail;
    }
    hs->u = u;
#ifdef CONFIG_TLS
    if (is_wss) {
        dyn_tls_opts_t to;
        memset(&to, 0, sizeof to);
        hs->tls_ctx = dyn_tls_ctx_client(&to, hs->tls_err,
            sizeof hs->tls_err);
        if (!hs->tls_ctx) {
            char msg[256];
            snprintf(msg, sizeof msg, "WsClient: TLS setup failed: %s",
                hs->tls_err);
            JS_FreeCString(ctx, url);
            free(hs->url);
            free(hs->http_url);
            free(hs);
            JS_ThrowTypeError(ctx, "%s", msg);
            goto fail;
        }
        hs->tls = dyn_tls_conn_new(hs->tls_ctx,
            dyn_host_bare(u.host, hbuf3, sizeof hbuf3), hs->tls_err,
            sizeof hs->tls_err);
        if (!hs->tls) {
            char msg[256];
            snprintf(msg, sizeof msg, "WsClient: TLS setup failed: %s",
                hs->tls_err);
            dyn_tls_ctx_free(hs->tls_ctx);
            hs->tls_ctx = NULL;
            JS_FreeCString(ctx, url);
            free(hs->url);
            free(hs->http_url);
            free(hs);
            JS_ThrowTypeError(ctx, "%s", msg);
            goto fail;
        }
        if (u.port == 0)
            u.port = 443;
        hs->u.port = u.port;
    }
#endif

    dyn_http_async_hook(ctx);
    obj = dyn_res_wrap(ctx, new_target, dyn_wsc_class_id, w, dyn_wsc_dispose);
    if (JS_IsException(obj)) {
        w = NULL;
        goto fail;
    }
    w->self = JS_DupValue(ctx, obj);
    w->hs = hs;
    dyn_aio_offload(w->aio, wsc_hs_work, wsc_hs_done, hs);
    hs = NULL;

    JS_FreeCString(ctx, url);
    return obj;

oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    if (hs) {
        free(hs->req.data);
        free(hs->http_url);
        free(hs->url);
        free(hs);
    }
    if (w) {
        if (w->aio)
            dyn_net_reactor_release(ctx);
        JS_FreeValue(ctx, w->handlers);
        dyn_iobuf_free(&w->in);
        dyn_iobuf_free(&w->frag);
        free(w);
    }
    JS_FreeCString(ctx, url);
    return JS_EXCEPTION;
}

static const JSClassDef dyn_wsc_class = {
    "WsClient",
    .finalizer = dyn_res_finalizer,
};

#define DYN_APP_CHAINED(fn)                                              \
    static JSValue fn##_chain(JSContext* ctx, JSValueConst this_val,     \
        int argc, JSValueConst* argv)                                    \
    {                                                                    \
        JSValue r = fn(ctx, this_val, argc, argv);                       \
        if (JS_IsUndefined(r))                                           \
            return JS_DupValue(ctx, this_val);                           \
        return r;                                                        \
    }
DYN_APP_CHAINED(dyn_app_rpc)
DYN_APP_CHAINED(dyn_app_static)
DYN_APP_CHAINED(dyn_app_proxy)
DYN_APP_CHAINED(dyn_app_upload)
DYN_APP_CHAINED(dyn_app_ws_register)
DYN_APP_CHAINED(dyn_app_sse_register)

static const JSCFunctionListEntry dyn_app_proto[] = {
    JS_CFUNC_DEF("rpc", 2, dyn_app_rpc_chain),
    JS_CFUNC_DEF("static", 2, dyn_app_static_chain),
    JS_CFUNC_DEF("proxy", 2, dyn_app_proxy_chain),
    JS_CFUNC_DEF("upload", 3, dyn_app_upload_chain),
    JS_CFUNC_DEF("ws", 2, dyn_app_ws_register_chain),
    JS_CFUNC_DEF("sse", 2, dyn_app_sse_register_chain),
    JS_CFUNC_DEF("get", 2, dyn_app_get),
    JS_CFUNC_DEF("post", 2, dyn_app_post),
    JS_CFUNC_DEF("put", 2, dyn_app_put),
    JS_CFUNC_DEF("patch", 2, dyn_app_patch),
    JS_CFUNC_DEF("del", 2, dyn_app_del),
    JS_CFUNC_DEF("use", 1, dyn_app_use),
    JS_CFUNC_DEF("start", 0, dyn_app_start),
    JS_CGETSET_DEF("port", dyn_app_get_port, NULL),
    JS_CGETSET_DEF("workers", dyn_app_get_workers, NULL),
    JS_CGETSET_DEF("backlog", dyn_app_get_backlog, NULL),
};

static int dyn_http_register_one(JSContext* ctx, JSModuleDef* m,
    JSClassID* pid, const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs, JSCFunction* ctor_fn,
    const char* name)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0) {
        proto = JS_GetClassProto(ctx, *pid);
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        if (JS_SetModuleExport(ctx, m, name, ctor) < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        return 0;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    dyn_res_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

static int dyn_http_register_internal(JSContext* ctx, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return 0;
    {
        JSValue wp = JS_NewObject(ctx);
        if (JS_IsException(wp))
            return -1;
        JS_SetPropertyFunctionList(ctx, wp, proto_funcs, n_funcs);
        JS_SetClassProto(ctx, *pid, wp);
    }
    return 0;
}

int dyn_http_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_httpmsg_register(ctx, m) < 0)
        return -1;
    simd_init();
    if (dyn_http_register_one(ctx, m, &dyn_http_client_class_id,
            &dyn_http_client_class, dyn_http_client_proto,
            countof(dyn_http_client_proto),
            dyn_http_client_ctor, "HTTPClient")
        < 0)
        return -1;
    {
        JSAtom hook = dyn_registered_symbol(ctx, DYN_HTTP_REQUEST_HOOK);
        JSValue proto = JS_GetClassProto(ctx, dyn_http_client_class_id);
        if (hook != JS_ATOM_NULL && JS_IsObject(proto))
            JS_DefinePropertyValue(ctx, proto, hook,
                JS_NewCFunction(ctx, dyn_http_client_request_pending, "request", 2),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeAtom(ctx, hook);
        JS_FreeValue(ctx, proto);
    }
    if (dyn_http_register_one(ctx, m, &dyn_http_server_class_id,
            &dyn_http_server_class, dyn_http_server_proto,
            countof(dyn_http_server_proto),
            dyn_http_server_ctor, "HTTPServer")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_http_async_class_id,
            &dyn_http_async_class, dyn_http_async_proto,
            countof(dyn_http_async_proto),
            dyn_http_async_ctor, "HTTPServerAsync")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_app_class_id,
            &dyn_app_class, dyn_app_proto,
            countof(dyn_app_proto),
            dyn_app_ctor, "App")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_wsc_class_id,
            &dyn_wsc_class, dyn_wsc_proto,
            countof(dyn_wsc_proto),
            dyn_wsc_ctor, "WsClient")
        < 0)
        return -1;
    {
        JSRuntime* rt = JS_GetRuntime(ctx);
        JSValue sp = JS_NewObject(ctx);
        if (JS_IsException(sp))
            return -1;
        JS_NewClassID(&dyn_hc_stream_class_id);
        if (JS_NewClass(rt, dyn_hc_stream_class_id, &dyn_hc_stream_class) < 0) {
            JS_FreeValue(ctx, sp);
        } else {
            JS_SetPropertyFunctionList(ctx, sp, dyn_hc_stream_proto,
                (int)countof(dyn_hc_stream_proto));
            dyn_res_class_common(ctx, dyn_hc_stream_class_id, sp);
            JS_SetClassProto(ctx, dyn_hc_stream_class_id, sp);
        }
    }
    if (dyn_http_register_internal(ctx, &dyn_ws_class_id, &dyn_ws_class,
            dyn_ws_proto, countof(dyn_ws_proto))
        < 0)
        return -1;
    if (dyn_http_register_internal(ctx, &dyn_sse_class_id, &dyn_sse_class,
            dyn_sse_proto, countof(dyn_sse_proto))
        < 0)
        return -1;
    if (dyn_http_register_internal(ctx, &dyn_pend_class_id, &dyn_pend_class,
            NULL, 0)
        < 0)
        return -1;
    {
        static const char* const fetch_names[] = {
            "fetch", "Request", "Response", "Headers", "AbortSignal",
            "AbortController", "FormData"
        };
        JSValue g = JS_GetGlobalObject(ctx);
        size_t i;
        for (i = 0; i < countof(fetch_names); i++) {
            JSValue v = JS_GetPropertyStr(ctx, g, fetch_names[i]);
            if (!JS_IsUndefined(v))
                JS_SetModuleExport(ctx, m, fetch_names[i], v);
        }
        JS_FreeValue(ctx, g);
    }
    return 0;
}

void dyn_http_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const fetch_names[] = {
        "fetch", "Request", "Response", "Headers", "AbortSignal",
        "AbortController", "FormData"
    };
    size_t i;
    dyn_httpmsg_add_exports(ctx, m);
    JS_AddModuleExport(ctx, m, "HTTPClient");
    JS_AddModuleExport(ctx, m, "HTTPServer");
    JS_AddModuleExport(ctx, m, "HTTPServerAsync");
    JS_AddModuleExport(ctx, m, "App");
    JS_AddModuleExport(ctx, m, "WsClient");
    for (i = 0; i < countof(fetch_names); i++)
        JS_AddModuleExport(ctx, m, fetch_names[i]);
}

#endif
