#include "webrtc-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <stdlib.h>
#include <string.h>

#define WRT_ARENA_KEEP (64u << 10)

void wrt_ev_push(wrt_pc* pc, int type, int flag, uint32_t id, uint32_t a, const void* p, size_t n)
{
    wrt_ev* e;
    if (pc->nev == pc->cap_ev) {
        uint32_t nc = pc->cap_ev ? pc->cap_ev * 2 : 16;
        wrt_ev* ne = (wrt_ev*)realloc(pc->evs, nc * sizeof(*ne));
        if (!ne) {
            pc->ev_lost++;
            return;
        }
        pc->evs = ne;
        pc->cap_ev = nc;
    }
    if (n) {
        if ((size_t)pc->arena_len + n > UINT32_MAX) {
            pc->ev_lost++;
            return;
        }
        if (pc->arena_len + n > pc->arena_cap) {
            size_t nc = pc->arena_cap ? pc->arena_cap : 4096;
            uint8_t* na;
            while (nc < pc->arena_len + n)
                nc *= 2;
            na = (uint8_t*)realloc(pc->arena, nc);
            if (!na) {
                pc->ev_lost++;
                return;
            }
            pc->arena = na;
            pc->arena_cap = (uint32_t)(nc > UINT32_MAX ? UINT32_MAX : nc);
        }
        memcpy(pc->arena + pc->arena_len, p, n);
    }
    e = &pc->evs[pc->nev++];
    e->type = (uint8_t)type;
    e->flag = (uint8_t)flag;
    e->id = id;
    e->a = a;
    e->off = pc->arena_len;
    e->len = (uint32_t)n;
    pc->arena_len += (uint32_t)n;
}

void wrt_pc_events_done(wrt_pc* pc)
{
    pc->nev = 0;
    pc->arena_len = 0;
    if (pc->arena_cap > WRT_ARENA_KEEP) {
        free(pc->arena);
        pc->arena = NULL;
        pc->arena_cap = 0;
    }
}


static void wrt_sock_send(wrt_pc* pc, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    if (sock < 0 || sock >= pc->nsocks)
        return;
    if (wrt_udp_send(&pc->socks[sock].u, to, p, n) == 0) {
        pc->bytes_tx += n;
        pc->pkts_tx++;
    } else
        pc->send_errors++;
}

static void wrt_route(wrt_pc* pc, const rtc_cand* l, const rtc_addr* to, const uint8_t* p, size_t n)
{
    if (l->relay) {
        int ti = l->relay - 1;
        if (ti < pc->nturns && rtc_turn_send(&pc->turns[ti], to, p, n, wrt_now_us()) == 0) {
            pc->bytes_tx += n;
            pc->pkts_tx++;
        }
        return;
    }
    wrt_sock_send(pc, l->sock, to, p, n);
}

static void wrt_ice_send(void* ud, const rtc_cand* local, const rtc_addr* to, const uint8_t* p, size_t n)
{
    wrt_route((wrt_pc*)ud, local, to, p, n);
}

void wrt_pc_send_dtls(wrt_pc* pc, const uint8_t* p, size_t n)
{
    const rtc_cand *l, *r;
    if (pc->closed && pc->dtls.state != WRT_DTLS_CONNECTED)
        return;
    if (pc->ice.state == RTC_ICE_FAILED)
        return;
    if (rtc_ice_get_selected(&pc->ice, &l, &r)) {
        wrt_route(pc, l, &r->addr, p, n);
        return;
    }
    if (pc->have_dtls_peer && !pc->ever_selected)
        wrt_route(pc, &pc->ice.local[pc->dtls_local], &pc->dtls_peer, p, n);
}


void wrt_pc_dtls_data(wrt_pc* pc, const uint8_t* p, size_t n)
{
    if (pc->sctp)
        rtc_sctp_input(pc->sctp, p, n, wrt_now_us());
}

static void wrt_sctp_begin(wrt_pc* pc)
{
    if (pc->dtls.state != WRT_DTLS_CONNECTED || !pc->has_sctp || pc->sctp)
        return;
    pc->sctp = rtc_sctp_new(&wrt_sctp_cbs, pc, &pc->rng, pc->sctp_port, pc->remote_sctp_port, WRT_SCTP_MTU);
    if (!pc->sctp)
        return;
    pc->sctp->max_msg = pc->local_max_msg;
    if (pc->sctp_streams)
        pc->sctp->local_streams = pc->sctp_streams;
    pc->sctp->zcsum_local = (uint8_t)!pc->sctp_zcsum_off;
    rtc_sctp_connect(pc->sctp, wrt_now_us());
}

void wrt_pc_dtls_state(wrt_pc* pc)
{
    int st = pc->dtls.state;
    wrt_ev_push(pc, WRT_EV_DTLS_STATE, 0, 0, (uint32_t)st, NULL, 0);
    if (st == WRT_DTLS_CONNECTED) {
        wrt_media_key(pc);
        wrt_sctp_begin(pc);
    }
    else if ((st == WRT_DTLS_CLOSED || st == WRT_DTLS_FAILED) && pc->sctp && pc->sctp->state != RTC_SCTP_ABORTED
        && pc->sctp->state != RTC_SCTP_CLOSED)
        wrt_sctp_on_state(pc, RTC_SCTP_CLOSED);
}

static void wrt_release_orphans(wrt_pc* pc);

static void wrt_ice_ev(void* ud, int ev)
{
    wrt_pc* pc = (wrt_pc*)ud;
    if (ev == RTC_ICE_EV_STATE)
        wrt_ev_push(pc, WRT_EV_ICE_STATE, 0, 0, (uint32_t)pc->ice.state, NULL, 0);
    else if (ev == RTC_ICE_EV_SELECTED) {
        pc->ever_selected = 1;
        if (pc->release_due)
            wrt_release_orphans(pc);
        wrt_ev_push(pc, WRT_EV_PAIR, 0, 0, (uint32_t)pc->ice.selected, NULL, 0);
        if (pc->dtls.ssl && pc->dtls.state == WRT_DTLS_NEW && !pc->dtls.is_server)
            wrt_dtls_start(&pc->dtls);
    }
}

#define WRT_EARLY_MAX 4

struct wrt_early {
    uint32_t len[WRT_EARLY_MAX];
    uint8_t buf[WRT_EARLY_MAX][1500];
};

static void wrt_early_hold(wrt_pc* pc, const uint8_t* p, size_t n)
{
    if (n > 1500 || pc->nearly >= WRT_EARLY_MAX) {
        pc->dtls_dropped++;
        return;
    }
    if (!pc->early) {
        pc->early = (struct wrt_early*)malloc(sizeof(*pc->early));
        if (!pc->early)
            return;
    }
    memcpy(pc->early->buf[pc->nearly], p, n);
    pc->early->len[pc->nearly++] = (uint32_t)n;
}

static void wrt_early_replay(wrt_pc* pc)
{
    struct wrt_early* e = pc->early;
    int i, k = pc->nearly;
    pc->early = NULL;
    pc->nearly = 0;
    if (!e)
        return;
    pc->in_input++;
    for (i = 0; i < k && pc->dtls.ssl; i++)
        wrt_dtls_input(&pc->dtls, e->buf[i], e->len[i]);
    pc->in_input--;
    free(e);
}

static int wrt_demux(wrt_pc* pc, int sock, int relay, const rtc_addr* from, const uint8_t* p, size_t n, uint64_t now)
{
    uint8_t b;
    if (!n)
        return 0;
    b = p[0];
    pc->bytes_rx += n;
    pc->pkts_rx++;
    if (b < 4) {
        rtc_stun_msg m;
        if (rtc_stun_parse(&m, p, n) < 0)
            return 1;
        rtc_ice_input(&pc->ice, rtc_ice_find_local(&pc->ice, sock, relay ? relay : -1), from, &m, now);
        return 1;
    }
    if (b >= 20 && b <= 63) {
        int li = rtc_ice_find_local(&pc->ice, sock, relay ? relay : -1);
        if (li < 0 || !rtc_ice_peer_ok(&pc->ice, li, from)) {
            pc->dtls_dropped++;
            return 1;
        }
        rtc_ice_note_rx(&pc->ice, from);
        if (!pc->have_dtls_peer || pc->dtls.state == WRT_DTLS_NEW || pc->dtls.state == WRT_DTLS_CONNECTING) {
            pc->dtls_local = li;
            pc->dtls_peer = *from;
            pc->have_dtls_peer = 1;
        }
        if (!pc->dtls.ssl) {
            wrt_early_hold(pc, p, n);
            return 1;
        }
        wrt_dtls_input(&pc->dtls, p, n);
        return 1;
    }
    if (b >= 128 && b <= 191) {
        int li = rtc_ice_find_local(&pc->ice, sock, relay ? relay : -1);
        if (li < 0 || !rtc_ice_peer_ok(&pc->ice, li, from)) {
            pc->rtp_dropped++;
            return 0;
        }
        rtc_ice_note_rx(&pc->ice, from);
        return wrt_media_input(pc, p, n, now);
    }
    return 1;
}

static void wrt_on_dgram(void* ud, const uint8_t* buf, size_t len, const rtc_addr* src)
{
    wrt_sock* s = (wrt_sock*)ud;
    wrt_pc* pc = s->pc;
    rtc_addr from = *src;
    uint64_t now;
    int i, dirty = 1;
    if (pc->closed)
        return;
    now = wrt_now_us();
    pc->in_input++;
    for (i = 0; i < pc->nturns; i++) {
        rtc_turn* t = &pc->turns[i];
        if (t->sock == s->idx && rtc_addr_eq(&t->server, &from)) {
            rtc_addr pr;
            const uint8_t* d = NULL;
            size_t dl = 0;
            int r = rtc_turn_input(t, buf, len, now, &pr, &d, &dl);
            if (r == 2) {
                dirty = wrt_demux(pc, s->idx, i + 1, &pr, d, dl, now);
                goto done;
            }
            if (r == 1)
                goto done;
        }
    }
    dirty = wrt_demux(pc, s->idx, 0, &from, buf, len, now);
done:
    if (dirty)
        pc->rx_dirty = 1;
    pc->in_input--;
    if (!pc->in_input && pc->free_pending)
        wrt_pc_free(pc);
}

static void wrt_on_batch_end(void* ud)
{
    wrt_pc* pc = ((wrt_sock*)ud)->pc;
    if (pc->closed || !pc->rx_dirty)
        return;
    pc->rx_dirty = 0;
    pc->rx_drains++;
    pc->in_input++;
    if (pc->notify)
        pc->notify(pc);
    pc->in_input--;
    if (!pc->in_input && pc->free_pending)
        wrt_pc_free(pc);
}

static void wrt_turn_send_cb(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n)
{
    wrt_sock_send((wrt_pc*)ud, sock, to, p, n);
}

static void wrt_add_local(wrt_pc* pc, const rtc_cand* c)
{
    int idx;
    if (pc->closed)
        return;
    idx = rtc_ice_add_local(&pc->ice, c);
    if (idx >= 0)
        wrt_ev_push(pc, WRT_EV_CANDIDATE, 0, 0, (uint32_t)idx, NULL, 0);
}

static void wrt_gather_check(wrt_pc* pc)
{
    if (pc->gathering && !pc->gather_pending && !pc->gather_done) {
        pc->gather_done = 1;
        pc->ice.local_done = 1;
        pc->gathering = 0;
        wrt_ev_push(pc, WRT_EV_GATHER_DONE, 0, 0, 0, NULL, 0);
    }
}

static const char* wrt_turn_url(const wrt_pc* pc, int idx)
{
    return pc->turn_server[idx] < pc->nservers ? pc->servers[pc->turn_server[idx]].url : "";
}

static void wrt_add_srflx(wrt_pc* pc, int idx)
{
    rtc_turn* t = &pc->turns[idx];
    wrt_sock* s = &pc->socks[t->sock];
    rtc_cand c;
    if (pc->relay_only || !t->mapped.fam || rtc_addr_eq(&t->mapped, &s->u.local))
        return;
    memset(&c, 0, sizeof(c));
    c.comp = 1;
    c.sock = (uint8_t)t->sock;
    c.type = RTC_CAND_SRFLX;
    c.addr = t->mapped;
    c.base = s->u.local;
    c.rel = s->u.local;
    c.prio = rtc_ice_cand_prio(RTC_CAND_SRFLX, 65535u - (uint32_t)t->sock, 1);
    c.found = rtc_ice_foundation(RTC_CAND_SRFLX, &s->u.local, wrt_turn_url(pc, idx));
    wrt_add_local(pc, &c);
}

static void wrt_turn_ev_cb(void* ud, int idx, int ev, int code)
{
    wrt_pc* pc = (wrt_pc*)ud;
    rtc_turn* t = &pc->turns[idx];
    wrt_sock* s = &pc->socks[t->sock];
    const char* url = wrt_turn_url(pc, idx);
    rtc_cand c;
    memset(&c, 0, sizeof(c));
    c.comp = 1;
    c.sock = (uint8_t)t->sock;
    if (ev == RTC_TURN_EV_MAPPED) {
        wrt_add_srflx(pc, idx);
        if (t->kind == RTC_TURN_KIND_STUN && !(pc->turn_done & (1ull << idx))) {
            pc->turn_done |= 1ull << idx;
            pc->gather_pending--;
        }
    } else if (ev == RTC_TURN_EV_RELAYED) {
        c.type = RTC_CAND_RELAY;
        c.addr = t->relayed;
        c.base = t->relayed;
        c.rel = t->mapped;
        c.relay = (uint8_t)(idx + 1);
        c.prio = rtc_ice_cand_prio(RTC_CAND_RELAY, 65535u - (uint32_t)idx, 1);
        c.found = rtc_ice_foundation(RTC_CAND_RELAY, &s->u.local, url);
        wrt_add_local(pc, &c);
        if (!(pc->turn_done & (1ull << idx))) {
            pc->turn_done |= 1ull << idx;
            pc->gather_pending--;
        }
    } else if (ev == RTC_TURN_EV_FAILED) {
        wrt_ev_push(pc, WRT_EV_CAND_ERROR, 0, (uint32_t)pc->turn_server[idx], (uint32_t)code, NULL, 0);
        if (!(pc->turn_done & (1ull << idx))) {
            pc->turn_done |= 1ull << idx;
            pc->gather_pending--;
        }
    }
    wrt_gather_check(pc);
}

static int wrt_open_sock(wrt_pc* pc, const rtc_addr* a)
{
    wrt_sock* s;
    rtc_addr b = *a;
    if (pc->nsocks >= WRT_MAX_SOCKS)
        return -1;
    s = &pc->socks[pc->nsocks];
    s->pc = pc;
    s->idx = (uint8_t)pc->nsocks;
    s->loopback = (uint8_t)rtc_addr_is_loopback(a);
    b.port = 0;
    if (wrt_udp_open(&s->u, pc->aio, &b, wrt_on_dgram, wrt_on_batch_end, s) < 0)
        return -1;
    return pc->nsocks++;
}


static void wrt_add_host(wrt_pc* pc, int si)
{
    rtc_cand c;
    if (pc->relay_only)
        return;
    memset(&c, 0, sizeof(c));
    c.type = RTC_CAND_HOST;
    c.addr = pc->socks[si].u.local;
    c.base = c.addr;
    c.comp = 1;
    c.sock = (uint8_t)si;
    c.prio = rtc_ice_cand_prio(RTC_CAND_HOST, 65535u - (uint32_t)si * 256u - (c.addr.fam == 6 ? 0u : 128u), 1);
    c.found = rtc_ice_foundation(RTC_CAND_HOST, &c.base, NULL);
    wrt_add_local(pc, &c);
}

static int wrt_turn_add(wrt_pc* pc, int i, int j)
{
    wrt_server* sv = &pc->servers[i];
    wrt_sock* s = &pc->socks[j];
    int ti, k;
    if (s->u.local.fam != sv->addr.fam || s->loopback != (uint8_t)rtc_addr_is_loopback(&sv->addr))
        return 0;
    for (k = 0; k < pc->nturns; k++)
        if (pc->turn_server[k] == (uint8_t)i && pc->turns[k].sock == j)
            return 0;
    for (ti = 0; ti < pc->nturns; ti++)
        if (pc->turn_server[ti] == 0xFF && pc->turns[ti].state == RTC_TURN_CLOSED)
            break;
    if (ti == pc->nturns && pc->nturns >= 64)
        return -1;
    if (ti == pc->nturns && pc->nturns == pc->turns_cap) {
        int nc = pc->turns_cap ? pc->turns_cap * 2 : 8;
        rtc_turn* nt;
        uint8_t* ns;
        if (nc > 64)
            nc = 64;
        nt = (rtc_turn*)realloc(pc->turns, (size_t)nc * sizeof(rtc_turn));
        if (!nt)
            return -1;
        pc->turns = nt;
        ns = (uint8_t*)realloc(pc->turn_server, (size_t)nc);
        if (!ns)
            return -1;
        pc->turn_server = ns;
        pc->turns_cap = nc;
    }
    if (ti == pc->nturns)
        pc->nturns++;
    pc->turn_done &= ~(1ull << ti);
    memset(&pc->turns[ti], 0, sizeof(rtc_turn));
    pc->turn_server[ti] = (uint8_t)i;
    rtc_turn_init(&pc->turns[ti], &pc->rng, ti, sv->turn ? RTC_TURN_KIND_TURN : RTC_TURN_KIND_STUN, j, &sv->addr, sv->user, sv->pass,
        wrt_turn_send_cb, wrt_turn_ev_cb, pc);
    pc->gather_pending++;
    rtc_turn_start(&pc->turns[ti], wrt_now_us());
    return 1;
}

static void wrt_gather_servers(wrt_pc* pc)
{
    int i, j;
    for (i = 0; i < pc->nservers; i++) {
        wrt_server* sv = &pc->servers[i];
        int matched = 0, full = 0;
        if ((!sv->turn && pc->relay_only) || sv->tcp)
            continue;
        for (j = 0; j < pc->nsocks; j++) {
            int k, have = 0, r = 0;
            for (k = 0; k < pc->nturns; k++)
                if (pc->turn_server[k] == (uint8_t)i && pc->turns[k].sock == j)
                    have = 1;
            if (!have)
                r = wrt_turn_add(pc, i, j);
            if (have || r > 0)
                matched = 1;
            else if (r < 0)
                full = 1;
        }
        if (!matched)
            wrt_ev_push(pc, WRT_EV_CAND_ERROR, full, (uint32_t)i, 701, NULL, 0);
    }
}

static int wrt_have_sock(const wrt_pc* pc, const rtc_addr* a)
{
    int j;
    for (j = 0; j < pc->nsocks; j++) {
        rtc_addr l = pc->socks[j].u.local;
        l.port = a->port;
        if (rtc_addr_eq(&l, a))
            return 1;
    }
    return 0;
}

int wrt_pc_gather(wrt_pc* pc)
{
    rtc_addr addrs[WRT_MAX_SOCKS];
    int n, i;
    if (pc->gathering || pc->gather_done || pc->closed)
        return 0;
    pc->gathering = 1;
    pc->relay_only = pc->relay_next;
    if (pc->bind_n)
        n = pc->bind_n, memcpy(addrs, pc->bind_addrs, (size_t)n * sizeof(rtc_addr));
    else
        n = wrt_collect_addrs(addrs, WRT_MAX_SOCKS);
    for (i = 0; i < n; i++) {
        int si = wrt_open_sock(pc, &addrs[i]);
        if (si >= 0)
            wrt_add_host(pc, si);
    }
    pc->ice.relay_only = pc->relay_only;
    wrt_gather_servers(pc);
    wrt_gather_check(pc);
    return 0;
}

int wrt_pc_local_shown(const wrt_pc* pc, int i)
{
    const rtc_cand* c = &pc->ice.local[i];
    return !c->off && (!pc->relay_only || c->type == RTC_CAND_RELAY);
}

static void wrt_hide_relay(wrt_pc* pc, int ti)
{
    int k;
    for (k = 0; k < pc->ice.nlocal; k++)
        if (pc->ice.local[k].relay == ti + 1)
            pc->ice.local[k].off = 1;
}

static void wrt_release_orphans(wrt_pc* pc)
{
    int i, keep = -1;
    if (pc->ice.selected >= 0)
        keep = pc->ice.local[pc->ice.pairs[pc->ice.selected].l].relay - 1;
    pc->release_due = 0;
    for (i = 0; i < pc->nturns; i++) {
        if (pc->turn_server[i] != 0xFF || pc->turns[i].state == RTC_TURN_CLOSED)
            continue;
        if (i == keep) {
            pc->release_due = 1;
            continue;
        }
        rtc_turn_close(&pc->turns[i], wrt_now_us());
        wrt_hide_relay(pc, i);
    }
}

void wrt_pc_regather(wrt_pc* pc)
{
    rtc_addr addrs[WRT_MAX_SOCKS];
    int i, n;
    if (pc->closed || pc->gathering)
        return;
    for (i = 0; i < pc->nturns; i++)
        if (pc->turn_server[i] == 0xFF && pc->turns[i].state != RTC_TURN_CLOSED) {
            wrt_hide_relay(pc, i);
            pc->release_due = 1;
        }
    pc->gather_done = 0;
    pc->ice.local_done = 0;
    pc->gathering = 1;
    pc->relay_only = pc->relay_next;
    pc->ice.relay_only = pc->relay_only;
    for (i = 0; i < pc->ice.nlocal; i++)
        if (wrt_pc_local_shown(pc, i))
            wrt_ev_push(pc, WRT_EV_CANDIDATE, 0, 0, (uint32_t)i, NULL, 0);
    if (pc->bind_n)
        n = pc->bind_n, memcpy(addrs, pc->bind_addrs, (size_t)n * sizeof(rtc_addr));
    else
        n = wrt_collect_addrs(addrs, WRT_MAX_SOCKS);
    for (i = 0; i < pc->nsocks; i++)
        wrt_add_host(pc, i);
    for (i = 0; i < pc->nturns; i++)
        wrt_add_srflx(pc, i);
    for (i = 0; i < n; i++) {
        int si;
        if (wrt_have_sock(pc, &addrs[i]))
            continue;
        si = wrt_open_sock(pc, &addrs[i]);
        if (si >= 0)
            wrt_add_host(pc, si);
    }
    wrt_gather_servers(pc);
    wrt_gather_check(pc);
}


int wrt_pc_add_server(wrt_pc* pc, const char* url, const char* user, const char* pass, const char** err)
{
    wrt_server sv;
    size_t ul;
    memset(&sv, 0, sizeof(sv));
    if (wrt_parse_server(url, &sv, err) < 0)
        return -1;
    if (sv.turn) {
        if (!user || !pass) {
            *err = "InvalidAccessError";
            return -1;
        }
        if (strlen(user) >= sizeof(sv.user) || strlen(pass) >= sizeof(sv.pass)) {
            *err = "InvalidAccessError";
            return -1;
        }
        memcpy(sv.user, user, strlen(user) + 1);
        memcpy(sv.pass, pass, strlen(pass) + 1);
    }
    ul = strlen(url);
    if (ul >= sizeof(sv.url))
        ul = sizeof(sv.url) - 1;
    memcpy(sv.url, url, ul);
    sv.url[ul] = 0;
    if (pc->nservers >= WRT_MAX_SERVERS) {
        *err = "NotSupportedError";
        return -1;
    }
    if (sv.unresolved)
        return 0;
    pc->servers[pc->nservers++] = sv;
    return 0;
}

wrt_pc* wrt_pc_new(struct dyn_aio* aio, wrt_cert* cert, const uint8_t seed[32])
{
    wrt_pc* pc = (wrt_pc*)calloc(1, sizeof(*pc));
    if (!pc)
        return NULL;
    pc->aio = aio;
    rtc_rng_seed(&pc->rng, seed);
    rtc_ice_init(&pc->ice, &pc->rng, 1, wrt_ice_send, wrt_ice_ev, pc);
    pc->cert = cert;
    wrt_cert_ref(cert);
    pc->sctp_port = 5000;
    pc->remote_sctp_port = 5000;
    pc->local_max_msg = 16u << 20;
    pc->remote_max_msg = 65536;
    return pc;
}

int wrt_pc_set_dtls_role(wrt_pc* pc, int is_server, const uint8_t* fp, size_t fp_len, int fp_alg)
{
    if (!pc->dtls.ssl) {
        if (wrt_dtls_init(&pc->dtls, pc, pc->cert, is_server) < 0)
            return -1;
        pc->sctp_client = !is_server;
        pc->role_known = 1;
    }
    if (fp_len > sizeof(pc->dtls.remote_fp))
        fp_len = sizeof(pc->dtls.remote_fp);
    memcpy(pc->dtls.remote_fp, fp, fp_len);
    pc->dtls.remote_fp_len = (uint8_t)fp_len;
    pc->dtls.remote_fp_alg = (uint8_t)fp_alg;
    wrt_pc_assign_ids(pc);
    if (pc->nearly)
        wrt_early_replay(pc);
    return 0;
}

int wrt_pc_add_remote_cand(wrt_pc* pc, const rtc_sdp_cand* sc)
{
    rtc_cand c;
    if (!sc->udp || sc->comp != 1 || !sc->addr.fam || !sc->addr.port)
        return 0;
    memset(&c, 0, sizeof(c));
    c.addr = sc->addr;
    c.base = sc->addr;
    c.rel = sc->rel;
    c.prio = sc->prio;
    c.type = sc->type;
    c.comp = 1;
    {
        uint32_t h = 2166136261u;
        const char* f;
        for (f = sc->foundation; *f; f++)
            h = (h ^ (uint8_t)*f) * 16777619u;
        c.found = h;
    }
    return rtc_ice_add_remote(&pc->ice, &c) < 0 ? -1 : 0;
}

void wrt_pc_remote_eoc(wrt_pc* pc)
{
    pc->ice.remote_done = 1;
}

void wrt_pc_start(wrt_pc* pc)
{
    if (pc->closed)
        return;
    rtc_ice_start(&pc->ice, wrt_now_us());
    if (pc->dtls.ssl && pc->dtls.state == WRT_DTLS_NEW && !pc->dtls.is_server && pc->ice.selected >= 0)
        wrt_dtls_start(&pc->dtls);
    wrt_sctp_begin(pc);
}

uint64_t wrt_pc_tick(wrt_pc* pc, uint64_t now)
{
    uint64_t next = UINT64_MAX, d;
    int i;
    if (pc->closed)
        return UINT64_MAX;
    pc->in_input++;
    d = rtc_ice_tick(&pc->ice, now);
    if (d < next)
        next = d;
    for (i = 0; i < pc->nturns; i++) {
        d = rtc_turn_tick(&pc->turns[i], now);
        if (d < next)
            next = d;
    }
    if (pc->dtls.ssl && pc->dtls.state == WRT_DTLS_CONNECTING) {
        d = wrt_dtls_deadline(&pc->dtls, now);
        if (d <= now) {
            wrt_dtls_timeout(&pc->dtls);
            d = wrt_dtls_deadline(&pc->dtls, now);
        }
        if (d < next)
            next = d;
    }
    if (pc->sctp) {
        d = rtc_sctp_tick(pc->sctp, now);
        if (d < next)
            next = d;
    }
    d = wrt_media_tick(pc, now);
    if (d < next)
        next = d;
    pc->in_input--;
    return next;
}

void wrt_pc_flush(wrt_pc* pc)
{
    if (pc->sctp && !pc->closed)
        rtc_sctp_flush(pc->sctp, wrt_now_us());
}

void wrt_pc_close(wrt_pc* pc)
{
    uint32_t i;
    uint64_t now = wrt_now_us();
    if (pc->closed)
        return;
    pc->in_input++;
    if (pc->sctp && pc->sctp->state == RTC_SCTP_ESTABLISHED)
        rtc_sctp_abort(pc->sctp);
    for (i = 0; i < pc->ndcs; i++)
        if (pc->dcs[i] && pc->dcs[i]->state != WRT_DC_CLOSED) {
            pc->dcs[i]->state = WRT_DC_CLOSED;
            if (pc->dcs[i]->has_id && pc->dcs[i]->id < pc->id_cap)
                pc->id2slot[pc->dcs[i]->id] = 0;
        }
    if (pc->dtls.ssl)
        wrt_dtls_close(&pc->dtls);
    for (i = 0; i < (uint32_t)pc->nturns; i++)
        rtc_turn_close(&pc->turns[i], now);
    rtc_ice_close(&pc->ice);
    pc->closed = 1;
    for (i = 0; i < (uint32_t)pc->nsocks; i++)
        wrt_udp_close(&pc->socks[i].u);
    pc->in_input--;
}

void wrt_pc_free(wrt_pc* pc)
{
    uint32_t i;
    if (!pc)
        return;
    if (pc->in_input) {
        pc->free_pending = 1;
        return;
    }
    pc->notify = NULL;
    wrt_pc_close(pc);
    if (pc->sctp)
        rtc_sctp_free(pc->sctp);
    wrt_dtls_free(&pc->dtls);
    wrt_media_free(pc);
    for (i = 0; i < pc->ndcs; i++)
        if (pc->dcs[i]) {
            free(pc->dcs[i]->label);
            free(pc->dcs[i]->protocol);
            free(pc->dcs[i]);
        }
    free(pc->dcs);
    free(pc->id2slot);
    free(pc->evs);
    free(pc->arena);
    free(pc->early);
    free(pc->turns);
    free(pc->turn_server);
    wrt_cert_unref(pc->cert);
    free(pc);
}

static const char* const wrt_typ[] = { "host", "srflx", "prflx", "relay" };

static void wrt_to_sdp_cand(const rtc_cand* c, rtc_sdp_cand* sc)
{
    memset(sc, 0, sizeof(*sc));
    {
        uint32_t f = c->found;
        char tmp[16];
        int k = 0, j;
        do {
            tmp[k++] = (char)('0' + f % 10);
            f /= 10;
        } while (f);
        for (j = 0; j < k; j++)
            sc->foundation[j] = tmp[k - 1 - j];
        sc->foundation[k] = 0;
    }
    memcpy(sc->typ, wrt_typ[c->type & 3], strlen(wrt_typ[c->type & 3]) + 1);
    sc->addr = c->addr;
    sc->comp = 1;
    sc->prio = c->prio;
    sc->udp = 1;
    sc->type = c->type;
    if (c->type != RTC_CAND_HOST) {
        sc->has_rel = 1;
        sc->rel = c->rel;
    }
}

size_t wrt_pc_cand_text(const wrt_pc* pc, int i, char* out, size_t cap)
{
    rtc_sdp_cand sc;
    if (i < 0 || i >= pc->ice.nlocal)
        return 0;
    wrt_to_sdp_cand(&pc->ice.local[i], &sc);
    return rtc_sdp_write_candidate(&sc, out, cap);
}

size_t wrt_pc_local_cands(const wrt_pc* pc)
{
    return (size_t)pc->ice.nlocal;
}

size_t wrt_cand_text(const rtc_cand* c, char* out, size_t cap)
{
    rtc_sdp_cand sc;
    wrt_to_sdp_cand(c, &sc);
    return rtc_sdp_write_candidate(&sc, out, cap);
}

#endif
