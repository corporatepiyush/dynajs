#ifndef DYN_MAILK_H
#define DYN_MAILK_H

#include <stddef.h>
#include <stdint.h>
#include "dyn-sb.h"

enum {
    MAILK_OK = 0,
    MAILK_NEED = 1,
    MAILK_EBAD = -1,
    MAILK_ELONG = -2,
    MAILK_ENOMEM = -3,
    MAILK_EBARE = -4
};

#define MAILK_F_UTF8 1u
#define MAILK_F_LENIENT 2u
#define MAILK_F_STRICT 4u

#define MAILK_LINE_MAX_DEFAULT 2048u
#define MAILK_REPLY_LINE_MAX 4096u
#define MAILK_REPLY_LINES_MAX 512u
#define MAILK_LOCAL_MAX 64u
#define MAILK_DOMAIN_MAX 255u
#define MAILK_LABEL_MAX 63u
#define MAILK_PATH_MAX 256u
#define MAILK_TEXT_LINE_MAX 998u

typedef struct {
    dyn_sb_t buf;
    size_t max;
    int cr;
} mailk_line_t;

void mailk_line_init(mailk_line_t* l, size_t max);
void mailk_line_free(mailk_line_t* l);
void mailk_line_reset(mailk_line_t* l);
int mailk_line_feed(mailk_line_t* l, const uint8_t* p, size_t n, size_t* used);

typedef struct {
    int code;
    int esc_class, esc_subject, esc_detail;
    uint32_t nlines;
    int done;
    dyn_sb_t text;
    mailk_line_t line;
} mailk_reply_t;

void mailk_reply_init(mailk_reply_t* r);
void mailk_reply_free(mailk_reply_t* r);
void mailk_reply_reset(mailk_reply_t* r);
int mailk_reply_feed(mailk_reply_t* r, const uint8_t* p, size_t n, size_t* used);
int mailk_esc_parse(const char* s, size_t n, int* cls, int* subject, int* detail, size_t* used);

typedef struct {
    const char* kw;
    size_t kwn;
    const char* params;
    size_t paramsn;
} mailk_ehlo_ext_t;

int mailk_ehlo_next(const char* text, size_t n, size_t* pos, mailk_ehlo_ext_t* out);
int mailk_keyword_ok(const char* s, size_t n);

enum {
    MAILK_VERB_UNKNOWN = 0,
    MAILK_VERB_HELO,
    MAILK_VERB_EHLO,
    MAILK_VERB_MAIL,
    MAILK_VERB_RCPT,
    MAILK_VERB_DATA,
    MAILK_VERB_BDAT,
    MAILK_VERB_RSET,
    MAILK_VERB_NOOP,
    MAILK_VERB_QUIT,
    MAILK_VERB_VRFY,
    MAILK_VERB_EXPN,
    MAILK_VERB_HELP,
    MAILK_VERB_STARTTLS,
    MAILK_VERB_AUTH
};

typedef struct {
    int verb;
    const char* arg;
    size_t argn;
} mailk_cmd_t;

int mailk_cmd_parse(const char* s, size_t n, mailk_cmd_t* out);

typedef struct {
    size_t lo, ln, dn_off, dn;
    int null_path, postmaster, utf8, quoted, literal;
} mailk_path_t;

typedef struct {
    const char* kw;
    size_t kwn;
    const char* val;
    size_t valn;
} mailk_param_t;

int mailk_mailbox_parse(const char* s, size_t n, unsigned flags, mailk_path_t* out);
int mailk_domain_ok(const char* s, size_t n, unsigned flags);
int mailk_path_parse(const char* s, size_t n, unsigned flags, int rcpt, mailk_path_t* out, size_t* used);
int mailk_mail_parse(const char* arg, size_t n, int rcpt, unsigned flags, mailk_path_t* path, size_t* params_off);
int mailk_param_next(const char* s, size_t n, size_t* pos, unsigned flags, mailk_param_t* out);
int mailk_cmd_path(dyn_sb_t* out, int rcpt, const char* mbox, size_t n, unsigned flags,
    const char* const* params, size_t np);

int mailk_xtext_encode(dyn_sb_t* out, const uint8_t* s, size_t n);
int mailk_xtext_decode(dyn_sb_t* out, const char* s, size_t n);

typedef struct {
    int st;
    unsigned flags;
    uint64_t in, out;
    uint32_t col, maxcol;
    uint32_t fixed_bare;
    int has8bit, hasnul;
} mailk_stuff_t;

void mailk_stuff_init(mailk_stuff_t* s, unsigned flags);
int mailk_stuff_feed(mailk_stuff_t* s, dyn_sb_t* out, const uint8_t* p, size_t n);
int mailk_stuff_end(mailk_stuff_t* s, dyn_sb_t* out);

typedef struct {
    int st;
    unsigned flags;
    uint64_t bytes, max;
    uint32_t bare;
    int over, done;
} mailk_unstuff_t;

void mailk_unstuff_init(mailk_unstuff_t* u, unsigned flags, uint64_t max);
int mailk_unstuff_feed(mailk_unstuff_t* u, dyn_sb_t* out, const uint8_t* p, size_t n, size_t* used);

int mailk_utf8_len(const uint8_t* p, size_t n);

#define MAILK_EAUTH (-5)
#define MAILK_CANCEL 2
#define MAILK_SASL_MAX 16384u
#define MAILK_SASL_FIELD_MAX 255u
#define MAILK_PREP_MAX 1024u
#define MAILK_SCRAM_ITER_MIN 4096u
#define MAILK_SCRAM_ITER_MAX 1000000u

int mailk_b64_decode(const char* s, size_t n, dyn_sb_t* out);
int mailk_b64_encode(dyn_sb_t* out, const uint8_t* p, size_t n);
int mailk_sasl_resp_decode(const char* s, size_t n, dyn_sb_t* out);
int mailk_sasl_resp_encode(dyn_sb_t* out, const uint8_t* p, size_t n);
int mailk_saslprep(const char* s, size_t n, dyn_sb_t* out);
int mailk_saslname_decode(dyn_sb_t* out, const char* s, size_t n);
int mailk_saslname_encode(dyn_sb_t* out, const char* s, size_t n);

typedef struct {
    size_t az, azn, ac, acn, pw, pwn;
} mailk_plain_t;

int mailk_plain_build(dyn_sb_t* out, const char* authzid, const char* authcid, const char* passwd);
int mailk_plain_parse(const uint8_t* m, size_t n, mailk_plain_t* out);
int mailk_login_prompt(const uint8_t* m, size_t n);
int mailk_xoauth2_build(dyn_sb_t* out, const char* user, const char* token);
int mailk_cram_md5(dyn_sb_t* out, const char* user, const uint8_t* secret, size_t secretn,
    const uint8_t* challenge, size_t challengen);

typedef struct {
    size_t az, azn, host, hostn, port, portn, tok, tokn;
} mailk_oauth_t;

int mailk_xoauth2_parse(const uint8_t* m, size_t n, mailk_oauth_t* out);
int mailk_oauthbearer_build(dyn_sb_t* out, const char* authzid, const char* host, unsigned port, const char* token);
int mailk_oauthbearer_parse(const uint8_t* m, size_t n, mailk_oauth_t* out);

enum {
    MAILK_SCRAM_SHA1 = 1,
    MAILK_SCRAM_SHA256 = 2,
    MAILK_SCRAM_SHA512 = 3
};

typedef struct {
    int algo, st, cbflag, plus;
    uint32_t iters, min_iters, max_iters;
    size_t dlen, cnonce_n, bare_n;
    uint8_t stored[64], server[64], ssig[64];
    char* pw;
    size_t pwn;
    dyn_sb_t gs2, msg, user, authzid, nonce, cbname, cbd;
} mailk_scram_t;

void mailk_scram_init(mailk_scram_t* c, int algo);
void mailk_scram_free(mailk_scram_t* c);
int mailk_scram_cb(mailk_scram_t* c, int flag, const char* cbname, const uint8_t* cbdata, size_t cbdlen);
int mailk_scram_client_first(mailk_scram_t* c, const char* authcid, const char* authzid,
    const char* passwd, const char* cnonce, dyn_sb_t* out);
int mailk_scram_client_final(mailk_scram_t* c, const uint8_t* in, size_t n, dyn_sb_t* out);
int mailk_scram_client_verify(mailk_scram_t* c, const uint8_t* in, size_t n);
int mailk_scram_derive(int algo, const char* passwd, const uint8_t* salt, size_t saltn, uint32_t iters,
    uint8_t* stored, uint8_t* server);
int mailk_scram_server_first(mailk_scram_t* s, const uint8_t* in, size_t n);
int mailk_scram_server_challenge(mailk_scram_t* s, const uint8_t* salt, size_t saltn, uint32_t iters,
    const uint8_t* stored, const uint8_t* server, const char* snonce, dyn_sb_t* out);
int mailk_scram_server_final(mailk_scram_t* s, const uint8_t* in, size_t n, dyn_sb_t* out);


enum {
    MAILK_TLS_NONE = 0,
    MAILK_TLS_STARTTLS = 1,
    MAILK_TLS_OPPORTUNISTIC = 2,
    MAILK_TLS_IMPLICIT = 3
};

#define MAILK_CAP_PIPELINING 0x1u
#define MAILK_CAP_8BITMIME 0x2u
#define MAILK_CAP_SMTPUTF8 0x4u
#define MAILK_CAP_CHUNKING 0x8u
#define MAILK_CAP_BINARYMIME 0x10u
#define MAILK_CAP_DSN 0x20u
#define MAILK_CAP_STARTTLS 0x40u
#define MAILK_CAP_SIZE 0x80u
#define MAILK_CAP_ENHANCED 0x100u
#define MAILK_CAP_AUTH 0x200u

#define MAILK_MECH_PLAIN 0x1u
#define MAILK_MECH_LOGIN 0x2u
#define MAILK_MECH_CRAM_MD5 0x4u
#define MAILK_MECH_XOAUTH2 0x8u
#define MAILK_MECH_OAUTHBEARER 0x10u
#define MAILK_MECH_SCRAM_SHA1 0x20u
#define MAILK_MECH_SCRAM_SHA256 0x40u
#define MAILK_MECH_SCRAM_SHA512 0x80u
#define MAILK_MECH_DEFAULT 0xFBu

#define MAILK_EV_READY 0x1u
#define MAILK_EV_TLS 0x2u
#define MAILK_EV_DONE 0x4u
#define MAILK_EV_ERROR 0x8u
#define MAILK_EV_CLOSED 0x10u

#define MAILK_RCPT_MAX 1000u

typedef struct {
    int code, cls, sub, det;
    size_t off, len;
} mailk_res_t;

typedef struct {
    const char* name;
    int tls;
    unsigned mechs;
    int insecure_auth;
    const char *user, *pass, *token, *authzid;
    uint32_t scram_max_iters;
} mailk_cli_opts_t;

typedef struct {
    const char* from;
    const char* const* to;
    size_t nto;
    const uint8_t* body;
    size_t bodyn;
    int bdat;
    const char* ret;
    const char* envid;
    const char* notify;
    int orcpt;
} mailk_msg_t;

typedef struct {
    int st, tls_mode, tls_on, insecure_auth, sub, mech, helo, failed, ok, bdat;
    unsigned caps, auth_mechs, mechs, ev;
    uint64_t size_max;
    uint32_t scram_max_iters;
    char *name, *user, *pass, *token, *authzid;
    mailk_reply_t r;
    mailk_scram_t scram;
    dyn_sb_t out, texts, err, body, cmd;
    uint8_t* q;
    size_t qh, qn, qcap, nrcpt, ri, cpos;
    mailk_res_t mail, final, last, *rcpt;
} mailk_cli_t;

int mailk_cli_init(mailk_cli_t* c, const mailk_cli_opts_t* o);
void mailk_cli_free(mailk_cli_t* c);
int mailk_cli_feed(mailk_cli_t* c, const uint8_t* p, size_t n);
int mailk_cli_tls_done(mailk_cli_t* c);
int mailk_cli_send(mailk_cli_t* c, const mailk_msg_t* m);
int mailk_cli_quit(mailk_cli_t* c);
int mailk_cli_rset(mailk_cli_t* c);


#define MAILK_SEV_TLS 0x1u
#define MAILK_SEV_CLOSE 0x2u
#define MAILK_SEV_PENDING 0x4u
#define MAILK_SEV_BODY 0x8u

enum {
    MAILK_PEND_NONE = 0,
    MAILK_PEND_HELO,
    MAILK_PEND_MAIL,
    MAILK_PEND_RCPT,
    MAILK_PEND_AUTH,
    MAILK_PEND_DATA
};

#define MAILK_HOOK(p) (1u << (p))
#define MAILK_SRV_LINE_MAX 12288u
#define MAILK_SRV_MECHS 0xFBu

typedef struct {
    const char* name;
    unsigned caps, mechs, hooks;
    int tls_on, require_tls, require_auth, insecure_auth;
    uint64_t size_max;
    uint32_t line_max, rcpt_max, errors_max, msgs_max, auth_max, cmd_ms, data_ms, body_hwm;
} mailk_srv_opts_t;

typedef struct {
    int st, pend, step, tls_on, helo, ehlo, pend_ehlo, tx, authed, mech, body8, utf8, ret, skip;
    int require_tls, require_auth, insecure_auth, chunking, bdat_last, bdat_err, over;
    unsigned caps, mechs, hooks, ev;
    uint64_t size_max, bdat_left, msg_bytes;
    uint32_t rcpt_max, errors_max, msgs_max, auth_max, cmd_ms, data_ms, body_hwm, errors, msgs, auth_fails, nrcpt;
    char* name;
    mailk_line_t line;
    mailk_unstuff_t un;
    mailk_scram_t scram;
    dyn_sb_t out, helo_arg, from, envid, rcpt, rcpts, body, user, authz, secret, rej;
} mailk_srv_t;

int mailk_srv_init(mailk_srv_t* s, const mailk_srv_opts_t* o);
void mailk_srv_free(mailk_srv_t* s);
int mailk_srv_feed(mailk_srv_t* s, const uint8_t* p, size_t n, size_t* used);
int mailk_srv_decide(mailk_srv_t* s, int code, const char* esc, const char* text);
int mailk_srv_scram_creds(mailk_srv_t* s, const uint8_t* salt, size_t saltn, uint32_t iters,
    const uint8_t* stored, const uint8_t* server);
int mailk_srv_tls_done(mailk_srv_t* s);
uint32_t mailk_srv_timeout_ms(const mailk_srv_t* s);
int mailk_srv_timeout(mailk_srv_t* s);

#endif
