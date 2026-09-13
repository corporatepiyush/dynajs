/* Subprocesses for dyna:sys (design 23). ARGV ONLY -- there is no shell
   interface and no `shell: true`, so command injection is unrepresentable.
   A caller who wants a shell writes Exec("/bin/sh", ["-c", s]) and owns it. */

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>

#include "core/dyn-sb.h"

/* A child can produce faster than a caller can want. */
#define DYN_EXEC_MAXBUF   (8u << 20)
#define DYN_EXEC_GRACE_MS 2000          /* SIGTERM, then this, then SIGKILL */
#define DYN_EXEC_CHUNK    65536

/* Byte views in, Uint8Array out. dyna:sys had no binary surface before this. */
static JSValue dyn_sys_bytes_new(JSContext *ctx, const uint8_t *p, size_t n)
{
    static const uint8_t zero_stub = 0;
    JSValueConst ta[3];
    JSValue ab, r;

    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero_stub, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;                         /* all THREE: with one argument this
                                           builds a view of length zero */
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return r;
}

static int dyn_sys_bytes(JSContext *ctx, JSValueConst v, const uint8_t **pp,
                         size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a string or a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base || off > ab || len > ab - off) {
        if (base) JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* The name, so a caller can tell SIGTERM from SIGKILL from a nonzero exit. */
static const char *dyn_signal_name(int sig)
{
    switch (sig) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGBUS:  return "SIGBUS";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    default:      return "SIGNAL";
    }
}

static int dyn_is_exec(const char *p);

/* Resolve a program name against a PATH. The CHILD cannot search when it has a
   replacement environment (execve does not), so the parent always resolves and
   a missing command is a clear error rather than a bare exit code 127. */
static int dyn_path_lookup(const char *name, const char *path, char *buf,
                           size_t bufsz)
{
    size_t nl;
    if (strchr(name, '/')) {
        /* One scan: strcpy would walk `name` a third time, after strchr and
           strlen have each already walked it. */
        nl = strlen(name);
        if (nl >= bufsz || !dyn_is_exec(name))
            return -1;
        memcpy(buf, name, nl + 1);
        return 0;
    }
    if (!path)
        path = "/usr/bin:/bin";
    nl = strlen(name);                /* invariant: hoisted out of the loop */
    while (*path) {
        /* One pass: strchr walked to the terminator, strlen walked it again. */
        const char *sep = path;
        size_t dlen;
        const char *dir;
        while (*sep && *sep != ':') sep++;
        dlen = (size_t)(sep - path);
        if (!*sep) sep = NULL;
        dir = dlen ? path : ".";
        if (!dlen) dlen = 1;
        if (dlen + 1 + nl + 1 <= bufsz) {
            memcpy(buf, dir, dlen);
            buf[dlen] = '/';
            memcpy(buf + dlen + 1, name, nl + 1);
            if (dyn_is_exec(buf))
                return 0;
        }
        if (!sep)
            break;
        path = sep + 1;
    }
    return -1;
}

/* Thin wrapper over core/dyn-sb.h. The drift is deliberate and kept: the
   caller-supplied `cap` bound (DYN_EXEC_MAXBUF) is checked BEFORE growing,
   the seed is 4096 (pipe-sized chunks), and add keeps both error signals --
   sticky oom flag and -1 return. */
typedef struct { uint8_t *p; size_t n, cap; int oom; } dyn_pbuf_t;

static void dyn_pbuf_free(dyn_pbuf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static int dyn_pbuf_add(dyn_pbuf_t *b, const uint8_t *p, size_t n, size_t cap)
{
    if (b->n + n > cap)
        return -1;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 4096)) {
        b->oom = 1;
        return -1;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}

/* The child is its own group leader, so the group id is its pid. Signal both:
   the group for the tree, the pid in case setsid() failed. */
static void dyn_kill_group(pid_t pid, int sig)
{
    kill(-pid, sig);
    kill(pid, sig);
}

static int64_t dyn_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One element, duplicated out of the engine's heap into plain malloc: after
   the fork nothing may call into the engine at all. */
static int dyn_strv_at(JSContext *ctx, JSValueConst arr, uint32_t i, char **slot)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    const char *s;

    if (JS_IsException(e))
        return -1;
    if (!JS_IsString(e)) {
        JS_FreeValue(ctx, e);
        JS_ThrowTypeError(ctx, "every argument must be a string");
        return -1;
    }
    s = JS_ToCString(ctx, e);
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

/* A NULL-terminated char* vector from a JS array of strings. Built BEFORE the
   fork: after it, only async-signal-safe calls are legal. */
static char **dyn_strv(JSContext *ctx, JSValueConst arr, const char *first)
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
            JS_ThrowRangeError(ctx, "too many arguments");
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
        if (!v[k]) goto oom;
        k++;
    }
    for (i = 0; i < n; i++) {
        if (dyn_strv_at(ctx, arr, i, &v[k]) < 0)
            goto fail;
        k++;
    }
    return v;
oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    for (i = 0; i < k; i++) free(v[i]);
    free(v);
    return NULL;
}

/* One `name=value`, in plain malloc for the same reason. */
static int dyn_envv_at(JSContext *ctx, JSValueConst obj, JSAtom atom, char **slot)
{
    JSValue val = JS_GetProperty(ctx, obj, atom);
    const char *nm = JS_AtomToCString(ctx, atom);
    const char *vs = JS_IsException(val) ? NULL : JS_ToCString(ctx, val);
    size_t sz;

    JS_FreeValue(ctx, val);
    if (!nm || !vs) {
        if (nm) JS_FreeCString(ctx, nm);
        if (vs) JS_FreeCString(ctx, vs);
        return -1;
    }
    sz = strlen(nm) + strlen(vs) + 2;
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

/* `name=value` strings from an object. NULL means "inherit". */
static char **dyn_envv(JSContext *ctx, JSValueConst obj)
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
        if (dyn_envv_at(ctx, obj, tab[i].atom, &v[k]) < 0)
            goto fail;
        k++;
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return v;
fail:
    JS_FreePropertyEnum(ctx, tab, len);
    for (i = 0; i < k; i++) free(v[i]);
    free(v);
    return NULL;
}

static void dyn_strv_free(char **v)
{
    size_t i;
    if (!v)
        return;
    for (i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

/* Everything the child needs, resolved before the fork. */
typedef struct {
    char   **argv, **envp;
    char    *cwd;
    char     path[PATH_MAX];            /* resolved before the fork */
    long     maxfd;                     /* captured pre-fork: sysconf not AS-safe */
    int      in[2], out[2], err[2];
    int64_t  timeout_ms;
    size_t   maxbuf;
    uid_t    uid;                       /* (uid_t)-1: the option was absent */
    gid_t    gid;
    const uint8_t *input;
    size_t   inlen;
} dyn_exec_t;

/* The child half. ONLY async-signal-safe calls are legal here: no malloc, no
   JS, no stdio. SIGPIPE is restored because an ignored disposition is
   inherited across exec and changes how the child behaves in a pipeline. */
static void dyn_exec_child(const dyn_exec_t *e)
{
    long maxfd = e->maxfd, fd;

    signal(SIGPIPE, SIG_DFL);
    setsid();                           /* so a timeout can signal the GROUP */
    if (e->in[0] >= 0 && dup2(e->in[0], 0) < 0) _exit(127);
    if (dup2(e->out[1], 1) < 0) _exit(127);
    if (dup2(e->err[1], 2) < 0) _exit(127);
    if (maxfd < 3 || maxfd > 4096)
        maxfd = 4096;
    for (fd = 3; fd < maxfd; fd++)
        close((int)fd);
    if (e->cwd && chdir(e->cwd) < 0)
        _exit(127);
    /* Drop privileges LAST (gid before uid: once the uid is gone neither the
       setgid nor a re-setuid can happen). (uid_t)-1 is the "absent" sentinel,
       which is also what setuid(-1)/setgid(-1) mean by POSIX, so the test is
       belt and braces. A failure here must not exec with the wrong identity:
       it reports through the same channel every child-side failure uses. */
    if (e->gid != (gid_t)-1 && setgid(e->gid) != 0)
        _exit(127);
    if (e->uid != (uid_t)-1 && setuid(e->uid) != 0)
        _exit(127);
    execve(e->path, e->argv, e->envp ? e->envp : dyn_environ);
    _exit(127);                         /* exec failed after the fork */
}

/* The descriptors still worth polling, and where each landed. */
static int dyn_pump_fds(dyn_exec_t *e, struct pollfd *pf, int *o_i, int *e_i,
                        int *w_i)
{
    int nf = 0;

    *o_i = *e_i = *w_i = -1;
    if (e->out[0] >= 0) { pf[nf].fd = e->out[0]; pf[nf].events = POLLIN; *o_i = nf++; }
    if (e->err[0] >= 0) { pf[nf].fd = e->err[0]; pf[nf].events = POLLIN; *e_i = nf++; }
    if (e->in[1] >= 0)  { pf[nf].fd = e->in[1];  pf[nf].events = POLLOUT; *w_i = nf++; }
    return nf;
}

/* SIGTERM at the deadline, SIGKILL after the grace period. Returns 1 when it
   signalled, so the caller re-polls rather than reading a stale revents. */
static int dyn_pump_expire(pid_t pid, int64_t now, int64_t deadline,
                           int *killed, int64_t *kill_at, int *timed_out)
{
    if (deadline && !*killed && now >= deadline) {
        /* The whole GROUP: a grandchild holding the pipe keeps it open, and
           killing only the child leaves the drain waiting for an EOF that a
           30-second orphan is still sitting on. */
        dyn_kill_group(pid, SIGTERM);
        *killed = 1;
        *timed_out = 1;
        *kill_at = now + DYN_EXEC_GRACE_MS;
        return 1;
    }
    if (*killed && *kill_at && now >= *kill_at) {
        dyn_kill_group(pid, SIGKILL);
        *kill_at = 0;
        return 1;
    }
    return 0;
}

/* Drain both pipes WHILE waiting: a child that fills a pipe buffer and blocks
   is the classic deadlock, and it is only avoided by never waiting on exit
   with a full pipe. Returns 0, or -1 having thrown. */
/* Feed the child's stdin. POLLHUP means it is gone, and a write into a closed
   pipe raises SIGPIPE, so closing beats writing. */
static void dyn_pump_write(dyn_exec_t *e, short revents, size_t *sent)
{
    size_t left;
    ssize_t r;

    if (revents & (POLLERR | POLLHUP)) {
        close(e->in[1]);
        e->in[1] = -1;
        return;
    }
    if (!(revents & POLLOUT))
        return;
    left = e->inlen - *sent;
    if (left > DYN_EXEC_CHUNK)
        left = DYN_EXEC_CHUNK;
    r = left ? write(e->in[1], e->input + *sent, left) : 0;
    if (r > 0)
        *sent += (size_t)r;
    if (r < 0 && errno == EINTR)
        return;
    if (r <= 0 || *sent >= e->inlen) {
        close(e->in[1]);                /* EOF for the child */
        e->in[1] = -1;
    }
}

/* Drain one of the child's output pipes. Returns -1 having thrown when the
   capture would exceed maxBuffer -- refusing beats silently truncating. */
static int dyn_pump_read(JSContext *ctx, dyn_exec_t *e, int fd, int is_out,
                         dyn_pbuf_t *b, pid_t pid)
{
    uint8_t buf[DYN_EXEC_CHUNK];
    ssize_t r = read(fd, buf, sizeof buf);

    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 0;
        r = 0;
    }
    if (r == 0) {
        close(fd);
        if (is_out) e->out[0] = -1;
        else        e->err[0] = -1;
        return 0;
    }
    if (dyn_pbuf_add(b, buf, (size_t)r, e->maxbuf) < 0) {
        dyn_kill_group(pid, SIGKILL);
        if (b->oom)
            JS_ThrowOutOfMemory(ctx);
        else
            JS_ThrowRangeError(ctx,
                "Exec: child output exceeds maxBuffer (%u bytes)",
                (unsigned)e->maxbuf);
        return -1;
    }
    return 0;
}

/* Drain both pipes WHILE waiting: a child that fills a pipe buffer and blocks
   is the classic deadlock, and it is only avoided by never waiting on exit
   with a full pipe. Returns 0, or -1 having thrown. */
static int dyn_exec_pump(JSContext *ctx, dyn_exec_t *e, pid_t pid,
                         dyn_pbuf_t *out, dyn_pbuf_t *err, int *timed_out)
{
    struct pollfd pf[3];
    size_t sent = 0;
    /* now + timeout saturates: 9e18 ms must mean "effectively never", not a
       signed overflow that lands the deadline in the PAST and SIGTERMs a
       healthy child immediately. */
    int64_t deadline = 0, now0 = dyn_now_ms();
    int64_t kill_at = 0;
    int killed = 0;

    if (e->timeout_ms > 0)
        deadline = e->timeout_ms > INT64_MAX - now0 ? INT64_MAX
                                                    : now0 + e->timeout_ms;

    while (e->out[0] >= 0 || e->err[0] >= 0 || e->in[1] >= 0) {
        int nf, i, wait_ms = -1, o_i, e_i, w_i;
        int64_t now = dyn_now_ms();

        nf = dyn_pump_fds(e, pf, &o_i, &e_i, &w_i);
        if (deadline && !killed) {
            int64_t left = deadline - now;
            wait_ms = left <= 0 ? 0
                    : (left > INT32_MAX ? INT32_MAX : (int)left);
        }
        else if (kill_at)
            wait_ms = (int)(kill_at - now > 0 ? kill_at - now : 0);
        if (poll(pf, (nfds_t)nf, wait_ms) < 0) {
            if (errno == EINTR)
                continue;
            JS_ThrowInternalError(ctx, "Exec: poll failed (%s)", strerror(errno));
            return -1;
        }
        if (dyn_pump_expire(pid, dyn_now_ms(), deadline, &killed, &kill_at,
                            timed_out))
            continue;
        for (i = 0; i < nf; i++) {
            if (i == w_i)
                dyn_pump_write(e, pf[i].revents, &sent);
            else if ((pf[i].revents & (POLLIN | POLLERR | POLLHUP))
                     && dyn_pump_read(ctx, e, pf[i].fd, i == o_i,
                                      i == o_i ? out : err, pid) < 0)
                return -1;
        }
        (void)e_i;
    }
    return 0;
}

/* An integer option with a floor. Absent leaves *out alone. */
static int dyn_opt_int(JSContext *ctx, JSValueConst o, const char *key,
                       int64_t floor_, const char *what, int64_t *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int64_t t = 0;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &t, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (t < floor_) {
        JS_ThrowRangeError(ctx, "Exec: %s", what);
        return -1;
    }
    *out = t;
    return 0;
}

static int dyn_exec_opt_cwd(JSContext *ctx, JSValueConst o, dyn_exec_t *e)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "cwd");
    const char *s;
    struct stat st;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (stat(s, &st) < 0 || !S_ISDIR(st.st_mode)) {
        JS_ThrowInternalError(ctx, "Exec: cwd is not a directory: %s", s);
        JS_FreeCString(ctx, s);
        return -1;
    }
    e->cwd = strdup(s);
    JS_FreeCString(ctx, s);
    if (!e->cwd) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int dyn_exec_opt_input(JSContext *ctx, JSValueConst o, dyn_exec_t *e,
                              JSValue *input_ref)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "input");

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_IsString(v)) {
        const char *s = JS_ToCStringLen(ctx, &e->inlen, v);
        if (!s) { JS_FreeValue(ctx, v); return -1; }
        e->input = (const uint8_t *)s;
    } else if (dyn_sys_bytes(ctx, v, &e->input, &e->inlen) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    *input_ref = v;                     /* keeps the bytes alive until we finish */
    return 0;
}

/* A uid/gid option: when present, a non-negative integer that fits uid_t.
   Absent leaves *out at its (uid_t)-1 sentinel. Returns 0, or -1 thrown. */
static int dyn_exec_opt_id(JSContext *ctx, JSValueConst o, const char *key,
                           int64_t *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int64_t t;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &t, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (t < 0 || t > (int64_t)UINT_MAX) {
        JS_ThrowRangeError(ctx, "Exec: %s must be a non-negative integer id",
                           key);
        return -1;
    }
    *out = t;
    return 0;
}

static int dyn_exec_opts(JSContext *ctx, JSValueConst o, dyn_exec_t *e,
                         JSValue *input_ref, int *want_bytes)
{
    int64_t t = 0, m = (int64_t)DYN_EXEC_MAXBUF;
    int64_t uid = -1, gid = -1;
    JSValue v;

    e->timeout_ms = 0;
    e->maxbuf = DYN_EXEC_MAXBUF;
    e->uid = (uid_t)-1;
    e->gid = (gid_t)-1;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "Exec(command, args, options): options must be an object");
        return -1;
    }
    if (dyn_exec_opt_cwd(ctx, o, e) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "env");
    if (JS_IsException(v))
        return -1;
    if (JS_IsObject(v)) {
        e->envp = dyn_envv(ctx, v);
        if (!e->envp) { JS_FreeValue(ctx, v); return -1; }
    }
    JS_FreeValue(ctx, v);
    if (dyn_opt_int(ctx, o, "timeoutMs", 0, "timeoutMs must not be negative", &t) < 0
        || dyn_opt_int(ctx, o, "maxBuffer", 1, "maxBuffer must be positive", &m) < 0
        || dyn_exec_opt_id(ctx, o, "uid", &uid) < 0
        || dyn_exec_opt_id(ctx, o, "gid", &gid) < 0)
        return -1;
    e->timeout_ms = t;
    e->maxbuf = (size_t)m;
    e->uid = (uid_t)uid;
    e->gid = (gid_t)gid;
    v = JS_GetPropertyStr(ctx, o, "encoding");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (!s) { JS_FreeValue(ctx, v); return -1; }
        *want_bytes = strcmp(s, "bytes") == 0;
        if (!*want_bytes && strcmp(s, "utf8") != 0) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "Exec: encoding must be \"utf8\" or \"bytes\"");
            return -1;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return dyn_exec_opt_input(ctx, o, e, input_ref);
}

static JSValue dyn_exec_result(JSContext *ctx, int status, int timed_out,
                               dyn_pbuf_t *out, dyn_pbuf_t *err, int want_bytes)
{
    JSValue r = JS_NewObject(ctx), so, se;
    int code = -1;
    const char *sig = NULL;

    if (JS_IsException(r))
        return r;
    if (WIFEXITED(status))
        code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        sig = dyn_signal_name(WTERMSIG(status));
    if (want_bytes) {
        so = dyn_sys_bytes_new(ctx, out->p, out->n);
        se = dyn_sys_bytes_new(ctx, err->p, err->n);
    } else {
        so = JS_NewStringLen(ctx, (const char *)out->p, out->n);
        se = JS_NewStringLen(ctx, (const char *)err->p, err->n);
    }
    if (JS_IsException(so) || JS_IsException(se)) {
        JS_FreeValue(ctx, so);
        JS_FreeValue(ctx, se);
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    /* A signalled child and a nonzero exit mean different things, so `code` is
       null when a signal killed it rather than a plausible small integer. */
    JS_SetPropertyStr(ctx, r, "code", sig ? JS_NULL : JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, r, "signal", sig ? JS_NewString(ctx, sig) : JS_NULL);
    JS_SetPropertyStr(ctx, r, "stdout", so);
    JS_SetPropertyStr(ctx, r, "stderr", se);
    JS_SetPropertyStr(ctx, r, "timedOut", JS_NewBool(ctx, timed_out));
    return r;
}

/* SIGPIPE off for the duration, and its old disposition saved. The child
   restores SIG_DFL before exec, because an ignored SIGPIPE is inherited. */
static int dyn_pipe_ignore(struct sigaction *old)
{
    struct sigaction ign;

    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    return sigaction(SIGPIPE, &ign, old) == 0;
}

static void dyn_exec_cleanup(JSContext *ctx, dyn_exec_t *e, JSValue input_ref,
                             dyn_pbuf_t *out, dyn_pbuf_t *err)
{
    int k;

    for (k = 0; k < 2; k++) {
        if (e->in[k] >= 0)  close(e->in[k]);
        if (e->out[k] >= 0) close(e->out[k]);
        if (e->err[k] >= 0) close(e->err[k]);
    }
    if (JS_IsString(input_ref) && e->input)
        JS_FreeCString(ctx, (const char *)e->input);
    JS_FreeValue(ctx, input_ref);
    dyn_strv_free(e->argv);
    dyn_strv_free(e->envp);
    free(e->cwd);
    dyn_pbuf_free(out);
    dyn_pbuf_free(err);
}

/* Everything that must happen before the fork: argv, options, the program
   path, and the pipes. Only async-signal-safe calls are legal after it. */
static int dyn_exec_setup(JSContext *ctx, int argc, JSValueConst *argv,
                          dyn_exec_t *e, JSValue *input_ref, int *want_bytes)
{
    const char *cmd, *path = NULL;
    size_t i;

    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "Exec(command, args, options): command must be a string");
        return -1;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
        && JS_IsArray(ctx, argv[1]) != 1) {
        JS_ThrowTypeError(ctx,
            "Exec(command, args): args must be an array -- there is no shell, "
            "so a command line is not a string here");
        return -1;
    }
    cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd)
        return -1;
    e->argv = dyn_strv(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, cmd);
    JS_FreeCString(ctx, cmd);
    if (!e->argv)
        return -1;
    if (dyn_exec_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, e, input_ref,
                      want_bytes) < 0)
        return -1;
    /* execve does not search, so the PARENT resolves -- against the child's
       PATH when the environment is being replaced. */
    for (i = 0; e->envp && e->envp[i]; i++)
        if (strncmp(e->envp[i], "PATH=", 5) == 0) { path = e->envp[i] + 5; break; }
    if (!path && !e->envp)
        path = getenv("PATH");
    if (dyn_path_lookup(e->argv[0], path, e->path, sizeof e->path) < 0) {
        JS_ThrowInternalError(ctx, "Exec: command not found: %s", e->argv[0]);
        return -1;
    }
    if ((e->input && pipe(e->in) < 0) || pipe(e->out) < 0 || pipe(e->err) < 0) {
        JS_ThrowInternalError(ctx, "Exec: pipe failed (%s)", strerror(errno));
        return -1;
    }
    /* Close-on-exec on every pipe end: after the fork the child dup2()s the
       write ends onto 1/2 (which clears FD_CLOEXEC on the targets), and the
       read ends stay on the parent. Marking them CLOEXEC stops them leaking
       from the child into a grandchild. */
    if (e->input) { fcntl(e->in[0],  F_SETFD, FD_CLOEXEC); fcntl(e->in[1],  F_SETFD, FD_CLOEXEC); }
    fcntl(e->out[0], F_SETFD, FD_CLOEXEC); fcntl(e->out[1], F_SETFD, FD_CLOEXEC);
    fcntl(e->err[0], F_SETFD, FD_CLOEXEC); fcntl(e->err[1], F_SETFD, FD_CLOEXEC);
    {
        long mf = sysconf(_SC_OPEN_MAX);
        if (mf < 3 || mf > 65536) mf = 4096;
        e->maxfd = mf;
    }
    return 0;
}

static JSValue dyn_exec(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    dyn_exec_t e;
    dyn_pbuf_t out, err;
    JSValue result = JS_EXCEPTION, input_ref = JS_UNDEFINED;
    struct sigaction old_pipe;
    pid_t pid;
    int status = 0, timed_out = 0, want_bytes = 0, pipe_saved = 0, drained;

    (void)this_val;
    memset(&e, 0, sizeof e);
    memset(&out, 0, sizeof out);
    memset(&err, 0, sizeof err);
    e.in[0] = e.in[1] = e.out[0] = e.out[1] = e.err[0] = e.err[1] = -1;
    if (dyn_exec_setup(ctx, argc, argv, &e, &input_ref, &want_bytes) < 0)
        goto done;
    if (e.input)
        pipe_saved = dyn_pipe_ignore(&old_pipe);
    pid = fork();
    if (pid < 0) {
        JS_ThrowInternalError(ctx, "Exec: fork failed (%s)", strerror(errno));
        goto done;
    }
    if (pid == 0) {
        if (e.in[1] >= 0) close(e.in[1]);
        close(e.out[0]);
        close(e.err[0]);
        dyn_exec_child(&e);
        _exit(127);
    }
    if (e.in[0] >= 0) { close(e.in[0]); e.in[0] = -1; }
    close(e.out[1]); e.out[1] = -1;
    close(e.err[1]); e.err[1] = -1;
    /* Reap in BOTH cases -- a child left unreaped is a zombie whether or not
       the drain succeeded -- but build the result only from a real status. */
    drained = dyn_exec_pump(ctx, &e, pid, &out, &err, &timed_out);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (drained == 0)
        result = dyn_exec_result(ctx, status, timed_out, &out, &err, want_bytes);
done:
    if (pipe_saved)
        sigaction(SIGPIPE, &old_pipe, NULL);
    dyn_exec_cleanup(ctx, &e, input_ref, &out, &err);
    return result;
}

/* Which(name) -- the PATH walk, with no shell anywhere in it. */
static int dyn_is_exec(const char *p)
{
    struct stat st;
    /* X_OK answers 0 for ROOT on almost any regular file (the kernel grants a
       privileged caller VEXEC without mode bits), so a PATH walk that trusted
       it handed root which(1)-style wrong answers. The x-bits are checked
       directly; access() still covers the non-root permission case. */
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) &&
           (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 &&
           access(p, X_OK) == 0;
}

static JSValue dyn_which(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const char *name;
    char buf[PATH_MAX];
    JSValue r = JS_NULL;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Which(name): name must be a string");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (name[0] && dyn_path_lookup(name, getenv("PATH"), buf, sizeof buf) == 0)
        r = JS_NewString(ctx, buf);
    JS_FreeCString(ctx, name);
    return r;
}
