#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-aio.h"

void js_std_set_io_reactor_wait(JSContext *ctx,
                                int (*wait)(void *udata, int timeout_ms));

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char HELLO[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";
#define HELLO_LEN (sizeof(HELLO) - 1)

typedef struct { dyn_aio_t *aio; int fd; } conn_t;

static int count_requests(const uint8_t *buf, unsigned len)
{
    int n = 0;
    unsigned i;
    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            { n++; i += 3; }
    return n;
}

static void on_recv(dyn_aio_t *aio, int res, const uint8_t *buf, unsigned len,
                    void *ud)
{
    conn_t *c = (conn_t *)ud;
    int reqs, i;
    if (res <= 0) {
        dyn_aio_close(aio, c->fd);
        free(c);
        return;
    }
    reqs = count_requests(buf, len);
    for (i = 0; i < reqs; i++)
        dyn_aio_send(aio, c->fd, HELLO, HELLO_LEN, 0, NULL, NULL);
}

static void on_accept(dyn_aio_t *aio, int res, const uint8_t *buf, unsigned len,
                      void *ud)
{
    conn_t *c;
    (void)buf; (void)len; (void)ud;
    if (res < 0)
        return;
    c = (conn_t *)malloc(sizeof(*c));
    c->aio = aio;
    c->fd = res;
    dyn_aio_recv(aio, res, 0,  1, on_recv, c);
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    dyn_aio_t *aio;
    int lfd, port = argc > 1 ? atoi(argv[1]) : 8080;

    signal(SIGPIPE, SIG_IGN);
    js_std_init_handlers(rt);
    js_std_add_helpers(ctx, 0, NULL);

    aio = dyn_aio_new(4096, 0);
    lfd = dyn_aio_listen(aio, "0.0.0.0", (uint16_t)port, 1024);
    if (!aio || lfd < 0) { fprintf(stderr, "listen(%d) failed\n", port); return 2; }
    dyn_aio_accept(aio, lfd, on_accept, NULL);
    js_std_set_io_reactor(ctx, dyn_aio_backend_fd(aio), dyn_aio_drain, aio);
    js_std_set_io_reactor_wait(
        ctx, (int (*)(void *, int))dyn_aio_run);

    { const char *pf = getenv("DYNA_BENCH_PORT_FILE");
      if (pf) { FILE *f = fopen(pf, "w"); if (f) { fprintf(f, "%d", port); fclose(f); } } }
    fprintf(stderr, "dynajs dyn_aio http hello on :%d\n", port);
    js_std_loop(ctx);
    dyn_aio_free(aio);
    return 0;
}
