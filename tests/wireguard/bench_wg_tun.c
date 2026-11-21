/*
 * bench_wg_tun.c -- dyna:wireguard S13d tun fd layer cost on the loop thread, over an AF_UNIX datagram socketpair
 * standing in for the device (a real utun or /dev/net/tun needs root, not available here).
 *  tx: ns per wg_tun_write (frame + write/writev syscall) with the peer draining, 64 and 1420 B, both framings.
 *  rx: ns per packet for wg_tun_read in batches of 64 (read syscall + unframe + callback) after the peer queued 64.
 *  mem: bytes per tun (RSS and malloc size) at N = 16..96 (fd-limited), and fd count + RSS over 20 cycles of
 *      96 open/close (plateau).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <fcntl.h>

#include "../../src/modules/wireguard/wg-tun.h"

static double tcpu(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static double rss_kb(void)
{
#ifdef __APPLE__
    mach_task_basic_info_data_t i;
    mach_msg_type_number_t c = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &c) == KERN_SUCCESS)
        return (double)i.resident_size / 1024.0;
    return 0;
#else
    long a = 0, b = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &a, &b) != 2)
            b = 0;
        fclose(f);
    }
    return (double)b * 4.0;
#endif
}

static int nfds(void)
{
    int i, n = 0;
    for (i = 0; i < 1024; i++)
        if (fcntl(i, F_GETFD) != -1)
            n++;
    return n;
}

static size_t sink;
static void on_rx(void* ud, const uint8_t* ip, size_t n)
{
    (void)ud;
    sink += ip[n - 1];
}

static wg_tun_t* pair(int fr, int* peer)
{
    int s[2], big = 1 << 20;
    socketpair(AF_UNIX, SOCK_DGRAM, 0, s);
    setsockopt(s[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(s[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(s[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(s[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    *peer = s[1];
    return wg_tun_from_fd(s[0], fr, NULL, 1420, NULL);
}

static void b_io(void)
{
    static const size_t sz[] = { 64, 1420 };
    unsigned k, fr, i, r, rounds = 4000;
    uint8_t p[1500], q[1500];
    for (fr = 0; fr < 2; fr++)
        for (k = 0; k < 2; k++) {
            int peer;
            wg_tun_t* t = pair((int)fr, &peer);
            size_t n = sz[k], hl = fr ? 4 : 0;
            double c0, tx, rx = 0, sc = 0, dr = 0, d0;
            wg_tun_stats_t st;
            memset(p, 0x5a, sizeof p);
            p[0] = 0x45;
            c0 = tcpu();
            for (i = 0; i < rounds * 16; i++) {
                wg_tun_write(t, p, n);
                if ((i & 31) == 31) {
                    d0 = tcpu();
                    while (recv(peer, q, sizeof q, MSG_DONTWAIT) > 0)
                        ;
                    dr += tcpu() - d0;
                }
            }
            tx = (tcpu() - c0 - dr) / (rounds * 16);
            wg_tun_stats(t, &st);
            while (recv(peer, q, sizeof q, MSG_DONTWAIT) > 0)
                ;
            memcpy(q + hl, p, n);
            if (fr) {
                q[0] = q[1] = q[2] = 0;
                q[3] = 2;
            }
            for (r = 0; r < rounds; r++) {
                double s0 = tcpu();
                for (i = 0; i < 64; i++)
                    send(peer, q, n + hl, 0);
                c0 = tcpu();
                sc += c0 - s0;
                wg_tun_read(t, 64, on_rx, NULL);
                rx += tcpu() - c0;
            }
            printf("framing %-4s %4zu B: write %5.0f ns/pkt (EAGAIN %llu), read batch-64 %5.0f ns/pkt (peer send %.0f ns/pkt)\n", fr ? "AF" : "NONE", n, tx, (unsigned long long)st.tx_again,
                   rx / (rounds * 64.0), sc / (rounds * 64.0));
            wg_tun_close(t);
            close(peer);
        }
}

static void b_mem(void)
{
    static const unsigned ns[] = { 16, 32, 64, 96 };
    static wg_tun_t* t[96];
    static int peer[96];
    unsigned k, i, c;
    int base = nfds();
    for (k = 0; k < sizeof ns / sizeof ns[0]; k++) {
        double r0 = rss_kb(), r1;
        for (i = 0; i < ns[k]; i++)
            t[i] = pair(0, &peer[i]);
        r1 = rss_kb();
        printf("tuns %3u: RSS +%.0f KB, %.0f B/tun (struct + MTU-sized read buffer)\n", ns[k], r1 - r0, (r1 - r0) * 1024.0 / ns[k]);
        for (i = 0; i < ns[k]; i++) {
            wg_tun_close(t[i]);
            close(peer[i]);
        }
    }
    printf("open/close 96 tuns x20: fds");
    for (c = 0; c < 20; c++) {
        for (i = 0; i < 96; i++)
            t[i] = pair((int)(c & 1), &peer[i]);
        for (i = 0; i < 96; i++) {
            wg_tun_close(t[i]);
            close(peer[i]);
        }
        if (c % 5 == 4)
            printf(" %d(base %d) rss %.0f KB;", nfds(), base, rss_kb());
    }
    printf("\n");
}

int main(void)
{
    printf("rss start %.0f KB\n", rss_kb());
    b_io();
    b_mem();
    printf("rss end %.0f KB (sink %zu)\n", rss_kb(), sink & 1);
    return 0;
}
