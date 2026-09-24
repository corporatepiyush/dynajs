/*
 * DynaJS Javascript Engine
 *
 * Copyright (c) 2017-2025 Fabrice Bellard
 * Copyright (c) 2017-2025 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
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

/* Hot-Math platform-libm dispatch (CONFIG_SYSTEMLIBM).  openlibm is linked
   as a static archive ahead of -lm, so plain symbol references bind to its
   software copies; on Apple/arm64 its fdlibm-derived sqrt costs ~156ns
   while the platform's hardware fsqrt costs ~1ns.  For the functions where
   a 12.45M-case differential proved the two implementations BIT-IDENTICAL
   (sqrt, fabs, floor, ceil, trunc — IEEE-correctly-rounded or exact in
   both), Math.* resolves through these dlsym(RTLD_NEXT) wrappers instead.
   RTLD_NEXT from the main executable skips the in-image openlibm copies
   and reaches libSystem; a failed lookup falls back to the openlibm
   symbol, so the worst case is the old behavior, never wrong results.
   Math.pow / the ** operator stay on openlibm by design: the differential
   measured 104,576 one-ulp diffs in pow (and similar in atan2/hypot/tan/
   cbrt/exp/log/...), so cross-platform reproducibility is preserved for
   everything NOT swapped.  Default ON only on Darwin/arm64 hosts (see the
   Makefile); CONFIG_SYSTEMLIBM=n disables, =y forces (needs dlsym). */
#if defined(CONFIG_SYSTEMLIBM)
#include <dlfcn.h>
#define JS_SYSTEMLIBM_WRAPPER(name)                              \
    static double js_sys_##name(double a) {                      \
        static double (*fn)(double); /* benign same-value race   \
                                        (Workers hold their own  \
                                        runtimes) */             \
        if (unlikely(!fn)) {                                     \
            fn = (double (*)(double))dlsym(RTLD_NEXT, #name);    \
            if (!fn)                                             \
                fn = name;                                       \
        }                                                        \
        return fn(a);                                            \
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
#include "dyna-simd-kernels.h"  /* shared multi-ISA SIMD dispatch table */
#include "core/dyn-prng.h"      /* dyn_os_entropy: atom/shape hash salts */

#define OPTIMIZE         1
#define SHORT_OPCODES    1
/* Fused compare+branch superinstructions (resolve_labels folds a relational
   op immediately followed by OP_if_false into one dispatch). Gate exists so a
   #define-off oracle build can prove the transform is output-identical. */
#ifndef CONFIG_FUSED_CMP
#define CONFIG_FUSED_CMP 1
#endif
/* Fused two-local arithmetic superinstructions (resolve_labels folds
   `get_loc A; get_loc B; <mul|add|sub>` into a single [OP_ext][op2][a][b] op
   that reads both locals directly — 6 bytes / 1 dispatch replacing 7 bytes /
   3 dispatches). Gate exists so a #define-off oracle build proves it output-
   identical. Bank-2 ARITH shard; see dyna-opcode2.h. */
#ifndef CONFIG_FUSED_ARITH
#define CONFIG_FUSED_ARITH 1
#endif
/* Fused method-call superinstructions (resolve_labels folds the BACKWARD
   window `get_field2 <atom>; [<one hot arg op>]` in front of OP_call_method
   into a single bank-2 op — the argument sits BETWEEN the pair, so the fold
 anchors at the call site; audit ). Gate exists so a #define-off oracle
   build proves it output-identical. Bank-2 CALL shard; see dyna-opcode2.h. */
#ifndef CONFIG_FUSED_METHOD
#define CONFIG_FUSED_METHOD 1
#endif
/* Dense-integer switch jump table: a `switch` whose cases are enough densely
   packed constant integers gets an OP_switch jump-table prefix (O(1) dispatch)
   before the unchanged linear compare chain (correct fallback). Gate exists so
   a #define-off oracle build proves the transform output-identical. */
#ifndef CONFIG_JUMP_SWITCH
#define CONFIG_JUMP_SWITCH 1
#endif
#if defined(__EMSCRIPTEN__) || defined(FORCE_SWITCH_DISPATCH)
#define DIRECT_DISPATCH  0
#else
#define DIRECT_DISPATCH  1
#endif

#if defined(__APPLE__)
#define MALLOC_OVERHEAD  0
#else
#define MALLOC_OVERHEAD  8
#endif

#if !defined(_WIN32)
/* define it if printf uses the RNDN rounding mode instead of RNDNA */
#define CONFIG_PRINTF_RNDN
#endif

/* define to include Atomics.* operations which depend on the OS
   threads */
#if !defined(__EMSCRIPTEN__)
#define CONFIG_ATOMICS
#endif

#if !defined(__EMSCRIPTEN__)
/* enable stack limitation */
#define CONFIG_STACK_CHECK
#endif


/* dump object free */
//#define DUMP_FREE
//#define DUMP_CLOSURE
/* dump the bytecode of the compiled functions: combination of bits
   1: dump pass 3 final byte code
   2: dump pass 2 code
   4: dump pass 1 code
   8: dump stdlib functions
  16: dump bytecode in hex
  32: dump line number table
  64: dump compute_stack_size
 */
//#define DUMP_BYTECODE  (1)
/* dump the occurence of the automatic GC */
//#define DUMP_GC
/* dump objects freed by the garbage collector */
//#define DUMP_GC_FREE
/* dump objects leaking when freeing the runtime */
//#define DUMP_LEAKS  1
/* dump memory usage before running the garbage collector */
//#define DUMP_MEM
//#define DUMP_OBJECTS    /* dump objects in JS_FreeContext */
//#define DUMP_ATOMS      /* dump atoms in JS_FreeContext */
//#define DUMP_SHAPES     /* dump shapes in JS_FreeContext */
//#define DUMP_MODULE_RESOLVE
//#define DUMP_MODULE_EXEC
//#define DUMP_PROMISE
//#define DUMP_READ_OBJECT
//#define DUMP_ROPE_REBALANCE
/* add asm labels to each opcode so that it is easier to see the generated code */
//#define OPCODE_ASM_LABEL

/* test the GC by forcing it before each object allocation */
//#define FORCE_GC_AT_MALLOC

#ifdef CONFIG_ATOMICS
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#endif

enum {
    /* classid tag        */    /* union usage   | properties */
    JS_CLASS_OBJECT = 1,        /* must be first */
    JS_CLASS_ARRAY,             /* u.array       | length */
    JS_CLASS_ERROR,
    JS_CLASS_NUMBER,            /* u.object_data */
    JS_CLASS_STRING,            /* u.object_data */
    JS_CLASS_BOOLEAN,           /* u.object_data */
    JS_CLASS_SYMBOL,            /* u.object_data */
    JS_CLASS_ARGUMENTS,         /* u.array       | length */
    JS_CLASS_MAPPED_ARGUMENTS,  /* u.array       | length */
    JS_CLASS_DATE,              /* u.object_data */
    JS_CLASS_MODULE_NS,
    JS_CLASS_C_FUNCTION,        /* u.cfunc */
    JS_CLASS_BYTECODE_FUNCTION, /* u.func */
    JS_CLASS_BOUND_FUNCTION,    /* u.bound_function */
    JS_CLASS_C_FUNCTION_DATA,   /* u.c_function_data_record */
    JS_CLASS_GENERATOR_FUNCTION, /* u.func */
    JS_CLASS_FOR_IN_ITERATOR,   /* u.for_in_iterator */
    JS_CLASS_REGEXP,            /* u.regexp */
    JS_CLASS_ARRAY_BUFFER,      /* u.array_buffer */
    JS_CLASS_SHARED_ARRAY_BUFFER, /* u.array_buffer */
    JS_CLASS_UINT8C_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT8_ARRAY,        /* u.array (typed_array) */
    JS_CLASS_UINT8_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_INT16_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT16_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT32_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT32_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_BIG_INT64_ARRAY,   /* u.array (typed_array) */
    JS_CLASS_BIG_UINT64_ARRAY,  /* u.array (typed_array) */
    JS_CLASS_FLOAT16_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_FLOAT32_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_FLOAT64_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_DATAVIEW,          /* u.typed_array */
    JS_CLASS_BIG_INT,           /* u.object_data */
    JS_CLASS_MAP,               /* u.map_state */
    JS_CLASS_SET,               /* u.map_state */
    JS_CLASS_WEAKMAP,           /* u.map_state */
    JS_CLASS_WEAKSET,           /* u.map_state */
    JS_CLASS_ITERATOR,          /* u.map_iterator_data */
    JS_CLASS_ITERATOR_CONCAT,   /* u.iterator_concat_data */
    JS_CLASS_ITERATOR_ZIP,      /* u.iterator_zip_data */
    JS_CLASS_ITERATOR_HELPER,   /* u.iterator_helper_data */
    JS_CLASS_ITERATOR_WRAP,     /* u.iterator_wrap_data */
    JS_CLASS_MAP_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_SET_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_ARRAY_ITERATOR,    /* u.array_iterator_data */
    JS_CLASS_STRING_ITERATOR,   /* u.array_iterator_data */
    JS_CLASS_REGEXP_STRING_ITERATOR,   /* u.regexp_string_iterator_data */
    JS_CLASS_GENERATOR,         /* u.generator_data */
    JS_CLASS_GLOBAL_OBJECT,     /* u.global_object */
    JS_CLASS_RAWJSON,
    JS_CLASS_PROXY,             /* u.proxy_data */
    JS_CLASS_PROMISE,           /* u.promise_data */
    JS_CLASS_PROMISE_RESOLVE_FUNCTION,  /* u.promise_function_data */
    JS_CLASS_PROMISE_REJECT_FUNCTION,   /* u.promise_function_data */
    JS_CLASS_ASYNC_FUNCTION,            /* u.func */
    JS_CLASS_ASYNC_FUNCTION_RESOLVE,    /* u.async_function_data */
    JS_CLASS_ASYNC_FUNCTION_REJECT,     /* u.async_function_data */
    JS_CLASS_ASYNC_FROM_SYNC_ITERATOR,  /* u.async_from_sync_iterator_data */
    JS_CLASS_ASYNC_GENERATOR_FUNCTION,  /* u.func */
    JS_CLASS_ASYNC_GENERATOR,   /* u.async_generator_data */
    JS_CLASS_WEAK_REF,
    JS_CLASS_FINALIZATION_REGISTRY,
    JS_CLASS_DISPOSABLE_STACK,       /* u.opaque: JSDisposableStackData */
    JS_CLASS_ASYNC_DISPOSABLE_STACK, /* u.opaque: JSDisposableStackData */

    JS_CLASS_INIT_COUNT, /* last entry for predefined classes */
};

/* number of typed array types */
#define JS_TYPED_ARRAY_COUNT  (JS_CLASS_FLOAT64_ARRAY - JS_CLASS_UINT8C_ARRAY + 1)
static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT];
#define typed_array_size_log2(classid)  (typed_array_size_log2[(classid)- JS_CLASS_UINT8C_ARRAY])

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

    JS_NATIVE_ERROR_COUNT, /* number of different NativeError objects */
} JSErrorEnum;

/* the variable and scope indexes must fit on 16 bits. The (-1) and
   ARG_SCOPE_END values are reserved. */
#define JS_MAX_LOCAL_VARS 65534
#define JS_STACK_SIZE_MAX 65534
#define JS_STRING_LEN_MAX ((1 << 30) - 1)

/* substrings at least this long become lazy slices instead of eager copies.
   Overridable at runtime for testing via DYNA_SLICE_MIN_LEN (read once at
   startup; clamped to [1, JS_STRING_LEN_MAX]). */
#define JS_SLICE_MIN_LEN_DEFAULT 64

/* strings <= this length are not concatenated using ropes. if too
   small, the rope memory overhead becomes high. */
#define JS_STRING_ROPE_SHORT_LEN  512
/* specific threshold for initial rope use */
#define JS_STRING_ROPE_SHORT2_LEN 8192
/* rope depth at which we rebalance */
#define JS_STRING_ROPE_MAX_DEPTH 60

#define __exception __attribute__((warn_unused_result))

typedef struct JSShape JSShape;
typedef struct JSString JSString;
typedef struct JSString JSAtomStruct;
typedef struct JSObject JSObject;

#define JS_VALUE_GET_OBJ(v) ((JSObject *)JS_VALUE_GET_PTR(v))
#define JS_VALUE_GET_STRING(v) ((JSString *)JS_VALUE_GET_PTR(v))
#define JS_VALUE_GET_STRING_ROPE(v) ((JSStringRope *)JS_VALUE_GET_PTR(v))

typedef enum {
    JS_GC_PHASE_NONE,
    JS_GC_PHASE_DECREF,
    JS_GC_PHASE_REMOVE_CYCLES,
} JSGCPhaseEnum;

typedef enum OPCodeEnum OPCodeEnum;

/* JS malloc */

#define JS_MALLOC_ALIGN 8
#define JS_MALLOC_ARENA_SIZE 4096
#define JS_MALLOC_BLOCK_SIZE_COUNT 31
#define JS_MALLOC_MIN_SMALL_SIZE 16
#define JS_MALLOC_MAX_SMALL_SIZE 512
#if defined(__SANITIZE_ADDRESS__)
/* use the host malloc() for all allocations */
#define JS_MALLOC_LARGE_BLOCKS_ONLY 1
#else
#define JS_MALLOC_LARGE_BLOCKS_ONLY 0
#endif

/* allow iteration among the allocated blocks. Currently not used. May
   be used to suppress the memory overhead of JSGCObjectHeader */
//#define JS_MALLOC_USE_ITER

#define FREE_NIL 0xffff

/* 8 byte header */
/* Notes: 
   - the header is necessary at least to recover a pointer to
     JSMallocArena because we don't want to enforce a page
     alignment on the system malloc().
   - could store the block offset instead of (block_idx,
   block_size_idx), but it would require a division to recover the block
   index.
*/
typedef struct JSMallocBlockHeader {
    union {
        uint16_t block_idx; /* FREE_NIL if large block */
        uint16_t free_next; /* FREE_NIL if none */
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
    uint16_t n_used_blocks; /* number of allocated blocks */
    uint16_t n_blocks; /* total number of blocks */
    uint16_t first_free_block; /* FREE_NIL if none */
#ifdef JS_MALLOC_USE_ITER    
    /* bit set to 1 for allocated block */
    uint32_t bitmap[((JS_MALLOC_ARENA_SIZE / JS_MALLOC_MIN_SMALL_SIZE) + 31) / 32]; 
#endif
    /* n_blocks memory blocks of identical size */
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t blocks[];
} JSMallocArena;

typedef struct {
    struct list_head arena_list[JS_MALLOC_BLOCK_SIZE_COUNT]; /* list of JSMallocArena.link (all arenas) */
    struct list_head free_arena_list[JS_MALLOC_BLOCK_SIZE_COUNT]; /* list of JSMallocArena.free_link (arenas where n_used_blocks < n_blocks) */
    /* number of fully-empty arenas retained per size class as spares (hysteresis:
       avoids malloc+freelist-init churn under alloc-one/free-one workloads). */
    uint16_t n_empty_arenas[JS_MALLOC_BLOCK_SIZE_COUNT];
    /* Runtime switch: set when DYNAJS_MALLOC_POOLS is "0"/"off"/"no" at
       runtime creation. The small-block arenas then serve NOTHING: every
       js_malloc request falls through to js_malloc_large and reaches the
       host allocator, exactly like the ASan build's compile-time
       JS_MALLOC_LARGE_BLOCKS_ONLY. The point is ACCOUNTABILITY: an
       allocation gate (tests/run_base58_alloc.sh under -T) or a malloc
       interposer must see every engine allocation -- including the pooled
       sizes an arena free-list would otherwise serve invisibly. Default
       (unset) keeps the arenas on; behavior and speed are unchanged. */
    uint8_t no_pools;
#ifdef JS_MALLOC_USE_ITER
    struct list_head large_block_list; /* list of JSMallocLargeBlockHeader.link */
#endif
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t zero_size_block[sizeof(JSMallocBlockHeader)];

    /* callbacks to the host malloc */
    JSMallocFunctions mf;
    JSMallocState malloc_state;
} JSMallocContext;

/* end JS Malloc */

/* CONFIG_OBJ_POOL recycle-cache sizing (pools live in JSRuntime below,
   push/pop helpers in src/object/shapes_objects_gc.inc.c, teardown drain
   in src/mm/js_malloc.inc.c next to the date-cache drain) */
#define JS_OBJ_POOL_MAX_OBJECTS 1024  /* pooled JSObject headers, LIFO cap */
#define JS_ARRAY_POOL_NBUCKETS 11     /* capacities 1..1024 slots, powers of two */
#define JS_ARRAY_POOL_MAX_BUCKET (1 << (JS_ARRAY_POOL_NBUCKETS - 1))
#define JS_ARRAY_POOL_MAX_SLOTS 65536 /* total pooled JSValue slots */

struct JSDateFieldsCache; /* full definition below (before JSObject) */

struct JSRuntime {
    JSMallocContext malloc_ctx;
    const char *rt_info;

    /* freelist of recycled per-Date field caches (JSDateFieldsCache):
       short-lived Dates would otherwise malloc/free 112 bytes on their
       first field getter -- pop/push turns that into two pointer ops */
    struct JSDateFieldsCache *date_cache_free_list;

#ifdef CONFIG_OBJ_POOL
    /* recycle cache for JSObject header structs and for fast-array
       JS_CLASS_ARRAY values buffers. Every JSObject allocation is
       sizeof(JSObject) (the class payload lives in the u union), so one
       LIFO serves all classes; the freelist link is stashed in
       header.link.next, which add_gc_object() rewrites on reuse. Values
       buffers are pooled per capacity bucket; the freelist link is
       stashed in values[0]. Pooled blocks stay "live" to the arena
       allocator (their JSMallocBlockHeader is intact), so eviction and
       the JS_FreeRuntime drain release them with js_free_rt. */
    struct JSObject *obj_pool_free_list;
    int obj_pool_count;
    void *array_pool_free_list[JS_ARRAY_POOL_NBUCKETS];
    int array_pool_count;
    int array_pool_slots;
#endif

#ifdef CONFIG_NURSERY_PROBE
    /* diagnostic bump arena (see Makefile CONFIG_NURSERY_PROBE note):
       singly-linked 2MB system-malloc'd chunks; allocations are never
       returned (free = no-op accounting), so a single bump pointer per
       chunk suffices. State is drained at JS_FreeRuntime. */
    uint8_t *nursery_ptr, *nursery_end;
    void *nursery_chunks;
    uint64_t nursery_alloc_count, nursery_alloc_bytes;
#endif

    int atom_hash_size; /* power of two */
    int atom_count;
    int atom_size;
    int atom_count_resize; /* resize hash table at this count */
    uint32_t *atom_hash;
    JSAtomStruct **atom_array;
    int atom_free_index; /* 0 = none */

    int class_count;    /* size of class_array */
    JSClass *class_array;

    struct list_head context_list; /* list of JSContext.link */
    /* list of JSGCObjectHeader.link. List of allocated GC objects (used
       by the garbage collector) */
    struct list_head gc_obj_list;
    /* list of JSGCObjectHeader.link. Used during JS_FreeValueRT() */
    struct list_head gc_zero_ref_count_list;
    struct list_head tmp_obj_list; /* used during GC */
    JSGCPhaseEnum gc_phase : 8;
    size_t malloc_gc_threshold;
    struct list_head weakref_list; /* list of JSWeakRefHeader.link */
#ifdef DUMP_LEAKS
    struct list_head string_list; /* list of JSString.link */
#endif
    /* stack limitation */
    uintptr_t stack_size; /* in bytes, 0 if no limit */
    uintptr_t stack_top;
    uintptr_t stack_limit; /* lower stack limit */

    JSValue current_exception;
    /* true if the current exception cannot be catched */
    BOOL current_exception_is_uncatchable : 8;
    /* true if inside an out of memory error, to avoid recursing */
    BOOL in_out_of_memory : 8;

    struct JSStackFrame *current_stack_frame;

    JSInterruptHandler *interrupt_handler;
    void *interrupt_opaque;

    JSHostPromiseRejectionTracker *host_promise_rejection_tracker;
    void *host_promise_rejection_tracker_opaque;

    struct list_head job_list; /* list of JSJobEntry.link */

    /* list of JSShutdownSweepEntry.link: the shutdown-sweep registry
       (JS_AddShutdownSweep). Run at the LAST JS_FreeContext teardown --
       the hook that fails C-pinned parked operations while JS is legal. */
    struct list_head shutdown_sweeps;

    /* list of JSShutdownDeferEntry.link: dead shells freed at the very end
       of JS_FreeRuntime (JS_ShutdownDeferFree). */
    struct list_head shutdown_deferred;

    JSModuleNormalizeFunc *module_normalize_func;
    BOOL module_loader_has_attr;
    union {
        JSModuleLoaderFunc *module_loader_func;
        JSModuleLoaderFunc2 *module_loader_func2;
    } u;
    JSModuleCheckSupportedImportAttributes *module_check_attrs;
    void *module_loader_opaque;
    /* timestamp for internal use in module evaluation */
    int64_t module_async_evaluation_next_timestamp;

    BOOL can_block : 8; /* TRUE if Atomics.wait can block */
    /* used to allocate, free and clone SharedArrayBuffers */
    JSSharedArrayBufferFunctions sab_funcs;
    /* see JS_SetStripInfo() */
    uint8_t strip_flags;
    
    /* Shape hash table */
    int shape_hash_bits;
    int shape_hash_size;
    int shape_hash_count; /* number of hashed shapes */
    JSShape **shape_hash;
    /* Per-runtime random salts (audit 1.5, 2.3b): mixed into atom hashing and
       shape hashing so precomputed collision sets cannot be replayed across
       runtimes. Seeded from kernel entropy in JS_InitAtoms() and
       init_shape_hash(); zero only before those run. */
    uint64_t atom_hash_seed;
    uint64_t shape_hash_seed;
    /* lazily built "typeof" result strings, keyed by the static result atom;
       tag != JS_TAG_STRING means not built yet */
    JSValue typeof_strings[8];
    /* lazily built latin1 single-character strings (0..255), used by
       js_new_string_char(). NULL means not built yet. Each entry holds ONE
       reference for the cache itself; a char that gets interned as an atom
       (JS_NewAtomStr donates the caller's reference) ends up owned by the
       atom table instead -- those entries are skipped at teardown and freed
       with the atoms. */
    JSString *latin1_char_cache[256];
    void *user_opaque;
};

struct JSClass {
    uint32_t class_id; /* 0 means free entry */
    JSAtom class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    JSClassCall *call;
    /* pointers for exotic behavior, can be NULL if none are present */
    const JSClassExoticMethods *exotic;
};

#define JS_MODE_STRICT (1 << 0)
#define JS_MODE_ASYNC  (1 << 2) /* async function */
#define JS_MODE_BACKTRACE_BARRIER (1 << 3) /* stop backtrace before this frame */

typedef struct JSStackFrame {
    struct JSStackFrame *prev_frame; /* NULL if first stack frame */
    JSValue cur_func; /* current function, JS_UNDEFINED if the frame is detached */
    JSValue *arg_buf; /* arguments */
    JSValue *var_buf; /* variables */
    struct JSVarRef **var_refs; /* references to arguments or local variables */ 
    const uint8_t *cur_pc; /* only used in bytecode functions : PC of the
                        instruction after the call */
    int arg_count;
    int js_mode; /* not supported for C functions */
    /* only used in generators. Current stack pointer value. NULL if
       the function is running. */
    JSValue *cur_sp;
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

/* header for GC objects. GC objects are C data structures with a
   reference count that can reference other GC objects. JS Objects are
   a particular type of GC object. */
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
    JSGCObjectHeader header; /* must come first */
    uint8_t is_detached;
    uint8_t is_lexical; /* only used with global variables */
    uint8_t is_const; /* only used with global variables */
    JSValue *pvalue; /* pointer to the value, either on the stack or
                        to 'value' */
    union {
        JSValue value; /* used when is_detached = TRUE */
        struct {
            uint16_t var_ref_idx; /* index in JSStackFrame.var_refs[] */
            JSStackFrame *stack_frame;
        }; /* used when is_detached = FALSE */
    };
} JSVarRef;

/* bigint */

#if JS_LIMB_BITS == 32

typedef int32_t js_slimb_t;
typedef uint32_t js_limb_t;
typedef int64_t js_sdlimb_t;
typedef uint64_t js_dlimb_t;

#define JS_LIMB_DIGITS 9

#else

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;
typedef int64_t js_slimb_t;
typedef uint64_t js_limb_t;
typedef int128_t js_sdlimb_t;
typedef uint128_t js_dlimb_t;

#define JS_LIMB_DIGITS 19

#endif

typedef struct JSBigInt {
    uint32_t len; /* number of limbs, >= 1 */
    js_limb_t tab[]; /* two's complement representation, always
                        normalized so that 'len' is the minimum
                        possible length >= 1 */
} JSBigInt;

/* this bigint structure can hold a 64 bit integer */
typedef struct {
    js_limb_t big_int_buf[sizeof(JSBigInt) / sizeof(js_limb_t)]; /* for JSBigInt */
    /* must come just after */
    js_limb_t tab[(64 + JS_LIMB_BITS - 1) / JS_LIMB_BITS];
} JSBigIntBuf;
    
typedef enum {
    JS_AUTOINIT_ID_PROTOTYPE,
    JS_AUTOINIT_ID_MODULE_NS,
    JS_AUTOINIT_ID_PROP,
} JSAutoInitIDEnum;

/* must be large enough to have a negligible runtime cost and small
   enough to call the interrupt callback often. */
#define JS_INTERRUPT_COUNTER_INIT 10000

struct JSContext {
    JSGCObjectHeader header; /* must come first */
    JSRuntime *rt;
    struct list_head link;

    uint16_t binary_object_count;
    int binary_object_size;
    
    JSShape *array_shape;   /* initial shape for Array objects */
    JSShape *arguments_shape;  /* shape for arguments objects */
    JSShape *mapped_arguments_shape;  /* shape for mapped arguments objects */
    JSShape *regexp_shape;  /* shape for regexp objects */
    JSShape *regexp_result_shape;  /* shape for regexp result objects */
    JSShape *iterator_result_shape;  /* shape for {value, done} iterator result objects */

    JSValue *class_proto;
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

    JSValue global_obj; /* global object */
    JSValue global_var_obj; /* contains the global let/const definitions */

    uint64_t random_state;

    /* when the counter reaches zero, JSRutime.interrupt_handler is called */
    int interrupt_counter;

    struct list_head loaded_modules; /* list of JSModuleDef.link */

    /* if NULL, RegExp compilation is not supported */
    JSValue (*compile_regexp)(JSContext *ctx, JSValueConst pattern,
                              JSValueConst flags);
    /* if NULL, eval is not supported */
    JSValue (*eval_internal)(JSContext *ctx, JSValueConst this_obj,
                             const char *input, size_t input_len,
                             const char *filename, int flags, int scope_idx);
    void *user_opaque;
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

#define JS_ATOM_HASH_MASK  ((1 << 30) - 1)
#define JS_ATOM_HASH_PRIVATE JS_ATOM_HASH_MASK

/* Sliced strings (lazy substrings). A slice is a value-only JSString
   (never an atom) that borrows a contiguous char range of a flat parent
   string: character data lives at parent->u.strX + offset, length len. The
   slice holds one reference on 'parent', keeping it alive; 'parent' never
   references the slice, so no reference cycle can form. Slice-of-slice
   resolves to the root at creation time (offset adjusted), so 'parent' is
   always a flat JSString and structural depth is exactly 1. Since wave B,
   'parent' may also be an ATOM (string literals as runtime values ARE the
   interned JSString): the intern table is weak (an entry is unlinked as
   soon as its refcount hits zero), so the slice holds a strong reference
   to keep the atom's characters alive. Constant atoms (index <
   JS_ATOM_END) are the exception: JS_DupAtom/JS_FreeAtom are no-ops for
   them, so the slice takes no reference and marks that in its otherwise
   unused hash_next bit 0 ("parent ref taken"); the free paths
   (js_free_string / __JS_FreeValueRT) decrement only when the bit is set.

   Layout: the slice payload lives in a JS_SLICE_PREFIX_SIZE-byte block
   immediately BEFORE the JSString header (freshly malloc'd memory, so the
   effective-type rules are satisfied and the payload pointer is naturally
   aligned; see js_slice_payload()). This keeps sizeof(JSString) and every
   flat-string allocation size byte-identical to the pre-slice engine. */
typedef struct JSStringSlice {
    /* parent FIRST: the 16-byte prefix block is laid out as
       [parent @ +0][offset @ +8][refcount @ +12][JSString @ +16], where the
       refcount is the JSRefCountHeader the engine reads at header-4. */
    JSString *parent;  /* flat (is_slice == 0); value string or atom */
    uint32_t offset;   /* char offset of the slice inside 'parent' */
} JSStringSlice;

#define JS_SLICE_PREFIX_SIZE 16 /* >= sizeof(JSStringSlice), 8-aligned */

/* is_slice takes the top bit of the first word; len is capped at
   JS_STRING_LEN_MAX ((1<<30)-1), already the effective cap at every
   concat/buffer site (now enforced at the js_alloc_string_rt choke point).
   The hash/atom_type word is untouched so atom hashing/interning stay
   bit-identical to the pre-slice layout. */
struct JSString {
    uint32_t len : 30;
    uint32_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
    uint32_t is_slice : 1;     /* 1 = lazy substring; payload before header */
    /* for JS_ATOM_TYPE_SYMBOL: hash = weakref_count, atom_type = 3,
       for JS_ATOM_TYPE_PRIVATE: hash = JS_ATOM_HASH_PRIVATE, atom_type = 3
       XXX: could change encoding to have one more bit in hash */
    uint32_t hash : 30;
    uint8_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
    uint32_t hash_next; /* atom_index for JS_ATOM_TYPE_SYMBOL */
#ifdef DUMP_LEAKS
    struct list_head link; /* string list */
#endif
    union {
        uint8_t str8[0]; /* 8 bit strings will get an extra null terminator */
        uint16_t str16[0];
    } u;
};

#if defined(__GNUC__) && !defined(__cplusplus)
/* Guard the bitfield packing and the header size: every string allocation
   is sized off sizeof(JSString), so a silent layout change would corrupt
   the heap. sizeof(JSString) must stay exactly what the pre-slice engine
   had (3 x uint32_t on LP64). */
_Static_assert(sizeof(JSString) == 3 * sizeof(uint32_t),
               "JSString header layout changed");
_Static_assert(sizeof(JSStringSlice) <= JS_SLICE_PREFIX_SIZE,
               "slice payload no longer fits the prefix block");
_Static_assert((JS_SLICE_PREFIX_SIZE & (sizeof(void *) - 1)) == 0,
               "slice prefix must keep the header pointer aligned");
#endif

/* Accessor contract: js_slice_parent(p)->u.str8[offset..offset+len) is the
   window; there is NO NUL after str8[len] for a slice (flat narrow strings
   allocate +1 for the terminator, slice windows do not -- the byte after
   the window is the next parent character or one past the parent's end).
   Zero-copy consumers of character data must use JS_ToCStringLen2, which
   refuses the zero-copy path for slices and materializes a NUL-terminated
   copy (free with JS_FreeCString). See js_str_data8/js_str_data16 in
   src/mm/js_malloc.inc.c for the full accessor contract. */
#define js_slice_parent(p) (js_slice_payload(p)->parent)
#define js_slice_offset(p) (js_slice_payload(p)->offset)

typedef struct JSStringRope {
    uint32_t len;
    uint8_t is_wide_char; /* 0 = 8 bits, 1 = 16 bits characters */
    uint8_t depth; /* max depth of the rope tree */
    /* XXX: could reduce memory usage by using a direct pointer with
       bit 0 to select rope or string */
    JSValue left;
    JSValue right; /* might be the empty string */
} JSStringRope;

typedef enum {
    JS_CLOSURE_LOCAL, /* 'var_idx' is the index of a local variable in the parent function */
    JS_CLOSURE_ARG, /* 'var_idx' is the index of a argument variable in the parent function */
    JS_CLOSURE_REF, /* 'var_idx' is the index of a closure variable in the parent function */
    JS_CLOSURE_GLOBAL_REF, /* 'var_idx' in the index of a closure
                              variable in the parent function
                              referencing a global variable */
    JS_CLOSURE_GLOBAL_DECL, /* global variable declaration (eval code only) */
    JS_CLOSURE_GLOBAL, /* global variable (eval code only) */
    JS_CLOSURE_MODULE_DECL, /* definition of a module variable (eval code only) */
    JS_CLOSURE_MODULE_IMPORT, /* definition of a module import (eval code only) */ 
} JSClosureTypeEnum;

typedef struct JSClosureVar {
    JSClosureTypeEnum closure_type : 3;
    uint8_t is_lexical : 1; /* lexical variable */
    uint8_t is_const : 1; /* const variable (is_lexical = 1 if is_const = 1 */
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    uint16_t var_idx; /* is_local = TRUE: index to a normal variable of the
                    parent function. otherwise: index to a closure
                    variable of the parent function */
    JSAtom var_name;
} JSClosureVar;

#define ARG_SCOPE_INDEX 1
#define ARG_SCOPE_END (-2)

typedef enum {
    /* XXX: add more variable kinds here instead of using bit fields */
    JS_VAR_NORMAL,
    JS_VAR_FUNCTION_DECL, /* lexical var with function declaration */
    JS_VAR_NEW_FUNCTION_DECL, /* lexical var with async/generator
                                 function declaration */
    JS_VAR_CATCH,
    JS_VAR_FUNCTION_NAME, /* function expression name */
    JS_VAR_PRIVATE_FIELD,
    JS_VAR_PRIVATE_METHOD,
    JS_VAR_PRIVATE_GETTER,
    JS_VAR_PRIVATE_SETTER, /* must come after JS_VAR_PRIVATE_GETTER */
    JS_VAR_PRIVATE_GETTER_SETTER, /* must come after JS_VAR_PRIVATE_SETTER */
    JS_VAR_GLOBAL_FUNCTION_DECL, /* global function definition, only in JSVarDef */
} JSVarKindEnum;

typedef struct JSBytecodeVarDef {
    JSAtom var_name;
    /* index into JSFunctionBytecode.vars of the next variable in the same or
       enclosing lexical scope
    */
    int scope_next; /* XXX: store on 16 bits */
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1; /* XXX: could remove and use a var_ref_idx value */
    uint8_t has_scope: 1; /* true if JSVarDef.scope_level != 0 */
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    /* If is_captured = TRUE, provides, the index of the corresponding
       JSVarRef on stack. It would be more compact to have a separate
       table with the corresponding inverted table but it requires
       more modifications in the code. */
    uint16_t var_ref_idx;
} JSBytecodeVarDef;

/* for the encoding of the pc2line table */
#define PC2LINE_BASE     (-1)
#define PC2LINE_RANGE    5
#define PC2LINE_OP_FIRST 1
#define PC2LINE_DIFF_PC_MAX ((255 - PC2LINE_OP_FIRST) / PC2LINE_RANGE)

typedef enum JSFunctionKindEnum {
    JS_FUNC_NORMAL = 0,
    JS_FUNC_GENERATOR = (1 << 0),
    JS_FUNC_ASYNC = (1 << 1),
    JS_FUNC_ASYNC_GENERATOR = (JS_FUNC_GENERATOR | JS_FUNC_ASYNC),
} JSFunctionKindEnum;

/* Construction pre-sizing: a base (non-derived) constructor whose body begins
   with an unconditional straight-line run of `this.<name> = <leaf>` stores gets
   its instance created directly at the final shape, turning the field stores
   into plain set-value hits (no per-property shape transition / prop-array
   realloc). Gated so a -DCONFIG_PRESIZE_CTOR=0 oracle build is byte-for-byte
   behavior-identical. */
#ifndef CONFIG_PRESIZE_CTOR
#define CONFIG_PRESIZE_CTOR 1
#endif

#if CONFIG_PRESIZE_CTOR
/* Runtime-only (never serialized) construction pre-size hint attached to a
   JSFunctionBytecode. 'fields' is the ordered, deduped list of own-property
   atoms proven safe to pre-create. 'cached_shape' memoizes the final shape for
   the constructor's current .prototype (rebuilt on a .prototype change). The
   shape holds an owning ref that pins its proto, so it is GC-marked (see
   mark_children) to keep cycle collection correct, and released when the
   bytecode is freed. */
typedef struct JSCtorPresize {
    int field_count;
    JSAtom *fields;          /* owned atoms, length field_count */
    JSShape *cached_shape;   /* owned dup ref, or NULL until first build */
} JSCtorPresize;
#endif

/* Object-literal pre-sizing. An object literal's key set is fixed at compile
   time, so `{a:x, b:y, c:z}` can be created directly at its final shape instead
   of transitioning once per field.
   Unlike the constructor case this needs NO prototype-safety check:
   OP_define_field is [[DefineOwnProperty]], which never consults the prototype
   chain, and the half-built literal is unreachable from user code (it lives
   only on the interpreter stack until the last field is defined).
   Runtime-only and never serialized -- the table is keyed by an offset into the
   *final* bytecode, so it is rebuilt by the analysis pass on compile and is
   simply absent for bytecode loaded from a frozen file. */
#ifndef CONFIG_PRESIZE_LITERAL
#define CONFIG_PRESIZE_LITERAL 1
#endif

/* Runtime-recorded literal boilerplate (requires CONFIG_PRESIZE_LITERAL).
   ON by default (see the Makefile): the first evaluation of an object-literal
   site records the final shape as a presize site, so later evaluations build
   the object directly at that shape (computed-value literals included, which
   the parse-time analyzer cannot see). See js_object_presize_record_site().
   Fast property teardown for all-plain shapes: JSShape carries an all_plain
   bit (no slot has a JS_PROP_TMASK flag) and free_object() sweeps the
   property array with plain value frees, skipping the per-slot flag
   dispatch and the shape-prop walk. See free_object() and the
   js_shape_prepare_update()/add_shape_property() bit maintenance.
   REVIEW-FIX (F1): NEITHER flag has a source-level default. The Makefile
   always passes -DCONFIG_...=0|1 (both are CONFIG_SIG-tracked), so an
   opt-out build genuinely compiles the levers out; a build path that
   defines neither gets 0 (undefined identifiers evaluate to 0 in #if),
   i.e. OFF, never silently ON. */
#if !defined(CONFIG_LITERAL_BOILERPLATE)
#define CONFIG_LITERAL_BOILERPLATE 0
#endif
#if !defined(CONFIG_FAST_PROP_TEARDOWN)
#define CONFIG_FAST_PROP_TEARDOWN 0
#endif

#if CONFIG_PRESIZE_LITERAL
/* Cap per function; a function with more literal sites than this gets none.
   Sites are looked up by a linear scan (real functions have a handful), so the
   bound is also what keeps that scan cheap. */
#define JS_PRESIZE_MAX_SITES 32
/* Cap on the number of fields one recorded site may describe (parse-time
   scratch cap and runtime-recording cap; see js_object_presize_record_site). */
#define JS_PRESIZE_MAX_FIELDS 32

typedef struct JSObjectPresizeSite {
    uint32_t bc_offset;      /* offset of the OP_object byte in byte_code_buf */
    int field_count;
    JSAtom *fields;          /* owned atoms, length field_count */
    JSShape *cached_shape;   /* owned dup ref, or NULL until first build */
} JSObjectPresizeSite;

typedef struct JSObjectPresize {
    int site_count;
    int site_cap;            /* allocated slots; parse tables are exact-sized,
                                runtime-recorded tables have spare room */
    JSObjectPresizeSite sites[];
} JSObjectPresize;
#endif

/* `debug` MUST stay the last member: the bytecode reader does
   memcpy(b, &bc, offsetof(JSFunctionBytecode, debug)), a blob copy of every
   preceding field, and the destination is js_mallocz'd -- so a field moved
   after `debug` silently becomes 0/NULL instead of being copied.
   Reordering for cache locality was tried and MEASURED NEUTRAL (see
   micro-opt item 8): packing every field the call prologue reads into
   bytes 0..63 (one cache line, down from two) moved no benchmark, including one
   rotating over 4000 distinct functions to keep the metadata cold. Do not
   re-chase it. */
#ifndef CONFIG_FIELD_IC
#define CONFIG_FIELD_IC 0 /* experiment: no net win, shapes already hash-chained */
#endif
#if CONFIG_FIELD_IC
typedef struct JSFieldIC {
    struct JSShape *shape; /* strong ref; NULL = empty slot */
    uint32_t off;          /* index into JSObject.prop */
    uint32_t site;         /* (pc offset of atom operand) + 1; 0 = empty */
} JSFieldIC;
#endif

typedef struct JSFunctionBytecode {
    JSGCObjectHeader header; /* must come first */
    uint8_t js_mode;
    uint8_t has_prototype : 1; /* true if a prototype field is necessary */
    uint8_t has_simple_parameter_list : 1;
    uint8_t is_derived_class_constructor : 1;
    /* true if home_object needs to be initialized */
    uint8_t need_home_object : 1;
    uint8_t func_kind : 2;
    uint8_t new_target_allowed : 1;
    uint8_t super_call_allowed : 1;
    uint8_t super_allowed : 1;
    uint8_t arguments_allowed : 1;
    uint8_t has_debug : 1;
    uint8_t read_only_bytecode : 1;
    uint8_t is_direct_or_indirect_eval : 1; /* used by JS_GetScriptOrModuleName() */
    /* XXX: 9 bits available */
    uint8_t *byte_code_buf; /* (self pointer) */
    int byte_code_len;
    JSAtom func_name;
    JSBytecodeVarDef *vardefs; /* arguments + local variables (arg_count + var_count) (self pointer) */
    JSClosureVar *closure_var; /* list of variables in the closure (self pointer) */
    uint16_t arg_count;
    uint16_t var_count;
    uint16_t defined_arg_count; /* for length function property */
    uint16_t stack_size; /* maximum stack size */
    uint16_t var_ref_count; /* number of local variable references */
    JSContext *realm; /* function realm */
    JSValue *cpool; /* constant pool (self pointer) */
    int cpool_count;
    int closure_var_count;
#if CONFIG_PRESIZE_CTOR
    JSCtorPresize *ctor_presize; /* runtime-only, non-serialized; NULL if none */
#endif
#if CONFIG_PRESIZE_LITERAL
    JSObjectPresize *obj_presize; /* runtime-only, non-serialized; NULL if none */
    /* monomorphic inline cache for OP_get_field sites: direct-mapped by
       hashed bytecode offset, keyed by (site, shape); own plain-data
       properties only. Runtime-only, non-serialized; NULL when absent. */
#if CONFIG_FIELD_IC
    struct JSFieldIC *field_ic;
    uint32_t field_ic_mask;
#endif
#endif
    struct {
        /* debug info, move to separate structure to save memory? */
        JSAtom filename;
        int source_len; 
        int pc2line_len;
        uint8_t *pc2line_buf;
        char *source;
    } debug;
} JSFunctionBytecode;

/* Pin one dialect across every TU and helper script. C17 (201710L) is a
   defect-fix release of C11; the point is uniformity, not new syntax. */
_Static_assert(__STDC_VERSION__ >= 201710L,
               "dynascript requires C17 (-std=gnu17); a lower -std= has drifted in");

/* Guards for invariants that otherwise fail silently (wrong answer, no crash).

   bc_read memcpys only up to offsetof(...,debug), so a field after `debug`
   is left NULL instead of copied. */
_Static_assert(offsetof(JSFunctionBytecode, debug) +
                   sizeof(((JSFunctionBytecode *)0)->debug) ==
               sizeof(JSFunctionBytecode),
               "JSFunctionBytecode.debug must remain the LAST member: bc_read "
               "copies only up to offsetof(...,debug) and a later field becomes NULL");

/* JS_FLOAT64_TAG_ADDEND derives from JS_TAG_FIRST; a moved range mis-encodes. */
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
    JSPropertyEnum *tab_atom; /* is_array = FALSE */
} JSForInIterator;

typedef struct JSRegExp {
    JSString *pattern;
    JSString *bytecode; /* also contains the flags */
} JSRegExp;

typedef struct JSProxyData {
    JSValue target;
    JSValue handler;
    uint8_t is_func;
    uint8_t is_revoked;
} JSProxyData;

typedef struct JSArrayBuffer {
    int byte_length; /* 0 if detached */
    int max_byte_length; /* -1 if not resizable; >= byte_length otherwise */
    uint8_t detached;
    uint8_t shared; /* if shared, the array buffer cannot be detached */
    uint8_t *data; /* NULL if detached */
    struct list_head array_list;
    void *opaque;
    JSFreeArrayBufferDataFunc *free_func;
} JSArrayBuffer;

typedef struct JSTypedArray {
    struct list_head link; /* link to arraybuffer */
    JSObject *obj; /* back pointer to the TypedArray/DataView object */
    JSObject *buffer; /* based array buffer */
    uint32_t offset; /* byte offset in the array buffer */
    uint32_t length; /* byte length in the array buffer */
    BOOL track_rab; /* auto-track length of backing array buffer */
} JSTypedArray;

typedef struct JSGlobalObject {
    JSValue uninitialized_vars; /* hidden object containing the list of uninitialized variables */
} JSGlobalObject;

typedef struct JSAsyncFunctionState {
    JSGCObjectHeader header;
    JSValue this_val; /* 'this' argument */
    int argc; /* number of function arguments */
    BOOL throw_flag; /* used to throw an exception in JS_CallInternal() */
    BOOL is_completed; /* TRUE if the function has returned. The stack
                          frame is no longer valid */
    JSValue resolving_funcs[2]; /* only used in JS async functions */
    JSStackFrame frame;
    /* arg_buf, var_buf, stack_buf and var_refs follow */
} JSAsyncFunctionState;

typedef enum {
   /* binary operators */
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
   /* unary operators */
   JS_OVOP_POS = JS_OVOP_BINARY_COUNT,
   JS_OVOP_NEG,
   JS_OVOP_INC,
   JS_OVOP_DEC,
   JS_OVOP_NOT,

   JS_OVOP_COUNT,
} JSOverloadableOperatorEnum;

typedef struct {
    uint32_t operator_index;
    JSObject *ops[JS_OVOP_BINARY_COUNT]; /* self operators */
} JSBinaryOperatorDefEntry;

typedef struct {
    int count;
    JSBinaryOperatorDefEntry *tab;
} JSBinaryOperatorDef;

typedef struct {
    uint32_t operator_counter;
    BOOL is_primitive; /* OperatorSet for a primitive type */
    /* NULL if no operator is defined */
    JSObject *self_ops[JS_OVOP_COUNT]; /* self operators */
    JSBinaryOperatorDef left;
    JSBinaryOperatorDef right;
} JSOperatorSetData;

typedef struct JSReqModuleEntry {
    JSAtom module_name;
    JSModuleDef *module; /* used using resolution */
    JSValue attributes; /* JS_UNDEFINED or an object contains the attributes as key/value */
} JSReqModuleEntry;

typedef enum JSExportTypeEnum {
    JS_EXPORT_TYPE_LOCAL,
    JS_EXPORT_TYPE_INDIRECT,
} JSExportTypeEnum;

typedef struct JSExportEntry {
    union {
        struct {
            int var_idx; /* closure variable index */
            JSVarRef *var_ref; /* if != NULL, reference to the variable */
        } local; /* for local export */
        int req_module_idx; /* module for indirect export */
    } u;
    JSExportTypeEnum export_type;
    JSAtom local_name; /* '*' if export ns from. not used for local
                          export after compilation */
    JSAtom export_name; /* exported variable name */
} JSExportEntry;

typedef struct JSStarExportEntry {
    int req_module_idx; /* in req_module_entries */
} JSStarExportEntry;

typedef struct JSImportEntry {
    int var_idx; /* closure variable index */
    BOOL is_star; /* import_name = '*' is a valid import name, so need a flag */
    JSAtom import_name;
    int req_module_idx; /* in req_module_entries */
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
    JSGCObjectHeader header; /* must come first */
    JSAtom module_name;
    struct list_head link;

    JSReqModuleEntry *req_module_entries;
    int req_module_entries_count;
    int req_module_entries_size;

    JSExportEntry *export_entries;
    int export_entries_count;
    int export_entries_size;

    JSStarExportEntry *star_export_entries;
    int star_export_entries_count;
    int star_export_entries_size;

    JSImportEntry *import_entries;
    int import_entries_count;
    int import_entries_size;

    JSValue module_ns;
    JSValue func_obj; /* only used for JS modules */
    JSModuleInitFunc *init_func; /* only used for C modules */
    BOOL has_tla : 8; /* true if func_obj contains await */
    BOOL resolved : 8;
    BOOL func_created : 8;
    JSModuleStatus status : 8;
    /* temp use during js_module_link() & js_module_evaluate() */
    int dfs_index, dfs_ancestor_index;
    JSModuleDef *stack_prev;
    /* temp use during js_module_evaluate() */
    JSModuleDef **async_parent_modules;
    int async_parent_modules_count;
    int async_parent_modules_size;
    int pending_async_dependencies;
    BOOL async_evaluation; /* true: async_evaluation_timestamp corresponds to [[AsyncEvaluationOrder]] 
                              false: [[AsyncEvaluationOrder]] is UNSET or DONE */
    int64_t async_evaluation_timestamp;
    JSModuleDef *cycle_root;
    JSValue promise; /* corresponds to spec field: capability */
    JSValue resolving_funcs[2]; /* corresponds to spec field: capability */

    /* true if evaluation yielded an exception. It is saved in
       eval_exception */
    BOOL eval_has_exception : 8;
    JSValue eval_exception;
    JSValue meta_obj; /* for import.meta */
    JSValue private_value; /* private value for C modules */
};

typedef struct JSJobEntry {
    struct list_head link;
    JSContext *realm;
    JSJobFunc *job_func;
    int argc;
    JSValue argv[0];
} JSJobEntry;

typedef struct JSProperty {
    union {
        JSValue value;      /* JS_PROP_NORMAL */
        struct {            /* JS_PROP_GETSET */
            JSObject *getter; /* NULL if undefined */
            JSObject *setter; /* NULL if undefined */
        } getset;
        JSVarRef *var_ref;  /* JS_PROP_VARREF */
        struct {            /* JS_PROP_AUTOINIT */
            /* in order to use only 2 pointers, we compress the realm
               and the init function pointer */
            uintptr_t realm_and_id; /* realm and init_id (JS_AUTOINIT_ID_x)
                                       in the 2 low bits */
            void *opaque;
        } init;
    } u;
} JSProperty;

/* Raising this to trade reallocs for memory was MEASURED and REJECTED
   (2026-07-26). Speed: size 4 gave JSON.parse 1.08-1.10x and 9-field dynamic
   object construction 1.20x; size 8 gave 1.45x on the latter. Memory: on the
   worst case that matters (1M {x} + 1M {p,q}) peak RSS went 253.7 -> 316.2 MB,
   **+24.6%**, for size 4 alone. Small objects are far too common in real JS to
   pay that. The right fix is to presize where the count is KNOWN (as
   CONFIG_PRESIZE_LITERAL already does for object literals -- an object literal
   with 9 fields costs 123 ns vs 279 ns building the same fields one at a
   time), not to over-allocate every object blindly. */
#define JS_PROP_INITIAL_SIZE 2
#define JS_PROP_INITIAL_HASH_SIZE 4 /* must be a power of two */

typedef struct JSShapeProperty {
    uint32_t hash_next : 26; /* 0 if last in list */
    uint32_t flags : 6;   /* JS_PROP_XXX */
    JSAtom atom; /* JS_ATOM_NULL = free property entry */
} JSShapeProperty;

struct JSShape {
    JSGCObjectHeader header;
    /* true if the shape is inserted in the shape hash table. If not,
       JSShape.hash is not valid */
    uint8_t is_hashed;
#if CONFIG_FAST_PROP_TEARDOWN
    /* CONFIG_FAST_PROP_TEARDOWN: no property slot of this shape has a
       JS_PROP_TMASK flag (GETSET/VARREF/AUTOINIT); every slot is a plain
       JSValue that free_object() can sweep without per-slot flag dispatch.
       Monotonically cleared on any transition that adds a masked flag, and
       conservatively cleared by js_shape_prepare_update() (any in-place
       shape update may re-flag slots). */
    uint8_t all_plain;
#endif
    uint32_t hash; /* current hash value */
    uint32_t prop_hash_mask; /* >= 2 */
    int prop_size; /* allocated properties */
    int prop_count; /* include deleted properties */
    int deleted_prop_count;
    JSShape *shape_hash_next; /* in JSRuntime.shape_hash[h] list */
    JSObject *proto;
    uint32_t hash_table[]; /* prop_hash_mask + 1 elements */
    /* followed by JSShapeProperty prop[prop_size]; */
};

/* Per-Date-object cached field breakdown (see u.date in JSObject).
 *
 * get_date_fields() used to recompute the full civil-time breakdown
 * (tz shift, magic-mul days, year_from_days loop, month walk) on every
 * getter; repeat getters on one Date now validate one double (and, for
 * local time, the tz-memo epoch) and answer from here instead.
 *
 * Invalidation rule (see date.inc.c get_date_fields/JS_SetThisTimeValue):
 *  - STRUCTURAL: every store of a new time value goes through
 *    JS_SetThisTimeValue (all mutating setters) or JS_SetObjectData
 *    (construction) -- both clear valid on both slots.  These are the
 *    only writers of the Date time value (audited).
 *  - TIMEZONE: a local breakdown is a pure function of
 *    (time value, is_local, offset-for-that-UTC-second).  The offset is
 *    only served while it still equals what the tz memo would return:
 *    either the memo has not refilled since the computation (epoch
 *    fast path), or it refilled but re-resolving the same UTC second
 *    yields the same offset (slow revalidation).  This inherits the
 *    tz memo's documented <=1-second staleness window after a TZ
 *    change; it never extends it.  UTC slots have tz == 0 by
 *    definition and need no tz validation.
 * The struct is plain data (no JSValue, no GC refs): nothing to mark,
 * freed by js_object_data_finalizer. */
typedef struct JSDateFieldsCacheEntry {
    double time;          /* time value the fields were computed from */
    uint32_t tz_epoch;    /* tz memo epoch sampled AFTER the offset lookup
                             (local slot only; 0 for the UTC slot) */
    uint32_t valid;       /* 0 = empty, 1 = fields[] usable */
    int32_t fields[9];    /* y, mon(0-11), day(1-31), h, m, s, ms, wd, tz(min) */
} JSDateFieldsCacheEntry;

typedef struct JSDateFieldsCache {
    JSDateFieldsCacheEntry e[2]; /* e[0] = UTC breakdown, e[1] = local */
    struct JSDateFieldsCache *next_free; /* runtime freelist link while recycled */
} JSDateFieldsCache;

/* Recycled allocation for u.date.fields_cache: popped entries only have
 * their valid flags cleared -- a cache entry is only ever read through
 * its valid flag, so stale payload bytes are unreachable. */
static JSDateFieldsCache *js_date_cache_alloc(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSDateFieldsCache *fc = rt->date_cache_free_list;
    if (likely(fc != NULL)) {
        rt->date_cache_free_list = fc->next_free;
        fc->e[0].valid = 0;
        fc->e[1].valid = 0;
        return fc;
    }
    return js_mallocz(ctx, sizeof(*fc));
}

static void js_date_cache_free(JSRuntime *rt, JSDateFieldsCache *fc)
{
    fc->next_free = rt->date_cache_free_list;
    rt->date_cache_free_list = fc;
}

struct JSObject {
    JSGCObjectHeader header;
    /* TRUE if the array prototype is "normal":
       - no small index properties which are get/set or non writable
       - its prototype is Object.prototype
       - Object.prototype has no small index properties which are get/set or non writable
       - the prototype of Object.prototype is null (always true as it is immutable)
    */
    uint8_t is_std_array_prototype : 1;
    
    uint8_t extensible : 1;
    uint8_t free_mark : 1; /* only used when freeing objects with cycles */
    uint8_t is_exotic : 1; /* TRUE if object has exotic property handlers */
    uint8_t fast_array : 1; /* TRUE if u.array is used for get/put (for JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS, JS_CLASS_MAPPED_ARGUMENTS and typed arrays) */
    uint8_t is_constructor : 1; /* TRUE if object is a constructor function */
    uint8_t has_immutable_prototype : 1; /* cannot modify the prototype */
    uint8_t tmp_mark : 1; /* used in JS_WriteObjectRec() */
    uint8_t is_HTMLDDA : 1; /* specific annex B IsHtmlDDA behavior */
    uint16_t class_id; /* see JS_CLASS_x */
    /* count the number of weak references to this object. The object
       structure is freed only if header.ref_count = 0 and
       weakref_count = 0 */
    uint32_t weakref_count; 
    JSShape *shape; /* prototype and property names + flag */
    JSProperty *prop; /* array of properties */
    union {
        void *opaque;
        struct JSBoundFunction *bound_function; /* JS_CLASS_BOUND_FUNCTION */
        struct JSCFunctionDataRecord *c_function_data_record; /* JS_CLASS_C_FUNCTION_DATA */
        struct JSForInIterator *for_in_iterator; /* JS_CLASS_FOR_IN_ITERATOR */
        struct JSArrayBuffer *array_buffer; /* JS_CLASS_ARRAY_BUFFER, JS_CLASS_SHARED_ARRAY_BUFFER */
        struct JSTypedArray *typed_array; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_DATAVIEW */
        struct JSMapState *map_state;   /* JS_CLASS_MAP..JS_CLASS_WEAKSET */
        struct JSMapIteratorData *map_iterator_data; /* JS_CLASS_MAP_ITERATOR, JS_CLASS_SET_ITERATOR */
        struct JSArrayIteratorData *array_iterator_data; /* JS_CLASS_ARRAY_ITERATOR, JS_CLASS_STRING_ITERATOR */
        struct JSRegExpStringIteratorData *regexp_string_iterator_data; /* JS_CLASS_REGEXP_STRING_ITERATOR */
        struct JSGeneratorData *generator_data; /* JS_CLASS_GENERATOR */
        struct JSIteratorConcatData *iterator_concat_data; /* JS_CLASS_ITERATOR_CONCAT */
        struct JSIteratorZipData *iterator_zip_data; /* JS_CLASS_ITERATOR_ZIP */
        struct JSIteratorHelperData *iterator_helper_data; /* JS_CLASS_ITERATOR_HELPER */
        struct JSIteratorWrapData *iterator_wrap_data; /* JS_CLASS_ITERATOR_WRAP */
        struct JSProxyData *proxy_data; /* JS_CLASS_PROXY */
        struct JSPromiseData *promise_data; /* JS_CLASS_PROMISE */
        struct JSPromiseFunctionData *promise_function_data; /* JS_CLASS_PROMISE_RESOLVE_FUNCTION, JS_CLASS_PROMISE_REJECT_FUNCTION */
        struct JSAsyncFunctionState *async_function_data; /* JS_CLASS_ASYNC_FUNCTION_RESOLVE, JS_CLASS_ASYNC_FUNCTION_REJECT */
        struct JSAsyncFromSyncIteratorData *async_from_sync_iterator_data; /* JS_CLASS_ASYNC_FROM_SYNC_ITERATOR */
        struct JSAsyncGeneratorData *async_generator_data; /* JS_CLASS_ASYNC_GENERATOR */
        struct { /* JS_CLASS_BYTECODE_FUNCTION: 12/24 bytes */
            /* also used by JS_CLASS_GENERATOR_FUNCTION, JS_CLASS_ASYNC_FUNCTION and JS_CLASS_ASYNC_GENERATOR_FUNCTION */
            struct JSFunctionBytecode *function_bytecode;
            JSVarRef **var_refs;
            JSObject *home_object; /* for 'super' access */
        } func;
        struct { /* JS_CLASS_C_FUNCTION: 12/20 bytes */
            JSContext *realm;
            JSCFunctionType c_function;
            uint8_t length;
            uint8_t cproto;
            int16_t magic;
        } cfunc;
        /* array part for fast arrays and typed arrays */
        struct { /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS, JS_CLASS_MAPPED_ARGUMENTS, JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
            union {
                uint32_t size;          /* JS_CLASS_ARRAY */
                struct JSTypedArray *typed_array; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
            } u1;
            union {
                JSValue *values;        /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS */
                JSVarRef **var_refs;     /* JS_CLASS_MAPPED_ARGUMENTS */
                void *ptr;              /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
                int8_t *int8_ptr;       /* JS_CLASS_INT8_ARRAY */
                uint8_t *uint8_ptr;     /* JS_CLASS_UINT8_ARRAY, JS_CLASS_UINT8C_ARRAY */
                int16_t *int16_ptr;     /* JS_CLASS_INT16_ARRAY */
                uint16_t *uint16_ptr;   /* JS_CLASS_UINT16_ARRAY */
                int32_t *int32_ptr;     /* JS_CLASS_INT32_ARRAY */
                uint32_t *uint32_ptr;   /* JS_CLASS_UINT32_ARRAY */
                int64_t *int64_ptr;     /* JS_CLASS_INT64_ARRAY */
                uint64_t *uint64_ptr;   /* JS_CLASS_UINT64_ARRAY */
                uint16_t *fp16_ptr;     /* JS_CLASS_FLOAT16_ARRAY */
                float *float_ptr;       /* JS_CLASS_FLOAT32_ARRAY */
                double *double_ptr;     /* JS_CLASS_FLOAT64_ARRAY */
            } u;
            uint32_t count; /* <= 2^31-1. 0 for a detached typed array */
        } array;    /* 12/20 bytes */
        JSRegExp regexp;    /* JS_CLASS_REGEXP: 8/16 bytes */
        JSValue object_data;    /* for JS_SetObjectData(): 8/16/16 bytes */
        struct { /* JS_CLASS_DATE: 16 + 8 bytes; no union growth (func arm
                    is 3 pointers).  object_data MUST stay first so the
                    generic u.object_data accesses (JS_SetObjectData,
                    js_object_data_finalizer/mark, JS_ThisTimeValue,
                    bytecode serialization) keep working unchanged. */
            JSValue object_data;
            JSDateFieldsCache *fields_cache; /* lazily allocated by
                                                get_date_fields(), NULL
                                                until the first breakdown */
        } date;
        JSGlobalObject global_object;
    } u;
};

/* the date arm's object_data must alias u.object_data (generic code
   keeps using the latter for JS_CLASS_DATE) */
_Static_assert(offsetof(struct JSObject, u.date.object_data) ==
               offsetof(struct JSObject, u.object_data),
               "u.date.object_data must alias u.object_data");
/* and the arm must not grow JSObject (the func arm is 3 pointers wide) */
_Static_assert(sizeof(((struct JSObject *)0)->u.date) <= 3 * sizeof(void *),
               "u.date must not grow the JSObject union");

typedef struct JSMapRecord {
    int ref_count; /* used during enumeration to avoid freeing the record */
    BOOL empty : 8; /* TRUE if the record is deleted */
    struct list_head link;
    struct JSMapRecord *hash_next;
    JSValue key;
    JSValue value;
} JSMapRecord;

typedef struct JSMapState {
    BOOL is_weak; /* TRUE if WeakSet/WeakMap */
    struct list_head records; /* list of JSMapRecord.link */
    uint32_t record_count;
    JSMapRecord **hash_table;
    int hash_bits;
    uint32_t hash_size; /* = 2 ^ hash_bits */
    uint32_t record_count_threshold; /* count at which a hash table
                                        resize is needed */
    JSWeakRefHeader weakref_header; /* only used if is_weak = TRUE */
} JSMapState;

enum {
    __JS_ATOM_NULL = JS_ATOM_NULL,
#define DEF(name, str) JS_ATOM_ ## name,
#include "dyna-atom.h"
#undef DEF
    JS_ATOM_END,
};
#define JS_ATOM_LAST_KEYWORD JS_ATOM_super
#define JS_ATOM_LAST_STRICT_KEYWORD JS_ATOM_yield

static const char js_atom_init[] =
#define DEF(name, str) str "\0"
#include "dyna-atom.h"
#undef DEF
;

typedef enum OPCodeFormat {
#define FMT(f) OP_FMT_ ## f,
#define DEF(id, size, n_pop, n_push, f)
#include "dyna-opcode.h"
#undef DEF
#undef FMT
} OPCodeFormat;

enum OPCodeEnum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_ ## id,
#define def(id, size, n_pop, n_push, f)
#include "dyna-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT, /* excluding temporary opcodes */
    /* temporary opcodes : overlap with the short opcodes */
    OP_TEMP_START = OP_nop + 1,
    OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_ ## id,
#include "dyna-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_TEMP_END,
};

/* Bank-2 opcode ids (emitted as [OP_ext][op2]); see dyna-opcode2.h. */
enum OP2CodeEnum {
#define DEF2(id, size, n_pop, n_push, f) OP2_ ## id,
#include "dyna-opcode2.h"
#undef DEF2
    OP2_COUNT,
};

/* An untrusted byte (JS_ReadObject) indexes dispatch_table[256]. */
_Static_assert(OP_COUNT <= 256, "opcode count must fit the 256-entry dispatch table");
_Static_assert(OP2_COUNT <= 256, "OP_ext opcode count must fit dispatch_table2");
/* category ranges — contiguous per category for handler locality + range checks */
#define OP2_ARITH_FIRST OP2_mul_loc_loc
#define OP2_ARITH_LAST  OP2_sub_loc_loc
#define OP2_BRANCH_FIRST OP2_streq_const_if_false
#define OP2_BRANCH_LAST  OP2_streq_varprop_if_false
#define OP2_CALL_FIRST   OP2_call_method0
#define OP2_CALL_LAST    OP2_call_method1_const

/* OP_switch jump-table access. The table is a cpool string whose bytes are
   opaque to the (de)serializer (never byte-swapped), so ints are packed and
   read explicitly little-endian for host independence. */
static inline uint32_t switch_tbl_get(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void switch_tbl_put(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static int JS_InitAtoms(JSRuntime *rt);
static JSAtom __JS_NewAtomInit(JSRuntime *rt, const char *str, int len,
                               int atom_type);
static void JS_FreeAtomStruct(JSRuntime *rt, JSAtomStruct *p);
static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b);
static JSValue js_call_c_function(JSContext *ctx, JSValueConst func_obj,
                                  JSValueConst this_obj,
                                  int argc, JSValueConst *argv, int flags);
static JSValue js_call_bound_function(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj,
                                      int argc, JSValueConst *argv, int flags);
static JSValue JS_CallInternal(JSContext *ctx, JSValueConst func_obj,
                               JSValueConst this_obj, JSValueConst new_target,
                               int argc, JSValue *argv, int flags);
static JSValue JS_CallConstructorInternal(JSContext *ctx,
                                          JSValueConst func_obj,
                                          JSValueConst new_target,
                                          int argc, JSValue *argv, int flags);
static JSValue JS_CallFree(JSContext *ctx, JSValue func_obj, JSValueConst this_obj,
                           int argc, JSValueConst *argv);
static JSValue JS_InvokeFree(JSContext *ctx, JSValue this_val, JSAtom atom,
                             int argc, JSValueConst *argv);
static __exception int JS_ToArrayLengthFree(JSContext *ctx, uint32_t *plen,
                                            JSValue val, BOOL is_array_ctor);
static JSValue JS_EvalObject(JSContext *ctx, JSValueConst this_obj,
                             JSValueConst val, int flags, int scope_idx);
JSValue __attribute__((format(printf, 2, 3))) JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...);
static __maybe_unused void JS_DumpAtoms(JSRuntime *rt);
static __maybe_unused void JS_DumpString(JSRuntime *rt, const JSString *p);
static __maybe_unused void JS_DumpObjectHeader(JSRuntime *rt);
static __maybe_unused void JS_DumpObject(JSRuntime *rt, JSObject *p);
static __maybe_unused void JS_DumpGCObject(JSRuntime *rt, JSGCObjectHeader *p);
static __maybe_unused void JS_DumpAtom(JSContext *ctx, const char *str, JSAtom atom);
static __maybe_unused void JS_DumpValueRT(JSRuntime *rt, const char *str, JSValueConst val);
static __maybe_unused void JS_DumpValue(JSContext *ctx, const char *str, JSValueConst val);
static __maybe_unused void JS_DumpShapes(JSRuntime *rt);
static void js_dump_value_write(void *opaque, const char *buf, size_t len);
static JSValue js_function_apply(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic);
static void js_array_finalizer(JSRuntime *rt, JSValue val);
static void js_array_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
static void js_mapped_arguments_finalizer(JSRuntime *rt, JSValue val);
static void js_mapped_arguments_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
static void js_object_data_finalizer(JSRuntime *rt, JSValue val);
static void js_object_data_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
static void js_c_function_finalizer(JSRuntime *rt, JSValue val);
static void js_c_function_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
static void js_bytecode_function_finalizer(JSRuntime *rt, JSValue val);
static void js_bytecode_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_bound_function_finalizer(JSRuntime *rt, JSValue val);
static void js_bound_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_for_in_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_for_in_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_regexp_finalizer(JSRuntime *rt, JSValue val);
static void js_array_buffer_finalizer(JSRuntime *rt, JSValue val);
static void js_typed_array_finalizer(JSRuntime *rt, JSValue val);
static void js_typed_array_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_proxy_finalizer(JSRuntime *rt, JSValue val);
static void js_proxy_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_map_finalizer(JSRuntime *rt, JSValue val);
static void js_map_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_map_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_map_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_array_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_array_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_iterator_concat_finalizer(JSRuntime *rt, JSValue val);
static void js_iterator_zip_finalizer(JSRuntime *rt, JSValue val);
static void js_iterator_zip_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func);
static void js_iterator_concat_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func);
static void js_iterator_helper_finalizer(JSRuntime *rt, JSValue val);
static void js_iterator_helper_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func);
static void js_iterator_wrap_finalizer(JSRuntime *rt, JSValue val);
static void js_iterator_wrap_mark(JSRuntime *rt, JSValueConst val,
                                  JS_MarkFunc *mark_func);
static void js_regexp_string_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_regexp_string_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_generator_finalizer(JSRuntime *rt, JSValue obj);
static void js_generator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_global_object_finalizer(JSRuntime *rt, JSValue obj);
static void js_global_object_mark(JSRuntime *rt, JSValueConst val,
                                  JS_MarkFunc *mark_func);
static void js_promise_finalizer(JSRuntime *rt, JSValue val);
static void js_promise_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_promise_resolve_function_finalizer(JSRuntime *rt, JSValue val);
static void js_promise_resolve_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);

#define HINT_STRING  0
#define HINT_NUMBER  1
#define HINT_NONE    2
#define HINT_FORCE_ORDINARY (1 << 4) // don't try Symbol.toPrimitive
static JSValue JS_ToPrimitiveFree(JSContext *ctx, JSValue val, int hint);
static JSValue JS_ToStringFree(JSContext *ctx, JSValue val);
static int JS_ToBoolFree(JSContext *ctx, JSValue val);
static int JS_ToInt32Free(JSContext *ctx, int32_t *pres, JSValue val);
static int JS_ToFloat64Free(JSContext *ctx, double *pres, JSValue val);
static int JS_ToUint8ClampFree(JSContext *ctx, int32_t *pres, JSValue val);
static JSValue js_new_string8_len(JSContext *ctx, const char *buf, int len);
static JSValue js_compile_regexp(JSContext *ctx, JSValueConst pattern,
                                 JSValueConst flags);
static JSValue JS_NewRegexp(JSContext *ctx, JSValue pattern, JSValue bc);
static void gc_decref(JSRuntime *rt);
static int JS_NewClass1(JSRuntime *rt, JSClassID class_id,
                        const JSClassDef *class_def, JSAtom name);

typedef enum JSStrictEqModeEnum {
    JS_EQ_STRICT,
    JS_EQ_SAME_VALUE,
    JS_EQ_SAME_VALUE_ZERO,
} JSStrictEqModeEnum;

static BOOL js_strict_eq2(JSContext *ctx, JSValueConst op1, JSValueConst op2,
                          JSStrictEqModeEnum eq_mode);
static BOOL js_strict_eq(JSContext *ctx, JSValueConst op1, JSValueConst op2);
static BOOL js_same_value(JSContext *ctx, JSValueConst op1, JSValueConst op2);
static BOOL js_same_value_zero(JSContext *ctx, JSValueConst op1, JSValueConst op2);
static JSValue JS_ToObject(JSContext *ctx, JSValueConst val);
static JSValue JS_ToObjectFree(JSContext *ctx, JSValue val);
static JSProperty *add_property(JSContext *ctx,
                                JSObject *p, JSAtom prop, int prop_flags);
static void free_property(JSRuntime *rt, JSProperty *pr, int prop_flags);
static int JS_ToBigInt64Free(JSContext *ctx, int64_t *pres, JSValue val);
JSValue JS_ThrowOutOfMemory(JSContext *ctx);
static JSValue JS_ThrowTypeErrorRevokedProxy(JSContext *ctx);

static int js_resolve_proxy(JSContext *ctx, JSValueConst *pval, int throw_exception);
static int JS_CreateProperty(JSContext *ctx, JSObject *p,
                             JSAtom prop, JSValueConst val,
                             JSValueConst getter, JSValueConst setter,
                             int flags);
static int js_string_memcmp(const JSString *p1, int pos1, const JSString *p2,
                            int pos2, int len);
static JSValue js_array_buffer_constructor3(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, uint64_t *max_len,
                                            JSClassID class_id,
                                            uint8_t *buf,
                                            JSFreeArrayBufferDataFunc *free_func,
                                            void *opaque, BOOL alloc_flag);
static void js_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr);
static JSArrayBuffer *js_get_array_buffer(JSContext *ctx, JSValueConst obj);
static BOOL array_buffer_is_resizable(const JSArrayBuffer *abuf);
static JSValue js_typed_array_constructor(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int classid);
static JSValue js_typed_array_constructor_ta(JSContext *ctx,
                                             JSValueConst new_target,
                                             JSValueConst src_obj,
                                             int classid, uint32_t len);
static BOOL typed_array_is_oob(JSObject *p);
static int js_typed_array_get_length_unsafe(JSContext *ctx, JSValueConst obj);
static JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext *ctx);
static JSValue JS_ThrowTypeErrorArrayBufferOOB(JSContext *ctx);
static JSVarRef *js_create_var_ref(JSContext *ctx, BOOL is_lexical);
static JSVarRef *get_var_ref(JSContext *ctx, JSStackFrame *sf, int var_idx,
                             BOOL is_arg);
static void __async_func_free(JSRuntime *rt, JSAsyncFunctionState *s);
static void async_func_free(JSRuntime *rt, JSAsyncFunctionState *s);
static JSValue js_generator_function_call(JSContext *ctx, JSValueConst func_obj,
                                          JSValueConst this_obj,
                                          int argc, JSValueConst *argv,
                                          int flags);
static void js_async_function_resolve_finalizer(JSRuntime *rt, JSValue val);
static void js_async_function_resolve_mark(JSRuntime *rt, JSValueConst val,
                                           JS_MarkFunc *mark_func);
static JSValue JS_EvalInternal(JSContext *ctx, JSValueConst this_obj,
                               const char *input, size_t input_len,
                               const char *filename, int flags, int scope_idx);
static void js_free_module_def(JSRuntime *rt, JSModuleDef *m);
static void js_mark_module_def(JSRuntime *rt, JSModuleDef *m,
                               JS_MarkFunc *mark_func);
static JSValue js_import_meta(JSContext *ctx);
static JSValue js_dynamic_import(JSContext *ctx, JSValueConst specifier, JSValueConst options);
static void free_var_ref(JSRuntime *rt, JSVarRef *var_ref);
static JSValue js_new_promise_capability(JSContext *ctx,
                                         JSValue *resolving_funcs,
                                         JSValueConst ctor);
static __exception int perform_promise_then(JSContext *ctx,
                                            JSValueConst promise,
                                            JSValueConst *resolve_reject,
                                            JSValueConst *cap_resolving_funcs);
static JSValue js_promise_resolve(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic);
static JSValue js_promise_then(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static BOOL js_string_eq(JSContext *ctx,
                         const JSString *p1, const JSString *p2);
static int js_string_compare(JSContext *ctx,
                             const JSString *p1, const JSString *p2);
static JSValue JS_ToNumber(JSContext *ctx, JSValueConst val);
static int JS_SetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                               JSValue prop, JSValue val, int flags);
static int JS_NumberIsInteger(JSContext *ctx, JSValueConst val);
static BOOL JS_NumberIsNegativeOrMinusZero(JSContext *ctx, JSValueConst val);
static JSValue JS_ToNumberFree(JSContext *ctx, JSValue val);
static int JS_GetOwnPropertyInternal(JSContext *ctx, JSPropertyDescriptor *desc,
                                     JSObject *p, JSAtom prop);
static void js_free_desc(JSContext *ctx, JSPropertyDescriptor *desc);
static int JS_AddIntrinsicBasicObjects(JSContext *ctx);
static void js_free_shape(JSRuntime *rt, JSShape *sh);
static void js_free_shape_null(JSRuntime *rt, JSShape *sh);
static int js_shape_prepare_update(JSContext *ctx, JSObject *p,
                                   JSShapeProperty **pprs);
static int init_shape_hash(JSRuntime *rt);
static __exception int js_get_length32(JSContext *ctx, uint32_t *pres,
                                       JSValueConst obj);
static __exception int js_get_length64(JSContext *ctx, int64_t *pres,
                                       JSValueConst obj);
static void free_arg_list(JSContext *ctx, JSValue *tab, uint32_t len);
static JSValue *build_arg_list(JSContext *ctx, uint32_t *plen,
                               JSValueConst array_arg);
static BOOL js_get_fast_array(JSContext *ctx, JSValueConst obj,
                              JSValue **arrpp, uint32_t *countp);
/* for-of fast path over built-in array iterators (bc_read.inc.c) */
static BOOL js_for_of_next_fast_array(JSContext *ctx, JSValue *sp, int offset,
                                      JSValue *pvalue, int *pdone);
static JSValue JS_CreateAsyncFromSyncIterator(JSContext *ctx,
                                              JSValueConst sync_iter);
static void js_c_function_data_finalizer(JSRuntime *rt, JSValue val);
static void js_c_function_data_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func);
static JSValue js_c_function_data_call(JSContext *ctx, JSValueConst func_obj,
                                       JSValueConst this_val,
                                       int argc, JSValueConst *argv, int flags);
static JSAtom js_symbol_to_atom(JSContext *ctx, JSValue val);
static void add_gc_object(JSRuntime *rt, JSGCObjectHeader *h,
                          JSGCObjectTypeEnum type);
static void remove_gc_object(JSGCObjectHeader *h);
static JSValue js_instantiate_prototype(JSContext *ctx, JSObject *p, JSAtom atom, void *opaque);
static JSValue js_module_ns_autoinit(JSContext *ctx, JSObject *p, JSAtom atom,
                                 void *opaque);
static JSValue JS_InstantiateFunctionListItem2(JSContext *ctx, JSObject *p,
                                               JSAtom atom, void *opaque);
static JSValue js_object_groupBy(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int is_map);
static void map_delete_weakrefs(JSRuntime *rt, JSWeakRefHeader *wh);
static void weakref_delete_weakref(JSRuntime *rt, JSWeakRefHeader *wh);
static void finrec_delete_weakref(JSRuntime *rt, JSWeakRefHeader *wh);
static void JS_RunGCInternal(JSRuntime *rt, BOOL remove_weak_objects);
static JSValue js_array_from_iterator(JSContext *ctx, uint32_t *plen,
                                      JSValueConst obj, JSValueConst method);
static int js_string_find_invalid_codepoint(JSString *p);
static JSValue js_regexp_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
static JSValue get_date_string(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic);
static JSValue js_error_toString(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
static JSVarRef *js_global_object_find_uninitialized_var(JSContext *ctx, JSObject *p,
                                                         JSAtom atom, BOOL is_lexical);
static int typed_array_init(JSContext *ctx, JSValueConst obj,
                            JSValue buffer, uint64_t offset, uint64_t len,
                            BOOL track_rab);


static const JSClassExoticMethods js_arguments_exotic_methods;
static const JSClassExoticMethods js_string_exotic_methods;
static const JSClassExoticMethods js_proxy_exotic_methods;
static const JSClassExoticMethods js_module_ns_exotic_methods;
static JSClassID js_class_id_alloc = JS_CLASS_INIT_COUNT;

/* ──: prototype-extension opt-out ──
   When set BEFORE the first JS_NewContext, the project-added non-ECMAScript
   extension tiers are not installed at context creation: Array.prototype,
   String.prototype, Number.prototype, Date.prototype, Function.prototype,
   Iterator.prototype extensions and the Array/Object/Function static
   extensions. Standard ES builtins are untouched, so the engine runs with
   prototypes off; scripts that do not use the extensions behave identically.
   Process-wide by design: the CLI resolves --no-prototypes / the
   DYNAJS_NO_PROTOTYPES env once and every context in the process (main,
   workers, -q benchmark loop) sees the same surface. Default is unset --
   the extensions ship, exactly as before. */
static int js_no_prototype_extensions;

void JS_SetNoPrototypeExtensions(int disable)
{
    js_no_prototype_extensions = !!disable;
}

int JS_NoPrototypeExtensions(void)
{
    return js_no_prototype_extensions;
}


/* ── unity-build body: layered fragments included in original order ──
   Split for navigability while keeping a single translation unit so the
   whole-engine inlining the perf work depends on is preserved. See
   the modularization notes. Fragments are #included, never compiled alone. */

#include "src/mm/js_malloc.inc.c"
#include "src/value/atoms.inc.c"
#include "src/runtime/class.inc.c"
#include "src/object/shapes_objects_gc.inc.c"
#include "src/object/property_get.inc.c"
#include "src/object/property_set_convert.inc.c"
#include "src/vm/interpreter.inc.c"
#include "src/parser/parser.inc.c"
#include "src/serialize/bc_write.inc.c"
#include "src/serialize/bc_read.inc.c"
#include "src/builtins/iterator.inc.c"
#include "src/builtins/array.inc.c"
#include "src/builtins/number.inc.c"
#include "src/builtins/string_width.inc.c"
#include "src/builtins/string.inc.c"
#include "src/builtins/math.inc.c"
#include "src/builtins/date_timezone.inc.c"
#include "src/builtins/regexp.inc.c"
#include "src/builtins/json.inc.c"
#include "src/builtins/reflect.inc.c"
#include "src/builtins/proxy.inc.c"
#include "src/builtins/symbol.inc.c"
#include "src/builtins/promise_async.inc.c"
#include "src/builtins/date.inc.c"
#include "src/builtins/bigint_number.inc.c"
#include "src/builtins/typedarray_atomics.inc.c"
#include "src/builtins/weakref_init.inc.c"
#include "src/builtins/disposable.inc.c"
#include "src/runtime/using.inc.c"
