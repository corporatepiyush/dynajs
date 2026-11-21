/*
 * test_wg_device.c -- dyna:wireguard S13b2 device layer (src/modules/wireguard/wg-device.c): real UDP sockets on
 * 127.0.0.1, the real aio loop and lock-free pool (handshakes and data batches run on workers, results applied on the
 * loop thread), an injected clock so timers are driven without sleeping. A test-owned UDP relay sits between some
 * device pairs to drop, duplicate, tamper with and re-source datagrams.
 *  1. Handshake on first packet, 3000 IPv4 packets (20..1420 B) each way in send order; the responder learns the
 *     initiator's endpoint from the first authenticated packet; both batched (pool) and inline paths are used.
 *  2. Cryptokey routing: an inner source outside the sender's allowed IPs is dropped; no route means send fails; IPv6.
 *  3. Relay: duplicated datagrams are delivered once (replay window); a tampered or replayed packet from a new address
 *     does not move the endpoint; authentic packets from a new address do (roaming).
 *  4. Under load (threshold 0): initiations get a cookie reply, the retry 5 s later carries mac2 and completes.
 *  5. Timers: keepalive 10 s after unanswered data; initiator rekeys after 120 s and traffic continues on the new
 *     keypair; with the path down, 1 + 19 retransmissions then give-up, keys zeroed at 540 s.
 *  6. 32 spokes on one hub: every spoke's packets reach the hub in order (64 keypairs in the index table).
 *  7. Removing a peer with jobs in flight, and freeing devices with work pending (ASan/UBSan: no leak, no UAF).
 *  8. add_peer when the pool refuses the job (wg-device.c is built with dyn_aio_offload_cpu -> wg_test_offload_cpu):
 *     X25519 runs inline, the callback fires before add_peer returns; a low-order key returns NULL, never a freed peer.
 *  9. Held pool jobs (the injected offload queues jobs; the test runs them in a chosen order on the loop thread):
 *     an initiation for a peer whose X25519 is still pending is dropped (no response from zero keys); a response
 *     consumed after the initiator already re-initiated (new local index, new generation) installs no keypair.
 *     In 7, data sent while the peer is pending queues no initiation job until X25519 is done.
 * 10. dev-ops (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks): random
 *     sends, pumps, clock jumps, remove/re-add with held jobs released in random order, relay drop/dup, load and
 *     verify toggles; every delivery is exact, from an allowed source, once, to a live peer; heal, then free with
 *     traffic in flight. Equivalent: dropping the k->dead or pjob dead checks (already guarded upstream).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../src/dyna-aio.h"
#include "../lib/dyn-fuzzkit.h"
#include "../../src/modules/wireguard/wg-internal.h"
#include "../../src/modules/wireguard/wg-ip.h"

static int fails, checks;
#define CHECK(c, msg)                                          \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            fails++;                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                      \
    } while (0)

static uint64_t rs = 0x5deece66dULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static uint64_t T_ms = 1000000, T_ns = 1800000000000000000ULL;
static uint64_t clk(void* ud, int real)
{
    (void)ud;
    return real ? T_ns + T_ms * 1000000ULL : T_ms;
}

typedef struct {
    wg_dev_t* d;
    uint32_t got, bad_order, last_seq, first_seq, bytes;
    uint8_t last_src[16];
} node_t;

static unsigned rx_verified;
static void on_rx(void* ud, wg_dpeer_t* p, const uint8_t* ip, size_t n, int verified)
{
    node_t* x = (node_t*)ud;
    rx_verified += (unsigned)!!verified;
    uint32_t seq = n >= 24 ? (uint32_t)ip[20] << 24 | (uint32_t)ip[21] << 16 | (uint32_t)ip[22] << 8 | ip[23] : 0;
    (void)p;
    if (x->got && seq <= x->last_seq)
        x->bad_order++;
    if (!x->got)
        x->first_seq = seq;
    x->last_seq = seq;
    x->got++;
    x->bytes += (uint32_t)n;
    memcpy(x->last_src, ip + 12, 4);
}

static dyn_aio_t* aio;
static int relay = -1, relay2 = -1;
static struct sockaddr_in rel_e, rel_f;
static int relay_mode, relay_use2;
static uint8_t captured[2048];
static ssize_t captured_n;

enum { R_FWD, R_DROP, R_DUP, R_CAPTURE, R_HOLDINIT, R_HOLDDATA };
static uint8_t held[148];
static int held_n;

static void relay_pump(int fd)
{
    uint8_t b[2048];
    struct sockaddr_in from;
    socklen_t fl;
    ssize_t n;
    for (;;) {
        int out;
        fl = sizeof from;
        n = recvfrom(fd, b, sizeof b, 0, (struct sockaddr*)&from, &fl);
        if (n < 0)
            break;
        if (from.sin_port == rel_e.sin_port) {
            if ((relay_mode == R_CAPTURE || relay_mode == R_HOLDDATA) && n > 32 && b[0] == 4 && !captured_n) {
                memcpy(captured, b, (size_t)n);
                captured_n = n;
            }
            if (relay_mode == R_HOLDINIT && n == 148 && b[0] == 1 && !held_n) {
                memcpy(held, b, 148);
                held_n = 1;
            }
            if (relay_mode == R_DROP || relay_mode == R_HOLDINIT || relay_mode == R_HOLDDATA)
                continue;
            out = relay_use2 ? relay2 : relay;
            sendto(out, b, (size_t)n, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
            if (relay_mode == R_DUP)
                sendto(out, b, (size_t)n, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
        } else if (relay_mode != R_DROP && relay_mode != R_HOLDINIT)
            sendto(relay, b, (size_t)n, 0, (struct sockaddr*)&rel_e, sizeof rel_e);
    }
}

static node_t* all_nodes[40];
static int n_nodes;

static void pump(int rounds)
{
    int i, k;
    for (i = 0; i < rounds; i++) {
        dyn_aio_run_us(aio, 1000);
        if (relay >= 0) {
            relay_pump(relay);
            relay_pump(relay2);
        }
        for (k = 0; k < n_nodes; k++) {
            wg_dev_flush(all_nodes[k]->d);
            wg_dev_tick(all_nodes[k]->d);
        }
    }
}

static void wait_ready(wg_dpeer_t* const* ps, int n)
{
    int k, i;
    for (k = 0; k < 5000; k++) {
        for (i = 0; i < n && wg_dev_peer_ready(ps[i]); i++)
            ;
        if (i == n)
            return;
        pump(1);
    }
    printf("FAIL peers never became ready\n");
    fails++;
}

static void pump_until(uint32_t* v, uint32_t want, int max)
{
    while (max-- > 0 && *v < want)
        pump(1);
    pump(5);
}

static int udp_sock(void)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr*)&a, sizeof a);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

static struct sockaddr_in addr_of(int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    return a;
}

static struct sockaddr_in sock_addr(int fd)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    getsockname(fd, (struct sockaddr*)&a, &l);
    return a;
}

static node_t* node(uint8_t* pub)
{
    uint8_t priv[32];
    node_t* x = (node_t*)calloc(1, sizeof *x);
    wg_keygen(priv, pub);
    x->d = wg_dev_new(aio, priv, "127.0.0.1", 0, on_rx, x);
    wg_dev_set_clock(x->d, clk, NULL);
    all_nodes[n_nodes++] = x;
    return x;
}

static void node_free(node_t* x)
{
    int k;
    wg_dev_free(x->d);
    for (k = 0; k < n_nodes; k++)
        if (all_nodes[k] == x)
            all_nodes[k] = all_nodes[--n_nodes];
    free(x);
}

static size_t ip4(uint8_t* b, const uint8_t src[4], const uint8_t dst[4], uint32_t seq, size_t n)
{
    size_t i;
    memset(b, 0, 24);
    b[0] = 0x45;
    b[2] = (uint8_t)(n >> 8);
    b[3] = (uint8_t)n;
    b[9] = 17;
    memcpy(b + 12, src, 4);
    memcpy(b + 16, dst, 4);
    b[20] = (uint8_t)(seq >> 24);
    b[21] = (uint8_t)(seq >> 16);
    b[22] = (uint8_t)(seq >> 8);
    b[23] = (uint8_t)seq;
    for (i = 24; i < n; i++)
        b[i] = (uint8_t)(i * 7 + seq);
    return n;
}

static const uint8_t IP_A[4] = { 10, 0, 0, 1 }, IP_B[4] = { 10, 0, 0, 2 }, IP_X[4] = { 10, 0, 0, 9 };

static void link_pair(node_t* a, const uint8_t* apub, node_t* b, const uint8_t* bpub, wg_dpeer_t** pa, wg_dpeer_t** pb, const struct sockaddr_in* a_to_b)
{
    *pa = wg_dev_add_peer(a->d, bpub, NULL, 0, NULL, NULL);
    *pb = wg_dev_add_peer(b->d, apub, NULL, 0, NULL, NULL);
    wait_ready(pa, 1);
    wait_ready(pb, 1);
    wg_dev_set_endpoint(*pa, (const struct sockaddr*)a_to_b, sizeof *a_to_b);
    wg_dev_allow(a->d, *pa, 0, IP_B, 32);
    wg_dev_allow(b->d, *pb, 0, IP_A, 32);
}

static uint32_t sent_seq;
static void burst(node_t* from, const uint8_t* src, const uint8_t* dst, int n, int big)
{
    static uint8_t b[1500];
    int i;
    for (i = 0; i < n; i++) {
        size_t len = big ? 24 + (size_t)(rnd() % 1397) : 24 + (size_t)(rnd() % 200);
        ip4(b, src, dst, ++sent_seq, len);
        wg_dev_send(from->d, b, len);
    }
    wg_dev_flush(from->d);
}

static void t_basic(void)
{
    uint8_t apub[32], bpub[32], b[1500];
    node_t *a = node(apub), *bn = node(bpub);
    wg_dpeer_t *pa, *pb;
    struct sockaddr_in to = addr_of(wg_dev_port(bn->d)), ep;
    unsigned el = sizeof ep;
    wg_dev_stats_t sa, sb;
    int i;
    CHECK(a->d && bn->d, "devices");
    link_pair(a, apub, bn, bpub, &pa, &pb, &to);
    CHECK(wg_dev_get_endpoint(pb, (struct sockaddr*)&ep, &el) < 0, "responder has no endpoint yet");
    burst(a, IP_A, IP_B, 200, 0);
    pump_until(&bn->got, 128, 2000);
    CHECK(bn->got == 128 && bn->first_seq == 73 && bn->last_seq == 200 && bn->bad_order == 0, "200 packets staged before the handshake: the newest 128 delivered in order");
    wg_dev_stats(a->d, &sa);
    CHECK(sa.hs_init_sent == 1 && sa.hs_done == 1, "one handshake");
    el = sizeof ep;
    CHECK(wg_dev_get_endpoint(pb, (struct sockaddr*)&ep, &el) == 0 && ep.sin_port == htons((uint16_t)wg_dev_port(a->d)), "responder learned the endpoint");
    for (i = 0; i < 3000 / 100; i++) {
        burst(a, IP_A, IP_B, 100, 1);
        pump(3);
    }
    pump_until(&bn->got, 3128, 5000);
    CHECK(bn->got == 3128 && bn->bad_order == 0, "3000 packets A->B delivered in send order");
    for (i = 0; i < 50; i++) {
        burst(a, IP_A, IP_B, 64, 1);
        burst(a, IP_A, IP_B, 1, 0);
        pump(2);
    }
    pump_until(&bn->got, 3128 + 50 * 65, 5000);
    CHECK(bn->got == 3128 + 50 * 65 && bn->bad_order == 0, "a lone small packet never overtakes a batch in flight");
    sent_seq = 0;
    for (i = 0; i < 30; i++) {
        burst(bn, IP_B, IP_A, 100, i & 1);
        pump(3);
    }
    pump_until(&a->got, 3000, 5000);
    CHECK(a->got == 3000 && a->bad_order == 0, "3000 packets B->A delivered in send order over the learned endpoint");
    wg_dev_stats(a->d, &sa);
    wg_dev_stats(bn->d, &sb);
    pump(20);
    burst(a, IP_A, IP_B, 1, 0);
    pump_until(&bn->got, 3128 + 50 * 65 + 1, 2000);
    wg_dev_stats(a->d, &sa);
    CHECK(sa.jobs_offloaded > 0 && sb.jobs_offloaded > 0 && sa.inline_ops > 0, "batches run on the pool, a lone small packet inline");
    CHECK(sb.hs_resp_sent == 1 && sb.hs_done == 0 && sa.hs_done == 1, "no extra handshakes");
    ip4(b, IP_X, IP_B, ++sent_seq, 60);
    wg_dev_send(a->d, b, 60);
    wg_dev_flush(a->d);
    pump(30);
    wg_dev_stats(bn->d, &sb);
    CHECK(bn->got == 3128 + 50 * 65 + 1 && sb.rx_drop >= 1, "inner source outside allowed IPs dropped");
    ip4(b, IP_A, IP_X, 1, 60);
    CHECK(wg_dev_send(a->d, b, 60) < 0, "no route: send fails");
    CHECK(wg_dev_send(a->d, b, 10) < 0 && wg_dev_send(a->d, b, 1421) < 0, "short or over-MTU packet refused");
    {
        static const uint8_t s6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, d6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
        uint32_t before = bn->got;
        wg_dev_allow(a->d, pa, 1, d6, 128);
        wg_dev_allow(bn->d, pb, 1, s6, 128);
        memset(b, 0, 100);
        b[0] = 0x60;
        b[5] = 60;
        b[6] = 17;
        memcpy(b + 8, s6, 16);
        memcpy(b + 24, d6, 16);
        CHECK(wg_dev_send(a->d, b, 100) == 0, "IPv6 routed");
        wg_dev_flush(a->d);
        pump_until(&bn->got, before + 1, 2000);
        CHECK(bn->got == before + 1, "IPv6 delivered");
    }
    node_free(a);
    node_free(bn);
}

static void relay_setup(node_t* e, node_t* f)
{
    relay = udp_sock();
    relay2 = udp_sock();
    rel_e = addr_of(wg_dev_port(e->d));
    rel_f = addr_of(wg_dev_port(f->d));
    relay_mode = R_FWD;
    relay_use2 = 0;
    captured_n = 0;
}

static void relay_close(void)
{
    close(relay);
    close(relay2);
    relay = relay2 = -1;
}

static void t_relay(void)
{
    uint8_t epub[32], fpub[32], tam[2048];
    node_t *e = node(epub), *f = node(fpub);
    wg_dpeer_t *pe, *pf;
    struct sockaddr_in r, ep, r2;
    unsigned el;
    wg_dev_stats_t sf0, sf1;
    relay_setup(e, f);
    r = sock_addr(relay);
    r2 = sock_addr(relay2);
    link_pair(e, epub, f, fpub, &pe, &pf, &r);
    sent_seq = 0;
    burst(e, IP_A, IP_B, 1, 0);
    pump_until(&f->got, 1, 2000);
    CHECK(f->got == 1, "handshake and data through the relay");
    {
        int k;
        wg_dev_stats(f->d, &sf0);
        relay_mode = R_HOLDINIT;
        held_n = 0;
        T_ms += 130000;
        burst(e, IP_A, IP_B, 1, 0);
        for (k = 0; k < 200 && !held_n; k++)
            pump(1);
        CHECK(held_n, "captured the rekey initiation");
        relay_mode = R_FWD;
        sendto(relay, held, 148, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
        pump(40);
        T_ms += 1000;
        sendto(relay2, held, 148, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
        pump(40);
        wg_dev_stats(f->d, &sf1);
        CHECK(sf1.hs_resp_sent == sf0.hs_resp_sent + 1, "a replayed initiation gets no second response");
        burst(e, IP_A, IP_B, 1, 0);
        pump_until(&f->got, 2, 2000);
        CHECK(f->got == 2, "handshake completes after the replay and data flows on the new keypair");
    }
    relay_mode = R_DUP;
    burst(e, IP_A, IP_B, 200, 0);
    pump_until(&f->got, 202, 3000);
    pump(20);
    wg_dev_stats(f->d, &sf0);
    CHECK(f->got == 202 && f->bad_order == 0 && sf0.rx_drop >= 150, "duplicated datagrams delivered once");
    relay_mode = R_CAPTURE;
    burst(e, IP_A, IP_B, 1, 0);
    pump_until(&f->got, 203, 2000);
    CHECK(captured_n > 32, "captured a data packet");
    memcpy(tam, captured, (size_t)captured_n);
    tam[14] ^= 1;
    wg_dev_stats(f->d, &sf0);
    sendto(relay2, tam, (size_t)captured_n, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
    sendto(relay2, captured, (size_t)captured_n, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
    pump(40);
    wg_dev_stats(f->d, &sf1);
    el = sizeof ep;
    wg_dev_get_endpoint(pf, (struct sockaddr*)&ep, &el);
    CHECK(ep.sin_port == r.sin_port && sf1.rx_drop >= sf0.rx_drop + 2 && sf1.roams == sf0.roams, "tampered and replayed packets from a new address do not move the endpoint");
    relay_mode = R_FWD;
    relay_use2 = 1;
    burst(e, IP_A, IP_B, 5, 0);
    pump_until(&f->got, 208, 2000);
    el = sizeof ep;
    wg_dev_get_endpoint(pf, (struct sockaddr*)&ep, &el);
    wg_dev_stats(f->d, &sf1);
    CHECK(f->got == 208 && ep.sin_port == r2.sin_port && sf1.roams == sf0.roams + 1, "authentic packets from a new address roam the endpoint");
    relay_close();
    node_free(e);
    node_free(f);
}

static void t_cookie(void)
{
    uint8_t epub[32], fpub[32];
    node_t *e = node(epub), *f = node(fpub);
    wg_dpeer_t *pe, *pf;
    struct sockaddr_in to = addr_of(wg_dev_port(f->d));
    wg_dev_stats_t se, sf;
    int i;
    wg_dev_set_load_threshold(f->d, 0);
    link_pair(e, epub, f, fpub, &pe, &pf, &to);
    sent_seq = 0;
    burst(e, IP_A, IP_B, 1, 0);
    pump(100);
    wg_dev_stats(e->d, &se);
    wg_dev_stats(f->d, &sf);
    CHECK(sf.cookie_sent == 1 && se.cookie_recv == 1 && f->got == 0 && se.hs_done == 0, "under load: cookie reply instead of a response");
    for (i = 0; i < 6 && !f->got; i++) {
        T_ms += 1000;
        pump(50);
    }
    wg_dev_stats(e->d, &se);
    CHECK(f->got == 1 && se.hs_done == 1 && se.hs_init_sent == 2, "retry after 5 s carries mac2 and completes");
    node_free(e);
    node_free(f);
}

static void t_timers(void)
{
    uint8_t epub[32], fpub[32];
    node_t *e = node(epub), *f = node(fpub);
    wg_dpeer_t *pe, *pf;
    struct sockaddr_in r;
    wg_dev_stats_t se, se0, sf, sf1;
    const wg_keypair_t* k0;
    int i;
    relay_setup(e, f);
    r = sock_addr(relay);
    link_pair(e, epub, f, fpub, &pe, &pf, &r);
    sent_seq = 0;
    burst(e, IP_A, IP_B, 1, 0);
    pump_until(&f->got, 1, 2000);
    T_ms += 10000;
    pump(30);
    wg_dev_stats(e->d, &se);
    wg_dev_stats(f->d, &sf);
    CHECK(sf.keepalive_sent >= 1 && se.rx_keepalive >= 1, "keepalive 10 s after unanswered data");
    se0 = se;
    T_ms += 11000;
    pump(30);
    wg_dev_stats(e->d, &se);
    CHECK(se.keepalive_sent == se0.keepalive_sent, "a received keepalive is not answered with a keepalive");
    k0 = wg_dev_current(pe);
    T_ms += 111000;
    burst(e, IP_A, IP_B, 1, 0);
    pump(60);
    wg_dev_stats(e->d, &se);
    CHECK(se.hs_done == 2 && wg_dev_current(pe) != k0, "initiator rekeys after 120 s");
    burst(e, IP_A, IP_B, 20, 0);
    pump_until(&f->got, 22, 2000);
    CHECK(f->got == 22 && f->bad_order == 0, "traffic continues on the new keypair");
    relay_mode = R_HOLDDATA;
    captured_n = 0;
    burst(e, IP_A, IP_B, 1, 0);
    for (i = 0; i < 200 && !captured_n; i++)
        pump(1);
    wg_dev_stats(f->d, &sf);
    relay_mode = R_DROP;
    wg_dev_stats(e->d, &se0);
    T_ms += 181000;
    sendto(relay, captured, (size_t)captured_n, 0, (struct sockaddr*)&rel_f, sizeof rel_f);
    pump(20);
    wg_dev_stats(f->d, &sf1);
    CHECK(captured_n > 32 && f->got == 22 && sf1.rx_drop == sf.rx_drop + 1, "authentic packet on a keypair older than 180 s is rejected");
    burst(e, IP_A, IP_B, 1, 0);
    pump(20);
    for (i = 0; i < 30; i++) {
        T_ms += 5400;
        pump(10);
    }
    wg_dev_stats(e->d, &se);
    CHECK(se.hs_init_sent - se0.hs_init_sent == 20 && f->got == 22, "path down: 1 + 19 handshake attempts, then give up");
    T_ms += 540000;
    pump(10);
    CHECK(wg_dev_current(pe) == NULL, "keys zeroed 540 s after the last session");
    relay_close();
    node_free(e);
    node_free(f);
}

static void t_hub(void)
{
    enum { NS = 32 };
    uint8_t hpub[32], spub[NS][32];
    node_t *h = node(hpub), *s[NS];
    wg_dpeer_t *ph[NS], *ps[NS];
    struct sockaddr_in to = addr_of(wg_dev_port(h->d));
    int i, k;
    uint32_t want = 0;
    for (i = 0; i < NS; i++) {
        uint8_t ip[4] = { 10, 1, 0, (uint8_t)(i + 1) };
        s[i] = node(spub[i]);
        ps[i] = wg_dev_add_peer(s[i]->d, hpub, NULL, 0, NULL, NULL);
        ph[i] = wg_dev_add_peer(h->d, spub[i], NULL, 0, NULL, NULL);
        wg_dev_set_endpoint(ps[i], (struct sockaddr*)&to, sizeof to);
        wg_dev_allow(s[i]->d, ps[i], 0, IP_B, 32);
        wg_dev_allow(h->d, ph[i], 0, ip, 32);
    }
    wait_ready(ps, NS);
    wait_ready(ph, NS);
    sent_seq = 0;
    for (k = 0; k < 5; k++)
        for (i = 0; i < NS; i++) {
            uint8_t ip[4] = { 10, 1, 0, (uint8_t)(i + 1) };
            burst(s[i], ip, IP_B, 10, 1);
            want += 10;
            pump(1);
        }
    pump_until(&h->got, want, 5000);
    CHECK(h->got == want, "32 spokes: every packet reaches the hub");
    for (i = 0; i < NS; i++)
        burst(s[i], (uint8_t[4]) { 10, 1, 0, (uint8_t)(i + 1) }, IP_B, 64, 1);
    wg_dev_remove_peer(h->d, ph[3]);
    pump(50);
    for (i = 0; i < NS; i++)
        node_free(s[i]);
    node_free(h);
    CHECK(1, "remove a peer and free devices with work in flight");
}

static int fail_offload, hold_offload, nheld;
static struct {
    void (*work)(void*);
    void (*done)(void*);
    void* arg;
} heldj[64];
int wg_test_offload_cpu(dyn_aio_t* a, void (*work)(void*), void (*done)(void*), void* arg);
int wg_test_offload_cpu(dyn_aio_t* a, void (*work)(void*), void (*done)(void*), void* arg)
{
    if (fail_offload)
        return -1;
    if (hold_offload && nheld < 64) {
        heldj[nheld].work = work;
        heldj[nheld].done = done;
        heldj[nheld++].arg = arg;
        return 0;
    }
    return dyn_aio_offload_cpu(a, work, done, arg);
}

static void run_held(int i)
{
    if (i < nheld && heldj[i].work) {
        heldj[i].work(heldj[i].arg);
        heldj[i].done(heldj[i].arg);
        heldj[i].work = NULL;
    }
}

static void release_held(void)
{
    int i, n = nheld;
    hold_offload = 0;
    for (i = 0; i < n; i++)
        run_held(i);
    nheld = 0;
}

static void pump_held(int want)
{
    int k;
    for (k = 0; k < 2000 && nheld < want; k++)
        pump(1);
}

static int ready_calls, ready_rc;
static wg_dpeer_t* ready_p;
static void on_ready(void* ud, wg_dpeer_t* p, int rc)
{
    (void)ud;
    ready_calls++;
    ready_rc = rc;
    ready_p = p;
}

static void t_async_peer(void)
{
    uint8_t apub[32], bpub[32], b[1500], zero[32], other[32];
    node_t *a = node(apub), *bn = node(bpub);
    wg_dpeer_t *pa, *pb, *pz;
    wg_dev_stats_t st;
    struct sockaddr_in to = addr_of(wg_dev_port(bn->d));
    int k;
    memset(zero, 0, sizeof zero);
    ready_calls = 0;
    hold_offload = 1;
    pa = wg_dev_add_peer(a->d, bpub, NULL, 0, on_ready, NULL);
    hold_offload = 0;
    wg_dev_stats(a->d, &st);
    CHECK(pa && !wg_dev_peer_ready(pa) && st.peers_pending == 1 && ready_calls == 0, "add_peer returns at once; X25519 runs on the pool (peer pending)");
    CHECK(!wg_dev_add_peer(a->d, bpub, NULL, 0, NULL, NULL), "same public key twice: NULL");
    wg_dev_set_endpoint(pa, (const struct sockaddr*)&to, sizeof to);
    wg_dev_allow(a->d, pa, 0, IP_B, 32);
    pb = wg_dev_add_peer(bn->d, apub, NULL, 0, NULL, NULL);
    wg_dev_allow(bn->d, pb, 0, IP_A, 32);
    wait_ready(&pb, 1);
    ip4(b, IP_A, IP_B, 1, 100);
    wg_dev_send(a->d, b, 100);
    wg_dev_flush(a->d);
    wg_dev_stats(a->d, &st);
    CHECK(!wg_dev_peer_ready(pa) && st.hs_init_sent == 0 && wg_dev_pending(a->d) == 1, "no initiation job while the peer is pending (only its X25519 job)");
    release_held();
    pump_until(&bn->got, 1, 2000);
    wg_dev_stats(a->d, &st);
    CHECK(ready_calls == 1 && ready_rc == 0 && ready_p == pa && wg_dev_peer_ready(pa), "ready callback fires once with rc 0");
    CHECK(bn->got == 1 && st.hs_init_sent == 1 && st.hs_done == 1 && st.peers_pending == 0, "the deferred handshake runs once with the derived keys; the staged packet arrives");
    pz = wg_dev_add_peer(a->d, zero, NULL, 0, on_ready, NULL);
    for (k = 0; k < 2000 && ready_calls < 2; k++)
        pump(1);
    CHECK(pz && ready_calls == 2 && ready_rc == -1 && ready_p == pz, "low-order public key: X25519 fails on the pool, callback rc -1");
    pz = wg_dev_add_peer(a->d, zero, NULL, 0, NULL, NULL);
    CHECK(pz != NULL, "the failed peer was removed (same key can be added again)");
    wg_dev_remove_peer(a->d, pz);
    wg_random(other, 32);
    pz = wg_dev_add_peer(a->d, other, NULL, 0, on_ready, NULL);
    wg_dev_remove_peer(a->d, pz);
    pump(30);
    wg_dev_stats(a->d, &st);
    CHECK(ready_calls == 2 && st.peers_pending == 0, "removed while pending: no callback, job retired");
    wg_random(other, 32);
    wg_dev_add_peer(a->d, other, NULL, 0, on_ready, NULL);
    node_free(a);
    node_free(bn);
    CHECK(ready_calls == 2, "device freed with an add_peer job in flight: no callback (ASan: no leak or UAF)");
}

static size_t udp4(uint8_t* b, const uint8_t src[4], const uint8_t dst[4], size_t n, int bad)
{
    wg_ipaddr_t S, D;
    uint16_t c;
    size_t i;
    memset(&S, 0, sizeof S);
    memset(&D, 0, sizeof D);
    memcpy(S.a, src, 4);
    memcpy(D.a, dst, 4);
    memset(b, 0, 28);
    b[0] = 0x45;
    b[2] = (uint8_t)(n >> 8);
    b[3] = (uint8_t)n;
    b[8] = 64;
    b[9] = 17;
    memcpy(b + 12, src, 4);
    memcpy(b + 16, dst, 4);
    c = wg_csum_fin(wg_csum_acc(0, b, 20));
    memcpy(b + 10, &c, 2);
    b[20] = 0x1f;
    b[22] = 0x00;
    b[23] = 53;
    b[24] = (uint8_t)((n - 20) >> 8);
    b[25] = (uint8_t)(n - 20);
    for (i = 28; i < n; i++)
        b[i] = (uint8_t)(i * 5);
    c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&S, &D, 17, n - 20), b + 20, n - 20));
    if (!c)
        c = 0xffff;
    if (bad)
        c ^= 0x0100;
    memcpy(b + 26, &c, 2);
    return n;
}

static void t_held_jobs(void)
{
    uint8_t apub[32], bpub[32], cpub[32], epub[32], b[200];
    node_t *a = node(apub), *bn = node(bpub), *c = node(cpub), *e = node(epub);
    wg_dpeer_t *pa, *pb, *pc, *pe;
    wg_dev_stats_t sa, sb, sc;
    struct sockaddr_in to = addr_of(wg_dev_port(bn->d)), toe = addr_of(wg_dev_port(e->d));
    pa = wg_dev_add_peer(a->d, bpub, NULL, 0, NULL, NULL);
    wait_ready(&pa, 1);
    wg_dev_set_endpoint(pa, (const struct sockaddr*)&to, sizeof to);
    wg_dev_allow(a->d, pa, 0, IP_B, 32);
    nheld = 0;
    hold_offload = 1;
    pb = wg_dev_add_peer(bn->d, apub, NULL, 0, NULL, NULL);
    hold_offload = 0;
    wg_dev_allow(bn->d, pb, 0, IP_A, 32);
    ip4(b, IP_A, IP_B, 1, 100);
    wg_dev_send(a->d, b, 100);
    wg_dev_flush(a->d);
    for (int k = 0; k < 2000; k++) {
        wg_dev_stats(bn->d, &sb);
        if (sb.hs_drop_pending)
            break;
        pump(1);
    }
    pump(20);
    wg_dev_stats(a->d, &sa);
    wg_dev_stats(bn->d, &sb);
    CHECK(sa.hs_init_sent == 1 && sb.hs_drop_pending == 1 && sb.hs_resp_sent == 0 && sa.hs_done == 0, "initiation for a peer still pending: dropped, no response from zero keys");
    release_held();
    T_ms += 6000;
    pump_until(&bn->got, 1, 3000);
    wg_dev_stats(a->d, &sa);
    CHECK(wg_dev_peer_ready(pb) && sa.hs_done == 1 && bn->got == 1, "once ready, the retry completes and the staged packet arrives");
    link_pair(c, cpub, e, epub, &pc, &pe, &toe);
    nheld = 0;
    hold_offload = 1;
    ip4(b, IP_A, IP_B, 2, 100);
    wg_dev_send(c->d, b, 100);
    wg_dev_flush(c->d);
    CHECK(nheld == 1, "initiation job held");
    run_held(0);
    pump_held(2);
    run_held(1);
    CHECK(nheld == 3, "responder's consume and response jobs held in turn");
    run_held(2);
    pump_held(4);
    CHECK(nheld == 4, "initiator's consume-response job held");
    T_ms += 6000;
    pump_held(5);
    CHECK(nheld == 5, "initiator re-initiated (retry) while the response was being consumed");
    run_held(3);
    wg_dev_stats(c->d, &sc);
    CHECK(sc.hs_done == 0 && !wg_dev_current(pc), "stale consumed response (old generation and index) installs no keypair");
    release_held();
    pump_until(&e->got, 1, 3000);
    wg_dev_stats(c->d, &sc);
    CHECK(sc.hs_done == 1 && e->got == 1, "the fresh handshake completes");
    node_free(a);
    node_free(bn);
    node_free(c);
    node_free(e);
}

static void t_inline_peer(void)
{
    uint8_t apub[32], bpriv[32], bpub[32], zero[32];
    node_t* a = node(apub);
    wg_dpeer_t *p, *pz;
    wg_dev_stats_t st;
    wg_keygen(bpriv, bpub);
    memset(zero, 0, sizeof zero);
    fail_offload = 1;
    ready_calls = 0;
    p = wg_dev_add_peer(a->d, bpub, NULL, 0, on_ready, NULL);
    CHECK(p && ready_calls == 1 && ready_rc == 0 && ready_p == p && wg_dev_peer_ready(p), "pool refused: X25519 inline, ready before add_peer returns");
    pz = wg_dev_add_peer(a->d, zero, NULL, 0, on_ready, NULL);
    CHECK(!pz && ready_calls == 2 && ready_rc == -1, "pool refused, low-order key: callback rc -1 and add_peer returns NULL (not a freed peer)");
    pz = wg_dev_add_peer(a->d, zero, NULL, 0, NULL, NULL);
    CHECK(!pz, "and the key is not left in the table half-added");
    wg_dev_stats(a->d, &st);
    CHECK(st.peers_pending == 0 && st.inline_ops >= 3, "inline jobs counted, nothing pending");
    fail_offload = 0;
    node_free(a);
}

static void t_verify_dev(void)
{
    uint8_t apub[32], bpub[32], b[1500];
    node_t *a = node(apub), *bn = node(bpub);
    wg_dpeer_t *pa, *pb;
    struct sockaddr_in to = addr_of(wg_dev_port(bn->d)), ta;
    wg_dev_stats_t sb, sa;
    unsigned v0;
    int i, k;
    link_pair(a, apub, bn, bpub, &pa, &pb, &to);
    ta = addr_of(wg_dev_port(a->d));
    wg_dev_set_endpoint(pb, (const struct sockaddr*)&ta, sizeof ta);
    wg_dev_set_verify(bn->d, 1);
    ip4(b, IP_A, IP_B, 1, 100);
    wg_dev_send(a->d, b, 100);
    pump_until(&bn->got, 1, 2000);
    rx_verified = 0;
    for (k = 0; k < 20; k++) {
        for (i = 0; i < 64; i++)
            wg_dev_send(a->d, b, udp4(b, IP_A, IP_B, 300 + (size_t)i * 16, 0));
        wg_dev_flush(a->d);
        pump(2);
    }
    pump_until(&bn->got, 1 + 1280, 3000);
    wg_dev_stats(bn->d, &sb);
    CHECK(bn->got == 1281 && rx_verified > 1000 && rx_verified == sb.csum_verified, "verify on: batched packets with good checksums arrive flagged verified (checked on the pool)");
    v0 = rx_verified;
    for (k = 0; k < 5; k++) {
        for (i = 0; i < 64; i++)
            wg_dev_send(a->d, b, udp4(b, IP_A, IP_B, 300, 1));
        wg_dev_flush(a->d);
        pump(2);
    }
    pump_until(&bn->got, 1281 + 320, 3000);
    CHECK(bn->got == 1601 && rx_verified == v0, "bad UDP checksum: delivered unverified (the IP layer drops it on the loop thread)");
    wg_dev_send(bn->d, b, udp4(b, IP_B, IP_A, 300, 0));
    wg_dev_flush(bn->d);
    pump_until(&a->got, 1, 2000);
    wg_dev_stats(a->d, &sa);
    CHECK(a->got == 1 && rx_verified == v0 && sa.csum_verified == 0, "verify off (default, tun mode): nothing is verified");
    node_free(a);
    node_free(bn);
}

typedef struct {
    wg_dev_t* d;
    wg_dpeer_t* live;
    const uint8_t* want_src;
    uint8_t seen[1 << 15];
    unsigned got, bad_content, bad_src, dup, dead_peer;
} dvn_t;

static void dv_rx(void* ud, wg_dpeer_t* p, const uint8_t* ip, size_t n, int verified)
{
    dvn_t* x = (dvn_t*)ud;
    uint32_t seq;
    size_t i;
    (void)verified;
    if (p != x->live)
        x->dead_peer++;
    if (n < 24 || memcmp(ip + 12, x->want_src, 4)) {
        x->bad_src++;
        return;
    }
    seq = (uint32_t)ip[20] << 24 | (uint32_t)ip[21] << 16 | (uint32_t)ip[22] << 8 | ip[23];
    for (i = 24; i < n; i++)
        if (ip[i] != (uint8_t)(i * 7 + seq)) {
            x->bad_content++;
            return;
        }
    if (seq >= (1u << 18) || ((size_t)ip[2] << 8 | ip[3]) != n) {
        x->bad_content++;
        return;
    }
    if (x->seen[seq >> 3] >> (seq & 7) & 1)
        x->dup++;
    x->seen[seq >> 3] |= (uint8_t)(1u << (seq & 7));
    x->got++;
}

static node_t dv_shadow[2];
static dvn_t* dv_new(uint8_t* pub, const uint8_t* want_src, int k)
{
    uint8_t priv[32];
    dvn_t* x = (dvn_t*)calloc(1, sizeof *x);
    wg_keygen(priv, pub);
    x->d = wg_dev_new(aio, priv, "127.0.0.1", 0, dv_rx, x);
    x->want_src = want_src;
    wg_dev_set_clock(x->d, clk, NULL);
    dv_shadow[k].d = x->d;
    all_nodes[n_nodes++] = &dv_shadow[k];
    return x;
}

static void dv_free(dvn_t* x, int k)
{
    int i;
    wg_dev_free(x->d);
    for (i = 0; i < n_nodes; i++)
        if (all_nodes[i] == &dv_shadow[k])
            all_nodes[i] = all_nodes[--n_nodes];
    free(x);
}

static void dv_release(fk_rng_t* r)
{
    int n = nheld, i, order[64];
    hold_offload = 0;
    for (i = 0; i < n; i++)
        order[i] = i;
    for (i = n - 1; i > 0; i--) {
        int j = (int)fk_below(r, (uint64_t)i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (i = 0; i < n; i++)
        run_held(order[i]);
    nheld = 0;
}

static uint32_t dv_seq;
static void dv_burst(fk_rng_t* r, dvn_t* from, const uint8_t* src, const uint8_t* dst, unsigned n)
{
    static uint8_t b[1500];
    unsigned i;
    for (i = 0; i < n && dv_seq + 1 < (1u << 18); i++) {
        size_t len = 24 + (size_t)fk_below(r, fk_chance(r, 70) ? 200 : 1397);
        ip4(b, fk_chance(r, 10) ? IP_X : src, dst, ++dv_seq, len);
        wg_dev_send(from->d, b, len);
    }
    wg_dev_flush(from->d);
}

static void dv_link(dvn_t* x, const uint8_t* peer_pub, const uint8_t* allow, const struct sockaddr_in* ep, int wait)
{
    x->live = wg_dev_add_peer(x->d, peer_pub, NULL, 0, NULL, NULL);
    if (!x->live)
        return;
    if (wait)
        wait_ready(&x->live, 1);
    if (ep)
        wg_dev_set_endpoint(x->live, (const struct sockaddr*)ep, sizeof *ep);
    wg_dev_allow(x->d, x->live, 0, allow, 32);
}

static void p_dev_ops(void)
{
    fk_rng_t r;
    unsigned it, n, dead = 0, content = 0, src = 0, dup = 0, stuck = 0, removes = 0, held_removes = 0, releases = 0, dups_on = 0, rekeys = 0, freed_busy = 0;
    if (!fk_only("dev-ops"))
        return;
    r = fk_stream("dev-ops");
    n = (unsigned)fk_iters(14);
    for (it = 0; it < n; it++) {
        uint8_t apub[32], bpub[32];
        dvn_t *A = dv_new(apub, IP_B, 0), *B = dv_new(bpub, IP_A, 1);
        int use_relay = fk_chance(&r, 80), ops = 40 + (int)fk_below(&r, 80), k;
        struct sockaddr_in to = addr_of(wg_dev_port(B->d));
        unsigned before;
        wg_dev_stats_t st;
        dv_seq = 0;
        if (use_relay) {
            relay_setup((node_t*)&dv_shadow[0], (node_t*)&dv_shadow[1]);
            to = sock_addr(relay);
        }
        dv_link(A, bpub, IP_B, &to, 1);
        dv_link(B, apub, IP_A, NULL, 1);
        for (k = 0; k < ops; k++) {
            unsigned op = (unsigned)fk_below(&r, 100);
            if (op < 28)
                dv_burst(&r, A, IP_A, IP_B, 1 + (unsigned)fk_below(&r, 16));
            else if (op < 38)
                dv_burst(&r, B, IP_B, IP_A, 1 + (unsigned)fk_below(&r, 16));
            else if (op < 55)
                pump(1 + (int)fk_below(&r, 8));
            else if (op < 61)
                T_ms += fk_chance(&r, 70) ? fk_below(&r, 6000) : fk_below(&r, 200000);
            else if (op < 66) {
                int side = (int)fk_below(&r, 2);
                dvn_t* x = side ? B : A;
                removes++;
                if (fk_chance(&r, 50))
                    pump(1);
                wg_dev_remove_peer(x->d, x->live);
                x->live = NULL;
                if (fk_chance(&r, 50))
                    pump(1 + (int)fk_below(&r, 3));
                dv_link(x, side ? apub : bpub, side ? IP_A : IP_B, side ? NULL : &to, 0);
            } else if (op < 73) {
                if (hold_offload) {
                    releases++;
                    dv_release(&r);
                } else
                    hold_offload = 1;
            } else if (op < 78 && use_relay) {
                relay_mode = fk_chance(&r, 35) ? R_FWD : fk_chance(&r, 80) ? R_DUP : R_DROP;
                dups_on += relay_mode == R_DUP;
            } else if (op < 81)
                wg_dev_set_load_threshold((fk_chance(&r, 50) ? A : B)->d, fk_chance(&r, 50) ? 0 : WG_UNDER_LOAD_QUEUE);
            else if (op < 84)
                wg_dev_set_verify((fk_chance(&r, 50) ? A : B)->d, (int)fk_below(&r, 2));
            else if (op < 92) {
                int side = (int)fk_below(&r, 2);
                dvn_t* x = side ? B : A;
                held_removes++;
                hold_offload = 1;
                if (fk_chance(&r, 65))
                    T_ms += 130000;
                dv_burst(&r, side ? A : B, side ? IP_A : IP_B, side ? IP_B : IP_A, 1 + (unsigned)fk_below(&r, 8));
                pump(1 + (int)fk_below(&r, 4));
                wg_dev_remove_peer(x->d, x->live);
                x->live = NULL;
                if (fk_chance(&r, 50))
                    dv_link(x, side ? apub : bpub, side ? IP_A : IP_B, side ? NULL : &to, 0);
                dv_release(&r);
                pump(1 + (int)fk_below(&r, 4));
                if (!x->live)
                    dv_link(x, side ? apub : bpub, side ? IP_A : IP_B, side ? NULL : &to, 0);
            }
            else
                pump(1);
        }
        dv_release(&r);
        relay_mode = R_FWD;
        wg_dev_set_load_threshold(A->d, WG_UNDER_LOAD_QUEUE);
        wg_dev_set_load_threshold(B->d, WG_UNDER_LOAD_QUEUE);
        pump(20);
        before = B->got;
        for (k = 0; k < 40 && B->got == before; k++) {
            T_ms += 6000;
            dv_burst(&r, A, IP_A, IP_B, 4);
            pump(40);
        }
        stuck += B->got == before;
        wg_dev_stats(A->d, &st);
        rekeys += st.hs_done > 1;
        if (fk_chance(&r, 60)) {
            dv_burst(&r, A, IP_A, IP_B, 16);
            dv_burst(&r, B, IP_B, IP_A, 16);
            freed_busy++;
        }
        content += A->bad_content + B->bad_content;
        src += A->bad_src + B->bad_src;
        dup += A->dup + B->dup;
        dead += A->dead_peer + B->dead_peer;
        if (fk_chance(&r, 50)) {
            dv_free(A, 0);
            dv_free(B, 1);
        } else {
            dv_free(B, 1);
            dv_free(A, 0);
        }
        if (use_relay)
            relay_close();
    }
    printf("  dev-ops: %u runs, %u removes (%u with held jobs), %u held releases, %u dup periods, %u runs with a rekey, %u freed with traffic in flight\n", n, removes, held_removes, releases, dups_on, rekeys, freed_busy);
    CHECK(content == 0 && src == 0, "dev-ops: under random send/pump/clock/remove-readd/held-job-order/relay dup-drop/load/verify sequences every delivered packet is intact and from the peer's allowed source");
    CHECK(dup == 0, "dev-ops: no packet is delivered twice (relay duplication, held batches released in random order)");
    CHECK(dead == 0, "dev-ops: nothing is delivered for a peer after it was removed");
    CHECK(stuck == 0, "dev-ops: after the sequence, held jobs released and the link healed, traffic flows again");
    if (n >= 10)
        CHECK(removes && releases && dups_on && rekeys, "dev-ops: removes, held releases, duplication and rekeys all exercised");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    aio = dyn_aio_new(256, 0);
    if (!aio) {
        printf("FAIL aio\n");
        return 1;
    }
    fk_init("test_wg_device");
    if (fk_legacy()) {
        t_basic();
        t_relay();
        t_cookie();
        t_timers();
        t_hub();
        t_async_peer();
        t_held_jobs();
        t_verify_dev();
        t_inline_peer();
    }
    p_dev_ops();
    dyn_aio_free(aio);
    printf("test_wg_device: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
