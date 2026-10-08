#include "dyna-nat.h"
#include "dyna-libc.h"

#ifdef CONFIG_NATIVE_MODULES

#include "core/dyn-ds.h"
#include "core/dyn-prng.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_NAT_MAGIC ((size_t)0x64796e61746e6174ULL)
#define DYN_NAT_FAULT UINT64_MAX

typedef struct {
    size_t size;
    size_t magic;
} dyn_nat_hdr;

static _Atomic uint64_t dyn_nat_live_bytes;
static _Atomic uint64_t dyn_nat_byte_limit;
static _Atomic uint64_t dyn_nat_operator_ceiling;

int dyn_nat_track(size_t size)
{
    uint64_t limit = atomic_load_explicit(&dyn_nat_byte_limit,
        memory_order_relaxed);
    uint64_t live = atomic_load_explicit(&dyn_nat_live_bytes,
        memory_order_relaxed);
    uint64_t loose = limit ? dyn_trk_bytes() : 0;
    if (limit && loose >= limit)
        return -1;
    if (limit)
        limit -= loose;
    for (;;) {
        if (live == DYN_NAT_FAULT || (uint64_t)size > UINT64_MAX - live
            || (limit && (live >= limit || (uint64_t)size > limit - live)))
            return -1;
        if (atomic_compare_exchange_weak_explicit(&dyn_nat_live_bytes,
                &live, live + (uint64_t)size,
                memory_order_relaxed, memory_order_relaxed))
            return 0;
    }
}

void dyn_nat_untrack(size_t size)
{
    uint64_t live = atomic_load_explicit(&dyn_nat_live_bytes,
        memory_order_relaxed);
    for (;;) {
        uint64_t next = live == DYN_NAT_FAULT || live < (uint64_t)size
            ? DYN_NAT_FAULT
            : live - (uint64_t)size;
        if (atomic_compare_exchange_weak_explicit(&dyn_nat_live_bytes,
                &live, next, memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

void* dyn_nat_malloc(size_t size)
{
    dyn_nat_hdr* h;
    if (size > SIZE_MAX - sizeof(dyn_nat_hdr))
        return NULL;
    if (dyn_nat_track(size))
        return NULL;
    h = (dyn_nat_hdr*)malloc(sizeof(dyn_nat_hdr) + size);
    if (!h) {
        dyn_nat_untrack(size);
        return NULL;
    }
    h->size = size;
    h->magic = DYN_NAT_MAGIC;
    return h + 1;
}

void* dyn_nat_calloc(size_t nmemb, size_t size)
{
    void* p;
    if (size && nmemb > SIZE_MAX / size)
        return NULL;
    p = dyn_nat_malloc(nmemb * size);
    if (p)
        memset(p, 0, nmemb * size);
    return p;
}

void* dyn_nat_realloc(void* ptr, size_t size)
{
    dyn_nat_hdr *h, *nh;
    int is_grow;
    uint64_t delta;

    if (!ptr)
        return dyn_nat_malloc(size);
    h = (dyn_nat_hdr*)ptr - 1;
    assert(h->magic == DYN_NAT_MAGIC);
    if (size > SIZE_MAX - sizeof(dyn_nat_hdr))
        return NULL;
    is_grow = size > h->size;
    delta = is_grow ? (uint64_t)size - (uint64_t)h->size
                    : (uint64_t)h->size - (uint64_t)size;
    if (is_grow) {
        if (dyn_nat_track((size_t)delta))
            return NULL;
    }
    nh = (dyn_nat_hdr*)realloc(h, sizeof(dyn_nat_hdr) + size);
    if (!nh) {
        if (is_grow)
            dyn_nat_untrack((size_t)delta);
        return NULL;
    }
    if (!is_grow)
        dyn_nat_untrack((size_t)delta);
    nh->size = size;
    nh->magic = DYN_NAT_MAGIC;
    return nh + 1;
}

char* dyn_nat_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)dyn_nat_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void dyn_nat_free(void* ptr)
{
    dyn_nat_hdr* h;
    if (!ptr)
        return;
    h = (dyn_nat_hdr*)ptr - 1;
    assert(h->magic == DYN_NAT_MAGIC);
    dyn_nat_untrack(h->size);
    free(h);
}

static uint64_t dyn_nat_exact_bytes(void)
{
    uint64_t exact = atomic_load_explicit(&dyn_nat_live_bytes,
        memory_order_relaxed);
    return exact == DYN_NAT_FAULT ? UINT64_MAX : exact;
}

uint64_t dyn_nat_bytes(void)
{
    uint64_t exact = atomic_load_explicit(&dyn_nat_live_bytes,
        memory_order_relaxed);
    uint64_t loose = dyn_trk_bytes();
    return exact == DYN_NAT_FAULT || exact > UINT64_MAX - loose
        ? DYN_NAT_FAULT
        : exact + loose;
}

void dyn_nat_set_limit(uint64_t limit)
{
    atomic_store_explicit(&dyn_nat_operator_ceiling, limit,
        memory_order_relaxed);
    atomic_store_explicit(&dyn_nat_byte_limit, limit, memory_order_relaxed);
    dyn_trk_set_limit(limit);
}

int dyn_nat_set_script_limit(uint64_t limit, uint64_t* ceiling_out)
{
    uint64_t ceiling = atomic_load_explicit(&dyn_nat_operator_ceiling,
        memory_order_relaxed);

    if (ceiling_out)
        *ceiling_out = ceiling;
    if (ceiling && (limit == 0 || limit > ceiling))
        return -1;
    atomic_store_explicit(&dyn_nat_byte_limit, limit, memory_order_relaxed);
    dyn_trk_set_limit(limit);
    return 0;
}

uint64_t dyn_nat_limit(void)
{
    return atomic_load_explicit(&dyn_nat_byte_limit, memory_order_relaxed);
}

uint64_t dyn_nat_operator_limit(void)
{
    return atomic_load_explicit(&dyn_nat_operator_ceiling,
        memory_order_relaxed);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void dyn_nat_wmsg_boot(void)
{
    dyn_trk_set_peer(dyn_nat_exact_bytes);
    dyn_wmsg_set_allocator(dyn_nat_malloc, dyn_nat_free);
}
#endif

#define DYN_MAX_CLASSES 256
static JSClassID dyn_class_ids[DYN_MAX_CLASSES];
static int dyn_n_classes;
static pthread_mutex_t dyn_class_mu = PTHREAD_MUTEX_INITIALIZER;

static int dyn_is_our_class(JSClassID id)
{
    int i, found = 0;
    pthread_mutex_lock(&dyn_class_mu);
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == id) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&dyn_class_mu);
    return found;
}

static void dyn_res_release(JSRuntime* rt, DynResource* r)
{
    (void)rt;
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native);
    r->native = NULL;
}

void dyn_res_mark_closed(JSRuntime* rt, JSValue obj, JSClassID class_id)
{
    DynResource* r = JS_GetOpaque(obj, class_id);
    (void)rt;
    if (r) {
        r->closed = 1;
        r->native = NULL;
    }
}

void dyn_res_finalizer(JSRuntime* rt, JSValue val)
{
    JSClassID id;
    DynResource* r = JS_GetAnyOpaque(val, &id);
    if (r) {
        dyn_res_release(rt, r);
        dyn_nat_untrack(sizeof(DynResource));
        js_free_rt(rt, r);
    }
}

JSValue dyn_ctor_proto(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id)
{
    JSValue proto;

    if (!JS_IsObject(new_target))
        return JS_GetClassProto(ctx, class_id);
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        return proto;
    if (JS_IsObject(proto))
        return proto;
    JS_FreeValue(ctx, proto);
    return JS_GetClassProto(ctx, class_id);
}

JSValue dyn_res_wrap(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, void* native,
    DynDisposeFunc dispose)
{
    JSValue proto, obj;
    DynResource* r;

    proto = dyn_ctor_proto(ctx, new_target, class_id);
    if (JS_IsException(proto))
        goto fail;
    obj = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
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

DynResource* dyn_res_get(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    DynResource* r = JS_GetOpaque2(ctx, this_val, class_id);
    if (!r)
        return NULL;
    if (r->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return NULL;
    }
    return r;
}

void dyn_res_hold(JSContext* ctx, DynResource* r)
{
    if (r && !JS_IsNativeCallActive(ctx, r->use_seq))
        r->use_seq = JS_GetNativeCallSeq(ctx);
}

static JSValue dyn_method_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSClassID id;
    DynResource* r = JS_GetAnyOpaque(this_val, &id);
    (void)argc;
    (void)argv;
    if (r && dyn_is_our_class(id)) {
        if (!r->closed && r->use_seq != JS_GetNativeCallSeq(ctx) && JS_IsNativeCallActive(ctx, r->use_seq))
            return JS_ThrowTypeError(ctx, "cannot close a native resource while one of its methods is running");
        dyn_res_release(JS_GetRuntime(ctx), r);
    }
    return JS_UNDEFINED;
}

static JSValue dyn_getter_closed(JSContext* ctx, JSValueConst this_val)
{
    JSClassID id;
    DynResource* r = JS_GetAnyOpaque(this_val, &id);
    if (r && dyn_is_our_class(id))
        return JS_NewBool(ctx, r->closed);
    return JS_ThrowTypeError(ctx, "not a native resource");
}

static const JSCFunctionListEntry dyn_common_funcs[] = {
    JS_CFUNC_DEF("close", 0, dyn_method_close),
    JS_CFUNC_DEF("dispose", 0, dyn_method_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, dyn_method_close),
    JS_CGETSET_DEF("closed", dyn_getter_closed, NULL),
};

void dyn_res_class_common(JSContext* ctx, JSClassID class_id, JSValue proto)
{
    int i, known = 0;
    pthread_mutex_lock(&dyn_class_mu);
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == class_id) {
            known = 1;
            break;
        }
    if (!known && dyn_n_classes < DYN_MAX_CLASSES)
        dyn_class_ids[dyn_n_classes++] = class_id;
    pthread_mutex_unlock(&dyn_class_mu);
    JS_SetPropertyFunctionList(ctx, proto, dyn_common_funcs,
        countof(dyn_common_funcs));
}

JSValue dyn_plain_wrap(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, void* native,
    DynDisposeFunc dispose)
{
    JSValue proto, obj;

    proto = dyn_ctor_proto(ctx, new_target, class_id);
    if (JS_IsException(proto)) {
        if (dispose && native)
            dispose(native);
        return JS_EXCEPTION;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        if (dispose && native)
            dispose(native);
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

int dyn_register_plain_class(JSContext* ctx, JSModuleDef* m, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs, JSCFunction* ctor_fn,
    const char* name)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
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
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

int dyn_register_class(JSContext* ctx, JSModuleDef* m, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs, int n_funcs,
    JSCFunction* ctor_fn, const char* name)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
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

#ifndef CONFIG_NATIVE_MODULE_FILE
const char* dyn_path_borrow(JSContext* ctx, JSValueConst v, const char* what,
    size_t* plen)
{
    (void)v;
    (void)plen;
    JS_ThrowTypeError(ctx, "%s must be a Path, but dyna:file is not built in",
        what);
    return NULL;
}
int dyn_value_is_path(JSValueConst v)
{
    (void)v;
    return 0;
}
#endif

static void dyn_ds_seed_fallback(uint64_t seed[2])
{
    struct timespec ts = { 0, 0 };
    uint64_t sm = seed[0] ^ seed[1];
    clock_gettime(CLOCK_MONOTONIC, &ts);
    sm ^= (uint64_t)(uintptr_t)dyn_ds_seed_fallback;
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)(uintptr_t)seed;
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)(uintptr_t)&dyn_ds_hash_seed;
    sm = dyn_splitmix64(&sm);
    sm ^= (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    sm = dyn_splitmix64(&sm);
    seed[0] = dyn_splitmix64(&sm);
    seed[1] = dyn_splitmix64(&sm);
}

int js_nat_init_all(JSContext* ctx)
{
    {
        static atomic_int ds_seeded;
        int expected = 0;
        if (atomic_compare_exchange_strong(&ds_seeded, &expected, 1)) {
            uint64_t ds_seed[2] = { 0, 0 };
            if (dyn_os_entropy(ds_seed, sizeof ds_seed) < 0)
                dyn_ds_seed_fallback(ds_seed);
            dyn_ds_hash_seed(ds_seed[0], ds_seed[1]);
        }
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
#ifdef CONFIG_NATIVE_MODULE_STREAM
    if (js_nat_init_stream(ctx))
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
#ifdef CONFIG_NATIVE_MODULE_ASYNC
    if (js_nat_init_async(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SCRAPE
    if (js_nat_init_scrape(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_BENCH
    if (js_nat_init_bench(ctx))
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

#endif

JSAtom dyn_registered_symbol(JSContext* ctx, const char* key)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue sym_ctor = JS_GetPropertyStr(ctx, g, "Symbol");
    JSValue for_fn = JS_GetPropertyStr(ctx, sym_ctor, "for");
    JSValue k = JS_NewString(ctx, key);
    JSValue sym = JS_UNDEFINED;
    JSAtom a = JS_ATOM_NULL;

    if (JS_IsFunction(ctx, for_fn) && !JS_IsException(k))
        sym = JS_Call(ctx, for_fn, sym_ctor, 1, (JSValueConst*)&k);
    if (JS_IsSymbol(sym))
        a = JS_ValueToAtom(ctx, sym);
    else if (JS_IsException(sym) || JS_IsException(k))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, for_fn);
    JS_FreeValue(ctx, sym_ctor);
    JS_FreeValue(ctx, g);
    return a;
}
