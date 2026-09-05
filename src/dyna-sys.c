/*
 * dyna:sys -- process and environment interface. Self-contained, in-repo (no
 * external deps), synchronous, portable across macOS and Linux.
 *
 *   import * as sys from "dyna:sys";
 *
 * Scope: this module owns ONLY the process/environment surface. The filesystem
 * surface (metadata, directories, links, globbing, temp files) moved to
 * dyna:file, which now owns all filesystem operations alongside buffered
 * content I/O; path-STRING logic (join/normalize/dirname/...) stays in
 * dyna:path.
 *
 * Surface:
 *   Process  : env(), getEnv(name), setEnv(name, val), args(), cwd(),
 *              chDir(path), platform(), pid(), hostName(), homeDir().
 *
 * Coercion discipline (CLAUDE.md sec.5): every method coerces ALL of its JS
 * arguments into owned C locals (JS_ToCString / JS_ToInt32 / JS_ToBool) FIRST,
 * then performs the syscall. These are transient plain functions -- no `this`,
 * no long-lived native handle -- so there is no resource for a reentrant
 * valueOf/toString to corrupt; the discipline here is simply that every
 * JS_ToCString result is released on every path, including every error path.
 */
#include "dyna-nat.h"
#include "dyna-simd-kernels.h"   /* cpu_features: report what the dispatcher picked */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define dyn_environ (*_NSGetEnviron())
#else
extern char **environ;
#define dyn_environ environ
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


/* ==================================================================== *
 *  errno -> thrown JS Error (descriptive message + .code + .errno)      *
 * ==================================================================== */

static const char *dyn_sys_errno_code(int e)
{
    switch (e) {
    case ENOENT:        return "ENOENT";
    case EACCES:        return "EACCES";
    case EEXIST:        return "EEXIST";
    case ENOTDIR:       return "ENOTDIR";
    case EISDIR:        return "EISDIR";
    case ENOTEMPTY:     return "ENOTEMPTY";
    case EPERM:         return "EPERM";
    case ELOOP:         return "ELOOP";
    case ENAMETOOLONG:  return "ENAMETOOLONG";
    case EXDEV:         return "EXDEV";
    case EINVAL:        return "EINVAL";
    case ENOSPC:        return "ENOSPC";
    case EROFS:         return "EROFS";
    case EBUSY:         return "EBUSY";
    case EMFILE:        return "EMFILE";
    case ENFILE:        return "ENFILE";
    case ENOMEM:        return "ENOMEM";
    default:            return NULL;
    }
}

/* Build and throw a descriptive Error for a failed syscall. Returns
 * JS_EXCEPTION. `path` may be NULL. Reads errno via the `e` argument (captured
 * by the caller immediately after the failing call). */
static JSValue dyn_sys_throw(JSContext *ctx, int e, const char *op,
                             const char *path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char *code = dyn_sys_errno_code(e);

    if (path)
        snprintf(msg, sizeof(msg), "sys.%s(\"%s\"): %s", op, path, strerror(e));
    else
        snprintf(msg, sizeof(msg), "sys.%s: %s", op, strerror(e));

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


/* ==================================================================== *
 *  process / environment                                                *
 * ==================================================================== */

static JSValue dyn_sys_env(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    char **envp = dyn_environ;
    JSValue obj;
    uint32_t idx;
    (void)this_val; (void)argc; (void)argv;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (idx = 0; envp[idx] != NULL; idx++) {
        const char *entry = envp[idx];
        const char *eq = strchr(entry, '=');
        JSAtom atom;
        if (!eq)
            continue;
        atom = JS_NewAtomLen(ctx, entry, (size_t)(eq - entry));
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValue(ctx, obj, atom, JS_NewString(ctx, eq + 1),
                               JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    return obj;
}

static JSValue dyn_sys_get_env(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *name, *val;
    JSValue out;
    (void)this_val; (void)argc;

    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = getenv(name);
    out = val ? JS_NewString(ctx, val) : JS_UNDEFINED;
    JS_FreeCString(ctx, name);
    return out;
}

static JSValue dyn_sys_set_env(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *name, *val;
    size_t name_len, val_len;
    int r;
    (void)this_val; (void)argc;

    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = JS_ToCStringLen(ctx, &val_len, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    /* setenv() stops at the first NUL, so "PATH\0X" silently sets PATH: a
       caller that validated the name against a denylist is bypassed by
       appending a NUL. strlen < the string's own length IS an embedded NUL.
       Refuse rather than truncate; '=' cannot appear in a name either. */
    if (strlen(name) != name_len || strlen(val) != val_len ||
        strchr(name, '=') != NULL || name_len == 0) {
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

/* args() -> the process argument vector. macOS reads it via crt_externs;
 * Linux reads /proc/self/cmdline; elsewhere returns an empty array. */
static JSValue dyn_sys_args(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    JSValue arr;
    (void)this_val; (void)argc; (void)argv;

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;

#if defined(__APPLE__)
    {
        int ac = *_NSGetArgc();
        char **av = *_NSGetArgv();
        int i;
        for (i = 0; i < ac && av && av[i]; i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewString(ctx, av[i]));
    }
#elif defined(__linux__)
    {
        int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char *buf = NULL;
            size_t len = 0, cap = 0;
            for (;;) {
                ssize_t got;
                if (len + 4096 > cap) {
                    size_t nc = cap ? cap * 2 : 8192;
                    char *nb = (char *)realloc(buf, nc);
                    if (!nb) { free(buf); buf = NULL; len = 0; break; }
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
                    JS_SetPropertyUint32(ctx, arr, idx++,
                                         JS_NewStringLen(ctx, buf + start,
                                                         i - start));
                    i++; /* skip the NUL separator */
                }
                free(buf);
            }
        }
    }
#endif
    return arr;
}

static JSValue dyn_sys_cwd(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    char stackbuf[PATH_MAX];
    char *cwd;
    JSValue out;
    (void)this_val; (void)argc; (void)argv;

    cwd = getcwd(stackbuf, sizeof(stackbuf));
    if (cwd)
        return JS_NewString(ctx, cwd);
    if (errno != ERANGE)
        return dyn_sys_throw(ctx, errno, "cwd", NULL);
    /* path longer than PATH_MAX: grow until it fits */
    {
        size_t cap = sizeof(stackbuf);
        for (;;) {
            char *buf;
            cap *= 2;
            if (cap > (1u << 20))
                return dyn_sys_throw(ctx, ENAMETOOLONG, "cwd", NULL);
            buf = (char *)malloc(cap);
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

static JSValue dyn_sys_chdir(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    const char *path;
    (void)this_val; (void)argc;

    path = JS_ToCString(ctx, argv[0]);
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

static JSValue dyn_sys_platform(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
#if defined(__APPLE__)
    return JS_NewString(ctx, "darwin");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue dyn_sys_pid(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int32_t)getpid());
}

static JSValue dyn_sys_host_name(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    char buf[256];
    (void)this_val; (void)argc; (void)argv;
    if (gethostname(buf, sizeof(buf)) != 0)
        return dyn_sys_throw(ctx, errno, "hostName", NULL);
    buf[sizeof(buf) - 1] = '\0'; /* gethostname may not NUL-terminate on trunc */
    return JS_NewString(ctx, buf);
}

static JSValue dyn_sys_home_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *h;
    struct passwd *pw;
    (void)this_val; (void)argc; (void)argv;

    h = getenv("HOME");
    if (h && *h)
        return JS_NewString(ctx, h);
    pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return JS_NewString(ctx, pw->pw_dir);
    return dyn_sys_throw(ctx, ENOENT, "homeDir", NULL);
}

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */


/* memoryUsage() -> the engine's own accounting plus the OS's peak RSS.
 *
 * This exists because "does this API change cost anything?" cannot be answered
 * with a stopwatch alone. A restructuring that keeps the same CPU time while
 * allocating an extra object per call has not been free -- it has moved the
 * cost to the collector, where a wall-clock microbenchmark will not see it.
 * The cost gate therefore reports bytes and allocations per
 * operation next to nanoseconds, and this is where those two numbers come from.
 *
 *   mallocCount / mallocSize   LIVE allocations and bytes (net, not churn):
 *                              the delta across N operations is what those
 *                              operations RETAINED. Non-zero after a gc() means
 *                              growth, which is the leak signal.
 *   objCount / objSize         live JS objects -- the number a capability
 *                              moves when it replaces a call with an instance.
 *   peakRss                    the OS's high-water mark, which is the only
 *                              number that reflects transient churn; the engine
 *                              counters cannot see memory that was freed.
 *   nativeSize                 module-native bytes: libc memory the dyna:*
 *                              modules hold OUTSIDE the JS heap, where the
 *                              engine counters cannot follow (audit E0208-01).
 *                              Counts the DynResource box every native
 *                              resource carries (which also stays visible to
 *                              the engine's mallocSize -- the ~40-byte double
 *                              entry is deliberate) plus allocations made
 *                              through the counted dyn_nat_* allocator
 *                              (dyna:structures). Payload buffers in modules
 *                              not yet converted (dyna:dataframe, dyna:ml,
 *                              dyna:http, ...) are NOT in this number.
 *   nativeLimit                the cap setNativeMemoryLimit() installed;
 *                              0 = uncapped.
 *
 * Sizes are the allocator's usable size plus its per-block overhead, so they
 * match what the process actually took, not what was asked for. */
static JSValue dyn_sys_memory_usage(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSMemoryUsage u;
    struct rusage ru;
    JSValue o;
    (void)this_val; (void)argc; (void)argv;

    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &u);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SET(name, v) \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewInt64(ctx, (int64_t)(v)), \
                                  JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
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
        /* Linux reports kilobytes, the BSDs and macOS report bytes. Normalise
         * to bytes so a number printed on one platform means the same on the
         * other -- getting this wrong makes a 1024x difference look like a
         * regression. */
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

/* setNativeMemoryLimit(bytes): JS_SetMemoryLimit for module-native memory.
 *
 * The engine limit cannot see libc memory a module allocates outside the JS
 * heap, so an embedder who wants one ceiling on the process had no lever on
 * this half. `bytes` caps the module-native ledger; an allocation past it is
 * REFUSED, not served -- the C allocator returns NULL and the call site
 * surfaces JS_ThrowOutOfMemory, so a script sees an exception instead of the
 * process growing without bound.
 *
 * The default is 0 -- UNLIMITED, deliberately. Native allocations were never
 * limited before, and flipping a second, independent ceiling on by default
 * would start failing scripts that never asked for it (an engine limit is not
 * a native limit: it is a different ledger). Embedders who want the cap set
 * it explicitly at startup, before running untrusted code.
 *
 * Setting a limit never frees anything; it only gates new allocations, so a
 * live module over the new cap keeps working until it allocates. */
static JSValue dyn_sys_set_native_memory_limit(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    int64_t limit;
    (void)this_val; (void)argc;

    if (JS_ToInt64(ctx, &limit, argv[0]))
        return JS_EXCEPTION;
    if (limit < 0)
        return JS_ThrowRangeError(ctx, "memory limit must be >= 0");
    dyn_nat_set_limit((uint64_t)limit);
    return JS_UNDEFINED;
}

/* CPU, memory, load, uptime and disk. */
#include "dyna-machine.inc.c"

/* Subprocesses: argv only, no shell. */
#include "dyna-proc.inc.c"

static const JSCFunctionListEntry dyn_sys_funcs[] = {
    /* process / environment */
    JS_CFUNC_DEF("env", 0, dyn_sys_env),
    JS_CFUNC_DEF("getEnv", 1, dyn_sys_get_env),
    JS_CFUNC_DEF("setEnv", 2, dyn_sys_set_env),
    JS_CFUNC_DEF("args", 0, dyn_sys_args),
    JS_CFUNC_DEF("cwd", 0, dyn_sys_cwd),
    JS_CFUNC_DEF("chDir", 1, dyn_sys_chdir),
    JS_CFUNC_DEF("platform", 0, dyn_sys_platform),
    JS_CFUNC_DEF("pid", 0, dyn_sys_pid),
    JS_CFUNC_DEF("hostName", 0, dyn_sys_host_name),

    /* machine facts */
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

static int dyn_sys_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

int js_nat_init_sys(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:sys", dyn_sys_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SYS */
