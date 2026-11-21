#include "libc-internal.h"
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#define JS_LOOP_BATCH 64
#define JS_WATCH_R 1
#define JS_WATCH_W 2
#define JS_WATCH_DIRECT 4
#define JS_WATCH_STALE 8
#define JS_WATCH_BADF 16
#define JS_WATCH_ENTER 16
#define JS_WATCH_LEAVE 8
#define JS_WATCH_SWEEP_MS 1000
#define JS_LOOP_JOB_ERROR (-2)
#define WORKER_EXIT_POLL_MS 20
#define JS_SIGNAL_POLL_MS 50

static int js_std_deadline_expired(void);

static void js_release_worker_refs(JSRuntime* rt, JSThreadState* ts);

static BOOL js_ports_have_pending(JSThreadState* ts);

#if !defined(_WIN32)
void js_watch_apply(JSThreadState* ts, JSOSRWHandler* rh, int want);
#endif

#if defined(__APPLE__)
static uint64_t js_tb_numer = 1, js_tb_denom = 1, js_tb_lim = UINT64_MAX;

__attribute__((constructor)) static void js_tb_init(void)
{
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.numer && tb.denom && tb.numer != tb.denom) {
        js_tb_numer = tb.numer;
        js_tb_denom = tb.denom;
        js_tb_lim = UINT64_MAX / tb.numer;
    }
}

int64_t get_time_ns(void)
{
    uint64_t t = mach_continuous_time();
    if (likely(t <= js_tb_lim))
        return (int64_t)(t * js_tb_numer / js_tb_denom);
    return (int64_t)(t / js_tb_denom * js_tb_numer + t % js_tb_denom * js_tb_numer / js_tb_denom);
}

int64_t get_time_ms(void)
{
    return get_time_ns() / 1000000;
}
#elif defined(__linux__)
int64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

int64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#else
int64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

int64_t get_time_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000000 + (tv.tv_usec * 1000);
}
#endif

void free_timer_body(JSRuntime* rt, JSOSTimer* th)
{
    int i;
    JS_FreeValueRT(rt, th->func);
    if (th->args) {
        for (i = 0; i < th->argc; i++)
            JS_FreeValueRT(rt, th->args[i]);
        js_free_rt(rt, th->args);
    }
    js_free_rt(rt, th);
}

enum { JS_TIMER_IDLE_LISTS = 32 };

static int tl_before(const JSOSTimerList* a, const JSOSTimerList* b)
{
    return a->key_timeout < b->key_timeout || (a->key_timeout == b->key_timeout && a->key_seq < b->key_seq);
}

static void tl_place(JSThreadState* ts, uint32_t i, JSOSTimerList* l)
{
    ts->timer_heap[i] = l;
    l->heap_idx = i;
}

static void tl_up(JSThreadState* ts, uint32_t i)
{
    JSOSTimerList* l = ts->timer_heap[i];
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (!tl_before(l, ts->timer_heap[parent]))
            break;
        tl_place(ts, i, ts->timer_heap[parent]);
        i = parent;
    }
    tl_place(ts, i, l);
}

static void tl_down(JSThreadState* ts, uint32_t i)
{
    JSOSTimerList* l = ts->timer_heap[i];
    uint32_t n = ts->n_timer_lists;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n && tl_before(ts->timer_heap[c + 1], ts->timer_heap[c]))
            c++;
        if (!tl_before(ts->timer_heap[c], l))
            break;
        tl_place(ts, i, ts->timer_heap[c]);
        i = c;
    }
    tl_place(ts, i, l);
}

static void tl_key(JSOSTimerList* l)
{
    l->key_timeout = l->head->timeout;
    l->key_seq = l->head->seq;
}

static uint32_t tl_hash(int64_t off, uint32_t cap)
{
    return (uint32_t)(((uint64_t)off * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
}

static JSOSTimerList* tl_find(JSThreadState* ts, int64_t off)
{
    JSOSTimerList* l = ts->timer_list_last;
    if (l && l->off == off)
        return l;
    if (!ts->timer_lists_cap)
        return NULL;
    for (l = ts->timer_lists[tl_hash(off, ts->timer_lists_cap)]; l; l = l->map_next)
        if (l->off == off)
            return ts->timer_list_last = l;
    return NULL;
}

static int tl_map_grow(JSRuntime* rt, JSThreadState* ts)
{
    uint32_t cap = ts->timer_lists_cap ? ts->timer_lists_cap * 2 : 16, i;
    JSOSTimerList** tab = (JSOSTimerList**)js_mallocz_rt(rt, (size_t)cap * sizeof(*tab));
    if (!tab)
        return -1;
    for (i = 0; i < ts->timer_lists_cap; i++) {
        JSOSTimerList* l = ts->timer_lists[i];
        while (l) {
            JSOSTimerList* next = l->map_next;
            uint32_t b = tl_hash(l->off, cap);
            l->map_next = tab[b];
            tab[b] = l;
            l = next;
        }
    }
    js_free_rt(rt, ts->timer_lists);
    ts->timer_lists = tab;
    ts->timer_lists_cap = cap;
    return 0;
}

static void tl_drop(JSRuntime* rt, JSThreadState* ts, JSOSTimerList* l)
{
    uint32_t i = l->heap_idx;
    ts->n_timer_lists--;
    if (i != ts->n_timer_lists) {
        JSOSTimerList* moved = ts->timer_heap[ts->n_timer_lists];
        tl_place(ts, i, moved);
        tl_down(ts, i);
        tl_up(ts, moved->heap_idx);
    }
    if (ts->n_idle_timer_lists < JS_TIMER_IDLE_LISTS) {
        l->heap_idx = UINT32_MAX;
        ts->n_idle_timer_lists++;
    } else {
        JSOSTimerList** pp = &ts->timer_lists[tl_hash(l->off, ts->timer_lists_cap)];
        while (*pp != l)
            pp = &(*pp)->map_next;
        *pp = l->map_next;
        ts->n_mapped_timer_lists--;
        if (ts->timer_list_last == l)
            ts->timer_list_last = NULL;
        js_free_rt(rt, l);
    }
}

static void tl_append(JSThreadState* ts, JSOSTimerList* l, JSOSTimer* th)
{
    JSOSTimer* at = l->tail;
    th->list = l;
    th->lprev = at;
    th->lnext = NULL;
    l->tail = th;
    if (at) {
        at->lnext = th;
    } else {
        l->head = th;
        tl_key(l);
        tl_up(ts, l->heap_idx);
    }
}

static void tl_unlink(JSThreadState* ts, JSOSTimer* th)
{
    JSOSTimerList* l = th->list;
    int was_head = !th->lprev;
    if (th->lprev)
        th->lprev->lnext = th->lnext;
    else
        l->head = th->lnext;
    if (th->lnext)
        th->lnext->lprev = th->lprev;
    else
        l->tail = th->lprev;
    th->lnext = th->lprev = NULL;
    if (was_head && l->head) {
        tl_key(l);
        tl_down(ts, l->heap_idx);
    }
}

int timer_insert(JSRuntime* rt, JSThreadState* ts, JSOSTimer* th, int64_t off)
{
    JSOSTimerList* l = tl_find(ts, off);
    if (!l || l->heap_idx == UINT32_MAX) {
        if (ts->n_timer_lists == ts->timer_heap_cap) {
            uint32_t cap = ts->timer_heap_cap ? ts->timer_heap_cap * 2 : 16;
            JSOSTimerList** h = (JSOSTimerList**)js_realloc_rt(rt, ts->timer_heap, (size_t)cap * sizeof(*h));
            if (!h)
                return -1;
            ts->timer_heap = h;
            ts->timer_heap_cap = cap;
        }
    }
    if (!l) {
        if (ts->n_mapped_timer_lists >= ts->timer_lists_cap && tl_map_grow(rt, ts) < 0)
            return -1;
        l = (JSOSTimerList*)js_mallocz_rt(rt, sizeof(*l));
        if (!l)
            return -1;
        l->off = off;
        {
            uint32_t b = tl_hash(off, ts->timer_lists_cap);
            l->map_next = ts->timer_lists[b];
            ts->timer_lists[b] = l;
        }
        ts->n_mapped_timer_lists++;
        ts->timer_list_last = l;
        l->heap_idx = ts->n_timer_lists;
        ts->timer_heap[ts->n_timer_lists++] = l;
    } else if (l->heap_idx == UINT32_MAX) {
        ts->n_idle_timer_lists--;
        l->heap_idx = ts->n_timer_lists;
        ts->timer_heap[ts->n_timer_lists++] = l;
    }
    if (th->timer_id > 0) {
        uint32_t b;
        if ((uint32_t)ts->n_timers >= ts->timer_ids_cap)
            timer_ids_grow(rt, ts);
        if (!ts->timer_ids_cap) {
            if (!l->head)
                tl_drop(rt, ts, l);
            return -1;
        }
        b = (uint32_t)th->timer_id & (ts->timer_ids_cap - 1);
        th->id_next = ts->timer_ids[b];
        ts->timer_ids[b] = th;
    }
    th->seq = ++ts->timer_seq;
    tl_append(ts, l, th);
    ts->n_timers++;
    return 0;
}

static void timer_ids_unlink(JSThreadState* ts, JSOSTimer* th)
{
    JSOSTimer** pp;
    if (th->timer_id <= 0 || !ts->timer_ids_cap)
        return;
    pp = &ts->timer_ids[(uint32_t)th->timer_id & (ts->timer_ids_cap - 1)];
    while (*pp && *pp != th)
        pp = &(*pp)->id_next;
    if (*pp)
        *pp = th->id_next;
    th->id_next = NULL;
}

void timer_ids_grow(JSRuntime* rt, JSThreadState* ts)
{
    uint32_t cap = ts->timer_ids_cap ? ts->timer_ids_cap * 2 : 64, i;
    JSOSTimer** tab = (JSOSTimer**)js_mallocz_rt(rt, (size_t)cap * sizeof(*tab));
    if (!tab)
        return;
    for (i = 0; i < ts->timer_ids_cap; i++) {
        JSOSTimer* th = ts->timer_ids[i];
        while (th) {
            JSOSTimer* next = th->id_next;
            uint32_t b = (uint32_t)th->timer_id & (cap - 1);
            th->id_next = tab[b];
            tab[b] = th;
            th = next;
        }
    }
    js_free_rt(rt, ts->timer_ids);
    ts->timer_ids = tab;
    ts->timer_ids_cap = cap;
}

void free_timer(JSRuntime* rt, JSOSTimer* th)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (ts && th->list) {
        JSOSTimerList* l = th->list;
        timer_ids_unlink(ts, th);
        ts->n_timers--;
        tl_unlink(ts, th);
        if (!l->head)
            tl_drop(rt, ts, l);
    }
    free_timer_body(rt, th);
}

static void timer_requeue(JSThreadState* ts, JSOSTimer* th)
{
    JSOSTimerList* l = th->list;
    tl_unlink(ts, th);
    th->seq = ++ts->timer_seq;
    if (!l->head) {
        l->head = l->tail = th;
        th->lprev = th->lnext = NULL;
        tl_key(l);
        tl_down(ts, l->heap_idx);
    } else {
        tl_append(ts, l, th);
    }
}

#ifdef USE_WORKER

#ifdef _WIN32

int js_waker_init(JSWaker* w)
{
    w->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    return w->handle ? 0 : -1;
}

void js_waker_signal(JSWaker* w)
{
    SetEvent(w->handle);
}

static void js_waker_clear(JSWaker* w)
{
    ResetEvent(w->handle);
}

void js_waker_close(JSWaker* w)
{
    CloseHandle(w->handle);
    w->handle = INVALID_HANDLE_VALUE;
}

#else

int js_waker_init(JSWaker* w)
{
    int fds[2];

    if (dyn_pipe_cloexec(fds) < 0)
        return -1;
    w->read_fd = fds[0];
    w->write_fd = fds[1];
    return 0;
}

void js_waker_signal(JSWaker* w)
{
    int ret;

    for (;;) {
        ret = write(w->write_fd, "", 1);
        if (ret == 1)
            break;
        if (ret < 0 && errno != EAGAIN && errno != EINTR)
            break;
    }
}

static void js_waker_clear(JSWaker* w)
{
    uint8_t buf[16];
    int ret;

    for (;;) {
        ret = read(w->read_fd, buf, sizeof(buf));
        if (ret >= 0)
            break;
        if (errno != EAGAIN && errno != EINTR)
            break;
    }
}

void js_waker_close(JSWaker* w)
{
    close(w->read_fd);
    close(w->write_fd);
    w->read_fd = -1;
    w->write_fd = -1;
}

#endif

void js_free_message(JSWorkerMessage* msg);

static int handle_posted_message(JSRuntime* rt, JSContext* ctx,
    JSWorkerMessageHandler* port)
{
    JSWorkerMessagePipe* ps = port->recv_pipe;
    int ret;
    struct list_head* el;
    JSWorkerMessage* msg;
    JSValue obj, data_obj, func, retval;

    pthread_mutex_lock(&ps->mutex);
    if (!list_empty(&ps->msg_queue)) {
        el = ps->msg_queue.next;
        msg = list_entry(el, JSWorkerMessage, link);

        list_del(&msg->link);
        ps->queued_bytes = ps->queued_bytes > msg->data_len
            ? ps->queued_bytes - msg->data_len
            : 0;

        if (list_empty(&ps->msg_queue))
            js_waker_clear(&ps->waker);

        pthread_mutex_unlock(&ps->mutex);

        data_obj = JS_ReadObject(ctx, msg->data, msg->data_len,
            JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);

        js_free_message(msg);

        if (!JS_IsException(data_obj))
            data_obj = js_streams_transfer_revive(ctx, data_obj);
        if (JS_IsException(data_obj))
            goto fail;
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, data_obj);
            goto fail;
        }
        JS_DefinePropertyValueStr(ctx, obj, "data", data_obj, JS_PROP_C_W_E);

        func = JS_DupValue(ctx, port->on_message_func);
        retval = JS_Call(ctx, func, JS_UNDEFINED, 1, (JSValueConst*)&obj);
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, func);
        if (JS_IsException(retval)) {
        fail:
            js_std_dump_error(ctx);
        } else {
            JS_FreeValue(ctx, retval);
        }
        ret = 1;
    } else {
        pthread_mutex_unlock(&ps->mutex);
        ret = 0;
    }
    return ret;
}
#else
static int handle_posted_message(JSRuntime* rt, JSContext* ctx,
    JSWorkerMessageHandler* port)
{
    return 0;
}
#endif

#if defined(_WIN32)

int js_os_poll(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int min_delay, count;
    int64_t cur_time, delay;
    JSOSRWHandler* rh;
    struct list_head* el;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];

    if (list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0) && list_empty(&ts->port_list)) {
        return -1;
    }

    if ((ts->n_timers > 0)) {
        JSOSTimer* th = timer_first(ts);
        cur_time = js_time_us();
        delay = th->timeout - cur_time;
        if (delay <= 0) {
            JSValue func;
            if (th->repeat) {
                th->timeout = cur_time + th->delay * 1000;
                timer_requeue(ts, th);
                call_timer(ctx, th->func, th->argc, (JSValueConst*)th->args);
                return 0;
            }
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue* args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                call_timer(ctx, func, nargs, (JSValueConst*)args);
                if (args) {
                    for (int i = 0; i < nargs; i++)
                        JS_FreeValueRT(rt, args[i]);
                    js_free_rt(rt, args);
                }
            }
            JS_FreeValue(ctx, func);
            return 0;
        }
        delay = (delay + 999) / 1000;
        min_delay = delay < 10000 ? delay : 10000;
    } else {
        min_delay = -1;
    }

    count = 0;
    list_for_each(el, &ts->os_rw_handlers)
    {
        rh = list_entry(el, JSOSRWHandler, link);
        if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
            handles[count++] = (HANDLE)_get_osfhandle(rh->fd);
            if (count == (int)countof(handles))
                break;
        }
    }

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (JS_IsNull(port->on_message_func))
            continue;
        handles[count++] = port->recv_pipe->waker.handle;
        if (count == (int)countof(handles))
            break;
    }

    if (count > 0) {
        DWORD ret, timeout = INFINITE;
        if (min_delay != -1)
            timeout = min_delay;
        ret = WaitForMultipleObjects(count, handles, FALSE, timeout);

        if (ret < count) {
            list_for_each(el, &ts->os_rw_handlers)
            {
                rh = list_entry(el, JSOSRWHandler, link);
                if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
                    call_handler(ctx, rh->rw_func[0]);
                    goto done;
                }
            }

            list_for_each(el, &ts->port_list)
            {
                JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
                if (!JS_IsNull(port->on_message_func)) {
                    JSWorkerMessagePipe* ps = port->recv_pipe;
                    if (ps->waker.handle == handles[ret]) {
                        if (handle_posted_message(rt, ctx, port))
                            goto done;
                    }
                }
            }
        }
    } else {
        Sleep(min_delay);
    }
done:
    return 0;
}

#else

static no_inline int js_poll_expand(JSThreadState* ts)
{
    struct pollfd* new_fds;
    int new_size = max_int(ts->poll_fds_size + ts->poll_fds_size / 2, 16);
    new_fds = realloc(ts->poll_fds, new_size * sizeof(struct pollfd));
    if (!new_fds)
        return -1;
    ts->poll_fds = new_fds;
    ts->poll_fds_size = new_size;
    return 0;
}

static int js_poll_add_poll_fd(JSThreadState* ts, int* pnfds, int fd, int events)
{
    struct pollfd* fds;
    int nfds;
    nfds = *pnfds;
    if (unlikely(nfds >= ts->poll_fds_size)) {
        if (js_poll_expand(ts))
            return -1;
    }
    fds = &ts->poll_fds[nfds++];
    fds->fd = fd;
    fds->events = events;
    fds->revents = 0;
    *pnfds = nfds;
    return 0;
}

static int js_poll_wait_us(JSThreadState* ts, int nfds, int min_delay, int64_t us)
{
#if defined(JS_WATCH_KQUEUE)
    if (us > 0 && us < 1000000) {
        struct kevent ch[2], out[2];
        struct timespec zero = { 0, 0 };
        if (ts->deadline_kq < 0) {
            ts->deadline_kq = kqueue();
            if (ts->deadline_kq >= 0)
                dyn_fd_set_cloexec(ts->deadline_kq);
        }
        EV_SET(&ch[0], 1, EVFILT_TIMER, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
        EV_SET(&ch[1], 1, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT | EV_RECEIPT, NOTE_USECONDS | NOTE_CRITICAL, us, NULL);
        if (ts->deadline_kq >= 0 && kevent(ts->deadline_kq, ch, 2, out, 2, &zero) == 2 && !(out[1].data)
            && js_poll_add_poll_fd(ts, &nfds, ts->deadline_kq, POLLIN))
            return -1;
    }
    return poll(ts->poll_fds, (nfds_t)nfds, min_delay);
#elif defined(__linux__) && defined(_GNU_SOURCE)
    struct timespec t;
    (void)min_delay;
    if (us < 0)
        return poll(ts->poll_fds, (nfds_t)nfds, -1);
    t.tv_sec = (time_t)(us / 1000000);
    t.tv_nsec = (long)(us % 1000000) * 1000L;
    return ppoll(ts->poll_fds, (nfds_t)nfds, &t, NULL);
#else
    (void)us;
    return poll(ts->poll_fds, (nfds_t)nfds, min_delay);
#endif
}

static int js_watch_want(const JSOSRWHandler* rh)
{
    return (JS_IsNull(rh->rw_func[0]) ? 0 : JS_WATCH_R)
        | (JS_IsNull(rh->rw_func[1]) ? 0 : JS_WATCH_W);
}

static void js_watch_reset(JSThreadState* ts)
{
    struct list_head* el;
    list_for_each(el, &ts->os_rw_handlers)
        list_entry(el, JSOSRWHandler, link)
            ->watch_mask = 0;
}

static void js_watch_open(JSThreadState* ts)
{
    int fd = -1;
#if defined(JS_WATCH_KQUEUE)
    fd = kqueue();
    if (fd >= 0)
        dyn_fd_set_cloexec(fd);
#elif defined(JS_WATCH_EPOLL)
    fd = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (fd < 0) {
        ts->watch_failed = 1;
        return;
    }
    ts->watch_fd = fd;
    ts->watch_swept_ms = get_time_ms();
    js_watch_reset(ts);
}

static void js_watch_close(JSThreadState* ts)
{
    close(ts->watch_fd);
    ts->watch_fd = -1;
    js_watch_reset(ts);
}

void js_watch_apply(JSThreadState* ts, JSOSRWHandler* rh, int want)
{
    int have = rh->watch_mask & JS_WATCH_RW, err = 0;

    if (rh->watch_mask & JS_WATCH_DIRECT) {
        rh->watch_mask = want ? JS_WATCH_DIRECT : 0;
        return;
    }
    if (!rh->watch_id) {
        if (++ts->watch_next_id == 0)
            ts->watch_next_id = 1;
        rh->watch_id = ts->watch_next_id;
    }
#if defined(JS_WATCH_KQUEUE)
    {
        struct kevent ch[2], out[2];
        struct timespec zero = { 0, 0 };
        int n = 0, i, r, force = rh->watch_mask & JS_WATCH_STALE;
        int add_r = 0, add_w = 0;
        if ((want & JS_WATCH_R) && (force || !(have & JS_WATCH_R))) {
            EV_SET(&ch[n++], rh->fd, EVFILT_READ, EV_ADD | EV_RECEIPT, 0, 0, rh);
            add_r = 1;
        } else if (!(want & JS_WATCH_R) && (have & JS_WATCH_R)) {
            EV_SET(&ch[n++], rh->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, rh);
        }
        if ((want & JS_WATCH_W) && (force || !(have & JS_WATCH_W))) {
            EV_SET(&ch[n++], rh->fd, EVFILT_WRITE, EV_ADD | EV_RECEIPT, 0, 0, rh);
            add_w = 1;
        } else if (!(want & JS_WATCH_W) && (have & JS_WATCH_W)) {
            EV_SET(&ch[n++], rh->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, rh);
        }
        if (n) {
            r = kevent(ts->watch_fd, ch, n, out, n, &zero);
            if (r < 0)
                err = errno;
            for (i = 0; i < r; i++) {
                int added = out[i].filter == EVFILT_READ ? add_r : add_w;
                if ((out[i].flags & EV_ERROR) && out[i].data && added && err != EBADF)
                    err = (int)out[i].data;
            }
        }
        if (err && err != EBADF) {
            EV_SET(&ch[0], rh->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, rh);
            EV_SET(&ch[1], rh->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, rh);
            kevent(ts->watch_fd, ch, 2, out, 2, &zero);
        }
    }
#elif defined(JS_WATCH_EPOLL)
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = ((want & JS_WATCH_R) ? EPOLLIN : 0u)
            | ((want & JS_WATCH_W) ? EPOLLOUT : 0u) | EPOLLONESHOT;
        ev.data.u64 = ((uint64_t)rh->watch_id << 32) | (uint32_t)rh->fd;
        if (!want) {
            if (have)
                epoll_ctl(ts->watch_fd, EPOLL_CTL_DEL, rh->fd, &ev);
        } else if (epoll_ctl(ts->watch_fd, have ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, rh->fd, &ev) < 0) {
            err = errno;
            if (err == ENOENT || err == EEXIST)
                err = epoll_ctl(ts->watch_fd, err == ENOENT ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                          rh->fd, &ev)
                        < 0
                    ? errno
                    : 0;
        }
    }
#else
    err = ENOSYS;
#endif
    if (err == EBADF)
        rh->watch_mask = JS_WATCH_BADF;
    else if (err)
        rh->watch_mask = want ? JS_WATCH_DIRECT : 0;
    else
        rh->watch_mask = want;
}

static int js_watch_fetch(JSThreadState* ts, JSOSRWHandler** ready)
{
    int ne, i, nr = 0;
    uint32_t serial = ++ts->watch_serial;
#if defined(JS_WATCH_KQUEUE)
    struct kevent evs[JS_LOOP_BATCH];
    struct timespec zero = { 0, 0 };
    ne = kevent(ts->watch_fd, NULL, 0, evs, JS_LOOP_BATCH, &zero);
    for (i = 0; i < ne; i++) {
        JSOSRWHandler* rh = (JSOSRWHandler*)evs[i].udata;
        int re = evs[i].filter == EVFILT_READ ? POLLIN : POLLOUT;
        if (evs[i].flags & EV_ERROR)
            continue;
        if (evs[i].flags & EV_EOF)
            re |= POLLHUP;
#elif defined(JS_WATCH_EPOLL)
    struct epoll_event evs[JS_LOOP_BATCH];
    ne = epoll_wait(ts->watch_fd, evs, JS_LOOP_BATCH, 0);
    for (i = 0; i < ne; i++) {
        int fd = (int)(uint32_t)evs[i].data.u64, re = 0;
        uint32_t id = (uint32_t)(evs[i].data.u64 >> 32);
        JSOSRWHandler* rh = find_rh(ts, fd);
        if (!rh || rh->watch_id != id)
            continue;
        rh->watch_mask |= JS_WATCH_STALE;
        if (evs[i].events & EPOLLIN)
            re |= POLLIN;
        if (evs[i].events & EPOLLOUT)
            re |= POLLOUT;
        if (evs[i].events & EPOLLHUP)
            re |= POLLHUP;
        if (evs[i].events & EPOLLERR)
            re |= POLLERR;
#else
    ne = 0;
    for (i = 0; i < ne; i++) {
        JSOSRWHandler* rh = NULL;
        int re = 0;
#endif
        if (rh->watch_seen != serial) {
            rh->watch_seen = serial;
            rh->watch_re = 0;
            ready[nr++] = rh;
        }
        rh->watch_re |= re;
    }
    return nr;
}

static int js_watch_sync(JSRuntime* rt, JSThreadState* ts, int* pnfds)
{
    struct list_head *el, *el1;
    int reaped = 0;

    if (ts->watch_swept_ms + JS_WATCH_SWEEP_MS <= get_time_ms()) {
        ts->watch_swept_ms = get_time_ms();
        list_for_each(el, &ts->os_rw_handlers)
        {
            JSOSRWHandler* rh = list_entry(el, JSOSRWHandler, link);
            if (rh->watch_mask & JS_WATCH_RW)
                rh->watch_mask |= JS_WATCH_STALE;
        }
    }
    list_for_each_safe(el, el1, &ts->os_rw_handlers)
    {
        JSOSRWHandler* rh = list_entry(el, JSOSRWHandler, link);
        int want = js_watch_want(rh);
        rh->poll_fd_index = -1;
        if (rh->watch_mask & JS_WATCH_DIRECT) {
            if (!want)
                rh->watch_mask = 0;
        } else if (want != (rh->watch_mask & JS_WATCH_RW) || (rh->watch_mask & JS_WATCH_STALE)) {
            js_watch_apply(ts, rh, want);
        }
        if (rh->watch_mask == JS_WATCH_BADF) {
            free_rw_handler(rt, rh);
            reaped = 1;
            continue;
        }
        if ((rh->watch_mask & JS_WATCH_DIRECT) && want) {
            rh->poll_fd_index = *pnfds;
            if (js_poll_add_poll_fd(ts, pnfds, rh->fd,
                    ((want & JS_WATCH_R) ? POLLIN : 0) | ((want & JS_WATCH_W) ? POLLOUT : 0)))
                return -1;
        }
    }
    return reaped;
}

typedef struct JSLoopPoster {
    pthread_mutex_t mu;
    JSLoopPost *head, *tail;
    JSWaker waker;
    _Atomic int pending;
} JSLoopPoster;

static pthread_mutex_t js_main_poster_mu = PTHREAD_MUTEX_INITIALIZER;
static JSLoopPoster* js_main_poster;

static void js_loop_poster_kick(JSLoopPoster* po)
{
    if (atomic_exchange_explicit(&po->pending, 1, memory_order_acq_rel) == 0) {
        (void)dyn_write_eintr(po->waker.write_fd, "", 1);
    }
}

int js_loop_poster_ensure(JSThreadState* ts)
{
    JSLoopPoster* po;
    int i;
    if (ts->poster)
        return 0;
    po = calloc(1, sizeof(*po));
    if (!po)
        return -1;
    if (js_waker_init(&po->waker)) {
        free(po);
        return -1;
    }
    for (i = 0; i < 2; i++) {
        int fd = i ? po->waker.write_fd : po->waker.read_fd;
        if (dyn_fd_set_nonblock(fd) < 0) {
            js_waker_close(&po->waker);
            free(po);
            return -1;
        }
    }
    pthread_mutex_init(&po->mu, NULL);
    ts->poster = po;
    if (!ts->recv_pipe) {
        pthread_mutex_lock(&js_main_poster_mu);
        js_main_poster = po;
        pthread_mutex_unlock(&js_main_poster_mu);
        atomic_store_explicit(&js_main_wake_wfd, po->waker.write_fd, memory_order_release);
    }
    return 0;
}

void js_main_loop_wake(void)
{
    pthread_mutex_lock(&js_main_poster_mu);
    if (js_main_poster)
        js_loop_poster_kick(js_main_poster);
    pthread_mutex_unlock(&js_main_poster_mu);
}

static void js_loop_poster_free(JSThreadState* ts)
{
    JSLoopPoster* po = ts->poster;
    if (!po)
        return;
    if (ts->io_reactor_watched_fd == po->waker.read_fd && ts->io_reactor_unwatch)
        ts->io_reactor_unwatch(ts->io_reactor_udata, po->waker.read_fd);
    ts->io_reactor_watched_fd = -1;
    pthread_mutex_lock(&js_main_poster_mu);
    if (js_main_poster == po) {
        atomic_store_explicit(&js_main_wake_wfd, -1, memory_order_release);
        js_main_poster = NULL;
    }
    pthread_mutex_unlock(&js_main_poster_mu);
    js_waker_close(&po->waker);
    pthread_mutex_destroy(&po->mu);
    free(po);
    ts->poster = NULL;
}

void* js_std_loop_hold(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!ts || js_loop_poster_ensure(ts))
        return NULL;
    ts->post_holds++;
    return ts->poster;
}

void js_std_loop_release(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (ts && ts->post_holds > 0)
        ts->post_holds--;
}

void js_std_loop_post(void* handle, JSLoopPost* node)
{
    JSLoopPoster* po = handle;
    node->next = NULL;
    pthread_mutex_lock(&po->mu);
    if (po->tail)
        po->tail->next = node;
    else
        po->head = node;
    po->tail = node;
    pthread_mutex_unlock(&po->mu);
    js_loop_poster_kick(po);
}

static int js_loop_run_posts(JSContext* ctx, JSThreadState* ts, int readable)
{
    JSLoopPoster* po = ts->poster;
    JSLoopPost* n;
    int ran = 0;
    if (!po)
        return 0;
    if (atomic_exchange_explicit(&po->pending, 0, memory_order_acq_rel) == 0 && !readable)
        return 0;
    {
        uint8_t sink[64];
        while (read(po->waker.read_fd, sink, sizeof(sink)) > 0) {
        }
    }
    pthread_mutex_lock(&po->mu);
    n = po->head;
    po->head = po->tail = NULL;
    pthread_mutex_unlock(&po->mu);
    while (n) {
        JSLoopPost* next = n->next;
        n->fn(ctx, n->udata);
        n = next;
        ran++;
    }
    ts->stat_posts += (uint64_t)ran;
    return ran;
}

void js_std_set_shutdown_hook(JSContext* ctx, void (*hook)(JSRuntime* rt))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (ts && (!ts->shutdown_hook || ts->shutdown_hook == hook))
        ts->shutdown_hook = hook;
}

int js_std_loop_wait_posts(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    struct pollfd pfd;
    if (!ts || !ts->poster)
        return 0;
    if (js_loop_run_posts(ctx, ts, 0))
        return 1;
    pfd.fd = ts->poster->waker.read_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) <= 0)
        return 0;
    return js_loop_run_posts(ctx, ts, 1);
}

void js_std_set_io_reactor_rt(JSRuntime* rt, int fd,
    void (*drain)(void* udata), void* udata)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (!ts)
        return;
    ts->io_reactor_fd = fd;
    ts->io_reactor_drain = drain;
    ts->io_reactor_udata = udata;
    ts->io_reactor_watched_fd = -1;
    if (fd < 0) {
        ts->io_reactor_wait = NULL;
        ts->io_reactor_wait_us = NULL;
        ts->io_reactor_watch = NULL;
        ts->io_reactor_unwatch = NULL;
        ts->io_reactor_weak = 0;
        ts->io_reactor_detach = NULL;
    }
}

void js_std_set_io_reactor_detach(JSContext* ctx, void (*detach)(JSRuntime* rt))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (ts)
        ts->io_reactor_detach = detach;
}

void js_std_set_io_reactor_watch(JSContext* ctx,
    int (*watch)(void* udata, int fd, void (*ready)(void* arg), void* arg),
    int (*unwatch)(void* udata, int fd))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!ts)
        return;
    ts->io_reactor_watch = watch;
    ts->io_reactor_unwatch = unwatch;
    ts->io_reactor_watched_fd = -1;
}

static void js_loop_poster_seen(void* arg)
{
    ((JSThreadState*)arg)->poster_ready = 1;
}

void js_std_set_io_reactor_weak_rt(JSRuntime* rt, int weak)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (ts)
        ts->io_reactor_weak = weak ? 1 : 0;
}

static int js_reactor_holds_loop(const JSThreadState* ts)
{
    return ts->io_reactor_fd >= 0 && !ts->io_reactor_weak;
}

void js_std_set_io_reactor_wait_rt(JSRuntime* rt,
    int (*wait)(void* udata, int timeout_ms))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (!ts)
        return;
    ts->io_reactor_wait = wait;
}

void js_std_set_io_reactor_wait(JSContext* ctx,
    int (*wait)(void* udata, int timeout_ms))
{
    js_std_set_io_reactor_wait_rt(JS_GetRuntime(ctx), wait);
}

void js_std_set_io_reactor_wait_us(JSContext* ctx,
    int (*wait)(void* udata, int64_t timeout_us))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!ts)
        return;
    ts->io_reactor_wait_us = wait;
}

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata)
{
    js_std_set_io_reactor_rt(JS_GetRuntime(ctx), fd, drain, udata);
}

static BOOL js_ports_have_pending(JSThreadState* ts)
{
    struct list_head* el;
    BOOL pending = FALSE;

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (!JS_IsNull(port->on_message_func)) {
            JSWorkerMessagePipe* ps = port->recv_pipe;
            BOOL empty;
            pthread_mutex_lock(&ps->mutex);
            empty = list_empty(&ps->msg_queue);
            pthread_mutex_unlock(&ps->mutex);
            if (!empty) {
                pending = TRUE;
                break;
            }
        }
    }
    return pending;
}

static void js_release_worker_refs(JSRuntime* rt, JSThreadState* ts)
{
    struct list_head *el, *el1;

    list_for_each_safe(el, el1, &ts->worker_obj_list)
    {
        JSWorkerObjRef* wref = list_entry(el, JSWorkerObjRef, link);
        list_del(&wref->link);
        JS_FreeValueRT(rt, wref->obj);
        js_free_rt(rt, wref);
    }
}

#define JS_TIMER_IO_SLICE_MS 2

static void js_std_promise_rejection_check(JSContext* ctx);

void js_std_set_job_error_rt(JSRuntime* rt, JSValue exc)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (!ts || ts->job_error_set) {
        JS_FreeValueRT(rt, exc);
        return;
    }
    ts->job_error = exc;
    ts->job_error_set = 1;
}

int js_std_job_error_pending_rt(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    return ts && ts->job_error_set;
}

static int js_take_job_error(JSContext* ctx)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue exc;
    if (!ts || !ts->job_error_set)
        return 0;
    exc = ts->job_error;
    ts->job_error = JS_UNDEFINED;
    ts->job_error_set = 0;
    if (!JS_IsUninitialized(exc))
        JS_Throw(ctx, exc);
    return 1;
}

int js_std_take_job_error_ctx(JSContext* ctx)
{
    return js_take_job_error(ctx);
}

static int js_run_jobs(JSContext* ctx, JSValueConst until)
{
    JSThreadState* ts;
    if (js_take_job_error(ctx))
        return -1;
    ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    return JS_ExecutePendingJobsUntil(JS_GetRuntime(ctx), until, ts ? &ts->job_error_set : NULL);
}

static int js_loop_after_callback(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    int err;

    if (js_take_job_error(ctx))
        return JS_LOOP_JOB_ERROR;
    while ((err = JS_ExecutePendingJob(rt, NULL)) > 0) {
    }
    if (err < 0)
        return JS_LOOP_JOB_ERROR;
    if (js_std_deadline_expired())
        return 1;
    js_std_promise_rejection_check(ctx);
    return 0;
}

static int js_rw_fire(JSContext* ctx, JSThreadState* ts, JSOSRWHandler* rh,
    short re, uint32_t* gen, int* budget, int* did)
{
    int k, r;
    if (re & POLLNVAL) {
        free_rw_handler(JS_GetRuntime(ctx), rh);
        *gen = ts->rw_gen;
        return 0;
    }
    for (k = 0; k < 2; k++) {
        if (JS_IsNull(rh->rw_func[k]) || !(re & (POLLERR | POLLHUP | (k ? POLLOUT : POLLIN))))
            continue;
        ts->stat_fd_events++;
        *did = 1;
        call_handler(ctx, rh->rw_func[k]);
        r = js_loop_after_callback(ctx);
        if (r == JS_LOOP_JOB_ERROR)
            return r;
        if (r || ts->rw_gen != *gen || --*budget == 0)
            return 1;
        break;
    }
    return 0;
}

int js_os_poll(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int min_delay, nfds;
    int64_t min_delay_us;
    int64_t cur_time, delay;
    JSOSRWHandler* rh;
    struct list_head *el, *el1;

    if (!ts->recv_pipe && unlikely(atomic_load_explicit(&os_pending_signals, memory_order_relaxed) != 0)) {
        JSOSSignalHandler* sh;
        uint64_t mask;

        list_for_each(el, &ts->os_signal_handlers)
        {
            sh = list_entry(el, JSOSSignalHandler, link);
            mask = (uint64_t)1 << sh->sig_num;
            if (atomic_load_explicit(&os_pending_signals, memory_order_relaxed) & mask) {
                atomic_fetch_and_explicit(&os_pending_signals, ~mask,
                    memory_order_relaxed);
                ts->stat_signals++;
                call_handler(ctx, sh->func);
                return 0;
            }
        }
    }

    ts->rw_gen++;
    if (js_loop_run_posts(ctx, ts, 0))
        return 0;

    if (list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0) && list_empty(&ts->port_list) && !js_reactor_holds_loop(ts) && ts->post_holds == 0)
        return -1;

    if (!ts->recv_pipe && atomic_load_explicit(&js_live_worker_threads, memory_order_acquire) == 0 && !js_ports_have_pending(ts)) {
        js_release_worker_refs(rt, ts);
        if (!js_reactor_holds_loop(ts) && list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0) && ts->post_holds == 0)
            return -1;
    }

    if ((ts->n_timers > 0)) {
        JSOSTimer* th = timer_first(ts);
        if (th->timeout <= ts->clock_us && !th->repeat && ts->io_reactor_fd < 0 && !ts->recv_pipe
            && list_empty(&ts->os_rw_handlers) && list_empty(&ts->port_list))
            cur_time = ts->clock_us;
        else
            cur_time = ts->clock_us = js_time_us();
        delay = th->timeout - cur_time;
        if (delay <= 0
            && (ts->last_io_poll_us == 0
                || cur_time - ts->last_io_poll_us < JS_TIMER_IO_SLICE_MS * 1000
                || (ts->io_reactor_fd < 0 && !ts->recv_pipe
                    && list_empty(&ts->os_rw_handlers)
                    && list_empty(&ts->port_list)))) {
            JSValue func;
            if (ts->last_io_poll_us == 0)
                ts->last_io_poll_us = cur_time;
            if (th->repeat) {
                th->timeout = cur_time + th->delay * 1000;
                timer_requeue(ts, th);
                ts->stat_timers++;
                call_timer(ctx, th->func, th->argc, (JSValueConst*)th->args);
                return 0;
            }
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue* args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                ts->stat_timers++;
                call_timer(ctx, func, nargs, (JSValueConst*)args);
                if (args) {
                    for (int i = 0; i < nargs; i++)
                        JS_FreeValueRT(rt, args[i]);
                    js_free_rt(rt, args);
                }
            }
            JS_FreeValue(ctx, func);
            return 0;
        }
        ts->last_io_poll_us = 0;
        min_delay_us = delay <= 0 ? 0 : delay < 10000000 ? delay : 10000000;
        min_delay = (int)((min_delay_us + 999) / 1000);
    } else {
        min_delay = -1;
        min_delay_us = -1;
    }

    nfds = 0;
    {
        int nactive = 0;
        list_for_each(el, &ts->os_rw_handlers)
        {
            if (js_watch_want(list_entry(el, JSOSRWHandler, link)))
                nactive++;
        }
        if (ts->watch_fd < 0 && nactive >= JS_WATCH_ENTER && !ts->watch_failed)
            js_watch_open(ts);
        else if (ts->watch_fd >= 0 && nactive < JS_WATCH_LEAVE)
            js_watch_close(ts);
    }
    int watch_pfd_index = -1;
    if (ts->watch_fd >= 0) {
        int reaped = js_watch_sync(rt, ts, &nfds);
        if (reaped < 0)
            return -1;
        if (reaped)
            return 0;
        watch_pfd_index = nfds;
        if (js_poll_add_poll_fd(ts, &nfds, ts->watch_fd, POLLIN))
            return -1;
    } else {
        list_for_each(el, &ts->os_rw_handlers)
        {
            int events;

            rh = list_entry(el, JSOSRWHandler, link);
            events = 0;
            if (!JS_IsNull(rh->rw_func[0]))
                events |= POLLIN;
            if (!JS_IsNull(rh->rw_func[1]))
                events |= POLLOUT;
            if (events) {
                rh->poll_fd_index = nfds;
                if (js_poll_add_poll_fd(ts, &nfds, rh->fd, events))
                    return -1;
            }
        }
    }

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (!JS_IsNull(port->on_message_func)) {
            JSWorkerMessagePipe* ps = port->recv_pipe;
            port->poll_fd_index = nfds;
            if (js_poll_add_poll_fd(ts, &nfds, ps->waker.read_fd, POLLIN))
                return -1;
        }
    }

    int reactor_pfd_index = -1;
    if (ts->io_reactor_fd >= 0 && ts->io_reactor_drain) {
        reactor_pfd_index = nfds;
        if (js_poll_add_poll_fd(ts, &nfds, ts->io_reactor_fd, POLLIN))
            return -1;
    }
    int poster_pfd_index = -1;
    int live_workers = !ts->recv_pipe
        && atomic_load_explicit(&js_live_worker_threads, memory_order_acquire) != 0;
    if (ts->poster && (ts->post_holds > 0 || live_workers || (!ts->recv_pipe && !list_empty(&ts->os_signal_handlers)))) {
        poster_pfd_index = nfds;
        if (js_poll_add_poll_fd(ts, &nfds, ts->poster->waker.read_fd, POLLIN))
            return -1;
    }

    int reactor_wait = ts->io_reactor_fd >= 0 && (ts->io_reactor_wait || ts->io_reactor_wait_us) && ts->io_reactor_drain
        && list_empty(&ts->os_rw_handlers) && list_empty(&ts->port_list)
        && !ts->recv_pipe && ts->post_holds == 0;

    int poster_in_reactor = 0;
    if (reactor_wait && poster_pfd_index >= 0 && ts->io_reactor_watch) {
        int pfd = ts->poster->waker.read_fd;
        if (ts->io_reactor_watched_fd != pfd
            && ts->io_reactor_watch(ts->io_reactor_udata, pfd, js_loop_poster_seen, ts) == 0)
            ts->io_reactor_watched_fd = pfd;
        poster_in_reactor = ts->io_reactor_watched_fd == pfd;
    }

    if ((reactor_wait && !poster_in_reactor) || poster_pfd_index < 0) {
        if (live_workers && (min_delay < 0 || min_delay > WORKER_EXIT_POLL_MS))
            min_delay = WORKER_EXIT_POLL_MS;
        if (!list_empty(&ts->os_signal_handlers) && (min_delay < 0 || min_delay > JS_SIGNAL_POLL_MS))
            min_delay = JS_SIGNAL_POLL_MS;
    }

    {
        int deadline_left = js_std_deadline_remaining_ms();
        if (deadline_left >= 0 && (min_delay < 0 || min_delay > deadline_left))
            min_delay = deadline_left;
    }
    if (watch_pfd_index >= 0 && (min_delay < 0 || min_delay > JS_WATCH_SWEEP_MS))
        min_delay = JS_WATCH_SWEEP_MS;
    if (min_delay >= 0 && (min_delay_us < 0 || (int64_t)min_delay * 1000 < min_delay_us))
        min_delay_us = (int64_t)min_delay * 1000;

    ts->stat_waits++;
    if (reactor_wait) {
        ts->poster_ready = 0;
        if (ts->io_reactor_wait_us)
            ts->io_reactor_wait_us(ts->io_reactor_udata, min_delay_us);
        else
            ts->io_reactor_wait(ts->io_reactor_udata, min_delay);
        if (ts->poster_ready && ts->poster)
            js_loop_run_posts(ctx, ts, 1);
        goto done;
    }

#ifdef CONFIG_NATIVE_MODULES
    dyn_evloop_flush_pending();
#endif
    nfds = js_poll_wait_us(ts, nfds, min_delay, min_delay_us);
    if (nfds > 0) {
        uint32_t gen;
        int budget = JS_LOOP_BATCH, did = 0, r;

        if (poster_pfd_index >= 0 && ts->poll_fds[poster_pfd_index].revents) {
            js_loop_run_posts(ctx, ts, 1);
            goto done;
        }
        if (reactor_pfd_index >= 0 && (ts->poll_fds[reactor_pfd_index].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            ts->io_reactor_drain(ts->io_reactor_udata);
            goto done;
        }
        gen = ts->rw_gen;
        list_for_each_safe(el, el1, &ts->os_rw_handlers)
        {
            rh = list_entry(el, JSOSRWHandler, link);
            if (rh->poll_fd_index < 0 || (JS_IsNull(rh->rw_func[0]) && JS_IsNull(rh->rw_func[1])))
                continue;
            r = js_rw_fire(ctx, ts, rh, ts->poll_fds[rh->poll_fd_index].revents, &gen, &budget, &did);
            if (r == JS_LOOP_JOB_ERROR)
                return r;
            if (r)
                goto done;
        }
        if (!did && watch_pfd_index >= 0 && ts->poll_fds[watch_pfd_index].revents) {
            JSOSRWHandler* ready[JS_LOOP_BATCH];
            int nr = js_watch_fetch(ts, ready), j;
            for (j = 0; j < nr; j++) {
                rh = ready[j];
                if (JS_IsNull(rh->rw_func[0]) && JS_IsNull(rh->rw_func[1]))
                    continue;
                r = js_rw_fire(ctx, ts, rh, (short)rh->watch_re, &gen, &budget, &did);
                if (r == JS_LOOP_JOB_ERROR)
                    return r;
                if (r)
                    goto done;
            }
        }
        if (did)
            goto done;

        list_for_each(el, &ts->port_list)
        {
            JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
            uint32_t pgen = ts->port_gen;
            int k;

            if (JS_IsNull(port->on_message_func) || ts->poll_fds[port->poll_fd_index].revents == 0)
                continue;
            for (k = 0; k < JS_LOOP_BATCH; k++) {
                if (!handle_posted_message(rt, ctx, port))
                    break;
                ts->stat_messages++;
                did = 1;
                r = js_loop_after_callback(ctx);
                if (r == JS_LOOP_JOB_ERROR)
                    return r;
                if (r || ts->port_gen != pgen || ts->rw_gen != gen)
                    goto done;
            }
            if (did)
                goto done;
        }
    }
done:
    return 0;
}
#endif

#ifdef USE_WORKER
void js_free_message(JSWorkerMessage* msg)
{
    size_t i;
    for (i = 0; i < msg->sab_tab_len; i++) {
        js_sab_free(NULL, msg->sab_tab[i]);
    }
    dyn_wmsg_free(msg->sab_tab);
    dyn_wmsg_free(msg->data);
    dyn_wmsg_free(msg);
}
#endif

static int js_std_deadline_expired(void)
{
    return js_std_deadline_hit();
}

static _Thread_local int std_handlers_freed;

void js_std_init_handlers(JSRuntime* rt)
{
    JSThreadState* ts;

    std_handlers_freed = 0;

    signal(SIGPIPE, SIG_IGN);

    ts = malloc(sizeof(*ts));
    if (!ts) {
        fprintf(stderr, "Could not allocate memory for the worker");
        exit(1);
    }
    memset(ts, 0, sizeof(*ts));
    init_list_head(&ts->os_rw_handlers);
    init_list_head(&ts->os_signal_handlers);
    init_list_head(&ts->port_list);
    init_list_head(&ts->worker_obj_list);
    init_list_head(&ts->rejected_promise_list);
    ts->next_timer_id = 1;
    ts->io_reactor_fd = -1;
    ts->io_reactor_watched_fd = -1;
#if !defined(_WIN32)
    ts->watch_fd = -1;
    ts->deadline_kq = -1;
#endif

    JS_SetRuntimeOpaque(rt, ts);

#ifdef USE_WORKER
    {
        JSSharedArrayBufferFunctions sf;
        memset(&sf, 0, sizeof(sf));
        sf.sab_alloc = js_sab_alloc;
        sf.sab_free = js_sab_free;
        sf.sab_dup = js_sab_dup;
        sf.sab_valid_ptr = js_sab_valid_ptr;
        JS_SetSharedArrayBufferFunctions(rt, &sf);
    }
#endif
}

void js_std_free_handlers(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    struct list_head *el, *el1;

    std_handlers_freed = 1;

    if (ts->job_error_set) {
        JS_FreeValueRT(rt, ts->job_error);
        ts->job_error_set = 0;
    }
    if (ts->streams_ns_set) {
        JS_FreeValueRT(rt, ts->streams_ns);
        ts->streams_ns_set = 0;
    }
    wst_free_rt(rt, ts);
    js_blob_free_rt(rt, ts);
    if (ts->shutdown_hook)
        ts->shutdown_hook(rt);
    if (ts->io_reactor_detach)
        ts->io_reactor_detach(rt);

#if !defined(_WIN32)
    if (ts->watch_fd >= 0) {
        close(ts->watch_fd);
        ts->watch_fd = -1;
    }
    if (ts->deadline_kq >= 0) {
        close(ts->deadline_kq);
        ts->deadline_kq = -1;
    }
#endif
    list_for_each_safe(el, el1, &ts->os_rw_handlers)
    {
        JSOSRWHandler* rh = list_entry(el, JSOSRWHandler, link);
        free_rw_handler(rt, rh);
    }

    list_for_each_safe(el, el1, &ts->os_signal_handlers)
    {
        JSOSSignalHandler* sh = list_entry(el, JSOSSignalHandler, link);
        free_sh(rt, sh);
    }

    while (ts->n_timers > 0)
        free_timer(rt, timer_first(ts));
    js_free_rt(rt, ts->timer_heap);
    js_free_rt(rt, ts->timer_ids);
    {
        uint32_t i;
        for (i = 0; i < ts->timer_lists_cap; i++) {
            JSOSTimerList* l = ts->timer_lists[i];
            while (l) {
                JSOSTimerList* next = l->map_next;
                js_free_rt(rt, l);
                l = next;
            }
        }
    }
    js_free_rt(rt, ts->timer_lists);
    ts->n_idle_timer_lists = ts->n_mapped_timer_lists = 0;
    ts->timer_list_last = NULL;
    ts->timer_heap = NULL;
    ts->timer_ids = NULL;
    ts->timer_lists = NULL;
    ts->timer_heap_cap = ts->timer_ids_cap = ts->timer_lists_cap = 0;

    list_for_each_safe(el, el1, &ts->rejected_promise_list)
    {
        JSRejectedPromiseEntry* rp = list_entry(el, JSRejectedPromiseEntry, link);
        JS_FreeValueRT(rt, rp->promise);
        JS_FreeValueRT(rt, rp->reason);
        free(rp);
    }

#ifdef USE_WORKER
    js_release_worker_refs(rt, ts);

    js_free_message_pipe(ts->recv_pipe);
    js_free_message_pipe(ts->send_pipe);

    list_for_each_safe(el, el1, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        port->link.prev = NULL;
        port->link.next = NULL;
    }
#endif

#if !defined(_WIN32)
    free(ts->poll_fds);
    js_loop_poster_free(ts);
#endif

    free(ts);
    JS_SetRuntimeOpaque(rt, NULL);
}

void js_std_promise_rejection_tracker(JSContext* ctx, JSValueConst promise,
    JSValueConst reason,
    BOOL is_handled, void* opaque)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSRejectedPromiseEntry* rp;

    if (!ts || std_handlers_freed)
        return;

    if (!is_handled) {
        rp = find_rejected_promise(ctx, ts, promise);
        if (!rp) {
            rp = malloc(sizeof(*rp));
            if (rp) {
                rp->promise = JS_DupValue(ctx, promise);
                rp->reason = JS_DupValue(ctx, reason);
                list_add_tail(&rp->link, &ts->rejected_promise_list);
            }
        }
    } else {
        rp = find_rejected_promise(ctx, ts, promise);
        if (rp) {
            JS_FreeValue(ctx, rp->promise);
            JS_FreeValue(ctx, rp->reason);
            list_del(&rp->link);
            free(rp);
        }
    }
}

static void js_std_promise_rejection_check(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    struct list_head* el;

    if (unlikely(!list_empty(&ts->rejected_promise_list))) {
        struct list_head tmp = LIST_HEAD_INIT(tmp);
        tmp.next = ts->rejected_promise_list.next;
        tmp.prev = ts->rejected_promise_list.prev;
        ts->rejected_promise_list.next->prev = &tmp;
        ts->rejected_promise_list.prev->next = &tmp;
        ts->rejected_promise_list.next = &ts->rejected_promise_list;
        ts->rejected_promise_list.prev = &ts->rejected_promise_list;

        list_for_each(el, &tmp)
        {
            JSRejectedPromiseEntry* rp = list_entry(el, JSRejectedPromiseEntry, link);
            dyn_stdio_sync();
            fprintf(stderr, "Possibly unhandled promise rejection: ");
            js_std_dump_error1(ctx, rp->reason);
        }
        exit(1);
    }
}

int js_std_loop(JSContext* ctx)
{
    int err;

    for (;;) {
        for (;;) {
            err = js_run_jobs(ctx, JS_UNDEFINED);
            if (err <= 0) {
                if (err < 0) {
                    js_std_dump_error(ctx);
                    return -1;
                }
                break;
            }
        }

        if (js_std_deadline_expired())
            goto deadline;

        js_std_promise_rejection_check(ctx);
        dyn_stdio_idle();

        JSOSPollFunc poll = atomic_load_explicit(&os_poll_func,
            memory_order_relaxed);
        if (!poll)
            break;
        err = poll(ctx);
        if (err == JS_LOOP_JOB_ERROR) {
            js_std_dump_error(ctx);
            return -1;
        }
        if (err)
            break;
    }
    {
        JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
        dyn_stdio_sync();
        if (!(ts && ts->recv_pipe) && dyn_stdio_report_unreported())
            return -1;
    }
    return 0;
deadline:
    fprintf(stderr, "InternalError: interrupted\n");
    return -1;
}

JSValue js_std_await(JSContext* ctx, JSValue obj)
{
    JSValue ret;
    int state;

    for (;;) {
        state = JS_PromiseState(ctx, obj);
        if (state == JS_PROMISE_FULFILLED) {
            ret = JS_PromiseResult(ctx, obj);
            JS_FreeValue(ctx, obj);
            break;
        } else if (state == JS_PROMISE_REJECTED) {
            ret = JS_Throw(ctx, JS_PromiseResult(ctx, obj));
            JS_FreeValue(ctx, obj);
            break;
        } else if (state == JS_PROMISE_PENDING) {
            int err;
            err = js_run_jobs(ctx, obj);
            if (err < 0) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            if (err == 0) {
                JSOSPollFunc poll;
                if (js_std_deadline_expired()) {
                    JS_FreeValue(ctx, obj);
                    JS_ThrowInternalError(ctx, "interrupted");
                    JS_SetUncatchableException(ctx, TRUE);
                    return JS_EXCEPTION;
                }
                js_std_promise_rejection_check(ctx);
                dyn_stdio_idle();

                poll = atomic_load_explicit(&os_poll_func,
                    memory_order_relaxed);
                err = poll ? poll(ctx) : -1;
                if (err == JS_LOOP_JOB_ERROR) {
                    JS_FreeValue(ctx, obj);
                    return JS_EXCEPTION;
                }
                if (err && !JS_IsJobPending(JS_GetRuntime(ctx))
                    && JS_PromiseState(ctx, obj) == JS_PROMISE_PENDING) {
                    JS_FreeValue(ctx, obj);
                    return JS_ThrowInternalError(ctx, "awaited promise can never settle: no pending work is left");
                }
            }
        } else {
            ret = obj;
            break;
        }
    }
    return ret;
}
