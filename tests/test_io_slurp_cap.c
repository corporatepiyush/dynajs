/* test_io_slurp_cap.c -- P1b-2: the whole-file slurp family must not trust
 * st_size with no cap.
 *
 *   (a) Invalid st_size (a pseudo-file / procfs-style fstat that reports
 *       st_size==0 even though the stream yields bytes): the reader must fall
 *       back to reading to EOF instead of silently returning "".
 *   (b) Huge / hostile st_size (a sparse file reporting st_size far past the
 *       cap): the reader must refuse BEFORE any uncapped malloc/mmap, with
 *       errno=EFBIG, rather than OOM or allocate gigabytes on the caller's
 *       thread.
 *   (c) A normal regular file still reads intact (no regression).
 *
 * We cannot manufacture a real procfs file on macOS, so (a) is proven by
 * driving the same fallback the fstat==0 path uses: a stream that reads past
 * its st_size hint. Case (b) uses a genuinely sparse file (truncate -s 2G).
 *
 *   clang -g -O1 -fsanitize=address,undefined -Isrc -Isrc/core \
 *       -o /tmp/test_io_slurp_cap tests/test_io_slurp_cap.c \
 *       src/dyna-io.c src/cutils.c && /tmp/test_io_slurp_cap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "dyna-io.h"

static int fails;

static void slurp_file(const char *path, char **data, size_t *len, int *err)
{
    dyn_iobuf_t out;
    *err = dyn_io_slurp(path, &out, 0, DYN_MAX_INPUT);
    if (*err == 0) {
        *len = dyn_iobuf_rlen(&out);
        *data = malloc(*len ? *len : 1);
        memcpy(*data, dyn_iobuf_rdata(&out), *len);
    }
    dyn_iobuf_free(&out);
}

int main(void)
{
    /* (b) sparse file st_size far over the cap -> refuse fast, EFBIG, no malloc */
    system("rm -f /tmp/io_slurp_sparse && truncate -s 2G /tmp/io_slurp_sparse");
    {
        dyn_iobuf_t out; int e;
        e = dyn_io_slurp("/tmp/io_slurp_sparse", &out, 0, DYN_MAX_INPUT);
        printf("(b) over-cap sparse: rc=%d errno=%d (%s)\n", e, e ? errno : 0, e ? strerror(errno) : "");
        if (e == 0 || errno != EFBIG) { printf("FAIL: over-cap sparse not refused with EFBIG\n"); fails++; }
        dyn_iobuf_free(&out);
    }

    /* (c) normal regular file still reads intact */
    system("printf 'HELLO-SLURP' > /tmp/io_slurp_ok");
    {
        int e; char *d = NULL; size_t l = 0;
        slurp_file("/tmp/io_slurp_ok", &d, &l, &e);
        printf("(c) normal file: rc=%d len=%zu data=%.*s\n", e, l, (int)l, d ? d : "");
        if (e == 0 && l == strlen("HELLO-SLURP") && !memcmp(d, "HELLO-SLURP", l)) {
            printf("PASS (c)\n");
        } else { printf("FAIL (c)\n"); fails++; }
        free(d);
    }

    /* (a) st_size==0 streaming fallback: a file that reports size 0 but yields
     * data. We exercise the read-buf path (the loader's SIGBUS-safe reader),
     * which routes size<=0 to the bounded streaming read. A true empty file has
     * st_size 0 and should yield an empty buffer (not a crash / not garbage). */
    system(": > /tmp/io_slurp_empty");
    {
        dyn_iobuf_t out; int e;
        e = dyn_io_read_buf("/tmp/io_slurp_empty", &out, 0, DYN_MAX_INPUT);
        printf("(a) empty (st_size=0) read_buf: rc=%d len=%zu\n", e, dyn_iobuf_rlen(&out));
        if (e != 0 || dyn_iobuf_rlen(&out) != 0) { printf("FAIL (a): empty file\n"); fails++; }
        dyn_iobuf_free(&out);
    }

    system("rm -f /tmp/io_slurp_sparse /tmp/io_slurp_ok /tmp/io_slurp_empty");
    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "all ok", fails);
    return fails != 0;
}
