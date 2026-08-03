/* Watcher -- kernel-event file watching for dyna:file (design 23).
 *
 * TWO EVENT SOURCES, ONE CLASSIFIER. Every backend under-reports: kqueue says
 * "this directory changed" with no name, inotify names the file but not whether
 * a rename was a move-in or a delete-plus-create. So neither is trusted to
 * classify. Both do exactly one thing -- set a dirty flag -- and the events are
 * derived by diffing a snapshot of the tree. That is what makes the two
 * platforms produce identical events rather than merely similar ones.
 *
 *   macOS/BSD  EVFILT_VNODE into the shared reactor, one fd per directory AND
 *              per file -- a directory watch never reports a file's contents
 *              changing, so directories alone would miss every save.
 *   Linux      inotify, one fd for the whole tree, an ordinary readable fd;
 *              a directory watch already covers its files.
 *
 * DEBOUNCE IS NOT A NICETY. An editor saves by writing a temp file, renaming it
 * over the target and chmod-ing it: 3-5 kernel events for one logical save.
 * Emitting each is why naive watchers cause rebuild storms.
 */

#include <dirent.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY      /* macOS-only: watch without pinning the mount */
#endif

#define DYN_WATCH_MAX_ENTRIES 100000  /* snapshot cap; reported, not silent */
#define DYN_WATCH_MAX_DEPTH   64
/* kqueue only: a watched FILE costs a descriptor. Directories alone are not
 * enough there -- EVFILT_VNODE on a directory fires when an entry is added or
 * removed, never when a file's CONTENTS change, so a save would go unreported.
 * inotify has no such gap: a directory watch reports IN_MODIFY for files in it. */
#define DYN_WATCH_MAX_FDS     4096

typedef struct {
    char    *path;              /* relative to the root */
    ino_t    ino;
    int64_t  mtime;             /* seconds; ns compared separately */
    long     mtime_ns;
    off_t    size;
    unsigned is_dir : 1;
    unsigned seen   : 1;        /* mark for the diff sweep */
} dyn_watch_ent_t;

typedef struct dyn_watch_dir {
    struct dyn_watch_dir *next;
    int   fd;                   /* kqueue backend: the watched directory fd */
    char *path;
} dyn_watch_dir_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    struct dyn_evloop *lp;
    char *root;
    JSValue on_change;
    JSValue self;               /* held while armed; a discarded Watcher would
                                   otherwise be collected and go silent */
    dyn_watch_ent_t *snap;
    size_t n_snap, cap_snap;
    char **ignore;
    size_t n_ignore;
    dyn_watch_dir_t *dirs;
    uint64_t debounce_ms, dirty_at;
    int recursive, dirty, closed, hooked, truncated;
    uint64_t n_events;
#ifdef __linux__
    int ifd;
#endif
} dyn_watch_t;

static JSClassID dyn_watch_class_id;

static const JSClassDef dyn_watch_class = {
    "Watcher", .finalizer = dyn_res_finalizer,
};

/* ---- snapshot --------------------------------------------------------- */

static void dyn_watch_snap_free(dyn_watch_t *w)
{
    size_t i;
    for (i = 0; i < w->n_snap; i++)
        free(w->snap[i].path);
    free(w->snap);
    w->snap = NULL;
    w->n_snap = w->cap_snap = 0;
}

static int dyn_watch_ignored(const dyn_watch_t *w, const char *rel,
                             const char *name)
{
    size_t i;
    for (i = 0; i < w->n_ignore; i++)
        if (dyn_glob_match(w->ignore[i], rel) ||
            dyn_glob_match(w->ignore[i], name))
            return 1;
    return 0;
}

static int dyn_watch_push(dyn_watch_t *w, const char *rel, const struct stat *st)
{
    dyn_watch_ent_t *e;
    if (w->n_snap == w->cap_snap) {
        size_t nc = w->cap_snap ? w->cap_snap * 2 : 64;
        dyn_watch_ent_t *n = (dyn_watch_ent_t *)realloc(w->snap, nc * sizeof(*n));
        if (!n)
            return -1;
        w->snap = n;
        w->cap_snap = nc;
    }
    e = &w->snap[w->n_snap];
    e->path = strdup(rel);
    if (!e->path)
        return -1;
    e->ino = st->st_ino;
    e->mtime = (int64_t)st->st_mtime;
#if defined(__APPLE__)
    e->mtime_ns = st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    e->mtime_ns = st->st_mtim.tv_nsec;
#else
    e->mtime_ns = 0;
#endif
    e->size = st->st_size;
    e->is_dir = S_ISDIR(st->st_mode) ? 1u : 0u;
    e->seen = 0;
    w->n_snap++;
    return 0;
}

static int dyn_watch_arm_entry(dyn_watch_t *w, const char *abs, int is_dir);

/* Walk `abs` (whose path relative to the root is `rel`) into the snapshot.
 * Directories are armed as they are found, so a newly created subtree starts
 * being watched on the same pass that first reports it. */
static int dyn_watch_walk(dyn_watch_t *w, const char *abs, const char *rel,
                          int depth)
{
    DIR *d;
    struct dirent *de;

    if (depth > DYN_WATCH_MAX_DEPTH)
        return 0;
    d = opendir(abs);
    if (!d)
        return 0;                       /* vanished mid-walk: the diff reports it */
    if (dyn_watch_arm_entry(w, abs, 1) < 0) {
        closedir(d);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char cabs[2048], crel[2048];
        struct stat st;
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
            continue;
        if (w->n_snap >= DYN_WATCH_MAX_ENTRIES) {
            w->truncated = 1;           /* say so; a silent cap reads as coverage */
            break;
        }
        snprintf(cabs, sizeof(cabs), "%s/%s", abs, de->d_name);
        if (*rel)
            snprintf(crel, sizeof(crel), "%s/%s", rel, de->d_name);
        else
            snprintf(crel, sizeof(crel), "%s", de->d_name);
        if (dyn_watch_ignored(w, crel, de->d_name))
            continue;                   /* BEFORE descending: walking
                                           node_modules to then ignore it is the
                                           difference between 20 ms and 20 s */
        if (lstat(cabs, &st) != 0)
            continue;
        if (dyn_watch_push(w, crel, &st) < 0) {
            closedir(d);
            return -1;
        }
        if (!S_ISDIR(st.st_mode) && dyn_watch_arm_entry(w, cabs, 0) < 0) {
            closedir(d);
            return -1;
        }
        /* lstat, so a symlinked directory is an entry and not a subtree: that
           is what stops a symlink loop without a visited set. */
        if (S_ISDIR(st.st_mode) && w->recursive) {
            if (dyn_watch_walk(w, cabs, crel, depth + 1) < 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

/* ---- backends: both only set `dirty` ---------------------------------- */

static void dyn_watch_mark(dyn_watch_t *w)
{
    w->dirty = 1;
    w->dirty_at = dyn_timer_now_ms();
}

#ifdef __linux__
static void dyn_watch_on_inotify(struct dyn_evloop *lp, int fd, int events,
                                 void *udata)
{
    dyn_watch_t *w = (dyn_watch_t *)udata;
    char buf[8192];
    ssize_t n;
    (void)lp; (void)events;
    /* MUST drain: inotify's fd stays readable until the queue is consumed, so
       leaving it would spin the loop at 100% CPU. The contents are discarded --
       the diff, not the event, is what classifies. */
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        ;
    (void)n;
    dyn_watch_mark(w);
}

static int dyn_watch_arm_entry(dyn_watch_t *w, const char *abs, int is_dir)
{
    /* One inotify fd covers the tree; each directory is a watch descriptor on
       it. ENOSPC here is the max_user_watches limit and is worth naming. */
    if (w->ifd < 0 || !is_dir)
        return 0;   /* a directory watch already reports its files' IN_MODIFY */
    if (inotify_add_watch(w->ifd, abs,
                          IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM |
                          IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF) < 0) {
        if (errno == ENOSPC)
            return -1;
    }
    return 0;
}
#else
static void dyn_watch_on_vnode(struct dyn_evloop *lp, int fd, int events,
                               void *udata)
{
    dyn_watch_t *w = (dyn_watch_t *)udata;
    (void)lp; (void)fd; (void)events;
    dyn_watch_mark(w);
}

static int dyn_watch_arm_entry(dyn_watch_t *w, const char *abs, int is_dir)
{
    dyn_watch_dir_t *d;
    int fd;
    size_t n = 0;

    (void)is_dir;                       /* files need a watch here too */
    for (d = w->dirs; d; d = d->next, n++)
        if (strcmp(d->path, abs) == 0)
            return 0;                   /* already watched */
    if (n >= DYN_WATCH_MAX_FDS) {
        w->truncated = 1;               /* say so rather than going quiet */
        return 0;
    }
    fd = open(abs, O_EVTONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;                       /* unreadable dir is not a fatal error */
    d = (dyn_watch_dir_t *)calloc(1, sizeof(*d));
    if (!d) { close(fd); return -1; }
    d->fd = fd;
    d->path = strdup(abs);
    if (!d->path) { close(fd); free(d); return -1; }
    if (dyn_evloop_add(w->lp, fd, DYN_EV_VNODE, dyn_watch_on_vnode, w) < 0) {
        close(fd); free(d->path); free(d);
        return -1;
    }
    d->next = w->dirs;
    w->dirs = d;
    return 0;
}
#endif

/* ---- the diff, which is where events actually come from ---------------- */

static void dyn_watch_emit(dyn_watch_t *w, const char *type, const char *path)
{
    JSContext *ctx = w->ctx;
    JSValue ev, r;
    JSValueConst a[1];

    w->n_events++;
    if (!JS_IsFunction(ctx, w->on_change))
        return;
    ev = JS_NewObject(ctx);
    if (JS_IsException(ev))
        return;
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "path", JS_NewString(ctx, path));
    a[0] = ev;
    r = JS_Call(ctx, w->on_change, JS_UNDEFINED, 1, a);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, ev);
}

static void dyn_watch_rescan(dyn_watch_t *w)
{
    dyn_watch_ent_t *old = w->snap;
    size_t n_old = w->n_snap, i, j;

    /* Clear the marks the LAST diff left behind. Without this every entry is
       already `seen`, the match loop skips it, and each rescan re-reports the
       whole tree as new while never reporting a deletion. */
    for (i = 0; i < n_old; i++)
        old[i].seen = 0;

    w->snap = NULL;
    w->n_snap = w->cap_snap = 0;
    if (dyn_watch_walk(w, w->root, "", 0) < 0) {
        dyn_watch_snap_free(w);
        w->snap = old;
        w->n_snap = n_old;
        return;                          /* keep the old snapshot on failure */
    }
    /* O(n*m), and deliberately so: the alternative is a hash table whose build
       cost lands on every rescan. Trees this watches are thousands, not millions. */
    for (i = 0; i < w->n_snap; i++) {
        dyn_watch_ent_t *ne = &w->snap[i];
        for (j = 0; j < n_old; j++) {
            if (old[j].seen || strcmp(old[j].path, ne->path) != 0)
                continue;
            old[j].seen = 1;
            ne->seen = 1;
            if (!ne->is_dir &&
                (old[j].mtime != ne->mtime || old[j].mtime_ns != ne->mtime_ns ||
                 old[j].size != ne->size || old[j].ino != ne->ino))
                dyn_watch_emit(w, "change", ne->path);
            break;
        }
        if (!ne->seen)
            dyn_watch_emit(w, ne->is_dir ? "addDir" : "add", ne->path);
    }
    for (j = 0; j < n_old; j++)
        if (!old[j].seen)
            dyn_watch_emit(w, old[j].is_dir ? "unlinkDir" : "unlink", old[j].path);

    for (j = 0; j < n_old; j++)
        free(old[j].path);
    free(old);
}

/* Runs on the shared reactor's drain hook, so a tree that changes while nothing
 * else touches the loop is still swept. */
static void dyn_watch_tick(void *udata)
{
    dyn_watch_t *w = (dyn_watch_t *)udata;
    if (w->closed || !w->dirty)
        return;
    if (dyn_timer_now_ms() - w->dirty_at < w->debounce_ms)
        return;                          /* still coalescing this burst */
    w->dirty = 0;
    dyn_watch_rescan(w);
}

/* ---- lifetime ---------------------------------------------------------- */

static void dyn_watch_disarm(dyn_watch_t *w)
{
    dyn_watch_dir_t *d = w->dirs;
    while (d) {
        dyn_watch_dir_t *next = d->next;
        if (w->lp)
            dyn_evloop_del(w->lp, d->fd);
        close(d->fd);
        free(d->path);
        free(d);
        d = next;
    }
    w->dirs = NULL;
#ifdef __linux__
    if (w->ifd >= 0) {
        if (w->lp)
            dyn_evloop_del(w->lp, w->ifd);
        close(w->ifd);
        w->ifd = -1;
    }
#endif
}

static void dyn_watch_dispose(void *native)
{
    dyn_watch_t *w = (dyn_watch_t *)native;
    size_t i;

    w->closed = 1;
    dyn_watch_disarm(w);
    if (w->hooked)
        dyn_net_off_drain(w);
    if (w->aio)
        dyn_net_reactor_release(w->ctx);
    dyn_watch_snap_free(w);
    for (i = 0; i < w->n_ignore; i++)
        free(w->ignore[i]);
    free(w->ignore);
    JS_FreeValue(w->ctx, w->on_change);
    free(w->root);
    free(w);
}

static JSValue dyn_watch_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    dyn_watch_t *w;
    const char *root;
    JSValue res;
    (void)new_target;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Watcher(path[, options])");
    /* Coerce every argument to a C local BEFORE any resource exists: coercion
       runs arbitrary user code. */
    root = dyn_path_borrow(ctx, argv[0], "Watcher(path)", NULL);
    if (!root)
        return JS_EXCEPTION;

    w = (dyn_watch_t *)calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    w->ctx = ctx;
    w->on_change = JS_UNDEFINED;
    w->self = JS_UNDEFINED;
    w->recursive = 1;
    w->debounce_ms = 50;
    w->root = strdup(root);
#ifdef __linux__
    w->ifd = -1;
#endif
    if (!w->root) {
        free(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        if (!JS_IsUndefined(v))
            w->recursive = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "debounceMs");
        if (!JS_IsUndefined(v)) {
            int32_t ms = 0;
            if (JS_ToInt32(ctx, &ms, v) == 0 && ms >= 0)
                w->debounce_ms = (uint64_t)ms;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "ignore");
        if (JS_IsArray(ctx, v)) {
            uint32_t n = 0, i;
            JSValue jl = JS_GetPropertyStr(ctx, v, "length");
            JS_ToUint32(ctx, &n, jl);
            JS_FreeValue(ctx, jl);
            w->ignore = (char **)calloc(n ? n : 1, sizeof(char *));
            for (i = 0; w->ignore && i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, i);
                const char *s = JS_ToCString(ctx, e);
                if (s) {
                    w->ignore[w->n_ignore] = strdup(s);
                    if (w->ignore[w->n_ignore])
                        w->n_ignore++;
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, v);
    }

    w->aio = dyn_net_reactor_acquire(ctx);
    if (!w->aio) {
        dyn_watch_dispose(w);
        return JS_ThrowInternalError(ctx, "no reactor");
    }
    w->lp = dyn_aio_evloop(w->aio);

    res = dyn_res_wrap(ctx, dyn_watch_class_id, w, dyn_watch_dispose);
    return res;
}

static JSValue dyn_watch_start(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_watch_t *w = (dyn_watch_t *)dyn_res_native(ctx, this_val,
                                                   dyn_watch_class_id);
    struct stat st;

    if (!w)
        return JS_EXCEPTION;
    if (argc > 0 && JS_IsFunction(ctx, argv[0])) {
        JS_FreeValue(ctx, w->on_change);
        w->on_change = JS_DupValue(ctx, argv[0]);
    }
    if (w->dirs
#ifdef __linux__
        || w->ifd >= 0
#endif
        )
        return JS_ThrowInternalError(ctx, "already started");
    if (stat(w->root, &st) != 0 || !S_ISDIR(st.st_mode))
        return JS_ThrowTypeError(ctx, "Watcher: not a directory");
#ifdef __linux__
    w->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->ifd < 0)
        return JS_ThrowInternalError(ctx, "inotify_init failed");
    if (dyn_evloop_add(w->lp, w->ifd, DYN_EV_READ, dyn_watch_on_inotify, w) < 0) {
        close(w->ifd);
        w->ifd = -1;
        return JS_ThrowInternalError(ctx, "cannot watch: reactor refused the fd");
    }
#endif
    /* The first walk is the baseline, so nothing already present is reported. */
    if (dyn_watch_walk(w, w->root, "", 0) < 0) {
        dyn_watch_disarm(w);
        return JS_ThrowInternalError(ctx,
            "cannot watch: out of watch descriptors "
            "(Linux: fs.inotify.max_user_watches)");
    }
    /* The sweep is time-driven: without its own tick it would run only when
       other traffic wakes the loop, which is exactly the quiet tree that needs
       it. The return is checked for that reason. */
    if (dyn_net_on_drain(dyn_watch_tick, w) < 0) {
        dyn_watch_disarm(w);
        return JS_ThrowInternalError(ctx,
            "cannot watch: the backend cannot arm a clock, so the debounce "
            "sweep would never fire");
    }
    w->hooked = 1;
    return JS_UNDEFINED;
}

static JSValue dyn_watch_stats(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_watch_t *w = (dyn_watch_t *)dyn_res_native(ctx, this_val,
                                                   dyn_watch_class_id);
    JSValue o;
    size_t ndirs = 0;
    dyn_watch_dir_t *d;
    (void)argc; (void)argv;

    if (!w)
        return JS_EXCEPTION;
    for (d = w->dirs; d; d = d->next)
        ndirs++;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "entries", JS_NewInt64(ctx, (int64_t)w->n_snap));
    JS_SetPropertyStr(ctx, o, "directories", JS_NewInt64(ctx, (int64_t)ndirs));
    JS_SetPropertyStr(ctx, o, "events", JS_NewInt64(ctx, (int64_t)w->n_events));
    JS_SetPropertyStr(ctx, o, "truncated", JS_NewBool(ctx, w->truncated));
    JS_SetPropertyStr(ctx, o, "debounceMs", JS_NewInt64(ctx,
                                                (int64_t)w->debounce_ms));
    return o;
}

static const JSCFunctionListEntry dyn_watch_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_watch_start),
    JS_CFUNC_DEF("stats", 0, dyn_watch_stats),
};
