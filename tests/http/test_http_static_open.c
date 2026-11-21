/* test_http_static_open: dyn_static_open (src/modules/http/http-static-open.c), the App.static
 * file open: realpath of root and target, a containment check, then one open(O_NOFOLLOW). No openat
 * anywhere. Symlinks that stay inside the root are served, symlinks that leave
 * it are 403, dangling links and loops are 404 or 403. Root revalidation: the root is reopened by path
 * on every open, so a root renamed away and replaced serves the NEW directory, a removed root serves 404.
 * Build+run: tests/ctest/run.sh test-http-static-open. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http-static-open.h"



static int fails, checks;
static char base[PATH_MAX], root[PATH_MAX], outside[PATH_MAX];

static void put(const char* dir, const char* name, const char* body)
{
    char p[PATH_MAX];
    int fd;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, body, strlen(body)) != (ssize_t)strlen(body)) {
        perror(p);
        exit(2);
    }
    close(fd);
}

static void sub(const char* dir, const char* name)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (mkdir(p, 0755) < 0) {
        perror(p);
        exit(2);
    }
}

static void lnk(const char* target, const char* dir, const char* name)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (symlink(target, p) < 0) {
        perror(p);
        exit(2);
    }
}

static void outcome(dyn_static_root_t* r, const char* rel, char* out, size_t n)
{
    int st = 0, fd;
    fd = dyn_static_open(r, rel, &st);
    if (fd < 0) {
        snprintf(out, n, "%d", st);
        return;
    }
    {
        char buf[64];
        struct stat sb;
        ssize_t k;
        if (fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            snprintf(out, n, "dir");
        } else {
            k = read(fd, buf, sizeof buf - 1);
            buf[k < 0 ? 0 : k] = 0;
            snprintf(out, n, "ok:%s", buf);
        }
    }
    close(fd);
}

static void expect(dyn_static_root_t* r, const char* rel, const char* want)
{
    char got[96];
    outcome(r, rel, got, sizeof got);
    checks++;
    if (strcmp(got, want) != 0) {
        fails++;
        printf("FAIL %s: got %s want %s\n", rel, got, want);
    }
}

int main(void)
{
    char tmpl[PATH_MAX], gone[PATH_MAX], abs[PATH_MAX], other[PATH_MAX];
    dyn_static_root_t* r;
    const char* tmpdir = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/dyn_static_open_XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    if (!mkdtemp(tmpl) || !realpath(tmpl, base)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(root, sizeof root, "%s/root", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    sub(base, "root");
    sub(base, "outside");
    sub(root, "sub");
    put(root, "f.txt", "inside-file");
    put(root, "sub/deep.txt", "deep-file");
    put(outside, "secret.txt", "TOP-SECRET");
    snprintf(abs, sizeof abs, "%s/secret.txt", outside);
    lnk(abs, root, "abs_out.txt");
    lnk("../outside/secret.txt", root, "rel_out.txt");
    lnk(outside, root, "abs_outdir");
    lnk("../outside", root, "rel_outdir");
    snprintf(abs, sizeof abs, "%s/f.txt", root);
    lnk(abs, root, "abs_in.txt");
    lnk("f.txt", root, "rel_in.txt");
    lnk("sub", root, "subalias");
    snprintf(abs, sizeof abs, "%s/sub", root);
    lnk(abs, root, "abs_subalias");
    lnk("../f.txt", root, "sub/up_in.txt");
    lnk("../../outside/secret.txt", root, "sub/up_out.txt");
    lnk("nowhere.txt", root, "dangling.txt");
    lnk("loop2", root, "loop1");
    lnk("loop1", root, "loop2");

    r = dyn_static_root_new(root);
    if (!r)
        return 2;
    expect(r, "f.txt", "ok:inside-file");
    expect(r, "sub/deep.txt", "ok:deep-file");
    expect(r, "sub", "dir");
    expect(r, "nope.txt", "404");
    expect(r, "sub/nope.txt", "404");
    expect(r, "f.txt/x", "404");
    expect(r, "abs_out.txt", "403");
    expect(r, "rel_out.txt", "403");
    expect(r, "abs_outdir/secret.txt", "403");
    expect(r, "rel_outdir/secret.txt", "403");
    expect(r, "abs_outdir", "403");
    expect(r, "sub/up_out.txt", "403");
    expect(r, "abs_in.txt", "ok:inside-file");
    expect(r, "rel_in.txt", "ok:inside-file");
    expect(r, "subalias/deep.txt", "ok:deep-file");
    expect(r, "sub/up_in.txt", "ok:inside-file");
    expect(r, "dangling.txt", "404");
    expect(r, "subalias/nope.txt", "404");
    expect(r, "abs_subalias/nope.txt", "404");
    expect(r, "abs_outdir/nope.txt", "404");
    expect(r, "rel_outdir/nope.txt", "404");
    expect(r, "dangling.txt/x", "404");
    expect(r, "loop1", "404");
    expect(r, "../outside/secret.txt", "403");
    expect(r, "sub/../f.txt", "ok:inside-file");


    snprintf(gone, sizeof gone, "%s/root.old", base);
    expect(r, "f.txt", "ok:inside-file");
    if (rename(root, gone) < 0) {
        perror("rename");
        return 2;
    }
    expect(r, "f.txt", "404");
    sub(base, "root");
    put(root, "f.txt", "replacement");
    expect(r, "f.txt", "ok:replacement");
    expect(r, "sub/deep.txt", "404");
    snprintf(other, sizeof other, "%s/root.new", base);
    rename(root, other);
    rename(gone, root);
    expect(r, "f.txt", "ok:inside-file");
    expect(r, "abs_in.txt", "ok:inside-file");
    rename(root, gone);
    lnk("root.new", base, "root");
    expect(r, "f.txt", "ok:replacement");
    expect(r, "abs_out.txt", "404");
    dyn_static_root_free(r);

    {
        char cmd[PATH_MAX + 16];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
        if (system(cmd) != 0)
            printf("warn: cleanup of %s failed\n", base);
    }
    printf("test_http_static_open: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
