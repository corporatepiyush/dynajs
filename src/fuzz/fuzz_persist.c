#include "dyn-hash.h"
#include "dyn-persist.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static volatile uint64_t sink;

static void touch(const dyn_persist_r* r)
{
    uint32_t i;
    uint64_t acc = 0, j;
    for (i = 0; i < r->nsect; i++) {
        const dyn_persist_rsect* s = &r->s[i];
        if (s->len) {
            acc += s->p[0];
            acc += s->p[s->len - 1];
        }
        for (j = 0; j < s->len && j < 256; j++)
            acc += s->p[j];
        if (dyn_persist_find(r, s->kind, s->tag) == NULL)
            abort();
    }
    sink = acc;
}

static void check(const uint8_t* buf, size_t size)
{
    dyn_persist_r r;
    dyn_persist_r* h = NULL;
    dyn_persist_err e;
    int v;
    for (v = 0; v < 2; v++) {
        memset(&e, 0, sizeof e);
        if (dyn_persist_validate(buf, size, v, 0, &r, &e) == 0) {
            touch(&r);
            free(r.s);
        } else if (e.code == 0 || !e.msg[0]) {
            abort();
        }
    }
    if (dyn_persist_from_bytes(buf, size, 1, 0, &h, &e) == 0) {
        dyn_persist_w w;
        uint8_t* img;
        size_t n;
        uint32_t i;
        touch(h);
        dyn_persist_w_init(&w);
        dyn_persist_w_begin(&w, h->type_id, h->count);
        for (i = 0; i < DYN_PERSIST_NPARAMS; i++)
            dyn_persist_w_param(&w, i, h->params[i]);
        for (i = 0; i < h->nsect; i++)
            dyn_persist_w_ref(&w, h->s[i].kind, h->s[i].tag, h->s[i].elem,
                h->s[i].flags, h->s[i].p, h->s[i].len);
        if (dyn_persist_w_image(&w, &img, &n) != 0)
            abort();
        if (dyn_persist_validate(img, n, 1, h->type_id, NULL, NULL) != 0)
            abort();
        free(img);
        dyn_persist_w_free(&w);
        dyn_persist_release(h);
    }
}

static void put32(uint8_t* p, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t* p, uint64_t v)
{
    uint32_t i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get64(const uint8_t* p)
{
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static void fix_slot(uint8_t* fix, size_t size, uint32_t slot_size, uint32_t idx, uint32_t nsect)
{
    uint8_t* h = fix + DYN_PERSIST_SUPER + (size_t)idx * slot_size;
    uint64_t fs = (uint64_t)size, data = DYN_PERSIST_SUPER + 2 * (uint64_t)slot_size;
    uint64_t base_end = data, ov;
    size_t hlen = DYN_PERSIST_HDR + (size_t)nsect * DYN_PERSIST_SEC;
    uint32_t i, nov, npage = 0, pages[8];
    uint8_t raw41 = h[41], raw152 = h[152];
    memcpy(h, "DYNAPF\n\x1a", 8);
    put32(h + 8, DYN_PERSIST_VERSION);
    memcpy(h + 12, &(uint32_t) { 0x01020304u }, 4);
    put32(h + 24, nsect);
    put32(h + 28, DYN_PERSIST_PAGE);
    put32(h + 132, idx);
    for (i = 0; i < nsect; i++) {
        uint8_t* t = h + DYN_PERSIST_HDR + (size_t)i * DYN_PERSIST_SEC;
        uint32_t el = (uint32_t)(t[8] % 9 + 1), page = t[12] & 1;
        uint64_t off = up(data + (((uint64_t)t[17] << 6) % (fs - data + 1)), page ? DYN_PERSIST_PAGE : 64u);
        uint64_t len = (uint64_t)t[25] * el * (page ? 256u : 1u);
        if (!(t[0] & 1))
            continue;
        put32(t + 8, el);
        put32(t + 12, page);
        put64(t + 16, off);
        put64(t + 24, len);
        put32(t + 36, 0);
        if (off + len <= fs) {
            uint64_t end = page ? up(off + len, DYN_PERSIST_PAGE) : off + len;
            put32(t + 32, dyn_crc32c(fix + off, (size_t)len));
            if (end <= fs && end > base_end)
                base_end = end;
            if (page && len && end <= fs)
                pages[npage++] = i;
        }
    }
    put64(h + 40, (raw41 & 1) ? get64(h + 40) : fs);
    put64(h + 136, base_end);
    nov = raw152 % 4u;
    put64(h + 144, 0);
    put32(h + 152, 0);
    put32(h + 156, dyn_crc32c(fix, 0));
    ov = up(base_end, 64u);
    if (nov && npage && ov + (uint64_t)nov * DYN_PERSIST_OV <= fs) {
        for (i = 0; i < nov; i++) {
            uint8_t* o = fix + ov + (size_t)i * DYN_PERSIST_OV;
            uint32_t sect = pages[o[0] % npage];
            const uint8_t* t = h + DYN_PERSIST_HDR + (size_t)sect * DYN_PERSIST_SEC;
            uint64_t chunks = (get64(t + 24) + DYN_PERSIST_PAGE - 1) / DYN_PERSIST_PAGE;
            put32(o, sect);
            put32(o + 4, (uint32_t)(o[4] % chunks));
            put64(o + 8, up(base_end, DYN_PERSIST_PAGE) + (uint64_t)(o[9] % 8u) * DYN_PERSIST_PAGE);
        }
        put64(h + 144, ov);
        put32(h + 152, nov);
        put32(h + 156, dyn_crc32c(fix + ov, (size_t)nov * DYN_PERSIST_OV));
    }
    put32(h + 128, 0);
    put32(h + 128, dyn_crc32c(h, hlen));
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    uint8_t* fix;
    uint32_t nsect, slot_size, i;
    if (size > (1u << 20))
        return 0;
    check(data, size);
    if (size < DYN_PERSIST_SUPER + 2 * DYN_PERSIST_HDR)
        return 0;
    fix = (uint8_t*)malloc(size);
    if (!fix)
        return 0;
    memcpy(fix, data, size);
    nsect = (uint32_t)data[size - 1] % 8u;
    if (data[size - 2] & 4)
        nsect = 1 + nsect % 2u;
    slot_size = (uint32_t)((DYN_PERSIST_HDR + nsect * DYN_PERSIST_SEC + 63u) & ~63u);
    memcpy(fix, "DYNAPF\n\x1a", 8);
    put32(fix + 8, DYN_PERSIST_VERSION);
    memcpy(fix + 12, &(uint32_t) { 0x01020304u }, 4);
    put32(fix + 16, slot_size);
    memset(fix + 20, 0, 40);
    put32(fix + 60, dyn_crc32c(fix, 60));
    if (DYN_PERSIST_SUPER + 2 * (size_t)slot_size <= size) {
        for (i = 0; i < 2; i++)
            if (data[size - 2] & (1u << i))
                fix_slot(fix, size, slot_size, i, nsect);
        check(fix, size);
    }
    free(fix);
    return 0;
}
