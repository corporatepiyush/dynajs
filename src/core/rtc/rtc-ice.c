#include "rtc-ice.h"

static const uint32_t rtc_type_pref[4] = { 126, 100, 110, 0 };

uint32_t rtc_ice_cand_prio(int type, uint32_t local_pref, int comp)
{
    return (rtc_type_pref[type & 3] << 24) | ((local_pref & 0xFFFF) << 8) | (uint32_t)(256 - comp);
}

uint32_t rtc_ice_foundation(int type, const rtc_addr* base, const char* server)
{
    uint32_t h = 2166136261u ^ (uint32_t)type;
    size_t i, n = base->fam == 4 ? 4 : 16;
    for (i = 0; i < n; i++)
        h = (h ^ base->ip[i]) * 16777619u;
    if (server)
        for (; *server; server++)
            h = (h ^ (uint8_t)*server) * 16777619u;
    return h & 0x7FFFFFFF;
}

static void rtc_ice_set_state(rtc_ice* ice, int st)
{
    if (ice->state == st)
        return;
    ice->state = st;
    if (ice->ev)
        ice->ev(ice->ud, RTC_ICE_EV_STATE);
}

static uint64_t rtc_pair_prio(uint32_t g, uint32_t d)
{
    uint64_t mn = g < d ? g : d, mx = g > d ? g : d;
    return (mn << 32) + 2 * mx + (g > d ? 1 : 0);
}

static void rtc_ice_reprio(rtc_ice* ice)
{
    int i;
    for (i = 0; i < ice->npairs; i++) {
        rtc_pair* p = &ice->pairs[i];
        uint32_t lp = ice->local[p->l].prio, rp = ice->remote[p->r].prio;
        p->prio = ice->controlling ? rtc_pair_prio(lp, rp) : rtc_pair_prio(rp, lp);
    }
}

void rtc_ice_init(rtc_ice* ice, rtc_rng* rng, int controlling, rtc_ice_send_fn send, rtc_ice_ev_fn ev, void* ud)
{
    memset(ice, 0, sizeof(*ice));
    ice->rng = rng;
    ice->send = send;
    ice->ev = ev;
    ice->ud = ud;
    ice->controlling = controlling;
    ice->tiebreaker = rtc_rng_next(rng);
    ice->selected = -1;
    ice->nominating = -1;
    ice->state = RTC_ICE_NEW;
}

static void rtc_cpy_cred(char* dst, const char* src)
{
    size_t n = strlen(src);
    if (n >= RTC_ICE_CRED_MAX)
        n = RTC_ICE_CRED_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void rtc_ice_set_local_creds(rtc_ice* ice, const char* ufrag, const char* pwd)
{
    rtc_cpy_cred(ice->lufrag, ufrag);
    rtc_cpy_cred(ice->lpwd, pwd);
}

void rtc_ice_stage_local(rtc_ice* ice, const char* ufrag, const char* pwd)
{
    rtc_cpy_cred(ice->stage_ufrag, ufrag);
    rtc_cpy_cred(ice->stage_pwd, pwd);
}

static void rtc_ice_keep_prev(rtc_ice* ice)
{
    rtc_pair* p;
    rtc_ice_prev* v = &ice->prev;
    if (ice->selected < 0)
        return;
    p = &ice->pairs[ice->selected];
    v->l = ice->local[p->l];
    v->r = ice->remote[p->r];
    v->last_consent_us = ice->last_consent_us;
    v->consent_next_us = ice->consent_next_us;
    memcpy(v->tid, p->tid, 12);
    memcpy(v->tid2, p->tid2, 12);
    v->controlling = (uint8_t)ice->controlling;
    memcpy(v->lufrag, ice->lufrag, sizeof(v->lufrag));
    memcpy(v->lpwd, ice->lpwd, sizeof(v->lpwd));
    memcpy(v->rufrag, ice->rufrag, sizeof(v->rufrag));
    memcpy(v->rpwd, ice->rpwd, sizeof(v->rpwd));
    ice->have_prev = 1;
}

static void rtc_ice_clear_checks(rtc_ice* ice)
{
    rtc_ice_keep_prev(ice);
    ice->npairs = 0;
    ice->nremote = 0;
    ice->ntrig = 0;
    ice->selected = -1;
    ice->nominating = -1;
    ice->remote_done = 0;
    ice->first_valid_us = 0;
}

int rtc_ice_set_remote_creds(rtc_ice* ice, const char* ufrag, const char* pwd)
{
    int changed = ice->rufrag[0] && (strcmp(ice->rufrag, ufrag) || strcmp(ice->rpwd, pwd));
    if (changed)
        rtc_ice_clear_checks(ice);
    rtc_cpy_cred(ice->rufrag, ufrag);
    rtc_cpy_cred(ice->rpwd, pwd);
    return changed;
}

void rtc_ice_set_role(rtc_ice* ice, int controlling)
{
    if (ice->controlling == controlling)
        return;
    ice->controlling = controlling;
    rtc_ice_reprio(ice);
    if (ice->ev)
        ice->ev(ice->ud, RTC_ICE_EV_ROLE);
}

static int rtc_ice_pair_find(const rtc_ice* ice, int l, int r)
{
    int i;
    for (i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].l == l && ice->pairs[i].r == r)
            return i;
    return -1;
}

static int rtc_ice_pair_redundant(const rtc_ice* ice, int l, int r)
{
    int i;
    const rtc_cand* lc = &ice->local[l];
    for (i = 0; i < ice->npairs; i++) {
        const rtc_cand* o = &ice->local[ice->pairs[i].l];
        if (ice->pairs[i].r == r && o->type == lc->type && o->sock == lc->sock && o->relay == lc->relay
            && rtc_addr_eq(&o->base, &lc->base))
            return 1;
    }
    return 0;
}

static int rtc_ice_pair_add(rtc_ice* ice, int l, int r)
{
    rtc_pair* p;
    const rtc_cand* lc = &ice->local[l];
    const rtc_cand* rc = &ice->remote[r];
    if (lc->type == RTC_CAND_SRFLX || lc->type == RTC_CAND_PRFLX || lc->off)
        return -1;
    if (ice->relay_only && lc->type != RTC_CAND_RELAY)
        return -1;
    if (lc->addr.fam != rc->addr.fam || lc->comp != rc->comp)
        return -1;
    if (lc->type != RTC_CAND_RELAY && (rtc_addr_is_link_local(&lc->addr) != rtc_addr_is_link_local(&rc->addr)))
        return -1;
    if (rtc_ice_pair_redundant(ice, l, r))
        return -1;
    if (ice->npairs >= RTC_ICE_MAX_PAIRS)
        return -1;
    p = &ice->pairs[ice->npairs];
    memset(p, 0, sizeof(*p));
    p->l = (uint8_t)l;
    p->r = (uint8_t)r;
    p->state = RTC_PAIR_FROZEN;
    p->prio = ice->controlling ? rtc_pair_prio(lc->prio, rc->prio) : rtc_pair_prio(rc->prio, lc->prio);
    return ice->npairs++;
}

int rtc_ice_add_local(rtc_ice* ice, const rtc_cand* c)
{
    int i, idx;
    for (i = 0; i < ice->nlocal; i++)
        if (ice->local[i].type == c->type && rtc_addr_eq(&ice->local[i].addr, &c->addr)
            && rtc_addr_eq(&ice->local[i].base, &c->base))
            return -1;
    if (ice->nlocal < RTC_ICE_MAX_LOCAL)
        idx = ice->nlocal++;
    else {
        for (idx = 0; idx < ice->nlocal; idx++) {
            if (!ice->local[idx].off)
                continue;
            for (i = 0; i < ice->npairs && ice->pairs[i].l != idx; i++)
                ;
            if (i == ice->npairs)
                break;
        }
        if (idx == ice->nlocal)
            return -1;
    }
    ice->local[idx] = *c;
    for (i = 0; i < ice->nremote; i++)
        if (ice->remote[i].type != RTC_CAND_PRFLX)
            rtc_ice_pair_add(ice, idx, i);
    return idx;
}

static int rtc_ice_add_remote_ex(rtc_ice* ice, const rtc_cand* c, int only_l)
{
    int i, idx;
    for (i = 0; i < ice->nremote; i++) {
        if (rtc_addr_eq(&ice->remote[i].addr, &c->addr) && ice->remote[i].comp == c->comp) {
            if (ice->remote[i].type == RTC_CAND_PRFLX && c->type != RTC_CAND_PRFLX) {
                int l;
                ice->remote[i].type = c->type;
                ice->remote[i].prio = c->prio;
                ice->remote[i].found = c->found;
                ice->remote[i].rel = c->rel;
                rtc_ice_reprio(ice);
                for (l = 0; l < ice->nlocal; l++)
                    if (rtc_ice_pair_find(ice, l, i) < 0)
                        rtc_ice_pair_add(ice, l, i);
            }
            return i;
        }
    }
    if (ice->nremote >= RTC_ICE_MAX_REMOTE)
        return -1;
    idx = ice->nremote++;
    ice->remote[idx] = *c;
    for (i = 0; i < ice->nlocal; i++)
        if (only_l < 0 || i == only_l)
            rtc_ice_pair_add(ice, i, idx);
    if (ice->started && ice->state == RTC_ICE_NEW)
        rtc_ice_set_state(ice, RTC_ICE_CHECKING);
    return idx;
}

int rtc_ice_add_remote(rtc_ice* ice, const rtc_cand* c)
{
    return rtc_ice_add_remote_ex(ice, c, -1);
}

int rtc_ice_find_local(const rtc_ice* ice, int sock, int relay)
{
    int i, found = -1;
    for (i = 0; i < ice->nlocal; i++) {
        const rtc_cand* c = &ice->local[i];
        if (relay >= 0) {
            if (c->type == RTC_CAND_RELAY && c->relay == relay) {
                if (!c->off)
                    return i;
                if (found < 0)
                    found = i;
            }
        } else if (c->type == RTC_CAND_HOST && c->sock == sock)
            return i;
    }
    return found;
}

int rtc_ice_peer_ok(const rtc_ice* ice, int local, const rtc_addr* from)
{
    int i;
    if (ice->state == RTC_ICE_CLOSED || ice->state == RTC_ICE_FAILED)
        return 0;
    for (i = 0; i < ice->npairs; i++) {
        const rtc_pair* p = &ice->pairs[i];
        if ((p->flags & (RTC_PF_VALID | RTC_PF_AUTH_RX)) && (local < 0 || p->l == local)
            && rtc_addr_eq(&ice->remote[p->r].addr, from))
            return 1;
    }
    return (ice->have_prev || ice->prev_rx) && rtc_addr_eq(&ice->prev.r.addr, from);
}

void rtc_ice_note_rx(rtc_ice* ice, const rtc_addr* from)
{
    if (ice->prev_rx && ice->selected >= 0 && rtc_addr_eq(&ice->remote[ice->pairs[ice->selected].r].addr, from)
        && !rtc_addr_eq(&ice->prev.r.addr, from))
        ice->prev_rx = 0;
}

void rtc_ice_start(rtc_ice* ice, uint64_t now)
{
    if (ice->state == RTC_ICE_CLOSED)
        return;
    if (!ice->started) {
        ice->started = 1;
        ice->started_us = now;
        ice->next_ta_us = now;
    }
    if (ice->state == RTC_ICE_NEW && ice->nremote > 0)
        rtc_ice_set_state(ice, RTC_ICE_CHECKING);
}

static void rtc_ice_trigger(rtc_ice* ice, int pi)
{
    int i;
    for (i = 0; i < ice->ntrig; i++)
        if (ice->trig[i] == pi)
            return;
    if (ice->ntrig < RTC_ICE_MAX_PAIRS)
        ice->trig[ice->ntrig++] = (uint8_t)pi;
    ice->pairs[pi].flags |= RTC_PF_TRIGGERED;
}

static size_t rtc_ice_build_req(uint8_t* buf, size_t cap, const uint8_t* tid, const char* ruf, const char* luf, const char* rpwd,
    uint32_t prio, int controlling, uint64_t tiebreaker, int use_cand)
{
    char user[2 * RTC_ICE_CRED_MAX + 2];
    size_t ul = strlen(ruf), ll = strlen(luf);
    rtc_stun_wr w;
    memcpy(user, ruf, ul);
    user[ul] = ':';
    memcpy(user + ul + 1, luf, ll);
    rtc_stun_wr_init(&w, buf, cap, rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), tid);
    rtc_stun_wr_attr(&w, RTC_SA_USERNAME, user, ul + 1 + ll);
    rtc_stun_wr_u32(&w, RTC_SA_PRIORITY, prio);
    rtc_stun_wr_u64(&w, controlling ? RTC_SA_ICE_CONTROLLING : RTC_SA_ICE_CONTROLLED, tiebreaker);
    if (use_cand)
        rtc_stun_wr_attr(&w, RTC_SA_USE_CANDIDATE, NULL, 0);
    rtc_stun_wr_integrity(&w, (const uint8_t*)rpwd, strlen(rpwd));
    rtc_stun_wr_fingerprint(&w);
    return rtc_stun_wr_done(&w);
}

static void rtc_ice_send_check(rtc_ice* ice, int pi, uint64_t now, int consent, int fresh)
{
    rtc_pair* p = &ice->pairs[pi];
    const rtc_cand* lc = &ice->local[p->l];
    const rtc_cand* rc = &ice->remote[p->r];
    uint8_t buf[600];
    size_t n;
    if (!fresh && !(p->flags & RTC_PF_SENT_CTL) != !ice->controlling)
        fresh = 1;
    if (fresh) {
        memcpy(p->tid2, p->tid, 12);
        rtc_rng_bytes(ice->rng, p->tid, 12);
    }
    n = rtc_ice_build_req(buf, sizeof(buf), p->tid, ice->rufrag, ice->lufrag, ice->rpwd,
        rtc_ice_cand_prio(RTC_CAND_PRFLX, (lc->prio >> 8) & 0xFFFF, lc->comp), ice->controlling, ice->tiebreaker,
        ice->controlling && (p->flags & RTC_PF_USE_CAND));
    if (!n)
        return;
    if (consent)
        p->flags |= RTC_PF_CONSENT;
    else
        p->flags &= (uint8_t)~RTC_PF_CONSENT;
    if (ice->controlling)
        p->flags |= RTC_PF_SENT_CTL;
    else
        p->flags &= (uint8_t)~RTC_PF_SENT_CTL;
    p->sent_us = now;
    p->req_tx++;
    ice->send(ice->ud, lc, &rc->addr, buf, n);
}

static void rtc_ice_send_prev_consent(rtc_ice* ice)
{
    rtc_ice_prev* v = &ice->prev;
    uint8_t buf[600];
    size_t n;
    memcpy(v->tid2, v->tid, 12);
    rtc_rng_bytes(ice->rng, v->tid, 12);
    n = rtc_ice_build_req(buf, sizeof(buf), v->tid, v->rufrag, v->lufrag, v->rpwd,
        rtc_ice_cand_prio(RTC_CAND_PRFLX, (v->l.prio >> 8) & 0xFFFF, v->l.comp), v->controlling, ice->tiebreaker, 0);
    if (n)
        ice->send(ice->ud, &v->l, &v->r.addr, buf, n);
}

static void rtc_ice_start_check(rtc_ice* ice, int pi, uint64_t now)
{
    rtc_pair* p = &ice->pairs[pi];
    int i, active = 0;
    for (i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].state == RTC_PAIR_WAITING || ice->pairs[i].state == RTC_PAIR_INPROGRESS)
            active++;
    p->state = RTC_PAIR_INPROGRESS;
    p->tries = 1;
    p->rto_us = (uint32_t)(RTC_ICE_TA_US * (uint32_t)active);
    if (p->rto_us < 500000)
        p->rto_us = 500000;
    p->next_us = now + p->rto_us;
    rtc_ice_send_check(ice, pi, now, 0, 1);
}

static void rtc_ice_prune(rtc_ice* ice, int pi)
{
    int i;
    for (i = 0; i < ice->npairs; i++) {
        rtc_pair* q = &ice->pairs[i];
        if (i == pi)
            continue;
        if (q->state == RTC_PAIR_FROZEN || q->state == RTC_PAIR_WAITING
            || (q->state == RTC_PAIR_INPROGRESS && q->prio < ice->pairs[pi].prio))
            q->state = RTC_PAIR_FAILED;
    }
    ice->ntrig = 0;
}

static void rtc_ice_select(rtc_ice* ice, int pi, uint64_t now)
{
    if (ice->selected >= 0 && ice->pairs[ice->selected].prio >= ice->pairs[pi].prio
        && (ice->pairs[ice->selected].flags & RTC_PF_NOMINATED))
        return;
    ice->selected = pi;
    ice->prev_rx = ice->have_prev;
    ice->have_prev = 0;
    ice->last_consent_us = now;
    ice->consent_next_us = now + 4000000 + (rtc_rng_u32(ice->rng) % 2000000);
    if (ice->pairs[pi].flags & RTC_PF_NOMINATED)
        rtc_ice_prune(ice, pi);
    if (ice->ev)
        ice->ev(ice->ud, RTC_ICE_EV_SELECTED);
    rtc_ice_set_state(ice, RTC_ICE_CONNECTED);
}

static void rtc_ice_unfreeze_found(rtc_ice* ice, uint32_t lf, uint32_t rf)
{
    int i;
    for (i = 0; i < ice->npairs; i++) {
        rtc_pair* p = &ice->pairs[i];
        if (p->state == RTC_PAIR_FROZEN && ice->local[p->l].found == lf && ice->remote[p->r].found == rf)
            p->state = RTC_PAIR_WAITING;
    }
}

static const uint16_t rtc_ice_known[] = { RTC_SA_MAPPED_ADDRESS, RTC_SA_USERNAME, RTC_SA_MESSAGE_INTEGRITY, RTC_SA_ERROR_CODE,
    RTC_SA_UNKNOWN_ATTRIBUTES, RTC_SA_REALM, RTC_SA_NONCE, RTC_SA_MESSAGE_INTEGRITY_SHA256, RTC_SA_XOR_MAPPED_ADDRESS, RTC_SA_PRIORITY,
    RTC_SA_USE_CANDIDATE };

static int rtc_ice_err_budget(rtc_ice* ice, uint64_t now)
{
    if (now - ice->err_refill_us >= 1000000u || ice->err_refill_us > now) {
        ice->err_refill_us = now;
        ice->err_tokens = 0;
    }
    if (ice->err_tokens >= RTC_ICE_ERR_BURST)
        return 0;
    ice->err_tokens++;
    return 1;
}

static void rtc_ice_respond(rtc_ice* ice, const rtc_cand* lc, const rtc_addr* to, const rtc_stun_msg* m, int err, const char* pwd)
{
    uint8_t buf[256];
    uint8_t ua[2 * RTC_STUN_MAX_ATTR];
    uint16_t unk[RTC_STUN_MAX_ATTR];
    rtc_stun_wr w;
    size_t n;
    int nu = 0, i;
    rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_BINDING, err ? RTC_STUN_ERROR : RTC_STUN_SUCCESS), m->tid);
    if (err)
        rtc_stun_wr_error(&w, err, err == 487 ? "Role Conflict" : err == 420 ? "Unknown Attribute" : "");
    if (err == 420) {
        nu = rtc_stun_unknown_required(m, rtc_ice_known, (int)(sizeof(rtc_ice_known) / sizeof(rtc_ice_known[0])), unk, RTC_STUN_MAX_ATTR);
        for (i = 0; i < nu && i < RTC_STUN_MAX_ATTR; i++)
            rtc_wr16(ua + 2 * i, unk[i]);
        rtc_stun_wr_attr(&w, RTC_SA_UNKNOWN_ATTRIBUTES, ua, 2 * (size_t)(nu < RTC_STUN_MAX_ATTR ? nu : RTC_STUN_MAX_ATTR));
    } else if (!err)
        rtc_stun_wr_addr(&w, RTC_SA_XOR_MAPPED_ADDRESS, to);
    if (pwd)
        rtc_stun_wr_integrity(&w, (const uint8_t*)pwd, strlen(pwd));
    rtc_stun_wr_fingerprint(&w);
    n = rtc_stun_wr_done(&w);
    if (n)
        ice->send(ice->ud, lc, to, buf, n);
}

static int rtc_ice_user_is(const rtc_stun_msg* m, const rtc_stun_attr* ua, const char* lufrag)
{
    size_t ll = strlen(lufrag);
    return ll && ua->len > ll && !memcmp(m->buf + ua->off, lufrag, ll) && m->buf[ua->off + ll] == ':';
}

static void rtc_ice_new_tiebreaker(rtc_ice* ice)
{
    ice->tiebreaker = rtc_rng_next(ice->rng);
}

static void rtc_ice_on_request(rtc_ice* ice, int l, const rtc_addr* from, const rtc_stun_msg* m, uint64_t now)
{
    const rtc_stun_attr* ua = rtc_stun_find(m, RTC_SA_USERNAME);
    const rtc_cand* lc = &ice->local[l];
    uint64_t tb;
    uint32_t prio = 0;
    int r, pi, err = 0;
    if (ice->state == RTC_ICE_FAILED)
        return;
    if (!ua || m->mi < 0) {
        if (rtc_ice_err_budget(ice, now))
            rtc_ice_respond(ice, lc, from, m, 400, NULL);
        return;
    }
    if (!rtc_ice_user_is(m, ua, ice->lufrag) || !rtc_stun_check_integrity(m, (const uint8_t*)ice->lpwd, strlen(ice->lpwd))) {
        if (ice->have_prev && rtc_ice_user_is(m, ua, ice->prev.lufrag)
            && rtc_stun_check_integrity(m, (const uint8_t*)ice->prev.lpwd, strlen(ice->prev.lpwd))) {
            if (rtc_addr_eq(from, &ice->prev.r.addr))
                rtc_ice_respond(ice, lc, from, m, 0, ice->prev.lpwd);
            return;
        }
        if (ice->stage_ufrag[0] && rtc_ice_user_is(m, ua, ice->stage_ufrag)
            && rtc_stun_check_integrity(m, (const uint8_t*)ice->stage_pwd, strlen(ice->stage_pwd))) {
            rtc_ice_respond(ice, lc, from, m, 0, ice->stage_pwd);
            return;
        }
        if (rtc_ice_err_budget(ice, now))
            rtc_ice_respond(ice, lc, from, m, 401, NULL);
        return;
    }
    if (rtc_stun_unknown_required(m, rtc_ice_known, (int)(sizeof(rtc_ice_known) / sizeof(rtc_ice_known[0])), NULL, 0)) {
        rtc_ice_respond(ice, lc, from, m, 420, ice->lpwd);
        return;
    }
    if (rtc_stun_get_u32(m, RTC_SA_PRIORITY, &prio) < 0) {
        rtc_ice_respond(ice, lc, from, m, 400, ice->lpwd);
        return;
    }
    if (ice->controlling && rtc_stun_get_u64(m, RTC_SA_ICE_CONTROLLING, &tb) == 0) {
        if (ice->tiebreaker >= tb)
            err = 487;
        else
            rtc_ice_set_role(ice, 0);
    } else if (!ice->controlling && rtc_stun_get_u64(m, RTC_SA_ICE_CONTROLLED, &tb) == 0) {
        if (ice->tiebreaker >= tb)
            rtc_ice_set_role(ice, 1);
        else
            err = 487;
    }
    if (err) {
        rtc_ice_respond(ice, lc, from, m, err, ice->lpwd);
        return;
    }
    rtc_ice_respond(ice, lc, from, m, 0, ice->lpwd);
    if (ice->state == RTC_ICE_CLOSED)
        return;
    for (r = 0; r < ice->nremote; r++)
        if (rtc_addr_eq(&ice->remote[r].addr, from))
            break;
    if (r == ice->nremote) {
        rtc_cand c;
        memset(&c, 0, sizeof(c));
        c.addr = *from;
        c.base = *from;
        c.prio = prio;
        c.type = RTC_CAND_PRFLX;
        c.comp = 1;
        c.found = rtc_rng_u32(ice->rng) & 0x7FFFFFFF;
        r = rtc_ice_add_remote_ex(ice, &c, l);
        if (r < 0)
            return;
    }
    pi = rtc_ice_pair_find(ice, l, r);
    if (pi < 0)
        pi = rtc_ice_pair_add(ice, l, r);
    if (pi < 0)
        return;
    ice->pairs[pi].flags |= RTC_PF_AUTH_RX;
    ice->pairs[pi].req_rx++;
    ice->pairs[pi].rsp_tx++;
    ice->pairs[pi].last_rx_us = now;
    if (!ice->controlling && rtc_stun_find(m, RTC_SA_USE_CANDIDATE)) {
        if (ice->pairs[pi].flags & RTC_PF_VALID) {
            ice->pairs[pi].flags |= RTC_PF_NOMINATED;
            rtc_ice_select(ice, pi, now);
        } else
            ice->pairs[pi].flags |= RTC_PF_NOM_ON_OK;
    }
    if (ice->selected >= 0 && (ice->pairs[ice->selected].flags & RTC_PF_NOMINATED) && pi != ice->selected
        && !(ice->pairs[pi].flags & RTC_PF_NOM_ON_OK))
        return;
    switch (ice->pairs[pi].state) {
    case RTC_PAIR_FROZEN:
    case RTC_PAIR_WAITING:
    case RTC_PAIR_FAILED:
        ice->pairs[pi].state = RTC_PAIR_WAITING;
        rtc_ice_trigger(ice, pi);
        break;
    case RTC_PAIR_INPROGRESS:
    case RTC_PAIR_SUCCEEDED:
    default:
        break;
    }
    if (ice->started && ice->next_ta_us > now)
        ice->next_ta_us = now;
}

static void rtc_ice_fail_selected(rtc_ice* ice)
{
    if (ice->selected >= 0)
        ice->pairs[ice->selected].state = RTC_PAIR_FAILED;
    ice->selected = -1;
    ice->have_prev = 0;
    rtc_ice_set_state(ice, RTC_ICE_FAILED);
}

static int rtc_tid_is(const uint8_t* a, const uint8_t* tid)
{
    static const uint8_t zero[12] = { 0 };
    return memcmp(a, zero, 12) && !memcmp(a, tid, 12);
}

static int rtc_ice_prev_lost(rtc_ice* ice)
{
    if (ice->rpwd[0]) {
        ice->have_prev = 0;
        return 0;
    }
    rtc_ice_fail_selected(ice);
    return 1;
}

static void rtc_ice_on_prev_response(rtc_ice* ice, const rtc_addr* from, const rtc_stun_msg* m, uint64_t now)
{
    rtc_ice_prev* v = &ice->prev;
    if (!rtc_stun_check_integrity(m, (const uint8_t*)v->rpwd, strlen(v->rpwd)) || !rtc_addr_eq(from, &v->r.addr))
        return;
    if (rtc_stun_class(m->type) == RTC_STUN_ERROR) {
        if (rtc_stun_error_code(m) == 403 && ice->selected < 0)
            rtc_ice_prev_lost(ice);
        return;
    }
    v->last_consent_us = now;
    if (ice->state == RTC_ICE_DISCONNECTED)
        rtc_ice_set_state(ice, RTC_ICE_CONNECTED);
}

static void rtc_ice_on_response(rtc_ice* ice, int local, const rtc_addr* from, const rtc_stun_msg* m, uint64_t now)
{
    int pi, consent, cur;
    rtc_pair* p;
    for (pi = 0; pi < ice->npairs; pi++)
        if (rtc_tid_is(ice->pairs[pi].tid, m->tid) || rtc_tid_is(ice->pairs[pi].tid2, m->tid))
            break;
    if (pi == ice->npairs) {
        if (ice->have_prev && (rtc_tid_is(ice->prev.tid, m->tid) || rtc_tid_is(ice->prev.tid2, m->tid)))
            rtc_ice_on_prev_response(ice, from, m, now);
        return;
    }
    p = &ice->pairs[pi];
    if (!rtc_stun_check_integrity(m, (const uint8_t*)ice->rpwd, strlen(ice->rpwd)))
        return;
    if (rtc_stun_class(m->type) == RTC_STUN_SUCCESS
        && rtc_stun_unknown_required(m, rtc_ice_known, (int)(sizeof(rtc_ice_known) / sizeof(rtc_ice_known[0])), NULL, 0))
        return;
    consent = (p->flags & RTC_PF_CONSENT) != 0;
    cur = rtc_tid_is(p->tid, m->tid);
    if (cur)
        memset(p->tid, 0, 12);
    else
        memset(p->tid2, 0, 12);
    if (!rtc_addr_eq(from, &ice->remote[p->r].addr) || (local >= 0 && local != p->l)) {
        if (!consent)
            p->state = RTC_PAIR_FAILED;
        return;
    }
    p->rsp_rx++;
    p->last_rx_us = now;
    if (rtc_stun_class(m->type) == RTC_STUN_ERROR) {
        int code = rtc_stun_error_code(m);
        if (code == 487 && !consent && !cur)
            return;
        if (code == 487 && !consent) {
            rtc_ice_set_role(ice, !(p->flags & RTC_PF_SENT_CTL));
            rtc_ice_new_tiebreaker(ice);
            p->state = RTC_PAIR_WAITING;
            rtc_ice_trigger(ice, pi);
        } else if (code == 403 && consent && pi == ice->selected) {
            rtc_ice_fail_selected(ice);
        } else if (!consent)
            p->state = RTC_PAIR_FAILED;
        return;
    }
    p->rtt_us = (uint32_t)(now - p->sent_us);
    p->rtt_total_us += p->rtt_us;
    if (consent) {
        if (pi == ice->selected) {
            ice->last_consent_us = now;
            if (ice->state == RTC_ICE_DISCONNECTED)
                rtc_ice_set_state(ice, RTC_ICE_CONNECTED);
        }
        return;
    }
    p->state = RTC_PAIR_SUCCEEDED;
    p->flags |= RTC_PF_VALID;
    if (!ice->first_valid_us)
        ice->first_valid_us = now;
    rtc_ice_unfreeze_found(ice, ice->local[p->l].found, ice->remote[p->r].found);
    if ((ice->controlling && (p->flags & RTC_PF_USE_CAND)) || (!ice->controlling && (p->flags & RTC_PF_NOM_ON_OK))) {
        p->flags |= RTC_PF_NOMINATED;
        rtc_ice_select(ice, pi, now);
    }
}

int rtc_ice_input(rtc_ice* ice, int local, const rtc_addr* from, const rtc_stun_msg* m, uint64_t now)
{
    uint16_t cls;
    if (ice->state == RTC_ICE_CLOSED || rtc_stun_method(m->type) != RTC_STUN_BINDING)
        return 0;
    if (m->fp < 0 || !rtc_stun_check_fingerprint(m))
        return 1;
    cls = rtc_stun_class(m->type);
    if (cls == RTC_STUN_REQUEST) {
        if (local < 0 || local >= ice->nlocal)
            return 1;
        rtc_ice_on_request(ice, local, from, m, now);
    } else if (cls == RTC_STUN_SUCCESS || cls == RTC_STUN_ERROR)
        rtc_ice_on_response(ice, local, from, m, now);
    return 1;
}

static int rtc_ice_pick_ordinary(rtc_ice* ice)
{
    int i, best = -1;
    for (i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].state == RTC_PAIR_WAITING && (best < 0 || ice->pairs[i].prio > ice->pairs[best].prio))
            best = i;
    if (best >= 0)
        return best;
    for (i = 0; i < ice->npairs; i++) {
        int j, busy = 0;
        rtc_pair* p = &ice->pairs[i];
        if (p->state != RTC_PAIR_FROZEN)
            continue;
        for (j = 0; j < ice->npairs; j++) {
            rtc_pair* q = &ice->pairs[j];
            if ((q->state == RTC_PAIR_WAITING || q->state == RTC_PAIR_INPROGRESS)
                && ice->local[q->l].found == ice->local[p->l].found && ice->remote[q->r].found == ice->remote[p->r].found)
                busy = 1;
        }
        if (!busy && (best < 0 || p->prio > ice->pairs[best].prio))
            best = i;
    }
    return best;
}

static void rtc_ice_nominate(rtc_ice* ice, uint64_t now)
{
    int i, best = -1, pending_higher = 0;
    if (!ice->controlling || ice->nominating >= 0 || ice->selected >= 0)
        return;
    for (i = 0; i < ice->npairs; i++)
        if ((ice->pairs[i].flags & RTC_PF_VALID) && (best < 0 || ice->pairs[i].prio > ice->pairs[best].prio))
            best = i;
    if (best < 0)
        return;
    for (i = 0; i < ice->npairs; i++) {
        int s = ice->pairs[i].state;
        if (ice->pairs[i].prio > ice->pairs[best].prio
            && (s == RTC_PAIR_FROZEN || s == RTC_PAIR_WAITING || s == RTC_PAIR_INPROGRESS))
            pending_higher = 1;
    }
    if (pending_higher && now - ice->first_valid_us < 1000000)
        return;
    ice->nominating = best;
    ice->pairs[best].flags |= RTC_PF_USE_CAND;
    ice->pairs[best].state = RTC_PAIR_WAITING;
    rtc_ice_trigger(ice, best);
}

static void rtc_ice_check_done(rtc_ice* ice)
{
    int i, live = 0;
    if (ice->state == RTC_ICE_CLOSED || ice->state == RTC_ICE_FAILED)
        return;
    for (i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].state != RTC_PAIR_FAILED)
            live++;
    if (ice->selected < 0 && !ice->have_prev && live == 0 && ice->remote_done && ice->local_done)
        rtc_ice_set_state(ice, RTC_ICE_FAILED);
    if (ice->state == RTC_ICE_CONNECTED && ice->remote_done && ice->local_done) {
        int busy = 0;
        for (i = 0; i < ice->npairs; i++)
            if (ice->pairs[i].state == RTC_PAIR_WAITING || ice->pairs[i].state == RTC_PAIR_INPROGRESS)
                busy = 1;
        if (!busy)
            rtc_ice_set_state(ice, RTC_ICE_COMPLETED);
    }
}

static uint64_t rtc_ice_consent_tick(rtc_ice* ice, uint64_t now, uint64_t* last, uint64_t* due, int prev, uint64_t next)
{
    if (now >= *due) {
        if (prev)
            rtc_ice_send_prev_consent(ice);
        else
            rtc_ice_send_check(ice, ice->selected, now, 1, 1);
        *due = now + 4000000 + (rtc_rng_u32(ice->rng) % 2000000);
    }
    if (now - *last >= RTC_ICE_CONSENT_EXPIRE_US) {
        if (prev)
            return rtc_ice_prev_lost(ice) ? 0 : next;
        rtc_ice_fail_selected(ice);
        return 0;
    }
    if (now - *last >= RTC_ICE_DISCONNECT_US) {
        if (ice->state == RTC_ICE_CONNECTED || ice->state == RTC_ICE_COMPLETED || ice->state == RTC_ICE_CHECKING)
            rtc_ice_set_state(ice, RTC_ICE_DISCONNECTED);
        if (*last + RTC_ICE_CONSENT_EXPIRE_US < next)
            next = *last + RTC_ICE_CONSENT_EXPIRE_US;
    } else if (*last + RTC_ICE_DISCONNECT_US < next)
        next = *last + RTC_ICE_DISCONNECT_US;
    if (*due < next)
        next = *due;
    return next;
}

uint64_t rtc_ice_tick(rtc_ice* ice, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    int i, work = 0;
    if (ice->state == RTC_ICE_CLOSED || ice->state == RTC_ICE_FAILED || !ice->started)
        return next;
    if (ice->have_prev && ice->selected < 0) {
        next = rtc_ice_consent_tick(ice, now, &ice->prev.last_consent_us, &ice->prev.consent_next_us, 1, next);
        if (!next)
            return UINT64_MAX;
    }
    if (!ice->rpwd[0])
        return next;
    for (i = 0; i < ice->npairs; i++) {
        rtc_pair* p = &ice->pairs[i];
        if (p->state != RTC_PAIR_INPROGRESS)
            continue;
        if (now >= p->next_us) {
            if (p->tries >= RTC_ICE_RC) {
                p->state = RTC_PAIR_FAILED;
                if (ice->nominating == i)
                    ice->nominating = -1;
                continue;
            }
            p->tries++;
            p->rto_us = p->rto_us * 2 > 8000000 ? 8000000 : p->rto_us * 2;
            p->next_us = now + p->rto_us;
            rtc_ice_send_check(ice, i, now, 0, 0);
        }
        if (p->next_us < next)
            next = p->next_us;
    }
    rtc_ice_nominate(ice, now);
    if (now >= ice->next_ta_us) {
        int pi = -1;
        while (ice->ntrig > 0 && pi < 0) {
            int c = ice->trig[0];
            memmove(ice->trig, ice->trig + 1, (size_t)(ice->ntrig - 1));
            ice->ntrig--;
            ice->pairs[c].flags &= (uint8_t)~RTC_PF_TRIGGERED;
            if (ice->pairs[c].state == RTC_PAIR_WAITING)
                pi = c;
        }
        if (pi < 0 && !(ice->selected >= 0 && (ice->pairs[ice->selected].flags & RTC_PF_NOMINATED)))
            pi = rtc_ice_pick_ordinary(ice);
        if (pi >= 0) {
            rtc_ice_start_check(ice, pi, now);
            ice->next_ta_us = now + RTC_ICE_TA_US;
            if (ice->pairs[pi].next_us < next)
                next = ice->pairs[pi].next_us;
        }
    }
    for (i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].state == RTC_PAIR_WAITING || ice->pairs[i].state == RTC_PAIR_FROZEN)
            work = 1;
    if (work || ice->ntrig) {
        if (ice->next_ta_us < next)
            next = ice->next_ta_us;
    }
    if (ice->controlling && ice->selected < 0 && ice->nominating < 0 && ice->first_valid_us) {
        uint64_t t = ice->first_valid_us + 1000000;
        if (t < next)
            next = t > now ? t : now + 1000;
    }
    if (ice->selected >= 0) {
        next = rtc_ice_consent_tick(ice, now, &ice->last_consent_us, &ice->consent_next_us, 0, next);
        if (!next)
            return UINT64_MAX;
    }
    rtc_ice_check_done(ice);
    return next;
}

int rtc_ice_get_selected(const rtc_ice* ice, const rtc_cand** l, const rtc_cand** r)
{
    if (ice->selected >= 0) {
        *l = &ice->local[ice->pairs[ice->selected].l];
        *r = &ice->remote[ice->pairs[ice->selected].r];
        return 1;
    }
    if (ice->have_prev) {
        *l = &ice->prev.l;
        *r = &ice->prev.r;
        return 1;
    }
    return 0;
}

void rtc_ice_restart(rtc_ice* ice, const char* ufrag, const char* pwd)
{
    rtc_ice_clear_checks(ice);
    ice->rufrag[0] = 0;
    ice->rpwd[0] = 0;
    ice->local_done = 0;
    ice->stage_ufrag[0] = 0;
    rtc_ice_set_local_creds(ice, ufrag, pwd);
    if (!ice->have_prev && ice->state != RTC_ICE_CLOSED)
        rtc_ice_set_state(ice, RTC_ICE_NEW);
}

void rtc_ice_close(rtc_ice* ice)
{
    ice->selected = -1;
    ice->have_prev = 0;
    ice->prev_rx = 0;
    ice->state = RTC_ICE_CLOSED;
}
