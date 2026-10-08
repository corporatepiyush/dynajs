#include "dyna-nat.h"
#include "cutils.h"
#include "dyna-simd-kernels.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

#include "dyna-utf8-lossy.inc.c"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <grp.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#endif
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define dyn_environ (*_NSGetEnviron())
#else
extern char** environ;
#define dyn_environ environ
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DYN_SPAWN_MAXPIPE_CAP (1 << 30)

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

static JSValue dyn_sys_throw(JSContext* ctx, int e, const char* op,
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

static const char* dyn_sys_cstr(JSContext* ctx, JSValueConst v,
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

static JSValue dyn_sys_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    char** envp = dyn_environ;
    JSValue obj;
    uint32_t idx;
    (void)this_val;
    (void)argc;
    (void)argv;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (idx = 0; envp[idx] != NULL; idx++) {
        const char* entry = envp[idx];
        const char* eq = strchr(entry, '=');
        JSAtom atom;
        if (!eq)
            continue;
        atom = JS_NewAtomLen(ctx, entry, (size_t)(eq - entry));
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValue(ctx, obj, atom,
            dyn_utf8_lossy_string(ctx, (const uint8_t*)(eq + 1),
                strlen(eq + 1)),
            JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    return obj;
}

static JSValue dyn_sys_get_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *name, *val;
    JSValue out;
    (void)this_val;
    (void)argc;

    name = dyn_sys_cstr(ctx, argv[0], "getEnv: name");
    if (!name)
        return JS_EXCEPTION;
    val = getenv(name);
    out = val ? dyn_utf8_lossy_string(ctx, (const uint8_t*)val, strlen(val))
              : JS_UNDEFINED;
    JS_FreeCString(ctx, name);
    return out;
}

static JSValue dyn_sys_set_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *name, *val;
    size_t name_len, val_len;
    int r;
    (void)this_val;
    (void)argc;

    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = JS_ToCStringLen(ctx, &val_len, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (strlen(name) != name_len || strlen(val) != val_len || strchr(name, '=') != NULL || name_len == 0) {
        JSValue ex = JS_ThrowTypeError(ctx,
            "setEnv: name must be non-empty and free of '=' and NUL, "
            "and the value free of NUL");
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        return ex;
    }
    r = setenv(name, val, 1);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "setEnv", name);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        return ex;
    }
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
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
                    strlen(av[i])), JS_PROP_C_W_E);
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
                            (const uint8_t*)(buf + start), i - start), JS_PROP_C_W_E);
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
    struct passwd* pw;
    (void)this_val;
    (void)argc;
    (void)argv;

    h = getenv("HOME");
    if (h && *h)
        return JS_NewString(ctx, h);
    pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
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

#include "dyna-machine.inc.c"

#include "dyna-proc.inc.c"
struct dyn_aio;
struct dyn_aio* dyn_net_reactor_acquire(JSContext* ctx);
void dyn_net_reactor_release(JSContext* ctx);
void dyn_net_reactor_release_rt(JSRuntime* rt);
int dyn_net_on_drain(void (*fn)(void*), void* udata);
void dyn_net_off_drain(void* udata);

#include <strings.h>

#include "dyna-aio.h"
#include "dyna-evloop.h"

typedef struct dyn_spawn dyn_spawn_t;

typedef struct {
    dyn_spawn_t* sp;
    int fd;
    int eof;
    int err;
    int watch;
    uint8_t* buf;
    size_t len, cap, head;
    int pending;
    JSValue rbuf;
    JSValue rresolve, rreject;
    JSValue promise;
} dyn_spawn_pipe_t;

static void spawn_pipe_consume(dyn_spawn_pipe_t* p, size_t n)
{
    if (n < p->len) {
        p->head += n;
        p->len -= n;
    } else {
        p->head = 0;
        p->len = 0;
    }
}

static int spawn_pipe_room(dyn_spawn_pipe_t* p, size_t r)
{
    if (p->head + p->len + r <= p->cap)
        return 0;
    if (p->head && p->head >= p->len) {
        memmove(p->buf, p->buf + p->head, p->len);
        p->head = 0;
    }
    if (p->head + p->len + r > p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 4096;
        uint8_t* nb;
        while (nc < p->head + p->len + r)
            nc *= 2;
        nb = (uint8_t*)realloc(p->buf, nc);
        if (!nb)
            return -1;
        p->buf = nb;
        p->cap = nc;
    }
    return 0;
}

typedef struct {
    dyn_spawn_t* sp;
    int fd;
    int piped;
    int close_after_drain;
    int err;
    int err_errno;
    JSValue flush_resolve, flush_reject;
} dyn_spawn_stdin_t;

struct dyn_spawn {
    int refs;
    int torn_down;
    int user_closed;
    JSRuntime* rt;
    JSContext* ctx;
    pid_t pid;
    int exited, code, sig, timed_out, reaped;
    int wait_settled;
    JSValue wait_resolve, wait_reject;
    dyn_spawn_pipe_t out, err;
    dyn_spawn_stdin_t in;
    uint8_t* wq;
    size_t wlen, wcap, woff;
    size_t maxpipe;
    struct dyn_aio* aio;
    int hooked;
    int64_t deadline, kill_at;
    int term_sent;
    int dead;
};

static JSClassID dyn_spawn_class_id, dyn_spawn_pipe_class_id,
    dyn_spawn_stdin_class_id;

static void spawn_release(dyn_spawn_t* sp);
static void spawn_unpark(dyn_spawn_t* sp);
static void spawn_pipe_force_eof(dyn_spawn_pipe_t* p);
static uint8_t* spawn_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen);

static JSValue spawn_promise_new(JSContext* ctx, JSValue* resolve,
    JSValue* reject)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    *resolve = funcs[0];
    *reject = funcs[1];
    return promise;
}

static void spawn_settle(JSContext* ctx, JSValue* resolve, JSValue* reject,
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

static JSValue spawn_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, val, 0);
    return promise;
}

static JSValue spawn_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, exc, 1);
    return promise;
}

static JSValue spawn_cap_error(JSContext* ctx, dyn_spawn_t* sp, int kind)
{
    (void)sp;
    if (kind == 2)
        JS_ThrowOutOfMemory(ctx);
    else
        JS_ThrowInternalError(ctx, "Spawn: pipe drain failed");
    return JS_GetException(ctx);
}

static void spawn_close_fd(dyn_spawn_t* sp, int* fd)
{
    if (*fd >= 0) {
        if (sp->aio)
            dyn_evloop_del(dyn_aio_evloop(sp->aio), *fd);
        close(*fd);
        *fd = -1;
    }
}

static void spawn_kill_child(dyn_spawn_t* sp, int sig)
{
    if (!sp->exited && sp->pid > 0)
        dyn_kill_group(sp->pid, sig);
}

static void spawn_pipe_watch(dyn_spawn_pipe_t* p, int on)
{
    dyn_spawn_t* sp = p->sp;

    if (p->fd < 0 || p->eof || !sp->aio)
        return;
    if (on == p->watch)
        return;
    if (dyn_evloop_mod(dyn_aio_evloop(sp->aio), p->fd,
            on ? DYN_EV_READ : 0)
        == 0)
        p->watch = on;
}

static void spawn_aio_release_if_idle(dyn_spawn_t* sp)
{
    if (sp->aio) {
        if (!sp->exited || sp->out.fd >= 0 || sp->err.fd >= 0
            || sp->in.fd >= 0)
            return;
        dyn_net_reactor_release_rt(sp->rt);
        sp->aio = NULL;
    }
}

static JSValue spawn_wait_result(dyn_spawn_t* sp)
{
    JSContext* ctx = sp->ctx;
    JSValue o, v;
    const char* signame = sp->sig ? dyn_signal_name(sp->sig) : NULL;

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    v = signame ? JS_NULL : JS_NewInt32(ctx, sp->code);
    if (JS_DefinePropertyValueStr(ctx, o, "code", v, JS_PROP_C_W_E) < 0)
        goto fail;
    if (signame) {
        v = JS_NewString(ctx, signame);
        if (!JS_IsException(v) && JS_DefinePropertyValueStr(ctx, o, "signal", v, JS_PROP_C_W_E) >= 0) {
            v = JS_UNDEFINED;
        } else {
            JS_FreeValue(ctx, v);
            goto fail;
        }
    } else {
        if (JS_DefinePropertyValueStr(ctx, o, "signal", JS_NULL, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "timedOut",
            JS_NewBool(ctx, sp->timed_out), JS_PROP_C_W_E)
        < 0)
        goto fail;
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static void spawn_unpark(dyn_spawn_t* sp);

static void spawn_wait_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)opaque;
    if (sp->wait_settled || JS_IsUndefined(sp->wait_resolve))
        return;
    sp->wait_settled = 1;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc))
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, "Spawn.wait: aborted at engine shutdown"),
                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        else
            exc = JS_GetException(ctx);
        r = JS_Call(ctx, sp->wait_reject, JS_UNDEFINED, 1,
            (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValueRT(rt, sp->wait_resolve);
    JS_FreeValueRT(rt, sp->wait_reject);
    sp->wait_resolve = JS_UNDEFINED;
    sp->wait_reject = JS_UNDEFINED;
    spawn_unpark(sp);
}

static void spawn_settle_wait(dyn_spawn_t* sp)
{
    if (sp->wait_settled || JS_IsUndefined(sp->wait_resolve))
        return;
    JS_RemoveShutdownSweep(sp->rt, spawn_wait_sweep, sp);
    sp->wait_settled = 1;
    spawn_settle(sp->ctx, &sp->wait_resolve, &sp->wait_reject,
        spawn_wait_result(sp), 0);
    spawn_unpark(sp);
}

static void spawn_record_status(dyn_spawn_t* sp, int status)
{
    sp->exited = 1;
    sp->reaped = 1;
    sp->code = -1;
    sp->sig = 0;
    if (WIFEXITED(status))
        sp->code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        sp->sig = WTERMSIG(status);
}

static int spawn_try_reap(dyn_spawn_t* sp)
{
    int status = 0;
    pid_t r;

    if (sp->exited || sp->pid <= 0)
        return 0;
    do {
        r = waitpid(sp->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r != sp->pid)
        return 0;
    spawn_record_status(sp, status);
    spawn_settle_wait(sp);
    return 1;
}

static void spawn_reap_blocking(dyn_spawn_t* sp)
{
    int status = 0;
    pid_t r;

    if (sp->pid <= 0 || sp->reaped)
        return;
    do {
        r = waitpid(sp->pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    spawn_record_status(sp, status);
}

static void spawn_unhook_if_done(dyn_spawn_t* sp)
{
    if (sp->exited && sp->hooked) {
        dyn_net_off_drain(sp);
        sp->hooked = 0;
    }
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

static void spawn_ref(dyn_spawn_t* sp)
{
    sp->refs++;
}

static void spawn_unpark(dyn_spawn_t* sp)
{
    spawn_release(sp);
}

static void spawn_release(dyn_spawn_t* sp)
{
    if (--sp->refs > 0)
        return;
    JS_RemoveShutdownSweep(sp->rt, spawn_wait_sweep, sp);
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

static void spawn_pipe_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spawn_pipe_t* p = (dyn_spawn_pipe_t*)JS_GetOpaque(val,
        dyn_spawn_pipe_class_id);
    if (p && p->sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(p->sp);
    }
}

static void spawn_stdin_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spawn_stdin_t* in = (dyn_spawn_stdin_t*)JS_GetOpaque(val,
        dyn_spawn_stdin_class_id);
    if (in && in->sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(in->sp);
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

static void spawn_reap_hook(void* ud)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)ud;
    int64_t now;

    spawn_ref(sp);
    if (sp->dead)
        goto out;
    if (spawn_try_reap(sp)) {
        if (sp->timed_out) {
            spawn_pipe_force_eof(&sp->out);
            spawn_pipe_force_eof(&sp->err);
        }
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
        goto out;
    }
    now = dyn_now_ms();
    if (sp->deadline && !sp->term_sent && now >= sp->deadline) {
        sp->term_sent = 1;
        sp->timed_out = 1;
        dyn_kill_group(sp->pid, SIGTERM);
        sp->kill_at = now + DYN_EXEC_GRACE_MS;
    } else if (sp->term_sent && sp->kill_at && now >= sp->kill_at) {
        dyn_kill_group(sp->pid, SIGKILL);
        sp->kill_at = 0;
    }
out:
    spawn_release(sp);
}

static void spawn_pipe_feed(dyn_spawn_pipe_t* p)
{
    dyn_spawn_t* sp = p->sp;
    JSValue rr, rj;
    size_t n;

    if (!p->pending)
        return;
    p->pending = 0;
    rr = p->rresolve;
    rj = p->rreject;
    p->rresolve = p->rreject = JS_UNDEFINED;
    JS_FreeValue(sp->ctx, p->promise);
    p->promise = JS_UNDEFINED;
    if (p->err) {
        JS_FreeValue(sp->ctx, p->rbuf);
        p->rbuf = JS_UNDEFINED;
        spawn_settle(sp->ctx, &rr, &rj, spawn_cap_error(sp->ctx, sp, p->err),
            1);
        spawn_unpark(sp);
        return;
    }
    {
        size_t rlen = 0;
        uint8_t* rbase = spawn_view_bytes(sp->ctx, p->rbuf, &rlen);
        if (!rbase) {
            JSValue ex = JS_GetException(sp->ctx);
            if (JS_IsUndefined(ex)) {
                JS_FreeValue(sp->ctx, ex);
                ex = JS_ThrowTypeError(sp->ctx,
                    "Spawn: read buffer is detached or out of bounds");
                ex = JS_GetException(sp->ctx);
            }
            JS_FreeValue(sp->ctx, p->rbuf);
            p->rbuf = JS_UNDEFINED;
            spawn_settle(sp->ctx, &rr, &rj, ex, 1);
            spawn_unpark(sp);
            return;
        }
        n = p->len < rlen ? p->len : rlen;
        if (n)
            memmove(rbase, p->buf + p->head, n);
    }
    spawn_pipe_consume(p, n);
    JS_FreeValue(sp->ctx, p->rbuf);
    p->rbuf = JS_UNDEFINED;
    spawn_pipe_watch(p, 1);
    spawn_settle(sp->ctx, &rr, &rj, JS_NewInt64(sp->ctx, (int64_t)n), 0);
    spawn_unpark(sp);
}

static void spawn_pipe_force_eof(dyn_spawn_pipe_t* p)
{
    dyn_spawn_t* sp = p->sp;

    if (p->fd >= 0) {
        for (;;) {
            uint8_t chunk[DYN_EXEC_CHUNK];
            size_t space = p->len < sp->maxpipe ? sp->maxpipe - p->len : 0;
            size_t want = space < sizeof(chunk) ? space : sizeof(chunk);
            ssize_t r;
            if (want == 0)
                break;
            r = read(p->fd, chunk, want);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (r == 0)
                break;
            if (spawn_pipe_room(p, (size_t)r) < 0)
                break;
            memcpy(p->buf + p->head + p->len, chunk, (size_t)r);
            p->len += (size_t)r;
        }
        spawn_close_fd(sp, &p->fd);
    }
    p->eof = 1;
    if (p->pending)
        spawn_pipe_feed(p);
}

static void spawn_pipe_fail(dyn_spawn_pipe_t* p, int kind)
{
    dyn_spawn_t* sp = p->sp;

    if (!p->err)
        p->err = kind;
    spawn_kill_child(sp, SIGKILL);
    spawn_close_fd(sp, &p->fd);
    p->eof = 1;
    if (p->pending)
        spawn_pipe_feed(p);
    spawn_try_reap(sp);
}

static void spawn_pipe_cb(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    dyn_spawn_pipe_t* p = (dyn_spawn_pipe_t*)ud;
    dyn_spawn_t* sp = p->sp;
    uint8_t chunk[DYN_EXEC_CHUNK];

    (void)lp;
    (void)fd;
    (void)events;
    spawn_ref(sp);
    if (sp->dead || p->fd < 0)
        goto out;
    if (p->len >= sp->maxpipe) {
        spawn_pipe_watch(p, 0);
        goto out;
    }
    for (;;) {
        size_t space, want;
        ssize_t r;

        space = sp->maxpipe - p->len;
        want = sizeof chunk;
        if (space < want)
            want = space;
        r = read(p->fd, chunk, want);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                goto out;
            r = 0;
        }
        if (r == 0) {
            spawn_close_fd(sp, &p->fd);
            p->eof = 1;
            if (p->pending)
                spawn_pipe_feed(p);
            if (spawn_try_reap(sp))
                spawn_unhook_if_done(sp);
            spawn_aio_release_if_idle(sp);
            goto out;
        }
        if (spawn_pipe_room(p, (size_t)r) < 0) {
            spawn_pipe_fail(p, 1);
            goto out;
        }
        memcpy(p->buf + p->head + p->len, chunk, (size_t)r);
        p->len += (size_t)r;
        if (p->pending)
            spawn_pipe_feed(p);
        else if (p->len >= sp->maxpipe)
            spawn_pipe_watch(p, 0);
        goto out;
    }
out:
    spawn_release(sp);
}

static uint8_t* spawn_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
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

static void spawn_pipe_fail_pending(dyn_spawn_pipe_t* p, const char* msg)
{
    dyn_spawn_t* sp = p->sp;

    if (!p->pending)
        return;
    p->pending = 0;
    JS_ThrowTypeError(sp->ctx, "Spawn: %s", msg);
    spawn_settle(sp->ctx, &p->rresolve, &p->rreject, JS_GetException(sp->ctx),
        1);
    JS_FreeValue(sp->ctx, p->rbuf);
    JS_FreeValue(sp->ctx, p->promise);
    p->rbuf = JS_UNDEFINED;
    p->promise = JS_UNDEFINED;
    spawn_unpark(sp);
}

static JSValue spawn_pipe_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_pipe_t* p;
    uint8_t* base;
    size_t len = 0;
    JSValue promise, resolve, reject;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: read(buf) needs a Uint8Array");
    base = spawn_view_bytes(ctx, argv[0], &len);
    if (!base)
        return JS_EXCEPTION;
    p = (dyn_spawn_pipe_t*)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
    if (!p || !p->sp || p->sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: read on a closed stdio");
    if (p->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (len == 0)
        return spawn_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (p->pending)
        return JS_ThrowTypeError(ctx,
            "Spawn: a read() is already pending on this pipe");
    if (p->err)
        return spawn_promise_rejected(ctx,
            spawn_cap_error(ctx, p->sp, p->err));
    if (p->len > 0) {
        size_t n = p->len < len ? p->len : len;
        memcpy(base, p->buf + p->head, n);
        spawn_pipe_consume(p, n);
        spawn_pipe_watch(p, 1);
        return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
    if (p->eof)
        return spawn_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    p->pending = 1;
    p->rbuf = JS_DupValue(ctx, argv[0]);
    p->rresolve = resolve;
    p->rreject = reject;
    p->promise = JS_DupValue(ctx, promise);
    spawn_ref(p->sp);
    return promise;
}

static JSValue spawn_pipe_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_pipe_t* p;
    (void)argc;
    (void)argv;

    p = (dyn_spawn_pipe_t*)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
    if (!p || !p->sp || p->sp->dead)
        return JS_UNDEFINED;
    if (p->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    spawn_pipe_fail_pending(p, "read() aborted by close()");
    spawn_pipe_watch(p, 0);
    spawn_close_fd(p->sp, &p->fd);
    p->eof = 1;
    return JS_UNDEFINED;
}

static void spawn_stdin_unwatch(dyn_spawn_t* sp)
{
    if (sp->in.fd >= 0 && sp->aio)
        dyn_evloop_del(dyn_aio_evloop(sp->aio), sp->in.fd);
}

static void spawn_stdin_close_now(dyn_spawn_t* sp)
{
    spawn_stdin_unwatch(sp);
    if (sp->in.fd >= 0) {
        close(sp->in.fd);
        sp->in.fd = -1;
    }
    spawn_aio_release_if_idle(sp);
}

static JSValue spawn_stdin_error(JSContext* ctx, dyn_spawn_stdin_t* in)
{
    JS_ThrowInternalError(ctx, "Spawn: stdin write failed (%s)",
        strerror(in->err_errno ? in->err_errno : EPIPE));
    return JS_GetException(ctx);
}

static void spawn_stdin_reject_flush(dyn_spawn_t* sp)
{
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            spawn_stdin_error(sp->ctx, &sp->in), 1);
        spawn_unpark(sp);
    }
}

static void spawn_stdin_cb(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    dyn_spawn_stdin_t* in = (dyn_spawn_stdin_t*)ud;
    dyn_spawn_t* sp = in->sp;

    (void)lp;
    (void)fd;
    (void)events;
    spawn_ref(sp);
    if (sp->dead || in->fd < 0)
        goto out;
    while (sp->woff < sp->wlen) {
        ssize_t w = write(in->fd, sp->wq + sp->woff, sp->wlen - sp->woff);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                goto out;
            in->err = 1;
            in->err_errno = errno;
            spawn_stdin_close_now(sp);
            spawn_stdin_reject_flush(sp);
            goto out;
        }
        sp->woff += (size_t)w;
    }
    sp->woff = sp->wlen = 0;
    spawn_stdin_unwatch(sp);
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
            JS_UNDEFINED, 0);
        spawn_unpark(sp);
    }
    if (in->close_after_drain)
        spawn_stdin_close_now(sp);
out:
    spawn_release(sp);
}

static int spawn_stdin_enqueue(dyn_spawn_t* sp, const uint8_t* src, size_t n)
{
    if (n > sp->maxpipe || sp->wlen > sp->maxpipe - n) {
        JS_ThrowRangeError(sp->ctx,
            "Spawn: stdin queue exceeds maxPipe (%u bytes)",
            (unsigned)sp->maxpipe);
        return -1;
    }
    if (sp->wlen + n > sp->wcap) {
        size_t nc = sp->wcap ? sp->wcap : 4096;
        uint8_t* nw;
        while (nc < sp->wlen + n)
            nc *= 2;
        nw = (uint8_t*)realloc(sp->wq, nc);
        if (!nw) {
            JS_ThrowOutOfMemory(sp->ctx);
            return -1;
        }
        sp->wq = nw;
        sp->wcap = nc;
    }
    memcpy(sp->wq + sp->wlen, src, n);
    sp->wlen += n;
    if (sp->in.fd >= 0 && sp->aio)
        dyn_evloop_mod(dyn_aio_evloop(sp->aio), sp->in.fd, DYN_EV_WRITE);
    return 0;
}

static JSValue spawn_stdin_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    const uint8_t* src;
    size_t n = 0;
    int is_str;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: stdin.write(buf) needs bytes");
    is_str = JS_IsString(argv[0]);
    if (is_str) {
        const char* cs;
        n = 0;
        cs = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!cs)
            return JS_EXCEPTION;
        src = (const uint8_t*)cs;
    } else {
        src = spawn_view_bytes(ctx, argv[0], &n);
        if (!src)
            return JS_EXCEPTION;
    }
    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_ThrowTypeError(ctx, "Spawn: write on a closed stdin");
    }
    if (in->sp->user_closed) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    }
    if (in->err || in->fd < 0) {
        JSValue r;
        if (in->err)
            r = spawn_promise_rejected(ctx, spawn_stdin_error(ctx, in));
        else if (!in->piped)
            r = (JSValue)JS_ThrowTypeError(ctx,
                "Spawn: stdin is not piped (pass opts.stdin: \"pipe\")");
        else
            r = (JSValue)JS_ThrowTypeError(ctx,
                "Spawn: stdin is closed (EOF already sent)");
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return r;
    }
    if (n && spawn_stdin_enqueue(in->sp, src, n) < 0) {
        if (is_str)
            JS_FreeCString(ctx, (const char*)src);
        return JS_EXCEPTION;
    }
    if (is_str)
        JS_FreeCString(ctx, (const char*)src);
    return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
}

static JSValue spawn_stdin_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    JSValue promise, resolve, reject;
    (void)argc;
    (void)argv;

    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: flush on a closed stdin");
    if (in->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (in->err)
        return spawn_promise_rejected(ctx, spawn_stdin_error(ctx, in));
    if (in->sp->woff >= in->sp->wlen)
        return spawn_promise_resolved(ctx, JS_UNDEFINED);
    if (!JS_IsUndefined(in->flush_resolve))
        return JS_ThrowTypeError(ctx,
            "Spawn: a flush() is already pending on stdin");
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    in->flush_resolve = resolve;
    in->flush_reject = reject;
    spawn_ref(in->sp);
    return promise;
}

static JSValue spawn_stdin_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_stdin_t* in;
    (void)argc;
    (void)argv;

    in = (dyn_spawn_stdin_t*)JS_GetOpaque(this_val,
        dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead)
        return JS_UNDEFINED;
    if (in->sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (in->fd < 0)
        return JS_UNDEFINED;
    in->close_after_drain = 1;
    if (in->sp->woff >= in->sp->wlen)
        spawn_stdin_close_now(in->sp);
    return JS_UNDEFINED;
}

static dyn_spawn_t* spawn_this(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead)
        JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return sp;
}

static JSValue dyn_spawn_wait(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    JSValue promise, resolve, reject;
    (void)argc;
    (void)argv;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        return spawn_promise_resolved(ctx, spawn_wait_result(sp));
    if (!sp->exited && spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    if (sp->exited)
        return spawn_promise_resolved(ctx, spawn_wait_result(sp));
    if (!JS_IsUndefined(sp->wait_resolve))
        return JS_ThrowTypeError(ctx, "Spawn: wait() is already pending");
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    sp->wait_resolve = resolve;
    sp->wait_reject = reject;
    JS_AddShutdownSweep(sp->rt, spawn_wait_sweep, sp);
    spawn_ref(sp);
    if (spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return promise;
}

static int spawn_sig_from_name(const char* s)
{
    static const struct {
        const char* nm;
        int sig;
    } tab[] = {
        { "HUP", SIGHUP },
        { "INT", SIGINT },
        { "QUIT", SIGQUIT },
        { "ILL", SIGILL },
        { "ABRT", SIGABRT },
        { "FPE", SIGFPE },
        { "KILL", SIGKILL },
        { "SEGV", SIGSEGV },
        { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM },
        { "TERM", SIGTERM },
        { "BUS", SIGBUS },
        { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 },
        { "CHLD", SIGCHLD },
        { "CONT", SIGCONT },
        { "STOP", SIGSTOP },
        { "TSTP", SIGTSTP },
        { "TTIN", SIGTTIN },
        { "TTOU", SIGTTOU },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcasecmp(tab[i].nm, s) == 0)
            return tab[i].sig;
    return 0;
}

static JSValue dyn_spawn_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    int sig = SIGTERM;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        return JS_FALSE;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
            const char* s = JS_ToCString(ctx, argv[0]);
            const char* nm;
            if (!s)
                return JS_EXCEPTION;
            nm = !strncasecmp(s, "SIG", 3) ? s + 3 : s;
            sig = spawn_sig_from_name(nm);
            JS_FreeCString(ctx, s);
            if (sig <= 0)
                return JS_ThrowRangeError(ctx, "Spawn: unknown signal name");
        } else if (JS_IsNumber(argv[0])) {
            int32_t n;
            if (JS_ToInt32(ctx, &n, argv[0]))
                return JS_EXCEPTION;
            if (n <= 0 || n > 31)
                return JS_ThrowRangeError(ctx,
                    "Spawn: signal number out of range");
            sig = n;
        } else {
            return JS_ThrowTypeError(ctx,
                "Spawn: kill(signal) takes a number or a \"SIGxxx\" string");
        }
    }
    if (sp->exited)
        return JS_FALSE;
    dyn_kill_group(sp->pid, sig);
    if (spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return JS_TRUE;
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
    if (!sp->exited && spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return JS_NewBool(ctx, sp->exited);
}

static JSValue spawn_view_get(JSContext* ctx, JSValueConst this_val,
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

static JSValue spawn_get_stdin(JSContext* ctx, JSValueConst this_val)
{
    dyn_spawn_t* sp = (dyn_spawn_t*)JS_GetOpaque(this_val,
        dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_stdin_class_id, &sp->in);
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
};

static const JSCFunctionListEntry dyn_spawn_stdin_proto_funcs[] = {
    JS_CFUNC_DEF("write", 1, spawn_stdin_write),
    JS_CFUNC_DEF("flush", 0, spawn_stdin_flush),
    JS_CFUNC_DEF("close", 0, spawn_stdin_close),
    JS_CFUNC_DEF("dispose", 0, spawn_stdin_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, spawn_stdin_close),
};

static int spawn_strv_at(JSContext* ctx, JSValueConst arr, uint32_t i,
    char** slot)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    const char* s;

    if (JS_IsException(e))
        return -1;
    if (!JS_IsString(e)) {
        JS_FreeValue(ctx, e);
        JS_ThrowTypeError(ctx, "Spawn: every argument must be a string");
        return -1;
    }
    s = dyn_sys_cstr(ctx, e, "Spawn: args");
    JS_FreeValue(ctx, e);
    if (!s)
        return -1;
    *slot = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*slot) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static char** spawn_strv(JSContext* ctx, JSValueConst arr, const char* first)
{
    uint32_t n = 0, i, k = 0;
    char** v;
    JSValue lv;

    if (JS_IsArray(ctx, arr) == 1) {
        lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv) < 0) {
            JS_FreeValue(ctx, lv);
            return NULL;
        }
        JS_FreeValue(ctx, lv);
        if (n > 65535) {
            JS_ThrowRangeError(ctx, "Spawn: too many arguments");
            return NULL;
        }
    }
    v = (char**)calloc((size_t)n + 2, sizeof *v);
    if (!v) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    if (first) {
        v[k] = strdup(first);
        if (!v[k]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        k++;
    }
    for (i = 0; i < n; i++) {
        if (spawn_strv_at(ctx, arr, i, &v[k]) < 0)
            goto fail;
        k++;
    }
    return v;
fail:
    for (i = 0; i < k; i++)
        free(v[i]);
    free(v);
    return NULL;
}

static int spawn_env_at(JSContext* ctx, JSValueConst obj, JSAtom atom,
    char** slot)
{
    JSValue val = JS_GetProperty(ctx, obj, atom);
    const char *nm, *vs;
    size_t nm_len, sz;

    nm = JS_AtomToCStringLen(ctx, &nm_len, atom);
    if (!nm) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    if (strlen(nm) != nm_len) {
        JS_ThrowTypeError(ctx, "Spawn: env name contains a NUL byte");
        JS_FreeCString(ctx, nm);
        JS_FreeValue(ctx, val);
        return -1;
    }
    vs = JS_IsException(val) ? NULL : dyn_sys_cstr(ctx, val, "Spawn: env value");
    JS_FreeValue(ctx, val);
    if (!vs) {
        JS_FreeCString(ctx, nm);
        return -1;
    }
    sz = nm_len + strlen(vs) + 2;
    *slot = (char*)malloc(sz);
    if (*slot)
        snprintf(*slot, sz, "%s=%s", nm, vs);
    JS_FreeCString(ctx, nm);
    JS_FreeCString(ctx, vs);
    if (!*slot) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static char** spawn_envv(JSContext* ctx, JSValueConst obj)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i, k = 0;
    char** v;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return NULL;
    v = (char**)calloc((size_t)len + 1, sizeof *v);
    if (!v) {
        JS_FreePropertyEnum(ctx, tab, len);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if (spawn_env_at(ctx, obj, tab[i].atom, &v[k]) < 0) {
            uint32_t j;
            JS_FreePropertyEnum(ctx, tab, len);
            for (j = 0; j < k; j++)
                free(v[j]);
            free(v);
            return NULL;
        }
        k++;
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return v;
}

static int spawn_opt_int(JSContext* ctx, JSValueConst o, const char* key,
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

static int spawn_opt_id(JSContext* ctx, JSValueConst o, const char* key,
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

static void spawn_sigpipe_ignore(void)
{
    static int done;
    struct sigaction sa, old;

    if (done)
        return;
    done = 1;
    if (sigaction(SIGPIPE, NULL, &old) != 0)
        return;
    if (old.sa_handler != SIG_DFL)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

static void spawn_ctor_free_exec(dyn_exec_t* e, JSContext* ctx,
    JSValue input_ref)
{
    dyn_strv_free(e->argv);
    dyn_strv_free(e->envp);
    free(e->cwd);
    free(DYN_UNCONST(e->input));
    e->input = NULL;
    JS_FreeValue(ctx, input_ref);
}

static JSValue dyn_spawn_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_exec_t e;
    JSValue obj = JS_UNDEFINED, input_ref = JS_UNDEFINED;
    JSValue proto;
    dyn_spawn_t* sp = NULL;
    const char *cmd, *path = NULL;
    struct dyn_aio* aio;
    size_t i;
    int64_t timeout_ms = 0, maxpipe = (int64_t)DYN_EXEC_MAXBUF;
    int stdin_pipe = 0, stdin_inherit = 0;
    int64_t uid = -1, gid = -1;
    pid_t pid;

    memset(&e, 0, sizeof e);
    e.in[0] = e.in[1] = e.out[0] = e.out[1] = e.err[0] = e.err[1] = -1;
    e.uid = (uid_t)-1;
    e.gid = (gid_t)-1;

    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx,
            "sys.Spawn: command must be a string");
        goto fail_noobj;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
        && JS_IsArray(ctx, argv[1]) != 1) {
        JS_ThrowTypeError(ctx,
            "sys.Spawn: args must be an array -- there is no "
            "shell, so a command line is not a string here");
        goto fail_noobj;
    }
    cmd = dyn_sys_cstr(ctx, argv[0], "Spawn: command");
    if (!cmd)
        goto fail_noobj;
    e.argv = spawn_strv(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, cmd);
    JS_FreeCString(ctx, cmd);
    if (!e.argv)
        goto fail_noobj;

    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        JSValue v;
        if (!JS_IsObject(argv[2])) {
            JS_ThrowTypeError(ctx,
                "sys.Spawn: options must be an object");
            goto fail;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "cwd");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* s = dyn_sys_cstr(ctx, v, "Spawn: cwd");
            struct stat st;
            JS_FreeValue(ctx, v);
            if (!s)
                goto fail;
            if (stat(s, &st) < 0 || !S_ISDIR(st.st_mode)) {
                JS_ThrowInternalError(ctx,
                    "Spawn: cwd is not a directory: %s", s);
                JS_FreeCString(ctx, s);
                goto fail;
            }
            e.cwd = strdup(s);
            JS_FreeCString(ctx, s);
            if (!e.cwd) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
        } else {
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "env");
        if (JS_IsException(v))
            goto fail;
        if (JS_IsObject(v)) {
            e.envp = spawn_envv(ctx, v);
            if (!e.envp) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
        }
        JS_FreeValue(ctx, v);
        if (spawn_opt_int(ctx, argv[2], "timeoutMs", 0,
                "timeoutMs must not be negative", &timeout_ms)
                < 0
            || spawn_opt_int(ctx, argv[2], "maxPipe", 1,
                   "maxPipe must be positive", &maxpipe)
                < 0
            || spawn_opt_id(ctx, argv[2], "uid", &uid) < 0
            || spawn_opt_id(ctx, argv[2], "gid", &gid) < 0)
            goto fail;
        if (maxpipe > DYN_SPAWN_MAXPIPE_CAP) {
            JS_ThrowRangeError(ctx,
                "Spawn: maxPipe must be 1 to %d",
                DYN_SPAWN_MAXPIPE_CAP);
            goto fail;
        }
        e.uid = (uid_t)uid;
        e.gid = (gid_t)gid;
        v = JS_GetPropertyStr(ctx, argv[2], "stdin");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* sm = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!sm)
                goto fail;
            if (strcmp(sm, "pipe") == 0)
                stdin_pipe = 1;
            else if (strcmp(sm, "inherit") == 0)
                stdin_inherit = 1;
            else if (strcmp(sm, "ignore") != 0) {
                JS_FreeCString(ctx, sm);
                JS_ThrowRangeError(ctx,
                    "Spawn: stdin must be \"pipe\", \"ignore\" or \"inherit\"");
                goto fail;
            }
            JS_FreeCString(ctx, sm);
        } else {
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "allowPathCwd");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v)) {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                goto fail;
            e.no_path_cwd = !b;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    e.timeout_ms = timeout_ms;

    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "input");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const uint8_t* src;
            size_t n = 0;
            int is_str = JS_IsString(v);
            n = 0;
            if (is_str) {
                const char* cs = JS_ToCStringLen(ctx, &n, v);
                src = (const uint8_t*)cs;
            } else {
                src = spawn_view_bytes(ctx, v, &n);
            }
            if (!src) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            stdin_pipe = 1;
            e.input = (const uint8_t*)malloc(n ? n : 1);
            if (!e.input) {
                if (is_str)
                    JS_FreeCString(ctx, (const char*)src);
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            memcpy(DYN_UNCONST(e.input), src, n);
            if (is_str)
                JS_FreeCString(ctx, (const char*)src);
            e.inlen = n;
        }
        JS_FreeValue(ctx, v);
    }

    for (i = 0; e.envp && e.envp[i]; i++)
        if (strncmp(e.envp[i], "PATH=", 5) == 0) {
            path = e.envp[i] + 5;
            break;
        }
    if (!path && !e.envp)
        path = getenv("PATH");
    if (dyn_path_lookup(e.argv[0], path, e.path, sizeof e.path,
            e.no_path_cwd)
        < 0) {
        JS_ThrowInternalError(ctx, "Spawn: command not found: %s",
            e.argv[0]);
        goto fail;
    }
    if ((stdin_pipe && pipe(e.in) < 0) || pipe(e.out) < 0 || pipe(e.err) < 0) {
        JS_ThrowInternalError(ctx, "Spawn: pipe failed (%s)", strerror(errno));
        goto fail;
    }
    if (stdin_pipe) {
        fcntl(e.in[0], F_SETFD, FD_CLOEXEC);
        fcntl(e.in[1], F_SETFD, FD_CLOEXEC);
    }
    fcntl(e.out[0], F_SETFD, FD_CLOEXEC);
    fcntl(e.out[1], F_SETFD, FD_CLOEXEC);
    fcntl(e.err[0], F_SETFD, FD_CLOEXEC);
    fcntl(e.err[1], F_SETFD, FD_CLOEXEC);
    e.maxfd = dyn_exec_fd_limit();

    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) {
        JS_ThrowInternalError(ctx,
            "Spawn: the async reactor is unavailable in this build");
        goto fail;
    }
    spawn_sigpipe_ignore();

    pid = fork();
    if (pid < 0) {
        JS_ThrowInternalError(ctx, "Spawn: fork failed (%s)",
            strerror(errno));
        dyn_net_reactor_release(ctx);
        goto fail;
    }
    if (pid == 0) {
        if (e.in[1] >= 0)
            close(e.in[1]);
        close(e.out[0]);
        close(e.err[0]);
        if (!stdin_pipe && !stdin_inherit) {
            int nul = open("/dev/null", O_RDONLY);
            if (nul < 0)
                _exit(127);
            if (nul != 0) {
                if (dup2(nul, 0) < 0)
                    _exit(127);
                close(nul);
            }
        }
        dyn_exec_child(&e);
        _exit(127);
    }
    if (e.in[0] >= 0) {
        close(e.in[0]);
        e.in[0] = -1;
    }
    close(e.out[1]);
    e.out[1] = -1;
    close(e.err[1]);
    e.err[1] = -1;
    fcntl(e.out[0], F_SETFL, O_NONBLOCK);
    fcntl(e.err[0], F_SETFL, O_NONBLOCK);
    if (stdin_pipe)
        fcntl(e.in[1], F_SETFL, O_NONBLOCK);

    sp = (dyn_spawn_t*)calloc(1, sizeof *sp);
    if (!sp) {
        JS_ThrowOutOfMemory(ctx);
        dyn_net_reactor_release(ctx);
        goto fail_parent;
    }
    sp->refs = 1;
    sp->rt = JS_GetRuntime(ctx);
    sp->ctx = ctx;
    sp->pid = pid;
    sp->aio = aio;
    sp->maxpipe = (size_t)maxpipe;
    sp->out.sp = sp;
    sp->err.sp = sp;
    sp->in.sp = sp;
    sp->wait_resolve = sp->wait_reject = JS_UNDEFINED;
    sp->out.rbuf = sp->out.rresolve = sp->out.rreject = JS_UNDEFINED;
    sp->err.rbuf = sp->err.rresolve = sp->err.rreject = JS_UNDEFINED;
    sp->out.promise = sp->err.promise = JS_UNDEFINED;
    sp->in.flush_resolve = sp->in.flush_reject = JS_UNDEFINED;
    sp->out.fd = e.out[0];
    sp->err.fd = e.err[0];
    sp->in.fd = stdin_pipe ? e.in[1] : -1;
    sp->in.piped = stdin_pipe;
    if (timeout_ms > 0) {
        int64_t now0 = dyn_now_ms();
        sp->deadline = timeout_ms > INT64_MAX - now0 ? INT64_MAX
                                                     : now0 + timeout_ms;
    }

    proto = dyn_ctor_proto(ctx, new_target, dyn_spawn_class_id);
    if (JS_IsException(proto))
        goto fail_spawn;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_spawn_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        obj = JS_UNDEFINED;
        goto fail_spawn;
    }
    JS_SetOpaque(obj, sp);

    sp->out.watch = sp->err.watch = 1;
    if (dyn_evloop_add(dyn_aio_evloop(aio), sp->out.fd, DYN_EV_READ,
            spawn_pipe_cb, &sp->out)
            < 0
        || dyn_evloop_add(dyn_aio_evloop(aio), sp->err.fd, DYN_EV_READ,
               spawn_pipe_cb, &sp->err)
            < 0
        || (sp->in.fd >= 0
            && dyn_evloop_add(dyn_aio_evloop(aio), sp->in.fd, 0,
                   spawn_stdin_cb, &sp->in)
                < 0)) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not watch the child's pipes (%s)", strerror(errno));
        goto fail_spawn;
    }
    if (dyn_net_on_drain(spawn_reap_hook, sp) < 0) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not register the reaper; wait() may never settle");
        goto fail_spawn;
    }
    sp->hooked = 1;

    if (e.input) {
        if (spawn_stdin_enqueue(sp, e.input, e.inlen) < 0)
            goto fail_spawn;
        sp->in.close_after_drain = 1;
    }

    spawn_ctor_free_exec(&e, ctx, input_ref);
    return obj;

fail_spawn:
    if (!JS_IsUndefined(obj)) {
        JS_SetOpaque(obj, NULL);
        JS_FreeValue(ctx, obj);
        obj = JS_UNDEFINED;
    }
    if (sp && !sp->dead)
        spawn_release(sp);
    sp = NULL;
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail_parent:
    {
        int status;
        pid_t r;
        dyn_kill_group(pid, SIGKILL);
        do {
            r = waitpid(pid, &status, 0);
        } while (r < 0 && errno == EINTR);
    }
    {
        int k;
        for (k = 0; k < 2; k++) {
            if (e.in[k] >= 0)
                close(e.in[k]);
            if (e.out[k] >= 0)
                close(e.out[k]);
            if (e.err[k] >= 0)
                close(e.err[k]);
        }
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail:
    for (i = 0; i < 2; i++) {
        if (e.in[i] >= 0)
            close(e.in[i]);
        if (e.out[i] >= 0)
            close(e.out[i]);
        if (e.err[i] >= 0)
            close(e.err[i]);
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail_noobj:
    return JS_EXCEPTION;
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
    JS_CFUNC_DEF("rusage", 0, dyn_sys_rusage),
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
