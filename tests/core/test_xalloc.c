/* tests/core/test_xalloc.c -- tests/lib/dyn-xalloc.h: xmalloc/xcalloc/xrealloc return usable blocks, a
 * zero-size request never aborts, and a failing request aborts with "file:line: out of memory" on stderr
 * (checked in a forked child: SIGABRT plus the message), and each macro argument is evaluated once (a side-effecting
 * size such as (cap *= 2) used to double twice and overrun). Seeded sizes via dyn-fuzzkit.h. */
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../lib/dyn-fuzzkit.h"
#include "../lib/dyn-xalloc.h"

static int child_aborts(int kind, char* msg, size_t cap)
{
    int fds[2], st = 0;
    ssize_t n;
    pid_t pid;
    if (pipe(fds) != 0)
        return 0;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        void* p = NULL;
        dup2(fds[1], 2);
        if (kind == 0)
            p = xmalloc(SIZE_MAX / 2);
        else if (kind == 1)
            p = xcalloc(SIZE_MAX / 4, 4);
        else
            p = xrealloc(NULL, SIZE_MAX / 2);
        _exit(p ? 3 : 4);
    }
    close(fds[1]);
    n = read(fds[0], msg, cap - 1);
    msg[n > 0 ? n : 0] = 0;
    close(fds[0]);
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

int main(void)
{
    fk_rng_t r;
    unsigned long i, iters;
    int k;
    fk_init("test_xalloc");
    r = fk_stream("blocks");
    iters = fk_iters(2000);
    for (i = 0; i < iters; i++) {
        size_t n = fk_size(&r, 1 << 16);
        unsigned char* a = xmalloc(n);
        unsigned char* b = xcalloc(n, 1);
        size_t j;
        if (n) {
            memset(a, 0xA5, n);
            for (j = 0; j < n; j++)
                FK_CHECK(b[j] == 0, "xcalloc not zeroed at %zu/%zu", j, n);
        }
        a = xrealloc(a, n + 1);
        FK_CHECK(a != NULL, "xrealloc(%zu) returned NULL", n + 1);
        FK_CHECK(!n || a[n - 1] == 0xA5, "xrealloc lost data at %zu", n);
        free(a);
        free(b);
    }
    fk_case = "single-eval";
    {
        size_t cap = 8, na = 3, nb = 4;
        unsigned* q = xmalloc((na += 1) * sizeof *q);
        unsigned* z = xcalloc((na += 1), (nb += 1) * sizeof *z);
        FK_CHECK(na == 5 && nb == 5, "xmalloc/xcalloc evaluated an argument twice: na %zu nb %zu", na, nb);
        q = xrealloc(q, (cap *= 2) * sizeof *q);
        FK_CHECK(cap == 16, "xrealloc evaluated its size twice: cap %zu after one doubling", cap);
        q[cap - 1] = 1;
        free(q);
        free(z);
    }
    free(xmalloc(0));
    free(xcalloc(0, 8));
    for (k = 0; k < 3; k++) {
        char msg[256];
        fk_case = "abort";
        FK_CHECK(child_aborts(k, msg, sizeof msg), "kind %d: a failing request must abort", k);
        FK_CHECK(strstr(msg, "test_xalloc.c:") && strstr(msg, ": out of memory"), "kind %d: stderr '%s' lacks file:line", k, msg);
    }
    return fk_done("test_xalloc");
}
