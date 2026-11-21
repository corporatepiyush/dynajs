#ifndef SSH_INTERNAL_H
#define SSH_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

#include "dyna-aio.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define SSH_MSG_DISCONNECT 1
#define SSH_MSG_IGNORE 2
#define SSH_MSG_UNIMPLEMENTED 3
#define SSH_MSG_DEBUG 4
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT 6
#define SSH_MSG_EXT_INFO 7
#define SSH_MSG_KEXINIT 20
#define SSH_MSG_NEWKEYS 21
#define SSH_MSG_KEX_INIT 30
#define SSH_MSG_KEX_REPLY 31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_USERAUTH_BANNER 53
#define SSH_MSG_USERAUTH_PK_OK 60
#define SSH_MSG_GLOBAL_REQUEST 80
#define SSH_MSG_REQUEST_SUCCESS 81
#define SSH_MSG_REQUEST_FAILURE 82
#define SSH_MSG_CHANNEL_OPEN 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA 94
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_MSG_CHANNEL_EOF 96
#define SSH_MSG_CHANNEL_CLOSE 97
#define SSH_MSG_CHANNEL_REQUEST 98
#define SSH_MSG_CHANNEL_SUCCESS 99
#define SSH_MSG_CHANNEL_FAILURE 100

#define SSH_DISC_PROTOCOL_ERROR 2
#define SSH_DISC_SERVICE_NOT_AVAILABLE 7
#define SSH_DISC_TOO_MANY_CONNECTIONS 12
#define SSH_DISC_ILLEGAL_USER_NAME 15
#define SSH_DISC_KEY_EXCHANGE_FAILED 3
#define SSH_DISC_MAC_ERROR 5
#define SSH_DISC_HOST_KEY_NOT_VERIFIABLE 9
#define SSH_DISC_BY_APPLICATION 11
#define SSH_DISC_NO_MORE_AUTH_METHODS 14

enum {
    SSH_KEX_MLKEM768X25519 = 0,
    SSH_KEX_SNTRUP761X25519,
    SSH_KEX_CURVE25519,
    SSH_KEX_N
};

enum {
    SSH_HK_ED25519 = 0,
    SSH_HK_SK_ED25519,
    SSH_HK_RSA_SHA2_512,
    SSH_HK_ED25519_CERT,
    SSH_HK_N
};

enum {
    SSH_CIPHER_NONE = 0,
    SSH_CIPHER_CHACHAPOLY,
    SSH_CIPHER_AES256GCM
};

enum {
    SSH_ST_VERSION = 0,
    SSH_ST_KEX,
    SSH_ST_SERVICE,
    SSH_ST_AUTH,
    SSH_ST_READY,
    SSH_ST_DEAD
};

enum {
    SSH_CH_OPENING = 0,
    SSH_CH_REQUESTING,
    SSH_CH_OPEN,
    SSH_CH_CLOSED,
    SSH_CH_PENDING
};

#define SSH_TRIM_CAP (256u << 10)

typedef struct {
    uint8_t* p;
    size_t len, cap;
} sshb_t;

typedef struct {
    const uint8_t* p;
    size_t len, off;
    int err;
} sshr_t;

typedef struct {
    int kind;
    int enc;
    EVP_CIPHER_CTX* main;
    EVP_CIPHER_CTX* hdr;
    EVP_MAC_CTX* mac;
    uint8_t iv[12];
    unsigned bs, tag;
} ssh_aead_t;

typedef struct ssh_conn ssh_conn_t;

typedef struct {
    uint64_t mark;
    int which;
    JSValue resolve;
} ssh_wwait_t;

typedef struct ssh_srv ssh_srv_t;

struct ssh_chan;

typedef struct ssh_chan_ops {
    void (*opened)(struct ssh_chan* ch, const char* err);
    void (*data)(struct ssh_chan* ch, int which, const uint8_t* p, size_t n);
    void (*eof)(struct ssh_chan* ch);
    void (*written)(struct ssh_chan* ch);
    void (*closed)(struct ssh_chan* ch, const char* err);
    void (*release)(JSRuntime* rt, struct ssh_chan* ch);
} ssh_chan_ops_t;

typedef struct ssh_chan {
    ssh_conn_t* conn;
    int refs;
    int state;
    uint32_t lid, rid;
    uint64_t lwin;
    uint64_t win_size;
    uint32_t lmax;
    uint64_t rwin;
    uint32_t rmax;
    int sent_eof, sent_close, got_eof, got_close;
    int want_eof;
    int want_close;
    char* cmd;
    size_t cmdlen;
    JSValue jsobj;
    int js_held;
    int js_gone;
    JSValue ctl[2];
    int rs_done[2];
    uint64_t queued[2];
    JSValue open_res, open_rej;
    JSValue exit_res;
    int has_status;
    int exit_code;
    char* exit_signal;
    int exit_core;
    char* exit_msg;
    sshb_t inq;
    size_t inq_off;
    uint64_t in_total, in_sent;
    ssh_wwait_t* ww;
    int nww, cww;
    int srv;
    int started;
    int want_exit;
    int exit_kind;
    char* term;
    uint32_t cols, rows, width, height;
    sshb_t modes;
    int has_pty;
    sshb_t env;
    int nenv;
    sshb_t errq;
    size_t errq_off;
    uint64_t err_total, err_sent;
    const ssh_chan_ops_t* ops;
    void* opq;
    int subsys;
    int got_data;
    const char* ctype;
    sshb_t oextra;
    int fwd;
    uint64_t ftoken;
} ssh_chan_t;

typedef struct ssh_ident {
    int kind;
    sshb_t blob;
} ssh_ident_t;

typedef struct {
    int accept_new;
    int allow_rsa;
    uint32_t max_packet;
    uint32_t window;
    uint32_t chan_packet;
    int max_channels;
    uint64_t handshake_ms;
    uint64_t rekey_bytes;
    uint64_t rekey_ms;
    int max_auth_tries;
    int max_conns;
    uint64_t idle_ms;
    int shrink;
    int public_binds;
} ssh_cfg_t;

struct ssh_conn {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    int state;
    int busy;
    int closing;
    int released;
    int hooked;
    int pinned;
    int swept;
    JSValue self_weak;
    char* err;
    ssh_cfg_t cfg;
    char* host;
    uint16_t port;
    char* user;
    char* known_hosts;
    EVP_PKEY* ukey;
    uint8_t upub[32];
    char vc[256];
    char vs[256];
    sshb_t rbuf;
    size_t rpos;
    sshb_t obuf;
    sshb_t msg;
    sshb_t pend;
    uint32_t seq_in, seq_out;
    uint32_t cur_len;
    int have_len;
    ssh_aead_t ain, aout, nin, nout;
    int kex_active, kex_done_once, kexinit_sent, kexinit_recv, reply_done, newkeys_sent;
    int strict, skip_next;
    sshb_t ic, is;
    int kex_alg, hk_alg, cip_cs, cip_sc;
    EVP_PKEY* xkey;
    EVP_PKEY* mkey;
    uint8_t qc[1216];
    uint8_t* ssk;
    size_t qclen;
    uint8_t sid[64];
    size_t sidlen;
    sshb_t hk0;
    uint64_t kex_bytes;
    uint64_t kex_at_ms;
    uint64_t deadline_ms;
    char* banner;
    char* server_sig_algs;
    char hk_type[40];
    char hk_fp[64];
    ssh_chan_t** ch;
    int nch;
    int nopen;
    uint64_t bytes_in, bytes_out, pk_in, pk_out;
    int rekeys;
    JSValue conn_res, conn_rej;
    int in_final;
    int merr;
    int tcp_up;
    uint32_t pre_bytes;
    int server;
    ssh_srv_t* srv;
    ssh_conn_t* snext;
    ssh_conn_t* sprev;
    uint64_t cid;
    char peer[64];
    int ext_info_c;
    int auth_tries;
    uint32_t auth_seq;
    int auth_pending;
    int srv_hook;
    int srv_session;
    char* force_cmd;
    int cert_pty;
    sshb_t ucert;
    char ufp[64];
    char ukt[48];
    int offer_hk_cert;
    size_t hlen;
    uint64_t last_io_ms;
    int banner_sent;
    int cert_fwd;
    int fwd_hook;
    struct ssh_fwd* fwds;
    struct ssh_lis* lis;
    struct ssh_relay* lingers;
    int nlinger;
    int nlis;
    struct ssh_greq* gq;
    int ngq, cgq;
    struct ssh_fwd** gw;
    int ngw, cgw;
    ssh_conn_t* via;
    ssh_chan_t* via_ch;
    JSValue via_obj;
    char* agent_path;
    struct ssh_agc* agc;
    ssh_ident_t* ids;
    int nids, idi;
    int agent_phase;
    int tried;
};

struct ssh_srv {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int listen_fd;
    uint16_t port;
    char* host;
    int closed;
    int released;
    int pinned;
    int swept;
    int busy;
    int dead;
    int hooked;
    JSValue self_weak;
    JSValue authorize;
    JSValue on_session;
    JSValue forward;
    EVP_PKEY* hkey;
    uint8_t hpub[32];
    sshb_t hblob;
    sshb_t hcert;
    sshb_t akeys;
    sshb_t cakeys;
    char* banner;
    ssh_cfg_t cfg;
    ssh_conn_t* conns;
    int nconns;
    uint64_t next_cid;
    uint64_t accepted, refused;
};

void sshb_free(sshb_t* b);
int sshb_reserve(sshb_t* b, size_t extra);
int sshb_put(sshb_t* b, const void* p, size_t n);
int sshb_u8(sshb_t* b, uint8_t v);
int sshb_u32(sshb_t* b, uint32_t v);
int sshb_str(sshb_t* b, const void* p, size_t n);
int sshb_cstr(sshb_t* b, const char* s);
int sshb_mpint(sshb_t* b, const uint8_t* p, size_t n);
void sshr_init(sshr_t* r, const uint8_t* p, size_t n);
uint8_t sshr_u8(sshr_t* r);
uint32_t sshr_u32(sshr_t* r);
const uint8_t* sshr_str(sshr_t* r, size_t* n);
int sshr_bool(sshr_t* r);
int ssh_namelist_has(const uint8_t* list, size_t n, const char* name);
uint32_t ssh_be32(const uint8_t* p);
void ssh_put_be32(uint8_t* p, uint32_t v);

int ssh_aead_init(ssh_aead_t* a, int kind, int enc, const uint8_t* key,
    const uint8_t* iv);
void ssh_aead_free(ssh_aead_t* a);
int ssh_aead_len(ssh_aead_t* a, uint32_t seq, const uint8_t* p, uint32_t* len);
int ssh_aead_seal(ssh_aead_t* a, uint32_t seq, uint8_t* p, size_t plen);
int ssh_aead_open(ssh_aead_t* a, uint32_t seq, uint8_t* p, size_t plen);
unsigned ssh_cipher_keylen(int kind);
unsigned ssh_cipher_ivlen(int kind);

int ssh_hash(int sha512, const void* p, size_t n, uint8_t* out);
int ssh_hash2(int sha512, const void* a, size_t an, const void* b, size_t bn,
    uint8_t* out);
EVP_PKEY* ssh_x25519_new(uint8_t pub[32]);
int ssh_x25519_derive(EVP_PKEY* k, const uint8_t peer[32], uint8_t out[32]);
EVP_PKEY* ssh_mlkem_new(uint8_t pub[1184]);
int ssh_mlkem_decap(EVP_PKEY* k, const uint8_t ct[1088], uint8_t out[32]);
int ssh_verify_hostsig(int hk_alg, const uint8_t* ks, size_t kslen,
    const uint8_t* sig, size_t siglen, const uint8_t* data, size_t dlen,
    char* why, size_t whylen);
int ssh_ed25519_sign(EVP_PKEY* k, const uint8_t* data, size_t n, uint8_t sig[64]);
void ssh_fingerprint(const uint8_t* blob, size_t n, char out[64]);
size_t ssh_b64_decode(const char* s, size_t n, uint8_t* out, size_t cap);
size_t ssh_b64_encode(const uint8_t* p, size_t n, char* out, int pad);
void ssh_random(void* p, size_t n);

EVP_PKEY* ssh_parse_private_key(const uint8_t* text, size_t n,
    const char* pass, size_t passlen, uint8_t pub[32], char* why, size_t whylen);

enum {
    SSH_KH_OK = 0,
    SSH_KH_UNKNOWN,
    SSH_KH_CHANGED,
    SSH_KH_REVOKED,
    SSH_KH_ERROR
};
int ssh_known_hosts_check(const char* path, const char* host, uint16_t port,
    const char* type, const uint8_t* blob, size_t bloblen);
int ssh_known_hosts_add(const char* path, const char* host, uint16_t port,
    const char* type, const uint8_t* blob, size_t bloblen);

int ssh_start(ssh_conn_t* c);
void ssh_input(ssh_conn_t* c, const uint8_t* buf, size_t len);
void ssh_flush(ssh_conn_t* c);
void ssh_fail(ssh_conn_t* c, int reason, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void ssh_tick(ssh_conn_t* c, uint64_t now);
int ssh_start_rekey(ssh_conn_t* c);
int ssh_send_msg(ssh_conn_t* c);
void ssh_msg_begin(ssh_conn_t* c, uint8_t type);
int ssh_chan_open(ssh_conn_t* c, ssh_chan_t* ch);
void ssh_chan_window(ssh_chan_t* ch);
void ssh_chan_pump(ssh_chan_t* ch);
void ssh_chan_close(ssh_chan_t* ch);
void ssh_chan_unref(JSRuntime* rt, ssh_chan_t* ch);
void ssh_conn_free_kex(ssh_conn_t* c);

void ssh_js_connected(ssh_conn_t* c);
void ssh_js_died(ssh_conn_t* c);
void ssh_js_chan_opened(ssh_chan_t* ch, const char* err);
void ssh_js_chan_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n);
void ssh_js_chan_eof(ssh_chan_t* ch);
void ssh_js_chan_closed(ssh_chan_t* ch, const char* err);
void ssh_js_chan_written(ssh_chan_t* ch);
uint64_t ssh_now_ms(void);

#define SSH_SNTRUP_PK 1158
#define SSH_SNTRUP_SK 1763
#define SSH_SNTRUP_CT 1039
int ssh_sntrup_keypair(uint8_t pk[SSH_SNTRUP_PK], uint8_t sk[SSH_SNTRUP_SK]);
int ssh_sntrup_enc(uint8_t ct[SSH_SNTRUP_CT], uint8_t ss[32], const uint8_t pk[SSH_SNTRUP_PK]);
int ssh_sntrup_dec(uint8_t ss[32], const uint8_t ct[SSH_SNTRUP_CT], const uint8_t sk[SSH_SNTRUP_SK]);
int ssh_mlkem_encap(const uint8_t pk[1184], uint8_t ct[1088], uint8_t ss[32]);
int ssh_ed25519_verify(const uint8_t pk[32], const uint8_t* sig, size_t siglen,
    const uint8_t* data, size_t n);
int ssh_verify_usersig(const uint8_t* blob, size_t bloblen, const uint8_t* sig,
    size_t siglen, const uint8_t* data, size_t dlen, char* why, size_t whylen);

#define SSH_CERT_USER 1
#define SSH_CERT_HOST 2

typedef struct {
    uint8_t pk[32];
    uint64_t serial;
    uint32_t type;
    const uint8_t* key_id;
    size_t key_id_len;
    const uint8_t* principals;
    size_t principals_len;
    uint64_t valid_after, valid_before;
    const uint8_t* crit;
    size_t crit_len;
    const uint8_t* ext;
    size_t ext_len;
    const uint8_t* ca;
    size_t ca_len;
} ssh_cert_t;

int ssh_cert_parse(const uint8_t* blob, size_t n, ssh_cert_t* c, char* why, size_t whylen);
int ssh_cert_check(const ssh_cert_t* c, uint32_t type, const char* principal,
    const char* peer, char** force_cmd, int* permit_pty, char* why, size_t whylen);
int ssh_cert_ext_has(const ssh_cert_t* c, const char* name);
void ssh_ed25519_blob(sshb_t* b, const uint8_t pk[32]);
int ssh_blob_list_has(const sshb_t* list, const uint8_t* blob, size_t n);
int ssh_parse_pubkey_lines(const char* text, size_t n, sshb_t* list, char* why, size_t whylen);
int ssh_parse_pubkey(const char* text, size_t n, sshb_t* out, char* type, size_t typecap);

int ssh_known_hosts_has_ca(const char* path, const char* host, uint16_t port);
int ssh_known_hosts_check_ca(const char* path, const char* host, uint16_t port,
    const uint8_t* ca, size_t calen, const uint8_t* key, size_t keylen);

void ssh_m_u8(ssh_conn_t* c, uint8_t v);
void ssh_m_u32(ssh_conn_t* c, uint32_t v);
void ssh_m_str(ssh_conn_t* c, const void* p, size_t n);
void ssh_m_cstr(ssh_conn_t* c, const char* s);
int ssh_frame(ssh_conn_t* c, const uint8_t* p, size_t n);
int ssh_derive(const sshb_t* k, const uint8_t* h, size_t hl, const uint8_t* sid, size_t sidl,
    char letter, uint8_t* out, size_t need);
int ssh_kex_hash(ssh_conn_t* c, const uint8_t* ks, size_t kn, const uint8_t* qc, size_t qcn,
    const uint8_t* qs, size_t qsn, const sshb_t* k, uint8_t* h);
int ssh_kex_keys(ssh_conn_t* c, const sshb_t* k, const uint8_t* h);
int ssh_newkeys_out(ssh_conn_t* c);
void ssh_chan_finish(ssh_conn_t* c, ssh_chan_t* ch, const char* err);
void ssh_chan_send_close(ssh_chan_t* ch);
int ssh_chan_slot(ssh_conn_t* c, ssh_chan_t* ch);
const char* ssh_kex_name(int id);
const char* ssh_hk_name(int id);

int ssh_srv_kex_init(ssh_conn_t* c, sshr_t* r);
int ssh_srv_service(ssh_conn_t* c, sshr_t* r);
int ssh_srv_userauth(ssh_conn_t* c, sshr_t* r);
void ssh_srv_auth_done(ssh_conn_t* c, uint32_t seq, int accept);
int ssh_srv_chan_open(ssh_conn_t* c, sshr_t* r);
int ssh_srv_chan_request(ssh_conn_t* c, ssh_chan_t* ch, sshr_t* r);
void ssh_srv_send_ext_info(ssh_conn_t* c);

int ssh_js_authorize(ssh_conn_t* c, const char* user, const char* alg, const uint8_t* key,
    size_t keylen, const uint8_t* certblob, size_t certlen, int listed);
int ssh_js_session(ssh_conn_t* c, ssh_chan_t* ch, const char* kind, const uint8_t* cmd, size_t cmdlen);
void ssh_js_signal(ssh_chan_t* ch, const char* name);
void ssh_js_resize(ssh_chan_t* ch);
ssh_conn_t* ssh_client_of(JSContext* ctx, JSValueConst v);
ssh_chan_t* ssh_session_of(JSValueConst v);
ssh_chan_t* ssh_subsys_open(ssh_conn_t* c, const char* name, const ssh_chan_ops_t* ops, void* opq, JSValueConst holder);
void ssh_chan_kick(ssh_chan_t* ch);
int ssh_js_off(void);
int ssh_then2(JSContext* ctx, JSValueConst p, JSCFunctionData* fn, int nd, JSValueConst* d);

#define SSH_MAX_REMOTE_FWD 16
#define SSH_AGENT_MAX_MSG (256u << 10)

typedef struct ssh_akey {
    int kind;
    sshb_t blob;
    EVP_PKEY* k;
    char* comment;
    uint64_t expires_ms;
    struct ssh_akey* next;
} ssh_akey_t;

typedef struct ssh_agent_store {
    ssh_akey_t* keys;
    ssh_akey_t* tail;
    int nkeys;
    int max_keys;
    int locked;
    uint8_t lsalt[16];
    uint8_t lhash[64];
    uint64_t requests, signs, refused;
} ssh_agent_store_t;

typedef struct ssh_agent_srv {
    dyn_aio_t* aio;
    int lfd;
    char* path;
    ssh_agent_store_t st;
    struct ssh_agconn* conns;
    int nconns, max_conns;
    int closed;
    uint64_t accepted, refused_conns, offloaded;
    int live;
    struct ssh_agent_srv* live_next;
    struct ssh_agent_srv* live_prev;
} ssh_agent_srv_t;

int ssh_agent_blob_kind(const uint8_t* blob, size_t n);
void ssh_agent_store_clear(ssh_agent_store_t* st);
void ssh_agent_store_expire(ssh_agent_store_t* st, uint64_t now);
int ssh_agent_store_put(ssh_agent_store_t* st, ssh_akey_t* k);
int ssh_agent_store_remove(ssh_agent_store_t* st, const uint8_t* blob, size_t n);
ssh_akey_t* ssh_agent_key_from(int kind, EVP_PKEY* pk, const uint8_t* blob, size_t bn, const char* comment, size_t cn);
int ssh_agent_handle(ssh_agent_store_t* st, const uint8_t* req, size_t n, uint64_t now, sshb_t* out);
int ssh_agent_srv_listen(ssh_agent_srv_t* a, const char* path, char* why, size_t whylen);
void ssh_agent_srv_close(ssh_agent_srv_t* a);
int ssh_auth_begin(ssh_conn_t* c);
int ssh_auth_failure(ssh_conn_t* c, const uint8_t* m, size_t mn);
int ssh_auth_pk_ok(ssh_conn_t* c, sshr_t* r);
void ssh_auth_drop(ssh_conn_t* c);

typedef struct ssh_fwd {
    ssh_conn_t* c;
    JSContext* ctx;
    JSRuntime* rt;
    int kind;
    int state;
    int lfd;
    char* lhost;
    uint16_t lport;
    char* rhost;
    uint16_t rport;
    uint64_t conns;
    JSValue obj;
    JSValue res, rej;
    struct ssh_fwd* next;
} ssh_fwd_t;

void ssh_conn_enter(ssh_conn_t* c);
void ssh_conn_leave(ssh_conn_t* c);
JSValue ssh_js_error(JSContext* ctx, const char* msg);
int ssh_js_forward(ssh_conn_t* c, const char* type, const char* host, uint32_t port, const char* ohost, uint32_t oport, uint64_t token);
void ssh_fwd_decided(ssh_conn_t* c, uint64_t token, int ok);
int ssh_fwd_open_in(ssh_conn_t* c, uint32_t sender, uint32_t win, uint32_t maxp, sshr_t* r);
int ssh_fwd_global(ssh_conn_t* c, const uint8_t* name, size_t nn, int want, sshr_t* r);
int ssh_fwd_reply(ssh_conn_t* c, int ok, sshr_t* r);
void ssh_fwd_conn_dead(ssh_conn_t* c);
void ssh_fwd_conn_free(ssh_conn_t* c);
int ssh_fwd_listen_local(ssh_conn_t* c, ssh_fwd_t* f, char* why, size_t whylen);
int ssh_fwd_request_remote(ssh_conn_t* c, ssh_fwd_t* f);
void ssh_fwd_close(ssh_fwd_t* f);
void ssh_fwd_settle(ssh_fwd_t* f, const char* err);
void ssh_fwd_drop(ssh_fwd_t* f);
int ssh_via_open(ssh_conn_t* c, ssh_conn_t* outer);
void ssh_via_flush(ssh_conn_t* c);
void ssh_via_detach(ssh_conn_t* c);
int ssh_chan_ready_in(ssh_chan_t* ch);

#endif
#endif
