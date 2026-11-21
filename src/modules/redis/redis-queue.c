#include "redis-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_REDIS)
#define RD_MAX_FLUSH 256

static _Thread_local dyn_redis_t* net_flush_pending[RD_MAX_FLUSH];

static _Thread_local int net_n_flush;

dyn_redis_pending_t* pend_new(JSContext* ctx)
{
    dyn_redis_pending_t* p = (dyn_redis_pending_t*)calloc(1, sizeof(*p));
    (void)ctx;
    if (!p)
        return NULL;
    p->resolve = p->reject = p->acc = JS_UNDEFINED;
    p->want = 1;
    return p;
}

void pend_wipe_bytes(dyn_redis_pending_t* p)
{
    if (p->bytes && p->internal)
        redis_secure_zero(p->bytes, p->nbytes);
}

void pend_free(JSContext* ctx, dyn_redis_pending_t* p)
{
    JS_FreeValue(ctx, p->resolve);
    JS_FreeValue(ctx, p->reject);
    JS_FreeValue(ctx, p->acc);
    pend_wipe_bytes(p);
    free(p->bytes);
    free(p);
}

void pend_free_rt(JSRuntime* rt, dyn_redis_pending_t* p)
{
    JS_FreeValueRT(rt, p->resolve);
    JS_FreeValueRT(rt, p->reject);
    JS_FreeValueRT(rt, p->acc);
    pend_wipe_bytes(p);
    free(p->bytes);
    free(p);
}

void pend_push(dyn_redis_pending_t** head, dyn_redis_pending_t** tail,
    dyn_redis_pending_t* p)
{
    p->next = NULL;
    if (*tail)
        (*tail)->next = p;
    else
        *head = p;
    *tail = p;
}

dyn_redis_pending_t* pend_pop(dyn_redis_pending_t** head,
    dyn_redis_pending_t** tail)
{
    dyn_redis_pending_t* p = *head;
    if (!p)
        return NULL;
    *head = p->next;
    if (!*head)
        *tail = NULL;
    p->next = NULL;
    return p;
}

static JSValue redis_flush_job(JSContext* ctx, int argc, JSValueConst* argv);

int redis_flush(dyn_redis_t* r)
{
    int rc;
    if (r->olen == 0 || r->state == RD_ST_CONNECTING || r->state == RD_ST_DEAD)
        return 0;
    rc = dyn_aio_send(r->aio, r->fd, r->obuf, r->olen, 0, NULL, NULL);
    if (rc < 0)
        return rc;
    r->olen = 0;
    return 0;
}

void redis_flush_soon(dyn_redis_t* r)
{
    if (r->flush_queued || r->olen == 0 || r->state != RD_ST_READY)
        return;
    if (net_n_flush >= RD_MAX_FLUSH) {
        if (redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    if (JS_EnqueueJob(r->ctx, redis_flush_job, 0, NULL) < 0) {
        if (redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    r->flush_queued = 1;
    net_flush_pending[net_n_flush++] = r;
}

void redis_flush_drop(dyn_redis_t* r)
{
    int i;
    r->flush_queued = 0;
    for (i = 0; i < net_n_flush; i++)
        if (net_flush_pending[i] == r) {
            net_flush_pending[i] = net_flush_pending[net_n_flush - 1];
            net_n_flush--;
            return;
        }
}

static JSValue redis_flush_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    while (net_n_flush > 0) {
        dyn_redis_t* r = net_flush_pending[0];
        r->cb_depth++;
        redis_flush_drop(r);
        if (r->state == RD_ST_READY && redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        redis_cb_exit(r);
    }
    return JS_UNDEFINED;
}

int redis_queue_hello(dyn_redis_t* r)
{
    const char* argvh[2] = { "HELLO", "3" };
    if (r->hello_queued)
        return 0;
    r->hello_queued = 1;
    if (r->pass) {
        const char* five[5] = { "HELLO", "3", "AUTH",
            r->user ? r->user : "default", r->pass };
        return redis_send_internal(r, RD_INT_HELLO, 5, five, NULL);
    }
    return redis_send_internal(r, RD_INT_HELLO, 2, argvh, NULL);
}

void args_free(JSContext* ctx, redis_args_t* a)
{
    int i;
    for (i = 0; i < a->n; i++) {
        if (!a->argv[i] || !a->is_cstr || !a->is_cstr[i])
            continue;
        if (a->is_cstr[i] == 1)
            JS_FreeCString(ctx, a->argv[i]);
        else
            free((void*)(uintptr_t)a->argv[i]);
    }
    free(a->blk);
    memset(a, 0, sizeof(*a));
}

int args_build(JSContext* ctx, redis_args_t* a, int argc,
    JSValueConst* argv)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->blk = calloc(1, (size_t)argc * (sizeof(char*) + sizeof(size_t) + 1));
    if (!a->blk) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    a->argv = (const char**)a->blk;
    a->lens = (size_t*)(a->argv + argc);
    a->is_cstr = (uint8_t*)(a->lens + argc);
    for (i = 0; i < argc; i++) {
        size_t off = 0, blen = 0, bpe = 0;
        if (JS_IsObject(argv[i])) {
            JSValue ab = JS_GetArrayBufferView(ctx, argv[i], &off, &blen, &bpe);
            uint8_t* base = NULL;
            size_t total = 0;
            if (JS_IsException(ab)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                base = JS_GetArrayBuffer(ctx, &total, argv[i]);
                if (!base)
                    JS_FreeValue(ctx, JS_GetException(ctx));
                else
                    off = 0, blen = total;
            } else {
                base = JS_GetArrayBuffer(ctx, &total, ab);
                JS_FreeValue(ctx, ab);
            }
            if (base) {
                uint8_t* snap;
                if (off > total || blen > total - off) {
                    JS_ThrowRangeError(ctx,
                        "Redis: buffer argument view is out of bounds");
                    args_free(ctx, a);
                    return -1;
                }
                snap = (uint8_t*)malloc(blen ? blen : 1);
                if (!snap) {
                    JS_ThrowOutOfMemory(ctx);
                    args_free(ctx, a);
                    return -1;
                }
                memcpy(snap, base + off, blen);
                a->argv[i] = (const char*)snap;
                a->lens[i] = blen;
                a->is_cstr[i] = 2;
                a->n = i + 1;
                continue;
            }
        }
        {
            size_t l = 0;
            const char* s = JS_ToCStringLen(ctx, &l, argv[i]);
            if (!s) {
                args_free(ctx, a);
                return -1;
            }
            a->argv[i] = s;
            a->lens[i] = l;
            a->is_cstr[i] = 1;
            a->n = i + 1;
        }
    }
    a->n = argc;
    return 0;
}

JSValue redis_enqueue(JSContext* ctx, dyn_redis_t* r, int argc,
    const char* const* argv, const size_t* lens,
    int want, JSValue acc)
{
    JSValue funcs[2], promise;
    dyn_redis_pending_t* p;
    size_t need;
    uint8_t* b = NULL;

    if (r->npending + r->nwait >= r->maxpending) {
        JS_FreeValue(ctx, acc);
        return JS_ThrowInternalError(ctx,
            "Redis: %d commands already in flight (maxPending)", r->maxpending);
    }
    need = dyn_resp_cmd_size(argc, argv, lens);
    p = pend_new(ctx);
    if (!p) {
        JS_FreeValue(ctx, acc);
        return JS_ThrowOutOfMemory(ctx);
    }

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        pend_free(ctx, p);
        JS_FreeValue(ctx, acc);
        return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];
    p->want = want;
    p->acc = acc;
    p->is_sub_cmd = argc > 0
        && (redis_verb_is(argv[0], lens[0], RD_SUB, countof(RD_SUB))
            || redis_verb_is(argv[0], lens[0], RD_UNSUB, countof(RD_UNSUB)));
    if (argc > 1 && redis_verb_is(argv[0], lens[0], RD_SUB, countof(RD_SUB))) {
        p->sub_left = argc - 1;
        r->sub_guess += p->sub_left;
        redis_sub_recount(r);
    }

    if (r->state == RD_ST_READY) {
        if (redis_buf_reserve(&r->obuf, &r->ocap, r->olen + need) < 0 || dyn_resp_cmd_encode(r->obuf + r->olen, need, argc, argv, lens, NULL) < 0) {
            pend_free(ctx, p);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        r->olen += need;
        redis_arm(r, p);
        redis_flush_soon(r);
    } else {
        b = (uint8_t*)malloc(need);
        if (!b || dyn_resp_cmd_encode(b, need, argc, argv, lens, NULL) < 0) {
            free(b);
            pend_free(ctx, p);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        p->bytes = b;
        p->nbytes = need;
        pend_push(&r->wq_head, &r->wq_tail, p);
        r->nwait++;
    }
    return promise;
}
#endif
