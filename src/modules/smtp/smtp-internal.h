#ifndef SMTP_INTERNAL_H
#define SMTP_INTERNAL_H

#include "dyna-nat.h"

#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif

#include "dyna-aio.h"
#include "core/dyn-mailk.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SMTP_SAMPLES 1024u

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct smtp_msg {
    struct smtp_msg* next;
    JSValue resolve, reject;
    char* from;
    char** to;
    size_t nto;
    uint8_t* body;
    size_t bodyn;
    int bdat, orcpt;
    char *ret, *envid, *notify;
    uint64_t main_ns;
} smtp_msg_t;

struct smtp;

typedef struct smtp_conn {
    struct smtp_conn* next;
    struct smtp* s;
    int fd, refs, busy, dead, ready, quitting, tls_wait, keepq, eof;
    mailk_cli_t eng;
    dyn_sb_t inq;
    smtp_msg_t* cur;
    void* parked;
    uint32_t sent;
    uint64_t deadline_ms, idle_ms;
} smtp_conn_t;

typedef struct smtp {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int refs, closing, hooked, released, self_held, cb_depth, quitting, strong;
    JSValue self, quit_resolve;
    char *host, *ca, *servername, *name, *user, *pass, *token, *authzid;
    uint16_t port;
    int tls;
    unsigned mechs;
    int insecure_auth;
    uint32_t scram_max_iters;
    uint64_t connect_ms, command_ms, idle_timeout_ms;
    int max_conns, nconns;
    uint32_t max_per_conn;
    smtp_msg_t *qh, *qt;
    int nq;
    smtp_conn_t* conns;
    uint64_t jobs, msgs;
    uint32_t hs[SMTP_SAMPLES], ms[SMTP_SAMPLES];
    uint64_t hsn, msn;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls_ctx;
#endif
} smtp_t;

uint64_t smtp_now_ns(void);
void smtp_pump(smtp_t* s);
void smtp_conn_kill(smtp_conn_t* c, const char* code, const char* msg);
void smtp_settle_msg(smtp_t* s, smtp_msg_t* m, int reject, JSValue v);
JSValue smtp_error(JSContext* ctx, const char* code, const char* msg, const mailk_cli_t* e, const mailk_res_t* r);
JSValue smtp_result(JSContext* ctx, smtp_msg_t* m, const mailk_cli_t* e);
void smtp_add_rejected(JSContext* ctx, JSValueConst err, const smtp_msg_t* m, const mailk_cli_t* e);
void smtp_msg_free(JSRuntime* rt, smtp_msg_t* m);
void smtp_unref(smtp_t* s);
void smtp_tick(void* ud);
void smtp_release_if_idle(smtp_t* s);
void smtp_enter(smtp_t* s);
void smtp_leave(smtp_t* s);
void smtp_run_parked(smtp_t* s);
char* smtp_opt_str(JSContext* ctx, JSValueConst o, const char* k, int* bad);
int64_t smtp_opt_num(JSContext* ctx, JSValueConst o, const char* k, int64_t dflt, int64_t lo, int64_t hi, int* bad);
unsigned smtp_opt_mechs(JSContext* ctx, JSValueConst o, int* bad);
double smtp_pct(const uint32_t* ring, uint64_t n, double q);
uint8_t* smtp_bytes_of(JSContext* ctx, JSValueConst v, size_t* n);

enum { HK_HELO, HK_AUTH, HK_MAIL, HK_RCPT, HK_CHUNK, HK_DATA, HK_N };

typedef struct {
    int code, scram;
    char esc[16];
    char* text;
    uint8_t salt[256], stored[64], server[64];
    size_t saltn;
    uint32_t iters;
} ssrv_dec_t;

struct ssrv;

typedef struct ssrv_conn {
    struct ssrv_conn* next;
    struct ssrv* sv;
    int fd, refs, busy, dead, closing, tls_wait, implicit_hs, waiting, wait_hk, paused, chunk_err, has_dec, tail_sent, user_set;
    const char* bye;
    uint32_t wait_seq, tmo_ms;
    uint64_t deadline_ms, main_ns, hook_ns;
    mailk_srv_t eng;
    dyn_sb_t inq;
    JSValue session;
    ssrv_dec_t dec;
    void* parked;
} ssrv_conn_t;

typedef struct ssrv {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int refs, cb_depth, listening, closing, hooked, released, tls, listen_fd, max_conns, nconns;
    JSValue self, hooks[HK_N];
    char *host, *name, *cert, *key;
    uint16_t port;
    mailk_srv_opts_t eo;
    uint32_t seq;
    ssrv_conn_t* conns;
    uint64_t jobs, msgs, sessions, refused, hook_errors;
    uint32_t hs[SMTP_SAMPLES], ms[SMTP_SAMPLES];
    uint64_t hsn, msn;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls_ctx;
#endif
} ssrv_t;

int ssrv_register(JSContext* ctx, JSModuleDef* m);
int ssrv_opts(JSContext* ctx, ssrv_t* sv, JSValueConst o);
extern const int ssrv_hk_pend[HK_N];
void ssrv_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud);
void ssrv_kill(ssrv_conn_t* c, int bye);
void ssrv_kick(ssrv_conn_t* c);
void ssrv_tick(void* ud);
void ssrv_run_parked(ssrv_t* sv);
void ssrv_unref(ssrv_t* sv);
void ssrv_dispatch(ssrv_conn_t* c);
void ssrv_conn_unref(ssrv_conn_t* c);
int ssrv_session_new(ssrv_conn_t* c);
#endif

#endif
