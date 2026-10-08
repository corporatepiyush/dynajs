#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#include "dyna-utf8-lossy.inc.c"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#endif

#include "core/dyn-path.h"

#define DYN_PATH_STACK_SCRATCH 1024

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    char* buf;
    size_t len;
    dyn_path_split_t split;
    int refs;
} dyn_path_rec_t;

static JSClassID dyn_path_class_id;

static void dyn_path_rec_unref(dyn_path_rec_t* p)
{
    if (!p)
        return;
    if (--p->refs > 0)
        return;
    free(p->buf);
    free(p);
}

static void dyn_path_dispose(void* native)
{
    dyn_path_rec_unref((dyn_path_rec_t*)native);
}

static void dyn_path_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_path_rec_unref((dyn_path_rec_t*)JS_GetOpaque(val, dyn_path_class_id));
}

static const JSClassDef dyn_path_class = {
    "Path",
    .finalizer = dyn_path_finalizer,
};

static dyn_path_rec_t* dyn_path_rec_adopt(char* buf, size_t len)
{
    dyn_path_rec_t* p = (dyn_path_rec_t*)calloc(1, sizeof(*p));
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

static dyn_path_rec_t* dyn_path_rec_from(const char* src, size_t n)
{
    char* buf = (char*)malloc(dyn_path_cstr_cap(dyn_path_normalize_cap(n)));
    size_t len;
    if (!buf)
        return NULL;
    len = dyn_path_normalize(src, n, buf);
    buf[len] = '\0';
    return dyn_path_rec_adopt(buf, len);
}

static JSValue dyn_path_wrap_rec(JSContext* ctx, JSValueConst new_target,
    dyn_path_rec_t* p)
{
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_plain_wrap(ctx, new_target, dyn_path_class_id, p, dyn_path_dispose);
}

static JSValue dyn_path_share(JSContext* ctx, JSValueConst new_target,
    dyn_path_rec_t* p)
{
    p->refs++;
    return dyn_path_wrap_rec(ctx, new_target, p);
}

static dyn_path_rec_t* dyn_path_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_path_rec_t*)JS_GetOpaque2(ctx, v, dyn_path_class_id);
}

static int dyn_is_path(JSValueConst v)
{
    return JS_GetOpaque(v, dyn_path_class_id) != NULL;
}

static const char* dyn_path_arg(JSContext* ctx, JSValueConst v, const char* what)
{
    dyn_path_rec_t* p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "dyna:file: %s must be a Path -- wrap it with "
                               "new Path(...)",
            what);
        return NULL;
    }
    p = (dyn_path_rec_t*)JS_GetOpaque(v, dyn_path_class_id);
    return p->buf;
}

static const char* dyn_path_arg_len(JSContext* ctx, JSValueConst v,
    const char* what, size_t* plen)
{
    dyn_path_rec_t* p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "dyna:file: %s must be a Path -- wrap it with "
                               "new Path(...)",
            what);
        return NULL;
    }
    p = (dyn_path_rec_t*)JS_GetOpaque(v, dyn_path_class_id);
    *plen = p->len;
    return p->buf;
}

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static int dyn_openat_parent(const char* path, const char** leaf)
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
        if (!slash) {
            *leaf = p;
            return dirfd;
        }
        {
            size_t len = (size_t)(slash - p);
            char comp[PATH_MAX];
            if (len == 0 || len >= sizeof(comp)) {
                if (dirfd != AT_FDCWD)
                    close(dirfd);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(comp, p, len);
            comp[len] = '\0';
            next = openat(dirfd, comp,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | ((comp[0] == '.' && (comp[1] == '\0' || (comp[1] == '.' && comp[2] == '\0'))) ? 0 : O_NOFOLLOW));
            if (next < 0) {
                int saved = errno;
                struct stat lst;
                if (fstatat(dirfd, comp, &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode))
                    saved = ELOOP;
                if (dirfd != AT_FDCWD)
                    close(dirfd);
                errno = saved;
                return -1;
            }
            if (dirfd != AT_FDCWD)
                close(dirfd);
            dirfd = next;
        }
        p = slash + 1;
    }
}

static int dyn_open_strict(const char* path, int flags, mode_t mode)
{
    const char* leaf = NULL;
    int dirfd, fd, saved;

    dirfd = dyn_openat_parent(path, &leaf);
    if (dirfd < 0)
        return -1;
    if (!*leaf) {
        if (dirfd == AT_FDCWD)
            return open(".", flags | O_NOFOLLOW, mode);
        return dirfd;
    }
    fd = openat(dirfd, leaf, flags | O_NOFOLLOW, mode);
    saved = errno;
    if (dirfd != AT_FDCWD)
        close(dirfd);
    errno = saved;
    return fd;
}

static int dyn_stat_strict(const char* path, struct stat* st, int follow)
{
    const char* leaf = NULL;
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

static const char* dyn_fs_errno_code(int e)
{
    switch (e) {
    case ENOENT:
        return "ENOENT";
    case EACCES:
        return "EACCES";
    case EWOULDBLOCK:
        return "EWOULDBLOCK";
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
    case EIO:
        return "EIO";
    case ENOTSOCK:
        return "ENOTSOCK";
    default:
        return NULL;
    }
}

static JSValue dyn_fs_throw(JSContext* ctx, int e, const char* op,
    const char* path)
{
    JSValue err;
    char msg[PATH_MAX + 128];
    const char* code = dyn_fs_errno_code(e);

    if (path) {
        char epath[PATH_MAX * 4 + 16];
        dyn_esc_ctrl(epath, sizeof(epath), path);
        snprintf(msg, sizeof(msg), "file.%s(\"%s\"): %s", op, epath,
            strerror(e));
    } else {
        snprintf(msg, sizeof(msg), "file.%s: %s", op, strerror(e));
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

const char* dyn_path_borrow(JSContext* ctx, JSValueConst v, const char* what,
    size_t* plen)
{
    dyn_path_rec_t* p;
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "%s must be a Path -- wrap it with "
                               "new Path(...) from dyna:file",
            what);
        return NULL;
    }
    p = (dyn_path_rec_t*)JS_GetOpaque(v, dyn_path_class_id);
    if (plen)
        *plen = p->len;
    return p->buf;
}

int dyn_value_is_path(JSValueConst v)
{
    return dyn_is_path(v);
}

static size_t dyn_path_resolve_prefix(const char* s, size_t n, char* dst)
{
    char buf[PATH_MAX], real[PATH_MAX];
    const char* slash;
    size_t plen, rl, tl;

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
        tl = n - plen;
        if (rl + tl >= PATH_MAX)
            return 0;
        memcpy(dst, real, rl);
        memcpy(dst + rl, s + plen, tl);
        dst[rl + tl] = '\0';
        return rl + tl;
    }
    if (realpath(buf, real)) {
        rl = strlen(real);
        if (rl >= PATH_MAX)
            return 0;
        memcpy(dst, real, rl);
        dst[rl] = '\0';
        return rl;
    }
    return 0;
}

static JSValue dyn_path_new_from(JSContext* ctx, const char* s, size_t n)
{
    char res[PATH_MAX];
    size_t rn = dyn_path_resolve_prefix(s, n, res);
    if (rn)
        return dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_from(res, rn));
    return dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_from(s, n));
}

static inline void dyn_path_unborrow(JSContext* ctx, const char* p)
{
    (void)ctx;
    (void)p;
}

typedef struct {
    const char** ptr;
    size_t* len;
    const char** owned;
    int n, n_owned;
} dyn_seg_list_t;

static void dyn_segs_free(JSContext* ctx, dyn_seg_list_t* s)
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

static int dyn_segs_collect(JSContext* ctx, int argc, JSValueConst* argv,
    dyn_seg_list_t* s)
{
    int i;
    memset(s, 0, sizeof(*s));
    if (argc <= 0)
        return 0;
    s->ptr = (const char**)calloc((size_t)argc, sizeof(*s->ptr));
    s->len = (size_t*)calloc((size_t)argc, sizeof(*s->len));
    s->owned = (const char**)calloc((size_t)argc, sizeof(*s->owned));
    if (!s->ptr || !s->len || !s->owned) {
        dyn_segs_free(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < argc; i++) {
        if (dyn_is_path(argv[i])) {
            dyn_path_rec_t* p = (dyn_path_rec_t*)JS_GetOpaque(argv[i], dyn_path_class_id);
            s->ptr[i] = p->buf;
            s->len[i] = p->len;
        } else if (JS_IsString(argv[i])) {
            size_t sl;
            const char* cs = JS_ToCStringLen(ctx, &sl, argv[i]);
            if (!cs) {
                dyn_segs_free(ctx, s);
                return -1;
            }
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

static JSValue dyn_path_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_seg_list_t segs;
    dyn_path_rec_t* rec;
    char *out, *scratch;
    char stack_scratch[DYN_PATH_STACK_SCRATCH];
    size_t sum = 0, cap, len;
    int i;

    if (argc == 0)
        return JS_ThrowTypeError(ctx, "new Path(...segments) needs at least "
                                      "one segment");

    if (argc == 1 && dyn_is_path(argv[0])) {
        dyn_path_rec_t* p = (dyn_path_rec_t*)JS_GetOpaque(argv[0], dyn_path_class_id);
        return dyn_path_share(ctx, new_target, p);
    }

    if (dyn_segs_collect(ctx, argc, argv, &segs) < 0)
        return JS_EXCEPTION;

    for (i = 0; i < segs.n; i++)
        sum += segs.len[i];
    cap = dyn_path_join_cap(sum, (size_t)segs.n);

    out = (char*)malloc(dyn_path_cstr_cap(cap));
    scratch = (cap <= DYN_PATH_STACK_SCRATCH) ? stack_scratch
                                              : (char*)malloc(cap);
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

    {
        char res[PATH_MAX];
        size_t rn = dyn_path_resolve_prefix(out, len, res);
        if (rn) {
            char* rp = (char*)malloc(dyn_path_cstr_cap(rn));
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
    return dyn_path_wrap_rec(ctx, new_target, rec);
}

static JSValue dyn_path_get_str(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf, p->len);
}

static JSValue dyn_path_get_dirname(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    dyn_path_rec_t* d;
    char* buf;
    const char* src;
    size_t n;

    if (!p)
        return JS_EXCEPTION;
    if (p->split.dir_is_dot) {
        src = ".";
        n = 1;
    } else if (p->split.dir_is_root) {
        src = "/";
        n = 1;
    } else {
        src = p->buf + p->split.dir_off;
        n = p->split.dir_len;
    }

    buf = (char*)malloc(n + 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(buf, src, n);
    buf[n] = '\0';
    d = dyn_path_rec_adopt(buf, n);
    return dyn_path_wrap_rec(ctx, JS_UNDEFINED, d);
}

static JSValue dyn_path_get_basename(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.base_off, p->split.base_len);
}

static JSValue dyn_path_get_extname(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.ext_off, p->split.ext_len);
}

static JSValue dyn_path_get_is_absolute(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->split.is_absolute);
}

static JSValue dyn_path_compose(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_path_rec_t* self = dyn_path_of(ctx, this_val);
    dyn_seg_list_t segs;
    const char** ptr = NULL;
    size_t* len = NULL;
    char *out = NULL, *scratch = NULL;
    char stack_scratch[DYN_PATH_STACK_SCRATCH];
    size_t sum, cap, outlen;
    int i, n;
    JSValue res;

    if (!self)
        return JS_EXCEPTION;
    if (dyn_segs_collect(ctx, argc, argv, &segs) < 0)
        return JS_EXCEPTION;

    n = segs.n + 1;
    ptr = (const char**)calloc((size_t)n, sizeof(*ptr));
    len = (size_t*)calloc((size_t)n, sizeof(*len));
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
    out = (char*)malloc(dyn_path_cstr_cap(cap));
    scratch = (cap <= DYN_PATH_STACK_SCRATCH) ? stack_scratch
                                              : (char*)malloc(cap);
    if (!out || !scratch) {
        res = JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    outlen = magic ? dyn_path_resolve(ptr, len, (size_t)n, out, scratch)
                   : dyn_path_join(ptr, len, (size_t)n, out, scratch);
    out[outlen] = '\0';
    res = dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_adopt(out, outlen));
    out = NULL;

done:
    free(out);
    if (scratch != stack_scratch)
        free(scratch);
    free(ptr);
    free(len);
    dyn_segs_free(ctx, &segs);
    return res;
}

static JSValue dyn_path_relative_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_path_rec_t* self = dyn_path_of(ctx, this_val);
    dyn_path_rec_t* to;
    char *out, *scratch;
    size_t cap, len;
    JSValue res;

    if (!self)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_is_path(argv[0]))
        return JS_ThrowTypeError(ctx, "Path.relativeTo(other): other must be "
                                      "a Path");
    to = (dyn_path_rec_t*)JS_GetOpaque(argv[0], dyn_path_class_id);

    cap = dyn_path_relative_cap(self->len, to->len);
    out = (char*)malloc(dyn_path_cstr_cap(cap));
    scratch = (char*)malloc(cap);
    if (!out || !scratch) {
        free(out);
        free(scratch);
        return JS_ThrowOutOfMemory(ctx);
    }
    len = dyn_path_relative(self->buf, self->len, to->buf, to->len, out,
        scratch);
    free(scratch);
    if (len == 0) {
        free(out);
        return dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_from(".", 1));
    }
    out[len] = '\0';
    res = dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_adopt(out, len));
    return res;
}

static JSValue dyn_path_equals(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_path_rec_t* self = dyn_path_of(ctx, this_val);
    dyn_path_rec_t* o;
    if (!self)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_is_path(argv[0]))
        return JS_NewBool(ctx, 0);
    o = (dyn_path_rec_t*)JS_GetOpaque(argv[0], dyn_path_class_id);
    return JS_NewBool(ctx, self->len == o->len && memcmp(self->buf, o->buf, self->len) == 0);
}

static JSValue dyn_path_basename_without(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    const char* suf = NULL;
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

static JSValue dyn_path_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_path_get_str(ctx, this_val);
}

static JSValue dyn_path_dir_from(JSContext* ctx, const char* v)
{
    size_t n = strlen(v);
    while (n > 1 && v[n - 1] == DYN_PATH_SEP)
        n--;
    return dyn_path_new_from(ctx, v, n);
}

static JSValue dyn_path_static_from_env(JSContext* ctx, const char* var,
    const char* fallback)
{
    const char* v = getenv(var);
    if (!v || !*v)
        v = fallback;
    return dyn_path_dir_from(ctx, v);
}

static JSValue dyn_path_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    (void)this_val;
    switch (magic) {
    case 0: {
        char buf[PATH_MAX];
        if (!getcwd(buf, sizeof(buf)))
            return JS_ThrowInternalError(ctx, "Path.cwd(): %s", strerror(errno));
        return dyn_path_dir_from(ctx, buf);
    }
    case 1:
        return dyn_path_static_from_env(ctx, "HOME", "/");
    case 2:
        return dyn_path_static_from_env(ctx, "TMPDIR", "/tmp");
    default:
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
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, dyn_path_to_string),
};

#define DYN_FILE_DEFAULT_BUF (1u << 17)
#define DYN_FILE_MIN_BUF 4096u
#define DYN_FILE_MAX_BUF (1u << 26)

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

static JSClassID dyn_freader_class_id;

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t start;
    size_t end;
    int fd;
    int eof;
} dyn_freader_t;

_Static_assert(sizeof(dyn_freader_t) == 4 * sizeof(void*) + 8,
    "dyn_freader_t regained padding: keep the two ints adjacent");

static void dyn_freader_dispose(void* native)
{
    dyn_freader_t* r = (dyn_freader_t*)native;
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    free(r);
}

static const JSClassDef dyn_freader_class = {
    "FileReader",
    .finalizer = dyn_res_finalizer,
};

static ssize_t dyn_freader_fill(dyn_freader_t* r)
{
    ssize_t n;
    if (r->start < r->end)
        return (ssize_t)(r->end - r->start);
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

static JSValue dyn_freader_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    const char* path;
    int64_t bufsize = 0;
    struct stat st;
    int fd;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileReader(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bufferSize" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    fd = dyn_open_strict(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "FileReader", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    if (fstat(fd, &st) == 0)
        dyn_io_advise_seq_read(fd, st.st_size);
    dyn_path_unborrow(ctx, path);

    r = (dyn_freader_t*)calloc(1, sizeof(*r));
    if (!r) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->fd = fd;
    r->cap = dyn_file_clamp_bufsize(bufsize);
    r->buf = (unsigned char*)malloc(r->cap);
    if (!r->buf) {
        close(fd);
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_freader_class_id, r, dyn_freader_dispose);
}

static JSValue dyn_file_bytes_to_u8(JSContext* ctx, const uint8_t* data,
    size_t n);

static JSValue dyn_freader_read_raw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    const char* op, int as_bytes)
{
    dyn_freader_t* r;
    int64_t want = -1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &want, argv[0]))
            return JS_EXCEPTION;
    }
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    {
        char* acc = NULL;
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
                break;
            avail = r->end - r->start;
            take = avail;
            if (want >= 0 && take > (size_t)(want - (int64_t)acc_len))
                take = (size_t)(want - (int64_t)acc_len);
            if (acc_len + take + 1 > acc_cap) {
                size_t nc = acc_cap ? acc_cap * 2 : 8192;
                char* na;
                while (nc < acc_len + take + 1)
                    nc *= 2;
                if (nc > DYN_MAX_INPUT) {
                    free(acc);
                    return JS_ThrowRangeError(ctx,
                        "%s: accumulated read exceeds %u bytes"
                        " (DYN_MAX_INPUT)",
                        op, (unsigned)DYN_MAX_INPUT);
                }
                na = (char*)realloc(acc, nc);
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
        if (as_bytes)
            out = dyn_file_bytes_to_u8(ctx, (const uint8_t*)(acc ? acc : ""),
                acc_len);
        else
            out = dyn_utf8_lossy_string(ctx, (const uint8_t*)(acc ? acc : ""), acc_len);
        free(acc);
        return out;
    }
}

static JSValue dyn_freader_read(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return dyn_freader_read_raw(ctx, this_val, argc, argv, "FileReader.read", 0);
}

static JSValue dyn_freader_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_freader_read_raw(ctx, this_val, argc, argv,
        "FileReader.readBytes", 1);
}

static JSValue dyn_freader_read_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    JSValue ab;
    uint8_t *base, *dst;
    size_t off, len, bpe, ab_size, total = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx,
            "FileReader.readInto(buf) requires a Uint8Array (or any "
            "TypedArray/DataView)");
    ab = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, ab);
    if (!base) {
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    if (off > ab_size || len > ab_size - off) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r) {
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    dst = base + off;
    while (total < len) {
        size_t avail, take;
        ssize_t f = dyn_freader_fill(r);
        if (f < 0) {
            JS_FreeValue(ctx, ab);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break;
        avail = r->end - r->start;
        take = avail < len - total ? avail : len - total;
        memcpy(dst + total, r->buf + r->start, take);
        r->start += take;
        total += take;
    }
    JS_FreeValue(ctx, ab);
    return JS_NewInt64(ctx, (int64_t)total);
}

static JSValue dyn_freader_read_line(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_freader_t* r;
    char* acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    int saw_any = 0;
    JSValue out;

    (void)argc;
    (void)argv;
    r = (dyn_freader_t*)dyn_res_native(ctx, this_val, dyn_freader_class_id);
    if (!r)
        return JS_EXCEPTION;
    for (;;) {
        ssize_t f = dyn_freader_fill(r);
        unsigned char* nl;
        size_t avail, take;
        if (f < 0) {
            free(acc);
            return JS_ThrowInternalError(ctx, "FileReader: read error");
        }
        if (f == 0)
            break;
        saw_any = 1;
        avail = r->end - r->start;
        nl = (unsigned char*)memchr(r->buf + r->start, '\n', avail);
        take = nl ? (size_t)(nl - (r->buf + r->start)) : avail;
        if (acc_len + take + 1 > acc_cap) {
            size_t nc = acc_cap ? acc_cap * 2 : 256;
            char* na;
            while (nc < acc_len + take + 1)
                nc *= 2;
            if (nc > DYN_MAX_INPUT) {
                free(acc);
                return JS_ThrowRangeError(ctx,
                    "FileReader.readLine: line exceeds %u bytes"
                    " (DYN_MAX_INPUT)",
                    (unsigned)DYN_MAX_INPUT);
            }
            na = (char*)realloc(acc, nc);
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
            r->start++;
            if (acc_len > 0 && acc[acc_len - 1] == '\r')
                acc_len--;
            out = dyn_utf8_lossy_string(ctx, (const uint8_t*)acc, acc_len);
            free(acc);
            return out;
        }
    }
    if (!saw_any && acc_len == 0) {
        free(acc);
        return JS_NULL;
    }
    out = dyn_utf8_lossy_string(ctx, (const uint8_t*)(acc ? acc : ""), acc_len);
    free(acc);
    return out;
}

static const JSCFunctionListEntry dyn_freader_proto[] = {
    JS_CFUNC_DEF("read", 0, dyn_freader_read),
    JS_CFUNC_DEF("readLine", 0, dyn_freader_read_line),
    JS_CFUNC_DEF("readAll", 0, dyn_freader_read),
    JS_CFUNC_DEF("readInto", 1, dyn_freader_read_into),
    JS_CFUNC_DEF("readBytes", 0, dyn_freader_read_bytes),
};

#if defined(CONFIG_NATIVE_MODULE_NET)
#include "dyna-aio.h"
#include "dyna-evloop.h"
#include "core/dyn-timer.h"
#endif

static JSClassID dyn_fwriter_class_id;

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t len;
    int fd;
    int dirty;
} dyn_fwriter_t;

_Static_assert(sizeof(dyn_fwriter_t) == 3 * sizeof(void*) + 8,
    "dyn_fwriter_t regained padding: keep the two ints adjacent");

static void dyn_fwriter_flush_native(dyn_fwriter_t* w, int* err)
{
    size_t off = 0;
    *err = 0;
    while (off < w->len) {
        ssize_t n = write(w->fd, w->buf + off, w->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            *err = 1;
            break;
        }
        if (n == 0) {
            *err = 1;
            break;
        }
        off += (size_t)n;
    }
    if (off > 0 && off < w->len)
        memmove(w->buf, w->buf + off, w->len - off);
    w->len -= off;
}

static void dyn_fwriter_dispose(void* native)
{
    dyn_fwriter_t* w = (dyn_fwriter_t*)native;
    int err;
    if (w->fd >= 0) {
        dyn_fwriter_flush_native(w, &err);
        close(w->fd);
    }
    free(w->buf);
    free(w);
}

static const JSClassDef dyn_fwriter_class = {
    "FileWriter",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_fwriter_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_fwriter_t* w;
    const char* path;
    int64_t bufsize = 0, preallocate = 0;
    int append = 0, flags;
    int fd;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FileWriter(path[, options]) requires a path");
    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bufferSize", "preallocate", "append" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 3))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bufferSize");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &bufsize, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "preallocate");
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, &preallocate, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "append");
        {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                return JS_EXCEPTION;
            append = b;
        }
    }
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    fd = dyn_open_strict(path, flags, 0644);
    if (fd < 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "FileWriter", path);
        dyn_path_unborrow(ctx, path);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    if (preallocate > 0)
        dyn_io_preallocate(fd, (off_t)preallocate);

    w = (dyn_fwriter_t*)calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->fd = fd;
    w->cap = dyn_file_clamp_bufsize(bufsize);
    w->buf = (unsigned char*)malloc(w->cap);
    if (!w->buf) {
        close(fd);
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_res_wrap(ctx, new_target, dyn_fwriter_class_id, w, dyn_fwriter_dispose);
}

static int dyn_fwriter_put(dyn_fwriter_t* w, const char* data, size_t len)
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

static int dyn_file_payload(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned)
{
    *powned = NULL;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_ThrowTypeError(ctx,
            "data must be a string, Uint8Array, or ArrayBuffer");
        return -1;
    }
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
    {
        size_t n;
        uint8_t* p = (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &n, v) : NULL);
        if (p) {
            *pdata = p;
            *plen = n;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        size_t off, len, bpe, ab_size;
        uint8_t* base;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            base = JS_GetArrayBuffer(ctx, &ab_size, ab);
            JS_FreeValue(ctx, ab);
            if (!base)
                return -1;
            if (off > ab_size || len > ab_size - off) {
                JS_ThrowRangeError(ctx, "typed array out of bounds");
                return -1;
            }
            *pdata = base + off;
            *plen = len;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        *powned = s;
        *pdata = (const uint8_t*)s;
        *plen = n;
        return 0;
    }
}

static JSValue dyn_fwriter_write(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    const uint8_t* data = NULL;
    const char* str = NULL;
    size_t len = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "write(data) requires an argument");
    if (dyn_file_payload(ctx, argv[0], &data, &len, &str))
        return JS_EXCEPTION;

    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (dyn_fwriter_put(w, (const char*)data, len) < 0) {
        if (str)
            JS_FreeCString(ctx, str);
        return JS_ThrowInternalError(ctx, "FileWriter: write error");
    }
    if (str)
        JS_FreeCString(ctx, str);
    return JS_NewInt64(ctx, (int64_t)len);
}

static JSValue dyn_fwriter_flush(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    int err;
    (void)argc;
    (void)argv;
    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err)
        return JS_ThrowInternalError(ctx, "FileWriter: flush error");
    return JS_UNDEFINED;
}

static JSValue dyn_fwriter_sync(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fwriter_t* w;
    int err;
    (void)argc;
    (void)argv;
    w = (dyn_fwriter_t*)dyn_res_native(ctx, this_val, dyn_fwriter_class_id);
    if (!w)
        return JS_EXCEPTION;
    dyn_fwriter_flush_native(w, &err);
    if (err || dyn_io_durable_sync(w->fd) < 0)
        return JS_ThrowInternalError(ctx, "FileWriter: sync error");
    w->dirty = 0;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_fwriter_proto[] = {
    JS_CFUNC_DEF("write", 1, dyn_fwriter_write),
    JS_CFUNC_DEF("flush", 0, dyn_fwriter_flush),
    JS_CFUNC_DEF("sync", 0, dyn_fwriter_sync),
};

static int dyn_file_slurp_arg(JSContext* ctx, JSValueConst pathv,
    const char* op, dyn_iobuf_t* src)
{
    const char* path;
    path = dyn_path_arg(ctx, pathv, "path");
    if (!path)
        return -1;
    {
        int rfd = dyn_open_strict(path, O_RDONLY | O_CLOEXEC, 0);
        if (rfd < 0) {
            int e = errno;
            dyn_fs_throw(ctx, e, op, path);
            dyn_path_unborrow(ctx, path);
            return -1;
        }
        if (dyn_io_slurp_fd(rfd, src, 0, DYN_MAX_INPUT) < 0) {
            int se = errno;
            dyn_path_unborrow(ctx, path);
            if (se == EFBIG)
                JS_ThrowRangeError(ctx,
                    "%s: file exceeds %u bytes (DYN_MAX_INPUT)",
                    op, (unsigned)DYN_MAX_INPUT);
            else
                dyn_fs_throw(ctx, se ? se : EIO, op, path);
            return -1;
        }
    }
    dyn_path_unborrow(ctx, path);
    return 0;
}

static JSValue dyn_file_read_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    JSValue out;
    int as_bytes = 0;
    (void)this_val;

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "bytes" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "bytes");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        as_bytes = b;
    }
    if (dyn_file_slurp_arg(ctx, argv[0], "readFile", &src))
        return JS_EXCEPTION;
    if (as_bytes)
        out = dyn_file_bytes_to_u8(ctx, dyn_iobuf_rdata(&src),
            dyn_iobuf_rlen(&src));
    else

        out = dyn_utf8_lossy_string(ctx, dyn_iobuf_rdata(&src),
            dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return out;
}

static JSValue dyn_file_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    JSValue out;
    (void)this_val;
    (void)argc;

    if (dyn_file_slurp_arg(ctx, argv[0], "readBytes", &src))
        return JS_EXCEPTION;
    out = dyn_file_bytes_to_u8(ctx, dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return out;
}

static JSValue dyn_file_write_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *path = NULL, *str = NULL;
    const uint8_t* data = NULL;
    size_t len = 0, off = 0;
    int append = 0, flags, fd;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "writeFile(path, data[, options])");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    if (dyn_file_payload(ctx, argv[1], &data, &len, &str)) {
        dyn_path_unborrow(ctx, path);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        static const char* const keys[] = { "append" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[2], keys, 1)) {
            dyn_path_unborrow(ctx, path);
            if (str)
                JS_FreeCString(ctx, str);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "append");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0) {
            dyn_path_unborrow(ctx, path);
            if (str)
                JS_FreeCString(ctx, str);
            return JS_EXCEPTION;
        }
        append = b;
    }

    flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    fd = dyn_open_strict(path, flags, 0644);
    if (fd < 0) {
        int e = errno;
        JSValue ex = dyn_fs_throw(ctx, e, "writeFile", path);
        dyn_path_unborrow(ctx, path);
        if (str)
            JS_FreeCString(ctx, str);
        return ex;
    }
    dyn_path_unborrow(ctx, path);
    {
        const char* src = (const char*)data;
        int werr = 0;
        int werrno = 0;
        while (off < len) {
            ssize_t n = write(fd, src + off, len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                werr = 1;
                werrno = errno;
                break;
            }
            off += (size_t)n;
        }
        close(fd);
        if (str)
            JS_FreeCString(ctx, str);
        if (werr)
            return dyn_fs_throw(ctx, werrno ? werrno : EIO, "writeFile", NULL);
    }
    return JS_NewInt64(ctx, (int64_t)len);
}

#define DYN_FS_RMRF_MAX_DEPTH 512
#define DYN_FS_RMRF_FD_FALLBACK 16
#define DYN_FS_GLOB_MAX_DEPTH 512
#define DYN_FS_READLINK_MAX (1u << 16)

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

static char* dyn_join(const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    int sep = (la > 0 && a[la - 1] != '/');
    char* r;

    if (la == 0) {
        r = (char*)malloc(lb + 1);
        if (!r)
            return NULL;
        memcpy(r, b, lb + 1);
        return r;
    }
    r = (char*)malloc(la + (size_t)sep + lb + 1);
    if (!r)
        return NULL;
    memcpy(r, a, la);
    if (sep)
        r[la] = '/';
    memcpy(r + la + (size_t)sep, b, lb);
    r[la + (size_t)sep + lb] = '\0';
    return r;
}

static int dyn_is_dot_or_dotdot(const char* name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static JSValue dyn_fs_stat_common(JSContext* ctx, JSValueConst arg, int follow)
{
    const char* path;
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
    JS_DefinePropertyValueStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)st.st_size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isFile", JS_NewBool(ctx, S_ISREG(st.st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isSymlink",
        JS_NewBool(ctx, S_ISLNK(st.st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "mtimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_MTIM(st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "atimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_ATIM(st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ctimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_CTIM(st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "uid", JS_NewInt32(ctx, (int32_t)st.st_uid), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "gid", JS_NewInt32(ctx, (int32_t)st.st_gid), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ino", JS_NewInt64(ctx, (int64_t)st.st_ino), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "nlink",
        JS_NewInt64(ctx, (int64_t)st.st_nlink), JS_PROP_C_W_E);
    return obj;
}

static JSValue dyn_fs_stat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 1);
}

static JSValue dyn_fs_lstat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    return dyn_fs_stat_common(ctx, argv[0], 0);
}

static JSValue dyn_fs_exists(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    struct stat st;
    int ok;

    (void)this_val;
    (void)argc;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    ok = (dyn_stat_strict(path, &st, 0) == 0);
    dyn_path_unborrow(ctx, path);
    return JS_NewBool(ctx, ok);
}

#define DYN_DIRENT_INLINE 48

typedef struct {
    char* heap;
    char inl[DYN_DIRENT_INLINE];
    int is_dir, is_file, is_symlink;
} dyn_dirent_t;

static const char* dyn_dirent_name(const dyn_dirent_t* e)
{
    return e->heap ? e->heap : e->inl;
}

static int dyn_dirent_cmp(const void* a, const void* b)
{
    return strcmp(dyn_dirent_name((const dyn_dirent_t*)a),
        dyn_dirent_name((const dyn_dirent_t*)b));
}

static void dyn_entry_type(int dtype, const char* dir, size_t dirlen,
    const char* name, size_t namelen,
    int* is_dir, int* is_file, int* is_symlink)
{
    char stackbuf[512], *full = stackbuf, *heap = NULL;
    size_t need = dirlen + 1 + namelen + 1;
    struct stat st;

    *is_dir = *is_file = *is_symlink = 0;
#ifdef DT_DIR
    switch (dtype) {
    case DT_DIR:
        *is_dir = 1;
        return;
    case DT_REG:
        *is_file = 1;
        return;
    case DT_LNK:
        *is_symlink = 1;
        return;
    default:
        break;
    }
#else
    (void)dtype;
#endif
    if (need > sizeof(stackbuf)) {
        heap = (char*)malloc(need);
        if (!heap)
            return;
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

static JSValue dyn_fs_read_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    size_t path_len;
    DIR* d;
    struct dirent* e;
    dyn_dirent_t* ents = NULL;
    size_t n = 0, cap = 0, i;
    JSValue arr;
    int err = 0;

    (void)this_val;
    (void)argc;
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
            dyn_dirent_t* ne = (dyn_dirent_t*)realloc(ents, ncap * sizeof(*ents));
            if (!ne) {
                err = 1;
                break;
            }
            ents = ne;
            cap = ncap;
        }
        if (nlen < DYN_DIRENT_INLINE) {
            ents[n].heap = NULL;
            memcpy(ents[n].inl, e->d_name, nlen + 1);
        } else {
            ents[n].heap = (char*)malloc(nlen + 1);
            if (!ents[n].heap) {
                err = 1;
                break;
            }
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
    if (n)
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt64(ctx, (int64_t)n));
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
                JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E);
            }
            free(ents[i].heap);
        }
        JS_FreeAtom(ctx, a_name);
        JS_FreeAtom(ctx, a_dir);
        JS_FreeAtom(ctx, a_file);
        JS_FreeAtom(ctx, a_link);
    }
    free(ents);
    return arr;
}

static int dyn_fs_mkdirp(const char* path, mode_t mode)
{
    struct stat st;
    size_t len;
    char* parent;
    int r;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT)
        return -1;

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
    parent = (char*)malloc(len + 1);
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

static JSValue dyn_fs_make_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    int recursive = 0;
    int32_t mode = 0777;
    int r;

    (void)this_val;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "recursive", "mode" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 2)) {
            dyn_path_unborrow(ctx, path);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0) {
            dyn_path_unborrow(ctx, path);
            return JS_EXCEPTION;
        }
        recursive = b;
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

static JSValue dyn_fs_remove(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    (void)this_val;
    (void)argc;
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

typedef struct {
    size_t fd_budget;
    int root_fd;
    char** names;
    size_t ncomp;
    size_t names_cap;
} dyn_fs_rmrf_t;

static size_t dyn_fs_rmrf_fd_budget(void)
{
    size_t b = DYN_FS_RMRF_FD_FALLBACK * 4;
#ifndef _WIN32
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        if (rl.rlim_cur > 40)
            b = (size_t)rl.rlim_cur - 24;
        else
            b = rl.rlim_cur > 8 ? (size_t)rl.rlim_cur / 2 : 4;
    }
#endif
    if (b > DYN_FS_RMRF_MAX_DEPTH + 2)
        b = DYN_FS_RMRF_MAX_DEPTH + 2;
    if (b < 4)
        b = 4;
    return b;
}

static int dyn_fs_rmrf_push(dyn_fs_rmrf_t* p, const char* name)
{
    char* d;

    if (p->ncomp == p->names_cap) {
        size_t nc = p->names_cap ? p->names_cap * 2 : 16;
        char** nn = (char**)realloc(p->names, nc * sizeof(*nn));
        if (!nn) {
            errno = ENOMEM;
            return -1;
        }
        p->names = nn;
        p->names_cap = nc;
    }
    d = strdup(name);
    if (!d) {
        errno = ENOMEM;
        return -1;
    }
    p->names[p->ncomp++] = d;
    return 0;
}

static void dyn_fs_rmrf_pop(dyn_fs_rmrf_t* p)
{
    if (p->ncomp)
        free(p->names[--p->ncomp]);
}

static void dyn_fs_rmrf_clear(dyn_fs_rmrf_t* p)
{
    while (p->ncomp)
        dyn_fs_rmrf_pop(p);
    free(p->names);
    p->names = NULL;
    p->names_cap = 0;
}

static int dyn_fs_rmrf_reach(dyn_fs_rmrf_t* p, size_t ncomp, int* out_fd)
{
    int fd, i;

    if (p->root_fd < 0) {
        errno = EBADF;
        return -1;
    }
    fd = dup(p->root_fd);
    if (fd < 0)
        return -1;
    for (i = 0; i < (int)ncomp; i++) {
        int nfd = openat(fd, p->names[i],
            O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        int e = errno;
        close(fd);
        if (nfd < 0) {
            errno = e;
            return -1;
        }
        fd = nfd;
    }
    *out_fd = fd;
    return 0;
}

static int dyn_fs_rmrf_deep(dyn_fs_rmrf_t* p, int depth)
{
    DIR* d;
    struct dirent* e;
    char** dirs = NULL;
    size_t nd = 0, dc = 0, i;
    size_t base = p->ncomp;
    int rc = 0, fd = -1;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        errno = ELOOP;
        return -1;
    }
    if (dyn_fs_rmrf_reach(p, p->ncomp, &fd) != 0)
        return -1;
    d = fdopendir(fd);
    if (!d) {
        int e2 = errno;
        close(fd);
        errno = e2;
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            break;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlinkat(dirfd(d), e->d_name, 0) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (nd == dc) {
            size_t nc2 = dc ? dc * 2 : 16;
            char** np = (char**)realloc(dirs, nc2 * sizeof(*np));
            if (!np) {
                errno = ENOMEM;
                rc = -1;
                break;
            }
            dirs = np;
            dc = nc2;
        }
        dirs[nd] = strdup(e->d_name);
        if (!dirs[nd]) {
            errno = ENOMEM;
            rc = -1;
            break;
        }
        nd++;
    }
    {
        int saved = errno;
        closedir(d);
        if (rc)
            errno = saved;
    }
    for (i = 0; rc == 0 && i < nd; i++) {
        int pfd = -1;
        if (dyn_fs_rmrf_push(p, dirs[i]) < 0) {
            rc = -1;
            break;
        }
        if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
            rc = -1;
            dyn_fs_rmrf_pop(p);
            break;
        }
        dyn_fs_rmrf_pop(p);
        p->ncomp = base;
        if (dyn_fs_rmrf_reach(p, base, &pfd) != 0) {
            rc = -1;
            break;
        }
        if (unlinkat(pfd, dirs[i], AT_REMOVEDIR) != 0) {
            int e2 = errno;
            close(pfd);
            errno = e2;
            rc = -1;
            break;
        }
        close(pfd);
    }
    p->ncomp = base;
    for (i = 0; i < nd; i++)
        free(dirs[i]);
    free(dirs);
    return rc;
}

static int dyn_fs_rmrf_children(int dirfd, int depth, dyn_fs_rmrf_t* p)
{
    DIR* d;
    struct dirent* e;
    size_t base = p->ncomp;
    int rc = 0;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        close(dirfd);
        errno = ELOOP;
        return -1;
    }
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
            int cfd;
            if (dyn_fs_rmrf_push(p, e->d_name) < 0) {
                rc = -1;
                break;
            }
            if ((size_t)(depth + 1) >= p->fd_budget) {
                if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
                    rc = -1;
                    dyn_fs_rmrf_pop(p);
                    break;
                }
            } else {
                cfd = openat(dirfd, e->d_name,
                    O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
                if (cfd < 0) {
                    if (errno == EMFILE || errno == ENFILE) {
                        if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
                            rc = -1;
                            dyn_fs_rmrf_pop(p);
                            break;
                        }
                    } else {
                        rc = -1;
                        dyn_fs_rmrf_pop(p);
                        break;
                    }
                } else {
                    if (dyn_fs_rmrf_children(cfd, depth + 1, p) != 0) {
                        rc = -1;
                        dyn_fs_rmrf_pop(p);
                        break;
                    }
                }
            }
            dyn_fs_rmrf_pop(p);
            p->ncomp = base;
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
    {
        int saved = errno;
        closedir(d);
        if (rc)
            errno = saved;
    }
    p->ncomp = base;
    return rc;
}

static JSValue dyn_fs_remove_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    struct stat st;

    (void)this_val;
    (void)argc;
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    if (lstat(path, &st) != 0) {
        int e = errno;
        dyn_path_unborrow(ctx, path);
        if (e == ENOENT)
            return JS_UNDEFINED;
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
        {
            int pin_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            dyn_fs_rmrf_t p;
            if (pin_fd < 0) {
                close(fd);
                dyn_path_unborrow(ctx, path);
                return dyn_fs_throw(ctx, errno, "removeAll", NULL);
            }
            p.fd_budget = dyn_fs_rmrf_fd_budget();
            p.root_fd = pin_fd;
            p.names = NULL;
            p.ncomp = 0;
            p.names_cap = 0;
            if (dyn_fs_rmrf_children(fd, 0, &p) != 0) {
                int e = errno;
                dyn_fs_rmrf_clear(&p);
                close(pin_fd);
                JSValue ex = dyn_fs_throw(ctx, e, "removeAll", path);
                dyn_path_unborrow(ctx, path);
                return ex;
            }
            dyn_fs_rmrf_clear(&p);
            {
                struct stat pin, now;
                int bad = fstat(pin_fd, &pin) != 0 || stat(path, &now) != 0
                    || now.st_dev != pin.st_dev || now.st_ino != pin.st_ino;
                close(pin_fd);
                if (bad) {
                    dyn_path_unborrow(ctx, path);
                    return dyn_fs_throw(ctx, EXDEV, "removeAll",
                        "directory changed during removal");
                }
            }
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

static JSValue dyn_fs_rename(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *from, *to;
    (void)this_val;
    (void)argc;

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

static JSValue dyn_fs_symlink(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *target, *linkpath;
    size_t tlen = 0;
    (void)this_val;
    (void)argc;

    target = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    if (strlen(target) != tlen) {
        JS_FreeCString(ctx, target);
        return JS_ThrowTypeError(ctx, "symlink: target contains a NUL byte");
    }
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

static JSValue dyn_fs_read_link(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    size_t cap = 256;
    (void)this_val;
    (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;

    for (;;) {
        char* buf = (char*)malloc(cap);
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
            JSValue out = JS_NewStringLen(ctx, buf, (size_t)n);
            free(buf);
            dyn_path_unborrow(ctx, path);
            return out;
        }
        free(buf);
        cap *= 2;
        if (cap > DYN_FS_READLINK_MAX) {
            JSValue ex = dyn_fs_throw(ctx, ENAMETOOLONG, "readLink", path);
            dyn_path_unborrow(ctx, path);
            return ex;
        }
    }
}

static JSValue dyn_fs_real_path(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    char* resolved;
    JSValue out;
    (void)this_val;
    (void)argc;

    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    resolved = realpath(path, NULL);
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

static JSValue dyn_fs_chmod(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* path;
    int32_t mode;
    (void)this_val;
    (void)argc;

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

static int dyn_glob_match(const char* p, const char* s)
{
    const char *star_p = NULL, *star_s = NULL;

    for (;;) {
        unsigned char pc = (unsigned char)*p;

        if (pc == '*') {
            while (*p == '*')
                p++;
            if (*p == '\0')
                return 1;
            star_p = p;
            star_s = s;
            continue;
        }
        if (pc == '\0') {
            if (*s == '\0')
                return 1;
            goto backtrack;
        }
        if (*s == '\0')
            goto backtrack;

        if (pc == '?') {
            p++;
            s++;
            continue;
        }
        if (pc == '[') {
            const char* q = p + 1;
            const char* start;
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
                if (c != '[')
                    goto backtrack;
                p++;
                s++;
                continue;
            }
            q++;
            if (matched == negate)
                goto backtrack;
            p = q;
            s++;
            continue;
        }
        if (pc == (unsigned char)*s) {
            p++;
            s++;
            continue;
        }

    backtrack:
        if (!star_p)
            return 0;
        if (*star_s == '\0')
            return 0;
        star_s++;
        p = star_p;
        s = star_s;
    }
}

static int dyn_glob_match_name(const char* pat, const char* name)
{
    if (name[0] == '.' && pat[0] != '.')
        return 0;
    return dyn_glob_match(pat, name);
}

static int dyn_glob_has_wildcard(const char* s)
{
    return strpbrk(s, "*?[") != NULL;
}

typedef struct {
    char** items;
    size_t count, cap;
    int oom;
} dyn_glob_res;

static void dyn_glob_res_push(dyn_glob_res* r, const char* s, size_t len)
{
    char* dup;
    if (r->oom)
        return;
    if (r->count == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 32;
        char** ni = (char**)realloc(r->items, nc * sizeof(*ni));
        if (!ni) {
            r->oom = 1;
            return;
        }
        r->items = ni;
        r->cap = nc;
    }
    dup = (char*)malloc(len + 1);
    if (!dup) {
        r->oom = 1;
        return;
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    r->items[r->count++] = dup;
}

static void dyn_glob_res_free(dyn_glob_res* r)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        free(r->items[i]);
    free(r->items);
}

static void dyn_glob_emit(dyn_glob_res* res, const char* rel, int is_abs)
{
    if (is_abs) {
        char* disp = dyn_join("/", rel);
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

static char* dyn_glob_fsdir(const char* base, const char* rel)
{
    if (rel[0] == '\0')
        return dyn_join(base, "");
    return dyn_join(base, rel);
}

static void dyn_glob_walk(dyn_glob_res* res, const char* base, char** segs,
    int nseg, int si, const char* rel, int is_abs,
    int depth);

static void dyn_glob_walk(dyn_glob_res* res, const char* base, char** segs,
    int nseg, int si, const char* rel, int is_abs,
    int depth)
{
    const char* seg;
    char* fsdir;

    if (res->oom || depth > DYN_FS_GLOB_MAX_DEPTH)
        return;
    if (si == nseg) {
        dyn_glob_emit(res, rel, is_abs);
        return;
    }
    seg = segs[si];

    if (strcmp(seg, "**") == 0) {
        int is_last = (si == nseg - 1);
        DIR* d;
        struct dirent* e;

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
                if (!childrel) {
                    res->oom = 1;
                    break;
                }
                childfs = dyn_join(fsdir, e->d_name);
                if (!childfs) {
                    free(childrel);
                    res->oom = 1;
                    break;
                }
                if (lstat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                    dyn_glob_walk(res, base, segs, nseg, si, childrel, is_abs,
                        depth + 1);
                else if (is_last)
                    dyn_glob_emit(res, childrel, is_abs);
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

    if (!dyn_glob_has_wildcard(seg)) {
        char* childrel = (rel[0] == '\0') ? dyn_join("", seg)
                                          : dyn_join(rel, seg);
        char* childfs;
        struct stat st;
        if (!childrel) {
            res->oom = 1;
            return;
        }
        childfs = dyn_glob_fsdir(base, childrel);
        if (!childfs) {
            free(childrel);
            res->oom = 1;
            return;
        }
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

    fsdir = dyn_glob_fsdir(base, rel);
    if (!fsdir) {
        res->oom = 1;
        return;
    }
    {
        DIR* d = opendir(fsdir);
        int is_last = (si == nseg - 1);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                char* childrel;
                if (dyn_is_dot_or_dotdot(e->d_name))
                    continue;
                if (!dyn_glob_match_name(seg, e->d_name))
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) {
                    res->oom = 1;
                    break;
                }
                if (is_last) {
                    dyn_glob_emit(res, childrel, is_abs);
                } else {
                    char* childfs = dyn_join(fsdir, e->d_name);
                    struct stat st;
                    if (!childfs) {
                        free(childrel);
                        res->oom = 1;
                        break;
                    }
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

static int dyn_glob_str_cmp(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static JSValue dyn_fs_glob(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *pattern = NULL, *cwd = NULL;
    char *patcopy = NULL, **segs = NULL;
    int nseg = 0, is_abs, i;
    size_t plen;
    const char* base;
    dyn_glob_res res;
    JSValue arr;
    char* p;

    (void)this_val;
    pattern = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pattern)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "cwd" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 1)) {
            JS_FreeCString(ctx, pattern);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "cwd");
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
        JS_FreeCString(ctx, pattern);
        if (cwd)
            dyn_path_unborrow(ctx, cwd);
        return JS_NewArray(ctx);
    }

    patcopy = (char*)malloc(plen + 1);
    segs = (char**)malloc((plen + 2) * sizeof(*segs));
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

    p = patcopy;
    while (*p) {
        char* seg_start;
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
        if (nseg > 0 && strcmp(seg_start, "**") == 0 && strcmp(segs[nseg - 1], "**") == 0)
            continue;
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
            JS_DefinePropertyValueUint32(ctx, arr, out++,
                dyn_path_new_from(ctx, res.items[i],
                    strlen(res.items[i])), JS_PROP_C_W_E);
        }
    }
    dyn_glob_res_free(&res);
    return arr;
}

static const char* dyn_fs_tempdir_str(void)
{
    const char* t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return t;
}

static JSValue dyn_fs_temp_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* t = dyn_fs_tempdir_str();
    size_t len = strlen(t);
    (void)this_val;
    (void)argc;
    (void)argv;
    while (len > 1 && t[len - 1] == '/')
        len--;
    return dyn_path_new_from(ctx, t, len);
}

static int dyn_fs_temp_prefix_ok(const char* prefix, size_t len)
{
    size_t i;
    if (len > 200 || (len && prefix[0] == '.') || strlen(prefix) != len)
        return 0;
    for (i = 0; i < len; i++)
        if (prefix[i] == '/' || prefix[i] == '\\')
            return 0;
    return 1;
}

static char* dyn_fs_temp_template(const char* prefix)
{
    const char* t = dyn_fs_tempdir_str();
    size_t tl = strlen(t), pl = prefix ? strlen(prefix) : 0;
    char* tpl;
    while (tl > 1 && t[tl - 1] == '/')
        tl--;
    tpl = (char*)malloc(tl + 1 + pl + 6 + 1);
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

static JSValue dyn_fs_make_temp_dir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* prefix = NULL;
    char* tpl;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    if (prefix && !dyn_fs_temp_prefix_ok(prefix, strlen(prefix))) {
        JS_FreeCString(ctx, prefix);
        return JS_ThrowRangeError(ctx,
            "temp name prefix must be a plain file-name fragment: no '/', "
            "no '\\', no leading '.', at most 200 bytes");
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

static JSValue dyn_fs_make_temp_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* prefix = NULL;
    char* tpl;
    int fd;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        prefix = JS_ToCString(ctx, argv[0]);
        if (!prefix)
            return JS_EXCEPTION;
    }
    if (prefix && !dyn_fs_temp_prefix_ok(prefix, strlen(prefix))) {
        JS_FreeCString(ctx, prefix);
        return JS_ThrowRangeError(ctx,
            "temp name prefix must be a plain file-name fragment: no '/', "
            "no '\\', no leading '.', at most 200 bytes");
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
    close(fd);
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

#include "dyna-filecopy.inc.c"
#include "dyna-watch.inc.c"

static char* dyn_pdir_join(const char* a, const char* b)
{
    size_t al = strlen(a), bl = strlen(b);
    char* out = (char*)malloc(al + bl + 2);

    if (!out)
        return NULL;
    memcpy(out, a, al);
    if (al && a[al - 1] != '/')
        out[al++] = '/';
    memcpy(out + al, b, bl + 1);
    return out;
}

static char* dyn_pdir_base(int kind, int site)
{
    const char* home = getenv("HOME");

    if (!home || !*home)
        home = "/";

#if defined(_WIN32)
    {
        const char* e = getenv(site ? "PROGRAMDATA" : "LOCALAPPDATA");
        if (!e || !*e)
            e = home;
        return strdup(e);
    }
#elif defined(__APPLE__)
    if (site) {
        static const char* const s[3] = {
            "/Library/Application Support",
            "/Library/Preferences",
            "/Library/Caches",
        };
        return strdup(s[kind]);
    }
    {
        static const char* const s[3] = {
            "Library/Application Support",
            "Library/Preferences",
            "Library/Caches",
        };
        return dyn_pdir_join(home, s[kind]);
    }
#else
    if (site) {
        switch (kind) {
        case 0: {
            const char* dirs = getenv("XDG_DATA_DIRS");
            if (dirs && *dirs) {
                const char* colon = strchr(dirs, ':');
                size_t n = colon ? (size_t)(colon - dirs) : strlen(dirs);
                if (n > 0) {
                    char* out = (char*)malloc(n + 1);
                    if (!out)
                        return NULL;
                    memcpy(out, dirs, n);
                    out[n] = '\0';
                    return out;
                }
            }
            return strdup("/usr/local/share");
        }
        case 1: {
            const char* dirs = getenv("XDG_CONFIG_DIRS");
            if (dirs && *dirs) {
                const char* colon = strchr(dirs, ':');
                size_t n = colon ? (size_t)(colon - dirs) : strlen(dirs);
                if (n > 0) {
                    char* out = (char*)malloc(n + 1);
                    if (!out)
                        return NULL;
                    memcpy(out, dirs, n);
                    out[n] = '\0';
                    return out;
                }
            }
            return strdup("/etc/xdg");
        }
        default:
            return strdup("/var/cache");
        }
    }
    {
        const char* e;
        switch (kind) {
        case 0:
            e = getenv("XDG_DATA_HOME");
            if (e && *e && e[0] == '/')
                return strdup(e);
            return dyn_pdir_join(home, ".local/share");
        case 1:
            e = getenv("XDG_CONFIG_HOME");
            if (e && *e && e[0] == '/')
                return strdup(e);
            return dyn_pdir_join(home, ".config");
        default:
            e = getenv("XDG_CACHE_HOME");
            if (e && *e && e[0] == '/')
                return strdup(e);
            return dyn_pdir_join(home, ".cache");
        }
    }
#endif
}

static JSValue dyn_pdir_get(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    int kind = magic & 3;
    int site = (magic >> 2) & 1;
    char *base, *full;
    const char* app = NULL;
    size_t app_len = 0;
    JSValue out;
    (void)this_val;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:file: app must be a string");
        app = JS_ToCStringLen(ctx, &app_len, argv[0]);
        if (!app)
            return JS_EXCEPTION;
        if (app_len == 0 || strchr(app, '/') || strchr(app, '\\') || strlen(app) != app_len || strcmp(app, ".") == 0 || strcmp(app, "..") == 0) {
            JS_FreeCString(ctx, app);
            return JS_ThrowTypeError(ctx,
                "dyna:file: app must be a single non-empty path segment "
                "(no '/', '\\' or NUL, and not '.' or '..')");
        }
    }
    base = dyn_pdir_base(kind, site);
    if (!base) {
        if (app)
            JS_FreeCString(ctx, app);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (app && app_len > 0) {
        size_t bl = strlen(base);
        int sep = (bl > 0 && base[bl - 1] != '/');
        full = (char*)malloc(bl + sep + app_len + 1);
        if (!full) {
            free(base);
            JS_FreeCString(ctx, app);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(full, base, bl);
        if (sep)
            full[bl] = '/';
        memcpy(full + bl + sep, app, app_len);
        full[bl + sep + app_len] = '\0';
        free(base);
        base = full;
    }
    if (app)
        JS_FreeCString(ctx, app);
    out = dyn_path_new_from(ctx, base, strlen(base));
    free(base);
    return out;
}

typedef struct {
    int fd;
    int excl;
    char* lockfile;
} dyn_flock_t;

static JSClassID dyn_flock_class_id;

static void dyn_flock_sleep_ms(int64_t ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int dyn_flock_try(dyn_flock_t* l, const char* path)
{
    int fd;

#if defined(_WIN32)
    HANDLE h;
    OVERLAPPED ov;
    (void)path;
    if (l->fd < 0) {
        l->fd = _open(path, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (l->fd < 0)
            return -1;
    }
    h = (HANDLE)_get_osfhandle(l->fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }
    memset(&ov, 0, sizeof(ov));
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 0, 0xFFFFFFFF, &ov))
        return 0;
    if (GetLastError() == ERROR_LOCK_VIOLATION) {
        errno = EWOULDBLOCK;
        return 1;
    }
    errno = EACCES;
    return -1;
#else
    if (l->excl) {
        fd = open(l->lockfile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            l->fd = fd;
            return 0;
        }
        if (errno == EEXIST) {
            errno = EWOULDBLOCK;
            return 1;
        }
        return -1;
    }
    if (l->fd < 0) {
        l->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (l->fd < 0 && errno == EACCES) {
            l->fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (l->fd < 0)
            return -1;
    }
    if (flock(l->fd, LOCK_EX | LOCK_NB) == 0)
        return 0;
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        errno = EWOULDBLOCK;
        return 1;
    }
    if (errno == EOPNOTSUPP || errno == ENOLCK || errno == ENOSYS) {
        size_t pl = strlen(path);
        close(l->fd);
        l->fd = -1;
        l->excl = 1;
        l->lockfile = (char*)malloc(pl + 6);
        if (!l->lockfile) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(l->lockfile, path, pl);
        memcpy(l->lockfile + pl, ".lock", 6);
        return dyn_flock_try(l, path);
    }
    return -1;
#endif
}

static void dyn_flock_release(dyn_flock_t* l)
{
    if (l->fd < 0)
        return;
#if defined(_WIN32)
    {
        HANDLE h = (HANDLE)_get_osfhandle(l->fd);
        OVERLAPPED ov;
        if (h != INVALID_HANDLE_VALUE) {
            memset(&ov, 0, sizeof(ov));
            UnlockFileEx(h, 0, 0xFFFFFFFF, 0, &ov);
        }
        _close(l->fd);
    }
#else
    if (l->excl) {
        close(l->fd);
        unlink(l->lockfile);
    } else {
        flock(l->fd, LOCK_UN);
        close(l->fd);
    }
#endif
    l->fd = -1;
}

static void dyn_flock_dispose(void* native)
{
    dyn_flock_t* l = (dyn_flock_t*)native;
    if (!l)
        return;
    dyn_flock_release(l);
    free(l->lockfile);
    free(l);
}

static JSValue dyn_flock_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_flock_t* l;
    const char* path;
    JSValue pathv, res;
    int64_t retry = 0, retry_ms = 100;
    int attempt;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "new FileLock(path[, options]) requires a path");

    if (argc > 1 && JS_IsObject(argv[1])) {
        static const char* const keys[] = { "retry", "retryMs" };
        JSValue v;
        if (dyn_opts_strict(ctx, argv[1], keys, 2))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "retry");
        if (!JS_IsUndefined(v)) {
            if (!JS_IsNumber(v) || JS_ToInt64(ctx, &retry, v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "FileLock: retry must be a number");
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "retryMs");
        if (!JS_IsUndefined(v)) {
            if (!JS_IsNumber(v) || JS_ToInt64(ctx, &retry_ms, v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "FileLock: retryMs must be a number");
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (retry < 0 || retry_ms < 0)
        return JS_ThrowRangeError(ctx, "FileLock: retry and retryMs must be >= 0");
    if (retry > 86400000ll || retry_ms > 86400000ll || (retry_ms > 0 && retry > 86400000ll / retry_ms))
        return JS_ThrowRangeError(ctx,
            "FileLock: retry/retryMs exceed the 24h total wait cap");

    if (dyn_is_path(argv[0])) {
        pathv = JS_DupValue(ctx, argv[0]);
    } else if (JS_IsString(argv[0])) {
        pathv = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(pathv))
            return pathv;
    } else {
        return JS_ThrowTypeError(ctx, "new FileLock(path): path must be a Path or a string");
    }
    path = dyn_path_arg(ctx, pathv, "path");
    if (!path) {
        JS_FreeValue(ctx, pathv);
        return JS_EXCEPTION;
    }

    l = (dyn_flock_t*)calloc(1, sizeof(*l));
    if (!l) {
        JS_FreeValue(ctx, pathv);
        return JS_ThrowOutOfMemory(ctx);
    }
    l->fd = -1;

    for (attempt = 0;; attempt++) {
        int r = dyn_flock_try(l, path);
        if (r == 0)
            break;
        if (r < 0) {
            int e = errno;
            JSValue ex = dyn_fs_throw(ctx, e, "FileLock", path);
            JS_FreeValue(ctx, pathv);
            dyn_flock_dispose(l);
            return ex;
        }
        if (attempt >= retry) {
            JSValue ex = dyn_fs_throw(ctx, EWOULDBLOCK, "FileLock", path);
            JS_FreeValue(ctx, pathv);
            dyn_flock_dispose(l);
            return ex;
        }
        dyn_flock_sleep_ms(retry_ms);
    }
    JS_FreeValue(ctx, pathv);

    res = dyn_res_wrap(ctx, new_target, dyn_flock_class_id, l, dyn_flock_dispose);
    return res;
}

static JSValue dyn_flock_with_lock(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box;
    dyn_flock_t* l;
    JSValue ret;
    (void)argc;

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "FileLock.withLock(fn): fn must be a function");
    box = dyn_res_get(ctx, this_val, dyn_flock_class_id);
    if (!box)
        return JS_EXCEPTION;
    l = (dyn_flock_t*)box->native;
    ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (!box->closed)
        dyn_flock_dispose(l);
    dyn_res_mark_closed(JS_GetRuntime(ctx), this_val, dyn_flock_class_id);
    return ret;
}

static const JSCFunctionListEntry dyn_flock_proto[] = {
    JS_CFUNC_DEF("withLock", 1, dyn_flock_with_lock),
};

static const JSClassDef dyn_flock_class = {
    "FileLock",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_file_funcs[] = {
    JS_CFUNC_DEF("readFile", 1, dyn_file_read_file),
    JS_CFUNC_DEF("readBytes", 1, dyn_file_read_bytes),
    JS_CFUNC_DEF("writeFile", 2, dyn_file_write_file),
    JS_CFUNC_DEF("stat", 1, dyn_fs_stat),
    JS_CFUNC_DEF("lstat", 1, dyn_fs_lstat),
    JS_CFUNC_DEF("exists", 1, dyn_fs_exists),
    JS_CFUNC_DEF("readDir", 1, dyn_fs_read_dir),
    JS_CFUNC_DEF("makeDir", 1, dyn_fs_make_dir),
    JS_CFUNC_DEF("remove", 1, dyn_fs_remove),
    JS_CFUNC_DEF("removeAll", 1, dyn_fs_remove_all),
    JS_CFUNC_DEF("rename", 2, dyn_fs_rename),
    JS_CFUNC_DEF("copyFile", 2, dyn_fs_copy_file),
    JS_CFUNC_DEF("move", 2, dyn_fs_move),
    JS_CFUNC_DEF("sniffType", 1, dyn_fs_sniff_type),
    JS_CFUNC_DEF("symlink", 2, dyn_fs_symlink),
    JS_CFUNC_DEF("readLink", 1, dyn_fs_read_link),
    JS_CFUNC_DEF("realPath", 1, dyn_fs_real_path),
    JS_CFUNC_DEF("chmod", 2, dyn_fs_chmod),
    JS_CFUNC_DEF("glob", 1, dyn_fs_glob),
    JS_CFUNC_DEF("tempDir", 0, dyn_fs_temp_dir),
    JS_CFUNC_DEF("makeTempDir", 0, dyn_fs_make_temp_dir),
    JS_CFUNC_DEF("makeTempFile", 0, dyn_fs_make_temp_file),
    JS_CFUNC_MAGIC_DEF("dataDir", 0, dyn_pdir_get, 0),
    JS_CFUNC_MAGIC_DEF("configDir", 0, dyn_pdir_get, 1),
    JS_CFUNC_MAGIC_DEF("cacheDir", 0, dyn_pdir_get, 2),
    JS_CFUNC_MAGIC_DEF("dataDirSite", 0, dyn_pdir_get, 4),
    JS_CFUNC_MAGIC_DEF("configDirSite", 0, dyn_pdir_get, 5),
    JS_CFUNC_MAGIC_DEF("cacheDirSite", 0, dyn_pdir_get, 6),
};

typedef struct {
    JSValue path;
} dyn_fh_t;

static JSClassID dyn_fh_class_id;

static void dyn_fh_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_fh_t* f = (dyn_fh_t*)JS_GetOpaque(val, dyn_fh_class_id);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->path);
    free(f);
}

static void dyn_fh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_fh_t* f = (dyn_fh_t*)JS_GetOpaque(val, dyn_fh_class_id);
    if (f)
        JS_MarkValue(rt, f->path, mark_func);
}

static const JSClassDef dyn_fh_class = {
    "File",
    .finalizer = dyn_fh_finalizer,
    .gc_mark = dyn_fh_mark,
};

static JSValue dyn_fh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f;
    JSValue obj, path, proto;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new File(path) requires a Path");
    proto = dyn_ctor_proto(ctx, new_target, dyn_fh_class_id);
    if (JS_IsException(proto))
        return proto;
    if (dyn_is_path(argv[0])) {
        path = JS_DupValue(ctx, argv[0]);
    } else if (JS_IsString(argv[0])) {
        path = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(path)) {
            JS_FreeValue(ctx, proto);
            return path;
        }
    } else {
        JS_FreeValue(ctx, proto);
        return JS_ThrowTypeError(ctx, "new File(path): path must be a Path or a string");
    }

    f = (dyn_fh_t*)calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->path = path;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_fh_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, path);
        free(f);
        return obj;
    }
    JS_SetOpaque(obj, f);
    return obj;
}

static dyn_fh_t* dyn_fh_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_fh_t*)JS_GetOpaque2(ctx, v, dyn_fh_class_id);
}

#define DYN_FH_FORWARD(name, fn, maxargs)                                \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_fh_t* f = dyn_fh_of(ctx, this_val);                          \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!f)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = f->path;                                                  \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

static JSValue dyn_file_bytes_to_u8(JSContext* ctx, const uint8_t* data, size_t n)
{
    JSValue ab, global, ctor, out;
    JSValueConst args[1];

    ab = JS_NewArrayBufferCopy(ctx, data, n);
    if (JS_IsException(ab))
        return ab;
    global = JS_GetGlobalObject(ctx);
    ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);
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

DYN_FH_FORWARD(dyn_fh_read_bytes, dyn_file_read_bytes, 0)

DYN_FH_FORWARD(dyn_fh_write_text, dyn_file_write_file, 2)
DYN_FH_FORWARD(dyn_fh_stat, dyn_fs_stat, 0)
DYN_FH_FORWARD(dyn_fh_lstat, dyn_fs_lstat, 0)
DYN_FH_FORWARD(dyn_fh_exists, dyn_fs_exists, 0)
DYN_FH_FORWARD(dyn_fh_remove, dyn_fs_remove, 0)
DYN_FH_FORWARD(dyn_fh_real_path, dyn_fs_real_path, 0)
DYN_FH_FORWARD(dyn_fh_chmod, dyn_fs_chmod, 1)

static JSValue dyn_fh_append(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[3];
    JSValue opts, r;

    if (!f)
        return JS_EXCEPTION;
    opts = JS_NewObject(ctx);
    if (JS_IsException(opts))
        return opts;
    JS_DefinePropertyValueStr(ctx, opts, "append", JS_NewBool(ctx, 1), JS_PROP_C_W_E);
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    a[2] = opts;
    r = dyn_file_write_file(ctx, JS_UNDEFINED, 3, a);
    JS_FreeValue(ctx, opts);
    return r;
}

static JSValue dyn_fh_move_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
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
    JS_FreeValue(ctx, f->path);
    f->path = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_fh_copy_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[3];

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "copyTo(dest[, options]) requires a Path");
    a[0] = f->path;
    a[1] = argv[0];
    a[2] = (argc > 1) ? argv[1] : JS_UNDEFINED;
    {
        JSValue r = dyn_fs_copy_file(ctx, JS_UNDEFINED, 3, a);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    JSValueConst one[1];
    one[0] = argv[0];
    return dyn_fh_ctor(ctx, JS_UNDEFINED, 1, one);
}

static JSValue dyn_fh_stream(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];

    if (!f)
        return JS_EXCEPTION;
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    return magic ? dyn_fwriter_ctor(ctx, JS_UNDEFINED, 2, a)
                 : dyn_freader_ctor(ctx, JS_UNDEFINED, 2, a);
}

static JSValue dyn_fh_get_path(JSContext* ctx, JSValueConst this_val)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, f->path);
}

static JSValue dyn_fh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    return dyn_path_get_str(ctx, f->path);
}

static const JSCFunctionListEntry dyn_fh_proto[] = {
    JS_CGETSET_DEF("path", dyn_fh_get_path, NULL),
    JS_CFUNC_DEF("readText", 0, dyn_fh_read_text),
    JS_CFUNC_DEF("readBytes", 0, dyn_fh_read_bytes),
    JS_CFUNC_DEF("writeText", 1, dyn_fh_write_text),
    JS_CFUNC_DEF("writeBytes", 1, dyn_fh_write_text),
    JS_CFUNC_DEF("append", 1, dyn_fh_append),
    JS_CFUNC_DEF("stat", 0, dyn_fh_stat),
    JS_CFUNC_DEF("lstat", 0, dyn_fh_lstat),
    JS_CFUNC_DEF("exists", 0, dyn_fh_exists),
    JS_CFUNC_DEF("remove", 0, dyn_fh_remove),
    JS_CFUNC_DEF("realPath", 0, dyn_fh_real_path),
    JS_CFUNC_DEF("chmod", 1, dyn_fh_chmod),
    JS_CFUNC_DEF("moveTo", 1, dyn_fh_move_to),
    JS_CFUNC_DEF("copyTo", 1, dyn_fh_copy_to),
    JS_CFUNC_MAGIC_DEF("reader", 0, dyn_fh_stream, 0),
    JS_CFUNC_MAGIC_DEF("writer", 0, dyn_fh_stream, 1),
    JS_CFUNC_DEF("toString", 0, dyn_fh_to_string),
    JS_CFUNC_DEF("toJSON", 0, dyn_fh_to_string),
};

typedef struct {
    char* pattern;
    size_t len;
    int has_wildcard;
} dyn_gl_t;

static JSClassID dyn_gl_class_id;

static void dyn_gl_dispose(void* native)
{
    dyn_gl_t* g = (dyn_gl_t*)native;
    if (!g)
        return;
    free(g->pattern);
    free(g);
}

static void dyn_gl_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_gl_dispose(JS_GetOpaque(val, dyn_gl_class_id));
}

static const JSClassDef dyn_gl_class = {
    "Glob",
    .finalizer = dyn_gl_finalizer,
};

static JSValue dyn_gl_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g;
    const char* pat;
    size_t plen;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Glob(pattern) requires a string");
    pat = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    g = (dyn_gl_t*)calloc(1, sizeof(*g));
    if (!g) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    g->pattern = (char*)malloc(plen + 1);
    if (!g->pattern) {
        JS_FreeCString(ctx, pat);
        free(g);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(g->pattern, pat, plen);
    g->pattern[plen] = '\0';
    g->len = plen;
    g->has_wildcard = dyn_glob_has_wildcard(pat);
    JS_FreeCString(ctx, pat);
    return dyn_plain_wrap(ctx, new_target, dyn_gl_class_id, g, dyn_gl_dispose);
}

static dyn_gl_t* dyn_gl_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_gl_t*)JS_GetOpaque2(ctx, v, dyn_gl_class_id);
}

static JSValue dyn_gl_matches(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    const char* path;

    if (!g)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "matches(path) requires a Path");
    path = dyn_path_arg(ctx, argv[0], "path");
    if (!path)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_glob_match(g->pattern, path));
}

static JSValue dyn_gl_expand(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
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
        JS_DefinePropertyValueStr(ctx, opts, "cwd", JS_DupValue(ctx, argv[0]), JS_PROP_C_W_E);
    }
    a[0] = pat;
    a[1] = opts;
    r = dyn_fs_glob(ctx, JS_UNDEFINED, 2, a);
    JS_FreeValue(ctx, pat);
    JS_FreeValue(ctx, opts);
    return r;
}

static JSValue dyn_gl_filter(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
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
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        const char* path;
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        path = dyn_path_arg(ctx, e, "element");
        if (!path) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        if (dyn_glob_match(g->pattern, path))
            JS_DefinePropertyValueUint32(ctx, out, k++, e, JS_PROP_C_W_E);
        else
            JS_FreeValue(ctx, e);
    }
    return out;
}

static JSValue dyn_gl_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_gl_t* g = dyn_gl_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewStringLen(ctx, g->pattern, g->len);
    return JS_NewBool(ctx, g->has_wildcard);
}

static JSValue dyn_gl_static_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *path, *pat;
    size_t plen;
    JSValue ret;
    int m;
    (void)this_val;

    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Glob.match(path, pattern): pattern must be a string");
    if (JS_IsString(argv[0])) {
        path = JS_ToCString(ctx, argv[0]);
        if (!path)
            return JS_EXCEPTION;
        pat = JS_ToCStringLen(ctx, &plen, argv[1]);
        if (!pat) {
            JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
        m = dyn_glob_match(pat, path);
        JS_FreeCString(ctx, pat);
        JS_FreeCString(ctx, path);
    } else {
        path = dyn_path_arg(ctx, argv[0], "Glob.match(path, pattern)");
        if (!path)
            return JS_EXCEPTION;
        pat = JS_ToCStringLen(ctx, &plen, argv[1]);
        if (!pat)
            return JS_EXCEPTION;
        m = dyn_glob_match(pat, path);
        JS_FreeCString(ctx, pat);
    }
    ret = JS_NewBool(ctx, m);
    return ret;
}

static const JSCFunctionListEntry dyn_gl_proto[] = {
    JS_CFUNC_DEF("matches", 1, dyn_gl_matches),
    JS_CFUNC_DEF("expand", 0, dyn_gl_expand),
    JS_CFUNC_DEF("filter", 1, dyn_gl_filter),
    JS_CGETSET_MAGIC_DEF("pattern", dyn_gl_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("hasWildcard", dyn_gl_get, NULL, 1),
};

static int dyn_register_path_class(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
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
    JS_DefinePropertyValueStr(ctx, ctor, "cwd",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "cwd", 0,
            JS_CFUNC_generic_magic, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "home",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "home", 0,
            JS_CFUNC_generic_magic, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "temp",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "temp", 0,
            JS_CFUNC_generic_magic, 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "isPath",
        JS_NewCFunctionMagic(ctx, dyn_path_static, "isPath", 1,
            JS_CFUNC_generic_magic, 3), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "sep", JS_NewString(ctx, sep), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ctor, "delimiter", JS_NewString(ctx, delim), JS_PROP_C_W_E);
    return JS_SetModuleExport(ctx, m, "Path", ctor);
}

static int dyn_file_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_path_class(ctx, m) < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_fh_class_id, &dyn_fh_class,
            dyn_fh_proto, (int)countof(dyn_fh_proto),
            dyn_fh_ctor, "File")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_gl_class_id, &dyn_gl_class,
            dyn_gl_proto, (int)countof(dyn_gl_proto),
            dyn_gl_ctor, "Glob")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_gl_class_id);
        JSValue ctor;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        if (JS_DefinePropertyValueStr(ctx, ctor, "match",
                JS_NewCFunction(ctx, dyn_gl_static_match, "match", 2),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_class(ctx, m, &dyn_freader_class_id, &dyn_freader_class,
            dyn_freader_proto, countof(dyn_freader_proto),
            dyn_freader_ctor, "FileReader")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_fwriter_class_id, &dyn_fwriter_class,
            dyn_fwriter_proto, countof(dyn_fwriter_proto),
            dyn_fwriter_ctor, "FileWriter")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_watch_class_id, &dyn_watch_class,
            dyn_watch_proto, countof(dyn_watch_proto),
            dyn_watch_ctor, "Watcher")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_flock_class_id, &dyn_flock_class,
            dyn_flock_proto, countof(dyn_flock_proto),
            dyn_flock_ctor, "FileLock")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_file_funcs,
        (int)countof(dyn_file_funcs));
}

int js_nat_init_file(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:file", dyn_file_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Path");
    JS_AddModuleExport(ctx, m, "File");
    JS_AddModuleExport(ctx, m, "Glob");
    JS_AddModuleExport(ctx, m, "FileReader");
    JS_AddModuleExport(ctx, m, "FileWriter");
    JS_AddModuleExport(ctx, m, "Watcher");
    JS_AddModuleExport(ctx, m, "FileLock");
    JS_AddModuleExportList(ctx, m, dyn_file_funcs, (int)countof(dyn_file_funcs));
    return 0;
}

#endif
