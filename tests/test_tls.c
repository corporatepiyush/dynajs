/*
 * test_tls.c -- the badssl matrix, which design 16 makes mandatory.
 *
 * "Verify against the world": every BAD endpoint must fail AND fail for the
 * named reason; every GOOD one must connect. A handshake that merely completes
 * proves nothing -- the interesting cases are the refusals.
 *
 * Fault injection without editing code: the same wrong-host endpoint is run
 * once with verification ON (must fail) and once with insecure=1 (must
 * succeed). If the second also failed, the first proved nothing about hostname
 * checking; if the first succeeded, the check is not load-bearing.
 *
 * Needs the network. Absence is a LOUD skip, and a failure under
 * DYNAJS_REQUIRE_TOOLS=1, because a silently skipped matrix reads as a pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "dyna-tls.h"

#ifndef CONFIG_TLS
int main(void) { printf("test_tls: CONFIG_TLS not built; nothing to run\n"); return 0; }
#else

static int n_pass, n_fail, n_skip;

static void ok(int cond, const char *what, const char *detail)
{
    if (cond) { n_pass++; printf("  ok    %s\n", what); }
    else      { n_fail++; printf("  FAIL  %s  [%s]\n", what, detail ? detail : "-"); }
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL, *ai;
    struct timeval tv;
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        /* A blocking socket with a timeout: this test is about certificate
           decisions, not about the reactor, which has its own tests. */
        tv.tv_sec = 10; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Drive the state machine to handshake completion over a real socket.
   0 = handshaked, -1 = refused (why[] says which check), -2 = no network. */
static int handshake(const char *host, const char *port, const char *sni,
                     int insecure, int min_version,
                     char *why, size_t whylen, char *info, size_t infolen)
{
    dyn_tls_opts_t o;
    dyn_tls_ctx_t *ctx;
    dyn_tls_conn_t *t;
    uint8_t buf[16384];
    int fd, rc, ret = -1;
    char err[192];

    why[0] = 0; if (info) info[0] = 0;
    memset(&o, 0, sizeof o);
    o.insecure = insecure;
    o.min_version = min_version;
    o.alpn = "http/1.1";

    ctx = dyn_tls_ctx_client(&o, err, sizeof err);
    if (!ctx) { snprintf(why, whylen, "ctx: %s", err); return -1; }

    fd = tcp_connect(host, port);
    if (fd < 0) { dyn_tls_ctx_free(ctx); return -2; }

    t = dyn_tls_conn_new(ctx, sni, err, sizeof err);
    if (!t) { snprintf(why, whylen, "conn: %s", err); close(fd); dyn_tls_ctx_free(ctx); return -1; }

    for (;;) {
        int st = dyn_tls_handshake(t);
        ssize_t nw, nr;
        if (st < 0) {
            const char *e = dyn_tls_error(t);
            snprintf(why, whylen, "%s", e ? e : "handshake failed");
            break;
        }
        /* Flush whatever the engine queued, then feed it what arrives. */
        while ((rc = dyn_tls_pull(t, buf, sizeof buf)) > 0) {
            nw = write(fd, buf, (size_t)rc);
            if (nw != rc) { snprintf(why, whylen, "socket write"); goto done; }
        }
        if (st == 1) {
            char alpn[64];
            if (info)
                snprintf(info, infolen, "%s alpn=%s",
                         dyn_tls_version_negotiated(t),
                         dyn_tls_alpn_selected(t, alpn, sizeof alpn)
                             ? alpn : "-");
            ret = 0;
            break;
        }
        nr = read(fd, buf, sizeof buf);
        if (nr <= 0) { snprintf(why, whylen, "peer closed during handshake"); break; }
        if (dyn_tls_feed(t, buf, (size_t)nr) != 0) {
            snprintf(why, whylen, "feed");
            break;
        }
    }
done:
    dyn_tls_conn_free(t);
    close(fd);
    dyn_tls_ctx_free(ctx);
    return ret;
}

/* A bad endpoint must be refused, and the reason must MENTION `expect` --
   "handshake failed" would pass a check that rejected for the wrong reason. */
static void must_refuse(const char *host, const char *port, const char *expect)
{
    char why[192], label[256];
    int r = handshake(host, port, host, 0, 12, why, sizeof why, NULL, 0);
    snprintf(label, sizeof label, "REFUSED %s (%s)", host, expect);
    if (r == -2) { n_skip++; printf("  SKIP  %s -- no network\n", host); return; }
    if (r == 0) { n_fail++; printf("  FAIL  %s  [CONNECTED -- the check is not live]\n", label); return; }
    ok(strstr(why, expect) != NULL, label, why);
}

/* Assert the negotiated version, because a client that silently never offers
   1.3 passes every plain "did it connect" check. */
static void must_connect_version(const char *host, const char *port,
                                 const char *want)
{
    char why[192], info[128], label[256];
    int r = handshake(host, port, host, 0, 12, why, sizeof why, info, sizeof info);
    snprintf(label, sizeof label, "CONNECT %s negotiates %s [%s]", host, want, info);
    if (r == -2) { n_skip++; printf("  SKIP  %s -- no network\n", host); return; }
    ok(r == 0 && strstr(info, want) != NULL, label, r ? why : info);
}

static void must_connect(const char *host, const char *port)
{
    char why[192], info[128], label[256];
    int r = handshake(host, port, host, 0, 12, why, sizeof why, info, sizeof info);
    snprintf(label, sizeof label, "CONNECT %s [%s]", host, info);
    if (r == -2) { n_skip++; printf("  SKIP  %s -- no network\n", host); return; }
    ok(r == 0, label, why);
}

int main(void)
{
    const char *req = getenv("DYNAJS_REQUIRE_TOOLS");
    char why[192], info[128];
    int r;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* a hang must not erase the results */
    printf("test_tls: backend=%s linked=%s\n",
           dyn_tls_backend(), dyn_tls_runtime_version());

    printf("--- good endpoints ---\n");
    must_connect("badssl.com", "443");
    must_connect("tls-v1-2.badssl.com", "1012");
    /* badssl.com is 1.2-only, so a 1.3 host is REQUIRED to prove 1.3 works. */
    must_connect_version("cloudflare.com", "443", "TLSv1.3");

    printf("--- bad endpoints: each must fail FOR ITS OWN REASON ---\n");
    must_refuse("expired.badssl.com", "443", "expired");
    must_refuse("wrong.host.badssl.com", "443", "match");
    must_refuse("self-signed.badssl.com", "443", "self-signed certificate");
    must_refuse("untrusted-root.badssl.com", "443", "in certificate chain");

    printf("--- the checks are load-bearing (no code edit: insecure=1) ---\n");
    r = handshake("wrong.host.badssl.com", "443", "wrong.host.badssl.com",
                  1, 12, why, sizeof why, info, sizeof info);
    if (r == -2) { n_skip++; printf("  SKIP  insecure probe -- no network\n"); }
    else ok(r == 0, "wrong-host CONNECTS with verification off "
                    "(so the refusal above was hostname checking)", why);

    printf("--- version floor ---\n");
    r = handshake("tls-v1-2.badssl.com", "1012", "tls-v1-2.badssl.com",
                  0, 13, why, sizeof why, NULL, 0);
    if (r == -2) { n_skip++; printf("  SKIP  1.3 floor -- no network\n"); }
    else ok(r != 0, "a 1.2-only server is refused when the floor is 1.3", why);

    printf("test_tls: %d passed, %d failed, %d skipped\n", n_pass, n_fail, n_skip);
    if (n_skip && req && !strcmp(req, "1")) {
        printf("FAIL: skips are not allowed with DYNAJS_REQUIRE_TOOLS=1\n");
        return 1;
    }
    return n_fail ? 1 : 0;
}
#endif
