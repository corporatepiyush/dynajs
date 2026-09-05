/*
 * Proves the dyn_aio completion adapter (kqueue/epoll backend) end to end: a TCP
 * echo server built on dyn_aio, folded into js_std_loop, echoes a real client's
 * bytes on the JS thread. Build/run:
 *   make CONFIG_NATIVE_MODULES=y
 *   cc -Isrc -I. -DCONFIG_NATIVE_MODULES tests/test_aio_echo.c \
 *      libdynajs.a .obj/dyna-aio.o .obj/dyna-evloop.o .obj/dyna-io.o \
 *      -lm -lpthread -o /tmp/test_aio_echo && /tmp/test_aio_echo
 */
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-aio.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 47821

static int g_client_ok;

typedef struct { JSContext *ctx; dyn_aio_t *aio; } srv_t;
/* per-connection state carries the fd (the "handle carries the fd" pattern) */
typedef struct { srv_t *s; int fd; } conn_t;
static conn_t g_conn;

static void on_recv(dyn_aio_t *aio, int res, const uint8_t *buf, unsigned len,
                    void *ud)
{
    conn_t *c = (conn_t *)ud;
    if (res > 0)
        dyn_aio_send(aio, c->fd, buf, len, 0, NULL, NULL); /* echo */
    /* one message proves the accept->recv->send path: stop the loop */
    js_std_set_io_reactor(c->s->ctx, -1, NULL, NULL);
}

static void on_accept(dyn_aio_t *aio, int res, const uint8_t *buf, unsigned len,
                      void *ud)
{
    (void)buf; (void)len;
    if (res >= 0) { /* res = the new connection fd */
        g_conn.s = (srv_t *)ud;
        g_conn.fd = res;
        dyn_aio_recv(aio, res, 0, /*multishot=*/1, on_recv, &g_conn);
    }
}

static void *client_main(void *arg)
{
    int fd;
    struct sockaddr_in sa;
    char rx[16];
    ssize_t n;
    (void)arg;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return NULL; }
    if (write(fd, "ping", 4) != 4) { close(fd); return NULL; }
    n = read(fd, rx, sizeof(rx));
    if (n == 4 && memcmp(rx, "ping", 4) == 0)
        g_client_ok = 1;
    close(fd);
    return NULL;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    dyn_aio_t *aio;
    srv_t s;
    pthread_t th;
    int lfd;

    js_std_init_handlers(rt);
    js_std_add_helpers(ctx, 0, NULL);

    aio = dyn_aio_new(256, 0);
    lfd = dyn_aio_listen(aio, "127.0.0.1", PORT, 128);
    if (!aio || lfd < 0) { fprintf(stderr, "listen failed\n"); return 2; }
    s.ctx = ctx; s.aio = aio;
    dyn_aio_accept(aio, lfd, on_accept, &s);
    js_std_set_io_reactor(ctx, dyn_aio_backend_fd(aio), dyn_aio_drain, aio);

    pthread_create(&th, NULL, client_main, NULL);
    js_std_loop(ctx);
    pthread_join(th, NULL);

    dyn_aio_close(aio, lfd);
    dyn_aio_free(aio);
    printf("client_ok=%d (expect 1)\n", g_client_ok);
    return g_client_ok ? 0 : 1;
}
