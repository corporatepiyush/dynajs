#include "rtc-sctp.h"
#include "../dyn-hash.h"

#include <stdlib.h>

enum {
    TXS_OUT,
    TXS_GAP,
    TXS_RTX,
    TXS_ABANDON
};

#define F_E 1
#define F_B 2
#define F_U 4
#define F_I 8


static uint32_t sctp_max_payload(const rtc_sctp* s)
{
    return s->mtu - 12 - 16;
}

static rtc_sctp_rxc rtc_sctp_tomb;

#define SCTP_LIVE(s) ((s)->state == RTC_SCTP_ESTABLISHED || (s)->state == RTC_SCTP_SHUTDOWN_RECEIVED)

static void sctp_pkt_begin(rtc_sctp* s, uint32_t vtag)
{
    rtc_wr16(s->pkt, s->lport);
    rtc_wr16(s->pkt + 2, s->rport);
    rtc_wr32(s->pkt + 4, vtag);
    rtc_wr32(s->pkt + 8, 0);
    s->pkt_len = 12;
}

static uint32_t sctp_room(const rtc_sctp* s)
{
    return s->mtu > s->pkt_len ? s->mtu - s->pkt_len : 0;
}

static uint8_t* sctp_chunk(rtc_sctp* s, uint8_t type, uint8_t flags, uint32_t body)
{
    uint8_t* c = s->pkt + s->pkt_len;
    uint32_t total = 4 + body, padded = (total + 3) & ~3u;
    if (body > s->mtu || padded > sctp_room(s))
        return NULL;
    c[0] = type;
    c[1] = flags;
    rtc_wr16(c + 2, total);
    if (padded > total)
        memset(c + total, 0, padded - total);
    s->pkt_len += padded;
    return c + 4;
}

static void sctp_pkt_emit(rtc_sctp* s)
{
    uint32_t crc;
    if (s->pkt_len <= 12)
        return;
    if (s->peer_zcsum && (SCTP_LIVE(s) || s->state == RTC_SCTP_SHUTDOWN_ACK_SENT)) {
        crc = 0;
        s->st.zcsum_tx++;
    } else
        crc = dyn_crc32c(s->pkt, s->pkt_len);
    s->pkt[8] = (uint8_t)crc;
    s->pkt[9] = (uint8_t)(crc >> 8);
    s->pkt[10] = (uint8_t)(crc >> 16);
    s->pkt[11] = (uint8_t)(crc >> 24);
    s->st.pkts_tx++;
    s->cb.packet(s->ud, s->pkt, s->pkt_len);
    s->pkt_len = 0;
}

static int sctp_crc_into(uint8_t* tmp, const uint8_t* p, size_t n)
{
    uint32_t want = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    memcpy(tmp, p, n);
    memset(tmp + 8, 0, 4);
    return dyn_crc32c(tmp, n) == want;
}

int rtc_sctp_check_packet(const uint8_t* p, size_t n)
{
    uint8_t buf[2048];
    uint8_t* tmp;
    int ok;
    if (n < 12 || (n & 3))
        return 0;
    if (n <= sizeof(buf))
        return sctp_crc_into(buf, p, n);
    tmp = (uint8_t*)malloc(n);
    if (!tmp)
        return 0;
    ok = sctp_crc_into(tmp, p, n);
    free(tmp);
    return ok;
}

static int sctp_crc_ok(rtc_sctp* s, const uint8_t* p, size_t n)
{
    if (s->zcsum_local && !p[8] && !p[9] && !p[10] && !p[11] && s->state != RTC_SCTP_CLOSED && p[12] != RTC_SCTP_INIT
        && p[12] != RTC_SCTP_COOKIE_ECHO) {
        s->st.zcsum_rx++;
        return 1;
    }
    if (n > sizeof(s->pkt) || s->pkt_len || (n & 3))
        return rtc_sctp_check_packet(p, n);
    return sctp_crc_into(s->pkt, p, n);
}

static rtc_sctp_stream* sctp_stream_at(const rtc_sctp* s, uint16_t id)
{
    uint32_t pg = (uint32_t)id / RTC_SCTP_SPAGE;
    if (pg >= s->npages || !s->spages[pg])
        return NULL;
    return &s->spages[pg][id % RTC_SCTP_SPAGE];
}

static rtc_sctp_stream* sctp_stream(rtc_sctp* s, uint16_t id)
{
    uint32_t pg = (uint32_t)id / RTC_SCTP_SPAGE;
    if (id >= s->local_streams)
        return NULL;
    if (pg >= s->npages) {
        uint32_t n = s->npages ? s->npages : 4;
        rtc_sctp_stream** np;
        while (n <= pg)
            n *= 2;
        np = (rtc_sctp_stream**)realloc(s->spages, n * sizeof(*np));
        if (!np)
            return NULL;
        memset(np + s->npages, 0, (n - s->npages) * sizeof(*np));
        s->spages = np;
        s->npages = n;
    }
    if (!s->spages[pg]) {
        s->spages[pg] = (rtc_sctp_stream*)calloc(RTC_SCTP_SPAGE, sizeof(rtc_sctp_stream));
        if (!s->spages[pg])
            return NULL;
        if (!(s->pg_rx_seen[pg >> 3] & (1u << (pg & 7)))) {
            uint32_t k;
            for (k = 0; k < RTC_SCTP_SPAGE; k++)
                s->spages[pg][k].in_sync = 1;
        }
        s->nspages++;
    }
    return &s->spages[pg][id % RTC_SCTP_SPAGE];
}

rtc_sctp* rtc_sctp_new(const rtc_sctp_cb* cb, void* ud, rtc_rng* rng, uint16_t lport, uint16_t rport, uint32_t mtu)
{
    rtc_sctp* s = (rtc_sctp*)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cb = *cb;
    s->ud = ud;
    s->rng = rng;
    s->lport = lport;
    s->rport = rport;
    s->mtu = mtu ? mtu : RTC_SCTP_DEFAULT_MTU;
    if (s->mtu > sizeof(s->pkt))
        s->mtu = sizeof(s->pkt);
    if (s->mtu < 512)
        s->mtu = 512;
    s->mtu &= ~3u;
    s->rto_us = RTC_SCTP_RTO_INITIAL_US;
    s->max_msg = 16u << 20;
    s->local_streams = RTC_SCTP_STREAMS;
    s->zcsum_local = 1;
    rtc_rng_bytes(rng, s->secret, sizeof(s->secret));
    return s;
}

static void sctp_free_msgs(rtc_sctp* s)
{
    uint32_t i;
    for (i = s->om_head; i != s->om_tail; i++)
        free(s->om[i & (s->om_cap - 1)].data);
    s->om_head = s->om_tail = s->om_chunk = 0;
}

void rtc_sctp_free(rtc_sctp* s)
{
    uint32_t i;
    if (!s)
        return;
    sctp_free_msgs(s);
    free(s->om);
    free(s->tx);
    free(s->rtxq);
    if (s->stash) {
        for (i = 0; i < RTC_SCTP_RX_WIN; i++)
            if (s->stash[i] && s->stash[i] != &rtc_sctp_tomb)
                free(s->stash[i]);
        free(s->stash);
    }
    free(s->hix);
    while (s->slot_nfree)
        free(s->slot_stack[--s->slot_nfree]);
    free(s->slot_stack);
    free(s->asm_buf);
    for (i = 0; i < s->npages; i++)
        free(s->spages[i]);
    free(s->spages);
    free(s->rw_list);
    free(s->cookie);
    free(s);
}

static void sctp_send_simple(rtc_sctp* s, uint8_t type, uint8_t flags, uint32_t vtag);

static void sctp_set_state(rtc_sctp* s, int st)
{
    if (s->state == st)
        return;
    s->state = st;
    s->cb.state(s->ud, st);
}

static void sctp_on_heartbeat_ack(rtc_sctp* s, const uint8_t* c, uint32_t len);

static uint32_t sctp_nonzero(rtc_sctp* s)
{
    uint32_t v;
    do
        v = rtc_rng_u32(s->rng);
    while (!v);
    return v;
}

static void sctp_init_params(const rtc_sctp* s, uint8_t* b, uint32_t* off)
{
    if (s->zcsum_local) {
        rtc_wr16(b + *off, 0x8001);
        rtc_wr16(b + *off + 2, 8);
        rtc_wr32(b + *off + 4, RTC_SCTP_EDMID_DTLS);
        *off += 8;
    }
    rtc_wr16(b + *off, 0x8008);
    rtc_wr16(b + *off + 2, 6);
    b[*off + 4] = RTC_SCTP_FORWARD_TSN;
    b[*off + 5] = RTC_SCTP_RECONFIG;
    b[*off + 6] = 0;
    b[*off + 7] = 0;
    *off += 8;
    rtc_wr16(b + *off, 0xC000);
    rtc_wr16(b + *off + 2, 4);
    *off += 4;
}

static uint32_t sctp_a_rwnd(const rtc_sctp* s)
{
    return s->stash_bytes >= RTC_SCTP_RWND ? 0 : RTC_SCTP_RWND - s->stash_bytes;
}

static void sctp_send_init(rtc_sctp* s)
{
    uint8_t* b;
    uint32_t off = 16;
    sctp_pkt_begin(s, 0);
    b = s->pkt + s->pkt_len + 4;
    rtc_wr32(b, s->my_tag);
    rtc_wr32(b + 4, RTC_SCTP_RWND);
    rtc_wr16(b + 8, s->local_streams);
    rtc_wr16(b + 10, s->local_streams);
    rtc_wr32(b + 12, s->my_itsn);
    sctp_init_params(s, b, &off);
    sctp_chunk(s, RTC_SCTP_INIT, 0, off);
    sctp_pkt_emit(s);
    s->st.init_tx++;
}

static void sctp_reset_tx(rtc_sctp* s)
{
    s->next_tsn = s->my_itsn;
    s->ack_base = s->my_itsn;
    s->adv_ack = s->my_itsn - 1;
    s->reconf_req_seq = s->my_itsn;
}

void rtc_sctp_connect(rtc_sctp* s, uint64_t now)
{
    if (s->state != RTC_SCTP_CLOSED)
        return;
    s->my_tag = sctp_nonzero(s);
    s->my_itsn = rtc_rng_u32(s->rng);
    sctp_reset_tx(s);
    s->state = RTC_SCTP_COOKIE_WAIT;
    s->init_tries = 1;
    s->t1_rto = RTC_SCTP_RTO_INITIAL_US;
    s->t1_us = now + s->t1_rto;
    sctp_send_init(s);
}

static void sctp_cookie_mac(const rtc_sctp* s, const uint8_t* c, size_t n, uint8_t out[16])
{
    uint8_t mac[32];
    dyn_hmac(dyn_hash_algo_by_id(DYN_HASH_SHA256), s->secret, sizeof(s->secret), c, n, mac);
    memcpy(out, mac, 16);
}

#define SCTP_COOKIE_LEN 52

static void sctp_parse_init_params(rtc_sctp* s, const uint8_t* p, size_t n, const uint8_t** cookie, uint16_t* cookie_len)
{
    size_t off = 0;
    s->peer_prsctp = 0;
    s->peer_reconfig = 0;
    s->peer_zcsum = 0;
    while (off + 4 <= n) {
        uint16_t t = rtc_rd16(p + off), l = rtc_rd16(p + off + 2);
        size_t i;
        if (l < 4 || off + l > n)
            break;
        if (t == 0xC000)
            s->peer_prsctp = 1;
        else if (t == 0x8001 && l == 8 && rtc_rd32(p + off + 4) == RTC_SCTP_EDMID_DTLS)
            s->peer_zcsum = 1;
        else if (t == 0x8008) {
            for (i = 4; i < l; i++) {
                if (p[off + i] == RTC_SCTP_FORWARD_TSN)
                    s->peer_prsctp = 1;
                if (p[off + i] == RTC_SCTP_RECONFIG)
                    s->peer_reconfig = 1;
            }
        } else if (t == 7 && cookie) {
            *cookie = p + off + 4;
            *cookie_len = (uint16_t)(l - 4);
        }
        off += ((size_t)l + 3) & ~(size_t)3;
    }
}

static void sctp_on_init(rtc_sctp* s, const uint8_t* c, uint32_t len, uint64_t now)
{
    uint8_t ck[SCTP_COOKIE_LEN];
    uint8_t* b;
    uint32_t peer_tag, peer_rwnd, peer_itsn, my_tag, my_itsn, off = 16;
    if (len < 20)
        return;
    peer_tag = rtc_rd32(c + 4);
    peer_rwnd = rtc_rd32(c + 8);
    peer_itsn = rtc_rd32(c + 16);
    if (!peer_tag || rtc_rd16(c + 12) == 0 || rtc_rd16(c + 14) == 0)
        return;
    if (s->state == RTC_SCTP_SHUTDOWN_ACK_SENT) {
        sctp_send_simple(s, RTC_SCTP_SHUTDOWN_ACK, 0, s->peer_tag);
        return;
    }
    if (s->state == RTC_SCTP_ESTABLISHED || s->state == RTC_SCTP_ABORTED || s->state == RTC_SCTP_SHUTDOWN_SENT
        || s->state == RTC_SCTP_SHUTDOWN_RECEIVED)
        return;
    sctp_parse_init_params(s, c + 20, len - 20, NULL, NULL);
    if (s->state == RTC_SCTP_CLOSED) {
        my_tag = sctp_nonzero(s);
        my_itsn = rtc_rng_u32(s->rng);
    } else {
        my_tag = s->my_tag;
        my_itsn = s->my_itsn;
    }
    rtc_wr32(ck, my_tag);
    rtc_wr32(ck + 4, peer_tag);
    rtc_wr32(ck + 8, my_itsn);
    rtc_wr32(ck + 12, peer_itsn);
    rtc_wr32(ck + 16, peer_rwnd);
    ck[20] = s->peer_prsctp;
    ck[21] = s->peer_reconfig;
    ck[22] = s->peer_zcsum;
    ck[23] = 0;
    rtc_wr64(ck + 24, now);
    rtc_wr16(ck + 32, rtc_rd16(c + 12));
    rtc_wr16(ck + 34, rtc_rd16(c + 14));
    sctp_cookie_mac(s, ck, 36, ck + 36);
    sctp_pkt_begin(s, peer_tag);
    b = s->pkt + s->pkt_len + 4;
    rtc_wr32(b, my_tag);
    rtc_wr32(b + 4, RTC_SCTP_RWND);
    rtc_wr16(b + 8, s->local_streams);
    rtc_wr16(b + 10, s->local_streams);
    rtc_wr32(b + 12, my_itsn);
    rtc_wr16(b + off, 7);
    rtc_wr16(b + off + 2, 4 + SCTP_COOKIE_LEN);
    memcpy(b + off + 4, ck, SCTP_COOKIE_LEN);
    off += 4 + SCTP_COOKIE_LEN;
    sctp_init_params(s, b, &off);
    sctp_chunk(s, RTC_SCTP_INIT_ACK, 0, off);
    sctp_pkt_emit(s);
}

static void sctp_set_streams(rtc_sctp* s, uint16_t peer_os, uint16_t peer_mis)
{
    s->n_in = peer_os < s->local_streams ? peer_os : s->local_streams;
    s->n_out = peer_mis < s->local_streams ? peer_mis : s->local_streams;
}

static void sctp_establish(rtc_sctp* s, uint32_t peer_rwnd)
{
    uint32_t m = s->mtu;
    s->rx_cum = s->peer_itsn - 1;
    s->reconf_peer_seq = s->peer_itsn;
    s->peer_rwnd = peer_rwnd;
    s->cwnd = 4 * m < (2 * m > 4380 ? 2 * m : 4380) ? 4 * m : (2 * m > 4380 ? 2 * m : 4380);
    s->ssthresh = peer_rwnd;
    s->rto_us = RTC_SCTP_RTO_INITIAL_US;
    s->t1_us = 0;
    free(s->cookie);
    s->cookie = NULL;
    sctp_set_state(s, RTC_SCTP_ESTABLISHED);
}

static void sctp_send_cookie_echo(rtc_sctp* s)
{
    uint8_t* b;
    if (!s->cookie)
        return;
    sctp_pkt_begin(s, s->peer_tag);
    b = sctp_chunk(s, RTC_SCTP_COOKIE_ECHO, 0, s->cookie_len);
    if (!b) {
        s->pkt_len = 0;
        return;
    }
    memcpy(b, s->cookie, s->cookie_len);
    sctp_pkt_emit(s);
}

static void sctp_on_init_ack(rtc_sctp* s, const uint8_t* c, uint32_t len, uint64_t now)
{
    const uint8_t* ck = NULL;
    uint16_t cl = 0;
    if (s->state != RTC_SCTP_COOKIE_WAIT || len < 20)
        return;
    sctp_parse_init_params(s, c + 20, len - 20, &ck, &cl);
    if (!ck || !rtc_rd32(c + 4) || !rtc_rd16(c + 12) || !rtc_rd16(c + 14))
        return;
    s->peer_tag = rtc_rd32(c + 4);
    if (4u + cl > s->mtu - 12) {
        sctp_send_simple(s, RTC_SCTP_ABORT, 0, s->peer_tag);
        s->t1_us = 0;
        sctp_set_state(s, RTC_SCTP_ABORTED);
        return;
    }
    s->peer_rwnd = rtc_rd32(c + 8);
    s->peer_itsn = rtc_rd32(c + 16);
    sctp_set_streams(s, rtc_rd16(c + 12), rtc_rd16(c + 14));
    s->cookie = (uint8_t*)malloc(cl);
    if (!s->cookie)
        return;
    memcpy(s->cookie, ck, cl);
    s->cookie_len = cl;
    s->state = RTC_SCTP_COOKIE_ECHOED;
    s->init_tries = 1;
    s->t1_rto = RTC_SCTP_RTO_INITIAL_US;
    s->t1_us = now + s->t1_rto;
    sctp_send_cookie_echo(s);
}

static int sctp_on_cookie_echo(rtc_sctp* s, uint32_t vtag, const uint8_t* c, uint32_t len, uint64_t now)
{
    uint8_t mac[16];
    uint32_t my_tag, peer_tag;
    uint64_t ts;
    if (len != 4 + SCTP_COOKIE_LEN)
        return 0;
    c += 4;
    sctp_cookie_mac(s, c, 36, mac);
    if (!dyn_ct_equal(mac, c + 36, 16))
        return 0;
    ts = rtc_rd64(c + 24);
    if (now < ts || now - ts > 60000000u)
        return 0;
    my_tag = rtc_rd32(c);
    peer_tag = rtc_rd32(c + 4);
    if (vtag != my_tag)
        return 0;
    if (s->state == RTC_SCTP_ESTABLISHED) {
        if (my_tag != s->my_tag || peer_tag != s->peer_tag)
            return 0;
        s->cookie_ack_pending = 1;
        return 1;
    }
    if (s->state == RTC_SCTP_COOKIE_WAIT || s->state == RTC_SCTP_COOKIE_ECHOED) {
        if (my_tag != s->my_tag)
            return 0;
    } else if (s->state != RTC_SCTP_CLOSED)
        return 0;
    s->my_tag = my_tag;
    s->peer_tag = peer_tag;
    s->my_itsn = rtc_rd32(c + 8);
    s->peer_itsn = rtc_rd32(c + 12);
    s->peer_prsctp = c[20];
    s->peer_reconfig = c[21];
    s->peer_zcsum = c[22];
    sctp_set_streams(s, rtc_rd16(c + 32), rtc_rd16(c + 34));
    if (s->state == RTC_SCTP_CLOSED)
        sctp_reset_tx(s);
    s->cookie_ack_pending = 1;
    sctp_establish(s, rtc_rd32(c + 16));
    return 1;
}

static void sctp_rtx_push(rtc_sctp* s, uint32_t tsn)
{
    if (s->rtxq_len == s->rtxq_cap) {
        uint32_t nc = s->rtxq_cap ? s->rtxq_cap * 2 : 64, i;
        uint32_t* n = (uint32_t*)malloc(nc * sizeof(*n));
        if (!n)
            return;
        for (i = 0; i < s->rtxq_len; i++)
            n[i] = s->rtxq[(s->rtxq_head + i) & (s->rtxq_cap - 1)];
        free(s->rtxq);
        s->rtxq = n;
        s->rtxq_cap = nc;
        s->rtxq_head = 0;
    }
    s->rtxq[(s->rtxq_head + s->rtxq_len) & (s->rtxq_cap - 1)] = tsn;
    s->rtxq_len++;
}

static rtc_sctp_tx* sctp_tx_at(rtc_sctp* s, uint32_t tsn)
{
    return &s->tx[tsn & (s->tx_cap - 1)];
}

static rtc_sctp_omsg* sctp_om_at(rtc_sctp* s, uint32_t seq)
{
    return &s->om[seq & (s->om_cap - 1)];
}

static void sctp_msg_sent_done(rtc_sctp* s, rtc_sctp_omsg* m)
{
    rtc_sctp_stream* st;
    if (m->counted)
        return;
    m->counted = 1;
    st = sctp_stream(s, m->stream);
    if (st)
        st->queued -= m->len < st->queued ? m->len : st->queued;
    s->cb.sent(s->ud, m->stream, m->len);
}

static void sctp_abandon(rtc_sctp* s, uint32_t seq)
{
    rtc_sctp_omsg* m = sctp_om_at(s, seq);
    uint32_t tsn;
    if (m->abandoned)
        return;
    m->abandoned = 1;
    if (m->off > 0) {
        for (tsn = m->first_tsn; rtc_seq_lt32(tsn, s->next_tsn); tsn++) {
            rtc_sctp_tx* e;
            if (rtc_seq_lt32(tsn, s->ack_base))
                continue;
            e = sctp_tx_at(s, tsn);
            if (e->msg != seq)
                break;
            if (e->state == TXS_OUT)
                s->flight -= e->len < s->flight ? e->len : s->flight;
            if (e->state != TXS_GAP)
                e->state = TXS_ABANDON;
        }
    }
    if (m->off < m->len) {
        m->off = m->len;
        sctp_msg_sent_done(s, m);
        if (s->om_chunk == seq)
            s->om_chunk++;
    }
}

static int sctp_should_abandon(rtc_sctp* s, rtc_sctp_tx* e, uint64_t now)
{
    rtc_sctp_omsg* m = sctp_om_at(s, e->msg);
    if (!s->peer_prsctp)
        return 0;
    if (m->max_rtx >= 0 && e->rtx >= (uint32_t)m->max_rtx)
        return 1;
    if (m->expire_us && now >= m->expire_us)
        return 1;
    return 0;
}

static void sctp_update_adv(rtc_sctp* s)
{
    uint32_t a = s->adv_ack;
    if (rtc_seq_lt32(a, s->ack_base - 1))
        a = s->ack_base - 1;
    while (rtc_seq_lt32(a + 1, s->next_tsn) && sctp_tx_at(s, a + 1)->state == TXS_ABANDON)
        a++;
    if (rtc_seq_lt32(s->ack_base - 1, a)) {
        if (a != s->adv_ack || !s->fwd_pending)
            s->fwd_pending = 1;
        s->adv_ack = a;
    }
}

static void sctp_free_head(rtc_sctp* s)
{
    while (s->om_head != s->om_chunk) {
        rtc_sctp_omsg* m = sctp_om_at(s, s->om_head);
        if (m->off < m->len || m->unacked)
            break;
        free(m->data);
        m->data = NULL;
        s->om_head++;
    }
    if (s->om_head == s->om_tail && s->om_cap > RTC_SCTP_OM_KEEP) {
        rtc_sctp_omsg* nm = (rtc_sctp_omsg*)calloc(64, sizeof(*nm));
        if (nm) {
            free(s->om);
            s->om = nm;
            s->om_cap = 64;
        }
    }
}

static void sctp_rto_sample(rtc_sctp* s, uint32_t r)
{
    if (!s->have_rtt) {
        s->srtt_us = r;
        s->rttvar_us = r / 2;
        s->have_rtt = 1;
    } else {
        uint32_t d = s->srtt_us > r ? s->srtt_us - r : r - s->srtt_us;
        s->rttvar_us = s->rttvar_us - s->rttvar_us / 4 + d / 4;
        s->srtt_us = s->srtt_us - s->srtt_us / 8 + r / 8;
    }
    s->rto_us = s->srtt_us + 4 * (s->rttvar_us ? s->rttvar_us : 1);
    if (s->rto_us < RTC_SCTP_RTO_MIN_US)
        s->rto_us = RTC_SCTP_RTO_MIN_US;
    if (s->rto_us > RTC_SCTP_RTO_MAX_US)
        s->rto_us = RTC_SCTP_RTO_MAX_US;
}

static void sctp_on_sack(rtc_sctp* s, const uint8_t* c, uint32_t len, uint64_t now)
{
    uint32_t cum, a_rwnd, ngap, ndup, i, acked = 0, highest, cwnd_full, prev_ge = 0;
    int cum_moved = 0, have_rtt = 0, newly_gap = 0;
    uint32_t rtt = 0;
    if (!SCTP_LIVE(s))
        return;
    if (len < 16)
        return;
    cum = rtc_rd32(c + 4);
    a_rwnd = rtc_rd32(c + 8);
    ngap = rtc_rd16(c + 12);
    ndup = rtc_rd16(c + 14);
    if (16 + 4 * ngap + 4 * ndup > len)
        return;
    if (rtc_seq_lt32(cum, s->ack_base - 1))
        return;
    if (!rtc_seq_lt32(cum, s->next_tsn))
        return;
    cwnd_full = s->flight + s->mtu >= s->cwnd;
    while (rtc_seq_le32(s->ack_base, cum)) {
        rtc_sctp_tx* e = sctp_tx_at(s, s->ack_base);
        rtc_sctp_omsg* m = sctp_om_at(s, e->msg);
        if (e->state == TXS_OUT) {
            s->flight -= e->len < s->flight ? e->len : s->flight;
            acked += e->len;
            if (e->rtx == 0) {
                rtt = (uint32_t)(now - e->sent_us);
                have_rtt = 1;
            }
        } else if (e->state == TXS_RTX)
            acked += e->len;
        if (m->unacked)
            m->unacked--;
        s->ack_base++;
        cum_moved = 1;
    }
    highest = cum;
    for (i = 0; i < ngap; i++) {
        uint32_t gs = rtc_rd16(c + 16 + 4 * i), ge = rtc_rd16(c + 18 + 4 * i);
        uint32_t st = cum + gs, en = cum + ge, t;
        if (gs <= prev_ge || ge < gs || !rtc_seq_lt32(en, s->next_tsn))
            break;
        prev_ge = ge;
        for (t = st; rtc_seq_le32(t, en); t++) {
            rtc_sctp_tx* e;
            if (rtc_seq_lt32(t, s->ack_base))
                continue;
            e = sctp_tx_at(s, t);
            if (e->state == TXS_OUT) {
                s->flight -= e->len < s->flight ? e->len : s->flight;
                acked += e->len;
                e->state = TXS_GAP;
                newly_gap = 1;
                if (rtc_seq_lt32(highest, t))
                    highest = t;
            } else if (e->state == TXS_RTX) {
                acked += e->len;
                e->state = TXS_GAP;
                newly_gap = 1;
                if (rtc_seq_lt32(highest, t))
                    highest = t;
            }
        }
    }
    if (have_rtt)
        sctp_rto_sample(s, rtt);
    if (s->in_recovery && rtc_seq_le32(s->recovery_tsn, cum))
        s->in_recovery = 0;
    if (newly_gap || ngap) {
        uint32_t t;
        int entered = 0;
        for (t = s->ack_base; rtc_seq_lt32(t, highest); t++) {
            rtc_sctp_tx* e = sctp_tx_at(s, t);
            if (e->state != TXS_OUT || e->fast)
                continue;
            if (++e->miss >= 3) {
                if (sctp_should_abandon(s, e, now)) {
                    sctp_abandon(s, e->msg);
                    continue;
                }
                e->fast = 1;
                e->state = TXS_RTX;
                s->flight -= e->len < s->flight ? e->len : s->flight;
                sctp_rtx_push(s, t);
                s->st.fast_rtx++;
                if (!s->in_recovery && !entered) {
                    entered = 1;
                    s->ssthresh = s->cwnd / 2 > 4 * s->mtu ? s->cwnd / 2 : 4 * s->mtu;
                    s->cwnd = s->ssthresh;
                    s->pba = 0;
                    s->in_recovery = 1;
                    s->recovery_tsn = s->next_tsn - 1;
                }
            }
        }
    }
    (void)ndup;
    if (acked && cum_moved && !s->in_recovery) {
        if (s->cwnd <= s->ssthresh) {
            if (cwnd_full)
                s->cwnd += acked < s->mtu ? acked : s->mtu;
        } else {
            s->pba += acked;
            if (s->pba >= s->cwnd && cwnd_full) {
                s->pba -= s->cwnd;
                s->cwnd += s->mtu;
            }
        }
    }
    s->peer_rwnd = a_rwnd > s->flight ? a_rwnd - s->flight : 0;
    if (cum_moved)
        s->assoc_errors = 0;
    if (s->ack_base == s->next_tsn || s->flight == 0) {
        int out = 0;
        uint32_t t;
        for (t = s->ack_base; rtc_seq_lt32(t, s->next_tsn); t++) {
            uint8_t st = sctp_tx_at(s, t)->state;
            if (st == TXS_OUT || st == TXS_RTX) {
                out = 1;
                break;
            }
        }
        if (!out && !(s->peer_prsctp && rtc_seq_lt32(cum, s->adv_ack)))
            s->t3_us = 0;
        else if (cum_moved)
            s->t3_us = now + s->rto_us;
    } else if (cum_moved)
        s->t3_us = now + s->rto_us;
    if (s->fwd_pending && rtc_seq_le32(s->adv_ack, cum))
        s->fwd_pending = 0;
    if (s->peer_prsctp) {
        sctp_update_adv(s);
        if (rtc_seq_lt32(cum, s->adv_ack))
            s->fwd_pending = 1;
    }
    sctp_free_head(s);
}

static void sctp_t3_expire(rtc_sctp* s, uint64_t now)
{
    uint32_t t;
    s->st.t3_expiries++;
    if (s->fwd_pending == 2)
        s->fwd_pending = 1;
    s->ssthresh = s->cwnd / 2 > 4 * s->mtu ? s->cwnd / 2 : 4 * s->mtu;
    s->cwnd = s->mtu;
    s->pba = 0;
    s->in_recovery = 0;
    s->rto_us = s->rto_us * 2 > RTC_SCTP_RTO_MAX_US ? RTC_SCTP_RTO_MAX_US : s->rto_us * 2;
    s->rtxq_len = 0;
    for (t = s->ack_base; rtc_seq_lt32(t, s->next_tsn); t++) {
        rtc_sctp_tx* e = sctp_tx_at(s, t);
        if (e->state != TXS_OUT && e->state != TXS_RTX)
            continue;
        if (sctp_should_abandon(s, e, now)) {
            sctp_abandon(s, e->msg);
            continue;
        }
        if (e->state == TXS_OUT)
            s->flight -= e->len < s->flight ? e->len : s->flight;
        e->state = TXS_RTX;
        sctp_rtx_push(s, t);
    }
    if (s->peer_prsctp)
        sctp_update_adv(s);
    s->t3_us = now + s->rto_us;
    if (++s->assoc_errors > RTC_SCTP_ASSOC_MAX_RTX) {
        s->t3_us = 0;
        sctp_set_state(s, RTC_SCTP_ABORTED);
    }
}

static void sctp_bit_set(rtc_sctp* s, uint32_t tsn)
{
    uint32_t i = tsn & (RTC_SCTP_RX_WIN - 1);
    s->rx_bits[i >> 6] |= 1ull << (i & 63);
}

static int sctp_bit_get(const rtc_sctp* s, uint32_t tsn)
{
    uint32_t i = tsn & (RTC_SCTP_RX_WIN - 1);
    return (int)((s->rx_bits[i >> 6] >> (i & 63)) & 1);
}

static void sctp_bit_clr(rtc_sctp* s, uint32_t tsn)
{
    uint32_t i = tsn & (RTC_SCTP_RX_WIN - 1);
    s->rx_bits[i >> 6] &= ~(1ull << (i & 63));
}

static void sctp_pg_seen(rtc_sctp* s, uint16_t stream)
{
    uint32_t pg = (uint32_t)stream / RTC_SCTP_SPAGE;
    s->pg_rx_seen[pg >> 3] |= (uint8_t)(1u << (pg & 7));
}

static void sctp_deliver(rtc_sctp* s, uint16_t stream, uint16_t ssn, uint32_t ppid, int unordered, const uint8_t* p, size_t n)
{
    if (!unordered) {
        rtc_sctp_stream* st = sctp_stream_at(s, stream);
        if (st && st->in_sync && rtc_seq_lt16(ssn, st->in_ssn))
            return;
        if (st) {
            st->in_ssn = (uint16_t)(ssn + 1);
            st->in_sync = 1;
        } else
            sctp_pg_seen(s, stream);
    }
    s->st.msgs_rx++;
    s->st.bytes_rx += n;
    s->cb.message(s->ud, stream, ppid, p, n);
}

static void sctp_asm_drop(rtc_sctp* s)
{
    s->asm_active = 0;
    s->asm_len = 0;
    if (s->asm_cap > RTC_SCTP_ASM_KEEP) {
        free(s->asm_buf);
        s->asm_buf = NULL;
        s->asm_cap = 0;
    }
}

static void sctp_asm_append(rtc_sctp* s, const uint8_t* p, uint32_t n)
{
    if (s->asm_active != 1)
        return;
    if (s->asm_len + n > s->max_msg) {
        sctp_asm_drop(s);
        s->asm_active = 2;
        return;
    }
    if (s->asm_len + n > s->asm_cap) {
        uint32_t nc = s->asm_cap ? s->asm_cap : 4096;
        uint8_t* nb;
        while (nc < s->asm_len + n)
            nc *= 2;
        nb = (uint8_t*)realloc(s->asm_buf, nc);
        if (!nb) {
            sctp_asm_drop(s);
            s->asm_active = 2;
            return;
        }
        s->asm_buf = nb;
        s->asm_cap = nc;
    }
    memcpy(s->asm_buf + s->asm_len, p, n);
    s->asm_len += n;
}

static void sctp_inorder(rtc_sctp* s, uint8_t flags, uint16_t stream, uint16_t ssn, uint32_t ppid, const uint8_t* p, uint32_t n)
{
    int b = flags & F_B, e = flags & F_E;
    if (b && e) {
        if (s->asm_active)
            sctp_asm_drop(s);
        sctp_deliver(s, stream, ssn, ppid, flags & F_U, p, n);
        return;
    }
    if (b) {
        if (s->asm_active)
            sctp_asm_drop(s);
        s->asm_active = 1;
        s->asm_stream = stream;
        s->asm_ppid = ppid;
        s->asm_flags = flags;
        s->asm_len = 0;
        sctp_asm_append(s, p, n);
        return;
    }
    if (s->asm_active == 1 && s->asm_stream == stream) {
        sctp_asm_append(s, p, n);
        if (e && s->asm_active == 1) {
            s->asm_active = 0;
            sctp_deliver(s, stream, ssn, s->asm_ppid, s->asm_flags & F_U, s->asm_buf, s->asm_len);
            s->asm_len = 0;
            if (s->asm_cap > RTC_SCTP_ASM_KEEP) {
                free(s->asm_buf);
                s->asm_buf = NULL;
                s->asm_cap = 0;
            }
        }
        return;
    }
    if (e)
        s->asm_active = 0;
}

static rtc_sctp_rxc* sctp_slot_get(rtc_sctp* s, uint32_t n)
{
    rtc_sctp_rxc* x;
    if (n > RTC_SCTP_SLOT)
        return (rtc_sctp_rxc*)malloc(sizeof(*x) + n);
    if (s->slot_nfree)
        return s->slot_stack[--s->slot_nfree];
    return (rtc_sctp_rxc*)malloc(sizeof(*x) + RTC_SCTP_SLOT);
}

static void sctp_slot_put(rtc_sctp* s, rtc_sctp_rxc* x)
{
    if (x->len > RTC_SCTP_SLOT || s->slot_nfree >= RTC_SCTP_SLOT_CACHE
        || (!s->slot_stack && !(s->slot_stack = (rtc_sctp_rxc**)malloc(RTC_SCTP_SLOT_CACHE * sizeof(*s->slot_stack))))) {
        free(x);
        return;
    }
    s->slot_stack[s->slot_nfree++] = x;
}

#define SCTP_COST(n) ((n) < RTC_SCTP_SLOT ? RTC_SCTP_SLOT : (n))

static uint32_t sctp_hix_home(const rtc_sctp_rxc* c)
{
    return (((uint32_t)c->stream << 16 | c->ssn) * 0x9E3779B1u) >> 21;
}

static int sctp_hix_head(const rtc_sctp_rxc* c)
{
    return c && c != &rtc_sctp_tomb && (c->flags & (F_B | F_U)) == F_B;
}

static void sctp_hix_put(rtc_sctp* s, uint32_t tsn, const rtc_sctp_rxc* c)
{
    uint32_t i, n;
    if (!s->hix && !(s->hix = (uint16_t*)calloc(RTC_SCTP_HIX, sizeof(*s->hix))))
        return;
    for (i = sctp_hix_home(c), n = 0; n < RTC_SCTP_HIX; i = (i + 1) & (RTC_SCTP_HIX - 1), n++)
        if (!s->hix[i]) {
            s->hix[i] = (uint16_t)((tsn & (RTC_SCTP_RX_WIN - 1)) + 1);
            return;
        }
}

static void sctp_hix_del(rtc_sctp* s, uint32_t tsn, const rtc_sctp_rxc* c)
{
    uint16_t v = (uint16_t)((tsn & (RTC_SCTP_RX_WIN - 1)) + 1);
    uint32_t i, j, k, n;
    if (!s->hix || !sctp_hix_head(c))
        return;
    for (i = sctp_hix_home(c), n = 0; s->hix[i] != v; i = (i + 1) & (RTC_SCTP_HIX - 1))
        if (!s->hix[i] || ++n >= RTC_SCTP_HIX)
            return;
    for (j = i;;) {
        j = (j + 1) & (RTC_SCTP_HIX - 1);
        if (!s->hix[j])
            break;
        k = sctp_hix_home(s->stash[s->hix[j] - 1]);
        if (((j - k) & (RTC_SCTP_HIX - 1)) >= ((j - i) & (RTC_SCTP_HIX - 1))) {
            s->hix[i] = s->hix[j];
            i = j;
        }
    }
    s->hix[i] = 0;
}

static void sctp_stash_take(rtc_sctp* s, uint32_t tsn)
{
    rtc_sctp_rxc** slot = &s->stash[tsn & (RTC_SCTP_RX_WIN - 1)];
    rtc_sctp_rxc* c = *slot;
    sctp_hix_del(s, tsn, c);
    *slot = NULL;
    sctp_bit_clr(s, tsn);
    if (!c || c == &rtc_sctp_tomb)
        return;
    s->stash_bytes -= SCTP_COST(c->len) < s->stash_bytes ? SCTP_COST(c->len) : s->stash_bytes;
    sctp_inorder(s, c->flags, c->stream, c->ssn, c->ppid, c->data, c->len);
    sctp_slot_put(s, c);
}

static void sctp_drain_stash(rtc_sctp* s)
{
    if (!s->stash)
        return;
    while (sctp_bit_get(s, s->rx_cum + 1)) {
        s->rx_cum++;
        sctp_stash_take(s, s->rx_cum);
    }
}

static void sctp_check_in_reset(rtc_sctp* s);

static rtc_sctp_rxc* sctp_stashed(const rtc_sctp* s, uint32_t tsn)
{
    rtc_sctp_rxc* c;
    if (!rtc_seq_lt32(s->rx_cum, tsn) || tsn - s->rx_cum > RTC_SCTP_RX_WIN - 1 || !sctp_bit_get(s, tsn))
        return NULL;
    c = s->stash[tsn & (RTC_SCTP_RX_WIN - 1)];
    return c == &rtc_sctp_tomb ? NULL : c;
}

static void sctp_tomb(rtc_sctp* s, uint32_t tsn)
{
    rtc_sctp_rxc** slot = &s->stash[tsn & (RTC_SCTP_RX_WIN - 1)];
    rtc_sctp_rxc* c = *slot;
    sctp_hix_del(s, tsn, c);
    s->stash_bytes -= SCTP_COST(c->len) < s->stash_bytes ? SCTP_COST(c->len) : s->stash_bytes;
    *slot = &rtc_sctp_tomb;
    sctp_slot_put(s, c);
}

static int sctp_find_head(rtc_sctp* s, uint16_t stream, uint16_t ssn, uint32_t* out)
{
    rtc_sctp_rxc key;
    uint32_t i, n;
    if (!s->hix)
        return 0;
    key.stream = stream;
    key.ssn = ssn;
    for (i = sctp_hix_home(&key), n = 0; s->hix[i] && n < RTC_SCTP_HIX; i = (i + 1) & (RTC_SCTP_HIX - 1), n++) {
        uint32_t t = s->rx_cum + 1 + ((s->hix[i] - 1u - (s->rx_cum + 1)) & (RTC_SCTP_RX_WIN - 1));
        rtc_sctp_rxc* c = sctp_stashed(s, t);
        s->st.early_steps++;
        if (c && c->stream == stream && c->ssn == ssn && (c->flags & (F_B | F_U)) == F_B) {
            *out = t;
            return 1;
        }
    }
    return 0;
}

static int sctp_seg_join(const rtc_sctp_rxc* a, const rtc_sctp_rxc* b)
{
    return a->stream == b->stream && !((a->flags ^ b->flags) & F_U) && !(a->flags & F_E) && !(b->flags & F_B);
}

static void sctp_seg_link(rtc_sctp* s, uint32_t tsn, uint32_t* ob, uint32_t* oe)
{
    rtc_sctp_rxc* x = sctp_stashed(s, tsn);
    rtc_sctp_rxc* l = sctp_stashed(s, tsn - 1);
    rtc_sctp_rxc* r = sctp_stashed(s, tsn + 1);
    rtc_sctp_rxc *cb, *ce;
    uint32_t b = tsn, e = tsn;
    s->st.early_steps++;
    if (l && sctp_seg_join(l, x))
        b = l->seg;
    if (r && sctp_seg_join(x, r))
        e = r->seg;
    cb = sctp_stashed(s, b);
    ce = sctp_stashed(s, e);
    x->seg = tsn;
    if (cb)
        cb->seg = e;
    if (ce)
        ce->seg = b;
    *ob = b;
    *oe = e;
}

static int sctp_seg_deliver(rtc_sctp* s, uint32_t b)
{
    rtc_sctp_rxc* cb = sctp_stashed(s, b);
    rtc_sctp_rxc* ce;
    uint32_t e, t, total = 0;
    if (!cb || !(cb->flags & F_B))
        return 0;
    e = cb->seg;
    ce = sctp_stashed(s, e);
    if (!ce || !(ce->flags & F_E))
        return 0;
    if (!(cb->flags & F_U)) {
        rtc_sctp_stream* st = sctp_stream_at(s, cb->stream);
        if (!st || !st->in_sync || cb->ssn != st->in_ssn)
            return 0;
    }
    if (b == e) {
        sctp_deliver(s, cb->stream, cb->ssn, cb->ppid, cb->flags & F_U, cb->data, cb->len);
    } else {
        uint8_t* buf;
        for (t = b; rtc_seq_le32(t, e); t++)
            total += sctp_stashed(s, t)->len;
        buf = (uint8_t*)malloc(total ? total : 1);
        if (!buf)
            return 0;
        total = 0;
        for (t = b; rtc_seq_le32(t, e); t++) {
            rtc_sctp_rxc* f = sctp_stashed(s, t);
            memcpy(buf + total, f->data, f->len);
            total += f->len;
        }
        sctp_deliver(s, cb->stream, cb->ssn, cb->ppid, cb->flags & F_U, buf, total);
        free(buf);
    }
    for (t = b; rtc_seq_le32(t, e); t++)
        sctp_tomb(s, t);
    return 1;
}

static void sctp_try_early(rtc_sctp* s, uint32_t tsn)
{
    rtc_sctp_rxc* cb;
    rtc_sctp_stream* st;
    uint32_t b, e;
    uint16_t stream;
    int unordered;
    sctp_seg_link(s, tsn, &b, &e);
    cb = sctp_stashed(s, b);
    if (!cb)
        return;
    stream = cb->stream;
    unordered = cb->flags & F_U;
    if (!sctp_seg_deliver(s, b) || unordered)
        return;
    st = sctp_stream_at(s, stream);
    while (st && sctp_find_head(s, stream, st->in_ssn, &b)) {
        e = sctp_stashed(s, b)->seg;
        if (!sctp_seg_deliver(s, b))
            return;
    }
}

static void sctp_abort_cause(rtc_sctp* s, uint16_t cause, uint32_t val)
{
    uint8_t* b;
    s->pkt_len = 0;
    sctp_pkt_begin(s, s->peer_tag);
    b = sctp_chunk(s, RTC_SCTP_ABORT, 0, 8);
    if (b) {
        rtc_wr16(b, cause);
        rtc_wr16(b + 2, 8);
        rtc_wr32(b + 4, val);
        sctp_pkt_emit(s);
    }
    s->pkt_len = 0;
    s->t1_us = 0;
    s->t3_us = 0;
    sctp_set_state(s, RTC_SCTP_ABORTED);
}

static void sctp_on_data(rtc_sctp* s, uint8_t flags, const uint8_t* c, uint32_t len)
{
    uint32_t tsn, ppid, n;
    uint16_t stream, ssn;
    if (!SCTP_LIVE(s))
        return;
    if (len < 16)
        return;
    tsn = rtc_rd32(c + 4);
    if (len == 16) {
        sctp_abort_cause(s, 9, tsn);
        return;
    }
    stream = rtc_rd16(c + 8);
    ssn = rtc_rd16(c + 10);
    ppid = rtc_rd32(c + 12);
    n = len - 16;
    s->need_sack = 1;
    if (flags & F_I)
        s->sack_now = 1;
    if (rtc_seq_le32(tsn, s->rx_cum) || (tsn != s->rx_cum + 1 && sctp_bit_get(s, tsn))) {
        if (s->ndups < 16)
            s->dups[s->ndups++] = tsn;
        s->sack_now = 1;
        return;
    }
    if (tsn - s->rx_cum > RTC_SCTP_RX_WIN - 1) {
        s->sack_now = 1;
        return;
    }
    if (stream >= s->n_in) {
        s->bad_sid_pending = 1;
        s->bad_sid = stream;
        s->sack_now = 1;
        if (tsn == s->rx_cum + 1) {
            s->rx_cum = tsn;
            if (s->asm_active)
                sctp_asm_drop(s);
            sctp_drain_stash(s);
            sctp_check_in_reset(s);
            return;
        }
        if (!s->stash) {
            s->stash = (rtc_sctp_rxc**)calloc(RTC_SCTP_RX_WIN, sizeof(*s->stash));
            if (!s->stash)
                return;
        }
        sctp_bit_set(s, tsn);
        s->stash[tsn & (RTC_SCTP_RX_WIN - 1)] = &rtc_sctp_tomb;
        return;
    }
    if (tsn == s->rx_cum + 1) {
        s->rx_cum = tsn;
        sctp_inorder(s, flags, stream, ssn, ppid, c + 16, n);
        sctp_drain_stash(s);
        if (s->rx_cum != tsn || s->stash_bytes)
            s->sack_now = 1;
        sctp_check_in_reset(s);
        return;
    }
    s->sack_now = 1;
    if (!s->stash) {
        s->stash = (rtc_sctp_rxc**)calloc(RTC_SCTP_RX_WIN, sizeof(*s->stash));
        if (!s->stash)
            return;
    }
    if ((flags & (F_U | F_B | F_E)) == (F_U | F_B | F_E)) {
        sctp_bit_set(s, tsn);
        s->stash[tsn & (RTC_SCTP_RX_WIN - 1)] = &rtc_sctp_tomb;
        sctp_deliver(s, stream, ssn, ppid, 1, c + 16, n);
        return;
    }
    if (s->stash_bytes + SCTP_COST(n) > RTC_SCTP_RWND)
        return;
    {
        rtc_sctp_rxc* x = sctp_slot_get(s, n);
        if (!x)
            return;
        x->len = n;
        x->ppid = ppid;
        x->stream = stream;
        x->ssn = ssn;
        x->flags = flags;
        memcpy(x->data, c + 16, n);
        s->stash[tsn & (RTC_SCTP_RX_WIN - 1)] = x;
        s->stash_bytes += SCTP_COST(n);
        sctp_bit_set(s, tsn);
        if (sctp_hix_head(x))
            sctp_hix_put(s, tsn, x);
    }
    sctp_try_early(s, tsn);
}

static void sctp_on_fwd_tsn(rtc_sctp* s, const uint8_t* c, uint32_t len)
{
    uint32_t nc, i, lim;
    if (!SCTP_LIVE(s) || len < 8)
        return;
    nc = rtc_rd32(c + 4);
    s->need_sack = 1;
    s->sack_now = 1;
    if (rtc_seq_le32(nc, s->rx_cum))
        return;
    lim = nc - s->rx_cum > RTC_SCTP_RX_WIN - 1 ? s->rx_cum + RTC_SCTP_RX_WIN - 1 : nc;
    while (rtc_seq_lt32(s->rx_cum, lim)) {
        s->rx_cum++;
        if (s->stash && sctp_bit_get(s, s->rx_cum))
            sctp_stash_take(s, s->rx_cum);
        else if (s->asm_active)
            sctp_asm_drop(s);
    }
    if (s->rx_cum != nc) {
        if (s->asm_active)
            sctp_asm_drop(s);
        s->rx_cum = nc;
    }
    for (i = 8; i + 4 <= len; i += 4) {
        uint16_t sid = rtc_rd16(c + i), ssn = rtc_rd16(c + i + 2);
        rtc_sctp_stream* st = sid < s->n_in ? sctp_stream_at(s, sid) : NULL;
        if (st && (!st->in_sync || !rtc_seq_lt16(ssn, st->in_ssn))) {
            st->in_ssn = (uint16_t)(ssn + 1);
            st->in_sync = 1;
        } else if (!st && sid < s->n_in)
            sctp_pg_seen(s, sid);
    }
    sctp_drain_stash(s);
    sctp_check_in_reset(s);
}

static void sctp_write_sack(rtc_sctp* s)
{
    uint8_t* b;
    uint32_t room = sctp_room(s), maxgaps, ng = 0, off, i;
    uint32_t base = s->rx_cum;
    if (room < 20)
        return;
    while (s->ndups && room < 20 + 4u * s->ndups)
        s->ndups--;
    maxgaps = (room - 20 - 4u * s->ndups) / 4;
    b = s->pkt + s->pkt_len + 4;
    if (s->stash) {
        uint32_t k = 1, run_start = 0;
        int in_run = 0;
        while (k < RTC_SCTP_RX_WIN && ng < maxgaps) {
            uint32_t tsn = base + k;
            uint32_t bi = tsn & (RTC_SCTP_RX_WIN - 1);
            uint64_t w = s->rx_bits[bi >> 6] >> (bi & 63);
            if (!in_run && w == 0 && (bi & 63) == 0 && k + 64 <= RTC_SCTP_RX_WIN) {
                k += 64;
                continue;
            }
            if (w & 1) {
                if (!in_run) {
                    in_run = 1;
                    run_start = k;
                }
            } else if (in_run) {
                rtc_wr16(b + 12 + 4 * ng, run_start);
                rtc_wr16(b + 14 + 4 * ng, k - 1);
                ng++;
                in_run = 0;
            }
            k++;
        }
        if (in_run && ng < maxgaps) {
            rtc_wr16(b + 12 + 4 * ng, run_start);
            rtc_wr16(b + 14 + 4 * ng, k - 1);
            ng++;
        }
    }
    rtc_wr32(b, s->rx_cum);
    rtc_wr32(b + 4, sctp_a_rwnd(s));
    rtc_wr16(b + 8, ng);
    rtc_wr16(b + 10, s->ndups);
    off = 12 + 4 * ng;
    for (i = 0; i < s->ndups; i++) {
        rtc_wr32(b + off, s->dups[i]);
        off += 4;
    }
    sctp_chunk(s, RTC_SCTP_SACK, 0, off);
    s->ndups = 0;
    s->need_sack = 0;
    s->sack_now = 0;
    s->pkts_since_sack = 0;
    s->sack_due_us = 0;
}

static int sctp_write_fwd(rtc_sctp* s)
{
    uint8_t* b;
    uint16_t sids[384], ssns[384];
    uint32_t n = 0, t, i, room = sctp_room(s), nc = s->adv_ack;
    if (room < 12)
        return 0;
    for (t = s->ack_base; rtc_seq_le32(t, s->adv_ack); t++) {
        rtc_sctp_tx* e = sctp_tx_at(s, t);
        rtc_sctp_omsg* m = sctp_om_at(s, e->msg);
        if (m->unordered)
            continue;
        for (i = 0; i < n; i++)
            if (sids[i] == m->stream)
                break;
        if (i == n) {
            if (n == 384 || 8 + 4 * (n + 1) > room) {
                nc = t - 1;
                break;
            }
            sids[n] = m->stream;
            ssns[n] = m->ssn;
            n++;
        } else if (rtc_seq_lt16(ssns[i], m->ssn))
            ssns[i] = m->ssn;
    }
    if (rtc_seq_lt32(nc, s->ack_base))
        return 0;
    b = s->pkt + s->pkt_len + 4;
    rtc_wr32(b, nc);
    for (i = 0; i < n; i++) {
        rtc_wr16(b + 4 + 4 * i, sids[i]);
        rtc_wr16(b + 6 + 4 * i, ssns[i]);
    }
    if (!sctp_chunk(s, RTC_SCTP_FORWARD_TSN, 0, 4 + 4 * n))
        return 0;
    s->st.fwd_tsn_tx++;
    return 1;
}

static void sctp_write_reconf_resp(rtc_sctp* s, uint32_t seq, uint32_t result)
{
    uint8_t* b;
    if (sctp_room(s) < 16) {
        sctp_pkt_emit(s);
        sctp_pkt_begin(s, s->peer_tag);
    }
    b = s->pkt + s->pkt_len + 4;
    rtc_wr16(b, 16);
    rtc_wr16(b + 2, 12);
    rtc_wr32(b + 4, seq);
    rtc_wr32(b + 8, result);
    sctp_chunk(s, RTC_SCTP_RECONFIG, 0, 12);
}

static void sctp_check_in_reset(rtc_sctp* s)
{
    uint16_t i;
    if (!s->in_reset_pending || rtc_seq_lt32(s->rx_cum, s->in_reset_tsn))
        return;
    s->in_reset_pending = 0;
    if (s->in_reset_all) {
        uint32_t pg, k;
        s->in_reset_all = 0;
        for (pg = 0; pg < s->npages; pg++)
            for (k = 0; s->spages[pg] && k < RTC_SCTP_SPAGE; k++) {
                uint32_t id = pg * RTC_SCTP_SPAGE + k;
                if (id >= s->n_in)
                    break;
                s->spages[pg][k].in_ssn = 0;
                s->cb.reset_in(s->ud, (uint16_t)id);
            }
    }
    for (i = 0; i < s->in_reset_n; i++) {
        rtc_sctp_stream* st = sctp_stream_at(s, s->in_reset_list[i]);
        if (st)
            st->in_ssn = 0;
        s->cb.reset_in(s->ud, s->in_reset_list[i]);
    }
    s->reconf_peer_seq = s->in_reset_seq + 1;
    s->need_sack = 1;
    s->sack_now = 1;
}

static void sctp_on_reconf(rtc_sctp* s, const uint8_t* c, uint32_t len, uint64_t now)
{
    uint32_t off = 4;
    if (s->state != RTC_SCTP_ESTABLISHED)
        return;
    while (off + 4 <= len) {
        uint16_t t = rtc_rd16(c + off), l = rtc_rd16(c + off + 2);
        if (l < 4 || off + l > len)
            break;
        if (t == 13 && l >= 16) {
            uint32_t seq = rtc_rd32(c + off + 4), last = rtc_rd32(c + off + 12), i;
            if (seq == s->reconf_peer_seq && !s->in_reset_pending) {
                s->in_reset_pending = 1;
                s->in_reset_seq = seq;
                s->in_reset_tsn = last;
                s->in_reset_n = 0;
                s->in_reset_all = l == 16;
                for (i = 16; i + 2 <= l && s->in_reset_n < 256; i += 2)
                    if (rtc_rd16(c + off + i) < s->n_in)
                        s->in_reset_list[s->in_reset_n++] = rtc_rd16(c + off + i);
                sctp_check_in_reset(s);
                s->reconf_resp_due = 1;
                s->reconf_resp_seq = seq;
                s->reconf_resp_result = s->in_reset_pending ? 6 : 1;
                s->reconf_last_result = 1;
            } else if (seq == s->reconf_peer_seq && s->in_reset_pending) {
                s->reconf_resp_due = 1;
                s->reconf_resp_seq = seq;
                s->reconf_resp_result = 6;
            } else if (seq == s->reconf_peer_seq - 1) {
                s->reconf_resp_due = 1;
                s->reconf_resp_seq = seq;
                s->reconf_resp_result = s->reconf_last_result ? s->reconf_last_result : 1;
            } else {
                s->reconf_resp_due = 1;
                s->reconf_resp_seq = seq;
                s->reconf_resp_result = 5;
            }
        } else if (t == 16 && l >= 12) {
            uint32_t seq = rtc_rd32(c + off + 4), result = rtc_rd32(c + off + 8);
            uint16_t i;
            if (s->reconf_inflight && seq == s->reconf_req_seq) {
                if (result == 6) {
                    s->reconf_us = now + s->rto_us;
                } else {
                    s->reconf_inflight = 0;
                    s->reconf_req_seq++;
                    for (i = 0; i < s->reconf_n; i++) {
                        rtc_sctp_stream* st = sctp_stream(s, s->reconf_list[i]);
                        if (st) {
                            if (result <= 1)
                                st->out_ssn = 0;
                            st->reset_want = 0;
                            st->reset_sent = 0;
                        }
                        s->cb.reset_out(s->ud, s->reconf_list[i]);
                    }
                    s->reconf_n = 0;
                }
            }
        }
        off += ((uint32_t)l + 3) & ~3u;
    }
}

static void sctp_write_reconf_req(rtc_sctp* s)
{
    uint8_t* b;
    uint32_t i, body;
    if (sctp_room(s) < 20 + 2u * s->reconf_n)
        return;
    b = s->pkt + s->pkt_len + 4;
    body = 16 + 2u * s->reconf_n;
    rtc_wr16(b, 13);
    rtc_wr16(b + 2, body);
    rtc_wr32(b + 4, s->reconf_req_seq);
    rtc_wr32(b + 8, s->reconf_peer_seq - 1);
    rtc_wr32(b + 12, s->reconf_last_tsn);
    for (i = 0; i < s->reconf_n; i++)
        rtc_wr16(b + 16 + 2 * i, s->reconf_list[i]);
    if (body & 3)
        memset(b + body, 0, 4 - (body & 3));
    sctp_chunk(s, RTC_SCTP_RECONFIG, 0, (body + 3) & ~3u);
}

static void sctp_prepare_reconf(rtc_sctp* s, uint64_t now)
{
    uint32_t i, k = 0;
    if (!s->rw_n || s->reconf_inflight || !s->peer_reconfig)
        return;
    s->reconf_n = 0;
    for (i = 0; i < s->rw_n; i++) {
        uint16_t id = s->rw_list[i];
        rtc_sctp_stream* st = sctp_stream_at(s, id);
        if (st && st->reset_want && !st->reset_sent && st->queued == 0 && s->reconf_n < 256) {
            st->reset_sent = 1;
            s->reconf_list[s->reconf_n++] = id;
        } else if (st && st->reset_want && !st->reset_sent)
            s->rw_list[k++] = id;
    }
    s->rw_n = k;
    if (!s->reconf_n)
        return;
    s->reconf_inflight = 1;
    s->reconf_tries = 0;
    s->reconf_last_tsn = s->next_tsn - 1;
    s->reconf_us = now;
}

static void sctp_tx_grow(rtc_sctp* s)
{
    uint32_t nc = s->tx_cap ? s->tx_cap * 2 : 256, t;
    rtc_sctp_tx* n = (rtc_sctp_tx*)malloc(nc * sizeof(*n));
    if (!n)
        return;
    for (t = s->ack_base; rtc_seq_lt32(t, s->next_tsn); t++)
        n[t & (nc - 1)] = s->tx[t & (s->tx_cap - 1)];
    free(s->tx);
    s->tx = n;
    s->tx_cap = nc;
}

static void sctp_write_data(rtc_sctp* s, uint32_t tsn, rtc_sctp_tx* e)
{
    rtc_sctp_omsg* m = sctp_om_at(s, e->msg);
    uint8_t* b = s->pkt + s->pkt_len + 4;
    rtc_wr32(b, tsn);
    rtc_wr16(b + 4, m->stream);
    rtc_wr16(b + 6, m->ssn);
    rtc_wr32(b + 8, m->ppid);
    memcpy(b + 12, m->data + e->off, e->len);
    sctp_chunk(s, RTC_SCTP_DATA, e->flags, 12u + e->len);
}

static void sctp_flush_data(rtc_sctp* s, uint64_t now)
{
    uint32_t maxp = sctp_max_payload(s);
    while (s->rtxq_len) {
        uint32_t tsn = s->rtxq[s->rtxq_head];
        rtc_sctp_tx* e;
        if (rtc_seq_lt32(tsn, s->ack_base)) {
            s->rtxq_head = (s->rtxq_head + 1) & (s->rtxq_cap - 1);
            s->rtxq_len--;
            continue;
        }
        e = sctp_tx_at(s, tsn);
        if (e->state != TXS_RTX) {
            s->rtxq_head = (s->rtxq_head + 1) & (s->rtxq_cap - 1);
            s->rtxq_len--;
            continue;
        }
        if (s->flight > 0 && s->flight + e->len > s->cwnd)
            break;
        if (sctp_room(s) < 16u + e->len) {
            sctp_pkt_emit(s);
            sctp_pkt_begin(s, s->peer_tag);
        }
        s->rtxq_head = (s->rtxq_head + 1) & (s->rtxq_cap - 1);
        s->rtxq_len--;
        e->state = TXS_OUT;
        e->rtx++;
        e->miss = 0;
        e->sent_us = now;
        s->flight += e->len;
        s->st.rtx_chunks++;
        sctp_write_data(s, tsn, e);
        if (!s->t3_us)
            s->t3_us = now + s->rto_us;
    }
    while (s->om_chunk != s->om_tail) {
        rtc_sctp_omsg* m = sctp_om_at(s, s->om_chunk);
        rtc_sctp_tx* e;
        uint32_t len, room, tsn;
        if (m->off == 0 && m->expire_us && now >= m->expire_us && s->peer_prsctp) {
            sctp_abandon(s, s->om_chunk);
            continue;
        }
        if (s->flight > 0 && s->flight >= s->cwnd)
            break;
        if (s->flight > 0 && s->peer_rwnd == 0)
            break;
        if (s->next_tsn - s->ack_base >= RTC_SCTP_RX_WIN - 1)
            break;
        room = sctp_room(s);
        len = m->len - m->off;
        if (len > maxp)
            len = maxp;
        if (room < 16u + len) {
            if (s->pkt_len > 12 && (room < 16u + 64u || m->len - m->off <= maxp)) {
                sctp_pkt_emit(s);
                sctp_pkt_begin(s, s->peer_tag);
                room = sctp_room(s);
            }
            if (room < 16u + len)
                len = room - 16u;
        }
        if (s->next_tsn - s->ack_base >= s->tx_cap) {
            sctp_tx_grow(s);
            if (s->next_tsn - s->ack_base >= s->tx_cap)
                break;
        }
        tsn = s->next_tsn++;
        e = sctp_tx_at(s, tsn);
        memset(e, 0, sizeof(*e));
        e->msg = s->om_chunk;
        e->off = m->off;
        e->len = (uint16_t)len;
        e->state = TXS_OUT;
        e->sent_us = now;
        e->flags = (uint8_t)((m->off == 0 ? F_B : 0) | (m->off + len == m->len ? F_E : 0) | (m->unordered ? F_U : 0));
        if (m->off == 0) {
            m->first_tsn = tsn;
            if (!m->unordered) {
                rtc_sctp_stream* st = sctp_stream(s, m->stream);
                m->ssn = st ? st->out_ssn++ : 0;
            }
        }
        m->off += len;
        m->unacked++;
        s->flight += len;
        s->peer_rwnd = s->peer_rwnd > len ? s->peer_rwnd - len : 0;
        sctp_write_data(s, tsn, e);
        s->st.bytes_tx += len;
        if (m->off == m->len) {
            s->st.msgs_tx++;
            s->om_chunk++;
            sctp_msg_sent_done(s, m);
        }
        if (!s->t3_us)
            s->t3_us = now + s->rto_us;
    }
}

static void sctp_send_simple(rtc_sctp* s, uint8_t type, uint8_t flags, uint32_t vtag);

static void sctp_shutdown_progress(rtc_sctp* s, uint64_t now)
{
    if (s->state != RTC_SCTP_SHUTDOWN_RECEIVED || s->om_chunk != s->om_tail || s->ack_base != s->next_tsn)
        return;
    sctp_send_simple(s, RTC_SCTP_SHUTDOWN_ACK, 0, s->peer_tag);
    s->t3_us = 0;
    s->t2_tries = 0;
    s->t2_us = now + s->rto_us;
    sctp_set_state(s, RTC_SCTP_SHUTDOWN_ACK_SENT);
}

void rtc_sctp_flush(rtc_sctp* s, uint64_t now)
{
    if (!SCTP_LIVE(s))
        return;
    sctp_pkt_begin(s, s->peer_tag);
    if (s->cookie_ack_pending) {
        sctp_chunk(s, RTC_SCTP_COOKIE_ACK, 0, 0);
        s->cookie_ack_pending = 0;
    }
    if (s->need_sack && (s->sack_now || s->pkts_since_sack >= 2 || (s->sack_due_us && now >= s->sack_due_us)))
        sctp_write_sack(s);
    if (s->bad_sid_pending) {
        uint8_t* e = sctp_chunk(s, RTC_SCTP_ERROR, 0, 8);
        s->bad_sid_pending = 0;
        if (e) {
            rtc_wr16(e, 1);
            rtc_wr16(e + 2, 8);
            rtc_wr16(e + 4, s->bad_sid);
            rtc_wr16(e + 6, 0);
        }
    }
    if (s->fwd_pending == 1 && s->peer_prsctp && sctp_write_fwd(s)) {
        s->fwd_pending = 2;
        if (!s->t3_us)
            s->t3_us = now + s->rto_us;
    }
    if (s->reconf_resp_due) {
        s->reconf_resp_due = 0;
        sctp_write_reconf_resp(s, s->reconf_resp_seq, s->reconf_resp_result);
    }
    sctp_flush_data(s, now);
    sctp_prepare_reconf(s, now);
    if (s->reconf_inflight && s->reconf_us && now >= s->reconf_us) {
        if (sctp_room(s) < 20u + 2u * s->reconf_n) {
            sctp_pkt_emit(s);
            sctp_pkt_begin(s, s->peer_tag);
        }
        sctp_write_reconf_req(s);
        s->reconf_tries++;
        s->reconf_us = now + s->rto_us;
    }
    if (s->need_sack && !s->sack_due_us)
        s->sack_due_us = now + RTC_SCTP_SACK_DELAY_US;
    sctp_pkt_emit(s);
}

static void sctp_send_simple(rtc_sctp* s, uint8_t type, uint8_t flags, uint32_t vtag)
{
    sctp_pkt_begin(s, vtag);
    sctp_chunk(s, type, flags, 0);
    sctp_pkt_emit(s);
}

void rtc_sctp_input(rtc_sctp* s, const uint8_t* p, size_t n, uint64_t now)
{
    uint32_t vtag, off = 12;
    int had_data = 0;
    if (n < 16 || n > 65535)
        return;
    if (rtc_rd16(p) != s->rport || rtc_rd16(p + 2) != s->lport)
        return;
    if (!sctp_crc_ok(s, p, n))
        return;
    s->pkt_len = 0;
    s->now_us = now;
    s->st.pkts_rx++;
    vtag = rtc_rd32(p + 4);
    if (p[12] == RTC_SCTP_INIT) {
        if (vtag != 0)
            return;
    } else if (p[12] == RTC_SCTP_ABORT && (p[13] & 1)) {
        if (vtag != s->peer_tag)
            return;
    } else if (p[12] != RTC_SCTP_COOKIE_ECHO && vtag != s->my_tag)
        return;
    while (off + 4 <= n) {
        const uint8_t* c = p + off;
        uint8_t type = c[0], flags = c[1];
        uint32_t len = rtc_rd16(c + 2);
        if (len < 4 || off + len > n)
            break;
        switch (type) {
        case RTC_SCTP_DATA:
            had_data = 1;
            sctp_on_data(s, flags, c, len);
            break;
        case RTC_SCTP_INIT:
            sctp_on_init(s, c, len, now);
            return;
        case RTC_SCTP_INIT_ACK:
            sctp_on_init_ack(s, c, len, now);
            return;
        case RTC_SCTP_COOKIE_ECHO:
            if (!sctp_on_cookie_echo(s, vtag, c, len, now))
                return;
            break;
        case RTC_SCTP_COOKIE_ACK:
            if (s->state == RTC_SCTP_COOKIE_ECHOED)
                sctp_establish(s, s->peer_rwnd);
            break;
        case RTC_SCTP_SACK:
            sctp_on_sack(s, c, len, now);
            break;
        case RTC_SCTP_FORWARD_TSN:
            sctp_on_fwd_tsn(s, c, len);
            break;
        case RTC_SCTP_RECONFIG:
            sctp_on_reconf(s, c, len, now);
            break;
        case RTC_SCTP_HEARTBEAT:
            if (SCTP_LIVE(s) && len <= 1024) {
                uint8_t* b;
                sctp_pkt_begin(s, s->peer_tag);
                b = sctp_chunk(s, RTC_SCTP_HEARTBEAT_ACK, 0, len - 4);
                if (b) {
                    memcpy(b, c + 4, len - 4);
                    sctp_pkt_emit(s);
                } else {
                    s->pkt_len = 0;
                }
            }
            break;
        case RTC_SCTP_ABORT:
            if (s->state != RTC_SCTP_CLOSED) {
                s->t3_us = 0;
                s->t1_us = 0;
                sctp_set_state(s, RTC_SCTP_ABORTED);
            }
            return;
        case RTC_SCTP_SHUTDOWN:
            if (len >= 8 && SCTP_LIVE(s)) {
                uint8_t sk[16];
                memset(sk, 0, sizeof(sk));
                sk[0] = RTC_SCTP_SACK;
                rtc_wr16(sk + 2, 16);
                memcpy(sk + 4, c + 4, 4);
                rtc_wr32(sk + 8, s->peer_rwnd);
                sctp_on_sack(s, sk, 16, now);
                if (s->state == RTC_SCTP_ESTABLISHED)
                    sctp_set_state(s, RTC_SCTP_SHUTDOWN_RECEIVED);
                sctp_shutdown_progress(s, now);
            } else if (s->state == RTC_SCTP_SHUTDOWN_ACK_SENT)
                sctp_send_simple(s, RTC_SCTP_SHUTDOWN_ACK, 0, s->peer_tag);
            break;
        case RTC_SCTP_SHUTDOWN_ACK:
            if (s->state == RTC_SCTP_SHUTDOWN_SENT || s->state == RTC_SCTP_SHUTDOWN_ACK_SENT) {
                sctp_send_simple(s, RTC_SCTP_SHUTDOWN_COMPLETE, 0, s->peer_tag);
                s->t2_us = 0;
                sctp_set_state(s, RTC_SCTP_CLOSED);
            }
            return;
        case RTC_SCTP_SHUTDOWN_COMPLETE:
            if (s->state == RTC_SCTP_SHUTDOWN_ACK_SENT) {
                s->t2_us = 0;
                sctp_set_state(s, RTC_SCTP_CLOSED);
                return;
            }
            break;
        case RTC_SCTP_HEARTBEAT_ACK:
            sctp_on_heartbeat_ack(s, c, len);
            break;
        case RTC_SCTP_ERROR:
            break;
        default:
            if ((type & 0xC0) == 0x00)
                goto done;
            if (type & 0x40) {
                if (SCTP_LIVE(s)) {
                    uint8_t* b;
                    uint32_t cl = len;
                    sctp_pkt_emit(s);
                    sctp_pkt_begin(s, s->peer_tag);
                    b = sctp_chunk(s, RTC_SCTP_ERROR, 0, 4 + cl);
                    if (!b) {
                        cl = 64;
                        b = sctp_chunk(s, RTC_SCTP_ERROR, 0, 4 + cl);
                    }
                    if (b) {
                        rtc_wr16(b, 6);
                        rtc_wr16(b + 2, 4 + cl);
                        memcpy(b + 4, c, cl);
                        sctp_pkt_emit(s);
                    } else {
                        s->pkt_len = 0;
                    }
                }
                if (!(type & 0x80))
                    goto done;
            }
            break;
        }
        off += (len + 3) & ~3u;
    }
done:
    if (had_data)
        s->pkts_since_sack++;
    if (SCTP_LIVE(s))
        rtc_sctp_flush(s, now);
    if (s->state == RTC_SCTP_SHUTDOWN_RECEIVED)
        sctp_shutdown_progress(s, now);
}

int rtc_sctp_send(rtc_sctp* s, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n, int unordered, int32_t max_rtx,
    int64_t lifetime_ms, uint64_t now)
{
    rtc_sctp_omsg* m;
    rtc_sctp_stream* st;
    if (s->state != RTC_SCTP_ESTABLISHED)
        return -1;
    if (n == 0 || n > s->max_msg)
        return -2;
    if (stream >= s->n_out)
        return -4;
    st = sctp_stream(s, stream);
    if (!st)
        return -3;
    if (s->om_tail - s->om_head == s->om_cap) {
        uint32_t nc = s->om_cap ? s->om_cap * 2 : 64, q;
        rtc_sctp_omsg* nm = (rtc_sctp_omsg*)malloc(nc * sizeof(*nm));
        if (!nm)
            return -3;
        for (q = s->om_head; q != s->om_tail; q++)
            nm[q & (nc - 1)] = s->om[q & (s->om_cap - 1)];
        free(s->om);
        s->om = nm;
        s->om_cap = nc;
    }
    m = sctp_om_at(s, s->om_tail);
    memset(m, 0, sizeof(*m));
    m->data = (uint8_t*)malloc(n);
    if (!m->data)
        return -3;
    memcpy(m->data, p, n);
    m->len = (uint32_t)n;
    m->ppid = ppid;
    m->stream = stream;
    m->unordered = (uint8_t)(unordered != 0);
    m->max_rtx = max_rtx;
    m->expire_us = lifetime_ms >= 0 ? now + (uint64_t)lifetime_ms * 1000u : 0;
    if (lifetime_ms == 0)
        m->expire_us = now + 1;
    s->om_tail++;
    st->queued += (uint32_t)n;
    return 0;
}

int rtc_sctp_reset_stream(rtc_sctp* s, uint16_t stream)
{
    rtc_sctp_stream* st = sctp_stream(s, stream);
    if (!st)
        return -1;
    if (st->reset_want)
        return 0;
    if (s->rw_n == s->rw_cap) {
        uint32_t nc = s->rw_cap ? s->rw_cap * 2 : 16;
        uint16_t* nl = (uint16_t*)realloc(s->rw_list, nc * sizeof(uint16_t));
        if (!nl)
            return -1;
        s->rw_list = nl;
        s->rw_cap = nc;
    }
    st->reset_want = 1;
    s->rw_list[s->rw_n++] = stream;
    return 0;
}

uint32_t rtc_sctp_max_channels(const rtc_sctp* s)
{
    return s->n_in < s->n_out ? s->n_in : s->n_out;
}

uint32_t rtc_sctp_buffered(const rtc_sctp* s, uint16_t stream)
{
    const rtc_sctp_stream* st = sctp_stream_at(s, stream);
    return st ? st->queued : 0;
}

static void sctp_heartbeat(rtc_sctp* s, uint64_t now)
{
    uint8_t* b;
    if (s->t3_us || !s->hb_us) {
        s->hb_outstanding = 0;
        s->hb_us = now + s->rto_us + RTC_SCTP_HB_INTERVAL_US;
        return;
    }
    if (now < s->hb_us)
        return;
    if (s->hb_outstanding) {
        s->rto_us = s->rto_us * 2 > RTC_SCTP_RTO_MAX_US ? RTC_SCTP_RTO_MAX_US : s->rto_us * 2;
        if (++s->assoc_errors > RTC_SCTP_ASSOC_MAX_RTX) {
            s->hb_us = 0;
            sctp_set_state(s, RTC_SCTP_ABORTED);
            return;
        }
    }
    s->hb_nonce = ((uint64_t)rtc_rng_u32(s->rng) << 32) | rtc_rng_u32(s->rng);
    sctp_pkt_begin(s, s->peer_tag);
    b = sctp_chunk(s, RTC_SCTP_HEARTBEAT, 0, 20);
    if (b) {
        rtc_wr16(b, 1);
        rtc_wr16(b + 2, 20);
        rtc_wr64(b + 4, s->hb_nonce);
        rtc_wr64(b + 12, now);
        sctp_pkt_emit(s);
        s->st.hb_tx++;
    } else
        s->pkt_len = 0;
    s->hb_outstanding = 1;
    s->hb_us = now + s->rto_us + RTC_SCTP_HB_INTERVAL_US;
}

static void sctp_on_heartbeat_ack(rtc_sctp* s, const uint8_t* c, uint32_t len)
{
    if (!s->hb_outstanding || len != 24 || rtc_rd16(c + 4) != 1 || rtc_rd16(c + 6) != 20 || rtc_rd64(c + 8) != s->hb_nonce)
        return;
    s->hb_outstanding = 0;
    s->assoc_errors = 0;
    s->st.hb_ack_rx++;
    if (s->now_us >= rtc_rd64(c + 16) && s->now_us - rtc_rd64(c + 16) < RTC_SCTP_RTO_MAX_US)
        sctp_rto_sample(s, (uint32_t)(s->now_us - rtc_rd64(c + 16)));
}

uint64_t rtc_sctp_tick(rtc_sctp* s, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    if (s->state == RTC_SCTP_COOKIE_WAIT || s->state == RTC_SCTP_COOKIE_ECHOED) {
        if (s->t1_us && now >= s->t1_us) {
            if (++s->init_tries > RTC_SCTP_MAX_INIT_RTX) {
                s->t1_us = 0;
                sctp_set_state(s, RTC_SCTP_ABORTED);
                return UINT64_MAX;
            }
            s->t1_rto = s->t1_rto * 2 > RTC_SCTP_RTO_MAX_US ? RTC_SCTP_RTO_MAX_US : s->t1_rto * 2;
            s->t1_us = now + s->t1_rto;
            if (s->state == RTC_SCTP_COOKIE_WAIT)
                sctp_send_init(s);
            else
                sctp_send_cookie_echo(s);
        }
        return s->t1_us ? s->t1_us : UINT64_MAX;
    }
    if (s->state == RTC_SCTP_SHUTDOWN_ACK_SENT) {
        if (s->t2_us && now >= s->t2_us) {
            if (++s->t2_tries > RTC_SCTP_ASSOC_MAX_RTX) {
                s->t2_us = 0;
                sctp_set_state(s, RTC_SCTP_ABORTED);
                return UINT64_MAX;
            }
            sctp_send_simple(s, RTC_SCTP_SHUTDOWN_ACK, 0, s->peer_tag);
            s->t2_us = now + (s->rto_us << (s->t2_tries < 6 ? s->t2_tries : 6));
            if (s->t2_us - now > RTC_SCTP_RTO_MAX_US)
                s->t2_us = now + RTC_SCTP_RTO_MAX_US;
        }
        return s->t2_us ? s->t2_us : UINT64_MAX;
    }
    if (!SCTP_LIVE(s))
        return UINT64_MAX;
    if (s->t3_us && now >= s->t3_us)
        sctp_t3_expire(s, now);
    if (!SCTP_LIVE(s))
        return UINT64_MAX;
    rtc_sctp_flush(s, now);
    if (s->t3_us)
        next = s->t3_us;
    sctp_heartbeat(s, now);
    if (!SCTP_LIVE(s))
        return UINT64_MAX;
    if (s->hb_us && s->hb_us < next)
        next = s->hb_us;
    if (s->sack_due_us && s->sack_due_us < next)
        next = s->sack_due_us;
    if (s->reconf_inflight && s->reconf_us && s->reconf_us < next)
        next = s->reconf_us;
    if (s->om_chunk != s->om_tail && s->peer_prsctp) {
        rtc_sctp_omsg* m = sctp_om_at(s, s->om_chunk);
        if (m->off == 0 && m->expire_us && m->expire_us < next)
            next = m->expire_us;
    }
    return next;
}

void rtc_sctp_abort(rtc_sctp* s)
{
    if (SCTP_LIVE(s) || s->state == RTC_SCTP_SHUTDOWN_ACK_SENT || s->state == RTC_SCTP_COOKIE_ECHOED || s->state == RTC_SCTP_COOKIE_WAIT) {
        if (s->peer_tag)
            sctp_send_simple(s, RTC_SCTP_ABORT, 0, s->peer_tag);
    }
    s->state = RTC_SCTP_ABORTED;
    s->t3_us = 0;
    s->t1_us = 0;
}
