#include "libc-internal.h"

#define JS_WORKER_QUEUE_MAX_BYTES ((size_t)256 << 20)

#ifdef USE_WORKER
typedef struct {
    JSWorkerMessagePipe* recv_pipe;
    JSWorkerMessagePipe* send_pipe;
    JSWorkerMessageHandler* msg_handler;
} JSWorkerData;

typedef struct {
    char* filename;
    char* basename;
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
} WorkerFuncArgs;

typedef struct {
    int ref_count;
    struct list_head link;
    size_t size;
    uint64_t buf[0];
} JSSABHeader;

#define JS_WORKER_LIVE_MAX 256
#define JS_WORKER_THREAD_STACK_BYTES ((size_t)JS_DEFAULT_STACK_SIZE * 8)
#endif


#ifdef USE_WORKER
static struct list_head sab_registry = LIST_HEAD_INIT(sab_registry);

static pthread_mutex_t sab_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static JSContext* (*js_worker_new_context_func)(JSRuntime* rt);

static int atomic_add_int(int* ptr, int v)
{
    return atomic_fetch_add((_Atomic(uint32_t)*)ptr, v) + v;
}

void* js_sab_alloc(void* opaque, size_t size)
{
    JSSABHeader* sab;
    sab = malloc(sizeof(JSSABHeader) + size);
    if (!sab)
        return NULL;
    sab->ref_count = 1;
    sab->size = size;
    pthread_mutex_lock(&sab_registry_mutex);
    list_add_tail(&sab->link, &sab_registry);
    pthread_mutex_unlock(&sab_registry_mutex);
    return sab->buf;
}

void js_sab_free(void* opaque, void* ptr)
{
    JSSABHeader* sab;
    int ref_count;
    sab = (JSSABHeader*)((uint8_t*)ptr - sizeof(JSSABHeader));
    ref_count = atomic_add_int(&sab->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        pthread_mutex_lock(&sab_registry_mutex);
        list_del(&sab->link);
        pthread_mutex_unlock(&sab_registry_mutex);
        free(sab);
    }
}

void js_sab_dup(void* opaque, void* ptr)
{
    JSSABHeader* sab;
    sab = (JSSABHeader*)((uint8_t*)ptr - sizeof(JSSABHeader));
    atomic_add_int(&sab->ref_count, 1);
}

int js_sab_valid_ptr(void* opaque, void* ptr, size_t byte_length,
    size_t max_byte_length)
{
    struct list_head* el;
    int valid = 0;

    pthread_mutex_lock(&sab_registry_mutex);
    list_for_each(el, &sab_registry)
    {
        JSSABHeader* sab = list_entry(el, JSSABHeader, link);
        if (sab->buf == ptr && byte_length <= sab->size
            && (max_byte_length == (size_t)UINT32_MAX
                || max_byte_length <= sab->size)) {
            valid = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sab_registry_mutex);
    return valid;
}

static JSWorkerMessagePipe* js_new_message_pipe(void)
{
    JSWorkerMessagePipe* ps;

    ps = calloc(1, sizeof(*ps));
    if (!ps)
        return NULL;
    if (js_waker_init(&ps->waker)) {
        free(ps);
        return NULL;
    }
    ps->ref_count = 1;
    init_list_head(&ps->msg_queue);
    pthread_mutex_init(&ps->mutex, NULL);
    return ps;
}

static JSWorkerMessagePipe* js_dup_message_pipe(JSWorkerMessagePipe* ps)
{
    atomic_add_int(&ps->ref_count, 1);
    return ps;
}

static void* dyn_wmsg_malloc(size_t n)
{
    if (dyn_wmsg_alloc_fn)
        return dyn_wmsg_alloc_fn(n);
    return malloc(n);
}

void dyn_wmsg_free(void* p)
{
    if (dyn_wmsg_free_fn) {
        dyn_wmsg_free_fn(p);
        return;
    }
    free(p);
}

void js_free_message_pipe(JSWorkerMessagePipe* ps)
{
    struct list_head *el, *el1;
    JSWorkerMessage* msg;
    int ref_count;

    if (!ps)
        return;

    ref_count = atomic_add_int(&ps->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        list_for_each_safe(el, el1, &ps->msg_queue)
        {
            msg = list_entry(el, JSWorkerMessage, link);
            js_free_message(msg);
        }
        pthread_mutex_destroy(&ps->mutex);
        js_waker_close(&ps->waker);
        free(ps);
    }
}

static void js_free_port(JSRuntime* rt, JSWorkerMessageHandler* port)
{
    if (port) {
        JSThreadState* ts = JS_GetRuntimeOpaque(rt);
        if (ts)
            ts->port_gen++;
        js_free_message_pipe(port->recv_pipe);
        JS_FreeValueRT(rt, port->on_message_func);
        if (port->link.prev)
            list_del(&port->link);
        js_free_rt(rt, port);
    }
}

void js_worker_finalizer(JSRuntime* rt, JSValue val)
{
    JSWorkerData* worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        js_free_message_pipe(worker->recv_pipe);
        js_free_message_pipe(worker->send_pipe);
        js_free_port(rt, worker->msg_handler);
        js_free_rt(rt, worker);
    }
}

void js_worker_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSWorkerData* worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        JSWorkerMessageHandler* port = worker->msg_handler;
        if (port) {
            JS_MarkValue(rt, port->on_message_func, mark_func);
        }
    }
}

static void* worker_func(void* opaque)
{
    WorkerFuncArgs* args = opaque;
    JSRuntime* rt;
    JSThreadState* ts;
    JSContext* ctx;
    JSValue val;

    dyn_thread_name("dyn-worker");
    rt = JS_NewRuntime();
    if (rt == NULL) {
        fprintf(stderr, "JS_NewRuntime failure");
        exit(1);
    }
    js_std_apply_budgets(rt);
    js_std_init_handlers(rt);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    ts = JS_GetRuntimeOpaque(rt);
    ts->recv_pipe = args->recv_pipe;
    ts->send_pipe = args->send_pipe;

    ctx = js_worker_new_context_func(rt);
    if (ctx == NULL) {
        fprintf(stderr, "JS_NewContext failure");
        free(args->filename);
        free(args->basename);
        free(args);
        atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_release);
        js_main_loop_wake();
        js_std_free_handlers(rt);
        JS_FreeRuntime(rt);
        return NULL;
    }

    JS_SetCanBlock(rt, TRUE);

    js_std_add_helpers(ctx, -1, NULL);

    val = JS_LoadModule(ctx, args->basename, args->filename);
    free(args->filename);
    free(args->basename);
    free(args);
    val = js_std_await(ctx, val);
    if (JS_IsException(val))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, val);

    js_std_loop(ctx);

    atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_release);
    js_main_loop_wake();

    JS_FreeContext(ctx);
    js_std_free_handlers(rt);
    JS_FreeRuntime(rt);
    return NULL;
}

JSValue js_worker_ctor_internal(JSContext* ctx, JSValueConst new_target,
    JSWorkerMessagePipe* recv_pipe,
    JSWorkerMessagePipe* send_pipe)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSWorkerData* s;

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_worker_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_worker_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        goto fail;
    s->recv_pipe = js_dup_message_pipe(recv_pipe);
    s->send_pipe = js_dup_message_pipe(send_pipe);

    JS_SetOpaque(obj, s);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

JSValue js_worker_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    WorkerFuncArgs* args = NULL;
    JSWorkerObjRef* wref;
    pthread_t tid;
    pthread_attr_t attr;
    JSValue obj = JS_UNDEFINED;
    int ret;
    const char *filename = NULL, *basename;
    JSAtom basename_atom;

    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx, "Worker: cannot create a worker inside a worker");

    basename_atom = JS_GetScriptOrModuleName(ctx, 1);
    if (basename_atom == JS_ATOM_NULL) {
        return JS_ThrowTypeError(ctx, "Worker: could not determine calling script or module name");
    }
    basename = JS_AtomToCString(ctx, basename_atom);
    JS_FreeAtom(ctx, basename_atom);
    if (!basename)
        goto fail;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;

    args = malloc(sizeof(*args));
    if (!args)
        goto oom_fail;
    memset(args, 0, sizeof(*args));
    args->filename = strdup(filename);
    args->basename = strdup(basename);

    if (atomic_load_explicit(&js_live_worker_threads, memory_order_relaxed) >= JS_WORKER_LIVE_MAX) {
        JS_ThrowRangeError(ctx, "Worker: %d workers are already running", JS_WORKER_LIVE_MAX);
        goto fail;
    }
    args->recv_pipe = js_new_message_pipe();
    if (!args->recv_pipe)
        goto oom_fail;
    args->send_pipe = js_new_message_pipe();
    if (!args->send_pipe)
        goto oom_fail;


    obj = js_worker_ctor_internal(ctx, new_target,
        args->send_pipe, args->recv_pipe);
    if (JS_IsException(obj))
        goto fail;

    wref = js_mallocz(ctx, sizeof(*wref));
    if (!wref) {
        JS_FreeValue(ctx, obj);
        obj = JS_EXCEPTION;
        goto fail;
    }
    wref->obj = JS_DupValue(ctx, obj);
    list_add_tail(&wref->link, &ts->worker_obj_list);

    if (!ts->recv_pipe)
        (void)js_loop_poster_ensure(ts);
    atomic_fetch_add_explicit(&js_live_worker_threads, 1, memory_order_relaxed);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, JS_WORKER_THREAD_STACK_BYTES);
    ret = pthread_create(&tid, &attr, worker_func, args);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        JS_ThrowTypeError(ctx, "Worker: could not create worker");
        atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_relaxed);
        list_del(&wref->link);
        JS_FreeValue(ctx, wref->obj);
        js_free(ctx, wref);
        goto fail;
    }
    JS_FreeCString(ctx, basename);
    JS_FreeCString(ctx, filename);
    return obj;
oom_fail:
    JS_ThrowOutOfMemory(ctx);
fail:
    JS_FreeCString(ctx, basename);
    JS_FreeCString(ctx, filename);
    if (args) {
        free(args->filename);
        free(args->basename);
        js_free_message_pipe(args->recv_pipe);
        js_free_message_pipe(args->send_pipe);
        free(args);
    }
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_worker_post_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_worker_postMessage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue prep, args[2], r;

    if (!JS_GetOpaque2(ctx, this_val, js_worker_class_id))
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[1]) || !js_streams_loaded(ctx))
        return js_worker_post_impl(ctx, this_val, argc, argv);
    prep = js_streams_transfer_prepare(ctx, argv[0], argv[1]);
    if (JS_IsException(prep))
        return prep;
    if (JS_IsUndefined(prep))
        return js_worker_post_impl(ctx, this_val, argc, argv);
    args[0] = JS_GetPropertyUint32(ctx, prep, 0);
    args[1] = JS_IsException(args[0]) ? JS_EXCEPTION : JS_GetPropertyUint32(ctx, prep, 1);
    if (JS_IsException(args[1])) {
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, prep);
        return JS_EXCEPTION;
    }
    r = js_worker_post_impl(ctx, this_val, 2, args);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        JSValue abort = JS_GetPropertyUint32(ctx, prep, 2);
        if (!JS_IsException(abort) && JS_IsFunction(ctx, abort))
            JS_FreeValue(ctx, JS_Call(ctx, abort, JS_UNDEFINED, 1, (JSValueConst*)&exc));
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, abort);
        JS_Throw(ctx, exc);
    } else {
        JSValue commit = JS_GetPropertyUint32(ctx, prep, 3);
        if (JS_IsException(commit)) {
            JS_FreeValue(ctx, r);
            r = JS_EXCEPTION;
        } else if (JS_IsFunction(ctx, commit)) {
            JSValue c = JS_Call(ctx, commit, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(c)) {
                JS_FreeValue(ctx, r);
                r = JS_EXCEPTION;
            } else {
                JS_FreeValue(ctx, c);
            }
        }
        JS_FreeValue(ctx, commit);
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, prep);
    return r;
}

static JSValue js_worker_post_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessagePipe* ps;
    size_t data_len, sab_tab_len, i;
    int queue_full;
    uint8_t* data;
    JSWorkerMessage* msg;
    uint8_t** sab_tab;
    JSValue* xfer = NULL;
    uint32_t nxfer = 0;

    if (!worker)
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue lv;
        if (!JS_IsObject(argv[1]) || JS_IsArray(ctx, argv[1]) != 1) {
            JS_ThrowTypeError(ctx, "Worker.postMessage: transfer must be an array");
            return JS_EXCEPTION;
        }
        lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nxfer, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (nxfer > 1024) {
            JS_ThrowRangeError(ctx, "Worker.postMessage: transfer too large");
            return JS_EXCEPTION;
        }
        if (nxfer) {
            xfer = (JSValue*)malloc(sizeof(JSValue) * nxfer);
            if (!xfer)
                return JS_ThrowOutOfMemory(ctx);
            for (uint32_t ti = 0; ti < nxfer; ti++) {
                JSValue e = JS_GetPropertyUint32(ctx, argv[1], ti);
                JSValue buf = JS_UNDEFINED;
                int is_buf = 0;
                if (JS_IsException(e)) {
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    return JS_EXCEPTION;
                }
                {
                    JSValue vb = JS_GetArrayBufferView(ctx, e, NULL, NULL, NULL);
                    if (!JS_IsException(vb)) {
                        buf = vb;
                        is_buf = 1;
                    } else {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        {
                            size_t ablen = 0;
                            uint8_t* ab = JS_GetArrayBuffer(ctx, &ablen, e);
                            if (ab || !JS_HasException(ctx)) {
                                if (JS_HasException(ctx)) {
                                    JS_FreeValue(ctx, JS_GetException(ctx));
                                }
                                buf = JS_DupValue(ctx, e);
                                is_buf = 1;
                            }
                        }
                    }
                }
                JS_FreeValue(ctx, e);
                if (is_buf) {
                    JSValue glob = JS_GetGlobalObject(ctx);
                    JSValue abctor = JS_GetPropertyStr(ctx, glob, "ArrayBuffer");
                    int is_ab = JS_IsException(abctor) ? -1 : JS_IsInstanceOf(ctx, buf, abctor);
                    int is_sab = 0;
                    if (is_ab == 0) {
                        JSValue sabctor = JS_GetPropertyStr(ctx, glob, "SharedArrayBuffer");
                        if (!JS_IsException(sabctor) && !JS_IsUndefined(sabctor) && !JS_IsNull(sabctor)) {
                            is_sab = JS_IsInstanceOf(ctx, buf, sabctor);
                            if (is_sab < 0)
                                is_sab = 0;
                        }
                        JS_FreeValue(ctx, sabctor);
                    }
                    JS_FreeValue(ctx, abctor);
                    JS_FreeValue(ctx, glob);
                    if (is_ab <= 0) {
                        JS_FreeValue(ctx, buf);
                        for (uint32_t k = 0; k < ti; k++)
                            JS_FreeValue(ctx, xfer[k]);
                        free(xfer);
                        if (is_ab < 0)
                            return JS_EXCEPTION;
                        if (is_sab)
                            return JS_ThrowTypeError(ctx,
                                "Worker.postMessage: transfer[%u]: cannot transfer a SharedArrayBuffer", ti);
                        return JS_ThrowTypeError(ctx,
                            "Worker.postMessage: transfer[%u]: cannot transfer an ArrayBuffer or ArrayBufferView from another realm", ti);
                    }
                }
                if (!is_buf) {
                    JS_FreeValue(ctx, buf);
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    JS_ThrowTypeError(ctx, "Worker.postMessage: transfer[%u] is not an ArrayBuffer", ti);
                    return JS_EXCEPTION;
                }
                if (JS_IsArrayBufferBorrowed(ctx, buf) || JS_IsArrayBufferImmutable(ctx, buf)) {
                    BOOL imm = JS_IsArrayBufferImmutable(ctx, buf);
                    JS_FreeValue(ctx, buf);
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    return JS_ThrowTypeError(ctx, imm ? "Worker.postMessage: transfer[%u] is an immutable ArrayBuffer" : "Worker.postMessage: transfer[%u] is in use by a running native call", ti);
                }
                xfer[ti] = buf;
            }
        }
    }

    data = JS_WriteObject2(ctx, &data_len, argv[0],
        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE,
        &sab_tab, &sab_tab_len);
    if (!data) {
        if (xfer) {
            for (uint32_t k = 0; k < nxfer; k++)
                JS_FreeValue(ctx, xfer[k]);
            free(xfer);
        }
        return JS_EXCEPTION;
    }

    ps = worker->send_pipe;
    pthread_mutex_lock(&ps->mutex);
    queue_full = data_len > JS_WORKER_QUEUE_MAX_BYTES
        || ps->queued_bytes > JS_WORKER_QUEUE_MAX_BYTES - data_len;
    pthread_mutex_unlock(&ps->mutex);
    if (queue_full) {
        js_free(ctx, data);
        js_free(ctx, sab_tab);
        if (xfer) {
            for (uint32_t k = 0; k < nxfer; k++)
                JS_FreeValue(ctx, xfer[k]);
            free(xfer);
        }
        return JS_ThrowRangeError(ctx,
            "Worker.postMessage: the receiver has %u MiB undelivered; "
            "wait for it to drain",
            (unsigned)(JS_WORKER_QUEUE_MAX_BYTES >> 20));
    }

    msg = dyn_wmsg_malloc(sizeof(*msg));
    if (!msg)
        goto fail;
    msg->data = NULL;
    msg->sab_tab = NULL;

    msg->data = dyn_wmsg_malloc(data_len);
    if (!msg->data)
        goto fail;
    memcpy(msg->data, data, data_len);
    msg->data_len = data_len;

    if (sab_tab_len > 0) {
        msg->sab_tab = dyn_wmsg_malloc(sizeof(msg->sab_tab[0]) * sab_tab_len);
        if (!msg->sab_tab)
            goto fail;
        memcpy(msg->sab_tab, sab_tab, sizeof(msg->sab_tab[0]) * sab_tab_len);
    }
    msg->sab_tab_len = sab_tab_len;

    js_free(ctx, data);
    js_free(ctx, sab_tab);

    for (i = 0; i < msg->sab_tab_len; i++) {
        js_sab_dup(NULL, msg->sab_tab[i]);
    }

    pthread_mutex_lock(&ps->mutex);
    if (list_empty(&ps->msg_queue))
        js_waker_signal(&ps->waker);
    list_add_tail(&msg->link, &ps->msg_queue);
    ps->queued_bytes += msg->data_len;
    pthread_mutex_unlock(&ps->mutex);
    if (xfer) {
        for (uint32_t k = 0; k < nxfer; k++) {
            JS_DetachArrayBuffer(ctx, xfer[k]);
            JS_FreeValue(ctx, xfer[k]);
        }
        free(xfer);
    }
    return JS_UNDEFINED;
fail:
    if (msg) {
        dyn_wmsg_free(msg->data);
        dyn_wmsg_free(msg->sab_tab);
        dyn_wmsg_free(msg);
    }
    js_free(ctx, data);
    js_free(ctx, sab_tab);
    if (xfer) {
        for (uint32_t k = 0; k < nxfer; k++)
            JS_FreeValue(ctx, xfer[k]);
        free(xfer);
    }
    return JS_EXCEPTION;
}

JSValue js_worker_set_onmessage(JSContext* ctx, JSValueConst this_val,
    JSValueConst func)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler* port;

    if (!worker)
        return JS_EXCEPTION;

    port = worker->msg_handler;
    if (JS_IsNull(func)) {
        if (port) {
            js_free_port(rt, port);
            worker->msg_handler = NULL;
        }
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, "Worker.onmessage: not a function");
        if (!port) {
            port = js_mallocz(ctx, sizeof(*port));
            if (!port)
                return JS_EXCEPTION;
            port->recv_pipe = js_dup_message_pipe(worker->recv_pipe);
            port->on_message_func = JS_NULL;
            list_add_tail(&port->link, &ts->port_list);
            worker->msg_handler = port;
        }
        JS_FreeValue(ctx, port->on_message_func);
        port->on_message_func = JS_DupValue(ctx, func);
        ts->port_gen++;
    }
    return JS_UNDEFINED;
}

JSValue js_worker_get_onmessage(JSContext* ctx, JSValueConst this_val)
{
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler* port;
    if (!worker)
        return JS_EXCEPTION;
    port = worker->msg_handler;
    if (port) {
        return JS_DupValue(ctx, port->on_message_func);
    } else {
        return JS_NULL;
    }
}

typedef struct {
    JSWorkerMessagePipe* recv_pipe;
    JSWorkerMessagePipe* send_pipe;
    JSWorkerMessageHandler* handler;
    uint8_t* gone;
    size_t gone_len;
    int closed;
} JSStreamPort;

typedef struct JSStreamPortSlot {
    struct JSStreamPortSlot* next;
    uint64_t id;
    JSWorkerMessagePipe* recv_pipe;
    JSWorkerMessagePipe* send_pipe;
} JSStreamPortSlot;

static JSClassID js_stream_port_class_id;
static pthread_mutex_t js_stream_port_mutex = PTHREAD_MUTEX_INITIALIZER;
static JSStreamPortSlot* js_stream_port_slots;
static uint64_t js_stream_port_next_id = 1;

static int js_pipe_push(JSWorkerMessagePipe* ps, const uint8_t* data, size_t len,
    uint8_t** sab_tab, size_t sab_tab_len)
{
    JSWorkerMessage* msg;
    size_t i;

    msg = dyn_wmsg_malloc(sizeof(*msg));
    if (!msg)
        return -1;
    msg->data = dyn_wmsg_malloc(len ? len : 1);
    msg->sab_tab = NULL;
    msg->sab_tab_len = 0;
    if (!msg->data) {
        dyn_wmsg_free(msg);
        return -1;
    }
    memcpy(msg->data, data, len);
    msg->data_len = len;
    if (sab_tab_len > 0) {
        msg->sab_tab = dyn_wmsg_malloc(sizeof(msg->sab_tab[0]) * sab_tab_len);
        if (!msg->sab_tab) {
            dyn_wmsg_free(msg->data);
            dyn_wmsg_free(msg);
            return -1;
        }
        memcpy(msg->sab_tab, sab_tab, sizeof(msg->sab_tab[0]) * sab_tab_len);
        msg->sab_tab_len = sab_tab_len;
        for (i = 0; i < sab_tab_len; i++)
            js_sab_dup(NULL, msg->sab_tab[i]);
    }
    pthread_mutex_lock(&ps->mutex);
    if (list_empty(&ps->msg_queue))
        js_waker_signal(&ps->waker);
    list_add_tail(&msg->link, &ps->msg_queue);
    ps->queued_bytes += msg->data_len;
    pthread_mutex_unlock(&ps->mutex);
    return 0;
}

static void js_stream_port_release(JSRuntime* rt, JSStreamPort* p, int send_gone)
{
    if (p->closed)
        return;
    p->closed = 1;
    if (send_gone && p->gone)
        js_pipe_push(p->send_pipe, p->gone, p->gone_len, NULL, 0);
    js_free_port(rt, p->handler);
    p->handler = NULL;
    js_free_message_pipe(p->recv_pipe);
    js_free_message_pipe(p->send_pipe);
    p->recv_pipe = p->send_pipe = NULL;
    dyn_wmsg_free(p->gone);
    p->gone = NULL;
}

static void js_stream_port_finalizer(JSRuntime* rt, JSValue val)
{
    JSStreamPort* p = JS_GetOpaque(val, js_stream_port_class_id);
    if (p) {
        js_stream_port_release(rt, p, 1);
        js_free_rt(rt, p);
    }
}

static void js_stream_port_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    JSStreamPort* p = JS_GetOpaque(val, js_stream_port_class_id);
    if (p && p->handler)
        JS_MarkValue(rt, p->handler->on_message_func, mark_func);
}

static const JSClassDef js_stream_port_class = {
    "StreamPort",
    .finalizer = js_stream_port_finalizer,
    .gc_mark = js_stream_port_mark,
};

static JSStreamPort* js_stream_port_of(JSContext* ctx, JSValueConst obj)
{
    JSStreamPort* p = JS_GetOpaque2(ctx, obj, js_stream_port_class_id);
    if (p && p->closed) {
        JS_ThrowTypeError(ctx, "StreamPort: closed");
        return NULL;
    }
    return p;
}

static JSValue js_stream_port_new(JSContext* ctx, JSWorkerMessagePipe* recv_pipe,
    JSWorkerMessagePipe* send_pipe)
{
    JSValue obj, gone, v;
    JSStreamPort* p;
    uint8_t* bytes;
    size_t len = 0;

    obj = JS_NewObjectClass(ctx, js_stream_port_class_id);
    if (JS_IsException(obj))
        return obj;
    p = js_mallocz(ctx, sizeof(*p));
    if (!p) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    p->recv_pipe = recv_pipe;
    p->send_pipe = send_pipe;
    JS_SetOpaque(obj, p);
    gone = JS_NewObject(ctx);
    v = JS_NewObject(ctx);
    if (JS_IsException(gone) || JS_IsException(v)) {
        JS_FreeValue(ctx, gone);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_SetPropertyStr(ctx, v, "@@dyna.streams.error", JS_NewString(ctx, "TypeError"));
    JS_SetPropertyStr(ctx, v, "message", JS_NewString(ctx, "the other side of a transferred stream went away"));
    JS_SetPropertyStr(ctx, gone, "type", JS_NewString(ctx, "error"));
    JS_SetPropertyStr(ctx, gone, "value", v);
    bytes = JS_WriteObject(ctx, &len, gone, 0);
    JS_FreeValue(ctx, gone);
    if (!bytes) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    p->gone = dyn_wmsg_malloc(len);
    if (!p->gone) {
        js_free(ctx, bytes);
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(p->gone, bytes, len);
    p->gone_len = len;
    js_free(ctx, bytes);
    return obj;
}

static JSValue js_stream_port_pair(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSWorkerMessagePipe *a, *b;
    JSStreamPortSlot* slot;
    JSValue port, arr;
    uint64_t id;

    (void)this_val;
    (void)argc;
    (void)argv;
    a = js_new_message_pipe();
    b = a ? js_new_message_pipe() : NULL;
    slot = b ? calloc(1, sizeof(*slot)) : NULL;
    if (!slot) {
        js_free_message_pipe(a);
        js_free_message_pipe(b);
        return JS_ThrowOutOfMemory(ctx);
    }
    port = js_stream_port_new(ctx, b, a);
    if (JS_IsException(port)) {
        free(slot);
        return port;
    }
    pthread_mutex_lock(&js_stream_port_mutex);
    id = js_stream_port_next_id++;
    slot->id = id;
    slot->recv_pipe = js_dup_message_pipe(a);
    slot->send_pipe = js_dup_message_pipe(b);
    slot->next = js_stream_port_slots;
    js_stream_port_slots = slot;
    pthread_mutex_unlock(&js_stream_port_mutex);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, port);
        return arr;
    }
    JS_SetPropertyUint32(ctx, arr, 0, port);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, (double)id));
    return arr;
}

static JSValue js_stream_port_claim(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSStreamPortSlot **pp, *slot = NULL;
    double d;
    uint64_t id;
    JSValue port;

    (void)this_val;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0)
        return JS_EXCEPTION;
    id = (uint64_t)d;
    pthread_mutex_lock(&js_stream_port_mutex);
    for (pp = &js_stream_port_slots; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            slot = *pp;
            *pp = slot->next;
            break;
        }
    }
    pthread_mutex_unlock(&js_stream_port_mutex);
    if (!slot)
        return JS_ThrowTypeError(ctx, "transferred stream %.0f was already claimed or never existed", d);
    port = js_stream_port_new(ctx, slot->recv_pipe, slot->send_pipe);
    if (JS_IsException(port)) {
        js_free_message_pipe(slot->recv_pipe);
        js_free_message_pipe(slot->send_pipe);
    }
    free(slot);
    return port;
}

static JSValue js_stream_port_post(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSStreamPort* p = js_stream_port_of(ctx, this_val);
    uint8_t* data;
    uint8_t** sab_tab = NULL;
    size_t len = 0, sab_len = 0;
    int rc;

    if (!p)
        return JS_EXCEPTION;
    data = JS_WriteObject2(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED,
        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE, &sab_tab, &sab_len);
    if (!data)
        return JS_EXCEPTION;
    rc = js_pipe_push(p->send_pipe, data, len, sab_tab, sab_len);
    js_free(ctx, data);
    js_free(ctx, sab_tab);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue js_stream_port_listen(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSStreamPort* p = js_stream_port_of(ctx, this_val);
    JSValueConst fn = argc > 0 ? argv[0] : JS_NULL;

    if (!p)
        return JS_EXCEPTION;
    if (JS_IsNull(fn) || JS_IsUndefined(fn)) {
        js_free_port(rt, p->handler);
        p->handler = NULL;
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "StreamPort.listen: not a function");
    if (!ts)
        return JS_ThrowInternalError(ctx, "StreamPort.listen: no event loop in this runtime");
    if (!p->handler) {
        p->handler = js_mallocz(ctx, sizeof(*p->handler));
        if (!p->handler)
            return JS_EXCEPTION;
        p->handler->recv_pipe = js_dup_message_pipe(p->recv_pipe);
        p->handler->on_message_func = JS_NULL;
        list_add_tail(&p->handler->link, &ts->port_list);
    }
    JS_FreeValue(ctx, p->handler->on_message_func);
    p->handler->on_message_func = JS_DupValue(ctx, fn);
    ts->port_gen++;
    return JS_UNDEFINED;
}

static JSValue js_stream_port_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSStreamPort* p = JS_GetOpaque2(ctx, this_val, js_stream_port_class_id);

    (void)argc;
    (void)argv;
    if (!p)
        return JS_EXCEPTION;
    js_stream_port_release(JS_GetRuntime(ctx), p, 0);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_stream_port_proto_funcs[] = {
    JS_CFUNC_DEF("post", 1, js_stream_port_post),
    JS_CFUNC_DEF("listen", 1, js_stream_port_listen),
    JS_CFUNC_DEF("close", 0, js_stream_port_close),
};

int js_stream_port_natives(JSContext* ctx, JSValueConst natives)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto;

    JS_NewClassID(&js_stream_port_class_id);
    if (!JS_IsRegisteredClass(rt, js_stream_port_class_id)
        && JS_NewClass(rt, js_stream_port_class_id, &js_stream_port_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, js_stream_port_proto_funcs,
            countof(js_stream_port_proto_funcs))
        < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, js_stream_port_class_id, proto);
    if (JS_DefinePropertyValueStr(ctx, natives, "portPair",
            JS_NewCFunction(ctx, js_stream_port_pair, "portPair", 0), JS_PROP_C_W_E) < 0)
        return -1;
    if (JS_DefinePropertyValueStr(ctx, natives, "portClaim",
            JS_NewCFunction(ctx, js_stream_port_claim, "portClaim", 1), JS_PROP_C_W_E) < 0)
        return -1;
    return 0;
}
#endif

void js_std_set_worker_new_context_func(JSContext* (*func)(JSRuntime* rt))
{
#ifdef USE_WORKER
    js_worker_new_context_func = func;
#endif
}

int js_std_interrupt_handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    return js_std_deadline_hit();
}

void js_std_apply_budgets(JSRuntime* rt)
{
    if (!rt)
        return;
    if (js_std_exec_deadline_ms)
        JS_SetInterruptHandler(rt, js_std_interrupt_handler, NULL);
}
