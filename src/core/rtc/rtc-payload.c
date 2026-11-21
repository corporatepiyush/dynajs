#include "rtc-payload.h"

static const uint16_t opus_frame48[32] = { 480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 480, 960, 120,
    240, 480, 960, 120, 240, 480, 960, 120, 240, 480, 960, 120, 240, 480, 960 };

static int opus_len(const uint8_t* p, size_t n, size_t* i, uint32_t* len)
{
    if (*i >= n)
        return -1;
    if (p[*i] < 252) {
        *len = p[(*i)++];
        return 0;
    }
    if (*i + 1 >= n)
        return -1;
    *len = (uint32_t)p[*i] + 4u * p[*i + 1];
    *i += 2;
    return 0;
}

int rtc_opus_parse(const uint8_t* p, size_t n, rtc_opus_info* o)
{
    size_t i = 1, end = n, k;
    uint32_t per, m, l, sum = 0;
    if (!p || n < 1 || n > 65535u)
        return -1;
    memset(o, 0, sizeof(*o));
    o->config = (uint8_t)(p[0] >> 3);
    o->stereo = (uint8_t)((p[0] >> 2) & 1u);
    o->code = (uint8_t)(p[0] & 3u);
    per = opus_frame48[p[0] >> 3];
    switch (o->code) {
    case 0:
        if (n - 1 > RTC_OPUS_MAX_FRAME_BYTES)
            return -1;
        o->nframes = 1;
        o->frame_off[0] = 1;
        o->frame_len[0] = (uint16_t)(n - 1);
        break;
    case 1:
        if ((n - 1) & 1u || (n - 1) / 2 > RTC_OPUS_MAX_FRAME_BYTES)
            return -1;
        o->nframes = 2;
        o->frame_off[0] = 1;
        o->frame_len[0] = o->frame_len[1] = (uint16_t)((n - 1) / 2);
        o->frame_off[1] = (uint16_t)(1 + (n - 1) / 2);
        break;
    case 2:
        if (opus_len(p, n, &i, &l) < 0 || l > n - i || l > RTC_OPUS_MAX_FRAME_BYTES || n - i - l > RTC_OPUS_MAX_FRAME_BYTES)
            return -1;
        o->nframes = 2;
        o->frame_off[0] = (uint16_t)i;
        o->frame_len[0] = (uint16_t)l;
        o->frame_off[1] = (uint16_t)(i + l);
        o->frame_len[1] = (uint16_t)(n - i - l);
        break;
    default:
        if (n < 2)
            return -1;
        m = p[1] & 0x3Fu;
        o->vbr = (uint8_t)(p[1] >> 7);
        i = 2;
        if (m == 0 || m * per > 5760u)
            return -1;
        if (p[1] & 0x40u) {
            uint8_t b;
            do {
                if (i >= end)
                    return -1;
                b = p[i++];
                l = b == 255 ? 254u : b;
                if (l > end - i)
                    return -1;
                end -= l;
                o->padding += l;
            } while (b == 255);
        }
        o->nframes = (uint8_t)m;
        if (o->vbr) {
            for (k = 0; k + 1 < m; k++) {
                if (opus_len(p, end, &i, &l) < 0)
                    return -1;
                o->frame_len[k] = (uint16_t)l;
                sum += l;
            }
            if (sum > end - i || end - i - sum > RTC_OPUS_MAX_FRAME_BYTES)
                return -1;
            o->frame_len[m - 1] = (uint16_t)(end - i - sum);
        } else {
            if ((end - i) % m || (end - i) / m > RTC_OPUS_MAX_FRAME_BYTES)
                return -1;
            for (k = 0; k < m; k++)
                o->frame_len[k] = (uint16_t)((end - i) / m);
        }
        for (k = 0; k < m; k++) {
            o->frame_off[k] = (uint16_t)i;
            i += o->frame_len[k];
        }
        break;
    }
    o->samples = o->nframes * per;
    return (int)o->samples;
}

int rtc_opus_samples(const uint8_t* p, size_t n)
{
    rtc_opus_info o;
    return rtc_opus_parse(p, n, &o);
}

void rtc_g711_pay_init(rtc_g711_pay* s, const uint8_t* samples, size_t n, uint32_t ptime_ms)
{
    s->s = samples;
    s->n = n > UINT32_MAX ? 0 : (uint32_t)n;
    s->off = 0;
    s->per = 8u * (ptime_ms < 10u ? 20u : ptime_ms > 150u ? 150u : ptime_ms);
}

size_t rtc_g711_pay_next(rtc_g711_pay* s, uint8_t* out, size_t cap, uint32_t* ts_delta)
{
    size_t k = s->n - s->off;
    if (s->off >= s->n || cap == 0)
        return 0;
    if (k > s->per)
        k = s->per;
    if (k > cap)
        k = cap;
    memcpy(out, s->s + s->off, k);
    s->off += (uint32_t)k;
    *ts_delta = (uint32_t)k;
    return k;
}

int rtc_vp8_desc_parse(const uint8_t* p, size_t n, rtc_vp8_desc* d)
{
    size_t i = 1;
    uint8_t b;
    if (n < 2)
        return -1;
    b = p[0];
    d->non_ref = (uint8_t)((b >> 5) & 1u);
    d->start = (uint8_t)((b >> 4) & 1u);
    d->partition = (uint8_t)(b & 7u);
    d->picture_id = -1;
    d->tl0picidx = -1;
    d->tid = -1;
    d->keyidx = -1;
    d->layer_sync = 0;
    if (b & 0x80u) {
        uint8_t x = p[i++];
        if (x & 0x80u) {
            if (i >= n)
                return -1;
            if (p[i] & 0x80u) {
                if (i + 1 >= n)
                    return -1;
                d->picture_id = (int32_t)(((p[i] & 0x7Fu) << 8) | p[i + 1]);
                i += 2;
            } else
                d->picture_id = p[i++];
        }
        if (x & 0x40u) {
            if (i >= n)
                return -1;
            d->tl0picidx = p[i++];
        }
        if (x & 0x30u) {
            uint8_t c;
            if (i >= n)
                return -1;
            c = p[i++];
            if (x & 0x20u) {
                d->tid = (int8_t)(c >> 6);
                d->layer_sync = (uint8_t)((c >> 5) & 1u);
            }
            if (x & 0x10u)
                d->keyidx = (int8_t)(c & 0x1Fu);
        }
    }
    if (i >= n)
        return -1;
    d->hlen = (uint8_t)i;
    return (int)i;
}

int rtc_vp8_is_keyframe(const uint8_t* p, size_t n)
{
    rtc_vp8_desc d;
    if (rtc_vp8_desc_parse(p, n, &d) < 0)
        return 0;
    return d.start && d.partition == 0 && !(p[d.hlen] & 1u);
}

int rtc_vp8_hdr_parse(const uint8_t* f, size_t n, rtc_vp8_hdr* h)
{
    uint32_t tag;
    if (n < 3)
        return -1;
    tag = (uint32_t)f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16;
    memset(h, 0, sizeof(*h));
    h->key = (uint8_t)!(tag & 1u);
    h->version = (uint8_t)((tag >> 1) & 7u);
    h->show = (uint8_t)((tag >> 4) & 1u);
    h->first_part = tag >> 5;
    h->hlen = 3;
    if (h->version > 3)
        return -1;
    if (h->key) {
        if (n < 10 || f[3] != 0x9d || f[4] != 0x01 || f[5] != 0x2a)
            return -1;
        h->width = (uint16_t)((f[6] | f[7] << 8) & 0x3FFF);
        h->hscale = (uint8_t)(f[7] >> 6);
        h->height = (uint16_t)((f[8] | f[9] << 8) & 0x3FFF);
        h->vscale = (uint8_t)(f[9] >> 6);
        h->hlen = 10;
    }
    if (h->first_part > n - h->hlen)
        return -1;
    return h->hlen;
}

void rtc_vp8_depay_reset(rtc_vp8_depay* s)
{
    s->picture_id = -1;
    s->in_frame = 0;
    s->keyframe = 0;
}

int rtc_vp8_depay_push(rtc_vp8_depay* s, const uint8_t* p, size_t n, uint8_t* out, size_t cap, size_t* len)
{
    rtc_vp8_desc d;
    size_t pl;
    if (rtc_vp8_desc_parse(p, n, &d) < 0 || *len > cap)
        return -1;
    pl = n - d.hlen;
    if (!s->in_frame) {
        if (!d.start || d.partition != 0)
            return -1;
    } else if ((d.start && d.partition == 0) || d.picture_id != s->picture_id)
        return -1;
    if (cap - *len < pl)
        return -2;
    if (!s->in_frame) {
        s->in_frame = 1;
        s->picture_id = d.picture_id;
        s->keyframe = (uint8_t)!(p[d.hlen] & 1u);
    }
    memcpy(out + *len, p + d.hlen, pl);
    *len += pl;
    return 0;
}

size_t rtc_vp8_desc_write(uint8_t* out, size_t cap, int start, int32_t picture_id)
{
    if (picture_id < 0) {
        if (cap < 1)
            return 0;
        out[0] = (uint8_t)(start ? 0x10u : 0);
        return 1;
    }
    if (cap < 4)
        return 0;
    out[0] = (uint8_t)(0x80u | (start ? 0x10u : 0));
    out[1] = 0x80u;
    out[2] = (uint8_t)(0x80u | ((uint32_t)picture_id >> 8 & 0x7Fu));
    out[3] = (uint8_t)picture_id;
    return 4;
}

void rtc_vp8_pay_init(rtc_vp8_pay* s, const uint8_t* frame, size_t n, size_t mtu, int32_t picture_id)
{
    s->f = frame;
    s->n = n > UINT32_MAX ? 0 : (uint32_t)n;
    s->off = 0;
    s->mtu = (uint32_t)(mtu > 65535u ? 65535u : mtu < RTC_PAY_MIN_MTU ? RTC_PAY_MIN_MTU : mtu);
    s->picture_id = picture_id < 0 ? -1 : picture_id & 0x7FFF;
}

size_t rtc_vp8_pay_next(rtc_vp8_pay* s, uint8_t* out, size_t cap, int* last)
{
    size_t mtu = s->mtu < cap ? s->mtu : cap, dl, chunk;
    if (s->off >= s->n || mtu < RTC_PAY_MIN_MTU)
        return 0;
    dl = rtc_vp8_desc_write(out, mtu, s->off == 0, s->picture_id);
    chunk = s->n - s->off;
    if (chunk > mtu - dl)
        chunk = mtu - dl;
    memcpy(out + dl, s->f + s->off, chunk);
    s->off += (uint32_t)chunk;
    *last = s->off == s->n;
    return dl + chunk;
}

static uint32_t h264_find_sc(const uint8_t* f, uint32_t n, uint32_t i)
{
    for (;;) {
        const uint8_t* q;
        uint32_t j;
        if (i + 3 > n)
            return n;
        q = memchr(f + i + 2, 1, n - i - 2);
        if (!q)
            return n;
        j = (uint32_t)(q - f);
        if (f[j - 1] == 0 && f[j - 2] == 0)
            return j - 2;
        i = j - 1;
    }
}

int rtc_h264_next_nal(const uint8_t* f, size_t n, uint32_t* pos, uint32_t* start, uint32_t* len)
{
    uint32_t nn, s, e;
    if (n > UINT32_MAX)
        return 0;
    nn = (uint32_t)n;
    for (;;) {
        s = h264_find_sc(f, nn, *pos);
        if (s >= nn) {
            *pos = nn;
            return 0;
        }
        s += 3;
        e = h264_find_sc(f, nn, s);
        *pos = e;
        while (e > s && f[e - 1] == 0)
            e--;
        if (e > s) {
            *start = s;
            *len = e - s;
            return 1;
        }
    }
}

void rtc_h264_pay_init(rtc_h264_pay* s, const uint8_t* frame, size_t n, size_t mtu)
{
    memset(s, 0, sizeof(*s));
    s->f = frame;
    s->n = n > UINT32_MAX ? 0 : (uint32_t)n;
    s->mtu = (uint32_t)(mtu > 65535u ? 65535u : mtu < RTC_PAY_MIN_MTU ? RTC_PAY_MIN_MTU : mtu);
}

static int h264_more(const rtc_h264_pay* s)
{
    uint32_t p = s->pos, st, ln;
    return s->in_fu || rtc_h264_next_nal(s->f, s->n, &p, &st, &ln);
}

static size_t h264_fu(rtc_h264_pay* s, uint8_t* out, size_t mtu, int* last)
{
    uint32_t chunk = s->fu_len - s->fu_at;
    if (chunk > mtu - 2)
        chunk = (uint32_t)(mtu - 2);
    out[0] = (uint8_t)((s->fu_hdr & 0xE0u) | 28u);
    out[1] = (uint8_t)((s->fu_at == 0 ? 0x80u : 0) | (s->fu_at + chunk == s->fu_len ? 0x40u : 0) | (s->fu_hdr & 0x1Fu));
    memcpy(out + 2, s->f + s->fu_off + s->fu_at, chunk);
    s->fu_at += chunk;
    if (s->fu_at == s->fu_len)
        s->in_fu = 0;
    *last = !h264_more(s);
    return (size_t)chunk + 2;
}

size_t rtc_h264_pay_next(rtc_h264_pay* s, uint8_t* out, size_t cap, int* last)
{
    size_t mtu = s->mtu < cap ? s->mtu : cap;
    uint32_t p, st, ln, q, st2, ln2, total, cnt, end;
    uint8_t fbit, nri;
    if (mtu < RTC_PAY_MIN_MTU)
        return 0;
    if (s->in_fu)
        return h264_fu(s, out, mtu, last);
    p = s->pos;
    if (!rtc_h264_next_nal(s->f, s->n, &p, &st, &ln))
        return 0;
    if (ln > mtu) {
        s->in_fu = 1;
        s->fu_hdr = s->f[st];
        s->fu_off = st + 1;
        s->fu_len = ln - 1;
        s->fu_at = 0;
        s->pos = p;
        return h264_fu(s, out, mtu, last);
    }
    q = p;
    end = p;
    total = 3 + ln;
    cnt = 1;
    fbit = s->f[st] & 0x80u;
    nri = s->f[st] & 0x60u;
    while (rtc_h264_next_nal(s->f, s->n, &q, &st2, &ln2) && total + 2 + ln2 <= mtu) {
        total += 2 + ln2;
        cnt++;
        end = q;
        fbit |= s->f[st2] & 0x80u;
        if ((s->f[st2] & 0x60u) > nri)
            nri = s->f[st2] & 0x60u;
    }
    if (cnt == 1) {
        memcpy(out, s->f + st, ln);
        s->pos = p;
        *last = !h264_more(s);
        return ln;
    }
    out[0] = (uint8_t)(fbit | nri | 24u);
    total = 1;
    while (cnt--) {
        rtc_h264_next_nal(s->f, s->n, &s->pos, &st2, &ln2);
        rtc_wr16(out + total, ln2);
        memcpy(out + total + 2, s->f + st2, ln2);
        total += 2 + ln2;
    }
    s->pos = end;
    *last = !h264_more(s);
    return total;
}

int rtc_h264_is_keyframe(const uint8_t* p, size_t n)
{
    size_t off;
    if (n < 1)
        return 0;
    switch (p[0] & 0x1Fu) {
    case 5:
    case 7:
        return 1;
    case 24:
        for (off = 1; off + 3 <= n;) {
            size_t l = rtc_rd16(p + off);
            if (!l || off + 2 + l > n)
                return 0;
            if ((p[off + 2] & 0x1Fu) == 5 || (p[off + 2] & 0x1Fu) == 7)
                return 1;
            off += 2 + l;
        }
        return 0;
    case 28:
        return n >= 2 && (p[1] & 0x80u) && ((p[1] & 0x1Fu) == 5 || (p[1] & 0x1Fu) == 7);
    default:
        return 0;
    }
}

int rtc_h264_start_hint(const uint8_t* p, size_t n)
{
    unsigned t;
    if (n < 2)
        return -1;
    t = p[0] & 0x1Fu;
    if (t == 24 && n >= 4)
        t = p[3] & 0x1Fu;
    else if (t == 28)
        t = (p[1] & 0x80u) ? p[1] & 0x1Fu : 0;
    return t == 7 || t == 9 ? 1 : -1;
}

void rtc_h264_depay_reset(rtc_h264_depay* s)
{
    memset(s, 0, sizeof(*s));
}

static int h264_put(uint8_t* out, size_t cap, size_t* len, int sc, const uint8_t* hdr, const uint8_t* p, size_t n)
{
    size_t need = (sc ? 4u : 0) + (hdr ? 1u : 0) + n;
    if (cap - *len < need)
        return -2;
    if (sc) {
        memcpy(out + *len, "\0\0\0\1", 4);
        *len += 4;
    }
    if (hdr)
        out[(*len)++] = *hdr;
    memcpy(out + *len, p, n);
    *len += n;
    return 0;
}

int rtc_h264_depay_push(rtc_h264_depay* s, const uint8_t* p, size_t n, uint8_t* out, size_t cap, size_t* len)
{
    unsigned t;
    size_t off, need;
    if (n < 1 || (p[0] & 0x80u) || *len > cap)
        return -1;
    t = p[0] & 0x1Fu;
    if (t >= 1 && t <= 23) {
        if (s->in_fu)
            return -1;
        if (t == 5)
            s->keyframe = 1;
        return h264_put(out, cap, len, 1, NULL, p, n);
    }
    if (t == 24) {
        if (s->in_fu)
            return -1;
        need = 0;
        for (off = 1; off < n;) {
            size_t l;
            unsigned it;
            if (off + 2 > n)
                return -1;
            l = rtc_rd16(p + off);
            if (!l || off + 2 + l > n)
                return -1;
            it = p[off + 2] & 0x1Fu;
            if (it == 0 || it > 23 || (p[off + 2] & 0x80u))
                return -1;
            need += 4 + l;
            off += 2 + l;
        }
        if (n < 4)
            return -1;
        if (cap - *len < need)
            return -2;
        for (off = 1; off < n;) {
            size_t l = rtc_rd16(p + off);
            if ((p[off + 2] & 0x1Fu) == 5)
                s->keyframe = 1;
            (void)h264_put(out, cap, len, 1, NULL, p + off + 2, l);
            off += 2 + l;
        }
        return 0;
    }
    if (t == 28) {
        uint8_t fh, nt, hdr;
        int r;
        if (n < 3)
            return -1;
        fh = p[1];
        nt = fh & 0x1Fu;
        if ((fh & 0xC0u) == 0xC0u || nt == 0 || nt > 23)
            return -1;
        if (fh & 0x80u) {
            if (s->in_fu)
                return -1;
            hdr = (uint8_t)((p[0] & 0xE0u) | nt);
            if ((r = h264_put(out, cap, len, 1, &hdr, p + 2, n - 2)) < 0)
                return r;
            s->in_fu = 1;
            s->fu_type = nt;
            if (nt == 5)
                s->keyframe = 1;
        } else {
            if (!s->in_fu || nt != s->fu_type)
                return -1;
            if ((r = h264_put(out, cap, len, 0, NULL, p + 2, n - 2)) < 0)
                return r;
        }
        if (fh & 0x40u)
            s->in_fu = 0;
        return 0;
    }
    return -1;
}
