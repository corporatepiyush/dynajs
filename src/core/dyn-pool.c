#include "dyn-pool.h"
#include "dyn-cdefs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sysinfo.h>
#include <sys/eventfd.h>
#else
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#define POOL_MIN_THREADS 4u
#define POOL_MAX_THREADS 1024u

typedef struct {
    dyn_pool_fn work;
    dyn_pool_fn done;
    void* arg;
    dyn_pool_chan_t* ch;
} pool_job_t;

struct dyn_pool_chan {
    dyn_pool_t* pool;
    pool_job_t* cdone;
    unsigned chead, ctail, ccount;
    int wake_r, wake_w;
    int wake_pending;
    size_t inflight;
    unsigned running;
    int closing;
    dyn_pool_chan_t* next;
};

struct dyn_pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_cond_t quiesce;

    pool_job_t* slots;
    unsigned cap;
    unsigned head, tail, count;

    pthread_t* threads;
    unsigned nthreads;

    dyn_pool_chan_t* chans;
    int stopping;
    size_t inflight;
};

static unsigned pool_default_override;

static int pool_inline_only;

void dyn_pool_set_default_threads(unsigned n)
{
    if (n == 0) {
        pool_inline_only = 1;
        pool_default_override = 0;
        return;
    }
    if (n > POOL_MAX_THREADS)
        n = POOL_MAX_THREADS;
    pool_inline_only = 0;
    pool_default_override = n;
}

int dyn_pool_is_inline_only(void) { return pool_inline_only; }

unsigned dyn_pool_default_threads(void)
{
    long n = 0;
    if (pool_default_override)
        return pool_default_override;
#if defined(__linux__)
    n = get_nprocs();
#elif defined(_SC_NPROCESSORS_ONLN)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < (long)POOL_MIN_THREADS)
        n = (long)POOL_MIN_THREADS;
    if (n > (long)POOL_MAX_THREADS)
        n = (long)POOL_MAX_THREADS;
    return (unsigned)n;
}

static void chan_signal_locked(dyn_pool_chan_t* ch)
{
    if (ch->wake_pending)
        return;
#if defined(__linux__)
    {
        uint64_t one = 1;
        if (write(ch->wake_w, &one, sizeof(one)) == (ssize_t)sizeof(one))
            ch->wake_pending = 1;
    }
#else
    {
        unsigned char b = 1;
        if (write(ch->wake_w, &b, 1) == 1)
            ch->wake_pending = 1;
    }
#endif
}

static void* pool_worker(void* arg)
{
    dyn_pool_t* p = (dyn_pool_t*)arg;

    for (;;) {
        pool_job_t j;

        pthread_mutex_lock(&p->mu);
        while (p->count == 0 && !p->stopping)
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->stopping && p->count == 0) {
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        j = p->slots[p->head];
        p->head = (p->head + 1u) % p->cap;
        p->count--;
        j.ch->running++;
        pthread_mutex_unlock(&p->mu);

        if (j.work)
            j.work(j.arg);

        pthread_mutex_lock(&p->mu);
        j.ch->running--;
        if (j.ch->closing) {
            j.ch->inflight--;
            p->inflight--;
            pthread_cond_broadcast(&p->quiesce);
        } else {
            j.ch->cdone[j.ch->ctail] = j;
            j.ch->ctail = (j.ch->ctail + 1u) % p->cap;
            j.ch->ccount++;
            chan_signal_locked(j.ch);
            pthread_cond_broadcast(&p->quiesce);
        }
        pthread_mutex_unlock(&p->mu);
    }
}

dyn_pool_t* dyn_pool_new(unsigned nthreads, unsigned queue_cap)
{
    dyn_pool_t* p;
    unsigned i;
    int fds[2];

    if (pool_inline_only)
        return NULL;
    if (nthreads == 0)
        nthreads = dyn_pool_default_threads();
    if (nthreads > POOL_MAX_THREADS)
        nthreads = POOL_MAX_THREADS;
    if (queue_cap == 0)
        queue_cap = nthreads * 4u;
    if (queue_cap < 8u)
        queue_cap = 8u;

    (void)fds;
    p = (dyn_pool_t*)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->cap = queue_cap;
    p->nthreads = nthreads;

    p->slots = (pool_job_t*)calloc(queue_cap, sizeof(pool_job_t));
    p->threads = (pthread_t*)calloc(nthreads, sizeof(pthread_t));
    if (!p->slots || !p->threads)
        goto fail;

    if (pthread_mutex_init(&p->mu, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&p->cv, NULL) != 0) {
        pthread_mutex_destroy(&p->mu);
        goto fail;
    }
    if (pthread_cond_init(&p->quiesce, NULL) != 0) {
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
        goto fail;
    }

    for (i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, pool_worker, p) != 0) {
            pthread_mutex_lock(&p->mu);
            p->stopping = 1;
            pthread_cond_broadcast(&p->cv);
            pthread_mutex_unlock(&p->mu);
            while (i-- > 0)
                pthread_join(p->threads[i], NULL);
            pthread_cond_destroy(&p->quiesce);
            pthread_cond_destroy(&p->cv);
            pthread_mutex_destroy(&p->mu);
            goto fail;
        }
    }
    return p;

fail:
    free(p->slots);
    free(p->threads);
    free(p);
    return NULL;
}

void dyn_pool_free(dyn_pool_t* p)
{
    unsigned i;
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->stopping = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    while (p->chans) {
        dyn_pool_chan_t* ch = p->chans;
        p->chans = ch->next;
        if (ch->wake_r >= 0)
            close(ch->wake_r);
        if (ch->wake_w >= 0 && ch->wake_w != ch->wake_r)
            close(ch->wake_w);
        free(ch->cdone);
        free(ch);
    }
    pthread_cond_destroy(&p->quiesce);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p->slots);
    free(p->threads);
    free(p);
}

unsigned dyn_pool_threads(const dyn_pool_t* p) { return p ? p->nthreads : 0; }
unsigned dyn_pool_capacity(const dyn_pool_t* p) { return p ? p->cap : 0; }

dyn_pool_chan_t* dyn_pool_chan_new(dyn_pool_t* p)
{
    dyn_pool_chan_t* ch;

    if (!p)
        return NULL;
    ch = (dyn_pool_chan_t*)calloc(1, sizeof(*ch));
    if (!ch)
        return NULL;
    ch->pool = p;
    ch->wake_r = ch->wake_w = -1;
    ch->cdone = (pool_job_t*)calloc(p->cap, sizeof(pool_job_t));
    if (!ch->cdone)
        goto fail;
#if defined(__linux__)
    ch->wake_r = ch->wake_w = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ch->wake_r < 0)
        goto fail;
#else
    {
        int fds[2];
        if (pipe(fds) != 0)
            goto fail;
        ch->wake_r = fds[0];
        ch->wake_w = fds[1];
        fcntl(ch->wake_r, F_SETFL, fcntl(ch->wake_r, F_GETFL, 0) | O_NONBLOCK);
        fcntl(ch->wake_w, F_SETFL, fcntl(ch->wake_w, F_GETFL, 0) | O_NONBLOCK);
        fcntl(ch->wake_r, F_SETFD, FD_CLOEXEC);
        fcntl(ch->wake_w, F_SETFD, FD_CLOEXEC);
    }
#endif

    pthread_mutex_lock(&p->mu);
    ch->next = p->chans;
    p->chans = ch;
    pthread_mutex_unlock(&p->mu);
    return ch;

fail:
    free(ch->cdone);
    free(ch);
    return NULL;
}

void dyn_pool_chan_free(dyn_pool_chan_t* ch)
{
    dyn_pool_t* p;
    dyn_pool_chan_t** pp;

    if (!ch)
        return;
    p = ch->pool;
    pthread_mutex_lock(&p->mu);
    ch->closing = 1;
    while (ch->running > 0 || ch->inflight > ch->ccount)
        pthread_cond_wait(&p->quiesce, &p->mu);
    for (pp = &p->chans; *pp; pp = &(*pp)->next) {
        if (*pp == ch) {
            *pp = ch->next;
            break;
        }
    }
    pthread_mutex_unlock(&p->mu);

    if (ch->wake_r >= 0)
        close(ch->wake_r);
    if (ch->wake_w >= 0 && ch->wake_w != ch->wake_r)
        close(ch->wake_w);
    free(ch->cdone);
    free(ch);
}

int dyn_pool_wake_fd(const dyn_pool_chan_t* ch) { return ch ? ch->wake_r : -1; }

size_t dyn_pool_inflight(const dyn_pool_chan_t* ch)
{
    size_t n;
    dyn_pool_chan_t* m = DYN_UNCONST(ch);
    if (!ch)
        return 0;
    pthread_mutex_lock(&m->pool->mu);
    n = m->inflight;
    pthread_mutex_unlock(&m->pool->mu);
    return n;
}

int dyn_pool_submit(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg)
{
    dyn_pool_t* p;
    if (!ch)
        return -1;
    p = ch->pool;
    pthread_mutex_lock(&p->mu);
    if (p->stopping || ch->closing || p->inflight >= (size_t)p->cap) {
        pthread_mutex_unlock(&p->mu);
        errno = EAGAIN;
        return -1;
    }
    p->slots[p->tail].work = work;
    p->slots[p->tail].done = done;
    p->slots[p->tail].arg = arg;
    p->slots[p->tail].ch = ch;
    p->tail = (p->tail + 1u) % p->cap;
    p->count++;
    p->inflight++;
    ch->inflight++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int dyn_pool_drain(dyn_pool_chan_t* ch)
{
    dyn_pool_t* p;
    int ran = 0;

    if (!ch)
        return 0;
    p = ch->pool;
    pthread_mutex_lock(&p->mu);
    {
        uint64_t v;
        while (read(ch->wake_r, &v, sizeof(v)) > 0)
            ;
    }
    ch->wake_pending = 0;

    while (ch->ccount > 0) {
        pool_job_t j = ch->cdone[ch->chead];
        ch->chead = (ch->chead + 1u) % p->cap;
        ch->ccount--;
        ch->inflight--;
        p->inflight--;
        pthread_mutex_unlock(&p->mu);
        if (j.done)
            j.done(j.arg);
        ran++;
        pthread_mutex_lock(&p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    return ran;
}
