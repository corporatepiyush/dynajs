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

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

struct dyn_tls_ctx {
    SSL_CTX *ctx;
    char    *alpn_wire;      /* length-prefixed ALPN vector, or NULL */
    size_t   alpn_len;
    int      insecure;
};

struct dyn_tls_conn {
    SSL  *ssl;
    BIO  *rbio;              /* ciphertext IN  (we write, engine reads) */
    BIO  *wbio;              /* ciphertext OUT (engine writes, we read) */
    int   handshake_done;
    int   fatal;
    char  err[192];
};

/* The LINKED library's version, never OPENSSL_VERSION_TEXT: on a host with a
   keg-only OpenSSL beside a system LibreSSL the two disagree and only this is
   true. Design 16 trap 1. */
const char *dyn_tls_runtime_version(void)
{
    return OpenSSL_version(OPENSSL_VERSION);
}

const char *dyn_tls_backend(void) { return "openssl"; }

static void tls_err(char *dst, size_t cap, const char *what)
{
    unsigned long e = ERR_get_error();
    char buf[128];
    if (e) {
        ERR_error_string_n(e, buf, sizeof buf);
        snprintf(dst, cap, "%s: %s", what, buf);
    } else {
        snprintf(dst, cap, "%s", what);
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

dyn_tls_ctx_t *dyn_tls_ctx_client(const dyn_tls_opts_t *o,
                                  char *err, size_t errlen)
{
    dyn_tls_ctx_t *c;
    long opts;

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
    opts = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;
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
        } else if (!SSL_CTX_set_default_verify_paths(c->ctx)) {
            tls_err(err, errlen, "no platform trust store");
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
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

dyn_tls_ctx_t *dyn_tls_ctx_server(const char *cert_pem, const char *key_pem,
                                  const char *alpn, char *err, size_t errlen)
{
    dyn_tls_ctx_t *c;
    long opts;

    if (!cert_pem || !key_pem) {
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
    /* The chain, then the key, then PROVE they match. Loading a key that does
       not belong to the certificate fails at the first handshake otherwise --
       at runtime, per client, instead of at startup. */
    if (SSL_CTX_use_certificate_chain_file(c->ctx, cert_pem) != 1) {
        tls_err(err, errlen, "cannot read the certificate chain");
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(c->ctx, key_pem, SSL_FILETYPE_PEM) != 1) {
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
    if (alpn && *alpn) {
        if (alpn_encode(alpn, &c->alpn_wire, &c->alpn_len) != 0) {
            snprintf(err, errlen, "bad alpn list");
            goto fail;
        }
    }
    return c;
fail:
    dyn_tls_ctx_free(c);
    return NULL;
}

void dyn_tls_ctx_free(dyn_tls_ctx_t *c)
{
    if (!c)
        return;
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    free(c->alpn_wire);
    free(c);
}

dyn_tls_conn_t *dyn_tls_conn_new(dyn_tls_ctx_t *c, const char *servername,
                                 char *err, size_t errlen)
{
    dyn_tls_conn_t *t = (dyn_tls_conn_t *)calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }

    t->ssl = SSL_new(c->ctx);
    if (!t->ssl) { tls_err(err, errlen, "SSL_new"); free(t); return NULL; }

    /* Hostname verification is part of certificate verification, not a
       separate step a caller can forget: SSL_set1_host makes a name mismatch a
       verify failure. Partial wildcards (a*.example) are refused. */
    if (!c->insecure && servername && *servername) {
        SSL_set_hostflags(t->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!SSL_set1_host(t->ssl, servername)) {
            tls_err(err, errlen, "set1_host");
            goto fail;
        }
    }
    if (servername && *servername)
        SSL_set_tlsext_host_name(t->ssl, servername);   /* SNI */

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
    SSL_free(t->ssl);        /* frees the BIO pair it owns */
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

size_t dyn_tls_pending(const dyn_tls_conn_t *t)
{
    return (size_t)BIO_ctrl_pending(t->wbio);
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
    if (!t->handshake_done)
        return -1;
    if (n == 0)
        return 0;
    ERR_clear_error();
    rc = SSL_write(t->ssl, in, tls_clamp(n));
    if (rc > 0)
        return rc;
    return tls_status(t, rc, "write");
}

int dyn_tls_handshake_done(const dyn_tls_conn_t *t) { return t->handshake_done; }

const char *dyn_tls_alpn_selected(const dyn_tls_conn_t *t)
{
    const unsigned char *p = NULL;
    unsigned len = 0;
    static char buf[64];
    SSL_get0_alpn_selected(t->ssl, &p, &len);
    if (!p || len == 0 || len >= sizeof buf)
        return NULL;
    memcpy(buf, p, len);
    buf[len] = 0;
    return buf;
}

const char *dyn_tls_version_negotiated(const dyn_tls_conn_t *t)
{
    return SSL_get_version(t->ssl);
}

#endif /* CONFIG_TLS */
