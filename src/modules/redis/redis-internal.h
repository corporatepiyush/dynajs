#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

#include "dyna-nat.h"

#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif

#include "dyna-aio.h"
#include "core/dyn-resp.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_REDIS)
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define RD_ST_CONNECTING 0
#define RD_ST_READY 2
#define RD_ST_DEAD 3
#define RD_INT_HELLO 1

typedef struct dyn_redis_pending {
    struct dyn_redis_pending* next;
    JSValue resolve, reject;
    JSValue acc;
    uint8_t* bytes;
    size_t nbytes;
    int internal;
    int is_sub_cmd;
    int sub_left;
    int want;
    int got;
    uint64_t deadline_ms;
} dyn_redis_pending_t;

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd;
    int state;
    int proto;
    int binary;
    int bigint;
    int hooked;
    int released;
    int hello_queued;
    char *host, *path, *user, *pass;
    uint16_t port;
    int port_i;
    int db;
    size_t maxbulk;
    int maxpending;
    uint64_t connect_deadline_ms;
    int cb_depth;
    int closing;
    uint64_t command_timeout_ms;
    dyn_resp_scan_t rscan;
    JSValue self;
    int self_held;

    uint8_t* rbuf;
    size_t rcap, rlen, rpos;
    uint8_t* obuf;
    size_t ocap, olen;

    dyn_redis_pending_t *head, *tail;
    dyn_redis_pending_t *wq_head, *wq_tail;
    int npending, nwait;
    int flush_queued;
    int subscribed;
    int sub_guess;
    int sub_cp;
    int sub_shard;
    JSValue h_push, h_error;
#ifdef CONFIG_TLS
    int use_tls;
    char* tls_ca;
    dyn_tls_ctx_t* tls_ctx;
#endif
} dyn_redis_t;

void dyn_redis_teardown(dyn_redis_t* r);

void redis_fail_all(dyn_redis_t* r, const char* msg);

void redis_cb_exit(dyn_redis_t* r);

void redis_release_if_idle(dyn_redis_t* r);

void redis_client_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

int redis_buf_reserve(uint8_t** p, size_t* cap, size_t need);

dyn_redis_pending_t* pend_new(JSContext* ctx);

void redis_secure_zero(void* p, size_t n);

void pend_wipe_bytes(dyn_redis_pending_t* p);

void pend_free(JSContext* ctx, dyn_redis_pending_t* p);

void pend_free_rt(JSRuntime* rt, dyn_redis_pending_t* p);

void pend_push(dyn_redis_pending_t** head, dyn_redis_pending_t** tail,
    dyn_redis_pending_t* p);

dyn_redis_pending_t* pend_pop(dyn_redis_pending_t** head,
    dyn_redis_pending_t** tail);

void redis_settle(dyn_redis_t* r, dyn_redis_pending_t* p, int reject,
    JSValue v);

JSValue redis_conn_error(JSContext* ctx, const char* msg);

int redis_flush(dyn_redis_t* r);

void redis_flush_soon(dyn_redis_t* r);

void redis_flush_drop(dyn_redis_t* r);

int redis_arm(dyn_redis_t* r, dyn_redis_pending_t* p);

int redis_send_internal(dyn_redis_t* r, int kind, int argc,
    const char* const* argv, const size_t* lens);

void redis_sub_recount(dyn_redis_t* r);

void redis_one_reply(dyn_redis_t* r, const uint8_t* msg, size_t len);

int redis_gone(dyn_redis_t* r);

int redis_queue_hello(dyn_redis_t* r);

void redis_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

typedef struct {
    const char** argv;
    size_t* lens;
    uint8_t* is_cstr;
    void* blk;
    int n;
} redis_args_t;

void args_free(JSContext* ctx, redis_args_t* a);

int args_build(JSContext* ctx, redis_args_t* a, int argc,
    JSValueConst* argv);

extern const char* const RD_SUB[3];

extern const char* const RD_UNSUB[3];

int redis_verb_is(const char* cmd, size_t len, const char* const* tab,
    size_t n);

int redis_reply_count(const char* cmd, size_t len, int argc);

JSValue redis_enqueue(JSContext* ctx, dyn_redis_t* r, int argc,
    const char* const* argv, const size_t* lens,
    int want, JSValue acc);

extern _Thread_local int redis_in_final;
#endif

#endif
