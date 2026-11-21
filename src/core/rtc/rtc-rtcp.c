#include "rtc-rtcp.h"

void rtc_rtcp_iter_init(rtc_rtcp_iter* it, const uint8_t* p, size_t n)
{
    it->p = p;
    it->n = n;
    it->off = 0;
}

int rtc_rtcp_next(rtc_rtcp_iter* it, rtc_rtcp_pkt* out)
{
    const uint8_t* p = it->p + it->off;
    size_t len, rest = it->n - it->off;
    if (!rest)
        return 0;
    if (rest < 4 || (p[0] >> 6) != 2)
        return -1;
    len = ((size_t)rtc_rd16(p + 2) + 1) * 4;
    if (len > rest)
        return -1;
    out->p = p;
    out->n = len;
    if (p[0] & 0x20u) {
        size_t pad = p[len - 1];
        if (pad == 0 || pad > len - 4 || it->off + len != it->n)
            return -1;
        out->n = len - pad;
    }
    out->pt = p[1];
    out->count = p[0] & 31u;
    it->off += len;
    return 1;
}

int rtc_rtcp_check(const uint8_t* p, size_t n)
{
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    int r, any = 0;
    rtc_rtcp_iter_init(&it, p, n);
    while ((r = rtc_rtcp_next(&it, &k)) == 1) {
        if (k.pt < 192 || k.pt > 223)
            return -1;
        if ((k.pt == RTC_RTCP_SR && k.n < 28 + 24u * k.count) || (k.pt == RTC_RTCP_RR && k.n < 8 + 24u * k.count))
            return -1;
        any = 1;
    }
    return r == 0 && any ? 0 : -1;
}

int rtc_rtcp_sr_parse(const rtc_rtcp_pkt* k, rtc_rtcp_sr* sr)
{
    if (k->pt != RTC_RTCP_SR || k->n < 28 + 24u * k->count)
        return -1;
    sr->ssrc = rtc_rd32(k->p + 4);
    sr->ntp = rtc_rd64(k->p + 8);
    sr->rtp_ts = rtc_rd32(k->p + 16);
    sr->pkts = rtc_rd32(k->p + 20);
    sr->octets = rtc_rd32(k->p + 24);
    return 0;
}

int rtc_rtcp_rr_ssrc(const rtc_rtcp_pkt* k, uint32_t* ssrc)
{
    if (k->pt != RTC_RTCP_RR || k->n < 8 + 24u * k->count)
        return -1;
    *ssrc = rtc_rd32(k->p + 4);
    return 0;
}

static size_t rtcp_rb_base(const rtc_rtcp_pkt* k)
{
    return k->pt == RTC_RTCP_SR ? 28u : 8u;
}

unsigned rtc_rtcp_rb_count(const rtc_rtcp_pkt* k)
{
    size_t b = rtcp_rb_base(k);
    if ((k->pt != RTC_RTCP_SR && k->pt != RTC_RTCP_RR) || k->n < b + 24u * k->count)
        return 0;
    return k->count;
}

int rtc_rtcp_rb_at(const rtc_rtcp_pkt* k, unsigned i, rtc_rtcp_rb* rb)
{
    const uint8_t* q;
    uint32_t l;
    if (i >= rtc_rtcp_rb_count(k))
        return -1;
    q = k->p + rtcp_rb_base(k) + 24u * i;
    rb->ssrc = rtc_rd32(q);
    rb->fraction = q[4];
    l = rtc_rd24(q + 5);
    rb->lost = (l & 0x800000u) ? (int32_t)l - 0x1000000 : (int32_t)l;
    rb->ext_seq = rtc_rd32(q + 8);
    rb->jitter = rtc_rd32(q + 12);
    rb->lsr = rtc_rd32(q + 16);
    rb->dlsr = rtc_rd32(q + 20);
    return 0;
}

int rtc_rtcp_fb_parse(const rtc_rtcp_pkt* k, rtc_rtcp_fb* fb)
{
    if ((k->pt != RTC_RTCP_RTPFB && k->pt != RTC_RTCP_PSFB) || k->n < 12)
        return -1;
    fb->sender = rtc_rd32(k->p + 4);
    fb->media = rtc_rd32(k->p + 8);
    fb->fci = k->p + 12;
    fb->fci_len = k->n - 12;
    return 0;
}

int rtc_rtcp_fb_check(const rtc_rtcp_pkt* k, const rtc_rtcp_fb* fb)
{
    size_t l = fb->fci_len;
    if (k->pt == RTC_RTCP_RTPFB && k->count == RTC_RTCP_FB_NACK)
        return l && !(l % 4) ? 0 : -1;
    if (k->pt == RTC_RTCP_RTPFB && k->count == RTC_RTCP_FB_TWCC)
        return l >= 8 ? 0 : -1;
    if (k->pt == RTC_RTCP_PSFB && k->count == RTC_RTCP_FB_PLI)
        return l == 0 ? 0 : -1;
    if (k->pt == RTC_RTCP_PSFB && k->count == RTC_RTCP_FB_FIR)
        return l && !(l % 8) ? 0 : -1;
    return 0;
}

unsigned rtc_rtcp_nack_seqs(const rtc_rtcp_fb* fb, uint16_t* out, unsigned max)
{
    size_t off;
    unsigned k = 0;
    for (off = 0; off + 4 <= fb->fci_len && k < max; off += 4) {
        uint16_t pid = rtc_rd16(fb->fci + off), blp = rtc_rd16(fb->fci + off + 2);
        unsigned b;
        out[k++] = pid;
        for (b = 0; b < 16 && k < max; b++)
            if (blp & (1u << b))
                out[k++] = (uint16_t)(pid + b + 1);
    }
    return k;
}

unsigned rtc_rtcp_fir_count(const rtc_rtcp_fb* fb)
{
    return (unsigned)(fb->fci_len / 8);
}

int rtc_rtcp_fir_at(const rtc_rtcp_fb* fb, unsigned i, uint32_t* ssrc, uint8_t* seq)
{
    if (i >= rtc_rtcp_fir_count(fb))
        return -1;
    *ssrc = rtc_rd32(fb->fci + 8u * i);
    *seq = fb->fci[8u * i + 4];
    return 0;
}

int rtc_rtcp_remb(const rtc_rtcp_fb* fb, uint64_t* bps, uint32_t* ssrcs, unsigned max, unsigned* n)
{
    const uint8_t* q = fb->fci;
    unsigned cnt, i, e;
    uint64_t m;
    if (fb->fci_len < 8 || memcmp(q, "REMB", 4) != 0)
        return -1;
    cnt = q[4];
    if (fb->fci_len < 8 + 4u * cnt)
        return -1;
    e = q[5] >> 2;
    m = ((uint64_t)(q[5] & 3u) << 16) | rtc_rd16(q + 6);
    if (e > 46)
        return -1;
    *bps = m << e;
    for (i = 0; i < cnt && i < max; i++)
        ssrcs[i] = rtc_rd32(q + 8 + 4 * i);
    *n = i;
    return 0;
}

unsigned rtc_rtcp_bye_ssrcs(const rtc_rtcp_pkt* k, uint32_t* out, unsigned max)
{
    unsigned i;
    if (k->pt != RTC_RTCP_BYE || k->n < 4 + 4u * k->count)
        return 0;
    for (i = 0; i < k->count && i < max; i++)
        out[i] = rtc_rd32(k->p + 4 + 4 * i);
    return i;
}

int rtc_rtcp_sdes_cname(const rtc_rtcp_pkt* k, uint32_t* ssrc, const uint8_t** name, size_t* len)
{
    size_t off = 4;
    unsigned c;
    if (k->pt != RTC_RTCP_SDES)
        return -1;
    for (c = 0; c < k->count; c++) {
        size_t start = off;
        uint32_t s;
        if (off + 4 > k->n)
            return -1;
        s = rtc_rd32(k->p + off);
        off += 4;
        for (;;) {
            uint8_t t, l;
            if (off >= k->n)
                return -1;
            t = k->p[off];
            if (t == 0)
                break;
            if (off + 2 > k->n)
                return -1;
            l = k->p[off + 1];
            if (off + 2 + l > k->n)
                return -1;
            if (t == 1) {
                *ssrc = s;
                *name = k->p + off + 2;
                *len = l;
                return 0;
            }
            off += 2u + l;
        }
        off++;
        off = start + ((off - start + 3) & ~(size_t)3);
    }
    return -1;
}

static void rtcp_hdr(uint8_t* out, unsigned count, unsigned pt, size_t len)
{
    out[0] = (uint8_t)(0x80u | count);
    out[1] = (uint8_t)pt;
    rtc_wr16(out + 2, (uint32_t)(len / 4 - 1));
}

static void rtcp_put_rb(uint8_t* q, const rtc_rtcp_rb* rb)
{
    int32_t lost = rb->lost;
    if (lost > 0x7FFFFF)
        lost = 0x7FFFFF;
    if (lost < -0x800000)
        lost = -0x800000;
    rtc_wr32(q, rb->ssrc);
    q[4] = rb->fraction;
    rtc_wr24(q + 5, (uint32_t)lost & 0xFFFFFFu);
    rtc_wr32(q + 8, rb->ext_seq);
    rtc_wr32(q + 12, rb->jitter);
    rtc_wr32(q + 16, rb->lsr);
    rtc_wr32(q + 20, rb->dlsr);
}

size_t rtc_rtcp_write_sr(uint8_t* out, size_t cap, const rtc_rtcp_sr* sr, const rtc_rtcp_rb* rb, unsigned nrb)
{
    size_t len = 28 + 24u * nrb;
    unsigned i;
    if (nrb > RTC_RTCP_MAX_RB || len > cap)
        return 0;
    rtcp_hdr(out, nrb, RTC_RTCP_SR, len);
    rtc_wr32(out + 4, sr->ssrc);
    rtc_wr64(out + 8, sr->ntp);
    rtc_wr32(out + 16, sr->rtp_ts);
    rtc_wr32(out + 20, sr->pkts);
    rtc_wr32(out + 24, sr->octets);
    for (i = 0; i < nrb; i++)
        rtcp_put_rb(out + 28 + 24 * i, &rb[i]);
    return len;
}

size_t rtc_rtcp_write_rr(uint8_t* out, size_t cap, uint32_t ssrc, const rtc_rtcp_rb* rb, unsigned nrb)
{
    size_t len = 8 + 24u * nrb;
    unsigned i;
    if (nrb > RTC_RTCP_MAX_RB || len > cap)
        return 0;
    rtcp_hdr(out, nrb, RTC_RTCP_RR, len);
    rtc_wr32(out + 4, ssrc);
    for (i = 0; i < nrb; i++)
        rtcp_put_rb(out + 8 + 24 * i, &rb[i]);
    return len;
}

size_t rtc_rtcp_write_sdes(uint8_t* out, size_t cap, uint32_t ssrc, const char* cname, size_t len)
{
    size_t chunk = (4 + 2 + len + 1 + 3) & ~(size_t)3, total = 4 + chunk, off;
    if (len > 255 || total > cap)
        return 0;
    rtcp_hdr(out, 1, RTC_RTCP_SDES, total);
    rtc_wr32(out + 4, ssrc);
    out[8] = 1;
    out[9] = (uint8_t)len;
    if (len)
        memcpy(out + 10, cname, len);
    for (off = 10 + len; off < total; off++)
        out[off] = 0;
    return total;
}

size_t rtc_rtcp_write_bye(uint8_t* out, size_t cap, const uint32_t* ssrc, unsigned n, const char* reason, size_t rlen)
{
    size_t len = 4 + 4u * n, off;
    unsigned i;
    if (rlen)
        len += (1 + rlen + 3) & ~(size_t)3;
    if (n > 31 || rlen > 255 || len > cap)
        return 0;
    rtcp_hdr(out, n, RTC_RTCP_BYE, len);
    for (i = 0; i < n; i++)
        rtc_wr32(out + 4 + 4 * i, ssrc[i]);
    off = 4 + 4u * n;
    if (rlen) {
        out[off] = (uint8_t)rlen;
        memcpy(out + off + 1, reason, rlen);
        for (off += 1 + rlen; off < len; off++)
            out[off] = 0;
    }
    return len;
}

static unsigned rtcp_nack_pairs(const uint16_t* seq, unsigned n, uint8_t* fci)
{
    unsigned i = 0, k = 0;
    while (i < n) {
        uint16_t pid = seq[i], blp = 0;
        i++;
        while (i < n) {
            uint16_t d = (uint16_t)(seq[i] - pid);
            if (d == 0) {
                i++;
                continue;
            }
            if (d > 16)
                break;
            blp = (uint16_t)(blp | (1u << (d - 1)));
            i++;
        }
        if (fci) {
            rtc_wr16(fci + 4 * k, pid);
            rtc_wr16(fci + 4 * k + 2, blp);
        }
        k++;
    }
    return k;
}

size_t rtc_rtcp_write_nack(uint8_t* out, size_t cap, uint32_t sender, uint32_t media, const uint16_t* seq, unsigned n)
{
    unsigned pairs = rtcp_nack_pairs(seq, n, NULL);
    size_t len = 12 + 4u * pairs;
    if (!n || len > cap || len > 4u * 65536)
        return 0;
    rtcp_hdr(out, RTC_RTCP_FB_NACK, RTC_RTCP_RTPFB, len);
    rtc_wr32(out + 4, sender);
    rtc_wr32(out + 8, media);
    rtcp_nack_pairs(seq, n, out + 12);
    return len;
}

size_t rtc_rtcp_write_pli(uint8_t* out, size_t cap, uint32_t sender, uint32_t media)
{
    if (cap < 12)
        return 0;
    rtcp_hdr(out, RTC_RTCP_FB_PLI, RTC_RTCP_PSFB, 12);
    rtc_wr32(out + 4, sender);
    rtc_wr32(out + 8, media);
    return 12;
}

size_t rtc_rtcp_write_fir(uint8_t* out, size_t cap, uint32_t sender, uint32_t media, uint8_t seq)
{
    if (cap < 20)
        return 0;
    rtcp_hdr(out, RTC_RTCP_FB_FIR, RTC_RTCP_PSFB, 20);
    rtc_wr32(out + 4, sender);
    rtc_wr32(out + 8, 0);
    rtc_wr32(out + 12, media);
    out[16] = seq;
    out[17] = out[18] = out[19] = 0;
    return 20;
}

size_t rtc_rtcp_write_remb(uint8_t* out, size_t cap, uint32_t sender, uint64_t bps, const uint32_t* ssrc, unsigned n)
{
    size_t len = 20 + 4u * n;
    unsigned e = 0, i;
    if (n > 255 || len > cap)
        return 0;
    while (e < 63 && (bps >> e) >= (1u << 18))
        e++;
    if (e > 46)
        return 0;
    rtcp_hdr(out, RTC_RTCP_FB_AFB, RTC_RTCP_PSFB, len);
    rtc_wr32(out + 4, sender);
    rtc_wr32(out + 8, 0);
    memcpy(out + 12, "REMB", 4);
    out[16] = (uint8_t)n;
    out[17] = (uint8_t)((e << 2) | (uint32_t)((bps >> e) >> 16));
    rtc_wr16(out + 18, (uint32_t)(bps >> e));
    for (i = 0; i < n; i++)
        rtc_wr32(out + 20 + 4 * i, ssrc[i]);
    return len;
}
