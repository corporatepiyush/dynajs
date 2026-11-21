#include <stdlib.h>
#include "dd.h"

const char* dd_event_name(int op)
{
    switch (op) {
    case DD_EV_INSERT:
        return "insert";
    case DD_EV_UPDATE:
        return "update";
    case DD_EV_REPLACE:
        return "replace";
    case DD_EV_DELETE:
        return "delete";
    case DD_EV_DROP:
        return "drop";
    case DD_EV_INVALIDATE:
        return "invalidate";
    default:
        return "unknown";
    }
}

int dd_ring_enable(dd_coll* c, uint32_t cap)
{
    if (c->ring_on)
        return 0;
    if (cap == 0)
        cap = c->ring_cap ? c->ring_cap : 4096;
    c->ring.ev = calloc(cap, sizeof *c->ring.ev);
    if (!c->ring.ev)
        return -1;
    c->ring.cap = cap;
    c->ring.head = 0;
    c->ring.first_seq = c->db ? c->db->seq + 1 : 1;
    c->ring.evicted = 0;
    c->ring_on = 1;
    return 0;
}

void dd_ring_free(dd_ring* r)
{
    uint32_t i;
    if (r->ev)
        for (i = 0; i < r->cap; i++)
            free(r->ev[i].ev);
    free(r->ev);
    memset(r, 0, sizeof *r);
}

void dd_token_hex(uint64_t seq, char out[17])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 15; i >= 0; i--) {
        out[i] = hx[seq & 15];
        seq >>= 4;
    }
    out[16] = 0;
}

int dd_token_parse(const char* s, size_t n, uint64_t* seq)
{
    uint64_t v = 0;
    size_t i;
    if (n != 16)
        return -1;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                                           : -1;
        if (d < 0)
            return -1;
        v = (v << 4) | (uint64_t)d;
    }
    *seq = v;
    return 0;
}

static uint64_t ring_count(const dd_ring* r)
{
    return r->head < r->cap ? r->head : r->cap;
}

uint64_t dd_ring_oldest(const dd_ring* r)
{
    uint64_t cnt = ring_count(r);
    if (!cnt)
        return 0;
    return r->ev[(r->head - cnt) % r->cap].seq;
}

const dd_event* dd_ring_at(const dd_ring* r, uint64_t seq)
{
    uint64_t cnt = ring_count(r), lo = 0, hi = cnt, base = r->head - cnt;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (r->ev[(base + mid) % r->cap].seq < seq)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= cnt)
        return NULL;
    return &r->ev[(base + lo) % r->cap];
}

int dd_emit(dd_coll* c, int op, const uint8_t* id_elem_doc, const uint8_t* full, const dd_upd_desc* desc, uint32_t docno,
    int64_t now_ms)
{
    dyn_bson_w w;
    char tok[17];
    uint64_t seq;
    dd_event* slot;
    if (!c->db)
        return 0;
    if (!c->ring_on && c->db->watching && dd_ring_enable(c, 0) < 0)
        return -1;
    seq = ++c->db->seq;
    if (!c->ring_on) {
        c->db->unrecorded = seq;
        return 0;
    }
    dd_token_hex(seq, tok);
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "_id", 3);
    dyn_bson_w_string(&w, "_data", 5, tok, 16);
    dyn_bson_w_sub_end(&w);
    {
        const char* nm = dd_event_name(op);
        dyn_bson_w_string(&w, "operationType", 13, nm, strlen(nm));
    }
    dyn_bson_w_date(&w, "wallTime", 8, now_ms);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "ns", 2);
    if (c->db->name)
        dyn_bson_w_string(&w, "db", 2, c->db->name, strlen(c->db->name));
    dyn_bson_w_string(&w, "coll", 4, c->name, c->name_n);
    dyn_bson_w_sub_end(&w);
    if (id_elem_doc)
        dyn_bson_w_value(&w, "documentKey", 11, (dyn_bson_val) { DYN_BSON_DOC, id_elem_doc, dyn_bson_rd32(id_elem_doc) });
    if (full && (op == DD_EV_INSERT || op == DD_EV_REPLACE))
        dyn_bson_w_value(&w, "fullDocument", 12, (dyn_bson_val) { DYN_BSON_DOC, full, dyn_bson_rd32(full) });
    if (op == DD_EV_UPDATE && desc) {
        dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "updateDescription", 17);
        if (desc->updated.n)
            dyn_bson_w_value(&w, "updatedFields", 13,
                (dyn_bson_val) { DYN_BSON_DOC, desc->updated.p, (uint32_t)desc->updated.n });
        else {
            dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "updatedFields", 13);
            dyn_bson_w_sub_end(&w);
        }
        if (desc->removed.n)
            dyn_bson_w_value(&w, "removedFields", 13,
                (dyn_bson_val) { DYN_BSON_ARRAY, desc->removed.p, (uint32_t)desc->removed.n });
        else {
            dyn_bson_w_sub_begin(&w, DYN_BSON_ARRAY, "removedFields", 13);
            dyn_bson_w_sub_end(&w);
        }
        dyn_bson_w_sub_end(&w);
    }
    dyn_bson_w_doc_end(&w);
    if (w.oom) {
        dyn_bson_w_free(&w);
        return -1;
    }
    slot = &c->ring.ev[c->ring.head % c->ring.cap];
    if (c->ring.head >= c->ring.cap) {
        c->ring.evicted = slot->seq;
        free(slot->ev);
    }
    slot->seq = seq;
    slot->ev = w.p;
    slot->ev_n = (uint32_t)w.n;
    slot->docno = docno;
    slot->op = (uint8_t)op;
    c->ring.head++;
    return 0;
}
