/*
 * PostgreSQL client, part of dyna:net. Same discipline as the Redis client: on
 * the JS thread through the shared reactor, one Promise per query, a strict
 * FIFO that never tries to resynchronise.
 *
 * Protocol version 3.0. 3.2 (new in PostgreSQL 18) changes nothing a client
 * here wants -- its one visible difference is a longer cancel key -- so asking
 * for 3.0 gets the same behaviour from every server back to 7.4 and avoids the
 * NegotiateProtocolVersion path entirely. That message is still handled, because
 * a server may also send it for an unrecognised _pq_ option, and a client that
 * skips it reads the NEXT message at the wrong offset.
 *
 * THE CANCEL KEY IS SIZED FROM ITS MESSAGE, NOT FROM THE VERSION. The server
 * sends 4 octets on 3.0 and 32 on 3.2, the protocol permits up to 256 so a
 * pooler may use its own, and reading a fixed width desynchronises the stream.
 *
 *   const db = new PostgreSQL({ host, port, user, password, database });
 *   const r = await db.query("SELECT $1::int AS n", [7]);   // r.rows[0].n
 *
 * NO TLS, and that is a security statement, not an omission -- see the note on
 * authentication below and dyn-scram.h.
 */
#include "dyna-nat.h"
#include "dtoa.h"   /* js_atod */
#include <math.h>   /* js_atod: correctly-rounded, locale-free float parse */
#include "dyna-aio.h"
#include "core/dyn-scram.h"
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define PG_PROTO_30       196608u      /* PG_PROTOCOL(3,0) */
#define PG_CANCEL_CODE     80877102u   /* PG_PROTOCOL(1234,5678) */
#define PG_MAX_STARTUP     10000       /* the server's own limit */
#define PG_MAX_CANCEL_KEY  256         /* the protocol's, not the server's 32 */
#define PG_DEFAULT_MAXMSG  (64u * 1024u * 1024u)
#define PG_CONNECT_TIMEOUT 10000

/* Authentication request codes. */
#define PG_AUTH_OK          0
#define PG_AUTH_CLEARTEXT   3
#define PG_AUTH_MD5         5
#define PG_AUTH_SASL       10
#define PG_AUTH_SASL_CONT  11
#define PG_AUTH_SASL_FINAL 12

/* Type OIDs we convert. Everything else stays text, which is exact. */
#define PG_OID_BOOL     16
#define PG_OID_BYTEA    17
#define PG_OID_INT8     20
#define PG_OID_INT2     21
#define PG_OID_INT4     23
#define PG_OID_OID      26
#define PG_OID_FLOAT4  700
#define PG_OID_FLOAT8  701
#define PG_OID_UUID   2950

/* BINARY IS NOT A BLANKET WIN, which is why this is a per-column list and not
 * a flag. For a fixed-width or text-expanded type it removes the server's
 * formatting, the wire bytes and our parse; for a TEXT-LIKE type it COSTS
 * about 16%, because `textout` hands back a pointer while `textsend` makes an
 * extra copy. So: numbers, bools, uuid and bytea in binary; text, varchar,
 * json, numeric and the timestamps stay text (numeric's binary form is a
 * base-10000 digit vector, and the timestamps are documented as text). */
static int pg_oid_prefers_binary(uint32_t oid)
{
    switch (oid) {
    case PG_OID_BOOL: case PG_OID_BYTEA: case PG_OID_INT8: case PG_OID_INT2:
    case PG_OID_INT4: case PG_OID_OID:   case PG_OID_FLOAT4:
    case PG_OID_FLOAT8: case PG_OID_UUID:
        return 1;
    default:
        return 0;
    }
}

#define PG_ST_CONNECTING 0
#define PG_ST_AUTH       1
#define PG_ST_READY      2
#define PG_ST_DEAD       3

typedef struct dyn_pg_pending {
    struct dyn_pg_pending *next;
    JSValue resolve, reject;
    JSValue rows, fields;
    JSValue error;            /* the first ErrorResponse of this query */
    uint8_t *bytes;           /* encoded, while queued behind the handshake */
    size_t nbytes;
    char tag[64];             /* CommandComplete */
    int rowcount, ncomplete;
    uint64_t deadline_ms;
    /* The column name and type read ONCE per RowDescription instead of once
     * per column per row. The atom is what matters: without it every row
     * re-interns every column name, which is a hash of the name per cell. */
    JSAtom *fatom;
    uint32_t *foid;
    uint8_t *fformat;      /* 0 text, 1 binary -- as the SERVER reported it */
    int nfield;
    /* The named statement this query used, "" for the one-shot arm. Stored by
     * NAME, not by pointer: the cache array compacts on eviction, so a pointer
     * into it would dangle. An error here means the server rejected that
     * statement (a schema change invalidates a cached plan) and the entry must
     * go, or every later call fails the same way for ever. */
    char stmt_name[24];
} dyn_pg_pending_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd, state, hooked;
    int insecure_auth;        /* cleartext and MD5 are refused without this */
    int raw;                  /* every column as text */
    int bytes_out;            /* bytea as a Uint8Array rather than \x hex */
    int binary_results;       /* request per-column binary where it wins */
    int bigint;               /* int8 as BigInt rather than text past 2^53 */
    char *host, *path, *user, *pass, *database, *appname;
    uint16_t port;
    size_t maxmsg;
    uint64_t connect_deadline_ms, query_timeout_ms;

    uint8_t *rbuf; size_t rcap, rlen, rpos;
    uint8_t *obuf; size_t ocap, olen;

    dyn_pg_pending_t *head, *tail, *wq_head, *wq_tail;
    int npending, nwait, maxpending;
    int flush_queued;         /* a coalescing flush job is pending */

    /* ---- prepared-statement cache. STRATEGY, not a switch: an unnamed
       statement is one round trip and leaves no server state, which is right
       for a one-shot query; a NAMED one skips the server's parse and plan,
       which is right for a query issued repeatedly. The condition that
       selects the second is simply "we have seen this SQL before". */
    struct pg_stmt {
        char *sql;
        size_t sqllen;
        /* Result column types, learned from the FIRST execution's
         * RowDescription. Per-column binary needs the types at Bind time, and
         * they only arrive after Describe -- so this is what the statement
         * cache buys beyond skipping Parse: the second execution onwards can
         * ask for binary without an extra round trip. */
        uint32_t *oids;
        uint32_t hash;
        int uses;             /* sightings, including this one */
        int prepared;         /* the server has Parsed it under `name` */
        int noids;
        char name[24];        /* "djsN" -- short: it is on every Bind */
    } *stmts;
    int nstmts, cap_stmts;
    int stmt_cache_max;       /* 0 disables: PgBouncer cannot do named ones */
    int prepare_after;        /* sightings before promoting; >=1 */
    uint32_t stmt_seq;
    uint64_t n_prepared_hits; /* Binds that skipped a Parse */
    uint64_t n_unnamed;       /* queries that took the one-shot arm */

    dyn_scram_t scram;
    int scram_live, scram_done;
    uint32_t backend_pid;
    uint8_t cancel_key[PG_MAX_CANCEL_KEY];
    size_t cancel_key_len;
    char tx_status;           /* 'I' idle, 'T' in a block, 'E' failed block */
    JSValue params;           /* ParameterStatus, as an object */
    JSValue h_notice, h_notify, h_error;
} dyn_pg_t;

static JSClassID dyn_pg_class_id;

static void pg_stmt_evict_named(dyn_pg_t *g, const char *name);

/* Connections with bytes waiting for the end-of-turn flush. Per thread, like
 * the reactor itself; an entry is removed on flush and on dispose, so a closed
 * connection is never reached through it. */
#define PG_MAX_FLUSH 256
static _Thread_local dyn_pg_t *pg_flush_pending[PG_MAX_FLUSH];
static _Thread_local int pg_n_flush;

static void pg_fail_all(dyn_pg_t *g, const char *msg);

/* ---- buffers ------------------------------------------------------------ */

static int pgbuf_reserve(uint8_t **p, size_t *cap, size_t need)
{
    size_t c = *cap;
    uint8_t *n;
    if (need <= c)
        return 0;
    if (c == 0)
        c = 1024;
    while (c < need)
        c = c < (1u << 20) ? c * 2 : c + (c / 4);
    n = (uint8_t *)realloc(*p, c);
    if (!n)
        return -1;
    *p = n; *cap = c;
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

/* ---- outgoing messages -------------------------------------------------- */

typedef struct { uint8_t *b; size_t len, cap; int bad; } pgw_t;

static void pgw_raw(pgw_t *w, const void *p, size_t n)
{
    if (w->bad || pgbuf_reserve(&w->b, &w->cap, w->len + n) < 0) { w->bad = 1; return; }
    if (n) memcpy(w->b + w->len, p, n);
    w->len += n;
}
static void pgw_u8(pgw_t *w, uint8_t v)   { pgw_raw(w, &v, 1); }
static void pgw_u32(pgw_t *w, uint32_t v) { uint8_t t[4]; put32(t, v); pgw_raw(w, t, 4); }
static void pgw_u16(pgw_t *w, uint16_t v) { uint8_t t[2]; t[0]=(uint8_t)(v>>8); t[1]=(uint8_t)v; pgw_raw(w, t, 2); }
static void pgw_str(pgw_t *w, const char *s) { pgw_raw(w, s, strlen(s) + 1); }

/* Begin a typed message, remembering where its length goes. */
static size_t pgw_begin(pgw_t *w, char type)
{
    pgw_u8(w, (uint8_t)type);
    pgw_u32(w, 0);
    return w->len - 4;
}
static void pgw_end(pgw_t *w, size_t at)
{
    if (!w->bad)
        put32(w->b + at, (uint32_t)(w->len - at));  /* length includes itself */
}

/* ---- pending ------------------------------------------------------------ */

static dyn_pg_pending_t *pgp_new(void)
{
    dyn_pg_pending_t *p = (dyn_pg_pending_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->resolve = p->reject = p->rows = p->fields = p->error = JS_UNDEFINED;
    return p;
}

static void pgp_free_fields(JSContext *ctx, dyn_pg_pending_t *p)
{
    int i;
    for (i = 0; i < p->nfield; i++)
        JS_FreeAtom(ctx, p->fatom[i]);
    free(p->fatom); p->fatom = NULL;
    free(p->foid);  p->foid = NULL;
    free(p->fformat); p->fformat = NULL;
    p->nfield = 0;
}

static void pgp_free(JSContext *ctx, dyn_pg_pending_t *p)
{
    JS_FreeValue(ctx, p->resolve); JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->rows);    JS_FreeValue(ctx, p->fields);
    JS_FreeValue(ctx, p->error);
    pgp_free_fields(ctx, p);
    free(p->bytes);
    free(p);
}

static void pgp_push(dyn_pg_pending_t **h, dyn_pg_pending_t **t, dyn_pg_pending_t *p)
{
    p->next = NULL;
    if (*t) (*t)->next = p; else *h = p;
    *t = p;
}
static dyn_pg_pending_t *pgp_pop(dyn_pg_pending_t **h, dyn_pg_pending_t **t)
{
    dyn_pg_pending_t *p = *h;
    if (!p) return NULL;
    *h = p->next;
    if (!*h) *t = NULL;
    p->next = NULL;
    return p;
}

static void pg_settle(JSContext *ctx, dyn_pg_pending_t *p, int reject, JSValue v)
{
    JSValue fn = reject ? p->reject : p->resolve;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { v };
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, v);
    pgp_free(ctx, p);
}

static JSValue pg_conn_error(JSContext *ctx, const char *msg)
{
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "CONNECTION"));
    return e;
}

/* ---- writing ------------------------------------------------------------ */

static int pg_flush(dyn_pg_t *g)
{
    int rc;
    if (g->olen == 0 || g->state == PG_ST_CONNECTING || g->state == PG_ST_DEAD)
        return 0;
    rc = dyn_aio_send(g->aio, g->fd, g->obuf, g->olen, 0, NULL, NULL);
    if (rc < 0)
        return rc;
    g->olen = 0;
    return 0;
}

static void pg_flush_drop(dyn_pg_t *g)
{
    int i;
    g->flush_queued = 0;
    for (i = 0; i < pg_n_flush; i++)
        if (pg_flush_pending[i] == g) {
            pg_flush_pending[i] = pg_flush_pending[pg_n_flush - 1];
            pg_n_flush--;
            return;
        }
}

static JSValue pg_flush_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    /* One job drains every waiting connection: several may have queued in the
     * same turn, and each job would otherwise flush only its own. */
    while (pg_n_flush > 0) {
        dyn_pg_t *g = pg_flush_pending[0];
        pg_flush_drop(g);
        if (g->state == PG_ST_READY && pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
    }
    return JS_UNDEFINED;
}

/* MEASURED: 2000 queries issued in one turn cost one send() each. Deferring the
 * flush to a job that runs at the end of the same turn collapses them into one
 * write, before the loop can poll, so no reply waits longer. */
static void pg_flush_soon(dyn_pg_t *g)
{
    if (g->flush_queued || g->olen == 0 || g->state != PG_ST_READY)
        return;
    if (pg_n_flush >= PG_MAX_FLUSH ||
        JS_EnqueueJob(g->ctx, pg_flush_job, 0, NULL) < 0) {
        if (pg_flush(g) < 0)                 /* no room, or no job queue */
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }
    g->flush_queued = 1;
    pg_flush_pending[pg_n_flush++] = g;
}

static int pg_write(dyn_pg_t *g, const uint8_t *b, size_t n)
{
    if (pgbuf_reserve(&g->obuf, &g->ocap, g->olen + n) < 0)
        return -1;
    memcpy(g->obuf + g->olen, b, n);
    g->olen += n;
    return 0;
}

/* Authentication replies are not queries: they carry no promise and must go out
 * ahead of anything the caller queued. */
static int pg_send_now(dyn_pg_t *g, const uint8_t *b, size_t n)
{
    if (pg_write(g, b, n) < 0)
        return -1;
    return pg_flush(g);
}

/* ---- ErrorResponse ------------------------------------------------------ */

/* Field types are one byte; the doc says a frontend must silently ignore ones
 * it does not know, because more get added. */
static JSValue pg_error_value(JSContext *ctx, const uint8_t *p, size_t len)
{
    JSValue e = JS_NewError(ctx);
    size_t i = 0;
    const uint8_t *sev_s = NULL;   /* localised; used only if V never arrives */
    size_t sev_s_len = 0;
    int have_v = 0;
    while (i < len && p[i] != 0) {
        char code = (char)p[i];
        size_t s = ++i, n;
        while (i < len && p[i] != 0)
            i++;
        n = i - s;
        if (i < len)
            i++;                       /* the field's NUL */
        switch (code) {
        case 'M': JS_SetPropertyStr(ctx, e, "message",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'C': JS_SetPropertyStr(ctx, e, "code",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'V': have_v = 1;
                  JS_SetPropertyStr(ctx, e, "severity",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        /* S is always present; V only from 9.6 on, and not at all from some
         * protocol-compatible servers and poolers. Prefer V (never localised)
         * but do not drop the field when only S came. */
        case 'S': sev_s = p + s; sev_s_len = n; break;
        case 'D': JS_SetPropertyStr(ctx, e, "detail",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'H': JS_SetPropertyStr(ctx, e, "hint",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'P': JS_SetPropertyStr(ctx, e, "position",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 's': JS_SetPropertyStr(ctx, e, "schema",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 't': JS_SetPropertyStr(ctx, e, "table",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'c': JS_SetPropertyStr(ctx, e, "column",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        case 'n': JS_SetPropertyStr(ctx, e, "constraint",
                      JS_NewStringLen(ctx, (const char *)p + s, n)); break;
        default: break;                /* unknown field types are ignored */
        }
    }
    if (!have_v && sev_s)
        JS_SetPropertyStr(ctx, e, "severity",
                          JS_NewStringLen(ctx, (const char *)sev_s, sev_s_len));
    return e;
}

/* ---- authentication ----------------------------------------------------- */

/* PostgreSQL's MD5 scheme: md5(md5(password + user) + salt), hex, prefixed. */
static void pg_md5_auth(const char *pass, const char *user,
                        const uint8_t salt[4], char out[36])
{
    uint8_t d1[16], d2[16];
    char h1[33];
    size_t n1 = strlen(pass), n2 = strlen(user);
    uint8_t *tmp = (uint8_t *)malloc(n1 + n2 + 16);
    if (!tmp) { out[0] = '\0'; return; }
    memcpy(tmp, pass, n1);
    memcpy(tmp + n1, user, n2);
    dyn_md5(tmp, n1 + n2, d1);
    dyn_codec_hex_encode(d1, 16, h1);
    h1[32] = '\0';
    memcpy(tmp, h1, 32);
    memcpy(tmp + 32, salt, 4);
    dyn_md5(tmp, 36, d2);
    memcpy(out, "md5", 3);
    dyn_codec_hex_encode(d2, 16, out + 3);
    out[35] = '\0';
    free(tmp);
}

static void pg_handle_auth(dyn_pg_t *g, const uint8_t *body, size_t len)
{
    uint32_t code;
    pgw_t w;

    if (len < 4) { pg_fail_all(g, "PostgreSQL: truncated authentication request"); return; }
    code = get32(body);
    memset(&w, 0, sizeof(w));

    switch (code) {
    case PG_AUTH_OK:
        /* A server can simply OMIT AuthenticationSASLFinal. Accepting the OK
         * without it throws away the half of SCRAM that authenticates the
         * SERVER -- the client would then hand its statements and data to
         * something that never proved it knows the password. */
        if (g->scram_live && !g->scram_done) {
            pg_fail_all(g, "PostgreSQL: the server skipped the SCRAM final "
                           "message, so it never proved it knows the password");
            return;
        }
        return;                        /* the handshake continues to ReadyForQuery */

    case PG_AUTH_CLEARTEXT:
        /* Without TLS this hands the password to anything on the path. The
         * refusal is the DEFAULT; accepting it must be asked for by name. */
        if (!g->insecure_auth || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for a cleartext password; "
                           "this client has no TLS, so that is refused unless "
                           "insecureAuth is set");
            return;
        }
        { size_t at = pgw_begin(&w, 'p'); pgw_str(&w, g->pass); pgw_end(&w, at); }
        break;

    case PG_AUTH_MD5:
        /* MD5 over a 4-byte salt, on a plaintext link. Same rule. */
        if (!g->insecure_auth || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for MD5 authentication; "
                           "this client requires SCRAM-SHA-256 unless "
                           "insecureAuth is set");
            return;
        }
        if (len < 8) { pg_fail_all(g, "PostgreSQL: truncated MD5 salt"); return; }
        {
            char hashed[36];
            size_t at;
            pg_md5_auth(g->pass, g->user ? g->user : "", body + 4, hashed);
            at = pgw_begin(&w, 'p');
            pgw_str(&w, hashed);
            pgw_end(&w, at);
        }
        break;

    case PG_AUTH_SASL: {
        /* The mechanism list is NUL-separated and terminated by a second NUL.
         * SCRAM-SHA-256-PLUS may be advertised whenever the server was built
         * with SSL, whether or not THIS connection is encrypted -- so its
         * presence tells a TLS-less client nothing and is not consulted. */
        char cfirst[256];
        size_t at, i = 4;
        int found = 0;
        while (i < len && body[i]) {
            size_t s = i;
            while (i < len && body[i]) i++;
            if (i - s == 13 && memcmp(body + s, "SCRAM-SHA-256", 13) == 0)
                found = 1;
            if (i < len) i++;
        }
        if (!found) {
            pg_fail_all(g, "PostgreSQL: the server offers no SCRAM-SHA-256; "
                           "this client implements no other SASL mechanism");
            return;
        }
        if (!g->pass) {
            pg_fail_all(g, "PostgreSQL: the server requires a password and none "
                           "was given");
            return;
        }
        if (dyn_scram_client_first(&g->scram, cfirst, sizeof(cfirst)) < 0) {
            pg_fail_all(g, "PostgreSQL: cannot begin SCRAM");
            return;
        }
        g->scram_live = 1;
        at = pgw_begin(&w, 'p');
        pgw_str(&w, "SCRAM-SHA-256");
        {
            size_t cf_len = strlen(cfirst);
            pgw_u32(&w, (uint32_t)cf_len);
            pgw_raw(&w, cfirst, cf_len);
        }
        pgw_end(&w, at);
        break;
    }

    case PG_AUTH_SASL_CONT: {
        char cfinal[1024];
        int n;
        size_t at;
        if (!g->scram_live) {
            pg_fail_all(g, "PostgreSQL: SASL continue with no exchange in progress");
            return;
        }
        n = dyn_scram_server_first(&g->scram, (const char *)body + 4, len - 4,
                                   g->pass, cfinal, sizeof(cfinal));
        if (n < 0) {
            pg_fail_all(g, dyn_scram_strerror(n));
            return;
        }
        at = pgw_begin(&w, 'p');
        pgw_raw(&w, cfinal, (size_t)n);
        pgw_end(&w, at);
        break;
    }

    case PG_AUTH_SASL_FINAL: {
        /* Verifying this is what authenticates the SERVER. Skipping it keeps
         * only the password-hiding half of SCRAM. */
        int rc;
        if (!g->scram_live) {
            pg_fail_all(g, "PostgreSQL: SASL final with no exchange in progress");
            return;
        }
        rc = dyn_scram_server_final(&g->scram, (const char *)body + 4, len - 4);
        if (rc != DYN_SCRAM_OK) {
            pg_fail_all(g, rc == DYN_SCRAM_E_VERIFY
                ? "PostgreSQL: the server failed to prove it knows the password; "
                  "this is what a relayed connection looks like"
                : dyn_scram_strerror(rc));
            return;
        }
        g->scram_done = 1;
        return;
    }

    default:
        pg_fail_all(g, "PostgreSQL: unsupported authentication request");
        return;
    }

    if (w.bad || pg_send_now(g, w.b, w.len) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the authentication reply");
    free(w.b);
}

/* ---- rows --------------------------------------------------------------- */

/* Locale-independent decimal -> double; see the note at its use. Returns 0 on
 * anything it does not fully consume, so the caller falls back to text rather
 * than inventing a number. */
/* PostgreSQL float text -> double, EXACTLY.
 *
 * NOT strtod: it reads LC_NUMERIC for the radix character, so under a locale
 * where that is ',' every float silently loses everything after the '.', set
 * by some unrelated module calling setlocale months later. It is also far too
 * permissive for a wire grammar (0x10, leading space, partial input).
 *
 * NOT a hand-rolled digit accumulator either, which is what this was: `v =
 * v*10 + d` then a divide cannot round-trip a double, and a differential
 * against the binary format caught it -- text gave -1.0000000000000002e+300
 * where the wire said -1e300. js_atod is the engine's own JS number parser:
 * correctly rounded by construction and locale-independent, because the
 * ECMAScript grammar has no locale.
 *
 * The grammar is still ours, not js_atod's: the input must be ENTIRELY
 * consumed, so 0x10, "1 2" and "" are all refused. */
static int pg_parse_double(const uint8_t *p, size_t n, double *out)
{
    char stackbuf[64], *s = stackbuf, *heap = NULL;
    const char *end = NULL;
    JSATODTempMem tmp;
    double v;

    if (n == 0)
        return 0;
    /* PostgreSQL spells the specials out; js_atod does not know them and the
     * old parser returned them as strings, which changed a column's JS TYPE
     * depending on its value. */
    if (n == 3 && memcmp(p, "NaN", 3) == 0)        { *out = NAN;  return 1; }
    if (n == 8 && memcmp(p, "Infinity", 8) == 0)   { *out = INFINITY;  return 1; }
    if (n == 9 && memcmp(p, "-Infinity", 9) == 0)  { *out = -INFINITY; return 1; }

    if (n + 1 > sizeof(stackbuf)) {
        heap = (char *)malloc(n + 1);
        if (!heap)
            return 0;
        s = heap;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    /* radix 10 and no flags: no 0x, no underscores, no leading whitespace. */
    v = js_atod(s, &end, 10, 0, &tmp);
    if (!end || end != s + n || v != v) {
        /* v != v is a NaN test. js_atod implements the ECMAScript grammar,
         * which is not PostgreSQL's: it tolerates `1e` and returns NaN. The
         * specials are spelled out and handled above, so a NaN here means the
         * parser accepted a shape the wire cannot produce. */
        free(heap);
        return 0;
    }
    free(heap);
    *out = v;
    return 1;
}

static JSValue pg_bytes_u8(JSContext *ctx, const uint8_t *p, size_t n)
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

/* The `\x<hex>` spelling the TEXT path would have produced, so a caller that
 * did not ask for bytes sees the same value whichever format the wire used. */
static JSValue pg_bytea_to_hex_string(JSContext *ctx, const uint8_t *p, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    char stackbuf[128], *s = stackbuf, *heap = NULL;
    JSValue out;
    size_t i, need = n * 2 + 2;
    if (need > sizeof(stackbuf)) {
        heap = (char *)malloc(need);
        if (!heap) return JS_ThrowOutOfMemory(ctx);
        s = heap;
    }
    s[0] = '\\'; s[1] = 'x';
    for (i = 0; i < n; i++) {
        s[2 + i * 2] = hex[p[i] >> 4];
        s[3 + i * 2] = hex[p[i] & 15];
    }
    out = JS_NewStringLen(ctx, s, need);
    free(heap);
    return out;
}

static JSValue pg_bytea(JSContext *ctx, const uint8_t *p, size_t n)
{
    static const int8_t hexval[256] = {
        ['0']=0,['1']=1,['2']=2,['3']=3,['4']=4,['5']=5,['6']=6,['7']=7,
        ['8']=8,['9']=9,['a']=10,['b']=11,['c']=12,['d']=13,['e']=14,['f']=15,
        ['A']=10,['B']=11,['C']=12,['D']=13,['E']=14,['F']=15,
    };
    size_t i, nb;
    JSValue ab, ta;
    JSValueConst a3[3];
    uint8_t *out;

    if (n < 2 || p[0] != '\\' || p[1] != 'x' || (n & 1))
        return JS_UNDEFINED;
    for (i = 2; i < n; i++)
        if (!hexval[p[i]] && p[i] != '0')
            return JS_UNDEFINED;
    nb = (n - 2) / 2;
    out = (uint8_t *)js_malloc(ctx, nb ? nb : 1);
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < nb; i++)
        out[i] = (uint8_t)((hexval[p[2 + i * 2]] << 4) | hexval[p[3 + i * 2]]);
    ab = JS_NewArrayBufferCopy(ctx, out, nb);
    js_free(ctx, out);
    if (JS_IsException(ab))
        return ab;
    /* three arguments: with one, the view's length defaults to 0 */
    a3[0] = ab; a3[1] = JS_NewInt32(ctx, 0); a3[2] = JS_NewInt32(ctx, (int)nb);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

/* How a bound parameter's storage must be released -- NOT inferable from the
 * pointer, and getting it wrong is a leak or a free of the wrong allocator. */
#define PG_PF_NONE    0
#define PG_PF_CSTR    1     /* JS_FreeCString */
#define PG_PF_MALLOC  2     /* free */

static void pg_free_params(JSContext *ctx, const char **pv, const uint8_t *pf,
                           uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        if (!pv[k]) continue;
        if (pf[k] == PG_PF_CSTR) JS_FreeCString(ctx, pv[k]);
        else if (pf[k] == PG_PF_MALLOC) free((void *)pv[k]);
    }
}

/* Encode a TypedArray, DataView or ArrayBuffer as the "\x<hex>" literal the
 * server parses as bytea. Returns 1 encoded, 0 not a byte view, -1 thrown. */
static int pg_param_bytea(JSContext *ctx, JSValueConst v, const char **out,
                          size_t *outlen)
{
    static const char hex[] = "0123456789abcdef";
    size_t off = 0, len = 0, bpe = 0, total = 0, i;
    uint8_t *base;
    char *s;
    JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &total, v);      /* a bare ArrayBuffer */
        if (!base) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
        off = 0; len = total;
    } else {
        uint8_t *b = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!b) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
        base = b;
    }
    if (len > (SIZE_MAX - 3) / 2) { JS_ThrowOutOfMemory(ctx); return -1; }
    s = (char *)malloc(len * 2 + 3);
    if (!s) { JS_ThrowOutOfMemory(ctx); return -1; }
    s[0] = '\\'; s[1] = 'x';
    for (i = 0; i < len; i++) {
        s[2 + i * 2]     = hex[base[off + i] >> 4];
        s[3 + i * 2]     = hex[base[off + i] & 15];
    }
    s[2 + len * 2] = '\0';
    *out = s;
    *outlen = len * 2 + 2;
    return 1;
}

/* Decode a column the server sent in BINARY. Every width is checked against
 * the declared type: the length is a number the peer chose, and a float8 that
 * arrives as 4 bytes must be refused rather than read past. Anything
 * unrecognised or mis-sized falls through to the raw bytes, which is visible
 * rather than silently wrong. All integers are network byte order. */
static JSValue pg_column_binary(JSContext *ctx, dyn_pg_t *g, uint32_t oid,
                                const uint8_t *p, size_t n)
{
    switch (oid) {
    case PG_OID_BOOL:
        if (n == 1) return JS_NewBool(ctx, p[0] != 0);
        break;
    case PG_OID_BYTEA:
        /* Binary bytea is the whole point: no \x hex doubling on the wire and
         * no hex decode here. Still a string unless the caller asked for
         * bytes, so the text and binary paths agree on the JS type. */
        if (g->bytes_out)
            return pg_bytes_u8(ctx, p, n);
        return pg_bytea_to_hex_string(ctx, p, n);
    case PG_OID_INT2:
        if (n == 2) return JS_NewInt32(ctx, (int16_t)((p[0] << 8) | p[1]));
        break;
    case PG_OID_INT4:
        if (n == 4) return JS_NewInt32(ctx, (int32_t)get32(p));
        break;
    case PG_OID_OID:
        if (n == 4) return JS_NewUint32(ctx, get32(p));
        break;
    case PG_OID_INT8:
        if (n == 8) {
            int64_t v = (int64_t)(((uint64_t)get32(p) << 32) | get32(p + 4));
            if (g->bigint)
                return JS_NewBigInt64(ctx, v);
            if (v > 9007199254740992LL || v < -9007199254740992LL) {
                char b[24];
                snprintf(b, sizeof(b), "%lld", (long long)v);
                return JS_NewString(ctx, b);
            }
            return JS_NewInt64(ctx, v);
        }
        break;
    case PG_OID_FLOAT4:
        if (n == 4) { uint32_t u = get32(p); float f; memcpy(&f, &u, 4);
                      return JS_NewFloat64(ctx, (double)f); }
        break;
    case PG_OID_FLOAT8:
        if (n == 8) { uint64_t u = ((uint64_t)get32(p) << 32) | get32(p + 4);
                      double d; memcpy(&d, &u, 8);
                      return JS_NewFloat64(ctx, d); }
        break;
    case PG_OID_UUID:
        if (n == 16) {
            static const char hex[] = "0123456789abcdef";
            char s[37]; int i, k = 0;
            for (i = 0; i < 16; i++) {
                if (i == 4 || i == 6 || i == 8 || i == 10) s[k++] = '-';
                s[k++] = hex[p[i] >> 4];
                s[k++] = hex[p[i] & 15];
            }
            return JS_NewStringLen(ctx, s, 36);
        }
        break;
    default:
        break;
    }
    /* Unknown type, or a width that contradicts it: hand back the raw bytes
     * rather than guess. A wrong number is worse than an obviously odd value. */
    return pg_bytes_u8(ctx, p, n);
}

static JSValue pg_column(JSContext *ctx, dyn_pg_t *g, uint32_t oid,
                         const uint8_t *p, size_t n)
{
    char tmp[48];
    if (g->raw)
        return JS_NewStringLen(ctx, (const char *)p, n);
    switch (oid) {
    case PG_OID_BYTEA:
        if (g->bytes_out) {
            JSValue b = pg_bytea(ctx, p, n);
            if (!JS_IsUndefined(b))
                return b;
        }
        return JS_NewStringLen(ctx, (const char *)p, n);
    case PG_OID_BOOL:
        return JS_NewBool(ctx, n == 1 && p[0] == 't');
    case PG_OID_INT2: case PG_OID_INT4:
    case PG_OID_FLOAT4: case PG_OID_FLOAT8: {
        double d;
        /* NOT strtod: it reads LC_NUMERIC for the radix character, so under a
         * locale where that is ',' every float would silently lose everything
         * after the '.' -- and the setlocale that caused it would be in some
         * other module entirely. PostgreSQL always sends '.', so parse '.'. */
        if (n == 0 || !pg_parse_double(p, n, &d))
            return JS_NewStringLen(ctx, (const char *)p, n);
        return JS_NewFloat64(ctx, d);
    }
    case PG_OID_INT8: {
        /* int8 is 64-bit; past 2^53 a double rounds. `bigint` returns the
         * exact value with its type, otherwise the digits stay text. */
        long long v;
        char *end;
        if (n == 0 || n >= sizeof(tmp))
            return JS_NewStringLen(ctx, (const char *)p, n);
        memcpy(tmp, p, n); tmp[n] = '\0';
        v = strtoll(tmp, &end, 10);
        if (*end)
            return JS_NewStringLen(ctx, (const char *)p, n);
        if (g->bigint)
            return JS_NewBigInt64(ctx, (int64_t)v);
        if (v > 9007199254740992LL || v < -9007199254740992LL)
            return JS_NewStringLen(ctx, (const char *)p, n);
        return JS_NewInt64(ctx, v);
    }
    default:
        /* numeric, timestamps, json, arrays: text is EXACT, and inventing a
         * conversion for each would be a second thing to keep correct. */
        return JS_NewStringLen(ctx, (const char *)p, n);
    }
}

/* Remember this statement's result column types, so the NEXT execution can ask
 * for binary without an extra round trip. Learned once; if a schema change
 * alters them the server rejects the plan and the entry is evicted whole. */
static void pg_stmt_learn_oids(dyn_pg_t *g, const char *name,
                               const uint32_t *oids, int n)
{
    int i;
    if (!name || !name[0] || n <= 0)
        return;
    for (i = 0; i < g->nstmts; i++) {
        struct pg_stmt *s = &g->stmts[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->oids)
            return;                     /* already learned */
        s->oids = (uint32_t *)malloc((size_t)n * sizeof(*s->oids));
        if (!s->oids)
            return;
        memcpy(s->oids, oids, (size_t)n * sizeof(*s->oids));
        s->noids = n;
        return;
    }
}

static void pg_row_description(dyn_pg_t *g, dyn_pg_pending_t *p,
                               const uint8_t *b, size_t len)
{
    JSContext *ctx = g->ctx;
    uint16_t nf, i;
    size_t at = 2;

    if (len < 2) {
        pg_fail_all(g, "PostgreSQL: truncated RowDescription");
        return;
    }
    nf = get16(b);
    JS_FreeValue(ctx, p->fields);
    p->fields = JS_NewArray(ctx);
    pgp_free_fields(ctx, p);
    if (nf) {
        p->fatom = (JSAtom *)calloc(nf, sizeof(*p->fatom));
        p->foid = (uint32_t *)calloc(nf, sizeof(*p->foid));
        p->fformat = (uint8_t *)calloc(nf, 1);
        if (!p->fatom || !p->foid || !p->fformat) {
            pgp_free_fields(ctx, p);
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
        }
    }
    for (i = 0; i < nf; i++) {
        size_t s = at;
        JSValue f;
        while (at < len && b[at] != 0)
            at++;
        /* The framing loop already proved the whole body is here, so a field
         * that runs off the end means the message contradicts its own length.
         * Leaving a short `fields` behind makes that look like real data. */
        if (at >= len) {
            pg_fail_all(g, "PostgreSQL: malformed RowDescription");
            return;
        }
        f = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, f, "name",
                          JS_NewStringLen(ctx, (const char *)b + s, at - s));
        /* The atom is interned once here and reused for every row. */
        p->fatom[i] = JS_NewAtomLen(ctx, (const char *)b + s, at - s);
        p->nfield = i + 1;
        at++;
        if (at + 18 > len) {
            JS_FreeValue(ctx, f);
            pg_fail_all(g, "PostgreSQL: malformed RowDescription");
            return;
        }
        JS_SetPropertyStr(ctx, f, "tableOid", JS_NewUint32(ctx, get32(b + at)));
        JS_SetPropertyStr(ctx, f, "column", JS_NewUint32(ctx, get16(b + at + 4)));
        JS_SetPropertyStr(ctx, f, "typeOid", JS_NewUint32(ctx, get32(b + at + 6)));
        JS_SetPropertyStr(ctx, f, "format", JS_NewUint32(ctx, get16(b + at + 16)));
        p->foid[i] = get32(b + at + 6);
        /* Trust the SERVER's format code, never the one we asked for: if it
         * ignored the request, decoding as binary would read garbage. */
        p->fformat[i] = get16(b + at + 16) ? 1 : 0;
        JS_SetPropertyUint32(ctx, p->fields, i, f);
        at += 18;
    }
    pg_stmt_learn_oids(g, p->stmt_name, p->foid, p->nfield);
}

static void pg_data_row(dyn_pg_t *g, dyn_pg_pending_t *p,
                        const uint8_t *b, size_t len)
{
    JSContext *ctx = g->ctx;
    uint16_t nc, i;
    size_t at = 2;
    JSValue row;

    if (len < 2) {
        pg_fail_all(g, "PostgreSQL: truncated DataRow");
        return;
    }
    nc = get16(b);
    row = JS_NewObject(ctx);
    for (i = 0; i < nc; i++) {
        uint32_t clen;
        JSValue v;
        if (at + 4 > len) {
            JS_FreeValue(ctx, row);
            pg_fail_all(g, "PostgreSQL: malformed DataRow");
            return;
        }
        clen = get32(b + at);
        at += 4;
        if (clen == 0xffffffffu) {              /* -1 is NULL, not empty */
            v = JS_NULL;
        } else {
            if (clen > len - at) {
                JS_FreeValue(ctx, row);
                pg_fail_all(g, "PostgreSQL: DataRow column runs past the message");
                return;
            }
            /* The type comes from the cache the RowDescription filled, not
             * from a walk of the `fields` array: that walk was three property
             * reads and a conversion PER CELL. A DataRow with more columns
             * than the description is the server contradicting itself, so
             * fall back to 0 (text) rather than index past the cache. */
            if (i < p->nfield && p->fformat[i])
                v = pg_column_binary(ctx, g, p->foid[i], b + at, clen);
            else
                v = pg_column(ctx, g, i < p->nfield ? p->foid[i] : 0, b + at, clen);
            at += clen;
        }
        if (i < p->nfield && p->fatom[i] != JS_ATOM_NULL) {
            /* DEFINE: a column literally named __proto__ would otherwise
             * retarget the row's prototype and vanish. */
            JS_DefinePropertyValue(ctx, row, p->fatom[i], v, JS_PROP_C_W_E);
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    if (JS_IsUndefined(p->rows))
        p->rows = JS_NewArray(ctx);
    /* rowcount IS the next index; reading .length back was a property get and
     * a conversion per row to recompute a number already held. */
    JS_DefinePropertyValueUint32(ctx, p->rows, (uint32_t)p->rowcount, row,
                                 JS_PROP_C_W_E);
    p->rowcount++;
}

/* ---- the message loop --------------------------------------------------- */

static void pg_ready_for_query(dyn_pg_t *g, char status)
{
    JSContext *ctx = g->ctx;
    dyn_pg_pending_t *p;

    g->tx_status = status;
    if (g->state != PG_ST_READY) {
        g->state = PG_ST_READY;
        g->connect_deadline_ms = 0;
        /* Release what was queued before the handshake finished. */
        while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
            g->nwait--;
            if (p->bytes && pg_write(g, p->bytes, p->nbytes) < 0) {
                pgp_push(&g->wq_head, &g->wq_tail, p);
                g->nwait++;
                pg_fail_all(g, "PostgreSQL: out of memory");
                return;
            }
            free(p->bytes); p->bytes = NULL; p->nbytes = 0;
            if (g->query_timeout_ms)
                p->deadline_ms = dyn_timer_now_ms() + g->query_timeout_ms;
            pgp_push(&g->head, &g->tail, p);
            g->npending++;
        }
        if (pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }

    /* ReadyForQuery ends exactly one query, simple or extended. */
    p = pgp_pop(&g->head, &g->tail);
    if (!p)
        return;
    g->npending--;
    if (!JS_IsUndefined(p->error)) {
        JSValue e = p->error;
        p->error = JS_UNDEFINED;
        pg_settle(ctx, p, 1, e);
        return;
    }
    {
        JSValue res = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, res, "rows",
                          JS_IsUndefined(p->rows) ? JS_NewArray(ctx) : p->rows);
        p->rows = JS_UNDEFINED;
        JS_SetPropertyStr(ctx, res, "fields",
                          JS_IsUndefined(p->fields) ? JS_NewArray(ctx) : p->fields);
        p->fields = JS_UNDEFINED;
        JS_SetPropertyStr(ctx, res, "command", JS_NewString(ctx, p->tag));
        JS_SetPropertyStr(ctx, res, "rowCount", JS_NewInt32(ctx, p->rowcount));
        pg_settle(ctx, p, 0, res);
    }
}

static void pg_call1(dyn_pg_t *g, JSValue fn, JSValue arg)
{
    JSContext *ctx = g->ctx;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { arg };
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arg);
}

static void pg_message(dyn_pg_t *g, char type, const uint8_t *b, size_t len)
{
    JSContext *ctx = g->ctx;
    dyn_pg_pending_t *p = g->head;

    switch (type) {
    case 'R': pg_handle_auth(g, b, len); return;
    case 'v':
        /* NegotiateProtocolVersion. We asked for 3.0, the lowest a server
         * supports, so this can only be an unrecognised _pq_ option -- but it
         * MUST be consumed or the next message is read at the wrong offset. */
        return;
    case 'S':
        if (len > 1) {
            size_t i = 0;
            while (i < len && b[i]) i++;
            if (i + 1 < len) {
                size_t j = i + 1, s = j;
                while (j < len && b[j]) j++;
                /* DEFINE, not set: the name is server-controlled, and
                 * "__proto__" would retarget this object's prototype. */
                JS_DefinePropertyValueStr(ctx, g->params,
                    (const char *)b,   /* NUL-terminated inside the message */
                    JS_NewStringLen(ctx, (const char *)b + s, j - s),
                    JS_PROP_C_W_E);
            }
        }
        return;
    case 'K':
        if (len >= 4) {
            size_t klen = len - 4;
            g->backend_pid = get32(b);
            /* SIZED FROM THE MESSAGE: 4 octets on 3.0, 32 on 3.2, up to 256
             * from a pooler. A fixed width here desynchronises everything. */
            if (klen > sizeof(g->cancel_key))
                klen = sizeof(g->cancel_key);
            memcpy(g->cancel_key, b + 4, klen);
            g->cancel_key_len = klen;
        }
        return;
    case 'Z':
        pg_ready_for_query(g, len >= 1 ? (char)b[0] : 'I');
        return;
    case 'T':
        /* A second result set in one query. `rows` would concatenate while
         * `fields` described only the last, so the caller gets one array whose
         * shape changes partway through. Refusing names the cause. */
        if (p && !JS_IsUndefined(p->fields)) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per query; "
                           "the server returned a second result set");
            return;
        }
        if (p) pg_row_description(g, p, b, len);
        return;
    case 'D': if (p) pg_data_row(g, p, b, len); return;
    case 'C':
        if (p && ++p->ncomplete > 1) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per query; "
                           "the server completed a second command");
            return;
        }
        if (p) {
            size_t n = len;
            while (n && b[n - 1] == 0) n--;
            if (n >= sizeof(p->tag)) n = sizeof(p->tag) - 1;
            memcpy(p->tag, b, n);
            p->tag[n] = '\0';
        }
        return;
    case 'I': if (p) { p->tag[0] = '\0'; } return;
    case 'E': {
        JSValue e = pg_error_value(ctx, b, len);
        /* Whatever the error was, this named statement is now suspect: a
         * schema change makes the server refuse a cached plan, and the name
         * may not exist at all. Evicting means the next call re-Parses;
         * keeping it means failing identically for ever. */
        if (p && p->stmt_name[0]) {
            pg_stmt_evict_named(g, p->stmt_name);
            p->stmt_name[0] = '\0';
        }
        if (p && JS_IsUndefined(p->error)) {
            /* Held until ReadyForQuery: the server keeps sending until then,
             * and settling early would leave those messages for the NEXT
             * query to misread. */
            p->error = e;
        } else if (p) {
            JS_FreeValue(ctx, e);
        } else {
            /* Before any query: a failed handshake. */
            if (g->state != PG_ST_READY) {
                JSValue msg = JS_GetPropertyStr(ctx, e, "message");
                const char *m = JS_ToCString(ctx, msg);
                char buf[256];
                snprintf(buf, sizeof(buf), "PostgreSQL: %s", m ? m : "connection rejected");
                if (m) JS_FreeCString(ctx, m);
                JS_FreeValue(ctx, msg);
                JS_FreeValue(ctx, e);
                pg_fail_all(g, buf);
                return;
            }
            pg_call1(g, g->h_error, e);
            return;
        }
        return;
    }
    case 'N': pg_call1(g, g->h_notice, pg_error_value(ctx, b, len)); return;
    case 'A': {
        JSValue o = JS_NewObject(ctx);
        if (len >= 4) {
            size_t i = 4, s;
            JS_SetPropertyStr(ctx, o, "pid", JS_NewUint32(ctx, get32(b)));
            s = i; while (i < len && b[i]) i++;
            JS_SetPropertyStr(ctx, o, "channel",
                              JS_NewStringLen(ctx, (const char *)b + s, i - s));
            if (i < len) i++;
            s = i; while (i < len && b[i]) i++;
            JS_SetPropertyStr(ctx, o, "payload",
                              JS_NewStringLen(ctx, (const char *)b + s, i - s));
        }
        pg_call1(g, g->h_notify, o);
        return;
    }
    /* ParseComplete/BindComplete/CloseComplete/NoData/PortalSuspended/
     * ParameterDescription: acknowledgements with nothing to collect. */
    case '1': case '2': case '3': case 'n': case 's': case 't':
        return;
    /* COPY is not implemented. Naming it beats "unknown message type", which
     * sends the reader to the framing code instead of to the statement. */
    case 'G': case 'H': case 'W': case 'd': case 'c':
        pg_fail_all(g, "PostgreSQL: COPY is not supported by this client");
        return;
    default:
        /* An unknown type byte is not something to guess at on a stream whose
         * framing we are trusting. */
        pg_fail_all(g, "PostgreSQL: unknown message type from the server");
        return;
    }
}

static void pg_on_recv(dyn_aio_t *aio, int res, const uint8_t *buf,
                       unsigned len, void *ud)
{
    dyn_pg_t *g = (dyn_pg_t *)ud;
    (void)aio;

    if (g->state == PG_ST_DEAD)
        return;
    if (res < 0) { pg_fail_all(g, "PostgreSQL: connection error"); return; }
    if (res == 0 && len == 0) {
        pg_fail_all(g, "PostgreSQL: server closed the connection");
        return;
    }
    if (g->rlen - g->rpos + len > g->maxmsg + 5) {
        pg_fail_all(g, "PostgreSQL: message exceeds maxMessageBytes");
        return;
    }
    if (pgbuf_reserve(&g->rbuf, &g->rcap, g->rlen + len) < 0) {
        pg_fail_all(g, "PostgreSQL: out of memory");
        return;
    }
    memcpy(g->rbuf + g->rlen, buf, len);
    g->rlen += len;

    for (;;) {
        uint32_t mlen;
        size_t avail = g->rlen - g->rpos;
        const uint8_t *m = g->rbuf + g->rpos;
        char type;
        if (avail < 5)
            break;
        type = (char)m[0];
        mlen = get32(m + 1);           /* includes itself, excludes the type */
        /* A length under 4 would make the body size negative and, unchecked,
         * turn the loop into a rewind. */
        if (mlen < 4 || mlen > g->maxmsg) {
            pg_fail_all(g, "PostgreSQL: message length out of range");
            return;
        }
        if (avail < (size_t)mlen + 1)
            break;
        pg_message(g, type, m + 5, (size_t)mlen - 4);
        if (g->state == PG_ST_DEAD)
            return;
        g->rpos += (size_t)mlen + 1;
    }
    if (g->rpos) {
        memmove(g->rbuf, g->rbuf + g->rpos, g->rlen - g->rpos);
        g->rlen -= g->rpos;
        g->rpos = 0;
    }
}

static void pg_fail_all(dyn_pg_t *g, const char *msg)
{
    JSContext *ctx = g->ctx;
    dyn_pg_pending_t *p;

    if (g->state == PG_ST_DEAD)
        return;
    g->state = PG_ST_DEAD;
    if (g->fd >= 0) { dyn_aio_close(g->aio, g->fd); g->fd = -1; }
    while ((p = pgp_pop(&g->head, &g->tail)) != NULL) {
        g->npending--;
        pg_settle(ctx, p, 1, pg_conn_error(ctx, msg));
    }
    while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
        g->nwait--;
        pg_settle(ctx, p, 1, pg_conn_error(ctx, msg));
    }
    if (JS_IsFunction(ctx, g->h_error))
        pg_call1(g, g->h_error, pg_conn_error(ctx, msg));
}

/* ---- startup ------------------------------------------------------------ */

static int pg_send_startup(dyn_pg_t *g)
{
    pgw_t w;
    int rc;
    memset(&w, 0, sizeof(w));
    pgw_u32(&w, 0);                    /* length, patched below */
    pgw_u32(&w, PG_PROTO_30);
    pgw_str(&w, "user");     pgw_str(&w, g->user ? g->user : "postgres");
    if (g->database) { pgw_str(&w, "database"); pgw_str(&w, g->database); }
    pgw_str(&w, "client_encoding"); pgw_str(&w, "UTF8");
    if (g->appname) { pgw_str(&w, "application_name"); pgw_str(&w, g->appname); }
    pgw_u8(&w, 0);                     /* the empty key that ends the list */
    if (w.bad) { free(w.b); return -1; }
    if (w.len > PG_MAX_STARTUP) {
        /* The server rejects anything longer, so failing here names the cause
         * instead of getting a FATAL with no context. */
        free(w.b);
        return -1;
    }
    put32(w.b, (uint32_t)w.len);
    rc = pg_write(g, w.b, w.len);
    free(w.b);
    return rc;
}

static void pg_on_connect(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    dyn_pg_t *g = (dyn_pg_t *)ud;
    (void)aio; (void)buf; (void)len;

    if (g->state == PG_ST_DEAD)
        return;
    if (res < 0) { pg_fail_all(g, "PostgreSQL: connect failed"); return; }
    g->state = PG_ST_AUTH;
    if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0) {
        pg_fail_all(g, "PostgreSQL: cannot read from the socket");
        return;
    }
    if (pg_send_startup(g) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the startup message");
}

static void pg_tick(void *udata)
{
    dyn_pg_t *g = (dyn_pg_t *)udata;
    uint64_t now;
    if (g->state == PG_ST_DEAD)
        return;
    now = dyn_timer_now_ms();
    if (g->connect_deadline_ms && now >= g->connect_deadline_ms) {
        pg_fail_all(g, "PostgreSQL: connect timed out");
        return;
    }
    if (g->head && g->head->deadline_ms && now >= g->head->deadline_ms)
        pg_fail_all(g, "PostgreSQL: query timed out");
}

/* ---- JS surface --------------------------------------------------------- */

static dyn_pg_t *pg_this(JSContext *ctx, JSValueConst this_val)
{
    return (dyn_pg_t *)dyn_res_native(ctx, this_val, dyn_pg_class_id);
}

/* One query, encoded. Simple Query when there are no parameters; the extended
 * protocol otherwise, because that is what makes a parameter a VALUE rather
 * than text spliced into the statement. */
/* ---- the statement cache ------------------------------------------------
 * Linear scan over a small array: `stmt_cache_max` is tens, and a hash compare
 * rejects almost every entry before the memcmp. Measure before replacing this
 * with a hash table -- an O(n) scan of 64 entries is not where the time goes.
 */
static uint32_t pg_stmt_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static struct pg_stmt *pg_stmt_find(dyn_pg_t *g, const char *sql, size_t n,
                                    uint32_t h)
{
    int i;
    for (i = 0; i < g->nstmts; i++)
        if (g->stmts[i].hash == h && g->stmts[i].sqllen == n &&
            memcmp(g->stmts[i].sql, sql, n) == 0)
            return &g->stmts[i];
    return NULL;
}

/* Forget one entry. Called when the server rejects a statement -- a schema
 * change invalidates a cached plan (0A000 "cached plan must not change result
 * type") and the NAME may no longer exist (26000). Keyed by NAME, never by a
 * pointer: eviction compacts the array, so a held pointer would dangle. */
static void pg_stmt_evict_named(dyn_pg_t *g, const char *name)
{
    int i;
    if (!name || !name[0])
        return;
    for (i = 0; i < g->nstmts; i++)
        if (strcmp(g->stmts[i].name, name) == 0) {
            free(g->stmts[i].sql);
            free(g->stmts[i].oids);
            g->stmts[i] = g->stmts[--g->nstmts];
            return;
        }
}

static void pg_stmt_clear(dyn_pg_t *g)
{
    int i;
    for (i = 0; i < g->nstmts; i++) {
        free(g->stmts[i].sql);
        free(g->stmts[i].oids);
    }
    free(g->stmts);
    g->stmts = NULL;
    g->nstmts = g->cap_stmts = 0;
}

/* The entry for `sql`, creating it if there is room. NULL means "no cache" --
 * the caller then takes the unnamed arm, which is always correct. */
static struct pg_stmt *pg_stmt_get(dyn_pg_t *g, const char *sql, size_t n)
{
    uint32_t h;
    struct pg_stmt *s;

    if (g->stmt_cache_max <= 0)
        return NULL;
    h = pg_stmt_hash(sql, n);
    s = pg_stmt_find(g, sql, n, h);
    if (s) {
        if (s->uses < 1000000) s->uses++;
        return s;
    }
    if (g->nstmts >= g->stmt_cache_max)
        return NULL;            /* full: stay unnamed rather than thrash */
    if (g->nstmts == g->cap_stmts) {
        int cap = g->cap_stmts ? g->cap_stmts * 2 : 8;
        struct pg_stmt *ns = (struct pg_stmt *)realloc(g->stmts,
                                                       (size_t)cap * sizeof(*ns));
        if (!ns) return NULL;
        g->stmts = ns; g->cap_stmts = cap;
    }
    s = &g->stmts[g->nstmts];
    memset(s, 0, sizeof(*s));
    s->sql = (char *)malloc(n + 1);
    if (!s->sql) return NULL;
    memcpy(s->sql, sql, n); s->sql[n] = '\0';
    s->sqllen = n;
    s->hash = h;
    s->uses = 1;
    snprintf(s->name, sizeof(s->name), "djs%u", ++g->stmt_seq);
    g->nstmts++;
    return s;
}

static int pg_encode_query(pgw_t *w, const char *sql, int nparam,
                           const char **pv, const size_t *plen,
                           const uint8_t *pnull, int extended,
                           const char *stmt_name, int stmt_prepared,
                           const uint8_t *rfmt, int nrfmt)
{
    int i;
    /* Branch on whether the CALLER supplied a parameter array, not on how many
     * it held. The two protocols differ in more than parameters: the simple
     * one runs several statements separated by semicolons, the extended one
     * refuses them. Choosing by length means `query(sql, [])` silently takes
     * the multi-statement path, which is exactly where a caller believes they
     * are parameterised and are not. */
    if (!extended) {
        size_t at = pgw_begin(w, 'Q');
        pgw_str(w, sql);
        pgw_end(w, at);
        return w->bad ? -1 : 0;
    }
    /* Parse ONLY when the server does not already hold this statement. That
     * skip is the whole win: the server stops re-parsing and re-planning the
     * same SQL. `stmt_name` is "" for the one-shot arm, which is byte-for-byte
     * what this always sent. */
    if (!stmt_prepared) {
        size_t at = pgw_begin(w, 'P');
        pgw_str(w, stmt_name ? stmt_name : "");
        pgw_str(w, sql);
        pgw_u16(w, 0);                 /* let the server infer the types */
        pgw_end(w, at);
    }
    { size_t at = pgw_begin(w, 'B');
      pgw_u8(w, 0);                    /* unnamed portal */
      pgw_str(w, stmt_name ? stmt_name : "");
      pgw_u16(w, 0);                   /* all parameters in TEXT format */
      pgw_u16(w, (uint16_t)nparam);
      for (i = 0; i < nparam; i++) {
          if (pnull[i]) {
              pgw_u32(w, 0xffffffffu); /* -1 is NULL; 0 would be an empty string */
          } else {
              pgw_u32(w, (uint32_t)plen[i]);
              pgw_raw(w, pv[i], plen[i]);
          }
      }
      /* Result-column format codes. R==0 means "all text", which is what this
       * sent before and still sends until the types are known. Once the
       * statement cache has learned them from a previous execution's
       * RowDescription, emit one code PER COLUMN -- binary for the types that
       * gain by it, text for the rest, because binary costs ~16% on text-like
       * types. R must equal the column count exactly or the server errors. */
      if (rfmt && nrfmt > 0) {
          int k;
          pgw_u16(w, (uint16_t)nrfmt);
          for (k = 0; k < nrfmt; k++) pgw_u16(w, rfmt[k] ? 1 : 0);
      } else {
          pgw_u16(w, 0);               /* all results in text format */
      }
      pgw_end(w, at); }
    { size_t at = pgw_begin(w, 'D'); pgw_u8(w, 'P'); pgw_u8(w, 0); pgw_end(w, at); }
    { size_t at = pgw_begin(w, 'E'); pgw_u8(w, 0); pgw_u32(w, 0); pgw_end(w, at); }
    { size_t at = pgw_begin(w, 'S'); pgw_end(w, at); }
    return w->bad ? -1 : 0;
}

static JSValue dyn_pg_query(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_pg_t *g;
    const char *sql = NULL;
    const char **pv = NULL;
    size_t *plen = NULL;
    uint8_t *pnull = NULL, *pfree = NULL;
    uint32_t nparam = 0, i;
    struct pg_stmt *stmt = NULL;
    const char *stmt_name = NULL;
    int stmt_prepared = 0;
    uint8_t *rfmt = NULL;
    int nrfmt = 0;
    int have_params = 0;
    pgw_t w;
    dyn_pg_pending_t *p;
    JSValue funcs[2], promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "query: a statement is required");

    /* Coerce EVERYTHING first: a toString hook runs arbitrary JS and can close
     * this connection before the native handle is even resolved. */
    sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue lv;
        if (!JS_IsArray(ctx, argv[1])) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "query: parameters must be an array");
        }
        have_params = 1;
        lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_ToUint32(ctx, &nparam, lv) < 0) {
            JS_FreeValue(ctx, lv); JS_FreeCString(ctx, sql); return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (nparam > 65535) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowRangeError(ctx, "query: at most 65535 parameters");
        }
    }
    if (nparam) {
        pv = (const char **)calloc(nparam, sizeof(*pv));
        plen = (size_t *)calloc(nparam, sizeof(*plen));
        pnull = (uint8_t *)calloc(nparam, 1);
        pfree = (uint8_t *)calloc(nparam, 1);
        if (!pv || !plen || !pnull || !pfree) {
            free(pv); free(plen); free(pnull); free(pfree);
            JS_FreeCString(ctx, sql);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < nparam; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
            if (JS_IsUndefined(e) || JS_IsNull(e)) {
                pnull[i] = 1;
                JS_FreeValue(ctx, e);
                continue;
            }
            /* A byte view is the one object with an exact text form: the
             * server parses "\x<hex>" as bytea whatever bytea_output says. */
            if (JS_IsObject(e)) {
                int enc = pg_param_bytea(ctx, e, &pv[i], &plen[i]);
                if (enc > 0) { pfree[i] = PG_PF_MALLOC; JS_FreeValue(ctx, e); continue; }
                if (enc < 0) { JS_FreeValue(ctx, e); goto param_fail; }
                /* Everything else stringifies to "[object Object]" or "1,2":
                 * both store cleanly and both are wrong. Refuse, and name the
                 * conversion, so the cost lands on a line the caller wrote. */
                JS_FreeValue(ctx, e);
                pg_free_params(ctx, pv, pfree, i);
                free(pv); free(plen); free(pnull); free(pfree);
                JS_FreeCString(ctx, sql);
                return JS_ThrowTypeError(ctx,
                    "query: parameter %u is an object; a parameter is a value. "
                    "Pass a Uint8Array or ArrayBuffer for bytea, "
                    "JSON.stringify(v) for json/jsonb, "
                    "or an ISO string for a timestamp", (unsigned)i + 1);
            }
            pv[i] = JS_ToCStringLen(ctx, &plen[i], e);
            pfree[i] = PG_PF_CSTR;
            JS_FreeValue(ctx, e);
            if (!pv[i]) {
param_fail:
                pg_free_params(ctx, pv, pfree, i);
                free(pv); free(plen); free(pnull); free(pfree);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
        }
    }

    g = pg_this(ctx, this_val);
    if (!g || g->state == PG_ST_DEAD) {
        pg_free_params(ctx, pv, pfree, nparam);
        free(pv); free(plen); free(pnull); free(pfree); JS_FreeCString(ctx, sql);
        return g ? JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed")
                 : JS_EXCEPTION;
    }
    if (g->npending + g->nwait >= g->maxpending) {
        pg_free_params(ctx, pv, pfree, nparam);
        free(pv); free(plen); free(pnull); free(pfree); JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: %d queries already in flight (maxPending)", g->maxpending);
    }

    memset(&w, 0, sizeof(w));
    /* ---- STRATEGY SELECTION -------------------------------------------
     * Unnamed: one round trip, no server-side state, nothing to invalidate.
     * Right for a one-shot query, and it is what this always did.
     * Named: the server keeps the parse and the plan. Right once the same
     * SQL has been seen `prepare_after` times -- promoting on the FIRST
     * sighting would pay for server state that a one-shot query never uses.
     * The extended path only; a simple query has no Parse to skip. */
    if (have_params) {
        stmt = pg_stmt_get(g, sql, strlen(sql));
        if (stmt && stmt->uses >= g->prepare_after) {
            stmt_name = stmt->name;
            stmt_prepared = stmt->prepared;
            /* Per-column binary needs the result types, which only a previous
             * execution's RowDescription can supply -- so this arm exists only
             * because the statement cache does. */
            if (g->binary_results && stmt->oids && stmt->noids > 0) {
                rfmt = (uint8_t *)malloc((size_t)stmt->noids);
                if (rfmt) {
                    int k;
                    nrfmt = stmt->noids;
                    for (k = 0; k < nrfmt; k++)
                        rfmt[k] = (uint8_t)pg_oid_prefers_binary(stmt->oids[k]);
                }
            }
            /* Optimistic: the Parse is pipelined with the Bind, so a failure
             * arrives as one ErrorResponse and the error path evicts. */
            stmt->prepared = 1;
            if (stmt_prepared) g->n_prepared_hits++;
        } else {
            g->n_unnamed++;
        }
    }
    { int rc = pg_encode_query(&w, sql, (int)nparam, pv, plen, pnull,
                               have_params, stmt_name, stmt_prepared,
                               rfmt, nrfmt);
      free(rfmt);
      pg_free_params(ctx, pv, pfree, nparam);
      free(pv); free(plen); free(pnull); free(pfree); JS_FreeCString(ctx, sql);
      if (rc < 0) { free(w.b); return JS_ThrowOutOfMemory(ctx); } }

    p = pgp_new();
    if (!p) { free(w.b); return JS_ThrowOutOfMemory(ctx); }
    if (stmt_name)
        snprintf(p->stmt_name, sizeof(p->stmt_name), "%s", stmt_name);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { free(w.b); pgp_free(ctx, p); return promise; }
    p->resolve = funcs[0];
    p->reject = funcs[1];

    if (g->state == PG_ST_READY) {
        if (pg_write(g, w.b, w.len) < 0) {
            free(w.b); pgp_free(ctx, p); JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        free(w.b);
        if (g->query_timeout_ms)
            p->deadline_ms = dyn_timer_now_ms() + g->query_timeout_ms;
        pgp_push(&g->head, &g->tail, p);
        g->npending++;
        pg_flush_soon(g);
    } else {
        /* Held until ReadyForQuery: a query written during authentication
         * would be read by the server as part of the handshake. */
        p->bytes = w.b;
        p->nbytes = w.len;
        pgp_push(&g->wq_head, &g->wq_tail, p);
        g->nwait++;
    }
    return promise;
}

/* A cancel goes on a FRESH connection -- the busy one is not reading it -- and
 * outlives the request that started it, so it owns its own reactor reference
 * and its own buffer. */
typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    int fd, done, hooked;
    uint64_t deadline_ms;
    uint8_t msg[12 + PG_MAX_CANCEL_KEY];
    size_t len;
} pg_cancel_t;

static void pg_cancel_tick(void *udata);

static void pg_cancel_finish(pg_cancel_t *c)
{
    if (c->done)
        return;
    c->done = 1;
    if (c->hooked)
        dyn_net_off_drain(c);
    if (c->fd >= 0)
        dyn_aio_close(c->aio, c->fd);
    dyn_net_reactor_release(c->ctx);
    free(c);
}

/* The server processes the request and closes without answering -- "for
 * security reasons, no direct reply is made" -- so EOF is the completion. */
static void pg_cancel_eof(dyn_aio_t *aio, int res, const uint8_t *buf,
                          unsigned len, void *ud)
{
    (void)aio; (void)res; (void)buf; (void)len;
    pg_cancel_finish((pg_cancel_t *)ud);
}

/* Bounds a LEAK, not a hang, and the distinction was measured: a reactor
 * reference on its own does NOT keep the loop alive -- the JS handle does -- so
 * a cancel still in flight never stops a program exiting. In a long-lived
 * process a connect that is neither accepted nor refused would otherwise hold
 * this struct, its fd and a reactor reference for as long as the process runs.
 * UNREACHED on the development host: the addresses available here answer or
 * refuse at once, so the timer path is exercised only by injection. */
static void pg_cancel_tick(void *udata)
{
    pg_cancel_t *c = (pg_cancel_t *)udata;
    if (!c->done && c->deadline_ms && dyn_timer_now_ms() >= c->deadline_ms)
        pg_cancel_finish(c);
}

static void pg_cancel_connected(dyn_aio_t *aio, int res, const uint8_t *buf,
                                unsigned len, void *ud)
{
    pg_cancel_t *c = (pg_cancel_t *)ud;
    (void)aio; (void)buf; (void)len;

    if (res < 0) { pg_cancel_finish(c); return; }
    /* Arm the read BEFORE sending: closing straight after the send would drop
     * whatever the adapter had to buffer, and would leak the fd otherwise. */
    if (dyn_aio_recv(c->aio, c->fd, 0, 0, pg_cancel_eof, c) < 0) {
        pg_cancel_finish(c);
        return;
    }
    if (dyn_aio_send(c->aio, c->fd, c->msg, c->len, 0, NULL, NULL) < 0)
        pg_cancel_finish(c);
}

static JSValue dyn_pg_cancel(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    pg_cancel_t *c;
    (void)argc; (void)argv;

    if (!g)
        return JS_EXCEPTION;
    if (!g->cancel_key_len)
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: no cancel key yet; the server sends it during startup");
    c = (pg_cancel_t *)calloc(1, sizeof(*c));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->ctx = ctx;
    c->fd = -1;
    c->len = 12 + g->cancel_key_len;
    put32(c->msg, (uint32_t)c->len);
    put32(c->msg + 4, PG_CANCEL_CODE);
    put32(c->msg + 8, g->backend_pid);
    memcpy(c->msg + 12, g->cancel_key, g->cancel_key_len);

    /* Its own reference: the connection being cancelled may be closed while
     * this is still in flight, and the reactor must outlive it. */
    c->aio = dyn_net_reactor_acquire(ctx);
    if (!c->aio) { free(c); return JS_ThrowInternalError(ctx, "PostgreSQL: cancel: no reactor"); }
    c->deadline_ms = dyn_timer_now_ms() +
        (g->query_timeout_ms ? g->query_timeout_ms : PG_CONNECT_TIMEOUT);
    if (dyn_net_on_drain(pg_cancel_tick, c) == 0)
        c->hooked = 1;
    c->fd = g->path
          ? dyn_aio_unix_connect(c->aio, g->path, pg_cancel_connected, c)
          : dyn_aio_connect(c->aio, g->host ? g->host : "127.0.0.1", g->port,
                            pg_cancel_connected, c);
    if (c->fd < 0) {
        int e = errno;
        pg_cancel_finish(c);
        return JS_ThrowInternalError(ctx, "PostgreSQL: cancel: %s", strerror(e));
    }
    return JS_UNDEFINED;
}

static JSValue dyn_pg_on(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    dyn_pg_t *g;
    const char *ev;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, handler)");
    ev = JS_ToCString(ctx, argv[0]);
    if (!ev)
        return JS_EXCEPTION;
    g = pg_this(ctx, this_val);
    if (!g) { JS_FreeCString(ctx, ev); return JS_EXCEPTION; }
    if (strcmp(ev, "notice") == 0) {
        JS_FreeValue(ctx, g->h_notice); g->h_notice = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "notification") == 0) {
        JS_FreeValue(ctx, g->h_notify); g->h_notify = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "error") == 0) {
        JS_FreeValue(ctx, g->h_error); g->h_error = JS_DupValue(ctx, argv[1]);
    } else {
        JS_FreeCString(ctx, ev);
        return JS_ThrowRangeError(ctx,
            "on: want 'notice', 'notification' or 'error'");
    }
    JS_FreeCString(ctx, ev);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_pg_get_ready(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    return g ? JS_NewBool(ctx, g->state == PG_ST_READY) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_pending(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->npending + g->nwait) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_pid(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    return g ? JS_NewUint32(ctx, g->backend_pid) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_tx(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    char t[2];
    if (!g) return JS_EXCEPTION;
    t[0] = g->tx_status ? g->tx_status : 'I'; t[1] = '\0';
    return JS_NewString(ctx, t);
}
static JSValue dyn_pg_get_params(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    return g ? JS_DupValue(ctx, g->params) : JS_EXCEPTION;
}

/* ---- construction and teardown ------------------------------------------ */

static void dyn_pg_dispose(void *native)
{
    dyn_pg_t *g = (dyn_pg_t *)native;
    dyn_pg_pending_t *p;

    if (!g)
        return;
    pg_flush_drop(g);          /* never reachable through the flush list again */
    pg_stmt_clear(g);
    if (g->hooked)
        dyn_net_off_drain(g);
    if (g->state == PG_ST_READY && g->fd >= 0) {
        /* Terminate. Without it every disconnect is an "unexpected EOF" in the
         * server's log, indistinguishable from a client that crashed. */
        static const uint8_t bye[5] = { 'X', 0, 0, 0, 4 };
        (void)dyn_aio_send(g->aio, g->fd, bye, sizeof(bye), 0, NULL, NULL);
    }
    if (g->state != PG_ST_DEAD) {
        g->state = PG_ST_DEAD;
        while ((p = pgp_pop(&g->head, &g->tail)) != NULL)
            pg_settle(g->ctx, p, 1, pg_conn_error(g->ctx, "PostgreSQL: client closed"));
        while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL)
            pg_settle(g->ctx, p, 1, pg_conn_error(g->ctx, "PostgreSQL: client closed"));
    }
    if (g->aio) {
        if (g->fd >= 0)
            dyn_aio_close(g->aio, g->fd);
        dyn_net_reactor_release(g->ctx);
    }
    dyn_scram_free(&g->scram);
    JS_FreeValue(g->ctx, g->params);
    JS_FreeValue(g->ctx, g->h_notice);
    JS_FreeValue(g->ctx, g->h_notify);
    JS_FreeValue(g->ctx, g->h_error);
    /* The password is its OWN allocation, so zeroing the struct a line later
     * clears the pointer and leaves the bytes in the heap. */
    if (g->pass) { memset(g->pass, 0, strlen(g->pass)); free(g->pass); }
    free(g->host); free(g->path); free(g->user);
    free(g->database); free(g->appname);
    free(g->rbuf); free(g->obuf);
    memset(g, 0, sizeof(*g));
    free(g);
}

static char *pg_opt_str(JSContext *ctx, JSValueConst o, const char *k)
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
static int pg_opt_int(JSContext *ctx, JSValueConst o, const char *k, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return dflt; }
    if (JS_ToInt32(ctx, &n, v) < 0) { JS_FreeValue(ctx, v); return dflt; }
    JS_FreeValue(ctx, v);
    return (int)n;
}
static int pg_opt_bool(JSContext *ctx, JSValueConst o, const char *k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static JSValue dyn_pg_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    dyn_pg_t *g;
    JSValueConst opt = argc > 0 ? argv[0] : JS_UNDEFINED;
    int ct, qt, mm, mp;
    (void)new_target;

    if (argc > 0 && !JS_IsObject(opt))
        return JS_ThrowTypeError(ctx, "PostgreSQL: expects an options object");
    if (JS_IsObject(opt) && pg_opt_bool(ctx, opt, "tls"))
        return JS_ThrowTypeError(ctx,
            "PostgreSQL: TLS is not supported; SCRAM-SHA-256 protects the "
            "credential but not the session, so terminate TLS in front of the "
            "server or use a trusted network");

    g = (dyn_pg_t *)calloc(1, sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->ctx = ctx;
    g->fd = -1;
    g->state = PG_ST_CONNECTING;
    g->maxmsg = PG_DEFAULT_MAXMSG;
    g->maxpending = 1024;
    /* Defaults: cache up to 64 statements, promote on the SECOND sighting.
     * 0 disables the cache entirely, which PgBouncer in transaction mode
     * requires -- it cannot carry named prepared statements across a pooled
     * connection. */
    g->stmt_cache_max = 64;
    g->prepare_after = 2;
    /* ON by default: the decode is checked against the declared width on every
     * column, and the value a caller sees is identical either way (the tests
     * assert that directly). `textResults: true` forces the old path. */
    g->binary_results = 1;
    g->tx_status = 'I';
    g->params = JS_NewObject(ctx);
    g->h_notice = g->h_notify = g->h_error = JS_UNDEFINED;
    g->port = 5432;

    if (JS_IsObject(opt)) {
        g->host = pg_opt_str(ctx, opt, "host");
        g->path = pg_opt_str(ctx, opt, "path");
        g->user = pg_opt_str(ctx, opt, "user");
        g->pass = pg_opt_str(ctx, opt, "password");
        g->database = pg_opt_str(ctx, opt, "database");
        g->appname = pg_opt_str(ctx, opt, "applicationName");
        g->port = (uint16_t)pg_opt_int(ctx, opt, "port", 5432);
        g->raw = pg_opt_bool(ctx, opt, "raw");
        g->bytes_out = pg_opt_bool(ctx, opt, "bytes");
        g->binary_results = !pg_opt_bool(ctx, opt, "textResults");
        { int sc = pg_opt_int(ctx, opt, "statementCacheSize", -1);
          if (sc >= 0) g->stmt_cache_max = sc;
          sc = pg_opt_int(ctx, opt, "prepareAfter", 0);
          if (sc > 0) g->prepare_after = sc; }
        g->bigint = pg_opt_bool(ctx, opt, "bigint");
        g->insecure_auth = pg_opt_bool(ctx, opt, "insecureAuth");
        mm = pg_opt_int(ctx, opt, "maxMessageBytes", 0);
        if (mm > 0) g->maxmsg = (size_t)mm;
        mp = pg_opt_int(ctx, opt, "maxPending", 0);
        if (mp > 0) g->maxpending = mp;
        qt = pg_opt_int(ctx, opt, "queryTimeoutMs", 0);
        if (qt > 0) g->query_timeout_ms = (uint64_t)qt;
        ct = pg_opt_int(ctx, opt, "connectTimeoutMs", PG_CONNECT_TIMEOUT);
        if (ct > 0) g->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)ct;
        if (g->port == 0 && !g->path) {
            dyn_pg_dispose(g);
            return JS_ThrowRangeError(ctx, "PostgreSQL: port must be 1..65535");
        }
    } else {
        g->connect_deadline_ms = dyn_timer_now_ms() + PG_CONNECT_TIMEOUT;
    }

    g->aio = dyn_net_reactor_acquire(ctx);
    if (!g->aio) {
        dyn_pg_dispose(g);
        return JS_ThrowInternalError(ctx, "PostgreSQL: cannot acquire the reactor");
    }
    g->fd = g->path
          ? dyn_aio_unix_connect(g->aio, g->path, pg_on_connect, g)
          : dyn_aio_connect(g->aio, g->host ? g->host : "127.0.0.1",
                            g->port, pg_on_connect, g);
    if (g->fd < 0) {
        JSValue e = JS_ThrowInternalError(ctx, "PostgreSQL: connect: %s",
                                          strerror(errno));
        dyn_pg_dispose(g);
        return e;
    }
    if (dyn_net_on_drain(pg_tick, g) == 0)
        g->hooked = 1;
    return dyn_res_wrap(ctx, dyn_pg_class_id, g, dyn_pg_dispose);
}

/* The three handlers and the parameter object; see the note in dyna-net-redis.c
 * for why the pending entries are not traversed. */
static void dyn_pg_gc_mark(JSRuntime *rt, JSValueConst val,
                           JS_MarkFunc *mark_func)
{
    DynResource *res = (DynResource *)JS_GetOpaque(val, dyn_pg_class_id);
    dyn_pg_t *g;

    if (!res || res->closed || !res->native)
        return;
    g = (dyn_pg_t *)res->native;
    JS_MarkValue(rt, g->params, mark_func);
    JS_MarkValue(rt, g->h_notice, mark_func);
    JS_MarkValue(rt, g->h_notify, mark_func);
    JS_MarkValue(rt, g->h_error, mark_func);
}

static const JSClassDef dyn_pg_class = {
    "PostgreSQL",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_pg_gc_mark,
};


/* No "close" here: the framework installs it on every resource proto. */
/* {size, max, preparedHits, unnamed, prepareAfter} -- which arm each query
 * took. A portfolio whose selection cannot be observed is one that can
 * silently stop choosing the fast arm with nothing to say so. */
static JSValue dyn_pg_get_stmt_stats(JSContext *ctx, JSValueConst this_val)
{
    dyn_pg_t *g = pg_this(ctx, this_val);
    JSValue o;
    if (!g) return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, g->nstmts));
    JS_SetPropertyStr(ctx, o, "max", JS_NewInt32(ctx, g->stmt_cache_max));
    JS_SetPropertyStr(ctx, o, "prepareAfter", JS_NewInt32(ctx, g->prepare_after));
    JS_SetPropertyStr(ctx, o, "preparedHits",
                      JS_NewInt64(ctx, (int64_t)g->n_prepared_hits));
    JS_SetPropertyStr(ctx, o, "unnamed",
                      JS_NewInt64(ctx, (int64_t)g->n_unnamed));
    return o;
}

static const JSCFunctionListEntry dyn_pg_proto[] = {
    JS_CFUNC_DEF("query", 1, dyn_pg_query),
    JS_CFUNC_DEF("cancel", 0, dyn_pg_cancel),
    JS_CFUNC_DEF("on", 2, dyn_pg_on),
    JS_CGETSET_DEF("ready", dyn_pg_get_ready, NULL),
    JS_CGETSET_DEF("statementCache", dyn_pg_get_stmt_stats, NULL),
    JS_CGETSET_DEF("pending", dyn_pg_get_pending, NULL),
    JS_CGETSET_DEF("backendPid", dyn_pg_get_pid, NULL),
    JS_CGETSET_DEF("transactionStatus", dyn_pg_get_tx, NULL),
    JS_CGETSET_DEF("parameters", dyn_pg_get_params, NULL),
};

int dyn_pg_register(JSContext *ctx, JSModuleDef *m)
{
    return dyn_register_class(ctx, m, &dyn_pg_class_id, &dyn_pg_class,
                              dyn_pg_proto, countof(dyn_pg_proto),
                              dyn_pg_ctor, "PostgreSQL");
}

void dyn_pg_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "PostgreSQL");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
