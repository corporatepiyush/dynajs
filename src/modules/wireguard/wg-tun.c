#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "wg-tun.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <net/if.h>

#if defined(__APPLE__)
#include <net/if_utun.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#elif defined(__linux__)
#include <linux/if_tun.h>
#endif

#include "../../core/dyn-common.h"

struct wg_tun {
    int fd, framing;
    unsigned mtu;
    char name[WG_TUN_NAME_MAX + 1];
    size_t cap;
    wg_tun_stats_t st;
    uint8_t buf[];
};

static int ip_version(const uint8_t* ip, size_t n)
{
    if (n >= 20 && ip[0] >> 4 == 4)
        return 4;
    if (n >= 40 && ip[0] >> 4 == 6)
        return 6;
    return 0;
}

int wg_tun_name_ok(const char* name, int framing)
{
    size_t i, n;
    if (!name)
        return 0;
    n = strlen(name);
    if (!n || n > WG_TUN_NAME_MAX)
        return 0;
    if (framing == WG_TUN_FRAME_AF) {
        if (n < 5 || memcmp(name, "utun", 4) || (name[4] == '0' && n > 5))
            return 0;
        for (i = 4; i < n; i++)
            if (name[i] < '0' || name[i] > '9')
                return 0;
        return strtoul(name + 4, NULL, 10) < 0x7fffffffu;
    }
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

size_t wg_tun_frame_hdr(int framing, const uint8_t* ip, size_t n, uint8_t hdr[WG_TUN_HDR])
{
    int v = ip_version(ip, n);
    if (!v)
        return (size_t)-1;
    if (framing != WG_TUN_FRAME_AF)
        return 0;
    hdr[0] = hdr[1] = hdr[2] = 0;
    hdr[3] = (uint8_t)(v == 4 ? WG_TUN_AF_INET : WG_TUN_AF_INET6);
    return WG_TUN_HDR;
}

int wg_tun_unframe(int framing, const uint8_t* p, size_t n, const uint8_t** ip, size_t* ipn)
{
    int v;
    if (framing == WG_TUN_FRAME_AF) {
        if (n < WG_TUN_HDR || p[0] || p[1] || p[2] || (p[3] != WG_TUN_AF_INET && p[3] != WG_TUN_AF_INET6))
            return -1;
        v = ip_version(p + WG_TUN_HDR, n - WG_TUN_HDR);
        if (v != (p[3] == WG_TUN_AF_INET ? 4 : 6))
            return -1;
        *ip = p + WG_TUN_HDR;
        *ipn = n - WG_TUN_HDR;
        return v;
    }
    v = ip_version(p, n);
    if (!v)
        return -1;
    *ip = p;
    *ipn = n;
    return v;
}

const char* wg_tun_reason(const wg_tun_err_t* e, char* buf, size_t cap)
{
    static const char* const what[] = { "ok",
                                        "tun devices are not supported on this platform",
                                        "invalid argument",
                                        "socket(PF_SYSTEM) for utun",
                                        "ioctl(CTLIOCGINFO) for utun",
                                        "connect to utun control",
                                        "open /dev/net/tun",
                                        "ioctl(TUNSETIFF)",
                                        "reading the interface name",
                                        "fcntl(O_NONBLOCK|FD_CLOEXEC)",
                                        "setting the interface MTU",
                                        "out of memory" };
    const char* w = e->stage >= 0 && e->stage <= WG_TUN_ST_NOMEM ? what[e->stage] : "unknown stage";
    const char* hint = (e->err == EPERM || e->err == EACCES) ? " (needs root or CAP_NET_ADMIN)" : "";
    if (e->stage == WG_TUN_ST_OK || e->stage == WG_TUN_ST_UNSUPPORTED)
        snprintf(buf, cap, "%s", w);
    else
        snprintf(buf, cap, "%s: %s%s", w, strerror(e->err), hint);
    return buf;
}

static wg_tun_t* fail(wg_tun_err_t* e, int stage, int err, int fd)
{
    if (fd >= 0)
        close(fd);
    if (e) {
        e->stage = stage;
        e->err = err;
    }
    return NULL;
}

wg_tun_t* wg_tun_from_fd(int fd, int framing, const char* name, unsigned mtu, wg_tun_err_t* e)
{
    wg_tun_t* t;
    if (fd < 0 || (framing != WG_TUN_FRAME_NONE && framing != WG_TUN_FRAME_AF) || (name && *name && !wg_tun_name_ok(name, framing)) || mtu < WG_TUN_MTU_MIN ||
        mtu > WG_TUN_MTU_MAX)
        return fail(e, WG_TUN_ST_ARG, EINVAL, fd);
    if (dyn_fd_set_nonblock(fd) < 0 || dyn_fd_set_cloexec(fd) < 0)
        return fail(e, WG_TUN_ST_FCNTL, errno, fd);
    if (!(t = (wg_tun_t*)calloc(1, sizeof *t + WG_TUN_HDR + mtu + 1)))
        return fail(e, WG_TUN_ST_NOMEM, ENOMEM, fd);
    t->fd = fd;
    t->framing = framing;
    t->mtu = mtu;
    t->cap = WG_TUN_HDR + mtu + 1;
    if (name)
        snprintf(t->name, sizeof t->name, "%s", name);
    if (e)
        e->stage = e->err = 0;
    return t;
}

#if defined(__APPLE__) || defined(__linux__)
static int set_mtu(const char* name, unsigned mtu)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0), r;
    if (s < 0)
        return -1;
    if (dyn_fd_set_cloexec(s) < 0) {
        close(s);
        return -1;
    }
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
    ifr.ifr_mtu = (int)mtu;
    r = ioctl(s, SIOCSIFMTU, &ifr);
    if (r < 0) {
        int er = errno;
        close(s);
        errno = er;
        return -1;
    }
    close(s);
    return 0;
}
#endif

wg_tun_t* wg_tun_open(const char* name, unsigned mtu, wg_tun_err_t* e)
{
#if defined(__APPLE__)
    struct ctl_info ci;
    struct sockaddr_ctl sc;
    char ifn[IFNAMSIZ];
    socklen_t il = sizeof ifn;
    unsigned long unit = 0;
    int fd;
    if (!mtu)
        mtu = WG_TUN_MTU_DEFAULT;
    if ((name && *name && !wg_tun_name_ok(name, WG_TUN_FRAME_AF)) || mtu < WG_TUN_MTU_MIN || mtu > WG_TUN_MTU_MAX)
        return fail(e, WG_TUN_ST_ARG, EINVAL, -1);
    if (name && *name)
        unit = strtoul(name + 4, NULL, 10) + 1;
    if ((fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)) < 0)
        return fail(e, WG_TUN_ST_SOCKET, errno, -1);
    memset(&ci, 0, sizeof ci);
    snprintf(ci.ctl_name, sizeof ci.ctl_name, "%s", UTUN_CONTROL_NAME);
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0)
        return fail(e, WG_TUN_ST_IOCTL, errno, fd);
    memset(&sc, 0, sizeof sc);
    sc.sc_len = sizeof sc;
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ci.ctl_id;
    sc.sc_unit = (uint32_t)unit;
    if (connect(fd, (struct sockaddr*)&sc, sizeof sc) < 0)
        return fail(e, WG_TUN_ST_CONNECT, errno, fd);
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifn, &il) < 0)
        return fail(e, WG_TUN_ST_IFNAME, errno, fd);
    ifn[sizeof ifn - 1] = 0;
    if (set_mtu(ifn, mtu) < 0)
        return fail(e, WG_TUN_ST_MTU, errno, fd);
    return wg_tun_from_fd(fd, WG_TUN_FRAME_AF, ifn, mtu, e);
#elif defined(__linux__)
    struct ifreq ifr;
    int fd;
    if (!mtu)
        mtu = WG_TUN_MTU_DEFAULT;
    if ((name && *name && !wg_tun_name_ok(name, WG_TUN_FRAME_NONE)) || mtu < WG_TUN_MTU_MIN || mtu > WG_TUN_MTU_MAX)
        return fail(e, WG_TUN_ST_ARG, EINVAL, -1);
    if ((fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK)) < 0)
        return fail(e, WG_TUN_ST_OPEN, errno, -1);
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (name && *name)
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0)
        return fail(e, WG_TUN_ST_SETIFF, errno, fd);
    ifr.ifr_name[IFNAMSIZ - 1] = 0;
    if (set_mtu(ifr.ifr_name, mtu) < 0)
        return fail(e, WG_TUN_ST_MTU, errno, fd);
    return wg_tun_from_fd(fd, WG_TUN_FRAME_NONE, ifr.ifr_name, mtu, e);
#else
    (void)name;
    (void)mtu;
    return fail(e, WG_TUN_ST_UNSUPPORTED, ENOTSUP, -1);
#endif
}

int wg_tun_fd(const wg_tun_t* t)
{
    return t->fd;
}

int wg_tun_framing(const wg_tun_t* t)
{
    return t->framing;
}

unsigned wg_tun_mtu(const wg_tun_t* t)
{
    return t->mtu;
}

const char* wg_tun_name(const wg_tun_t* t)
{
    return t->name;
}

int wg_tun_read(wg_tun_t* t, unsigned max, wg_tun_rx_fn fn, void* ud)
{
    unsigned got = 0;
    while (got < max) {
        const uint8_t* ip;
        size_t ipn;
        ssize_t r = read(t->fd, t->buf, t->cap);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return got ? (int)got : -errno;
        }
        if (r == 0)
            break;
        if (wg_tun_unframe(t->framing, t->buf, (size_t)r, &ip, &ipn) < 0) {
            t->st.rx_drop_frame++;
            continue;
        }
        if (ipn > t->mtu) {
            t->st.rx_drop_big++;
            continue;
        }
        t->st.rx_pkts++;
        t->st.rx_bytes += ipn;
        got++;
        fn(ud, ip, ipn);
    }
    return (int)got;
}

int wg_tun_write(wg_tun_t* t, const uint8_t* ip, size_t n)
{
    uint8_t hdr[WG_TUN_HDR];
    struct iovec v[2];
    size_t hl = wg_tun_frame_hdr(t->framing, ip, n, hdr);
    ssize_t r;
    if (hl == (size_t)-1 || n > t->mtu) {
        t->st.tx_drop_bad++;
        return -EINVAL;
    }
    v[0].iov_base = hdr;
    v[0].iov_len = hl;
    v[1].iov_base = (void*)(uintptr_t)ip;
    v[1].iov_len = n;
    do
        r = hl ? writev(t->fd, v, 2) : write(t->fd, ip, n);
    while (r < 0 && errno == EINTR);
    if (r < 0) {
        int er = errno;
        if (er == EAGAIN || er == EWOULDBLOCK || er == ENOBUFS) {
            t->st.tx_again++;
            return -EAGAIN;
        }
        t->st.tx_err++;
        return -er;
    }
    t->st.tx_pkts++;
    t->st.tx_bytes += n;
    return 0;
}

void wg_tun_stats(const wg_tun_t* t, wg_tun_stats_t* st)
{
    *st = t->st;
}

void wg_tun_close(wg_tun_t* t)
{
    if (!t)
        return;
    if (t->fd >= 0)
        close(t->fd);
    free(t);
}
