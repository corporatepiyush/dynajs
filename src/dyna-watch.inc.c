#include <dirent.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

#define DYN_WATCH_MAX_ENTRIES 100000
#define DYN_WATCH_MAX_DEPTH 64
#define DYN_WATCH_MAX_FDS 4096

typedef struct {
    char* path;
    ino_t ino;
    int64_t mtime;
    long mtime_ns;
    off_t size;
    unsigned is_dir : 1;
    unsigned seen : 1;
} dyn_watch_ent_t;

typedef struct dyn_watch_dir {
    struct dyn_watch_dir* next;
    int fd;
    char* path;
} dyn_watch_dir_t;

#define DYN_WATCH_QMAX 8192

typedef struct dyn_watch_q {
    struct dyn_watch_q* next;
    JSValue ev;
} dyn_watch_q_t;

typedef struct dyn_watch_wait {
    struct dyn_watch_wait* next;
    JSValue resolve, reject;
} dyn_watch_wait_t;

typedef struct {
    JSContext* ctx;
    dyn_aio_t* aio;
    struct dyn_evloop* lp;
    char* root;
    JSValue on_change;
    dyn_watch_ent_t* snap;
    size_t n_snap, cap_snap;
    char** ignore;
    size_t n_ignore;
    dyn_watch_dir_t* dirs;
    uint64_t debounce_ms, dirty_at;
    dyn_watch_wait_t *whead, *wtail;
    dyn_watch_q_t *qhead, *qtail;
    size_t n_q;
    int recursive, dirty, closed, hooked, truncated;
    int in_emit;
    int in_gc;
    int busy;
    int dead;
    int iter_on;
    int iter_done;
    int stop_req;
    uint64_t n_events;
#ifdef __linux__
    int ifd;
#endif
} dyn_watch_t;

static JSClassID dyn_watch_class_id;

static void dyn_watch_dispose(void* native);

static void dyn_watch_leave(dyn_watch_t* w)
{
    if (--w->busy == 0 && w->dead)
        dyn_watch_dispose(w);
}

static void dyn_watch_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_watch_class_id);
    dyn_watch_t* w = (r && !r->closed) ? (dyn_watch_t*)r->native : NULL;
    if (w) {
        dyn_watch_wait_t* wt;
        dyn_watch_q_t* q;
        JS_MarkValue(rt, w->on_change, mark_func);
        for (wt = w->whead; wt; wt = wt->next) {
            JS_MarkValue(rt, wt->resolve, mark_func);
            JS_MarkValue(rt, wt->reject, mark_func);
        }
        for (q = w->qhead; q; q = q->next)
            JS_MarkValue(rt, q->ev, mark_func);
    }
}

static void dyn_watch_class_finalizer(JSRuntime* rt, JSValue val)
{
    JSClassID id;
    DynResource* r = (DynResource*)JS_GetAnyOpaque(val, &id);

    if (r && !r->closed && r->native)
        ((dyn_watch_t*)r->native)->in_gc = 1;
    dyn_res_finalizer(rt, val);
}

static const JSClassDef dyn_watch_class = {
    "Watcher",
    .finalizer = dyn_watch_class_finalizer,
    .gc_mark = dyn_watch_gc_mark,
};

static void dyn_watch_snap_free(dyn_watch_t* w)
{
    size_t i;
    for (i = 0; i < w->n_snap; i++)
        free(w->snap[i].path);
    free(w->snap);
    w->snap = NULL;
    w->n_snap = w->cap_snap = 0;
}

static int dyn_watch_ignored(const dyn_watch_t* w, const char* rel,
    const char* name)
{
    size_t i;
    for (i = 0; i < w->n_ignore; i++)
        if (dyn_glob_match(w->ignore[i], rel) || dyn_glob_match(w->ignore[i], name))
            return 1;
    return 0;
}

static int dyn_watch_push(dyn_watch_t* w, const char* rel, const struct stat* st)
{
    dyn_watch_ent_t* e;
    if (w->n_snap == w->cap_snap) {
        size_t nc = w->cap_snap ? w->cap_snap * 2 : 64;
        dyn_watch_ent_t* n = (dyn_watch_ent_t*)realloc(w->snap, nc * sizeof(*n));
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
    e->mtime_ns = DYN_STAT_MTIM((*st)).tv_nsec;
    e->size = st->st_size;
    e->is_dir = S_ISDIR(st->st_mode) ? 1u : 0u;
    e->seen = 0;
    w->n_snap++;
    return 0;
}

static int dyn_watch_arm_entry(dyn_watch_t* w, const char* abs, int is_dir);

static int dyn_watch_walk(dyn_watch_t* w, const char* abs, const char* rel,
    int depth)
{
    DIR* d;
    struct dirent* de;

    if (depth > DYN_WATCH_MAX_DEPTH)
        return 0;
    d = opendir(abs);
    if (!d)
        return 0;
    if (dyn_watch_arm_entry(w, abs, 1) < 0) {
        closedir(d);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char *cabs, *crel;
        struct stat st;
        if (de->d_name[0] == '.' && (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
            continue;
        if (w->n_snap >= DYN_WATCH_MAX_ENTRIES) {
            w->truncated = 1;
            break;
        }
        cabs = dyn_join(abs, de->d_name);
        if (!cabs) {
            closedir(d);
            return -1;
        }
        crel = *rel ? dyn_join(rel, de->d_name) : strdup(de->d_name);
        if (!crel) {
            free(cabs);
            closedir(d);
            return -1;
        }
        if (dyn_watch_ignored(w, crel, de->d_name)) {
            free(cabs);
            free(crel);
            continue;
        }
        if (lstat(cabs, &st) != 0) {
            free(cabs);
            free(crel);
            continue;
        }
        if (dyn_watch_push(w, crel, &st) < 0) {
            free(cabs);
            free(crel);
            closedir(d);
            return -1;
        }
        if (!S_ISDIR(st.st_mode) && dyn_watch_arm_entry(w, cabs, 0) < 0) {
            free(cabs);
            free(crel);
            closedir(d);
            return -1;
        }
        if (S_ISDIR(st.st_mode) && w->recursive) {
            if (dyn_watch_walk(w, cabs, crel, depth + 1) < 0) {
                free(cabs);
                free(crel);
                closedir(d);
                return -1;
            }
        }
        free(cabs);
        free(crel);
    }
    closedir(d);
    return 0;
}

static void dyn_watch_mark(dyn_watch_t* w)
{
    w->dirty = 1;
    w->dirty_at = dyn_timer_now_ms();
}

#ifdef __linux__
static void dyn_watch_drain_mark(int fd, void* udata)
{
    dyn_watch_t* w = (dyn_watch_t*)udata;
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        ;
    (void)n;
    dyn_watch_mark(w);
}

#if defined(CONFIG_IO_URING)
static void dyn_watch_on_inotify_uring(dyn_aio_t* aio, int fd, void* ud)
{
    (void)aio;
    dyn_watch_drain_mark(fd, ud);
}
#else
static void dyn_watch_on_inotify(struct dyn_evloop* lp, int fd, int events,
    void* udata)
{
    (void)lp;
    (void)events;
    dyn_watch_drain_mark(fd, udata);
}
#endif

static int dyn_watch_arm_entry(dyn_watch_t* w, const char* abs, int is_dir)
{
    if (w->ifd < 0 || !is_dir)
        return 0;
    if (inotify_add_watch(w->ifd, abs,
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF)
        < 0) {
        if (errno == ENOENT)
            return 0;
        if (errno == ENOSPC)
            return -1;
        w->truncated = 1;
        return 0;
    }
    return 0;
}
#else
static void dyn_watch_on_vnode(struct dyn_evloop* lp, int fd, int events,
    void* udata)
{
    dyn_watch_t* w = (dyn_watch_t*)udata;
    (void)lp;
    (void)fd;
    (void)events;
    dyn_watch_mark(w);
}

static int dyn_watch_arm_entry(dyn_watch_t* w, const char* abs, int is_dir)
{
    dyn_watch_dir_t* d;
    int fd;
    size_t n = 0;

    (void)is_dir;
    for (d = w->dirs; d; d = d->next, n++)
        if (strcmp(d->path, abs) == 0)
            return 0;
    if (n >= DYN_WATCH_MAX_FDS) {
        w->truncated = 1;
        return 0;
    }
    fd = open(abs, O_EVTONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            w->truncated = 1;
        return 0;
    }
    d = (dyn_watch_dir_t*)calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return -1;
    }
    d->fd = fd;
    d->path = strdup(abs);
    if (!d->path) {
        close(fd);
        free(d);
        return -1;
    }
    if (dyn_evloop_add(w->lp, fd, DYN_EV_VNODE, dyn_watch_on_vnode, w) < 0) {
        close(fd);
        free(d->path);
        free(d);
        return -1;
    }
    d->next = w->dirs;
    w->dirs = d;
    return 0;
}
#endif

static JSValue dyn_watch_event_new(JSContext* ctx, const char* kind,
    const char* path)
{
    JSValue ev, pv, kv;

    ev = JS_NewObject(ctx);
    if (JS_IsException(ev))
        return ev;
    pv = JS_NewString(ctx, path);
    if (JS_IsException(pv)) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    kv = JS_NewString(ctx, kind);
    if (JS_IsException(kv)) {
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, ev, "path", pv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, ev, "kind", kv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

static void dyn_watch_settle_one(dyn_watch_t* w, dyn_watch_wait_t* wt,
    JSValue res)
{
    JSContext* ctx = w->ctx;
    JSValue arg, r;

    if (JS_IsException(res)) {
        arg = JS_GetException(ctx);
        r = JS_Call(ctx, wt->reject, JS_UNDEFINED, 1, &arg);
    } else {
        arg = res;
        r = JS_Call(ctx, wt->resolve, JS_UNDEFINED, 1, &arg);
    }
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, wt->resolve);
    JS_FreeValue(ctx, wt->reject);
    free(wt);
}

static JSValue dyn_watch_result_new(JSContext* ctx, JSValue val, int done)
{
    JSValue res = JS_NewObject(ctx);

    if (JS_IsException(res)) {
        JS_FreeValue(ctx, val);
        return res;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "value", val, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static void dyn_watch_pump(dyn_watch_t* w)
{
    JSContext* ctx = w->ctx;

    w->busy++;
    while (w->whead && w->qhead) {
        dyn_watch_wait_t* wt = w->whead;
        dyn_watch_q_t* q = w->qhead;
        JSValue res;

        w->whead = wt->next;
        if (!w->whead)
            w->wtail = NULL;
        w->qhead = q->next;
        if (!w->qhead)
            w->qtail = NULL;
        w->n_q--;
        res = dyn_watch_result_new(ctx, q->ev, 0);
        free(q);
        dyn_watch_settle_one(w, wt, res);
    }
    dyn_watch_leave(w);
}

static void dyn_watch_settle_done(dyn_watch_t* w)
{
    JSContext* ctx = w->ctx;

    while (w->whead) {
        dyn_watch_wait_t* wt = w->whead;
        JSValue res;

        w->whead = wt->next;
        if (!w->whead)
            w->wtail = NULL;
        res = dyn_watch_result_new(ctx, JS_UNDEFINED, 1);
        dyn_watch_settle_one(w, wt, res);
    }
}

static void dyn_watch_wait_clear(dyn_watch_t* w)
{
    while (w->whead) {
        dyn_watch_wait_t* wt = w->whead;
        w->whead = wt->next;
        JS_FreeValue(w->ctx, wt->resolve);
        JS_FreeValue(w->ctx, wt->reject);
        free(wt);
    }
    w->wtail = NULL;
}

static void dyn_watch_q_clear(dyn_watch_t* w)
{
    while (w->qhead) {
        dyn_watch_q_t* q = w->qhead;
        w->qhead = q->next;
        JS_FreeValue(w->ctx, q->ev);
        free(q);
    }
    w->qtail = NULL;
    w->n_q = 0;
}

static void dyn_watch_emit(dyn_watch_t* w, const char* kind, const char* path)
{
    JSContext* ctx = w->ctx;
    JSValue ev, r;
    JSValueConst a[1];

    if (w->closed || w->stop_req)
        return;
    w->n_events++;
    ev = dyn_watch_event_new(ctx, kind, path);
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        w->truncated = 1;
        return;
    }
    if (JS_IsFunction(ctx, w->on_change)) {
        a[0] = ev;
        w->in_emit = 1;
        r = JS_Call(ctx, w->on_change, JS_UNDEFINED, 1, a);
        w->in_emit = 0;
        JS_FreeValue(ctx, r);
    }
    if (w->iter_on && !w->iter_done) {
        if (w->n_q >= DYN_WATCH_QMAX) {
            w->truncated = 1;
        } else {
            dyn_watch_q_t* q = (dyn_watch_q_t*)malloc(sizeof(*q));
            if (!q) {
                w->truncated = 1;
            } else {
                q->ev = JS_DupValue(ctx, ev);
                q->next = NULL;
                if (w->qtail)
                    w->qtail->next = q;
                else
                    w->qhead = q;
                w->qtail = q;
                w->n_q++;
            }
        }
        dyn_watch_pump(w);
    }
    JS_FreeValue(ctx, ev);
}

static void dyn_watch_emit(dyn_watch_t* w, const char* kind, const char* path);

static void dyn_watch_dispose(void* native);

static void dyn_watch_disarm(dyn_watch_t* w);

static void dyn_watch_halt(dyn_watch_t* w)
{
    w->busy++;
    dyn_watch_disarm(w);
    if (w->hooked) {
        dyn_net_off_drain(w);
        w->hooked = 0;
    }
    dyn_watch_snap_free(w);
    w->dirty = 0;
    w->iter_done = 1;
    dyn_watch_pump(w);
    dyn_watch_settle_done(w);
    dyn_watch_leave(w);
}

static void dyn_watch_iter_begin(dyn_watch_t* w)
{
    w->iter_on = 1;
}

static JSValue dyn_watch_iter_result(JSContext* ctx, JSValue val, int done)
{
    return dyn_watch_result_new(ctx, val, done);
}

static JSValue dyn_watch_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue funcs[2], promise, res, arg;
    dyn_watch_t* w;
    (void)argc;
    (void)argv;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    w = (dyn_watch_t*)0;
    {
        DynResource* r = (DynResource*)JS_GetOpaque(this_val,
            dyn_watch_class_id);
        w = (r && !r->closed) ? (dyn_watch_t*)r->native : NULL;
    }
    if (!w) {
        res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
        goto settle;
    }
    dyn_watch_iter_begin(w);
    if (w->qhead) {
        dyn_watch_q_t* q = w->qhead;
        w->qhead = q->next;
        if (!w->qhead)
            w->qtail = NULL;
        w->n_q--;
        res = dyn_watch_iter_result(ctx, q->ev, 0);
        free(q);
        goto settle;
    }
    if (w->iter_done) {
        res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
        goto settle;
    }
    {
        dyn_watch_wait_t* wt = (dyn_watch_wait_t*)calloc(1, sizeof(*wt));
        if (!wt) {
            JS_FreeValue(ctx, promise);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return JS_ThrowOutOfMemory(ctx);
        }
        wt->resolve = funcs[0];
        wt->reject = funcs[1];
        if (JS_DefinePropertyValueStr(ctx, promise, "..watcher",
                JS_DupValue(ctx, this_val), 0)
            < 0) {
            JS_FreeValue(ctx, promise);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            free(wt);
            return JS_EXCEPTION;
        }
        if (w->wtail)
            w->wtail->next = wt;
        else
            w->whead = wt;
        w->wtail = wt;
        return promise;
    }

settle:
    if (JS_IsException(res)) {
        arg = JS_GetException(ctx);
        res = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &arg);
    } else {
        arg = res;
        res = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &arg);
    }
    if (JS_IsException(res))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue dyn_watch_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue funcs[2], promise, res, arg;
    dyn_watch_t* w;
    DynResource* r;
    (void)argc;
    (void)argv;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    r = (DynResource*)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t*)r->native : NULL;
    if (w) {
        dyn_watch_iter_begin(w);
        w->iter_done = 1;
        dyn_watch_q_clear(w);
        dyn_watch_settle_done(w);
        if (w->in_emit) {
            w->stop_req = 1;
        } else {
            dyn_watch_halt(w);
        }
    }
    res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
    if (JS_IsException(res)) {
        arg = JS_GetException(ctx);
        res = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &arg);
    } else {
        arg = res;
        res = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &arg);
    }
    if (JS_IsException(res))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue dyn_watch_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* r;
    dyn_watch_t* w;
    (void)argc;
    (void)argv;

    r = (DynResource*)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t*)r->native : NULL;
    if (w)
        dyn_watch_iter_begin(w);
    return JS_DupValue(ctx, this_val);
}

static void dyn_watch_dispose(void* native);
static void dyn_watch_halt(dyn_watch_t* w);

static int dyn_watch_ent_cmp(const void* a, const void* b)
{
    return strcmp(((const dyn_watch_ent_t*)a)->path,
        ((const dyn_watch_ent_t*)b)->path);
}

static void dyn_watch_rescan(dyn_watch_t* w)
{
    dyn_watch_ent_t* old = w->snap;
    size_t n_old = w->n_snap, i, j;

    w->busy++;

    w->snap = NULL;
    w->n_snap = w->cap_snap = 0;
    if (dyn_watch_walk(w, w->root, "", 0) < 0) {
        dyn_watch_snap_free(w);
        w->snap = old;
        w->n_snap = n_old;
        dyn_watch_leave(w);
        return;
    }

    if (n_old)
        qsort(old, n_old, sizeof(*old), dyn_watch_ent_cmp);
    if (w->n_snap)
        qsort(w->snap, w->n_snap, sizeof(*w->snap), dyn_watch_ent_cmp);

    i = 0;
    j = 0;
    while (i < n_old && j < w->n_snap) {
        int c = strcmp(old[i].path, w->snap[j].path);
        if (c < 0) {
            dyn_watch_emit(w, old[i].is_dir ? "unlinkDir" : "unlink",
                old[i].path);
            i++;
        } else if (c > 0) {
            dyn_watch_ent_t* ne = &w->snap[j];
            dyn_watch_emit(w, ne->is_dir ? "addDir" : "add", ne->path);
            j++;
        } else {
            dyn_watch_ent_t* ne = &w->snap[j];
            if (!ne->is_dir && (old[i].mtime != ne->mtime || old[i].mtime_ns != ne->mtime_ns || old[i].size != ne->size || old[i].ino != ne->ino))
                dyn_watch_emit(w, "change", ne->path);
            i++;
            j++;
        }
        if (w->closed || w->stop_req)
            goto dropped;
    }
    for (; i < n_old; i++) {
        dyn_watch_emit(w, old[i].is_dir ? "unlinkDir" : "unlink",
            old[i].path);
        if (w->closed || w->stop_req)
            break;
    }
    for (; j < w->n_snap; j++) {
        dyn_watch_ent_t* ne = &w->snap[j];
        dyn_watch_emit(w, ne->is_dir ? "addDir" : "add", ne->path);
        if (w->closed || w->stop_req)
            break;
    }

dropped:
    for (j = 0; j < n_old; j++)
        free(old[j].path);
    free(old);
    if (w->closed)
        dyn_watch_dispose(w);
    else if (w->stop_req) {
        w->stop_req = 0;
        dyn_watch_halt(w);
    }
    dyn_watch_leave(w);
}

static void dyn_watch_tick(void* udata)
{
    dyn_watch_t* w = (dyn_watch_t*)udata;
    if (w->closed || w->stop_req || !w->dirty)
        return;
    if (dyn_timer_now_ms() - w->dirty_at < w->debounce_ms)
        return;
    w->dirty = 0;
    dyn_watch_rescan(w);
}

static void dyn_watch_disarm(dyn_watch_t* w)
{
    dyn_watch_dir_t* d = w->dirs;
    while (d) {
        dyn_watch_dir_t* next = d->next;
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
#if defined(CONFIG_IO_URING)
        if (w->aio)
            dyn_aio_unwatch_fd(w->aio, w->ifd);
#else
        if (w->lp)
            dyn_evloop_del(w->lp, w->ifd);
#endif
        close(w->ifd);
        w->ifd = -1;
    }
#endif
}

static void dyn_watch_dispose(void* native)
{
    dyn_watch_t* w = (dyn_watch_t*)native;
    size_t i;
    JSRuntime* rt = w->ctx ? JS_GetRuntime(w->ctx) : NULL;

    if (w->in_emit || w->busy) {
        w->closed = 1;
        w->iter_done = 1;
        w->dead = 1;
        return;
    }
    w->closed = 1;
    w->iter_done = 1;
    dyn_watch_disarm(w);
    if (w->hooked) {
        dyn_net_off_drain(w);
        w->hooked = 0;
    }
    if (w->aio)
        dyn_net_reactor_release(w->ctx);
    dyn_watch_snap_free(w);
    if (w->in_gc)
        dyn_watch_wait_clear(w);
    else
        dyn_watch_settle_done(w);
    dyn_watch_q_clear(w);
    for (i = 0; i < w->n_ignore; i++)
        free(w->ignore[i]);
    free(w->ignore);
    if (rt) {
        JS_FreeValueRT(rt, w->on_change);
    } else {
        JS_FreeValue(w->ctx, w->on_change);
    }
    free(w->root);
    free(w);
}

static JSValue dyn_watch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_watch_t* w;
    const char* root;
    JSValue res;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Watcher(path[, options])");
    root = dyn_path_borrow(ctx, argv[0], "Watcher(path)", NULL);
    if (!root)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Watcher(path, options): options must be an object");

    w = (dyn_watch_t*)calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    w->ctx = ctx;
    w->on_change = JS_UNDEFINED;
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
        static const char* const keys[] = { "recursive", "debounceMs", "ignore" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 3)) {
            dyn_watch_dispose(w);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        if (!JS_IsUndefined(v)) {
            b = JS_ToBool(ctx, v);
            if (b < 0) {
                JS_FreeValue(ctx, v);
                dyn_watch_dispose(w);
                return JS_EXCEPTION;
            }
            w->recursive = b;
        }
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
            w->ignore = (char**)calloc(n ? n : 1, sizeof(char*));
            for (i = 0; w->ignore && i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, i);
                const char* s = JS_ToCString(ctx, e);
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
#if defined(__linux__) && defined(CONFIG_IO_URING)
#else
    w->lp = dyn_aio_evloop(w->aio);
    if (!w->lp) {
        dyn_watch_dispose(w);
        return JS_ThrowInternalError(ctx,
            "watch: not available on this build's reactor (io_uring has no "
            "vnode interest); build without CONFIG_IO_URING to use it");
    }
#endif

    res = dyn_res_wrap(ctx, new_target, dyn_watch_class_id, w, dyn_watch_dispose);
    return res;
}

static JSValue dyn_watch_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_watch_t* w = (dyn_watch_t*)dyn_res_native(ctx, this_val,
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
    {
        int added;
#if defined(CONFIG_IO_URING)
        added = dyn_aio_watch_fd(w->aio, w->ifd, dyn_watch_on_inotify_uring, w);
#else
        added = dyn_evloop_add(w->lp, w->ifd, DYN_EV_READ, dyn_watch_on_inotify, w);
#endif
        if (added < 0) {
            close(w->ifd);
            w->ifd = -1;
            return JS_ThrowInternalError(ctx,
                "cannot watch: reactor refused the fd");
        }
    }
#endif
    if (dyn_watch_walk(w, w->root, "", 0) < 0) {
        dyn_watch_disarm(w);
        return JS_ThrowInternalError(ctx,
            "cannot watch: out of watch descriptors "
            "(Linux: fs.inotify.max_user_watches)");
    }
    if (dyn_net_on_drain(dyn_watch_tick, w) < 0) {
        dyn_watch_disarm(w);
        return JS_ThrowInternalError(ctx,
            "cannot watch: the backend cannot arm a clock, so the debounce "
            "sweep would never fire");
    }
    w->hooked = 1;
    w->iter_done = 0;
    return JS_UNDEFINED;
}

static JSValue dyn_watch_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* r;
    dyn_watch_t* w;
    (void)argc;
    (void)argv;

    r = (DynResource*)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t*)r->native : NULL;
    if (!w)
        return JS_UNDEFINED;
    if (w->in_emit) {
        w->stop_req = 1;
        return JS_UNDEFINED;
    }
    dyn_watch_halt(w);
    return JS_UNDEFINED;
}

static JSValue dyn_watch_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_watch_t* w = (dyn_watch_t*)dyn_res_native(ctx, this_val,
        dyn_watch_class_id);
    JSValue o;
    size_t ndirs = 0;
    dyn_watch_dir_t* d;
    (void)argc;
    (void)argv;

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
    JS_SetPropertyStr(ctx, o, "debounceMs", JS_NewInt64(ctx, (int64_t)w->debounce_ms));
    return o;
}

static const JSCFunctionListEntry dyn_watch_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_watch_start),
    JS_CFUNC_DEF("stop", 0, dyn_watch_stop),
    JS_CFUNC_DEF("stats", 0, dyn_watch_stats),
    JS_CFUNC_DEF("next", 0, dyn_watch_next),
    JS_CFUNC_DEF("return", 0, dyn_watch_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, dyn_watch_async_iterator),
};
