/* test_io_atomic_write.c -- dyn_io_write_whole_atomic must not follow a symlink.
 *
 * The temp name is `<path>.dynajs.tmp.<pid>`, fully predictable, and the open
 * used O_CREAT|O_TRUNC with no O_EXCL: a symlink planted next to the target was
 * followed and written through -- an arbitrary file write from any code path
 * that saves a file (dyna:csv reaches it). Verified failing pre-fix.
 *
 * Case (c) exists because O_EXCL is the kind of fix that breaks robustness: a
 * stale temp from a crashed writer must be stepped over, not turned into a
 * permanent failure.
 *
 *   clang -g -O1 -fsanitize=address,undefined -Isrc -o /tmp/t \
 *       tests/test_io_atomic_write.c src/dyna-io.c src/cutils.c && /tmp/t
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "dyna-io.h"
static char *slurp(const char *p, char *b, size_t n) {
    int fd = open(p, O_RDONLY); ssize_t r;
    if (fd < 0) { strcpy(b, "<none>"); return b; }
    r = read(fd, b, n-1); close(fd); b[r > 0 ? r : 0] = 0; return b;
}
int main(void) {
    char buf[256]; int rc, bad = 0;
    system("rm -rf /tmp/awc && mkdir -p /tmp/awc && printf VICTIM > /tmp/awc/victim");
    /* (a) plain write */
    rc = dyn_io_write_whole_atomic("/tmp/awc/f.txt", "HELLO", 5, 0);
    printf("write rc=%d content=%s\n", rc, slurp("/tmp/awc/f.txt", buf, sizeof buf));
    if (rc != 0 || strcmp(buf, "HELLO")) { printf("FAIL: plain write\n"); bad++; }
    /* (b) planted symlink at the exact temp name */
    { char t[256]; snprintf(t, sizeof t, "/tmp/awc/g.txt.dynajs.tmp.%ld", (long)getpid());
      if (symlink("/tmp/awc/victim", t) != 0) printf("(symlink failed)\n"); }
    rc = dyn_io_write_whole_atomic("/tmp/awc/g.txt", "PWNED", 5, 0);
    { char v[256], g[256];
      slurp("/tmp/awc/victim", v, sizeof v); slurp("/tmp/awc/g.txt", g, sizeof g);
      printf("symlink-case rc=%d victim=%s g.txt=%s\n", rc, v, g); }
    slurp("/tmp/awc/victim", buf, sizeof buf);
    if (strcmp(buf, "VICTIM")) { printf("FAIL: wrote through the symlink\n"); bad++; }
    /* (c) stale temp must not block */
    { char t[256]; snprintf(t, sizeof t, "/tmp/awc/h.txt.dynajs.tmp.%ld", (long)getpid());
      int fd = open(t, O_WRONLY|O_CREAT|O_TRUNC, 0600); if (fd >= 0) { write(fd, "stale", 5); close(fd); } }
    rc = dyn_io_write_whole_atomic("/tmp/awc/h.txt", "FRESH", 5, 0);
    printf("stale-temp rc=%d content=%s\n", rc, slurp("/tmp/awc/h.txt", buf, sizeof buf));
    if (rc != 0 || strcmp(buf, "FRESH")) { printf("FAIL: stale temp blocked the write\n"); bad++; }
    printf(bad ? "RESULT: %d failures\n" : "RESULT: all ok\n", bad);
    return bad != 0;
}
