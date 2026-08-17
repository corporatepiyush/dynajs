/*
 * dynascript native modules -- self-contained, in-repo (no external deps).
 *
 * This is the shared framework every native `dyna:*` module builds on. It is a
 * from-scratch, simplified replacement for the old secure-c-libs binding layer:
 * there is NO per-object arena and NO external allocator. A native resource is
 * just an opaque pointer plus a dispose callback that frees whatever the module
 * allocated (with plain libc malloc/free, which is thread-safe -- important for
 * modules like the HTTP server whose worker threads allocate concurrently).
 *
 * The other half of that rule: memory the ENGINE hands you -- a property table
 * from JS_GetOwnPropertyNames, a JS_ToCString buffer -- is engine-allocated and
 * goes back through js_free/JS_FreeCString, never libc free.
 *
 * Memory model: a JS wrapper object owns one native pointer. JavaScript releases
 * it deterministically via .close()/.dispose()/[Symbol.dispose]; the class
 * finalizer is only a safety net for a leaked object. Native results are copied
 * into JS values at the call boundary -- no native pointer escapes into the JS
 * heap. Every method must coerce its JS args to C locals FIRST, then resolve the
 * native handle (dyn_res_get, which rejects a closed resource), with no
 * JS-invoking call in between (coercion can run user JS that close()s `this`).
 */
#ifndef DYNAJS_NAT_H
#define DYNAJS_NAT_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <sys/types.h>   /* off_t */

/* The shared disk/network I/O substrate: dyn_iobuf_t plus the dyn_io_ and
 * dyn_net_ primitives (read_whole, preallocate, durable_sync, slurp, ...).
 * Engine-core (always linked), used by dyna:file, dyna:csv, ... */
#include "dyna-io.h"

/* Teardown callback a module supplies: free `native` and everything it owns. */
typedef void (*DynDisposeFunc)(void *native);

/* Opaque payload of every native-resource JS object. */
typedef struct {
    void *native;             /* the module's native object (module-owned) */
    DynDisposeFunc dispose;   /* module teardown; may be NULL */
    int closed;               /* 1 once released (idempotent) */
} DynResource;

/* Wrap `native` as a JS object of class `class_id` whose proto already carries
 * close()/[Symbol.dispose]/closed. Takes ownership: on any later disposal the
 * framework runs `dispose(native)`. On error runs `dispose(native)` and returns
 * JS_EXCEPTION (so the caller never double-frees). */
JSValue dyn_res_wrap(JSContext *ctx, JSClassID class_id, void *native,
                     DynDisposeFunc dispose);

/* Fetch the live resource for `this`, or throw (and return NULL) if the object
 * is closed or of the wrong class. */
DynResource *dyn_res_get(JSContext *ctx, JSValueConst this_val,
                         JSClassID class_id);

/* Mark a resource closed WITHOUT running its dispose: for a completion that
 * already tore the native down itself, so the later finalizer is a no-op
 * instead of a second dispose. Safe on any object. */
void dyn_res_mark_closed(JSRuntime *rt, JSValue obj, JSClassID class_id);

/* ---- Plain GC-managed classes (for pure data structures) ----
 *
 * A "plain" native class is an ORDINARY JS object: no .close()/.closed/
 * [Symbol.dispose] surface. The native pointer is the object's opaque directly
 * (no DynResource box). The JS GC owns its lifetime exactly like a Map/Set --
 * the object is reclaimed when unreachable (refcount 0 for the acyclic case;
 * the cycle collector otherwise). The caller's JSClassDef MUST supply a
 * `.finalizer` (frees the native) and, if the native holds JSValues, a
 * `.gc_mark` (marks them so the cycle collector can trace/reclaim cycles). */

/* Wrap `native` as a plain object of `class_id` (opaque = native). On object-
 * creation failure, runs `dispose(native)` and returns JS_EXCEPTION. */
JSValue dyn_plain_wrap(JSContext *ctx, JSClassID class_id, void *native,
                       DynDisposeFunc dispose);

/* Resolve `this` to its native pointer, or NULL (throwing TypeError for a
 * wrong-class receiver). No "closed" state exists -- the object is live until
 * the GC frees it. */
static inline void *dyn_plain_get(JSContext *ctx, JSValueConst this_val,
                                  JSClassID class_id)
{
    return JS_GetOpaque2(ctx, this_val, class_id);
}

/* An index argument, honest about the range. ToUint32(2^32+k) wraps to k BY
 * SPEC, so a wrapped index silently aliases a small real one past every
 * bounds guard (measured: set(2^32+3) set bit 3, update(2^32+3) wrote index
 * 3, RingBuffer(2^32+16) built a 16-slot buffer). Out-of-uint32 values throw
 * RangeError; negatives map to UINT32_MAX so the caller's ordinary bounds
 * check answers them as out-of-range (undefined where that is the documented
 * reply, RangeError where the method grows). Non-integers throw. */
static inline int dyn_idx_arg(JSContext *ctx, JSValueConst v, uint32_t *out)
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

/* Register a plain class (id/class/proto/ctor) and export it from module `m`.
 * Does NOT install any close/dispose surface. Returns 0 or -1. */
int dyn_register_plain_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                             const JSClassDef *def,
                             const JSCFunctionListEntry *proto_funcs,
                             int n_funcs, JSCFunction *ctor_fn,
                             const char *name);

/* Convenience: resolve `this` to its live native pointer, or NULL (throwing). */
static inline void *dyn_res_native(JSContext *ctx, JSValueConst this_val,
                                   JSClassID class_id)
{
    DynResource *r = dyn_res_get(ctx, this_val, class_id);
    return r ? r->native : NULL;
}

/* Install close()/dispose()/[Symbol.dispose]/`closed` on `proto` and register
 * `class_id` as framework-owned. Call once per class from the module init. */
void dyn_res_class_common(JSContext *ctx, JSClassID class_id, JSValue proto);

/* The shared finalizer (disposes if still open). Reference it from JSClassDef. */
void dyn_res_finalizer(JSRuntime *rt, JSValue val);

/* Helper for classes: create id, class, proto (with common funcs + protos),
 * constructor, and export it from module `m`. Returns 0 or -1. */
int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                       const JSClassDef *def,
                       const JSCFunctionListEntry *proto_funcs, int n_funcs,
                       JSCFunction *ctor_fn, const char *name);

/* ---- Path, the cross-module value handle --------------------------------
 *
 * `dyna:file` owns the Path class, but `dyna:csv`, `dyna:uring`, `dyna:http`
 * and `dyna:ml` all take paths too, and a second class id would mean a Path
 * built by one module was not a Path to another. So the class id lives in
 * dyna-file.c and every other module borrows through these two functions.
 *
 * dyn_path_borrow returns the handle's NORMALISED, NUL-TERMINATED bytes with no
 * copy and nothing to free, or NULL having thrown a TypeError. The pointer is
 * valid for as long as the JSValue is reachable -- which, for an argument, is
 * the whole call, because argv roots it. A Path has no close(), so unlike a
 * resource there is no way for user JS to invalidate the pointer mid-call;
 * that is what makes borrowing safe here and not elsewhere (CLAUDE.md sec.8).
 *
 * `what` names the parameter for the error message ("path", "root", ...).
 * `plen` may be NULL. */
const char *dyn_path_borrow(JSContext *ctx, JSValueConst v, const char *what,
                            size_t *plen);
int dyn_value_is_path(JSValueConst v);

/* Per-family module initializers (each defined in its own translation unit).
 * A family is compiled in iff its CONFIG_NATIVE_MODULE_<X> flag is set. */
int js_nat_init_structures(JSContext *ctx);
int js_nat_init_structures_ext(JSContext *ctx);
/* Registered by src/dyna-graph.c into dyna:structures. */
int dyn_graph_register(JSContext *ctx, JSModuleDef *m);
void dyn_graph_add_exports(JSContext *ctx, JSModuleDef *m);
#ifdef CONFIG_NATIVE_MODULE_NET
/* dyna:net is registered by src/dyna-net.c; each capability lives in its own
 * translation unit and registers into that one module. */
int js_nat_init_net(JSContext *ctx);
int dyn_netip_register(JSContext *ctx, JSModuleDef *m);
int dyn_netip_add_exports(JSContext *ctx, JSModuleDef *m);
int dyn_http_register(JSContext *ctx, JSModuleDef *m);
void dyn_http_add_exports(JSContext *ctx, JSModuleDef *m);
/* The one reactor every dyna:net object shares; see src/dyna-net.c. */
struct dyn_aio;
struct dyn_aio *dyn_net_reactor_acquire(JSContext *ctx);
void dyn_net_reactor_release(JSContext *ctx);
/* Run `fn(udata)` after every reactor drain -- for work that must happen on the
 * loop thread but is not reactor IO (a timeout sweep). Keyed by udata. */
int dyn_net_on_drain(void (*fn)(void *), void *udata);
void dyn_net_off_drain(void *udata);
int dyn_tcp_register(JSContext *ctx, JSModuleDef *m);
void dyn_tcp_add_exports(JSContext *ctx, JSModuleDef *m);
int dyn_proxy_register(JSContext *ctx, JSModuleDef *m);
void dyn_proxy_add_exports(JSContext *ctx, JSModuleDef *m);
int dyn_dns_register(JSContext *ctx, JSModuleDef *m);
int dyn_ratelimit_register(JSContext *ctx, JSModuleDef *m);
int dyn_metrics_register(JSContext *ctx, JSModuleDef *m);
void dyn_dns_add_exports(JSContext *ctx, JSModuleDef *m);
void dyn_ratelimit_add_exports(JSContext *ctx, JSModuleDef *m);
void dyn_metrics_add_exports(JSContext *ctx, JSModuleDef *m);
/* The C half of Metrics: lock-free bumps from surfaces with no JS in the
   loop (the HTTP request path). kind is a DYN_MET_* value; an invalid name,
   a full registry or a type clash DROPS the observation -- telemetry never
   fails the work it measures. */
enum { DYN_MET_COUNTER, DYN_MET_GAUGE, DYN_MET_HISTOGRAM };
void dyn_metrics_c_record(int kind, const char *name, double value);
char *dyn_metrics_c_scrape(size_t *out_len); /* malloc'd; caller frees */
/* Redis and PostgreSQL speak their protocols directly and depend on no external
 * library, so they are NOT behind CONFIG_SQLITE. Declaring them inside that
 * guard built fine on a host with sqlite and failed on one without -- the
 * definitions were always compiled, only the declarations disappeared. */
int dyn_redis_register(JSContext *ctx, JSModuleDef *m);
void dyn_redis_add_exports(JSContext *ctx, JSModuleDef *m);
int dyn_pg_register(JSContext *ctx, JSModuleDef *m);
void dyn_pg_add_exports(JSContext *ctx, JSModuleDef *m);
#ifdef CONFIG_SQLITE
int dyn_sqlite_register(JSContext *ctx, JSModuleDef *m);
void dyn_sqlite_add_exports(JSContext *ctx, JSModuleDef *m);
#endif
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
int js_nat_init_ml(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
int js_nat_init_compress(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
int js_nat_init_random(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
int js_nat_init_structures3(JSContext *ctx);
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
int js_nat_init_simd(JSContext *ctx);
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
int js_nat_init_uring(JSContext *ctx); /* dyna:uring disk I/O (Linux only) */
/* io_uring high-queue-depth whole-file read (0 ok, caller free()s *out). */
int dyn_uring_read_all(const char *path, char **out, size_t *outlen);
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
int js_nat_init_file(JSContext *ctx); /* dyna:file buffered reader/writer */
#endif
#ifdef CONFIG_NATIVE_MODULE_SEMVER
int js_nat_init_semver(JSContext *ctx); /* dyna:semver SemVer 2.0.0 + ranges */
#endif
#ifdef CONFIG_NATIVE_MODULE_BYTES
int js_nat_init_bytes(JSContext *ctx); /* dyna:bytes byte-buffer utilities */
#endif
#ifdef CONFIG_NATIVE_MODULE_CRYPTO
/* Both live in src/dyna-crypto.c: one argument-coercion boundary, two module
 * namespaces split by whether the operation depends on a secret. */
int js_nat_init_hash(JSContext *ctx);   /* dyna:hash digests/CRC/xxhash/Hasher */
int js_nat_init_crypto(JSContext *ctx); /* dyna:crypto Hmac/KDF/randomBytes */
#endif
#ifdef CONFIG_NATIVE_MODULE_MATCHER
int js_nat_init_matcher(JSContext *ctx); /* dyna:matcher Matcher/MultiMatcher */
#endif
#ifdef CONFIG_NATIVE_MODULE_ENCODING
int js_nat_init_encoding(JSContext *ctx); /* dyna:encoding hex/base32/base64/varint codecs */
#endif
#ifdef CONFIG_NATIVE_MODULE_TIME
int js_nat_init_time(JSContext *ctx); /* dyna:time durations/clock/RFC3339 */
#endif
#ifdef CONFIG_NATIVE_MODULE_MATHX
int js_nat_init_mathx(JSContext *ctx); /* dyna:mathx math + int/BigInt helpers */
#endif
#ifdef CONFIG_NATIVE_MODULE_CSV
int js_nat_init_csv(JSContext *ctx); /* dyna:csv file CRUD, SIMD parse + atomic I/O */
#endif
#ifdef CONFIG_NATIVE_MODULE_DATAFRAME
int js_nat_init_dataframe(JSContext *ctx); /* dyna:dataframe columnar TypedArray frames */
#endif
#ifdef CONFIG_NATIVE_MODULE_UUID
int js_nat_init_uuid(JSContext *ctx); /* dyna:uuid RFC 9562 UUID v4/v7/v3/v5 + parse, NanoID, ULID */
#endif
#ifdef CONFIG_NATIVE_MODULE_CONFIG
int js_nat_init_config(JSContext *ctx); /* dyna:config INI/.env/front-matter */
#endif
#ifdef CONFIG_NATIVE_MODULE_LOG
int js_nat_init_log(JSContext *ctx); /* dyna:log leveled structured logging */
#endif
#ifdef CONFIG_NATIVE_MODULE_URL
int js_nat_init_url(JSContext *ctx); /* dyna:url RFC 3986 URL + form codec */
#endif
#ifdef CONFIG_NATIVE_MODULE_TERM
int js_nat_init_term(JSContext *ctx); /* dyna:cli argv parsing, styling, TTY */
#endif
#ifdef CONFIG_NATIVE_MODULE_VALIDATE
int js_nat_init_validate(JSContext *ctx); /* dyna:validate format validators */
#endif
#ifdef CONFIG_NATIVE_MODULE_JSON
int js_nat_init_json(JSContext *ctx); /* dyna:json RFC 6901 pointer / RFC 6902 patch */
#endif
#ifdef CONFIG_NATIVE_MODULE_SCHEMA
int js_nat_init_schema(JSContext *ctx); /* dyna:schema JSON Schema 2020-12 core validator */
#endif
#ifdef CONFIG_NATIVE_MODULE_XML
int js_nat_init_xml(JSContext *ctx);      /* dyna:xml SAX, tree, serializer */
#endif
#ifdef CONFIG_NATIVE_MODULE_YAML
int js_nat_init_yaml(JSContext *ctx);     /* dyna:yaml core-schema subset */
#endif
#ifdef CONFIG_NATIVE_MODULE_DECIMAL
int js_nat_init_decimal(JSContext *ctx);  /* dyna:decimal exact arithmetic */
#endif
#ifdef CONFIG_NATIVE_MODULE_VSERIALIZE
int js_nat_init_vserialize(JSContext *ctx); /* dyna:serialize msgpack/cbor/structuredClone */
/* protobuf and ASN.1 join the dyna:serialize module (netip-inside-net idiom) */
int dyn_proto_register(JSContext *ctx, JSModuleDef *m);
int dyn_proto_add_exports(JSContext *ctx, JSModuleDef *m);
int dyn_asn1_register(JSContext *ctx, JSModuleDef *m);
int dyn_asn1_add_exports(JSContext *ctx, JSModuleDef *m);
#endif
#ifdef CONFIG_NATIVE_MODULE_HTML
int js_nat_init_html(JSContext *ctx);     /* dyna:html parse, select, sanitize */
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
int js_nat_init_sys(JSContext *ctx); /* dyna:sys filesystem/process/env interface */
int js_nat_init_scrape(JSContext *ctx); /* dyna:scrape robots/politeness policy */
#endif

/* Register every compiled-in native module in `ctx`. Called from the CLI. */
int js_nat_init_all(JSContext *ctx);

#endif /* CONFIG_NATIVE_MODULES */
#endif /* DYNAJS_NAT_H */
