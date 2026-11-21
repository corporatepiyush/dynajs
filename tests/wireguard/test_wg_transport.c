/*
 * test_wg_transport.c -- dyna:wireguard S13b1 (src/modules/wireguard/wg-transport.c, wg-timers.c, wg-table.c and the
 * per-thread AEAD context in wg-crypto.c), linked alone with OpenSSL and src/core/dyn-pool.c. Rules come from the
 * WireGuard whitepaper section 6 and wireguard-go device/{constants,timers,send,receive,noise-protocol}.go.
 *  1. Data messages: padding (multiple of 16, clamped to the MTU), seal/open round trip 0..1500 bytes against an
 *     independent AEAD construction, header layout, short/wrong-type/over-limit/undersized-buffer refusals, tampered
 *     counter or body refused; the receiver index is not authenticated (it only selects the key).
 *  2. Keypair limits: Reject-After-Time 180 s and Reject-After-Messages for sending, receive age, Rekey-After-Time
 *     120 s (initiator only), Rekey-After-Messages 2^60, the 165 s last-minute receive rekey.
 *     Replay peek (no marking) predicts the marking check over 200000 counters.
 *  3. Keypair slots previous/current/next: initiator rotation, responder parks in next until the first authenticated
 *     packet on it, every keypair dropped exactly once (refcount model over 20000 random events).
 *  4. Timers: retransmit after 5 s + jitter (< 334 ms), give up after 18 retries with zeroing in 540 s, handshake
 *     rate limit 5 s, new handshake 15 s after unanswered data, keepalive 10 s after data (and once more if more data
 *     came), zeroing 540 s after a session, persistent keepalive, next-deadline.
 *  5. Index table (random 32-bit receiver indices): 300000 random put/get/del against a model, clustered keys, growth.
 *  6. Public-key table: 65536 peers, put/get/del against a model.
 *  7. Allowed IPs: longest-prefix match differential against brute force for IPv4 and IPv6 (clustered prefixes force
 *     splits), removal, removal of a peer's every prefix, node count back to 0.
 *  8. Batches on the real lock-free pool: 4 workers, two peers, out-of-order completion, per-peer FIFO release keeps
 *     send order, every packet opens on the other side with the counter it was sealed with.
 *  9. Fuzz: 300000 inputs through src/fuzz/fuzz_wireguard.c (handshake, cookie, data parsers and the tables).
 * 10. Seeded properties (tests/lib/dyn-fuzzkit.h): timers-model: 400 runs x 400 random events (initiate, complete, derive,
 *     data sent/received, auth, traversal, time jumps to and around deadlines, 30% with the path down) against an
 *     independent model of the whitepaper timers, actions and next deadline after every event; slots-model: random
 *     begin/receive/clear against the wireguard-go rotation model with exact drop counts; data-garble: padding against
 *     the wireguard-go formula over random lengths and MTUs, seal refusal at the counter limit and short buffers, open
 *     after corruption/truncation/extension/reserved-byte rewrites (the receiver index is unauthenticated);
 *     tables-reseeded: the replay-peek, index, public-key and allowed-IPs model tests above re-run on the run seed.
 *     Equivalent mutant: dropping the previous slot on promotion (previous is always empty while next is parked).
 */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/dyn-pool.h"
#include "../lib/dyn-fuzzkit.h"
#include "../../src/modules/wireguard/wg-internal.h"

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
static void fill(uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
}

static void t_data(void)
{
    static uint8_t buf[1600], ref[1600], pt[1600], rc[1700];
    uint8_t key[32];
    uint32_t rx;
    uint64_t ctr;
    size_t n;
    int bad = 0, r;
    CHECK(wg_pad_len(0, 1420) == 0, "keepalive pads to 0");
    CHECK(wg_pad_len(1, 1420) == 16 && wg_pad_len(16, 1420) == 16 && wg_pad_len(17, 1420) == 32, "pad to 16");
    CHECK(wg_pad_len(1419, 1420) == 1420 && wg_pad_len(1420, 1420) == 1420, "pad clamps to the MTU");
    CHECK(wg_pad_len(1421, 1420) == 1436, "over-MTU tail pads by its own remainder");
    CHECK(wg_pad_len(17, 0) == 32 && wg_pad_len(0, 0) == 0, "MTU 0 pads to 16");
    fill(key, 32);
    for (n = 0; n <= 1500; n++) {
        size_t padded = wg_pad_len(n, 1420);
        fill(pt, n);
        memcpy(buf + 16, pt, n);
        ctr = rnd() % 100000;
        r = wg_data_seal(key, 0xa1b2c3d4u, ctr, buf, n, sizeof buf, 1420);
        if (r != (int)(padded + 32)) {
            bad++;
            continue;
        }
        memset(ref, 0, sizeof ref);
        memcpy(ref, pt, n);
        wg_aead_seal(key, ctr, ref, padded, NULL, 0, rc);
        if (memcmp(buf + 16, rc, padded + 16) != 0 || wg_le32(buf) != 4 || wg_le32(buf + 4) != 0xa1b2c3d4u || wg_le64(buf + 8) != ctr)
            bad++;
        if (wg_data_parse(buf, (size_t)r, &rx, &ctr) < 0 || rx != 0xa1b2c3d4u)
            bad++;
        if (wg_data_open(key, buf, (size_t)r) != (int)padded || memcmp(buf + 16, pt, n) != 0)
            bad++;
    }
    CHECK(bad == 0, "data seal/open 0..1500 bytes matches the AEAD construction");
    memcpy(buf + 16, pt, 100);
    r = wg_data_seal(key, 5, 77, buf, 100, sizeof buf, 1420);
    memcpy(ref, buf, (size_t)r);
    buf[8] ^= 1;
    CHECK(wg_data_open(key, buf, (size_t)r) < 0, "tampered counter refused");
    memcpy(buf, ref, (size_t)r);
    buf[40] ^= 0x80;
    CHECK(wg_data_open(key, buf, (size_t)r) < 0, "tampered body refused");
    memcpy(buf, ref, (size_t)r);
    buf[(size_t)r - 1] ^= 1;
    CHECK(wg_data_open(key, buf, (size_t)r) < 0, "tampered tag refused");
    memcpy(buf, ref, (size_t)r);
    buf[5] ^= 1;
    CHECK(wg_data_open(key, buf, (size_t)r) >= 0, "receiver index is not authenticated");
    memcpy(buf, ref, (size_t)r);
    buf[0] = 3;
    CHECK(wg_data_open(key, buf, (size_t)r) < 0 && wg_data_parse(buf, (size_t)r, &rx, &ctr) < 0, "wrong type refused");
    memcpy(buf, ref, (size_t)r);
    buf[1] = 1;
    CHECK(wg_data_parse(buf, (size_t)r, &rx, &ctr) < 0, "reserved bytes must be zero");
    CHECK(wg_data_parse(ref, 31, &rx, &ctr) < 0, "shorter than 32 refused");
    CHECK(wg_data_seal(key, 1, 0, buf, 0, sizeof buf, 1420) == 32 && wg_data_open(key, buf, 32) == 0, "keepalive is 32 bytes");
    CHECK(wg_data_seal(key, 1, WG_REJECT_AFTER_MESSAGES, buf, 10, sizeof buf, 1420) < 0, "counter at the limit refused");
    CHECK(wg_data_seal(key, 1, 1, buf, 100, 16 + 112 + 15, 1420) < 0 && wg_data_seal(key, 1, 1, buf, 100, 16 + 112 + 16, 1420) == 144, "buffer must hold header, padding and tag");
    memset(ref, 0, 48);
    wg_put_le32(ref, 4);
    wg_put_le64(ref + 8, WG_REJECT_AFTER_MESSAGES);
    wg_aead_seal(key, WG_REJECT_AFTER_MESSAGES, ref + 16, 16, NULL, 0, ref + 16);
    memcpy(buf, ref, 48);
    wg_put_le64(buf + 8, WG_REJECT_AFTER_MESSAGES - 1);
    wg_aead_seal(key, WG_REJECT_AFTER_MESSAGES - 1, buf + 16, 16, NULL, 0, buf + 16);
    CHECK(wg_data_open(key, buf, 48) == 16, "last counter below the limit opens");
    CHECK(wg_data_open(key, ref, 48) < 0, "authentic message at the counter limit refused");
}

static void t_kp(void)
{
    wg_keypair_t k;
    memset(&k, 0, sizeof k);
    k.created_ms = 1000;
    CHECK(wg_kp_can_send(&k, 1000 + 179999) && !wg_kp_can_send(&k, 1000 + 180000), "Reject-After-Time 180 s for sending");
    CHECK(wg_kp_can_recv(&k, 1000 + 180000) && !wg_kp_can_recv(&k, 1000 + 180001), "receive refused after 180 s");
    CHECK(!wg_kp_can_send(NULL, 0) && !wg_kp_can_recv(NULL, 0), "no keypair");
    k.send_ctr = WG_REJECT_AFTER_MESSAGES - 1;
    CHECK(wg_kp_can_send(&k, 1000), "last counter usable");
    k.send_ctr = WG_REJECT_AFTER_MESSAGES;
    CHECK(!wg_kp_can_send(&k, 1000), "Reject-After-Messages");
    k.send_ctr = WG_REKEY_AFTER_MESSAGES;
    CHECK(!wg_kp_fresh_send(&k, 1000), "rekey not before 2^60 messages");
    k.send_ctr++;
    CHECK(wg_kp_fresh_send(&k, 1000), "rekey after 2^60 messages (either side)");
    k.send_ctr = 0;
    CHECK(!wg_kp_fresh_send(&k, 1000 + 120001), "responder never rekeys by time on send");
    k.initiator = 1;
    CHECK(!wg_kp_fresh_send(&k, 1000 + 120000) && wg_kp_fresh_send(&k, 1000 + 120001), "initiator rekeys after 120 s");
    CHECK(!wg_kp_fresh_recv(&k, 1000 + 165000) && wg_kp_fresh_recv(&k, 1000 + 165001), "initiator last-minute rekey after 165 s");
    k.initiator = 0;
    CHECK(!wg_kp_fresh_recv(&k, 1000 + 170000), "responder no last-minute rekey");
}

static void t_peek(void)
{
    wg_replay_t r;
    uint64_t hi = 0;
    int i, bad = 0;
    memset(&r, 0, sizeof r);
    for (i = 0; i < 200000; i++) {
        uint64_t c = rnd() % 3 == 0 ? hi + rnd() % 100 : (hi > 3000 ? hi - rnd() % 3000 : rnd() % 3000);
        int p1 = wg_replay_peek(&r, c), p2 = wg_replay_peek(&r, c), k = wg_replay_check(&r, c);
        if (p1 != p2 || p1 != k || (k == 0 && wg_replay_peek(&r, c) == 0))
            bad++;
        if (k == 0 && c > hi)
            hi = c;
    }
    CHECK(bad == 0, "replay peek predicts check and does not mark");
    CHECK(wg_replay_peek(&r, WG_REJECT_AFTER_MESSAGES) < 0, "peek refuses the counter limit");
}

static int dropped[20000];
static void on_drop(void* ud, wg_keypair_t* kp)
{
    (void)ud;
    dropped[kp->local_index]++;
}

static void t_slots(void)
{
    static wg_keypair_t kp[20000];
    wg_slots_t s;
    int i, bad = 0, used = 0, live = 0;
    memset(&s, 0, sizeof s);
    for (i = 0; i < 20000; i++)
        kp[i].local_index = (uint32_t)i;
    kp[0].initiator = 1;
    wg_slots_begin(&s, &kp[0], on_drop, NULL);
    CHECK(s.cur == &kp[0] && !s.prev && !s.next, "initiator keypair goes to current");
    kp[1].initiator = 1;
    wg_slots_begin(&s, &kp[1], on_drop, NULL);
    CHECK(s.cur == &kp[1] && s.prev == &kp[0] && !s.next, "current moves to previous");
    wg_slots_begin(&s, &kp[2], on_drop, NULL);
    CHECK(s.cur == &kp[1] && s.next == &kp[2] && !s.prev && dropped[0] == 1, "responder parks in next, previous dropped");
    CHECK(!wg_slots_received(&s, &kp[1], on_drop, NULL) && s.next == &kp[2], "packet on current does not promote");
    CHECK(wg_slots_received(&s, &kp[2], on_drop, NULL) && s.cur == &kp[2] && s.prev == &kp[1] && !s.next, "first packet on next promotes it");
    wg_slots_begin(&s, &kp[3], on_drop, NULL);
    kp[4].initiator = 1;
    wg_slots_begin(&s, &kp[4], on_drop, NULL);
    CHECK(s.cur == &kp[4] && s.prev == &kp[3] && !s.next && dropped[2] == 1 && dropped[1] == 1, "initiator with a pending next: next becomes previous, current dropped");
    wg_slots_clear(&s, on_drop, NULL);
    CHECK(!s.prev && !s.cur && !s.next && dropped[3] == 1 && dropped[4] == 1, "clear drops all");
    memset(dropped, 0, sizeof dropped);
    for (i = 0; i < 20000; i++) {
        unsigned r = (unsigned)(rnd() % 4);
        int j, nslots;
        if (r < 2 && used < 20000) {
            kp[used].initiator = (uint8_t)r;
            wg_slots_begin(&s, &kp[used++], on_drop, NULL);
        } else if (r == 2 && s.next)
            wg_slots_received(&s, s.next, on_drop, NULL);
        if ((s.prev && (s.prev == s.cur || s.prev == s.next)) || (s.cur && s.cur == s.next))
            bad++;
        if (i % 97 == 0) {
            live = 0;
            for (j = 0; j < used; j++) {
                if (dropped[j] > 1)
                    bad++;
                live += !dropped[j];
            }
            nslots = (s.prev != NULL) + (s.cur != NULL) + (s.next != NULL);
            if (live != nslots)
                bad++;
        }
    }
    wg_slots_clear(&s, on_drop, NULL);
    for (i = 0; i < used; i++)
        if (dropped[i] != 1)
            bad++;
    CHECK(bad == 0 && used > 5000, "every keypair dropped exactly once, live keypairs equal filled slots");
}

static void t_timers
(void)
{
    wg_timers_t t;
    uint64_t now = 1000;
    unsigned a, i, sends = 0;
    memset(&t, 0, sizeof t);
    wg_timers_start(&t, now);
    CHECK(wg_timers_hs_allowed(&t, now), "first handshake allowed");
    wg_timers_hs_initiated(&t, now, 333, 0);
    CHECK(!wg_timers_hs_allowed(&t, now + 4999) && wg_timers_hs_allowed(&t, now + 5000), "handshake rate limit 5 s");
    CHECK(wg_timers_next(&t) == now + 5333, "retransmit at 5 s + jitter");
    CHECK(wg_timers_due(&t, now + 5332) == 0, "not before");
    a = wg_timers_due(&t, now + 5333);
    CHECK(a == (WG_ACT_SEND_INIT | WG_ACT_RETRY) && t.attempts == 1, "retransmit fires as a retry");
    wg_timers_hs_initiated(&t, now, 334, 0);
    CHECK(t.retransmit_at == now + 5000 && t.attempts == 0, "jitter is below 334 ms; a fresh attempt resets the count");
    now = 10000;
    for (i = 0; i < 40; i++) {
        uint64_t at = wg_timers_next(&t);
        if (!at)
            break;
        now = at;
        a = wg_timers_due(&t, now);
        if (a & WG_ACT_SEND_INIT) {
            sends++;
            wg_timers_hs_initiated(&t, now, 0, 1);
        }
        if (a & WG_ACT_GIVE_UP)
            break;
    }
    CHECK(sends == 19 && (a & WG_ACT_GIVE_UP), "19 retries, the 20th timeout gives up");
    CHECK(t.zero_at == now + 540000, "give-up schedules zeroing at 540 s");
    a = wg_timers_due(&t, now + 540000);
    CHECK(a == WG_ACT_ZERO_KEYS, "zero keys fires");
    memset(&t, 0, sizeof t);
    now = 1000;
    wg_timers_hs_initiated(&t, now, 0, 0);
    wg_timers_hs_complete(&t, now + 50);
    CHECK(t.retransmit_at == 0 && t.last_hs_done_ms == now + 50, "complete stops retransmit");
    wg_timers_session_derived(&t, now);
    CHECK(t.zero_at == now + 540000, "session derived arms zeroing at 540 s");
    wg_timers_data_sent(&t, now, 100);
    CHECK(t.new_hs_at == now + 15100, "unanswered data: new handshake at 15 s + jitter");
    wg_timers_data_sent(&t, now + 5000, 0);
    CHECK(t.new_hs_at == now + 15100, "pending new-handshake timer not pushed back");
    wg_timers_auth_recv(&t);
    CHECK(t.new_hs_at == 0, "any authenticated packet cancels it");
    wg_timers_data_sent(&t, now, 0);
    CHECK(wg_timers_due(&t, now + 15000) == WG_ACT_SEND_INIT, "new handshake fires as a first attempt");
    wg_timers_data_recv(&t, now);
    CHECK(t.keepalive_at == now + 10000, "keepalive 10 s after data");
    wg_timers_data_recv(&t, now + 3000);
    CHECK(t.keepalive_at == now + 10000 && t.need_keepalive, "more data marks another keepalive");
    CHECK(wg_timers_due(&t, now + 10000) == WG_ACT_SEND_KEEPALIVE && t.keepalive_at == now + 20000, "keepalive re-armed once");
    CHECK(wg_timers_due(&t, now + 20000) == WG_ACT_SEND_KEEPALIVE && t.keepalive_at == 0, "then stops");
    wg_timers_data_recv(&t, now);
    wg_timers_auth_sent(&t);
    CHECK(t.keepalive_at == 0, "any authenticated send cancels the keepalive");
    memset(&t, 0, sizeof t);
    t.persist_s = 25;
    wg_timers_start(&t, now);
    CHECK(t.persist_s == 25, "start keeps the persistent interval");
    wg_timers_traversal(&t, now);
    CHECK(t.persist_at == now + 25000 && wg_timers_due(&t, now + 25000) == WG_ACT_SEND_KEEPALIVE, "persistent keepalive");
    t.persist_s = 0;
    wg_timers_traversal(&t, now);
    CHECK(wg_timers_next(&t) == 0, "no timers pending");
    memset(&t, 0, sizeof t);
    wg_timers_data_recv(&t, 0);
    CHECK(t.keepalive_at != 0, "deadline at time 0 still pending");
}

static void t_idx(void)
{
    wg_idx_t t;
    enum { N = 100000 };
    static uint32_t keys[N];
    static uint8_t live[N];
    uint32_t k;
    int i, bad = 0, n = 0;
    CHECK(wg_idx_init(&t, 4) == 0, "idx init");
    for (i = 0; i < N; i++) {
        keys[i] = i < N / 4 ? (uint32_t)i << 16 : i < N / 2 ? (uint32_t)i : (uint32_t)rnd();
        while (wg_idx_put(&t, keys[i], &keys[i]) < 0)
            keys[i] = (uint32_t)rnd() | 0x80000000u;
        live[i] = 1;
        n++;
    }
    CHECK(wg_idx_put(&t, keys[0], NULL) < 0, "duplicate refused");
    for (i = 0; i < 3 * N; i++) {
        int j = (int)(rnd() % N);
        if (rnd() & 1) {
            if (live[j]) {
                if (wg_idx_del(&t, keys[j]) < 0)
                    bad++;
                live[j] = 0;
                n--;
            } else if (wg_idx_get(&t, keys[j]))
                bad++;
        } else if (!live[j]) {
            if (wg_idx_put(&t, keys[j], &keys[j]) < 0)
                bad++;
            live[j] = 1;
            n++;
        }
    }
    for (i = 0; i < N; i++)
        if ((wg_idx_get(&t, keys[i]) == &keys[i]) != live[i])
            bad++;
    CHECK(bad == 0 && t.n == (uint32_t)n, "index table matches the model");
    CHECK(wg_idx_alloc(&t, &bad, &k) == 0 && wg_idx_get(&t, k) == &bad, "random index allocation");
    while (wg_idx_get(&t, k))
        k++;
    CHECK(wg_idx_del(&t, k) < 0, "deleting an absent key");
    wg_idx_free(&t);
}

static const uint8_t* pub_of(const void* v)
{
    return (const uint8_t*)v;
}

static void t_pubtab(void)
{
    enum { N = 65536 };
    static uint8_t pubs[N][32];
    static uint8_t live[N];
    wg_pubtab_t t;
    int i, bad = 0;
    CHECK(wg_pubtab_init(&t, pub_of) == 0, "pubtab init");
    for (i = 0; i < N; i++) {
        fill(pubs[i], 32);
        if (i % 7 == 0 && i)
            memcpy(pubs[i], pubs[i - 1], 16);
        if (wg_pubtab_put(&t, pubs[i]) < 0)
            bad++;
        live[i] = 1;
    }
    CHECK(wg_pubtab_put(&t, pubs[5]) < 0, "duplicate key refused");
    for (i = 0; i < N; i += 2) {
        if (wg_pubtab_del(&t, pubs[i]) < 0)
            bad++;
        live[i] = 0;
    }
    for (i = 0; i < N; i++)
        if ((wg_pubtab_get(&t, pubs[i]) == pubs[i]) != live[i])
            bad++;
    CHECK(bad == 0 && t.n == N / 2, "public-key table matches the model at 64k peers");
    wg_pubtab_free(&t);
}

typedef struct {
    uint8_t a[16];
    unsigned cidr;
    int peer;
    int live;
} pfx_t;

static int pfx_match(const uint8_t* p, const uint8_t* a, unsigned cidr)
{
    unsigned i;
    for (i = 0; i < cidr; i++)
        if (((p[i >> 3] ^ a[i >> 3]) >> (7 - (i & 7))) & 1)
            return 0;
    return 1;
}

static void t_aip_family(int v6)
{
    enum { NP = 1500, NQ = 4000 };
    static pfx_t px[NP];
    static int peers[64];
    wg_aip_t t;
    unsigned bytes = v6 ? 16 : 4, i, j;
    int bad = 0;
    memset(&t, 0, sizeof t);
    for (i = 0; i < NP; i++) {
        fill(px[i].a, 16);
        if (i % 3)
            memcpy(px[i].a, px[rnd() % (i ? i : 1)].a, (size_t)(rnd() % bytes));
        px[i].cidr = (unsigned)(rnd() % (bytes * 8 + 1));
        for (j = px[i].cidr; j < 128; j++)
            px[i].a[j >> 3] &= (uint8_t)~(0x80u >> (j & 7));
        px[i].peer = (int)(rnd() % 64);
        px[i].live = 0;
        for (j = 0; j < i; j++)
            if (px[j].live && px[j].cidr == px[i].cidr && memcmp(px[j].a, px[i].a, 16) == 0)
                px[j].live = 0;
        if (wg_aip_insert(&t, v6, px[i].a, px[i].cidr, &peers[px[i].peer]) < 0)
            bad++;
        px[i].live = 1;
        if (i % 5 == 4) {
            unsigned d = (unsigned)(rnd() % (i + 1));
            if (px[d].live) {
                if (wg_aip_remove(&t, v6, px[d].a, px[d].cidr) < 0)
                    bad++;
                px[d].live = 0;
            }
        }
    }
    for (int round = 0; round < 2; round++) {
        for (i = 0; i < NQ; i++) {
            uint8_t q[16];
            int best = -1;
            unsigned bc = 0;
            fill(q, 16);
            if (i % 2)
                memcpy(q, px[rnd() % NP].a, (size_t)(rnd() % (bytes + 1)));
            for (j = 0; j < NP; j++)
                if (px[j].live && (best < 0 || px[j].cidr > bc) && pfx_match(px[j].a, q, px[j].cidr)) {
                    best = (int)j;
                    bc = px[j].cidr;
                }
            if (wg_aip_lookup(&t, v6, q) != (best < 0 ? NULL : (void*)&peers[px[best].peer]))
                bad++;
        }
        if (round == 0) {
            wg_aip_remove_peer(&t, &peers[3]);
            wg_aip_remove_peer(&t, &peers[17]);
            for (j = 0; j < NP; j++)
                if (px[j].peer == 3 || px[j].peer == 17)
                    px[j].live = 0;
        }
    }
    CHECK(bad == 0, v6 ? "allowed IPs IPv6 longest-prefix match equals brute force" : "allowed IPs IPv4 longest-prefix match equals brute force");
    CHECK(wg_aip_remove(&t, v6, px[0].a, bytes * 8 + 1) < 0 && wg_aip_insert(&t, v6, px[0].a, bytes * 8 + 1, &peers[0]) < 0, "cidr over the address length refused");
    for (j = 0; j < NP; j++)
        if (px[j].live) {
            if (wg_aip_remove(&t, v6, px[j].a, px[j].cidr) < 0)
                bad++;
            px[j].live = 0;
        }
    CHECK(bad == 0 && t.nodes == 0 && !t.root4 && !t.root6, "removing every prefix frees every node");
    CHECK(wg_aip_remove(&t, v6, px[0].a, px[0].cidr) < 0, "removing an absent prefix");
    wg_aip_insert(&t, v6, px[1].a, px[1].cidr, &peers[1]);
    wg_aip_free(&t);
    CHECK(t.nodes == 0, "free");
}

static void t_aip(void)
{
    static const uint8_t a[4] = { 10, 1, 2, 3 }, b[4] = { 10, 1, 0, 0 }, c[4] = { 192, 168, 0, 1 };
    wg_aip_t t;
    int p1, p2, p3;
    memset(&t, 0, sizeof t);
    wg_aip_insert(&t, 0, b, 16, &p1);
    wg_aip_insert(&t, 0, a, 32, &p2);
    wg_aip_insert(&t, 0, c, 0, &p3);
    CHECK(wg_aip_lookup(&t, 0, a) == &p2 && wg_aip_lookup(&t, 0, b) == &p1 && wg_aip_lookup(&t, 0, c) == &p3, "host, /16 and default route");
    CHECK(wg_aip_lookup(&t, 1, c) == NULL, "families are separate");
    wg_aip_insert(&t, 0, a, 32, &p3);
    CHECK(wg_aip_lookup(&t, 0, a) == &p3, "re-insert moves the prefix to the new peer");
    wg_aip_remove_peer(&t, &p3);
    CHECK(wg_aip_lookup(&t, 0, a) == &p1 && wg_aip_lookup(&t, 0, c) == NULL, "remove peer");
    wg_aip_free(&t);
    t_aip_family(0);
    t_aip_family(1);
}

typedef struct {
    wg_batch_t b;
    int peer;
    int seq;
    uint8_t mem[WG_BATCH_MAX][256];
} tb_t;

static int done_count;
static void tb_done(void* arg)
{
    ((wg_batch_t*)arg)->done = 1;
    done_count++;
}

static void t_batches(void)
{
    enum { NB = 200 };
    static tb_t tb[NB];
    static uint8_t plain[NB][WG_BATCH_MAX][200];
    static uint32_t plen[NB][WG_BATCH_MAX];
    dyn_pool_t* pool = dyn_pool_new(4, 256);
    dyn_pool_chan_t* ch = pool ? dyn_pool_chan_new(pool) : NULL;
    wg_fifo_t q[2];
    uint8_t key[2][32];
    uint64_t ctr[2] = { 0, 0 };
    int i, j, bad = 0, order_bad = 0, next_seq[2] = { 0, 0 }, released = 0, submitted = 0;
    CHECK(pool && ch, "pool");
    if (!ch)
        return;
    memset(q, 0, sizeof q);
    fill(key[0], 32);
    fill(key[1], 32);
    for (i = 0; i < NB; i++) {
        tb_t* t = &tb[i];
        t->peer = (int)(rnd() & 1);
        t->seq = next_seq[t->peer]++;
        memcpy(t->b.key, key[t->peer], 32);
        t->b.remote_index = (uint32_t)(100 + t->peer);
        t->b.enc = 1;
        t->b.mtu = 1420;
        t->b.n = (uint16_t)(1 + rnd() % WG_BATCH_MAX);
        for (j = 0; j < t->b.n; j++) {
            wg_pkt_t* p = &t->b.pkt[j];
            p->buf = t->mem[j];
            p->cap = sizeof t->mem[j];
            p->len = plen[i][j] = (uint32_t)(rnd() % 200);
            p->ctr = ctr[t->peer]++;
            fill(plain[i][j], p->len);
            memcpy(p->buf + 16, plain[i][j], p->len);
        }
        wg_fifo_push(&q[t->peer], &t->b);
        if (dyn_pool_submit_cpu(ch, wg_batch_work, tb_done, &t->b) == 0)
            submitted++;
        else {
            wg_batch_work(&t->b);
            tb_done(&t->b);
        }
    }
    memset(next_seq, 0, sizeof next_seq);
    while (released < NB) {
        struct pollfd pf;
        int k;
        pf.fd = dyn_pool_wake_fd(ch);
        pf.events = POLLIN;
        if (dyn_pool_inflight(ch))
            poll(&pf, 1, 100);
        dyn_pool_drain(ch);
        for (k = 0; k < 2; k++) {
            wg_batch_t* b;
            while ((b = wg_fifo_pop_ready(&q[k])) != NULL) {
                tb_t* t = (tb_t*)(void*)b;
                if (t->seq != next_seq[k]++ || t->peer != k)
                    order_bad++;
                for (j = 0; j < b->n; j++) {
                    wg_pkt_t* p = &b->pkt[j];
                    uint32_t rx;
                    uint64_t c;
                    if (p->res < 32 || wg_data_parse(p->buf, (size_t)p->res, &rx, &c) < 0 || c != p->ctr || rx != (uint32_t)(100 + k)) {
                        bad++;
                        continue;
                    }
                    p->len = (uint32_t)p->res;
                }
                b->enc = 0;
                b->done = 0;
                wg_batch_work(b);
                for (j = 0; j < b->n; j++) {
                    wg_pkt_t* p = &b->pkt[j];
                    size_t want = wg_pad_len(plen[t - tb][j], 1420);
                    if (p->res != (int32_t)want || memcmp(p->buf + 16, plain[t - tb][j], plen[t - tb][j]) != 0)
                        bad++;
                }
                released++;
            }
        }
    }
    CHECK(order_bad == 0, "per-peer FIFO releases batches in send order");
    CHECK(bad == 0, "every batched packet seals with its counter and opens on the other side");
    CHECK(done_count == NB && submitted > 0, "every batch completed through the pool");
    dyn_pool_chan_free(ch);
    dyn_pool_free(pool);
}

static void t_fuzz(void)
{
    static uint8_t in[600];
    int i;
    for (i = 0; i < 300000; i++) {
        size_t n = 2 + (size_t)(rnd() % (i % 10 == 0 ? sizeof in - 2 : 64));
        fill(in, n);
        LLVMFuzzerTestOneInput(in, n);
    }
    checks++;
}

typedef struct {
    int on[5];
    uint64_t at[5];
    uint64_t last_sent;
    int sent, need;
    unsigned attempts, persist;
} tmodel_t;

enum { TM_RT, TM_KA, TM_NH, TM_Z, TM_PS };

static unsigned tm_due(tmodel_t* m, uint64_t now)
{
    unsigned act = 0;
    if (m->on[TM_RT] && m->at[TM_RT] <= now) {
        m->on[TM_RT] = 0;
        if (m->attempts > 18) {
            act |= WG_ACT_GIVE_UP;
            m->on[TM_KA] = 0;
            if (!m->on[TM_Z])
                m->on[TM_Z] = 1, m->at[TM_Z] = now + 540000;
        } else {
            m->attempts++;
            act |= WG_ACT_SEND_INIT | WG_ACT_RETRY;
        }
    }
    if (m->on[TM_KA] && m->at[TM_KA] <= now) {
        m->on[TM_KA] = 0;
        act |= WG_ACT_SEND_KEEPALIVE;
        if (m->need)
            m->need = 0, m->on[TM_KA] = 1, m->at[TM_KA] = now + 10000;
    }
    if (m->on[TM_NH] && m->at[TM_NH] <= now)
        m->on[TM_NH] = 0, act |= WG_ACT_SEND_INIT;
    if (m->on[TM_Z] && m->at[TM_Z] <= now)
        m->on[TM_Z] = 0, act |= WG_ACT_ZERO_KEYS;
    if (m->on[TM_PS] && m->at[TM_PS] <= now) {
        m->on[TM_PS] = 0;
        if (m->persist)
            act |= WG_ACT_SEND_KEEPALIVE;
    }
    return act;
}

static uint64_t tm_next(const tmodel_t* m)
{
    uint64_t best = 0;
    int i;
    for (i = 0; i < 5; i++)
        if (m->on[i] && (!best || m->at[i] < best))
            best = m->at[i];
    return best;
}

static void p_timers_model(void)
{
    fk_rng_t r;
    unsigned run, runs, i, mism = 0, nmism = 0, allow = 0, gave = 0, zeros = 0, kas = 0, retries = 0;
    if (!fk_only("timers-model"))
        return;
    r = fk_stream("timers-model");
    runs = (unsigned)fk_iters(400);
    for (run = 0; run < runs; run++) {
        wg_timers_t t;
        tmodel_t m;
        uint64_t now = 1 + fk_below(&r, 1u << 30);
        int down = fk_chance(&r, 30);
        memset(&t, 0, sizeof t);
        memset(&m, 0, sizeof m);
        t.persist_s = (uint16_t)(fk_chance(&r, 30) ? 1 + fk_below(&r, 60) : 0);
        m.persist = t.persist_s;
        wg_timers_start(&t, now);
        for (i = 0; i < 400; i++) {
            unsigned op = (unsigned)fk_below(&r, 12), j = (unsigned)fk_below(&r, 1000), a, b;
            if (down && op < 2 && i)
                op = 9;
            int want_allowed = !m.sent || now - m.last_sent >= 5000;
            if ((wg_timers_hs_allowed(&t, now) != 0) != want_allowed)
                mism++;
            allow += want_allowed;
            switch (op) {
            case 0:
                wg_timers_hs_initiated(&t, now, j, 0);
                m.attempts = 0;
                m.last_sent = now, m.sent = 1, m.on[TM_RT] = 1, m.at[TM_RT] = now + 5000 + j % 334;
                break;
            case 1:
                wg_timers_hs_complete(&t, now);
                m.on[TM_RT] = 0, m.attempts = 0;
                break;
            case 2:
                wg_timers_session_derived(&t, now);
                m.on[TM_Z] = 1, m.at[TM_Z] = now + 540000;
                break;
            case 3:
                wg_timers_data_sent(&t, now, j);
                if (!m.on[TM_NH])
                    m.on[TM_NH] = 1, m.at[TM_NH] = now + 15000 + j % 334;
                break;
            case 4:
                wg_timers_data_recv(&t, now);
                if (!m.on[TM_KA])
                    m.on[TM_KA] = 1, m.at[TM_KA] = now + 10000;
                else
                    m.need = 1;
                break;
            case 5:
                wg_timers_auth_recv(&t);
                m.on[TM_NH] = 0;
                break;
            case 6:
                wg_timers_auth_sent(&t);
                m.on[TM_KA] = 0;
                break;
            case 7:
                wg_timers_traversal(&t, now);
                if (m.persist)
                    m.on[TM_PS] = 1, m.at[TM_PS] = now + (uint64_t)m.persist * 1000;
                break;
            case 8:
                now += fk_below(&r, 20000);
                break;
            default: {
                uint64_t nx = tm_next(&m);
                if (nx && fk_chance(&r, 80))
                    now = nx + (fk_chance(&r, 20) ? fk_below(&r, 3) : 0) - (fk_chance(&r, 10) && nx > now ? 1 : 0);
                else
                    now += fk_below(&r, 600000);
                a = wg_timers_due(&t, now);
                b = tm_due(&m, now);
                if (a != b)
                    mism++;
                gave += (b & WG_ACT_GIVE_UP) != 0;
                zeros += (b & WG_ACT_ZERO_KEYS) != 0;
                kas += (b & WG_ACT_SEND_KEEPALIVE) != 0;
                retries += (b & WG_ACT_RETRY) != 0;
                if (b & WG_ACT_SEND_INIT) {
                    wg_timers_hs_initiated(&t, now, j, (b & WG_ACT_RETRY) != 0);
                    if (!(b & WG_ACT_RETRY))
                        m.attempts = 0;
                    m.last_sent = now, m.sent = 1, m.on[TM_RT] = 1, m.at[TM_RT] = now + 5000 + j % 334;
                }
            }
            }
            if (wg_timers_next(&t) != tm_next(&m))
                nmism++;
        }
    }
    CHECK(mism == 0, "timers-model: random event orders (initiate, complete, derive, data, auth, traversal, time jumps) fire the model's actions");
    CHECK(nmism == 0, "timers-model: next deadline equals the model's earliest armed timer after every event");
    CHECK(gave > 0 && zeros > 0 && kas > runs && retries > runs && allow > 0, "timers-model: give-up, zeroing, keepalives and retries were reached");
}

static void p_slots_model(void)
{
    fk_rng_t r;
    static wg_keypair_t kp[4096];
    static int mdrop[4096];
    wg_slots_t s;
    wg_keypair_t *mp = NULL, *mc = NULL, *mn = NULL;
    unsigned i, n, used = 0, mism = 0, promoted = 0;
    if (!fk_only("slots-model"))
        return;
    r = fk_stream("slots-model");
    memset(&s, 0, sizeof s);
    memset(dropped, 0, sizeof dropped);
    memset(mdrop, 0, sizeof mdrop);
    n = (unsigned)fk_iters(20000);
    for (i = 0; i < n; i++) {
        unsigned op = (unsigned)fk_below(&r, 7);
        if (op < 3 && used < 4096) {
            wg_keypair_t* k = &kp[used];
            memset(k, 0, sizeof *k);
            k->local_index = used++;
            k->initiator = (uint8_t)(op == 0 || (op == 2 && fk_chance(&r, 50)));
            wg_slots_begin(&s, k, on_drop, NULL);
            if (k->initiator) {
                if (mn) {
                    if (mc)
                        mdrop[mc->local_index]++;
                    if (mp)
                        mdrop[mp->local_index]++;
                    mp = mn, mn = NULL;
                } else {
                    if (mp)
                        mdrop[mp->local_index]++;
                    mp = mc;
                }
                mc = k;
            } else {
                if (mn)
                    mdrop[mn->local_index]++;
                if (mp)
                    mdrop[mp->local_index]++;
                mn = k, mp = NULL;
            }
        } else if (op < 6) {
            wg_keypair_t* k = op == 3 ? mn : op == 4 ? mc : used ? &kp[fk_below(&r, used)] : NULL;
            int want = k && k == mn, got = wg_slots_received(&s, k, on_drop, NULL);
            if (got != want)
                mism++;
            if (want) {
                promoted++;
                if (mp)
                    mdrop[mp->local_index]++;
                mp = mc, mc = mn, mn = NULL;
            }
        } else if (fk_chance(&r, 5)) {
            wg_slots_clear(&s, on_drop, NULL);
            if (mp)
                mdrop[mp->local_index]++;
            if (mc)
                mdrop[mc->local_index]++;
            if (mn)
                mdrop[mn->local_index]++;
            mp = mc = mn = NULL;
        }
        if (s.prev != mp || s.cur != mc || s.next != mn)
            mism++;
        if (used == 4096)
            break;
    }
    for (i = 0; i < used; i++)
        if (dropped[i] != mdrop[i] || dropped[i] > 1)
            mism++;
    CHECK(mism == 0 && promoted > 50, "slots-model: previous/current/next and every drop follow the wireguard-go rotation model under random begin/receive/clear");
}

static size_t pad_model(size_t n, unsigned mtu)
{
    size_t last = n, p;
    if (!mtu)
        return (n + 15) / 16 * 16;
    if (n > mtu)
        last = n % mtu;
    p = (last + 15) / 16 * 16;
    if (p > mtu)
        p = mtu;
    return n - last + p;
}

static void p_data_garble(void)
{
    fk_rng_t r;
    static uint8_t buf[3200], orig[3200], pt[3200];
    uint8_t key[32];
    unsigned i, n, padbad = 0, sealbad = 0, bad = 0, opened = 0;
    if (!fk_only("data-garble"))
        return;
    r = fk_stream("data-garble");
    n = (unsigned)fk_iters(6000);
    for (i = 0; i < n; i++) {
        unsigned mtu = fk_chance(&r, 10) ? 0 : (unsigned)(fk_chance(&r, 50) ? 1280 + fk_below(&r, 200) : 16 + fk_below(&r, 1500));
        size_t len = (size_t)fk_below(&r, 1600), cap = sizeof buf, k;
        uint64_t ctr = fk_chance(&r, 5) ? WG_REJECT_AFTER_MESSAGES - fk_below(&r, 2) : fk_next(&r) >> fk_below(&r, 64);
        int sl, op;
        if (pad_model(len, mtu) != wg_pad_len(len, mtu))
            padbad++;
        if (fk_chance(&r, 10))
            cap = 16 + pad_model(len, mtu) + 16 - fk_below(&r, 2);
        fk_bytes(&r, key, 32);
        fk_bytes(&r, pt, len);
        memcpy(buf + 16, pt, len);
        sl = wg_data_seal(key, (uint32_t)fk_next(&r), ctr, buf, len, cap, mtu);
        if ((sl >= 0) != (ctr < WG_REJECT_AFTER_MESSAGES && cap >= 16 + pad_model(len, mtu) + 16) || (sl >= 0 && (size_t)sl != 32 + pad_model(len, mtu)))
            sealbad++;
        if (sl < 0)
            continue;
        memcpy(orig, buf, (size_t)sl);
        k = (size_t)sl;
        op = (int)fk_below(&r, 5);
        if (op == 0)
            buf[fk_below(&r, k)] ^= (uint8_t)(1 + fk_below(&r, 255));
        else if (op == 1)
            k = (size_t)fk_below(&r, k);
        else if (op == 2)
            buf[k++] = (uint8_t)fk_next(&r);
        else if (op == 3)
            wg_put_le32(buf + 4, (uint32_t)fk_next(&r));
        else
            buf[1 + fk_below(&r, 3)] = (uint8_t)(1 + fk_below(&r, 255));
        {
            int same = k == (size_t)sl && !memcmp(buf, orig, 4) && !memcmp(buf + 8, orig + 8, k - 8);
            int o = wg_data_open(key, buf, k);
            if ((o >= 0) != same || (o >= 0 && ((size_t)o != pad_model(len, mtu) || memcmp(buf + 16, pt, len))))
                bad++;
            opened += o >= 0;
        }
    }
    CHECK(padbad == 0, "data-garble: wg_pad_len equals the wireguard-go padding formula for random lengths and MTUs (0, small, typical)");
    CHECK(sealbad == 0, "data-garble: seal succeeds exactly when the counter is below the limit and the buffer holds header, padding and tag");
    CHECK(bad == 0 && opened > n / 10, "data-garble: open accepts only the untouched message (receiver index aside) and returns the padded plaintext");
}

static void p_tables_reseeded(void)
{
    fk_rng_t r;
    unsigned i, n;
    if (!fk_only("tables-reseeded"))
        return;
    r = fk_stream("tables-reseeded");
    n = (unsigned)fk_iters(2);
    for (i = 0; i < n; i++) {
        rs = fk_next(&r) | 1;
        t_peek();
        t_idx();
        t_pubtab();
        t_aip();
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fk_init("test_wg_transport");
    if (fk_legacy()) {
        t_data();
        t_kp();
        t_peek();
        t_slots();
        t_timers();
        t_idx();
        t_pubtab();
        t_aip();
        t_batches();
        t_fuzz();
    }
    p_timers_model();
    p_slots_model();
    p_data_garble();
    p_tables_reseeded();
    printf("test_wg_transport: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
