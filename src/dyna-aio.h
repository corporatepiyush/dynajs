#ifndef DYNAJS_AIO_H
#define DYNAJS_AIO_H

#ifdef CONFIG_NATIVE_MODULES

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "dyna-io.h"

typedef struct dyn_aio dyn_aio_t;

typedef void (*dyn_aio_cb)(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned buf_len, void* udata);

dyn_aio_t* dyn_aio_new(unsigned entries, unsigned disk_workers);
void dyn_aio_free(dyn_aio_t* aio);

int dyn_aio_backend_fd(const dyn_aio_t* aio);

struct dyn_evloop;
struct dyn_evloop* dyn_aio_evloop(dyn_aio_t* aio);

void dyn_aio_drain(void* aio);
int dyn_aio_run(dyn_aio_t* aio, int timeout_ms);

size_t dyn_aio_inflight(const dyn_aio_t* aio);
size_t dyn_aio_queued(const dyn_aio_t* aio, int fd);

int dyn_aio_offload(dyn_aio_t* aio, void (*work)(void*), void (*done)(void*),
    void* arg);

int dyn_aio_pool_register(dyn_aio_t* aio, unsigned n, unsigned sz);

int dyn_aio_listen(dyn_aio_t* aio, const char* host, uint16_t port, int backlog);

int dyn_aio_accept(dyn_aio_t* aio, int listen_fd, dyn_aio_cb cb, void* udata);

int dyn_aio_connect(dyn_aio_t* aio, const char* host, uint16_t port,
    dyn_aio_cb cb, void* udata);

int dyn_aio_recv(dyn_aio_t* aio, int fd, int pool, int multishot,
    dyn_aio_cb cb, void* udata);

#define DYN_AIO_ZC 1
#ifdef CONFIG_TLS
struct dyn_tls_conn;
int dyn_aio_tls_attach(dyn_aio_t* aio, int fd, struct dyn_tls_conn* tls,
    dyn_aio_cb hs_cb, void* hs_udata);
int dyn_aio_tls_start(dyn_aio_t* aio, int fd);
#endif

int dyn_aio_send(dyn_aio_t* aio, int fd, const void* buf, size_t len, int flags,
    dyn_aio_cb cb, void* udata);

int dyn_aio_sendfile(dyn_aio_t* aio, int out_fd, int in_fd, off_t offset,
    size_t len, dyn_aio_cb cb, void* udata);

int dyn_aio_close(dyn_aio_t* aio, int fd);

int dyn_aio_udp_bind(dyn_aio_t* aio, const char* bind_host, uint16_t port);

int dyn_aio_unix_listen(dyn_aio_t* aio, const char* path, int backlog);

int dyn_aio_unix_connect(dyn_aio_t* aio, const char* path,
    dyn_aio_cb cb, void* udata);

typedef void (*dyn_aio_dgram_cb)(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned buf_len, const struct sockaddr* peer,
    unsigned peerlen, void* udata);
int dyn_aio_recvfrom(dyn_aio_t* aio, int fd, dyn_aio_dgram_cb cb, void* udata);
int dyn_aio_sendto(dyn_aio_t* aio, int fd, const void* buf, size_t len,
    const struct sockaddr* peer, unsigned peerlen);

int dyn_aio_watch_fd(dyn_aio_t* aio, int fd,
    void (*cb)(dyn_aio_t* aio, int fd, void* ud), void* ud);
int dyn_aio_unwatch_fd(dyn_aio_t* aio, int fd);

int dyn_aio_openat(dyn_aio_t* aio, int dirfd, const char* path, int flags,
    int mode, dyn_aio_cb cb, void* udata);
int dyn_aio_read(dyn_aio_t* aio, int fd, void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata);
int dyn_aio_write(dyn_aio_t* aio, int fd, const void* buf, size_t len, off_t off,
    dyn_aio_cb cb, void* udata);
int dyn_aio_fsync(dyn_aio_t* aio, int fd, int datasync, dyn_aio_cb cb, void* ud);

int dyn_aio_disk_fd(const dyn_aio_t* aio);
void dyn_aio_disk_drain(dyn_aio_t* aio);

int dyn_aio_set_timer(dyn_aio_t* aio, unsigned period_ms);

int dyn_aio_cancel(dyn_aio_t* aio, dyn_aio_cb cb, void* udata);

int dyn_aio_connect_addr(dyn_aio_t* a, const struct sockaddr_storage* sa,
    socklen_t salen, int fam,
    dyn_aio_cb cb, void* udata);

static inline int dyn_aio_resolve(const char* host, uint16_t port,
    struct sockaddr_storage* ss, socklen_t* slen,
    int* fam)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (res) {
            freeaddrinfo(res);
            res = NULL;
        }
        hints.ai_flags = 0;
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
            return -1;
    }
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *slen = (socklen_t)res->ai_addrlen;
    *fam = res->ai_family;
    freeaddrinfo(res);
    return 0;
}

#define DYN_AIO_MAX_CAND 8
typedef struct dyn_aio_cand {
    struct sockaddr_storage addr[DYN_AIO_MAX_CAND];
    socklen_t len[DYN_AIO_MAX_CAND];
    int n;
    int i;
    int fd;
    dyn_aio_cb cb;
    void* udata;
} dyn_aio_cand_t;

static inline void dyn_aio_cand_map4(struct sockaddr_storage* ss, socklen_t* slen)
{
    const struct sockaddr_in* in4 = (const struct sockaddr_in*)ss;
    struct sockaddr_in6 in6;
    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_port = in4->sin_port;
    ((uint8_t*)&in6.sin6_addr)[10] = 0xff;
    ((uint8_t*)&in6.sin6_addr)[11] = 0xff;
    memcpy((uint8_t*)&in6.sin6_addr + 12, &in4->sin_addr, 4);
    memcpy(ss, &in6, sizeof(in6));
    *slen = (socklen_t)sizeof(in6);
}

static inline int dyn_aio_cand_collect(const char* host, uint16_t port,
    dyn_aio_cand_t* c)
{
    struct addrinfo hints, *res = NULL, *r;
    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    c->n = 0;
    for (r = res; r && c->n < DYN_AIO_MAX_CAND; r = r->ai_next) {
        if (r->ai_addrlen > sizeof(c->addr[0]))
            continue;
        memcpy(&c->addr[c->n], r->ai_addr, r->ai_addrlen);
        c->len[c->n] = (socklen_t)r->ai_addrlen;
        if (r->ai_family == AF_INET)
            dyn_aio_cand_map4(&c->addr[c->n], &c->len[c->n]);
        else if (r->ai_family != AF_INET6)
            continue;
        c->n++;
    }
    freeaddrinfo(res);
    return c->n > 0 ? 0 : -1;
}

static inline int dyn_aio_cand_refresh_fd(dyn_aio_cand_t* c)
{
    int nfd, v6only = 0;

    nfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (nfd < 0)
        return -1;
    fcntl(nfd, F_SETFD, FD_CLOEXEC);
#ifdef IPV6_V6ONLY
    (void)setsockopt(nfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
    dyn_net_set_nonblock(nfd);
    dyn_net_set_nodelay(nfd);
#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(nfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    if (dup2(nfd, c->fd) < 0) {
        close(nfd);
        return -1;
    }
    close(nfd);
    return 0;
}

#endif
#endif
