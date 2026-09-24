/* JS malloc */

/* max overhead for size >= 64: 12.5% */
static const uint16_t js_malloc_block_sizes[JS_MALLOC_BLOCK_SIZE_COUNT] = {
    16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120,
    128, 144, 160, 176, 192, 208, 224, 240, 256, 288, 320, 352,
    384, 416, 448, 480, 512,
};

/* Number of fully-empty arenas kept as spares per size class. One is enough to
   eliminate the arena malloc+freelist-init churn of alloc-one/free-one workloads
   (e.g. short-lived closures/objects); the bounded extra RSS is <= one arena per
   active size class. */
#define JS_MALLOC_MAX_EMPTY_ARENAS 1

/* Replacing this branch chain with a 65-entry uint8 lookup table indexed by
   (size+7)>>3 was tried and MEASURED SLIGHTLY NEGATIVE (object churn 0.98x,
   everything else flat) -- the same result CLAUDE.md sec 4 records for the
   JS_CFUNC switch->table conversion. The branches are well predicted and the
   arithmetic is cheaper than the indexed load. Do not re-chase. */
static int get_block_size_index(size_t size)
{
    if (likely(size <= 16)) {
        return 0;
    } else if (likely(size <= 128)) {
        return (int)((size + 7) / 8 - 2);
    } else if (likely(size <= 256)) {
        return (int)((size + 15) / 16 + 6);
    } else if (likely(size <= 512)) {
        return (int)((size + 31) / 32 + 14);
    } else {
        return JS_MALLOC_BLOCK_SIZE_COUNT;
    }
}


#ifdef CONFIG_NURSERY_PROBE
/* ---- diagnostic nursery probe (see Makefile CONFIG_NURSERY_PROBE note) ----
 * Block layout (16-byte prefix entirely before the payload, so the real
 * JSMallocBlockHeader field offsets js_rc() reads at [-8..-1] are intact):
 *   [-16..-13] "NURS" magic   [-12..-9] payload size (u32, for realloc escape)
 *   [-8 ..-7]  0xFFFF (FREE_NIL fail-safe)   [-6] 0xAA sentinel
 *   [-5]       gc byte (0)   [-4..-1] ref_count = 1
 * js_free_rt on a probe block is a no-op (memory is never returned);
 * js_realloc_rt escapes the block to the normal heap (memcpy, no free). */
#define JS_NURSERY_CHUNK_SIZE (2u * 1024u * 1024u)
#define JS_NURSERY_SENTINEL 0xAA

static BOOL js_nursery_probe_owned(const void *ptr)
{
    const uint8_t *p;
    if (unlikely(ptr == NULL))
        return FALSE;
    /* Only ONE byte, at payload[-6]. For every REAL engine allocation that
       byte is the JSMallocBlockHeader.block_size_idx field itself (values
       0..30, or 0xff for large/zero blocks) -- 0xAA can never occur there,
       so the check is exact for engine pointers, which are the only
       pointers js_free_rt/js_realloc_rt ever receive. It must stay a
       single byte in [-8..-1]: those 8 header bytes are always mapped,
       but [-16] can be an unmapped page for a large block placed at a
       page start -- reading it SIGBUS'd (caught under lldb, repl.js). */
    p = (const uint8_t *)ptr;
    return p[-6] == JS_NURSERY_SENTINEL;
}

static void *js_nursery_probe_alloc_rt(JSRuntime *rt, size_t size)
{
    size_t need = (size + 7) & ~(size_t)7;
    uint8_t *p;
    if (unlikely(rt->nursery_ptr == NULL ||
                 (size_t)(rt->nursery_end - rt->nursery_ptr) < need + 16)) {
        size_t cap = JS_NURSERY_CHUNK_SIZE;
        uint8_t *nb;
        if (need + 16 > cap)
            return NULL; /* oversized: caller falls back to js_malloc */
        nb = rt->malloc_ctx.mf.js_malloc(&rt->malloc_ctx.malloc_state, cap);
        if (unlikely(!nb))
            return NULL;
        *(void **)nb = rt->nursery_chunks; /* chunk list lives at [0..7] */
        rt->nursery_chunks = nb;
        /* the first bump allocation needs its full 16-byte prefix INSIDE
           the chunk: [0..7] = chunk link, [8..23] = prefix of alloc #1,
           first payload at +40. Starting the bump at +8 (as an earlier
           revision did) wrote the prefix 8 bytes BEFORE the chunk --
           out-of-bounds memset + link clobber, caught by dynajsc crashing
           in js_nursery_probe_release_all. */
        rt->nursery_ptr = nb + 24;
        rt->nursery_end = nb + cap;
    }
    p = rt->nursery_ptr;
    rt->nursery_ptr += need + 16;
    memset(p - 16, 0, 16);
    p[-16] = 0x53; p[-15] = 0x52; p[-14] = 0x55; p[-13] = 0x4E; /* "NURS" */
    *(uint32_t *)(void *)(p - 12) = (uint32_t)size;
    p[-6] = JS_NURSERY_SENTINEL;
    *(uint16_t *)(void *)(p - 8) = 0xffff; /* FREE_NIL: fail-safe */
    *(int *)(void *)(p - 4) = 1;           /* ref_count; JS_NewObjectFromShape
                                              overwrites for objects */
    rt->nursery_alloc_count++;
    rt->nursery_alloc_bytes += size;
    /* accounting mirrors js_def_malloc; never decremented on free: the
       memory genuinely stays mapped until runtime teardown (leak probe). */
    rt->malloc_ctx.malloc_state.malloc_size += size + MALLOC_OVERHEAD;
    rt->malloc_ctx.malloc_state.malloc_count++;
    return p;
}

static void js_nursery_probe_release_all(JSRuntime *rt)
{
    uint8_t *c = rt->nursery_chunks, *nx;
    while (c != NULL) {
        nx = *(uint8_t **)c;
        rt->malloc_ctx.mf.js_free(&rt->malloc_ctx.malloc_state, c);
        c = nx;
    }
    rt->nursery_chunks = NULL;
    rt->nursery_ptr = NULL;
    rt->nursery_end = NULL;
}
#endif /* CONFIG_NURSERY_PROBE */

static JSMallocBlockHeader *get_zero_size_block(JSMallocContext *s)
{
    return (JSMallocBlockHeader *)s->zero_size_block;
}

static void js_malloc_init(JSMallocContext *s)
{
    int i;
    memset(s, 0, sizeof(*s));
    get_zero_size_block(s)->u.block_idx = FREE_NIL;
    for (i = 0; i < JS_MALLOC_BLOCK_SIZE_COUNT; i++) {
        init_list_head(&s->arena_list[i]);
        init_list_head(&s->free_arena_list[i]);
    }
#ifdef JS_MALLOC_USE_ITER
    init_list_head(&s->large_block_list);
#endif
}

static void *get_arena_block(JSMallocArena *ar, unsigned int idx,
                             unsigned int block_size)
{
    return ar->blocks + idx * block_size;
}

/* Release every fully-empty arena retained as a spare (see n_empty_arenas).
   Must run after all user blocks are freed at runtime teardown so no arena is
   leaked; empty arenas always sit on free_arena_list. */
static void js_malloc_release_empty_arenas(JSMallocContext *s)
{
    struct list_head *el, *el1;
    int i;
    for (i = 0; i < JS_MALLOC_BLOCK_SIZE_COUNT; i++) {
        list_for_each_safe(el, el1, &s->free_arena_list[i]) {
            JSMallocArena *ar = list_entry(el, JSMallocArena, free_link);
            if (ar->n_used_blocks == 0) {
                list_del(&ar->link);
                list_del(&ar->free_link);
                s->mf.js_free(&s->malloc_state, ar);
            }
        }
        s->n_empty_arenas[i] = 0;
    }
}

static inline JSMallocBlockHeader *js_rc(void *ptr)
{
    return container_of(ptr, JSMallocBlockHeader, user_data);
}

/* no_inline: slow-path arena creation, not worth inlining. */
static no_inline JSMallocArena *js_malloc_new_arena(JSMallocContext *s,
                                                     int block_size_idx)
{
    JSMallocBlockHeader *b;
    JSMallocArena *ar;
    int n_blocks, block_size, i;

    block_size = js_malloc_block_sizes[block_size_idx];
    n_blocks = (JS_MALLOC_ARENA_SIZE - sizeof(JSMallocArena)) / block_size;
    /* Arena must fit at least one block; JS_MALLOC_ARENA_SIZE is expected
       to be >> 512 (the largest small size class). */
    assert(n_blocks > 0);

    ar = s->mf.js_malloc(&s->malloc_state,
                         sizeof(JSMallocArena) + n_blocks * block_size);
    if (!ar)
        return NULL;

    ar->block_size_idx = block_size_idx;
    ar->n_blocks = n_blocks;
    ar->n_used_blocks = 0;
    ar->first_free_block = 0;
#ifdef JS_MALLOC_USE_ITER
    {
        int n_bitmap_words = (n_blocks + 31) / 32;
        /* If JSMallocArena uses a fixed-size bitmap array, n_bitmap_words must
           not exceed its capacity.  If it uses a flex array, the allocation above
           must have reserved sufficient space before ar->blocks[]. */
        (void)n_bitmap_words; /* silence warning if asserts are disabled */
        assert(n_bitmap_words <=
               (int)((sizeof(ar->bitmap) / sizeof(ar->bitmap[0]))));
        for (i = 0; i < n_bitmap_words; i++)
            ar->bitmap[i] = 0;
    }
#endif
    for (i = 0; i < n_blocks - 1; i++) {
        b = get_arena_block(ar, i, block_size);
        b->u.free_next = i + 1;
        b->block_size_idx = block_size_idx;
    }
    b = get_arena_block(ar, n_blocks - 1, block_size);
    b->u.free_next = FREE_NIL;
    b->block_size_idx = block_size_idx;

    /* add to the head */
    list_add(&ar->link, &s->arena_list[block_size_idx]);
    list_add(&ar->free_link, &s->free_arena_list[block_size_idx]);
    return ar;
}

static no_inline void *js_malloc_large(JSMallocContext *s, size_t size)
{
    JSMallocLargeBlockHeader *b;
    if (unlikely(size > SIZE_MAX - sizeof(JSMallocLargeBlockHeader)))
        return NULL;
    b = s->mf.js_malloc(&s->malloc_state, sizeof(JSMallocLargeBlockHeader) + size);
    if (!b)
        return NULL;
    b->header.u.block_idx = FREE_NIL;
    b->header.block_size_idx = 0xff; /* fail safe */
#ifdef JS_MALLOC_USE_ITER
    list_add_tail(&b->link, &s->large_block_list);
#endif
    return b->header.user_data;
}

static void *__js_malloc(JSMallocContext *s, size_t size)
{
    size_t total_size;
    if (unlikely(size == 0)) {
        JSMallocBlockHeader *b = get_zero_size_block(s);
        return b->user_data;
    }

    total_size = (size + JS_MALLOC_ALIGN - 1) & ~(JS_MALLOC_ALIGN - 1);
    /* Overflow guard: if the aligned payload already wrapped, or adding the
       header wraps, we must bail out before touching the size classes. */
    if (unlikely(total_size < size ||
                 total_size + sizeof(JSMallocBlockHeader) < total_size)) {
        return NULL;
    }
    total_size += sizeof(JSMallocBlockHeader);

    if (!JS_MALLOC_LARGE_BLOCKS_ONLY && likely(!s->no_pools)
        && likely(total_size <= JS_MALLOC_MAX_SMALL_SIZE)) {
        int block_size_idx;
        unsigned int block_idx, block_size;
        JSMallocBlockHeader *b;
        JSMallocArena *ar;
        struct list_head *el, *head;

        block_size_idx = get_block_size_index(total_size);
        block_size = js_malloc_block_sizes[block_size_idx];
        head = &s->free_arena_list[block_size_idx];
        el = head->next;
        if (unlikely(el == head)) {
            ar = js_malloc_new_arena(s, block_size_idx);
            if (!ar)
                return NULL;
        } else {
            ar = list_entry(el, JSMallocArena, free_link);
            if (ar->n_used_blocks == 0)      /* reusing a retained spare */
                s->n_empty_arenas[block_size_idx]--;
        }
        block_idx = ar->first_free_block;
        assert(block_idx < (unsigned int)ar->n_blocks);
        b = get_arena_block(ar, ar->first_free_block, block_size);
        ar->first_free_block = b->u.free_next;
        b->u.block_idx = block_idx;
        ar->n_used_blocks++;
        if (unlikely(ar->n_used_blocks == ar->n_blocks)) {
            list_del(&ar->free_link);
        }
#ifdef JS_MALLOC_USE_ITER
        ar->bitmap[block_idx / 32] |= 1U << (block_idx % 32);
#endif
        return b->user_data;
    } else {
        return js_malloc_large(s, size);
    }
}

static void __js_free(JSMallocContext *s, void *ptr)
{
    JSMallocBlockHeader *b;

    if (!ptr)
        return;
    b = container_of(ptr, JSMallocBlockHeader, user_data);
    if (unlikely(b->u.block_idx == FREE_NIL)) {
        /* large or zero size block */
        if (b == get_zero_size_block(s)) {
            /* nothing to do */
        } else {
            JSMallocLargeBlockHeader *lb =
                container_of(ptr, JSMallocLargeBlockHeader, header.user_data);
#ifdef JS_MALLOC_USE_ITER
            list_del(&lb->link);
#endif
            s->mf.js_free(&s->malloc_state, lb);
        }
    } else {
        unsigned int block_idx = b->u.block_idx;
        unsigned int block_size_idx = b->block_size_idx;
        unsigned int block_size = js_malloc_block_sizes[block_size_idx];
        /* Recover arena base from block pointer.  The arena was allocated as
           sizeof(JSMallocArena) + n_blocks * block_size, with blocks starting
           at ar->blocks.  This arithmetic must stay in sync with the layout
           used in js_malloc_new_arena. */
        JSMallocArena *ar = (JSMallocArena *)((uint8_t *)b -
                                               block_size * block_idx -
                                               sizeof(JSMallocArena));
        assert((uint8_t *)ar + sizeof(JSMallocArena) +
               block_size * block_idx == (uint8_t *)b);
        assert(block_idx < (unsigned int)ar->n_blocks);

        b->u.free_next = ar->first_free_block;
        ar->first_free_block = block_idx;
#ifdef JS_MALLOC_USE_ITER
        ar->bitmap[block_idx / 32] &= ~(1U << (block_idx % 32));
#endif
        /* add back to the free list if needed */
        if (unlikely(ar->n_used_blocks == ar->n_blocks)) {
            list_add(&ar->free_link, &s->free_arena_list[block_size_idx]);
        }
        ar->n_used_blocks--;
        if (unlikely(ar->n_used_blocks == 0)) {
            /* Keep a bounded number of empty arenas as spares (it stays on both
               lists, freelist intact, ready for immediate reuse) instead of
               returning it to the OS only to re-malloc+re-init it next alloc. */
            if (s->n_empty_arenas[block_size_idx] < JS_MALLOC_MAX_EMPTY_ARENAS) {
                s->n_empty_arenas[block_size_idx]++;
            } else {
                list_del(&ar->link);
                list_del(&ar->free_link);
                s->mf.js_free(&s->malloc_state, ar);
            }
        }
    }
}

static void *__js_realloc(JSMallocContext *s, void *ptr, size_t size)
{
    JSMallocBlockHeader *b;
    if (ptr == NULL) {
        return __js_malloc(s, size);
    } else if (size == 0) {
        __js_free(s, ptr);
        return NULL;
    }
    b = container_of(ptr, JSMallocBlockHeader, user_data);
    if (b->u.block_idx == FREE_NIL) {
        if (b == get_zero_size_block(s)) {
            return __js_malloc(s, size);
        } else {
            JSMallocLargeBlockHeader *lb, *new_lb;
            lb = container_of(ptr, JSMallocLargeBlockHeader, header.user_data);
#ifdef JS_MALLOC_USE_ITER
            list_del(&lb->link);
#endif
            if (unlikely(size > SIZE_MAX - sizeof(JSMallocLargeBlockHeader)))
                return NULL;
            new_lb = s->mf.js_realloc(&s->malloc_state, lb,
                                      sizeof(JSMallocLargeBlockHeader) + size);
            if (!new_lb) {
#ifdef JS_MALLOC_USE_ITER
                /* add again in the list */
                list_add_tail(&lb->link, &s->large_block_list);
#endif
                return NULL;
            }
            new_lb->header.u.block_idx = FREE_NIL;
            new_lb->header.block_size_idx = 0xff; /* fail safe */
#ifdef JS_MALLOC_USE_ITER
            list_add_tail(&new_lb->link, &s->large_block_list);
#endif
            return new_lb->header.user_data;
        }
    } else {
        unsigned int block_size_idx = b->block_size_idx;
        size_t block_size = js_malloc_block_sizes[block_size_idx];
        size_t total_size, old_size, copy_size;
        void *new_ptr;
        JSMallocBlockHeader *new_b;

        total_size = ((size + JS_MALLOC_ALIGN - 1) & ~(JS_MALLOC_ALIGN - 1)) +
            sizeof(JSMallocBlockHeader);
        if (total_size <= block_size)
            return ptr;
        new_ptr = __js_malloc(s, size);
        if (!new_ptr)
            return NULL;
        new_b = container_of(new_ptr, JSMallocBlockHeader, user_data);
        /* copy the GC data */
        new_b->gc_obj_type = b->gc_obj_type;
        new_b->mark = b->mark;
        new_b->ref_count = b->ref_count;
        /* copy the data */
        old_size = block_size - sizeof(JSMallocBlockHeader);
        copy_size = size < old_size ? size : old_size;
        memcpy(new_ptr, ptr, copy_size);
        __js_free(s, ptr);
        return new_ptr;
    }
}

static size_t __js_malloc_usable_size(JSMallocContext *s, const void *ptr)
{
    JSMallocBlockHeader *b;
    if (!ptr)
        return 0;
    b = container_of(ptr, JSMallocBlockHeader, user_data);
    if (b->u.block_idx == FREE_NIL) {
        if (b == get_zero_size_block(s)) {
            return 0;
        } else {
            JSMallocLargeBlockHeader *lb;
            size_t size;
            lb = container_of(ptr, JSMallocLargeBlockHeader, header.user_data);
            if (s->mf.js_malloc_usable_size) {
                size = s->mf.js_malloc_usable_size(lb);
                if (size != 0)
                    size -= sizeof(JSMallocLargeBlockHeader);
                return size;
            } else {
                return 0;
            }
        }
    } else {
        size_t block_size = js_malloc_block_sizes[b->block_size_idx];
        return block_size - sizeof(*b);
    }
}

static __maybe_unused void js_malloc_dump_arenas(JSMallocContext *s)
{
    struct list_head *el;
    int block_size_idx;

    printf("%20s %10s %10s\n", "PTR", "BLK_SIZE", "ALLOC");
    for (block_size_idx = 0; block_size_idx < JS_MALLOC_BLOCK_SIZE_COUNT;
         block_size_idx++) {
        int block_size = js_malloc_block_sizes[block_size_idx];
        list_for_each(el, &s->arena_list[block_size_idx]) {
            JSMallocArena *ar = list_entry(el, JSMallocArena, link);
            printf("%20p %10d %9.1f%%\n",
                   (void *)ar, block_size,
                   (double)ar->n_used_blocks / ar->n_blocks * 100.0);
        }
    }
}

#ifdef JS_MALLOC_USE_ITER
typedef void JSMallocIterFunc(void *opaque, void *ptr);

/* iterate thru allocated blocks. The allocated block list should not
   be modified while iterating. */
static __maybe_unused void js_malloc_iter(JSMallocContext *s,
                                          JSMallocIterFunc *iter_func,
                                          void *iter_opaque)
{
    struct list_head *el;
    int block_size_idx;
    int i, j, n_words;
    uint32_t bmp;

    for (block_size_idx = 0; block_size_idx < JS_MALLOC_BLOCK_SIZE_COUNT;
         block_size_idx++) {
        unsigned int block_size = js_malloc_block_sizes[block_size_idx];
        list_for_each(el, &s->arena_list[block_size_idx]) {
            JSMallocArena *ar = list_entry(el, JSMallocArena, link);
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
    list_for_each(el, &s->large_block_list) {
        JSMallocLargeBlockHeader *lb =
            list_entry(el, JSMallocLargeBlockHeader, link);
        iter_func(iter_opaque, lb->header.user_data);
    }
}
#endif

/* end JS malloc */

static void js_trigger_gc(JSRuntime *rt, size_t size)
{
    BOOL force_gc;
#ifdef FORCE_GC_AT_MALLOC
    force_gc = TRUE;
#else
    force_gc = ((rt->malloc_ctx.malloc_state.malloc_size + size) >
                rt->malloc_gc_threshold);
#endif
    if (force_gc) {
#ifdef DUMP_GC
        printf("GC: size=%" PRIu64 "\n",
               (uint64_t)rt->malloc_ctx.malloc_state.malloc_size);
#endif
        JS_RunGC(rt);
        {
            size_t cur = rt->malloc_ctx.malloc_state.malloc_size;
            size_t add = cur >> 1;
            rt->malloc_gc_threshold = (cur > SIZE_MAX - add) ? SIZE_MAX : cur + add;
        }
    }
}

void *js_malloc_rt(JSRuntime *rt, size_t size)
{
    return __js_malloc(&rt->malloc_ctx, size);
}

void js_free_rt(JSRuntime *rt, void *ptr)
{
#ifdef CONFIG_NURSERY_PROBE
    if (unlikely(js_nursery_probe_owned(ptr)))
        return; /* probe: never returned; chunk release happens at teardown */
#endif
    __js_free(&rt->malloc_ctx, ptr);
}

void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size)
{
#ifdef CONFIG_NURSERY_PROBE
    if (unlikely(js_nursery_probe_owned(ptr))) {
        /* escape to the heap: the copy-out the real design would do when a
           prop array outgrows the arena. Old block stays (arena reclaim). */
        void *np = js_malloc_rt(rt, size);
        uint32_t old_size;
        if (unlikely(!np))
            return NULL;
        old_size = *(const uint32_t *)(const void *)((const uint8_t *)ptr - 12);
        memcpy(np, ptr, size < (size_t)old_size ? size : (size_t)old_size);
        return np;
    }
#endif
    return __js_realloc(&rt->malloc_ctx, ptr, size);
}

size_t js_malloc_usable_size_rt(JSRuntime *rt, const void *ptr)
{
    return __js_malloc_usable_size(&rt->malloc_ctx, ptr);
}

void *js_mallocz_rt(JSRuntime *rt, size_t size)
{
    void *ptr;
    ptr = js_malloc_rt(rt, size);
    if (unlikely(!ptr))
        return NULL;
    return memset(ptr, 0, size);
}

/* Throw out of memory in case of error */
void *js_malloc(JSContext *ctx, size_t size)
{
    void *ptr;
    ptr = js_malloc_rt(ctx->rt, size);
    if (unlikely(!ptr)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ptr;
}

/* Throw out of memory in case of error */
void *js_mallocz(JSContext *ctx, size_t size)
{
    void *ptr;
    ptr = js_mallocz_rt(ctx->rt, size);
    if (unlikely(!ptr)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ptr;
}

void js_free(JSContext *ctx, void *ptr)
{
    js_free_rt(ctx->rt, ptr);
}

/* Throw out of memory in case of error */
void *js_realloc(JSContext *ctx, void *ptr, size_t size)
{
    void *ret;
    ret = js_realloc_rt(ctx->rt, ptr, size);
    if (unlikely(!ret && size != 0)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ret;
}

/* store extra allocated size in *pslack if successful */
void *js_realloc2(JSContext *ctx, void *ptr, size_t size, size_t *pslack)
{
    void *ret;
    ret = js_realloc_rt(ctx->rt, ptr, size);
    if (unlikely(!ret && size != 0)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    if (pslack) {
        size_t new_size = js_malloc_usable_size_rt(ctx->rt, ret);
        *pslack = (new_size > size) ? new_size - size : 0;
    }
    return ret;
}

size_t js_malloc_usable_size(JSContext *ctx, const void *ptr)
{
    return js_malloc_usable_size_rt(ctx->rt, ptr);
}

/* Throw out of memory exception in case of error */
char *js_strndup(JSContext *ctx, const char *s, size_t n)
{
    char *ptr;
    if (unlikely(n == SIZE_MAX))
        return NULL;
    ptr = js_malloc(ctx, n + 1);
    if (ptr) {
        memcpy(ptr, s, n);
        ptr[n] = '\0';
    }
    return ptr;
}

char *js_strdup(JSContext *ctx, const char *str)
{
    return js_strndup(ctx, str, strlen(str));
}

static no_inline int js_realloc_array(JSContext *ctx, void **parray,
                                      int elem_size, int *psize, int req_size)
{
    int new_size;
    int64_t grow, want;
    size_t slack, new_bytes;
    void *new_array;

    assert(elem_size > 0);
    /* compute growth in 64-bit and reject int/byte overflow */
    grow = (int64_t)*psize * 3 / 2;
    want = req_size > grow ? req_size : grow;
    /* first-touch reserve: growing from empty otherwise reallocs on nearly
       every add (1,2,3,4,6,9,…). Reserve a small floor so tiny arrays (the
       common per-function parser scaffolding) allocate once. */
    if (*psize == 0 && want < 8)
        want = 8;
    new_bytes = (size_t)want * (size_t)elem_size;
    if (want > INT32_MAX ||
        new_bytes / (size_t)elem_size != (size_t)want) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    new_size = (int)want;
    new_array = js_realloc2(ctx, *parray, new_bytes, &slack);
    if (!new_array)
        return -1;
    new_size += (int)(slack / (size_t)elem_size);
    *psize = new_size;
    *parray = new_array;
    return 0;
}

/* resize the array and update its size if req_size > *psize */
static inline int js_resize_array(JSContext *ctx, void **parray, int elem_size,
                                  int *psize, int req_size)
{
    if (unlikely(req_size > *psize))
        return js_realloc_array(ctx, parray, elem_size, psize, req_size);
    else
        return 0;
}

/* Correctly-typed DynBufReallocFunc adapter: calling js_realloc_rt (first arg
   JSRuntime*) through the DynBufReallocFunc type (first arg void*) is C UB and
   traps under -fsanitize=function / CFI / LTO. */
static void *js_realloc_dbuf_rt(void *opaque, void *ptr, size_t size)
{
    return js_realloc_rt((JSRuntime *)opaque, ptr, size);
}

static inline void js_dbuf_init(JSContext *ctx, DynBuf *s)
{
    dbuf_init2(s, ctx->rt, js_realloc_dbuf_rt);
}

static void *js_realloc_bytecode_rt(void *opaque, void *ptr, size_t size)
{
    JSRuntime *rt = opaque;
    if (size > (INT32_MAX / 2)) {
        /* the bytecode cannot be larger than 2G. Leave some slack to
           avoid some overflows. */
        return NULL;
    } else {
        return js_realloc_rt(rt, ptr, size);
    }
}

static inline void js_dbuf_bytecode_init(JSContext *ctx, DynBuf *s)
{
    dbuf_init2(s, ctx->rt, js_realloc_bytecode_rt);
}

static inline int is_digit(int c) {
    return c >= '0' && c <= '9';
}

/* Slice payload: a JSStringSlice block stored immediately before the
   JSString header (see the layout note in dynajs.c). Allocations for
   slices are JS_SLICE_PREFIX_SIZE + sizeof(JSString) bytes starting at the
   payload, so the payload address is naturally aligned and the effective
   type rules are satisfied (fresh malloc'd memory written as JSStringSlice). */
static inline const JSStringSlice *js_slice_payload(const JSString *p)
{
    return (const JSStringSlice *)((const uint8_t *)p - JS_SLICE_PREFIX_SIZE);
}

/* Data accessors: the single sanctioned way to obtain a character-data
   pointer from a JSString. Slices resolve to the parent allocation (which
   the slice keeps alive by reference), so the returned pointer is valid as
   long as the JSString is reachable and string data is never mutated in
   place (JS_ConcatStringInPlace refuses slices). Read-only consumers use
   these so the SIMD kernels and pointer-recovery arithmetic (e.g.
   (capture - js_str_data8(s)) >> shift in GetSubstitution) stay consistent.

   TERMINATION CAVEAT (do not zero-copy from a slice): a flat narrow string
   allocates len+1 bytes so str8[len] is always NUL, but a slice window
   does NOT -- str8[len] is simply the next character of the parent (or
   one past the parent's end, which has no NUL either). Any consumer that
   hands the pointer to a NUL-terminated C API must go through
   JS_ToCStringLen2, which refuses the zero-copy path for slices and
   materializes a terminated copy instead (see its `!str->is_slice`
   guard), and must free the result with JS_FreeCString. */
static inline const uint8_t *js_str_data8(const JSString *p) {
    if (unlikely(p->is_slice))
        return js_slice_parent(p)->u.str8 + js_slice_offset(p);
    return p->u.str8;
}

static inline const uint16_t *js_str_data16(const JSString *p) {
    if (unlikely(p->is_slice))
        return js_slice_parent(p)->u.str16 + js_slice_offset(p);
    return p->u.str16;
}

/* Base pointer for the regexp engine's byte addressing and for capture
   recovery ((capture - base) >> is_wide_char): the bytes live at the
   uint16 view for wide strings and at the byte view for narrow ones. For
   flat strings the union makes both identical, but a slice's offset is in
   characters, so the view MUST match is_wide_char or the base is off by
   (offset bytes vs offset units). */
static inline const uint8_t *js_str_data_units(const JSString *p) {
    return p->is_wide_char ? (const uint8_t *)js_str_data16(p)
                           : (const uint8_t *)js_str_data8(p);
}

static inline int string_get(const JSString *p, int idx) {
    return p->is_wide_char ? js_str_data16(p)[idx] : js_str_data8(p)[idx];
}

typedef struct JSClassShortDef {
    JSAtom class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
} JSClassShortDef;

static JSClassShortDef const js_std_class_def[] = {
    { JS_ATOM_Object, NULL, NULL },                             /* JS_CLASS_OBJECT */
    { JS_ATOM_Array, js_array_finalizer, js_array_mark },       /* JS_CLASS_ARRAY */
    { JS_ATOM_Error, NULL, NULL }, /* JS_CLASS_ERROR */
    { JS_ATOM_Number, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_NUMBER */
    { JS_ATOM_String, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_STRING */
    { JS_ATOM_Boolean, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_BOOLEAN */
    { JS_ATOM_Symbol, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_SYMBOL */
    { JS_ATOM_Arguments, js_array_finalizer, js_array_mark },   /* JS_CLASS_ARGUMENTS */
    { JS_ATOM_Arguments, js_mapped_arguments_finalizer, js_mapped_arguments_mark }, /* JS_CLASS_MAPPED_ARGUMENTS */
    { JS_ATOM_Date, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_DATE */
    { JS_ATOM_Object, NULL, NULL },                             /* JS_CLASS_MODULE_NS */
    { JS_ATOM_Function, js_c_function_finalizer, js_c_function_mark }, /* JS_CLASS_C_FUNCTION */
    { JS_ATOM_Function, js_bytecode_function_finalizer, js_bytecode_function_mark }, /* JS_CLASS_BYTECODE_FUNCTION */
    { JS_ATOM_Function, js_bound_function_finalizer, js_bound_function_mark }, /* JS_CLASS_BOUND_FUNCTION */
    { JS_ATOM_Function, js_c_function_data_finalizer, js_c_function_data_mark }, /* JS_CLASS_C_FUNCTION_DATA */
    { JS_ATOM_GeneratorFunction, js_bytecode_function_finalizer, js_bytecode_function_mark },  /* JS_CLASS_GENERATOR_FUNCTION */
    { JS_ATOM_ForInIterator, js_for_in_iterator_finalizer, js_for_in_iterator_mark },      /* JS_CLASS_FOR_IN_ITERATOR */
    { JS_ATOM_RegExp, js_regexp_finalizer, NULL },                              /* JS_CLASS_REGEXP */
    { JS_ATOM_ArrayBuffer, js_array_buffer_finalizer, NULL },                   /* JS_CLASS_ARRAY_BUFFER */
    { JS_ATOM_SharedArrayBuffer, js_array_buffer_finalizer, NULL },             /* JS_CLASS_SHARED_ARRAY_BUFFER */
    { JS_ATOM_Uint8ClampedArray, js_typed_array_finalizer, js_typed_array_mark }, /* JS_CLASS_UINT8C_ARRAY */
    { JS_ATOM_Int8Array, js_typed_array_finalizer, js_typed_array_mark },       /* JS_CLASS_INT8_ARRAY */
    { JS_ATOM_Uint8Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_UINT8_ARRAY */
    { JS_ATOM_Int16Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_INT16_ARRAY */
    { JS_ATOM_Uint16Array, js_typed_array_finalizer, js_typed_array_mark },     /* JS_CLASS_UINT16_ARRAY */
    { JS_ATOM_Int32Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_INT32_ARRAY */
    { JS_ATOM_Uint32Array, js_typed_array_finalizer, js_typed_array_mark },     /* JS_CLASS_UINT32_ARRAY */
    { JS_ATOM_BigInt64Array, js_typed_array_finalizer, js_typed_array_mark },   /* JS_CLASS_BIG_INT64_ARRAY */
    { JS_ATOM_BigUint64Array, js_typed_array_finalizer, js_typed_array_mark },  /* JS_CLASS_BIG_UINT64_ARRAY */
    { JS_ATOM_Float16Array, js_typed_array_finalizer, js_typed_array_mark },    /* JS_CLASS_FLOAT16_ARRAY */
    { JS_ATOM_Float32Array, js_typed_array_finalizer, js_typed_array_mark },    /* JS_CLASS_FLOAT32_ARRAY */
    { JS_ATOM_Float64Array, js_typed_array_finalizer, js_typed_array_mark },    /* JS_CLASS_FLOAT64_ARRAY */
    { JS_ATOM_DataView, js_typed_array_finalizer, js_typed_array_mark },        /* JS_CLASS_DATAVIEW */
    { JS_ATOM_BigInt, js_object_data_finalizer, js_object_data_mark },      /* JS_CLASS_BIG_INT */
    { JS_ATOM_Map, js_map_finalizer, js_map_mark },             /* JS_CLASS_MAP */
    { JS_ATOM_Set, js_map_finalizer, js_map_mark },             /* JS_CLASS_SET */
    { JS_ATOM_WeakMap, js_map_finalizer, js_map_mark },         /* JS_CLASS_WEAKMAP */
    { JS_ATOM_WeakSet, js_map_finalizer, js_map_mark },         /* JS_CLASS_WEAKSET */
    { JS_ATOM_Iterator, NULL, NULL },                           /* JS_CLASS_ITERATOR */
    { JS_ATOM_IteratorConcat, js_iterator_concat_finalizer, js_iterator_concat_mark }, /* JS_CLASS_ITERATOR_CONCAT */
    { JS_ATOM_IteratorZip, js_iterator_zip_finalizer, js_iterator_zip_mark }, /* JS_CLASS_ITERATOR_ZIP */
    { JS_ATOM_IteratorHelper, js_iterator_helper_finalizer, js_iterator_helper_mark }, /* JS_CLASS_ITERATOR_HELPER */
    { JS_ATOM_IteratorWrap, js_iterator_wrap_finalizer, js_iterator_wrap_mark }, /* JS_CLASS_ITERATOR_WRAP */
    { JS_ATOM_Map_Iterator, js_map_iterator_finalizer, js_map_iterator_mark }, /* JS_CLASS_MAP_ITERATOR */
    { JS_ATOM_Set_Iterator, js_map_iterator_finalizer, js_map_iterator_mark }, /* JS_CLASS_SET_ITERATOR */
    { JS_ATOM_Array_Iterator, js_array_iterator_finalizer, js_array_iterator_mark }, /* JS_CLASS_ARRAY_ITERATOR */
    { JS_ATOM_String_Iterator, js_array_iterator_finalizer, js_array_iterator_mark }, /* JS_CLASS_STRING_ITERATOR */
    { JS_ATOM_RegExp_String_Iterator, js_regexp_string_iterator_finalizer, js_regexp_string_iterator_mark }, /* JS_CLASS_REGEXP_STRING_ITERATOR */
    { JS_ATOM_Generator, js_generator_finalizer, js_generator_mark }, /* JS_CLASS_GENERATOR */
    { JS_ATOM_Object, js_global_object_finalizer, js_global_object_mark }, /* JS_CLASS_GLOBAL_OBJECT */
    { JS_ATOM_Object, NULL, NULL }, /* JS_CLASS_RAWJSON */
};

static int init_class_range(JSRuntime *rt, JSClassShortDef const *tab,
                            int start, int count)
{
    JSClassDef cm_s, *cm = &cm_s;
    int i, class_id;

    for(i = 0; i < count; i++) {
        class_id = i + start;
        memset(cm, 0, sizeof(*cm));
        cm->finalizer = tab[i].finalizer;
        cm->gc_mark = tab[i].gc_mark;
        if (JS_NewClass1(rt, class_id, cm, tab[i].class_name) < 0)
            return -1;
    }
    return 0;
}

#if !defined(CONFIG_STACK_CHECK)
/* no stack limitation */
static inline uintptr_t js_get_stack_pointer(void)
{
    return 0;
}

static inline BOOL js_check_stack_overflow(JSRuntime *rt, size_t alloca_size)
{
    return FALSE;
}
#else
/* Note: OS and CPU dependent */
static inline uintptr_t js_get_stack_pointer(void)
{
    return (uintptr_t)__builtin_frame_address(0);
}

static inline BOOL js_check_stack_overflow(JSRuntime *rt, size_t alloca_size)
{
    uintptr_t sp;
    sp = js_get_stack_pointer() - alloca_size;
    return unlikely(sp < rt->stack_limit);
}
#endif

JSRuntime *JS_NewRuntime2(const JSMallocFunctions *mf, void *opaque)
{
    JSRuntime *rt;
    JSMallocState ms;

    memset(&ms, 0, sizeof(ms));
    ms.opaque = opaque;
    ms.malloc_limit = -1;

    /* install the best-available SIMD kernels once (idempotent, pthread_once). */
    simd_init();

    rt = mf->js_malloc(&ms, sizeof(JSRuntime));
    if (!rt)
        return NULL;
    memset(rt, 0, sizeof(*rt));
    js_malloc_init(&rt->malloc_ctx);
    rt->malloc_ctx.mf = *mf;
    rt->malloc_ctx.malloc_state = ms;
    rt->malloc_gc_threshold = 256 * 1024;
    {
        /* DYNAJS_MALLOC_POOLS=0|off|no: bypass the small-block arenas so
           every engine allocation reaches the host allocator (see the
           no_pools field's note in dynajs.c). Resolved ONCE here, before
           any script runs. */
        const char *mp = getenv("DYNAJS_MALLOC_POOLS");
        if (mp && (strcmp(mp, "0") == 0 || strcasecmp(mp, "off") == 0
                   || strcasecmp(mp, "no") == 0))
            rt->malloc_ctx.no_pools = 1;
    }

    init_list_head(&rt->context_list);
    init_list_head(&rt->gc_obj_list);
    init_list_head(&rt->gc_zero_ref_count_list);
    rt->gc_phase = JS_GC_PHASE_NONE;
    init_list_head(&rt->weakref_list);

#ifdef DUMP_LEAKS
    init_list_head(&rt->string_list);
#endif
    init_list_head(&rt->job_list);
    init_list_head(&rt->shutdown_sweeps);
    init_list_head(&rt->shutdown_deferred);

    if (JS_InitAtoms(rt))
        goto fail;

    /* create the object, array and function classes */
    if (init_class_range(rt, js_std_class_def, JS_CLASS_OBJECT,
                         countof(js_std_class_def)) < 0)
        goto fail;
    rt->class_array[JS_CLASS_ARGUMENTS].exotic = &js_arguments_exotic_methods;
    rt->class_array[JS_CLASS_MAPPED_ARGUMENTS].exotic = &js_arguments_exotic_methods;
    rt->class_array[JS_CLASS_STRING].exotic = &js_string_exotic_methods;
    rt->class_array[JS_CLASS_MODULE_NS].exotic = &js_module_ns_exotic_methods;

    rt->class_array[JS_CLASS_C_FUNCTION].call = js_call_c_function;
    rt->class_array[JS_CLASS_C_FUNCTION_DATA].call = js_c_function_data_call;
    rt->class_array[JS_CLASS_BOUND_FUNCTION].call = js_call_bound_function;
    rt->class_array[JS_CLASS_GENERATOR_FUNCTION].call = js_generator_function_call;
    if (init_shape_hash(rt))
        goto fail;

    rt->stack_size = JS_DEFAULT_STACK_SIZE;
    JS_UpdateStackTop(rt);

    rt->current_exception = JS_UNINITIALIZED;

    return rt;
 fail:
    JS_FreeRuntime(rt);
    return NULL;
}

void *JS_GetRuntimeOpaque(JSRuntime *rt)
{
    return rt->user_opaque;
}

void JS_SetRuntimeOpaque(JSRuntime *rt, void *opaque)
{
    rt->user_opaque = opaque;
}

/* default memory allocation functions with memory limitation */
static size_t js_def_malloc_usable_size(const void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void *)ptr);
#elif defined(__EMSCRIPTEN__)
    return 0;
#elif defined(__linux__) || defined(__GLIBC__)
    return malloc_usable_size((void *)ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size((void *)ptr);
#endif
}

static void *js_def_malloc(JSMallocState *s, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    if (unlikely(size == 0))
        size = 1;

    /* Check limit without wrapping malloc_size.  malloc_limit == -1 means
       unlimited (SIZE_MAX), so skip the check in the common case. */
    if (unlikely(s->malloc_limit != (size_t)-1 &&
                 (size > s->malloc_limit || s->malloc_size > s->malloc_limit - size)))
        return NULL;

    ptr = malloc(size);
    if (!ptr)
        return NULL;

    s->malloc_count++;
    s->malloc_size += js_def_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_def_free(JSMallocState *s, void *ptr)
{
    size_t old_size;

    if (!ptr)
        return;

    old_size = js_def_malloc_usable_size(ptr);
    assert(s->malloc_count > 0);
    assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    free(ptr);
}

static void *js_def_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_def_malloc(s, size);
    }
    old_size = js_def_malloc_usable_size(ptr);
    if (size == 0) {
        assert(s->malloc_count > 0);
        assert(s->malloc_size >= old_size + MALLOC_OVERHEAD);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    /* Avoid unsigned underflow / wrap when checking limit.  We check against
       the requested delta; the actual delta may differ (usable size rounding)
       but this is intentionally conservative. */
    if (s->malloc_limit != (size_t)-1 && size > old_size) {
        size_t delta = size - old_size;
        if (unlikely(delta > s->malloc_limit ||
                     s->malloc_size > s->malloc_limit - delta))
            return NULL;
    }

    ptr = realloc(ptr, size);
    if (!ptr)
        return NULL;

    s->malloc_size += js_def_malloc_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions def_malloc_funcs = {
    js_def_malloc,
    js_def_free,
    js_def_realloc,
    js_def_malloc_usable_size,
};

#ifdef CONFIG_SCL_ALLOC
/* Route the runtime's backing allocations through a secure-c-libs allocator
   (per-runtime state in JSMallocState.opaque; no global allocator state).
   ABI-compatible local view of scl_allocator_t (see
   secure-c-libs/libs/core/scl_common.h) so the engine TU need not include the
   scl headers. The default scl allocator forwards to libc, so pointers remain
   libc pointers and js_def_malloc_usable_size stays valid for accounting and
   for passing an accurate old_size to scl realloc. */
typedef struct js_scl_allocator {
    void *(*malloc_fn)(void *state, size_t size, size_t alignment);
    void *(*calloc_fn)(void *state, size_t count, size_t size, size_t alignment);
    void *(*realloc_fn)(void *state, void *ptr, size_t old_size,
                        size_t new_size, size_t alignment);
    void (*free_fn)(void *state, void *ptr);
    void *state;
} js_scl_allocator;

extern js_scl_allocator *scl_allocator_default(void); /* from libscl.a */

static void *js_scl_malloc(JSMallocState *s, size_t size)
{
    js_scl_allocator *a = s->opaque;
    void *ptr;

    if (unlikely(size == 0))
        size = 1;
    if (unlikely(s->malloc_limit != (size_t)-1 &&
                 (size > s->malloc_limit || s->malloc_size > s->malloc_limit - size)))
        return NULL;
    ptr = a->malloc_fn(a->state, size, 0);
    if (!ptr)
        return NULL;
    s->malloc_count++;
    s->malloc_size += js_def_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_scl_free(JSMallocState *s, void *ptr)
{
    js_scl_allocator *a = s->opaque;
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

static void *js_scl_realloc(JSMallocState *s, void *ptr, size_t size)
{
    js_scl_allocator *a = s->opaque;
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
    if (s->malloc_limit != (size_t)-1 && size > old_size) {
        size_t delta = size - old_size;
        if (unlikely(delta > s->malloc_limit ||
                     s->malloc_size > s->malloc_limit - delta))
            return NULL;
    }
    /* Contract: on failure realloc_fn must return NULL and leave the old block
       intact (standard C realloc semantics), because the engine keeps using
       `ptr` after a failed grow. The default scl allocator (libc realloc)
       honors this; a backend that frees-on-failure (e.g. scl built with
       SCL_ALLOC_MAX_SIZE below the engine's needs, or a future arena/tlsf that
       reclaims on error) would leave the caller a dangling pointer -- such a
       backend must not be used here, or this path needs a copy-based grow. */
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
#endif /* CONFIG_SCL_ALLOC */

#ifdef CONFIG_MIMALLOC
/* Route the runtime's backing allocations through mimalloc v3 (github.com/
   microsoft/mimalloc, vendored under third_party/mimalloc). Per-runtime
   accounting stays valid because mi_usable_size() reports the real block size.
   Experimental, opt-in (CONFIG_MIMALLOC=y). */
#include "mimalloc.h"

static size_t js_mi_malloc_usable_size(const void *ptr)
{
    return mi_usable_size((void *)ptr);
}

static void *js_mi_malloc(JSMallocState *s, size_t size)
{
    void *ptr;
    if (unlikely(size == 0))
        size = 1;
    if (unlikely(s->malloc_limit != (size_t)-1 &&
                 (size > s->malloc_limit || s->malloc_size > s->malloc_limit - size)))
        return NULL;
    ptr = mi_malloc(size);
    if (!ptr)
        return NULL;
    s->malloc_count++;
    s->malloc_size += mi_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_mi_free(JSMallocState *s, void *ptr)
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

static void *js_mi_realloc(JSMallocState *s, void *ptr, size_t size)
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
    if (s->malloc_limit != (size_t)-1 && size > old_size) {
        size_t delta = size - old_size;
        if (unlikely(delta > s->malloc_limit ||
                     s->malloc_size > s->malloc_limit - delta))
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

/* Curated engine defaults + a DYNA_MI_* env override layer for the tunables that
 * matter most for a JS runtime (mimalloc also reads its own MIMALLOC_* env vars
 * natively). purge_delay ms trades RSS (lower = return memory to the OS sooner)
 * against throughput; large/huge OS pages trade startup + RSS against TLB perf. */
static void js_mimalloc_configure(void)
{
    const char *e;
    if ((e = getenv("DYNA_MI_PURGE_DELAY")))     mi_option_set(mi_option_purge_delay, atoi(e));
    if ((e = getenv("DYNA_MI_PURGE_DECOMMITS"))) mi_option_set(mi_option_purge_decommits, atoi(e));
    if ((e = getenv("DYNA_MI_EAGER_COMMIT")))    mi_option_set(mi_option_eager_commit, atoi(e));
    if ((e = getenv("DYNA_MI_LARGE_OS_PAGES")))  mi_option_set(mi_option_allow_large_os_pages, atoi(e));
    if ((e = getenv("DYNA_MI_RESERVE_HUGE")))    mi_option_set(mi_option_reserve_huge_os_pages, atoi(e));
    if ((e = getenv("DYNA_MI_ARENA_RESERVE")))   mi_option_set(mi_option_arena_reserve, atoi(e));
}
#endif /* CONFIG_MIMALLOC */

JSRuntime *JS_NewRuntime(void)
{
#if defined(CONFIG_MIMALLOC)
    js_mimalloc_configure();
    return JS_NewRuntime2(&mi_malloc_funcs, NULL);
#elif defined(CONFIG_SCL_ALLOC)
    return JS_NewRuntime2(&scl_malloc_funcs, scl_allocator_default());
#else
    return JS_NewRuntime2(&def_malloc_funcs, NULL);
#endif
}

void JS_SetMemoryLimit(JSRuntime *rt, size_t limit)
{
    rt->malloc_ctx.malloc_state.malloc_limit = limit;
}

/* use -1 to disable automatic GC */
void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold)
{
    rt->malloc_gc_threshold = gc_threshold;
}

/* libc free(), captured BEFORE the block below forbids the name: the
   shutdown-deferred shells are plain calloc'd module structs (never JS
   allocator blocks) and are released at the very end of JS_FreeRuntime. */
static void (*const js_libc_free)(void *) = free;

#define malloc(s) malloc_is_forbidden(s)
#define free(p) free_is_forbidden(p)
#define realloc(p,s) realloc_is_forbidden(p,s)

void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque)
{
    rt->interrupt_handler = cb;
    rt->interrupt_opaque = opaque;
}

void JS_SetCanBlock(JSRuntime *rt, BOOL can_block)
{
    rt->can_block = can_block;
}

void JS_SetSharedArrayBufferFunctions(JSRuntime *rt,
                                      const JSSharedArrayBufferFunctions *sf)
{
    rt->sab_funcs = *sf;
}

void JS_SetStripInfo(JSRuntime *rt, int flags)
{
    rt->strip_flags = flags;
}

int JS_GetStripInfo(JSRuntime *rt)
{
    return rt->strip_flags;
}

static int JS_EnqueueJob2(JSContext *ctx, JSJobFunc *job_func,
                          int argc, JSValueConst *argv, BOOL no_exception)
{
    JSRuntime *rt = ctx->rt;
    JSJobEntry *e;
    int i;

    if (no_exception)
        e = js_malloc_rt(ctx->rt, sizeof(*e) + argc * sizeof(JSValue));
    else
        e = js_malloc(ctx, sizeof(*e) + argc * sizeof(JSValue));
    if (!e)
        return -1;
    e->realm = JS_DupContext(ctx);
    e->job_func = job_func;
    e->argc = argc;
    for(i = 0; i < argc; i++) {
        e->argv[i] = JS_DupValue(ctx, argv[i]);
    }
    list_add_tail(&e->link, &rt->job_list);
    return 0;
}

/* return 0 if OK, < 0 if exception */
int JS_EnqueueJob(JSContext *ctx, JSJobFunc *job_func,
                  int argc, JSValueConst *argv)
{
    return JS_EnqueueJob2(ctx, job_func, argc, argv, FALSE);
}

BOOL JS_IsJobPending(JSRuntime *rt)
{
    return !list_empty(&rt->job_list);
}

/* return < 0 if exception, 0 if no job pending, 1 if a job was
   executed successfully. The context of the job is stored in '*pctx'
   if pctx != NULL. It may be NULL if the context was already
   destroyed or if no job was pending. The 'pctx' parameter is now
   absolete. */
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx)
{
    JSContext *ctx;
    JSJobEntry *e;
    JSValue res;
    int i, ret;

    if (list_empty(&rt->job_list)) {
        if (pctx)
            *pctx = NULL;
        return 0;
    }

    /* get the first pending job and execute it */
    e = list_entry(rt->job_list.next, JSJobEntry, link);
    list_del(&e->link);
    ctx = e->realm;
    res = e->job_func(ctx, e->argc, (JSValueConst *)e->argv);
    for(i = 0; i < e->argc; i++)
        JS_FreeValue(ctx, e->argv[i]);
    if (JS_IsException(res))
        ret = -1;
    else
        ret = 1;
    JS_FreeValue(ctx, res);
    js_free(ctx, e);
    if (pctx) {
        if (js_rc(ctx)->ref_count > 1)
            *pctx = ctx;
        else
            *pctx = NULL;
    }
    JS_FreeContext(ctx);
    return ret;
}

static inline uint32_t atom_get_free(const JSAtomStruct *p)
{
    return (uintptr_t)p >> 1;
}

static inline BOOL atom_is_free(const JSAtomStruct *p)
{
    return (uintptr_t)p & 1;
}

static inline JSAtomStruct *atom_set_free(uint32_t v)
{
    return (JSAtomStruct *)(((uintptr_t)v << 1) | 1);
}

/* Note: the string contents are uninitialized */
static JSString *js_alloc_string_rt(JSRuntime *rt, int max_len, int is_wide_char)
{
    JSString *str;
    size_t alloc_size;
    if (unlikely((uint32_t)max_len > JS_STRING_LEN_MAX))
        return NULL; /* len field is 30 bits; JS_STRING_LEN_MAX was already
                        the effective cap at every concat/buffer site */
    alloc_size = sizeof(JSString) + ((size_t)max_len << is_wide_char);
    if (is_wide_char == 0)
        alloc_size += 1;
    if (unlikely(alloc_size < sizeof(JSString)))
        return NULL;
    str = js_malloc_rt(rt, alloc_size);
    if (unlikely(!str))
        return NULL;
    js_rc(str)->ref_count = 1;
    str->is_wide_char = is_wide_char;
    str->is_slice = 0;
    str->len = max_len;
    str->atom_type = 0;
    str->hash = 0;          /* optional but costless */
    str->hash_next = 0;     /* optional */
#ifdef DUMP_LEAKS
    list_add_tail(&str->link, &rt->string_list);
#endif
    return str;
}

static JSString *js_alloc_string(JSContext *ctx, int max_len, int is_wide_char)
{
    JSString *p;
    p = js_alloc_string_rt(ctx->rt, max_len, is_wide_char);
    if (unlikely(!p)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return p;
}

/* same as JS_FreeValueRT() but faster */
static inline void js_free_string(JSRuntime *rt, JSString *str)
{
    if (--js_rc(str)->ref_count <= 0) {
        if (str->atom_type) {
            JS_FreeAtomStruct(rt, str);
        } else if (str->is_slice) {
            /* the slice holds one reference on its flat parent (a value
               string, or a non-constant atom whose eventual unlink from
               the weak intern table happens right here) — the parent can
               never own a reference on the slice, so this recursion is
               exactly one level deep. For CONSTANT atom parents
               (slice hash_next bit 0 clear) no reference was taken at
               creation (constant atoms are never dup'd: JS_DupAtom and
               JS_FreeAtom are no-ops for them), so no decrement here. */
            JSString *parent = js_slice_parent(str);
            int parent_holds_ref = str->hash_next & 1;
#ifdef DUMP_LEAKS
            list_del(&str->link);
#endif
            js_free_rt(rt, (uint8_t *)str - JS_SLICE_PREFIX_SIZE);
            if (parent_holds_ref)
                js_free_string(rt, parent);
        } else {
#ifdef DUMP_LEAKS
            list_del(&str->link);
#endif
            js_free_rt(rt, str);
        }
    }
}

void JS_SetRuntimeInfo(JSRuntime *rt, const char *s)
{
    if (rt)
        rt->rt_info = s;
}

static void js_run_shutdown_sweeps(JSContext *ctx, JSRuntime *rt);

typedef struct JSShutdownSweepEntry {
    struct list_head link;
    JSShutdownSweepFunc func;
    void *opaque;
} JSShutdownSweepEntry;

typedef struct JSShutdownDeferEntry {
    struct list_head link;
    void *ptr;
} JSShutdownDeferEntry;

void JS_FreeRuntime(JSRuntime *rt)
{
    struct list_head *el, *el1;
    int i;

    JS_FreeValueRT(rt, rt->current_exception);

    /* Shutdown sweeps, FIRST: C function objects hold context refs, so the
       CLI's JS_FreeContext is usually an early return and the REAL last
       context teardown happens inside the GC below -- too late to fail
       parked operations while JS is legal. At this depth every surviving
       context is still fully usable (nothing has been torn down), so the
       sweeps run here with the first of them; contexts already torn down
       ran their own sweeps at the JS_FreeContext hook. */
    if (!list_empty(&rt->context_list))
        js_run_shutdown_sweeps(list_entry(rt->context_list.next,
                                          JSContext, link), rt);
    else
        js_run_shutdown_sweeps(NULL, rt);

    list_for_each_safe(el, el1, &rt->job_list) {
        JSJobEntry *e = list_entry(el, JSJobEntry, link);
        for(i = 0; i < e->argc; i++)
            JS_FreeValueRT(rt, e->argv[i]);
        JS_FreeContext(e->realm);
        js_free_rt(rt, e);
    }
    init_list_head(&rt->job_list);

    /* don't remove the weak objects to avoid create new jobs with
       FinalizationRegistry */
    JS_RunGCInternal(rt, FALSE);

    /* Shutdown sweep stragglers: a park made by a finalizer during the GC
       above is released here WITHOUT settling -- JS is no longer legal at
       this depth. (Drain-time stragglers were already released by the
       re-check at the end of the pass above.) The normal case is an empty
       registry. */
    js_run_shutdown_sweeps(NULL, rt);

    /* One more GC: a straggler release drops pins whose graph (promise /
       reaction / closure / realm cycles) only a GC can collect, and the
       runtime's GC above has already run. Without this pass those objects
       are still in gc_obj_list at the assertion below -- the teardown abort
       (exit 134) that any drain-time re-park used to cause. */
    JS_RunGCInternal(rt, FALSE);

#ifdef DUMP_LEAKS
    /* leaking objects */
    {
        BOOL header_done;
        JSGCObjectHeader *p;
        int count;

        /* remove the internal refcounts to display only the object
           referenced externally */
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            js_rc(p)->mark = 0;
        }
        gc_decref(rt);

        header_done = FALSE;
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            if (js_rc(p)->ref_count != 0) {
                if (!header_done) {
                    printf("Object leaks:\n");
                    JS_DumpObjectHeader(rt);
                    header_done = TRUE;
                }
                JS_DumpGCObject(rt, p);
            }
        }

        count = 0;
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            if (js_rc(p)->ref_count == 0) {
                count++;
            }
        }
        if (count != 0)
            printf("Secondary object leaks: %d\n", count);
    }
#endif
    if (!list_empty(&rt->gc_obj_list) && getenv("DYNA_LEAK_DUMP")) {
        struct list_head *el;
        int n = 0;
        fprintf(stderr, "---- leak dump ----\n");
        list_for_each(el, &rt->gc_obj_list) {
            JSGCObjectHeader *p = list_entry(el, JSGCObjectHeader, link);
            fprintf(stderr, "LEAK rc=%d: ", js_rc(p)->ref_count);
            JS_DumpGCObject(rt, p);
            if (++n > 30) break;
        }
    }
    assert(list_empty(&rt->gc_obj_list));
    assert(list_empty(&rt->weakref_list));

    /* Shutdown-deferred shells (JS_ShutdownDeferFree): dead native structs
       kept alive past the sweeps because a stale settle may still have
       named them. No JS, job or settle can run past this point -- the
       last legal user of each shell is long gone -- so they are freed now,
       before the allocator teardown, with plain free(). */
    {
        struct list_head *el, *el1;
        list_for_each_safe(el, el1, &rt->shutdown_deferred) {
            JSShutdownDeferEntry *e = list_entry(el, JSShutdownDeferEntry,
                                                 link);
            list_del(&e->link);
            js_libc_free(e->ptr);
            js_free_rt(rt, e);
        }
    }

    /* free the classes */
    for(i = 0; i < rt->class_count; i++) {
        JSClass *cl = &rt->class_array[i];
        if (cl->class_id != 0) {
            JS_FreeAtomRT(rt, cl->class_name);
        }
    }
    js_free_rt(rt, rt->class_array);

    /* release the cached typeof result strings */
    for(i = 0; i < (int)countof(rt->typeof_strings); i++)
        JS_FreeValueRT(rt, rt->typeof_strings[i]);

    /* release the latin1 single-character string cache. Entries that were
       interned as atoms (JS_NewAtomStr donates the caller's reference to
       the atom table) are owned by the atom table now and are freed with
       the other atoms below; do not free them here. */
    for(i = 0; i < (int)countof(rt->latin1_char_cache); i++) {
        JSString *str = rt->latin1_char_cache[i];
        if (str && str->atom_type == 0) {
            if (--js_rc(str)->ref_count <= 0) {
#ifdef DUMP_LEAKS
                list_del(&str->link);
#endif
                js_free_rt(rt, str);
            }
        }
    }

#ifdef DUMP_LEAKS
    /* only the atoms defined in JS_InitAtoms() should be left */
    {
        BOOL header_done = FALSE;

        for(i = 0; i < rt->atom_size; i++) {
            JSAtomStruct *p = rt->atom_array[i];
            if (!atom_is_free(p) /* && p->str*/) {
                if (i >= JS_ATOM_END || js_rc(p)->ref_count != 1) {
                    if (!header_done) {
                        header_done = TRUE;
                        if (rt->rt_info) {
                            printf("%s:1: atom leakage:", rt->rt_info);
                        } else {
                            printf("Atom leaks:\n"
                                   "    %6s %6s %s\n",
                                   "ID", "REFCNT", "NAME");
                        }
                    }
                    if (rt->rt_info) {
                        printf(" ");
                    } else {
                        printf("    %6u %6u ", i, js_rc(p)->ref_count);
                    }
                    switch (p->atom_type) {
                    case JS_ATOM_TYPE_STRING:
                        JS_DumpString(rt, p);
                        break;
                    case JS_ATOM_TYPE_GLOBAL_SYMBOL:
                        printf("Symbol.for(");
                        JS_DumpString(rt, p);
                        printf(")");
                        break;
                    case JS_ATOM_TYPE_SYMBOL:
                        if (p->hash != JS_ATOM_HASH_PRIVATE) {
                            printf("Symbol(");
                            JS_DumpString(rt, p);
                            printf(")");
                        } else {
                            printf("Private(");
                            JS_DumpString(rt, p);
                            printf(")");
                        }
                        break;
                    }
                    if (rt->rt_info) {
                        printf(":%u", js_rc(p)->ref_count);
                    } else {
                        printf("\n");
                    }
                }
            }
        }
        if (rt->rt_info && header_done)
            printf("\n");
    }
#endif

#ifdef CONFIG_NURSERY_PROBE
    /* release the diagnostic nursery chunks (nothing inside is live at
       teardown: JS_FreeRuntime runs after all contexts/objects are gone) */
    js_nursery_probe_release_all(rt);
#endif

    /* drain the per-Date field-cache freelist (see js_date_cache_free) */
    {
        JSDateFieldsCache *fc = rt->date_cache_free_list, *nx;
        for (; fc != NULL; fc = nx) {
            nx = fc->next_free;
            js_free_rt(rt, fc);
        }
        rt->date_cache_free_list = NULL;
    }

#ifdef CONFIG_OBJ_POOL
    /* drain the CONFIG_OBJ_POOL recycle caches (see js_obj_pool_free /
       js_array_pool_free). Pooled blocks are still "live" to the arena
       allocator (their JSMallocBlockHeader is intact) and must be
       released like ordinary allocations at teardown. */
    {
        struct JSObject *p = rt->obj_pool_free_list, *nx;
        while (p != NULL) {
            nx = *(struct JSObject **)p; /* freelist link: header.link.next */
            js_free_rt(rt, p);
            p = nx;
        }
        rt->obj_pool_free_list = NULL;
        rt->obj_pool_count = 0;
    }
    {
        int b;
        for (b = 0; b < JS_ARRAY_POOL_NBUCKETS; b++) {
            JSValue *tab = (JSValue *)rt->array_pool_free_list[b], *nx;
            while (tab != NULL) {
                nx = *(JSValue **)tab; /* freelist link: values[0] */
                js_free_rt(rt, tab);
                tab = nx;
            }
            rt->array_pool_free_list[b] = NULL;
        }
        rt->array_pool_count = 0;
        rt->array_pool_slots = 0;
    }
#endif

    /* free the atoms */
    for(i = 0; i < rt->atom_size; i++) {
        JSAtomStruct *p = rt->atom_array[i];
        if (!atom_is_free(p)) {
#ifdef DUMP_LEAKS
            list_del(&p->link);
#endif
            js_free_rt(rt, p);
        }
    }
    js_free_rt(rt, rt->atom_array);
    js_free_rt(rt, rt->atom_hash);
    js_free_rt(rt, rt->shape_hash);
    /* free the retained empty-arena spares so nothing is leaked and the
       leak accounting below sees a clean slate. */
    js_malloc_release_empty_arenas(&rt->malloc_ctx);
#ifdef DUMP_LEAKS
    if (!list_empty(&rt->string_list)) {
        if (rt->rt_info) {
            printf("%s:1: string leakage:", rt->rt_info);
        } else {
            printf("String leaks:\n"
                   "    %6s %s\n",
                   "REFCNT", "VALUE");
        }
        list_for_each_safe(el, el1, &rt->string_list) {
            JSString *str = list_entry(el, JSString, link);
            if (rt->rt_info) {
                printf(" ");
            } else {
                printf("    %6u ", js_rc(str)->ref_count);
            }
            JS_DumpString(rt, str);
            if (rt->rt_info) {
                printf(":%u", js_rc(str)->ref_count);
            } else {
                printf("\n");
            }
            list_del(&str->link);
            js_free_rt(rt, str);
        }
        if (rt->rt_info)
            printf("\n");
    }
    {
        JSMallocState *s = &rt->malloc_ctx.malloc_state;
        if (s->malloc_count > 1) {
            if (rt->rt_info)
                printf("%s:1: ", rt->rt_info);
            printf("Memory leak: %"PRIu64" bytes lost in %"PRIu64" block%s\n",
                   (uint64_t)(s->malloc_size - sizeof(JSRuntime)),
                   (uint64_t)(s->malloc_count - 1), &"s"[s->malloc_count == 2]);
        }
    }
#endif

    {
        JSMallocState ms = rt->malloc_ctx.malloc_state;
        rt->malloc_ctx.mf.js_free(&ms, rt);
    }
}

JSContext *JS_NewContextRaw(JSRuntime *rt)
{
    JSContext *ctx;
    int i;

    ctx = js_mallocz_rt(rt, sizeof(JSContext));
    if (!ctx)
        return NULL;
    js_rc(ctx)->ref_count = 1;
    add_gc_object(rt, &ctx->header, JS_GC_OBJ_TYPE_JS_CONTEXT);

    ctx->class_proto = js_malloc_rt(rt, sizeof(ctx->class_proto[0]) *
                                    rt->class_count);
    if (!ctx->class_proto) {
        js_free_rt(rt, ctx);
        return NULL;
    }
    ctx->rt = rt;
    list_add_tail(&ctx->link, &rt->context_list);
    for(i = 0; i < rt->class_count; i++)
        ctx->class_proto[i] = JS_NULL;
    ctx->array_ctor = JS_NULL;
    ctx->iterator_ctor = JS_NULL;
    ctx->regexp_ctor = JS_NULL;
    ctx->promise_ctor = JS_NULL;
    init_list_head(&ctx->loaded_modules);

    if (JS_AddIntrinsicBasicObjects(ctx)) {
        JS_FreeContext(ctx);
        return NULL;
    }
    return ctx;
}

JSContext *JS_NewContext(JSRuntime *rt)
{
    JSContext *ctx;

    ctx = JS_NewContextRaw(rt);
    if (!ctx)
        return NULL;

    if (JS_AddIntrinsicBaseObjects(ctx) ||
        JS_AddIntrinsicDate(ctx) ||
        JS_AddIntrinsicLens(ctx) ||
        JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicStringNormalize(ctx) ||
        JS_AddIntrinsicRegExp(ctx) ||
        JS_AddIntrinsicJSON(ctx) ||
        JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) ||
        JS_AddIntrinsicTypedArrays(ctx) ||
        JS_AddIntrinsicPromise(ctx) ||
        JS_AddIntrinsicWeakRef(ctx) ||
        JS_AddIntrinsicDisposableStack(ctx)) {
        JS_FreeContext(ctx);
        return NULL;
    }
    return ctx;
}

void *JS_GetContextOpaque(JSContext *ctx)
{
    return ctx->user_opaque;
}

void JS_SetContextOpaque(JSContext *ctx, void *opaque)
{
    ctx->user_opaque = opaque;
}

/* set the new value and free the old value after (freeing the value
   can reallocate the object data) */
static inline void set_value(JSContext *ctx, JSValue *pval, JSValue new_val)
{
    JSValue old_val;
    old_val = *pval;
    *pval = new_val;
    JS_FreeValue(ctx, old_val);
}

void JS_SetClassProto(JSContext *ctx, JSClassID class_id, JSValue obj)
{
    JSRuntime *rt = ctx->rt;
    assert(class_id < rt->class_count);
    set_value(ctx, &ctx->class_proto[class_id], obj);
}

JSValue JS_GetClassProto(JSContext *ctx, JSClassID class_id)
{
    JSRuntime *rt = ctx->rt;
    assert(class_id < rt->class_count);
    return JS_DupValue(ctx, ctx->class_proto[class_id]);
}

typedef enum JSFreeModuleEnum {
    JS_FREE_MODULE_ALL,
    JS_FREE_MODULE_NOT_RESOLVED,
} JSFreeModuleEnum;

/* XXX: would be more efficient with separate module lists */
static void js_free_modules(JSContext *ctx, JSFreeModuleEnum flag)
{
    struct list_head *el, *el1;
    list_for_each_safe(el, el1, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        if (flag == JS_FREE_MODULE_ALL ||
            (flag == JS_FREE_MODULE_NOT_RESOLVED && !m->resolved)) {
            /* warning: the module may be referenced elsewhere. It
               could be simpler to use an array instead of a list for
               'ctx->loaded_modules' */
            list_del(&m->link);
            m->link.prev = NULL;
            m->link.next = NULL;
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_MODULE, m));
        }
    }
}

JSContext *JS_DupContext(JSContext *ctx)
{
    js_rc(ctx)->ref_count++;
    return ctx;
}

/* used by the GC */
static void JS_MarkContext(JSRuntime *rt, JSContext *ctx,
                           JS_MarkFunc *mark_func)
{
    int i;
    struct list_head *el;

    list_for_each(el, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        JS_MarkValue(rt, JS_MKPTR(JS_TAG_MODULE, m), mark_func);
    }

    JS_MarkValue(rt, ctx->global_obj, mark_func);
    JS_MarkValue(rt, ctx->global_var_obj, mark_func);

    JS_MarkValue(rt, ctx->throw_type_error, mark_func);
    JS_MarkValue(rt, ctx->eval_obj, mark_func);

    JS_MarkValue(rt, ctx->array_proto_values, mark_func);
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JS_MarkValue(rt, ctx->native_error_proto[i], mark_func);
    }
    for(i = 0; i < rt->class_count; i++) {
        JS_MarkValue(rt, ctx->class_proto[i], mark_func);
    }
    JS_MarkValue(rt, ctx->iterator_ctor, mark_func);
    JS_MarkValue(rt, ctx->async_iterator_proto, mark_func);
    JS_MarkValue(rt, ctx->promise_ctor, mark_func);
    JS_MarkValue(rt, ctx->array_ctor, mark_func);
    JS_MarkValue(rt, ctx->regexp_ctor, mark_func);
    JS_MarkValue(rt, ctx->function_ctor, mark_func);
    JS_MarkValue(rt, ctx->function_proto, mark_func);

    if (ctx->array_shape)
        mark_func(rt, &ctx->array_shape->header);

    if (ctx->arguments_shape)
        mark_func(rt, &ctx->arguments_shape->header);

    if (ctx->mapped_arguments_shape)
        mark_func(rt, &ctx->mapped_arguments_shape->header);

    if (ctx->regexp_shape)
        mark_func(rt, &ctx->regexp_shape->header);

    if (ctx->regexp_result_shape)
        mark_func(rt, &ctx->regexp_result_shape->header);

    if (ctx->iterator_result_shape)
        mark_func(rt, &ctx->iterator_result_shape->header);
}

/* ---- shutdown sweeps (JS_AddShutdownSweep) -----------------------------
 *
 * The mechanism behind the JS_FreeContext hook: a module that parks a
 * promise across JS registers a sweep that FAILS the parked operation at
 * teardown -- before the context dies, while JS calls are still legal --
 * releasing the resolve/reject (and everything they pin) that would
 * otherwise still be live at JS_FreeRuntime and trip
 * assert(list_empty(&rt->gc_obj_list)).
 *
 * Lifecycle: JS_AddShutdownSweep runs at EVERY park (idempotent on the
 * (func, opaque) pair), JS_RemoveShutdownSweep at every normal settle, and
 * the engine runs each registered sweep exactly once -- at the LAST context
 * teardown with that context (usable), and any straggler (a park made while
 * the sweeps were running: the drain's own user code, or a finalizer) in
 * the ctx == NULL mode -- the sweep must then release its pins WITHOUT
 * settling, JS calls being no longer legal there. Drain-time stragglers
 * are released by the pass's own re-check (still before the runtime's
 * GCs); a finalizer-made one is released at JS_FreeRuntime and one more GC
 * follows, so everything the release drops is collected before the
 * gc_obj_list assertion. Nothing runs during normal execution: in-flight
 * semantics are untouched.
 *
 * Bounded walk: a pass CALLS at most JS_SHUTDOWN_SWEEP_PASS_MAX sweeps
 * (the initial walk and the drain-time re-check share the budget). The
 * registry is normally drained until empty and no shipped sweep
 * re-registers, so the bound is unreachable for them; it exists for a
 * pathological sweep that re-registers the IDENTICAL (func, opaque) pair
 * it was just called with, which would otherwise keep its own walk from
 * ever seeing an empty registry. On exhaustion the remaining entries are
 * unlinked and freed WITHOUT being called -- the ctx == NULL straggler
 * outcome (no JS runs, nothing is settled) -- so whatever they pinned is
 * not released either. Terminating the pass in bounded time is the point.
 *
 * JS_ShutdownDeferFree: memory a sweep must keep alive past the job drain
 * -- a "dead shell" whose address a late settle may still name (the
 * once-only settle continuation of a parked flight or pipeline). The shell
 * stays allocated -- so a stale settle that fires while user reactions drain
 * reads consistent (dead-flag) state, never freed memory -- and the engine
 * frees every deferred block at the very end of JS_FreeRuntime, when no JS,
 * no job and no settle can ever run again. JS_ShutdownUndeferFree cancels
 * a defer when the module frees the shell earlier.
 *
 * Diagnostics: with DYNA_LEAK_DUMP=1 in the environment, JS_FreeRuntime
 * prints the GC objects still alive (address, refcount, dump) just before
 * the gc_obj_list assertion fires. Env-gated and off by default; it is the
 * teardown leak dump a shutdown-registry bug is diagnosed with. */

int JS_AddShutdownSweep(JSRuntime *rt, JSShutdownSweepFunc func, void *opaque)
{
    struct list_head *el;
    JSShutdownSweepEntry *e;

    if (!rt || !func)
        return -1;
    /* Idempotent: a double registration would fail the same parked
       operation twice at teardown. */
    list_for_each(el, &rt->shutdown_sweeps) {
        e = list_entry(el, JSShutdownSweepEntry, link);
        if (e->func == func && e->opaque == opaque)
            return 0;
    }
    e = js_malloc_rt(rt, sizeof(*e));
    if (!e)
        return -1;
    e->func = func;
    e->opaque = opaque;
    list_add_tail(&e->link, &rt->shutdown_sweeps);
    return 0;
}

void JS_RemoveShutdownSweep(JSRuntime *rt, JSShutdownSweepFunc func,
                            void *opaque)
{
    struct list_head *el, *el1;

    if (!rt)
        return;
    list_for_each_safe(el, el1, &rt->shutdown_sweeps) {
        JSShutdownSweepEntry *e = list_entry(el, JSShutdownSweepEntry, link);
        if (e->func == func && e->opaque == opaque) {
            list_del(&e->link);
            js_free_rt(rt, e);
        }
    }
}

int JS_ShutdownDeferFree(JSRuntime *rt, void *ptr)
{
    struct list_head *el;
    JSShutdownDeferEntry *e;

    if (!rt || !ptr)
        return -1;
    /* Idempotent: a shell deferred twice would be freed twice. */
    list_for_each(el, &rt->shutdown_deferred) {
        e = list_entry(el, JSShutdownDeferEntry, link);
        if (e->ptr == ptr)
            return 0;
    }
    e = js_malloc_rt(rt, sizeof(*e));
    if (!e)
        return -1;
    e->ptr = ptr;
    list_add_tail(&e->link, &rt->shutdown_deferred);
    return 0;
}

void JS_ShutdownUndeferFree(JSRuntime *rt, void *ptr)
{
    struct list_head *el, *el1;

    if (!rt)
        return;
    list_for_each_safe(el, el1, &rt->shutdown_deferred) {
        JSShutdownDeferEntry *e = list_entry(el, JSShutdownDeferEntry, link);
        if (e->ptr == ptr) {
            list_del(&e->link);
            js_free_rt(rt, e);
        }
    }
}

/* Bounded walk: a pass CALLS at most this many sweeps (the initial walk and
 * the drain-time re-check share the budget). See the rule in the section
 * comment above: on exhaustion the remaining entries are unlinked and freed
 * WITHOUT being called, so a pathological registry terminates in bounded
 * time with no JS run and nothing settled. */
#define JS_SHUTDOWN_SWEEP_PASS_MAX 65536

/* Drain the registry, calling each entry with `ctx`, at most *budget times
 * overall: the count called is subtracted from *budget. On exhaustion (an
 * entry left with no budget) the remaining entries are dropped WITHOUT
 * being called, with one diagnostic; *budget is then 0 for the rest of the
 * pass. Returns the number of sweeps called. */
static int js_call_shutdown_sweeps(JSContext *ctx, JSRuntime *rt,
                                   int *budget)
{
    int called = 0;

    while (!list_empty(&rt->shutdown_sweeps)) {
        JSShutdownSweepEntry *e;
        JSShutdownSweepFunc func;
        void *opaque;

        if (*budget <= 0) {
            struct list_head *el, *el1;

            fprintf(stderr,
                    "shutdown: sweep pass capped at %d calls; "
                    "remaining sweeps dropped without release\n",
                    JS_SHUTDOWN_SWEEP_PASS_MAX);
            list_for_each_safe(el, el1, &rt->shutdown_sweeps) {
                e = list_entry(el, JSShutdownSweepEntry, link);
                list_del(&e->link);
                js_free_rt(rt, e);
            }
            break;
        }
        e = list_entry(rt->shutdown_sweeps.next, JSShutdownSweepEntry, link);
        func = e->func;
        opaque = e->opaque;
        list_del(&e->link);
        js_free_rt(rt, e);
        func(ctx, rt, opaque);
        (*budget)--;
        called++;
    }
    return called;
}

/* Run every registered sweep exactly once (each is unlinked as it runs, so
 * a sweep that registers or removes others cannot corrupt the walk), then
 * -- with a live context -- drain the job queue so the rejection reactions
 * of the failed parks (user .catch handlers) run while JS is legal. The
 * drain is what makes "fail" observable; the sweep itself never runs user
 * code.
 *
 * A park made BY that drain-time user code registers a fresh sweep. The
 * RE-CHECK below releases every such straggler (ctx == NULL semantics: pins
 * released WITHOUT settling -- the documented contract for a park
 * registered during the drain) before returning, so the release happens
 * while a GC can still collect the graph it drops. Running the straggler
 * any later -- after the runtime's last GC -- is exactly the teardown
 * abort: the freed promise/reaction/closure graph cyclically includes the
 * realm (function objects hold context refs), so refcounts cannot collect
 * it and no GC runs again before the gc_obj_list assertion. A sweep still
 * outstanding after the runtime's last GC (a park made by a finalizer) is
 * released by the JS_FreeRuntime straggler pass, which is followed by one
 * more GC for the same reason.
 *
 * A job that THROWS during the drain stops it (JS_ExecutePendingJob returns
 * -1). The exception value is then owned by rt->current_exception and has
 * no consumer left -- the drain is the last JS the engine runs and there is
 * no host to report a teardown throw to -- so it is released here, exactly
 * once, and the slot is left holding an immediate (JS_UNDEFINED) so no
 * later free of the slot can reach a stale pointer. JS_FreeRuntime frees
 * the slot at its top, BEFORE this drain on that path, so this is the LAST
 * free of the value; on the JS_FreeContext hook path that top free sees the
 * JS_UNDEFINED left here and is a no-op. Without the release the surviving
 * exception keeps its prototype chain alive, and through the realm ref
 * every C function object holds, the whole context graph with it -- the
 * runtime's GCs then collect nothing and the gc_obj_list assertion aborts
 * the teardown (exit 134).
 *
 * The context is held for the duration: executing a job drops the job's
 * realm ref (JS_FreeContext inside JS_ExecutePendingJob), and the extra ref
 * keeps that from re-entering the free body of the very context being torn
 * down. The ref is deliberately NOT released here -- the caller IS the free
 * body that will dispose the context. */
static void js_run_shutdown_sweeps(JSContext *ctx, JSRuntime *rt)
{
    int drain_rc = 0;
    int guard;
    int budget = JS_SHUTDOWN_SWEEP_PASS_MAX;
    int ran = js_call_shutdown_sweeps(ctx, rt, &budget);

    if (!ctx || !ran)
        return;
    /* Hold the context across the drain by moving the count BY HAND:
       each job carries a realm ref which JS_ExecutePendingJob releases, and
       on the hook path the free body of THIS very context is mid-flight at
       count zero -- a nested decrement reaching zero would re-enter it and
       a JS_DupContext here could never be undone (JS_FreeContext at count
       one would start the free body again). The manual pair leaves the
       bookkeeping exactly as it found it: the outer teardown completes the
       free (hook path) or the count returns to its pre-drain value. */
    js_rc(ctx)->ref_count++;
    for (guard = 0; guard < 100000; guard++) {
        drain_rc = JS_ExecutePendingJob(rt, NULL);
        if (drain_rc <= 0)
            break;
    }
    /* Released INSIDE the manual hold: freeing the value runs no user JS,
       but it can drop the last reference on GC objects (a finalizer may
       even park again, which the re-check below then releases), and the
       hold is what keeps such a release from re-entering the context's
       free body. */
    if (drain_rc < 0) {
        JS_FreeValueRT(rt, rt->current_exception);
        rt->current_exception = JS_UNDEFINED;
    }
    js_rc(ctx)->ref_count--;
    /* RE-CHECK: whatever the drain parked is a straggler, and a straggler
       releases its pins WITHOUT settling (its user code already had its
       chance). The release must happen HERE -- not at the JS_FreeRuntime
       pass -- so the runtime's GCs can still collect what it drops. A
       straggler release runs no JS, so this walk terminates after one
       round; the loop is belt-and-braces, bounded by the pass budget. */
    (void)js_call_shutdown_sweeps(NULL, rt, &budget);
}

void JS_FreeContext(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    int i;

    if (--js_rc(ctx)->ref_count > 0)
        return;
    assert(js_rc(ctx)->ref_count == 0);

    /* The shutdown hook: when this is the runtime's LAST context, fail the
       C-pinned parked operations FIRST -- right here, with `ctx` still
       fully usable (the sweep rejections and their reaction jobs drain
       inside). (When job realms hold references on the context, this
       teardown is deferred to the job cleanup inside JS_FreeRuntime, which
       is still before the gc_obj_list assertion -- so every embedding path
       gets its sweeps.) */
    if (ctx->link.next == &rt->context_list &&
        ctx->link.prev == &rt->context_list)
        js_run_shutdown_sweeps(ctx, rt);

#ifdef DUMP_ATOMS
    JS_DumpAtoms(ctx->rt);
#endif
#ifdef DUMP_SHAPES
    JS_DumpShapes(ctx->rt);
#endif
#ifdef DUMP_OBJECTS
    {
        struct list_head *el;
        JSGCObjectHeader *p;
        printf("JSObjects: {\n");
        JS_DumpObjectHeader(ctx->rt);
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            JS_DumpGCObject(rt, p);
        }
        printf("}\n");
    }
#endif
#ifdef DUMP_MEM
    {
        JSMemoryUsage stats;
        JS_ComputeMemoryUsage(rt, &stats);
        JS_DumpMemoryUsage(stdout, &stats, rt);
    }
#endif

    js_free_modules(ctx, JS_FREE_MODULE_ALL);

    JS_FreeValue(ctx, ctx->global_obj);
    JS_FreeValue(ctx, ctx->global_var_obj);

    JS_FreeValue(ctx, ctx->throw_type_error);
    JS_FreeValue(ctx, ctx->eval_obj);

    JS_FreeValue(ctx, ctx->array_proto_values);
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JS_FreeValue(ctx, ctx->native_error_proto[i]);
    }
    for(i = 0; i < rt->class_count; i++) {
        JS_FreeValue(ctx, ctx->class_proto[i]);
    }
    js_free_rt(rt, ctx->class_proto);
    JS_FreeValue(ctx, ctx->iterator_ctor);
    JS_FreeValue(ctx, ctx->async_iterator_proto);
    JS_FreeValue(ctx, ctx->promise_ctor);
    JS_FreeValue(ctx, ctx->array_ctor);
    JS_FreeValue(ctx, ctx->regexp_ctor);
    JS_FreeValue(ctx, ctx->function_ctor);
    JS_FreeValue(ctx, ctx->function_proto);

    js_free_shape_null(ctx->rt, ctx->array_shape);
    js_free_shape_null(ctx->rt, ctx->arguments_shape);
    js_free_shape_null(ctx->rt, ctx->mapped_arguments_shape);
    js_free_shape_null(ctx->rt, ctx->regexp_shape);
    js_free_shape_null(ctx->rt, ctx->regexp_result_shape);
    js_free_shape_null(ctx->rt, ctx->iterator_result_shape);

    list_del(&ctx->link);
    remove_gc_object(&ctx->header);
    js_free_rt(ctx->rt, ctx);
}

JSRuntime *JS_GetRuntime(JSContext *ctx)
{
    return ctx->rt;
}

static void update_stack_limit(JSRuntime *rt)
{
    if (rt->stack_size == 0) {
        rt->stack_limit = 0; /* no limit */
    } else {
        rt->stack_limit = rt->stack_top - rt->stack_size;
    }
}

void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size)
{
    rt->stack_size = stack_size;
    update_stack_limit(rt);
}

void JS_UpdateStackTop(JSRuntime *rt)
{
    rt->stack_top = js_get_stack_pointer();
    update_stack_limit(rt);
}

static inline BOOL is_strict_mode(JSContext *ctx)
{
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    return (sf && (sf->js_mode & JS_MODE_STRICT));
}
