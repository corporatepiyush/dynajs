#include "webrtc-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/srtp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "webrtc-srtp.h"

uint64_t wrt_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static BIO_METHOD* wrt_bio_meth;
static pthread_once_t wrt_bio_once = PTHREAD_ONCE_INIT;

static int wrt_bio_create(BIO* b)
{
    BIO_set_init(b, 1);
    return 1;
}

static int wrt_bio_destroy(BIO* b)
{
    BIO_set_data(b, NULL);
    return 1;
}

static int wrt_bio_write(BIO* b, const char* in, int inl)
{
    wrt_dtls* d = (wrt_dtls*)BIO_get_data(b);
    BIO_clear_retry_flags(b);
    if (d && d->pc && inl > 0)
        wrt_pc_send_dtls(d->pc, (const uint8_t*)in, (size_t)inl);
    return inl;
}

static int wrt_bio_read(BIO* b, char* out, int outl)
{
    wrt_dtls* d = (wrt_dtls*)BIO_get_data(b);
    size_t n;
    BIO_clear_retry_flags(b);
    if (!d || !d->in_n) {
        BIO_set_retry_read(b);
        return -1;
    }
    n = d->in_n < (size_t)outl ? d->in_n : (size_t)outl;
    memcpy(out, d->in_p, n);
    d->in_n = 0;
    return (int)n;
}

static long wrt_bio_ctrl(BIO* b, int cmd, long num, void* ptr)
{
    (void)b;
    (void)num;
    (void)ptr;
    switch (cmd) {
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU:
    case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
        return WRT_DTLS_MTU;
    case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
        return 0;
    case BIO_CTRL_WPENDING:
    case BIO_CTRL_PENDING:
        return 0;
    case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        return 1;
    default:
        return 0;
    }
}

static void wrt_bio_setup(void)
{
    BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "dyna-dtls-dgram");
    if (!m)
        return;
    BIO_meth_set_write(m, wrt_bio_write);
    BIO_meth_set_read(m, wrt_bio_read);
    BIO_meth_set_ctrl(m, wrt_bio_ctrl);
    BIO_meth_set_create(m, wrt_bio_create);
    BIO_meth_set_destroy(m, wrt_bio_destroy);
    wrt_bio_meth = m;
}

typedef struct {
    wrt_cert c;
    SSL_CTX* ctx;
} wrt_cert_full;

static int wrt_fp_matches(const wrt_dtls* d, X509* peer);

static int wrt_verify_cb(int ok, X509_STORE_CTX* x)
{
    SSL* ssl;
    wrt_dtls* d;
    (void)ok;
    if (X509_STORE_CTX_get_error_depth(x) != 0)
        return 1;
    ssl = (SSL*)X509_STORE_CTX_get_ex_data(x, SSL_get_ex_data_X509_STORE_CTX_idx());
    d = ssl ? (wrt_dtls*)SSL_get_app_data(ssl) : NULL;
    if (!d || !d->remote_fp_len)
        return 1;
    if (!wrt_fp_matches(d, X509_STORE_CTX_get_current_cert(x))) {
        d->fp_failed = 1;
        X509_STORE_CTX_set_error(x, X509_V_ERR_CERT_REJECTED);
        return 0;
    }
    return 1;
}

static SSL_CTX* wrt_make_ctx(wrt_cert* c)
{
    SSL_CTX* ctx = SSL_CTX_new(DTLS_method());
    if (!ctx)
        return NULL;
    SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, DTLS1_2_VERSION);
    if (SSL_CTX_use_certificate(ctx, (X509*)c->x509) != 1 || SSL_CTX_use_PrivateKey(ctx, (EVP_PKEY*)c->pkey) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, wrt_verify_cb);
    SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
                                 "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305");
    {
        static char groups[] = "X25519:P-256:P-384";
        SSL_CTX_set1_groups_list(ctx, groups);
    }
    SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AEAD_AES_128_GCM:SRTP_AEAD_AES_256_GCM:SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");
    SSL_CTX_set_read_ahead(ctx, 1);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(ctx, SSL_OP_NO_QUERY_MTU | SSL_OP_NO_TICKET);
    return ctx;
}

static wrt_cert* wrt_cert_wrap(EVP_PKEY* pk, X509* x, double expires_ms, int rsa)
{
    wrt_cert_full* f = (wrt_cert_full*)calloc(1, sizeof(*f));
    unsigned int len = 0;
    if (!f)
        return NULL;
    f->c.refs = 1;
    f->c.pkey = pk;
    f->c.x509 = x;
    f->c.expires_ms = expires_ms;
    f->c.alg_rsa = rsa;
    X509_digest(x, EVP_sha256(), f->c.fp, &len);
    f->ctx = wrt_make_ctx(&f->c);
    if (!f->ctx) {
        free(f);
        return NULL;
    }
    return &f->c;
}

wrt_cert* wrt_cert_generate(int rsa, uint32_t modulus, double expires_ms, const char** err)
{
    EVP_PKEY* pk;
    X509* x;
    X509_NAME* nm;
    uint64_t serial = 0;
    wrt_cert* c;
    long secs;
    *err = NULL;
    pk = rsa ? EVP_RSA_gen(modulus) : EVP_EC_gen("P-256");
    if (!pk) {
        *err = "key generation failed";
        return NULL;
    }
    x = X509_new();
    if (!x) {
        EVP_PKEY_free(pk);
        *err = "out of memory";
        return NULL;
    }
    X509_set_version(x, 2);
    RAND_bytes((unsigned char*)&serial, sizeof(serial));
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(x), serial >> 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -86400);
    secs = (long)((expires_ms - (double)time(NULL) * 1000.0) / 1000.0);
    if (secs < 60)
        secs = 60;
    X509_gmtime_adj(X509_getm_notAfter(x), secs);
    nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char*)"WebRTC", -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509_set_pubkey(x, pk);
    if (!X509_sign(x, pk, EVP_sha256())) {
        X509_free(x);
        EVP_PKEY_free(pk);
        *err = "certificate signing failed";
        return NULL;
    }
    c = wrt_cert_wrap(pk, x, expires_ms, rsa);
    if (!c) {
        X509_free(x);
        EVP_PKEY_free(pk);
        *err = "certificate setup failed";
    }
    return c;
}

wrt_cert* wrt_cert_from_pem(const char* cert_pem, const char* key_pem, const char** err)
{
    BIO* b;
    X509* x = NULL;
    EVP_PKEY* pk = NULL;
    wrt_cert* c;
    *err = "invalid PEM";
    b = BIO_new_mem_buf(cert_pem, -1);
    if (b) {
        x = PEM_read_bio_X509(b, NULL, NULL, NULL);
        BIO_free(b);
    }
    b = BIO_new_mem_buf(key_pem, -1);
    if (b) {
        pk = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
        BIO_free(b);
    }
    if (!x || !pk || X509_check_private_key(x, pk) != 1) {
        X509_free(x);
        EVP_PKEY_free(pk);
        return NULL;
    }
    {
        const ASN1_TIME* na = X509_get0_notAfter(x);
        struct tm tmv;
        double ms = 0;
        if (ASN1_TIME_to_tm(na, &tmv) == 1)
            ms = (double)timegm(&tmv) * 1000.0;
        c = wrt_cert_wrap(pk, x, ms, EVP_PKEY_get_base_id(pk) == EVP_PKEY_RSA);
    }
    if (!c) {
        X509_free(x);
        EVP_PKEY_free(pk);
        return NULL;
    }
    *err = NULL;
    return c;
}

int wrt_cert_pem(const wrt_cert* c, char** cert_pem, char** key_pem)
{
    BIO* b;
    char* p;
    long n;
    *cert_pem = *key_pem = NULL;
    b = BIO_new(BIO_s_mem());
    if (!b)
        return -1;
    PEM_write_bio_X509(b, (X509*)c->x509);
    n = BIO_get_mem_data(b, &p);
    *cert_pem = (char*)malloc((size_t)n + 1);
    if (*cert_pem) {
        memcpy(*cert_pem, p, (size_t)n);
        (*cert_pem)[n] = 0;
    }
    BIO_free(b);
    b = BIO_new(BIO_s_mem());
    if (!b) {
        free(*cert_pem);
        *cert_pem = NULL;
        return -1;
    }
    PEM_write_bio_PrivateKey(b, (EVP_PKEY*)c->pkey, NULL, NULL, 0, NULL, NULL);
    n = BIO_get_mem_data(b, &p);
    *key_pem = (char*)malloc((size_t)n + 1);
    if (*key_pem) {
        memcpy(*key_pem, p, (size_t)n);
        (*key_pem)[n] = 0;
    }
    BIO_free(b);
    if (*cert_pem && *key_pem)
        return 0;
    free(*cert_pem);
    free(*key_pem);
    *cert_pem = *key_pem = NULL;
    return -1;
}

void wrt_cert_ref(wrt_cert* c)
{
    if (c)
        c->refs++;
}

void wrt_cert_unref(wrt_cert* c)
{
    wrt_cert_full* f = (wrt_cert_full*)c;
    if (!c || --c->refs > 0)
        return;
    SSL_CTX_free(f->ctx);
    X509_free((X509*)c->x509);
    EVP_PKEY_free((EVP_PKEY*)c->pkey);
    free(f);
}

size_t wrt_cert_fp_text(const wrt_cert* c, char* out, size_t cap)
{
    return rtc_hex_upper_colon(c->fp, 32, out, cap);
}

int wrt_dtls_init(wrt_dtls* d, wrt_pc* pc, wrt_cert* cert, int is_server)
{
    SSL* ssl;
    BIO* b;
    pthread_once(&wrt_bio_once, wrt_bio_setup);
    if (!wrt_bio_meth)
        return -1;
    ssl = SSL_new(((wrt_cert_full*)cert)->ctx);
    if (!ssl)
        return -1;
    b = BIO_new(wrt_bio_meth);
    if (!b) {
        SSL_free(ssl);
        return -1;
    }
    BIO_set_data(b, d);
    SSL_set_bio(ssl, b, b);
    SSL_set_app_data(ssl, d);
    SSL_set_mtu(ssl, WRT_DTLS_MTU);
    DTLS_set_link_mtu(ssl, WRT_DTLS_MTU);
    d->ssl = ssl;
    d->bio = b;
    d->pc = pc;
    d->is_server = is_server;
    d->state = WRT_DTLS_NEW;
    if (is_server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);
    return 0;
}

void wrt_dtls_free(wrt_dtls* d)
{
    if (d->ssl)
        SSL_free((SSL*)d->ssl);
    d->ssl = NULL;
    d->bio = NULL;
}

static void wrt_dtls_set_state(wrt_dtls* d, int st)
{
    if (d->state == st)
        return;
    d->state = st;
    wrt_pc_dtls_state(d->pc);
}

static int wrt_fp_matches(const wrt_dtls* d, X509* peer)
{
    const EVP_MD* md;
    uint8_t buf[64];
    unsigned int len = 0;
    if (!peer)
        return 0;
    switch (d->remote_fp_alg) {
    case RTC_FP_SHA1:
        md = EVP_sha1();
        break;
    case RTC_FP_SHA224:
        md = EVP_sha224();
        break;
    case RTC_FP_SHA256:
        md = EVP_sha256();
        break;
    case RTC_FP_SHA384:
        md = EVP_sha384();
        break;
    case RTC_FP_SHA512:
        md = EVP_sha512();
        break;
    case RTC_FP_NONE:
    default:
        md = NULL;
        break;
    }
    return md && X509_digest(peer, md, buf, &len) == 1 && len == d->remote_fp_len && CRYPTO_memcmp(buf, d->remote_fp, len) == 0;
}

static int wrt_dtls_check_peer(wrt_dtls* d)
{
    X509* peer = SSL_get1_peer_certificate((SSL*)d->ssl);
    int ok = wrt_fp_matches(d, peer);
    X509_free(peer);
    return ok;
}

static void wrt_dtls_on_connected(wrt_dtls* d)
{
    SSL* ssl = (SSL*)d->ssl;
    SRTP_PROTECTION_PROFILE* prof;
    if (!wrt_dtls_check_peer(d)) {
        d->fp_failed = 1;
        wrt_dtls_set_state(d, WRT_DTLS_FAILED);
        return;
    }
    prof = SSL_get_selected_srtp_profile(ssl);
    if (prof) {
        size_t kl = 0, sl = 0;
        uint32_t klen;
        if (wrt_srtp_profile_lengths((int)prof->id, &kl, &sl) < 0)
            kl = sl = 0;
        klen = (uint32_t)(2 * (kl + sl));
        d->srtp_profile = (uint8_t)prof->id;
        if (klen && SSL_export_keying_material(ssl, d->keys, klen, "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) == 1)
            d->keys_len = klen;
    }
    wrt_dtls_set_state(d, WRT_DTLS_CONNECTED);
}

void wrt_dtls_start(wrt_dtls* d)
{
    int r;
    if (!d->ssl || d->state != WRT_DTLS_NEW)
        return;
    wrt_dtls_set_state(d, WRT_DTLS_CONNECTING);
    if (d->is_server)
        return;
    r = SSL_do_handshake((SSL*)d->ssl);
    if (r != 1) {
        int e = SSL_get_error((SSL*)d->ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            wrt_dtls_set_state(d, WRT_DTLS_FAILED);
    }
    ERR_clear_error();
}

static _Thread_local uint8_t wrt_rec[16384 + 2048];

int wrt_dtls_input(wrt_dtls* d, const uint8_t* p, size_t n)
{
    SSL* ssl = (SSL*)d->ssl;
    if (!ssl || d->state == WRT_DTLS_FAILED || d->state == WRT_DTLS_CLOSED)
        return -1;
    d->in_p = p;
    d->in_n = n;
    if (d->state == WRT_DTLS_NEW && d->is_server)
        wrt_dtls_set_state(d, WRT_DTLS_CONNECTING);
    if (d->state == WRT_DTLS_CONNECTING) {
        int r = SSL_do_handshake(ssl);
        if (r == 1)
            wrt_dtls_on_connected(d);
        else {
            int e = SSL_get_error(ssl, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                d->in_n = 0;
                ERR_clear_error();
                wrt_dtls_set_state(d, WRT_DTLS_FAILED);
                return -1;
            }
        }
    }
    while (d->state == WRT_DTLS_CONNECTED) {
        int r = SSL_read(ssl, wrt_rec, (int)sizeof(wrt_rec));
        if (r > 0) {
            wrt_pc_dtls_data(d->pc, wrt_rec, (size_t)r);
            if (SSL_has_pending(ssl))
                continue;
            break;
        }
        {
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN)
                wrt_dtls_set_state(d, WRT_DTLS_CLOSED);
            else if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_SYSCALL)
                wrt_dtls_set_state(d, WRT_DTLS_FAILED);
        }
        break;
    }
    d->in_n = 0;
    ERR_clear_error();
    return 0;
}

int wrt_dtls_write(wrt_dtls* d, const uint8_t* p, size_t n)
{
    int r;
    if (!d->ssl || d->state != WRT_DTLS_CONNECTED)
        return -1;
    r = SSL_write((SSL*)d->ssl, p, (int)n);
    if (r <= 0) {
        ERR_clear_error();
        return -1;
    }
    return 0;
}

uint64_t wrt_dtls_deadline(wrt_dtls* d, uint64_t now)
{
    struct timeval tv;
    if (!d->ssl || d->state != WRT_DTLS_CONNECTING)
        return UINT64_MAX;
    if (DTLSv1_get_timeout((SSL*)d->ssl, &tv) != 1)
        return UINT64_MAX;
    return now + (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

void wrt_dtls_timeout(wrt_dtls* d)
{
    if (!d->ssl || d->state != WRT_DTLS_CONNECTING)
        return;
    if (DTLSv1_handle_timeout((SSL*)d->ssl) < 0)
        wrt_dtls_set_state(d, WRT_DTLS_FAILED);
    ERR_clear_error();
}

void wrt_dtls_close(wrt_dtls* d)
{
    if (d->ssl && d->state == WRT_DTLS_CONNECTED) {
        SSL_shutdown((SSL*)d->ssl);
        ERR_clear_error();
    }
    if (d->state != WRT_DTLS_FAILED)
        d->state = WRT_DTLS_CLOSED;
}

const char* wrt_dtls_cipher(const wrt_dtls* d)
{
    const SSL_CIPHER* c;
    if (!d->ssl || d->state != WRT_DTLS_CONNECTED)
        return NULL;
    c = SSL_get_current_cipher((SSL*)d->ssl);
    return c ? SSL_CIPHER_standard_name(c) : NULL;
}

const char* wrt_dtls_srtp_name(const wrt_dtls* d)
{
    if (d->srtp_profile == SRTP_AEAD_AES_128_GCM)
        return "AEAD_AES_128_GCM";
    if (d->srtp_profile == SRTP_AES128_CM_SHA1_80)
        return "AES_CM_128_HMAC_SHA1_80";
    if (d->srtp_profile == SRTP_AEAD_AES_256_GCM)
        return "AEAD_AES_256_GCM";
    if (d->srtp_profile == SRTP_AES128_CM_SHA1_32)
        return "AES_CM_128_HMAC_SHA1_32";
    return NULL;
}

int wrt_dtls_peer_fp(const wrt_dtls* d, uint8_t out[32])
{
    X509* peer;
    unsigned int len = 0;
    if (!d->ssl)
        return 0;
    peer = SSL_get1_peer_certificate((SSL*)d->ssl);
    if (!peer)
        return 0;
    X509_digest(peer, EVP_sha256(), out, &len);
    X509_free(peer);
    return len == 32;
}

int wrt_dtls_peer_der(const wrt_dtls* d, uint8_t** out)
{
    X509* peer;
    unsigned char* buf = NULL;
    int n;
    *out = NULL;
    if (!d->ssl)
        return 0;
    peer = SSL_get1_peer_certificate((SSL*)d->ssl);
    if (!peer)
        return 0;
    n = i2d_X509(peer, &buf);
    X509_free(peer);
    if (n <= 0)
        return 0;
    *out = (uint8_t*)buf;
    return n;
}

void wrt_dtls_der_free(uint8_t* p)
{
    OPENSSL_free(p);
}

#endif
