#include "webrtc-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>

#include "webrtc-srtp.h"
#include "../../core/rtc/rtc-rtp.h"
#include "../../core/rtc/rtc-rtcp.h"
#include "../../core/rtc/rtc-rtpstat.h"

#define WRT_MEDIA_BUF 2048u
#define WRT_RTP_MAX 1200u

typedef struct {
    uint32_t pkts, ends, markers, segs, last_dur, first_dur, last_ts;
    uint8_t last_event, last_vol, have_ts;
} wrt_te;

typedef struct {
    uint32_t id;
    uint32_t clock;
    uint64_t pts[2];
    uint64_t rtx_pts[2];
    uint32_t sig[WRT_RX_SSRC_MAX];
    uint8_t nsig;
    uint8_t receptive;
    uint8_t muted;
    uint8_t alevel_ext;
    uint8_t clevel_ext;
    uint8_t has_st;
    uint8_t seen;
    char mid[RTC_SDP_MID_MAX];
    uint8_t midlen;
    rtc_rtp_rx st;
    uint64_t pkts, bytes;
    uint64_t last_us;
    struct wrt_relay* relay;
    wrt_te* te;
    uint8_t te_pt[2];
    wrt_src ss[WRT_RX_SSRC_MAX];
    wrt_src cs[WRT_RX_CSRC_MAX];
} wrt_rx;

typedef struct {
    uint8_t active;
    uint8_t event;
    uint8_t marker;
    uint8_t te_pt;
    uint32_t te_clock;
    uint32_t ts;
    uint64_t seg_us;
    uint64_t end_us;
    uint64_t next_us;
} wrt_dtmf;

typedef struct {
    uint32_t id;
    uint32_t ssrc;
    uint32_t ts_off;
    uint16_t seq;
    uint32_t last_ts;
    uint32_t clock;
    wrt_dtmf* dtmf;
    uint8_t live;
    uint8_t pt;
    uint8_t mid_ext;
    uint8_t alevel_ext;
    uint8_t clevel_ext;
    uint8_t seen;
    char mid[RTC_SDP_MID_MAX];
    uint8_t midlen;
    uint64_t pkts, bytes;
} wrt_tx;

typedef struct wrt_media {
    wrt_srtp stx, srx;
    uint8_t keyed;
    uint8_t mid_ext;
    wrt_rx* rx;
    uint32_t nrx, cap_rx;
    wrt_tx* tx;
    uint32_t ntx, cap_tx;
    uint32_t* map_key;
    uint32_t* map_val;
    uint32_t map_cap, map_n;
    struct wrt_relay** hubs;
    uint32_t nhub, cap_hub;
    uint32_t imp_loss, imp_dup, imp_reorder;
    uint64_t imp_state;
    size_t held_n;
    uint8_t* held;
    wrt_media_counts c;
} wrt_media;

static _Thread_local uint8_t wrt_ibuf[WRT_MEDIA_BUF];
static _Thread_local uint8_t wrt_obuf[WRT_MEDIA_BUF];

static wrt_media* wrt_m(wrt_pc* pc)
{
    if (!pc->media)
        pc->media = (wrt_media*)calloc(1, sizeof(wrt_media));
    return pc->media;
}

static int64_t wrt_wall_off;
static pthread_once_t wrt_wall_once = PTHREAD_ONCE_INIT;

static void wrt_wall_init(void)
{
    struct timespec r;
    clock_gettime(CLOCK_REALTIME, &r);
    wrt_wall_off = ((int64_t)r.tv_sec * 1000000 + r.tv_nsec / 1000) - (int64_t)wrt_now_us();
}

double wrt_wall_ms(uint64_t mono_us)
{
    pthread_once(&wrt_wall_once, wrt_wall_init);
    return (double)((int64_t)mono_us + wrt_wall_off) / 1000.0;
}

typedef struct {
    wrt_pc* pc;
    uint32_t id;
    uint32_t src_ssrc;
    uint32_t ts_delta;
    uint16_t seq_off;
    uint16_t next;
    uint8_t have;
    uint8_t ndst;
    uint8_t map[128];
    wrt_codec_desc dst[WRT_RELAY_CODECS];
} wrt_sink;

typedef struct wrt_relay {
    int refs;
    int ncodec;
    uint8_t ended;
    wrt_sink* s;
    uint32_t n, cap;
    wrt_codec_desc codecs[WRT_RELAY_CODECS];
} wrt_relay;

wrt_relay* wrt_relay_new(void)
{
    wrt_relay* h = (wrt_relay*)calloc(1, sizeof(wrt_relay));
    if (h)
        h->refs = 1;
    return h;
}

void wrt_relay_ref(wrt_relay* h)
{
    if (h)
        h->refs++;
}

void wrt_relay_unref(wrt_relay* h)
{
    if (!h || --h->refs > 0)
        return;
    free(h->s);
    free(h);
}

void wrt_relay_end(wrt_relay* h)
{
    if (h)
        h->ended = 1;
}

size_t wrt_relay_bytes(const wrt_relay* h)
{
    return h ? sizeof(*h) + (size_t)h->cap * sizeof(wrt_sink) : 0;
}

static int wrt_codec_same(const wrt_codec_desc* a, const wrt_codec_desc* b)
{
    return a->clock == b->clock && (a->channels ? a->channels : 1) == (b->channels ? b->channels : 1)
        && !strcasecmp(a->name, b->name);
}

static void wrt_sink_remap(const wrt_relay* h, wrt_sink* s)
{
    int q, z;
    memset(s->map, WRT_PT_NONE, sizeof(s->map));
    for (q = 0; q < h->ncodec; q++)
        for (z = 0; z < s->ndst; z++)
            if (wrt_codec_same(&h->codecs[q], &s->dst[z])) {
                s->map[h->codecs[q].pt & 127] = s->dst[z].pt;
                break;
            }
}

static int wrt_relay_add(wrt_relay* h, wrt_pc* pc, uint32_t id, const wrt_codec_desc* dst, int ndst)
{
    uint32_t i;
    wrt_sink* s = NULL;
    for (i = 0; i < h->n; i++)
        if (h->s[i].pc == pc && h->s[i].id == id) {
            s = &h->s[i];
            break;
        }
    if (!s) {
        if (h->n == h->cap) {
            uint32_t nc = h->cap ? h->cap * 2 : 2;
            wrt_sink* ns = (wrt_sink*)realloc(h->s, (size_t)nc * sizeof(*ns));
            if (!ns)
                return -1;
            h->s = ns;
            h->cap = nc;
        }
        s = &h->s[h->n++];
        memset(s, 0, sizeof(*s));
        s->pc = pc;
        s->id = id;
    }
    s->ndst = (uint8_t)(ndst < 0 ? 0 : ndst > WRT_RELAY_CODECS ? WRT_RELAY_CODECS : ndst);
    if (s->ndst)
        memcpy(s->dst, dst, (size_t)s->ndst * sizeof(*dst));
    wrt_sink_remap(h, s);
    return 0;
}

static void wrt_relay_drop_pc(wrt_relay* h, const wrt_pc* pc)
{
    uint32_t i, k = 0;
    for (i = 0; i < h->n; i++)
        if (h->s[i].pc != pc)
            h->s[k++] = h->s[i];
    h->n = k;
}

static void wrt_hubs_release(wrt_pc* pc, wrt_media* m)
{
    uint32_t i;
    for (i = 0; i < m->nhub; i++) {
        wrt_relay_drop_pc(m->hubs[i], pc);
        wrt_relay_unref(m->hubs[i]);
    }
    m->nhub = 0;
}

static void wrt_rx_drop(wrt_rx* r)
{
    wrt_relay_unref(r->relay);
    free(r->te);
}

int wrt_media_key(wrt_pc* pc)
{
    wrt_media* m;
    if (pc->dtls.state != WRT_DTLS_CONNECTED || !pc->dtls.keys_len || !pc->dtls.srtp_profile)
        return -1;
    m = wrt_m(pc);
    if (!m)
        return -1;
    if (m->keyed)
        return 0;
    if (wrt_srtp_init_dtls(&m->stx, &m->srx, pc->dtls.srtp_profile, pc->dtls.keys, pc->dtls.keys_len, !pc->dtls.is_server) < 0)
        return -1;
    m->keyed = 1;
    return 0;
}

void wrt_media_free(wrt_pc* pc)
{
    wrt_media* m = pc->media;
    uint32_t i;
    if (!m)
        return;
    wrt_hubs_release(pc, m);
    free(m->hubs);
    for (i = 0; i < m->nrx; i++)
        wrt_rx_drop(&m->rx[i]);
    for (i = 0; i < m->ntx; i++)
        free(m->tx[i].dtmf);
    free(m->held);
    if (m->keyed) {
        wrt_srtp_free(&m->stx);
        wrt_srtp_free(&m->srx);
    }
    free(m->rx);
    free(m->tx);
    free(m->map_key);
    free(m->map_val);
    free(m);
    pc->media = NULL;
}

static uint32_t wrt_hash(uint32_t k)
{
    k ^= k >> 16;
    k *= 0x7feb352du;
    k ^= k >> 15;
    k *= 0x846ca68bu;
    k ^= k >> 16;
    return k;
}

static int wrt_map_find(const wrt_media* m, uint32_t ssrc)
{
    uint32_t mask, i;
    if (!m->map_cap)
        return -1;
    mask = m->map_cap - 1;
    for (i = wrt_hash(ssrc) & mask;; i = (i + 1) & mask) {
        if (!m->map_val[i])
            return -1;
        if (m->map_key[i] == ssrc)
            return (int)m->map_val[i] - 1;
    }
}

static int wrt_map_put(wrt_media* m, uint32_t ssrc, uint32_t idx)
{
    uint32_t mask, i;
    if ((m->map_n + 1) * 2 > m->map_cap) {
        uint32_t nc = m->map_cap ? m->map_cap * 2 : 16, oc = m->map_cap, j;
        uint32_t *ok = m->map_key, *ov = m->map_val;
        uint32_t* nk = (uint32_t*)calloc(nc, sizeof(uint32_t));
        uint32_t* nv = (uint32_t*)calloc(nc, sizeof(uint32_t));
        if (!nk || !nv) {
            free(nk);
            free(nv);
            return -1;
        }
        m->map_key = nk;
        m->map_val = nv;
        m->map_cap = nc;
        m->map_n = 0;
        for (j = 0; j < oc; j++)
            if (ov[j])
                wrt_map_put(m, ok[j], ov[j] - 1);
        free(ok);
        free(ov);
    }
    mask = m->map_cap - 1;
    for (i = wrt_hash(ssrc) & mask;; i = (i + 1) & mask) {
        if (!m->map_val[i]) {
            m->map_key[i] = ssrc;
            m->map_val[i] = idx + 1;
            m->map_n++;
            return 0;
        }
        if (m->map_key[i] == ssrc) {
            m->map_val[i] = idx + 1;
            return 0;
        }
    }
}

static void wrt_map_clear(wrt_media* m)
{
    if (m->map_cap) {
        memset(m->map_val, 0, m->map_cap * sizeof(uint32_t));
        m->map_n = 0;
    }
}

int wrt_media_sync_begin(wrt_pc* pc)
{
    wrt_media* m = wrt_m(pc);
    uint32_t i;
    if (!m)
        return -1;
    for (i = 0; i < m->nrx; i++)
        m->rx[i].seen = 0;
    for (i = 0; i < m->ntx; i++)
        m->tx[i].seen = 0;
    m->mid_ext = 0;
    wrt_hubs_release(pc, m);
    return 0;
}

static void wrt_mid_copy(char* dst, uint8_t* len, const char* src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= RTC_SDP_MID_MAX)
        n = RTC_SDP_MID_MAX - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
    *len = (uint8_t)n;
}

int wrt_media_rx_set(wrt_pc* pc, const wrt_rx_cfg* c)
{
    wrt_media* m = pc->media;
    wrt_rx* r = NULL;
    uint32_t i;
    int k;
    if (!m)
        return -1;
    for (i = 0; i < m->nrx; i++)
        if (m->rx[i].id == c->id) {
            r = &m->rx[i];
            break;
        }
    if (!r) {
        if (m->nrx == m->cap_rx) {
            uint32_t nc = m->cap_rx ? m->cap_rx * 2 : 4;
            wrt_rx* nr = (wrt_rx*)realloc(m->rx, (size_t)nc * sizeof(*nr));
            if (!nr)
                return -1;
            m->rx = nr;
            m->cap_rx = nc;
        }
        r = &m->rx[m->nrx++];
        memset(r, 0, sizeof(*r));
        r->id = c->id;
    }
    r->seen = 1;
    r->clock = c->clock ? c->clock : 90000;
    r->pts[0] = c->pts[0];
    r->pts[1] = c->pts[1];
    r->rtx_pts[0] = c->rtx_pts[0];
    r->rtx_pts[1] = c->rtx_pts[1];
    r->receptive = c->receptive;
    r->muted = c->muted;
    r->alevel_ext = c->alevel_ext;
    r->clevel_ext = c->clevel_ext;
    wrt_mid_copy(r->mid, &r->midlen, c->mid);
    r->te_pt[0] = c->te_pt[0];
    r->te_pt[1] = c->te_pt[1];
    if (r->relay != c->relay) {
        wrt_relay_unref(r->relay);
        r->relay = c->relay;
        wrt_relay_ref(r->relay);
    }
    if (c->relay) {
        int q, nc = c->ncodec < WRT_RELAY_CODECS ? c->ncodec : WRT_RELAY_CODECS;
        for (q = 0; q < nc; q++)
            c->relay->codecs[q] = c->codecs[q];
        c->relay->ncodec = nc;
        for (q = 0; q < (int)c->relay->n; q++)
            wrt_sink_remap(c->relay, &c->relay->s[q]);
    }
    r->nsig = 0;
    for (k = 0; k < c->nssrc && r->nsig < WRT_RX_SSRC_MAX; k++)
        r->sig[r->nsig++] = c->ssrcs[k];
    if (c->mid_ext && !m->mid_ext)
        m->mid_ext = c->mid_ext;
    return 0;
}

int wrt_media_tx_set(wrt_pc* pc, const wrt_tx_cfg* c)
{
    wrt_media* m = pc->media;
    wrt_tx* t = NULL;
    uint32_t i;
    if (!m)
        return -1;
    for (i = 0; i < m->ntx; i++)
        if (m->tx[i].id == c->id) {
            t = &m->tx[i];
            break;
        }
    if (!t) {
        if (m->ntx == m->cap_tx) {
            uint32_t nc = m->cap_tx ? m->cap_tx * 2 : 4;
            wrt_tx* nt = (wrt_tx*)realloc(m->tx, (size_t)nc * sizeof(*nt));
            if (!nt)
                return -1;
            m->tx = nt;
            m->cap_tx = nc;
        }
        t = &m->tx[m->ntx++];
        memset(t, 0, sizeof(*t));
        t->id = c->id;
        t->seq = (uint16_t)rtc_rng_u32(&pc->rng);
        t->ts_off = rtc_rng_u32(&pc->rng);
    }
    t->seen = 1;
    t->ssrc = c->ssrc;
    t->live = c->live;
    t->pt = c->pt;
    t->mid_ext = c->mid_ext;
    t->alevel_ext = c->alevel_ext;
    t->clevel_ext = c->clevel_ext;
    wrt_mid_copy(t->mid, &t->midlen, c->mid);
    t->clock = c->clock ? c->clock : 90000;
    if (!t->live && t->dtmf)
        t->dtmf->active = 0;
    if (c->relay && t->live) {
        if (m->nhub == m->cap_hub) {
            uint32_t nc = m->cap_hub ? m->cap_hub * 2 : 2;
            struct wrt_relay** nh = (struct wrt_relay**)realloc(m->hubs, (size_t)nc * sizeof(*nh));
            if (!nh)
                return -1;
            m->hubs = nh;
            m->cap_hub = nc;
        }
        if (wrt_relay_add(c->relay, pc, t->id, c->codecs, c->ncodec) < 0)
            return -1;
        wrt_relay_ref(c->relay);
        m->hubs[m->nhub++] = c->relay;
    }
    return 0;
}

static void wrt_sig_map(wrt_media* m)
{
    uint32_t i, k;
    for (i = 0; i < m->nrx; i++)
        for (k = 0; k < m->rx[i].nsig; k++)
            wrt_map_put(m, m->rx[i].sig[k], i);
}

void wrt_media_sync_end(wrt_pc* pc)
{
    wrt_media* m = pc->media;
    uint32_t i, k = 0, j;
    uint32_t* keep_k = NULL;
    uint32_t* keep_v = NULL;
    uint32_t nkeep = 0;
    if (!m)
        return;
    if (m->map_n) {
        keep_k = (uint32_t*)malloc(m->map_n * sizeof(uint32_t));
        keep_v = (uint32_t*)malloc(m->map_n * sizeof(uint32_t));
        if (keep_k && keep_v)
            for (j = 0; j < m->map_cap; j++)
                if (m->map_val[j]) {
                    keep_k[nkeep] = m->map_key[j];
                    keep_v[nkeep++] = m->rx[m->map_val[j] - 1].id;
                }
    }
    for (i = 0; i < m->nrx; i++)
        if (m->rx[i].seen)
            m->rx[k++] = m->rx[i];
        else
            wrt_rx_drop(&m->rx[i]);
    m->nrx = k;
    for (i = 0, k = 0; i < m->ntx; i++)
        if (m->tx[i].seen)
            m->tx[k++] = m->tx[i];
        else
            free(m->tx[i].dtmf);
    m->ntx = k;
    wrt_map_clear(m);
    for (j = 0; j < nkeep; j++)
        for (i = 0; i < m->nrx; i++)
            if (m->rx[i].id == keep_v[j]) {
                wrt_map_put(m, keep_k[j], i);
                break;
            }
    free(keep_k);
    free(keep_v);
    wrt_sig_map(m);
}

static wrt_rx* wrt_rx_by_id(wrt_media* m, uint32_t id)
{
    uint32_t i;
    for (i = 0; m && i < m->nrx; i++)
        if (m->rx[i].id == id)
            return &m->rx[i];
    return NULL;
}

static wrt_tx* wrt_tx_by_id(wrt_media* m, uint32_t id)
{
    uint32_t i;
    for (i = 0; m && i < m->ntx; i++)
        if (m->tx[i].id == id)
            return &m->tx[i];
    return NULL;
}

static int wrt_pt_in(const uint64_t* set, uint8_t pt)
{
    return (int)((set[pt >> 6] >> (pt & 63)) & 1u);
}

static int wrt_route(wrt_media* m, const uint8_t* p, const rtc_rtp_hdr* h)
{
    int idx = wrt_map_find(m, h->ssrc);
    const uint8_t* mv = NULL;
    size_t ml = 0;
    uint32_t i;
    int hit = -1;
    if (m->mid_ext && rtc_rtp_ext_get(p, h, m->mid_ext, &mv, &ml) > 0 && ml) {
        if (idx >= 0 && m->rx[idx].midlen == ml && !memcmp(m->rx[idx].mid, mv, ml))
            return idx;
        for (i = 0; i < m->nrx; i++)
            if (m->rx[i].midlen == ml && !memcmp(m->rx[i].mid, mv, ml)) {
                wrt_map_put(m, h->ssrc, i);
                return (int)i;
            }
        return -1;
    }
    if (idx >= 0)
        return idx;
    for (i = 0; i < m->nrx; i++)
        if (wrt_pt_in(m->rx[i].pts, h->pt) || wrt_pt_in(m->rx[i].rtx_pts, h->pt)) {
            if (hit >= 0)
                return -1;
            hit = (int)i;
        }
    if (hit >= 0)
        wrt_map_put(m, h->ssrc, (uint32_t)hit);
    return hit;
}

static void wrt_src_note(wrt_src* tab, int cap, uint32_t src, uint32_t rtp_ts, uint8_t level, uint64_t now)
{
    int i, free_i = -1, old_i = 0;
    for (i = 0; i < cap; i++) {
        if (tab[i].used && tab[i].src == src) {
            if (now - tab[i].at_us < WRT_SRC_WINDOW_US && (int32_t)(rtp_ts - tab[i].rtp_ts) < 0)
                return;
            tab[i].at_us = now;
            tab[i].rtp_ts = rtp_ts;
            tab[i].level = level;
            return;
        }
        if (!tab[i].used && free_i < 0)
            free_i = i;
        if (tab[i].used && tab[i].at_us < tab[old_i].at_us)
            old_i = i;
    }
    i = free_i >= 0 ? free_i : old_i;
    tab[i].used = 1;
    tab[i].src = src;
    tab[i].at_us = now;
    tab[i].rtp_ts = rtp_ts;
    tab[i].level = level;
}

static void wrt_relay_forward(struct wrt_relay* hub, const uint8_t* p, const rtc_rtp_hdr* h, int level, const uint8_t* clevels, size_t ncl);

static void wrt_rx_packet(wrt_pc* pc, wrt_rx* r, const uint8_t* p, const rtc_rtp_hdr* h, size_t n, uint64_t now)
{
    const uint8_t* v = NULL;
    size_t vl = 0;
    uint8_t level = WRT_LEVEL_NONE;
    unsigned i;
    int raw = -1;
    r->last_us = now;
    if (!r->has_st || r->st.ssrc != h->ssrc) {
        rtc_rtp_rx_init(&r->st, h->ssrc, h->seq);
        r->has_st = 1;
    }
    if (!rtc_rtp_rx_update(&r->st, h->seq))
        return;
    rtc_rtp_rx_arrival(&r->st, h->ts, (uint32_t)(now * r->clock / 1000000u));
    r->pkts++;
    r->bytes += h->plen;
    r->st.bytes += n;
    if (r->alevel_ext && rtc_rtp_ext_get(p, h, r->alevel_ext, &v, &vl) > 0 && vl >= 1) {
        level = (uint8_t)(v[0] & 0x7f);
        raw = v[0];
    }
    wrt_src_note(r->ss, WRT_RX_SSRC_MAX, h->ssrc, h->ts, level, now);
    v = NULL;
    vl = 0;
    if (h->cc && r->clevel_ext && rtc_rtp_ext_get(p, h, r->clevel_ext, &v, &vl) <= 0) {
        v = NULL;
        vl = 0;
    }
    for (i = 0; i < h->cc; i++) {
        uint32_t cs = (uint32_t)p[12 + 4 * i] << 24 | (uint32_t)p[13 + 4 * i] << 16 | (uint32_t)p[14 + 4 * i] << 8 | p[15 + 4 * i];
        wrt_src_note(r->cs, WRT_RX_CSRC_MAX, cs, h->ts, v && i < vl ? (uint8_t)(v[i] & 0x7f) : WRT_LEVEL_NONE, now);
    }
    if ((h->pt == r->te_pt[0] || h->pt == r->te_pt[1]) && h->plen >= 4
        && (r->te || (r->te = (wrt_te*)calloc(1, sizeof(wrt_te))) != NULL)) {
        const uint8_t* q = p + h->hlen;
        wrt_te* e = r->te;
        e->pkts++;
        if (h->marker) {
            e->markers++;
            e->first_dur = (uint32_t)q[2] << 8 | q[3];
        }
        if (!e->have_ts || e->last_ts != h->ts)
            e->segs++;
        e->have_ts = 1;
        e->last_ts = h->ts;
        e->last_event = q[0];
        e->last_vol = (uint8_t)(q[1] & 0x3f);
        e->last_dur = (uint32_t)q[2] << 8 | q[3];
        if (q[1] & 0x80)
            e->ends++;
    }
    if (r->relay && r->relay->n)
        wrt_relay_forward(r->relay, p, h, raw, v, vl);
    if (r->muted && r->receptive) {
        r->muted = 0;
        wrt_ev_push(pc, WRT_EV_RTP_UNMUTE, 0, r->id, 0, NULL, 0);
    }
}

static int wrt_rtcp_input(wrt_pc* pc, wrt_media* m, uint8_t* b, size_t n, uint64_t now)
{
    size_t ol = 0;
    int r = wrt_srtcp_unprotect(&m->srx, b, n, &ol);
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    int ev = 0;
    if (r != WRT_SRTP_OK) {
        if (r == WRT_SRTP_ERR_AUTH)
            m->c.srtp_auth++;
        else if (r == WRT_SRTP_ERR_REPLAY)
            m->c.srtp_replay++;
        else
            m->c.srtp_other++;
        return 0;
    }
    m->c.rtcp_in++;
    if (rtc_rtcp_check(b, ol) < 0) {
        m->c.rtcp_bad++;
        return 0;
    }
    rtc_rtcp_iter_init(&it, b, ol);
    while (rtc_rtcp_next(&it, &k) > 0) {
        uint32_t ss[31];
        unsigned nb, i;
        if (k.pt == RTC_RTCP_SR) {
            rtc_rtcp_sr sr;
            int si;
            if (rtc_rtcp_sr_parse(&k, &sr) == 0 && (si = wrt_map_find(m, sr.ssrc)) >= 0)
                m->rx[si].last_us = now;
            continue;
        }
        if (k.pt != RTC_RTCP_BYE)
            continue;
        nb = rtc_rtcp_bye_ssrcs(&k, ss, 31);
        for (i = 0; i < nb; i++) {
            int idx = wrt_map_find(m, ss[i]);
            wrt_rx* rx;
            int j;
            if (idx < 0)
                continue;
            rx = &m->rx[idx];
            m->c.byes++;
            for (j = 0; j < WRT_RX_SSRC_MAX; j++)
                if (rx->ss[j].used && rx->ss[j].src == ss[i])
                    rx->ss[j].used = 0;
            if (!rx->muted) {
                rx->muted = 1;
                wrt_ev_push(pc, WRT_EV_RTP_MUTE, 0, rx->id, 0, NULL, 0);
                ev = 1;
            }
        }
    }
    return ev;
}

int wrt_media_input(wrt_pc* pc, const uint8_t* p, size_t n, uint64_t now)
{
    wrt_media* m = pc->media;
    rtc_rtp_hdr h;
    size_t ol = 0;
    uint32_t nev = pc->nev;
    int r, idx;
    if (!m || !m->keyed) {
        pc->rtp_dropped++;
        if (m)
            m->c.unkeyed++;
        return 0;
    }
    if (n > WRT_MEDIA_BUF) {
        m->c.oversize++;
        return 0;
    }
    memcpy(wrt_ibuf, p, n);
    if (rtc_rtp_is_rtcp(wrt_ibuf, n))
        return wrt_rtcp_input(pc, m, wrt_ibuf, n, now);
    r = wrt_srtp_unprotect(&m->srx, wrt_ibuf, n, &ol);
    if (r != WRT_SRTP_OK) {
        if (r == WRT_SRTP_ERR_AUTH)
            m->c.srtp_auth++;
        else if (r == WRT_SRTP_ERR_REPLAY)
            m->c.srtp_replay++;
        else
            m->c.srtp_other++;
        return 0;
    }
    m->c.rtp_in++;
    if (rtc_rtp_parse(wrt_ibuf, ol, &h) < 0) {
        m->c.unroutable++;
        return 0;
    }
    idx = wrt_route(m, wrt_ibuf, &h);
    if (idx < 0) {
        m->c.unroutable++;
        return 0;
    }
    if (wrt_pt_in(m->rx[idx].rtx_pts, h.pt) || !wrt_pt_in(m->rx[idx].pts, h.pt)) {
        m->c.pt_drop++;
        return 0;
    }
    wrt_rx_packet(pc, &m->rx[idx], wrt_ibuf, &h, ol, now);
    return pc->nev != nev;
}

static uint32_t wrt_imp_roll(wrt_media* m)
{
    uint64_t x = m->imp_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m->imp_state = x;
    return (uint32_t)((x >> 11) % 1000000u);
}

void wrt_media_impair(wrt_pc* pc, uint32_t loss_ppm, uint32_t dup_ppm, uint32_t reorder_ppm, uint64_t seed)
{
    wrt_media* m = wrt_m(pc);
    if (!m)
        return;
    m->imp_loss = loss_ppm;
    m->imp_dup = dup_ppm;
    m->imp_reorder = reorder_ppm;
    m->imp_state = seed ? seed : 0x9e3779b97f4a7c15ull;
    if (m->held_n)
        wrt_pc_send_dtls(pc, m->held, m->held_n);
    m->held_n = 0;
}

void wrt_media_rx_age(wrt_pc* pc, uint32_t id, uint64_t us)
{
    wrt_media* m = pc->media;
    uint32_t i;
    for (i = 0; m && i < m->nrx; i++)
        if (m->rx[i].id == id && m->rx[i].last_us)
            m->rx[i].last_us = m->rx[i].last_us > us ? m->rx[i].last_us - us : 1;
}

static void wrt_wire(wrt_pc* pc, wrt_media* m, const uint8_t* p, size_t n)
{
    if (m->imp_loss || m->imp_dup || m->imp_reorder) {
        if (m->imp_loss && wrt_imp_roll(m) < m->imp_loss) {
            m->c.imp_drop++;
            return;
        }
        if (m->imp_reorder && !m->held_n && (m->held || (m->held = (uint8_t*)malloc(WRT_MEDIA_BUF)) != NULL)
            && wrt_imp_roll(m) < m->imp_reorder) {
            memcpy(m->held, p, n);
            m->held_n = n;
            m->c.imp_reorder++;
            return;
        }
        wrt_pc_send_dtls(pc, p, n);
        if (m->imp_dup && wrt_imp_roll(m) < m->imp_dup) {
            m->c.imp_dup++;
            wrt_pc_send_dtls(pc, p, n);
        }
        if (m->held_n) {
            wrt_pc_send_dtls(pc, m->held, m->held_n);
            m->held_n = 0;
        }
        return;
    }
    wrt_pc_send_dtls(pc, p, n);
}

static int wrt_tx_emit(wrt_pc* pc, wrt_media* m, wrt_tx* t, uint8_t pt, int marker, uint16_t seq, uint32_t ts, const uint32_t* csrc,
    unsigned cc, int level, const uint8_t* clevels, const uint8_t* payload, size_t len)
{
    rtc_rtp_ext ext[3];
    uint8_t lv;
    unsigned next = 0;
    size_t n, ol = 0;
    if (t->mid_ext && t->midlen) {
        ext[next].id = t->mid_ext;
        ext[next].len = t->midlen;
        ext[next++].val = (const uint8_t*)t->mid;
    }
    if (t->alevel_ext && level >= 0) {
        lv = (uint8_t)(level > 255 ? 255 : level);
        ext[next].id = t->alevel_ext;
        ext[next].len = 1;
        ext[next++].val = &lv;
    }
    if (t->clevel_ext && clevels && cc) {
        ext[next].id = t->clevel_ext;
        ext[next].len = (uint8_t)cc;
        ext[next++].val = clevels;
    }
    n = rtc_rtp_write(wrt_obuf, WRT_RTP_MAX - WRT_SRTP_MAX_TRAILER, pt, marker, seq, ts, t->ssrc, csrc, cc, ext, next, payload, len);
    if (!n)
        return -2;
    if (wrt_srtp_protect(&m->stx, wrt_obuf, n, WRT_MEDIA_BUF, &ol) != WRT_SRTP_OK) {
        m->c.send_fail++;
        return -3;
    }
    t->pkts++;
    t->bytes += len;
    t->last_ts = ts;
    m->c.rtp_out++;
    wrt_wire(pc, m, wrt_obuf, ol);
    return 0;
}

int wrt_media_send(wrt_pc* pc, uint32_t id, const wrt_rtp_out* o)
{
    wrt_media* m = pc->media;
    wrt_tx* t = wrt_tx_by_id(m, id);
    uint32_t pt;
    int r;
    if (!t || !t->live || !m->keyed || pc->closed)
        return -1;
    pt = o->pt == WRT_PT_DEFAULT ? t->pt : o->pt;
    if (o->cc > 15 || pt > 127)
        return -2;
    r = wrt_tx_emit(pc, m, t, (uint8_t)pt, o->marker, t->seq, t->ts_off + o->ts, o->csrc, o->cc, o->level, o->clevels, o->payload,
        o->len);
    if (r == 0)
        t->seq++;
    return r;
}

static void wrt_relay_forward(wrt_relay* hub, const uint8_t* p, const rtc_rtp_hdr* h, int level, const uint8_t* clevels, size_t ncl)
{
    uint32_t csrc[15], i;
    unsigned k;
    if (hub->ended)
        return;
    for (k = 0; k < h->cc; k++)
        csrc[k] = (uint32_t)p[12 + 4 * k] << 24 | (uint32_t)p[13 + 4 * k] << 16 | (uint32_t)p[14 + 4 * k] << 8 | p[15 + 4 * k];
    for (i = 0; i < hub->n; i++) {
        wrt_sink* s = &hub->s[i];
        wrt_pc* dst = s->pc;
        wrt_media* dm = dst->media;
        wrt_tx* t = wrt_tx_by_id(dm, s->id);
        uint8_t pt = s->map[h->pt & 127];
        uint16_t seq;
        if (!t || !dm->keyed || dst->closed)
            continue;
        if (pt == WRT_PT_NONE) {
            dm->c.relay_skip++;
            continue;
        }
        if (!s->have || s->src_ssrc != h->ssrc || t->seq != s->next)
            s->seq_off = (uint16_t)(t->seq - h->seq);
        if (!s->have || s->src_ssrc != h->ssrc) {
            s->ts_delta = (t->pkts ? t->last_ts + 1u : t->ts_off) - h->ts;
            s->src_ssrc = h->ssrc;
            s->have = 1;
        }
        seq = (uint16_t)(h->seq + s->seq_off);
        if (wrt_tx_emit(dst, dm, t, pt, h->marker, seq, h->ts + s->ts_delta, h->cc ? csrc : NULL, h->cc, level,
                ncl >= h->cc ? clevels : NULL, p + h->hlen, h->plen)
            == 0) {
            dm->c.relay_out++;
            if ((int16_t)(uint16_t)(seq + 1u - t->seq) > 0)
                t->seq = (uint16_t)(seq + 1u);
            s->next = t->seq;
        }
    }
}

static void wrt_dtmf_packet(wrt_pc* pc, wrt_media* m, wrt_tx* t, int end, uint32_t dur)
{
    uint8_t pl[4];
    pl[0] = t->dtmf->event;
    pl[1] = (uint8_t)((end ? 0x80u : 0) | WRT_DTMF_VOLUME);
    pl[2] = (uint8_t)(dur >> 8);
    pl[3] = (uint8_t)dur;
    if (wrt_tx_emit(pc, m, t, t->dtmf->te_pt, t->dtmf->marker, t->seq, t->dtmf->ts, NULL, 0, -1, NULL, pl, 4) == 0) {
        t->seq++;
        m->c.dtmf_out++;
    }
    t->dtmf->marker = 0;
}

static uint32_t wrt_units(uint64_t us, uint32_t clock)
{
    uint64_t u = us * clock / 1000000u;
    return u > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)u;
}

static uint32_t wrt_ts32(uint64_t us, uint32_t clock)
{
    return (uint32_t)(us / 1000000u * clock + us % 1000000u * clock / 1000000u);
}

static void wrt_dtmf_step(wrt_pc* pc, wrt_media* m, wrt_tx* t, uint64_t now)
{
    wrt_dtmf* d = t->dtmf;
    uint32_t total, dur;
    int i;
    for (;;) {
        total = wrt_units(d->end_us - d->seg_us, t->dtmf->te_clock);
        dur = wrt_units((now >= d->end_us ? d->end_us : now) - d->seg_us + WRT_DTMF_INTERVAL_US, t->dtmf->te_clock);
        if (dur > total)
            dur = total;
        if (dur <= 0xFFFFu || total <= 0xFFFFu)
            break;
        wrt_dtmf_packet(pc, m, t, 0, 0xFFFFu);
        d->ts += 0xFFFFu;
        d->seg_us += (uint64_t)0xFFFFu * 1000000u / t->dtmf->te_clock;
    }
    if (now >= d->end_us) {
        if (total > 0xFFFFu)
            total = 0xFFFFu;
        for (i = 0; i < (int)WRT_DTMF_ENDS; i++)
            wrt_dtmf_packet(pc, m, t, 1, total);
        d->active = 0;
        return;
    }
    wrt_dtmf_packet(pc, m, t, 0, dur > 0xFFFFu ? 0xFFFFu : dur);
    d->next_us = now + WRT_DTMF_INTERVAL_US;
    if (d->next_us > d->end_us)
        d->next_us = d->end_us;
}

int wrt_media_dtmf(wrt_pc* pc, uint32_t id, int event, uint32_t dur_ms, uint8_t te_pt, uint32_t te_clock, uint64_t now)
{
    wrt_media* m = pc->media;
    wrt_tx* t;
    if (!m || !(t = wrt_tx_by_id(m, id)) || !t->live || !m->keyed || pc->closed || te_pt > 127 || !te_clock || event < 0 || event > 15)
        return -1;
    if (!t->dtmf && !(t->dtmf = (wrt_dtmf*)calloc(1, sizeof(wrt_dtmf))))
        return -1;
    t->dtmf->te_pt = te_pt;
    t->dtmf->te_clock = te_clock;
    t->dtmf->active = 1;
    t->dtmf->event = (uint8_t)event;
    t->dtmf->marker = 1;
    t->dtmf->ts = t->ts_off + wrt_ts32(now, t->dtmf->te_clock);
    t->dtmf->seg_us = now;
    t->dtmf->end_us = now + (uint64_t)dur_ms * 1000u;
    wrt_dtmf_step(pc, m, t, now);
    return 0;
}

uint64_t wrt_media_tick(wrt_pc* pc, uint64_t now)
{
    wrt_media* m = pc->media;
    uint64_t next = UINT64_MAX;
    uint32_t i;
    if (!m || pc->closed)
        return UINT64_MAX;
    for (i = 0; i < m->ntx; i++) {
        wrt_tx* t = &m->tx[i];
        if (!t->dtmf || !t->dtmf->active)
            continue;
        if (!t->live || !m->keyed) {
            t->dtmf->active = 0;
            continue;
        }
        if (now >= t->dtmf->next_us)
            wrt_dtmf_step(pc, m, t, now);
        if (t->dtmf->active && t->dtmf->next_us < next)
            next = t->dtmf->next_us;
    }
    for (i = 0; i < m->nrx; i++) {
        wrt_rx* r = &m->rx[i];
        uint64_t dl;
        if (r->muted || !r->last_us)
            continue;
        dl = r->last_us + WRT_RX_TIMEOUT_US;
        if (now >= dl) {
            r->muted = 1;
            m->c.timeouts++;
            wrt_ev_push(pc, WRT_EV_RTP_MUTE, 0, r->id, 0, NULL, 0);
        } else if (dl < next)
            next = dl;
    }
    return next;
}

int wrt_media_send_sr(wrt_pc* pc, uint32_t id, uint64_t now)
{
    wrt_media* m = pc->media;
    wrt_tx* t = wrt_tx_by_id(m, id);
    rtc_rtcp_sr sr;
    struct timespec ts;
    size_t n, ol = 0;
    if (!t || !t->live || !m->keyed || pc->closed)
        return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    sr.ssrc = t->ssrc;
    sr.ntp = ((uint64_t)ts.tv_sec + 2208988800ull) << 32 | (uint64_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ull);
    sr.rtp_ts = t->ts_off + wrt_ts32(now, t->clock);
    sr.pkts = (uint32_t)t->pkts;
    sr.octets = (uint32_t)t->bytes;
    n = rtc_rtcp_write_sr(wrt_obuf, WRT_RTP_MAX - WRT_SRTP_MAX_TRAILER - 4, &sr, NULL, 0);
    if (!n || wrt_srtcp_protect(&m->stx, wrt_obuf, n, WRT_MEDIA_BUF, &ol) != WRT_SRTP_OK) {
        m->c.send_fail++;
        return -3;
    }
    m->c.rtcp_out++;
    wrt_pc_send_dtls(pc, wrt_obuf, ol);
    return 0;
}

int wrt_media_send_bye(wrt_pc* pc, uint32_t id)
{
    wrt_media* m = pc->media;
    wrt_tx* t = wrt_tx_by_id(m, id);
    size_t n, ol = 0;
    if (!t || !m->keyed || pc->closed)
        return -1;
    n = rtc_rtcp_write_bye(wrt_obuf, WRT_RTP_MAX - WRT_SRTP_MAX_TRAILER - 4, &t->ssrc, 1, NULL, 0);
    if (!n || wrt_srtcp_protect(&m->stx, wrt_obuf, n, WRT_MEDIA_BUF, &ol) != WRT_SRTP_OK) {
        m->c.send_fail++;
        return -3;
    }
    m->c.rtcp_out++;
    wrt_pc_send_dtls(pc, wrt_obuf, ol);
    return 0;
}

int wrt_media_sources(wrt_pc* pc, uint32_t id, int csrc, uint64_t now, wrt_src* out, int max)
{
    wrt_rx* r = wrt_rx_by_id(pc->media, id);
    const wrt_src* tab;
    int cap, i, k = 0, j;
    if (!r)
        return 0;
    tab = csrc ? r->cs : r->ss;
    cap = csrc ? WRT_RX_CSRC_MAX : WRT_RX_SSRC_MAX;
    for (i = 0; i < cap && k < max; i++) {
        wrt_src e;
        if (!tab[i].used || now - tab[i].at_us >= WRT_SRC_WINDOW_US)
            continue;
        e = tab[i];
        for (j = k; j > 0 && out[j - 1].at_us < e.at_us; j--)
            out[j] = out[j - 1];
        out[j] = e;
        k++;
    }
    return k;
}

int wrt_media_rx_stats(wrt_pc* pc, uint32_t id, wrt_rx_stats* out)
{
    wrt_rx* r = wrt_rx_by_id(pc->media, id);
    memset(out, 0, sizeof(*out));
    if (!r)
        return -1;
    out->pkts = r->pkts;
    out->bytes = r->bytes;
    out->has = r->has_st;
    if (r->te) {
        out->te_pkts = r->te->pkts;
        out->te_ends = r->te->ends;
        out->te_markers = r->te->markers;
        out->te_segs = r->te->segs;
        out->te_last_dur = r->te->last_dur;
        out->te_first_dur = r->te->first_dur;
        out->te_last_event = r->te->last_event;
        out->te_last_vol = r->te->last_vol;
    }
    if (r->has_st) {
        out->ssrc = r->st.ssrc;
        out->ext_max = rtc_rtp_rx_ext_max(&r->st);
        out->jitter = r->st.jitter >> 4;
        out->lost = rtc_rtp_rx_lost(&r->st);
    }
    return 0;
}

void wrt_media_counts_get(const wrt_pc* pc, wrt_media_counts* out)
{
    if (pc->media)
        *out = pc->media->c;
    else
        memset(out, 0, sizeof(*out));
}

size_t wrt_media_bytes(const wrt_pc* pc)
{
    const wrt_media* m = pc->media;
    size_t b;
    uint32_t i;
    if (!m)
        return 0;
    b = sizeof(*m) + (size_t)m->cap_rx * sizeof(wrt_rx) + (size_t)m->cap_tx * sizeof(wrt_tx) + (size_t)m->map_cap * 8u
        + (size_t)m->srx.cap * sizeof(wrt_srtp_stream) + (size_t)m->stx.cap * sizeof(wrt_srtp_stream) + (size_t)m->cap_hub * sizeof(void*)
        + (m->held ? WRT_MEDIA_BUF : 0);
    for (i = 0; i < m->nrx; i++)
        b += m->rx[i].te ? sizeof(wrt_te) : 0;
    for (i = 0; i < m->ntx; i++)
        b += m->tx[i].dtmf ? sizeof(wrt_dtmf) : 0;
    return b;
}

#endif
