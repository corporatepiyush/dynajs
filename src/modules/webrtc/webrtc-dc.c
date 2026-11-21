#include "webrtc-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

#include <stdlib.h>
#include <string.h>

static void wrt_dc_set_closed(wrt_pc* pc, wrt_dc* dc)
{
    if (dc->state == WRT_DC_CLOSED)
        return;
    dc->state = WRT_DC_CLOSED;
    if (dc->has_id && dc->id < pc->id_cap && pc->id2slot[dc->id] == dc->slot + 1)
        pc->id2slot[dc->id] = 0;
    pc->dc_closed++;
    wrt_ev_push(pc, WRT_EV_DC_CLOSED, 0, dc->slot, 0, NULL, 0);
}

static void wrt_dc_set_open(wrt_pc* pc, wrt_dc* dc)
{
    if (dc->state != WRT_DC_CONNECTING)
        return;
    dc->state = WRT_DC_OPEN;
    dc->acked = 1;
    pc->dc_opened++;
    wrt_ev_push(pc, WRT_EV_DC_OPEN, 0, dc->slot, 0, NULL, 0);
}

static int wrt_id_map(wrt_pc* pc, uint16_t id, uint32_t slot)
{
    if (id >= pc->id_cap) {
        uint32_t nc = pc->id_cap ? pc->id_cap : 64;
        uint32_t* nm;
        while (nc <= id)
            nc *= 2;
        if (nc > 65536)
            nc = 65536;
        nm = (uint32_t*)realloc(pc->id2slot, nc * sizeof(uint32_t));
        if (!nm)
            return -1;
        memset(nm + pc->id_cap, 0, (nc - pc->id_cap) * sizeof(uint32_t));
        pc->id2slot = nm;
        pc->id_cap = nc;
    }
    pc->id2slot[id] = slot + 1;
    return 0;
}

wrt_dc* wrt_dc_by_id(wrt_pc* pc, uint16_t id)
{
    uint32_t s;
    if (id >= pc->id_cap || !(s = pc->id2slot[id]))
        return NULL;
    return pc->dcs[s - 1];
}

int wrt_dc_id_taken(const wrt_pc* pc, int id)
{
    return id >= 0 && (uint32_t)id < pc->id_cap && pc->id2slot[id] != 0;
}

static int wrt_dcep_open(wrt_pc* pc, wrt_dc* dc)
{
    size_t ll = dc->label ? strlen(dc->label) : 0, pl = dc->protocol ? strlen(dc->protocol) : 0;
    size_t cap = rtc_dcep_open_size(ll, pl), n;
    uint8_t* msg = (uint8_t*)malloc(cap);
    int r;
    if (!msg)
        return -1;
    n = rtc_dcep_write_open(msg, cap, dc->ordered, dc->max_rtx, dc->max_life, 256, dc->label, ll, dc->protocol, pl);
    r = n ? rtc_sctp_send(pc->sctp, dc->id, RTC_DCEP_PPID, msg, n, 0, -1, -1, wrt_now_us()) : -1;
    free(msg);
    if (r == 0)
        dc->dcep_pending += (uint32_t)n;
    return r;
}

static void wrt_dc_start(wrt_pc* pc, wrt_dc* dc)
{
    if (dc->state != WRT_DC_CONNECTING || !dc->has_id || !pc->sctp || pc->sctp->state != RTC_SCTP_ESTABLISHED)
        return;
    if (dc->negotiated) {
        wrt_dc_set_open(pc, dc);
        return;
    }
    if (dc->open_sent)
        return;
    dc->open_sent = 1;
    if (wrt_dcep_open(pc, dc) < 0) {
        wrt_ev_push(pc, WRT_EV_DC_ERROR, 0, dc->slot, 0, NULL, 0);
        wrt_dc_set_closed(pc, dc);
    }
}

int wrt_dc_id_limit(const wrt_pc* pc)
{
    if (pc->sctp && pc->sctp->state == RTC_SCTP_ESTABLISHED)
        return (int)rtc_sctp_max_channels(pc->sctp);
    return 65535;
}

int wrt_dc_free_id(const wrt_pc* pc)
{
    int id, lim = wrt_dc_id_limit(pc);
    for (id = pc->sctp_client ? 0 : 1; id < lim; id += 2)
        if (!wrt_dc_id_taken(pc, id))
            return id;
    return -1;
}

void wrt_pc_assign_ids(wrt_pc* pc)
{
    uint32_t i;
    if (!pc->role_known)
        return;
    for (i = 0; i < pc->ndcs; i++) {
        wrt_dc* dc = pc->dcs[i];
        int id;
        if (!dc || dc->has_id || dc->state == WRT_DC_CLOSED)
            continue;
        id = wrt_dc_free_id(pc);
        if (id < 0 || wrt_id_map(pc, (uint16_t)id, dc->slot) < 0) {
            wrt_ev_push(pc, WRT_EV_DC_ERROR, 1, dc->slot, 0, NULL, 0);
            wrt_dc_set_closed(pc, dc);
            continue;
        }
        dc->id = (uint16_t)id;
        dc->has_id = 1;
        wrt_dc_start(pc, dc);
    }
}

static wrt_dc* wrt_dc_alloc(wrt_pc* pc)
{
    wrt_dc* dc;
    uint32_t i;
    for (i = 0; i < pc->ndcs; i++)
        if (!pc->dcs[i])
            break;
    if (i == pc->ndcs) {
        if (pc->ndcs == pc->cap_dcs) {
            uint32_t nc = pc->cap_dcs ? pc->cap_dcs * 2 : 8;
            wrt_dc** nd = (wrt_dc**)realloc(pc->dcs, nc * sizeof(*nd));
            if (!nd)
                return NULL;
            pc->dcs = nd;
            pc->cap_dcs = nc;
        }
        pc->ndcs++;
    }
    dc = (wrt_dc*)calloc(1, sizeof(*dc));
    if (!dc) {
        if (i == pc->ndcs - 1)
            pc->ndcs--;
        return NULL;
    }
    dc->slot = i;
    pc->dcs[i] = dc;
    return dc;
}

static char* wrt_strndup(const char* s, size_t n)
{
    char* d = (char*)malloc(n + 1);
    if (!d)
        return NULL;
    if (n)
        memcpy(d, s, n);
    d[n] = 0;
    return d;
}

wrt_dc* wrt_dc_create(wrt_pc* pc, const char* label, size_t ll, const char* proto, size_t pl, int ordered, int32_t max_rtx,
    int64_t max_life, int negotiated, int id)
{
    wrt_dc* dc;
    if (negotiated && wrt_dc_id_taken(pc, id))
        return NULL;
    dc = wrt_dc_alloc(pc);
    if (!dc)
        return NULL;
    dc->label = wrt_strndup(label, ll);
    dc->protocol = wrt_strndup(proto, pl);
    if (!dc->label || !dc->protocol) {
        wrt_dc_release(pc, dc);
        return NULL;
    }
    dc->state = WRT_DC_CONNECTING;
    dc->ordered = (uint8_t)(ordered != 0);
    dc->max_rtx = max_rtx;
    dc->max_life = max_life;
    dc->negotiated = (uint8_t)(negotiated != 0);
    if (id >= 0) {
        if (wrt_id_map(pc, (uint16_t)id, dc->slot) < 0) {
            wrt_dc_release(pc, dc);
            return NULL;
        }
        dc->id = (uint16_t)id;
        dc->has_id = 1;
    }
    if (dc->has_id)
        wrt_dc_start(pc, dc);
    else
        wrt_pc_assign_ids(pc);
    return dc;
}

void wrt_dc_release(wrt_pc* pc, wrt_dc* dc)
{
    if (!dc)
        return;
    if (dc->has_id && dc->id < pc->id_cap && pc->id2slot[dc->id] == dc->slot + 1)
        pc->id2slot[dc->id] = 0;
    if (dc->slot < pc->ndcs && pc->dcs[dc->slot] == dc)
        pc->dcs[dc->slot] = NULL;
    if (dc->remote) {
        uint64_t c = WRT_REMOTE_DC_COST + (dc->label ? strlen(dc->label) : 0) + (dc->protocol ? strlen(dc->protocol) : 0);
        pc->remote_dc_bytes -= c < pc->remote_dc_bytes ? c : pc->remote_dc_bytes;
    }
    free(dc->label);
    free(dc->protocol);
    free(dc);
}

int wrt_dc_send(wrt_pc* pc, wrt_dc* dc, const uint8_t* p, size_t n, int binary)
{
    static const uint8_t zero = 0;
    uint32_t ppid = binary ? (n ? 53 : 57) : (n ? 51 : 56);
    int r;
    if (dc->state != WRT_DC_OPEN || !pc->sctp)
        return -1;
    r = rtc_sctp_send(pc->sctp, dc->id, ppid, n ? p : &zero, n ? n : 1, !dc->ordered, dc->max_rtx,
        dc->max_life >= 0 ? dc->max_life : -1, wrt_now_us());
    if (r < 0)
        return -1;
    dc->buffered += (uint64_t)n;
    dc->msgs_tx++;
    dc->bytes_tx += n;
    pc->data_tx += n;
    if (!n)
        dc->zero_pending++;
    return 0;
}

void wrt_dc_close(wrt_pc* pc, wrt_dc* dc)
{
    if (dc->state == WRT_DC_CLOSING || dc->state == WRT_DC_CLOSED)
        return;
    dc->state = WRT_DC_CLOSING;
    if (dc->has_id && pc->sctp && pc->sctp->state == RTC_SCTP_ESTABLISHED) {
        dc->reset_out = 1;
        if (rtc_sctp_reset_stream(pc->sctp, dc->id) < 0)
            wrt_dc_set_closed(pc, dc);
        return;
    }
    wrt_dc_set_closed(pc, dc);
}

static void wrt_dc_try_closed(wrt_pc* pc, wrt_dc* dc)
{
    if (dc->reset_in && dc->reset_out == 2)
        wrt_dc_set_closed(pc, dc);
}

static void wrt_sctp_packet(void* ud, const uint8_t* p, size_t n)
{
    wrt_pc* pc = (wrt_pc*)ud;
    wrt_dtls_write(&pc->dtls, p, n);
}

static void wrt_dcep_in(wrt_pc* pc, uint16_t sid, const uint8_t* p, size_t n)
{
    wrt_dc* dc;
    rtc_dcep_msg m;
    uint8_t ack = RTC_DCEP_ACK;
    if (rtc_dcep_parse(p, n, &m) < 0)
        return;
    if (m.type == RTC_DCEP_ACK) {
        dc = wrt_dc_by_id(pc, sid);
        if (dc && !dc->negotiated)
            wrt_dc_set_open(pc, dc);
        return;
    }
    dc = wrt_dc_id_taken(pc, sid) ? wrt_dc_by_id(pc, sid) : NULL;
    if (dc && dc->state == WRT_DC_CLOSING && dc->reset_in)
        wrt_dc_set_closed(pc, dc);
    if ((sid & 1) == (pc->sctp_client ? 0 : 1) || wrt_dc_id_taken(pc, sid) || pc->closed)
        return;
    if (pc->remote_dc_bytes + WRT_REMOTE_DC_COST + m.label_len + m.protocol_len > WRT_REMOTE_DC_BUDGET) {
        if (pc->dc_refused < WRT_REFUSE_MAX) {
            pc->dc_refused++;
            rtc_sctp_reset_stream(pc->sctp, sid);
        }
        return;
    }
    dc = wrt_dc_alloc(pc);
    if (!dc)
        return;
    dc->label = wrt_strndup((const char*)m.label, m.label_len);
    dc->protocol = wrt_strndup((const char*)m.protocol, m.protocol_len);
    if (!dc->label || !dc->protocol || wrt_id_map(pc, sid, dc->slot) < 0) {
        wrt_dc_release(pc, dc);
        return;
    }
    dc->id = sid;
    dc->has_id = 1;
    dc->ordered = m.ordered;
    dc->max_rtx = m.max_rtx;
    dc->max_life = m.max_life;
    dc->state = WRT_DC_OPEN;
    dc->acked = 1;
    dc->remote = 1;
    pc->remote_dc_bytes += WRT_REMOTE_DC_COST + m.label_len + m.protocol_len;
    pc->dc_opened++;
    if (rtc_sctp_send(pc->sctp, sid, RTC_DCEP_PPID, &ack, 1, 0, -1, -1, wrt_now_us()) == 0)
        dc->dcep_pending += 1;
    wrt_ev_push(pc, WRT_EV_DC_NEW, 0, dc->slot, 0, NULL, 0);
}

static void wrt_sctp_message(void* ud, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n)
{
    wrt_pc* pc = (wrt_pc*)ud;
    wrt_dc* dc;
    int binary;
    if (ppid == RTC_DCEP_PPID) {
        wrt_dcep_in(pc, stream, p, n);
        return;
    }
    dc = wrt_dc_by_id(pc, stream);
    if (!dc)
        return;
    if (dc->state == WRT_DC_CONNECTING && !dc->negotiated && dc->open_sent)
        wrt_dc_set_open(pc, dc);
    if (dc->state != WRT_DC_OPEN)
        return;
    switch (ppid) {
    case 51:
    case 54:
        binary = 0;
        break;
    case 56:
        binary = 0;
        n = 0;
        break;
    case 53:
    case 52:
        binary = 1;
        break;
    case 57:
        binary = 1;
        n = 0;
        break;
    default:
        return;
    }
    dc->msgs_rx++;
    dc->bytes_rx += n;
    pc->data_rx += n;
    wrt_ev_push(pc, WRT_EV_DC_MSG, binary, dc->slot, 0, p, n);
}

void wrt_sctp_on_state(void* ud, int state)
{
    wrt_pc* pc = (wrt_pc*)ud;
    uint32_t i;
    if (state == RTC_SCTP_ESTABLISHED) {
        wrt_ev_push(pc, WRT_EV_SCTP_UP, 0, 0, 0, NULL, 0);
        wrt_pc_assign_ids(pc);
        for (i = 0; i < pc->ndcs; i++) {
            wrt_dc* dc = pc->dcs[i];
            if (dc && dc->has_id && dc->state == WRT_DC_CONNECTING && dc->id >= rtc_sctp_max_channels(pc->sctp)) {
                wrt_ev_push(pc, WRT_EV_DC_ERROR, 3, dc->slot, 0, NULL, 0);
                wrt_dc_set_closed(pc, dc);
            } else if (dc)
                wrt_dc_start(pc, dc);
        }
        return;
    }
    if (state == RTC_SCTP_CLOSED || state == RTC_SCTP_ABORTED) {
        for (i = 0; i < pc->ndcs; i++)
            if (pc->dcs[i] && pc->dcs[i]->state != WRT_DC_CLOSED) {
                if (state == RTC_SCTP_ABORTED && pc->dcs[i]->state == WRT_DC_OPEN && !pc->closed)
                    wrt_ev_push(pc, WRT_EV_DC_ERROR, 2, i, 0, NULL, 0);
                wrt_dc_set_closed(pc, pc->dcs[i]);
            }
        wrt_ev_push(pc, WRT_EV_SCTP_DOWN, 0, 0, (uint32_t)state, NULL, 0);
    }
}

static void wrt_sctp_reset_in(void* ud, uint16_t stream)
{
    wrt_pc* pc = (wrt_pc*)ud;
    wrt_dc* dc = wrt_dc_by_id(pc, stream);
    if (!dc)
        return;
    dc->reset_in = 1;
    if (!dc->reset_out) {
        if (dc->state == WRT_DC_CONNECTING || dc->state == WRT_DC_OPEN) {
            dc->state = WRT_DC_CLOSING;
            wrt_ev_push(pc, WRT_EV_DC_CLOSING, 0, dc->slot, 0, NULL, 0);
        }
        dc->reset_out = 1;
        if (rtc_sctp_reset_stream(pc->sctp, stream) < 0)
            dc->reset_out = 2;
    }
    wrt_dc_try_closed(pc, dc);
}

static void wrt_sctp_reset_out(void* ud, uint16_t stream)
{
    wrt_pc* pc = (wrt_pc*)ud;
    wrt_dc* dc = wrt_dc_by_id(pc, stream);
    if (!dc)
        return;
    dc->reset_out = 2;
    wrt_dc_try_closed(pc, dc);
}

static void wrt_sctp_sent(void* ud, uint16_t stream, size_t n)
{
    wrt_pc* pc = (wrt_pc*)ud;
    wrt_dc* dc = wrt_dc_by_id(pc, stream);
    uint64_t before;
    if (!dc)
        return;
    if (dc->dcep_pending) {
        uint32_t k = n < dc->dcep_pending ? (uint32_t)n : dc->dcep_pending;
        dc->dcep_pending -= k;
        return;
    }
    if (n == 1 && dc->zero_pending) {
        dc->zero_pending--;
        n = 0;
    }
    before = dc->buffered;
    dc->buffered -= n < dc->buffered ? n : dc->buffered;
    if (before > dc->low_threshold && dc->buffered <= dc->low_threshold)
        wrt_ev_push(pc, WRT_EV_DC_BUFLOW, 0, dc->slot, 0, NULL, 0);
}

const rtc_sctp_cb wrt_sctp_cbs = {
    wrt_sctp_packet,
    wrt_sctp_message,
    wrt_sctp_on_state,
    wrt_sctp_reset_in,
    wrt_sctp_reset_out,
    wrt_sctp_sent,
};

#endif
