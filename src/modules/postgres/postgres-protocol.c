#include "postgres-internal.h"
#include "core/dyn-pgwire.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#define PG_MAX_PARAMS 256
#define PG_AUTH_OK 0
#define PG_AUTH_CLEARTEXT 3
#define PG_AUTH_MD5 5
#define PG_AUTH_SASL 10
#define PG_AUTH_SASL_CONT 11
#define PG_AUTH_SASL_FINAL 12
#define PG_ROW_TEMPLATE_MAX_FIELDS 32

dyn_pg_pending_t* pgp_new(void)
{
    dyn_pg_pending_t* p = (dyn_pg_pending_t*)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->resolve = p->reject = p->rows = p->fields = p->error = JS_UNDEFINED;
    return p;
}

void pgp_free_fields(JSContext* ctx, dyn_pg_pending_t* p)
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

void pgp_free(JSContext* ctx, dyn_pg_pending_t* p)
{
    if (p->copy)
        pg_copy_detach(ctx, JS_GetRuntime(ctx), p->copy);
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->rows);
    JS_FreeValue(ctx, p->fields);
    JS_FreeValue(ctx, p->error);
    pgp_free_fields(ctx, p);
    free(p->bytes);
    free(p);
}

void pgp_free_rt(JSRuntime* rt, dyn_pg_pending_t* p)
{
    int i;
    if (p->copy)
        pg_copy_detach(NULL, rt, p->copy);
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

void pgp_push(dyn_pg_pending_t** h, dyn_pg_pending_t** t, dyn_pg_pending_t* p)
{
    p->next = NULL;
    if (*t)
        (*t)->next = p;
    else
        *h = p;
    *t = p;
}

dyn_pg_pending_t* pgp_pop(dyn_pg_pending_t** h, dyn_pg_pending_t** t)
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

static int pg_send_now(dyn_pg_t* g, const uint8_t* b, size_t n)
{
    if (pg_write(g, b, n) < 0)
        return -1;
    return pg_flush(g);
}

static JSValue pg_error_value(JSContext* ctx, const uint8_t* p, size_t len)
{
    JSValue e = JS_NewError(ctx);
    size_t i = 0, n;
    const uint8_t *sev_s = NULL, *s;
    size_t sev_s_len = 0;
    int have_v = 0;
    char code;
    while (dyn_pgw_field_next(p, len, &i, &code, &s, &n)) {
        switch (code) {
        case 'M':
            JS_DefinePropertyValueStr(ctx, e, "message",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'C':
            JS_DefinePropertyValueStr(ctx, e, "code",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'V':
            have_v = 1;
            JS_DefinePropertyValueStr(ctx, e, "severity",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'S':
            sev_s = s;
            sev_s_len = n;
            break;
        case 'D':
            JS_DefinePropertyValueStr(ctx, e, "detail",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'H':
            JS_DefinePropertyValueStr(ctx, e, "hint",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'P':
            JS_DefinePropertyValueStr(ctx, e, "position",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 's':
            JS_DefinePropertyValueStr(ctx, e, "schema",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 't':
            JS_DefinePropertyValueStr(ctx, e, "table",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'c':
            JS_DefinePropertyValueStr(ctx, e, "column",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        case 'n':
            JS_DefinePropertyValueStr(ctx, e, "constraint",
                JS_NewStringLen(ctx, (const char*)s, n), JS_PROP_C_W_E);
            break;
        default:
            break;
        }
    }
    if (!have_v && sev_s)
        JS_DefinePropertyValueStr(ctx, e, "severity",
            JS_NewStringLen(ctx, (const char*)sev_s, sev_s_len), JS_PROP_C_W_E);
    return e;
}

static void pg_md5_auth(const char* pass, const char* user,
    const uint8_t salt[4], char out[36])
{
    uint8_t d1[16], d2[16];
    char h1[33];
    size_t n1 = strlen(pass), n2 = strlen(user);
    size_t need = n1 + n2 + 16 > 36 ? n1 + n2 + 16 : 36;
    uint8_t* tmp = (uint8_t*)malloc(need);
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
    memset(tmp, 0, need);
    free(tmp);
    memcpy(out, "md5", 3);
    dyn_codec_hex_encode(d2, 16, out + 3);
    out[35] = '\0';
}

static int pg_err_is(const uint8_t* b, size_t len, const char* code)
{
    size_t i = 0, cl = strlen(code);
    while (i < len && b[i]) {
        char t = (char)b[i++];
        size_t s = i;
        while (i < len && b[i])
            i++;
        if (t == 'C' && i - s == cl && !memcmp(b + s, code, cl))
            return 1;
        i++;
    }
    return 0;
}

struct pg_scram_job {
    dyn_pg_t* g;
    struct pg_scram_job* next;
    struct pg_scram_job* prev;
    dyn_scram_t s;
    char* msg;
    size_t len;
    char* pw;
    int rc;
    char out[1024];
};

static _Thread_local struct pg_scram_job* pg_jobs;

static void pg_wipe(void* p, size_t n)
{
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--)
        *v++ = 0;
}

static void pg_scram_job_free(struct pg_scram_job* j)
{
    if (j->next)
        j->next->prev = j->prev;
    if (j->prev)
        j->prev->next = j->next;
    else if (pg_jobs == j)
        pg_jobs = j->next;
    if (j->pw) {
        pg_wipe(j->pw, strlen(j->pw));
        free(j->pw);
    }
    if (j->msg) {
        pg_wipe(j->msg, j->len);
        free(j->msg);
    }
    pg_wipe(j, sizeof *j);
    free(j);
}

static void pg_scram_work(void* a)
{
    struct pg_scram_job* j = (struct pg_scram_job*)a;
    j->rc = dyn_scram_server_first(&j->s, j->msg, j->len, j->pw, j->out, sizeof j->out);
}

static void pg_scram_done(void* a)
{
    struct pg_scram_job* j = (struct pg_scram_job*)a;
    dyn_pg_t* g = j->g;
    pgw_t w;
    size_t at;
    if (!g) {
        dyn_scram_free(&j->s);
        pg_scram_job_free(j);
        return;
    }
    g->scram_job = NULL;
    g->scram = j->s;
    g->cb_depth++;
    if (pg_gone(g)) {
    } else if (j->rc < 0) {
        pg_fail_all(g, dyn_scram_strerror(j->rc));
    } else {
        memset(&w, 0, sizeof w);
        at = pgw_begin(&w, 'p');
        pgw_raw(&w, j->out, (size_t)j->rc);
        pgw_end(&w, at);
        if (w.bad || pg_send_now(g, w.b, w.len) < 0)
            pg_fail_all(g, "PostgreSQL: cannot send the authentication reply");
        free(w.b);
    }
    pg_scram_job_free(j);
    pg_cb_exit(g);
}

void pg_scram_job_orphan(dyn_pg_t* g)
{
    if (g->scram_job) {
        g->scram_job->g = NULL;
        g->scram_job = NULL;
    }
}

void pg_scram_jobs_reap(void)
{
    while (pg_jobs) {
        struct pg_scram_job* j = pg_jobs;
        if (j->g)
            j->g->scram_job = NULL;
        dyn_scram_free(&j->s);
        pg_scram_job_free(j);
    }
}

static void pg_scram_job_start(dyn_pg_t* g, const uint8_t* msg, size_t len)
{
    struct pg_scram_job* j = (struct pg_scram_job*)calloc(1, sizeof *j);
    if (j) {
        j->msg = (char*)malloc(len ? len : 1);
        j->pw = g->pass ? strdup(g->pass) : NULL;
    }
    if (!j || !j->msg || !j->pw) {
        if (j)
            pg_scram_job_free(j);
        pg_fail_all(g, "PostgreSQL: out of memory during SCRAM");
        return;
    }
    memcpy(j->msg, msg, len);
    j->len = len;
    j->g = g;
    j->s = g->scram;
    memset(&g->scram, 0, sizeof g->scram);
    g->scram_job = j;
    j->next = pg_jobs;
    if (pg_jobs)
        pg_jobs->prev = j;
    pg_jobs = j;
    if (dyn_net_on_reactor_free(pg_scram_jobs_reap) != 0 || dyn_aio_offload_cpu(g->aio, pg_scram_work, pg_scram_done, j) != 0) {
        pg_scram_work(j);
        pg_scram_done(j);
    }
}

static void pg_handle_npv(dyn_pg_t* g, const uint8_t* b, size_t len)
{
    uint32_t minor = 0, nopt = 0;
    char m[160];
    if (g->npv_seen || g->auth_seen || g->state == PG_ST_READY) {
        pg_fail_all(g, "PostgreSQL: NegotiateProtocolVersion out of order (it may only precede authentication, once)");
        return;
    }
    g->npv_seen = 1;
    if (dyn_pgw_negotiate_version(b, len, &minor, &nopt) != 0) {
        pg_fail_all(g, "PostgreSQL: malformed NegotiateProtocolVersion");
        return;
    }
    if (nopt) {
        pg_fail_all(g, "PostgreSQL: NegotiateProtocolVersion named protocol options that were never requested");
        return;
    }
    if (minor >= (uint32_t)g->max_minor) {
        snprintf(m, sizeof m, "PostgreSQL: NegotiateProtocolVersion offered 3.%u, which is not a downgrade from 3.%d", minor, g->max_minor);
        pg_fail_all(g, m);
        return;
    }
    if (minor == 1) {
        pg_fail_all(g, "PostgreSQL: the server offered protocol 3.1, which is not defined");
        return;
    }
    if (minor < (uint32_t)g->min_minor) {
        snprintf(m, sizeof m, "PostgreSQL: the server supports only protocol 3.%u and minProtocolVersion is 3.%d", minor, g->min_minor);
        pg_fail_all(g, m);
        return;
    }
    g->proto_minor = (int)minor;
}

static void pg_handle_auth(dyn_pg_t* g, const uint8_t* body, size_t len)
{
    uint32_t code;
    pgw_t w;

    g->auth_seen = 1;
    if (g->state == PG_ST_READY || g->auth_ok_seen) {
        pg_fail_all(g, "PostgreSQL: the server requested authentication after "
                       "the exchange was already complete");
        return;
    }
    if (len < 4) {
        pg_fail_all(g, "PostgreSQL: truncated authentication request");
        return;
    }
    code = dyn_pgw_u32(body);
    memset(&w, 0, sizeof(w));

    switch (code) {
    case PG_AUTH_OK:
        if (g->scram_live && !g->scram_done) {
            pg_fail_all(g, "PostgreSQL: the server skipped the SCRAM final "
                           "message, so it never proved it knows the password");
            return;
        }
        if (g->chan_bind == PG_CB_REQUIRE && !g->scram_plus) {
            pg_fail_all(g, "PostgreSQL: channelBinding is 'require' but the server authenticated without SCRAM-SHA-256-PLUS");
            return;
        }
        g->auth_ok_seen = 1;
        return;

    case PG_AUTH_CLEARTEXT:
    case PG_AUTH_MD5:
        if (g->chan_bind == PG_CB_REQUIRE) {
            pg_fail_all(g, "PostgreSQL: channelBinding is 'require'; password and MD5 authentication cannot bind the channel");
            return;
        }
        if (code == PG_AUTH_MD5)
            goto md5;
        if ((!g->insecure_auth && !g->use_tls) || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for a cleartext password; "
                           "refused unless the connection is TLS or "
                           "insecureAuth is set");
            return;
        }
        {
            size_t at = pgw_begin(&w, 'p');
            pgw_str(&w, g->pass);
            pgw_end(&w, at);
        }
        break;

    md5:
        if ((!g->insecure_auth && !g->use_tls) || !g->pass) {
            pg_fail_all(g, "PostgreSQL: the server asked for MD5 authentication; "
                           "this client requires SCRAM-SHA-256 unless the "
                           "connection is TLS or insecureAuth is set");
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
        int found = 0, found_plus = 0, mode;
        if (g->scram_live) {
            pg_fail_all(g, "PostgreSQL: the server restarted the SCRAM "
                           "exchange; one exchange per connection");
            return;
        }
        while (i < len && body[i]) {
            size_t s = i;
            while (i < len && body[i])
                i++;
            if (i - s == 13 && memcmp(body + s, "SCRAM-SHA-256", 13) == 0)
                found = 1;
            if (i - s == 18 && memcmp(body + s, "SCRAM-SHA-256-PLUS", 18) == 0)
                found_plus = 1;
            if (i < len)
                i++;
        }
        if (found_plus && !g->use_tls) {
            pg_fail_all(g, "PostgreSQL: the server offered SCRAM-SHA-256-PLUS over a connection without TLS");
            return;
        }
        if (g->chan_bind == PG_CB_REQUIRE && !found_plus) {
            pg_fail_all(g, g->use_tls ? "PostgreSQL: channelBinding is 'require' but the server does not offer SCRAM-SHA-256-PLUS" : "PostgreSQL: channelBinding is 'require' and needs tls: true");
            return;
        }
        if (g->chan_bind != PG_CB_DISABLE && found_plus && g->ep_len <= 0) {
            pg_fail_all(g, g->chan_bind == PG_CB_REQUIRE ? "PostgreSQL: channelBinding is 'require' but the server certificate's signature has no usable hash (RFC 5929)"
                                                         : "PostgreSQL: channelBinding is 'prefer', the server offers SCRAM-SHA-256-PLUS, but the server certificate's signature has no usable hash (RFC 5929); use channelBinding: 'disable'");
            return;
        }
        if (g->use_tls && g->chan_bind != PG_CB_DISABLE && found_plus && g->ep_len > 0)
            mode = DYN_SCRAM_CB_TLS_EP;
        else if (!found) {
            pg_fail_all(g, "PostgreSQL: the server offers no SCRAM-SHA-256 this connection can use; "
                           "this client implements no other SASL mechanism");
            return;
        } else
            mode = g->use_tls && g->chan_bind != PG_CB_DISABLE && !found_plus ? DYN_SCRAM_CB_CLIENT : DYN_SCRAM_CB_NONE;
        if (!g->pass) {
            pg_fail_all(g, "PostgreSQL: the server requires a password and none "
                           "was given");
            return;
        }
        if (dyn_scram_client_first_cb(&g->scram, mode, g->ep_hash, mode == DYN_SCRAM_CB_TLS_EP ? (size_t)g->ep_len : 0, cfirst, sizeof(cfirst)) < 0) {
            pg_fail_all(g, "PostgreSQL: cannot begin SCRAM");
            return;
        }
        g->scram_live = 1;
        g->scram_plus = mode == DYN_SCRAM_CB_TLS_EP;
        at = pgw_begin(&w, 'p');
        pgw_str(&w, g->scram_plus ? "SCRAM-SHA-256-PLUS" : "SCRAM-SHA-256");
        {
            size_t cf_len = strlen(cfirst);
            pgw_u32(&w, (uint32_t)cf_len);
            pgw_raw(&w, cfirst, cf_len);
        }
        pgw_end(&w, at);
        break;
    }

    case PG_AUTH_SASL_CONT:
        if (!g->scram_live || g->scram_job) {
            pg_fail_all(g, "PostgreSQL: SASL continue with no exchange in progress");
            return;
        }
        pg_scram_job_start(g, body + 4, len - 4);
        return;

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

static JSValue pg_bytea_to_hex_string(JSContext* ctx, const uint8_t* p, size_t n)
{
    char stackbuf[128], *s = stackbuf, *heap = NULL;
    JSValue out;
    size_t need = n * 2 + 2;
    if (need > sizeof(stackbuf)) {
        heap = (char*)malloc(need);
        if (!heap)
            return JS_ThrowOutOfMemory(ctx);
        s = heap;
    }
    dyn_pgw_hex(p, n, s);
    out = JS_NewStringLen(ctx, s, need);
    free(heap);
    return out;
}

static JSValue pg_bytea(JSContext* ctx, const uint8_t* p, size_t n)
{
    long nb = dyn_pgw_bytea_hex(p, n, NULL);
    JSValue ab, ta;
    JSValueConst a3[3];
    uint8_t* out;

    if (nb < 0)
        return JS_UNDEFINED;
    out = (uint8_t*)js_malloc(ctx, nb ? (size_t)nb : 1);
    if (!out)
        return JS_EXCEPTION;
    dyn_pgw_bytea_hex(p, n, out);
    ab = JS_NewArrayBufferCopy(ctx, out, (size_t)nb);
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

int pg_param_bytea(JSContext* ctx, JSValueConst v, const char** out,
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

static JSValue pg_numeric_value(JSContext* ctx, dyn_pg_t* g, const char* s, size_t n)
{
    JSValue str, v;
    if (!JS_IsFunction(ctx, g->numeric_ctor))
        return JS_NewStringLen(ctx, s, n);
    if ((n == 3 && !memcmp(s, "NaN", 3)) || (n == 8 && !memcmp(s, "Infinity", 8)) || (n == 9 && !memcmp(s, "-Infinity", 9)))
        return JS_NewFloat64(ctx, n == 3 ? (double)NAN : s[0] == '-' ? -(double)INFINITY : (double)INFINITY);
    str = JS_NewStringLen(ctx, s, n);
    if (JS_IsException(str))
        return str;
    v = JS_CallConstructor(ctx, g->numeric_ctor, 1, (JSValueConst*)&str);
    JS_FreeValue(ctx, str);
    return v;
}

static JSValue pg_time_value(JSContext* ctx, int kind, int64_t us, const uint8_t* raw, size_t rawn)
{
    double ms;
    char b[48];
    int k;
    ms = (double)(us >= 0 ? us / 1000 : -((-us + 999) / 1000)) + 946684800000.0;
    if (ms >= -8.64e15 && ms <= 8.64e15)
        return JS_NewDate(ctx, ms);
    if (raw)
        return JS_NewStringLen(ctx, (const char*)raw, rawn);
    k = dyn_pgw_ts_format(us, kind, b, sizeof b);
    return k > 0 ? JS_NewStringLen(ctx, b, (size_t)k) : JS_NULL;
}

static JSValue pg_date_value(JSContext* ctx, int64_t d)
{
    double ms = (double)d * 86400000.0 + 946684800000.0;
    int64_t y;
    unsigned m, dd;
    char b[48];
    int k;
    if (ms >= -8.64e15 && ms <= 8.64e15)
        return JS_NewDate(ctx, ms);
    dyn_pgw_civil_from_days(d + DYN_PGW_EPOCH_DAYS, &y, &m, &dd);
    k = snprintf(b, sizeof b, "%04lld-%02u-%02u%s", (long long)(y > 0 ? y : 1 - y), m, dd, y > 0 ? "" : " BC");
    return k > 0 ? JS_NewStringLen(ctx, b, (size_t)k) : JS_NULL;
}

static JSValue pg_interval_value(JSContext* ctx, int64_t us, int32_t days, int32_t months)
{
    JSValue o;
    if (months == INT32_MAX && days == INT32_MAX && us == INT64_MAX)
        return JS_NewFloat64(ctx, (double)INFINITY);
    if (months == INT32_MIN && days == INT32_MIN && us == INT64_MIN)
        return JS_NewFloat64(ctx, -(double)INFINITY);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "months", JS_NewInt32(ctx, months), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "days", JS_NewInt32(ctx, days), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "microseconds", us > 9007199254740991LL || us < -9007199254740991LL ? JS_NewBigInt64(ctx, us) : JS_NewInt64(ctx, us), JS_PROP_C_W_E);
    return o;
}

static JSValue pg_text_decoded(JSContext* ctx, dyn_pg_t* g, uint32_t oid, const uint8_t* p, size_t n)
{
    int64_t us;
    int32_t days, months;
    int r, kind;
    if (oid == PG_OID_NUMERIC)
        return pg_numeric_value(ctx, g, (const char*)p, n);
    if (oid == PG_OID_INTERVAL && ((n == 8 && !memcmp(p, "infinity", 8)) || (n == 9 && !memcmp(p, "-infinity", 9))))
        return JS_NewFloat64(ctx, n == 8 ? (double)INFINITY : -(double)INFINITY);
    if (oid == PG_OID_INTERVAL)
        return dyn_pgw_interval_text((const char*)p, n, &us, &days, &months) == 0 ? pg_interval_value(ctx, us, days, months) : JS_NewStringLen(ctx, (const char*)p, n);
    kind = oid == PG_OID_DATE ? 0 : oid == PG_OID_TIMESTAMP ? 1 : 2;
    r = dyn_pgw_ts_text((const char*)p, n, kind, &us);
    if (r == 1 || r == 2)
        return JS_NewFloat64(ctx, r == 1 ? (double)INFINITY : -(double)INFINITY);
    if (r < 0)
        return JS_NewStringLen(ctx, (const char*)p, n);
    return pg_time_value(ctx, kind, us, p, n);
}

static JSValue pg_column(JSContext* ctx, dyn_pg_t* g, uint32_t oid, const uint8_t* p, size_t n);
static JSValue pg_column_binary(JSContext* ctx, dyn_pg_t* g, uint32_t oid, const uint8_t* p, size_t n);

static JSValue pg_json_value(JSContext* ctx, const uint8_t* p, size_t n)
{
    char sb[512], *b = n < sizeof sb ? sb : (char*)malloc(n + 1);
    JSValue v;
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(b, p, n);
    b[n] = 0;
    v = JS_ParseJSON(ctx, b, n, "<postgres json>");
    if (b != sb)
        free(b);
    return v;
}

static JSValue pg_array_build(JSContext* ctx, dyn_pg_t* g, uint32_t elem, const int32_t* dims, int ndim, int lvl,
    const dyn_pgw_span_t* sp, const char* out, dyn_pgw_arrbin_t* h, size_t* at, size_t* k)
{
    JSValue a = JS_NewArray(ctx), v;
    int32_t i, cnt = lvl < ndim ? dims[lvl] : 0;
    if (JS_IsException(a))
        return a;
    for (i = 0; i < cnt; i++) {
        if (lvl + 1 < ndim)
            v = pg_array_build(ctx, g, elem, dims, ndim, lvl + 1, sp, out, h, at, k);
        else if (h) {
            const uint8_t* e = NULL;
            int32_t len = -1;
            v = dyn_pgw_array_bin_next(h, at, &e, &len) && len >= 0 ? pg_column_binary(ctx, g, elem, e, (size_t)len) : JS_NULL;
        } else {
            const dyn_pgw_span_t* s = &sp[(*k)++];
            v = s->len < 0 ? JS_NULL : pg_column(ctx, g, elem, (const uint8_t*)out + s->off, (size_t)s->len);
        }
        if (JS_IsException(v) || JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

static JSValue pg_array_text(JSContext* ctx, dyn_pg_t* g, uint32_t elem, const uint8_t* p, size_t n)
{
    dyn_pgw_span_t ss[64], *sp = ss;
    char ob[1024], *out = n <= sizeof ob ? ob : (char*)malloc(n);
    int32_t dims[DYN_PGW_MAXDIM];
    int nd = 0, r;
    size_t nel = 0, k = 0;
    JSValue v;
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    r = dyn_pgw_array_text((const char*)p, n, &nd, dims, sp, 64, out, &nel);
    if (r == -2) {
        sp = (dyn_pgw_span_t*)malloc(nel * sizeof(*sp));
        r = sp ? dyn_pgw_array_text((const char*)p, n, &nd, dims, sp, nel, out, &nel) : -3;
    }
    if (r == 0)
        v = pg_array_build(ctx, g, elem, dims, nd, 0, sp, out, NULL, NULL, &k);
    else if (r == -3)
        v = JS_ThrowOutOfMemory(ctx);
    else
        v = JS_NewStringLen(ctx, (const char*)p, n);
    if (sp && sp != ss)
        free(sp);
    if (out != ob)
        free(out);
    return v;
}

static JSValue pg_array_binary(JSContext* ctx, dyn_pg_t* g, uint32_t elem, const uint8_t* p, size_t n)
{
    dyn_pgw_arrbin_t h;
    size_t at = 0;
    if (dyn_pgw_array_bin(p, n, &h) < 0 || h.elem != elem)
        return dyn_nat_u8_copy(ctx, p, n);
    return pg_array_build(ctx, g, elem, h.dims, h.ndim, 0, NULL, NULL, &h, &at, NULL);
}

static JSValue pg_range_value(JSContext* ctx, dyn_pg_t* g, uint32_t sub, const dyn_pgw_range_t* r, int bin)
{
    JSValue o = JS_NewObject(ctx), lo = JS_NULL, hi = JS_NULL;
    int empty = (r->flags & DYN_PGW_RANGE_EMPTY) != 0;
    if (JS_IsException(o))
        return o;
    if (!empty && r->lo_len >= 0) {
        lo = bin ? pg_column_binary(ctx, g, sub, r->lo, (size_t)r->lo_len) : pg_column(ctx, g, sub, r->lo, (size_t)r->lo_len);
        if (JS_IsException(lo))
            goto fail;
    }
    if (!empty && r->hi_len >= 0) {
        hi = bin ? pg_column_binary(ctx, g, sub, r->hi, (size_t)r->hi_len) : pg_column(ctx, g, sub, r->hi, (size_t)r->hi_len);
        if (JS_IsException(hi)) {
            JS_FreeValue(ctx, lo);
            goto fail;
        }
    }
    JS_DefinePropertyValueStr(ctx, o, "empty", JS_NewBool(ctx, empty), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "lower", lo, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "upper", hi, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "lowerInclusive", JS_NewBool(ctx, (r->flags & DYN_PGW_RANGE_LB_INC) != 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "upperInclusive", JS_NewBool(ctx, (r->flags & DYN_PGW_RANGE_UB_INC) != 0), JS_PROP_C_W_E);
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue pg_range_any(JSContext* ctx, dyn_pg_t* g, uint32_t sub, int multi, int bin, const uint8_t* p, size_t n)
{
    dyn_pgw_range_t r;
    char ob[512], *out = NULL;
    JSValue v, a;
    size_t at = 0;
    uint32_t i = 0;
    int k;
    if (!bin) {
        out = n <= sizeof ob ? ob : (char*)malloc(n);
        if (!out)
            return JS_ThrowOutOfMemory(ctx);
    }
    if (!multi) {
        k = bin ? dyn_pgw_range_bin(p, n, &r) : dyn_pgw_range_text((const char*)p, n, &r, out);
        v = k == 0 ? pg_range_value(ctx, g, sub, &r, bin) : bin ? dyn_nat_u8_copy(ctx, p, n) : JS_NewStringLen(ctx, (const char*)p, n);
        goto done;
    }
    a = JS_NewArray(ctx);
    if (JS_IsException(a)) {
        v = a;
        goto done;
    }
    while ((k = bin ? dyn_pgw_multirange_bin(p, n, &at, &r) : dyn_pgw_multirange_text((const char*)p, n, &at, &r, out)) > 0) {
        JSValue e;
        if (r.flags & DYN_PGW_RANGE_EMPTY)
            continue;
        e = pg_range_value(ctx, g, sub, &r, bin);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, a, i++, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            v = JS_EXCEPTION;
            goto done;
        }
    }
    if (k < 0) {
        JS_FreeValue(ctx, a);
        v = bin ? dyn_nat_u8_copy(ctx, p, n) : JS_NewStringLen(ctx, (const char*)p, n);
    } else
        v = a;
done:
    if (out && out != ob)
        free(out);
    return v;
}

static uint32_t pg_range_sub_of(uint32_t oid, int* multi)
{
    uint32_t r = dyn_pgw_multirange_range(oid);
    *multi = r != 0;
    return dyn_pgw_range_sub(r ? r : oid);
}

static JSValue pg_column_binary(JSContext* ctx, dyn_pg_t* g, uint32_t oid,
    const uint8_t* p, size_t n)
{
    uint32_t ae = dyn_pgw_array_elem(oid), rs;
    int multi = 0;
    if (ae)
        return pg_array_binary(ctx, g, ae, p, n);
    rs = pg_range_sub_of(oid, &multi);
    if (rs)
        return pg_range_any(ctx, g, rs, multi, 1, p, n);
    switch (oid) {
    case PG_OID_NUMERIC: {
        char sb[256], *s = sb;
        size_t need = 0;
        int k = dyn_pgw_numeric_bin(p, n, sb, sizeof sb, &need);
        JSValue v;
        if (k == -2 && (s = (char*)malloc(need)) != NULL)
            k = dyn_pgw_numeric_bin(p, n, s, need, NULL);
        if (k == -2 && !s)
            return JS_ThrowOutOfMemory(ctx);
        v = k >= 0 ? pg_numeric_value(ctx, g, s, (size_t)k) : dyn_nat_u8_copy(ctx, p, n);
        if (s != sb)
            free(s);
        return v;
    }
    case PG_OID_DATE:
        if (n == 4) {
            int32_t d = (int32_t)dyn_pgw_u32(p);
            if (d == INT32_MAX || d == INT32_MIN)
                return JS_NewFloat64(ctx, d == INT32_MAX ? (double)INFINITY : -(double)INFINITY);
            return pg_date_value(ctx, d);
        }
        break;
    case PG_OID_TIMESTAMP:
    case PG_OID_TIMESTAMPTZ:
        if (n == 8) {
            int64_t v = (int64_t)(((uint64_t)dyn_pgw_u32(p) << 32) | dyn_pgw_u32(p + 4));
            if (v == INT64_MAX || v == INT64_MIN)
                return JS_NewFloat64(ctx, v == INT64_MAX ? (double)INFINITY : -(double)INFINITY);
            return pg_time_value(ctx, oid == PG_OID_TIMESTAMP ? 1 : 2, v, NULL, 0);
        }
        break;
    case PG_OID_INTERVAL:
        if (n == 16)
            return pg_interval_value(ctx, (int64_t)(((uint64_t)dyn_pgw_u32(p) << 32) | dyn_pgw_u32(p + 4)), (int32_t)dyn_pgw_u32(p + 8), (int32_t)dyn_pgw_u32(p + 12));
        break;
    case PG_OID_BOOL:
        if (n == 1)
            return JS_NewBool(ctx, p[0] != 0);
        break;
    case PG_OID_BYTEA:
        if (g->bytes_out)
            return dyn_nat_u8_copy(ctx, p, n);
        return pg_bytea_to_hex_string(ctx, p, n);
    case PG_OID_INT2:
        if (n == 2)
            return JS_NewInt32(ctx, (int16_t)((p[0] << 8) | p[1]));
        break;
    case PG_OID_INT4:
        if (n == 4)
            return JS_NewInt32(ctx, (int32_t)dyn_pgw_u32(p));
        break;
    case PG_OID_OID:
        if (n == 4)
            return JS_NewUint32(ctx, dyn_pgw_u32(p));
        break;
    case PG_OID_INT8:
        if (n == 8) {
            int64_t v = (int64_t)(((uint64_t)dyn_pgw_u32(p) << 32) | dyn_pgw_u32(p + 4));
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
            uint32_t u = dyn_pgw_u32(p);
            float f;
            memcpy(&f, &u, 4);
            return JS_NewFloat64(ctx, (double)f);
        }
        break;
    case PG_OID_FLOAT8:
        if (n == 8) {
            uint64_t u = ((uint64_t)dyn_pgw_u32(p) << 32) | dyn_pgw_u32(p + 4);
            double d;
            memcpy(&d, &u, 8);
            return JS_NewFloat64(ctx, d);
        }
        break;
    case PG_OID_UUID:
        if (n == 16) {
            char s[36];
            dyn_pgw_uuid(p, s);
            return JS_NewStringLen(ctx, s, 36);
        }
        break;
    default:
        break;
    }
    return dyn_nat_u8_copy(ctx, p, n);
}

static JSValue pg_column(JSContext* ctx, dyn_pg_t* g, uint32_t oid,
    const uint8_t* p, size_t n)
{
    if (g->raw)
        return JS_NewStringLen(ctx, (const char*)p, n);
    if (g->arrays_native && dyn_pgw_array_elem(oid))
        return pg_array_text(ctx, g, dyn_pgw_array_elem(oid), p, n);
    if (g->ranges_obj) {
        int multi = 0;
        uint32_t rs = pg_range_sub_of(oid, &multi);
        if (rs)
            return pg_range_any(ctx, g, rs, multi, 0, p, n);
    }
    switch (oid) {
    case PG_OID_JSON:
    case PG_OID_JSONB:
        if (g->json_parse)
            return pg_json_value(ctx, p, n);
        return JS_NewStringLen(ctx, (const char*)p, n);
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
    case PG_OID_NUMERIC:
        return pg_numeric_value(ctx, g, (const char*)p, n);
    case PG_OID_DATE:
    case PG_OID_TIMESTAMP:
    case PG_OID_TIMESTAMPTZ:
    case PG_OID_INTERVAL:
        if (g->dates_native)
            return pg_text_decoded(ctx, g, oid, p, n);
        return JS_NewStringLen(ctx, (const char*)p, n);
    case PG_OID_INT8: {
        int64_t v;
        if (!dyn_pgw_int8_text(p, n, &v))
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

static void pg_row_description(dyn_pg_t* g, dyn_pg_pending_t* p,
    const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    dyn_pgw_iter_t it;
    dyn_pgw_field_t fd;
    int nf = dyn_pgw_rowdesc_begin(&it, b, len), r;
    uint16_t i = 0;

    if (nf < 0) {
        pg_fail_all(g, "PostgreSQL: truncated RowDescription");
        return;
    }
    JS_FreeValue(ctx, p->fields);
    p->fields = JS_NewArray(ctx);
    pgp_free_fields(ctx, p);
    if (nf) {
        p->fatom = (JSAtom*)calloc(1, (size_t)nf * (sizeof(*p->fatom) + sizeof(*p->foid)) + (size_t)nf);
        if (!p->fatom) {
            pgp_free_fields(ctx, p);
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
        }
        p->foid = (uint32_t*)(p->fatom + nf);
        p->fformat = (uint8_t*)(p->foid + nf);
    }
    while ((r = dyn_pgw_rowdesc_next(&it, &fd)) > 0) {
        JSValue f = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, f, "name",
            JS_NewStringLen(ctx, (const char*)fd.name, fd.nlen), JS_PROP_C_W_E);
        p->fatom[i] = JS_NewAtomLen(ctx, (const char*)fd.name, fd.nlen);
        p->nfield = i + 1;
        JS_DefinePropertyValueStr(ctx, f, "tableOid",
            JS_NewUint32(ctx, fd.table_oid), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, f, "column",
            JS_NewUint32(ctx, fd.column), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, f, "typeOid",
            JS_NewUint32(ctx, fd.type_oid), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, f, "format",
            JS_NewUint32(ctx, fd.format), JS_PROP_C_W_E);
        p->foid[i] = fd.type_oid;
        p->fformat[i] = fd.format ? 1 : 0;
        JS_DefinePropertyValueUint32(ctx, p->fields, i, f, JS_PROP_C_W_E);
        i++;
    }
    if (r < 0) {
        pg_fail_all(g, "PostgreSQL: malformed RowDescription");
        return;
    }
    if (nf && nf <= PG_ROW_TEMPLATE_MAX_FIELDS)
        p->tpl = JS_NewObjectTemplate(ctx, p->fatom, (uint32_t)nf);
    pg_stmt_learn_oids(g, p->stmt_name, p->foid, p->nfield);
}

static JSValue pg_cell(dyn_pg_t* g, dyn_pg_pending_t* p, uint16_t i,
    const uint8_t* col, uint32_t clen)
{
    if (clen == DYN_PGW_NULL)
        return JS_NULL;
    if (i < p->nfield && p->fformat[i])
        return pg_column_binary(g->ctx, g, p->foid[i], col, clen);
    return pg_column(g->ctx, g, i < p->nfield ? p->foid[i] : 0, col, clen);
}

static void pg_data_row(dyn_pg_t* g, dyn_pg_pending_t* p,
    const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    dyn_pgw_iter_t it;
    const uint8_t* col;
    uint32_t clen;
    int nc = dyn_pgw_datarow_begin(&it, b, len), r, tpl;
    uint16_t i = 0;
    JSValue row;

    if (js_std_job_error_pending_rt(JS_GetRuntime(ctx)))
        return;
    if (nc < 0) {
        pg_fail_all(g, "PostgreSQL: truncated DataRow");
        return;
    }
    tpl = p->tpl && nc == (int)p->nfield;
    row = tpl ? JS_NewObjectFromTemplate(ctx, p->tpl) : JS_NewObject(ctx);
    if (JS_IsException(row)) {
        pg_fail_all(g, "PostgreSQL: out of memory");
        return;
    }
    while ((r = dyn_pgw_datarow_next(&it, &col, &clen)) > 0) {
        JSValue v = pg_cell(g, p, i, col, clen);
        if (JS_IsException(v) && JS_HasUncatchableException(ctx)) {
            dyn_exc_forward(ctx);
            JS_FreeValue(ctx, row);
            if (JS_IsUndefined(p->error))
                p->error = pg_conn_error(ctx, "PostgreSQL: interrupted while decoding a row");
            return;
        }
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            if (JS_IsUndefined(p->error))
                p->error = e;
            else
                JS_FreeValue(ctx, e);
            v = JS_NULL;
        }
        if (tpl)
            JS_SetTemplateCell(ctx, row, i, v);
        else if (i < p->nfield && p->fatom[i] != JS_ATOM_NULL)
            JS_DefinePropertyValue(ctx, row, p->fatom[i], v, JS_PROP_C_W_E);
        else
            JS_FreeValue(ctx, v);
        i++;
    }
    if (r < 0) {
        JS_FreeValue(ctx, row);
        pg_fail_all(g, r == DYN_PGW_E_PAST ? "PostgreSQL: DataRow column runs past the message" : "PostgreSQL: malformed DataRow");
        return;
    }
    p->result_bytes += (uint64_t)len + 64;
    if (g->max_result && p->result_bytes > g->max_result) {
        JS_FreeValue(ctx, row);
        pg_fail_all(g, "PostgreSQL: result exceeds maxResultBytes");
        return;
    }
    if (p->max_rows > 0 && p->rowcount >= p->max_rows) {
        JS_FreeValue(ctx, row);
        if (JS_IsUndefined(p->error))
            p->error = pg_conn_error(ctx,
                "PostgreSQL: result exceeds maxRows");
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
    pg_release_if_idle(g);
    if (!settled_last) {
        while ((p = g->head) != NULL && p->batch == b) {
            int qlast = p->sync_end;
            JSValue ae = JS_NewError(ctx);
            if (p->stmt_name[0]) {
                pg_stmt_evict_named(g, p->stmt_name);
                p->stmt_name[0] = '\0';
            }
            JS_DefinePropertyValueStr(ctx, ae, "message",
                JS_NewString(ctx,
                    "PostgreSQL: an earlier statement of the pipeline "
                    "failed; the server skipped this one"),
                JS_PROP_C_W_E);
            pgp_pop(&g->head, &g->tail);
            g->npending--;
            pg_settle_entry_err(g, p, ae);
            if (qlast)
                break;
        }
    }
}

static const uint8_t pg_q_show[] = "Q\0\0\0\x1f" "SHOW transaction_read_only";
static const uint8_t pg_q_rec[] = "Q\0\0\0\x2a" "SELECT pg_catalog.pg_is_in_recovery()";
static const uint8_t pg_q_bye[5] = { 'X', 0, 0, 0, 4 };

static void pg_param_note(dyn_pg_t* g, const char* k, const uint8_t* v, size_t n)
{
    size_t i;
    int sv = 0;
    if (!strcmp(k, "default_transaction_read_only"))
        g->ro_on = n == 2 && v[0] == 'o' && v[1] == 'n' ? 2 : 1;
    else if (!strcmp(k, "in_hot_standby"))
        g->hs_on = n == 2 && v[0] == 'o' && v[1] == 'n' ? 2 : 1;
    else if (!strcmp(k, "server_version")) {
        for (i = 0; i < n && i < 6 && v[i] >= '0' && v[i] <= '9'; i++)
            sv = sv * 10 + (v[i] - '0');
        g->sv_major = sv;
    }
}

static void pg_target_miss(dyn_pg_t* g, const char* why)
{
    if (g->fd >= 0)
        (void)dyn_aio_send(g->aio, g->fd, pg_q_bye, sizeof(pg_q_bye), 0, NULL, NULL);
    pg_try_next(g, why);
}

static int pg_chk_send(dyn_pg_t* g, const uint8_t* m, size_t n, uint8_t which)
{
    g->chk = which;
    g->chk_ok = g->chk_err = g->chk_vn = 0;
    g->chk_rows = 0;
    if (dyn_aio_send(g->aio, g->fd, m, n, 0, NULL, NULL) < 0) {
        pg_fail_all(g, "PostgreSQL: cannot write to the socket");
        return -1;
    }
    return 0;
}

static int pg_target_check(dyn_pg_t* g)
{
    int t = g->tsa == PG_TSA_PREFER && g->tsa_pass2 ? PG_TSA_ANY : g->tsa;
    if (t == PG_TSA_RW || t == PG_TSA_RO) {
        int ro;
        if (!g->ro_on || !g->hs_on) {
            (void)pg_chk_send(g, pg_q_show, sizeof(pg_q_show), 1);
            return 0;
        }
        ro = g->ro_on == 2 || g->hs_on == 2;
        if (t == PG_TSA_RW ? ro : !ro) {
            pg_target_miss(g, t == PG_TSA_RW ? "session is read-only" : "session is not read-only");
            return 0;
        }
    } else if (t != PG_TSA_ANY) {
        if (g->sv_major < 9)
            g->hs_on = 1;
        if (!g->hs_on) {
            (void)pg_chk_send(g, pg_q_rec, sizeof(pg_q_rec), 2);
            return 0;
        }
        if (t == PG_TSA_PRIMARY ? g->hs_on == 2 : g->hs_on == 1) {
            pg_target_miss(g, t == PG_TSA_PRIMARY ? "server is in hot standby mode" : "server is not in hot standby mode");
            return 0;
        }
    }
    return 1;
}

static void pg_check_msg(dyn_pg_t* g, char type, const uint8_t* b, size_t len)
{
    if (type == 'T') {
        g->chk_ok = len >= 2 && b[0] == 0 && b[1] == 1;
    } else if (type == 'D') {
        g->chk_rows++;
        if (len >= 6 && b[0] == 0 && b[1] == 1) {
            int32_t n = (int32_t)((uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | (uint32_t)b[5]);
            if (n > 0 && (size_t)n > len - 6)
                g->chk_ok = 0;
            else {
                g->chk_vn = (uint8_t)(n <= 0 ? 0 : n < 2 ? n : 2);
                if (g->chk_vn)
                    memcpy(g->chk_val, b + 6, g->chk_vn);
            }
        } else {
            g->chk_ok = 0;
        }
    } else if (type == 'E') {
        g->chk_err = 1;
    }
}

static int pg_check_done(dyn_pg_t* g)
{
    int which = g->chk, ok = g->chk_ok && !g->chk_err && g->chk_rows == 1;
    g->chk = 0;
    if (!ok) {
        pg_target_miss(g, which == 1 ? "\"SHOW transaction_read_only\" failed" : "\"SELECT pg_is_in_recovery()\" failed");
        return 0;
    }
    if (which == 1)
        g->ro_on = g->hs_on = g->chk_vn == 2 && g->chk_val[0] == 'o' && g->chk_val[1] == 'n' ? 2 : 1;
    else
        g->hs_on = g->chk_vn >= 1 && g->chk_val[0] == 't' ? 2 : 1;
    return pg_target_check(g);
}

static void pg_ready_for_query(dyn_pg_t* g, char status)
{
    dyn_pg_pending_t* p;

    g->tx_status = status;
    if (g->state != PG_ST_READY) {
        if (!g->auth_ok_seen) {
            pg_fail_all(g, "PostgreSQL: the server sent ReadyForQuery before "
                           "completing authentication");
            return;
        }
        if (g->chk ? !pg_check_done(g) : g->tsa != PG_TSA_ANY && !pg_target_check(g))
            return;
        g->state = PG_ST_READY;
        g->connect_deadline_ms = 0;
        free(g->errs);
        g->errs = NULL;
        g->errs_len = 0;
        if (pg_drain_wq(g) < 0) {
            pg_fail_all(g, "PostgreSQL: out of memory");
            return;
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
        pg_release_if_idle(g);
        p = g->head;
        if (p && !JS_IsUndefined(p->error))
            pg_sync_failed(g, p);
    }
}

static void pg_message(dyn_pg_t* g, char type, const uint8_t* b, size_t len)
{
    JSContext* ctx = g->ctx;
    dyn_pg_pending_t* p = g->head;

    if (g->copy_mode && (type == 'C' || type == 'E' || type == 'Z'))
        pg_copy_end(g, type);
    if (g->chk && (type == 'T' || type == 'D' || type == 'C' || type == 'E' || type == 'I')) {
        pg_check_msg(g, type, b, len);
        return;
    }
    switch (type) {
    case 'R':
        pg_handle_auth(g, b, len);
        return;
    case 'v':
        pg_handle_npv(g, b, len);
        return;
    case 'S':
        if (g->nparams >= PG_MAX_PARAMS) {
            pg_fail_all(g, "PostgreSQL: the server sent too many "
                           "ParameterStatus messages");
            return;
        }
        if (len > 1) {
            size_t i = 0;
            while (i < len && b[i])
                i++;
            if (i + 1 < len) {
                size_t j = i + 1, s = j;
                while (j < len && b[j])
                    j++;
                g->nparams++;
                JS_DefinePropertyValueStr(ctx, g->params,
                    (const char*)b,
                    JS_NewStringLen(ctx, (const char*)b + s, j - s),
                    JS_PROP_C_W_E);
                pg_param_note(g, (const char*)b, b + s, j - s);
            }
        }
        return;
    case 'K':
        if (dyn_pgw_backend_key(b, len, g->proto_minor, &g->backend_pid) != 0) {
            pg_fail_all(g, g->proto_minor < 2 ? "PostgreSQL: BackendKeyData secret key must be 4 bytes in protocol 3.0"
                                              : "PostgreSQL: BackendKeyData secret key must be 4..256 bytes in protocol 3.2");
            return;
        }
        memcpy(g->cancel_key, b + 4, len - 4);
        g->cancel_key_len = len - 4;
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
                const char* m = JS_IsException(msg) ? NULL : JS_ToCString(ctx, msg);
                char buf[256];
                if (!m)
                    dyn_exc_drop(ctx);
                snprintf(buf, sizeof(buf), "PostgreSQL: %s", m ? m : "connection rejected");
                if (m)
                    JS_FreeCString(ctx, m);
                JS_FreeValue(ctx, msg);
                JS_FreeValue(ctx, e);
                if (pg_err_is(b, len, "57P03"))
                    pg_try_next(g, buf + 12);
                else
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
            JS_DefinePropertyValueStr(ctx, o, "pid",
                JS_NewUint32(ctx, dyn_pgw_u32(b)), JS_PROP_C_W_E);
            s = i;
            while (i < len && b[i])
                i++;
            JS_DefinePropertyValueStr(ctx, o, "channel",
                JS_NewStringLen(ctx, (const char*)b + s, i - s), JS_PROP_C_W_E);
            if (i < len)
                i++;
            s = i;
            while (i < len && b[i])
                i++;
            JS_DefinePropertyValueStr(ctx, o, "payload",
                JS_NewStringLen(ctx, (const char*)b + s, i - s), JS_PROP_C_W_E);
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
        pg_copy_message(g, type, b, len);
        return;
    default:
        pg_fail_all(g, "PostgreSQL: unknown message type from the server");
        return;
    }
}

#ifdef CONFIG_TLS
static void pg_tls_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

int pg_start_tls(dyn_pg_t* g)
{
    dyn_tls_opts_t to;
    char terr[192];
    int via_path = (!g->host || !g->host[0]) && g->path;
    dyn_tls_conn_t* t;

    memset(&to, 0, sizeof to);
    to.ca_file = g->tls_ca;
    to.alpn = "postgresql";
    to.insecure = via_path ? 1 : 0;
    if (!g->tls_ctx)
        g->tls_ctx = dyn_tls_ctx_client(&to, terr, sizeof terr);
    if (!g->tls_ctx) {
        pg_fail_all(g, terr);
        return -1;
    }
    t = dyn_tls_conn_new(g->tls_ctx, via_path ? NULL : g->host, terr,
        sizeof terr);
    g->tls_conn = t;
    if (!t) {
        pg_fail_all(g, terr);
        return -1;
    }
    if (dyn_aio_tls_attach(g->aio, g->fd, t, pg_tls_done, g) < 0
        || dyn_aio_tls_start(g->aio, g->fd) < 0) {
        pg_fail_all(g, "PostgreSQL: TLS handshake failed to start");
        return -1;
    }
    return 0;
}
#endif

static void pg_on_recv_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
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
#ifdef CONFIG_TLS
    if (g->ssl_wait) {
        g->ssl_wait = 0;
        if (len != 1 || buf[0] != 'S') {
            pg_fail_all(g, len == 1 && buf[0] == 'N' ? "PostgreSQL: the server does not accept TLS (it answered N to SSLRequest)" : "PostgreSQL: unexpected bytes in answer to SSLRequest; refusing to start TLS");
            pg_gone(g);
            return;
        }
        if (pg_start_tls(g) < 0)
            pg_gone(g);
        return;
    }
#endif
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
    pg_parse_buffered(g);
}

void pg_parse_buffered(dyn_pg_t* g)
{
    if (g->in_parse)
        return;
    g->in_parse = 1;
    while (!g->rx_paused) {
        const uint8_t* body;
        size_t blen;
        char type;
        unsigned gen0;
        long used = dyn_pgw_frame(g->rbuf + g->rpos, g->rlen - g->rpos, g->maxmsg, &type, &body, &blen);
        if (used == 0)
            break;
        if (used < 0) {
            g->in_parse = 0;
            pg_fail_all(g, "PostgreSQL: message length out of range");
            return;
        }
        gen0 = g->gen;
        pg_message(g, type, body, blen);
        if (pg_gone(g) || g->gen != gen0) {
            g->in_parse = 0;
            return;
        }
        g->rpos += (size_t)used;
        if (g->copy_gate) {
            pg_copy_gate_check(g);
            if (pg_gone(g)) {
                g->in_parse = 0;
                return;
            }
        }
    }
    if (g->rpos) {
        memmove(g->rbuf, g->rbuf + g->rpos, g->rlen - g->rpos);
        g->rlen -= g->rpos;
        g->rpos = 0;
    }
    g->in_parse = 0;
    if (!g->rx_paused && g->rx_off) {
        g->rx_off = 0;
        if (dyn_aio_recv(g->aio, g->fd, 0, 1, pg_on_recv, g) < 0)
            pg_fail_all(g, "PostgreSQL: cannot read from the socket");
    }
}

void pg_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;

    g->cb_depth++;
    pg_on_recv_inner(aio, res, buf, len, ud);
    pg_cb_exit(g);
}

#ifdef CONFIG_TLS
static void pg_tls_done_inner(dyn_aio_t* aio, int res, const uint8_t* buf,
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
    {
        char ab[64];
        const char* sel = dyn_tls_alpn_selected(g->tls_conn, ab, sizeof ab);
        int has = sel && *sel;
        if (has ? strcmp(sel, "postgresql") != 0 : g->ssl_direct) {
            g->tls_conn = NULL;
            pg_fail_all(g, has ? "PostgreSQL: the server selected an ALPN protocol other than 'postgresql'"
                               : "PostgreSQL: direct TLS needs the server to select ALPN 'postgresql' (PostgreSQL 17+)");
            pg_gone(g);
            return;
        }
    }
    g->ep_len = dyn_tls_end_point_hash(g->tls_conn, g->ep_hash, sizeof g->ep_hash);
    g->tls_conn = NULL;
    if (pg_send_startup(g) < 0 || pg_flush(g) < 0)
        pg_fail_all(g, "PostgreSQL: cannot send the startup message");
}

static void pg_tls_done(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_pg_t* g = (dyn_pg_t*)ud;

    g->cb_depth++;
    pg_tls_done_inner(aio, res, buf, len, ud);
    pg_cb_exit(g);
}
#endif

void pg_param_arrays_free(const char** pv, size_t* plen,
    uint8_t* pnull, uint8_t* pfree)
{
    (void)plen;
    (void)pnull;
    (void)pfree;
    free((void*)pv);
}

JSValue dyn_pg_get_proto(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (g->state != PG_ST_READY)
        return JS_NULL;
    return JS_NewString(ctx, g->proto_minor >= 2 ? "3.2" : "3.0");
}

JSValue dyn_pg_get_ready(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    return g ? JS_NewBool(ctx, g->state == PG_ST_READY) : JS_EXCEPTION;
}
#endif
