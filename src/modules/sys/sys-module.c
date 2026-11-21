#include "sys-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

static const char* dyn_sys_errno_code(int e)
{
    switch (e) {
    case ENOENT:
        return "ENOENT";
    case EACCES:
        return "EACCES";
    case EEXIST:
        return "EEXIST";
    case ENOTDIR:
        return "ENOTDIR";
    case EISDIR:
        return "EISDIR";
    case ENOTEMPTY:
        return "ENOTEMPTY";
    case EPERM:
        return "EPERM";
    case ELOOP:
        return "ELOOP";
    case ENAMETOOLONG:
        return "ENAMETOOLONG";
    case EXDEV:
        return "EXDEV";
    case EINVAL:
        return "EINVAL";
    case ENOSPC:
        return "ENOSPC";
    case EROFS:
        return "EROFS";
    case EBUSY:
        return "EBUSY";
    case EMFILE:
        return "EMFILE";
    case ENFILE:
        return "ENFILE";
    case ENOMEM:
        return "ENOMEM";
    default:
        return NULL;
    }
}

JSValue dyn_sys_throw(JSContext* ctx, int e, const char* op,
    const char* path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char* code = dyn_sys_errno_code(e);

    if (path) {
        char epath[PATH_MAX * 4 + 16];
        dyn_esc_ctrl(epath, sizeof(epath), path);
        snprintf(msg, sizeof(msg), "sys.%s(\"%s\"): %s", op, epath,
            strerror(e));
    } else {
        snprintf(msg, sizeof(msg), "sys.%s: %s", op, strerror(e));
    }

    err = JS_NewError(ctx);
    if (JS_IsException(err))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e),
        JS_PROP_C_W_E);
    if (code)
        JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, code),
            JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

const char* dyn_sys_cstr(JSContext* ctx, JSValueConst v,
    const char* what)
{
    size_t len;
    const char* s = JS_ToCStringLen(ctx, &len, v);

    if (!s)
        return NULL;
    if (strlen(s) != len) {
        JS_ThrowTypeError(ctx, "%s: string contains a NUL byte", what);
        JS_FreeCString(ctx, s);
        return NULL;
    }
    return s;
}

static JSValue dyn_sys_args(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    JSValue arr;
    (void)this_val;
    (void)argc;
    (void)argv;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;

#if defined(__APPLE__)
    {
        int ac = *_NSGetArgc();
        char** av = *_NSGetArgv();
        int i;
        for (i = 0; i < ac && av && av[i]; i++)
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                dyn_utf8_lossy_string(ctx, (const uint8_t*)av[i],
                    strlen(av[i])),
                JS_PROP_C_W_E);
    }
#elif defined(__linux__)
    {
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char* buf = NULL;
            size_t len = 0, cap = 0;
            for (;;) {
                ssize_t got;
                if (len + 4096 > cap) {
                    size_t nc = cap ? cap * 2 : 8192;
                    char* nb = (char*)realloc(buf, nc);
                    if (!nb) {
                        free(buf);
                        buf = NULL;
                        len = 0;
                        break;
                    }
                    buf = nb;
                    cap = nc;
                }
                got = read(fd, buf + len, cap - len);
                if (got < 0) {
                    if (errno == EINTR)
                        continue;
                    free(buf);
                    buf = NULL;
                    len = 0;
                    break;
                }
                if (got == 0)
                    break;
                len += (size_t)got;
            }
            close(fd);
            if (buf) {
                size_t i = 0;
                uint32_t idx = 0;
                while (i < len) {
                    size_t start = i;
                    while (i < len && buf[i] != '\0')
                        i++;
                    JS_DefinePropertyValueUint32(ctx, arr, idx++,
                        dyn_utf8_lossy_string(ctx,
                            (const uint8_t*)(buf + start), i - start),
                        JS_PROP_C_W_E);
                    i++;
                }
                free(buf);
            }
        }
    }
#endif
    return arr;
}

static JSValue dyn_sys_cwd(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    char stackbuf[PATH_MAX];
    char* cwd;
    JSValue out;
    (void)this_val;
    (void)argc;
    (void)argv;

    cwd = getcwd(stackbuf, sizeof(stackbuf));
    if (cwd)
        return JS_NewString(ctx, cwd);
    if (errno != ERANGE)
        return dyn_sys_throw(ctx, errno, "cwd", NULL);
    {
        size_t cap = sizeof(stackbuf);
        for (;;) {
            char* buf;
            cap *= 2;
            if (cap > (1u << 20))
                return dyn_sys_throw(ctx, ENAMETOOLONG, "cwd", NULL);
            buf = (char*)malloc(cap);
            if (!buf)
                return JS_ThrowOutOfMemory(ctx);
            if (getcwd(buf, cap)) {
                out = JS_NewString(ctx, buf);
                free(buf);
                return out;
            }
            free(buf);
            if (errno != ERANGE)
                return dyn_sys_throw(ctx, errno, "cwd", NULL);
        }
    }
}

static JSValue dyn_sys_chdir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    (void)this_val;
    (void)argc;

    path = dyn_sys_cstr(ctx, argv[0], "chDir: path");
    if (!path)
        return JS_EXCEPTION;
    if (chdir(path) != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "chDir", path);
        JS_FreeCString(ctx, path);
        return ex;
    }
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static const char* dyn_sys_platform_from_sysname(const char* sysname)
{
    static const char* const known[] = {
        "darwin",
        "linux",
        "freebsd",
        "openbsd",
        "netbsd",
        "dragonfly",
        "sunos",
    };
    size_t i;
    for (i = 0; i < countof(known); i++) {
        const char* k = known[i];
        size_t j;
        for (j = 0; k[j]; j++)
            if (tolower((unsigned char)sysname[j]) != k[j])
                break;
        if (k[j] == '\0')
            return k;
    }
    return NULL;
}

static JSValue dyn_sys_platform(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    struct utsname un;
    const char* p;
    (void)this_val;
    (void)argc;
    (void)argv;

    if (uname(&un) == 0) {
        p = dyn_sys_platform_from_sysname(un.sysname);
        if (p)
            return JS_NewString(ctx, p);
    }
#if defined(__APPLE__)
    return JS_NewString(ctx, "darwin");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue dyn_sys_arch(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    struct utsname un;
    const char* m;
    char buf[sizeof(un.machine)];
    size_t i;
    (void)this_val;
    (void)argc;
    (void)argv;

    if (uname(&un) != 0)
        return JS_NewString(ctx, "unknown");
    m = un.machine;
    if (strcasecmp(m, "aarch64") == 0)
        return JS_NewString(ctx, "arm64");
    if (strcasecmp(m, "amd64") == 0)
        return JS_NewString(ctx, "x86_64");
    if (strcasecmp(m, "x86_64") == 0 || strcasecmp(m, "arm64") == 0)
        return JS_NewString(ctx, m);
    for (i = 0; m[i] && i < sizeof(buf) - 1; i++)
        buf[i] = (char)tolower((unsigned char)m[i]);
    buf[i] = '\0';
    return JS_NewStringLen(ctx, buf, i);
}

static JSValue dyn_sys_uname(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    struct utsname un;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;

    if (uname(&un) != 0)
        return dyn_sys_throw(ctx, errno, "uname", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETSTR(name, v)                                               \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewString(ctx, v), \
            JS_PROP_C_W_E)                                            \
        < 0) {                                                        \
        JS_FreeValue(ctx, o);                                         \
        return JS_EXCEPTION;                                          \
    }
    SETSTR("sysname", un.sysname)
    SETSTR("nodename", un.nodename)
    SETSTR("release", un.release)
    SETSTR("version", un.version)
    SETSTR("machine", un.machine)
#undef SETSTR
    return o;
}

static JSValue dyn_sys_cpu_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct rusage ru;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return dyn_sys_throw(ctx, errno, "cpuUsage", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETSEC(name, tv)                                                                                              \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewFloat64(ctx, (double)(tv).tv_sec + (double)(tv).tv_usec / 1e6), \
            JS_PROP_C_W_E)                                                                                            \
        < 0) {                                                                                                        \
        JS_FreeValue(ctx, o);                                                                                         \
        return JS_EXCEPTION;                                                                                          \
    }
    SETSEC("user", ru.ru_utime)
    SETSEC("system", ru.ru_stime)
#undef SETSEC
    return o;
}

static JSValue dyn_sys_thread_cpu_now(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct timespec ts;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return dyn_sys_throw(ctx, errno, "threadCpuNow", NULL);
    return JS_NewFloat64(ctx, (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6);
}

static JSValue dyn_sys_par_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_par_stats_t st;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;
    dyn_par_stats(&st);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "runs", JS_NewInt64(ctx, (int64_t)st.runs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "serialRuns", JS_NewInt64(ctx, (int64_t)st.serial_runs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "spawned", JS_NewInt64(ctx, (int64_t)st.spawned), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "spawnFailures", JS_NewInt64(ctx, (int64_t)st.spawn_failures), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "live", JS_NewInt64(ctx, st.live), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "peak", JS_NewInt64(ctx, st.live_peak), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "budget", JS_NewInt64(ctx, st.budget), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue dyn_sys_rusage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct rusage ru;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return dyn_sys_throw(ctx, errno, "rusage", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETI64(name, v)                         \
    if (JS_DefinePropertyValueStr(ctx, o, name, \
            JS_NewInt64(ctx, (int64_t)(v)),     \
            JS_PROP_C_W_E)                      \
        < 0) {                                  \
        JS_FreeValue(ctx, o);                   \
        return JS_EXCEPTION;                    \
    }
#define SETSEC(name, tv)                                                                                              \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewFloat64(ctx, (double)(tv).tv_sec + (double)(tv).tv_usec / 1e6), \
            JS_PROP_C_W_E)                                                                                            \
        < 0) {                                                                                                        \
        JS_FreeValue(ctx, o);                                                                                         \
        return JS_EXCEPTION;                                                                                          \
    }
    SETSEC("user", ru.ru_utime)
    SETSEC("system", ru.ru_stime)
#if defined(__linux__)
    SETI64("maxrss", (int64_t)ru.ru_maxrss * 1024)
#else
    SETI64("maxrss", (int64_t)ru.ru_maxrss)
#endif
    SETI64("idrss", ru.ru_idrss)
    SETI64("isrss", ru.ru_isrss)
    SETI64("minflt", ru.ru_minflt)
    SETI64("majflt", ru.ru_majflt)
    SETI64("nswap", ru.ru_nswap)
    SETI64("inblock", ru.ru_inblock)
    SETI64("oublock", ru.ru_oublock)
    SETI64("msgsnd", ru.ru_msgsnd)
    SETI64("msgrcv", ru.ru_msgrcv)
    SETI64("nsigs", ru.ru_nsignals)
    SETI64("nvcsw", ru.ru_nvcsw)
    SETI64("nivcsw", ru.ru_nivcsw)
#undef SETI64
    return o;
}

static JSValue dyn_sys_getuid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int32_t)getuid());
}

static JSValue dyn_sys_getgid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int32_t)getgid());
}

static JSValue dyn_sys_set_id(JSContext* ctx, JSValueConst* argv,
    const char* what, int32_t* out)
{
    int32_t id;
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "%s: id must be a number", what);
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    if (!isfinite(d))
        return JS_ThrowRangeError(ctx, "%s: id must be finite", what);
    if (d < 0.0)
        return JS_ThrowRangeError(ctx, "%s: id must be >= 0", what);
    if (d > 2147483647.0)
        return JS_ThrowRangeError(ctx, "%s: id must be <= 2147483647", what);
    if (d != floor(d))
        return JS_ThrowTypeError(ctx, "%s: id must be an integer", what);
    id = (int32_t)d;
    *out = id;
    return JS_UNDEFINED;
}

static JSValue dyn_sys_setuid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t id;
    (void)this_val;
    (void)argc;
    {
        JSValue r = dyn_sys_set_id(ctx, argv, "setUid", &id);
        if (JS_IsException(r))
            return r;
    }
    if ((uid_t)id != getuid() && getuid() == 0 && setgroups(0, NULL) != 0)
        return dyn_sys_throw(ctx, errno, "setUid", NULL);
    if (setuid((uid_t)id) != 0)
        return dyn_sys_throw(ctx, errno, "setUid", NULL);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_setgid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t id;
    (void)this_val;
    (void)argc;
    {
        JSValue r = dyn_sys_set_id(ctx, argv, "setGid", &id);
        if (JS_IsException(r))
            return r;
    }
    if ((gid_t)id != getgid() && getuid() == 0 && setgroups(0, NULL) != 0)
        return dyn_sys_throw(ctx, errno, "setGid", NULL);
    if (setgid((gid_t)id) != 0)
        return dyn_sys_throw(ctx, errno, "setGid", NULL);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_pid(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int32_t)getpid());
}

static JSValue dyn_sys_host_name(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char buf[256];
    (void)this_val;
    (void)argc;
    (void)argv;
    if (gethostname(buf, sizeof(buf)) != 0)
        return dyn_sys_throw(ctx, errno, "hostName", NULL);
    buf[sizeof(buf) - 1] = '\0';
    return JS_NewString(ctx, buf);
}

static JSValue dyn_sys_home_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* h;
    struct passwd pwd, *pw = NULL;
    char pwbuf[16384];
    (void)this_val;
    (void)argc;
    (void)argv;

    h = getenv("HOME");
    if (h && *h)
        return JS_NewString(ctx, h);
    if (getpwuid_r(getuid(), &pwd, pwbuf, sizeof pwbuf, &pw) == 0 && pw && pw->pw_dir)
        return JS_NewString(ctx, pw->pw_dir);
    return dyn_sys_throw(ctx, ENOENT, "homeDir", NULL);
}

static JSValue dyn_sys_memory_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSMemoryUsage u;
    struct rusage ru;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;

    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &u);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SET(name, v)                                                            \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewInt64(ctx, (int64_t)(v)), \
            JS_PROP_C_W_E)                                                      \
        < 0) {                                                                  \
        JS_FreeValue(ctx, o);                                                   \
        return JS_EXCEPTION;                                                    \
    }
    SET("mallocCount", u.malloc_count)
    SET("mallocSize", u.malloc_size)
    SET("memoryUsedCount", u.memory_used_count)
    SET("memoryUsedSize", u.memory_used_size)
    SET("objCount", u.obj_count)
    SET("objSize", u.obj_size)
    SET("strCount", u.str_count)
    SET("strSize", u.str_size)
    SET("propCount", u.prop_count)
    SET("shapeCount", u.shape_count)
    SET("arrayCount", u.array_count)
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__linux__)
        SET("peakRss", (int64_t)ru.ru_maxrss * 1024)
#else
        SET("peakRss", (int64_t)ru.ru_maxrss)
#endif
    } else {
        SET("peakRss", 0)
    }
    SET("nativeSize", (int64_t)dyn_nat_bytes())
    SET("nativeLimit", (int64_t)dyn_nat_limit())
#undef SET
    return o;
}

static JSValue dyn_sys_set_native_memory_limit(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t limit;
    double d;
    (void)this_val;
    (void)argc;

    if (!JS_IsNumber(argv[0]) || JS_ToFloat64(ctx, &d, argv[0]) != 0
        || !isfinite(d))
        return JS_ThrowTypeError(ctx,
            "setNativeMemoryLimit: bytes must be a finite number");
    if (JS_ToInt64(ctx, &limit, argv[0]))
        return JS_EXCEPTION;
    if (limit < 0)
        return JS_ThrowRangeError(ctx,
            "setNativeMemoryLimit: memory limit must be >= 0");
    {
        uint64_t ceiling = 0;
        if (dyn_nat_set_script_limit((uint64_t)limit, &ceiling))
            return JS_ThrowRangeError(ctx,
                "setNativeMemoryLimit: the operator limit is %llu bytes and cannot be raised or cleared by a script",
                (unsigned long long)ceiling);
    }
    return JS_UNDEFINED;
}

JSClassID dyn_spawn_class_id, dyn_spawn_pipe_class_id,
    dyn_spawn_stdin_class_id;

void spawn_settle(JSContext* ctx, JSValue* resolve, JSValue* reject,
    JSValue val, int failed)
{
    JSValue f = failed ? *reject : *resolve;
    JSValue r;

    if (!JS_IsUndefined(f)) {
        r = JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst*)&val);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, *resolve);
    JS_FreeValue(ctx, *reject);
    *resolve = *reject = JS_UNDEFINED;
    JS_FreeValue(ctx, val);
}

static void spawn_settle_wait(dyn_spawn_t* sp)
{
    if (sp->wait_settled || JS_IsUndefined(sp->wait_resolve))
        return;
    sp->wait_settled = 1;
    spawn_settle(sp->ctx, &sp->wait_resolve, &sp->wait_reject,
        spawn_wait_result(sp), 0);
    spawn_unpark(sp);
}

static void spawn_record_status(dyn_spawn_t* sp, int status)
{
    dyn_childreg_del(sp->pid);
    sp->exited = 1;
    sp->reaped = 1;
    sp->code = -1;
    sp->sig = 0;
    if (WIFEXITED(status))
        sp->code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        sp->sig = WTERMSIG(status);
}

int spawn_try_reap(dyn_spawn_t* sp)
{
    int status = 0;
    pid_t r;

    if (sp->exited || sp->pid <= 0)
        return 0;
    r = dyn_waitpid_eintr(sp->pid, &status, WNOHANG);
    if (r != sp->pid)
        return 0;
    spawn_record_status(sp, status);
    spawn_settle_wait(sp);
    return 1;
}

static void spawn_reap_blocking(dyn_spawn_t* sp)
{
    int status = 0;

    if (sp->pid <= 0 || sp->reaped)
        return;
    dyn_waitpid_eintr(sp->pid, &status, 0);
    spawn_record_status(sp, status);
}

static void spawn_teardown(dyn_spawn_t* sp)
{
    if (sp->torn_down)
        return;
    sp->torn_down = 1;
    spawn_kill_child(sp, SIGKILL);
    spawn_reap_blocking(sp);
    sp->exited = 1;
    spawn_close_fd(sp, &sp->out.fd);
    spawn_close_fd(sp, &sp->err.fd);
    spawn_close_fd(sp, &sp->in.fd);
    if (sp->hooked) {
        dyn_net_off_drain(sp);
        sp->hooked = 0;
    }
    if (sp->aio) {
        dyn_net_reactor_release_rt(sp->rt);
        sp->aio = NULL;
    }
}

void spawn_release(dyn_spawn_t* sp)
{
    if (--sp->refs > 0)
        return;
    JS_RemoveShutdownSweep(sp->rt, spawn_shutdown_sweep, sp);
    spawn_teardown(sp);
    JS_FreeValueRT(sp->rt, sp->wait_resolve);
    JS_FreeValueRT(sp->rt, sp->wait_reject);
    JS_FreeValueRT(sp->rt, sp->out.rbuf);
    JS_FreeValueRT(sp->rt, sp->out.rresolve);
    JS_FreeValueRT(sp->rt, sp->out.rreject);
    JS_FreeValueRT(sp->rt, sp->out.promise);
    JS_FreeValueRT(sp->rt, sp->err.rbuf);
    JS_FreeValueRT(sp->rt, sp->err.rresolve);
    JS_FreeValueRT(sp->rt, sp->err.rreject);
    JS_FreeValueRT(sp->rt, sp->err.promise);
    JS_FreeValueRT(sp->rt, sp->in.flush_resolve);
    JS_FreeValueRT(sp->rt, sp->in.flush_reject);
    free(sp->out.buf);
    free(sp->err.buf);
    free(sp->wq);
    sp->dead = 1;
    free(sp);
}

static void spawn_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(val, dyn_spawn_class_id);
    if (sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(sp);
    }
}

static JSClassDef dyn_spawn_class = {
    "Spawn",
    .finalizer = spawn_finalizer,
};

static JSClassDef dyn_spawn_pipe_class = {
    "SpawnPipe",
    .finalizer = spawn_pipe_finalizer,
};

static JSClassDef dyn_spawn_stdin_class = {
    "SpawnStdin",
    .finalizer = spawn_stdin_finalizer,
};

uint8_t* spawn_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
{
    size_t off, len, bpe, ab;
    JSValue buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    uint8_t* base;

    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base) {
            JS_ThrowTypeError(ctx,
                "Spawn: buf must be a byte-wide view (Uint8Array)");
            return NULL;
        }
        *plen = ab;
        return base;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx,
            "Spawn: buf must be a byte-wide view (Uint8Array)");
        return NULL;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base || off > ab || len > ab - off) {
        if (base)
            JS_ThrowRangeError(ctx, "Spawn: buf view out of bounds");
        return NULL;
    }
    *plen = len;
    return base + off;
}

static dyn_spawn_t* spawn_this(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead)
        JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return sp;
}

static JSValue dyn_spawn_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_t* sp = spawn_this(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    spawn_teardown(sp);
    spawn_settle_wait(sp);
    spawn_pipe_fail_pending(&sp->out, "read() aborted by close()");
    spawn_pipe_fail_pending(&sp->err, "read() aborted by close()");
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        JS_ThrowTypeError(ctx, "Spawn: stdin closed by close()");
        spawn_settle(ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            JS_GetException(ctx), 1);
        spawn_unpark(sp);
    }
    sp->user_closed = 1;
    JS_SetOpaque(this_val, NULL);
    spawn_release(sp);
    return JS_UNDEFINED;
}

static JSValue spawn_get_closed(JSContext* ctx, JSValueConst this_val)
{
    return JS_NewBool(ctx,
        JS_GetOpaque(this_val, dyn_spawn_class_id) == NULL);
}

static JSValue spawn_get_pid(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead)
        return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, (int32_t)sp->pid);
}

static JSValue spawn_get_exited(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead)
        return JS_TRUE;
    if (!sp->exited && spawn_try_reap(sp))
        spawn_after_reap(sp);
    return JS_NewBool(ctx, sp->exited);
}

JSValue spawn_view_get(JSContext* ctx, JSValueConst this_val,
    JSClassID cid, void* opaque)
{
    dyn_spawn_t* sp;
    JSValue proto, obj;

    sp = spawn_this(ctx, this_val);
    if (!sp)
        return JS_EXCEPTION;
    proto = dyn_ctor_proto(ctx, JS_UNDEFINED, cid);
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, cid);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, opaque);
    sp->refs++;
    return obj;
}

static JSValue spawn_get_stdout(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_pipe_class_id, &sp->out);
}

static JSValue spawn_get_stderr(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_pipe_class_id, &sp->err);
}

static const JSCFunctionListEntry dyn_spawn_proto_funcs[] = {
    JS_CFUNC_DEF("wait", 0, dyn_spawn_wait),
    JS_CFUNC_DEF("kill", 1, dyn_spawn_kill),
    JS_CFUNC_DEF("close", 0, dyn_spawn_close),
    JS_CFUNC_DEF("dispose", 0, dyn_spawn_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, dyn_spawn_close),
    JS_CGETSET_DEF("closed", spawn_get_closed, NULL),
    JS_CGETSET_DEF("pid", spawn_get_pid, NULL),
    JS_CGETSET_DEF("exited", spawn_get_exited, NULL),
    JS_CGETSET_DEF("stdout", spawn_get_stdout, NULL),
    JS_CGETSET_DEF("stderr", spawn_get_stderr, NULL),
    JS_CGETSET_DEF("stdin", spawn_get_stdin, NULL),
};

static const JSCFunctionListEntry dyn_spawn_pipe_proto_funcs[] = {
    JS_CFUNC_DEF("read", 1, spawn_pipe_read),
    JS_CFUNC_DEF("close", 0, spawn_pipe_close),
    JS_CFUNC_DEF("dispose", 0, spawn_pipe_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, spawn_pipe_close),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, spawn_pipe_async_iterator),
};

static const JSCFunctionListEntry dyn_spawn_stdin_proto_funcs[] = {
    JS_CFUNC_DEF("write", 1, spawn_stdin_write),
    JS_CFUNC_DEF("flush", 0, spawn_stdin_flush),
    JS_CFUNC_DEF("close", 0, spawn_stdin_close),
    JS_CFUNC_DEF("dispose", 0, spawn_stdin_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, spawn_stdin_close),
};

int spawn_opt_int(JSContext* ctx, JSValueConst o, const char* key,
    int64_t floor_, const char* what, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    double d;
    int64_t t = 0;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) != 0 || !isfinite(d)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Spawn: %s must be a finite number", key);
        return -1;
    }
    if (JS_ToInt64(ctx, &t, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (t < floor_) {
        JS_ThrowRangeError(ctx, "Spawn: %s", what);
        return -1;
    }
    *out = t;
    return 0;
}

int spawn_opt_id(JSContext* ctx, JSValueConst o, const char* key,
    int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    double d;
    int64_t t;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) != 0 || !isfinite(d)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Spawn: %s must be a finite integer id", key);
        return -1;
    }
    if (JS_ToInt64(ctx, &t, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (t < 0 || t >= (int64_t)UINT_MAX) {
        JS_ThrowRangeError(ctx,
            "Spawn: %s must be a non-negative integer id", key);
        return -1;
    }
    *out = t;
    return 0;
}

static int dyn_spawn_register_classes(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto;

    JS_NewClassID(&dyn_spawn_class_id);
    if (JS_NewClass(rt, dyn_spawn_class_id, &dyn_spawn_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_spawn_proto_funcs,
        (int)countof(dyn_spawn_proto_funcs));
    JS_SetClassProto(ctx, dyn_spawn_class_id, proto);

    JS_NewClassID(&dyn_spawn_pipe_class_id);
    if (JS_NewClass(rt, dyn_spawn_pipe_class_id, &dyn_spawn_pipe_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_spawn_pipe_proto_funcs,
        (int)countof(dyn_spawn_pipe_proto_funcs));
    JS_SetClassProto(ctx, dyn_spawn_pipe_class_id, proto);

    JS_NewClassID(&dyn_spawn_stdin_class_id);
    if (JS_NewClass(rt, dyn_spawn_stdin_class_id, &dyn_spawn_stdin_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_spawn_stdin_proto_funcs,
        (int)countof(dyn_spawn_stdin_proto_funcs));
    JS_SetClassProto(ctx, dyn_spawn_stdin_class_id, proto);
    return 0;
}

static int dyn_spawn_register_export(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;

    if (dyn_spawn_register_classes(ctx) < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyn_spawn_class_id);
    if (!JS_IsObject(proto))
        return -1;
    ctor = JS_NewCFunction2(ctx, dyn_spawn_ctor, "Spawn", 1,
        JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    return JS_SetModuleExport(ctx, m, "Spawn", ctor);
}

static const JSCFunctionListEntry dyn_sys_funcs[] = {
    JS_CFUNC_DEF("env", 0, dyn_sys_env),
    JS_CFUNC_DEF("getEnv", 1, dyn_sys_get_env),
    JS_CFUNC_DEF("setEnv", 2, dyn_sys_set_env),
    JS_CFUNC_DEF("args", 0, dyn_sys_args),
    JS_CFUNC_DEF("cwd", 0, dyn_sys_cwd),
    JS_CFUNC_DEF("chDir", 1, dyn_sys_chdir),
    JS_CFUNC_DEF("platform", 0, dyn_sys_platform),
    JS_CFUNC_DEF("arch", 0, dyn_sys_arch),
    JS_CFUNC_DEF("uname", 0, dyn_sys_uname),
    JS_CFUNC_DEF("getuid", 0, dyn_sys_getuid),
    JS_CFUNC_DEF("getgid", 0, dyn_sys_getgid),
    JS_CFUNC_DEF("setUid", 1, dyn_sys_setuid),
    JS_CFUNC_DEF("setGid", 1, dyn_sys_setgid),
    JS_CFUNC_DEF("cpuUsage", 0, dyn_sys_cpu_usage),
    JS_CFUNC_DEF("threadCpuNow", 0, dyn_sys_thread_cpu_now),
    JS_CFUNC_DEF("rusage", 0, dyn_sys_rusage),
    JS_CFUNC_DEF("parStats", 0, dyn_sys_par_stats),
    JS_CFUNC_DEF("pid", 0, dyn_sys_pid),
    JS_CFUNC_DEF("hostName", 0, dyn_sys_host_name),

    JS_CFUNC_DEF("cpuInfo", 0, dyn_sys_cpu_info),
    JS_CFUNC_DEF("memInfo", 0, dyn_sys_mem_info),
    JS_CFUNC_DEF("loadAvg", 0, dyn_sys_load_avg),
    JS_CFUNC_DEF("uptime", 0, dyn_sys_uptime),
    JS_CFUNC_DEF("diskUsage", 1, dyn_sys_disk_usage),
    JS_CFUNC_DEF("homeDir", 0, dyn_sys_home_dir),
    JS_CFUNC_DEF("memoryUsage", 0, dyn_sys_memory_usage),
    JS_CFUNC_DEF("setNativeMemoryLimit", 1, dyn_sys_set_native_memory_limit),
    JS_CFUNC_DEF("Exec", 1, dyn_exec),
    JS_CFUNC_DEF("Which", 1, dyn_which),
};

static int dyn_sys_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_spawn_register_export(ctx, m) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_sys_funcs,
        (int)countof(dyn_sys_funcs));
}

int js_nat_init_sys(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:sys", dyn_sys_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Spawn") < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_sys_funcs,
        (int)countof(dyn_sys_funcs));
}

#endif
