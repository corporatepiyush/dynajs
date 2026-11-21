#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"
#include "../../dyna-aio.h"
#include "wg-ip.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>

#define WG_DEV_MTU 1420u
#define WG_DEV_SLOT (WG_DEV_MTU + WG_DATA_MIN + WG_PAD)
#define WG_DEV_INLINE_MAX 256u
#define WG_DEV_STAGE_MAX 128u
#define WG_DEV_SLOT_CACHE 4096u
#define WG_DEV_READ_BURST 256
#define WG_DEV_SOCKBUF (4 << 20)

enum { K_PEER = 1, K_KP = 2 };
enum { J_INIT, J_CONSUME_INIT, J_RESP, J_CONSUME_RESP };

typedef struct wg_slot {
    struct wg_slot* next;
    uint32_t len;
    uint8_t data[WG_DEV_SLOT];
} wg_slot_t;

typedef struct {
    union {
        struct sockaddr sa;
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } u;
    socklen_t len;
} wg_addr_t;

typedef struct wg_dkp {
    uint8_t kind;
    uint8_t dead;
    wg_keypair_t k;
    struct wg_dpeer* peer;
} wg_dkp_t;

struct wg_dpeer {
    uint8_t kind;
    uint8_t dead;
    uint8_t init_busy;
    uint8_t dirty;
    uint8_t pending;
    uint8_t want_init;
    uint32_t refs;
    wg_dev_peer_fn ready;
    void* ready_ud;
    uint32_t hs_gen;
    int hpos;
    wg_peer_t np;
    void* e_pk;
    wg_slots_t slots;
    wg_timers_t t;
    wg_addr_t ep;
    wg_slot_t* stage_head;
    wg_slot_t* stage_tail;
    uint32_t nstage;
    wg_fifo_t txq;
    wg_fifo_t rxq;
    wg_dev_t* dev;
};

typedef struct wg_dbatch {
    wg_batch_t b;
    struct wg_dbatch* free_next;
    wg_dpeer_t* peer;
    wg_dkp_t* kp;
    wg_slot_t* slot[WG_BATCH_MAX];
    wg_addr_t src[WG_BATCH_MAX];
} wg_dbatch_t;

typedef struct {
    int type;
    int rc;
    wg_dev_t* dev;
    wg_dpeer_t* peer;
    uint32_t gen;
    uint32_t gen2;
    uint32_t local_index;
    uint64_t unix_ns;
    wg_sym_t st;
    uint8_t ss[WG_KEY], psk[WG_KEY], rpub[WG_KEY], hash0[WG_KEY], mac1_key[WG_KEY], cookie[WG_MACLEN], spub[WG_KEY];
    uint8_t send[WG_KEY], recv[WG_KEY], ts[12];
    int has_cookie;
    int retry;
    void* e_pk;
    wg_addr_t src;
    uint8_t msg[WG_INIT_LEN];
} wg_hjob_t;

struct wg_dev {
    dyn_aio_t* aio;
    int fd;
    wg_static_t s;
    wg_cookie_gen_t cg;
    wg_idx_t idx;
    wg_pubtab_t pubs;
    wg_aip_t aip;
    wg_dpeer_t** heap;
    uint32_t nheap, cheap;
    wg_dpeer_t** dirty;
    uint32_t ndirty, cdirty;
    wg_slot_t* free_slots;
    uint32_t nfree_slots;
    wg_dbatch_t* free_batches;
    wg_dbatch_t* rxb;
    size_t jobs;
    int closing;
    int verify;
    unsigned hs_pending, load_threshold;
    uint64_t under_load_until;
    wg_dev_rx_fn rx;
    void* ud;
    wg_dev_clock_fn clock;
    void* clock_ud;
    wg_dev_stats_t st;
};

static uint64_t now_ms(wg_dev_t* d)
{
    struct timespec ts;
    if (d->clock)
        return d->clock(d->clock_ud, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t unix_ns(wg_dev_t* d)
{
    struct timespec ts;
    if (d->clock)
        return d->clock(d->clock_ud, 1);
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static unsigned jitter(void)
{
    uint8_t r[2];
    if (wg_random(r, sizeof r) < 0)
        return 0;
    return (unsigned)(r[0] | r[1] << 8) % WG_REKEY_JITTER_MS;
}

static wg_slot_t* slot_get(wg_dev_t* d)
{
    wg_slot_t* s = d->free_slots;
    if (s) {
        d->free_slots = s->next;
        d->nfree_slots--;
    } else
        s = (wg_slot_t*)malloc(sizeof *s);
    if (s)
        s->next = NULL;
    return s;
}

static void slot_put(wg_dev_t* d, wg_slot_t* s)
{
    if (!s)
        return;
    if (d->nfree_slots >= WG_DEV_SLOT_CACHE) {
        free(s);
        return;
    }
    s->next = d->free_slots;
    d->free_slots = s;
    d->nfree_slots++;
}

static wg_dbatch_t* batch_get(wg_dev_t* d)
{
    wg_dbatch_t* b = d->free_batches;
    if (b)
        d->free_batches = b->free_next;
    else if (!(b = (wg_dbatch_t*)malloc(sizeof *b)))
        return NULL;
    memset(&b->b, 0, offsetof(wg_batch_t, pkt));
    b->free_next = NULL;
    b->peer = NULL;
    b->kp = NULL;
    b->b.owner = d;
    return b;
}

static void peer_unref(wg_dpeer_t* p);
static void kp_unref(wg_dkp_t* k);

static void batch_put(wg_dev_t* d, wg_dbatch_t* b)
{
    unsigned i;
    for (i = 0; i < b->b.n; i++)
        slot_put(d, b->slot[i]);
    wg_wipe(b->b.key, WG_KEY);
    kp_unref(b->kp);
    peer_unref(b->peer);
    b->b.n = 0;
    b->free_next = d->free_batches;
    d->free_batches = b;
}

static void kp_unref(wg_dkp_t* k)
{
    if (k && --k->k.refs == 0) {
        wg_wipe(k, sizeof *k);
        free(k);
    }
}

static void kp_drop(void* ud, wg_keypair_t* kp)
{
    wg_dev_t* d = (wg_dev_t*)ud;
    wg_dkp_t* k = (wg_dkp_t*)(void*)((uint8_t*)kp - offsetof(wg_dkp_t, k));
    if (wg_idx_get(&d->idx, kp->local_index) == k)
        wg_idx_del(&d->idx, kp->local_index);
    k->dead = 1;
    kp_unref(k);
}

static void hs_reset(wg_dev_t* d, wg_dpeer_t* p)
{
    if (p->np.hs.state != WG_HS_NONE && wg_idx_get(&d->idx, p->np.hs.local_index) == p)
        wg_idx_del(&d->idx, p->np.hs.local_index);
    wg_wipe(&p->np.hs, sizeof p->np.hs);
    wg_pkey_free(p->e_pk);
    p->e_pk = NULL;
    p->hs_gen++;
}

static void peer_unref(wg_dpeer_t* p)
{
    if (p && --p->refs == 0) {
        wg_wipe(p, sizeof *p);
        free(p);
    }
}

static int heap_less(wg_dev_t* d, uint32_t a, uint32_t b)
{
    return wg_timers_next(&d->heap[a]->t) < wg_timers_next(&d->heap[b]->t);
}

static void heap_swap(wg_dev_t* d, uint32_t a, uint32_t b)
{
    wg_dpeer_t* t = d->heap[a];
    d->heap[a] = d->heap[b];
    d->heap[b] = t;
    d->heap[a]->hpos = (int)a;
    d->heap[b]->hpos = (int)b;
}

static void heap_fix(wg_dev_t* d, uint32_t i)
{
    while (i > 0 && heap_less(d, i, (i - 1) / 2)) {
        heap_swap(d, i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < d->nheap && heap_less(d, l, m))
            m = l;
        if (r < d->nheap && heap_less(d, r, m))
            m = r;
        if (m == i)
            break;
        heap_swap(d, i, m);
        i = m;
    }
}

static void heap_remove(wg_dev_t* d, wg_dpeer_t* p)
{
    uint32_t i = (uint32_t)p->hpos;
    if (p->hpos < 0)
        return;
    p->hpos = -1;
    if (i != --d->nheap) {
        d->heap[i] = d->heap[d->nheap];
        d->heap[i]->hpos = (int)i;
        heap_fix(d, i);
    }
}

static void resched(wg_dev_t* d, wg_dpeer_t* p)
{
    if (p->dead)
        return;
    if (!wg_timers_next(&p->t)) {
        heap_remove(d, p);
        return;
    }
    if (p->hpos < 0) {
        if (d->nheap == d->cheap) {
            uint32_t c = d->cheap ? d->cheap * 2 : 64;
            wg_dpeer_t** h = (wg_dpeer_t**)realloc(d->heap, c * sizeof *h);
            if (!h)
                return;
            d->heap = h;
            d->cheap = c;
        }
        p->hpos = (int)d->nheap;
        d->heap[d->nheap++] = p;
    }
    heap_fix(d, (uint32_t)p->hpos);
}

static void send_to(wg_dev_t* d, wg_dpeer_t* p, const uint8_t* buf, size_t n)
{
    if (p->ep.len && dyn_aio_sendto(d->aio, d->fd, buf, n, &p->ep.u.sa, (unsigned)p->ep.len) < 0)
        d->st.tx_drop++;
}

static void job_submit(wg_dev_t* d, void (*work)(void*), void (*done)(void*), void* arg)
{
    d->jobs++;
    if (dyn_aio_offload_cpu(d->aio, work, done, arg) == 0)
        d->st.jobs_offloaded++;
    else {
        d->st.inline_ops++;
        work(arg);
        done(arg);
    }
}

static void hjob_work(void* arg);
static void hjob_done(void* arg);

static void initiate(wg_dev_t* d, wg_dpeer_t* p, int retry)
{
    uint64_t now = now_ms(d);
    wg_hjob_t* j;
    if (p->pending && !p->dead) {
        p->want_init = 1;
        return;
    }
    if (d->closing || p->dead || p->init_busy || !p->ep.len || !wg_timers_hs_allowed(&p->t, now))
        return;
    if (!(j = (wg_hjob_t*)calloc(1, sizeof *j)))
        return;
    hs_reset(d, p);
    if (wg_idx_alloc(&d->idx, p, &j->local_index) < 0) {
        free(j);
        return;
    }
    p->np.hs.local_index = j->local_index;
    p->np.hs.state = WG_HS_CREATED_INIT;
    p->init_busy = 1;
    p->t.last_hs_sent_ms = now;
    p->t.has_hs_sent = 1;
    j->type = J_INIT;
    j->retry = retry;
    j->dev = d;
    j->peer = p;
    j->gen = p->hs_gen;
    p->refs++;
    j->unix_ns = unix_ns(d);
    memcpy(j->hash0, p->np.hash0, WG_KEY);
    memcpy(j->rpub, p->np.pub, WG_KEY);
    memcpy(j->ss, p->np.ss, WG_KEY);
    job_submit(d, hjob_work, hjob_done, j);
}

static void macs(uint8_t* msg, size_t n, const uint8_t mac1_key[WG_KEY], const uint8_t* cookie)
{
    wg_mac(msg + n - 2 * WG_MACLEN, mac1_key, WG_KEY, msg, n - 2 * WG_MACLEN);
    if (cookie)
        wg_mac(msg + n - WG_MACLEN, cookie, WG_MACLEN, msg, n - WG_MACLEN);
    else
        memset(msg + n - WG_MACLEN, 0, WG_MACLEN);
}

static void hjob_work(void* arg)
{
    wg_hjob_t* j = (wg_hjob_t*)arg;
    const wg_static_t* s = &j->dev->s;
    uint8_t epub[WG_KEY], epriv[WG_KEY];
    j->rc = -1;
    switch (j->type) {
    case J_INIT:
        wg_sym_start_h0(&j->st, j->hash0);
        wg_tai64n(j->ts, j->unix_ns);
        j->e_pk = wg_keygen_pk(epriv, epub);
        memset(j->msg, 0, WG_INIT_LEN);
        if (j->e_pk && wg_noise_init_write(&j->st, j->e_pk, epub, j->rpub, s->pub, j->ss, j->ts, sizeof j->ts, j->msg + 8) == 0) {
            j->msg[0] = WG_MSG_INIT;
            wg_put_le32(j->msg + 4, j->local_index);
            j->rc = 0;
        }
        break;
    case J_CONSUME_INIT:
        wg_sym_start_h0(&j->st, s->hash0);
        j->rc = wg_noise_init_read_s(&j->st, s, j->msg + 8, j->spub);
        break;
    case J_RESP:
        if (wg_noise_init_read_pl(&j->st, j->ss, j->msg + 88, sizeof j->ts + WG_TAG, j->ts) < 0)
            break;
        j->e_pk = wg_keygen_pk(epriv, epub);
        memcpy(j->spub, j->msg + 8, WG_KEY);
        j->gen2 = wg_le32(j->msg + 4);
        memset(j->msg, 0, WG_INIT_LEN);
        j->msg[0] = WG_MSG_RESP;
        wg_put_le32(j->msg + 8, j->gen2);
        if (!j->e_pk || wg_noise_resp_write(&j->st, j->e_pk, epub, j->spub, j->rpub, j->psk, NULL, 0, j->msg + 12) < 0)
            break;
        wg_put_le32(j->msg + 4, j->local_index);
        macs(j->msg, WG_RESP_LEN, j->mac1_key, j->has_cookie ? j->cookie : NULL);
        wg_noise_split(&j->st, j->recv, j->send);
        j->rc = 0;
        break;
    default:
        if (wg_noise_resp_read(&j->st, j->e_pk, s, j->psk, j->msg + 12, WG_KEY + WG_TAG, NULL) == 0) {
            wg_noise_split(&j->st, j->send, j->recv);
            j->rc = 0;
        }
        break;
    }
    wg_wipe(epriv, sizeof epriv);
}

static void flush_peer(wg_dev_t* d, wg_dpeer_t* p);
static void send_keepalive(wg_dev_t* d, wg_dpeer_t* p);

static void set_ep(wg_dev_t* d, wg_dpeer_t* p, const wg_addr_t* a)
{
    if (a->len && (p->ep.len != a->len || memcmp(&p->ep.u, &a->u, a->len) != 0)) {
        if (p->ep.len)
            d->st.roams++;
        p->ep = *a;
    }
}

static wg_dkp_t* kp_new(wg_dev_t* d, wg_dpeer_t* p, const wg_hjob_t* j, uint32_t local, uint32_t remote, int initiator)
{
    wg_dkp_t* k = (wg_dkp_t*)calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->kind = K_KP;
    k->peer = p;
    memcpy(k->k.send, j->send, WG_KEY);
    memcpy(k->k.recv, j->recv, WG_KEY);
    k->k.created_ms = now_ms(d);
    k->k.local_index = local;
    k->k.remote_index = remote;
    k->k.initiator = (uint8_t)initiator;
    k->k.refs = 1;
    if (wg_idx_get(&d->idx, local))
        wg_idx_del(&d->idx, local);
    if (wg_idx_put(&d->idx, local, k) < 0) {
        free(k);
        return NULL;
    }
    wg_slots_begin(&p->slots, &k->k, kp_drop, d);
    wg_timers_session_derived(&p->t, now_ms(d));
    return k;
}

static void on_consumed_init(wg_dev_t* d, wg_hjob_t* j)
{
    wg_dpeer_t* p = (wg_dpeer_t*)wg_pubtab_get(&d->pubs, j->spub);
    uint64_t now = now_ms(d);
    wg_hjob_t* r;
    uint32_t li;
    if (p && p->pending) {
        d->st.hs_drop_pending++;
        return;
    }
    if (d->closing || !p || p->dead || !(r = (wg_hjob_t*)calloc(1, sizeof *r)))
        return;
    if (wg_idx_alloc(&d->idx, p, &li) < 0) {
        free(r);
        return;
    }
    *r = *j;
    r->local_index = li;
    r->type = J_RESP;
    r->peer = p;
    r->gen = p->hs_gen;
    p->refs++;
    memcpy(r->ss, p->np.ss, WG_KEY);
    memcpy(r->psk, p->np.psk, WG_KEY);
    memcpy(r->rpub, p->np.pub, WG_KEY);
    memcpy(r->mac1_key, p->np.mac1_key, WG_KEY);
    r->has_cookie = p->np.has_cookie && now - p->np.cookie_ms < WG_COOKIE_VALID_MS;
    memcpy(r->cookie, p->np.cookie, WG_MACLEN);
    r->e_pk = NULL;
    d->hs_pending++;
    job_submit(d, hjob_work, hjob_done, r);
}

static void hjob_done(void* arg)
{
    wg_hjob_t* j = (wg_hjob_t*)arg;
    wg_dev_t* d = j->dev;
    wg_dpeer_t* p = j->peer;
    uint64_t now = now_ms(d);
    d->jobs--;
    if (j->type != J_INIT)
        d->hs_pending--;
    if (j->rc == 0 && j->type == J_CONSUME_INIT)
        on_consumed_init(d, j);
    else if (j->type == J_INIT) {
        p->init_busy = 0;
        if (j->rc >= 0 && !p->dead && j->gen == p->hs_gen && p->np.hs.state == WG_HS_CREATED_INIT) {
            int cookie = p->np.has_cookie && now - p->np.cookie_ms < WG_COOKIE_VALID_MS;
            macs(j->msg, WG_INIT_LEN, p->np.mac1_key, cookie ? p->np.cookie : NULL);
            memcpy(p->np.last_mac1, j->msg + WG_INIT_LEN - 2 * WG_MACLEN, WG_MACLEN);
            p->np.has_last_mac1 = 1;
            memcpy(p->np.hs.ck, j->st.ck, WG_KEY);
            memcpy(p->np.hs.h, j->st.h, WG_KEY);
            p->e_pk = j->e_pk;
            j->e_pk = NULL;
            send_to(d, p, j->msg, WG_INIT_LEN);
            d->st.hs_init_sent++;
            wg_timers_hs_initiated(&p->t, now, jitter(), j->retry);
            wg_timers_auth_sent(&p->t);
            wg_timers_traversal(&p->t, now);
        } else if (!p->dead && j->gen == p->hs_gen)
            hs_reset(d, p);
    } else if (j->type == J_RESP) {
        int ok = j->rc == 0 && !p->dead && memcmp(j->ts, p->np.greatest_ts, sizeof j->ts) > 0 && !(p->np.has_last_init && now - p->np.last_init_ms < WG_INIT_MIN_GAP_MS);
        if (ok && wg_idx_get(&d->idx, j->local_index) == p) {
            uint32_t remote = wg_le32(j->msg + 8);
            memcpy(p->np.greatest_ts, j->ts, sizeof j->ts);
            p->np.last_init_ms = now;
            p->np.has_last_init = 1;
            if (kp_new(d, p, j, j->local_index, remote, 0)) {
                set_ep(d, p, &j->src);
                memcpy(p->np.last_mac1, j->msg + WG_RESP_LEN - 2 * WG_MACLEN, WG_MACLEN);
                p->np.has_last_mac1 = 1;
                send_to(d, p, j->msg, WG_RESP_LEN);
                d->st.hs_resp_sent++;
                wg_timers_auth_sent(&p->t);
                wg_timers_traversal(&p->t, now);
            }
        } else if (wg_idx_get(&d->idx, j->local_index) == p)
            wg_idx_del(&d->idx, j->local_index);
    } else if (j->type == J_CONSUME_RESP) {
        if (j->rc == 0 && !p->dead && j->gen == p->hs_gen && p->np.hs.state == WG_HS_CREATED_INIT && p->np.hs.local_index == j->local_index) {
            uint32_t local = p->np.hs.local_index;
            p->np.hs.state = WG_HS_NONE;
            wg_pkey_free(p->e_pk);
            p->e_pk = NULL;
            wg_wipe(&p->np.hs, sizeof p->np.hs);
            p->hs_gen++;
            if (kp_new(d, p, j, local, wg_le32(j->msg + 4), 1)) {
                set_ep(d, p, &j->src);
                d->st.hs_done++;
                wg_timers_hs_complete(&p->t, now);
                wg_timers_auth_recv(&p->t);
                if (p->nstage)
                    flush_peer(d, p);
                else
                    send_keepalive(d, p);
            }
        }
    }
    if (p)
        resched(d, p);
    wg_pkey_free(j->e_pk);
    peer_unref(p);
    wg_wipe(j, sizeof *j);
    free(j);
}

static void tx_done(void* arg);
static void rx_done(void* arg);

static size_t inner_len(const uint8_t* ip, size_t n);

static void batch_work(void* arg)
{
    wg_dbatch_t* b = (wg_dbatch_t*)arg;
    wg_dev_t* d = (wg_dev_t*)b->b.owner;
    unsigned i;
    wg_batch_work(&b->b);
    if (b->b.enc || !d->verify)
        return;
    for (i = 0; i < b->b.n; i++) {
        wg_pkt_t* pk = &b->b.pkt[i];
        size_t l = pk->res > 0 ? inner_len(pk->buf + WG_DATA_HDR, (size_t)pk->res) : 0;
        pk->verified = (uint8_t)(l && wg_ip_verify(pk->buf + WG_DATA_HDR, l));
    }
}

static void tx_release(wg_dev_t* d, wg_dpeer_t* p)
{
    wg_batch_t* bb;
    while ((bb = wg_fifo_pop_ready(&p->txq)) != NULL) {
        wg_dbatch_t* b = (wg_dbatch_t*)(void*)bb;
        unsigned i, data = 0, sent = 0;
        uint64_t now = now_ms(d);
        for (i = 0; i < b->b.n; i++)
            if (b->b.pkt[i].res > 0 && !p->dead) {
                send_to(d, p, b->b.pkt[i].buf, (size_t)b->b.pkt[i].res);
                sent++;
                if (b->slot[i]->len)
                    data++;
                else
                    d->st.keepalive_sent++;
            } else
                d->st.tx_drop++;
        d->st.tx_data += data;
        if (sent && !p->dead) {
            if (data)
                wg_timers_data_sent(&p->t, now, jitter());
            wg_timers_auth_sent(&p->t);
            wg_timers_traversal(&p->t, now);
            resched(d, p);
        }
        batch_put(d, b);
    }
}

static void tx_done(void* arg)
{
    wg_dbatch_t* b = (wg_dbatch_t*)arg;
    wg_dev_t* d = (wg_dev_t*)b->b.owner;
    wg_dpeer_t* p = b->peer;
    d->jobs--;
    b->b.done = 1;
    p->refs++;
    tx_release(d, p);
    peer_unref(p);
}

static void flush_peer(wg_dev_t* d, wg_dpeer_t* p)
{
    uint64_t now = now_ms(d);
    wg_keypair_t* kp = p->slots.cur;
    if (!p->nstage || p->dead || d->closing)
        return;
    if (!wg_kp_can_send(kp, now)) {
        initiate(d, p, 0);
        return;
    }
    while (p->stage_head) {
        wg_dbatch_t* b = batch_get(d);
        unsigned i;
        if (!b)
            return;
        b->peer = p;
        p->refs++;
        memcpy(b->b.key, kp->send, WG_KEY);
        b->b.remote_index = kp->remote_index;
        b->b.enc = 1;
        b->b.mtu = (uint16_t)WG_DEV_MTU;
        for (i = 0; i < WG_BATCH_MAX && p->stage_head && kp->send_ctr < WG_REJECT_AFTER_MESSAGES; i++) {
            wg_slot_t* s = p->stage_head;
            p->stage_head = s->next;
            p->nstage--;
            b->slot[i] = s;
            b->b.pkt[i].buf = s->data;
            b->b.pkt[i].len = s->len;
            b->b.pkt[i].cap = WG_DEV_SLOT;
            b->b.pkt[i].ctr = kp->send_ctr++;
        }
        b->b.n = (uint16_t)i;
        if (!p->stage_head)
            p->stage_tail = NULL;
        wg_fifo_push(&p->txq, &b->b);
        if (b->b.n == 1 && b->b.pkt[0].len <= WG_DEV_INLINE_MAX && p->txq.head == &b->b) {
            d->st.inline_ops++;
            wg_batch_work(&b->b);
            b->b.done = 1;
            tx_release(d, p);
        } else
            job_submit(d, batch_work, tx_done, b);
        if (!i)
            break;
    }
    if (wg_kp_fresh_send(kp, now))
        initiate(d, p, 0);
}

static int stage(wg_dev_t* d, wg_dpeer_t* p, const uint8_t* ip, size_t n)
{
    wg_slot_t* s = slot_get(d);
    if (!s)
        return -1;
    s->len = (uint32_t)n;
    if (n)
        memcpy(s->data + WG_DATA_HDR, ip, n);
    if (p->nstage >= WG_DEV_STAGE_MAX) {
        wg_slot_t* o = p->stage_head;
        p->stage_head = o->next;
        p->nstage--;
        slot_put(d, o);
        d->st.tx_drop++;
    }
    if (p->stage_tail)
        p->stage_tail->next = s;
    else
        p->stage_head = s;
    p->stage_tail = s;
    p->nstage++;
    if (!p->dirty) {
        if (d->ndirty == d->cdirty) {
            uint32_t c = d->cdirty ? d->cdirty * 2 : 64;
            wg_dpeer_t** a = (wg_dpeer_t**)realloc(d->dirty, c * sizeof *a);
            if (!a)
                return 0;
            d->dirty = a;
            d->cdirty = c;
        }
        p->dirty = 1;
        p->refs++;
        d->dirty[d->ndirty++] = p;
    }
    return 0;
}

static void send_keepalive(wg_dev_t* d, wg_dpeer_t* p)
{
    if (p->nstage || stage(d, p, NULL, 0) < 0)
        return;
    flush_peer(d, p);
}

void wg_dev_flush(wg_dev_t* d)
{
    uint32_t i;
    for (i = 0; i < d->ndirty; i++) {
        wg_dpeer_t* p = d->dirty[i];
        p->dirty = 0;
        flush_peer(d, p);
        resched(d, p);
        peer_unref(p);
    }
    d->ndirty = 0;
}

static const uint8_t* route_addr(const uint8_t* ip, size_t n, int src, int* v6)
{
    if (n >= 20 && ip[0] >> 4 == 4) {
        *v6 = 0;
        return ip + (src ? 12 : 16);
    }
    if (n >= 40 && ip[0] >> 4 == 6) {
        *v6 = 1;
        return ip + (src ? 8 : 24);
    }
    return NULL;
}

int wg_dev_send(wg_dev_t* d, const uint8_t* ip, size_t n)
{
    int v6;
    const uint8_t* a = route_addr(ip, n, 0, &v6);
    wg_dpeer_t* p = a && n <= WG_DEV_MTU ? (wg_dpeer_t*)wg_aip_lookup(&d->aip, v6, a) : NULL;
    if (!p || stage(d, p, ip, n) < 0) {
        d->st.tx_drop++;
        return -1;
    }
    if (p->nstage >= WG_BATCH_MAX)
        flush_peer(d, p);
    return 0;
}

static size_t inner_len(const uint8_t* ip, size_t n)
{
    size_t l;
    if (n >= 20 && ip[0] >> 4 == 4) {
        l = (size_t)ip[2] << 8 | ip[3];
        return l >= 20 && l <= n ? l : 0;
    }
    if (n >= 40 && ip[0] >> 4 == 6) {
        l = ((size_t)ip[4] << 8 | ip[5]) + 40;
        return l <= n ? l : 0;
    }
    return 0;
}

static void rx_release(wg_dev_t* d, wg_dpeer_t* p)
{
    wg_batch_t* bb;
    while ((bb = wg_fifo_pop_ready(&p->rxq)) != NULL) {
        wg_dbatch_t* b = (wg_dbatch_t*)(void*)bb;
        wg_dkp_t* k = b->kp;
        uint64_t now = now_ms(d);
        unsigned i, ok = 0;
        for (i = 0; i < b->b.n && !p->dead; i++) {
            wg_pkt_t* pk = &b->b.pkt[i];
            const uint8_t* ip = pk->buf + WG_DATA_HDR;
            size_t l;
            int v6;
            const uint8_t* src;
            if (pk->res < 0 || k->dead || !wg_kp_can_recv(&k->k, now) || wg_replay_check(&k->k.replay, pk->ctr) < 0) {
                d->st.rx_drop++;
                continue;
            }
            ok++;
            if (wg_slots_received(&p->slots, &k->k, kp_drop, d)) {
                wg_timers_hs_complete(&p->t, now);
                flush_peer(d, p);
            }
            set_ep(d, p, &b->src[i]);
            if (pk->res == 0) {
                d->st.rx_keepalive++;
                continue;
            }
            wg_timers_data_recv(&p->t, now);
            l = inner_len(ip, (size_t)pk->res);
            src = l ? route_addr(ip, l, 1, &v6) : NULL;
            if (!src || wg_aip_lookup(&d->aip, v6, src) != p) {
                d->st.rx_drop++;
                continue;
            }
            d->st.rx_data++;
            if (pk->verified)
                d->st.csum_verified++;
            if (d->rx)
                d->rx(d->ud, p, ip, l, pk->verified);
        }
        if (ok && !p->dead) {
            wg_timers_auth_recv(&p->t);
            wg_timers_traversal(&p->t, now);
            if (!k->dead && wg_kp_fresh_recv(&k->k, now) && !p->t.last_minute) {
                p->t.last_minute = 1;
                initiate(d, p, 0);
            }
            resched(d, p);
        }
        batch_put(d, b);
    }
}

static void rx_done(void* arg)
{
    wg_dbatch_t* b = (wg_dbatch_t*)arg;
    wg_dev_t* d = (wg_dev_t*)b->b.owner;
    wg_dpeer_t* p = b->peer;
    d->jobs--;
    b->b.done = 1;
    p->refs++;
    rx_release(d, p);
    peer_unref(p);
}

static void rx_submit(wg_dev_t* d)
{
    wg_dbatch_t* b = d->rxb;
    wg_dpeer_t* p;
    if (!b)
        return;
    d->rxb = NULL;
    p = b->peer;
    wg_fifo_push(&p->rxq, &b->b);
    if (b->b.n == 1 && b->b.pkt[0].len <= WG_DEV_INLINE_MAX + WG_DATA_MIN && p->rxq.head == &b->b) {
        d->st.inline_ops++;
        wg_batch_work(&b->b);
        b->b.done = 1;
        rx_release(d, p);
    } else
        job_submit(d, batch_work, rx_done, b);
}

static int rx_data(wg_dev_t* d, wg_slot_t* s, const wg_addr_t* src)
{
    uint32_t rxi;
    uint64_t ctr;
    wg_dkp_t* k;
    wg_dbatch_t* b;
    if (wg_data_parse(s->data, s->len, &rxi, &ctr) < 0 || !(k = (wg_dkp_t*)wg_idx_get(&d->idx, rxi)) || k->kind != K_KP || k->dead || !wg_kp_can_recv(&k->k, now_ms(d)) || wg_replay_peek(&k->k.replay, ctr) < 0)
        return -1;
    if (d->rxb && (d->rxb->kp != k || d->rxb->b.n == WG_BATCH_MAX))
        rx_submit(d);
    if (!d->rxb) {
        if (!(b = batch_get(d)))
            return -1;
        b->kp = k;
        k->k.refs++;
        b->peer = k->peer;
        k->peer->refs++;
        memcpy(b->b.key, k->k.recv, WG_KEY);
        d->rxb = b;
    }
    b = d->rxb;
    b->slot[b->b.n] = s;
    b->src[b->b.n] = *src;
    b->b.pkt[b->b.n].buf = s->data;
    b->b.pkt[b->b.n].len = s->len;
    b->b.pkt[b->b.n].cap = WG_DEV_SLOT;
    b->b.pkt[b->b.n].ctr = ctr;
    b->b.pkt[b->b.n].verified = 0;
    b->b.n++;
    return 0;
}

static wg_peer_t* cookie_lookup(void* ud, uint32_t index)
{
    wg_dpeer_t* p = (wg_dpeer_t*)wg_idx_get(&((wg_dev_t*)ud)->idx, index);
    return p && p->kind == K_PEER ? &p->np : NULL;
}

static void rx_handshake(wg_dev_t* d, const uint8_t* m, size_t n, const wg_addr_t* src)
{
    uint64_t now = now_ms(d);
    uint8_t ip[18], reply[WG_COOKIE_LEN];
    size_t ipn = 0;
    int type = (int)wg_le32(m), rc;
    wg_hjob_t* j;
    wg_dpeer_t* p = NULL;
    if (type == WG_MSG_COOKIE) {
        if (wg_consume_cookie_reply(m, n, now, cookie_lookup, d))
            d->st.cookie_recv++;
        return;
    }
    if (src->u.sa.sa_family == AF_INET) {
        memcpy(ip, &src->u.v4.sin_addr, 4);
        memcpy(ip + 4, &src->u.v4.sin_port, 2);
        ipn = 6;
    } else {
        memcpy(ip, &src->u.v6.sin6_addr, 16);
        memcpy(ip + 16, &src->u.v6.sin6_port, 2);
        ipn = 18;
    }
    if (d->hs_pending >= d->load_threshold)
        d->under_load_until = now + WG_UNDER_LOAD_MS;
    rc = wg_check_macs(&d->s, &d->cg, now, m, n, ip, ipn, now < d->under_load_until);
    if (rc == WG_MAC_BAD || d->hs_pending >= WG_HS_QUEUE)
        return;
    if (rc == WG_MAC_NEED_COOKIE) {
        if (wg_create_cookie_reply(&d->s, &d->cg, now, m, n, ip, ipn, reply) == 0 && dyn_aio_sendto(d->aio, d->fd, reply, sizeof reply, &src->u.sa, (unsigned)src->len) >= 0)
            d->st.cookie_sent++;
        return;
    }
    if (type == WG_MSG_RESP) {
        p = (wg_dpeer_t*)wg_idx_get(&d->idx, wg_le32(m + 8));
        if (!p || p->kind != K_PEER || p->np.hs.state != WG_HS_CREATED_INIT || p->np.hs.local_index != wg_le32(m + 8) || !p->e_pk)
            return;
    }
    if (!(j = (wg_hjob_t*)calloc(1, sizeof *j)))
        return;
    j->dev = d;
    j->src = *src;
    memcpy(j->msg, m, n);
    if (p) {
        j->type = J_CONSUME_RESP;
        j->peer = p;
        p->refs++;
        j->gen = p->hs_gen;
        j->local_index = p->np.hs.local_index;
        memcpy(j->st.ck, p->np.hs.ck, WG_KEY);
        memcpy(j->st.h, p->np.hs.h, WG_KEY);
        memcpy(j->psk, p->np.psk, WG_KEY);
        j->e_pk = wg_pkey_up(p->e_pk);
    } else
        j->type = J_CONSUME_INIT;
    d->hs_pending++;
    job_submit(d, hjob_work, hjob_done, j);
}

static void on_readable(dyn_aio_t* aio, int fd, void* ud)
{
    wg_dev_t* d = (wg_dev_t*)ud;
    int i;
    (void)aio;
    for (i = 0; i < WG_DEV_READ_BURST; i++) {
        wg_slot_t* s = slot_get(d);
        wg_addr_t src;
        ssize_t n;
        int t;
        if (!s)
            break;
        src.len = sizeof src.u;
        n = recvfrom(fd, s->data, WG_DEV_SLOT, 0, &src.u.sa, &src.len);
        if (n < 0) {
            slot_put(d, s);
            if (errno == EINTR)
                continue;
            break;
        }
        s->len = (uint32_t)n;
        t = n >= 4 ? (int)wg_le32(s->data) : 0;
        if (t == WG_MSG_DATA && rx_data(d, s, &src) == 0)
            continue;
        if ((t == WG_MSG_INIT && n == WG_INIT_LEN) || (t == WG_MSG_RESP && n == WG_RESP_LEN) || (t == WG_MSG_COOKIE && n == WG_COOKIE_LEN))
            rx_handshake(d, s->data, (size_t)n, &src);
        else
            d->st.rx_drop++;
        slot_put(d, s);
    }
    rx_submit(d);
}

static const uint8_t* dpeer_pub(const void* v)
{
    return ((const wg_dpeer_t*)v)->np.pub;
}

wg_dev_t* wg_dev_new(struct dyn_aio* aio, const uint8_t priv[WG_KEY], const char* host, uint16_t port, wg_dev_rx_fn rx, void* ud)
{
    wg_static_t s;
    if (wg_static_init(&s, priv) < 0)
        return NULL;
    return wg_dev_new_static(aio, &s, host, port, rx, ud);
}

wg_dev_t* wg_dev_new_static(struct dyn_aio* aio, wg_static_t* s, const char* host, uint16_t port, wg_dev_rx_fn rx, void* ud)
{
    wg_dev_t* d = (wg_dev_t*)calloc(1, sizeof *d);
    if (!d) {
        wg_static_free(s);
        return NULL;
    }
    d->s = *s;
    wg_wipe(s, sizeof *s);
    d->aio = aio;
    d->rx = rx;
    d->ud = ud;
    d->load_threshold = WG_UNDER_LOAD_QUEUE;
    d->fd = -1;
    if (wg_idx_init(&d->idx, 64) < 0 || wg_pubtab_init(&d->pubs, dpeer_pub) < 0)
        goto fail;
    d->fd = dyn_aio_udp_bind(aio, host, port);
    if (d->fd >= 0) {
        int sz = WG_DEV_SOCKBUF;
        setsockopt(d->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
        setsockopt(d->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    }
    if (d->fd < 0 || dyn_aio_pool_warm(aio) < 0 || dyn_aio_watch_fd(aio, d->fd, on_readable, d) < 0)
        goto fail;
    return d;
fail:
    wg_dev_free(d);
    return NULL;
}

void wg_dev_remove_peer(wg_dev_t* d, wg_dpeer_t* p)
{
    wg_slot_t* s;
    if (!p || p->dead)
        return;
    wg_pubtab_del(&d->pubs, p->np.pub);
    wg_aip_remove_peer(&d->aip, p);
    heap_remove(d, p);
    wg_slots_clear(&p->slots, kp_drop, d);
    hs_reset(d, p);
    while ((s = p->stage_head) != NULL) {
        p->stage_head = s->next;
        slot_put(d, s);
    }
    p->stage_tail = NULL;
    p->nstage = 0;
    p->dead = 1;
    peer_unref(p);
}

void wg_dev_close(wg_dev_t* d)
{
    if (d->fd >= 0) {
        dyn_aio_unwatch_fd(d->aio, d->fd);
        close(d->fd);
        d->fd = -1;
    }
    d->closing = 1;
}

int wg_dev_routable(const wg_dev_t* d, int v6, const uint8_t* addr)
{
    return wg_aip_lookup(&d->aip, v6, addr) != NULL;
}

wg_dpeer_t* wg_dev_peer(const wg_dev_t* d, const uint8_t pub[WG_KEY])
{
    return (wg_dpeer_t*)wg_pubtab_get(&d->pubs, pub);
}

void* wg_dev_peer_ud(const wg_dpeer_t* p)
{
    return p->ready_ud;
}

void wg_dev_free(wg_dev_t* d)
{
    wg_slot_t* s;
    wg_dbatch_t* b;
    uint32_t i;
    if (!d)
        return;
    wg_dev_close(d);
    while (d->jobs)
        dyn_aio_run_us(d->aio, 10000);
    for (i = 0; i < d->ndirty; i++) {
        d->dirty[i]->dirty = 0;
        peer_unref(d->dirty[i]);
    }
    d->ndirty = 0;
    if (d->pubs.s && d->pubs.n) {
        wg_dpeer_t** all = (wg_dpeer_t**)malloc(d->pubs.n * sizeof *all);
        uint32_t n = 0;
        for (i = 0; all && i <= d->pubs.mask; i++)
            if (d->pubs.s[i])
                all[n++] = (wg_dpeer_t*)d->pubs.s[i];
        for (i = 0; i < n; i++)
            wg_dev_remove_peer(d, all[i]);
        free(all);
    }
    while ((b = d->free_batches) != NULL) {
        d->free_batches = b->free_next;
        free(b);
    }
    while ((s = d->free_slots) != NULL) {
        d->free_slots = s->next;
        free(s);
    }
    free(d->heap);
    free(d->dirty);
    wg_aip_free(&d->aip);
    wg_idx_free(&d->idx);
    wg_pubtab_free(&d->pubs);
    wg_static_free(&d->s);
    wg_wipe(d, sizeof *d);
    free(d);
}

int wg_dev_port(const wg_dev_t* d)
{
    struct sockaddr_storage ss;
    socklen_t l = sizeof ss;
    if (getsockname(d->fd, (struct sockaddr*)&ss, &l) < 0)
        return -1;
    return ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&ss)->sin6_port : ((struct sockaddr_in*)&ss)->sin_port);
}

const uint8_t* wg_dev_pub(const wg_dev_t* d)
{
    return d->s.pub;
}

void wg_dev_set_clock(wg_dev_t* d, wg_dev_clock_fn fn, void* ud)
{
    d->clock = fn;
    d->clock_ud = ud;
}

void wg_dev_set_load_threshold(wg_dev_t* d, unsigned pending)
{
    d->load_threshold = pending;
}

typedef struct {
    wg_dev_t* d;
    wg_dpeer_t* p;
    wg_peer_t tmp;
    uint8_t pub[WG_KEY], psk[WG_KEY];
    int has_psk, rc;
} wg_pjob_t;

static void pjob_work(void* arg)
{
    wg_pjob_t* j = (wg_pjob_t*)arg;
    j->rc = wg_peer_init(&j->tmp, &j->d->s, j->pub, j->has_psk ? j->psk : NULL);
}

static void pjob_done(void* arg)
{
    wg_pjob_t* j = (wg_pjob_t*)arg;
    wg_dev_t* d = j->d;
    wg_dpeer_t* p = j->p;
    d->jobs--;
    d->st.peers_pending--;
    if (!p->dead && !d->closing) {
        if (j->rc < 0) {
            wg_dev_peer_fn fn = p->ready;
            void* ud = p->ready_ud;
            p->refs++;
            wg_dev_remove_peer(d, p);
            if (fn)
                fn(ud, p, -1);
            peer_unref(p);
        } else {
            memcpy(p->np.ss, j->tmp.ss, WG_KEY);
            memcpy(p->np.hash0, j->tmp.hash0, WG_KEY);
            memcpy(p->np.mac1_key, j->tmp.mac1_key, WG_KEY);
            memcpy(p->np.cookie_key, j->tmp.cookie_key, WG_KEY);
            p->pending = 0;
            if (p->ready)
                p->ready(p->ready_ud, p, 0);
            if (!p->dead && (p->want_init || p->nstage)) {
                p->want_init = 0;
                initiate(d, p, 0);
            }
            if (!p->dead)
                resched(d, p);
        }
    }
    peer_unref(p);
    wg_wipe(j, sizeof *j);
    free(j);
}

wg_dpeer_t* wg_dev_add_peer(wg_dev_t* d, const uint8_t pub[WG_KEY], const uint8_t* psk, unsigned persist_s, wg_dev_peer_fn ready, void* ud)
{
    wg_dpeer_t* p = (wg_dpeer_t*)calloc(1, sizeof *p);
    wg_pjob_t* j = (wg_pjob_t*)calloc(1, sizeof *j);
    if (!p || !j || d->closing) {
        free(p);
        free(j);
        return NULL;
    }
    p->kind = K_PEER;
    p->refs = 1;
    p->hpos = -1;
    p->dev = d;
    p->pending = 1;
    p->ready = ready;
    p->ready_ud = ud;
    p->t.persist_s = (uint16_t)(persist_s > 65535u ? 65535u : persist_s);
    memcpy(p->np.pub, pub, WG_KEY);
    if (psk)
        memcpy(p->np.psk, psk, WG_KEY);
    if (wg_pubtab_get(&d->pubs, pub) || wg_pubtab_put(&d->pubs, p) < 0) {
        wg_wipe(p, sizeof *p);
        free(p);
        free(j);
        return NULL;
    }
    j->d = d;
    j->p = p;
    memcpy(j->pub, pub, WG_KEY);
    if (psk) {
        memcpy(j->psk, psk, WG_KEY);
        j->has_psk = 1;
    }
    p->refs += 2;
    d->st.peers_pending++;
    job_submit(d, pjob_work, pjob_done, j);
    if (p->dead) {
        peer_unref(p);
        return NULL;
    }
    peer_unref(p);
    return p;
}

int wg_dev_peer_ready(const wg_dpeer_t* p)
{
    return p && !p->pending && !p->dead;
}

void wg_dev_set_verify(wg_dev_t* d, int on)
{
    d->verify = !!on;
}

int wg_dev_set_endpoint(wg_dpeer_t* p, const struct sockaddr* sa, unsigned len)
{
    if (len > sizeof p->ep.u || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6))
        return -1;
    memset(&p->ep, 0, sizeof p->ep);
    memcpy(&p->ep.u, sa, len);
    p->ep.len = (socklen_t)len;
    return 0;
}

int wg_dev_get_endpoint(const wg_dpeer_t* p, struct sockaddr* sa, unsigned* len)
{
    if (!p->ep.len || *len < (unsigned)p->ep.len)
        return -1;
    memcpy(sa, &p->ep.u, p->ep.len);
    *len = (unsigned)p->ep.len;
    return 0;
}

int wg_dev_allow(wg_dev_t* d, wg_dpeer_t* p, int v6, const uint8_t* addr, unsigned cidr)
{
    return wg_aip_insert(&d->aip, v6, addr, cidr, p);
}

void wg_dev_tick(wg_dev_t* d)
{
    uint64_t now = now_ms(d);
    while (d->nheap && wg_timers_next(&d->heap[0]->t) <= now) {
        wg_dpeer_t* p = d->heap[0];
        unsigned a = wg_timers_due(&p->t, now);
        if (a & WG_ACT_ZERO_KEYS) {
            wg_slots_clear(&p->slots, kp_drop, d);
            hs_reset(d, p);
        }
        if (a & WG_ACT_GIVE_UP)
            while (p->stage_head) {
                wg_slot_t* s = p->stage_head;
                p->stage_head = s->next;
                p->nstage--;
                slot_put(d, s);
            }
        if (!p->stage_head)
            p->stage_tail = NULL;
        if (a & WG_ACT_SEND_INIT)
            initiate(d, p, (a & WG_ACT_RETRY) != 0);
        if (a & WG_ACT_SEND_KEEPALIVE)
            send_keepalive(d, p);
        resched(d, p);
    }
}

uint64_t wg_dev_next_deadline(const wg_dev_t* d)
{
    return d->nheap ? wg_timers_next(&d->heap[0]->t) : 0;
}

size_t wg_dev_pending(const wg_dev_t* d)
{
    return d->jobs;
}

void wg_dev_stats(const wg_dev_t* d, wg_dev_stats_t* st)
{
    *st = d->st;
}

const wg_keypair_t* wg_dev_current(const wg_dpeer_t* p)
{
    return p->slots.cur;
}

#endif
