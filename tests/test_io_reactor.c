/*
 * Proves the reactor<->js_std_loop seam: a dyn_evloop reactor folded into the
 * JS thread's event loop dispatches its fd completion ON THE JS THREAD, and the
 * loop exits once the reactor is unregistered. Build/run:
 *   make CONFIG_NATIVE_MODULES=y
 *   cc -Isrc tests/test_io_reactor.c libdynajs.a .obj/dyna-evloop.o \
 *      -lm -lpthread -o /tmp/test_io_reactor && /tmp/test_io_reactor
 */
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-evloop.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static int g_fired;

typedef struct {
    JSContext *ctx;
    dyn_evloop_t *lp;
} drain_ctx_t;

static void on_ready(dyn_evloop_t *lp, int fd, int events, void *udata)
{
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    (void)lp; (void)events; (void)udata;
    if (n > 0)
        g_fired += (int)n;
}

static void drain(void *udata)
{
    drain_ctx_t *d = (drain_ctx_t *)udata;
    dyn_evloop_poll(d->lp, 0); /* reap ALL ready fds, dispatch their callbacks */
    if (g_fired)               /* done: unregister so js_std_loop can exit */
        js_std_set_io_reactor(d->ctx, -1, NULL, NULL);
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    dyn_evloop_t *lp;
    drain_ctx_t d;
    int sv[2];

    js_std_init_handlers(rt);
    js_std_add_helpers(ctx, 0, NULL); /* arms os_poll_func = js_os_poll */

    lp = dyn_evloop_new();
    if (!lp || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        fprintf(stderr, "setup failed\n");
        return 2;
    }
    dyn_evloop_add(lp, sv[0], DYN_EV_READ, on_ready, NULL);
    if (write(sv[1], "hello", 5) != 5) { fprintf(stderr, "write failed\n"); return 2; }

    d.ctx = ctx;
    d.lp = lp;
    js_std_set_io_reactor(ctx, dyn_evloop_backend_fd(lp), drain, &d);

    js_std_loop(ctx); /* must drain the reactor on the JS thread, then exit */

    printf("g_fired=%d (expect 5)\n", g_fired);
    return g_fired == 5 ? 0 : 1;
}
