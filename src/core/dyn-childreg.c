#include "dyn-childreg.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DYN_CHILDREG_NSIG 3

static const int dyn_childreg_sigs[DYN_CHILDREG_NSIG] = { SIGTERM, SIGINT, SIGHUP };
static _Atomic int dyn_childreg_slot[DYN_CHILDREG_MAX];
static atomic_int dyn_childreg_hwm;
static atomic_int dyn_childreg_owner;
static struct sigaction dyn_childreg_old[DYN_CHILDREG_NSIG];
static pthread_once_t dyn_childreg_once = PTHREAD_ONCE_INIT;

static int64_t dyn_childreg_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void dyn_childreg_nap(void)
{
    struct timespec ts = { 0, 10 * 1000000 };
    nanosleep(&ts, NULL);
}

static int dyn_childreg_group_alive(pid_t p)
{
    return kill(-p, 0) == 0 || errno == EPERM;
}

static void dyn_childreg_signal_all(int sig)
{
    int i, n = atomic_load(&dyn_childreg_hwm);
    for (i = 0; i < n; i++) {
        int p = atomic_load(&dyn_childreg_slot[i]);
        if (p > 0 && kill(-p, sig) != 0)
            kill(p, sig);
    }
}

static int dyn_childreg_reap_pass(unsigned char* reaped)
{
    int i, live = 0, n = atomic_load(&dyn_childreg_hwm);
    for (i = 0; i < n; i++) {
        int st, p = atomic_load(&dyn_childreg_slot[i]);
        if (p <= 0)
            continue;
        if (!(reaped[i >> 3] & (1u << (i & 7)))) {
            pid_t r = waitpid(p, &st, WNOHANG);
            if (r == p || (r < 0 && errno == ECHILD))
                reaped[i >> 3] |= (unsigned char)(1u << (i & 7));
        }
        if (!(reaped[i >> 3] & (1u << (i & 7))) || dyn_childreg_group_alive(p))
            live++;
        else
            atomic_compare_exchange_strong(&dyn_childreg_slot[i], &p, 0);
    }
    return live;
}

static int dyn_childreg_wait_gone(unsigned char* reaped, int ms)
{
    int64_t deadline = dyn_childreg_now_ms() + ms;
    for (;;) {
        if (!dyn_childreg_reap_pass(reaped))
            return 1;
        if (dyn_childreg_now_ms() >= deadline)
            return 0;
        dyn_childreg_nap();
    }
}

void dyn_childreg_kill_all(int grace_ms)
{
    unsigned char reaped[DYN_CHILDREG_MAX / 8];
    int owner = atomic_load(&dyn_childreg_owner);
    int saved = errno;

    if (!owner || owner != (int)getpid())
        return;
    memset(reaped, 0, sizeof reaped);
    dyn_childreg_signal_all(SIGTERM);
    if (!dyn_childreg_wait_gone(reaped, grace_ms)) {
        dyn_childreg_signal_all(SIGKILL);
        dyn_childreg_wait_gone(reaped, 500);
    }
    errno = saved;
}

void dyn_childreg_kill_now(void)
{
    int owner = atomic_load(&dyn_childreg_owner);
    int saved = errno;
    if (owner && owner == (int)getpid())
        dyn_childreg_signal_all(SIGKILL);
    errno = saved;
}

static void dyn_childreg_on_signal(int sig)
{
    int i;
    dyn_childreg_kill_all(DYN_CHILDREG_GRACE_MS);
    for (i = 0; i < DYN_CHILDREG_NSIG; i++)
        if (dyn_childreg_sigs[i] == sig)
            sigaction(sig, &dyn_childreg_old[i], NULL);
    raise(sig);
}

int dyn_childreg_is_handler(const struct sigaction* sa)
{
    return !(sa->sa_flags & SA_SIGINFO) && sa->sa_handler == dyn_childreg_on_signal;
}

static void dyn_childreg_atexit(void)
{
    dyn_childreg_kill_all(DYN_CHILDREG_GRACE_MS);
}

static void dyn_childreg_install(void)
{
    int i;
    atomic_store(&dyn_childreg_owner, (int)getpid());
    atexit(dyn_childreg_atexit);
    for (i = 0; i < DYN_CHILDREG_NSIG; i++) {
        struct sigaction sa;
        if (sigaction(dyn_childreg_sigs[i], NULL, &dyn_childreg_old[i]) != 0
            || (dyn_childreg_old[i].sa_flags & SA_SIGINFO)
            || dyn_childreg_old[i].sa_handler != SIG_DFL)
            continue;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = dyn_childreg_on_signal;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(dyn_childreg_sigs[i], &sa, NULL);
    }
}

void dyn_childreg_block(sigset_t* old)
{
    sigset_t set;
    int i;
    sigemptyset(&set);
    for (i = 0; i < DYN_CHILDREG_NSIG; i++)
        sigaddset(&set, dyn_childreg_sigs[i]);
    pthread_sigmask(SIG_BLOCK, &set, old);
}

void dyn_childreg_unblock(const sigset_t* old)
{
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

void dyn_childreg_add(pid_t pgid)
{
    int i;
    if (pgid <= 0)
        return;
    pthread_once(&dyn_childreg_once, dyn_childreg_install);
    for (i = 0; i < DYN_CHILDREG_MAX; i++) {
        int z = 0;
        if (atomic_compare_exchange_strong(&dyn_childreg_slot[i], &z, (int)pgid)) {
            int h = atomic_load(&dyn_childreg_hwm);
            while (h < i + 1 && !atomic_compare_exchange_weak(&dyn_childreg_hwm, &h, i + 1)) {
            }
            return;
        }
    }
}

void dyn_childreg_del(pid_t pgid)
{
    int i, n = atomic_load(&dyn_childreg_hwm);
    if (pgid <= 0)
        return;
    for (i = 0; i < n; i++) {
        int p = (int)pgid;
        if (atomic_compare_exchange_strong(&dyn_childreg_slot[i], &p, 0))
            return;
    }
}
