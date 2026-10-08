#ifndef DYNAJS_ENGINE_INTERNAL_H
#define DYNAJS_ENGINE_INTERNAL_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>
#include <fenv.h>
#include <math.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#endif

#include "cutils.h"

#if defined(CONFIG_SYSTEMLIBM)
#include <dlfcn.h>
#define JS_SYSTEMLIBM_WRAPPER(name)                           \
    static inline double js_sys_##name(double a)              \
    {                                                         \
        static double (*fn)(double);                          \
        if (unlikely(!fn)) {                                  \
            fn = (double (*)(double))dlsym(RTLD_NEXT, #name); \
            if (!fn)                                          \
                fn = name;                                    \
        }                                                     \
        return fn(a);                                         \
    }
JS_SYSTEMLIBM_WRAPPER(sqrt)
JS_SYSTEMLIBM_WRAPPER(fabs)
JS_SYSTEMLIBM_WRAPPER(floor)
JS_SYSTEMLIBM_WRAPPER(ceil)
JS_SYSTEMLIBM_WRAPPER(trunc)
#else
#define js_sys_sqrt sqrt
#define js_sys_fabs fabs
#define js_sys_floor floor
#define js_sys_ceil ceil
#define js_sys_trunc trunc
#endif
#include "list.h"
#include "dynajs.h"
#include "libregexp.h"
#include "libunicode.h"
#include "dtoa.h"
#include "dyna-simd-kernels.h"
#include "core/dyn-prng.h"

#define OPTIMIZE 1
#define SHORT_OPCODES 1
#ifndef CONFIG_FUSED_CMP
#define CONFIG_FUSED_CMP 1
#endif
#ifndef CONFIG_FUSED_ARITH
#define CONFIG_FUSED_ARITH 1
#endif
#ifndef CONFIG_FUSED_METHOD
#define CONFIG_FUSED_METHOD 1
#endif
#ifndef CONFIG_JUMP_SWITCH
#define CONFIG_JUMP_SWITCH 1
#endif
#if defined(__EMSCRIPTEN__) || defined(FORCE_SWITCH_DISPATCH)
#define DIRECT_DISPATCH 0
#else
#define DIRECT_DISPATCH 1
#endif

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

#if !defined(_WIN32)
#define CONFIG_PRINTF_RNDN
#endif

#if !defined(__EMSCRIPTEN__)
#define CONFIG_ATOMICS
#endif

#if !defined(__EMSCRIPTEN__)
#define CONFIG_STACK_CHECK
#endif

#ifdef CONFIG_ATOMICS
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#endif

enum {
    JS_CLASS_OBJECT = 1,
    JS_CLASS_ARRAY,
    JS_CLASS_ERROR,
    JS_CLASS_NUMBER,
    JS_CLASS_STRING,
    JS_CLASS_BOOLEAN,
    JS_CLASS_SYMBOL,
    JS_CLASS_ARGUMENTS,
    JS_CLASS_MAPPED_ARGUMENTS,
    JS_CLASS_DATE,
    JS_CLASS_MODULE_NS,
    JS_CLASS_C_FUNCTION,
    JS_CLASS_BYTECODE_FUNCTION,
    JS_CLASS_BOUND_FUNCTION,
    JS_CLASS_C_FUNCTION_DATA,
    JS_CLASS_GENERATOR_FUNCTION,
    JS_CLASS_FOR_IN_ITERATOR,
    JS_CLASS_REGEXP,
    JS_CLASS_ARRAY_BUFFER,
    JS_CLASS_SHARED_ARRAY_BUFFER,
    JS_CLASS_UINT8C_ARRAY,
    JS_CLASS_INT8_ARRAY,
    JS_CLASS_UINT8_ARRAY,
    JS_CLASS_INT16_ARRAY,
    JS_CLASS_UINT16_ARRAY,
    JS_CLASS_INT32_ARRAY,
    JS_CLASS_UINT32_ARRAY,
    JS_CLASS_BIG_INT64_ARRAY,
    JS_CLASS_BIG_UINT64_ARRAY,
    JS_CLASS_FLOAT16_ARRAY,
    JS_CLASS_FLOAT32_ARRAY,
    JS_CLASS_FLOAT64_ARRAY,
    JS_CLASS_DATAVIEW,
    JS_CLASS_BIG_INT,
    JS_CLASS_MAP,
    JS_CLASS_SET,
    JS_CLASS_WEAKMAP,
    JS_CLASS_WEAKSET,
    JS_CLASS_ITERATOR,
    JS_CLASS_ITERATOR_CONCAT,
    JS_CLASS_ITERATOR_ZIP,
    JS_CLASS_ITERATOR_HELPER,
    JS_CLASS_ITERATOR_WRAP,
    JS_CLASS_MAP_ITERATOR,
    JS_CLASS_SET_ITERATOR,
    JS_CLASS_ARRAY_ITERATOR,
    JS_CLASS_STRING_ITERATOR,
    JS_CLASS_REGEXP_STRING_ITERATOR,
    JS_CLASS_GENERATOR,
    JS_CLASS_GLOBAL_OBJECT,
    JS_CLASS_RAWJSON,
    JS_CLASS_PROXY,
    JS_CLASS_PROMISE,
    JS_CLASS_PROMISE_RESOLVE_FUNCTION,
    JS_CLASS_PROMISE_REJECT_FUNCTION,
    JS_CLASS_ASYNC_FUNCTION,
    JS_CLASS_ASYNC_FUNCTION_RESOLVE,
    JS_CLASS_ASYNC_FUNCTION_REJECT,
    JS_CLASS_ASYNC_FROM_SYNC_ITERATOR,
    JS_CLASS_ASYNC_GENERATOR_FUNCTION,
    JS_CLASS_ASYNC_GENERATOR,
    JS_CLASS_WEAK_REF,
    JS_CLASS_FINALIZATION_REGISTRY,
    JS_CLASS_DISPOSABLE_STACK,
    JS_CLASS_ASYNC_DISPOSABLE_STACK,

    JS_CLASS_INIT_COUNT,
};

#define JS_TYPED_ARRAY_COUNT (JS_CLASS_FLOAT64_ARRAY - JS_CLASS_UINT8C_ARRAY + 1)
#define typed_array_size_log2(classid) (typed_array_size_log2[(classid) - JS_CLASS_UINT8C_ARRAY])

typedef enum JSErrorEnum {
    JS_EVAL_ERROR,
    JS_RANGE_ERROR,
    JS_REFERENCE_ERROR,
    JS_SYNTAX_ERROR,
    JS_TYPE_ERROR,
    JS_URI_ERROR,
    JS_INTERNAL_ERROR,
    JS_AGGREGATE_ERROR,
    JS_SUPPRESSED_ERROR,

    JS_NATIVE_ERROR_COUNT,
} JSErrorEnum;

#define JS_MAX_LOCAL_VARS 65534
#define JS_STACK_SIZE_MAX 65534
#define JS_STRING_LEN_MAX ((1 << 30) - 1)

#define JS_SLICE_MIN_LEN_DEFAULT 64

#define JS_STRING_ROPE_SHORT_LEN 512
#define JS_STRING_ROPE_SHORT2_LEN 8192
#define JS_STRING_ROPE_MAX_DEPTH 60

#define __exception __attribute__((warn_unused_result))

typedef struct JSShape JSShape;
typedef struct JSString JSString;
typedef struct JSString JSAtomStruct;
typedef struct JSObject JSObject;

#define JS_VALUE_GET_OBJ(v) ((JSObject*)JS_VALUE_GET_PTR(v))
#define JS_VALUE_GET_STRING(v) ((JSString*)JS_VALUE_GET_PTR(v))
#define JS_VALUE_GET_STRING_ROPE(v) ((JSStringRope*)JS_VALUE_GET_PTR(v))

typedef enum {
    JS_GC_PHASE_NONE,
    JS_GC_PHASE_DECREF,
    JS_GC_PHASE_REMOVE_CYCLES,
} JSGCPhaseEnum;

typedef enum OPCodeEnum OPCodeEnum;

#define JS_MALLOC_ALIGN 8
#define JS_MALLOC_ARENA_SIZE 4096
#define JS_MALLOC_BLOCK_SIZE_COUNT 31
#define JS_MALLOC_MIN_SMALL_SIZE 16
#define JS_MALLOC_MAX_SMALL_SIZE 512
#if defined(__SANITIZE_ADDRESS__)
#define JS_MALLOC_LARGE_BLOCKS_ONLY 1
#else
#define JS_MALLOC_LARGE_BLOCKS_ONLY 0
#endif

#define FREE_NIL 0xffff

typedef struct JSMallocBlockHeader {
    union {
        uint16_t block_idx;
        uint16_t free_next;
    } u;
    uint8_t block_size_idx;
    uint8_t gc_obj_type : 7;
    uint8_t mark : 1;
    int ref_count;
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t user_data[];
} JSMallocBlockHeader;

typedef struct JSMallocLargeBlockHeader {
#ifdef JS_MALLOC_USE_ITER
    struct list_head link;
#endif
    JSMallocBlockHeader header;
} JSMallocLargeBlockHeader;

typedef struct {
    struct list_head free_link;
    struct list_head link;
    uint8_t block_size_idx;
    uint16_t n_used_blocks;
    uint16_t n_blocks;
    uint16_t first_free_block;
#ifdef JS_MALLOC_USE_ITER
    uint32_t bitmap[((JS_MALLOC_ARENA_SIZE / JS_MALLOC_MIN_SMALL_SIZE) + 31) / 32];
#endif
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t blocks[];
} JSMallocArena;

typedef struct {
    struct list_head arena_list[JS_MALLOC_BLOCK_SIZE_COUNT];
    struct list_head free_arena_list[JS_MALLOC_BLOCK_SIZE_COUNT];
    uint16_t n_empty_arenas[JS_MALLOC_BLOCK_SIZE_COUNT];
    uint8_t no_pools;
#ifdef JS_MALLOC_USE_ITER
    struct list_head large_block_list;
#endif
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t zero_size_block[sizeof(JSMallocBlockHeader)];

    JSMallocFunctions mf;
    JSMallocState malloc_state;
} JSMallocContext;

#define JS_OBJ_POOL_MAX_OBJECTS 1024
#define JS_ARRAY_POOL_NBUCKETS 11
#define JS_ARRAY_POOL_MAX_BUCKET (1 << (JS_ARRAY_POOL_NBUCKETS - 1))
#define JS_ARRAY_POOL_MAX_SLOTS 65536

struct JSDateFieldsCache;

#define JS_NATIVE_BORROW_STACK 32

struct JSRuntime {
    JSMallocContext malloc_ctx;
    const char* rt_info;

    struct JSDateFieldsCache* date_cache_free_list;

#ifdef CONFIG_OBJ_POOL
    struct JSObject* obj_pool_free_list;
    int obj_pool_count;
    void* array_pool_free_list[JS_ARRAY_POOL_NBUCKETS];
    int array_pool_count;
    int array_pool_slots;
#endif

#ifdef CONFIG_NURSERY_PROBE
    uint8_t *nursery_ptr, *nursery_end;
    void* nursery_chunks;
    uint64_t nursery_alloc_count, nursery_alloc_bytes;
#endif

    int atom_hash_size;
    int atom_count;
    int atom_size;
    int atom_count_resize;
    uint32_t* atom_hash;
    JSAtomStruct** atom_array;
    int atom_free_index;

    int class_count;
    JSClass* class_array;

    struct list_head context_list;
    struct list_head gc_obj_list;
    struct list_head gc_zero_ref_count_list;
    struct list_head tmp_obj_list;
    JSGCPhaseEnum gc_phase : 8;
    size_t malloc_gc_threshold;
    struct list_head weakref_list;
#ifdef DUMP_LEAKS
    struct list_head string_list;
#endif
    uintptr_t stack_size;
    uintptr_t stack_top;
    uintptr_t stack_limit;

    JSValue current_exception;
    BOOL current_exception_is_uncatchable : 8;
    BOOL in_out_of_memory : 8;

    struct JSStackFrame* current_stack_frame;
    uint64_t native_borrow_epoch;
    uint32_t native_borrow_depth;
    uint64_t native_borrow_stack[JS_NATIVE_BORROW_STACK];
    BOOL atom_hash_strong;
    uint64_t atom_hash_key;
    uint64_t atom_hash_key2;
    uint64_t atom_hash_init[8];

    JSInterruptHandler* interrupt_handler;
    void* interrupt_opaque;

    JSHostPromiseRejectionTracker* host_promise_rejection_tracker;
    void* host_promise_rejection_tracker_opaque;

    struct list_head job_list;

    struct list_head shutdown_sweeps;

    struct list_head shutdown_deferred;

    JSModuleNormalizeFunc* module_normalize_func;
    BOOL module_loader_has_attr;
    union {
        JSModuleLoaderFunc* module_loader_func;
        JSModuleLoaderFunc2* module_loader_func2;
    } u;
    JSModuleCheckSupportedImportAttributes* module_check_attrs;
    void* module_loader_opaque;
    int64_t module_async_evaluation_next_timestamp;

    BOOL can_block : 8;
    JSSharedArrayBufferFunctions sab_funcs;
    uint8_t strip_flags;

    int shape_hash_bits;
    int shape_hash_size;
    int shape_hash_count;
    JSShape** shape_hash;
    uint64_t atom_hash_seed;
    uint64_t shape_hash_seed;
    JSValue typeof_strings[8];
    JSString* latin1_char_cache[256];
    void* user_opaque;
};

struct JSClass {
    uint32_t class_id;
    JSAtom class_name;
    JSClassFinalizer* finalizer;
    JSClassGCMark* gc_mark;
    JSClassCall* call;
    const JSClassExoticMethods* exotic;
};

#define JS_MODE_STRICT (1 << 0)
#define JS_MODE_ASYNC (1 << 2)
#define JS_MODE_BACKTRACE_BARRIER (1 << 3)

typedef struct JSStackFrame {
    struct JSStackFrame* prev_frame;
    JSValue cur_func;
    JSValue* arg_buf;
    JSValue* var_buf;
    union {
        struct JSVarRef** var_refs;
        uint64_t borrow_epoch;
    };
    const uint8_t* cur_pc;
    int arg_count;
    int js_mode;
    JSValue* cur_sp;
} JSStackFrame;

typedef enum {
    JS_GC_OBJ_TYPE_JS_OBJECT,
    JS_GC_OBJ_TYPE_FUNCTION_BYTECODE,
    JS_GC_OBJ_TYPE_SHAPE,
    JS_GC_OBJ_TYPE_VAR_REF,
    JS_GC_OBJ_TYPE_ASYNC_FUNCTION,
    JS_GC_OBJ_TYPE_JS_CONTEXT,
    JS_GC_OBJ_TYPE_MODULE,
} JSGCObjectTypeEnum;

struct JSGCObjectHeader {
    struct list_head link;
};

typedef enum {
    JS_WEAKREF_TYPE_MAP,
    JS_WEAKREF_TYPE_WEAKREF,
    JS_WEAKREF_TYPE_FINREC,
} JSWeakRefHeaderTypeEnum;

typedef struct {
    struct list_head link;
    JSWeakRefHeaderTypeEnum weakref_type;
} JSWeakRefHeader;

typedef struct JSVarRef {
    JSGCObjectHeader header;
    uint8_t is_detached;
    uint8_t is_lexical;
    uint8_t is_const;
    JSValue* pvalue;
    union {
        JSValue value;
        struct {
            uint16_t var_ref_idx;
            JSStackFrame* stack_frame;
        };
    };
} JSVarRef;

#if JS_LIMB_BITS == 32

typedef int32_t js_slimb_t;
typedef uint32_t js_limb_t;
typedef int64_t js_sdlimb_t;
typedef uint64_t js_dlimb_t;

#define JS_LIMB_DIGITS 9

#else

typedef __int128 js_i128_t;
typedef unsigned __int128 js_u128_t;
typedef int64_t js_slimb_t;
typedef uint64_t js_limb_t;
typedef js_i128_t js_sdlimb_t;
typedef js_u128_t js_dlimb_t;

#define JS_LIMB_DIGITS 19

#endif

typedef struct JSBigInt {
    uint32_t len;
    js_limb_t tab[];
} JSBigInt;

typedef struct {
    js_limb_t big_int_buf[sizeof(JSBigInt) / sizeof(js_limb_t)];
    js_limb_t tab[(64 + JS_LIMB_BITS - 1) / JS_LIMB_BITS];
} JSBigIntBuf;

typedef enum {
    JS_AUTOINIT_ID_PROTOTYPE,
    JS_AUTOINIT_ID_MODULE_NS,
    JS_AUTOINIT_ID_PROP,
    JS_AUTOINIT_ID_COUNT,
} JSAutoInitIDEnum;

#define JS_INTERRUPT_COUNTER_INIT 10000

struct JSContext {
    JSGCObjectHeader header;
    JSRuntime* rt;
    struct list_head link;

    uint16_t binary_object_count;
    int binary_object_size;

    JSShape* array_shape;
    JSShape* arguments_shape;
    JSShape* mapped_arguments_shape;
    JSShape* regexp_shape;
    JSShape* regexp_result_shape;
    JSShape* iterator_result_shape;

    JSValue* class_proto;
    JSValue function_proto;
    JSValue function_ctor;
    JSValue array_ctor;
    JSValue regexp_ctor;
    JSValue promise_ctor;
    JSValue native_error_proto[JS_NATIVE_ERROR_COUNT];
    JSValue iterator_ctor;
    JSValue async_iterator_proto;
    JSValue array_proto_values;
    JSValue throw_type_error;
    JSValue eval_obj;

    JSValue global_obj;
    JSValue global_var_obj;

    uint64_t random_state;

    int interrupt_counter;

    struct list_head loaded_modules;

    JSValue (*compile_regexp)(JSContext* ctx, JSValueConst pattern,
        JSValueConst flags);
    JSValue (*eval_internal)(JSContext* ctx, JSValueConst this_obj,
        const char* input, size_t input_len,
        const char* filename, int flags, int scope_idx);
    void* user_opaque;
};

typedef union JSFloat64Union {
    double d;
    uint64_t u64;
    uint32_t u32[2];
} JSFloat64Union;

enum {
    JS_ATOM_TYPE_STRING = 1,
    JS_ATOM_TYPE_GLOBAL_SYMBOL,
    JS_ATOM_TYPE_SYMBOL,
    JS_ATOM_TYPE_PRIVATE,
};

typedef enum {
    JS_ATOM_KIND_STRING,
    JS_ATOM_KIND_SYMBOL,
    JS_ATOM_KIND_PRIVATE,
} JSAtomKindEnum;

#define JS_ATOM_HASH_MASK ((1 << 30) - 1)
#define JS_ATOM_HASH_PRIVATE JS_ATOM_HASH_MASK

typedef struct JSStringSlice {
    JSString* parent;
    uint32_t offset;
} JSStringSlice;

#define JS_SLICE_PREFIX_SIZE 16

struct JSString {
    uint32_t len : 30;
    uint32_t is_wide_char : 1;
    uint32_t is_slice : 1;
    uint32_t hash : 30;
    uint8_t atom_type : 2;
    uint32_t hash_next;
#ifdef DUMP_LEAKS
    struct list_head link;
#endif
    union {
        uint8_t str8[0];
        uint16_t str16[0];
    } u;
};

#if defined(__GNUC__) && !defined(__cplusplus)
_Static_assert(sizeof(JSString) == 3 * sizeof(uint32_t),
    "JSString header layout changed");
_Static_assert(sizeof(JSStringSlice) <= JS_SLICE_PREFIX_SIZE,
    "slice payload no longer fits the prefix block");
_Static_assert((JS_SLICE_PREFIX_SIZE & (sizeof(void*) - 1)) == 0,
    "slice prefix must keep the header pointer aligned");
#endif

#define js_slice_parent(p) (js_slice_payload(p)->parent)
#define js_slice_offset(p) (js_slice_payload(p)->offset)

typedef struct JSStringRope {
    uint32_t len;
    uint8_t is_wide_char;
    uint8_t depth;
    JSValue left;
    JSValue right;
} JSStringRope;

typedef enum {
    JS_CLOSURE_LOCAL,
    JS_CLOSURE_ARG,
    JS_CLOSURE_REF,
    JS_CLOSURE_GLOBAL_REF,
    JS_CLOSURE_GLOBAL_DECL,
    JS_CLOSURE_GLOBAL,
    JS_CLOSURE_MODULE_DECL,
    JS_CLOSURE_MODULE_IMPORT,
} JSClosureTypeEnum;

typedef struct JSClosureVar {
    JSClosureTypeEnum closure_type : 3;
    uint8_t is_lexical : 1;
    uint8_t is_const : 1;
    uint8_t var_kind : 4;
    uint16_t var_idx;
    JSAtom var_name;
} JSClosureVar;

#define ARG_SCOPE_INDEX 1
#define ARG_SCOPE_END (-2)

typedef enum {
    JS_VAR_NORMAL,
    JS_VAR_FUNCTION_DECL,
    JS_VAR_NEW_FUNCTION_DECL,
    JS_VAR_CATCH,
    JS_VAR_FUNCTION_NAME,
    JS_VAR_PRIVATE_FIELD,
    JS_VAR_PRIVATE_METHOD,
    JS_VAR_PRIVATE_GETTER,
    JS_VAR_PRIVATE_SETTER,
    JS_VAR_PRIVATE_GETTER_SETTER,
    JS_VAR_GLOBAL_FUNCTION_DECL,
} JSVarKindEnum;

typedef struct JSBytecodeVarDef {
    JSAtom var_name;
    int scope_next;
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1;
    uint8_t has_scope : 1;
    uint8_t var_kind : 4;
    uint16_t var_ref_idx;
} JSBytecodeVarDef;

#define PC2LINE_BASE (-1)
#define PC2LINE_RANGE 5
#define PC2LINE_OP_FIRST 1
#define PC2LINE_DIFF_PC_MAX ((255 - PC2LINE_OP_FIRST) / PC2LINE_RANGE)

typedef enum JSFunctionKindEnum {
    JS_FUNC_NORMAL = 0,
    JS_FUNC_GENERATOR = (1 << 0),
    JS_FUNC_ASYNC = (1 << 1),
    JS_FUNC_ASYNC_GENERATOR = (JS_FUNC_GENERATOR | JS_FUNC_ASYNC),
} JSFunctionKindEnum;

#ifndef CONFIG_PRESIZE_CTOR
#define CONFIG_PRESIZE_CTOR 1
#endif

#if CONFIG_PRESIZE_CTOR
typedef struct JSCtorPresize {
    int field_count;
    JSAtom* fields;
    JSShape* cached_shape;
} JSCtorPresize;
#endif

#ifndef CONFIG_PRESIZE_LITERAL
#define CONFIG_PRESIZE_LITERAL 1
#endif

#if !defined(CONFIG_LITERAL_BOILERPLATE)
#define CONFIG_LITERAL_BOILERPLATE 0
#endif
#if !defined(CONFIG_FAST_PROP_TEARDOWN)
#define CONFIG_FAST_PROP_TEARDOWN 0
#endif

#if CONFIG_PRESIZE_LITERAL
#define JS_PRESIZE_MAX_SITES 32
#define JS_PRESIZE_MAX_FIELDS 32

typedef struct JSObjectPresizeSite {
    uint32_t bc_offset;
    int field_count;
    JSAtom* fields;
    JSShape* cached_shape;
} JSObjectPresizeSite;

typedef struct JSObjectPresize {
    int site_count;
    int site_cap;
    JSObjectPresizeSite sites[];
} JSObjectPresize;
#endif


typedef struct JSFunctionBytecode {
    JSGCObjectHeader header;
    uint8_t js_mode;
    uint8_t has_prototype : 1;
    uint8_t has_simple_parameter_list : 1;
    uint8_t is_derived_class_constructor : 1;
    uint8_t need_home_object : 1;
    uint8_t func_kind : 2;
    uint8_t new_target_allowed : 1;
    uint8_t super_call_allowed : 1;
    uint8_t super_allowed : 1;
    uint8_t arguments_allowed : 1;
    uint8_t has_debug : 1;
    uint8_t read_only_bytecode : 1;
    uint8_t is_direct_or_indirect_eval : 1;
    uint8_t* byte_code_buf;
    int byte_code_len;
    JSAtom func_name;
    JSBytecodeVarDef* vardefs;
    JSClosureVar* closure_var;
    uint16_t arg_count;
    uint16_t var_count;
    uint16_t defined_arg_count;
    uint16_t stack_size;
    uint16_t var_ref_count;
    JSContext* realm;
    JSValue* cpool;
    int cpool_count;
    int closure_var_count;
#if CONFIG_PRESIZE_CTOR
    JSCtorPresize* ctor_presize;
#endif
#if CONFIG_PRESIZE_LITERAL
    JSObjectPresize* obj_presize;
#endif
    struct {
        JSAtom filename;
        int source_len;
        int pc2line_len;
        uint8_t* pc2line_buf;
        char* source;
    } debug;
} JSFunctionBytecode;

_Static_assert(__STDC_VERSION__ >= 201710L,
    "dynascript requires C17 (-std=gnu17); a lower -std= has drifted in");

_Static_assert(offsetof(JSFunctionBytecode, debug) + sizeof(((JSFunctionBytecode*)0)->debug) == sizeof(JSFunctionBytecode),
    "JSFunctionBytecode.debug must remain the LAST member: bc_read "
    "copies only up to offsetof(...,debug) and a later field becomes NULL");

_Static_assert(JS_TAG_FIRST < 0, "JS_TAG_FIRST must stay negative for NaN boxing");
_Static_assert(JS_TAG_FLOAT64 - JS_TAG_FIRST < 32,
    "tag range must stay small enough for the NaN-boxing addend");

typedef struct JSBoundFunction {
    JSValue func_obj;
    JSValue this_val;
    int argc;
    JSValue argv[0];
} JSBoundFunction;

typedef enum JSIteratorKindEnum {
    JS_ITERATOR_KIND_KEY,
    JS_ITERATOR_KIND_VALUE,
    JS_ITERATOR_KIND_KEY_AND_VALUE,
} JSIteratorKindEnum;

typedef struct JSForInIterator {
    JSValue obj;
    uint32_t idx;
    uint32_t atom_count;
    uint8_t in_prototype_chain;
    uint8_t is_array;
    JSPropertyEnum* tab_atom;
} JSForInIterator;

typedef struct JSRegExp {
    JSString* pattern;
    JSString* bytecode;
} JSRegExp;

typedef struct JSProxyData {
    JSValue target;
    JSValue handler;
    uint8_t is_func;
    uint8_t is_revoked;
} JSProxyData;

typedef struct JSArrayBuffer {
    int byte_length;
    int max_byte_length;
    uint8_t detached;
    uint8_t shared;
    uint8_t* data;
    uint64_t borrow_seq;
    struct list_head array_list;
    void* opaque;
    JSFreeArrayBufferDataFunc* free_func;
} JSArrayBuffer;

typedef struct JSTypedArray {
    struct list_head link;
    JSObject* obj;
    JSObject* buffer;
    uint32_t offset;
    uint32_t length;
    BOOL track_rab;
} JSTypedArray;

typedef struct JSGlobalObject {
    JSValue uninitialized_vars;
} JSGlobalObject;

typedef struct JSAsyncFunctionState {
    JSGCObjectHeader header;
    JSValue this_val;
    int argc;
    BOOL throw_flag;
    BOOL is_completed;
    JSValue resolving_funcs[2];
    JSStackFrame frame;
} JSAsyncFunctionState;

typedef enum {
    JS_OVOP_ADD,
    JS_OVOP_SUB,
    JS_OVOP_MUL,
    JS_OVOP_DIV,
    JS_OVOP_MOD,
    JS_OVOP_POW,
    JS_OVOP_OR,
    JS_OVOP_AND,
    JS_OVOP_XOR,
    JS_OVOP_SHL,
    JS_OVOP_SAR,
    JS_OVOP_SHR,
    JS_OVOP_EQ,
    JS_OVOP_LESS,

    JS_OVOP_BINARY_COUNT,
    JS_OVOP_POS = JS_OVOP_BINARY_COUNT,
    JS_OVOP_NEG,
    JS_OVOP_INC,
    JS_OVOP_DEC,
    JS_OVOP_NOT,

    JS_OVOP_COUNT,
} JSOverloadableOperatorEnum;

typedef struct {
    uint32_t operator_index;
    JSObject* ops[JS_OVOP_BINARY_COUNT];
} JSBinaryOperatorDefEntry;

typedef struct {
    int count;
    JSBinaryOperatorDefEntry* tab;
} JSBinaryOperatorDef;

typedef struct {
    uint32_t operator_counter;
    BOOL is_primitive;
    JSObject* self_ops[JS_OVOP_COUNT];
    JSBinaryOperatorDef left;
    JSBinaryOperatorDef right;
} JSOperatorSetData;

typedef struct JSReqModuleEntry {
    JSAtom module_name;
    JSModuleDef* module;
    JSValue attributes;
} JSReqModuleEntry;

typedef enum JSExportTypeEnum {
    JS_EXPORT_TYPE_LOCAL,
    JS_EXPORT_TYPE_INDIRECT,
} JSExportTypeEnum;

typedef struct JSExportEntry {
    union {
        struct {
            int var_idx;
            JSVarRef* var_ref;
        } local;
        int req_module_idx;
    } u;
    JSExportTypeEnum export_type;
    JSAtom local_name;
    JSAtom export_name;
} JSExportEntry;

typedef struct JSStarExportEntry {
    int req_module_idx;
} JSStarExportEntry;

typedef struct JSImportEntry {
    int var_idx;
    BOOL is_star;
    JSAtom import_name;
    int req_module_idx;
} JSImportEntry;

typedef enum {
    JS_MODULE_STATUS_UNLINKED,
    JS_MODULE_STATUS_LINKING,
    JS_MODULE_STATUS_LINKED,
    JS_MODULE_STATUS_EVALUATING,
    JS_MODULE_STATUS_EVALUATING_ASYNC,
    JS_MODULE_STATUS_EVALUATED,
} JSModuleStatus;

struct JSModuleDef {
    JSGCObjectHeader header;
    JSAtom module_name;
    struct list_head link;

    JSReqModuleEntry* req_module_entries;
    int req_module_entries_count;
    int req_module_entries_size;

    JSExportEntry* export_entries;
    int export_entries_count;
    int export_entries_size;

    JSStarExportEntry* star_export_entries;
    int star_export_entries_count;
    int star_export_entries_size;

    JSImportEntry* import_entries;
    int import_entries_count;
    int import_entries_size;

    JSValue module_ns;
    JSValue func_obj;
    JSModuleInitFunc* init_func;
    BOOL has_tla : 8;
    BOOL resolved : 8;
    BOOL func_created : 8;
    JSModuleStatus status : 8;
    int dfs_index, dfs_ancestor_index;
    JSModuleDef* stack_prev;
    JSModuleDef** async_parent_modules;
    int async_parent_modules_count;
    int async_parent_modules_size;
    int pending_async_dependencies;
    BOOL async_evaluation;
    int64_t async_evaluation_timestamp;
    JSModuleDef* cycle_root;
    JSValue promise;
    JSValue resolving_funcs[2];

    BOOL eval_has_exception : 8;
    JSValue eval_exception;
    JSValue meta_obj;
    JSValue private_value;
};

typedef struct JSJobEntry {
    struct list_head link;
    JSContext* realm;
    JSJobFunc* job_func;
    int argc;
    JSValue argv[0];
} JSJobEntry;

typedef struct JSProperty {
    union {
        JSValue value;
        struct {
            JSObject* getter;
            JSObject* setter;
        } getset;
        JSVarRef* var_ref;
        struct {
            uintptr_t realm_and_id;
            void* opaque;
        } init;
    } u;
} JSProperty;

#define JS_PROP_INITIAL_SIZE 2
#define JS_PROP_INITIAL_HASH_SIZE 4

typedef struct JSShapeProperty {
    uint32_t hash_next : 26;
    uint32_t flags : 6;
    JSAtom atom;
} JSShapeProperty;

struct JSShape {
    JSGCObjectHeader header;
    uint8_t is_hashed;
#if CONFIG_FAST_PROP_TEARDOWN
    uint8_t all_plain;
#endif
    uint32_t hash;
    uint32_t prop_hash_mask;
    int prop_size;
    int prop_count;
    int deleted_prop_count;
    JSShape* shape_hash_next;
    JSObject* proto;
    uint32_t hash_table[];
};

typedef struct JSDateFieldsCacheEntry {
    double time;
    uint32_t tz_epoch;
    uint32_t valid;
    int32_t fields[9];
} JSDateFieldsCacheEntry;

typedef struct JSDateFieldsCache {
    JSDateFieldsCacheEntry e[2];
    struct JSDateFieldsCache* next_free;
} JSDateFieldsCache;

struct JSObject {
    JSGCObjectHeader header;
    uint8_t is_std_array_prototype : 1;

    uint8_t extensible : 1;
    uint8_t free_mark : 1;
    uint8_t is_exotic : 1;
    uint8_t fast_array : 1;
    uint8_t is_constructor : 1;
    uint8_t has_immutable_prototype : 1;
    uint8_t tmp_mark : 1;
    uint8_t is_HTMLDDA : 1;
    uint16_t class_id;
    uint32_t weakref_count;
    JSShape* shape;
    JSProperty* prop;
    union {
        void* opaque;
        struct JSBoundFunction* bound_function;
        struct JSCFunctionDataRecord* c_function_data_record;
        struct JSForInIterator* for_in_iterator;
        struct JSArrayBuffer* array_buffer;
        struct JSTypedArray* typed_array;
        struct JSMapState* map_state;
        struct JSMapIteratorData* map_iterator_data;
        struct JSArrayIteratorData* array_iterator_data;
        struct JSRegExpStringIteratorData* regexp_string_iterator_data;
        struct JSGeneratorData* generator_data;
        struct JSIteratorConcatData* iterator_concat_data;
        struct JSIteratorZipData* iterator_zip_data;
        struct JSIteratorHelperData* iterator_helper_data;
        struct JSIteratorWrapData* iterator_wrap_data;
        struct JSProxyData* proxy_data;
        struct JSPromiseData* promise_data;
        struct JSPromiseFunctionData* promise_function_data;
        struct JSAsyncFunctionState* async_function_data;
        struct JSAsyncFromSyncIteratorData* async_from_sync_iterator_data;
        struct JSAsyncGeneratorData* async_generator_data;
        struct {
            struct JSFunctionBytecode* function_bytecode;
            JSVarRef** var_refs;
            JSObject* home_object;
        } func;
        struct {
            JSContext* realm;
            JSCFunctionType c_function;
            uint8_t length;
            uint8_t cproto;
            int16_t magic;
        } cfunc;
        struct {
            union {
                uint32_t size;
                struct JSTypedArray* typed_array;
            } u1;
            union {
                JSValue* values;
                JSVarRef** var_refs;
                void* ptr;
                int8_t* int8_ptr;
                uint8_t* uint8_ptr;
                int16_t* int16_ptr;
                uint16_t* uint16_ptr;
                int32_t* int32_ptr;
                uint32_t* uint32_ptr;
                int64_t* int64_ptr;
                uint64_t* uint64_ptr;
                uint16_t* fp16_ptr;
                float* float_ptr;
                double* double_ptr;
            } u;
            uint32_t count;
        } array;
        JSRegExp regexp;
        JSValue object_data;
        struct {
            JSValue object_data;
            JSDateFieldsCache* fields_cache;
        } date;
        JSGlobalObject global_object;
    } u;
};

_Static_assert(offsetof(struct JSObject, u.date.object_data) == offsetof(struct JSObject, u.object_data),
    "u.date.object_data must alias u.object_data");
_Static_assert(sizeof(((struct JSObject*)0)->u.date) <= 3 * sizeof(void*),
    "u.date must not grow the JSObject union");

typedef struct JSMapRecord {
    int ref_count;
    BOOL empty : 8;
    struct list_head link;
    struct JSMapRecord* hash_next;
    JSValue key;
    JSValue value;
} JSMapRecord;

typedef struct JSMapState {
    BOOL is_weak;
    struct list_head records;
    uint32_t record_count;
    JSMapRecord** hash_table;
    int hash_bits;
    uint32_t hash_size;
    uint32_t record_count_threshold;
    BOOL hash_strong;
    JSWeakRefHeader weakref_header;
} JSMapState;

enum {
    __JS_ATOM_NULL = JS_ATOM_NULL,
#define DEF(name, str) JS_ATOM_##name,
#include "dyna-atom.h"
#undef DEF
    JS_ATOM_END,
};
#define JS_ATOM_LAST_KEYWORD JS_ATOM_super
#define JS_ATOM_LAST_STRICT_KEYWORD JS_ATOM_yield

typedef enum OPCodeFormat {
#define FMT(f) OP_FMT_##f,
#define DEF(id, size, n_pop, n_push, f)
#include "dyna-opcode.h"
#undef DEF
#undef FMT
} OPCodeFormat;

enum OPCodeEnum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f)
#include "dyna-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT,
    OP_TEMP_START = OP_nop + 1,
    OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_##id,
#include "dyna-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_TEMP_END,
};

enum OP2CodeEnum {
#define DEF2(id, size, n_pop, n_push, f) OP2_##id,
#include "dyna-opcode2.h"
#undef DEF2
    OP2_COUNT,
};

_Static_assert(OP_COUNT <= 256, "opcode count must fit the 256-entry dispatch table");
_Static_assert(OP2_COUNT <= 256, "OP_ext opcode count must fit dispatch_table2");
#define OP2_ARITH_FIRST OP2_mul_loc_loc
#define OP2_ARITH_LAST OP2_sub_loc_loc
#define OP2_BRANCH_FIRST OP2_streq_const_if_false
#define OP2_BRANCH_LAST OP2_streq_varprop_if_false
#define OP2_CALL_FIRST OP2_call_method0
#define OP2_CALL_LAST OP2_call_method1_const

static inline uint32_t switch_tbl_get(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void switch_tbl_put(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

void free_function_bytecode(JSRuntime* rt, JSFunctionBytecode* b);
JSValue JS_CallFree(JSContext* ctx, JSValue func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv);
__exception int JS_ToArrayLengthFree(JSContext* ctx, uint32_t* plen,
    JSValue val, BOOL is_array_ctor);
JSValue JS_EvalObject(JSContext* ctx, JSValueConst this_obj,
    JSValueConst val, int flags, int scope_idx);
JSValue __attribute__((format(printf, 2, 3))) JS_ThrowInternalError(JSContext* ctx, const char* fmt, ...);
JSValue js_function_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);
void js_array_iterator_finalizer(JSRuntime* rt, JSValue val);
void js_array_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
void js_iterator_wrap_finalizer(JSRuntime* rt, JSValue val);
void js_iterator_wrap_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

#define HINT_STRING 0
#define HINT_NUMBER 1
#define HINT_NONE 2
#define HINT_FORCE_ORDINARY (1 << 4)
JSValue JS_ToStringFree(JSContext* ctx, JSValue val);
int JS_ToBoolFree(JSContext* ctx, JSValue val);
int JS_ToFloat64Free(JSContext* ctx, double* pres, JSValue val);
JSValue js_new_string8_len(JSContext* ctx, const char* buf, int len);

typedef enum JSStrictEqModeEnum {
    JS_EQ_STRICT,
    JS_EQ_SAME_VALUE,
    JS_EQ_SAME_VALUE_ZERO,
} JSStrictEqModeEnum;

BOOL js_strict_eq2(JSContext* ctx, JSValueConst op1, JSValueConst op2,
    JSStrictEqModeEnum eq_mode);
BOOL js_same_value(JSContext* ctx, JSValueConst op1, JSValueConst op2);
JSValue JS_ToObject(JSContext* ctx, JSValueConst val);
JSValue JS_ToObjectFree(JSContext* ctx, JSValue val);
JSProperty* add_property(JSContext* ctx,
    JSObject* p, JSAtom prop, int prop_flags);
void free_property(JSRuntime* rt, JSProperty* pr, int prop_flags);
JSValue JS_ThrowOutOfMemory(JSContext* ctx);

JSValue js_array_buffer_constructor3(JSContext* ctx,
    JSValueConst new_target,
    uint64_t len, uint64_t* max_len,
    JSClassID class_id,
    uint8_t* buf,
    JSFreeArrayBufferDataFunc* free_func,
    void* opaque, BOOL alloc_flag);
void js_array_buffer_free(JSRuntime* rt, void* opaque, void* ptr);
JSArrayBuffer* js_get_array_buffer(JSContext* ctx, JSValueConst obj);
JSValue js_typed_array_constructor(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv,
    int classid);
BOOL typed_array_is_oob(JSObject* p);
int js_typed_array_get_length_unsafe(JSContext* ctx, JSValueConst obj);
JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext* ctx);
JSValue JS_ThrowTypeErrorArrayBufferOOB(JSContext* ctx);
JSVarRef* js_create_var_ref(JSContext* ctx, BOOL is_lexical);
void js_free_module_def(JSRuntime* rt, JSModuleDef* m);
void js_mark_module_def(JSRuntime* rt, JSModuleDef* m,
    JS_MarkFunc* mark_func);
JSValue js_import_meta(JSContext* ctx);
JSValue js_dynamic_import(JSContext* ctx, JSValueConst specifier, JSValueConst options);
void free_var_ref(JSRuntime* rt, JSVarRef* var_ref);
__exception int perform_promise_then(JSContext* ctx,
    JSValueConst promise,
    JSValueConst* resolve_reject,
    JSValueConst* cap_resolving_funcs);
JSValue js_promise_resolve(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);
JSValue js_promise_then(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
int js_string_compare(JSContext* ctx,
    const JSString* p1, const JSString* p2);
int JS_SetPropertyValue(JSContext* ctx, JSValueConst this_obj,
    JSValue prop, JSValue val, int flags);
int JS_GetOwnPropertyInternal(JSContext* ctx, JSPropertyDescriptor* desc,
    JSObject* p, JSAtom prop);
void js_free_desc(JSContext* ctx, JSPropertyDescriptor* desc);
void js_free_shape(JSRuntime* rt, JSShape* sh);
int js_shape_prepare_update(JSContext* ctx, JSObject* p,
    JSShapeProperty** pprs);
__exception int js_get_length32(JSContext* ctx, uint32_t* pres,
    JSValueConst obj);
__exception int js_get_length64(JSContext* ctx, int64_t* pres,
    JSValueConst obj);
void free_arg_list(JSContext* ctx, JSValue* tab, uint32_t len);
JSValue* build_arg_list(JSContext* ctx, uint32_t* plen,
    JSValueConst array_arg);
BOOL js_get_fast_array(JSContext* ctx, JSValueConst obj,
    JSValue** arrpp, uint32_t* countp);
BOOL js_for_of_next_fast_array(JSContext* ctx, JSValue* sp, int offset,
    JSValue* pvalue, int* pdone);
JSValue JS_CreateAsyncFromSyncIterator(JSContext* ctx,
    JSValueConst sync_iter);
void add_gc_object(JSRuntime* rt, JSGCObjectHeader* h,
    JSGCObjectTypeEnum type);
void remove_gc_object(JSGCObjectHeader* h);
JSValue js_module_ns_autoinit(JSContext* ctx, JSObject* p, JSAtom atom,
    void* opaque);
extern const JSClassExoticMethods js_module_ns_exotic_methods;
JSValue JS_InstantiateFunctionListItem2(JSContext* ctx, JSObject* p,
    JSAtom atom, void* opaque);
JSValue js_object_groupBy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_map);
int js_string_find_invalid_codepoint(JSString* p);

typedef struct BlockEnv {
    struct BlockEnv* prev;
    JSAtom label_name;
    int label_break;
    int label_cont;
    int drop_count;
    int label_finally;
    int scope_level;
    uint8_t has_iterator : 1;
    uint8_t is_regular_stmt : 1;
    uint8_t is_async_iterator : 1;
    uint8_t is_using_head : 1;
} BlockEnv;

typedef struct UsingScope {
    struct UsingScope* prev;
    BlockEnv be;
    int scope_level;
    int label_catch;
    int label_finally;
    int label_end;
    uint8_t has_async : 1;
    uint8_t in_for_of : 1;
    uint8_t try_started : 1;
} UsingScope;

typedef struct JSGlobalVar {
    int cpool_idx;
    uint8_t force_init : 1;
    uint8_t is_lexical : 1;
    uint8_t is_const : 1;
    int scope_level;
    JSAtom var_name;
} JSGlobalVar;

typedef struct RelocEntry {
    struct RelocEntry* next;
    uint32_t addr;
    int size;
} RelocEntry;

typedef struct JumpSlot {
    int op;
    int size;
    int pos;
    int label;
} JumpSlot;

typedef struct LabelSlot {
    int ref_count;
    int pos;
    int pos2;
    int addr;
    RelocEntry* first_reloc;
} LabelSlot;

typedef struct LineNumberSlot {
    uint32_t pc;
    uint32_t source_pos;
} LineNumberSlot;

typedef struct {
    const uint8_t* ptr;
    int line_num;
    int col_num;
    const uint8_t* buf_start;
} GetLineColCache;

typedef enum JSParseFunctionEnum {
    JS_PARSE_FUNC_STATEMENT,
    JS_PARSE_FUNC_VAR,
    JS_PARSE_FUNC_EXPR,
    JS_PARSE_FUNC_ARROW,
    JS_PARSE_FUNC_GETTER,
    JS_PARSE_FUNC_SETTER,
    JS_PARSE_FUNC_METHOD,
    JS_PARSE_FUNC_CLASS_STATIC_INIT,
    JS_PARSE_FUNC_CLASS_CONSTRUCTOR,
    JS_PARSE_FUNC_DERIVED_CLASS_CONSTRUCTOR,
} JSParseFunctionEnum;

typedef enum JSParseExportEnum {
    JS_PARSE_EXPORT_NONE,
    JS_PARSE_EXPORT_NAMED,
    JS_PARSE_EXPORT_DEFAULT,
} JSParseExportEnum;

typedef struct JSVarScope {
    int parent;
    int first;
    uint32_t* name_hash;
    uint32_t name_hash_size;
    uint32_t name_hash_count;
} JSVarScope;

typedef struct JSVarDef {
    JSAtom var_name;
    int scope_level;
    int scope_next;
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1;
    uint8_t is_static_private : 1;
    uint8_t var_kind : 4;
    uint16_t var_ref_idx;
    int func_pool_idx;
} JSVarDef;

typedef struct JSSwitchInfo {
    int cp_idx;
    int min;
    int count;
    int* labels;
} JSSwitchInfo;

typedef struct JSFunctionDef JSFunctionDef;

typedef struct CPMember {
    JSAtom key;
    int32_t val;
    uint8_t kind;
    int killed;
} CPMember;

enum {
    CPV_NONE = 0,
    CPV_INT,
    CPV_STR,
    CPV_TRUE,
    CPV_FALSE,
    CPV_NULL,
};

typedef struct CPBinding {
    JSAtom name;
    uint32_t decl_src_off;
    int is_frozen;
    int decl_first;
    int name_killed;
    JSFunctionDef* decl_fd;
    CPMember* members;
    int member_count;
    int member_size;
} CPBinding;

typedef struct CPSite {
    JSFunctionDef* fd;
    int off;
    int end;
    int bind_idx;
    int member_idx;
    int dynamic;
    JSAtom prop_atom;
    int case_cp_idx;
    JSFunctionDef* case_fd;
    uint32_t src_off;
    int in_function;
    int folded;
} CPSite;

typedef struct CPTmpEnt {
    JSAtom key;
    int32_t val;
    uint8_t kind;
} CPTmpEnt;

typedef struct JSFunctionDef {
    JSContext* ctx;
    struct JSFunctionDef* parent;
    int parent_cpool_idx;
    int parent_scope_level;
    struct list_head child_list;
    struct list_head link;

    uint8_t is_eval : 1;
    uint8_t is_global_var : 1;
    uint8_t is_func_expr : 1;
    uint8_t has_home_object : 1;
    uint8_t has_prototype : 1;
    uint8_t has_simple_parameter_list : 1;
    uint8_t has_parameter_expressions : 1;
    uint8_t has_use_strict : 1;
    uint8_t has_eval_call : 1;
    uint8_t has_arguments_binding : 1;
    uint8_t has_this_binding : 1;
    uint8_t new_target_allowed : 1;
    uint8_t super_call_allowed : 1;
    uint8_t super_allowed : 1;
    uint8_t arguments_allowed : 1;
    uint8_t is_derived_class_constructor : 1;
    uint8_t in_function_body : 1;
    uint8_t need_home_object : 1;
    uint8_t has_await : 1;
    uint8_t strip_debug : 1;
    uint8_t strip_source : 1;
    uint8_t has_class_instance_fields : 1;
    JSFunctionKindEnum func_kind : 8;
    JSParseFunctionEnum func_type : 8;
    uint8_t js_mode;
    int eval_type;
    JSAtom func_name;

    JSVarDef* vars;
    int var_size;
    int var_count;
    uint32_t* var_hash;
    int var_hash_size;
    int var_hash_stamp;
    JSVarDef* args;
    int arg_size;
    int arg_count;
    uint32_t* arg_hash;
    int arg_hash_size;
    int arg_hash_stamp;
    uint32_t* dup_var_hash;
    int dup_var_hash_size;
    int dup_var_hash_stamp;
    int defined_arg_count;
    int var_ref_count;
    int var_object_idx;
    int arg_var_object_idx;
    int arguments_var_idx;
    int arguments_arg_idx;
    int func_var_idx;
    int eval_ret_idx;
    int this_var_idx;
    int new_target_var_idx;
    int this_active_func_var_idx;
    int home_object_var_idx;

    int scope_level;
    int scope_first;
    int scope_size;
    int scope_count;
    JSVarScope* scopes;
    JSVarScope def_scope_array[4];
    int body_scope;

    int global_var_count;
    int global_var_size;
    JSGlobalVar* global_vars;
    uint32_t* global_var_hash;
    int global_var_hash_size;

    DynBuf byte_code;
    int last_opcode_pos;
    const uint8_t* last_opcode_source_ptr;
    BOOL use_short_opcodes;

    LabelSlot* label_slots;
    int label_size;
    int label_count;
    BlockEnv* top_break;

    UsingScope* top_using;

    JSValue* cpool;
    int cpool_count;
    int cpool_size;

    struct JSSwitchInfo* switch_infos;
    int switch_info_count;
    int switch_info_size;

    int closure_var_count;
    int closure_var_size;
    JSClosureVar* closure_var;
    uint32_t* closure_hash;
    int closure_hash_size;
    int closure_var_pseudo_count;

    JumpSlot* jump_slots;
    int jump_size;
    int jump_count;

    LineNumberSlot* line_number_slots;
    int line_number_size;
    int line_number_count;
    int line_number_last;
    int line_number_last_pc;

    JSAtom filename;
    uint32_t source_pos;
    GetLineColCache* get_line_col_cache;
    DynBuf pc2line;

    char* source;
    int source_len;

    JSModuleDef* module;

    CPBinding* cp_binds;
    int cp_bind_count;
    int cp_bind_size;
    CPSite* cp_sites;
    int cp_site_count;
    int cp_site_size;
    struct CPLV {
        int bind_idx;
        int member_idx;
        int all_kill;
    }* cp_lvs;
    int cp_lv_count;
    int cp_lv_size;
    int cp_eval_with_seen;
    JSAtom cp_freeze_atom;
    int cp_prog_off;
} JSFunctionDef;

typedef struct JSToken {
    int val;
    const uint8_t* ptr;
    union {
        struct {
            JSValue str;
            int sep;
        } str;
        struct {
            JSValue val;
        } num;
        struct {
            JSAtom atom;
            BOOL has_escape;
            BOOL is_reserved;
        } ident;
        struct {
            JSValue body;
            JSValue flags;
        } regexp;
    } u;
} JSToken;

typedef struct JSParseState {
    JSContext* ctx;
    const char* filename;
    JSToken token;
    BOOL got_lf;
    const uint8_t* last_ptr;
    const uint8_t* buf_start;
    const uint8_t* buf_ptr;
    const uint8_t* buf_end;

    JSFunctionDef* cur_func;
    BOOL is_module;
    BOOL allow_html_comments;
    BOOL allow_using;
    BOOL ext_json;
    GetLineColCache get_line_col_cache;

    JSFunctionDef* cp_top;
    JSFunctionDef* cp_init_fd;
    int cp_init_active;
    int cp_init_start;
    int cp_init_frozen;
    int cp_lit_start;
    int cp_lit_end;
    int cp_objlit_simple;
    int cp_objlit_active;
    struct {
        JSFunctionDef* fd;
        int end;
        int off;
        int bind_idx;
        uint32_t src_off;
    } cp_last_ident;
    CPTmpEnt* cp_tmp;
    int cp_tmp_count;
    int cp_tmp_size;
    int cp_last_site;
    uint64_t lookahead_budget;
} JSParseState;

typedef struct JSOpCode {
#ifdef DUMP_BYTECODE
    const char* name;
#endif
    uint8_t size;
    uint8_t n_pop;
    uint8_t n_push;
    uint8_t fmt;
} JSOpCode;

extern const JSOpCode opcode_info[OP_COUNT + (OP_TEMP_END - OP_TEMP_START)];
extern const JSOpCode opcode_info2[OP2_COUNT];
#if SHORT_OPCODES
#define short_opcode_info(op) \
    opcode_info[(op) >= OP_TEMP_START ? (op) + (OP_TEMP_END - OP_TEMP_START) : (op)]
#else
#define short_opcode_info(op) opcode_info[op]
#endif

typedef struct StringBuffer {
    JSContext* ctx;
    JSString* str;
    int len;
    int size;
    int is_wide_char;
    int error_status;
} StringBuffer;
#define ATOM_GET_STR_BUF_SIZE 64
#define ATOD_INT_ONLY (1 << 0)
#define ATOD_ACCEPT_BIN_OCT (1 << 2)
#define ATOD_ACCEPT_LEGACY_OCTAL (1 << 4)
#define ATOD_ACCEPT_UNDERSCORES (1 << 5)
#define ATOD_ACCEPT_SUFFIX (1 << 6)
#define ATOD_TYPE_MASK (3 << 7)
#define ATOD_TYPE_FLOAT64 (0 << 7)
#define ATOD_TYPE_BIG_INT (1 << 7)
#define ATOD_ACCEPT_PREFIX_AFTER_SIGN (1 << 10)
#define GLOBAL_VAR_OFFSET 0x40000000
#define ARGUMENT_VAR_OFFSET 0x20000000
#define JS_THROW_VAR_RO 0
#define JS_THROW_VAR_REDECL 1
#define JS_THROW_VAR_UNINITIALIZED 2
#define JS_THROW_ERROR_DELETE_SUPER 3
#define JS_THROW_ERROR_ITERATOR_THROW 4
#define OP_DEFINE_METHOD_METHOD 0
#define OP_DEFINE_METHOD_GETTER 1
#define OP_DEFINE_METHOD_SETTER 2
#define OP_DEFINE_METHOD_ENUMERABLE 4
typedef enum JSFreeModuleEnum {
    JS_FREE_MODULE_ALL,
    JS_FREE_MODULE_NOT_RESOLVED,
} JSFreeModuleEnum;
typedef enum {
    OP_SPECIAL_OBJECT_ARGUMENTS,
    OP_SPECIAL_OBJECT_MAPPED_ARGUMENTS,
    OP_SPECIAL_OBJECT_THIS_FUNC,
    OP_SPECIAL_OBJECT_NEW_TARGET,
    OP_SPECIAL_OBJECT_HOME_OBJECT,
    OP_SPECIAL_OBJECT_VAR_OBJECT,
    OP_SPECIAL_OBJECT_IMPORT_META,
} OPSpecialObjectEnum;
#define JS_DEFINE_CLASS_HAS_HERITAGE (1 << 0)
#define JS_ThrowSyntaxErrorAtom(ctx, fmt, atom) __JS_ThrowSyntaxErrorAtom(ctx, atom, fmt, "")
void js_free_modules(JSContext* ctx, JSFreeModuleEnum flag);
void set_value(JSContext* ctx, JSValue* pval, JSValue new_val);
BOOL js_check_stack_overflow(JSRuntime* rt, size_t alloca_size);
int is_digit(int c);
void js_dbuf_bytecode_init(JSContext* ctx, DynBuf* s);
void js_dbuf_init(JSContext* ctx, DynBuf* s);
int js_resize_array(JSContext* ctx, void** parray, int elem_size, int* psize, int req_size);
JSMallocBlockHeader* js_rc(void* ptr);
JSAtom js_atom_concat_num(JSContext* ctx, JSAtom name, uint32_t n);
JSAtom js_atom_concat_str(JSContext* ctx, JSAtom name, const char* str1);
const char* JS_AtomGetStr(JSContext* ctx, char* buf, int buf_size, JSAtom atom);
JSAtom JS_NewAtomStr(JSContext* ctx, JSString* p);
JSAtomKindEnum JS_AtomGetKind(JSContext* ctx, JSAtom v);
JSAtom __JS_AtomFromUInt32(uint32_t v);
BOOL __JS_AtomIsTaggedInt(JSAtom v);
JSValue JS_ThrowStackOverflow(JSContext* ctx);
JSValue __attribute__((format(printf, 3, 4))) __JS_ThrowSyntaxErrorAtom(JSContext* ctx, JSAtom atom, const char* fmt, ...);
JSValue __attribute__((format(printf, 3, 0))) JS_ThrowError2(JSContext* ctx, JSErrorEnum error_num, const char* fmt, va_list ap, BOOL add_backtrace);
void build_backtrace(JSContext* ctx, JSValueConst error_obj, const char* filename, int line_num, int col_num, int backtrace_flags);
void dbuf_put_sleb128(DynBuf* s, int32_t v1);
void dbuf_put_leb128(DynBuf* s, uint32_t v);
JSShapeProperty* find_own_property1(JSObject* p, JSAtom atom);
BOOL js_class_has_bytecode(JSClassID class_id);
JSValue js_closure(JSContext* ctx, JSValue bfunc, JSVarRef** cur_var_refs, JSStackFrame* sf, BOOL is_eval);
JSValue js_closure2(JSContext* ctx, JSValue func_obj, JSFunctionBytecode* b, JSVarRef** cur_var_refs, JSStackFrame* sf, BOOL is_eval, JSModuleDef* m);
JSValue js_atof(JSContext* ctx, const char* str, const char** pp, int radix, int flags);
int to_digit(int c);
int JS_DefineAutoInitProperty(JSContext* ctx, JSValueConst this_obj, JSAtom prop, JSAutoInitIDEnum id, void* opaque, int flags);
int js_update_property_flags(JSContext* ctx, JSObject* p, JSShapeProperty** pprs, int flags);
int __exception JS_GetOwnPropertyNamesInternal(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen, JSObject* p, int flags);
JSValue js_async_function_call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_obj, int argc, JSValueConst* argv, int flags);
JSValue JS_ConcatString1(JSContext* ctx, const JSString* p1, const JSString* p2);
JSValue string_buffer_end(StringBuffer* s);
int string_buffer_write8(StringBuffer* s, const uint8_t* p, int len);
int string_buffer_putc(StringBuffer* s, uint32_t c);
int string_buffer_putc8(StringBuffer* s, uint32_t c);
void string_buffer_free(StringBuffer* s);
int string_buffer_init(JSContext* ctx, StringBuffer* s, int size);

enum {
    TOK_NUMBER = -128,
    TOK_STRING,
    TOK_TEMPLATE,
    TOK_IDENT,
    TOK_REGEXP,
    TOK_MUL_ASSIGN,
    TOK_DIV_ASSIGN,
    TOK_MOD_ASSIGN,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_SHL_ASSIGN,
    TOK_SAR_ASSIGN,
    TOK_SHR_ASSIGN,
    TOK_AND_ASSIGN,
    TOK_XOR_ASSIGN,
    TOK_OR_ASSIGN,
    TOK_POW_ASSIGN,
    TOK_LAND_ASSIGN,
    TOK_LOR_ASSIGN,
    TOK_DOUBLE_QUESTION_MARK_ASSIGN,
    TOK_DEC,
    TOK_INC,
    TOK_SHL,
    TOK_SAR,
    TOK_SHR,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,
    TOK_EQ,
    TOK_STRICT_EQ,
    TOK_NEQ,
    TOK_STRICT_NEQ,
    TOK_LAND,
    TOK_LOR,
    TOK_POW,
    TOK_ARROW,
    TOK_ELLIPSIS,
    TOK_DOUBLE_QUESTION_MARK,
    TOK_QUESTION_MARK_DOT,
    TOK_ERROR,
    TOK_PRIVATE_NAME,
    TOK_EOF,
    TOK_NULL,
    TOK_FALSE,
    TOK_TRUE,
    TOK_IF,
    TOK_ELSE,
    TOK_RETURN,
    TOK_VAR,
    TOK_THIS,
    TOK_DELETE,
    TOK_VOID,
    TOK_TYPEOF,
    TOK_NEW,
    TOK_IN,
    TOK_INSTANCEOF,
    TOK_DO,
    TOK_WHILE,
    TOK_FOR,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_THROW,
    TOK_TRY,
    TOK_CATCH,
    TOK_FINALLY,
    TOK_FUNCTION,
    TOK_DEBUGGER,
    TOK_WITH,
    TOK_CLASS,
    TOK_CONST,
    TOK_ENUM,
    TOK_EXPORT,
    TOK_EXTENDS,
    TOK_IMPORT,
    TOK_SUPER,
    TOK_IMPLEMENTS,
    TOK_INTERFACE,
    TOK_LET,
    TOK_PACKAGE,
    TOK_PRIVATE,
    TOK_PROTECTED,
    TOK_PUBLIC,
    TOK_STATIC,
    TOK_YIELD,
    TOK_AWAIT,
    TOK_OF,
};

#define TOK_FIRST_KEYWORD TOK_NULL
#define TOK_LAST_KEYWORD TOK_AWAIT
__attribute__((format(printf, 2, 3))) int js_parse_error(JSParseState* s, const char* fmt, ...);
__exception int json_next_token(JSParseState* s);
JSModuleDef* js_new_module_def(JSContext* ctx, JSAtom name);
JSValue JS_NewModuleValue(JSContext* ctx, JSModuleDef* m);

JSValue __JS_EvalInternal(JSContext* ctx, JSValueConst this_obj, const char* input, size_t input_len, const char* filename, int flags, int scope_idx);
void js_parse_init(JSContext* ctx, JSParseState* s, const char* input, size_t input_len, const char* filename);
void free_token(JSParseState* s, JSToken* token);

int check_function(JSContext* ctx, JSValueConst obj);
int js_deep_equals(JSContext* ctx, JSValueConst a, JSValueConst b, int depth);
JSValue js_array_at(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_with(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_concat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_every(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int special);
JSValue js_array_toSorted(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
#define special_every 0
#define special_some 1
#define special_forEach 2
#define special_map 3
#define special_filter 4
#define special_TA 8
typedef struct JSIteratorWrapData {
    JSValue wrapped_iter;
    JSValue wrapped_next;
} JSIteratorWrapData;

JSValue JS_GetOwnPropertyNames2(JSContext* ctx, JSValueConst obj1, int flags, int kind);
JSValue JS_NewCConstructor(JSContext* ctx, int class_id, const char* name, JSCFunction* func, int length, JSCFunctionEnum cproto, int magic, JSValueConst parent_ctor, const JSCFunctionListEntry* ctor_fields, int n_ctor_fields, const JSCFunctionListEntry* proto_fields, int n_proto_fields, int flags);
JSValue JS_NewObjectProtoList(JSContext* ctx, JSValueConst proto, const JSCFunctionListEntry* fields, int n_fields);
int JS_SetConstructor2(JSContext* ctx, JSValueConst func_obj, JSValueConst proto, int proto_flags, int ctor_flags);
JSValue JS_SpeciesConstructor(JSContext* ctx, JSValueConst obj, JSValueConst defaultConstructor);
int check_exception_free(JSContext* ctx, JSValue obj);
BOOL is_be(void);
JSValue js_aggregate_error_constructor(JSContext* ctx, JSValueConst errors);
JSValue js_array_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_array_copyWithin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_fill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_find(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int mode);
JSValue js_array_flatten(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int map);
JSValue js_array_includes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_indexOf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_join(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int toLocaleString);
JSValue js_array_lastIndexOf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_pop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int shift);
JSValue js_array_push(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int unshift);
JSValue js_array_reduce(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int special);
JSValue js_array_reverse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_slice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_sort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_splice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_toReversed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_toSpliced(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_toString(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_error_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic);
int js_from_fast_probe(JSContext* ctx, JSValueConst items, JSValueConst iter, BOOL need_iter_close, JSObject** pp, uint32_t* pcount, BOOL* pis_ta);
JSValue js_function_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic);
JSValue js_function_proto(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_get_this(JSContext* ctx, JSValueConst this_val);
JSValue js_global_eval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_global_isFinite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_global_isNaN(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
int js_obj_to_desc(JSContext* ctx, JSPropertyDescriptor* d, JSValueConst desc);
JSValue js_object_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_object_defineProperty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue js_object_getOwnPropertyDescriptor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue js_object_getPrototypeOf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue js_object_isExtensible(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int reflect);
JSValue js_object_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int kind);
JSValue js_object_preventExtensions(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int reflect);
JSValue js_object_seal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int freeze_flag);
#define special_reduceRight 1
#define special_reduce 0
#define JS_NEW_CTOR_READONLY (1 << 3)
#define JS_NEW_CTOR_PROTO_EXIST (1 << 2)
#define JS_NEW_CTOR_PROTO_CLASS (1 << 1)
#define JS_NEW_CTOR_NO_GLOBAL (1 << 0)

enum {
    ArrayFind,
    ArrayFindIndex,
    ArrayFindLast,
    ArrayFindLastIndex,
};
typedef struct JSArrayIteratorData {
    JSValue obj;
    JSIteratorKindEnum kind;
    uint32_t idx;
} JSArrayIteratorData;

#define GEN_MAGIC_NEXT 0
#define GEN_MAGIC_RETURN 1
BOOL JS_AtomIsString(JSContext* ctx, JSAtom v);
#define JS_BACKTRACE_FLAG_SKIP_FIRST_LEVEL (1 << 0)
JSValue JS_CompactBigInt(JSContext* ctx, JSBigInt* p);
JSValue JS_ConcatString(JSContext* ctx, JSValue op1, JSValue op2);
JSValue JS_ConcatString3(JSContext* ctx, const char* str1, JSValue str2, const char* str3);
__exception int JS_CopyDataProperties(JSContext* ctx, JSValueConst target, JSValueConst source, JSValueConst excluded, BOOL setprop);
int JS_CreateDataPropertyUint32(JSContext* ctx, JSValueConst this_obj, int64_t idx, JSValue val, int flags);
int JS_DefinePropertyValueInt64(JSContext* ctx, JSValueConst this_obj, int64_t idx, JSValue val, int flags);
int JS_DefinePropertyValueValue(JSContext* ctx, JSValueConst this_obj, JSValue prop, JSValue val, int flags);
int JS_DeletePropertyInt64(JSContext* ctx, JSValueConst obj, int64_t idx, int flags);
JSValueConst JS_GetActiveFunction(JSContext* ctx);
JSContext* JS_GetFunctionRealm(JSContext* ctx, JSValueConst func_obj);
JSValue JS_GetIterator(JSContext* ctx, JSValueConst obj, BOOL is_async);
JSValue JS_GetIterator2(JSContext* ctx, JSValueConst obj, JSValueConst method);
JSValue JS_GetPropertyInt64(JSContext* ctx, JSValueConst obj, int64_t idx);
JSValue JS_GetPropertyValue(JSContext* ctx, JSValueConst this_obj, JSValue prop);
JSValue JS_GetPrototypeFree(JSContext* ctx, JSValue obj);
BOOL JS_IsCFunction(JSContext* ctx, JSValueConst val, JSCFunction* func, int magic);
BOOL JS_IsEmptyString(JSValueConst v);
int JS_IteratorClose(JSContext* ctx, JSValueConst enum_obj, BOOL is_exception_pending);
JSValue JS_IteratorNext(JSContext* ctx, JSValueConst enum_obj, JSValueConst method, int argc, JSValueConst* argv, BOOL* pdone);
JSValue JS_IteratorNext2(JSContext* ctx, JSValueConst enum_obj, JSValueConst method, int argc, JSValueConst* argv, int* pdone);
JSValue JS_NewCFunction3(JSContext* ctx, JSCFunction* func, const char* name, int length, JSCFunctionEnum cproto, int magic, JSValueConst proto_val, int n_fields);
JSValue JS_NewObjectProtoClassAlloc(JSContext* ctx, JSValueConst proto_val, JSClassID class_id, int n_alloc_props);
int JS_OrdinaryIsInstanceOf(JSContext* ctx, JSValueConst val, JSValueConst obj);
int JS_SetObjectData(JSContext* ctx, JSValueConst obj, JSValue val);
int JS_SetPrototypeInternal(JSContext* ctx, JSValueConst obj, JSValueConst proto_val, BOOL throw_flag);
JSValue JS_ThrowTypeErrorNotAConstructor(JSContext* ctx, JSValueConst func_obj);
JSValue JS_ThrowTypeErrorNotAnObject(JSContext* ctx);
int JS_ToInt32Sat(JSContext* ctx, int* pres, JSValueConst val);
int JS_ToInt64Clamp(JSContext* ctx, int64_t* pres, JSValueConst val, int64_t min, int64_t max, int64_t neg_offset);
int JS_ToInt64Sat(JSContext* ctx, int64_t* pres, JSValueConst val);
__exception int JS_ToLengthFree(JSContext* ctx, int64_t* plen, JSValue val);
JSValue JS_ToLocaleStringFree(JSContext* ctx, JSValue val);
JSValue JS_ToStringCheckObject(JSContext* ctx, JSValueConst val);
int JS_ToUint32Free(JSContext* ctx, uint32_t* pres, JSValue val);
int JS_TryGetPropertyInt64(JSContext* ctx, JSValueConst obj, int64_t idx, JSValue* pval);
#define MAX_SAFE_INTEGER (((int64_t)1 << 53) - 1)
uint32_t __JS_AtomToUInt32(JSAtom atom);
BOOL can_extend_fast_array(JSObject* p);
int JS_ToInt32Clamp(JSContext* ctx, int* pres, JSValueConst val, int min, int max, int min_offset);
int compact_properties(JSContext* ctx, JSObject* p);
int delete_property(JSContext* ctx, JSObject* p, JSAtom atom);
int expand_fast_array(JSContext* ctx, JSObject* p, uint32_t new_len);
JSShapeProperty* find_own_property(JSProperty** ppr, JSObject* p, JSAtom atom);
extern const uint16_t func_kind_to_class_id[];
int get_leb128(uint32_t* pval, const uint8_t* buf, const uint8_t* buf_end);
JSShapeProperty* get_shape_prop(JSShape* sh);
int get_sleb128(int32_t* pval, const uint8_t* buf, const uint8_t* buf_end);
JSString* js_alloc_string(JSContext* ctx, int max_len, int is_wide_char);
JSValue js_allocate_fast_array(JSContext* ctx, int64_t len);
JSValue js_array_iterator_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, BOOL* pdone, int magic);
JSBigInt* js_bigint_new(JSContext* ctx, int len);
JSBigInt* js_bigint_set_short(JSBigIntBuf* buf, JSValueConst val);
JSValue js_create_array(JSContext* ctx, int len, JSValueConst* tab);
JSValue js_create_from_ctor(JSContext* ctx, JSValueConst ctor, int class_id);
void js_free_string(JSRuntime* rt, JSString* str);
JSValue js_function_proto_fileName(JSContext* ctx, JSValueConst this_val);
JSValue js_function_proto_lineNumber(JSContext* ctx, JSValueConst this_val, int is_col);
JSValue js_new_string8(JSContext* ctx, const char* buf);
__exception int js_poll_interrupts(JSContext* ctx);
const uint16_t* js_str_data16(const JSString* p);
const uint8_t* js_str_data8(const JSString* p);
void set_cycle_flag(JSContext* ctx, JSValueConst obj);
int string_buffer_concat(StringBuffer* s, const JSString* p, uint32_t from, uint32_t to);
int string_buffer_concat_value(StringBuffer* s, JSValueConst v);
int string_buffer_concat_value_free(StringBuffer* s, JSValue v);
int string_buffer_puts8(StringBuffer* s, const char* str);

extern const JSCFunctionListEntry js_array_funcs[5];
extern const JSCFunctionListEntry js_error_funcs[1];
extern const JSCFunctionListEntry js_error_proto_funcs[3];
extern const JSCFunctionListEntry js_function_ext_funcs[34];
extern const JSCFunctionListEntry js_function_proto_funcs[8];
extern const JSCFunctionListEntry js_function_static_ext_funcs[11];
extern const JSCFunctionListEntry js_iterator_wrap_proto_funcs[2];
extern const JSCFunctionListEntry js_native_error_proto_funcs[18];
extern const JSCFunctionListEntry js_object_ext_funcs[73];
extern const JSCFunctionListEntry js_object_funcs[23];
extern const JSCFunctionListEntry js_object_proto_funcs[11];

JSValue js_create_array_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue js_create_typed_array_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
int js_from_fast_probe(JSContext* ctx, JSValueConst items, JSValueConst iter, BOOL need_iter_close, JSObject** pp, uint32_t* pcount, BOOL* pis_ta);

int64_t date_now(void);

JSValue js_typed_array___speciesCreate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_array_every(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int special);

#endif


