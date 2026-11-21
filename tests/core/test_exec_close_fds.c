/* test_exec_close_fds: dyn_close_fds_from(3) (src/dyna-io.c), the close-inherited-fds step every
 * fork->exec child runs (os.exec fork path, dyna:sys Spawn/Exec), compiled once per path with the
 * test-only defines so each branch runs on every CI platform:
 *   default            close_range (Linux >= 5.9) / proc_pidinfo (macOS) / closefrom (BSD)
 *   fail-close-range   close_range called but failing (EINVAL) -> the next method must run
 *   getdents           close_range absent -> /proc/self/fd walked with raw getdents64 (Linux)
 *   no-pidinfo         proc_pidinfo absent (macOS) -> plain loop
 *   loop               every faster method absent -> loop to max(RLIMIT_NOFILE soft, _SC_OPEN_MAX)
 * Each variant raises the soft RLIMIT_NOFILE to 4096, plants non-CLOEXEC fds at 5, 1500, 2000 and
 * 4095, forks, and the child reports through its exit status which planted fd survived and which
 * path ran (DYN_CLOSE_TEST_TRACE). fds 0-2 must survive. The base os.exec loop stopped at 1024, so
 * a cap at 1024 leaves 1500/2000/4095 open and fails the loop variant.
 * Build+run: tests/ctest/run.sh test-exec-close-fds (one binary per -DVARIANT_*; exit 77 = variant n/a here). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dyna-io.h"

extern volatile int dyn_close_path;

#ifndef VARIANT
#define VARIANT "default"
#endif

static const char* path_name(int p)
{
    switch (p) {
    case 1:
        return "close_range";
    case 2:
        return "closefrom";
    case 3:
        return "getdents";
    case 4:
        return "pidinfo";
    case 5:
        return "loop";
    default:
        return "none";
    }
}

static int want_path(void)
{
#if defined(VARIANT_LOOP) || (defined(VARIANT_NO_PIDINFO) && defined(__APPLE__))
    return 5;
#elif defined(__linux__)
#if defined(VARIANT_GETDENTS) || defined(VARIANT_FAIL_CLOSE_RANGE)
    return 3;
#else
    return 1;
#endif
#elif defined(__APPLE__)
    return 4;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    return 2;
#else
    return 5;
#endif
}

static int applicable(void)
{
#if defined(VARIANT_GETDENTS) || defined(VARIANT_FAIL_CLOSE_RANGE)
#if !defined(__linux__)
    return 0;
#endif
#endif
#if defined(VARIANT_NO_PIDINFO) && !defined(__APPLE__)
    return 0;
#endif
    return 1;
}

int main(void)
{
    static const int planted[] = { 5, 1500, 2000, 4095 };
    struct rlimit rl;
    int i, p[2], st = 0, survivors, path;
    pid_t pid;

    if (!applicable()) {
        printf("test_exec_close_fds[%s]: SKIP (this path does not exist on this platform)\n", VARIANT);
        return 77;
    }
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        printf("FAIL getrlimit: %s\n", strerror(errno));
        return 1;
    }
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 4096) {
        printf("FAIL hard RLIMIT_NOFILE %llu < 4096: cannot plant fd 4095\n", (unsigned long long)rl.rlim_max);
        return 1;
    }
    rl.rlim_cur = 4096;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        printf("FAIL setrlimit 4096: %s\n", strerror(errno));
        return 1;
    }
    if (pipe(p) != 0)
        return 1;
    for (i = 0; i < 4; i++) {
        if (dup2(p[0], planted[i]) != planted[i]) {
            printf("FAIL dup2 -> %d: %s\n", planted[i], strerror(errno));
            return 1;
        }
        fcntl(planted[i], F_SETFD, 0);
    }
    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        int bits = 0;
        dyn_close_fds_from(3);
        for (i = 0; i < 4; i++)
            if (fcntl(planted[i], F_GETFD) >= 0)
                bits |= 1 << i;
        for (i = 0; i < 3; i++)
            if (fcntl(i, F_GETFD) < 0)
                bits |= 0x10;
        _exit(bits | (dyn_close_path << 5));
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) {
        printf("FAIL child did not exit normally\n");
        return 1;
    }
    survivors = WEXITSTATUS(st) & 0x1f;
    path = WEXITSTATUS(st) >> 5;
    for (i = 0; i < 4; i++)
        printf("%s   [%s] fd %d closed in the child\n", survivors & (1 << i) ? "FAIL" : "ok  ", VARIANT, planted[i]);
    printf("%s   [%s] fds 0-2 kept\n", survivors & 0x10 ? "FAIL" : "ok  ", VARIANT);
    printf("%s   [%s] path %s (want %s)\n", path == want_path() ? "ok  " : "FAIL", VARIANT, path_name(path),
        path_name(want_path()));
    if (survivors || path != want_path()) {
        printf("test_exec_close_fds[%s]: FAILED\n", VARIANT);
        return 1;
    }
    printf("test_exec_close_fds[%s]: ok\n", VARIANT);
    return 0;
}
