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
    system("rm -f /tmp/io_slurp_sparse && truncate -s 2G /tmp/io_slurp_sparse");
    {
        dyn_iobuf_t out; int e;
        e = dyn_io_slurp("/tmp/io_slurp_sparse", &out, 0, DYN_MAX_INPUT);
        printf("(b) over-cap sparse: rc=%d errno=%d (%s)\n", e, e ? errno : 0, e ? strerror(errno) : "");
        if (e == 0 || errno != EFBIG) { printf("FAIL: over-cap sparse not refused with EFBIG\n"); fails++; }
        dyn_iobuf_free(&out);
    }

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
