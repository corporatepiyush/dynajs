#define DYN_ENGINE_CORE_TU
#include "engine-shared.h"

#define JS_MALLOC_MAX_EMPTY_ARENAS 1

static inline uint32_t atom_get_free(const JSAtomStruct* p)
{
    return (uintptr_t)p >> 1;
}

static inline BOOL atom_is_free(const JSAtomStruct* p)
{
    return (uintptr_t)p & 1;
}

static inline JSAtomStruct* atom_set_free(uint32_t v)
{
    return (JSAtomStruct*)(((uintptr_t)v << 1) | 1);
}

typedef struct JSShutdownSweepEntry {
    struct list_head link;
    JSShutdownSweepFunc func;
    void* opaque;
} JSShutdownSweepEntry;

typedef struct JSShutdownDeferEntry {
    struct list_head link;
    void* ptr;
} JSShutdownDeferEntry;

#define JS_SHUTDOWN_SWEEP_PASS_MAX 65536
#define JS_ATOM_MAX_INT (JS_ATOM_TAG_INT - 1)
#define JS_ATOM_MAX ((1U << 30) - 1)
#define JS_ATOM_COUNT_RESIZE(n) ((n) * 2)

static inline BOOL __JS_AtomIsConst(JSAtom v)
{
#if defined(DUMP_LEAKS) && DUMP_LEAKS > 1
    return (int32_t)v <= 0;
#else
    return (int32_t)v < JS_ATOM_END;
#endif
}

static inline BOOL is_num_string(uint32_t* pval, const JSString* p)
{
    uint32_t n;
    uint64_t n64;
    int c, i, len;

    len = p->len;
    if (len == 0 || len > 10)
        return FALSE;
    c = string_get(p, 0);
    if (is_num(c)) {
        if (c == '0') {
            if (len != 1)
                return FALSE;
            n = 0;
        } else {
            n = c - '0';
            for (i = 1; i < len; i++) {
                c = string_get(p, i);
                if (!is_num(c))
                    return FALSE;
                n64 = (uint64_t)n * 10 + (c - '0');
                if ((n64 >> 32) != 0)
                    return FALSE;
                n = n64;
            }
        }
        *pval = n;
        return TRUE;
    } else {
        return FALSE;
    }
}

typedef struct JSStringHash {
    uint64_t h;
    uint64_t key;
    uint64_t pend;
    uint32_t npend;
} JSStringHash;

static inline uint64_t hash_string_mix64(uint64_t x)
{
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

static inline uint32_t hash_string16_fast_narrow(const uint8_t* str, size_t len, uint32_t h)
{
    size_t i;
    for (i = 0; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static inline uint32_t hash_string8_mode(JSRuntime* rt, const uint8_t* str, size_t len, uint32_t domain, BOOL strong)
{
    if (unlikely(strong))
        return hash_string8_run(str, len, rt->atom_hash_init[domain], rt->atom_hash_key, rt->atom_hash_key2);
    return hash_string8_fast(str, len, (uint32_t)rt->atom_hash_seed ^ domain);
}

static inline uint32_t hash_string8(JSRuntime* rt, const uint8_t* str, size_t len, uint32_t domain)
{
    return hash_string8_mode(rt, str, len, domain, rt->atom_hash_strong);
}

static inline void hash_string_feed_unit(JSStringHash* st, uint32_t c)
{
    st->pend = (st->pend << 16) | c;
    if (++st->npend == HASH_STRING_BLOCK) {
        st->h = hash_string_step(st->h, st->key, ((uint64_t)HASH_STRING_BLOCK << 48) | st->pend);
        st->pend = 0;
        st->npend = 0;
    }
}

#define JS_UTF8_SIMD_MIN_LEN 64
#define JS_STRCMP_QUAD_MIN 4
#define JS_STRCMP_CHUNK_MIN 16
#define JS_STREQ_MEMCMP_MIN 8

typedef struct {
    JSValueConst stack[JS_STRING_ROPE_MAX_DEPTH];
    int stack_len;
} JSStringRopeIter;

#define ROPE_N_BUCKETS 44

static inline size_t get_shape_size(size_t hash_size, size_t prop_size)
{
    return sizeof(JSShape) + hash_size * sizeof(uint32_t) + prop_size * sizeof(JSShapeProperty);
}

static inline void js_shape_set_hash_size(JSShape* sh, uint32_t hash_size)
{
    assert(hash_size >= 2 && (hash_size & (hash_size - 1)) == 0);
    sh->prop_hash_mask = hash_size - 1;
}

static inline JSShape* js_new_shape_nohash(JSContext* ctx, JSObject* proto,
    int hash_size, int prop_size)
{
    JSRuntime* rt = ctx->rt;
    JSShape* sh;

    sh = js_malloc(ctx, get_shape_size(hash_size, prop_size));
    if (!sh)
        return NULL;
    js_rc(sh)->ref_count = 1;
    add_gc_object(rt, &sh->header, JS_GC_OBJ_TYPE_SHAPE);
    if (proto)
        JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, proto));
    sh->proto = proto;
    memset(sh->hash_table, 0, sizeof(sh->hash_table[0]) * hash_size);
    js_shape_set_hash_size(sh, hash_size);
    sh->prop_size = prop_size;
    sh->prop_count = 0;
    sh->deleted_prop_count = 0;
    sh->version = ++rt->shape_version;
    sh->is_hashed = FALSE;
#if CONFIG_FAST_PROP_TEARDOWN
    sh->all_plain = TRUE;
#endif
    return sh;
}

struct JSObjectTemplate {
    JSShape* sh;
    JSAtom* fields;
    uint32_t n;
};

typedef struct JSMemoryUsage_helper {
    double memory_used_count;
    double str_count;
    double str_size;
    int64_t js_func_count;
    double js_func_size;
    int64_t js_func_code_size;
    int64_t js_func_pc2line_count;
    int64_t js_func_pc2line_size;
} JSMemoryUsage_helper;

#define JS_PROTO_WALK_LIMIT 1048576
#define JS_FAST_ARRAY_FILL_POLL 65536
#define DEFINE_GLOBAL_LEX_VAR (1 << 7)
#define DEFINE_GLOBAL_FUNC_VAR (1 << 6)

static inline BOOL JS_IsHTMLDDA(JSContext* ctx, JSValueConst obj)
{
    JSObject* p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(obj);
    return p->is_HTMLDDA;
}

#define JS_BIGINT_MAX_SIZE ((1024 * 1024) / JS_LIMB_BITS)

#define ADDC(res, carry_out, op1, op2, carry_in) \
    do {                                         \
        js_limb_t __v, __a, __k, __k1;           \
        __v = (op1);                             \
        __a = __v + (op2);                       \
        __k1 = __a < __v;                        \
        __k = (carry_in);                        \
        __a = __a + __k;                         \
        carry_out = (__a < __k) | __k1;          \
        res = __a;                               \
    } while (0)

static inline js_limb_t js_limb_safe_clz(js_limb_t a)
{
    if (a == 0)
        return JS_LIMB_BITS;
    else
        return js_limb_clz(a);
}

#define MP_MUL_KARA_THRESHOLD 32
#define MP_MUL_KARA_MAX_DEPTH 64

static inline js_limb_t udiv1norm_init(js_limb_t d)
{
    js_limb_t a0, a1;
    a1 = -d - 1;
    a0 = -1;
    return (((js_dlimb_t)a1 << JS_LIMB_BITS) | a0) / d;
}

static inline js_limb_t udiv1norm(js_limb_t* pr, js_limb_t a1, js_limb_t a0,
    js_limb_t d, js_limb_t d_inv)
{
    js_limb_t n1m, n_adj, q, r, ah;
    js_dlimb_t a;
    n1m = ((js_slimb_t)a0 >> (JS_LIMB_BITS - 1));
    n_adj = a0 + (n1m & d);
    a = (js_dlimb_t)d_inv * (a1 - n1m) + n_adj;
    q = (a >> JS_LIMB_BITS) + a1;
    a = ((js_dlimb_t)a1 << JS_LIMB_BITS) | a0;
    a = a - (js_dlimb_t)q * d - d;
    ah = a >> JS_LIMB_BITS;
    q += 1 + ah;
    r = (js_limb_t)a + (ah & d);
    *pr = r;
    return q;
}

#define UDIV1NORM_THRESHOLD 3
#define JS_RADIX_MAX 36
#define JS_BIGINT_TOSTR_DC_THRESHOLD 64
#define JS_BIGINT_TOSTR_MAX_DEPTH 64
#define JS_BIGINT_TOSTR_POW_MAX 16

typedef struct JSToStrPow {
    js_limb_t* tab;
    int len;
} JSToStrPow;

#define JS_PRINT_MAX_DEPTH 8

typedef struct {
    JSRuntime* rt;
    JSContext* ctx;
    JSPrintValueOptions options;
    JSPrintValueWrite* write_func;
    void* write_opaque;
    int level;
    JSObject* print_stack[JS_PRINT_MAX_DEPTH];
} JSPrintValueState;

static inline BOOL tag_is_string(uint32_t tag)
{
    return tag == JS_TAG_STRING || tag == JS_TAG_STRING_ROPE;
}

#define JS_CALL_FLAG_COPY_ARGV (1 << 1)
#define JS_CALL_FLAG_GENERATOR (1 << 2)

static inline int js_num_arg(JSValueConst v, double* d)
{
    uint32_t tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) {
        *d = JS_VALUE_GET_INT(v);
        return 1;
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        *d = JS_VALUE_GET_FLOAT64(v);
        return 1;
    }
    return 0;
}

static inline int js_call_numeric_cfunc(JSContext* ctx, JSValueConst func,
    int argc, JSValueConst* argv, JSValue* out)
{
    JSObject* p;
    double a, b;
    if (JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT || argc < 1)
        return 0;
    p = JS_VALUE_GET_OBJ(func);
    if (p->class_id != JS_CLASS_C_FUNCTION || !js_num_arg(argv[0], &a))
        return 0;
    if (p->u.cfunc.cproto == JS_CFUNC_f_f) {
        *out = JS_NewFloat64(ctx, p->u.cfunc.c_function.f_f(a));
        return 1;
    }
    if (p->u.cfunc.cproto == JS_CFUNC_f_f_f && argc >= 2 && js_num_arg(argv[1], &b)) {
        *out = JS_NewFloat64(ctx, p->u.cfunc.c_function.f_f_f(a, b));
        return 1;
    }
    return 0;
}

#define FUNC_RET_AWAIT 0
#define FUNC_RET_YIELD 1
#define FUNC_RET_YIELD_STAR 2
#define FUNC_RET_INITIAL_YIELD 3

typedef enum JSGeneratorStateEnum {
    JS_GENERATOR_STATE_SUSPENDED_START,
    JS_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_GENERATOR_STATE_EXECUTING,
    JS_GENERATOR_STATE_COMPLETED,
} JSGeneratorStateEnum;

typedef struct JSGeneratorData {
    JSGeneratorStateEnum state;
    JSAsyncFunctionState* func_state;
} JSGeneratorData;

typedef struct JSAsyncGeneratorRequest {
    struct list_head link;
    int completion_type;
    JSValue result;
    JSValue promise;
    JSValue resolving_funcs[2];
} JSAsyncGeneratorRequest;

typedef struct JSDisposableResource {
    JSValue value;
    JSValue method;
    BOOL async;
} JSDisposableResource;

typedef struct JSDisposableStackData {
    BOOL disposed;
    int count;
    int size;
    JSDisposableResource* resources;
    JSValue pending;
    JSValue await_value;
    int cursor;
    uint8_t needs_await;
    uint8_t has_awaited;
} JSDisposableStackData;

enum {
    ASYNC_DISPOSE_RESOLVE = 0,
    ASYNC_DISPOSE_REJECT,
    ASYNC_DISPOSE_STACK,
    ASYNC_DISPOSE_INDEX,
    ASYNC_DISPOSE_ERROR,
    ASYNC_DISPOSE_DATA_COUNT,
};

static int JS_InitAtoms(JSRuntime* rt);

static JSAtom __JS_NewAtomInit(JSRuntime* rt, const char* str, int len,
    int atom_type);

static void JS_FreeAtomStruct(JSRuntime* rt, JSAtomStruct* p);

static void __JS_FreeAtom(JSRuntime* rt, uint32_t i);

static JSAtom js_get_atom_index(JSRuntime* rt, JSAtomStruct* p);

static JSValue js_call_c_function(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags);

static JSValue js_call_bound_function(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags);

static JSValue JS_CallInternal(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj, JSValueConst new_target,
    int argc, JSValue* argv, int flags);
static force_inline JSValue js_call_dispatch(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj, JSValueConst new_target,
    int argc, JSValue* argv, int flags);

static JSValue JS_CallConstructorInternal(JSContext* ctx,
    JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValue* argv, int flags);

static __maybe_unused void JS_DumpAtoms(JSRuntime* rt);

static __maybe_unused void JS_DumpString(JSRuntime* rt, const JSString* p);

static __maybe_unused void JS_DumpObjectHeader(JSRuntime* rt);

static __maybe_unused void JS_DumpObject(JSRuntime* rt, JSObject* p);

static __maybe_unused void JS_DumpGCObject(JSRuntime* rt, JSGCObjectHeader* p);

static __maybe_unused void JS_DumpAtom(JSContext* ctx, const char* str, JSAtom atom);

static __maybe_unused void JS_DumpValueRT(JSRuntime* rt, const char* str, JSValueConst val);

static __maybe_unused void JS_DumpValue(JSContext* ctx, const char* str, JSValueConst val);

static __maybe_unused void JS_DumpShapes(JSRuntime* rt);

static void js_dump_value_write(void* opaque, const char* buf, size_t len);

static void js_array_finalizer(JSRuntime* rt, JSValue val);

static void js_array_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

static void js_mapped_arguments_finalizer(JSRuntime* rt, JSValue val);

static void js_mapped_arguments_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

static void js_object_data_finalizer(JSRuntime* rt, JSValue val);

static void js_object_data_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

static void js_c_function_finalizer(JSRuntime* rt, JSValue val);

static void js_c_function_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

static void js_bound_function_finalizer(JSRuntime* rt, JSValue val);

static void js_bound_function_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static void js_for_in_iterator_finalizer(JSRuntime* rt, JSValue val);

static void js_for_in_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static void js_generator_finalizer(JSRuntime* rt, JSValue obj);

static void js_generator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static void js_global_object_finalizer(JSRuntime* rt, JSValue obj);

static void js_global_object_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static JSValue JS_ToPrimitiveFree(JSContext* ctx, JSValue val, int hint);

static int JS_ToInt32Free(JSContext* ctx, int32_t* pres, JSValue val);

static int JS_ToUint8ClampFree(JSContext* ctx, int32_t* pres, JSValue val);

static void gc_decref(JSRuntime* rt);

static int JS_NewClass1(JSRuntime* rt, JSClassID class_id,
    const JSClassDef* class_def, JSAtom name);

static BOOL js_strict_eq(JSContext* ctx, JSValueConst op1, JSValueConst op2);

static int JS_ToBigInt64Free(JSContext* ctx, int64_t* pres, JSValue val);

static int JS_CreateProperty(JSContext* ctx, JSObject* p,
    JSAtom prop, JSValueConst val,
    JSValueConst getter, JSValueConst setter,
    int flags);

static int js_string_memcmp(const JSString* p1, int pos1, const JSString* p2,
    int pos2, int len);

static JSVarRef* get_var_ref(JSContext* ctx, JSStackFrame* sf, int var_idx,
    BOOL is_arg);

static void __async_func_free(JSRuntime* rt, JSAsyncFunctionState* s);

static JSValue js_generator_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags);

static BOOL js_string_eq(JSContext* ctx,
    const JSString* p1, const JSString* p2);

static BOOL JS_NumberIsNegativeOrMinusZero(JSContext* ctx, JSValueConst val);

static void js_free_shape_null(JSRuntime* rt, JSShape* sh);

static int init_shape_hash(JSRuntime* rt);

static void js_c_function_data_finalizer(JSRuntime* rt, JSValue val);

static void js_c_function_data_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static JSValue js_c_function_data_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_val,
    int argc, JSValueConst* argv, int flags);

static JSAtom js_symbol_to_atom(JSContext* ctx, JSValue val);

static JSValue js_instantiate_prototype(JSContext* ctx, JSObject* p, JSAtom atom, void* opaque);

static void JS_RunGCInternal(JSRuntime* rt, BOOL remove_weak_objects);
static BOOL gc_exit_finalize(JSRuntime* rt);

static JSVarRef* js_global_object_find_uninitialized_var(JSContext* ctx, JSObject* p,
    JSAtom atom, BOOL is_lexical);

static BOOL js_ctor_presize_proto_safe(JSObject* proto, JSCtorPresize* cp);

static void js_date_cache_free(JSRuntime* rt, JSDateFieldsCache* fc)
{
    fc->next_free = rt->date_cache_free_list;
    rt->date_cache_free_list = fc;
}

static const char js_atom_init[] =
#define DEF(name, str) str "\0"
#include "dyna-atom.h"
#undef DEF
    ;

static const JSClassExoticMethods js_arguments_exotic_methods;

static JSClassID js_class_id_alloc = JS_CLASS_INIT_COUNT;

int js_no_prototype_extensions;

void JS_SetNoPrototypeExtensions(int disable)
{
    js_no_prototype_extensions = !!disable;
}

int JS_NoPrototypeExtensions(void)
{
    return js_no_prototype_extensions;
}

#include "src/mm/js_malloc.inc.c"
#include "src/value/atoms.inc.c"
#include "src/runtime/class.inc.c"
#include "src/object/shapes_objects_gc.inc.c"
#include "src/object/property_get.inc.c"
#include "src/object/property_set_convert.inc.c"
#include "src/vm/tailcall.inc.c"
#include "src/vm/interpreter.inc.c"

static int js_async_function_settle(JSContext* ctx, JSValueConst func_obj,
    JSValueConst arg, BOOL is_reject);

#include "src/builtins/disposable.inc.c"
#include "src/runtime/using.inc.c"
