/* tests/sys/test_sys_raw_mode_sigterm.c -- F33 follow-up: raw mode and the child registry stack their
 * SIGTERM/SIGHUP handlers. The first Spawn installs the child-registry handler (src/core/dyn-childreg.c:
 * TERM the child groups, KILL after 2 s, re-raise); dyna:cli raw mode (keypress/select, src/modules/term/
 * term-raw-mode.c) then hooks over it, treating the registry handler like the default disposition. On
 * SIGTERM while raw mode is on, the raw handler must restore the terminal, take the child groups down
 * and chain to the registry handler, and the process must still die BY SIGTERM.
 *
 * Runs DYNAJS (argv[1], default ./dynajs) on a pty (forkpty) with a fixture that Spawns
 * `sh -c "sleep 30 & echo $$; exec sleep 30"` (so the group has two members), prints the group id, then
 * blocks in keypress() (raw mode). The parent waits until the pty leaves canonical mode, sends SIGTERM,
 * and checks: the process died by SIGTERM; the pty is back in ICANON|ECHO|ISIG mode; the child group is
 * gone within 3 s. Mutants: raw mode refusing to stack over the registry handler (terminal stays raw),
 * and the raw handler dropping both the kill and the chain (the group survives). Exit 77 = no pty here. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static const char fixture[] = "import { Spawn } from \"dyna:sys\";\n"
                              "import { keypress } from \"dyna:cli\";\n"
                              "import * as std from \"std\";\n"
                              "const p = new Spawn(\"sh\", [\"-c\", \"sleep 30 & echo $$; exec sleep 30\"], { stdin: \"ignore\" });\n"
                              "const b = new Uint8Array(32);\n"
                              "const n = await p.stdout.read(b);\n"
                              "std.out.puts(\"PG \" + new TextDecoder().decode(b.subarray(0, n)).trim() + \"\\n\");\n"
                              "std.out.flush();\n"
                              "keypress();\n"
                              "std.out.puts(\"NOT-REACHED\\n\");\n";

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int group_alive(int pg)
{
    return kill(-pg, 0) == 0 || errno == EPERM;
}

static int read_until(int fd, char* buf, size_t cap, const char* want, int ms)
{
    size_t len = strlen(buf);
    long end = now_ms() + ms;
    while (now_ms() < end) {
        fd_set r;
        struct timeval tv = { 0, 50000 };
        ssize_t k;
        if (strstr(buf, want) && strchr(strstr(buf, want), '\n'))
            return 1;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0)
            continue;
        k = read(fd, buf + len, cap - 1 - len);
        if (k <= 0)
            return 0;
        len += (size_t)k;
        buf[len] = 0;
    }
    return strstr(buf, want) && strchr(strstr(buf, want), '\n');
}

int main(int argc, char** argv)
{
    const char* bin = argc > 1 ? argv[1] : "./dynajs";
    const char* tmp = getenv("TMPDIR");
    char path[1024], out[4096] = { 0 };
    struct termios t;
    int fd, master, st, pg = 0, fails = 0, raw = 0;
    long end;
    pid_t pid;
    FILE* f;

    snprintf(path, sizeof path, "%s/dj_rawterm_XXXXXX.js", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemps(path, 3);
    if (fd < 0 || !(f = fdopen(fd, "w"))) {
        perror("fixture");
        return 1;
    }
    fputs(fixture, f);
    fclose(f);

    pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        perror("forkpty");
        unlink(path);
        return 77;
    }
    if (pid == 0) {
        execl(bin, bin, "--std", path, (char*)NULL);
        _exit(127);
    }

    if (!read_until(master, out, sizeof out, "PG ", 10000) || sscanf(strstr(out, "PG ") + 3, "%d", &pg) != 1 || pg <= 0) {
        printf("FAIL: the fixture never reported its child group (output: %s)\n", out);
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        unlink(path);
        return 1;
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    for (end = now_ms() + 5000; now_ms() < end; nap(10))
        if (tcgetattr(master, &t) == 0 && !(t.c_lflag & ICANON)) {
            raw = 1;
            break;
        }
    if (!raw) {
        printf("FAIL: the pty never left canonical mode (keypress did not enter raw mode)\n");
        fails++;
    }
    if (!group_alive(pg)) {
        printf("FAIL: child group %d is not alive before SIGTERM\n", pg);
        fails++;
    }
    nap(50);
    kill(pid, SIGTERM);
    end = now_ms() + 8000;
    while (waitpid(pid, &st, WNOHANG) == 0) {
        char drain[256];
        if (now_ms() > end) {
            printf("FAIL: the process survived SIGTERM for 8 s\n");
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            fails++;
            break;
        }
        while (read(master, drain, sizeof drain) > 0) {
        }
        nap(10);
    }
    if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM)) {
        printf("FAIL: the process did not die by SIGTERM (status 0x%x)\n", st);
        fails++;
    }
    if (tcgetattr(master, &t) != 0) {
        printf("FAIL: tcgetattr on the pty master: %s\n", strerror(errno));
        fails++;
    } else if ((t.c_lflag & (ICANON | ECHO | ISIG)) != (ICANON | ECHO | ISIG)) {
        printf("FAIL: the terminal was left in raw mode (lflag 0x%lx)\n", (unsigned long)t.c_lflag);
        fails++;
    }
    for (end = now_ms() + 3000; group_alive(pg) && now_ms() < end; nap(25)) {
    }
    if (group_alive(pg)) {
        printf("FAIL: child group %d outlived the SIGTERM'd parent\n", pg);
        kill(-pg, SIGKILL);
        fails++;
    }
    close(master);
    unlink(path);
    printf("test_sys_raw_mode_sigterm: %s (group %d)\n", fails ? "FAILED" : "ok", pg);
    return fails ? 1 : 0;
}
