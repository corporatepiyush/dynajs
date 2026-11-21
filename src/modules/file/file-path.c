#include "file-internal.h"
#include <pthread.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#define DYN_PATH_STACK_SCRATCH 1024

typedef struct {
    char* buf;
    size_t len;
    dyn_path_split_t split;
    int refs;
} dyn_path_rec_t;

typedef struct {
    const char** ptr;
    size_t* len;
    const char** owned;
    int n, n_owned;
} dyn_seg_list_t;

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

void dyn_path_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_path_rec_unref((dyn_path_rec_t*)JS_GetOpaque(val, dyn_path_class_id));
}

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

int dyn_is_path(JSValueConst v)
{
    return JS_GetOpaque(v, dyn_path_class_id) != NULL;
}

typedef struct dyn_path_tmp {
    const char* buf;
    JSValue v;
    struct dyn_path_tmp* next;
} dyn_path_tmp_t;

static dyn_path_tmp_t* dyn_path_tmps;
static pthread_mutex_t dyn_path_tmps_mu = PTHREAD_MUTEX_INITIALIZER;

static const char* dyn_path_from_string(JSContext* ctx, JSValueConst v, size_t* plen)
{
    dyn_path_tmp_t* t;
    dyn_path_rec_t* p;
    JSValue pv = dyn_path_ctor(ctx, JS_UNDEFINED, 1, &v);
    if (JS_IsException(pv))
        return NULL;
    t = (dyn_path_tmp_t*)malloc(sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, pv);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    p = (dyn_path_rec_t*)JS_GetOpaque(pv, dyn_path_class_id);
    t->buf = p->buf;
    t->v = pv;
    pthread_mutex_lock(&dyn_path_tmps_mu);
    t->next = dyn_path_tmps;
    dyn_path_tmps = t;
    pthread_mutex_unlock(&dyn_path_tmps_mu);
    if (plen)
        *plen = p->len;
    return p->buf;
}

void dyn_path_unborrow(JSContext* ctx, const char* buf)
{
    dyn_path_tmp_t **pp, *t = NULL;
    if (!buf)
        return;
    pthread_mutex_lock(&dyn_path_tmps_mu);
    for (pp = &dyn_path_tmps; *pp; pp = &(*pp)->next)
        if ((*pp)->buf == buf) {
            t = *pp;
            *pp = t->next;
            break;
        }
    pthread_mutex_unlock(&dyn_path_tmps_mu);
    if (t) {
        JS_FreeValue(ctx, t->v);
        free(t);
    }
}

const char* dyn_path_arg(JSContext* ctx, JSValueConst v, const char* what)
{
    dyn_path_rec_t* p;
    if (JS_IsString(v))
        return dyn_path_from_string(ctx, v, NULL);
    if (!dyn_is_path(v)) {
        JS_ThrowTypeError(ctx, "dyna:file: %s must be a Path -- wrap it with "
                               "new Path(...)",
            what);
        return NULL;
    }
    p = (dyn_path_rec_t*)JS_GetOpaque(v, dyn_path_class_id);
    return p->buf;
}

const char* dyn_path_arg_len(JSContext* ctx, JSValueConst v,
    const char* what, size_t* plen)
{
    dyn_path_rec_t* p;
    if (JS_IsString(v))
        return dyn_path_from_string(ctx, v, plen);
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

JSValue dyn_path_new_from(JSContext* ctx, const char* s, size_t n)
{
    char res[PATH_MAX];
    size_t rn = dyn_path_resolve_prefix(s, n, res);
    if (rn)
        return dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_from(res, rn));
    return dyn_path_wrap_rec(ctx, JS_UNDEFINED, dyn_path_rec_from(s, n));
}

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

JSValue dyn_path_ctor(JSContext* ctx, JSValueConst new_target, int argc,
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

JSValue dyn_path_get_str(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf, p->len);
}

JSValue dyn_path_get_dirname(JSContext* ctx, JSValueConst this_val)
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

JSValue dyn_path_get_basename(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.base_off, p->split.base_len);
}

JSValue dyn_path_get_extname(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, p->buf + p->split.ext_off, p->split.ext_len);
}

JSValue dyn_path_get_is_absolute(JSContext* ctx, JSValueConst this_val)
{
    dyn_path_rec_t* p = dyn_path_of(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->split.is_absolute);
}

JSValue dyn_path_compose(JSContext* ctx, JSValueConst this_val, int argc,
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

JSValue dyn_path_relative_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_path_rec_t* self = dyn_path_of(ctx, this_val);
    dyn_path_rec_t* to;
    char *out, *scratch;
    size_t cap, len;
    JSValue res;

    if (!self)
        return JS_EXCEPTION;
    if (argc < 1 || (!dyn_is_path(argv[0]) && !JS_IsString(argv[0])))
        return JS_ThrowTypeError(ctx, "Path.relativeTo(other): other must be "
                                      "a Path");
    if (JS_IsString(argv[0])) {
        JSValue pv = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(pv))
            return pv;
        res = dyn_path_relative_to(ctx, this_val, 1, (JSValueConst*)&pv);
        JS_FreeValue(ctx, pv);
        return res;
    }
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

JSValue dyn_path_equals(JSContext* ctx, JSValueConst this_val, int argc,
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

JSValue dyn_path_basename_without(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_path_to_string(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_path_static(JSContext* ctx, JSValueConst this_val, int argc,
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

JSValue dyn_pdir_get(JSContext* ctx, JSValueConst this_val, int argc,
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

#endif
