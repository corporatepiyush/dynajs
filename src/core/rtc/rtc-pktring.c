#include <stdlib.h>

#include "rtc-pktring.h"

int rtc_pktring_init(rtc_pktring* r, size_t bytes, uint32_t nslot)
{
    memset(r, 0, sizeof(*r));
    if (!bytes || nslot < 2 || (nslot & (nslot - 1)) || nslot > 32768u)
        return -1;
    r->buf = malloc(bytes);
    r->slot = calloc(nslot, sizeof(*r->slot));
    if (!r->buf || !r->slot) {
        rtc_pktring_free(r);
        return -1;
    }
    r->cap = bytes;
    r->mask = nslot - 1;
    return 0;
}

void rtc_pktring_free(rtc_pktring* r)
{
    free(r->buf);
    free(r->slot);
    memset(r, 0, sizeof(*r));
}

void rtc_pktring_clear(rtc_pktring* r)
{
    if (r->slot)
        memset(r->slot, 0, ((size_t)r->mask + 1) * sizeof(*r->slot));
    r->count = 0;
}

static int pktring_live(const rtc_pktring* r, const rtc_pkt_slot* s)
{
    return s->used && r->head - s->off <= r->cap;
}

int rtc_pktring_put(rtc_pktring* r, uint16_t seq, uint32_t ts, int marker, uint32_t at_ms, const uint8_t* p, size_t n)
{
    rtc_pkt_slot* s;
    size_t at;
    if (!r->buf || !n || n > r->cap)
        return -1;
    at = (size_t)(r->head % r->cap);
    if (at + n > r->cap) {
        r->head += r->cap - at;
        at = 0;
    }
    memcpy(r->buf + at, p, n);
    s = &r->slot[seq & r->mask];
    if (!s->used)
        r->count++;
    s->off = r->head;
    s->len = (uint32_t)n;
    s->ts = ts;
    s->at_ms = at_ms;
    s->seq = seq;
    s->used = 1;
    s->marker = (uint8_t)(marker != 0);
    r->head += n;
    return 0;
}

const uint8_t* rtc_pktring_get(const rtc_pktring* r, uint16_t seq, const rtc_pkt_slot** out)
{
    const rtc_pkt_slot* s;
    if (!r->slot)
        return NULL;
    s = &r->slot[seq & r->mask];
    if (!s->used || s->seq != seq || !pktring_live(r, s))
        return NULL;
    if (out)
        *out = s;
    return r->buf + (size_t)(s->off % r->cap);
}

void rtc_pktring_drop(rtc_pktring* r, uint16_t seq)
{
    rtc_pkt_slot* s;
    if (!r->slot)
        return;
    s = &r->slot[seq & r->mask];
    if (s->used && s->seq == seq) {
        s->used = 0;
        r->count--;
    }
}

size_t rtc_pktring_bytes(const rtc_pktring* r)
{
    return r->buf ? r->cap + ((size_t)r->mask + 1) * sizeof(*r->slot) : 0;
}
