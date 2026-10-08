#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "dyna-io.h"

static int fails;

static void write_fill(const char *path, size_t n, int c)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t off = 0;
    if (fd < 0) { printf("FAIL: cannot create %s\n", path); fails++; return; }
    while (off < n) {
        char buf[4096];
        size_t chunk = n - off < sizeof(buf) ? n - off : sizeof(buf);
        memset(buf, c, chunk);
        if (write(fd, buf, chunk) != (ssize_t)chunk) break;
        off += chunk;
    }
    close(fd);
}

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

    {
        long pg = sysconf(_SC_PAGESIZE);
        size_t aligned = pg > 0 ? (size_t)pg * 16 : 65536;
        dyn_iobuf_t out;
        int e, rc;

        write_fill("probe_io_nul_aligned.tmp", aligned, 'A');
        write_fill("probe_io_nul_unaligned.tmp", aligned + 3, 'B');

        e = dyn_io_slurp("probe_io_nul_aligned.tmp", &out, DYN_SLURP_MMAP, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d1) aligned mmap ensure_nul: rc=%d errno=%d kind=%d\n",
            rc, rc < 0 ? errno : 0, (int)out.kind);
        if (e != 0 || out.kind != DYN_IOBUF_MMAP || rc != -1 || errno != ENOTSUP) {
            printf("FAIL (d1): page-aligned mmap must not claim a NUL byte\n");
            fails++;
        }
        dyn_iobuf_free(&out);

        e = dyn_io_slurp("probe_io_nul_unaligned.tmp", &out, DYN_SLURP_NUL | DYN_SLURP_MMAP, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d2) partial-page mmap ensure_nul: rc=%d kind=%d tail=%d\n",
            rc, (int)out.kind, e == 0 ? out.data[out.len] : -1);
        if (e != 0 || out.kind != DYN_IOBUF_MMAP || rc != 0 || out.data[out.len] != 0) {
            printf("FAIL (d2): partial last page must keep its zero-fill NUL\n");
            fails++;
        }
        dyn_iobuf_free(&out);

        e = dyn_io_slurp("probe_io_nul_aligned.tmp", &out, DYN_SLURP_NUL, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d3) aligned heap ensure_nul: rc=%d kind=%d tail=%d\n",
            rc, (int)out.kind, e == 0 ? out.data[out.len] : -1);
        if (e != 0 || out.kind != DYN_IOBUF_HEAP || rc != 0 || out.data[out.len] != 0) {
            printf("FAIL (d3): NUL slurp of an aligned file must land on the heap\n");
            fails++;
        }
        dyn_iobuf_free(&out);
    }
    system("rm -f probe_io_nul_aligned.tmp probe_io_nul_unaligned.tmp");

    system("rm -f /tmp/io_slurp_sparse /tmp/io_slurp_ok /tmp/io_slurp_empty");
    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "all ok", fails);
    return fails != 0;
}
