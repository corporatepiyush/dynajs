#ifndef ENGINE_SHARED_H
#define ENGINE_SHARED_H

#include "src/engine-internal.h"

#ifdef DYN_ENGINE_CORE_TU
static JSAtom js_get_atom_index(JSRuntime* rt, JSAtomStruct* p);
#else
#define js_get_atom_index js_get_atom_index__ext
#endif
JSAtom js_get_atom_index__ext(JSRuntime* rt, JSAtomStruct* p);

static inline JSValue JS_InvokeFree(JSContext* ctx, JSValue this_val, JSAtom atom,
    int argc, JSValueConst* argv);

void js_bytecode_function_finalizer(JSRuntime* rt, JSValue val);

void js_bytecode_function_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_regexp_finalizer(JSRuntime* rt, JSValue val);

void js_array_buffer_finalizer(JSRuntime* rt, JSValue val);

void js_typed_array_finalizer(JSRuntime* rt, JSValue val);

void js_typed_array_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_map_finalizer(JSRuntime* rt, JSValue val);

void js_map_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_map_iterator_finalizer(JSRuntime* rt, JSValue val);

void js_map_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_iterator_concat_finalizer(JSRuntime* rt, JSValue val);

void js_iterator_zip_finalizer(JSRuntime* rt, JSValue val);

void js_iterator_zip_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_iterator_concat_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_iterator_helper_finalizer(JSRuntime* rt, JSValue val);

void js_iterator_helper_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

void js_regexp_string_iterator_finalizer(JSRuntime* rt, JSValue val);

void js_regexp_string_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

#ifdef DYN_ENGINE_CORE_TU
static JSValue JS_ToPrimitiveFree(JSContext* ctx, JSValue val, int hint);
#else
#define JS_ToPrimitiveFree JS_ToPrimitiveFree__ext
#endif
JSValue JS_ToPrimitiveFree__ext(JSContext* ctx, JSValue val, int hint);

#ifdef DYN_ENGINE_CORE_TU
static int JS_ToInt32Free(JSContext* ctx, int32_t* pres, JSValue val);
#else
#define JS_ToInt32Free JS_ToInt32Free__ext
#endif
int JS_ToInt32Free__ext(JSContext* ctx, int32_t* pres, JSValue val);

#ifdef DYN_ENGINE_CORE_TU
static int JS_ToUint8ClampFree(JSContext* ctx, int32_t* pres, JSValue val);
#else
#define JS_ToUint8ClampFree JS_ToUint8ClampFree__ext
#endif
int JS_ToUint8ClampFree__ext(JSContext* ctx, int32_t* pres, JSValue val);

JSValue JS_NewRegexp(JSContext* ctx, JSValue pattern, JSValue bc);

static inline BOOL js_same_value_zero(JSContext* ctx, JSValueConst op1, JSValueConst op2);

JSValue JS_ThrowTypeErrorRevokedProxy(JSContext* ctx);

int js_resolve_proxy(JSContext* ctx, JSValueConst* pval, int throw_exception);

BOOL array_buffer_is_resizable(const JSArrayBuffer* abuf);

static inline void async_func_free(JSRuntime* rt, JSAsyncFunctionState* s);

#ifdef DYN_ENGINE_CORE_TU
static BOOL js_string_eq(JSContext* ctx,
    const JSString* p1, const JSString* p2);
#else
#define js_string_eq js_string_eq__ext
#endif
BOOL js_string_eq__ext(JSContext* ctx,
    const JSString* p1, const JSString* p2);

void js_regexp_cache_free(JSContext* ctx);
JSValue js_string_charCodeAt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static inline JSValue JS_ToNumber(JSContext* ctx, JSValueConst val);

static inline int JS_NumberIsInteger(JSContext* ctx, JSValueConst val);

static inline JSValue JS_ToNumberFree(JSContext* ctx, JSValue val);

int JS_AddIntrinsicBasicObjects(JSContext* ctx);

void map_delete_weakrefs(JSRuntime* rt, JSWeakRefHeader* wh);

void weakref_delete_weakref(JSRuntime* rt, JSWeakRefHeader* wh);

void finrec_delete_weakref(JSRuntime* rt, JSWeakRefHeader* wh);

JSValue get_date_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

extern const uint8_t typed_array_size_log2[12];

extern const JSClassExoticMethods js_string_exotic_methods;

extern int js_no_prototype_extensions;

#ifdef CONFIG_NURSERY_PROBE
#define JS_NURSERY_CHUNK_SIZE (2u * 1024u * 1024u)
#define JS_NURSERY_SENTINEL 0xAA

static BOOL js_nursery_probe_owned(const void* ptr)
{
    const uint8_t* p;
    if (unlikely(ptr == NULL))
        return FALSE;
    p = (const uint8_t*)ptr;
    return p[-6] == JS_NURSERY_SENTINEL;
}

static void* js_nursery_probe_alloc_rt(JSRuntime* rt, size_t size)
{
    size_t need = (size + 7) & ~(size_t)7;
    uint8_t* p;
    if (unlikely(rt->nursery_ptr == NULL || (size_t)(rt->nursery_end - rt->nursery_ptr) < need + 16)) {
        size_t cap = JS_NURSERY_CHUNK_SIZE;
        uint8_t* nb;
        if (need + 16 > cap)
            return NULL;
        nb = rt->malloc_ctx.mf.js_malloc(&rt->malloc_ctx.malloc_state, cap);
        if (unlikely(!nb))
            return NULL;
        *(void**)nb = rt->nursery_chunks;
        rt->nursery_chunks = nb;
        rt->nursery_ptr = nb + 24;
        rt->nursery_end = nb + cap;
    }
    p = rt->nursery_ptr;
    rt->nursery_ptr += need + 16;
    memset(p - 16, 0, 16);
    p[-16] = 0x53;
    p[-15] = 0x52;
    p[-14] = 0x55;
    p[-13] = 0x4E;
    *(uint32_t*)(void*)(p - 12) = (uint32_t)size;
    p[-6] = JS_NURSERY_SENTINEL;
    *(uint16_t*)(void*)(p - 8) = 0xffff;
    *(int*)(void*)(p - 4) = 1;
    rt->nursery_alloc_count++;
    rt->nursery_alloc_bytes += size;
    rt->malloc_ctx.malloc_state.malloc_size += size + MALLOC_OVERHEAD;
    rt->malloc_ctx.malloc_state.malloc_count++;
    return p;
}

static void js_nursery_probe_release_all(JSRuntime* rt)
{
    uint8_t *c = rt->nursery_chunks, *nx;
    while (c != NULL) {
        nx = *(uint8_t**)c;
        rt->malloc_ctx.mf.js_free(&rt->malloc_ctx.malloc_state, c);
        c = nx;
    }
    rt->nursery_chunks = NULL;
    rt->nursery_ptr = NULL;
    rt->nursery_end = NULL;
}
#endif

#ifdef JS_MALLOC_USE_ITER
typedef void JSMallocIterFunc(void* opaque, void* ptr);

static __maybe_unused void js_malloc_iter(JSMallocContext* s,
    JSMallocIterFunc* iter_func,
    void* iter_opaque)
{
    struct list_head* el;
    int block_size_idx;
    int i, j, n_words;
    uint32_t bmp;

    for (block_size_idx = 0; block_size_idx < JS_MALLOC_BLOCK_SIZE_COUNT;
        block_size_idx++) {
        unsigned int block_size = js_malloc_block_sizes[block_size_idx];
        list_for_each(el, &s->arena_list[block_size_idx])
        {
            JSMallocArena* ar = list_entry(el, JSMallocArena, link);
            n_words = (ar->n_blocks + 31) / 32;
            for (i = 0; i < n_words; i++) {
                bmp = ar->bitmap[i];
                while (bmp != 0) {
                    int block_nr;
                    j = ctz32(bmp);
                    bmp &= ~(1U << j);
                    block_nr = i * 32 + j;
                    if (block_nr < ar->n_blocks)
                        iter_func(iter_opaque,
                            get_arena_block(ar, block_nr, block_size));
                }
            }
        }
    }
    list_for_each(el, &s->large_block_list)
    {
        JSMallocLargeBlockHeader* lb = list_entry(el, JSMallocLargeBlockHeader, link);
        iter_func(iter_opaque, lb->header.user_data);
    }
}
#endif

void* js_malloc_rt(JSRuntime* rt, size_t size);

void js_free_rt(JSRuntime* rt, void* ptr);

void* js_realloc_rt(JSRuntime* rt, void* ptr, size_t size);

void* js_malloc(JSContext* ctx, size_t size);

void* js_mallocz(JSContext* ctx, size_t size);

void js_free(JSContext* ctx, void* ptr);

void* js_realloc(JSContext* ctx, void* ptr, size_t size);

void* js_realloc2(JSContext* ctx, void* ptr, size_t size, size_t* pslack);

char* js_strdup(JSContext* ctx, const char* str);

int js_resize_array(JSContext* ctx, void** parray, int elem_size,
    int* psize, int req_size);

static inline void* js_realloc_dbuf_rt(void* opaque, void* ptr, size_t size)
{
    return js_realloc_rt((JSRuntime*)opaque, ptr, size);
}

static inline const uint8_t* js_str_data_units(const JSString* p)
{
    return p->is_wide_char ? (const uint8_t*)js_str_data16(p)
                           : (const uint8_t*)js_str_data8(p);
}

static inline int string_get(const JSString* p, int idx)
{
    return p->is_wide_char ? js_str_data16(p)[idx] : js_str_data8(p)[idx];
}

typedef struct JSClassShortDef {
    JSAtom class_name;
    JSClassFinalizer* finalizer;
    JSClassGCMark* gc_mark;
} JSClassShortDef;

#ifdef DYN_ENGINE_CORE_TU
static int init_class_range(JSRuntime* rt, JSClassShortDef const* tab,
    int start, int count);
#else
#define init_class_range init_class_range__ext
#endif
int init_class_range__ext(JSRuntime* rt, JSClassShortDef const* tab,
    int start, int count);

#ifdef DYN_ENGINE_CORE_TU
static size_t js_def_malloc_usable_size(const void* ptr);
#else
#define js_def_malloc_usable_size js_def_malloc_usable_size__ext
#endif
size_t js_def_malloc_usable_size__ext(const void* ptr);

#ifdef CONFIG_SCL_ALLOC
typedef struct js_scl_allocator {
    void* (*malloc_fn)(void* state, size_t size, size_t alignment);
    void* (*calloc_fn)(void* state, size_t count, size_t size, size_t alignment);
    void* (*realloc_fn)(void* state, void* ptr, size_t old_size,
        size_t new_size, size_t alignment);
    void (*free_fn)(void* state, void* ptr);
    void* state;
} js_scl_allocator;

extern js_scl_allocator* scl_allocator_default(void);

static void* js_scl_malloc(JSMallocState* s, size_t size)
{
    js_scl_allocator* a = s->opaque;
    void* ptr;

    if (unlikely(size == 0))
        size = 1;
    ptr = a->malloc_fn(a->state, size, 0);
    if (!ptr)
        return NULL;
    s->malloc_count++;
    s->malloc_size += js_def_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_scl_free(JSMallocState* s, void* ptr)
{
    js_scl_allocator* a = s->opaque;
    size_t old_size;

    if (!ptr)
        return;
    old_size = js_def_malloc_usable_size(ptr);
    assert(s->malloc_count > 0);
    assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    a->free_fn(a->state, ptr);
}

static void* js_scl_realloc(JSMallocState* s, void* ptr, size_t size)
{
    js_scl_allocator* a = s->opaque;
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_scl_malloc(s, size);
    }
    old_size = js_def_malloc_usable_size(ptr);
    if (size == 0) {
        assert(s->malloc_count > 0);
        assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        a->free_fn(a->state, ptr);
        return NULL;
    }
    ptr = a->realloc_fn(a->state, ptr, old_size, size, 0);
    if (!ptr)
        return NULL;
    s->malloc_size += js_def_malloc_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions scl_malloc_funcs = {
    js_scl_malloc,
    js_scl_free,
    js_scl_realloc,
    js_def_malloc_usable_size,
};
#endif

#ifdef CONFIG_MIMALLOC
#include "mimalloc.h"

static size_t js_mi_malloc_usable_size(const void* ptr)
{
    return mi_usable_size((void*)ptr);
}

static void* js_mi_malloc(JSMallocState* s, size_t size)
{
    void* ptr;
    if (unlikely(size == 0))
        size = 1;
    ptr = mi_malloc(size);
    if (!ptr)
        return NULL;
    s->malloc_count++;
    s->malloc_size += mi_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_mi_free(JSMallocState* s, void* ptr)
{
    size_t old_size;
    if (!ptr)
        return;
    old_size = mi_usable_size(ptr);
    assert(s->malloc_count > 0);
    assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    mi_free(ptr);
}

static void* js_mi_realloc(JSMallocState* s, void* ptr, size_t size)
{
    size_t old_size;
    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_mi_malloc(s, size);
    }
    old_size = mi_usable_size(ptr);
    if (size == 0) {
        assert(s->malloc_count > 0);
        assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        mi_free(ptr);
        return NULL;
    }
    ptr = mi_realloc(ptr, size);
    if (!ptr)
        return NULL;
    s->malloc_size += mi_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions mi_malloc_funcs = {
    js_mi_malloc,
    js_mi_free,
    js_mi_realloc,
    js_mi_malloc_usable_size,
};

static long js_mi_env_long(const char* e)
{
    char* end;
    long v;
    errno = 0;
    v = strtol(e, &end, 10);
    if (end == e || errno == ERANGE)
        return 0;
    return v;
}

static void js_mimalloc_configure(void)
{
    const char* e;
    if ((e = getenv("DYNA_MI_PURGE_DELAY")))
        mi_option_set(mi_option_purge_delay, js_mi_env_long(e));
    if ((e = getenv("DYNA_MI_PURGE_DECOMMITS")))
        mi_option_set(mi_option_purge_decommits, js_mi_env_long(e));
    if ((e = getenv("DYNA_MI_EAGER_COMMIT")))
        mi_option_set(mi_option_eager_commit, js_mi_env_long(e));
    if ((e = getenv("DYNA_MI_LARGE_OS_PAGES")))
        mi_option_set(mi_option_allow_large_os_pages, js_mi_env_long(e));
    if ((e = getenv("DYNA_MI_RESERVE_HUGE")))
        mi_option_set(mi_option_reserve_huge_os_pages, js_mi_env_long(e));
    if ((e = getenv("DYNA_MI_ARENA_RESERVE")))
        mi_option_set(mi_option_arena_reserve, js_mi_env_long(e));
}
#endif

extern void (*const js_libc_free)(void*);

#ifdef DYN_ENGINE_CORE_TU
static int JS_EnqueueJob2(JSContext* ctx, JSJobFunc* job_func,
    int argc, JSValueConst* argv, BOOL no_exception);
#else
#define JS_EnqueueJob2 JS_EnqueueJob2__ext
#endif
int JS_EnqueueJob2__ext(JSContext* ctx, JSJobFunc* job_func,
    int argc, JSValueConst* argv, BOOL no_exception);

int JS_EnqueueJob(JSContext* ctx, JSJobFunc* job_func,
    int argc, JSValueConst* argv);

JSString* js_alloc_string(JSContext* ctx, int max_len, int is_wide_char);

JSContext* JS_DupContext(JSContext* ctx);

void JS_FreeContext(JSContext* ctx);

JSRuntime* JS_GetRuntime(JSContext* ctx);

static inline BOOL is_strict_mode(JSContext* ctx)
{
    JSStackFrame* sf = ctx->rt->current_stack_frame;
    return (sf && (sf->js_mode & JS_MODE_STRICT));
}

static inline int is_num(int c)
{
    return c >= '0' && c <= '9';
}

#define HASH_STRING_PRIME ((UINT64_C(1) << 61) - 1)
#define HASH_STRING_BLOCK 3
#define HASH_STRING_DOMAINS 8

static inline uint64_t hash_string_reduce(__uint128_t x)
{
    uint64_t r = ((uint64_t)x & HASH_STRING_PRIME) + (uint64_t)(x >> 61);
    return (r & HASH_STRING_PRIME) + (r >> 61);
}

static inline uint64_t hash_string_step(uint64_t h, uint64_t key, uint64_t m)
{
    return hash_string_reduce((__uint128_t)h * key + m);
}

static inline uint64_t hash_string_step2(uint64_t h, uint64_t key, uint64_t key2, uint64_t m1, uint64_t m2)
{
    return hash_string_reduce((__uint128_t)h * key2 + (__uint128_t)m1 * key + m2);
}

_Static_assert(JS_ATOM_TYPE_PRIVATE < HASH_STRING_DOMAINS, "every atom type needs a hash domain slot");

_Static_assert(HASH_STRING_DOMAINS == countof(((JSRuntime*)0)->atom_hash_init), "domain table size mismatch");

#define HASH_STRING_BLOCK_AT(str, i) (((uint64_t)HASH_STRING_BLOCK << 48) | ((uint64_t)(str)[i] << 32) | ((uint64_t)(str)[(i) + 1] << 16) | (str)[(i) + 2])

#define HASH_STRING_RUN_BODY                                                                       \
    size_t i = 0;                                                                                  \
    for (; i + 2 * HASH_STRING_BLOCK <= len; i += 2 * HASH_STRING_BLOCK)                           \
        h = hash_string_step2(h, key, key2, HASH_STRING_BLOCK_AT(str, i),                          \
            HASH_STRING_BLOCK_AT(str, i + HASH_STRING_BLOCK));                                     \
    if (i + HASH_STRING_BLOCK <= len) {                                                            \
        h = hash_string_step(h, key, HASH_STRING_BLOCK_AT(str, i));                                \
        i += HASH_STRING_BLOCK;                                                                    \
    }                                                                                              \
    if (i + 2 == len)                                                                              \
        h = hash_string_step(h, key, ((uint64_t)2 << 48) | ((uint64_t)str[i] << 16) | str[i + 1]); \
    else if (i + 1 == len)                                                                         \
        h = hash_string_step(h, key, ((uint64_t)1 << 48) | str[i]);                                \
    return (uint32_t)(h ^ (h >> 31));

static inline uint32_t hash_string8_run(const uint8_t* str, size_t len, uint64_t h, uint64_t key, uint64_t key2)
{
    HASH_STRING_RUN_BODY
}

static inline uint32_t hash_string16_run(const uint16_t* str, size_t len, uint64_t h, uint64_t key, uint64_t key2) {
    HASH_STRING_RUN_BODY
}

#define HASH_STRING_MUL1 263u
#define HASH_STRING_MUL2 69169u
#define HASH_STRING_MUL3 18191447u
#define HASH_STRING_MUL4 489383265u
#define HASH_STRING_FOLD_MIN 8
#define JS_HASH_CHAIN_FLOOD 64

static inline uint32_t hash_string8_fast(const uint8_t* str, size_t len, uint32_t h)
{
    size_t i = 0;
    if (unlikely(len >= HASH_STRING_FOLD_MIN)) {
        for (; i + 4 <= len; i += 4)
            h = h * HASH_STRING_MUL4 + (uint32_t)str[i] * HASH_STRING_MUL3 + (uint32_t)str[i + 1] * HASH_STRING_MUL2 + (uint32_t)str[i + 2] * HASH_STRING_MUL1 + (uint32_t)str[i + 3];
    }
    for (; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static inline uint32_t hash_string16_fast(const uint16_t* str, size_t len, uint32_t h)
{
    size_t i;
    for (i = 0; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static inline uint32_t hash_string_mode(JSRuntime* rt, const JSString* str, uint32_t domain, BOOL strong)
{
    if (unlikely(strong)) {
        if (str->is_wide_char)
            return hash_string16_run(js_str_data16(str), str->len, rt->atom_hash_init[domain], rt->atom_hash_key, rt->atom_hash_key2);
        return hash_string8_run(js_str_data8(str), str->len, rt->atom_hash_init[domain], rt->atom_hash_key, rt->atom_hash_key2);
    }
    if (str->is_wide_char)
        return hash_string16_fast(js_str_data16(str), str->len, (uint32_t)rt->atom_hash_seed ^ domain);
    return hash_string8_fast(js_str_data8(str), str->len, (uint32_t)rt->atom_hash_seed ^ domain);
}

uint32_t hash_string_rope(JSRuntime* rt, JSValueConst val, uint32_t domain, BOOL strong);

JSAtom JS_DupAtom(JSContext* ctx, JSAtom v);

#ifdef DYN_ENGINE_CORE_TU
static JSAtom __JS_NewAtom(JSRuntime* rt, JSString* str, int atom_type);
#else
#define __JS_NewAtom __JS_NewAtom__ext
#endif
JSAtom __JS_NewAtom__ext(JSRuntime* rt, JSString* str, int atom_type);

JSAtom JS_NewAtom(JSContext* ctx, const char* str);
JSAtom js_str_atom_get(JSContext* ctx, const char* str, BOOL keep_new);
void js_str_atom_unpin_all(JSRuntime* rt);

JSAtom JS_NewAtomUInt32(JSContext* ctx, uint32_t n);

static inline JSValue JS_NewSymbol(JSContext* ctx, JSString* p, int atom_type)
{
    JSRuntime* rt = ctx->rt;
    JSAtom atom;
    atom = __JS_NewAtom(rt, p, atom_type);
    if (atom == JS_ATOM_NULL)
        return JS_ThrowOutOfMemory(ctx);
    return JS_MKPTR(JS_TAG_SYMBOL, rt->atom_array[atom]);
}

const char* JS_AtomGetStr(JSContext* ctx, char* buf, int buf_size, JSAtom atom);

JSValue JS_AtomToValue(JSContext* ctx, JSAtom atom);

JSValue JS_AtomToString(JSContext* ctx, JSAtom atom);

void JS_FreeAtom(JSContext* ctx, JSAtom v);

void JS_FreeAtomRT(JSRuntime* rt, JSAtom v);

BOOL JS_IsRegisteredClass(JSRuntime* rt, JSClassID class_id);

JSValue js_new_string8_len(JSContext* ctx, const char* buf, int len);

JSValue js_new_string8(JSContext* ctx, const char* buf);

static inline JSValue js_new_string16_len(JSContext* ctx, const uint16_t* buf, int len)
{
    JSString* str;
    str = js_alloc_string(ctx, len, 1);
    if (!str)
        return JS_EXCEPTION;
    memcpy(str->u.str16, buf, len * 2);
    return JS_MKPTR(JS_TAG_STRING, str);
}

#ifdef DYN_ENGINE_CORE_TU
static JSValue js_new_string_char(JSContext* ctx, uint16_t c);
#else
#define js_new_string_char js_new_string_char__ext
#endif
JSValue js_new_string_char__ext(JSContext* ctx, uint16_t c);

JSValue js_sub_string(JSContext* ctx, JSString* p, int start, int end);

#ifdef DYN_ENGINE_CORE_TU
static int string_buffer_init2(JSContext* ctx, StringBuffer* s, int size,
    int is_wide);
#else
#define string_buffer_init2 string_buffer_init2__ext
#endif
int string_buffer_init2__ext(JSContext* ctx, StringBuffer* s, int size,
    int is_wide);

int string_buffer_init(JSContext* ctx, StringBuffer* s, int size);

void string_buffer_free(StringBuffer* s);

int string_buffer_putc8(StringBuffer* s, uint32_t c);

#ifdef DYN_ENGINE_CORE_TU
static int string_buffer_putc16(StringBuffer* s, uint32_t c);
#else
#define string_buffer_putc16 string_buffer_putc16__ext
#endif
int string_buffer_putc16__ext(StringBuffer* s, uint32_t c);

int string_buffer_putc(StringBuffer* s, uint32_t c);

int string_getc(const JSString* p, int* pidx);

int string_buffer_write8(StringBuffer* s, const uint8_t* p, int len);

#ifdef DYN_ENGINE_CORE_TU
static int string_buffer_write16(StringBuffer* s, const uint16_t* p, int len);
#else
#define string_buffer_write16 string_buffer_write16__ext
#endif
int string_buffer_write16__ext(StringBuffer* s, const uint16_t* p, int len);

int string_buffer_puts8(StringBuffer* s, const char* str);

int string_buffer_concat(StringBuffer* s, const JSString* p,
    uint32_t from, uint32_t to);

int string_buffer_concat_value(StringBuffer* s, JSValueConst v);

int string_buffer_concat_value_free(StringBuffer* s, JSValue v);

int string_buffer_fill(StringBuffer* s, int c, int count);

JSValue string_buffer_end(StringBuffer* s);

#ifndef CONFIG_UTF8_SIMD
#define CONFIG_UTF8_SIMD 1
#endif

JSValue JS_NewStringLen(JSContext* ctx, const char* buf, size_t buf_len);

JSValue JS_ConcatString3(JSContext* ctx, const char* str1,
    JSValue str2, const char* str3);

const char* JS_ToCStringLen2(JSContext* ctx, size_t* plen, JSValueConst val1, BOOL cesu8);

void JS_FreeCString(JSContext* ctx, const char* ptr);

_Static_assert(JS_STRING_LEN_MAX < 1134903170, "rope bucket table must cover the maximum string length");

JSValue JS_ConcatString(JSContext* ctx, JSValue op1, JSValue op2);

static force_inline uint32_t prop_hash_bucket(JSAtom atom, uint32_t mask)
{
    uint32_t h = (uint32_t)atom;
#ifdef CONFIG_PROP_HASH_MIX
    h ^= h >> 18;
#endif
    return h & mask;
}

#ifdef DYN_ENGINE_CORE_TU
static no_inline JSShape* js_new_shape2(JSContext* ctx, JSObject* proto,
    int hash_size, int prop_size);
#else
#define js_new_shape2 js_new_shape2__ext
#endif
JSShape* js_new_shape2__ext(JSContext* ctx, JSObject* proto,
    int hash_size, int prop_size);

static inline JSShape* js_dup_shape(JSShape* sh)
{
    js_rc(sh)->ref_count++;
    return sh;
}

void js_free_shape(JSRuntime* rt, JSShape* sh);

#ifdef DYN_ENGINE_CORE_TU
static int add_shape_property(JSContext* ctx, JSShape** psh,
    JSObject* p, JSAtom atom, int prop_flags);
#else
#define add_shape_property add_shape_property__ext
#endif
int add_shape_property__ext(JSContext* ctx, JSShape** psh,
    JSObject* p, JSAtom atom, int prop_flags);

#ifdef CONFIG_OBJ_POOL
static JSObject* js_obj_pool_alloc(JSContext* ctx)
{
    JSRuntime* rt = ctx->rt;
    JSObject* p = rt->obj_pool_free_list;
    if (likely(p != NULL)) {
        rt->obj_pool_free_list = *(JSObject**)p;
        rt->obj_pool_count--;
        return p;
    }
    return js_malloc(ctx, sizeof(JSObject));
}

static void js_obj_pool_free(JSRuntime* rt, JSObject* p)
{
    if (rt->obj_pool_count < JS_OBJ_POOL_MAX_OBJECTS) {
        *(JSObject**)p = rt->obj_pool_free_list;
        rt->obj_pool_free_list = p;
        rt->obj_pool_count++;
    } else {
        js_free_rt(rt, p);
    }
}

static JSValue* js_array_pool_alloc(JSRuntime* rt, uint32_t min_slots,
    uint32_t* psize)
{
    int b = min_slots <= 1 ? 0 : 32 - clz32(min_slots - 1);
    JSValue* tab;
    if (b >= JS_ARRAY_POOL_NBUCKETS)
        return NULL;
    tab = (JSValue*)rt->array_pool_free_list[b];
    if (likely(tab != NULL)) {
        rt->array_pool_free_list[b] = *(JSValue**)tab;
        rt->array_pool_count--;
        rt->array_pool_slots -= 1 << b;
        *psize = 1 << b;
    }
    return tab;
}

static void js_array_pool_free(JSRuntime* rt, JSObject* p)
{
    JSValue* tab = p->u.array.u.values;
    if (tab == NULL)
        return;
    if (p->class_id == JS_CLASS_ARRAY) {
        uint32_t size = p->u.array.u1.size;
        if (size != 0 && size <= JS_ARRAY_POOL_MAX_BUCKET && (size & (size - 1)) == 0 && rt->array_pool_slots + (int)size <= JS_ARRAY_POOL_MAX_SLOTS) {
            int b = 31 - clz32(size);
            *(JSValue**)tab = (JSValue*)rt->array_pool_free_list[b];
            rt->array_pool_free_list[b] = tab;
            rt->array_pool_count++;
            rt->array_pool_slots += size;
            return;
        }
    }
    js_free_rt(rt, tab);
}
#endif

#ifdef DYN_ENGINE_CORE_TU
static JSValue JS_NewObjectFromShape(JSContext* ctx, JSShape* sh, JSClassID class_id,
    JSProperty* props);
#else
#define JS_NewObjectFromShape JS_NewObjectFromShape__ext
#endif
JSValue JS_NewObjectFromShape__ext(JSContext* ctx, JSShape* sh, JSClassID class_id,
    JSProperty* props);

static inline JSObject* get_proto_obj(JSValueConst proto_val)
{
    if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_OBJECT)
        return NULL;
    else
        return JS_VALUE_GET_OBJ(proto_val);
}

JSValue JS_NewObjectProtoClass(JSContext* ctx, JSValueConst proto_val,
    JSClassID class_id);

JSValue JS_NewObjectProtoClassAlloc(JSContext* ctx, JSValueConst proto_val,
    JSClassID class_id, int n_alloc_props);

int JS_SetObjectData(JSContext* ctx, JSValueConst obj, JSValue val);

JSValue JS_NewObjectClass(JSContext* ctx, int class_id);

JSValue JS_NewObjectProto(JSContext* ctx, JSValueConst proto);

JSValue JS_NewArray(JSContext* ctx);

JSValue JS_NewObject(JSContext* ctx);

#ifdef DYN_ENGINE_CORE_TU
static JSValue js_new_anon_native_function(JSContext* ctx, JSClassID class_id,
    int length);
#else
#define js_new_anon_native_function js_new_anon_native_function__ext
#endif
JSValue js_new_anon_native_function__ext(JSContext* ctx, JSClassID class_id,
    int length);

BOOL js_class_has_bytecode(JSClassID class_id);

static inline JSFunctionBytecode* JS_GetFunctionBytecode(JSValueConst val)
{
    JSObject* p;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(val);
    if (!js_class_has_bytecode(p->class_id))
        return NULL;
    return p->u.func.function_bytecode;
}

JSValue JS_NewCFunction3(JSContext* ctx, JSCFunction* func,
    const char* name,
    int length, JSCFunctionEnum cproto, int magic,
    JSValueConst proto_val, int n_fields);

typedef struct JSCFunctionDataRecord {
    JSCFunctionData* func;
    uint8_t length;
    uint8_t data_len;
    uint16_t magic;
    JSValue data[0];
} JSCFunctionDataRecord;

JSValue JS_NewCFunctionData(JSContext* ctx, JSCFunctionData* func,
    int length, int magic, int data_len,
    JSValueConst* data);

#ifdef DYN_ENGINE_CORE_TU
static void free_zero_refcount(JSRuntime* rt);
#else
#define free_zero_refcount free_zero_refcount__ext
#endif
void free_zero_refcount__ext(JSRuntime* rt);

void add_gc_object(JSRuntime* rt, JSGCObjectHeader* h,
    JSGCObjectTypeEnum type);

void JS_MarkValue(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

JSValue JS_Throw(JSContext* ctx, JSValue obj);

JSValue JS_GetException(JSContext* ctx);

JSValue JS_ThrowError2(JSContext* ctx, JSErrorEnum error_num,
    const char* fmt, va_list ap, BOOL add_backtrace);

static inline JSValue __attribute__((format(printf, 3, 0))) JS_ThrowError(JSContext* ctx, JSErrorEnum error_num,
    const char* fmt, va_list ap)
{
    JSRuntime* rt = ctx->rt;
    JSStackFrame* sf;
    BOOL add_backtrace;

    sf = rt->current_stack_frame;
    add_backtrace = !rt->in_out_of_memory && (!sf || (JS_GetFunctionBytecode(sf->cur_func) == NULL));
    return JS_ThrowError2(ctx, error_num, fmt, ap, add_backtrace);
}

JSValue __attribute__((format(printf, 2, 3))) JS_ThrowSyntaxError(JSContext* ctx, const char* fmt, ...);

JSValue __attribute__((format(printf, 2, 3))) JS_ThrowTypeError(JSContext* ctx, const char* fmt, ...);

int __attribute__((format(printf, 3, 4))) JS_ThrowTypeErrorOrFalse(JSContext* ctx, int flags, const char* fmt, ...);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static inline JSValue __attribute__((format(printf, 3, 4))) __JS_ThrowTypeErrorAtom(JSContext* ctx, JSAtom atom, const char* fmt, ...)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    return JS_ThrowTypeError(ctx, fmt,
        JS_AtomGetStr(ctx, buf, sizeof(buf), atom));
}
#pragma GCC diagnostic pop

#define JS_ThrowTypeErrorAtom(ctx, fmt, atom) __JS_ThrowTypeErrorAtom(ctx, atom, fmt, "")

static inline int JS_ThrowTypeErrorReadOnly(JSContext* ctx, int flags, JSAtom atom)
{
    if ((flags & JS_PROP_THROW) || ((flags & JS_PROP_THROW_STRICT) && is_strict_mode(ctx))) {
        JS_ThrowTypeErrorAtom(ctx, "'%s' is read-only", atom);
        return -1;
    } else {
        return FALSE;
    }
}

JSValue __attribute__((format(printf, 2, 3))) JS_ThrowRangeError(JSContext* ctx, const char* fmt, ...);

JSValue __attribute__((format(printf, 2, 3))) JS_ThrowInternalError(JSContext* ctx, const char* fmt, ...);

JSValue JS_ThrowOutOfMemory(JSContext* ctx);

JSValue JS_ThrowStackOverflow(JSContext* ctx);

JSValue JS_ThrowTypeErrorNotAnObject(JSContext* ctx);

JSValue JS_ThrowTypeErrorNotAConstructor(JSContext* ctx,
    JSValueConst func_obj);

static inline JSValue JS_ThrowTypeErrorInvalidClass(JSContext* ctx, int class_id)
{
    JSRuntime* rt = ctx->rt;
    JSAtom name;
    name = rt->class_array[class_id].class_name;
    return JS_ThrowTypeErrorAtom(ctx, "%s object expected", name);
}

JSValue js_interrupt_swallowed(JSContext* ctx, JSValue ret);
#if defined(__SANITIZE_ADDRESS__)
void js_boundary_check(JSContext* ctx, JSValueConst ret);
void js_boundary_enter(JSContext* ctx);
#define JS_BOUNDARY_CHECK(ctx, ret) js_boundary_check(ctx, ret)
#define JS_BOUNDARY_ENTER(ctx) js_boundary_enter(ctx)
#else
#define JS_BOUNDARY_CHECK(ctx, ret) ((void)0)
#define JS_BOUNDARY_ENTER(ctx) ((void)0)
#endif

static inline void JS_ThrowInterrupted(JSContext* ctx)
{
    JS_ThrowInternalError(ctx, "interrupted");
    JS_SetUncatchableException(ctx, TRUE);
}

__exception int js_poll_interrupts(JSContext* ctx);

int JS_SetPrototypeInternal(JSContext* ctx, JSValueConst obj,
    JSValueConst proto_val,
    BOOL throw_flag);

JSValue JS_GetPrototype(JSContext* ctx, JSValueConst obj);

int JS_OrdinaryIsInstanceOf(JSContext* ctx, JSValueConst val,
    JSValueConst obj);

typedef JSValue JSAutoInitFunc(JSContext* ctx, JSObject* p, JSAtom atom, void* opaque);

extern JSAutoInitFunc* const js_autoinit_func_table[3];

_Static_assert(countof(js_autoinit_func_table) == JS_AUTOINIT_ID_COUNT,
    "autoinit function table out of sync with JSAutoInitIDEnum");

_Static_assert(countof(js_autoinit_func_table) <= 4,
    "autoinit id must fit the 2-bit realm_and_id field");

JSValue JS_GetPropertyInternal(JSContext* ctx, JSValueConst obj,
    JSAtom prop, JSValueConst this_obj,
    BOOL throw_ref_error);

#ifdef DYN_ENGINE_CORE_TU
static uint32_t js_string_obj_get_length(JSContext* ctx,
    JSValueConst obj);
#else
#define js_string_obj_get_length js_string_obj_get_length__ext
#endif
uint32_t js_string_obj_get_length__ext(JSContext* ctx,
    JSValueConst obj);

void JS_FreePropertyEnum(JSContext* ctx, JSPropertyEnum* tab, uint32_t len);

int __exception JS_GetOwnPropertyNamesInternal(JSContext* ctx,
    JSPropertyEnum** ptab,
    uint32_t* plen,
    JSObject* p, int flags);

int JS_GetOwnPropertyNames(JSContext* ctx, JSPropertyEnum** ptab,
    uint32_t* plen, JSValueConst obj, int flags);

int JS_GetOwnPropertyInternal(JSContext* ctx, JSPropertyDescriptor* desc,
    JSObject* p, JSAtom prop);

int JS_GetOwnProperty(JSContext* ctx, JSPropertyDescriptor* desc,
    JSValueConst obj, JSAtom prop);

int JS_IsExtensible(JSContext* ctx, JSValueConst obj);

int JS_PreventExtensions(JSContext* ctx, JSValueConst obj);

int JS_HasProperty(JSContext* ctx, JSValueConst obj, JSAtom prop);

JSAtom JS_ValueToAtom(JSContext* ctx, JSValueConst val);

JSValue JS_GetPropertyValue(JSContext* ctx, JSValueConst this_obj,
    JSValue prop);

JSValue JS_GetPropertyUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx);

int JS_TryGetPropertyInt64(JSContext* ctx, JSValueConst obj, int64_t idx, JSValue* pval);

JSValue JS_GetPropertyInt64(JSContext* ctx, JSValueConst obj, int64_t idx);

JSValue JS_GetPropertyStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop);

int expand_fast_array(JSContext* ctx, JSObject* p, uint32_t new_len);

static inline int add_fast_array_element(JSContext* ctx, JSObject* p,
    JSValue val, int flags)
{
    uint32_t new_len, array_len;
    new_len = p->u.array.count + 1;
    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
        array_len = JS_VALUE_GET_INT(p->prop[0].u.value);
        if (new_len > array_len) {
            if (unlikely(!(get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE))) {
                JS_FreeValue(ctx, val);
                return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);
            }
            p->prop[0].u.value = JS_NewInt32(ctx, new_len);
        }
    }
    if (unlikely(new_len > p->u.array.u1.size)) {
        if (expand_fast_array(ctx, p, new_len)) {
            JS_FreeValue(ctx, val);
            return -1;
        }
    }
    p->u.array.u.values[new_len - 1] = val;
    p->u.array.count = new_len;
    return TRUE;
}

JSValue js_allocate_fast_array(JSContext* ctx, int64_t len);

JSValue JS_NewArrayFrom(JSContext* ctx, int count, const JSValue* values);

JSValue js_create_array(JSContext* ctx, int len, JSValueConst* tab);

void js_free_desc(JSContext* ctx, JSPropertyDescriptor* desc);

int JS_SetPropertyInternal(JSContext* ctx, JSValueConst obj,
    JSAtom prop, JSValue val, JSValueConst this_obj, int flags);

int JS_SetPropertyValue(JSContext* ctx, JSValueConst this_obj,
    JSValue prop, JSValue val, int flags);

int JS_SetPropertyUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx, JSValue val);

int JS_SetPropertyInt64(JSContext* ctx, JSValueConst this_obj,
    int64_t idx, JSValue val);

int JS_SetPropertyStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop, JSValue val);

#ifdef DYN_ENGINE_CORE_TU
static BOOL check_define_prop_flags(int prop_flags, int flags);
#else
#define check_define_prop_flags check_define_prop_flags__ext
#endif
BOOL check_define_prop_flags__ext(int prop_flags, int flags);

int JS_DefineProperty(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValueConst val,
    JSValueConst getter, JSValueConst setter, int flags);

int JS_DefinePropertyValue(JSContext* ctx, JSValueConst this_obj,
    JSAtom prop, JSValue val, int flags);

int JS_DefinePropertyValueUint32(JSContext* ctx, JSValueConst this_obj,
    uint32_t idx, JSValue val, int flags);

int JS_DefinePropertyValueInt64(JSContext* ctx, JSValueConst this_obj,
    int64_t idx, JSValue val, int flags);

int JS_DefinePropertyValueStr(JSContext* ctx, JSValueConst this_obj,
    const char* prop, JSValue val, int flags);

int JS_CreateDataPropertyUint32(JSContext* ctx, JSValueConst this_obj,
    int64_t idx, JSValue val, int flags);

int JS_DeleteProperty(JSContext* ctx, JSValueConst obj, JSAtom prop, int flags);

BOOL JS_IsFunction(JSContext* ctx, JSValueConst val);

BOOL JS_IsCFunction(JSContext* ctx, JSValueConst val, JSCFunction* func, int magic);

BOOL JS_IsConstructor(JSContext* ctx, JSValueConst val);

BOOL JS_SetConstructorBit(JSContext* ctx, JSValueConst func_obj, BOOL val);

void JS_SetUncatchableException(JSContext* ctx, BOOL flag);

void JS_SetOpaque(JSValue obj, void* opaque);

void* JS_GetOpaque(JSValueConst obj, JSClassID class_id);

void* JS_GetOpaque2(JSContext* ctx, JSValueConst obj, JSClassID class_id);

static inline JSValue JS_ToPrimitive(JSContext* ctx, JSValueConst val, int hint)
{
    return JS_ToPrimitiveFree(ctx, JS_DupValue(ctx, val), hint);
}

int JS_ToBoolFree(JSContext* ctx, JSValue val);

int JS_ToBool(JSContext* ctx, JSValueConst val);

#ifdef DYN_ENGINE_CORE_TU
static int skip_spaces(const char* pc);
#else
#define skip_spaces skip_spaces__ext
#endif
int skip_spaces__ext(const char* pc);

#if JS_SHORT_BIG_INT_BITS == 32
#define JS_SHORT_BIG_INT_MIN INT32_MIN
#define JS_SHORT_BIG_INT_MAX INT32_MAX
#elif JS_SHORT_BIG_INT_BITS == 64
#define JS_SHORT_BIG_INT_MIN INT64_MIN
#define JS_SHORT_BIG_INT_MAX INT64_MAX
#else
#error unsupported
#endif

#if JS_LIMB_BITS == 32
static inline js_limb_t js_limb_clz(js_limb_t a)
{
    return clz32(a);
}
#else
static inline js_limb_t js_limb_clz(js_limb_t a)
{
    return clz64(a);
}
#endif

JSBigInt* js_bigint_new(JSContext* ctx, int len);

JSBigInt* js_bigint_set_short(JSBigIntBuf* buf, JSValueConst val);

#ifdef DYN_ENGINE_CORE_TU
static JSBigInt* js_bigint_normalize1(JSContext* ctx, JSBigInt* a, int l);
#else
#define js_bigint_normalize1 js_bigint_normalize1__ext
#endif
JSBigInt* js_bigint_normalize1__ext(JSContext* ctx, JSBigInt* a, int l);

static inline JSBigInt* js_bigint_normalize(JSContext* ctx, JSBigInt* a)
{
    return js_bigint_normalize1(ctx, a, a->len);
}

static inline int js_bigint_sign(const JSBigInt* a)
{
    return a->tab[a->len - 1] >> (JS_LIMB_BITS - 1);
}

double js_bigint_to_float64(JSContext* ctx, const JSBigInt* a);

JSBigInt* js_bigint_from_float64(JSContext* ctx, int* pres, double a1);

#ifdef DYN_ENGINE_CORE_TU
static JSValue js_bigint_to_string1(JSContext* ctx, JSValueConst val, int radix);
#else
#define js_bigint_to_string1 js_bigint_to_string1__ext
#endif
JSValue js_bigint_to_string1__ext(JSContext* ctx, JSValueConst val, int radix);

JSValue JS_CompactBigInt(JSContext* ctx, JSBigInt* p);

JSValue js_atof(JSContext* ctx, const char* str, const char** pp,
    int radix, int flags);

typedef enum JSToNumberHintEnum {
    TON_FLAG_NUMBER,
    TON_FLAG_NUMERIC,
} JSToNumberHintEnum;

JSValue JS_ToNumberHintFree(JSContext* ctx, JSValue val,
    JSToNumberHintEnum flag);

static inline JSValue JS_ToNumberFree(JSContext* ctx, JSValue val)
{
    return JS_ToNumberHintFree(ctx, val, TON_FLAG_NUMBER);
}

static inline JSValue JS_ToNumericFree(JSContext* ctx, JSValue val)
{
    return JS_ToNumberHintFree(ctx, val, TON_FLAG_NUMERIC);
}

int JS_ToFloat64Free(JSContext* ctx, double* pres, JSValue val);

int JS_ToFloat64(JSContext* ctx, double* pres, JSValueConst val);

static inline JSValue JS_ToNumber(JSContext* ctx, JSValueConst val)
{
    return JS_ToNumberFree(ctx, JS_DupValue(ctx, val));
}

__maybe_unused JSValue JS_ToIntegerFree(JSContext* ctx, JSValue val);

int JS_ToInt32Sat(JSContext* ctx, int* pres, JSValueConst val);

int JS_ToInt32Clamp(JSContext* ctx, int* pres, JSValueConst val,
    int min, int max, int min_offset);

int JS_ToInt64Sat(JSContext* ctx, int64_t* pres, JSValueConst val);

int JS_ToInt64Clamp(JSContext* ctx, int64_t* pres, JSValueConst val,
    int64_t min, int64_t max, int64_t neg_offset);

#ifdef DYN_ENGINE_CORE_TU
static int JS_ToInt64Free(JSContext* ctx, int64_t* pres, JSValue val);
#else
#define JS_ToInt64Free JS_ToInt64Free__ext
#endif
int JS_ToInt64Free__ext(JSContext* ctx, int64_t* pres, JSValue val);

int JS_ToInt64(JSContext* ctx, int64_t* pres, JSValueConst val);

int JS_ToInt32(JSContext* ctx, int32_t* pres, JSValueConst val);

int JS_ToIndex(JSContext* ctx, uint64_t* plen, JSValueConst val);

__exception int JS_ToLengthFree(JSContext* ctx, int64_t* plen,
    JSValue val);

static inline int JS_NumberIsInteger(JSContext* ctx, JSValueConst val)
{
    double d;
    if (!JS_IsNumber(val))
        return FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, val)))
        return -1;
    return isfinite(d) && floor(d) == d;
}

#ifdef DYN_ENGINE_CORE_TU
static JSValue js_dtoa2(JSContext* ctx,
    double d, int radix, int n_digits, int flags);
#else
#define js_dtoa2 js_dtoa2__ext
#endif
JSValue js_dtoa2__ext(JSContext* ctx,
    double d, int radix, int n_digits, int flags);

JSValue JS_ToString(JSContext* ctx, JSValueConst val);

JSValue JS_ToStringFree(JSContext* ctx, JSValue val);

JSValue JS_ToLocaleStringFree(JSContext* ctx, JSValue val);

JSValue JS_ToStringCheckObject(JSContext* ctx, JSValueConst val);

int JS_IsArray(JSContext* ctx, JSValueConst val);

static inline double js_pow(double a, double b)
{
    if (unlikely(!isfinite(b)) && fabs(a) == 1) {
        return JS_FLOAT64_NAN;
    } else {
        return pow(a, b);
    }
}

JSValue JS_NewBigInt64(JSContext* ctx, int64_t v);

JSValue JS_NewBigUint64(JSContext* ctx, uint64_t v);

#ifdef DYN_ENGINE_CORE_TU
static JSValue JS_StringToBigInt(JSContext* ctx, JSValue val);
#else
#define JS_StringToBigInt JS_StringToBigInt__ext
#endif
JSValue JS_StringToBigInt__ext(JSContext* ctx, JSValue val);

static inline JSValue JS_StringToBigIntErr(JSContext* ctx, JSValue val)
{
    val = JS_StringToBigInt(ctx, val);
    if (JS_VALUE_IS_NAN(val))
        return JS_ThrowSyntaxError(ctx, "invalid bigint literal");
    return val;
}

#ifdef DYN_ENGINE_CORE_TU
static JSValue JS_ToBigIntFree(JSContext* ctx, JSValue val);
#else
#define JS_ToBigIntFree JS_ToBigIntFree__ext
#endif
JSValue JS_ToBigIntFree__ext(JSContext* ctx, JSValue val);

int JS_ToBigInt64(JSContext* ctx, int64_t* pres, JSValueConst val);

#ifdef DYN_ENGINE_CORE_TU
static no_inline __exception int js_binary_arith_slow(JSContext* ctx, JSValue* sp,
    OPCodeEnum op);
#else
#define js_binary_arith_slow js_binary_arith_slow__ext
#endif
__exception int js_binary_arith_slow__ext(JSContext* ctx, JSValue* sp,
    OPCodeEnum op);

#ifdef DYN_ENGINE_CORE_TU
static no_inline __exception int js_add_slow(JSContext* ctx, JSValue* sp);
#else
#define js_add_slow js_add_slow__ext
#endif
__exception int js_add_slow__ext(JSContext* ctx, JSValue* sp);

#ifdef DYN_ENGINE_CORE_TU
static no_inline int js_relational_slow(JSContext* ctx, JSValue* sp,
    OPCodeEnum op);
#else
#define js_relational_slow js_relational_slow__ext
#endif
int js_relational_slow__ext(JSContext* ctx, JSValue* sp,
    OPCodeEnum op);

BOOL js_strict_eq2(JSContext* ctx, JSValueConst op1, JSValueConst op2,
    JSStrictEqModeEnum eq_mode);

BOOL js_same_value(JSContext* ctx, JSValueConst op1, JSValueConst op2);

static inline BOOL js_same_value_zero(JSContext* ctx, JSValueConst op1, JSValueConst op2)
{
    return js_strict_eq2(ctx, op1, op2, JS_EQ_SAME_VALUE_ZERO);
}

BOOL JS_SameValueZero(JSContext* ctx, JSValueConst op1, JSValueConst op2);

JSValue JS_GetIterator2(JSContext* ctx, JSValueConst obj,
    JSValueConst method);

JSValue JS_GetIterator(JSContext* ctx, JSValueConst obj, BOOL is_async);

JSValue JS_IteratorNext2(JSContext* ctx, JSValueConst enum_obj,
    JSValueConst method,
    int argc, JSValueConst* argv, int* pdone);

#ifdef DYN_ENGINE_CORE_TU
static JSValue __attribute__((noinline)) js_iterator_result_unpack(JSContext* ctx,
    JSValue obj, BOOL* pdone);
#else
#define js_iterator_result_unpack js_iterator_result_unpack__ext
#endif
JSValue js_iterator_result_unpack__ext(JSContext* ctx,
    JSValue obj, BOOL* pdone);

JSValue JS_IteratorNext(JSContext* ctx, JSValueConst enum_obj,
    JSValueConst method,
    int argc, JSValueConst* argv, BOOL* pdone);

int JS_IteratorClose(JSContext* ctx, JSValueConst enum_obj,
    BOOL is_exception_pending);

JSValue js_array_iterator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    BOOL* pdone, int magic);

static inline __attribute__((always_inline)) JSValue js_iterator_step(JSContext* ctx,
    JSValueConst enum_obj, JSValueConst method, BOOL* pdone)
{
    if (likely(JS_VALUE_GET_TAG(method) == JS_TAG_OBJECT)) {
        JSObject* m = JS_VALUE_GET_OBJ(method);
        if (m->class_id == JS_CLASS_C_FUNCTION && m->u.cfunc.cproto == JS_CFUNC_iterator_next) {
            JSValueConst undef = JS_UNDEFINED;
            JSValue r;
            int done;
            if (m->u.cfunc.c_function.iterator_next == js_array_iterator_next
                && JS_VALUE_GET_TAG(enum_obj) == JS_TAG_OBJECT
                && JS_VALUE_GET_OBJ(enum_obj)->class_id == JS_CLASS_ARRAY_ITERATOR) {
                JSArrayIteratorData* it = JS_VALUE_GET_OBJ(enum_obj)->u.array_iterator_data;
                if (likely(it && it->kind == JS_ITERATOR_KIND_VALUE && JS_VALUE_GET_TAG(it->obj) == JS_TAG_OBJECT)) {
                    JSObject* p = JS_VALUE_GET_OBJ(it->obj);
                    uint32_t idx = it->idx;
                    if (likely(p->class_id == JS_CLASS_ARRAY && p->fast_array
                            && JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT
                            && idx < (uint32_t)JS_VALUE_GET_INT(p->prop[0].u.value)
                            && idx < p->u.array.count)) {
                        it->idx = idx + 1;
                        *pdone = FALSE;
                        return JS_DupValue(ctx, p->u.array.u.values[idx]);
                    }
                }
            }
            done = 0;
            {
                uint64_t taken0 = ctx->rt->uncatchable_events;
                JS_BOUNDARY_ENTER(ctx);
                r = m->u.cfunc.c_function.iterator_next(ctx, enum_obj, 0, &undef, &done, m->u.cfunc.magic);
                if (unlikely(ctx->rt->uncatchable_events != taken0))
                    r = js_interrupt_swallowed(ctx, r);
                JS_BOUNDARY_CHECK(ctx, r);
            }
            if (unlikely(JS_IsException(r))) {
                *pdone = FALSE;
                return r;
            }
            if (likely(done == 0)) {
                *pdone = FALSE;
                return r;
            }
            if (done != 2) {
                JS_FreeValue(ctx, r);
                *pdone = TRUE;
                return JS_UNDEFINED;
            }
            return js_iterator_result_unpack(ctx, r, pdone);
        }
    }
    return JS_IteratorNext(ctx, enum_obj, method, 0, NULL, pdone);
}

#ifdef DYN_ENGINE_CORE_TU
static JSValue JS_IteratorGetCompleteValue(JSContext* ctx, JSValueConst obj,
    BOOL* pdone);
#else
#define JS_IteratorGetCompleteValue JS_IteratorGetCompleteValue__ext
#endif
JSValue JS_IteratorGetCompleteValue__ext(JSContext* ctx, JSValueConst obj,
    BOOL* pdone);

JSValue js_new_iterator_result(JSContext* ctx, JSValue val,
    BOOL done);

static inline JSValue js_create_iterator_result(JSContext* ctx,
    JSValue val,
    BOOL done)
{
    return js_new_iterator_result(ctx, val, done);
}

JSValue js_create_array_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

BOOL js_get_fast_array(JSContext* ctx, JSValueConst obj,
    JSValue** arrpp, uint32_t* countp);

__exception int JS_CopyDataProperties(JSContext* ctx,
    JSValueConst target,
    JSValueConst source,
    JSValueConst excluded,
    BOOL setprop);

JSValueConst JS_GetActiveFunction(JSContext* ctx);

static force_inline BOOL js_num_i32(JSValueConst v, int32_t* out)
{
    uint32_t tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return TRUE;
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        double d = JS_VALUE_GET_FLOAT64(v);
        if (d >= -2147483648.0 && d < 2147483648.0) {
            *out = (int32_t)d;
            return TRUE;
        }
    }
    return FALSE;
}

#ifdef OPCODE_ASM_LABEL
#pragma GCC diagnostic pop
#endif

JSValue js_call_owned(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj, int argc, JSValue* argv);

JSValue JS_Call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv);

JSValue JS_CallFree(JSContext* ctx, JSValue func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv);

JSValue js_create_from_ctor(JSContext* ctx, JSValueConst ctor,
    int class_id);

JSValue JS_CallConstructor2(JSContext* ctx, JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue JS_CallConstructor(JSContext* ctx, JSValueConst func_obj,
    int argc, JSValueConst* argv);

JSValue JS_Invoke(JSContext* ctx, JSValueConst this_val, JSAtom atom,
    int argc, JSValueConst* argv);

static inline JSValue JS_InvokeFree(JSContext* ctx, JSValue this_val, JSAtom atom,
    int argc, JSValueConst* argv)
{
    JSValue res = JS_Invoke(ctx, this_val, atom, argc, argv);
    JS_FreeValue(ctx, this_val);
    return res;
}

static inline void async_func_free(JSRuntime* rt, JSAsyncFunctionState* s)
{
    if (--js_rc(s)->ref_count == 0) {
        if (rt->gc_phase != JS_GC_PHASE_REMOVE_CYCLES) {
            list_del(&s->header.link);
            list_add(&s->header.link, &rt->gc_zero_ref_count_list);
            if (rt->gc_phase == JS_GC_PHASE_NONE) {
                free_zero_refcount(rt);
            }
        }
    }
}

#define GEN_MAGIC_THROW 2

JSValue js_generator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    BOOL* pdone, int magic);

#ifdef DYN_ENGINE_CORE_TU
static int js_async_function_settle(JSContext* ctx, JSValueConst func_obj,
    JSValueConst arg, BOOL is_reject);
#else
#define js_async_function_settle js_async_function_settle__ext
#endif
int js_async_function_settle__ext(JSContext* ctx, JSValueConst func_obj,
    JSValueConst arg, BOOL is_reject);

JSValue js_async_function_resolve_call(JSContext* ctx,
    JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags);

JSValue js_async_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags);
JSValue js_async_function_call_then(JSContext* ctx, JSValueConst func_obj, JSValueConst this_obj, int argc, JSValueConst* argv, JSValueConst* then_funcs);

typedef enum JSAsyncGeneratorStateEnum {
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_START,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_ASYNC_GENERATOR_STATE_EXECUTING,
    JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN,
    JS_ASYNC_GENERATOR_STATE_COMPLETED,
} JSAsyncGeneratorStateEnum;

typedef struct JSAsyncGeneratorData {
    JSObject* generator;
    JSAsyncGeneratorStateEnum state;
    JSAsyncFunctionState* func_state;
    struct list_head queue;
} JSAsyncGeneratorData;

#ifdef DYN_ENGINE_CORE_TU
static void js_async_generator_free(JSRuntime* rt,
    JSAsyncGeneratorData* s);
#else
#define js_async_generator_free js_async_generator_free__ext
#endif
void js_async_generator_free__ext(JSRuntime* rt,
    JSAsyncGeneratorData* s);

void js_async_generator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

JSValue js_async_generator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic);

JSValue js_async_generator_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags);

JSValue js_iterator_constructor_getset(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic,
    JSValue* func_data);

JSValue js_iterator_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);


typedef enum {
    ZIP_SUSPENDED_START,
    ZIP_SUSPENDED_YIELD,
    ZIP_RUNNING,
    ZIP_COMPLETED,
} JSIteratorZipState;

typedef enum JSIteratorHelperKindEnum {
    JS_ITERATOR_HELPER_KIND_DROP,
    JS_ITERATOR_HELPER_KIND_EVERY,
    JS_ITERATOR_HELPER_KIND_FILTER,
    JS_ITERATOR_HELPER_KIND_FIND,
    JS_ITERATOR_HELPER_KIND_FLAT_MAP,
    JS_ITERATOR_HELPER_KIND_FOR_EACH,
    JS_ITERATOR_HELPER_KIND_MAP,
    JS_ITERATOR_HELPER_KIND_SOME,
    JS_ITERATOR_HELPER_KIND_TAKE,
    JS_ITERATOR_HELPER_KIND_TAKE_WHILE,
    JS_ITERATOR_HELPER_KIND_DROP_WHILE,
    JS_ITERATOR_HELPER_KIND_SCAN,
    JS_ITERATOR_HELPER_KIND_INTERSPERSE,
    JS_ITERATOR_HELPER_KIND_COMPACT,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH,
    JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY,
    JS_ITERATOR_HELPER_KIND_APERTURE,
    JS_ITERATOR_HELPER_KIND_SPLIT_EVERY,
    JS_ITERATOR_HELPER_KIND_ZIP_WITH,
    JS_ITERATOR_HELPER_KIND_PLUCK,
    JS_ITERATOR_HELPER_KIND_INIT,
    JS_ITERATOR_HELPER_KIND_TAIL,
    JS_ITERATOR_HELPER_KIND_UNIQUE,
    JS_ITERATOR_HELPER_KIND_TEE,
    JS_ITERATOR_HELPER_KIND_CHUNKS,
    JS_ITERATOR_HELPER_KIND_WINDOWS,
    JS_ITERATOR_HELPER_KIND_WINDOWS_PARTIAL,
    JS_ITERATOR_HELPER_KIND_WITH_INDEX,
    JS_ITERATOR_HELPER_KIND_CYCLE,
    JS_ITERATOR_HELPER_KIND_STEP_BY,
    JS_ITERATOR_HELPER_KIND_TAP,
    JS_ITERATOR_HELPER_KIND_FLAT,
} JSIteratorHelperKindEnum;

typedef struct JSIteratorHelperData {
    JSValue obj;
    JSValue next;
    JSValue func;
    JSValue inner;
    JSValue acc;
    JSValue aux;
    int64_t count;
    int64_t count2;
    JSIteratorHelperKindEnum kind : 8;
    uint8_t executing : 1;
    uint8_t done : 1;
    uint8_t started : 1;
    uint8_t pending : 1;
    uint8_t src_done : 1;
    JSValue* buf;
    uint32_t buf_len;
    uint32_t buf_cap;
    uint32_t buf_head;
    struct JSCallPrep* cp;
} JSIteratorHelperData;

int js_iter_buf_push(JSContext* ctx, JSIteratorHelperData* it, JSValue v);

JSValue js_iterator_lazy_next(JSContext* ctx, JSIteratorHelperData* it,
    int magic, int* pdone);

JSValue js_create_iterator_helper(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_iterator_proto_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_iterator_range_proto_funcs[2];

extern const JSClassShortDef js_iterator_range_class_def[1];

extern const JSCFunctionListEntry js_iterator_funcs[4];

extern const JSCFunctionListEntry js_iterator_static_ext_funcs[1];

extern const JSCFunctionListEntry js_iterator_proto_funcs[15];

extern const JSCFunctionListEntry js_iterator_helper_proto_funcs[3];

extern const JSCFunctionListEntry js_array_unscopables_funcs[16];

uint64_t xorshift64star(uint64_t* pstate);

uint32_t map_hash_key(JSRuntime* rt, JSValueConst key, BOOL strong);

JSValueConst map_normalize_key_const(JSContext* ctx, JSValueConst key);

int js_array_ext_getel(JSContext* ctx, JSValueConst obj, int64_t i,
    JSValue* pval);

#include "src/builtins/poll.inc.c"

extern const JSCFunctionListEntry js_array_static_ext_funcs[1];

extern const JSCFunctionListEntry js_array_ext_funcs[97];

extern const JSCFunctionListEntry js_array_proto_funcs[40];

extern const JSCFunctionListEntry js_array_iterator_proto_funcs[2];

#ifndef MAGIC_SET
#define MAGIC_SET (1 << 0)
#endif

JSValue js_map_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv, int magic);

JSValue js_map_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_map_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

extern const JSCFunctionListEntry js_iterator_ext_funcs[51];

JSValue js_number_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_number_funcs[15];

int js_get_radix(JSContext* ctx, JSValueConst val);

double js_math_round(double a);

extern const JSCFunctionListEntry js_number_ext_funcs[44];

extern const JSCFunctionListEntry js_number_proto_funcs[6];

JSValue js_parseInt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_parseFloat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_boolean_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_boolean_proto_funcs[2];

#define STRING_EXT_SIMD_MIN 64

JSValue js_string_ext_stripAnsi(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_string_ext_displayWidth(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_string_ext_graphemes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_string_ext_wrapAnsi(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_string_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

int string_indexof_char(JSString* p, int c, int from);

int64_t string_advance_index(JSString* p, int64_t index, BOOL unicode);

int js_is_regexp(JSContext* ctx, JSValueConst obj);

int js_string_GetSubstitution(JSContext* ctx,
    StringBuffer* b,
    JSValueConst matched,
    JSString* sp,
    uint32_t position,
    JSValueConst captures_val,
    JSValueConst namedCaptures,
    JSValueConst rep,
    uint8_t** captures,
    uint32_t captures_len);

enum {
    magic_string_anchor,
    magic_string_big,
    magic_string_blink,
    magic_string_bold,
    magic_string_fixed,
    magic_string_fontcolor,
    magic_string_fontsize,
    magic_string_italics,
    magic_string_link,
    magic_string_small,
    magic_string_strike,
    magic_string_sub,
    magic_string_sup,
};

extern const JSCFunctionListEntry js_string_funcs[3];

#if defined(__clang__) && !defined(DYN_NO_VEC)
#define DYN_VECTORIZE_LOOP _Pragma("clang loop vectorize(enable) interleave(enable)")
#else
#define DYN_VECTORIZE_LOOP
#endif

JSValue js_global_encodeURI(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue js_global_decodeURI(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

extern const JSCFunctionListEntry js_string_ext_funcs[55];

extern const JSCFunctionListEntry js_string_proto_funcs[50];

extern const JSCFunctionListEntry js_string_iterator_proto_funcs[2];

int JS_AddIntrinsicStringNormalize(JSContext* ctx);

void js_random_init(JSContext* ctx);

extern const JSCFunctionListEntry js_math_obj[1];

static force_inline int js_regexp_get_lastIndex(JSContext* ctx, int64_t* plast_index,
    JSValueConst this_val)
{
    JSObject* p = JS_VALUE_GET_OBJ(this_val);

    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
        *plast_index = max_int(JS_VALUE_GET_INT(p->prop[0].u.value), 0);
        return 0;
    } else {
        return JS_ToLengthFree(ctx, plast_index, JS_DupValue(ctx, p->prop[0].u.value));
    }
}

static force_inline int js_regexp_set_lastIndex(JSContext* ctx, JSValueConst this_val,
    int last_index)
{
    JSObject* p = JS_VALUE_GET_OBJ(this_val);

    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT && (get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE))) {
        set_value(ctx, &p->prop[0].u.value, JS_NewInt32(ctx, last_index));
    } else {
        if (JS_SetProperty(ctx, this_val, JS_ATOM_lastIndex,
                JS_NewInt32(ctx, last_index))
            < 0)
            return -1;
    }
    return 0;
}

int JS_AddIntrinsicRegExp(JSContext* ctx);

#include "core/dyn-simdk.h"

extern const char json_hex_digits[17];

extern const uint8_t json_esc_bm[32];

static force_inline int json_clean_run8(const uint8_t* p, int len)
{
    size_t k = simd.find_bitmap(p, (size_t)len, json_esc_bm);
    return k == (size_t)-1 ? len : (int)k;
}

static force_inline int json_put_escape8(StringBuffer* b, uint32_t c)
{
    char buf[6];

    switch (c) {
    case '\t':
        c = 't';
        goto quote;
    case '\r':
        c = 'r';
        goto quote;
    case '\n':
        c = 'n';
        goto quote;
    case '\b':
        c = 'b';
        goto quote;
    case '\f':
        c = 'f';
        goto quote;
    case '\"':
    case '\\':
    quote:
        if (string_buffer_putc8(b, '\\'))
            return -1;
        return string_buffer_putc8(b, c);
    default:
        buf[0] = '\\';
        buf[1] = 'u';
        buf[2] = '0';
        buf[3] = '0';
        buf[4] = json_hex_digits[(c >> 4) & 0xf];
        buf[5] = json_hex_digits[c & 0xf];
        return string_buffer_write8(b, (const uint8_t*)buf, 6);
    }
}

int JS_AddIntrinsicJSON(JSContext* ctx);

extern const JSCFunctionListEntry js_reflect_obj[1];

int JS_AddIntrinsicProxy(JSContext* ctx);

JSValue js_symbol_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_symbol_proto_funcs[5];

extern const JSCFunctionListEntry js_symbol_funcs[17];

BOOL js_weakref_is_target(JSValueConst val);

BOOL js_weakref_is_live(JSValueConst val);

void js_weakref_free(JSRuntime* rt, JSValue val);

JSValue js_weakref_new(JSContext* ctx, JSValueConst val);

#define MAGIC_SET (1 << 0)

int JS_AddIntrinsicMapSet(JSContext* ctx);

extern const JSCFunctionListEntry js_generator_function_proto_funcs[1];

extern const JSCFunctionListEntry js_generator_proto_funcs[4];

JSValue JS_NewPromiseCapability(JSContext* ctx, JSValue* resolving_funcs);

JSValue js_promise_resolve(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

__exception int perform_promise_then(JSContext* ctx,
    JSValueConst promise,
    JSValueConst* resolve_reject,
    JSValueConst* cap_resolving_funcs);

JSValue JS_CreateAsyncFromSyncIterator(JSContext* ctx,
    JSValueConst sync_iter);

int JS_AddIntrinsicPromise(JSContext* ctx);

extern const JSCFunctionListEntry js_global_funcs[15];

extern const char date_digit_pairs[201];

int JS_AddIntrinsicDate(JSContext* ctx);

int JS_AddIntrinsicEval(JSContext* ctx);

int JS_AddIntrinsicBaseObjects(JSContext* ctx);

JSValue js_array_buffer_constructor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue js_shared_array_buffer_constructor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_array_buffer_funcs[2];

JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext* ctx);

JSValue JS_ThrowTypeErrorArrayBufferOOB(JSContext* ctx);

extern const JSCFunctionListEntry js_array_buffer_proto_funcs[12];

extern const JSCFunctionListEntry js_shared_array_buffer_funcs[1];

extern const JSCFunctionListEntry js_shared_array_buffer_proto_funcs[6];

BOOL typed_array_is_oob(JSObject* p);

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define TA_NEON 1
#endif

extern const JSCFunctionListEntry js_typed_array_base_funcs[3];

extern const JSCFunctionListEntry js_typed_array_base_proto_funcs[42];

extern const JSCFunctionListEntry js_typed_array_funcs[4];

extern const JSCFunctionListEntry js_uint8array_proto_funcs[5];

extern const JSCFunctionListEntry js_uint8array_funcs[3];

JSValue js_typed_array_base_constructor(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_typed_array_constructor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv,
    int classid);

JSValue js_dataview_constructor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry js_dataview_proto_funcs[26];

#ifdef CONFIG_ATOMICS
typedef enum AtomicsOpEnum {
    ATOMICS_OP_ADD,
    ATOMICS_OP_AND,
    ATOMICS_OP_OR,
    ATOMICS_OP_SUB,
    ATOMICS_OP_XOR,
    ATOMICS_OP_EXCHANGE,
    ATOMICS_OP_COMPARE_EXCHANGE,
    ATOMICS_OP_LOAD,
} AtomicsOpEnum;

#if defined(__APPLE__)
#define JS_ATOMICS_WAIT_RELATIVE 1
#elif defined(CLOCK_MONOTONIC)
#define JS_ATOMICS_DEADLINE_CLOCK CLOCK_MONOTONIC
#endif

#ifndef JS_ATOMICS_DEADLINE_CLOCK
#define JS_ATOMICS_DEADLINE_CLOCK CLOCK_REALTIME
#endif

#if defined(__aarch64__)
static inline void cpu_pause(void)
{
    __asm__ volatile("yield" ::: "memory");
}
#elif defined(__x86_64) || defined(__i386__)
static inline void cpu_pause(void)
{
    __asm__ volatile("pause" ::: "memory");
}
#else
static inline void cpu_pause(void)
{
}
#endif
#endif

int JS_AddIntrinsicTypedArrays(JSContext* ctx);

int JS_AddIntrinsicWeakRef(JSContext* ctx);

#endif
