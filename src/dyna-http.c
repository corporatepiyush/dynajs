/*
 * The App server and HTTP client, part of dyna:net.
 *
 * `App` is the only supported server; low-level HTTP is deliberately not
 * exposed. Routes are typed: static (sendfile), rpc (strict JSON-RPC 2.0),
 * upload, ws. Handlers run ON the JS thread with a zero-copy request view.
 * ONE shared recv buffer in the readiness backend -- never regress to
 * per-connection buffers; that is what keeps RSS flat.
 * The request path is SYSCALL-bound, not CPU-bound: the lever is syscalls per
 * request, not C micro-optimisation.
 * NAMED LIMITATION: ws/sse sends have no backpressure. dyn_aio_send queues
 * what the kernel will not take, so a handler flooding a non-reading peer
 * grows that queue without bound; frames are size-capped, the queue is not.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"
#include "dyna-tls.h"   /* no-op without CONFIG_TLS */
#include "dyna-evloop.h"
#include "dyna-aio.h"
#include "core/dyn-pool.h"   /* the --io-threads default sizes `workers` too */
#include "core/dyn-timer.h"  /* the App's idle-timeout sweep */

/* from dyna-libc.h (not included here): fold a reactor into the JS event loop */
void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata);

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

/* the shared pure-C libraries (src/core/): the WebSocket accept key is
 * base64(sha1(...)), so it needs both */
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-compress.h" /* dyn_gzip_build: response compression */
#include "core/dyn-prng.h"     /* dyn_os_entropy: WS client keys and masks */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>  /* writev: one syscall per response, not two */
#include <limits.h>   /* PATH_MAX, for the static-root containment check */
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "dyna-simd-kernels.h" /* shared multi-ISA `simd` table (strfind) */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Structured error codes surfaced as the numeric `.dynajsError` on a thrown Error. */
#define DYN_HTTP_ERR_URL      1 /* malformed / unsupported URL */
#define DYN_HTTP_ERR_RESOLVE  2 /* getaddrinfo failed */
#define DYN_HTTP_ERR_CONNECT  3 /* socket/connect failed */
#define DYN_HTTP_ERR_SEND     4 /* send failed */
#define DYN_HTTP_ERR_RECV     5 /* recv failed */
#define DYN_HTTP_ERR_PARSE    6 /* malformed response */
#define DYN_HTTP_ERR_OOM      7 /* out of memory */
#define DYN_HTTP_ERR_TOOBIG   8 /* response exceeded the client cap */
#define DYN_HTTP_ERR_TLS      9 /* TLS setup or handshake failed */
#define DYN_HTTP_ERR_CANCEL  10 /* aborted via disconnect()/AbortSignal */
#define DYN_HTTP_ERR_TRUNC   11 /* connection ended before a declared length */

#define DYN_HTTP_DEFAULT_TIMEOUT_MS 15000
#define DYN_HTTP_DEFAULT_MAX_BODY   (16 * 1024 * 1024) /* 16 MB */
/* Thread-pool server: an ABSOLUTE deadline for completing one request. The
 * 5 s SO_RCVTIMEO is per-recv and resets on every byte, so a dribbling peer
 * could hold a worker forever; workers + queue-depth connections were enough
 * to stall the whole server. 0 is the explicit opt-out. */
#define DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT 30000
/* Idle gap BETWEEN keep-alive requests. A connection waiting for its next
 * request holds a thread-pool worker, so this stays small; the full request
 * budget applies only once bytes of an actual request have arrived. */
#define DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT 5000
/* The RESPONSE header block gets its own cap, independent of the body cap. The
 * server has enforced one on requests since it was written (DYN_APP_MAX_HEADER);
 * the client had none, so a hostile server could make it buffer max_body -- 16 MB
 * by default -- of headers per request before anything rejected it. Same number,
 * same reason, other direction. */
#define DYN_HTTP_MAX_RESP_HEADER    (64 * 1024)
/* Whole-request ceiling the HTTPServer pump enforces (431 for a header
 * block that grows past it, 413 for a head+body that declares more): the
 * historical size of the per-connection frame buffer. */
#define DYN_HTTP_SERVER_FRAME_MAX   (16 * 1024 - 1)
/* Effective simultaneous-connection ceiling the queue+workers design ever
 * admitted; the reactor graft keeps the same number as its accept cap. */
#define DYN_HTTP_CONN_QUEUE_CAP     256

/* ---- request-head caps (App and the scanner below share them) ---------- */
/* Header block cap, independent of any body limit. */
#define DYN_APP_MAX_HEADER (64 * 1024)
/* Per-block header COUNT cap: the byte cap above still admits ~4000 tiny
   headers, each of which every header scan re-examines and every handler
   re-parses. 256 is generous for real traffic. Same enforcement points and
   status as the byte cap. */
#define DYN_APP_MAX_HEADER_COUNT 256

/* ==================================================================== *
 *  strict options *
 * ==================================================================== */

/* Every options bag in this module is checked against its valid-key table
 * BEFORE any option is read, socket is opened, or route is registered: an
 * unknown key throws a TypeError naming the key AND the full valid set,
 * instead of being silently ignored. Same shape as the pilot dyn_opts_strict
 * in dyna-file.c: a non-object bag (null / undefined / a primitive) is
 * accepted as absent; own ENUMERABLE STRING keys only (symbol and inherited
 * keys are invisible); a Proxy ownKeys trap that throws propagates; an array
 * bag's "0","1",... keys are unknown -> throws. */
static int dyn_opts_strict(JSContext *ctx, JSValueConst opts,
                           const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { /* OOM converting the key */
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            /* Size the valid list, then build it: no fixed buffer to
             * overflow, no silently truncated message. */
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
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

/* The valid-key tables, one per options bag. dyn_opts_strict rejects anything
 * not listed here, so each table is also that bag's option documentation. */
static const char *const http_srv_keys[] =
    { "port", "workers", "backlog", "requestTimeoutMs", "host", "routes" };
static const char *const http_async_keys[] =
    { "port", "backlog", "host", "idleTimeoutMs", "maxConns", "routes" };
static const char *const http_app_keys[] =
    { "port", "idleTimeoutMs", "maxConns", "host", "compress", "metrics",
      "workers", "backlog" };
static const char *const http_route_val_keys[] = { "status", "contentType", "body" };
static const char *const http_static_keys[]   = { "maxFileSize", "allow" };
static const char *const http_upload_keys[]   = { "dir", "maxFileSize", "allow" };
static const char *const http_proxy_keys[]    = { "host", "port" };
static const char *const http_ws_handler_keys[]   = { "open", "message", "close" };
static const char *const http_sse_handler_keys[]  = { "open", "close" };
static const char *const http_cookie_keys[]       = { "maxAge", "domain", "path", "sameSite", "secure", "httpOnly" };

/* ==================================================================== *
 *  Small shared helpers (libc only -- reused by client and server)      *
 * ==================================================================== */

/* Grow-only byte buffer backed by libc malloc (thread-safe; used off the JS
 * thread by the server). Returns -1 on OOM. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dyn_bytes_t;

static int dyn_bytes_reserve(dyn_bytes_t *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t nc;
    char *nd;

    if (need < b->len)          /* size_t overflow: an absurd request, refuse */
        return -1;
    if (need <= b->cap)
        return 0;
    nc = b->cap ? b->cap * 2 : 4096;
    while (nc < need) {
        size_t doubled = nc * 2;
        if (doubled < nc)       /* doubling overflowed: cannot satisfy */
            return -1;
        nc = doubled;
    }
    nd = (char *)realloc(b->data, nc);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int dyn_bytes_append(dyn_bytes_t *b, const char *src, size_t n)
{
    if (dyn_bytes_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int dyn_req_headers_valid(const char *buf, size_t len);

/* Portable, dependency-free substring search over a possibly-NUL-containing
 * byte range (avoids relying on memmem / _GNU_SOURCE). Delegates to the shared
 * multi-ISA SIMD strfind kernel (Muła first+last): request/response header
 * blocks are hundreds of bytes, long enough that the vectorised scan beats the
 * scalar byte loop ~4.75x on a realistic header set (see tests/bench_memfind.c).
 * Pure C, no JS -- safe to call from the acceptor/worker threads. The `simd`
 * table is installed once (simd_init) on the JS thread before any thread spawns
 * or any request is parsed. */
static const char *dyn_memfind(const char *hay, size_t hlen,
                               const char *needle, size_t nlen)
{
    size_t idx;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    idx = simd.strfind((const uint8_t *)hay, hlen,
                       (const uint8_t *)needle, nlen);
    return idx == SIZE_MAX ? NULL : hay + idx;
}

static int dyn_ci_equal(const char *a, size_t alen, const char *b)
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

/* Write all `len` bytes to `fd`, retrying short writes. Returns 0 or -1. */
/* Disable Nagle's algorithm: an HTTP request/response is a ping-pong of small
 * writes, where Nagle + delayed-ACK can stall a reply ~40ms waiting to coalesce.
 * Every serious HTTP endpoint sets this on both accepted and client sockets.
 * Best-effort (ignore failure: a non-TCP fd or unsupported option is harmless). */
static void dyn_set_nodelay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/* HTTP message codecs (design 15). Included BEFORE the client section: the
 * request builder validates user-supplied methods against the same DYN_TCHAR
 * table these codecs parse with, so the serializer and the parsers cannot
 * drift apart. */
#include "dyna-httpmsg.inc.c"

/* ==================================================================== *
 *  HTTPClient                                                           *
 * ==================================================================== */

static JSClassID dyn_http_client_class_id;

typedef struct {
    int64_t timeout_ms;  /* recv/connect timeout; <=0 => none */
    size_t max_body;     /* cap on accepted response bytes */
    _Atomic int cancelled; /* set by disconnect(); polled by the in-flight job */
    /* AN ASYNC OPERATION MUST OUTLIVE THE CALLER'S REFERENCE. An offloaded job
     * polls `&cancelled` on the WORKER and settles from this native on the
     * LOOP, so the wrapper -- and this struct with it -- must survive the JS
     * reference being dropped mid-flight (`new HTTPClient().getAsync(url)` is
     * legal and used to free the struct out from under the job: dispose ran,
     * and the worker then polled freed memory). While n_async > 0 the wrapper
     * is pinned here (dup'd at submit, released by the completing job or by
     * dispose). The pin is deliberately NOT exposed through .gc_mark: the
     * cycle collector's gc_decref pass subtracts every reference gc_mark
     * reports, so a marked self-reference reads as a one-object cycle and is
     * collected WHILE THE JOB RUNS (proven by ASan: gc_free_cycles freed a
     * pinned client mid-flight). An unmarked C-held ref is invisible to
     * gc_decref, which is exactly what keeps the refcount above zero through
     * a gc() -- the same anchoring FileWriter.syncAsync's `self` relies on. */
    JSValue self_pending;
    int n_async;         /* jobs currently holding self_pending */
    JSContext *ctx;      /* owner context, for the dispose-time release */
#ifdef CONFIG_TLS
    /* Built on FIRST https use, not at construction: loading the platform
       trust store costs milliseconds and most clients never speak TLS. */
    dyn_tls_ctx_t *tls;
#endif
} dyn_http_client_t;

/* Drop one job's claim on the self-pin; the last one out frees the VALUE
 * (clear the slot FIRST: freeing it can let the GC collect the wrapper). */
static void dyn_http_client_unpin(dyn_http_client_t *cl)
{
    JSContext *ctx = cl->ctx;
    if (--cl->n_async > 0)
        return;
    {
        JSValue v = cl->self_pending;
        cl->self_pending = JS_UNDEFINED;
        JS_FreeValue(ctx, v);
    }
}

static void dyn_http_client_dispose(void *native)
{
    dyn_http_client_t *c = (dyn_http_client_t *)native;
    if (c) {
        /* Teardown can run with a pin still held (runtime shutdown frees
         * remaining objects regardless of refcount): release it, like
         * dyn_tcp_teardown does -- freeing JS_UNDEFINED (the settled case)
         * is a no-op. */
        JSValue v = c->self_pending;
        c->self_pending = JS_UNDEFINED;
        JS_FreeValue(c->ctx, v);
#ifdef CONFIG_TLS
        dyn_tls_ctx_free(c->tls);
#endif
    }
    free(native);
}


/* One connection: a socket, and the TLS engine when the scheme asked for it.
   Every read and write in the client goes through hc_send/hc_recv, so the
   request logic above is identical for both schemes. */
typedef struct {
    int fd;
#ifdef CONFIG_TLS
    dyn_tls_conn_t *tls;   /* NULL = plaintext */
#endif
} hc_conn_t;

static ssize_t hc_raw_send(int fd, const void *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t s = send(fd, (const char *)p + off, n - off, 0);
        if (s < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)s;
    }
    return (ssize_t)n;
}

#ifdef CONFIG_TLS
/* Push whatever the engine queued onto the wire. */
static int hc_tls_flush(hc_conn_t *c)
{
    uint8_t out[16384];
    int n;
    while ((n = dyn_tls_pull(c->tls, out, sizeof out)) > 0)
        if (hc_raw_send(c->fd, out, (size_t)n) < 0)
            return -1;
    return 0;
}

/* Drive the handshake to completion on a blocking socket. */
static int hc_tls_handshake(hc_conn_t *c)
{
    uint8_t buf[16384];
    for (;;) {
        int st = dyn_tls_handshake(c->tls);
        ssize_t r;
        if (hc_tls_flush(c) < 0) return -1;
        if (st < 0) return -1;
        if (st == 1) return 0;
        r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0) return -1;
        if (dyn_tls_feed(c->tls, buf, (size_t)r) != 0) return -1;
    }
}
#endif

static ssize_t hc_send(hc_conn_t *c, const void *p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        /* LOOP: dyn_tls_write clamps to an int, so a large body is a partial
           write and returning n regardless would silently drop the tail. */
        size_t off = 0;
        while (off < n) {
            int w = dyn_tls_write(c->tls, (const uint8_t *)p + off, n - off);
            if (w <= 0) return -1;
            off += (size_t)w;
            if (hc_tls_flush(c) != 0) return -1;
        }
        return (ssize_t)n;
    }
#endif
    return hc_raw_send(c->fd, p, n);
}

static ssize_t hc_recv(hc_conn_t *c, void *p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        for (;;) {
            uint8_t cipher[16384];
            ssize_t r;
            int got = dyn_tls_read(c->tls, (uint8_t *)p, n);
            if (got > 0) return got;
            if (got < 0) return -1;
            r = recv(c->fd, cipher, sizeof cipher, 0);
            if (r <= 0) return r;          /* 0 = clean close */
            if (dyn_tls_feed(c->tls, cipher, (size_t)r) != 0) return -1;
        }
    }
#endif
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

static const JSClassDef dyn_http_client_class = {
    "HTTPClient",
    .finalizer = dyn_res_finalizer,
    /* No .gc_mark on purpose: see self_pending above -- a marked pin is
       collected as a cycle mid-op; the unmarked C ref is the anchor. */
};

static JSValue dyn_http_client_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    int64_t max_body = 0; /* 0 => default */

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt64(ctx, &max_body, argv[0]))
            return JS_EXCEPTION;
        if (max_body < 0)
            max_body = 0;
    }
    /* calloc, not malloc: every field must start zero. `tls` is built lazily
       and tested with `if (!cl->tls)`, so uninitialised memory read as
       "already built" and dereferenced garbage -- ASan SEGV, invisible in a
       normal build where the allocation happened to come back zeroed. */
    cl = (dyn_http_client_t *)calloc(1, sizeof(*cl));
    if (!cl)
        return JS_ThrowOutOfMemory(ctx);
    cl->timeout_ms = DYN_HTTP_DEFAULT_TIMEOUT_MS;
    cl->max_body = max_body ? (size_t)max_body : DYN_HTTP_DEFAULT_MAX_BODY;
    cl->self_pending = JS_UNDEFINED;    /* calloc cannot write this tag */
    cl->ctx = ctx;
    return dyn_res_wrap(ctx, new_target, dyn_http_client_class_id, cl,
                        dyn_http_client_dispose);
}

/* A request-line method must be a non-empty sequence of RFC 9110 tchar: it is
 * emitted verbatim into the request line, so anything else (a space, a CR/LF)
 * is a request-splitting injection (CWE-93). */
static int dyn_method_valid(const char *m)
{
    size_t i;
    if (!m || !*m)
        return 0;
    for (i = 0; m[i]; i++)
        if (!DYN_TCHAR[(unsigned char)m[i]])
            return 0;
    return 1;
}

/* Fire-and-forget JS invocation for handler callbacks whose result nobody
 * consumes. The exception state is CLEARED: a JS_EXCEPTION dropped without
 * JS_GetException leaves the context's pending exception stuck, surfacing
 * later as a bogus throw at an unrelated boundary. */
static void dyn_call_drop(JSContext *ctx, JSValueConst fn,
                          JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue r = JS_Call(ctx, fn, this_val, argc, argv);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

/* --- URL parsing: "http[s]://host[:port][/path]" --- */

/* The path is BORROWED from the caller's url -- it runs to that string's NUL,
   so it needs no copy. Inlining 2048 bytes for it made this struct 2306 bytes
   across 37 cache lines, for a value that lives on the request stack. */
#define DYN_URL_MAX_PATH 2048

typedef struct {
    const char *path;                   /* borrowed from the url, or "/" */
    uint16_t    port;
    uint8_t     tls;                    /* in port's padding: the assert below
                                           still holds at the same size */
    char        host[256];              /* copied: getaddrinfo wants a C string */
} dyn_url_t;

_Static_assert(sizeof(dyn_url_t) <= 288,
               "dyn_url_t is a request-stack local: keep it near one page");

static int dyn_parse_url(const char *url, dyn_url_t *out)
{
    const char *p = url;
    const char *host_start, *host_end;
    size_t host_len, path_len;

    out->tls = 0;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
#ifdef CONFIG_TLS
    else if (strncmp(p, "https://", 8) == 0) { p += 8; out->tls = 1; }
#else
    else if (strncmp(p, "https://", 8) == 0)
        return -1; /* this build has no TLS: rebuild with CONFIG_TLS=y */
#endif
    else
        return -1; /* require an explicit scheme */

    host_start = p;
    while (*p && *p != ':' && *p != '/')
        p++;
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return -1;
    {
        /* CR/LF -- or any byte outside printable ASCII -- in the host would be
           emitted verbatim into the request line and Host header (CWE-93). */
        const char *h;
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
        return 0;
    }
    if (*p != '/')
        return -1;
    path_len = strlen(p);
    if (path_len >= DYN_URL_MAX_PATH)    /* the refusal the old buffer gave */
        return -1;
    {
        const char *q;
        for (q = p; q < p + path_len; q++)
            if ((unsigned char)*q < 0x21 || (unsigned char)*q > 0x7e)
                return -1;               /* CRLF injection via the path */
    }
    out->path = p;
    return 0;
}

/* Non-blocking connect with a timeout. Returns a connected fd, or -1 with
 * *perr set. */
static int dyn_tcp_connect(const char *host, uint16_t port, int64_t timeout_ms,
                           int *perr)
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
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        /* The aio sockets set this; this blocking client creates its own
           fd, and a write racing the peer's RST must not kill the process
           (the global SIG_IGN in js_std_init_handlers is the backstop). */
        { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
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
            /* (int)timeout_ms truncates: a huge timeout arrived as 0 or
             * negative, i.e. "don't wait at all" / "wait forever". */
            int pt = timeout_ms > INT_MAX ? INT_MAX
                     : timeout_ms < -1 ? -1 : (int)timeout_ms;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            pr = poll(&pfd, 1, pt);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                    soerr == 0) {
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

/* Framing headers the request builder emits itself (Host, Content-Length,
 * Connection, and the encodings that would fight our framing). A
 * caller-supplied copy reached the wire NEXT TO ours -- two Content-Lengths,
 * a client-declared Transfer-Encoding beside a length-framed body, two Hosts
 * -- and any intermediary that resolves the disagreement differently than the
 * origin is a request-smuggling primitive (CWE-444). Dropped, never
 * forwarded. */
static int dyn_ci_eq(const char *a, const char *b, size_t n); /* below */
static int dyn_hdr_is_client_framing_name(const char *n, size_t len)
{
    static const char *const names[] = {
        "host", "content-length", "transfer-encoding",
        "connection", "keep-alive",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        if (strlen(names[i]) == len && dyn_ci_eq(n, names[i], len))
            return 1;
    return 0;
}

/* Build the extra-header block ("Name: Value\r\n...") from the JS `headers`
 * argument (a { name: value } object or a pre-formatted string). Returns a
 * malloc'd NUL-terminated string the caller free()s, or NULL. NULL with
 * *perr == 0 means "no extra headers"; NULL with *perr == 1 means a JS
 * exception is pending. Rejects CR/LF in names/values (header injection). */
static char *dyn_headers_to_string(JSContext *ctx, JSValueConst headers,
                                   int *perr)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    dyn_bytes_t buf = {0};

    *perr = 0;
    if (JS_IsUndefined(headers) || JS_IsNull(headers))
        return NULL;

    if (JS_IsString(headers)) {
        const char *s = JS_ToCString(ctx, headers);
        dyn_bytes_t b = {0};
        size_t i, n;
        if (!s) {
            *perr = 1;
            return NULL;
        }
        n = strlen(s);
        /* The pre-formatted form reaches the wire verbatim, so it gets the
           SAME refusal the object form applies -- except that ONE trailing
           CRLF, which is how a well-formed block ends. Anything else (an
           interior CR/LF, a blank line that would swallow our framing) is a
           header injection (CWE-93), not a formatting choice. */
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
        /* Re-emit line by line (CRLF-terminated): a FRAMING line is dropped
           (the builder writes its own -- a caller-supplied copy desyncs
           intermediaries, CWE-444), and a final line WITHOUT its CRLF gets
           the terminator appended. The old verbatim copy glued our built
           headers onto the caller's last line -- "X: vConnection: close" --
           silently discarding the framing header. */
        i = 0;
        while (i < n) {
            size_t ls = i, le;
            const char *colon;
            size_t nlen;
            while (i + 1 < n && !(s[i] == '\r' && s[i + 1] == '\n'))
                i++;
            if (i + 1 < n) {
                le = i;                        /* CRLF at (i, i+1) */
                i += 2;
            } else {
                le = n;                        /* final line without CRLF */
                i = n;
            }
            if (le > ls) {
                colon = memchr(s + ls, ':', le - ls);
                nlen = colon ? (size_t)(colon - (s + ls)) : 0;
                if (!colon || !dyn_hdr_is_client_framing_name(s + ls, nlen)) {
                    if (dyn_bytes_append(&b, s + ls, le - ls) < 0 ||
                        dyn_bytes_append(&b, "\r\n", 2) < 0)
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
        return NULL; /* numbers/bools etc.: treat as "no extra headers" */
    if (JS_IsArray(ctx, headers)) {
        /* An array's enumerable own names are "0","1",...: enumerated as
           headers it emits numeric-named junk on the wire. A caller passing
           a list meant name/value pairs -- refuse rather than mangle. */
        *perr = 1;
        JS_ThrowTypeError(ctx,
            "headers must be an object of name/value pairs or a "
            "pre-formatted string, not an array");
        return NULL;
    }

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const char *val;
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
        if (memchr(name, '\r', nl) || memchr(name, '\n', nl) ||
            memchr(val, '\r', vl) || memchr(val, '\n', vl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                              "header name/value must not contain CR or LF");
            return NULL;
        }
        if (dyn_hdr_is_client_framing_name(name, nl)) {
            /* the builder emits its own Host/Content-Length/Connection: a
             * caller-supplied copy desyncs framing (CWE-444); dropped */
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            continue;
        }
        if (dyn_bytes_append(&buf, name, nl) < 0 ||
            dyn_bytes_append(&buf, ": ", 2) < 0 ||
            dyn_bytes_append(&buf, val, vl) < 0 ||
            dyn_bytes_append(&buf, "\r\n", 2) < 0) {
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
            return NULL; /* *perr stays 0: treated as no-headers on OOM */
        }
    }
    return buf.data; /* NUL-terminated, or NULL if no own string keys */

 fail:
    free(buf.data);
    JS_FreePropertyEnum(ctx, tab, len);
    *perr = 1;
    return NULL;
}

/* Turn a code into a thrown JS Error with a numeric `.dynajsError`. Returns
 * JS_EXCEPTION. */
/* The error as a VALUE (the async path rejects with it); dyn_http_throw is
   the throwing wrapper. tlswhy keeps WHICH TLS check failed -- "handshake
   failed" is not actionable, "certificate has expired" is. */
static const char *hc_err_name(int code)
{
    static const char *const names[] = {
        "ok", "bad URL", "DNS resolution failed", "connection failed",
        "send failed", "receive failed", "malformed response",
        "out of memory", "response too large",
        "TLS handshake failed", "aborted", "truncated body"};
    return (code >= 0 && code < (int)countof(names)) ? names[code] : "error";
}

/* The message with `what` spelled by the CALLER: some failures know exactly
   which rule was hit, and the per-code name ("malformed response") cannot say
   it -- a chunked body that ended before its terminal chunk is a truncated
   exchange, not a generic parse failure. */
static JSValue hc_error_value_named(JSContext *ctx, int code, const char *method,
                                    const char *url, const char *what)
{
    JSValue e;
    char msg[512];

    snprintf(msg, sizeof(msg), "HTTP %s %s failed: %s", method,
             url ? url : "", what);
    e = JS_NewError(ctx);
    if (JS_IsException(e))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, e, "dynajsError", JS_NewInt32(ctx, code),
                              JS_PROP_C_W_E);
    return e;
}

static JSValue hc_error_value(JSContext *ctx, int code, const char *method,
                              const char *url, const char *tlswhy)
{
    const char *what = hc_err_name(code);

    if (code == DYN_HTTP_ERR_TLS && tlswhy && *tlswhy)
        what = tlswhy;
    return hc_error_value_named(ctx, code, method, url, what);
}

static JSValue dyn_http_throw(JSContext *ctx, int code, const char *method,
                              const char *url, const char *tlswhy)
{
    JSValue e = hc_error_value(ctx, code, method, url, tlswhy);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

/* dyn_http_throw, with the failure named by the caller (see
   hc_error_value_named). */
static JSValue dyn_http_throw_named(JSContext *ctx, int code,
                                    const char *method, const char *url,
                                    const char *what)
{
    JSValue e = hc_error_value_named(ctx, code, method, url, what);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

/* Parse a single header block (NUL-free byte range between the status line and
 * the blank line) into a fresh JS object. */
static JSValue dyn_headers_object(JSContext *ctx, const char *p, const char *end)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    while (p < end) {
        const char *eol = dyn_memfind(p, (size_t)(end - p), "\r\n", 2);
        const char *line_end = eol ? eol : end;
        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char *val = colon + 1;
            size_t val_len = (size_t)(line_end - val);
            JSAtom key;
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }
            if (name_len > 0) {
                key = JS_NewAtomLen(ctx, p, name_len);
                if (key != JS_ATOM_NULL) {
                    JS_DefinePropertyValue(ctx, obj, key,
                                           JS_NewStringLen(ctx, val, val_len),
                                           JS_PROP_C_W_E);
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

/* De-chunk a Transfer-Encoding: chunked body into a fresh malloc'd buffer.
 *
 * The framing rule is not advisory: a body with no terminal 0-length chunk has
 * NO defined length, and returning the bytes seen so far would report a cut
 * exchange as a complete 200 -- exactly how a streamed handler's failure is
 * signalled on the wire (the connection truncates; the peer is meant to see
 * it). So the outcome is reported instead of guessed:
 *   DECHUNK_OK         the terminal chunk and its trailer section were seen
 *   DECHUNK_TRUNCATED  the body ended before the terminal chunk (EOF where a
 *                      chunk-size line, chunk data, or the closing CRLF of the
 *                      trailer section belongs)
 *   DECHUNK_MALFORMED  bytes that are not `hex[;ext]CRLF` + chunk-data CRLF
 *   DECHUNK_OOM        allocation failure
 * On OK, *out owns the bytes (NULL when the body was legitimately empty) and
 * *out_len is their count; on every error both are zeroed. */
enum { DECHUNK_OK = 0, DECHUNK_TRUNCATED = 1, DECHUNK_MALFORMED = 2,
       DECHUNK_OOM = 3 };

static int dyn_dechunk(const char *p, size_t len, char **out, size_t *out_len)
{
    dyn_bytes_t o = {0};
    size_t i = 0;

    *out = NULL;
    *out_len = 0;
    for (;;) {
        size_t chunk = 0, j = i;
        int digits = 0;
        /* Significant hex digits past the leading zeros. The accumulator
           below is exact for 13 digits (16^13-1 < 2^52); a 14th significant
           digit cannot add information to any legal frame -- a size that
           big dwarfs every body cap -- and a long enough digit string
           WRAPS the accumulator outright: a declared 2^64 ("1" followed by
           16 zeros) lands on 0 and is consumed as the TERMINAL chunk, so a
           hostile frame decodes as a clean empty 200. More than 13
           significant digits is therefore MALFORMED, not merely absurd
           (t3 review F1). Leading zeros stay legal -- RFC 9112 allows
           1*HEXDIG -- and do not count toward the cap. */
        int sig = 0;
        if (i >= len)
            goto truncated;   /* EOF where a chunk-size belongs */
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
                sig++;        /* zeros before the first nonzero are padding */
            chunk = chunk * 16 + (size_t)d;
            digits++;
            j++;
        }
        if (!digits)
            goto malformed;   /* no chunk-size where one belongs */
        if (sig > 13)
            goto malformed;   /* past the accumulator's exact range: refuse,
                                 never wrap (see the note above) */
        /* After the hex digits only a chunk extension or the CRLF may follow
           (RFC 9112 7.1) -- a stray byte there is not a size line. */
        if (j >= len)
            goto truncated;   /* the size line never ended */
        if (p[j] != '\r' && p[j] != ';')
            goto malformed;
        /* skip to end of the chunk-size line (extension bytes, then LF) */
        while (j < len && p[j] != '\n')
            j++;
        if (j >= len)
            goto truncated;
        i = j + 1; /* past the \n */
        if (chunk == 0) {
            /* last chunk: the trailer section ends at the first empty line */
            for (;;) {
                const char *eol = dyn_memfind(p + i, len - i, "\r\n", 2);
                if (!eol)
                    goto truncated;
                if (eol == p + i) {   /* the empty line: end of trailers */
                    *out = o.data;
                    *out_len = o.len;
                    return DECHUNK_OK;
                }
                i = (size_t)(eol - p) + 2;   /* skip a trailer field line */
            }
        }
        /* `chunk > len - i`, never `i + chunk > len`: the latter overflows for
           an attacker-chosen 64-bit chunk size and lets an absurd length slip
           past the clamp into an unbounded append (CWE-190 -> CWE-835). i <= len
           holds by construction, so `len - i` cannot underflow. Here an
           over-long chunk is not clamped but reported: less data than the frame
           promises is a TRUNCATED body, and silently appending the shortfall
           was how a cut exchange read as a complete one. */
        if (chunk > len - i)
            goto truncated;
        if (i + chunk + 2 > len)
            goto truncated;   /* chunk data missing its CRLF */
        if (p[i + chunk] != '\r' || p[i + chunk + 1] != '\n')
            goto malformed;   /* chunk data not CRLF-terminated */
        if (dyn_bytes_append(&o, p + i, chunk) < 0)
            goto oom;
        i += chunk + 2;
    }
/* One cleanup for every failure: a body that was already partly de-chunked
   has bytes in `o`, and each of these exits used to return them to nobody
   (found by LeakSanitizer, not by the rows). */
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

/* Read the full response off `fd` into `resp`, honouring Content-Length,
 * Transfer-Encoding: chunked, and Connection: close. Returns 0 or an error
 * code. On success *resp owns a malloc'd buffer (caller free()s resp->data).
 * *pchunked reports the framing decision so the response builder does not
 * have to rescan the header block for it. */
#define HC_CANCELLED(cancel) \
    ((cancel) && atomic_load_explicit((cancel), memory_order_relaxed))

static int dyn_read_response(hc_conn_t *conn, size_t max_body, dyn_bytes_t *resp,
                             size_t *phdr_end, int *pchunked,
                             const _Atomic int *cancel,
                             char *truncwhy, size_t truncwhy_n)
{
    const char *hdr_marker;
    size_t hdr_end = 0;
    /* Bytes already proven free of the terminator: "\r\n\r\n" is 4 bytes, so
     * only a 3-byte tail can straddle into the next chunk -- resume there
     * instead of rescanning the whole buffer per recv. */
    size_t scanned = 0;

    /* 1. read until end-of-headers. The search runs on what is ALREADY
     * buffered before each recv: skipping an interim 1xx can leave the final
     * head fully buffered, and the old recv-first order would block on a
     * socket that owes us nothing more. */
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
                return DYN_HTTP_ERR_PARSE; /* closed before full headers */
            resp->len += (size_t)r;
            if (resp->len > max_body)
                return DYN_HTTP_ERR_TOOBIG;
            /* Before the body cap: until the terminator is found every byte
             * here is header, and 16 MB of it is a memory amplification the
             * caller never asked for. */
            if (resp->len > DYN_HTTP_MAX_RESP_HEADER)
                return DYN_HTTP_ERR_TOOBIG;
            continue;
        }
        {
            hdr_end = (size_t)(hdr_marker - resp->data) + 4;
            /* RFC 9110 15.2 / RFC 9112: an interim 1xx response (any status
             * in [100,199] except 101 Switching Protocols) is terminated by
             * the end of its header section, and a client skips it to await
             * the final response. Treating it as final swallowed the REAL
             * response into the body and reported status 100. */
            const char *sp = memchr(resp->data, ' ',
                                    hdr_end < 32 ? hdr_end : 32);
            if (sp && resp->len >= hdr_end &&
                memcmp(resp->data, "HTTP/", 5) == 0 &&
                (size_t)(sp - resp->data) <= 12 &&
                sp + 4 <= resp->data + hdr_end &&
                sp[1] == '1' && sp[2] >= '0' && sp[2] <= '9' &&
                sp[3] >= '0' && sp[3] <= '9' &&
                !(sp[1] == '1' && sp[2] == '0' && sp[3] == '1')) {
                memmove(resp->data, resp->data + hdr_end,
                        resp->len - hdr_end);
                resp->len -= hdr_end;
                hdr_end = 0;
                scanned = 0;
                continue;          /* re-scan: the final head may be buffered */
            }
        }
        break;
    }
    *phdr_end = hdr_end;

    /* 2. inspect headers for framing */
    {
        const char *hstart = dyn_memfind(resp->data, hdr_end, "\r\n", 2);
        const char *hp = hstart ? hstart + 2 : resp->data;
        const char *hend = resp->data + hdr_end - 2; /* before blank line */
        size_t content_length = 0;
        int have_cl = 0, chunked = 0;

        /* Validate header line syntax (RFC 9112: no OWS before colon) */
        if (!dyn_req_headers_valid(resp->data, hdr_end))
            return DYN_HTTP_ERR_PARSE;

        while (hp < hend) {
            const char *eol = dyn_memfind(hp, (size_t)(hend - hp), "\r\n", 2);
            const char *line_end = eol ? eol : hend;
            const char *colon = memchr(hp, ':', (size_t)(line_end - hp));
            if (colon) {
                size_t nlen = (size_t)(colon - hp);
                const char *val = colon + 1;
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
                        return DYN_HTTP_ERR_PARSE; /* RFC 9112 6.3: conflicting duplicate Content-Length */
                    content_length = parsed_cl;
                    have_cl = 1;
                } else if (dyn_ci_equal(hp, nlen, "transfer-encoding")) {
                    if (dyn_memfind(val, vlen, "chunked", 7))
                        chunked = 1;
                }
            }
            if (!eol)
                break;
            hp = eol + 2;
        }
        /* Both framings on one response is the classic desync attempt
           (CWE-444). RFC 9112 6.1 lets a recipient process it (chunked
           wins) or reject; as the CLIENT of a possibly hostile server we
           reject -- there is no honest way to be sure we frame it the way
           any intermediary did. */
        if (chunked && have_cl)
            return DYN_HTTP_ERR_PARSE;
        *pchunked = chunked;

        /* 3. read the body per the framing rule */
        if (chunked || !have_cl) {
            /* read until the peer closes (Connection: close) */
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
                if (r == 0)
                    break;
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
                    /* t3 review F2, decided: the framing rule is not
                       advisory. A connection ending before the declared
                       Content-Length is a TRUNCATED exchange -- the same
                       refusal the chunked path makes at its terminal chunk
                       and the streaming reader makes per read -- because
                       returning the bytes seen so far reports a cut
                       exchange as a complete 200. (Read-to-close remains
                       the framing ONLY where no length was declared.) */
                    snprintf(truncwhy, truncwhy_n,
                             "truncated body: connection ended after %llu of "
                             "%llu declared bytes",
                             (unsigned long long)(resp->len - hdr_end),
                             (unsigned long long)content_length);
                    return DYN_HTTP_ERR_TRUNC;
                }
                resp->len += (size_t)r;
            }
        }
    }
    return 0;
}

/* Parse the accumulated raw response into a JS { status, statusText, ok,
 * headers, body } object. `chunked` is the framing decision
 * dyn_read_response already made -- rescanning the header block here would be
 * a second parser for the same fact. `method`/`url` are the request's, for
 * errors only: a refusal must name the exchange that failed. */
static JSValue dyn_build_response(JSContext *ctx, const char *raw, size_t len,
                                  size_t hdr_end, int chunked,
                                  const char *method, const char *url)
{
    JSValue obj, headers;
    const char *line_end = dyn_memfind(raw, len, "\r\n", 2);
    const char *sp1, *sp2;
    int status = 0;
    const char *status_text = "";
    size_t status_text_len = 0;
    const char *body;
    size_t body_len;
    char *dechunked = NULL;

    if (!line_end)
        return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);

    /* The status line MUST be "HTTP/x.y SP code" ("HTTP/1.1 200", "HTTP/2
     * 200" ...): version digits, an optional dot, ONE space, then exactly
     * three status digits. Anything else is a protocol failure and rejects
     * (the API contract: "network and protocol failures reject"), rather
     * than parsing a non-HTTP or truncated exchange into a fabricated
     * {status:0} success. curl reports "Invalid HTTP response" here;
     * python http.client raises BadStatusLine. */
    if (line_end - raw < 9 || memcmp(raw, "HTTP/", 5) != 0)
        return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
    {
        const char *d = raw + 5;
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
        if (!(line_end - d >= 3 && d[0] >= '0' && d[0] <= '9' &&
              d[1] >= '0' && d[1] <= '9' && d[2] >= '0' && d[2] <= '9'))
            return dyn_http_throw(ctx, DYN_HTTP_ERR_PARSE, "response", NULL, NULL);
    }

    /* status line: HTTP/1.x <code> <reason> */
    sp1 = memchr(raw, ' ', (size_t)(line_end - raw));
    if (sp1) {
        /* A hostile server controls this line. The digit count is capped so
         * an absurd code cannot overflow the accumulator (CWE-190): five
         * digits cover every status ever defined with room to spare. */
        const char *code = sp1 + 1;
        unsigned acc = 0;
        int nd = 0;
        while (code < line_end && *code >= '0' && *code <= '9') {
            if (nd < 5) {
                acc = acc * 10 + (unsigned)(*code - '0');
                nd++;
            }
            code++;
        }
        status = nd ? (int)acc : 0;
        sp2 = (code < line_end && *code == ' ') ? code + 1 : code;
        status_text = sp2;
        status_text_len = (size_t)(line_end - sp2);
    }

    body = raw + hdr_end;
    body_len = len - hdr_end;
    /* Responses that cannot carry a body (RFC 9110: 204 No Content and 304
       Not Modified have none; a HEAD response has only the head) must not be
       judged against the chunked framing rule: a legal HEAD answer to a
       `Transfer-Encoding: chunked` resource arrives with the header and ZERO
       body bytes, and demanding the terminal chunk there would refuse a
       perfectly good response. The bytes after the head of such a response are
       never a body, so none are exposed. */
    {
        int may_have_body = status != 204 && status != 304 &&
                            !(method && dyn_ci_equal(method, strlen(method),
                                                     "HEAD"));
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
    /* M10-05: the bytes-native body. `body` above is a JS string, so every
     * byte >= 0x80 rode JS_NewStringLen's UTF-8 conversion and could never be
     * recovered downstream -- fetch's bytes() could only re-encode the mangled
     * text. bodyBytes is the EXACT octets the server sent (post-dechunk), so
     * fetch and raw HTTPClient callers can reach binary content at all.
     *
     * JS_NewArrayBufferCopy, not an external buffer with a free hint: the
     * bytes live in x.resp.data (headers + body in one malloc) which the
     * caller frees as soon as this function returns, and the dechunked case
     * lives in a transient buffer freed below -- a zero-copy external buffer
     * would either alias memory that dies here or force a body-sized malloc
     * plus finalizer plumbing anyway. One copy, owned by the JS value, no
     * lifetime coupling to the exchange. */
    {
        JSValue bb = JS_NewArrayBufferCopy(ctx,
                                           (const uint8_t *)(body ? body : ""),
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

/* The blocking request/response exchange as pure C: no JSContext, no JSValue,
   so the async methods run it unchanged on an io-pool worker. All JS coercion
   happens before submit, all JS value building after completion -- both on
   the loop thread. url/hdr/body are BORROWED (the caller outlives the call);
   u.path borrows url. */
typedef struct {
    const char *method;
    char *url, *hdr, *body;
    size_t body_len;
    int64_t timeout_ms;
    size_t max_body;
    dyn_url_t u;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t *tls_ctx;         /* borrowed from the client */
#endif
    dyn_bytes_t resp;               /* out: the raw response (caller frees) */
    size_t hdr_end;                 /* out: headers/body boundary in resp */
    int chunked;                    /* out: the framing decision */
    int err;                        /* out: a DYN_HTTP_ERR_* code, 0 on ok */
    char tlswhy[192];               /* out: WHICH TLS check failed */
    char truncwhy[128];             /* out: served/declared counts for the
                                       truncated-body refusal (same shape
                                       as the streaming reader's message) */
    const _Atomic int *cancel;      /* borrowed: polled in every recv loop */
    /* P3: connect-time resolved-IP callback. Runs between TCP connect and the
       first send; returning nonzero refuses the connection. NULL = no hook. */
    int (*on_connect)(const char *ip, void *ud);
    void *on_connect_ud;
    char peer_ip[64];               /* out: the resolved address ("N.N.N.N") */
} hc_exch_t;

static void hc_exchange(hc_exch_t *x)
{
    dyn_bytes_t req = {0};
    int fd = -1, err = 0;
    /* ZEROED AT DECLARATION, not after connect: `goto done` frees conn.tls on
       every path, and a garbage pointer there was a free of noise. */
    hc_conn_t conn = {0};
    char line[512];

    x->tlswhy[0] = 0;
    /* fresh connection per request (simplest correct behaviour) */
    fd = dyn_tcp_connect(x->u.host, x->u.port, x->timeout_ms, &err);
    if (fd < 0)
        goto done;
    if (x->on_connect) {
        /* P3: surface the resolved address; the hook may refuse the peer
           BEFORE any request bytes leave. */
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
            char ipbuf[INET6_ADDRSTRLEN];
            const void *ap = NULL;
            if (ss.ss_family == AF_INET6)
                ap = &((struct sockaddr_in6 *)&ss)->sin6_addr;
            else if (ss.ss_family == AF_INET)
                ap = &((struct sockaddr_in *)&ss)->sin_addr;
            if (ap && inet_ntop(ss.ss_family, ap, ipbuf, sizeof ipbuf)) {
                snprintf(x->peer_ip, sizeof x->peer_ip, "%s", ipbuf);
                if (x->on_connect(x->peer_ip, x->on_connect_ud) != 0) {
                    err = DYN_HTTP_ERR_CONNECT;   /* done: copies err to x->err */
                    goto done;
                }
            }
        }
    }
    if (x->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = x->timeout_ms / 1000;
        tv.tv_usec = (x->timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    conn.fd = fd;
#ifdef CONFIG_TLS
    if (x->u.tls) {
        char terr[192];
        if (!x->tls_ctx) {
            snprintf(x->tlswhy, sizeof x->tlswhy, "no TLS context");
            err = DYN_HTTP_ERR_TLS;
            goto done;
        }
        /* The certificate is verified against the URL's host, which is also
           the SNI name -- the two must not be able to disagree. */
        conn.tls = dyn_tls_conn_new(x->tls_ctx, x->u.host, terr, sizeof terr);
        if (!conn.tls || hc_tls_handshake(&conn) != 0) {
            const char *why = conn.tls ? dyn_tls_error(conn.tls) : terr;
            snprintf(x->tlswhy, sizeof x->tlswhy, "%s",
                     why ? why : "handshake failed");
            err = DYN_HTTP_ERR_TLS;
            goto done;
        }
    }
#endif

    /* Appended piece by piece. A fixed 512-byte line buffer silently TRUNCATED
       any path over ~450 bytes into a well-formed request for the wrong
       resource -- snprintf reports that by return value, which nobody read. */
    if (x->u.port == 80)
        snprintf(line, sizeof(line), "Host: %s\r\n", x->u.host);
    else
        snprintf(line, sizeof(line), "Host: %s:%u\r\n", x->u.host,
                 (unsigned)x->u.port);
    if (dyn_bytes_append(&req, x->method, strlen(x->method)) < 0
        || dyn_bytes_append(&req, " ", 1) < 0
        || dyn_bytes_append(&req, x->u.path, strlen(x->u.path)) < 0
        || dyn_bytes_append(&req, " HTTP/1.1\r\n", 11) < 0
        || dyn_bytes_append(&req, line, strlen(line)) < 0) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }
    if (x->hdr && dyn_bytes_append(&req, x->hdr, strlen(x->hdr)) < 0) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }
    if (x->body) {
        snprintf(line, sizeof(line), "Content-Length: %zu\r\n", x->body_len);
        if (dyn_bytes_append(&req, line, strlen(line)) < 0) {
            err = DYN_HTTP_ERR_OOM;
            goto done;
        }
    }
    if (dyn_bytes_append(&req, "Connection: close\r\n\r\n", 21) < 0 ||
        (x->body && dyn_bytes_append(&req, x->body, x->body_len) < 0)) {
        err = DYN_HTTP_ERR_OOM;
        goto done;
    }

    if (hc_send(&conn, req.data, req.len) < 0) {
        err = DYN_HTTP_ERR_SEND;
        goto done;
    }
    err = dyn_read_response(&conn, x->max_body, &x->resp, &x->hdr_end,
                            &x->chunked, x->cancel,
                            x->truncwhy, sizeof x->truncwhy);

 done:
#ifdef CONFIG_TLS
    dyn_tls_conn_free(conn.tls);      /* NULL-safe; the ctx is the client's */
#endif
    if (fd >= 0)
        close(fd);
    free(req.data);
    x->err = err;
}

/* Create the client's TLS context on first https use. Runs ONLY on the JS
   thread: a pool worker creating it would race every other in-flight request
   on the same client. Returns 0, or fills terr and returns -1. */
#ifdef CONFIG_TLS
static int hc_tls_ensure(JSContext *ctx, dyn_http_client_t *cl, char *terr,
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


/* P3: the C-side connect hook — calls the JS onConnect(ip) handler.
   Runs on the JS thread (the sync exchange path). Returns nonzero to refuse. */
typedef struct { JSContext *ctx; JSValue fn; } hc_onconn_t;

static int hc_js_on_connect(const char *ip, void *ud)
{
    hc_onconn_t *h = (hc_onconn_t *)ud;
    JSValueConst argv[1];
    JSValue r;
    int refuse;

    argv[0] = JS_NewString(h->ctx, ip);
    r = JS_Call(h->ctx, h->fn, JS_UNDEFINED, 1, argv);
    JS_FreeValue(h->ctx, argv[0]);
    if (JS_IsException(r)) {
        JS_FreeValue(h->ctx, JS_GetException(h->ctx));  /* refuse on error */
        return 1;
    }
    refuse = !JS_ToBool(h->ctx, r);   /* false = refuse */
    JS_FreeValue(h->ctx, r);
    return refuse;
}

/* Shared request driver. All JS args are coerced to C locals BEFORE the native
 * handle is resolved. */
static JSValue dyn_http_perform(JSContext *ctx, JSValueConst this_val,
                                const char *method, JSValueConst url_val,
                                JSValueConst body_val, JSValueConst headers_val)
{
    dyn_http_client_t *cl;
    const char *url = NULL;
    const char *body = NULL;
    size_t body_len = 0;
    char *raw_body = NULL;
    size_t raw_body_len = 0;
    char *hdr = NULL;
    int hdr_err = 0;
    hc_exch_t x;
    hc_onconn_t onconn = { NULL, JS_UNDEFINED };
    JSValue result;

    memset(&x, 0, sizeof x);
    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");
    if (!dyn_method_valid(method))
        return JS_ThrowTypeError(ctx, "invalid HTTP method");

    /* --- coerce every JS arg first (may run user valueOf/toString) --- */
    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        if (JS_IsString(body_val)) {
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        } else {
            /* a byte view is a raw body; anything else coerces via ToString.
               JS_GetArrayBufferView throws for non-views -- clear and coerce. */
            size_t boff = 0, blen = 0, bpe = 0, ab = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, body_val, &boff, &blen, &bpe);
            if (!JS_IsException(buf)) {
                uint8_t *base;
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
                if (!base) {            /* detached mid-resolve */
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                if (boff > ab || blen > ab - boff) {
                    JS_ThrowRangeError(ctx, "body view out of bounds");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                raw_body = (char *)malloc(blen + 1);
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

    /* --- resolve the client AFTER all coercions (rejects a closed client) --- */
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
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
    x.url = (char *)url;
    x.hdr = hdr;
    x.body = raw_body ? raw_body : (char *)body;
    x.body_len = raw_body_len ? raw_body_len : body_len;
    x.timeout_ms = cl->timeout_ms;
    x.max_body = cl->max_body;
    /* P3: fire onConnect(ip) between TCP connect and the first send. The sync
       path runs on the JS thread so calling back into JS is safe. Returning
       false refuses the peer (the exchange aborts before any request bytes).
       The handler is read from the JS OBJECT (client.onConnect = fn). */
    {
        JSValue oc = JS_GetPropertyStr(ctx, this_val, "onConnect");
        if (JS_IsFunction(ctx, oc)) {
            onconn.ctx = ctx;
            onconn.fn = oc;       /* freed at the function tail */
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
        result = dyn_build_response(ctx, x.resp.data ? (char *)x.resp.data : "",
                                    x.resp.len, x.hdr_end, x.chunked,
                                    method, url);
        free(x.resp.data);
    }
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, onconn.fn);
    return result;
}

static JSValue dyn_http_client_get(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return dyn_http_perform(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
                            argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue dyn_http_client_post(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return dyn_http_perform(ctx, this_val, "POST", argv[0],
                            argc > 1 ? argv[1] : JS_UNDEFINED,
                            argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue dyn_http_client_request(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *method;
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

/* ---- async: the same exchange on the io pool, settled as a Promise -------
 *
 * The sync methods block the loop thread for the whole round trip, which in
 * a single-threaded runtime freezes every connection the process is serving.
 * The async methods submit hc_exchange to the io pool (dyn_aio_offload) and
 * settle a Promise from the completion on the loop thread. With
 * --io-threads=0 the pool is absent and the job runs inline -- correct, but
 * blocking again; the flag, not the API, decides.
 */

/* Releasing the shared reactor from inside a pool completion is a
   use-after-free (the completion runs inside dyn_pool_drain's walk of the
   channel; dropping the last ref frees that channel). So a completion only
   COUNTS its release and this post-drain hook performs it -- the one point
   the reactor is not iterating anything. Shared by the async HTTP client
   jobs and WsClient teardowns. */
static long dyn_http_async_pending;
static JSContext *dyn_http_async_ctx;
static int dyn_http_async_hooked;

static void dyn_http_async_reap(void *unused)
{
    (void)unused;
    while (dyn_http_async_pending > 0 && dyn_http_async_ctx) {
        dyn_http_async_pending--;
        dyn_net_reactor_release(dyn_http_async_ctx);
    }
    /* Unhook when idle: a registered hook arms a periodic wakeup, which would
       otherwise hold the loop open after the last job finished. */
    if (dyn_http_async_pending == 0 && dyn_http_async_hooked) {
        dyn_http_async_hooked = 0;
        dyn_net_off_drain(&dyn_http_async_hooked);
        dyn_http_async_ctx = NULL;
    }
}

/* Arm the reaper BEFORE the first release can be counted. */
static void dyn_http_async_hook(JSContext *ctx)
{
    if (!dyn_http_async_hooked
        && dyn_net_on_drain(dyn_http_async_reap, &dyn_http_async_hooked) >= 0) {
        dyn_http_async_hooked = 1;
        dyn_http_async_ctx = ctx;
    }
}

/* Count a release for the post-drain reaper. If the reaper could NOT be
   armed (dyn_net_on_drain failed), release directly instead -- the
   reactor's own drain guard defers the free safely whether or not we are
   inside one, and an uncounted release would leak the ref and hold the
   loop open forever. */
static void dyn_http_async_release(JSContext *ctx)
{
    if (dyn_http_async_hooked)
        dyn_http_async_pending++;
    else
        dyn_net_reactor_release(ctx);
}

typedef struct {
    hc_exch_t x;
    JSContext *ctx;
    dyn_http_client_t *cl;          /* native of the pinned wrapper; see unpin */
    JSValue resolve, reject;
    char *method_own;               /* x.method points here */
    int offloaded;
} hc_job_t;

static void hc_job_free(hc_job_t *j)
{
    free(j->x.resp.data);
    free(j->method_own);
    free(j->x.url);
    free(j->x.hdr);
    free(j->x.body);
    free(j);
}

/* WORKER THREAD. Touches nothing but `j`, and calls NO JS_* function. */
static void hc_job_work(void *arg)
{
    hc_job_t *j = (hc_job_t *)arg;
    hc_exchange(&j->x);
}

/* THE shutdown sweep for one async hop (the fsync_job_sweep template): the
 * self-pin is a C-held JSValue the collector deliberately cannot see (a
 * marked self-reference reads as a one-object cycle and is collected while
 * the job runs), so a wrapper with a hop in flight at engine shutdown is
 * NEVER collectable -- its dispose never runs, the pin never releases, and
 * the whole realm graph survives to JS_FreeRuntime's gc_obj_list assert
 * (exit 134; found by the teardown rows, flaky-passing on master only when
 * the hop happened to complete before the exit). Registered at every
 * offloaded submit, idempotent per job; removed at the normal completion.
 *
 * Body: release the pin (ctx form while JS is legal, rt form for the
 * JS_FreeRuntime straggler) and settle-or-release the settle pair exactly
 * as the file module's park template does. Nothing here reads j->x: the
 * worker may still be inside hc_exchange writing it. The C buffers stay
 * with the job -- hc_job_free releases them at the completion, and a
 * completion that can never run (the loop is dead) leaves them to process
 * exit, reachable from the pool's pending list and therefore invisible to
 * LSan. */
static void hc_job_sweep(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    hc_job_t *j = (hc_job_t *)opaque;
    dyn_http_client_t *cl = j->cl;

    if (cl) {
        JSValue v = JS_UNDEFINED;
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
        /* JS is still legal: settle the park with a rejection naming the
         * shutdown, exactly like every other failed park. */
        JSValue e = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(e)) {
            JS_DefinePropertyValueStr(ctx, e, "message",
                JS_NewString(ctx, "HTTP exchange aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            e = JS_GetException(ctx);
        }
        r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, (JSValueConst *)&e);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }
    /* released on every path; a pending promise whose settle died here has
     * its rejection already routed (ctx) or nobody left to observe it (rt) */
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

/* LOOP THREAD. Builds the JS value and settles; JS is safe here. */
static void hc_job_done(void *arg)
{
    hc_job_t *j = (hc_job_t *)arg;
    JSContext *ctx = j->ctx;
    JSValue v, r;
    JSValueConst a1[1];

    JS_RemoveShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
    if (!j->cl && JS_IsUndefined(j->resolve)) {
        /* the shutdown sweep already released the pin and the settle pair:
           only the C buffers are left (never observed to run -- the loop is
           dead by then -- but the guard keeps the free exact if that ever
           changes) */
        hc_job_free(j);
        return;
    }

    /* THE completion path -- inline, offloaded, resolved or rejected all come
     * through here -- so it is the one place the self-pin can be dropped.
     * Before the settle: nothing after this touch of j->cl, and the wrapper
     * stays alive for the settle itself because the pin is released only
     * after the settle's JS values are already in hand. */
    if (j->cl) {
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
            v = dyn_build_response(ctx, j->x.resp.data ? (char *)j->x.resp.data : "",
                                   j->x.resp.len, j->x.hdr_end, j->x.chunked,
                                   j->x.method, j->x.url);
        }
        if (JS_IsException(v)) {
            /* a malformed response must REJECT, not resolve-with-Error: the
             * exception is captured here and routed to j->reject below */
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
        dyn_http_async_release(j->ctx); /* the reaper drops the ref post-drain */
    hc_job_free(j);
}

static JSValue hc_submit_async(JSContext *ctx, JSValueConst this_val,
                               const char *method, JSValueConst url_val,
                               JSValueConst body_val, JSValueConst headers_val)
{
    dyn_http_client_t *cl;
    const char *url = NULL;
    const char *body = NULL;
    size_t body_len = 0;
    char *raw_body = NULL;
    size_t raw_body_len = 0;
    char *hdr = NULL;
    int hdr_err = 0;
    hc_job_t *j;
    JSValue funcs[2], promise;
    struct dyn_aio *aio;

    if (JS_IsUndefined(url_val) || JS_IsNull(url_val))
        return JS_ThrowTypeError(ctx, "url is required");
    if (!dyn_method_valid(method))
        return JS_ThrowTypeError(ctx, "invalid HTTP method");
    /* --- coerce every JS arg first (may run user valueOf/toString) --- */
    url = JS_ToCString(ctx, url_val);
    if (!url)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
        if (JS_IsString(body_val)) {
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        } else {
            /* a byte view (Uint8Array & co, DataView) is a raw body; anything
               else falls through to ToString. JS_GetArrayBufferView throws for
               non-views -- clear and coerce. */
            size_t boff = 0, blen = 0, bpe = 0, ab = 0;
            JSValue buf = JS_GetArrayBufferView(ctx, body_val, &boff, &blen, &bpe);
            if (!JS_IsException(buf)) {
                uint8_t *base;
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
                if (!base) {            /* detached mid-resolve */
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                if (boff > ab || blen > ab - boff) {
                    JS_ThrowRangeError(ctx, "body view out of bounds");
                    JS_FreeCString(ctx, url);
                    return JS_EXCEPTION;
                }
                raw_body = (char *)malloc(blen + 1);
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
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
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

    j = (hc_job_t *)calloc(1, sizeof(*j));
    if (!j)
        goto oom;
    j->x.hdr = hdr;                 /* ownership moves to the job */
    hdr = NULL;
    j->method_own = strdup(method);
    j->x.url = strdup(url);
    j->x.body = NULL;
    if (body || raw_body) {         /* a body may hold NULs: copy by length */
        j->x.body = (char *)malloc((body ? body_len : raw_body_len) + 1);
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
    /* A malformed URL is an argument error, not an IO event: it throws here
       rather than rejecting, exactly like the sync methods. */
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
    j->ctx = ctx;

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
    /* abort wiring: disconnect() sets the client flag, the worker's recv
       loops poll it. Reset first so a reused client starts clean. */
    atomic_store_explicit(&cl->cancelled, 0, memory_order_relaxed);
    j->x.cancel = &cl->cancelled;
    /* AN ASYNC OPERATION MUST OUTLIVE THE CALLER'S REFERENCE: from here the
     * job owns &cl->cancelled and settles from cl, so the wrapper must be
     * pinned for the hop -- `new HTTPClient().getAsync(url)` drops the last
     * JS reference before the worker even starts. Released by hc_job_done on
     * every completion path (and by dispose as the teardown backstop). */
    j->cl = cl;
    cl->n_async++;
    if (cl->n_async == 1)           /* first job pins; the last release drops */
        cl->self_pending = JS_DupValue(ctx, this_val);
    if (body)
        JS_FreeCString(ctx, body);
    if (raw_body)
        free(raw_body);
    JS_FreeCString(ctx, url);

    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) {                     /* no reactor in this context: inline */
        hc_job_work(j);
        hc_job_done(j);
        return promise;
    }
    /* The ref keeps the loop (and the reactor) alive across the hop; the
       completion counts its release and the reaper performs it post-drain. */
    dyn_http_async_hook(ctx);
    j->offloaded = 1;
    /* The hop can now outlive the script: register the teardown sweep while
       the pin is held (the inline path above completed and returned, so it
       never carries a sweep). */
    JS_AddShutdownSweep(JS_GetRuntime(ctx), hc_job_sweep, j);
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

static JSValue dyn_http_client_get_async(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    return hc_submit_async(ctx, this_val, "GET", argv[0], JS_UNDEFINED,
                           argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue dyn_http_client_post_async(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    return hc_submit_async(ctx, this_val, "POST", argv[0],
                           argc > 1 ? argv[1] : JS_UNDEFINED,
                           argc > 2 ? argv[2] : JS_UNDEFINED);
}

static JSValue dyn_http_client_request_async(JSContext *ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    const char *method;
    JSValue res;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
                                 "requestAsync(method, url[, body[, headers]])");
    method = JS_ToCString(ctx, argv[0]);
    if (!method)
        return JS_EXCEPTION;
    res = hc_submit_async(ctx, this_val, method, argv[1],
                          argc > 2 ? argv[2] : JS_UNDEFINED,
                          argc > 3 ? argv[3] : JS_UNDEFINED);
    JS_FreeCString(ctx, method);
    return res;
}

static JSValue dyn_http_client_set_timeout(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    int64_t ms;

    (void)argc;
    if (JS_ToInt64(ctx, &ms, argv[0])) /* coerce FIRST (may close `this`) */
        return JS_EXCEPTION;
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    cl->timeout_ms = ms;
    return JS_UNDEFINED;
}

/* ====: the streaming body path (HTTPClient.getStream) ================
 *
 * get()/request() buffer the whole body before they resolve -- the right
 * shape for API calls, and the wrong one for big or open-ended bodies.
 * getStream(url[, headers]) performs ONE exchange (no redirect chasing,
 * exactly like get(): a 3xx is returned as the response it is), reads the
 * head, and returns the response and the body in one object:
 *
 *   { status, statusText, ok, headers, url, contentType,
 *     read(buf) -> Promise<number>,      // 0 = end of body
 *     close(), dispose(), [Symbol.dispose], closed }
 *
 * `read`/`close` are dyna:stream's ByteSource shape (duck-typed -- this
 * module neither imports nor links dyna:stream), so pipe(), lines(),
 * ndjson() and inflate() consume the response directly:
 *
 *   const r = client.getStream(url);
 *   for await (const line of stream.lines(r)) ...
 *
 * Framing: Content-Length, chunked (incremental dechunking; a malformed
 * size line or terminator rejects) and read-to-EOF (Connection: close) --
 * the same three dyn_read_response decides for the buffered path. The
 * client's max_body is enforced ON the stream: a declared length over the
 * cap refuses before the body starts; received bytes are counted as they
 * are handed out, and crossing the cap rejects the current read and closes
 * the stream. A connection ending before a declared Content-Length is a
 * truncated body: the read REJECTS naming bytes served and declared, never
 * a silent 0. The client's timeout applies (socket timeouts, set exactly
 * as hc_exchange sets them); disconnect() is NOT wired -- aborting a
 * half-read stream is `stream.close()`, the caller owns it.
 *
 * The lifetime rule is the house one: read() coerces the buffer FIRST (the
 * coercion can run user JS that may close() `this`), then resolves the
 * native handle, then runs ONE bounded recv, then settles inline -- no raw
 * socket pointer outlives a user-JS call, and a failed read tears the
 * connection down before the rejection reaches the caller.
 */

typedef struct {
    hc_conn_t conn;
    int        chunked;
    uint64_t   clen;        /* framing end; UINT64_MAX = read to EOF */
    uint64_t   served;
    uint64_t   max_body;
    int        eof;
    int        failed;
    uint64_t   chunk_left;
    int        need_size;
    uint8_t   *rbuf; size_t rn, rcap, rpos;
    char      *err;         /* sticky failure text for later reads */
} hc_stream_t;

static JSClassID dyn_hc_stream_class_id;

static void hc_stream_dispose(void *native)
{
    hc_stream_t *s = (hc_stream_t *)native;
    if (!s) return;
#ifdef CONFIG_TLS
    dyn_tls_conn_free(s->conn.tls);
    s->conn.tls = NULL;
#endif
    if (s->conn.fd >= 0) close(s->conn.fd);
    free(s->rbuf);
    free(s->err);
    free(s);
}

static const JSClassDef dyn_hc_stream_class = {
    "HTTPBodyStream", .finalizer = dyn_res_finalizer,
};

static void hc_stream_fail(hc_stream_t *s, const char *msg)
{
    if (!s->failed) {
        s->failed = 1;
        free(s->err);
        s->err = strdup(msg);
    }
}

static int hc_stream_fill(hc_stream_t *s, char *why, size_t whyn)
{
    ssize_t r;
    if (s->rn == s->rcap) {
        size_t nc = s->rcap ? s->rcap * 2 : 8192;
        uint8_t *nb = (uint8_t *)realloc(s->rbuf, nc);
        if (!nb) { snprintf(why, whyn, "out of memory"); return -1; }
        s->rbuf = nb; s->rcap = nc;
    }
    r = hc_recv(&s->conn, s->rbuf + s->rn, s->rcap - s->rn);
    if (r < 0) { snprintf(why, whyn, "receiving body failed"); return -1; }
    if (r == 0) { s->eof = 1; return 0; }
    s->rn += (size_t)r;
    return 0;
}

static int hc_stream_line(hc_stream_t *s, char *line, size_t cap,
                          char *why, size_t whyn)
{
    size_t o = 0;
    for (;;) {
        if (s->rpos >= s->rn) {
            s->rpos = s->rn = 0;
            if (hc_stream_fill(s, why, whyn) < 0) return -1;
            if (s->eof) { snprintf(why, whyn, "truncated chunked body: connection ended mid-line"); return -1; }
            continue;
        }
        {
            uint8_t c = s->rbuf[s->rpos++];
            if (c == '\n') {
                if (o && line[o-1] == '\r') o--;
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

static size_t hc_stream_take(hc_stream_t *s, uint8_t *dst, size_t want)
{
    size_t have = s->rn - s->rpos;
    if (have > want) have = want;
    memcpy(dst, s->rbuf + s->rpos, have);
    s->rpos += have;
    if (s->rpos == s->rn) s->rpos = s->rn = 0;
    return have;
}

/* Every partial drain is accounted BEFORE the refill retry -- advancing
   `got` only on full takes wrote the refill OVER the bytes already moved. */
static long long hc_stream_body(hc_stream_t *s, uint8_t *dst, size_t want,
                                char *why, size_t whyn)
{
    size_t got = 0;
    if (s->eof) return 0;
    while (got < want) {
        if (s->chunked) {
            if (s->need_size) {
                char line[64];
                if (hc_stream_line(s, line, sizeof line, why, whyn) < 0) return -1;
                {
                    char *endp;
                    long long sz = strtoll(line, &endp, 16);
                    if (endp == line || (*endp && *endp != ';' && *endp != ' ')) {
                        snprintf(why, whyn, "malformed chunk size \"%.16s\"", line);
                        return -1;
                    }
                    if (sz == 0) {
                        for (;;) {
                            if (hc_stream_line(s, line, sizeof line, why, whyn) < 0)
                                return -1;
                            if (!line[0]) break;
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
                if (hc_stream_line(s, line, sizeof line, why, whyn) < 0) return -1;
                if (line[0]) {
                    snprintf(why, whyn, "malformed chunk terminator");
                    return -1;
                }
                s->need_size = 1;
                continue;
            }
            {
                size_t room = (size_t)(s->chunk_left < (uint64_t)(want - got)
                                       ? s->chunk_left : (uint64_t)(want - got));
                size_t moved = hc_stream_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                s->chunk_left -= moved;
                if (moved < room) {
                    if (hc_stream_fill(s, why, whyn) < 0) return -1;
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
            if (left == 0) { s->eof = 1; return (long long)got; }
            {
                size_t room = (size_t)((uint64_t)(want - got) < left
                                       ? (uint64_t)(want - got) : left);
                size_t moved = hc_stream_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                if (moved < room) {
                    if (hc_stream_fill(s, why, whyn) < 0) return -1;
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

/* Inline-settled promise helpers (the ByteSource contract: read answers on
 * the promise, never by throwing). */
static JSValue hc_promise_resolved(JSContext *ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, val); return promise; }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue hc_promise_rejected(JSContext *ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;
    if (JS_IsException(exc)) exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, exc); return promise; }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* byte-wide view coercion, coerce-FIRST (the conversion can run user JS) */
static uint8_t *hc_view_bytes(JSContext *ctx, JSValueConst v, size_t *plen)
{
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    *plen = 0;
    if (p) { *plen = n; return p; }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t *base;
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
        if (!base) return NULL;
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}

static JSValue dyn_hc_stream_read(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    size_t len = 0;
    uint8_t *base;
    hc_stream_t *s;
    char why[160];

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "HTTPBodyStream.read: buf must be a byte-wide view (Uint8Array)");
    base = hc_view_bytes(ctx, argv[0], &len);
    if (!base) return JS_EXCEPTION;
    s = (hc_stream_t *)dyn_res_native(ctx, this_val, dyn_hc_stream_class_id);
    if (!s) return JS_EXCEPTION;
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
            if (s->conn.fd >= 0) { close(s->conn.fd); s->conn.fd = -1; }
            exc = JS_ThrowRangeError(ctx, "HTTPBodyStream: %s", why);
            return hc_promise_rejected(ctx, exc);
        }
        if (n == 0) s->eof = 1;
        return hc_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
}

static const JSCFunctionListEntry dyn_hc_stream_proto[] = {
    JS_CFUNC_DEF("read", 1, dyn_hc_stream_read),
};

/* Read the status line + head. Returns a malloc'd block in *raw (*rawlen
   set, NUL-terminated) or NULL with why set. Bounded. */
#define HC_STREAM_HDR_MAX (32 * 1024)
static char *hc_read_head(hc_conn_t *conn, size_t *rawlen,
                          char *why, size_t whyn)
{
    char *buf = (char *)malloc(HC_STREAM_HDR_MAX + 1);
    size_t n = 0;
    if (!buf) { snprintf(why, whyn, "out of memory"); return NULL; }
    while (n < HC_STREAM_HDR_MAX) {
        ssize_t r = hc_recv(conn, buf + n, 1);
        if (r < 0) { snprintf(why, whyn, "receiving response head failed"); goto bad; }
        if (r == 0) { snprintf(why, whyn, "connection closed before response head"); goto bad; }
        n++;
        if (n >= 4 && buf[n-1] == '\n' && buf[n-2] == '\r' &&
            buf[n-3] == '\n' && buf[n-4] == '\r')
            break;
    }
    if (n >= HC_STREAM_HDR_MAX) {
        snprintf(why, whyn, "response head exceeds %d bytes", HC_STREAM_HDR_MAX);
        goto bad;
    }
    buf[n] = 0;
    /* status line sanity, the same shape dyn_build_response demands */
    if (n < 12 || memcmp(buf, "HTTP/", 5) != 0) {
        snprintf(why, whyn, "not an HTTP/1.x response");
        goto bad;
    }
    {
        const char *p = buf + 5;
        if (*p >= '0' && *p <= '9') {
            p++;
            if (*p == '.' && p[1] >= '0' && p[1] <= '9') p += 2;
        }
        if (*p != ' ' || !(p - buf >= 8)) {
            snprintf(why, whyn, "malformed status line");
            goto bad;
        }
    }
    *rawlen = n;
    return buf;
bad:
    free(buf);
    return NULL;
}

/* One header value by name from the raw head block (case-insensitive; the
   last occurrence wins). Returns a malloc'd copy or NULL. */
static char *hc_head_value(const char *raw, size_t n, const char *want)
{
    const char *p = raw;
    const char *end = raw + n;
    char *out = NULL;
    size_t wl = strlen(want);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        const char *colon;
        if (ll && p[ll-1] == '\r') ll--;
        colon = (const char *)memchr(p, ':', ll);
        if (colon) {
            size_t kl = (size_t)(colon - p);
            const char *v = colon + 1;
            size_t vl = ll - kl - 1;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            while (vl && (v[vl-1] == ' ' || v[vl-1] == '\t')) vl--;
            if (kl == wl && strncasecmp(p, want, kl) == 0) {
                free(out);
                out = (char *)malloc(vl + 1);
                if (out) { memcpy(out, v, vl); out[vl] = 0; }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return out;
}

/* getStream(url[, headers]) -> the response object whose read() streams. */
static JSValue dyn_http_client_get_stream(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    const char *url0;
    char *url = NULL, *hdr = NULL;
    int hdr_err = 0;
    dyn_url_t u;
    hc_conn_t conn = { -1,
#ifdef CONFIG_TLS
                       NULL
#endif
    };
    char *raw = NULL;
    size_t rawlen = 0;
    char why[160];
    JSValue ret = JS_EXCEPTION;
    int status = 0;
    char status_text[64] = "";
    char *ctv = NULL, *clv = NULL, *te = NULL;
    hc_stream_t *s = NULL;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getStream(url[, headers])");
    /* Coerce FIRST: the conversions can run user JS that may close() us. */
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0) return JS_EXCEPTION;
    url = strdup(url0);
    JS_FreeCString(ctx, url0);
    if (!url) return JS_ThrowOutOfMemory(ctx);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        hdr = dyn_headers_to_string(ctx, argv[1], &hdr_err);
    if (hdr_err) { free(url); return JS_EXCEPTION; }
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl) { free(url); free(hdr); return JS_EXCEPTION; }

    if (dyn_parse_url(url, &u) < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_URL, "GET", url, NULL);
        free(url); free(hdr);
        return e;
    }
#ifdef CONFIG_TLS
    if (u.tls) {
        char terr[192];
        if (hc_tls_ensure(ctx, cl, terr, sizeof terr) < 0) {
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, "GET", url, terr);
            free(url); free(hdr);
            return e;
        }
    }
#endif
    /* connect + handshake -- hc_exchange's steps, minus the request */
    conn.fd = dyn_tcp_connect(u.host, u.port, cl->timeout_ms, &hdr_err);
    if (conn.fd < 0) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_CONNECT, "GET", url, NULL);
        free(url); free(hdr);
        return e;
    }
    if (cl->timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = cl->timeout_ms / 1000;
        tv.tv_usec = (cl->timeout_ms % 1000) * 1000;
        /* a refused timeout knob costs the deadline, not the connection */
        (void)setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(conn.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    dyn_set_nodelay(conn.fd);
#ifdef CONFIG_TLS
    if (u.tls) {
        char terr[192];
        conn.tls = dyn_tls_conn_new(cl->tls, u.host, terr, sizeof terr);
        if (!conn.tls || hc_tls_handshake(&conn) != 0) {
            const char *w = conn.tls ? dyn_tls_error(conn.tls) : terr;
            JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_TLS, "GET", url,
                                       w ? w : "handshake failed");
            free(url); free(hdr);
            dyn_tls_conn_free(conn.tls);
            close(conn.fd);
            return e;
        }
    }
#endif
    /* the request, appended piece by piece (hc_exchange's builder) */
    {
        dyn_bytes_t req = {0};
        char line[512];
        int bad = dyn_bytes_append(&req, "GET ", 4) < 0 ||
                  dyn_bytes_append(&req, u.path, strlen(u.path)) < 0 ||
                  dyn_bytes_append(&req, " HTTP/1.1\r\n", 11) < 0;
        if (!bad) {
            if (u.port == 80)
                snprintf(line, sizeof(line), "Host: %s\r\n", u.host);
            else
                snprintf(line, sizeof(line), "Host: %s:%u\r\n", u.host,
                         (unsigned)u.port);
            bad = dyn_bytes_append(&req, line, strlen(line)) < 0 ||
                  (hdr && dyn_bytes_append(&req, hdr, strlen(hdr)) < 0) ||
                  dyn_bytes_append(&req, "Connection: close\r\n\r\n", 21) < 0;
        }
        if (!bad && hc_send(&conn, req.data, req.len) < 0)
            bad = 2;
        free(req.data);
        if (bad) {
            JSValue e = dyn_http_throw(ctx,
                                       bad == 2 ? DYN_HTTP_ERR_SEND
                                                : DYN_HTTP_ERR_OOM,
                                       "GET", url, NULL);
            free(url); free(hdr);
#ifdef CONFIG_TLS
            dyn_tls_conn_free(conn.tls);
#endif
            close(conn.fd);
            return e;
        }
    }
    raw = hc_read_head(&conn, &rawlen, why, sizeof why);
    if (!raw) {
        JSValue e = dyn_http_throw(ctx, DYN_HTTP_ERR_RECV, "GET", url, why);
        free(url); free(hdr);
#ifdef CONFIG_TLS
        dyn_tls_conn_free(conn.tls);
#endif
        close(conn.fd);
        return e;
    }
    /* the status code, from the same line grammar dyn_build_response uses */
    {
        const char *sp = raw + 5;
        while (*sp != ' ') sp++;
        sp++;
        status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
        {
            const char *rs = sp + 3;
            const char *eol = (const char *)memchr(rs, '\r', 64);
            size_t rn = eol ? (size_t)(eol - rs) : 0;
            if (rn >= sizeof status_text) rn = sizeof status_text - 1;
            if (rn && *rs == ' ') { rs++; rn--; }
            memcpy(status_text, rs, rn);
        }
    }
    ctv = hc_head_value(raw, rawlen, "content-type");
    clv = hc_head_value(raw, rawlen, "content-length");
    te  = hc_head_value(raw, rawlen, "transfer-encoding");
    s = (hc_stream_t *)calloc(1, sizeof *s);
    if (!s) {
        JS_ThrowOutOfMemory(ctx);
        goto out;
    }
    s->conn = conn;
    conn.fd = -1;
#ifdef CONFIG_TLS
    conn.tls = NULL;
#endif
    s->max_body = cl->max_body;
    s->chunked = te && strcasestr(te, "chunked") != NULL;
    s->need_size = s->chunked;   /* a chunked body opens ON a size line */
    s->clen = (!s->chunked && clv) ? (uint64_t)strtoull(clv, NULL, 10)
                                   : UINT64_MAX;
    if (!s->chunked && s->clen != UINT64_MAX && s->clen > s->max_body) {
        /* the declared-length refusal, before the body starts */
        JS_ThrowRangeError(ctx,
            "HTTPClient.getStream: declared Content-Length %llu exceeds the "
            "client's max body", (unsigned long long)s->clen);
        hc_stream_dispose(s);
        s = NULL;
        goto out;
    }
    {
        JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_hc_stream_class_id,
                                   s, hc_stream_dispose);
        if (JS_IsException(obj)) { hc_stream_dispose(s); s = NULL; goto out; }
        JS_DefinePropertyValueStr(ctx, obj, "status",
                                  JS_NewInt32(ctx, status), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "statusText",
                                  JS_NewString(ctx, status_text), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ok",
                                  JS_NewBool(ctx, status >= 200 && status < 300),
                                  JS_PROP_C_W_E);
        {
            const char *hstart = (const char *)memchr(raw, '\n', rawlen);
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
        s = NULL;                      /* the wrapper owns it now */
    }
out:
    if (s) hc_stream_dispose(s);
    free(raw);
    free(ctv);
    free(clv);
    free(te);
    free(url);
    free(hdr);
    /* on the failure paths the conn was never handed off: tear it down */
#ifdef CONFIG_TLS
    dyn_tls_conn_free(conn.tls);
#endif
    if (conn.fd >= 0) close(conn.fd);
    return ret;
}

static JSValue dyn_http_client_disconnect(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    dyn_http_client_t *cl;
    (void)argc; (void)argv;
    cl = (dyn_http_client_t *)dyn_res_native(ctx, this_val,
                                             dyn_http_client_class_id);
    if (!cl)
        return JS_EXCEPTION;
    /* REAL abort, not a no-op: the in-flight async job's recv loops poll this
       flag and bail with DYN_HTTP_ERR_CANCELLED. A fresh connection per
       request means there is no socket to close here; the worker closes its
       own fd when it unwinds. */
    atomic_store_explicit(&cl->cancelled, 1, memory_order_relaxed);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_http_client_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_http_client_get),
    JS_CFUNC_DEF("post", 1, dyn_http_client_post),
    JS_CFUNC_DEF("request", 2, dyn_http_client_request),
    JS_CFUNC_DEF("getAsync", 1, dyn_http_client_get_async),
    JS_CFUNC_DEF("postAsync", 1, dyn_http_client_post_async),
    JS_CFUNC_DEF("requestAsync", 2, dyn_http_client_request_async),
    JS_CFUNC_DEF("setTimeout", 1, dyn_http_client_set_timeout),
    JS_CFUNC_DEF("disconnect", 0, dyn_http_client_disconnect),
    /*the streaming body path -- one exchange, the head, and a
       dyna:stream-compatible ByteSource over the open connection */
    JS_CFUNC_DEF("getStream", 1, dyn_http_client_get_stream),
};

/* ==================================================================== *
 *  HTTPServer                                                           *
 * ==================================================================== */

static JSClassID dyn_http_server_class_id;

/* One deep-copied route: all bytes are libc-malloc'd C memory, immutable after
 * the server's threads start. Workers only ever READ these. */
typedef struct {
    char *path;
    int status;
    char *content_type;
    char *body;
    size_t body_len;
} dyn_route_t;

typedef struct dyn_http_async dyn_http_async_t;

#define DYN_ACONN_IDLE_MS_DEFAULT   30000  /* matches App's idleTimeoutMs */
#define DYN_ACONN_MAX_CONNS_DEFAULT 8192   /* 0 = unbounded, opt-out only */

/* Per-connection non-blocking state machine. Owns its two buffers. */
typedef struct dyn_aconn_s {
    dyn_http_async_t *srv;
    int fd;
    dyn_bytes_t in;   /* accumulated request bytes (may hold pipelined extras) */
    dyn_bytes_t out;  /* queued response bytes */
    size_t out_off;   /* bytes of `out` already sent */
    size_t hdr_scan_from; /* where the CRLFCRLF search resumes (see the pump) */
    int closing;      /* close once `out` is fully flushed */
    int nreq;
    /* Stamped when a request is CONSUMED, never on byte arrival: a slowloris
       delivers bytes forever without completing anything, so a read-callback
       stamp makes the attacker look permanently active and the timeout inert
       while appearing implemented. */
    uint64_t last_ms;
    struct dyn_aconn_s *lnext, *lprev;   /* server live list, reactor thread */
} dyn_aconn_t;

struct dyn_http_async {
    int listen_fd;
    uint16_t port;
    int backlog;

    dyn_route_t *routes; /* immutable after start */
    size_t n_routes;

    /* Both immutable after start: written by the ctor on the JS thread, read
       only by the reactor thread. 0 disables either defence. A security
       default that is opt-in protects nobody, so both default ON. */
    uint64_t idle_ms;
    int max_conns;
    /* Whole-request cap the pump enforces (431/413 above it). The async
       class default is DYN_ACONN_MAX_REQ; the thread-pool HTTPServer grafts
       this machinery with its historical 16 KiB frame-buffer bound. */
    size_t max_req;
    dyn_aconn_t *live;       /* reactor-thread only */
    int nconns;
    uint64_t last_sweep_ms;
    atomic_ullong n_refused; /* dropped by max_conns, observable from JS */

    dyn_evloop_t *loop;      /* created on the reactor thread (readiness path) */
    void *uring;             /* dyn_uring_ctx* when the io_uring reactor is live */
    pthread_t reactor;
    int started;             /* JS-thread only */
    atomic_int stop_flag;    /* reactor loop breaks on this */
    atomic_int spawn_ok;     /* reactor published its loop init result */
    JSContext *ctx;          /* JS thread, for the reactor keep-alive */
    int reactor_held;        /* start() acquired the shared reactor */
    /* Set by the HTTPServer graft: routes and listen_fd are BORROWED from the
       HTTPServer object (it bound at construction and owns its route copy),
       so stop/dispose neither frees the routes nor closes the listener. */
    int borrowed;
};

static int dyn_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void dyn_http_async_stop_internal(struct dyn_http_async *s);
static int dyn_http_async_spawn(struct dyn_http_async *s);

/* HTTPServer is SERVED by the async machinery: start() grafts a
 * dyn_http_async_t onto this object's ctor-bound listener and deep-copied
 * route table (borrowed = the graft frees neither) and spawns the reactor
 * thread. The thread-per-keep-alive-connection pool this replaces pinned one
 * worker per connection for its whole lifetime, so a closed-loop client
 * count above the worker count starved every other connection into a 503
 * storm (measured: 512 wrk connections against 4 workers served 4 of them
 * and answered ~197k 503s). The reactor holds no thread per connection, and
 * the single reactor thread keeps the class's original property that a
 * SYNCHRONOUS HTTPClient on the JS thread can talk to it -- the JS thread
 * never has to pump the server's I/O.
 *
 * `workers` is still accepted, validated into [1,64] and readable back --
 * exactly the compatibility shape App gives the same option -- but it sizes
 * nothing: there is one reactor thread regardless. Multi-core serving with
 * this class is one process per core (the listener sets SO_REUSEPORT). */
typedef struct {
    int listen_fd;
    uint16_t port;
    int backlog;
    int num_workers;         /* kept for API compatibility; sizes nothing */
    int req_timeout_ms;      /* idle budget, 0 = off (slowloris opt-out) */

    dyn_route_t *routes; /* immutable after start */
    size_t n_routes;

    struct dyn_http_async *async;  /* the graft; NULL until start() */
    int started;             /* JS-thread only */
    JSContext *ctx;          /* JS thread, for the reactor keep-alive */
} dyn_http_server_t;

static const char *dyn_reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    /* Never "OK": a 413 whose reason phrase read OK is what this replaces. A
       class-generic phrase is wrong for no status, whereas "OK" is wrong for
       every one that is not 2xx. */
    default:  return status < 200 ? "Continue"
                   : status < 300 ? "OK"
                   : status < 400 ? "Found"
                   : status < 500 ? "Bad Request" : "Internal Server Error";
    }
}

/* --- worker/acceptor code: NO JS_* CALLS BELOW (different threads) --- */

static const dyn_route_t *dyn_route_lookup(const dyn_route_t *routes,
                                           size_t n_routes, const char *path)
{
    size_t i;
    for (i = 0; i < n_routes; i++) {
        if (strcmp(routes[i].path, path) == 0)
            return &routes[i];
    }
    return NULL;
}

/* Case-insensitive byte compare of `n` bytes. */
static int dyn_ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

/* Find request header `name` (case-insensitive) in the header block
 * [buf, buf+len). Every header line is preceded by CRLF (the request line is
 * not, so it never false-matches). Returns the value with leading spaces
 * trimmed and sets *vlen, or NULL if absent. */
static const char *dyn_req_header(const char *buf, size_t len,
                                  const char *name, size_t *vlen)
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

/* True if the comma-separated header value [v,v+vlen) contains the WHOLE
 * token `tok` (case-insensitive, OWS-trimmed elements). A substring test
 * matched "xclose" as "close", so a front end reading tokens and this server
 * reading substrings disagreed about connection lifetime. */
static int dyn_hdr_token(const char *v, size_t vlen, const char *tok)
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

/* Reject malformed header lines per RFC 9112 5.1/5.2. A field line must be
 * "name: value" with the colon IMMEDIATELY after the name: OWS before the
 * colon (Transfer-Encoding : chunked) makes the line an UNKNOWN header to
 * every scan below, so this server frames by next-request while a front end
 * that normalizes the OWS frames by TE (CWE-444). A line starting with SP or
 * HTAB is an obs-fold continuation, also rejected. The block's first line is
 * the request line and is not CRLF-preceded, so it is never examined; the
 * empty line before the terminator ends the scan. Returns 1 if every line in
 * [buf, buf+len) is a well-formed field line. */
static int dyn_req_headers_valid(const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i + 2 < len; i++) {
        const char *line, *c, *end;
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;
        line = buf + i + 2;
        if (line[0] == '\r')
            return 1;              /* empty line = end of the header block */
        end = buf + len;
        for (c = line; c < end && *c != '\r' && *c != '\n' && *c != ':'; c++) {
            if (*c == ' ' || *c == '\t' ||
                (unsigned char)*c < 0x20 || (unsigned char)*c == 0x7f)
                return 0;          /* SP/CTL inside the field name */
        }
        if (c == line || c >= end || *c != ':')
            return 0;              /* empty name, no colon, or a bare CR */
        if (c[-1] == ' ' || c[-1] == '\t')
            return 0;              /* OWS before the colon: RFC 9112 5.1 */
        /* RFC 9110 5.5: a field value is HT / SP / visible / obs-text. A CTL
         * (a NUL was reaching the parsed value) is not, on either side of the
         * wire: parsers that keep it and parsers that drop it read different
         * messages. */
        {
            const char *v;
            for (v = c + 1; v < end && *v != '\r' && *v != '\n'; v++)
                if (((unsigned char)*v < 0x20 && *v != '\t') ||
                    (unsigned char)*v == 0x7f)
                    return 0;
        }
    }
    return 1;
}

/* ==================================================================== *
 *  one-pass request-head scan                                          *
 * ==================================================================== *
 * The App request pump used to walk the same header block eight-plus
 * times: validate, frame, duplicate-detection (itself two nested walks),
 * accept-encoding, connection, http11 (twice), the request target, and
 * the rpc content-type probe. On a keep-alive ping-pong path those scans
 * are a large fraction of the per-request CPU, and every one of them is
 * over the SAME bytes with the SAME line structure. This scan walks the
 * block ONCE and produces everything any caller needs.
 *
 * Semantics are those of the helpers it replaces -- each rule below
 * cites the helper whose decision it reproduces:
 *   - dyn_req_headers_valid: field-line syntax (name without SP/CTL,
 *     colon immediately after the name, value without CTLs), obs-fold
 *     rejection, first-line-skipped, empty line ends the block;
 *   - dyn_req_frame: first Content-Length value + total count, any
 *     Transfer-Encoding, Host count;
 *   - the duplicate-header refusal with the comma-joinable allowlist
 *     keeping first-wins;
 *   - dyn_req_header: FIRST occurrence wins for accept-encoding,
 *     connection and content-type, OWS-trimmed values;
 *   - dyn_req_is_http11: the version read from the LAST 8 bytes of the
 *     request line, never from a header value;
 *   - dyn_parse_req_path: the request target ends at SP/CR/'?' and must
 *     be printable ASCII (0x21..0x7e, no DEL), else bad_target.
 *
 * `bad` names the FIRST violated rule so the caller answers with the
 * same status text the individual helpers produced. Returns:
 *   0  head parsed, struct filled (fields NULL/0 when absent)
 *   -1 head is WELL-FORMED so far but INCOMPLETE (no terminator yet):
 *      only the line count is filled, for the header-count cap.
 *   >0 head is malformed: the dyn_head_err code (caller maps to status). */
typedef enum {
    DYN_HEAD_OK = 0,
    DYN_HEAD_ERR_LINE = 1,     /* malformed field line (400, was headers_valid) */
    DYN_HEAD_ERR_DUP = 2,      /* duplicate non-joinable header (400) */
} dyn_head_err;

typedef struct {
    /* framing (dyn_req_frame) */
    const char *cl; size_t cl_len; int cl_count; int has_te; int host_count;
    int cl_set;                       /* first CL captured (vs absent) */
    int dup;                          /* a duplicated non-joinable name seen */
    /* negotiation / lifetime / typing (dyn_req_header, first-wins) */
    const char *ae, *conn, *ct; size_t ae_len, conn_len, ct_len;
    /* request line */
    int http11;                       /* dyn_req_is_http11 */
    const char *method; size_t method_len;
    const char *target; size_t target_len;   /* BEFORE the query cut, verbatim */
    int bad_target;                   /* dyn_parse_req_path would refuse */
    size_t head_len;                  /* bytes including the CRLFCRLF */
    size_t n_lines;                   /* header lines seen (for the cap) */
} dyn_reqinfo_t;

/* The comma-joinable / special fields exempt from the duplicate refusal.
 * Byte-identical list to the pump's dup_first_wins table. */
static int dyn_head_dup_ok(const char *n, size_t len)
{
    static const struct { const char *s; size_t l; } ok[] = {
        { "cookie", 6 }, { "set-cookie", 10 },
        { "connection", 10 }, { "accept", 6 }, { "accept-encoding", 15 },
        { "accept-language", 15 }, { "cache-control", 13 }, { "vary", 4 },
    };
    size_t i;
    for (i = 0; i < countof(ok); i++)
        if (ok[i].l == len && dyn_ci_eq(n, ok[i].s, len))
            return 1;
    return 0;
}

/* Case-insensitive compare against a LOWERCASE literal of known length. */
static int dyn_ci_lit(const char *p, size_t len, const char *lit, size_t ll)
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

static int dyn_scan_head(const char *buf, size_t avail, dyn_reqinfo_t *ri)
{
    const char *base = buf, *end = buf + avail;
    const char *line, *le = NULL;      /* current line, its CRLF */
    const char *reqline_end = NULL;
    /* seen-name table for duplicate detection. Header counts are capped by
       DYN_APP_MAX_HEADER_COUNT (256) BEFORE dispatch, and the dup refusal
       must survive a head that fills that cap -- a ring smaller than the
       cap would silently stop refusing duplicates of names beyond it. */
    const char *seen[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t seen_len[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t nseen = 0;

    memset(ri, 0, sizeof(*ri));
    ri->n_lines = 0;

    line = base;
    for (;;) {
        /* find this line's CRLF */
        const char *p = line;
        while (p + 1 < end && !(p[0] == '\r' && p[1] == '\n'))
            p++;
        if (p + 1 >= end) {
            /* No terminator for THIS line yet: the head as a whole is
               incomplete (no CRLFCRLF pair closed it). The partial line is
               NOT counted -- the old dyn_req_header_count only counted
               completed CRLF pairs on this path either. */
            return -1;
        }
        le = p;
        if (!reqline_end) {
            /* request line: method SP target SP version */
            const char *sp1 = (const char *)memchr(line, ' ', (size_t)(le - line));
            const char *sp2 = sp1
                ? (const char *)memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1)) : NULL;
            if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1) {
                /* the callers' downstream parse refuses this; mark and go on */
                ri->method = line; ri->method_len = 0;
                ri->target = le; ri->target_len = 0;
                ri->bad_target = 1;
            } else {
                const char *t = sp1 + 1;
                size_t tlen = (size_t)(sp2 - t);
                ri->method = line; ri->method_len = (size_t)(sp1 - line);
                ri->target = t;
                ri->bad_target = 0;
                while (tlen > 0) {
                    unsigned char ch = (unsigned char)*t;
                    if (ch == '?')
                        break;             /* target ends before the query */
                    if (ch < 0x21 || ch == 0x7f) {
                        ri->bad_target = 1; /* RFC 9112 3: printable only */
                        break;
                    }
                    ri->target_len++;
                    t++; tlen--;
                }
            }
            /* version from the LAST 8 bytes of the request line */
            ri->http11 = (le - line) >= 8 &&
                         memcmp(le - 8, "HTTP/1.1", 8) == 0;
            reqline_end = le;
        } else {
            /* a header field line */
            const char *c = line;
            if (c == le) {
                /* empty line: end of the block. The head INCLUDES this
                   line's CRLF -- the same bytes the old (hdr_end - base) + 4
                   covered: every prior line's CRLF plus the final one. */
                ri->head_len = (size_t)(le + 2 - base);
                break;
            }
            ri->n_lines++;
            /* name: up to ':', no SP/HTAB/CTL inside (headers_valid) */
            while (c < le && *c != ':' && *c != ' ' && *c != '\t' &&
                   (unsigned char)*c >= 0x20 && (unsigned char)*c != 0x7f)
                c++;
            if (c == line)
                return DYN_HEAD_ERR_LINE;      /* empty name */
            if (c >= le || *c != ':')
                return DYN_HEAD_ERR_LINE;      /* no colon / SP-terminated name */
            if (c[-1] == ' ' || c[-1] == '\t')
                return DYN_HEAD_ERR_LINE;      /* OWS before the colon */
            {
                const char *name = line;
                size_t nlen = (size_t)(c - name);
                const char *v = c + 1, *ve = le;
                /* OWS trim both ends */
                while (v < ve && (*v == ' ' || *v == '\t'))
                    v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                    ve--;
                /* value CTL check (HTAB allowed), exactly headers_valid's */
                {
                    const char *q;
                    for (q = v; q < ve; q++)
                        if (((unsigned char)*q < 0x20 && *q != '\t') ||
                            (unsigned char)*q == 0x7f)
                            return DYN_HEAD_ERR_LINE;
                }
                /* duplicate detection: recorded, not refused here, so the
                   caller's verdict ORDER (TE, CL-count, Host, dup -- the
                   original pump's order) decides which status answers. */
                if (!dyn_head_dup_ok(name, nlen)) {
                    size_t i;
                    int dup = 0;
                    for (i = 0; i < nseen; i++)
                        if (seen_len[i] == nlen &&
                            dyn_ci_eq(name, seen[i], nlen)) {
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
                /* interesting-name extraction, first occurrence wins */
                if (dyn_ci_lit(name, nlen, "content-length", 14)) {
                    if (!ri->cl_set) {
                        ri->cl = v; ri->cl_len = (size_t)(ve - v);
                        ri->cl_set = 1;
                    }
                    ri->cl_count++;
                } else if (dyn_ci_lit(name, nlen, "transfer-encoding", 17))
                    ri->has_te = 1;
                else if (dyn_ci_lit(name, nlen, "host", 4))
                    ri->host_count++;
                if (!ri->ae && dyn_ci_lit(name, nlen, "accept-encoding", 15)) {
                    ri->ae = v; ri->ae_len = (size_t)(ve - v);
                }
                if (!ri->conn && dyn_ci_lit(name, nlen, "connection", 10)) {
                    ri->conn = v; ri->conn_len = (size_t)(ve - v);
                }
                if (!ri->ct && dyn_ci_lit(name, nlen, "content-type", 12)) {
                    ri->ct = v; ri->ct_len = (size_t)(ve - v);
                }
            }
        }
        line = le + 2;
        if (line >= end) {
            /* ran out of bytes without the empty line: incomplete head */
            return -1;
        }
    }
    return 0;   /* head_len was set where the empty line ended the block */
}

/* --- lifecycle (JS thread only) --- */

/* Idempotent: stop the reactor graft (BEFORE any teardown) if running. */
static void dyn_http_server_stop_internal(dyn_http_server_t *s)
{
    if (!s->started)
        return;
    if (s->async) {
        dyn_http_async_stop_internal(s->async); /* joins the reactor */
        free(s->async);   /* borrowed mode: routes/listener stay ours */
        s->async = NULL;
    }
    s->started = 0;
}

static void dyn_http_server_dispose(void *native)
{
    dyn_http_server_t *s = (dyn_http_server_t *)native;
    size_t i;

    dyn_http_server_stop_internal(s); /* join BEFORE freeing anything */
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

/* Deep-copy one route value (string => 200/text/plain; object => {status,
 * contentType, body}) into `r`. Runs on the JS thread. Returns 0 or -1
 * (exception pending). The path and content type are emitted into response
 * bytes verbatim, so a CTL byte in either is response splitting (CWE-113);
 * the status lands in the status line, so it must be a valid code. */
static int dyn_route_copy(JSContext *ctx, const char *path, JSValueConst val,
                          dyn_route_t *r)
{
    const char *body = NULL, *ct = NULL;
    size_t body_len = 0;
    int32_t status = 200;
    int body_owned = 0;             /* body is malloc'd, not a JS string */

    r->path = NULL;
    r->content_type = NULL;
    r->body = NULL;
    r->body_len = 0;
    r->status = 200;

    {
        const char *p;
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
        ct = NULL; /* default text/plain */
    } else if (JS_IsFunction(ctx, val)) {
        /* A function here is an App-shaped handler aimed at the wrong
           server: accepting it (a function IS an object) silently compiled
           to an empty-body 200 forever. */
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
        if (status < 100 || status > 999) {
            if (ct)
                JS_FreeCString(ctx, ct);
            if (body)
                JS_FreeCString(ctx, body);
            JS_FreeValue(ctx, vc);
            JS_FreeValue(ctx, vb);
            JS_ThrowRangeError(ctx, "route status must be in [100, 999]");
            return -1;
        }
        if (!JS_IsUndefined(vc) && !JS_IsNull(vc)) {
            ct = JS_ToCString(ctx, vc);
            if (!ct) {
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
            {
                const char *p;
                for (p = ct; *p; p++)
                    if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) {
                        JS_FreeCString(ctx, ct);
                        JS_FreeValue(ctx, vc);
                        JS_FreeValue(ctx, vb);
                        JS_ThrowTypeError(ctx,
                            "route contentType must not contain control "
                            "characters");
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
                /* M10-05: a byte view is the route body VERBATIM. Through
                 * JS_ToCStringLen the body either became digit soup
                 * (ToString of a view) or, for a string workaround, had every
                 * non-ASCII byte mangled by the UTF-8 conversion -- a static
                 * route could not serve binary content at all. The view's
                 * bytes are copied out exactly, NULs included. */
                size_t off = 0, total = 0, bpe = 0;
                uint8_t *base;
                uint8_t *nb;
                JSValue ab = JS_GetArrayBufferView(ctx, vb, &off, &body_len,
                                                   &bpe);
                if (JS_IsException(ab)) {
                    /* not a view: a bare ArrayBuffer is accepted too */
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
                nb = (uint8_t *)malloc(body_len > 0 ? body_len : 1);
                if (!nb) {
                    JS_FreeValue(ctx, vb);
                    if (ct)
                        JS_FreeCString(ctx, ct);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                memcpy(nb, base + off, body_len);
                body = (const char *)nb;
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
    r->body = (char *)malloc(body_len + 1);
    if (!r->path || !r->content_type || !r->body) {
        if (body) {
            if (body_owned)
                free((void *)body);
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
            free((void *)body);
        else
            JS_FreeCString(ctx, body);
    }
    if (ct)
        JS_FreeCString(ctx, ct);
    return 0;
}

/* Bind an IPv4 listening socket (SO_REUSEADDR). host==NULL => all interfaces.
 * Resolves an ephemeral port when `*pport`==0. Returns the fd or -1. */
static int dyn_http_bind(const char *host, uint16_t *pport, int backlog)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, on = 1;

    /* A v6 literal never reaches getaddrinfo below: this binder is AF_INET
       by contract, so v6 is handled directly -- wildcard v6 with V6ONLY off
       serves v4-mapped peers too. */
    if (host && strchr(host, ':')) {
        struct sockaddr_in6 sa6;
        char hbuf[64];
#ifdef IPV6_V6ONLY
        int v6only = 0;
#endif
        {
            const char *h = host;
            size_t hl = strlen(host);
            if (hl >= sizeof(hbuf))
                hl = sizeof(hbuf) - 1;
            if (h[0] == '[') { h++; if (hl > 0) hl--;
                               if (hl > 0 && h[hl - 1] == ']') hl--; }
            memcpy(hbuf, h, hl);
            hbuf[hl] = '\0';
        }
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef IPV6_V6ONLY
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(*pport);
        if (inet_pton(AF_INET6, hbuf, &sa6.sin6_addr) != 1 ||
            bind(fd, (struct sockaddr *)&sa6, sizeof(sa6)) < 0 ||
            listen(fd, backlog > 0 ? backlog : SOMAXCONN) < 0) {
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
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

 resolved:
    /* Ephemeral-port read: sin_family/sin_port and sin6_family/sin6_port
       share their offsets, so the sockaddr_in read is correct for both. */
    if (*pport == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0)
            *pport = ntohs(sin.sin_port);
    }
    return fd;
}

static JSValue dyn_http_server_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    dyn_http_server_t *s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char *host_c = NULL;
    char *host_dup = NULL;
    /* Default comes from the process-wide --io-threads knob so there is one
     * place to size threading; `workers` stays a per-server override. */
    int32_t port = 0, workers = (int32_t)dyn_pool_default_threads(), backlog = 0;
    int32_t req_timeout_ms = DYN_HTTP_REQ_TIMEOUT_MS_DEFAULT;
    JSPropertyEnum *tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    /* --- coerce every option to a C local FIRST (may run user JS) --- */
    if (JS_IsObject(opts)) {
        JSValue v;
        /*reject unknown keys before any option is read or bound. */
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
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &workers, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &backlog, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "requestTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &req_timeout_ms, v)) {
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

        routes_val = JS_GetPropertyStr(ctx, opts, "routes"); /* owned */
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
        req_timeout_ms = 0; /* 0 = the explicit opt-out */

    s = (dyn_http_server_t *)calloc(1, sizeof(*s));
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

    /* --- DEEP-COPY the route table into C memory (JS thread, pre-start) --- */
    if (JS_IsObject(routes_val)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &n_routes, routes_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t *)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char *path = JS_AtomToCString(ctx, tab[i].atom);
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

    /* --- bind now so port 0 resolves at construction --- */
    bound_port = (uint16_t)port;
    listen_fd = dyn_http_bind(host_dup, &bound_port, backlog);
    free(host_dup);
    host_dup = NULL;
    if (listen_fd < 0) {
        /* dispose frees routes/workers/mutex; hand it a consistent object */
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

static JSValue dyn_http_server_start(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    {
        dyn_http_async_t *a = (dyn_http_async_t *)calloc(1, sizeof(*a));
        if (!a)
            return JS_ThrowOutOfMemory(ctx);
        /* The graft borrows THIS object's ctor-bound listener and route
           copy; it serves them on its own reactor thread (io_uring on
           Linux, readiness elsewhere) with the static-route pump. */
        a->ctx = s->ctx;
        a->listen_fd = s->listen_fd;
        a->routes = s->routes;
        a->n_routes = s->n_routes;
        a->borrowed = 1;
        /* The old per-connection frame buffer was 16 KiB and every 431/413
           test pins that bound; the async class default is 1 MiB, so the
           graft keeps its own. */
        a->max_req = DYN_HTTP_SERVER_FRAME_MAX;
        /* Idle budget: the old server gave an idle keep-alive connection
           5 s (capped by the caller's requestTimeoutMs) and a request with
           bytes in flight the full budget. The reactor has ONE clock,
           stamped on completed requests, so the graft uses the tighter of
           the two -- a dribbled partial request is cut no later than the
           old absolute deadline, and sooner than the old 30 s default when
           the caller did not lower it. */
        a->idle_ms = s->req_timeout_ms
            ? (uint64_t)(s->req_timeout_ms < DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT
                         ? s->req_timeout_ms : DYN_HTTP_IDLE_TIMEOUT_MS_DEFAULT)
            : 0;
        /* The old queue+workers design admitted DYN_HTTP_CONN_QUEUE_CAP +
           workers connections and 503-stormed the rest; the reactor holds
           no per-connection cost, so the graft takes the async class's
           default ceiling rather than inheriting the artifact. */
        a->max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
        atomic_init(&a->stop_flag, 0);
        atomic_init(&a->spawn_ok, 0);
        atomic_init(&a->n_refused, 0);
        /* the readiness accept drain loops on EAGAIN; a blocking listener
           would wedge the reactor on a spurious wakeup */
        dyn_set_nonblock(s->listen_fd);
        if (dyn_http_async_spawn(a) < 0) {
            free(a);
            return JS_ThrowInternalError(
                ctx, "HTTPServer.start: failed to start the reactor thread");
        }
        /* The reactor thread is invisible to js_os_poll, so a started
           server would let the process exit at the end of the script. Hold
           the shared reactor, exactly as App does; stop() releases it. */
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

static JSValue dyn_http_server_stop(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
        ctx, this_val, dyn_http_server_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_server_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_server_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_http_server_t *s = (dyn_http_server_t *)dyn_res_native(
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

/* ==================================================================== *
 *  HTTPServerAsync -- Model A: single-threaded kqueue/epoll reactor      *
 *                                                                        *
 *  One background reactor thread multiplexes ALL connections with non-   *
 *  blocking sockets (no per-connection thread, no fd queue). Native      *
 *  static-route handlers only -- the reactor thread never touches the    *
 *  JSContext, so it is safe by construction exactly like the thread-pool *
 *  server. Removes the worker cap and the queue-full connection drops:   *
 *  connection count is bounded only by the fd limit. This is the C-speed *
 *  ceiling the JS-handler path (Model B) is measured against.            *
 * ==================================================================== */

static JSClassID dyn_http_async_class_id;

#define DYN_ACONN_MAX_REQ   (1 * 1024 * 1024) /* cap a single buffered request */
#define DYN_ACONN_MAX_REQS  100000            /* keep-alive requests/connection */
/* WebSocket work budgets. DYN_ACONN_MAX_REQ already bounds reassembly MEMORY;
   these bound the CPU a peer can demand without making progress -- reaching the
   size cap one byte per frame, or obliging a pong per ping forever. Both refill
   on a delivered message, so a client doing real work never reaches them. */
#define DYN_WS_MAX_FRAGMENTS 4096             /* frames per reassembled message */
#define DYN_WS_CTL_BUDGET    64               /* control frames between messages */
/* Outbound bound for streaming senders (ws/sse push, client frames): when
   this much is already queued for the peer, it is not reading -- send more
   and dyn_aio's queue grows without limit. The connection is closed rather
   than the frame silently dropped. */
#define DYN_HTTP_OUTBOUND_MAX (4 << 20)
#define DYN_APP_MAX_BATCH    256              /* JSON-RPC calls in one batch */
#define DYN_APP_MAX_PARAMS   256              /* JSON-RPC by-position args */



/* Append a full HTTP/1.1 response (head + body) into `out`. Returns 0 or -1. */
static inline size_t dyn_put_u64(char *p, uint64_t v)
{
    char tmp[20];
    int n = 0, i;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    for (i = 0; i < n; i++)
        p[i] = tmp[n - 1 - i];
    return (size_t)n;
}

static int dyn_http_format(dyn_bytes_t *out, int status, const char *ct,
                           const char *body, size_t body_len, int keep_alive)
{
    char head[512];
    int n;
    /* The 200 text/plain keep-alive head is constant apart from the length,
       and this runs once per response -- the format scan showed up as
       __vfprintf at the same rates the App pump measured before it grew the
       same hand-rolled head (dyna-http.c:6226). Bytes are identical to the
       snprintf below. */
    if (status == 200 && keep_alive && ct &&
        strcmp(ct, "text/plain") == 0) {
        static const char pre[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
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
    if (body_len > 0 && dyn_bytes_append(out, body, body_len) < 0)
        return -1;
    return 0;
}

static void dyn_aconn_free(dyn_aconn_t *c)
{
    free(c->in.data);
    free(c->out.data);
    free(c);
}

/* Drop a connection: unregister, close, free. */
/* Intrusive live list, reactor thread only. Unlink is idempotent. */
static void dyn_aconn_link(dyn_http_async_t *s, dyn_aconn_t *c)
{
    c->lnext = s->live;
    if (s->live)
        s->live->lprev = c;
    s->live = c;
    s->nconns++;
}

static void dyn_aconn_unlink(dyn_http_async_t *s, dyn_aconn_t *c)
{
    if (c->lprev)
        c->lprev->lnext = c->lnext;
    else if (s->live == c)
        s->live = c->lnext;
    else
        return;                          /* never linked */
    if (c->lnext)
        c->lnext->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
    if (s->nconns > 0)
        s->nconns--;
}

static void dyn_aconn_close(dyn_evloop_t *lp, dyn_aconn_t *c)
{
    dyn_aconn_unlink(c->srv, c);
    dyn_evloop_del(lp, c->fd);
    close(c->fd);
    dyn_aconn_free(c);
}

/* Close every connection idle past idle_ms. Driven by the reactor's own 200 ms
   poll tick, never by traffic: a peer that connects and then sends nothing
   generates no event, and it is exactly the one that must be swept. */
static void dyn_aconn_sweep(dyn_evloop_t *lp, dyn_http_async_t *s, uint64_t now)
{
    dyn_aconn_t *c = s->live, *next;
    while (c) {
        next = c->lnext;                 /* close frees c */
        if (now - c->last_ms >= s->idle_ms)
            dyn_aconn_close(lp, c);
        c = next;
    }
}

/* Parse as many complete pipelined requests as `c->in` holds, appending a
 * response for each to `c->out`. Returns 1 if the connection should close after
 * the queued output drains, 0 to keep reading. */
/* Core request pump shared by the readiness (Model A) and io_uring completion
 * reactors: parse every complete pipelined request buffered in `in`, appending
 * a response for each to `out`, and consume it from `in`. *pnreq counts
 * requests served on this connection (keep-alive cap). Returns 1 if the
 * connection should close once `out` drains, else 0. */
static int dyn_http_pump(dyn_bytes_t *in, dyn_bytes_t *out, int *pnreq,
                         size_t *pscan,
                         const dyn_route_t *routes, size_t n_routes,
                         size_t max_req)
{
    char path[2048];

    for (;;) {
        const char *base = in->data;
        size_t avail = in->len;
        /* Resume where the last search stopped, backing up 3 bytes so a
           terminator straddling the chunk boundary is still found. Scanning
           from 0 on every recv was O(n^2) over dribbled headers (the App
           path's hdr_scan_from at :4161 exists for exactly this). */
        size_t from = *pscan < avail ? *pscan : avail;
        const char *hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        size_t head_len, body_len = 0, req_total, clv_len = 0, connv_len = 0;
        const char *cl, *conn;
        int http11, keep_alive;
        const dyn_route_t *route;
        dyn_reqinfo_t ri;

        if (!hdr_end) {
            *pscan = avail >= 3 ? avail - 3 : 0;
            if (avail > max_req) {
                /* Answer before dropping: a peer that never sends CRLFCRLF
                   gets a status, not silence. The recv loop holds max_req +
                   slack so this branch -- not a silent close -- is what fires
                   at the cap; memory stays bounded either way. */
                dyn_http_format(out, 431, "text/plain",
                                "Request Header Fields Too Large", 31, 0);
                return 1;
            }
            return 0;     /* need more bytes */
        }
        head_len = (size_t)(hdr_end - base) + 4;

        /* One pass for validity + framing + connection, the same facts the
           App pump gets from dyn_scan_head. This server has NO duplicate-
           header refusal (ri.dup is ignored) and no Accept-Encoding use --
           exactly the helpers it used to call, folded into one walk. */
        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src != 0 || ri.head_len == 0 ||
                ri.head_len > (size_t)(hdr_end - base) + 4) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
                return 1;              /* malformed header line: 400 and close */
            }
        }
        head_len = ri.head_len;
        http11 = ri.http11;

        /* A COMPLETE header block past the cap is 431 (RFC 9110 12.5.5):
         * the framing is known, what is oversized is the header itself. The
         * 413 below is reserved for a head that fits with a DECLARED body
         * that does not. The thread server answered 431 here through its
         * recv-buffer bound; the pump used to fold both into 413, which
         * reported the wrong member as oversized. */
        if (head_len > max_req) {
            dyn_http_format(out, 431, "text/plain",
                            "Request Header Fields Too Large", 31, 0);
            return 1;
        }

        if (ri.has_te) {
            dyn_http_format(out, 501, "text/plain", "Not Implemented", 15, 0);
            return 1;                    /* unsupported framing: 501 and close */
        }
        {
            if (ri.cl_count > 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
                return 1;
            }
            /* RFC 9110 7.2: an HTTP/1.1 request with no Host, or more than
             * one, MUST be a 400 (the App and thread-pool pumps enforce the
             * same rule). */
            if (http11 && ri.host_count != 1) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
                return 1;
            }
        }
        cl = ri.cl; clv_len = ri.cl_len;
        if (cl) {
            /* RFC 9110 8.6: the value is a sequence of DIGITS. Stopping at the
               first non-digit read "-1" as 0 and "5abc" as 5, which a front end
               that parses it differently turns into a smuggled request. */
            size_t j;
            if (clv_len == 0 || clv_len > 19) {
                dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
                return 1;
            }
            for (j = 0; j < clv_len; j++) {
                if (cl[j] < '0' || cl[j] > '9') {
                    dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
                    return 1;
                }
                body_len = body_len * 10 + (size_t)(cl[j] - '0');
            }
        }
        req_total = head_len + body_len;
        if (req_total > max_req) {
            dyn_http_format(out, 413, "text/plain", "Content Too Large", 17, 0);
            return 1;
        }
        if (avail < req_total)
            return 0; /* body not fully arrived yet */

        conn = ri.conn; connv_len = ri.conn_len;
        keep_alive = http11;
        if (dyn_hdr_token(conn, connv_len, "close"))
            keep_alive = 0;
        else if (dyn_hdr_token(conn, connv_len, "keep-alive"))
            keep_alive = 1;
        if (++(*pnreq) >= DYN_ACONN_MAX_REQS)
            keep_alive = 0;

        if (ri.bad_target) {
            dyn_http_format(out, 400, "text/plain", "Bad Request", 11, 0);
            return 1;
        }
        {
            size_t plen = ri.target_len < sizeof(path) - 1
                        ? ri.target_len : sizeof(path) - 1;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
        }
        route = dyn_route_lookup(routes, n_routes, path);
        if (route)
            dyn_http_format(out, route->status, route->content_type,
                            route->body, route->body_len, keep_alive);
        else
            dyn_http_format(out, 404, "text/plain", "Not Found", 9, keep_alive);

        /* drop the consumed request; keep any pipelined trailing bytes */
        memmove(in->data, in->data + req_total, in->len - req_total);
        in->len -= req_total;
        *pscan = 0;   /* the next request starts its own scan */
        if (!keep_alive)
            return 1;
    }
}

static int dyn_aconn_process(dyn_aconn_t *c)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
                                    &c->hdr_scan_from,
                                    c->srv->routes, c->srv->n_routes,
                                    c->srv->max_req);
    /* The pump consumes only WHOLE requests, so a shrinking `in` is the one
       unambiguous signal that the peer made protocol progress. */
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

/* Attempt to flush `c->out`; on full drain either close or resume reading.
 * Returns 0 if the connection lives, 1 if it was closed/freed. */
static int dyn_aconn_flush(dyn_evloop_t *lp, dyn_aconn_t *c)
{
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
            dyn_evloop_mod(lp, c->fd, DYN_EV_WRITE); /* wait to drain */
            return 0;
        }
        dyn_aconn_close(lp, c); /* peer gone / hard error */
        return 1;
    }
    /* fully sent */
    c->out.len = 0;
    c->out_off = 0;
    if (c->closing) {
        dyn_aconn_close(lp, c);
        return 1;
    }
    dyn_evloop_mod(lp, c->fd, DYN_EV_READ);
    return 0;
}

static void dyn_aconn_cb(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_aconn_t *c = (dyn_aconn_t *)udata;
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
                /* The pump answers 431/413 at max_req; this slack is the
                   hard memory bound that only fires if the pump never ran. */
                if (c->in.len > c->srv->max_req + 16384)
                    { dyn_aconn_close(lp, c); return; }
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break; /* drained the socket buffer */
            /* r == 0 (peer half/closed its write side) or a hard error: no more
             * input will arrive. Fall through to process whatever is already
             * buffered (a client may send a full request then shutdown(SHUT_WR)
             * while still waiting to read the reply), flush it, then close. */
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

static void dyn_alisten_cb(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    dyn_http_async_t *s = (dyn_http_async_t *)udata;
    (void)events;
    for (;;) {
        dyn_aconn_t *c;
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* no more pending connections */
            if (errno == ECONNABORTED)
                continue;
            /* Resource exhaustion (EMFILE/ENFILE/ENOBUFS/ENOMEM): the
               listening fd stays READABLE, so returning now busy-loops the
               level-triggered reactor at 100% CPU. Pause on the fd itself,
               then return -- the next event retries accept. */
            {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                poll(&pfd, 1, 50);
            }
            break;
        }
        /* Refuse BEFORE allocating: the cap exists so a peer cannot make the
           process spend an fd and a buffer pair per connection. Accept-then-
           close is the refusal; leaving it queued spins a level-triggered fd. */
        if (s->max_conns && s->nconns >= s->max_conns) {
            atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
            close(cfd);
            continue;
        }
        if (dyn_set_nonblock(cfd) < 0) {
            close(cfd);
            continue;
        }
        dyn_set_nodelay(cfd);
        c = (dyn_aconn_t *)calloc(1, sizeof(*c));
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
/* ==================================================================== *
 *  io_uring completion-model reactor (Linux, CONFIG_IO_URING)           *
 *                                                                        *
 *  NOT the poll-mode readiness shim in dyna-evloop.c. This is the real *
 *  io_uring recipe: multishot accept + multishot recv fed from a SHARED  *
 *  provided-buffer ring (idle connections hold zero buffer memory) +     *
 *  batched send, driven by SINGLE_ISSUER|DEFER_TASKRUN|COOP_TASKRUN so   *
 *  the whole submit/complete cycle is one syscall. Data is delivered in  *
 *  completions -- there is no per-fd readiness poll and no recv/accept   *
 *  syscall per event.                                                    *
 * ==================================================================== */

#define DYN_URING_ENTRIES 8192
#define DYN_URING_NBUFS   4096
#define DYN_URING_BUFSZ   2048
#define DYN_URING_BGID    1
#define DYN_URING_ACCEPT_UD 0ULL

enum { DYN_OP_ACCEPT = 0, DYN_OP_RECV = 1, DYN_OP_SEND = 2, DYN_OP_CLOSE = 3 };
#define DYN_UD_TAG(ud)  ((int)((ud) & 7))
#define DYN_UD_CONN(ud) ((dyn_uconn_t *)(uintptr_t)((ud) & ~(uint64_t)7))

/* One connection in the completion model. `refs` are the outstanding io_uring
 * ops that carry this pointer as user_data (a live multishot recv + an in-flight
 * send); the object is freed only once both are gone and it is closing. */
typedef struct dyn_uconn {
    int fd;
    dyn_bytes_t in;   /* accumulated request bytes (copied out of pool buffers) */
    dyn_bytes_t out;  /* response bytes pending send (immutable while sending) */
    size_t out_off;
    size_t hdr_scan_from; /* where the CRLFCRLF search resumes (see the pump) */
    int nreq;
    unsigned recv_armed : 1;
    unsigned send_inflight : 1;
    unsigned closing : 1;
    /* Stamped when a request is CONSUMED, never on byte arrival -- the same
     * slowloris rule the readiness model's dyn_aconn_t carries. */
    uint64_t last_ms;
    struct dyn_uconn *next, *prev; /* server's live-connection list */
} dyn_uconn_t;

typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    unsigned char *buf_base; /* nbufs * bufsz slab backing the provided ring */
    int nbufs, bufsz;
    int accept_errs;         /* consecutive accept failures (backoff) */
    int accept_needs_arm;    /* re-arm failed: retry each loop tick */
    int nconns;              /* live count, mirrors s->max_conns' readiness role */
    dyn_uconn_t *live;
} dyn_uring_ctx;

/* Get an SQE, flushing the backlog and retrying. If the ring is STILL full,
   wait for one completion: harvesting a CQE is what frees its SQE -- without
   this, exhaustion left recv/send silently un-armed and the connection
   stalled forever with no event to recover it. The brief block lands on the
   reactor thread only under genuine exhaustion. */
static struct io_uring_sqe *dyn_ur_sqe(dyn_uring_ctx *u)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&u->ring);
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

static int dyn_ur_arm_accept(dyn_uring_ctx *u, int listen_fd)
{
    struct io_uring_sqe *sqe = dyn_ur_sqe(u);
    if (!sqe)
        return -1;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, DYN_URING_ACCEPT_UD);
    return 0;
}

static void dyn_ur_arm_recv(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    struct io_uring_sqe *sqe;
    if (c->fd < 0) {
        /* the fd was already dropped by a cancel-close: nothing to arm */
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        /* No SQE even after harvesting: leave recv_armed clear so the next
           completion on this connection re-arms, and mark it for close so a
           fully-starved ring cannot strand it silent forever. */
        c->closing = 1;
        return;
    }
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = DYN_URING_BGID;
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_RECV);
    c->recv_armed = 1;
}

static void dyn_ur_submit_send(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    struct io_uring_sqe *sqe;
    if (c->fd < 0) {
        /* the fd was already dropped by a cancel-close: nothing to send on */
        c->closing = 1;
        return;
    }
    sqe = dyn_ur_sqe(u);
    if (!sqe) {
        /* The response stays in c->out; the close step below ends the
           connection rather than stalling it with output pending. */
        c->closing = 1;
        return;
    }
    io_uring_prep_send(sqe, c->fd, c->out.data + c->out_off,
                       c->out.len - c->out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)c | DYN_OP_SEND);
    c->send_inflight = 1;
}

static void dyn_ur_recycle(dyn_uring_ctx *u, int bid)
{
    io_uring_buf_ring_add(u->br, u->buf_base + (size_t)bid * u->bufsz, u->bufsz,
                          bid, io_uring_buf_ring_mask(u->nbufs), 0);
    io_uring_buf_ring_advance(u->br, 1);
}

static dyn_uconn_t *dyn_uconn_new(dyn_uring_ctx *u, int fd)
{
    dyn_uconn_t *c = (dyn_uconn_t *)calloc(1, sizeof(*c));
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

static void dyn_uconn_destroy(dyn_uring_ctx *u, dyn_uconn_t *c)
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

/* Progress a closing connection: once no send is in flight, retire the fd
 * (which terminates the multishot recv with a final CQE), then free when no
 * op remains that still references this object. */
static void dyn_uconn_close_step(dyn_uring_ctx *u, dyn_uconn_t *c)
{
    if (!c->closing || c->send_inflight)
        return;
    if (c->recv_armed) {
        if (c->fd >= 0) {
            /* close(2) alone does NOT hang up on the peer: the armed multishot
             * recv holds a kernel file reference, so the fd number went away
             * while the socket lived on FIN-less until ring teardown -- the
             * same hang dyn_aio_close fixed on the shared ring. Cancel every
             * op on this fd BEFORE the close (it resolves the fd number at
             * submit time); the provoked CQEs clear recv_armed. */
            struct io_uring_sqe *sqe = dyn_ur_sqe(u);
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

static void dyn_ur_on_accept(dyn_uring_ctx *u, dyn_http_async_t *s,
                             struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE)) { /* multishot accept ended */
        if (dyn_ur_arm_accept(u, s->listen_fd) < 0) {
            /* No SQE even after harvesting: retry on a later loop tick
               rather than go deaf -- any completion frees ring space. */
            u->accept_needs_arm = 1;
            return;
        }
    }
    if (res < 0) {
        /* A persistent failure (EMFILE...) re-arms into an immediate error:
           a CQE storm spinning the ring. Back off before the next arm. */
        if (++u->accept_errs >= 32) {
            usleep(50000);
            u->accept_errs = 0;
        }
        return;
    }
    u->accept_errs = 0;
    /* Refuse BEFORE allocating, mirroring the readiness accept loop: the cap
       bounds what a peer can make the process spend, and accept-then-close is
       the refusal. The shared-ring servers enforce it; this ring must too or
       maxConns silently disappears on the uring build. */
    if (s->max_conns && u->nconns >= s->max_conns) {
        atomic_fetch_add_explicit(&s->n_refused, 1, memory_order_relaxed);
        close(res);
        return;
    }
    {
        dyn_uconn_t *c = dyn_uconn_new(u, res);
        if (!c) {
            close(res);
            return;
        }
        dyn_set_nodelay(res);
        dyn_ur_arm_recv(u, c);
    }
}

/* The pump with the uring model's progress stamp: `in` shrinking means a
 * WHOLE request was consumed -- the same rule as dyn_aconn_process. */
static int dyn_ur_pump(dyn_uconn_t *c, dyn_http_async_t *s)
{
    size_t before = c->in.len;
    int close_after = dyn_http_pump(&c->in, &c->out, &c->nreq,
                                    &c->hdr_scan_from,
                                    s->routes, s->n_routes, s->max_req);
    if (c->in.len < before)
        c->last_ms = dyn_timer_now_ms();
    return close_after;
}

static void dyn_ur_on_recv(dyn_uring_ctx *u, dyn_http_async_t *s,
                           dyn_uconn_t *c, struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    int bid = (cqe->flags & IORING_CQE_F_BUFFER)
                  ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;

    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->recv_armed = 0; /* multishot recv terminated */

    if (res > 0) {
        int oom = 0;
        if (bid >= 0) {
            if (dyn_bytes_append(&c->in,
                                 (char *)(u->buf_base + (size_t)bid * u->bufsz),
                                 (size_t)res) < 0)
                oom = 1;
            dyn_ur_recycle(u, bid);
        }
        if (oom || c->in.len > DYN_ACONN_MAX_REQ + 16384) {
            c->closing = 1;   /* hard bound; the pump answers 431 at the cap */
        } else if (!c->send_inflight) {
            /* only touch `out` when no send references it (no realloc-under-send) */
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
            if (!c->closing && !c->recv_armed) /* pool momentarily drained */
                dyn_ur_arm_recv(u, c);
        } else { /* EOF (0) or hard error: flush what we have, then close */
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

static void dyn_ur_on_send(dyn_uring_ctx *u, dyn_http_async_t *s,
                           dyn_uconn_t *c, struct io_uring_cqe *cqe)
{
    int res = cqe->res;
    c->send_inflight = 0;
    if (res > 0) {
        c->out_off += (size_t)res;
        if (c->out_off < c->out.len) {
            dyn_ur_submit_send(u, c); /* short send: ship the remainder */
        } else {
            c->out.len = 0;
            c->out_off = 0;
            if (!c->closing) {
                /* response flushed; drain any pipelined requests buffered while
                 * the send held `out` */
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

/* Close every connection idle past idle_ms -- the uring twin of
 * dyn_aconn_sweep. Runs on the reactor's own 200 ms tick (1 Hz gate), never
 * on traffic: the silent peer generates no completion at all, and it is
 * exactly the one that must be swept. A swept connection with a send still
 * in flight settles from that send's completion (close_step refuses while
 * send_inflight), so nothing is freed under a live SQE. */
static void dyn_ur_sweep(dyn_uring_ctx *u, dyn_http_async_t *s, uint64_t now)
{
    dyn_uconn_t *c = u->live, *next;
    while (c) {
        next = c->next;                 /* close_step may free c */
        if (now - c->last_ms >= s->idle_ms) {
            c->closing = 1;
            dyn_uconn_close_step(u, c);
        }
        c = next;
    }
}

/* Returns 1 if the io_uring reactor initialised and ran (and cleaned up); 0 if
 * init failed (caller falls back to the readiness reactor). */
static int dyn_http_uring_try_run(dyn_http_async_t *s)
{
    dyn_uring_ctx *u = (dyn_uring_ctx *)calloc(1, sizeof(*u));
    struct io_uring_params p;
    int ret = 0, i;

    if (!u)
        return 0;
    u->nbufs = DYN_URING_NBUFS;
    u->bufsz = DYN_URING_BUFSZ;

    /* Prefer the modern single-issuer/deferred-taskrun setup; degrade on older
     * kernels that reject the flags. */
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_COOP_TASKRUN;
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

    u->buf_base = (unsigned char *)malloc((size_t)u->nbufs * u->bufsz);
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
        struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 200000000L};
        struct io_uring_cqe *cqe;
        unsigned head, count = 0;

        /* The tick is unconditional, so the sweep runs on a clock rather than
           on traffic -- same shape and 1 Hz rate as the readiness loop below. */
        {
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_ur_sweep(u, s, now);
            }
        }
        if (u->accept_needs_arm &&
            dyn_ur_arm_accept(u, s->listen_fd) == 0)
            u->accept_needs_arm = 0;
        ret = io_uring_submit_and_wait_timeout(&u->ring, &cqe, 1, &ts, NULL);
        if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -ETIMEDOUT)
            break;
        io_uring_for_each_cqe(&u->ring, head, cqe) {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            count++;
            if (ud == DYN_URING_ACCEPT_UD) {
                dyn_ur_on_accept(u, s, cqe);
            } else if (DYN_UD_TAG(ud) == DYN_OP_CLOSE) {
                /* the cancel-before-close marker: nothing to dispatch */
            } else {
                dyn_uconn_t *c = DYN_UD_CONN(ud);
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
#endif /* DYN_HTTP_HAVE_URING */

static void *dyn_http_async_reactor(void *arg)
{
    dyn_http_async_t *s = (dyn_http_async_t *)arg;

#ifdef DYN_HTTP_HAVE_URING
    if (dyn_http_uring_try_run(s))
        return NULL; /* the io_uring reactor ran to shutdown and cleaned up */
    /* io_uring init failed on this kernel: fall back to the readiness reactor */
#endif

    s->loop = dyn_evloop_new();
    if (!s->loop ||
        dyn_evloop_add(s->loop, s->listen_fd, DYN_EV_READ, dyn_alisten_cb, s) < 0) {
        atomic_store_explicit(&s->spawn_ok, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&s->spawn_ok, 1, memory_order_release);

    while (!atomic_load_explicit(&s->stop_flag, memory_order_relaxed)) {
        {   /* The 200 ms tick is unconditional, so the sweep runs on a clock
               rather than on traffic -- the quiet server that needs it most is
               exactly the one that generates no events. 1 Hz: it is O(live). */
            uint64_t now = dyn_timer_now_ms();
            if (s->idle_ms && now - s->last_sweep_ms >= 1000) {
                s->last_sweep_ms = now;
                dyn_aconn_sweep(s->loop, s, now);
            }
        }
        if (dyn_evloop_poll(s->loop, 200) < 0) /* tick to observe stop_flag */
            break;
    }
    return NULL;
}

static void dyn_http_async_stop_internal(dyn_http_async_t *s)
{
    if (!s->started)
        return;
    atomic_store_explicit(&s->stop_flag, 1, memory_order_relaxed);
    pthread_join(s->reactor, NULL);
    if (s->loop) {
        dyn_evloop_free(s->loop); /* reactor is joined: sole owner now */
        s->loop = NULL;
    }
    if (s->reactor_held) {
        dyn_net_reactor_release(s->ctx);
        s->reactor_held = 0;
    }
    s->started = 0;
}

static void dyn_http_async_dispose(void *native)
{
    dyn_http_async_t *s = (dyn_http_async_t *)native;
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

static JSValue dyn_http_async_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    dyn_http_async_t *s;
    JSValue opts, routes_val = JS_UNDEFINED;
    const char *host_c = NULL;
    char *host_dup = NULL;
    int32_t port = 0, backlog = 0, max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
    int64_t idle_ms = DYN_ACONN_IDLE_MS_DEFAULT;
    JSPropertyEnum *tab = NULL;
    uint32_t n_routes = 0, i;
    int listen_fd = -1;
    uint16_t bound_port;

    opts = (argc > 0) ? argv[0] : JS_UNDEFINED;

    if (JS_IsObject(opts)) {
        JSValue v;
        /*reject unknown keys before any option is read or bound. */
        if (dyn_opts_strict(ctx, opts, http_async_keys, 6))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, opts, "port");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &port, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt32(ctx, &backlog, v)) {
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
        /* Both default ON: the insecure behaviour was the incumbent, and a
           defence shipped behind a flag that defaults off changes nothing for
           every existing caller. 0 is the explicit opt-out. */
        v = JS_GetPropertyStr(ctx, opts, "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &idle_ms, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (idle_ms < 0) idle_ms = 0;
        v = JS_GetPropertyStr(ctx, opts, "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &max_conns, v)) {
            JS_FreeValue(ctx, v);
            free(host_dup);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (max_conns < 0) max_conns = 0;
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

    s = (dyn_http_async_t *)calloc(1, sizeof(*s));
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
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto fail_pending;
        if (n_routes > 0) {
            s->routes = (dyn_route_t *)calloc(n_routes, sizeof(dyn_route_t));
            if (!s->routes)
                goto oom_enum;
        }
        for (i = 0; i < n_routes; i++) {
            const char *path = JS_AtomToCString(ctx, tab[i].atom);
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

/* Spawn the reactor thread and wait for its loop-init verdict. 0 ok, -1
 * thread/loop failure (exception NOT set: both callers throw their own). */
static int dyn_http_async_spawn(dyn_http_async_t *s)
{
    atomic_store_explicit(&s->stop_flag, 0, memory_order_relaxed);
    atomic_store_explicit(&s->spawn_ok, 0, memory_order_relaxed);
    if (pthread_create(&s->reactor, NULL, dyn_http_async_reactor, s) != 0)
        return -1;
    /* wait for the reactor to publish loop-init success/failure */
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

static JSValue dyn_http_async_start(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->started)
        return JS_UNDEFINED;
    if (dyn_http_async_spawn(s) < 0)
        return JS_ThrowInternalError(ctx, "failed to start reactor thread");
    /* Same keep-alive as dyn_http_server_start: the reactor thread is
       invisible to js_os_poll, so hold the shared reactor or a script whose
       server is the only work exits the instant it ends. */
    if (!dyn_net_reactor_acquire(ctx)) {
        dyn_http_async_stop_internal(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->reactor_held = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_stop(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
        ctx, this_val, dyn_http_async_class_id);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    dyn_http_async_stop_internal(s);
    return JS_UNDEFINED;
}

static JSValue dyn_http_async_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_http_async_t *s = (dyn_http_async_t *)dyn_res_native(
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

/* ==================================================================== *
 *  App -- the controlled JS-handler server (Model B)                    *
 *                                                                        *
 *  Runs ENTIRELY on the JS thread via the dyn_aio reactor folded into   *
 *  js_std_loop -- so handlers are plain JS, invoked with a zero-copy     *
 *  view of the request, no cross-thread hop. Low-level HTTP is not       *
 *  exposed; a user registers typed routes: .rpc (strict JSON-RPC 2.0),  *
 *  and (next) .static/.upload/.ws. This is the usable-server fix.        *
 * ==================================================================== */

enum { APP_RPC = 1, APP_STATIC, APP_UPLOAD, APP_WS, APP_PROXY, APP_SSE, APP_DYN };

/* Ordered largest-first: the declaration order cost 10 bytes of interior
   padding (80 -> 72). A route is per-registration, not per-request, so this is
   layout hygiene rather than a measured win. */
typedef struct {
    JSValue handler;  /* rpc: methods object; upload/ws: handler(s); dyn: handler fn */
    char *path;       /* exact match (rpc/upload/ws) or prefix (static) */
    char *dir;        /* static/upload: filesystem root */
    char **allow;     /* file-type filter: NULL=allow all; else ".ext"/"mime" list */
    char *up_host;    /* proxy: upstream */
    char *dyn_method; /* dyn: "GET"/"POST"/"PUT"/"PATCH"/"DELETE" */
    char *dyn_pattern;/* dyn: original pattern ("/users/:id") */
    int64_t max_file; /* static/upload: size cap (0 = default) */
    size_t n_allow;
    int type;
    uint16_t up_port;
} dyn_app_route_t;

_Static_assert(sizeof(dyn_app_route_t) <= 96,
               "dyn_app_route_t regained padding: reorder largest-first");

typedef struct dyn_app {
    JSContext *ctx;
    dyn_aio_t *aio;
    int listen_fd;
    uint16_t port;
    dyn_app_route_t *routes;
    size_t n_routes;
    int started;
    int n_conns;              /* live conns holding the app alive */
    int dispose_called;       /* dispose ran: free the app at the last conn */
    /* Idle-timeout sweep. ONE repeating timer for every connection, not one
       timer each: per-connection timers churn a heap entry per request, and the
       sweep is O(live conns) at 1 Hz. 0 disables. */
    void *conns;              /* dyn_app_conn_t* head of the live list */
    uint64_t idle_ms;
    int max_conns;            /* 0 = unbounded; default matches HTTPServerAsync */
    dyn_timers_t *timers;
    dyn_timer_id sweep;
    int compress;             /* gzip responses when the client accepts them */
    int metrics_http;         /* expose GET /metrics and /healthz */
    char *listen_host;        /* NULL => "0.0.0.0"; a v6 literal binds dual-stack */
    /*listen(2) backlog, from the ctor's `backlog` option (0 = the
       1024 default dyn_app_start has always used). `workers` is stored
       beside it so the ctor's configuration is introspectable (app.workers),
       but read the comment at dyn_app_start before assuming it buys threads. */
    int backlog;
    int workers;
    /*dynamic routes (method + pattern) live in routes[] with
     * type APP_DYN; middleware is a separate ordered list of JS functions
     * run before the matched handler. Both hold JSValues, so gc_mark traces
     * them and dispose frees them. */
    JSValue *mw;
    size_t n_mw, cap_mw;
} dyn_app_t;

typedef struct dyn_ws dyn_ws_t;
typedef struct dyn_sse dyn_sse_t;
typedef struct dyn_app_upload dyn_app_upload_t;
typedef struct dyn_app_stream dyn_app_stream_t;

typedef struct {
    dyn_app_t *app;
    int fd;
    dyn_iobuf_t in; /* accumulated request bytes (keep-alive/pipelined) */
    dyn_app_upload_t *up; /* non-NULL while streaming a large upload to disk */
    int refs;       /* 1 for the live connection + 1 per in-flight async handler */
    int closed;     /* peer gone: settle callbacks must not send, just unref */
    int close_code; /* code for the close handler; 0 = default 1000 */
    /* WebSocket mode (after a successful Upgrade on a ws route) */
    int is_ws;
    JSValue ws_handlers; /* {open,message,close} (dup) */
    JSValue ws_this;     /* the WsConn JS object (conn holds a strong ref) */
    dyn_ws_t *ws_native; /* WsConn opaque; ws_native->conn nulled on teardown */
    dyn_iobuf_t ws_frag; /* reassembly buffer for a fragmented message */
    int ws_frag_op;      /* opcode of the in-progress fragmented message, or 0 */
    /* Two budgets, both bounding CPU a peer can spend for free. The size cap on
       ws_frag bounds MEMORY but not the frame count: a peer can reach it one
       byte at a time and pay per-frame cost each time. And a ping obliges a
       pong, so an unlimited ping rate is an unlimited work rate. Each budget
       refills when the peer makes real progress -- a completed message -- so a
       normal client never sees them. */
    int ws_frag_frames;  /* frames in the message being reassembled */
    int ws_ctl_budget;   /* control frames left before real work must happen */
    /* SSE mode (after a successful 200 on an sse route): the handler owns the
       connection and pushes events; there is no inbound grammar at all. */
    int is_sse;
    JSValue sse_handlers; /* {open,close} (dup) */
    JSValue sse_this;     /* the SseConn JS object (conn holds a strong ref) */
    dyn_sse_t *sse_native; /* SseConn opaque; sse_native->conn nulled on teardown */
    /* The current request's Accept-Encoding, parsed at dispatch: 1 = gzip
       permitted. Per-request, so pipelined requests each get their own. */
    int accept_gzip;
    /* 1 = the request explicitly refuses identity (identity;q=0 or *;q=0),
       so an uncompressed response would violate the negotiation. */
    int identity_refused;
    /* 1 = this request asked to close (Connection: close, or HTTP/1.0 with
       no keep-alive): the response's send completion closes the socket, and
       no pipelined request behind it is answered. */
    int close_after;
    /* Responses currently parked on a handler promise (a dynamic handler or
       async rpc method returned a thenable). While one is outstanding the
       request pump HOLDS: buffered pipelined requests are not dispatched
       until the parked response settles. That keeps responses in request
       order on the wire (RFC 9112) and stops a later request's
       Connection: close from closing the socket out from under an earlier
       still-pending response. The held bytes are bounded by the same
       whole-request cap as every other buffered request. */
    int resp_parked;
    /* dyn_app_process re-entry guard: a thenable may settle synchronously
       (its then() calls the reaction before returning), so a settle can
       land while the pump is already on the stack. */
    int in_process;
    /* Dispatch timestamp of the request being answered, for the duration
       histogram. 0 = none in flight (async handlers settle later). */
    uint64_t req_start_ms;
    /*1 when the request line said HTTP/1.1 -- chunked responses are
       illegal on 1.0 peers, so a streaming result refuses instead. */
    int http11;
    /*the live response-stream pump, or NULL. Owned like the upload
       state: freed at pump end / conn close, one conn ref held throughout. */
    dyn_app_stream_t *st;
    /* Where the CRLFCRLF search resumes. Without it every recv rescans the
       whole accumulated buffer from byte 0, and nothing is consumed while the
       header block is incomplete, so the buffer never moves: a header arriving
       in K chunks costs O(K * total). The 64 KiB cap bounds the BYTES a peer
       can send, not the WORK -- one byte per packet is 65536 events and about
       2.15e9 bytes scanned, a 32768x amplification, on the single JS thread
       where it blocks every other connection. Same rule as the WebSocket
       budgets above: bound what a peer can demand for free. */
    size_t hdr_scan_from;
    /* Idle timeout (CWE-400). Stamped when the peer makes PROGRESS -- bytes
       actually arrive -- not on every wakeup, because a dribbling client wakes
       the loop constantly while achieving nothing. Intrusive links so accepting
       a connection costs no second allocation. */
    uint64_t last_ms;
    void *lprev, *lnext;   /* dyn_app_conn_t*; void* because the struct is anonymous */
} dyn_app_conn_t;

struct dyn_app_upload {
    int fd;
    int64_t remaining; /* body bytes not yet handed to the disk pool */
    int64_t size;      /* total content-length */
    char *path;
    char ctype[128];
    /* The handler VALUE, not a route pointer: registration reallocs the route
       array, and an upload outlives the turn it started on -- a pointer into
       the old array was a use-after-free the next app.upload() could fire. */
    JSValue handler;
    /* ---- async drain state (the machine: drain -> wdone -> resume) ----
       Body chunks are written with dyn_aio_write on the aio disk pool, a few
       in flight, instead of blocking write(2) on the reactor thread. Two
       events drive the machine: bytes arrive (dyn_app_upload_drain) and a
       write completes (dyn_app_upload_wdone); both land in
       dyn_app_upload_resume, which submits what is buffered and finishes the
       upload once the declared content-length is fully on disk. */
    int64_t off;       /* file offset the next chunk is submitted at */
    int wbusy;         /* writes in flight on the pool */
    int err;           /* first failed write, as -errno; 0 = clean */
};

/* Unlink from the App's live list. Idempotent: a conn is unlinked when it
 * closes, and again when its last ref drops, and those are different moments. */
static void dyn_app_conn_unlink(dyn_app_conn_t *c)
{
    dyn_app_t *app = c->app;
    if (c->lprev)
        ((dyn_app_conn_t *)c->lprev)->lnext = c->lnext;
    else if (app->conns == c)
        app->conns = c->lnext;
    else
        return;                 /* already unlinked */
    if (c->lnext)
        ((dyn_app_conn_t *)c->lnext)->lprev = c->lprev;
    c->lprev = c->lnext = NULL;
}

static void dyn_app_conn_unref(dyn_app_conn_t *c)
{
    if (--c->refs == 0) {
        dyn_app_t *app = c->app;
        dyn_app_conn_unlink(c);
        if (c->up) { /* connection died mid-upload: drop the partial file */
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
        free(c);
        /* A conn keeps its app alive, so a close() with requests in flight
           cannot free the app out from under their settle callbacks. */
        if (--app->n_conns == 0 && app->dispose_called)
            free(app);
    }
}

static void dyn_app_conn_close(dyn_app_conn_t *c);
static void dyn_app_send_err(dyn_app_conn_t *c, int status, const char *msg);

/* Response telemetry, one call per response: a counter, the body bytes, and
   a duration observation when a dispatch stamp is live. Label-free, so a
   peer cannot grow the fixed registry; lock-free bumps, so it costs the
   request path nothing measurable. */
static void dyn_app_met_response(dyn_app_conn_t *c, double bytes)
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

/* Own-property read for the response-envelope and middleware lookups. An
 * INHERITED property must never shape a response: with one prototype-
 * pollution gadget anywhere in the app, a prototype-chain read turns a plain
 * `{}` handler return into an injected body (inherited `body`), an inherited
 * numeric `status` into a 500, or an inherited `response` into a middleware
 * short-circuit -- while the docs promise these are OWN properties. An OWN
 * accessor is still read (its getter runs, as a property read always has);
 * anything only the prototype chain provides reads as absent. Returns
 * JS_UNDEFINED when the property is not an own property, JS_EXCEPTION only
 * on a throwing read. */
static JSValue dyn_app_own_get(JSContext *ctx, JSValueConst obj, const char *key)
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
    /* The own property EXISTS, so this read cannot fall through to the
     * prototype chain; an own accessor invokes its getter exactly as before. */
    v = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return v;
}

/* ---- WebSocket (RFC 6455) ------------------------------------------- */

struct dyn_ws { dyn_app_conn_t *conn; }; /* nulled on teardown; borrowed ptr */
static JSClassID dyn_ws_class_id;

/* The RFC 6455 accept key: base64(sha1(clientKey || GUID)). Both primitives now
 * come from the shared pure-C libraries (src/core/dyn-hash.c, dyn-codec.c) --
 * this is a security-relevant value on the request path, so it runs the
 * implementations the FIPS 180-4 and RFC 4648 vectors cover rather than the
 * compact local copies that used to live here. */
static void dws_accept(const char *key, size_t klen, char out[32])
{
    uint8_t buf[64], d[20];
    size_t kn = klen < 24 ? klen : 24, n;

    memcpy(buf, key, kn);
    memcpy(buf + kn, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    dyn_sha1(buf, kn + 36, d);
    /* 20 digest bytes -> exactly 28 base64 characters, so 29 with the
     * terminator fits `out[32]`. The core returns a length and does not
     * terminate; callers here treat the result as a C string. */
    n = dyn_codec_base64_encode(d, sizeof(d), out);
    out[n] = '\0';
}

/* Send a server frame (FIN=1, unmasked). opcode: 1 text, 2 binary, 8 close,
 * 9 ping, 10 pong. */
static void dyn_ws_send_frame(dyn_app_conn_t *c, int opcode, const uint8_t *data,
                              size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10];
    size_t hn;
    int i;
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (opcode & 0x0f);
    if (len < 126) { h[1] = (uint8_t)len; hn = 2; }
    else if (len <= 0xffff) { h[1]=126; h[2]=(len>>8)&0xff; h[3]=len&0xff; hn=4; }
    else { h[1]=127; for (i=0;i<8;i++) h[2+i]=(uint8_t)((uint64_t)len>>((7-i)*8)); hn=10; }
    dyn_iobuf_append(&f, h, hn);
    if (len) dyn_iobuf_append(&f, data, len);
    /* Backpressure: a peer that never reads must not get an unbounded queue */
    if (dyn_aio_queued(c->app->aio, c->fd) + f.len > DYN_HTTP_OUTBOUND_MAX) {
        dyn_iobuf_free(&f);
        dyn_app_conn_close(c);
        return;
    }
    dyn_aio_send(c->app->aio, c->fd, f.data, f.len, 0, NULL, NULL);
    dyn_iobuf_free(&f);
}

static void dyn_ws_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(val, dyn_ws_class_id);
    (void)rt;
    free(w);
}
static const JSClassDef dyn_ws_class = { "WsConn", .finalizer = dyn_ws_finalizer };

/* conn.send(data) -- data string => text frame, ArrayBuffer => binary. */
static JSValue dyn_ws_send(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(this_val, dyn_ws_class_id);
    const char *str = NULL;
    uint8_t *abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w) return JS_ThrowTypeError(ctx, "not a WsConn");
    if (argc < 1) return JS_UNDEFINED;
    /* coerce data FIRST (may run JS), THEN check the (possibly closed) conn */
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) return JS_EXCEPTION;
    } else {
        abuf = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (abuf) binary = 1;
        else { str = JS_ToCStringLen(ctx, &len, argv[0]); if (!str) return JS_EXCEPTION; }
    }
    if (w->conn && !w->conn->closed)
        dyn_ws_send_frame(w->conn, binary ? 2 : 1,
                          str ? (const uint8_t *)str : abuf, len);
    if (str) JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue dyn_ws_close_method(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_ws_t *w = (dyn_ws_t *)JS_GetOpaque(this_val, dyn_ws_class_id);
    (void)argc; (void)argv;
    if (!w) return JS_ThrowTypeError(ctx, "not a WsConn");
    if (w->conn && !w->conn->closed) {
        dyn_ws_send_frame(w->conn, 8, NULL, 0); /* close frame */
        dyn_app_conn_close(w->conn);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_ws_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_ws_send),
    JS_CFUNC_DEF("close", 0, dyn_ws_close_method),
};

/* The RFC 6455 4.2.1 opening-handshake checks the old code skipped: any
 * `Upgrade` value (h2c included) upgraded, no version was required, and the
 * key length was unexamined. Each rule here is one the RFC states with MUST;
 * failing any is "return an appropriate error code (such as 400)". */
static int dyn_ws_handshake_valid(const char *base, size_t head_len)
{
    const char *key, *upg, *conn, *ver;
    size_t keylen = 0, upglen = 0, connlen = 0, verlen = 0, b;

    if (head_len < 5 || memcmp(base, "GET ", 4) != 0)
        return 0;                              /* 1: an HTTP/1.1+ GET */
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    upg = dyn_req_header(base, head_len, "upgrade", &upglen);
    conn = dyn_req_header(base, head_len, "connection", &connlen);
    ver = dyn_req_header(base, head_len, "sec-websocket-version", &verlen);
    if (!key || keylen != 24)                  /* 5: base64 of 16 bytes */
        return 0;
    for (b = 0; b < keylen; b++) {
        char ch = key[b];
        int okc = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' ||
                  ch == '=';
        if (!okc)
            return 0;
    }
    if (!dyn_hdr_token(upg, upglen, "websocket"))   /* 3 */
        return 0;
    if (!dyn_hdr_token(conn, connlen, "upgrade"))   /* 4 */
        return 0;
    while (verlen > 0 && (*ver == ' ' || *ver == '\t')) { ver++; verlen--; }
    while (verlen > 0 && (ver[verlen-1] == ' ' || ver[verlen-1] == '\t'))
        verlen--;
    return verlen == 2 && ver[0] == '1' && ver[1] == '3';   /* 6 */
}

/* Perform the RFC 6455 Upgrade on connection `c`: send 101, switch to WS mode,
 * create the WsConn object, call the open handler. Returns 1 if upgraded. */
static int dyn_app_ws_handshake(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                const char *base, size_t head_len)
{
    JSContext *ctx = c->app->ctx;
    const char *key;
    size_t keylen = 0;
    char accept[32], resp[256];
    int rn;
    dyn_ws_t *w;
    JSValue obj, oh;

    if (!dyn_ws_handshake_valid(base, head_len))
        return 0;
    key = dyn_req_header(base, head_len, "sec-websocket-key", &keylen);
    dws_accept(key, keylen, accept);
    rn = snprintf(resp, sizeof(resp),
                  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    dyn_aio_send(c->app->aio, c->fd, resp, (size_t)rn, 0, NULL, NULL);

    c->is_ws = 1;
    c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
    c->ws_frag_frames = 0;
    c->ws_handlers = JS_DupValue(ctx, rt->handler);
    w = (dyn_ws_t *)malloc(sizeof(*w));
    if (!w)
        return 1; /* upgraded; capability object unavailable (OOM) */
    w->conn = c;
    c->ws_native = w;
    obj = JS_NewObjectClass(ctx, dyn_ws_class_id);
    JS_SetOpaque(obj, w);
    c->ws_this = obj; /* the connection owns this reference */

    oh = JS_GetPropertyStr(ctx, rt->handler, "open");
    if (JS_IsFunction(ctx, oh)) {
        JSValueConst a[1] = { obj };
        dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
    }
    JS_FreeValue(ctx, oh);
    return 1;
}

static void dyn_app_ws_dispatch_msg(dyn_app_conn_t *c, int opcode,
                                    const uint8_t *payload, size_t plen)
{
    JSContext *ctx = c->app->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, c->ws_handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        /* RFC 6455 8.1.1: text messages must be valid UTF-8; fail the
         * connection with 1007. The old path handed handlers a silently
         * mangled string that never existed on the wire. */
        JS_FreeValue(ctx, mh);
        {
            uint8_t cl[2] = { 0x03, 0xef };        /* 1007, big-endian */
            dyn_ws_send_frame(c, 8, cl, 2);
        }
        c->close_code = 1007;
        dyn_app_conn_close(c);
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? JS_NewArrayBufferCopy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char *)payload, plen);
        JSValueConst args[3] = { c->ws_this, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

/* Parse and dispatch every complete WS frame buffered in c->in. Caller holds a
 * ref on `c` (a control frame may close it mid-loop). v1: FIN assumed (each
 * frame is a complete message); client frames must be masked (per RFC). */
static void dyn_app_ws_process(dyn_app_conn_t *c)
{
    for (;;) {
        uint8_t *p = dyn_iobuf_rdata(&c->in);
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
            if (avail < 4) return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10) return;
            plen = 0;
            for (i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (!masked) { dyn_app_conn_close(c); return; } /* RFC: must be masked */
        /* RFC 6455 5.5: a control frame carries at most 125 octets and is never
         * fragmented. Without the length rule a ping is a reflector -- the
         * payload is echoed in the pong, so 64 pings (the control budget) of
         * DYN_ACONN_MAX_REQ each is 64 MiB the server buffers and writes back.
         * Failing the connection is what the RFC prescribes for both. */
        if (opcode >= 0x8 && (plen > 125 || !fin)) {
            dyn_app_conn_close(c);
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) { /* cap BEFORE the size math to avoid
                                         * hdr+4+plen overflow -> OOB read */
            dyn_app_conn_close(c);
            return;
        }
        if (avail < hdr + 4 + plen) return; /* need mask key + full payload */
        mask = p + hdr;
        payload = p + hdr + 4;
        /* Unmask 8 bytes at a time. The key repeats every 4 bytes from i=0, so
           every 8-byte block sits at mask phase 0 and one 64-bit key serves them
           all. memcpy for the unaligned access (a cast is UB), and the key is
           built from the same memory order as the payload, so this is
           endian-independent. Byte loop for the tail. */
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
            for (i = (uint32_t)k; i < plen; i++) payload[i] ^= mask[i & 3];
        }
        frame_total = hdr + 4 + (size_t)plen;

        if (opcode == 0x8) {                /* close */
            dyn_ws_send_frame(c, 8, NULL, 0);
            dyn_iobuf_consume(&c->in, frame_total);
            dyn_app_conn_close(c);
            return;
        } else if (opcode == 0x9) {         /* ping -> pong */
            if (--c->ws_ctl_budget < 0) { dyn_app_conn_close(c); return; }
            dyn_ws_send_frame(c, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {         /* pong: ignore */
        } else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
            int frag_active = c->ws_frag_op != 0;
            /* protocol errors: stray continuation, or a new message mid-fragment */
            if ((opcode == 0x0 && !frag_active) || (opcode != 0x0 && frag_active)) {
                dyn_app_conn_close(c); return;
            }
            if (opcode != 0x0 && fin) {
                c->ws_ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_app_ws_dispatch_msg(c, opcode, payload, (size_t)plen); /* unfragmented */
            } else { /* fragmented: accumulate, dispatch on FIN */
                if (opcode != 0x0) { c->ws_frag_op = opcode; c->ws_frag_frames = 0; }
                if (++c->ws_frag_frames > DYN_WS_MAX_FRAGMENTS ||
                    dyn_iobuf_rlen(&c->ws_frag) + plen > DYN_ACONN_MAX_REQ ||
                    dyn_iobuf_append(&c->ws_frag, payload, (size_t)plen) < 0) {
                    dyn_app_conn_close(c); return;
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
        } /* other opcodes: ignore */
        dyn_iobuf_consume(&c->in, frame_total);
        if (c->closed)
            return;
    }
}

/* ---- Server-Sent Events ----------------------------------------------
 * An sse route keeps the connection open after the 200 and the handler
 * pushes events. There is no inbound grammar: bytes from the client are
 * discarded. The lifecycle is the client's disconnect or the handler's
 * close() -- the idle sweep does not apply, because a healthy stream is
 * server-driven and can be silent far longer than idle_ms.
 */

struct dyn_sse { dyn_app_conn_t *conn; }; /* nulled on teardown; borrowed */
static JSClassID dyn_sse_class_id;

static void dyn_sse_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_sse_t *s = (dyn_sse_t *)JS_GetOpaque(val, dyn_sse_class_id);
    (void)rt;
    free(s);
}
static const JSClassDef dyn_sse_class = { "SseConn", .finalizer = dyn_sse_finalizer };

/* One event per the wire grammar: an optional `event:` line, then one
   `data:` line PER LINE of payload (a newline inside the data is a frame
   break otherwise), then the blank-line terminator. */
/* Returns 0, or -1 on OOM (a partial frame is a corrupt stream: the caller
   closes the connection rather than sending a frame that ends mid-line). */
static int dyn_sse_frame(dyn_iobuf_t *f, const char *data, size_t dlen,
                         const char *event, size_t elen)
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
        i++;                            /* past the \n: another data: line */
    }
    return dyn_iobuf_append(f, "\n", 1);
}

/* conn.send(data[, event]) -- frame and push one event. */
static JSValue dyn_sse_send(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_sse_t *s = (dyn_sse_t *)JS_GetOpaque(this_val, dyn_sse_class_id);
    const char *data = NULL, *event = NULL;
    size_t dlen = 0, elen = 0;
    dyn_iobuf_t f;

    if (!s) return JS_ThrowTypeError(ctx, "not an SseConn");
    if (argc < 1) return JS_ThrowTypeError(ctx, "send(data[, event])");
    /* coerce FIRST (may run user code), THEN touch the (possibly closed) conn */
    data = JS_ToCStringLen(ctx, &dlen, argv[0]);
    if (!data) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        event = JS_ToCStringLen(ctx, &elen, argv[1]);
        if (!event) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }
        if (memchr(event, '\n', elen) || memchr(event, '\r', elen)) {
            JS_FreeCString(ctx, event);
            JS_FreeCString(ctx, data);
            return JS_ThrowTypeError(ctx, "SseConn.send: an event name cannot "
                                          "contain a newline");
        }
    }
    /* A CR is a line break to some parsers and invisible to the splitter
       here: a frame-smuggling hole. Refuse it rather than split silently. */
    if (memchr(data, '\r', dlen)) {
        if (event) JS_FreeCString(ctx, event);
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "SseConn.send: data cannot contain \\r");
    }
    if (s->conn && !s->conn->closed) {
        dyn_iobuf_init(&f);
        if (dyn_sse_frame(&f, data, dlen, event, elen) < 0) {
            /* OOM mid-frame: end the stream rather than send a frame that
               breaks the grammar the client's parser relies on. */
            dyn_app_conn_close(s->conn);
        } else if (dyn_aio_queued(s->conn->app->aio, s->conn->fd) + f.len
                   > DYN_HTTP_OUTBOUND_MAX) {
            /* The peer stopped reading: bound the queue by ending the stream */
            dyn_app_conn_close(s->conn);
        } else {
            dyn_aio_send(s->conn->app->aio, s->conn->fd, f.data, f.len, 0,
                         NULL, NULL);
            s->conn->last_ms = dyn_timer_now_ms(); /* a push is liveness */
        }
        dyn_iobuf_free(&f);
    }
    if (event) JS_FreeCString(ctx, event);
    JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static JSValue dyn_sse_close_method(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_sse_t *s = (dyn_sse_t *)JS_GetOpaque(this_val, dyn_sse_class_id);
    (void)argc; (void)argv;
    if (!s) return JS_ThrowTypeError(ctx, "not an SseConn");
    if (s->conn && !s->conn->closed)
        dyn_app_conn_close(s->conn);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_sse_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_sse_send),
    JS_CFUNC_DEF("close", 0, dyn_sse_close_method),
};

/* Answer 200 with the SSE content type, switch the connection to SSE mode,
   create the SseConn object and call the open handler with it. */
static void dyn_app_sse_handshake(dyn_app_conn_t *c,
                                  const dyn_app_route_t *rt)
{
    JSContext *ctx = c->app->ctx;
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
    dyn_sse_t *s;
    JSValue obj, oh;

    /* Allocate BEFORE announcing success: a 200 followed by no SseConn is a
       stream the client can never close server-side -- refuse instead. */
    s = (dyn_sse_t *)malloc(sizeof(*s));
    if (!s) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    s->conn = c;
    c->sse_native = s;
    obj = JS_NewObjectClass(ctx, dyn_sse_class_id);
    JS_SetOpaque(obj, s);
    c->sse_this = obj; /* the connection owns this reference */
    dyn_aio_send(c->app->aio, c->fd, hdr, sizeof(hdr) - 1, 0, NULL, NULL);
    c->is_sse = 1;
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

/* Unsigned decimal, no format-string scan. Returns bytes written. */
/* dyn_put_u64 is defined above dyn_http_format (the static pump's head uses
   it too; one definition, not two). */

/* Smallest body worth a gzip pass: the framing is 18 bytes plus block
   overhead, so below this the saving rarely covers the bytes and the CPU
   spent is pure tax. */
#define DYN_APP_GZIP_MIN 256

/* 1 if the Accept-Encoding value permits gzip: a `gzip` or `*` token whose
   q-value is not 0. Tokens are comma-separated; parameters follow ';'. A
   token match is on the whole token -- "xgzip" does not match "gzip". */
/* The effective q of one whole token in an Accept-Encoding value: q*1000
   (1000 = default weight), 0 for an explicit q=0, or -1 when the token is
   absent. Tokens are comma-separated; parameters follow ';'. A token match
   is on the whole name -- "xgzip" does not match "gzip" -- and the q
   parameter is matched by NAME after a ';', so a bare 'q' inside some other
   parameter's value cannot veto. Only q=0 refuses (RFC 9110 12.5.3): ANY
   q>0 -- q=0.05 included -- accepts at that weight. */
static int dyn_ae_q(const char *v, size_t n, const char *tok, size_t tl)
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
                    const char *n = pp + 1, *eq;
                    while (n < pe && (*n == ' ' || *n == '\t'))
                        n++;
                    eq = (*n == 'q' || *n == 'Q') ? n + 1 : NULL;
                    if (eq) {
                        while (eq < pe && (*eq == ' ' || *eq == '\t'))
                            eq++;
                    }
                    if (eq && *eq == '=') {
                        const char *q = eq + 1;
                        int seen = 0, is_zero = 1;
                        while (q < pe && (*q == ' ' || *q == '\t'))
                            q++;          /* OWS after '=' is legal */
                        /* q=0 in any spelling (0, 0.0, 0.000) refuses; a
                           nonzero digit ANYWHERE (0.05 included) accepts. */
                        if (q < pe && *q >= '0' && *q <= '9') {
                            seen = 1;
                            if (*q != '0') is_zero = 0;
                            q++;
                        }
                        while (q < pe && *q >= '0' && *q <= '9') {
                            if (*q != '0') is_zero = 0;
                            q++;
                        }
                        if (q < pe && *q == '.') {
                            q++;
                            while (q < pe && *q >= '0' && *q <= '9') {
                                if (*q != '0') is_zero = 0;
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

/* Small-response ceiling for the no-assembly fast path below: head (<=512)
 * plus body fitting in one 1 KiB stack buffer. Covers the entire JSON-RPC
 * envelope class (ping/pong, small results, every error body) -- responses
 * this size used to pay an iobuf init, a heap malloc and two appends each. */
#define DYN_APP_SMALL_RESP 1024

/* dyn_aio_close discards queued bytes, so "respond, then close" has to ride
 * the send completion: the ref the caller held for udata dies here. */
static void dyn_app_close_after_send(dyn_aio_t *aio, int res,
                                     const uint8_t *buf, unsigned len,
                                     void *udata)
{
    dyn_app_conn_t *c = (dyn_app_conn_t *)udata;
    (void)aio; (void)res; (void)buf; (void)len;
    dyn_app_conn_close(c);
    dyn_app_conn_unref(c);
}

/* Queue a full HTTP response onto the connection. Every App response funnels
   here, which makes this the one place compression and response telemetry
   can live without per-route duplication. `extra` is an optional extra
   header line ("Allow: ...\r\n") inserted after the status line -- the 405
   path's RFC 9110 MUST. */
static void dyn_app_send_body_x(dyn_app_conn_t *c, int status,
                                const char *ctype, const char *body,
                                size_t body_len, const char *extra)
{
    /* The 200 header is constant apart from Content-Length, and this runs once
       per response -- at ~110k req/s the snprintf format scan was showing up as
       __vfprintf in the profile. Bytes are identical to the fallback below. */
    static const char hdr200_a[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
    static const char hdr_b[] = "\r\nConnection: keep-alive\r\n\r\n";
    dyn_iobuf_t out;
    char head[512];
    const char *sbody = body;
    size_t slen = body_len;
    uint8_t *gz = NULL;
    size_t gz_len = 0;
    int n, compressed = 0;

    /* The request's Accept-Encoding was parsed into c->accept_gzip at
       dispatch. A body that does not shrink is sent identity. */
    if (c->app->compress && c->accept_gzip
        && (body_len >= DYN_APP_GZIP_MIN || c->identity_refused)
        && dyn_gzip_build((const uint8_t *)body, body_len, &gz,
                          &gz_len) == 0) {
        /* Accept gzip when it shrinks -- or, when identity is refused,
           accept it regardless of size: it is the only representation the
           client accepts, and a 406 must be the last resort. */
        if (gz && (gz_len < body_len || c->identity_refused)) {
            compressed = 1;
            sbody = (const char *)gz;
            slen = gz_len;
        } else {
            free(gz);
            gz = NULL;
        }
    }
    if (!compressed && c->identity_refused) {
        /* Nothing the client accepts: 406 (RFC 9110 12.5.3). The refusal
           notice itself is exempt from the negotiation, or it would
           recurse into the same check. */
        c->identity_refused = 0;
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        return;
    }

    dyn_iobuf_init(&out);
    if (compressed) {
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
    if (n > 0) {
        /* Fast path: a head+body that fits one stack buffer is sent straight
           from it -- no iobuf, no heap allocation, two memcpys. The iobuf
           path below is byte-for-byte what large/compressed responses
           always did. */
        if ((size_t)n + slen <= DYN_APP_SMALL_RESP) {
            char small[DYN_APP_SMALL_RESP];
            memcpy(small, head, (size_t)n);
            if (slen)
                memcpy(small + n, sbody, slen);
            if (c->close_after) {
                /* The close must not truncate THIS response: ride its
                   completion. An INLINE send failure never runs the callback,
                   so the held ref and the close are handled here instead of
                   stranding both. */
                c->refs++;
                if (dyn_aio_send(c->app->aio, c->fd, small,
                                 (size_t)n + slen, 0,
                                 dyn_app_close_after_send, c) < 0)
                    dyn_app_close_after_send(NULL, -ECONNRESET, NULL, 0, c);
            } else {
                dyn_aio_send(c->app->aio, c->fd, small, (size_t)n + slen, 0,
                             NULL, NULL);
            }
            free(gz);
            dyn_app_met_response(c, (double)slen);
            return;
        }
        dyn_iobuf_init(&out);
        dyn_iobuf_append(&out, head, (size_t)n);
        if (slen)
            dyn_iobuf_append(&out, sbody, slen);
        if (c->close_after) {
            /* The close must not truncate THIS response: ride its completion.
               An INLINE send failure never runs the callback, so the held ref
               and the close are handled here instead of stranding both. */
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                             dyn_app_close_after_send, c) < 0)
                dyn_app_close_after_send(NULL, -ECONNRESET, NULL, 0, c);
        } else {
            dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0, NULL, NULL);
        }
        dyn_iobuf_free(&out);
    }
    free(gz);
    dyn_app_met_response(c, (double)slen);
}

static void dyn_app_send_body(dyn_app_conn_t *c, int status, const char *ctype,
                              const char *body, size_t body_len)
{
    dyn_app_send_body_x(c, status, ctype, body, body_len, NULL);
}

static void dyn_app_send_json(dyn_app_conn_t *c, int status, const char *body,
                              size_t body_len)
{
    dyn_app_send_body(c, status, "application/json", body, body_len);
}

/* Append `s` to buf as a JSON string BODY (no surrounding quotes), escaping
   ", \\ and control bytes. Stops before buf[limit]; the caller reserves room
   for what follows. Exception text reaches these bodies verbatim today, and a
   quote in it made every error response invalid JSON. The explicit-length
   form exists because error text is arbitrary JS string data -- it can carry
   embedded NULs, and a NUL-terminated walk silently dropped everything after
   the first one from the error body. */
static void dyn_json_escape_n_into(char *buf, size_t limit, size_t *pos,
                                   const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && *pos + 7 < limit; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '"'; }
        else if (ch == '\\') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '\\'; }
        else if (ch == '\n') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'n'; }
        else if (ch == '\r') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'r'; }
        else if (ch == '\t') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 't'; }
        else if (ch < 0x20)
            *pos += (size_t)snprintf(buf + *pos, 7, "\\u%04x", ch);
        else
            buf[(*pos)++] = (char)ch;
    }
}

/* Compose one JSON-RPC 2.0 error object. Room for the tail is reserved up
   front so an over-long message truncates the MESSAGE, never the JSON. */
static size_t dyn_rpc_err_compose_n(char *buf, size_t cap, int code,
                                    const char *msg, size_t msg_len,
                                    const char *id_json)
{
    const char *idsafe = id_json && *id_json ? id_json : "null";
    size_t idlen = strlen(idsafe);
    size_t tail_room, pos = 0;
    int n;

    /* The id is attacker-controlled and unbounded; one that cannot fit is
       echoed as null. Without the clamp tail_room exceeds cap and the size_t
       `cap - tail_room` below UNDERFLOWS, handing the escaper an unbounded
       limit -- a stack overflow with a long message (found in review). */
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

static size_t dyn_rpc_err_compose(char *buf, size_t cap, int code,
                                  const char *msg, const char *id_json)
{
    return dyn_rpc_err_compose_n(buf, cap, code, msg,
                                 msg ? strlen(msg) : 0, id_json);
}

/* Build a JSON-RPC 2.0 error string {"jsonrpc":"2.0","error":{code,message},id}
 * with a raw (already-JSON) id token, and send it. */
static void dyn_app_rpc_error(dyn_app_conn_t *c, int http_status, int code,
                              const char *msg, const char *id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

/* The explicit-length twin: thrown text can carry embedded NULs and must not
 * be truncated at the first one. */
static void dyn_app_rpc_error_n(dyn_app_conn_t *c, int http_status, int code,
                                const char *msg, size_t msg_len,
                                const char *id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose_n(buf, sizeof(buf), code, msg, msg_len,
                                     id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

/* ====: response streaming -- handlers may return a ByteSource ========
 *
 * A route handler that returns a dyna:stream-compatible ByteSource (any
 * object with a callable `read`; the shape stream.pipe/lines/ndjson accept)
 * -- or `{ stream, contentType?, status? }` -- opts that response OUT of
 * the JSON-RPC envelope: the App answers with the handler's status (200
 * default), the handler's content type ("application/octet-stream"
 * default), `Transfer-Encoding: chunked`, and PUMPS the source -- one
 * read(buf) per chunk, each read's bytes framed and sent before the next
 * read is issued, so backpressure is the socket and a runaway source is
 * bounded by the peer walking away. read() returning 0 ends the body with
 * the terminal chunk; the source's close() (when present) runs at that
 * point. Whatever the handler throws or rejects DURING streaming is past
 * the status line already: the connection truncates (a chunked body
 * without its terminal chunk is precisely how a peer sees "broken") -- the
 * only behaviour an HTTP server has once headers have flushed.
 *
 * Contracts, stated:
 * - HTTP/1.0 peers get a 500 instead ("streaming requires HTTP/1.1"):
 *   chunked framing does not exist below 1.1, and a buffered fallback
 *   would need a whole-body memory cap this path deliberately lacks.
 * - A peer refusing identity encoding gets 406: streamed bodies go
 *   identity, uncompressed (an honest compressed pump would need the
 *   whole stream to size the gzip member).
 * - A ByteSource in a BATCH response is an internal error: a batch is ONE
 *   HTTP response and two streams cannot share it.
 * - read() runs as user JS: the pump holds the source, its read function
 *   and the chunk buffer as C-held dup'd refs on the conn (the same
 *   anchoring the WS/SSE handlers use) and re-resolves the chunk view on
 *   EVERY frame -- no raw pointer into a source outlives a user-JS call.
 * - ONE stream per conn, and the request pump HOLDS for its lifetime: a
 *   request arriving while a stream pumps (pipelined or written mid-stream)
 *   is buffered and dispatched only after the stream's terminal chunk (or
 *   its truncation) -- responses leave in request order, and an ordinary
 *   response can never be injected INSIDE a live chunked body. The pump
 *   slot itself is multi-safe on top of that: a second stream can never
 *   overwrite a live one (the refusal truncates -- the only protocol-safe
 *   answer while a body is in flight). Hold-then-dispatch was chosen over
 *   refusing the second request because it IS the HTTP/1.1 pipelining
 *   contract (responses in request order on one connection) and it matches
 *   the hold the pump already applies to parked handler promises; refusing
 *   would break well-formed pipelined clients that the wire protocol owes
 *   service.
 * - Every read() promise gets its reaction attached BEFORE the pump can
 *   abandon it, through dyn_app_read_attach: genuine promises are attached
 *   via Promise.prototype.then.call so a hostile OWN `then` (a Promise
 *   subclass whose then() throws) can never block the attach; a non-promise
 *   thenable genuinely needs its own `then` invoked, and one that throws
 *   synchronously is swallowed (the pair it is handed is once-only). A
 *   rejecting read truncates the body (the documented failure signal once
 *   the status line is out); an abandoned read's late rejection is
 *   swallowed deliberately and must never surface as an unhandled
 *   rejection.
 *
 * Re-entrancy and teardown discipline: sends complete on the loop,
 * sometimes INLINE (before dyn_aio_send returns), and the pump runs user
 * JS (read(), a source's close()) that can close the conn -- in both cases
 * a completion or an abort lands in the middle of a pump frame that still
 * holds the state. The state is therefore NEVER freed where teardown is
 * requested: every piece of pump code that touches the state brackets
 * itself with dyn_app_stream_enter/leave, teardown merely marks `retiring`,
 * and the LAST frame to leave performs the free (dyn_app_stream_reap --
 * the single call site of the free). An inline send completion thus only
 * clears in_send (the frame continues driving the loop from state that is
 * provably alive); a completion after the frame returned re-enters resume.
 * Exactly one pump loop runs per conn, and exactly one owner frees.
 */

struct dyn_app_stream {
    JSValue src;        /* the ByteSource (dup'd; anchors the pump) */
    JSValue readfn;     /* its read method (dup'd once) */
    JSValue chunk;      /* the 64 KiB Uint8Array read() fills */
    JSValue pending;    /* the in-flight read's promise, while pending */
    uint8_t *frame;     /* chunk frame scratch: header + data + CRLF */
    size_t   frame_cap;
    uint64_t total;     /* body bytes handed to the socket */
    int      status;
    char    *ct;
    int      done;      /* the pump reached its end (cleanly or not) */
    int      busy;      /* a resume loop is running */
    int      in_send;   /* a dyn_aio_send is outstanding for this pump */
    int      depth;     /* pump frames on the stack (enter/leave) */
    int      retiring;  /* teardown requested; the last leave frees */
};

#define DYN_APP_STREAM_CHUNK (64 * 1024)

static void dyn_app_stream_resume(dyn_app_conn_t *c);
static void dyn_app_stream_stop(dyn_app_conn_t *c);
static void dyn_app_pump_resume(dyn_app_conn_t *c);
static void dyn_app_stream_sent(dyn_aio_t *aio, int res,
                                const uint8_t *buf, unsigned len, void *udata);
static void dyn_app_stream_finished(dyn_aio_t *aio, int res,
                                    const uint8_t *buf, unsigned len,
                                    void *udata);

/* 1 when `v` duck-types as a ByteSource: an object with a callable read.
   Every dyna:stream consumer accepts exactly this, so the App does too. */
static int dyn_value_is_byte_source(JSContext *ctx, JSValueConst v)
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

/* The { stream, contentType?, status? } form: an object naming a `stream`
   that is itself a ByteSource. Returns the stream value (own ref) or
   JS_UNDEFINED when the shape is not that. */
static JSValue dyn_value_stream_member(JSContext *ctx, JSValueConst v)
{
    JSValue sv;
    if (!JS_IsObject(v) || dyn_value_is_byte_source(ctx, v))
        return JS_UNDEFINED;
    /* OWN property only, like every envelope read: a polluted prototype must
     * not turn an unrelated return into a streamed response. */
    sv = dyn_app_own_get(ctx, v, "stream");
    if (!dyn_value_is_byte_source(ctx, sv)) {
        JS_FreeValue(ctx, sv);
        return JS_UNDEFINED;
    }
    return sv;
}
/* Free the pump and drop its conn ref. THE SINGLE FREE POINT: only
   dyn_app_stream_reap calls this, and only when no frame can still touch
   the state. */
static void dyn_app_stream_stop(dyn_app_conn_t *c)
{
    dyn_app_stream_t *st = c->st;
    JSContext *ctx = c->app->ctx;
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
    /* The streamed response is OVER (terminal sent, or truncated): the
       requests the pump held behind it dispatch now, in request order (the
       same resume a released parked response does). Ordered after the slot
       clears -- the hold is `c->st` -- and BEFORE the pump's conn ref drops,
       so the conn is provably alive across the dispatch's user JS. When the
       free lands inside the request pump's own frame the resume is a no-op
       (in_process) and that loop simply continues instead. */
    if (!c->closed)
        dyn_app_pump_resume(c);
    dyn_app_conn_unref(c);          /* the pump's ref */
}

/* Enter/leave bracket every piece of pump code that touches the state or
   can run user JS or an inline send completion. See the re-entrancy
   contract above: the free happens only in the last leave (via reap), so
   no frame is ever left reading or writing freed state -- the shape where
   an inline completion freed the state and the caller then steered the
   pump from it. */
static void dyn_app_stream_enter(dyn_app_stream_t *st) { st->depth++; }

/* Free the pump once nothing can touch it: teardown requested, no frame on
   the stack, and no parked read. The parked read is special: its settle
   continuation carries the CONN pointer and needs the pump's conn ref
   alive, so the settle (which clears `pending` first) is the last holder
   and reaps when it fires. */
static void dyn_app_stream_reap(dyn_app_conn_t *c)
{
    dyn_app_stream_t *st = c->st;
    if (!st || !st->retiring || st->depth > 0 ||
        !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_stop(c);         /* nothing may touch st/c after this */
}

/* Request teardown of the pump -- from anywhere: an inline completion, a
   failed send, a conn abort raised under user JS. Idempotent, and never
   frees under a live frame. */
static void dyn_app_stream_retire(dyn_app_conn_t *c)
{
    dyn_app_stream_t *st = c->st;
    if (!st)
        return;
    st->done = 1;
    st->retiring = 1;
    dyn_app_stream_reap(c);
}

/* A frame is done with the state. MAY FREE (pump and conn): a caller must
   not touch c or st after this. */
static void dyn_app_stream_leave(dyn_app_conn_t *c)
{
    dyn_app_stream_t *st = c->st;
    if (!st || --st->depth > 0)
        return;
    dyn_app_stream_reap(c);
}

/* conn close: the peer is gone or the conn is closing. Teardown is
   requested, never performed here: conn close can run in the middle of a
   pump frame (user JS closing the app mid-read or from a source's close())
   and a read may be parked (its settle owns the free). */
static void dyn_app_stream_abort(dyn_app_conn_t *c)
{
    if (!c->st)
        return;
    dyn_app_stream_retire(c);
}

/* Handle one fulfilled read of `n` bytes (the count the source resolved):
   frame the chunk around the bytes now in the pump's buffer and hand it to
   the socket, or -- for n == 0 -- run the source's close() and send the
   terminal chunk. ALWAYS called under a pump frame (resume's or the
   settle's): an INLINE completion can retire the pump inside the send and
   the frame keeps the state alive until the caller has taken its decision.
   Returns 1 when the caller should keep pumping (the send completed inline
   and the pump is still live), 0 when the pump is parked on the io backend
   or has ended, -1 on failure. */
static int dyn_app_stream_on_count(dyn_app_conn_t *c, double n)
{
    dyn_app_stream_t *st = c->st;
    JSContext *ctx = c->app->ctx;

    if (n < 0 || n > (double)DYN_APP_STREAM_CHUNK)
        return -1;              /* outside the window the contract names */
    if (n == 0) {
        /* the source's close(), when present, runs BEFORE the terminal
           chunk (a throw is dropped: the body is complete either way) */
        JSValue cf = JS_GetPropertyStr(ctx, st->src, "close");
        if (JS_IsFunction(ctx, cf))
            dyn_call_drop(ctx, cf, st->src, 0, NULL);
        JS_FreeValue(ctx, cf);
        if (st->done || c->closed)
            return 0;   /* close() aborted the pump: nothing left to send */
        st->done = 1;
        memcpy(st->frame, "0\r\n\r\n", 5);
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame, 5, 0,
                         dyn_app_stream_finished, c) < 0) {
            st->in_send = 0;
            dyn_app_stream_finished(NULL, -ECONNRESET, NULL, 0, c);
            return 0;
        }
        /* An INLINE completion already ran and may have retired the pump;
           the frame holds the state, so this read is safe either way. */
        return (st->in_send || st->retiring) ? 0 : 1;
    }
    {
        size_t un = (size_t)n;
        size_t voff, vlen, vbpe, ab_size = 0;
        int hl;
        JSValue ab;
        uint8_t *base;
        /* the chunk view's bytes: re-resolved EVERY frame -- a read may
           have detached or replaced the view while running */
        ab = JS_GetArrayBufferView(ctx, st->chunk, &voff, &vlen, &vbpe);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return -1;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base)
            return -1;              /* detached mid-read: already threw */
        if (un > vlen || voff > ab_size || un > ab_size - voff)
            return -1;              /* outside the buffer the contract names */
        hl = snprintf((char *)st->frame, 16, "%zx\r\n", un);
        if (hl <= 0 || (size_t)hl + un + 2 > st->frame_cap)
            return -1;
        memcpy(st->frame + hl, base + voff, un);
        st->frame[hl + un] = '\r';
        st->frame[hl + un + 1] = '\n';
        st->total += un;
        st->in_send = 1;
        if (dyn_aio_send(c->app->aio, c->fd, st->frame,
                         (unsigned)(hl + un + 2), 0,
                         dyn_app_stream_sent, c) < 0) {
            st->in_send = 0;
            return -1;
        }
        /* same inline-completion rule as the terminal chunk above */
        return (st->in_send || st->retiring) ? 0 : 1;
    }
}

/* The pump's failure path: the status line is already out -- truncate the
   connection (a chunked body without its terminal chunk IS the signal).
   Must run under a frame (resume's or the settle's). */
static void dyn_app_stream_fail(dyn_app_conn_t *c)
{
    if (!c->st || c->st->done || c->closed)
        return;
    c->st->done = 1;
    dyn_app_conn_close(c);
    dyn_app_stream_retire(c);       /* abort already did; idempotent */
}

/* The promise settle for a PENDING read: magic 0 fulfilled, 1 rejected.
   The data carries the conn pointer (the pump's conn ref keeps it alive).
   argv[0] IS the byte count the source resolved -- the parked loop is
   gone, so the settled value is processed HERE, never re-read. */
static JSValue dyn_app_stream_settle(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic,
                                     JSValue *data)
{
    int64_t ptr = 0;
    dyn_app_conn_t *c;
    dyn_app_stream_t *st;
    double n = -1;
    (void)this_val;
    if (JS_ToInt64(ctx, &ptr, data[0]))
        return JS_EXCEPTION;
    c = (dyn_app_conn_t *)(uintptr_t)ptr;
    st = c->st;
    if (!st)
        return JS_UNDEFINED;    /* already reaped: nothing can be parked */
    dyn_app_stream_enter(st);
    /* The parked read is over -- drop its ref FIRST: from here on this
       continuation is the holder, and reap must see `pending` clear (this
       is the read the free was waiting for). */
    JS_FreeValue(ctx, st->pending);
    st->pending = JS_UNDEFINED;
    if (st->done || c->closed) {
        /* aborted while the read was in flight: we are the last holder */
        dyn_app_stream_retire(c);
    } else {
        st->busy = 0;                   /* the parked loop is gone */
        if (!magic && argc > 0 && JS_IsNumber(argv[0])) {
            if (JS_ToFloat64(ctx, &n, argv[0]) < 0) {
                dyn_app_stream_leave(c);    /* MAY free */
                return JS_EXCEPTION;    /* n would be stale otherwise */
            }
        }
        {
            int keep = magic ? -1 : dyn_app_stream_on_count(c, n);
            if (keep == 1)
                dyn_app_stream_resume(c);   /* inline: keep the loop running */
            else if (keep != 0)
                dyn_app_stream_fail(c);
        }
    }
    dyn_app_stream_leave(c);            /* MAY free: last touch of st/c */
    return JS_UNDEFINED;
}

/* Send completion: the socket took the head/frame -- pump again. A failed
   send truncates. udata is the conn (its pump ref covers this callback). */
static void dyn_app_stream_sent(dyn_aio_t *aio, int res,
                                const uint8_t *buf, unsigned len, void *udata)
{
    dyn_app_conn_t *c = (dyn_app_conn_t *)udata;
    dyn_app_stream_t *st = c->st;
    int inline_cb;
    (void)aio; (void)buf; (void)len;
    if (!st)
        return;         /* retired under us (conn close completes sends) */
    /* A completion landing under a pump frame is the INLINE case (a send
       that finished before dyn_aio_send returned): the frame's owner keeps
       driving -- the loop continues, the settle re-enters, the stream
       entry starts the pump -- and must not find us re-entering it. Only
       a completion the loop is NOT around for re-enters resume here. */
    inline_cb = st->depth > 0;
    dyn_app_stream_enter(st);
    if (res < 0) {
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
    } else {
        st->in_send = 0;
        if (!inline_cb)
            dyn_app_stream_resume(c);   /* deferred completion: re-enter */
    }
    dyn_app_stream_leave(c);            /* MAY free */
}

/* The terminal chunk went out: keep-alive resumes; close_after rides THIS
   completion, exactly like every other close_after response. */
static void dyn_app_stream_finished(dyn_aio_t *aio, int res,
                                    const uint8_t *buf, unsigned len,
                                    void *udata)
{
    dyn_app_conn_t *c = (dyn_app_conn_t *)udata;
    dyn_app_stream_t *st = c->st;
    (void)aio; (void)buf; (void)len;
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
    /* The pump is over. This may be an INLINE completion sitting in the
       middle of dyn_app_stream_on_count -- the free is deferred to the
       last frame out, so the caller's post-send reads stay valid. */
    dyn_app_stream_retire(c);
    dyn_app_stream_leave(c);            /* MAY free */
}

/* A read() promise the pump is DROPPING gets its reactions attached first --
 * always, before the free. An abandoned read promise that later rejects (or
 * is already rejected) must NEVER surface as an unhandled rejection: the
 * engine's loop-boundary policy prints and exit(1)s the whole process over
 * one. The deliberate contract for an abandoned read is TRUNCATION (the
 * pump's fail path below) plus this swallow of the late rejection. A
 * reaction attached to an already-rejected promise also clears its
 * unhandled entry (the tracker's handled notification). */
static JSValue dyn_app_read_noop(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic,
                                 JSValue *data)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic; (void)data;
    return JS_UNDEFINED;
}

/* 1 when `pr` is a GENUINE promise -- the engine's promise class, Promise
   subclass instances included (they carry the same internal reaction list
   and the same unhandled-rejection tracking). */
static int dyn_read_is_promise(JSContext *ctx, JSValueConst pr)
{
    JSPromiseStateEnum ps = JS_PromiseState(ctx, pr);
    return ps == JS_PROMISE_PENDING || ps == JS_PROMISE_FULFILLED ||
           ps == JS_PROMISE_REJECTED;
}

/* Attach the pair through Promise.prototype.then.call: 0 landed, -1 threw.
   This BYPASSES any own `then` -- the whole point. A genuine promise may
   carry a hostile one (a Promise subclass whose then() throws), and
   invoking THAT leaves the promise's reaction list empty, so its rejection
   surfaces at the loop boundary and exit(1)s the process. */
static int dyn_app_proto_then(JSContext *ctx, JSValueConst pr,
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
        /* The one way Promise.prototype.then can still fail on a genuine
           promise is a poisoned `constructor` (its species lookup). Shadow
           it with the intrinsic and retry: a reaction must land over any
           observable nicety -- nothing else keeps a late rejection from
           killing the process. (If the object is sealed against even that,
           the caller's own-`then` fallback below is the last resort.) */
        if (JS_DefinePropertyValueStr(ctx, pr, "constructor",
                                     JS_DupValue(ctx, pcons),
                                     JS_PROP_WRITABLE |
                                     JS_PROP_CONFIGURABLE) >= 0) {
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

/* Hand the reaction pair to a read() result (the pump's single attach
   point -- the park and every abandon shape go through here). GENUINE
   promises are attached via Promise.prototype.then.call (above); a
   non-promise thenable has no internal reaction list at all and genuinely
   needs its own `then` invoked, so the current path -- get `then`, call it
   with the pair -- is kept for it (and is the last resort for a genuine
   promise whose every intrinsic route threw). A `then` that throws
   synchronously is SWALLOWED here: the pair it was handed is once-only (a
   call before the throw stays the winner) and a thenable carries no
   unhandled-rejection tracking, so every way out is the swallow/truncate
   contract, never an unhandled rejection and never a process kill.
   Returns 0 when the pair was handed over, -1 when nothing landed (the
   park then rejects ITS promise, so its settle still fires exactly once). */
static int dyn_app_read_attach(JSContext *ctx, JSValueConst pr,
                               JSValueConst onres, JSValueConst onrej)
{
    JSValue thenf, tr;
    JSValueConst a[2];

    if (!JS_IsObject(pr))
        return 0;               /* a primitive can never be a rejection */
    a[0] = onres;
    a[1] = onrej;
    if (dyn_read_is_promise(ctx, pr)) {
        if (dyn_app_proto_then(ctx, pr, onres, onrej) == 0)
            return 0;
        /* fall through: the own `then` is the only route left */
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

static void dyn_app_read_abandon(JSContext *ctx, JSValueConst pr)
{
    JSValue onres, onrej;
    if (!JS_IsObject(pr))
        return;                 /* a primitive can never be a rejection */
    onres = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 0, 0, NULL);
    onrej = JS_NewCFunctionData(ctx, dyn_app_read_noop, 0, 1, 0, NULL);
    if (!JS_IsException(onres) && !JS_IsException(onrej))
        dyn_app_read_attach(ctx, pr, onres, onrej);
    else {
        if (JS_IsException(onres)) JS_FreeValue(ctx, JS_GetException(ctx));
        if (JS_IsException(onrej)) JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, onres);
    JS_FreeValue(ctx, onrej);
}

/* The pump loop: call read(), process the count, repeat. Runs until a
   read's promise is PENDING (returns; the settle processes the value), a
   frame send parks on the io backend (returns; the sent callback
   resumes), or the pump ends. Inline send completions just continue the
   loop -- no recursion, no stack growth. */
static void dyn_app_stream_resume(dyn_app_conn_t *c)
{
    dyn_app_stream_t *st = c->st;
    JSContext *ctx = c->app->ctx;

    /* ONE pump loop and AT MOST ONE parked read per conn: a resume while a
       read is parked would skip that read's frame -- the settle that owns
       it processes its value, so a second entry here must be a no-op (this
       exact guard is what stopped every other async line vanishing). */
    if (!st || st->busy || !JS_IsUndefined(st->pending))
        return;
    dyn_app_stream_enter(st);
    st->busy = 1;
    while (!st->done && !c->closed) {
        JSValue pr, v;
        JSPromiseStateEnum ps;
        double n = -1;
        int keep;

        pr = JS_Call(ctx, st->readfn, st->src, 1, (JSValueConst *)&st->chunk);
        if (JS_IsException(pr)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;                      /* fail path below */
        }
        if (st->done || c->closed) {
            /* read() ran user JS that closed the conn (the abort deferred
               the free to this frame): the pump is over -- drop the read,
               with its reactions attached FIRST: a late rejection of an
               abandoned read must never surface as unhandled */
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;                      /* fail path below */
        }
        ps = JS_PromiseState(ctx, pr);
        if (ps == JS_PROMISE_REJECTED) {
            /* the contract: a rejecting read TRUNCATES the body (the fail
               path below). The dropped promise is marked handled first --
               its rejection is deliberately swallowed, never surfaced. */
            dyn_app_read_abandon(ctx, pr);
            JS_FreeValue(ctx, pr);
            break;                      /* fail path below */
        }
        if (ps == JS_PROMISE_PENDING) {
            /* park on the promise: OUR settle pair fires when the user's
               promise settles -- the engine's once-only reaction rule makes
               a misbehaving thenable harmless (the dyn_app_dispatch_rpc
               argument). funcs stay alive until the pair has been handed
               over. */
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
                /* no reaction could land -- a `then` that throws (or is
                   missing) leaves nobody to settle: reject OUR promise so
                   the settle still fires exactly once. The pair is
                   once-only, so a call made before the throw stays the
                   winner and this rejection is inert -- the swallow/
                   truncation contract, never a kill. */
                JSValue undef = JS_UNDEFINED;
                tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &undef);
                JS_FreeValue(ctx, tr);
            }
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            st->pending = pr;           /* owned until the settle/stop */
            st->busy = 0;
            goto out;                   /* the settle resumes us */
        }
        /* fulfilled inline: the count rides the promise result */
        v = JS_PromiseResult(ctx, pr);
        JS_FreeValue(ctx, pr);
        if (JS_IsNumber(v)) {
            if (JS_ToFloat64(ctx, &n, v) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, v);
                break;                  /* fail path below */
            }
        }
        JS_FreeValue(ctx, v);
        keep = dyn_app_stream_on_count(c, n);
        if (keep == 1)
            continue;                   /* inline send: keep pumping */
        if (keep == 0) {
            st->busy = 0;
            goto out;                   /* parked on the io backend */
        }
        break;                          /* keep == -1: fail path below */
    }
    st->busy = 0;
    dyn_app_stream_fail(c);
out:
    dyn_app_stream_leave(c);            /* MAY free: last touch of st/c */
}

/* Entry: the handler's result was a ByteSource (or {stream,...}); answer
   with a chunked pump. Returns 1 when the result WAS stream-shaped (the
   caller sends nothing further), 0 when it is an ordinary value. */
static int dyn_app_try_stream_result(dyn_app_conn_t *c, JSValueConst result)
{
    JSContext *ctx = c->app->ctx;
    JSValue src = JS_UNDEFINED;
    JSValue sv, ctv;
    const char *cts = NULL;
    int status = 200;
    dyn_app_stream_t *st;
    char head[512];
    int hn;

    /* MULTI-SAFE slot: exactly one stream pump per conn, and a live one is
       NEVER overwritten. The request pump holds while `c->st` is set (see
       dyn_app_process_loop), so a second stream cannot be dispatched while
       one is pumping; this guard is the belt-and-braces refusal for any
       path that ever slips that hold. The only protocol-safe answer while
       another response's chunked body is in flight is to truncate the
       connection -- every byte this response could write would land INSIDE
       that body -- so the slot is refused by closing, never by replacing. */
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
            cts = JS_ToCString(ctx, ctv);
        }
        JS_FreeValue(ctx, ctv);
    }
    if (c->closed) {
        JS_FreeValue(ctx, src);
        if (cts) JS_FreeCString(ctx, cts);
        return 1;
    }
    if (!c->http11) {
        JS_FreeValue(ctx, src);
        if (cts) JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500,
            "{\"error\":\"streaming bodies require an HTTP/1.1 peer\"}");
        return 1;
    }
    if (c->identity_refused) {
        JS_FreeValue(ctx, src);
        if (cts) JS_FreeCString(ctx, cts);
        /* streamed bodies are identity; a peer refusing it gets 406, the
           same last resort dyn_app_send_body has */
        c->identity_refused = 0;
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        return 1;
    }
    st = (dyn_app_stream_t *)calloc(1, sizeof *st);
    if (!st) {
        JS_FreeValue(ctx, src);
        if (cts) JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
        return 1;
    }
    st->status = status;
    st->ct = strdup(cts && *cts ? cts : "application/octet-stream");
    if (cts) JS_FreeCString(ctx, cts);
    st->frame_cap = DYN_APP_STREAM_CHUNK + 32;
    st->frame = (uint8_t *)malloc(st->frame_cap);
    st->pending = JS_UNDEFINED;
    if (st->frame) {
        JSValue dim = JS_NewInt64(ctx, DYN_APP_STREAM_CHUNK);
        st->chunk = JS_NewTypedArray(ctx, 1, &dim, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, dim);
        st->readfn = JS_GetPropertyStr(ctx, src, "read");
    }
    if (!st->frame || !st->ct || JS_IsException(st->chunk) ||
        !JS_IsFunction(ctx, st->readfn)) {
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
    st->src = src;                  /* the pump owns it now */
    c->st = st;
    c->refs++;                      /* the pump's claim on the conn */
    dyn_app_stream_enter(st);       /* the state is live: frame it */
    hn = snprintf(head, sizeof head,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                  "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
                  st->status, dyn_reason_phrase(st->status), st->ct,
                  c->close_after ? "close" : "keep-alive");
    if (hn <= 0 || (size_t)hn >= sizeof head) {
        dyn_app_conn_close(c);
        dyn_app_send_err(c, 500, "{\"error\":\"stream start failed\"}");
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);        /* MAY free */
        return 1;
    }
    st->in_send = 1;
    if (dyn_aio_send(c->app->aio, c->fd, (const uint8_t *)head,
                     (unsigned)hn, 0, dyn_app_stream_sent, c) < 0) {
        st->in_send = 0;
        /* the head never left: the conn is going away either way */
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);        /* MAY free */
        return 1;
    }
    /* The head's completion is dyn_app_stream_sent: a deferred one re-enters
       the pump itself; an INLINE one only cleared in_send (this frame is its
       driver), so the pump starts here. */
    if (!st->in_send && !st->retiring)
        dyn_app_stream_resume(c);
    dyn_app_stream_leave(c);            /* MAY free */
    return 1;
}

/* Send a JSON-RPC success {"jsonrpc":"2.0","result":<result>,"id":<id>}. */
static void dyn_app_send_rpc_result(dyn_app_conn_t *c, JSValueConst result,
                                    const char *id_s)
{
    JSContext *ctx = c->app->ctx;
    size_t out_len;
    /*a ByteSource result streams instead of JSON-stringifying (the
       id is deliberately not honored -- the response is the stream) */
    if (dyn_app_try_stream_result(c, result))
        return;
    /* JSON.stringify(undefined) returns the VALUE undefined, not a string, and
       converting that yields the text "undefined" -- not JSON, and rejected by
       every strict parser. A handler returning nothing serialises as null. */
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    int res_undef = !JS_IsException(jstr) && JS_IsUndefined(jstr);
    const char *out_s = (JS_IsException(jstr) || res_undef)
                        ? NULL : JS_ToCStringLen(ctx, &out_len, jstr);
    if (res_undef) { out_s = "null"; out_len = 4; }
    if (out_s) {
        dyn_iobuf_t o;
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(&o, out_s, out_len);
        dyn_iobuf_append(&o, ",\"id\":", 6);
        dyn_iobuf_append(&o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char *)o.data, o.len);
        dyn_iobuf_free(&o);
        if (!res_undef)          /* "null" above is a literal, not owned */
            JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr)) JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 500, -32603, "Internal error", id_s);
    }
    JS_FreeValue(ctx, jstr);
}

/* Send a JSON-RPC error from a thrown/rejected value. The text is read with
 * an explicit length: a thrown message may carry embedded NULs and the error
 * body must carry all of it, not stop at the first one. */
static void dyn_app_send_rpc_throw(dyn_app_conn_t *c, JSValueConst exc,
                                   const char *id_s)
{
    JSContext *ctx = c->app->ctx;
    size_t em_len = 0;
    const char *em = JS_ToCStringLen(ctx, &em_len, exc);
    dyn_app_rpc_error_n(c, 500, -32000, em ? em : "Server error",
                        em ? em_len : strlen("Server error"), id_s);
    if (em) JS_FreeCString(ctx, em);
}

/* One parked response's lifetime (a dynamic handler's or an async rpc
 * method's thenable). The struct is owned by a small JS object class whose
 * instances are carried in the settle closures' function-data slots: the
 * pending struct lives exactly as long as SOMEBODY can still settle it, and
 * the class finalizer releases it when the reactions die without settling
 * (so an abandoned handler costs nothing at exit). The accept_gzip/
 * identity_refused pair is captured at DISPATCH for rpc responses: the
 * conn's copy is per-request state that a pipelined later request overwrites,
 * and that response must be negotiated against ITS OWN Accept-Encoding. */
typedef struct {
    dyn_app_conn_t *conn;   /* one reference, dropped by the release below */
    char *id;               /* rpc only: the JSON id token (owned) */
    int accept_gzip;        /* rpc only: the request's own negotiation */
    int identity_refused;
    int counted;            /* holds one conn->resp_parked count */
} dyn_app_pend_t;

static JSClassID dyn_pend_class_id;

static void dyn_app_pump_resume(dyn_app_conn_t *c);

/* Release a pending response: free it, drop the pump's hold and the conn
 * ref. `abandoned` means the reactions died without settling -- the request
 * can then NEVER be answered, so the connection closes instead of leaving
 * the peer waiting and the pipeline framing desynced. Exactly one of the
 * settle reaction, the sync-throw fallback and the finalizer reaches this
 * (see dyn_app_pend_claim). */
static void dyn_app_pend_release(dyn_app_pend_t *pd, int abandoned)
{
    dyn_app_conn_t *c = pd->conn;
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

static void dyn_pend_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_app_pend_t *pd = (dyn_app_pend_t *)JS_GetOpaque(val, dyn_pend_class_id);
    (void)rt;
    if (pd) {
        JS_SetOpaque(val, NULL);
        dyn_app_pend_release(pd, 1);
    }
}

static const JSClassDef dyn_pend_class = {
    "PendingResponse", .finalizer = dyn_pend_finalizer,
};

/* The settle-once latch: claim hands the pending struct to EXACTLY ONE
 * caller -- the first settle reaction, or the sync-throw fallback when no
 * reaction ran. Every later claim gets NULL and does nothing at all (no
 * second response, no second free, no second conn unref). */
static dyn_app_pend_t *dyn_app_pend_claim(JSValueConst obj)
{
    dyn_app_pend_t *pd = (dyn_app_pend_t *)JS_GetOpaque(obj, dyn_pend_class_id);
    if (pd)
        JS_SetOpaque(obj, NULL);
    return pd;
}

/* Promise reaction for an async rpc handler: magic 0 = fulfilled, 1 = rejected.
 * Runs on the JS thread when the handler's promise settles. */
static JSValue dyn_app_rpc_settle(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    dyn_app_pend_t *pd;
    dyn_app_conn_t *c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd) /* already settled: the first reaction owns the response */
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        int saved_g = c->accept_gzip, saved_i = c->identity_refused;
        c->accept_gzip = pd->accept_gzip;
        c->identity_refused = pd->identity_refused;
        if (magic) dyn_app_send_rpc_throw(c, v, pd->id);
        else       dyn_app_send_rpc_result(c, v, pd->id);
        c->accept_gzip = saved_g;
        c->identity_refused = saved_i;
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

/* ---- JSON-RPC batch (array of requests) -- sync elements ------------- */

static void dyn_app_build_error(int code, const char *msg, const char *id_s,
                                dyn_iobuf_t *o)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_s);
    if (n > 0) dyn_iobuf_append(o, buf, n);
}
static void dyn_app_build_result(JSContext *ctx, JSValueConst result,
                                 const char *id_s, dyn_iobuf_t *o)
{
    size_t out_len;
    JSValue jstr = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    const char *out_s = JS_IsException(jstr) ? NULL : JS_ToCStringLen(ctx, &out_len, jstr);
    if (out_s) {
        dyn_iobuf_append(o, "{\"jsonrpc\":\"2.0\",\"result\":", 26);
        dyn_iobuf_append(o, out_s, out_len);
        dyn_iobuf_append(o, ",\"id\":", 6);
        dyn_iobuf_append(o, id_s ? id_s : "null", strlen(id_s ? id_s : "null"));
        dyn_iobuf_append(o, "}", 1);
        JS_FreeCString(ctx, out_s);
    } else {
        if (JS_IsException(jstr)) JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_build_error(-32603, "Internal error", id_s, o);
    }
    JS_FreeValue(ctx, jstr);
}

/* Process one batch element into `out`. Returns 1 if a response was written (0
 * for a notification, i.e. no `id`). Sync handlers only; an async result in a
 * batch yields an error (single requests keep full async support). */
static int dyn_app_rpc_build_one(dyn_app_conn_t *c, JSValueConst methods,
                                 JSValueConst elem, dyn_iobuf_t *out, int comma)
{
    JSContext *ctx = c->app->ctx;
    JSValue method_v = JS_GetPropertyStr(ctx, elem, "method");
    JSValue id = JS_GetPropertyStr(ctx, elem, "id");
    JSValue params = JS_GetPropertyStr(ctx, elem, "params");
    int has_id = !JS_IsUndefined(id);
    const char *method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    const char *id_s;
    int wrote = 0;
    dyn_iobuf_t tmp;
    dyn_iobuf_init(&tmp);
    { JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
      /* undefined stringifies to the VALUE undefined; its text is
         "undefined", not a JSON token. NULL makes the callers'
         `id_s ? id_s : "null"` fallback fire. */
      id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
             ? NULL : JS_ToCString(ctx, idj);
      JS_FreeValue(ctx, idj); }

    if (!method_s) {
        if (has_id) { dyn_app_build_error(-32600, "Invalid Request", id_s, &tmp); wrote = 1; }
    } else {
        JSValue fn = JS_GetPropertyStr(ctx, methods, method_s);
        if (!JS_IsFunction(ctx, fn)) {
            if (has_id) { dyn_app_build_error(-32601, "Method not found", id_s, &tmp); wrote = 1; }
        } else {
            JSValue res = JS_UNDEFINED;
            int arg_err = 0;    /* params rejected: no call happened, no res */
            /* JSON-RPC params may be a by-position array (spread, the
               WHATWG-Node convention), a by-name object (one argument), or
               omitted (zero arguments). Same dispatch as the single request
               path. */
            if (JS_IsUndefined(params)) {
                res = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
            } else if (JS_IsArray(ctx, params)) {
                JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
                uint32_t len = 0, i;
                JSValueConst *argv = NULL;
                if (JS_ToUint32(ctx, &len, lenv)) {
                    JS_FreeValue(ctx, lenv);
                    dyn_app_rpc_error(c, 400, -32600, "Invalid Request",
                                      "unreadable params length");
                    return 1;   /* replied on the wire; handled */
                }
                JS_FreeValue(ctx, lenv);
                if (len > DYN_APP_MAX_PARAMS) {
                    /* a notification must not produce a response at all */
                    if (has_id) { dyn_app_build_error(-32602, "Invalid params", id_s, &tmp); wrote = 1; }
                    arg_err = 1;
                } else if (len > 0) {
                    argv = (JSValueConst *)malloc(len * sizeof(*argv));
                    if (!argv) {
                        if (has_id) { dyn_app_build_error(-32603, "Internal error", id_s, &tmp); wrote = 1; }
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
            } else {
                JSValueConst a[1] = { params };
                res = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
            }
            if (!arg_err && JS_IsException(res)) {
                JSValue exc = JS_GetException(ctx);
                if (has_id) { const char *em = JS_ToCString(ctx, exc);
                    dyn_app_build_error(-32000, em ? em : "Server error", id_s, &tmp);
                    if (em) JS_FreeCString(ctx, em); wrote = 1; }
                JS_FreeValue(ctx, exc);
            } else if (!arg_err) {
                int thenable = 0;
                if (JS_IsObject(res)) {
                    JSValue th = JS_GetPropertyStr(ctx, res, "then");
                    thenable = JS_IsFunction(ctx, th);
                    JS_FreeValue(ctx, th);
                }
                if (thenable) {
                    if (has_id) { dyn_app_build_error(-32000, "async handler not allowed in batch", id_s, &tmp); wrote = 1; }
                } else if (has_id && dyn_value_is_byte_source(ctx, res)) {
                    /*a batch is ONE http response; a stream cannot
                       share it with the other elements' JSON bodies */
                    dyn_app_build_error(-32603,
                        "streaming body not allowed in a batch", id_s, &tmp);
                    wrote = 1;
                } else if (has_id) {
                    dyn_app_build_result(ctx, res, id_s, &tmp); wrote = 1;
                }
                JS_FreeValue(ctx, res);
            }
        }
        JS_FreeValue(ctx, fn);
    }
    if (wrote) {
        if (comma) dyn_iobuf_append(out, ",", 1);
        dyn_iobuf_append(out, tmp.data, tmp.len);
    }
    dyn_iobuf_free(&tmp);
    if (method_s) JS_FreeCString(ctx, method_s);
    if (id_s) JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    return wrote;
}

static void dyn_app_dispatch_batch(dyn_app_conn_t *c, JSValueConst methods,
                                   JSValueConst arr)
{
    JSContext *ctx = c->app->ctx;
    uint32_t len = 0, i;
    int nresp = 0;
    dyn_iobuf_t out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "unreadable length");
        return;
    }
    JS_FreeValue(ctx, lv);
    if (len == 0) { dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null"); return; }
    /* Handlers run ON the JS thread (the single-thread law), so a batch is a
       synchronous span during which nothing else -- including keep-alive pings
       on other connections -- is served. An unbounded array length is therefore
       an unbounded stall bought with one request. */
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
        dyn_app_send_json(c, 200, (const char *)out.data, out.len);
    else
        dyn_app_send_json(c, 200, "", 0); /* all notifications: no content */
    dyn_iobuf_free(&out);
}

/* Dispatch one complete JSON-RPC request body against a methods object. All JS
 * runs on this (the JS) thread. Sends the response on `c` (sync or, if the
 * handler returns a thenable, when the promise settles). */
static void dyn_app_dispatch_rpc(dyn_app_conn_t *c, JSValueConst methods,
                                 const char *body, size_t body_len)
{
    JSContext *ctx = c->app->ctx;
    JSValue req, method_v, params, id, fn, result;
    const char *method_s, *id_s;

    /* The JSON parser reads body[body_len] as a sentinel; the body is a slice of
     * our own mutable buffer, so NUL-terminate it in place (on_recv guarantees
     * the slot is writable) and restore the byte after. */
    { char *m = (char *)body;
      char save = m[body_len];
      m[body_len] = 0;
      req = JS_ParseJSON(ctx, body, body_len, "<rpc>");
      m[body_len] = save; }
    if (JS_IsException(req)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 400, -32700, "Parse error", "null");
        return;
    }
    if (JS_IsArray(ctx, req)) { /* JSON-RPC batch */
        dyn_app_dispatch_batch(c, methods, req);
        JS_FreeValue(ctx, req);
        return;
    }
    method_v = JS_GetPropertyStr(ctx, req, "method");
    id = JS_GetPropertyStr(ctx, req, "id");
    params = JS_GetPropertyStr(ctx, req, "params");
    /* serialize id back to a JSON token for the response */
    { JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
      /* undefined stringifies to the VALUE undefined; its text is
         "undefined", not a JSON token. NULL makes the callers'
         `id_s ? id_s : "null"` fallback fire. */
      id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
             ? NULL : JS_ToCString(ctx, idj);
      JS_FreeValue(ctx, idj); }

    /* A request with NO id member is a notification: it never gets a
       response, whatever the outcome -- the rule the batch path already
       applies via has_id. (An explicit "id": null is answered with null.) */
    if (JS_IsUndefined(id)) {
        goto cleanup;
    }

    method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    if (!method_s) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", id_s);
        goto cleanup;
    }
    fn = JS_GetPropertyStr(ctx, methods, method_s);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        dyn_app_rpc_error(c, 404, -32601, "Method not found", id_s);
        JS_FreeCString(ctx, method_s);
        goto cleanup;
    }
    /* call handler(params). JSON-RPC params may be a by-position array
     * (spread, WHATWG-Node convention) or a by-name object (single
     * argument); omitted params call with zero arguments. */
    if (JS_IsUndefined(params)) {
        result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    } else if (JS_IsArray(ctx, params)) {
        JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
        uint32_t len = 0, i;
        /* Stack slots for the common arity: a heap argv per request was a
           malloc/free pair on the hottest RPC shape (ping/sum-style calls
           with a handful of positional params). DYN_APP_MAX_PARAMS is the
           protocol ceiling; the stack array covers ordinary calls and the
           heap path stays for the declared-maximum case. */
        JSValueConst argv_stack[8];
        JSValueConst *argv = NULL;
        if (JS_ToUint32(ctx, &len, lenv)) {
            JS_FreeValue(ctx, lenv);
            dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "unreadable length");
            return;
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
                argv = (JSValueConst *)malloc(len * sizeof(*argv));
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
        goto cleanup; /* result is JS_EXCEPTION: nothing to free */
    }
    /* async: handler returned a thenable -- settle the response later.
     * The user's `then` drives an ENGINE-owned resolve/reject pair, whose
     * once-only settlement rule makes a thenable that calls its callback
     * twice (or after throwing) harmless: our settle runs as a reaction of
     * OUR promise, exactly once, instead of being invoked directly by user
     * code. On top of that the pending struct is carried by a JS object the
     * reactions hold (dyn_pend_class), so its release is settled-once (the
     * dyn_app_pend_claim latch) and a promise that dies without settling is
     * reclaimed by the finalizer instead of leaking the struct and the
     * connection it holds. */
    if (JS_IsObject(result)) {
        JSValue then = JS_GetPropertyStr(ctx, result, "then");
        if (JS_IsFunction(ctx, then)) {
            dyn_app_pend_t *pd = (dyn_app_pend_t *)calloc(1, sizeof(*pd));
            if (pd) {
                /* Zeroed: the capability-creation failure path frees these
                   before JS_NewPromiseCapability has written them. */
                JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
                JSValue promise, pthen, pobj, onres, onrej, tr;
                JSValueConst thenargs[2], settleargs[2];

                pd->conn = c;
                c->refs++; /* keep the connection alive until it settles */
                pd->id = strdup(id_s ? id_s : "null");
                pd->accept_gzip = c->accept_gzip;
                pd->identity_refused = c->identity_refused;
                pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
                if (JS_IsException(pobj)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_pend_release(pd, 0); /* never parked: free + unref */
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_SetOpaque(pobj, pd);
                promise = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(promise) || !pd->id) {
                    if (!JS_IsException(promise)) JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, pobj); /* finalizer frees pd */
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                /* Attach our settle BEFORE the user's then can run: a then
                   that resolves and then throws must still settle exactly
                   once, from the reaction queue. */
                onres = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 0, 1, &pobj);
                onrej = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 1, 1, &pobj);
                pthen = JS_GetPropertyStr(ctx, promise, "then");
                settleargs[0] = onres;
                settleargs[1] = onrej;
                tr = JS_IsException(pthen) ? pthen
                    : JS_Call(ctx, pthen, promise, 2, settleargs);
                if (JS_IsException(tr)) { /* cannot happen for built-in then */
                    JS_FreeValue(ctx, tr);
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, pthen);
                    JS_FreeValue(ctx, onres);
                    JS_FreeValue(ctx, onrej);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, pobj); /* finalizer frees pd */
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_FreeValue(ctx, tr);
                JS_FreeValue(ctx, pthen);
                JS_FreeValue(ctx, onres);
                JS_FreeValue(ctx, onrej);
                pd->counted = 1;   /* the pump holds until this settles */
                c->resp_parked++;
                thenargs[0] = funcs[0];
                thenargs[1] = funcs[1];
                tr = JS_Call(ctx, then, result, 2, thenargs);
                if (JS_IsException(tr)) {
                    /* A throwing `then` leaves nobody to settle: reject OUR
                       promise. Ignored if the thenable already resolved, so
                       the response still fires exactly once. */
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
                goto cleanup; /* response is sent when the promise settles */
            }
        }
        JS_FreeValue(ctx, then);
    }
    /* sync result */
    dyn_app_send_rpc_result(c, result, id_s);
    JS_FreeValue(ctx, result);
cleanup:
    if (id_s) JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, req);
}

static const char *dyn_app_content_type(const char *path)
{
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm")) return "text/html";
    if (!strcasecmp(d, ".js") || !strcasecmp(d, ".mjs")) return "text/javascript";
    if (!strcasecmp(d, ".css")) return "text/css";
    if (!strcasecmp(d, ".json")) return "application/json";
    if (!strcasecmp(d, ".txt")) return "text/plain";
    if (!strcasecmp(d, ".csv")) return "text/csv";
    if (!strcasecmp(d, ".png")) return "image/png";
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(d, ".gif")) return "image/gif";
    if (!strcasecmp(d, ".svg")) return "image/svg+xml";
    if (!strcasecmp(d, ".ico")) return "image/x-icon";
    if (!strcasecmp(d, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

/* File-type filter: 1 if `path` passes the route's allow-list (by ".ext" or
 * mime), or the route has no filter. */
static int dyn_app_allowed(const dyn_app_route_t *rt, const char *path)
{
    size_t k;
    const char *dot, *ct;
    if (!rt->allow) return 1;
    dot = strrchr(path, '.');
    ct = dyn_app_content_type(path);
    for (k = 0; k < rt->n_allow; k++) {
        if (dot && !strcasecmp(rt->allow[k], dot)) return 1;
        if (!strcmp(rt->allow[k], ct)) return 1;
    }
    return 0;
}

/* Send a small plain-text error as JSON. */
static void dyn_app_send_err(dyn_app_conn_t *c, int status, const char *msg)
{
    dyn_app_send_json(c, status, msg, strlen(msg));
}

/* The same error with an Allow header (RFC 9110 15.5.6 makes it a MUST on
 * 405: a method-not-allowed answer must say what the path DOES answer). */
static void dyn_app_send_err_allow(dyn_app_conn_t *c, int status,
                                   const char *msg, const char *allow)
{
    char extra[256];
    size_t al = strlen(allow);
    size_t ml = strlen(msg);
    int n;
    if (al > 200) al = 200; /* bounded header line; the list is registration data */
    n = snprintf(extra, sizeof(extra), "Allow: %.*s\r\n", (int)al, allow);
    if (n < 0 || (size_t)n >= sizeof(extra))
        dyn_app_send_json(c, status, msg, ml);
    else
        dyn_app_send_body_x(c, status, "application/json", msg, ml, extra);
}

/* Serve a file from a static route (blocking read + send; sendfile/async-disk
 * is the optimization). Size-capped, type-filtered, and CONTAINED: the
 * resolved file must sit under the resolved root, which is what stops a
 * symlink escaping -- the `..` substring check alone did not. The fd is
 * opened with O_NOFOLLOW and fstat'd, so the size and regularity come from
 * the SAME object that is sent: a stat/open race or a final-component symlink
 * swap cannot serve bytes other than the ones measured. */
static void dyn_app_serve_static(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                 const char *reqpath,
                                 const char *base, size_t head_len)
{
    const char *sub = reqpath + strlen(rt->path);
    char fpath[2048];
    struct stat st;
    int64_t cap;
    dyn_iobuf_t out;
    char head[512];
    int n, ffd;
    int head_only;
    int64_t rstart = -1, rend = -1;   /* single Range; -1 = no/ignored range */
    int range_unsat = 0;

    head_only = (head_len >= 5 && memcmp(base, "HEAD ", 5) == 0);

    if (strstr(sub, "..")) { /* reject traversal outright */
        dyn_app_send_err(c, 403, "{\"error\":\"forbidden\"}");
        return;
    }
    while (*sub == '/') sub++;
    snprintf(fpath, sizeof(fpath), "%s/%s", rt->dir, *sub ? sub : "index.html");

    /* CONTAINMENT, because the `..` check above is a SUBSTRING test and cannot
       see a SYMLINK: a link inside the served directory pointing outside it
       contains no dots at all. Resolve BOTH sides and require the file to
       still be underneath. */
    {
        char rdir[PATH_MAX], rfile[PATH_MAX];
        size_t dl;
        if (!realpath(rt->dir, rdir) || !realpath(fpath, rfile)) {
            dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
            return;
        }
        dl = strlen(rdir);
        /* The separator check is not optional: a plain prefix test lets
           /srv/wwwEVIL through for a root of /srv/www. */
        if (strncmp(rfile, rdir, dl) != 0 ||
            (rfile[dl] != '/' && rfile[dl] != '\0')) {
            dyn_app_send_err(c, 403, "{\"error\":\"forbidden\"}");
            return;
        }
    }
    /* O_NOFOLLOW closes the gap between realpath above and open: a symlink
       swapped into the final component after containment was checked fails
       here instead of being followed. */
    ffd = open(fpath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
    /* Single-range support. Multi-range requests fall back to a full 200 --
       merging parts into multipart/byteranges buys complexity no consumer
       here has asked for. An unparseable or unsatisfiable single range is a
       416 with the RFC 9110 failure form. */
    {
        size_t rv_len = 0;
        const char *rv = dyn_req_header(base, head_len, "range", &rv_len);
        if (rv && rv_len > 6 && strncasecmp(rv, "bytes=", 6) == 0 &&
            !memchr(rv, ',', rv_len)) {
            size_t b = 6, e = rv_len;
            int64_t S = -1, E = -1;
            while (b < e && (rv[b] == ' ' || rv[b] == '\t')) b++;
            while (e > b && (rv[e - 1] == ' ' || rv[e - 1] == '\t')) e--;
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
        close(ffd);   /* nothing to send from the file */
        if (n > 0) {
            dyn_iobuf_init(&out);
            dyn_iobuf_append(&out, head, (size_t)n);
            if (c->close_after) {
                c->refs++;
                if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                                 dyn_app_close_after_send, c) < 0)
                    dyn_app_close_after_send(NULL, -ECONNRESET, NULL, 0, c);
            } else {
                dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0, NULL, NULL);
            }
            dyn_iobuf_free(&out);
            dyn_app_met_response(c, 0);
        }
        return;
    }
    /* zero-copy: send the header, then sendfile the body straight from the page
     * cache (SIGBUS-safe, unlike mmap; the kernel handles a truncation). */
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
        /* The close callback may ride the HEADER send only when no body
           phase follows (HEAD): otherwise it fires before sendfile runs and
           truncates the response to its headers -- measured as an empty
           200 by test_http_params before this move. */
        if (c->close_after && head_only) {
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0,
                             dyn_app_close_after_send, c) < 0)
                dyn_app_close_after_send(NULL, -ECONNRESET, NULL, 0, c);
        } else {
            dyn_aio_send(c->app->aio, c->fd, out.data, out.len, 0, NULL, NULL);
        }
    }
    dyn_iobuf_free(&out);
    if (head_only) {
        /* HEAD advertises the length but sends no bytes -- and therefore no
           sendfile: the fd dies here instead of being owned by a transfer. */
        close(ffd);
        dyn_app_met_response(c, 0);
        return;
    }
    /* dyn_aio_sendfile owns ffd and closes it on completion or connection
     * close. With close_after the close rides the TRANSFER's completion --
     * the only callback that fires after the last body byte. The completion
     * releases "the ref the caller held for udata" (dyn_app_close_after_send),
     * and a small file completes INLINE inside this pump frame -- where only
     * the pump's and the recv callback's refs keep the conn alive -- so the
     * udata ref is taken HERE, exactly like every dyn_aio_send close_after
     * site above. Omitting it freed the conn under dyn_app_process and the
     * recv callback then read it (ASan: heap-use-after-free in dyn_app_on_recv,
     * reproduced by tests/test_http_hardening.js's Connection: close static
     * row; native builds read the freed memory silently). A REFUSED transfer
     * (EBUSY) never fires the callback, so its ref comes back. */
    if (c->close_after)
        c->refs++;
    {
        int rc;
        if (rstart >= 0)
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, (off_t)rstart,
                                  (size_t)(rend - rstart + 1),
                                  c->close_after ? dyn_app_close_after_send : NULL,
                                  c->close_after ? (void *)c : NULL);
        else
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, 0, (size_t)st.st_size,
                                  c->close_after ? dyn_app_close_after_send : NULL,
                                  c->close_after ? (void *)c : NULL);
        if (rc < 0 && c->close_after)
            dyn_app_conn_unref(c);
    }
    /* the zero-copy path bypasses dyn_app_send_body, so it reports itself */
    dyn_app_met_response(c, (double)(rstart >= 0 ? rend - rstart + 1
                                                 : st.st_size));
}

/* File upload: streamed straight to disk as the body arrives (no whole-body
 * buffering), so uploads are bounded by maxFileSize, not the request buffer. */
static unsigned long dyn_app_upload_seq;

/* Body fully written: close, invoke the handler with the saved path + metadata,
 * respond, and return the connection to HTTP mode. */
static void dyn_app_upload_finish(dyn_app_conn_t *c)
{
    dyn_app_upload_t *u = c->up;
    JSContext *ctx = c->app->ctx;
    JSValue pathv, meta, r;
    JSValueConst args[2];
    c->up = NULL; /* detach before running JS (handler must not re-enter it) */
    close(u->fd);
    pathv = JS_NewString(ctx, u->path);
    meta = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, meta, "size", JS_NewInt64(ctx, u->size));
    JS_SetPropertyStr(ctx, meta, "contentType", JS_NewString(ctx, u->ctype));
    args[0] = pathv; args[1] = meta;
    r = JS_Call(ctx, u->handler, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"handler failed\"}");
    } else {
        dyn_iobuf_t o;
        JSValue pj = JS_JSONStringify(ctx, pathv, JS_UNDEFINED, JS_UNDEFINED);
        const char *ps = JS_IsException(pj) ? NULL : JS_ToCString(ctx, pj);
        dyn_iobuf_init(&o);
        dyn_iobuf_append(&o, "{\"ok\":true,\"path\":", 18);
        dyn_iobuf_append(&o, ps ? ps : "\"\"", strlen(ps ? ps : "\"\""));
        dyn_iobuf_append(&o, "}", 1);
        dyn_app_send_json(c, 200, (const char *)o.data, o.len);
        dyn_iobuf_free(&o);
        if (ps) JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, pj);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, pathv);
    JS_FreeValue(ctx, meta);
    JS_FreeValue(ctx, u->handler);
    free(u->path);
    free(u);
}

/* ---- the async drain ------------------------------------------------- *
 *
 * The body was drained with a blocking write(2) loop ON THE REACTOR THREAD:
 * a peer with a fast link and a slow disk made the whole server stall inside
 * one upload. The drain now rides the aio disk pool the async file pair uses:
 * dyn_app_upload_resume copies the next chunk out of c->in and hands it to
 * dyn_aio_write; dyn_app_upload_wdone re-drives the machine from the
 * completion. The JS contract is untouched -- same handler, same meta, same
 * response -- only the blocking writes went async.
 *
 * BOUNDS, both named. CPU per event: one bounded chunk copy + one submit per
 * arrival, never a syscall wait -- the peer still cannot buy unbounded work
 * with bytes alone. MEMORY: c->in may now hold up to the DECLARED body size
 * while writes are in flight (the peer can deliver that fast), so it is
 * bounded by the route's maxFileSize -- the same cap that bounds the file --
 * never by an unbounded queue. Each in-flight copy is capped per op: */
#define DYN_APP_UPLOAD_CHUNK (256u * 1024u)  /* one in-flight write */
#define DYN_APP_UPLOAD_MAXW  4               /* writes in flight per upload */

/* One in-flight chunk. The bytes are COPIED, not borrowed: c->in is compacted
 * and reallocated as more of the body arrives, while the worker's pwrite
 * reads the buffer long after this turn. dyn_aio_write ALWAYS completes
 * (inline when the pool is unavailable), so every op reaches wdone exactly
 * once and frees itself there. */
typedef struct dyn_app_upload_w {
    dyn_app_upload_t *u;   /* parent; valid: ops pin the conn, which owns u */
    dyn_app_conn_t *c;     /* conn, pinned by the op's ref until completion */
    uint8_t *buf;          /* chunk copy (advances over a short write) */
    int64_t off;           /* absolute file offset of buf[0] */
    size_t len;            /* bytes of this op still to write */
} dyn_app_upload_w_t;

static void dyn_app_upload_wdone(dyn_aio_t *aio, int res, const uint8_t *buf,
                                 unsigned len, void *ud);

/* The machine's driver: called when body bytes arrive and when a write
 * completes. Submits buffered chunks to the disk pool, finishes the upload
 * when the declared body is fully on disk, and closes the connection on the
 * first failed write (the old EPIPE path, unchanged in effect). */
static void dyn_app_upload_resume(dyn_app_conn_t *c)
{
    dyn_app_upload_t *u = c->up;
    struct dyn_aio *aio = c->app->aio;

    if (u->err) {              /* a write already failed: drop the conn */
        dyn_app_conn_close(c);
        return;
    }
    while (u->remaining > 0 && u->wbusy < DYN_APP_UPLOAD_MAXW) {
        size_t take = dyn_iobuf_rlen(&c->in);
        dyn_app_upload_w_t *w;
        if (take == 0)
            return;                        /* need more bytes from the peer */
        if ((int64_t)take > u->remaining) take = (size_t)u->remaining;
        if (take > DYN_APP_UPLOAD_CHUNK) take = DYN_APP_UPLOAD_CHUNK;
        w = (dyn_app_upload_w_t *)malloc(sizeof(*w));
        if (w) w->buf = (uint8_t *)malloc(take);
        if (!w || !w->buf) {
            if (w) free(w->buf);
            free(w);
            u->err = -ENOMEM;
            dyn_app_conn_close(c);
            return;
        }
        memcpy(w->buf, dyn_iobuf_rdata(&c->in), take);
        w->u = u;
        w->c = c;
        w->off = u->off;
        w->len = take;
        u->off += take;
        u->remaining -= take;
        dyn_iobuf_consume(&c->in, take);
        u->wbusy++;
        c->refs++;             /* one ref per in-flight async op: wdone unrefs
                                  it, so the conn outlives the write */
        dyn_aio_write(aio, u->fd, w->buf, take, w->off,
                      dyn_app_upload_wdone, w);
        if (!c->up || c->closed)
            return;     /* an INLINE completion finished or closed us: `u`
                           may already be freed, touch nothing of it */
    }
    if (u->remaining == 0 && u->wbusy == 0)
        dyn_app_upload_finish(c);          /* whole declared body on disk */
}

/* LOOP THREAD (a pool completion). Advance the machine by one finished op. */
static void dyn_app_upload_wdone(dyn_aio_t *aio, int res, const uint8_t *buf,
                                 unsigned len, void *ud)
{
    dyn_app_upload_w_t *w = (dyn_app_upload_w_t *)ud;
    dyn_app_upload_t *u = w->u;
    dyn_app_conn_t *c = w->c;
    (void)aio; (void)buf; (void)len;

    if (res == -EINTR) {           /* the old inline loop retried these */
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                          dyn_app_upload_wdone, w) == 0)
            return;
        res = -EIO;
    }
    if (res > 0 && (size_t)res < w->len) {
        /* Short pwrite on a regular file: retry the tail at the exact
           offset. The op stays in flight and keeps its conn ref. */
        w->buf += res;
        w->off += res;
        w->len -= (size_t)res;
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                          dyn_app_upload_wdone, w) == 0)
            return;
        res = -EIO;                /* submit refused (OOM): fail the upload */
    }
    if (res == 0)
        res = -EIO;                /* a regular-file write advanced nothing */
    free(w->buf);
    free(w);
    u->wbusy--;
    if (res < 0 && !u->err)
        u->err = res;
    if (c->closed) {
        /* Peer gone mid-drain: settle only. The partial file is unlinked by
           the conn's own unref cleanup when the last op ref lands. */
        dyn_app_conn_unref(c);
        return;
    }
    dyn_app_upload_resume(c);      /* finish, or submit what arrived meanwhile */
    dyn_app_conn_unref(c);         /* drop the op's ref LAST: resume ran JS */
}

/* Bytes arrived for an in-progress upload (or the upload just started):
 * push whatever is buffered toward the disk pool. */
static void dyn_app_upload_drain(dyn_app_conn_t *c)
{
    dyn_app_upload_resume(c);
}

/* Begin streaming an upload: validate size/type, open the dest file, consume the
 * header, write any buffered body, and switch the connection to upload mode. */
/* ---- App .proxy -- the L7 (HTTP) reverse proxy ----------------------- *
 *
 * L4 forwards bytes; this one must NOT. Every header sent upstream is
 * RE-SERIALISED from this server's own parse, never copied from the client's
 * bytes. That is the structural defence against header smuggling (CWE-444):
 * if the proxy forwards raw bytes, the proxy and the backend parse the same
 * octets with two parsers and any disagreement between them is an exploit.
 * The request line, the header names and the framing are all re-emitted here.
 *
 * BOUNDED, NOT STREAMING. Request and response bodies are buffered under the
 * App's existing 1 MiB cap, and upstream connections are not pooled. Both are
 * named in the module header rather than implied.
 */

#define DYN_PROXY_HEAD_MAX  8192      /* re-serialised head; 431 past it */

/* Hop-by-hop (RFC 9110 7.6.1): meaningful to one connection only, so
 * forwarding them lets a client steer the proxy-to-backend connection. */
static int dyn_proxy_hop_by_hop(const char *n, size_t len)
{
    static const char *const hop[] = {
        "connection", "keep-alive", "te", "trailer", "transfer-encoding",
        "upgrade", "proxy-authenticate", "proxy-authorization",
        /* We emit our own framing and forwarding headers, so a client-supplied
           copy must never survive: X-Forwarded-For is appended to, not trusted. */
        "content-length", "host", "x-forwarded-for", "x-forwarded-proto",
        "x-forwarded-host", "x-real-ip", "via",
    };
    size_t i;
    for (i = 0; i < countof(hop); i++)
        if (strlen(hop[i]) == len && dyn_ci_eq(n, hop[i], len))
            return 1;
    return 0;
}

typedef struct {
    dyn_app_conn_t *c;       /* client side; holds a ref */
    int fd;                  /* upstream */
    uint8_t *req;            /* re-serialised request, owned */
    size_t req_len;
    dyn_iobuf_t resp;        /* upstream response as it arrives */
    int sent;
} dyn_app_proxy_t;

static void dyn_proxy_fail(dyn_app_proxy_t *p, int status, const char *msg)
{
    dyn_app_conn_t *c = p->c;
    if (p->fd >= 0) { int fd = p->fd; p->fd = -1; dyn_aio_close(c->app->aio, fd); }
    if (!p->sent) { p->sent = 1; dyn_app_send_err(c, status, msg); }
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_unref(c);
    free(p);
}

/* Copy the upstream response through, minus its hop-by-hop headers. The status
 * line and each retained header are re-emitted, for the same reason as the
 * request: two parsers that disagree about the response is response splitting. */
static void dyn_proxy_relay_response(dyn_app_proxy_t *p)
{
    dyn_app_conn_t *c = p->c;
    const char *buf = (const char *)p->resp.data;
    size_t len = p->resp.len, i, head_len = 0, eol;
    char out[DYN_PROXY_HEAD_MAX];
    size_t n = 0;

    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            { head_len = i + 4; break; }
    if (!head_len)
        return;                               /* head still arriving */

    for (eol = 0; eol + 1 < head_len; eol++)  /* status line, verbatim shape */
        if (buf[eol] == '\r' && buf[eol+1] == '\n')
            break;
    if (eol + 1 >= head_len || eol < 12 || memcmp(buf, "HTTP/1.", 7) != 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"bad upstream response\"}");
        return;
    }
    if (eol + 2 > sizeof(out)) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream status line too long\"}");
        return;
    }
    memcpy(out, buf, eol);
    n = eol;
    n += (size_t)snprintf(out + n, sizeof(out) - n, "\r\n");

    {   /* walk the response headers and drop the hop-by-hop ones */
        size_t ls = eol + 2;
        while (ls + 1 < head_len) {
            size_t le = ls, colon;
            while (le + 1 < head_len && !(buf[le] == '\r' && buf[le+1] == '\n'))
                le++;
            if (le == ls)
                break;
            for (colon = ls; colon < le && buf[colon] != ':'; colon++)
                ;
            if (colon < le && !dyn_proxy_hop_by_hop(buf + ls, colon - ls)) {
                if (n + (le - ls) + 2 >= sizeof(out)) {
                    dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
                    return;
                }
                memcpy(out + n, buf + ls, le - ls);
                n += le - ls;
                out[n++] = '\r'; out[n++] = '\n';
            }
            ls = le + 2;
        }
    }
    {   /* Our own framing: the body length we are actually going to write. */
        size_t body = len - head_len;
        int k = snprintf(out + n, sizeof(out) - n,
                         "Content-Length: %llu\r\nConnection: close\r\n\r\n",
                         (unsigned long long)body);
        if (k < 0 || n + (size_t)k >= sizeof(out)) {
            dyn_proxy_fail(p, 502, "{\"error\":\"upstream headers too large\"}");
            return;
        }
        n += (size_t)k;
        p->sent = 1;
        dyn_aio_send(c->app->aio, c->fd, out, n, 0, NULL, NULL);
        if (body)
            dyn_aio_send(c->app->aio, c->fd, buf + head_len, body, 0, NULL, NULL);
    }
    { int fd = p->fd; p->fd = -1; if (fd >= 0) dyn_aio_close(c->app->aio, fd); }
    dyn_iobuf_free(&p->resp);
    free(p->req);
    dyn_app_conn_unref(c);
    dyn_app_conn_close(p->c);
    free(p);
}

/* Is the whole response present? Head plus its declared Content-Length. A
 * response with neither Content-Length nor chunked framing is close-delimited,
 * and only then does EOF decide. Chunked upstreams are refused, not guessed. */
static int dyn_proxy_response_complete(dyn_app_proxy_t *p)
{
    const char *buf = (const char *)p->resp.data;
    size_t len = p->resp.len, i, head_len = 0, vlen = 0;
    const char *cl;
    int64_t clen = 0;

    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            { head_len = i + 4; break; }
    if (!head_len)
        return 0;
    cl = dyn_req_header(buf, head_len, "content-length", &vlen);
    if (!cl || vlen == 0)
        return 0;                       /* no length: wait for EOF */
    for (i = 0; i < vlen; i++) {
        if (cl[i] < '0' || cl[i] > '9')
            return 0;
        clen = clen * 10 + (cl[i] - '0');
        if (clen > DYN_ACONN_MAX_REQ)
            return 1;                   /* over the cap; relay_response refuses */
    }
    return len >= head_len + (size_t)clen;
}

static void dyn_proxy_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *udata)
{
    dyn_app_proxy_t *p = (dyn_app_proxy_t *)udata;
    (void)aio;

    if (p->fd < 0)
        return;
    if (res < 0) { dyn_proxy_fail(p, 502, "{\"error\":\"upstream read failed\"}"); return; }
    if (res == 0) {                            /* upstream finished */
        dyn_proxy_relay_response(p);
        if (p->fd >= 0)                        /* head never completed */
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
    /* Relay on the response's OWN framing, never on EOF: a keep-alive upstream
       never closes, and waiting for it would hang every request. */
    if (dyn_proxy_response_complete(p))
        dyn_proxy_relay_response(p);
}

static void dyn_proxy_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                                 unsigned len, void *udata)
{
    dyn_app_proxy_t *p = (dyn_app_proxy_t *)udata;
    (void)aio; (void)buf; (void)len;

    if (res < 0 || p->fd < 0) {
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
        return;
    }
    if (dyn_aio_send(p->c->app->aio, p->fd, p->req, p->req_len, 0, NULL, NULL) < 0 ||
        dyn_aio_recv(p->c->app->aio, p->fd, 0, 1, dyn_proxy_on_recv, p) < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream write failed\"}");
}

/* Re-serialise the request and start the upstream leg. */
static void dyn_app_proxy_start(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                const char *base, size_t head_len,
                                const char *path, int64_t clen)
{
    dyn_app_proxy_t *p;
    char head[DYN_PROXY_HEAD_MAX];
    size_t n = 0, ls, sp1 = 0, i;
    const char *sub;
    int k;

    for (i = 0; i < head_len && base[i] != ' '; i++)
        ;
    sp1 = i;
    if (sp1 == 0 || sp1 > 24) {
        dyn_app_send_err(c, 400, "{\"error\":\"bad method\"}");
        dyn_app_conn_close(c);
        return;
    }
    sub = path + strlen(rt->path);
    if (!*sub)
        sub = "/";

    /* Request line and Host re-emitted by us: the client's bytes are data. */
    k = snprintf(head, sizeof(head), "%.*s %s HTTP/1.1\r\nHost: %s:%u\r\n",
                 (int)sp1, base, sub, rt->up_host, (unsigned)rt->up_port);
    if (k < 0 || (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n = (size_t)k;

    for (ls = 0; ls + 1 < head_len; ) {        /* skip the request line */
        if (base[ls] == '\r' && base[ls+1] == '\n') { ls += 2; break; }
        ls++;
    }
    while (ls + 1 < head_len) {
        size_t le = ls, colon;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le+1] == '\n'))
            le++;
        if (le == ls)
            break;
        for (colon = ls; colon < le && base[colon] != ':'; colon++)
            ;
        /* The name is re-emitted verbatim into the upstream request, so it
           must be an RFC 9110 token -- the same DYN_TCHAR rule the request
           line applies to the method. The head validation above only bars
           CTLs and SP inside the name, so "X(Name)" survived to here and was
           forwarded as header text no conforming parser agrees on. */
        if (colon < le) {
            size_t ni;
            int token = colon > ls;
            for (ni = ls; token && ni < colon; ni++)
                if (!DYN_TCHAR[(unsigned char)base[ni]])
                    token = 0;
            if (!token) {
                dyn_app_send_err(c, 400, "{\"error\":\"bad header name\"}");
                dyn_app_conn_close(c);
                return;
            }
        }
        if (colon < le && !dyn_proxy_hop_by_hop(base + ls, colon - ls)) {
            if (n + (le - ls) + 2 >= sizeof(head)) {
                dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
                dyn_app_conn_close(c);
                return;
            }
            memcpy(head + n, base + ls, le - ls);
            n += le - ls;
            head[n++] = '\r'; head[n++] = '\n';
        }
        ls = le + 2;
    }

    /* Our own forwarding record. X-Forwarded-For is SET, not appended to a
       client-supplied one -- a backend doing IP auth on it is the exploit, and
       the client's copy was dropped as hop-by-hop above. */
    k = snprintf(head + n, sizeof(head) - n,
                 "X-Forwarded-For: %s\r\nX-Forwarded-Proto: http\r\n"
                 "Via: 1.1 dynajs\r\nContent-Length: %lld\r\n"
                 "Connection: close\r\n\r\n",
                 "127.0.0.1", (long long)clen);
    if (k < 0 || n + (size_t)k >= sizeof(head)) {
        dyn_app_send_err(c, 431, "{\"error\":\"request head too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    n += (size_t)k;

    p = (dyn_app_proxy_t *)calloc(1, sizeof(*p));
    if (!p) {
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    p->c = c;
    p->fd = -1;
    dyn_iobuf_init(&p->resp);
    p->req_len = n + (size_t)clen;
    p->req = (uint8_t *)malloc(p->req_len ? p->req_len : 1);
    if (!p->req) {
        free(p);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    memcpy(p->req, head, n);
    if (clen > 0)
        memcpy(p->req + n, base + head_len, (size_t)clen);

    c->refs++;                                  /* the upstream leg holds it */
    p->fd = dyn_aio_connect(c->app->aio, rt->up_host, rt->up_port,
                            dyn_proxy_on_connect, p);
    if (p->fd < 0)
        dyn_proxy_fail(p, 502, "{\"error\":\"upstream connect failed\"}");
}

static void dyn_app_upload_start(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                 const char *base, size_t head_len, int64_t clen)
{
    JSContext *ctx = c->app->ctx;
    const char *ct;
    size_t ctlen = 0;
    int64_t cap = rt->max_file > 0 ? rt->max_file : (int64_t)(16 * 1024 * 1024);
    char ctbuf[128], fpath[2048];
    dyn_app_upload_t *u;
    int fd;

    if (clen < 0 || clen > cap) {
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        dyn_app_conn_close(c);
        return;
    }
    ct = dyn_req_header(base, head_len, "content-type", &ctlen);
    { size_t n = ctlen < sizeof(ctbuf) - 1 ? ctlen : sizeof(ctbuf) - 1;
      char *sc;
      memcpy(ctbuf, ct ? ct : "", n); ctbuf[n] = 0;
      if ((sc = strchr(ctbuf, ';')) != NULL) *sc = 0;
      /* Was two strlen calls PER ITERATION -- quadratic in a header the peer
         controls. Measure once, then walk back. */
      { size_t cl = strlen(ctbuf);
        while (cl && ctbuf[cl - 1] == ' ') ctbuf[--cl] = 0; } }
    if (rt->allow) {
        size_t k; int ok = 0;
        for (k = 0; k < rt->n_allow; k++)
            if (!strcmp(rt->allow[k], ctbuf)) { ok = 1; break; }
        if (!ok) { dyn_app_send_err(c, 403, "{\"error\":\"type not allowed\"}"); dyn_app_conn_close(c); return; }
    }
    /* O_EXCL|O_NOFOLLOW, not O_TRUNC: the name is predictable, so without
       O_EXCL a pre-placed symlink in the upload dir would be followed and
       truncated. O_EXCL fails on any existing path including a symlink; retry
       with a fresh sequence number rather than reusing a name. */
    fd = -1;
    for (int attempt = 0; attempt < 64 && fd < 0; attempt++) {
        snprintf(fpath, sizeof(fpath), "%s/up_%ld_%lu", rt->dir, (long)getpid(),
                 ++dyn_app_upload_seq);
        fd = open(fpath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) { dyn_app_send_err(c, 500, "{\"error\":\"open failed\"}"); dyn_app_conn_close(c); return; }
    u = (dyn_app_upload_t *)calloc(1, sizeof(*u));
    if (!u) { close(fd); dyn_app_conn_close(c); return; }
    u->fd = fd; u->remaining = clen; u->size = clen;
    u->handler = JS_DupValue(ctx, rt->handler);
    u->path = strdup(fpath);
    if (!u->path) {
        /* finish() would hand a NULL path to JS_NewString; fail the upload
           instead. The handler ref was taken above and dies with u here. */
        JS_FreeValue(ctx, u->handler);
        free(u);
        close(fd);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    /* Copy the STRING, not the buffer: memcpy of sizeof(u->ctype) was correct
       only because both are 128, which nothing pinned, and it dragged the
       uninitialised tail of a stack buffer into the heap struct. */
    snprintf(u->ctype, sizeof(u->ctype), "%s", ctbuf);
    c->up = u;
    dyn_iobuf_consume(&c->in, head_len); /* drop the request head */
    c->hdr_scan_from = 0;                /* next parse starts at the buffer head */
    dyn_app_upload_drain(c);             /* write buffered body; finish if complete */
}

/* Route + dispatch every complete request buffered in c->in. */
/* (The header caps DYN_APP_MAX_HEADER / DYN_APP_MAX_HEADER_COUNT are defined
   above with the one-pass scanner that also enforces them.) */

/* Number of complete field lines in a possibly-INCOMPLETE header block: used
   by the "no terminator yet" path to hold the count cap. The complete-block
   path gets its count from dyn_scan_head. */
static size_t dyn_req_header_count(const char *buf, size_t len)
{
    size_t i, n = 0;
    for (i = 0; i + 2 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] != '\r')
            n++;
    return n;
}

/* forward: dynamic dispatch lives after the typed handlers
 * (registration order), but the request pump above needs it. Returns 1
 * when a dynamic route matched and was dispatched (response sent or
 * pending), 0 when none matched, -1 with a response already sent. */
static int dyn_app_try_dyn(dyn_app_conn_t *c, const char *base, size_t head_len,
                           const char *path, const char *body, size_t body_len);

static void dyn_app_process_loop(dyn_app_conn_t *c)
{
    dyn_app_t *app = c->app;
    for (;;) {
        const char *base = (const char *)dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        /* Resume where the last search stopped, backing up 3 bytes so a
           terminator straddling the chunk boundary is still found. */
        size_t from = c->hdr_scan_from < avail ? c->hdr_scan_from : avail;
        const char *hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        size_t head_len, body_len, req_total, clv_len = 0, i, r;
        const char *cl;
        char path[1024];
        int64_t clen = 0;
        const dyn_app_route_t *route = NULL;
        dyn_reqinfo_t ri;

        if (!hdr_end) {
            c->hdr_scan_from = avail >= 3 ? avail - 3 : 0;
            /* Cap the header block independently of any body limit: without
               this a peer that never sends CRLFCRLF grows c->in without bound.
               431 is the status for it; close, since the stream is unusable. */
            if (avail > DYN_APP_MAX_HEADER ||
                dyn_req_header_count(base, avail) > DYN_APP_MAX_HEADER_COUNT) {
                dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
                dyn_app_conn_close(c);
            }
            return; /* need more header bytes */
        }

        /* ONE pass over the head produces every fact the pump and its
           dispatches need (validity, framing, duplicates, the interesting
           header values, the request line) where this loop used to walk the
           same bytes eight-plus times. Error-precedence matches the old
           helper order exactly: malformed line -> caps -> TE -> dup CL ->
           Host -> duplicate header -> content-length digits. */
        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src == DYN_HEAD_ERR_LINE) {
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
            if (src != 0 || ri.head_len == 0 ||
                ri.head_len > (size_t)(hdr_end - base) + 4) {
                /* the terminator was found but the scan did not accept a
                   well-formed head ending there: treat as malformed, the
                   same verdict dyn_req_headers_valid gave odd shapes */
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
        }
        head_len = ri.head_len;

        /* The same caps for a COMPLETE block. Checking them only on the
           incomplete path above bounds a peer that never sends CRLFCRLF and
           nothing else: a peer that simply finishes its headers was accepted
           at any size up to the whole-request ceiling, so the advertised
           64 KiB header limit did not hold against the ordinary case. Found
           by driving the boundary from both sides -- an 83 KiB complete
           block answered 200. */
        if (head_len > DYN_APP_MAX_HEADER ||
            ri.n_lines > DYN_APP_MAX_HEADER_COUNT) {
            dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
            dyn_app_conn_close(c);
            return;
        }

        /* Framing decisions, in the order the separate scans used to make
           them. Transfer-Encoding the server cannot decode, or a duplicated
           Content-Length, would let the body be reinterpreted as a pipelined
           request (CWE-444): 501 or 400 and close rather than guess.
           RFC 7230 3.3.3. */
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
        /* RFC 9110 7.2: an HTTP/1.1 request without a Host header, or
           with more than one, MUST be a 400. Both used to dispatch
           (first-wins on Host), so the routing authority was whatever a
           duplicate said twice. */
        if (ri.http11 && ri.host_count != 1) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad host\"}");
            dyn_app_conn_close(c);
            return;
        }
        /* Duplicated non-framing headers used to be silently FIRST-WINS for
           every dyn_req_header scan, which is a front-end disagreement in
           itself: a proxy that joins list fields or keeps the LAST copy reads
           a different request than this server does (the smuggling class
           again). Refuse with 400. Fields that are comma-joinable lists, or
           special (cookie/set-cookie), keep first-wins; the framing fields
           and Host were already answered above with their own errors. */
        if (ri.dup) {
            dyn_app_send_err(c, 400, "{\"error\":\"duplicate header\"}");
            dyn_app_conn_close(c);
            return;
        }
        cl = ri.cl; clv_len = ri.cl_len;
        /* RFC 9110 8.6: DIGIT+ only. Stopping at the first non-digit made
           "-1" mean 0 and "5abc" mean 5 -- a length this server and a front end
           disagree about is a smuggled request (CWE-444), which is the same
           class as the duplicate-header refusal above. */
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
                if (clen > (int64_t)(1LL << 40)) { clen = (int64_t)(1LL << 40); break; }
            }
        }

        /* Stamped at dispatch, cleared when the response is queued: the
           duration histogram covers the handler's await for async routes.
           Not overwritten while a response is pending: a pipelined second
           request must not steal the first's stamp (that duration would
           read as the gap between the two dispatches). */
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
                /* identity is acceptable unless explicitly q=0 -- either
                   directly or through a *;q=0 that covers every coding. */
                {
                    int qi = dyn_ae_q(ri.ae, ri.ae_len, "identity", 8);
                    if (qi < 0 && dyn_ae_q(ri.ae, ri.ae_len, "*", 1) == 0)
                        qi = 0;
                    c->identity_refused = (qi == 0);
                }
            }
        }

        /* keep-alive decision, the same rule as the other two servers:
           HTTP/1.1 stays up unless Connection: close; HTTP/1.0 closes unless
           it asks keep-alive. The response's own send completion closes. */
        {
            int keep = ri.http11;
            if (dyn_hdr_token(ri.conn, ri.conn_len, "close"))
                keep = 0;
            else if (dyn_hdr_token(ri.conn, ri.conn_len, "keep-alive"))
                keep = 1;
            c->close_after = !keep;
            c->http11 = ri.http11;
        }

        /* the request target from the scan, with the original's exact
           truncation semantics (a target past the buffer is cut at 1023,
           never refused: the refusal is for non-printables only) */
        if (ri.bad_target) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad request\"}");
            dyn_app_conn_close(c);
            return;
        }
        {
            size_t plen = ri.target_len < sizeof(path) - 1
                        ? ri.target_len : sizeof(path) - 1;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
        }
        for (r = 0; r < app->n_routes; r++) {
            const dyn_app_route_t *rt = &app->routes[r];
            if (rt->type == APP_DYN)
                continue;
            if (rt->type == APP_STATIC || rt->type == APP_PROXY) {
                /* A prefix ends at a path segment: without the boundary a
                   route "/api" also captured "/apiv2", serving or forwarding
                   paths that were never registered with it. */
                size_t plen = strlen(rt->path);
                if (strncmp(path, rt->path, plen) == 0 &&
                    (path[plen] == '/' || path[plen] == '\0')) { route = rt; break; }
            } else if (strcmp(rt->path, path) == 0) { route = rt; break; }
        }

        /* A declared body past the whole-request cap can never complete:
           refuse now instead of buffering toward the cap waiting for bytes
           that would overflow it anyway. Uploads stream to disk under their
           own per-route cap and are exempt. */
        if (clen > (int64_t)DYN_ACONN_MAX_REQ &&
            !(route && route->type == APP_UPLOAD)) {
            dyn_app_send_err(c, 413, "{\"error\":\"payload too large\"}");
            dyn_app_conn_close(c);
            return;
        }

        /* uploads stream to disk -- start before the whole body is buffered */
        if (route && route->type == APP_UPLOAD) {
            dyn_app_upload_start(c, route, base, head_len, clen);
            /* start() streams any already-buffered body and may FINISH
               synchronously; a request pipelined behind the upload is in
               the buffer NOW, so keep the loop going instead of waiting
               for a wakeup that never comes (measured as a stall). */
            if (c->up || c->closed)
                return;
            continue;
        }

        /* other routes need the full body buffered (bodies over the cap never
         * complete and the connection is dropped by on_recv's MAX_REQ guard) */
        body_len = clen > DYN_ACONN_MAX_REQ ? (size_t)DYN_ACONN_MAX_REQ + 1 : (size_t)clen;
        req_total = head_len + body_len;
        if (avail < req_total)
            return;

        if (!route) {
            /*typed routes miss first; a dynamic route answers
               before the observability/404 fallback (registration beats
               convention). dyn_rc != 0 means the response is sent or
               pending: fall through to the consume tail like every other
               answered request. */
            int dyn_rc = dyn_app_try_dyn(c, base, head_len, path,
                                         base + head_len, body_len);
            if (dyn_rc == 0) {
                /* The observability routes lose to a user route of the same
                   path: registration beats convention. */
                if (app->metrics_http && strcmp(path, "/metrics") == 0) {
                    size_t ml = 0;
                    char *m = dyn_metrics_c_scrape(&ml);
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
            /* Documented contract: "A content-type other than application/json
             * is refused with 415". A missing content-type is tolerated (the
             * wording refuses a type that IS something else); a present type
             * must name application/json, ignoring parameters (charset). */
            {
                const char *rct = ri.ct;
                size_t rctlen = ri.ct_len;
                if (rct) {
                    char typebuf[64];
                    size_t n = rctlen < sizeof(typebuf) - 1 ? rctlen : sizeof(typebuf) - 1;
                    char *sc;
                    memcpy(typebuf, rct, n);
                    typebuf[n] = 0;
                    if ((sc = strchr(typebuf, ';')) != NULL)
                        *sc = 0;
                    { size_t cl = strlen(typebuf);
                      while (cl && (typebuf[cl - 1] == ' ' || typebuf[cl - 1] == '\t'))
                          typebuf[--cl] = 0; }
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
            if (dyn_app_ws_handshake(c, route, base, head_len)) {
                dyn_iobuf_consume(&c->in, req_total);
        c->hdr_scan_from = 0;   /* next request starts its own scan */
                return; /* upgraded: remaining bytes are WS frames */
            }
            dyn_app_send_err(c, 400, "{\"error\":\"expected websocket upgrade\"}");
        } else if (route->type == APP_SSE) {
            dyn_app_sse_handshake(c, route);
            dyn_iobuf_consume(&c->in, req_total);
            c->hdr_scan_from = 0;
            return;             /* the handler owns the connection now */
        }
        dyn_iobuf_consume(&c->in, req_total);
        c->hdr_scan_from = 0;   /* next request starts its own scan */
        if (c->closed)
            return;
        /* A response is parked on a handler promise: HOLD here. The
           remaining pipelined bytes keep buffering until it settles, so
           responses leave in request order and a later request's close
           cannot land under an earlier pending response. A LIVE STREAMED
           RESPONSE holds exactly the same way for the same reason: its
           chunked body IS the current response, so any response written
           before its terminal chunk would land inside that body (a
           keep-alive queue desync), and a second stream would fight over
           the single pump slot. The hold lasts until the stream's terminal
           (or its truncation); the pump teardown resumes this loop through
           dyn_app_pump_resume. The held bytes are bounded by the same
           whole-request cap as every other buffered request. */
        if (c->resp_parked || c->st)
            return;
        /* This request asked to close: nothing pipelined behind it is
           answered (the same rule the thread-pool pump applies), and the
           response's send completion closes the socket. */
        if (c->close_after)
            return;
    }
}

/* The request pump, re-entry guarded. A handler thenable may settle
 * synchronously (its then() calls the reaction before returning), which puts
 * a settle -- and its resume -- on the stack under this loop; the guard makes
 * the nested call a no-op and the outer loop simply continues. A pump with a
 * response parked is likewise a no-op: the resume from the settle is the one
 * caller that gets through (it drops the count first). A LIVE STREAMED
 * RESPONSE holds the pump the same way: its teardown is the caller that gets
 * through (it clears the slot first). The ref keeps the
 * conn alive across the loop: an abandoned response's release can close the
 * conn (and drop its own ref) mid-dispatch, and the loop still has bytes to
 * consume before it notices. */
static void dyn_app_process(dyn_app_conn_t *c)
{
    if (c->in_process || c->resp_parked || c->st)
        return;
    c->in_process = 1;
    c->refs++;
    dyn_app_process_loop(c);
    c->in_process = 0;
    dyn_app_conn_unref(c);
}

/* A parked response settled (or a streamed response ended): pump the buffered
 * pipelined requests again. Skipped when the settling request asked to close
 * (its response rides the close; requests behind it are not answered) or the
 * conn is otherwise busy or gone -- or a stream is STILL live (`c->st`): the
 * resume that runs after the stream's teardown is the one caller that gets
 * through (it clears the slot first). */
static void dyn_app_pump_resume(dyn_app_conn_t *c)
{
    if (c->closed || c->in_process || c->close_after || c->resp_parked ||
        c->st || c->up || c->is_ws || c->is_sse)
        return;
    if (dyn_iobuf_rlen(&c->in) == 0)
        return;
    dyn_iobuf_ensure_nul(&c->in);
    dyn_app_process(c);
    if (!c->closed)
        dyn_iobuf_compact(&c->in);
}

static void dyn_app_conn_close(dyn_app_conn_t *c)
{
    if (c->closed)
        return;
    c->closed = 1;
    /*a live stream pump dies with the conn (a pending read's settle
       owns the teardown when one is parked) */
    dyn_app_stream_abort(c);
    if (c->is_ws) {
        JSContext *ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->ws_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[3] = { c->ws_this,
                                  JS_NewInt32(ctx, c->close_code
                                                      ? c->close_code : 1000),
                                  JS_NewStringLen(ctx, "", 0) };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 3, a);
            JS_FreeValue(ctx, a[2]);
        }
        JS_FreeValue(ctx, ch);
        if (c->ws_native)
            c->ws_native->conn = NULL; /* a retained WsConn now sends nothing */
    }
    if (c->is_sse) {
        JSContext *ctx = c->app->ctx;
        JSValue ch = JS_GetPropertyStr(ctx, c->sse_handlers, "close");
        if (JS_IsFunction(ctx, ch)) {
            JSValueConst a[1] = { c->sse_this };
            dyn_call_drop(ctx, ch, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, ch);
        if (c->sse_native)
            c->sse_native->conn = NULL; /* a retained SseConn now sends nothing */
    }
    dyn_aio_close(c->app->aio, c->fd);
    dyn_app_conn_unref(c); /* drop the connection's own ref (pending handlers keep it) */
}

static void dyn_app_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *ud)
{
    dyn_app_conn_t *c = (dyn_app_conn_t *)ud;
    (void)aio;
    if (res <= 0) { dyn_app_conn_close(c); return; }
    c->refs++; /* hold across processing: a handler/frame may close the conn */
    if (dyn_iobuf_append(&c->in, buf, len) < 0) {
        dyn_app_conn_close(c);
    } else if (!c->up && dyn_iobuf_rlen(&c->in) > DYN_ACONN_MAX_REQ) {
        dyn_app_conn_close(c); /* oversized non-streaming request */
    } else {
        /* PROTOCOL progress resets the idle clock, not byte arrival: a
           slowloris peer delivers bytes forever without ever completing a
           header, so stamping on recv would make it look permanently active.
           An upload streaming to disk and a WebSocket frame ARE progress. */
        size_t before = dyn_iobuf_rlen(&c->in);
        if (c->up) {
            dyn_app_upload_drain(c); /* stream the upload body to disk */
            c->last_ms = dyn_timer_now_ms();
            /* The upload finished and left a PIPELINED request buffered
               behind it: process() returned early when the upload started,
               so without this the next request waits for a byte that never
               comes -- measured as a stall with both in one packet. */
            while (!c->closed && !c->up && dyn_iobuf_rlen(&c->in) > 0) {
                size_t had = dyn_iobuf_rlen(&c->in);
                dyn_iobuf_ensure_nul(&c->in);
                dyn_app_process(c);
                if (!c->closed && !c->up)
                    dyn_iobuf_compact(&c->in);
                /* A partial request consumes nothing: wait for more bytes */
                if (dyn_iobuf_rlen(&c->in) >= had)
                    break;
            }
        } else if (c->is_ws) {
            dyn_app_ws_process(c);
        } else if (c->is_sse) {
            /* An SSE client has nothing to say after the request; whatever
               arrives is pipelined junk -- discard it, and it is certainly
               not protocol progress. */
            dyn_iobuf_reset(&c->in);
        } else {
            dyn_iobuf_ensure_nul(&c->in); /* writable sentinel slot at data[len] */
            dyn_app_process(c);           /* HTTP; may upgrade WS or start upload */
            if (!c->closed && c->is_ws)
                dyn_app_ws_process(c);    /* frames right after an upgrade */
        }
        if (!c->closed) {
            dyn_iobuf_compact(&c->in); /* drop consumed prefix */
            /* Bytes were CONSUMED: a request or frame completed. */
            if (dyn_iobuf_rlen(&c->in) < before)
                c->last_ms = dyn_timer_now_ms();
        }
    }
    dyn_app_conn_unref(c);
}

/* Close every connection idle past app->idle_ms. One pass over the live list,
 * once a second; re-arms itself. */
static void dyn_app_idle_sweep(void *arg)
{
    dyn_app_t *app = (dyn_app_t *)arg;
    uint64_t now = dyn_timer_now_ms();
    dyn_app_conn_t *c = (dyn_app_conn_t *)app->conns, *next;

    while (c) {
        next = (dyn_app_conn_t *)c->lnext;   /* close() unlinks c */
        /* SSE is exempt: a healthy stream is server-driven and can be silent
           far longer than idle_ms. Its lifecycle is the client's disconnect
           or the handler's close(), not inbound traffic. */
        if (!c->closed && !c->is_sse && now - c->last_ms >= app->idle_ms) {
            dyn_app_conn_unlink(c);
            dyn_app_conn_close(c);
        }
        c = next;
    }
    if (app->timers && app->idle_ms)
        app->sweep = dyn_timer_add(app->timers, now, 1000,
                                   dyn_app_idle_sweep, app);
}

static void dyn_app_on_accept(dyn_aio_t *aio, int res, const uint8_t *buf,
                              unsigned len, void *ud)
{
    dyn_app_t *app = (dyn_app_t *)ud;
    dyn_app_conn_t *c;
    (void)buf; (void)len;
    if (res < 0) return;
    /* Refuse BEFORE allocating: the cap exists so a peer cannot spend an fd
       and a connection struct per connection. Accept-then-close is the
       refusal. Same default as HTTPServerAsync. */
    if (app->max_conns && app->n_conns >= app->max_conns) {
        dyn_aio_close(aio, res);
        return;
    }
    c = (dyn_app_conn_t *)calloc(1, sizeof(*c));
    if (!c) { dyn_aio_close(aio, res); return; }
    c->app = app;
    c->fd = res;
    c->refs = 1; /* the live connection holds one ref */
    c->ws_handlers = JS_UNDEFINED;
    c->ws_this = JS_UNDEFINED;
    c->sse_handlers = JS_UNDEFINED;
    c->sse_this = JS_UNDEFINED;
    dyn_iobuf_init(&c->ws_frag);
    dyn_iobuf_init(&c->in);
    c->last_ms = dyn_timer_now_ms();
    /* Always on the list: dispose must reach every live conn, not just the
       swept ones. The links are in-struct, so this costs no allocation. */
    c->lnext = app->conns;
    if (app->conns)
        ((dyn_app_conn_t *)app->conns)->lprev = c;
    app->conns = c;
    app->n_conns++;
    dyn_aio_recv(aio, res, 0, /*multishot=*/1, dyn_app_on_recv, c);
}

static void dyn_app_dispose(void *native)
{
    dyn_app_t *app = (dyn_app_t *)native;
    size_t i;
    /* Close every live conn BEFORE the app: a conn's reactor callback can
       land after dispose returns, and it reads c->app. Conns with pending
       async handlers survive as closed husks and free the app on their
       last unref (see dyn_app_conn_unref). */
    while (app->conns) {
        dyn_app_conn_t *c = (dyn_app_conn_t *)app->conns;
        dyn_app_conn_unlink(c);
        dyn_app_conn_close(c);
    }
    if (app->timers) {
        dyn_timers_free(app->timers);
        app->timers = NULL;      /* the sweep must not re-arm after this */
        app->idle_ms = 0;
    }
    if (app->aio) {
        dyn_net_off_drain(app);
        if (app->listen_fd >= 0)
            dyn_aio_close(app->aio, app->listen_fd);
        dyn_net_reactor_release(app->ctx);  /* shared: last user frees */
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
    for (i = 0; i < app->n_mw; i++)
        JS_FreeValue(app->ctx, app->mw[i]);
    free(app->mw);
    free(app->listen_host);
    /* A pending async handler may still hold a conn husk; its last unref
       frees the app. */
    app->dispose_called = 1;
    if (app->n_conns == 0)
        free(app);
}

/* An App holds JSValues -- one handler per rpc/upload/ws route -- so the cycle
 * collector has to be able to trace them. Without this mark it CANNOT, and the
 * commonest possible usage makes an uncollectable cycle:
 *
 *     const app = new App({port});
 *     app.rpc("/x", { ping: () => "pong" });
 *
 * The handler closes over the module scope, which contains `app`, so
 * App -> handler -> scope -> App. Invisible to the collector, never freed, and
 * it surfaces only as `Assertion failed: (list_empty(&rt->gc_obj_list))` inside
 * JS_FreeRuntime at process exit -- which no test noticed because the HTTP
 * tests close their App and an explicitly closed App drops its handlers.
 *
 * The per-connection ws_handlers are NOT marked here: a connection is owned by
 * the aio layer, not by the App's route table, and it releases them on close.
 * Marking them from here would be marking something this object does not own. */
static void dyn_app_gc_mark(JSRuntime *rt, JSValueConst val,
                            JS_MarkFunc *mark_func)
{
    DynResource *r = (DynResource *)JS_GetOpaque(val, dyn_app_class_id);
    dyn_app_t *app = (r && !r->closed) ? (dyn_app_t *)r->native : NULL;
    size_t i;

    if (!app)
        return;
    for (i = 0; i < app->n_routes; i++)
        JS_MarkValue(rt, app->routes[i].handler, mark_func);
    for (i = 0; i < app->n_mw; i++)
        JS_MarkValue(rt, app->mw[i], mark_func);
}

static const JSClassDef dyn_app_class = {
    "App", .finalizer = dyn_res_finalizer, .gc_mark = dyn_app_gc_mark,
};

static JSValue dyn_app_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_app_t *app;
    int64_t port = 0;
    /*the two knobs the raw thread-pool HTTPServer has always had.
       0 means "not configured": start() then uses the same defaults as
       before this option existed. */
    int64_t workers = 0, backlog = 0;
    char *host_dup = NULL;
    /* Defaults to ON: the insecure behaviour is the old one, so leaving this
       off by default would mean the fix protects nobody (CWE-400). 0 disables
       it explicitly, for a caller who really wants unbounded idle peers. */
    int64_t idle_ms = 30000;
    /* Connection cap, default ON like HTTPServerAsync's: each accepted peer
       costs an fd and a connection struct, so without a cap a peer can spend
       both to exhaustion. 0 is the explicit opt-out. */
    int32_t max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
    /* gzip is likewise default-ON: it only fires for clients that asked via
     * Accept-Encoding, so the default costs wire nothing. */
    int compress_opt = 1;
    /* /metrics and /healthz are opt-IN: exposing internals is a stance, and
       the instrumentation itself (a lock-free bump per response) is always on. */
    int metrics_opt = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        /*reject unknown keys before any option is read or bound. */
        if (dyn_opts_strict(ctx, argv[0], http_app_keys, 8))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v)) { if (JS_ToInt64(ctx, &port, v)) return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &idle_ms, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (idle_ms < 0) idle_ms = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt32(ctx, &max_conns, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (max_conns < 0) max_conns = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char *h = JS_ToCString(ctx, v);
            if (!h) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            host_dup = strdup(h);
            JS_FreeCString(ctx, h);
            if (!host_dup) { JS_FreeValue(ctx, v); return JS_ThrowOutOfMemory(ctx); }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "compress");
        if (!JS_IsUndefined(v)) {
            compress_opt = JS_ToBool(ctx, v);
            /* -1 = the property get threw (getter ran user JS): refuse */
            if (compress_opt < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "metrics");
        if (!JS_IsUndefined(v)) {
            metrics_opt = JS_ToBool(ctx, v);
            if (metrics_opt < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, v);
        /*workers/backlog, validated as positive integers before the
           app exists. Both are read through the same integrality rule
           decimal's `precision` uses: 2.5 silently truncating to 2 looks
           like a working option and quietly changes the answer. */
        v = JS_GetPropertyStr(ctx, argv[0], "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            /* A throwing getter propagates ITS error, not a range message. */
            if (JS_ToFloat64(ctx, &d, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
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
            if (JS_ToFloat64(ctx, &d, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if ((int64_t)d != d || d < 1 || d > 65535) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "App: backlog must be an integer from 1 to 65535");
            }
            backlog = (int64_t)d;
        }
        JS_FreeValue(ctx, v);
    }
    app = (dyn_app_t *)calloc(1, sizeof(*app));
    if (!app) return JS_ThrowOutOfMemory(ctx);
    app->ctx = ctx;
    app->listen_fd = -1;
    app->port = (uint16_t)port;
    app->idle_ms = (uint64_t)idle_ms;
    app->max_conns = max_conns;
    app->compress = compress_opt;
    app->metrics_http = metrics_opt;
    app->listen_host = host_dup;
    /*0 = "not configured"; start applies the same defaults as ever. */
    app->workers = (int)workers;
    app->backlog = (int)backlog;
    app->aio = dyn_net_reactor_acquire(ctx);
    if (!app->aio) { free(host_dup); free(app); return JS_ThrowOutOfMemory(ctx); }
    return dyn_res_wrap(ctx, new_target, dyn_app_class_id, app, dyn_app_dispose);
}

/* rpc(path, methodsObject) -- register a strict JSON-RPC 2.0 endpoint. */
static JSValue dyn_app_rpc(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path;
    dyn_app_route_t *nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "rpc(path, methods) requires a methods object");
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr)); /* realloc does not zero the new slot */
    nr->path = strdup(path);
    nr->type = APP_RPC;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/*dynamic routes + middleware.
 *
 * get/post/put/patch/del(pattern, handler) register a method-specific route
 * whose pattern may contain `:param` (one segment) and a trailing `*rest`
 * (the remainder, slashes included). use(fn) appends a sync middleware run
 * before the matched handler: fn(ctx) returning an object with an own
 * `response` property short-circuits with that response, anything else
 * continues. ctx is {method, path, params, query, headers, body}. Handler
 * and short-circuit responses share one sender: a string is text/plain, a
 * byte view is application/octet-stream, an envelope {status?, body?,
 * contentType?} uses its fields, anything else is JSON. Handlers may be
 * async (a thenable is awaited); middleware is sync (a thenable from one is
 * a 500 -- use an async handler for async work). All registration is
 * additive: existing rpc/static/proxy/upload/ws/sse dispatch is untouched,
 * and dynamic routes are checked in registration order after the typed
 * routes miss. */

static int dyn_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode src[0..n) into a fresh NUL-terminated string. Invalid
 * % sequences are kept literally (lenient: a bad encoding in a path is
 * data, not a 400). `plus_space` converts '+' to ' ' for query strings. */
static char *dyn_pct_decode(const char *s, size_t n, int plus_space, size_t *outn)
{
    char *o = (char *)malloc(n + 1);
    size_t w = 0, i = 0;
    if (!o) return NULL;
    while (i < n) {
        if (s[i] == '%' && i + 2 < n + 1) {
            int h, l;
            if (i + 2 < n && ((h = dyn_hexval(s[i+1])) >= 0) &&
                ((l = dyn_hexval(s[i+2])) >= 0)) {
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
    if (outn) *outn = w;
    return o;
}

static int dyn_is_pname(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Validate a dynamic pattern. Returns 0 with no exception, -1 with a
 * TypeError pending naming the problem. Rules: non-empty, starts with '/',
 * no '?'/'#' (path only), no empty segments, static segments carry no
 * ':'/'*', ':name' needs a valid name, '*name' needs a valid name and must
 * be the last segment, at most one wildcard, no duplicate param names. */
static int dyn_app_dyn_validate(JSContext *ctx, const char *pat)
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
        while (seg_end < n && pat[seg_end] != '/') seg_end++;
        {
            size_t slen = seg_end - seg_start;
            if (slen == 0 && seg_end < n) {
                JS_ThrowTypeError(ctx, "dynamic route pattern has an empty segment");
                return -1;
            }
            if (slen > 0) {
                const char *seg = pat + seg_start;
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
                        if (strlen(names[k]) == slen - 1 &&
                            !memcmp(names[k], seg + 1, slen - 1)) {
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
                        if (strlen(names[k]) == slen - 1 &&
                            !memcmp(names[k], seg + 1, slen - 1)) {
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
        if (seg_end >= n) break;
        seg_start = seg_end + 1;
    }
    return 0;
}

/* Match pattern against path (both '/'-led, no query). On match fills
 * params (a fresh object) and returns 1; on mismatch returns 0; on OOM or
 * a throwing setter returns -1 with an exception pending. Values are
 * percent-decoded (path semantics: '+' stays literal). */
static int dyn_app_dyn_match(JSContext *ctx, const char *pat, const char *path,
                             JSValue params)
{
    const char *pp = pat, *qp = path;
    while (1) {
        const char *pe, *qe;
        size_t plen, qlen, qrem;
        while (*pp == '/') pp++;
        while (*qp == '/') qp++;
        if (!*pp && !*qp) return 1;
        if (!*pp || !*qp) {
            /* A trailing wildcard matches the empty remainder: the pattern
             * "/f/" + "*rest" meets "/f" and "/f/". Anything else needs
             * both sides. */
            if (*pp == '*' || (pp[0] == '*' )) {
                const char *nm = pp + 1;
                size_t nl = strlen(nm);
                char *dec;
                size_t dn;
                if (nl == 0) return 0;
                dec = dyn_pct_decode("", 0, 0, &dn);
                if (!dec) { JS_ThrowOutOfMemory(ctx); return -1; }
                {
                    JSValue v = JS_NewStringLen(ctx, dec, dn);
                    free(dec);
                    if (JS_IsException(v)) return -1;
                    /* DefinePropertyValue: an OWN property, so a capture
                     * named __proto__ lands as data instead of invoking
                     * Object.prototype's setter. It consumes v. */
                    if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                                                  JS_PROP_C_W_E) < 0)
                        return -1;
                }
                return 1;
            }
            return 0;
        }
        pe = strchr(pp, '/');
        qe = strchr(qp, '/');
        plen = pe ? (size_t)(pe - pp) : strlen(pp);
        qrem = strlen(qp);   /* path remainder from here (slashes included) */
        qlen = qe ? (size_t)(qe - qp) : qrem;
        if (pp[0] == ':') {
            char nm[64];
            char *dec;
            size_t dn;
            if (qlen == 0) return 0;
            if (plen - 1 >= sizeof nm) return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            dec = dyn_pct_decode(qp, qlen, 0, &dn);
            if (!dec) { JS_ThrowOutOfMemory(ctx); return -1; }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v)) return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                                              JS_PROP_C_W_E) < 0)
                    return -1;
            }
        } else if (pp[0] == '*') {
            char nm[64];
            /* Wildcard takes the path remainder from HERE (qp points at
             * the segment start; the remainder includes slashes). */
            size_t rlen = qrem;  /* wildcard consumes the whole remainder */
            char *dec;
            size_t dn;
            if (plen - 1 >= sizeof nm) return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            dec = dyn_pct_decode(qp, rlen, 0, &dn);
            if (!dec) { JS_ThrowOutOfMemory(ctx); return -1; }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v)) return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                                              JS_PROP_C_W_E) < 0)
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

static void dyn_app_req_method(const char *base, char *out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap && base[i] && base[i] != ' ' && base[i] != '\r')
        { out[i] = base[i]; i++; }
    out[i] = '\0';
}

static void dyn_app_req_target(const char *base, size_t head_len,
                               const char **tgt, size_t *tlen)
{
    const char *s = strchr(base, ' ');
    const char *e;
    if (!s) { *tgt = ""; *tlen = 0; return; }
    s++;
    e = strchr(s, ' ');
    if (!e || (size_t)(e - base) > head_len) { *tgt = ""; *tlen = 0; return; }
    *tgt = s;
    *tlen = (size_t)(e - s);
}

static JSValue dyn_app_build_query(JSContext *ctx, const char *qs, size_t qn)
{
    JSValue o = JS_NewObject(ctx);
    size_t i = 0;
    if (JS_IsException(o)) return o;
    while (i < qn) {
        size_t ks = i, ke, vs, ve;
        while (i < qn && qs[i] != '&') i++;
        ke = i;
        if (i < qn && qs[i] == '&') i++;
        vs = ke;
        ve = ke;
        {
            size_t eq = ks;
            while (eq < ke && qs[eq] != '=') eq++;
            if (eq < ke) { vs = eq + 1; ve = ke; ke = eq; }
            else { vs = ve = ke; }
        }
        if (ke > ks) {
            /* Keys decode exactly like values -- percent-decode with `+` as
             * space (the documented contract, and the WHATWG form-urlencoded
             * rule URLSearchParams follows). Defined as own properties: a
             * "__proto__" query key is data, and a repeated key redefines
             * (last wins). */
            char *kdec = dyn_pct_decode(qs + ks, ke - ks, 1, NULL);
            char *vdec = dyn_pct_decode(qs + vs, ve - vs, 1, NULL);
            if (!kdec || !vdec) {
                free(kdec); free(vdec);
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
                if (rc < 0) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
            }
        }
    }
    return o;
}

static JSValue dyn_app_build_headers(JSContext *ctx, const char *base, size_t head_len)
{
    JSValue o = JS_NewObject(ctx);
    size_t ls, le;
    if (JS_IsException(o)) return o;
    ls = 0;
    while (ls + 1 < head_len && !(base[ls] == '\r' && base[ls+1] == '\n')) ls++;
    ls += 2;
    while (ls + 1 < head_len) {
        size_t colon, vs, ve, ns, ne;
        char nbuf[128];
        size_t k;
        le = ls;
        while (le + 1 < head_len && !(base[le] == '\r' && base[le+1] == '\n')) le++;
        if (le == ls) break;
        colon = ls;
        while (colon < le && base[colon] != ':') colon++;
        if (colon >= le) { ls = le + 2; continue; }
        ns = ls; ne = colon;
        while (ne > ns && (base[ne-1] == ' ' || base[ne-1] == '\t')) ne--;
        while (ns < ne && (base[ns] == ' ' || base[ns] == '\t')) ns++;
        vs = colon + 1; ve = le;
        while (vs < ve && (base[vs] == ' ' || base[vs] == '\t')) vs++;
        while (ve > vs && (base[ve-1] == ' ' || base[ve-1] == '\t')) ve--;
        if (ne - ns >= sizeof nbuf) { ls = le + 2; continue; }
        for (k = 0; k < ne - ns; k++) {
            unsigned char c = (unsigned char)base[ns + k];
            nbuf[k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        nbuf[ne - ns] = '\0';
        {
            JSValue cur = JS_GetPropertyStr(ctx, o, nbuf);
            JSValue nv = JS_NewStringLen(ctx, base + vs, ve - vs);
            if (JS_IsException(nv)) { JS_FreeValue(ctx, cur); JS_FreeValue(ctx, o); return JS_EXCEPTION; }
            if (JS_IsString(cur)) {
                /* Repeats only reach here for the comma-joinable list fields
                 * and cookie/set-cookie (any other duplicate is refused with
                 * 400 before dispatch). Join per the field's own grammar:
                 * list fields comma-separate, Cookie's pairs join with "; "
                 * (RFC 6265), and Set-Cookie values cannot be combined at
                 * all (RFC 9110 5.2 -- its value contains commas) so the
                 * first copy stands. */
                size_t a, b;
                const char *as = JS_ToCStringLen(ctx, &a, cur);
                const char *bs = JS_ToCStringLen(ctx, &b, nv);
                const char *sep = ", ";
                size_t sepl = 2;
                char *joined;
                JSValue jv;
                if (!strcmp(nbuf, "cookie")) { sep = "; "; sepl = 2; }
                JS_FreeValue(ctx, cur);
                JS_FreeValue(ctx, nv);
                if (!strcmp(nbuf, "set-cookie")) {
                    /* first copy stands (already stored); drop the rest */
                    if (as) JS_FreeCString(ctx, as);
                    if (bs) JS_FreeCString(ctx, bs);
                } else if (!as || !bs) {
                    if (as) JS_FreeCString(ctx, as);
                    if (bs) JS_FreeCString(ctx, bs);
                    JS_FreeValue(ctx, o);
                    return JS_EXCEPTION;
                } else {
                    joined = (char *)malloc(a + sepl + b + 1);
                    if (!joined) {
                        JS_FreeCString(ctx, as); JS_FreeCString(ctx, bs);
                        JS_FreeValue(ctx, o);
                        return JS_ThrowOutOfMemory(ctx);
                    }
                    memcpy(joined, as, a);
                    memcpy(joined + a, sep, sepl);
                    memcpy(joined + a + sepl, bs, b);
                    joined[a + sepl + b] = '\0';
                    JS_FreeCString(ctx, as); JS_FreeCString(ctx, bs);
                    jv = JS_NewString(ctx, joined);
                    free(joined);
                    if (JS_IsException(jv)) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
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

static int dyn_value_is_bytes(JSContext *ctx, JSValueConst v)
{
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    if (p) return 1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) { JS_FreeValue(ctx, ab); return bpe == 1; }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return 0;
}

static int dyn_bytes_to_c(JSContext *ctx, JSValueConst v,
                          const uint8_t **pp, size_t *pn,
                          JSValue *keep)
{
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    if (p) { *pp = p; *pn = n; *keep = JS_UNDEFINED; return 0; }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, absz;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t *base;
        if (JS_IsException(ab)) return -1;
        if (bpe != 1) { JS_FreeValue(ctx, ab); return -1; }
        base = JS_GetArrayBuffer(ctx, &absz, ab);
        *keep = ab;
        if (!base) { JS_FreeValue(ctx, ab); *keep = JS_UNDEFINED; return -1; }
        if (off > absz || len > absz - off) {
            JS_FreeValue(ctx, ab); *keep = JS_UNDEFINED;
            return -1;
        }
        *pp = base + off;
        *pn = len;
        return 0;
    }
}

/* Send a dynamic-route response value. Envelope detection: an object with an
 * own numeric `status` or an own `body` is an envelope {status?, body?,
 * contentType?}; anything else is a direct value (string/bytes -> raw,
 * object -> JSON). Undefined is a 500 (a handler must return a response;
 * middleware uses undefined to continue, which never reaches here). */
static void dyn_app_dyn_send(dyn_app_conn_t *c, JSValueConst val);

static void dyn_app_dyn_send_json_c(dyn_app_conn_t *c, JSContext *ctx, JSValueConst v)
{
    JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jstr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"unserializable response\"}");
        return;
    }
    {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, jstr);
        JS_FreeValue(ctx, jstr);
        if (!s) {
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        dyn_app_send_body(c, 200, "application/json", s, n);
        JS_FreeCString(ctx, s);
    }
}

static void dyn_app_dyn_send(dyn_app_conn_t *c, JSValueConst val)
{
    JSContext *ctx = c->app->ctx;
    if (JS_IsString(val)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, val);
        if (!s) { dyn_app_send_err(c, 500, "{\"error\":\"oom\"}"); return; }
        dyn_app_send_body(c, 200, "text/plain", s, n);
        JS_FreeCString(ctx, s);
        return;
    }
    if (dyn_value_is_bytes(ctx, val)) {
        const uint8_t *p = NULL;
        size_t n = 0;
        JSValue keep = JS_UNDEFINED;
        if (dyn_bytes_to_c(ctx, val, &p, &n, &keep) == 0) {
            dyn_app_send_body(c, 200, "application/octet-stream",
                              (const char *)p, n);
            JS_FreeValue(ctx, keep);
        } else {
            JS_FreeValue(ctx, keep);
            dyn_app_send_err(c, 500, "{\"error\":\"bad bytes response\"}");
        }
        return;
    }
    if (JS_IsObject(val)) {
        /* Envelope detection reads OWN properties only, as the docs promise:
         * with a prototype-pollution gadget, an inherited `body` or numeric
         * `status` would turn every plain object return into an injected
         * response or a 500. */
        JSValue has_status = dyn_app_own_get(ctx, val, "status");
        JSValue has_body = dyn_app_own_get(ctx, val, "body");
        int is_env = 0;
        int status = 200;
        if (!JS_IsException(has_status) && !JS_IsUndefined(has_status) &&
            JS_IsNumber(has_status))
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
            char ct_owned[128];
            const char *ct = "application/json";
            int ct_set = 0;
            if (!JS_IsException(ctv) && JS_IsString(ctv)) {
                size_t cn = 0;
                const char *cs = JS_ToCStringLen(ctx, &cn, ctv);
                if (cs) {
                    size_t k = cn < sizeof ct_owned - 1 ? cn : sizeof ct_owned - 1;
                    memcpy(ct_owned, cs, k);
                    ct_owned[k] = '\0';
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
                const char *s = JS_ToCStringLen(ctx, &n, has_body);
                /* A string body in an envelope defaults to text, not JSON:
                 * sending "\"hi\"" for body "hi" surprises. An explicit
                 * contentType always wins. */
                const char *use_ct = ct_set ? ct : "text/plain";
                JS_FreeValue(ctx, has_body);
                if (!s) { dyn_app_send_err(c, 500, "{\"error\":\"oom\"}"); return; }
                dyn_app_send_body(c, status, use_ct, s, n);
                JS_FreeCString(ctx, s);
                return;
            }
            if (dyn_value_is_bytes(ctx, has_body)) {
                const uint8_t *p = NULL;
                size_t n = 0;
                JSValue keep = JS_UNDEFINED;
                const char *use_ct = ct_set ? ct : "application/octet-stream";
                if (dyn_bytes_to_c(ctx, has_body, &p, &n, &keep) == 0) {
                    dyn_app_send_body(c, status, use_ct, (const char *)p, n);
                    JS_FreeValue(ctx, keep);
                } else {
                    JS_FreeValue(ctx, keep);
                    dyn_app_send_err(c, 500, "{\"error\":\"bad response body\"}");
                }
                JS_FreeValue(ctx, has_body);
                return;
            }
            /* Any other body value JSON-encodes inside the envelope. */
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
                    const char *s = JS_ToCStringLen(ctx, &n, jstr);
                    JS_FreeValue(ctx, jstr);
                    if (!s) { dyn_app_send_err(c, 500, "{\"error\":\"oom\"}"); return; }
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

/* {"error":"<msg>"} with the message JSON-escaped. A thrown Error's message
 * is arbitrary user text: quotes, backslashes and control bytes in it must
 * not corrupt the response body's JSON (the raw snprintf form let a throw
 * from user code inject body bytes). The explicit-length form keeps text
 * after an embedded NUL (a C-string walk truncated it). */
static void dyn_app_dyn_err_json_n(dyn_app_conn_t *c, int status,
                                   const char *msg, size_t msg_len)
{
    char buf[256];
    size_t pos = 0;
    buf[pos++] = '{'; buf[pos++] = '"'; buf[pos++] = 'e'; buf[pos++] = 'r';
    buf[pos++] = 'r'; buf[pos++] = 'o'; buf[pos++] = 'r'; buf[pos++] = '"';
    buf[pos++] = ':'; buf[pos++] = '"';
    dyn_json_escape_n_into(buf, sizeof buf - 3, &pos,
                           msg ? msg : "Server error",
                           msg ? msg_len : strlen("Server error"));
    buf[pos++] = '"'; buf[pos++] = '}';
    dyn_app_send_json(c, status, buf, pos);
}

/* Settle reaction for a dynamic handler's thenable: magic 0 = fulfilled,
 * 1 = rejected. The pending struct is claimed through the settle-once latch
 * (dyn_app_pend_claim): a thenable that resolves twice, or resolves and then
 * rejects/throws, sends ONE response and releases the struct and the conn
 * ref exactly once. */
static JSValue dyn_app_dyn_settle(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    dyn_app_pend_t *pd;
    dyn_app_conn_t *c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd) /* already settled: the first reaction owns the response */
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        if (magic) {
            const char *em = NULL;
            size_t em_len = 0;
            JSValue m = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "message") : JS_UNDEFINED;
            if (JS_IsString(m)) em = JS_ToCStringLen(ctx, &em_len, m);
            JS_FreeValue(ctx, m);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                                   em ? em_len : strlen("Server error"));
            if (em) JS_FreeCString(ctx, em);
        } else {
            dyn_app_dyn_send(c, v);
        }
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

static void dyn_app_dyn_send_or_await(dyn_app_conn_t *c, JSValue res)
{
    JSContext *ctx = c->app->ctx;
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
        dyn_app_pend_t *pd = (dyn_app_pend_t *)calloc(1, sizeof *pd);
        JSValue pobj, thenv, onres, onrej;
        if (!pd) {
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        pd->conn = c;
        c->refs++; /* released exactly once, by whichever path settles */
        pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
        if (JS_IsException(pobj)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_app_pend_release(pd, 0); /* never parked: free + unref */
            JS_FreeValue(ctx, res);
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        JS_SetOpaque(pobj, pd);
        onres = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 0, 1, &pobj);
        onrej = JS_NewCFunctionData(ctx, dyn_app_dyn_settle, 1, 1, 1, &pobj);
        if (JS_IsException(onres) || JS_IsException(onrej)) {
            JS_FreeValue(ctx, onres); JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj); /* finalizer: free + unref */
            dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
            return;
        }
        thenv = JS_GetPropertyStr(ctx, res, "then");
        if (JS_IsException(thenv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, onres); JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, pobj); /* finalizer: free + unref */
            dyn_app_send_err(c, 500, "{\"error\":\"bad thenable\"}");
            return;
        }
        {
            JSValueConst a[2] = { onres, onrej };
            JSValue r;
            pd->counted = 1;    /* the pump holds until this settles */
            c->resp_parked++;
            r = JS_Call(ctx, thenv, res, 2, a);
            JS_FreeValue(ctx, thenv);
            JS_FreeValue(ctx, onres);
            JS_FreeValue(ctx, onrej);
            JS_FreeValue(ctx, res);
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                /* The then call threw synchronously: settle as a rejection
                 * would -- UNLESS a reaction already ran. Exactly one of the
                 * reactions and this fallback releases the struct. */
                dyn_app_pend_t *p2 = dyn_app_pend_claim(pobj);
                if (p2) {
                    if (!p2->conn->closed)
                        dyn_app_send_json(p2->conn, 500,
                                          "{\"error\":\"Server error\"}", 24);
                    dyn_app_pend_release(p2, 0);
                }
            } else {
                JS_FreeValue(ctx, r);
            }
            JS_FreeValue(ctx, pobj); /* the reactions hold it while parked */
        }
    }
}

/* Dispatch a matched dynamic route: run sync middleware, then the handler.
 * ctx_obj owns params/query/headers/body already; this takes ownership of
 * ctx_obj (frees on every path). */
static void dyn_app_dispatch_dyn(dyn_app_conn_t *c, const dyn_app_route_t *rt,
                                 JSValue ctx_obj)
{
    JSContext *ctx = c->app->ctx;
    dyn_app_t *app = c->app;
    size_t i;
    for (i = 0; i < app->n_mw; i++) {
        JSValueConst a[1] = { ctx_obj };
        JSValue r = JS_Call(ctx, app->mw[i], JS_UNDEFINED, 1, a);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char *em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, ctx_obj);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                                   em ? em_len : strlen("Server error"));
            if (em) JS_FreeCString(ctx, em);
            return;
        }
        if (JS_IsObject(r)) {
            JSValue th = JS_GetPropertyStr(ctx, r, "then");
            int is_then = JS_IsFunction(ctx, th);
            JS_FreeValue(ctx, th);
            if (is_then) {
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, ctx_obj);
                dyn_app_send_err(c, 500, "{\"error\":\"async middleware not supported\"}");
                return;
            }
        }
        if (JS_IsObject(r)) {
            /* OWN property only (the documented contract): an inherited
             * `response` must not short-circuit the chain -- one prototype-
             * pollution gadget would otherwise answer every gated request. */
            JSValue rv = dyn_app_own_get(ctx, r, "response");
            int has = !JS_IsException(rv) && !JS_IsUndefined(rv);
            JS_FreeValue(ctx, r);
            if (JS_IsException(rv)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, ctx_obj);
                dyn_app_send_err(c, 500, "{\"error\":\"middleware response read failed\"}");
                return;
            }
            if (has) {
                JS_FreeValue(ctx, ctx_obj);
                dyn_app_dyn_send_or_await(c, rv);
                return;
            }
            JS_FreeValue(ctx, rv);
            continue;
        }
        JS_FreeValue(ctx, r);
    }
    {
        JSValueConst a[1] = { ctx_obj };
        JSValue res = JS_Call(ctx, rt->handler, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, ctx_obj);
        if (JS_IsException(res)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char *em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                                   em ? em_len : strlen("Server error"));
            if (em) JS_FreeCString(ctx, em);
            return;
        }
        dyn_app_dyn_send_or_await(c, res);
    }
}

static JSValue dyn_app_dyn_register(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    const char *method)
{
    dyn_app_t *app;
    const char *pat = NULL;
    dyn_app_route_t *nr;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern and handler are required",
                                 method);
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern must be a string", method);
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): handler must be a function", method);
    pat = JS_ToCString(ctx, argv[0]);
    if (!pat) return JS_EXCEPTION;
    if (dyn_app_dyn_validate(ctx, pat) < 0) {
        JS_FreeCString(ctx, pat);
        return JS_EXCEPTION;
    }
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, pat); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, pat); return JS_ThrowOutOfMemory(ctx); }
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
        free(nr->dyn_method); free(nr->dyn_pattern); free(nr->path);
        JS_FreeValue(ctx, nr->handler);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->n_routes++;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_app_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return dyn_app_dyn_register(ctx, this_val, argc, argv, "GET"); }
static JSValue dyn_app_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return dyn_app_dyn_register(ctx, this_val, argc, argv, "POST"); }
static JSValue dyn_app_put(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return dyn_app_dyn_register(ctx, this_val, argc, argv, "PUT"); }
static JSValue dyn_app_patch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return dyn_app_dyn_register(ctx, this_val, argc, argv, "PATCH"); }
static JSValue dyn_app_del(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return dyn_app_dyn_register(ctx, this_val, argc, argv, "DELETE"); }

static JSValue dyn_app_use(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    dyn_app_t *app;
    JSValue *nm;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "use(fn): fn must be a function");
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) return JS_EXCEPTION;
    if (app->n_mw == app->cap_mw) {
        size_t nc = app->cap_mw ? app->cap_mw * 2 : 4;
        nm = (JSValue *)realloc(app->mw, nc * sizeof *nm);
        if (!nm) return JS_ThrowOutOfMemory(ctx);
        app->mw = nm;
        app->cap_mw = nc;
    }
    app->mw[app->n_mw++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

/* dispatch entry, called by the request pump when no typed route
 * matched. Scans the APP_DYN routes in registration order: a route matches
 * when its method equals the request method (exact, case-sensitive -- HEAD
 * needs its own registration) AND its pattern matches the request path
 * (raw, query stripped; `:param` values are percent-decoded, '+' stays
 * literal in paths). The first method+pattern match wins; a pattern that
 * matches under another method is remembered and, absent a full match,
 * answers 405 rather than 404. The query string becomes `query` (values
 * '+'-decoded), the raw body `body`, and the whole request head
 * `headers` (lower-cased names; only the list fields and cookie/set-cookie
 * can repeat -- anything else is a 400 before dispatch -- repeats join ", ",
 * cookie joins "; ", set-cookie keeps the first copy). Returns 1 when a
 * route matched and was dispatched (response sent or pending on a handler
 * promise), 0 when nothing matched (the pump answers metrics/404), -1
 * when a response was already sent (405 or a dispatch-level failure). */
static int dyn_app_try_dyn(dyn_app_conn_t *c, const char *base, size_t head_len,
                           const char *path, const char *body, size_t body_len)
{
    dyn_app_t *app = c->app;
    JSContext *ctx = app->ctx;
    char method[16];
    const char *tgt;
    size_t tlen, i;
    int pat_hit = 0;

    dyn_app_req_method(base, method, sizeof method);
    dyn_app_req_target(base, head_len, &tgt, &tlen);
    for (i = 0; i < app->n_routes; i++) {
        const dyn_app_route_t *rt = &app->routes[i];
        JSValue params, ctx_obj, qv, hv, bv, mv, pv;
        const char *qs;
        size_t qn;
        int m;
        if (rt->type != APP_DYN)
            continue;
        params = JS_NewObject(ctx);
        if (JS_IsException(params))
            goto fail_oom;
        m = dyn_app_dyn_match(ctx, rt->dyn_pattern, path, params);
        if (m < 0) { JS_FreeValue(ctx, params); goto fail_oom; }
        if (m == 0) { JS_FreeValue(ctx, params); continue; }
        pat_hit = 1;
        if (strcmp(rt->dyn_method, method) != 0) {
            JS_FreeValue(ctx, params);
            continue;
        }
        /* The matched route owns the request: build ctx and dispatch. */
        qs = (const char *)memchr(tgt, '?', tlen);
        qn = qs ? (size_t)(tgt + tlen - (qs + 1)) : 0;
        qv = qs ? dyn_app_build_query(ctx, qs + 1, qn) : JS_NewObject(ctx);
        hv = dyn_app_build_headers(ctx, base, head_len);
        bv = JS_NewStringLen(ctx, body, body_len);
        mv = JS_NewString(ctx, method);
        pv = JS_NewString(ctx, path);
        ctx_obj = JS_NewObject(ctx);
        if (JS_IsException(qv) || JS_IsException(hv) || JS_IsException(bv) ||
            JS_IsException(mv) || JS_IsException(pv) || JS_IsException(ctx_obj)) {
            JS_FreeValue(ctx, qv); JS_FreeValue(ctx, hv); JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, mv); JS_FreeValue(ctx, pv);
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
        /* RFC 9110 15.5.6: a 405 MUST carry Allow naming the methods the
         * matched resource does answer. Collect the registered methods of
         * every dynamic route whose pattern matches this path (deduped,
         * registration order, bounded). */
        char allow[256];
        size_t al = 0, a;
        for (i = 0; i < app->n_routes; i++) {
            const dyn_app_route_t *rt = &app->routes[i];
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
                while (e < al && allow[e] != ',') e++;
                if (e - a == ml && memcmp(allow + a, rt->dyn_method, ml) == 0)
                    { dup = 1; break; }
                a = e + 2; /* ", " separator */
            }
            if (dup || al + ml + 3 >= sizeof allow)
                continue;
            if (al) { allow[al++] = ','; allow[al++] = ' '; }
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

/* Coerce opts.allow (array of ".ext"/"mime" strings) into a fresh char** list. */
static char **dyn_app_parse_allow(JSContext *ctx, JSValueConst opts, size_t *pn)
{
    char **out = NULL;
    JSValue av = JS_GetPropertyStr(ctx, opts, "allow");
    *pn = 0;
    if (JS_IsArray(ctx, av)) {
        uint32_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, av, "length");
        if (JS_ToUint32(ctx, &len, lv)) { JS_FreeValue(ctx, lv); return NULL; }
        JS_FreeValue(ctx, lv);
        if (len)
            out = (char **)calloc(len, sizeof(char *));
        for (i = 0; out && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, av, i);
            const char *s = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (s) { out[*pn] = strdup(s); if (out[*pn]) (*pn)++; JS_FreeCString(ctx, s); }
        }
    }
    JS_FreeValue(ctx, av);
    return out;
}

/* static(prefix, dir[, {maxFileSize, allow}]) -- serve files from a directory. */

/* app.proxy(prefix, { host, port }) -- forward matching requests upstream.
 * Registration coerces every argument to a C local before touching the App:
 * coercion runs arbitrary user code that could close it. */
static JSValue dyn_app_proxy(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_app_t *app;
    const char *prefix = NULL, *host = NULL;
    dyn_app_route_t *nr;
    JSValue jh, jp;
    int32_t port = 0;

    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "proxy(prefix, { host, port })");
    /*reject unknown keys before any coercion runs user code. */
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

    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto fail;
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_ThrowOutOfMemory(ctx); goto fail; }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_PROXY;
    nr->handler = JS_UNDEFINED;
    nr->up_host = strdup(host);
    nr->up_port = (uint16_t)port;
    if (!nr->path || !nr->up_host) {
        free(nr->path); free(nr->up_host);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->n_routes++;
    JS_FreeValue(ctx, jh); JS_FreeValue(ctx, jp);
    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_DupValue(ctx, this_val);
fail:
    JS_FreeValue(ctx, jh); JS_FreeValue(ctx, jp);
    if (host) JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}

static JSValue dyn_app_static(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_app_t *app;
    const char *prefix, *dir;
    dyn_app_route_t *nr;
    char **allow = NULL;
    size_t n_allow = 0;
    int64_t maxf = 0;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "static(prefix, dir[, opts])");
    /* coerce ALL args to C locals BEFORE resolving the native handle */
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix) return JS_EXCEPTION;
    /* The document root is a Path, like every other path-taking API in the
     * library (W3.7). It is BORROWED -- a Path has no close(), so nothing can
     * invalidate the bytes -- and it is resolved exactly once here, at route
     * registration, never per request. The prefix beside it stays a string
     * because it is a URL path, not a filesystem one. */
    dir = dyn_path_borrow(ctx, argv[1], "static(prefix, root)", NULL);
    if (!dir) { JS_FreeCString(ctx, prefix); return JS_EXCEPTION; }
    if (argc > 2 && JS_IsObject(argv[2])) {
        /* `prefix` is already an OWNED C string here: the strict refusal
         * must release it, like the fail path below. */
        if (dyn_opts_strict(ctx, argv[2], http_static_keys, 2)) {
            JS_FreeCString(ctx, prefix);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxFileSize");
        if (!JS_IsUndefined(v)) { if (JS_ToInt64(ctx, &maxf, v)) return JS_EXCEPTION; }
        JS_FreeValue(ctx, v);
        allow = dyn_app_parse_allow(ctx, argv[2], &n_allow);
    }

    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) goto fail;
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_ThrowOutOfMemory(ctx); goto fail; }
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
    /* `dir` is borrowed from a Path -- nothing to free. */
    return JS_UNDEFINED;
fail:
    { size_t k; for (k = 0; k < n_allow; k++) free(allow[k]); free(allow); }
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}

/* upload(path, {dir, maxFileSize, allow}, handler(savedPath, meta)). */
static JSValue dyn_app_upload(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path = NULL, *dir = NULL;
    dyn_app_route_t *nr;
    char **allow = NULL;
    size_t n_allow = 0, k;
    int64_t maxf = 0;
    JSValue handler = JS_UNDEFINED, v;

    if (argc < 3 || !JS_IsObject(argv[1]) || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "upload(path, {dir,...}, handler)");
    /*reject unknown keys before anything is coerced or resolved. */
    if (dyn_opts_strict(ctx, argv[1], http_upload_keys, 3))
        return JS_EXCEPTION;
    /* coerce ALL args to C locals BEFORE resolving the native handle */
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[1], "dir");
    /* Also a Path (W3.7): this is where uploaded files land. */
    dir = JS_IsUndefined(v) ? NULL
                            : dyn_path_borrow(ctx, v, "upload opts.dir", NULL);
    JS_FreeValue(ctx, v);
    if (!dir) {
        JS_FreeCString(ctx, path);
        /* If dyn_path_borrow already threw (a non-Path was supplied), keep ITS
         * message: replacing it with "opts.dir required" told the caller the
         * option was missing when it was present and of the wrong type. */
        if (JS_IsUndefined(v))
            return JS_ThrowTypeError(ctx, "upload: opts.dir required");
        return JS_EXCEPTION;
    }
    v = JS_GetPropertyStr(ctx, argv[1], "maxFileSize");
    if (!JS_IsUndefined(v)) { if (JS_ToInt64(ctx, &maxf, v)) return JS_EXCEPTION; }
    JS_FreeValue(ctx, v);
    allow = dyn_app_parse_allow(ctx, argv[1], &n_allow);
    handler = JS_DupValue(ctx, argv[2]);

    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) goto ufail;
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_ThrowOutOfMemory(ctx); goto ufail; }
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
    /* borrowed from a Path */
    return JS_UNDEFINED;
ufail:
    for (k = 0; k < n_allow; k++) free(allow[k]);
    free(allow);
    JS_FreeValue(ctx, handler);
    JS_FreeCString(ctx, path);
    /* borrowed from a Path */
    return JS_EXCEPTION;
}

/* sse(path, {open, close}) -- register a server-sent-events endpoint. open
   receives an SseConn; the connection stays open until either side closes. */
static JSValue dyn_app_sse_register(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path;
    dyn_app_route_t *nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "sse(path, {open,close})");
    /*a typo'd handler name used to silently never fire. */
    if (dyn_opts_strict(ctx, argv[1], http_sse_handler_keys, 2))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr)); /* realloc does not zero the new slot */
    nr->path = strdup(path);
    nr->type = APP_SSE;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ws(path, {open, message, close}) -- register a WebSocket endpoint. */
static JSValue dyn_app_ws_register(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_app_t *app;
    const char *path;
    dyn_app_route_t *nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "ws(path, {open,message,close})");
    /*a typo'd handler name used to silently never fire. */
    if (dyn_opts_strict(ctx, argv[1], http_ws_handler_keys, 3))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    nr = (dyn_app_route_t *)realloc(app->routes,
                                    (app->n_routes + 1) * sizeof(*nr));
    if (!nr) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
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

/* The reactor drain, plus the idle sweep. Registered instead of dyn_aio_drain
 * so the sweep runs on every wakeup the IO already causes.
 * LIMITATION: a peer that connects and then sends NOTHING generates no further
 * wakeups, so on a totally idle server it is not swept until something else
 * wakes the loop. A dribbling slowloris -- the attack this defends against --
 * wakes it with every byte. */
/* Runs AFTER the shared reactor drain, on the loop thread. It must not drain
 * the reactor itself -- that already happened, and doing it twice would reap an
 * empty queue on every wakeup. */
static void dyn_app_drain(void *udata)
{
    dyn_app_t *app = (dyn_app_t *)udata;
    if (app->timers)
        dyn_timer_run(app->timers, dyn_timer_now_ms());
}

/*the honest finding: App does NOT embed the thread-pool HTTPServer,
 * so unlike that server it has no request-worker threads to size. App is the
 * reactor model: every handler runs on the ONE loop thread (see the class
 * comment on HTTPServerAsync), and the JS runtime itself is single-threaded,
 * so N extra workers would have nowhere legal to run handlers. The `workers`
 * option therefore exists at the ctor (validated, clamped to HTTPServer's
 * 1..64 range, and readable back through app.workers) so a configuration
 * shared between the two servers does not silently drop the key -- but today
 * it sizes nothing. Multi-core HTTP with this API shape means one process
 * per core sharing the port (the listen socket sets SO_REUSEPORT for exactly
 * that). If handlers ever move off the loop thread, this is the option that
 * grows the pool. */
static JSValue dyn_app_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    (void)argc; (void)argv;
    if (!app) return JS_EXCEPTION;
    if (app->started) return JS_UNDEFINED;
    /*the backlog is finally the caller's choice. The hardcoded 1024
       predates the option and stays the default; the thread-pool HTTPServer
       falls back to SOMAXCONN for an unset backlog, but 1024 is what App has
       always listened with, so 1024 stays what silence means. */
    app->listen_fd = dyn_aio_listen(app->aio,
                                    app->listen_host ? app->listen_host
                                                     : "0.0.0.0",
                                    app->port,
                                    app->backlog > 0 ? app->backlog : 1024);
    if (app->listen_fd < 0)
        return JS_ThrowInternalError(ctx, "App: listen failed");
    /* port 0 binds an ephemeral port, and without this `.port` reports the 0
       that was configured -- so the caller can never learn where it is
       listening. HTTPServer has always done this; App did not. */
    if (app->port == 0) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(app->listen_fd, (struct sockaddr *)&sin, &sl) == 0)
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
    /* The sweep runs as a DRAIN HOOK rather than by owning the reactor: the
       engine has one reactor slot, so installing our own would evict any other
       dyna:net object in this process. */
    {   /* -2 is "the backend cannot arm a clock", which is a different
           problem from running out of hook slots and needs a different fix. */
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

static JSValue dyn_app_get_port(JSContext *ctx, JSValueConst this_val)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->port);
}

/*the ctor's parallelism configuration, readable back so a probe (or
   a config dashboard) can see what the server was constructed with instead
   of trusting its own bookkeeping. Defaults, not 0s: the numbers answer
   "what will/would this server run with", and before the first start() the
   backlog answer is the 1024 the listen call will use. */
static JSValue dyn_app_get_workers(JSContext *ctx, JSValueConst this_val)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->workers ? app->workers : 1);
}

static JSValue dyn_app_get_backlog(JSContext *ctx, JSValueConst this_val)
{
    dyn_app_t *app = (dyn_app_t *)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->backlog ? app->backlog : 1024);
}

/* ---- WebSocket client (RFC 6455) --------------------------------------
 * new WsClient("ws://host:port/path", {open, message, close})
 *
 * The connect and the upgrade handshake run OFF THE EVENT LOOP on the io
 * pool -- one DNS lookup, one connect, one round trip -- and `open` fires
 * when the 101 is verified. A blocking handshake would stall the single
 * thread for the whole round trip, which is the one thing a runtime this
 * shape cannot afford. Scheme and argument errors still throw from the
 * constructor; handshake and network failures surface as close(1006,
 * reason) because there is no error event. wss:// needs a TLS engine
 * framed into the reactor for clients, which does not exist yet: it
 * throws rather than silently downgrading.
 */
typedef struct wsc_hs wsc_hs_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;         /* acquired at construction, released at teardown */
    int fd;                 /* -1 until the handshake completes */
    JSValue handlers;       /* {open,message,close} (dup) */
    JSValue self;           /* strong ref while connected: the reactor's
                               callbacks must never point at a dead object */
    dyn_iobuf_t in;
    dyn_iobuf_t frag;
    int frag_op, frag_frames, ctl_budget;
    int closed;
    int close_sent;
    wsc_hs_t *hs;           /* non-NULL while the handshake job is in flight */
    int dispose_deferred;   /* dispose ran during the handshake: the
                               completion owns the final free */
    int release_deferred;   /* close ran during the handshake: the async-hook
                               reactor release defers to the completion, or
                               the channel is freed under the in-flight job
                               and the pool drops its completion */
} dyn_wsc_t;

/* The handshake exchange, carried by the pool job. No JSContext, no JSValue:
   the worker only reads its own fields. */
struct wsc_hs {
    dyn_wsc_t *w;
    char *url;              /* for the failure reason (freed by the completion) */
    dyn_url_t u;            /* u.path borrows http_url */
    char *http_url;
    char key_b64[32], accept[32];
    dyn_bytes_t req;
    int fd;                 /* the connected socket: job-owned until done */
    char head[16384];       /* handshake response head */
    size_t got, hdr_end;
    int err;                /* a DYN_HTTP_ERR_* code, 0 on ok */
#ifdef CONFIG_TLS
    dyn_tls_conn_t *tls;    /* wss://: the engine, handshake completed in the
                               pool job; attached to the aio fd at completion
                               so reactor reads decrypt transparently */
    dyn_tls_ctx_t *tls_ctx; /* job-owned ctx (freed by the completion) */
    char tls_err[192];
#endif
};

static JSClassID dyn_wsc_class_id;

static void dyn_wsc_close_internal(dyn_wsc_t *w, int code, const char *reason);

/* Send a client frame (FIN=1, MASKED -- a server is required to refuse an
   unmasked client frame, and ours does). */
static void dyn_wsc_send_frame(dyn_wsc_t *w, int opcode, const uint8_t *data,
                               size_t len)
{
    dyn_iobuf_t f;
    uint8_t h[10], mask[4];
    size_t hn, i, off;

    if (w->closed)
        return;
    /* Symmetric bound: a SERVER that never reads must not grow our queue.
       Full teardown, not a silent flag: close_internal is what drops the
       self-ref and counts the reactor release. */
    if (dyn_aio_queued(w->aio, w->fd) + len + 16 > DYN_HTTP_OUTBOUND_MAX) {
        dyn_wsc_close_internal(w, 1008, "output backpressure");
        return;
    }
    if (dyn_os_entropy(mask, sizeof mask) < 0) {
        /* RFC 6455: a client frame MUST be masked; an unmasked one is not
         * sendable, so the honest failure is to drop the frame and close. */
        w->closed = 1;
        return;
    }
    dyn_iobuf_init(&f);
    h[0] = 0x80 | (uint8_t)(opcode & 0x0f);
    if (len < 126) { h[1] = 0x80 | (uint8_t)len; hn = 2; }
    else if (len <= 0xffff) { h[1] = 0x80 | 126; h[2] = (len >> 8) & 0xff;
                              h[3] = len & 0xff; hn = 4; }
    else { h[1] = 0x80 | 127;
           for (i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
           hn = 10; }
    dyn_iobuf_append(&f, h, hn);
    dyn_iobuf_append(&f, mask, 4);
    off = f.len;
    if (len) {
        /* mask in place after appending: the key repeats every 4 bytes, so
           one 64-bit key serves every 8-byte block (same trick as the
           server's unmask, and likewise endian-independent). */
        uint8_t *p;
        uint32_t m32;
        uint64_t m64;
        size_t k = 0, n8 = len & ~(size_t)7;
        dyn_iobuf_append(&f, data, len);
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

static void dyn_wsc_dispatch_msg(dyn_wsc_t *w, int opcode,
                                 const uint8_t *payload, size_t plen)
{
    JSContext *ctx = w->ctx;
    JSValue mh = JS_GetPropertyStr(ctx, w->handlers, "message");
    if (opcode != 2 && simd.validate_utf8
        && simd.validate_utf8(payload, plen) != plen) {
        /* RFC 6455 8.1.1: fail the connection on invalid UTF-8 text. */
        JS_FreeValue(ctx, mh);
        dyn_wsc_close_internal(w, 1007, "invalid UTF-8 in text message");
        return;
    }
    if (JS_IsFunction(ctx, mh)) {
        JSValue data = (opcode == 2)
            ? JS_NewArrayBufferCopy(ctx, payload, plen)
            : JS_NewStringLen(ctx, (const char *)payload, plen);
        JSValueConst args[3] = { w->self, data, JS_NewBool(ctx, opcode == 2) };
        dyn_call_drop(ctx, mh, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, mh);
}

/* Parse and dispatch every complete frame buffered in w->in. Client rules:
   a server frame MUST NOT be masked (RFC 6455 5.1), control frames carry at
   most 125 octets and are never fragmented (5.5). */
static void dyn_wsc_process(dyn_wsc_t *w)
{
    if (w->closed)
        return;
    for (;;) {
        uint8_t *p = dyn_iobuf_rdata(&w->in);
        size_t avail = dyn_iobuf_rlen(&w->in);
        int fin, opcode, masked;
        uint64_t plen;
        size_t hdr, i, frame_total;
        uint8_t *payload;

        if (avail < 2)
            return;
        fin = p[0] & 0x80;
        opcode = p[0] & 0x0f;
        masked = p[1] & 0x80;
        plen = p[1] & 0x7f;
        hdr = 2;
        if (plen == 126) {
            if (avail < 4) return;
            plen = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (plen == 127) {
            if (avail < 10) return;
            plen = 0;
            for (i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
            hdr = 10;
        }
        if (masked || (opcode >= 0x8 && (plen > 125 || !fin))) {
            dyn_wsc_close_internal(w, 1002, "protocol error");
            return;
        }
        if (plen > DYN_ACONN_MAX_REQ) { /* cap BEFORE the size math */
            dyn_wsc_close_internal(w, 1009, "too big");
            return;
        }
        if (avail < hdr + plen)
            return;                     /* need the full frame */
        payload = p + hdr;
        frame_total = hdr + (size_t)plen;

        if (opcode == 0x8) {                /* close */
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
        } else if (opcode == 0x9) {         /* ping -> pong */
            if (--w->ctl_budget < 0) {
                dyn_wsc_close_internal(w, 1002, "control flood");
                return;
            }
            dyn_wsc_send_frame(w, 10, payload, (size_t)plen);
        } else if (opcode == 0xA) {         /* pong: ignore */
        } else if (opcode <= 0x2) {
            int frag_active = w->frag_op != 0;
            /* protocol errors: stray continuation, or a new message
               mid-fragment */
            if ((opcode == 0x0 && !frag_active)
                || (opcode != 0x0 && frag_active)) {
                dyn_wsc_close_internal(w, 1002, "protocol error");
                return;
            }
            if (opcode != 0x0 && fin) {
                w->ctl_budget = DYN_WS_CTL_BUDGET;
                dyn_wsc_dispatch_msg(w, opcode, payload, (size_t)plen);
            } else { /* fragmented: accumulate, dispatch on FIN */
                if (opcode != 0x0) { w->frag_op = opcode; w->frag_frames = 0; }
                if (++w->frag_frames > DYN_WS_MAX_FRAGMENTS ||
                    dyn_iobuf_rlen(&w->frag) + plen > DYN_ACONN_MAX_REQ ||
                    dyn_iobuf_append(&w->frag, payload, (size_t)plen) < 0) {
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
        } /* other opcodes: ignore */
        dyn_iobuf_consume(&w->in, frame_total);
        if (w->closed)
            return;
    }
}

static void dyn_wsc_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                            unsigned len, void *ud)
{
    dyn_wsc_t *w = (dyn_wsc_t *)ud;
    (void)aio;
    if (res <= 0) {
        dyn_wsc_close_internal(w, 1006, "abnormal closure");
        return;
    }
    if (dyn_iobuf_append(&w->in, buf, len) < 0
        || dyn_iobuf_rlen(&w->in) > DYN_ACONN_MAX_REQ) {
        dyn_wsc_close_internal(w, 1009, "too big");
        return;
    }
    dyn_wsc_process(w);
}

/* Idempotent teardown: disarm the fd, fire close once, drop the self-ref and
   the handlers, and count the reactor release for the post-drain reaper
   (this can run INSIDE a drain callback, where freeing the reactor is a
   use-after-free). During the handshake the fd belongs to the job; -1 here
   means "nothing to disarm". */
static void dyn_wsc_close_internal(dyn_wsc_t *w, int code, const char *reason)
{
    JSContext *ctx;
    JSValue ch;

    if (w->closed)
        return;
    w->closed = 1;
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
        /* The handshake job is still carrying its completion through this
           reactor's pool channel. Releasing the async hook now lets the
           reap free the reactor -- and the channel -- under the job, and
           the pool's closing branch then drops the completion: job,
           handshake state and client all strand. Defer the release to
           wsc_hs_done, next to dispose's deferred free. */
        w->release_deferred = 1;
    } else {
        dyn_http_async_release(w->ctx);
    }
}

static JSValue dyn_wsc_send(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_wsc_t *w = (dyn_wsc_t *)dyn_res_native(ctx, this_val,
                                               dyn_wsc_class_id);
    const char *str = NULL;
    uint8_t *abuf = NULL;
    size_t len = 0;
    int binary = 0;
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_UNDEFINED;
    /* coerce data FIRST (may run JS), THEN check the (possibly closed) conn */
    if (JS_IsString(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) return JS_EXCEPTION;
    } else {
        abuf = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (abuf) binary = 1;
        else { str = JS_ToCStringLen(ctx, &len, argv[0]); if (!str) return JS_EXCEPTION; }
    }
    if (!w->closed)
        dyn_wsc_send_frame(w, binary ? 2 : 1,
                           str ? (const uint8_t *)str : abuf, len);
    if (str) JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_wsc_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_wsc_send),
};

/* dyn_res_wrap dispose: normally unreachable before teardown, because the
   self-ref pins the object while connected -- but a dropped reference must
   not leak the fd or the reactor ref either. The close frame first is the
   polite teardown. If the handshake job is still in flight, the free is
   deferred to its completion (dispose_deferred), which reads w exactly
   once after this. */
static void dyn_wsc_dispose(void *native)
{
    dyn_wsc_t *w = (dyn_wsc_t *)native;
    if (!w)
        return;
    if (!w->closed) {
        if (!w->close_sent && w->fd >= 0) {
            w->close_sent = 1;
            dyn_wsc_send_frame(w, 8, NULL, 0);
        }
        dyn_wsc_close_internal(w, 1000, "");
    }
    if (!w->hs) {
        dyn_iobuf_free(&w->in);
        dyn_iobuf_free(&w->frag);
        free(w);
    } else {
        w->dispose_deferred = 1;
    }
}

/* The blocking half of the handshake, on the pool worker: DNS + connect +
   send the upgrade request + read the head. Touches nothing but `hs` and
   calls no JS_* function. The request is a plain HTTP GET with the upgrade
   headers and a verification of the accept key against the one we sent --
   anything less accepts a response from a server that never saw the request. */
static void wsc_hs_work(void *arg)
{
    wsc_hs_t *hs = (wsc_hs_t *)arg;
    uint8_t key_raw[16];
    char hhost[320];
    size_t klen;
    int err = 0;

    if (dyn_os_entropy(key_raw, sizeof key_raw) < 0) {
        hs->err = -1;   /* sentinel: entropy unavailable, abort the handshake */
        return;
    }
    klen = dyn_codec_base64_encode(key_raw, sizeof key_raw, hs->key_b64);
    hs->key_b64[klen] = '\0';

    hs->fd = dyn_tcp_connect(hs->u.host, hs->u.port,
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
        /* wss://: blocking TLS handshake on the pool thread. Steady-state
           TLS is the AIO layer's (dyn_aio_tls_attach at completion). */
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
        || dyn_bytes_append(&hs->req, hs->u.path, strlen(hs->u.path)) < 0
        || dyn_bytes_append(&hs->req, " HTTP/1.1\r\nHost: ", 17) < 0
        || dyn_bytes_append(&hs->req, hhost, strlen(hhost)) < 0
        || dyn_bytes_append(&hs->req, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: ", 62) < 0
        || dyn_bytes_append(&hs->req, hs->key_b64, klen) < 0
        || dyn_bytes_append(&hs->req, "\r\nSec-WebSocket-Version: 13\r\n\r\n", 31) < 0) {
        hs->err = DYN_HTTP_ERR_OOM;
        return;
    }
#ifdef CONFIG_TLS
    if (hs->tls) {
        /* wss://: the request goes through the engine, ciphertext to the wire */
        uint8_t ob[16384];
        size_t off = 0;
        int n;
        while (off < hs->req.len) {
            n = dyn_tls_write(hs->tls, (const uint8_t *)hs->req.data + off,
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

    /* Read the head, bounded: anything past CRLFCRLF is already WS frames
       and is preserved for the parser. */
    for (;;) {
        const char *m;
        ssize_t r;
        if (hs->got >= sizeof hs->head) {
            hs->err = -1;       /* sentinel: headers too large */
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

/* LOOP THREAD: verify the 101 and the accept key, arm the reactor, fire
   `open` -- or fire close(1006, reason) on any failure. Reads w exactly
   once; if the user closed during the handshake, this only frees. */
static void wsc_hs_done(void *arg)
{
    wsc_hs_t *hs = (wsc_hs_t *)arg;
    dyn_wsc_t *w = hs->w;
    JSContext *ctx = w->ctx;
    char why[384];
    int fail = 1, code = 1006;
    /* Snapshot BEFORE anything runs: the failure branch below may free w,
       and the deferred-free tail must not read it afterwards. */
    int deferred = w->dispose_deferred;
    int rel_deferred = w->release_deferred;
    w->release_deferred = 0;

    w->hs = NULL;
    why[0] = 0;
    if (hs->err == 0) {
        const char *sp = memchr(hs->head, ' ', hs->hdr_end);
        const char *acc;
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
                    /* An accept key that does not match OUR key is a
                       response to somebody else's request, not to ours. */
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
        {   int fl = fcntl(hs->fd, F_GETFL, 0);
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
        /* From here the AIO layer owns TLS read/write on this fd; the
           engine pointer moves to the slot, so do NOT free it below. */
#endif
        dyn_aio_recv(w->aio, hs->fd, 0, /*multishot=*/1, dyn_wsc_on_recv, w);
        oh = JS_GetPropertyStr(ctx, w->handlers, "open");
        if (JS_IsFunction(ctx, oh)) {
            JSValueConst a[1] = { w->self };
            dyn_call_drop(ctx, oh, JS_UNDEFINED, 1, a);
        }
        JS_FreeValue(ctx, oh);
        dyn_wsc_process(w);     /* frames that arrived with the 101 */
    } else {
        if (hs->fd >= 0)
            close(hs->fd);
        if (!w->closed) {
            /* Tear down with the real reason. Mark FIRST: close_internal
               frees self, which can free the object, whose finalizer must
               not run dispose over a freed w. */
            dyn_res_mark_closed(JS_GetRuntime(ctx), w->self, dyn_wsc_class_id);
            dyn_wsc_close_internal(w, code, why);
            dyn_iobuf_free(&w->in);
            dyn_iobuf_free(&w->frag);
            free(w);
        }
    }
    free(hs->req.data);
    free(hs->http_url);
    free(hs->url);
#ifdef CONFIG_TLS
    if (hs->tls_ctx)
        dyn_tls_ctx_free(hs->tls_ctx);
    if (fail && hs->tls)
        dyn_tls_conn_free(hs->tls);   /* success path handed it to the aio */
#endif
    free(hs);
    if (deferred) {
        /* The user closed during the handshake: dispose deferred the free
           to this point. */
        dyn_iobuf_free(&w->in);
        dyn_iobuf_free(&w->frag);
        free(w);
    }
    if (rel_deferred) {
        /* close() ran during the handshake: the async-hook reactor release
           was deferred to here, now that the job's completion has run and
           the channel is no longer needed by this client. `ctx` (not w->ctx):
           the deferred branch above may already have freed w. */
        dyn_http_async_release(ctx);
    }
}

/* Argument and scheme errors THROW; the handshake itself settles later on
   the reactor, and its failures surface as close(1006, reason). Every
   fallible step happens BEFORE dyn_res_wrap so the fail label never has to
   tear down a half-wrapped resource. */
static JSValue dyn_wsc_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_wsc_t *w = NULL;
    wsc_hs_t *hs = NULL;
    const char *url = NULL;
    dyn_url_t u;
    JSValue obj;
    int is_wss = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "new WsClient(url, {open,message,close})");
    /*reject unknown handler names BEFORE any connect work; a typo'd
     * handler used to silently never fire. */
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

    w = (dyn_wsc_t *)calloc(1, sizeof(*w));
    if (!w)
        goto oom;
    w->ctx = ctx;
    w->fd = -1;
    w->aio = dyn_net_reactor_acquire(ctx);
    if (!w->aio) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    w->handlers = JS_DupValue(ctx, argv[1]);
    w->ctl_budget = DYN_WS_CTL_BUDGET;
    dyn_iobuf_init(&w->in);
    dyn_iobuf_init(&w->frag);

    hs = (wsc_hs_t *)calloc(1, sizeof(*hs));
    if (!hs)
        goto oom;
    hs->w = w;
    hs->url = strdup(url);
    hs->http_url = (char *)malloc(strlen(url) + 3);
    if (!hs->url || !hs->http_url)
        goto oom;
    /* dyn_parse_url speaks http(s): rewrite the scheme and reuse it whole.
       u.path then borrows http_url, which outlives the handshake. */
    /* wss:// parses as https:// (same structure, TLS implied) */
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
        memset(&to, 0, sizeof to);      /* insecure=0: certificates verified */
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
        hs->tls = dyn_tls_conn_new(hs->tls_ctx, u.host, hs->tls_err,
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
            u.port = 443;               /* wss default */
        hs->u.port = u.port;
    }
#endif

    /* Armed before the first teardown can count a release. */
    dyn_http_async_hook(ctx);
    obj = dyn_res_wrap(ctx, new_target, dyn_wsc_class_id, w, dyn_wsc_dispose);
    if (JS_IsException(obj)) {
        w = NULL;               /* dyn_res_wrap already disposed it */
        goto fail;
    }
    w->self = JS_DupValue(ctx, obj);
    w->hs = hs;                 /* set BEFORE the offload: its inline fallback
                                   runs the completion synchronously */
    dyn_aio_offload(w->aio, wsc_hs_work, wsc_hs_done, hs);
    hs = NULL;                  /* ownership moved to the job */

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
    if (w) {                    /* pre-wrap only: raw teardown, no JS object */
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
    "WsClient", .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_app_proto[] = {
    JS_CFUNC_DEF("rpc", 2, dyn_app_rpc),
    JS_CFUNC_DEF("static", 2, dyn_app_static),
    JS_CFUNC_DEF("proxy", 2, dyn_app_proxy),
    JS_CFUNC_DEF("upload", 3, dyn_app_upload),
    JS_CFUNC_DEF("ws", 2, dyn_app_ws_register),
    JS_CFUNC_DEF("sse", 2, dyn_app_sse_register),
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

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */

/* Register one public class, re-entrantly. Both dyna:net and dyna:http call
 * dyn_http_register, and this fork's JS_NewClassID REUSES a stored id, so the
 * second registration on one runtime fails. A failed JS_NewClass therefore
 * means "already registered HERE": re-export the ctor from the existing proto
 * (net.App === http.App). On a fresh runtime the stale id is out of range and
 * JS_NewClass grows the arrays, so workers keep working. */
static int dyn_http_register_one(JSContext *ctx, JSModuleDef *m,
                                 JSClassID *pid, const JSClassDef *def,
                                 const JSCFunctionListEntry *proto_funcs,
                                 int n_funcs, JSCFunction *ctor_fn,
                                 const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
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

/* Register one internal (non-constructible) class, re-entrantly. */
static int dyn_http_register_internal(JSContext *ctx, JSClassID *pid,
                                      const JSClassDef *def,
                                      const JSCFunctionListEntry *proto_funcs,
                                      int n_funcs)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return 0; /* already registered on this runtime */
    {
        JSValue wp = JS_NewObject(ctx);
        if (JS_IsException(wp))
            return -1;
        JS_SetPropertyFunctionList(ctx, wp, proto_funcs, n_funcs);
        JS_SetClassProto(ctx, *pid, wp);
    }
    return 0;
}

int dyn_http_register(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_httpmsg_register(ctx, m) < 0)
        return -1;
    /* Install the SIMD dispatch table on the JS thread before any acceptor/
     * worker thread spawns; dyn_memfind reads it lock-free thereafter. */
    simd_init();
    if (dyn_http_register_one(ctx, m, &dyn_http_client_class_id,
                              &dyn_http_client_class, dyn_http_client_proto,
                              countof(dyn_http_client_proto),
                              dyn_http_client_ctor, "HTTPClient") < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_http_server_class_id,
                              &dyn_http_server_class, dyn_http_server_proto,
                              countof(dyn_http_server_proto),
                              dyn_http_server_ctor, "HTTPServer") < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_http_async_class_id,
                              &dyn_http_async_class, dyn_http_async_proto,
                              countof(dyn_http_async_proto),
                              dyn_http_async_ctor, "HTTPServerAsync") < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_app_class_id,
                              &dyn_app_class, dyn_app_proto,
                              countof(dyn_app_proto),
                              dyn_app_ctor, "App") < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_wsc_class_id,
                              &dyn_wsc_class, dyn_wsc_proto,
                              countof(dyn_wsc_proto),
                              dyn_wsc_ctor, "WsClient") < 0)
        return -1;
    /*the HTTPBodyStream resource is factory-built (getStream), never
     * user-constructed; ids/protos survive re-entry, and the close surface
     * comes from the resource framework (dyna:stream's factory-class shape). */
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
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
    /* WsConn/SseConn: internal classes (created at handshake, not
     * user-constructible); re-entry is a no-op. */
    if (dyn_http_register_internal(ctx, &dyn_ws_class_id, &dyn_ws_class,
                                   dyn_ws_proto, countof(dyn_ws_proto)) < 0)
        return -1;
    if (dyn_http_register_internal(ctx, &dyn_sse_class_id, &dyn_sse_class,
                                   dyn_sse_proto, countof(dyn_sse_proto)) < 0)
        return -1;
    /* PendingResponse: the lifetime holder for a parked handler thenable's
     * settle closures (never user-visible); re-entry is a no-op. */
    if (dyn_http_register_internal(ctx, &dyn_pend_class_id, &dyn_pend_class,
                                   NULL, 0) < 0)
        return -1;
    {
        static const char *const fetch_names[] = {
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

void dyn_http_add_exports(JSContext *ctx, JSModuleDef *m)
{
    static const char *const fetch_names[] = {
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

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
