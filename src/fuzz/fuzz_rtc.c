#include "core/dyn-rtc.h"
#include "core/dyn-hash.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t buf[16][1500];
    size_t len[16];
    int n;
} fz_q;

static fz_q fz_qs[2];
volatile uint64_t fz_rtc_sink;

static void fz_packet(void* ud, const uint8_t* p, size_t n)
{
    fz_q* q = &fz_qs[(intptr_t)ud];
    if (q->n < 16 && n <= sizeof(q->buf[0])) {
        memcpy(q->buf[q->n], p, n);
        q->len[q->n++] = n;
    }
}

static void fz_message(void* ud, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n)
{
    (void)ud;
    fz_rtc_sink += stream + ppid + n + (n ? p[0] + p[n - 1] : 0);
}

static void fz_state(void* ud, int st)
{
    (void)ud;
    fz_rtc_sink += (uint64_t)st;
}

static void fz_reset(void* ud, uint16_t stream)
{
    (void)ud;
    fz_rtc_sink += stream;
}

static void fz_sent(void* ud, uint16_t stream, size_t n)
{
    (void)ud;
    fz_rtc_sink += stream + n;
}

static void fz_seed(rtc_rng* rng)
{
    uint8_t seed[32];
    memset(seed, 0x5a, sizeof(seed));
    rtc_rng_seed(rng, seed);
}

static void fz_stun(const uint8_t* p, size_t n)
{
    rtc_stun_msg m;
    int i;
    if (rtc_stun_parse(&m, p, n) < 0)
        return;
    rtc_stun_check_fingerprint(&m);
    rtc_stun_check_integrity(&m, (const uint8_t*)"pwd", 3);
    rtc_stun_error_code(&m);
    for (i = 0; i < m.nattr; i++) {
        rtc_addr a;
        uint32_t u;
        uint64_t v;
        rtc_stun_get_addr(&m, &m.attr[i], &a);
        rtc_stun_get_u32(&m, m.attr[i].type, &u);
        rtc_stun_get_u64(&m, m.attr[i].type, &v);
    }
}

static void fz_sdp(const uint8_t* p, size_t n)
{
    static rtc_sdp sdp;
    const char* err = NULL;
    rtc_sdp_cand c;
    char line[512];
    char* s = (char*)malloc(n + 1);
    if (!s)
        return;
    memcpy(s, p, n);
    s[n] = 0;
    rtc_sdp_parse(&sdp, s, n, &err);
    if (rtc_sdp_parse_candidate(s, n, &c) == 0 && rtc_sdp_write_candidate(&c, line, sizeof(line)) > 0) {
        rtc_sdp_cand c2;
        if (rtc_sdp_parse_candidate(line, strlen(line), &c2) != 0 || !rtc_addr_eq(&c.addr, &c2.addr) || c.prio != c2.prio)
            abort();
    }
    rtc_sdp_fp_alg(s, n);
    free(s);
}

static void fz_dcep(const uint8_t* p, size_t n)
{
    rtc_dcep_msg m;
    uint8_t out[1024];
    if (rtc_dcep_parse(p, n, &m) == 0 && m.type == RTC_DCEP_OPEN) {
        size_t w = rtc_dcep_write_open(out, sizeof(out), m.ordered, m.max_rtx, m.max_life, m.priority, (const char*)m.label, m.label_len,
            (const char*)m.protocol, m.protocol_len);
        rtc_dcep_msg m2;
        if (w && (rtc_dcep_parse(out, w, &m2) != 0 || m2.ordered != m.ordered || m2.max_rtx != m.max_rtx || m2.label_len != m.label_len))
            abort();
    }
}

static void fz_deliver(rtc_sctp* to, int from, uint64_t now)
{
    fz_q* q = &fz_qs[from];
    int i, k = q->n;
    q->n = 0;
    for (i = 0; i < k; i++)
        rtc_sctp_input(to, q->buf[i], q->len[i], now);
}

static void fz_sctp(const uint8_t* p, size_t n)
{
    static const rtc_sctp_cb cb = { fz_packet, fz_message, fz_state, fz_reset, fz_reset, fz_sent };
    rtc_rng rng;
    rtc_sctp *a, *b;
    uint64_t now = 1000000;
    uint8_t pkt[1500];
    int i;
    fz_seed(&rng);
    fz_qs[0].n = fz_qs[1].n = 0;
    a = rtc_sctp_new(&cb, (void*)(intptr_t)0, &rng, 5000, 5000, 0);
    b = rtc_sctp_new(&cb, (void*)(intptr_t)1, &rng, 5000, 5000, 0);
    if (!a || !b)
        goto out;
    rtc_sctp_connect(a, now);
    for (i = 0; i < 8; i++) {
        fz_deliver(b, 0, now);
        fz_deliver(a, 1, now);
    }
    rtc_sctp_send(a, 1, 51, (const uint8_t*)"hello", 5, 0, -1, -1, now);
    rtc_sctp_flush(a, now);
    fz_deliver(b, 0, now);
    while (n >= 2) {
        size_t l = ((size_t)p[0] << 8 | p[1]) % 1200;
        p += 2;
        n -= 2;
        if (l > n)
            l = n;
        if (l + 12 <= sizeof(pkt)) {
            uint32_t crc;
            rtc_wr16(pkt, 5000);
            rtc_wr16(pkt + 2, 5000);
            rtc_wr32(pkt + 4, b->my_tag);
            memset(pkt + 8, 0, 4);
            memcpy(pkt + 12, p, l);
            if (l && (p[0] & 0x80) && l >= 4)
                memcpy(pkt + 4, p, 4);
            crc = dyn_crc32c(pkt, l + 12);
            pkt[8] = (uint8_t)crc;
            pkt[9] = (uint8_t)(crc >> 8);
            pkt[10] = (uint8_t)(crc >> 16);
            pkt[11] = (uint8_t)(crc >> 24);
            rtc_sctp_input(b, pkt, l + 12, now);
            now += 50000;
            rtc_sctp_tick(b, now);
            rtc_sctp_flush(b, now);
            fz_qs[1].n = 0;
        }
        p += l;
        n -= l;
    }
    rtc_sctp_reset_stream(b, 1);
    rtc_sctp_tick(b, now + 120000000ull);
out:
    if (a)
        rtc_sctp_free(a);
    if (b)
        rtc_sctp_free(b);
}

static void fz_sctp_client(const uint8_t* p, size_t n)
{
    static const rtc_sctp_cb cb = { fz_packet, fz_message, fz_state, fz_reset, fz_reset, fz_sent };
    rtc_rng rng;
    rtc_sctp* a;
    uint64_t now = 1000000;
    uint8_t* pkt;
    fz_seed(&rng);
    fz_qs[0].n = 0;
    a = rtc_sctp_new(&cb, (void*)(intptr_t)0, &rng, 5000, 5000, 0);
    pkt = (uint8_t*)malloc(12 + 65536 + 64);
    if (!a || !pkt)
        goto out;
    rtc_sctp_connect(a, now);
    while (n >= 3) {
        size_t take = ((size_t)p[0] << 8 | p[1]), l;
        int raw = p[2] & 1;
        uint32_t crc;
        p += 3;
        n -= 3;
        if (take > n)
            take = n;
        rtc_wr16(pkt, 5000);
        rtc_wr16(pkt + 2, 5000);
        rtc_wr32(pkt + 4, a->my_tag);
        if (raw) {
            memcpy(pkt + 12, p, take);
            l = take;
        } else {
            size_t cl = take > 16 ? take - 16 : 0;
            pkt[12] = RTC_SCTP_INIT_ACK;
            pkt[13] = 0;
            rtc_wr16(pkt + 14, (uint32_t)(4 + 16 + 4 + cl));
            memset(pkt + 16, 0, 16);
            memcpy(pkt + 16, p, take < 16 ? take : 16);
            rtc_wr16(pkt + 32, 7);
            rtc_wr16(pkt + 34, (uint32_t)(4 + cl));
            if (cl)
                memcpy(pkt + 36, p + 16, cl);
            l = 24 + cl;
        }
        while (l & 3)
            pkt[12 + l++] = 0;
        memset(pkt + 8, 0, 4);
        crc = dyn_crc32c(pkt, 12 + l);
        pkt[8] = (uint8_t)crc;
        pkt[9] = (uint8_t)(crc >> 8);
        pkt[10] = (uint8_t)(crc >> 16);
        pkt[11] = (uint8_t)(crc >> 24);
        rtc_sctp_input(a, pkt, 12 + l, now);
        now += 400000;
        rtc_sctp_tick(a, now);
        rtc_sctp_flush(a, now);
        fz_qs[0].n = 0;
        p += take;
        n -= take;
    }
out:
    free(pkt);
    if (a)
        rtc_sctp_free(a);
}

static void fz_turn_send(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)sock;
    (void)to;
    fz_rtc_sink += n + (n ? p[0] : 0);
}

static void fz_turn_ev(void* ud, int idx, int ev, int code)
{
    (void)ud;
    fz_rtc_sink += (uint64_t)(idx + ev + code);
}

static void fz_turn(const uint8_t* p, size_t n)
{
    static rtc_turn t;
    rtc_rng rng;
    rtc_addr srv, peer;
    uint8_t buf[2048];
    const uint8_t* data;
    size_t dl;
    uint64_t now = 1000000;
    fz_seed(&rng);
    rtc_addr_parse("127.0.0.1", 9, 3478, &srv);
    rtc_turn_init(&t, &rng, 0, (n && (p[0] & 1)) ? RTC_TURN_KIND_STUN : RTC_TURN_KIND_TURN, 0, &srv, "user", "pass", fz_turn_send,
        fz_turn_ev, NULL);
    rtc_turn_start(&t, now);
    while (n >= 2) {
        size_t l = ((size_t)p[0] << 8 | p[1]) % sizeof(buf);
        p += 2;
        n -= 2;
        if (l > n)
            l = n;
        memcpy(buf, p, l);
        if (l >= 20 && t.tx[0].kind != RTC_TX_NONE && !(buf[0] & 0x40))
            memcpy(buf + 8, t.tx[0].tid, 12);
        rtc_turn_input(&t, buf, l, now, &peer, &data, &dl);
        now += 100000;
        rtc_turn_tick(&t, now);
        if (t.state == RTC_TURN_READY)
            rtc_turn_send(&t, &srv, (const uint8_t*)"x", 1, now);
        p += l;
        n -= l;
    }
    rtc_turn_close(&t, now);
}

static void fz_ice_send(void* ud, const rtc_cand* local, const rtc_addr* to, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)local;
    (void)to;
    fz_rtc_sink += n + (n ? p[0] : 0);
}

static void fz_ice_ev(void* ud, int ev)
{
    (void)ud;
    fz_rtc_sink += (uint64_t)ev;
}

static void fz_ice(const uint8_t* p, size_t n)
{
    static rtc_ice ice;
    rtc_rng rng;
    rtc_cand l, r;
    rtc_stun_msg m;
    uint64_t now = 1000000;
    fz_seed(&rng);
    rtc_ice_init(&ice, &rng, n && (p[0] & 1), fz_ice_send, fz_ice_ev, NULL);
    rtc_ice_set_local_creds(&ice, "LUFR", "localpasswordlocalpassword");
    rtc_ice_set_remote_creds(&ice, "RUFR", "remotepasswordremotepasswd");
    memset(&l, 0, sizeof(l));
    l.type = RTC_CAND_HOST;
    l.comp = 1;
    rtc_addr_parse("127.0.0.1", 9, 40000, &l.addr);
    l.base = l.addr;
    l.prio = rtc_ice_cand_prio(RTC_CAND_HOST, 65535, 1);
    l.found = 1;
    rtc_ice_add_local(&ice, &l);
    r = l;
    rtc_addr_parse("127.0.0.1", 9, 40001, &r.addr);
    rtc_ice_add_remote(&ice, &r);
    rtc_ice_start(&ice, now);
    while (n >= 2) {
        size_t len = ((size_t)p[0] << 8 | p[1]) % 1500;
        p += 2;
        n -= 2;
        if (len > n)
            len = n;
        if (rtc_stun_parse(&m, p, len) == 0)
            rtc_ice_input(&ice, 0, &r.addr, &m, now);
        now += 20000;
        rtc_ice_tick(&ice, now);
        p += len;
        n -= len;
    }
    rtc_ice_close(&ice);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1 || size > 65536)
        return 0;
    switch (data[0] % 7) {
    case 0:
        fz_stun(data + 1, size - 1);
        break;
    case 1:
        fz_sdp(data + 1, size - 1);
        break;
    case 2:
        fz_dcep(data + 1, size - 1);
        break;
    case 3:
        fz_sctp(data + 1, size - 1);
        break;
    case 4:
        fz_turn(data + 1, size - 1);
        break;
    case 6:
        fz_sctp_client(data + 1, size - 1);
        break;
    default:
        fz_ice(data + 1, size - 1);
        break;
    }
    return 0;
}
