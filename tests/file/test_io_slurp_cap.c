#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "dyna-io.h"

static int fails;
static char dir[256];

static const char* P(const char* name)
{
    static char b[4][320];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof b[k], "%s/%s", dir, name);
    return b[k];
}

static void write_fill(const char* path, size_t n, int c)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t off = 0;
    if (fd < 0) {
        printf("FAIL: cannot create %s\n", path);
        fails++;
        return;
    }
    while (off < n) {
        char buf[4096];
        size_t chunk = n - off < sizeof(buf) ? n - off : sizeof(buf);
        memset(buf, c, chunk);
        if (write(fd, buf, chunk) != (ssize_t)chunk)
            break;
        off += chunk;
    }
    close(fd);
}

static void slurp_file(const char* path, char** data, size_t* len, int* err)
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
    const char* base = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/io_slurp.XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    {
        int fd = open(P("sparse"), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0 || ftruncate(fd, (off_t)2 << 30) != 0) {
            printf("FAIL: cannot create the 2 GiB sparse file the over-cap case needs\n");
            fails++;
        }
        if (fd >= 0)
            close(fd);
    }
    {
        dyn_iobuf_t out;
        int e;
        e = dyn_io_slurp(P("sparse"), &out, 0, DYN_MAX_INPUT);
        printf("(b) over-cap sparse: rc=%d errno=%d (%s)\n", e, e ? errno : 0, e ? strerror(errno) : "");
        if (e == 0 || errno != EFBIG) {
            printf("FAIL: over-cap sparse not refused with EFBIG\n");
            fails++;
        }
        dyn_iobuf_free(&out);
    }

    {
        int fd = open(P("ok"), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0 || write(fd, "HELLO-SLURP", 11) != 11) {
            printf("FAIL: cannot create the normal-file fixture\n");
            fails++;
        }
        if (fd >= 0)
            close(fd);
    }
    {
        int e;
        char* d = NULL;
        size_t l = 0;
        slurp_file(P("ok"), &d, &l, &e);
        printf("(c) normal file: rc=%d len=%zu data=%.*s\n", e, l, (int)l, d ? d : "");
        if (e == 0 && l == strlen("HELLO-SLURP") && !memcmp(d, "HELLO-SLURP", l)) {
            printf("PASS (c)\n");
        } else {
            printf("FAIL (c)\n");
            fails++;
        }
        free(d);
    }

    write_fill(P("empty"), 0, 0);
    {
        dyn_iobuf_t out;
        int e;
        e = dyn_io_read_buf(P("empty"), &out, 0, DYN_MAX_INPUT);
        printf("(a) empty (st_size=0) read_buf: rc=%d len=%zu\n", e, dyn_iobuf_rlen(&out));
        if (e != 0 || dyn_iobuf_rlen(&out) != 0) {
            printf("FAIL (a): empty file\n");
            fails++;
        }
        dyn_iobuf_free(&out);
    }

    {
        long pg = sysconf(_SC_PAGESIZE);
        size_t aligned = pg > 0 ? (size_t)pg * 16 : 65536;
        dyn_iobuf_t out;
        int e, rc;

        write_fill(P("aligned"), aligned, 'A');
        write_fill(P("unaligned"), aligned + 3, 'B');

        e = dyn_io_slurp(P("aligned"), &out, DYN_SLURP_MMAP, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d1) aligned mmap ensure_nul: rc=%d errno=%d kind=%d\n",
            rc, rc < 0 ? errno : 0, (int)out.kind);
        if (e != 0 || out.kind != DYN_IOBUF_MMAP || rc != -1 || errno != ENOTSUP) {
            printf("FAIL (d1): page-aligned mmap must not claim a NUL byte\n");
            fails++;
        }
        dyn_iobuf_free(&out);

        e = dyn_io_slurp(P("unaligned"), &out, DYN_SLURP_NUL | DYN_SLURP_MMAP, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d2) partial-page mmap ensure_nul: rc=%d kind=%d tail=%d\n",
            rc, (int)out.kind, e == 0 ? out.data[out.len] : -1);
        if (e != 0 || out.kind != DYN_IOBUF_MMAP || rc != 0 || out.data[out.len] != 0) {
            printf("FAIL (d2): partial last page must keep its zero-fill NUL\n");
            fails++;
        }
        dyn_iobuf_free(&out);

        e = dyn_io_slurp(P("aligned"), &out, DYN_SLURP_NUL, DYN_MAX_INPUT);
        rc = e == 0 ? dyn_iobuf_ensure_nul(&out) : -2;
        printf("(d3) aligned heap ensure_nul: rc=%d kind=%d tail=%d\n",
            rc, (int)out.kind, e == 0 ? out.data[out.len] : -1);
        if (e != 0 || out.kind != DYN_IOBUF_HEAP || rc != 0 || out.data[out.len] != 0) {
            printf("FAIL (d3): NUL slurp of an aligned file must land on the heap\n");
            fails++;
        }
        dyn_iobuf_free(&out);
    }
    {
        static const char* const names[] = { "sparse", "ok", "empty", "aligned", "unaligned" };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            unlink(P(names[i]));
        rmdir(dir);
    }
    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "all ok", fails);
    return fails != 0;
}
