#ifndef DYNAJS_NAT_H
#define DYNAJS_NAT_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#include "dyna-io.h"

typedef void (*DynDisposeFunc)(void* native);

typedef struct {
    void* native;
    DynDisposeFunc dispose;
    int closed;
    int async_users;
    uint64_t use_seq;
    JSValue pin;
} DynResource;

typedef struct DynFiber DynFiber;
typedef JSValue DynFiberEntry(JSContext* ctx, void* arg);

extern _Atomic int dyn_fiber_offloaded_n;
extern _Atomic int dyn_fiber_live_n;
extern _Atomic int dyn_fiber_draining_cpu;

JSValue dyn_fiber_start(JSContext* ctx, DynFiberEntry* entry, void* arg);
void* dyn_fiber_arg(DynFiberEntry* entry);
void dyn_fiber_offload(void);
void dyn_fiber_offload_pool_slow(int cls);
int dyn_fiber_can_offload_slow(void);
int dyn_fiber_cancel_state(void);
void dyn_fiber_onjs_slow(void);
JSValue dyn_fiber_await(JSContext* ctx, JSValue v);
void dyn_fiber_stats(uint64_t* spawned, uint64_t* ran_inline, int* live);

static inline void dyn_fiber_onjs(void)
{
    if (__builtin_expect(atomic_load_explicit(&dyn_fiber_offloaded_n, memory_order_relaxed) != 0, 0))
        dyn_fiber_onjs_slow();
}

static inline int dyn_fiber_any(void)
{
    return atomic_load_explicit(&dyn_fiber_live_n, memory_order_relaxed) != 0;
}

static inline int dyn_fiber_can_offload(void)
{
    return dyn_fiber_any() && dyn_fiber_can_offload_slow();
}

#define DYN_OFFLOAD_IF_(live, cls, expr) __extension__({ int dyn_off_l_ = (live); if (__builtin_expect(dyn_off_l_ != 0, 0)) dyn_fiber_offload_pool_slow(cls); __typeof__(expr) dyn_off_r_ = (expr); if (__builtin_expect(dyn_off_l_ != 0, 0)) dyn_fiber_onjs(); dyn_off_r_; })
#define DYN_OFFLOAD_CPU_IF(live, expr) DYN_OFFLOAD_IF_(live, 1, expr)
#define DYN_OFFLOAD_CPU(expr) DYN_OFFLOAD_IF_(dyn_fiber_any(), 1, expr)
#define DYN_OFFLOAD_IO(expr) DYN_OFFLOAD_IF_(dyn_fiber_any(), 2, expr)

JSValue dyn_res_wrap(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, void* native,
    DynDisposeFunc dispose);

DynResource* dyn_res_get(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id);

void dyn_res_mark_closed(JSRuntime* rt, JSValue obj, JSClassID class_id);

void dyn_res_hold(JSContext* ctx, DynResource* r);
int dyn_res_pin(JSContext* ctx, JSValueConst obj, JSClassID class_id);
void dyn_res_unpin(JSRuntime* rt, JSValueConst obj, JSClassID class_id);

void* dyn_nat_malloc(size_t size);
void* dyn_nat_calloc(size_t nmemb, size_t size);
void* dyn_nat_realloc(void* ptr, size_t size);
char* dyn_nat_strdup(const char* s);
void dyn_nat_free(void* ptr);
JSValue dyn_nat_u8_copy(JSContext* ctx, const uint8_t* data, size_t len);
JSValue dyn_nat_new_string_wf(JSContext* ctx, const char* s, size_t len);
JSValue dyn_nat_ab_take(JSContext* ctx, void* data, size_t len);
JSValue dyn_utf8_lossy_string(JSContext* ctx, const uint8_t* p, size_t n);
size_t dyn_utf8_lossy_into(const uint8_t* p, size_t n, uint8_t* out, int* changed);
int dyn_utf8_valid(const uint8_t* p, size_t n);

int dyn_nat_track(size_t size);
void dyn_nat_untrack(size_t size);

uint64_t dyn_nat_bytes(void);
void dyn_nat_set_limit(uint64_t limit);
int dyn_nat_set_script_limit(uint64_t limit, uint64_t* ceiling_out);
uint64_t dyn_nat_limit(void);
uint64_t dyn_nat_operator_limit(void);

struct DfkCsvSpec;
typedef JSValue DynDfCsvReader(JSContext* ctx, const char* op, const uint8_t* buf,
    size_t len, const struct DfkCsvSpec* spec);
extern DynDfCsvReader* const dyn_df_csv_reader;
JSValue dyn_df_read_csv(JSContext* ctx, const char* op, const uint8_t* buf,
    size_t len, const struct DfkCsvSpec* spec);
struct DfkCsvOut;
typedef JSValue DynDfCsvOutFrame(JSContext* ctx, const char* op, struct DfkCsvOut* out);
extern DynDfCsvOutFrame* const dyn_df_csv_frame;
JSValue dyn_df_csv_out_frame(JSContext* ctx, const char* op, struct DfkCsvOut* out);

JSValue dyn_plain_wrap(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, void* native,
    DynDisposeFunc dispose);

JSValue dyn_ctor_proto(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id);

static inline void* dyn_plain_get(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    return JS_GetOpaque2(ctx, this_val, class_id);
}

static inline int dyn_js_len(JSContext* ctx, JSValueConst obj, int64_t* out)
{
    JSValue l = JS_GetPropertyStr(ctx, obj, "length");
    int r = JS_ToInt64(ctx, out, l);
    JS_FreeValue(ctx, l);
    if (r)
        *out = 0;
    return r;
}

static inline int dyn_js_len_u32(JSContext* ctx, JSValueConst obj, uint32_t* out)
{
    JSValue l = JS_GetPropertyStr(ctx, obj, "length");
    int r = JS_ToUint32(ctx, out, l);
    JS_FreeValue(ctx, l);
    if (r)
        *out = 0;
    return r;
}

void js_std_set_job_error_rt(JSRuntime* rt, JSValue exc);
int js_std_job_error_pending_rt(JSRuntime* rt);
int js_std_take_job_error_ctx(JSContext* ctx);

static inline void dyn_exc_forward(JSContext* ctx)
{
    if (JS_HasUncatchableException(ctx))
        js_std_set_job_error_rt(JS_GetRuntime(ctx), JS_GetException(ctx));
}

static inline int dyn_exc_reclaim(JSContext* ctx, int pending_before)
{
    if (pending_before || !js_std_take_job_error_ctx(ctx))
        return 0;
    JS_SetUncatchableException(ctx, 1);
    return 1;
}

static inline void dyn_exc_drop(JSContext* ctx)
{
    if (JS_HasUncatchableException(ctx))
        js_std_set_job_error_rt(JS_GetRuntime(ctx), JS_GetException(ctx));
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
}

static inline int dyn_settle(JSContext* ctx, JSValueConst p, JSValueConst v, int is_reject)
{
    if (JS_PromiseSettle(ctx, p, v, is_reject) == 0)
        return 0;
    if (JS_HasUncatchableException(ctx))
        return -1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 0;
}

static inline int dyn_settle_value(JSContext* ctx, JSValueConst p, JSValueConst v)
{
    JSValue e;
    int rc;
    if (!JS_IsException(v))
        return dyn_settle(ctx, p, v, 0);
    if (JS_HasUncatchableException(ctx))
        return -1;
    e = JS_GetException(ctx);
    rc = dyn_settle(ctx, p, e, 1);
    JS_FreeValue(ctx, e);
    return rc;
}

static inline int dyn_get(JSContext* ctx, JSValueConst obj, const char* name, JSValue* out)
{
    *out = JS_GetPropertyStr(ctx, obj, name);
    return JS_IsException(*out) ? -1 : 0;
}

static inline int dyn_take_syntax_error(JSContext* ctx)
{
    JSValue ex;
    if (JS_HasUncatchableException(ctx))
        return 0;
    ex = JS_GetException(ctx);
    if (JS_IsSyntaxError(ctx, ex)) {
        JS_FreeValue(ctx, ex);
        return 1;
    }
    JS_Throw(ctx, ex);
    return 0;
}

static inline int dyn_get_truthy(JSContext* ctx, JSValueConst obj, const char* name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int b;
    if (JS_IsException(v))
        return -1;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static inline double dyn_num_value(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_INT ? (double)JS_VALUE_GET_INT(v) : JS_VALUE_GET_FLOAT64(v);
}

static inline int32_t dyn_int_or(JSValueConst v, int32_t def)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_INT ? JS_VALUE_GET_INT(v) : def;
}

static inline int dyn_idx_arg(JSContext* ctx, JSValueConst v, uint32_t* out)
{
    double nd;
    if (JS_ToFloat64(ctx, &nd, v))
        return -1;
    if (!(nd >= 0) || nd > (double)UINT32_MAX) {
        if (nd > (double)UINT32_MAX) {
            JS_ThrowRangeError(ctx, "index out of range");
            return -1;
        }
        *out = UINT32_MAX;
        return 0;
    }
    if (nd != (double)(uint32_t)nd) {
        JS_ThrowRangeError(ctx, "index out of range");
        return -1;
    }
    *out = (uint32_t)nd;
    return 0;
}

JSValue dyn_tls_missing(JSContext* ctx, const char* who, const char* what);
int dyn_register_plain_class(JSContext* ctx, JSModuleDef* m, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs, JSCFunction* ctor_fn,
    const char* name);

static inline int dyn_wtf8_has_surrogate(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++)
        if ((unsigned char)s[i] == 0xED && (unsigned char)s[i + 1] >= 0xA0)
            return 1;
    return 0;
}

extern int (*dyn_sink_direct)(JSContext* ctx, JSValueConst sink, JSValueConst write_fn, const uint8_t* p, size_t n);
extern JSClassID dyn_sink_direct_cid;

static inline void* dyn_res_native(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    DynResource* r = dyn_res_get(ctx, this_val, class_id);
    return r ? r->native : NULL;
}

void dyn_res_class_common(JSContext* ctx, JSClassID class_id, JSValue proto);

void dyn_res_finalizer(JSRuntime* rt, JSValue val);

int dyn_register_class(JSContext* ctx, JSModuleDef* m, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs, int n_funcs,
    JSCFunction* ctor_fn, const char* name);

const char* dyn_path_borrow(JSContext* ctx, JSValueConst v, const char* what,
    size_t* plen);
int dyn_value_is_path(JSValueConst v);

int js_nat_init_structures(JSContext* ctx);
JSAtom dyn_registered_symbol(JSContext* ctx, const char* key);
#define DYN_HTTP_REQUEST_HOOK "dyna.http.request"
#define DYN_HTTP_REDIRECT_HOOK "dyna.http.redirect"
int js_nat_init_structures_ext(JSContext* ctx);
struct dyn_aio;
struct dyn_aio* dyn_net_reactor_acquire(JSContext* ctx);
void dyn_net_reactor_release(JSContext* ctx);
void dyn_net_reactor_weak_hold(void);
void dyn_net_reactor_hold(void);
void dyn_net_reactor_weak_drop(JSRuntime* rt);
JSValue dyn_net_throw_no_reactor(JSContext* ctx, const char* who);
void dyn_net_reactor_release_rt(JSRuntime* rt);
int dyn_net_on_drain(void (*fn)(void*), void* udata);
int dyn_net_on_reactor_free(void (*fn)(void));
int dyn_net_on_drain_notick(void (*fn)(void*), void* udata);
void dyn_net_off_drain(void* udata);
extern JSClassID dyn_sp_class_id;
JSValue dyn_sp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);
JSValue dyn_sp_constructor(JSContext* ctx);
void dyn_sp_install(JSContext* ctx, JSValueConst global_obj);
JSValue dyn_form_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_form_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
static inline int dyn_hm_ows(char c) { return c == ' ' || c == '\t'; }
int dyn_mp_ctl(const char* s, size_t n);
void dyn_hm_trim(const char* s, size_t* b, size_t* e);
size_t dyn_hm_next_elem(const char* s, size_t n, size_t i, size_t* end);
JSValue dyn_hm_ctype_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_mp_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_mp_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_http_multipart_format(JSContext* ctx, JSValueConst parts);
JSValue dyn_http_multipart_parse(JSContext* ctx, JSValueConst ctype, JSValueConst body);
enum {
    NET_HK_TCP_CONNECT = 0,
    NET_HK_TCP_DATA,
    NET_HK_TCP_CLOSE,
    NET_HK_UDP_MESSAGE,
    NET_HK_TCP_DRAIN,
    NET_HK_DNS_QUERY,
    NET_HK_N
};
void dyn_net_handler_result(JSContext* ctx, JSValue r, int kind);
uint64_t dyn_net_handler_throw_count(void);
int dyn_idna_hostname_ok(const char* utf8, size_t n);
#ifdef CONFIG_NATIVE_MODULE_NET
int js_nat_init_net(JSContext* ctx);
int dyn_tcp_register(JSContext* ctx, JSModuleDef* m);
void dyn_tcp_add_exports(JSContext* ctx, JSModuleDef* m);
int dyn_ratelimit_register(JSContext* ctx, JSModuleDef* m);
void dyn_ratelimit_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_NETIP
int js_nat_init_netip(JSContext* ctx);
int dyn_netip_register(JSContext* ctx, JSModuleDef* m);
int dyn_netip_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_HTTP
int js_nat_init_http(JSContext* ctx);
int dyn_http_register(JSContext* ctx, JSModuleDef* m);
void dyn_http_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_DNS
int js_nat_init_dns(JSContext* ctx);
int dyn_dns_register(JSContext* ctx, JSModuleDef* m);
void dyn_dns_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_PROXY
int js_nat_init_proxy(JSContext* ctx);
int dyn_proxy_register(JSContext* ctx, JSModuleDef* m);
void dyn_proxy_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_METRICS
int js_nat_init_metrics(JSContext* ctx);
int dyn_metrics_register(JSContext* ctx, JSModuleDef* m);
void dyn_metrics_add_exports(JSContext* ctx, JSModuleDef* m);
enum { DYN_MET_COUNTER,
    DYN_MET_GAUGE,
    DYN_MET_HISTOGRAM };
void dyn_metrics_c_record(int kind, const char* name, double value);
typedef struct {
    _Atomic uint32_t key;
} dyn_met_handle_t;
void dyn_metrics_c_record_h(dyn_met_handle_t* h, int kind, const char* name, double value);
char* dyn_metrics_c_scrape(size_t* out_len);
#endif
#ifdef CONFIG_NATIVE_MODULE_POSTGRES
int js_nat_init_postgres(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_REDIS
int js_nat_init_redis(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SMTP
int js_nat_init_smtp(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_WIREGUARD
int js_nat_init_wireguard(JSContext* ctx);
#endif
#if defined(CONFIG_NATIVE_MODULE_SQLITE) && defined(CONFIG_SQLITE)
int js_nat_init_sqlite(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
int js_nat_init_ml(JSContext* ctx);
#endif
#ifdef CONFIG_SDL3
int js_nat_init_sdl(JSContext* ctx);
int dyn_sdl_renderer_parts(JSContext* ctx, JSValueConst v, void** renderer, void** window);
int dyn_sdl_renderer_alive(JSValueConst v);
#endif
#ifdef CONFIG_RAYLIB
int js_nat_init_raylib(JSContext* ctx);
int dyn_raylib_target(JSContext* ctx, JSValueConst v, int* in_frame);
int dyn_raylib_texture_height(void);
int dyn_raylib_blend_mode(void);
int dyn_raylib_window_alive(JSValueConst v);
#endif
#ifdef CONFIG_UI
int js_nat_init_ui(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
int js_nat_init_compress(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
int js_nat_init_random(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
int js_nat_init_simd(JSContext* ctx);
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
int js_nat_init_uring(JSContext* ctx);
int dyn_uring_read_all(const char* path, char** out, size_t* outlen);
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
int js_nat_init_file(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_STREAM
int js_nat_init_stream(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SEMVER
int js_nat_init_semver(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_VEC
int js_nat_init_vec(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SVG
int js_nat_init_svg(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_BYTES
int js_nat_init_bytes(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_CRYPTO
int js_nat_init_hash(JSContext* ctx);
int js_nat_init_crypto(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_MATCHER
int js_nat_init_matcher(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_ENCODING
int js_nat_init_encoding(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_TIME
int js_nat_init_time(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_MATHX
int js_nat_init_mathx(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_CSV
int js_nat_init_csv(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_DATAFRAME
int js_nat_init_dataframe(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_UUID
int js_nat_init_uuid(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_OTP
int js_nat_init_otp(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_GRAPH
int js_nat_init_graph(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_PROTOBUF
int js_nat_init_protobuf(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_ASN1
int js_nat_init_asn1(JSContext* ctx);
#endif
#if defined(CONFIG_NATIVE_MODULE_DKIM) && defined(CONFIG_OPENSSL)
int js_nat_init_dkim(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_CONFIG
int js_nat_init_config(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_LOG
int js_nat_init_log(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_URL
int js_nat_init_url(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SSH
int js_nat_init_ssh(JSContext* ctx);
int js_nat_init_sftp(JSContext* ctx);
#endif
#if defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)
int js_nat_init_webrtc(JSContext* ctx);
void js_webrtc_install_globals(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_TERM
int js_nat_init_term(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_VALIDATE
int js_nat_init_validate(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_JSON
int js_nat_init_json(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SCHEMA
int js_nat_init_schema(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_XML
int js_nat_init_xml(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_YAML
int js_nat_init_yaml(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_DECIMAL
int js_nat_init_decimal(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_DOCDB
int js_nat_init_docdb(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_VSERIALIZE
int js_nat_init_vserialize(JSContext* ctx);
#endif
#if defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_SCHEMA)
int dyn_value_hash64(JSContext* ctx, JSValueConst v, uint64_t* out);
#endif
#ifdef CONFIG_NATIVE_MODULE_HTML
int js_nat_init_html(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
int js_nat_init_sys(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_ASYNC
int js_nat_init_async(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SCRAPE
int js_nat_init_scrape(JSContext* ctx);
#endif
#if defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)
int js_nat_init_ai(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_BENCH
int js_nat_init_bench(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_OAUTH2
int js_nat_init_oauth2(JSContext* ctx);
#endif

int js_nat_init_all(JSContext* ctx);

#endif
int dyn_opts_strict(JSContext* ctx, JSValueConst opts, const char* const* keys, int nkeys);
int dyn_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, const char* fn, int* out);
int dyn_opts_bag(JSContext* ctx, int argc, JSValueConst* argv, int idx, const char* const* keys, int nkeys, const char* fn);
JSValue dyn_bigint_to_number(JSContext* ctx, JSValue big);

#endif
