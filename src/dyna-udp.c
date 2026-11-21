#include "dyna-udp.h"
#include "core/dyn-common.h"

#ifdef CONFIG_NATIVE_MODULES

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__) && defined(UDP_SEGMENT)
#define DYN_UDPB_HAVE_GSO 1
#endif
#if defined(__linux__) && defined(UDP_GRO)
#define DYN_UDPB_HAVE_GRO 1
#endif

#define DYN_UDPB_CMSG 64

typedef struct {
    uint8_t* buf;
    uint32_t cap;
} udp_spare_t;

struct dyn_udpb_batch {
    dyn_aio_t* aio;
    int fd;
    int in_cb;
    uint8_t closing;
    uint8_t paused;
    uint8_t starved;
    uint8_t watched;
    uint8_t gro;
    dyn_udpb_ops_t ops;
    void* ud;
    unsigned nspare;
    udp_spare_t spare[DYN_UDPB_BATCH];
    dyn_udpb_stats_t st;
};

static void udp_free(dyn_udpb_batch_t* u)
{
    if (u->nspare && u->ops.release) {
        dyn_udpb_msg_t m[DYN_UDPB_BATCH];
        unsigned i, n = u->nspare;
        memset(m, 0, n * sizeof(*m));
        for (i = 0; i < n; i++) {
            m[i].buf = u->spare[n - 1 - i].buf;
            m[i].cap = u->spare[n - 1 - i].cap;
        }
        u->ops.release(u->ud, m, n);
    }
    u->nspare = 0;
    free(u);
}

static unsigned udp_fill(dyn_udpb_batch_t* u, dyn_udpb_msg_t* m)
{
    unsigned have = 0, n = u->nspare;
    if (n < DYN_UDPB_BATCH && u->ops.take)
        have = u->ops.take(u->ud, m, DYN_UDPB_BATCH - n);
    while (n) {
        n--;
        m[have].buf = u->spare[n].buf;
        m[have].cap = u->spare[n].cap;
        have++;
    }
    u->nspare = 0;
    return have;
}

static void udp_keep(dyn_udpb_batch_t* u, const dyn_udpb_msg_t* m, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        u->spare[i].buf = m[n - 1 - i].buf;
        u->spare[i].cap = m[n - 1 - i].cap;
    }
    u->nspare = n;
}

#ifdef __linux__
static int udp_round(dyn_udpb_batch_t* u, dyn_udpb_msg_t* m, unsigned have, int* err)
{
    struct mmsghdr h[DYN_UDPB_BATCH];
    struct iovec iov[DYN_UDPB_BATCH];
    unsigned char cm[DYN_UDPB_BATCH][DYN_UDPB_CMSG];
    unsigned i;
    int k;

    for (i = 0; i < have; i++) {
        iov[i].iov_base = m[i].buf;
        iov[i].iov_len = m[i].cap;
        memset(&h[i], 0, sizeof(h[i]));
        h[i].msg_hdr.msg_name = &m[i].addr;
        h[i].msg_hdr.msg_namelen = sizeof(m[i].addr);
        h[i].msg_hdr.msg_iov = &iov[i];
        h[i].msg_hdr.msg_iovlen = 1;
        if (u->gro) {
            h[i].msg_hdr.msg_control = cm[i];
            h[i].msg_hdr.msg_controllen = sizeof(cm[i]);
        }
    }
    do
        k = recvmmsg(u->fd, h, have, MSG_DONTWAIT, NULL);
    while (k < 0 && errno == EINTR);
    if (k < 0) {
        *err = errno;
        return -1;
    }
    for (i = 0; i < (unsigned)k; i++) {
        m[i].len = h[i].msg_len;
        m[i].addr_len = h[i].msg_hdr.msg_namelen;
        m[i].truncated = (h[i].msg_hdr.msg_flags & MSG_TRUNC) != 0;
        m[i].gso = 0;
#ifdef DYN_UDPB_HAVE_GRO
        if (u->gro) {
            struct cmsghdr* c;
            for (c = CMSG_FIRSTHDR(&h[i].msg_hdr); c; c = CMSG_NXTHDR(&h[i].msg_hdr, c))
                if (c->cmsg_level == SOL_UDP && c->cmsg_type == UDP_GRO) {
                    int seg;
                    memcpy(&seg, CMSG_DATA(c), sizeof(seg));
                    m[i].gso = (uint16_t)seg;
                }
        }
#endif
    }
    return k;
}
#else
static int udp_round(dyn_udpb_batch_t* u, dyn_udpb_msg_t* m, unsigned have, int* err)
{
    unsigned i;
    for (i = 0; i < have; i++) {
        struct iovec iov;
        struct msghdr h;
        ssize_t r;
        iov.iov_base = m[i].buf;
        iov.iov_len = m[i].cap;
        memset(&h, 0, sizeof(h));
        h.msg_name = &m[i].addr;
        h.msg_namelen = sizeof(m[i].addr);
        h.msg_iov = &iov;
        h.msg_iovlen = 1;
        do
            r = recvmsg(u->fd, &h, MSG_DONTWAIT);
        while (r < 0 && errno == EINTR);
        if (r < 0) {
            if (i == 0) {
                *err = errno;
                return -1;
            }
            break;
        }
        m[i].len = (uint32_t)r;
        m[i].addr_len = h.msg_namelen;
        m[i].truncated = (h.msg_flags & MSG_TRUNC) != 0;
        m[i].gso = 0;
    }
    return (int)i;
}
#endif

static void udp_on_ready(dyn_aio_t* aio, int fd, void* ud);

static int udp_watch(dyn_udpb_batch_t* u, int on)
{
    if (!!on == u->watched)
        return 0;
    u->watched = on ? 1 : 0;
    return on ? dyn_aio_watch_fd(u->aio, u->fd, udp_on_ready, u) : dyn_aio_unwatch_fd(u->aio, u->fd);
}

static void udp_on_ready(dyn_aio_t* aio, int fd, void* ud)
{
    dyn_udpb_batch_t* u = (dyn_udpb_batch_t*)ud;
    dyn_udpb_msg_t m[DYN_UDPB_BATCH];
    unsigned round, delivered = 0;
    (void)aio;
    (void)fd;

    if (u->closing || u->paused)
        return;
    u->st.wakeups++;
    u->in_cb++;
    for (round = 0; round < DYN_UDPB_ROUNDS && !u->closing && !u->paused; round++) {
        unsigned have = udp_fill(u, m), i;
        int err = 0, k;
        if (!have) {
            u->st.rx_starved++;
            u->starved = 1;
            udp_watch(u, 0);
            break;
        }
        k = udp_round(u, m, have, &err);
        if (k < 0) {
            udp_keep(u, m, have);
            if (err != EAGAIN && err != EWOULDBLOCK) {
                u->st.rx_errors++;
                if (u->ops.error)
                    u->ops.error(u->ud, err);
                continue;
            }
            break;
        }
        udp_keep(u, m + k, have - (unsigned)k);
        u->st.rounds++;
        u->st.rx_datagrams += (uint64_t)k;
        for (i = 0; i < (unsigned)k; i++) {
            u->st.rx_bytes += m[i].len;
            u->st.rx_truncated += m[i].truncated;
        }
        if (k && u->ops.batch)
            u->ops.batch(u->ud, m, (unsigned)k);
        delivered += (unsigned)k;
        if ((unsigned)k < have)
            break;
    }
    if (delivered && !u->closing && u->ops.batch_end)
        u->ops.batch_end(u->ud);
    if (--u->in_cb == 0 && u->closing)
        udp_free(u);
}

static int udp_bind(const struct sockaddr* sa, socklen_t sl, const dyn_udpb_opts_t* o)
{
    int fd, on = 1;
    fd = dyn_net_socket_cloexec(sa->sa_family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    dyn_net_set_nonblock(fd);
    if (o && (o->flags & DYN_UDPB_REUSEPORT)) {
#ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }
#ifdef IPV6_V6ONLY
    if (sa->sa_family == AF_INET6) {
        int v6 = (o && (o->flags & DYN_UDPB_V6ONLY)) ? 1 : 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6, sizeof(v6));
    }
#endif
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    if (o && o->rcvbuf > 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &o->rcvbuf, sizeof(o->rcvbuf));
    if (o && o->sndbuf > 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &o->sndbuf, sizeof(o->sndbuf));
    if (bind(fd, sa, sl) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

dyn_udpb_batch_t* dyn_udpb_open(dyn_aio_t* aio, const struct sockaddr* bind_addr,
    socklen_t bind_len, const dyn_udpb_opts_t* opts, const dyn_udpb_ops_t* ops, void* ud)
{
    dyn_udpb_batch_t* u;
    if (!aio || !bind_addr || !ops || !ops->take || !ops->batch
        || (bind_addr->sa_family != AF_INET && bind_addr->sa_family != AF_INET6)) {
        errno = EINVAL;
        return NULL;
    }
    u = (dyn_udpb_batch_t*)calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->aio = aio;
    u->ops = *ops;
    u->ud = ud;
    u->fd = udp_bind(bind_addr, bind_len, opts);
    if (u->fd < 0) {
        int e = errno;
        free(u);
        errno = e;
        return NULL;
    }
#ifdef DYN_UDPB_HAVE_GRO
    if (opts && (opts->flags & DYN_UDPB_GRO)) {
        int on = 1;
        u->gro = setsockopt(u->fd, SOL_UDP, UDP_GRO, &on, sizeof(on)) == 0;
    }
#endif
    if (dyn_aio_watch_fd(aio, u->fd, udp_on_ready, u) < 0) {
        int e = errno;
        close(u->fd);
        free(u);
        errno = e;
        return NULL;
    }
    u->watched = 1;
    return u;
}

int dyn_udpb_fd(const dyn_udpb_batch_t* u)
{
    return u ? u->fd : -1;
}

int dyn_udpb_local(const dyn_udpb_batch_t* u, struct sockaddr_storage* ss, socklen_t* len)
{
    *len = sizeof(*ss);
    return getsockname(u->fd, (struct sockaddr*)ss, len);
}

int dyn_udpb_gso_supported(void)
{
#ifdef DYN_UDPB_HAVE_GSO
    return 1;
#else
    return 0;
#endif
}

#ifdef __linux__
int dyn_udpb_send(dyn_udpb_batch_t* u, const dyn_udpb_msg_t* m, unsigned n, int* err)
{
    struct mmsghdr h[DYN_UDPB_BATCH];
    struct iovec iov[DYN_UDPB_BATCH];
    unsigned char cm[DYN_UDPB_BATCH][CMSG_SPACE(sizeof(uint16_t))];
    unsigned done = 0;

    if (err)
        *err = 0;
    if (!u || u->closing) {
        if (err)
            *err = EBADF;
        return -1;
    }
    while (done < n) {
        unsigned chunk = n - done < DYN_UDPB_BATCH ? n - done : DYN_UDPB_BATCH, i;
        int k;
        for (i = 0; i < chunk; i++) {
            const dyn_udpb_msg_t* s = &m[done + i];
            iov[i].iov_base = s->buf;
            iov[i].iov_len = s->len;
            memset(&h[i], 0, sizeof(h[i]));
            h[i].msg_hdr.msg_name = DYN_UNCONST(&s->addr);
            h[i].msg_hdr.msg_namelen = s->addr_len;
            h[i].msg_hdr.msg_iov = &iov[i];
            h[i].msg_hdr.msg_iovlen = 1;
#ifdef DYN_UDPB_HAVE_GSO
            if (s->gso && s->len > s->gso) {
                struct cmsghdr* c;
                uint16_t seg = s->gso;
                h[i].msg_hdr.msg_control = cm[i];
                h[i].msg_hdr.msg_controllen = sizeof(cm[i]);
                c = CMSG_FIRSTHDR(&h[i].msg_hdr);
                c->cmsg_level = SOL_UDP;
                c->cmsg_type = UDP_SEGMENT;
                c->cmsg_len = CMSG_LEN(sizeof(seg));
                memcpy(CMSG_DATA(c), &seg, sizeof(seg));
            }
#endif
        }
        do
            k = sendmmsg(u->fd, h, chunk, MSG_DONTWAIT | MSG_NOSIGNAL);
        while (k < 0 && errno == EINTR);
        u->st.tx_syscalls++;
        if (k < 0) {
            int e = errno;
            if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS)
                u->st.tx_again++;
            else
                u->st.tx_errors++;
            if (err)
                *err = e;
            break;
        }
        for (i = 0; i < (unsigned)k; i++) {
            u->st.tx_bytes += m[done + i].len;
            u->st.tx_datagrams += (m[done + i].gso && m[done + i].len > m[done + i].gso)
                ? (m[done + i].len + m[done + i].gso - 1) / m[done + i].gso
                : 1;
        }
        done += (unsigned)k;
        if ((unsigned)k < chunk)
            break;
    }
    return (int)done;
}
#else
int dyn_udpb_send(dyn_udpb_batch_t* u, const dyn_udpb_msg_t* m, unsigned n, int* err)
{
    unsigned done = 0;
    if (err)
        *err = 0;
    if (!u || u->closing) {
        if (err)
            *err = EBADF;
        return -1;
    }
    for (; done < n; done++) {
        const dyn_udpb_msg_t* s = &m[done];
        ssize_t r;
        if (s->gso && s->len > s->gso) {
            uint32_t off = 0;
            int failed = 0;
            while (off < s->len) {
                uint32_t seg = s->len - off < s->gso ? s->len - off : s->gso;
                do
                    r = sendto(u->fd, s->buf + off, seg, 0, (const struct sockaddr*)&s->addr, s->addr_len);
                while (r < 0 && errno == EINTR);
                u->st.tx_syscalls++;
                if (r < 0) {
                    failed = errno;
                    break;
                }
                u->st.tx_datagrams++;
                off += seg;
            }
            if (failed) {
                if (failed == EAGAIN || failed == EWOULDBLOCK || failed == ENOBUFS)
                    u->st.tx_again++;
                else
                    u->st.tx_errors++;
                if (err)
                    *err = failed;
                break;
            }
            u->st.tx_bytes += s->len;
            continue;
        }
        do
            r = sendto(u->fd, s->buf, s->len, 0, (const struct sockaddr*)&s->addr, s->addr_len);
        while (r < 0 && errno == EINTR);
        u->st.tx_syscalls++;
        if (r < 0) {
            int e = errno;
            if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS)
                u->st.tx_again++;
            else
                u->st.tx_errors++;
            if (err)
                *err = e;
            break;
        }
        u->st.tx_datagrams++;
        u->st.tx_bytes += s->len;
    }
    return (int)done;
}
#endif

int dyn_udpb_pause(dyn_udpb_batch_t* u, int paused)
{
    if (!u || u->closing)
        return -1;
    if (!!paused == u->paused)
        return 0;
    u->paused = paused ? 1 : 0;
    return udp_watch(u, !u->paused && !u->starved);
}

int dyn_udpb_refill(dyn_udpb_batch_t* u)
{
    if (!u || u->closing)
        return -1;
    if (!u->starved)
        return 0;
    u->starved = 0;
    return udp_watch(u, !u->paused);
}

void dyn_udpb_stats(const dyn_udpb_batch_t* u, dyn_udpb_stats_t* out)
{
    if (u)
        *out = u->st;
    else
        memset(out, 0, sizeof(*out));
}

void dyn_udpb_close(dyn_udpb_batch_t* u)
{
    if (!u || u->closing)
        return;
    u->closing = 1;
    udp_watch(u, 0);
    dyn_aio_close(u->aio, u->fd);
    u->fd = -1;
    if (u->in_cb == 0)
        udp_free(u);
}

#endif
