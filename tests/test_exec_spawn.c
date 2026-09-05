/*
 * test_exec_spawn.c — pins the os.exec child-spawn mapping (audit M35-01).
 *
 * js_os_exec() needs a JSContext, so this test drives the spawn construction
 * directly: src/dyna-libc.c keeps the whole child-setup implementation
 * (my_execvpe + os_exec_fork_child + os_exec_spawn_child) between the
 * DYN_SPAWN_EXTRACT_BEGIN/END markers as pure libc, and this TU links
 * against an extracted copy of exactly that region. The engine-level glue
 * (JS option parsing, the WNOHANG/SIGTERM->SIGKILL wait ladder) is unchanged
 * by M35-01 and stays covered by tests/test_std.js.
 *
 * Build & run against the CURRENT implementation, from the repo root:
 *   mkdir -p /tmp/m35t
 *   sed -n '/^\/\* DYN_SPAWN_EXTRACT_BEGIN/,/^\/\* DYN_SPAWN_EXTRACT_END/p' \
 *       src/dyna-libc.c \
 *       | sed -e 's/^static int os_exec_spawn_child(/int os_exec_spawn_child(/' \
 *       > /tmp/m35t/spawn_impl.c
 *   cc -std=gnu17 -D_GNU_SOURCE -O1 -o /tmp/m35t/t_new \
 *       tests/test_exec_spawn.c /tmp/m35t/spawn_impl.c -lpthread
 * (glibc >= 2.34: add -DHAVE_CLOSEFROM, matching the Makefile compat probe;
 *  macOS: add -Wno-deprecated-declarations for addchdir_np.)
 *
 * Parity: build the same test against the PRE-M35-01 fork implementation by
 * wrapping git show HEAD:src/dyna-libc.c's child setup in this same helper
 * signature. Every functional assertion must hold for BOTH implementations;
 * only the malloc-storm section is allowed to differ (the old fork child may
 * deadlock under it — that hazard is the point of the fix; run under a
 * timeout and report).
 *
 * Exit code: number of failed assertions (0 = pass).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

/* Copy of the helper signature in src/dyna-libc.c (DYN_SPAWN_EXTRACT
 * region). Returns 0 with *out set to the child pid, or the errno.
 * std_fds remap the child's 0/1/2 when != the slot number; cwd chdirs the
 * child; uid/gid (uid_t)-1 mean "leave alone"; use_path selects the
 * execvp-style PATH search. */
int os_exec_spawn_child(const char *file, char **argv, char **envp,
                        int std_fds[3], const char *cwd,
                        uid_t uid, gid_t gid, int use_path, pid_t *out);

static int n_pass, n_fail;

static void check(int cond, const char *name, const char *detail)
{
    if (cond) {
        n_pass++;
        printf("ok   %s\n", name);
    } else {
        n_fail++;
        printf("FAIL %s (%s)\n", name, detail ? detail : "");
    }
}

/* Translate a helper result the way js_os_exec does, so the SAME assertions
 * hold for the fork-based (old) and posix_spawn-based (new) implementations:
 *  - rc == EAGAIN/ENOMEM/EPERM/EINVAL: the process could not be created;
 *    the engine raises TypeError("fork error") — reported as -1000-rc;
 *  - rc != 0 otherwise: exec-class failure. The old child answered every
 *    such case with _exit(127) and the engine returns 127 in blocking mode;
 *    posix_spawn reports them synchronously as an errno instead.
 *  - rc == 0: reap the child; WEXITSTATUS, or -WTERMSIG. */
static int effective_exit(int rc, pid_t pid)
{
    int st;

    if (rc != 0) {
        if (rc == EAGAIN || rc == ENOMEM || rc == EPERM || rc == EINVAL)
            return -1000 - rc;
        return 127;
    }
    if (waitpid(pid, &st, 0) != pid)
        return -999;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -WTERMSIG(st);
}

/* Spawn with the child's stdout captured through the std_fds remap (what
 * os.exec's {stdout: fd} option exercises). Returns rc; on success reaps
 * the child, fills out[] with its stdout and *eff with its exit status. */
static int spawn_capture_uid(const char *file, char **argv, char **envp,
                             const char *cwd, int use_path,
                             uid_t uid, gid_t gid,
                             char *out, size_t out_sz, int *eff)
{
    int pfd[2];
    int std_fds[3] = { 0, 1, 2 };
    pid_t pid = 0;
    int rc;

    if (pipe(pfd) < 0)
        return -1001;
    std_fds[1] = pfd[1];
    rc = os_exec_spawn_child(file, argv, envp, std_fds, cwd, uid, gid,
                             use_path, &pid);
    close(pfd[1]);
    if (rc != 0) {
        close(pfd[0]);
        *eff = effective_exit(rc, pid);
        return rc;
    }
    out[0] = '\0';
    {
        ssize_t n = read(pfd[0], out, out_sz - 1);
        if (n > 0)
            out[n] = '\0';
    }
    close(pfd[0]);
    *eff = effective_exit(0, pid);
    return 0;
}

static int spawn_capture(const char *file, char **argv, char **envp,
                         const char *cwd, int use_path,
                         char *out, size_t out_sz, int *eff)
{
    return spawn_capture_uid(file, argv, envp, cwd, use_path,
                             (uid_t)-1, (gid_t)-1, out, out_sz, eff);
}

static char *v_sh[4];   /* {"sh", "-c", cmd, NULL} -- set per test */

static void set_sh_cmd(const char *cmd)
{
    v_sh[0] = (char *)"sh";
    v_sh[1] = (char *)"-c";
    v_sh[2] = (char *)cmd;
    v_sh[3] = NULL;
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "TIMEOUT: spawned child path hung "
                              "(malloc-storm deadlock?)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(98);
}

/* ---------------- malloc storm: the WHY of M35-01 ---------------- */

static atomic_int storm_stop;

static void *hammer_malloc(void *arg)
{
    (void)arg;
    while (!atomic_load(&storm_stop)) {
        size_t sz;
        for (sz = 16; sz <= (1u << 16); sz <<= 1) {
            char *p = malloc(sz);
            if (p) {
                p[0] = (char)sz;
                free(p);
            }
        }
    }
    return NULL;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 100 spawns while two threads hammer the allocator — the original bug: a
 * malloc lock held by another thread at fork() time deadlocks the forked
 * child before it can exec. With posix_spawn the child setup never
 * allocates against the parent's locks. */
static void test_malloc_storm(void)
{
    pthread_t th[2];
    int i, ok = 0;
    int64_t t0, t1;

    atomic_store(&storm_stop, 0);
    for (i = 0; i < 2; i++)
        pthread_create(&th[i], NULL, hammer_malloc, NULL);

    set_sh_cmd("exit 0");
    t0 = now_ms();
    for (i = 0; i < 100; i++) {
        int std_fds[3] = { 0, 1, 2 };
        pid_t pid = 0;
        int rc = os_exec_spawn_child("sh", v_sh, environ, std_fds, NULL,
                                     (uid_t)-1, (gid_t)-1, 1, &pid);
        if (effective_exit(rc, pid) == 0)
            ok++;
    }
    t1 = now_ms();

    atomic_store(&storm_stop, 1);
    for (i = 0; i < 2; i++)
        pthread_join(th[i], NULL);

    printf("     malloc-storm: %d/100 spawns reaped exit 0 in %lld ms "
           "under live malloc threads\n", ok, (long long)(t1 - t0));
    check(ok == 100, "malloc-storm: 100/100 spawns complete", "spawn deadlock");
}

int main(void)
{
    char out[512], expect[600];
    int eff = 0, rc;
    pid_t pid = 0;
    char tmpdir[] = "/tmp/dynaspawnXXXXXX";

    /* the storm rows must not silently hang forever */
    signal(SIGALRM, on_alarm);
    alarm(240);
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a crash */

    /* The PATH-search rows pin the old my_execvpe() contract: PATH comes
       from the CALLER's environ, not from the child envp. */
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("DYN_SPAWN_PARENT_ENV", "inherited", 1);

    /* 1. stdout capture through the std_fds remap (os.exec {stdout: fd}) */
    {
        char *av[3] = { (char *)"echo", (char *)"dyna", NULL };
        rc = spawn_capture("/bin/echo", av, environ, NULL, 0, out, sizeof out, &eff);
        snprintf(expect, sizeof expect, "dyna\n");
        check(rc == 0 && eff == 0 && strcmp(out, expect) == 0,
              "stdout dup2 file action captures child output", out);
    }

    /* 2. cwd (posix_spawn_file_actions_addchdir_np path) */
    if (mkdtemp(tmpdir) != NULL) {
        char real[PATH_MAX];
        char expect_real[PATH_MAX + 8];
        char *av[2] = { (char *)"pwd", NULL };
        rc = spawn_capture("/bin/pwd", av, environ, tmpdir, 0, out, sizeof out, &eff);
        /* /bin/pwd prints the physical dir; /tmp is a symlink on macOS */
        realpath(tmpdir, real);
        snprintf(expect_real, sizeof expect_real, "%s\n", real);
        check(rc == 0 && eff == 0 && strcmp(out, expect_real) == 0,
              "cwd file action chdirs the child", out);
        rmdir(tmpdir);
    } else {
        printf("skip cwd row (mkdtemp failed)\n");
    }

    /* 3. explicit envp: passed through, and REPLACES the environment */
    {
        char *env[2] = { (char *)"DYN_TEST_ENV=forty-two", NULL };
        set_sh_cmd("printf %s \"$DYN_TEST_ENV\"");
        rc = spawn_capture("sh", v_sh, env, NULL, 1, out, sizeof out, &eff);
        check(rc == 0 && eff == 0 && strcmp(out, "forty-two") == 0,
              "explicit envp is passed to the child", out);

        set_sh_cmd("if [ -n \"$DYN_SPAWN_PARENT_ENV\" ]; then exit 55; fi");
        rc = spawn_capture("sh", v_sh, env, NULL, 1, out, sizeof out, &eff);
        check(rc == 0 && eff == 0,
              "explicit envp replaces (not merges) the parent environment", out);
    }

    /* 4. inherited environ default (os.exec without env) */
    {
        set_sh_cmd("printf %s \"$DYN_SPAWN_PARENT_ENV\"");
        rc = spawn_capture("sh", v_sh, environ, NULL, 1, out, sizeof out, &eff);
        check(rc == 0 && eff == 0 && strcmp(out, "inherited") == 0,
              "environ inheritance when envp == environ", out);
    }

    /* 5. PATH search (posix_spawnp, name without slash) */
    {
        set_sh_cmd("exit 3");
        rc = spawn_capture("sh", v_sh, environ, NULL, 1, out, sizeof out, &eff);
        check(rc == 0 && eff == 3, "use_path resolves via PATH (spawnp)", out);
    }

    /* 6. PATH comes from the caller's environ, not from envp (old
          my_execvpe used getenv("PATH"); probed spawnp does the same) */
    {
        char *env[2] = { (char *)"PATH=/nonexistent-dir", NULL };
        set_sh_cmd("exit 7");
        rc = spawn_capture("sh", v_sh, env, NULL, 1, out, sizeof out, &eff);
        check(rc == 0 && eff == 7,
              "PATH search uses caller environ, not child envp", out);
    }

    /* 7. nonexistent binary, direct form -> 127 (old child _exit(127)) */
    {
        char *av[2] = { (char *)"/no/such/dynajs-binary", NULL };
        rc = spawn_capture("/no/such/dynajs-binary", av, environ, NULL, 0,
                           out, sizeof out, &eff);
        check(eff == 127, "nonexistent binary (direct) -> 127", out);
    }

    /* 8. nonexistent binary, PATH form -> 127 */
    {
        char *av[2] = { (char *)"dynajs-no-such-binary", NULL };
        rc = spawn_capture("dynajs-no-such-binary", av, environ, NULL, 1,
                           out, sizeof out, &eff);
        check(eff == 127, "nonexistent binary (PATH) -> 127", out);
    }

    /* 9. existing but non-executable file -> 127 (old _exit(127), new
          synchronous EACCES mapped to 127) */
    if (access("/etc/hosts", F_OK) == 0) {
        char *av[2] = { (char *)"/etc/hosts", NULL };
        rc = spawn_capture("/etc/hosts", av, environ, NULL, 0, out, sizeof out, &eff);
        check(eff == 127, "non-executable file -> 127", out);
    } else {
        printf("skip non-executable row (/etc/hosts missing)\n");
    }

    /* 10. invalid std fd -> 127. The probed libcs silently IGNORE a failing
           dup2 file action, so the new code pre-validates; the old child
           died 127 at the dup2. */
    {
        int std_fds[3] = { -1, 1, 2 };
        set_sh_cmd("echo SPAWNED");
        rc = os_exec_spawn_child("sh", v_sh, environ, std_fds, NULL,
                                 (uid_t)-1, (gid_t)-1, 1, &pid);
        check(effective_exit(rc, pid) == 127, "invalid stdin fd -> 127", out);

        /* once-valid-but-closed fd must behave the same */
        {
            int pfd[2];
            if (pipe(pfd) == 0) {
                close(pfd[0]);
                std_fds[0] = pfd[0];
                rc = os_exec_spawn_child("sh", v_sh, environ, std_fds, NULL,
                                         (uid_t)-1, (gid_t)-1, 1, &pid);
                check(effective_exit(rc, pid) == 127,
                      "closed stdin fd -> 127", out);
            }
        }
    }

    /* 11. non-stdio fds are closed in the child (dup2 first, then the
           closefrom/close pass — order pinned) */
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            char cmd[128];
            /* the pipe write end is an fd >= 3 the child must NOT see;
               /dev/fd/1 is a live control (the remapped stdout) */
            snprintf(cmd, sizeof cmd,
                     "if [ -e /dev/fd/1 ] && [ ! -e /dev/fd/%d ]; then exit 0; "
                     "else exit 43; fi", pfd[1]);
            set_sh_cmd(cmd);
            rc = spawn_capture("sh", v_sh, environ, NULL, 1,
                               out, sizeof out, &eff);
            check(rc == 0 && eff == 0,
                  "extra fds are closed in the child (dup2 survives)", out);
            close(pfd[0]);
            close(pfd[1]);
        }
    }

    /* 12. uid/gid. None of glibc/musl/macOS has POSIX_SPAWN_SETUID/SETGID,
            so this takes the retained fork child on all of them; as root it
            must drop to the requested id (verified through `id`), as
            non-root setuid fails and the old observable was _exit(127). */
    {
        set_sh_cmd("id -u");
        rc = spawn_capture_uid("sh", v_sh, environ, NULL, 1,
                               (uid_t)65534, (gid_t)-1, out, sizeof out, &eff);
        if (geteuid() == 0)
            check(rc == 0 && eff == 0 && strncmp(out, "65534", 5) == 0,
                  "setuid drops to 65534 (root)", out);
        else
            check(eff == 127, "setuid as non-root fails -> 127", out);

        set_sh_cmd("id -g");
        rc = spawn_capture_uid("sh", v_sh, environ, NULL, 1,
                               (uid_t)-1, (gid_t)65534, out, sizeof out, &eff);
        if (geteuid() == 0)
            check(rc == 0 && eff == 0 && strncmp(out, "65534", 5) == 0,
                  "setgid drops to 65534 (root)", out);
        else
            check(eff == 127, "setgid as non-root fails -> 127", out);
    }

    /* 13. signal observable: un-reaped spawn + SIGTERM (mirrors the
            os.exec(["cat"], {block:false}) + os.kill row in test_std.js) */
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            int std_fds[3] = { pfd[0], 1, 2 };
            char *av[2] = { (char *)"cat", NULL };
            rc = os_exec_spawn_child("cat", av, environ, std_fds, NULL,
                                     (uid_t)-1, (gid_t)-1, 1, &pid);
            if (rc == 0) {
                int st = 0;
                kill(pid, SIGTERM);
                waitpid(pid, &st, 0);
                check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
                      "SIGTERM to spawned child surfaces as -SIGTERM", NULL);
            } else {
                check(0, "spawn cat for signal row", strerror(rc));
            }
            close(pfd[0]);
            close(pfd[1]);
        }
    }

    /* 14. the reason for the fix */
    test_malloc_storm();

    printf("%s: %d passed, %d failed\n",
           n_fail ? "FAILURES" : "ALL OK", n_pass, n_fail);
    return n_fail;
}
