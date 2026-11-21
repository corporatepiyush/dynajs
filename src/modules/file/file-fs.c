#include "file-internal.h"
#include <stdarg.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#define DYN_FS_READLINK_MAX (1u << 16)

static int dyn_fs_path_stat(const char* path, struct stat* st, int follow)
{
    return follow ? stat(path, st) : lstat(path, st);
}

JSValue dyn_fs_throw(JSContext* ctx, int e, const char* op,
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

JSValue __attribute__((format(printf, 2, 3))) dyn_fs_throw_type(JSContext* ctx, const char* fmt, ...)
{
    va_list ap;
    char* msg;
    int n;
    JSValue err;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return JS_ThrowTypeError(ctx, "%s", fmt);
    msg = js_malloc(ctx, (size_t)n + 1);
    if (!msg)
        return JS_EXCEPTION;
    va_start(ap, fmt);
    vsnprintf(msg, (size_t)n + 1, fmt, ap);
    va_end(ap);
    JS_ThrowTypeError(ctx, "%s", "");
    err = JS_GetException(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewStringLen(ctx, msg, (size_t)n),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    js_free(ctx, msg);
    return JS_Throw(ctx, err);
}

static double dyn_timespec_ms(struct timespec ts)
{
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

JSValue dyn_fs_stat_obj(JSContext* ctx, const struct stat* st)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)st->st_size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st->st_mode), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st->st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isFile", JS_NewBool(ctx, S_ISREG(st->st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "isSymlink",
        JS_NewBool(ctx, S_ISLNK(st->st_mode)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "mtimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_MTIM(*st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "atimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_ATIM(*st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ctimeMs",
        JS_NewFloat64(ctx, dyn_timespec_ms(DYN_STAT_CTIM(*st))), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "uid", JS_NewInt32(ctx, (int32_t)st->st_uid), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "gid", JS_NewInt32(ctx, (int32_t)st->st_gid), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ino", JS_NewInt64(ctx, (int64_t)st->st_ino), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "nlink",
        JS_NewInt64(ctx, (int64_t)st->st_nlink), JS_PROP_C_W_E);
    return obj;
}

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

static void dyn_fs_dir_run(dyn_fs_req_t* q)
{
    DIR* d;
    struct dirent* e;
    dyn_dirent_t* ents = NULL;
    size_t n = 0, cap = 0, i, plen = strlen(q->path);
    int oom = 0;

    d = opendir(q->path);
    if (!d) {
        q->err = errno;
        return;
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
                oom = 1;
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
                oom = 1;
                break;
            }
            memcpy(ents[n].heap, e->d_name, nlen + 1);
        }
        dyn_entry_type(dtype, q->path, plen, e->d_name, nlen,
            &ents[n].is_dir, &ents[n].is_file, &ents[n].is_symlink);
        n++;
    }
    closedir(d);
    if (oom) {
        for (i = 0; i < n; i++)
            free(ents[i].heap);
        free(ents);
        q->ekind = DYN_FS_EK_OOM;
        return;
    }
    if (n > 1)
        qsort(ents, n, sizeof(*ents), dyn_dirent_cmp);
    q->u.dir.ents = ents;
    q->u.dir.n = n;
}

static JSValue dyn_fs_dir_obj(JSContext* ctx, const dyn_fs_req_t* q)
{
    const dyn_dirent_t* ents = q->u.dir.ents;
    size_t n = q->u.dir.n, i;
    JSAtom a_name, a_dir, a_file, a_link;
    JSValue arr = JS_NewArray(ctx);

    if (JS_IsException(arr))
        return JS_EXCEPTION;
    if (n)
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt64(ctx, (int64_t)n));
    a_name = JS_NewAtom(ctx, "name");
    a_dir = JS_NewAtom(ctx, "isDir");
    a_file = JS_NewAtom(ctx, "isFile");
    a_link = JS_NewAtom(ctx, "isSymlink");
    for (i = 0; i < n; i++) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o))
            continue;
        JS_DefinePropertyValue(ctx, o, a_name,
            JS_NewString(ctx, dyn_dirent_name(&ents[i])), JS_PROP_C_W_E);
        JS_DefinePropertyValue(ctx, o, a_dir, JS_NewBool(ctx, ents[i].is_dir), JS_PROP_C_W_E);
        JS_DefinePropertyValue(ctx, o, a_file, JS_NewBool(ctx, ents[i].is_file), JS_PROP_C_W_E);
        JS_DefinePropertyValue(ctx, o, a_link, JS_NewBool(ctx, ents[i].is_symlink), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E);
    }
    JS_FreeAtom(ctx, a_name);
    JS_FreeAtom(ctx, a_dir);
    JS_FreeAtom(ctx, a_file);
    JS_FreeAtom(ctx, a_link);
    return arr;
}

static void dyn_fs_readlink_run(dyn_fs_req_t* q)
{
    size_t cap = 256;
    for (;;) {
        char* buf = (char*)malloc(cap);
        ssize_t n;
        if (!buf) {
            q->ekind = DYN_FS_EK_OOM;
            return;
        }
        n = readlink(q->path, buf, cap);
        if (n < 0) {
            q->err = errno;
            free(buf);
            return;
        }
        if ((size_t)n < cap) {
            q->u.str.s = buf;
            q->u.str.n = (size_t)n;
            return;
        }
        free(buf);
        cap *= 2;
        if (cap > DYN_FS_READLINK_MAX) {
            q->err = ENAMETOOLONG;
            return;
        }
    }
}

static void dyn_fs_realpath_run(dyn_fs_req_t* q)
{
    char buf[PATH_MAX];
    size_t n;
    if (!realpath(q->path, buf)) {
        q->err = errno;
        return;
    }
    n = strlen(buf);
    q->u.str.s = (char*)malloc(n + 1);
    if (!q->u.str.s) {
        q->ekind = DYN_FS_EK_OOM;
        return;
    }
    memcpy(q->u.str.s, buf, n + 1);
    q->u.str.n = n;
}

const char* const dyn_fs_op_names[DYN_FS_NOPS] = { "stat", "lstat", "exists", "readDir",
    "makeDir", "remove", "removeAll", "rename", "copyFile", "move", "symlink", "readLink",
    "realPath", "chmod" };

void dyn_fs_req_run(dyn_fs_req_t* q)
{
    struct stat st;
    q->err = 0;
    q->ekind = DYN_FS_EK_ERRNO;
    q->epath = q->path;
    switch (q->op) {
    case DYN_FS_STAT:
    case DYN_FS_LSTAT:
        if (dyn_fs_path_stat(q->path, &q->u.st, q->op == DYN_FS_STAT) != 0)
            q->err = errno;
        break;
    case DYN_FS_EXISTS:
        q->u.yes = dyn_fs_path_stat(q->path, &st, 0) == 0;
        break;
    case DYN_FS_READDIR:
        dyn_fs_dir_run(q);
        break;
    case DYN_FS_MKDIR:
        if ((q->flag ? dyn_fs_mkdirp(q->path, (mode_t)q->mode) : mkdir(q->path, (mode_t)q->mode)) != 0)
            q->err = errno;
        break;
    case DYN_FS_REMOVE:
        if (remove(q->path) != 0)
            q->err = errno;
        break;
    case DYN_FS_REMOVEALL: {
        int e = 0;
        const char* what = q->path;
        if (dyn_fs_remove_tree(q->path, &e, &what) != 0) {
            q->err = e ? e : EIO;
            q->epath = what;
        }
        break;
    }
    case DYN_FS_RENAME:
        if (rename(q->path, q->path2) != 0)
            q->err = errno;
        break;
    case DYN_FS_COPY:
        dyn_fs_copy_run(q);
        break;
    case DYN_FS_MOVE:
        dyn_fs_move_run(q);
        break;
    case DYN_FS_SYMLINK:
        if (symlink(q->path2, q->path) != 0)
            q->err = errno;
        break;
    case DYN_FS_READLINK:
        dyn_fs_readlink_run(q);
        break;
    case DYN_FS_REALPATH:
        dyn_fs_realpath_run(q);
        break;
    case DYN_FS_CHMOD:
        if (chmod(q->path, (mode_t)q->mode) != 0)
            q->err = errno;
        break;
    default:
        q->err = EINVAL;
        break;
    }
}

JSValue dyn_fs_req_result(JSContext* ctx, dyn_fs_req_t* q)
{
    switch (q->ekind) {
    case DYN_FS_EK_OOM:
        return JS_ThrowOutOfMemory(ctx);
    case DYN_FS_EK_ISDIR:
        return dyn_fs_throw_type(ctx, "copyFile: '%s' is a directory", q->path);
    case DYN_FS_EK_NOTREG:
        return dyn_fs_throw_type(ctx, "copyFile: '%s' is not a regular file", q->path);
    case DYN_FS_EK_SAME:
        return dyn_fs_throw_type(ctx, "copyFile: '%s' and '%s' are the same file", q->path, q->path2);
    default:
        break;
    }
    if (q->err)
        return dyn_fs_throw(ctx, q->err, dyn_fs_op_names[q->op], q->epath);
    switch (q->op) {
    case DYN_FS_STAT:
    case DYN_FS_LSTAT:
        return dyn_fs_stat_obj(ctx, &q->u.st);
    case DYN_FS_EXISTS:
        return JS_NewBool(ctx, q->u.yes);
    case DYN_FS_READDIR:
        return dyn_fs_dir_obj(ctx, q);
    case DYN_FS_COPY:
        return JS_NewInt64(ctx, q->u.num);
    case DYN_FS_READLINK:
        return JS_NewStringLen(ctx, q->u.str.s, q->u.str.n);
    case DYN_FS_REALPATH:
        return dyn_path_new_from(ctx, q->u.str.s, q->u.str.n);
    default:
        return JS_UNDEFINED;
    }
}

void dyn_fs_req_clear(dyn_fs_req_t* q)
{
    size_t i;
    if (q->op == DYN_FS_READDIR && q->u.dir.ents) {
        for (i = 0; i < q->u.dir.n; i++)
            free(q->u.dir.ents[i].heap);
        free(q->u.dir.ents);
        q->u.dir.ents = NULL;
        q->u.dir.n = 0;
    } else if ((q->op == DYN_FS_READLINK || q->op == DYN_FS_REALPATH) && q->u.str.s) {
        free(q->u.str.s);
        q->u.str.s = NULL;
    }
}

void dyn_fs_req_unborrow(JSContext* ctx, dyn_fs_req_t* q)
{
    if (q->borrowed & 1)
        dyn_path_unborrow(ctx, q->path);
    if (q->borrowed & 2)
        dyn_path_unborrow(ctx, q->path2);
    if (q->borrowed & 4)
        JS_FreeCString(ctx, q->path2);
    q->borrowed = 0;
}

static int dyn_fs_parse_paths(JSContext* ctx, JSValueConst a, const char* na,
    JSValueConst b, const char* nb, dyn_fs_req_t* q)
{
    q->path = dyn_path_arg(ctx, a, na);
    if (!q->path)
        return -1;
    q->borrowed |= 1;
    if (!nb)
        return 0;
    q->path2 = dyn_path_arg(ctx, b, nb);
    if (!q->path2) {
        dyn_fs_req_unborrow(ctx, q);
        return -1;
    }
    q->borrowed |= 2;
    return 0;
}

static int dyn_fs_bool_opt(JSContext* ctx, JSValueConst o, const char* key, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int b;
    if (JS_IsException(v))
        return -1;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (b < 0)
        return -1;
    *out = b;
    return 0;
}

static int dyn_fs_mkdir_opts(JSContext* ctx, JSValueConst o, dyn_fs_req_t* q)
{
    static const char* const keys[] = { "recursive", "mode" };
    JSValue v;
    if (dyn_opts_strict(ctx, o, keys, 2) || dyn_fs_bool_opt(ctx, o, "recursive", &q->flag))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "mode");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &q->mode, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

int dyn_fs_req_parse(JSContext* ctx, int op, int argc, JSValueConst* argv, dyn_fs_req_t* q)
{
    q->op = op;
    switch (op) {
    case DYN_FS_MKDIR:
        if (dyn_fs_parse_paths(ctx, argv[0], "path", JS_UNDEFINED, NULL, q))
            return -1;
        q->mode = 0777;
        if (argc > 1 && JS_IsObject(argv[1]) && dyn_fs_mkdir_opts(ctx, argv[1], q)) {
            dyn_fs_req_unborrow(ctx, q);
            return -1;
        }
        return 0;
    case DYN_FS_RENAME:
        return dyn_fs_parse_paths(ctx, argv[0], "from", argv[1], "to", q);
    case DYN_FS_MOVE:
        if (argc < 2) {
            JS_ThrowTypeError(ctx, "move(from, to): both are required");
            return -1;
        }
        return dyn_fs_parse_paths(ctx, argv[0], "from", argv[1], "to", q);
    case DYN_FS_COPY:
        if (argc < 2) {
            JS_ThrowTypeError(ctx, "copyFile(from, to): both are required");
            return -1;
        }
        if (argc > 2 && JS_IsObject(argv[2])) {
            static const char* const keys[] = { "overwrite" };
            if (dyn_opts_strict(ctx, argv[2], keys, 1) || dyn_fs_bool_opt(ctx, argv[2], "overwrite", &q->flag))
                return -1;
        }
        return dyn_fs_parse_paths(ctx, argv[0], "from", argv[1], "to", q);
    case DYN_FS_SYMLINK: {
        size_t tlen = 0;
        const char* t = JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!t)
            return -1;
        if (strlen(t) != tlen) {
            JS_FreeCString(ctx, t);
            JS_ThrowTypeError(ctx, "symlink: target contains a NUL byte");
            return -1;
        }
        q->path2 = t;
        q->borrowed = 4;
        q->path = dyn_path_arg(ctx, argv[1], "linkpath");
        if (!q->path) {
            dyn_fs_req_unborrow(ctx, q);
            return -1;
        }
        q->borrowed |= 1;
        return 0;
    }
    case DYN_FS_CHMOD:
        if (dyn_fs_parse_paths(ctx, argv[0], "path", JS_UNDEFINED, NULL, q))
            return -1;
        if (JS_ToInt32(ctx, &q->mode, argv[1])) {
            dyn_fs_req_unborrow(ctx, q);
            return -1;
        }
        return 0;
    default:
        return dyn_fs_parse_paths(ctx, argv[0], "path", JS_UNDEFINED, NULL, q);
    }
}

JSValue dyn_fs_req_sync(JSContext* ctx, int op, int argc, JSValueConst* argv)
{
    dyn_fs_req_t q;
    JSValue r;
    memset(&q, 0, sizeof(q));
    if (dyn_fs_req_parse(ctx, op, argc, argv, &q) < 0)
        return JS_EXCEPTION;
    dyn_fs_req_run(&q);
    r = dyn_fs_req_result(ctx, &q);
    dyn_fs_req_unborrow(ctx, &q);
    dyn_fs_req_clear(&q);
    return r;
}

JSValue dyn_fs_stat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_STAT, argc, argv);
}

JSValue dyn_fs_lstat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_LSTAT, argc, argv);
}

JSValue dyn_fs_exists(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_EXISTS, argc, argv);
}

JSValue dyn_fs_read_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_READDIR, argc, argv);
}

JSValue dyn_fs_make_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_MKDIR, argc, argv);
}

JSValue dyn_fs_remove(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_REMOVE, argc, argv);
}

JSValue dyn_fs_remove_all(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_REMOVEALL, argc, argv);
}

JSValue dyn_fs_rename(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_RENAME, argc, argv);
}

JSValue dyn_fs_symlink(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_SYMLINK, argc, argv);
}

JSValue dyn_fs_read_link(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_READLINK, argc, argv);
}

JSValue dyn_fs_real_path(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_REALPATH, argc, argv);
}

JSValue dyn_fs_chmod(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
    return dyn_fs_req_sync(ctx, DYN_FS_CHMOD, argc, argv);
}

JSValue dyn_fs_glob(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *pattern = NULL, *cwd = NULL;
    char *patcopy = NULL, **segs = NULL;
    int nseg = 0, is_abs, i, erc;
    size_t plen, k;
    const char* base;
    dyn_glob_res res;
    dyn_glob_alts alts;
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

    erc = dyn_glob_expand(pattern, &alts);
    JS_FreeCString(ctx, pattern);
    if (erc) {
        if (cwd)
            dyn_path_unborrow(ctx, cwd);
        if (erc == -2)
            return JS_ThrowRangeError(ctx,
                "glob: brace expansion yields more than %d patterns",
                DYN_GLOB_MAX_ALTS);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (k = 0; k < alts.n && !res.oom; k++) {
        const char* alt = alts.v[k];
        size_t alen = strlen(alt);
        nseg = 0;
        patcopy = (char*)malloc(alen + 1);
        segs = (char**)malloc((alen + 2) * sizeof(*segs));
        if (!patcopy || !segs) {
            free(patcopy);
            free(segs);
            res.oom = 1;
            break;
        }
        memcpy(patcopy, alt, alen + 1);
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
    }

    dyn_glob_alts_free(&alts);
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
                    strlen(res.items[i])),
                JS_PROP_C_W_E);
        }
    }
    dyn_glob_res_free(&res);
    return arr;
}

JSValue dyn_fs_temp_dir(JSContext* ctx, JSValueConst this_val, int argc,
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

JSValue dyn_fs_make_temp_dir(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_fs_make_temp_file(JSContext* ctx, JSValueConst this_val,
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

#endif
