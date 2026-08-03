/*
 * dyna:file -- filesystem access and buffered IO.
 *
 * All disk IO goes through the dyna-aio adapter (io_uring on Linux, kqueue +
 * thread pool elsewhere); it must never add a copy or a syscall the raw OS API
 * would not. Handles are value handles: constructed with a path, closed
 * explicitly or by the GC finalizer, and ASan-clean on both paths.
 * Full API: docs/dynajs-guide/API.md.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>

#include "core/dyn-path.h"

/* Scratch for the path composers. The normaliser needs a working buffer the
 * same size as its output, and that buffer dies at the end of the call -- so
 * for any path that fits here it comes off the stack and the call makes one
 * malloc instead of two. 1 KiB covers essentially every real path (PATH_MAX is
 * 1024 on Darwin, 4096 on Linux); longer ones fall back to the heap. */
#define DYN_PATH_STACK_SCRATCH 1024


#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ==================================================================== *
 *  Path -- the value handle every filesystem entry point takes          *
 *                                                                       *
 *  A Path is constructed WITH ITS DATA (`new Path("/var/log", name)`),   *
 *  which makes it a value handle rather than a compiled capability: it   *
 *  is not configuration reused across unbounded inputs, it IS the input. *
 *  The distinction matters because it sets what the object is allowed to *
 *  cost -- a handle must be as cheap as the string it replaces, not      *
 *  merely cheaper after N uses.                                          *
 *                                                                       *
 *  What it caches, and why each item earns its bytes:                    *
 *    - the NORMALISED bytes, NUL-terminated, so every syscall below can  *
 *      borrow them directly. This is the whole performance argument: the *
 *      string form had to be coerced, copied and normalised at every     *
 *      call site, and now it is done once.                               *
 *    - the three lexical split offsets, so `.dirname`, `.basename` and   *
 *      `.extname` are slices rather than scans.                          *
 *    - `isAbsolute`, which is one byte of the answer but is consulted by *
 *      resolve() on every composition.                                   *
 *                                                                       *
 *  IT IS A PLAIN GC CLASS, NOT A RESOURCE -- deliberately. A resource    *
 *  has close(), and an object with close() brings the close-during-      *
 *  coercion hazard of CLAUDE.md sec.8 with it: user JS running inside an *
 *  argument coercion could free the buffer a syscall is about to read.   *
 *  A Path cannot be closed, so every entry point below can BORROW its    *
 *  bytes with no copy and no lifetime analysis. Making the handle        *
 *  immutable is what makes borrowing safe, and borrowing is what makes   *
 *  the handle free.                                                      *
 *                                                                       *
 *  The record is refcounted so `new Path(existingPath)` and `.dirname`   *
 *  chains share one buffer instead of copying it per derivation.         *
 * ==================================================================== */

typedef struct {
    char *buf;                 /* normalised, NUL-terminated */
    size_t len;
    dyn_path_split_t split;
    int refs;
} dyn_path_rec_t;

static JSClassID dyn_path_class_id;

static void dyn_path_rec_unref(dyn_path_rec_t *p)
{
    if (!p)
        return;
    if (--p->refs > 0)
        return;
    free(p->buf);
    free(p);
}

static void dyn_path_dispose(void *native)
{
    dyn_path_rec_unref((dyn_path_rec_t *)native);
}

static void dyn_path_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_path_rec_unref((dyn_path_rec_t *)JS_GetOpaque(val, dyn_path_class_id));
}

static const JSClassDef dyn_path_class = {
    "Path", .finalizer = dyn_path_finalizer,
};

/* Build a record from already-normalised bytes. Takes ownership of `buf`. */
static dyn_path_rec_t *dyn_path_rec_adopt(char *buf, size_t len)
{
    dyn_path_rec_t *p = (dyn_path_rec_t *)calloc(1, sizeof(*p));
    if (!p) {
        free(buf);
        return NULL;
    }
    p->buf = buf;
    p->len = len;
    p->refs = 1;
    dyn_path_split(buf, len, &p->split);
    return p;
}

/* Normalise `src` into a fresh record. */
static dyn_path_rec_t *dyn_path_rec_from(const char *src, size_t n)
{
    char *buf = (char *)malloc(dyn_path_cstr_cap(dyn_path_normalize_cap(n)));
    size_t len;
    if (!buf)
        return NULL;
    len = dyn_path_normalize(src, n, buf);
    buf[len] = '\0';
    return dyn_path_rec_adopt(buf, len);
}

static JSValue dyn_path_wrap_rec(JSContext *ctx, dyn_path_rec_t *p)
{
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, dyn_path_class_id, p, dyn_path_dispose);
}

/* A second JS object over the SAME record: one refcount bump, no copy. */
static JSValue dyn_path_share(JSContext *ctx, dyn_path_rec_t *p)
{
    p->refs++;
    return dyn_path_wrap_rec(ctx, p);
}

static dyn_path_rec_t *dyn_path_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_path_rec_t *)JS_GetOpaque2(ctx, v, dyn_path_class_id);
}

static int dyn_is_path(JSValueConst v)
{
    return JS_GetOpaque(v, dyn_path_class_id) != NULL;
}

/* ---- THE ARGUMENT CONVENTION FOR EVERY FILESYSTEM ENTRY POINT ----------
 *
 * Returns the Path's own NUL-terminated bytes, BORROWED -- no copy, no
 * allocation, nothing to free. Throws TypeError for anything that is not a
 * Path, including a string: `dyna:file` has one path surface, and accepting
 * both would be the redundancy this redesign exists to remove.
 *
 * The error message names the fix rather than the rule, because the first
 * thing a caller hitting this needs is the two words that make it work.
 */
static const char *dyn_path_arg(JSContext *ctx, JSValueConst v, const char *what)
{
    dyn_path_rec_t *p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "dyna:file: %s must be a Path -- wrap it with "
                               "new Path(...)", what);
        return NULL;
    }
    p = (dyn_path_rec_t *)JS_GetOpaque(v, dyn_path_class_id);
    return p->buf;
}

/* Same, when the callee also needs the byte length. */
static const char *dyn_path_arg_len(JSContext *ctx, JSValueConst v,
                                    const char *what, size_t *plen)
{
    dyn_path_rec_t *p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "dyna:file: %s must be a Path -- wrap it with "
                               "new Path(...)", what);
        return NULL;
    }
    p = (dyn_path_rec_t *)JS_GetOpaque(v, dyn_path_class_id);
    *plen = p->len;
    return p->buf;
}

/* ---------------------------------------------------------------------------
 * STRICT openat() RESOLUTION
 *
 * Walk the path one component at a time from a held directory fd, with
 * O_NOFOLLOW on every step. A symlink ANYWHERE in the path is refused with
 * ELOOP.
 *
 * Why not realpath() plus a containment check: that is racy by construction.
 * The path is resolved, checked, and then opened AGAIN by name, so anything
 * that swaps a component for a symlink between the check and the open wins
 * (TOCTOU). Resolving against a held fd closes the window -- the fd names the
 * inode, not the string.
 *
 * COMPATIBILITY, measured, not assumed: on macOS /var IS a symlink to
 * /private/var, so makeTempDir() returns a path this refuses. That is the
 * intended strictness, not a bug -- pass the realpath (dyn_fs_realpath) once at
 * the boundary if you need to reach through a system symlink, then every
 * operation from there is strict.
 * ------------------------------------------------------------------------ */

/* Open the parent directory of `path`, strictly. On success *leaf points at
   the final component inside `path`. Returns a dirfd the caller must close, or
   AT_FDCWD (which it must NOT close), or -1 with errno set. */
static int dyn_openat_parent(const char *path, const char **leaf)
{
    int dirfd, next;
    const char *p = path, *slash;

    if (*p == '/') {
        dirfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd < 0)
            return -1;
        while (*p == '/')
            p++;
    } else {
        dirfd = AT_FDCWD;
    }
    for (;;) {
        while (*p == '/')
            p++;
        slash = strchr(p, '/');
        if (!slash) {                    /* p is the final component */
            *leaf = p;
            return dirfd;
        }
        {
            size_t len = (size_t)(slash - p);
            char comp[PATH_MAX];
            if (len == 0 || len >= sizeof(comp)) {
                if (dirfd != AT_FDCWD) close(dirfd);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(comp, p, len);
            comp[len] = '\0';
            /* "." and ".." are directory entries, never symlinks, so
               O_NOFOLLOW is meaningless on them and would not fire anyway. */
            next = openat(dirfd, comp,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                          ((comp[0] == '.' && (comp[1] == '\0' ||
                            (comp[1] == '.' && comp[2] == '\0')))
                           ? 0 : O_NOFOLLOW));
            if (dirfd != AT_FDCWD)
                close(dirfd);
            if (next < 0)
                return -1;               /* ELOOP here means a symlink component */
            dirfd = next;
        }
        p = slash + 1;
    }
}

/* open(2) with strict, symlink-refusing resolution. Same return contract. */
static int dyn_open_strict(const char *path, int flags, mode_t mode)
{
    const char *leaf = NULL;
    int dirfd, fd, saved;

    dirfd = dyn_openat_parent(path, &leaf);
    if (dirfd < 0)
        return -1;
    if (!*leaf) {                        /* trailing slash: the dir itself */
        if (dirfd == AT_FDCWD)
            return open(".", flags | O_NOFOLLOW, mode);
        return dirfd;                    /* caller owns it */
    }
    fd = openat(dirfd, leaf, flags | O_NOFOLLOW, mode);
    saved = errno;
    if (dirfd != AT_FDCWD)
        close(dirfd);
    errno = saved;
    return fd;
}

/* stat(2)/lstat(2) with the same strictness. `follow` only decides whether the
   FINAL component may be a symlink; intermediate ones never may. */
static int dyn_stat_strict(const char *path, struct stat *st, int follow)
{
    const char *leaf = NULL;
    int dirfd, r, saved;

    dirfd = dyn_openat_parent(path, &leaf);
    if (dirfd < 0)
        return -1;
    if (!*leaf)
        r = fstatat(dirfd, ".", st, 0);
    else
        r = fstatat(dirfd, leaf, st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    saved = errno;
    if (dirfd != AT_FDCWD)
        close(dirfd);
    errno = saved;
    return r;
}

/* The cross-module entry points declared in dyna-nat.h. Same borrow, exported
 * so csv/uring/http/ml resolve a Path through the ONE class id rather than
 * each minting its own -- which would make a Path built by one module not a
 * Path to another. */
const char *dyn_path_borrow(JSContext *ctx, JSValueConst v, const char *what,
                            size_t *plen)
{
    dyn_path_rec_t *p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "%s must be a Path -- wrap it with "
                               "new Path(...) from dyna:file", what);
        return NULL;
    }
    p = (dyn_path_rec_t *)JS_GetOpaque(v, dyn_path_class_id);
    if (plen)
        *plen = p->len;
    return p->buf;
}

int dyn_value_is_path(JSValueConst v)
{
    return dyn_is_path(v);
}

/* Every entry point that RETURNS a path returns a Path, not a string. The
 * alternative -- returning a string that the caller must immediately re-wrap --
 * is precisely the two-surface redundancy the Path-only decision removes, and
 * it would put the re-wrap in every caller instead of once here. */
/* Resolve the longest EXISTING prefix once, here, and keep the rest literal.
 *
 * Every operation on a Path is strict: it walks with O_NOFOLLOW and refuses a
 * symlink component. Without this one resolve, `new Path("/tmp/out")` is
 * unusable on macOS -- /tmp and /var are themselves symlinks -- and the API
 * docs' own examples stop running, which is how this was found.
 *
 * Resolving here rather than per operation is what keeps the strictness
 * meaningful: a symlink swapped in AFTER construction is still refused, which
 * is the TOCTOU window a realpath-then-open-by-name check leaves open. The
 * trailing component is deliberately NOT resolved -- it may not exist yet
 * (writeFile, makeDir), and a final symlink is exactly what strict mode
 * refuses. */
/* Resolve the longest EXISTING prefix of `s` into `dst` (capacity PATH_MAX).
 * Returns the new length, or 0 to mean "use the original unchanged".
 *
 * Every operation on a Path is strict: it walks with O_NOFOLLOW and refuses a
 * symlink component. Without this ONE resolve, `new Path("/tmp/out")` is
 * unusable on macOS -- /tmp and /var are themselves symlinks -- and the API
 * docs' own examples stop running, which is how this was found.
 *
 * Resolving here rather than per operation is what keeps the strictness
 * meaningful: a symlink swapped in AFTER construction is still refused, which
 * is the TOCTOU window a realpath-then-open-by-name check leaves open. The
 * trailing component is deliberately NOT resolved -- it may not exist yet
 * (writeFile, makeDir), and a final symlink is exactly what strict mode
 * refuses. */
static size_t dyn_path_resolve_prefix(const char *s, size_t n, char *dst)
{
    char buf[PATH_MAX], real[PATH_MAX];
    const char *slash;
    size_t plen, rl, tl;

    /* ABSOLUTE only. Resolving a relative path would make it absolute and
       change the Path's identity -- Path("") normalises to "." and became the
       cwd. Symlink resolution is the goal; rebasing is not. */
    if (!n || n >= PATH_MAX || s[0] != '/')
        return 0;
    memcpy(buf, s, n);
    buf[n] = '\0';
    slash = strrchr(buf, '/');
    if (slash && slash != buf) {
        plen = (size_t)(slash - buf);
        buf[plen] = '\0';
        if (!realpath(buf, real))
            return 0;
        rl = strlen(real);
        tl = n - plen;                       /* includes the leading '/' */
        if (rl + tl >= PATH_MAX)
            return 0;
        memcpy(dst, real, rl);
        memcpy(dst + rl, s + plen, tl);
        dst[rl + tl] = '\0';
        return rl + tl;
    }
    if (realpath(buf, real)) {               /* "/tmp" itself, or relative */
        rl = strlen(real);
        if (rl >= PATH_MAX)
            return 0;
        memcpy(dst, real, rl);
        dst[rl] = '\0';
        return rl;
    }
    return 0;
}

static JSValue dyn_path_new_from(JSContext *ctx, const char *s, size_t n)
{
    char res[PATH_MAX];
    size_t rn = dyn_path_resolve_prefix(s, n, res);
    if (rn)
        return dyn_path_wrap_rec(ctx, dyn_path_rec_from(res, rn));
    return dyn_path_wrap_rec(ctx, dyn_path_rec_from(s, n));

}

/* The counterpart of dyn_path_arg. It does nothing, and it exists anyway: the
 * call sites below used to pair every coercion with a JS_FreeCString, and
 * deleting that half would leave a reader unable to tell a borrow that needs no
 * release from a coercion whose release was FORGOTTEN. Keeping the pairing
 * visible is what makes the second kind of bug obvious on sight. It compiles to
 * nothing. */
static inline void dyn_path_unborrow(JSContext *ctx, const char *p)
{
    (void)ctx;
    (void)p;
}

/* Collect the constructor/join/resolve argument list into (ptr,len) pairs.
 * A segment may be a Path (borrow its bytes) or a string (coerce it) -- inside
 * a Path constructor a string is the raw material, not a second path surface.
 * Every segment is materialised BEFORE anything is built, per CLAUDE.md sec.8:
 * reading argv can run a getter on an exotic argument, and a half-built record
 * must never be observable. */
typedef struct {
    const char **ptr;
    size_t *len;
    const char **owned;   /* JS_ToCStringLen results needing release */
    int n, n_owned;
} dyn_seg_list_t;

static void dyn_segs_free(JSContext *ctx, dyn_seg_list_t *s)
{
    int i;
    for (i = 0; i < s->n_owned; i++)
        JS_FreeCString(ctx, s->owned[i]);
    free(s->ptr);
    free(s->len);
    free(s->owned);
    s->ptr = NULL;
    s->len = NULL;
    s->owned = NULL;
}

static int dyn_segs_collect(JSContext *ctx, int argc, JSValueConst *argv,
                            dyn_seg_list_t *s)
{
    int i;
    memset(s, 0, sizeof(*s));
    if (argc <= 0)
        return 0;
    s->ptr = (const char **)calloc((size_t)argc, sizeof(*s->ptr));
    s->len = (size_t *)calloc((size_t)argc, sizeof(*s->len));
    s->owned = (const char **)calloc((size_t)argc, sizeof(*s->owned));
    if (!s->ptr || !s->len || !s->owned) {
        dyn_segs_free(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < argc; i++) {
        if (dyn_is_path(argv[i])) {
            dyn_path_rec_t *p =
                (dyn_path_rec_t *)JS_GetOpaque(argv[i], dyn_path_class_id);
            s->ptr[i] = p->buf;
            s->len[i] = p->len;
        } else if (JS_IsString(argv[i])) {
            size_t sl;
            const char *cs = JS_ToCStringLen(ctx, &sl, argv[i]);
            if (!cs) {
                dyn_segs_free(ctx, s);
                return -1;
            }
            /* Every syscall stops at the first NUL, so "secret.key\0.png"
               opens secret.key while any suffix check the caller wrote sees
               ".png". Refuse at construction: this is the ONE place the bytes
               enter a Path, so it covers every consumer that borrows one. */
            if (strlen(cs) != sl) {
                s->owned[s->n_owned++] = cs;
                dyn_segs_free(ctx, s);
                JS_ThrowTypeError(ctx, "new Path(...): a segment must not "
                                       "contain a NUL byte");
                return -1;
            }
            s->owned[s->n_owned++] = cs;
            s->ptr[i] = cs;
            s->len[i] = sl;
        } else {
            dyn_segs_free(ctx, s);
            JS_ThrowTypeError(ctx, "new Path(...): every segment must be a "
                                   "string or a Path");
            return -1;
        }
        s->n = i + 1;
    }
    return 0;
}

static JSValue dyn_path_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv)
{
    dyn_seg_list_t segs;
    dyn_path_rec_t *rec;
    char *out, *scratch;
    char stack_scratch[DYN_PATH_STACK_SCRATCH];
    size_t sum = 0, cap, len;
    int i;

    (void)new_target;

    if (argc == 0)
        return JS_ThrowTypeError(ctx, "new Path(...segments) needs at least "
                                      "one segment");

    /* `new Path(p)` on an existing Path shares the record rather than
     * re-normalising it: normalisation is idempotent, so the work would be
     * provably wasted, and the copy would be too. */
    if (argc == 1 && dyn_is_path(argv[0])) {
        dyn_path_rec_t *p =
            (dyn_path_rec_t *)JS_GetOpaque(argv[0], dyn_path_class_id);
        return dyn_path_share(ctx, p);
    }

    if (dyn_segs_collect(ctx, argc, argv, &segs) < 0)
        return JS_EXCEPTION;

    for (i = 0; i < segs.n; i++)
        sum += segs.len[i];
    cap = dyn_path_join_cap(sum, (size_t)segs.n);

    out = (char *)malloc(dyn_path_cstr_cap(cap));
    scratch = (cap <= DYN_PATH_STACK_SCRATCH) ? stack_scratch
                                              : (char *)malloc(cap);
    if (!out || !scratch) {
        free(out);
        if (scratch != stack_scratch)
            free(scratch);
        dyn_segs_free(ctx, &segs);
        return JS_ThrowOutOfMemory(ctx);
    }
    len = dyn_path_join(segs.ptr, segs.len, (size_t)segs.n, out, scratch);
    out[len] = '\0';
    if (scratch != stack_scratch)
        free(scratch);
    dyn_segs_free(ctx, &segs);

    {   /* same one-time resolve as dyn_path_new_from: new Path(...) is the
           path users actually take, and it does not go through it. */
        char res[PATH_MAX];
        size_t rn = dyn_path_resolve_prefix(out, len, res);
        if (rn) {
            char *rp = (char *)malloc(dyn_path_cstr_cap(rn));
            if (rp) {
                memcpy(rp, res, rn);
                rp[rn] = '\0';
                free(out);
                out = rp;
                len = rn;
            }
        }
    }
    rec = dyn_path_rec_adopt(out, len);
    return dyn_path_wrap_rec(ctx, rec);
}

/* ---- getters: every one of these is a slice of the cached buffer ------- */

static JSValue dyn_path_get_str(JSContext *ctx, JSValueConst this_val)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf, p->len);
}

static JSValue dyn_path_get_dirname(JSContext *ctx, JSValueConst this_val)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    dyn_path_rec_t *d;
    char *buf;
    const char *src;
    size_t n;

    if (!p)
        return JS_EXCEPTION;
    if (p->split.dir_is_dot) { src = "."; n = 1; }
    else if (p->split.dir_is_root) { src = "/"; n = 1; }
    else { src = p->buf + p->split.dir_off; n = p->split.dir_len; }

    /* Already normalised by construction, so this copies rather than
     * re-running the normaliser -- the offsets ARE the answer. */
    buf = (char *)malloc(n + 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(buf, src, n);
    buf[n] = '\0';
    d = dyn_path_rec_adopt(buf, n);
    return dyn_path_wrap_rec(ctx, d);
}

static JSValue dyn_path_get_basename(JSContext *ctx, JSValueConst this_val)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.base_off, p->split.base_len);
}

static JSValue dyn_path_get_extname(JSContext *ctx, JSValueConst this_val)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.ext_off, p->split.ext_len);
}

static JSValue dyn_path_get_is_absolute(JSContext *ctx, JSValueConst this_val)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->split.is_absolute);
}

/* ---- composition ------------------------------------------------------- */

/* join / resolve share everything but which core function runs, so they share
 * a body and differ by magic -- CLAUDE.md sec.7: magic dispatch is free. */
static JSValue dyn_path_compose(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic)
{
    dyn_path_rec_t *self = dyn_path_of(ctx, this_val);
    dyn_seg_list_t segs;
    const char **ptr = NULL;
    size_t *len = NULL;
    char *out = NULL, *scratch = NULL;
    char stack_scratch[DYN_PATH_STACK_SCRATCH];
    size_t sum, cap, outlen;
    int i, n;
    JSValue res;

    if (!self)
        return JS_EXCEPTION;
    if (dyn_segs_collect(ctx, argc, argv, &segs) < 0)
        return JS_EXCEPTION;

    /* `this` leads, then the arguments. */
    n = segs.n + 1;
    ptr = (const char **)calloc((size_t)n, sizeof(*ptr));
    len = (size_t *)calloc((size_t)n, sizeof(*len));
    if (!ptr || !len) {
        res = JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ptr[0] = self->buf;
    len[0] = self->len;
    sum = self->len;
    for (i = 0; i < segs.n; i++) {
        ptr[i + 1] = segs.ptr[i];
        len[i + 1] = segs.len[i];
        sum += segs.len[i];
    }

    cap = magic ? dyn_path_resolve_cap(sum, (size_t)n)
                : dyn_path_join_cap(sum, (size_t)n);
    out = (char *)malloc(dyn_path_cstr_cap(cap));
    scratch = (cap <= DYN_PATH_STACK_SCRATCH) ? stack_scratch
                                              : (char *)malloc(cap);
    if (!out || !scratch) {
        res = JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    outlen = magic ? dyn_path_resolve(ptr, len, (size_t)n, out, scratch)
                   : dyn_path_join(ptr, len, (size_t)n, out, scratch);
    out[outlen] = '\0';
    res = dyn_path_wrap_rec(ctx, dyn_path_rec_adopt(out, outlen));
    out = NULL; /* adopted */

done:
    free(out);
    if (scratch != stack_scratch)
        free(scratch);
    free(ptr);
    free(len);
    dyn_segs_free(ctx, &segs);
    return res;
}

static JSValue dyn_path_relative_to(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_path_rec_t *self = dyn_path_of(ctx, this_val);
    dyn_path_rec_t *to;
    char *out, *scratch;
    size_t cap, len;
    JSValue res;

    if (!self)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_is_path(argv[0]))
        return JS_ThrowTypeError(ctx, "Path.relativeTo(other): other must be "
                                      "a Path");
    to = (dyn_path_rec_t *)JS_GetOpaque(argv[0], dyn_path_class_id);

    cap = dyn_path_relative_cap(self->len, to->len);
    out = (char *)malloc(dyn_path_cstr_cap(cap));
    scratch = (char *)malloc(cap);
    if (!out || !scratch) {
        free(out);
        free(scratch);
        return JS_ThrowOutOfMemory(ctx);
    }
    len = dyn_path_relative(self->buf, self->len, to->buf, to->len, out,
                            scratch);
    free(scratch);
    /* relative() can produce "", which normalises to "." -- and "." is the
     * right Path for "the same place", so it goes through the normaliser
     * rather than being special-cased into an empty buffer. */
    if (len == 0) {
        free(out);
        return dyn_path_wrap_rec(ctx, dyn_path_rec_from(".", 1));
    }
    out[len] = '\0';
    res = dyn_path_wrap_rec(ctx, dyn_path_rec_adopt(out, len));
    return res;
}

static JSValue dyn_path_equals(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    dyn_path_rec_t *self = dyn_path_of(ctx, this_val);
    dyn_path_rec_t *o;
    if (!self)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_is_path(argv[0]))
        return JS_NewBool(ctx, 0);
    o = (dyn_path_rec_t *)JS_GetOpaque(argv[0], dyn_path_class_id);
    /* Both sides are normalised by construction, so byte equality IS path
     * equality -- no re-normalisation, and "a//b" equals "a/b" for free. */
    return JS_NewBool(ctx, self->len == o->len &&
                               memcmp(self->buf, o->buf, self->len) == 0);
}

static JSValue dyn_path_basename_without(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    dyn_path_rec_t *p = dyn_path_of(ctx, this_val);
    const char *suf = NULL;
    size_t suflen = 0, off, len;
    JSValue res;

    if (!p)
        return JS_EXCEPTION;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        suf = JS_ToCStringLen(ctx, &suflen, argv[0]);
        if (!suf)
            return JS_EXCEPTION;
    }
    dyn_path_basename(p->buf, p->len, suf, suflen, &off, &len);
    res = JS_NewStringLen(ctx, p->buf + off, len);
    if (suf)
        JS_FreeCString(ctx, suf);
    return res;
}

static JSValue dyn_path_to_string(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return dyn_path_get_str(ctx, this_val);
}

/* ---- statics ----------------------------------------------------------- */

/* A trailing separator is MEANINGFUL to the normaliser -- Node preserves it and
 * so does dyn_path_normalize -- but it carries no information on a path that
 * names a directory, and macOS sets TMPDIR with one while the free tempDir()
 * strips it. Two spellings of the same directory that stringify differently is
 * the kind of divergence this programme keeps finding, so the OS-derived
 * statics strip it and agree. Root stays "/", which is not a trailing
 * separator but the whole path. */
static JSValue dyn_path_dir_from(JSContext *ctx, const char *v)
{
    size_t n = strlen(v);
    while (n > 1 && v[n - 1] == DYN_PATH_SEP)
        n--;
    /* Through the same one-time resolve as the constructor: Path.temp() and
       tempDir() must agree, and TMPDIR is under the /var symlink on macOS. */
    return dyn_path_new_from(ctx, v, n);
}

static JSValue dyn_path_static_from_env(JSContext *ctx, const char *var,
                                        const char *fallback)
{
    const char *v = getenv(var);
    if (!v || !*v)
        v = fallback;
    return dyn_path_dir_from(ctx, v);
}

static JSValue dyn_path_static(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic)
{
    (void)this_val;
    switch (magic) {
    case 0: { /* cwd */
        char buf[PATH_MAX];
        if (!getcwd(buf, sizeof(buf)))
            return JS_ThrowInternalError(ctx, "Path.cwd(): %s", strerror(errno));
        return dyn_path_dir_from(ctx, buf);
    }
    case 1:
        return dyn_path_static_from_env(ctx, "HOME", "/");
    case 2:
        return dyn_path_static_from_env(ctx, "TMPDIR", "/tmp");
    default: /* isPath */
        return JS_NewBool(ctx, argc >= 1 && dyn_is_path(argv[0]));
    }
}

static const JSCFunctionListEntry dyn_path_proto[] = {
    JS_CGETSET_DEF("dirname", dyn_path_get_dirname, NULL),
    JS_CGETSET_DEF("basename", dyn_path_get_basename, NULL),
    JS_CGETSET_DEF("extname", dyn_path_get_extname, NULL),
    JS_CGETSET_DEF("isAbsolute", dyn_path_get_is_absolute, NULL),
    JS_CFUNC_MAGIC_DEF("join", 0, dyn_path_compose, 0),
    JS_CFUNC_MAGIC_DEF("resolve", 0, dyn_path_compose, 1),
    JS_CFUNC_DEF("relativeTo", 1, dyn_path_relative_to),
    JS_CFUNC_DEF("equals", 1, dyn_path_equals),
    JS_CFUNC_DEF("basenameWithout", 1, dyn_path_basename_without),
    JS_CFUNC_DEF("toString", 0, dyn_path_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_path_to_string),
    /* Explicit @@toPrimitive, per W3.1. Without it a Path still stringifies in
     * a string context -- ToPrimitive falls through valueOf to toString -- but
     * `+p` and other NUMBER hints would go the same route and produce NaN via
     * a string. Defining it makes the answer the normalised path for EVERY
     * hint, which is the only sensible primitive a path has. */
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, dyn_path_to_string),
};

#define DYN_FILE_DEFAULT_BUF (1u << 17) /* 128 KiB */
#define DYN_FILE_MIN_BUF     4096u
#define DYN_FILE_MAX_BUF     (1u << 26) /* 64 MiB cap on a caller-chosen size */

static unsigned dyn_file_clamp_bufsize(int64_t v)
{
    if (v <= 0)
        return DYN_FILE_DEFAULT_BUF;
    if (v < DYN_FILE_MIN_BUF)
        return DYN_FILE_MIN_BUF;
    if (v > DYN_FILE_MAX_BUF)
        return DYN_FILE_MAX_BUF;
    return (unsigned)v;
}

/* ==================================================================== *
 *  FileReader -- buffered sequential reader                             *
 * ==================================================================== */

static JSClassID dyn_freader_class_id;

typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t start; /* first unconsumed byte in buf */
    size_t end;   /* one past the last valid byte in buf */
    int fd;
    int eof;      /* underlying read returned 0 */
} dyn_freader_t;

_Static_assert(sizeof(dyn_freader_t) == 4 * sizeof(void *) + 8,
               "dyn_freader_t regained padding: keep the two ints adjacent");

static void dyn_freader_dispose(void *native)
{
    dyn_freader_t *r = (dyn_freader_t *)native;
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    free(r);
}

static const JSClassDef dyn_freader_class = {
    "FileReader",
    .finalizer = dyn_res_finalizer,
};

/* Refill buf from the fd when it is fully consumed. Returns bytes read, or -1. */
static ssize_t dyn_freader_fill(dyn_freader_t *r)
{
    ssize_t n;
    if (r->start < r->end)
        return (ssize_t)(r->end - r->start); /* still have buffered data */
    r->start = r->end = 0;
    if (r->eof)
        return 0;
    for (;;) {
        n = read(r->fd, r->buf, r->cap);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        break;
    }
    r->end = (size_t)n;
    if (n == 0)
        r->eof = 1;
    return n;
}

static JSValue dyn_freader_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_freader_t *r;
    const char *path;
    int64_t bufsize = 0;
    struct stat st;
    int fd;

    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileReader(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    fd = dyn_open_strict(path, O_RDONLY, 0);
    if (fd < 0) {
        dyn_path_unborrow(ctx, path);
        return JS_ThrowInternalError(ctx, "FileReader: cannot open file");
    }
    if (fstat(fd, &st) == 0)
        dyn_io_advise_seq_read(fd, st.st_size);
    dyn_path_unborrow(ctx, path);

    r = (dyn_freader_t *)calloc(1, sizeof(*r));
    if (!r) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->fd = fd;
    r->cap = dyn_file_clamp_bufsize(bufsize);
    r->buf = (unsigned char *)malloc(r->cap);
    if (!r->buf) {
        close(fd);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, dyn_freader_class_id, r, dyn_freader_dispose);
}

/* read([n]) -> up to n bytes as a string ("" at EOF); n omitted => read all. */
static JSValue dyn_freader_read(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_freader_t *r;
    int64_t want = -1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &want, argv[0])) /* coerce FIRST */
            return JS_EXCEPTION;
    }
    r = (dyn_freader_t *)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    {
        /* accumulate into a small growable buffer */
        char *acc = NULL;
        size_t acc_len = 0, acc_cap = 0;
        JSValue out;
        for (;;) {
            size_t avail, take;
            ssize_t f;
            if (want >= 0 && (int64_t)acc_len >= want)
                break;
            f = dyn_freader_fill(r);
            if (f < 0) {
                free(acc);
                return JS_ThrowInternalError(ctx, "FileReader: read error");
            }
            if (f == 0)
                break; /* EOF */
            avail = r->end - r->start;
            take = avail;
            if (want >= 0 && take > (size_t)(want - (int64_t)acc_len))
                take = (size_t)(want - (int64_t)acc_len);
            if (acc_len + take + 1 > acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8192;
                char *na;
                while (nc < acc_len + take + 1)
                    nc *= 2;
                na = (char *)realloc(acc, nc);
                if (!na) {
                    free(acc);
                    return JS_ThrowOutOfMemory(ctx);
                }
                acc = na;
                acc_cap = nc;
            }
            memcpy(acc + acc_len, r->buf + r->start, take);
            acc_len += take;
            r->start += take;
        }
        out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
        free(acc);
        return out;
    }
}

/* readLine() -> next line without the trailing newline, or null at EOF. */
static JSValue dyn_freader_read_line(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_freader_t *r;
    char *acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    int saw_any = 0;
    JSValue out;

    (void)argc; (void)argv;
    r = (dyn_freader_t *)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    for (;;) {
        ssize_t f = dyn_freader_fill(r);
        unsigned char *nl;
        size_t avail, take;
        if (f < 0) {
            free(acc);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break; /* EOF */
        saw_any = 1;
        avail = r->end - r->start;
        nl = (unsigned char *)memchr(r->buf + r->start, '\n', avail);
        take = nl ? (size_t)(nl - (r->buf + r->start)) : avail;
        if (acc_len + take + 1 > acc_cap) {
            size_t nc = acc_cap ? acc_cap * 2 : 256;
            char *na;
            while (nc < acc_len + take + 1)
                nc *= 2;
            na = (char *)realloc(acc, nc);
            if (!na) {
                free(acc);
                return JS_ThrowOutOfMemory(ctx);
            }
            acc = na;
            acc_cap = nc;
        }
        memcpy(acc + acc_len, r->buf + r->start, take);
        acc_len += take;
        r->start += take;
        if (nl) {
            r->start++; /* consume the '\n' */
            /* strip a trailing '\r' for CRLF files */
            if (acc_len > 0 && acc[acc_len - 1] == '\r')
                acc_len--;
            out = JS_NewStringLen(ctx, acc, acc_len);
            free(acc);
            return out;
        }
    }
    if (!saw_any && acc_len == 0) {
        free(acc);
        return JS_NULL; /* clean EOF with nothing buffered */
    }
    out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
    free(acc);
    return out;
}

static const JSCFunctionListEntry dyn_freader_proto[] = {
    JS_CFUNC_DEF("read", 0, dyn_freader_read),
    JS_CFUNC_DEF("readLine", 0, dyn_freader_read_line),
    JS_CFUNC_DEF("readAll", 0, dyn_freader_read),
};

#if defined(CONFIG_NATIVE_MODULE_NET)
/* The offload machinery is defined with the async content I/O further down;
 * FileWriter.syncAsync shares it so there is ONE reap hook and one pair of
 * counters, not two that could disagree about which arm ran. */
#include "dyna-aio.h"
#include "dyna-evloop.h"   /* DYN_EV_VNODE for the Watcher */
#include "core/dyn-timer.h" /* dyn_timer_now_ms: the debounce clock */
static uint64_t file_n_inline, file_n_offload;
static _Thread_local int file_async_pending_release;
static _Thread_local int file_async_hooked;
static _Thread_local JSContext *file_async_ctx;
static void file_async_reap(void *unused);
#endif

/* ==================================================================== *
 *  FileWriter -- buffered writer                                        *
 * ==================================================================== */

static JSClassID dyn_fwriter_class_id;

typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t len; /* buffered bytes not yet written to the fd */
    int fd;
    /* Anything written since the last successful durable sync. THE gate for
     * syncAsync: a durable sync with data to commit waits on the DEVICE
     * (measured 4810 us here), while one with nothing dirty is 7.3 us -- 660x
     * apart, so an unconditional offload would put a ~200 us hop in front of
     * 7 us of work. The predicate is a flag we already have to maintain. */
    int dirty;
} dyn_fwriter_t;

_Static_assert(sizeof(dyn_fwriter_t) == 3 * sizeof(void *) + 8,
               "dyn_fwriter_t regained padding: keep the two ints adjacent");

static void dyn_fwriter_flush_native(dyn_fwriter_t *w, int *err)
{
    size_t off = 0;
    *err = 0;
    while (off < w->len) {
        ssize_t n = write(w->fd, w->buf + off, w->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            *err = 1;
            return;
        }
        off += (size_t)n;
    }
    w->len = 0;
}

static void dyn_fwriter_dispose(void *native)
{
    dyn_fwriter_t *w = (dyn_fwriter_t *)native;
    int err;
    if (w->fd >= 0) {
        dyn_fwriter_flush_native(w, &err); /* best-effort flush on teardown */
        close(w->fd);
    }
    free(w->buf);
    free(w);
}

static const JSClassDef dyn_fwriter_class = {
    "FileWriter",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_fwriter_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_fwriter_t *w;
    const char *path;
    int64_t bufsize = 0, preallocate = 0;
    int append = 0, flags;
    int fd;

    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileWriter(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "preallocate");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) &&
            JS_ToInt64(ctx, &preallocate, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "append");
        append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fd = dyn_open_strict(path, flags, 0644);
    dyn_path_unborrow(ctx, path);
    if (fd < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: cannot open file");
    if (preallocate > 0)
        dyn_io_preallocate(fd, (off_t)preallocate);

    w = (dyn_fwriter_t *)calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->fd = fd;
    w->cap = dyn_file_clamp_bufsize(bufsize);
    w->buf = (unsigned char *)malloc(w->cap);
    if (!w->buf) {
        close(fd);
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, dyn_fwriter_class_id, w, dyn_fwriter_dispose);
}

/* Append `data`/`len` through the buffer, flushing when it fills; a write
 * larger than the buffer is sent directly after flushing what's buffered. */
static int dyn_fwriter_put(dyn_fwriter_t *w, const char *data, size_t len)
{
    int err;
    if (len >= w->cap) {
        size_t off = 0;
        dyn_fwriter_flush_native(w, &err);
        w->dirty = 1;
        if (err)
            return -1;
        while (off < len) {
            ssize_t n = write(w->fd, data + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            off += (size_t)n;
        }
        return 0;
    }
    if (w->len + len > w->cap) {
        dyn_fwriter_flush_native(w, &err);
        if (err)
            return -1;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    w->dirty = 1;
    return 0;
}

/* Resolve a write payload to a byte range.
 *
 * A string materialises to an OWNED UTF-8 buffer (*powned, release with
 * JS_FreeCString); an ArrayBuffer or ANY TypedArray/DataView yields a borrowed
 * pointer into its backing store, valid for the synchronous remainder of the
 * call; anything else goes through ToString.
 *
 * The TypedArray case is the one that matters. Without it a Uint8Array falls
 * through to ToString and is written as the decimal text "0,1,2,255,65" -- so
 * writeFile(p, gzip(x)), the most natural call there is, silently produced a
 * corrupt file and returned a plausible byte count. It also has to clear the
 * pending exception each failed probe leaves behind, or the function returns a
 * value with an exception still set.
 *
 * Returns 0 (exactly one of *powned / a borrowed pointer is set) or -1 with a
 * pending exception. Mirrors dyn_crypto_data in dyna-crypto.c. */
static int dyn_file_payload(JSContext *ctx, JSValueConst v, const uint8_t **pdata,
                            size_t *plen, const char **powned)
{
    *powned = NULL;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
    {
        size_t n;
        uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
        if (p) {
            *pdata = p;
            *plen = n;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not an ArrayBuffer: retry */
    }
    {
        size_t off, len, bpe, ab_size;
        uint8_t *base;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1; /* detached mid-resolve; already threw */
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not a view: fall through */
    }
    {   /* generic: ToString (runs user JS) -> owned UTF-8 bytes */
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t *)s;
        *plen = n;
        return 0;
    }
}

/* write(data): a string, an ArrayBuffer, or any TypedArray/DataView. Returns
 * bytes accepted. */
static JSValue dyn_fwriter_write(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_fwriter_t *w;
    const uint8_t *data = NULL;
    const char *str = NULL;
    size_t len = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "write(data) requires an argument");
    /* coerce the payload to a C view FIRST (may run user JS that closes this) */
    if (dyn_file_payload(ctx, argv[0], &data, &len, &str))
        return JS_EXCEPTION;

    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (dyn_fwriter_put(w, (const char *)data, len) < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "FileWriter: write error");
    }
    if (str)
        JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx, (int64_t)len);
}

static JSValue dyn_fwriter_flush(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    dyn_fwriter_t *w;
    int err;
    (void)argc; (void)argv;
    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err)
        return JS_ThrowInternalError(ctx, "FileWriter: flush error");
    return JS_UNDEFINED;
}

static JSValue dyn_fwriter_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_fwriter_t *w;
    int err;
    (void)argc; (void)argv;
    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err || dyn_io_durable_sync(w->fd) < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: sync error");
    w->dirty = 0;
    return JS_UNDEFINED;
}

#if defined(CONFIG_NATIVE_MODULE_NET)
/* ---- syncAsync: the same durability, off the loop --------------------------
 *
 * A durable sync is the hardest-blocking operation in this module. On Darwin
 * it is fcntl(F_FULLFSYNC), which waits for the DEVICE to commit rather than
 * for the kernel to accept -- MEASURED HERE at 4810 us against 38.7 us for a
 * plain fsync, 135x. Five milliseconds is fifty times the block that justified
 * offloading a 1 MiB read, and until now there was no way to avoid it.
 *
 * TWO STRATEGIES, and the adversarial case decides the gate: a sync with
 * NOTHING dirty measured 7.3 us -- 660x cheaper -- so offloading every call
 * would put a ~200 us hop in front of 7 us of work, 27x slower. The gate is
 * therefore "is there anything of ours to commit", which is a flag, not a
 * syscall. The flush still happens on the LOOP thread: it is a plain write to
 * the page cache, and moving it would hand the worker a buffer the caller can
 * still touch. */
typedef struct {
    JSContext *ctx;
    JSValue resolve, reject;
    JSValue self;               /* keeps the FileWriter alive across the hop */
    int fd;
    int rc;
    int err;
    int offloaded;
} fsync_job_t;

static void fsync_job_work(void *arg)      /* WORKER: no JS_* in here */
{
    fsync_job_t *j = (fsync_job_t *)arg;
    j->rc = dyn_io_durable_sync(j->fd);
    j->err = j->rc < 0 ? errno : 0;
}

static void fsync_job_done(void *arg)      /* LOOP: JS is safe here */
{
    fsync_job_t *j = (fsync_job_t *)arg;
    JSContext *ctx = j->ctx;
    JSValue v, r;
    JSValueConst a1[1];

    if (j->rc < 0) {
        v = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, v, "message",
                          JS_NewString(ctx, strerror(j->err ? j->err : EIO)));
        JS_SetPropertyStr(ctx, v, "errno", JS_NewInt32(ctx, j->err));
        a1[0] = v;
        r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, a1);
    } else {
        v = JS_UNDEFINED;
        a1[0] = v;
        r = JS_Call(ctx, j->resolve, JS_UNDEFINED, 1, a1);
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    JS_FreeValue(ctx, j->self);
    if (j->offloaded)
        file_async_pending_release++;   /* the hook releases; never in here */
    free(j);
}

static JSValue dyn_fwriter_sync_async(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_fwriter_t *w;
    fsync_job_t *j;
    JSValue funcs[2], promise;
    struct dyn_aio *aio;
    int err, was_dirty;
    (void)argc; (void)argv;

    w = (dyn_fwriter_t *)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    /* Flush here, on the loop: these bytes go to the page cache, and handing
     * a worker a buffer the caller still owns is a race, not an offload. */
    dyn_fwriter_flush_native(w, &err);
    was_dirty = w->dirty;
    j = (fsync_job_t *)calloc(1, sizeof(*j));
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { free(j); return promise; }
    j->ctx = ctx; j->fd = w->fd;
    j->resolve = funcs[0]; j->reject = funcs[1];
    j->self = JS_DupValue(ctx, this_val);   /* the fd must outlive the hop */
    if (err) { j->rc = -1; j->err = EIO; fsync_job_done(j); return promise; }
    w->dirty = 0;

    if (!was_dirty) {                 /* 7.3 us: the hop would cost 27x that */
        file_n_inline++;
        fsync_job_work(j);
        fsync_job_done(j);
        return promise;
    }
    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) { file_n_inline++; fsync_job_work(j); fsync_job_done(j); return promise; }
    if (!file_async_hooked && dyn_net_on_drain(file_async_reap,
                                               &file_async_hooked) >= 0) {
        file_async_hooked = 1;
        file_async_ctx = ctx;
    }
    j->offloaded = 1;
    if (dyn_aio_offload(aio, fsync_job_work, fsync_job_done, j) == 0)
        file_n_offload++;
    else
        file_n_inline++;
    return promise;
}
#endif /* CONFIG_NATIVE_MODULE_NET */

static const JSCFunctionListEntry dyn_fwriter_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_fwriter_write),
    JS_CFUNC_DEF("flush", 0, dyn_fwriter_flush),
#if defined(CONFIG_NATIVE_MODULE_NET)
    JS_CFUNC_DEF("syncAsync", 0, dyn_fwriter_sync_async),
#endif
    JS_CFUNC_DEF("sync", 0, dyn_fwriter_sync),
};

/* ==================================================================== *
 *  one-shot convenience functions                                       *
 * ==================================================================== */

static JSValue dyn_file_read_file(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    dyn_iobuf_t src;
    JSValue out;
    (void)this_val; (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    /* Strict open FIRST, then slurp from the fd: dyn_io_slurp resolves by name
       and would follow a symlink component. */
    {
        int rfd = dyn_open_strict(path, O_RDONLY | O_CLOEXEC, 0);
        if (rfd < 0 || dyn_io_slurp_fd(rfd, &src, 0) < 0) {
            dyn_path_unborrow(ctx, path);
            return JS_ThrowInternalError(ctx, "readFile: cannot read file");
        }
    }
    dyn_path_unborrow(ctx, path);
    out = JS_NewStringLen(ctx, (const char *)dyn_iobuf_rdata(&src),
                          dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return out;
}

static JSValue dyn_file_write_file(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *path = NULL, *str = NULL;
    const uint8_t *data = NULL;
    size_t len = 0, off = 0;
    int append = 0, flags, fd;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, options])");
    /* coerce everything first */
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (dyn_file_payload(ctx, argv[1], &data, &len, &str)) {
        dyn_path_unborrow(ctx, path);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "append");
        append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }

    flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    fd = dyn_open_strict(path, flags, 0644);
    dyn_path_unborrow(ctx, path);
    if (fd < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "writeFile: cannot open file");
    }
    {
        const char *src = (const char *)data;
        int werr = 0;
        while (off < len) {
            ssize_t n = write(fd, src + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                werr = 1;
                break;
            }
            off += (size_t)n;
        }
        close(fd);
        if (str)
            JS_FreeCString(ctx, str);
        if (werr)
            return JS_ThrowInternalError(ctx, "writeFile: write error");
    }
    return JS_NewInt64(ctx, (int64_t)len);
}

/* ==================================================================== *
 *  filesystem operations -- metadata, directories, links, globbing and
 *  temp files (moved here from dyna:sys; dyna:sys keeps process/env only).
 *  These are transient plain functions (no `this`): they coerce every JS
 *  arg to an owned C local, then perform the syscall, freeing on every path.
 * ==================================================================== */

/* Bounds on recursive filesystem walks (removeAll / glob). Real directory
 * trees are far shallower; these only guard against a pathological/adversarial
 * nesting so the JS-thread C stack can never overflow. */
#define DYN_FS_RMRF_MAX_DEPTH 512
#define DYN_FS_GLOB_MAX_DEPTH 512
#define DYN_FS_READLINK_MAX   (1u << 16)

static const char *dyn_fs_errno_code(int e)
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
static JSValue dyn_fs_throw(JSContext *ctx, int e, const char *op,
                             const char *path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char *code = dyn_fs_errno_code(e);

    if (path)
        snprintf(msg, sizeof(msg), "file.%s(\"%s\"): %s", op, path, strerror(e));
    else
        snprintf(msg, sizeof(msg), "file.%s: %s", op, strerror(e));

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

/* macOS names the sub-second stat fields differently from Linux. */
#if defined(__APPLE__)
#define DYN_STAT_MTIM(st) ((st).st_mtimespec)
#define DYN_STAT_ATIM(st) ((st).st_atimespec)
#define DYN_STAT_CTIM(st) ((st).st_ctimespec)
#else
#define DYN_STAT_MTIM(st) ((st).st_mtim)
#define DYN_STAT_ATIM(st) ((st).st_atim)
#define DYN_STAT_CTIM(st) ((st).st_ctim)
#endif

static double dyn_timespec_ms(struct timespec ts)
{
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* ==================================================================== *
 *  small path-string helpers (heap, sized to input)                     *
 * ==================================================================== */

/* Join two path fragments with a single '/'. If `a` is empty, returns a copy of
 * `b`; if `a` already ends with '/', no extra separator is inserted. Returns a
 * malloc'd NUL-terminated string, or NULL on OOM (caller frees). */
static char *dyn_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int sep = (la > 0 && a[la - 1] != '/');
    char *r;

    if (la == 0) {
        r = (char *)malloc(lb + 1);
        if (!r)
            return NULL;
        memcpy(r, b, lb + 1);
        return r;
    }
    r = (char *)malloc(la + (size_t)sep + lb + 1);
    if (!r)
        return NULL;
    memcpy(r, a, la);
    if (sep)
        r[la] = '/';
    memcpy(r + la + (size_t)sep, b, lb);
    r[la + (size_t)sep + lb] = '\0';
    return r;
}

static int dyn_is_dot_or_dotdot(const char *name)
{
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* ==================================================================== *
 *  stat / lstat / exists                                                *
 * ==================================================================== */

static JSValue dyn_fs_stat_common(JSContext *ctx, JSValueConst arg, int follow)
{
    const char *path;
    struct stat st;
    int r;
    JSValue obj;

    path = dyn_path_arg(ctx, arg, "path");
    if (!path)
        return JS_EXCEPTION;
    r = dyn_stat_strict(path, &st, follow);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, follow ? "stat" : "lstat", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, S_ISREG(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isSymlink",
                      JS_NewBool(ctx, S_ISLNK(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "mtimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_MTIM(st))));
    JS_SetPropertyStr(ctx, obj, "atimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_ATIM(st))));
    JS_SetPropertyStr(ctx, obj, "ctimeMs",
                      JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_CTIM(st))));
    JS_SetPropertyStr(ctx, obj, "uid", JS_NewInt32(ctx, (int32_t)st.st_uid));
    JS_SetPropertyStr(ctx, obj, "gid", JS_NewInt32(ctx, (int32_t)st.st_gid));
    JS_SetPropertyStr(ctx, obj, "ino", JS_NewInt64(ctx, (int64_t)st.st_ino));
    JS_SetPropertyStr(ctx, obj, "nlink",
                      JS_NewInt64(ctx, (int64_t)st.st_nlink));
    return obj;
}

static JSValue dyn_fs_stat(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 1);
}

static JSValue dyn_fs_lstat(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 0);
}

/* exists(path) -> bool. Never throws: any error (missing, permission, ...)
 * yields false. Uses lstat, so a dangling symlink reports true. */
static JSValue dyn_fs_exists(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *path;
    struct stat st;
    int ok;

    (void)this_val; (void)argc;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    ok = (dyn_stat_strict(path, &st, 0) == 0);
    dyn_path_unborrow(ctx, path);
    return JS_NewBool(ctx, ok);
}

/* ==================================================================== *
 *  readDir                                                              *
 * ==================================================================== */

/* The name lives INLINE unless it is long: a directory listing was one malloc
   per entry for a string used once. Not a pointer into the struct -- qsort
   moves elements, so a self-reference would dangle after the sort. */
#define DYN_DIRENT_INLINE 48

typedef struct {
    char *heap;                 /* NULL => the name is in `inl` */
    char  inl[DYN_DIRENT_INLINE];
    int is_dir, is_file, is_symlink;
} dyn_dirent_t;

static const char *dyn_dirent_name(const dyn_dirent_t *e)
{
    return e->heap ? e->heap : e->inl;
}

static int dyn_dirent_cmp(const void *a, const void *b)
{
    return strcmp(dyn_dirent_name((const dyn_dirent_t *)a),
                  dyn_dirent_name((const dyn_dirent_t *)b));
}

/* Fill (is_dir,is_file,is_symlink) for one entry. Uses readdir's d_type as a
 * fast path and falls back to lstat when it is unknown/unsupported. */
/* Classify one entry. Takes the path PIECES rather than a joined string so the
 * join happens only in the lstat branch -- the branch that needs it. Building
 * it in the caller "just in case" cost a realloc and two memcpys per entry on
 * every filesystem that reports d_type (APFS, ext4, btrfs: all of them), and,
 * worse, put the correctness of the DT_UNKNOWN path in a buffer the caller
 * maintained. DT_UNKNOWN happens on NFS and some FUSE mounts and CANNOT be
 * produced on this host, so that arrangement was unverifiable here -- inverting
 * the caller's condition passed the entire suite. One construction site, inside
 * the only consumer, is safe by shape instead of by test. */
static void dyn_entry_type(int dtype, const char *dir, size_t dirlen,
                           const char *name, size_t namelen,
                           int *is_dir, int *is_file, int *is_symlink)
{
    char stackbuf[512], *full = stackbuf, *heap = NULL;
    size_t need = dirlen + 1 + namelen + 1;
    struct stat st;

    *is_dir = *is_file = *is_symlink = 0;
#ifdef DT_DIR
    switch (dtype) {
    case DT_DIR: *is_dir = 1; return;
    case DT_REG: *is_file = 1; return;
    case DT_LNK: *is_symlink = 1; return;
    default: break; /* DT_UNKNOWN or a type we don't surface -> lstat */
    }
#else
    (void)dtype;
#endif
    if (need > sizeof(stackbuf)) {
        heap = (char *)malloc(need);
        if (!heap)
            return;             /* all three stay 0: unknown, not wrong */
        full = heap;
    }
    memcpy(full, dir, dirlen);
    full[dirlen] = '/';
    memcpy(full + dirlen + 1, name, namelen + 1);
    if (lstat(full, &st) == 0) {
        *is_dir = S_ISDIR(st.st_mode);
        *is_file = S_ISREG(st.st_mode);
        *is_symlink = S_ISLNK(st.st_mode);
    }
    free(heap);
}

static JSValue dyn_fs_read_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *path;
    size_t path_len;
    DIR *d;
    struct dirent *e;
    dyn_dirent_t *ents = NULL;
    size_t n = 0, cap = 0, i;
    JSValue arr;
    int err = 0;

    (void)this_val; (void)argc;
    path = dyn_path_arg_len(ctx, argv[0], "path", &path_len);
    if (!path)
        return JS_EXCEPTION;

    d = opendir(path);
    if (!d) {
        int en = errno;
        JSValue ex = dyn_fs_throw(ctx, en, "readDir", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }

    while ((e = readdir(d)) != NULL) {
        size_t nlen;
        int dtype;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        nlen = strlen(e->d_name);
#ifdef DT_DIR
        dtype = e->d_type;
#else
        dtype = 0;
#endif
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            dyn_dirent_t *ne =
                (dyn_dirent_t *)realloc(ents, ncap * sizeof(*ents));
            if (!ne) { err = 1; break; }
            ents = ne;
            cap = ncap;
        }
        if (nlen < DYN_DIRENT_INLINE) {
            ents[n].heap = NULL;
            memcpy(ents[n].inl, e->d_name, nlen + 1);
        } else {
            ents[n].heap = (char *)malloc(nlen + 1);
            if (!ents[n].heap) { err = 1; break; }
            memcpy(ents[n].heap, e->d_name, nlen + 1);
        }
        dyn_entry_type(dtype, path, path_len, e->d_name, nlen,
                       &ents[n].is_dir, &ents[n].is_file, &ents[n].is_symlink);
        n++;
    }
    closedir(d);
    dyn_path_unborrow(ctx, path);

    if (err) {
        for (i = 0; i < n; i++)
            free(ents[i].heap);
        free(ents);
        return JS_ThrowOutOfMemory(ctx);
    }

    qsort(ents, n, sizeof(*ents), dyn_dirent_cmp);

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        for (i = 0; i < n; i++)
            free(ents[i].heap);
        free(ents);
        return JS_EXCEPTION;
    }
    /* Presize once instead of growing on every append. */
    if (n)
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt64(ctx, (int64_t)n));
    /* Intern the four keys ONCE, not once per entry. JS_SetPropertyStr interns
     * its name string on every call, so this loop was 4n atom lookups for four
     * distinct keys; it is now 4. Define rather than Set: these are fresh own
     * properties, so Set's prototype-chain walk is pure overhead. NOTE the
     * four keys are fixed literals, so unlike the PostgreSQL row decoder --
     * where a COLUMN NAME becomes the key -- there is no __proto__ hazard
     * here; this is a speed choice, not a safety one. */
    {
    JSAtom a_name = JS_NewAtom(ctx, "name");
    JSAtom a_dir = JS_NewAtom(ctx, "isDir");
    JSAtom a_file = JS_NewAtom(ctx, "isFile");
    JSAtom a_link = JS_NewAtom(ctx, "isSymlink");
    for (i = 0; i < n; i++) {
        JSValue o = JS_NewObject(ctx);
        if (!JS_IsException(o)) {
            JS_DefinePropertyValue(ctx, o, a_name,
                                   JS_NewString(ctx, dyn_dirent_name(&ents[i])), JS_PROP_C_W_E);
            JS_DefinePropertyValue(ctx, o, a_dir,
                                   JS_NewBool(ctx, ents[i].is_dir), JS_PROP_C_W_E);
            JS_DefinePropertyValue(ctx, o, a_file,
                                   JS_NewBool(ctx, ents[i].is_file), JS_PROP_C_W_E);
            JS_DefinePropertyValue(ctx, o, a_link,
                                   JS_NewBool(ctx, ents[i].is_symlink), JS_PROP_C_W_E);
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        free(ents[i].heap);
    }
    JS_FreeAtom(ctx, a_name); JS_FreeAtom(ctx, a_dir);
    JS_FreeAtom(ctx, a_file); JS_FreeAtom(ctx, a_link);
    }
    free(ents);
    return arr;
}

/* ==================================================================== *
 *  makeDir / remove / removeAll / rename                                *
 * ==================================================================== */

/* mkdir -p: create `path` and any missing parents. Returns 0 or -1 (errno set,
 * and matches the failing mkdir's errno). An existing directory is success. */
static int dyn_fs_mkdirp(const char *path, mode_t mode)
{
    struct stat st;
    size_t len;
    char *parent;
    int r;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST; /* exists but is not a directory */
        return -1;
    }
    if (errno != ENOENT)
        return -1;

    /* create the parent, then retry */
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/')
        len--;
    while (len > 0 && path[len - 1] != '/')
        len--;
    while (len > 0 && path[len - 1] == '/')
        len--;
    if (len == 0) {
        errno = ENOENT;
        return -1;
    }
    parent = (char *)malloc(len + 1);
    if (!parent) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    r = dyn_fs_mkdirp(parent, mode);
    free(parent);
    if (r != 0)
        return -1;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
    }
    return -1;
}

static JSValue dyn_fs_make_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *path;
    int recursive = 0;
    int32_t mode = 0777;
    int r;

    (void)this_val;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "mode");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt32(ctx, &mode, v)) {
                JS_FreeValue(ctx, v);
                dyn_path_unborrow(ctx, path);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    if (recursive)
        r = dyn_fs_mkdirp(path, (mode_t)mode);
    else
        r = mkdir(path, (mode_t)mode);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "makeDir", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    return JS_UNDEFINED;
}

/* remove(path): unlink a file or an EMPTY directory (libc remove()). */
static JSValue dyn_fs_remove(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *path;
    (void)this_val; (void)argc;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (remove(path) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "remove", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    return JS_UNDEFINED;
}

/* Remove everything UNDER the directory referred to by `dirfd`, recursively,
 * without ever following a symlink. Takes ownership of `dirfd` (closes it).
 * Returns 0 or -1 (errno set). */
static int dyn_fs_rmrf_children(int dirfd, int depth)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        close(dirfd);
        errno = ELOOP;
        return -1;
    }
    /* fdopendir takes ownership of dirfd (closedir closes it). */
    d = fdopendir(dirfd);
    if (!d) {
        int e2 = errno;
        close(dirfd);
        errno = e2;
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int cfd = openat(dirfd, e->d_name,
                             O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            if (cfd < 0) {
                rc = -1;
                break;
            }
            if (dyn_fs_rmrf_children(cfd, depth + 1) != 0) {
                rc = -1;
                break;
            }
            if (unlinkat(dirfd, e->d_name, AT_REMOVEDIR) != 0) {
                rc = -1;
                break;
            }
        } else {
            if (unlinkat(dirfd, e->d_name, 0) != 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(d); /* closes dirfd */
    return rc;
}

/* removeAll(path): recursive, symlink-safe, missing path is a no-op. */
static JSValue dyn_fs_remove_all(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    struct stat st;

    (void)this_val; (void)argc;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    if (lstat(path, &st) != 0) {
        int e = errno;
        dyn_path_unborrow(ctx, path);
        if (e == ENOENT)
            return JS_UNDEFINED; /* nothing to remove */
        return dyn_fs_throw(ctx, e, "removeAll", NULL);
    }

    if (S_ISDIR(st.st_mode)) {
        int fd = dyn_open_strict(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        if (fd < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
        if (dyn_fs_rmrf_children(fd, 0) != 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
        if (rmdir(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
    }
    dyn_path_unborrow(ctx, path);
    return JS_UNDEFINED;
}

static JSValue dyn_fs_rename(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    const char *from, *to;
    (void)this_val; (void)argc;

    from = dyn_path_arg(ctx, argv[0], "from");
    if (!from)
        return JS_EXCEPTION;
    to = dyn_path_arg(ctx, argv[1], "to");
    if (!to) {
        dyn_path_unborrow(ctx, from);
        return JS_EXCEPTION;
    }
    if (rename(from, to) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "rename", from);
        dyn_path_unborrow(ctx, from);
        dyn_path_unborrow(ctx, to);
        return ex;
    }
    dyn_path_unborrow(ctx, from);
    dyn_path_unborrow(ctx, to);
    return JS_UNDEFINED;
}

/* ==================================================================== *
 *  symlink / readLink / realPath / chmod                                *
 * ==================================================================== */

static JSValue dyn_fs_symlink(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    const char *target, *linkpath;
    (void)this_val; (void)argc;

    /* The symlink TARGET is not a path this module resolves -- it is opaque
     * bytes stored inside the link, may be relative, and may name something
     * that does not exist. Normalising it would silently rewrite the link:
     * symlink("a/../b", l) would store "b". So the target is a string and only
     * the LINK LOCATION is a Path. */
    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    linkpath = dyn_path_arg(ctx, argv[1], "linkpath");
    if (!linkpath) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    if (symlink(target, linkpath) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "symlink", linkpath);
        JS_FreeCString(ctx, target);
        dyn_path_unborrow(ctx, linkpath);
        return ex;
    }
    JS_FreeCString(ctx, target);
    dyn_path_unborrow(ctx, linkpath);
    return JS_UNDEFINED;
}

static JSValue dyn_fs_read_link(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *path;
    size_t cap = 256;
    (void)this_val; (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    for (;;) {
        char *buf = (char *)malloc(cap);
        ssize_t n;
        if (!buf) {
            dyn_path_unborrow(ctx, path);
            return JS_ThrowOutOfMemory(ctx);
        }
        n = readlink(path, buf, cap);
        if (n < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "readLink", path);
            free(buf);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
        if ((size_t)n < cap) {
            /* Returns the stored target verbatim, for the same reason
             * symlink() takes it verbatim: it is the link's contents, not a
             * resolved location. realPath() is the one that returns a Path. */
            JSValue out = JS_NewStringLen(ctx, buf, (size_t)n);
            free(buf);
            dyn_path_unborrow(ctx, path);
            return out;
        }
        /* result filled the buffer: it may be truncated -- grow and retry */
        free(buf);
        cap *= 2;
        if (cap > DYN_FS_READLINK_MAX) {
            JSValue ex = dyn_fs_throw(ctx, ENAMETOOLONG, "readLink", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
    }
}

static JSValue dyn_fs_real_path(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *path;
    char *resolved;
    JSValue out;
    (void)this_val; (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    resolved = realpath(path, NULL); /* POSIX.1-2008: NULL => malloc'd result */
    if (!resolved) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "realPath", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    out = dyn_path_new_from(ctx, resolved, strlen(resolved));
    free(resolved);
    return out;
}

static JSValue dyn_fs_chmod(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    const char *path;
    int32_t mode;
    (void)this_val; (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &mode, argv[1])) {
        dyn_path_unborrow(ctx, path);
        return JS_EXCEPTION;
    }
    if (chmod(path, (mode_t)mode) != 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "chmod", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    return JS_UNDEFINED;
}

/* ==================================================================== *
 *  glob -- self-contained *, **, ?, [...] matcher + directory walk      *
 * ==================================================================== */

/* Match a single path component `s` against pattern `p` (which contains no
 * '/'). Supports *, ?, and [...] classes (with ranges, and ! or ^ negation).
 * An unterminated '[' is treated as a literal '['. */
/* Iterative, single-backtrack glob match.
 *
 * The recursive form re-entered itself for EVERY suffix at every '*', which is
 * O(n^k / k!) in the star count: "*a*a*a*a*a*a*a*b" against 40 'a's took
 * 286 ms, and a pattern with enough stars overflowed the C stack before it
 * finished. Since '/' is an ordinary byte here, matches() runs the pattern
 * against the whole path, so long subjects are easy to supply.
 *
 * Only ONE backtrack point is ever needed: remember where the last '*' was and
 * how much of the subject it had consumed, and on any later mismatch let it
 * swallow one more character. O(n*m) worst case, O(n) typical, no recursion.
 * '?' and '[...]' consume exactly one character each, so they compose with the
 * rewind unchanged. */
static int dyn_glob_match(const char *p, const char *s)
{
    const char *star_p = NULL, *star_s = NULL;

    for (;;) {
        unsigned char pc = (unsigned char)*p;

        if (pc == '*') {
            while (*p == '*')
                p++;
            if (*p == '\0')
                return 1;           /* trailing '*' matches the rest */
            star_p = p;             /* resume here if the tail fails */
            star_s = s;
            continue;
        }
        if (pc == '\0') {
            if (*s == '\0')
                return 1;
            goto backtrack;         /* pattern spent, subject is not */
        }
        if (*s == '\0')
            goto backtrack;         /* subject spent, pattern still needs input */

        if (pc == '?') {
            p++; s++;
            continue;
        }
        if (pc == '[') {
            const char *q = p + 1;
            const char *start;
            int negate = 0, matched = 0;
            unsigned char c = (unsigned char)*s;
            if (*q == '!' || *q == '^') {
                negate = 1;
                q++;
            }
            start = q;
            while (*q && !(*q == ']' && q != start)) {
                if (q[0] && q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char lo = (unsigned char)q[0];
                    unsigned char hi = (unsigned char)q[2];
                    if (lo <= c && c <= hi)
                        matched = 1;
                    q += 3;
                } else {
                    if ((unsigned char)*q == c)
                        matched = 1;
                    q++;
                }
            }
            if (*q != ']') {
                /* unterminated class: treat '[' as a literal character */
                if (c != '[')
                    goto backtrack;
                p++; s++;
                continue;
            }
            q++;                    /* consume ']' */
            if (matched == negate)
                goto backtrack;
            p = q; s++;
            continue;
        }
        /* literal byte */
        if (pc == (unsigned char)*s) {
            p++; s++;
            continue;
        }

    backtrack:
        if (!star_p)
            return 0;               /* no '*' to give ground */
        star_s++;                   /* let the last '*' eat one more char */
        if (*star_s == '\0' && *(star_s - 1) == '\0')
            return 0;
        p = star_p;
        s = star_s;
        if (*(s - 1) == '\0')
            return 0;               /* ran past the end of the subject */
    }
}

/* Apply the minimatch "leading dot" rule: a wildcard metacharacter does not
 * match a name that begins with '.'; the segment must start with a literal
 * '.' to match a dotfile. */
static int dyn_glob_match_name(const char *pat, const char *name)
{
    if (name[0] == '.' && pat[0] != '.')
        return 0;
    return dyn_glob_match(pat, name);
}

static int dyn_glob_has_wildcard(const char *s)
{
    return strpbrk(s, "*?[") != NULL;
}

typedef struct {
    char **items;
    size_t count, cap;
    int oom;
} dyn_glob_res;

static void dyn_glob_res_push(dyn_glob_res *r, const char *s, size_t len)
{
    char *dup;
    if (r->oom)
        return;
    if (r->count == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 32;
        char **ni = (char **)realloc(r->items, nc * sizeof(*ni));
        if (!ni) {
            r->oom = 1;
            return;
        }
        r->items = ni;
        r->cap = nc;
    }
    dup = (char *)malloc(len + 1);
    if (!dup) {
        r->oom = 1;
        return;
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    r->items[r->count++] = dup;
}

static void dyn_glob_res_free(dyn_glob_res *r)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        free(r->items[i]);
    free(r->items);
}

/* Emit the fully-matched relative path `rel` in display form (prefixed with '/'
 * for an absolute pattern; "." for the empty relative root). */
static void dyn_glob_emit(dyn_glob_res *res, const char *rel, int is_abs)
{
    if (is_abs) {
        char *disp = dyn_join("/", rel); /* "/"+rel, or "/" when rel=="" */
        if (!disp) {
            res->oom = 1;
            return;
        }
        dyn_glob_res_push(res, disp, strlen(disp));
        free(disp);
    } else if (rel[0] == '\0') {
        dyn_glob_res_push(res, ".", 1);
    } else {
        dyn_glob_res_push(res, rel, strlen(rel));
    }
}

/* Compute the on-disk directory to list for the walk position `rel`. */
static char *dyn_glob_fsdir(const char *base, const char *rel)
{
    if (rel[0] == '\0')
        return dyn_join(base, ""); /* == a copy of base */
    return dyn_join(base, rel);
}

static void dyn_glob_walk(dyn_glob_res *res, const char *base, char **segs,
                          int nseg, int si, const char *rel, int is_abs,
                          int depth);

static void dyn_glob_walk(dyn_glob_res *res, const char *base, char **segs,
                          int nseg, int si, const char *rel, int is_abs,
                          int depth)
{
    const char *seg;
    char *fsdir;

    if (res->oom || depth > DYN_FS_GLOB_MAX_DEPTH)
        return;
    if (si == nseg) {
        dyn_glob_emit(res, rel, is_abs);
        return;
    }
    seg = segs[si];

    if (strcmp(seg, "**") == 0) {
        int is_last = (si == nseg - 1);
        DIR *d;
        struct dirent *e;

        /* '**' matching zero directories: advance to the next segment here.
         * As the last segment, '**' also matches the current directory itself
         * (when non-empty) plus its whole subtree (files and dirs below). */
        if (is_last) {
            if (rel[0] != '\0')
                dyn_glob_emit(res, rel, is_abs);
        } else {
            dyn_glob_walk(res, base, segs, nseg, si + 1, rel, is_abs, depth);
        }

        fsdir = dyn_glob_fsdir(base, rel);
        if (!fsdir) {
            res->oom = 1;
            return;
        }
        d = opendir(fsdir);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char *childrel, *childfs;
                struct stat st;
                if (dyn_is_dot_or_dotdot(e->d_name) || e->d_name[0] == '.')
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) { res->oom = 1; break; }
                childfs = dyn_join(fsdir, e->d_name);
                if (!childfs) { free(childrel); res->oom = 1; break; }
                /* '**' spans into real subdirectories only (never a symlink,
                 * so a symlink cycle cannot recurse forever). */
                if (lstat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                    dyn_glob_walk(res, base, segs, nseg, si, childrel, is_abs,
                                  depth + 1);
                else if (is_last)
                    dyn_glob_emit(res, childrel, is_abs); /* trailing-'**' file */
                free(childfs);
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
        free(fsdir);
        return;
    }

    /* literal segment (no wildcard): probe the specific child directly, so a
     * component we cannot list (permission) still resolves an explicit name. */
    if (!dyn_glob_has_wildcard(seg)) {
        char *childrel = (rel[0] == '\0') ? dyn_join("", seg)
                                          : dyn_join(rel, seg);
        char *childfs;
        struct stat st;
        if (!childrel) { res->oom = 1; return; }
        childfs = dyn_glob_fsdir(base, childrel);
        if (!childfs) { free(childrel); res->oom = 1; return; }
        if (si == nseg - 1) {
            if (lstat(childfs, &st) == 0)
                dyn_glob_emit(res, childrel, is_abs);
        } else {
            if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                dyn_glob_walk(res, base, segs, nseg, si + 1, childrel, is_abs,
                              depth + 1);
        }
        free(childfs);
        free(childrel);
        return;
    }

    /* wildcard segment: list the directory and match each entry. */
    fsdir = dyn_glob_fsdir(base, rel);
    if (!fsdir) {
        res->oom = 1;
        return;
    }
    {
        DIR *d = opendir(fsdir);
        int is_last = (si == nseg - 1);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char *childrel;
                if (dyn_is_dot_or_dotdot(e->d_name))
                    continue;
                if (!dyn_glob_match_name(seg, e->d_name))
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) { res->oom = 1; break; }
                if (is_last) {
                    dyn_glob_emit(res, childrel, is_abs);
                } else {
                    char *childfs = dyn_join(fsdir, e->d_name);
                    struct stat st;
                    if (!childfs) { free(childrel); res->oom = 1; break; }
                    if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                        dyn_glob_walk(res, base, segs, nseg, si + 1, childrel,
                                      is_abs, depth + 1);
                    free(childfs);
                }
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
    }
    free(fsdir);
}

static int dyn_glob_str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static JSValue dyn_fs_glob(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    const char *pattern = NULL, *cwd = NULL;
    char *patcopy = NULL, **segs = NULL;
    int nseg = 0, is_abs, i;
    size_t plen;
    const char *base;
    dyn_glob_res res;
    JSValue arr;
    char *p;

    (void)this_val;
    pattern = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pattern)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "cwd");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            cwd = dyn_path_arg(ctx, v, "cwd");
            if (!cwd) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, pattern);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    memset(&res, 0, sizeof(res));

    if (plen == 0) {
        /* empty pattern matches nothing */
        JS_FreeCString(ctx, pattern);
        if (cwd)
            dyn_path_unborrow(ctx, cwd);
        return JS_NewArray(ctx);
    }

    patcopy = (char *)malloc(plen + 1);
    segs = (char **)malloc((plen + 2) * sizeof(*segs));
    if (!patcopy || !segs) {
        free(patcopy);
        free(segs);
        JS_FreeCString(ctx, pattern);
        if (cwd)
            dyn_path_unborrow(ctx, cwd);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(patcopy, pattern, plen + 1);
    is_abs = (patcopy[0] == '/');

    /* split on '/', dropping empty segments (leading/trailing/duplicate '/') */
    p = patcopy;
    while (*p) {
        char *seg_start;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        seg_start = p;
        while (*p && *p != '/')
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        segs[nseg++] = seg_start;
    }

    base = is_abs ? "/" : ((cwd && cwd[0]) ? cwd : ".");
    dyn_glob_walk(&res, base, segs, nseg, 0, "", is_abs, 0);

    free(patcopy);
    free(segs);
    JS_FreeCString(ctx, pattern);
    if (cwd)
        dyn_path_unborrow(ctx, cwd);

    if (res.oom) {
        dyn_glob_res_free(&res);
        return JS_ThrowOutOfMemory(ctx);
    }

    /* sort, then drop adjacent duplicates (a pathological pattern with two
     * '**' segments can reach the same path twice). */
    qsort(res.items, res.count, sizeof(res.items[0]), dyn_glob_str_cmp);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_glob_res_free(&res);
        return JS_EXCEPTION;
    }
    {
        uint32_t out = 0;
        for (i = 0; i < (int)res.count; i++) {
            if (i > 0 && strcmp(res.items[i], res.items[i - 1]) == 0)
                continue;
            JS_SetPropertyUint32(ctx, arr, out++,
                                 dyn_path_new_from(ctx, res.items[i],
                                                   strlen(res.items[i])));
        }
    }
    dyn_glob_res_free(&res);
    return arr;
}

/* ==================================================================== *
 *  temp                                                                 *
 * ==================================================================== */

/* System temp directory (TMPDIR, else /tmp). Returned pointer is into environ
 * or a static literal -- valid transiently, do not free. */
static const char *dyn_fs_tempdir_str(void)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return t;
}

static JSValue dyn_fs_temp_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    const char *t = dyn_fs_tempdir_str();
    size_t len = strlen(t);
    (void)this_val; (void)argc; (void)argv;
    /* strip a trailing '/' (but keep a bare "/") */
    while (len > 1 && t[len - 1] == '/')
        len--;
    return dyn_path_new_from(ctx, t, len);
}

/* Shared mkdtemp/mkstemp template builder: "<tmp>/<prefix>XXXXXX". */
static char *dyn_fs_temp_template(const char *prefix)
{
    const char *t = dyn_fs_tempdir_str();
    size_t tl = strlen(t), pl = prefix ? strlen(prefix) : 0;
    char *tpl;
    while (tl > 1 && t[tl - 1] == '/')
        tl--;
    tpl = (char *)malloc(tl + 1 + pl + 6 + 1);
    if (!tpl)
        return NULL;
    memcpy(tpl, t, tl);
    tpl[tl] = '/';
    if (pl)
        memcpy(tpl + tl + 1, prefix, pl);
    memcpy(tpl + tl + 1 + pl, "XXXXXX", 6);
    tpl[tl + 1 + pl + 6] = '\0';
    return tpl;
}

static JSValue dyn_fs_make_temp_dir(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *prefix = NULL;
    char *tpl;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    tpl = dyn_fs_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    if (!mkdtemp(tpl)) {
        int e = errno;
        free(tpl);
        return dyn_fs_throw(ctx, e, "makeTempDir", NULL);
    }
    /* Resolve ONCE, here. The system temp dir sits under a symlink on macOS
       (/var -> /private/var), and every operation on a Path is strict: it
       refuses a symlink component. Returning the raw template would hand back
       a path this module then refuses to open. This is the deliberate
       boundary resolve; everything after it is symlink-free. */
    {
        char rp[PATH_MAX];
        if (realpath(tpl, rp)) {
            free(tpl);
            tpl = strdup(rp);
            if (!tpl)
                return JS_ThrowOutOfMemory(ctx);
        }
    }
    out = dyn_path_new_from(ctx, tpl, strlen(tpl));
    free(tpl);
    return out;
}

static JSValue dyn_fs_make_temp_file(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *prefix = NULL;
    char *tpl;
    int fd;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    tpl = dyn_fs_temp_template(prefix ? prefix : "tmp");
    if (prefix)
        JS_FreeCString(ctx, prefix);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    fd = mkstemp(tpl);
    if (fd < 0) {
        int e = errno;
        free(tpl);
        return dyn_fs_throw(ctx, e, "makeTempFile", NULL);
    }
    close(fd); /* leave an empty file, return its path */
    out = dyn_path_new_from(ctx, tpl, strlen(tpl));
    free(tpl);
    return out;
}

/* ==================================================================== *
 *  async content I/O -- a TWO-STRATEGY portfolio, selected per call     *
 * ==================================================================== *
 *
 * Handing work to a pool thread costs a hop (measured below); a page-cache
 * hit is ~200ns. So neither "always inline" nor "always offload" is right:
 *
 *   INLINE  small payloads -- do it on the loop thread and settle. The hop
 *           would cost more than the read.
 *   OFFLOAD large payloads -- dyn_aio_offload, so the loop keeps serving.
 *
 * The read gate needs the size, which costs a stat(2); the write gate gets it
 * free from the payload. Both thresholds are MEASURED, not guessed -- see
 * tests/bench_file_async.js and the constants below.
 *
 * Needs dyna:net, which owns the shared reactor. Without it the async
 * functions are NOT EXPORTED, rather than exported and blocking.
 */
#if defined(CONFIG_NATIVE_MODULE_NET)

/* Defined below with the other byte helpers; used here by file_job_done. */
static JSValue dyn_file_bytes_to_u8(JSContext *ctx, const uint8_t *data, size_t n);

#define FILE_OP_READ   0
#define FILE_OP_WRITE  1

/* MEASURED, and the threshold is a LOOP-BLOCK BUDGET, not a throughput
 * crossover -- offload never wins on throughput for a page-cache hit.
 *
 * Cost of the hop is roughly FIXED at ~190-275us (a full loop turn: worker,
 * wake fd, drain, settle), so as a ratio it is 10.2x at 64 KiB, 2.95x at
 * 1 MiB, 1.06x at 8 MiB. What it buys is the loop: 30 x 8 MiB sync fired a
 * 1 ms timer ZERO times in 31 ms; async fired it 29 times for 33 ms.
 *
 * So the gate asks "would the sync read stall the loop long enough to matter",
 * and 1 MiB is where the sync cost (~110us) reaches that. Below it, a caller
 * pays 190us+ to avoid a 20-40us stall, which is a tax.
 * Sync cost by size: 20us@64K, 25us@128K, 40us@256K, 110us@1M, 395us@4M. */
#ifndef DYN_FILE_ASYNC_READ_MIN
#define DYN_FILE_ASYNC_READ_MIN   (1024u * 1024u)
#endif
#ifndef DYN_FILE_ASYNC_WRITE_MIN
#define DYN_FILE_ASYNC_WRITE_MIN  (1024u * 1024u)
#endif

/* Which arm ran, per process. A portfolio whose selection cannot be observed
 * is one that silently stops picking the fast arm and nothing ever says so.
 * Declared above the FileWriter section, which shares them. */

/* RELEASING THE REACTOR FROM A COMPLETION IS A USE-AFTER-FREE. The completion
 * runs inside dyn_pool_drain's walk of the channel; if it drops the last
 * reference, dyn_aio_free frees that very channel and the walk resumes on
 * freed memory (caught by ASan, stack: dyn_pool_drain -> aio_offload_done ->
 * file_job_done -> dyn_net_reactor_release -> dyn_aio_free). So a completed
 * job only COUNTS its release, and a post-drain hook performs it -- which is
 * the one point the reactor is not iterating anything. */
static void file_async_reap(void *unused)
{
    (void)unused;
    while (file_async_pending_release > 0 && file_async_ctx) {
        file_async_pending_release--;
        dyn_net_reactor_release(file_async_ctx);
    }
    /* Unhook when idle: a registered hook arms a periodic wakeup, which would
     * otherwise hold the loop open after the last read finished. */
    if (file_async_pending_release == 0 && file_async_hooked) {
        file_async_hooked = 0;
        dyn_net_off_drain(&file_async_hooked);
        file_async_ctx = NULL;
    }
}

typedef struct {
    JSContext *ctx;
    JSValue resolve, reject;
    char *path;                 /* owned copy: the borrow ends at submit */
    uint8_t *buf;               /* write: payload (owned). read: unused. */
    dyn_iobuf_t src;            /* read: the slurped file, freed on the loop */
    int have_src;
    size_t len;
    int op, append, as_bytes;
    int err;                    /* errno from the worker, 0 on success */
    int offloaded;              /* whether a reactor ref must be released */
} file_job_t;

static void file_job_free(file_job_t *j)
{
    if (j->have_src)
        dyn_iobuf_free(&j->src);
    free(j->path);
    free(j->buf);
    free(j);
}

/* WORKER THREAD. Touches nothing but `j`, and calls NO JS_* function. */
static void file_job_work(void *arg)
{
    file_job_t *j = (file_job_t *)arg;
    if (j->op == FILE_OP_READ) {
        /* Hand the iobuf itself across and free it on the loop thread. An
         * intermediate malloc+memcpy here would copy the whole file a second
         * time -- measured 2-10x worse than the sync path, which builds the JS
         * value straight off the mmap view. */
        if (dyn_io_slurp(j->path, &j->src, 0) < 0) {
            j->err = errno ? errno : EIO;
            return;
        }
        j->have_src = 1;
        j->len = dyn_iobuf_rlen(&j->src);
    } else {
        int flags = O_WRONLY | O_CREAT | (j->append ? O_APPEND : O_TRUNC);
        int fd = open(j->path, flags, 0644);
        size_t off = 0;
        if (fd < 0) { j->err = errno ? errno : EIO; return; }
        while (off < j->len) {
            ssize_t n = write(fd, j->buf + off, j->len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                j->err = errno ? errno : EIO;
                break;
            }
            off += (size_t)n;
        }
        close(fd);
    }
}

/* LOOP THREAD. Builds the JS value and settles; JS is safe here. */
static void file_job_done(void *arg)
{
    file_job_t *j = (file_job_t *)arg;
    JSContext *ctx = j->ctx;
    JSValue v, r;
    JSValueConst a1[1];

    if (j->err) {
        v = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, v, "message",
                          JS_NewString(ctx, strerror(j->err)));
        JS_SetPropertyStr(ctx, v, "errno", JS_NewInt32(ctx, j->err));
        JS_SetPropertyStr(ctx, v, "path", JS_NewString(ctx, j->path));
        a1[0] = v;
        r = JS_Call(ctx, j->reject, JS_UNDEFINED, 1, a1);
    } else {
        const uint8_t *p = j->have_src ? dyn_iobuf_rdata(&j->src) : j->buf;
        if (j->op == FILE_OP_WRITE)
            v = JS_NewInt64(ctx, (int64_t)j->len);
        else if (j->as_bytes)
            v = dyn_file_bytes_to_u8(ctx, p, j->len);
        else
            v = JS_NewStringLen(ctx, (const char *)p, j->len);
        a1[0] = v;
        r = JS_Call(ctx, j->resolve, JS_UNDEFINED, 1, a1);
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, j->resolve);
    JS_FreeValue(ctx, j->reject);
    if (j->offloaded)
        file_async_pending_release++;   /* the hook releases; see above */
    file_job_free(j);
}

/* Run it here and settle on this turn's microtask drain. The op never reaches
 * a thread, so it never holds the reactor either. */
static void file_job_inline(file_job_t *j)
{
    file_n_inline++;
    file_job_work(j);
    file_job_done(j);
}

/* {inline, offloaded, readMin, writeMin} -- the counters and the thresholds
 * that produced them, so a test can assert WHICH arm ran. */
static JSValue dyn_file_async_stats(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue o = JS_NewObject(ctx);
    (void)this_val; (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, o, "inline", JS_NewInt64(ctx, (int64_t)file_n_inline));
    JS_SetPropertyStr(ctx, o, "offloaded", JS_NewInt64(ctx, (int64_t)file_n_offload));
    JS_SetPropertyStr(ctx, o, "readMin", JS_NewInt64(ctx, DYN_FILE_ASYNC_READ_MIN));
    JS_SetPropertyStr(ctx, o, "writeMin", JS_NewInt64(ctx, DYN_FILE_ASYNC_WRITE_MIN));
    return o;
}

static JSValue file_async_submit(JSContext *ctx, file_job_t *j, int offload)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    struct dyn_aio *aio;

    if (JS_IsException(promise)) { file_job_free(j); return promise; }
    j->ctx = ctx;
    j->resolve = funcs[0];
    j->reject = funcs[1];

    if (!offload) { file_job_inline(j); return promise; }

    /* The ref is what keeps the loop alive across the hop; the reap hook
     * drops it once the completion has run and the drain is over. */
    aio = dyn_net_reactor_acquire(ctx);
    if (!aio) { file_job_inline(j); return promise; }
    if (!file_async_hooked && dyn_net_on_drain(file_async_reap,
                                               &file_async_hooked) >= 0) {
        file_async_hooked = 1;
        file_async_ctx = ctx;
    }
    j->offloaded = 1;
    /* 1 = the pool refused and it ran inline; the completion still fired, so
     * only the counter can tell the two apart. */
    if (dyn_aio_offload(aio, file_job_work, file_job_done, j) == 0)
        file_n_offload++;
    else
        file_n_inline++;
    return promise;
}

static JSValue dyn_file_read_file_async(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    const char *path;
    file_job_t *j;
    struct stat st;
    int offload;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "readFileAsync(path[, options])");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    j = (file_job_t *)calloc(1, sizeof(*j));
    if (!j) { dyn_path_unborrow(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    j->op = FILE_OP_READ;
    j->path = strdup(path);
    /* stat IS the strategy predicate: it is one cheap syscall against a hop
     * that costs several, and a miss here only picks the slower arm. */
    offload = (stat(path, &st) == 0 && st.st_size >= (off_t)DYN_FILE_ASYNC_READ_MIN);
    dyn_path_unborrow(ctx, path);
    if (!j->path) { file_job_free(j); return JS_ThrowOutOfMemory(ctx); }
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "bytes");
        j->as_bytes = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    return file_async_submit(ctx, j, offload);
}

static JSValue dyn_file_write_file_async(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    const char *path, *owned = NULL;
    const uint8_t *data = NULL;
    size_t len = 0;
    file_job_t *j;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFileAsync(path, data[, options])");
    /* Coerce EVERYTHING before anything is allocated: a toString hook runs
     * arbitrary JS that can close handles this call depends on. */
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (dyn_file_payload(ctx, argv[1], &data, &len, &owned)) {
        dyn_path_unborrow(ctx, path);
        return JS_EXCEPTION;
    }
    j = (file_job_t *)calloc(1, sizeof(*j));
    if (j) {
        j->op = FILE_OP_WRITE;
        j->path = strdup(path);
        j->len = len;
        j->buf = (uint8_t *)malloc(len ? len : 1);
        /* The payload must be COPIED: `data` borrows a JS string or view that
         * the collector may move or free the moment this returns. */
        if (j->buf && len)
            memcpy(j->buf, data, len);
    }
    if (argc > 2 && JS_IsObject(argv[2]) && j) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "append");
        j->append = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    dyn_path_unborrow(ctx, path);
    if (!j || !j->path || !j->buf) {
        if (j) file_job_free(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    return file_async_submit(ctx, j, len >= DYN_FILE_ASYNC_WRITE_MIN);
}

#endif /* CONFIG_NATIVE_MODULE_NET */

/* copyFile, move and content sniffing. */
#include "dyna-filecopy.inc.c"
#include "dyna-watch.inc.c"   /* Watcher: reuses dyn_glob_match above */

static const JSCFunctionListEntry dyn_file_funcs[] = {
    /* content I/O */
    JS_CFUNC_DEF("readFile", 1, dyn_file_read_file),
    JS_CFUNC_DEF("writeFile", 2, dyn_file_write_file),
#if defined(CONFIG_NATIVE_MODULE_NET)
    JS_CFUNC_DEF("readFileAsync", 1, dyn_file_read_file_async),
    JS_CFUNC_DEF("writeFileAsync", 2, dyn_file_write_file_async),
    JS_CFUNC_DEF("asyncStats", 0, dyn_file_async_stats),
#endif
    /* metadata */
    JS_CFUNC_DEF("stat", 1, dyn_fs_stat),
    JS_CFUNC_DEF("lstat", 1, dyn_fs_lstat),
    JS_CFUNC_DEF("exists", 1, dyn_fs_exists),
    /* directories */
    JS_CFUNC_DEF("readDir", 1, dyn_fs_read_dir),
    JS_CFUNC_DEF("makeDir", 2, dyn_fs_make_dir),
    JS_CFUNC_DEF("remove", 1, dyn_fs_remove),
    JS_CFUNC_DEF("removeAll", 1, dyn_fs_remove_all),
    JS_CFUNC_DEF("rename", 2, dyn_fs_rename),
    JS_CFUNC_DEF("copyFile", 3, dyn_fs_copy_file),
    JS_CFUNC_DEF("move", 2, dyn_fs_move),
    JS_CFUNC_DEF("sniffType", 1, dyn_fs_sniff_type),
    /* links / perms */
    JS_CFUNC_DEF("symlink", 2, dyn_fs_symlink),
    JS_CFUNC_DEF("readLink", 1, dyn_fs_read_link),
    JS_CFUNC_DEF("realPath", 1, dyn_fs_real_path),
    JS_CFUNC_DEF("chmod", 2, dyn_fs_chmod),
    /* globbing */
    JS_CFUNC_DEF("glob", 2, dyn_fs_glob),
    /* temp */
    JS_CFUNC_DEF("tempDir", 0, dyn_fs_temp_dir),
    JS_CFUNC_DEF("makeTempDir", 1, dyn_fs_make_temp_dir),
    JS_CFUNC_DEF("makeTempFile", 1, dyn_fs_make_temp_file),
};

/* ==================================================================== *
 *  module registration                                                  *
 * ==================================================================== */


/* ==================================================================== *
 *  File -- the handle over ONE path, and Glob -- a compiled pattern
 *
 *  File is a VALUE HANDLE like Path: constructed with the datum, not with
 *  configuration. It exists because "one path, many operations" is the
 *  shape almost every caller actually has, and writing the Path once is
 *  both shorter and impossible to get inconsistent.
 *
 *  It delegates to the free functions rather than reimplementing them, so
 *  there is one implementation of readFile and one of stat. The whole
 *  class is a rebinding of the argument, not a second code path -- which
 *  is what lets the cost gate assert it is free.
 *
 *  Glob, by contrast, IS a compiled capability: the pattern is the
 *  configuration and it is matched against unbounded paths. Its crossover
 *  is published with its losing row like every other one.
 * ==================================================================== */

typedef struct {
    JSValue path;      /* a Path handle -- File holds it, hence the gc_mark */
} dyn_fh_t;

static JSClassID dyn_fh_class_id;

static void dyn_fh_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_fh_t *f = (dyn_fh_t *)JS_GetOpaque(val, dyn_fh_class_id);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->path);
    free(f);
}

static void dyn_fh_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    dyn_fh_t *f = (dyn_fh_t *)JS_GetOpaque(val, dyn_fh_class_id);
    if (f)
        JS_MarkValue(rt, f->path, mark_func);
}

static const JSClassDef dyn_fh_class = {
    "File", .finalizer = dyn_fh_finalizer, .gc_mark = dyn_fh_mark,
};

static JSValue dyn_fh_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    dyn_fh_t *f;
    JSValue obj, path;

    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new File(path) requires a Path");
    /* A string is accepted here and NOWHERE else, because `new File("x")` is
     * the one place the intent is unambiguous -- it is a constructor whose
     * whole subject is the path. It builds the Path for you rather than
     * making you write new File(new Path(s)). */
    if (dyn_is_path(argv[0])) {
        path = JS_DupValue(ctx, argv[0]);
    } else if (JS_IsString(argv[0])) {
        path = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(path))
            return path;
    } else {
        return JS_ThrowTypeError(ctx, "new File(path): path must be a Path or a string");
    }

    f = (dyn_fh_t *)calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->path = path;
    obj = JS_NewObjectClass(ctx, dyn_fh_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, path);
        free(f);
        return obj;
    }
    JS_SetOpaque(obj, f);
    return obj;
}

static dyn_fh_t *dyn_fh_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_fh_t *)JS_GetOpaque2(ctx, v, dyn_fh_class_id);
}

/* Every method is the corresponding free function with `this`'s Path pushed in
 * front of the arguments. One implementation, one place for a bug to be. */
#define DYN_FH_FORWARD(name, fn, maxargs)                                     \
    static JSValue name(JSContext *ctx, JSValueConst this_val, int argc,      \
                        JSValueConst *argv)                                   \
    {                                                                         \
        dyn_fh_t *f = dyn_fh_of(ctx, this_val);                               \
        JSValueConst a[(maxargs) + 1];                                        \
        int i;                                                                \
        if (!f)                                                               \
            return JS_EXCEPTION;                                              \
        a[0] = f->path;                                                       \
        for (i = 0; i < (maxargs); i++)                                       \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;                   \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                       \
    }

/* Build a fresh Uint8Array from raw bytes. Copies -- no native pointer escapes
 * into a JS value, which is the standing rule for every binding here. */
static JSValue dyn_file_bytes_to_u8(JSContext *ctx, const uint8_t *data, size_t n)
{
    JSValue ab, global, ctor, out;
    JSValueConst args[1];

    ab = JS_NewArrayBufferCopy(ctx, data, n);
    if (JS_IsException(ab))
        return ab;
    global = JS_GetGlobalObject(ctx);
    ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);          /* JS_GetGlobalObject returns a REF */
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, ab);
        return ctor;
    }
    args[0] = ab;
    out = JS_CallConstructor(ctx, ctor, 1, args);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, ab);
    return out;
}

DYN_FH_FORWARD(dyn_fh_read_text, dyn_file_read_file, 0)

/* readBytes: the binary half. It reads the file directly rather than going
 * through readText, and that is not a style preference -- readText builds a JS
 * STRING, so any byte that is not valid UTF-8 becomes U+FFFD and cannot be
 * recovered. Measured: writing 1,2,255,0,3 and reading it back through the
 * string path returned SEVEN bytes (1,2,239,191,189,0,3), silently corrupting
 * the payload. That is defect D-1's shape all over again, in the opposite
 * direction. */
static JSValue dyn_fh_read_bytes(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    const char *path;
    dyn_iobuf_t src;
    JSValue out;

    (void)argc; (void)argv;
    if (!f)
        return JS_EXCEPTION;
    path = dyn_path_arg(ctx, f->path, "path");
    if (!path)
        return JS_EXCEPTION;
    if (dyn_io_slurp(path, &src, 0) < 0) {
        dyn_path_unborrow(ctx, path);
        return JS_ThrowInternalError(ctx, "readBytes: cannot read file");
    }
    dyn_path_unborrow(ctx, path);
    out = dyn_file_bytes_to_u8(ctx, dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return out;
}

DYN_FH_FORWARD(dyn_fh_write_text, dyn_file_write_file, 2)
DYN_FH_FORWARD(dyn_fh_stat, dyn_fs_stat, 0)
DYN_FH_FORWARD(dyn_fh_lstat, dyn_fs_lstat, 0)
DYN_FH_FORWARD(dyn_fh_exists, dyn_fs_exists, 0)
DYN_FH_FORWARD(dyn_fh_remove, dyn_fs_remove, 0)
DYN_FH_FORWARD(dyn_fh_real_path, dyn_fs_real_path, 0)
DYN_FH_FORWARD(dyn_fh_chmod, dyn_fs_chmod, 1)

/* append(data) is writeFile with {append:true} supplied here, so a caller
 * cannot spell the common case wrong. */
static JSValue dyn_fh_append(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    JSValueConst a[3];
    JSValue opts, r;

    if (!f)
        return JS_EXCEPTION;
    opts = JS_NewObject(ctx);
    if (JS_IsException(opts))
        return opts;
    JS_SetPropertyStr(ctx, opts, "append", JS_NewBool(ctx, 1));
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    a[2] = opts;
    r = dyn_file_write_file(ctx, JS_UNDEFINED, 3, a);
    JS_FreeValue(ctx, opts);
    return r;
}

/* moveTo/copyTo take a destination Path. copyTo reads and writes rather than
 * calling link(2): a copy that silently became a hard link would share writes
 * with the original, which is not what "copy" means anywhere. */
static JSValue dyn_fh_move_to(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];
    JSValue r;

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "moveTo(dest) requires a Path");
    a[0] = f->path;
    a[1] = argv[0];
    r = dyn_fs_rename(ctx, JS_UNDEFINED, 2, a);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    /* The handle now names the new location, which is what a caller means by
     * moving a file they hold. */
    JS_FreeValue(ctx, f->path);
    f->path = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_fh_copy_to(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];
    JSValue data, r;

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "copyTo(dest) requires a Path");
    /* readBytes, NOT readFile. readFile returns a JS STRING, and an invalid
     * UTF-8 sequence becomes U+FFFD on the way in -- so copying a PNG, a wasm
     * module or a gzip blob succeeded and produced a plausible file that was
     * not the original. The same defect was found and fixed in readBytes; this
     * caller still went through the string path. It also held the whole file
     * as a string, so peak RSS was about 3x the file. */
    (void)a;
    data = dyn_fh_read_bytes(ctx, this_val, 0, NULL);
    if (JS_IsException(data))
        return data;
    a[0] = argv[0];
    a[1] = data;
    r = dyn_file_write_file(ctx, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, data);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    {
        JSValueConst one[1];
        one[0] = argv[0];
        return dyn_fh_ctor(ctx, JS_UNDEFINED, 1, one);
    }
}

/* reader/writer construct the buffered streams over this handle's path. */
static JSValue dyn_fh_stream(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];

    if (!f)
        return JS_EXCEPTION;
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    return magic ? dyn_fwriter_ctor(ctx, JS_UNDEFINED, 2, a)
                 : dyn_freader_ctor(ctx, JS_UNDEFINED, 2, a);
}

static JSValue dyn_fh_get_path(JSContext *ctx, JSValueConst this_val)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, f->path);
}

static JSValue dyn_fh_to_string(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    dyn_fh_t *f = dyn_fh_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!f)
        return JS_EXCEPTION;
    return dyn_path_get_str(ctx, f->path);
}

static const JSCFunctionListEntry dyn_fh_proto[] = {
    JS_CGETSET_DEF("path", dyn_fh_get_path, NULL),
    JS_CFUNC_DEF("readText", 0, dyn_fh_read_text),
    JS_CFUNC_DEF("readBytes", 0, dyn_fh_read_bytes),
    JS_CFUNC_DEF("writeText", 2, dyn_fh_write_text),
    /* writeBytes IS writeText: dyn_file_write_file already accepts a string,
     * an ArrayBuffer or any TypedArray/DataView (that was defect D-1). A
     * separate entry point would be a second name for one function, so this is
     * an alias and says so rather than a copy. */
    JS_CFUNC_DEF("writeBytes", 2, dyn_fh_write_text),
    JS_CFUNC_DEF("append", 1, dyn_fh_append),
    JS_CFUNC_DEF("stat", 0, dyn_fh_stat),
    JS_CFUNC_DEF("lstat", 0, dyn_fh_lstat),
    JS_CFUNC_DEF("exists", 0, dyn_fh_exists),
    JS_CFUNC_DEF("remove", 0, dyn_fh_remove),
    JS_CFUNC_DEF("realPath", 0, dyn_fh_real_path),
    JS_CFUNC_DEF("chmod", 1, dyn_fh_chmod),
    JS_CFUNC_DEF("moveTo", 1, dyn_fh_move_to),
    JS_CFUNC_DEF("copyTo", 1, dyn_fh_copy_to),
    JS_CFUNC_MAGIC_DEF("reader", 1, dyn_fh_stream, 0),
    JS_CFUNC_MAGIC_DEF("writer", 1, dyn_fh_stream, 1),
    JS_CFUNC_DEF("toString", 0, dyn_fh_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_fh_to_string),
};

/* ---- Glob: a compiled pattern ------------------------------------------ */

typedef struct {
    char *pattern;
    size_t len;
    int has_wildcard;
} dyn_gl_t;

static JSClassID dyn_gl_class_id;

static void dyn_gl_dispose(void *native)
{
    dyn_gl_t *g = (dyn_gl_t *)native;
    if (!g)
        return;
    free(g->pattern);
    free(g);
}

static void dyn_gl_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_gl_dispose(JS_GetOpaque(val, dyn_gl_class_id));
}

static const JSClassDef dyn_gl_class = {
    "Glob", .finalizer = dyn_gl_finalizer,
};

static JSValue dyn_gl_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    dyn_gl_t *g;
    const char *pat;
    size_t plen;

    (void)new_target;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Glob(pattern) requires a string");
    pat = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    g = (dyn_gl_t *)calloc(1, sizeof(*g));
    if (!g) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    g->pattern = (char *)malloc(plen + 1);
    if (!g->pattern) {
        JS_FreeCString(ctx, pat);
        free(g);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(g->pattern, pat, plen);
    g->pattern[plen] = '\0';
    g->len = plen;
    /* Checked ONCE, at construction, because it is a property of the pattern
     * and not of any path -- the cheapest precondition is the one you can
     * check once (CLAUDE.md sec.15). */
    g->has_wildcard = dyn_glob_has_wildcard(pat);
    JS_FreeCString(ctx, pat);
    return dyn_plain_wrap(ctx, dyn_gl_class_id, g, dyn_gl_dispose);
}

static dyn_gl_t *dyn_gl_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_gl_t *)JS_GetOpaque2(ctx, v, dyn_gl_class_id);
}

static JSValue dyn_gl_matches(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_gl_t *g = dyn_gl_of(ctx, this_val);
    const char *path;

    if (!g)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "matches(path) requires a Path");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    /* Purely lexical: no filesystem access, so a pattern can be tested against
     * a path that does not exist. expand() is the one that walks the disk. */
    return JS_NewBool(ctx, dyn_glob_match(g->pattern, path));
}

static JSValue dyn_gl_expand(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_gl_t *g = dyn_gl_of(ctx, this_val);
    JSValueConst a[2];
    JSValue pat, opts, r;

    if (!g)
        return JS_EXCEPTION;
    pat = JS_NewStringLen(ctx, g->pattern, g->len);
    if (JS_IsException(pat))
        return pat;
    opts = JS_UNDEFINED;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        opts = JS_NewObject(ctx);
        if (JS_IsException(opts)) {
            JS_FreeValue(ctx, pat);
            return opts;
        }
        JS_SetPropertyStr(ctx, opts, "cwd", JS_DupValue(ctx, argv[0]));
    }
    a[0] = pat;
    a[1] = opts;
    r = dyn_fs_glob(ctx, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, pat);
    JS_FreeValue(ctx, opts);
    return r;
}

static JSValue dyn_gl_filter(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    dyn_gl_t *g = dyn_gl_of(ctx, this_val);
    JSValue out;
    int64_t len = 0, i;
    uint32_t k = 0;

    if (!g)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "filter(paths[]) requires an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        const char *path;
        if (JS_IsException(e)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        path = dyn_path_arg(ctx, e, "element");
        if (!path) { JS_FreeValue(ctx, e); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        if (dyn_glob_match(g->pattern, path))
            JS_DefinePropertyValueUint32(ctx, out, k++, e, JS_PROP_C_W_E);
        else
            JS_FreeValue(ctx, e);
    }
    return out;
}

static JSValue dyn_gl_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_gl_t *g = dyn_gl_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewStringLen(ctx, g->pattern, g->len);
    return JS_NewBool(ctx, g->has_wildcard);
}

static const JSCFunctionListEntry dyn_gl_proto[] = {
    JS_CFUNC_DEF("matches", 1, dyn_gl_matches),
    JS_CFUNC_DEF("expand", 1, dyn_gl_expand),
    JS_CFUNC_DEF("filter", 1, dyn_gl_filter),
    JS_CGETSET_MAGIC_DEF("pattern", dyn_gl_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("hasWildcard", dyn_gl_get, NULL, 1),
};

/* Path is registered by hand rather than through dyn_register_plain_class,
 * because it is the one class here with STATICS -- cwd/home/temp/isPath/sep/
 * delimiter -- and that helper exports the constructor before a caller can
 * decorate it. `sep` and `delimiter` land here rather than as module exports:
 * they were module-level constants of the retired `dyna:path`, and a separator
 * is a property of the path grammar, so the grammar's type is where it goes. */
static int dyn_register_path_class(JSContext *ctx, JSModuleDef *m)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    char sep[2] = { DYN_PATH_SEP, 0 };
    char delim[2] = { DYN_PATH_DELIM, 0 };

    JS_NewClassID(&dyn_path_class_id);
    if (JS_NewClass(rt, dyn_path_class_id, &dyn_path_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_path_proto,
                               (int)countof(dyn_path_proto));
    JS_SetClassProto(ctx, dyn_path_class_id, proto);

    ctor = JS_NewCFunction2(ctx, dyn_path_ctor, "Path", 1,
                            JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPropertyStr(ctx, ctor, "cwd",
                      JS_NewCFunctionMagic(ctx, dyn_path_static, "cwd", 0,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, ctor, "home",
                      JS_NewCFunctionMagic(ctx, dyn_path_static, "home", 0,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, ctor, "temp",
                      JS_NewCFunctionMagic(ctx, dyn_path_static, "temp", 0,
                                           JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, ctor, "isPath",
                      JS_NewCFunctionMagic(ctx, dyn_path_static, "isPath", 1,
                                           JS_CFUNC_generic_magic, 3));
    JS_SetPropertyStr(ctx, ctor, "sep", JS_NewString(ctx, sep));
    JS_SetPropertyStr(ctx, ctor, "delimiter", JS_NewString(ctx, delim));
    return JS_SetModuleExport(ctx, m, "Path", ctor);
}

static int dyn_file_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_path_class(ctx, m) < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_fh_class_id, &dyn_fh_class,
                                 dyn_fh_proto, (int)countof(dyn_fh_proto),
                                 dyn_fh_ctor, "File") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_gl_class_id, &dyn_gl_class,
                                 dyn_gl_proto, (int)countof(dyn_gl_proto),
                                 dyn_gl_ctor, "Glob") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_freader_class_id, &dyn_freader_class,
                           dyn_freader_proto, countof(dyn_freader_proto),
                           dyn_freader_ctor, "FileReader") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_fwriter_class_id, &dyn_fwriter_class,
                           dyn_fwriter_proto, countof(dyn_fwriter_proto),
                           dyn_fwriter_ctor, "FileWriter") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_watch_class_id, &dyn_watch_class,
                           dyn_watch_proto, countof(dyn_watch_proto),
                           dyn_watch_ctor, "Watcher") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_file_funcs,
                                  (int)countof(dyn_file_funcs));
}

int js_nat_init_file(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:file", dyn_file_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Path");
    JS_AddModuleExport(ctx, m, "File");
    JS_AddModuleExport(ctx, m, "Glob");
    JS_AddModuleExport(ctx, m, "FileReader");
    JS_AddModuleExport(ctx, m, "FileWriter");
    JS_AddModuleExport(ctx, m, "Watcher");
    JS_AddModuleExportList(ctx, m, dyn_file_funcs, (int)countof(dyn_file_funcs));
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_FILE */
