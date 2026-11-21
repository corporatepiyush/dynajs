#include "libc-internal.h"

#define DYN_EXEC_TIMEOUT_GRACE_MS 200

static void js_os_clear_handler(JSContext* ctx, int fd);

JSValue js_std_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *filename, *mode = NULL;
    FILE* f;
    int err;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+b")] != '\0') {
        JS_ThrowTypeError(ctx, "std.open: invalid file mode");
        goto fail;
    }

    f = fopen(filename, mode);
    if (!f)
        err = errno;
    else
        err = 0;
    if (argc >= 3)
        js_set_error_object(ctx, argv[2], err);
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, mode);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
fail:
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, mode);
    return JS_EXCEPTION;
}

JSValue js_std_fdopen(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* mode;
    FILE* f;
    int fd, err;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+")] != '\0') {
        JS_ThrowTypeError(ctx, "std.fdopen: invalid file mode");
        goto fail;
    }

    f = fdopen(fd, mode);
    if (!f)
        err = errno;
    else
        err = 0;
    if (argc >= 3)
        js_set_error_object(ctx, argv[2], err);
    JS_FreeCString(ctx, mode);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
fail:
    JS_FreeCString(ctx, mode);
    return JS_EXCEPTION;
}

JSValue js_os_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* filename;
    int flags, mode, ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &flags, argv[1]))
        goto fail;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32(ctx, &mode, argv[2])) {
        fail:
            JS_FreeCString(ctx, filename);
            return JS_EXCEPTION;
        }
    } else {
        mode = 0666;
    }
#if defined(_WIN32)
    if (!(flags & O_TEXT))
        flags |= O_BINARY;
#endif
    if (!(flags & O_CLOEXEC))
        flags |= O_CLOEXEC;
    ret = js_get_errno(open(filename, flags, mode));
    JS_FreeCString(ctx, filename);
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, ret;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (fd == 1 || fd == 2)
        dyn_stdio_sync();
    js_os_clear_handler(ctx, fd);
    ret = js_get_errno(close(fd));
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, whence;
    int64_t pos, ret;
    BOOL is_bigint;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    is_bigint = JS_IsBigInt(ctx, argv[1]);
    if (JS_ToInt64Ext(ctx, &pos, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &whence, argv[2]))
        return JS_EXCEPTION;
    ret = lseek(fd, pos, whence);
    if (ret == -1)
        ret = -errno;
    if (is_bigint)
        return JS_NewBigInt64(ctx, ret);
    else
        return JS_NewInt64(ctx, ret);
}

JSValue js_os_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int fd;
    uint64_t pos = 0, len = 0;
    int64_t fpos = 0;
    BOOL has_len = FALSE, has_fpos = FALSE;
    size_t size;
    ssize_t ret;
    uint8_t* buf;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToIndex(ctx, &pos, argv[2]))
            return JS_EXCEPTION;
    }
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        if (JS_ToIndex(ctx, &len, argv[3]))
            return JS_EXCEPTION;
        has_len = TRUE;
    }
    if (argc > 4 && !JS_IsUndefined(argv[4])) {
        if (JS_ToInt64(ctx, &fpos, argv[4]))
            return JS_EXCEPTION;
        if (fpos < 0)
            return JS_ThrowRangeError(ctx, "os: read/write position must not be negative");
        has_fpos = TRUE;
    }
    if (!magic && JS_CheckWritableBuffer(ctx, argv[1]) < 0)
        return JS_EXCEPTION;
    if (magic && (fd == 1 || fd == 2))
        dyn_stdio_sync();
    buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf)
        return JS_EXCEPTION;
    if (pos > size)
        return JS_ThrowRangeError(ctx, "os: read/write array buffer overflow");
    if (has_len) {
        if (len > size - pos)
            return JS_ThrowRangeError(ctx, "os: read/write array buffer overflow");
    } else {
        len = size - pos;
    }
    if (has_fpos) {
        if (magic)
            ret = js_get_errno(pwrite(fd, buf + pos, len, (off_t)fpos));
        else
            ret = js_get_errno(pread(fd, buf + pos, len, (off_t)fpos));
        if (ret < 0 && errno == ESPIPE) {
            if (magic)
                ret = js_get_errno(write(fd, buf + pos, len));
            else
                ret = js_get_errno(read(fd, buf + pos, len));
        }
    } else {
        if (magic)
            ret = js_get_errno(write(fd, buf + pos, len));
        else
            ret = js_get_errno(read(fd, buf + pos, len));
    }
    return JS_NewInt64(ctx, ret);
}

JSValue js_os_isatty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd));
}

#if defined(_WIN32)
JSValue js_os_ttyGetWinSize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    HANDLE handle;
    CONSOLE_SCREEN_BUFFER_INFO info;
    JSValue obj;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    handle = (HANDLE)_get_osfhandle(fd);

    if (!GetConsoleScreenBufferInfo(handle, &info))
        return JS_NULL;
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, info.dwSize.X), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, info.dwSize.Y), JS_PROP_C_W_E);
    return obj;
}

#define __ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define __ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200

JSValue js_os_ttySetRaw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    HANDLE handle;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    handle = (HANDLE)_get_osfhandle(fd);
    SetConsoleMode(handle, ENABLE_WINDOW_INPUT | __ENABLE_VIRTUAL_TERMINAL_INPUT);
    _setmode(fd, _O_BINARY);
    if (fd == 0) {
        handle = (HANDLE)_get_osfhandle(1);
        SetConsoleMode(handle, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | __ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    return JS_UNDEFINED;
}
#else
JSValue js_os_ttyGetWinSize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    struct winsize ws;
    JSValue obj;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 4 && ws.ws_row >= 4) {
        obj = JS_NewArray(ctx);
        if (JS_IsException(obj))
            return obj;
        JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, ws.ws_col), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, ws.ws_row), JS_PROP_C_W_E);
        return obj;
    } else {
        return JS_NULL;
    }
}

static struct termios oldtty;
static int term_exit_armed;

static void term_exit(void)
{
    dyn_stdio_sync();
    if (isatty(1)) {
        ssize_t n = write(1, "\x1b[?2004l", 8);
        (void)n;
    }
    tcsetattr(0, TCSANOW, &oldtty);
}

JSValue js_os_ttySetRaw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct termios tty;
    int fd;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;

    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd, &tty);
    oldtty = tty;

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
        | INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tty);

    if (!term_exit_armed) {
        term_exit_armed = 1;
        atexit(term_exit);
    }
    return JS_UNDEFINED;
}

#endif

JSValue js_os_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* filename;
    int ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
#if defined(_WIN32)
    {
        struct stat st;
        if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            ret = rmdir(filename);
        } else {
            ret = unlink(filename);
        }
    }
#else
    ret = remove(filename);
#endif
    ret = js_get_errno(ret);
    JS_FreeCString(ctx, filename);
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_rename(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *oldpath, *newpath;
    int ret;

    oldpath = JS_ToCString(ctx, argv[0]);
    if (!oldpath)
        return JS_EXCEPTION;
    newpath = JS_ToCString(ctx, argv[1]);
    if (!newpath) {
        JS_FreeCString(ctx, oldpath);
        return JS_EXCEPTION;
    }
    ret = js_get_errno(rename(oldpath, newpath));
    JS_FreeCString(ctx, oldpath);
    JS_FreeCString(ctx, newpath);
    return JS_NewInt32(ctx, ret);
}

void free_rw_handler(JSRuntime* rt, JSOSRWHandler* rh)
{
    int i;
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (ts)
        ts->rw_gen++;
#if !defined(_WIN32)
    if (ts && ts->watch_fd >= 0 && (rh->watch_mask & JS_WATCH_RW))
        js_watch_apply(ts, rh, 0);
#endif
    list_del(&rh->link);
    for (i = 0; i < 2; i++) {
        JS_FreeValueRT(rt, rh->rw_func[i]);
    }
    js_free_rt(rt, rh);
}

static void js_os_clear_handler(JSContext* ctx, int fd)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler* rh = find_rh(ts, fd);
    if (rh)
        free_rw_handler(rt, rh);
}

JSValue js_os_setReadHandler(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler* rh;
    int fd;
    JSValueConst func;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    func = argv[1];
    if (JS_IsNull(func)) {
        rh = find_rh(ts, fd);
        if (rh) {
            JS_FreeValue(ctx, rh->rw_func[magic]);
            rh->rw_func[magic] = JS_NULL;
            ts->rw_gen++;
            if (JS_IsNull(rh->rw_func[0]) && JS_IsNull(rh->rw_func[1])) {
                free_rw_handler(JS_GetRuntime(ctx), rh);
            }
        }
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, magic ? "os.setWriteHandler: not a function" : "os.setReadHandler: not a function");
        rh = find_rh(ts, fd);
        if (!rh) {
            rh = js_mallocz(ctx, sizeof(*rh));
            if (!rh)
                return JS_EXCEPTION;
            rh->fd = fd;
            rh->rw_func[0] = JS_NULL;
            rh->rw_func[1] = JS_NULL;
            list_add_tail(&rh->link, &ts->os_rw_handlers);
        }
        JS_FreeValue(ctx, rh->rw_func[magic]);
        rh->rw_func[magic] = JS_DupValue(ctx, func);
        ts->rw_gen++;
    }
    return JS_UNDEFINED;
}

static void os_signal_handler(int sig_num)
{
    int fd;
    atomic_fetch_or_explicit(&os_pending_signals,
        (uint64_t)1 << sig_num, memory_order_relaxed);
#if !defined(_WIN32)
    fd = atomic_load_explicit(&js_main_wake_wfd, memory_order_relaxed);
    if (fd >= 0) {
        int saved = errno;
        ssize_t w = write(fd, "", 1);
        (void)w;
        errno = saved;
    }
#else
    (void)fd;
#endif
}

#if !defined(_WIN32)
static int os_signal_install(uint32_t sig_num, sighandler_t handler)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return sigaction((int)sig_num, &sa, NULL);
}
#else
#define os_signal_install(sig_num, handler) signal((sig_num), (handler))
#endif

JSValue js_os_signal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSSignalHandler* sh;
    uint32_t sig_num;
    JSValueConst func;
    sighandler_t handler;

    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx,
            "os.signal: signal handler can only be set in the main thread");

    if (JS_ToUint32(ctx, &sig_num, argv[0]))
        return JS_EXCEPTION;
    if (sig_num >= 64)
        return JS_ThrowRangeError(ctx, "os.signal: invalid signal number");
    func = argv[1];
    if (JS_IsNull(func) || JS_IsUndefined(func)) {
        sh = find_sh(ts, sig_num);
        if (sh) {
            free_sh(JS_GetRuntime(ctx), sh);
        }
        if (JS_IsNull(func))
            handler = SIG_DFL;
        else
            handler = SIG_IGN;
        os_signal_install(sig_num, handler);
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, "os.signal: not a function");
        sh = find_sh(ts, sig_num);
        if (!sh) {
            sh = js_mallocz(ctx, sizeof(*sh));
            if (!sh)
                return JS_EXCEPTION;
            sh->sig_num = sig_num;
            list_add_tail(&sh->link, &ts->os_signal_handlers);
        }
        JS_FreeValue(ctx, sh->func);
        sh->func = JS_DupValue(ctx, func);
        (void)js_loop_poster_ensure(ts);
        os_signal_install(sig_num, os_signal_handler);
    }
    return JS_UNDEFINED;
}

JSValue js_os_loop_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue o = JS_NewObject(ctx);
    static const char* const names[] = { "waits", "timers", "fdEvents", "messages", "posts", "signals", "watched", "ioWaits", "ioChanges" };
    uint64_t vals[9];
    int i;

    if (JS_IsException(o))
        return o;
    vals[0] = ts->stat_waits;
    vals[1] = ts->stat_timers;
    vals[2] = ts->stat_fd_events;
    vals[3] = ts->stat_messages;
    vals[4] = ts->stat_posts;
    vals[5] = ts->stat_signals;
    vals[6] = 0;
    vals[7] = vals[8] = 0;
#ifdef CONFIG_NATIVE_MODULES
    dyn_evloop_thread_stats(&vals[7], &vals[8]);
#endif
#if !defined(_WIN32)
    if (ts->watch_fd >= 0) {
        struct list_head* el;
        list_for_each(el, &ts->os_rw_handlers)
        {
            if (list_entry(el, JSOSRWHandler, link)->watch_mask & JS_WATCH_RW)
                vals[6]++;
        }
    }
#endif
    for (i = 0; i < 9; i++) {
        if (JS_DefinePropertyValueStr(ctx, o, names[i], JS_NewInt64(ctx, (int64_t)vals[i]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    return o;
}

JSValue js_os_now(JSContext* ctx, JSValue this_val,
    int argc, JSValue* argv)
{
    uint64_t ns = (uint64_t)get_time_ns();
    ns = (ns / 1000) * 1000;
    return JS_NewFloat64(ctx, (double)ns / 1e6);
}

JSValue js_os_setTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSValueConst func;
    JSOSTimer* th;

    if (!ts)
        return JS_ThrowTypeError(ctx, "os.setTimeout: timers are not available in this context");
    func = argv[0];
    if (!JS_IsFunction(ctx, func))
        return JS_ThrowTypeError(ctx, "os.setTimeout: not a function");
    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[1]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
    th = js_mallocz(ctx, sizeof(*th));
    if (!th)
        return JS_EXCEPTION;
    do {
        th->timer_id = ts->next_timer_id;
        ts->next_timer_id++;
        if (ts->next_timer_id == INT32_MAX)
            ts->next_timer_id = 1;
    } while (find_timer_by_id(ts, th->timer_id));
    th->repeat = magic;
    if (magic && delay == 0)
        delay = 1;
    th->delay = delay;
    {
        int64_t now = js_time_us();
        ts->clock_us = now;
        th->timeout = now + delay * 1000;
    }
    th->func = JS_DupValue(ctx, func);
    if (argc > 2) {
        int i;
        th->argc = argc - 2;
        th->args = js_mallocz(ctx, sizeof(JSValue) * (size_t)th->argc);
        if (!th->args) {
            free_timer_body(rt, th);
            return JS_EXCEPTION;
        }
        for (i = 0; i < th->argc; i++)
            th->args[i] = JS_DupValue(ctx, argv[i + 2]);
    }
    if (timer_insert(rt, ts, th, delay) < 0) {
        free_timer_body(rt, th);
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_NewInt32(ctx, th->timer_id);
}

JSValue js_os_clearTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSTimer* th;
    int timer_id;

    if (JS_ToInt32(ctx, &timer_id, argv[0]))
        return JS_EXCEPTION;
    th = find_timer_by_id(ts, timer_id);
    if (!th)
        return JS_UNDEFINED;
    free_timer(rt, th);
    return JS_UNDEFINED;
}

JSValue js_os_sleepAsync(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSOSTimer* th;
    JSValue promise, resolving_funcs[2];

    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;

    th = js_mallocz(ctx, sizeof(*th));
    if (!th) {
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return JS_EXCEPTION;
    }
    th->timer_id = -1;
    {
        int64_t now = js_time_us();
        ts->clock_us = now;
        th->timeout = now + delay * 1000;
    }
    th->func = JS_DupValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    if (timer_insert(rt, ts, th, delay) < 0) {
        free_timer_body(rt, th);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }
    return promise;
}

void call_handler(JSContext* ctx, JSValueConst func)
{
    JSValue ret, func1;
    func1 = JS_DupValue(ctx, func);
    ret = JS_Call(ctx, func1, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, func1);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    JS_FreeValue(ctx, ret);
}

void call_timer(JSContext* ctx, JSValueConst func, int argc,
    JSValueConst* args)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue ret, func1;
    JSValue* save = NULL;
    int i;

    func1 = JS_DupValue(ctx, func);
    if (argc > 0) {
        save = js_malloc_rt(rt, sizeof(JSValue) * (size_t)argc);
        if (save) {
            for (i = 0; i < argc; i++)
                save[i] = JS_DupValue(ctx, args[i]);
        } else {
            argc = 0;
        }
    }
    ret = JS_Call(ctx, func1, JS_UNDEFINED, argc, (JSValueConst*)save);
    JS_FreeValue(ctx, func1);
    if (save) {
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, save[i]);
        js_free_rt(rt, save);
    }
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    JS_FreeValue(ctx, ret);
}

static JSValue make_obj_error(JSContext* ctx,
    JSValue obj,
    int err)
{
    JSValue arr;
    if (JS_IsException(obj))
        return obj;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    JS_DefinePropertyValueUint32(ctx, arr, 0, obj,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, err),
        JS_PROP_C_W_E);
    return arr;
}

static JSValue make_string_error(JSContext* ctx,
    const char* buf,
    int err)
{
    return make_obj_error(ctx, JS_NewString(ctx, buf), err);
}

JSValue js_os_getcwd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char buf[PATH_MAX];
    int err;

    if (!getcwd(buf, sizeof(buf))) {
        buf[0] = '\0';
        err = errno;
    } else {
        err = 0;
    }
    return make_string_error(ctx, buf, err);
}

JSValue js_os_chdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* target;
    int err;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    err = js_get_errno(chdir(target));
    JS_FreeCString(ctx, target);
    return JS_NewInt32(ctx, err);
}

JSValue js_os_mkdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int mode, ret;
    const char* path;

    if (argc >= 2) {
        if (JS_ToInt32(ctx, &mode, argv[1]))
            return JS_EXCEPTION;
    } else {
        mode = 0777;
    }
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    (void)mode;
    ret = js_get_errno(mkdir(path));
#else
    ret = js_get_errno(mkdir(path, mode));
#endif
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_readdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    DIR* f;
    struct dirent* d;
    JSValue obj;
    int err;
    uint32_t len;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    f = opendir(path);
    if (!f)
        err = errno;
    else
        err = 0;
    JS_FreeCString(ctx, path);
    if (!f)
        goto done;
    len = 0;
    for (;;) {
        errno = 0;
        d = readdir(f);
        if (!d) {
            err = errno;
            break;
        }
        JS_DefinePropertyValueUint32(ctx, obj, len++,
            JS_NewString(ctx, d->d_name),
            JS_PROP_C_W_E);
    }
    closedir(f);
done:
    return make_obj_error(ctx, obj, err);
}

#if !defined(_WIN32)
static int64_t timespec_to_ms(const struct timespec* tv)
{
    return (int64_t)tv->tv_sec * 1000 + (tv->tv_nsec / 1000000);
}
#endif

JSValue js_os_stat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_lstat)
{
    const char* path;
    int err, res;
    struct stat st;
    JSValue obj;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    res = stat(path, &st);
#else
    if (is_lstat)
        res = lstat(path, &st);
    else
        res = stat(path, &st);
#endif
    if (res < 0)
        err = errno;
    else
        err = 0;
    JS_FreeCString(ctx, path);
    if (res < 0) {
        obj = JS_NULL;
    } else {
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        JS_DefinePropertyValueStr(ctx, obj, "dev",
            JS_NewInt64(ctx, st.st_dev),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ino",
            JS_NewInt64(ctx, st.st_ino),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mode",
            JS_NewInt32(ctx, st.st_mode),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "nlink",
            JS_NewInt64(ctx, st.st_nlink),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "uid",
            JS_NewInt64(ctx, st.st_uid),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "gid",
            JS_NewInt64(ctx, st.st_gid),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "rdev",
            JS_NewInt64(ctx, st.st_rdev),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "size",
            JS_NewInt64(ctx, st.st_size),
            JS_PROP_C_W_E);
#if !defined(_WIN32)
        JS_DefinePropertyValueStr(ctx, obj, "blocks",
            JS_NewInt64(ctx, st.st_blocks),
            JS_PROP_C_W_E);
#endif
#if defined(_WIN32)
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, (int64_t)st.st_atime * 1000),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, (int64_t)st.st_mtime * 1000),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, (int64_t)st.st_ctime * 1000),
            JS_PROP_C_W_E);
#elif defined(__APPLE__)
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_atimespec)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_mtimespec)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_ctimespec)),
            JS_PROP_C_W_E);
#else
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_atim)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_mtim)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_ctim)),
            JS_PROP_C_W_E);
#endif
    }
    return make_obj_error(ctx, obj, err);
}

#if !defined(_WIN32)
static void ms_to_timeval(struct timeval* tv, uint64_t v)
{
    tv->tv_sec = v / 1000;
    tv->tv_usec = (v % 1000) * 1000;
}
#endif

JSValue js_os_utimes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    int64_t atime, mtime;
    int ret;

    if (JS_ToInt64(ctx, &atime, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &mtime, argv[2]))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    {
        struct _utimbuf times;
        times.actime = atime / 1000;
        times.modtime = mtime / 1000;
        ret = js_get_errno(_utime(path, &times));
    }
#else
    {
        struct timeval times[2];
        ms_to_timeval(&times[0], atime);
        ms_to_timeval(&times[1], mtime);
        ret = js_get_errno(utimes(path, times));
    }
#endif
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_sleep(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t delay;
    int ret;

    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
#if defined(_WIN32)
    {
        if (delay > INT32_MAX)
            delay = INT32_MAX;
        Sleep(delay);
        ret = 0;
    }
#else
    {
        struct timespec ts, rem;
        ts.tv_sec = delay / 1000;
        ts.tv_nsec = (delay % 1000) * 1000000;
        dyn_stdio_idle();
        while ((ret = nanosleep(&ts, &rem)) < 0 && errno == EINTR)
            ts = rem;
        ret = js_get_errno(ret);
    }
#endif
    return JS_NewInt32(ctx, ret);
}

#if defined(_WIN32)
char* realpath(const char* path, char* buf)
{
    if (!_fullpath(buf, path, PATH_MAX)) {
        errno = ENOENT;
        return NULL;
    } else {
        return buf;
    }
}
#endif

JSValue js_os_realpath(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    char buf[PATH_MAX], *res;
    int err;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    res = realpath(path, buf);
    JS_FreeCString(ctx, path);
    if (!res) {
        buf[0] = '\0';
        err = errno;
    } else {
        err = 0;
    }
    return make_string_error(ctx, buf, err);
}

#if !defined(_WIN32)
JSValue js_os_symlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *target, *linkpath;
    int err;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    linkpath = JS_ToCString(ctx, argv[1]);
    if (!linkpath) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    err = js_get_errno(symlink(target, linkpath));
    JS_FreeCString(ctx, target);
    JS_FreeCString(ctx, linkpath);
    return JS_NewInt32(ctx, err);
}

JSValue js_os_readlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    char buf[PATH_MAX];
    int err;
    ssize_t res;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    res = readlink(path, buf, sizeof(buf) - 1);
    if (res < 0) {
        buf[0] = '\0';
        err = errno;
    } else {
        buf[res] = '\0';
        err = 0;
    }
    JS_FreeCString(ctx, path);
    return make_string_error(ctx, buf, err);
}

static const char* os_exec_cstr(JSContext* ctx, JSValueConst val)
{
    size_t n = 0;
    const char* str = JS_ToCStringLen(ctx, &n, val);
    if (!str)
        return NULL;
    if (memchr(str, 0, n)) {
        JS_FreeCString(ctx, str);
        JS_ThrowTypeError(ctx, "os.exec: string contains a NUL character");
        return NULL;
    }
    return str;
}

JSValue js_os_exec(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValueConst options, args = argv[0];
    JSValue val, ret_val;
    const char **exec_argv, *file = NULL, *str, *cwd = NULL;
    char** envp = environ;
    uint32_t exec_argc, i;
    int ret, status;
    pid_t pid;

    dyn_stdio_sync();
    BOOL block_flag = TRUE, use_path = TRUE;
    int64_t timeout_ms = 0;
    static const char* std_name[3] = { "stdin", "stdout", "stderr" };
    int std_fds[3];
    uint32_t uid = -1, gid = -1;

    val = JS_GetPropertyStr(ctx, args, "length");
    if (JS_IsException(val))
        return JS_EXCEPTION;
    ret = JS_ToUint32(ctx, &exec_argc, val);
    JS_FreeValue(ctx, val);
    if (ret)
        return JS_EXCEPTION;
    if (exec_argc < 1 || exec_argc > 65535) {
        return JS_ThrowTypeError(ctx, "os.exec: invalid number of arguments");
    }
    exec_argv = js_mallocz(ctx, sizeof(exec_argv[0]) * (exec_argc + 1));
    if (!exec_argv)
        return JS_EXCEPTION;
    for (i = 0; i < exec_argc; i++) {
        val = JS_GetPropertyUint32(ctx, args, i);
        if (JS_IsException(val))
            goto exception;
        str = os_exec_cstr(ctx, val);
        JS_FreeValue(ctx, val);
        if (!str)
            goto exception;
        exec_argv[i] = str;
    }
    exec_argv[exec_argc] = NULL;

    for (i = 0; i < 3; i++)
        std_fds[i] = i;

    if (argc >= 2) {
        options = argv[1];

        if (get_bool_option(ctx, &block_flag, options, "blocking"))
            goto exception;
        if (get_bool_option(ctx, &block_flag, options, "block"))
            goto exception;
        if (get_bool_option(ctx, &use_path, options, "usePath"))
            goto exception;

        val = JS_GetPropertyStr(ctx, options, "timeout");
        if (JS_IsException(val))
            goto exception;
        if (JS_IsUndefined(val)) {
            JS_FreeValue(ctx, val);
            val = JS_GetPropertyStr(ctx, options, "timeoutMs");
            if (JS_IsException(val))
                goto exception;
        }
        if (!JS_IsUndefined(val)) {
            if (JS_ToInt64(ctx, &timeout_ms, val)) {
                JS_FreeValue(ctx, val);
                goto exception;
            }
            if (timeout_ms < 0) {
                JS_FreeValue(ctx, val);
                JS_ThrowRangeError(ctx, "os.exec: timeout must not be negative");
                goto exception;
            }
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, options, "file");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            file = os_exec_cstr(ctx, val);
            JS_FreeValue(ctx, val);
            if (!file)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "cwd");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            cwd = os_exec_cstr(ctx, val);
            JS_FreeValue(ctx, val);
            if (!cwd)
                goto exception;
        }

        for (i = 0; i < 3; i++) {
            val = JS_GetPropertyStr(ctx, options, std_name[i]);
            if (JS_IsException(val))
                goto exception;
            if (!JS_IsUndefined(val)) {
                int fd;
                ret = JS_ToInt32(ctx, &fd, val);
                JS_FreeValue(ctx, val);
                if (ret)
                    goto exception;
                std_fds[i] = fd;
            }
        }

        val = JS_GetPropertyStr(ctx, options, "env");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            envp = build_envp(ctx, val);
            JS_FreeValue(ctx, val);
            if (!envp)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "uid");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            ret = JS_ToUint32(ctx, &uid, val);
            JS_FreeValue(ctx, val);
            if (ret)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "gid");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            ret = JS_ToUint32(ctx, &gid, val);
            JS_FreeValue(ctx, val);
            if (ret)
                goto exception;
        }
    }

    {
        const char* spawn_file = file ? file : exec_argv[0];
        os_exec_own_group = block_flag && timeout_ms > 0;
        ret = os_exec_spawn_child(spawn_file, DYN_UNCONST(exec_argv), envp,
            std_fds, cwd, (uid_t)uid, (gid_t)gid,
            use_path, &pid);
    }
    if (ret != 0) {
        if (ret == EAGAIN || ret == ENOMEM || ret == EPERM || ret == EINVAL) {
            JS_ThrowTypeError(ctx, "os.exec: fork error");
            goto exception;
        }
        if (!block_flag) {
            JS_ThrowTypeError(ctx, "os.exec: fork error");
            goto exception;
        }
        ret_val = JS_NewInt32(ctx, 127);
        goto done;
    }
    if (block_flag) {
        int64_t start = get_time_ms();
        int64_t grace = timeout_ms > 0 ? DYN_EXEC_TIMEOUT_GRACE_MS : 0;
        pid_t wret;

        if (timeout_ms <= 0) {
            for (;;) {
                wret = waitpid(pid, &status, 0);
                if (wret == pid) {
                    if (WIFEXITED(status)) {
                        ret = WEXITSTATUS(status);
                        break;
                    } else if (WIFSIGNALED(status)) {
                        ret = -WTERMSIG(status);
                        break;
                    }
                    continue;
                }
                if (wret < 0) {
                    if (errno == EINTR)
                        continue;
                    ret = -errno;
                }
                break;
            }
        } else {
            for (;;) {
                wret = waitpid(pid, &status, WNOHANG);
                if (wret == pid) {
                    if (WIFEXITED(status)) {
                        ret = WEXITSTATUS(status);
                        break;
                    } else if (WIFSIGNALED(status)) {
                        ret = -WTERMSIG(status);
                        break;
                    }
                    continue;
                }
                if (wret < 0) {
                    if (errno == EINTR)
                        continue;
                    ret = -errno;
                    break;
                }
                if (get_time_ms() >= start + timeout_ms) {
                    if (kill(-pid, SIGTERM) == 0 || kill(pid, SIGTERM) == 0) {
                        struct timespec ts = { grace / 1000, (grace % 1000) * 1000000L };
                        nanosleep(&ts, NULL);
                        kill(-pid, SIGKILL);
                        if (waitpid(pid, &status, WNOHANG) != pid) {
                            kill(pid, SIGKILL);
                            dyn_waitpid_eintr(pid, &status, 0);
                        }
                    }
                    ret = -SIGTERM;
                    break;
                }
                {
                    struct timespec ts = { 0, 1000000L };
                    nanosleep(&ts, NULL);
                }
            }
        }
    } else {
        ret = pid;
    }
    ret_val = JS_NewInt32(ctx, ret);
done:
    JS_FreeCString(ctx, file);
    JS_FreeCString(ctx, cwd);
    for (i = 0; i < exec_argc; i++)
        JS_FreeCString(ctx, exec_argv[i]);
    js_free(ctx, exec_argv);
    if (envp && envp != environ) {
        char** p;
        p = envp;
        while (*p != NULL) {
            js_free(ctx, *p);
            p++;
        }
        js_free(ctx, envp);
    }
    return ret_val;
exception:
    ret_val = JS_EXCEPTION;
    goto done;
}

JSValue js_os_getpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return JS_NewInt32(ctx, getpid());
}

JSValue js_os_waitpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pid, status, options, ret;
    JSValue obj;

    if (JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &options, argv[1]))
        return JS_EXCEPTION;

    ret = waitpid(pid, &status, options);
    if (ret < 0) {
        ret = -errno;
        status = 0;
    }

    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, ret),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, status),
        JS_PROP_C_W_E);
    return obj;
}

JSValue js_os_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pipe_fds[2], ret;
    JSValue obj;

    ret = dyn_pipe_cloexec(pipe_fds);
    if (ret < 0)
        return JS_NULL;
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, pipe_fds[0]),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, pipe_fds[1]),
        JS_PROP_C_W_E);
    return obj;
}

JSValue js_os_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pid, sig, ret;

    if (JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sig, argv[1]))
        return JS_EXCEPTION;
    if (pid == 0 || pid == -1)
        return JS_ThrowRangeError(ctx,
            "os.kill: pid %d addresses every process; name a pid or -pgid", pid);
    if (sig < 0 || sig >= NSIG)
        return JS_ThrowRangeError(ctx, "os.kill: signal %d out of range", sig);
    ret = js_get_errno(kill(pid, sig));
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_dup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    ret = js_get_errno(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    return JS_NewInt32(ctx, ret);
}

JSValue js_os_dup2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, fd2, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd2, argv[1]))
        return JS_EXCEPTION;
    if (fd != fd2)
        js_os_clear_handler(ctx, fd2);
    if (fd2 == 1 || fd2 == 2)
        dyn_stdio_sync();
    ret = js_get_errno(dup2(fd, fd2));
    return JS_NewInt32(ctx, ret);
}
#endif

#ifdef USE_WORKER
static JSClassDef js_worker_class = {
    "Worker",
    .finalizer = js_worker_finalizer,
    .gc_mark = js_worker_mark,
};

static const JSCFunctionListEntry js_worker_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 2, js_worker_postMessage),
    JS_CGETSET_DEF("onmessage", js_worker_get_onmessage, js_worker_set_onmessage),
};
#endif

int js_os_init(JSContext* ctx, JSModuleDef* m)
{
    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);

#ifdef USE_WORKER
    {
        JSRuntime* rt = JS_GetRuntime(ctx);
        JSThreadState* ts = JS_GetRuntimeOpaque(rt);
        JSValue proto, obj;
        JS_NewClassID(&js_worker_class_id);
        JS_NewClass(JS_GetRuntime(ctx), js_worker_class_id, &js_worker_class);
        proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        if (JS_SetPropertyFunctionList(ctx, proto, js_worker_proto_funcs, countof(js_worker_proto_funcs)) < 0) {
            JS_FreeValue(ctx, proto);
            return -1;
        }
        obj = JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 1,
            JS_CFUNC_constructor, 0);
        if (JS_IsException(obj) || JS_SetConstructor(ctx, obj, proto) < 0) {
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, proto);
            return -1;
        }
        JS_SetClassProto(ctx, js_worker_class_id, proto);
        if (ts->recv_pipe && ts->send_pipe) {
            JSValue par = js_worker_ctor_internal(ctx, JS_UNDEFINED, ts->recv_pipe, ts->send_pipe);
            if (JS_IsException(par) || JS_DefinePropertyValueStr(ctx, obj, "parent", par, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, obj);
                return -1;
            }
        }
        if (JS_SetModuleExport(ctx, m, "Worker", obj) < 0)
            return -1;
    }
#endif

    return JS_SetModuleExportList(ctx, m, js_os_funcs,
        countof(js_os_funcs));
}

int js_std_deadline_remaining_ms(void)
{
    int64_t left;
    if (js_std_exec_deadline_ms == 0)
        return -1;
    left = (int64_t)js_std_exec_deadline_ms - get_time_ms();
    if (left < 0)
        return 0;
    return left > INT32_MAX ? INT32_MAX : (int)left;
}
