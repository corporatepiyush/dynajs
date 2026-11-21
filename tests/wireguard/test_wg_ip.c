/*
 * test_wg_ip.c -- dyna:wireguard S13c1 IP layer (src/modules/wireguard/wg-ip.c), pure: packets in as bytes, out
 * through a callback, injected clock. Every expected checksum comes from a bytewise RFC 1071 reference written here.
 *  1. Checksum: RFC 1071 section 3 worked example (sum ddf2), a classic IPv4 header (checksum b861), differential
 *     against the reference over random lengths, alignments and even split points, 64-bit carry folding.
 *  2. ICMP echo (RFC 792 / RFC 4443 4.2): v4 type 8 -> 0 without pseudo-header, v6 128 -> 129 with it; addresses
 *     swapped, identifier/sequence/payload kept; bad ICMP checksum dropped.
 *  3. UDP (RFC 768 / RFC 8200 8.1): IP padding beyond the UDP length is not delivered; v4 zero checksum means none;
 *     v6 zero checksum dropped; bad checksum, length < 8 and length > payload dropped; sent zero sum goes out as ffff.
 *  4. IPv4 (RFC 791 / RFC 1122 3.2.1): version, IHL, total length, header checksum, MF or offset (fragments dropped),
 *     non-local destination, martian sources; options skipped. IPv6 (RFC 8200 4): payload length, hop-by-hop only
 *     first, option action bits (4.2), routing with segments left, fragment header dropped, chain cap 8, truncation.
 *  5. Port unreachable (RFC 1122 4.1.3.1, RFC 4443 2.4): v4 3/3 with the original header, v6 1/4 within 1280 bytes;
 *     token bucket of 10, one more per 100 ms.
 *  6. Ports: fixed binds, per-family tables, ephemeral 49152..65535 distinct and exhaustible, reuse after close,
 *     free with sockets open (ASan leak check). Protocol hook (TCP, ICMP non-echo).
 *  7. Two stacks wired back to back: UDP echo and ping both ways, v4 and v6.
 *  8. The fuzz entry (src/fuzz/fuzz_wg_ip.c) on seeds plus 40000 mutated inputs; its output check aborts on a bad
 *     IPv4 header checksum or length.
 *  9. Pool-side verify (wg_ip_verify) on 20000 random UDP/ICMP/TCP packets, clean and bit-flipped, v4 and v6: no false
 *     positive; trusted input skips exactly one L4 checksum, deliveries match an untrusted twin stack; trust is scoped to
 *     the call; a v4 UDP zero checksum is never "verified" even when the data sums to ffff (RFC 768: none was sent).
 * 10. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks):
 *     ip-classify: 60000 random v4/v6 packets (IHL, total/payload length, padding, truncation, header checksum, MF/DF/
 *     offset, martian, near-local and cross-family addresses, v6 chains up to 10 with option action bits, routing, fragment,
 *     UDP length/zero/bad sums, echo at the MTU edge) each move exactly the counter a naive RFC validator predicts; UDP
 *     payloads and echo replies are checked byte for byte. csum-naive: 1..5 even chunks against the bytewise reference.
 *     ip-reseeded: sections 8 and 9 under a fresh seed, verify vs fragments. 25 mutants killed on 3 seeds,
 *     none equivalent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/modules/wireguard/wg-ip.h"
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

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static uint32_t ref_sum(const uint8_t* p, size_t n, uint32_t s)
{
    size_t i;
    for (i = 0; i < n; i += 2)
        s += (uint32_t)p[i] << 8 | (i + 1 < n ? p[i + 1] : 0);
    return s;
}

static uint16_t ref_fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)s;
}

static uint16_t ref_csum(const uint8_t* p, size_t n, uint32_t init)
{
    return (uint16_t)~ref_fold(ref_sum(p, n, init));
}

static uint32_t ref_pseudo(int v6, const uint8_t* s, const uint8_t* d, uint8_t proto, size_t len)
{
    uint32_t r = ref_sum(s, v6 ? 16 : 4, 0);
    r = ref_sum(d, v6 ? 16 : 4, r);
    return r + proto + (uint32_t)(len >> 16) + (uint32_t)(len & 0xffff);
}

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static const uint8_t A4[4] = { 10, 0, 0, 1 }, B4[4] = { 10, 0, 0, 2 }, C4[4] = { 10, 0, 0, 9 };
static const uint8_t A6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const uint8_t B6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

static wg_ipaddr_t ad(int v6, const uint8_t* a)
{
    wg_ipaddr_t r;
    memset(&r, 0, sizeof r);
    r.v6 = v6;
    memcpy(r.a, a, v6 ? 16 : 4);
    return r;
}

static size_t mk(uint8_t* b, int v6, const uint8_t* s, const uint8_t* d, uint8_t proto, const uint8_t* l4, size_t n)
{
    if (v6) {
        memset(b, 0, 40);
        b[0] = 0x60;
        put16(b + 4, (unsigned)n);
        b[6] = proto;
        b[7] = 64;
        memcpy(b + 8, s, 16);
        memcpy(b + 24, d, 16);
        memmove(b + 40, l4, n);
        return n + 40;
    }
    memset(b, 0, 20);
    b[0] = 0x45;
    put16(b + 2, (unsigned)n + 20);
    b[8] = 64;
    b[9] = proto;
    memcpy(b + 12, s, 4);
    memcpy(b + 16, d, 4);
    put16(b + 10, ref_csum(b, 20, 0));
    memmove(b + 20, l4, n);
    return n + 20;
}

static size_t mk_udp(uint8_t* b, int v6, const uint8_t* s, const uint8_t* d, unsigned sp, unsigned dp, const uint8_t* data, size_t n)
{
    uint8_t u[1500];
    put16(u, sp);
    put16(u + 2, dp);
    put16(u + 4, (unsigned)n + 8);
    put16(u + 6, 0);
    memcpy(u + 8, data, n);
    put16(u + 6, ref_csum(u, n + 8, ref_pseudo(v6, s, d, 17, n + 8)));
    if (!be16(u + 6))
        put16(u + 6, 0xffff);
    return mk(b, v6, s, d, 17, u, n + 8);
}

static size_t mk_echo(uint8_t* b, int v6, const uint8_t* s, const uint8_t* d, unsigned id, unsigned seq, size_t n)
{
    uint8_t m[1500];
    size_t i;
    m[0] = v6 ? 128 : 8;
    m[1] = 0;
    put16(m + 2, 0);
    put16(m + 4, id);
    put16(m + 6, seq);
    for (i = 0; i < n; i++)
        m[8 + i] = (uint8_t)(i * 13 + seq);
    put16(m + 2, ref_csum(m, n + 8, v6 ? ref_pseudo(1, s, d, 58, n + 8) : 0));
    return mk(b, v6, s, d, v6 ? 58 : 1, m, n + 8);
}

typedef struct {
    uint8_t q[64][1500];
    size_t len[64];
    unsigned head, n, total;
    int refuse;
} cap_t;

static int out_cb(void* ud, const uint8_t* p, size_t n)
{
    cap_t* c = (cap_t*)ud;
    unsigned at;
    if (c->refuse)
        return -1;
    at = (c->head + c->n) % 64;
    memcpy(c->q[at], p, n);
    c->len[at] = n;
    if (c->n < 64)
        c->n++;
    else
        c->head = (c->head + 1) % 64;
    c->total++;
    return 0;
}

static const uint8_t* last(cap_t* c, size_t* n)
{
    unsigned at = (c->head + c->n + 63) % 64;
    *n = c->n ? c->len[at] : 0;
    return c->q[at];
}

static void clear(cap_t* c)
{
    c->head = c->n = 0;
}

static uint64_t T;
static uint64_t clk(void* ud)
{
    (void)ud;
    return T;
}

typedef struct {
    unsigned got;
    wg_ipaddr_t src;
    uint16_t sport;
    uint8_t data[1500];
    size_t n;
    int echo;
} sink_t;

static void on_udp(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* d, size_t n)
{
    sink_t* k = (sink_t*)ud;
    if (!k)
        return;
    k->got++;
    k->src = *src;
    k->sport = sport;
    memcpy(k->data, d, n);
    k->n = n;
    if (k->echo)
        wg_udp_sendto(s, src, sport, d, n);
}

static unsigned hook_n;
static wg_ipaddr_t hook_src, hook_dst;
static size_t hook_len;
static void on_hook(void* ud, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n)
{
    (void)ud;
    (void)l4;
    hook_n++;
    hook_src = *src;
    hook_dst = *dst;
    hook_len = n;
}

static wg_ip_t* stack(cap_t* c, const uint8_t* a4, const uint8_t* a6)
{
    wg_ip_t* ip = wg_ip_new(out_cb, c);
    wg_ipaddr_t x = ad(0, a4), y = ad(1, a6);
    wg_ip_set_clock(ip, clk, NULL);
    wg_ip_add_addr(ip, &x);
    wg_ip_add_addr(ip, &y);
    return ip;
}

static int v4_ok(const uint8_t* p, size_t n)
{
    return n >= 20 && p[0] == 0x45 && be16(p + 2) == n && ref_csum(p, 20, 0) == 0 && p[8] == 64 && (be16(p + 6) & 0xbfff) == 0;
}

static int l4_ok(const uint8_t* p, size_t n)
{
    int v6 = p[0] >> 4 == 6;
    size_t off = v6 ? 40 : 20;
    uint8_t proto = v6 ? p[6] : p[9];
    uint32_t init = proto == 1 ? 0 : ref_pseudo(v6, p + (v6 ? 8 : 12), p + (v6 ? 24 : 16), proto, n - off);
    if (v6 && (be16(p + 4) + 40u != n || p[7] != 64))
        return 0;
    if (!v6 && !v4_ok(p, n))
        return 0;
    return ref_csum(p + off, n - off, init) == 0;
}

static void t_csum(void)
{
    static const uint8_t rfc[8] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    static const uint8_t hdr[20] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0, 0, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
    static uint8_t big[70000 + 8];
    uint8_t h[20];
    uint16_t c;
    int i, bad = 0;
    CHECK(ref_fold(ref_sum(rfc, 8, 0)) == 0xddf2, "reference: RFC 1071 example sums to ddf2");
    c = wg_csum_fin(wg_csum_acc(0, rfc, 8));
    CHECK(((const uint8_t*)&c)[0] == 0x22 && ((const uint8_t*)&c)[1] == 0x0d, "RFC 1071 example: checksum 220d in wire order");
    memcpy(h, hdr, 20);
    CHECK(ref_csum(h, 20, 0) == 0xb861, "reference: IPv4 header checksum b861");
    c = wg_csum_fin(wg_csum_acc(0, h, 20));
    memcpy(h + 10, &c, 2);
    CHECK(h[10] == 0xb8 && h[11] == 0x61 && wg_csum_fin(wg_csum_acc(0, h, 20)) == 0, "IPv4 header: b861 and verifies to zero");
    for (i = 0; i < 20000; i++) {
        size_t n = (size_t)(rnd() % 2049), off = (size_t)(rnd() % 8), cut = n ? (size_t)(rnd() % (n + 1)) & ~(size_t)1 : 0, k;
        uint8_t* p = big + off;
        uint64_t acc;
        uint16_t want, got;
        for (k = 0; k < n; k++)
            p[k] = (uint8_t)rnd();
        if (i % 7 == 0)
            memset(p, 0xff, n);
        want = ref_csum(p, n, 0);
        acc = wg_csum_acc(wg_csum_acc(0, p, cut), p + cut, n - cut);
        got = wg_csum_fin(acc);
        if (((const uint8_t*)&got)[0] != (uint8_t)(want >> 8) || ((const uint8_t*)&got)[1] != (uint8_t)want)
            bad++;
    }
    CHECK(bad == 0, "differential: 20000 random buffers, alignments 0..7, even split points");
    memset(big, 0xff, sizeof big);
    c = wg_csum_fin(wg_csum_acc(0, big, 70000));
    CHECK(c == 0 && ref_csum(big, 70000, 0) == 0, "64-bit carry fold on 70000 bytes of ff");
    big[0] = 0x01;
    c = wg_csum_fin(wg_csum_acc(0, big, 70001));
    CHECK(((const uint8_t*)&c)[0] == (uint8_t)(ref_csum(big, 70001, 0) >> 8) && ((const uint8_t*)&c)[1] == (uint8_t)ref_csum(big, 70001, 0), "odd length tail byte");
}

static void t_echo(void)
{
    cap_t c;
    wg_ip_t* ip;
    uint8_t b[1500];
    const uint8_t* r;
    size_t n, rn;
    wg_ip_stats_t st;
    int v6;
    memset(&c, 0, sizeof c);
    ip = stack(&c, A4, A6);
    for (v6 = 0; v6 < 2; v6++) {
        clear(&c);
        n = mk_echo(b, v6, v6 ? B6 : B4, v6 ? A6 : A4, 0x1234, 7, 100);
        wg_ip_input(ip, b, n);
        r = last(&c, &rn);
        CHECK(c.n == 1 && rn == n && l4_ok(r, rn), v6 ? "v6 echo reply valid (pseudo-header checksum)" : "v4 echo reply valid");
        CHECK(r[v6 ? 40 : 20] == (v6 ? 129 : 0) && memcmp(r + (v6 ? 8 : 12), v6 ? A6 : A4, v6 ? 16 : 4) == 0 && memcmp(r + (v6 ? 24 : 16), v6 ? B6 : B4, v6 ? 16 : 4) == 0,
              "echo reply type and swapped addresses");
        CHECK(memcmp(r + (v6 ? 44 : 24), b + (v6 ? 44 : 24), n - (v6 ? 44 : 24)) == 0, "identifier, sequence and payload kept");
        b[n - 1] ^= 1;
        clear(&c);
        wg_ip_input(ip, b, n);
        CHECK(c.n == 0, "echo with a bad ICMP checksum dropped");
        b[n - 1] ^= 1;
        memset(b + n, 0xa5, 4);
        clear(&c);
        wg_ip_input(ip, b, n + 4);
        r = last(&c, &rn);
        CHECK(c.n == 1 && rn == n && l4_ok(r, rn), "link padding beyond the IP length is not part of the datagram");
    }
    n = mk_echo(b, 0, B4, A4, 1, 1, 8);
    b[21] = 1;
    put16(b + 22, 0);
    put16(b + 22, ref_csum(b + 20, n - 20, 0));
    clear(&c);
    wg_ip_input(ip, b, n);
    CHECK(c.n == 0, "echo with code != 0 not answered");
    c.refuse = 1;
    n = mk_echo(b, 0, B4, A4, 1, 1, 8);
    wg_ip_input(ip, b, n);
    c.refuse = 0;
    wg_ip_stats(ip, &st);
    CHECK(st.echo == 4 && st.rx_csum == 2 && st.tx_fail == 1, "echo counters; refused output counted");
    wg_ip_free(ip);
}

static void t_udp_rx(void)
{
    cap_t c;
    wg_ip_t* ip;
    uint8_t b[1500], d[600];
    size_t n;
    sink_t k4, k6;
    wg_ip_stats_t st;
    unsigned i;
    memset(&c, 0, sizeof c);
    memset(&k4, 0, sizeof k4);
    memset(&k6, 0, sizeof k6);
    ip = stack(&c, A4, A6);
    wg_udp_bind(ip, 0, 5000, on_udp, &k4);
    wg_udp_bind(ip, 1, 5000, on_udp, &k6);
    for (i = 0; i < sizeof d; i++)
        d[i] = (uint8_t)(i ^ 0x5a);
    n = mk_udp(b, 0, B4, A4, 4444, 5000, d, 300);
    memset(b + n, 0xee, 16);
    put16(b + 2, (unsigned)n + 16);
    put16(b + 10, 0);
    put16(b + 10, ref_csum(b, 20, 0));
    wg_ip_input(ip, b, n + 16);
    CHECK(k4.got == 1 && k4.n == 300 && memcmp(k4.data, d, 300) == 0 && k4.sport == 4444 && memcmp(k4.src.a, B4, 4) == 0 && !k4.src.v6,
          "v4 UDP delivered with its own length, not the padded IP length");
    n = mk_udp(b, 1, B6, A6, 4445, 5000, d, 0);
    wg_ip_input(ip, b, n);
    CHECK(k6.got == 1 && k6.n == 0 && k6.sport == 4445 && k6.src.v6, "v6 empty UDP datagram delivered");
    n = mk_udp(b, 0, B4, A4, 1, 5000, d, 40);
    put16(b + 26, 0);
    wg_ip_input(ip, b, n);
    CHECK(k4.got == 2, "v4 zero UDP checksum means none");
    n = mk_udp(b, 1, B6, A6, 1, 5000, d, 40);
    put16(b + 46, 0);
    wg_ip_input(ip, b, n);
    CHECK(k6.got == 1, "v6 zero UDP checksum dropped");
    n = mk_udp(b, 0, B4, A4, 1, 5000, d, 40);
    b[n - 1] ^= 0x80;
    wg_ip_input(ip, b, n);
    n = mk_udp(b, 1, B6, A6, 1, 5000, d, 40);
    b[n - 1] ^= 0x80;
    wg_ip_input(ip, b, n);
    CHECK(k4.got == 2 && k6.got == 1, "bad UDP checksum dropped (v4, v6)");
    n = mk_udp(b, 0, B4, A4, 1, 5000, d, 40);
    put16(b + 24, 7);
    wg_ip_input(ip, b, n);
    n = mk_udp(b, 0, B4, A4, 1, 5000, d, 40);
    put16(b + 24, 49);
    wg_ip_input(ip, b, n);
    n = mk(b, 0, B4, A4, 17, d, 7);
    wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &st);
    CHECK(k4.got == 2 && st.rx_bad == 3, "UDP length < 8, > payload, short header dropped");
    CHECK(st.rx_csum == 3 && st.udp_rx == 3, "UDP counters");
    wg_ip_free(ip);
}

static void t_hdr(void)
{
    cap_t c;
    wg_ip_t* ip;
    uint8_t b[1500], d[64], x[1500];
    size_t n;
    sink_t k;
    wg_ip_stats_t s0, s1;
    memset(&c, 0, sizeof c);
    memset(&k, 0, sizeof k);
    memset(d, 7, sizeof d);
    ip = stack(&c, A4, A6);
    wg_udp_bind(ip, 0, 53, on_udp, &k);
    wg_udp_bind(ip, 1, 53, on_udp, &k);
#define FIX4(p) (put16((p) + 10, 0), put16((p) + 10, ref_csum((p), (size_t)((p)[0] & 15) * 4, 0)))
    n = mk_udp(b, 0, B4, A4, 9, 53, d, 20);
    memcpy(x, b, n);
    wg_ip_stats(ip, &s0);
    x[0] = 0x55;
    FIX4(x);
    wg_ip_input(ip, x, n);
    memcpy(x, b, n);
    x[0] = 0x44;
    wg_ip_input(ip, x, n);
    memcpy(x, b, n);
    x[0] = 0x4f;
    wg_ip_input(ip, x, 40);
    memcpy(x, b, n);
    put16(x + 2, 19);
    FIX4(x);
    wg_ip_input(ip, x, n);
    memcpy(x, b, n);
    put16(x + 2, (unsigned)n + 1);
    FIX4(x);
    wg_ip_input(ip, x, n);
    wg_ip_input(ip, b, 19);
    wg_ip_input(ip, b, 0);
    wg_ip_stats(ip, &s1);
    CHECK(k.got == 0 && s1.rx_bad - s0.rx_bad == 7, "v4 version, IHL < 5, IHL > length, total < IHL, total > length, short, empty");
    memcpy(x, b, n);
    x[8] ^= 1;
    wg_ip_input(ip, x, n);
    memcpy(x, b, n);
    put16(x + 6, 0x2000);
    FIX4(x);
    wg_ip_input(ip, x, n);
    memcpy(x, b, n);
    put16(x + 6, 0x0001);
    FIX4(x);
    wg_ip_input(ip, x, n);
    wg_ip_stats(ip, &s0);
    CHECK(k.got == 0 && s0.rx_csum - s1.rx_csum == 1 && s0.rx_frag - s1.rx_frag == 2, "bad header checksum; MF and fragment offset dropped");
    memcpy(x, b, n);
    put16(x + 6, 0x4000);
    FIX4(x);
    wg_ip_input(ip, x, n);
    CHECK(k.got == 1, "DF alone is not a fragment");
    {
        uint8_t o[1500];
        memcpy(o, b, 20);
        o[0] = 0x47;
        o[20] = 1;
        o[21] = 1;
        o[22] = 1;
        o[23] = 0;
        o[24] = o[25] = o[26] = o[27] = 0;
        memcpy(o + 28, b + 20, n - 20);
        put16(o + 2, (unsigned)n + 8);
        FIX4(o);
        wg_ip_input(ip, o, n + 8);
        CHECK(k.got == 2, "v4 options skipped by IHL");
    }
    n = mk_udp(x, 0, B4, C4, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 0, (const uint8_t[]){ 224, 0, 0, 1 }, A4, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 0, (const uint8_t[]){ 127, 0, 0, 1 }, A4, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 0, (const uint8_t[]){ 0, 0, 0, 0 }, A4, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 1, (const uint8_t[]){ 0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, A6, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 1, (const uint8_t[]){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, A6, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    n = mk_udp(x, 1, B6, B6, 9, 53, d, 20);
    wg_ip_input(ip, x, n);
    wg_ip_stats(ip, &s1);
    CHECK(k.got == 2 && s1.rx_not_local - s0.rx_not_local == 2 && s1.rx_bad - s0.rx_bad == 5 && c.total == 0,
          "non-local destination; multicast, loopback, zero sources dropped without an ICMP error");
    n = mk_udp(x, 1, B6, A6, 9, 53, d, 20);
    wg_ip_input(ip, x, n - 1 + 1);
    put16(x + 4, (unsigned)(n - 40 + 1));
    wg_ip_input(ip, x, n);
    wg_ip_input(ip, x, 39);
    wg_ip_stats(ip, &s0);
    CHECK(k.got == 3 && s0.rx_bad - s1.rx_bad == 2, "v6 payload length beyond the packet and short header dropped");
#undef FIX4
    wg_ip_free(ip);
}

static size_t v6_ext(uint8_t* b, const uint8_t* chain, unsigned nc, const uint8_t* opt, size_t ol)
{
    uint8_t u[64], *p;
    size_t n, i, at = 40;
    uint8_t d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    n = mk_udp(u, 1, B6, A6, 9, 53, d, 8) - 40;
    memset(b, 0, 40);
    b[0] = 0x60;
    b[7] = 64;
    memcpy(b + 8, B6, 16);
    memcpy(b + 24, A6, 16);
    b[6] = nc ? chain[0] : 17;
    for (i = 0; i < nc; i++) {
        p = b + at;
        memset(p, 0, 8);
        p[0] = i + 1 < nc ? chain[i + 1] : 17;
        if (chain[i] == 43) {
            p[2] = 0;
            p[3] = opt ? opt[0] : 0;
        } else if (chain[i] != 44) {
            if (opt && i == 0)
                memcpy(p + 2, opt, ol);
            else {
                p[2] = 1;
                p[3] = 4;
            }
        }
        at += 8;
    }
    memcpy(b + at, u + 40, n);
    put16(b + 4, (unsigned)(at - 40 + n));
    return at + n;
}

static void t_v6ext(void)
{
    cap_t c;
    wg_ip_t* ip;
    uint8_t b[1500];
    size_t n;
    sink_t k;
    wg_ip_stats_t s0, s1;
    static const uint8_t hd[] = { 0, 60 }, hh[] = { 60, 0 }, rt[] = { 43 }, fr[] = { 44 }, many[] = { 60, 60, 60, 60, 60, 60, 60, 60, 60 }, eight[] = { 0, 60, 60, 60, 60, 60, 60, 60 };
    static const uint8_t pad_unknown_skip[6] = { 5, 2, 0, 0, 1, 0 }, unknown_discard[6] = { 0x45, 2, 0, 0, 1, 0 }, unknown_icmp[6] = { 0xc2, 4, 0, 0, 0, 0 }, pad1[6] = { 0, 0, 0, 0, 0, 0 },
                         overrun[6] = { 1, 9, 0, 0, 0, 0 };
    memset(&c, 0, sizeof c);
    memset(&k, 0, sizeof k);
    ip = stack(&c, A4, A6);
    wg_udp_bind(ip, 1, 53, on_udp, &k);
    n = v6_ext(b, hd, 2, NULL, 0);
    wg_ip_input(ip, b, n);
    CHECK(k.got == 1 && k.n == 8, "hop-by-hop + destination options (PadN) walked, UDP delivered");
    n = v6_ext(b, eight, 8, NULL, 0);
    wg_ip_input(ip, b, n);
    CHECK(k.got == 2, "a chain of 8 extension headers is accepted");
    n = v6_ext(b, hd, 1, pad_unknown_skip, 6);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, hd, 1, pad1, 6);
    wg_ip_input(ip, b, n);
    CHECK(k.got == 4, "unknown option with action 00 skipped; Pad1 run");
    n = v6_ext(b, rt, 1, NULL, 0);
    wg_ip_input(ip, b, n);
    CHECK(k.got == 5, "routing header with segments left 0 skipped");
    wg_ip_stats(ip, &s0);
    n = v6_ext(b, hh, 2, NULL, 0);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, hd, 1, unknown_discard, 6);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, hd, 1, unknown_icmp, 6);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, hd, 1, overrun, 6);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, rt, 1, (const uint8_t[]){ 1 }, 1);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, many, 9, NULL, 0);
    wg_ip_input(ip, b, n);
    n = v6_ext(b, hd, 2, NULL, 0);
    put16(b + 4, 12);
    wg_ip_input(ip, b, 52);
    n = v6_ext(b, hd, 2, NULL, 0);
    b[41] = 200;
    wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &s1);
    CHECK(k.got == 5 && s1.rx_bad - s0.rx_bad == 8, "hop-by-hop not first, option action 01/11, option overrun, segments left, 9 headers, truncated, length overrun dropped");
    n = v6_ext(b, fr, 1, NULL, 0);
    wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &s0);
    CHECK(k.got == 5 && s0.rx_frag - s1.rx_frag == 1, "fragment header dropped");
    wg_ip_free(ip);
}

static void t_udp_tx(void)
{
    cap_t c;
    wg_ip_t* ip;
    wg_udp_t *s4, *s6;
    uint8_t d[1500];
    const uint8_t* r;
    size_t rn, i;
    wg_ipaddr_t b4 = ad(0, B4), b6 = ad(1, B6);
    uint16_t id0;
    memset(&c, 0, sizeof c);
    for (i = 0; i < sizeof d; i++)
        d[i] = (uint8_t)(i * 31);
    ip = stack(&c, A4, A6);
    s4 = wg_udp_bind(ip, 0, 0, on_udp, NULL);
    s6 = wg_udp_bind(ip, 1, 0, on_udp, NULL);
    CHECK(wg_udp_sendto(s4, &b4, 53, d, 100) == 0, "v4 send");
    r = last(&c, &rn);
    CHECK(rn == 128 && l4_ok(r, rn) && r[9] == 17 && be16(r + 6) == 0x4000 && be16(r + 20) == wg_udp_port(s4) && be16(r + 22) == 53 && be16(r + 24) == 108 && memcmp(r + 12, A4, 4) == 0 &&
              memcmp(r + 28, d, 100) == 0,
          "v4 datagram: DF, TTL 64, header and UDP checksums valid, ports and length");
    id0 = be16(r + 4);
    wg_udp_sendto(s4, &b4, 53, d, 1);
    r = last(&c, &rn);
    CHECK(be16(r + 4) == (uint16_t)(id0 + 1), "IPv4 identification advances");
    CHECK(wg_udp_sendto(s6, &b6, 53, d, 1000) == 0, "v6 send");
    r = last(&c, &rn);
    CHECK(rn == 1048 && l4_ok(r, rn) && r[6] == 17 && memcmp(r + 8, A6, 16) == 0 && memcmp(r + 48, d, 1000) == 0, "v6 datagram valid");
    {
        uint32_t s;
        d[0] = d[1] = 0;
        s = ref_pseudo(0, A4, B4, 17, 18);
        s = ref_sum((const uint8_t[]){ (uint8_t)(wg_udp_port(s4) >> 8), (uint8_t)wg_udp_port(s4), 0, 53, 0, 18, 0, 0 }, 8, s);
        s = ref_sum(d, 10, s);
        put16(d, (unsigned)(0xffff - ref_fold(s)));
        wg_udp_sendto(s4, &b4, 53, d, 10);
        r = last(&c, &rn);
        CHECK(be16(r + 26) == 0xffff && l4_ok(r, rn), "a computed zero UDP checksum is sent as ffff");
    }
    c.total = 0;
    CHECK(wg_udp_sendto(s4, &b4, 53, d, WG_IP_MTU - 28) == 0 && wg_udp_sendto(s4, &b4, 53, d, WG_IP_MTU - 27) < 0, "v4 MTU limit");
    CHECK(wg_udp_sendto(s6, &b6, 53, d, WG_IP_MTU - 48) == 0 && wg_udp_sendto(s6, &b6, 53, d, WG_IP_MTU - 47) < 0, "v6 MTU limit");
    CHECK(wg_udp_sendto(s4, &b6, 53, d, 4) < 0 && c.total == 2, "family mismatch refused");
    CHECK(wg_ip_output(ip, NULL, &b4, 6, d, 40) == 0 && (r = last(&c, &rn), v4_ok(r, rn) && rn == 60 && r[9] == 6 && memcmp(r + 12, A4, 4) == 0 && memcmp(r + 20, d, 40) == 0), "raw output with the default source");
    CHECK(wg_ip_output(ip, NULL, &b4, 6, d, WG_IP_MTU - 19) < 0, "raw output MTU limit");
    wg_ip_free(ip);
    memset(&c, 0, sizeof c);
    ip = wg_ip_new(out_cb, &c);
    s4 = wg_udp_bind(ip, 0, 0, on_udp, NULL);
    CHECK(wg_udp_sendto(s4, &b4, 53, d, 4) < 0 && wg_ip_output(ip, NULL, &b6, 6, d, 4) < 0 && c.total == 0, "no local address: send refused");
    for (i = 0; i < WG_IP_MAX_ADDRS; i++)
        wg_ip_add_addr(ip, &b4);
    CHECK(wg_ip_add_addr(ip, &b4) < 0, "address table bounded");
    wg_ip_free(ip);
}

static void t_unreach(void)
{
    cap_t c;
    wg_ip_t* ip;
    uint8_t b[1500], d[1400];
    const uint8_t* r;
    size_t n, rn;
    wg_ip_stats_t st;
    int i;
    memset(&c, 0, sizeof c);
    memset(d, 3, sizeof d);
    T = 1000;
    ip = stack(&c, A4, A6);
    n = mk_udp(b, 0, B4, A4, 1234, 999, d, 40);
    wg_ip_input(ip, b, n);
    r = last(&c, &rn);
    CHECK(c.n == 1 && rn == 20 + 8 + n && l4_ok(r, rn) && r[20] == 3 && r[21] == 3 && memcmp(r + 12, A4, 4) == 0 && memcmp(r + 16, B4, 4) == 0 && memcmp(r + 28, b, n) == 0,
          "v4 port unreachable 3/3 quoting the datagram");
    n = mk_udp(b, 0, B4, A4, 1234, 999, d, 1300);
    wg_ip_input(ip, b, n);
    r = last(&c, &rn);
    CHECK(rn == 576 && l4_ok(r, rn) && memcmp(r + 28, b, 548) == 0, "v4 error capped at 576 bytes");
    n = mk_udp(b, 1, B6, A6, 1234, 999, d, 1300);
    wg_ip_input(ip, b, n);
    r = last(&c, &rn);
    CHECK(rn == 1280 && l4_ok(r, rn) && r[40] == 1 && r[41] == 4 && memcmp(r + 48, b, 1232) == 0, "v6 port unreachable 1/4 capped at 1280 bytes");
    n = mk_udp(b, 1, B6, A6, 1234, 999, d, 10);
    for (i = 0; i < 20; i++)
        wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &st);
    CHECK(st.unreach_sent == 10 && st.unreach_limited == 13, "burst of 10 errors, the rest rate limited");
    T += 250;
    for (i = 0; i < 5; i++)
        wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &st);
    CHECK(st.unreach_sent == 12, "one more error token per 100 ms");
    T += 100000;
    for (i = 0; i < 20; i++)
        wg_ip_input(ip, b, n);
    wg_ip_stats(ip, &st);
    CHECK(st.unreach_sent == 22 && st.udp_no_port == 3 + 20 + 5 + 20, "bucket refills to the burst only");
    wg_ip_free(ip);
}

static void t_ports(void)
{
    cap_t c;
    wg_ip_t* ip;
    wg_udp_t *a, *b, *x, *s[16384];
    static uint8_t seen[65536];
    unsigned i, dup = 0, range = 1;
    uint16_t old;
    uint8_t p[1500], d[200];
    size_t n;
    wg_ip_stats_t st;
    memset(&c, 0, sizeof c);
    memset(d, 9, sizeof d);
    ip = stack(&c, A4, A6);
    a = wg_udp_bind(ip, 0, 80, on_udp, NULL);
    CHECK(a && wg_udp_port(a) == 80 && !wg_udp_bind(ip, 0, 80, on_udp, NULL), "fixed port in use");
    b = wg_udp_bind(ip, 1, 80, on_udp, NULL);
    CHECK(b != NULL, "same port in the other family");
    wg_udp_close(a);
    a = wg_udp_bind(ip, 0, 80, on_udp, NULL);
    CHECK(a != NULL, "port reusable after close");
    for (i = 0; i < 16384; i++) {
        s[i] = wg_udp_bind(ip, 0, 0, on_udp, NULL);
        if (!s[i] || wg_udp_port(s[i]) < WG_IP_EPH_LO)
            range = 0;
        else if (seen[wg_udp_port(s[i])]++)
            dup++;
    }
    CHECK(range && dup == 0, "16384 ephemeral ports, all distinct, all in 49152..65535");
    CHECK(wg_udp_bind(ip, 0, 0, on_udp, NULL) == NULL && wg_udp_bind(ip, 0, 50000, on_udp, NULL) == NULL, "ephemeral range exhausted");
    old = wg_udp_port(s[777]);
    wg_udp_close(s[777]);
    x = s[777] = wg_udp_bind(ip, 0, 0, on_udp, NULL);
    CHECK(x && wg_udp_port(x) == old, "freed ephemeral port reused");
    for (i = 0; i < 16384; i += 2)
        wg_udp_close(s[i]);
    n = mk_udp(p, 0, B4, A4, 1, wg_udp_port(s[1]), d, 10);
    wg_ip_input(ip, p, n);
    n = mk_udp(p, 0, B4, A4, 1, wg_udp_port(s[1]) - 1, d, 10);
    wg_ip_input(ip, p, n);
    wg_ip_stats(ip, &st);
    CHECK(st.udp_rx == 1 && st.udp_no_port == 1, "lookup after half the ports closed");
    CHECK(c.total == 1, "the closed port got one port unreachable");
    c.total = 0;
    wg_ip_set_proto(ip, WG_PROTO_TCP, on_hook, NULL);
    n = mk(p, 0, B4, A4, 6, d, 40);
    wg_ip_input(ip, p, n);
    CHECK(hook_n == 1 && hook_len == 40 && memcmp(hook_src.a, B4, 4) == 0 && memcmp(hook_dst.a, A4, 4) == 0, "TCP hook gets source, destination, segment");
    n = mk(p, 1, B6, A6, 6, d, 60);
    wg_ip_input(ip, p, n);
    CHECK(hook_n == 2 && hook_len == 60 && hook_src.v6, "TCP hook v6");
    n = mk(p, 0, B4, A4, 132, d, 40);
    wg_ip_input(ip, p, n);
    wg_ip_stats(ip, &st);
    CHECK(hook_n == 2 && st.rx_proto == 1 && c.total == 0, "unknown protocol counted, no reply");
    {
        uint8_t m[16] = { 3, 3, 0, 0 };
        put16(m + 2, ref_csum(m, 16, 0));
        n = mk(p, 0, B4, A4, 1, m, 16);
        wg_ip_input(ip, p, n);
        CHECK(hook_n == 2, "ICMP error without a hook counted");
        wg_ip_set_proto(ip, WG_PROTO_ICMP, on_hook, NULL);
        wg_ip_input(ip, p, n);
        CHECK(hook_n == 3 && c.total == 0, "ICMP error goes to the ICMP hook, never answered");
    }
    CHECK(wg_ip_set_proto(ip, 50, on_hook, NULL) == 0 && wg_ip_set_proto(ip, 51, on_hook, NULL) == 0 && wg_ip_set_proto(ip, 52, on_hook, NULL) < 0 && wg_ip_set_proto(ip, 6, NULL, NULL) == 0,
          "hook table bounded, replaceable");
    wg_ip_free(ip);
}

typedef struct {
    cap_t c;
    wg_ip_t* ip;
} end_t;

static void wire(end_t* x, end_t* y)
{
    int moved = 1;
    while (moved) {
        moved = 0;
        while (x->c.n) {
            uint8_t b[1500];
            size_t n = x->c.len[x->c.head];
            memcpy(b, x->c.q[x->c.head], n);
            x->c.head = (x->c.head + 1) % 64;
            x->c.n--;
            wg_ip_input(y->ip, b, n);
            moved = 1;
        }
        while (y->c.n) {
            uint8_t b[1500];
            size_t n = y->c.len[y->c.head];
            memcpy(b, y->c.q[y->c.head], n);
            y->c.head = (y->c.head + 1) % 64;
            y->c.n--;
            wg_ip_input(x->ip, b, n);
            moved = 1;
        }
    }
}

static void t_pair(void)
{
    static end_t e, f;
    sink_t echo, back;
    wg_udp_t *srv[2], *cli[2];
    wg_ipaddr_t fa[2] = { ad(0, B4), ad(1, B6) };
    wg_ip_stats_t se, sf;
    uint8_t d[1200], m[64];
    int v6, i, ok = 1;
    memset(&echo, 0, sizeof echo);
    memset(&back, 0, sizeof back);
    echo.echo = 1;
    e.ip = stack(&e.c, A4, A6);
    f.ip = stack(&f.c, B4, B6);
    for (v6 = 0; v6 < 2; v6++) {
        srv[v6] = wg_udp_bind(f.ip, v6, 7, on_udp, &echo);
        cli[v6] = wg_udp_bind(e.ip, v6, 0, on_udp, &back);
    }
    for (i = 0; i < 400; i++) {
        size_t n = (size_t)(rnd() % sizeof d), k;
        v6 = i & 1;
        for (k = 0; k < n; k++)
            d[k] = (uint8_t)rnd();
        wg_udp_sendto(cli[v6], &fa[v6], 7, d, n);
        wire(&e, &f);
        if (back.got != (unsigned)i + 1 || back.n != n || memcmp(back.data, d, n) != 0 || back.sport != 7 || back.src.v6 != v6)
            ok = 0;
    }
    CHECK(ok, "UDP echo across two stacks, 400 datagrams, v4 and v6, sizes 0..1199");
    for (v6 = 0; v6 < 2; v6++) {
        memset(m, 0, sizeof m);
        m[0] = v6 ? 128 : 8;
        put16(m + 6, 1);
        {
            wg_ipaddr_t s = ad(v6, v6 ? A6 : A4);
            uint16_t c = wg_csum_fin(wg_csum_acc(v6 ? wg_csum_pseudo(&s, &fa[v6], 58, sizeof m) : 0, m, sizeof m));
            memcpy(m + 2, &c, 2);
        }
        wg_ip_output(e.ip, NULL, &fa[v6], v6 ? 58 : 1, m, sizeof m);
        wire(&e, &f);
    }
    wg_ip_set_proto(e.ip, WG_PROTO_ICMP, on_hook, NULL);
    wg_ip_set_proto(e.ip, WG_PROTO_ICMP6, on_hook, NULL);
    hook_n = 0;
    for (v6 = 0; v6 < 2; v6++) {
        wg_ipaddr_t s = ad(v6, v6 ? A6 : A4);
        uint16_t c;
        m[0] = v6 ? 128 : 8;
        m[2] = m[3] = 0;
        c = wg_csum_fin(wg_csum_acc(v6 ? wg_csum_pseudo(&s, &fa[v6], 58, sizeof m) : 0, m, sizeof m));
        memcpy(m + 2, &c, 2);
        wg_ip_output(e.ip, NULL, &fa[v6], v6 ? 58 : 1, m, sizeof m);
        wire(&e, &f);
    }
    wg_ip_stats(e.ip, &se);
    wg_ip_stats(f.ip, &sf);
    CHECK(sf.echo == 4 && hook_n == 2 && se.rx_csum == 0 && sf.rx_csum == 0 && se.rx_bad == 0 && sf.rx_bad == 0, "ping across two stacks; replies reach the ICMP hooks");
    wg_ip_free(e.ip);
    wg_ip_free(f.ip);
}

static size_t mk_tcp(uint8_t* b, int v6, const uint8_t* s, const uint8_t* d, size_t n)
{
    uint8_t t[1500];
    size_t i;
    memset(t, 0, 20);
    put16(t, 4000);
    put16(t + 2, 80);
    t[12] = 0x50;
    t[13] = 0x18;
    for (i = 20; i < n + 20; i++)
        t[i] = (uint8_t)(i * 31);
    put16(t + 16, ref_csum(t, n + 20, ref_pseudo(v6, s, d, 6, n + 20)));
    return mk(b, v6, s, d, 6, t, n + 20);
}

static void t_verify(void)
{
    cap_t ca, cb;
    sink_t ka, kb;
    wg_ip_t *a, *b;
    wg_ip_stats_t sa, sb;
    uint8_t p[1500], d[1400];
    unsigned i, good = 0, caught = 0, agree = 0, tcpv = 0, trusted = 0, clean_ok = 0, clean = 0, falsepos = 0;
    memset(&ca, 0, sizeof ca);
    memset(&cb, 0, sizeof cb);
    memset(&ka, 0, sizeof ka);
    memset(&kb, 0, sizeof kb);
    ka.echo = kb.echo = 1;
    a = stack(&ca, A4, A6);
    b = stack(&cb, A4, A6);
    wg_udp_bind(a, 0, 53, on_udp, &ka);
    wg_udp_bind(a, 1, 53, on_udp, &ka);
    wg_udp_bind(b, 0, 53, on_udp, &kb);
    wg_udp_bind(b, 1, 53, on_udp, &kb);
    wg_ip_set_proto(a, WG_PROTO_TCP, on_hook, NULL);
    wg_ip_set_proto(b, WG_PROTO_TCP, on_hook, NULL);
    for (i = 0; i < 20000; i++) {
        int v6 = (int)(rnd() & 1), kind = (int)(rnd() % 4), corrupt = (int)(rnd() % 3 == 0), v;
        size_t n, m = rnd() % 1300, k;
        uint64_t before_a, before_b;
        for (k = 0; k < m; k++)
            d[k] = (uint8_t)rnd();
        if (kind == 0)
            n = mk_udp(p, v6, v6 ? B6 : B4, v6 ? A6 : A4, 7000, (rnd() & 7) ? 53 : 54, d, m);
        else if (kind == 1)
            n = mk_echo(p, v6, v6 ? B6 : B4, v6 ? A6 : A4, 7, i, m);
        else if (kind == 2)
            n = mk_tcp(p, v6, v6 ? B6 : B4, v6 ? A6 : A4, m);
        else {
            n = mk_udp(p, v6, v6 ? B6 : B4, v6 ? A6 : A4, 7000, 53, d, m);
            if (!v6)
                p[20 + 6] = p[20 + 7] = 0;
        }
        if (corrupt)
            p[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
        v = wg_ip_verify(p, n);
        if (!corrupt && kind != 3) {
            clean++;
            clean_ok += (unsigned)v;
        }
        if (corrupt && !v)
            caught++;
        tcpv += (unsigned)(v && kind == 2);
        wg_ip_stats(b, &sb);
        before_b = sb.rx_csum;
        wg_ip_stats(a, &sa);
        before_a = sa.csum_skipped;
        wg_ip_input(b, p, n);
        wg_ip_stats(b, &sb);
        if (v && sb.rx_csum != before_b)
            falsepos++;
        if (v) {
            wg_ip_input_trusted(a, p, n);
            wg_ip_stats(a, &sa);
            if (kind != 2) {
                trusted++;
                good += sa.csum_skipped == before_a + 1;
            } else
                agree += sa.csum_skipped == before_a;
        } else
            wg_ip_input(a, p, n);
    }
    wg_ip_stats(a, &sa);
    wg_ip_stats(b, &sb);
    CHECK(clean > 1000 && clean_ok == clean, "verify: every clean UDP/ICMP/TCP packet v4 and v6 with a checksum verifies");
    CHECK(caught > 500, "verify: bit flips that break a checksum are not verified");
    CHECK(!falsepos, "verify: a verified packet never fails the main-thread checksum (no false positive)");
    CHECK(good == trusted && sa.csum_skipped == trusted, "trusted input skips exactly one L4 checksum per verified UDP/ICMP packet");
    CHECK(sa.rx == sb.rx && sa.rx_bad == sb.rx_bad && sa.udp_rx == sb.udp_rx && sa.echo == sb.echo && sa.udp_no_port == sb.udp_no_port && sa.rx_csum == sb.rx_csum && ka.got == kb.got && ca.total == cb.total,
          "differential: trusted and untrusted stacks give identical deliveries, replies and drop counters");
    CHECK(sb.csum_skipped == 0 && agree == tcpv && tcpv > 100, "untrusted input never skips; TCP checksums are left to the TCP hook");
    {
        uint8_t q[1500];
        size_t n = mk_udp(q, 0, B4, A4, 7000, 53, d, 100);
        unsigned g = ka.got;
        q[30] ^= 1;
        wg_ip_input_trusted(a, q, n);
        CHECK(!wg_ip_take_trust(a), "trust does not outlive the trusted input call");
        wg_ip_input(a, q, n);
        CHECK(ka.got == g + 1, "trust is scoped to one call: the same corrupt packet untrusted is dropped");
    }
    {
        uint8_t q[1500], z[16];
        wg_ipaddr_t S, D;
        uint16_t x;
        size_t n;
        unsigned g = ka.got;
        memset(z, 0, sizeof z);
        memset(&S, 0, sizeof S);
        memset(&D, 0, sizeof D);
        memcpy(S.a, B4, 4);
        memcpy(D.a, A4, 4);
        n = mk_udp(q, 0, B4, A4, 7000, 53, z, sizeof z);
        q[26] = q[27] = 0;
        x = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&S, &D, WG_PROTO_UDP, n - 20), q + 20, n - 20));
        memcpy(q + 28, &x, 2);
        CHECK(!wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&S, &D, WG_PROTO_UDP, n - 20), q + 20, n - 20)), "crafted v4 UDP whose data sums to ffff with a zero checksum field");
        CHECK(!wg_ip_verify(q, n), "verify: v4 UDP zero checksum means none was sent, never verified even when the sum works out");
        wg_ip_input(a, q, n);
        CHECK(ka.got == g + 1, "and the stack still delivers it (RFC 768: no checksum)");
    }
    wg_ip_free(a);
    wg_ip_free(b);
}

static void t_fuzz(void)
{
    uint8_t b[1600], s[1500], d[300];
    static const uint8_t hd[] = { 0, 60 };
    size_t n;
    int i, k;
    memset(d, 1, sizeof d);
    for (i = 0; i < 40000; i++) {
        switch (i % 6) {
        case 0:
            n = mk_udp(s, 0, B4, (const uint8_t[]){ 10, 0, 0, 2 }, 9, rnd() & 1 ? 53 : 54, d, (size_t)(rnd() % 300));
            break;
        case 1:
            n = mk_udp(s, 1, B6, (const uint8_t[]){ 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 }, 9, 53, d, (size_t)(rnd() % 300));
            break;
        case 2:
            n = mk_echo(s, (int)(rnd() & 1), B6, (const uint8_t[]){ 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 }, 1, 1, (size_t)(rnd() % 200));
            break;
        case 3:
            n = v6_ext(s, hd, 2, NULL, 0);
            s[24 + 15] = 2;
            break;
        default:
            n = (size_t)(rnd() % 200);
            for (k = 0; k < (int)n; k++)
                s[k] = (uint8_t)rnd();
            s[0] = (uint8_t)(rnd() & 1 ? 0x45 : 0x60);
            break;
        }
        for (k = (int)(rnd() % 4); k > 0 && n; k--)
            s[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
        if (rnd() % 5 == 0 && n)
            n = (size_t)(rnd() % n);
        b[0] = (uint8_t)(rnd() % 3 ? 1 : 0) | (uint8_t)(rnd() & 0xfe);
        memcpy(b + 1, s, n);
        LLVMFuzzerTestOneInput(b, n + 1);
    }
    CHECK(1, "fuzz entry: 40000 seeded and mutated packets without a sanitizer report or output-check abort");
}

enum { E_BAD, E_CSUM, E_FRAG, E_NOTLOCAL, E_PROTO, E_UDP, E_NOPORT, E_ECHO, E_HOOK, E_N };

static int naive_src_ok(int v6, const uint8_t* a)
{
    static const uint8_t z[16];
    if (!v6)
        return !(a[0] == 0 || a[0] == 127 || a[0] >= 224);
    return !(a[0] == 0xff || !memcmp(a, z, 16) || (!memcmp(a, z, 15) && a[15] == 1));
}

static int naive_ok(const uint8_t* p, size_t n, uint32_t init)
{
    return ref_fold(ref_sum(p, n, init)) == 0xffff;
}

static int naive_l4(int v6, const uint8_t* s, const uint8_t* d, uint8_t proto, const uint8_t* l4, size_t n)
{
    if (!naive_src_ok(v6, s))
        return E_BAD;
    if (memcmp(d, v6 ? A6 : A4, v6 ? 16 : 4))
        return E_NOTLOCAL;
    if (proto == 17) {
        size_t ul;
        if (n < 8 || (ul = be16(l4 + 4)) < 8 || ul > n)
            return E_BAD;
        if (be16(l4 + 6) == 0 ? v6 : !naive_ok(l4, ul, ref_pseudo(v6, s, d, 17, ul)))
            return E_CSUM;
        return be16(l4 + 2) == 53 ? E_UDP : E_NOPORT;
    }
    if (proto == (v6 ? 58 : 1)) {
        if (n < 8 || !naive_ok(l4, n, v6 ? ref_pseudo(1, s, d, 58, n) : 0))
            return E_CSUM;
        if (l4[0] == (v6 ? 128 : 8) && l4[1] == 0)
            return n > 1420 - (v6 ? 40u : 20u) ? E_BAD : E_ECHO;
        return E_PROTO;
    }
    return proto == 6 ? E_HOOK : E_PROTO;
}

static int naive_v4(const uint8_t* p, size_t n, size_t* l4o)
{
    size_t hl, tot;
    if (n < 20 || (hl = (size_t)(p[0] & 15) * 4) < 20 || hl > n || (tot = be16(p + 2)) < hl || tot > n)
        return E_BAD;
    if (!naive_ok(p, hl, 0))
        return E_CSUM;
    if ((p[6] & 0x20) || ((p[6] & 0x1f) | p[7]))
        return E_FRAG;
    *l4o = hl;
    return naive_l4(0, p + 12, p + 16, p[9], p + hl, tot - hl);
}

static int naive_opts(const uint8_t* o, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (o[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= n || i + 2 + (size_t)o[i + 1] > n)
            return 0;
        if (o[i] != 1 && (o[i] & 0xc0))
            return 0;
        i += 2 + o[i + 1];
    }
    return 1;
}

static int naive_v6(const uint8_t* p, size_t n, size_t* l4o)
{
    size_t off = 40, end;
    unsigned k = 0;
    uint8_t nh;
    if (n < 40 || (end = 40 + (size_t)be16(p + 4)) > n)
        return E_BAD;
    nh = p[6];
    while (nh == 0 || nh == 43 || nh == 60) {
        size_t l;
        if (nh == 0 && k > 0)
            return E_BAD;
        if (k == 8 || off + 8 > end)
            return E_BAD;
        l = 8 * ((size_t)p[off + 1] + 1);
        if (off + l > end)
            return E_BAD;
        if (nh == 43 ? p[off + 3] != 0 : !naive_opts(p + off + 2, l - 2))
            return E_BAD;
        nh = p[off];
        off += l;
        k++;
    }
    if (nh == 44)
        return E_FRAG;
    *l4o = off;
    return naive_l4(1, p + 8, p + 24, nh, p + off, end - off);
}

static uint64_t stat_of(const wg_ip_stats_t* s, int e)
{
    switch (e) {
    case E_BAD: return s->rx_bad;
    case E_CSUM: return s->rx_csum;
    case E_FRAG: return s->rx_frag;
    case E_NOTLOCAL: return s->rx_not_local;
    case E_PROTO: return s->rx_proto;
    case E_UDP: return s->udp_rx;
    case E_NOPORT: return s->udp_no_port;
    case E_ECHO: return s->echo;
    default: return hook_n;
    }
}

static void fix16(uint8_t* f, uint16_t v)
{
    put16(f, v);
}

static size_t gen_l4(fk_rng_t* r, int v6, const uint8_t* s, const uint8_t* d, uint8_t* l4, uint8_t* proto, size_t room)
{
    unsigned kind = (unsigned)fk_below(r, 6);
    size_t n = (size_t)fk_below(r, room < 300 ? room : 300), ul;
    if (room > 1440 && fk_chance(r, 4))
        n = 1340 + (size_t)fk_below(r, 100);
    if (kind <= 1) {
        *proto = 17;
        if (n < 8)
            n = 8;
        fk_bytes(r, l4, n);
        put16(l4 + 2, fk_chance(r, 70) ? 53 : 54 + (unsigned)fk_below(r, 1000));
        ul = fk_chance(r, 85) ? n : (size_t)fk_below(r, n + 9);
        fix16(l4 + 4, (uint16_t)ul);
        fix16(l4 + 6, 0);
        if (ul >= 8 && ul <= n && !fk_chance(r, 10)) {
            uint16_t c = ref_csum(l4, ul, ref_pseudo(v6, s, d, 17, ul));
            fix16(l4 + 6, c ? c : 0xffff);
        }
        if (fk_chance(r, 8))
            fix16(l4 + 6, 0);
        if (fk_chance(r, 5))
            l4[fk_below(r, n)] ^= (uint8_t)(1 + fk_below(r, 255));
    } else if (kind <= 3) {
        *proto = v6 ? 58 : 1;
        if (n < 8 && fk_chance(r, 90))
            n = 8;
        fk_bytes(r, l4, n);
        if (n >= 2 && fk_chance(r, 70))
            l4[0] = v6 ? 128 : 8, l4[1] = 0;
        if (n >= 4) {
            fix16(l4 + 2, 0);
            fix16(l4 + 2, ref_csum(l4, n, v6 ? ref_pseudo(1, s, d, 58, n) : 0));
            if (fk_chance(r, 8))
                l4[fk_below(r, n)] ^= (uint8_t)(1 + fk_below(r, 255));
        }
    } else {
        *proto = kind == 4 ? 6 : (uint8_t)fk_next(r);
        fk_bytes(r, l4, n);
    }
    return n;
}

static void pick_addr(fk_rng_t* r, int v6, uint8_t* a, int dst)
{
    unsigned k = (unsigned)fk_below(r, 10);
    if (k == 7 && dst && fk_chance(r, 50)) {
        fk_bytes(r, a, v6 ? 16 : 4);
        memcpy(a, v6 ? A4 : A6, 4);
        return;
    }
    if (k < 6)
        memcpy(a, dst ? (v6 ? A6 : A4) : (v6 ? B6 : B4), v6 ? 16 : 4);
    else if (k == 6)
        fk_bytes(r, a, v6 ? 16 : 4);
    else if (k == 7 && dst) {
        memcpy(a, v6 ? A6 : A4, v6 ? 16 : 4);
        a[fk_below(r, v6 ? 16 : 4)] ^= (uint8_t)(1 + fk_below(r, 255));
    } else if (k == 7) {
        fk_bytes(r, a, v6 ? 16 : 4);
        memcpy(a, v6 ? A4 : A6, 4);
    }
    else if (!v6) {
        static const uint8_t first[] = { 0, 127, 224, 239, 240, 255, 1, 126, 128, 223 };
        fk_bytes(r, a, 4);
        a[0] = first[fk_below(r, sizeof first)];
    } else {
        memset(a, 0, 16);
        if (fk_chance(r, 33))
            a[0] = 0xff, a[1] = (uint8_t)fk_next(r);
        else if (fk_chance(r, 50))
            a[15] = (uint8_t)fk_below(r, 3);
        else
            a[0] = 0xfe, a[15] = 1;
    }
}

static size_t gen_v4(fk_rng_t* r, uint8_t* p)
{
    uint8_t s[4], d[4], proto;
    size_t ihl = fk_chance(r, 80) ? 20 : 20 + 4 * (size_t)fk_below(r, 11), n, tot;
    pick_addr(r, 0, s, 0);
    pick_addr(r, 0, d, 1);
    memset(p, 0, ihl);
    fk_bytes(r, p + 20, ihl - 20);
    n = gen_l4(r, 0, s, d, p + ihl, &proto, 1560 - ihl);
    p[0] = (uint8_t)(0x40 | (ihl / 4));
    tot = ihl + n;
    put16(p + 2, (unsigned)tot);
    p[8] = (uint8_t)fk_next(r);
    p[9] = proto;
    memcpy(p + 12, s, 4);
    memcpy(p + 16, d, 4);
    if (fk_chance(r, 15)) {
        unsigned f = (unsigned)fk_below(r, 4);
        p[6] = f == 0 ? 0x40 : f == 1 ? 0x20 : (uint8_t)fk_next(r);
        p[7] = f >= 2 ? (uint8_t)fk_next(r) : 0;
    }
    if (fk_chance(r, 10))
        p[0] = (uint8_t)(0x40 | fk_below(r, 16));
    if (fk_chance(r, 10))
        put16(p + 2, (unsigned)fk_below(r, tot + 30));
    put16(p + 10, ref_csum(p, (size_t)(p[0] & 15) * 4 >= 20 ? (size_t)(p[0] & 15) * 4 : 20, 0));
    if (fk_chance(r, 8))
        p[10 + fk_below(r, 2)] ^= (uint8_t)(1 + fk_below(r, 255));
    if (fk_chance(r, 10))
        tot += (size_t)fk_below(r, 40);
    return fk_chance(r, 3) ? (size_t)fk_below(r, tot + 1) : tot;
}

static size_t gen_v6(fk_rng_t* r, uint8_t* p)
{
    uint8_t s[16], d[16], proto, *nhp = p + 6;
    size_t off = 40, n, k, next = (size_t)fk_below(r, 11);
    int chain = fk_chance(r, 6);
    pick_addr(r, 1, s, 0);
    pick_addr(r, 1, d, 1);
    memset(p, 0, 40);
    p[0] = 0x60;
    p[7] = 64;
    memcpy(p + 8, s, 16);
    memcpy(p + 24, d, 16);
    if (fk_chance(r, 50))
        next = 0;
    if (chain)
        next = 7 + (size_t)fk_below(r, 4);
    for (k = 0; k < next && off < 900; k++) {
        static const uint8_t types[] = { 0, 43, 60, 60, 44 };
        uint8_t t = chain ? (fk_chance(r, 50) ? 43 : 60) : types[fk_below(r, sizeof types)];
        size_t l = t == 44 ? 8 : 8 * (1 + (size_t)fk_below(r, 3)), i;
        *nhp = t;
        memset(p + off, 0, l);
        p[off + 1] = (uint8_t)(l / 8 - 1);
        if (t == 43) {
            p[off + 2] = (uint8_t)fk_next(r);
            p[off + 3] = !chain && fk_chance(r, 15) ? (uint8_t)(1 + fk_below(r, 5)) : 0;
        } else if (t != 44) {
            i = 2;
            while (i < l) {
                unsigned o = (unsigned)fk_below(r, chain ? 6 : 10);
                if (o < 3)
                    p[off + i++] = 0;
                else {
                    size_t room = l - i, ol;
                    if (room < 2) {
                        p[off + i++] = 0;
                        continue;
                    }
                    ol = (size_t)fk_below(r, room - 1);
                    p[off + i] = o < 6 ? 1 : (uint8_t)(fk_chance(r, 50) ? fk_below(r, 64) + 2 : fk_next(r));
                    p[off + i + 1] = (uint8_t)(!chain && fk_chance(r, 7) ? ol + 1 + fk_below(r, 4) : ol);
                    fk_bytes(r, p + off + i + 2, ol);
                    i += 2 + ol;
                }
            }
        }
        nhp = p + off;
        off += l;
    }
    n = gen_l4(r, 1, s, d, p + off, &proto, 1560 - off);
    *nhp = proto;
    put16(p + 4, (unsigned)(off - 40 + n));
    if (fk_chance(r, 8))
        put16(p + 4, (unsigned)fk_below(r, off - 40 + n + 30));
    n += off;
    if (fk_chance(r, 10))
        n += (size_t)fk_below(r, 40);
    return fk_chance(r, 3) ? (size_t)fk_below(r, n + 1) : n;
}

static void p_ip_classify(void)
{
    fk_rng_t r;
    static uint8_t p[1600];
    static cap_t c;
    static sink_t k4, k6;
    wg_ip_t* ip;
    unsigned i, n, mism = 0, udp_bad = 0, echo_bad = 0, big_echo = 0, reached[E_N];
    memset(&c, 0, sizeof c);
    memset(&k4, 0, sizeof k4);
    memset(&k6, 0, sizeof k6);
    memset(reached, 0, sizeof reached);
    if (!fk_only("ip-classify"))
        return;
    r = fk_stream("ip-classify");
    ip = stack(&c, A4, A6);
    wg_udp_bind(ip, 0, 53, on_udp, &k4);
    wg_udp_bind(ip, 1, 53, on_udp, &k6);
    wg_ip_set_proto(ip, WG_PROTO_TCP, on_hook, NULL);
    n = (unsigned)fk_iters(60000);
    for (i = 0; i < n; i++) {
        int v6 = fk_chance(&r, 50), want, e, bad = 0;
        size_t len, l4o = 0;
        wg_ip_stats_t a, b;
        uint64_t before[E_N];
        unsigned g4 = k4.got, g6 = k6.got;
        memset(p, 0, sizeof p);
        len = v6 ? gen_v6(&r, p) : gen_v4(&r, p);
        if (fk_chance(&r, 2))
            p[0] = (uint8_t)((fk_below(&r, 16) << 4) | (p[0] & 15));
        {
            unsigned ver = len ? p[0] >> 4 : 0;
            want = ver == 4 ? naive_v4(p, len, &l4o) : ver == 6 ? naive_v6(p, len, &l4o) : E_BAD;
            v6 = ver == 6;
        }
        wg_ip_stats(ip, &a);
        for (e = 0; e < E_N; e++)
            before[e] = stat_of(&a, e);
        clear(&c);
        wg_ip_input(ip, p, len);
        wg_ip_stats(ip, &b);
        if (b.rx != a.rx + 1)
            bad = 1;
        for (e = 0; e < E_N; e++)
            if (stat_of(&b, e) != before[e] + (e == want))
                bad = 1;
        mism += bad;
        reached[want]++;
        if (len > 1300 && ((len ? p[0] >> 4 : 0) == 4 ? p[9] == 1 : (len ? p[0] >> 4 : 0) == 6 && p[6] == 58) && (len > l4o + 1 ? p[l4o] == (v6 ? 128 : 8) && p[l4o + 1] == 0 : 0))
            big_echo++;
        if (want == E_UDP) {
            if ((v6 ? k6.got != g6 + 1 || memcmp(k6.data, p + l4o + 8, k6.n) || k6.n != (size_t)be16(p + l4o + 4) - 8 : k4.got != g4 + 1 || memcmp(k4.data, p + l4o + 8, k4.n) || k4.n != (size_t)be16(p + l4o + 4) - 8))
                udp_bad++;
        } else if (k4.got != g4 || k6.got != g6)
            udp_bad++;
        if (want == E_ECHO) {
            size_t rn;
            const uint8_t* q = last(&c, &rn);
            size_t hl = v6 ? 40 : 20;
            if (!c.n || rn < hl + 8 || (v6 ? memcmp(q + 24, p + 8, 16) || memcmp(q + 8, p + 24, 16) || q[40] != 129 || !naive_ok(q + 40, rn - 40, ref_pseudo(1, q + 8, q + 24, 58, rn - 40)) : memcmp(q + 16, p + 12, 4) || memcmp(q + 12, p + 16, 4) || q[20] != 0 || !naive_ok(q, 20, 0) || !naive_ok(q + 20, rn - 20, 0)) || memcmp(q + hl + 4, p + l4o + 4, rn - hl - 4))
                echo_bad++;
        }
    }
    printf("ip-classify: %u packets: big-echo %u bad %u csum %u frag %u not-local %u proto %u udp %u no-port %u echo %u hook %u\n", n, big_echo, reached[E_BAD], reached[E_CSUM], reached[E_FRAG], reached[E_NOTLOCAL], reached[E_PROTO], reached[E_UDP], reached[E_NOPORT], reached[E_ECHO], reached[E_HOOK]);
    wg_ip_free(ip);
    CHECK(mism == 0, "ip-classify: random v4/v6 packets (IHL, lengths, padding, header checksum, MF/DF/offset, martians, non-local, v6 extension chains with options, routing, fragment) land in exactly the counter a naive RFC 791/8200/768/792 validator predicts");
    CHECK(udp_bad == 0, "ip-classify: exactly the accepted UDP datagrams reach the socket, with their own length");
    CHECK(echo_bad == 0, "ip-classify: every accepted echo request gets a reply with swapped addresses and valid checksums");
    CHECK(reached[E_BAD] && reached[E_CSUM] && reached[E_FRAG] && reached[E_NOTLOCAL] && reached[E_PROTO] && reached[E_UDP] && reached[E_NOPORT] && reached[E_ECHO] && reached[E_HOOK], "ip-classify: every outcome was reached");
}

static void p_csum_naive(void)
{
    fk_rng_t r;
    static uint8_t big[9000];
    unsigned i, n, bad = 0;
    if (!fk_only("csum-naive"))
        return;
    r = fk_stream("csum-naive");
    n = (unsigned)fk_iters(20000);
    for (i = 0; i < n; i++) {
        size_t len = (size_t)fk_below(&r, 4097), off = (size_t)fk_below(&r, 16), parts = 1 + (size_t)fk_below(&r, 5), at = 0, j;
        uint8_t* q = big + off;
        uint64_t acc = 0;
        uint16_t want, got;
        fk_bytes(&r, q, len);
        if (fk_chance(&r, 10))
            memset(q, fk_chance(&r, 50) ? 0xff : 0, len);
        want = ref_csum(q, len, 0);
        for (j = 0; j < parts && at < len; j++) {
            size_t c = j + 1 == parts ? len - at : ((size_t)fk_below(&r, len - at + 1)) & ~(size_t)1;
            acc = wg_csum_acc(acc, q + at, c);
            at += c;
        }
        if (at < len)
            acc = wg_csum_acc(acc, q + at, len - at);
        got = wg_csum_fin(acc);
        bad += ((const uint8_t*)&got)[0] != (uint8_t)(want >> 8) || ((const uint8_t*)&got)[1] != (uint8_t)want;
    }
    CHECK(bad == 0, "csum-naive: checksum over 1..5 even-split chunks, alignments 0..15, lengths 0..4096 equals the bytewise RFC 1071 reference");
}

static void p_ip_reseeded(void)
{
    fk_rng_t r;
    if (!fk_only("ip-reseeded"))
        return;
    r = fk_stream("ip-reseeded");
    rs = fk_next(&r) | 1;
    t_verify();
    t_fuzz();
    {
        uint8_t b[200], d[40];
        size_t n;
        unsigned k, good = 0, frag_ok = 0;
        for (k = 0; k < 64; k++) {
            int v6 = (int)(k & 1);
            fk_bytes(&r, d, sizeof d);
            n = mk_udp(b, v6, v6 ? B6 : B4, v6 ? A6 : A4, 7, 53, d, 1 + (size_t)fk_below(&r, sizeof d));
            good += wg_ip_verify(b, n) != 0;
            if (!v6) {
                b[6] = (uint8_t)(fk_chance(&r, 50) ? 0x20 : 0x40 | (1 + fk_below(&r, 31)));
                b[7] = (uint8_t)fk_next(&r);
                put16(b + 10, 0);
                put16(b + 10, ref_csum(b, 20, 0));
                frag_ok += wg_ip_verify(b, n) != 0;
            }
        }
        CHECK(good == 64 && frag_ok == 0, "ip-reseeded: verify accepts clean UDP, v4 and v6, and never a v4 fragment (MF or offset)");
    }
}

int main(void)
{
    fk_init("test_wg_ip");
    if (fk_legacy()) {
        t_csum();
        t_echo();
        t_udp_rx();
        t_hdr();
        t_v6ext();
        t_udp_tx();
        t_unreach();
        t_ports();
        t_pair();
        t_verify();
        t_fuzz();
    }
    p_ip_classify();
    p_csum_naive();
    p_ip_reseeded();
    printf("test_wg_ip: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
