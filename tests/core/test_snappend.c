/* tests/core/test_snappend.c -- src/core/dyn-snappend.h: dyn_snappend(buf, cap, off, fmt, ...) appends at off and
 * returns the new offset clamped to cap - 1 (never the would-write count), keeps buf NUL-terminated, leaves buf
 * untouched when off >= cap or cap == 0. Seeded random appends (dyn-fuzzkit.h) are checked against one plain
 * snprintf of the whole text; a chained append that overflows matches the truncated concatenation. */
#include <stdint.h>
#include <string.h>
#include "../lib/dyn-fuzzkit.h"
#include "../../src/core/dyn-snappend.h"

static size_t fmt_one(char* dst, size_t cap, size_t off, int kind, const char* s, int v, double d)
{
    switch (kind) {
    case 0:
        return dyn_snappend(dst, cap, off, "%s", s);
    case 1:
        return dyn_snappend(dst, cap, off, "%d", v);
    default:
        return dyn_snappend(dst, cap, off, "[%s|%.3f]", s, d);
    }
}

static int ref_one(char* dst, size_t cap, int kind, const char* s, int v, double d)
{
    switch (kind) {
    case 0:
        return snprintf(dst, cap, "%s", s);
    case 1:
        return snprintf(dst, cap, "%d", v);
    default:
        return snprintf(dst, cap, "[%s|%.3f]", s, d);
    }
}

int main(void)
{
    fk_rng_t r;
    unsigned long i, iters;
    fk_init("test_snappend");
    r = fk_stream("single");
    iters = fk_iters(20000);
    for (i = 0; i < iters; i++) {
        char buf[80], keep[80], s[48], ref[256];
        size_t cap = 1 + fk_below(&r, 72), off = fk_below(&r, (uint32_t)cap + 6), got, want, slen = fk_below(&r, 40);
        int kind = (int)fk_below(&r, 3), v = (int)fk_next(&r), n;
        double d = (double)(int32_t)fk_next(&r) / 7.0;
        fk_pick_bytes(&r, (uint8_t*)s, slen, "abcxyz0123456789-_");
        s[slen] = 0;
        fk_pick_bytes(&r, (uint8_t*)buf, sizeof buf, "ABCDEFGH");
        memcpy(keep, buf, sizeof buf);
        got = fmt_one(buf, cap, off, kind, s, v, d);
        if (off >= cap) {
            FK_CHECK(got == cap - 1, "off %zu >= cap %zu: returned %zu, want %zu", off, cap, got, cap - 1);
            FK_CHECK(!memcmp(buf, keep, sizeof buf), "off %zu >= cap %zu: buffer was written", off, cap);
            continue;
        }
        n = ref_one(ref, sizeof ref, kind, s, v, d);
        want = off + (size_t)n < cap ? off + (size_t)n : cap - 1;
        FK_CHECK(got == want, "cap %zu off %zu kind %d n %d: returned %zu, want %zu", cap, off, kind, n, got, want);
        FK_CHECK(got < cap && buf[got] == 0, "cap %zu off %zu: no NUL at the returned offset %zu", cap, off, got);
        FK_CHECK(!memcmp(buf, keep, off), "cap %zu off %zu: bytes before off changed", cap, off);
        FK_CHECK(!memcmp(buf + off, ref, got - off), "cap %zu off %zu kind %d: appended text differs", cap, off, kind);
        FK_CHECK(!memcmp(buf + cap, keep + cap, sizeof buf - cap), "cap %zu off %zu: wrote past cap", cap, off);
    }
    fk_case = "chain";
    r = fk_stream("chain");
    for (i = 0; i < iters / 10; i++) {
        char buf[40], whole[1024];
        size_t cap = 1 + fk_below(&r, (uint32_t)sizeof buf - 1), off = 0, wn = 0;
        unsigned k, parts = 1 + fk_below(&r, 12);
        for (k = 0; k < parts; k++) {
            int v = (int)fk_below(&r, 100000);
            off = dyn_snappend(buf, cap, off, "%s%d", k ? "," : "", v);
            wn += (size_t)snprintf(whole + wn, sizeof whole - wn, "%s%d", k ? "," : "", v);
            FK_CHECK(off < cap, "chain cap %zu part %u: offset %zu escaped the buffer", cap, k, off);
        }
        FK_CHECK(off == (wn < cap ? wn : cap - 1), "chain cap %zu: final offset %zu, want %zu", cap, off, wn < cap ? wn : cap - 1);
        FK_CHECK(!memcmp(buf, whole, off) && buf[off] == 0, "chain cap %zu: text is not the truncated concatenation", cap);
    }
    fk_case = "edges";
    {
        char b[4] = { 'x', 'x', 'x', 'x' };
        FK_CHECK(dyn_snappend(b, 0, 0, "%s", "abc") == 0 && b[0] == 'x', "cap 0 must not write and must return 0");
        FK_CHECK(dyn_snappend(b, 4, 3, "%s", "abc") == 3 && b[3] == 0, "off cap-1 writes only the NUL");
        FK_CHECK(dyn_snappend(b, 4, 0, "%s", "") == 0 && b[0] == 0, "an empty append keeps the offset");
    }
    return fk_done("test_snappend");
}
