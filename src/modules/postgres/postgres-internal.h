#ifndef POSTGRES_INTERNAL_H
#define POSTGRES_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif

#include "dtoa.h"
#include <math.h>
#include "dyna-aio.h"
#include "core/dyn-scram.h"
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-timer.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define PG_MAX_CANCEL_KEY 256
#define PG_CONNECT_TIMEOUT 10000
#define PG_OID_BOOL 16
#define PG_OID_BYTEA 17
#define PG_OID_INT8 20
#define PG_OID_INT2 21
#define PG_OID_INT4 23
#define PG_OID_OID 26
#define PG_OID_FLOAT4 700
#define PG_OID_FLOAT8 701
#define PG_OID_UUID 2950
#define PG_OID_NUMERIC 1700
#define PG_OID_DATE 1082
#define PG_OID_TIMESTAMP 1114
#define PG_OID_TIMESTAMPTZ 1184
#define PG_OID_INTERVAL 1186
#define PG_OID_JSON 114
#define PG_OID_JSONB 3802
#define PG_ST_READY 2
#define PG_ST_DEAD 3
#define PG_TSA_ANY 0
#define PG_TSA_RW 1
#define PG_TSA_RO 2
#define PG_TSA_PRIMARY 3
#define PG_TSA_STANDBY 4
#define PG_TSA_PREFER 5

typedef struct dyn_pg_batch {
    JSContext* ctx;
    JSValue* slots;
    int k, ndone;
    int dead;
    int settled;
    JSValue resolve, reject;
} dyn_pg_batch_t;

typedef struct dyn_pg_pending {
    struct dyn_pg_pending* next;
    JSValue resolve, reject;
    JSValue rows, fields;
    JSValue error;
    uint8_t* bytes;
    size_t nbytes;
    char tag[64];
    int rowcount;
    int64_t max_rows;
    uint64_t result_bytes;
    uint64_t deadline_ms;
    uint64_t esync;
    int sync_end;
    dyn_pg_batch_t* batch;
    int bidx;
    JSAtom* fatom;
    uint32_t* foid;
    uint8_t* fformat;
    int nfield;
    JSObjectTemplate* tpl;
    char stmt_name[24];
    struct pg_copy* copy;
    int gates;
} dyn_pg_pending_t;

typedef struct {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    int fd, state, hooked;
    int released;
    int insecure_auth;
    int raw;
    int bytes_out;
    int binary_results;
    int bigint;
    int dates_native;
    int json_parse;
    int arrays_native;
    int ranges_obj;
    JSValue numeric_ctor;
    char *host, *path, *user, *pass, *database, *appname;
    uint16_t port;
    size_t maxmsg;
    uint64_t connect_deadline_ms, query_timeout_ms;

    uint8_t* rbuf;
    size_t rcap, rlen, rpos;
    uint8_t* obuf;
    size_t ocap, olen;

    dyn_pg_pending_t *head, *tail, *wq_head, *wq_tail;
    dyn_pg_pending_t *dhead, *dtail;
    uint64_t sync_seq, sync_recv;
    int sync_closed;
    int npending, nwait, maxpending;
    int flush_queued;

    struct pg_stmt {
        char* sql;
        size_t sqllen;
        uint32_t* oids;
        uint32_t hash;
        int uses;
        int prepared;
        int noids;
        char name[24];
    }* stmts;
    int nstmts, cap_stmts;
    int stmt_cache_max;
    uint64_t max_result;
    int prepare_after;
    uint32_t stmt_seq;
    uint64_t n_prepared_hits;
    uint64_t n_unnamed;

    dyn_scram_t scram;
    int scram_live, scram_done;
    uint32_t backend_pid;
    uint8_t cancel_key[PG_MAX_CANCEL_KEY];
    size_t cancel_key_len;
    char tx_status;
    JSValue params;
    int nparams;
    JSValue h_notice, h_notify, h_error;
    int cb_depth;
    int closing;
    int auth_ok_seen;
    JSValue self;
    int self_held;
    int use_tls;
    int ssl_wait;
    int chan_bind, scram_plus, ep_len;
    int max_minor, min_minor, proto_minor, ssl_direct, npv_seen, auth_seen;
    uint8_t ep_hash[64];
    struct pg_scram_job* scram_job;
    struct pg_pp_job* pgpass_job;
    struct pg_hent {
        char* host;
        char* addr;
        uint16_t port;
    }* hosts;
    int nh, hi, pass_pp;
    unsigned gen;
    uint8_t tsa, tsa_pass2, lb, chk, chk_ok, chk_err, ro_on, hs_on, chk_vn;
    char chk_val[2];
    int chk_rows, sv_major;
    char *tsa_s, *lb_s;
    const char* addr_cur;
    char *hostaddr, *pp_file, *errs;
    size_t errs_len;
    uint64_t connect_ms;
    int copy_gate, copy_mode, rx_paused, rx_off, in_parse;
    dyn_pg_pending_t* gate_p;
#ifdef CONFIG_TLS
    char* tls_ca;
    dyn_tls_ctx_t* tls_ctx;
    dyn_tls_conn_t* tls_conn;
#endif
} dyn_pg_t;

int pg_gone(dyn_pg_t* g);

extern _Thread_local int pg_in_final;

void pg_stmt_evict_named(dyn_pg_t* g, const char* name);

void pg_fail_all(dyn_pg_t* g, const char* msg);

#define PG_CB_PREFER 0
#define PG_CB_DISABLE 1
#define PG_CB_REQUIRE 2

void pg_scram_job_orphan(dyn_pg_t* g);
void pg_scram_jobs_reap(void);
void pg_pgpass_start(dyn_pg_t* g);
void pg_pgpass_job_orphan(dyn_pg_t* g);
void pg_after_pgpass(dyn_pg_t* g, int rc);
int pg_pgpass_prep(dyn_pg_t* g);
int pg_host_load(dyn_pg_t* g, int i);
void pg_hosts_free(dyn_pg_t* g);
void pg_try_next(dyn_pg_t* g, const char* why);

int pgbuf_reserve(uint8_t** p, size_t* cap, size_t need);

void put32(uint8_t* p, uint32_t v);

typedef struct {
    uint8_t* b;
    size_t len, cap;
    int bad;
} pgw_t;

void pgw_raw(pgw_t* w, const void* p, size_t n);

void pgw_u8(pgw_t* w, uint8_t v);

void pgw_u32(pgw_t* w, uint32_t v);

void pgw_str(pgw_t* w, const char* s);

size_t pgw_begin(pgw_t* w, char type);

void pgw_end(pgw_t* w, size_t at);

dyn_pg_pending_t* pgp_new(void);

void pgp_free_fields(JSContext* ctx, dyn_pg_pending_t* p);

void pgp_free(JSContext* ctx, dyn_pg_pending_t* p);

void pgp_free_rt(JSRuntime* rt, dyn_pg_pending_t* p);

void pgp_push(dyn_pg_pending_t** h, dyn_pg_pending_t** t, dyn_pg_pending_t* p);

dyn_pg_pending_t* pgp_pop(dyn_pg_pending_t** h, dyn_pg_pending_t** t);

void pg_release_if_idle(dyn_pg_t* g);

void pg_cb_exit(dyn_pg_t* g);

void pg_hold(JSContext* ctx, JSValueConst obj, dyn_pg_t* g);

void pg_settle(dyn_pg_t* g, dyn_pg_pending_t* p, int reject, JSValue v);

JSValue pg_conn_error(JSContext* ctx, const char* msg);

void pg_batch_fail(dyn_pg_t* g, dyn_pg_batch_t* b, const char* msg);

void pg_settle_entry_err(dyn_pg_t* g, dyn_pg_pending_t* p, JSValue e);

void pg_settle_entry(dyn_pg_t* g, dyn_pg_pending_t* p);

int pg_flush(dyn_pg_t* g);

void pg_flush_soon(dyn_pg_t* g);

int pg_write(dyn_pg_t* g, const uint8_t* b, size_t n);

#define PG_PF_NONE 0
#define PG_PF_CSTR 1
#define PG_PF_MALLOC 2

void pg_free_params(JSContext* ctx, const char** pv, const uint8_t* pf,
    uint32_t n);

int pg_param_bytea(JSContext* ctx, JSValueConst v, const char** out,
    size_t* outlen);

void pg_stmt_learn_oids(dyn_pg_t* g, const char* name,
    const uint32_t* oids, int n);

void pg_call1(dyn_pg_t* g, JSValue fn, JSValue arg);

void pg_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

int pg_send_startup(dyn_pg_t* g);

typedef struct pg_cps pg_cps_t;

typedef void (*pg_cps_step_fn)(JSContext* ctx, pg_cps_t* cp,
    JSValueConst val, int failed);

typedef struct pg_cps_ref pg_cps_ref_t;

struct pg_cps_ref {
    JSValue obj;
    pg_cps_ref_t* next;
};

struct pg_cps {
    pg_cps_step_fn step;
    int settled;
    pg_cps_ref_t* refs;
};

extern JSClassID pgcps_class_id;

void pg_cps_defuse_all(JSContext* ctx, pg_cps_t* cp);

void pg_cps_await(JSContext* ctx, pg_cps_t* cp, pg_cps_step_fn step,
    JSValue promise);

typedef struct pgq_iter pgq_iter_t;

typedef struct pgq_op pgq_op_t;

struct pgq_op {
    pg_cps_t cps;
    pgq_op_t* qnext;
    JSValue it_obj;
    JSValue jresolve, jreject;
    int mode;
    int want_return;
};

#define PGQ_ST_IDLE 0

struct pgq_iter {
    JSContext* ctx;
    JSValue conn_obj;
    JSValue query_fn;
    char* sql;
    JSValue params;
    char name[24];
    JSValue rows;
    uint32_t row_index, row_count;
    int64_t batch;
    int64_t max_rows;
    int64_t emitted;
    int state;
    int declared;
    int began;
    int running;
    int in_pump;
    JSValue error;
    pgq_op_t *qhead, *qtail;
};

extern JSClassID pgq_class_id;

void pgq_iter_finalizer(JSRuntime* rt, JSValue val);

void pgq_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

JSValue pgq_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue pgq_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue pgq_throw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue pgq_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_queryiter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

dyn_pg_t* pg_this(JSContext* ctx, JSValueConst this_val);

void pg_stmt_clear(dyn_pg_t* g);

struct pg_stmt* pg_stmt_get(dyn_pg_t* g, const char* sql, size_t n);

void pg_param_arrays_free(const char** pv, size_t* plen,
    uint8_t* pnull, uint8_t* pfree);

int pg_coerce_params(JSContext* ctx, JSValueConst params,
    const char*** out_pv, size_t** out_plen,
    uint8_t** out_pnull, uint8_t** out_pfree,
    uint32_t* out_n, const char* what);

int pg_encode_member(dyn_pg_t* g, pgw_t* w, const char* sql,
    int nparam, const char** pv, const size_t* plen,
    const uint8_t* pnull, char* name_out,
    size_t name_outcap);

int pg_submit(dyn_pg_t* g, dyn_pg_pending_t* p, uint8_t* bytes,
    size_t nbytes, uint64_t esync, int sync_end);

typedef struct pg_copy pg_copy_t;

int pg_sql_is_copy(const char* s);

int pg_drain_wq(dyn_pg_t* g);

void pg_copy_gate_check(dyn_pg_t* g);

void pg_parse_buffered(dyn_pg_t* g);

void pg_copy_message(dyn_pg_t* g, char type, const uint8_t* b, size_t len);

void pg_copy_end(dyn_pg_t* g, char type);

void pg_copy_tick(dyn_pg_t* g);

void pg_copy_settled(JSContext* ctx, pg_copy_t* c, int reject, JSValueConst v);

void pg_copy_detach(JSContext* ctx, JSRuntime* rt, pg_copy_t* c);

int pg_copy_init(JSContext* ctx);

JSValue dyn_pg_copy_from(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_copy_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_pipeline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_cancel(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_pg_get_ready(JSContext* ctx, JSValueConst this_val);
JSValue dyn_pg_get_proto(JSContext* ctx, JSValueConst this_val);
int pg_start_tls(dyn_pg_t* g);

JSValue dyn_pg_get_stmt_stats(JSContext* ctx, JSValueConst this_val);
typedef struct {
    int port, tls, ct;
} pg_ci_t;
int pg_conninfo(JSContext* ctx, dyn_pg_t* g, JSValueConst url, int use_env, const pg_ci_t* ci);
int pg_pgpass_read(const char* file, const char* host, const char* port, const char* db, const char* user, char** out);
#endif

#endif
