/*
 * dynascript native modules -- shared framework (in-repo, no external deps).
 * See dyna-nat.h for the resource/ownership model.
 */
#include "dyna-nat.h"

#ifdef CONFIG_NATIVE_MODULES

#include "core/dyn-ds.h"      /* dyn_ds_hash_seed (audit 13.8.1) */
#include "core/dyn-prng.h"    /* dyn_os_entropy */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* The shared disk/network I/O primitives (dyn_iobuf_t and the dyn_io_ / dyn_net_
 * functions) now live in dyna-io.c (engine-core, always linked); dyna-nat.h
 * includes dyna-io.h so every module keeps seeing them. */

/* ---- Counted native allocator (audit E0208-01) --------------------------
 *
 * Every byte these entry points hand out is libc malloc underneath -- same
 * allocator, same semantics, same NULL-on-failure contract -- so adopting a
 * call site changes nothing about HOW the memory behaves, only about whether
 * the ledger sees it. See dyna-nat.h for the scope statement.
 *
 * Layout: a header sits in front of every payload recording the caller's
 * requested size, so free/realloc can balance the ledger without the caller
 * carrying sizes around and without platform-specific malloc_usable_size.
 * The header is a union with max_align_t and a 16-byte floor, so the payload
 * keeps the alignment malloc promised (SIMD-touching modules rely on it) and
 * is big enough for the size field on every supported ABI.
 *
 * The ledger is a plain unsigned count with relaxed ordering: it answers
 * "roughly how much native memory is live" and gates a soft cap. Two worker
 * threads racing one alloc/free pair can transiently mis-order the
 * add/subtract (the counter stays correct in total for matched pairs); the
 * check-then-claim in dyn_nat_track can likewise admit two racing allocations
 * that together overshoot the cap by one allocation. A hard, race-free cap
 * would need a lock on every module allocation -- the wrong trade for a
 * defensive limit. Caps are opt-in (the default is uncapped) precisely so an
 * embedder who wants the stronger guarantee opts into its cost. */

typedef union {
    size_t size;            /* the caller's requested byte count */
    max_align_t align;      /* payload alignment >= malloc's promise */
    unsigned char pad[16];  /* and >= 16 bytes of room for `size` */
} dyn_nat_hdr;

/* Live module-native bytes on the ledger, net of frees. */
static _Atomic uint64_t dyn_nat_live_bytes;
/* The cap: 0 = uncapped, which is the DEFAULT -- an engine limit
 * (JS_SetMemoryLimit) that already covers the embedder's needs must not have
 * a second, stricter limit silently appear under it. */
static _Atomic uint64_t dyn_nat_byte_limit;

/* Reserve `size` bytes against the cap and add them to the ledger. Returns 0,
 * or -1 when over the cap (nothing recorded). */
int dyn_nat_track(size_t size)
{
    uint64_t limit = atomic_load_explicit(&dyn_nat_byte_limit,
                                          memory_order_relaxed);
    if (limit) {
        uint64_t live = atomic_load_explicit(&dyn_nat_live_bytes,
                                             memory_order_relaxed);
        if (live >= limit || (uint64_t)size > limit - live)
            return -1;
    }
    atomic_fetch_add_explicit(&dyn_nat_live_bytes, (uint64_t)size,
                              memory_order_relaxed);
    return 0;
}

void dyn_nat_untrack(size_t size)
{
    atomic_fetch_sub_explicit(&dyn_nat_live_bytes, (uint64_t)size,
                              memory_order_relaxed);
}

void *dyn_nat_malloc(size_t size)
{
    dyn_nat_hdr *h;
    if (size > SIZE_MAX - sizeof(dyn_nat_hdr))
        return NULL;                    /* would overflow the header math */
    if (dyn_nat_track(size))
        return NULL;                    /* over cap */
    h = (dyn_nat_hdr *)malloc(sizeof(dyn_nat_hdr) + size);
    if (!h) {
        dyn_nat_untrack(size);          /* malloc lost the race, not the cap */
        return NULL;
    }
    h->size = size;
    return h + 1;
}

void *dyn_nat_calloc(size_t nmemb, size_t size)
{
    void *p;
    if (size && nmemb > SIZE_MAX / size)
        return NULL;                    /* would overflow the multiply */
    p = dyn_nat_malloc(nmemb * size);
    if (p)
        memset(p, 0, nmemb * size);
    return p;
}

void *dyn_nat_realloc(void *ptr, size_t size)
{
    dyn_nat_hdr *h, *nh;

    if (!ptr)
        return dyn_nat_malloc(size);
    h = (dyn_nat_hdr *)ptr - 1;
    if (size > SIZE_MAX - sizeof(dyn_nat_hdr))
        return NULL;
    /* Reserve the DELTA first so a capped refusal leaves the ledger and the
     * original block exactly as they were. */
    if (size > h->size) {
        if (dyn_nat_track(size - h->size))
            return NULL;
    } else {
        dyn_nat_untrack(h->size - size);
    }
    nh = (dyn_nat_hdr *)realloc(h, sizeof(dyn_nat_hdr) + size);
    if (!nh) {
        /* realloc(3) kept the original block: undo this call's ledger delta. */
        if (size > h->size)
            dyn_nat_untrack(size - h->size);
        else
            dyn_nat_track(h->size - size);
        return NULL;
    }
    nh->size = size;
    return nh + 1;
}

char *dyn_nat_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)dyn_nat_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void dyn_nat_free(void *ptr)
{
    dyn_nat_hdr *h;
    if (!ptr)
        return;
    h = (dyn_nat_hdr *)ptr - 1;
    dyn_nat_untrack(h->size);
    free(h);
}

uint64_t dyn_nat_bytes(void)
{
    return atomic_load_explicit(&dyn_nat_live_bytes, memory_order_relaxed);
}

void dyn_nat_set_limit(uint64_t limit)
{
    atomic_store_explicit(&dyn_nat_byte_limit, limit, memory_order_relaxed);
}

uint64_t dyn_nat_limit(void)
{
    return atomic_load_explicit(&dyn_nat_byte_limit, memory_order_relaxed);
}

/* Registry of framework-owned class ids, so the shared close()/closed methods
 * can validate `this` before treating its opaque as a DynResource. A foreign
 * object passed via close.call(x) must never be reinterpreted (it may store a
 * non-pointer opaque). Registration happens only from js_nat_init_all() on the
 * main context at startup -- worker threads never register modules -- so this
 * table is not concurrently mutated. Class ids are process-unique and never
 * reset; the cap is sized well beyond the number of native classes. */
#define DYN_MAX_CLASSES 256
static JSClassID dyn_class_ids[DYN_MAX_CLASSES];
static int dyn_n_classes;

static int dyn_is_our_class(JSClassID id)
{
    int i;
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == id)
            return 1;
    return 0;
}

/* Idempotent teardown: run the module dispose exactly once. */
static void dyn_res_release(JSRuntime *rt, DynResource *r)
{
    (void)rt;
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native);
    r->native = NULL;
}

/* Mark the resource closed WITHOUT running its dispose: for a completion
   that has already torn the native down itself (with its own reason), so
   the later finalizer is a no-op instead of a second dispose. */
void dyn_res_mark_closed(JSRuntime *rt, JSValue obj, JSClassID class_id)
{
    DynResource *r = JS_GetOpaque(obj, class_id);
    (void)rt;
    if (r) {
        r->closed = 1;
        r->native = NULL;
    }
}

void dyn_res_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(val, &id);
    /* only reached for our classes (set in their JSClassDef), so `r` is ours */
    if (r) {
        dyn_res_release(rt, r);
        dyn_nat_untrack(sizeof(DynResource)); /* the box's module-ledger entry */
        js_free_rt(rt, r);
    }
}

JSValue dyn_res_wrap(JSContext *ctx, JSClassID class_id, void *native,
                     DynDisposeFunc dispose)
{
    JSValue obj;
    DynResource *r;

    obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj))
        goto fail;
    /* The box itself is engine memory (js_mallocz), so a configured engine
     * limit keeps seeing exactly what it saw before. It is ALSO module-native
     * memory -- it lives outside the JS heap for as long as the resource does
     * -- so it is tracked on the module ledger, which makes every dyna:*
     * resource visible to memoryUsage().nativeSize and refuseable by the
     * native cap: a resource flood is capped even with the engine limit
     * unset. The double entry (~40 bytes/resource) is deliberate and
     * documented in memoryUsage(). */
    r = js_mallocz(ctx, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    if (dyn_nat_track(sizeof(DynResource))) {
        js_free_rt(JS_GetRuntime(ctx), r);
        JS_FreeValue(ctx, obj);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    r->native = native;
    r->dispose = dispose;
    r->closed = 0;
    JS_SetOpaque(obj, r);
    return obj;
 fail:
    if (dispose && native)
        dispose(native);
    return JS_EXCEPTION;
}

DynResource *dyn_res_get(JSContext *ctx, JSValueConst this_val,
                         JSClassID class_id)
{
    DynResource *r = JS_GetOpaque2(ctx, this_val, class_id);
    if (!r)
        return NULL; /* JS_GetOpaque2 already threw */
    if (r->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return NULL;
    }
    return r;
}

/* close() / dispose(): explicit deterministic release. Safe for any `this`. */
static JSValue dyn_method_close(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    (void)argc; (void)argv;
    if (r && dyn_is_our_class(id))
        dyn_res_release(JS_GetRuntime(ctx), r);
    return JS_UNDEFINED;
}

static JSValue dyn_getter_closed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    if (r && dyn_is_our_class(id))
        return JS_NewBool(ctx, r->closed);
    return JS_ThrowTypeError(ctx, "not a native resource");
}

/* close/dispose/[Symbol.dispose]/closed installed on every resource proto.
 * [Symbol.dispose] makes these work with `using` and DisposableStack.use(). */
static const JSCFunctionListEntry dyn_common_funcs[] = {
    JS_CFUNC_DEF("close", 0, dyn_method_close),
    JS_CFUNC_DEF("dispose", 0, dyn_method_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, dyn_method_close),
    JS_CGETSET_DEF("closed", dyn_getter_closed, NULL),
};

void dyn_res_class_common(JSContext *ctx, JSClassID class_id, JSValue proto)
{
    if (dyn_n_classes < DYN_MAX_CLASSES)
        dyn_class_ids[dyn_n_classes++] = class_id;
    JS_SetPropertyFunctionList(ctx, proto, dyn_common_funcs,
                               countof(dyn_common_funcs));
}

/* ---- Plain GC-managed classes (no close/dispose surface) ---- */

JSValue dyn_plain_wrap(JSContext *ctx, JSClassID class_id, void *native,
                       DynDisposeFunc dispose)
{
    JSValue obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj)) {
        if (dispose && native)
            dispose(native);
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

int dyn_register_plain_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                             const JSClassDef *def,
                             const JSCFunctionListEntry *proto_funcs,
                             int n_funcs, JSCFunction *ctor_fn,
                             const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0) {
        /* UNREACHED today: no two modules share a register fn except
         * dyn_http_register, which has its own recovery. Kept because the
         * shape recurs (dyna:http/dyna:net was exactly this) and the failure
         * it prevents -- dead exports in TDZ -- fails silently. */
        proto = JS_GetClassProto(ctx, *pid);
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        return JS_SetModuleExport(ctx, m, name, ctor);
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    /* NB: no dyn_res_class_common -- a plain class carries no close surface. */
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                       const JSClassDef *def,
                       const JSCFunctionListEntry *proto_funcs, int n_funcs,
                       JSCFunction *ctor_fn, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0) {
        proto = JS_GetClassProto(ctx, *pid);
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        return JS_SetModuleExport(ctx, m, name, ctor);
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    dyn_res_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

/* Path lives in dyna-file.c. When that family is not compiled there is no Path
 * class at all, so every other module's path argument is unsatisfiable -- and
 * saying so is better than failing to link. */
#ifndef CONFIG_NATIVE_MODULE_FILE
const char *dyn_path_borrow(JSContext *ctx, JSValueConst v, const char *what,
                            size_t *plen)
{
    (void)v; (void)plen;
    JS_ThrowTypeError(ctx, "%s must be a Path, but dyna:file is not built in",
                      what);
    return NULL;
}
int dyn_value_is_path(JSValueConst v) { (void)v; return 0; }
#endif

/* Fallback seed for E11-06: an entropy-source failure must not leave the
 * flood defense seeded from uninitialized memory -- getrandom's error path
 * can return with the buffer partially written, and fresh-frame stack garbage
 * may be predictable, which is exactly the seed a key-flooding attacker needs.
 * The fallback mixes whatever partial bytes the kernel did write (real
 * entropy; zeros when it wrote none) with ASLR-dependent addresses and the
 * monotonic clock, folding each input through a SplitMix64 step like a
 * splitmix stream. Attacker model is a REMOTE script author: neither pointer
 * values nor monotonic time are observable from script, so this is a real --
 * if weaker -- entropy source. Kernel entropy remains the primary source; no
 * more than that is claimed here. */
static void dyn_ds_seed_fallback(uint64_t seed[2])
{
    struct timespec ts = {0, 0};
    uint64_t sm = seed[0] ^ seed[1];
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sm ^= (uint64_t)(uintptr_t)dyn_ds_seed_fallback;  /* code address: ASLR */
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)(uintptr_t)seed;                  /* stack address: ASLR */
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)(uintptr_t)&dyn_ds_hash_seed;     /* data address: ASLR */
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    sm = dyn_splitmix64(&sm);                         /* absorb before expand */
    seed[0] = dyn_splitmix64(&sm);
    seed[1] = dyn_splitmix64(&sm);
}

int js_nat_init_all(JSContext *ctx)
{
    /* Key the dyn-ds hash tables per process (audit 13.8.1): a fixed seed is
       collision-floodable with precomputed keys. Seeded once; the first
       runtime (the main context) wins, worker-context inits no-op. */
    {
        /* zeroed, so a partially-written failed getrandom leaves the
           fallback's inputs deterministic instead of stack garbage */
        uint64_t ds_seed[2] = {0, 0};
        if (dyn_os_entropy(ds_seed, sizeof ds_seed) < 0)
            dyn_ds_seed_fallback(ds_seed);
        dyn_ds_hash_seed(ds_seed[0], ds_seed[1]);
    }
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES
    if (js_nat_init_structures(ctx))
        return -1;
    if (js_nat_init_structures_ext(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_NET
    if (js_nat_init_net(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
    if (js_nat_init_ml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
    if (js_nat_init_compress(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
    if (js_nat_init_random(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
    if (js_nat_init_structures3(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
    if (js_nat_init_simd(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
    if (js_nat_init_file(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SEMVER
    if (js_nat_init_semver(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_BYTES
    if (js_nat_init_bytes(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CRYPTO
    if (js_nat_init_hash(ctx))
        return -1;
    if (js_nat_init_crypto(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_MATCHER
    if (js_nat_init_matcher(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ENCODING
    if (js_nat_init_encoding(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TIME
    if (js_nat_init_time(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_MATHX
    if (js_nat_init_mathx(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CSV
    if (js_nat_init_csv(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DATAFRAME
    if (js_nat_init_dataframe(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_UUID
    if (js_nat_init_uuid(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CONFIG
    if (js_nat_init_config(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_LOG
    if (js_nat_init_log(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_URL
    if (js_nat_init_url(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TERM
    if (js_nat_init_term(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_VALIDATE
    if (js_nat_init_validate(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_JSON
    if (js_nat_init_json(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SCHEMA
    if (js_nat_init_schema(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_XML
    if (js_nat_init_xml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_YAML
    if (js_nat_init_yaml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DECIMAL
    if (js_nat_init_decimal(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_VSERIALIZE
    if (js_nat_init_vserialize(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_HTML
    if (js_nat_init_html(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
    if (js_nat_init_sys(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SCRAPE
    if (js_nat_init_scrape(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_OAUTH2
    if (js_nat_init_oauth2(ctx))
        return -1;
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
    if (js_nat_init_uring(ctx))
        return -1;
#endif
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES */
