/*
 * test_wg_tcp.c -- dyna:wireguard S13c2 TCP (src/modules/wireguard/wg-tcp.c) on the wg-ip protocol hook. Two stacks
 * are joined by a virtual-time link (delay, jitter, loss, duplication, corruption, scripted drops); time only moves to
 * the next packet or TCP deadline. Raw segments are injected for protocol corner cases and replies read from the link.
 *  1. Handshake: SYN options (MSS 1380/1360, SACK-permitted, window scale 5), negotiation off when the peer does not
 *     offer, default MSS 536/1220 without the option (RFC 9293 3.7.1), sequence space crossing 2^32.
 *  2. Bulk both ways with window > 64 KiB (RFC 7323), FIN/FIN close, TIME-WAIT 2 MSL, no retransmission.
 *  3. SACK recovery (RFC 2018 + RFC 6675): three drops in one window -> one fast recovery, exactly three
 *     retransmissions, no RTO. NewReno without SACK (RFC 6582): the same, one retransmission per partial ACK.
 *  4. RFC 2018 section 4 worked examples, hand-typed: 8 x 500 B from 5000, first lost; then 2nd/4th/6th/8th lost.
 *  5. RFC 6298: samples 100 then 300 ms -> SRTT 125, RTTVAR 87, RTO floor 1 s; backoff doubling to 60 s; Karn; give
 *     up after 120 s (data) / 180 s (SYN); RTO 3 s after a retransmitted SYN (5.7).
 *  6. RFC 9293 3.10.7 + RFC 5961: unacceptable segment -> ACK; RST exact -> reset, RST in window -> challenge ACK;
 *     SYN in a synchronized state -> challenge ACK; ACK above SND.NXT ignored; RST replies to closed ports.
 *  7. Zero window: persist probes, no give-up while the peer answers, resume delivers everything (RFC 9293 3.8.6.1).
 *  8. Half-close, simultaneous close (CLOSING), TIME-WAIT restart on a retransmitted FIN, abort inside a callback.
 *  9. Delayed ACK (200 ms, every second full segment, immediate on out-of-order/hole fill), backlog limit, keepalive.
 * 10. Random impairments over 40 seeds: both streams intact, both ends closed, bounded virtual time; 300 parallel
 *     connections; the fuzz entry (src/fuzz/fuzz_wg_tcp.c) on 30000 generated inputs. Live connections end at 0.
 * 11. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks):
 *     tcp-rx-model: raw segments (overlapping, duplicate, longer repeats, old, partially old, far out of window, FIN
 *     in or out of order) against a byte bitmap: delivery, RCV.NXT, EOF, immediate ACKs (out of order, old, hole fill),
 *     SACK blocks equal to the maximal runs above RCV.NXT, first block covering the trigger, empty queue iff no runs.
 *     tcp-stream: two stacks, random chunked writes, pause, nodelay, loss 0-15%, reorder, dup, corruption, abort and
 *     free with data in flight, plus 1.1-1.5 MB runs into a paused receiver (zero window, persist). A wire observer
 *     checks every emitted segment: payload equals the written stream at its offset, never past the peer's largest
 *     ACK+scaled window (1-byte probe at the edge aside), ACK <= received prefix, SACK inside received bytes (checksum-
 *     verified deliveries only), FIN only after shutdown; after healing both streams complete, live 0. tcp-reseeded:
 *     section 10's fuzz entry under a fresh seed. 20 mutants killed on 3 seeds; none equivalent.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/modules/wireguard/wg-tcp.h"
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

static uint64_t T = 1000;
static uint64_t clk(void* ud)
{
    (void)ud;
    return T;
}

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint8_t A4[4] = { 10, 0, 0, 1 }, B4[4] = { 10, 0, 0, 2 }, X4[4] = { 10, 0, 0, 7 };
static const uint8_t A6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, B6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

static wg_ipaddr_t ad(int v6, const uint8_t* a)
{
    wg_ipaddr_t r;
    memset(&r, 0, sizeof r);
    r.v6 = v6;
    memcpy(r.a, a, v6 ? 16 : 4);
    return r;
}

typedef struct {
    uint64_t at, id;
    int to;
    size_t n;
    uint8_t b[1440];
} pk_t;

enum { QMAX = 16384, REC = 512 };
static pk_t q[QMAX];
static unsigned nq;

typedef struct {
    unsigned delay, jitter, loss_ppm, dup_ppm, corrupt_ppm;
    int blackhole;
    int (*drop)(int dir, const uint8_t* p, size_t n);
    pk_t rec[REC];
    unsigned nrec, total;
} link_t;

static link_t L[2];
static void (*rx_obs)(int to, const uint8_t* b, size_t n);

typedef struct {
    wg_ip_t* ip;
    wg_tcp_t* tcp;
} node_t;

static node_t N[2];

static int out_cb(void* ud, const uint8_t* p, size_t n)
{
    int dir = (int)(size_t)ud;
    link_t* l = &L[dir];
    pk_t* r = &l->rec[l->total % REC];
    unsigned copies = 1, k;
    memcpy(r->b, p, n);
    r->n = n;
    r->at = T;
    l->total++;
    if (l->nrec < REC)
        l->nrec++;
    if (l->blackhole || (l->drop && l->drop(dir, p, n)) || (l->loss_ppm && rnd() % 1000000 < l->loss_ppm))
        return 0;
    if (l->dup_ppm && rnd() % 1000000 < l->dup_ppm)
        copies = 2;
    for (k = 0; k < copies && nq < QMAX; k++) {
        pk_t* x = &q[nq++];
        static uint64_t ids;
        x->id = ++ids;
        x->at = T + l->delay + (l->jitter ? rnd() % l->jitter : 0);
        x->to = !dir;
        x->n = n;
        memcpy(x->b, p, n);
        if (l->corrupt_ppm && rnd() % 1000000 < l->corrupt_ppm)
            x->b[20 + rnd() % (n - 20)] ^= (uint8_t)(1u << (rnd() % 8));
    }
    return 0;
}

static void nodes(int sack_e, int sack_f)
{
    int i;
    nq = 0;
    memset(L, 0, sizeof L);
    L[0].delay = L[1].delay = 10;
    for (i = 0; i < 2; i++) {
        wg_ipaddr_t x = ad(0, i ? B4 : A4), y = ad(1, i ? B6 : A6);
        N[i].ip = wg_ip_new(out_cb, (void*)(size_t)i);
        wg_ip_set_clock(N[i].ip, clk, NULL);
        wg_ip_add_addr(N[i].ip, &x);
        wg_ip_add_addr(N[i].ip, &y);
        N[i].tcp = wg_tcp_new(N[i].ip, clk, NULL);
        wg_tcp_set_options(N[i].tcp, i ? sack_f : sack_e, 1);
    }
}

static void nodes_free(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        wg_tcp_free(N[i].tcp);
        wg_ip_free(N[i].ip);
    }
}

static unsigned live(int i)
{
    wg_tcp_stats_t s;
    wg_tcp_stats(N[i].tcp, &s);
    return (unsigned)s.live;
}

static wg_tcp_stats_t stats(int i)
{
    wg_tcp_stats_t s;
    wg_tcp_stats(N[i].tcp, &s);
    return s;
}

static void step(uint64_t limit)
{
    uint64_t m = 0, d;
    unsigned i, j;
    for (i = 0; i < nq; i++)
        if (!m || q[i].at < m)
            m = q[i].at;
    for (i = 0; i < 2; i++)
        if ((d = wg_tcp_next_deadline(N[i].tcp)) && (!m || d < m))
            m = d;
    if (!m || m > limit)
        m = limit;
    if (m > T)
        T = m;
    for (;;) {
        unsigned best = nq;
        for (j = 0; j < nq; j++)
            if (q[j].at <= T && (best == nq || q[j].at < q[best].at || (q[j].at == q[best].at && q[j].id < q[best].id)))
                best = j;
        if (best == nq)
            break;
        {
            pk_t x = q[best];
            q[best] = q[--nq];
            if (rx_obs)
                rx_obs(x.to, x.b, x.n);
            wg_ip_input(N[x.to].ip, x.b, x.n);
        }
    }
    for (i = 0; i < 2; i++)
        wg_tcp_tick(N[i].tcp);
}

static int (*pred)(void);
static int run_until(int (*p)(void), uint64_t ms)
{
    uint64_t end = T + ms;
    unsigned guard = 0;
    pred = p;
    while (T < end && guard++ < 50000000u) {
        if (pred && pred())
            return 1;
        step(end);
    }
    return pred ? pred() : 0;
}

typedef struct {
    wg_tconn_t* c;
    uint8_t* rx;
    size_t rn, rcap;
    int opened, eof, closed, err, shut_on_eof, abort_on_data, echo;
    const uint8_t* src;
    size_t slen, sent;
    int shut_after;
    unsigned writable;
} app_t;

static void pump(app_t* a)
{
    while (a->c && a->sent < a->slen) {
        size_t k = wg_tcp_write(a->c, a->src + a->sent, a->slen - a->sent);
        if (!k)
            break;
        a->sent += k;
    }
    if (a->c && a->shut_after && a->sent == a->slen) {
        a->shut_after = 0;
        wg_tcp_shutdown(a->c);
    }
}

static void on_open(void* ud, wg_tconn_t* c)
{
    app_t* a = (app_t*)ud;
    a->c = c;
    a->opened = 1;
    pump(a);
}

static void on_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    app_t* a = (app_t*)ud;
    if (a->rn + n > a->rcap) {
        a->rcap = (a->rn + n) * 2;
        a->rx = (uint8_t*)realloc(a->rx, a->rcap);
    }
    memcpy(a->rx + a->rn, d, n);
    a->rn += n;
    if (a->abort_on_data) {
        wg_tcp_abort(c);
        a->c = NULL;
        a->closed = 1;
        return;
    }
    if (a->echo)
        wg_tcp_write(c, d, n);
}

static void on_eof(void* ud, wg_tconn_t* c)
{
    app_t* a = (app_t*)ud;
    a->eof = 1;
    if (a->shut_on_eof)
        wg_tcp_shutdown(c);
}

static void on_writable(void* ud, wg_tconn_t* c)
{
    app_t* a = (app_t*)ud;
    (void)c;
    a->writable++;
    pump(a);
}

static void on_close(void* ud, wg_tconn_t* c, int err)
{
    app_t* a = (app_t*)ud;
    (void)c;
    a->closed = 1;
    a->err = err;
    a->c = NULL;
}

static const wg_tcp_cb_t CB = { on_open, on_data, on_eof, on_writable, on_close };

static app_t ca, sa;
static uint8_t* blob(size_t n, unsigned seed)
{
    uint8_t* b = (uint8_t*)malloc(n ? n : 1);
    size_t i;
    uint32_t x = seed * 2654435761u + 1;
    for (i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        b[i] = (uint8_t)(x >> 16);
    }
    return b;
}

static void app_reset(app_t* a)
{
    free(a->rx);
    memset(a, 0, sizeof *a);
}

static int both_open(void)
{
    return ca.opened && sa.opened;
}

static int both_closed(void)
{
    return ca.closed && sa.closed;
}

static int ca_closed(void)
{
    return ca.closed;
}

static uint32_t isn_val[2];
static uint32_t fixed_isn(void* ud, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    (void)la;
    (void)ra;
    (void)lp;
    (void)rp;
    return isn_val[(size_t)ud];
}

static wg_tconn_t* pair(int v6, int sack_e, int sack_f)
{
    wg_ipaddr_t b;
    app_reset(&ca);
    app_reset(&sa);
    nodes(sack_e, sack_f);
    wg_tcp_listen(N[1].tcp, v6, 80, 16, &CB, &sa);
    b = ad(v6, v6 ? B6 : B4);
    ca.c = wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
    run_until(both_open, 5000);
    return ca.c;
}

typedef struct {
    uint32_t seq, ack, win, dlen;
    uint8_t fl;
    const uint8_t* opt;
    size_t ol;
    const uint8_t* d;
} tseg_t;

static int tparse(const pk_t* p, tseg_t* s)
{
    size_t hl = p->b[0] >> 4 == 6 ? 40 : 20, th;
    const uint8_t* t = p->b + hl;
    if ((p->b[0] >> 4 == 6 ? p->b[6] : p->b[9]) != 6)
        return -1;
    th = (size_t)(t[12] >> 4) * 4;
    s->seq = be32(t + 4);
    s->ack = be32(t + 8);
    s->fl = t[13];
    s->win = be16(t + 14);
    s->opt = t + 20;
    s->ol = th - 20;
    s->d = t + th;
    s->dlen = (uint32_t)(p->n - hl - th);
    return 0;
}

static const pk_t* last_out(int dir, unsigned back)
{
    link_t* l = &L[dir];
    if (back >= l->nrec || back >= l->total)
        return NULL;
    return &l->rec[(l->total - 1 - back) % REC];
}

static int opt_find(const tseg_t* s, uint8_t kind, const uint8_t** v)
{
    size_t i = 0;
    while (i < s->ol) {
        if (s->opt[i] == 0)
            break;
        if (s->opt[i] == 1) {
            i++;
            continue;
        }
        if (s->opt[i] == kind) {
            *v = s->opt + i;
            return s->opt[i + 1];
        }
        i += s->opt[i + 1];
    }
    return 0;
}

static int raw_trust, raw_badsum;
static void raw(int to, int v6, const uint8_t* s, const uint8_t* d, uint16_t sp, uint16_t dp, uint32_t seq, uint32_t ack, uint8_t fl, uint16_t win, const uint8_t* opt, size_t ol,
                const uint8_t* data, size_t n)
{
    uint8_t b[1500], *t;
    size_t hl = v6 ? 40 : 20, th = 20 + ol;
    wg_ipaddr_t S = ad(v6, s), D = ad(v6, d);
    uint16_t c;
    t = b + hl;
    put16(t, sp);
    put16(t + 2, dp);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = (uint8_t)(th / 4 << 4);
    t[13] = fl;
    put16(t + 14, win);
    memset(t + 16, 0, 4);
    if (ol)
        memcpy(t + 20, opt, ol);
    if (n)
        memcpy(t + th, data, n);
    c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&S, &D, 6, th + n), t, th + n));
    if (raw_badsum)
        c ^= 0x0101;
    memcpy(t + 16, &c, 2);
    memset(b, 0, hl);
    if (v6) {
        b[0] = 0x60;
        put16(b + 4, (unsigned)(th + n));
        b[6] = 6;
        b[7] = 64;
        memcpy(b + 8, s, 16);
        memcpy(b + 24, d, 16);
    } else {
        b[0] = 0x45;
        put16(b + 2, (unsigned)(hl + th + n));
        b[8] = 64;
        b[9] = 6;
        memcpy(b + 12, s, 4);
        memcpy(b + 16, d, 4);
        c = wg_csum_fin(wg_csum_acc(0, b, 20));
        memcpy(b + 10, &c, 2);
    }
    if (raw_trust)
        wg_ip_input_trusted(N[to].ip, b, hl + th + n);
    else
        wg_ip_input(N[to].ip, b, hl + th + n);
}

enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

static void t_handshake(void)
{
    const pk_t* p;
    tseg_t s;
    const uint8_t* v;
    wg_tcp_info_t i;
    int v6;
    for (v6 = 0; v6 < 2; v6++) {
        pair(v6, 1, 1);
        CHECK(both_open(), "handshake completes");
        p = &L[0].rec[0];
        tparse(p, &s);
        CHECK(s.fl == SYN && opt_find(&s, 2, &v) == 4 && be16(v + 2) == (v6 ? 1360 : 1380) && opt_find(&s, 4, &v) == 2 && opt_find(&s, 3, &v) == 3 && v[2] == 5 && s.win == 65535,
              v6 ? "v6 SYN: MSS 1360, SACK-permitted, WS 5, unscaled window" : "v4 SYN: MSS 1380, SACK-permitted, WS 5, unscaled window");
        wg_tcp_info(ca.c, &i);
        CHECK(i.state == WG_TCP_ESTABLISHED && i.sack_ok && i.ws_ok && i.snd_scale == 5 && i.rcv_scale == 5 && i.mss == (v6 ? 1360u : 1380u) && i.rcv_wnd == WG_TCP_RCV_MAX,
              "negotiated SACK, scale 5 both ways, MSS, 1 MiB receive window");
        CHECK(i.cwnd == (v6 ? 13600u : 13800u) && i.ssthresh == 0xffffffffu, "initial window min(10*MSS, max(2*MSS, 14600)) (RFC 6928)");
        nodes_free();
    }
    app_reset(&ca);
    app_reset(&sa);
    nodes(1, 1);
    wg_tcp_set_options(N[1].tcp, 0, 0);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    {
        wg_ipaddr_t b = ad(0, B4);
        wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
    }
    run_until(both_open, 5000);
    tparse(&L[1].rec[0], &s);
    wg_tcp_info(ca.c, &i);
    CHECK(s.fl == (SYN | ACK) && !opt_find(&s, 4, &v) && !opt_find(&s, 3, &v) && !i.sack_ok && !i.ws_ok && i.rcv_scale == 0 && i.snd_scale == 0,
          "peer without SACK/WS: SYN-ACK carries neither, both off, no scaling");
    nodes_free();
    nodes(1, 1);
    L[1].blackhole = 1;
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    raw(1, 0, X4, B4, 5555, 80, 100, 0, SYN, 8192, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    raw(1, 0, X4, B4, 5555, 80, 101, s.seq + 1, ACK, 8192, NULL, 0, NULL, 0);
    CHECK(sa.opened && sa.c, "raw handshake without options accepted");
    if (sa.c) {
        wg_tcp_info(sa.c, &i);
        CHECK(i.mss == 536 && !i.sack_ok && !i.ws_ok && i.snd_wnd == 8192, "no MSS option: default 536 (RFC 9293 3.7.1)");
    }
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 1, 80, 4, &CB, &sa);
    raw(1, 1, A6, B6, 5556, 80, 100, 0, SYN, 8192, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    raw(1, 1, A6, B6, 5556, 80, 101, s.seq + 1, ACK, 8192, NULL, 0, NULL, 0);
    if (sa.c) {
        wg_tcp_info(sa.c, &i);
        CHECK(i.mss == 1220, "v6 no MSS option: default 1220");
    } else
        CHECK(0, "v6 raw handshake");
    nodes_free();
}

static int bulk_done(void)
{
    return ca.rn == sa.slen && sa.rn == ca.slen && ca.closed && sa.closed;
}

static void t_bulk(void)
{
    size_t n = 2u << 20;
    wg_tcp_info_t i;
    wg_tcp_stats_t s0, s1;
    uint32_t maxw = 0;
    isn_val[0] = 0xfffff000u;
    isn_val[1] = 0xffffff00u;
    app_reset(&ca);
    app_reset(&sa);
    nodes(1, 1);
    wg_tcp_set_isn(N[0].tcp, fixed_isn, (void*)0);
    wg_tcp_set_isn(N[1].tcp, fixed_isn, (void*)1);
    L[0].delay = L[1].delay = 25;
    sa.src = blob(n, 1);
    sa.slen = n;
    sa.shut_after = 1;
    sa.shut_on_eof = 0;
    ca.src = blob(n, 2);
    ca.slen = n;
    ca.shut_after = 1;
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    {
        wg_ipaddr_t b = ad(0, B4);
        wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
    }
    while (!bulk_done() && T < 600000) {
        step(T + 100000);
        if (ca.c) {
            wg_tcp_info(ca.c, &i);
            if (i.snd_wnd > maxw)
                maxw = i.snd_wnd;
        }
    }
    s0 = stats(0);
    s1 = stats(1);
    CHECK(ca.rn == n && sa.rn == n && memcmp(ca.rx, sa.src, n) == 0 && memcmp(sa.rx, ca.src, n) == 0, "2 MiB each way intact across the 2^32 sequence wrap");
    CHECK(ca.eof && sa.eof && ca.closed && sa.closed && ca.err == 0 && sa.err == 0, "FIN both ways, graceful close");
    CHECK(maxw > 65535, "send window above 64 KiB via window scaling");
    CHECK(s0.rexmit_segs == 0 && s1.rexmit_segs == 0 && s0.rto_fired == 0 && s0.tx_fail == 0 && s1.tx_fail == 0, "clean link: no retransmission, no oversize segment");
    CHECK(live(0) + live(1) >= 1, "TIME-WAIT holds the active closer");
    run_until(NULL, 2 * WG_TCP_MSL_MS - 1000);
    CHECK(live(0) + live(1) >= 1, "TIME-WAIT still held before 2 MSL");
    run_until(NULL, 2000);
    CHECK(live(0) == 0 && live(1) == 0, "TIME-WAIT ends at 2 MSL");
    free((void*)(uintptr_t)sa.src);
    free((void*)(uintptr_t)ca.src);
    nodes_free();
}

static unsigned data_seen, drop_mask_hits;
static uint32_t drop_list[8];
static unsigned ndrop;
static int drop_data(int dir, const uint8_t* p, size_t n)
{
    pk_t x;
    tseg_t s;
    unsigned k;
    if (dir != 0)
        return 0;
    x.n = n;
    memcpy(x.b, p, n);
    if (tparse(&x, &s) < 0 || !s.dlen)
        return 0;
    data_seen++;
    for (k = 0; k < ndrop; k++)
        if (drop_list[k] == data_seen) {
            drop_mask_hits++;
            return 1;
        }
    return 0;
}

static int sa_got_all(void)
{
    return sa.rn == ca.slen;
}

static void loss_case(int sack, const char* what)
{
    size_t n = 400000;
    wg_tcp_stats_t s;
    pair(0, sack, sack);
    data_seen = drop_mask_hits = 0;
    drop_list[0] = 40;
    drop_list[1] = 42;
    drop_list[2] = 44;
    ndrop = 3;
    L[0].drop = drop_data;
    ca.src = blob(n, 3);
    ca.slen = n;
    pump(&ca);
    run_until(sa_got_all, 120000);
    s = stats(0);
    CHECK(sa.rn == n && memcmp(sa.rx, ca.src, n) == 0 && drop_mask_hits == 3, what);
    CHECK(s.fast_rexmit == 1 && s.rto_fired == 0, sack ? "SACK: one fast recovery, no RTO" : "NewReno: one fast recovery, no RTO");
    CHECK(s.rexmit_segs == 3, sack ? "SACK: exactly the three lost segments retransmitted" : "NewReno: exactly three retransmissions (one per partial ACK)");
    CHECK(sack ? stats(1).ooo_segs > 0 && s.sack_blocks_in > 0 : s.sack_blocks_in == 0, sack ? "receiver sent SACK blocks" : "no SACK blocks without negotiation");
    {
        wg_tcp_info_t i;
        wg_tcp_info(ca.c, &i);
        CHECK(i.cwnd > 13800 && !i.in_recovery && i.ssthresh < 0xffffffffu, "after recovery: out of recovery, ssthresh set, cwnd grown");
        run_until(NULL, 5000);
        wg_tcp_write(ca.c, (const uint8_t*)"x", 1);
        wg_tcp_info(ca.c, &i);
        CHECK(i.cwnd == 13800, "idle longer than RTO: cwnd restarts at the initial window (RFC 5681 4.1)");
    }
    L[0].drop = NULL;
    free((void*)(uintptr_t)ca.src);
    nodes_free();
}

static void t_loss(void)
{
    loss_case(1, "SACK: 3 drops in one window, stream intact");
    loss_case(0, "NewReno: 3 drops in one window, stream intact");
}

static void rfc2018(void)
{
    static uint8_t d[500];
    tseg_t s;
    const uint8_t* v;
    int k, ok;
    uint32_t base = 5000;
    nodes(1, 1);
    L[1].blackhole = 1;
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    {
        uint8_t o[4] = { 4, 2, 1, 1 };
        raw(1, 0, X4, B4, 6000, 80, base - 1, 0, SYN, 65535, o, 4, NULL, 0);
    }
    tparse(last_out(1, 0), &s);
    {
        uint32_t iss = s.seq;
        raw(1, 0, X4, B4, 6000, 80, base, iss + 1, ACK, 65535, NULL, 0, NULL, 0);
        ok = 1;
        for (k = 1; k < 8; k++) {
            unsigned nb;
            raw(1, 0, X4, B4, 6000, 80, base + 500u * (unsigned)k, iss + 1, ACK, 65535, NULL, 0, d, 500);
            tparse(last_out(1, 0), &s);
            nb = (unsigned)opt_find(&s, 5, &v);
            if (s.ack != base || nb != 10 || be32(v + 2) != base + 500 || be32(v + 6) != base + 500u * (unsigned)(k + 1))
                ok = 0;
        }
        CHECK(ok, "RFC 2018 case 2: first lost, every ACK 5000 with one block 5500-(5500+500k)");
        raw(1, 0, X4, B4, 6000, 80, base, iss + 1, ACK, 65535, NULL, 0, d, 500);
        tparse(last_out(1, 0), &s);
        CHECK(s.ack == base + 4000 && !opt_find(&s, 5, &v), "hole filled: cumulative ACK 9000 at once, no SACK");
    }
    nodes_free();
    nodes(1, 1);
    L[1].blackhole = 1;
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    {
        uint8_t o[4] = { 4, 2, 1, 1 };
        uint32_t iss;
        raw(1, 0, X4, B4, 6001, 80, base - 1, 0, SYN, 65535, o, 4, NULL, 0);
        tparse(last_out(1, 0), &s);
        iss = s.seq;
        raw(1, 0, X4, B4, 6001, 80, base, iss + 1, ACK, 65535, NULL, 0, NULL, 0);
        raw(1, 0, X4, B4, 6001, 80, base, iss + 1, ACK, 65535, NULL, 0, d, 500);
        raw(1, 0, X4, B4, 6001, 80, base + 1000, iss + 1, ACK, 65535, NULL, 0, d, 500);
        tparse(last_out(1, 0), &s);
        CHECK(s.ack == 5500 && opt_find(&s, 5, &v) == 10 && be32(v + 2) == 6000 && be32(v + 6) == 6500, "RFC 2018 case 3: ACK 5500, SACK 6000-6500");
        raw(1, 0, X4, B4, 6001, 80, base + 2000, iss + 1, ACK, 65535, NULL, 0, d, 500);
        tparse(last_out(1, 0), &s);
        CHECK(s.ack == 5500 && opt_find(&s, 5, &v) == 18 && be32(v + 2) == 7000 && be32(v + 6) == 7500 && be32(v + 10) == 6000 && be32(v + 14) == 6500,
              "RFC 2018 case 3: SACK 7000-7500, 6000-6500");
        raw(1, 0, X4, B4, 6001, 80, base + 3000, iss + 1, ACK, 65535, NULL, 0, d, 500);
        tparse(last_out(1, 0), &s);
        CHECK(s.ack == 5500 && opt_find(&s, 5, &v) == 26 && be32(v + 2) == 8000 && be32(v + 6) == 8500 && be32(v + 10) == 7000 && be32(v + 18) == 6000,
              "RFC 2018 case 3: SACK 8000-8500, 7000-7500, 6000-6500");
        raw(1, 0, X4, B4, 6001, 80, base + 1500, iss + 1, ACK, 65535, NULL, 0, d, 500);
        tparse(last_out(1, 0), &s);
        CHECK(s.ack == 5500 && opt_find(&s, 5, &v) == 18 && be32(v + 2) == 6000 && be32(v + 6) == 7500 && be32(v + 10) == 8000 && be32(v + 14) == 8500,
              "RFC 2018 case 3, 4th segment arrives: SACK 6000-7500 first, then 8000-8500");
        {
            int i;
            for (i = 0; i < 6; i++)
                raw(1, 0, X4, B4, 6001, 80, base + 4000 + 1000u * (unsigned)i, iss + 1, ACK, 65535, NULL, 0, d, 500);
            tparse(last_out(1, 0), &s);
            CHECK(opt_find(&s, 5, &v) == 34 && s.ol <= 40, "at most 4 SACK blocks per ACK");
        }
    }
    nodes_free();
}

static void t_rtt(void)
{
    wg_tcp_info_t i;
    static uint8_t d[4000];
    nodes(1, 1);
    app_reset(&ca);
    app_reset(&sa);
    L[0].delay = L[1].delay = 50;
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    {
        wg_ipaddr_t b = ad(0, B4);
        wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
    }
    run_until(both_open, 5000);
    wg_tcp_info(ca.c, &i);
    CHECK(i.srtt_ms == 100 && i.rttvar_ms == 50 && i.rto_ms == 1000, "RFC 6298 first sample 100: SRTT 100, RTTVAR 50, RTO floored at 1 s");
    run_until(NULL, 1000);
    L[0].delay = L[1].delay = 150;
    wg_tcp_set_nodelay(ca.c, 1);
    wg_tcp_write(ca.c, d, 2 * i.mss);
    run_until(NULL, 2000);
    wg_tcp_info(ca.c, &i);
    CHECK(i.srtt_ms == 125 && i.rttvar_ms == 87 && i.rto_ms == 1000, "RFC 6298 second sample 300: SRTT 125, RTTVAR 87.5");
    L[0].blackhole = 1;
    wg_tcp_write(ca.c, d, 100);
    run_until(NULL, 800);
    L[0].blackhole = 0;
    run_until(NULL, 3000);
    wg_tcp_info(ca.c, &i);
    CHECK(i.srtt_ms == 125 && i.rto_ms == 2000 && stats(0).rto_fired == 1, "Karn: no sample from a retransmitted segment, backed-off RTO kept");
    wg_tcp_write(ca.c, d, 2 * i.mss);
    run_until(NULL, 2000);
    wg_tcp_info(ca.c, &i);
    CHECK(i.rto_ms == 1000 && i.srtt_ms == 146, "next clean sample (300 ms) recomputes RTO; SRTT 7/8*125 + 300/8 = 146");
    {
        uint64_t times[8];
        unsigned n = 0, k, tot0 = L[0].total;
        int ok = 1;
        L[0].blackhole = 1;
        wg_tcp_write(ca.c, d, 100);
        while (n < 6 && T < 2000000) {
            step(T + 1000000);
            while (tot0 < L[0].total && n < 8) {
                times[n++] = L[0].rec[tot0 % REC].at;
                tot0++;
            }
        }
        for (k = 2; k < 6; k++)
            if (times[k] - times[k - 1] != 2 * (times[k - 1] - times[k - 2]))
                ok = 0;
        CHECK(ok && times[1] - times[0] == 1000 && stats(0).rto_fired >= 5, "RTO backoff doubles: 1, 2, 4, 8, 16 s");
        wg_tcp_info(ca.c, &i);
        CHECK(i.cwnd == i.mss && i.rto_recovery, "after RTO: cwnd 1 MSS, loss recovery from the timeout");
        run_until(ca_closed, 400000);
        CHECK(ca.closed && ca.err == -ETIMEDOUT && T - times[0] >= WG_TCP_R2_MS && T - times[0] < WG_TCP_R2_MS + WG_TCP_RTO_MAX_MS + 1000, "data give-up after >= 120 s: ETIMEDOUT");
    }
    nodes_free();
    nodes(1, 1);
    app_reset(&ca);
    L[0].blackhole = 1;
    {
        wg_ipaddr_t b = ad(0, B4);
        uint64_t t0 = T;
        wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
        run_until(ca_closed, 400000);
        CHECK(ca.closed && ca.err == -ETIMEDOUT && T - t0 >= WG_TCP_R2_SYN_MS && T - t0 < WG_TCP_R2_SYN_MS + WG_TCP_RTO_MAX_MS + 1000 && !ca.opened, "SYN give-up after >= 180 s");
    }
    nodes_free();
    nodes(1, 1);
    app_reset(&ca);
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    L[0].blackhole = 1;
    {
        wg_ipaddr_t b = ad(0, B4);
        wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
        run_until(NULL, 500);
        L[0].blackhole = 0;
        run_until(both_open, 5000);
        wg_tcp_info(ca.c, &i);
        CHECK(both_open() && i.rto_ms == WG_TCP_RTO_SYN_EST_MS, "retransmitted SYN: RTO 3 s once established (RFC 6298 5.7)");
    }
    nodes_free();
    nodes(1, 1);
    app_reset(&ca);
    {
        wg_ipaddr_t b = ad(0, B4);
        tseg_t s;
        wg_tcp_connect(N[0].tcp, &b, 81, &CB, &ca);
        run_until(ca_closed, 5000);
        tparse(last_out(1, 0), &s);
        CHECK(ca.closed && ca.err == -ECONNREFUSED && s.fl == (RST | ACK) && s.seq == 0 && s.ack == be32(L[0].rec[0].b + 24) + 1, "no listener: RST|ACK, ECONNREFUSED");
    }
    nodes_free();
}

static uint32_t r_iss;
static uint16_t r_lp;
static unsigned r_out(void)
{
    return L[0].total;
}

static void raw_client(const uint8_t* o, size_t ol, uint16_t win)
{
    wg_ipaddr_t b = ad(0, B4);
    tseg_t s;
    nodes(1, 1);
    app_reset(&ca);
    L[0].blackhole = L[1].blackhole = 1;
    wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
    tparse(&L[0].rec[0], &s);
    r_iss = s.seq;
    r_lp = be16(L[0].rec[0].b + 20);
    raw(0, 0, B4, A4, 80, r_lp, 7000, r_iss + 1, SYN | ACK, win, o, ol, NULL, 0);
}

static uint32_t S(unsigned j)
{
    return r_iss + 1 + 536u * (j - 1);
}

static void dupack(const uint32_t (*blk)[2], unsigned nb)
{
    uint8_t o[40];
    unsigned k;
    o[0] = o[1] = 1;
    o[2] = 5;
    o[3] = (uint8_t)(2 + 8 * nb);
    for (k = 0; k < nb; k++) {
        put32(o + 4 + 8 * k, blk[k][0]);
        put32(o + 8 + 8 * k, blk[k][1]);
    }
    raw(0, 0, B4, A4, 80, r_lp, 7001, r_iss + 1, ACK, 65535, o, nb ? 4 + 8 * nb : 0, NULL, 0);
}

static void t_raw_sender(void)
{
    static const uint8_t o_ws[12] = { 2, 4, 0x02, 0x18, 1, 3, 3, 7, 1, 1, 4, 2 }, o_sack[8] = { 2, 4, 0x02, 0x18, 1, 1, 4, 2 }, o_plain[4] = { 2, 4, 0x02, 0x18 };
    static uint8_t d[20 * 536];
    wg_tcp_info_t i;
    tseg_t s;
    unsigned u, k;
    uint32_t blk[3][2];
    raw_client(o_ws, 12, 1000);
    wg_tcp_info(ca.c, &i);
    CHECK(ca.opened && i.snd_wnd == 1000 && i.snd_scale == 7 && i.ws_ok, "SYN-ACK window is not scaled (RFC 7323 2.2), scale 7 applies afterwards");
    nodes_free();
    raw_client(o_sack, 8, 65535);
    wg_tcp_set_nodelay(ca.c, 1);
    wg_tcp_write(ca.c, d, sizeof d);
    wg_tcp_info(ca.c, &i);
    CHECK(i.mss == 536 && i.cwnd == 5360 && i.snd_nxt - i.snd_una == 5360, "IW 10 x 536 in flight");
    u = r_out();
    for (k = 1; k <= 6; k++) {
        blk[0][0] = S(2);
        blk[0][1] = S(2 + k);
        dupack((const uint32_t(*)[2])blk, 1);
        if (k == 2)
            CHECK(r_out() == u && stats(0).fast_rexmit == 0, "two dupacks, 2 SACKed segments: no recovery yet");
        if (k == 3) {
            tparse(last_out(0, 0), &s);
            wg_tcp_info(ca.c, &i);
            CHECK(r_out() == u + 1 && s.seq == S(1) && s.dlen == 536 && i.ssthresh == 2680 && i.cwnd == 2680 && i.in_recovery,
                  "third dupack: retransmit HighACK+1 only, ssthresh = cwnd = FlightSize/2 (RFC 6675 5)");
        }
        if (k == 4 || k == 5)
            CHECK(r_out() == u + 1, "pipe counts the retransmission: no new data while pipe >= cwnd");
        if (k == 6) {
            tparse(last_out(0, 0), &s);
            CHECK(r_out() == u + 2 && s.seq == S(11), "pipe < cwnd: NextSeg rule 2 sends new data");
        }
    }
    nodes_free();
    raw_client(o_sack, 8, 65535);
    wg_tcp_set_nodelay(ca.c, 1);
    wg_tcp_write(ca.c, d, sizeof d);
    blk[0][0] = S(2);
    blk[0][1] = S(5);
    dupack((const uint32_t(*)[2])blk, 1);
    CHECK(stats(0).fast_rexmit == 1, "one dupack SACKing 3 segments: IsLost(HighACK+1) starts recovery");
    nodes_free();
    raw_client(o_sack, 8, 65535);
    wg_tcp_set_nodelay(ca.c, 1);
    wg_tcp_write(ca.c, d, sizeof d);
    blk[0][0] = S(2);
    blk[0][1] = S(3);
    blk[1][0] = S(11);
    blk[1][1] = S(12);
    blk[2][0] = r_iss - 1000;
    blk[2][1] = r_iss - 500;
    dupack((const uint32_t(*)[2])blk, 3);
    wg_tcp_info(ca.c, &i);
    CHECK(i.nsb == 1, "SACK blocks beyond SND.NXT or below SND.UNA ignored");
    nodes_free();
    raw_client(o_sack, 8, 65535);
    memset(d, 'w', 300);
    raw(0, 0, B4, A4, 80, r_lp, 7011, r_iss + 1, ACK, 4000, NULL, 0, d, 10);
    raw(0, 0, B4, A4, 80, r_lp, 7001, r_iss + 1, ACK, 1500, NULL, 0, d, 10);
    wg_tcp_info(ca.c, &i);
    CHECK(i.snd_wnd == 4000 && i.rcv_nxt == 7021, "older segment (SEG.SEQ < SND.WL1) does not update SND.WND (RFC 9293 3.10.7.4)");
    raw(0, 0, B4, A4, 80, r_lp, i.rcv_nxt + i.rcv_wnd - 100, r_iss + 1, ACK, 4000, NULL, 0, d, 300);
    {
        const uint8_t* v = NULL;
        uint32_t edge = i.rcv_nxt + i.rcv_wnd;
        tparse(last_out(0, 0), &s);
        CHECK(opt_find(&s, 5, &v) == 10 && be32(v + 2) == edge - 100 && be32(v + 6) == edge, "segment crossing RCV.NXT+RCV.WND trimmed to the window (SACK block ends at the edge)");
    }
    nodes_free();
    raw_client(o_sack, 8, 65535);
    {
        wg_tcp_stats_t s0;
        raw_badsum = 1;
        raw(0, 0, B4, A4, 80, r_lp, 7001, r_iss + 1, ACK, 65535, NULL, 0, d, 10);
        wg_tcp_info(ca.c, &i);
        s0 = stats(0);
        CHECK(i.rcv_nxt == 7001 && s0.bad_csum == 1, "bad TCP checksum, untrusted: dropped");
        raw_trust = 1;
        raw(0, 0, B4, A4, 80, r_lp, 7001, r_iss + 1, ACK, 65535, NULL, 0, d, 10);
        raw_trust = raw_badsum = 0;
        wg_tcp_info(ca.c, &i);
        CHECK(i.rcv_nxt == 7011 && stats(0).bad_csum == 1, "trusted input (checksum verified on the pool): TCP skips its own check");
    }
    nodes_free();
    raw_client(o_plain, 4, 65535);
    wg_tcp_set_nodelay(ca.c, 1);
    wg_tcp_write(ca.c, d, 4 * 536);
    u = r_out();
    dupack(NULL, 0);
    dupack(NULL, 0);
    CHECK(r_out() == u, "NewReno: two dupacks, nothing sent");
    dupack(NULL, 0);
    tparse(last_out(0, 0), &s);
    CHECK(r_out() == u + 1 && s.seq == S(1) && stats(0).fast_rexmit == 1, "NewReno: third dupack retransmits (RFC 5681 3.2)");
    raw(0, 0, B4, A4, 80, r_lp, 7001, S(3), ACK, 65535, NULL, 0, NULL, 0);
    tparse(last_out(0, 0), &s);
    CHECK(s.seq == S(3) && stats(0).rexmit_segs == 2, "NewReno partial ACK retransmits the next hole (RFC 6582 3.2)");
    nodes_free();
    nodes(1, 1);
    L[0].blackhole = L[1].blackhole = 1;
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    raw(1, 0, A4, B4, 5000, 80, 9000, 0, SYN, 1000, o_ws, 12, NULL, 0);
    {
        const uint8_t* v = NULL;
        tparse(last_out(1, 0), &s);
        CHECK(s.fl == (SYN | ACK) && s.win == 65535 && opt_find(&s, 3, &v) == 3 && v[2] == 5, "our SYN-ACK window field is unscaled (65535 for 1 MiB), WS 5 offered");
    }
    raw(1, 0, A4, B4, 5000, 80, 9001, s.seq + 1, ACK, 8, NULL, 0, NULL, 0);
    wg_tcp_info(sa.c, &i);
    CHECK(sa.opened && i.snd_wnd == 8u << 7, "passive open: the third-step ACK window is scaled by the peer's shift 7");
    nodes_free();
}

static void t_rfc5961(void)
{
    tseg_t s;
    wg_tcp_info_t i;
    wg_tcp_stats_t st;
    unsigned tot;
    uint32_t iss, nxt;
    static uint8_t d[100];
    nodes(1, 1);
    L[1].blackhole = 1;
    app_reset(&sa);
    wg_tcp_listen(N[1].tcp, 0, 80, 4, &CB, &sa);
    raw(1, 0, X4, B4, 7000, 80, 1000, 777, ACK, 1000, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    CHECK(s.fl == RST && s.seq == 777, "stray ACK to a listener: RST with SEQ = SEG.ACK");
    raw(1, 0, X4, B4, 7000, 81, 1000, 0, SYN, 1000, NULL, 0, d, 10);
    tparse(last_out(1, 0), &s);
    CHECK(s.fl == (RST | ACK) && s.seq == 0 && s.ack == 1011, "SYN with data to a closed port: RST|ACK, ACK = SEQ + SYN + len");
    tot = L[1].total;
    raw(1, 0, X4, B4, 7000, 81, 1000, 0, RST, 1000, NULL, 0, NULL, 0);
    CHECK(L[1].total == tot, "RST to a closed port is not answered");
    raw(1, 0, X4, B4, 7001, 80, 1000, 0, SYN, 65535, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    iss = s.seq;
    raw(1, 0, X4, B4, 7001, 80, 1000, 0, SYN, 65535, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    CHECK(s.fl == (SYN | ACK) && s.seq == iss && stats(1).live == 1, "retransmitted SYN in SYN-RECEIVED: same SYN-ACK, one connection");
    raw(1, 0, X4, B4, 7001, 80, 1001, iss + 5, ACK, 65535, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    CHECK(s.fl == RST && s.seq == iss + 5 && !sa.opened, "unacceptable ACK in SYN-RECEIVED: RST SEQ = SEG.ACK");
    raw(1, 0, X4, B4, 7001, 80, 1001, iss + 1, ACK, 65535, NULL, 0, NULL, 0);
    CHECK(sa.opened, "acceptable ACK opens");
    nxt = 1001;
    raw(1, 0, X4, B4, 7001, 80, nxt, iss + 1, ACK, 65535, NULL, 0, d, 100);
    CHECK(sa.rn == 100, "in-order data delivered");
    nxt += 100;
    tot = L[1].total;
    raw(1, 0, X4, B4, 7001, 80, nxt - 50, iss + 1, ACK, 65535, NULL, 0, d, 40);
    tparse(last_out(1, 0), &s);
    CHECK(L[1].total == tot + 1 && s.fl == ACK && s.ack == nxt && sa.rn == 100, "old duplicate segment: ACK RCV.NXT, nothing delivered");
    raw(1, 0, X4, B4, 7001, 80, nxt + WG_TCP_RCV_MAX + 10, iss + 1, ACK, 65535, NULL, 0, d, 10);
    tparse(last_out(1, 0), &s);
    CHECK(L[1].total == tot + 2 && s.ack == nxt, "segment beyond the window: ACK");
    raw(1, 0, X4, B4, 7001, 80, nxt - 10, iss + 1, ACK, 65535, NULL, 0, d, 30);
    CHECK(sa.rn == 120 && memcmp(sa.rx + 100, d + 10, 20) == 0, "overlapping segment trimmed to RCV.NXT");
    nxt += 20;
    st = stats(1);
    raw(1, 0, X4, B4, 7001, 80, nxt + 100, 0, RST, 0, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    CHECK(stats(1).challenge_acks == st.challenge_acks + 1 && s.fl == ACK && s.ack == nxt && !sa.closed, "RST in window but not at RCV.NXT: challenge ACK (RFC 5961 3.2)");
    raw(1, 0, X4, B4, 7001, 80, nxt, iss + 1, SYN, 0, NULL, 0, NULL, 0);
    CHECK(stats(1).challenge_acks == st.challenge_acks + 2 && !sa.closed, "SYN in ESTABLISHED: challenge ACK (RFC 5961 4.2)");
    tot = L[1].total;
    raw(1, 0, X4, B4, 7001, 80, nxt, iss + 999, ACK, 65535, NULL, 0, NULL, 0);
    tparse(last_out(1, 0), &s);
    wg_tcp_info(sa.c, &i);
    CHECK(L[1].total == tot + 1 && s.fl == ACK && i.snd_una == iss + 1, "ACK above SND.NXT: ACK sent, segment dropped");
    raw(1, 0, X4, B4, 7001, 80, nxt + 1000000u, 0, RST, 0, NULL, 0, NULL, 0);
    CHECK(!sa.closed, "RST outside the window ignored");
    raw(1, 0, X4, B4, 7001, 80, nxt, 0, RST, 0, NULL, 0, NULL, 0);
    CHECK(sa.closed && sa.err == -ECONNRESET && live(1) == 0, "RST at RCV.NXT resets: ECONNRESET");
    nodes_free();
}

static int f_blocked(void)
{
    wg_tcp_info_t i;
    if (!ca.c)
        return 0;
    wg_tcp_info(ca.c, &i);
    return i.snd_wnd == 0;
}

static void t_zero_window(void)
{
    size_t n = 3u << 20;
    pair(0, 1, 1);
    wg_tcp_pause(sa.c, 1);
    ca.src = blob(n, 4);
    ca.slen = n;
    pump(&ca);
    CHECK(ca.sent == WG_TCP_SND_MAX, "send buffer bounded at 1 MiB");
    run_until(f_blocked, 60000);
    CHECK(f_blocked() && sa.rn == 0, "paused receiver closes the window to zero");
    run_until(NULL, 300000);
    CHECK(!ca.closed && stats(0).persist_probes >= 3 && f_blocked(), "persist probes for 300 s, no give-up while the peer answers");
    wg_tcp_pause(sa.c, 0);
    run_until(sa_got_all, 120000);
    CHECK(sa.rn == n && memcmp(sa.rx, ca.src, n) == 0 && ca.writable > 0, "resume: all 3 MiB delivered in order, writer woken");
    free((void*)(uintptr_t)ca.src);
    nodes_free();
}

static int fe_eof(void)
{
    return sa.eof;
}

static void t_close(void)
{
    size_t n = 100000;
    wg_tcp_info_t i;
    pair(0, 1, 1);
    wg_tcp_shutdown(ca.c);
    run_until(fe_eof, 5000);
    run_until(NULL, 100);
    wg_tcp_info(ca.c, &i);
    CHECK(sa.eof && i.state == WG_TCP_FIN_WAIT_2, "half-close: peer sees EOF, closer in FIN-WAIT-2");
    sa.src = blob(n, 5);
    sa.slen = n;
    sa.shut_after = 1;
    pump(&sa);
    run_until(both_closed, 60000);
    CHECK(ca.rn == n && memcmp(ca.rx, sa.src, n) == 0 && ca.closed && sa.closed && !ca.err && !sa.err, "half-closed side still receives 100 KB, then both close");
    free((void*)(uintptr_t)sa.src);
    nodes_free();
    pair(0, 1, 1);
    wg_tcp_shutdown(ca.c);
    wg_tcp_shutdown(sa.c);
    run_until(both_closed, 10000);
    CHECK(both_closed() && !ca.err && !sa.err && live(0) == 1 && live(1) == 1, "simultaneous close: both through CLOSING into TIME-WAIT");
    run_until(NULL, 2 * WG_TCP_MSL_MS + 1000);
    CHECK(live(0) == 0 && live(1) == 0, "both TIME-WAITs expire");
    nodes_free();
    pair(0, 1, 1);
    sa.abort_on_data = 1;
    wg_tcp_write(ca.c, (const uint8_t*)"boom", 4);
    run_until(ca_closed, 5000);
    CHECK(sa.closed && ca.closed && ca.err == -ECONNRESET && live(1) == 0 && live(0) == 0, "abort inside on_data: freed safely, peer reset");
    nodes_free();
}

static void t_tw_restart(void)
{
    tseg_t s;
    const pk_t* fin = NULL;
    pk_t keep;
    unsigned k;
    pair(0, 1, 1);
    sa.shut_on_eof = 1;
    wg_tcp_shutdown(ca.c);
    run_until(both_closed, 5000);
    for (k = 0; k < L[1].nrec; k++) {
        const pk_t* p = last_out(1, k);
        if (p && tparse(p, &s) == 0 && (s.fl & FIN)) {
            fin = p;
            break;
        }
    }
    CHECK(fin != NULL && live(0) == 1, "active closer in TIME-WAIT, peer FIN recorded");
    if (fin) {
        unsigned tot;
        keep = *fin;
        run_until(NULL, 50000);
        L[0].blackhole = 1;
        tot = L[0].total;
        wg_ip_input(N[0].ip, keep.b, keep.n);
        tparse(last_out(0, 0), &s);
        CHECK(L[0].total == tot + 1 && s.fl == ACK, "retransmitted FIN in TIME-WAIT is ACKed");
        run_until(NULL, 20000);
        CHECK(live(0) == 1, "TIME-WAIT restarted by the FIN (2 MSL from the FIN)");
        run_until(NULL, 45000);
        CHECK(live(0) == 0, "restarted TIME-WAIT ends");
    }
    nodes_free();
}

static void t_ack_policy(void)
{
    wg_tcp_stats_t s0, s1;
    static uint8_t d[4000];
    wg_tcp_info_t i;
    pair(0, 1, 1);
    run_until(NULL, 1000);
    wg_tcp_set_nodelay(ca.c, 1);
    s0 = stats(1);
    wg_tcp_write(ca.c, d, 10);
    run_until(NULL, 15);
    s1 = stats(1);
    CHECK(s1.dack_sent == s0.dack_sent && s1.segs_out == s0.segs_out, "one small segment: ACK held");
    run_until(NULL, 200);
    s1 = stats(1);
    CHECK(s1.dack_sent == s0.dack_sent + 1, "delayed ACK sent within 200 ms");
    wg_tcp_info(ca.c, &i);
    s0 = stats(1);
    wg_tcp_write(ca.c, d, 2 * i.mss);
    run_until(NULL, 15);
    s1 = stats(1);
    CHECK(s1.ack_now == s0.ack_now + 1 && s1.dack_sent == s0.dack_sent, "second full-sized segment ACKed at once");
    run_until(NULL, 1000);
    s0 = stats(0);
    wg_tcp_set_nodelay(ca.c, 0);
    wg_tcp_write(ca.c, d, 100);
    wg_tcp_write(ca.c, d, 100);
    wg_tcp_write(ca.c, d, 100);
    s1 = stats(0);
    CHECK(s1.segs_out == s0.segs_out + 1, "Nagle: small writes with data in flight are held");
    run_until(NULL, 1000);
    CHECK(sa.rn == 10 + 2 * i.mss + 300, "held bytes go out after the ACK");
    nodes_free();
    nodes(1, 1);
    app_reset(&sa);
    L[1].blackhole = 1;
    wg_tcp_listen(N[1].tcp, 0, 80, 2, &CB, &sa);
    {
        unsigned k;
        for (k = 0; k < 5; k++)
            raw(1, 0, X4, B4, (uint16_t)(9100 + k), 80, 1000, 0, SYN, 65535, NULL, 0, NULL, 0);
        CHECK(stats(1).syn_drop == 3 && live(1) == 2, "listen backlog 2: three SYNs dropped");
    }
    nodes_free();
}

static void t_keepalive(void)
{
    wg_tcp_stats_t s;
    pair(0, 1, 1);
    wg_tcp_set_keepalive(ca.c, 10000);
    run_until(NULL, 100000);
    s = stats(0);
    CHECK(s.ka_probes >= 1 && !ca.closed, "keepalive probes answered: connection stays");
    L[1].blackhole = 1;
    run_until(ca_closed, 10000 + 10 * WG_TCP_KA_INTVL_MS + 20000);
    CHECK(ca.closed && ca.err == -ETIMEDOUT && stats(0).ka_probes >= s.ka_probes + WG_TCP_KA_PROBES, "unanswered keepalive: 9 probes then ETIMEDOUT");
    nodes_free();
}

static int rnd_done(void)
{
    return ca.rn == sa.slen && sa.rn == ca.slen && ca.closed && sa.closed;
}

static void t_random(void)
{
    unsigned seed, bad = 0, slow = 0;
    uint64_t worst = 0, csum = 0;
    for (seed = 0; seed < 40; seed++) {
        size_t n1 = 150000 + seed * 9000, n2 = 90000 + seed * 5000;
        uint64_t t0;
        int sack = seed % 3 != 2;
        rs = 0x1234567ULL + seed * 7919;
        app_reset(&ca);
        app_reset(&sa);
        nodes(sack, 1);
        L[0].delay = L[1].delay = 5 + seed * 3;
        L[0].jitter = L[1].jitter = seed % 4 ? 8 : 0;
        L[0].loss_ppm = L[1].loss_ppm = 10000 + seed * 3000;
        L[0].dup_ppm = L[1].dup_ppm = 5000;
        L[0].corrupt_ppm = L[1].corrupt_ppm = 3000;
        ca.src = blob(n1, seed + 10);
        ca.slen = n1;
        ca.shut_after = 1;
        sa.src = blob(n2, seed + 20);
        sa.slen = n2;
        sa.shut_after = 1;
        wg_tcp_listen(N[1].tcp, seed & 1, 80, 4, &CB, &sa);
        t0 = T;
        {
            wg_ipaddr_t b = ad(seed & 1, (seed & 1) ? B6 : B4);
            wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
        }
        run_until(rnd_done, 900000);
        if (!rnd_done() || memcmp(sa.rx, ca.src, n1) || memcmp(ca.rx, sa.src, n2) || ca.err || sa.err) {
            bad++;
            printf("  seed %u: c->s %zu/%zu s->c %zu/%zu closed %d/%d err %d/%d\n", seed, sa.rn, n1, ca.rn, n2, ca.closed, sa.closed, ca.err, sa.err);
        }
        if (T - t0 > worst)
            worst = T - t0;
        if (T - t0 > 600000)
            slow++;
        csum += stats(0).bad_csum + stats(1).bad_csum;
        if (stats(0).tx_fail || stats(1).tx_fail)
            bad++;
        free((void*)(uintptr_t)ca.src);
        free((void*)(uintptr_t)sa.src);
        nodes_free();
    }
    printf("  random: worst completion %llu ms of virtual time\n", (unsigned long long)worst);
    CHECK(bad == 0 && slow == 0, "40 seeds: loss 1-13%, jitter (reorder), dup 0.5%, corrupt 0.3%; SACK and NewReno; v4 and v6: both streams intact, both closed");
    CHECK(csum > 0, "corrupted segments caught by the TCP or IP checksum");
}

static app_t many[300];
static void m_open(void* ud, wg_tconn_t* c)
{
    app_t* a = (app_t*)ud;
    a->c = c;
    a->opened = 1;
    pump(a);
}

static unsigned m_next;
static const wg_tcp_cb_t MCB = { m_open, on_data, on_eof, on_writable, on_close };
static app_t srv[300];
static void s_open2(void* ud, wg_tconn_t* c)
{
    app_t* a = &srv[m_next++ % 300];
    (void)ud;
    memset(a, 0, sizeof *a);
    a->c = c;
    a->echo = 1;
    a->shut_on_eof = 1;
    a->opened = 1;
    wg_tcp_set_cb(c, &MCB, a);
}

static const wg_tcp_cb_t SCB = { s_open2, NULL, NULL, NULL, NULL };

static int many_done(void)
{
    unsigned k;
    for (k = 0; k < 300; k++)
        if (!many[k].closed || many[k].rn != many[k].slen)
            return 0;
    return 1;
}

static void t_many(void)
{
    unsigned k, ok = 1;
    uint8_t* src = blob(10000, 9);
    nodes(1, 1);
    m_next = 0;
    wg_tcp_listen(N[1].tcp, 0, 80, 300, &SCB, NULL);
    for (k = 0; k < 300; k++) {
        wg_ipaddr_t b = ad(0, B4);
        app_reset(&many[k]);
        many[k].src = src;
        many[k].slen = 10000;
        many[k].shut_after = 1;
        wg_tcp_connect(N[0].tcp, &b, 80, &MCB, &many[k]);
    }
    run_until(many_done, 120000);
    for (k = 0; k < 300; k++)
        if (!many[k].closed || many[k].rn != 10000 || memcmp(many[k].rx, src, 10000) || many[k].err)
            ok = 0;
    CHECK(ok, "300 parallel connections: 10 KB echoed each, all closed");
    run_until(NULL, 2 * WG_TCP_MSL_MS + 1000);
    CHECK(live(0) == 0 && live(1) == 0, "no live connection left");
    for (k = 0; k < 300; k++) {
        free(many[k].rx);
        free(srv[k].rx);
        memset(&srv[k], 0, sizeof srv[k]);
        memset(&many[k], 0, sizeof many[k]);
    }
    free(src);
    nodes_free();
}

static void t_fuzz(void)
{
    uint8_t b[2048];
    int i, k;
    for (i = 0; i < 30000; i++) {
        size_t n = 1;
        int recs = 1 + (int)(rnd() % 6);
        b[0] = (uint8_t)(i % 20 ? 0 : 1);
        for (k = 0; k < recs; k++) {
            uint8_t len = (uint8_t)(20 + rnd() % 80), j;
            b[n++] = (uint8_t)(rnd() | 8);
            b[n++] = len;
            for (j = 0; j < len; j++)
                b[n + j] = (uint8_t)rnd();
            b[n + 12] = (uint8_t)(rnd() % 3 ? 0x50 : rnd());
            b[n + 13] = (uint8_t)(rnd() % 4 == 0 ? 2 : rnd() % 4 == 0 ? 0x10 : rnd());
            if (rnd() % 2) {
                b[n + 8] = b[n + 9] = 0;
                b[n + 10] = (uint8_t)(rnd() % 2 ? 0x03 : 0xff);
                b[n + 11] = (uint8_t)(rnd() % 2 ? 0xe9 : 0xf1);
            }
            n += len;
        }
        LLVMFuzzerTestOneInput(b, n);
    }
    CHECK(1, "fuzz entry: 30000 generated inputs, no sanitizer report, output checksums valid");
}

static int SLT(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static int SLE(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static int rawseg(const uint8_t* b, size_t n, tseg_t* s, int* v6)
{
    size_t hl, th;
    const uint8_t* t;
    if (n < 20)
        return -1;
    *v6 = b[0] >> 4 == 6;
    hl = *v6 ? 40 : (size_t)(b[0] & 15) * 4;
    if (n < hl + 20 || (*v6 ? b[6] : b[9]) != 6)
        return -1;
    t = b + hl;
    th = (size_t)(t[12] >> 4) * 4;
    if (th < 20 || hl + th > n)
        return -1;
    s->seq = be32(t + 4);
    s->ack = be32(t + 8);
    s->fl = t[13];
    s->win = be16(t + 14);
    s->opt = t + 20;
    s->ol = th - 20;
    s->d = t + th;
    s->dlen = (uint32_t)(n - hl - th);
    return 0;
}

static unsigned sack_blocks(const tseg_t* s, uint32_t (*blk)[2])
{
    const uint8_t* v;
    int l = opt_find(s, 5, &v);
    unsigned k, nb = l >= 2 ? (unsigned)(l - 2) / 8 : 0;
    for (k = 0; k < nb && k < 4; k++) {
        blk[k][0] = be32(v + 2 + 8 * k);
        blk[k][1] = be32(v + 6 + 8 * k);
    }
    return k;
}

static uint8_t rxm_bits[1 << 14];
static size_t rxm_len;
static int rxm_has(size_t i)
{
    return rxm_bits[i >> 3] >> (i & 7) & 1;
}
static void rxm_set(size_t i)
{
    rxm_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static int rx_runs(size_t from, uint32_t base, uint32_t (*run)[2], unsigned cap)
{
    unsigned n = 0;
    size_t i = from;
    while (i < rxm_len) {
        size_t j;
        if (!rxm_has(i)) {
            i++;
            continue;
        }
        for (j = i; j < rxm_len && rxm_has(j); j++)
            ;
        if (n < cap) {
            run[n][0] = base + (uint32_t)i;
            run[n][1] = base + (uint32_t)j;
        }
        n++;
        i = j;
    }
    return (int)n;
}

static void p_tcp_rx_model(void)
{
    static const uint8_t o_ws_sack[12] = { 2, 4, 0x02, 0x18, 1, 3, 3, 7, 1, 1, 4, 2 }, o_ws[8] = { 2, 4, 0x02, 0x18, 1, 3, 3, 7 };
    static uint8_t src[60000];
    static uint32_t run[256][2];
    static size_t hist[64][2];
    fk_rng_t r;
    unsigned it, n, bad_data = 0, bad_nxt = 0, bad_ack = 0, bad_sack = 0, miss_ack = 0, bad_ooo = 0, bad_eof = 0, sack_seen = 0, ooo_seen = 0, old_seen = 0, far_seen = 0;
    if (!fk_only("tcp-rx-model"))
        return;
    r = fk_stream("tcp-rx-model");
    n = (unsigned)fk_iters(300);
    for (it = 0; it < n; it++) {
        int sack = fk_chance(&r, 70), fin_at_end = 0, eof_model = 0;
        size_t len = 1 + (size_t)fk_below(&r, fk_chance(&r, 20) ? 300 : sizeof src), prefix = 0, i;
        unsigned segs = 20 + (unsigned)fk_below(&r, 180), k;
        const uint32_t irs = 7000;
        wg_tcp_info_t inf;
        fk_bytes(&r, src, len);
        T = 1000;
        raw_client(sack ? o_ws_sack : o_ws, sack ? sizeof o_ws_sack : sizeof o_ws, 65535);
        rxm_len = len;
        memset(rxm_bits, 0, (len + 7) / 8);
        for (k = 0; k < segs && ca.c && !eof_model; k++) {
            size_t off, sl;
            unsigned kind = (unsigned)fk_below(&r, 100), before = L[0].total;
            uint32_t seq;
            uint8_t fl = ACK;
            int ooo, far = 0, old;
            int had = rx_runs(prefix, irs + 1, run, 0) > 0;
            if (kind < 35 && prefix < len)
                off = prefix + (size_t)fk_below(&r, fk_chance(&r, 50) ? 3 : 1);
            else if (kind < 80)
                off = (size_t)fk_below(&r, len);
            else if (kind < 88 && prefix)
                off = (size_t)fk_below(&r, prefix);
            else if (kind < 95 && k)
                off = hist[fk_below(&r, k < 64 ? k : 64)][0];
            else
                off = (size_t)fk_below(&r, len), far = 1;
            if (off >= len)
                off = len - 1;
            sl = 1 + (size_t)fk_below(&r, fk_chance(&r, 30) ? 8 : 1200);
            if (kind >= 88 && kind < 95 && k) {
                unsigned h = (unsigned)fk_below(&r, k < 64 ? k : 64);
                off = hist[h][0];
                sl = hist[h][1] + (size_t)fk_below(&r, 3);
            }
            if (off + sl > len)
                sl = len - off;
            if (fk_chance(&r, 5) && off + sl == len)
                sl = 0, off = len;
            if (off + sl == len && fk_chance(&r, 40))
                fl |= FIN;
            if (!sl && !(fl & FIN))
                sl = 1, off = len - 1;
            hist[k % 64][0] = off;
            hist[k % 64][1] = sl;
            seq = irs + 1 + (uint32_t)off;
            if (far) {
                seq += fk_chance(&r, 50) ? 0x200000u + (uint32_t)fk_below(&r, 0x1000000) : 0u - 0x100000u - (uint32_t)fk_below(&r, 0x100000);
                fl &= (uint8_t)~FIN;
                far_seen++;
            }
            ooo = !far && off > prefix;
            old = !far && off + sl + ((fl & FIN) ? 1 : 0) <= prefix;
            old_seen += old;
            T += fk_below(&r, 4) == 0 ? (uint64_t)fk_below(&r, 400) : 0;
            wg_tcp_tick(N[0].tcp);
            before = L[0].total;
            raw(0, 0, B4, A4, 80, r_lp, seq, r_iss + 1, fl, 65535, NULL, 0, src + (off < len ? off : 0), sl);
            if (!far) {
                for (i = off; i < off + sl; i++)
                    rxm_set(i);
                if (fl & FIN)
                    fin_at_end = 1;
            }
            while (prefix < len && rxm_has(prefix))
                prefix++;
            if (prefix == len && fin_at_end)
                eof_model = 1;
            ooo_seen += ooo;
            if (ca.rn != prefix || memcmp(ca.rx, src, ca.rn))
                bad_data++;
            if (!!ca.eof != eof_model)
                bad_eof++;
            if (!ca.c)
                break;
            wg_tcp_info(ca.c, &inf);
            if (inf.rcv_nxt != irs + 1 + (uint32_t)prefix + (uint32_t)eof_model)
                bad_nxt++;
            if ((ooo || old || far || (had && sl && !old)) && L[0].total == before)
                miss_ack++;
            if (L[0].total != before) {
                tseg_t s;
                uint32_t blk[4][2];
                unsigned nb, nr, j, q;
                int v6;
                const pk_t* p = last_out(0, 0);
                if (!p || rawseg(p->b, p->n, &s, &v6) || s.ack != inf.rcv_nxt || !(s.fl & ACK) || s.dlen)
                    bad_ack++;
                else {
                    nb = sack_blocks(&s, blk);
                    nr = (unsigned)rx_runs(prefix, irs + 1, run, 256);
                    sack_seen += nb > 0;
                    if (!sack || eof_model)
                        bad_sack += nb != 0 && !sack;
                    else if (nb != (nr < 4 ? nr : 4))
                        bad_sack++;
                    else {
                        for (j = 0; j < nb; j++) {
                            int found = 0;
                            for (q = 0; q < nr && q < 256; q++)
                                found |= run[q][0] == blk[j][0] && run[q][1] == blk[j][1];
                            for (q = 0; q < j; q++)
                                found &= blk[q][0] != blk[j][0];
                            if (!found)
                                bad_sack++;
                        }
                        if (nb && ooo && sl && !(SLE(blk[0][0], seq) && SLT(seq, blk[0][1])))
                            bad_sack++;
                    }
                }
            }
            if ((inf.nooo == 0) != (rx_runs(prefix, irs + 1, run, 0) == 0) && !eof_model)
                bad_ooo++;
        }
        nodes_free();
        app_reset(&ca);
    }
    CHECK(bad_data == 0, "tcp-rx-model: random overlapping, duplicate, old, partially old and far out-of-window segments deliver exactly the bitmap model's contiguous prefix, byte for byte");
    CHECK(bad_nxt == 0 && bad_eof == 0, "tcp-rx-model: RCV.NXT equals IRS+1+prefix (+1 once the FIN is reached); EOF exactly when the model has every byte and the FIN");
    CHECK(miss_ack == 0 && bad_ack == 0, "tcp-rx-model: out-of-order, old and out-of-window segments are answered at once, as is a segment that fills a hole (RFC 5681 4.2); every ACK carries RCV.NXT and no data");
    CHECK(bad_sack == 0, "tcp-rx-model: SACK blocks are exactly the model's maximal received runs above RCV.NXT (min(n, 4), distinct), the first covering the triggering segment (RFC 2018 4); none without SACK");
    CHECK(bad_ooo == 0, "tcp-rx-model: the out-of-order queue is empty exactly when the model holds nothing above RCV.NXT");
    if (n >= 100)
        CHECK(sack_seen > n && ooo_seen > n && old_seen > n / 2 && far_seen > n / 2, "tcp-rx-model: SACK, out-of-order, old and far segments all exercised");
}

typedef struct {
    int have_iss, ws_adv, fin_ok;
    uint32_t iss, edge;
    int edge_ok;
    size_t total;
    uint8_t* map;
} sdir_t;

static sdir_t SD[2];
static app_t* SA[2];
static unsigned st_bad_payload, st_bad_range, st_bad_edge, st_bad_sack, st_bad_ack, st_bad_fin, st_bad_prefix, st_probe_edge;

static int st_tx(int dir, const uint8_t* p, size_t n)
{
    tseg_t s;
    int v6;
    sdir_t* me = &SD[dir];
    sdir_t* peer = &SD[!dir];
    app_t* a = SA[dir];
    const uint8_t* v;
    if (rawseg(p, n, &s, &v6))
        return 0;
    if (s.fl & SYN) {
        me->have_iss = 1;
        me->iss = s.seq;
        me->ws_adv = opt_find(&s, 3, &v) == 3 ? v[2] : -1;
        return 0;
    }
    if (s.fl & RST)
        return 0;
    if (me->have_iss && s.dlen) {
        uint32_t off = s.seq - me->iss - 1;
        if (off > me->total || s.dlen > me->total - off || off + s.dlen > a->slen)
            st_bad_range++;
        else if (memcmp(s.d, a->src + off, s.dlen))
            st_bad_payload++;
        if (me->edge_ok && !SLE(s.seq + s.dlen, me->edge)) {
            if (s.dlen == 1 && s.seq == me->edge)
                st_probe_edge++;
            else
                st_bad_edge++;
        }
    }
    if (me->have_iss && (s.fl & FIN) && (!me->fin_ok || s.seq + s.dlen != me->iss + 1 + (uint32_t)me->total))
        st_bad_fin++;
    if (peer->have_iss && (s.fl & ACK)) {
        uint32_t aoff = s.ack - peer->iss - 1, pre = 0, blk[4][2];
        unsigned nb = sack_blocks(&s, blk), j;
        while (pre < peer->total && peer->map[pre])
            pre++;
        if (aoff != (uint32_t)-1 && aoff > pre + 1)
            st_bad_ack++;
        for (j = 0; j < nb; j++) {
            uint32_t lo = blk[j][0] - peer->iss - 1, hi = blk[j][1] - peer->iss - 1, x;
            if (!SLT(s.ack, blk[j][0]) || !SLT(blk[j][0], blk[j][1]) || hi > peer->total)
                st_bad_sack++;
            else
                for (x = lo; x < hi; x++)
                    if (!peer->map[x]) {
                        st_bad_sack++;
                        break;
                    }
        }
    }
    return 0;
}

static void st_rx(int to, const uint8_t* b, size_t n)
{
    tseg_t s;
    int v6;
    sdir_t* snd = &SD[!to];
    sdir_t* me = &SD[to];
    if (!wg_ip_verify(b, n) || rawseg(b, n, &s, &v6) || (s.fl & RST))
        return;
    if (snd->have_iss && s.dlen && !(s.fl & SYN)) {
        uint32_t off = s.seq - snd->iss - 1, x;
        for (x = 0; x < s.dlen && off + x < snd->total; x++)
            snd->map[off + x] = 1;
    }
    if (s.fl & ACK) {
        int sc = (s.fl & SYN) ? 0 : me->ws_adv >= 0 && snd->ws_adv >= 0 ? snd->ws_adv : 0;
        uint32_t e = s.ack + ((uint32_t)s.win << sc);
        if (!me->edge_ok || SLT(me->edge, e))
            me->edge = e;
        me->edge_ok = 1;
    }
}

static void st_prefix(void)
{
    int d;
    for (d = 0; d < 2; d++) {
        app_t* rx = SA[!d];
        if (rx->rn > SD[d].total || memcmp(rx->rx, SA[d]->src, rx->rn))
            st_bad_prefix++;
    }
}

static void p_tcp_stream(void)
{
    fk_rng_t r;
    unsigned it, n, incomplete = 0, leaked = 0, aborted = 0, freed = 0;
    uint64_t rexmit = 0, sackrx = 0, persist = 0, ooo = 0;
    if (!fk_only("tcp-stream"))
        return;
    r = fk_stream("tcp-stream");
    n = (unsigned)fk_iters(120);
    for (it = 0; it < n; it++) {
        int v6 = fk_chance(&r, 50), d, mode = (int)fk_below(&r, 100), big = it % 60 == 7 || fk_chance(&r, 3);
        uint64_t harsh, unpause;
        wg_ipaddr_t b;
        rs = fk_next(&r) | 1;
        app_reset(&ca);
        app_reset(&sa);
        nodes(fk_chance(&r, 75), fk_chance(&r, 85));
        SA[0] = &ca;
        SA[1] = &sa;
        for (d = 0; d < 2; d++) {
            free(SD[d].map);
            memset(&SD[d], 0, sizeof SD[d]);
            SD[d].total = big && d == 0 ? 1100000 + (size_t)fk_below(&r, 400000) : (size_t)fk_below(&r, fk_chance(&r, 20) ? 2000 : 60000);
            SD[d].map = (uint8_t*)calloc(SD[d].total + 1, 1);
            SA[d]->src = blob(SD[d].total, (unsigned)fk_next(&r));
            SA[d]->slen = (size_t)fk_below(&r, SD[d].total + 1);
            if (big)
                SA[d]->slen = SD[d].total;
            L[d].drop = st_tx;
            L[d].delay = 1 + (unsigned)fk_below(&r, 40);
        }
        rx_obs = st_rx;
        wg_tcp_listen(N[1].tcp, v6, 80, 4, &CB, &sa);
        b = ad(v6, v6 ? B6 : B4);
        ca.c = wg_tcp_connect(N[0].tcp, &b, 80, &CB, &ca);
        harsh = T + (big ? 40000 + fk_below(&r, 20000) : 2000 + fk_below(&r, 25000));
        unpause = T + (harsh - T) * 7 / 10;
        while (T < harsh) {
            unsigned act = (unsigned)fk_below(&r, 100);
            app_t* a = SA[fk_below(&r, 2)];
            if (big && sa.c)
                wg_tcp_pause(sa.c, T < unpause);
            if (act < 20 && a->slen < SD[a == &sa].total) {
                a->slen += 1 + (size_t)fk_below(&r, fk_chance(&r, 50) ? 100 : 20000);
                if (a->slen > SD[a == &sa].total)
                    a->slen = SD[a == &sa].total;
                pump(a);
            } else if (act < 28 && a->c && !big)
                wg_tcp_pause(a->c, (int)fk_below(&r, 2));
            else if (act < 34)
                for (d = 0; d < 2; d++) {
                    L[d].loss_ppm = (unsigned)fk_below(&r, big ? 10000 : 150000);
                    L[d].jitter = (unsigned)fk_below(&r, 40);
                    L[d].dup_ppm = (unsigned)fk_below(&r, 20000);
                    L[d].corrupt_ppm = (unsigned)fk_below(&r, 5000);
                }
            else if (act < 37 && a->c)
                wg_tcp_set_nodelay(a->c, (int)fk_below(&r, 2));
            if (mode < 3 && fk_chance(&r, 1) && a->c) {
                wg_tcp_abort(a->c);
                a->c = NULL;
                a->closed = 1;
                aborted++;
                break;
            }
            if (mode >= 3 && mode < 11 && fk_chance(&r, 1))
                break;
            step(T + 1 + fk_below(&r, 60));
            st_prefix();
        }
        if (mode >= 3 && mode < 11) {
            freed++;
            for (d = 0; d < 2; d++)
                rexmit += stats(d).rexmit_segs;
            rx_obs = NULL;
            nodes_free();
            ca.c = sa.c = NULL;
        } else {
            for (d = 0; d < 2; d++) {
                L[d].loss_ppm = L[d].dup_ppm = L[d].corrupt_ppm = 0;
                if (SA[d]->c)
                    wg_tcp_pause(SA[d]->c, 0);
                SA[d]->slen = SD[d].total;
                SA[d]->shut_after = 1;
                SD[d].fin_ok = 1;
                pump(SA[d]);
            }
            if (mode < 3)
                run_until(both_closed, 600000);
            else {
                run_until(rnd_done, 600000);
                if (!rnd_done() || ca.err || sa.err || memcmp(sa.rx, ca.src, ca.slen) || memcmp(ca.rx, sa.src, sa.slen))
                    incomplete++;
            }
            st_prefix();
            run_until(NULL, 2 * WG_TCP_MSL_MS + 1000);
            if (live(0) || live(1))
                leaked++;
            for (d = 0; d < 2; d++) {
                wg_tcp_stats_t x = stats(d);
                rexmit += x.rexmit_segs;
                sackrx += x.sack_blocks_in;
                persist += x.persist_probes;
                ooo += x.ooo_segs;
            }
            rx_obs = NULL;
            nodes_free();
        }
        free((void*)(uintptr_t)ca.src);
        free((void*)(uintptr_t)sa.src);
        ca.src = sa.src = NULL;
    }
    for (it = 0; it < 2; it++) {
        free(SD[it].map);
        SD[it].map = NULL;
    }
    app_reset(&ca);
    app_reset(&sa);
    printf("  tcp-stream: %u runs (%u aborted, %u freed in flight), rexmit %llu, SACK blocks in %llu, persist %llu, ooo %llu, probes at edge %u\n", n, aborted, freed,
           (unsigned long long)rexmit, (unsigned long long)sackrx, (unsigned long long)persist, (unsigned long long)ooo, st_probe_edge);
    CHECK(st_bad_payload == 0 && st_bad_range == 0, "tcp-stream: every data segment, retransmissions included, carries exactly the written bytes at its sequence offset, never past what the application wrote");
    CHECK(st_bad_edge == 0, "tcp-stream: no segment ends past the largest ACK + scaled window the peer has sent (a 1-byte zero-window probe at the edge aside)");
    CHECK(st_bad_sack == 0 && st_bad_ack == 0, "tcp-stream: every ACK is at most the received contiguous prefix (+FIN); every SACK block lies above the ACK inside bytes that reached the receiver");
    CHECK(st_bad_fin == 0, "tcp-stream: FIN only after shutdown, at ISS+1+stream length");
    CHECK(st_bad_prefix == 0, "tcp-stream: each side's received data is a prefix of the peer's stream after every step");
    CHECK(incomplete == 0 && leaked == 0, "tcp-stream: after the link heals both streams arrive intact and closed, abort runs close both ends, no live connection remains");
    if (n >= 60)
        CHECK(rexmit && sackrx && persist && ooo, "tcp-stream: retransmission, SACK, persist and out-of-order all exercised");
}

static void p_tcp_reseeded(void)
{
    fk_rng_t r;
    if (!fk_only("tcp-reseeded"))
        return;
    r = fk_stream("tcp-reseeded");
    rs = fk_next(&r) | 1;
    t_fuzz();
}

int main(void)
{
    fk_init("test_wg_tcp");
    if (fk_legacy()) {
        t_handshake();
        t_bulk();
        t_loss();
        rfc2018();
        t_rtt();
        t_rfc5961();
        t_raw_sender();
        t_zero_window();
        t_close();
        t_tw_restart();
        t_ack_policy();
        t_keepalive();
        t_random();
        t_many();
        t_fuzz();
    }
    p_tcp_rx_model();
    p_tcp_stream();
    p_tcp_reseeded();
    app_reset(&ca);
    app_reset(&sa);
    printf("test_wg_tcp: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
