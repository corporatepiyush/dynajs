#ifndef DYNAJS_NAT_H
#define DYNAJS_NAT_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "dyna-io.h"

typedef void (*DynDisposeFunc)(void* native);

typedef struct {
    void* native;
    DynDisposeFunc dispose;
    int closed;
} DynResource;

JSValue dyn_res_wrap(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, void* native,
    DynDisposeFunc dispose);

DynResource* dyn_res_get(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id);

void dyn_res_mark_closed(JSRuntime* rt, JSValue obj, JSClassID class_id);

void* dyn_nat_malloc(size_t size);
void* dyn_nat_calloc(size_t nmemb, size_t size);
void* dyn_nat_realloc(void* ptr, size_t size);
char* dyn_nat_strdup(const char* s);
void dyn_nat_free(void* ptr);

int dyn_nat_track(size_t size);
void dyn_nat_untrack(size_t size);

uint64_t dyn_nat_bytes(void);
void dyn_nat_set_limit(uint64_t limit);
uint64_t dyn_nat_limit(void);

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

int dyn_register_plain_class(JSContext* ctx, JSModuleDef* m, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs, JSCFunction* ctor_fn,
    const char* name);

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
int js_nat_init_structures_ext(JSContext* ctx);
int dyn_graph_register(JSContext* ctx, JSModuleDef* m);
void dyn_graph_add_exports(JSContext* ctx, JSModuleDef* m);
#ifdef CONFIG_NATIVE_MODULE_NET
int js_nat_init_net(JSContext* ctx);
int dyn_netip_register(JSContext* ctx, JSModuleDef* m);
int dyn_netip_add_exports(JSContext* ctx, JSModuleDef* m);
int dyn_http_register(JSContext* ctx, JSModuleDef* m);
void dyn_http_add_exports(JSContext* ctx, JSModuleDef* m);
struct dyn_aio;
struct dyn_aio* dyn_net_reactor_acquire(JSContext* ctx);
void dyn_net_reactor_release(JSContext* ctx);
void dyn_net_reactor_release_rt(JSRuntime* rt);
int dyn_net_on_drain(void (*fn)(void*), void* udata);
int dyn_net_on_drain_notick(void (*fn)(void*), void* udata);
void dyn_net_off_drain(void* udata);
int dyn_tcp_register(JSContext* ctx, JSModuleDef* m);
void dyn_tcp_add_exports(JSContext* ctx, JSModuleDef* m);
uint64_t dyn_net_handler_throw_count(void);
void dyn_net_set_debug(int on);
int dyn_proxy_register(JSContext* ctx, JSModuleDef* m);
void dyn_proxy_add_exports(JSContext* ctx, JSModuleDef* m);
int dyn_dns_register(JSContext* ctx, JSModuleDef* m);
int dyn_ratelimit_register(JSContext* ctx, JSModuleDef* m);
int dyn_metrics_register(JSContext* ctx, JSModuleDef* m);
void dyn_dns_add_exports(JSContext* ctx, JSModuleDef* m);
void dyn_ratelimit_add_exports(JSContext* ctx, JSModuleDef* m);
void dyn_metrics_add_exports(JSContext* ctx, JSModuleDef* m);
enum { DYN_MET_COUNTER,
    DYN_MET_GAUGE,
    DYN_MET_HISTOGRAM };
int dyn_value_hash64(JSContext* ctx, JSValueConst v, uint64_t* out);
void dyn_metrics_c_record(int kind, const char* name, double value);
char* dyn_metrics_c_scrape(size_t* out_len);
int dyn_redis_register(JSContext* ctx, JSModuleDef* m);
void dyn_redis_add_exports(JSContext* ctx, JSModuleDef* m);
int dyn_pg_register(JSContext* ctx, JSModuleDef* m);
void dyn_pg_add_exports(JSContext* ctx, JSModuleDef* m);
#ifdef CONFIG_SQLITE
int dyn_sqlite_register(JSContext* ctx, JSModuleDef* m);
void dyn_sqlite_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
int js_nat_init_ml(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
int js_nat_init_compress(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
int js_nat_init_random(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
int js_nat_init_structures3(JSContext* ctx);
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
#ifdef CONFIG_NATIVE_MODULE_CONFIG
int js_nat_init_config(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_LOG
int js_nat_init_log(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_URL
int js_nat_init_url(JSContext* ctx);
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
#ifdef CONFIG_NATIVE_MODULE_VSERIALIZE
int js_nat_init_vserialize(JSContext* ctx);
int dyn_proto_register(JSContext* ctx, JSModuleDef* m);
int dyn_proto_add_exports(JSContext* ctx, JSModuleDef* m);
int dyn_asn1_register(JSContext* ctx, JSModuleDef* m);
int dyn_asn1_add_exports(JSContext* ctx, JSModuleDef* m);
#endif
#ifdef CONFIG_NATIVE_MODULE_HTML
int js_nat_init_html(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
int js_nat_init_sys(JSContext* ctx);
int js_nat_init_async(JSContext* ctx);
int js_nat_init_scrape(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_BENCH
int js_nat_init_bench(JSContext* ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_OAUTH2
int js_nat_init_oauth2(JSContext* ctx);
#endif

int js_nat_init_all(JSContext* ctx);

#endif
#endif
