/*
 * test_arck_core.c -- src/core/arck (the dyna:compress tar and zip framing:
 * readers, entry-name safety, DOS time, octal fields, raw deflate for zip)
 * as a standalone C library.
 *
 * Oracles are written from the formats, not from the code: archives are
 * built byte by byte here from the POSIX ustar layout (name/prefix split,
 * octal fields, checksum over the header with the checksum field as
 * spaces), GNU 'L' long names, pax 'x' records (path, linkpath, size), and
 * from PKWARE APPNOTE local headers / central directory / EOCD with a
 * table-driven CRC-32 written here. Every random archive (300 tar, 300 zip)
 * must read back the exact names, sizes, modes, mtimes, link targets and
 * bodies; damaged ones (bad checksum, truncated block, size past the end,
 * overlapping zip members, wrong CRC, ".." / absolute / drive names) must be
 * refused with the reader's error set. DOS date/time is checked against a
 * day-counting calendar for 1980..2107 (2-second resolution, clamping
 * outside the range), and tar_put_octal against printf's %o. A deflated
 * member with a wrong CRC is refused as "does not inflate": the raw stream
 * is re-wrapped as gzip carrying the declared CRC, so the gzip trailer
 * check is what fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-arck.h"
#include "../lib/dyn-xalloc.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static uint32_t rs = 7;
static uint32_t rnd(uint32_t n)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) % n;
}

static uint32_t crc_tab[256];
static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static uint32_t crc32_ref(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct {
    uint8_t* p;
    size_t n, cap;
} buf_t;

static void put(buf_t* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 1024;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void pad512(buf_t* b)
{
    static const uint8_t z[512];
    if (b->n % 512)
        put(b, z, 512 - b->n % 512);
}
static void le16(buf_t* b, uint32_t v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(b, x, 2);
}
static void le32(buf_t* b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, x, 4);
}

static void ustar_header(buf_t* b, const char* name, const char* prefix, uint64_t size, uint64_t mode, uint64_t mtime, char type, const char* link)
{
    uint8_t h[512];
    unsigned sum = 0;
    size_t i;
    memset(h, 0, sizeof h);
    memcpy(h, name, strlen(name) > 100 ? 100 : strlen(name));
    snprintf((char*)h + 100, 8, "%07llo", (unsigned long long)mode);
    snprintf((char*)h + 108, 8, "%07o", 1000u);
    snprintf((char*)h + 116, 8, "%07o", 1000u);
    snprintf((char*)h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf((char*)h + 136, 12, "%011llo", (unsigned long long)mtime);
    memset(h + 148, ' ', 8);
    h[156] = (uint8_t)type;
    if (link)
        memcpy(h + 157, link, strlen(link));
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    if (prefix)
        memcpy(h + 345, prefix, strlen(prefix));
    for (i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char*)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    put(b, h, 512);
}

static void rand_name(char* s, size_t n)
{
    static const char A[] = "abcdefghijklmnopqrstuvwxyz0123456789_-.";
    size_t i;
    for (i = 0; i < n; i++) {
        s[i] = A[rnd(sizeof A - 1)];
        if (i > 0 && i + 1 < n && rnd(9) == 0 && s[i - 1] != '/')
            s[i] = '/';
    }
    if (s[0] == '.')
        s[0] = 'a';
    for (i = 1; i < n; i++)
        if (s[i] == '.' && s[i - 1] == '.')
            s[i] = 'x';
    s[n] = 0;
}

typedef struct {
    char name[600], link[200];
    uint64_t size, mode, mtime;
    char type;
    uint8_t* body;
} want_t;

static void test_tar(void)
{
    int it;
    for (it = 0; it < 300; it++) {
        buf_t b = { NULL, 0, 0 };
        want_t W[8];
        int nw = 1 + (int)rnd(8), i, got;
        for (i = 0; i < nw; i++) {
            want_t* w = &W[i];
            int form = (int)rnd(5);
            size_t nl = form == 0 ? 1 + rnd(99) : form == 1 ? 120 + rnd(100) : form == 2 ? 101 + rnd(400) : form == 3 ? 101 + rnd(400) : 1 + rnd(60);
            uint64_t k;
            rand_name(w->name, nl);
            w->size = rnd(5) == 0 ? 0 : rnd(3000);
            w->mode = rnd(01000);
            w->mtime = rnd(2000000000u);
            w->type = form == 4 ? '2' : '0';
            w->link[0] = 0;
            if (form == 4)
                rand_name(w->link, 1 + rnd(90));
            w->body = xmalloc(w->size + 1);
            for (k = 0; k < w->size; k++)
                w->body[k] = (uint8_t)rnd(256);
            if (form == 1) {
                size_t cut = nl - 1 - rnd(90);
                while (cut > 0 && w->name[cut] != '/')
                    cut--;
                if (cut == 0 || cut > 155 || nl - cut - 1 > 100 || nl - cut - 1 == 0) {
                    w->name[nl - 1 - 50] = '/';
                    cut = nl - 1 - 50;
                    if (cut > 155) {
                        w->name[100] = 0;
                        nl = 100;
                        form = 0;
                    }
                }
                if (form == 1) {
                    char pre[160], nm[110];
                    memcpy(pre, w->name, cut);
                    pre[cut] = 0;
                    strcpy(nm, w->name + cut + 1);
                    if (pre[0] == 0 || nm[0] == 0 || strstr(w->name, "//")) {
                        w->name[100] = 0;
                        form = 0;
                    } else
                        ustar_header(&b, nm, pre, w->size, w->mode, w->mtime, w->type, NULL);
                }
            }
            if (form == 2) {
                ustar_header(&b, "././@LongLink", NULL, nl + 1, 0, 0, 'L', NULL);
                put(&b, w->name, nl + 1);
                pad512(&b);
                ustar_header(&b, "trunc", NULL, w->size, w->mode, w->mtime, w->type, NULL);
            } else if (form == 3) {
                char rec[700], hdr[720];
                int rl = snprintf(rec, sizeof rec, " path=%s\n", w->name), tot = rl + 1;
                while ((int)snprintf(hdr, sizeof hdr, "%d%s", tot, rec) != tot)
                    tot++;
                ustar_header(&b, "PaxHeader", NULL, (uint64_t)tot, 0644, 0, 'x', NULL);
                put(&b, hdr, (size_t)tot);
                pad512(&b);
                ustar_header(&b, "short", NULL, w->size, w->mode, w->mtime, w->type, NULL);
            } else if (form == 0 || form == 4)
                ustar_header(&b, w->name, NULL, w->size, w->mode, w->mtime, w->type, form == 4 ? w->link : NULL);
            put(&b, w->body, w->size);
            pad512(&b);
        }
        {
            static const uint8_t z[1024];
            put(&b, z, 1024);
        }
        {
            tar_rd_t r;
            memset(&r, 0, sizeof r);
            r.p = b.p;
            r.n = b.n;
            for (i = 0; i < nw; i++) {
                tar_ent_t e;
                memset(&e, 0, sizeof e);
                got = arck_tar_next(&r, &e);
                CHECK(got == 1, "tar it %d entry %d: rc %d err %s", it, i, got, r.err);
                if (got != 1)
                    break;
                CHECK(strcmp(e.name, W[i].name) == 0 && e.nlen == strlen(W[i].name), "tar it %d entry %d name '%s' vs '%s'", it, i, e.name, W[i].name);
                CHECK(e.size == W[i].size && e.mode == W[i].mode && e.mtime == W[i].mtime && e.type == W[i].type, "tar it %d entry %d fields", it, i);
                CHECK(strcmp(e.link, W[i].link) == 0, "tar it %d entry %d link", it, i);
                CHECK(e.size == W[i].size && memcmp(b.p + e.body, W[i].body, W[i].size) == 0, "tar it %d entry %d body", it, i);
            }
            if (i == nw) {
                tar_ent_t e;
                memset(&e, 0, sizeof e);
                CHECK(arck_tar_next(&r, &e) == 0, "tar it %d: end marker", it);
            }
        }
        if (b.n > 1024) {
            tar_rd_t r;
            tar_ent_t e;
            size_t at = 148 + rnd(8);
            memset(&r, 0, sizeof r);
            memset(&e, 0, sizeof e);
            b.p[at] = b.p[at] == '1' ? '2' : '1';
            r.p = b.p;
            r.n = b.n;
            CHECK(arck_tar_next(&r, &e) == -1 && strstr(r.err, "checksum"), "tar it %d: checksum damage refused (%s)", it, r.err);
            memset(&r, 0, sizeof r);
            r.p = b.p;
            r.n = 700;
            b.p[at] = b.p[at] == '1' ? '2' : '1';
            memset(&e, 0, sizeof e);
            got = arck_tar_next(&r, &e);
            CHECK(got == -1 || (got == 1 && e.size <= 700 - e.body), "tar it %d: truncation refused or in bounds", it);
        }
        for (i = 0; i < nw; i++)
            free(W[i].body);
        free(b.p);
    }
    {
        static const char* const BAD[] = { "../x", "a/../b", "/etc/passwd", "\\x", "c:x", "a/..", ".." };
        static const char* const OK[] = { "a..b", "..a", "a/..b/c", "x/.../y", "a/b/c.txt" };
        size_t i;
        for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            buf_t b = { NULL, 0, 0 };
            tar_rd_t r;
            tar_ent_t e;
            static const uint8_t z[1024];
            CHECK(!arck_ar_name_safe(BAD[i], strlen(BAD[i])), "unsafe name '%s'", BAD[i]);
            ustar_header(&b, BAD[i], NULL, 0, 0644, 1, '0', NULL);
            put(&b, z, 1024);
            memset(&r, 0, sizeof r);
            memset(&e, 0, sizeof e);
            r.p = b.p;
            r.n = b.n;
            CHECK(arck_tar_next(&r, &e) == -1, "tar refuses '%s'", BAD[i]);
            memset(&r, 0, sizeof r);
            memset(&e, 0, sizeof e);
            r.p = b.p;
            r.n = b.n;
            r.allow_unsafe = 1;
            CHECK(arck_tar_next(&r, &e) == 1, "tar allowUnsafe takes '%s'", BAD[i]);
            free(b.p);
        }
        for (i = 0; i < sizeof OK / sizeof OK[0]; i++)
            CHECK(arck_ar_name_safe(OK[i], strlen(OK[i])), "safe name '%s'", OK[i]);
    }
    {
        int i;
        for (i = 0; i < 2000; i++) {
            uint64_t v = (uint64_t)rnd(1u << 30) * (rnd(2) ? 1 : 64);
            size_t n = 12;
            uint8_t got[12];
            char want[32];
            memset(got, 'z', sizeof got);
            arck_tar_put_octal(got, n, v);
            snprintf(want, sizeof want, "%0*llo", (int)(n - 1), (unsigned long long)v);
            CHECK(memcmp(got, want, n - 1) == 0 && got[n - 1] == 0, "tar_put_octal %llu", (unsigned long long)v);
        }
    }
}

typedef struct {
    char name[300];
    uint8_t* data;
    size_t n;
    uint32_t dos, mode;
    int method;
    uint8_t* comp;
    size_t cn;
} zw_t;

static void zip_build(buf_t* b, zw_t* W, int nw, int bad)
{
    size_t* loc = xmalloc(sizeof(size_t) * (size_t)nw);
    size_t cd;
    int i;
    for (i = 0; i < nw; i++) {
        zw_t* w = &W[i];
        uint32_t crc = crc32_ref(w->data, w->n) ^ (bad == 1 && i == nw - 1 ? 1u : 0u);
        loc[i] = b->n;
        le32(b, 0x04034b50);
        le16(b, 20);
        le16(b, 0);
        le16(b, (uint32_t)w->method);
        le32(b, w->dos);
        le32(b, crc);
        le32(b, (uint32_t)w->cn);
        le32(b, (uint32_t)w->n);
        le16(b, (uint32_t)strlen(w->name));
        le16(b, 0);
        put(b, w->name, strlen(w->name));
        put(b, w->comp, w->cn);
    }
    cd = b->n;
    for (i = 0; i < nw; i++) {
        zw_t* w = &W[i];
        uint32_t crc = crc32_ref(w->data, w->n) ^ (bad == 1 && i == nw - 1 ? 1u : 0u);
        le32(b, 0x02014b50);
        le16(b, 0x031e);
        le16(b, 20);
        le16(b, 0);
        le16(b, (uint32_t)w->method);
        le32(b, w->dos);
        le32(b, crc);
        le32(b, (uint32_t)w->cn);
        le32(b, (uint32_t)w->n);
        le16(b, (uint32_t)strlen(w->name));
        le16(b, 0);
        le16(b, 0);
        le16(b, 0);
        le16(b, 0);
        le32(b, w->mode << 16);
        le32(b, (uint32_t)(bad == 2 && i == nw - 1 && nw > 1 ? loc[0] : loc[i]));
        put(b, w->name, strlen(w->name));
    }
    le32(b, 0x06054b50);
    le16(b, 0);
    le16(b, 0);
    le16(b, (uint32_t)nw);
    le16(b, (uint32_t)nw);
    le32(b, (uint32_t)(b->n - cd - 0));
    le32(b, (uint32_t)cd);
    le16(b, 0);
    {
        uint32_t cds = (uint32_t)(b->n - 22 - cd);
        b->p[b->n - 22 + 12] = (uint8_t)cds;
        b->p[b->n - 22 + 13] = (uint8_t)(cds >> 8);
        b->p[b->n - 22 + 14] = (uint8_t)(cds >> 16);
        b->p[b->n - 22 + 15] = (uint8_t)(cds >> 24);
    }
    free(loc);
}

static void test_zip(void)
{
    int it;
    for (it = 0; it < 300; it++) {
        zw_t W[6];
        int nw = 1 + (int)rnd(6), i, bad = it % 5 == 3 ? 1 : it % 5 == 4 ? 2 : 0;
        buf_t b = { NULL, 0, 0 };
        zip_rd_t z;
        for (i = 0; i < nw; i++) {
            zw_t* w = &W[i];
            size_t k;
            rand_name(w->name, 1 + rnd(80));
            snprintf(w->name + strlen(w->name), 12, ".%d", i);
            w->n = rnd(4) == 0 ? 0 : rnd(6000);
            w->data = xmalloc(w->n + 1);
            for (k = 0; k < w->n; k++)
                w->data[k] = (uint8_t)(rnd(3) ? 'a' + rnd(4) : rnd(256));
            w->dos = (uint32_t)((rnd(100) << 25) | ((1 + rnd(12)) << 21) | ((1 + rnd(28)) << 16) | (rnd(24) << 11) | (rnd(60) << 5) | rnd(30));
            w->mode = 0100000u | rnd(0777);
            w->method = rnd(2) ? 8 : 0;
            if (w->method == 8) {
                CHECK(arck_zip_deflate_raw(w->data, w->n, &w->comp, &w->cn) == 0, "zip deflate");
            } else {
                w->comp = malloc(w->n + 1);
                memcpy(w->comp, w->data, w->n);
                w->cn = w->n;
            }
        }
        zip_build(&b, W, nw, bad);
        memset(&z, 0, sizeof z);
        z.p = b.p;
        z.n = b.n;
        CHECK(arck_zip_open(&z) == 0 && z.count == (uint32_t)nw, "zip open it %d: %s", it, z.err);
        if (bad == 2 && nw > 1) {
            CHECK(arck_zip_preflight(&z) == -1, "zip it %d: overlapping members refused", it);
        } else {
            size_t off = z.cd_off;
            CHECK(arck_zip_preflight(&z) == 0, "zip preflight it %d: %s", it, z.err);
            for (i = 0; i < nw; i++) {
                zip_ent_t e;
                dyn_outbuf_t o = { NULL, 0, 0, 0, 0 };
                int rc = arck_zip_entry_at(&z, &off, &e);
                CHECK(rc == 1 && strcmp(e.name, W[i].name) == 0 && e.usize == W[i].n && e.mode == W[i].mode && e.mtime == W[i].dos && (int)e.method == W[i].method,
                    "zip it %d entry %d: rc %d '%s'", it, i, rc, e.name);
                if (rc != 1)
                    break;
                rc = arck_zip_member(&z, &e, &o);
                if (bad == 1 && i == nw - 1)
                    CHECK(rc == -1 && (strstr(z.err, "CRC") || strstr(z.err, "does not inflate")), "zip it %d: CRC damage refused (%s)", it, z.err);
                else
                    CHECK(rc == 0 && o.len == W[i].n && (W[i].n == 0 || memcmp(o.buf, W[i].data, W[i].n) == 0), "zip it %d entry %d body (%s)", it, i, z.err);
                free(o.buf);
            }
        }
        for (i = 0; i < nw; i++) {
            free(W[i].data);
            free(W[i].comp);
        }
        free(b.p);
    }
    {
        zip_rd_t z;
        static const uint8_t junk[64];
        memset(&z, 0, sizeof z);
        z.p = junk;
        z.n = sizeof junk;
        CHECK(arck_zip_open(&z) == -1, "zip without EOCD refused");
    }
}

static int leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int mlen(int64_t y, int m)
{
    static const int T[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : T[m - 1];
}

static void test_dos(void)
{
    int64_t day = 3652, y = 1980;
    int m = 1, d = 1;
    while (y < 2108) {
        int64_t t = day * 86400 + (int64_t)rnd(86400);
        uint32_t dos = arck_zip_dos_from_unix(t);
        int64_t tod = t % 86400;
        uint32_t want = (uint32_t)((((uint32_t)(y - 1980)) << 25) | ((uint32_t)m << 21) | ((uint32_t)d << 16) | ((uint32_t)(tod / 3600) << 11) | ((uint32_t)(tod % 3600 / 60) << 5) | (uint32_t)(tod % 60 / 2));
        CHECK(dos == want, "dos_from_unix %lld (%lld-%d-%d)", (long long)t, (long long)y, m, d);
        CHECK(arck_zip_unix_from_dos(want) == t - (t % 2), "unix_from_dos %lld-%d-%d", (long long)y, m, d);
        day += 1 + rnd(20);
        d += 0;
        {
            int64_t k = day, yy = 1970;
            int mm = 1;
            while (k >= (leap(yy) ? 366 : 365)) {
                k -= leap(yy) ? 366 : 365;
                yy++;
            }
            while (k >= mlen(yy, mm)) {
                k -= mlen(yy, mm);
                mm++;
            }
            y = yy;
            m = mm;
            d = (int)k + 1;
        }
    }
    CHECK(arck_zip_dos_from_unix(0) == 0, "dos 0");
    CHECK(arck_zip_dos_from_unix(100) == arck_zip_dos_from_unix(315532800LL), "dos clamps before 1980");
    CHECK(arck_zip_dos_from_unix(5000000000LL) == arck_zip_dos_from_unix(4354819198LL), "dos clamps after 2107");
    CHECK(arck_zip_unix_from_dos(0) == 0 && arck_zip_unix_from_dos((13u << 21) | (1u << 16)) == 0, "bad dos month is 0");
}

int main(void)
{
    crc_init();
    test_tar();
    test_zip();
    test_dos();
    printf("test_arck_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
