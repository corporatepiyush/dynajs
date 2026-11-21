/*
 * test_fsops_core.c -- src/core/fsops (the dyna:file filesystem primitives) as
 * a standalone C library: no engine.
 *
 * Oracles from outside this code:
 *  - dyn_glob_match against the platform's POSIX fnmatch(3) with flags 0 (no
 *    FNM_PATHNAME, no FNM_PERIOD: '*' and '?' cross '/', no leading-dot rule,
 *    which is the matcher's documented lexical contract), exhaustively over
 *    every pattern of up to 4 symbols from { a b * ? [ ] ! - / } and every
 *    subject of up to 4 bytes from { a b - / [ ] }. Backslash is left out on
 *    purpose: fnmatch treats it as an escape and the matcher does not.
 *    Patterns with an UNCLOSED '[' are excluded from the differential: POSIX
 *    XCU 2.13.1 says such a '[' matches itself, glibc's fnmatch does so, and
 *    macOS fnmatch(3) instead fails the match (32,808 such cases measured).
 *    The POSIX rule is pinned separately with hand-picked cases. Ranges need
 *    five symbols, past the exhaustive length, so every ASCII range [x-y]
 *    and [!x-y] is checked against fnmatch over every byte 0x01..0x7f.
 *  - Magic numbers from the formats' own specifications: PNG (ISO/IEC 15948
 *    5.2), GIF89a, PDF ("%PDF"), gzip (RFC 1952 2.3.1), ZIP local header
 *    (APPNOTE 4.3.7), ELF, WebAssembly ("\0asm"), SQLite ("SQLite format 3\0",
 *    file format 1.3), POSIX ustar ("ustar" at offset 257).
 *  - POSIX errno names.
 *  - flock(2): a second open file description on the same file cannot take an
 *    exclusive lock while the first holds it.
 * Properties (pinned as properties, not digits): mkdirp builds a deep chain
 * and is idempotent; remove_tree removes it, accepts a missing path and a
 * plain file; the NUL scan in sniff stops at 512 bytes (cases at 511/512);
 * copy_fds copies a regular file longer than the 256 KiB copy buffer
 * byte-exactly (a pipe source was never supported and still is not);
 * the temp-prefix rule refuses 201 bytes and accepts 200.
 */
#include "dyn-fsops.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static long glob_cases, glob_diffs, glob_unclosed;

static int brackets_closed(const char* p)
{
    while (*p) {
        if (*p == '[') {
            const char* q = p + 1;
            if (*q == '!' || *q == '^')
                q++;
            if (*q == ']')
                q++;
            while (*q && *q != ']')
                q++;
            if (!*q)
                return 0;
            p = q + 1;
        } else {
            p++;
        }
    }
    return 1;
}

static void glob_pat(char* pat, int plen, int depth)
{
    static const char ps[] = "ab*?[]!-/";
    static const char ss[] = "ab-/[]";
    if (depth == plen) {
        char s[8];
        int slen, k, idx[4];
        pat[plen] = 0;
        if (!brackets_closed(pat)) {
            glob_unclosed++;
            return;
        }
        for (slen = 0; slen <= 4; slen++) {
            long total = 1, t;
            for (k = 0; k < slen; k++)
                total *= (long)(sizeof ss - 1);
            for (t = 0; t < total; t++) {
                long r = t;
                for (k = 0; k < slen; k++) {
                    idx[k] = (int)(r % (long)(sizeof ss - 1));
                    r /= (long)(sizeof ss - 1);
                    s[k] = ss[idx[k]];
                }
                s[slen] = 0;
                glob_cases++;
                if ((dyn_glob_match(pat, s) != 0) != (fnmatch(pat, s, 0) == 0)) {
                    if (glob_diffs++ < 5)
                        printf("FAIL glob: pattern \"%s\" subject \"%s\": ours %d, fnmatch %d\n", pat, s,
                            dyn_glob_match(pat, s), fnmatch(pat, s, 0) == 0);
                }
            }
        }
        return;
    }
    {
        size_t i;
        for (i = 0; i < sizeof ps - 1; i++) {
            pat[depth] = ps[i];
            glob_pat(pat, plen, depth + 1);
        }
    }
}

static void test_glob_match(void)
{
    char pat[8];
    int plen;
    for (plen = 0; plen <= 4; plen++)
        glob_pat(pat, plen, 0);
    CHECK(glob_cases > 5000000, "glob differential ran %ld cases (%ld unclosed-bracket patterns set aside)", glob_cases, glob_unclosed);
    CHECK(glob_diffs == 0, "glob agrees with fnmatch(3) on every case (%ld differ)", glob_diffs);
    {
        char pat[8], sub[2];
        int x, y, c, neg;
        long n = 0, bad = 0;
        for (neg = 0; neg < 2; neg++)
            for (x = 0x20; x < 0x7f; x++)
                for (y = 0x20; y < 0x7f; y++) {
                    int k = 0;
                    if (x == '\\' || y == '\\' || x == ']' || y == ']' || x == '!' || x == '^')
                        continue;
                    pat[k++] = '[';
                    if (neg)
                        pat[k++] = '!';
                    pat[k++] = (char)x;
                    pat[k++] = '-';
                    pat[k++] = (char)y;
                    pat[k++] = ']';
                    pat[k] = 0;
                    for (c = 1; c < 0x80; c++) {
                        sub[0] = (char)c;
                        sub[1] = 0;
                        n++;
                        if ((dyn_glob_match(pat, sub) != 0) != (fnmatch(pat, sub, 0) == 0) && bad++ < 5)
                            printf("FAIL range: pattern \"%s\" subject 0x%02x\n", pat, c);
                    }
                }
        CHECK(n > 1000000 && bad == 0, "every ASCII range [x-y] and [!x-y] agrees with fnmatch(3) over every byte (%ld cases, %ld differ)", n, bad);
    }
    CHECK(dyn_glob_match("[", "[") && dyn_glob_match("a[", "a[") && dyn_glob_match("[a", "[a") && !dyn_glob_match("[a", "a")
            && dyn_glob_match("*[", "x[") && !dyn_glob_match("[", "a"),
        "POSIX XCU 2.13.1: an unclosed '[' matches itself");
}

static void sniff(const char* what, const uint8_t* p, size_t n, const char* want)
{
    const char* got = dyn_sniff_bytes(p, n);
    CHECK(!strcmp(got, want), "sniff %s -> %s, want %s", what, got, want);
}

static void test_sniff(void)
{
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13 };
    static const uint8_t gif[] = { 'G', 'I', 'F', '8', '9', 'a', 1, 0 };
    static const uint8_t pdf[] = { '%', 'P', 'D', 'F', '-', '1', '.', '7' };
    static const uint8_t gz[] = { 0x1F, 0x8B, 8, 0 };
    static const uint8_t zip[] = { 'P', 'K', 3, 4, 20, 0 };
    static const uint8_t elf[] = { 0x7F, 'E', 'L', 'F', 2, 1 };
    static const uint8_t wasm[] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    static const uint8_t bom[] = { 0xEF, 0xBB, 0xBF, 'h', 'i' };
    uint8_t buf[600];
    sniff("PNG", png, sizeof png, "image/png");
    sniff("GIF89a", gif, sizeof gif, "image/gif");
    sniff("PDF", pdf, sizeof pdf, "application/pdf");
    sniff("gzip", gz, sizeof gz, "application/gzip");
    sniff("zip", zip, sizeof zip, "application/zip");
    sniff("ELF", elf, sizeof elf, "application/x-elf");
    sniff("wasm", wasm, sizeof wasm, "application/wasm");
    sniff("UTF-8 BOM", bom, sizeof bom, "text/plain");
    memcpy(buf, "SQLite format 3", 16);
    sniff("SQLite", buf, 100, "application/vnd.sqlite3");
    memset(buf, 'x', sizeof buf);
    memcpy(buf + 257, "ustar", 5);
    sniff("ustar", buf, 512, "application/x-tar");
    memset(buf, 'x', sizeof buf);
    sniff("empty", buf, 0, "application/octet-stream");
    sniff("plain text", buf, sizeof buf, "text/plain");
    buf[511] = 0;
    sniff("NUL at 511", buf, sizeof buf, "application/octet-stream");
    buf[511] = 'x';
    buf[512] = 0;
    sniff("NUL at 512 (past the scan)", buf, sizeof buf, "text/plain");
}

static void test_names(void)
{
    char* j;
    CHECK(!strcmp(dyn_fs_errno_code(ENOENT), "ENOENT") && !strcmp(dyn_fs_errno_code(EACCES), "EACCES")
            && !strcmp(dyn_fs_errno_code(EEXIST), "EEXIST") && !strcmp(dyn_fs_errno_code(ENOTDIR), "ENOTDIR"),
        "errno codes carry their POSIX names");
    j = dyn_join("a", "b");
    CHECK(j && !strcmp(j, "a/b"), "join a + b");
    free(j);
    j = dyn_join("a/", "b");
    CHECK(j && !strcmp(j, "a/b"), "join does not double a separator");
    free(j);
    j = dyn_join("", "b");
    CHECK(j && !strcmp(j, "b"), "join with an empty head");
    free(j);
    {
        char p[202];
        CHECK(dyn_fs_temp_prefix_ok("", 0), "an empty prefix is allowed");
        memset(p, 'p', sizeof p);
        p[200] = 0;
        CHECK(dyn_fs_temp_prefix_ok(p, 200), "a 200-byte prefix is allowed");
        p[200] = 'p';
        p[201] = 0;
        CHECK(!dyn_fs_temp_prefix_ok(p, 201), "a 201-byte prefix is refused");
        CHECK(!dyn_fs_temp_prefix_ok(".x", 2) && !dyn_fs_temp_prefix_ok("a/b", 3) && !dyn_fs_temp_prefix_ok("a\\b", 3),
            "a leading dot or a separator is refused");
        CHECK(!dyn_fs_temp_prefix_ok("ab\0c", 4), "an embedded NUL is refused");
    }
}

static char root[1024];

static int is_dir(const char* p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void touch(const char* p)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void test_tree(void)
{
    char deep[1024], f[1100];
    const char* what;
    int i, e = 0;
    size_t n;
    snprintf(deep, sizeof deep, "%s/t", root);
    n = strlen(deep);
    for (i = 0; i < 40 && n + 3 < sizeof deep; i++) {
        memcpy(deep + n, "/d", 3);
        n += 2;
    }
    CHECK(dyn_fs_mkdirp(deep, 0755) == 0 && is_dir(deep), "mkdirp builds a 40-level chain");
    CHECK(dyn_fs_mkdirp(deep, 0755) == 0, "mkdirp on an existing directory succeeds");
    snprintf(f, sizeof f, "%s/file", deep);
    touch(f);
    errno = 0;
    CHECK(dyn_fs_mkdirp(f, 0755) != 0 && errno == EEXIST, "mkdirp over a file is EEXIST");
    snprintf(f, sizeof f, "%s/t", root);
    CHECK(dyn_fs_remove_tree(f, &e, &what) == 0 && !is_dir(f), "remove_tree removes the whole chain");
    CHECK(dyn_fs_remove_tree(f, &e, &what) == 0, "remove_tree on a missing path succeeds");
    snprintf(f, sizeof f, "%s/plain", root);
    touch(f);
    CHECK(dyn_fs_remove_tree(f, &e, &what) == 0 && access(f, F_OK) != 0, "remove_tree unlinks a plain file");
}

static void test_walk(void)
{
    char d[1100], f[1200];
    char pat[] = "g/*/f?.txt";
    char* segs[4];
    dyn_glob_res res;
    const char* names[] = { "x/f1.txt", "x/f2.txt", "y/f3.txt", "y/fa.md", "y/f10.txt", ".h/f5.txt" };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(d, sizeof d, "%s/g/%.*s", root, (int)(strchr(names[i], '/') - names[i]), names[i]);
        dyn_fs_mkdirp(d, 0755);
        snprintf(f, sizeof f, "%s/g/%s", root, names[i]);
        touch(f);
    }
    segs[0] = pat;
    pat[1] = 0;
    segs[1] = pat + 2;
    pat[3] = 0;
    segs[2] = pat + 4;
    memset(&res, 0, sizeof res);
    dyn_glob_walk(&res, root, segs, 3, 0, "", 0, 0);
    qsort(res.items, res.count, sizeof res.items[0], dyn_glob_str_cmp);
    CHECK(!res.oom && res.count == 3 && !strcmp(res.items[0], "g/x/f1.txt") && !strcmp(res.items[1], "g/x/f2.txt")
            && !strcmp(res.items[2], "g/y/f3.txt"),
        "walk g/*/f?.txt finds exactly the three one-character names and skips the hidden directory (%zu)", res.count);
    dyn_glob_res_free(&res);
}

static void test_lock(void)
{
    char p[1100];
    dyn_flock_t a = { -1, 0, NULL }, b = { -1, 0, NULL };
    snprintf(p, sizeof p, "%s/lk", root);
    CHECK(dyn_flock_try(&a, p) == 0, "first lock is taken");
    CHECK(dyn_flock_try(&b, p) == 1 && errno == EWOULDBLOCK, "a second description is refused with EWOULDBLOCK");
    dyn_flock_release(&a);
    CHECK(a.fd == -1, "release closes the descriptor");
    CHECK(dyn_flock_try(&b, p) == 0, "the lock is free after release");
    dyn_flock_release(&b);
    free(a.lockfile);
    free(b.lockfile);
}

static void test_copy(void)
{
    enum { N = 300 * 1024 };
    char src[1100], out[1100];
    int in, o, ok = 1;
    unsigned char* buf = (unsigned char*)malloc(N + 1);
    ssize_t r;
    size_t i;
    if (!buf) {
        CHECK(0, "malloc");
        return;
    }
    snprintf(src, sizeof src, "%s/copy.in", root);
    snprintf(out, sizeof out, "%s/copy.out", root);
    for (i = 0; i < (size_t)N; i++)
        buf[i] = (unsigned char)(i * 131u >> 3);
    in = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(in >= 0 && write(in, buf, N) == N, "source written");
    lseek(in, 0, SEEK_SET);
    o = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(o >= 0 && dyn_copy_fds(in, o) == 0, "copy_fds copies a regular file larger than the copy buffer");
    memset(buf, 0, N + 1);
    lseek(o, 0, SEEK_SET);
    r = read(o, buf, N + 1);
    for (i = 0; r == N && i < (size_t)N; i++)
        if (buf[i] != (unsigned char)(i * 131u >> 3)) {
            ok = 0;
            break;
        }
    CHECK(r == N && ok, "copy is byte-exact over %d bytes (read %zd)", N, r);
    free(buf);
    close(in);
    close(o);
}

int main(void)
{
    char* tpl = dyn_fs_temp_template("fsops-core");
    int e;
    const char* what;
    if (!tpl || !mkdtemp(tpl)) {
        printf("FAIL cannot make a temp dir\n");
        return 1;
    }
    snprintf(root, sizeof root, "%s", tpl);
    free(tpl);
    test_glob_match();
    test_sniff();
    test_names();
    test_tree();
    test_walk();
    test_lock();
    test_copy();
    CHECK(dyn_fs_remove_tree(root, &e, &what) == 0 && !is_dir(root), "the scratch root is removed");
    printf("test_fsops_core: %d checks, %ld glob cases, %d failed\n", checks, glob_cases, fails);
    return fails ? 1 : 0;
}
