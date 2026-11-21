#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-parking-lot.h"

#ifndef DYN_DS_NO_THREADS
#define DS_PARK_BUCKETS 512

typedef struct ds_parker {
    const void* addr;
    pthread_cond_t cv;
    int woken;
    struct ds_parker *prev, *next;
} ds_parker;

typedef struct {
    pthread_mutex_t mu;
    ds_parker *head, *tail;
    char pad[64];
} ds_park_bucket;

static ds_park_bucket ds_park_tab[DS_PARK_BUCKETS];

static pthread_once_t ds_park_once = PTHREAD_ONCE_INIT;

static void ds_park_init(void)
{
    unsigned i;
    for (i = 0; i < DS_PARK_BUCKETS; i++) {
        pthread_mutex_init(&ds_park_tab[i].mu, NULL);
        ds_park_tab[i].head = ds_park_tab[i].tail = NULL;
    }
}

static ds_park_bucket* ds_park_bucket_of(const void* addr)
{
    pthread_once(&ds_park_once, ds_park_init);
    return &ds_park_tab[(ds_mix64((uint64_t)(uintptr_t)addr) >> 32) & (DS_PARK_BUCKETS - 1)];
}

static void ds_park_unlink(ds_park_bucket* b, ds_parker* p)
{
    if (p->prev)
        p->prev->next = p->next;
    else
        b->head = p->next;
    if (p->next)
        p->next->prev = p->prev;
    else
        b->tail = p->prev;
    p->prev = p->next = NULL;
}

static int64_t ds_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int dyn_park_wait(const void* addr, dyn_park_validate validate, void* ud, int64_t timeout_ns)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker me;
    int64_t deadline = timeout_ns >= 0 ? ds_mono_ns() + timeout_ns : 0;
    int result = 0;
#if !defined(__APPLE__)
    pthread_condattr_t attr;
#endif
    pthread_mutex_lock(&b->mu);
    if (validate && !validate(ud)) {
        pthread_mutex_unlock(&b->mu);
        return -1;
    }
    me.addr = addr;
    me.woken = 0;
    me.next = NULL;
    me.prev = b->tail;
#if defined(__APPLE__)
    pthread_cond_init(&me.cv, NULL);
#else
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&me.cv, &attr);
    pthread_condattr_destroy(&attr);
#endif
    if (b->tail)
        b->tail->next = &me;
    else
        b->head = &me;
    b->tail = &me;
    while (!me.woken) {
        if (timeout_ns < 0) {
            pthread_cond_wait(&me.cv, &b->mu);
        } else {
            int64_t left = deadline - ds_mono_ns();
            struct timespec ts;
            if (left <= 0) {
                result = 1;
                break;
            }
#if defined(__APPLE__)
            ts.tv_sec = (time_t)(left / 1000000000);
            ts.tv_nsec = (long)(left % 1000000000);
            pthread_cond_timedwait_relative_np(&me.cv, &b->mu, &ts);
#else
            ts.tv_sec = (time_t)(deadline / 1000000000);
            ts.tv_nsec = (long)(deadline % 1000000000);
            pthread_cond_timedwait(&me.cv, &b->mu, &ts);
#endif
        }
    }
    if (!me.woken)
        ds_park_unlink(b, &me);
    pthread_mutex_unlock(&b->mu);
    pthread_cond_destroy(&me.cv);
    return me.woken ? 0 : result;
}

unsigned dyn_park_wake(const void* addr, unsigned count)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker *p, *next;
    unsigned n = 0;
    pthread_mutex_lock(&b->mu);
    for (p = b->head; p && n < count; p = next) {
        next = p->next;
        if (p->addr != addr)
            continue;
        ds_park_unlink(b, p);
        p->woken = 1;
        pthread_cond_signal(&p->cv);
        n++;
    }
    pthread_mutex_unlock(&b->mu);
    return n;
}

unsigned dyn_park_waiters(const void* addr)
{
    ds_park_bucket* b = ds_park_bucket_of(addr);
    ds_parker* p;
    unsigned n = 0;
    pthread_mutex_lock(&b->mu);
    for (p = b->head; p; p = p->next)
        n += p->addr == addr;
    pthread_mutex_unlock(&b->mu);
    return n;
}
#endif
