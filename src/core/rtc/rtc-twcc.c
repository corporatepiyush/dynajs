#include "rtc-twcc.h"

void rtc_twcc_rx_init(rtc_twcc_rx* r)
{
    uint32_t i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < RTC_TWCC_WIN; i++)
        r->at_us[i] = RTC_TWCC_NONE;
}

void rtc_twcc_rx_on_packet(rtc_twcc_rx* r, uint16_t tseq, int64_t now_us)
{
    uint32_t e;
    if (now_us < 0)
        return;
    if (!r->init) {
        e = 0x10000u | tseq;
        r->init = 1;
        r->base_ext = e;
        r->newest_ext = e;
    } else {
        e = r->newest_ext + (uint32_t)(int32_t)(int16_t)(uint16_t)(tseq - (uint16_t)r->newest_ext);
        if ((int32_t)(e - r->base_ext) < 0)
            return;
        if ((int32_t)(e - r->newest_ext) > 0) {
            if (e - r->base_ext >= RTC_TWCC_WIN) {
                uint32_t nb = e - RTC_TWCC_WIN + 1, x;
                for (x = r->base_ext; x != nb && x - r->base_ext < RTC_TWCC_WIN; x++)
                    r->at_us[x & (RTC_TWCC_WIN - 1)] = RTC_TWCC_NONE;
                r->base_ext = nb;
            }
            r->newest_ext = e;
        }
    }
    r->at_us[e & (RTC_TWCC_WIN - 1)] = now_us;
}

static uint8_t twcc_sym(int64_t d)
{
    return d >= 0 && d <= 255 ? 1 : d >= -32768 && d <= 32767 ? 2 : 3;
}

size_t rtc_twcc_rx_build(rtc_twcc_rx* r, uint8_t* out, size_t cap, uint32_t sender, uint32_t media)
{
    uint8_t sym[RTC_TWCC_MAX_STATUS];
    int64_t dl[RTC_TWCC_MAX_STATUS], ref = 0, prev = 0;
    uint32_t count = 0, i, dbytes = 0, e;
    size_t len, off, doff;
    int have_ref = 0;
    if (!r->init || (int32_t)(r->newest_ext - r->base_ext) < 0 || cap < 24)
        return 0;
    for (e = r->base_ext; e != r->newest_ext && r->at_us[e & (RTC_TWCC_WIN - 1)] == RTC_TWCC_NONE; e++)
        ;
    if (e - r->base_ext >= RTC_TWCC_MAX_STATUS)
        r->base_ext = e;
    for (e = r->base_ext; e != r->newest_ext + 1 && count < RTC_TWCC_MAX_STATUS; e++) {
        int64_t at = r->at_us[e & (RTC_TWCC_WIN - 1)];
        uint8_t s = 0;
        int64_t d = 0;
        if (at != RTC_TWCC_NONE) {
            int64_t ticks = at / 250;
            if (!have_ref) {
                ref = at / 64000;
                prev = ref * 256;
                have_ref = 1;
            }
            d = ticks - prev;
            s = twcc_sym(d);
            if (s == 3)
                break;
            if (20 + 2 * (size_t)(count + 1) + dbytes + s + 3 > cap)
                break;
            prev = ticks;
        } else if (20 + 2 * (size_t)(count + 1) + dbytes + 3 > cap)
            break;
        sym[count] = s;
        dl[count] = d;
        dbytes += s;
        count++;
    }
    if (!count || !have_ref)
        return 0;
    off = 20;
    for (i = 0; i < count;) {
        uint32_t run = 1, n, k;
        int two = 0;
        while (i + run < count && sym[i + run] == sym[i] && run < 8191)
            run++;
        if (run >= 14 || i + run == count) {
            rtc_wr16(out + off, (uint32_t)sym[i] << 13 | run);
            i += run;
        } else {
            n = count - i < 14 ? count - i : 14;
            for (k = 0; k < n; k++)
                two |= sym[i + k] == 2;
            if (!two) {
                uint32_t v = 0x8000u;
                for (k = 0; k < n; k++)
                    v |= (uint32_t)sym[i + k] << (13 - k);
                rtc_wr16(out + off, v);
                i += n;
            } else {
                uint32_t v = 0xC000u;
                n = n < 7 ? n : 7;
                for (k = 0; k < n; k++)
                    v |= (uint32_t)sym[i + k] << (12 - 2 * k);
                rtc_wr16(out + off, v);
                i += n;
            }
        }
        off += 2;
    }
    doff = off;
    for (i = 0; i < count; i++) {
        if (sym[i] == 1)
            out[doff++] = (uint8_t)dl[i];
        else if (sym[i] == 2) {
            rtc_wr16(out + doff, (uint32_t)(uint16_t)(int16_t)dl[i]);
            doff += 2;
        }
    }
    len = (doff + 3) & ~(size_t)3;
    if (len > cap)
        return 0;
    memset(out + doff, 0, len - doff);
    out[0] = (uint8_t)(0x80u | RTC_RTCP_FB_TWCC);
    out[1] = RTC_RTCP_RTPFB;
    rtc_wr16(out + 2, (uint32_t)(len / 4 - 1));
    rtc_wr32(out + 4, sender);
    rtc_wr32(out + 8, media);
    rtc_wr16(out + 12, r->base_ext & 0xFFFFu);
    rtc_wr16(out + 14, count);
    rtc_wr24(out + 16, (uint32_t)ref & 0xFFFFFFu);
    out[19] = r->fb_count++;
    for (e = r->base_ext, i = 0; i < count; i++, e++)
        r->at_us[e & (RTC_TWCC_WIN - 1)] = RTC_TWCC_NONE;
    r->base_ext += count;
    return len;
}

int rtc_twcc_parse(const rtc_rtcp_fb* fb, rtc_twcc_fb* h, rtc_twcc_pkt* out, unsigned max)
{
    const uint8_t* q = fb->fci;
    size_t n = fb->fci_len, off = 8;
    uint32_t count, got = 0, i, ref24;
    int64_t t;
    if (n < 8)
        return -1;
    h->base = rtc_rd16(q);
    h->count = rtc_rd16(q + 2);
    ref24 = rtc_rd24(q + 4);
    h->fb_count = q[7];
    h->ref_us = (int64_t)((ref24 & 0x800000u) ? (int32_t)ref24 - 0x1000000 : (int32_t)ref24) * 64000;
    count = h->count;
    if (count > max)
        return -1;
    while (got < count) {
        uint32_t c;
        if (off + 2 > n)
            return -1;
        c = rtc_rd16(q + off);
        off += 2;
        if (!(c & 0x8000u)) {
            uint32_t s = (c >> 13) & 3u, run = c & 0x1FFFu;
            if (!run || s == 3)
                return -1;
            for (i = 0; i < run && got < count; i++, got++)
                out[got].received = (uint8_t)s;
        } else if (!(c & 0x4000u)) {
            for (i = 0; i < 14 && got < count; i++, got++)
                out[got].received = (uint8_t)((c >> (13 - i)) & 1u);
        } else {
            for (i = 0; i < 7 && got < count; i++, got++) {
                uint32_t s = (c >> (12 - 2 * i)) & 3u;
                if (s == 3)
                    return -1;
                out[got].received = (uint8_t)s;
            }
        }
    }
    t = h->ref_us;
    for (i = 0; i < count; i++) {
        uint8_t s = out[i].received;
        out[i].seq = (uint16_t)(h->base + i);
        out[i].at_us = 0;
        if (s == 1) {
            if (off + 1 > n)
                return -1;
            t += (int64_t)q[off++] * 250;
        } else if (s == 2) {
            if (off + 2 > n)
                return -1;
            t += (int64_t)(int16_t)rtc_rd16(q + off) * 250;
            off += 2;
        }
        if (s) {
            out[i].at_us = t;
            out[i].received = 1;
        }
    }
    return (int)count;
}

void rtc_twcc_ext_put(uint8_t* out, uint16_t tseq)
{
    rtc_wr16(out, tseq);
}

int rtc_twcc_ext_get(const uint8_t* pkt, const rtc_rtp_hdr* h, uint8_t id, uint16_t* tseq)
{
    const uint8_t* v = NULL;
    size_t l = 0;
    if (rtc_rtp_ext_get(pkt, h, id, &v, &l) <= 0 || l != 2)
        return -1;
    *tseq = rtc_rd16(v);
    return 0;
}
