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

#include "dyna-fiber.inc.c"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifdef CONFIG_NATIVE_MODULE_DATAFRAME
DynDfCsvReader* const dyn_df_csv_reader = dyn_df_read_csv;
DynDfCsvOutFrame* const dyn_df_csv_frame = dyn_df_csv_out_frame;
#else
DynDfCsvReader* const dyn_df_csv_reader = NULL;
DynDfCsvOutFrame* const dyn_df_csv_frame = NULL;
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

JSValue dyn_nat_ab_take(JSContext* ctx, void* data, size_t len)
{
    if (data && atomic_load_explicit(&dyn_trk_armed, memory_order_relaxed))
        dyn_trk_release(data);
    return JS_NewArrayBufferTake(ctx, data, len);
}

JSValue dyn_nat_new_string_wf(JSContext* ctx, const char* s, size_t len)
{
    const uint8_t* p = (const uint8_t*)s;
    size_t i, o, extra = 0;
    uint8_t* q;
    JSValue v = JS_NewStringLen(ctx, s, len);
    if (JS_IsException(v) || JS_GetNarrowStringBytes(ctx, v, NULL))
        return v;
    for (q = len ? (uint8_t*)memchr(p, 0xED, len) : NULL; q; q = (uint8_t*)memchr(q + 1, 0xED, len - (size_t)(q - p) - 1)) {
        i = (size_t)(q - p);
        if (i + 1 < len && p[i + 1] >= 0xA0 && p[i + 1] <= 0xBF)
            extra++;
    }
    if (!extra)
        return v;
    JS_FreeValue(ctx, v);
    if (extra > (SIZE_MAX - len) / 8)
        return JS_ThrowRangeError(ctx, "string too long");
    q = (uint8_t*)js_malloc(ctx, len + extra * 8 + 1);
    if (!q)
        return JS_EXCEPTION;
    for (i = 0, o = 0; i < len;) {
        if (p[i] == 0xED && i + 1 < len && p[i + 1] >= 0xA0 && p[i + 1] <= 0xBF) {
            size_t k = (i + 2 < len && p[i + 2] >= 0x80 && p[i + 2] <= 0xBF) ? 3 : 2;
            while (k--) {
                q[o++] = 0xEF;
                q[o++] = 0xBF;
                q[o++] = 0xBD;
                i++;
            }
        } else {
            q[o++] = p[i++];
        }
    }
    v = JS_NewStringLen(ctx, (const char*)q, o);
    js_free(ctx, q);
    return v;
}

JSValue dyn_nat_u8_copy(JSContext* ctx, const uint8_t* data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &zero_stub, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

size_t dyn_utf8_lossy_into(const uint8_t* p, size_t n, uint8_t* out, int* changed)
{
    size_t i = 0, o = 0;
    *changed = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len = 1, lo = 0x80, hi = 0xBF, j, bad;
        if (c < 0x80) {
            if (out)
                out[o] = c;
            o++;
            i++;
            continue;
        }
        if (c < 0xC2)
            bad = 1;
        else if (c < 0xE0) {
            len = 2;
            bad = 0;
        } else if (c < 0xF0) {
            len = 3;
            lo = (c == 0xE0) ? 0xA0 : 0x80;
            hi = (c == 0xED) ? 0x9F : 0xBF;
            bad = 0;
        } else if (c < 0xF5) {
            len = 4;
            lo = (c == 0xF0) ? 0x90 : 0x80;
            hi = (c == 0xF4) ? 0x8F : 0xBF;
            bad = 0;
        } else
            bad = 1;
        if (!bad) {
            for (j = 1; j < len; j++) {
                uint8_t jlo = (j == 1) ? lo : 0x80, jhi = (j == 1) ? hi : 0xBF;
                if (i + j >= n || p[i + j] < jlo || p[i + j] > jhi) {
                    bad = j;
                    break;
                }
            }
        }
        if (!bad) {
            if (out)
                memcpy(out + o, p + i, len);
            o += len;
            i += len;
        } else {
            if (out) {
                out[o] = 0xEF;
                out[o + 1] = 0xBF;
                out[o + 2] = 0xBD;
            }
            o += 3;
            i += bad;
            *changed = 1;
        }
    }
    return o;
}

static size_t dyn_utf8_ascii_prefix(const uint8_t* p, size_t n)
{
    size_t ascii = 0;
    while (ascii + 8 <= n) {
        uint64_t word;
        memcpy(&word, p + ascii, 8);
        if (word & 0x8080808080808080ULL)
            break;
        ascii += 8;
    }
    while (ascii < n && p[ascii] < 0x80)
        ascii++;
    return ascii;
}

int dyn_utf8_valid(const uint8_t* p, size_t n)
{
    size_t ascii = dyn_utf8_ascii_prefix(p, n);
    int changed;
    if (ascii == n)
        return 1;
#ifdef DYN_UTF8_LOSSY_HAS_SIMD
    return simd.validate_utf8(p, n) == n;
#else
    (void)dyn_utf8_lossy_into(p + ascii, n - ascii, NULL, &changed);
    return !changed;
#endif
}

JSValue dyn_utf8_lossy_string(JSContext* ctx, const uint8_t* p, size_t n)
{
    size_t o;
    int changed;
    uint8_t* out;
    JSValue ret;

    if (dyn_utf8_valid(p, n))
        return JS_NewStringLen(ctx, (const char*)p, n);
    out = (uint8_t*)js_malloc(ctx, n * 3 + 1);
    if (!out)
        return JS_EXCEPTION;
    o = dyn_utf8_lossy_into(p, n, out, &changed);
    ret = JS_NewStringLen(ctx, (const char*)out, o);
    js_free(ctx, out);
    return ret;
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

static void dyn_res_pin_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    DynResource* r = (DynResource*)opaque;
    JSValue v = r->pin;
    (void)ctx;
    r->pin = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
}

static void dyn_res_unpin_r(JSRuntime* rt, DynResource* r)
{
    JSValue v = r->pin;
    if (JS_IsUndefined(v))
        return;
    r->pin = JS_UNDEFINED;
    JS_RemoveShutdownSweep(rt, dyn_res_pin_sweep, r);
    JS_FreeValueRT(rt, v);
}

int dyn_res_pin(JSContext* ctx, JSValueConst obj, JSClassID class_id)
{
    DynResource* r = JS_GetOpaque(obj, class_id);
    if (!r || r->closed || !JS_IsUndefined(r->pin))
        return 0;
    if (JS_AddShutdownSweep(JS_GetRuntime(ctx), dyn_res_pin_sweep, r) < 0)
        return -1;
    r->pin = JS_DupValue(ctx, obj);
    return 0;
}

void dyn_res_unpin(JSRuntime* rt, JSValueConst obj, JSClassID class_id)
{
    DynResource* r = JS_GetOpaque(obj, class_id);
    if (r)
        dyn_res_unpin_r(rt, r);
}

static void dyn_res_release(JSRuntime* rt, DynResource* r)
{
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native);
    r->native = NULL;
    dyn_res_unpin_r(rt, r);
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
    r->pin = JS_UNDEFINED;
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
        if (!r->closed && r->async_users != 0)
            return JS_ThrowTypeError(ctx, "cannot close a native resource while an async call on it is in flight; await it first");
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
    JSAtom close_atom = JS_NewAtom(ctx, "close");
    int own = close_atom == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(ctx, NULL, proto, close_atom);
    JS_FreeAtom(ctx, close_atom);
    if (own != 0) {
        if (own < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    pthread_mutex_lock(&dyn_class_mu);
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == class_id) {
            known = 1;
            break;
        }
    if (!known && dyn_n_classes < DYN_MAX_CLASSES)
        dyn_class_ids[dyn_n_classes++] = class_id;
    pthread_mutex_unlock(&dyn_class_mu);
    if (JS_SetPropertyFunctionList(ctx, proto, dyn_common_funcs, countof(dyn_common_funcs)) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
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

JSValue dyn_tls_missing(JSContext* ctx, const char* who, const char* what)
{
    return JS_ThrowTypeError(ctx, "%s: this build has no TLS support: %s needs the optional \"tls\" module", who, what);
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
#ifdef CONFIG_NATIVE_MODULE_NETIP
    if (js_nat_init_netip(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_HTTP
    if (js_nat_init_http(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DNS
    if (js_nat_init_dns(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_PROXY
    if (js_nat_init_proxy(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_METRICS
    if (js_nat_init_metrics(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_POSTGRES
    if (js_nat_init_postgres(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_REDIS
    if (js_nat_init_redis(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SMTP
    if (js_nat_init_smtp(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_WIREGUARD
    if (js_nat_init_wireguard(ctx))
        return -1;
#endif
#if defined(CONFIG_NATIVE_MODULE_SQLITE) && defined(CONFIG_SQLITE)
    if (js_nat_init_sqlite(ctx))
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
#ifdef CONFIG_NATIVE_MODULE_VEC
    if (js_nat_init_vec(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SVG
    if (js_nat_init_svg(ctx))
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
#ifdef CONFIG_NATIVE_MODULE_OTP
    if (js_nat_init_otp(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_GRAPH
    if (js_nat_init_graph(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_PROTOBUF
    if (js_nat_init_protobuf(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ASN1
    if (js_nat_init_asn1(ctx))
        return -1;
#endif
#if defined(CONFIG_NATIVE_MODULE_DKIM) && defined(CONFIG_OPENSSL)
    if (js_nat_init_dkim(ctx))
        return -1;
#endif
#ifdef CONFIG_SDL3
    if (js_nat_init_sdl(ctx))
        return -1;
#endif
#ifdef CONFIG_RAYLIB
    if (js_nat_init_raylib(ctx))
        return -1;
#endif
#ifdef CONFIG_UI
    if (js_nat_init_ui(ctx))
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
#ifdef CONFIG_NATIVE_MODULE_SSH
    if (js_nat_init_ssh(ctx))
        return -1;
#endif
#if defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)
    if (js_nat_init_sftp(ctx))
        return -1;
#endif
#if defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)
    if (js_nat_init_webrtc(ctx))
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
#ifdef CONFIG_NATIVE_MODULE_DOCDB
    if (js_nat_init_docdb(ctx))
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
#if defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)
    if (js_nat_init_ai(ctx))
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

int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

int dyn_opt_bool(JSContext* ctx, JSValueConst opts, const char* name,
    const char* fn, int* out)
{
    JSValue v;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, name);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsBool(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: option %s must be a boolean", fn, name);
        return -1;
    }
    *out = JS_ToBool(ctx, v);
    return 0;
}

int dyn_opts_bag(JSContext* ctx, int argc, JSValueConst* argv, int idx,
    const char* const* keys, int nkeys, const char* fn)
{
    if (argc <= idx || JS_IsUndefined(argv[idx]) || JS_IsNull(argv[idx]))
        return 0;
    if (!JS_IsObject(argv[idx]) || JS_IsFunction(ctx, argv[idx])) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", fn);
        return -1;
    }
    return dyn_opts_strict(ctx, argv[idx], keys, nkeys);
}

JSValue dyn_bigint_to_number(JSContext* ctx, JSValue big)
{
    JSValue g, f, r;
    if (JS_IsException(big))
        return big;
    g = JS_GetGlobalObject(ctx);
    f = JS_GetPropertyStr(ctx, g, "Number");
    JS_FreeValue(ctx, g);
    r = JS_IsException(f) ? f : JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst*)&big);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, big);
    return r;
}
