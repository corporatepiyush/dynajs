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
    const char *alpn;        /* comma-separated, e.g. "h2,http/1.1"; may be NULL */
    int         min_version; /* 12 or 13; anything else means 12 */
    int         insecure;    /* rejectUnauthorized:false -- caller states it */
} dyn_tls_opts_t;

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
   to start. */
dyn_tls_ctx_t *dyn_tls_ctx_server(const char *cert_pem, const char *key_pem,
                                  const char *alpn, char *err, size_t errlen);
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
size_t dyn_tls_pending(const dyn_tls_conn_t *t);

/* >0 bytes, 0 = would block, -1 = fatal (dyn_tls_error says why). */
int dyn_tls_read(dyn_tls_conn_t *t, uint8_t *out, size_t cap);
int dyn_tls_write(dyn_tls_conn_t *t, const uint8_t *in, size_t n);

const char *dyn_tls_alpn_selected(const dyn_tls_conn_t *t);
const char *dyn_tls_version_negotiated(const dyn_tls_conn_t *t);

#endif /* CONFIG_TLS */
#endif /* DYNAJS_TLS_H */
