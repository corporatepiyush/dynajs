#include "rtc-rtp.h"

int rtc_rtp_parse(const uint8_t* p, size_t n, rtc_rtp_hdr* h)
{
    size_t hl;
    if (n < RTC_RTP_MIN || n > 65535u || (p[0] >> 6) != 2)
        return -1;
    h->cc = p[0] & 15u;
    h->marker = p[1] >> 7;
    h->pt = p[1] & 127u;
    h->seq = rtc_rd16(p + 2);
    h->ts = rtc_rd32(p + 4);
    h->ssrc = rtc_rd32(p + 8);
    h->ext_profile = 0;
    h->ext_off = 0;
    h->ext_len = 0;
    h->pad = 0;
    hl = RTC_RTP_MIN + 4u * h->cc;
    if (hl > n)
        return -1;
    if (p[0] & 0x10u) {
        size_t el;
        if (hl + 4 > n)
            return -1;
        el = 4u * (size_t)rtc_rd16(p + hl + 2);
        if (hl + 4 + el > n)
            return -1;
        h->ext_profile = rtc_rd16(p + hl);
        h->ext_off = (uint16_t)(hl + 4);
        h->ext_len = (uint16_t)el;
        hl += 4 + el;
    }
    if (p[0] & 0x20u) {
        size_t pad;
        if (hl >= n)
            return -1;
        pad = p[n - 1];
        if (pad == 0 || pad > n - hl)
            return -1;
        h->pad = (uint8_t)pad;
    }
    h->hlen = (uint16_t)hl;
    h->plen = (uint32_t)(n - hl - h->pad);
    return 0;
}

static int rtp_ext_form(const rtc_rtp_hdr* h)
{
    if (!h->ext_len)
        return 0;
    if (h->ext_profile == RTC_RTP_EXT_ONE)
        return 1;
    if ((h->ext_profile & 0xFFF0u) == RTC_RTP_EXT_TWO)
        return 2;
    return 0;
}

static int rtp_ext_next(const uint8_t* p, size_t* off, size_t end, int form, rtc_rtp_ext* e)
{
    while (*off < end) {
        uint8_t b = p[*off];
        size_t l;
        if (b == 0) {
            (*off)++;
            continue;
        }
        if (form == 1) {
            if ((b >> 4) == 15)
                return 0;
            e->id = b >> 4;
            l = (size_t)(b & 15u) + 1;
            (*off)++;
        } else {
            if (*off + 2 > end)
                return 0;
            e->id = b;
            l = p[*off + 1];
            *off += 2;
        }
        if (*off + l > end)
            return 0;
        e->val = p + *off;
        e->len = (uint8_t)l;
        *off += l;
        return 1;
    }
    return 0;
}

unsigned rtc_rtp_ext_list(const uint8_t* p, const rtc_rtp_hdr* h, rtc_rtp_ext* out, unsigned max)
{
    int form = rtp_ext_form(h);
    size_t off = h->ext_off, end = (size_t)h->ext_off + h->ext_len;
    unsigned k = 0;
    if (!form)
        return 0;
    while (k < max && rtp_ext_next(p, &off, end, form, &out[k]))
        k++;
    return k;
}

int rtc_rtp_ext_get(const uint8_t* p, const rtc_rtp_hdr* h, uint8_t id, const uint8_t** val, size_t* len)
{
    int form = rtp_ext_form(h);
    size_t off = h->ext_off, end = (size_t)h->ext_off + h->ext_len;
    rtc_rtp_ext e;
    if (!form || !id)
        return 0;
    while (rtp_ext_next(p, &off, end, form, &e)) {
        if (e.id == id) {
            *val = e.val;
            *len = e.len;
            return 1;
        }
    }
    return 0;
}

uint8_t* rtc_rtp_ext_slot(uint8_t* p, const rtc_rtp_hdr* h, uint8_t id, size_t len)
{
    const uint8_t* v;
    size_t l;
    if (!rtc_rtp_ext_get(p, h, id, &v, &l) || l != len)
        return NULL;
    return p + (v - p);
}

static int rtp_ext_one_byte(const rtc_rtp_ext* ext, unsigned next)
{
    unsigned i;
    for (i = 0; i < next; i++)
        if (ext[i].id < 1 || ext[i].id > 14 || ext[i].len < 1 || ext[i].len > 16)
            return 0;
    return 1;
}

static size_t rtp_ext_body(const rtc_rtp_ext* ext, unsigned next, int one)
{
    size_t s = 0;
    unsigned i;
    for (i = 0; i < next; i++)
        s += (one ? 1u : 2u) + ext[i].len;
    return (s + 3) & ~(size_t)3;
}

size_t rtc_rtp_hdr_size(unsigned cc, const rtc_rtp_ext* ext, unsigned next)
{
    unsigned i;
    if (cc > 15 || next > RTC_RTP_EXT_MAX)
        return 0;
    for (i = 0; i < next; i++)
        if (!ext[i].id)
            return 0;
    return RTC_RTP_MIN + 4u * cc + (next ? 4 + rtp_ext_body(ext, next, rtp_ext_one_byte(ext, next)) : 0);
}

size_t rtc_rtp_write_hdr(uint8_t* out, size_t cap, uint8_t pt, int marker, uint16_t seq, uint32_t ts, uint32_t ssrc,
    const uint32_t* csrc, unsigned cc, const rtc_rtp_ext* ext, unsigned next)
{
    size_t hl = rtc_rtp_hdr_size(cc, ext, next), off;
    unsigned i;
    int one;
    if (!hl || hl > cap || pt > 127)
        return 0;
    out[0] = (uint8_t)(0x80u | (next ? 0x10u : 0) | cc);
    out[1] = (uint8_t)((marker ? 0x80u : 0) | pt);
    rtc_wr16(out + 2, seq);
    rtc_wr32(out + 4, ts);
    rtc_wr32(out + 8, ssrc);
    off = RTC_RTP_MIN;
    for (i = 0; i < cc; i++, off += 4)
        rtc_wr32(out + off, csrc[i]);
    if (!next)
        return hl;
    one = rtp_ext_one_byte(ext, next);
    rtc_wr16(out + off, one ? RTC_RTP_EXT_ONE : RTC_RTP_EXT_TWO);
    rtc_wr16(out + off + 2, (uint32_t)((hl - off - 4) / 4));
    off += 4;
    for (i = 0; i < next; i++) {
        if (one) {
            out[off++] = (uint8_t)((ext[i].id << 4) | (ext[i].len - 1));
        } else {
            out[off++] = ext[i].id;
            out[off++] = ext[i].len;
        }
        if (ext[i].len)
            memcpy(out + off, ext[i].val, ext[i].len);
        off += ext[i].len;
    }
    while (off < hl)
        out[off++] = 0;
    return hl;
}

size_t rtc_rtp_write(uint8_t* out, size_t cap, uint8_t pt, int marker, uint16_t seq, uint32_t ts, uint32_t ssrc,
    const uint32_t* csrc, unsigned cc, const rtc_rtp_ext* ext, unsigned next, const uint8_t* payload, size_t plen)
{
    size_t hl = rtc_rtp_write_hdr(out, cap, pt, marker, seq, ts, ssrc, csrc, cc, ext, next);
    if (!hl || plen > cap - hl)
        return 0;
    if (plen)
        memcpy(out + hl, payload, plen);
    return hl + plen;
}
