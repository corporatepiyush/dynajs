#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "dyna-io.h"
static char dir[256];
static const char* P(char* b, size_t n, const char* name)
{
    snprintf(b, n, "%s/%s", dir, name);
    return b;
}
static char* slurp(const char* p, char* b, size_t n)
{
    int fd = open(p, O_RDONLY);
    ssize_t r;
    if (fd < 0) {
        strcpy(b, "<none>");
        return b;
    }
    r = read(fd, b, n - 1);
    close(fd);
    b[r > 0 ? r : 0] = 0;
    return b;
}
static void put(const char* p, const char* s)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (write(fd, s, strlen(s)) < 0)
            perror("write");
        close(fd);
    }
}
int main(void)
{
    char buf[256], p1[300], p2[300], t[300];
    int rc, bad = 0;
    const char* base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/awc.XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    put(P(p1, sizeof p1, "victim"), "VICTIM");
    rc = dyn_io_write_whole_atomic(P(p1, sizeof p1, "f.txt"), "HELLO", 5, 0);
    printf("write rc=%d content=%s\n", rc, slurp(p1, buf, sizeof buf));
    if (rc != 0 || strcmp(buf, "HELLO")) {
        printf("FAIL: plain write\n");
        bad++;
    }
    snprintf(t, sizeof t, "%s/g.txt.dynajs.tmp.%ld", dir, (long)getpid());
    if (symlink(P(p1, sizeof p1, "victim"), t) != 0) {
        printf("FAIL: could not plant the symlink, so the symlink case would prove nothing\n");
        bad++;
    }
    rc = dyn_io_write_whole_atomic(P(p2, sizeof p2, "g.txt"), "PWNED", 5, 0);
    {
        char v[256], g[256];
        slurp(P(p1, sizeof p1, "victim"), v, sizeof v);
        slurp(p2, g, sizeof g);
        printf("symlink-case rc=%d victim=%s g.txt=%s\n", rc, v, g);
    }
    slurp(P(p1, sizeof p1, "victim"), buf, sizeof buf);
    if (strcmp(buf, "VICTIM")) {
        printf("FAIL: wrote through the symlink\n");
        bad++;
    }
    snprintf(t, sizeof t, "%s/h.txt.dynajs.tmp.%ld", dir, (long)getpid());
    put(t, "stale");
    rc = dyn_io_write_whole_atomic(P(p1, sizeof p1, "h.txt"), "FRESH", 5, 0);
    printf("stale-temp rc=%d content=%s\n", rc, slurp(p1, buf, sizeof buf));
    if (rc != 0 || strcmp(buf, "FRESH")) {
        printf("FAIL: stale temp blocked the write\n");
        bad++;
    }
    snprintf(t, sizeof t, "rm -rf '%s'", dir);
    if (system(t) != 0)
        printf("(could not remove %s)\n", dir);
    printf(bad ? "RESULT: %d failures\n" : "RESULT: all ok\n", bad);
    return bad != 0;
}
