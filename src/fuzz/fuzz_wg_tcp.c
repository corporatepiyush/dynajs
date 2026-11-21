#include "modules/wireguard/wg-tcp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_wg_tcp: oracle failed at line %d\n", line);
    abort();
}

#define FZ(c) \
    do { \
        if (!(c)) \
            fz_die(__LINE__); \
    } while (0)

static wg_ip_t* fz_ip;
static wg_tcp_t* fz_tcp;
static uint64_t fz_ms = 1000;
static volatile size_t fz_sink;
static wg_tconn_t* fz_cl;
static uint16_t fz_lp;

static uint64_t fz_clock(void* ud)
{
    (void)ud;
    return fz_ms;
}

static uint32_t fz_isn(void* ud, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    (void)ud;
    (void)la;
    (void)ra;
    (void)rp;
    return lp == 80 ? 0xfffffff0u : 1000u;
}

static void fz_check_out(const uint8_t* p, size_t n)
{
    wg_ipaddr_t s, d;
    size_t hl;
    FZ(n >= 40 && n <= WG_IP_MTU);
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    if (p[0] >> 4 == 4) {
        hl = 20;
        FZ(!wg_csum_fin(wg_csum_acc(0, p, 20)) && ((size_t)p[2] << 8 | p[3]) == n);
        memcpy(s.a, p + 12, 4);
        memcpy(d.a, p + 16, 4);
    } else {
        FZ(p[0] >> 4 == 6 && ((size_t)p[4] << 8 | p[5]) + 40 == n);
        hl = 40;
        s.v6 = d.v6 = 1;
        memcpy(s.a, p + 8, 16);
        memcpy(d.a, p + 24, 16);
    }
    if (p[hl - (hl == 20 ? 11 : 34)] == WG_PROTO_TCP) {
        FZ(!wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&s, &d, WG_PROTO_TCP, n - hl), p + hl, n - hl)));
        FZ((p[hl + 12] >> 4) * 4u >= 20 && (p[hl + 12] >> 4) * 4u <= n - hl);
    }
}

static int fz_out(void* ud, const uint8_t* p, size_t n)
{
    (void)ud;
    fz_check_out(p, n);
    return 0;
}

static void fz_info(wg_tconn_t* c)
{
    wg_tcp_info_t i;
    if (!c)
        return;
    wg_tcp_info(c, &i);
    FZ(i.state >= WG_TCP_CLOSED && i.state <= WG_TCP_TIME_WAIT);
    FZ(i.nooo <= WG_TCP_OOO_MAX && i.nsb <= WG_TCP_SB_MAX && i.queued <= WG_TCP_SND_MAX);
    FZ(i.snd_nxt - i.snd_una <= WG_TCP_SND_MAX + 2u);
    if (i.state == WG_TCP_ESTABLISHED)
        FZ(i.rto_ms >= WG_TCP_RTO_MIN_MS && i.rto_ms <= WG_TCP_RTO_MAX_MS && i.cwnd > 0 && i.mss > 0);
}

static void fz_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    (void)ud;
    fz_sink += n;
    if (n && d[0] == 'A')
        wg_tcp_abort(c);
    else if (n && d[0] == 'S')
        wg_tcp_shutdown(c);
    else if (n && d[0] == 'P')
        wg_tcp_pause(c, 1);
    else
        wg_tcp_write(c, d, n > 300 ? 300 : n);
}

static void fz_eof(void* ud, wg_tconn_t* c)
{
    (void)ud;
    wg_tcp_shutdown(c);
}

static void fz_open(void* ud, wg_tconn_t* c)
{
    wg_tcp_stats_t st;
    (void)ud;
    wg_tcp_stats(fz_tcp, &st);
    if (st.live > 64)
        wg_tcp_abort(c);
    else
        wg_tcp_write(c, (const uint8_t*)"hello", 5);
}

static void fz_close(void* ud, wg_tconn_t* c, int err)
{
    (void)ud;
    (void)err;
    if (c == fz_cl)
        fz_cl = NULL;
}

static const wg_tcp_cb_t fz_cb = { fz_open, fz_data, fz_eof, NULL, NULL };
static const wg_tcp_cb_t fz_ccb = { fz_open, fz_data, fz_eof, NULL, fz_close };

static void fz_connect(void)
{
    wg_ipaddr_t a, la, ra;
    uint16_t rp;
    memset(&a, 0, sizeof a);
    a.a[0] = 10;
    a.a[3] = 1;
    fz_cl = wg_tcp_connect(fz_tcp, &a, 443, &fz_ccb, NULL);
    if (fz_cl)
        wg_tcp_endpoints(fz_cl, &la, &fz_lp, &ra, &rp);
}

static void fz_setup(void)
{
    wg_ipaddr_t a;
    memset(&a, 0, sizeof a);
    fz_ip = wg_ip_new(fz_out, NULL);
    wg_ip_set_clock(fz_ip, fz_clock, NULL);
    a.a[0] = 10;
    a.a[3] = 2;
    wg_ip_add_addr(fz_ip, &a);
    memset(&a, 0, sizeof a);
    a.v6 = 1;
    a.a[0] = 0xfd;
    a.a[15] = 2;
    wg_ip_add_addr(fz_ip, &a);
    fz_tcp = wg_tcp_new(fz_ip, fz_clock, NULL);
    wg_tcp_set_isn(fz_tcp, fz_isn, NULL);
    wg_tcp_listen(fz_tcp, 0, 80, 8, &fz_cb, NULL);
    wg_tcp_listen(fz_tcp, 1, 80, 8, &fz_cb, NULL);
}

static void fz_one(uint8_t ctl, const uint8_t* p, size_t n)
{
    uint8_t b[WG_IP_MTU], *t;
    wg_ipaddr_t s, d;
    int v6 = ctl & 1;
    size_t hl = v6 ? 40 : 20;
    uint16_t c;
    if (n < 20 || n > sizeof b - hl)
        return;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    s.v6 = d.v6 = v6;
    s.a[0] = d.a[0] = v6 ? 0xfd : 10;
    s.a[v6 ? 15 : 3] = 1;
    d.a[v6 ? 15 : 3] = 2;
    t = b + hl;
    memcpy(t, p, n);
    if (ctl & 2) {
        t[0] = (uint8_t)(ctl & 4 ? 0x01 : 0x04);
        t[1] = (uint8_t)(ctl & 4 ? 0xbb : (0x00 + (ctl >> 5)));
        t[2] = (uint8_t)(ctl & 4 ? fz_lp >> 8 : 0);
        t[3] = (uint8_t)(ctl & 4 ? fz_lp : 80);
    }
    if (ctl & 8) {
        t[12] = (uint8_t)((t[12] & 0xf0) < 0x50 ? 0x50 : t[12] & 0xf0);
        t[16] = t[17] = 0;
        c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(&s, &d, WG_PROTO_TCP, n), t, n));
        memcpy(t + 16, &c, 2);
    }
    memset(b, 0, hl);
    if (v6) {
        b[0] = 0x60;
        b[4] = (uint8_t)(n >> 8);
        b[5] = (uint8_t)n;
        b[6] = WG_PROTO_TCP;
        b[7] = 64;
        memcpy(b + 8, s.a, 16);
        memcpy(b + 24, d.a, 16);
    } else {
        b[0] = 0x45;
        b[2] = (uint8_t)((n + 20) >> 8);
        b[3] = (uint8_t)(n + 20);
        b[8] = 64;
        b[9] = WG_PROTO_TCP;
        memcpy(b + 12, s.a, 4);
        memcpy(b + 16, d.a, 4);
        c = wg_csum_fin(wg_csum_acc(0, b, 20));
        memcpy(b + 10, &c, 2);
    }
    wg_ip_input(fz_ip, b, n + hl);
}

static void m_raw(const uint8_t* data, size_t size)
{
    size_t i = 0;
    if (!fz_ip)
        fz_setup();
    if (!fz_cl)
        fz_connect();
    while (i + 2 <= size) {
        uint8_t ctl = data[i], len = data[i + 1];
        i += 2;
        if ((size_t)len > size - i)
            len = (uint8_t)(size - i);
        fz_one(ctl, data + i, len);
        i += len;
        fz_ms += (uint64_t)(ctl & 0x10 ? 1500 : 3);
        wg_tcp_tick(fz_tcp);
        fz_info(fz_cl);
    }
}

typedef struct {
    int to, bad;
    size_t n;
    uint8_t b[WG_IP_MTU];
} pk_t;

typedef struct {
    wg_ip_t* ip;
    wg_tcp_t* tcp;
    wg_tconn_t* c;
    size_t sent, rn;
    int opened, eof, closed, err, paused, shut;
} lp_t;

enum { QN = 256 };
static pk_t lq[QN];
static unsigned lqn, lh;
#define Q(k) (&lq[(lh + (k)) % QN])
static lp_t LP[2];
static uint64_t lt;

static uint8_t pat(int dir, size_t off)
{
    return (uint8_t)(off * 131u + (off >> 8) * 7u + (unsigned)dir * 89u + 1u);
}

enum { PATN = 65536 };
static uint8_t PAT[2][PATN + 4096];

static uint64_t lclk(void* ud)
{
    (void)ud;
    return lt;
}

static int l_out(void* ud, const uint8_t* p, size_t n)
{
    int dir = (int)(size_t)ud;
    fz_check_out(p, n);
    if (lqn < QN) {
        pk_t* x = Q(lqn);
        x->to = !dir;
        x->bad = 0;
        x->n = n;
        memcpy(x->b, p, n);
        lqn++;
    }
    return 0;
}

static void l_open(void* ud, wg_tconn_t* c)
{
    lp_t* a = (lp_t*)ud;
    a->c = c;
    a->opened = 1;
}

static void l_data(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n)
{
    lp_t* a = (lp_t*)ud;
    int dir = a == &LP[0];
    (void)c;
    FZ(!a->eof);
    FZ(a->rn + n <= LP[dir].sent);
    FZ(!memcmp(d, PAT[dir] + a->rn, n));
    a->rn += n;
}

static void l_eof(void* ud, wg_tconn_t* c)
{
    lp_t* a = (lp_t*)ud;
    (void)c;
    FZ(LP[a == &LP[0]].shut && a->rn == LP[a == &LP[0]].sent);
    a->eof = 1;
}

static void l_close(void* ud, wg_tconn_t* c, int err)
{
    lp_t* a = (lp_t*)ud;
    (void)c;
    a->closed = 1;
    a->err = err ? err : a->err;
    a->c = NULL;
}

static const wg_tcp_cb_t l_cb = { l_open, l_data, l_eof, NULL, l_close };

static void pk_cp(pk_t* d, const pk_t* s)
{
    d->to = s->to;
    d->bad = s->bad;
    d->n = s->n;
    memcpy(d->b, s->b, s->n);
}

static void l_deliver(unsigned k)
{
    static pk_t x;
    pk_cp(&x, Q(k));
    if (k)
        pk_cp(Q(k), Q(0));
    lh = (lh + 1) % QN;
    lqn--;
    wg_ip_input(LP[x.to].ip, x.b, x.n);
}

static void l_write(int i, size_t want)
{
    size_t got;
    lp_t* a = &LP[i];
    if (!a->c || a->shut || a->sent >= PATN)
        return;
    if (want > 4096)
        want = 4096;
    got = wg_tcp_write(a->c, PAT[i] + a->sent, want);
    FZ(got <= want);
    a->sent += got;
}

static void l_tick(uint64_t adv)
{
    lt += adv;
    wg_tcp_tick(LP[0].tcp);
    wg_tcp_tick(LP[1].tcp);
}

static void m_loop(const uint8_t* d, size_t n)
{
    static const uint8_t A[4] = { 10, 0, 0, 1 }, B[4] = { 10, 0, 0, 2 };
    static const uint64_t ADV[4] = { 1, 40, 250, 1100 };
    wg_ipaddr_t x;
    size_t i = 0;
    unsigned guard;
    int k, v6 = n && (d[0] & 1), sack = !n || !(d[0] & 2);
    if (!PAT[1][0]) {
        size_t o;
        for (o = 0; o < sizeof PAT[0]; o++) {
            PAT[0][o] = pat(0, o);
            PAT[1][o] = pat(1, o);
        }
    }
    memset(LP, 0, sizeof LP);
    lqn = lh = 0;
    lt = 5000;
    for (k = 0; k < 2; k++) {
        memset(&x, 0, sizeof x);
        x.v6 = v6;
        if (v6) {
            x.a[0] = 0xfd;
            x.a[15] = (uint8_t)(k + 1);
        } else
            memcpy(x.a, k ? B : A, 4);
        LP[k].ip = wg_ip_new(l_out, (void*)(size_t)k);
        wg_ip_set_clock(LP[k].ip, lclk, NULL);
        wg_ip_add_addr(LP[k].ip, &x);
        LP[k].tcp = wg_tcp_new(LP[k].ip, lclk, NULL);
        wg_tcp_set_options(LP[k].tcp, sack, n > 1 && (d[1] & 1));
    }
    wg_tcp_listen(LP[1].tcp, v6, 80, 4, &l_cb, &LP[1]);
    LP[0].c = wg_tcp_connect(LP[0].tcp, &x, 80, &l_cb, &LP[0]);
    FZ(LP[0].c != NULL);
    i = n > 2 ? 2 : n;
    while (i < n) {
        unsigned op = d[i++] % 12, a = i < n ? d[i++] : 0, b = i < n ? d[i] : 0;
        if (op <= 1)
            l_write((int)op, (size_t)a * (b % 4 == 0 ? 37 : 3) + 1);
        else if (op <= 4 && lqn)
            l_deliver(op == 2 ? 0 : a % lqn);
        else if (op == 5 && lqn) {
            unsigned j = a % lqn;
            l_deliver(j < lqn ? j : 0);
        } else if (op == 6 && lqn) {
            unsigned j = a % lqn;
            if (lqn < QN) {
                pk_cp(Q(lqn), Q(j));
                lqn++;
            }
        } else if (op == 7 && lqn) {
            unsigned j = a % lqn;
            if (j)
                pk_cp(Q(j), Q(0));
            lh = (lh + 1) % QN;
            lqn--;
        } else if (op == 8 && lqn) {
            pk_t* p = Q(a % lqn);
            size_t at = (size_t)b * 7 % p->n;
            if (!p->bad)
                p->b[at] ^= (uint8_t)(1u << (a % 8));
            p->bad = 1;
            i++;
        } else if (op == 9)
            l_tick(ADV[a % 4]);
        else if (op == 10) {
            lp_t* s = &LP[a & 1];
            if (s->c) {
                s->paused = !s->paused;
                wg_tcp_pause(s->c, s->paused);
            }
        } else if (op == 11 && (a & 3) == 0) {
            lp_t* s = &LP[a >> 2 & 1];
            if (s->c && !s->shut) {
                wg_tcp_info_t in;
                wg_tcp_info(s->c, &in);
                s->shut = 1;
                wg_tcp_shutdown(s->c);
                if (in.state == WG_TCP_SYN_SENT) {
                    s->c = NULL;
                    s->closed = 1;
                }
            }
        }
        fz_info(LP[0].c);
        fz_info(LP[1].c);
    }
    for (k = 0; k < 2; k++)
        if (LP[k].c && LP[k].paused) {
            LP[k].paused = 0;
            wg_tcp_pause(LP[k].c, 0);
        }
    for (guard = 0; guard < 4000; guard++) {
        uint64_t dl, m = 0;
        while (lqn)
            l_deliver(0);
        if (LP[0].rn == LP[1].sent && LP[1].rn == LP[0].sent && !lqn)
            break;
        if (LP[0].err || LP[1].err || LP[0].closed || LP[1].closed)
            break;
        for (k = 0; k < 2; k++)
            if ((dl = wg_tcp_next_deadline(LP[k].tcp)) && (!m || dl < m))
                m = dl;
        l_tick(m > lt ? m - lt : 200);
    }
    if (!LP[0].err && !LP[1].err && !LP[0].closed && !LP[1].closed && LP[1].opened) {
        FZ(LP[1].rn == LP[0].sent && LP[0].rn == LP[1].sent);
        fz_info(LP[0].c);
        fz_info(LP[1].c);
    }
    for (k = 0; k < 2; k++) {
        wg_tcp_free(LP[k].tcp);
        wg_ip_free(LP[k].ip);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1)
        return 0;
    if (data[0] % 3 == 0)
        m_raw(data + 1, size - 1);
    else
        m_loop(data + 1, size - 1);
    return 0;
}
