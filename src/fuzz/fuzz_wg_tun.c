#include "modules/wireguard/wg-tun.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_wg_tun: oracle failed at line %d\n", line);
    abort();
}

enum { FZ_MTU = 1280, QN = 64 };

static wg_tun_t* fz_t[2];
static int fz_peer[2] = { -1, -1 };
static volatile size_t fz_sink;

typedef struct {
    size_t n[QN];
    uint8_t b[QN][FZ_MTU];
    unsigned head, cnt;
} q_t;

static q_t fz_q[2];
static int fz_cur;

static int naive_ver(const uint8_t* p, size_t n)
{
    if (n >= 20 && p[0] >> 4 == 4)
        return 4;
    if (n >= 40 && p[0] >> 4 == 6)
        return 6;
    return 0;
}

static int naive_name(const char* s, int framing)
{
    size_t n = strlen(s), i;
    if (!n || n > 15)
        return 0;
    if (framing) {
        unsigned long long v = 0;
        if (n < 5 || strncmp(s, "utun", 4) || (s[4] == '0' && n > 5))
            return 0;
        for (i = 4; i < n; i++) {
            if (s[i] < '0' || s[i] > '9')
                return 0;
            v = v * 10 + (unsigned)(s[i] - '0');
        }
        return v < 0x7fffffffULL;
    }
    if (!strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (i = 0; i < n; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.", s[i]))
            return 0;
    return 1;
}

static void fz_rx(void* ud, const uint8_t* ip, size_t n)
{
    q_t* q = &fz_q[fz_cur];
    (void)ud;
    if (!q->cnt || q->n[q->head] != n || memcmp(q->b[q->head], ip, n))
        fz_die(__LINE__);
    q->head = (q->head + 1) % QN;
    q->cnt--;
    fz_sink += n;
}

static void fz_setup(void)
{
    int f, s[2];
    for (f = 0; f < 2; f++) {
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, s) < 0)
            fz_die(__LINE__);
        fz_t[f] = wg_tun_from_fd(s[0], f, f ? "utun7" : "wg0", FZ_MTU, NULL);
        fz_peer[f] = s[1];
        if (!fz_t[f])
            fz_die(__LINE__);
    }
}

static void fz_frame(int f, const uint8_t* p, size_t n)
{
    const uint8_t* ip = NULL;
    size_t ipn = 0;
    uint8_t h[WG_TUN_HDR];
    int v = wg_tun_unframe(f, p, n, &ip, &ipn), want;
    size_t hl = wg_tun_frame_hdr(f, p, n, h);
    if (f) {
        int fam = n >= 4 && !p[0] && !p[1] && !p[2] ? p[3] : -1;
        int iv = n >= 4 ? naive_ver(p + 4, n - 4) : 0;
        want = (fam == 2 && iv == 4) || (fam == 30 && iv == 6) ? iv : -1;
    } else
        want = naive_ver(p, n) ? naive_ver(p, n) : -1;
    if ((v < 0) != (want < 0) || (v >= 0 && (v != want || ip != p + (f ? 4 : 0) || ipn != n - (f ? 4u : 0u))))
        fz_die(__LINE__);
    if (!naive_ver(p, n) != (hl == (size_t)-1))
        fz_die(__LINE__);
    if (hl != (size_t)-1 && (hl != (f ? 4u : 0u) || (f && (h[0] || h[1] || h[2] || h[3] != (naive_ver(p, n) == 4 ? 2 : 30)))))
        fz_die(__LINE__);
}

static void push(int f, const uint8_t* ip, size_t n)
{
    q_t* q = &fz_q[f];
    if (q->cnt == QN)
        fz_die(__LINE__);
    memcpy(q->b[(q->head + q->cnt) % QN], ip, n);
    q->n[(q->head + q->cnt) % QN] = n;
    q->cnt++;
}

static void drain(int f)
{
    int r;
    fz_cur = f;
    do
        r = wg_tun_read(fz_t[f], 64, fz_rx, NULL);
    while (r > 0);
    if (r < 0 || fz_q[f].cnt)
        fz_die(__LINE__);
}

static void fix(uint8_t* b, size_t n, unsigned how)
{
    if (!n)
        return;
    if (how == 1)
        b[0] = (uint8_t)(0x40 | (b[0] & 15));
    else if (how == 2)
        b[0] = (uint8_t)(0x60 | (b[0] & 15));
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    char name[32];
    size_t i = 0, k;
    int f;
    wg_tun_stats_t s0[2], s1[2];
    unsigned long long want_drop[2] = { 0, 0 }, want_rx[2] = { 0, 0 }, want_tx[2] = { 0, 0 }, want_bad[2] = { 0, 0 };
    if (!fz_t[0])
        fz_setup();
    k = size < sizeof name - 1 ? size : sizeof name - 1;
    memcpy(name, data, k);
    name[k] = 0;
    for (f = 0; f < 2; f++)
        if (wg_tun_name_ok(name, f) != naive_name(name, f))
            fz_die(__LINE__);
    if (!wg_tun_name_ok(NULL, 0) != 1)
        fz_die(__LINE__);
    for (f = 0; f < 2; f++) {
        fz_frame(f, data, size);
        wg_tun_stats(fz_t[f], &s0[f]);
    }
    while (i + 3 <= size) {
        uint8_t ctl = data[i], b[FZ_MTU + 64];
        size_t len = ((size_t)data[i + 1] << 8 | data[i + 2]) % (FZ_MTU + 40);
        i += 3;
        if (len > size - i)
            len = size - i;
        f = ctl & 1;
        memcpy(b, data + i, len);
        fix(b, len, (ctl >> 4) % 3);
        if (ctl & 2) {
            int r = wg_tun_write(fz_t[f], b, len), v = naive_ver(b, len);
            if (!v || len > FZ_MTU) {
                if (r != -EINVAL)
                    fz_die(__LINE__);
                want_bad[f]++;
            } else {
                uint8_t got[FZ_MTU + 64];
                ssize_t g;
                if (r != 0)
                    fz_die(__LINE__);
                want_tx[f]++;
                g = recv(fz_peer[f], got, sizeof got, MSG_DONTWAIT);
                if (g != (ssize_t)(len + (f ? 4u : 0u)) || memcmp(got + (f ? 4 : 0), b, len) || (f && (got[0] | got[1] | got[2] || got[3] != (v == 4 ? 2 : 30))))
                    fz_die(__LINE__);
            }
        } else {
            uint8_t fr[FZ_MTU + 64];
            size_t fl = len;
            const uint8_t* ip;
            size_t ipn;
            if (f && (ctl & 4)) {
                int v = naive_ver(b, len);
                fr[0] = fr[1] = fr[2] = 0;
                fr[3] = (uint8_t)((ctl & 8) ? data[i ? i - 1 : 0] : v == 6 ? 30 : 2);
                memcpy(fr + 4, b, len);
                fl = len + 4;
            } else
                memcpy(fr, b, len);
            if (!fl) {
                i += len;
                continue;
            }
            if (send(fz_peer[f], fr, fl, MSG_DONTWAIT) != (ssize_t)fl) {
                drain(f);
                if (send(fz_peer[f], fr, fl, MSG_DONTWAIT) != (ssize_t)fl)
                    fz_die(__LINE__);
            }
            if (wg_tun_unframe(f, fr, fl, &ip, &ipn) < 0 || ipn > FZ_MTU)
                want_drop[f]++;
            else {
                push(f, ip, ipn);
                want_rx[f]++;
            }
            if (fz_q[f].cnt == QN || (ctl & 0x80))
                drain(f);
        }
        i += len;
    }
    for (f = 0; f < 2; f++) {
        drain(f);
        wg_tun_stats(fz_t[f], &s1[f]);
        if (s1[f].rx_pkts - s0[f].rx_pkts != want_rx[f] || (s1[f].rx_drop_frame + s1[f].rx_drop_big) - (s0[f].rx_drop_frame + s0[f].rx_drop_big) != want_drop[f] ||
            s1[f].tx_pkts - s0[f].tx_pkts != want_tx[f] || s1[f].tx_drop_bad - s0[f].tx_drop_bad != want_bad[f])
            fz_die(__LINE__);
    }
    return 0;
}
