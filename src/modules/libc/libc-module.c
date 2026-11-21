#include "libc-internal.h"
#include "libc-url.h"
#include "core/dyn-childreg.h"
#include "dyna-nat.h"

#define OS_FLAG(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_CONFIGURABLE)
#define JS_STD_HARD_STOP_EXIT_CODE 113
#define JS_STD_HARD_STOP_GRACE_MIN_MS 250
#define JS_STD_HARD_STOP_GRACE_MAX_MS 2000
#define JS_STD_HARD_STOP_TEARDOWN_MS 10000

extern uint64_t js_std_exec_deadline_ms;

_Atomic int js_main_wake_wfd = -1;

_Atomic(int) js_live_worker_threads;

_Atomic uint64_t os_pending_signals;

_Atomic(JSOSPollFunc) os_poll_func;

void* js_std_dbuf_realloc(void* opaque, void* ptr, size_t size)
{
    return js_realloc_rt((JSRuntime*)opaque, ptr, size);
}

JSValue js_loadScript(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    const char* filename;
    JSValue ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (dyn_io_read_buf(filename, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
        JS_ThrowReferenceError(ctx, "std.loadScript: could not load '%s'", filename);
        JS_FreeCString(ctx, filename);
        return JS_EXCEPTION;
    }
    ret = JS_Eval(ctx, (char*)dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src),
        filename, JS_EVAL_TYPE_GLOBAL);
    dyn_iobuf_free(&src);
    JS_FreeCString(ctx, filename);
    return ret;
}

int get_bool_option(JSContext* ctx, BOOL* pbool,
    JSValueConst obj,
    const char* option)
{
    JSValue val;
    val = JS_GetPropertyStr(ctx, obj, option);
    if (JS_IsException(val))
        return -1;
    if (!JS_IsUndefined(val)) {
        *pbool = JS_ToBool(ctx, val);
    }
    JS_FreeValue(ctx, val);
    return 0;
}

JSClassID js_std_file_class_id;

JSValue js_new_std_file(JSContext* ctx, FILE* f,
    BOOL close_in_finalizer)
{
    JSSTDFile* s;
    JSValue obj;
    obj = JS_NewObjectClass(ctx, js_std_file_class_id);
    if (JS_IsException(obj))
        return obj;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->close_in_finalizer = close_in_finalizer;
    s->f = f;
    JS_SetOpaque(obj, s);
    return obj;
}

int js_std_runtime_is_main(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    return !ts || !ts->recv_pipe;
}

#if !defined(_WIN32)
char** build_envp(JSContext* ctx, JSValueConst obj)
{
    uint32_t len, i;
    JSPropertyEnum* tab;
    char **envp, *pair;
    const char *key, *str;
    JSValue val;
    size_t key_len, str_len;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return NULL;
    envp = js_mallocz(ctx, sizeof(envp[0]) * ((size_t)len + 1));
    if (!envp)
        goto fail;
    for (i = 0; i < len; i++) {
        val = JS_GetProperty(ctx, obj, tab[i].atom);
        if (JS_IsException(val))
            goto fail;
        str = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);
        if (!str)
            goto fail;
        key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) {
            JS_FreeCString(ctx, str);
            goto fail;
        }
        key_len = strlen(key);
        str_len = strlen(str);
        pair = js_malloc(ctx, key_len + str_len + 2);
        if (!pair) {
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, str);
            goto fail;
        }
        memcpy(pair, key, key_len);
        pair[key_len] = '=';
        memcpy(pair + key_len + 1, str, str_len);
        pair[key_len + 1 + str_len] = '\0';
        envp[i] = pair;
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, str);
    }
done:
    JS_FreePropertyEnum(ctx, tab, len);
    return envp;
fail:
    if (envp) {
        for (i = 0; i < len; i++)
            js_free(ctx, envp[i]);
        js_free(ctx, envp);
        envp = NULL;
    }
    goto done;
}
#endif

#ifdef USE_WORKER
JSClassID js_worker_class_id;

void* (*dyn_wmsg_alloc_fn)(size_t);

void (*dyn_wmsg_free_fn)(void*);

void dyn_wmsg_set_allocator(void* (*alloc_fn)(size_t), void (*free_fn)(void*))
{
    dyn_wmsg_alloc_fn = alloc_fn;
    dyn_wmsg_free_fn = free_fn;
}
#endif

const JSCFunctionListEntry js_os_funcs[] = {
    JS_CFUNC_DEF("open", 2, js_os_open),
    OS_FLAG(O_RDONLY),
    OS_FLAG(O_WRONLY),
    OS_FLAG(O_RDWR),
    OS_FLAG(O_APPEND),
    OS_FLAG(O_CREAT),
    OS_FLAG(O_EXCL),
    OS_FLAG(O_TRUNC),
#if defined(_WIN32)
    OS_FLAG(O_BINARY),
    OS_FLAG(O_TEXT),
#endif
    JS_CFUNC_DEF("close", 1, js_os_close),
    JS_CFUNC_DEF("seek", 3, js_os_seek),
    JS_CFUNC_MAGIC_DEF("read", 4, js_os_read_write, 0),
    JS_CFUNC_MAGIC_DEF("write", 4, js_os_read_write, 1),
    JS_CFUNC_DEF("isatty", 1, js_os_isatty),
    JS_CFUNC_DEF("ttyGetWinSize", 1, js_os_ttyGetWinSize),
    JS_CFUNC_DEF("ttySetRaw", 1, js_os_ttySetRaw),
    JS_CFUNC_DEF("remove", 1, js_os_remove),
    JS_CFUNC_DEF("rename", 2, js_os_rename),
    JS_CFUNC_DEF("openAsBlob", 1, js_os_open_as_blob),
    JS_CFUNC_DEF("loopStats", 0, js_os_loop_stats),
    JS_CFUNC_MAGIC_DEF("setReadHandler", 2, js_os_setReadHandler, 0),
    JS_CFUNC_MAGIC_DEF("setWriteHandler", 2, js_os_setReadHandler, 1),
    JS_CFUNC_DEF("signal", 2, js_os_signal),
    OS_FLAG(SIGINT),
    OS_FLAG(SIGABRT),
    OS_FLAG(SIGFPE),
    OS_FLAG(SIGILL),
    OS_FLAG(SIGSEGV),
    OS_FLAG(SIGTERM),
#if !defined(_WIN32)
    OS_FLAG(SIGQUIT),
    OS_FLAG(SIGPIPE),
    OS_FLAG(SIGALRM),
    OS_FLAG(SIGUSR1),
    OS_FLAG(SIGUSR2),
    OS_FLAG(SIGCHLD),
    OS_FLAG(SIGCONT),
    OS_FLAG(SIGSTOP),
    OS_FLAG(SIGTSTP),
    OS_FLAG(SIGTTIN),
    OS_FLAG(SIGTTOU),
#endif
    JS_CFUNC_DEF("now", 0, js_os_now),
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_os_setTimeout, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_os_setTimeout, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_os_clearTimeout),
    JS_CFUNC_DEF("clearInterval", 1, js_os_clearTimeout),
    JS_PROP_STRING_DEF("platform", OS_PLATFORM, 0),
    JS_CFUNC_DEF("getcwd", 0, js_os_getcwd),
    JS_CFUNC_DEF("chdir", 0, js_os_chdir),
    JS_CFUNC_DEF("mkdir", 1, js_os_mkdir),
    JS_CFUNC_DEF("readdir", 1, js_os_readdir),
    OS_FLAG(S_IFMT),
    OS_FLAG(S_IFIFO),
    OS_FLAG(S_IFCHR),
    OS_FLAG(S_IFDIR),
    OS_FLAG(S_IFBLK),
    OS_FLAG(S_IFREG),
#if !defined(_WIN32)
    OS_FLAG(S_IFSOCK),
    OS_FLAG(S_IFLNK),
    OS_FLAG(S_ISGID),
    OS_FLAG(S_ISUID),
#endif
    JS_CFUNC_MAGIC_DEF("stat", 1, js_os_stat, 0),
    JS_CFUNC_DEF("utimes", 3, js_os_utimes),
    JS_CFUNC_DEF("sleep", 1, js_os_sleep),
    JS_CFUNC_DEF("realpath", 1, js_os_realpath),
#if !defined(_WIN32)
    JS_CFUNC_MAGIC_DEF("lstat", 1, js_os_stat, 1),
    JS_CFUNC_DEF("symlink", 2, js_os_symlink),
    JS_CFUNC_DEF("readlink", 1, js_os_readlink),
    JS_CFUNC_DEF("exec", 1, js_os_exec),
    JS_CFUNC_DEF("getpid", 0, js_os_getpid),
    JS_CFUNC_DEF("waitpid", 2, js_os_waitpid),
    OS_FLAG(WNOHANG),
    JS_CFUNC_DEF("pipe", 0, js_os_pipe),
    JS_CFUNC_DEF("kill", 2, js_os_kill),
    JS_CFUNC_DEF("dup", 1, js_os_dup),
    JS_CFUNC_DEF("dup2", 2, js_os_dup2),
#endif
};

static JSValue js_console_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* fp = (magic & 1) ? stderr : stdout;
    JSValue ret;
    ret = js_print_internal(ctx, argc, argv, fp);
    fflush(fp);
    return ret;
}

static JSValue js_console_assert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (argc > 0 && JS_ToBool(ctx, argv[0]))
        return JS_UNDEFINED;
    dyn_stdio_sync();
    fputs("Assertion failed:", stderr);
    if (argc > 1)
        fputc(' ', stderr);
    return js_console_write(ctx, this_val, argc > 1 ? argc - 1 : 0,
        argv + 1, 1);
}

static const struct {
    const char* name;
    int magic;
} js_console_methods[] = {
    { "log", 0 },
    { "info", 0 },
    { "debug", 0 },
    { "trace", 1 },
    { "warn", 1 },
    { "error", 1 },
};

static JSValue js_std_queueMicrotask_job(JSContext* ctx,
    int argc, JSValueConst* argv)
{
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue js_std_queueMicrotask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_EnqueueJob(ctx, js_std_queueMicrotask_job, 1, &argv[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

int js_std_modules_enabled = 0;

uint64_t js_std_exec_deadline_ms = 0;

_Atomic int js_std_deadline_delivered;
static uint64_t js_std_hard_stop_grace;

static uint64_t js_std_hard_stop_grace_ms(uint64_t timeout_ms)
{
    uint64_t grace = timeout_ms / 4;
    if (grace < JS_STD_HARD_STOP_GRACE_MIN_MS)
        grace = JS_STD_HARD_STOP_GRACE_MIN_MS;
    if (grace > JS_STD_HARD_STOP_GRACE_MAX_MS)
        grace = JS_STD_HARD_STOP_GRACE_MAX_MS;
    return grace;
}

#if !defined(_WIN32)
static void* js_std_hard_stop_thread(void* opaque)
{
    static const char msg[] = "dyna: execution deadline exceeded and the script did not stop; terminating\n";
    static const char msg_exit[] = "dyna: the script was stopped at its deadline but did not finish exiting; terminating\n";
    uint64_t stop_at = (uint64_t)(uintptr_t)opaque;
    int teardown = 0;
    uint64_t drain_at = 0;
    dyn_thread_name("dyn-watchdog");
    for (;;) {
        int64_t left;
        if (teardown && !drain_at && atomic_load_explicit(&dyn_fiber_draining_cpu, memory_order_acquire)) {
            drain_at = (uint64_t)get_time_ms() + js_std_hard_stop_grace;
            if (drain_at < stop_at)
                stop_at = drain_at;
        }
        left = (int64_t)stop_at - get_time_ms();
        if (left <= 0) {
            if (!teardown && atomic_load_explicit(&js_std_deadline_delivered, memory_order_relaxed)) {
                teardown = 1;
                stop_at = (uint64_t)get_time_ms() + JS_STD_HARD_STOP_TEARDOWN_MS;
                continue;
            }
            break;
        }
        poll(NULL, 0, left > 50 ? 50 : (int)left);
    }
    dyn_stdio_sync_try();
    if (teardown) {
        if (write(2, msg_exit, sizeof(msg_exit) - 1) < 0) {
        }
    } else if (write(2, msg, sizeof(msg) - 1) < 0) {
    }
    dyn_childreg_kill_now();
    _exit(JS_STD_HARD_STOP_EXIT_CODE);
    return NULL;
}
#endif

void js_std_arm_exec_deadline(uint64_t timeout_ms)
{
    js_std_exec_deadline_ms = (uint64_t)get_time_ms() + timeout_ms;
#if !defined(_WIN32)
    {
        pthread_t tid;
        pthread_attr_t attr;
        uint64_t stop_at;
        js_std_hard_stop_grace = js_std_hard_stop_grace_ms(timeout_ms);
        stop_at = js_std_exec_deadline_ms + js_std_hard_stop_grace;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, js_std_hard_stop_thread, (void*)(uintptr_t)stop_at) != 0) {
            fprintf(stderr, "dyna: cannot start the --timeout-ms watchdog thread\n");
            exit(2);
        }
        pthread_attr_destroy(&attr);
    }
#endif
}

void js_std_add_helpers(JSContext* ctx, int argc, char** argv)
{
    JSValue global_obj, console, args, performance;
    int i;

    global_obj = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    for (i = 0; i < (int)countof(js_console_methods); i++) {
        JS_DefinePropertyValueStr(ctx, console, js_console_methods[i].name,
            JS_NewCFunctionMagic(ctx, js_console_write,
                js_console_methods[i].name, 1,
                JS_CFUNC_generic_magic,
                js_console_methods[i].magic),
            JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, console, "assert",
        JS_NewCFunction(ctx, js_console_assert, "assert", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "console", console, JS_PROP_C_W_E);

    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);
    JS_DefinePropertyValueStr(ctx, global_obj, "setTimeout",
        JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setTimeout",
            2, JS_CFUNC_generic_magic, 0),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "setInterval",
        JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setInterval",
            2, JS_CFUNC_generic_magic, 1),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "clearTimeout",
        JS_NewCFunction(ctx, js_os_clearTimeout, "clearTimeout", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "clearInterval",
        JS_NewCFunction(ctx, js_os_clearTimeout, "clearInterval", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "queueMicrotask",
        JS_NewCFunction(ctx, js_std_queueMicrotask, "queueMicrotask", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "sleep",
        JS_NewCFunction(ctx, js_os_sleepAsync, "sleep", 1), JS_PROP_C_W_E);

    performance = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, performance, "now",
        JS_NewCFunction(ctx, js_os_now, "now", 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "performance", performance, JS_PROP_C_W_E);

    if (argc >= 0) {
        args = JS_NewArray(ctx);
        for (i = 0; i < argc; i++) {
            JS_DefinePropertyValueUint32(ctx, args, i, JS_NewString(ctx, argv[i]), JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, global_obj, "scriptArgs", args, JS_PROP_C_W_E);
    }

    JS_DefinePropertyValueStr(ctx, global_obj, "print",
        JS_NewCFunction(ctx, js_print, "print", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "__loadScript",
        JS_NewCFunction(ctx, js_loadScript, "__loadScript", 1), JS_PROP_C_W_E);

    js_textcodec_install(ctx, global_obj);
    js_fetch_install(ctx, global_obj);
    dyn_sp_install(ctx, global_obj);
    dyn_url_install(ctx, global_obj);
    js_blob_install_url(ctx, global_obj);
    js_streams_install(ctx, global_obj);
    JS_DefinePropertyValueStr(ctx, global_obj, "atob",
        JS_NewCFunction(ctx, js_global_atob, "atob", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "btoa",
        JS_NewCFunction(ctx, js_global_btoa, "btoa", 1), JS_PROP_C_W_E);

#if defined(CONFIG_NATIVE_MODULES)
#if defined(CONFIG_NATIVE_MODULE_VSERIALIZE)
    {
        static const char* const sc_pairs[] = {
            "structuredClone",
            "structuredClone",
        };
        dyn_global_install_from_module(ctx, "dyna:serialize", "__dyna_sc_ns",
            sc_pairs, 1);
    }
#endif
#if defined(CONFIG_NATIVE_MODULE_HTTP)
    {
        static const char* const ws_pairs[] = {
            "WsClient",
            "WebSocket",
        };
        dyn_global_install_from_module(ctx, "dyna:http", "__dyna_ws_ns",
            ws_pairs, 1);
    }
#endif
#if defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)
    js_webrtc_install_globals(ctx);
#endif
#endif

    JS_FreeValue(ctx, global_obj);
}

void js_std_dump_error(JSContext* ctx)
{
    JSValue exception_val;

    exception_val = JS_GetException(ctx);
    if (JS_IsNull(exception_val)) {
        fprintf(stderr, "InternalError: out of memory (fatal)\n");
        return;
    }
    js_std_dump_error1(ctx, exception_val);
    JS_FreeValue(ctx, exception_val);
}
