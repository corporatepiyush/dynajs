/* dyn-dns-model.h -- RFC 1035 model shared by tests/dns/test_dns_codec.c (seeded rng) and src/fuzz/fuzz_dns.c (fuzz
 * bytes). dm_ref_name is an independent name decoder (labels collected, text joined afterwards, 255-octet wire cap);
 * dm_gen writes a message of random, pointer-compressed names and records each name's text; dm_check_gen checks the
 * codec against that record, dm_check_raw checks arbitrary bytes against the reference, dm_check_enc round-trips
 * encode / build_query / begin_response / add_answer. Each check returns NULL or a message naming the broken rule. */
#ifndef DYN_DNS_MODEL_H
#define DYN_DNS_MODEL_H

#include "dyn-dns.h"
#include "dyn-fuzzkit.h"

#include <stdint.h>
#include <string.h>

#define DM_CAP 4096
#define DM_ENT 256
#define DM_RR 16
#define DM_NAMES 40

typedef struct {
    const uint8_t* p;
    size_t n;
    fk_rng_t* r;
} dm_src_t;

typedef struct {
    size_t off, tlen, wire;
    char txt[DM_CAP];
} dm_ent_t;

typedef struct {
    uint8_t buf[DM_CAP];
    size_t len, qend;
    uint16_t qd, an;
    int full;
    size_t nent, nname;
    dm_ent_t ent[DM_ENT], name[DM_NAMES];
    size_t nend[DM_NAMES], rname[DM_RR], rdoff[DM_RR];
    uint16_t rtype[DM_RR], rdlen[DM_RR];
    uint32_t rttl[DM_RR];
} dm_msg_t;

static uint32_t dm_pick(dm_src_t* s, uint32_t k)
{
    uint32_t v;
    if (s->r)
        return fk_below(s->r, k);
    if (!s->n || !k)
        return 0;
    v = *s->p++;
    s->n--;
    return v % k;
}

static int dm_ref_name(const uint8_t* m, size_t len, size_t off, char* txt, size_t* tlen)
{
    size_t lab[128], ln[128], k = 0, pos = off, lim = off, end = 0, wire = 1, i, t = 0;
    int jumped = 0;
    for (;;) {
        unsigned c;
        if (pos >= len)
            return DYN_DNS_E_SHORT;
        c = m[pos];
        if ((c >> 6) == 3) {
            size_t to;
            if (len - pos < 2)
                return DYN_DNS_E_SHORT;
            to = ((size_t)(c & 63) << 8) | m[pos + 1];
            if (to < DYN_DNS_HDR_LEN || to >= lim)
                return DYN_DNS_E_LOOP;
            if (!jumped)
                end = pos + 2;
            jumped = 1;
            lim = pos = to;
            continue;
        }
        if (c >> 6)
            return DYN_DNS_E_FORMAT;
        if (!c)
            break;
        if (len - pos - 1 < c)
            return DYN_DNS_E_SHORT;
        wire += 1 + c;
        if (wire > 255)
            return DYN_DNS_E_NAME;
        lab[k] = pos + 1;
        ln[k++] = c;
        pos += 1 + c;
    }
    for (i = 0; i < k; i++) {
        if (i)
            txt[t++] = '.';
        memcpy(txt + t, m + lab[i], ln[i]);
        t += ln[i];
    }
    txt[t] = 0;
    *tlen = t;
    return (int)(jumped ? end : pos + 1);
}

static int dm_ref_rr(const uint8_t* m, size_t len, size_t* off, dyn_dns_rr_t* rr, size_t* tlen)
{
    int a = dm_ref_name(m, len, *off, rr->name, tlen);
    size_t p;
    if (a < 0)
        return a;
    p = (size_t)a;
    if (len < p || len - p < 10)
        return DYN_DNS_E_SHORT;
    rr->type = (uint16_t)(m[p] * 256 + m[p + 1]);
    rr->cls = (uint16_t)(m[p + 2] * 256 + m[p + 3]);
    rr->ttl = (uint32_t)m[p + 4] << 24 | (uint32_t)m[p + 5] << 16 | (uint32_t)m[p + 6] << 8 | m[p + 7];
    if (rr->ttl > 0x7fffffffu)
        rr->ttl = 0;
    rr->rdlen = (uint16_t)(m[p + 8] * 256 + m[p + 9]);
    if (len - p - 10 < rr->rdlen)
        return DYN_DNS_E_SHORT;
    rr->rdata = m + p + 10;
    *off = p + 10 + rr->rdlen;
    return DYN_DNS_OK;
}

static const char* dm_cmp_name(const uint8_t* m, size_t len, size_t off)
{
    static char got[DYN_DNS_MAX_NAME + 8], want[DM_CAP], small[DYN_DNS_MAX_NAME + 8];
    size_t tl = 0, cap;
    int a = dyn_dns_name_decode(m, len, off, got, DYN_DNS_MAX_NAME + 1), b = dm_ref_name(m, len, off, want, &tl);
    if (a != b)
        return "name_decode verdict or end offset differs from the RFC 1035 reference";
    if (a >= 0 && memcmp(got, want, tl + 1))
        return "name_decode text differs from the reference";
    if (a >= 0 && tl > 253)
        return "name_decode accepted a name over 255 wire octets";
    cap = 1 + (off * 7 + len) % (tl + 3);
    memset(small, 0x5a, sizeof small);
    a = dyn_dns_name_decode(m, len, off, small, cap);
    if (small[cap] != 0x5a || (cap < sizeof small - 1 && small[cap + 1] != 0x5a))
        return "name_decode wrote past outcap";
    if (!memchr(small, 0, cap))
        return "name_decode left the output unterminated inside outcap";
    if (b >= 0 && a >= 0 && (a != b || memcmp(small, want, tl + 1)))
        return "a small outcap changed a successful decode";
    if (b >= 0 && a < 0 && (a != DYN_DNS_E_NAME || tl + 2 <= cap))
        return "a small outcap refused a name that fits (textlen + 2 <= outcap)";
    if (b >= 0 && a >= 0 && tl + 1 > cap)
        return "a small outcap accepted a name that does not fit";
    if (b < 0 && a >= 0)
        return "a small outcap accepted a name the reference refuses";
    return NULL;
}

static const char* dm_check_resp(const uint8_t* q, size_t qlen, int rcode, uint32_t ttl, const uint8_t* addr, uint16_t an)
{
    static uint8_t out[DM_CAP + 64];
    static char qn[DM_CAP];
    dyn_dns_hdr_t qh, h;
    dyn_dns_rr_t rr;
    size_t tl = 0, off, cap = qlen + 12 + an - (ttl & 1);
    int r = dyn_dns_begin_response(q, qlen, rcode, out, cap), e, a2;
    if (qlen < DYN_DNS_HDR_LEN)
        return r == DYN_DNS_E_SHORT ? NULL : "begin_response accepted a query shorter than a header";
    dyn_dns_hdr_decode(q, qlen, &qh);
    e = dm_ref_name(q, qlen, DYN_DNS_HDR_LEN, qn, &tl);
    if (qh.qdcount != 1)
        return r == DYN_DNS_E_FORMAT ? NULL : "begin_response accepted QDCOUNT != 1";
    if (e < 0)
        return r == e ? NULL : "begin_response question verdict differs from the reference";
    if ((size_t)e + 4 > qlen)
        return r == DYN_DNS_E_SHORT ? NULL : "begin_response accepted a question without QTYPE/QCLASS";
    if (r != e + 4)
        return "begin_response length is not header + question";
    if (memcmp(out + 12, q + 12, (size_t)e + 4 - 12))
        return "begin_response did not copy the question verbatim";
    dyn_dns_hdr_decode(out, (size_t)r, &h);
    if (h.id != qh.id || h.qdcount != 1 || h.ancount || h.nscount || h.arcount)
        return "begin_response header id or counts";
    if (h.flags != ((qh.flags & 0x7900) | 0x8080 | (rcode & 15)))
        return "begin_response flags: QR, RA, rcode, opcode and RD copied (RFC 1035 4.1.1)";
    a2 = dyn_dns_add_answer(out, cap, (size_t)r, DYN_DNS_T_AAAA, ttl, addr, an);
    if ((size_t)r + 12 + an > cap)
        return a2 == DYN_DNS_E_SHORT ? NULL : "add_answer wrote past outcap";
    if (a2 != r + 12 + an)
        return "add_answer length";
    dyn_dns_set_ancount(out, 1);
    off = DYN_DNS_HDR_LEN;
    if (dyn_dns_skip_questions(out, (size_t)a2, &off, 1) || off != (size_t)r)
        return "the response question does not skip to its end";
    if (dyn_dns_rr_decode(out, (size_t)a2, &off, &rr) || off != (size_t)a2)
        return "the response answer does not decode to the message end";
    if (strcmp(rr.name, qn) || rr.type != DYN_DNS_T_AAAA || rr.cls != DYN_DNS_C_IN || rr.rdlen != an
        || rr.ttl != (ttl > 0x7fffffffu ? 0 : ttl) || memcmp(rr.rdata, addr, an))
        return "the response answer does not round-trip (name via pointer 0xc00c, type, class, ttl, rdata)";
    return NULL;
}

static const char* dm_check_raw(const uint8_t* m, size_t len, dm_src_t* s)
{
    static dyn_dns_rr_t a, b;
    dyn_dns_hdr_t h;
    size_t o1, o2, tl, i;
    uint16_t k;
    const char* e;
    int r1, r2;
    static const uint8_t addr[16] = { 0x20, 1, 0xd, 0xb8 };
    if (dyn_dns_hdr_decode(m, len, &h) != (len < 12 ? DYN_DNS_E_SHORT : DYN_DNS_OK))
        return "hdr_decode verdict";
    for (i = 0; i < 4; i++) {
        size_t off = len ? dm_pick(s, (uint32_t)len) : 0;
        if ((e = dm_cmp_name(m, len, i ? off : (len > 12 ? 12 : 0))))
            return e;
    }
    if (len < 12)
        return dm_check_resp(m, len, 0, 0, addr, 0);
    {
        size_t oa = 12, ob = 12;
        char t[DM_CAP];
        int ra = dyn_dns_skip_questions(m, len, &oa, h.qdcount), rb = 0;
        for (k = 0; k < h.qdcount && !rb; k++) {
            rb = dm_ref_name(m, len, ob, t, &tl);
            rb = rb < 0 ? rb : (size_t)rb + 4 > len ? DYN_DNS_E_SHORT : (ob = (size_t)rb + 4, 0);
        }
        if (ra != rb || oa != (rb ? 12 : ob))
            return "skip_questions over QDCOUNT differs from the reference, or moved *off on error";
    }
    o1 = o2 = 12;
    for (k = 0; k < h.qdcount && k < 64; k++) {
        size_t before = o1;
        char t[DM_CAP];
        r1 = dyn_dns_skip_questions(m, len, &o1, 1);
        r2 = dm_ref_name(m, len, o2, t, &tl);
        if (r2 >= 0)
            r2 = (size_t)r2 + 4 > len ? DYN_DNS_E_SHORT : (o2 = (size_t)r2 + 4, 0);
        if (r1 != r2 || (r1 == 0 && o1 != o2))
            return "skip_questions differs from the reference";
        if (r1)
            return o1 == before ? NULL : "skip_questions moved *off on error";
    }
    for (k = 0; k < (unsigned)h.ancount + h.nscount + h.arcount && k < 64; k++) {
        size_t before = o1;
        r1 = dyn_dns_rr_decode(m, len, &o1, &a);
        r2 = dm_ref_rr(m, len, &o2, &b, &tl);
        if (r1 != r2)
            return "rr_decode verdict differs from the reference";
        if (r1)
            return o1 == before ? NULL : "rr_decode moved *off on error";
        if (o1 != o2 || strcmp(a.name, b.name) || a.type != b.type || a.cls != b.cls || a.ttl != b.ttl
            || a.rdlen != b.rdlen || a.rdata != b.rdata)
            return "rr_decode fields differ (TTL with the top bit set reads as 0, RFC 2181 8)";
    }
    return dm_check_resp(m, len, (int)dm_pick(s, 16), (uint32_t)dm_pick(s, 4) << 30 | dm_pick(s, 1000), addr,
        (uint16_t)(dm_pick(s, 2) ? 16 : dm_pick(s, 17)));
}

static void dm_mutate(dm_src_t* s, uint8_t* b, size_t n)
{
    unsigned e = dm_pick(s, 5);
    while (n > 1 && e--) {
        size_t p = dm_pick(s, (uint32_t)n - 1), t = dm_pick(s, 2) ? dm_pick(s, 16) : p + 2 - dm_pick(s, 6);
        if (dm_pick(s, 2)) {
            b[p] = (uint8_t)(0xc0 | (t >> 8 & 63));
            b[p + 1] = (uint8_t)t;
        } else
            b[p] = (uint8_t)dm_pick(s, 256);
    }
}

static void dm_put(dm_msg_t* m, const void* p, size_t n)
{
    if (m->len + n > DM_CAP) {
        m->full = 1;
        return;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
}

static void dm_u16(dm_msg_t* m, uint32_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    dm_put(m, b, 2);
}

static void dm_ent(dm_ent_t* e, size_t off, const char* txt, size_t tl, size_t wire)
{
    e->off = off;
    e->tlen = tl;
    e->wire = wire;
    memcpy(e->txt, txt, tl);
    e->txt[tl] = 0;
}

static void dm_gen_name(dm_src_t* s, dm_msg_t* m)
{
    static const char al[] = "abcdefghijklmnopqrstuvwxyz0123456789-_";
    static char txt[DM_CAP];
    size_t k = dm_pick(s, 16) ? dm_pick(s, 4) : 4 + dm_pick(s, 4), i, j, start = m->len, tl = 0, wire = 1;
    size_t idx[8], tpos[8], wpre[8];
    const dm_ent_t* tail = m->nent && dm_pick(s, 3) ? &m->ent[dm_pick(s, (uint32_t)m->nent)] : NULL;
    uint8_t z = 0;
    if (tail && tail->tlen > DM_CAP - 1024)
        tail = NULL;
    for (i = 0; i < k; i++) {
        uint8_t c = (uint8_t)(dm_pick(s, 8) ? 1 + dm_pick(s, 12) : 1 + dm_pick(s, 63)), b;
        if (tl)
            txt[tl++] = '.';
        idx[i] = m->len;
        tpos[i] = tl;
        wpre[i] = wire - 1;
        dm_put(m, &c, 1);
        for (j = 0; j < c; j++) {
            b = (uint8_t)(dm_pick(s, 32) ? al[dm_pick(s, sizeof al - 1)] : dm_pick(s, 256));
            dm_put(m, &b, 1);
            txt[tl++] = (char)b;
        }
        wire += 1u + c;
    }
    if (tail) {
        uint8_t p[2] = { (uint8_t)(0xc0 | tail->off >> 8), (uint8_t)tail->off };
        dm_put(m, p, 2);
        if (tl && tail->tlen)
            txt[tl++] = '.';
        memcpy(txt + tl, tail->txt, tail->tlen);
        tl += tail->tlen;
        wire += tail->wire - 1;
    } else
        dm_put(m, &z, 1);
    if (m->full || m->nname >= DM_NAMES)
        return;
    m->nend[m->nname] = m->len;
    dm_ent(&m->name[m->nname++], start, txt, tl, wire);
    for (i = 0; i < k && m->nent < DM_ENT; i++)
        dm_ent(&m->ent[m->nent++], idx[i], txt + tpos[i], tl - tpos[i], wire - wpre[i]);
    if (!k && !tail && m->nent < DM_ENT)
        dm_ent(&m->ent[m->nent++], start, txt, 0, 1);
}

static void dm_gen(dm_src_t* s, dm_msg_t* m)
{
    uint16_t i;
    m->len = m->nent = m->nname = 0;
    m->full = 0;
    m->qd = (uint16_t)(1 + dm_pick(s, 3));
    m->an = (uint16_t)dm_pick(s, DM_RR + 1);
    dm_u16(m, dm_pick(s, 65536));
    dm_u16(m, dm_pick(s, 65536));
    dm_u16(m, m->qd);
    dm_u16(m, m->an);
    dm_u16(m, 0);
    dm_u16(m, 0);
    for (i = 0; i < m->qd; i++) {
        dm_gen_name(s, m);
        dm_u16(m, 1 + dm_pick(s, 255));
        dm_u16(m, DYN_DNS_C_IN);
    }
    m->qend = m->len;
    for (i = 0; i < m->an; i++) {
        size_t j, n;
        m->rname[i] = m->nname;
        dm_gen_name(s, m);
        m->rtype[i] = (uint16_t)(dm_pick(s, 2) ? DYN_DNS_T_CNAME : 1 + dm_pick(s, 255));
        m->rttl[i] = dm_pick(s, 4) << 30 | dm_pick(s, 100000);
        dm_u16(m, m->rtype[i]);
        dm_u16(m, DYN_DNS_C_IN);
        dm_u16(m, m->rttl[i] >> 16);
        dm_u16(m, m->rttl[i] & 0xffff);
        j = m->len;
        dm_u16(m, 0);
        m->rdoff[i] = m->len;
        if (m->rtype[i] == DYN_DNS_T_CNAME)
            dm_gen_name(s, m);
        else
            for (n = dm_pick(s, 20); n; n--) {
                uint8_t b = (uint8_t)dm_pick(s, 256);
                dm_put(m, &b, 1);
            }
        m->rdlen[i] = (uint16_t)(m->len - m->rdoff[i]);
        if (!m->full) {
            m->buf[j] = (uint8_t)(m->rdlen[i] >> 8);
            m->buf[j + 1] = (uint8_t)m->rdlen[i];
        }
    }
}

static const char* dm_check_gen(dm_msg_t* m)
{
    static char got[DYN_DNS_MAX_NAME + 1];
    dyn_dns_hdr_t h;
    dyn_dns_rr_t rr;
    size_t i, off = DYN_DNS_HDR_LEN;
    int r;
    if (m->full)
        return NULL;
    if (dyn_dns_hdr_decode(m->buf, m->len, &h) || h.qdcount != m->qd || h.ancount != m->an)
        return "hdr_decode counts";
    for (i = 0; i < m->nname + m->nent; i++) {
        const dm_ent_t* e = i < m->nname ? &m->name[i] : &m->ent[i - m->nname];
        r = dyn_dns_name_decode(m->buf, m->len, e->off, got, sizeof got);
        if (e->wire > 255) {
            if (r != DYN_DNS_E_NAME)
                return "a generated name over 255 wire octets (RFC 1035 3.1) was not refused with E_NAME";
            continue;
        }
        if (r < 0)
            return "a generated, pointer-compressed name was refused";
        if (strlen(got) != strlen(e->txt) || memcmp(got, e->txt, e->tlen + 1))
            return "a pointer-compressed name decoded to the wrong text";
        if (i < m->nname && (size_t)r != m->nend[i])
            return "name_decode end offset is not the end of the name in place";
    }
    for (i = 0; i < m->nname; i++)
        if (m->name[i].wire > 255)
            return NULL;
    r = dyn_dns_skip_questions(m->buf, m->len, &off, m->qd);
    if (r || off != m->qend)
        return "skip_questions did not stop at the end of the question section";
    for (i = 0; i < m->an; i++) {
        const dm_ent_t* e = &m->name[m->rname[i]];
        if (dyn_dns_rr_decode(m->buf, m->len, &off, &rr))
            return "rr_decode refused a generated record";
        if (strcmp(rr.name, e->txt) || rr.type != m->rtype[i] || rr.cls != DYN_DNS_C_IN || rr.rdlen != m->rdlen[i]
            || rr.rdata != m->buf + m->rdoff[i] || rr.ttl != (m->rttl[i] >> 31 ? 0 : m->rttl[i]))
            return "rr_decode fields differ from the generated record (TTL top bit reads as 0, RFC 2181 8)";
    }
    return off == m->len ? NULL : "the answers did not end at the message end";
}

static int dm_ref_enc(const char* name, uint8_t* out, size_t cap)
{
    size_t len = strlen(name), w = 0, a = 0, b;
    if (len && name[len - 1] == '.')
        len--;
    if (!len && name[0] == '.')
        return DYN_DNS_E_FORMAT;
    while (len && a <= len) {
        for (b = a; b < len && name[b] != '.'; b++)
            ;
        if (b == a)
            return DYN_DNS_E_FORMAT;
        if (b - a > DYN_DNS_MAX_LABEL)
            return DYN_DNS_E_NAME;
        if (w + 2 + (b - a) > cap)
            return DYN_DNS_E_SHORT;
        if (w + 2 + (b - a) > DYN_DNS_MAX_NAME)
            return DYN_DNS_E_NAME;
        out[w++] = (uint8_t)(b - a);
        memcpy(out + w, name + a, b - a);
        w += b - a;
        a = b + 1;
    }
    if (w + 1 > cap)
        return DYN_DNS_E_SHORT;
    out[w++] = 0;
    return (int)w;
}

static const char* dm_check_enc(dm_src_t* s)
{
    static const char al[] = "abcxyz019-_";
    static const uint8_t addr[16] = { 0x20, 1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    char name[600], txt[600];
    uint8_t out[600], ref[600], q[620];
    size_t k = dm_pick(s, 4) ? dm_pick(s, 6) : dm_pick(s, 9), i, j, n = 0, cap, tl;
    int r, e;
    uint16_t id = (uint16_t)dm_pick(s, 65536), type = (uint16_t)(1 + dm_pick(s, 300));
    size_t want = dm_pick(s, 4) ? 0 : 246 + dm_pick(s, 14);
    if (want)
        k = (want + 63) / 64;
    for (i = 0; i < k; i++) {
        size_t c = dm_pick(s, 8) ? 1 + dm_pick(s, 12) : dm_pick(s, 5) ? 50 + dm_pick(s, 20) : 0;
        if (want)
            c = i + 1 < k ? 63 : want - 64 * i - 1;
        if (i)
            name[n++] = '.';
        for (j = 0; j < c; j++) {
            char ch = dm_pick(s, 16) ? al[dm_pick(s, sizeof al - 1)] : (char)(1 + dm_pick(s, 255));
            name[n++] = ch == '.' ? 'd' : ch;
        }
    }
    if (dm_pick(s, 6) == 0)
        name[n++] = '.';
    name[n] = 0;
    cap = dm_pick(s, 3) ? sizeof out : dm_pick(s, (uint32_t)n + 8);
    memset(out, 0xa5, sizeof out);
    r = dyn_dns_name_encode(name, out, cap);
    e = dm_ref_enc(name, ref, cap);
    if (r != e)
        return "name_encode verdict differs from the reference (labels 1..63, 255 wire octets, outcap)";
    if (cap < sizeof out && out[cap] != 0xa5)
        return "name_encode wrote past outcap";
    if (r >= 0) {
        if (memcmp(out, ref, (size_t)r))
            return "name_encode bytes differ from the reference";
        memcpy(txt, name, n + 1);
        if (n && txt[n - 1] == '.')
            txt[n - 1] = 0;
        if (dm_ref_name(out, (size_t)r, 0, (char*)q, &tl) != r || strcmp((char*)q, txt))
            return "name_encode output does not decode back to the name";
    }
    cap = dm_pick(s, 3) ? sizeof q : dm_pick(s, (uint32_t)n + 24);
    r = dyn_dns_build_query(id, name, type, q, cap);
    e = cap < DYN_DNS_HDR_LEN ? DYN_DNS_E_SHORT : dm_ref_enc(name, ref, cap - DYN_DNS_HDR_LEN);
    if (e >= 0)
        e = DYN_DNS_HDR_LEN + (size_t)e + 4 > cap ? DYN_DNS_E_SHORT : DYN_DNS_HDR_LEN + e + 4;
    if (r != e)
        return "build_query verdict or length differs from header + encoded name + 4";
    if (r < 0)
        return NULL;
    if (q[0] != id >> 8 || q[1] != (id & 255) || q[2] != 1 || q[3] || q[4] || q[5] != 1 || q[6] || q[7] || q[8] || q[9]
        || q[10] || q[11] || memcmp(q + 12, ref, (size_t)r - 16) || q[r - 4] != type >> 8 || q[r - 3] != (type & 255)
        || q[r - 2] || q[r - 1] != DYN_DNS_C_IN)
        return "build_query bytes: id, RD, QDCOUNT 1, name, QTYPE, QCLASS IN";
    if (dm_pick(s, 4) == 0)
        q[2] = (uint8_t)(q[2] | dm_pick(s, 16) << 3);
    return dm_check_resp(q, (size_t)r, (int)dm_pick(s, 16), (uint32_t)dm_pick(s, 4) << 30 | dm_pick(s, 999), addr,
        (uint16_t)(dm_pick(s, 2) ? 16 : 4));
}
#endif
