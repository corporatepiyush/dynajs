#include "dyna-tls.h"

#ifdef CONFIG_TLS

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#ifndef DYNA_CA_BUNDLE
#define DYNA_CA_BUNDLE "tests/corpus/ca-bundle.pem"
#endif

#define TLS_SESS_SLOTS 8

typedef struct {
    dyn_tls_ctx_t* owner;
    char* name;
    SSL_SESSION* s;
    unsigned long used;
} tls_sess_slot_t;

static struct {
    pthread_mutex_t lock;
    tls_sess_slot_t slot[TLS_SESS_SLOTS];
    unsigned long tick;
} tls_sess_cache = { PTHREAD_MUTEX_INITIALIZER, { { 0 } }, 0 };

static int tls_streq_opt(const char* a, const char* b)
{
    return (a == b) || (a && b && strcmp(a, b) == 0);
}

static void tls_sess_store(dyn_tls_ctx_t* c, const char* name, SSL_SESSION* s)
{
    int i, victim;
    char* copy;

    if (!s)
        return;
    if (!name || !*name || !SSL_SESSION_is_resumable(s)) {
        SSL_SESSION_free(s);
        return;
    }
    copy = strdup(name);
    if (!copy) {
        SSL_SESSION_free(s);
        return;
    }
    pthread_mutex_lock(&tls_sess_cache.lock);
    victim = -1;
    for (i = 0; i < TLS_SESS_SLOTS; i++) {
        if (!tls_sess_cache.slot[i].s && victim < 0)
            victim = i;
        if (tls_sess_cache.slot[i].s
            && tls_sess_cache.slot[i].owner == c
            && tls_streq_opt(tls_sess_cache.slot[i].name, name)) {
            free(tls_sess_cache.slot[i].name);
            SSL_SESSION_free(tls_sess_cache.slot[i].s);
            tls_sess_cache.slot[i].s = NULL;
            tls_sess_cache.slot[i].name = NULL;
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        victim = 0;
        for (i = 1; i < TLS_SESS_SLOTS; i++)
            if (tls_sess_cache.slot[i].used < tls_sess_cache.slot[victim].used)
                victim = i;
        free(tls_sess_cache.slot[victim].name);
        SSL_SESSION_free(tls_sess_cache.slot[victim].s);
        tls_sess_cache.slot[victim].s = NULL;
        tls_sess_cache.slot[victim].name = NULL;
    }
    tls_sess_cache.slot[victim].owner = c;
    tls_sess_cache.slot[victim].name = copy;
    tls_sess_cache.slot[victim].s = s;
    tls_sess_cache.slot[victim].used = ++tls_sess_cache.tick;
    pthread_mutex_unlock(&tls_sess_cache.lock);
}

static SSL_SESSION* tls_sess_lookup(dyn_tls_ctx_t* c, const char* name)
{
    SSL_SESSION* s = NULL;
    int i;

    if (!name || !*name)
        return NULL;
    pthread_mutex_lock(&tls_sess_cache.lock);
    for (i = 0; i < TLS_SESS_SLOTS; i++) {
        if (tls_sess_cache.slot[i].s
            && tls_sess_cache.slot[i].owner == c
            && tls_streq_opt(tls_sess_cache.slot[i].name, name)) {
            s = tls_sess_cache.slot[i].s;
            SSL_SESSION_up_ref(s);
            tls_sess_cache.slot[i].used = ++tls_sess_cache.tick;
            break;
        }
    }
    pthread_mutex_unlock(&tls_sess_cache.lock);
    return s;
}

struct dyn_tls_ctx {
    SSL_CTX* ctx;
    char* alpn_wire;
    size_t alpn_len;
    int insecure;
    int refs;
};

#define TLS_CTX_CACHE_MAX 8

typedef struct {
    char *ca_file, *ca_dir, *alpn;
    char *cert, *key;
    int insecure, min_version;
    dyn_tls_ctx_t* ctx;
    unsigned long used;
} tls_cache_slot_t;

static struct {
    pthread_mutex_t lock;
    tls_cache_slot_t slot[TLS_CTX_CACHE_MAX];
    int n;
    unsigned long tick;
} tls_ctx_cache = { PTHREAD_MUTEX_INITIALIZER, { { 0 } }, 0, 0 };

static int tls_key_eq(const tls_cache_slot_t* s, const dyn_tls_opts_t* o)
{
    return s->insecure == o->insecure
        && s->min_version == o->min_version
        && tls_streq_opt(s->ca_file, o->ca_file)
        && tls_streq_opt(s->ca_dir, o->ca_dir)
        && tls_streq_opt(s->alpn, o->alpn)
        && tls_streq_opt(s->cert, o->cert)
        && tls_streq_opt(s->key, o->key);
}

struct dyn_tls_conn {
    SSL* ssl;
    BIO* rbio;
    BIO* wbio;
    int handshake_done;
    int fatal;
    char err[192];
    dyn_tls_ctx_t* owner;
    char* sess_name;
};

const char* dyn_tls_runtime_version(void)
{
    return OpenSSL_version(OPENSSL_VERSION);
}

const char* dyn_tls_backend(void) { return "openssl"; }

static void tls_err(char* dst, size_t cap, const char* what)
{
    unsigned long e = ERR_get_error();
    char buf[128];
    size_t off;
    int k;

    if (!e) {
        snprintf(dst, cap, "%s", what);
        ERR_clear_error();
        return;
    }
    off = (size_t)snprintf(dst, cap, "%s: ", what);
    for (k = 0; e; k++) {
        if (k == 3)
            break;
        ERR_error_string_n(e, buf, sizeof buf);
        if (off < cap)
            off += (size_t)snprintf(dst + off, cap - off, "%s%s",
                k ? "; " : "", buf);
        e = ERR_get_error();
    }
    ERR_clear_error();
}

static int alpn_encode(const char* csv, char** out, size_t* outlen)
{
    size_t n = strlen(csv), i = 0, w = 0;
    char* buf;
    *out = NULL;
    *outlen = 0;
    if (n == 0)
        return 0;
    buf = (char*)malloc(n + 2);
    if (!buf)
        return -1;
    while (i <= n) {
        size_t start = i;
        while (i < n && csv[i] != ',')
            i++;
        if (i == start || i - start > 255) {
            free(buf);
            return -1;
        }
        buf[w++] = (char)(i - start);
        memcpy(buf + w, csv + start, i - start);
        w += i - start;
        if (i >= n)
            break;
        i++;
    }
    *out = buf;
    *outlen = w;
    return 0;
}

static int tls_alpn_select(SSL* ssl, const unsigned char** out,
    unsigned char* outlen, const unsigned char* in,
    unsigned int inlen, void* arg)
{
    const dyn_tls_ctx_t* c = (const dyn_tls_ctx_t*)arg;
    const unsigned char *p = in, *end = in + inlen;
    (void)ssl;

    if (!c->alpn_wire || c->alpn_len == 0)
        return SSL_TLSEXT_ERR_NOACK;
    while (p < end) {
        unsigned char l = *p;
        size_t w = 0;
        if (p + 1 + l > end)
            break;
        while (w + 1 <= c->alpn_len) {
            unsigned char ml = (unsigned char)c->alpn_wire[w];
            if (w + 1u + ml > c->alpn_len)
                break;
            if (ml == l && memcmp(c->alpn_wire + w + 1, p + 1, l) == 0) {
                *out = p + 1;
                *outlen = l;
                return SSL_TLSEXT_ERR_OK;
            }
            w += 1u + ml;
        }
        p += 1u + l;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

#if defined(__APPLE__)
typedef long CFIndex;
typedef struct __CFArray* CFArrayRef;
typedef struct __CFData* CFDataRef;
typedef struct OpaqueSecCertificateRef* SecCertificateRef;

static int tls_add_macos_anchors(X509_STORE* store)
{
    void* sec;
    CFArrayRef anchors = NULL;
    CFIndex i, n;
    int (*copy_anchors)(CFArrayRef*);
    CFDataRef (*cert_data)(SecCertificateRef);
    long (*cf_count)(CFArrayRef);
    const void* (*cf_get)(CFArrayRef, CFIndex);
    const unsigned char* (*cf_ptr)(CFDataRef);
    long (*cf_len)(CFDataRef);
    void (*cf_rel)(const void*);
    int added = 0;

    sec = dlopen("/System/Library/Frameworks/Security.framework/Security",
        RTLD_LAZY);
    if (!sec)
        return -1;
    copy_anchors = (int (*)(CFArrayRef*))dlsym(sec,
        "SecTrustCopyAnchorCertificates");
    cert_data = (CFDataRef (*)(SecCertificateRef))dlsym(sec,
        "SecCertificateCopyData");
    cf_count = (long (*)(CFArrayRef))dlsym(RTLD_DEFAULT, "CFArrayGetCount");
    cf_get = (const void* (*)(CFArrayRef, CFIndex))dlsym(RTLD_DEFAULT,
        "CFArrayGetValueAtIndex");
    cf_ptr = (const unsigned char* (*)(CFDataRef))dlsym(RTLD_DEFAULT,
        "CFDataGetBytePtr");
    cf_len = (long (*)(CFDataRef))dlsym(RTLD_DEFAULT, "CFDataGetLength");
    cf_rel = (void (*)(const void*))dlsym(RTLD_DEFAULT, "CFRelease");
    if (!copy_anchors || !cert_data || !cf_count || !cf_get || !cf_ptr
        || !cf_len || !cf_rel || copy_anchors(&anchors) != 0 || !anchors)
        return -1;
    n = cf_count(anchors);
    for (i = 0; i < n; i++) {
        SecCertificateRef c = (SecCertificateRef)(void*)(uintptr_t)cf_get(anchors, i);
        CFDataRef d = cert_data(c);
        const unsigned char* p;
        X509* x;
        if (!d)
            continue;
        p = cf_ptr(d);
        x = d2i_X509(NULL, &p, (long)cf_len(d));
        if (x) {
            X509_STORE_add_cert(store, x);
            X509_free(x);
            added++;
        }
        cf_rel(d);
    }
    cf_rel(anchors);
    dlclose(sec);
    return added > 0 ? 0 : -1;
}
#else
static int tls_add_macos_anchors(X509_STORE* store)
{
    (void)store;
    return -1;
}
#endif

static int tls_load_default_roots(SSL_CTX* ctx, char* err, size_t errlen)
{
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    int have = 0;

    if (tls_add_macos_anchors(store) == 0)
        have = 1;
    if (SSL_CTX_set_default_verify_paths(ctx))
        have = 1;
    if (SSL_CTX_load_verify_locations(ctx, DYNA_CA_BUNDLE, NULL))
        have = 1;
    else
        ERR_clear_error();
    if (!have) {
        snprintf(err, errlen, "no trust store: platform roots and vendored "
                              "Mozilla bundle both unavailable");
        return -1;
    }
    return 0;
}

dyn_tls_ctx_t* dyn_tls_ctx_client(const dyn_tls_opts_t* o,
    char* err, size_t errlen)
{
    dyn_tls_ctx_t* c;
    dyn_tls_ctx_t* dead = NULL;
    long opts;
    int i;

    pthread_mutex_lock(&tls_ctx_cache.lock);
    for (i = 0; i < tls_ctx_cache.n; i++) {
        if (tls_key_eq(&tls_ctx_cache.slot[i], o)) {
            c = tls_ctx_cache.slot[i].ctx;
            c->refs++;
            tls_ctx_cache.slot[i].used = ++tls_ctx_cache.tick;
            pthread_mutex_unlock(&tls_ctx_cache.lock);
            return c;
        }
    }
    pthread_mutex_unlock(&tls_ctx_cache.lock);

    c = (dyn_tls_ctx_t*)calloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) {
        tls_err(err, errlen, "SSL_CTX_new");
        free(c);
        return NULL;
    }

    if (!SSL_CTX_set_min_proto_version(c->ctx,
            o->min_version >= 13 ? TLS1_3_VERSION : TLS1_2_VERSION)) {
        tls_err(err, errlen, "set_min_proto_version");
        goto fail;
    }
    opts = SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
    opts |= SSL_OP_NO_RENEGOTIATION;
#endif
    SSL_CTX_set_options(c->ctx, opts);

    if (!SSL_CTX_set_cipher_list(c->ctx,
            "ECDHE+AESGCM:ECDHE+CHACHA20:!aNULL:!eNULL:!MD5:!RC4:!3DES")) {
        tls_err(err, errlen, "set_cipher_list");
        goto fail;
    }

    c->insecure = o->insecure;
    if (o->insecure) {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_verify_depth(c->ctx, 8);
        if (o->ca_file || o->ca_dir) {
            if (!SSL_CTX_load_verify_locations(c->ctx, o->ca_file, o->ca_dir)) {
                tls_err(err, errlen, "load_verify_locations");
                goto fail;
            }
        } else if (tls_load_default_roots(c->ctx, err, errlen)) {
            goto fail;
        }
    }

    if (o->cert || o->key) {
        if (!o->cert || !o->key) {
            snprintf(err, errlen, "tls: `cert` and `key` must be given together");
            goto fail;
        }
        if (SSL_CTX_use_certificate_chain_file(c->ctx, o->cert) != 1) {
            tls_err(err, errlen, "cannot read the client certificate");
            goto fail;
        }
        if (SSL_CTX_use_PrivateKey_file(c->ctx, o->key, SSL_FILETYPE_PEM) != 1) {
            tls_err(err, errlen, "cannot read the client key");
            goto fail;
        }
        if (SSL_CTX_check_private_key(c->ctx) != 1) {
            tls_err(err, errlen, "the client key does not match the certificate");
            goto fail;
        }
    }

    if (o->alpn && *o->alpn) {
        if (alpn_encode(o->alpn, &c->alpn_wire, &c->alpn_len) != 0) {
            snprintf(err, errlen, "bad alpn list");
            goto fail;
        }
        if (SSL_CTX_set_alpn_protos(c->ctx,
                (const unsigned char*)c->alpn_wire, (unsigned)c->alpn_len)) {
            tls_err(err, errlen, "set_alpn_protos");
            goto fail;
        }
    }

    c->refs = 1;
    pthread_mutex_lock(&tls_ctx_cache.lock);
    for (i = 0; i < tls_ctx_cache.n; i++) {
        if (tls_key_eq(&tls_ctx_cache.slot[i], o)) {
            dyn_tls_ctx_t* winner = tls_ctx_cache.slot[i].ctx;
            winner->refs++;
            tls_ctx_cache.slot[i].used = ++tls_ctx_cache.tick;
            pthread_mutex_unlock(&tls_ctx_cache.lock);
            dyn_tls_ctx_free(c);
            return winner;
        }
    }
    if (tls_ctx_cache.n < TLS_CTX_CACHE_MAX) {
        i = tls_ctx_cache.n++;
    } else {
        int ev = 0, k;
        for (k = 1; k < tls_ctx_cache.n; k++)
            if (tls_ctx_cache.slot[k].used < tls_ctx_cache.slot[ev].used)
                ev = k;
        dead = tls_ctx_cache.slot[ev].ctx;
        free(tls_ctx_cache.slot[ev].ca_file);
        free(tls_ctx_cache.slot[ev].ca_dir);
        free(tls_ctx_cache.slot[ev].alpn);
        free(tls_ctx_cache.slot[ev].cert);
        free(tls_ctx_cache.slot[ev].key);
        i = ev;
    }
    tls_ctx_cache.slot[i].ca_file = o->ca_file ? strdup(o->ca_file) : NULL;
    tls_ctx_cache.slot[i].ca_dir = o->ca_dir ? strdup(o->ca_dir) : NULL;
    tls_ctx_cache.slot[i].alpn = o->alpn ? strdup(o->alpn) : NULL;
    tls_ctx_cache.slot[i].cert = o->cert ? strdup(o->cert) : NULL;
    tls_ctx_cache.slot[i].key = o->key ? strdup(o->key) : NULL;
    if ((!o->ca_file || tls_ctx_cache.slot[i].ca_file)
        && (!o->ca_dir || tls_ctx_cache.slot[i].ca_dir)
        && (!o->alpn || tls_ctx_cache.slot[i].alpn)
        && (!o->cert || tls_ctx_cache.slot[i].cert)
        && (!o->key || tls_ctx_cache.slot[i].key)) {
        tls_ctx_cache.slot[i].insecure = o->insecure;
        tls_ctx_cache.slot[i].min_version = o->min_version;
        tls_ctx_cache.slot[i].ctx = c;
        tls_ctx_cache.slot[i].used = ++tls_ctx_cache.tick;
        c->refs = 2;
    } else {
        c->refs = 0;
    }
    pthread_mutex_unlock(&tls_ctx_cache.lock);
    if (dead)
        dyn_tls_ctx_free(dead);
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

dyn_tls_ctx_t* dyn_tls_ctx_server(const dyn_tls_srv_opts_t* o,
    char* err, size_t errlen)
{
    dyn_tls_ctx_t* c;
    long opts;

    if (!o->cert || !o->key) {
        snprintf(err, errlen, "a TLS server needs both `cert` and `key`");
        return NULL;
    }
    c = (dyn_tls_ctx_t*)calloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->ctx = SSL_CTX_new(TLS_server_method());
    if (!c->ctx) {
        tls_err(err, errlen, "SSL_CTX_new");
        free(c);
        return NULL;
    }

    if (!SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION)) {
        tls_err(err, errlen, "set_min_proto_version");
        goto fail;
    }
    opts = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;
#ifdef SSL_OP_NO_RENEGOTIATION
    opts |= SSL_OP_NO_RENEGOTIATION;
#endif
    SSL_CTX_set_options(c->ctx, opts);
    if (!SSL_CTX_set_cipher_list(c->ctx,
            "ECDHE+AESGCM:ECDHE+CHACHA20:!aNULL:!eNULL:!MD5:!RC4:!3DES")) {
        tls_err(err, errlen, "set_cipher_list");
        goto fail;
    }
    SSL_CTX_set_session_id_context(c->ctx,
        (const unsigned char*)"dynajs-tls-server", 17);
    if (SSL_CTX_use_certificate_chain_file(c->ctx, o->cert) != 1) {
        tls_err(err, errlen, "cannot read the certificate chain");
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(c->ctx, o->key, SSL_FILETYPE_PEM) != 1) {
        tls_err(err, errlen, "cannot read the private key");
        goto fail;
    }
    if (SSL_CTX_check_private_key(c->ctx) != 1) {
        tls_err(err, errlen, "the private key does not match the certificate");
        goto fail;
    }
    if (o->request_cert) {
        if (!o->ca_file) {
            snprintf(err, errlen, "requestCert needs `ca` (a PEM trust store): "
                                  "a client certificate cannot be verified "
                                  "without one");
            goto fail;
        }
        if (!SSL_CTX_load_verify_locations(c->ctx, o->ca_file, NULL)) {
            tls_err(err, errlen, "cannot read the client-cert trust store");
            goto fail;
        }
        SSL_CTX_set_verify(c->ctx,
            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        SSL_CTX_set_verify_depth(c->ctx, 8);
    }
    if (o->alpn && *o->alpn) {
        if (alpn_encode(o->alpn, &c->alpn_wire, &c->alpn_len) != 0) {
            snprintf(err, errlen, "bad alpn list");
            goto fail;
        }
        SSL_CTX_set_alpn_select_cb(c->ctx, tls_alpn_select, c);
    }
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

void dyn_tls_ctx_free(dyn_tls_ctx_t* c)
{
    int last;
    if (!c)
        return;
    if (c->refs > 0) {
        pthread_mutex_lock(&tls_ctx_cache.lock);
        last = (--c->refs == 0);
        pthread_mutex_unlock(&tls_ctx_cache.lock);
        if (!last)
            return;
    }
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    free(c->alpn_wire);
    free(c);
}

static int tls_name_is_ip(const char* s)
{
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == ':'))
            return 0;
    }
    return 1;
}

dyn_tls_conn_t* dyn_tls_conn_new(dyn_tls_ctx_t* c, const char* servername,
    char* err, size_t errlen)
{
    dyn_tls_conn_t* t = (dyn_tls_conn_t*)calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    t->ssl = SSL_new(c->ctx);
    if (!t->ssl) {
        tls_err(err, errlen, "SSL_new");
        free(t);
        return NULL;
    }

    t->owner = c;
    if (!c->insecure && servername && *servername) {
        SSL_SESSION* s = tls_sess_lookup(c, servername);
        if (s) {
            SSL_set_session(t->ssl, s);
            SSL_SESSION_free(s);
        }
    }

    if (!c->insecure) {
        if (!servername || !*servername) {
            snprintf(err, errlen,
                "servername is required when verification is on");
            goto fail;
        }
        SSL_set_hostflags(t->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!SSL_set1_host(t->ssl, servername)) {
            tls_err(err, errlen, "set1_host");
            goto fail;
        }
    }
    if (servername && *servername && !tls_name_is_ip(servername))
        SSL_set_tlsext_host_name(t->ssl, (void*)(uintptr_t)servername);

    t->sess_name = (servername && *servername) ? strdup(servername) : NULL;
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        snprintf(err, errlen, "BIO_new");
        if (t->rbio)
            BIO_free(t->rbio);
        if (t->wbio)
            BIO_free(t->wbio);
        goto fail;
    }
    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);
    SSL_set_connect_state(t->ssl);
    return t;
fail:
    SSL_free(t->ssl);
    free(t);
    return NULL;
}

dyn_tls_conn_t* dyn_tls_conn_accept(dyn_tls_ctx_t* c, char* err, size_t errlen)
{
    dyn_tls_conn_t* t = (dyn_tls_conn_t*)calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    t->ssl = SSL_new(c->ctx);
    if (!t->ssl) {
        tls_err(err, errlen, "SSL_new");
        free(t);
        return NULL;
    }
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        snprintf(err, errlen, "BIO_new");
        if (t->rbio)
            BIO_free(t->rbio);
        if (t->wbio)
            BIO_free(t->wbio);
        SSL_free(t->ssl);
        free(t);
        return NULL;
    }
    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);
    SSL_set_accept_state(t->ssl);
    return t;
}

void dyn_tls_conn_free(dyn_tls_conn_t* t)
{
    if (!t)
        return;
    if (t->ssl && t->handshake_done && !t->fatal)
        SSL_shutdown(t->ssl);
    if (t->ssl && t->handshake_done && !t->fatal && t->owner && t->sess_name)
        tls_sess_store(t->owner, t->sess_name, SSL_get1_session(t->ssl));
    SSL_free(t->ssl);
    free(t->sess_name);
    free(t);
}

const char* dyn_tls_error(const dyn_tls_conn_t* t)
{
    return t->err[0] ? t->err : NULL;
}

#define TLS_CHUNK_MAX (1 << 24)

static int tls_clamp(size_t n)
{
    return n > (size_t)TLS_CHUNK_MAX ? TLS_CHUNK_MAX : (int)n;
}

int dyn_tls_feed(dyn_tls_conn_t* t, const uint8_t* cipher, size_t n)
{
    int w, want;
    if (t->fatal)
        return -1;
    if (n == 0)
        return 0;
    want = tls_clamp(n);
    w = BIO_write(t->rbio, cipher, want);
    return (w == want) ? 0 : -1;
}

int dyn_tls_pull(dyn_tls_conn_t* t, uint8_t* out, size_t cap)
{
    int r = BIO_read(t->wbio, out, tls_clamp(cap));
    if (r > 0)
        return r;
    return 0;
}

static int tls_status(dyn_tls_conn_t* t, int rc, const char* what)
{
    int e = SSL_get_error(t->ssl, rc);
    long v;
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        return 0;
    if (e == SSL_ERROR_ZERO_RETURN) {
        t->fatal = 1;
        snprintf(t->err, sizeof t->err, "peer closed the TLS session");
        return -1;
    }
    v = SSL_get_verify_result(t->ssl);
    if (v != X509_V_OK)
        snprintf(t->err, sizeof t->err, "certificate rejected: %s",
            X509_verify_cert_error_string(v));
    else
        tls_err(t->err, sizeof t->err, what);
    t->fatal = 1;
    return -1;
}

int dyn_tls_handshake(dyn_tls_conn_t* t)
{
    int rc;
    if (t->fatal)
        return -1;
    if (t->handshake_done)
        return 1;
    ERR_clear_error();
    rc = SSL_do_handshake(t->ssl);
    if (rc == 1) {
        t->handshake_done = 1;
        if (t->owner && t->sess_name)
            tls_sess_store(t->owner, t->sess_name, SSL_get1_session(t->ssl));
        return 1;
    }
    return tls_status(t, rc, "handshake");
}

int dyn_tls_read(dyn_tls_conn_t* t, uint8_t* out, size_t cap)
{
    int rc;
    if (t->fatal)
        return -1;
    if (!t->handshake_done)
        return 0;
    ERR_clear_error();
    rc = SSL_read(t->ssl, out, tls_clamp(cap));
    if (rc > 0)
        return rc;
    return tls_status(t, rc, "read");
}

int dyn_tls_write(dyn_tls_conn_t* t, const uint8_t* in, size_t n)
{
    int rc;
    if (t->fatal)
        return -1;
    if (!t->handshake_done) {
        snprintf(t->err, sizeof t->err,
            "write before the TLS handshake completed");
        return -1;
    }
    if (n == 0)
        return 0;
    ERR_clear_error();
    rc = SSL_write(t->ssl, in, tls_clamp(n));
    if (rc > 0)
        return rc;
    return tls_status(t, rc, "write");
}

int dyn_tls_handshake_done(const dyn_tls_conn_t* t) { return t->handshake_done; }

const char* dyn_tls_alpn_selected(const dyn_tls_conn_t* t, char* dst,
    size_t cap)
{
    const unsigned char* p = NULL;
    unsigned len = 0;
    if (!dst || cap == 0)
        return NULL;
    SSL_get0_alpn_selected(t->ssl, &p, &len);
    if (!p || len == 0 || len >= cap)
        return NULL;
    memcpy(dst, p, len);
    dst[len] = 0;
    return dst;
}

const char* dyn_tls_version_negotiated(const dyn_tls_conn_t* t)
{
    return SSL_get_version(t->ssl);
}

#endif
