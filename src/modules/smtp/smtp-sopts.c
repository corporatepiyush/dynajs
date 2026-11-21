#include "smtp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)
#include <string.h>

static const char* const ssrv_keys[] = {
    "host", "port", "name", "tls", "cert", "key", "requireTls", "requireAuth", "insecureAuth", "mechanisms", "disable",
    "maxMessageSize", "maxRecipients", "maxErrors", "maxMessagesPerSession", "maxAuthFailures", "maxConnections",
    "commandTimeoutMs", "dataTimeoutMs", "chunkSize", "onHelo", "onAuth", "onMail", "onRcpt", "onChunk", "onData",
};
static const char* const hk_names[HK_N] = { "onHelo", "onAuth", "onMail", "onRcpt", "onChunk", "onData" };
const int ssrv_hk_pend[HK_N] = { MAILK_PEND_HELO, MAILK_PEND_AUTH, MAILK_PEND_MAIL, MAILK_PEND_RCPT, 0, MAILK_PEND_DATA };
static const struct {
    const char* n;
    unsigned cap;
} exts[] = {
    { "PIPELINING", MAILK_CAP_PIPELINING }, { "8BITMIME", MAILK_CAP_8BITMIME }, { "SMTPUTF8", MAILK_CAP_SMTPUTF8 },
    { "DSN", MAILK_CAP_DSN }, { "CHUNKING", MAILK_CAP_CHUNKING }, { "BINARYMIME", MAILK_CAP_BINARYMIME },
    { "SIZE", MAILK_CAP_SIZE }, { "ENHANCEDSTATUSCODES", MAILK_CAP_ENHANCED },
};

static unsigned opt_disable(JSContext* ctx, JSValueConst o, int* bad)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "disable");
    unsigned m = 0;
    uint32_t n = 0, i;
    size_t k;
    if (JS_IsException(v)) {
        *bad = 1;
        return 0;
    }
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SmtpServer: disable must be an array of extension names");
        *bad = 1;
        return 0;
    }
    if (dyn_js_len_u32(ctx, v, &n)) {
        JS_FreeValue(ctx, v);
        *bad = 1;
        return 0;
    }
    for (i = 0; i < n && !*bad; i++) {
        JSValue x = JS_GetPropertyUint32(ctx, v, i);
        const char* s = JS_ToCString(ctx, x);
        unsigned hit = 0;
        JS_FreeValue(ctx, x);
        if (!s) {
            *bad = 1;
            break;
        }
        for (k = 0; k < countof(exts); k++)
            if (!strcmp(s, exts[k].n))
                hit = exts[k].cap;
        if (!hit) {
            JS_ThrowRangeError(ctx, "SmtpServer: unknown extension '%s' in disable", s);
            *bad = 1;
        }
        m |= hit;
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return m;
}

int ssrv_opts(JSContext* ctx, ssrv_t* sv, JSValueConst o)
{
    int bad = 0, i;
    char* tls;
    mailk_srv_t probe;
    JSValue b;
    if (dyn_opts_strict(ctx, o, ssrv_keys, (int)countof(ssrv_keys)))
        return -1;
    sv->host = smtp_opt_str(ctx, o, "host", &bad);
    sv->name = smtp_opt_str(ctx, o, "name", &bad);
    sv->cert = smtp_opt_str(ctx, o, "cert", &bad);
    sv->key = smtp_opt_str(ctx, o, "key", &bad);
    tls = smtp_opt_str(ctx, o, "tls", &bad);
    sv->tls = sv->cert ? MAILK_TLS_STARTTLS : MAILK_TLS_NONE;
    if (!bad && tls)
        sv->tls = !strcmp(tls, "none") ? MAILK_TLS_NONE : !strcmp(tls, "starttls") ? MAILK_TLS_STARTTLS : !strcmp(tls, "implicit") ? MAILK_TLS_IMPLICIT : -1;
    free(tls);
    if (!bad && sv->tls < 0) {
        JS_ThrowRangeError(ctx, "SmtpServer: tls must be 'none', 'starttls' or 'implicit'");
        return -1;
    }
    if (!bad && sv->tls != MAILK_TLS_NONE && (!sv->cert || !sv->key)) {
        JS_ThrowTypeError(ctx, "SmtpServer: tls needs cert and key (PEM file paths)");
        return -1;
    }
    sv->eo.mechs = bad ? 0 : smtp_opt_mechs(ctx, o, &bad);
    if (!bad && (sv->eo.mechs & ~(unsigned)MAILK_SRV_MECHS)) {
        JS_ThrowRangeError(ctx, "SmtpServer: CRAM-MD5 is not offered by the server");
        return -1;
    }
    sv->eo.caps = (MAILK_CAP_PIPELINING | MAILK_CAP_8BITMIME | MAILK_CAP_SMTPUTF8 | MAILK_CAP_DSN | MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME | MAILK_CAP_SIZE | MAILK_CAP_ENHANCED | MAILK_CAP_AUTH) & ~(bad ? 0u : opt_disable(ctx, o, &bad));
    if (sv->tls == MAILK_TLS_STARTTLS)
        sv->eo.caps |= MAILK_CAP_STARTTLS;
    sv->eo.tls_on = sv->tls == MAILK_TLS_IMPLICIT;
    sv->port = (uint16_t)smtp_opt_num(ctx, o, "port", sv->tls == MAILK_TLS_IMPLICIT ? 465 : 25, 0, 65535, &bad);
    sv->eo.size_max = (uint64_t)smtp_opt_num(ctx, o, "maxMessageSize", 10485760, 1, (int64_t)1 << 40, &bad);
    sv->eo.rcpt_max = (uint32_t)smtp_opt_num(ctx, o, "maxRecipients", 100, 1, 100000, &bad);
    sv->eo.errors_max = (uint32_t)smtp_opt_num(ctx, o, "maxErrors", 10, 1, 1000, &bad);
    sv->eo.msgs_max = (uint32_t)smtp_opt_num(ctx, o, "maxMessagesPerSession", 100, 1, 1000000, &bad);
    sv->eo.auth_max = (uint32_t)smtp_opt_num(ctx, o, "maxAuthFailures", 3, 1, 100, &bad);
    sv->max_conns = (int)smtp_opt_num(ctx, o, "maxConnections", 100, 1, 100000, &bad);
    sv->eo.cmd_ms = (uint32_t)smtp_opt_num(ctx, o, "commandTimeoutMs", 300000, 1, 3600000, &bad);
    sv->eo.data_ms = (uint32_t)smtp_opt_num(ctx, o, "dataTimeoutMs", 300000, 1, 3600000, &bad);
    sv->eo.body_hwm = (uint32_t)smtp_opt_num(ctx, o, "chunkSize", 65536, 1024, 16777216, &bad);
    for (i = 0; i < 3 && !bad; i++) {
        static const char* const flags[] = { "requireTls", "requireAuth", "insecureAuth" };
        int fb = dyn_get_truthy(ctx, o, flags[i]);
        if (fb < 0) {
            bad = 1;
            break;
        }
        *(i == 0 ? &sv->eo.require_tls : i == 1 ? &sv->eo.require_auth : &sv->eo.insecure_auth) = fb;
    }
    for (i = 0; i < HK_N && !bad; i++) {
        b = JS_GetPropertyStr(ctx, o, hk_names[i]);
        if (JS_IsException(b))
            return -1;
        if (!JS_IsUndefined(b) && !JS_IsFunction(ctx, b)) {
            JS_FreeValue(ctx, b);
            JS_ThrowTypeError(ctx, "SmtpServer: %s must be a function", hk_names[i]);
            return -1;
        }
        sv->hooks[i] = b;
        if (!JS_IsUndefined(b))
            sv->eo.hooks |= MAILK_HOOK(i == HK_CHUNK ? MAILK_PEND_DATA : ssrv_hk_pend[i]);
    }
    if (bad)
        return -1;
    if (JS_IsUndefined(sv->hooks[HK_CHUNK]))
        sv->eo.body_hwm = 0;
    if (!sv->name && !(sv->name = strdup("localhost")))
        return JS_ThrowOutOfMemory(ctx), -1;
    sv->eo.name = sv->name;
    i = mailk_srv_init(&probe, &sv->eo);
    mailk_srv_free(&probe);
    if (i != MAILK_OK) {
        JS_ThrowTypeError(ctx, "SmtpServer: name must be a domain; requireAuth needs onAuth; requireTls needs tls");
        return -1;
    }
    return 0;
}
#endif
