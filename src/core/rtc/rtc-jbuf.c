#include <stdlib.h>

#include "rtc-jbuf.h"

int rtc_jb_init(rtc_jb* j, uint32_t nslot, size_t bytes, int audio)
{
    memset(j, 0, sizeof(*j));
    if (nslot < 4 || (nslot & (nslot - 1)) || nslot > 32768u)
        return -1;
    j->slot = calloc(nslot, sizeof(*j->slot));
    if (!j->slot || rtc_pktring_init(&j->ring, bytes, nslot) < 0) {
        rtc_jb_free(j);
        return -1;
    }
    j->mask = nslot - 1;
    j->audio = (uint8_t)(audio != 0);
    j->max_wait_ms = 1000;
    return 0;
}

void rtc_jb_free(rtc_jb* j)
{
    free(j->slot);
    rtc_pktring_free(&j->ring);
    memset(j, 0, sizeof(*j));
}

void rtc_jb_set_target(rtc_jb* j, uint32_t target_ms, uint32_t max_wait_ms)
{
    j->target_ms = target_ms;
    j->max_wait_ms = max_wait_ms;
}

static const rtc_jb_slot* jb_get(const rtc_jb* j, uint32_t e)
{
    const rtc_jb_slot* s = &j->slot[e & j->mask];
    return s->used && s->ext == e ? s : NULL;
}

static int jb_is_start(const rtc_jb* j, uint32_t e, const rtc_jb_slot* s)
{
    const rtc_jb_slot* p;
    if (s->start != RTC_JB_START_UNKNOWN)
        return s->start;
    if (e == j->next_ext && j->head_known)
        return 1;
    p = jb_get(j, e - 1);
    return p && (p->marker || p->ts != s->ts);
}

static int jb_frame_at(const rtc_jb* j, uint32_t e, rtc_jb_frame* f)
{
    const rtc_jb_slot* s = jb_get(j, e);
    uint32_t k;
    if (!s || !jb_is_start(j, e, s))
        return 0;
    f->ts = s->ts;
    f->first_ext = e;
    f->first_ms = s->at_ms;
    f->npkts = 0;
    f->key = 0;
    for (k = 0; k <= j->mask; k++, e++) {
        const rtc_jb_slot* q = jb_get(j, e);
        if (!q)
            return 0;
        if (q->ts != f->ts) {
            if (!k || !jb_is_start(j, e, q))
                return 0;
            f->last_ext = e - 1;
            return 1;
        }
        if ((int32_t)(q->at_ms - f->first_ms) < 0)
            f->first_ms = q->at_ms;
        f->npkts++;
        f->key |= q->key;
        if (q->marker) {
            f->last_ext = e;
            return 1;
        }
    }
    return 0;
}

static void jb_release(rtc_jb* j, uint32_t from, uint32_t to, int count_drops)
{
    uint32_t e, last_ts = 0;
    int have = 0;
    if (to - from > j->mask) {
        if (count_drops) {
            j->st.frames_dropped++;
            j->want_key = 1;
        }
        memset(j->slot, 0, ((size_t)j->mask + 1) * sizeof(*j->slot));
        return;
    }
    for (e = from; e != to; e++) {
        rtc_jb_slot* s = &j->slot[e & j->mask];
        if (!s->used || s->ext != e)
            continue;
        if (count_drops && (!have || s->ts != last_ts)) {
            j->st.frames_dropped++;
            j->want_key = 1;
        }
        have = 1;
        last_ts = s->ts;
        s->used = 0;
    }
}

int rtc_jb_insert(rtc_jb* j, uint16_t seq, uint32_t ts, int marker, int start, int key, uint32_t now_ms, const uint8_t* p, size_t n)
{
    rtc_jb_slot* s;
    uint32_t e;
    if (!j->slot)
        return -1;
    if (!j->init) {
        e = 0x10000u | seq;
        j->newest_ext = e;
        j->next_ext = e;
        j->init = 1;
        j->head_known = (uint8_t)(start > 0);
    } else {
        e = j->newest_ext + (uint32_t)(int32_t)(int16_t)(uint16_t)(seq - (uint16_t)j->newest_ext);
        if ((int32_t)(e - j->next_ext) < 0) {
            if (j->st.emitted || j->st.frames_dropped || j->next_ext - e > j->mask - (j->newest_ext - j->next_ext)) {
                j->st.late++;
                return -1;
            }
            j->next_ext = e;
            j->head_known = (uint8_t)(start > 0);
        }
        if ((int32_t)(e - j->newest_ext) > 0)
            j->newest_ext = e;
    }
    if (e - j->next_ext > j->mask) {
        uint32_t nh = e - j->mask;
        jb_release(j, j->next_ext, nh, 1);
        j->next_ext = nh;
        j->head_known = 0;
    }
    s = &j->slot[e & j->mask];
    if (s->used && s->ext == e) {
        j->st.dups++;
        return 1;
    }
    if (rtc_pktring_put(&j->ring, seq, ts, marker, now_ms, p, n) < 0)
        return -1;
    s->ext = e;
    s->ts = ts;
    s->at_ms = now_ms;
    s->used = 1;
    s->marker = (uint8_t)(j->audio || marker);
    s->start = (uint8_t)(j->audio ? 1 : start < 0 ? RTC_JB_START_UNKNOWN : start != 0);
    s->key = (uint8_t)(j->audio || key);
    return 0;
}

static int jb_head_stale(const rtc_jb* j, uint32_t now_ms)
{
    uint32_t e, span = j->newest_ext - j->next_ext;
    if (span > j->mask / 2)
        return 1;
    for (e = j->next_ext; e != j->newest_ext + 1; e++) {
        const rtc_jb_slot* s = jb_get(j, e);
        if (s && (int32_t)(now_ms - s->at_ms) >= (int32_t)j->max_wait_ms)
            return 1;
    }
    return 0;
}

static int jb_skip(rtc_jb* j)
{
    uint32_t e;
    rtc_jb_frame f;
    for (e = j->next_ext + 1; e != j->newest_ext + 1; e++) {
        if (!jb_frame_at(j, e, &f) || !f.key)
            continue;
        jb_release(j, j->next_ext, e, 1);
        j->next_ext = e;
        j->head_known = 1;
        return 1;
    }
    return 0;
}

int rtc_jb_pop(rtc_jb* j, uint32_t now_ms, rtc_jb_frame* f)
{
    if (!j->init)
        return 0;
    for (;;) {
        if ((int32_t)(j->newest_ext - j->next_ext) < 0)
            return 0;
        if (jb_frame_at(j, j->next_ext, f)) {
            uint32_t waited = (int32_t)(now_ms - f->first_ms) < 0 ? 0 : now_ms - f->first_ms;
            if (waited < j->target_ms)
                return 0;
            jb_release(j, f->first_ext, f->last_ext + 1, 0);
            j->next_ext = f->last_ext + 1;
            j->head_known = 1;
            j->st.emitted++;
            j->st.delay_ms += waited;
            j->st.target_ms += j->target_ms;
            j->st.multi += f->npkts > 1;
            j->st.keyframes += f->key;
            return 1;
        }
        if (!jb_head_stale(j, now_ms))
            return 0;
        if (!jb_skip(j)) {
            j->want_key = 1;
            return 0;
        }
    }
}

const uint8_t* rtc_jb_pkt(const rtc_jb* j, uint32_t ext, size_t* len)
{
    const rtc_pkt_slot* ps = NULL;
    const uint8_t* p = rtc_pktring_get(&j->ring, (uint16_t)ext, &ps);
    if (!p)
        return NULL;
    *len = ps->len;
    return p;
}

size_t rtc_jb_bytes(const rtc_jb* j)
{
    return j->slot ? ((size_t)j->mask + 1) * sizeof(*j->slot) + rtc_pktring_bytes(&j->ring) : 0;
}

int rtc_jb_take_keyreq(rtc_jb* j)
{
    int r = j->want_key;
    j->want_key = 0;
    return r;
}

int rtc_jb_frame_copy(rtc_jb* j, const rtc_jb_frame* f, int fmt, uint8_t* out, size_t cap, size_t* len)
{
    rtc_vp8_depay v;
    rtc_h264_depay h;
    uint32_t e;
    size_t start = *len;
    rtc_vp8_depay_reset(&v);
    rtc_h264_depay_reset(&h);
    for (e = f->first_ext; e != f->last_ext + 1; e++) {
        size_t l = 0;
        const uint8_t* p = rtc_jb_pkt(j, e, &l);
        int r;
        if (!p)
            r = -1;
        else if (fmt == RTC_JB_FMT_VP8)
            r = rtc_vp8_depay_push(&v, p, l, out, cap, len);
        else if (fmt == RTC_JB_FMT_H264)
            r = rtc_h264_depay_push(&h, p, l, out, cap, len);
        else if (cap - *len < l)
            r = -2;
        else {
            memcpy(out + *len, p, l);
            *len += l;
            r = 0;
        }
        if (r < 0) {
            *len = start;
            if (r == -1)
                j->want_key = 1;
            return r;
        }
    }
    if (fmt == RTC_JB_FMT_H264 && h.in_fu) {
        *len = start;
        j->want_key = 1;
        return -1;
    }
    return 0;
}
