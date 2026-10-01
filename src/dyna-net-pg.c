#include "dyna-nat.h"
#include "cutils.h"
#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif
#include "dtoa.h"
#include <math.h>
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

static const char* const pg_ctor_keys[] = {
    "tls",
    "ca",
    "host",
    "path",
    "user",
    "password",
    "database",
    "applicationName",
    "port",
    "raw",
    "bytes",
    "textResults",
    "statementCacheSize",
    "prepareAfter",
    "bigint",
    "insecureAuth",
    "maxMessageBytes",
    "maxPending",
    "queryTimeoutMs",
    "connectTimeoutMs",
};
static const char* const pg_query_keys[] = { "maxRows" };

#define PG_PROTO_30 196608u
#define PG_CANCEL_CODE 80877102u
#define PG_MAX_STARTUP 10000
#define PG_MAX_CANCEL_KEY 256
#define PG_DEFAULT_MAXMSG (64u * 1024u * 1024u)
#define PG_CONNECT_TIMEOUT 10000

#define PG_AUTH_OK 0
#define PG_AUTH_CLEARTEXT 3
#define PG_AUTH_MD5 5
#define PG_AUTH_SASL 10
#define PG_AUTH_SASL_CONT 11
#define PG_AUTH_SASL_FINAL 12

#define PG_OID_BOOL 16
#define PG_OID_BYTEA 17
#define PG_OID_INT8 20
#define PG_OID_INT2 21
#define PG_OID_INT4 23
#define PG_OID_OID 26
#define PG_OID_FLOAT4 700
#define PG_OID_FLOAT8 701
#define PG_OID_UUID 2950

static int pg_oid_prefers_binary(uint32_t oid)
{
    switch (oid) {
    case PG_OID_BOOL:
    case PG_OID_BYTEA:
    case PG_OID_INT8:
    case PG_OID_INT2:
    case PG_OID_INT4:
    case PG_OID_OID:
    case PG_OID_FLOAT4:
    case PG_OID_FLOAT8:
    case PG_OID_UUID:
        return 1;
    default:
        return 0;
    }
}

#define PG_ST_CONNECTING 0
#define PG_ST_AUTH 1
#define PG_ST_READY 2
#define PG_ST_DEAD 3

#define PG_ROW_TEMPLATE_MAX_FIELDS 32

typedef struct dyn_pg_batch {
    JSContext* ctx;
    JSValue* slots;
    int k, ndone;
    int dead;
    int settled;
    JSValue resolve, reject;
} dyn_pg_batch_t;

typedef struct dyn_pg_pending {
    struct dyn_pg_pending* next;
    JSValue resolve, reject;
    JSValue rows, fields;
    JSValue error;
    uint8_t* bytes;
    size_t nbytes;
    char tag[64];
    int rowcount;
    int64_t max_rows;
    uint64_t deadline_ms;
    uint64_t esync;
    int sync_end;
    dyn_pg_batch_t* batch;
    int bidx;
    JSAtom* fatom;
    uint32_t* foid;
    uint8_t* fformat;
    int nfield;
    JSObjectTemplate* tpl;
    char stmt_name[24];
} dyn_pg_pending_t;

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd, state, hooked;
    int released;
    int insecure_auth;
    int raw;
    int bytes_out;
    int binary_results;
    int bigint;
    char *host, *path, *user, *pass, *database, *appname;
    uint16_t port;
    size_t maxmsg;
    uint64_t connect_deadline_ms, query_timeout_ms;

    uint8_t* rbuf;
    size_t rcap, rlen, rpos;
    uint8_t* obuf;
    size_t ocap, olen;

    dyn_pg_pending_t *head, *tail, *wq_head, *wq_tail;
    dyn_pg_pending_t *dhead, *dtail;
    uint64_t sync_seq, sync_recv;
    int sync_closed;
    int npending, nwait, maxpending;
    int flush_queued;

    struct pg_stmt {
        char* sql;
        size_t sqllen;
        uint32_t* oids;
        uint32_t hash;
        int uses;
        int prepared;
        int noids;
        char name[24];
    }* stmts;
    int nstmts, cap_stmts;
    int stmt_cache_max;
    int prepare_after;
    uint32_t stmt_seq;
    uint64_t n_prepared_hits;
    uint64_t n_unnamed;

    dyn_scram_t scram;
    int scram_live, scram_done;
    uint32_t backend_pid;
    uint8_t cancel_key[PG_MAX_CANCEL_KEY];
    size_t cancel_key_len;
    char tx_status;
    JSValue params;
    JSValue h_notice, h_notify, h_error;
    int in_cb;
    int closing;
#ifdef CONFIG_TLS
    int use_tls;
    char* tls_ca;
    dyn_tls_ctx_t* tls_ctx;
#endif
} dyn_pg_t;

static void dyn_pg_teardown(dyn_pg_t* g);
static int pg_gone(dyn_pg_t* g);

static _Thread_local int pg_in_final;

static void dyn_pg_finalizer(JSRuntime* rt, JSValue val)
{
    pg_in_final++;
    dyn_res_finalizer(rt, val);
    pg_in_final--;
}

static JSClassID dyn_pg_class_id;

static void pg_stmt_evict_named(dyn_pg_t* g, const char* name);

#define PG_MAX_FLUSH 256
static _Thread_local dyn_pg_t* pg_flush_pending[PG_MAX_FLUSH];
static _Thread_local int pg_n_flush;

static void pg_fail_all(dyn_pg_t* g, const char* msg);

static int pgbuf_reserve(uint8_t** p, size_t* cap, size_t need)
{
    size_t c = *cap;
    uint8_t* n;
    if (need <= c)
        return 0;
    if (c == 0)
        c = 1024;
    while (c < need)
        c = c < (1u << 20) ? c * 2 : c + (c / 4);
    n = (uint8_t*)realloc(*p, c);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t get16(const uint8_t* p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

typedef struct {
    uint8_t* b;
    size_t len, cap;
    int bad;
} pgw_t;

static void pgw_raw(pgw_t* w, const void* p, size_t n)
{
    if (w->bad || pgbuf_reserve(&w->b, &w->cap, w->len + n) < 0) {
        w->bad = 1;
        return;
    }
    if (n)
        memcpy(w->b + w->len, p, n);
    w->len += n;
}
static void pgw_u8(pgw_t* w, uint8_t v) { pgw_raw(w, &v, 1); }
static void pgw_u32(pgw_t* w, uint32_t v)
{
    uint8_t t[4];
    put32(t, v);
    pgw_raw(w, t, 4);
}
static void pgw_u16(pgw_t* w, uint16_t v)
{
    uint8_t t[2];
    t[0] = (uint8_t)(v >> 8);
    t[1] = (uint8_t)v;
    pgw_raw(w, t, 2);
}
static void pgw_str(pgw_t* w, const char* s) { pgw_raw(w, s, strlen(s) + 1); }

static size_t pgw_begin(pgw_t* w, char type)
{
    pgw_u8(w, (uint8_t)type);
    pgw_u32(w, 0);
    return w->len - 4;
}
static void pgw_end(pgw_t* w, size_t at)
{
    if (!w->bad)
        put32(w->b + at, (uint32_t)(w->len - at));
}

static dyn_pg_pending_t* pgp_new(void)
{
    dyn_pg_pending_t* p = (dyn_pg_pending_t*)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->resolve = p->reject = p->rows = p->fields = p->error = JS_UNDEFINED;
    return p;
}

static void pgp_free_fields(JSContext* ctx, dyn_pg_pending_t* p)
{
    int i;
    for (i = 0; i < p->nfield; i++)
        JS_FreeAtom(ctx, p->fatom[i]);
    JS_FreeObjectTemplate(ctx, p->tpl);
    p->tpl = NULL;
    free(p->fatom);
    p->fatom = NULL;
    p->foid = NULL;
    p->fformat = NULL;
    p->nfield = 0;
}

static void pgp_free(JSContext* ctx, dyn_pg_pending_t* p)
{
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->rows);
    JS_FreeValue(ctx, p->fields);
    JS_FreeValue(ctx, p->error);
    pgp_free_fields(ctx, p);
    free(p->bytes);
    free(p);
}

static void pgp_free_rt(JSRuntime* rt, dyn_pg_pending_t* p)
{
    int i;
    JS_FreeValueRT(rt, p->resolve);
    JS_FreeValueRT(rt, p->reject);
    JS_FreeValueRT(rt, p->rows);
    JS_FreeValueRT(rt, p->fields);
    JS_FreeValueRT(rt, p->error);
    for (i = 0; i < p->nfield; i++)
        JS_FreeAtomRT(rt, p->fatom[i]);
    JS_FreeObjectTemplateRT(rt, p->tpl);
    free(p->fatom);
    free(p->bytes);
    free(p);
}

static void pgp_push(dyn_pg_pending_t** h, dyn_pg_pending_t** t, dyn_pg_pending_t* p)
{
    p->next = NULL;
    if (*t)
        (*t)->next = p;
    else
        *h = p;
    *t = p;
}
static dyn_pg_pending_t* pgp_pop(dyn_pg_pending_t** h, dyn_pg_pending_t** t)
{
    dyn_pg_pending_t* p = *h;
    if (!p)
        return NULL;
    *h = p->next;
    if (!*h)
        *t = NULL;
    p->next = NULL;
    return p;
}

static void pg_settle(dyn_pg_t* g, dyn_pg_pending_t* p, int reject, JSValue v)
{
    JSContext* ctx = g->ctx;
    JSValue fn = reject ? p->reject : p->resolve;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { v };
        g->in_cb = 1;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        g->in_cb = 0;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, v);
    pgp_free(ctx, p);
}

static JSValue pg_conn_error(JSContext* ctx, const char* msg)
{
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "CONNECTION"));
    return e;
}

static void pg_batch_assemble(dyn_pg_t* g, dyn_pg_batch_t* b)
{
    JSContext* ctx = b->ctx;
    JSValue arr = JS_NewArray(ctx);
    int i;

    if (JS_IsException(arr)) {
        for (i = 0; i < b->k; i++)
            JS_FreeValue(ctx, b->slots[i]);
        JS_FreeValue(ctx, JS_GetException(ctx));
        arr = pg_conn_error(ctx, "PostgreSQL: out of memory assembling the pipeline result");
        if (JS_IsFunction(ctx, b->reject)) {
            JSValueConst a[1] = { arr };
            g->in_cb = 1;
            JSValue r = JS_Call(ctx, b->reject, JS_UNDEFINED, 1, a);
            g->in_cb = 0;
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, arr);
    } else {
        for (i = 0; i < b->k; i++)
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, b->slots[i],
                JS_PROP_C_W_E);
        if (JS_IsFunction(ctx, b->resolve)) {
            JSValueConst a[1] = { arr };
            g->in_cb = 1;
            JSValue r = JS_Call(ctx, b->resolve, JS_UNDEFINED, 1, a);
            g->in_cb = 0;
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, arr);
    }
}

static void pg_batch_fill(dyn_pg_t* g, dyn_pg_pending_t* p, JSValue v)
{
    dyn_pg_batch_t* b = p->batch;

    p->batch = NULL;
    if (!b || b->dead || b->settled) {
        JS_FreeValue(g->ctx, v);
        return;
    }
    b->slots[p->bidx] = v;
    if (++b->ndone == b->k) {
        b->settled = 1;
        pg_batch_assemble(g, b);
        JS_FreeValue(b->ctx, b->resolve);
        JS_FreeValue(b->ctx, b->reject);
        free(b->slots);
        free(b);
    }
}

static void pg_batch_fail(dyn_pg_t* g, dyn_pg_batch_t* b, const char* msg)
{
    JSContext* ctx = b->ctx;
    dyn_pg_pending_t* p;
    JSValue e;
    int i;

    if (b->dead || b->settled)
        return;
    b->dead = 1;
    for (p = g->head; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    for (p = g->dhead; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    for (p = g->wq_head; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    if (pg_in_final) {
        for (i = 0; i < b->k; i++)
            JS_FreeValueRT(g->rt, b->slots[i]);
        free(b->slots);
        JS_FreeValueRT(g->rt, b->resolve);
        JS_FreeValueRT(g->rt, b->reject);
        free(b);
        return;
    }
    for (i = 0; i < b->k; i++)
        JS_FreeValue(ctx, b->slots[i]);
    free(b->slots);
    e = pg_conn_error(ctx, msg);
    if (JS_IsFunction(ctx, b->reject)) {
        JSValueConst a[1] = { e };
        g->in_cb = 1;
        JSValue r = JS_Call(ctx, b->reject, JS_UNDEFINED, 1, a);
        g->in_cb = 0;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, b->resolve);
    JS_FreeValue(ctx, b->reject);
    free(b);
}

static int32_t pg_tag_rowcount(const char* tag)
{
    const char* sp = strrchr(tag, ' ');
    const char* d;
    long v = 0;

    if (!sp || !sp[1])
        return -1;
    for (d = sp + 1; *d; d++) {
        if (*d < '0' || *d > '9')
            return -1;
        v = v * 10 + (*d - '0');
    }
    return (int32_t)v;
}

static void pg_settle_entry(dyn_pg_t* g, dyn_pg_pending_t* p)
{
    JSContext* ctx = g->ctx;
    int32_t affected;
    {
        int32_t t = p->rowcount > 0 ? p->rowcount : pg_tag_rowcount(p->tag);
        affected = t < 0 ? 0 : t;
    }

    if (p->batch) {
        JSValue res = JS_NewObject(ctx);
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            res = pg_conn_error(ctx,
                "PostgreSQL: out of memory building a pipeline result");
        } else {
            JS_SetPropertyStr(ctx, res, "rows",
                JS_IsUndefined(p->rows) ? JS_NewArray(ctx)
                                        : p->rows);
            p->rows = JS_UNDEFINED;
            JS_SetPropertyStr(ctx, res, "fields",
                JS_IsUndefined(p->fields) ? JS_NewArray(ctx)
                                          : p->fields);
            p->fields = JS_UNDEFINED;
            JS_SetPropertyStr(ctx, res, "command", JS_NewString(ctx, p->tag));
            JS_SetPropertyStr(ctx, res, "rowCount",
                JS_NewInt32(ctx, affected));
        }
        pgp_free_fields(ctx, p);
        free(p->bytes);
        p->bytes = NULL;
        pg_batch_fill(g, p, res);
        pgp_free(ctx, p);
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
        JS_SetPropertyStr(ctx, res, "rowCount", JS_NewInt32(ctx, affected));
        pg_settle(g, p, 0, res);
    }
}

static void pg_settle_entry_err(dyn_pg_t* g, dyn_pg_pending_t* p, JSValue e)
{
    JSContext* ctx = g->ctx;

    if (p->batch) {
        pgp_free_fields(ctx, p);
        free(p->bytes);
        p->bytes = NULL;
        pg_batch_fill(g, p, e);
        pgp_free(ctx, p);
        return;
    }
    pg_settle(g, p, 1, e);
}

static int pg_flush(dyn_pg_t* g)
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

static void pg_flush_drop(dyn_pg_t* g)
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

static JSValue pg_flush_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    while (pg_n_flush > 0) {
        dyn_pg_t* g = pg_flush_pending[0];
        pg_flush_drop(g);
        if (g->state == PG_ST_READY && pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
    }
    return JS_UNDEFINED;
}

static void pg_flush_soon(dyn_pg_t* g)
{
    if (g->flush_queued || g->olen == 0 || g->state != PG_ST_READY)
        return;
    if (pg_n_flush >= PG_MAX_FLUSH || JS_EnqueueJob(g->ctx, pg_flush_job, 0, NULL) < 0) {
        if (pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }
    g->flush_queued = 1;
    pg_flush_pending[pg_n_flush++] = g;
}

static int pg_write(dyn_pg_t* g, const uint8_t* b, size_t n)
{
    if (pgbuf_reserve(&g->obuf, &g->ocap, g->olen + n) < 0)
        return -1;
    memcpy(g->obuf + g->olen, b, n);
    g->olen += n;
    return 0;
}

static int pg_send_now(dyn_pg_t* g, const uint8_t* b, size_t n)
{
    if (pg_write(g, b, n) < 0)
        return -1;
    return pg_flush(g);
}

static JSValue pg_error_value(JSContext* ctx, const uint8_t* p, size_t len)
{
    JSValue e = JS_NewError(ctx);
    size_t i = 0;
    const uint8_t* sev_s = NULL;
    size_t sev_s_len = 0;
    int have_v = 0;
    while (i < len && p[i] != 0) {
        char code = (char)p[i];
        size_t s = ++i, n;
        while (i < len && p[i] != 0)
            i++;
        n = i - s;
        if (i < len)
            i++;
        switch (code) {
        case 'M':
            JS_SetPropertyStr(ctx, e, "message",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'C':
            JS_SetPropertyStr(ctx, e, "code",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'V':
            have_v = 1;
            JS_SetPropertyStr(ctx, e, "severity",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'S':
            sev_s = p + s;
            sev_s_len = n;
            break;
        case 'D':
            JS_SetPropertyStr(ctx, e, "detail",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'H':
            JS_SetPropertyStr(ctx, e, "hint",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'P':
            JS_SetPropertyStr(ctx, e, "position",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 's':
            JS_SetPropertyStr(ctx, e, "schema",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 't':
            JS_SetPropertyStr(ctx, e, "table",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'c':
            JS_SetPropertyStr(ctx, e, "column",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        case 'n':
            JS_SetPropertyStr(ctx, e, "constraint",
                JS_NewStringLen(ctx, (const char*)p + s, n));
            break;
        default:
            break;
        }
    }
    if (!have_v && sev_s)
        JS_SetPropertyStr(ctx, e, "severity",
            JS_NewStringLen(ctx, (const char*)sev_s, sev_s_len));
    return e;
}

static void pg_md5_auth(const char* pass, const char* user,
    const uint8_t salt[4], char out[36])
{
    uint8_t d1[16], d2[16];
    char h1[33];
    size_t n1 = strlen(pass), n2 = strlen(user);
    uint8_t* tmp = (uint8_t*)malloc(n1 + n2 + 16);
    if (!tmp) {
        out[0] = '\0';
        return;
    }
    memcpy(tmp, pass, n1);
    memcpy(tmp + n1, user, n2);
    dyn_md5(tmp, n1 + n2, d1);
    dyn_codec_hex_encode(d1, 16, h1);
    h1[32] = '\0';
    memcpy(tmp, h1, 32);
    memcpy(tmp + 32, salt, 4);
    dyn_md5(tmp, 36, d2);
    memset(tmp, 0, n1 + n2 + 16);
    free(tmp);
    memcpy(out, "md5", 3);
    dyn_codec_hex_encode(d2, 16, out + 3);
    out[35] = '\0';
}

static void pg_handle_auth(dyn_pg_t* g, const uint8_t* body, size_t len)
{
    uint32_t code;
    pgw_t w;

    if (len < 4) {
        pg_fail_all(g, "PostgreSQL: truncated authentication request");
        return;
    }
    code = get32(body);
    memset(&w, 0, sizeof(w));

    switch (code) {
    case PG_AUTH_OK:
        if (g->scram_live && !g->scram_done) {
            pg_fail_all(g, "PostgreSQL: the server skipped the SCRAM final "
                           "message, so it never proved it knows the password");
            return;
        }
        return;

    case PG_AUTH_CLEARTEXT:
        if (!g->insecure_auth || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for a cleartext password; "
                           "this client has no TLS, so that is refused unless "
                           "insecureAuth is set");
            return;
        }
        {
            size_t at = pgw_begin(&w, 'p');
            pgw_str(&w, g->pass);
            pgw_end(&w, at);
        }
        break;

    case PG_AUTH_MD5:
        if (!g->insecure_auth || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for MD5 authentication; "
                           "this client requires SCRAM-SHA-256 unless "
                           "insecureAuth is set");
            return;
        }
        if (len < 8) {
            pg_fail_all(g, "PostgreSQL: truncated MD5 salt");
            return;
        }
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
        char cfirst[256];
        size_t at, i = 4;
        int found = 0;
        while (i < len && body[i]) {
            size_t s = i;
            while (i < len && body[i])
                i++;
            if (i - s == 13 && memcmp(body + s, "SCRAM-SHA-256", 13) == 0)
                found = 1;
            if (i < len)
                i++;
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
        n = dyn_scram_server_first(&g->scram, (const char*)body + 4, len - 4,
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
        int rc;
        if (!g->scram_live) {
            pg_fail_all(g, "PostgreSQL: SASL final with no exchange in progress");
            return;
        }
        rc = dyn_scram_server_final(&g->scram, (const char*)body + 4, len - 4);
        if (rc != DYN_SCRAM_OK) {
            pg_fail_all(g, rc == DYN_SCRAM_E_VERIFY ? "PostgreSQL: the server failed to prove it knows the password; "
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

static int pg_parse_double(const uint8_t* p, size_t n, double* out)
{
    char stackbuf[64], *s = stackbuf, *heap = NULL;
    const char* end = NULL;
    JSATODTempMem tmp;
    double v;

    if (n == 0)
        return 0;
    if (n == 3 && memcmp(p, "NaN", 3) == 0) {
        *out = DYN_NAN;
        return 1;
    }
    if (n == 8 && memcmp(p, "Infinity", 8) == 0) {
        *out = DYN_INFINITY;
        return 1;
    }
    if (n == 9 && memcmp(p, "-Infinity", 9) == 0) {
        *out = -DYN_INFINITY;
        return 1;
    }

    if (n + 1 > sizeof(stackbuf)) {
        heap = (char*)malloc(n + 1);
        if (!heap)
            return 0;
        s = heap;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    v = js_atod(s, &end, 10, 0, &tmp);
    if (!end || end != s + n || v != v) {
        free(heap);
        return 0;
    }
    free(heap);
    *out = v;
    return 1;
}

static JSValue pg_bytes_u8(JSContext* ctx, const uint8_t* p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), ta;
    JSValueConst a3[3];
    if (JS_IsException(ab))
        return ab;
    a3[0] = ab;
    a3[1] = JS_NewInt32(ctx, 0);
    a3[2] = JS_NewInt32(ctx, (int)n);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue pg_bytea_to_hex_string(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    char stackbuf[128], *s = stackbuf, *heap = NULL;
    JSValue out;
    size_t i, need = n * 2 + 2;
    if (need > sizeof(stackbuf)) {
        heap = (char*)malloc(need);
        if (!heap)
            return JS_ThrowOutOfMemory(ctx);
        s = heap;
    }
    s[0] = '\\';
    s[1] = 'x';
    for (i = 0; i < n; i++) {
        s[2 + i * 2] = hex[p[i] >> 4];
        s[3 + i * 2] = hex[p[i] & 15];
    }
    out = JS_NewStringLen(ctx, s, need);
    free(heap);
    return out;
}

static JSValue pg_bytea(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const int8_t hexval[256] = {
        ['0'] = 0,
        ['1'] = 1,
        ['2'] = 2,
        ['3'] = 3,
        ['4'] = 4,
        ['5'] = 5,
        ['6'] = 6,
        ['7'] = 7,
        ['8'] = 8,
        ['9'] = 9,
        ['a'] = 10,
        ['b'] = 11,
        ['c'] = 12,
        ['d'] = 13,
        ['e'] = 14,
        ['f'] = 15,
        ['A'] = 10,
        ['B'] = 11,
        ['C'] = 12,
        ['D'] = 13,
        ['E'] = 14,
        ['F'] = 15,
    };
    size_t i, nb;
    JSValue ab, ta;
    JSValueConst a3[3];
    uint8_t* out;

    if (n < 2 || p[0] != '\\' || p[1] != 'x' || (n & 1))
        return JS_UNDEFINED;
    for (i = 2; i < n; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F')))
            return JS_UNDEFINED;
    nb = (n - 2) / 2;
    out = (uint8_t*)js_malloc(ctx, nb ? nb : 1);
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < nb; i++)
        out[i] = (uint8_t)((hexval[p[2 + i * 2]] << 4) | hexval[p[3 + i * 2]]);
    ab = JS_NewArrayBufferCopy(ctx, out, nb);
    js_free(ctx, out);
    if (JS_IsException(ab))
        return ab;
    a3[0] = ab;
    a3[1] = JS_NewInt32(ctx, 0);
    a3[2] = JS_NewInt32(ctx, (int)nb);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

#define PG_PF_NONE 0
#define PG_PF_CSTR 1
#define PG_PF_MALLOC 2

static void pg_free_params(JSContext* ctx, const char** pv, const uint8_t* pf,
    uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        if (!pv[k])
            continue;
        if (pf[k] == PG_PF_CSTR)
            JS_FreeCString(ctx, pv[k]);
        else if (pf[k] == PG_PF_MALLOC)
            free(DYN_UNCONST(pv[k]));
    }
}

static int pg_param_bytea(JSContext* ctx, JSValueConst v, const char** out,
    size_t* outlen)
{
    static const char hex[] = "0123456789abcdef";
    size_t off = 0, len = 0, bpe = 0, total = 0, i;
    uint8_t* base;
    char* s;
    JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &total, v);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return 0;
        }
        off = 0;
        len = total;
    } else {
        uint8_t* b = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!b) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return 0;
        }
        base = b;
    }
    if (len > (SIZE_MAX - 3) / 2) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    s = (char*)malloc(len * 2 + 3);
    if (!s) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    s[0] = '\\';
    s[1] = 'x';
    for (i = 0; i < len; i++) {
        s[2 + i * 2] = hex[base[off + i] >> 4];
        s[3 + i * 2] = hex[base[off + i] & 15];
    }
    s[2 + len * 2] = '\0';
    *out = s;
    *outlen = len * 2 + 2;
    return 1;
}

static JSValue pg_column_binary(JSContext* ctx, dyn_pg_t* g, uint32_t oid,
    const uint8_t* p, size_t n)
{
    switch (oid) {
    case PG_OID_BOOL:
        if (n == 1)
            return JS_NewBool(ctx, p[0] != 0);
        break;
    case PG_OID_BYTEA:
        if (g->bytes_out)
            return pg_bytes_u8(ctx, p, n);
        return pg_bytea_to_hex_string(ctx, p, n);
    case PG_OID_INT2:
        if (n == 2)
            return JS_NewInt32(ctx, (int16_t)((p[0] << 8) | p[1]));
        break;
    case PG_OID_INT4:
        if (n == 4)
            return JS_NewInt32(ctx, (int32_t)get32(p));
        break;
    case PG_OID_OID:
        if (n == 4)
            return JS_NewUint32(ctx, get32(p));
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
        if (n == 4) {
            uint32_t u = get32(p);
            float f;
            memcpy(&f, &u, 4);
            return JS_NewFloat64(ctx, (double)f);
        }
        break;
    case PG_OID_FLOAT8:
        if (n == 8) {
            uint64_t u = ((uint64_t)get32(p) << 32) | get32(p + 4);
            double d;
            memcpy(&d, &u, 8);
            return JS_NewFloat64(ctx, d);
        }
        break;
    case PG_OID_UUID:
        if (n == 16) {
            static const char hex[] = "0123456789abcdef";
            char s[37];
            int i, k = 0;
            for (i = 0; i < 16; i++) {
                if (i == 4 || i == 6 || i == 8 || i == 10)
                    s[k++] = '-';
                s[k++] = hex[p[i] >> 4];
                s[k++] = hex[p[i] & 15];
            }
            return JS_NewStringLen(ctx, s, 36);
        }
        break;
    default:
        break;
    }
    return pg_bytes_u8(ctx, p, n);
}

static JSValue pg_column(JSContext* ctx, dyn_pg_t* g, uint32_t oid,
    const uint8_t* p, size_t n)
{
    char tmp[48];
    if (g->raw)
        return JS_NewStringLen(ctx, (const char*)p, n);
    switch (oid) {
    case PG_OID_BYTEA:
        if (g->bytes_out) {
            JSValue b = pg_bytea(ctx, p, n);
            if (!JS_IsUndefined(b))
                return b;
        }
        return JS_NewStringLen(ctx, (const char*)p, n);
    case PG_OID_BOOL:
        return JS_NewBool(ctx, n == 1 && p[0] == 't');
    case PG_OID_INT2:
    case PG_OID_INT4:
    case PG_OID_FLOAT4:
    case PG_OID_FLOAT8: {
        double d;
        if (n == 0 || !pg_parse_double(p, n, &d))
            return JS_NewStringLen(ctx, (const char*)p, n);
        return JS_NewFloat64(ctx, d);
    }
    case PG_OID_INT8: {
        long long v;
        char* end;
        if (n == 0 || n >= sizeof(tmp))
            return JS_NewStringLen(ctx, (const char*)p, n);
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        v = strtoll(tmp, &end, 10);
        if (*end)
            return JS_NewStringLen(ctx, (const char*)p, n);
        if (g->bigint)
            return JS_NewBigInt64(ctx, (int64_t)v);
        if (v > 9007199254740992LL || v < -9007199254740992LL)
            return JS_NewStringLen(ctx, (const char*)p, n);
        return JS_NewInt64(ctx, v);
    }
    default:
        return JS_NewStringLen(ctx, (const char*)p, n);
    }
}

static void pg_stmt_learn_oids(dyn_pg_t* g, const char* name,
    const uint32_t* oids, int n)
{
    int i;
    if (!name || !name[0] || n <= 0)
        return;
    for (i = 0; i < g->nstmts; i++) {
        struct pg_stmt* s = &g->stmts[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->oids)
            return;
        s->oids = (uint32_t*)malloc((size_t)n * sizeof(*s->oids));
        if (!s->oids)
            return;
        memcpy(s->oids, oids, (size_t)n * sizeof(*s->oids));
        s->noids = n;
        return;
    }
}

static void pg_row_description(dyn_pg_t* g, dyn_pg_pending_t* p,
    const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
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
        p->fatom = (JSAtom*)calloc(1, (size_t)nf * (sizeof(*p->fatom) + sizeof(*p->foid)) + nf);
        if (!p->fatom) {
            pgp_free_fields(ctx, p);
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
        }
        p->foid = (uint32_t*)(p->fatom + nf);
        p->fformat = (uint8_t*)(p->foid + nf);
    }
    for (i = 0; i < nf; i++) {
        size_t s = at;
        JSValue f;
        while (at < len && b[at] != 0)
            at++;
        if (at >= len) {
            pg_fail_all(g, "PostgreSQL: malformed RowDescription");
            return;
        }
        f = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, f, "name",
            JS_NewStringLen(ctx, (const char*)b + s, at - s));
        p->fatom[i] = JS_NewAtomLen(ctx, (const char*)b + s, at - s);
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
        p->fformat[i] = get16(b + at + 16) ? 1 : 0;
        JS_DefinePropertyValueUint32(ctx, p->fields, i, f, JS_PROP_C_W_E);
        at += 18;
    }
    if (nf && nf <= PG_ROW_TEMPLATE_MAX_FIELDS)
        p->tpl = JS_NewObjectTemplate(ctx, p->fatom, (uint32_t)nf);
    pg_stmt_learn_oids(g, p->stmt_name, p->foid, p->nfield);
}

static void pg_data_row(dyn_pg_t* g, dyn_pg_pending_t* p,
    const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    uint16_t nc, i;
    size_t at = 2;
    JSValue row;

    if (len < 2) {
        pg_fail_all(g, "PostgreSQL: truncated DataRow");
        return;
    }
    nc = get16(b);
    if (p->tpl && nc == (uint16_t)p->nfield) {
        row = JS_NewObjectFromTemplate(ctx, p->tpl);
        if (JS_IsException(row)) {
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
        }
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
            if (clen == 0xffffffffu) {
                v = JS_NULL;
            } else {
                if (clen > len - at) {
                    JS_FreeValue(ctx, row);
                    pg_fail_all(g, "PostgreSQL: DataRow column runs past the message");
                    return;
                }
                if (p->fformat[i])
                    v = pg_column_binary(ctx, g, p->foid[i], b + at, clen);
                else
                    v = pg_column(ctx, g, p->foid[i], b + at, clen);
                at += clen;
            }
            JS_SetTemplateCell(ctx, row, i, v);
        }
        goto row_done;
    }
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
        if (clen == 0xffffffffu) {
            v = JS_NULL;
        } else {
            if (clen > len - at) {
                JS_FreeValue(ctx, row);
                pg_fail_all(g, "PostgreSQL: DataRow column runs past the message");
                return;
            }
            if (i < p->nfield && p->fformat[i])
                v = pg_column_binary(ctx, g, p->foid[i], b + at, clen);
            else
                v = pg_column(ctx, g, i < p->nfield ? p->foid[i] : 0, b + at, clen);
            at += clen;
        }
        if (i < p->nfield && p->fatom[i] != JS_ATOM_NULL) {
            JS_DefinePropertyValue(ctx, row, p->fatom[i], v, JS_PROP_C_W_E);
        } else {
            JS_FreeValue(ctx, v);
        }
    }
row_done:
    if (p->max_rows > 0 && p->rowcount >= p->max_rows) {
        JS_FreeValue(ctx, row);
        pg_fail_all(g, "PostgreSQL: result exceeds maxRows");
        return;
    }
    if (JS_IsUndefined(p->rows))
        p->rows = JS_NewArray(ctx);
    JS_DefinePropertyValueUint32(ctx, p->rows, (uint32_t)p->rowcount, row,
        JS_PROP_C_W_E);
    p->rowcount++;
}

static void pg_sync_failed(dyn_pg_t* g, dyn_pg_pending_t* p)
{
    JSContext* ctx = g->ctx;
    dyn_pg_batch_t* b = p->batch;
    JSValue e = p->error;
    int settled_last = !b || p->sync_end;

    if (p->stmt_name[0]) {
        pg_stmt_evict_named(g, p->stmt_name);
        p->stmt_name[0] = '\0';
    }
    p->error = JS_UNDEFINED;
    pgp_pop(&g->head, &g->tail);
    g->npending--;
    pg_settle_entry_err(g, p, e);
    if (!settled_last) {
        while ((p = g->head) != NULL && p->batch == b) {
            int qlast = p->sync_end;
            JSValue ae = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, ae, "message",
                JS_NewString(ctx,
                    "PostgreSQL: an earlier statement of the pipeline "
                    "failed; the server skipped this one"));
            pgp_pop(&g->head, &g->tail);
            g->npending--;
            pg_settle_entry_err(g, p, ae);
            if (qlast)
                break;
        }
    }
}

static void pg_ready_for_query(dyn_pg_t* g, char status)
{
    dyn_pg_pending_t* p;

    g->tx_status = status;
    if (g->state != PG_ST_READY) {
        g->state = PG_ST_READY;
        g->connect_deadline_ms = 0;
        while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
            g->nwait--;
            if (p->bytes && pg_write(g, p->bytes, p->nbytes) < 0) {
                pgp_push(&g->wq_head, &g->wq_tail, p);
                g->nwait++;
                pg_fail_all(g, "PostgreSQL: out of memory");
                return;
            }
            free(p->bytes);
            p->bytes = NULL;
            p->nbytes = 0;
            if (g->query_timeout_ms)
                p->deadline_ms = dyn_timer_now_ms() + g->query_timeout_ms;
            pgp_push(&g->head, &g->tail, p);
            g->npending++;
        }
        if (pg_flush(g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return;
    }

    {
        uint64_t r = ++g->sync_recv;

        g->sync_closed = 0;
        while (g->dhead && g->dhead->esync <= r) {
            p = pgp_pop(&g->dhead, &g->dtail);
            g->npending--;
            pg_settle_entry(g, p);
        }
        p = g->head;
        if (p && !JS_IsUndefined(p->error))
            pg_sync_failed(g, p);
    }
}

static void pg_call1(dyn_pg_t* g, JSValue fn, JSValue arg)
{
    JSContext* ctx = g->ctx;
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { arg };
        g->in_cb = 1;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        g->in_cb = 0;
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arg);
}

static void pg_message(dyn_pg_t* g, char type, const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    dyn_pg_pending_t* p = g->head;

    switch (type) {
    case 'R':
        pg_handle_auth(g, b, len);
        return;
    case 'v':
        return;
    case 'S':
        if (len > 1) {
            size_t i = 0;
            while (i < len && b[i])
                i++;
            if (i + 1 < len) {
                size_t j = i + 1, s = j;
                while (j < len && b[j])
                    j++;
                JS_DefinePropertyValueStr(ctx, g->params,
                    (const char*)b,
                    JS_NewStringLen(ctx, (const char*)b + s, j - s),
                    JS_PROP_C_W_E);
            }
        }
        return;
    case 'K':
        if (len >= 4) {
            size_t klen = len - 4;
            g->backend_pid = get32(b);
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
        if ((p && !JS_IsUndefined(p->fields)) || g->sync_closed) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per query; "
                           "the server returned a second result set");
            return;
        }
        if (p)
            pg_row_description(g, p, b, len);
        return;
    case 'D':
        if (g->sync_closed) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per "
                           "query; the server sent data after the statement "
                           "completed");
            return;
        }
        if (p)
            pg_data_row(g, p, b, len);
        return;
    case 'C':
        if (g->sync_closed) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per "
                           "query; the server completed a second command");
            return;
        }
        if (p) {
            size_t n = len;
            while (n && b[n - 1] == 0)
                n--;
            if (n >= sizeof(p->tag))
                n = sizeof(p->tag) - 1;
            memcpy(p->tag, b, n);
            p->tag[n] = '\0';
            pgp_pop(&g->head, &g->tail);
            pgp_push(&g->dhead, &g->dtail, p);
            if (!p->batch || p->sync_end)
                g->sync_closed = 1;
        }
        return;
    case 'I':
        if (g->sync_closed) {
            pg_fail_all(g, "PostgreSQL: this client runs ONE statement per "
                           "query; the server completed a second command");
            return;
        }
        if (p) {
            p->tag[0] = '\0';
            pgp_pop(&g->head, &g->tail);
            pgp_push(&g->dhead, &g->dtail, p);
            if (!p->batch || p->sync_end)
                g->sync_closed = 1;
        }
        return;
    case 'E': {
        JSValue e = pg_error_value(ctx, b, len);
        if (p && JS_IsUndefined(p->error)) {
            p->error = e;
        } else if (p) {
            JS_FreeValue(ctx, e);
        } else {
            if (g->state != PG_ST_READY) {
                JSValue msg = JS_GetPropertyStr(ctx, e, "message");
                const char* m = JS_ToCString(ctx, msg);
                char buf[256];
                snprintf(buf, sizeof(buf), "PostgreSQL: %s", m ? m : "connection rejected");
                if (m)
                    JS_FreeCString(ctx, m);
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
    case 'N':
        pg_call1(g, g->h_notice, pg_error_value(ctx, b, len));
        return;
    case 'A': {
        JSValue o = JS_NewObject(ctx);
        if (len >= 4) {
            size_t i = 4, s;
            JS_SetPropertyStr(ctx, o, "pid", JS_NewUint32(ctx, get32(b)));
            s = i;
            while (i < len && b[i])
                i++;
            JS_SetPropertyStr(ctx, o, "channel",
                JS_NewStringLen(ctx, (const char*)b + s, i - s));
            if (i < len)
                i++;
            s = i;
            while (i < len && b[i])
                i++;
            JS_SetPropertyStr(ctx, o, "payload",
                JS_NewStringLen(ctx, (const char*)b + s, i - s));
        }
        pg_call1(g, g->h_notify, o);
        return;
    }
    case '1':
    case '2':
    case '3':
    case 'n':
    case 's':
    case 't':
        return;
    case 'G':
    case 'H':
    case 'W':
    case 'd':
    case 'c':
        pg_fail_all(g, "PostgreSQL: COPY is not supported by this client");
        return;
    default:
        pg_fail_all(g, "PostgreSQL: unknown message type from the server");
        return;
    }
}

static void pg_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;
    (void)aio;

    if (pg_gone(g))
        return;
    if (res < 0) {
        pg_fail_all(g, "PostgreSQL: connection error");
        pg_gone(g);
        return;
    }
    if (res == 0 && len == 0) {
        pg_fail_all(g, "PostgreSQL: server closed the connection");
        pg_gone(g);
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
        const uint8_t* m = g->rbuf + g->rpos;
        char type;
        if (avail < 5)
            break;
        type = (char)m[0];
        mlen = get32(m + 1);
        if (mlen < 4 || mlen > g->maxmsg) {
            pg_fail_all(g, "PostgreSQL: message length out of range");
            return;
        }
        if (avail < (size_t)mlen + 1)
            break;
        pg_message(g, type, m + 5, (size_t)mlen - 4);
        if (pg_gone(g))
            return;
        g->rpos += (size_t)mlen + 1;
    }
    if (g->rpos) {
        memmove(g->rbuf, g->rbuf + g->rpos, g->rlen - g->rpos);
        g->rlen -= g->rpos;
        g->rpos = 0;
    }
}

static int pg_gone(dyn_pg_t* g)
{
    if (g->closing) {
        dyn_pg_teardown(g);
        return 1;
    }
    return g->state == PG_ST_DEAD;
}

static void pg_fail_all(dyn_pg_t* g, const char* msg)
{
    JSContext* ctx = g->ctx;
    dyn_pg_pending_t* p;

    if (g->state == PG_ST_DEAD)
        return;
    g->state = PG_ST_DEAD;
    if (g->fd >= 0) {
        dyn_aio_close(g->aio, g->fd);
        g->fd = -1;
    }
    while ((p = pgp_pop(&g->head, &g->tail)) != NULL) {
        g->npending--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    while ((p = pgp_pop(&g->dhead, &g->dtail)) != NULL) {
        g->npending--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
        g->nwait--;
        if (p->batch)
            pg_batch_fail(g, p->batch, msg);
        pg_settle(g, p, 1, pg_conn_error(ctx, msg));
    }
    if (JS_IsFunction(ctx, g->h_error))
        pg_call1(g, g->h_error, pg_conn_error(ctx, msg));
    if (g->hooked) {
        dyn_net_off_drain(g);
        g->hooked = 0;
    }
    if (g->aio && !g->released) {
        g->released = 1;
        dyn_net_reactor_release(g->ctx);
    }
}

static int pg_send_startup(dyn_pg_t* g)
{
    pgw_t w;
    int rc;
    memset(&w, 0, sizeof(w));
    pgw_u32(&w, 0);
    pgw_u32(&w, PG_PROTO_30);
    pgw_str(&w, "user");
    pgw_str(&w, g->user ? g->user : "postgres");
    if (g->database) {
        pgw_str(&w, "database");
        pgw_str(&w, g->database);
    }
    pgw_str(&w, "client_encoding");
    pgw_str(&w, "UTF8");
    if (g->appname) {
        pgw_str(&w, "application_name");
        pgw_str(&w, g->appname);
    }
    pgw_u8(&w, 0);
    if (w.bad) {
        free(w.b);
        return -1;
    }
    if (w.len > PG_MAX_STARTUP) {
        free(w.b);
        return -1;
    }
    put32(w.b, (uint32_t)w.len);
    rc = pg_write(g, w.b, w.len);
    free(w.b);
    return rc;
}

#ifdef CONFIG_TLS
static void pg_tls_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    if (g->state == PG_ST_DEAD)
        return;
    if (res < 0) {
        pg_fail_all(g, "PostgreSQL: TLS handshake failed");
        pg_gone(g);
        return;
    }
    if (pg_send_startup(g) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the startup message");
}
#endif

static void pg_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (g->state == PG_ST_DEAD)
        return;
    if (res < 0) {
        pg_fail_all(g, "PostgreSQL: connect failed");
        pg_gone(g);
        return;
    }
    g->state = PG_ST_AUTH;
#ifdef CONFIG_TLS
    if (g->use_tls) {
        dyn_tls_opts_t to;
        char terr[192];
        memset(&to, 0, sizeof to);
        to.ca_file = g->tls_ca;
        if (!g->tls_ctx)
            g->tls_ctx = dyn_tls_ctx_client(&to, terr, sizeof terr);
        if (!g->tls_ctx) {
            pg_fail_all(g, terr);
            pg_gone(g);
            return;
        }
        {
            dyn_tls_conn_t* t = dyn_tls_conn_new(g->tls_ctx, g->host,
                terr, sizeof terr);
            if (!t) {
                pg_fail_all(g, terr);
                pg_gone(g);
                return;
            }
            if (dyn_aio_tls_attach(g->aio, g->fd, t, pg_tls_done, g) < 0
                || dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0
                || dyn_aio_tls_start(g->aio, g->fd) < 0) {
                pg_fail_all(g, "PostgreSQL: TLS handshake failed to start");
                pg_gone(g);
                return;
            }
        }
        return;
    }
#endif
    if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0) {
        pg_fail_all(g, "PostgreSQL: cannot read from the socket");
        return;
    }
    if (pg_send_startup(g) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the startup message");
}

static void pg_tick(void* udata)
{
    dyn_pg_t* g = (dyn_pg_t*)udata;
    uint64_t now;
    if (pg_gone(g))
        return;
    now = dyn_timer_now_ms();
    if (g->connect_deadline_ms && now >= g->connect_deadline_ms) {
        pg_fail_all(g, "PostgreSQL: connect timed out");
        return;
    }
    if (g->head && g->head->deadline_ms && now >= g->head->deadline_ms)
        pg_fail_all(g, "PostgreSQL: query timed out");
}

typedef struct pg_cps pg_cps_t;
typedef void (*pg_cps_step_fn)(JSContext* ctx, pg_cps_t* cp,
    JSValueConst val, int failed);
struct pg_cps {
    pg_cps_step_fn step;
    int settled;
};

static JSValue pg_swallow_rejection(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue pg_cps_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValueConst* func_data)
{
    int64_t p = 0;
    pg_cps_t* cp;

    (void)this_val;
    (void)argc;
    if (JS_ToInt64(ctx, &p, func_data[0]))
        return JS_UNDEFINED;
    cp = (pg_cps_t*)(uintptr_t)p;
    if (cp->settled)
        return JS_UNDEFINED;
    cp->settled = 1;
    cp->step(ctx, cp, argc > 0 ? argv[0] : JS_UNDEFINED, magic);
    return JS_UNDEFINED;
}

static JSValue pg_cps_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    int64_t p = 0;
    pg_cps_t* cp;

    if (JS_ToInt64(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    cp = (pg_cps_t*)(uintptr_t)p;
    cp->step(ctx, cp, argc > 1 ? argv[1] : JS_UNDEFINED, 0);
    return JS_UNDEFINED;
}

static void pg_cps_await(JSContext* ctx, pg_cps_t* cp, pg_cps_step_fn step,
    JSValue promise)
{
    JSValue dptr, onok, onerr, pthen, tr;
    JSValueConst thenargs[2];

    cp->settled = 0;
    if (JS_IsException(promise)) {
        JSValue exc = JS_GetException(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    {
        JSPromiseStateEnum st = JS_PromiseState(ctx, promise);
        if (st == JS_PROMISE_REJECTED) {
            JSValue pthen2 = JS_GetPropertyStr(ctx, promise, "then");
            if (JS_IsFunction(ctx, pthen2)) {
                JSValue sw = JS_NewCFunction(ctx, pg_swallow_rejection, "", 1);
                JSValueConst na[2] = { JS_UNDEFINED, sw };
                JSValue tr2 = JS_Call(ctx, pthen2, promise, 2, na);
                if (JS_IsException(tr2))
                    JS_FreeValue(ctx, JS_GetException(ctx));
                else
                    JS_FreeValue(ctx, tr2);
                JS_FreeValue(ctx, sw);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, pthen2);
            {
                JSValue reason = JS_PromiseResult(ctx, promise);
                JS_FreeValue(ctx, promise);
                step(ctx, cp, reason, 1);
                JS_FreeValue(ctx, reason);
            }
            return;
        }
        if (st == JS_PROMISE_FULFILLED) {
            JSValue d, result = JS_PromiseResult(ctx, promise);
            JSValueConst jargs[2];
            int rc;
            JS_FreeValue(ctx, promise);
            cp->step = step;
            d = JS_NewInt64(ctx, (int64_t)(uintptr_t)cp);
            jargs[0] = d;
            jargs[1] = result;
            rc = JS_EnqueueJob(ctx, pg_cps_job, 2, jargs);
            JS_FreeValue(ctx, d);
            JS_FreeValue(ctx, result);
            if (rc < 0) {
                JSValue exc = JS_GetException(ctx);
                step(ctx, cp, exc, 1);
                JS_FreeValue(ctx, exc);
            }
            return;
        }
    }
    dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)cp);
    onok = JS_NewCFunctionData(ctx, pg_cps_settle, 1, 0, 1, &dptr);
    onerr = JS_NewCFunctionData(ctx, pg_cps_settle, 1, 1, 1, &dptr);
    JS_FreeValue(ctx, dptr);
    if (JS_IsException(onok) || JS_IsException(onerr)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    cp->step = step;
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    if (!JS_IsFunction(ctx, pthen)) {
        JSValue exc;
        JS_FreeValue(ctx, pthen);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        exc = JS_ThrowTypeError(ctx,
            "queryIter: query() returned a non-promise");
        step(ctx, cp, exc, 1);
        return;
    }
    thenargs[0] = onok;
    thenargs[1] = onerr;
    tr = JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onok);
    JS_FreeValue(ctx, onerr);
    JS_FreeValue(ctx, promise);
    if (JS_IsException(tr)) {
        JSValue exc = JS_GetException(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    JS_FreeValue(ctx, tr);
}

typedef struct pgq_iter pgq_iter_t;

typedef struct pgq_op pgq_op_t;
struct pgq_op {
    pg_cps_t cps;
    pgq_op_t* qnext;
    JSValue it_obj;
    JSValue jresolve, jreject;
    int mode;
    int want_return;
};

#define PGQ_ST_IDLE 0
#define PGQ_ST_BEGIN 1
#define PGQ_ST_DECLARE 2
#define PGQ_ST_FETCH 3
#define PGQ_ST_CLEANUP_CLOSE 4
#define PGQ_ST_CLEANUP_TX 5
#define PGQ_ST_DONE 6

struct pgq_iter {
    JSContext* ctx;
    JSValue conn_obj;
    JSValue query_fn;
    char* sql;
    JSValue params;
    char name[24];
    JSValue rows;
    uint32_t row_index, row_count;
    int64_t batch;
    int64_t max_rows;
    int64_t emitted;
    int state;
    int declared;
    int began;
    int running;
    int in_pump;
    JSValue error;
    pgq_op_t *qhead, *qtail;
};

static JSClassID pgq_class_id;

static void pgq_iter_finalizer(JSRuntime* rt, JSValue val)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(val, pgq_class_id);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->conn_obj);
    JS_FreeValueRT(rt, it->query_fn);
    JS_FreeValueRT(rt, it->params);
    JS_FreeValueRT(rt, it->rows);
    JS_FreeValueRT(rt, it->error);
    free(it->sql);
    free(it);
}

static void pgq_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(val, pgq_class_id);
    if (!it)
        return;
    JS_MarkValue(rt, it->conn_obj, mark);
    JS_MarkValue(rt, it->query_fn, mark);
    JS_MarkValue(rt, it->params, mark);
    JS_MarkValue(rt, it->rows, mark);
    JS_MarkValue(rt, it->error, mark);
}

static const JSClassDef pgq_class = {
    "PgRowStream",
    .finalizer = pgq_iter_finalizer,
    .gc_mark = pgq_iter_mark,
};

static JSValue pgq_query(JSContext* ctx, pgq_iter_t* it, const char* sql,
    JSValueConst params)
{
    JSValue s, ret;
    JSValueConst argv[2];
    int argc = 1;

    s = JS_NewString(ctx, sql);
    if (JS_IsException(s))
        return s;
    argv[0] = s;
    if (!JS_IsUndefined(params)) {
        argv[1] = params;
        argc = 2;
    }
    ret = JS_Call(ctx, it->query_fn, it->conn_obj, argc, argv);
    JS_FreeValue(ctx, s);
    return ret;
}

static pgq_op_t* pgq_shift(JSContext* ctx, pgq_iter_t* it)
{
    pgq_op_t* op = it->qhead;
    (void)ctx;
    if (!op)
        return NULL;
    it->qhead = op->qnext;
    if (!it->qhead)
        it->qtail = NULL;
    op->qnext = NULL;
    return op;
}

static void pgq_park(pgq_iter_t* it, pgq_op_t* op)
{
    op->qnext = NULL;
    if (it->qtail)
        it->qtail->qnext = op;
    else
        it->qhead = op;
    it->qtail = op;
}

static void pgq_pump(JSContext* ctx, pgq_iter_t* it);

static void pgq_op_unwind(JSContext* ctx, pgq_iter_t* it, pgq_op_t* op)
{
    it->running = 0;
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    pgq_pump(ctx, it);
    JS_FreeValue(ctx, op->it_obj);
    free(op);
}

static void pgq_op_bare_free(JSContext* ctx, pgq_op_t* op)
{
    JS_FreeValue(ctx, op->it_obj);
    JS_FreeValue(ctx, op->jresolve);
    JS_FreeValue(ctx, op->jreject);
    free(op);
}

static JSValue pgq_buffered_row(pgq_iter_t* it)
{
    JSContext* ctx = it->ctx;
    JSValue row;

    if (JS_IsUndefined(it->rows) || it->row_index >= it->row_count)
        return JS_UNDEFINED;
    row = JS_GetPropertyUint32(ctx, it->rows, it->row_index);
    it->row_index++;
    if (it->row_index >= it->row_count) {
        JS_FreeValue(ctx, it->rows);
        it->rows = JS_UNDEFINED;
        it->row_index = it->row_count = 0;
    }
    return row;
}

static void pgq_op_settle(JSContext* ctx, pgq_op_t* op, JSValue value,
    int failed)
{
    JSValue fn = failed ? op->jreject : op->jresolve, r;

    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { value };
        r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, value);
}

static void pgq_op_emit(JSContext* ctx, pgq_op_t* op, JSValue value)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        result = JS_GetException(ctx);
        pgq_op_settle(ctx, op, result, 1);
        return;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE, JS_PROP_C_W_E);
    pgq_op_settle(ctx, op, result, 0);
}

static void pgq_op_done(JSContext* ctx, pgq_op_t* op)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        pgq_op_settle(ctx, op, JS_UNDEFINED, 1);
        return;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
    pgq_op_settle(ctx, op, result, 0);
}

static void pgq_fail(JSContext* ctx, pgq_iter_t* it, JSValueConst val)
{
    if (JS_IsUndefined(it->error))
        it->error = JS_DupValue(ctx, val);
    it->state = PGQ_ST_CLEANUP_CLOSE;
}

static void pgq_step(JSContext* ctx, pg_cps_t* cp, JSValueConst val,
    int failed);

static void pgq_advance(JSContext* ctx, pgq_iter_t* it, pgq_op_t* op)
{
    char* sql;
    size_t n;
    JSValue p;

    if (op->want_return && (it->state == PGQ_ST_FETCH || it->state == PGQ_ST_IDLE)) {
        JS_FreeValue(ctx, it->rows);
        it->rows = JS_UNDEFINED;
        it->row_index = it->row_count = 0;
        it->state = (it->declared || it->began) ? PGQ_ST_CLEANUP_CLOSE
                                                : PGQ_ST_DONE;
    }

    switch (it->state) {
    case PGQ_ST_DONE:
        if (!JS_IsUndefined(it->error) && !op->want_return) {
            JSValueConst a[1] = { it->error };
            JSValue r = JS_Call(ctx, op->jreject, JS_UNDEFINED, 1, a);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, r);
        } else {
            pgq_op_done(ctx, op);
        }
        pgq_op_unwind(ctx, it, op);
        return;

    case PGQ_ST_IDLE:
        op->mode = PGQ_ST_BEGIN;
        p = pgq_query(ctx, it, "BEGIN", JS_UNDEFINED);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_DECLARE:
        op->mode = PGQ_ST_DECLARE;
        n = strlen(it->name) + strlen(it->sql) + 32;
        sql = (char*)malloc(n);
        if (!sql) {
            JSValue exc = JS_ThrowOutOfMemory(ctx);
            pgq_fail(ctx, it, exc);
            JS_FreeValue(ctx, exc);
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "DECLARE %s CURSOR FOR %s", it->name, it->sql);
        p = pgq_query(ctx, it, sql, it->params);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_FETCH:
        if (!JS_IsUndefined(it->rows) && it->row_index < it->row_count) {
            JSValue row = pgq_buffered_row(it);
            it->emitted++;
            if (it->max_rows > 0 && it->emitted >= it->max_rows) {
                JS_FreeValue(ctx, it->rows);
                it->rows = JS_UNDEFINED;
                it->row_index = it->row_count = 0;
                it->state = PGQ_ST_CLEANUP_CLOSE;
            }
            pgq_op_emit(ctx, op, row);
            pgq_op_unwind(ctx, it, op);
            return;
        }
        op->mode = PGQ_ST_FETCH;
        n = (size_t)snprintf(NULL, 0, "FETCH FORWARD %lld FROM %s",
                (long long)it->batch, it->name)
            + 1;
        sql = (char*)malloc(n);
        if (!sql) {
            JSValue exc = JS_ThrowOutOfMemory(ctx);
            pgq_fail(ctx, it, exc);
            JS_FreeValue(ctx, exc);
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "FETCH FORWARD %lld FROM %s",
            (long long)it->batch, it->name);
        p = pgq_query(ctx, it, sql, JS_UNDEFINED);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_CLEANUP_CLOSE:
        if (!it->declared) {
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        }
        op->mode = PGQ_ST_CLEANUP_CLOSE;
        n = strlen(it->name) + 16;
        sql = (char*)malloc(n);
        if (!sql) {
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        }
        snprintf(sql, n, "CLOSE %s", it->name);
        p = pgq_query(ctx, it, sql, JS_UNDEFINED);
        free(sql);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    case PGQ_ST_CLEANUP_TX:
        op->mode = PGQ_ST_CLEANUP_TX;
        p = pgq_query(ctx, it, JS_IsUndefined(it->error) ? "COMMIT" : "ROLLBACK",
            JS_UNDEFINED);
        pg_cps_await(ctx, &op->cps, pgq_step, p);
        return;

    default:
        pgq_op_done(ctx, op);
        pgq_op_unwind(ctx, it, op);
        return;
    }
}

static void pgq_step(JSContext* ctx, pg_cps_t* cp, JSValueConst val,
    int failed)
{
    pgq_op_t* op = (pgq_op_t*)cp;
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(op->it_obj, pgq_class_id);

    if (!it) {
        pgq_op_bare_free(ctx, op);
        return;
    }
    if (failed) {
        switch (op->mode) {
        case PGQ_ST_CLEANUP_CLOSE:
            it->state = PGQ_ST_CLEANUP_TX;
            pgq_advance(ctx, it, op);
            return;
        case PGQ_ST_CLEANUP_TX:
            it->state = PGQ_ST_DONE;
            pgq_advance(ctx, it, op);
            return;
        default:
            pgq_fail(ctx, it, val);
            pgq_advance(ctx, it, op);
            return;
        }
    }

    switch (op->mode) {
    case PGQ_ST_BEGIN:
        it->began = 1;
        it->state = PGQ_ST_DECLARE;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_DECLARE:
        it->declared = 1;
        it->state = PGQ_ST_FETCH;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_FETCH: {
        JSValue rows, row;
        uint32_t n = 0;
        JSValue lv;

        rows = JS_IsObject(val)
            ? JS_GetPropertyStr(ctx, val, "rows")
            : JS_UNDEFINED;
        if (JS_IsException(rows)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            rows = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(rows)) {
            lv = JS_GetPropertyStr(ctx, rows, "length");
            if (JS_ToUint32(ctx, &n, lv)) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, rows);
                rows = JS_UNDEFINED;
                n = 0;
            } else {
                JS_FreeValue(ctx, lv);
            }
        }
        if (JS_IsUndefined(rows) || n == 0) {
            JS_FreeValue(ctx, rows);
            it->state = PGQ_ST_CLEANUP_CLOSE;
            pgq_advance(ctx, it, op);
            return;
        }
        JS_FreeValue(ctx, it->rows);
        it->rows = rows;
        it->row_index = 0;
        it->row_count = n;
        if (op->want_return) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
            pgq_advance(ctx, it, op);
            return;
        }
        row = pgq_buffered_row(it);
        it->emitted++;
        if (it->max_rows > 0 && it->emitted >= it->max_rows) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
        }
        pgq_op_emit(ctx, op, row);
        pgq_op_unwind(ctx, it, op);
        return;
    }

    case PGQ_ST_CLEANUP_CLOSE:
        it->state = PGQ_ST_CLEANUP_TX;
        pgq_advance(ctx, it, op);
        return;

    case PGQ_ST_CLEANUP_TX:
        it->state = PGQ_ST_DONE;
        pgq_advance(ctx, it, op);
        return;

    default:
        pgq_op_unwind(ctx, it, op);
        return;
    }
}

static void pgq_pump(JSContext* ctx, pgq_iter_t* it)
{
    if (it->in_pump)
        return;
    it->in_pump = 1;
    while (!it->running) {
        pgq_op_t* op = pgq_shift(ctx, it);
        if (!op)
            break;
        it->running = 1;
        pgq_advance(ctx, it, op);
    }
    it->in_pump = 0;
}

static JSValue pgq_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(this_val, pgq_class_id);
    JSValue funcs[2], promise, row;
    pgq_op_t* op;

    (void)argc;
    (void)argv;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    if (!it) {
        JSValue e = JS_ThrowTypeError(ctx, "not a PgRowStream");
        JS_Call(ctx, funcs[1], JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (it->state == PGQ_ST_DONE || !JS_IsUndefined(it->error)) {
        if (JS_IsUndefined(it->error)) {
            JSValueConst a[1];
            JSValue result = JS_NewObject(ctx), r;
            JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE,
                JS_PROP_C_W_E);
            a[0] = result;
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, r);
        } else {
            JSValueConst a[1] = { it->error };
            JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, a);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (!it->running && !it->qhead && it->state == PGQ_ST_FETCH && !JS_IsUndefined(it->rows) && it->row_index < it->row_count) {
        row = pgq_buffered_row(it);
        it->emitted++;
        if (it->max_rows > 0 && it->emitted >= it->max_rows) {
            JS_FreeValue(ctx, it->rows);
            it->rows = JS_UNDEFINED;
            it->row_index = it->row_count = 0;
            it->state = PGQ_ST_CLEANUP_CLOSE;
        }
        {
            JSValueConst a[1];
            JSValue result = JS_NewObject(ctx), r;
            JS_DefinePropertyValueStr(ctx, result, "value", row,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, result, "done", JS_FALSE,
                JS_PROP_C_W_E);
            a[0] = result;
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, result);
            if (JS_IsException(r))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    op = (pgq_op_t*)calloc(1, sizeof(*op));
    if (!op) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    pgq_park(it, op);
    pgq_pump(ctx, it);
    return promise;
}

static JSValue pgq_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it = (pgq_iter_t*)JS_GetOpaque(this_val, pgq_class_id);
    JSValue funcs[2], promise;
    pgq_op_t* op;

    (void)argc;
    (void)argv;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    if (!it || it->state == PGQ_ST_DONE) {
        JSValueConst a[1];
        JSValue result = JS_NewObject(ctx), r;
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return promise;
        }
        JS_DefinePropertyValueStr(ctx, result, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, result, "done", JS_TRUE, JS_PROP_C_W_E);
        a[0] = result;
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, result);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    op = (pgq_op_t*)calloc(1, sizeof(*op));
    if (!op) {
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->it_obj = JS_DupValue(ctx, this_val);
    op->jresolve = funcs[0];
    op->jreject = funcs[1];
    op->want_return = 1;
    pgq_park(it, op);
    pgq_pump(ctx, it);
    return promise;
}

static JSValue pgq_throw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return pgq_return(ctx, this_val, 0, NULL);
}

static JSValue pgq_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static uint64_t pgq_seq;

static const char* const pgq_opts_keys[] = { "batch", "maxRows" };

static JSValue dyn_pg_queryiter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it;
    JSValue obj = JS_UNDEFINED, qfn;
    const char* sql;
    int64_t batch = 1000, max_rows = 0;

    if (argc > 2 && dyn_opts_strict(ctx, argv[2], pgq_opts_keys, 2))
        return JS_EXCEPTION;
    sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !JS_IsArray(ctx, argv[1])) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx,
            "queryIter: parameters must be an array");
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[2], "batch");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &batch, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "maxRows");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &max_rows, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (batch < 1)
        batch = 1;
    if (max_rows < 0)
        return JS_ThrowRangeError(ctx, "queryIter: maxRows must be >= 0");

    qfn = JS_GetPropertyStr(ctx, this_val, "query");
    if (!JS_IsFunction(ctx, qfn)) {
        JS_FreeValue(ctx, qfn);
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx,
            "queryIter: receiver has no query() method");
    }

    it = (pgq_iter_t*)calloc(1, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, qfn);
        JS_FreeCString(ctx, sql);
        return JS_ThrowOutOfMemory(ctx);
    }
    it->ctx = ctx;
    it->conn_obj = JS_DupValue(ctx, this_val);
    it->query_fn = qfn;
    {
        size_t slen = strlen(sql);
        it->sql = (char*)malloc(slen + 1);
        if (!it->sql) {
            JS_FreeValue(ctx, qfn);
            JS_FreeCString(ctx, sql);
            free(it);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(it->sql, sql, slen + 1);
    }
    JS_FreeCString(ctx, sql);
    it->params = (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        ? JS_DupValue(ctx, argv[1])
        : JS_UNDEFINED;
    it->rows = JS_UNDEFINED;
    it->error = JS_UNDEFINED;
    it->batch = batch;
    it->max_rows = max_rows;
    it->state = PGQ_ST_IDLE;
    snprintf(it->name, sizeof(it->name), "_djq%llu",
        (unsigned long long)++pgq_seq);

    obj = JS_NewObjectClass(ctx, (int)pgq_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, it->conn_obj);
        JS_FreeValue(ctx, it->query_fn);
        JS_FreeValue(ctx, it->params);
        JS_FreeValue(ctx, it->rows);
        JS_FreeValue(ctx, it->error);
        free(it->sql);
        free(it);
        return obj;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

static const JSCFunctionListEntry pgq_proto[] = {
    JS_CFUNC_DEF("next", 0, pgq_next),
    JS_CFUNC_DEF("return", 0, pgq_return),
    JS_CFUNC_DEF("throw", 1, pgq_throw),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, pgq_async_iterator),
};

static dyn_pg_t* pg_this(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_pg_t*)dyn_res_native(ctx, this_val, dyn_pg_class_id);
}

static uint32_t pg_stmt_hash(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static struct pg_stmt* pg_stmt_find(dyn_pg_t* g, const char* sql, size_t n,
    uint32_t h)
{
    int i;
    for (i = 0; i < g->nstmts; i++)
        if (g->stmts[i].hash == h && g->stmts[i].sqllen == n && memcmp(g->stmts[i].sql, sql, n) == 0)
            return &g->stmts[i];
    return NULL;
}

static void pg_stmt_evict_named(dyn_pg_t* g, const char* name)
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

static void pg_stmt_clear(dyn_pg_t* g)
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

static struct pg_stmt* pg_stmt_get(dyn_pg_t* g, const char* sql, size_t n)
{
    uint32_t h;
    struct pg_stmt* s;

    if (g->stmt_cache_max <= 0)
        return NULL;
    h = pg_stmt_hash(sql, n);
    s = pg_stmt_find(g, sql, n, h);
    if (s) {
        if (s->uses < 1000000)
            s->uses++;
        return s;
    }
    if (g->nstmts >= g->stmt_cache_max)
        return NULL;
    if (g->nstmts == g->cap_stmts) {
        int cap = g->cap_stmts ? g->cap_stmts * 2 : 8;
        struct pg_stmt* ns = (struct pg_stmt*)realloc(g->stmts,
            (size_t)cap * sizeof(*ns));
        if (!ns)
            return NULL;
        g->stmts = ns;
        g->cap_stmts = cap;
    }
    s = &g->stmts[g->nstmts];
    memset(s, 0, sizeof(*s));
    s->sql = (char*)malloc(n + 1);
    if (!s->sql)
        return NULL;
    memcpy(s->sql, sql, n);
    s->sql[n] = '\0';
    s->sqllen = n;
    s->hash = h;
    s->uses = 1;
    snprintf(s->name, sizeof(s->name), "djs%u", ++g->stmt_seq);
    g->nstmts++;
    return s;
}

static int pg_encode_query(pgw_t* w, const char* sql, int nparam,
    const char** pv, const size_t* plen,
    const uint8_t* pnull,
    const char* stmt_name, int stmt_prepared,
    const uint8_t* rfmt, int nrfmt)
{
    int i;
    if (!stmt_prepared) {
        size_t at = pgw_begin(w, 'P');
        pgw_str(w, stmt_name ? stmt_name : "");
        pgw_str(w, sql);
        pgw_u16(w, 0);
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'B');
        pgw_u8(w, 0);
        pgw_str(w, stmt_name ? stmt_name : "");
        pgw_u16(w, 0);
        pgw_u16(w, (uint16_t)nparam);
        for (i = 0; i < nparam; i++) {
            if (pnull[i]) {
                pgw_u32(w, 0xffffffffu);
            } else {
                pgw_u32(w, (uint32_t)plen[i]);
                pgw_raw(w, pv[i], plen[i]);
            }
        }
        if (rfmt && nrfmt > 0) {
            int k;
            pgw_u16(w, (uint16_t)nrfmt);
            for (k = 0; k < nrfmt; k++)
                pgw_u16(w, rfmt[k] ? 1 : 0);
        } else {
            pgw_u16(w, 0);
        }
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'D');
        pgw_u8(w, 'P');
        pgw_u8(w, 0);
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'E');
        pgw_u8(w, 0);
        pgw_u32(w, 0);
        pgw_end(w, at);
    }
    return w->bad ? -1 : 0;
}

static void pg_param_arrays_free(const char** pv, size_t* plen,
    uint8_t* pnull, uint8_t* pfree)
{
    (void)plen;
    (void)pnull;
    (void)pfree;
    free((void*)pv);
}

static int pg_coerce_params(JSContext* ctx, JSValueConst params,
    const char*** out_pv, size_t** out_plen,
    uint8_t** out_pnull, uint8_t** out_pfree,
    uint32_t* out_n, const char* what)
{
    const char** pv = NULL;
    size_t* plen = NULL;
    uint8_t *pnull = NULL, *pfree = NULL;
    uint32_t nparam = 0, i;
    JSValue lv;

    if (!JS_IsArray(ctx, params)) {
        JS_ThrowTypeError(ctx, "%s: parameters must be an array", what);
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, params, "length");
    if (JS_ToUint32(ctx, &nparam, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (nparam > 65535) {
        JS_ThrowRangeError(ctx, "%s: at most 65535 parameters", what);
        return -1;
    }
    *out_n = nparam;
    if (nparam == 0)
        return 0;
    pv = (const char**)calloc(1, (size_t)nparam * (sizeof(*pv) + sizeof(*plen)) + (size_t)nparam * 2);
    if (!pv) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    plen = (size_t*)(pv + nparam);
    pnull = (uint8_t*)(plen + nparam);
    pfree = pnull + nparam;
    for (i = 0; i < nparam; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, params, i);
        if (JS_IsUndefined(e) || JS_IsNull(e)) {
            pnull[i] = 1;
            JS_FreeValue(ctx, e);
            continue;
        }
        if (JS_IsObject(e)) {
            int enc = pg_param_bytea(ctx, e, &pv[i], &plen[i]);
            if (enc > 0) {
                pfree[i] = PG_PF_MALLOC;
                JS_FreeValue(ctx, e);
                continue;
            }
            if (enc < 0) {
                JS_FreeValue(ctx, e);
                goto fail;
            }
            JS_FreeValue(ctx, e);
            pg_free_params(ctx, pv, pfree, i);
            pg_param_arrays_free(pv, plen, pnull, pfree);
            JS_ThrowTypeError(ctx,
                "%s: parameter %u is an object; a parameter is a value. "
                "Pass a Uint8Array or ArrayBuffer for bytea, "
                "JSON.stringify(v) for json/jsonb, "
                "or an ISO string for a timestamp",
                what, (unsigned)i + 1);
            return -1;
        }
        pv[i] = JS_ToCStringLen(ctx, &plen[i], e);
        pfree[i] = PG_PF_CSTR;
        JS_FreeValue(ctx, e);
        if (!pv[i])
            goto fail;
    }
    *out_pv = pv;
    *out_plen = plen;
    *out_pnull = pnull;
    *out_pfree = pfree;
    return 0;

fail:
    pg_free_params(ctx, pv, pfree, i);
    pg_param_arrays_free(pv, plen, pnull, pfree);
    return -1;
}

static int pg_encode_member(dyn_pg_t* g, pgw_t* w, const char* sql,
    int nparam, const char** pv, const size_t* plen,
    const uint8_t* pnull, char* name_out,
    size_t name_outcap)
{
    struct pg_stmt* stmt = NULL;
    const char* stmt_name = NULL;
    int stmt_prepared = 0;
    uint8_t* rfmt = NULL;
    int nrfmt = 0;

    stmt = pg_stmt_get(g, sql, strlen(sql));
    if (stmt && stmt->uses >= g->prepare_after) {
        stmt_name = stmt->name;
        stmt_prepared = stmt->prepared;
        if (g->binary_results && stmt->oids && stmt->noids > 0) {
            rfmt = (uint8_t*)malloc((size_t)stmt->noids);
            if (rfmt) {
                int k;
                nrfmt = stmt->noids;
                for (k = 0; k < nrfmt; k++)
                    rfmt[k] = (uint8_t)pg_oid_prefers_binary(stmt->oids[k]);
            }
        }
        stmt->prepared = 1;
        g->n_prepared_hits++;
    } else {
        g->n_unnamed++;
    }

    if (name_out && stmt_name)
        snprintf(name_out, name_outcap, "%s", stmt_name);
    {
        int rc = pg_encode_query(w, sql, nparam, pv, plen, pnull,
            stmt_name, stmt_prepared, rfmt, nrfmt);
        free(rfmt);
        return rc;
    }
}

static void pg_submit(dyn_pg_t* g, dyn_pg_pending_t* p, uint8_t* bytes,
    size_t nbytes, uint64_t esync, int sync_end)
{
    p->bytes = bytes;
    p->nbytes = nbytes;
    p->esync = esync;
    p->sync_end = sync_end;
    if (g->state == PG_ST_READY) {
        if (pg_write(g, bytes, nbytes) < 0) {
            return;
        }
        free(bytes);
        p->bytes = NULL;
        p->nbytes = 0;
        if (g->query_timeout_ms)
            p->deadline_ms = dyn_timer_now_ms() + g->query_timeout_ms;
        pgp_push(&g->head, &g->tail, p);
        g->npending++;
    } else {
        pgp_push(&g->wq_head, &g->wq_tail, p);
        g->nwait++;
    }
}

static JSValue dyn_pg_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    const char* sql = NULL;
    const char** pv = NULL;
    size_t* plen = NULL;
    uint8_t *pnull = NULL, *pfree = NULL;
    uint32_t nparam = 0;
    int have_params = 0;
    char pending_name[24];
    pgw_t w;
    dyn_pg_pending_t* p;
    JSValue funcs[2], promise;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "query: a statement is required");

    if (argc > 2 && dyn_opts_strict(ctx, argv[2], pg_query_keys, 1))
        return JS_EXCEPTION;

    sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (pg_coerce_params(ctx, argv[1], &pv, &plen, &pnull, &pfree,
                &nparam, "query")
            < 0) {
            JS_FreeCString(ctx, sql);
            return JS_EXCEPTION;
        }
        have_params = 1;
    }

    g = pg_this(ctx, this_val);
    if (!g || g->state == PG_ST_DEAD) {
        pg_free_params(ctx, pv, pfree, nparam);
        pg_param_arrays_free(pv, plen, pnull, pfree);
        JS_FreeCString(ctx, sql);
        return g ? JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed")
                 : JS_EXCEPTION;
    }
    if (g->npending + g->nwait >= g->maxpending) {
        pg_free_params(ctx, pv, pfree, nparam);
        pg_param_arrays_free(pv, plen, pnull, pfree);
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: %d queries already in flight (maxPending)", g->maxpending);
    }

    memset(&w, 0, sizeof(w));
    if (have_params) {
        char name[24];
        name[0] = '\0';
        if (pg_encode_member(g, &w, sql, (int)nparam, pv, plen, pnull,
                name, sizeof(name))
            < 0) {
            pg_free_params(ctx, pv, pfree, nparam);
            pg_param_arrays_free(pv, plen, pnull, pfree);
            JS_FreeCString(ctx, sql);
            return JS_ThrowOutOfMemory(ctx);
        }
        {
            size_t at = pgw_begin(&w, 'S');
            pgw_end(&w, at);
        }
        snprintf(pending_name, sizeof(pending_name), "%s", name);
    } else {
        size_t at = pgw_begin(&w, 'Q');
        pgw_str(&w, sql);
        pgw_end(&w, at);
        {
            size_t at2 = pgw_begin(&w, 'S');
            pgw_end(&w, at2);
        }
        pending_name[0] = '\0';
    }
    pg_free_params(ctx, pv, pfree, nparam);
    pg_param_arrays_free(pv, plen, pnull, pfree);
    JS_FreeCString(ctx, sql);

    int64_t max_rows_opt = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[2], "maxRows");
        if (JS_IsException(mv)) {
            JS_FreeValue(ctx, mv);
            free(w.b);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
            int64_t mr = 0;
            if (JS_ToInt64(ctx, &mr, mv) || mr < 0) {
                JS_FreeValue(ctx, mv);
                free(w.b);
                return JS_ThrowRangeError(ctx, "PostgreSQL: maxRows must be >= 0");
            }
            max_rows_opt = mr;
        }
        JS_FreeValue(ctx, mv);
    }
    p = pgp_new();
    if (!p) {
        free(w.b);
        return JS_ThrowOutOfMemory(ctx);
    }
    p->max_rows = max_rows_opt;
    snprintf(p->stmt_name, sizeof(p->stmt_name), "%s", pending_name);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(w.b);
        pgp_free(ctx, p);
        return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];

    pg_submit(g, p, w.b, w.len, ++g->sync_seq, 1);
    if (g->state == PG_ST_READY) {
        if (p->bytes) {
            free(p->bytes);
            pgp_free(ctx, p);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        pg_flush_soon(g);
    }
    return promise;
}

static JSValue dyn_pg_pipeline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    uint32_t n = 0, i, coerced = 0, encoded = 0, submitted = 0;
    JSValue lv, promise;
    JSValue funcs[2];
    pgw_t* ws = NULL;
    char (*names)[24];
    const char** sqls = NULL;
    const char*** pvs = NULL;
    size_t** plens = NULL;
    uint8_t **pnulls = NULL, **pfrees = NULL;
    uint32_t* nparams = NULL;
    dyn_pg_batch_t* b;
    dyn_pg_pending_t* p;
    uint64_t esync;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "pipeline: expects an array of [sql, params?] elements");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n == 0)
        return JS_ThrowTypeError(ctx, "pipeline: at least one statement");
    if (n > 65535)
        return JS_ThrowRangeError(ctx, "pipeline: at most 65535 statements");

    sqls = (const char**)calloc(n, sizeof(*sqls));
    pvs = (const char***)calloc(n, sizeof(*pvs));
    plens = (size_t**)calloc(n, sizeof(*plens));
    pnulls = (uint8_t**)calloc(n, sizeof(*pnulls));
    pfrees = (uint8_t**)calloc(n, sizeof(*pfrees));
    nparams = (uint32_t*)calloc(n, sizeof(*nparams));
    ws = (pgw_t*)calloc(n, sizeof(*ws));
    names = (char (*)[24])calloc(n, sizeof(*names));
    if (!sqls || !pvs || !plens || !pnulls || !pfrees || !nparams || !ws || !names)
        goto oom_pre;

#define PGPIPE_MEMBER_FREE(k)                                         \
    do {                                                              \
        if (sqls[k])                                                  \
            JS_FreeCString(ctx, sqls[k]);                             \
        pg_free_params(ctx, pvs[k], pfrees[k], nparams[k]);           \
                                                                      \
        pg_param_arrays_free(pvs[k], plens[k], pnulls[k], pfrees[k]); \
        free(ws[k].b);                                                \
        ws[k].b = NULL;                                               \
    } while (0)

    for (i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, argv[0], i);
        JSValue sqlv, par;
        uint32_t m = 0;

        if (!JS_IsArray(ctx, el)) {
            JS_FreeValue(ctx, el);
            JS_ThrowTypeError(ctx, "pipeline: element %u is not [sql, params?]",
                i);
            goto fail_coerced;
        }
        {
            JSValue l2 = JS_GetPropertyStr(ctx, el, "length");
            int rc = JS_ToUint32(ctx, &m, l2);
            JS_FreeValue(ctx, l2);
            if (rc < 0) {
                JS_FreeValue(ctx, el);
                goto fail_coerced;
            }
        }
        if (m < 1) {
            JS_FreeValue(ctx, el);
            JS_ThrowTypeError(ctx, "pipeline: element %u is empty", i);
            goto fail_coerced;
        }
        sqlv = JS_GetPropertyUint32(ctx, el, 0);
        {
            size_t slen = 0;
            sqls[i] = JS_ToCStringLen(ctx, &slen, sqlv);
        }
        JS_FreeValue(ctx, sqlv);
        if (!sqls[i]) {
            JS_FreeValue(ctx, el);
            goto fail_coerced;
        }
        par = m > 1 ? JS_GetPropertyUint32(ctx, el, 1) : JS_UNDEFINED;
        if (!JS_IsUndefined(par) && !JS_IsNull(par)) {
            if (pg_coerce_params(ctx, par, &pvs[i], &plens[i], &pnulls[i],
                    &pfrees[i], &nparams[i], "pipeline")
                < 0) {
                JS_FreeValue(ctx, par);
                JS_FreeValue(ctx, el);
                goto fail_coerced;
            }
        }
        JS_FreeValue(ctx, par);
        JS_FreeValue(ctx, el);
        coerced = i + 1;
    }

    g = pg_this(ctx, this_val);
    if (!g || g->state == PG_ST_DEAD) {
        JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed");
        goto fail_ready;
    }
    if ((uint64_t)g->npending + g->nwait + n > (uint64_t)g->maxpending) {
        JS_ThrowInternalError(ctx,
            "PostgreSQL: %d queries already in flight (maxPending)",
            g->maxpending);
        goto fail_ready;
    }

    for (i = 0; i < n; i++) {
        if (pg_encode_member(g, &ws[i], sqls[i], (int)nparams[i], pvs[i],
                plens[i], pnulls[i], names[i], sizeof(names[0]))
            < 0)
            break;
        encoded = i + 1;
        if (i == n - 1) {
            size_t at = pgw_begin(&ws[i], 'S');
            pgw_end(&ws[i], at);
        }
    }
    if (encoded < n) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }

    b = (dyn_pg_batch_t*)calloc(1, sizeof(*b));
    if (!b) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }
    b->ctx = ctx;
    b->slots = (JSValue*)calloc(n, sizeof(*b->slots));
    if (!b->slots) {
        free(b);
        b = NULL;
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(b->slots);
        free(b);
        goto fail_ready;
    }
    b->resolve = funcs[0];
    b->reject = funcs[1];
    b->k = (int)n;
    for (i = 0; i < n; i++)
        b->slots[i] = JS_UNDEFINED;

    esync = ++g->sync_seq;
    for (i = 0; i < n; i++) {
        p = pgp_new();
        if (!p)
            break;
        snprintf(p->stmt_name, sizeof(p->stmt_name), "%s", names[i]);
        p->batch = b;
        p->bidx = (int)i;
        pg_submit(g, p, ws[i].b, ws[i].len, esync, i == n - 1);
        ws[i].b = NULL;
        if (g->state == PG_ST_READY && p->bytes) {
            free(p->bytes);
            pgp_free(ctx, p);
            break;
        }
        submitted = i + 1;
    }
    if (submitted < n && !b->dead && !b->settled)
        pg_batch_fail(g, b, "PostgreSQL: out of memory queueing the pipeline");

    if (g->state == PG_ST_READY)
        pg_flush_soon(g);
    for (i = 0; i < n; i++)
        PGPIPE_MEMBER_FREE(i);
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return promise;

fail_coerced:
    {
        uint32_t k;
        for (k = 0; k < n && k <= coerced; k++)
            PGPIPE_MEMBER_FREE(k);
    }
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_EXCEPTION;

fail_ready:
    for (i = 0; i < coerced; i++)
        PGPIPE_MEMBER_FREE(i);
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_EXCEPTION;

oom_pre:
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_ThrowOutOfMemory(ctx);

#undef PGPIPE_MEMBER_FREE
}

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    int fd, done, hooked;
    uint64_t deadline_ms;
    uint8_t msg[12 + PG_MAX_CANCEL_KEY];
    size_t len;
} pg_cancel_t;

static void pg_cancel_tick(void* udata);

static void pg_cancel_finish(pg_cancel_t* c)
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

static void pg_cancel_eof(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    (void)aio;
    (void)res;
    (void)buf;
    (void)len;
    pg_cancel_finish((pg_cancel_t*)ud);
}

static void pg_cancel_tick(void* udata)
{
    pg_cancel_t* c = (pg_cancel_t*)udata;
    if (!c->done && c->deadline_ms && dyn_timer_now_ms() >= c->deadline_ms)
        pg_cancel_finish(c);
}

static void pg_cancel_connected(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    pg_cancel_t* c = (pg_cancel_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;

    if (res < 0) {
        pg_cancel_finish(c);
        return;
    }
    if (dyn_aio_recv(c->aio, c->fd, 0, 0, pg_cancel_eof, c) < 0) {
        pg_cancel_finish(c);
        return;
    }
    if (dyn_aio_send(c->aio, c->fd, c->msg, c->len, 0, NULL, NULL) < 0)
        pg_cancel_finish(c);
}

static JSValue dyn_pg_cancel(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    pg_cancel_t* c;
    (void)argc;
    (void)argv;

    if (!g)
        return JS_EXCEPTION;
    if (!g->cancel_key_len)
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: no cancel key yet; the server sends it during startup");
    c = (pg_cancel_t*)calloc(1, sizeof(*c));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->ctx = ctx;
    c->fd = -1;
    c->len = 12 + g->cancel_key_len;
    put32(c->msg, (uint32_t)c->len);
    put32(c->msg + 4, PG_CANCEL_CODE);
    put32(c->msg + 8, g->backend_pid);
    memcpy(c->msg + 12, g->cancel_key, g->cancel_key_len);

    c->aio = dyn_net_reactor_acquire(ctx);
    if (!c->aio) {
        free(c);
        return JS_ThrowInternalError(ctx, "PostgreSQL: cancel: no reactor");
    }
    c->deadline_ms = dyn_timer_now_ms() + (g->query_timeout_ms ? g->query_timeout_ms : PG_CONNECT_TIMEOUT);
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

static JSValue dyn_pg_on(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    const char* ev;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on(event, handler)");
    ev = JS_ToCString(ctx, argv[0]);
    if (!ev)
        return JS_EXCEPTION;
    g = pg_this(ctx, this_val);
    if (!g) {
        JS_FreeCString(ctx, ev);
        return JS_EXCEPTION;
    }
    if (strcmp(ev, "notice") == 0) {
        JS_FreeValue(ctx, g->h_notice);
        g->h_notice = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "notification") == 0) {
        JS_FreeValue(ctx, g->h_notify);
        g->h_notify = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(ev, "error") == 0) {
        JS_FreeValue(ctx, g->h_error);
        g->h_error = JS_DupValue(ctx, argv[1]);
    } else {
        JS_FreeCString(ctx, ev);
        return JS_ThrowRangeError(ctx,
            "on: want 'notice', 'notification' or 'error'");
    }
    JS_FreeCString(ctx, ev);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_pg_get_ready(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewBool(ctx, g->state == PG_ST_READY) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_pending(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewInt32(ctx, g->npending + g->nwait) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_pid(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewUint32(ctx, g->backend_pid) : JS_EXCEPTION;
}
static JSValue dyn_pg_get_tx(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    char t[2];
    if (!g)
        return JS_EXCEPTION;
    t[0] = g->tx_status ? g->tx_status : 'I';
    t[1] = '\0';
    return JS_NewString(ctx, t);
}
static JSValue dyn_pg_get_params(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_DupValue(ctx, g->params) : JS_EXCEPTION;
}

static void dyn_pg_teardown(dyn_pg_t* g);

static void dyn_pg_dispose(void* native)
{
    dyn_pg_t* g = (dyn_pg_t*)native;

    if (!g)
        return;
    if (g->in_cb) {
        g->closing = 1;
        return;
    }
    dyn_pg_teardown(g);
}

static void pg_close_out(dyn_pg_t* g, dyn_pg_pending_t* p)
{
    if (pg_in_final) {
        pgp_free_rt(g->rt, p);
        return;
    }
    pg_settle(g, p, 1, pg_conn_error(g->ctx, "PostgreSQL: client closed"));
}

static void dyn_pg_teardown(dyn_pg_t* g)
{
    dyn_pg_pending_t* p;
    pg_flush_drop(g);
    pg_stmt_clear(g);
    if (g->hooked)
        dyn_net_off_drain(g);
    if (g->state == PG_ST_READY && g->fd >= 0) {
        static const uint8_t bye[5] = { 'X', 0, 0, 0, 4 };
        (void)dyn_aio_send(g->aio, g->fd, bye, sizeof(bye), 0, NULL, NULL);
    }
    if (g->state != PG_ST_DEAD) {
        g->state = PG_ST_DEAD;
        while ((p = pgp_pop(&g->head, &g->tail)) != NULL) {
            if (p->batch)
                pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
            pg_close_out(g, p);
        }
        while ((p = pgp_pop(&g->dhead, &g->dtail)) != NULL) {
            if (p->batch)
                pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
            pg_close_out(g, p);
        }
        while ((p = pgp_pop(&g->wq_head, &g->wq_tail)) != NULL) {
            if (p->batch)
                pg_batch_fail(g, p->batch, "PostgreSQL: client closed");
            pg_close_out(g, p);
        }
    }
    if (g->aio && !g->released) {
        if (g->fd >= 0)
            dyn_aio_close(g->aio, g->fd);
        dyn_net_reactor_release_rt(g->rt);
    }
    dyn_scram_free(&g->scram);
    JS_FreeValueRT(g->rt, g->params);
    JS_FreeValueRT(g->rt, g->h_notice);
    JS_FreeValueRT(g->rt, g->h_notify);
    JS_FreeValueRT(g->rt, g->h_error);
    if (g->pass) {
        memset(g->pass, 0, strlen(g->pass));
        free(g->pass);
    }
    free(g->host);
    free(g->path);
    free(g->user);
    free(g->database);
    free(g->appname);
    free(g->rbuf);
    free(g->obuf);
#ifdef CONFIG_TLS
    if (g->tls_ctx)
        dyn_tls_ctx_free(g->tls_ctx);
    free(g->tls_ca);
#endif
    memset(g, 0, sizeof(*g));
    free(g);
}

static char* pg_opt_str(JSContext* ctx, JSValueConst o, const char* k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char* s;
    char* out = NULL;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (s) {
        out = strdup(s);
        JS_FreeCString(ctx, s);
    }
    return out;
}
static int pg_opt_int(JSContext* ctx, JSValueConst o, const char* k, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return dflt;
    }
    if (JS_ToInt32(ctx, &n, v) < 0) {
        JS_FreeValue(ctx, v);
        return dflt;
    }
    JS_FreeValue(ctx, v);
    return (int)n;
}
static int pg_opt_bool(JSContext* ctx, JSValueConst o, const char* k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static JSValue dyn_pg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    JSValueConst opt = argc > 0 ? argv[0] : JS_UNDEFINED;
    int ct, qt, mm, mp;

    if (argc > 0 && !JS_IsObject(opt))
        return JS_ThrowTypeError(ctx, "PostgreSQL: expects an options object");
#ifdef CONFIG_TLS
#else
    if (JS_IsObject(opt) && pg_opt_bool(ctx, opt, "tls"))
        return JS_ThrowTypeError(ctx,
            "PostgreSQL: TLS is not supported in this build");
#endif

    g = (dyn_pg_t*)calloc(1, sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->ctx = ctx;
    g->rt = JS_GetRuntime(ctx);
    g->fd = -1;
    g->state = PG_ST_CONNECTING;
#ifdef CONFIG_TLS
    if (JS_IsObject(opt)) {
        g->use_tls = pg_opt_bool(ctx, opt, "tls");
        if (g->use_tls) {
            g->tls_ca = pg_opt_str(ctx, opt, "ca");
        }
    }
#endif
    g->maxmsg = PG_DEFAULT_MAXMSG;
    g->maxpending = 1024;
    g->stmt_cache_max = 64;
    g->prepare_after = 2;
    g->binary_results = 1;
    g->tx_status = 'I';
    g->params = JS_NewObject(ctx);
    g->h_notice = g->h_notify = g->h_error = JS_UNDEFINED;
    g->port = 5432;

    if (JS_IsObject(opt)) {
        if (dyn_opts_strict(ctx, opt, pg_ctor_keys, 20)) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
        }
        g->host = pg_opt_str(ctx, opt, "host");
        g->path = pg_opt_str(ctx, opt, "path");
        g->user = pg_opt_str(ctx, opt, "user");
        g->pass = pg_opt_str(ctx, opt, "password");
        g->database = pg_opt_str(ctx, opt, "database");
        g->appname = pg_opt_str(ctx, opt, "applicationName");
        {
            int pi = pg_opt_int(ctx, opt, "port", 5432);
            if ((pi < 1 || pi > 65535) && !g->path) {
                dyn_pg_dispose(g);
                return JS_ThrowRangeError(ctx, "PostgreSQL: port must be 1..65535");
            }
            g->port = (uint16_t)(pi > 0 ? pi : 0);
        }
        g->raw = pg_opt_bool(ctx, opt, "raw");
        g->bytes_out = pg_opt_bool(ctx, opt, "bytes");
        g->binary_results = !pg_opt_bool(ctx, opt, "textResults");
        {
            int sc = pg_opt_int(ctx, opt, "statementCacheSize", -1);
            if (sc >= 0)
                g->stmt_cache_max = sc;
            sc = pg_opt_int(ctx, opt, "prepareAfter", 0);
            if (sc > 0)
                g->prepare_after = sc;
        }
        g->bigint = pg_opt_bool(ctx, opt, "bigint");
        g->insecure_auth = pg_opt_bool(ctx, opt, "insecureAuth");
        mm = pg_opt_int(ctx, opt, "maxMessageBytes", 0);
        if (mm > 0)
            g->maxmsg = (size_t)mm;
        mp = pg_opt_int(ctx, opt, "maxPending", 0);
        if (mp > 0)
            g->maxpending = mp;
        qt = pg_opt_int(ctx, opt, "queryTimeoutMs", 0);
        if (qt > 0)
            g->query_timeout_ms = (uint64_t)qt;
        ct = pg_opt_int(ctx, opt, "connectTimeoutMs", PG_CONNECT_TIMEOUT);
        if (ct > 0)
            g->connect_deadline_ms = dyn_timer_now_ms() + (uint64_t)ct;
        if (JS_HasException(ctx)) {
            dyn_pg_dispose(g);
            return JS_EXCEPTION;
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
    return dyn_res_wrap(ctx, new_target, dyn_pg_class_id, g, dyn_pg_dispose);
}

static void dyn_pg_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* res = (DynResource*)JS_GetOpaque(val, dyn_pg_class_id);
    dyn_pg_t* g;

    if (!res || res->closed || !res->native)
        return;
    g = (dyn_pg_t*)res->native;
    JS_MarkValue(rt, g->params, mark_func);
    JS_MarkValue(rt, g->h_notice, mark_func);
    JS_MarkValue(rt, g->h_notify, mark_func);
    JS_MarkValue(rt, g->h_error, mark_func);
}

static const JSClassDef dyn_pg_class = {
    "PostgreSQL",
    .finalizer = dyn_pg_finalizer,
    .gc_mark = dyn_pg_gc_mark,
};

static JSValue dyn_pg_get_stmt_stats(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    JSValue o;
    if (!g)
        return JS_EXCEPTION;
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
    JS_CFUNC_DEF("queryIter", 1, dyn_pg_queryiter),
    JS_CFUNC_DEF("pipeline", 1, dyn_pg_pipeline),
    JS_CFUNC_DEF("cancel", 0, dyn_pg_cancel),
    JS_CFUNC_DEF("on", 2, dyn_pg_on),
    JS_CGETSET_DEF("ready", dyn_pg_get_ready, NULL),
    JS_CGETSET_DEF("statementCache", dyn_pg_get_stmt_stats, NULL),
    JS_CGETSET_DEF("pending", dyn_pg_get_pending, NULL),
    JS_CGETSET_DEF("backendPid", dyn_pg_get_pid, NULL),
    JS_CGETSET_DEF("transactionStatus", dyn_pg_get_tx, NULL),
    JS_CGETSET_DEF("parameters", dyn_pg_get_params, NULL),
};

int dyn_pg_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_pg_class_id, &dyn_pg_class,
            dyn_pg_proto, countof(dyn_pg_proto),
            dyn_pg_ctor, "PostgreSQL")
        < 0)
        return -1;
    JS_NewClassID(&pgq_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), pgq_class_id, &pgq_class) < 0)
        return -1;
    {
        JSValue cp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, cp, pgq_proto, countof(pgq_proto));
        JS_SetClassProto(ctx, pgq_class_id, cp);
    }
    return 0;
}

void dyn_pg_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "PostgreSQL");
}

#endif
