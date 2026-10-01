#ifndef DYNAJS_TLS_H
#define DYNAJS_TLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_TLS

typedef struct dyn_tls_ctx dyn_tls_ctx_t;
typedef struct dyn_tls_conn dyn_tls_conn_t;

typedef struct {
    const char* ca_file;
    const char* ca_dir;
    const char* cert;
    const char* key;
    const char* alpn;
    int min_version;
    int insecure;
} dyn_tls_opts_t;

typedef struct {
    const char* cert;
    const char* key;
    const char* alpn;
    const char* ca_file;
    int request_cert;
} dyn_tls_srv_opts_t;

const char* dyn_tls_runtime_version(void);
const char* dyn_tls_backend(void);

dyn_tls_ctx_t* dyn_tls_ctx_client(const dyn_tls_opts_t* o,
    char* err, size_t errlen);

dyn_tls_ctx_t* dyn_tls_ctx_server(const dyn_tls_srv_opts_t* o,
    char* err, size_t errlen);
void dyn_tls_ctx_free(dyn_tls_ctx_t* c);

dyn_tls_conn_t* dyn_tls_conn_new(dyn_tls_ctx_t* c, const char* servername,
    char* err, size_t errlen);

dyn_tls_conn_t* dyn_tls_conn_accept(dyn_tls_ctx_t* c, char* err, size_t errlen);
void dyn_tls_conn_free(dyn_tls_conn_t* t);

int dyn_tls_handshake(dyn_tls_conn_t* t);
int dyn_tls_handshake_done(const dyn_tls_conn_t* t);
const char* dyn_tls_error(const dyn_tls_conn_t* t);

int dyn_tls_feed(dyn_tls_conn_t* t, const uint8_t* cipher, size_t n);
int dyn_tls_pull(dyn_tls_conn_t* t, uint8_t* out, size_t cap);

int dyn_tls_read(dyn_tls_conn_t* t, uint8_t* out, size_t cap);
int dyn_tls_write(dyn_tls_conn_t* t, const uint8_t* in, size_t n);

const char* dyn_tls_alpn_selected(const dyn_tls_conn_t* t, char* dst,
    size_t cap);
const char* dyn_tls_version_negotiated(const dyn_tls_conn_t* t);

#endif
#endif
