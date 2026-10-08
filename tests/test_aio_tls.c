#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>

#include "dyna-aio.h"
#include "dyna-tls.h"

#ifndef CONFIG_TLS
int main(void) { printf("test_aio_tls: CONFIG_TLS not built; nothing to run\n"); return 0; }
#else

#define PAYLOAD (96 * 1024)

static int n_pass, n_fail;
static void ok(int c, const char *w) {
    if (c) { n_pass++; printf("  ok    %s\n", w); }
    else   { n_fail++; printf("  FAIL  %s\n", w); }
}

static int cli_up, srv_up, srv_fail;
static size_t srv_bytes;
static int srv_calls;
static unsigned char *srv_seen;

static void cli_hs(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *u)
{ (void)a; (void)b; (void)n; (void)u; if (res == 0) cli_up = 1; else srv_fail = 1; }

static void srv_hs(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *u)
{ (void)a; (void)b; (void)n; (void)u; if (res == 0) srv_up = 1; else srv_fail = 1; }

static void srv_data(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *u)
{
    (void)a; (void)u;
    if (res < 0) { srv_fail = 1; return; }
    if (n == 0) return;
    srv_calls++;
    if (srv_bytes + n <= PAYLOAD)
        memcpy(srv_seen + srv_bytes, b, n);
    srv_bytes += n;
}

static void cli_data(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *u)
{ (void)a; (void)res; (void)b; (void)n; (void)u; }

int main(void)
{
    dyn_aio_t *a;
    dyn_tls_ctx_t *sctx, *cctx;
    dyn_tls_conn_t *sconn, *cconn;
    dyn_tls_opts_t copt;
    unsigned char *payload;
    char err[192], cert[] = "/tmp/_aiotls_cert.pem", key[] = "/tmp/_aiotls_key.pem";
    char cmd[512];
    int sv[2], i, spins;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_aio_tls: backend=%s\n", dyn_tls_backend());

    snprintf(cmd, sizeof cmd,
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 2 "
             "-nodes -subj /CN=localhost >/dev/null 2>&1", key, cert);
    if (system(cmd) != 0) { printf("test_aio_tls: SKIP (no openssl)\n"); return 0; }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("test_aio_tls: SKIP (socketpair)\n"); return 0;
    }
    for (i = 0; i < 2; i++) {
        int fl = fcntl(sv[i], F_GETFL, 0);
        int big = 1 << 20;
        if (fl >= 0) fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
        setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    }
    a = dyn_aio_new(64, 0);
    if (!a) { printf("test_aio_tls: SKIP (reactor)\n"); return 0; }

    {
        dyn_tls_srv_opts_t sopt;
        memset(&sopt, 0, sizeof sopt);
        sopt.cert = cert;
        sopt.key = key;
        sctx = dyn_tls_ctx_server(&sopt, err, sizeof err);
    }
    if (!sctx) { printf("  FAIL  server ctx: %s\n", err); return 1; }
    memset(&copt, 0, sizeof copt);
    copt.insecure = 1;
    copt.min_version = 12;
    cctx = dyn_tls_ctx_client(&copt, err, sizeof err);
    if (!cctx) { printf("  FAIL  client ctx: %s\n", err); return 1; }

    sconn = dyn_tls_conn_accept(sctx, err, sizeof err);
    cconn = dyn_tls_conn_new(cctx, NULL, err, sizeof err);
    if (!sconn || !cconn) { printf("  FAIL  conn: %s\n", err); return 1; }

    ok(dyn_aio_tls_attach(a, sv[0], sconn, srv_hs, NULL) == 0,
       "a server engine attaches to an fd");
    ok(dyn_aio_tls_attach(a, sv[1], cconn, cli_hs, NULL) == 0,
       "a client engine attaches to the other end");
    ok(dyn_aio_tls_attach(a, sv[0], sconn, NULL, NULL) != 0,
       "a SECOND attach on the same fd is refused, not silently replacing it");

    srv_seen = (unsigned char *)malloc(PAYLOAD);
    payload = (unsigned char *)malloc(PAYLOAD);
    if (!srv_seen || !payload) { printf("  FAIL  oom\n"); return 1; }
    for (i = 0; i < PAYLOAD; i++)
        payload[i] = (unsigned char)(i * 31 + (i >> 8));

    if (dyn_aio_recv(a, sv[0], 0, 1, srv_data, NULL) < 0 ||
        dyn_aio_recv(a, sv[1], 0, 1, cli_data, NULL) < 0) {
        printf("  FAIL  arm recv\n"); return 1;
    }
    ok(dyn_aio_tls_start(a, sv[1]) == 0, "the client sends its first flight");

    for (spins = 0; spins < 4000 && !(cli_up && srv_up) && !srv_fail; spins++)
        dyn_aio_run(a, 10);
    ok(cli_up && srv_up && !srv_fail, "both ends complete the handshake");

    if (cli_up && srv_up) {
        ok(dyn_aio_send(a, sv[1], payload, PAYLOAD, 0, NULL, NULL) >= 0,
           "one send of the payload through the seam");
        {
            size_t last = 0;
            int stall = 0;
            for (spins = 0; spins < 3000 && srv_bytes < PAYLOAD && !srv_fail; spins++) {
                dyn_aio_run(a, 2);
                if (srv_bytes == last) { if (++stall > 200) break; }
                else { stall = 0; last = srv_bytes; }
            }
        }

        printf("  (delivered %zu bytes in %d callbacks)\n", srv_bytes, srv_calls);
        ok(srv_bytes == PAYLOAD, "every byte arrives from that single send");
        ok(srv_calls > 1, "delivered across several callbacks (informational -- "
                          "the byte count above is what catches a broken drain)");
        ok(srv_bytes == PAYLOAD && memcmp(srv_seen, payload, PAYLOAD) == 0,
           "the plaintext is byte-identical to what was sent");
    }

    dyn_aio_close(a, sv[0]);
    dyn_aio_close(a, sv[1]);
    dyn_aio_free(a);
    dyn_tls_ctx_free(sctx);
    dyn_tls_ctx_free(cctx);
    free(srv_seen); free(payload);
    unlink(cert); unlink(key);

    printf("test_aio_tls: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
#endif
