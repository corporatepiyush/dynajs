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
 *   Identity : getuid(), getgid(), setUid(uid), setGid(gid).
 *   Machine: uname, arch, cpuUsage, rusage.
 *   Children: Exec (sync, buffered) and Spawn (async, streaming
 *              stdio, wait()/kill()) -- argv only, no shell in either.
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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
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


/* The C-string boundary. JS strings carry NUL bytes; every syscall below
   takes a C string and would act on a silent truncation of what the caller
   actually passed -- the same truncation setEnv refuses (see its comment).
   The dangerous direction is a JS-level validator that approved the FULL
   string ("profile.txt\0.sh" ends in ".sh"; "/bin/echo\0-evil" starts with a
   known-good path) while the syscall acts on the PREFIX. Refuse, per the
   setEnv precedent. Returns the owned C string, or NULL having thrown. */
static const char *dyn_sys_cstr(JSContext *ctx, JSValueConst v,
                                const char *what)
{
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v);

    if (!s)
        return NULL;
    if (strlen(s) != len) {
        JS_ThrowTypeError(ctx, "%s: string contains a NUL byte", what);
        JS_FreeCString(ctx, s);
        return NULL;
    }
    return s;
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

    /* No environment name can contain a NUL, so a NUL-bearing lookup would
       otherwise silently act on the truncated prefix (getEnv("PATH\0x")
       reading PATH). Refused, like setEnv below. */
    name = dyn_sys_cstr(ctx, argv[0], "getEnv: name");
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

    /* chdir() on the truncated prefix of a NUL-bearing path is a classic
       validation bypass (a JS-level check sees the full string, the syscall
       moves to the prefix), so refused rather than truncated. */
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

/* Map a uname(2) sysname to a platform id. Everything uname reports
 * is accepted, so the BSDs (and anything else) get a real name instead of the
 * old compile-time "unknown": Darwin->darwin, Linux->linux, FreeBSD/
 * OpenBSD/NetBSD/DragonFly/SunOS->lowercase sysname. The mapping is
 * case-insensitive (macOS reports "Darwin", FreeBSD reports "FreeBSD"). */
static const char *dyn_sys_platform_from_sysname(const char *sysname)
{
    static const char *const known[] = {
        "darwin", "linux", "freebsd", "openbsd", "netbsd",
        "dragonfly", "sunos",
    };
    size_t i;
    for (i = 0; i < countof(known); i++) {
        const char *k = known[i];
        size_t j;
        for (j = 0; k[j]; j++)
            if (tolower((unsigned char)sysname[j]) != k[j])
                break;
        if (k[j] == '\0')
            return k;
    }
    return NULL;
}

static JSValue dyn_sys_platform(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    struct utsname un;
    const char *p;
    (void)this_val; (void)argc; (void)argv;

    /* uname() answers at runtime, which covers the BSDs a single binary could
     * in principle run on; the compile-time macros remain only as the
     * fallback when uname() itself fails (which is effectively never). The
     * darwin/linux strings are unchanged from the previous behavior. The
     * call and the lookup are split: each check stands on its own line. */
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

/* arch: the machine hardware name from uname(2), with the two
 * spellings people use interchangeably normalised (aarch64->arm64,
 * amd64->x86_64). Any other machine string (i386/i686/riscv64/...) passes
 * through verbatim, lowercased. "unknown" only when uname() failed. */
static JSValue dyn_sys_arch(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    struct utsname un;
    const char *m;
    char buf[sizeof(un.machine)];
    size_t i;
    (void)this_val; (void)argc; (void)argv;

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

/* uname: the five standard utsname fields as a plain object. All
 * five are strings; nodename matches hostName(). */
static JSValue dyn_sys_uname(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    struct utsname un;
    JSValue o;
    (void)this_val; (void)argc; (void)argv;

    if (uname(&un) != 0)
        return dyn_sys_throw(ctx, errno, "uname", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETSTR(name, v) \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewString(ctx, v), \
                                  JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
    SETSTR("sysname", un.sysname)
    SETSTR("nodename", un.nodename)
    SETSTR("release", un.release)
    SETSTR("version", un.version)
    SETSTR("machine", un.machine)
#undef SETSTR
    return o;
}

/* cpuUsage: {user, system} in seconds for THIS process, from
 * getrusage(RUSAGE_SELF) -- the same clocks getrusage() exposes, so the two
 * agree by construction. Monotonic non-decreasing over the process lifetime. */
static JSValue dyn_sys_cpu_usage(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    struct rusage ru;
    JSValue o;
    (void)this_val; (void)argc; (void)argv;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return dyn_sys_throw(ctx, errno, "cpuUsage", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETSEC(name, tv) \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewFloat64(ctx, \
            (double)(tv).tv_sec + (double)(tv).tv_usec / 1e6), \
            JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
    SETSEC("user", ru.ru_utime)
    SETSEC("system", ru.ru_stime)
#undef SETSEC
    return o;
}

/* rusage: the full struct rusage (RUSAGE_SELF) as a flat object.
 * Documented subset -- the fields every supported platform fills:
 *   user/system    seconds spent in user/kernel mode (cpuUsage() returns
 *                  exactly these two)
 *   maxrss         peak resident set size, normalised to BYTES like
 *                  memoryUsage().peakRss (Linux reports kB, BSDs/macOS bytes)
 *   idrss/isrss    (historical) integral shared/unshared memory; Linux
 *                  reports 0
 *   minflt/majflt  page faults not requiring / requiring I/O
 *   nswap          times swapped out (0 on Linux and modern macOS)
 *   inblock/oublock block I/O reads/writes
 *   msgsnd/msgrcv  IPC messages sent/received
 *   nsigs          signals delivered
 *   nvcsw/nivcsw   voluntary / involuntary context switches
 * Counter semantics beyond user/system/maxrss are kernel-specific; portable
 * code should treat them as advisory. */
static JSValue dyn_sys_rusage(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    struct rusage ru;
    JSValue o;
    (void)this_val; (void)argc; (void)argv;

    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return dyn_sys_throw(ctx, errno, "rusage", NULL);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define SETI64(name, v) \
    if (JS_DefinePropertyValueStr(ctx, o, name, \
                                  JS_NewInt64(ctx, (int64_t)(v)), \
                                  JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
/* user/system are SECONDS (fractional, like cpuUsage()) -- not truncated. */
#define SETSEC(name, tv) \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewFloat64(ctx, \
            (double)(tv).tv_sec + (double)(tv).tv_usec / 1e6), \
            JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
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

/* getuid/getgid: the real user/group id. setUid/setGid wire the setuid(2)/
 * setgid(2) syscalls: WITH privilege (root) they take effect; without it the
 * kernel refuses with EPERM, which surfaces as a clean Error
 * {code: "EPERM", errno: EPERM} -- a script never sees a raw -1. */
static JSValue dyn_sys_getuid(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int32_t)getuid());
}

static JSValue dyn_sys_getgid(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int32_t)getgid());
}

/* Shared argument discipline for setUid/setGid: a non-negative integer id,
 * refused as TypeError (non-number) / RangeError (negative) BEFORE the
 * syscall, so the only syscall-level failure a script can see is the
 * privilege one (EPERM), with its clean Error. */
static JSValue dyn_sys_set_id(JSContext *ctx, JSValueConst *argv,
                              const char *what, int32_t *out)
{
    int32_t id;
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "%s: id must be a number", what);
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    /* strict integer: a truncated 1.5 would setuid(1) -- an id the caller
       never asked for */
    if (d != (double)(int64_t)d)
        return JS_ThrowTypeError(ctx, "%s: id must be an integer", what);
    id = (int32_t)d;
    if (id < 0)
        return JS_ThrowRangeError(ctx, "%s: id must be >= 0", what);
    *out = id;
    return JS_UNDEFINED;
}

static JSValue dyn_sys_setuid(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int32_t id;
    (void)this_val; (void)argc;
    {
        JSValue r = dyn_sys_set_id(ctx, argv, "setUid", &id);
        if (JS_IsException(r))
            return r;
    }
    if (setuid((uid_t)id) != 0)
        return dyn_sys_throw(ctx, errno, "setUid", NULL);
    return JS_UNDEFINED;
}

static JSValue dyn_sys_setgid(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int32_t id;
    (void)this_val; (void)argc;
    {
        JSValue r = dyn_sys_set_id(ctx, argv, "setGid", &id);
        if (JS_IsException(r))
            return r;
    }
    if (setgid((gid_t)id) != 0)
        return dyn_sys_throw(ctx, errno, "setGid", NULL);
    return JS_UNDEFINED;
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
    double d;
    (void)this_val; (void)argc;

    /* 0 means UNLIMITED here, so a coercion that lands a non-number or a
       NaN/Infinity on 0 would silently DISARM a cap an embedder asked for --
       failing open on the one call that exists to refuse memory. Non-numbers
       and non-finite numbers are refused outright; a negative number still
       gets its own error below. */
    if (!JS_IsNumber(argv[0]) || JS_ToFloat64(ctx, &d, argv[0]) != 0
        || !isfinite(d))
        return JS_ThrowTypeError(ctx,
            "setNativeMemoryLimit: bytes must be a finite number");
    if (JS_ToInt64(ctx, &limit, argv[0]))
        return JS_EXCEPTION;
    if (limit < 0)
        return JS_ThrowRangeError(ctx,
            "setNativeMemoryLimit: memory limit must be >= 0");
    dyn_nat_set_limit((uint64_t)limit);
    return JS_UNDEFINED;
}

/* CPU, memory, load, uptime and disk. */
#include "dyna-machine.inc.c"

/* Subprocesses: argv only, no shell. */
#include "dyna-proc.inc.c"
/* ==================================================================== *
 *  Spawn: the ASYNC child process, alongside the sync Exec *
 * ==================================================================== *
 *
 *   const p = new sys.Spawn("cat", [], { input: "hi" });
 *   const n = await p.stdout.read(buf);      // dyna:stream ByteSource shape
 *   const r = await p.wait();                // {code, signal, timedOut}
 *
 * Design (each decision made against the engine's existing machinery):
 *
 * ASYNC MODEL. Exec pumps the child with poll() and BLOCKS the JS thread for
 * the child's whole life. Spawn must not: its pipes are watched on the shared
 * per-thread reactor (dyn_net_reactor_acquire -- the same one dyna:file's
 * async ops ride; dyna-net.o is linked into every build), so pipe readiness
 * wakes js_std_loop, which pumps the microtask queue between events. Exit is
 * reaped from a drain hook (dyn_net_on_drain arms the reactor's periodic
 * tick) plus an immediate waitpid(WNOHANG) at every pipe EOF, so a wait()
 * settles within about one tick and no Spawn ever leaves a zombie. The
 * reactor ref rides the CHILD'S LIFETIME (plus open pipes), not the JS
 * objects': while a child runs, the held ref keeps js_os_poll's "no more
 * events" exit closed, so a script that spawns and never awaits still runs
 * the child to completion before the process exits; once the child is reaped
 * and the pipes are closed the ref drops, so a script that HOLDS its Spawn
 * objects can still exit.
 *
 * NO SHELL, NO NUL. The child is built with the same discipline as Exec:
 * argv-only, execve(2) after the PARENT resolves PATH, and every string
 * (command, args, env, cwd) refused when it carries a NUL -- a syscall acting
 * on the truncated prefix of a validated string is the injection-shaped bug
 * the module's doctrine refuses (dyn_sys_cstr). The child half is
 * dyn_exec_child, reused verbatim: setsid, dup2, close 3..maxfd, gid-then-uid
 * drop, execve, _exit -- async-signal-safe only, SIGPIPE restored to SIG_DFL.
 *
 * BACKPRESSURE. read(buf) is the dyna:stream pull shape (fills UP TO
 * buf.length, resolves the count, 0 = EOF), but stdout and stderr are drained
 * CONTINUOUSLY into internal buffers -- a pull-only pipe deadlocks the moment
 * a child writes more to the pipe nobody is reading (the deadlock Exec's pump
 * exists to avoid). Each buffer is bounded by maxPipe (default 8 MiB, the
 * Exec maxBuffer default), and the bound is enforced the way a pipe ought to
 * be: when a buffer is full the READ watch comes OFF, the kernel pipe fills,
 * and the child BLOCKS on write until a read() drains room. True kernel
 * backpressure -- a slow reader slows the child; nobody is killed and
 * nothing grows without bound.
 *
 * LIFETIME. The native state is refcounted: the Spawn object and each of the
 * three stdio views (stdout/stderr/stdin) hold one reference, so a view pulled
 * into a local outlives a dropped Spawn. Each holder's finalizer releases; the
 * last release SIGKILLs a still-running child (group), reaps it, tears down
 * the watches and drops the reactor ref. close()/dispose() does that
 * deterministically. Every JSValue the native state pins (promise resolvers,
 * a pending read's buffer view) is marked from the class gc_mark -- the
 * dyna:file File pattern.
 *
 * SIGPIPE. Writing a pipe whose reader is gone raises SIGPIPE, whose default
 * disposition kills the process. Exec ignores it transiently around its pump;
 * Spawn's writes are asynchronous and unbounded in time, so the FIRST Spawn
 * sets SIG_IGN process-wide and never restores it -- exactly Node's policy.
 * The child restores SIG_DFL before exec (dyn_exec_child), so children still
 * die from SIGPIPE the way a shell pipeline expects. */

/* dyna-net.c is linked into every build (NAT_MODULE_OBJS / SLIMCOMPANION) but
 * its reactor accessors are declared in dyna-nat.h only under
 * CONFIG_NATIVE_MODULE_NET. Re-declared here, the established local
 * declaration precedent (dyna-net-proxy.c): the symbols are in every link
 * that contains dyna-sys.o. */
struct dyn_aio;
struct dyn_aio *dyn_net_reactor_acquire(JSContext *ctx);
void dyn_net_reactor_release(JSContext *ctx);
void dyn_net_reactor_release_rt(JSRuntime *rt);
int dyn_net_on_drain(void (*fn)(void *), void *udata);
void dyn_net_off_drain(void *udata);

#include <strings.h>                /* strcasecmp / strncasecmp (kill names) */

#include "dyna-aio.h"
#include "dyna-evloop.h"

typedef struct dyn_spawn dyn_spawn_t;

/* One output side (stdout or stderr). sp->out / sp->err are these. */
typedef struct {
    dyn_spawn_t *sp;
    int fd;                     /* -1 once closed (EOF, error, or dispose) */
    int eof;                    /* read() has delivered EOF (0) */
    int err;                    /* sticky: 1 = OOM draining */
    int watch;                  /* READ interest currently armed */
    uint8_t *buf;               /* drained-ahead bytes */
    size_t len, cap;
    /* the ONE pending read (a second concurrent read() is refused) */
    int pending;
    JSValue rbuf;               /* the caller's view, pinned (gc_mark) */
    uint8_t *rbase;
    size_t rlen;
    JSValue rresolve, rreject;  /* the pending promise's settle pair */
    JSValue promise;            /* the pending promise, C-pinned + MARKED:
                                   its reaction list is what keeps the
                                   awaiting frame's state traced */
} dyn_spawn_pipe_t;

/* stdin: a ByteSink-shaped write side. write() accepts into a queue and
 * flushes on WRITE readiness; close() sends EOF after the queue drains. */
typedef struct {
    dyn_spawn_t *sp;
    int fd;                     /* -1 once closed (or never piped) */
    int piped;                  /* 1 = opts.stdin "pipe" (or input given) */
    int close_after_drain;
    int err;                    /* sticky: 1 = a write failed (EPIPE et al) */
    int err_errno;
    JSValue flush_resolve, flush_reject;   /* the ONE pending flush */
} dyn_spawn_stdin_t;

struct dyn_spawn {
    int refs;                   /* the object (1) + each live stdio view
                                   (+1) + each operation parked in C (+1):
                                   the struct, its fds and its reactor ref
                                   outlive every JS handle dropped
                                   mid-operation (see spawn_unpark) */
    int torn_down;              /* the native teardown has run (kill, reap,
                                   fds, hook, reactor) -- idempotent */
    int user_closed;            /* close() was called: every further operation
                                   on the object or its views throws */
    JSRuntime *rt;
    JSContext *ctx;
    pid_t pid;
    int exited, code, sig, timed_out, reaped;
    int wait_settled;
    JSValue wait_resolve, wait_reject;     /* wait()'s settle pair */
    dyn_spawn_pipe_t out, err;
    dyn_spawn_stdin_t in;
    /* stdin write queue: bytes copied out of the caller's views */
    uint8_t *wq;
    size_t wlen, wcap, woff;
    size_t maxpipe;             /* per-pipe drain-ahead cap */
    struct dyn_aio *aio;        /* the shared reactor, ref'd */
    int hooked;                 /* reap hook registered */
    int64_t deadline, kill_at;  /* timeoutMs escalation (Exec's schedule) */
    int term_sent;
    int dead;                   /* struct freed (post-release sanity) */
};

static JSClassID dyn_spawn_class_id, dyn_spawn_pipe_class_id,
                 dyn_spawn_stdin_class_id;

static void spawn_release(dyn_spawn_t *sp);
static void spawn_unpark(dyn_spawn_t *sp);

/* ---- promise plumbing --------------------------------------------------- */

/* A fresh promise; hands the settle pair to the caller (owned refs). */
static JSValue spawn_promise_new(JSContext *ctx, JSValue *resolve,
                                 JSValue *reject)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    *resolve = funcs[0];
    *reject = funcs[1];
    return promise;
}

/* Settle a stored pair with `val` (consumed either way) and release the
 * pair. A resolve call cannot fail except for OOM; any exception is
 * swallowed -- the settle is fire-and-forget onto the promise's reaction
 * queue, and surfacing it here would turn a success into a random throw. */
static void spawn_settle(JSContext *ctx, JSValue *resolve, JSValue *reject,
                         JSValue val, int failed)
{
    JSValue f = failed ? *reject : *resolve;
    JSValue r;

    if (!JS_IsUndefined(f)) {
        r = JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst *)&val);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, *resolve);
    JS_FreeValue(ctx, *reject);
    *resolve = *reject = JS_UNDEFINED;
    JS_FreeValue(ctx, val);
}

/* A promise already settled with `val` (consumed): the inline-arm shape
 * dyna:stream uses for work that completed before the promise was handed
 * back. */
static JSValue spawn_promise_resolved(JSContext *ctx, JSValue val)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve,
                                                         &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, val, 0);
    return promise;
}

static JSValue spawn_promise_rejected(JSContext *ctx, JSValue exc)
{
    JSValue resolve, reject, promise = spawn_promise_new(ctx, &resolve,
                                                         &reject);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    spawn_settle(ctx, &resolve, &reject, exc, 1);
    return promise;
}

/* Build (and set as pending) the sticky drain OOM error, then hand it back
 * as a VALUE for a promise rejection. (An overflowing pipe never gets here:
 * full buffers turn into KERNEL backpressure -- see spawn_pipe_cb.) */
static JSValue spawn_cap_error(JSContext *ctx, dyn_spawn_t *sp, int kind)
{
    (void)sp;
    if (kind == 2)
        JS_ThrowOutOfMemory(ctx);
    else
        JS_ThrowInternalError(ctx, "Spawn: pipe drain failed");
    return JS_GetException(ctx);
}

/* ---- gc: everything the native state pins -------------------------------- */

/* No gc_mark on the Spawn class or the stdio views. The struct pins no
 * caller handler -- only in-flight operation state -- and each pinned value
 * is a counted C reference; declaring a mark for one hands that count to the
 * collector as the object's own, so dropping the object frees state a parked
 * read still owns (a free_object use-after-free at 32 concurrent reads). */
/* ---- teardown ------------------------------------------------------------ */

/* Unwatch (an interest-0 del is a no-op) then close. */
static void spawn_close_fd(dyn_spawn_t *sp, int *fd)
{
    if (*fd >= 0) {
        if (sp->aio)
            dyn_evloop_del(dyn_aio_evloop(sp->aio), *fd);
        close(*fd);
        *fd = -1;
    }
}

/* Kill the child GROUP (and the pid, in case setsid failed) -- the Exec
 * discipline: a grandchild holding a pipe must die with the child. */
static void spawn_kill_child(dyn_spawn_t *sp, int sig)
{
    if (!sp->exited && sp->pid > 0)
        dyn_kill_group(sp->pid, sig);
}

/* READ-interest management on an output pipe: BACKPRESSURE is "watch off"
 * -- the kernel pipe fills, the child blocks on write, and nobody's memory
 * grows. Level-triggered readiness means re-arming is always safe. */
static void spawn_pipe_watch(dyn_spawn_pipe_t *p, int on)
{
    dyn_spawn_t *sp = p->sp;

    if (p->fd < 0 || p->eof || !sp->aio)
        return;
    if (on == p->watch)
        return;
    if (dyn_evloop_mod(dyn_aio_evloop(sp->aio), p->fd,
                       on ? DYN_EV_READ : 0) == 0)
        p->watch = on;
}

/* Drop the reactor ref as soon as it serves nothing: the child is reaped
 * AND every pipe is closed. Buffered bytes still answer read()s inline
 * (they need no loop), and js_os_poll's "no more events" exit reopens --
 * a fully reaped Spawn must not keep the process alive at script end. The
 * REGISTRY reference is NOT dropped here: it keeps the struct (and the
 * reaped child's recorded result) until teardown. */
static void spawn_aio_release_if_idle(dyn_spawn_t *sp)
{
    if (sp->aio) {
        if (!sp->exited || sp->out.fd >= 0 || sp->err.fd >= 0
            || sp->in.fd >= 0)
            return;
        dyn_net_reactor_release_rt(sp->rt);
        sp->aio = NULL;
    }
}

/* wait()'s result object, Exec's convention: `code` is null when a signal
 * killed the child. Returns JS_EXCEPTION having thrown on OOM. */
static JSValue spawn_wait_result(dyn_spawn_t *sp)
{
    JSContext *ctx = sp->ctx;
    JSValue o, v;
    const char *signame = sp->sig ? dyn_signal_name(sp->sig) : NULL;

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    v = signame ? JS_NULL : JS_NewInt32(ctx, sp->code);
    if (JS_SetPropertyStr(ctx, o, "code", v) < 0)
        goto fail;
    if (signame) {
        v = JS_NewString(ctx, signame);
        if (!JS_IsException(v) && JS_SetPropertyStr(ctx, o, "signal", v) >= 0) {
            v = JS_UNDEFINED;
        } else {
            JS_FreeValue(ctx, v);
            goto fail;
        }
    } else {
        if (JS_SetPropertyStr(ctx, o, "signal", JS_NULL) < 0)
            goto fail;
    }
    if (JS_SetPropertyStr(ctx, o, "timedOut",
                          JS_NewBool(ctx, sp->timed_out)) < 0)
        goto fail;
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

/* Settle a parked wait(), exactly once. */
static void spawn_settle_wait(dyn_spawn_t *sp)
{
    if (sp->wait_settled || JS_IsUndefined(sp->wait_resolve))
        return;
    sp->wait_settled = 1;
    spawn_settle(sp->ctx, &sp->wait_resolve, &sp->wait_reject,
                 spawn_wait_result(sp), 0);
    spawn_unpark(sp);           /* last: the parked wait's reference */
}

/* Record a waited status into the wait() result fields. */
static void spawn_record_status(dyn_spawn_t *sp, int status)
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

/* One WNOHANG reaping attempt. Returns 1 when this call reaped. */
static int spawn_try_reap(dyn_spawn_t *sp)
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

/* Blocking reap of an ALREADY-SIGNALLED child (SIGKILL cannot be caught or
 * blocked, so this is bounded); records the real outcome. Never runs JS. */
static void spawn_reap_blocking(dyn_spawn_t *sp)
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

/* Drop the reap hook once the child is reaped (all callers do the same
 * dance, so it lives here). */
static void spawn_unhook_if_done(dyn_spawn_t *sp)
{
    if (sp->exited && sp->hooked) {
        dyn_net_off_drain(sp);
        sp->hooked = 0;
    }
}

/* The authoritative teardown, idempotent, never runs JS: SIGKILL the group,
 * blocking-reap (bounded -- SIGKILL cannot be caught), close every fd,
 * unhook, drop the reactor ref. Gates on torn_down, never on exited: a child
 * that merely has not exited yet still needs the kill. */
static void spawn_teardown(dyn_spawn_t *sp)
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

/* Reference helpers. An operation whose state lives in C (a pending read,
 * flush or wait) takes one until it settles, so the struct, its fds and the
 * watch that will settle it outlive every JS handle; the reactor callbacks
 * take one for their duration, so a settle may unpark mid-callback. */
static void spawn_ref(dyn_spawn_t *sp)
{
    sp->refs++;
}

static void spawn_unpark(dyn_spawn_t *sp)
{
    spawn_release(sp);
}

static void spawn_release(dyn_spawn_t *sp)
{
    if (--sp->refs > 0)
        return;
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

static void spawn_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(val, dyn_spawn_class_id);
    if (sp) {
        JS_SetOpaque(val, NULL);
        /* UNCONDITIONAL release: an abandoned Spawn (all handles dropped)
         * tears its child down on the last reference going away -- a read
         * still parked under a LIVE view keeps that view's reference, so
         * in-flight work is never torn down while anything can still
         * observe it. Pins freed here keep the engine's shutdown
         * accounting exact. */
        spawn_release(sp);
    }
}

static void spawn_pipe_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_spawn_pipe_t *p = (dyn_spawn_pipe_t *)JS_GetOpaque(val,
                                                    dyn_spawn_pipe_class_id);
    if (p && p->sp) {
        JS_SetOpaque(val, NULL);
        spawn_release(p->sp);
    }
}

static void spawn_stdin_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_spawn_stdin_t *in = (dyn_spawn_stdin_t *)JS_GetOpaque(val,
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
    /* no gc_mark: the views hold no C-pinned JSValues (see the single-
       marker note above); the Spawn object marks for the whole struct */
};

static JSClassDef dyn_spawn_stdin_class = {
    "SpawnStdin",
    .finalizer = spawn_stdin_finalizer,
};

/* ---- the drain hook: time-driven work (reap + timeout escalation) -------- */

static void spawn_reap_hook(void *ud)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)ud;
    int64_t now;

    spawn_ref(sp);              /* the hook's own reference */
    if (sp->dead)
        goto out;
    if (spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
        goto out;
    }
    /* Exec's escalation: SIGTERM at the deadline, SIGKILL one grace later. */
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
    spawn_release(sp);          /* last: may free the struct */
}

/* ---- stdout / stderr: continuous drain + pull reads ---------------------- */

/* Settle the pending read from whatever is buffered (also the EOF path:
 * buffered bytes first, then 0). Consumes the pending state. */
static void spawn_pipe_feed(dyn_spawn_pipe_t *p)
{
    dyn_spawn_t *sp = p->sp;
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
        p->rbase = NULL;
        p->rlen = 0;
        spawn_settle(sp->ctx, &rr, &rj, spawn_cap_error(sp->ctx, sp, p->err),
                     1);
        spawn_unpark(sp);
        return;
    }
    n = p->len < p->rlen ? p->len : p->rlen;
    if (n)
        memmove(p->rbase, p->buf, n);   /* different allocations, but the
                                           non-overlap is easier proven by
                                           construction than by review */
    if (n < p->len) {
        memmove(p->buf, p->buf + n, p->len - n);
        p->len -= n;
    } else {
        p->len = 0;
    }
    JS_FreeValue(sp->ctx, p->rbuf);
    p->rbuf = JS_UNDEFINED;
    p->rbase = NULL;
    p->rlen = 0;
    /* Room again: the child may be blocked on a full pipe. */
    spawn_pipe_watch(p, 1);
    spawn_settle(sp->ctx, &rr, &rj, JS_NewInt64(sp->ctx, (int64_t)n), 0);
    spawn_unpark(sp);           /* last: the read no longer needs the struct */
}

/* The sticky failure (OOM draining): kill the GROUP, mark the pipe, fail
 * its pending read. Called with the read loop holding no partial state. */
static void spawn_pipe_fail(dyn_spawn_pipe_t *p, int kind)
{
    dyn_spawn_t *sp = p->sp;

    if (!p->err)
        p->err = kind;
    spawn_kill_child(sp, SIGKILL);
    spawn_close_fd(sp, &p->fd);
    p->eof = 1;
    if (p->pending)
        spawn_pipe_feed(p);     /* p->err routes it to a rejection */
    spawn_try_reap(sp);         /* usually not dead yet; the hook finishes */
}

static void spawn_pipe_cb(dyn_evloop_t *lp, int fd, int events, void *ud)
{
    dyn_spawn_pipe_t *p = (dyn_spawn_pipe_t *)ud;
    dyn_spawn_t *sp = p->sp;
    uint8_t chunk[DYN_EXEC_CHUNK];

    (void)lp; (void)fd; (void)events;
    spawn_ref(sp);              /* the callback's own reference */
    if (sp->dead || p->fd < 0)
        goto out;
    /* FULL: backpressure, not kill. The watch goes off, the kernel pipe
     * fills, and the child blocks on write until a read() drains room
     * (spawn_pipe_watch re-arms). A caller that stops reading stalls the
     * child -- exactly what a pipe SHOULD do -- and memory stays bounded
     * by maxPipe. */
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
                goto out;       /* stale readiness: level-triggered re-fires */
            r = 0;              /* hard error: report EOF, keep the bytes */
        }
        if (r == 0) {
            spawn_close_fd(sp, &p->fd);
            p->eof = 1;
            if (p->pending)
                spawn_pipe_feed(p);
            /* The child usually exited right before closing its stdio. */
            if (spawn_try_reap(sp))
                spawn_unhook_if_done(sp);
            spawn_aio_release_if_idle(sp);
            goto out;
        }
        /* Append. An OOM here is the one sticky-failure case left: the
         * alternative -- dropping bytes -- corrupts the stream silently. */
        if (p->len + (size_t)r > p->cap) {
            size_t nc = p->cap ? p->cap * 2 : 4096;
            uint8_t *nb;
            while (nc < p->len + (size_t)r)
                nc *= 2;
            nb = (uint8_t *)realloc(p->buf, nc);
            if (!nb) {
                spawn_pipe_fail(p, 1);
                goto out;
            }
            p->buf = nb;
            p->cap = nc;
        }
        memcpy(p->buf + p->len, chunk, (size_t)r);
        p->len += (size_t)r;
        if (p->pending)
            spawn_pipe_feed(p);
        else if (p->len >= sp->maxpipe)
            spawn_pipe_watch(p, 0);
        /* ONE read per dispatch: level-triggered readiness re-fires if the
         * child keeps producing, and stderr gets its turn between wakes. */
        goto out;
    }
out:
    spawn_release(sp);          /* last: may free the struct */
}

static uint8_t *spawn_view_bytes(JSContext *ctx, JSValueConst v, size_t *plen)
{
    size_t off, len, bpe, ab;
    JSValue buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    uint8_t *base;

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

/* Fail a parked read (close() on the pipe, or Spawn close() failing all
 * pending I/O). Consumes the parked state; the rejection is the point. */
static void spawn_pipe_fail_pending(dyn_spawn_pipe_t *p, const char *msg)
{
    dyn_spawn_t *sp = p->sp;

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
    p->rbase = NULL;
    p->rlen = 0;
    spawn_unpark(sp);           /* last: the failed read released its ref */
}

/* read(buf) -> Promise<number>. The dyna:stream ByteSource shape. */
static JSValue spawn_pipe_read(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_spawn_pipe_t *p;
    uint8_t *base;
    size_t len = 0;
    JSValue promise, resolve, reject;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: read(buf) needs a Uint8Array");
    /* Coerce FIRST (house rule: the coercion can run user JS that closes
     * `this`), then resolve the native handle. */
    base = spawn_view_bytes(ctx, argv[0], &len);
    if (!base)
        return JS_EXCEPTION;
    p = (dyn_spawn_pipe_t *)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
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
    /* Buffered ahead: answer inline, the dyna:stream inline arm. */
    if (p->len > 0) {
        size_t n = p->len < len ? p->len : len;
        memcpy(base, p->buf, n);
        if (n < p->len) {
            memmove(p->buf, p->buf + n, p->len - n);
            p->len -= n;
        } else {
            p->len = 0;
        }
        spawn_pipe_watch(p, 1);   /* room made: the child may be blocked */
        return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
    if (p->eof)
        return spawn_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    /* Park until readiness. The promise is returned alive (the caller owns
     * it); only the settle pair is kept on the C side. */
    promise = spawn_promise_new(ctx, &resolve, &reject);
    if (JS_IsException(promise))
        return promise;
    p->pending = 1;
    p->rbuf = JS_DupValue(ctx, argv[0]);
    p->rbase = base;
    p->rlen = len;
    p->rresolve = resolve;
    p->rreject = reject;
    /* The promise is C-pinned so the awaiting frame's state survives the
     * last-use drop of `p`; the park reference keeps the struct (and the
     * watch that will settle it) alive with no JS handle left. */
    p->promise = JS_DupValue(ctx, promise);
    spawn_ref(p->sp);
    return promise;
}

/* close() on stdout/stderr: stop the drain. A pending read is failed rather
 * than left hanging (a promise that never settles leaks its state). */
static JSValue spawn_pipe_close(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_spawn_pipe_t *p;
    (void)argc; (void)argv;

    p = (dyn_spawn_pipe_t *)JS_GetOpaque(this_val, dyn_spawn_pipe_class_id);
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

/* ---- stdin: queued writes, flushed on WRITE readiness -------------------- */

static void spawn_stdin_unwatch(dyn_spawn_t *sp)
{
    if (sp->in.fd >= 0 && sp->aio)
        dyn_evloop_del(dyn_aio_evloop(sp->aio), sp->in.fd);
}

static void spawn_stdin_close_now(dyn_spawn_t *sp)
{
    spawn_stdin_unwatch(sp);
    if (sp->in.fd >= 0) {
        close(sp->in.fd);
        sp->in.fd = -1;
    }
    spawn_aio_release_if_idle(sp);
}

/* The sticky stdin write error, as a VALUE for rejections. */
static JSValue spawn_stdin_error(JSContext *ctx, dyn_spawn_stdin_t *in)
{
    JS_ThrowInternalError(ctx, "Spawn: stdin write failed (%s)",
                          strerror(in->err_errno ? in->err_errno : EPIPE));
    return JS_GetException(ctx);
}

static void spawn_stdin_reject_flush(dyn_spawn_t *sp)
{
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
                     spawn_stdin_error(sp->ctx, &sp->in), 1);
        spawn_unpark(sp);       /* last: the parked flush's reference */
    }
}

static void spawn_stdin_cb(dyn_evloop_t *lp, int fd, int events, void *ud)
{
    dyn_spawn_stdin_t *in = (dyn_spawn_stdin_t *)ud;
    dyn_spawn_t *sp = in->sp;

    (void)lp; (void)fd; (void)events;
    spawn_ref(sp);              /* the callback's own reference */
    if (sp->dead || in->fd < 0)
        goto out;
    while (sp->woff < sp->wlen) {
        ssize_t w = write(in->fd, sp->wq + sp->woff, sp->wlen - sp->woff);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                goto out;
            /* EPIPE or worse: the queued bytes are gone; say so on the
             * pending flush, and make later writes refuse rather than
             * accept bytes that can never be written. */
            in->err = 1;
            in->err_errno = errno;
            spawn_stdin_close_now(sp);
            spawn_stdin_reject_flush(sp);
            goto out;
        }
        sp->woff += (size_t)w;
    }
    sp->woff = sp->wlen = 0;
    spawn_stdin_unwatch(sp);    /* no WRITE interest while the queue is empty */
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        spawn_settle(sp->ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
                     JS_UNDEFINED, 0);
        spawn_unpark(sp);       /* the parked flush settled */
    }
    if (in->close_after_drain)
        spawn_stdin_close_now(sp);
out:
    spawn_release(sp);          /* last: may free the struct */
}

/* Copy `src` into the stdin queue (a caller's view must not be pinned across
 * an await). Arms the WRITE watch while bytes are queued. Returns 0, or -1
 * having thrown. */
static int spawn_stdin_enqueue(dyn_spawn_t *sp, const uint8_t *src, size_t n)
{
    if (sp->wlen + n > sp->wcap) {
        size_t nc = sp->wcap ? sp->wcap : 4096;
        uint8_t *nw;
        while (nc < sp->wlen + n)
            nc *= 2;
        nw = (uint8_t *)realloc(sp->wq, nc);
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

/* write(buf) -> Promise<number>: the WHOLE view accepted (ByteSink's
 * buffered-sink arm); the drain happens on WRITE readiness. */
static JSValue spawn_stdin_write(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_spawn_stdin_t *in;
    const uint8_t *src;
    size_t n = 0;
    int is_str;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Spawn: stdin.write(buf) needs bytes");
    is_str = JS_IsString(argv[0]);
    if (is_str) {
        const char *cs;
        n = 0;                  /* the out-param is stale on failure */
        cs = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!cs)
            return JS_EXCEPTION;
        src = (const uint8_t *)cs;
    } else {
        src = spawn_view_bytes(ctx, argv[0], &n);
        if (!src)
            return JS_EXCEPTION;
    }
    in = (dyn_spawn_stdin_t *)JS_GetOpaque(this_val,
                                           dyn_spawn_stdin_class_id);
    if (!in || !in->sp || in->sp->dead) {
        if (is_str)
            JS_FreeCString(ctx, (const char *)src);
        return JS_ThrowTypeError(ctx, "Spawn: write on a closed stdin");
    }
    if (in->sp->user_closed) {
        if (is_str)
            JS_FreeCString(ctx, (const char *)src);
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
            JS_FreeCString(ctx, (const char *)src);
        return r;
    }
    if (n && spawn_stdin_enqueue(in->sp, src, n) < 0) {
        if (is_str)
            JS_FreeCString(ctx, (const char *)src);
        return JS_EXCEPTION;
    }
    if (is_str)
        JS_FreeCString(ctx, (const char *)src);
    return spawn_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
}

static JSValue spawn_stdin_flush(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_spawn_stdin_t *in;
    JSValue promise, resolve, reject;
    (void)argc; (void)argv;

    in = (dyn_spawn_stdin_t *)JS_GetOpaque(this_val,
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
    spawn_ref(in->sp);          /* park: settled by the WRITE pump/close() */
    return promise;
}

/* close() on stdin: EOF after the queue drains (close-after-flush, so a
 * write() immediately before close() is not silently lost). */
static JSValue spawn_stdin_close(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_spawn_stdin_t *in;
    (void)argc; (void)argv;

    in = (dyn_spawn_stdin_t *)JS_GetOpaque(this_val,
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
    /* else: the pump closes after the queue drains */
    return JS_UNDEFINED;
}

/* ---- Spawn: wait / kill / close / facts ---------------------------------- */

static dyn_spawn_t *spawn_this(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    if (!sp || sp->dead)
        JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return sp;
}

/* wait() -> Promise<{code, signal, timedOut}>. */
static JSValue dyn_spawn_wait(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    JSValue promise, resolve, reject;
    (void)argc; (void)argv;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        /* A closed process's recorded outcome answers immediately (close()
         * killed + reaped it before recording). */
        return spawn_promise_resolved(ctx, spawn_wait_result(sp));
    /* Fresh ask the kernel first: fresher than the last tick. */
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
    spawn_ref(sp);              /* park: settled by the reap hook/close() */
    /* Race: the child may have exited between the check and the park. One
     * more WNOHANG settles it through the ordinary path (correct status);
     * otherwise the EOF callback, the reap hook, or kill() settles later. */
    if (spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return promise;
}

/* kill(signal="SIGTERM") -> true if the child was still running. */
static int spawn_sig_from_name(const char *s)
{
    static const struct { const char *nm; int sig; } tab[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
        { "ILL", SIGILL }, { "ABRT", SIGABRT }, { "FPE", SIGFPE },
        { "KILL", SIGKILL }, { "SEGV", SIGSEGV }, { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "BUS", SIGBUS },
        { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "CHLD", SIGCHLD },
        { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP },
        { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcasecmp(tab[i].nm, s) == 0)
            return tab[i].sig;
    return 0;
}

static JSValue dyn_spawn_kill(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    int sig = SIGTERM;

    if (!sp || sp->dead)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    if (sp->user_closed)
        return JS_FALSE;        /* nothing left to signal */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
            const char *s = JS_ToCString(ctx, argv[0]);
            const char *nm;
            if (!s)
                return JS_EXCEPTION;
            /* keep `s` the exact pointer ToCString returned: FreeCString
               must see the allocation's base (an interior free here
               scribbled the heap -- found by the probe battery). */
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
    /* SIGKILL closes the pipes promptly; the reap hook covers the rest. */
    if (spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return JS_TRUE;
}

/* close()/dispose(): the AUTHORITATIVE, deterministic teardown -- SIGKILL
 * the group for real, blocking-reap it, close the fds, drop the reactor --
 * REGARDLESS of how many stdio views are alive (views keep the struct
 * alive for GC; they never keep the process alive). The parked wait()
 * settles with the RECORDED kill result, promptly. Pending reads reject;
 * a pending flush rejects; every later operation on the object or its
 * views throws "the process is closed". */
static JSValue dyn_spawn_close(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_spawn_t *sp = spawn_this(ctx, this_val);
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    spawn_teardown(sp);             /* the REAL kill + reap (idempotent) */
    spawn_settle_wait(sp);          /* parked wait(): the recorded SIGKILL */
    spawn_pipe_fail_pending(&sp->out, "read() aborted by close()");
    spawn_pipe_fail_pending(&sp->err, "read() aborted by close()");
    if (!JS_IsUndefined(sp->in.flush_resolve)) {
        JS_ThrowTypeError(ctx, "Spawn: stdin closed by close()");
        spawn_settle(ctx, &sp->in.flush_resolve, &sp->in.flush_reject,
                     JS_GetException(ctx), 1);
        spawn_unpark(sp);
    }
    sp->user_closed = 1;
    JS_SetOpaque(this_val, NULL);   /* the getters now answer closed/-1/true */
    spawn_release(sp);              /* the object's own reference; may free
                                       (teardown already dropped the
                                       registry ref) */
    return JS_UNDEFINED;
}

static JSValue spawn_get_closed(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewBool(ctx,
                      JS_GetOpaque(this_val, dyn_spawn_class_id) == NULL);
}

static JSValue spawn_get_pid(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    if (!sp || sp->dead)
        return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, (int32_t)sp->pid);
}

static JSValue spawn_get_exited(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    if (!sp || sp->dead)
        return JS_TRUE;
    if (!sp->exited && spawn_try_reap(sp)) {
        spawn_unhook_if_done(sp);
        spawn_aio_release_if_idle(sp);
    }
    return JS_NewBool(ctx, sp->exited);
}

/* The stdio views: SURFACE-STABLE accessors (the names live on the
 * prototype, which is what the .d.ts sweep checks). Each access builds a
 * FRESH handle holding one native reference -- a view pulled into a local
 * outlives a dropped Spawn, and no view object is ever C-pinned (a C cache
 * would hold it past the final GC and trip the shutdown accounting). */
static JSValue spawn_view_get(JSContext *ctx, JSValueConst this_val,
                              JSClassID cid, void *opaque)
{
    dyn_spawn_t *sp;
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
    sp->refs++;                     /* each live view releases its reference */
    return obj;
}

static JSValue spawn_get_stdout(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_pipe_class_id, &sp->out);
}

static JSValue spawn_get_stderr(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
                                                  dyn_spawn_class_id);
    if (!sp || sp->dead || sp->user_closed)
        return JS_ThrowTypeError(ctx, "Spawn: the process is closed");
    return spawn_view_get(ctx, this_val, dyn_spawn_pipe_class_id, &sp->err);
}

static JSValue spawn_get_stdin(JSContext *ctx, JSValueConst this_val)
{
    dyn_spawn_t *sp = (dyn_spawn_t *)JS_GetOpaque(this_val,
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

/* ---- Spawn's own argv/env/option builders --------------------------------
 * Identical NUL doctrine to Exec's, with Spawn-prefixed errors;
 * dyna-proc.inc.c is left untouched. */

static int spawn_strv_at(JSContext *ctx, JSValueConst arr, uint32_t i,
                         char **slot)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    const char *s;

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

static char **spawn_strv(JSContext *ctx, JSValueConst arr, const char *first)
{
    uint32_t n = 0, i, k = 0;
    char **v;
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
    v = (char **)calloc((size_t)n + 2, sizeof *v);
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

static int spawn_env_at(JSContext *ctx, JSValueConst obj, JSAtom atom,
                        char **slot)
{
    JSValue val = JS_GetProperty(ctx, obj, atom);
    const char *nm, *vs;
    size_t nm_len, sz;

    /* The same NUL refusal as every other dyna:sys boundary: execve acts on
     * the truncated prefix of "NAME\0junk". */
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
    vs = JS_IsException(val) ? NULL : dyn_sys_cstr(ctx, val,
                                                   "Spawn: env value");
    JS_FreeValue(ctx, val);
    if (!vs) {
        JS_FreeCString(ctx, nm);
        return -1;
    }
    sz = nm_len + strlen(vs) + 2;       /* nm_len: hoisted at the NUL check */
    *slot = (char *)malloc(sz);
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

static char **spawn_envv(JSContext *ctx, JSValueConst obj)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i, k = 0;
    char **v;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return NULL;
    v = (char **)calloc((size_t)len + 1, sizeof *v);
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

/* An integer option with a floor; non-finite fails CLOSED (NaN coerced to 0
   would turn a deadline into "no deadline" or a cap into "no cap"). */
static int spawn_opt_int(JSContext *ctx, JSValueConst o, const char *key,
                         int64_t floor_, const char *what, int64_t *out)
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

/* A uid/gid option: when present, a non-negative integer that fits uid_t
   (the Exec rule; a NaN coerced to 0 would mean ROOT). */
static int spawn_opt_id(JSContext *ctx, JSValueConst o, const char *key,
                        int64_t *out)
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
    if (t < 0 || t > (int64_t)UINT_MAX) {
        JS_ThrowRangeError(ctx,
            "Spawn: %s must be a non-negative integer id", key);
        return -1;
    }
    *out = t;
    return 0;
}

/* SIGPIPE: ignored process-wide from the first Spawn on (Node's policy; see
 * the block comment). The child restores SIG_DFL before exec. A disposition
 * the process owner already set is left alone. */
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

/* ---- the constructor ----------------------------------------------------- */

/* Everything that must be freed on the way out of the ctor, before `sp`
 * exists (sp owns nothing yet). */
static void spawn_ctor_free_exec(dyn_exec_t *e, JSContext *ctx,
                                 JSValue input_ref)
{
    dyn_strv_free(e->argv);
    dyn_strv_free(e->envp);
    free(e->cwd);
    free((void *)e->input);
    e->input = NULL;
    JS_FreeValue(ctx, input_ref);
}

/* new Spawn(cmd, args?, opts?) -- and plain Spawn(cmd, ...) (a C constructor
 * handles a missing new.target). */
static JSValue dyn_spawn_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    dyn_exec_t e;
    JSValue obj = JS_UNDEFINED, input_ref = JS_UNDEFINED;
    JSValue proto;
    dyn_spawn_t *sp = NULL;
    const char *cmd, *path = NULL;
    struct dyn_aio *aio;
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
    /* The NUL refusal that matters most: execve on the truncated prefix runs
     * a DIFFERENT program than the validated string (the Exec doctrine). */
    cmd = dyn_sys_cstr(ctx, argv[0], "Spawn: command");
    if (!cmd)
        goto fail_noobj;
    e.argv = spawn_strv(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, cmd);
    JS_FreeCString(ctx, cmd);
    if (!e.argv)
        goto fail_noobj;

    /* options */
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
            const char *s = dyn_sys_cstr(ctx, v, "Spawn: cwd");
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
                          "timeoutMs must not be negative", &timeout_ms) < 0
            || spawn_opt_int(ctx, argv[2], "maxPipe", 1,
                             "maxPipe must be positive", &maxpipe) < 0
            || spawn_opt_id(ctx, argv[2], "uid", &uid) < 0
            || spawn_opt_id(ctx, argv[2], "gid", &gid) < 0)
            goto fail;
        e.uid = (uid_t)uid;
        e.gid = (gid_t)gid;
        v = JS_GetPropertyStr(ctx, argv[2], "stdin");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char *sm = JS_ToCString(ctx, v);
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
            /* false skips empty PATH elements (the execvp cwd search),
             * exactly Exec's allowPathCwd:false. */
            e.no_path_cwd = !b;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    e.timeout_ms = timeout_ms;

    /* input: string or bytes, COPIED before the fork (the JS values must
     * not be pinned across the child's lifetime). */
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "input");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const uint8_t *src;
            size_t n = 0;
            int is_str = JS_IsString(v);
            n = 0;              /* the out-param is stale on failure */
            if (is_str) {
                const char *cs = JS_ToCStringLen(ctx, &n, v);
                src = (const uint8_t *)cs;
            } else {
                src = spawn_view_bytes(ctx, v, &n);
            }
            if (!src) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
            stdin_pipe = 1;         /* input requires the pipe */
            e.input = (const uint8_t *)malloc(n ? n : 1);
            if (!e.input) {
                if (is_str)
                    JS_FreeCString(ctx, (const char *)src);
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            memcpy((void *)e.input, src, n);
            if (is_str)
                JS_FreeCString(ctx, (const char *)src);
            e.inlen = n;
        }
        JS_FreeValue(ctx, v);
    }

    /* execve does not search: the PARENT resolves, against the child's PATH
     * when the environment is being replaced (the Exec rule). */
    for (i = 0; e.envp && e.envp[i]; i++)
        if (strncmp(e.envp[i], "PATH=", 5) == 0) {
            path = e.envp[i] + 5;
            break;
        }
    if (!path && !e.envp)
        path = getenv("PATH");
    if (dyn_path_lookup(e.argv[0], path, e.path, sizeof e.path,
                        e.no_path_cwd) < 0) {
        JS_ThrowInternalError(ctx, "Spawn: command not found: %s",
                              e.argv[0]);
        goto fail;
    }
    /* stdin (Bun's defaults): "ignore" (the default) gives the child
     * /dev/null -- a child that reads stdin gets EOF, never a hang;
     * "pipe" creates p.stdin (write/flush/close) and is implied by
     * opts.input; "inherit" passes the engine's stdin through. */
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
    {
        long mf = sysconf(_SC_OPEN_MAX);
        if (mf < 3 || mf > 4096)
            mf = 4096;
        e.maxfd = mf;
    }

    /* The async reactor, ref'd for the Spawn's lifetime: it keeps
     * js_os_poll's exit closed, so the child is pumped even if the script
     * never awaits anything. */
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
        /* child: only async-signal-safe calls (dyn_exec_child's contract) */
        if (e.in[1] >= 0)
            close(e.in[1]);
        close(e.out[0]);
        close(e.err[0]);
        if (!stdin_pipe && !stdin_inherit) {
            /* "ignore": /dev/null on fd 0 -- reads give EOF, and the child
             * is never blocked on a pipe whose writer will never close. */
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
    /* parent: keep the ends we own, nonblocking for the reactor */
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

    sp = (dyn_spawn_t *)calloc(1, sizeof *sp);
    if (!sp) {
        JS_ThrowOutOfMemory(ctx);
        dyn_net_reactor_release(ctx);
        goto fail_parent;
    }
    sp->refs = 1;               /* the object's own reference */
    sp->rt = JS_GetRuntime(ctx);
    sp->ctx = ctx;
    sp->pid = pid;
    sp->aio = aio;
    sp->maxpipe = (size_t)maxpipe;
    sp->out.sp = sp;
    sp->err.sp = sp;
    sp->in.sp = sp;
    /* calloc zeroes are NOT JS_UNDEFINED: every pinned JSValue starts
       undefined explicitly. */
    sp->wait_resolve = sp->wait_reject = JS_UNDEFINED;
    sp->out.rbuf = sp->out.rresolve = sp->out.rreject = JS_UNDEFINED;
    sp->err.rbuf = sp->err.rresolve = sp->err.rreject = JS_UNDEFINED;
    sp->out.promise = sp->err.promise = JS_UNDEFINED;
    sp->in.flush_resolve = sp->in.flush_reject = JS_UNDEFINED;
    sp->out.fd = e.out[0];
    sp->err.fd = e.err[0];
    sp->in.fd = stdin_pipe ? e.in[1] : -1;
    sp->in.piped = stdin_pipe;
    if (timeout_ms > 0)
        sp->deadline = dyn_now_ms() + timeout_ms;

    proto = dyn_ctor_proto(ctx, new_target, dyn_spawn_class_id);
    if (JS_IsException(proto))
        goto fail_spawn;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_spawn_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail_parent;
    JS_SetOpaque(obj, sp);

    sp->out.watch = sp->err.watch = 1;
    if (dyn_evloop_add(dyn_aio_evloop(aio), sp->out.fd, DYN_EV_READ,
                       spawn_pipe_cb, &sp->out) < 0
        || dyn_evloop_add(dyn_aio_evloop(aio), sp->err.fd, DYN_EV_READ,
                          spawn_pipe_cb, &sp->err) < 0
        || (sp->in.fd >= 0
            && dyn_evloop_add(dyn_aio_evloop(aio), sp->in.fd, 0,
                              spawn_stdin_cb, &sp->in) < 0)) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not watch the child's pipes (%s)", strerror(errno));
        goto fail_spawn;
    }
    /* The reap hook arms the reactor's periodic tick: the wakeup that makes
     * exit detection and timeout escalation run on a clock, not on traffic. */
    if (dyn_net_on_drain(spawn_reap_hook, sp) < 0) {
        JS_ThrowInternalError(ctx,
            "Spawn: could not register the reaper; wait() may never settle");
        goto fail_spawn;
    }
    sp->hooked = 1;

    /* opts.input: enqueue, then EOF once it drains (Exec's pump_write rule:
     * the child reads until EOF, it never guesses a length). */
    if (e.input) {
        if (spawn_stdin_enqueue(sp, e.input, e.inlen) < 0)
            goto fail_spawn;
        sp->in.close_after_drain = 1;
    }

    /* The exec structs were only needed through the fork. */
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return obj;

fail_spawn:
    /* Single-free invariant: the Spawn object's finalizer (via release)
     * would drop the object's own reference, but a failing constructor must
     * not wait for a GC -- release it here and keep the finalizer out
     * (opaque NULL) so nothing double-releases. Any cached views (created
     * only through the getters, never in the ctor) release through the same
     * path. */
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
    /* The child exists: kill and reap it, close the parent's ends. */
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
            if (e.in[k] >= 0)  close(e.in[k]);
            if (e.out[k] >= 0) close(e.out[k]);
            if (e.err[k] >= 0) close(e.err[k]);
        }
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail:
    for (i = 0; i < 2; i++) {
        if (e.in[i] >= 0)  close(e.in[i]);
        if (e.out[i] >= 0) close(e.out[i]);
        if (e.err[i] >= 0) close(e.err[i]);
    }
    spawn_ctor_free_exec(&e, ctx, input_ref);
    return JS_EXCEPTION;

fail_noobj:
    return JS_EXCEPTION;
}

/* ---- class registration -------------------------------------------------- */

static int dyn_spawn_register_classes(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;

    /* PLAIN classes (opaque = the struct) with custom finalizer + gc_mark,
       the dyna:file File pattern: the native holds JSValues, so the cycle
       collector must be able to trace them. close()/dispose() are Spawn's
       OWN methods (they kill + reap), not the shared DynResource ones. */
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

/* Export the Spawn constructor from the dyna:sys module. Called from
 * dyn_sys_init_module. */
static int dyn_spawn_register_export(JSContext *ctx, JSModuleDef *m)
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
    /* process / environment */
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
    /* Spawn is a CLASS (constructor + proto), registered and exported here;
     * the free functions follow from the list. */
    if (dyn_spawn_register_export(ctx, m) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

int js_nat_init_sys(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:sys", dyn_sys_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Spawn") < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_sys_funcs,
                                  (int)countof(dyn_sys_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SYS */
