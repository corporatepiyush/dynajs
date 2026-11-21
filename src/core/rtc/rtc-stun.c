#include "rtc-stun.h"
#include "../dyn-hash.h"

int rtc_stun_is(const uint8_t* p, size_t n)
{
    return n >= 20 && p[0] < 4 && rtc_rd32(p + 4) == RTC_STUN_MAGIC && (rtc_rd16(p + 2) & 3) == 0;
}

int rtc_stun_parse(rtc_stun_msg* m, const uint8_t* p, size_t n)
{
    size_t off, end;
    if (!rtc_stun_is(p, n))
        return -1;
    end = 20 + (size_t)rtc_rd16(p + 2);
    if (end != n)
        return -1;
    m->buf = p;
    m->len = n;
    m->type = rtc_rd16(p);
    memcpy(m->tid, p + 8, 12);
    m->nattr = 0;
    m->mi = -1;
    m->fp = -1;
    off = 20;
    while (off < end) {
        uint16_t t, l;
        size_t padded;
        if (end - off < 4)
            return -1;
        t = rtc_rd16(p + off);
        l = rtc_rd16(p + off + 2);
        padded = ((size_t)l + 3) & ~(size_t)3;
        if (end - off - 4 < padded)
            return -1;
        if (m->fp >= 0)
            return -1;
        if (m->mi < 0 || t == RTC_SA_FINGERPRINT) {
            if (m->nattr >= RTC_STUN_MAX_ATTR)
                return -1;
            m->attr[m->nattr].type = t;
            m->attr[m->nattr].len = l;
            m->attr[m->nattr].off = (uint16_t)(off + 4);
            if (t == RTC_SA_MESSAGE_INTEGRITY) {
                if (l != 20)
                    return -1;
                m->mi = (int8_t)m->nattr;
            } else if (t == RTC_SA_FINGERPRINT) {
                if (l != 4)
                    return -1;
                m->fp = (int8_t)m->nattr;
            }
            m->nattr++;
        }
        off += 4 + padded;
    }
    return 0;
}

const rtc_stun_attr* rtc_stun_find(const rtc_stun_msg* m, uint16_t type)
{
    int i;
    for (i = 0; i < m->nattr; i++)
        if (m->attr[i].type == type)
            return &m->attr[i];
    return NULL;
}

int rtc_stun_check_fingerprint(const rtc_stun_msg* m)
{
    const rtc_stun_attr* a;
    size_t upto;
    uint32_t crc;
    if (m->fp < 0)
        return 0;
    a = &m->attr[m->fp];
    upto = (size_t)a->off - 4;
    crc = dyn_crc32(m->buf, upto) ^ 0x5354554Eu;
    return crc == rtc_rd32(m->buf + a->off);
}

int rtc_stun_check_integrity(const rtc_stun_msg* m, const uint8_t* key, size_t keylen)
{
    const rtc_stun_attr* a;
    uint8_t hdr[20], mac[20];
    dyn_hmac_ctx_t h;
    size_t upto;
    if (m->mi < 0)
        return 0;
    a = &m->attr[m->mi];
    upto = (size_t)a->off - 4;
    memcpy(hdr, m->buf, 20);
    rtc_wr16(hdr + 2, (uint32_t)(upto + 24 - 20));
    dyn_hmac_init(&h, dyn_hash_algo_by_id(DYN_HASH_SHA1), key, keylen);
    dyn_hmac_update(&h, hdr, 20);
    dyn_hmac_update(&h, m->buf + 20, upto - 20);
    dyn_hmac_final(&h, mac);
    return dyn_ct_equal(mac, m->buf + a->off, 20);
}

int rtc_stun_get_addr(const rtc_stun_msg* m, const rtc_stun_attr* a, rtc_addr* out)
{
    const uint8_t* v;
    int x;
    size_t i;
    if (!a || a->len < 8)
        return -1;
    v = m->buf + a->off;
    x = a->type != RTC_SA_MAPPED_ADDRESS;
    memset(out, 0, sizeof(*out));
    out->port = rtc_rd16(v + 2);
    if (x)
        out->port ^= (uint16_t)(RTC_STUN_MAGIC >> 16);
    if (v[1] == 1 && a->len == 8) {
        out->fam = 4;
        memcpy(out->ip, v + 4, 4);
        if (x)
            for (i = 0; i < 4; i++)
                out->ip[i] ^= m->buf[4 + i];
        return 0;
    }
    if (v[1] == 2 && a->len == 20) {
        out->fam = 6;
        memcpy(out->ip, v + 4, 16);
        if (x)
            for (i = 0; i < 16; i++)
                out->ip[i] ^= m->buf[4 + i];
        return 0;
    }
    return -1;
}

int rtc_stun_get_u32(const rtc_stun_msg* m, uint16_t type, uint32_t* out)
{
    const rtc_stun_attr* a = rtc_stun_find(m, type);
    if (!a || a->len != 4)
        return -1;
    *out = rtc_rd32(m->buf + a->off);
    return 0;
}

int rtc_stun_get_u64(const rtc_stun_msg* m, uint16_t type, uint64_t* out)
{
    const rtc_stun_attr* a = rtc_stun_find(m, type);
    if (!a || a->len != 8)
        return -1;
    *out = rtc_rd64(m->buf + a->off);
    return 0;
}

int rtc_stun_error_code(const rtc_stun_msg* m)
{
    const rtc_stun_attr* a = rtc_stun_find(m, RTC_SA_ERROR_CODE);
    const uint8_t* v;
    if (!a || a->len < 4)
        return -1;
    v = m->buf + a->off;
    return (v[2] & 7) * 100 + v[3];
}

void rtc_stun_wr_init(rtc_stun_wr* w, uint8_t* buf, size_t cap, uint16_t type, const uint8_t tid[12])
{
    w->buf = buf;
    w->cap = cap;
    w->len = 20;
    w->err = cap < 20;
    if (w->err)
        return;
    rtc_wr16(buf, type);
    rtc_wr16(buf + 2, 0);
    rtc_wr32(buf + 4, RTC_STUN_MAGIC);
    memcpy(buf + 8, tid, 12);
}

uint8_t* rtc_stun_wr_attr(rtc_stun_wr* w, uint16_t type, const void* data, size_t len)
{
    size_t padded = (len + 3) & ~(size_t)3;
    uint8_t* v;
    if (w->err || len > 0xFFFF || w->cap - w->len < 4 + padded) {
        w->err = 1;
        return NULL;
    }
    rtc_wr16(w->buf + w->len, type);
    rtc_wr16(w->buf + w->len + 2, (uint32_t)len);
    v = w->buf + w->len + 4;
    if (data && len)
        memcpy(v, data, len);
    if (padded > len)
        memset(v + len, 0, padded - len);
    w->len += 4 + padded;
    rtc_wr16(w->buf + 2, (uint32_t)(w->len - 20));
    return v;
}

void rtc_stun_wr_u32(rtc_stun_wr* w, uint16_t type, uint32_t v)
{
    uint8_t b[4];
    rtc_wr32(b, v);
    rtc_stun_wr_attr(w, type, b, 4);
}

void rtc_stun_wr_u64(rtc_stun_wr* w, uint16_t type, uint64_t v)
{
    uint8_t b[8];
    rtc_wr64(b, v);
    rtc_stun_wr_attr(w, type, b, 8);
}

void rtc_stun_wr_addr(rtc_stun_wr* w, uint16_t type, const rtc_addr* a)
{
    uint8_t b[20];
    size_t n = a->fam == 4 ? 4 : 16, i;
    int x = type != RTC_SA_MAPPED_ADDRESS;
    b[0] = 0;
    b[1] = a->fam == 4 ? 1 : 2;
    rtc_wr16(b + 2, x ? (uint32_t)(a->port ^ (RTC_STUN_MAGIC >> 16)) : a->port);
    memcpy(b + 4, a->ip, n);
    if (x && !w->err)
        for (i = 0; i < n; i++)
            b[4 + i] ^= w->buf[4 + i];
    rtc_stun_wr_attr(w, type, b, 4 + n);
}

void rtc_stun_wr_error(rtc_stun_wr* w, int code, const char* reason)
{
    uint8_t b[132];
    size_t rl = strlen(reason);
    if (rl > 128)
        rl = 128;
    b[0] = 0;
    b[1] = 0;
    b[2] = (uint8_t)(code / 100);
    b[3] = (uint8_t)(code % 100);
    memcpy(b + 4, reason, rl);
    rtc_stun_wr_attr(w, RTC_SA_ERROR_CODE, b, 4 + rl);
}

void rtc_stun_wr_integrity(rtc_stun_wr* w, const uint8_t* key, size_t keylen)
{
    uint8_t mac[20];
    uint8_t* v;
    size_t upto = w->len;
    v = rtc_stun_wr_attr(w, RTC_SA_MESSAGE_INTEGRITY, NULL, 20);
    if (!v)
        return;
    dyn_hmac(dyn_hash_algo_by_id(DYN_HASH_SHA1), key, keylen, w->buf, upto, mac);
    memcpy(v, mac, 20);
}

void rtc_stun_wr_fingerprint(rtc_stun_wr* w)
{
    uint8_t* v;
    size_t upto = w->len;
    v = rtc_stun_wr_attr(w, RTC_SA_FINGERPRINT, NULL, 4);
    if (!v)
        return;
    rtc_wr32(v, dyn_crc32(w->buf, upto) ^ 0x5354554Eu);
}

size_t rtc_stun_wr_done(rtc_stun_wr* w)
{
    return w->err ? 0 : w->len;
}

void rtc_stun_long_term_key(const char* user, const char* realm, const char* pass, uint8_t out[16])
{
    dyn_hash_ctx_t c;
    dyn_hash_init(&c, dyn_hash_algo_by_id(DYN_HASH_MD5));
    dyn_hash_update(&c, (const uint8_t*)user, strlen(user));
    dyn_hash_update(&c, (const uint8_t*)":", 1);
    dyn_hash_update(&c, (const uint8_t*)realm, strlen(realm));
    dyn_hash_update(&c, (const uint8_t*)":", 1);
    dyn_hash_update(&c, (const uint8_t*)pass, strlen(pass));
    dyn_hash_final(&c, out);
}

int rtc_stun_unknown_required(const rtc_stun_msg* m, const uint16_t* known, int nknown, uint16_t* out, int cap)
{
    int i, k, n = 0;
    for (i = 0; i < m->nattr; i++) {
        uint16_t t = m->attr[i].type;
        int hit = t >= 0x8000;
        for (k = 0; !hit && k < nknown; k++)
            hit = known[k] == t;
        for (k = 0; !hit && k < n && k < cap; k++)
            hit = out[k] == t;
        if (hit)
            continue;
        if (n < cap)
            out[n] = t;
        n++;
    }
    return n;
}
