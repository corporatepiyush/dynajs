/*
 * dyna-tls.h -- TLS transport adapter (design 16).
 *
 * The engine is a state machine over two byte buffers: feed ciphertext in,
 * pull ciphertext out, read/write plaintext. Nothing here blocks or touches a
 * socket, so it composes with the existing reactor rather than beside it.
 *
 * BACKEND IS LINKED, NOT VENDORED, AND THAT IS INTERIM. The library changes
 * underneath the binary and its CVE stream is not the project's to control;
 * CONFIG_TLS_BACKEND=vendored is the exit. Build with CONFIG_TLS=y.
 */
#ifndef DYNAJS_TLS_H
#define DYNAJS_TLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_TLS

typedef struct dyn_tls_ctx  dyn_tls_ctx_t;
typedef struct dyn_tls_conn dyn_tls_conn_t;

typedef struct {
    const char *ca_file;     /* NULL + ca_dir NULL = platform trust store */
    const char *ca_dir;
    const char *cert;        /* client cert PEM path to PRESENT (mTLS);
                                requires `key` */
    const char *key;         /* the matching key PEM path */
    const char *alpn;        /* comma-separated, e.g. "h2,http/1.1"; may be NULL */
    int         min_version; /* 12 or 13; anything else means 12 */
    int         insecure;    /* rejectUnauthorized:false -- caller states it */
} dyn_tls_opts_t;

typedef struct {
    const char *cert;        /* server chain PEM path (required) */
    const char *key;         /* matching key PEM path (required) */
    const char *alpn;        /* comma-separated offer list; may be NULL */
    const char *ca_file;     /* client-cert trust store; request_cert needs it */
    int         request_cert; /* mTLS: request + require + verify a client cert */
} dyn_tls_srv_opts_t;

/* The LINKED library's version and the backend name. NEVER report the header
   constant: where a keg-only OpenSSL sits beside a system LibreSSL the two
   disagree and only the runtime one is true. */
const char *dyn_tls_runtime_version(void);
const char *dyn_tls_backend(void);

dyn_tls_ctx_t *dyn_tls_ctx_client(const dyn_tls_opts_t *o,
                                  char *err, size_t errlen);

/* A SERVER context needs a certificate chain and its private key, both PEM
   paths. There is no default and no self-signed fallback: a server that
   silently serves an unverifiable certificate is worse than one that refuses
   to start. With request_cert, the server demands a client certificate and
   verifies it against ca_file -- a request without a store is refused at
   startup, because every later handshake would fail in the peer's arms. */
dyn_tls_ctx_t *dyn_tls_ctx_server(const dyn_tls_srv_opts_t *o,
                                  char *err, size_t errlen);
void dyn_tls_ctx_free(dyn_tls_ctx_t *c);

/* servername drives BOTH SNI and hostname verification. NULL or empty is
   refused when verification is on. */
dyn_tls_conn_t *dyn_tls_conn_new(dyn_tls_ctx_t *c, const char *servername,
                                 char *err, size_t errlen);

/* The accepted-side twin: same pump, same handshake loop, opposite role. */
dyn_tls_conn_t *dyn_tls_conn_accept(dyn_tls_ctx_t *c, char *err, size_t errlen);
void dyn_tls_conn_free(dyn_tls_conn_t *t);

/* 1 = handshake complete, 0 = needs more IO (not an error), -1 = failed.
   On -1, dyn_tls_error names WHICH check failed. */
int dyn_tls_handshake(dyn_tls_conn_t *t);
int dyn_tls_handshake_done(const dyn_tls_conn_t *t);
const char *dyn_tls_error(const dyn_tls_conn_t *t);

int    dyn_tls_feed(dyn_tls_conn_t *t, const uint8_t *cipher, size_t n);
int    dyn_tls_pull(dyn_tls_conn_t *t, uint8_t *out, size_t cap);

/* >0 bytes, 0 = would block, -1 = fatal (dyn_tls_error says why). */
int dyn_tls_read(dyn_tls_conn_t *t, uint8_t *out, size_t cap);
int dyn_tls_write(dyn_tls_conn_t *t, const uint8_t *in, size_t n);

/* Copies the negotiated ALPN protocol into dst (NUL-terminated) and returns
   dst; NULL when nothing was negotiated or dst is too small. Caller-owned
   storage: no shared static across concurrent connections. */
const char *dyn_tls_alpn_selected(const dyn_tls_conn_t *t, char *dst,
                                  size_t cap);
const char *dyn_tls_version_negotiated(const dyn_tls_conn_t *t);

#endif /* CONFIG_TLS */
#endif /* DYNAJS_TLS_H */
