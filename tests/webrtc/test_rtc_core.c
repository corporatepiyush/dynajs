/*
 * test_rtc_core.c -- src/core/rtc (STUN, ICE, TURN client, SCTP, SDP) as a standalone C library.
 *
 * Oracles, none derived from the code under test:
 * - STUN: the RFC 5769 sample request, IPv4 and IPv6 responses (integrity, fingerprint, XOR-MAPPED-ADDRESS),
 *   copied byte for byte from the RFC text; built messages must verify with the RFC's password.
 * - CRC-32c: the RFC 3720 B.4 check value of "123456789" (0xE3069283), used by the SCTP common header.
 * - SCTP: two associations over a simulated network with seeded loss, duplication and reordering schedules;
 *   every message must arrive intact, exactly once, in order where ordered (RFC 9260), partially reliable
 *   messages may be dropped but never corrupted or reordered (RFC 3758), stream reset closes both directions
 *   (RFC 6525 / RFC 8831). Large messages (up to 1 MiB) exercise fragmentation and reassembly.
 * - SCTP early-delivery head search (R2b-3 round 3): the next ordered head is found through an open-addressed (stream, ssn) index of
 *   stashed ordered B chunks (2048 uint16 slots, at most RWND/SLOT = 819 live; removed on take and on tomb), so a miss costs O(1)
 *   even with ~2000 delivered unordered tombs in the window; the oracle is the step counter bound 4*(ordered + unordered) and, with
 *   400 same-stream heads far ahead plus 8 chains released one after another, 8*(heads + fillers) and full delivery.
 * - ICE: two agents with host candidates must select one pair; both-controlling starts must resolve the role
 *   conflict (RFC 8445 7.3.1.1); wrong passwords must never connect.
 * - TURN/STUN client: the RFC 8489 6.2.1 retransmission schedule (RTO 500 ms doubling, Rc = 7, Rm = 16) on a
 *   simulated clock, ending in error 701.
 * - TURN channels: RFC 8656 12 keeps a binding 10 min past its last refresh and forbids rebinding its number or peer for
 *   5 more min, so a freed channel is held until its last ChannelBind send + 900 s; a returning peer gets its old number.
 *   ChannelData or a Data indication a session does not own returns 0 (not consumed), so a STUN session on the same
 *   server never swallows the TURN session's relay data (R2b-2); the transport then drops it (ICE ignores non-Binding).
 * - STUN 420: a Binding request with an unknown comprehension-required attribute gets an authenticated 420 whose
 *   UNKNOWN-ATTRIBUTES names exactly that attribute (RFC 8489 6.3.1); comprehension-optional ones are ignored.
 * - DCEP: RFC 8832 5.1 DATA_CHANNEL_OPEN bytes written out by hand (both reliability encodings) must parse and
 *   must be what the writer emits; truncated, unknown-type and overlong inputs are rejected.
 * - SDP: a browser-shaped offer parses into the expected fields; malformed lines and candidates are rejected.
 * - Seeded schedules use the ChaCha20 rtc_rng; under 30% two-way loss the RTO backs off to its 60 s cap (RFC 9260 RTO.Max), so the
 *   partial-reliability case waits up to one simulated hour for the final FORWARD-TSN instead of a fixed 600 s window.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dyn-rtc.h"
#include "dyn-hash.h"

static int fails, checks;

#define CHECK(c, ...)                                  \
    do {                                               \
        checks++;                                      \
        if (!(c)) {                                    \
            fails++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

static size_t unhex(const char* s, uint8_t* out)
{
    size_t n = 0;
    while (*s) {
        int a, b;
        while (*s == ' ' || *s == '\n')
            s++;
        if (!*s)
            break;
        a = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
        b = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (uint8_t)(a * 16 + b);
        s += 2;
    }
    return n;
}

static const char* const rfc5769_req = "00 01 00 58 21 12 a4 42 b7 e7 a7 01 bc 34 d6 86 fa 87 df ae 80 22 00 10 53 54 55 4e"
                                       " 20 74 65 73 74 20 63 6c 69 65 6e 74 00 24 00 04 6e 00 01 ff 80 29 00 08 93 2f f9 b1"
                                       " 51 26 3b 36 00 06 00 09 65 76 74 6a 3a 68 36 76 59 20 20 20 00 08 00 14 9a ea a7 0c"
                                       " bf d8 cb 56 78 1e f2 b5 b2 d3 f2 49 c1 b5 71 a2 80 28 00 04 e5 7a 3b cf";
static const char* const rfc5769_v4 = "01 01 00 3c 21 12 a4 42 b7 e7 a7 01 bc 34 d6 86 fa 87 df ae 80 22 00 0b 74 65 73 74"
                                      " 20 76 65 63 74 6f 72 20 00 20 00 08 00 01 a1 47 e1 12 a6 43 00 08 00 14 2b 91 f5 99"
                                      " fd 9e 90 c3 8c 74 89 f9 2a f9 ba 53 f0 6b e7 d7 80 28 00 04 c0 7d 4c 96";
static const char* const rfc5769_v6 = "01 01 00 48 21 12 a4 42 b7 e7 a7 01 bc 34 d6 86 fa 87 df ae 80 22 00 0b 74 65 73 74"
                                      " 20 76 65 63 74 6f 72 20 00 20 00 14 00 02 a1 47 01 13 a9 fa a5 d3 f1 79 bc 25 f4 b5"
                                      " be d2 b9 d9 00 08 00 14 a3 82 95 4e 4b e6 7b f1 17 84 c9 7c 82 92 c2 75 bf e3 ed 41"
                                      " 80 28 00 04 c8 fb 0b 4c";
static const char* const rfc5769_pwd = "VOkJxbRl1RmTxUk/WvJxBt";

static void test_stun(void)
{
    uint8_t b[256], tid[12], out[256];
    size_t n;
    rtc_stun_msg m;
    rtc_addr a, want;
    uint32_t prio = 0;
    uint64_t tb = 0;
    rtc_stun_wr w;
    const rtc_stun_attr* u;

    n = unhex(rfc5769_req, b);
    CHECK(rtc_stun_parse(&m, b, n) == 0, "5769 request parses");
    CHECK(rtc_stun_method(m.type) == RTC_STUN_BINDING && rtc_stun_class(m.type) == RTC_STUN_REQUEST, "binding request");
    CHECK(rtc_stun_check_fingerprint(&m), "5769 request fingerprint");
    CHECK(rtc_stun_check_integrity(&m, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd)), "5769 request integrity");
    CHECK(!rtc_stun_check_integrity(&m, (const uint8_t*)"wrong", 5), "wrong password fails");
    CHECK(rtc_stun_get_u32(&m, RTC_SA_PRIORITY, &prio) == 0 && prio == 0x6e0001ffu, "priority");
    CHECK(rtc_stun_get_u64(&m, RTC_SA_ICE_CONTROLLED, &tb) == 0 && tb == 0x932ff9b151263b36ull, "tiebreaker");
    u = rtc_stun_find(&m, RTC_SA_USERNAME);
    CHECK(u && u->len == 9 && !memcmp(b + u->off, "evtj:h6vY", 9), "username");
    b[30] ^= 1;
    CHECK(rtc_stun_parse(&m, b, n) == 0 && !rtc_stun_check_fingerprint(&m), "flipped bit breaks fingerprint");
    CHECK(!rtc_stun_check_integrity(&m, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd)), "flipped bit breaks integrity");

    n = unhex(rfc5769_v4, b);
    CHECK(rtc_stun_parse(&m, b, n) == 0, "5769 v4 parses");
    CHECK(rtc_stun_check_fingerprint(&m) && rtc_stun_check_integrity(&m, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd)), "v4 auth");
    rtc_addr_parse("192.0.2.1", 9, 32853, &want);
    CHECK(rtc_stun_get_addr(&m, rtc_stun_find(&m, RTC_SA_XOR_MAPPED_ADDRESS), &a) == 0 && rtc_addr_eq(&a, &want), "v4 xor-mapped");

    n = unhex(rfc5769_v6, b);
    CHECK(rtc_stun_parse(&m, b, n) == 0, "5769 v6 parses");
    CHECK(rtc_stun_check_fingerprint(&m) && rtc_stun_check_integrity(&m, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd)), "v6 auth");
    rtc_addr_parse("2001:db8:1234:5678:11:2233:4455:6677", 36, 32853, &want);
    CHECK(rtc_stun_get_addr(&m, rtc_stun_find(&m, RTC_SA_XOR_MAPPED_ADDRESS), &a) == 0 && rtc_addr_eq(&a, &want), "v6 xor-mapped");

    memcpy(tid, b + 8, 12);
    rtc_stun_wr_init(&w, out, sizeof(out), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_SUCCESS), tid);
    rtc_stun_wr_attr(&w, RTC_SA_SOFTWARE, "test vector", 11);
    rtc_stun_wr_addr(&w, RTC_SA_XOR_MAPPED_ADDRESS, &want);
    rtc_stun_wr_integrity(&w, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd));
    rtc_stun_wr_fingerprint(&w);
    n = rtc_stun_wr_done(&w);
    CHECK(n == 92, "built v6 response length %zu", n);
    CHECK(!memcmp(out + 36, b + 36, 24), "built XOR-MAPPED-ADDRESS matches RFC bytes");
    CHECK(rtc_stun_parse(&m, out, n) == 0 && rtc_stun_check_integrity(&m, (const uint8_t*)rfc5769_pwd, strlen(rfc5769_pwd))
            && rtc_stun_check_fingerprint(&m),
        "built message verifies");
    CHECK(rtc_stun_parse(&m, out, n - 1) < 0, "truncated rejected");
    out[3] = (uint8_t)(out[3] + 4);
    CHECK(rtc_stun_parse(&m, out, n) < 0, "bad length rejected");
    CHECK(!rtc_stun_is((const uint8_t*)"\x80\x01\x00\x00\x21\x12\xa4\x42xxxxxxxxxxxx", 20), "first byte >3 is not STUN");
    {
        uint8_t key[16], md[16];
        dyn_md5((const uint8_t*)"user:realm:pass", 15, md);
        rtc_stun_long_term_key("user", "realm", "pass", key);
        CHECK(!memcmp(key, md, 16), "long-term key = MD5(user:realm:pass)");
    }
}

static void test_crc(void)
{
    CHECK(dyn_crc32c((const uint8_t*)"123456789", 9) == 0xE3069283u, "crc32c check value");
    CHECK(dyn_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 check value");
}

typedef struct {
    uint64_t at;
    int to;
    size_t n;
    uint8_t* p;
} sim_pkt;

typedef struct {
    sim_pkt q[200000];
    int nq;
    uint64_t now;
    rtc_rng rng;
    unsigned loss_pct, dup_pct, reorder_pct;
    int drop_next;
    uint64_t delay;
    rtc_sctp* ep[2];
    int cur;
    int established[2];
    int aborted[2];
    int reset_in[2][64];
    int reset_out[2][64];
    int reset_req[2][64];
    uint8_t* rx_data[2];
    size_t rx_len[2];
    size_t rx_cap[2];
    uint32_t rx_count[2];
    uint32_t next_seq[2][64];
    int order_bad;
    int corrupt;
    uint32_t log_n[2];
    uint32_t first_chunk[2][16];
    uint8_t fwd_seen[8192];
    uint32_t ctl_zero;
    uint32_t fwd_bad;
    size_t log_len[2][32];
    size_t sent_done[2];
} sim_t;

static sim_t* SIM;
static uint32_t sim_mtu0;

static void sim_enqueue(sim_t* s, int to, const uint8_t* p, size_t n, uint64_t at)
{
    sim_pkt* k;
    if (s->nq >= (int)(sizeof(s->q) / sizeof(s->q[0])))
        return;
    k = &s->q[s->nq++];
    k->at = at;
    k->to = to;
    k->n = n;
    k->p = (uint8_t*)malloc(n);
    memcpy(k->p, p, n);
}

static void cb_packet(void* ud, const uint8_t* p, size_t n)
{
    sim_t* s = SIM;
    int to = (int)(intptr_t)ud ^ 1;
    if (n > 12 && p[12] < 16)
        s->first_chunk[to ^ 1][p[12]]++;
    if (n > 12 && (p[12] == RTC_SCTP_INIT || p[12] == RTC_SCTP_INIT_ACK || p[12] == RTC_SCTP_COOKIE_ECHO) && !p[8] && !p[9] && !p[10] && !p[11])
        s->ctl_zero++;
    if ((to ^ 1) == 0) {
        size_t off = 12;
        while (off + 4 <= n) {
            uint16_t l = rtc_rd16(p + off + 2);
            size_t k;
            if (l < 4 || off + l > n)
                break;
            if (p[off] == RTC_SCTP_FORWARD_TSN && l >= 8)
                for (k = off + 8; k + 4 <= off + l; k += 4)
                    s->fwd_seen[rtc_rd16(p + k) >> 3] |= (uint8_t)(1u << (rtc_rd16(p + k) & 7));
            off += ((size_t)l + 3) & ~(size_t)3;
        }
    }
    uint64_t at = s->now + s->delay;
    unsigned r = (unsigned)(rtc_rng_next(&s->rng) % 100);
    if (to == 1 && s->drop_next > 0) {
        s->drop_next--;
        return;
    }
    if (r < s->loss_pct)
        return;
    if ((unsigned)(rtc_rng_next(&s->rng) % 100) < s->reorder_pct)
        at += s->delay * 3;
    sim_enqueue(s, to, p, n, at);
    if ((unsigned)(rtc_rng_next(&s->rng) % 100) < s->dup_pct)
        sim_enqueue(s, to, p, n, at + 1000);
}

static uint32_t msg_pattern(uint32_t stream, uint32_t seq, size_t i)
{
    return (uint32_t)((stream * 131u + seq * 7919u + i * 2654435761u) >> 13);
}

static void cb_message(void* ud, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n)
{
    sim_t* s = SIM;
    int me = (int)(intptr_t)ud;
    uint32_t seq;
    size_t i;
    s->log_len[me][s->log_n[me]++ & 31] = n;
    if (n < 8) {
        s->corrupt++;
        return;
    }
    seq = rtc_rd32(p);
    if (rtc_rd32(p + 4) != (uint32_t)n) {
        s->corrupt++;
        return;
    }
    for (i = 8; i < n; i++)
        if (p[i] != (uint8_t)msg_pattern(stream, seq, i)) {
            s->corrupt++;
            return;
        }
    if (ppid == 51 && stream < 64) {
        if (seq < s->next_seq[me][stream])
            s->order_bad++;
        s->next_seq[me][stream] = seq + 1;
    }
    s->rx_count[me]++;
    s->rx_len[me] += n;
}

static void cb_state(void* ud, int st)
{
    int me = (int)(intptr_t)ud;
    if (st == RTC_SCTP_ESTABLISHED)
        SIM->established[me] = 1;
    if (st == RTC_SCTP_ABORTED)
        SIM->aborted[me] = 1;
}

static void cb_reset_in(void* ud, uint16_t stream)
{
    int me = (int)(intptr_t)ud;
    if (stream < 64) {
        if (!SIM->reset_req[me][stream]++)
            rtc_sctp_reset_stream(SIM->ep[me], stream);
        SIM->reset_in[me][stream]++;
    }
}

static void cb_reset_out(void* ud, uint16_t stream)
{
    int me = (int)(intptr_t)ud;
    if (stream < 64)
        SIM->reset_out[me][stream]++;
}

static void cb_sent(void* ud, uint16_t stream, size_t n)
{
    (void)stream;
    SIM->sent_done[(int)(intptr_t)ud] += n;
}

static void sim_run_until(sim_t* s, uint64_t until)
{
    while (s->now < until) {
        int i, best = -1;
        uint64_t next = until;
        for (i = 0; i < s->nq; i++)
            if (s->q[i].at < next) {
                next = s->q[i].at;
                best = i;
            }
        for (i = 0; i < 2; i++) {
            uint64_t t = rtc_sctp_tick(s->ep[i], s->now);
            if (t < next) {
                next = t;
                best = -2 - i;
            }
        }
        if (next > until)
            next = until;
        if (next > s->now)
            s->now = next;
        if (best >= 0) {
            sim_pkt k = s->q[best];
            s->q[best] = s->q[--s->nq];
            rtc_sctp_input(s->ep[k.to], k.p, k.n, s->now);
            free(k.p);
        } else if (best < -1)
            rtc_sctp_tick(s->ep[-2 - best], s->now);
        else if (next == until)
            break;
    }
}

static sim_t* sim_new(uint64_t seed, unsigned loss, unsigned dup, unsigned reorder)
{
    sim_t* s = (sim_t*)calloc(1, sizeof(*s));
    uint8_t sd[32] = { 0 };
    rtc_sctp_cb cb = { cb_packet, cb_message, cb_state, cb_reset_in, cb_reset_out, cb_sent };
    memcpy(sd, &seed, 8);
    rtc_rng_seed(&s->rng, sd);
    s->loss_pct = loss;
    s->dup_pct = dup;
    s->reorder_pct = reorder;
    s->delay = 5000;
    SIM = s;
    s->ep[0] = rtc_sctp_new(&cb, (void*)(intptr_t)0, &s->rng, 5000, 5000, sim_mtu0);
    s->ep[1] = rtc_sctp_new(&cb, (void*)(intptr_t)1, &s->rng, 5000, 5000, 0);
    return s;
}

static void sim_free(sim_t* s)
{
    int i;
    for (i = 0; i < s->nq; i++)
        free(s->q[i].p);
    rtc_sctp_free(s->ep[0]);
    rtc_sctp_free(s->ep[1]);
    free(s);
}

static uint8_t* make_msg(uint32_t stream, uint32_t seq, size_t n)
{
    uint8_t* p = (uint8_t*)malloc(n);
    size_t i;
    rtc_wr32(p, seq);
    rtc_wr32(p + 4, (uint32_t)n);
    for (i = 8; i < n; i++)
        p[i] = (uint8_t)msg_pattern(stream, seq, i);
    return p;
}

static void sctp_case(const char* name, uint64_t seed, unsigned loss, unsigned dup, unsigned reorder, int nmsg, size_t maxsz,
    int unordered, int both_connect)
{
    sim_t* s = sim_new(seed, loss, dup, reorder);
    int i;
    size_t total = 0;
    rtc_sctp_connect(s->ep[0], 0);
    if (both_connect)
        rtc_sctp_connect(s->ep[1], 0);
    sim_run_until(s, 60000000);
    CHECK(s->established[0] && s->established[1], "%s: established", name);
    for (i = 0; i < nmsg; i++) {
        size_t n = 8 + (size_t)(rtc_rng_next(&s->rng) % (maxsz - 7));
        uint16_t stream = (uint16_t)(i % 3);
        uint8_t* p = make_msg(stream, (uint32_t)i, n);
        CHECK(rtc_sctp_send(s->ep[i & 1 ? 1 : 0], stream, unordered ? 53 : 51, p, n, unordered, -1, -1, s->now) == 0, "%s: send", name);
        total += n;
        free(p);
        if ((i & 7) == 7) {
            rtc_sctp_flush(s->ep[0], s->now);
            rtc_sctp_flush(s->ep[1], s->now);
            sim_run_until(s, s->now + 2000);
        }
    }
    rtc_sctp_flush(s->ep[0], s->now);
    rtc_sctp_flush(s->ep[1], s->now);
    sim_run_until(s, s->now + 600000000);
    CHECK(s->rx_count[0] + s->rx_count[1] == (uint32_t)nmsg, "%s: delivered %u of %d", name, s->rx_count[0] + s->rx_count[1], nmsg);
    CHECK(s->rx_len[0] + s->rx_len[1] == total, "%s: bytes", name);
    CHECK(s->corrupt == 0, "%s: %d corrupt", name, s->corrupt);
    CHECK(s->order_bad == 0, "%s: %d out of order", name, s->order_bad);
    CHECK(s->sent_done[0] + s->sent_done[1] == total, "%s: bufferedAmount drained", name);
    CHECK(rtc_sctp_buffered(s->ep[0], 0) == 0 && rtc_sctp_buffered(s->ep[1], 1) == 0, "%s: queued 0", name);
    CHECK(!s->aborted[0] && !s->aborted[1], "%s: not aborted", name);
    sim_free(s);
}

static void test_sctp_stash_slots(void)
{
    sim_t* s = sim_new(21, 10, 5, 30);
    int i;
    uint32_t nfree;
    rtc_sctp_rxc* head;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    CHECK(s->established[0] && s->established[1], "stash slots: established");
    for (i = 0; i < 2000; i++) {
        uint8_t* p = make_msg(0, (uint32_t)i, 900);
        rtc_sctp_send(s->ep[0], 0, 51, p, 900, 0, -1, -1, s->now);
        free(p);
        if ((i & 7) == 7) {
            rtc_sctp_flush(s->ep[0], s->now);
            sim_run_until(s, s->now + 2000);
        }
    }
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 600000000);
    CHECK(s->rx_count[1] == 2000 && s->corrupt == 0 && s->order_bad == 0, "stash slots: lossy run delivered %u", s->rx_count[1]);
    nfree = s->ep[1]->slot_nfree;
    head = nfree ? s->ep[1]->slot_stack[nfree - 1] : NULL;
    CHECK(nfree > 0 && head != NULL, "stash slots: reordered chunks returned %u slots to the cache", nfree);
    CHECK(nfree <= RTC_SCTP_SLOT_CACHE, "stash slots: cache bounded (%u)", nfree);
    CHECK(s->ep[1]->stash_bytes == 0, "stash slots: stash empty after drain");
    s->loss_pct = s->dup_pct = s->reorder_pct = 0;
    s->drop_next = 1;
    {
        uint8_t* a = make_msg(0, 2000, 600);
        uint8_t* b = make_msg(0, 2001, 600);
        uint32_t tsn_b;
        rtc_sctp_send(s->ep[0], 0, 51, a, 600, 0, -1, -1, s->now);
        rtc_sctp_flush(s->ep[0], s->now);
        tsn_b = s->ep[1]->rx_cum + 2;
        rtc_sctp_send(s->ep[0], 0, 51, b, 600, 0, -1, -1, s->now);
        rtc_sctp_flush(s->ep[0], s->now);
        sim_run_until(s, s->now + 6000);
        CHECK(s->ep[1]->stash[tsn_b & (RTC_SCTP_RX_WIN - 1)] == head, "stash slots: out-of-order chunk reuses the cached slot");
        CHECK(s->ep[1]->slot_nfree == nfree - 1, "stash slots: cache shrank by one (%u)", s->ep[1]->slot_nfree);
        sim_run_until(s, s->now + 600000000);
        CHECK(s->rx_count[1] == 2002 && s->order_bad == 0, "stash slots: both delivered after retransmit");
        CHECK(s->ep[1]->slot_nfree == nfree, "stash slots: slot returned to the cache (%u)", s->ep[1]->slot_nfree);
        free(a);
        free(b);
    }
    sim_free(s);
}

static void test_sctp_gap_fill_sack(void)
{
    sim_t* s = sim_new(23, 0, 0, 0);
    uint8_t* a = make_msg(1, 0, 600);
    uint8_t* b = make_msg(1, 1, 600);
    uint64_t t_fill;
    int guard = 0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    CHECK(s->established[0] && s->established[1], "gap fill: established");
    sim_run_until(s, s->now + 1000000);
    s->drop_next = 1;
    rtc_sctp_send(s->ep[0], 1, 51, a, 600, 0, -1, -1, s->now);
    rtc_sctp_flush(s->ep[0], s->now);
    rtc_sctp_send(s->ep[0], 1, 51, b, 600, 0, -1, -1, s->now);
    rtc_sctp_flush(s->ep[0], s->now);
    while (s->rx_count[1] < 2 && guard++ < 100000)
        sim_run_until(s, s->now + 1000);
    CHECK(s->rx_count[1] == 2 && s->order_bad == 0, "gap fill: both delivered in order (%u)", s->rx_count[1]);
    t_fill = s->now;
    sim_run_until(s, t_fill + 3 * s->delay);
    CHECK(s->ep[0]->flight == 0, "gap fill: the SACK for the gap-filling DATA goes out at once (flight %u after %llu us)", s->ep[0]->flight, (unsigned long long)(3 * s->delay));
    free(a);
    free(b);
    sim_free(s);
}

static void test_sctp_stash_oversize(void)
{
    sim_t* s;
    int i;
    uint32_t big = 0;
    sim_mtu0 = 4000;
    s = sim_new(22, 10, 0, 40);
    sim_mtu0 = 0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    CHECK(s->established[0] && s->established[1], "stash oversize: established");
    for (i = 0; i < 400; i++) {
        uint8_t* p = make_msg(0, (uint32_t)i, 3500);
        rtc_sctp_send(s->ep[0], 0, 51, p, 3500, 0, -1, -1, s->now);
        free(p);
        rtc_sctp_flush(s->ep[0], s->now);
        sim_run_until(s, s->now + 1000);
        if (s->ep[1]->stash) {
            uint32_t k;
            for (k = 0; k < RTC_SCTP_RX_WIN; k++) {
                rtc_sctp_rxc* x = s->ep[1]->stash[k];
                if (x && x->len > RTC_SCTP_SLOT && x->len < 100000)
                    big++;
            }
        }
    }
    sim_run_until(s, s->now + 600000000);
    CHECK(big > 0, "stash oversize: chunks above the slot size were stashed (%u)", big);
    CHECK(s->rx_count[1] == 400 && s->corrupt == 0 && s->order_bad == 0, "stash oversize: delivered %u intact", s->rx_count[1]);
    {
        uint32_t cached_big = 0, n;
        for (n = 0; n < s->ep[1]->slot_nfree; n++)
            if (s->ep[1]->slot_stack[n]->len > RTC_SCTP_SLOT)
                cached_big++;
        CHECK(cached_big == 0 && n == s->ep[1]->slot_nfree, "stash oversize: oversize chunks are freed, not cached (%u of %u)", cached_big, n);
    }
    sim_free(s);
}

static void test_sctp_partial(void)
{
    sim_t* s = sim_new(77, 30, 0, 10);
    int i, nmsg = 400;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    CHECK(s->established[0] && s->established[1], "partial: established");
    for (i = 0; i < nmsg; i++) {
        uint8_t* p = make_msg(1, (uint32_t)i, 200 + (size_t)(i % 3000));
        CHECK(rtc_sctp_send(s->ep[0], 1, 51, p, 200 + (size_t)(i % 3000), 0, 0, -1, s->now) == 0, "partial send");
        free(p);
    }
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 600000000);
    for (i = 0; i < 60 && s->ep[0]->ack_base != s->ep[0]->next_tsn; i++)
        sim_run_until(s, s->now + 60000000);
    CHECK(s->rx_count[1] < (uint32_t)nmsg && s->rx_count[1] > 0, "partial: some dropped (%u)", s->rx_count[1]);
    CHECK(s->corrupt == 0 && s->order_bad == 0, "partial: intact and ordered");
    CHECK(s->ep[0]->ack_base == s->ep[0]->next_tsn, "partial: sender fully acked via FORWARD-TSN");
    CHECK(s->ep[0]->om_head == s->ep[0]->om_tail, "partial: send queue freed");
    s->loss_pct = 0;
    {
        uint8_t* p = make_msg(1, 100000, 100);
        rtc_sctp_send(s->ep[0], 1, 51, p, 100, 0, 0, -1, s->now);
        free(p);
        rtc_sctp_flush(s->ep[0], s->now);
        sim_run_until(s, s->now + 10000000);
        CHECK(s->next_seq[1][1] == 100001, "partial: stream continues after skips");
    }
    sim_free(s);
}

static void test_sctp_lifetime(void)
{
    sim_t* s = sim_new(99, 100, 0, 0);
    int i;
    s->loss_pct = 0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    s->loss_pct = 100;
    for (i = 0; i < 50; i++) {
        uint8_t* p = make_msg(2, (uint32_t)i, 1000);
        rtc_sctp_send(s->ep[0], 2, 53, p, 1000, 1, -1, 100, s->now);
        free(p);
    }
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 3000000);
    s->loss_pct = 0;
    sim_run_until(s, s->now + 60000000);
    CHECK(s->rx_count[1] == 0, "lifetime: expired messages not delivered (%u)", s->rx_count[1]);
    CHECK(s->ep[0]->om_head == s->ep[0]->om_tail, "lifetime: queue freed");
    CHECK(s->ep[0]->ack_base == s->ep[0]->next_tsn, "lifetime: all TSNs resolved");
    sim_free(s);
}

static void test_sctp_reset(void)
{
    sim_t* s = sim_new(5, 10, 5, 10);
    uint8_t* p;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    p = make_msg(4, 0, 500);
    rtc_sctp_send(s->ep[0], 4, 51, p, 500, 0, -1, -1, s->now);
    free(p);
    rtc_sctp_reset_stream(s->ep[0], 4);
    s->reset_req[0][4] = 1;
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 120000000);
    CHECK(s->rx_count[1] == 1, "reset: data before reset delivered");
    CHECK(s->reset_in[1][4] == 1, "reset: peer saw incoming reset");
    CHECK(s->reset_out[0][4] == 1, "reset: our outgoing reset done");
    CHECK(s->reset_in[0][4] == 1 && s->reset_out[1][4] == 1, "reset: peer reset its outgoing in turn");
    p = make_msg(4, 0, 64);
    rtc_sctp_send(s->ep[0], 4, 51, p, 64, 0, -1, -1, s->now);
    free(p);
    s->next_seq[1][4] = 0;
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 60000000);
    CHECK(s->rx_count[1] == 2 && s->order_bad == 0, "reset: stream reusable with SSN 0");
    sim_free(s);
}

static void sctp_seal(uint8_t* pkt, size_t n)
{
    uint32_t crc;
    memset(pkt + 8, 0, 4);
    crc = dyn_crc32c(pkt, n);
    pkt[8] = (uint8_t)crc;
    pkt[9] = (uint8_t)(crc >> 8);
    pkt[10] = (uint8_t)(crc >> 16);
    pkt[11] = (uint8_t)(crc >> 24);
}

static void test_rng_chacha20(void)
{
    static const uint8_t head[16] = { 0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4 };
    static const uint8_t tail[16] = { 0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e };
    uint32_t key[8], nonce[3] = { 0x09000000u, 0x4a000000u, 0 };
    uint8_t out[64], sd[32], a[64], b[64];
    rtc_rng r1, r2;
    int i, same = 0;
    for (i = 0; i < 8; i++)
        key[i] = (uint32_t)(4 * i) | ((uint32_t)(4 * i + 1) << 8) | ((uint32_t)(4 * i + 2) << 16) | ((uint32_t)(4 * i + 3) << 24);
    rtc_chacha20_block(key, 1, nonce, out);
    CHECK(!memcmp(out, head, 16) && !memcmp(out + 48, tail, 16), "ChaCha20 block matches RFC 8439 2.3.2");
    for (i = 0; i < 32; i++)
        sd[i] = (uint8_t)i;
    rtc_rng_seed(&r1, sd);
    rtc_rng_seed(&r2, sd);
    rtc_rng_bytes(&r1, a, 64);
    rtc_rng_bytes(&r2, b, 13);
    rtc_rng_bytes(&r2, b + 13, 51);
    CHECK(!memcmp(a, b, 64), "rng output is independent of read sizes");
    rtc_rng_bytes(&r1, b, 64);
    for (i = 0; i < 64; i++)
        same += a[i] == b[i];
    CHECK(same < 8, "consecutive rng blocks differ (%d equal bytes)", same);
    CHECK(r1.pos >= 32, "the key half of each block is never handed out");
}

static void sctp_inject_data(sim_t* s, int to, uint32_t tsn, uint16_t sid, uint8_t flags, uint32_t ppid, size_t n)
{
    uint8_t pkt[1200];
    size_t len = 12 + 16 + n, pad = (len + 3) & ~(size_t)3;
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_DATA;
    pkt[13] = flags;
    rtc_wr16(pkt + 14, (uint16_t)(16 + n));
    rtc_wr32(pkt + 16, tsn);
    rtc_wr16(pkt + 20, sid);
    rtc_wr16(pkt + 22, 0);
    rtc_wr32(pkt + 24, ppid);
    memset(pkt + 28, 'x', n);
    sctp_seal(pkt, pad);
    rtc_sctp_input(s->ep[to], pkt, pad, s->now);
}

static int sctp_q_has_error(sim_t* s, int to, uint16_t cause, uint16_t sid)
{
    int i;
    for (i = 0; i < s->nq; i++) {
        const uint8_t* p = s->q[i].p;
        size_t off = 12;
        if (s->q[i].to != to)
            continue;
        while (off + 4 <= s->q[i].n) {
            uint16_t l = rtc_rd16(p + off + 2);
            if (l < 4)
                break;
            if (p[off] == RTC_SCTP_ERROR && l >= 12 && rtc_rd16(p + off + 4) == cause && rtc_rd16(p + off + 8) == sid)
                return 1;
            off += ((size_t)l + 3) & ~(size_t)3;
        }
    }
    return 0;
}

static void test_sctp_streams(void)
{
    int side;
    for (side = 0; side < 2; side++) {
        sim_t* s = sim_new(41 + side, 0, 0, 0);
        uint32_t cum, before;
        uint8_t m[8] = { 0 };
        s->ep[1]->local_streams = 16;
        rtc_sctp_connect(s->ep[side], 0);
        sim_run_until(s, 60000000);
        CHECK(s->established[0] && s->established[1], "streams %d: established", side);
        CHECK(s->ep[0]->n_in == 16 && s->ep[0]->n_out == 16 && rtc_sctp_max_channels(s->ep[0]) == 16,
            "streams %d: the peer's OS/MIS 16 bound our streams (in %u out %u)", side, s->ep[0]->n_in, s->ep[0]->n_out);
        CHECK(rtc_sctp_max_channels(s->ep[1]) == 16, "streams %d: the small side reports 16", side);
        CHECK(rtc_sctp_send(s->ep[0], 20, 51, m, 8, 0, -1, -1, s->now) == -4, "streams %d: send on sid 20 >= peer MIS is refused", side);
        CHECK(rtc_sctp_send(s->ep[0], 15, 51, m, 8, 0, -1, -1, s->now) == 0, "streams %d: send on sid 15 is accepted", side);
        rtc_sctp_flush(s->ep[0], s->now);
        sim_run_until(s, s->now + 1000000);
        before = (uint32_t)s->ep[1]->st.msgs_rx;
        cum = s->ep[1]->rx_cum;
        sctp_inject_data(s, 1, cum + 1, 20, 0x03, 51, 8);
        CHECK(s->ep[1]->rx_cum == cum + 1, "streams %d: DATA on sid 20 is still acknowledged", side);
        CHECK(s->ep[1]->st.msgs_rx == before, "streams %d: DATA on sid 20 is discarded", side);
        CHECK(sctp_q_has_error(s, 0, 1, 20), "streams %d: ERROR Invalid Stream Identifier 20 is sent", side);
        sctp_inject_data(s, 1, cum + 3, 21, 0x03, 51, 8);
        sctp_inject_data(s, 1, cum + 2, 3, 0x03, 51, 8);
        CHECK(s->ep[1]->rx_cum == cum + 3 && s->ep[1]->st.msgs_rx == before + 1, "streams %d: out-of-order invalid sid is acked as a hole filler (cum +%u)", side, s->ep[1]->rx_cum - cum);
        sim_free(s);
    }
    {
        sim_t* s = sim_new(43, 0, 0, 0);
        uint32_t cum, i, pages;
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, 60000000);
        CHECK(rtc_sctp_max_channels(s->ep[0]) == 65535 && rtc_sctp_max_channels(s->ep[1]) == 65535, "streams: 65535 negotiated by default (RFC 8831 6.2)");
        pages = s->ep[1]->nspages;
        cum = s->ep[1]->rx_cum;
        for (i = 0; i < 2000; i++)
            sctp_inject_data(s, 1, cum + 1 + i, (uint16_t)(i * 32 + 1), i & 1 ? 0x07 : 0x03, 51, 8);
        sctp_inject_data(s, 1, cum + 2001, 65534, 0x03, 51, 8);
        CHECK(s->ep[1]->rx_cum == cum + 2001 && s->ep[1]->st.msgs_rx == 2001, "streams: 2001 messages on sparse sids delivered (%u)", (unsigned)s->ep[1]->st.msgs_rx);
        CHECK(s->ep[1]->nspages == pages && s->ep[1]->npages == 0, "streams: wire sids allocate no stream state (pages %u dir %u)", s->ep[1]->nspages, s->ep[1]->npages);
        for (i = 0; i < 300; i++)
            rtc_sctp_reset_stream(s->ep[0], (uint16_t)(i * 2));
        CHECK(s->ep[0]->rw_n == 300, "streams: 300 resets listed");
        rtc_sctp_flush(s->ep[0], s->now);
        CHECK(s->ep[0]->reconf_n == 256 && s->ep[0]->rw_n == 44, "streams: one request carries 256, 44 stay listed (%u/%u)", s->ep[0]->reconf_n, s->ep[0]->rw_n);
        sim_run_until(s, s->now + 30000000);
        CHECK(s->ep[0]->rw_n == 0 && !s->ep[0]->reconf_inflight, "streams: the rest go in the next request (%u left)", s->ep[0]->rw_n);
        CHECK(s->ep[0]->nspages <= 20, "streams: local resets use pages of 32 (%u)", s->ep[0]->nspages);
        sim_free(s);
    }
}

static int sctp_q_has_abort_cause(sim_t* s, int to, uint16_t cause)
{
    int i;
    for (i = 0; i < s->nq; i++)
        if (s->q[i].to == to && s->q[i].n >= 20 && s->q[i].p[12] == RTC_SCTP_ABORT && rtc_rd16(s->q[i].p + 16) == cause)
            return 1;
    return 0;
}

static void test_sctp_empty_and_slots(void)
{
    sim_t* s = sim_new(47, 0, 0, 0);
    uint32_t cum, i;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    cum = s->ep[1]->rx_cum;
    for (i = 0; i < 10; i++)
        sctp_inject_data(s, 1, cum + 2 + i, 1, 0x03, 50, 1);
    CHECK(s->ep[1]->stash_bytes == 10 * RTC_SCTP_SLOT, "slots: 10 stashed 1-byte chunks cost 10 slots (%u)", s->ep[1]->stash_bytes);
    sctp_inject_data(s, 1, cum + 1, 1, 0x03, 50, 1);
    CHECK(s->ep[1]->stash_bytes == 0 && s->ep[1]->rx_cum == cum + 11, "slots: drained back to 0 (%u)", s->ep[1]->stash_bytes);
    sctp_inject_data(s, 1, cum + 12, 1, 0x03, 50, 0);
    CHECK(s->ep[1]->state == RTC_SCTP_ABORTED && s->aborted[1], "empty DATA aborts the association (RFC 9260 6.2)");
    CHECK(sctp_q_has_abort_cause(s, 0, 9), "empty DATA: ABORT carries cause 9 No User Data");
    sim_free(s);
}

static void test_sctp_gap_blocks(void)
{
    sim_t* s = sim_new(49, 0, 0, 0);
    uint8_t m[16] = { 0 }, pkt[12 + 16 + 4 * 600];
    uint32_t base, i, fl;
    uint64_t t0, t1;
    struct timespec ts;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    s->loss_pct = 100;
    s->ep[0]->cwnd = 1u << 30;
    s->ep[0]->peer_rwnd = 1u << 30;
    for (i = 0; i < 4000; i++)
        rtc_sctp_send(s->ep[0], 1, 50, m, sizeof(m), 0, -1, -1, s->now);
    rtc_sctp_flush(s->ep[0], s->now);
    base = s->ep[0]->ack_base;
    CHECK(s->ep[0]->next_tsn - base >= 3000, "gap blocks: %u TSNs outstanding", s->ep[0]->next_tsn - base);
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[0]->my_tag);
    pkt[12] = RTC_SCTP_SACK;
    rtc_wr16(pkt + 14, 16 + 4 * 600);
    rtc_wr32(pkt + 16, base - 1);
    rtc_wr32(pkt + 20, 1u << 20);
    rtc_wr16(pkt + 24, 600);
    rtc_wr16(pkt + 28, 20);
    rtc_wr16(pkt + 30, 30);
    rtc_wr16(pkt + 32, 5);
    rtc_wr16(pkt + 34, 10);
    for (i = 2; i < 600; i++) {
        rtc_wr16(pkt + 28 + 4 * i, 2);
        rtc_wr16(pkt + 30 + 4 * i, 3000);
    }
    sctp_seal(pkt, sizeof(pkt));
    fl = s->ep[0]->flight;
    rtc_sctp_input(s->ep[0], pkt, sizeof(pkt), s->now);
    CHECK(fl - s->ep[0]->flight == 11 * 16, "gap blocks: only the first increasing block counts (%u bytes gap-acked)", fl - s->ep[0]->flight);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    for (i = 0; i < 20; i++)
        rtc_sctp_input(s->ep[0], pkt, sizeof(pkt), s->now);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    CHECK((t1 - t0) / 20 < 1000000u, "gap blocks: a 600-block overlapping SACK costs %llu ns (< 1 ms)", (unsigned long long)((t1 - t0) / 20));
    sim_free(s);
}

static void sctp_inject_fwd(sim_t* s, int to, uint32_t nc)
{
    uint8_t pkt[20];
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_FORWARD_TSN;
    rtc_wr16(pkt + 14, 8);
    rtc_wr32(pkt + 16, nc);
    sctp_seal(pkt, sizeof(pkt));
    rtc_sctp_input(s->ep[to], pkt, sizeof(pkt), s->now);
}

static void test_sctp_fwd_far(void)
{
    sim_t* s = sim_new(53, 0, 0, 0);
    uint32_t cum;
    uint64_t rx0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    cum = s->ep[1]->rx_cum;
    sctp_inject_data(s, 1, cum + 3, 1, 0x07, 50, 4);
    sctp_inject_data(s, 1, cum + 4000, 1, 0x02, 50, 4);
    rx0 = s->ep[1]->st.msgs_rx;
    sctp_inject_fwd(s, 1, cum + 5000);
    CHECK(s->ep[1]->rx_cum == cum + 5000, "fwd-tsn past the 4096 window jumps rx_cum (RFC 3758 3.6) (+%u)", s->ep[1]->rx_cum - cum);
    CHECK(s->ep[1]->stash_bytes == 0, "fwd-tsn past the window empties the stash (%u)", s->ep[1]->stash_bytes);
    sctp_inject_data(s, 1, cum + 5001, 1, 0x07, 50, 4);
    sctp_inject_data(s, 1, cum + 5002, 1, 0x03, 50, 4);
    CHECK(s->ep[1]->rx_cum == cum + 5002 && s->ep[1]->st.msgs_rx == rx0 + 2, "fwd-tsn far: DATA after it is delivered (%llu)", (unsigned long long)(s->ep[1]->st.msgs_rx - rx0));
    sim_free(s);
}

static void test_sctp_early_delivery(void)
{
    sim_t* s = sim_new(59, 0, 0, 0);
    uint32_t cum, n0;
    uint8_t m[8] = { 0 };
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    rtc_sctp_send(s->ep[1], 2, 50, m, 8, 0, -1, -1, s->now);
    rtc_sctp_flush(s->ep[1], s->now);
    sim_run_until(s, s->now + 1000000);
    cum = s->ep[1]->rx_cum;
    n0 = s->log_n[1];
    sctp_inject_data(s, 1, cum + 2, 1, 0x06, 50, 100);
    sctp_inject_data(s, 1, cum + 3, 1, 0x05, 50, 101);
    CHECK(s->log_n[1] == n0 + 1 && s->log_len[1][n0 & 31] == 201, "early: a complete unordered fragmented message is delivered past a hole (RFC 9260 6.6)");
    {
        uint8_t pkt[64];
        uint32_t tsn[4] = { cum + 6, cum + 7, cum + 5, cum + 4 }, ssn[4] = { 2, 2, 1, 0 }, len[4] = { 12, 13, 11, 10 }, fl[4] = { 0x02, 0x01, 0x03, 0x03 };
        int k;
        for (k = 0; k < 4; k++) {
            size_t pl = 12 + 16 + len[k], pad = (pl + 3) & ~(size_t)3;
            memset(pkt, 0, sizeof(pkt));
            rtc_wr16(pkt, 5000);
            rtc_wr16(pkt + 2, 5000);
            rtc_wr32(pkt + 4, s->ep[1]->my_tag);
            pkt[12] = RTC_SCTP_DATA;
            pkt[13] = (uint8_t)fl[k];
            rtc_wr16(pkt + 14, (uint16_t)(16 + len[k]));
            rtc_wr32(pkt + 16, tsn[k]);
            rtc_wr16(pkt + 20, 2);
            rtc_wr16(pkt + 22, (uint16_t)ssn[k]);
            rtc_wr32(pkt + 24, 50);
            sctp_seal(pkt, pad);
            rtc_sctp_input(s->ep[1], pkt, pad, s->now);
            if (k == 1)
                CHECK(s->log_n[1] == n0 + 1, "early: ordered ssn 2 waits for ssn 0 and 1");
        }
    }
    CHECK(s->log_n[1] == n0 + 4, "early: ordered stream 2 delivered past the hole at cum+1 (%u)", s->log_n[1] - n0);
    CHECK(s->log_len[1][(n0 + 1) & 31] == 10 && s->log_len[1][(n0 + 2) & 31] == 11 && s->log_len[1][(n0 + 3) & 31] == 25,
        "early: in SSN order 0,1,2 (%zu %zu %zu)", s->log_len[1][(n0 + 1) & 31], s->log_len[1][(n0 + 2) & 31], s->log_len[1][(n0 + 3) & 31]);
    sctp_inject_data(s, 1, cum + 1, 3, 0x03, 50, 9);
    CHECK(s->ep[1]->rx_cum == cum + 7 && s->log_n[1] == n0 + 5 && s->ep[1]->stash_bytes == 0, "early: the hole fills, nothing is delivered twice (%u)", s->log_n[1] - n0);
    sim_free(s);
}

static void sctp_inject_chunk(sim_t* s, int to, uint8_t type, uint32_t val, int with_val)
{
    uint8_t pkt[20];
    size_t n = with_val ? 20 : 16;
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = type;
    rtc_wr16(pkt + 14, (uint16_t)(n - 12));
    if (with_val)
        rtc_wr32(pkt + 16, val);
    sctp_seal(pkt, n);
    rtc_sctp_input(s->ep[to], pkt, n, s->now);
}

static void test_sctp_shrink(void)
{
    sim_t* s = sim_new(73, 0, 0, 0);
    uint8_t* big = make_msg(1, 0, 512u << 10);
    uint8_t* mid = make_msg(1, 1, 60000);
    uint32_t i, peak;
    uint64_t base;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 5000000);
    CHECK(rtc_sctp_send(s->ep[0], 1, 53, big, 512u << 10, 0, -1, -1, s->now) == 0, "shrink: queue a 512 KiB message");
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 60000000);
    CHECK(s->rx_count[1] == 1 && s->rx_len[1] == (512u << 10), "shrink: the 512 KiB message arrived (%u, %zu)", s->rx_count[1], s->rx_len[1]);
    CHECK(s->ep[1]->asm_cap <= 65536u, "shrink: the reassembly buffer above 64 KiB is released after delivery (%u)", s->ep[1]->asm_cap);
    CHECK(rtc_sctp_send(s->ep[0], 1, 53, mid, 60000, 0, -1, -1, s->now) == 0, "shrink: queue a 60 kB message");
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 20000000);
    CHECK(s->rx_count[1] == 2 && s->ep[1]->asm_cap > 0 && s->ep[1]->asm_cap <= 65536u, "shrink: a buffer up to 64 KiB is kept for reuse (%u)", s->ep[1]->asm_cap);
    base = s->ep[1]->st.msgs_rx;
    for (i = 0; i < 5000; i++)
        rtc_sctp_send(s->ep[0], 2, 51, (const uint8_t*)"0123456789abcdef", 16, 0, -1, -1, s->now);
    peak = s->ep[0]->om_cap;
    CHECK(peak >= 4096, "shrink: 5000 queued messages grow the send ring (%u)", peak);
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 600000000);
    CHECK(s->ep[1]->st.msgs_rx - base == 5000, "shrink: all 5000 delivered (%llu)", (unsigned long long)(s->ep[1]->st.msgs_rx - base));
    CHECK(s->ep[0]->om_head == s->ep[0]->om_tail && s->ep[0]->om_cap <= 1024u, "shrink: the drained send ring shrinks (%u -> %u)", peak, s->ep[0]->om_cap);
    CHECK(rtc_sctp_send(s->ep[0], 2, 51, (const uint8_t*)"after", 5, 0, -1, -1, s->now) == 0, "shrink: sending after the shrink works");
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 5000000);
    CHECK(s->ep[1]->st.msgs_rx - base == 5001, "shrink: delivered after the shrink (%llu)", (unsigned long long)(s->ep[1]->st.msgs_rx - base));
    free(big);
    free(mid);
    sim_free(s);
}

static void test_sctp_heartbeat(void)
{
    sim_t* s = sim_new(71, 0, 0, 0);
    uint64_t t0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 5000000);
    CHECK(s->established[0] && s->established[1], "heartbeat: established");
    t0 = s->now;
    sim_run_until(s, t0 + 600000000ull);
    CHECK(s->ep[0]->st.hb_tx >= 15 && s->ep[1]->st.hb_tx >= 15, "heartbeat: an idle association sends HEARTBEAT every RTO + 30 s (%llu, %llu)",
        (unsigned long long)s->ep[0]->st.hb_tx, (unsigned long long)s->ep[1]->st.hb_tx);
    CHECK(s->ep[0]->st.hb_ack_rx == s->ep[0]->st.hb_tx && s->ep[1]->st.hb_ack_rx == s->ep[1]->st.hb_tx, "heartbeat: every HEARTBEAT is acked");
    CHECK(s->ep[0]->state == RTC_SCTP_ESTABLISHED && s->ep[1]->state == RTC_SCTP_ESTABLISHED && !s->aborted[0] && !s->aborted[1],
        "heartbeat: an idle reachable peer stays established for 10 minutes");
    CHECK(s->ep[0]->hb_us > s->now && s->ep[0]->hb_us - s->now <= s->ep[0]->rto_us + RTC_SCTP_HB_INTERVAL_US, "heartbeat: next one is armed");
    {
        uint8_t* p = make_msg(1, 0, 100);
        uint64_t before = s->ep[0]->st.hb_tx;
        int i;
        for (i = 0; i < 40; i++) {
            rtc_sctp_send(s->ep[0], 1, 51, p, 100, 0, -1, -1, s->now);
            rtc_sctp_flush(s->ep[0], s->now);
            sim_run_until(s, s->now + 10000000);
        }
        free(p);
        CHECK(s->ep[0]->st.hb_tx - before <= 1, "heartbeat: none while user data keeps the path busy (%llu)",
            (unsigned long long)(s->ep[0]->st.hb_tx - before));
    }
    s->loss_pct = 100;
    t0 = s->now;
    while (!s->aborted[0] && s->now - t0 < 3600000000ull)
        sim_run_until(s, s->now + 1000000);
    CHECK(s->now - t0 <= 900000000ull, "heartbeat: an unreachable idle peer is detected within 15 minutes (%llu s)", (unsigned long long)((s->now - t0) / 1000000));
    printf("heartbeat: idle dead-peer detection took %llu s\n", (unsigned long long)((s->now - t0) / 1000000));
    sim_run_until(s, s->now + 3600000000ull);
    CHECK(s->aborted[0] && s->aborted[1], "heartbeat: an unreachable idle peer aborts the association (%d %d)", s->aborted[0], s->aborted[1]);
    CHECK(s->ep[0]->st.hb_tx >= RTC_SCTP_ASSOC_MAX_RTX, "heartbeat: after Association.Max.Retrans unacked heartbeats");
    sim_free(s);
    s = sim_new(72, 0, 0, 0);
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 5000000);
    s->loss_pct = 100;
    sim_run_until(s, s->now + 200000000ull);
    CHECK(s->ep[0]->assoc_errors >= 2 && !s->aborted[0], "heartbeat: missed heartbeats count as association errors (%u)", s->ep[0]->assoc_errors);
    s->loss_pct = 0;
    sim_run_until(s, s->now + 400000000ull);
    CHECK(!s->aborted[0] && !s->aborted[1] && s->ep[0]->assoc_errors == 0, "heartbeat: an acked HEARTBEAT clears the error count (%u)",
        s->ep[0]->assoc_errors);
    CHECK(s->ep[0]->rto_us <= 3000000u, "heartbeat: the HEARTBEAT-ACK RTT sample brings the doubled RTO back (%u us)", (unsigned)s->ep[0]->rto_us);
    sctp_inject_chunk(s, 0, RTC_SCTP_HEARTBEAT_ACK, 0, 0);
    CHECK(s->ep[0]->state == RTC_SCTP_ESTABLISHED, "heartbeat: a stray HEARTBEAT-ACK is ignored");
    sim_free(s);
}

static void sctp_inject_ssn(sim_t* s, int to, uint32_t tsn, uint16_t sid, uint16_t ssn, uint8_t flags, size_t n)
{
    uint8_t pkt[256];
    size_t len = 12 + 16 + n, pad = (len + 3) & ~(size_t)3;
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_DATA;
    pkt[13] = flags;
    rtc_wr16(pkt + 14, (uint16_t)(16 + n));
    rtc_wr32(pkt + 16, tsn);
    rtc_wr16(pkt + 20, sid);
    rtc_wr16(pkt + 22, ssn);
    rtc_wr32(pkt + 24, 53);
    memset(pkt + 28, 'y', n);
    sctp_seal(pkt, pad);
    rtc_sctp_input(s->ep[to], pkt, pad, s->now);
}

static void sctp_inject_fwd_ssn(sim_t* s, int to, uint32_t nc, uint16_t sid, uint16_t ssn)
{
    uint8_t pkt[24];
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_FORWARD_TSN;
    rtc_wr16(pkt + 14, 12);
    rtc_wr32(pkt + 16, nc);
    rtc_wr16(pkt + 20, sid);
    rtc_wr16(pkt + 22, ssn);
    sctp_seal(pkt, sizeof(pkt));
    rtc_sctp_input(s->ep[to], pkt, sizeof(pkt), s->now);
}

static void test_sctp_ssn_sync(void)
{
    sim_t* s = sim_new(91, 0, 0, 0);
    uint32_t cum, n0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    cum = s->ep[1]->rx_cum;
    n0 = s->log_n[1];
    sctp_inject_ssn(s, 1, cum + 1, 1, 39999, 0x03, 8);
    sctp_inject_ssn(s, 1, cum + 2, 2, 0, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 2, "ssn sync: ordered messages on receive-only streams are delivered");
    rtc_sctp_send(s->ep[1], 3, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
    sctp_inject_ssn(s, 1, cum + 3, 1, 40000, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 3, "ssn sync: a page created later by a local send on sid 3 does not make SSN 40000 on sid 1 stale (R2b-1)");
    sctp_inject_ssn(s, 1, cum + 4, 1, 40000, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 3, "ssn sync: once synced, a repeated SSN is stale");
    sctp_inject_ssn(s, 1, cum + 6, 2, 0, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 3, "ssn sync: a duplicate SSN 0 past a hole on a stream whose SSN is unknown is not delivered early");
    sctp_inject_ssn(s, 1, cum + 5, 2, 1, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 4 && s->ep[1]->rx_cum == cum + 6, "ssn sync: SSN 1 fills the hole and syncs the stream; the duplicate SSN 0 is then stale (%u)", s->log_n[1] - n0);
    sctp_inject_ssn(s, 1, cum + 7, 6, 7, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 5, "ssn sync: the first ordered message on another unsynced stream of the page is delivered");
    sctp_inject_fwd_ssn(s, 1, cum + 8, 4, 50000);
    sctp_inject_ssn(s, 1, cum + 10, 4, 50001, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 6, "ssn sync: FWD-TSN before the first delivery syncs sid 4, so SSN 50001 is delivered early past the hole (%u)", s->log_n[1] - n0);
    sctp_inject_ssn(s, 1, cum + 9, 4, 49999, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 6 && s->ep[1]->rx_cum == cum + 10, "ssn sync: 49999 fills the hole but is stale (%u)", s->log_n[1] - n0);
    sim_free(s);
}

static void test_sctp_early_linear(void)
{
    int dir;
    for (dir = 0; dir < 2; dir++) {
        sim_t* s = sim_new(93 + dir, 0, 0, 0);
        uint32_t cum, n0, k, N = 816;
        uint64_t w0;
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, 60000000);
        cum = s->ep[1]->rx_cum;
        n0 = s->log_n[1];
        w0 = s->ep[1]->st.early_steps;
        for (k = 0; k < N; k++) {
            uint32_t i = dir ? N - 1 - k : k;
            sctp_inject_ssn(s, 1, cum + 3 + i, 6, 0, 0x04, 16);
        }
        sctp_inject_ssn(s, 1, cum + 2, 6, 0, 0x06, 16);
        sctp_inject_ssn(s, 1, cum + 3 + N, 6, 0, 0x05, 16);
        CHECK(s->log_n[1] == n0 + 1 && s->log_len[1][n0 & 31] == (N + 2) * 16, "early linear %s: the %u-fragment message is delivered past the hole", dir ? "descending" : "ascending", N + 2);
        CHECK(s->ep[1]->st.early_steps - w0 <= 4 * (N + 2), "early linear %s: work is O(n): %llu steps for %u fragments (R2b-3)", dir ? "descending" : "ascending",
            (unsigned long long)(s->ep[1]->st.early_steps - w0), N + 2);
        sim_free(s);
    }
    {
        sim_t* s = sim_new(95, 0, 0, 0);
        uint32_t cum, n0, k, N = 800;
        uint64_t w0;
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, 60000000);
        rtc_sctp_send(s->ep[1], 6, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
        cum = s->ep[1]->rx_cum;
        n0 = s->log_n[1];
        sctp_inject_ssn(s, 1, cum + 1, 6, 0, 0x03, 8);
        w0 = s->ep[1]->st.early_steps;
        for (k = 1; k <= N; k++)
            sctp_inject_ssn(s, 1, cum + 3 + k, 6, (uint16_t)(k + 1), 0x03, 8);
        sctp_inject_ssn(s, 1, cum + 3, 6, 1, 0x03, 8);
        CHECK(s->log_n[1] == n0 + 2 + N, "early chain: SSN 1 releases %u stashed ordered messages past the hole (%u)", N, s->log_n[1] - n0);
        CHECK(s->ep[1]->st.early_steps - w0 <= 4 * N, "early chain: the head search is O(n) over the chain: %llu steps (R2b-3)", (unsigned long long)(s->ep[1]->st.early_steps - w0));
        sim_free(s);
    }
    {
        sim_t* s = sim_new(97, 0, 0, 0);
        uint32_t cum, n0, k, NA = 2000, NU = 1996;
        uint64_t w0;
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, 60000000);
        rtc_sctp_send(s->ep[1], 7, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
        cum = s->ep[1]->rx_cum;
        n0 = s->log_n[1];
        sctp_inject_ssn(s, 1, cum + 1, 7, 0, 0x03, 8);
        w0 = s->ep[1]->st.early_steps;
        for (k = 0; k < NU; k++)
            sctp_inject_ssn(s, 1, cum + 2100 + k, 3, 0, 0x07, 8);
        for (k = 0; k < NA; k++)
            sctp_inject_ssn(s, 1, cum + 3 + k, 7, (uint16_t)(k + 1), 0x03, 8);
        CHECK(s->log_n[1] == n0 + 1 + NU + NA, "early tombs: %u unordered and %u ordered messages delivered past the hole (%u)", NU, NA, s->log_n[1] - n0);
        CHECK(s->ep[1]->st.early_steps - w0 <= 4 * (NA + NU), "early tombs: a head search miss does not scan the %u delivered unordered tombs: %llu steps (R2b-3 round 3)", NU,
            (unsigned long long)(s->ep[1]->st.early_steps - w0));
        for (k = 0; k < 40; k++)
            sctp_inject_ssn(s, 1, cum + 2003 + 40 - 1 - k, 7, (uint16_t)(NA + 1 + 40 - 1 - k), 0x03, 8);
        CHECK(s->log_n[1] == n0 + 1 + NU + NA + 40, "early tombs: 40 later messages arriving in reverse are released by the index as their heads arrive (%u)", s->log_n[1] - n0);
        CHECK(s->ep[1]->hix != NULL, "early tombs: the head index exists");
        {
            uint32_t i, live = 0;
            for (i = 0; s->ep[1]->hix && i < RTC_SCTP_HIX; i++)
                live += s->ep[1]->hix[i] != 0;
            CHECK(live == 0, "early tombs: every delivered head left the index (%u live)", live);
        }
        sim_free(s);
    }
    for (int dir = 0; dir < 2; dir++) {
        sim_t* s = sim_new(98 + 10 * dir, 0, 0, 0);
        uint32_t cum, n0, k, st, NS = 8, NM = 50, F = 400, x = 12345 + dir, tsn;
        uint64_t w0;
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, 60000000);
        for (st = 1; st <= NS; st++)
            rtc_sctp_send(s->ep[1], (uint16_t)st, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
        cum = s->ep[1]->rx_cum;
        n0 = s->log_n[1];
        for (st = 1; st <= NS; st++)
            sctp_inject_ssn(s, 1, cum + st, (uint16_t)st, 0, 0x03, 8);
        tsn = cum + NS + 2 + NS;
        for (k = 0; k < F; k++) {
            x = x * 1103515245u + 12345u;
            sctp_inject_ssn(s, 1, tsn++, (uint16_t)(1 + k % NS), (uint16_t)(20000 + (x >> 16) % 40000), 0x03, 8);
        }
        w0 = s->ep[1]->st.early_steps;
        for (st = 1; st <= NS; st++)
            for (k = 0; k < NM; k++) {
                uint32_t m = dir ? k : NM - 1 - k;
                sctp_inject_ssn(s, 1, tsn + (st - 1) * NM + m, (uint16_t)st, (uint16_t)(m + 2), 0x03, 8);
            }
        for (st = 1; st <= NS; st++)
            sctp_inject_ssn(s, 1, cum + NS + 1 + st, (uint16_t)st, 1, 0x03, 8);
        CHECK(s->log_n[1] == n0 + NS * (NM + 2), "early index %s: %u streams release %u stashed heads each, one stream after another, among %u heads far ahead (%u)",
            dir ? "ascending" : "reverse", NS, NM, F, s->log_n[1] - n0);
        CHECK(s->ep[1]->st.early_steps - w0 <= 8 * (NS * NM + F), "early index %s: lookups stay O(1) with a crowded index: %llu steps (R2b-3 round 3)", dir ? "ascending" : "reverse",
            (unsigned long long)(s->ep[1]->st.early_steps - w0));
        sim_free(s);
    }
}

static void test_sctp_early_bounds(void)
{
    sim_t* s = sim_new(99, 0, 0, 0);
    uint32_t cum, n0, k, N = 300;
    uint64_t w0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    rtc_sctp_send(s->ep[1], 8, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
    cum = s->ep[1]->rx_cum;
    n0 = s->log_n[1];
    sctp_inject_ssn(s, 1, cum + 2, 9, 0, 0x06, 16);
    sctp_inject_ssn(s, 1, cum + 4, 9, 0, 0x04, 16);
    sctp_inject_ssn(s, 1, cum + 3, 9, 0, 0x05, 16);
    CHECK(s->log_n[1] == n0 + 1 && s->log_len[1][n0 & 31] == 32, "early bounds: a message ending in E is complete even when a middle fragment of the next one follows (%u)", s->log_n[1] - n0);
    sctp_inject_ssn(s, 1, cum + 5, 8, 7, 0x00, 16);
    sctp_inject_ssn(s, 1, cum + 6, 8, 0, 0x03, 16);
    CHECK(s->log_n[1] == n0 + 2 && s->log_len[1][(n0 + 1) & 31] == 16, "early bounds: a B|E message right after a middle fragment is delivered on its own (%u)", s->log_n[1] - n0);
    w0 = s->ep[1]->st.early_steps;
    for (k = 0; k < N; k++) {
        uint32_t i = N - 1 - k;
        sctp_inject_ssn(s, 1, cum + 10 + 2 * i + 1, 8, 0, 0x05, 16);
        sctp_inject_ssn(s, 1, cum + 10 + 2 * i, 8, 0, 0x06, 16);
    }
    CHECK(s->log_n[1] == n0 + 2 + N, "early bounds: %u unordered two-fragment messages are delivered past the hole (%u)", N, s->log_n[1] - n0);
    CHECK(s->ep[1]->st.early_steps - w0 <= 4 * 2 * N, "early bounds: an unordered delivery does not rescan the window: %llu steps (R2b-3)", (unsigned long long)(s->ep[1]->st.early_steps - w0));
    sim_free(s);
    s = sim_new(101, 0, 0, 0);
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    cum = s->ep[1]->rx_cum;
    n0 = s->log_n[1];
    sctp_inject_fwd_ssn(s, 1, cum + 1, 40, 40000);
    rtc_sctp_send(s->ep[1], 41, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
    sctp_inject_ssn(s, 1, cum + 2, 40, 40001, 0x03, 8);
    CHECK(s->log_n[1] == n0 + 1, "ssn sync: a FWD-TSN skip on a stream whose page did not exist yet leaves that page unsynced, so SSN 40001 is not stale (R2b-1)");
    sim_free(s);
}

static void sctp_inject_init(sim_t* s, int to)
{
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    pkt[12] = RTC_SCTP_INIT;
    rtc_wr16(pkt + 14, 20);
    rtc_wr32(pkt + 16, 0x1234567u);
    rtc_wr32(pkt + 20, 1u << 20);
    rtc_wr16(pkt + 24, 1024);
    rtc_wr16(pkt + 26, 1024);
    rtc_wr32(pkt + 28, 77);
    sctp_seal(pkt, sizeof(pkt));
    rtc_sctp_input(s->ep[to], pkt, sizeof(pkt), s->now);
}

static void test_sctp_init_in_shutdown(void)
{
    sim_t* s = sim_new(97, 0, 0, 0);
    uint32_t ia, sa;
    uint8_t pz, pp, pr;
    s->ep[0]->zcsum_local = 1;
    s->ep[1]->zcsum_local = 1;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    s->ep[1]->cwnd = 2 * s->ep[1]->mtu;
    rtc_sctp_send(s->ep[1], 1, 51, (const uint8_t*)"abcdefgh", 8, 0, -1, -1, s->now);
    s->drop_next = 0;
    sctp_inject_chunk(s, 1, RTC_SCTP_SHUTDOWN, s->ep[0]->rx_cum, 1);
    CHECK(s->ep[1]->state == RTC_SCTP_SHUTDOWN_RECEIVED, "init in shutdown: in SHUTDOWN-RECEIVED (%d)", s->ep[1]->state);
    pz = s->ep[1]->peer_zcsum;
    pp = s->ep[1]->peer_prsctp;
    pr = s->ep[1]->peer_reconfig;
    ia = s->first_chunk[1][RTC_SCTP_INIT_ACK];
    s->ctl_zero = 0;
    sctp_inject_init(s, 1);
    CHECK(s->ep[1]->peer_zcsum == pz && s->ep[1]->peer_prsctp == pp && s->ep[1]->peer_reconfig == pr,
        "init in shutdown: an INIT in SHUTDOWN-RECEIVED does not rewrite the peer's parameters (R2b-5)");
    CHECK(s->first_chunk[1][RTC_SCTP_INIT_ACK] == ia && s->ctl_zero == 0, "init in shutdown: no INIT ACK, never one with a zero checksum (RFC 9260 9.2, RFC 9653 5.2)");
    while (s->ep[1]->state == RTC_SCTP_SHUTDOWN_RECEIVED && s->now < 120000000)
        sim_run_until(s, s->now + 10000);
    CHECK(s->ep[1]->state == RTC_SCTP_SHUTDOWN_ACK_SENT, "init in shutdown: SHUTDOWN-ACK-SENT (%d)", s->ep[1]->state);
    sa = s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK];
    ia = s->first_chunk[1][RTC_SCTP_INIT_ACK];
    sctp_inject_init(s, 1);
    CHECK(s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK] == sa + 1 && s->first_chunk[1][RTC_SCTP_INIT_ACK] == ia && s->ctl_zero == 0,
        "init in shutdown: an INIT in SHUTDOWN-ACK-SENT is discarded and the SHUTDOWN ACK retransmitted (RFC 9260 9.2)");
    sim_free(s);
}

static void test_sctp_shutdown_received(void)
{
    sim_t* s = sim_new(61, 0, 0, 0);
    int i, guard = 0;
    uint32_t acks;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    s->ep[1]->cwnd = 4 * s->ep[1]->mtu;
    for (i = 0; i < 20; i++) {
        uint8_t* p = make_msg(1, (uint32_t)i, 1000);
        rtc_sctp_send(s->ep[1], 1, 51, p, 1000, 0, -1, -1, s->now);
        free(p);
    }
    rtc_sctp_flush(s->ep[1], s->now);
    sctp_inject_chunk(s, 1, RTC_SCTP_SHUTDOWN, s->ep[0]->rx_cum, 1);
    CHECK(s->ep[1]->state == RTC_SCTP_SHUTDOWN_RECEIVED, "shutdown: SHUTDOWN with data outstanding enters SHUTDOWN-RECEIVED (%d)", s->ep[1]->state);
    CHECK(rtc_sctp_send(s->ep[1], 1, 51, (const uint8_t*)"x", 1, 0, -1, -1, s->now) < 0, "shutdown: no new user data after SHUTDOWN");
    while (s->ep[1]->state == RTC_SCTP_SHUTDOWN_RECEIVED && guard++ < 10000)
        sim_run_until(s, s->now + 10000);
    CHECK(s->rx_count[0] == 20 && s->order_bad == 0, "shutdown: all 20 queued messages are flushed before the SHUTDOWN ACK (RFC 9260 9.2) (%u)", s->rx_count[0]);
    CHECK(s->ep[1]->state == RTC_SCTP_SHUTDOWN_ACK_SENT, "shutdown: then SHUTDOWN-ACK-SENT (%d)", s->ep[1]->state);
    CHECK(s->ep[0]->state == RTC_SCTP_ESTABLISHED, "shutdown: an unexpected SHUTDOWN ACK is ignored by an established peer");
    acks = s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK];
    sim_run_until(s, s->now + 3500000);
    CHECK(s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK] > acks, "shutdown: T2 retransmits the SHUTDOWN ACK (%u -> %u)", acks, s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK]);
    acks = s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK];
    sctp_inject_chunk(s, 1, RTC_SCTP_SHUTDOWN, s->ep[0]->rx_cum, 1);
    CHECK(s->first_chunk[1][RTC_SCTP_SHUTDOWN_ACK] == acks + 1, "shutdown: a retransmitted SHUTDOWN is answered again");
    sctp_inject_chunk(s, 1, RTC_SCTP_SHUTDOWN_COMPLETE, 0, 0);
    CHECK(s->ep[1]->state == RTC_SCTP_CLOSED && !s->aborted[1], "shutdown: SHUTDOWN COMPLETE closes gracefully (%d)", s->ep[1]->state);
    sim_free(s);
}

static void sctp_inject_reconf(sim_t* s, int to, uint16_t ptype, uint32_t a, uint32_t b, uint32_t c3, const uint16_t* ids, int nids)
{
    uint8_t pkt[64];
    uint32_t plen = ptype == 16 ? 12 : 16 + 2u * (uint32_t)nids, n = 12 + 4 + ((plen + 3) & ~3u);
    int i;
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_RECONFIG;
    rtc_wr16(pkt + 14, (uint16_t)(4 + plen));
    rtc_wr16(pkt + 16, ptype);
    rtc_wr16(pkt + 18, (uint16_t)plen);
    rtc_wr32(pkt + 20, a);
    rtc_wr32(pkt + 24, b);
    if (ptype != 16)
        rtc_wr32(pkt + 28, c3);
    for (i = 0; i < nids; i++)
        rtc_wr16(pkt + 32 + 2 * i, ids[i]);
    sctp_seal(pkt, n);
    rtc_sctp_input(s->ep[to], pkt, n, s->now);
}

static int sctp_q_reconf_result(sim_t* s, int to, uint32_t seq, uint32_t result)
{
    int i;
    for (i = 0; i < s->nq; i++) {
        const uint8_t* p = s->q[i].p;
        size_t off = 12;
        if (s->q[i].to != to)
            continue;
        while (off + 4 <= s->q[i].n) {
            uint16_t l = rtc_rd16(p + off + 2);
            if (l < 4)
                break;
            if (p[off] == RTC_SCTP_RECONFIG && l >= 16 && rtc_rd16(p + off + 4) == 16 && rtc_rd32(p + off + 8) == seq
                && rtc_rd32(p + off + 12) == result)
                return 1;
            off += ((size_t)l + 3) & ~(size_t)3;
        }
    }
    return 0;
}

static void test_sctp_reconfig_results(void)
{
    sim_t* s = sim_new(67, 0, 0, 0);
    int ro;
    uint8_t m[8] = { 0 };
    uint32_t seq;
    int i;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    for (i = 0; i < 3; i++) {
        rtc_sctp_send(s->ep[1], 1, 50, m, 8, 0, -1, -1, s->now);
        rtc_sctp_send(s->ep[1], 7, 50, m, 8, 0, -1, -1, s->now);
    }
    rtc_sctp_flush(s->ep[1], s->now);
    sim_run_until(s, s->now + 2000000);
    seq = s->ep[1]->reconf_peer_seq;
    sctp_inject_reconf(s, 1, 13, seq + 7, 0, s->ep[1]->rx_cum, NULL, 0);
    CHECK(sctp_q_reconf_result(s, 0, seq + 7, 5), "reconfig: an out-of-window request seq gets result 5 Bad Sequence Number (RFC 6525 5.2.2)");
    sctp_inject_reconf(s, 1, 13, seq, 0, s->ep[1]->rx_cum, NULL, 0);
    CHECK(s->reset_in[1][1] == 1 && s->reset_in[1][7] == 1, "reconfig: an empty stream list resets every incoming stream (RFC 6525 4.1) (%d %d)", s->reset_in[1][1], s->reset_in[1][7]);
    CHECK(sctp_q_reconf_result(s, 0, seq, 1), "reconfig: performed");
    sim_run_until(s, s->now + 2000000);
    s->loss_pct = 100;
    for (i = 0; i < 3; i++)
        rtc_sctp_send(s->ep[0], 4, 50, m, 8, 0, -1, -1, s->now);
    rtc_sctp_flush(s->ep[0], s->now);
    rtc_sctp_reset_stream(s->ep[0], 4);
    s->reset_req[0][4] = 1;
    s->ep[0]->flight = 0;
    s->ep[0]->ack_base = s->ep[0]->next_tsn;
    while (s->ep[0]->om_head != s->ep[0]->om_tail) {
        rtc_sctp_omsg* om = &s->ep[0]->om[s->ep[0]->om_head & (s->ep[0]->om_cap - 1)];
        free(om->data);
        om->data = NULL;
        s->ep[0]->om_head++;
    }
    s->ep[0]->om_chunk = s->ep[0]->om_tail;
    rtc_sctp_flush(s->ep[0], s->now);
    CHECK(s->ep[0]->reconf_inflight, "reconfig: request in flight");
    ro = s->reset_out[0][4];
    sctp_inject_reconf(s, 0, 16, s->ep[0]->reconf_req_seq, 2, 0, NULL, 0);
    CHECK(!s->ep[0]->reconf_inflight && s->reset_out[0][4] == ro + 1, "reconfig: a Denied result ends the request");
    CHECK(s->ep[0]->spages[0][4].out_ssn == 3, "reconfig: a Denied result does not reset the SSN (%u)", s->ep[0]->spages[0][4].out_ssn);
    sim_free(s);
}

static void test_sctp_fwd_many_streams(void)
{
    sim_t* s = sim_new(71, 0, 0, 0);
    uint8_t m[8] = { 0 };
    int i, seen = 0;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    s->drop_next = 1000000;
    s->ep[0]->cwnd = 1u << 30;
    for (i = 0; i < 400; i++)
        rtc_sctp_send(s->ep[0], (uint16_t)i, 50, m, 8, 0, 0, -1, s->now);
    rtc_sctp_flush(s->ep[0], s->now);
    sim_run_until(s, s->now + 1500000);
    s->drop_next = 0;
    sim_run_until(s, s->now + 30000000);
    for (i = 0; i < 400; i++)
        seen += (s->fwd_seen[i >> 3] >> (i & 7)) & 1;
    CHECK(seen == 400, "fwd-tsn: every abandoned ordered stream is listed across FORWARD-TSN chunks (%d of 400, RFC 3758 3.5)", seen);
    CHECK(s->ep[1]->rx_cum == s->ep[0]->next_tsn - 1, "fwd-tsn: the peer's cumulative TSN reaches the last abandoned TSN");
    sim_free(s);
}

static void test_sctp_unknown_chunks(void)
{
    sim_t* s = sim_new(73, 0, 0, 0);
    uint8_t pkt[48];
    uint32_t cum, k;
    static const uint8_t types[4] = { 0x3f, 0x7f, 0xbf, 0xff };
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    for (k = 0; k < 4; k++) {
        int before = 0, after = 0, i;
        for (i = 0; i < s->nq; i++)
            before += s->q[i].to == 0;
        cum = s->ep[1]->rx_cum;
        memset(pkt, 0, sizeof(pkt));
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        rtc_wr32(pkt + 4, s->ep[1]->my_tag);
        pkt[12] = types[k];
        rtc_wr16(pkt + 14, 8);
        pkt[20] = RTC_SCTP_DATA;
        pkt[21] = 0x07;
        rtc_wr16(pkt + 22, 24);
        rtc_wr32(pkt + 24, cum + 1);
        rtc_wr16(pkt + 28, 1);
        rtc_wr32(pkt + 32, 50);
        sctp_seal(pkt, 44);
        rtc_sctp_input(s->ep[1], pkt, 44, s->now);
        for (i = 0; i < s->nq; i++)
            after += s->q[i].to == 0;
        CHECK((s->ep[1]->rx_cum == cum + 1) == ((types[k] & 0x80) != 0), "unknown chunk 0x%02x: the following DATA is %s", types[k], types[k] & 0x80 ? "processed" : "discarded");
        CHECK(sctp_q_has_error(s, 0, 6, (uint16_t)((types[k] << 8) | 0)) == ((types[k] & 0x40) != 0), "unknown chunk 0x%02x: ERROR Unrecognized Chunk Type %s (RFC 9260 3.2)", types[k], types[k] & 0x40 ? "sent" : "not sent");
        sim_run_until(s, s->now + 1000000);
        (void)before;
        (void)after;
    }
    sim_free(s);
}

static void sctp_inject_data_zero(sim_t* s, int to, uint32_t tsn)
{
    uint8_t pkt[40];
    memset(pkt, 0, sizeof(pkt));
    rtc_wr16(pkt, 5000);
    rtc_wr16(pkt + 2, 5000);
    rtc_wr32(pkt + 4, s->ep[to]->my_tag);
    pkt[12] = RTC_SCTP_DATA;
    pkt[13] = 0x07;
    rtc_wr16(pkt + 14, 24);
    rtc_wr32(pkt + 16, tsn);
    rtc_wr16(pkt + 20, 1);
    rtc_wr32(pkt + 24, 50);
    rtc_sctp_input(s->ep[to], pkt, sizeof(pkt), s->now);
}

static void test_sctp_zero_checksum(void)
{
    int mode;
    for (mode = 0; mode < 4; mode++) {
        sim_t* s = sim_new(79 + mode, 5, 0, 5);
        int on0 = !(mode & 2), on1 = !(mode & 1), i, guard = 0;
        uint32_t cum;
        s->ep[0]->zcsum_local = (uint8_t)on0;
        s->ep[1]->zcsum_local = (uint8_t)on1;
        rtc_sctp_connect(s->ep[mode & 1], 0);
        sim_run_until(s, 60000000);
        CHECK(s->established[0] && s->established[1], "zcsum %d%d: established", on0, on1);
        CHECK(s->ep[0]->peer_zcsum == on1 && s->ep[1]->peer_zcsum == on0, "zcsum %d%d: each side learns the other's parameter (%d %d)", on0, on1, s->ep[0]->peer_zcsum, s->ep[1]->peer_zcsum);
        for (i = 0; i < 50; i++) {
            uint8_t* p = make_msg(2, (uint32_t)i, 700);
            rtc_sctp_send(s->ep[0], 2, 51, p, 700, 0, -1, -1, s->now);
            rtc_sctp_send(s->ep[1], 2, 51, p, 700, 0, -1, -1, s->now);
            free(p);
        }
        rtc_sctp_flush(s->ep[0], s->now);
        rtc_sctp_flush(s->ep[1], s->now);
        while ((s->rx_count[0] < 50 || s->rx_count[1] < 50) && guard++ < 1000)
            sim_run_until(s, s->now + 100000);
        CHECK(s->rx_count[0] == 50 && s->rx_count[1] == 50 && !s->corrupt, "zcsum %d%d: 50 messages each way", on0, on1);
        CHECK((s->ep[0]->st.zcsum_tx > 0) == on1 && (s->ep[1]->st.zcsum_tx > 0) == on0,
            "zcsum %d%d: zero checksums are sent only to a peer that advertised EDMID 1 (tx %llu %llu)", on0, on1,
            (unsigned long long)s->ep[0]->st.zcsum_tx, (unsigned long long)s->ep[1]->st.zcsum_tx);
        CHECK((s->ep[0]->st.zcsum_rx > 0) == on0 && (s->ep[1]->st.zcsum_rx > 0) == on1, "zcsum %d%d: and accepted only by the side that advertised it", on0, on1);
        CHECK(s->ctl_zero == 0, "zcsum %d%d: INIT, INIT ACK and COOKIE ECHO always carry a CRC32c", on0, on1);
        cum = s->ep[1]->rx_cum;
        sctp_inject_data_zero(s, 1, cum + 1);
        CHECK((s->ep[1]->rx_cum == cum + 1) == on1, "zcsum %d%d: an injected zero-checksum DATA is %s", on0, on1, on1 ? "accepted" : "dropped (not advertised)");
        sim_free(s);
    }
    {
        sim_t* s = sim_new(89, 0, 0, 0);
        uint8_t pkt[32];
        memset(pkt, 0, sizeof(pkt));
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        pkt[12] = RTC_SCTP_INIT;
        rtc_wr16(pkt + 14, 20);
        rtc_wr32(pkt + 16, 0x1234);
        rtc_wr32(pkt + 20, 65536);
        rtc_wr16(pkt + 24, 10);
        rtc_wr16(pkt + 26, 10);
        rtc_wr32(pkt + 28, 7);
        rtc_sctp_input(s->ep[1], pkt, sizeof(pkt), 0);
        CHECK(s->nq == 0, "zcsum: an INIT with a zero checksum is dropped (RFC 9653 5.2 1a)");
        sctp_seal(pkt, sizeof(pkt));
        rtc_sctp_input(s->ep[1], pkt, sizeof(pkt), 0);
        CHECK(s->nq == 1 && s->q[0].p[12] == RTC_SCTP_INIT_ACK, "zcsum: the same INIT with its CRC32c is answered");
        sim_free(s);
    }
    {
        sim_t* s = sim_new(97, 0, 0, 0);
        uint8_t pkt[32];
        rtc_sctp_connect(s->ep[0], 0);
        sim_run_until(s, s->delay + 1);
        CHECK(s->ep[1]->peer_zcsum == 1, "zcsum cookie: the INIT advertised EDMID 1");
        memset(pkt, 0, sizeof(pkt));
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        pkt[12] = RTC_SCTP_INIT;
        rtc_wr16(pkt + 14, 20);
        rtc_wr32(pkt + 16, 0x1234);
        rtc_wr32(pkt + 20, 65536);
        rtc_wr16(pkt + 24, 10);
        rtc_wr16(pkt + 26, 10);
        rtc_wr32(pkt + 28, 7);
        sctp_seal(pkt, sizeof(pkt));
        rtc_sctp_input(s->ep[1], pkt, sizeof(pkt), s->now);
        sim_run_until(s, 60000000);
        CHECK(s->established[1] && s->ep[1]->peer_zcsum == 1, "zcsum cookie: the association takes the peer's parameter from its own cookie, not from a later INIT");
        sim_free(s);
    }
}

static void test_addr_host_ok(void)
{
    static const char* const bad[] = { "127.0.0.1", "127.1.2.3", "169.254.1.1", "0.0.0.0", "::1", "::", "fe80::1", "fec0::1",
        "feff::1", "::1.2.3.4", "::ffff:10.0.0.1" };
    static const char* const good[] = { "10.0.0.1", "192.168.1.5", "8.8.8.8", "2001:db8::1", "fd00::1", "fe00::1" };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rtc_addr a;
        CHECK(rtc_addr_parse(bad[i], strlen(bad[i]), 0, &a) == 0 && !rtc_addr_host_ok(&a), "host candidates exclude %s (RFC 8445 5.1.1.1)", bad[i]);
    }
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        rtc_addr a;
        CHECK(rtc_addr_parse(good[i], strlen(good[i]), 0, &a) == 0 && rtc_addr_host_ok(&a), "host candidates keep %s", good[i]);
    }
}

static void test_sctp_big_cookie(void)
{
    static const uint32_t sizes[] = { 1800, 1484, 1180, 1100, 40000 };
    size_t z;
    for (z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
        sim_t* s = sim_new(31 + z, 0, 0, 0);
        uint32_t cl = sizes[z], n = 12 + 4 + 16 + 4 + ((cl + 3) & ~3u), i, big = 0;
        uint8_t* pkt = (uint8_t*)calloc(1, n);
        rtc_sctp_connect(s->ep[0], 0);
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        rtc_wr32(pkt + 4, s->ep[0]->my_tag);
        pkt[12] = RTC_SCTP_INIT_ACK;
        rtc_wr16(pkt + 14, 4 + 16 + 4 + cl);
        rtc_wr32(pkt + 16, 0x11223344u);
        rtc_wr32(pkt + 20, 65536);
        rtc_wr16(pkt + 24, 1024);
        rtc_wr16(pkt + 26, 1024);
        rtc_wr32(pkt + 28, 1);
        rtc_wr16(pkt + 32, 7);
        rtc_wr16(pkt + 34, 4 + cl);
        for (i = 0; i < cl; i++)
            pkt[36 + i] = (uint8_t)(i * 7 + 1);
        sctp_seal(pkt, n);
        rtc_sctp_input(s->ep[0], pkt, n, 1000);
        for (i = 0; i < (uint32_t)s->nq; i++)
            if (s->q[i].n > s->ep[0]->mtu)
                big++;
        CHECK(big == 0, "cookie %u: no emitted packet exceeds the MTU", (unsigned)cl);
        if (cl + 4 > s->ep[0]->mtu - 12)
            CHECK(s->ep[0]->state == RTC_SCTP_ABORTED, "cookie %u > MTU room aborts (state %d)", (unsigned)cl, s->ep[0]->state);
        else
            CHECK(s->ep[0]->state == RTC_SCTP_COOKIE_ECHOED, "cookie %u fits and is echoed (state %d)", (unsigned)cl, s->ep[0]->state);
        free(pkt);
        sim_free(s);
    }
}

static void test_sctp_malformed(void)
{
    sim_t* s = sim_new(11, 0, 0, 0);
    uint8_t pkt[64];
    size_t i;
    rtc_sctp_connect(s->ep[0], 0);
    sim_run_until(s, 60000000);
    for (i = 0; i < 2000; i++) {
        size_t n = 12 + 4 * (rtc_rng_next(&s->rng) % 13);
        uint32_t crc;
        rtc_rng_bytes(&s->rng, pkt, n);
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        rtc_wr32(pkt + 4, s->ep[1]->my_tag);
        memset(pkt + 8, 0, 4);
        crc = dyn_crc32c(pkt, n);
        pkt[8] = (uint8_t)crc;
        pkt[9] = (uint8_t)(crc >> 8);
        pkt[10] = (uint8_t)(crc >> 16);
        pkt[11] = (uint8_t)(crc >> 24);
        rtc_sctp_input(s->ep[1], pkt, n, s->now);
    }
    CHECK(1, "random chunks survive");
    sim_free(s);
}

typedef struct {
    rtc_ice a, b;
    rtc_rng rng;
    sim_pkt q[4096];
    int nq;
    uint64_t now;
    int drop_all;
    int drop_resp_to_a;
    int a_rx_local;
    uint64_t delay_us;
    int sent_by_a, sent_by_b;
} ice_sim;

static ice_sim* ISIM;

static void ice_send(void* ud, const rtc_cand* l, const rtc_addr* to, const uint8_t* p, size_t n)
{
    ice_sim* s = ISIM;
    sim_pkt* k;
    (void)to;
    if (ud == &s->a)
        s->sent_by_a++;
    else
        s->sent_by_b++;
    if (s->drop_all || s->nq >= 4096)
        return;
    if (s->drop_resp_to_a && ud == &s->b && n >= 2 && (p[0] & 1u) | (p[1] & 0x10u))
        return;
    k = &s->q[s->nq++];
    k->at = s->now + (s->delay_us ? s->delay_us : 1000);
    k->to = ud == &s->a ? 1 : 0;
    k->n = n;
    k->p = (uint8_t*)malloc(n + sizeof(rtc_addr));
    memcpy(k->p, p, n);
    memcpy(k->p + n, &l->addr, sizeof(rtc_addr));
}

static void ice_ev(void* ud, int ev)
{
    (void)ud;
    (void)ev;
}

static void ice_run(ice_sim* s, uint64_t until)
{
    while (s->now < until) {
        int i, best = -1;
        uint64_t next = until, t;
        for (i = 0; i < s->nq; i++)
            if (s->q[i].at < next) {
                next = s->q[i].at;
                best = i;
            }
        t = rtc_ice_tick(&s->a, s->now);
        if (t < next)
            next = t, best = -2;
        t = rtc_ice_tick(&s->b, s->now);
        if (t < next)
            next = t, best = -3;
        if (next > s->now)
            s->now = next;
        if (best >= 0) {
            sim_pkt k = s->q[best];
            rtc_stun_msg m;
            rtc_addr from;
            rtc_ice* dst = k.to ? &s->b : &s->a;
            s->q[best] = s->q[--s->nq];
            memcpy(&from, k.p + k.n, sizeof(rtc_addr));
            if (rtc_stun_parse(&m, k.p, k.n) == 0)
                rtc_ice_input(dst, k.to ? 0 : s->a_rx_local, &from, &m, s->now);
            free(k.p);
        } else if (best == -1)
            break;
    }
}

static void ice_setup(ice_sim* s, int a_ctl, int b_ctl, const char* bpwd_seen_by_a)
{
    uint8_t sd[32] = { 9 };
    rtc_cand c;
    memset(s, 0, sizeof(*s));
    ISIM = s;
    rtc_rng_seed(&s->rng, sd);
    rtc_ice_init(&s->a, &s->rng, a_ctl, ice_send, ice_ev, &s->a);
    rtc_ice_init(&s->b, &s->rng, b_ctl, ice_send, ice_ev, &s->b);
    rtc_ice_set_local_creds(&s->a, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_local_creds(&s->b, "bbbb", "bpassword-bpassword-bp");
    rtc_ice_set_remote_creds(&s->a, "bbbb", bpwd_seen_by_a);
    rtc_ice_set_remote_creds(&s->b, "aaaa", "apassword-apassword-ap");
    memset(&c, 0, sizeof(c));
    c.type = RTC_CAND_HOST;
    c.comp = 1;
    rtc_addr_parse("10.0.0.1", 8, 1111, &c.addr);
    c.base = c.addr;
    c.prio = rtc_ice_cand_prio(RTC_CAND_HOST, 65535, 1);
    c.found = 1;
    rtc_ice_add_local(&s->a, &c);
    rtc_ice_add_remote(&s->b, &c);
    rtc_addr_parse("10.0.0.2", 8, 2222, &c.addr);
    c.base = c.addr;
    c.found = 2;
    rtc_ice_add_local(&s->b, &c);
    rtc_ice_add_remote(&s->a, &c);
    s->a.local_done = s->b.local_done = 1;
    s->a.remote_done = s->b.remote_done = 1;
    rtc_ice_start(&s->a, 0);
    rtc_ice_start(&s->b, 0);
}

static void ice_free(ice_sim* s)
{
    int i;
    for (i = 0; i < s->nq; i++)
        free(s->q[i].p);
}

static void test_ice(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    const rtc_cand *l, *r;
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 5000000);
    CHECK(s->a.selected >= 0 && s->b.selected >= 0, "ice: both selected");
    CHECK(s->a.state == RTC_ICE_CONNECTED || s->a.state == RTC_ICE_COMPLETED, "ice: a connected (%d)", s->a.state);
    CHECK(rtc_ice_get_selected(&s->b, &l, &r) && r->addr.port == 1111, "ice: b selected a's address");
    ice_run(s, 40000000);
    CHECK(s->a.state == RTC_ICE_COMPLETED || s->a.state == RTC_ICE_CONNECTED, "ice: consent keeps it up (%d)", s->a.state);
    s->drop_all = 1;
    ice_run(s, 50000000);
    CHECK(s->a.state == RTC_ICE_DISCONNECTED, "ice: silence -> disconnected (%d)", s->a.state);
    ice_run(s, 80000000);
    CHECK(s->a.state == RTC_ICE_FAILED, "ice: consent expiry -> failed (%d)", s->a.state);
    ice_free(s);

    ice_setup(s, 1, 1, "bpassword-bpassword-bp");
    ice_run(s, 10000000);
    CHECK(s->a.controlling != s->b.controlling, "ice: role conflict resolved");
    CHECK(s->a.selected >= 0 && s->b.selected >= 0, "ice: conflict pair selected");
    ice_free(s);

    ice_setup(s, 1, 0, "not-the-right-password");
    ice_run(s, 60000000);
    CHECK(s->a.selected < 0, "ice: wrong password never selects");
    CHECK(s->a.state == RTC_ICE_FAILED, "ice: wrong password fails (%d)", s->a.state);
    ice_free(s);
    free(s);
}

static uint8_t icecap_buf[1024];
static size_t icecap_n;
static int icecap_count;

static void icecap_send(void* ud, const rtc_cand* l, const rtc_addr* to, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)l;
    (void)to;
    icecap_count++;
    if (n <= sizeof(icecap_buf)) {
        memcpy(icecap_buf, p, n);
        icecap_n = n;
    }
}

static void ice_add_host(rtc_ice* ice, const char* ip, uint16_t port, uint32_t found, int remote, int sock)
{
    rtc_cand c;
    memset(&c, 0, sizeof(c));
    c.type = RTC_CAND_HOST;
    c.comp = 1;
    c.sock = (uint8_t)sock;
    rtc_addr_parse(ip, strlen(ip), port, &c.addr);
    c.base = c.addr;
    c.prio = rtc_ice_cand_prio(RTC_CAND_HOST, 65535 - found, 1);
    c.found = found;
    if (remote)
        rtc_ice_add_remote(ice, &c);
    else
        rtc_ice_add_local(ice, &c);
}

static size_t ice_req(uint8_t* buf, size_t cap, const char* user, const char* pwd, int fp, int use_cand)
{
    rtc_stun_wr w;
    uint8_t tid[12] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    rtc_stun_wr_init(&w, buf, cap, rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), tid);
    if (user)
        rtc_stun_wr_attr(&w, RTC_SA_USERNAME, user, strlen(user));
    rtc_stun_wr_u32(&w, RTC_SA_PRIORITY, 1000);
    rtc_stun_wr_u64(&w, use_cand ? RTC_SA_ICE_CONTROLLING : RTC_SA_ICE_CONTROLLED, 1);
    if (use_cand)
        rtc_stun_wr_attr(&w, RTC_SA_USE_CANDIDATE, NULL, 0);
    if (pwd)
        rtc_stun_wr_integrity(&w, (const uint8_t*)pwd, strlen(pwd));
    if (fp)
        rtc_stun_wr_fingerprint(&w);
    return rtc_stun_wr_done(&w);
}

static void test_ice_auth_replies(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 3 }, buf[512], bare[20];
    rtc_stun_msg m;
    rtc_addr from;
    size_t n;
    int i;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 0, icecap_send, ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    rtc_ice_start(&ice, 0);
    CHECK(ice.state == RTC_ICE_NEW, "ice: no remote candidates yet -> still new, not checking (%d)", ice.state);
    rtc_addr_parse("10.0.0.9", 8, 9999, &from);
    memset(bare, 0, sizeof(bare));
    rtc_wr16(bare, 0x0001);
    rtc_wr32(bare + 4, 0x2112A442u);
    bare[8] = 1;
    icecap_count = 0;
    icecap_n = 0;
    CHECK(rtc_stun_parse(&m, bare, 20) == 0, "ice: 20 B bare binding request parses");
    m.fp = 0;
    n = ice_req(buf, sizeof(buf), NULL, NULL, 1, 0);
    rtc_stun_parse(&m, buf, n);
    rtc_ice_input(&ice, 0, &from, &m, 1000);
    CHECK(icecap_count == 1 && icecap_n <= 36, "ice: unauthenticated request gets a short 400 (%zu B for a %zu B request)", icecap_n, n);
    {
        rtc_stun_msg r;
        CHECK(rtc_stun_parse(&r, icecap_buf, icecap_n) == 0 && !rtc_stun_find(&r, RTC_SA_XOR_MAPPED_ADDRESS)
                && rtc_stun_error_code(&r) == 400,
            "ice: the 400 carries no XOR-MAPPED-ADDRESS");
    }
    n = ice_req(buf, sizeof(buf), "zzzz:bbbb", "nope-nope-nope-nope-no", 1, 0);
    rtc_stun_parse(&m, buf, n);
    icecap_count = 0;
    for (i = 0; i < 50; i++)
        rtc_ice_input(&ice, 0, &from, &m, 2000000 + (uint64_t)i * 1000);
    CHECK(icecap_count == (int)RTC_ICE_ERR_BURST, "ice: 401 replies are rate limited to %u per second (%d)", RTC_ICE_ERR_BURST, icecap_count);
    rtc_ice_input(&ice, 0, &from, &m, 3100000);
    CHECK(icecap_count == (int)RTC_ICE_ERR_BURST + 1, "ice: the error budget refills after a second");
    n = ice_req(buf, sizeof(buf), "aaaa:bbbb", "apassword-apassword-ap", 0, 0);
    rtc_stun_parse(&m, buf, n);
    icecap_count = 0;
    rtc_ice_input(&ice, 0, &from, &m, 4000000);
    CHECK(icecap_count == 0 && ice.nremote == 0, "ice: a check without FINGERPRINT is ignored (RFC 8445 7.2.2)");
    CHECK(!rtc_ice_peer_ok(&ice, 0, &from), "ice: an address that never authenticated is not a DTLS peer");
    n = ice_req(buf, sizeof(buf), "aaaa:bbbb", "apassword-apassword-ap", 1, 0);
    rtc_stun_parse(&m, buf, n);
    rtc_ice_input(&ice, 0, &from, &m, 4000000);
    CHECK(icecap_count == 1 && ice.nremote == 1 && ice.remote[0].type == RTC_CAND_PRFLX, "ice: authenticated check learns a prflx remote");
    CHECK(rtc_ice_peer_ok(&ice, 0, &from), "ice: an authenticated sender is a DTLS peer");
    CHECK(ice.state == RTC_ICE_CHECKING, "ice: a remote candidate moves new -> checking (%d)", ice.state);
}

static void test_ice_prflx_local(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 4 }, buf[512];
    rtc_stun_msg m;
    rtc_addr from;
    size_t n;
    int i, np = 0;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 0, icecap_send, ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_remote_creds(&ice, "bbbb", "bpassword-bpassword-bp");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    ice_add_host(&ice, "10.0.0.3", 1113, 3, 0, 1);
    ice_add_host(&ice, "10.0.0.4", 1114, 4, 0, 2);
    rtc_ice_start(&ice, 0);
    rtc_addr_parse("10.0.0.9", 8, 9999, &from);
    n = ice_req(buf, sizeof(buf), "aaaa:bbbb", "apassword-apassword-ap", 1, 0);
    rtc_stun_parse(&m, buf, n);
    rtc_ice_input(&ice, 1, &from, &m, 1000);
    for (i = 0; i < ice.npairs; i++)
        if (ice.pairs[i].r == 0)
            np++;
    CHECK(np == 1 && ice.pairs[0].l == 1, "ice: a prflx remote pairs only with the local that received it (RFC 8445 7.3.1.3) (%d)", np);
    ice_add_host(&ice, "10.0.0.9", 9999, 9, 1, 0);
    np = 0;
    for (i = 0; i < ice.npairs; i++)
        if (ice.pairs[i].r == 0)
            np++;
    CHECK(np == 3 && ice.remote[0].type == RTC_CAND_HOST, "ice: once signalled, the candidate pairs with every local (%d)", np);
}

static void test_ice_consent_v2(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    const rtc_cand *l, *r;
    int sel;
    ice_setup(s, 0, 1, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    CHECK(s->a.selected >= 0, "consent: connected");
    s->drop_resp_to_a = 1;
    ice_run(s, 3000000 + 31000000);
    CHECK(s->a.state == RTC_ICE_FAILED, "consent: inbound requests alone do not refresh consent (RFC 7675 5.1) -> failed by 31 s (%d)", s->a.state);
    CHECK(!rtc_ice_get_selected(&s->a, &l, &r), "consent: nothing selected after expiry");
    {
        uint8_t buf[512];
        rtc_stun_msg m;
        rtc_addr from;
        size_t n = ice_req(buf, sizeof(buf), "aaaa:bbbb", "apassword-apassword-ap", 1, 1);
        rtc_addr_parse("10.0.0.2", 8, 2222, &from);
        rtc_stun_parse(&m, buf, n);
        rtc_ice_input(&s->a, 0, &from, &m, s->now);
        CHECK(s->a.state == RTC_ICE_FAILED && s->a.selected < 0, "consent: a nomination after expiry does not revive the pair (RFC 7675)");
        CHECK(!rtc_ice_peer_ok(&s->a, 0, &from), "consent: no DTLS peer once failed");
    }
    ice_free(s);

    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    sel = s->a.selected;
    CHECK(sel >= 0, "consent 403: connected");
    {
        rtc_pair* p = &s->a.pairs[sel];
        rtc_stun_wr w;
        rtc_stun_msg m;
        uint8_t buf[256];
        size_t n;
        s->drop_all = 1;
        ice_run(s, 9000000);
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_ERROR), p->tid);
        rtc_stun_wr_error(&w, 403, "Forbidden");
        rtc_stun_wr_integrity(&w, (const uint8_t*)"bpassword-bpassword-bp", 22);
        rtc_stun_wr_fingerprint(&w);
        n = rtc_stun_wr_done(&w);
        rtc_stun_parse(&m, buf, n);
        rtc_ice_input(&s->a, 0, &s->a.remote[p->r].addr, &m, s->now);
        CHECK(s->a.state == RTC_ICE_FAILED, "consent 403: an authenticated 403 to a consent check revokes consent (RFC 7675) (%d)", s->a.state);
    }
    ice_free(s);
    free(s);
}

static void test_ice_tid_rtt(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    s->delay_us = 350000;
    ice_run(s, 20000000);
    CHECK(s->a.selected >= 0 && s->b.selected >= 0, "ice: 700 ms RTT still connects; retransmits keep the TID (RFC 8489 6.2.1)");
    ice_free(s);
    free(s);
}

static void test_ice_tid_reuse_and_role(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 5 }, tid0[12], buf[512];
    uint64_t now = 0;
    int same = 1, sends = 0, i, k;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 1, icecap_send, ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_remote_creds(&ice, "bbbb", "bpassword-bpassword-bp");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    ice_add_host(&ice, "10.0.0.2", 2222, 2, 1, 0);
    ice.local_done = ice.remote_done = 1;
    rtc_ice_start(&ice, 0);
    icecap_count = 0;
    while (now < 40000000 && ice.pairs[0].state != RTC_PAIR_FAILED) {
        uint64_t nx;
        int before = icecap_count;
        nx = rtc_ice_tick(&ice, now);
        if (icecap_count != before) {
            rtc_stun_msg m;
            rtc_stun_parse(&m, icecap_buf, icecap_n);
            if (!sends)
                memcpy(tid0, m.tid, 12);
            else if (memcmp(tid0, m.tid, 12))
                same = 0;
            sends++;
        }
        now = nx > now && nx != UINT64_MAX ? nx : now + 10000;
    }
    CHECK(sends == RTC_ICE_RC && same, "ice: %d retransmissions of one check all reuse its transaction id (RFC 8489 6.2.1)", sends);

    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 1, icecap_send, ice_ev, NULL);
    ice.tiebreaker = 5;
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_remote_creds(&ice, "bbbb", "bpassword-bpassword-bp");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    ice_add_host(&ice, "10.0.0.2", 2222, 2, 1, 0);
    ice_add_host(&ice, "10.0.0.3", 3333, 3, 1, 0);
    rtc_ice_start(&ice, 0);
    rtc_ice_tick(&ice, 0);
    rtc_ice_tick(&ice, 60000);
    CHECK(ice.pairs[0].state == RTC_PAIR_INPROGRESS && ice.pairs[1].state == RTC_PAIR_INPROGRESS, "role: two checks in flight as controlling");
    for (k = 0; k < 2; k++) {
        rtc_stun_wr w;
        rtc_stun_msg m;
        size_t n;
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_ERROR), ice.pairs[k].tid);
        rtc_stun_wr_error(&w, 487, "Role Conflict");
        rtc_stun_wr_integrity(&w, (const uint8_t*)"bpassword-bpassword-bp", 22);
        rtc_stun_wr_fingerprint(&w);
        n = rtc_stun_wr_done(&w);
        rtc_stun_parse(&m, buf, n);
        rtc_ice_input(&ice, 0, &ice.remote[ice.pairs[k].r].addr, &m, 70000);
    }
    CHECK(!ice.controlling, "role: two 487s to requests sent as controlling leave us controlled, not toggled back (RFC 8445 7.2.5.1)");
    CHECK(ice.tiebreaker != 5, "role: the tiebreaker changed after the switch");
    for (i = 0; i < 2; i++)
        CHECK(ice.pairs[i].state == RTC_PAIR_WAITING, "role: pair %d re-queued as waiting", i);
}

static void test_ice_role_multi(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    int k;
    for (k = 0; k < 8; k++) {
        ice_setup(s, 1, 1, "bpassword-bpassword-bp");
        s->a.tiebreaker = 100 + (uint64_t)k;
        s->b.tiebreaker = 100 + (uint64_t)(7 - k);
        ice_add_host(&s->a, "10.0.1.1", 1201, 11, 0, 1);
        ice_add_host(&s->a, "10.0.1.2", 1202, 12, 0, 2);
        ice_add_host(&s->a, "10.0.1.3", 1203, 13, 0, 3);
        ice_add_host(&s->b, "10.0.1.1", 1201, 11, 1, 0);
        ice_add_host(&s->b, "10.0.1.2", 1202, 12, 1, 0);
        ice_add_host(&s->b, "10.0.1.3", 1203, 13, 1, 0);
        s->delay_us = 120000;
        ice_run(s, 30000000);
        CHECK(s->a.controlling != s->b.controlling, "ice role (%d): exactly one side controls after conflicts with checks in flight", k);
        CHECK(s->a.selected >= 0 && s->b.selected >= 0, "ice role (%d): connected", k);
        CHECK(s->a.tiebreaker != 100 + (uint64_t)k || s->b.tiebreaker != 100 + (uint64_t)(7 - k), "ice role (%d): the side that got 487 changed its tiebreaker (RFC 8445 7.2.5.1)", k);
        ice_free(s);
    }
    free(s);
}

static void test_ice_restart_prev(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    const rtc_cand *l, *r;
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    CHECK(s->a.selected >= 0, "restart: connected");
    rtc_ice_restart(&s->a, "cccc", "cpassword-cpassword-cp");
    ice_run(s, 45000000);
    CHECK(s->a.state != RTC_ICE_FAILED && s->a.state != RTC_ICE_DISCONNECTED, "restart: an unanswered restart keeps consent on the old pair (%d)", s->a.state);
    CHECK(s->b.state == RTC_ICE_CONNECTED || s->b.state == RTC_ICE_COMPLETED, "restart: the peer's checks on the old pair still get answers (%d)", s->b.state);
    CHECK(rtc_ice_get_selected(&s->a, &l, &r) && r->addr.port == 2222, "restart: the old pair still carries data");
    s->drop_all = 1;
    ice_run(s, 80000000);
    CHECK(s->a.state == RTC_ICE_FAILED, "restart: a never-completing restart with a silent peer fails at consent expiry (%d)", s->a.state);
    ice_free(s);
    free(s);
}

static void ice_restart_both(ice_sim* s)
{
    rtc_ice_restart(&s->a, "cccc", "cpassword-cpassword-cp");
    rtc_ice_restart(&s->b, "dddd", "dpassword-dpassword-dp");
    rtc_ice_set_remote_creds(&s->a, "dddd", "dpassword-dpassword-dp");
    rtc_ice_set_remote_creds(&s->b, "cccc", "cpassword-cpassword-cp");
    ice_add_host(&s->a, "10.0.0.2", 2222, 2, 1, 0);
    ice_add_host(&s->b, "10.0.0.1", 1111, 1, 1, 0);
    s->a.local_done = s->b.local_done = 1;
    s->a.remote_done = s->b.remote_done = 1;
}


static void test_ice_restart_prev_rx(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    rtc_addr old, sel;
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    CHECK(s->b.selected >= 0, "restart rx: connected");
    ice_restart_both(s);
    rtc_addr_parse("10.0.0.7", 8, 7777, &old);
    s->b.prev.r.addr = old;
    CHECK(s->b.have_prev && rtc_ice_peer_ok(&s->b, -1, &old), "restart rx: the old remote is a DTLS peer during the restart");
    ice_run(s, s->now + 3000000);
    CHECK(s->b.selected >= 0 && !s->b.have_prev, "restart rx: the restart selected a new pair");
    CHECK(rtc_ice_peer_ok(&s->b, -1, &old), "restart rx (DEC-a): after selecting, datagrams the peer still sends on the old path before it selects are accepted until it moves");
    rtc_ice_note_rx(&s->b, &old);
    CHECK(rtc_ice_peer_ok(&s->b, -1, &old), "restart rx: more data on the old path keeps it open");
    sel = s->b.remote[s->b.pairs[s->b.selected].r].addr;
    rtc_ice_note_rx(&s->b, &sel);
    CHECK(!rtc_ice_peer_ok(&s->b, -1, &old), "restart rx: the first datagram on the selected pair closes the old path");
    CHECK(rtc_ice_peer_ok(&s->b, -1, &sel), "restart rx: the selected pair stays a DTLS peer");
    ice_free(s);
    free(s);
}
static size_t ice_err_rsp(uint8_t* buf, size_t cap, const uint8_t* tid, int code, const char* pwd)
{
    rtc_stun_wr w;
    rtc_stun_wr_init(&w, buf, cap, rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_ERROR), tid);
    rtc_stun_wr_error(&w, code, "x");
    rtc_stun_wr_integrity(&w, (const uint8_t*)pwd, strlen(pwd));
    rtc_stun_wr_fingerprint(&w);
    return rtc_stun_wr_done(&w);
}

static void test_ice_restart_consent(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    uint64_t t0;
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    CHECK(s->a.selected >= 0 && s->b.selected >= 0, "restart consent: connected");
    t0 = s->now;
    s->drop_all = 1;
    ice_run(s, t0 + 25000000);
    CHECK(s->a.state == RTC_ICE_DISCONNECTED, "restart consent: silent peer -> disconnected before the restart (%d)", s->a.state);
    ice_restart_both(s);
    CHECK(s->a.have_prev && s->b.have_prev, "restart consent: the old pair is kept for consent during the restart");
    CHECK(s->a.controlling && !s->b.controlling, "restart consent: roles survive rtc_ice_restart (RFC 8445 6.1.1)");
    ice_run(s, t0 + 31000000);
    CHECK(!s->a.have_prev && s->a.state != RTC_ICE_FAILED, "restart consent (R1b-2): the old pair's consent expiring mid-restart drops it and does not fail the agent (%d)",
        s->a.state);
    s->drop_all = 0;
    ice_run(s, t0 + 60000000);
    CHECK(s->a.selected >= 0 && (s->a.state == RTC_ICE_CONNECTED || s->a.state == RTC_ICE_COMPLETED),
        "restart consent (R1b-2): responses resume at 31 s -> the new checklist connects (%d)", s->a.state);
    CHECK(s->b.selected >= 0 && (s->b.state == RTC_ICE_CONNECTED || s->b.state == RTC_ICE_COMPLETED), "restart consent: the peer connects too (%d)", s->b.state);
    ice_free(s);

    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    ice_restart_both(s);
    s->drop_all = 1;
    ice_run(s, s->now + 7000000);
    {
        uint8_t buf[256];
        rtc_stun_msg m;
        size_t n = ice_err_rsp(buf, sizeof(buf), s->a.prev.tid, 403, "bpassword-bpassword-bp");
        rtc_stun_parse(&m, buf, n);
        rtc_ice_input(&s->a, 0, &s->a.prev.r.addr, &m, s->now);
    }
    CHECK(!s->a.have_prev && s->a.state != RTC_ICE_FAILED, "restart consent: a 403 on the old pair during a restart drops the old pair only (%d)", s->a.state);
    s->drop_all = 0;
    ice_run(s, s->now + 20000000);
    CHECK(s->a.selected >= 0, "restart consent: the new checklist still connects after the 403");
    ice_free(s);

    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_run(s, 3000000);
    s->a.local_done = s->a.remote_done = 1;
    s->drop_all = 1;
    ice_run(s, s->now + 3000000);
    ice_restart_both(s);
    s->a.remote_done = s->a.local_done = 1;
    ice_run(s, s->now + 80000000);
    CHECK(s->a.state == RTC_ICE_FAILED, "restart consent: old pair expired and every new pair failed -> failed (%d)", s->a.state);
    ice_free(s);
    free(s);
}

static void test_ice_487_tid(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 6 }, buf[512], t1[12];
    rtc_stun_msg m;
    rtc_addr from;
    size_t n;
    int before;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 1, icecap_send, ice_ev, NULL);
    ice.tiebreaker = 5;
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_remote_creds(&ice, "bbbb", "bpassword-bpassword-bp");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    ice_add_host(&ice, "10.0.0.2", 2222, 2, 1, 0);
    rtc_ice_start(&ice, 0);
    rtc_ice_tick(&ice, 0);
    CHECK(ice.pairs[0].state == RTC_PAIR_INPROGRESS && (ice.pairs[0].flags & RTC_PF_SENT_CTL), "487 tid: check in flight as controlling");
    memcpy(t1, ice.pairs[0].tid, 12);
    {
        rtc_stun_wr w;
        uint8_t tid[12] = { 9, 9, 9 };
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), tid);
        rtc_stun_wr_attr(&w, RTC_SA_USERNAME, "aaaa:bbbb", 9);
        rtc_stun_wr_u32(&w, RTC_SA_PRIORITY, 1000);
        rtc_stun_wr_u64(&w, RTC_SA_ICE_CONTROLLING, UINT64_MAX);
        rtc_stun_wr_integrity(&w, (const uint8_t*)"apassword-apassword-ap", 22);
        rtc_stun_wr_fingerprint(&w);
        n = rtc_stun_wr_done(&w);
        rtc_stun_parse(&m, buf, n);
        rtc_addr_parse("10.0.0.2", 8, 2222, &from);
        rtc_ice_input(&ice, 0, &from, &m, 10000);
    }
    CHECK(!ice.controlling, "487 tid: a higher controlling tiebreaker switches us to controlled");
    before = icecap_count;
    rtc_ice_tick(&ice, ice.pairs[0].next_us);
    CHECK(icecap_count > before && memcmp(ice.pairs[0].tid, t1, 12) && !(ice.pairs[0].flags & RTC_PF_SENT_CTL),
        "487 tid (R1b-8): the retransmit after a role change is a new transaction, sent as controlled");
    n = ice_err_rsp(buf, sizeof(buf), t1, 487, "bpassword-bpassword-bp");
    rtc_stun_parse(&m, buf, n);
    rtc_ice_input(&ice, 0, &from, &m, 600000);
    CHECK(!ice.controlling, "487 tid (R1b-8): a 487 to the request sent as controlling does not flip us back to controlling");
    CHECK(ice.pairs[0].state == RTC_PAIR_INPROGRESS, "487 tid: the late 487 leaves the current transaction running (%d)", ice.pairs[0].state);
    n = ice_err_rsp(buf, sizeof(buf), ice.pairs[0].tid, 487, "bpassword-bpassword-bp");
    rtc_stun_parse(&m, buf, n);
    rtc_ice_input(&ice, 0, &from, &m, 700000);
    CHECK(ice.controlling, "487 tid: a 487 to the request sent as controlled switches us to controlling (RFC 8445 7.2.5.1)");
}

static void test_ice_stage_local(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 7 }, buf[512];
    rtc_stun_msg m;
    rtc_addr from;
    size_t n;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 0, icecap_send, ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    rtc_ice_set_remote_creds(&ice, "bbbb", "bpassword-bpassword-bp");
    ice_add_host(&ice, "10.0.0.1", 1111, 1, 0, 0);
    rtc_addr_parse("10.0.0.7", 8, 7777, &from);
    rtc_ice_start(&ice, 0);
    n = ice_req(buf, sizeof(buf), "eeee:bbbb", "epassword-epassword-ep", 1, 0);
    rtc_stun_parse(&m, buf, n);
    icecap_count = 0;
    rtc_ice_input(&ice, 0, &from, &m, 1000);
    rtc_stun_parse(&m, icecap_buf, icecap_n);
    CHECK(icecap_count == 1 && rtc_stun_class(m.type) == RTC_STUN_ERROR, "stage: unknown credentials get 401");
    rtc_ice_stage_local(&ice, "eeee", "epassword-epassword-ep");
    n = ice_req(buf, sizeof(buf), "eeee:bbbb", "epassword-epassword-ep", 1, 0);
    rtc_stun_parse(&m, buf, n);
    icecap_count = 0;
    rtc_ice_input(&ice, 0, &from, &m, 2000);
    rtc_stun_parse(&m, icecap_buf, icecap_n);
    CHECK(icecap_count == 1 && rtc_stun_class(m.type) == RTC_STUN_SUCCESS && rtc_stun_check_integrity(&m, (const uint8_t*)"epassword-epassword-ep", 22),
        "stage: a check with the staged (offered, not yet answered) credentials is answered with them");
    CHECK(ice.npairs == 0 && ice.nremote == 0, "stage: staged credentials change nothing in the checklist");
    CHECK(!strcmp(ice.lufrag, "aaaa"), "stage: the current credentials stay in force");
    rtc_ice_stage_local(&ice, "", "");
    rtc_stun_parse(&m, buf, n);
    icecap_count = 0;
    rtc_ice_input(&ice, 0, &from, &m, 3000);
    rtc_stun_parse(&m, icecap_buf, icecap_n);
    CHECK(icecap_count == 1 && rtc_stun_class(m.type) == RTC_STUN_ERROR, "stage: after unstaging (rollback) the offered credentials get 401 again");
    rtc_ice_stage_local(&ice, "eeee", "epassword-epassword-ep");
    rtc_ice_restart(&ice, "ffff", "fpassword-fpassword-fp");
    CHECK(!ice.stage_ufrag[0], "stage: a restart clears the staged credentials");
}

static void test_ice_local_reuse(void)
{
    rtc_ice ice;
    rtc_rng rng;
    uint8_t sd[32] = { 8 };
    rtc_cand c;
    char ip[32];
    int i, r;
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 1, icecap_send, ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "aaaa", "apassword-apassword-ap");
    for (i = 0; i < RTC_ICE_MAX_LOCAL; i++) {
        memset(&c, 0, sizeof(c));
        c.type = RTC_CAND_RELAY;
        c.comp = 1;
        c.relay = (uint8_t)(i + 1);
        snprintf(ip, sizeof(ip), "10.1.0.%d", i + 1);
        rtc_addr_parse(ip, strlen(ip), 5000, &c.addr);
        c.base = c.addr;
        c.prio = 100;
        rtc_ice_add_local(&ice, &c);
    }
    CHECK(ice.nlocal == RTC_ICE_MAX_LOCAL, "local reuse: table full (%d)", ice.nlocal);
    c.addr.port = 6000;
    c.base = c.addr;
    c.relay = 3;
    CHECK(rtc_ice_add_local(&ice, &c) < 0, "local reuse: a full table with nothing released refuses");
    ice.local[2].off = 1;
    r = rtc_ice_add_local(&ice, &c);
    CHECK(r == 2 && !ice.local[2].off && ice.local[2].addr.port == 6000, "local reuse: a released candidate's slot is reused (%d)", r);
    CHECK(rtc_ice_find_local(&ice, 0, 3) == 2, "local reuse: the live candidate of relay 3 is found");
    ice.local[1].off = 1;
    ice.local[1].relay = 3;
    CHECK(rtc_ice_find_local(&ice, 0, 3) == 2, "local find: a live relay candidate wins over a released one at a lower index (slot reuse aliasing)");
    ice.local[2].off = 1;
    CHECK(rtc_ice_find_local(&ice, 0, 3) == 1, "local find: with only released ones, the first released one still answers (old pair until selection)");
    ice.local[2].off = 0;
    ice.local[5].off = 1;
    ice.npairs = 1;
    ice.pairs[0].l = 1;
    c.addr.port = 7000;
    c.base = c.addr;
    c.relay = 9;
    r = rtc_ice_add_local(&ice, &c);
    CHECK(r == 5, "local reuse: a released slot still referenced by a pair is skipped (%d)", r);
}

static void test_ice_after_nomination(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    uint64_t tx_before;
    int i;
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_add_host(&s->a, "10.0.1.1", 1201, 11, 0, 1);
    ice_add_host(&s->a, "10.0.1.3", 1203, 13, 0, 3);
    ice_add_host(&s->a, "10.0.1.4", 1204, 14, 0, 4);
    ice_add_host(&s->a, "10.0.1.5", 1205, 15, 0, 5);
    ice_add_host(&s->a, "10.0.1.2", 1202, 12, 0, 2);
    s->delay_us = 1000;
    while (s->a.selected < 0 && s->now < 5000000)
        ice_run(s, s->now + 2000);
    CHECK(s->a.selected >= 0 && (s->a.pairs[s->a.selected].flags & RTC_PF_NOMINATED), "nomination: selected and nominated");
    tx_before = 0;
    for (i = 0; i < s->a.npairs; i++)
        if (i != s->a.selected)
            tx_before += s->a.pairs[i].req_tx;
    ice_run(s, 20000000);
    {
        uint64_t tx_after = 0;
        for (i = 0; i < s->a.npairs; i++)
            if (i != s->a.selected)
                tx_after += s->a.pairs[i].req_tx;
        CHECK(tx_after == tx_before, "nomination: no checks on other pairs after nomination (RFC 8445 8.1.2) (%llu -> %llu)",
            (unsigned long long)tx_before, (unsigned long long)tx_after);
    }
    ice_free(s);
    free(s);
}

static void test_ice_symmetry_local(void)
{
    ice_sim* s = (ice_sim*)calloc(1, sizeof(*s));
    ice_setup(s, 1, 0, "bpassword-bpassword-bp");
    ice_add_host(&s->a, "10.0.1.1", 1201, 11, 0, 1);
    s->a_rx_local = 1;
    ice_run(s, 300000);
    CHECK(!(s->a.pairs[0].flags & RTC_PF_VALID), "symmetry: a response arriving on another local socket does not validate the pair (RFC 8445 7.2.5.2.1)");
    ice_free(s);
    free(s);
}

static void test_ice_relay_only_pairs(void)
{
    rtc_ice ice;
    rtc_rng rng;
    rtc_cand c;
    uint8_t sd[32] = { 7 };
    rtc_rng_seed(&rng, sd);
    rtc_ice_init(&ice, &rng, 1, NULL, NULL, NULL);
    ice.relay_only = 1;
    ice_add_host(&ice, "10.0.0.1", 1000, 1, 0, 0);
    memset(&c, 0, sizeof(c));
    c.type = RTC_CAND_RELAY;
    c.comp = 1;
    rtc_addr_parse("10.0.0.9", 8, 3478, &c.addr);
    c.base = c.addr;
    c.relay = 1;
    c.prio = rtc_ice_cand_prio(RTC_CAND_RELAY, 1, 1);
    c.found = 9;
    rtc_ice_add_local(&ice, &c);
    ice_add_host(&ice, "10.0.0.2", 2000, 2, 1, 0);
    CHECK(ice.npairs == 1 && ice.local[ice.pairs[0].l].type == RTC_CAND_RELAY,
        "relay-only: under iceTransportPolicy relay only relay locals are paired (%d pairs)", ice.npairs);
    ice.relay_only = 0;
    ice_add_host(&ice, "10.0.0.3", 3000, 3, 1, 0);
    CHECK(ice.npairs == 3, "relay-only: policy all pairs host locals again (%d pairs)", ice.npairs);
}

static const char* const offer_sdp = "v=0\r\n"
                                     "o=- 4215775240449105457 2 IN IP4 127.0.0.1\r\n"
                                     "s=-\r\n"
                                     "t=0 0\r\n"
                                     "a=group:BUNDLE 0\r\n"
                                     "a=extmap-allow-mixed\r\n"
                                     "a=msid-semantic: WMS\r\n"
                                     "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                                     "c=IN IP4 0.0.0.0\r\n"
                                     "a=candidate:1467250027 1 udp 2122260223 192.168.1.10 56143 typ host generation 0\r\n"
                                     "a=candidate:842163049 1 udp 1686052607 203.0.113.5 46154 typ srflx raddr 192.168.1.10 rport 56143\r\n"
                                     "a=candidate:1 1 udp 2122194687 abcd.local 5000 typ host\r\n"
                                     "a=ice-ufrag:EsAw\r\n"
                                     "a=ice-pwd:bP+XJMM09aR8AiX1jdukzR6Y\r\n"
                                     "a=ice-options:trickle\r\n"
                                     "a=fingerprint:sha-256 D2:FA:0E:C3:22:59:5E:14:95:69:92:3D:13:B4:84:24:2C:C2:A2:C0:3E:FD:34:8E:5E:EA:6F:AF:52:CE:E6:0F\r\n"
                                     "a=setup:actpass\r\n"
                                     "a=mid:0\r\n"
                                     "a=sctp-port:5000\r\n"
                                     "a=max-message-size:262144\r\n";

static void test_sdp(void)
{
    rtc_sdp* sdp = (rtc_sdp*)malloc(sizeof(*sdp));
    const char* err = NULL;
    rtc_sdp_cand c;
    char line[256];
    rtc_addr want;
    if (!sdp)
        return;
    rtc_sdp_init(sdp);
    CHECK(rtc_sdp_parse(sdp, offer_sdp, strlen(offer_sdp), &err) == 0, "offer parses (%s)", err ? err : "");
    CHECK(sdp->nmedia == 1 && !strcmp(sdp->media[0].kind, "application"), "one application section");
    CHECK(!strcmp(sdp->media[0].mid, "0") && sdp->has_bundle && sdp->nbundle == 1, "mid and bundle");
    CHECK(!strcmp(sdp->media[0].ufrag, "EsAw") && !strcmp(sdp->media[0].pwd, "bP+XJMM09aR8AiX1jdukzR6Y"), "ice creds");
    CHECK(sdp->media[0].fp_alg == RTC_FP_SHA256 && sdp->media[0].fp_len == 32 && sdp->media[0].fp[0] == 0xD2, "fingerprint");
    CHECK(sdp->media[0].setup == RTC_SETUP_ACTPASS, "setup");
    CHECK(sdp->media[0].sctp_port == 5000 && sdp->media[0].max_msg == 262144, "sctp-port / max-message-size");
    CHECK(sdp->media[0].ncands == 3 && sdp->media[0].cands[2].addr.fam == 0, "3 candidates, mDNS one unresolved");
    CHECK(sdp->media[0].cands[1].type == RTC_CAND_SRFLX && sdp->media[0].cands[1].has_rel && sdp->media[0].cands[1].rel.port == 56143,
        "srflx with raddr/rport");
    CHECK(sdp->media[0].trickle, "trickle");
    CHECK(rtc_sdp_parse(sdp, "x=0\r\n", 5, &err) < 0, "must start with v=");
    CHECK(rtc_sdp_parse(sdp, "v=0\r\nm=audio\r\n", 14, &err) < 0, "short m= line rejected");
    CHECK(rtc_sdp_parse(sdp, "v=0\r\nbad line\r\n", 15, &err) < 0, "garbage line rejected");
    CHECK(rtc_sdp_parse(sdp, "v=0\r\na=fingerprint:sha-256 ZZ\r\n", 31, &err) < 0, "bad fingerprint rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp 2122260223 10.1.2.3 9 typ host", 48, &c) == 0, "candidate parses");
    rtc_addr_parse("10.1.2.3", 8, 9, &want);
    CHECK(rtc_addr_eq(&c.addr, &want) && c.prio == 2122260223u && c.comp == 1 && c.udp, "candidate fields");
    CHECK(rtc_sdp_write_candidate(&c, line, sizeof(line)) > 0, "candidate writes");
    CHECK(!strcmp(line, "candidate:1 1 udp 2122260223 10.1.2.3 9 typ host generation 0"), "candidate text %s", line);
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp x 10.1.2.3 9 typ host", 39, &c) < 0, "bad priority rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp 1 10.1.2.3 9 typ weird", 40, &c) < 0, "bad type rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp 1 10.1.2.3", 28, &c) < 0, "truncated rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:a=b 1 udp 1 10.1.2.3 9 typ host", 41, &c) < 0, "foundation outside ice-char rejected (RFC 8839 5.1)");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 sctp 1 10.1.2.3 9 typ host", 40, &c) < 0, "unknown transport rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 TCP 1 10.1.2.3 9 typ host tcptype passive", 55, &c) == 0 && !c.udp, "TCP transport, any case");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp 1 a\r\nb 9 typ host", 35, &c) < 0, "address with CR/LF rejected");
    CHECK(rtc_sdp_parse_candidate("candidate:1 1 udp 7 6f1c2a9e-1b2c.local 5123 typ host", strlen("candidate:1 1 udp 7 6f1c2a9e-1b2c.local 5123 typ host"), &c) == 0 && c.addr.fam == 0 &&
              c.addr.port == 5123 && !strcmp(c.host, "6f1c2a9e-1b2c.local"),
        "mDNS host keeps its name and port");
    CHECK(rtc_sdp_write_candidate(&c, line, sizeof(line)) > 0 && !strcmp(line, "candidate:1 1 udp 7 6f1c2a9e-1b2c.local 5123 typ host generation 0"),
        "mDNS candidate writes back its host name (%s)", line);
    rtc_sdp_free(sdp);
    free(sdp);
}


static uint64_t turn_now;
static uint64_t turn_sends[16];
static int turn_nsends, turn_fail_code;
static uint64_t turn_fail_at;

static void turn_rec_send(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)sock;
    (void)to;
    (void)p;
    (void)n;
    if (turn_nsends < 16)
        turn_sends[turn_nsends] = turn_now;
    turn_nsends++;
}

static void turn_rec_ev(void* ud, int idx, int ev, int code)
{
    (void)ud;
    (void)idx;
    if (ev == RTC_TURN_EV_FAILED && !turn_fail_at) {
        turn_fail_at = turn_now;
        turn_fail_code = code;
    }
}

static void turn_schedule(int kind)
{
    static const uint64_t want_ms[7] = { 0, 500, 1500, 3500, 7500, 15500, 31500 };
    rtc_turn* t = (rtc_turn*)calloc(1, sizeof(*t));
    rtc_rng rng;
    rtc_addr srv;
    uint8_t seed[32] = { 7 };
    int i, ok = 1;
    rtc_rng_seed(&rng, seed);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    turn_now = 1000000;
    turn_nsends = 0;
    turn_fail_at = 0;
    turn_fail_code = 0;
    rtc_turn_init(t, &rng, 0, kind, 0, &srv, "u", "p", turn_rec_send, turn_rec_ev, NULL);
    rtc_turn_start(t, turn_now);
    while (!turn_fail_at && turn_now < 1000000 + 120000000ull) {
        uint64_t nx = rtc_turn_tick(t, turn_now);
        turn_now = nx > turn_now ? nx : turn_now + 1000;
    }
    CHECK(turn_nsends == 7, "turn rto (kind %d): Rc = 7 transmissions, got %d", kind, turn_nsends);
    for (i = 0; i < 7 && i < turn_nsends; i++)
        if (turn_sends[i] - 1000000 != want_ms[i] * 1000)
            ok = 0;
    CHECK(ok, "turn rto (kind %d): sends at 0, .5, 1.5, 3.5, 7.5, 15.5, 31.5 s (RFC 8489 6.2.1)", kind);
    CHECK(turn_fail_at - 1000000 == 39500000ull, "turn rto (kind %d): fails after Rm*RTO = 8 s past the last send, at 39.5 s (got %llu us)", kind,
        (unsigned long long)(turn_fail_at - 1000000));
    CHECK(turn_fail_code == 701, "turn rto (kind %d): timeout is error 701 (%d)", kind, turn_fail_code);
    free(t);
}

static int tp_perm_reqs;
static uint8_t tp_tid[12], tp_perm_tid[12];

static void tp_send(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    rtc_stun_msg m;
    (void)ud;
    (void)sock;
    (void)to;
    if (rtc_stun_parse(&m, p, n) < 0)
        return;
    memcpy(tp_tid, m.tid, 12);
    if (rtc_stun_method(m.type) == RTC_STUN_CREATE_PERMISSION && rtc_stun_class(m.type) == RTC_STUN_REQUEST) {
        tp_perm_reqs++;
        memcpy(tp_perm_tid, m.tid, 12);
    }
}

static void tp_ev(void* ud, int idx, int ev, int code)
{
    (void)ud;
    (void)idx;
    (void)ev;
    (void)code;
}

static size_t tp_reply(uint8_t* buf, size_t cap, uint16_t method, uint16_t cls, const uint8_t* tid, int err, const rtc_addr* relay)
{
    rtc_stun_wr w;
    rtc_stun_wr_init(&w, buf, cap, rtc_stun_type(method, cls), tid);
    if (err)
        rtc_stun_wr_error(&w, err, "x");
    if (relay) {
        rtc_stun_wr_addr(&w, RTC_SA_XOR_RELAYED_ADDRESS, relay);
        rtc_stun_wr_u32(&w, RTC_SA_LIFETIME, 600);
    }
    rtc_stun_wr_fingerprint(&w);
    return rtc_stun_wr_done(&w);
}

static void test_turn_perms(void)
{
    rtc_turn* t = (rtc_turn*)calloc(1, sizeof(*t));
    rtc_rng rng;
    rtc_addr srv, relay, peer, from;
    uint8_t seed[32] = { 9 }, buf[512];
    const uint8_t* d = NULL;
    size_t n, dl = 0;
    uint64_t now = 1000000;
    int r, i;
    rtc_rng_seed(&rng, seed);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    rtc_addr_parse("10.0.0.1", 8, 50000, &relay);
    rtc_addr_parse("10.9.9.9", 8, 40000, &peer);
    tp_perm_reqs = 0;
    rtc_turn_init(t, &rng, 0, RTC_TURN_KIND_TURN, 0, &srv, "u", "p", tp_send, tp_ev, NULL);
    rtc_turn_start(t, now);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_ALLOCATE, RTC_STUN_SUCCESS, tp_tid, 0, &relay);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(t->state == RTC_TURN_READY, "turn perms: allocated (%d)", t->state);
    {
        rtc_stun_wr w;
        uint8_t tid[12] = { 1, 2, 3 };
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_DATA, RTC_STUN_INDICATION), tid);
        rtc_stun_wr_addr(&w, RTC_SA_XOR_PEER_ADDRESS, &peer);
        rtc_stun_wr_attr(&w, RTC_SA_DATA, "spoof", 5);
        n = rtc_stun_wr_done(&w);
        r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
        CHECK(r != 2, "turn perms: a Data indication from a peer without a permission is dropped (RFC 8656 12.6) (%d)", r);
    }
    CHECK(rtc_turn_send(t, &peer, (const uint8_t*)"hi", 2, now) == 0 && tp_perm_reqs == 1, "turn perms: first send asks for a permission");
    for (i = 0; i < 200 && t->perm[0].perm_pending; i++) {
        uint64_t nx = rtc_turn_tick(t, now);
        now = nx > now && nx != UINT64_MAX ? nx : now + 1000000;
    }
    CHECK(!t->perm[0].perm_pending && !t->perm[0].perm_ok && tp_perm_reqs == 7, "turn perms: unanswered CreatePermission gives up after Rc=7 (%d)", tp_perm_reqs);
    rtc_turn_send(t, &peer, (const uint8_t*)"hi", 2, now);
    CHECK(tp_perm_reqs == 8 && t->perm[0].perm_pending, "turn perms: the next send re-issues the permission (%d)", tp_perm_reqs);
    buf[0] = 0x50;
    buf[1] = 0x00;
    buf[2] = 0;
    buf[3] = 4;
    memset(buf + 4, 0, 4);
    CHECK(rtc_turn_input(t, buf, 8, now, &from, &d, &dl) == 0, "turn perms: 0x5000 is not a ChannelData channel (RFC 8656 0x4000-0x4FFF)");
    t->have_key = 1;
    memset(t->key, 0x5A, 16);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_CREATE_PERMISSION, RTC_STUN_ERROR, tp_perm_tid, 403, NULL);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(t->perm[0].perm_pending, "turn perms: an unauthenticated 403 is ignored once the key is known");
    rtc_turn_reset_perms(t);
    CHECK(!t->perm[0].used, "turn perms: reset frees every permission");
    free(t);
}

static int tc_chan_reqs;
static uint16_t tc_chan_num;
static uint8_t tc_chan_tid[12];

static void tc_send(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    rtc_stun_msg m;
    uint32_t v = 0;
    tp_send(ud, sock, to, p, n);
    if (rtc_stun_parse(&m, p, n) < 0)
        return;
    if (rtc_stun_method(m.type) == RTC_STUN_CHANNEL_BIND && rtc_stun_class(m.type) == RTC_STUN_REQUEST) {
        tc_chan_reqs++;
        memcpy(tc_chan_tid, m.tid, 12);
        rtc_stun_get_u32(&m, RTC_SA_CHANNEL_NUMBER, &v);
        tc_chan_num = (uint16_t)(v >> 16);
    }
}

static int tc_slot(const rtc_turn* t, const rtc_addr* peer)
{
    int i;
    for (i = 0; i < RTC_TURN_MAX_PERM; i++)
        if (t->perm[i].used && rtc_addr_eq(&t->perm[i].peer, peer))
            return i;
    return -1;
}

static uint16_t tc_bind(rtc_turn* t, const rtc_addr* peer, uint64_t now)
{
    uint8_t buf[256];
    rtc_addr from;
    const uint8_t* d = NULL;
    size_t n, dl = 0;
    int reqs = tc_chan_reqs;
    rtc_turn_send(t, peer, (const uint8_t*)"hi", 2, now);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_CREATE_PERMISSION, RTC_STUN_SUCCESS, tp_perm_tid, 0, NULL);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    if (tc_chan_reqs == reqs)
        return 0;
    n = tp_reply(buf, sizeof(buf), RTC_STUN_CHANNEL_BIND, RTC_STUN_SUCCESS, tc_chan_tid, 0, NULL);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    return tc_chan_num;
}

static uint64_t tc_idle(rtc_turn* t, const rtc_addr* peer, uint64_t now)
{
    uint64_t end = now + 400000000u;
    while (now < end && tc_slot(t, peer) >= 0) {
        rtc_turn_tick(t, now);
        now += 1000000;
    }
    return now;
}

static size_t tc_chandata(uint8_t* buf, uint16_t ch)
{
    rtc_wr16(buf, ch);
    rtc_wr16(buf + 2, 2);
    buf[4] = 'o';
    buf[5] = 'k';
    return 6;
}

static size_t tc_dataind(uint8_t* buf, size_t cap, const rtc_addr* peer)
{
    rtc_stun_wr w;
    uint8_t tid[12] = { 7, 7, 7 };
    rtc_stun_wr_init(&w, buf, cap, rtc_stun_type(RTC_STUN_DATA, RTC_STUN_INDICATION), tid);
    rtc_stun_wr_addr(&w, RTC_SA_XOR_PEER_ADDRESS, peer);
    rtc_stun_wr_attr(&w, RTC_SA_DATA, "ok", 2);
    return rtc_stun_wr_done(&w);
}

static void test_turn_channels(void)
{
    rtc_turn* t = (rtc_turn*)calloc(1, sizeof(*t));
    rtc_turn* s = (rtc_turn*)calloc(1, sizeof(*s));
    rtc_rng rng;
    rtc_addr srv, relay, pa, pb, pc, pd, from;
    uint8_t seed[32] = { 11 }, buf[512];
    const uint8_t* d = NULL;
    size_t n, dl = 0;
    uint64_t now = 1000000, hold;
    uint16_t ca, cb, cc, ca2;
    int r, ia, k;
    rtc_rng_seed(&rng, seed);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    rtc_addr_parse("10.0.0.1", 8, 50000, &relay);
    rtc_addr_parse("10.9.9.1", 8, 40001, &pa);
    rtc_addr_parse("10.9.9.2", 8, 40002, &pb);
    rtc_addr_parse("10.9.9.3", 8, 40003, &pc);
    rtc_addr_parse("10.9.9.4", 8, 40004, &pd);
    tc_chan_reqs = 0;
    rtc_turn_init(t, &rng, 0, RTC_TURN_KIND_TURN, 0, &srv, "u", "p", tc_send, tp_ev, NULL);
    rtc_turn_start(t, now);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_ALLOCATE, RTC_STUN_SUCCESS, tp_tid, 0, &relay);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(t->state == RTC_TURN_READY, "turn chan: allocated (%d)", t->state);
    ca = tc_bind(t, &pa, now);
    ia = tc_slot(t, &pa);
    CHECK(ca >= 0x4000 && ca <= 0x4FFF && ia >= 0 && t->perm[ia].chan_ok, "turn chan: peer A bound to channel 0x%04x", ca);
    n = tc_chandata(buf, ca);
    CHECK(rtc_turn_input(t, buf, n, now, &from, &d, &dl) == 2 && rtc_addr_eq(&from, &pa) && dl == 2, "turn chan: ChannelData on A's channel is relay data");
    n = tc_chandata(buf, (uint16_t)(ca ^ 0x0800));
    r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn demux (R2b-2): ChannelData on an unbound channel is left to other sessions (0), not consumed (%d)", r);
    n = tc_dataind(buf, sizeof(buf), &pb);
    r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn demux (R2b-2): a Data indication from a peer without a permission is not consumed (%d)", r);
    rtc_turn_init(s, &rng, 1, RTC_TURN_KIND_STUN, 0, &srv, "", "", tc_send, tp_ev, NULL);
    rtc_turn_start(s, now);
    n = tc_chandata(buf, ca);
    r = rtc_turn_input(s, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn demux (R2b-2): a STUN session on the same server leaves ChannelData to the TURN session (%d)", r);
    n = tc_dataind(buf, sizeof(buf), &pa);
    r = rtc_turn_input(s, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn demux (R2b-2): a STUN session on the same server leaves Data indications to the TURN session (%d)", r);
    buf[0] = 0x40;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x40;
    CHECK(rtc_turn_input(t, buf, 6, now, &from, &d, &dl) == 1, "turn demux: a truncated ChannelData is consumed as malformed");
    now = tc_idle(t, &pa, now);
    CHECK(tc_slot(t, &pa) < 0, "turn chan: an idle permission is freed after 300 s");
    cb = tc_bind(t, &pb, now);
    CHECK(cb && cb != ca, "turn chan (R2b-6, RFC 8656 12): a new peer after the idle free gets a different channel (0x%04x vs 0x%04x)", cb, ca);
    ca2 = tc_bind(t, &pa, now);
    CHECK(ca2 == ca, "turn chan (RFC 8656 12): peer A returning within the server's binding lifetime gets its old channel back (0x%04x vs 0x%04x)", ca2, ca);
    ia = tc_slot(t, &pa);
    now = tc_idle(t, &pa, now);
    t->chan_next = (uint16_t)(ca - 0x4000);
    cc = tc_bind(t, &pc, now);
    CHECK(cc && cc != ca && cc != cb, "turn chan: the counter skips a channel still held for an idle-freed peer (0x%04x)", cc);
    t->chan_next = (uint16_t)(cb - 0x4000);
    rtc_turn_send(t, &pa, (const uint8_t*)"x", 1, now);
    ia = tc_slot(t, &pa);
    CHECK(ia >= 0 && t->perm[ia].chan == ca, "turn chan: A gets its held channel again");
    for (k = 0; k < RTC_TURN_CHQ; k++)
        if (t->chq[k].until_us > now && rtc_addr_eq(&t->chq[k].peer, &pa))
            break;
    CHECK(k == RTC_TURN_CHQ, "turn chan: a held channel handed back to its peer leaves the hold list");
    rtc_turn_reset_perms(t);
    t->chan_next = 0x0800;
    rtc_turn_send(t, &pc, (const uint8_t*)"x", 1, now);
    ia = tc_slot(t, &pc);
    CHECK(ia >= 0 && t->perm[ia].chan == cc, "turn chan: reset_perms holds every bound channel; C gets its own back (0x%04x)", ia >= 0 ? t->perm[ia].chan : 0);
    rtc_turn_send(t, &pb, (const uint8_t*)"x", 1, now);
    ia = tc_slot(t, &pb);
    CHECK(ia >= 0 && t->perm[ia].chan == cb, "turn chan: B gets its held channel back after reset_perms (0x%04x)", cb);
    t->chan_next = (uint16_t)(ca - 0x4000);
    rtc_turn_send(t, &pd, (const uint8_t*)"x", 1, now);
    ia = tc_slot(t, &pd);
    CHECK(ia >= 0 && t->perm[ia].chan != cc && t->perm[ia].chan != cb && t->perm[ia].chan != ca,
        "turn chan: a new peer gets neither a live peer's channel nor one held for a peer that never re-bound it (0x%04x)", ia >= 0 ? t->perm[ia].chan : 0);
    {
        rtc_turn_chq save[RTC_TURN_CHQ];
        rtc_turn_reset_perms(t);
        memcpy(save, t->chq, sizeof(save));
        rtc_turn_send(t, &pd, (const uint8_t*)"x", 1, now);
        for (k = 0; k < RTC_TURN_CHQ; k++) {
            t->chq[k].until_us = now + 1000000000ull + (uint64_t)k;
            t->chq[k].chan = (uint16_t)(0x4100 + k);
            t->chq[k].peer = pd;
            t->chq[k].peer.port = (uint16_t)(1000 + k);
        }
        rtc_turn_reset_perms(t);
        for (k = 0; k < RTC_TURN_CHQ; k++)
            if (t->chq[k].chan != 0x4100 + k)
                break;
        CHECK(k == RTC_TURN_CHQ, "turn chan: a peer that never sent a ChannelBind takes no hold-list slot");
        memcpy(t->chq, save, sizeof(save));
    }
    {
        rtc_turn_tx txs[RTC_TURN_MAX_TX];
        memcpy(txs, t->tx, sizeof(txs));
        for (k = 0; k < RTC_TURN_MAX_TX; k++)
            t->tx[k].kind = RTC_TX_REFRESH;
        t->chan_next = 0x0900;
        CHECK(rtc_turn_send(t, &pa, (const uint8_t*)"x", 1, now) < 0 && tc_slot(t, &pa) < 0, "turn chan: no free transaction slot -> the send fails");
        memcpy(t->tx, txs, sizeof(txs));
        rtc_turn_send(t, &pa, (const uint8_t*)"x", 1, now);
        ia = tc_slot(t, &pa);
        CHECK(ia >= 0 && t->perm[ia].chan == ca, "turn chan: a failed permission start puts A's held channel back (0x%04x)", ia >= 0 ? t->perm[ia].chan : 0);
        rtc_turn_reset_perms(t);
    }
    rtc_turn_reset_perms(t);
    now += 1000000000ull;
    t->chan_next = (uint16_t)(ca - 0x4000);
    rtc_turn_send(t, &pb, (const uint8_t*)"x", 1, now);
    ia = tc_slot(t, &pb);
    CHECK(ia >= 0 && t->perm[ia].chan == ca, "turn chan: after bind + 900 s (10 min lifetime + 5 min) a channel is free again (0x%04x)", ia >= 0 ? t->perm[ia].chan : 0);
    rtc_turn_reset_perms(t);
    {
        int reqs = tc_chan_reqs, k;
        uint64_t t0;
        rtc_turn_send(t, &pa, (const uint8_t*)"x", 1, now);
        n = tp_reply(buf, sizeof(buf), RTC_STUN_CREATE_PERMISSION, RTC_STUN_SUCCESS, tp_perm_tid, 0, NULL);
        rtc_turn_input(t, buf, n, now, &from, &d, &dl);
        t0 = now;
        for (k = 0; k < 4; k++) {
            uint64_t nx = rtc_turn_tick(t, now);
            now = nx > now && nx != UINT64_MAX ? nx : now + 1000000;
        }
        rtc_turn_tick(t, now);
        ia = tc_slot(t, &pa);
        hold = ia >= 0 ? t->perm[ia].chan_hold_us : 0;
        CHECK(tc_chan_reqs > reqs + 1 && now > t0 && hold > t0 + 900000000u && hold <= now + 900000000u,
            "turn chan: a ChannelBind retransmit moves the hold to its own send time + 900 s (%d sends)", tc_chan_reqs - reqs);
    }
    free(s);
    free(t);
}

static void test_turn_chan_early(void)
{
    rtc_turn* t = (rtc_turn*)calloc(1, sizeof(*t));
    rtc_rng rng;
    rtc_addr srv, relay, pe, from;
    uint8_t seed[32] = { 12 }, buf[512];
    const uint8_t* d = NULL;
    size_t n, dl = 0;
    uint64_t now = 1000000;
    uint16_t ch;
    int i, r, reqs;
    rtc_rng_seed(&rng, seed);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    rtc_addr_parse("10.0.0.1", 8, 50000, &relay);
    rtc_addr_parse("10.9.9.5", 8, 40005, &pe);
    rtc_turn_init(t, &rng, 0, RTC_TURN_KIND_TURN, 0, &srv, "u", "p", tc_send, tp_ev, NULL);
    rtc_turn_start(t, now);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_ALLOCATE, RTC_STUN_SUCCESS, tp_tid, 0, &relay);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(t->state == RTC_TURN_READY, "turn chan early: allocated (%d)", t->state);
    reqs = tc_chan_reqs;
    rtc_turn_send(t, &pe, (const uint8_t*)"hi", 2, now);
    i = tc_slot(t, &pe);
    ch = i >= 0 ? t->perm[i].chan : 0;
    n = tc_chandata(buf, ch);
    r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn chan early: before the ChannelBind request is sent the channel is not ours (%d)", r);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_CREATE_PERMISSION, RTC_STUN_SUCCESS, tp_perm_tid, 0, NULL);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(tc_chan_reqs == reqs + 1 && tc_chan_num == ch && i >= 0 && t->perm[i].chan_pending && !t->perm[i].chan_ok,
        "turn chan early: the ChannelBind request for 0x%04x is in flight", ch);
    n = tc_chandata(buf, ch);
    r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(r == 2 && rtc_addr_eq(&from, &pe) && dl == 2 && !memcmp(d, "ok", 2),
        "turn chan early (RFC 8656 12.1, DEC-a ASan stall): ChannelData that overtakes the ChannelBind success response is relay data (%d)", r);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_CHANNEL_BIND, RTC_STUN_ERROR, tc_chan_tid, 400, NULL);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(!t->perm[i].chan_pending && !t->perm[i].chan_ok, "turn chan early: a ChannelBind error ends the pending bind");
    n = tc_chandata(buf, ch);
    r = rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(r == 0, "turn chan early: after a failed bind the channel is no longer accepted (%d)", r);
    free(t);
}

static void test_turn_idle_clock(void)
{
    rtc_turn* t = (rtc_turn*)calloc(1, sizeof(*t));
    rtc_rng rng;
    rtc_addr srv, relay, pe, from;
    uint8_t seed[32] = { 13 }, buf[512];
    const uint8_t* d = NULL;
    size_t n, dl = 0;
    uint64_t now = 5000000;
    rtc_rng_seed(&rng, seed);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    rtc_addr_parse("10.0.0.1", 8, 50000, &relay);
    rtc_addr_parse("10.9.9.6", 8, 40006, &pe);
    rtc_turn_init(t, &rng, 0, RTC_TURN_KIND_TURN, 0, &srv, "u", "p", tc_send, tp_ev, NULL);
    rtc_turn_start(t, now);
    n = tp_reply(buf, sizeof(buf), RTC_STUN_ALLOCATE, RTC_STUN_SUCCESS, tp_tid, 0, &relay);
    rtc_turn_input(t, buf, n, now, &from, &d, &dl);
    CHECK(tc_bind(t, &pe, now) != 0, "turn idle clock: peer bound");
    rtc_turn_send(t, &pe, (const uint8_t*)"x", 1, now + 7);
    rtc_turn_tick(t, now);
    CHECK(tc_slot(t, &pe) >= 0,
        "turn idle clock (DEC-a ASan stall): a send stamped by a fresher clock than the tick's own now is not 2^64 us idle; the permission stays");
    rtc_turn_tick(t, now + 8 + 300000000u);
    CHECK(tc_slot(t, &pe) < 0, "turn idle clock: more than 300 s idle frees it");
    free(t);
}



static void test_dcep(void)
{
    static const uint8_t open_rtx[] = { 0x03, 0x81, 0x01, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00, 0x02, 'c', 'h', 'n', 'p', 'r' };
    static const uint8_t open_timed[] = { 0x03, 0x02, 0x02, 0x00, 0x00, 0x01, 0x86, 0xa0, 0x00, 0x00, 0x00, 0x00 };
    uint8_t out[64];
    rtc_dcep_msg m;
    size_t n;
    CHECK(rtc_dcep_parse(open_rtx, sizeof(open_rtx), &m) == 0, "dcep: RFC 8832 5.1 OPEN parses");
    CHECK(m.type == RTC_DCEP_OPEN && !m.ordered && m.max_rtx == 5 && m.max_life == -1 && m.priority == 256, "dcep: unordered rexmit 5");
    CHECK(m.label_len == 3 && !memcmp(m.label, "chn", 3) && m.protocol_len == 2 && !memcmp(m.protocol, "pr", 2), "dcep: label/protocol");
    CHECK(rtc_dcep_parse(open_timed, sizeof(open_timed), &m) == 0 && m.ordered && m.max_life == 100000 && m.max_rtx == -1 && m.priority == 512,
        "dcep: ordered timed 100000 ms");
    n = rtc_dcep_write_open(out, sizeof(out), 0, 5, -1, 256, "chn", 3, "pr", 2);
    CHECK(n == sizeof(open_rtx) && !memcmp(out, open_rtx, n), "dcep: writer emits the RFC layout byte for byte");
    n = rtc_dcep_write_open(out, sizeof(out), 1, -1, 100000, 512, "", 0, "", 0);
    CHECK(n == sizeof(open_timed) && !memcmp(out, open_timed, n), "dcep: timed writer");
    CHECK(rtc_dcep_write_open(out, 13, 1, -1, -1, 256, "chn", 3, "", 0) == 0, "dcep: writer refuses a short buffer");
    CHECK(rtc_dcep_parse((const uint8_t*)"\x02", 1, &m) == 0 && m.type == RTC_DCEP_ACK, "dcep: ACK");
    CHECK(rtc_dcep_parse(open_rtx, 11, &m) < 0, "dcep: truncated header rejected");
    CHECK(rtc_dcep_parse(open_rtx, sizeof(open_rtx) - 1, &m) < 0, "dcep: label/protocol past the end rejected");
    CHECK(rtc_dcep_parse((const uint8_t*)"", 0, &m) < 0, "dcep: empty rejected");
    CHECK(rtc_dcep_parse((const uint8_t*)"\x04", 1, &m) < 0, "dcep: unknown message type rejected");
    {
        uint8_t bad[12];
        memcpy(bad, open_timed, 12);
        bad[1] = 0x03;
        CHECK(rtc_dcep_parse(bad, 12, &m) < 0, "dcep: unknown channel type rejected");
        bad[1] = 0x01;
        bad[4] = 0xff;
        CHECK(rtc_dcep_parse(bad, 12, &m) == 0 && m.max_rtx == 65535, "dcep: rexmit clamps to 65535");
    }
}


static uint8_t i420_buf[512];
static size_t i420_len;

static void i420_send(void* ud, const rtc_cand* local, const rtc_addr* to, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)local;
    (void)to;
    if (n <= sizeof(i420_buf)) {
        memcpy(i420_buf, p, n);
        i420_len = n;
    }
}

static void i420_ev(void* ud, int ev)
{
    (void)ud;
    (void)ev;
}

static int i420_round(rtc_ice* ice, const rtc_addr* from, uint16_t extra, rtc_stun_msg* rsp)
{
    uint8_t req[256], tid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    rtc_stun_wr w;
    rtc_stun_msg m;
    size_t n;
    rtc_stun_wr_init(&w, req, sizeof(req), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), tid);
    rtc_stun_wr_attr(&w, RTC_SA_USERNAME, "LUFR:RUFR", 9);
    rtc_stun_wr_u32(&w, RTC_SA_PRIORITY, 12345);
    rtc_stun_wr_u64(&w, RTC_SA_ICE_CONTROLLING, 77);
    if (extra) {
        rtc_stun_wr_attr(&w, extra, "abcd", 4);
        rtc_stun_wr_attr(&w, 0xC057, "zz", 2);
    }
    rtc_stun_wr_integrity(&w, (const uint8_t*)"localpasswordlocalpassword", 26);
    rtc_stun_wr_fingerprint(&w);
    n = rtc_stun_wr_done(&w);
    i420_len = 0;
    if (rtc_stun_parse(&m, req, n) < 0)
        return -1;
    rtc_ice_input(ice, 0, from, &m, 2000000);
    return i420_len && rtc_stun_parse(rsp, i420_buf, i420_len) == 0 ? 0 : -1;
}

static void test_ice_unknown_attr(void)
{
    rtc_ice* ice = (rtc_ice*)calloc(1, sizeof(*ice));
    rtc_rng rng;
    uint8_t seed[32] = { 3 };
    rtc_cand l;
    rtc_addr from;
    rtc_stun_msg rsp;
    const rtc_stun_attr* ua;
    rtc_rng_seed(&rng, seed);
    rtc_ice_init(ice, &rng, 0, i420_send, i420_ev, NULL);
    rtc_ice_set_local_creds(ice, "LUFR", "localpasswordlocalpassword");
    rtc_ice_set_remote_creds(ice, "RUFR", "remotepasswordremotepasswd");
    memset(&l, 0, sizeof(l));
    l.type = RTC_CAND_HOST;
    l.comp = 1;
    rtc_addr_parse("127.0.0.1", 9, 40000, &l.addr);
    l.base = l.addr;
    l.prio = rtc_ice_cand_prio(RTC_CAND_HOST, 65535, 1);
    rtc_ice_add_local(ice, &l);
    rtc_addr_parse("127.0.0.1", 9, 40001, &from);
    rtc_ice_start(ice, 1000000);
    CHECK(i420_round(ice, &from, 0x0033, &rsp) == 0, "stun 420: a response was sent");
    CHECK(rtc_stun_class(rsp.type) == RTC_STUN_ERROR && rtc_stun_error_code(&rsp) == 420, "stun 420: unknown comprehension-required attribute -> 420 (RFC 8489 6.3.1)");
    ua = rtc_stun_find(&rsp, RTC_SA_UNKNOWN_ATTRIBUTES);
    CHECK(ua && ua->len == 2 && rtc_rd16(rsp.buf + ua->off) == 0x0033, "stun 420: UNKNOWN-ATTRIBUTES lists exactly 0x0033, not the optional 0xC057");
    CHECK(rtc_stun_check_integrity(&rsp, (const uint8_t*)"localpasswordlocalpassword", 26) && rtc_stun_check_fingerprint(&rsp),
        "stun 420: the error is authenticated with the local password");
    CHECK(i420_round(ice, &from, 0, &rsp) == 0 && rtc_stun_class(rsp.type) == RTC_STUN_SUCCESS, "stun 420: the same request without it succeeds");
    rtc_ice_close(ice);
    free(ice);
}

int main(void)
{
    test_stun();
    test_crc();
    test_sdp();
    test_dcep();
    test_ice();
    test_ice_auth_replies();
    test_ice_prflx_local();
    test_ice_consent_v2();
    test_ice_tid_rtt();
    test_ice_role_multi();
    test_ice_tid_reuse_and_role();
    test_ice_restart_prev();
    test_ice_restart_prev_rx();
    test_ice_restart_consent();
    test_ice_487_tid();
    test_ice_stage_local();
    test_ice_local_reuse();
    test_ice_after_nomination();
    test_ice_symmetry_local();
    test_ice_relay_only_pairs();
    test_ice_unknown_attr();
    turn_schedule(RTC_TURN_KIND_STUN);
    turn_schedule(RTC_TURN_KIND_TURN);
    sctp_case("clean ordered", 1, 0, 0, 0, 3000, 2000, 0, 0);
    sctp_case("lossy ordered", 2, 10, 5, 10, 2000, 5000, 0, 0);
    sctp_case("lossy unordered", 3, 15, 5, 20, 2000, 3000, 1, 0);
    sctp_case("large messages", 4, 5, 2, 5, 24, 1u << 20, 0, 0);
    sctp_case("init collision", 6, 5, 0, 0, 300, 1500, 0, 1);
    test_sctp_stash_slots();
    test_sctp_stash_oversize();
    test_sctp_gap_fill_sack();
    test_sctp_partial();
    test_sctp_lifetime();
    test_sctp_reset();
    test_sctp_malformed();
    test_sctp_big_cookie();
    test_sctp_streams();
    test_sctp_empty_and_slots();
    test_sctp_gap_blocks();
    test_sctp_fwd_far();
    test_sctp_early_delivery();
    test_sctp_shutdown_received();
    test_sctp_ssn_sync();
    test_sctp_early_linear();
    test_sctp_early_bounds();
    test_sctp_init_in_shutdown();
    test_sctp_heartbeat();
    test_sctp_shrink();
    test_sctp_reconfig_results();
    test_sctp_fwd_many_streams();
    test_sctp_unknown_chunks();
    test_sctp_zero_checksum();
    test_addr_host_ok();
    test_rng_chacha20();
    test_turn_perms();
    test_turn_channels();
    test_turn_chan_early();
    test_turn_idle_clock();
    printf("test_rtc_core: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
