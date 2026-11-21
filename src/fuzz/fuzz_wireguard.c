#include "modules/wireguard/wg-internal.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_wireguard: oracle failed at line %d\n", line);
    abort();
}

#define FZ(c) \
    do { \
        if (!(c)) \
            fz_die(__LINE__); \
    } while (0)

typedef struct {
    wg_static_t s;
    wg_peer_t peer;
} fz_side_t;

typedef struct {
    const uint8_t* p;
    size_t n, i;
} in_t;

static fz_side_t fz_i, fz_r;
static wg_peer_t fz_i0, fz_r0;
static wg_cookie_gen_t fz_gen;
static wg_keypair_t fz_kp;
static wg_aip_t fz_aip;
static uint8_t fz_init[WG_INIT_LEN], fz_resp[WG_RESP_LEN], fz_cookie[WG_COOKIE_LEN], fz_data[256];
static int fz_data_len, fz_ready;
static uint64_t fz_ms = 1000000;

static unsigned in_u8(in_t* s)
{
    return s->i < s->n ? s->p[s->i++] : 0;
}

static uint64_t in_u64(in_t* s, unsigned k)
{
    uint64_t v = 0;
    while (k--)
        v = v << 8 | in_u8(s);
    return v;
}

static wg_peer_t* fz_by_pub(void* ud, const uint8_t pub[WG_KEY])
{
    fz_side_t* s = (fz_side_t*)ud;
    return memcmp(s->peer.pub, pub, WG_KEY) == 0 ? &s->peer : NULL;
}

static wg_peer_t* fz_by_index(void* ud, uint32_t index)
{
    fz_side_t* s = (fz_side_t*)ud;
    return s->peer.hs.local_index == index || index == 7 ? &s->peer : NULL;
}

static void fz_setup(void)
{
    static const uint8_t ki[WG_KEY] = { 0x40, 1, 2, 3 }, kr[WG_KEY] = { 0x48, 9, 8, 7 }, psk[WG_KEY] = { 5 };
    static const uint8_t a4[4] = { 10, 0, 0, 0 }, a6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    uint8_t pl[64] = { 0x45 };
    wg_aip_t t;
    wg_static_init(&fz_i.s, ki);
    wg_static_init(&fz_r.s, kr);
    wg_peer_init(&fz_i.peer, &fz_i.s, fz_r.s.pub, psk);
    wg_peer_init(&fz_r.peer, &fz_r.s, fz_i.s.pub, psk);
    fz_r0 = fz_r.peer;
    FZ(wg_create_initiation(&fz_i.s, &fz_i.peer, 7, 1700000000000000000ULL, fz_ms, fz_init) == 0);
    fz_i0 = fz_i.peer;
    FZ(wg_consume_initiation(&fz_r.s, fz_init, sizeof fz_init, fz_ms, fz_by_pub, &fz_r) == &fz_r.peer);
    FZ(wg_create_response(&fz_r.s, &fz_r.peer, 9, fz_ms, fz_resp) == 0);
    FZ(wg_create_cookie_reply(&fz_r.s, &fz_gen, fz_ms, fz_init, sizeof fz_init, a4, sizeof a4, fz_cookie) == 0);
    memset(&fz_kp, 0, sizeof fz_kp);
    memset(fz_kp.send, 0x11, WG_KEY);
    memcpy(fz_kp.recv, fz_kp.send, WG_KEY);
    memcpy(fz_data + WG_DATA_HDR, pl, sizeof pl);
    fz_data_len = wg_data_seal(fz_kp.send, 9, 3, fz_data, sizeof pl, sizeof fz_data, 1420);
    memset(&t, 0, sizeof t);
    fz_aip = t;
    wg_aip_insert(&fz_aip, 0, a4, 8, &fz_i);
    wg_aip_insert(&fz_aip, 1, a6, 64, &fz_r);
    fz_ready = 1;
}

static void fz_feed(const uint8_t* m, size_t n, const uint8_t* src, size_t srcn, unsigned flags)
{
    uint8_t buf[2048], c[WG_COOKIE_LEN];
    uint32_t rx;
    uint64_t ctr;
    int mc;
    if (n > sizeof buf)
        n = sizeof buf;
    mc = wg_check_macs(&fz_r.s, &fz_gen, fz_ms, m, n, src, srcn, (int)(flags & 1));
    if (mc == WG_MAC_NEED_COOKIE)
        wg_create_cookie_reply(&fz_r.s, &fz_gen, fz_ms, m, n, src, srcn, c);
    fz_r.peer = fz_r0;
    wg_consume_initiation(&fz_r.s, m, n, fz_ms, fz_by_pub, &fz_r);
    fz_i.peer = fz_i0;
    wg_consume_response(&fz_i.s, m, n, fz_by_index, &fz_i);
    fz_i.peer = fz_i0;
    wg_consume_cookie_reply(m, n, fz_ms, fz_by_index, &fz_i);
    memcpy(buf, m, n);
    if (wg_data_parse(buf, n, &rx, &ctr) == 0 && wg_data_open(fz_kp.recv, buf, n) >= 0)
        wg_replay_check(&fz_kp.replay, ctr);
    if (n >= 16)
        wg_aip_lookup(&fz_aip, (int)(flags >> 1 & 1), m);
    else if (n >= 4)
        wg_aip_lookup(&fz_aip, 0, m);
}

static void m_replay(in_t* s)
{
    static uint64_t seen[512];
    wg_replay_t r;
    size_t ns = 0, k;
    uint64_t max = 0;
    int any = 0;
    memset(&r, 0, sizeof r);
    while (s->i < s->n && ns < 512) {
        unsigned op = in_u8(s) % 8;
        uint64_t c, d = in_u8(s);
        int want, got, peek;
        if (op == 0)
            c = max + 1 + d % 4;
        else if (op == 1)
            c = max + 1 + (d << 4 | in_u8(s)) * 7;
        else if (op == 2)
            c = max - (d << 3 | in_u8(s) % 8);
        else if (op == 3)
            c = max - WG_RW_WINDOW - 2 + d % 5;
        else if (op == 4)
            c = WG_REJECT_AFTER_MESSAGES - 3 + d % 6;
        else if (op == 5)
            c = in_u64(s, 8);
        else if (op == 6 && ns)
            c = seen[d % ns];
        else
            c = max + (d << 8 | in_u8(s)) * 64;
        want = 0;
        if (c >= WG_REJECT_AFTER_MESSAGES)
            want = -1;
        else if (any && c <= max) {
            if (max - c > WG_RW_WINDOW)
                want = -1;
            else
                for (k = 0; k < ns; k++)
                    if (seen[k] == c)
                        want = -1;
        }
        peek = wg_replay_peek(&r, c);
        FZ(peek == want);
        if (in_u8(s) & 1) {
            got = wg_replay_check(&r, c);
            FZ(got == want);
            if (!want) {
                size_t o = 0;
                if (!any || c > max)
                    max = c;
                any = 1;
                for (k = 0; k < ns; k++)
                    if (max - seen[k] <= WG_RW_WINDOW)
                        seen[o++] = seen[k];
                ns = o;
                seen[ns++] = c;
                FZ(r.next == max + 1);
            }
        }
    }
}

typedef struct {
    uint8_t key[16];
    unsigned cidr;
    int v6;
    void* peer;
} ment_t;

static int pfx_eq(const uint8_t* a, const uint8_t* b, unsigned bits)
{
    unsigned i;
    for (i = 0; i < bits; i++)
        if ((a[i / 8] >> (7 - i % 8) & 1) != (b[i / 8] >> (7 - i % 8) & 1))
            return 0;
    return 1;
}

static void* m_lpm(const ment_t* e, size_t n, int v6, const uint8_t* a)
{
    size_t k;
    int best = -1;
    void* p = NULL;
    for (k = 0; k < n; k++)
        if (e[k].v6 == v6 && pfx_eq(e[k].key, a, e[k].cidr) && (int)e[k].cidr > best) {
            best = (int)e[k].cidr;
            p = e[k].peer;
        }
    return p;
}

static void m_addr(in_t* s, int v6, uint8_t* a)
{
    static const uint8_t base[16] = { 0xfd, 0, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    unsigned k, mode = in_u8(s) % 4;
    memcpy(a, base, 16);
    if (!v6)
        a[0] = 10;
    for (k = 0; k < (v6 ? 16u : 4u); k++) {
        if (mode == 0)
            a[k] = (uint8_t)in_u8(s);
        else if (mode == 1 && k == (v6 ? 15u : 3u))
            a[k] = (uint8_t)in_u8(s);
        else if (mode == 2 && k >= (v6 ? 14u : 2u))
            a[k] = (uint8_t)(in_u8(s) & 0x0f);
    }
}

static void m_aip(in_t* s)
{
    static ment_t e[96];
    static int peers[5];
    wg_aip_t t;
    size_t n = 0, k;
    uint8_t a[16];
    memset(&t, 0, sizeof t);
    while (s->i < s->n) {
        unsigned op = in_u8(s) % 6;
        int v6 = (int)(in_u8(s) & 1);
        unsigned maxc = v6 ? 128 : 32;
        m_addr(s, v6, a);
        if (op <= 1 && n < 96) {
            unsigned cidr = in_u8(s) % (maxc + 2);
            void* p = &peers[in_u8(s) % 5];
            int rc = wg_aip_insert(&t, v6, a, cidr, p);
            FZ((rc == 0) == (cidr <= maxc));
            if (rc == 0) {
                ment_t x;
                memset(&x, 0, sizeof x);
                for (k = 0; k < cidr; k++)
                    x.key[k / 8] |= (uint8_t)(a[k / 8] & (0x80u >> (k % 8)));
                x.cidr = cidr;
                x.v6 = v6;
                x.peer = p;
                for (k = 0; k < n; k++)
                    if (e[k].v6 == v6 && e[k].cidr == cidr && !memcmp(e[k].key, x.key, 16))
                        break;
                e[k] = x;
                if (k == n)
                    n++;
            }
        } else if (op == 2) {
            unsigned cidr = in_u8(s) % (maxc + 2);
            int rc = wg_aip_remove(&t, v6, a, cidr), want = -1;
            uint8_t key[16] = { 0 };
            for (k = 0; k < cidr && cidr <= maxc; k++)
                key[k / 8] |= (uint8_t)(a[k / 8] & (0x80u >> (k % 8)));
            for (k = 0; k < n && cidr <= maxc; k++)
                if (e[k].v6 == v6 && e[k].cidr == cidr && !memcmp(e[k].key, key, 16)) {
                    e[k] = e[--n];
                    want = 0;
                    break;
                }
            FZ(rc == want);
        } else if (op == 3) {
            void* p = &peers[in_u8(s) % 5];
            size_t o = 0;
            wg_aip_remove_peer(&t, p);
            for (k = 0; k < n; k++)
                if (e[k].peer != p)
                    e[o++] = e[k];
            n = o;
        } else {
            FZ(wg_aip_lookup(&t, v6, a) == m_lpm(e, n, v6, a));
            for (k = 0; k < n; k++)
                FZ(wg_aip_lookup(&t, e[k].v6, e[k].key) == m_lpm(e, n, e[k].v6, e[k].key));
        }
        FZ(t.nodes <= (n ? 2 * n - 1 : 0));
        FZ(n || (!t.root4 && !t.root6));
    }
    for (k = 0; k < n; k++)
        FZ(wg_aip_lookup(&t, e[k].v6, e[k].key) == m_lpm(e, n, e[k].v6, e[k].key));
    wg_aip_free(&t);
}

static size_t n_pad(size_t n, unsigned mtu)
{
    size_t full, last, p;
    if (mtu == 0)
        return (n + 15) / 16 * 16;
    full = n / mtu;
    last = n % mtu;
    if (last == 0 && n)
        return n;
    p = (last + 15) / 16 * 16;
    return full * mtu + (p > mtu ? mtu : p);
}

static void m_data(in_t* s)
{
    uint8_t key[WG_KEY], buf[1600], pt[1600], cp[1600];
    unsigned k, mtus[6] = { 0, 1420, 1280, 576, 17, 1 }, mtu = mtus[in_u8(s) % 6];
    size_t n = (in_u8(s) << 8 | in_u8(s)) % 1500, cap = sizeof buf - (in_u8(s) % 4 == 0 ? in_u8(s) * 4 : 0);
    uint64_t ctr = in_u8(s) % 5 == 0 ? WG_REJECT_AFTER_MESSAGES - 2 + in_u8(s) % 4 : in_u64(s, 3);
    uint32_t rx = (uint32_t)in_u64(s, 4), grx;
    uint64_t gctr;
    size_t padded = n_pad(n, mtu);
    int len, ol;
    for (k = 0; k < WG_KEY; k++)
        key[k] = (uint8_t)in_u8(s);
    for (k = 0; k < n; k++)
        pt[k] = (uint8_t)(in_u8(s) ^ k);
    FZ(wg_pad_len(n, mtu) == padded);
    FZ(padded >= n && padded - n < 16);
    memset(buf, 0xee, sizeof buf);
    memcpy(buf + WG_DATA_HDR, pt, n);
    len = wg_data_seal(key, rx, ctr, buf, n, cap, mtu);
    if (padded > cap || cap - padded < WG_DATA_MIN || ctr >= WG_REJECT_AFTER_MESSAGES) {
        FZ(len == -1);
        return;
    }
    FZ(len == (int)(padded + WG_DATA_MIN));
    FZ(wg_data_parse(buf, (size_t)len, &grx, &gctr) == 0 && grx == rx && gctr == ctr && wg_le32(buf) == WG_MSG_DATA);
    memcpy(cp, buf, (size_t)len);
    ol = wg_data_open(key, cp, (size_t)len);
    FZ(ol == (int)padded && !memcmp(cp + WG_DATA_HDR, pt, n));
    for (k = (unsigned)n; k < padded; k++)
        FZ(cp[WG_DATA_HDR + k] == 0);
    while (s->i < s->n) {
        size_t at = (in_u8(s) << 8 | in_u8(s)) % (size_t)len;
        uint8_t x = (uint8_t)(in_u8(s) | 1);
        int r;
        memcpy(cp, buf, (size_t)len);
        cp[at] ^= x;
        r = wg_data_open(key, cp, (size_t)len);
        FZ(at >= 4 && at < 8 ? r == ol : r == -1);
    }
    memcpy(cp, buf, (size_t)len);
    FZ(wg_data_open(key, cp, (size_t)len - 1 - in_u8(s) % 8) == -1 || len < WG_DATA_MIN + 8);
}

static void m_hs(in_t* s)
{
    uint8_t m[WG_INIT_LEN + 64];
    unsigned kind = in_u8(s) % 3, gate = in_u8(s) & 1, edits = 0;
    const uint8_t* orig = kind == 0 ? fz_init : kind == 1 ? fz_resp : fz_cookie;
    size_t olen = kind == 0 ? WG_INIT_LEN : kind == 1 ? WG_RESP_LEN : WG_COOKIE_LEN, n = olen;
    int got, same, macs;
    memcpy(m, orig, n);
    while (s->i < s->n && edits < 8) {
        unsigned op = in_u8(s) % 5;
        edits++;
        if (op == 0 && n)
            m[in_u8(s) % n] ^= (uint8_t)(in_u8(s) | 1);
        else if (op == 1)
            n = n ? in_u8(s) % (n + 1) : 0;
        else if (op == 2 && n < sizeof m)
            m[n++] = (uint8_t)in_u8(s);
        else if (op == 3 && n >= 4)
            wg_put_le32(m, in_u8(s) % 5);
        else if (n)
            m[in_u8(s) % n] = 0;
    }
    same = n == olen && !memcmp(m, orig, kind == 2 ? n : n - 16);
    macs = n == olen && kind < 2 && !gate && !memcmp(m, orig, 4) && !memcmp(m + 8, orig + 8, n - 40);
    fz_r.peer = fz_r0;
    fz_i.peer = fz_i0;
    if (kind == 0)
        got = (!gate || wg_check_macs(&fz_r.s, &fz_gen, fz_ms, m, n, NULL, 0, 0) == WG_MAC_OK) && wg_consume_initiation(&fz_r.s, m, n, fz_ms, fz_by_pub, &fz_r) != NULL;
    else if (kind == 1)
        got = (!gate || wg_check_macs(&fz_i.s, &fz_gen, fz_ms, m, n, NULL, 0, 0) == WG_MAC_OK) && wg_consume_response(&fz_i.s, m, n, fz_by_index, &fz_i) != NULL;
    else
        got = wg_consume_cookie_reply(m, n, fz_ms, fz_by_index, &fz_i) != NULL;
    if (same)
        FZ(got);
    else if (!macs)
        FZ(!got);
    if (kind == 2 && got)
        FZ(fz_i.peer.has_cookie && fz_i.peer.cookie_ms == fz_ms && !fz_i.peer.has_last_mac1);
    fz_r.peer = fz_r0;
    fz_i.peer = fz_i0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    uint8_t m[2048];
    size_t n, i;
    unsigned flags;
    in_t s;
    if (!fz_ready)
        fz_setup();
    if (size < 2)
        return 0;
    flags = data[0];
    data++;
    size--;
    s.p = data;
    s.n = size;
    s.i = 0;
    switch (flags % 8) {
    case 0:
        fz_feed(data, size, data, size < 18 ? size : 18, flags >> 3);
        return 0;
    case 1:
        m_replay(&s);
        return 0;
    case 2:
        m_aip(&s);
        return 0;
    case 3:
        m_data(&s);
        return 0;
    case 4:
    case 5:
        m_hs(&s);
        return 0;
    default:
        break;
    }
    switch (flags >> 3 & 3) {
    case 0:
        memcpy(m, fz_init, n = sizeof fz_init);
        break;
    case 1:
        memcpy(m, fz_resp, n = sizeof fz_resp);
        break;
    case 2:
        memcpy(m, fz_cookie, n = sizeof fz_cookie);
        break;
    default:
        memcpy(m, fz_data, n = (size_t)fz_data_len);
        break;
    }
    for (i = 0; i + 1 < size; i += 2)
        m[data[i] % n] ^= data[i + 1];
    if ((size & 1) && data[size - 1] < n)
        n = data[size - 1];
    fz_feed(m, n, data, size < 18 ? size : 18, flags >> 5);
    return 0;
}
