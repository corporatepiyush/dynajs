#ifndef SCRAPE_INTERNAL_H
#define SCRAPE_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-crawlkit.h"
#include "dtoa.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <poll.h>
#include "core/dyn-timer.h"
#include "core/dyn-prng.h"
#include "core/dyn-simdk.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif
#include "core/dyn-httpk.h"
#include "modules/http/http-conn.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

extern JSClassID dyn_rb_class_id;

JSValue dyn_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_rb_allows(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rb_delay(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rb_sitemaps(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rb_rules(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_ex_class_id;

void dyn_ex_finalizer(JSRuntime* rt, JSValue val);

void dyn_ex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

JSValue dyn_ex_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_ex_run(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    char* host;
    double next_ok_ms;
    rb_t* robots;
    int robots_tried;
    int robots_unreachable;
    int robots_ok;
    double robots_next_ms;
} fe_host_t;

#define FE_CACHE_MAX_ENTRIES 32

typedef struct {
    char *url, *etag, *lm, *ct;
    char* body;
    size_t blen;
} fe_cache_ent_t;

typedef struct {
    char* agent;
    int robots_on, retries, max_redirects;
    double robots_ttl_ms;
    int allow_private_hosts;
    int revalidate;
    int allow_insecure_downgrade;
    double min_delay_ms, max_body;
    char* proxy;
    char* ca;
    int pool_size;
    uint64_t rng;
    fe_host_t* hosts;
    size_t n_hosts, cap_hosts;
    double fetched, skipped_robots, retried, throttled_ms, bytes;
    double revalidated, saved_bytes;
    fe_cache_ent_t cache[FE_CACHE_MAX_ENTRIES];
    size_t n_cache;
    int ca_adopted;
    int ca_adopted_stream;
    dyn_cookiejar_t* jar;
} fe_t;

extern JSClassID dyn_fe_class_id;

fe_t* fe_live(JSContext* ctx, JSValueConst fv);

void fe_cache_clear(fe_cache_ent_t* e);

fe_cache_ent_t* fe_cache_find(fe_t* f, const char* url);

fe_cache_ent_t* fe_cache_put(fe_t* f, const char* url,
    const char* etag, const char* lm,
    const char* ct,
    const char* body, size_t blen);

fe_host_t* fe_host(fe_t* f, const char* host);

double fe_host_floor_ms(const fe_t* f, const fe_host_t* h);

double fe_retry_wait(fe_t* f, int st, int attempt, const char* retry_after);

int fe_redirect_step(JSContext* ctx, fe_t* f, char* cur, size_t cap, int st,
    const char* loc, int hop);

int fe_num_prop(JSContext* ctx, JSValueConst o, const char* k, int dflt,
    int* out);

int fe_status(JSContext* ctx, JSValueConst res);

const char* fe_header(JSContext* ctx, JSValueConst res, const char* want,
    JSValue* hold);

char* fe_headers_all(JSContext* ctx, JSValueConst res, const char* want);

void fe_link_canonical(JSContext* ctx, JSValueConst res,
    const char* base, JSValue out);

int fe_robots_step(JSContext* ctx, JSValueConst fv, const char* host0,
    char* url, size_t cap, JSValue res, int failed, int hop);

JSValue fe_finish_response(JSContext* ctx, JSValueConst fetcher,
    fe_t* f, const char* cur,
    JSValue res, int st);

JSValue dyn_fe_stats(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} sc_sb_t;

void sc_sb_init(sc_sb_t* b);

void sc_sb_free(sc_sb_t* b);

void sc_sb_put(sc_sb_t* b, const char* s, size_t n);

void sc_sb_puts(sc_sb_t* b, const char* s);

JSValue fe_auto_client(JSContext* ctx, double max_body);

int fe_engine_install(JSContext* ctx, JSValueConst sitemap);

JSValue fe_sm_locs(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue fe_sm_source(JSContext* ctx, int argc, JSValueConst* argv, int* own);

int fe_sm_limits(int which);

int dyn_scrape_promise_swallow(JSContext* ctx, JSValue v);

JSValue dyn_fe_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv);

typedef struct {
    char* url;
    int depth;
} cr_item_t;

typedef struct cr_flight cr_flight_t;

typedef struct cr_next_wait cr_next_wait_t;

struct cr_next_wait {
    JSValue resolve;
    JSValue reject;
    cr_next_wait_t* next;
};

typedef struct {
    cr_item_t* q;
    size_t qn, qcap, qhead;
    char** seen;
    size_t sn, scap;
    uint32_t* sidx;
    size_t sidx_mask;
    int max_pages, max_depth, same_host;
    size_t dropped;
    char* link_field;
    char* base_field;
    char* canonical_field;
    char* rel_field;
    char* robots_field;
    char seed_host[300];
    int emitted;
    int started;
    int concurrency;
    JSContext* ctx;
    JSRuntime* rt;
    JSValue self_pending;
    void* owner;
    cr_flight_t* flights;
    size_t n_flights;
    cr_flight_t *done_head, *done_tail;
    size_t n_done;
    cr_next_wait_t *wait_head, *wait_tail;
    JSValue pending_err;
    unsigned flight_seq;
} cr_t;

struct cr_flight {
    int state;
    char cur[1024], host[300];
    const char* path;
    int https, hop, attempt, st, depth;
    double floor_ms;
    JSContext* ctx;
    cr_t* c;
    JSValue crawl;
    JSValue fetcher;
    JSValue client;
    JSValue uh;
    JSValue res;
    JSValue pending;
    JSValue err;
    int finished;
    int floor_claimed;
    int park_claims;
    int graveyard;
    int pins_released;
    int claim_released;
    unsigned magic;
    unsigned seq;
    cr_flight_t* next;
};

_Static_assert(offsetof(cr_flight_t, err) == offsetof(cr_flight_t, crawl) + 6 * sizeof(JSValue),
    "cr_flight_t pins must stay contiguous");

extern JSClassID dyn_cr_class_id;

JSValue dyn_cr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv);

JSValue dyn_cr_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_cr_next(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_cr_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_cr_serialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cr_resume(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sitemap_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
