/*
 * dyna-tls.c -- TLS transport adapter (design 16). OpenSSL backend, INTERIM.
 *
 * NOT an implementation: a record-layer pump. The engine is a pure state
 * machine over two memory BIOs, so it drops onto the existing non-blocking
 * reactor with no blocking call anywhere -- readable -> feed -> drain
 * plaintext; app write -> engine -> writable -> flush.
 */
#include "dyna-tls.h"

#ifdef CONFIG_TLS

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

/* 3.2 CA trust store -- where the DEFAULT roots come from (documented
   policy, pinned by tests/test_tls_roots.js):
     1. macOS: Security framework anchors (SystemRootCertificates), dlopen'd
        SecTrustCopyAnchorCertificates -> DER -> X509_STORE_add_cert. The
        framework is resolved at runtime so the CONFIG_TLS link needs no
        -framework Security flag.
     2. Every OS: SSL_CTX_set_default_verify_paths -- the distro bundle
        (Debian/Ubuntu /etc/ssl/certs/ca-certificates.crt, RHEL /etc/pki/
        tls/certs/ca-bundle.crt; on macOS the brew ca-certificates bundle).
     3. EVERY OS: the vendored Mozilla bundle at DYNA_CA_BUNDLE -- the
        deterministic fallback, so a build on any platform trusts at least
        the same committed root set. Origin/update: generated ONCE from the
        host's own curl/OpenSSL bundle by tests/gen_ca_bundle.sh, which
        refuses output lacking the pinned root; re-run it when the Mozilla
        root set moves, re-pin tests/test_tls_roots.js, re-commit both.
   Pinned root: ISRG Root X1, SHA-256
   96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6
   (crt.sh 9314791 / letsencrypt.org/certs/isrgrootx1.pem / host bundles). */
#ifndef DYNA_CA_BUNDLE
#define DYNA_CA_BUNDLE "tests/corpus/ca-bundle.pem"
#endif

/* Client session reuse (audit E12-02): a per-ctx store of resumable sessions
   keyed by the server name the connection targeted. Slots live on the CTX, so
   a session can only be offered back to a peer that the SAME trust settings
   produced it for -- an insecure ctx's sessions can never reach a verifying
   one, because those are different cache entries. The mutex is a single
   global: sessions are touched briefly (get1/set/free) and handshakes are
   milliseconds apart; a per-ctx lock buys nothing. */
#define TLS_SESS_SLOTS 8

typedef struct {
    dyn_tls_ctx_t *owner;   /* the ctx the session belongs to; compare-only */
    char        *name;      /* the servername the session belongs to */
    SSL_SESSION *s;         /* owned (get1 ref); NULL when the slot is free */
    unsigned long used;     /* LRU clock */
} tls_sess_slot_t;

static struct {
    pthread_mutex_t lock;
    tls_sess_slot_t slot[TLS_SESS_SLOTS];
    unsigned long   tick;
} tls_sess_cache = { PTHREAD_MUTEX_INITIALIZER, {{0}}, 0 };

static int tls_streq_opt(const char *a, const char *b)
{
    return (a == b) || (a && b && strcmp(a, b) == 0);
}

/* Take ownership of `s` (a get1 reference; consumed either way). */
static void tls_sess_store(dyn_tls_ctx_t *c, const char *name, SSL_SESSION *s)
{
    int i, victim;
    char *copy;

    if (!s)
        return;
    if (!name || !*name || !SSL_SESSION_is_resumable(s)) {
        SSL_SESSION_free(s);          /* the session is not offerable */
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
            victim = i;               /* first free slot */
        if (tls_sess_cache.slot[i].s
            && tls_sess_cache.slot[i].owner == c
            && tls_streq_opt(tls_sess_cache.slot[i].name, name)) {
            free(tls_sess_cache.slot[i].name);      /* replace in place */
            SSL_SESSION_free(tls_sess_cache.slot[i].s);
            tls_sess_cache.slot[i].s = NULL;
            tls_sess_cache.slot[i].name = NULL;
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        victim = 0;                   /* LRU: replace the oldest resident */
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

/* Returns a NEW reference (caller must SSL_SESSION_free it) or NULL. */
static SSL_SESSION *tls_sess_lookup(dyn_tls_ctx_t *c, const char *name)
{
    SSL_SESSION *s = NULL;
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
    SSL_CTX *ctx;
    char    *alpn_wire;      /* length-prefixed ALPN vector, or NULL */
    size_t   alpn_len;
    int      insecure;
    int      refs;           /* cache-shared clients only */
};

/* Shared-client ctx cache (audit E12-04). Keyed on every option that changes
   what the ctx DOES -- (ca_file, ca_dir, insecure, alpn, min_version) -- as a
   fixed 8-slot LRU with a linear scan: option combinations per process are a
   handful, so a hash tree buys nothing, and a single-entry cache (the old
   shape) silently handed a second caller's custom CA the first caller's
   trust store. The cache holds one reference per entry; dyn_tls_ctx_free
   releases it, so a ctx dies with its LAST user after eviction (or at exit). */
#define TLS_CTX_CACHE_MAX 8

typedef struct {
    char *ca_file, *ca_dir, *alpn;      /* owned copies of the key */
    char *cert, *key;                   /* presented client cert (mTLS) */
    int   insecure, min_version;
    dyn_tls_ctx_t *ctx;                 /* holds one cache reference */
    unsigned long used;                 /* LRU clock */
} tls_cache_slot_t;

static struct {
    pthread_mutex_t  lock;
    tls_cache_slot_t slot[TLS_CTX_CACHE_MAX];
    int              n;
    unsigned long    tick;
} tls_ctx_cache = { PTHREAD_MUTEX_INITIALIZER, {{0}}, 0, 0 };

static int tls_key_eq(const tls_cache_slot_t *s, const dyn_tls_opts_t *o)
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
    SSL  *ssl;
    BIO  *rbio;              /* ciphertext IN  (we write, engine reads) */
    BIO  *wbio;              /* ciphertext OUT (engine writes, we read) */
    int   handshake_done;
    int   fatal;
    char  err[192];
    /* Session reuse (E12-02): the ctx is BORROWED -- every teardown path in
       this repo frees connections before their ctx (dyna-net-tcp disposes
       conns, then the ctx; dyna-http frees conn.tls while the client's ctx
       lives on) -- and only dereferenced at handshake/close for the store. */
    dyn_tls_ctx_t *owner;
    char *sess_name;         /* the servername this session belongs to */
};

/* The LINKED library's version, never OPENSSL_VERSION_TEXT: on a host with a
   keg-only OpenSSL beside a system LibreSSL the two disagree and only this is
   true. Design 16 trap 1. */
const char *dyn_tls_runtime_version(void)
{
    return OpenSSL_version(OPENSSL_VERSION);
}

const char *dyn_tls_backend(void) { return "openssl"; }

/* Verification failures queue SEVERAL errors and the first can be a
   secondary symptom of the real one, so drain up to three, each appended
   "; "-separated and bounded by the destination. */
static void tls_err(char *dst, size_t cap, const char *what)
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
        if (off < cap) /* once truncated, further appends have no room */
            off += (size_t)snprintf(dst + off, cap - off, "%s%s",
                                    k ? "; " : "", buf);
        e = ERR_get_error();
    }
    ERR_clear_error();
}

/* "h2,http/1.1" -> 0x02 'h' '2' 0x08 'h' 't' ... . Returns 0 on success. */
static int alpn_encode(const char *csv, char **out, size_t *outlen)
{
    size_t n = strlen(csv), i = 0, w = 0;
    char *buf;
    *out = NULL; *outlen = 0;
    if (n == 0)
        return 0;
    buf = (char *)malloc(n + 2);
    if (!buf)
        return -1;
    while (i <= n) {
        size_t start = i;
        while (i < n && csv[i] != ',')
            i++;
        if (i == start || i - start > 255) { free(buf); return -1; }
        buf[w++] = (char)(i - start);
        memcpy(buf + w, csv + start, i - start);
        w += i - start;
        if (i >= n)
            break;
        i++;                                  /* skip the comma */
    }
    *out = buf; *outlen = w;
    return 0;
}

/* Server-side ALPN selection. The callback makes the server's configured
   `alpn` list REAL: until it was installed, alpn_wire was built and never
   consulted, and dyn_tls_alpn_selected on accepted conns always returned
   NULL. Preference order is the CLIENT's: the first protocol the client
   offered that appears in our list wins (RFC 7301 3.2). A client that offers
   nothing we advertise -- including a client that offers no ALPN at all --
   gets SSL_TLSEXT_ERR_NOACK and the handshake proceeds un-negotiated. */
static int tls_alpn_select(SSL *ssl, const unsigned char **out,
                           unsigned char *outlen, const unsigned char *in,
                           unsigned int inlen, void *arg)
{
    const dyn_tls_ctx_t *c = (const dyn_tls_ctx_t *)arg;
    const unsigned char *p = in, *end = in + inlen;
    (void)ssl;

    if (!c->alpn_wire || c->alpn_len == 0)
        return SSL_TLSEXT_ERR_NOACK;
    while (p < end) {
        unsigned char l = *p;
        size_t w = 0;
        if (p + 1 + l > end)
            break;                       /* malformed vector: stop scanning */
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
/* Opaque CoreFoundation/Security handles, declared by shape only: the
   framework is dlopen'd, so the CONFIG_TLS link needs no framework flag. */
typedef long CFIndex;
typedef struct __CFArray *CFArrayRef;
typedef struct __CFData *CFDataRef;
typedef struct OpaqueSecCertificateRef *SecCertificateRef;

/* macOS: add the OS's own trust anchors (SystemRootCertificates) to `store`.
   0 = ok, -1 = framework unavailable (a Linux build has no Security
   framework, so this is a documented non-source there). */
static int tls_add_macos_anchors(X509_STORE *store)
{
    void *sec;
    CFArrayRef anchors = NULL;
    CFIndex i, n;
    int (*copy_anchors)(CFArrayRef *);
    CFDataRef (*cert_data)(SecCertificateRef);
    long (*cf_count)(CFArrayRef);
    const void *(*cf_get)(CFArrayRef, CFIndex);
    const unsigned char *(*cf_ptr)(CFDataRef);
    long (*cf_len)(CFDataRef);
    void (*cf_rel)(const void *);
    int added = 0;

    sec = dlopen("/System/Library/Frameworks/Security.framework/Security",
                 RTLD_LAZY);
    if (!sec)
        return -1;
    copy_anchors = (int (*)(CFArrayRef *))dlsym(sec,
        "SecTrustCopyAnchorCertificates");
    cert_data = (CFDataRef (*)(SecCertificateRef))dlsym(sec,
        "SecCertificateCopyData");
    /* CF helpers resolve through RTLD_DEFAULT: CoreFoundation is always
       loaded in a macOS process. */
    cf_count = (long (*)(CFArrayRef))dlsym(RTLD_DEFAULT, "CFArrayGetCount");
    cf_get = (const void *(*)(CFArrayRef, CFIndex))dlsym(RTLD_DEFAULT,
        "CFArrayGetValueAtIndex");
    cf_ptr = (const unsigned char *(*)(CFDataRef))dlsym(RTLD_DEFAULT,
        "CFDataGetBytePtr");
    cf_len = (long (*)(CFDataRef))dlsym(RTLD_DEFAULT, "CFDataGetLength");
    cf_rel = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
    if (!copy_anchors || !cert_data || !cf_count || !cf_get || !cf_ptr
        || !cf_len || !cf_rel || copy_anchors(&anchors) != 0 || !anchors)
        return -1;
    n = cf_count(anchors);
    for (i = 0; i < n; i++) {
        SecCertificateRef c = (SecCertificateRef)cf_get(anchors, i);
        CFDataRef d = cert_data(c);
        const unsigned char *p;
        X509 *x;
        if (!d)
            continue;
        p = cf_ptr(d);
        x = d2i_X509(NULL, &p, (long)cf_len(d));
        if (x) {
            X509_STORE_add_cert(store, x);   /* up-refs; we free ours below */
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
static int tls_add_macos_anchors(X509_STORE *store)
{
    (void)store;
    return -1;   /* no Security framework on this OS */
}
#endif

/* Load the DEFAULT trust store (no ca_file/ca_dir): macOS anchors, then the
   distro bundle via default verify paths, then the vendored Mozilla bundle
   as the deterministic fallback everywhere. 0 = ok, -1 = NOTHING loaded. */
static int tls_load_default_roots(SSL_CTX *ctx, char *err, size_t errlen)
{
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    int have = 0;

    if (tls_add_macos_anchors(store) == 0)
        have = 1;
    if (SSL_CTX_set_default_verify_paths(ctx))
        have = 1;
    if (SSL_CTX_load_verify_locations(ctx, DYNA_CA_BUNDLE, NULL))
        have = 1;
    else
        ERR_clear_error();   /* missing bundle is fatal only if nothing else */
    if (!have) {
        snprintf(err, errlen, "no trust store: platform roots and vendored "
                              "Mozilla bundle both unavailable");
        return -1;
    }
    return 0;
}

dyn_tls_ctx_t *dyn_tls_ctx_client(const dyn_tls_opts_t *o,
                                  char *err, size_t errlen)
{
    dyn_tls_ctx_t *c;
    dyn_tls_ctx_t *dead = NULL;
    long opts;
    int i;

    /* Every client ctx goes through the cache, whatever its options: the key
       is the full option set, so two callers never share a ctx whose trust
       store, insecure flag, ALPN list or protocol floor they did not ask
       for. Look up under the lock; on a miss build OUTSIDE it (the trust-
       store load is milliseconds and must not serialize other callers). */
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

    c = (dyn_tls_ctx_t *)calloc(1, sizeof(*c));
    if (!c) { snprintf(err, errlen, "out of memory"); return NULL; }

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) { tls_err(err, errlen, "SSL_CTX_new"); free(c); return NULL; }

    /* 1.0/1.1/SSLv3 are not reachable: the floor is 1.2 and the caller may
       raise it to 1.3. Renegotiation and compression are off (CRIME). */
    if (!SSL_CTX_set_min_proto_version(c->ctx,
            o->min_version >= 13 ? TLS1_3_VERSION : TLS1_2_VERSION)) {
        tls_err(err, errlen, "set_min_proto_version");
        goto fail;
    }
    /* No SSL_OP_CIPHER_SERVER_PREFERENCE here: it orders the handshake by
       our list only when we are the server -- client-side it is a no-op
       (it belongs to the server factory below). */
    opts = SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
    opts |= SSL_OP_NO_RENEGOTIATION;
#endif
    SSL_CTX_set_options(c->ctx, opts);

    /* AEAD only for 1.2; 1.3's suites are AEAD by construction. */
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

    /* mTLS (audit E12-03), client side: what THIS client presents. One half
       of a pair is a misconfiguration, not a default -- refuse at build. */
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
                (const unsigned char *)c->alpn_wire, (unsigned)c->alpn_len)) {
            tls_err(err, errlen, "set_alpn_protos");
            goto fail;
        }
    }

    /* Publish. A caller that raced us to the same key loses its own build
       and takes the winner; a key-string strdup failure leaves the ctx
       uncached (refs == 0), which behaves exactly like the old fresh-build
       path. `refs == 1` is the cache's own reference. */
    c->refs = 1;
    pthread_mutex_lock(&tls_ctx_cache.lock);
    for (i = 0; i < tls_ctx_cache.n; i++) {
        if (tls_key_eq(&tls_ctx_cache.slot[i], o)) {
            dyn_tls_ctx_t *winner = tls_ctx_cache.slot[i].ctx;
            winner->refs++;
            tls_ctx_cache.slot[i].used = ++tls_ctx_cache.tick;
            pthread_mutex_unlock(&tls_ctx_cache.lock);
            dyn_tls_ctx_free(c);        /* the loser: refs drops to 0 */
            return winner;
        }
    }
    if (tls_ctx_cache.n < TLS_CTX_CACHE_MAX) {
        i = tls_ctx_cache.n++;
    } else {
        /* LRU eviction: replace the least recently used slot. Its ctx frees
           only when the last USER also dropped it -- the refcount below
           makes the cache's drop a no-op for any live holder. */
        int ev = 0, k;
        for (k = 1; k < tls_ctx_cache.n; k++)
            if (tls_ctx_cache.slot[k].used < tls_ctx_cache.slot[ev].used)
                ev = k;
        dead = tls_ctx_cache.slot[ev].ctx;      /* unref'd below */
        free(tls_ctx_cache.slot[ev].ca_file);
        free(tls_ctx_cache.slot[ev].ca_dir);
        free(tls_ctx_cache.slot[ev].alpn);
        free(tls_ctx_cache.slot[ev].cert);
        free(tls_ctx_cache.slot[ev].key);
        i = ev;
    }
    tls_ctx_cache.slot[i].ca_file = o->ca_file ? strdup(o->ca_file) : NULL;
    tls_ctx_cache.slot[i].ca_dir  = o->ca_dir  ? strdup(o->ca_dir)  : NULL;
    tls_ctx_cache.slot[i].alpn    = o->alpn    ? strdup(o->alpn)    : NULL;
    tls_ctx_cache.slot[i].cert    = o->cert    ? strdup(o->cert)    : NULL;
    tls_ctx_cache.slot[i].key     = o->key     ? strdup(o->key)     : NULL;
    if ((!o->ca_file || tls_ctx_cache.slot[i].ca_file)
        && (!o->ca_dir || tls_ctx_cache.slot[i].ca_dir)
        && (!o->alpn || tls_ctx_cache.slot[i].alpn)
        && (!o->cert || tls_ctx_cache.slot[i].cert)
        && (!o->key || tls_ctx_cache.slot[i].key)) {
        tls_ctx_cache.slot[i].insecure = o->insecure;
        tls_ctx_cache.slot[i].min_version = o->min_version;
        tls_ctx_cache.slot[i].ctx = c;
        tls_ctx_cache.slot[i].used = ++tls_ctx_cache.tick;
        c->refs = 2;      /* the cache holds one; THIS caller holds the other */
    } else {
        c->refs = 0;                    /* OOM: valid ctx, just not cached */
    }
    pthread_mutex_unlock(&tls_ctx_cache.lock);
    if (dead)
        dyn_tls_ctx_free(dead);         /* cache dropped its reference */
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

dyn_tls_ctx_t *dyn_tls_ctx_server(const dyn_tls_srv_opts_t *o,
                                  char *err, size_t errlen)
{
    dyn_tls_ctx_t *c;
    long opts;

    if (!o->cert || !o->key) {
        snprintf(err, errlen, "a TLS server needs both `cert` and `key`");
        return NULL;
    }
    c = (dyn_tls_ctx_t *)calloc(1, sizeof(*c));
    if (!c) { snprintf(err, errlen, "out of memory"); return NULL; }
    c->ctx = SSL_CTX_new(TLS_server_method());
    if (!c->ctx) { tls_err(err, errlen, "SSL_CTX_new"); free(c); return NULL; }

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
    /* Server-side session cache (audit E12-03 note): TLS 1.2 session ids are
       only issued once the server has a session_id_context to bind them to.
       Without it a 1.2 client's resumption attempt silently degrades to a
       full handshake. TLS 1.3 tickets need nothing here. */
    SSL_CTX_set_session_id_context(c->ctx,
        (const unsigned char *)"dynajs-tls-server", 17);
    /* The chain, then the key, then PROVE they match. Loading a key that does
       not belong to the certificate fails at the first handshake otherwise --
       at runtime, per client, instead of at startup. */
    if (SSL_CTX_use_certificate_chain_file(c->ctx, o->cert) != 1) {
        tls_err(err, errlen, "cannot read the certificate chain");
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(c->ctx, o->key, SSL_FILETYPE_PEM) != 1) {
        tls_err(err, errlen, "cannot read the private key");
        goto fail;
    }
    /* UNREACHED in practice: SSL_CTX_use_PrivateKey_file already rejects a key
       that does not belong to the loaded certificate ("key values mismatch"),
       so this second guard has no input only IT can catch. Kept because the
       obligation is documented at the call, not because it is a proven
       defence -- deleting it fails no test, which is exactly the tell. */
    if (SSL_CTX_check_private_key(c->ctx) != 1) {
        tls_err(err, errlen, "the private key does not match the certificate");
        goto fail;
    }
    /* mTLS (audit E12-03), server side: request a client certificate AND
       verify it against `ca`. VERIFY_FAIL_IF_NO_PEER_CERT makes a certless
       client a handshake refusal instead of an accepted anon connection.
       Requesting without a store is refused HERE, not per handshake: with an
       empty trust store every presented certificate would be rejected, so the
       server would look up but never let anyone in. */
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
        /* The whole point of the list: without the callback it was parsed,
           stored, and never consulted (the server negotiated nothing, ever). */
        SSL_CTX_set_alpn_select_cb(c->ctx, tls_alpn_select, c);
    }
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

void dyn_tls_ctx_free(dyn_tls_ctx_t *c)
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
    /* A cached ctx reaches the actual free only after the cache's own
       reference was dropped at eviction, so there is no slot to clear here:
       refcount zero means no holder anywhere can reach it. */
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    free(c->alpn_wire);
    free(c);
}

/* RFC 6066 3: a literal IPv4/IPv6 address must NOT be carried in SNI (the
 * server_name extension is for names). Strict shape check -- every byte a
 * digit, dot, or colon. Verification is unaffected either way: SSL_set1_host
 * matches IP literals against the certificate's SAN iIPaddress entries. */
static int tls_name_is_ip(const char *s)
{
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == ':'))
            return 0;
    }
    return 1;
}

dyn_tls_conn_t *dyn_tls_conn_new(dyn_tls_ctx_t *c, const char *servername,
                                 char *err, size_t errlen)
{
    dyn_tls_conn_t *t = (dyn_tls_conn_t *)calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }

    t->ssl = SSL_new(c->ctx);
    if (!t->ssl) { tls_err(err, errlen, "SSL_new"); free(t); return NULL; }

    /* Offer a session this ctx previously stored for this name (audit
       E12-02). A mismatched offer -- the peer restarted, the ticket rotated,
       the port changed -- is not an error: the peer declines and the
       handshake falls back to full. NULL when nothing was stored. */
    t->owner = c;
    if (!c->insecure && servername && *servername) {
        SSL_SESSION *s = tls_sess_lookup(c, servername);
        if (s) {
            SSL_set_session(t->ssl, s);   /* uprefs; our ref stays ours */
            SSL_SESSION_free(s);
        }
    }

    /* Hostname verification is part of certificate verification, not a
       separate step a caller can forget: SSL_set1_host makes a name mismatch a
       verify failure. Partial wildcards (a*.example) are refused. */
    if (!c->insecure) {
        if (!servername || !*servername) {
            /* SSL_VERIFY_PEER checks the chain only: with no reference
               identifier (RFC 6125 6.3/6.4) any CA-signed certificate
               passes. Refuse instead of accepting an unbound name. */
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
        SSL_set_tlsext_host_name(t->ssl, servername);   /* SNI */

    t->sess_name = (servername && *servername) ? strdup(servername) : NULL;
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        snprintf(err, errlen, "BIO_new");
        if (t->rbio) BIO_free(t->rbio);
        if (t->wbio) BIO_free(t->wbio);
        goto fail;
    }
    BIO_set_mem_eof_return(t->rbio, -1);     /* empty != EOF: want-read */
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);   /* takes ownership of both */
    SSL_set_connect_state(t->ssl);
    return t;
fail:
    SSL_free(t->ssl);
    free(t);
    return NULL;
}

dyn_tls_conn_t *dyn_tls_conn_accept(dyn_tls_ctx_t *c, char *err, size_t errlen)
{
    dyn_tls_conn_t *t = (dyn_tls_conn_t *)calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }
    t->ssl = SSL_new(c->ctx);
    if (!t->ssl) { tls_err(err, errlen, "SSL_new"); free(t); return NULL; }
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        snprintf(err, errlen, "BIO_new");
        if (t->rbio) BIO_free(t->rbio);
        if (t->wbio) BIO_free(t->wbio);
        SSL_free(t->ssl);
        free(t);
        return NULL;
    }
    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);
    SSL_set_accept_state(t->ssl);        /* the only difference from a client */
    return t;
}

void dyn_tls_conn_free(dyn_tls_conn_t *t)
{
    if (!t)
        return;
    /* Best-effort close_notify (RFC 5246 7.2.1): without it every local close
       is an abrupt FIN and a peer cannot tell a clean end from truncation.
       Attempt once, send side only -- SSL_shutdown's second leg waits for the
       peer's close_notify and would block this reactor -- and whatever it
       queued after this point has no pump to flush it, so the result is
       ignored by design. */
    if (t->ssl && t->handshake_done && !t->fatal)
        SSL_shutdown(t->ssl);
    /* Refresh the stored session (audit E12-02): by close time a TLS 1.3
       connection has processed the peer's NewSessionTickets during reads, so
       SSL_get1_session now yields what the handshake-time snapshot could not
       -- a RESUMABLE 1.3 session. The last successful store wins. */
    if (t->ssl && t->handshake_done && !t->fatal && t->owner && t->sess_name)
        tls_sess_store(t->owner, t->sess_name, SSL_get1_session(t->ssl));
    SSL_free(t->ssl);        /* frees the BIO pair it owns */
    free(t->sess_name);
    free(t);
}

const char *dyn_tls_error(const dyn_tls_conn_t *t)
{
    return t->err[0] ? t->err : NULL;
}

/* OpenSSL takes an int everywhere; a size_t from a caller can exceed INT_MAX
   and the cast then goes NEGATIVE, which is UB at the SSL_read/SSL_write
   boundary. Clamped once here rather than trusting every caller.

   UNREACHED TODAY, and measured so: the largest cap any caller passes is
   `resp->cap - resp->len`, and dyn_bytes_reserve grows in 8 KiB steps, so it
   never approaches HTTPClient's body cap even when that is set to 3e9 from JS.
   Removing the clamp changes nothing observable. Kept because it makes the
   primitive safe for a caller that DOES hand it a large span -- but do not
   count it as a verified defence. */
#define TLS_CHUNK_MAX (1 << 24)     /* 16 MiB: far above one 16 KiB record */

static int tls_clamp(size_t n)
{
    return n > (size_t)TLS_CHUNK_MAX ? TLS_CHUNK_MAX : (int)n;
}

int dyn_tls_feed(dyn_tls_conn_t *t, const uint8_t *cipher, size_t n)
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

int dyn_tls_pull(dyn_tls_conn_t *t, uint8_t *out, size_t cap)
{
    int r = BIO_read(t->wbio, out, tls_clamp(cap));
    if (r > 0)
        return r;
    return 0;                 /* nothing queued is not an error */
}

/* Turn an SSL_get_error into our tri-state, naming WHICH check failed on a
   verification error -- "handshake failed" is not an actionable report. */
static int tls_status(dyn_tls_conn_t *t, int rc, const char *what)
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

int dyn_tls_handshake(dyn_tls_conn_t *t)
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
        /* Early store (audit E12-02): a TLS 1.2 session id is resumable the
           moment the handshake lands. A 1.3 session is not -- its tickets
           arrive AFTER the handshake -- and is dropped here; conn_free
           refreshes it once the reads have processed the tickets. */
        if (t->owner && t->sess_name)
            tls_sess_store(t->owner, t->sess_name, SSL_get1_session(t->ssl));
        return 1;
    }
    return tls_status(t, rc, "handshake");
}

int dyn_tls_read(dyn_tls_conn_t *t, uint8_t *out, size_t cap)
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

int dyn_tls_write(dyn_tls_conn_t *t, const uint8_t *in, size_t n)
{
    int rc;
    if (t->fatal)
        return -1;
    if (!t->handshake_done) {
        /* A bare -1 left dyn_tls_error() NULL and the caller reported a
           failure with no reason. Name it. */
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

int dyn_tls_handshake_done(const dyn_tls_conn_t *t) { return t->handshake_done; }

/* Copies the negotiated ALPN protocol into the CALLER'S buffer (NUL-
   terminated) and returns dst, or NULL when nothing was negotiated or the
   buffer is too small. No static: a shared buffer was last-writer-wins
   across concurrent connections. */
const char *dyn_tls_alpn_selected(const dyn_tls_conn_t *t, char *dst,
                                  size_t cap)
{
    const unsigned char *p = NULL;
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

const char *dyn_tls_version_negotiated(const dyn_tls_conn_t *t)
{
    return SSL_get_version(t->ssl);
}

#endif /* CONFIG_TLS */
