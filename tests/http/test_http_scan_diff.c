/*
 * Differential + refusal oracle for the HTTP request-head scanner (dyn_scan_head,
 * src/modules/http/http-scan.c).
 *
 * OLD below is the scanner exactly as it shipped before the rewrite (frozen copy with
 * its own token table and case-insensitive compare, so it shares no code with the
 * scanner under test). Both run on every case of the corpus; the return code must be
 * identical, and on success every field (pointers compared as offsets into the buffer).
 * Callers only read the fields after a 0 return, which is why fields are compared there.
 *
 * Corpus: the named cases below (each with its own expected outcome: that list is the
 * refusal test and does not depend on OLD), every prefix of every named case (a read
 * boundary at every byte offset), every single-byte substitution with all 256 byte
 * values over the base requests, every line of tests/corpus/blns.txt as a header value,
 * a header name and a request target, and seeded random mutations (insert, delete,
 * duplicate, byte flip). Every case runs in a heap buffer of exactly its length, so a
 * one-byte over-read is an ASan report, not a silent pass.
 *
 * Header names are RFC 9110 5.1 tokens: OLD's name loop uses its own OLD_TCHAR, and
 * name_table() is exhaustive over the scanner's name table: every byte 0..255 inside a name
 * and as a whole name against an independently spelled tchar string (accept iff tchar, ':'
 * ends the name), and every tchar pair for case-insensitive equality via the dup flag
 * (equal iff ASCII-lowercase equal), so a table edit cannot regress silently. Until
 * 2026-11-28 any byte >= 0x20 except ':' SP HTAB DEL passed, so "Fo(o)@: x" was served.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "http-scan.h"

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static int old_ci_eq(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

static const uint8_t OLD_TCHAR[256] = {
    ['!'] = 1,
    ['#'] = 1,
    ['$'] = 1,
    ['%'] = 1,
    ['&'] = 1,
    ['\''] = 1,
    ['*'] = 1,
    ['+'] = 1,
    ['-'] = 1,
    ['.'] = 1,
    ['^'] = 1,
    ['_'] = 1,
    ['`'] = 1,
    ['|'] = 1,
    ['~'] = 1,
    ['0'] = 1,
    ['1'] = 1,
    ['2'] = 1,
    ['3'] = 1,
    ['4'] = 1,
    ['5'] = 1,
    ['6'] = 1,
    ['7'] = 1,
    ['8'] = 1,
    ['9'] = 1,
    ['a'] = 1,
    ['b'] = 1,
    ['c'] = 1,
    ['d'] = 1,
    ['e'] = 1,
    ['f'] = 1,
    ['g'] = 1,
    ['h'] = 1,
    ['i'] = 1,
    ['j'] = 1,
    ['k'] = 1,
    ['l'] = 1,
    ['m'] = 1,
    ['n'] = 1,
    ['o'] = 1,
    ['p'] = 1,
    ['q'] = 1,
    ['r'] = 1,
    ['s'] = 1,
    ['t'] = 1,
    ['u'] = 1,
    ['v'] = 1,
    ['w'] = 1,
    ['x'] = 1,
    ['y'] = 1,
    ['z'] = 1,
    ['A'] = 1,
    ['B'] = 1,
    ['C'] = 1,
    ['D'] = 1,
    ['E'] = 1,
    ['F'] = 1,
    ['G'] = 1,
    ['H'] = 1,
    ['I'] = 1,
    ['J'] = 1,
    ['K'] = 1,
    ['L'] = 1,
    ['M'] = 1,
    ['N'] = 1,
    ['O'] = 1,
    ['P'] = 1,
    ['Q'] = 1,
    ['R'] = 1,
    ['S'] = 1,
    ['T'] = 1,
    ['U'] = 1,
    ['V'] = 1,
    ['W'] = 1,
    ['X'] = 1,
    ['Y'] = 1,
    ['Z'] = 1,
};

static int old_head_dup_ok(const char* n, size_t len)
{
    static const struct {
        const char* s;
        size_t l;
    } ok[] = {
        { "cookie", 6 },
        { "set-cookie", 10 },
        { "connection", 10 },
        { "accept", 6 },
        { "accept-encoding", 15 },
        { "accept-language", 15 },
        { "cache-control", 13 },
        { "vary", 4 },
    };
    size_t i;
    for (i = 0; i < countof(ok); i++)
        if (ok[i].l == len && old_ci_eq(n, ok[i].s, len))
            return 1;
    return 0;
}

static int old_ci_lit(const char* p, size_t len, const char* lit, size_t ll)
{
    size_t i;
    if (len != ll)
        return 0;
    for (i = 0; i < ll; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != lit[i])
            return 0;
    }
    return 1;
}

static int old_scan_head(const char* buf, size_t avail, dyn_reqinfo_t* ri)
{
    const char *base = buf, *end = buf + avail;
    const char *line, *le = NULL;
    const char* reqline_end = NULL;
    const char* seen[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t seen_len[DYN_APP_MAX_HEADER_COUNT + 1];
    size_t nseen = 0;

    memset(ri, 0, sizeof(*ri));
    ri->n_lines = 0;

    line = base;
    for (;;) {
        const char* p = line;
        for (;;) {
            p = end - p > 1 ? (const char*)memchr(p, '\r', (size_t)(end - p - 1)) : NULL;
            if (!p)
                return -1;
            if (p[1] == '\n')
                break;
            p++;
        }
        le = p;
        if (!reqline_end) {
            const char* sp1 = (const char*)memchr(line, ' ', (size_t)(le - line));
            const char* sp2 = sp1
                ? (const char*)memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1))
                : NULL;
            if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1) {
                ri->method = line;
                ri->method_len = 0;
                ri->target = le;
                ri->target_len = 0;
                ri->bad_target = 1;
            } else {
                const char* t = sp1 + 1;
                size_t tlen = (size_t)(sp2 - t);
                int in_query = 0;
                ri->method = line;
                ri->method_len = (size_t)(sp1 - line);
                ri->target = t;
                ri->bad_target = 0;
                while (tlen > 0) {
                    unsigned char ch = (unsigned char)*t;
                    if (ch < 0x21 || ch == 0x7f) {
                        ri->bad_target = 1;
                        break;
                    }
                    if (!in_query) {
                        if (ch == '?')
                            in_query = 1;
                        else
                            ri->target_len++;
                    }
                    t++;
                    tlen--;
                }
                if (le - (sp2 + 1) != 8
                    || (memcmp(sp2 + 1, "HTTP/1.1", 8) != 0
                        && memcmp(sp2 + 1, "HTTP/1.0", 8) != 0))
                    ri->bad_target = 1;
                {
                    const char* mc;
                    for (mc = line; mc < sp1; mc++) {
                        if (!OLD_TCHAR[(unsigned char)*mc]) {
                            ri->bad_target = 1;
                            break;
                        }
                    }
                }
            }
            ri->http11 = (le - line) >= 8 && memcmp(le - 8, "HTTP/1.1", 8) == 0;
            reqline_end = le;
        } else {
            const char* c = line;
            if (c == le) {
                ri->head_len = (size_t)(le + 2 - base);
                break;
            }
            ri->n_lines++;
            while (c < le && OLD_TCHAR[(unsigned char)*c])
                c++;
            if (c == line)
                return DYN_HEAD_ERR_LINE;
            if (c >= le || *c != ':')
                return DYN_HEAD_ERR_LINE;
            if (c[-1] == ' ' || c[-1] == '\t')
                return DYN_HEAD_ERR_LINE;
            {
                const char* name = line;
                size_t nlen = (size_t)(c - name);
                const char *v = c + 1, *ve = le;
                while (v < ve && (*v == ' ' || *v == '\t'))
                    v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                    ve--;
                {
                    const char* q;
                    for (q = v; q < ve; q++)
                        if (((unsigned char)*q < 0x20 && *q != '\t') || (unsigned char)*q == 0x7f)
                            return DYN_HEAD_ERR_LINE;
                }
                if (!old_head_dup_ok(name, nlen)) {
                    size_t i;
                    int dup = 0;
                    for (i = 0; i < nseen; i++)
                        if (seen_len[i] == nlen && old_ci_eq(name, seen[i], nlen)) {
                            dup = 1;
                            break;
                        }
                    if (dup)
                        ri->dup = 1;
                    else if (nseen < countof(seen)) {
                        seen[nseen] = name;
                        seen_len[nseen] = nlen;
                        nseen++;
                    }
                }
                if (old_ci_lit(name, nlen, "content-length", 14)) {
                    if (!ri->cl_set) {
                        ri->cl = v;
                        ri->cl_len = (size_t)(ve - v);
                        ri->cl_set = 1;
                    }
                    ri->cl_count++;
                } else if (old_ci_lit(name, nlen, "transfer-encoding", 17))
                    ri->has_te = 1;
                else if (old_ci_lit(name, nlen, "host", 4))
                    ri->host_count++;
                if (!ri->ae && old_ci_lit(name, nlen, "accept-encoding", 15)) {
                    ri->ae = v;
                    ri->ae_len = (size_t)(ve - v);
                }
                if (!ri->conn && old_ci_lit(name, nlen, "connection", 10)) {
                    ri->conn = v;
                    ri->conn_len = (size_t)(ve - v);
                }
                if (!ri->ct && old_ci_lit(name, nlen, "content-type", 12)) {
                    ri->ct = v;
                    ri->ct_len = (size_t)(ve - v);
                }
            }
        }
        line = le + 2;
        if (line >= end) {
            return -1;
        }
    }
    return 0;
}

static long ncase, nfail, n_ok, n_err, n_inc, n_dup, n_te, n_cl2;

static void hexdump(const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n && i < 400; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c >= 0x20 && c < 0x7f && c != '\\')
            fputc(c, stderr);
        else
            fprintf(stderr, "\\x%02x", c);
    }
    if (n > 400)
        fprintf(stderr, "...(%zu bytes)", n);
    fputc('\n', stderr);
}

#define OFF(p, b) ((p) ? (long)((p) - (b)) : -1L)

static int fields_eq(const dyn_reqinfo_t* a, const dyn_reqinfo_t* b, const char* buf)
{
    return OFF(a->method, buf) == OFF(b->method, buf) && a->method_len == b->method_len
        && OFF(a->target, buf) == OFF(b->target, buf) && a->target_len == b->target_len
        && a->head_len == b->head_len && a->n_lines == b->n_lines
        && OFF(a->cl, buf) == OFF(b->cl, buf) && a->cl_len == b->cl_len
        && OFF(a->ae, buf) == OFF(b->ae, buf) && a->ae_len == b->ae_len
        && OFF(a->conn, buf) == OFF(b->conn, buf) && a->conn_len == b->conn_len
        && OFF(a->ct, buf) == OFF(b->ct, buf) && a->ct_len == b->ct_len
        && a->cl_count == b->cl_count && a->has_te == b->has_te && a->host_count == b->host_count
        && a->cl_set == b->cl_set && a->dup == b->dup && a->http11 == b->http11
        && a->bad_target == b->bad_target;
}

static int run(const char* src, size_t len, dyn_reqinfo_t* out_new)
{
    char* b = (char*)malloc(len ? len : 1);
    dyn_reqinfo_t ro, rn;
    int o, n;
    memcpy(b, src, len);
    o = old_scan_head(b, len, &ro);
    n = dyn_scan_head(b, len, &rn);
    ncase++;
    if (o == 0)
        n_ok++;
    else if (o == -1)
        n_inc++;
    else
        n_err++;
    if (o == 0 && ro.dup)
        n_dup++;
    if (o == 0 && ro.has_te)
        n_te++;
    if (o == 0 && ro.cl_count > 1)
        n_cl2++;
    if (o != n || (o == 0 && !fields_eq(&ro, &rn, b))) {
        if (nfail < 10) {
            fprintf(stderr, "FAIL differential: old=%d new=%d len=%zu: ", o, n, len);
            hexdump(b, len);
            if (o == 0 && n == 0)
                fprintf(stderr, "  old dup=%d n_lines=%zu head_len=%zu cl_count=%d host=%d | new dup=%d n_lines=%zu head_len=%zu cl_count=%d host=%d\n",
                    ro.dup, ro.n_lines, ro.head_len, ro.cl_count, ro.host_count,
                    rn.dup, rn.n_lines, rn.head_len, rn.cl_count, rn.host_count);
        }
        nfail++;
    }
    if (out_new)
        *out_new = rn;
    free(b);
    return n;
}

static void every_prefix(const char* s, size_t len)
{
    size_t k;
    for (k = 0; k <= len; k++)
        run(s, k, NULL);
}

static void every_byte(const char* s, size_t len)
{
    char* m = (char*)malloc(len);
    size_t i;
    int v;
    for (i = 0; i < len; i++) {
        memcpy(m, s, len);
        for (v = 0; v < 256; v++) {
            m[i] = (char)v;
            run(m, len, NULL);
        }
    }
    free(m);
}

enum { X_OK,
    X_ERR,
    X_INC };
typedef struct {
    const char* name;
    const char* req;
    int want;
    int want_dup, want_te, want_cl_count, want_bad_target, want_host;
} named_t;

#define R(s) "GET / HTTP/1.1\r\nHost: a\r\n" s "\r\n"

static const named_t NAMED[] = {
    { "minimal", "GET / HTTP/1.1\r\nHost: a\r\n\r\n", X_OK, 0, 0, 0, 0, 1 },
    { "obs-fold continuation line", R("X: a\r\n b\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "obs-fold with tab", R("X: a\r\n\tb\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "bare CR in value", R("X: a\rb\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "bare LF in value", R("X: a\nb\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "bare LF line end", R("X: a\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "NUL in value", "GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n", X_ERR, 0, 0, 0, 0, 1 },
    { "NUL in name", "GET / HTTP/1.1\r\nHost: a\r\nX\0Y: b\r\n\r\n", X_ERR, 0, 0, 0, 0, 1 },
    { "DEL in value", R("X: a\x7f"
                        "b\r\n"),
        X_ERR, 0, 0, 0, 0, 1 },
    { "control byte in value", R("X: a\x01"
                                 "b\r\n"),
        X_ERR, 0, 0, 0, 0, 1 },
    { "space before colon", R("X : a\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "tab before colon", R("X\t: a\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "empty name", R(": a\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "no colon", R("Xa\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "leading space line", R(" X: a\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "duplicate content-length", R("Content-Length: 1\r\ncontent-length: 1\r\n"), X_OK, 1, 0, 2, 0, 1 },
    { "content-length + transfer-encoding", R("Content-Length: 1\r\nTransfer-Encoding: chunked\r\n"), X_OK, 0, 1, 1, 0, 1 },
    { "duplicate host", R("HOST: b\r\n"), X_OK, 1, 0, 0, 0, 2 },
    { "duplicate ordinary header, mixed case", R("X-A: 1\r\nx-a: 2\r\n"), X_OK, 1, 0, 0, 0, 1 },
    { "duplicates allowed list", R("Cookie: a\r\ncookie: b\r\nAccept: x\r\naccept: y\r\nConnection: a\r\nconnection: b\r\nVary: a\r\nvary: b\r\nSet-Cookie: a\r\nset-cookie: b\r\nCache-Control: a\r\ncache-control: b\r\nAccept-Encoding: a\r\naccept-encoding: b\r\nAccept-Language: a\r\naccept-language: b\r\n"), X_OK, 0, 0, 0, 0, 1 },
    { "name with separators is refused (RFC 9110 tchar)", R("Fo(o)@: x\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "name with high bytes is refused", R("X\xc3\xa9: x\r\n"), X_ERR, 0, 0, 0, 0, 1 },
    { "every tchar class in one name", R("!#$%&'*+-.^_`|~09azAZ: x\r\n"), X_OK, 0, 0, 0, 0, 1 },
    { "value with high bytes and tab", R("X: a\t\xff\xfe b\r\n"), X_OK, 0, 0, 0, 0, 1 },
    { "bad method token", "G(T / HTTP/1.1\r\nHost: a\r\n\r\n", X_OK, 0, 0, 0, 1, 1 },
    { "bad version", "GET / HTTP/1.2\r\nHost: a\r\n\r\n", X_OK, 0, 0, 0, 1, 1 },
    { "control in target", "GET /\x01 HTTP/1.1\r\nHost: a\r\n\r\n", X_OK, 0, 0, 0, 1, 1 },
    { "missing target", "GET  HTTP/1.1\r\nHost: a\r\n\r\n", X_OK, 0, 0, 0, 1, 1 },
    { "HTTP/1.0 no host", "GET / HTTP/1.0\r\n\r\n", X_OK, 0, 0, 0, 0, 0 },
    { "no terminator", "GET / HTTP/1.1\r\nHost: a\r\n", X_INC, 0, 0, 0, 0, 0 },
    { "CR at the last byte", "GET / HTTP/1.1\r\nHost: a\r", X_INC, 0, 0, 0, 0, 0 },
    { "control byte, no CRLF after", "GET / HTTP/1.1\r\nHost: a\x01", X_INC, 0, 0, 0, 0, 0 },
};

static void named_cases(void)
{
    size_t i;
    for (i = 0; i < countof(NAMED); i++) {
        const named_t* t = &NAMED[i];
        size_t len = strlen(t->req);
        dyn_reqinfo_t r;
        int got;
        if (strncmp(t->name, "NUL in ", 7) == 0)
            len = 35;
        got = run(t->req, len, &r);
        if ((t->want == X_OK && got != 0) || (t->want == X_ERR && got != DYN_HEAD_ERR_LINE)
            || (t->want == X_INC && got != -1)
            || (t->want == X_OK && (r.dup != t->want_dup || r.has_te != t->want_te || r.cl_count != t->want_cl_count || r.bad_target != t->want_bad_target || r.host_count != t->want_host))) {
            fprintf(stderr, "FAIL refusal: %s: got %d (dup %d te %d cl_count %d bad_target %d host %d)\n",
                t->name, got, r.dup, r.has_te, r.cl_count, r.bad_target, r.host_count);
            nfail++;
        }
        every_prefix(t->req, len);
    }
}

static const char RFC_TCHAR[] = "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int rfc_tchar(int b)
{
    return b != 0 && memchr(RFC_TCHAR, b, sizeof RFC_TCHAR - 1) != NULL;
}

static int ascii_lower(int b)
{
    return b >= 'A' && b <= 'Z' ? b + 32 : b;
}

static long n_table;

static void table_case(const char* what, int b, const char* req, size_t len, int want_ok, int want_dup)
{
    dyn_reqinfo_t r;
    int got = run(req, len, &r);
    n_table++;
    if (want_ok ? (got != 0 || (want_dup >= 0 && r.dup != want_dup)) : got != DYN_HEAD_ERR_LINE) {
        fprintf(stderr, "FAIL name table: %s byte 0x%02x: got %d dup %d, want %s dup %d\n", what, b, got,
            got == 0 ? r.dup : -1, want_ok ? "ok" : "line error", want_dup);
        nfail++;
    }
}

static void name_table(void)
{
    char req[128];
    int a, b;
    size_t n;
    for (b = 0; b < 256; b++) {
        n = (size_t)snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: a\r\nX%cY: v\r\n\r\n", 1);
        req[26] = (char)b;
        table_case("inside a name", b, req, n, rfc_tchar(b) || b == ':', 0);
        n = (size_t)snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: a\r\n%c: v\r\n\r\n", 1);
        req[25] = (char)b;
        table_case("as a whole name", b, req, n, rfc_tchar(b), 0);
    }
    for (a = 0; a < 256; a++) {
        if (!rfc_tchar(a))
            continue;
        for (b = 0; b < 256; b++) {
            if (!rfc_tchar(b))
                continue;
            n = (size_t)snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: a\r\nZ%c: 1\r\nZ%c: 2\r\n\r\n", a, b);
            table_case("pair", a * 256 + b, req, n, 1, ascii_lower(a) == ascii_lower(b));
        }
    }
}

static const char BROWSER[] = "GET /users/42?x=abc HTTP/1.1\r\nHost: localhost:8080\r\nUser-Agent: Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/120.0\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\nAccept-Language: en-US,en;q=0.5\r\nAccept-Encoding: gzip, deflate, br\r\nConnection: keep-alive\r\nCookie: session=abcdef0123456789; theme=dark\r\nCache-Control: max-age=0\r\n\r\n";
static const char MINIMAL[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
static const char POST[] = "POST /api/v1/items?x=1&y=2 HTTP/1.1\r\nHost: example.com\r\nContent-Type: application/json\r\nContent-Length: 17\r\nConnection: keep-alive\r\nAccept-Encoding: gzip, br\r\n\r\n{\"a\":1,\"b\":[1,2]}";

static void blns(void)
{
    FILE* f = fopen("tests/corpus/blns.txt", "rb");
    char line[8192], req[20000];
    long n = 0;
    if (!f) {
        fprintf(stderr, "FAIL: tests/corpus/blns.txt missing (run from the repo root); the corpus cannot run on the floor list\n");
        nfail++;
        return;
    }
    while (fgets(line, sizeof line, f)) {
        size_t l = strcspn(line, "\n"), k;
        line[l] = 0;
        k = (size_t)snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: a\r\nX: %s\r\n\r\n", line);
        run(req, k, NULL);
        k = (size_t)snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: a\r\n%s: v\r\n\r\n", line);
        run(req, k, NULL);
        k = (size_t)snprintf(req, sizeof req, "GET /%s HTTP/1.1\r\nHost: a\r\n\r\n", line);
        run(req, k, NULL);
        n++;
    }
    fclose(f);
    if (n < 500) {
        fprintf(stderr, "FAIL: blns corpus has %ld lines (expected the vendored ~740)\n", n);
        nfail++;
    }
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void)
{
    uint64_t z = (rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void mutate(const char* s, size_t len, long rounds)
{
    static const char pieces[][8] = { "\r\n", "\r", "\n", ":", " ", "\t", "\r\n\r\n", "\x7f", "", "Host", "a: b" };
    char* m = (char*)malloc(len * 3 + 64);
    long r;
    for (r = 0; r < rounds; r++) {
        size_t n = len, ops = 1 + next() % 4, o;
        memcpy(m, s, len);
        for (o = 0; o < ops; o++) {
            size_t at = n ? next() % (n + 1) : 0;
            switch (next() % 4) {
            case 0:
                if (n < len * 2) {
                    const char* p = pieces[next() % countof(pieces)];
                    size_t pl = strlen(p);
                    memmove(m + at + pl, m + at, n - at);
                    memcpy(m + at, p, pl);
                    n += pl;
                }
                break;
            case 1:
                if (at < n) {
                    size_t dl = 1 + next() % 8;
                    if (dl > n - at)
                        dl = n - at;
                    memmove(m + at, m + at + dl, n - at - dl);
                    n -= dl;
                }
                break;
            case 2:
                if (at < n)
                    m[at] = (char)(next() & 0xff);
                break;
            default:
                if (at < n && n < len * 2) {
                    size_t dl = 1 + next() % 32, from = next() % n;
                    char seg[32];
                    if (dl > n - from)
                        dl = n - from;
                    memcpy(seg, m + from, dl);
                    memmove(m + at + dl, m + at, n - at);
                    memcpy(m + at, seg, dl);
                    n += dl;
                }
                break;
            }
        }
        run(m, n, NULL);
    }
    free(m);
}

static void many_headers(void)
{
    size_t cap = 64 * 1024, n;
    char* b = (char*)malloc(cap);
    int i, k;
    for (k = 255; k <= 260; k++) {
        n = (size_t)snprintf(b, cap, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (i = 0; i < k; i++)
            n += (size_t)snprintf(b + n, cap - n, "H%d: v\r\n", i);
        n += (size_t)snprintf(b + n, cap - n, "H%d: again\r\n\r\n", k - 1);
        run(b, n, NULL);
    }
    n = (size_t)snprintf(b, cap, "GET / HTTP/1.1\r\nHost: a\r\nX: ");
    memset(b + n, 'v', 20000);
    n += 20000;
    memcpy(b + n, "\r\n\r\n", 4);
    run(b, n + 4, NULL);
    free(b);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    named_cases();
    name_table();
    every_prefix(BROWSER, sizeof BROWSER - 1);
    every_prefix(POST, sizeof POST - 1);
    every_byte(MINIMAL, sizeof MINIMAL - 1);
    every_byte(BROWSER, sizeof BROWSER - 1);
    every_byte(POST, sizeof POST - 1);
    blns();
    many_headers();
    mutate(BROWSER, sizeof BROWSER - 1, 100000);
    mutate(POST, sizeof POST - 1, 100000);
    mutate(MINIMAL, sizeof MINIMAL - 1, 100000);
    printf("test_http_scan_diff: name table %ld cases\n", n_table);
    printf("test_http_scan_diff: %ld cases (ok %ld, line errors %ld, incomplete %ld; dup %ld, te %ld, cl>1 %ld), %ld failures\n",
        ncase, n_ok, n_err, n_inc, n_dup, n_te, n_cl2, nfail);
    if (n_err < 1000 || n_dup < 100 || n_te < 1 || n_cl2 < 1 || n_inc < 1000) {
        fprintf(stderr, "FAIL: the corpus no longer reaches every outcome class\n");
        nfail++;
    }
    return nfail ? 1 : 0;
}
