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

/*the async-iteration side. Events go to BOTH surfaces -- the
 * start() callback and the for-await queue -- and the queue only starts
 * collecting once iteration begins (first next()/[Symbol.asyncIterator]),
 * so a callback-only watcher never accumulates. One pending-consumer FIFO
 * and one event FIFO: next() resolves in arrival order, and a burst the
 * consumer has not pulled yet parks in the queue up to DYN_WATCH_QMAX (past
 * which the NEWEST event is dropped and `truncated` says so -- the same
 * "report it, never go silent" rule the snapshot cap follows). */
#define DYN_WATCH_QMAX 8192

typedef struct dyn_watch_q {
    struct dyn_watch_q *next;
    JSValue ev;
} dyn_watch_q_t;

typedef struct dyn_watch_wait {
    struct dyn_watch_wait *next;
    JSValue resolve, reject;
} dyn_watch_wait_t;

typedef struct {
    JSContext *ctx;
    dyn_aio_t *aio;
    struct dyn_evloop *lp;
    char *root;
    JSValue on_change;
    dyn_watch_ent_t *snap;
    size_t n_snap, cap_snap;
    char **ignore;
    size_t n_ignore;
    dyn_watch_dir_t *dirs;
    uint64_t debounce_ms, dirty_at;
    dyn_watch_wait_t *whead, *wtail;    /* parked next() calls, FIFO */
    dyn_watch_q_t *qhead, *qtail;       /* buffered events, FIFO */
    size_t n_q;
    int recursive, dirty, closed, hooked, truncated;
    int in_emit;                /* a change callback is on the stack: dispose
                                   defers to the rescan sweep (see dispose) */
    int in_gc;                  /* the class finalizer is tearing this down;
                                   no JS may run (see dyn_watch_dispose) */
    int busy;                   /* a sweep/pump/halt frame is walking `w`:
                                   teardown defers to its exit */
    int dead;                   /* dispose arrived while busy (or in a
                                   callback): free at the outermost leave */
    int iter_on;                /* iteration has begun: buffer for it */
    int iter_done;              /* the event stream is finished */
    int stop_req;               /* stop() from inside a callback: deferred to
                                   the sweep (same shape as dispose) */
    uint64_t n_events;
#ifdef __linux__
    int ifd;
#endif
} dyn_watch_t;

static JSClassID dyn_watch_class_id;

/* Defined below; dyn_watch_dispose is the res layer's teardown callback. */
static void dyn_watch_dispose(void *native);

/* A walker enters/leaves around any frame that keeps using `w` after JS
 * values are freed (the sweep, the pump, halt): freeing a waiter or an event
 * can cascade into the watcher's own finalizer (a pull promise that held the
 * watcher's last reference dying in the cascade), and dispose must then
 * DEFER instead of freeing `w` under the walker's feet. The outermost leave
 * runs the deferred teardown. Same doctrine as in_emit, minus the callback. */
static void dyn_watch_leave(dyn_watch_t *w)
{
    if (--w->busy == 0 && w->dead)
        dyn_watch_dispose(w);
}

static void dyn_watch_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    /* The opaque is the DynResource BOX, not the native: reading the native
       struct straight from it read 8 bytes past the 32-byte box (ASan:
       heap-buffer-overflow at the on_change load), and marking a CLOSED
       watcher would walk the freed native. Same shape as dyn_tcp_gc_mark. */
    DynResource *r = (DynResource *)JS_GetOpaque(val, dyn_watch_class_id);
    dyn_watch_t *w = (r && !r->closed) ? (dyn_watch_t *)r->native : NULL;
    if (w) {
        dyn_watch_wait_t *wt;
        dyn_watch_q_t *q;
        JS_MarkValue(rt, w->on_change, mark_func);
        /*parked next resolvers and buffered events are live JS the
           queue owns; miss them here and a GC during a long watch collects a
           pending Promise's closures out from under the iteration. The
           WATCHER side needs no pin here: each parked pull's own promise
           holds a hidden reference to the watcher (see dyn_watch_next), so
           any pull that can still be awaited keeps the watcher alive -- and
           when the finalizer does run, every parked pull is unreachable
           garbage and is freed without calling its resolvers. */
        for (wt = w->whead; wt; wt = wt->next) {
            JS_MarkValue(rt, wt->resolve, mark_func);
            JS_MarkValue(rt, wt->reject, mark_func);
        }
        for (q = w->qhead; q; q = q->next)
            JS_MarkValue(rt, q->ev, mark_func);
    }
}

/* The class finalizer runs DURING GC: it must never reach user JS. It flags
   the native first, so dyn_watch_dispose frees any parked pulls silently
   instead of settling them (an explicit close(), which runs in JS context,
   still settles them done). */
static void dyn_watch_class_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    DynResource *r = (DynResource *)JS_GetAnyOpaque(val, &id);

    if (r && !r->closed && r->native)
        ((dyn_watch_t *)r->native)->in_gc = 1;
    dyn_res_finalizer(rt, val);
}

static const JSClassDef dyn_watch_class = {
    "Watcher", .finalizer = dyn_watch_class_finalizer,
    .gc_mark = dyn_watch_gc_mark,
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
    e->mtime_ns = DYN_STAT_MTIM((*st)).tv_nsec;  /* the one same-TU macro */
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
        char *cabs, *crel;
        struct stat st;
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
            continue;
        if (w->n_snap >= DYN_WATCH_MAX_ENTRIES) {
            w->truncated = 1;           /* say so; a silent cap reads as coverage */
            break;
        }
        /* Composed on the heap, not in a fixed 2048-byte snprintf buffer: a
           deep tree silently truncated, which mis-diffed the snapshot (event
           paths that no longer compared equal to their own children). Same
           join helper the rest of this TU uses; OOM fails the walk loudly. */
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
            free(cabs); free(crel);
            continue;                   /* BEFORE descending: walking
                                           node_modules to then ignore it is the
                                           difference between 20 ms and 20 s */
        }
        if (lstat(cabs, &st) != 0) {
            free(cabs); free(crel);
            continue;
        }
        if (dyn_watch_push(w, crel, &st) < 0) {
            free(cabs); free(crel);
            closedir(d);
            return -1;
        }
        if (!S_ISDIR(st.st_mode) && dyn_watch_arm_entry(w, cabs, 0) < 0) {
            free(cabs); free(crel);
            closedir(d);
            return -1;
        }
        /* lstat, so a symlinked directory is an entry and not a subtree: that
           is what stops a symlink loop without a visited set. */
        if (S_ISDIR(st.st_mode) && w->recursive) {
            if (dyn_watch_walk(w, cabs, crel, depth + 1) < 0) {
                free(cabs); free(crel);
                closedir(d);
                return -1;
            }
        }
        free(cabs); free(crel);
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
/* The inotify fd is an ordinary readable fd; the callback's whole job is to
 * drain it (level-trigger: it stays readable until the queue is consumed, so
 * NOT draining spins the loop at 100% CPU) and set the dirty flag -- the
 * diff, not the event, is what classifies. Shared by both registration
 * shapes below; only the callback SIGNATURE differs per backend. */
static void dyn_watch_drain_mark(int fd, void *udata)
{
    dyn_watch_t *w = (dyn_watch_t *)udata;
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        ;
    (void)n;
    dyn_watch_mark(w);
}

#if defined(CONFIG_IO_URING)
/* io_uring backend: no dyn_evloop exists behind the ring, so readability of
 * the inotify fd rides the adapter's own poll-multishot hook. */
static void dyn_watch_on_inotify_uring(dyn_aio_t *aio, int fd, void *ud)
{
    (void)aio;
    dyn_watch_drain_mark(fd, ud);
}
#else
static void dyn_watch_on_inotify(struct dyn_evloop *lp, int fd, int events,
                                 void *udata)
{
    (void)lp; (void)events;
    dyn_watch_drain_mark(fd, udata);
}
#endif

static int dyn_watch_arm_entry(dyn_watch_t *w, const char *abs, int is_dir)
{
    /* One inotify fd covers the tree; each directory is a watch descriptor on
       it. ENOSPC here is the max_user_watches limit and is worth naming. */
    if (w->ifd < 0 || !is_dir)
        return 0;   /* a directory watch already reports its files' IN_MODIFY */
    if (inotify_add_watch(w->ifd, abs,
                          IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM |
                          IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF) < 0) {
        if (errno == ENOENT) return 0; /* raced deletion — diff will report */
        if (errno == ENOSPC)
            return -1;
        /* EACCES, EBADF, etc. — mark degraded rather than going silent */
        w->truncated = 1;
        return 0;
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
    if (fd < 0) {
        if (errno != ENOENT) w->truncated = 1;
        return 0;
    }
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

/*one event object -- { path, kind } -- delivered to BOTH surfaces. */
static JSValue dyn_watch_event_new(JSContext *ctx, const char *kind,
                                   const char *path)
{
    JSValue ev, pv, kv;

    ev = JS_NewObject(ctx);
    if (JS_IsException(ev))
        return ev;
    /* Each string is checked BEFORE it is handed to a define: a failed
       JS_NewString returns the exception SENTINEL, and passing that as the
       property value builds an event object whose `path`/`kind` is a
       poisoned value -- it flows to the callback and the queue as a
       "working" event. Refuse instead: the caller reports the drop. */
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
        /* the define consumed pv on its failure path; kv was never handed
           over, so it is freed here (a `||` chain would short-circuit past
           the second define and strand it) */
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

/* ---------- the settle policy (ONE policy, two carriers) --------------------
 *
 * Every asynchronous hand-off in this module ends in a settle, and every
 * settle -- however it is spelled -- obeys the same three invariants. They are
 * written down once, here, because the failure each one prevents is invisible
 * at the call site and shows up only as a hang or as garbage in user JS:
 *
 *   (a) NEVER settle with the JS_EXCEPTION sentinel as the value. The
 *       sentinel is an encoding of "an exception is pending", not a value: a
 *       resolve() handed the sentinel delivers a poisoned argument (the
 *       caller sees a number where an object was promised), and a reject()
 *       handed it loses the actual failure. The shape is CAPTURE-THEN-REJECT:
 *       take the pending exception with JS_GetException (which also clears
 *       it), then hand THAT to the reject path. See dyn_watch_settle_one.
 *   (b) SETTLE EXACTLY ONCE. A promise capability is a one-shot: a second
 *       call is silently dropped by the promise, which then leaves whatever
 *       the second call was carrying unowned -- and a hostile thenable that
 *       calls back twice must not run a continuation twice. The carriers
 *       below enforce it by consuming the capability (the waiter is freed and
 *       unlinked on the settle) or by a `settled` flag.
 *   (c) SWALLOW a resolver/callback throw at an EVENT-LOOP boundary. The
 *       sweep runs between jobs: a resolver that throws leaves an exception
 *       pending on the context, and the next frame to run inherits it as if
 *       it had thrown -- one user error poisons unrelated code. The failure
 *       is the resolver's; free it and carry on. Inside a JS call frame
 *       (next()/return()), by contrast, the error belongs to the caller and
 *       is propagated, not swallowed.
 *
 * TWO CARRIERS implement that policy, because they do different jobs:
 *
 *   - dyn_watch_settle_one settles a parked pull's promise capability from an
 *     event-loop sweep (the pump, stop/close drain). Its argument is either a
 *     built {value, done} result or the JS_EXCEPTION marker, and invariant (a)
 *     is what turns the marker into a rejection.
 *   - the CPS settle pattern (an await-style helper that parks a C
 *     continuation on a thenable and is resumed by a small callback that
 *     refuses to run twice; it lands with the interop shells). It settles NO
 *     promise -- it drives a C state machine -- so its canonical shape is
 *     `if (settled) return; settled = 1; step(...)`, and it returns undefined
 *     to the thenable rather than a value.
 *
 * Keep both: the promise carrier exists to settle pulls (user-visible
 * promises), the CPS carrier exists to avoid creating a promise per step.
 * They share the policy, not the code. */

/* Settle ONE parked pull and free its waiter. `res` is the {value, done}
 * result to resolve with, or JS_EXCEPTION to REJECT with the captured pending
 * exception (an allocation failure building the result object). Every path
 * SETTLES: a pull left unsettled by an allocation failure is a for-await that
 * hangs forever, and resolving with the exception SENTINEL as the value hands
 * user JS a poisoned argument -- both are exactly the failure shapes this
 * helper exists to make impossible. A resolver that throws is swallowed: this
 * runs on the event-loop sweep, where a leftover pending exception would
 * poison the frames above. */
static void dyn_watch_settle_one(dyn_watch_t *w, dyn_watch_wait_t *wt,
                                 JSValue res)
{
    JSContext *ctx = w->ctx;
    JSValue arg, r;

    if (JS_IsException(res)) {
        arg = JS_GetException(ctx);     /* capture + clear the pending one */
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

/* Build the { value, done } result object for a settling pull. On allocation
 * failure the pending exception is returned as JS_EXCEPTION (the marker), and
 * the caller hands it to dyn_watch_settle_one, which REJECTS with it. */
static JSValue dyn_watch_result_new(JSContext *ctx, JSValue val, int done)
{
    JSValue res = JS_NewObject(ctx);

    if (JS_IsException(res)) {
        JS_FreeValue(ctx, val);
        return res;
    }
    /* JS_DefinePropertyValueStr consumes `val`/the bool on BOTH paths. */
    if (JS_DefinePropertyValueStr(ctx, res, "value", val, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "done", JS_NewBool(ctx, done),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* Hand buffered events to parked next() calls, oldest first. */
static void dyn_watch_pump(dyn_watch_t *w)
{
    JSContext *ctx = w->ctx;

    w->busy++;
    while (w->whead && w->qhead) {
        dyn_watch_wait_t *wt = w->whead;
        dyn_watch_q_t *q = w->qhead;
        JSValue res;

        w->whead = wt->next;
        if (!w->whead)
            w->wtail = NULL;
        w->qhead = q->next;
        if (!w->qhead)
            w->qtail = NULL;
        w->n_q--;
        res = dyn_watch_result_new(ctx, q->ev, 0);  /* transfers the event */
        free(q);
        /* every dequeued pull is SETTLED, with the result or -- on an
           allocation failure -- with the failure itself */
        dyn_watch_settle_one(w, wt, res);
    }
    dyn_watch_leave(w);
}

/* Resolve every parked next() with { done: true }. Non-GC paths only (an
   explicit close/stop/return, or the deferred sweep teardown): this CALLS
   the resolvers, which is user JS. An allocation failure rejects that pull
   with the failure -- it is still SETTLED, never dropped. */
static void dyn_watch_settle_done(dyn_watch_t *w)
{
    JSContext *ctx = w->ctx;

    while (w->whead) {
        dyn_watch_wait_t *wt = w->whead;
        JSValue res;

        w->whead = wt->next;
        if (!w->whead)
            w->wtail = NULL;
        res = dyn_watch_result_new(ctx, JS_UNDEFINED, 1);
        dyn_watch_settle_one(w, wt, res);
    }
}

/* Free every parked pull WITHOUT settling it -- the GC-finalizer path, where
   running a resolver would call user JS during collection. A pull whose
   promise is unreachable (the only shape a finalizer can observe: the
   promise itself pins the watcher alive, see dyn_watch_next) has nobody to
   settle FOR, so dropping the waiter is the whole job. */
static void dyn_watch_wait_clear(dyn_watch_t *w)
{
    while (w->whead) {
        dyn_watch_wait_t *wt = w->whead;
        w->whead = wt->next;
        JS_FreeValue(w->ctx, wt->resolve);
        JS_FreeValue(w->ctx, wt->reject);
        free(wt);
    }
    w->wtail = NULL;
}

/* Drop every buffered event (the return()/close path: the iteration ends
   NOW and undelivered events are gone). */
static void dyn_watch_q_clear(dyn_watch_t *w)
{
    while (w->qhead) {
        dyn_watch_q_t *q = w->qhead;
        w->qhead = q->next;
        JS_FreeValue(w->ctx, q->ev);
        free(q);
    }
    w->qtail = NULL;
    w->n_q = 0;
}

static void dyn_watch_emit(dyn_watch_t *w, const char *kind, const char *path)
{
    JSContext *ctx = w->ctx;
    JSValue ev, r;
    JSValueConst a[1];

    if (w->closed || w->stop_req)
        return;     /* closed/stopped by an earlier callback in this sweep */
    w->n_events++;
    ev = dyn_watch_event_new(ctx, kind, path);
    if (JS_IsException(ev)) {
        /* An allocation failure mid-sweep must not leave the exception
           pending over the callbacks that follow (the sweep is a leaf on the
           event loop and has nowhere to propagate it), and must not end the
           stream. Report it the way the queue-overflow path reports: the
           event is dropped and `truncated` says so. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        w->truncated = 1;
        return;
    }
    /* surface 1: the start() callback */
    if (JS_IsFunction(ctx, w->on_change)) {
        a[0] = ev;
        /* The callback may call close() (or drop the last reference): the
           dispose must not free the watcher while this sweep still walks it,
           so bracket the call with in_emit and let the sweep run the
           deferred teardown once the callback stack is unwound. */
        w->in_emit = 1;
        r = JS_Call(ctx, w->on_change, JS_UNDEFINED, 1, a);
        w->in_emit = 0;
        JS_FreeValue(ctx, r);
    }
    /* surface 2: the async iteration (only once iteration has begun; past
       DYN_WATCH_QMAX the newest event is dropped and `truncated` says so) */
    if (w->iter_on && !w->iter_done) {
        if (w->n_q >= DYN_WATCH_QMAX) {
            w->truncated = 1;
        } else {
            dyn_watch_q_t *q = (dyn_watch_q_t *)malloc(sizeof(*q));
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

static void dyn_watch_emit(dyn_watch_t *w, const char *kind, const char *path);

/* Defined below; dyn_watch_rescan runs the deferred teardown (a close() that
   happened inside a change callback) once its sweep is done. */
static void dyn_watch_dispose(void *native);

/* ----: stop and the async iteration ------------------------------- */

/* Defined with the lifetime section below. */
static void dyn_watch_disarm(dyn_watch_t *w);

/* stop(): halt the WATCH but keep the object alive, re-startable, and
 * drainable. Idempotent. The iteration contract is the Channel close
 * contract: the source ends, events ALREADY buffered still drain through
 * next() in arrival order, and { done: true } comes after the drain; a
 * parked next() with an empty buffer resolves done immediately -- so
 * "stop, then iterate" always terminates. */
static void dyn_watch_halt(dyn_watch_t *w)
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
    dyn_watch_pump(w);          /* buffered events go to parked pulls first */
    dyn_watch_settle_done(w);   /* then the rest of the parked pulls end */
    dyn_watch_leave(w);
}

/* Iteration has begun: events buffer from here. No handle is pinned -- a
 * handle kept inside the native struct only closes a reference CYCLE the
 * collector reclaims anyway. What keeps a live pull alive is the pull's own
 * promise: dyn_watch_next hangs a hidden reference to the watcher on it, so
 * anything that can still await the pull keeps the watcher out of the
 * finalizer, and a dropped pull is collected together with the watcher. */
static void dyn_watch_iter_begin(dyn_watch_t *w)
{
    w->iter_on = 1;
}

/* { value, done } as the iterator protocol spells it. Same builder the
   settle paths use, so the result shape cannot drift between them. */
static JSValue dyn_watch_iter_result(JSContext *ctx, JSValue val, int done)
{
    return dyn_watch_result_new(ctx, val, done);
}

/* next() -> Promise<{ value, done }>: a buffered event, or done, or a park. */
static JSValue dyn_watch_next(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue funcs[2], promise, res, arg;
    dyn_watch_t *w;
    (void)argc; (void)argv;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    w = (dyn_watch_t *)0;
    {
        DynResource *r = (DynResource *)JS_GetOpaque(this_val,
                                                     dyn_watch_class_id);
        w = (r && !r->closed) ? (dyn_watch_t *)r->native : NULL;
    }
    if (!w) {
        /* closed (or foreign this): the stream is over */
        res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
        goto settle;
    }
    dyn_watch_iter_begin(w);
    if (w->qhead) {
        /* the buffer comes before done: stop() still drains (see halt) */
        dyn_watch_q_t *q = w->qhead;
        w->qhead = q->next;
        if (!w->qhead)
            w->qtail = NULL;
        w->n_q--;
        res = dyn_watch_iter_result(ctx, q->ev, 0);  /* transfers the event */
        free(q);
        goto settle;
    }
    if (w->iter_done) {
        res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
        goto settle;
    }
    {   /* park until the next event (or halt/dispose) */
        dyn_watch_wait_t *wt = (dyn_watch_wait_t *)calloc(1, sizeof(*wt));
        if (!wt) {
            JS_FreeValue(ctx, promise);
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return JS_ThrowOutOfMemory(ctx);
        }
        wt->resolve = funcs[0];
        wt->reject = funcs[1];
        /* A parked pull is settled ONLY from non-GC paths (the event pump,
           stop()/return(), or an explicit close), and this hidden,
           non-enumerable promise -> watcher reference is what makes that
           structural: any promise that can still be awaited keeps the
           watcher alive and out of the finalizer. When the whole pull is
           dropped together with the watcher, the collector reclaims both
           and the finalizer frees this waiter WITHOUT calling the
           resolvers -- nothing can be awaiting a promise nobody holds. */
        if (JS_DefinePropertyValueStr(ctx, promise, "..watcher",
                                      JS_DupValue(ctx, this_val), 0) < 0) {
            /* out of memory installing the pin: refuse the park rather than
               hand back a pull nothing would ever settle */
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
    /* `res` is the result object, or the JS_EXCEPTION marker when an
       allocation failure built it. Resolving with the marker would settle the
       promise with a POISONED value; rejecting with the captured failure both
       settles and tells the truth. */
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

/* return() -- what a for-await `break` calls: end the iteration NOW (the
 * buffer is dropped) and stop the watch, unconditionally -- the same
 * cleanup doctrine as pg.queryIter's return(). */
static JSValue dyn_watch_return(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue funcs[2], promise, res, arg;
    dyn_watch_t *w;
    DynResource *r;
    (void)argc; (void)argv;

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    r = (DynResource *)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t *)r->native : NULL;
    if (w) {
        dyn_watch_iter_begin(w);
        w->iter_done = 1;
        dyn_watch_q_clear(w);
        dyn_watch_settle_done(w);
        if (w->in_emit) {
            /* called from inside a change callback: the sweep teardown runs
               the halt once unwound (same deferral as dispose) */
            w->stop_req = 1;
        } else {
            dyn_watch_halt(w);
        }
    }
    res = dyn_watch_iter_result(ctx, JS_UNDEFINED, 1);
    /* same settle contract as next(): reject with the failure on an
       allocation error, never resolve with the exception sentinel */
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

/* [Symbol.asyncIterator]() -> the watcher itself (it has next/return). */
static JSValue dyn_watch_async_iterator(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    DynResource *r;
    dyn_watch_t *w;
    (void)argc; (void)argv;

    r = (DynResource *)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t *)r->native : NULL;
    if (w)
        dyn_watch_iter_begin(w);
    return JS_DupValue(ctx, this_val);
}

/* Defined below; dyn_watch_dispose is the res layer's teardown callback. */
static void dyn_watch_dispose(void *native);
/* And the deferred stop twin. */
static void dyn_watch_halt(dyn_watch_t *w);

static void dyn_watch_rescan(dyn_watch_t *w)
{
    dyn_watch_ent_t *old = w->snap;
    size_t n_old = w->n_snap, i, j;

    w->busy++;
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
        dyn_watch_leave(w);
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
                 old[j].size != ne->size || old[j].ino != ne->ino)) {
                dyn_watch_emit(w, "change", ne->path);
                if (w->closed || w->stop_req)
                    goto dropped;   /* closed/stopped in the callback:
                                       bail safely */
            }
            break;
        }
        if (!ne->seen)
            dyn_watch_emit(w, ne->is_dir ? "addDir" : "add", ne->path);
        if (w->closed || w->stop_req)
            goto dropped;           /* same, for the add/addDir callbacks */
    }
    for (j = 0; j < n_old; j++) {
        if (!old[j].seen) {
            dyn_watch_emit(w, old[j].is_dir ? "unlinkDir" : "unlink",
                           old[j].path);
            if (w->closed || w->stop_req)
                break;              /* stop delivering; teardown runs below */
        }
    }

dropped:
    for (j = 0; j < n_old; j++)
        free(old[j].path);
    free(old);
    /* A close() from inside one of the callbacks above deferred its teardown
       onto this sweep (dispose saw in_emit and only marked closed). The hook
       stack has unwound by now, so the watcher can be torn down here. The
 stop twin deferred the same way. */
    if (w->closed)
        dyn_watch_dispose(w);
    else if (w->stop_req) {
        w->stop_req = 0;
        dyn_watch_halt(w);
    }
    dyn_watch_leave(w);
}

/* Runs on the shared reactor's drain hook, so a tree that changes while nothing
 * else touches the loop is still swept. */
static void dyn_watch_tick(void *udata)
{
    dyn_watch_t *w = (dyn_watch_t *)udata;
    if (w->closed || w->stop_req || !w->dirty)
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
#if defined(CONFIG_IO_URING)
        /* detach the poll before close(2): an armed multishot poll holds a
           kernel file reference, and the fd would stay alive (still firing
           into a watcher whose native is being freed) until ring teardown */
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

static void dyn_watch_dispose(void *native)
{
    dyn_watch_t *w = (dyn_watch_t *)native;
    size_t i;
    JSRuntime *rt = w->ctx ? JS_GetRuntime(w->ctx) : NULL;

    if (w->in_emit || w->busy) {
        /* dispose is running while a frame still walks `w` -- a close()
           inside the change callback (or the last JS reference dropped
           there), or a value-free cascade inside the sweep/pump that dropped
           the watcher's last reference: freeing the snapshot and `w` now
           would leave that frame walking freed memory. Mark closed -- the
           outermost walker runs this teardown as it unwinds. */
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
    /* End the iteration. The explicit-close path (JS context) settles the
       parked pulls done now -- an explicit close is an explicit end, use
       stop() to drain first. The finalizer path must NOT settle: running a
       resolver there would call user JS during GC. It need not, either --
       a pull's promise pins the watcher (see dyn_watch_next), so waiters
       visible here belong to unreachable promises and are freed silently. */
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

static JSValue dyn_watch_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    dyn_watch_t *w;
    const char *root;
    JSValue res;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Watcher(path[, options])");
    /* Coerce every argument to a C local BEFORE any resource exists: coercion
       runs arbitrary user code. */
    root = dyn_path_borrow(ctx, argv[0], "Watcher(path)", NULL);
    if (!root)
        return JS_EXCEPTION;
    /* A bag argument is an object or absent (null/undefined): anything else
       is a type error, not a silently defaulted bag. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
        !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Watcher(path, options): options must be an object");

    w = (dyn_watch_t *)calloc(1, sizeof(*w));
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
        static const char *const keys[] = { "recursive", "debounceMs", "ignore" };
        JSValue v;
        int b;
        if (dyn_opts_strict(ctx, argv[1], keys, 3)) {
            dyn_watch_dispose(w); /* safe here: nothing user-visible exists yet */
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[1], "recursive");
        if (!JS_IsUndefined(v)) {
            b = JS_ToBool(ctx, v); /* -1 = a throwing valueOf: refuse */
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
#if defined(__linux__) && defined(CONFIG_IO_URING)
    /* io_uring: Linux watches through inotify, whose single fd is plain
       READABILITY -- dyn_aio_watch_fd -- so no dyn_evloop is needed. lp stays
       NULL and nothing on this platform touches it. */
#else
    /* NULL on the io_uring backend (unreachable here, but guard the other
       non-evloop futures the same way): DYN_EV_VNODE has nowhere to register.
       Refuse rather than store the NULL and fault on the first start(). */
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
    /* Both branches open the same brace so brace-counting tools (codegraph's
       regex parser sees every #if arm at once) stay balanced. */
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
    /* A (re)start reopens the event stream: an iteration a stop() ended
       begins delivering again. */
    w->iter_done = 0;
    return JS_UNDEFINED;
}

/* stop: halt the watch -- disarm, end the event stream, keep the
 * object alive and re-startable. Idempotent; on a closed watcher it is a
 * no-op, so the stop-then-close and close-then-stop orders both settle. */
static JSValue dyn_watch_stop(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DynResource *r;
    dyn_watch_t *w;
    (void)argc; (void)argv;

    r = (DynResource *)JS_GetOpaque(this_val, dyn_watch_class_id);
    w = (r && !r->closed) ? (dyn_watch_t *)r->native : NULL;
    if (!w)
        return JS_UNDEFINED;
    if (w->in_emit) {
        /* called from inside a change callback: the sweep halts the watcher
           once the callback stack unwinds (same deferral as close()) */
        w->stop_req = 1;
        return JS_UNDEFINED;
    }
    dyn_watch_halt(w);
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
    JS_CFUNC_DEF("stop", 0, dyn_watch_stop),
    JS_CFUNC_DEF("stats", 0, dyn_watch_stats),
    /*the async iteration (for await (const ev of w)); the manual
       iterator shape dyna-stream/pg established -- a promise of
       { value, done } per pull, and return() runs the cleanup. */
    JS_CFUNC_DEF("next", 0, dyn_watch_next),
    JS_CFUNC_DEF("return", 0, dyn_watch_return),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, dyn_watch_async_iterator),
};
