#ifndef DYNAJS_H
#define DYNAJS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define js_likely(x) __builtin_expect(!!(x), 1)
#define js_unlikely(x) __builtin_expect(!!(x), 0)
#define js_force_inline inline __attribute__((always_inline))
#define __js_printf_like(f, a) __attribute__((format(printf, f, a)))
#else
#define js_likely(x) (x)
#define js_unlikely(x) (x)
#define js_force_inline inline
#define __js_printf_like(a, b)
#endif

#define JS_BOOL int

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct JSClass JSClass;
typedef uint32_t JSClassID;
typedef uint32_t JSAtom;

#if INTPTR_MAX >= INT64_MAX
#define JS_PTR64
#define JS_PTR64_DEF(a) a
#else
#define JS_PTR64_DEF(a)
#endif

#ifndef JS_PTR64
#define JS_NAN_BOXING
#endif

#if defined(__SIZEOF_INT128__) && (INTPTR_MAX >= INT64_MAX)
#define JS_LIMB_BITS 64
#else
#define JS_LIMB_BITS 32
#endif

#define JS_SHORT_BIG_INT_BITS JS_LIMB_BITS

enum {
    JS_TAG_FIRST = -9,
    JS_TAG_BIG_INT = -9,
    JS_TAG_SYMBOL = -8,
    JS_TAG_STRING = -7,
    JS_TAG_STRING_ROPE = -6,
    JS_TAG_MODULE = -3,
    JS_TAG_FUNCTION_BYTECODE = -2,
    JS_TAG_OBJECT = -1,

    JS_TAG_INT = 0,
    JS_TAG_BOOL = 1,
    JS_TAG_NULL = 2,
    JS_TAG_UNDEFINED = 3,
    JS_TAG_UNINITIALIZED = 4,
    JS_TAG_CATCH_OFFSET = 5,
    JS_TAG_EXCEPTION = 6,
    JS_TAG_SHORT_BIG_INT = 7,
    JS_TAG_FLOAT64 = 8,
};

typedef struct JSRefCountHeader {
    int ref_count;
} JSRefCountHeader;

#define JS_FLOAT64_NAN ((double)NAN)

#ifdef CONFIG_CHECK_JSVALUE
typedef struct __JSValue* JSValue;
typedef const struct __JSValue* JSValueConst;

#define JS_VALUE_GET_TAG(v) (int)((uintptr_t)(v) & 0xf)
#define JS_VALUE_GET_NORM_TAG(v) JS_VALUE_GET_TAG(v)
#define JS_VALUE_GET_INT(v) (int)((intptr_t)(v) >> 4)
#define JS_VALUE_GET_BOOL(v) JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_FLOAT64(v) (double)JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_PTR(v) (void*)((intptr_t)(v) & ~0xf)

#define JS_MKVAL(tag, val) (JSValue)(intptr_t)(((val) << 4) | (tag))
#define JS_MKPTR(tag, p) (JSValue)((intptr_t)(p) | (tag))

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN JS_MKVAL(JS_TAG_FLOAT64, 1)

static inline JSValue __JS_NewFloat64(JSContext* ctx, double d)
{
    return JS_MKVAL(JS_TAG_FLOAT64, (int)d);
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    return 0;
}

static inline JSValue __JS_NewShortBigInt(JSContext* ctx, int32_t d)
{
    return JS_MKVAL(JS_TAG_SHORT_BIG_INT, d);
}

#elif defined(JS_NAN_BOXING)

typedef uint64_t JSValue;

#define JSValueConst JSValue

#define JS_VALUE_GET_TAG(v) (int)((v) >> 32)
#define JS_VALUE_GET_INT(v) (int)(v)
#define JS_VALUE_GET_BOOL(v) (int)(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) (int)(v)
#define JS_VALUE_GET_PTR(v) (void*)(intptr_t)(v)

#define JS_MKVAL(tag, val) (((uint64_t)(tag) << 32) | (uint32_t)(val))
#define JS_MKPTR(tag, ptr) (((uint64_t)(tag) << 32) | (uintptr_t)(ptr))

#define JS_FLOAT64_TAG_ADDEND (0x7ff80000 - JS_TAG_FIRST + 1)

static inline double JS_VALUE_GET_FLOAT64(JSValue v)
{
    union {
        JSValue v;
        double d;
    } u;
    u.v = v;
    u.v += (uint64_t)JS_FLOAT64_TAG_ADDEND << 32;
    return u.d;
}

#define JS_NAN (0x7ff8000000000000 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32))

static inline JSValue __JS_NewFloat64(JSContext* ctx, double d)
{
    union {
        double d;
        uint64_t u64;
    } u;
    JSValue v;
    u.d = d;
    if (js_unlikely((u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000))
        v = JS_NAN;
    else
        v = u.u64 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32);
    return v;
}

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)((tag) - JS_TAG_FIRST) >= (JS_TAG_FLOAT64 - JS_TAG_FIRST))

static inline int JS_VALUE_GET_NORM_TAG(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    if (JS_TAG_IS_FLOAT64(tag))
        return JS_TAG_FLOAT64;
    else
        return tag;
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    return tag == (JS_NAN >> 32);
}

static inline JSValue __JS_NewShortBigInt(JSContext* ctx, int32_t d)
{
    return JS_MKVAL(JS_TAG_SHORT_BIG_INT, d);
}

#else

typedef union JSValueUnion {
    uint64_t uint64;
    double float64;
    void* ptr;
#if JS_SHORT_BIG_INT_BITS == 32
    int32_t short_big_int;
#else
    int64_t short_big_int;
#endif
} JSValueUnion;

typedef struct JSValue {
    JSValueUnion u;
    int64_t tag;
} JSValue;

#define JSValueConst JSValue

#define JS_VALUE_GET_TAG(v) ((int32_t)(v).tag)
#define JS_VALUE_GET_NORM_TAG(v) JS_VALUE_GET_TAG(v)
#define JS_VALUE_GET_INT(v) ((int)(v).u.uint64)
#define JS_VALUE_GET_BOOL(v) ((int)(v).u.uint64)
#define JS_VALUE_GET_FLOAT64(v) ((v).u.float64)
#define JS_VALUE_GET_SHORT_BIG_INT(v) ((v).u.short_big_int)
#define JS_VALUE_GET_PTR(v) ((v).u.ptr)

#define JS_MKVAL(tag, val) \
    (JSValue) { (JSValueUnion) { .uint64 = (uint32_t)(val) }, tag }
#define JS_MKPTR(tag, p) \
    (JSValue) { (JSValueUnion) { .ptr = p }, tag }

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN \
    (JSValue) { .u.float64 = JS_FLOAT64_NAN, JS_TAG_FLOAT64 }

static inline JSValue __JS_NewFloat64(JSContext* ctx, double d)
{
    JSValue v;
    v.tag = JS_TAG_FLOAT64;
    v.u.float64 = d;
    return v;
}

static inline JS_BOOL JS_VALUE_IS_NAN(JSValue v)
{
    union {
        double d;
        uint64_t u64;
    } u;
    if (v.tag != JS_TAG_FLOAT64)
        return 0;
    u.d = v.u.float64;
    return (u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000;
}

static inline JSValue __JS_NewShortBigInt(JSContext* ctx, int64_t d)
{
    JSValue v;
    v.tag = JS_TAG_SHORT_BIG_INT;
    v.u.short_big_int = d;
    return v;
}

#endif

#define JS_VALUE_IS_BOTH_INT(v1, v2) ((JS_VALUE_GET_TAG(v1) | JS_VALUE_GET_TAG(v2)) == 0)
#define JS_VALUE_IS_BOTH_FLOAT(v1, v2) (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v1)) && JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v2)))

#define JS_VALUE_HAS_REF_COUNT(v) ((unsigned)JS_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST)

#define JS_NULL JS_MKVAL(JS_TAG_NULL, 0)
#define JS_UNDEFINED JS_MKVAL(JS_TAG_UNDEFINED, 0)
#define JS_FALSE JS_MKVAL(JS_TAG_BOOL, 0)
#define JS_TRUE JS_MKVAL(JS_TAG_BOOL, 1)
#define JS_EXCEPTION JS_MKVAL(JS_TAG_EXCEPTION, 0)
#define JS_UNINITIALIZED JS_MKVAL(JS_TAG_UNINITIALIZED, 0)

#define JS_PROP_CONFIGURABLE (1 << 0)
#define JS_PROP_WRITABLE (1 << 1)
#define JS_PROP_ENUMERABLE (1 << 2)
#define JS_PROP_C_W_E (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE)
#define JS_PROP_LENGTH (1 << 3)
#define JS_PROP_TMASK (3 << 4)
#define JS_PROP_NORMAL (0 << 4)
#define JS_PROP_GETSET (1 << 4)
#define JS_PROP_VARREF (2 << 4)
#define JS_PROP_AUTOINIT (3 << 4)

#define JS_PROP_HAS_SHIFT 8
#define JS_PROP_HAS_CONFIGURABLE (1 << 8)
#define JS_PROP_HAS_WRITABLE (1 << 9)
#define JS_PROP_HAS_ENUMERABLE (1 << 10)
#define JS_PROP_HAS_GET (1 << 11)
#define JS_PROP_HAS_SET (1 << 12)
#define JS_PROP_HAS_VALUE (1 << 13)

#define JS_PROP_THROW (1 << 14)
#define JS_PROP_THROW_STRICT (1 << 15)

#define JS_PROP_NO_EXOTIC (1 << 16)

#ifndef JS_DEFAULT_STACK_SIZE
#define JS_DEFAULT_STACK_SIZE (1024 * 1024)
#endif

#define JS_EVAL_TYPE_GLOBAL (0 << 0)
#define JS_EVAL_TYPE_MODULE (1 << 0)
#define JS_EVAL_TYPE_DIRECT (2 << 0)
#define JS_EVAL_TYPE_INDIRECT (3 << 0)
#define JS_EVAL_TYPE_MASK (3 << 0)

#define JS_EVAL_FLAG_STRICT (1 << 3)
#define JS_EVAL_FLAG_COMPILE_ONLY (1 << 5)
#define JS_EVAL_FLAG_BACKTRACE_BARRIER (1 << 6)
#define JS_EVAL_FLAG_ASYNC (1 << 7)

typedef JSValue JSCFunction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
typedef JSValue JSCFunctionMagic(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
typedef JSValue JSCFunctionData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* func_data);

typedef struct JSMallocState {
    size_t malloc_count;
    size_t malloc_size;
    size_t malloc_limit;
    void* opaque;
} JSMallocState;

typedef struct JSMallocFunctions {
    void* (*js_malloc)(JSMallocState* s, size_t size);
    void (*js_free)(JSMallocState* s, void* ptr);
    void* (*js_realloc)(JSMallocState* s, void* ptr, size_t size);
    size_t (*js_malloc_usable_size)(const void* ptr);
} JSMallocFunctions;

typedef struct JSGCObjectHeader JSGCObjectHeader;

JSRuntime* JS_NewRuntime(void);
void JS_SetRuntimeInfo(JSRuntime* rt, const char* info);
void JS_SetMemoryLimit(JSRuntime* rt, size_t limit);
void JS_SetGCThreshold(JSRuntime* rt, size_t gc_threshold);
void JS_SetMaxStackSize(JSRuntime* rt, size_t stack_size);
void JS_UpdateStackTop(JSRuntime* rt);
JSRuntime* JS_NewRuntime2(const JSMallocFunctions* mf, void* opaque);
void JS_FreeRuntime(JSRuntime* rt);
void* JS_GetRuntimeOpaque(JSRuntime* rt);
void JS_SetRuntimeOpaque(JSRuntime* rt, void* opaque);
typedef void JS_MarkFunc(JSRuntime* rt, JSGCObjectHeader* gp);
void JS_MarkValue(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
void JS_RunGC(JSRuntime* rt);
JS_BOOL JS_IsLiveObject(JSRuntime* rt, JSValueConst obj);

JSContext* JS_NewContext(JSRuntime* rt);
void JS_FreeContext(JSContext* s);

typedef void (*JSShutdownSweepFunc)(JSContext* ctx, JSRuntime* rt,
    void* opaque);
int JS_AddShutdownSweep(JSRuntime* rt, JSShutdownSweepFunc func, void* opaque);
void JS_RemoveShutdownSweep(JSRuntime* rt, JSShutdownSweepFunc func,
    void* opaque);
int JS_ShutdownDeferFree(JSRuntime* rt, void* ptr);
void JS_ShutdownUndeferFree(JSRuntime* rt, void* ptr);

JSContext* JS_DupContext(JSContext* ctx);
void* JS_GetContextOpaque(JSContext* ctx);
void JS_SetContextOpaque(JSContext* ctx, void* opaque);
JSRuntime* JS_GetRuntime(JSContext* ctx);
void JS_SetClassProto(JSContext* ctx, JSClassID class_id, JSValue obj);
JSValue JS_GetClassProto(JSContext* ctx, JSClassID class_id);

JSContext* JS_NewContextRaw(JSRuntime* rt);
void JS_SetNoPrototypeExtensions(int disable);
int JS_NoPrototypeExtensions(void);
int JS_AddIntrinsicBaseObjects(JSContext* ctx);
int JS_AddIntrinsicDate(JSContext* ctx);
int JS_AddIntrinsicLens(JSContext* ctx);
int JS_AddIntrinsicEval(JSContext* ctx);
int JS_AddIntrinsicStringNormalize(JSContext* ctx);
void JS_AddIntrinsicRegExpCompiler(JSContext* ctx);
int JS_AddIntrinsicRegExp(JSContext* ctx);
int JS_AddIntrinsicJSON(JSContext* ctx);
int JS_AddIntrinsicProxy(JSContext* ctx);
int JS_AddIntrinsicMapSet(JSContext* ctx);
int JS_AddIntrinsicTypedArrays(JSContext* ctx);
int JS_AddIntrinsicPromise(JSContext* ctx);
int JS_AddIntrinsicWeakRef(JSContext* ctx);
int JS_AddIntrinsicDisposableStack(JSContext* ctx);

JSValue js_string_codePointRange(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void* js_malloc_rt(JSRuntime* rt, size_t size);
void js_free_rt(JSRuntime* rt, void* ptr);
void* js_realloc_rt(JSRuntime* rt, void* ptr, size_t size);
size_t js_malloc_usable_size_rt(JSRuntime* rt, const void* ptr);
void* js_mallocz_rt(JSRuntime* rt, size_t size);

void* js_malloc(JSContext* ctx, size_t size);
void js_free(JSContext* ctx, void* ptr);
void* js_realloc(JSContext* ctx, void* ptr, size_t size);
size_t js_malloc_usable_size(JSContext* ctx, const void* ptr);
void* js_realloc2(JSContext* ctx, void* ptr, size_t size, size_t* pslack);
void* js_mallocz(JSContext* ctx, size_t size);
char* js_strdup(JSContext* ctx, const char* str);
char* js_strndup(JSContext* ctx, const char* s, size_t n);

typedef struct JSMemoryUsage {
    int64_t malloc_size, malloc_limit, memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t js_func_pc2line_count, js_func_pc2line_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} JSMemoryUsage;

void JS_ComputeMemoryUsage(JSRuntime* rt, JSMemoryUsage* s);
void JS_DumpMemoryUsage(FILE* fp, const JSMemoryUsage* s, JSRuntime* rt);

#define JS_ATOM_NULL 0

JSAtom JS_NewAtomLen(JSContext* ctx, const char* str, size_t len);
JSAtom JS_NewAtom(JSContext* ctx, const char* str);
JSAtom JS_NewAtomUInt32(JSContext* ctx, uint32_t n);
JSAtom JS_DupAtom(JSContext* ctx, JSAtom v);
void JS_FreeAtom(JSContext* ctx, JSAtom v);
void JS_FreeAtomRT(JSRuntime* rt, JSAtom v);
JSValue JS_AtomToValue(JSContext* ctx, JSAtom atom);
JSValue JS_AtomToString(JSContext* ctx, JSAtom atom);
const char* JS_AtomToCStringLen(JSContext* ctx, size_t* plen, JSAtom atom);
const char* JS_AtomBorrowASCII(JSContext* ctx, size_t* plen, JSAtom atom);
static inline const char* JS_AtomToCString(JSContext* ctx, JSAtom atom)
{
    return JS_AtomToCStringLen(ctx, NULL, atom);
}
JSAtom JS_ValueToAtom(JSContext* ctx, JSValueConst val);

typedef struct JSPropertyEnum {
    JS_BOOL is_enumerable;
    JSAtom atom;
} JSPropertyEnum;

typedef struct JSPropertyDescriptor {
    int flags;
    JSValue value;
    JSValue getter;
    JSValue setter;
} JSPropertyDescriptor;

typedef struct JSClassExoticMethods {
    int (*get_own_property)(JSContext* ctx, JSPropertyDescriptor* desc,
        JSValueConst obj, JSAtom prop);
    int (*get_own_property_names)(JSContext* ctx, JSPropertyEnum** ptab,
        uint32_t* plen,
        JSValueConst obj);
    int (*delete_property)(JSContext* ctx, JSValueConst obj, JSAtom prop);
    int (*define_own_property)(JSContext* ctx, JSValueConst this_obj,
        JSAtom prop, JSValueConst val,
        JSValueConst getter, JSValueConst setter,
        int flags);
    int (*has_property)(JSContext* ctx, JSValueConst obj, JSAtom atom);
    JSValue (*get_property)(JSContext* ctx, JSValueConst obj, JSAtom atom,
        JSValueConst receiver);
    int (*set_property)(JSContext* ctx, JSValueConst obj, JSAtom atom,
        JSValueConst value, JSValueConst receiver, int flags);

    JSValue (*get_prototype)(JSContext* ctx, JSValueConst obj);
    int (*set_prototype)(JSContext* ctx, JSValueConst obj, JSValueConst proto_val);
    int (*is_extensible)(JSContext* ctx, JSValueConst obj);
    int (*prevent_extensions)(JSContext* ctx, JSValueConst obj);
} JSClassExoticMethods;

typedef void JSClassFinalizer(JSRuntime* rt, JSValue val);
typedef void JSClassGCMark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
#define JS_CALL_FLAG_CONSTRUCTOR (1 << 0)
typedef JSValue JSClassCall(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_val, int argc, JSValueConst* argv,
    int flags);

typedef struct JSClassDef {
    const char* class_name;
    JSClassFinalizer* finalizer;
    JSClassGCMark* gc_mark;
    JSClassCall* call;
    JSClassExoticMethods* exotic;
} JSClassDef;

#define JS_INVALID_CLASS_ID 0
JSClassID JS_NewClassID(JSClassID* pclass_id);
JSClassID JS_GetClassID(JSValue v);
int JS_NewClass(JSRuntime* rt, JSClassID class_id, const JSClassDef* class_def);
int JS_IsRegisteredClass(JSRuntime* rt, JSClassID class_id);

static js_force_inline JSValue JS_NewBool(JSContext* ctx, JS_BOOL val)
{
    return JS_MKVAL(JS_TAG_BOOL, (val != 0));
}

static js_force_inline JSValue JS_NewInt32(JSContext* ctx, int32_t val)
{
    return JS_MKVAL(JS_TAG_INT, val);
}

static js_force_inline JSValue JS_NewCatchOffset(JSContext* ctx, int32_t val)
{
    return JS_MKVAL(JS_TAG_CATCH_OFFSET, val);
}

static js_force_inline JSValue JS_NewInt64(JSContext* ctx, int64_t val)
{
    JSValue v;
    if (val == (int32_t)val) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

static js_force_inline JSValue JS_NewUint32(JSContext* ctx, uint32_t val)
{
    JSValue v;
    if (val <= 0x7fffffff) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

JSValue JS_NewBigInt64(JSContext* ctx, int64_t v);
JSValue JS_NewBigUint64(JSContext* ctx, uint64_t v);

static js_force_inline JSValue JS_NewFloat64(JSContext* ctx, double d)
{
    int32_t val;
    union {
        double d;
        uint64_t u;
    } u, t;
    if (d >= INT32_MIN && d <= INT32_MAX) {
        u.d = d;
        val = (int32_t)d;
        t.d = val;
        if (u.u == t.u)
            return JS_MKVAL(JS_TAG_INT, val);
    }
    return __JS_NewFloat64(ctx, d);
}

static inline JS_BOOL JS_IsNumber(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag);
}

static inline JS_BOOL JS_IsBigInt(JSContext* ctx, JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline JS_BOOL JS_IsBool(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_BOOL;
}

static inline JS_BOOL JS_IsNull(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_NULL;
}

static inline JS_BOOL JS_IsUndefined(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED;
}

static inline JS_BOOL JS_IsException(JSValueConst v)
{
    return js_unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION);
}

static inline JS_BOOL JS_IsUninitialized(JSValueConst v)
{
    return js_unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED);
}

static inline JS_BOOL JS_IsString(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING || JS_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE;
}

static inline JS_BOOL JS_IsSymbol(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_SYMBOL;
}

static inline JS_BOOL JS_IsObject(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT;
}

JSValue JS_Throw(JSContext* ctx, JSValue obj);
void JS_SetUncatchableException(JSContext* ctx, JS_BOOL flag);
JSValue JS_GetException(JSContext* ctx);
JS_BOOL JS_HasException(JSContext* ctx);
JS_BOOL JS_IsError(JSContext* ctx, JSValueConst val);
JSValue JS_NewError(JSContext* ctx);
JSValue __js_printf_like(2, 3) JS_ThrowSyntaxError(JSContext* ctx, const char* fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowTypeError(JSContext* ctx, const char* fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowReferenceError(JSContext* ctx, const char* fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowRangeError(JSContext* ctx, const char* fmt, ...);
JSValue __js_printf_like(2, 3) JS_ThrowInternalError(JSContext* ctx, const char* fmt, ...);
JSValue JS_ThrowOutOfMemory(JSContext* ctx);

void __JS_FreeValue(JSContext* ctx, JSValue v);

static inline JSRefCountHeader* __js_rc(void* ptr)
{
    return (JSRefCountHeader*)((uint32_t*)ptr - 1);
}

static inline void JS_FreeValue(JSContext* ctx, JSValue v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader* p = __js_rc(JS_VALUE_GET_PTR(v));
        if (--p->ref_count <= 0) {
            __JS_FreeValue(ctx, v);
        }
    }
}
void __JS_FreeValueRT(JSRuntime* rt, JSValue v);
static inline void JS_FreeValueRT(JSRuntime* rt, JSValue v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader* p = __js_rc(JS_VALUE_GET_PTR(v));
        if (--p->ref_count <= 0) {
            __JS_FreeValueRT(rt, v);
        }
    }
}

static inline JSValue JS_DupValue(JSContext* ctx, JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader* p = __js_rc(JS_VALUE_GET_PTR(v));
        p->ref_count++;
    }
    return (JSValue)v;
}

static inline JSValue JS_DupValueRT(JSRuntime* rt, JSValueConst v)
{
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        JSRefCountHeader* p = __js_rc(JS_VALUE_GET_PTR(v));
        p->ref_count++;
    }
    return (JSValue)v;
}

JS_BOOL JS_StrictEq(JSContext* ctx, JSValueConst op1, JSValueConst op2);
JS_BOOL JS_SameValue(JSContext* ctx, JSValueConst op1, JSValueConst op2);
JS_BOOL JS_SameValueZero(JSContext* ctx, JSValueConst op1, JSValueConst op2);

int JS_ToBool(JSContext* ctx, JSValueConst val);
int JS_ToInt32(JSContext* ctx, int32_t* pres, JSValueConst val);
static inline int JS_ToUint32(JSContext* ctx, uint32_t* pres, JSValueConst val)
{
    return JS_ToInt32(ctx, (int32_t*)pres, val);
}
int JS_ToInt64(JSContext* ctx, int64_t* pres, JSValueConst val);
int JS_ToIndex(JSContext* ctx, uint64_t* plen, JSValueConst val);
int JS_ToFloat64(JSContext* ctx, double* pres, JSValueConst val);
int JS_ToBigInt64(JSContext* ctx, int64_t* pres, JSValueConst val);
int JS_ToInt64Ext(JSContext* ctx, int64_t* pres, JSValueConst val);

JSValue JS_NewStringLen(JSContext* ctx, const char* str1, size_t len1);
static inline JSValue JS_NewString(JSContext* ctx, const char* str)
{
    return JS_NewStringLen(ctx, str, strlen(str));
}
JSValue JS_NewAtomString(JSContext* ctx, const char* str);
JSValue JS_ToString(JSContext* ctx, JSValueConst val);
JSValue JS_ToPropertyKey(JSContext* ctx, JSValueConst val);
const char* JS_ToCStringLen2(JSContext* ctx, size_t* plen, JSValueConst val1, JS_BOOL cesu8);

const uint8_t* JS_GetNarrowStringBytes(JSContext* ctx, JSValueConst val,
    size_t* plen);
static inline const char* JS_ToCStringLen(JSContext* ctx, size_t* plen, JSValueConst val1)
{
    return JS_ToCStringLen2(ctx, plen, val1, 0);
}
static inline const char* JS_ToCString(JSContext* ctx, JSValueConst val1)
{
    return JS_ToCStringLen2(ctx, NULL, val1, 0);
}
void JS_FreeCString(JSContext* ctx, const char* ptr);

JSValue JS_NewObjectProtoClass(JSContext* ctx, JSValueConst proto, JSClassID class_id);
JSValue JS_NewObjectClass(JSContext* ctx, int class_id);
JSValue JS_NewObjectProto(JSContext* ctx, JSValueConst proto);
JSValue JS_NewObject(JSContext* ctx);

typedef struct JSObjectTemplate JSObjectTemplate;
JSObjectTemplate* JS_NewObjectTemplate(JSContext* ctx, const JSAtom* fields,
    uint32_t field_count);
void JS_FreeObjectTemplate(JSContext* ctx, JSObjectTemplate* tpl);
void JS_FreeObjectTemplateRT(JSRuntime* rt, JSObjectTemplate* tpl);
JSValue JS_NewObjectFromTemplate(JSContext* ctx, JSObjectTemplate* tpl);
void JS_SetTemplateCell(JSContext* ctx, JSValueConst row, uint32_t i,
    JSValue v);

JS_BOOL JS_IsFunction(JSContext* ctx, JSValueConst val);
JS_BOOL JS_IsConstructor(JSContext* ctx, JSValueConst val);
JS_BOOL JS_SetConstructorBit(JSContext* ctx, JSValueConst func_obj, JS_BOOL val);

JSValue JS_NewArray(JSContext* ctx);
int JS_IsArray(JSContext* ctx, JSValueConst val);

JSValue JS_NewDate(JSContext* ctx, double epoch_ms);

JSValue JS_GetPropertyInternal(JSContext* ctx, JSValueConst obj,
    JSAtom prop, JSValueConst receiver,
    JS_BOOL throw_ref_error);
static js_force_inline JSValue JS_GetProperty(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop)
{
    return JS_GetPropertyInternal(ctx, this_obj, prop, this_obj, 0);
}
JSValue JS_GetPropertyStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop);
JSValue JS_GetPropertyUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx);

int JS_SetPropertyInternal(JSContext* ctx, JSValueConst obj,
    JSAtom prop, JSValue val, JSValueConst this_obj,
    int flags);
static inline int JS_SetProperty(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValue val)
{
    return JS_SetPropertyInternal(ctx, this_obj, prop, val, this_obj, JS_PROP_THROW);
}
int JS_SetPropertyUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx, JSValue val);
int JS_SetPropertyInt64(JSContext* ctx, JSValueConst this_obj,
    int64_t idx, JSValue val);
int JS_SetPropertyStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop, JSValue val);
int JS_HasProperty(JSContext* ctx, JSValueConst this_obj, JSAtom prop);
int JS_IsExtensible(JSContext* ctx, JSValueConst obj);
int JS_PreventExtensions(JSContext* ctx, JSValueConst obj);
int JS_DeleteProperty(JSContext* ctx, JSValueConst obj, JSAtom prop, int flags);
int JS_SetPrototype(JSContext* ctx, JSValueConst obj, JSValueConst proto_val);
JSValue JS_GetPrototype(JSContext* ctx, JSValueConst val);

#define JS_GPN_STRING_MASK (1 << 0)
#define JS_GPN_SYMBOL_MASK (1 << 1)
#define JS_GPN_PRIVATE_MASK (1 << 2)
#define JS_GPN_ENUM_ONLY (1 << 4)
#define JS_GPN_SET_ENUM (1 << 5)

int JS_GetOwnFastProps(JSContext* ctx, JSValueConst obj, JSPropertyEnum* buf,
    uint32_t buf_size, uint32_t* plen);

int JS_GetOwnPropertyNames(JSContext* ctx, JSPropertyEnum** ptab,
    uint32_t* plen, JSValueConst obj, int flags);
void JS_FreePropertyEnum(JSContext* ctx, JSPropertyEnum* tab,
    uint32_t len);
JS_BOOL JS_AtomIsArrayIndex(JSContext* ctx, uint32_t* pval, JSAtom atom);

int JS_GetOwnProperty(JSContext* ctx, JSPropertyDescriptor* desc,
    JSValueConst obj, JSAtom prop);

JSValue JS_Call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv);
JSValue JS_Invoke(JSContext* ctx, JSValueConst this_val, JSAtom atom,
    int argc, JSValueConst* argv);
JSValue JS_CallConstructor(JSContext* ctx, JSValueConst func_obj,
    int argc, JSValueConst* argv);
JSValue JS_CallConstructor2(JSContext* ctx, JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValueConst* argv);
JS_BOOL JS_DetectModule(const char* input, size_t input_len);
JSValue JS_Eval(JSContext* ctx, const char* input, size_t input_len,
    const char* filename, int eval_flags);
JSValue JS_EvalThis(JSContext* ctx, JSValueConst this_obj,
    const char* input, size_t input_len,
    const char* filename, int eval_flags);
JSValue JS_GetGlobalObject(JSContext* ctx);
int JS_IsInstanceOf(JSContext* ctx, JSValueConst val, JSValueConst obj);
int JS_DefineProperty(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValueConst val,
    JSValueConst getter, JSValueConst setter, int flags);
int JS_DefinePropertyValue(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValue val, int flags);
int JS_DefinePropertyValueUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx, JSValue val, int flags);
int JS_DefinePropertyValueStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop, JSValue val, int flags);
int JS_DefinePropertyGetSet(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValue getter, JSValue setter,
    int flags);
void JS_SetOpaque(JSValue obj, void* opaque);
void* JS_GetOpaque(JSValueConst obj, JSClassID class_id);
void* JS_GetOpaque2(JSContext* ctx, JSValueConst obj, JSClassID class_id);
void* JS_GetAnyOpaque(JSValueConst obj, JSClassID* class_id);

JSValue JS_ParseJSON(JSContext* ctx, const char* buf, size_t buf_len,
    const char* filename);
#define JS_PARSE_JSON_EXT (1 << 0)
JSValue JS_ParseJSON2(JSContext* ctx, const char* buf, size_t buf_len,
    const char* filename, int flags);
JSValue JS_JSONStringify(JSContext* ctx, JSValueConst obj,
    JSValueConst replacer, JSValueConst space0);

typedef void JSFreeArrayBufferDataFunc(JSRuntime* rt, void* opaque, void* ptr);
JSValue JS_NewArrayBuffer(JSContext* ctx, uint8_t* buf, size_t len,
    JSFreeArrayBufferDataFunc* free_func, void* opaque,
    JS_BOOL is_shared);
JSValue JS_NewArrayBufferCopy(JSContext* ctx, const uint8_t* buf, size_t len);
JSValue JS_NewArrayBufferTake(JSContext* ctx, void* data, size_t len);
void JS_DetachArrayBuffer(JSContext* ctx, JSValueConst obj);
uint8_t* JS_GetArrayBuffer(JSContext* ctx, size_t* psize, JSValueConst obj);

typedef enum JSTypedArrayEnum {
    JS_TYPED_ARRAY_UINT8C = 0,
    JS_TYPED_ARRAY_INT8,
    JS_TYPED_ARRAY_UINT8,
    JS_TYPED_ARRAY_INT16,
    JS_TYPED_ARRAY_UINT16,
    JS_TYPED_ARRAY_INT32,
    JS_TYPED_ARRAY_UINT32,
    JS_TYPED_ARRAY_BIG_INT64,
    JS_TYPED_ARRAY_BIG_UINT64,
    JS_TYPED_ARRAY_FLOAT16,
    JS_TYPED_ARRAY_FLOAT32,
    JS_TYPED_ARRAY_FLOAT64,
} JSTypedArrayEnum;

JSValue JS_NewTypedArray(JSContext* ctx, int argc, JSValueConst* argv,
    JSTypedArrayEnum array_type);
JSValue JS_NewDataView(JSContext* ctx, JSValueConst buffer,
    uint64_t offset, uint64_t len);
JSValue JS_GetTypedArrayBuffer(JSContext* ctx, JSValueConst obj,
    size_t* pbyte_offset,
    size_t* pbyte_length,
    size_t* pbytes_per_element);
JSValue JS_GetArrayBufferView(JSContext* ctx, JSValueConst obj,
    size_t* pbyte_offset,
    size_t* pbyte_length,
    size_t* pbytes_per_element);
typedef struct {
    void* (*sab_alloc)(void* opaque, size_t size);
    void (*sab_free)(void* opaque, void* ptr);
    void (*sab_dup)(void* opaque, void* ptr);
    int (*sab_valid_ptr)(void* opaque, void* ptr);
    void* sab_opaque;
} JSSharedArrayBufferFunctions;
void JS_SetSharedArrayBufferFunctions(JSRuntime* rt,
    const JSSharedArrayBufferFunctions* sf);

typedef enum JSPromiseStateEnum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

JSValue JS_NewPromiseCapability(JSContext* ctx, JSValue* resolving_funcs);
JSPromiseStateEnum JS_PromiseState(JSContext* ctx, JSValue promise);
JSValue JS_PromiseResult(JSContext* ctx, JSValue promise);

typedef void JSHostPromiseRejectionTracker(JSContext* ctx, JSValueConst promise,
    JSValueConst reason,
    JS_BOOL is_handled, void* opaque);
void JS_SetHostPromiseRejectionTracker(JSRuntime* rt, JSHostPromiseRejectionTracker* cb, void* opaque);

typedef int JSInterruptHandler(JSRuntime* rt, void* opaque);
void JS_SetInterruptHandler(JSRuntime* rt, JSInterruptHandler* cb, void* opaque);
void JS_SetCanBlock(JSRuntime* rt, JS_BOOL can_block);
#define JS_STRIP_SOURCE (1 << 0)
#define JS_STRIP_DEBUG (1 << 1)
void JS_SetStripInfo(JSRuntime* rt, int flags);
int JS_GetStripInfo(JSRuntime* rt);

void JS_SetIsHTMLDDA(JSContext* ctx, JSValueConst obj);

typedef struct JSModuleDef JSModuleDef;

typedef char* JSModuleNormalizeFunc(JSContext* ctx,
    const char* module_base_name,
    const char* module_name, void* opaque);
typedef JSModuleDef* JSModuleLoaderFunc(JSContext* ctx,
    const char* module_name, void* opaque);
typedef JSModuleDef* JSModuleLoaderFunc2(JSContext* ctx,
    const char* module_name, void* opaque,
    JSValueConst attributes);
typedef int JSModuleCheckSupportedImportAttributes(JSContext* ctx, void* opaque,
    JSValueConst attributes);

void JS_SetModuleLoaderFunc(JSRuntime* rt,
    JSModuleNormalizeFunc* module_normalize,
    JSModuleLoaderFunc* module_loader, void* opaque);
void JS_SetModuleLoaderFunc2(JSRuntime* rt,
    JSModuleNormalizeFunc* module_normalize,
    JSModuleLoaderFunc2* module_loader,
    JSModuleCheckSupportedImportAttributes* module_check_attrs,
    void* opaque);
JSValue JS_GetImportMeta(JSContext* ctx, JSModuleDef* m);
JSAtom JS_GetModuleName(JSContext* ctx, JSModuleDef* m);
JSValue JS_GetModuleNamespace(JSContext* ctx, JSModuleDef* m);

typedef JSValue JSJobFunc(JSContext* ctx, int argc, JSValueConst* argv);
int JS_EnqueueJob(JSContext* ctx, JSJobFunc* job_func, int argc, JSValueConst* argv);

JS_BOOL JS_IsJobPending(JSRuntime* rt);
int JS_ExecutePendingJob(JSRuntime* rt, JSContext** pctx);

#define JS_WRITE_OBJ_BYTECODE (1 << 0)
#define JS_WRITE_OBJ_BSWAP (1 << 1)
#define JS_WRITE_OBJ_SAB (1 << 2)
#define JS_WRITE_OBJ_REFERENCE (1 << 3)
uint8_t* JS_WriteObject(JSContext* ctx, size_t* psize, JSValueConst obj,
    int flags);
uint8_t* JS_WriteObject2(JSContext* ctx, size_t* psize, JSValueConst obj,
    int flags, uint8_t*** psab_tab, size_t* psab_tab_len);

#define JS_READ_OBJ_BYTECODE (1 << 0)
#define JS_READ_OBJ_ROM_DATA (1 << 1)
#define JS_READ_OBJ_SAB (1 << 2)
#define JS_READ_OBJ_REFERENCE (1 << 3)
JSValue JS_ReadObject(JSContext* ctx, const uint8_t* buf, size_t buf_len,
    int flags);
JSValue JS_EvalFunction(JSContext* ctx, JSValue fun_obj);
int JS_ResolveModule(JSContext* ctx, JSValueConst obj);

JSAtom JS_GetScriptOrModuleName(JSContext* ctx, int n_stack_levels);
JSValue JS_LoadModule(JSContext* ctx, const char* basename,
    const char* filename);

typedef enum JSCFunctionEnum {
    JS_CFUNC_generic,
    JS_CFUNC_generic_magic,
    JS_CFUNC_constructor,
    JS_CFUNC_constructor_magic,
    JS_CFUNC_constructor_or_func,
    JS_CFUNC_constructor_or_func_magic,
    JS_CFUNC_f_f,
    JS_CFUNC_f_f_f,
    JS_CFUNC_getter,
    JS_CFUNC_setter,
    JS_CFUNC_getter_magic,
    JS_CFUNC_setter_magic,
    JS_CFUNC_iterator_next,
} JSCFunctionEnum;

typedef union JSCFunctionType {
    JSCFunction* generic;
    JSValue (*generic_magic)(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
    JSCFunction* constructor;
    JSValue (*constructor_magic)(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic);
    JSCFunction* constructor_or_func;
    double (*f_f)(double);
    double (*f_f_f)(double, double);
    JSValue (*getter)(JSContext* ctx, JSValueConst this_val);
    JSValue (*setter)(JSContext* ctx, JSValueConst this_val, JSValueConst val);
    JSValue (*getter_magic)(JSContext* ctx, JSValueConst this_val, int magic);
    JSValue (*setter_magic)(JSContext* ctx, JSValueConst this_val, JSValueConst val, int magic);
    JSValue (*iterator_next)(JSContext* ctx, JSValueConst this_val,
        int argc, JSValueConst* argv, int* pdone, int magic);
} JSCFunctionType;

JSValue JS_NewCFunction2(JSContext* ctx, JSCFunction* func,
    const char* name,
    int length, JSCFunctionEnum cproto, int magic);
JSValue JS_NewCFunctionData(JSContext* ctx, JSCFunctionData* func,
    int length, int magic, int data_len,
    JSValueConst* data);

static inline JSValue JS_NewCFunction(JSContext* ctx, JSCFunction* func, const char* name,
    int length)
{
    return JS_NewCFunction2(ctx, func, name, length, JS_CFUNC_generic, 0);
}

static inline JSValue JS_NewCFunctionMagic(JSContext* ctx, JSCFunctionMagic* func,
    const char* name,
    int length, JSCFunctionEnum cproto, int magic)
{
    JSCFunctionType ft = { .generic_magic = func };
    return JS_NewCFunction2(ctx, ft.generic, name, length, cproto, magic);
}
int JS_SetConstructor(JSContext* ctx, JSValueConst func_obj,
    JSValueConst proto);

typedef struct JSCFunctionListEntry {
    const char* name;
    uint8_t prop_flags;
    uint8_t def_type;
    int16_t magic;
    union {
        struct {
            uint8_t length;
            uint8_t cproto;
            JSCFunctionType cfunc;
        } func;
        struct {
            JSCFunctionType get;
            JSCFunctionType set;
        } getset;
        struct {
            const char* name;
            int base;
        } alias;
        struct {
            const struct JSCFunctionListEntry* tab;
            int len;
        } prop_list;
        const char* str;
        int32_t i32;
        int64_t i64;
        double f64;
    } u;
} JSCFunctionListEntry;

#define JS_DEF_CFUNC 0
#define JS_DEF_CGETSET 1
#define JS_DEF_CGETSET_MAGIC 2
#define JS_DEF_PROP_STRING 3
#define JS_DEF_PROP_INT32 4
#define JS_DEF_PROP_INT64 5
#define JS_DEF_PROP_DOUBLE 6
#define JS_DEF_PROP_UNDEFINED 7
#define JS_DEF_OBJECT 8
#define JS_DEF_ALIAS 9
#define JS_DEF_PROP_ATOM 10
#define JS_DEF_PROP_BOOL 11

#define JS_CFUNC_DEF(name, length, func1)                                                                                                   \
    {                                                                                                                                       \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } \
    }
#define JS_CFUNC_MAGIC_DEF(name, length, func1, magic)                                                                                                      \
    {                                                                                                                                                       \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } } \
    }
#define JS_CFUNC_SPECIAL_DEF(name, length, cproto, func1)                                                                                   \
    {                                                                                                                                       \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_##cproto, { .cproto = func1 } } } \
    }
#define JS_ITERATOR_NEXT_DEF(name, length, func1, magic)                                                                                                    \
    {                                                                                                                                                       \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_iterator_next, { .iterator_next = func1 } } } \
    }
#define JS_CGETSET_DEF(name, fgetter, fsetter)                                                                                           \
    {                                                                                                                                    \
        name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, .u = { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } \
    }
#define JS_CGETSET_MAGIC_DEF(name, fgetter, fsetter, magic)                                                                                                    \
    {                                                                                                                                                          \
        name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, magic, .u = { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } \
    }
#define JS_PROP_STRING_DEF(name, cstr, prop_flags)                    \
    {                                                                 \
        name, prop_flags, JS_DEF_PROP_STRING, 0, .u = { .str = cstr } \
    }
#define JS_PROP_INT32_DEF(name, val, prop_flags)                    \
    {                                                               \
        name, prop_flags, JS_DEF_PROP_INT32, 0, .u = { .i32 = val } \
    }
#define JS_PROP_INT64_DEF(name, val, prop_flags)                    \
    {                                                               \
        name, prop_flags, JS_DEF_PROP_INT64, 0, .u = { .i64 = val } \
    }
#define JS_PROP_DOUBLE_DEF(name, val, prop_flags)                    \
    {                                                                \
        name, prop_flags, JS_DEF_PROP_DOUBLE, 0, .u = { .f64 = val } \
    }
#define JS_PROP_UNDEFINED_DEF(name, prop_flags) { name, prop_flags, JS_DEF_PROP_UNDEFINED, 0, .u = { .i32 = 0 } }
#define JS_PROP_ATOM_DEF(name, val, prop_flags)                    \
    {                                                              \
        name, prop_flags, JS_DEF_PROP_ATOM, 0, .u = { .i32 = val } \
    }
#define JS_PROP_BOOL_DEF(name, val, prop_flags)                    \
    {                                                              \
        name, prop_flags, JS_DEF_PROP_BOOL, 0, .u = { .i32 = val } \
    }
#define JS_OBJECT_DEF(name, tab, len, prop_flags)                              \
    {                                                                          \
        name, prop_flags, JS_DEF_OBJECT, 0, .u = { .prop_list = { tab, len } } \
    }
#define JS_ALIAS_DEF(name, from)                                                                       \
    {                                                                                                  \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, -1 } } \
    }
#define JS_ALIAS_BASE_DEF(name, from, base)                                                              \
    {                                                                                                    \
        name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, base } } \
    }

int JS_SetPropertyFunctionList(JSContext* ctx, JSValueConst obj,
    const JSCFunctionListEntry* tab,
    int len);

typedef int JSModuleInitFunc(JSContext* ctx, JSModuleDef* m);

JSModuleDef* JS_NewCModule(JSContext* ctx, const char* name_str,
    JSModuleInitFunc* func);
int JS_AddModuleExport(JSContext* ctx, JSModuleDef* m, const char* name_str);
int JS_AddModuleExportList(JSContext* ctx, JSModuleDef* m,
    const JSCFunctionListEntry* tab, int len);
int JS_SetModuleExport(JSContext* ctx, JSModuleDef* m, const char* export_name,
    JSValue val);
int JS_SetModuleExportList(JSContext* ctx, JSModuleDef* m,
    const JSCFunctionListEntry* tab, int len);
int JS_SetModulePrivateValue(JSContext* ctx, JSModuleDef* m, JSValue val);
JSValue JS_GetModulePrivateValue(JSContext* ctx, JSModuleDef* m);

typedef struct {
    JS_BOOL show_hidden : 8;
    JS_BOOL raw_dump : 8;
    uint32_t max_depth;
    uint32_t max_string_length;
    uint32_t max_item_count;
} JSPrintValueOptions;

typedef void JSPrintValueWrite(void* opaque, const char* buf, size_t len);

void JS_PrintValueSetDefaultOptions(JSPrintValueOptions* options);
void JS_PrintValueRT(JSRuntime* rt, JSPrintValueWrite* write_func, void* write_opaque,
    JSValueConst val, const JSPrintValueOptions* options);
void JS_PrintValue(JSContext* ctx, JSPrintValueWrite* write_func, void* write_opaque,
    JSValueConst val, const JSPrintValueOptions* options);

#undef js_unlikely
#undef js_force_inline

#ifdef __cplusplus
}
#endif

#endif
