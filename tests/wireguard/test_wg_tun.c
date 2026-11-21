/*
 * test_wg_tun.c -- dyna:wireguard S13d tun device (src/modules/wireguard/wg-tun.c). The privileged open is one call
 * per OS; everything else runs on any fd, so both framings are exercised over AF_UNIX datagram socketpairs.
 * Contract pinned here (S13e maps it to the JS TypeError): wg_tun_open fails with {stage, errno}; macOS as non-root
 * fails at WG_TUN_ST_CONNECT with EPERM (probed on Darwin 27), Linux without CAP_NET_ADMIN at OPEN or SETIFF.
 * Framing AF is the utun 4-byte header: 0,0,0,AF with Darwin AF_INET 2 / AF_INET6 30 (<net/if_utun.h>,
 * <sys/socket.h>), matching the IP version nibble. Framing NONE is Linux IFF_NO_PI.
 *  1. Names: utunN (N decimal, no leading zero, < 2^31-1) for AF; 1..15 of [A-Za-z0-9._-], not . or .., for NONE.
 *  2. Frame/unframe vectors: v4/v6 headers, short packets, bad version, nonzero header bytes, family mismatch.
 *  3. Reason strings: stage + strerror + privilege hint on EPERM/EACCES; unsupported and unknown stages.
 *  4. Socketpair round trips, both framings, 20..MTU bytes: exact bytes on the wire both ways; batch read stops at
 *     max and at EAGAIN (returns 0, not an error); oversize (MTU+1) dropped on read and refused on write; bad frames
 *     counted; write EAGAIN when the peer buffer is full.
 *  5. from_fd: argument errors close the fd (ownership), EBADF at the fcntl stage, O_NONBLOCK and FD_CLOEXEC set;
 *     fd count unchanged across 2000 open/close cycles.
 *  6. wg_tun_open: non-root gets the exact stage/errno above with no fd leaked; root: open, name, MTU readback via
 *     SIOCGIFMTU and (Linux) a kernel round trip: address the device, send UDP through it, read it back. Skipped
 *     with a printed reason when not root.
 *  7. The fuzz entry (src/fuzz/fuzz_wg_tun.c) on 20000 random inputs.
 *  8. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks):
 *     tun-names: random names vs a naive rule (utunN near 2^31, leading zeros, dots, bytes outside the set);
 *     tun-frames: frame_hdr/unframe vs the naive rule on random and corrupted headers; tun-reseeded: pair+fuzz.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../src/modules/wireguard/wg-tun.h"
#include "../lib/dyn-fuzzkit.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int fails, checks;
#define CHECK(c, msg)                                          \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            fails++;                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                      \
    } while (0)

static uint64_t rs = 0x2545f4914f6cdd1dULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int nfds(void)
{
    int i, n = 0;
    for (i = 0; i < 1024; i++)
        if (fcntl(i, F_GETFD) != -1)
            n++;
    return n;
}

static size_t mkpkt(uint8_t* p, int v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(rnd() >> 24);
    p[0] = (uint8_t)(v == 4 ? 0x45 : 0x60);
    return n;
}

static void t_names(void)
{
    static const struct {
        const char* n;
        int f, ok;
    } v[] = { { "utun0", 1, 1 },          { "utun12", 1, 1 },     { "utun2147483646", 1, 1 }, { "utun2147483647", 1, 0 }, { "utun", 1, 0 },
              { "utun01", 1, 0 },         { "utunx", 1, 0 },      { "tun0", 1, 0 },           { "utun-1", 1, 0 },         { "wg0", 0, 1 },
              { "a-b_c.d", 0, 1 },        { "abcdefghijklmno", 0, 1 }, { "abcdefghijklmnop", 0, 0 }, { ".", 0, 0 },          { "..", 0, 0 },
              { "", 0, 0 },               { "wg 0", 0, 0 },       { "wg/0", 0, 0 },           { "wg:0", 0, 0 },           { "utun3", 0, 1 } };
    unsigned i, bad = 0;
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        if (wg_tun_name_ok(v[i].n, v[i].f) != v[i].ok) {
            printf("  name %s framing %d\n", v[i].n, v[i].f);
            bad++;
        }
    CHECK(!bad, "interface name rules (utunN for AF; 1..15 of [A-Za-z0-9._-], not . or .., for NONE)");
    CHECK(!wg_tun_name_ok(NULL, 0) && !wg_tun_name_ok(NULL, 1), "NULL name rejected");
}

static void t_frames(void)
{
    uint8_t p[64], h[WG_TUN_HDR], f[80];
    const uint8_t* ip;
    size_t n;
    mkpkt(p, 4, 20);
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_AF, p, 20, h) == 4 && !h[0] && !h[1] && !h[2] && h[3] == 2, "utun header for IPv4 is 00 00 00 02");
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_NONE, p, 20, h) == 0, "IFF_NO_PI: no header");
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_AF, p, 19, h) == (size_t)-1, "IPv4 shorter than 20 refused");
    mkpkt(p, 6, 40);
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_AF, p, 40, h) == 4 && h[3] == 30, "utun header for IPv6 is 00 00 00 1e (Darwin AF_INET6 30)");
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_NONE, p, 39, h) == (size_t)-1, "IPv6 shorter than 40 refused");
    p[0] = 0x55;
    CHECK(wg_tun_frame_hdr(WG_TUN_FRAME_NONE, p, 40, h) == (size_t)-1, "version 5 refused");
    mkpkt(f + 4, 4, 30);
    f[0] = f[1] = f[2] = 0;
    f[3] = 2;
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 34, &ip, &n) == 4 && ip == f + 4 && n == 30, "AF unframe v4");
    f[3] = 30;
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 34, &ip, &n) < 0, "AF 30 with an IPv4 packet: family mismatch dropped");
    mkpkt(f + 4, 6, 48);
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 52, &ip, &n) == 6 && n == 48, "AF unframe v6");
    f[2] = 1;
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 52, &ip, &n) < 0, "nonzero high header bytes dropped");
    f[2] = 0;
    f[3] = 24;
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 52, &ip, &n) < 0, "unknown family dropped");
    CHECK(wg_tun_unframe(WG_TUN_FRAME_AF, f, 3, &ip, &n) < 0, "shorter than the header dropped");
    CHECK(wg_tun_unframe(WG_TUN_FRAME_NONE, f + 4, 48, &ip, &n) == 6 && ip == f + 4, "NONE unframe v6");
    f[4] = 0x75;
    CHECK(wg_tun_unframe(WG_TUN_FRAME_NONE, f + 4, 48, &ip, &n) < 0, "NONE: version 7 dropped");
}

static void t_reason(void)
{
    char b[160];
    wg_tun_err_t e = { WG_TUN_ST_CONNECT, EPERM };
    CHECK(!strcmp(wg_tun_reason(&e, b, sizeof b), "connect to utun control: Operation not permitted (needs root or CAP_NET_ADMIN)"), "reason: stage, strerror, privilege hint");
    e.stage = WG_TUN_ST_OPEN;
    e.err = ENOENT;
    CHECK(!strcmp(wg_tun_reason(&e, b, sizeof b), "open /dev/net/tun: No such file or directory"), "reason without hint for ENOENT");
    e.stage = WG_TUN_ST_UNSUPPORTED;
    e.err = ENOTSUP;
    CHECK(!strcmp(wg_tun_reason(&e, b, sizeof b), "tun devices are not supported on this platform"), "reason: unsupported");
    e.stage = 99;
    e.err = EINVAL;
    CHECK(!strncmp(wg_tun_reason(&e, b, sizeof b), "unknown stage: ", 15), "reason: unknown stage");
    e.stage = WG_TUN_ST_SETIFF;
    e.err = EACCES;
    wg_tun_reason(&e, b, 12);
    CHECK(strlen(b) == 11, "reason truncates to the buffer");
    CHECK(!strcmp(wg_tun_reason(&e, b, sizeof b), "ioctl(TUNSETIFF): Permission denied (needs root or CAP_NET_ADMIN)"), "EACCES also gets the privilege hint");
}

static unsigned rx_n;
static size_t rx_len[256];
static uint8_t rx_last[2048];
static void on_rx(void* ud, const uint8_t* ip, size_t n)
{
    (void)ud;
    if (rx_n < 256)
        rx_len[rx_n] = n;
    rx_n++;
    memcpy(rx_last, ip, n);
}

static void t_pair(int fr)
{
    int s[2], i, r, bad = 0;
    uint8_t p[1600], q[1700];
    wg_tun_t* t;
    wg_tun_stats_t st;
    wg_tun_err_t e;
    size_t sizes[] = { 20, 40, 64, 576, 1279, 1420 }, k, hl = fr ? 4 : 0;
    socketpair(AF_UNIX, SOCK_DGRAM, 0, s);
    {
        int big = 1 << 20;
        setsockopt(s[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        setsockopt(s[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    }
    t = wg_tun_from_fd(s[0], fr, fr ? "utun3" : "wg0", 1420, &e);
    CHECK(t && e.stage == WG_TUN_ST_OK && wg_tun_fd(t) == s[0] && wg_tun_framing(t) == fr && wg_tun_mtu(t) == 1420 && !strcmp(wg_tun_name(t), fr ? "utun3" : "wg0"), "from_fd");
    CHECK((fcntl(s[0], F_GETFL) & O_NONBLOCK) && (fcntl(s[0], F_GETFD) & FD_CLOEXEC), "fd made non-blocking and close-on-exec");
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int v = sizes[k] >= 40 && (k & 1) ? 6 : 4;
        mkpkt(p, v, sizes[k]);
        r = wg_tun_write(t, p, sizes[k]);
        if (r || recv(s[1], q, sizeof q, 0) != (ssize_t)(sizes[k] + hl) || memcmp(q + hl, p, sizes[k]) || (fr && (q[0] || q[1] || q[2] || q[3] != (v == 4 ? 2 : 30))))
            bad++;
        memcpy(q + hl, p, sizes[k]);
        if (fr) {
            q[0] = q[1] = q[2] = 0;
            q[3] = (uint8_t)(v == 4 ? 2 : 30);
        }
        send(s[1], q, sizes[k] + hl, 0);
        rx_n = 0;
        if (wg_tun_read(t, 64, on_rx, NULL) != 1 || rx_n != 1 || rx_len[0] != sizes[k] || memcmp(rx_last, p, sizes[k]))
            bad++;
    }
    CHECK(!bad, "round trip 20..1420 B, v4 and v6: exact bytes on the wire both ways");
    rx_n = 0;
    CHECK(wg_tun_read(t, 64, on_rx, NULL) == 0 && rx_n == 0, "nothing pending: read returns 0 (EAGAIN is not an error)");
    mkpkt(q + hl, 4, 100);
    if (fr)
        q[3] = 2;
    for (i = 0; i < 100; i++)
        send(s[1], q, 100 + hl, 0);
    rx_n = 0;
    r = wg_tun_read(t, 64, on_rx, NULL);
    CHECK(r == 64 && rx_n == 64, "batch read stops at max");
    r = wg_tun_read(t, 64, on_rx, NULL);
    CHECK(r == 36 && rx_n == 100, "then drains the rest and stops at EAGAIN");
    mkpkt(p, 4, 1421);
    CHECK(wg_tun_write(t, p, 1421) == -EINVAL, "write above the MTU refused");
    memcpy(q + hl, p, 1421);
    send(s[1], q, 1421 + hl, 0);
    mkpkt(p, 4, 1600);
    memcpy(q + hl, p, 1600);
    send(s[1], q, 1600 + hl, 0);
    q[hl] = 0x35;
    send(s[1], q, 60 + hl, 0);
    rx_n = 0;
    r = wg_tun_read(t, 64, on_rx, NULL);
    wg_tun_stats(t, &st);
    CHECK(r == 0 && rx_n == 0 && st.rx_drop_big == 2 && st.rx_drop_frame == 1, "MTU+1 and truncated-oversize reads dropped as big, bad version as frame");
    p[0] = 0x15;
    CHECK(wg_tun_write(t, p, 60) == -EINVAL, "write of a non-IP packet refused");
    mkpkt(p, 4, 1400);
    for (i = 0; i < 100000 && (r = wg_tun_write(t, p, 1400)) == 0; i++)
        ;
    wg_tun_stats(t, &st);
    CHECK(r == -EAGAIN && st.tx_again == 1 && st.tx_drop_bad == 2 && st.rx_pkts == 106, "peer buffer full: -EAGAIN, counted");
    wg_tun_close(t);
    CHECK(fcntl(s[0], F_GETFD) == -1, "close closes the fd");
    close(s[1]);
}

static void t_from_fd(void)
{
    int base = nfds(), s[2], i, leak = 0;
    wg_tun_err_t e;
    socketpair(AF_UNIX, SOCK_DGRAM, 0, s);
    CHECK(!wg_tun_from_fd(s[0], 2, NULL, 1420, &e) && e.stage == WG_TUN_ST_ARG && e.err == EINVAL && fcntl(s[0], F_GETFD) == -1, "bad framing: ARG, fd closed");
    CHECK(!wg_tun_from_fd(s[1], 1, "wg0", 1420, &e) && e.stage == WG_TUN_ST_ARG && fcntl(s[1], F_GETFD) == -1, "bad name for the framing: ARG, fd closed");
    socketpair(AF_UNIX, SOCK_DGRAM, 0, s);
    CHECK(!wg_tun_from_fd(s[0], 0, NULL, 1279, &e) && e.stage == WG_TUN_ST_ARG, "MTU below 1280 refused (IPv6 minimum)");
    CHECK(!wg_tun_from_fd(s[1], 0, NULL, 65536, &e) && e.stage == WG_TUN_ST_ARG, "MTU above 65535 refused");
    CHECK(!wg_tun_from_fd(-1, 0, NULL, 1420, &e) && e.stage == WG_TUN_ST_ARG, "negative fd refused");
    CHECK(!wg_tun_from_fd(900, 0, NULL, 1420, &e) && e.stage == WG_TUN_ST_FCNTL && e.err == EBADF, "closed fd: FCNTL stage, EBADF");
    CHECK(!wg_tun_from_fd(s[0], 0, NULL, 1420, NULL), "NULL error out-param allowed");
    CHECK(nfds() == base, "every failing from_fd closed its fd");
    for (i = 0; i < 2000; i++) {
        wg_tun_t* t;
        socketpair(AF_UNIX, SOCK_DGRAM, 0, s);
        t = wg_tun_from_fd(s[0], i & 1, NULL, 1420 + (unsigned)i, NULL);
        if (!t)
            leak++;
        wg_tun_close(t);
        close(s[1]);
    }
    wg_tun_close(NULL);
    CHECK(!leak && nfds() == base, "2000 open/close cycles: fd count unchanged");
}

static void t_open(void)
{
    int base = nfds();
    wg_tun_err_t e;
    wg_tun_t* t;
    char b[160];
#if defined(__APPLE__)
    CHECK(!wg_tun_open("utun01", 0, &e) && e.stage == WG_TUN_ST_ARG, "open: bad utun name refused before any syscall");
    CHECK(!wg_tun_open("wg0", 0, &e) && e.stage == WG_TUN_ST_ARG, "open on macOS: only utunN names");
#elif defined(__linux__)
    CHECK(!wg_tun_open("bad/name", 0, &e) && e.stage == WG_TUN_ST_ARG, "open: bad name refused before any syscall");
#endif
    CHECK(!wg_tun_open(NULL, 1000, &e) && e.stage == WG_TUN_ST_ARG, "open: MTU below 1280 refused");
    if (geteuid() != 0) {
        t = wg_tun_open(NULL, 0, &e);
#if defined(__APPLE__)
        CHECK(!t && e.stage == WG_TUN_ST_CONNECT && e.err == EPERM, "non-root macOS: CONNECT stage, EPERM");
#elif defined(__linux__)
        CHECK(!t && ((e.stage == WG_TUN_ST_OPEN && (e.err == EACCES || e.err == ENOENT || e.err == EPERM)) || (e.stage == WG_TUN_ST_SETIFF && e.err == EPERM)),
              "non-root Linux: OPEN (EACCES/ENOENT) or SETIFF (EPERM)");
#else
        CHECK(!t && e.stage == WG_TUN_ST_UNSUPPORTED, "unsupported platform");
#endif
        CHECK(nfds() == base, "failed open leaked no fd");
        printf("  open skipped (not root): %s\n", wg_tun_reason(&e, b, sizeof b));
        return;
    }
    t = wg_tun_open(NULL, 0, &e);
    CHECK(t != NULL, wg_tun_reason(&e, b, sizeof b));
    if (!t)
        return;
    {
        struct ifreq ifr;
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", wg_tun_name(t));
        CHECK(ioctl(s, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu == 1420 && wg_tun_mtu(t) == 1420, "root: interface MTU set to 1420");
        printf("  root: opened %s\n", wg_tun_name(t));
#if defined(__linux__)
        {
            struct sockaddr_in a;
            int u = socket(AF_INET, SOCK_DGRAM, 0), tries;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            inet_pton(AF_INET, "10.231.0.1", &a.sin_addr);
            memcpy(&ifr.ifr_addr, &a, sizeof a);
            CHECK(ioctl(s, SIOCSIFADDR, &ifr) == 0, "root: address 10.231.0.1");
            inet_pton(AF_INET, "255.255.255.0", &a.sin_addr);
            memcpy(&ifr.ifr_netmask, &a, sizeof a);
            ioctl(s, SIOCSIFNETMASK, &ifr);
            ioctl(s, SIOCGIFFLAGS, &ifr);
            ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
            CHECK(ioctl(s, SIOCSIFFLAGS, &ifr) == 0, "root: interface up");
            inet_pton(AF_INET, "10.231.0.2", &a.sin_addr);
            a.sin_port = htons(9);
            rx_n = 0;
            for (tries = 0; tries < 50 && !rx_n; tries++) {
                sendto(u, "dyna-tun", 8, 0, (struct sockaddr*)&a, sizeof a);
                usleep(20000);
                while (wg_tun_read(t, 64, on_rx, NULL) > 0 && !(rx_last[0] == 0x45 && rx_last[9] == 17))
                    rx_n = 0;
            }
            CHECK(rx_n && rx_last[9] == 17 && !memcmp(rx_last + 28, "dyna-tun", 8), "root Linux: kernel routes a UDP datagram into the tun, read back intact");
            close(u);
        }
#endif
        close(s);
    }
    wg_tun_close(t);
    CHECK(nfds() == base, "root: close released the fd");
}

static void t_fuzz(void)
{
    static uint8_t b[4096];
    int i;
    for (i = 0; i < 20000; i++) {
        size_t n = rnd() % 1600, k;
        for (k = 0; k < n; k++)
            b[k] = (uint8_t)rnd();
        if (n > 8 && (i & 3) == 0) {
            b[0] = (uint8_t)(rnd() & 7);
            b[1] = 0;
            b[2] = (uint8_t)(n - 3 > 255 ? 255 : n - 3);
            b[3] = (rnd() & 1) ? 0x45 : 0x60;
        }
        LLVMFuzzerTestOneInput(b, n);
    }
    CHECK(1, "fuzz entry: 20000 inputs without a sanitizer report or invariant abort");
}

static int naive_name(const char* s, size_t n, int framing)
{
    size_t i;
    if (n == 0 || n > 15)
        return 0;
    if (framing == WG_TUN_FRAME_AF) {
        uint64_t v = 0;
        if (n < 5 || strncmp(s, "utun", 4) || (s[4] == '0' && n > 5))
            return 0;
        for (i = 4; i < n; i++) {
            if (s[i] < '0' || s[i] > '9')
                return 0;
            v = v > 0xffffffffffULL ? v : v * 10 + (uint64_t)(s[i] - '0');
        }
        return v < 0x7fffffffULL;
    }
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (i = 0; i < n; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.", s[i]) || !s[i])
            return 0;
    return 1;
}

static void p_tun_names(void)
{
    static const char al[] = "utn0123456789.-_aAzZ/ :\x7f\x80";
    fk_rng_t r;
    unsigned i, n, bad = 0, yes = 0;
    if (!fk_only("tun-names"))
        return;
    r = fk_stream("tun-names");
    n = fk_iters(200000);
    for (i = 0; i < n; i++) {
        char s[24];
        size_t len = (size_t)fk_below(&r, 19), k;
        int fr = (int)fk_below(&r, 2), got, want;
        unsigned mode = (unsigned)fk_below(&r, 4);
        for (k = 0; k < len; k++)
            s[k] = al[fk_below(&r, sizeof al - 1)];
        if (mode == 1 && len >= 4)
            memcpy(s, "utun", 4);
        if (mode == 2) {
            len = 4 + (size_t)fk_below(&r, 12);
            memcpy(s, "utun", 4);
            for (k = 4; k < len; k++)
                s[k] = (char)('0' + fk_below(&r, 10));
            if (fk_chance(&r, 30) && len >= 14) {
                static const char* edge[] = { "2147483646", "2147483647", "2147483648", "4294967295", "4294967296", "0000000001" };
                memcpy(s + 4, edge[fk_below(&r, 6)], 10);
                len = 14;
            }
        }
        if (mode == 3) {
            len = (size_t)fk_below(&r, 3);
            memset(s, '.', len);
        }
        s[len] = 0;
        want = naive_name(s, len, fr);
        got = wg_tun_name_ok(s, fr);
        yes += want;
        if (got != want) {
            if (bad < 4)
                printf("  tun-names: \"%s\" framing %d got %d want %d\n", s, fr, got, want);
            bad++;
        }
    }
    CHECK(bad == 0, "tun-names: random names (utunN edges near 2^31, leading zeros, dots, bytes outside the set, 0..18 chars) match the naive rule");
    if (n >= 1000)
        CHECK(yes > n / 20 && yes < n - n / 20, "tun-names: both verdicts well represented");
}

static void p_tun_frames(void)
{
    fk_rng_t r;
    static uint8_t p[2048], f[2052];
    unsigned i, n, bad = 0;
    if (!fk_only("tun-frames"))
        return;
    r = fk_stream("tun-frames");
    n = fk_iters(200000);
    for (i = 0; i < n; i++) {
        size_t len = (size_t)fk_below(&r, fk_chance(&r, 80) ? 64 : 2048), ipn = 0, hl;
        int fr = (int)fk_below(&r, 2), v, want, got;
        uint8_t h[WG_TUN_HDR];
        const uint8_t* ip = NULL;
        fk_bytes(&r, p, len);
        if (len && fk_chance(&r, 70))
            p[0] = (uint8_t)((fk_chance(&r, 50) ? 0x40 : 0x60) | (p[0] & 15));
        v = len >= 20 && p[0] >> 4 == 4 ? 4 : len >= 40 && p[0] >> 4 == 6 ? 6 : 0;
        hl = wg_tun_frame_hdr(fr, p, len, h);
        if (hl != (!v ? (size_t)-1 : fr == WG_TUN_FRAME_AF ? 4u : 0u) || (v && fr == WG_TUN_FRAME_AF && (h[0] | h[1] | h[2] || h[3] != (v == 4 ? 2 : 30))))
            bad++;
        if (fr == WG_TUN_FRAME_AF) {
            f[0] = f[1] = f[2] = 0;
            f[3] = v == 6 ? 30 : 2;
            if (fk_chance(&r, 25))
                f[fk_below(&r, 4)] = (uint8_t)fk_next(&r);
            memcpy(f + 4, p, len);
            {
                size_t fl = fk_chance(&r, 3) ? (size_t)fk_below(&r, 4) : len + 4;
                want = fl < 4 || f[0] || f[1] || f[2] || (f[3] != 2 && f[3] != 30) || !v || v != (f[3] == 2 ? 4 : 6) ? -1 : v;
                got = wg_tun_unframe(fr, f, fl, &ip, &ipn);
                if (got != want || (got >= 0 && (ip != f + 4 || ipn != len)))
                    bad++;
            }
        } else {
            want = v ? v : -1;
            got = wg_tun_unframe(fr, p, len, &ip, &ipn);
            if (got != want || (got >= 0 && (ip != p || ipn != len)))
                bad++;
        }
    }
    CHECK(bad == 0, "tun-frames: frame_hdr and unframe on random packets and corrupted utun headers equal the naive rule; frame then unframe returns the same bytes");
}

static void p_tun_reseeded(void)
{
    fk_rng_t r;
    if (!fk_only("tun-reseeded"))
        return;
    r = fk_stream("tun-reseeded");
    rs = fk_next(&r) | 1;
    t_pair(WG_TUN_FRAME_NONE);
    t_pair(WG_TUN_FRAME_AF);
    t_fuzz();
}

int main(void)
{
    fk_init("test_wg_tun");
    if (fk_legacy()) {
        t_names();
        t_frames();
        t_reason();
        t_pair(WG_TUN_FRAME_NONE);
        t_pair(WG_TUN_FRAME_AF);
        t_from_fd();
        t_open();
        t_fuzz();
    }
    p_tun_names();
    p_tun_frames();
    p_tun_reseeded();
    printf("test_wg_tun: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails != 0;
}
