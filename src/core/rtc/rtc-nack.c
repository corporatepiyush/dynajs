#include "rtc-nack.h"

void rtc_nack_init(rtc_nack* k)
{
    memset(k, 0, sizeof(*k));
}

static void nack_remove_at(rtc_nack* k, uint32_t i)
{
    memmove(&k->e[i], &k->e[i + 1], (k->n - i - 1) * sizeof(k->e[0]));
    k->n--;
}

static void nack_drop_old(rtc_nack* k)
{
    uint32_t i = 0, w;
    while (i < k->n && k->newest - k->e[i].ext > RTC_NACK_MAX_AGE)
        i++;
    if (!i)
        return;
    for (w = 0; i < k->n; i++, w++)
        k->e[w] = k->e[i];
    k->n = w;
}

int rtc_nack_on_packet(rtc_nack* k, uint16_t seq)
{
    uint32_t ext, i, gap;
    if (!k->init) {
        k->newest = 0x10000u | seq;
        k->init = 1;
        return 0;
    }
    ext = k->newest + (uint32_t)(int32_t)(int16_t)(uint16_t)(seq - (uint16_t)k->newest);
    if ((int32_t)(ext - k->newest) <= 0) {
        for (i = 0; i < k->n; i++)
            if (k->e[i].ext == ext) {
                nack_remove_at(k, i);
                k->recovered++;
                return 1;
            }
        return 0;
    }
    gap = ext - k->newest - 1;
    if (gap > RTC_NACK_MAX || k->n + gap > RTC_NACK_MAX) {
        k->n = 0;
        k->want_key = 1;
        k->newest = ext;
        return 0;
    }
    for (i = 1; i <= gap; i++) {
        k->e[k->n].ext = k->newest + i;
        k->e[k->n].sent_ms = 0;
        k->e[k->n].tries = 0;
        k->n++;
        k->unique++;
    }
    k->newest = ext;
    nack_drop_old(k);
    return 0;
}

unsigned rtc_nack_poll(rtc_nack* k, uint32_t now_ms, uint32_t rtt_ms, uint16_t* out, unsigned max)
{
    uint32_t i = 0, interval = rtt_ms < RTC_NACK_MIN_INTERVAL_MS ? RTC_NACK_MIN_INTERVAL_MS : rtt_ms;
    unsigned c = 0;
    while (i < k->n && c < max) {
        rtc_nack_ent* e = &k->e[i];
        if (e->tries && (int32_t)(now_ms - e->sent_ms) < (int32_t)interval) {
            i++;
            continue;
        }
        if (e->tries >= RTC_NACK_MAX_TRIES) {
            nack_remove_at(k, i);
            continue;
        }
        e->tries++;
        e->sent_ms = now_ms;
        out[c++] = (uint16_t)e->ext;
        k->requested++;
        i++;
    }
    return c;
}

int rtc_nack_take_keyreq(rtc_nack* k)
{
    int r = k->want_key;
    k->want_key = 0;
    return r;
}

unsigned rtc_nack_answer(const rtc_pktring* h, const uint16_t* seq, unsigned n, const uint8_t** pkt, size_t* len)
{
    unsigned i, found = 0;
    for (i = 0; i < n; i++) {
        const rtc_pkt_slot* s = NULL;
        const uint8_t* p = rtc_pktring_get(h, seq[i], &s);
        pkt[i] = p;
        len[i] = p ? s->len : 0;
        found += p != NULL;
    }
    return found;
}

void rtc_keyreq_init(rtc_keyreq* k, uint32_t gap_ms, uint8_t first_seq)
{
    memset(k, 0, sizeof(*k));
    k->gap_ms = gap_ms;
    k->next_seq = first_seq;
}

void rtc_keyreq_want(rtc_keyreq* k)
{
    if (k->pending)
        return;
    k->pending = 1;
    k->active = 0;
}

void rtc_keyreq_done(rtc_keyreq* k)
{
    k->pending = 0;
    k->active = 0;
}

size_t rtc_keyreq_poll(rtc_keyreq* k, uint32_t now_ms, int fir, uint32_t sender, uint32_t media, uint8_t* out, size_t cap)
{
    size_t n;
    if (!k->pending || (k->active && (int32_t)(now_ms - k->sent_ms) < (int32_t)k->gap_ms))
        return 0;
    if (!k->active)
        k->cur_seq = k->next_seq;
    n = fir ? rtc_rtcp_write_fir(out, cap, sender, media, k->cur_seq) : rtc_rtcp_write_pli(out, cap, sender, media);
    if (!n)
        return 0;
    if (!k->active)
        k->next_seq++;
    k->active = 1;
    k->sent_ms = now_ms;
    k->sent++;
    return n;
}

int rtc_fir_rx_new(rtc_fir_rx* r, uint8_t seq)
{
    if (r->has && r->seq == seq)
        return 0;
    r->has = 1;
    r->seq = seq;
    return 1;
}
