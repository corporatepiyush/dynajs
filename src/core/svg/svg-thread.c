#include "svg-int.h"

#if !defined(SVG2IMG_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#define SVG_HAVE_THREADS 1
#endif

#define SVG_MAX_THREADS 64

#ifdef SVG_HAVE_THREADS
typedef struct {
    void (*fn)(void*, int);
    void* arg;
    int   k;
} WorkerStart;

static size_t worker_stack_size(void) {
    size_t sz = (size_t)8 << 20;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        (size_t)rl.rlim_cur > sz)
        sz = (size_t)rl.rlim_cur;
    if (sz > ((size_t)64 << 20)) sz = (size_t)64 << 20;
    return sz;
}

static void* worker_main(void* p) {
    WorkerStart* ws = (WorkerStart*)p;
    svg_alloc_worker(1);
    ws->fn(ws->arg, ws->k);
    svg_alloc_worker(0);
    return NULL;
}
#endif

void svg_parallel_run(int n, void (*fn)(void* arg, int worker), void* arg) {
    if (n < 1) n = 1;
    if (n > SVG_MAX_THREADS) n = SVG_MAX_THREADS;
#ifdef SVG_HAVE_THREADS
    pthread_t   tid[SVG_MAX_THREADS];
    WorkerStart ws[SVG_MAX_THREADS];
    int started[SVG_MAX_THREADS];
    pthread_attr_t attr;
    int have_attr = n > 1 && pthread_attr_init(&attr) == 0;
    if (have_attr && pthread_attr_setstacksize(&attr, worker_stack_size()) != 0) {

        pthread_attr_destroy(&attr);
        have_attr = 0;
    }
    for (int k = 1; k < n; k++) {
        ws[k].fn = fn;
        ws[k].arg = arg;
        ws[k].k = k;
        started[k] = have_attr &&
                     pthread_create(&tid[k], &attr, worker_main, &ws[k]) == 0;
    }
    if (have_attr) pthread_attr_destroy(&attr);
    fn(arg, 0);
    for (int k = 1; k < n; k++)
        if (started[k]) pthread_join(tid[k], NULL);
#else
    fn(arg, 0);
    (void)n;
#endif
}

int svg_cpu_count(void) {
#if defined(SVG_HAVE_THREADS) && defined(_SC_NPROCESSORS_ONLN)
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    if (v < 1) return 1;
    return v > SVG_MAX_THREADS ? SVG_MAX_THREADS : (int)v;
#else
    return 1;
#endif
}
