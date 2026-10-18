#ifndef DYNA_FIBER_INC_C
#define DYNA_FIBER_INC_C

#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>

#include "cutils.h"
#include "dyna-libc.h"
#include "list.h"

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
#define DYN_FIBER_NATIVE 1
#else
#define DYN_FIBER_NATIVE 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DYN_FIBER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DYN_FIBER_ASAN 1
#endif
#endif
#ifndef DYN_FIBER_ASAN
#define DYN_FIBER_ASAN 0
#endif

#if defined(__SANITIZE_THREAD__)
#define DYN_FIBER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DYN_FIBER_TSAN 1
#endif
#endif
#ifndef DYN_FIBER_TSAN
#define DYN_FIBER_TSAN 0
#endif

#if DYN_FIBER_ASAN
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom,
    size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save,
    const void** bottom_old, size_t* size_old);
#endif
#if DYN_FIBER_TSAN
void* __tsan_get_current_fiber(void);
void* __tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void* fiber);
void __tsan_switch_to_fiber(void* fiber, unsigned flags);
#endif

#define DYN_FIBER_STACK_SIZE ((size_t)8 << 20)
#define DYN_FIBER_GUARD_SIZE ((size_t)64 << 10)
#define DYN_FIBER_JS_MARGIN ((size_t)256 << 10)
#define DYN_FIBER_THREAD_STACK ((size_t)512 << 10)
#define DYN_FIBER_STACK_CACHE 4
#define DYN_FIBER_MAX_THREADS 128

enum {
    DYN_FIBER_RUNNING = 0,
    DYN_FIBER_WANT_THREAD,
    DYN_FIBER_WANT_JS,
    DYN_FIBER_AWAIT,
    DYN_FIBER_DONE,
};

struct DynFiber {
    void* sp;
    void* host_sp;
    char* stack;
    JSContext* ctx;
    DynFiberEntry* entry;
    void* arg;
    int state;
    int offloaded;
    int cancelled;
    _Atomic int cancel_req;
    JSValue result;
    JSValue resolving[2];
    JSValue await_val;
    JSValue await_res;
    int await_rejected;
    void* loop;
    JSLoopPost post;
    struct list_head link;
    struct list_head wait_link;
    int waiting;
#if DYN_FIBER_ASAN
    void* asan_fake;
    const void* asan_host_bottom;
    size_t asan_host_size;
#endif
#if DYN_FIBER_TSAN
    void* tsan_fiber;
    void* tsan_host;
#endif
};

static _Thread_local DynFiber* dyn_fiber_cur_tls;
static _Thread_local struct list_head dyn_fiber_live;
static _Thread_local int dyn_fiber_live_ready;
static _Thread_local int dyn_fiber_shutting_down;
static _Thread_local struct list_head dyn_fiber_waiting;

_Atomic int dyn_fiber_offloaded_n;
static _Thread_local int dyn_fiber_thread_offloaded;
static _Atomic uint64_t dyn_fiber_threads_spawned;
static _Atomic uint64_t dyn_fiber_threads_inline;
static _Atomic int dyn_fiber_threads_live;

static pthread_mutex_t dyn_fiber_stack_mu = PTHREAD_MUTEX_INITIALIZER;
static char* dyn_fiber_stack_cache[DYN_FIBER_STACK_CACHE];
static int dyn_fiber_stack_cached;

static __attribute__((noinline)) DynFiber* dyn_fiber_cur_get(void)
{
    return dyn_fiber_cur_tls;
}

static __attribute__((noinline)) void dyn_fiber_cur_set(DynFiber* f)
{
    dyn_fiber_cur_tls = f;
}

#if DYN_FIBER_NATIVE

void dyn_ctx_switch(void** from_sp, void* to_sp);
void dyn_ctx_boot(void);

#if defined(__APPLE__)
#define DYN_ASM_SYM(x) "_" #x
#define DYN_ASM_TYPE(x)
#else
#define DYN_ASM_SYM(x) #x
#define DYN_ASM_TYPE(x) ".type " #x ", %function\n"
#endif

#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl " DYN_ASM_SYM(dyn_ctx_switch) "\n" DYN_ASM_TYPE(dyn_ctx_switch)
        DYN_ASM_SYM(dyn_ctx_switch) ":\n"
                                    "    sub sp, sp, #160\n"
                                    "    stp x19, x20, [sp, #0]\n"
                                    "    stp x21, x22, [sp, #16]\n"
                                    "    stp x23, x24, [sp, #32]\n"
                                    "    stp x25, x26, [sp, #48]\n"
                                    "    stp x27, x28, [sp, #64]\n"
                                    "    stp x29, x30, [sp, #80]\n"
                                    "    stp d8, d9, [sp, #96]\n"
                                    "    stp d10, d11, [sp, #112]\n"
                                    "    stp d12, d13, [sp, #128]\n"
                                    "    stp d14, d15, [sp, #144]\n"
                                    "    mov x9, sp\n"
                                    "    str x9, [x0]\n"
                                    "    mov sp, x1\n"
                                    "    ldp x19, x20, [sp, #0]\n"
                                    "    ldp x21, x22, [sp, #16]\n"
                                    "    ldp x23, x24, [sp, #32]\n"
                                    "    ldp x25, x26, [sp, #48]\n"
                                    "    ldp x27, x28, [sp, #64]\n"
                                    "    ldp x29, x30, [sp, #80]\n"
                                    "    ldp d8, d9, [sp, #96]\n"
                                    "    ldp d10, d11, [sp, #112]\n"
                                    "    ldp d12, d13, [sp, #128]\n"
                                    "    ldp d14, d15, [sp, #144]\n"
                                    "    add sp, sp, #160\n"
                                    "    ret\n"
                                    ".p2align 2\n"
                                    ".globl " DYN_ASM_SYM(dyn_ctx_boot) "\n" DYN_ASM_TYPE(dyn_ctx_boot)
                                        DYN_ASM_SYM(dyn_ctx_boot) ":\n"
                                                                  "    mov x0, x19\n"
                                                                  "    mov x29, #0\n"
                                                                  "    mov x30, #0\n"
                                                                  "    bl " DYN_ASM_SYM(dyn_fiber_main) "\n"
                                                                                                        "    brk #1\n");
#else
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " DYN_ASM_SYM(dyn_ctx_switch) "\n" DYN_ASM_TYPE(dyn_ctx_switch)
        DYN_ASM_SYM(dyn_ctx_switch) ":\n"
                                    "    pushq %rbp\n"
                                    "    pushq %rbx\n"
                                    "    pushq %r12\n"
                                    "    pushq %r13\n"
                                    "    pushq %r14\n"
                                    "    pushq %r15\n"
                                    "    movq %rsp, (%rdi)\n"
                                    "    movq %rsi, %rsp\n"
                                    "    popq %r15\n"
                                    "    popq %r14\n"
                                    "    popq %r13\n"
                                    "    popq %r12\n"
                                    "    popq %rbx\n"
                                    "    popq %rbp\n"
                                    "    ret\n"
                                    ".p2align 4\n"
                                    ".globl " DYN_ASM_SYM(dyn_ctx_boot) "\n" DYN_ASM_TYPE(dyn_ctx_boot)
                                        DYN_ASM_SYM(dyn_ctx_boot) ":\n"
                                                                  "    movq %r12, %rdi\n"
                                                                  "    xorl %ebp, %ebp\n"
                                                                  "    call " DYN_ASM_SYM(dyn_fiber_main) "\n"
                                                                                                          "    ud2\n");
#endif

static char* dyn_fiber_stack_get(void)
{
    char* st = NULL;
    pthread_mutex_lock(&dyn_fiber_stack_mu);
    if (dyn_fiber_stack_cached > 0)
        st = dyn_fiber_stack_cache[--dyn_fiber_stack_cached];
    pthread_mutex_unlock(&dyn_fiber_stack_mu);
    if (st)
        return st;
    st = mmap(NULL, DYN_FIBER_STACK_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (st == MAP_FAILED)
        return NULL;
    if (mprotect(st, DYN_FIBER_GUARD_SIZE, PROT_NONE) < 0) {
        munmap(st, DYN_FIBER_STACK_SIZE);
        return NULL;
    }
    return st;
}

static void dyn_fiber_stack_put(char* st)
{
    if (!st)
        return;
    pthread_mutex_lock(&dyn_fiber_stack_mu);
    if (dyn_fiber_stack_cached < DYN_FIBER_STACK_CACHE) {
        dyn_fiber_stack_cache[dyn_fiber_stack_cached++] = st;
        st = NULL;
    }
    pthread_mutex_unlock(&dyn_fiber_stack_mu);
    if (st)
        munmap(st, DYN_FIBER_STACK_SIZE);
}

static void dyn_fiber_enter(DynFiber* f)
{
#if DYN_FIBER_TSAN
    f->tsan_host = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(f->tsan_fiber, 0);
#endif
#if DYN_FIBER_ASAN
    {
        void* host_fake = NULL;
        __sanitizer_start_switch_fiber(&host_fake,
            f->stack + DYN_FIBER_GUARD_SIZE,
            DYN_FIBER_STACK_SIZE - DYN_FIBER_GUARD_SIZE);
        dyn_ctx_switch(&f->host_sp, f->sp);
        __sanitizer_finish_switch_fiber(host_fake, NULL, NULL);
    }
#else
    dyn_ctx_switch(&f->host_sp, f->sp);
#endif
}

static void dyn_fiber_leave(DynFiber* f, int final)
{
#if DYN_FIBER_TSAN
    __tsan_switch_to_fiber(f->tsan_host, 0);
#endif
#if DYN_FIBER_ASAN
    __sanitizer_start_switch_fiber(final ? NULL : &f->asan_fake,
        f->asan_host_bottom, f->asan_host_size);
    dyn_ctx_switch(&f->sp, f->host_sp);
    __sanitizer_finish_switch_fiber(f->asan_fake, &f->asan_host_bottom,
        &f->asan_host_size);
#else
    (void) final;
    dyn_ctx_switch(&f->sp, f->host_sp);
#endif
}

void dyn_fiber_main(DynFiber* f);

void dyn_fiber_main(DynFiber* f)
{
#if DYN_FIBER_ASAN
    __sanitizer_finish_switch_fiber(NULL, &f->asan_host_bottom,
        &f->asan_host_size);
#endif
    f->result = f->entry(f->ctx, f->arg);
    f->state = DYN_FIBER_DONE;
    dyn_fiber_leave(f, 1);
    abort();
}

static void dyn_fiber_free(DynFiber* f)
{
    JSContext* ctx = f->ctx;
    list_del(&f->link);
    JS_FreeValue(ctx, f->resolving[0]);
    JS_FreeValue(ctx, f->resolving[1]);
#if DYN_FIBER_TSAN
    if (f->tsan_fiber)
        __tsan_destroy_fiber(f->tsan_fiber);
#endif
    dyn_fiber_stack_put(f->stack);
    free(f);
}

static void dyn_fiber_settle(DynFiber* f)
{
    JSContext* ctx = f->ctx;
    JSValue r;
    if (JS_IsException(f->result)) {
        JSValue err = JS_GetException(ctx);
        r = JS_Call(ctx, f->resolving[1], JS_UNDEFINED, 1, (JSValueConst*)&err);
        JS_FreeValue(ctx, err);
    } else {
        r = JS_Call(ctx, f->resolving[0], JS_UNDEFINED, 1,
            (JSValueConst*)&f->result);
        JS_FreeValue(ctx, f->result);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    dyn_fiber_free(f);
}

static void dyn_fiber_resume_on_js(DynFiber* f)
{
    JSRuntime* rt = JS_GetRuntime(f->ctx);
    DynFiber* prev = dyn_fiber_cur_get();
    uintptr_t top, limit;
    JS_GetStackBounds(rt, &top, &limit);
    JS_SetStackBounds(rt, (uintptr_t)(f->stack + DYN_FIBER_STACK_SIZE),
        (uintptr_t)(f->stack + DYN_FIBER_GUARD_SIZE + DYN_FIBER_JS_MARGIN));
    dyn_fiber_cur_set(f);
    f->state = DYN_FIBER_RUNNING;
    dyn_fiber_enter(f);
    dyn_fiber_cur_set(prev);
    JS_SetStackBounds(rt, top, limit);
}

static void dyn_fiber_drive(DynFiber* f);

static void* dyn_fiber_thread(void* p)
{
    DynFiber* f = p;
    dyn_fiber_cur_set(f);
    dyn_fiber_enter(f);
    dyn_fiber_cur_set(NULL);
    atomic_fetch_sub_explicit(&dyn_fiber_threads_live, 1, memory_order_relaxed);
    js_std_loop_post(f->loop, &f->post);
    return NULL;
}

static int dyn_fiber_spawn(DynFiber* f);

static void dyn_fiber_start_waiting(void)
{
    while (!list_empty(&dyn_fiber_waiting)
        && (dyn_fiber_shutting_down
            || atomic_load_explicit(&dyn_fiber_threads_live, memory_order_relaxed) < DYN_FIBER_MAX_THREADS)) {
        DynFiber* w = list_entry(dyn_fiber_waiting.next, DynFiber, wait_link);
        list_del(&w->wait_link);
        w->waiting = 0;
        if (dyn_fiber_shutting_down || dyn_fiber_spawn(w) < 0) {
            atomic_fetch_add_explicit(&dyn_fiber_threads_inline, 1, memory_order_relaxed);
            dyn_fiber_drive(w);
        }
    }
}

static void dyn_fiber_back_on_js(JSContext* ctx, void* udata)
{
    DynFiber* f = udata;
    js_std_loop_release(ctx);
    f->offloaded = 0;
    dyn_fiber_thread_offloaded--;
    atomic_fetch_sub_explicit(&dyn_fiber_offloaded_n, 1, memory_order_acq_rel);
    dyn_fiber_drive(f);
    dyn_fiber_start_waiting();
}

static int dyn_fiber_spawn(DynFiber* f)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc;
    f->loop = js_std_loop_hold(f->ctx);
    if (!f->loop)
        return -1;
    f->post.fn = dyn_fiber_back_on_js;
    f->post.udata = f;
    if (pthread_attr_init(&attr)) {
        js_std_loop_release(f->ctx);
        return -1;
    }
    pthread_attr_setstacksize(&attr, DYN_FIBER_THREAD_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    f->offloaded = 1;
    atomic_fetch_add_explicit(&dyn_fiber_offloaded_n, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&dyn_fiber_threads_live, 1, memory_order_relaxed);
    rc = pthread_create(&th, &attr, dyn_fiber_thread, f);
    pthread_attr_destroy(&attr);
    if (rc) {
        f->offloaded = 0;
        atomic_fetch_sub_explicit(&dyn_fiber_offloaded_n, 1, memory_order_acq_rel);
        atomic_fetch_sub_explicit(&dyn_fiber_threads_live, 1, memory_order_relaxed);
        js_std_loop_release(f->ctx);
        return -1;
    }
    dyn_fiber_thread_offloaded++;
    atomic_fetch_add_explicit(&dyn_fiber_threads_spawned, 1, memory_order_relaxed);
    return 0;
}

static JSValue dyn_fiber_on_settled(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValue* data)
{
    DynFiber* f;
    int64_t bits;
    (void)this_val;
    if (JS_ToBigInt64(ctx, &bits, data[0]))
        return JS_EXCEPTION;
    f = (DynFiber*)(uintptr_t)bits;
    if (f->state != DYN_FIBER_AWAIT)
        return JS_UNDEFINED;
    f->await_res = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    f->await_rejected = magic;
    dyn_fiber_drive(f);
    return JS_UNDEFINED;
}

static int dyn_fiber_subscribe(DynFiber* f)
{
    JSContext* ctx = f->ctx;
    JSValue key = JS_NewBigInt64(ctx, (int64_t)(uintptr_t)f);
    JSValue cbs[2], r;
    JSAtom then_atom;
    if (JS_IsException(key))
        return -1;
    cbs[0] = JS_NewCFunctionData(ctx, dyn_fiber_on_settled, 1, 0, 1, &key);
    cbs[1] = JS_NewCFunctionData(ctx, dyn_fiber_on_settled, 1, 1, 1, &key);
    JS_FreeValue(ctx, key);
    if (JS_IsException(cbs[0]) || JS_IsException(cbs[1])) {
        JS_FreeValue(ctx, cbs[0]);
        JS_FreeValue(ctx, cbs[1]);
        return -1;
    }
    then_atom = JS_NewAtom(ctx, "then");
    r = JS_Invoke(ctx, f->await_val, then_atom, 2, (JSValueConst*)cbs);
    JS_FreeAtom(ctx, then_atom);
    JS_FreeValue(ctx, cbs[0]);
    JS_FreeValue(ctx, cbs[1]);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static void dyn_fiber_drive(DynFiber* f)
{
    for (;;) {
        dyn_fiber_resume_on_js(f);
        switch (f->state) {
        case DYN_FIBER_DONE:
            dyn_fiber_settle(f);
            return;
        case DYN_FIBER_WANT_THREAD:
            if (!dyn_fiber_shutting_down
                && atomic_load_explicit(&dyn_fiber_threads_live, memory_order_relaxed) >= DYN_FIBER_MAX_THREADS
                && dyn_fiber_thread_offloaded > 0) {
                f->waiting = 1;
                list_add_tail(&f->wait_link, &dyn_fiber_waiting);
                return;
            }
            if (dyn_fiber_shutting_down || dyn_fiber_spawn(f) < 0) {
                atomic_fetch_add_explicit(&dyn_fiber_threads_inline, 1,
                    memory_order_relaxed);
                continue;
            }
            return;
        case DYN_FIBER_AWAIT:
            if (dyn_fiber_subscribe(f) < 0) {
                f->await_res = JS_GetException(f->ctx);
                f->await_rejected = 1;
                continue;
            }
            return;
        default:
            abort();
        }
    }
}

static void dyn_fiber_shutdown(JSRuntime* rt)
{
    struct list_head *el, *el1;
    int progress = 1;
    (void)rt;
    if (!dyn_fiber_live_ready)
        return;
    dyn_fiber_shutting_down = 1;
    while (!list_empty(&dyn_fiber_live) && progress) {
        progress = 0;
        if (!list_empty(&dyn_fiber_waiting)) {
            dyn_fiber_start_waiting();
            progress = 1;
            continue;
        }
        if (dyn_fiber_thread_offloaded > 0) {
            JSContext* wctx = list_entry(dyn_fiber_live.next, DynFiber, link)->ctx;
            list_for_each(el, &dyn_fiber_live)
                atomic_store_explicit(&list_entry(el, DynFiber, link)->cancel_req, 1,
                    memory_order_release);
            while (dyn_fiber_thread_offloaded > 0)
                js_std_loop_wait_posts(wctx);
            progress = 1;
            continue;
        }
        list_for_each_safe(el, el1, &dyn_fiber_live)
        {
            DynFiber* f = list_entry(el, DynFiber, link);
            if (f->state == DYN_FIBER_AWAIT) {
                f->cancelled = 1;
                f->await_res = JS_UNDEFINED;
                f->await_rejected = 2;
                dyn_fiber_drive(f);
                progress = 1;
                break;
            }
        }
    }
    dyn_fiber_shutting_down = 0;
}

JSValue dyn_fiber_start(JSContext* ctx, DynFiberEntry* entry, void* arg)
{
    DynFiber* f;
    JSValue promise;
    uintptr_t* sp;

    f = calloc(1, sizeof(*f));
    if (!f)
        return JS_ThrowOutOfMemory(ctx);
    f->stack = dyn_fiber_stack_get();
    if (!f->stack) {
        free(f);
        return JS_ThrowOutOfMemory(ctx);
    }
    promise = JS_NewPromiseCapability(ctx, f->resolving);
    if (JS_IsException(promise)) {
        dyn_fiber_stack_put(f->stack);
        free(f);
        return promise;
    }
    if (!dyn_fiber_live_ready) {
        init_list_head(&dyn_fiber_live);
        init_list_head(&dyn_fiber_waiting);
        dyn_fiber_live_ready = 1;
    }
    js_std_set_shutdown_hook(ctx, dyn_fiber_shutdown);
    list_add_tail(&f->link, &dyn_fiber_live);
    f->ctx = ctx;
    f->entry = entry;
    f->arg = arg;
    f->result = JS_UNDEFINED;
    f->await_val = JS_UNDEFINED;
    f->await_res = JS_UNDEFINED;
#if DYN_FIBER_TSAN
    f->tsan_fiber = __tsan_create_fiber(0);
#endif
    sp = (uintptr_t*)(f->stack + DYN_FIBER_STACK_SIZE);
#if defined(__aarch64__)
    sp -= 20;
    memset(sp, 0, 20 * sizeof(*sp));
    sp[0] = (uintptr_t)f;
    sp[11] = (uintptr_t)dyn_ctx_boot;
#else
    sp -= 9;
    memset(sp, 0, 9 * sizeof(*sp));
    sp[3] = (uintptr_t)f;
    sp[6] = (uintptr_t)dyn_ctx_boot;
#endif
    f->sp = sp;
    dyn_fiber_drive(f);
    return promise;
}

void* dyn_fiber_arg(DynFiberEntry* entry)
{
    DynFiber* f = dyn_fiber_cur_get();
    return f && f->entry == entry ? f->arg : NULL;
}

int dyn_fiber_cancel_state(void)
{
    DynFiber* f;
    if (atomic_load_explicit(&dyn_fiber_offloaded_n, memory_order_relaxed) == 0)
        return -1;
    f = dyn_fiber_cur_get();
    if (!f || !f->offloaded)
        return -1;
    return atomic_load_explicit(&f->cancel_req, memory_order_acquire) ? 1 : 0;
}

void dyn_fiber_offload(void)
{
    DynFiber* f = dyn_fiber_cur_get();
    if (!f || f->offloaded || f->cancelled)
        return;
    f->state = DYN_FIBER_WANT_THREAD;
    dyn_fiber_leave(f, 0);
}

void dyn_fiber_onjs_slow(void)
{
    DynFiber* f = dyn_fiber_cur_get();
    if (!f || !f->offloaded)
        return;
    f->state = DYN_FIBER_WANT_JS;
    dyn_fiber_leave(f, 0);
}

JSValue dyn_fiber_await(JSContext* ctx, JSValue v)
{
    DynFiber* f;
    int st;
    dyn_fiber_onjs();
    if (JS_IsException(v))
        return v;
    st = JS_PromiseState(ctx, v);
    if (st != JS_PROMISE_PENDING && st != JS_PROMISE_FULFILLED
        && st != JS_PROMISE_REJECTED)
        return v;
    if (st == JS_PROMISE_FULFILLED) {
        JSValue res = JS_PromiseResult(ctx, v);
        JS_FreeValue(ctx, v);
        return res;
    }
    f = dyn_fiber_cur_get();
    if (!f)
        return js_std_await(ctx, v);
    if (f->cancelled) {
        JS_FreeValue(ctx, v);
        return JS_ThrowInternalError(ctx, "interrupted: the runtime is shutting down");
    }
    f->await_val = v;
    f->state = DYN_FIBER_AWAIT;
    dyn_fiber_leave(f, 0);
    JS_FreeValue(ctx, f->await_val);
    f->await_val = JS_UNDEFINED;
    {
        JSValue res = f->await_res;
        int rej = f->await_rejected;
        f->await_res = JS_UNDEFINED;
        f->await_rejected = 0;
        if (rej == 2) {
            JS_FreeValue(ctx, res);
            return JS_ThrowInternalError(ctx, "interrupted: the runtime is shutting down");
        }
        return rej ? JS_Throw(ctx, res) : res;
    }
}

#else

JSValue dyn_fiber_start(JSContext* ctx, DynFiberEntry* entry, void* arg)
{
    JSValue resolving[2], promise, r, res;
    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return promise;
    res = entry(ctx, arg);
    if (JS_IsException(res)) {
        JSValue err = JS_GetException(ctx);
        r = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, (JSValueConst*)&err);
        JS_FreeValue(ctx, err);
    } else {
        r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, (JSValueConst*)&res);
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    atomic_fetch_add_explicit(&dyn_fiber_threads_inline, 1, memory_order_relaxed);
    return promise;
}

void* dyn_fiber_arg(DynFiberEntry* entry)
{
    (void)entry;
    return NULL;
}

int dyn_fiber_cancel_state(void)
{
    return -1;
}

void dyn_fiber_offload(void)
{
}

void dyn_fiber_onjs_slow(void)
{
}

JSValue dyn_fiber_await(JSContext* ctx, JSValue v)
{
    if (JS_IsException(v))
        return v;
    return js_std_await(ctx, v);
}

#endif

void dyn_fiber_stats(uint64_t* spawned, uint64_t* ran_inline, int* live)
{
    *spawned = atomic_load_explicit(&dyn_fiber_threads_spawned, memory_order_relaxed);
    *ran_inline = atomic_load_explicit(&dyn_fiber_threads_inline, memory_order_relaxed);
    *live = atomic_load_explicit(&dyn_fiber_threads_live, memory_order_relaxed);
}

#endif
