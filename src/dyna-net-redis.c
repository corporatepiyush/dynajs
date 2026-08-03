/*
 * Redis client, part of dyna:net. Runs entirely on the JS thread through the
 * shared dyn_aio reactor -- a request/response protocol on a ready socket is
 * not work to hand another thread (NET_PLAN.md 2.1).
 *
 * Every command returns a Promise. Replies arrive in the order commands were
 * sent, so the pending list is a strict FIFO: nothing here ever tries to
 * RESYNC after a protocol error, because guessing which reply belongs to which
 * command is how a client silently returns one key's value for another. A
 * malformed reply tears the connection down and rejects everything on it.
 *
 *   const r = new Redis({ host: "127.0.0.1", port: 6379 });
 *   await r.command("SET", "k", "v");
 *   const v = await r.command("GET", "k");
 *   r.close();
 *
 * NO TLS. A `rediss://` endpoint is refused by name rather than downgraded to
 * plaintext, and a peer that answers with a TLS record is reported as such
 * instead of as a protocol error.
 */
#include "dyna-nat.h"
#include "dyna-aio.h"
#include "core/dyn-resp.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define RD_ST_CONNECTING 0
#define RD_ST_HANDSHAKE  1
#define RD_ST_READY      2
#define RD_ST_DEAD       3

/* An internal command settles inside the client and has no JS promise. */
#define RD_INT_NONE   0
#define RD_INT_HELLO  1
#define RD_INT_AUTH   2
#define RD_INT_SELECT 3

#define RD_DEFAULT_MAXBULK   (64u * 1024u * 1024u)
#define RD_DEFAULT_PENDING   4096
#define RD_CONNECT_TIMEOUT   10000

typedef struct dyn_redis_pending {
    struct dyn_redis_pending *next;
    JSValue resolve, reject;
    JSValue acc;            /* pipeline accumulator; JS_UNDEFINED otherwise */
    uint8_t *bytes;         /* encoded command, only while queued behind the
                             * handshake; NULL once written */
    size_t nbytes;
    int internal;           /* RD_INT_* */
    int want;               /* replies this entry consumes (pipeline depth) */
    int got;
    uint64_t deadline_ms;   /* 0 = no command timeout */
} dyn_redis_pending_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd;
    int state;
    int proto;              /* 2 or 3, decided by the HELLO reply */
    int binary;             /* bulk replies as Uint8Array rather than string */
    int bigint;             /* 64-bit ints and bignums as BigInt, not text */
    int hooked;             /* a drain hook is armed */
    char *host, *path, *user, *pass;
    uint16_t port;
    int db;
    size_t maxbulk;
    int maxpending;
    uint64_t connect_deadline_ms;
    uint64_t command_timeout_ms;

    uint8_t *rbuf;  size_t rcap, rlen, rpos;
    uint8_t *obuf;  size_t ocap, olen;

    dyn_redis_pending_t *head, *tail;   /* on the wire, in reply order */
    dyn_redis_pending_t *wq_head, *wq_tail;  /* issued before READY */
    int npending, nwait;   /* on the wire, and issued but not yet armed */
    int flush_queued;      /* a coalescing flush job is pending */
    int subscribed;
    JSValue h_push, h_error;
} dyn_redis_t;

static JSClassID dyn_redis_class_id;

/* Clients with bytes waiting for the end-of-turn flush. Bounded and compacted:
 * a client is removed on flush and on dispose, so a closed client is never
 * reached through it. */
#define RD_MAX_FLUSH 256
static _Thread_local dyn_redis_t *net_flush_pending[RD_MAX_FLUSH];
static _Thread_local int net_n_flush;

static void redis_fail_all(dyn_redis_t *r, const char *msg);
static void redis_tick(void *udata);

/* ---- small buffers ------------------------------------------------------ */

/* Growth decays with size: 2x is free while small and overshoots in absolute
 * bytes once large (CLAUDE.md 9). */
static int buf_reserve(uint8_t **p, size_t *cap, size_t need)
{
    size_t c = *cap;
    uint8_t *n;
    if (need <= c)
        return 0;
    if (c == 0)
        c = 512;
    while (c < need)
        c = c < (1u << 20) ? c * 2 : c + (c / 4);
    n = (uint8_t *)realloc(*p, c);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

/* ---- pending queue ------------------------------------------------------ */

static dyn_redis_pending_t *pend_new(JSContext *ctx)
{
    dyn_redis_pending_t *p = (dyn_redis_pending_t *)calloc(1, sizeof(*p));
    (void)ctx;
    if (!p)
        return NULL;
    p->resolve = p->reject = p->acc = JS_UNDEFINED;
    p->want = 1;
    return p;
}

static void pend_free(JSContext *ctx, dyn_redis_pending_t *p)
{
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->acc);
    free(p->bytes);
    free(p);
}

static void pend_push(dyn_redis_pending_t **head, dyn_redis_pending_t **tail,
                      dyn_redis_pending_t *p)
{
    p->next = NULL;
    if (*tail) (*tail)->next = p; else *head = p;
    *tail = p;
}

static dyn_redis_pending_t *pend_pop(dyn_redis_pending_t **head,
                                     dyn_redis_pending_t **tail)
{
    dyn_redis_pending_t *p = *head;
    if (!p)
        return NULL;
    *head = p->next;
    if (!*head)
        *tail = NULL;
    p->next = NULL;
    return p;
}

/* ---- settling ----------------------------------------------------------- */

static void redis_settle(JSContext *ctx, dyn_redis_pending_t *p, int reject,
                         JSValue v)
{
    JSValue fn = reject ? p->reject : p->resolve;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { v };
        JSValue res = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, v);
    pend_free(ctx, p);
}

/* The first token of a Redis error is its class (ERR, NOAUTH, WRONGPASS,
 * MOVED, LOADING...). Surfacing it as `.code` is what lets a caller branch
 * without parsing the message text. */
static JSValue redis_error_value(JSContext *ctx, const char *text, size_t len)
{
    JSValue e = JS_NewError(ctx);
    size_t i = 0;
    while (i < len && text[i] != ' ')
        i++;
    JS_SetPropertyStr(ctx, e, "message", JS_NewStringLen(ctx, text, len));
    JS_SetPropertyStr(ctx, e, "code", JS_NewStringLen(ctx, text, i));
    JS_SetPropertyStr(ctx, e, "redis", JS_TRUE);
    return e;
}

static JSValue redis_conn_error(JSContext *ctx, const char *msg)
{
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "CONNECTION"));
    return e;
}

/* ---- RESP -> JS --------------------------------------------------------- */

static JSValue redis_bytes(JSContext *ctx, const uint8_t *p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), ta;
    JSValueConst a3[3];
    if (JS_IsException(ab))
        return ab;
    /* three arguments: with one, the view's length defaults to 0 */
    a3[0] = ab; a3[1] = JS_NewInt32(ctx, 0); a3[2] = JS_NewInt32(ctx, (int)n);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

/* Arbitrary-precision decimal text as a BigInt, via the global constructor --
 * there is no C entry point wider than int64. Falls back to the text on
 * anything BigInt() refuses, so a malformed reply is visible, not an
 * exception from a getter the caller never called. */
static JSValue redis_bignum(JSContext *ctx, const char *p, size_t n)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "BigInt");
    JSValue arg = JS_NewStringLen(ctx, p, n), out;
    JSValueConst a1[1];

    JS_FreeValue(ctx, global);
    if (JS_IsException(arg) || !JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return JS_IsException(arg) ? arg : JS_NewStringLen(ctx, p, n);
    }
    a1[0] = arg;
    out = JS_Call(ctx, ctor, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return arg;                       /* not a number: hand back the text */
    }
    JS_FreeValue(ctx, arg);
    return out;
}

/* Depth is bounded by dyn_resp_scan before this runs, so the recursion here
 * cannot be driven past DYN_RESP_MAX_DEPTH by a peer. */
static JSValue redis_value(JSContext *ctx, dyn_redis_t *r,
                           dyn_resp_reader_t *rd, int *is_err)
{
    dyn_resp_item_t it;
    int rc = dyn_resp_next(rd, &it);

    if (rc != DYN_RESP_OK)
        return JS_ThrowInternalError(ctx, "Redis: %s", dyn_resp_strerror(rc));

    switch (it.type) {
    case DYN_RESP_SIMPLE:
        return JS_NewStringLen(ctx, (const char *)it.str, it.slen);
    case DYN_RESP_ERROR:
    case DYN_RESP_BLOBERR:
        if (is_err) *is_err = 1;
        return redis_error_value(ctx, (const char *)it.str, it.slen);
    case DYN_RESP_INT:
        /* JS_NewInt64 becomes a double above int32, so past 2^53 it ROUNDS --
         * and a 64-bit counter is an ordinary Redis reply. `bigint` returns
         * the exact value with its type, otherwise the digits stay text. */
        if (r->bigint)
            return JS_NewBigInt64(ctx, it.ival);
        if (it.ival > 9007199254740992LL || it.ival < -9007199254740992LL)
            return JS_NewStringLen(ctx, (const char *)it.str, it.slen);
        return JS_NewInt64(ctx, it.ival);
    case DYN_RESP_NULL:
        return JS_NULL;
    case DYN_RESP_BOOL:
        return JS_NewBool(ctx, (int)it.ival);
    case DYN_RESP_DOUBLE:
        return JS_NewFloat64(ctx, it.dval);
    case DYN_RESP_BIGNUM:
        /* A RESP3 big number is by definition one int64 cannot hold, so
         * JS_NewBigInt64 is the wrong tool: the global BigInt() is the only
         * arbitrary-precision entry point. Without `bigint` it stays text. */
        if (r->bigint)
            return redis_bignum(ctx, (const char *)it.str, it.slen);
        return JS_NewStringLen(ctx, (const char *)it.str, it.slen);
    case DYN_RESP_BULK:
    case DYN_RESP_VERB: {
        const uint8_t *p = it.str;
        size_t n = it.slen;
        if (it.isnull)
            return JS_NULL;
        /* "=<len>\r\n<3-byte enc>:<data>": len COVERS the prefix, which is a
         * display hint. INFO is a verbatim string under RESP3, so leaving it
         * on makes every line-splitting parser wrong on its first line. */
        if (it.type == DYN_RESP_VERB && n >= 4 && p[3] == ':') { p += 4; n -= 4; }
        if (r->binary)
            return redis_bytes(ctx, p, n);
        return JS_NewStringLen(ctx, (const char *)p, n);
    }
    case DYN_RESP_ARRAY:
    case DYN_RESP_SET:
    case DYN_RESP_PUSH: {
        JSValue arr;
        int64_t i;
        if (it.isnull)
            return JS_NULL;
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            return arr;
        for (i = 0; i < it.count; i++) {
            JSValue v = redis_value(ctx, r, rd, is_err);
            if (JS_IsException(v)) { JS_FreeValue(ctx, arr); return v; }
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, v);
        }
        return arr;
    }
    case DYN_RESP_ATTR: {
        /* Metadata ATTACHED to the value that follows. Returning it would
         * settle the promise with the decoration and leave the real reply
         * unread. Consume its 2n values, then read what it decorated. */
        int64_t i;
        for (i = 0; i < it.count * 2; i++) {
            JSValue skip = redis_value(ctx, r, rd, is_err);
            if (JS_IsException(skip))
                return skip;
            JS_FreeValue(ctx, skip);
        }
        return redis_value(ctx, r, rd, is_err);
    }
    case DYN_RESP_MAP: {
        JSValue obj;
        int64_t i;
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return obj;
        for (i = 0; i < it.count; i++) {
            JSValue k, v, kstr;
            const char *ks;
            int saved = r->binary;
            /* A property key is text whatever `binary` says: coercing a
             * Uint8Array here would give "109,97,120". */
            r->binary = 0;
            k = redis_value(ctx, r, rd, is_err);
            r->binary = saved;
            if (JS_IsException(k)) { JS_FreeValue(ctx, obj); return k; }
            v = redis_value(ctx, r, rd, is_err);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, k); JS_FreeValue(ctx, obj); return v;
            }
            kstr = JS_ToString(ctx, k);
            JS_FreeValue(ctx, k);
            ks = JS_ToCString(ctx, kstr);
            if (ks) {
                /* DEFINE, not set: JS_SetPropertyStr walks the prototype
                 * chain, so a key of "__proto__" would retarget the object's
                 * prototype and the key would vanish from the result. */
                JS_DefinePropertyValueStr(ctx, obj, ks, v, JS_PROP_C_W_E);
                JS_FreeCString(ctx, ks);
            } else {
                JS_FreeValue(ctx, v);
            }
            JS_FreeValue(ctx, kstr);
        }
        return obj;
    }
    default:
        return JS_ThrowInternalError(ctx, "Redis: unexpected type '%c'",
                                     it.type);
    }
}

/* ---- writing ------------------------------------------------------------ */

/* MEASURED: 2000 commands issued in one turn cost 2000 send() calls when each
 * flushes itself, and 1 when they coalesce -- the same syscall count a pipeline
 * gets, without the caller restructuring. The job runs at the end of the
 * current turn, before the loop can poll, so nothing waits longer for a reply. */
static JSValue redis_flush_job(JSContext *ctx, int argc, JSValueConst *argv);

static int redis_flush(dyn_redis_t *r)
{
    int rc;
    if (r->olen == 0 || r->state == RD_ST_CONNECTING || r->state == RD_ST_DEAD)
        return 0;
    rc = dyn_aio_send(r->aio, r->fd, r->obuf, r->olen, 0, NULL, NULL);
    if (rc < 0)
        return rc;               /* keep the bytes: they were not accepted */
    r->olen = 0;                 /* the adapter copies what it cannot send now */
    return 0;
}

/* Ask for one flush at the end of this turn. The client is kept alive by its
 * own JS object for as long as the job can run: the job only ever touches `r`
 * through the pointer it was given, and dispose cancels by clearing the flag
 * and the job then finds a dead client. */
static void redis_flush_soon(dyn_redis_t *r)
{
    if (r->flush_queued || r->olen == 0 || r->state != RD_ST_READY)
        return;
    if (net_n_flush >= RD_MAX_FLUSH) {           /* no room: send it now */
        if (redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    if (JS_EnqueueJob(r->ctx, redis_flush_job, 0, NULL) < 0) {
        if (redis_flush(r) < 0)                  /* no job queue: send now */
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    r->flush_queued = 1;
    net_flush_pending[net_n_flush++] = r;
}

static int redis_write(dyn_redis_t *r, const uint8_t *b, size_t n)
{
    if (buf_reserve(&r->obuf, &r->ocap, r->olen + n) < 0)
        return -1;
    memcpy(r->obuf + r->olen, b, n);
    r->olen += n;
    return 0;
}

static void redis_flush_drop(dyn_redis_t *r)
{
    int i;
    r->flush_queued = 0;
    for (i = 0; i < net_n_flush; i++)
        if (net_flush_pending[i] == r) {
            net_flush_pending[i] = net_flush_pending[net_n_flush - 1];
            net_n_flush--;
            return;
        }
}

static JSValue redis_flush_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    int i = 0;
    (void)ctx; (void)argc; (void)argv;
    /* One job drains every waiting client: several may have queued in the same
     * turn, and each job would otherwise flush only its own. */
    while (i < net_n_flush) {
        dyn_redis_t *r = net_flush_pending[i];
        redis_flush_drop(r);
        if (r->state == RD_ST_READY && redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        i = 0;                                   /* the array compacted */
        if (net_n_flush == 0)
            break;
    }
    return JS_UNDEFINED;
}

/* Move a command from "issued" to "on the wire". */
static int redis_arm(dyn_redis_t *r, dyn_redis_pending_t *p)
{
    if (p->bytes) {
        if (redis_write(r, p->bytes, p->nbytes) < 0)
            return -1;
        free(p->bytes);
        p->bytes = NULL;
        p->nbytes = 0;
    }
    if (r->command_timeout_ms)
        p->deadline_ms = dyn_timer_now_ms() + r->command_timeout_ms;
    pend_push(&r->head, &r->tail, p);
    r->npending++;
    return 0;
}

static int redis_send_internal(dyn_redis_t *r, int kind, int argc,
                               const char *const *argv, const size_t *lens)
{
    dyn_redis_pending_t *p = pend_new(r->ctx);
    size_t need = dyn_resp_cmd_size(argc, argv, lens);
    uint8_t *b;

    if (!p)
        return -1;
    b = (uint8_t *)malloc(need);
    if (!b) { pend_free(r->ctx, p); return -1; }
    if (dyn_resp_cmd_encode(b, need, argc, argv, lens, NULL) < 0) {
        free(b); pend_free(r->ctx, p); return -1;
    }
    p->internal = kind;
    p->bytes = b;
    p->nbytes = need;
    return redis_arm(r, p);
}

/* Everything issued before the handshake finished, in the order issued. */
static void redis_release_waitq(dyn_redis_t *r)
{
    dyn_redis_pending_t *p;
    while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL) {
        r->nwait--;
        if (redis_arm(r, p) < 0) {
            pend_push(&r->wq_head, &r->wq_tail, p);
            r->nwait++;
            redis_fail_all(r, "Redis: out of memory");
            return;              /* fail_all settles the whole queue, not one */
        }
    }
    if (redis_flush(r) < 0)
        redis_fail_all(r, "Redis: cannot write to the socket");
}

/* ---- handshake ---------------------------------------------------------- */

static void redis_after_handshake(dyn_redis_t *r)
{
    r->state = RD_ST_READY;
    /* Cleared HERE, not at TCP connect: a peer that completes the handshake
     * and then never answers HELLO would otherwise have no deadline at all,
     * because commandTimeoutMs defaults to off. */
    r->connect_deadline_ms = 0;
    redis_release_waitq(r);
}

static int redis_send_select(dyn_redis_t *r)
{
    char dbs[16];
    const char *argv[2];
    int n = snprintf(dbs, sizeof(dbs), "%d", r->db);
    if (n < 0 || (size_t)n >= sizeof(dbs))
        return -1;
    argv[0] = "SELECT"; argv[1] = dbs;
    return redis_send_internal(r, RD_INT_SELECT, 2, argv, NULL);
}

/* HELLO 3 is refused by Redis < 6 with an ordinary error, which is the only
 * portable way to learn the server's protocol. A failure here is therefore a
 * DOWNGRADE, not a fault -- but an AUTH failure after it is fatal. */
static void redis_handle_hello(dyn_redis_t *r, int is_err)
{
    if (is_err) {
        r->proto = 2;
        if (r->pass) {
            const char *argv[3];
            int argc = 0;
            argv[argc++] = "AUTH";
            if (r->user) argv[argc++] = r->user;
            argv[argc++] = r->pass;
            if (redis_send_internal(r, RD_INT_AUTH, argc, argv, NULL) < 0) {
                redis_fail_all(r, "Redis: out of memory during handshake");
                return;
            }
            if (r->db > 0 && redis_send_select(r) < 0) {
                redis_fail_all(r, "Redis: out of memory during handshake");
                return;
            }
            if (redis_flush(r) < 0)
                redis_fail_all(r, "Redis: cannot write to the socket");
            return;
        }
    } else {
        r->proto = 3;
    }
    if (r->db > 0) {
        if (redis_send_select(r) < 0) {
            redis_fail_all(r, "Redis: out of memory during handshake");
            return;
        }
        if (redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    redis_after_handshake(r);
}

/* ---- reply dispatch ----------------------------------------------------- */

/* In RESP3 a push is typed. In RESP2 it is an ordinary array, and the ONLY
 * thing distinguishing it from a command's reply is the first element -- which
 * is why a client must know it is subscribed before believing one. */
static int redis_is_push(dyn_redis_t *r, const uint8_t *msg, size_t len)
{
    dyn_resp_reader_t rd;
    dyn_resp_item_t it;
    static const char *kinds[] = { "message", "pmessage", "smessage" };
    size_t i;

    if (len == 0)
        return 0;
    if (msg[0] == DYN_RESP_PUSH)
        return 1;
    if (msg[0] != DYN_RESP_ARRAY || r->subscribed == 0)
        return 0;
    dyn_resp_reader_init(&rd, msg, len);
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.count < 3)
        return 0;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_BULK)
        return 0;
    for (i = 0; i < countof(kinds); i++)
        if (it.slen == strlen(kinds[i]) &&
            memcmp(it.str, kinds[i], it.slen) == 0)
            return 1;
    return 0;
}

/* A RESP3 push that CONFIRMS a subscribe belongs to the command that asked for
 * it; only a delivered message is unsolicited. Getting this backwards leaves
 * the subscribe promise pending forever. */
static int redis_push_is_delivery(const uint8_t *msg, size_t len)
{
    dyn_resp_reader_t rd;
    dyn_resp_item_t it;
    static const char *deliv[] = { "message", "pmessage", "smessage" };
    size_t i;

    dyn_resp_reader_init(&rd, msg, len);
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK)
        return 1;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_BULK)
        return 1;
    for (i = 0; i < countof(deliv); i++)
        if (it.slen == strlen(deliv[i]) && memcmp(it.str, deliv[i], it.slen) == 0)
            return 1;
    return 0;
}

static void redis_deliver_push(dyn_redis_t *r, const uint8_t *msg, size_t len)
{
    JSContext *ctx = r->ctx;
    dyn_resp_reader_t rd;
    JSValue v;
    int is_err = 0;

    if (!JS_IsFunction(ctx, r->h_push))
        return;
    dyn_resp_reader_init(&rd, msg, len);
    v = redis_value(ctx, r, &rd, &is_err);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    {
        JSValueConst a[1] = { v };
        JSValue res = JS_Call(ctx, r->h_push, JS_UNDEFINED, 1, a);
        if (JS_IsException(res))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, v);
}

static void redis_one_reply(dyn_redis_t *r, const uint8_t *msg, size_t len)
{
    JSContext *ctx = r->ctx;
    dyn_redis_pending_t *p;
    dyn_resp_reader_t rd;
    JSValue v;
    int is_err = 0;

    if (redis_is_push(r, msg, len)) {
        /* A subscribe/unsubscribe confirmation still answers its command. */
        if (msg[0] != DYN_RESP_PUSH || redis_push_is_delivery(msg, len) ||
            r->head == NULL || r->head->internal) {
            redis_deliver_push(r, msg, len);
            return;
        }
    }

    p = r->head;
    if (!p) {
        /* A reply with nothing pending means the peer is not following the
         * protocol. Continuing would answer the NEXT command with this. */
        redis_fail_all(r, "Redis: reply with no command outstanding");
        return;
    }

    if (p->internal) {
        int kind = p->internal;
        dyn_resp_reader_init(&rd, msg, len);
        v = redis_value(ctx, r, &rd, &is_err);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            redis_fail_all(r, "Redis: malformed handshake reply");
            return;
        }
        JS_FreeValue(ctx, v);
        pend_pop(&r->head, &r->tail);
        r->npending--;
        pend_free(ctx, p);
        if (kind == RD_INT_HELLO) {
            redis_handle_hello(r, is_err);
        } else if (kind == RD_INT_AUTH && is_err) {
            /* Never echo the credential: the message would land in a log. */
            redis_fail_all(r, "Redis: authentication rejected");
        } else if (kind == RD_INT_SELECT && is_err) {
            redis_fail_all(r, "Redis: SELECT rejected");
        } else if (r->head == NULL || r->head->internal == RD_INT_NONE) {
            if (r->state != RD_ST_READY)
                redis_after_handshake(r);
        }
        return;
    }

    /* Auth revoked underneath a live connection (a restart, CONFIG SET
     * requirepass, an ACL reload). Every later command would fail the same
     * way and this handle cannot re-run its handshake, so it goes. Only
     * NOAUTH: NOPERM is per-command and LOADING/BUSY/TRYAGAIN are transient. */
    if (len > 8 && msg[0] == DYN_RESP_ERROR &&
        memcmp(msg + 1, "NOAUTH ", 7) == 0) {
        redis_fail_all(r, "Redis: the server requires authentication again");
        return;
    }

    dyn_resp_reader_init(&rd, msg, len);
    v = redis_value(ctx, r, &rd, &is_err);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        redis_fail_all(r, "Redis: malformed reply");
        return;
    }

    if (p->want > 1) {
        JS_SetPropertyUint32(ctx, p->acc, (uint32_t)p->got, v);
        p->got++;
        if (p->got < p->want)
            return;
        pend_pop(&r->head, &r->tail);
        r->npending--;
        v = p->acc;
        p->acc = JS_UNDEFINED;
        redis_settle(ctx, p, 0, v);   /* a pipeline reports per-element errors */
        return;
    }
    pend_pop(&r->head, &r->tail);
    r->npending--;
    redis_settle(ctx, p, is_err, v);
}

/* ---- socket ------------------------------------------------------------- */

static void redis_fail_all(dyn_redis_t *r, const char *msg)
{
    JSContext *ctx = r->ctx;
    dyn_redis_pending_t *p;

    if (r->state == RD_ST_DEAD)
        return;
    r->state = RD_ST_DEAD;
    if (r->fd >= 0) {
        dyn_aio_close(r->aio, r->fd);
        r->fd = -1;
    }
    while ((p = pend_pop(&r->head, &r->tail)) != NULL) {
        r->npending--;
        if (p->internal) pend_free(ctx, p);
        else redis_settle(ctx, p, 1, redis_conn_error(ctx, msg));
    }
    while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL) {
        r->nwait--;
        redis_settle(ctx, p, 1, redis_conn_error(ctx, msg));
    }
    if (JS_IsFunction(ctx, r->h_error)) {
        JSValue e = redis_conn_error(ctx, msg);
        JSValueConst a[1] = { e };
        JSValue res = JS_Call(ctx, r->h_error, JS_UNDEFINED, 1, a);
        if (JS_IsException(res))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, e);
    }
}

static void redis_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    dyn_redis_t *r = (dyn_redis_t *)ud;
    (void)aio;

    if (r->state == RD_ST_DEAD)
        return;
    if (res < 0) {
        redis_fail_all(r, "Redis: connection error");
        return;
    }
    if (res == 0 && len == 0) {
        redis_fail_all(r, "Redis: server closed the connection");
        return;
    }

    /* Cap the read buffer at what one reply may legally be. Without this a peer
     * that sends an endless unterminated line makes us buffer until the process
     * dies -- the scan would keep saying INCOMPLETE, correctly. */
    if (r->rlen - r->rpos + len > r->maxbulk + DYN_RESP_MAX_LINE) {
        redis_fail_all(r, "Redis: reply exceeds maxReplyBytes");
        return;
    }
    if (buf_reserve(&r->rbuf, &r->rcap, r->rlen + len) < 0) {
        redis_fail_all(r, "Redis: out of memory");
        return;
    }
    memcpy(r->rbuf + r->rlen, buf, len);
    r->rlen += len;

    if (r->rpos == 0 && r->rlen >= 3 && dyn_resp_looks_like_tls(r->rbuf, r->rlen)) {
        redis_fail_all(r, "Redis: the endpoint answered with TLS; this client "
                          "is plaintext only");
        return;
    }

    for (;;) {
        size_t used = 0;
        int rc = dyn_resp_scan(r->rbuf + r->rpos, r->rlen - r->rpos,
                               r->maxbulk, &used);
        if (rc == DYN_RESP_INCOMPLETE)
            break;
        if (rc != DYN_RESP_OK) {
            redis_fail_all(r, dyn_resp_strerror(rc));
            return;
        }
        redis_one_reply(r, r->rbuf + r->rpos, used);
        if (r->state == RD_ST_DEAD)
            return;
        r->rpos += used;
    }
    if (r->rpos) {
        memmove(r->rbuf, r->rbuf + r->rpos, r->rlen - r->rpos);
        r->rlen -= r->rpos;
        r->rpos = 0;
    }
}

static void redis_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                             unsigned len, void *ud)
{
    dyn_redis_t *r = (dyn_redis_t *)ud;
    (void)aio; (void)buf; (void)len;

    if (r->state == RD_ST_DEAD)
        return;
    if (res < 0) {
        redis_fail_all(r, "Redis: connect failed");
        return;
    }
    r->state = RD_ST_HANDSHAKE;
    if (dyn_aio_recv(r->aio, r->fd, 0, /*multishot=*/1, redis_on_recv, r) < 0) {
        redis_fail_all(r, "Redis: cannot read from the socket");
        return;
    }
    if (redis_flush(r) < 0)
        redis_fail_all(r, "Redis: cannot write to the socket");
}

/* One tick drives both deadlines. Registering it only while something can time
 * out keeps an idle client off the reactor's timer entirely. */
static void redis_tick(void *udata)
{
    dyn_redis_t *r = (dyn_redis_t *)udata;
    uint64_t now;

    if (r->state == RD_ST_DEAD)
        return;
    now = dyn_timer_now_ms();
    if (r->connect_deadline_ms && now >= r->connect_deadline_ms) {
        redis_fail_all(r, "Redis: connect timed out");
        return;
    }
    if (r->head && r->head->deadline_ms && now >= r->head->deadline_ms) {
        /* A timed-out command cannot be un-sent, so its reply would land on
         * the NEXT command. The connection goes, not just the entry. */
        redis_fail_all(r, "Redis: command timed out");
        return;
    }
}

/* ---- argument coercion --------------------------------------------------
 * Coerce EVERY argument to a C local before touching the native handle: a
 * toString/valueOf hook runs arbitrary JS and can close the client underneath
 * us (Appendix A). */

typedef struct {
    const char **argv;
    size_t *lens;
    uint8_t *is_cstr;   /* how argv[i] must be released -- NOT inferable */
    JSValue *keep;      /* ArrayBuffers borrowed from typed arrays */
    int n, nkeep;
} redis_args_t;

static void args_free(JSContext *ctx, redis_args_t *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        if (a->argv[i] && a->is_cstr && a->is_cstr[i])
            JS_FreeCString(ctx, a->argv[i]);
    for (i = 0; i < a->nkeep; i++)
        JS_FreeValue(ctx, a->keep[i]);
    free(a->argv); free(a->lens); free(a->keep); free(a->is_cstr);
    memset(a, 0, sizeof(*a));
}

static int args_build(JSContext *ctx, redis_args_t *a, int argc,
                      JSValueConst *argv)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->argv = (const char **)calloc((size_t)argc, sizeof(*a->argv));
    a->lens = (size_t *)calloc((size_t)argc, sizeof(*a->lens));
    a->keep = (JSValue *)calloc((size_t)argc, sizeof(*a->keep));
    a->is_cstr = (uint8_t *)calloc((size_t)argc, sizeof(*a->is_cstr));
    if (!a->argv || !a->lens || !a->keep || !a->is_cstr) {
        args_free(ctx, a);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < argc; i++) {
        size_t off = 0, blen = 0, bpe = 0;
        JSValue ab = JS_GetArrayBufferView(ctx, argv[i], &off, &blen, &bpe);
        if (!JS_IsException(ab)) {
            size_t total = 0;
            uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab);
            if (!base) { JS_FreeValue(ctx, ab); args_free(ctx, a); return -1; }
            a->keep[a->nkeep++] = ab;
            a->argv[i] = (const char *)(base + off);
            a->lens[i] = blen;
            a->is_cstr[i] = 0;
            a->n = i + 1;
            continue;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        {
            size_t l = 0;
            const char *s = JS_ToCStringLen(ctx, &l, argv[i]);
            if (!s) { args_free(ctx, a); return -1; }
            a->argv[i] = s;
            a->lens[i] = l;
            a->is_cstr[i] = 1;
            a->n = i + 1;
        }
    }
    a->n = argc;
    return 0;
}

/* ---- JS surface --------------------------------------------------------- */

static dyn_redis_t *redis_this(JSContext *ctx, JSValueConst this_val)
{
    return (dyn_redis_t *)dyn_res_native(ctx, this_val, dyn_redis_class_id);
}

static const char *const RD_SUB[]   = { "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE" };
static const char *const RD_UNSUB[] = { "UNSUBSCRIBE", "PUNSUBSCRIBE",
                                        "SUNSUBSCRIBE" };

static int redis_verb_is(const char *cmd, size_t len, const char *const *tab,
                         size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (len == strlen(tab[i]) && strncasecmp(cmd, tab[i], len) == 0)
            return 1;
    return 0;
}

/* Replies this command consumes. A (P|S)SUBSCRIBE answers ONCE PER CHANNEL,
 * not once: treating it as one leaves every later reply one step out of phase,
 * which on a positional protocol means one key's value returned for another.
 * Returns -1 when the count is unknowable (a bare UNSUBSCRIBE answers once per
 * channel the SERVER thinks we hold). */
static int redis_reply_count(const char *cmd, size_t len, int argc)
{
    int per_channel = redis_verb_is(cmd, len, RD_SUB, countof(RD_SUB)) ||
                      redis_verb_is(cmd, len, RD_UNSUB, countof(RD_UNSUB));
    if (!per_channel)
        return 1;
    return argc > 1 ? argc - 1 : -1;
}

/* Track subscription state so a RESP2 push can be told from a reply at all.
 * Counts CHANNELS, not commands: `SUBSCRIBE a b c` then `UNSUBSCRIBE a` must
 * leave two, or deliveries stop being recognised as pushes. */
static void redis_note_subscription(dyn_redis_t *r, const char *cmd, size_t len,
                                    int argc)
{
    int n = argc > 1 ? argc - 1 : 0;
    if (redis_verb_is(cmd, len, RD_SUB, countof(RD_SUB)))
        r->subscribed += n;
    else if (redis_verb_is(cmd, len, RD_UNSUB, countof(RD_UNSUB)))
        r->subscribed = n && r->subscribed > n ? r->subscribed - n : 0;
}

/* RESP2 puts a subscribed connection into a mode that accepts only these. The
 * server would answer with an error anyway; refusing locally names the rule at
 * the line that broke it, and keeps a stray command from being in flight when
 * a delivery arrives -- which is the shape that desynchronises the FIFO. */
static int redis_allowed_while_subscribed(const char *cmd, size_t len)
{
    static const char *const ok[] = { "PING", "QUIT", "RESET" };
    return redis_verb_is(cmd, len, ok, countof(ok)) ||
           redis_verb_is(cmd, len, RD_SUB, countof(RD_SUB)) ||
           redis_verb_is(cmd, len, RD_UNSUB, countof(RD_UNSUB));
}

static JSValue redis_enqueue(JSContext *ctx, dyn_redis_t *r, int argc,
                             const char *const *argv, const size_t *lens,
                             int want, JSValue acc)
{
    JSValue funcs[2], promise;
    dyn_redis_pending_t *p;
    size_t need;
    uint8_t *b;

    /* Both queues, not just the armed one: a client that has not finished its
     * handshake puts everything in the wait queue, so gating on `npending`
     * alone leaves it unbounded exactly when it cannot drain. */
    if (r->npending + r->nwait >= r->maxpending) {
        JS_FreeValue(ctx, acc);
        return JS_ThrowInternalError(ctx,
            "Redis: %d commands already in flight (maxPending)", r->maxpending);
    }
    need = dyn_resp_cmd_size(argc, argv, lens);
    b = (uint8_t *)malloc(need);
    if (!b) { JS_FreeValue(ctx, acc); return JS_ThrowOutOfMemory(ctx); }
    if (dyn_resp_cmd_encode(b, need, argc, argv, lens, NULL) < 0) {
        free(b); JS_FreeValue(ctx, acc);
        return JS_ThrowInternalError(ctx, "Redis: cannot encode the command");
    }
    p = pend_new(ctx);
    if (!p) { free(b); JS_FreeValue(ctx, acc); return JS_ThrowOutOfMemory(ctx); }

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(b); pend_free(ctx, p); JS_FreeValue(ctx, acc);
        return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];
    p->bytes = b;
    p->nbytes = need;
    p->want = want;
    p->acc = acc;

    if (r->state == RD_ST_READY) {
        if (redis_arm(r, p) < 0) {
            pend_free(ctx, p);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        redis_flush_soon(r);
    } else {
        /* Held until the handshake settles, so a command cannot overtake AUTH
         * or SELECT and be answered by a server that has not applied them. */
        pend_push(&r->wq_head, &r->wq_tail, p);
        r->nwait++;
    }
    return promise;
}

static JSValue dyn_redis_command(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    redis_args_t a;
    dyn_redis_t *r;
    JSValue ret;
    int want;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "command: a command name is required");
    if (args_build(ctx, &a, argc, argv) < 0)
        return JS_EXCEPTION;
    r = redis_this(ctx, this_val);          /* AFTER coercion, never before */
    if (!r) { args_free(ctx, &a); return JS_EXCEPTION; }
    if (r->state == RD_ST_DEAD) {
        args_free(ctx, &a);
        return JS_ThrowInternalError(ctx, "Redis: the connection is closed");
    }
    /* READY as well as proto: before the handshake settles, `proto` is still
     * its 2 default and the connection may well turn out to be RESP3 -- so
     * gating on the value alone refuses commands on a connection that has no
     * such restriction. Nothing is on the wire before READY anyway. */
    if (r->state == RD_ST_READY && r->proto == 2 && r->subscribed > 0 &&
        !redis_allowed_while_subscribed(a.argv[0], a.lens[0])) {
        JSValue e = JS_ThrowTypeError(ctx,
            "Redis: '%s' is not allowed while subscribed on RESP2; only PING, "
            "QUIT, RESET and the (un)subscribe commands are", a.argv[0]);
        args_free(ctx, &a);
        return e;
    }
    want = redis_reply_count(a.argv[0], a.lens[0], a.n);
    if (want < 0) {
        args_free(ctx, &a);
        return JS_ThrowTypeError(ctx,
            "Redis: unsubscribe by name -- with no channel the reply count "
            "depends on server state and cannot be matched to this command");
    }
    ret = redis_enqueue(ctx, r, a.n, a.argv, a.lens, want,
                        want > 1 ? JS_NewArray(ctx) : JS_UNDEFINED);
    /* AFTER enqueue: a command refused by maxPending never reaches the server,
     * so counting its channels would leave the state permanently wrong. */
    if (!JS_IsException(ret))
        redis_note_subscription(r, a.argv[0], a.lens[0], a.n);
    args_free(ctx, &a);
    return ret;
}

/* One round trip for N commands. The pipeline is one pending entry consuming N
 * replies, so a partial answer cannot leave the FIFO one step out of phase. */
static JSValue dyn_redis_pipeline(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_redis_t *r;
    JSValue acc, ret;
    uint32_t n, i;
    uint8_t *all = NULL;
    size_t alen = 0, acap = 0;
    int replies = 0, chan_add = 0, chan_del = 0;
    dyn_redis_pending_t *p;
    JSValue funcs[2], promise;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "pipeline: expects an array of commands");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToUint32(ctx, &n, lv) < 0) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    }
    if (n == 0)
        return JS_ThrowTypeError(ctx, "pipeline: at least one command");

    /* Encode everything BEFORE touching the handle: each element's coercion can
     * run JS. */
    for (i = 0; i < n; i++) {
        JSValue cmd = JS_GetPropertyUint32(ctx, argv[0], i);
        redis_args_t a;
        uint32_t m, k;
        JSValueConst *parts;
        size_t need;
        if (!JS_IsArray(ctx, cmd)) {
            JS_FreeValue(ctx, cmd); free(all);
            return JS_ThrowTypeError(ctx, "pipeline: element %u is not an array", i);
        }
        { JSValue lv = JS_GetPropertyStr(ctx, cmd, "length");
          if (JS_ToUint32(ctx, &m, lv) < 0) { JS_FreeValue(ctx, lv); JS_FreeValue(ctx, cmd); free(all); return JS_EXCEPTION; }
          JS_FreeValue(ctx, lv); }
        if (m == 0) {
            JS_FreeValue(ctx, cmd); free(all);
            return JS_ThrowTypeError(ctx, "pipeline: element %u is empty", i);
        }
        parts = (JSValueConst *)calloc(m, sizeof(*parts));
        if (!parts) { JS_FreeValue(ctx, cmd); free(all); return JS_ThrowOutOfMemory(ctx); }
        for (k = 0; k < m; k++)
            parts[k] = JS_GetPropertyUint32(ctx, cmd, k);
        if (args_build(ctx, &a, (int)m, parts) < 0) {
            for (k = 0; k < m; k++) JS_FreeValue(ctx, parts[k]);
            free(parts); JS_FreeValue(ctx, cmd); free(all);
            return JS_EXCEPTION;
        }
        {   /* A (P|S)SUBSCRIBE in a batch answers once PER CHANNEL, so the
             * batch's reply count is a SUM, not the number of commands. */
            int rc1 = redis_reply_count(a.argv[0], a.lens[0], (int)m);
            if (rc1 < 0) {
                args_free(ctx, &a);
                for (k = 0; k < m; k++) JS_FreeValue(ctx, parts[k]);
                free(parts); JS_FreeValue(ctx, cmd); free(all);
                return JS_ThrowTypeError(ctx,
                    "Redis: unsubscribe by name in a pipeline -- with no "
                    "channel the reply count depends on server state");
            }
            replies += rc1;
            if (redis_verb_is(a.argv[0], a.lens[0], RD_SUB, countof(RD_SUB)))
                chan_add += m > 1 ? (int)m - 1 : 0;
            else if (redis_verb_is(a.argv[0], a.lens[0], RD_UNSUB,
                                   countof(RD_UNSUB)))
                chan_del += m > 1 ? (int)m - 1 : 0;
        }
        need = dyn_resp_cmd_size((int)m, a.argv, a.lens);
        if (buf_reserve(&all, &acap, alen + need) < 0 ||
            dyn_resp_cmd_encode(all + alen, acap - alen, (int)m,
                                a.argv, a.lens, NULL) < 0) {
            args_free(ctx, &a);
            for (k = 0; k < m; k++) JS_FreeValue(ctx, parts[k]);
            free(parts); JS_FreeValue(ctx, cmd); free(all);
            return JS_ThrowOutOfMemory(ctx);
        }
        alen += need;
        args_free(ctx, &a);
        for (k = 0; k < m; k++) JS_FreeValue(ctx, parts[k]);
        free(parts);
        JS_FreeValue(ctx, cmd);
    }

    r = redis_this(ctx, this_val);
    if (!r) { free(all); return JS_EXCEPTION; }
    if (r->state == RD_ST_DEAD) {
        free(all);
        return JS_ThrowInternalError(ctx, "Redis: the connection is closed");
    }
    if (r->npending + r->nwait >= r->maxpending) {
        free(all);
        return JS_ThrowInternalError(ctx, "Redis: too many commands in flight");
    }
    acc = JS_NewArray(ctx);
    if (JS_IsException(acc)) { free(all); return acc; }
    p = pend_new(ctx);
    if (!p) { free(all); JS_FreeValue(ctx, acc); return JS_ThrowOutOfMemory(ctx); }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(all); pend_free(ctx, p); JS_FreeValue(ctx, acc); return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];
    p->bytes = all;
    p->nbytes = alen;
    p->want = replies;
    p->acc = acc;
    /* The pipeline path used to skip this entirely, so a SUBSCRIBE sent in a
     * batch left the client believing it was not subscribed -- and on RESP2
     * every later delivery was then consumed as a command's reply. */
    r->subscribed += chan_add;
    r->subscribed = r->subscribed > chan_del ? r->subscribed - chan_del : 0;
    if (r->state == RD_ST_READY) {
        if (redis_arm(r, p) < 0) {
            pend_free(ctx, p); JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        redis_flush_soon(r);
    } else {
        pend_push(&r->wq_head, &r->wq_tail, p);
        r->nwait++;
    }
    ret = promise;
    return ret;
}

static JSValue dyn_redis_get_protocol(JSContext *ctx, JSValueConst this_val)
{
    dyn_redis_t *r = redis_this(ctx, this_val);
    return r ? JS_NewInt32(ctx, r->proto) : JS_EXCEPTION;
}

static JSValue dyn_redis_get_ready(JSContext *ctx, JSValueConst this_val)
{
    dyn_redis_t *r = redis_this(ctx, this_val);
    return r ? JS_NewBool(ctx, r->state == RD_ST_READY) : JS_EXCEPTION;
}

static JSValue dyn_redis_get_pending(JSContext *ctx, JSValueConst this_val)
{
    dyn_redis_t *r = redis_this(ctx, this_val);
    return r ? JS_NewInt32(ctx, r->npending + r->nwait) : JS_EXCEPTION;
}

static JSValue dyn_redis_on(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_redis_t *r;
    const char *ev;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, handler)");
    ev = JS_ToCString(ctx, argv[0]);
    if (!ev)
        return JS_EXCEPTION;
    r = redis_this(ctx, this_val);
    if (!r) { JS_FreeCString(ctx, ev); return JS_EXCEPTION; }
    if (strcmp(ev, "push") == 0 || strcmp(ev, "message") == 0) {
        JS_FreeValue(ctx, r->h_push);
        r->h_push = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "error") == 0) {
        JS_FreeValue(ctx, r->h_error);
        r->h_error = JS_DupValue(ctx, argv[1]);
    } else {
        JS_FreeCString(ctx, ev);
        return JS_ThrowRangeError(ctx, "on: unknown event; want 'push' or 'error'");
    }
    JS_FreeCString(ctx, ev);
    return JS_DupValue(ctx, this_val);
}

/* ---- construction and teardown ------------------------------------------ */

static void dyn_redis_dispose(void *native)
{
    dyn_redis_t *r = (dyn_redis_t *)native;
    dyn_redis_pending_t *p;

    if (!r)
        return;
    redis_flush_drop(r);       /* never reachable through the flush list again */
    if (r->hooked)
        dyn_net_off_drain(r);
    if (r->state != RD_ST_DEAD) {
        r->state = RD_ST_DEAD;
        while ((p = pend_pop(&r->head, &r->tail)) != NULL) {
            if (p->internal) pend_free(r->ctx, p);
            else redis_settle(r->ctx, p, 1,
                              redis_conn_error(r->ctx, "Redis: client closed"));
        }
        while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL)
            redis_settle(r->ctx, p, 1,
                         redis_conn_error(r->ctx, "Redis: client closed"));
    }
    if (r->aio) {
        if (r->fd >= 0)
            dyn_aio_close(r->aio, r->fd);
        dyn_net_reactor_release(r->ctx);
    }
    JS_FreeValue(r->ctx, r->h_push);
    JS_FreeValue(r->ctx, r->h_error);
    free(r->host); free(r->path); free(r->user); free(r->pass);
    free(r->rbuf); free(r->obuf);
    free(r);
}

static char *opt_str(JSContext *ctx, JSValueConst o, const char *k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char *s;
    char *out = NULL;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return NULL; }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (s) { out = strdup(s); JS_FreeCString(ctx, s); }
    return out;
}

static int opt_int(JSContext *ctx, JSValueConst o, const char *k, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return dflt; }
    if (JS_ToInt32(ctx, &n, v) < 0) { JS_FreeValue(ctx, v); return dflt; }
    JS_FreeValue(ctx, v);
    return (int)n;
}

static int opt_bool(JSContext *ctx, JSValueConst o, const char *k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static JSValue dyn_redis_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    dyn_redis_t *r;
    JSValueConst opt = argc > 0 ? argv[0] : JS_UNDEFINED;
    const char *argvh[4];
    JSValue res;
    (void)new_target;

    if (argc > 0 && !JS_IsObject(opt))
        return JS_ThrowTypeError(ctx, "Redis: expects an options object");

    r = (dyn_redis_t *)calloc(1, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->ctx = ctx;
    r->fd = -1;
    r->proto = 2;
    r->state = RD_ST_CONNECTING;
    r->h_push = r->h_error = JS_UNDEFINED;
    r->maxbulk = RD_DEFAULT_MAXBULK;
    r->maxpending = RD_DEFAULT_PENDING;

    if (JS_IsObject(opt)) {
        int mb, mp, ct, cmdt;
        /* A `rediss://` endpoint is REFUSED by name. Silently connecting in
         * plaintext to something asked for in TLS is worse than not working. */
        if (opt_bool(ctx, opt, "tls")) {
            free(r);
            return JS_ThrowTypeError(ctx,
                "Redis: TLS is not supported; use a plaintext endpoint or "
                "terminate TLS in front of it");
        }
        r->host = opt_str(ctx, opt, "host");
        r->path = opt_str(ctx, opt, "path");
        r->user = opt_str(ctx, opt, "username");
        r->pass = opt_str(ctx, opt, "password");
        r->port = (uint16_t)opt_int(ctx, opt, "port", 6379);
        r->db = opt_int(ctx, opt, "db", 0);
        r->binary = opt_bool(ctx, opt, "binary");
        r->bigint = opt_bool(ctx, opt, "bigint");
        mb = opt_int(ctx, opt, "maxReplyBytes", 0);
        if (mb > 0) r->maxbulk = (size_t)mb;
        mp = opt_int(ctx, opt, "maxPending", 0);
        if (mp > 0) r->maxpending = mp;
        ct = opt_int(ctx, opt, "connectTimeoutMs", RD_CONNECT_TIMEOUT);
        cmdt = opt_int(ctx, opt, "commandTimeoutMs", 0);
        if (cmdt > 0) r->command_timeout_ms = (uint64_t)cmdt;
        if (ct > 0) r->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)ct;
        if (r->db < 0 || r->db > 255) {
            free(r->host); free(r->path); free(r->user); free(r->pass); free(r);
            return JS_ThrowRangeError(ctx, "Redis: db must be 0..255");
        }
        if (r->port == 0 && !r->path) {
            free(r->host); free(r->path); free(r->user); free(r->pass); free(r);
            return JS_ThrowRangeError(ctx, "Redis: port must be 1..65535");
        }
    } else {
        r->port = 6379;
        r->connect_deadline_ms = dyn_timer_now_ms() + RD_CONNECT_TIMEOUT;
    }

    r->aio = dyn_net_reactor_acquire(ctx);
    if (!r->aio) {
        free(r->host); free(r->path); free(r->user); free(r->pass); free(r);
        return JS_ThrowInternalError(ctx, "Redis: cannot acquire the reactor");
    }

    r->fd = r->path
          ? dyn_aio_unix_connect(r->aio, r->path, redis_on_connect, r)
          : dyn_aio_connect(r->aio, r->host ? r->host : "127.0.0.1",
                            r->port, redis_on_connect, r);
    if (r->fd < 0) {
        JSValue e = JS_ThrowInternalError(ctx, "Redis: connect: %s",
                                          strerror(errno));
        dyn_redis_dispose(r);
        return e;
    }

    /* The handshake is enqueued HERE, synchronously, so it is ahead of any
     * command the caller issues on the next line. */
    argvh[0] = "HELLO"; argvh[1] = "3";
    if (r->pass) {
        /* RESP3 AUTH inside HELLO takes user and pass; "default" is the user
         * when only a password was configured. */
        {
            const char *four[5] = { "HELLO", "3", "AUTH",
                                    r->user ? r->user : "default", r->pass };
            if (redis_send_internal(r, RD_INT_HELLO, 5, four, NULL) < 0) {
                dyn_redis_dispose(r);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
    } else if (redis_send_internal(r, RD_INT_HELLO, 2, argvh, NULL) < 0) {
        dyn_redis_dispose(r);
        return JS_ThrowOutOfMemory(ctx);
    }

    if (dyn_net_on_drain(redis_tick, r) == 0)
        r->hooked = 1;

    res = dyn_res_wrap(ctx, dyn_redis_class_id, r, dyn_redis_dispose);
    return res;
}

/* The framework installs close() on every resource proto, so defining one here
 * would abort at registration. Teardown lives in dispose. */
static const JSCFunctionListEntry dyn_redis_proto[] = {
    JS_CFUNC_DEF("command", 1, dyn_redis_command),
    JS_CFUNC_DEF("pipeline", 1, dyn_redis_pipeline),
    JS_CFUNC_DEF("on", 2, dyn_redis_on),
    JS_CGETSET_DEF("protocol", dyn_redis_get_protocol, NULL),
    JS_CGETSET_DEF("ready", dyn_redis_get_ready, NULL),
    JS_CGETSET_DEF("pending", dyn_redis_get_pending, NULL),
};

/* Marks the two HANDLERS and nothing else. A handler closure that captures its
 * own client is the cycle that actually happens, and without this it survives to
 * JS_FreeRuntime.
 *
 * The pending entries' promise pairs are deliberately NOT marked, and that is a
 * measured limit rather than an oversight: traversing them crashes the collector
 * (`free_zero_refcount` asserts) under concurrent clients. The bisect is sharp
 * and rules out the obvious explanation -- marking something this object does
 * not own would fail on its own, but EACH class is clean alone and only the two
 * together fail, 3/3 against 0/3, so the fault is in the interaction and not in
 * either mark. Restricting both to the handlers is clean 3/3 under the same
 * stress. Reproducer: tests/test_net_rss.js with PER_ROUND raised to 12.
 * A promise pair is reachable from its promise anyway; a handler closure is not. */
static void dyn_redis_gc_mark(JSRuntime *rt, JSValueConst val,
                              JS_MarkFunc *mark_func)
{
    DynResource *res = (DynResource *)JS_GetOpaque(val, dyn_redis_class_id);
    dyn_redis_t *r;

    if (!res || res->closed || !res->native)
        return;
    r = (dyn_redis_t *)res->native;
    JS_MarkValue(rt, r->h_push, mark_func);
    JS_MarkValue(rt, r->h_error, mark_func);
}

static const JSClassDef dyn_redis_class = {
    "Redis",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_redis_gc_mark,
};


int dyn_redis_register(JSContext *ctx, JSModuleDef *m)
{
    return dyn_register_class(ctx, m, &dyn_redis_class_id, &dyn_redis_class,
                              dyn_redis_proto, countof(dyn_redis_proto),
                              dyn_redis_ctor, "Redis");
}

void dyn_redis_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "Redis");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
