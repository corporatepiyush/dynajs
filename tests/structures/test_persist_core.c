/* test_persist_core.c -- the dyn-persist file format, writer and loader, alone.
 *
 * Why each block exists:
 *  - round trip: sections come back byte-equal; page sections sit on a 16 KiB
 *    boundary so a structure can MAP_FIXED them into its own reservation, in
 *    copy and mmap mode.
 *  - every superblock/active-slot byte flipped, a byte flipped in each section,
 *    a truncation at every section boundary, a wrong version and a wrong type
 *    each come back as a documented error code, never a crash (ASan+UBSan via
 *    `tests/ctest/run.sh test-persist-core`). A flip in the unused header slot is
 *    harmless by design: it is the slot the next incremental save overwrites.
 *  - incremental saves are shadow-paged: dirty 16 KiB chunks are appended and
 *    listed in an overlay table, non-page sections are relocated whole, and the
 *    commit is one write of the OLDER of two header slots. Base data is never
 *    rewritten, so a reader that mmap'd the file keeps a stable view.
 *  - crash injection (built with -DDYN_PERSIST_FAILPOINTS): a crash or torn
 *    write at every write/fsync/rename step of a full save and of a first and a
 *    later incremental save must leave a file that loads as exactly the old or
 *    exactly the new content, never a checksum error and never a mix. The
 *    power-loss variant also drops every unsynced write but the newest, the
 *    reordering a disk cache may do; it is what proves the fsync between the
 *    appended data and the header commit. With a
 *    single header slot the torn-commit case loses the file; that is the mutant
 *    this block kills.
 *  - a chunk changed without its dirty bit is caught on load as a checksum
 *    error (never silently stale); lineage/generation mismatch falls back to a
 *    full atomic save; the atomic save always produces a new inode; appended
 *    space past the base triggers a compacting full save.
 *  - --emit-seeds DIR writes the fuzz_persist seed corpus (src/fuzz/corpus_*
 *    is git-ignored; tests/test-data.tsv regenerates it through
 *    `tests/ctest/run.sh persist-seeds`). The lineage is pinned so the bytes, and the
 *    manifest sha256, are reproducible; every normal run also emits the seeds
 *    into its scratch dir and loads each one back, overlay seed included.
 *  - dyn_persist_dirty_mark_range marks every chunk a byte range touches
 *    (ranges that straddle chunk and word boundaries, 784-byte records), and
 *    dyn_persist_from_bytes hands out sections at least 64-byte aligned and
 *    page sections host-page aligned, as open() does, so a structure can
 *    adopt them for SIMD scans.
 *  - durability: on Darwin fsync() leaves data in the drive cache, so every
 *    sync of the file and its directory goes through fcntl(F_FULLFSYNC)
 *    (falling back to fsync where the filesystem refuses it); a full save
 *    must issue at least two full syncs.
 *  - params are state, not layout: they change on every mutate() and still
 *    commit incrementally through the new slot (CountMinSketch keeps its running
 *    total there). Layout is pinned by each section's kind/tag/elem/flags/len.
 */
#include "dyn-persist.h"
#include "dyn-hash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../lib/dyn-xalloc.h"

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

#define SLOT0 DYN_PERSIST_SUPER
#define NSECT 4u
#define HLEN (DYN_PERSIST_HDR + NSECT * DYN_PERSIST_SEC)

static char dir[1024];

static void path_of(char* out, size_t n, const char* name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static int count_tmp(void)
{
    DIR* d = opendir(dir);
    struct dirent* de;
    int n = 0;
    if (!d)
        return -1;
    while ((de = readdir(d)))
        if (strstr(de->d_name, ".tmp."))
            n++;
    closedir(d);
    return n;
}

static void clean_tmp(void)
{
    DIR* d = opendir(dir);
    struct dirent* de;
    char p[2200];
    if (!d)
        return;
    while ((de = readdir(d)))
        if (strstr(de->d_name, ".tmp.")) {
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            unlink(p);
        }
    closedir(d);
}

#define NBIG 40000
static uint64_t big[NBIG];
static uint32_t small[37];
static uint64_t dirty[8];
static char rec[32] = "record-bytes-encoded-by-a-codec";
static uint64_t gparam = 6;

static void build(dyn_persist_w* w)
{
    dyn_persist_w_begin(w, 77, 12345);
    dyn_persist_w_param(w, 0, gparam);
    dyn_persist_w_param(w, 7, 0xdeadbeef);
    dyn_persist_w_ref_dirty(w, 1, 0, 8, DYN_PERSIST_S_PAGE, big, sizeof big, dirty, 12);
    dyn_persist_w_ref(w, 2, 0, 4, 0, small, sizeof small);
    dyn_persist_w_open(w, 3, 9, 1, 0);
    dyn_persist_w_put(w, rec, 10);
    dyn_persist_w_put(w, rec + 10, sizeof rec - 10);
    dyn_persist_w_close(w);
    dyn_persist_w_ref(w, 4, 0, 8, DYN_PERSIST_S_PAGE, NULL, 0);
}

typedef struct {
    uint64_t big[NBIG];
    uint32_t small[37];
    char rec[32];
    uint64_t param;
} snap;

static void take(snap* s)
{
    memcpy(s->big, big, sizeof big);
    memcpy(s->small, small, sizeof small);
    memcpy(s->rec, rec, sizeof rec);
    s->param = gparam;
}

static int matches(const dyn_persist_r* r, const snap* s)
{
    const dyn_persist_rsect *a = dyn_persist_find(r, 1, 0), *b = dyn_persist_find(r, 2, 0),
                            *c = dyn_persist_find(r, 3, 9), *z = dyn_persist_find(r, 4, 0);
    if (r->type_id != 77 || r->count != 12345 || r->nsect != NSECT || r->params[0] != s->param || r->params[7] != 0xdeadbeef)
        return 0;
    if (!a || !b || !c || !z || z->len != 0)
        return 0;
    if (a->len != sizeof s->big || memcmp(a->p, s->big, sizeof s->big) != 0 || a->off % DYN_PERSIST_PAGE)
        return 0;
    if (b->len != sizeof s->small || memcmp(b->p, s->small, sizeof s->small) != 0)
        return 0;
    return c->len == 32 && memcmp(c->p, s->rec, 32) == 0;
}

static int validate(const uint8_t* img, size_t n, uint32_t type)
{
    dyn_persist_r r;
    dyn_persist_err e;
    int rc;
    memset(&e, 0, sizeof e);
    rc = dyn_persist_validate(img, n, 1, type, &r, &e);
    if (rc == 0)
        dyn_persist_r_clear(&r);
    else
        CHECK(e.code == rc && e.msg[0]);
    return rc;
}

static void refix(uint8_t* img)
{
    uint32_t c;
    memset(img + SLOT0 + 128, 0, 4);
    c = dyn_crc32c(img + SLOT0, HLEN);
    memcpy(img + SLOT0 + 128, &c, 4);
}

static void put64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static void test_bounds(const uint8_t* img, size_t n)
{
    uint8_t* cp = malloc(n);
    uint8_t* h = cp + SLOT0;
    uint8_t* t0 = h + DYN_PERSIST_HDR;
    uint8_t* t1 = t0 + DYN_PERSIST_SEC;
    uint64_t off0, len0, off1;
    memcpy(&off0, img + SLOT0 + DYN_PERSIST_HDR + 16, 8);
    memcpy(&len0, img + SLOT0 + DYN_PERSIST_HDR + 24, 8);
    memcpy(&off1, img + SLOT0 + DYN_PERSIST_HDR + DYN_PERSIST_SEC + 16, 8);
#define BAD(stmt)                                            \
    do {                                                     \
        memcpy(cp, img, n);                                  \
        stmt;                                                \
        refix(cp);                                           \
        CHECK(validate(cp, n, 77) == DYN_PERSIST_E_CORRUPT); \
    } while (0)
    BAD(put64(t1 + 16, n));
    BAD(put64(t1 + 16, n + 64));
    BAD(put64(t1 + 24, n));
    BAD(put64(t1 + 24, UINT64_MAX - 63));
    BAD(put64(t1 + 16, off1 + 8));
    BAD(put64(t1 + 16, 64));
    BAD(put64(t0 + 16, off0 + 64));
    BAD(put64(t0 + 24, len0 + 8 * 4096));
    BAD(memset(t1 + 8, 0, 4));
    BAD(t1[8] = 3);
    BAD(t1[12] = 2);
    BAD(t1[36] = 1);
    BAD(h[29] = 0x20);
    BAD(put64(h + 136, 64));
    BAD(put64(h + 144, 8); h[152] = 1);
    BAD(put64(h + 144, n - 8); h[152] = 1);
    BAD(memset(h + 152, 0xff, 4));
#undef BAD
    memcpy(cp, img, n);
    put64(h + 40, (uint64_t)n + 1);
    refix(cp);
    CHECK(validate(cp, n, 77) == DYN_PERSIST_E_TRUNCATED);
    memcpy(cp, img, n);
    cp[20] = 1;
    CHECK(validate(cp, n, 77) == DYN_PERSIST_E_CHECKSUM);
    free(cp);
}

static void test_image_and_corruption(void)
{
    dyn_persist_w w;
    uint8_t *img, *cp;
    size_t n, i, k;
    dyn_persist_r r;
    dyn_persist_err e;
    snap* now = malloc(sizeof *now);
    uint32_t slot_size;
    take(now);
    dyn_persist_w_init(&w);
    build(&w);
    CHECK(dyn_persist_w_image(&w, &img, &n) == 0);
    slot_size = w.slot_size;
    CHECK(slot_size == ((HLEN + 63) & ~63u));
    CHECK(n % DYN_PERSIST_PAGE == 0);
    CHECK(dyn_persist_validate(img, n, 1, 77, &r, &e) == 0);
    CHECK(matches(&r, now) && r.slot == 0 && r.gen == 1 && r.nov == 0);
    cp = xmalloc(n + 64);
    for (i = 0; i < SLOT0 + HLEN; i++) {
        memcpy(cp, img, n);
        cp[i] ^= 0x40;
        CHECK(validate(cp, n, 77) != 0);
    }
    for (i = SLOT0 + slot_size; i < SLOT0 + 2 * slot_size; i++) {
        memcpy(cp, img, n);
        cp[i] ^= 0x40;
        CHECK(validate(cp, n, 77) == 0);
    }
    for (k = 0; k < r.nsect; k++) {
        if (!r.s[k].len)
            continue;
        for (i = 0; i < 3; i++) {
            size_t at = (size_t)r.s[k].off + (size_t)(i * (r.s[k].len - 1) / 2);
            memcpy(cp, img, n);
            cp[at] ^= 1;
            CHECK(validate(cp, n, 77) == DYN_PERSIST_E_CHECKSUM);
        }
    }
    for (k = 0; k < r.nsect; k++) {
        size_t cuts[3] = { (size_t)r.s[k].off, (size_t)(r.s[k].off + r.s[k].len), (size_t)(r.s[k].off + r.s[k].len / 2) };
        for (i = 0; i < 3; i++)
            if (cuts[i] < n)
                CHECK(validate(img, cuts[i], 77) == DYN_PERSIST_E_TRUNCATED);
    }
    for (i = 0; i < SLOT0 + 2 * slot_size; i++) {
        int rc = validate(img, i, 77);
        CHECK(rc == DYN_PERSIST_E_TRUNCATED || rc == DYN_PERSIST_E_MAGIC);
    }
    CHECK(validate(img, n - 1, 77) == DYN_PERSIST_E_TRUNCATED);
    memcpy(cp, img, n);
    cp[8] = 2;
    CHECK(validate(cp, n, 77) == DYN_PERSIST_E_VERSION);
    CHECK(validate(img, n, 78) == DYN_PERSIST_E_TYPE);
    memcpy(cp, img, n);
    cp[12] ^= 0xff;
    CHECK(validate(cp, n, 77) == DYN_PERSIST_E_ENDIAN);
    memcpy(cp, img, n);
    cp[0] = 'X';
    CHECK(validate(cp, n, 77) == DYN_PERSIST_E_MAGIC);
    CHECK(dyn_persist_validate(img, n, 0, 0, NULL, NULL) == 0);
    memcpy(cp, img, n);
    memset(cp + n, 0xab, 64);
    CHECK(validate(cp, n + 64, 77) == 0);
    test_bounds(img, n);
    {
        uint32_t seed = 1;
        for (i = 0; i < 20000; i++) {
            size_t len = n, j, nflip;
            memcpy(cp, img, n);
            seed = seed * 1103515245u + 12345u;
            nflip = 1 + (seed >> 16) % 4;
            for (j = 0; j < nflip; j++) {
                seed = seed * 1103515245u + 12345u;
                cp[(seed >> 8) % (SLOT0 + HLEN)] = (uint8_t)(seed >> 3);
            }
            seed = seed * 1103515245u + 12345u;
            if (seed & 1)
                len = (seed >> 4) % (n + 1);
            (void)validate(cp, len, 0);
        }
    }
    dyn_persist_r_clear(&r);
    free(cp);
    free(img);
    free(now);
    dyn_persist_w_free(&w);
}

static int load_is(const char* p, const snap* s, int mode)
{
    dyn_persist_r* r = NULL;
    dyn_persist_err e;
    int ok;
    if (dyn_persist_open(p, mode, 1, 77, &r, &e) != 0)
        return 0;
    ok = matches(r, s);
    dyn_persist_release(r);
    return ok;
}

static int save_full(dyn_persist_w* w, const char* p, uint64_t* lineage, uint64_t* gen)
{
    dyn_persist_save_opt o;
    dyn_persist_err e;
    int rc;
    memset(&o, 0, sizeof o);
    o.fsync = 1;
    dyn_persist_w_reset(w);
    memset(dirty, 0, sizeof dirty);
    build(w);
    rc = dyn_persist_save(w, p, &o, &e);
    *lineage = w->lineage;
    *gen = w->gen;
    return rc;
}

static int save_inc(dyn_persist_w* w, const char* p, uint64_t* lineage, uint64_t* gen,
    dyn_persist_save_opt* o)
{
    dyn_persist_err e;
    int rc;
    memset(o, 0, sizeof *o);
    o->fsync = 1;
    o->incremental = 1;
    dyn_persist_w_reset(w);
    build(w);
    w->lineage = *lineage;
    w->gen = *gen;
    rc = dyn_persist_save(w, p, o, &e);
    if (rc == 0) {
        *lineage = w->lineage;
        *gen = w->gen;
    }
    return rc;
}

static void mutate(int which)
{
    memset(dirty, 0, sizeof dirty);
    big[5000 + which] += 1000003;
    dyn_persist_dirty_mark(dirty, 12, (5000 + (uint64_t)which) * 8);
    big[30000 - which] ^= 0x5555;
    dyn_persist_dirty_mark(dirty, 12, (30000 - (uint64_t)which) * 8);
    small[3] += 7;
    rec[0] = (char)('a' + which % 26);
    gparam += 11;
}

static void test_files(void)
{
    char p[1100], bad[1100];
    dyn_persist_w w;
    dyn_persist_save_opt o;
    dyn_persist_err e;
    dyn_persist_r *r, *m;
    uint64_t lineage, gen;
    snap *s0 = malloc(sizeof *s0), *s1 = malloc(sizeof *s1), *s2 = malloc(sizeof *s2);
    struct stat st1, st2;
    int mode;
    path_of(p, sizeof p, "a.dpf");
    dyn_persist_w_init(&w);
    take(s0);
#ifdef DYN_PERSIST_FAILPOINTS
    dyn_persist_fp_fullsyncs = 0;
#endif
    CHECK(save_full(&w, p, &lineage, &gen) == 0);
#if defined(DYN_PERSIST_FAILPOINTS) && defined(F_FULLFSYNC)
    CHECK(dyn_persist_fp_fullsyncs >= 2);
#endif
    CHECK(count_tmp() == 0 && gen == 1 && lineage != 0);
    for (mode = 0; mode < 2; mode++)
        CHECK(load_is(p, s0, mode));
    CHECK(dyn_persist_open(p, 0, 1, 99, &r, &e) == DYN_PERSIST_E_TYPE && !r);
    path_of(bad, sizeof bad, "missing/sub/x.dpf");
    memset(&o, 0, sizeof o);
    CHECK(dyn_persist_save(&w, bad, &o, &e) == DYN_PERSIST_E_IO && e.sys_errno == ENOENT);
    CHECK(dyn_persist_open(bad, 0, 1, 0, &r, &e) == DYN_PERSIST_E_IO);
    CHECK(count_tmp() == 0);

    CHECK(dyn_persist_open(p, DYN_PERSIST_MODE_MMAP, 1, 77, &m, &e) == 0);
    mutate(1);
    take(s1);
    CHECK(save_inc(&w, p, &lineage, &gen, &o) == 0);
    CHECK(o.wrote_incremental == 1 && gen == 2);
    CHECK(o.bytes_written < 2 * DYN_PERSIST_PAGE + 4096);
    for (mode = 0; mode < 2; mode++)
        CHECK(load_is(p, s1, mode));
    if (m) {
        CHECK(matches(m, s0));
        dyn_persist_release(m);
    }
    CHECK(dyn_persist_open(p, 0, 1, 77, &r, &e) == 0);
    if (r) {
        CHECK(r->nov == 2 && r->slot == 1 && r->gen == 2);
        dyn_persist_release(r);
    }

    mutate(2);
    take(s2);
    CHECK(save_inc(&w, p, &lineage, &gen, &o) == 0 && o.wrote_incremental == 1 && gen == 3);
    for (mode = 0; mode < 2; mode++)
        CHECK(load_is(p, s2, mode));
    CHECK(dyn_persist_open(p, DYN_PERSIST_MODE_MMAP, 1, 77, &m, &e) == 0);
    if (m) {
        const dyn_persist_rsect* s = dyn_persist_find(m, 1, 0);
        size_t cap = 1u << 20;
        void* res = mmap(NULL, cap, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(m->slot == 0 && m->nov == 2);
        CHECK(res != MAP_FAILED);
        CHECK(dyn_persist_map_section(m, s, res, cap) == 0);
        CHECK(memcmp(res, big, sizeof big) == 0);
        ((uint64_t*)res)[1] = 0x55;
        CHECK(((const uint64_t*)s->p)[1] == big[1]);
        CHECK(dyn_persist_map_section(m, s, res, 4096) == -1);
        munmap(res, cap);
        dyn_persist_release(m);
    }

    {
        uint8_t* fb;
        struct stat sx;
        FILE* f;
        uint64_t ovoff = 0;
        uint32_t ovn = 0, j;
        CHECK(stat(p, &sx) == 0);
        fb = malloc((size_t)sx.st_size);
        f = fopen(p, "rb");
        CHECK(f && fread(fb, 1, (size_t)sx.st_size, f) == (size_t)sx.st_size);
        if (f)
            fclose(f);
        CHECK(dyn_persist_validate(fb, (size_t)sx.st_size, 1, 77, NULL, NULL) == 0);
        {
            dyn_persist_r vr;
            CHECK(dyn_persist_validate(fb, (size_t)sx.st_size, 0, 77, &vr, NULL) == 0);
            ovn = vr.nov;
            dyn_persist_r_clear(&vr);
        }
        memcpy(&ovoff, fb + SLOT0 + 144, 8);
        if (!ovn)
            memcpy(&ovoff, fb + SLOT0 + (size_t)((HLEN + 63) & ~63u) + 144, 8);
        CHECK(ovn == 2 && ovoff);
        for (j = 0; j < ovn * DYN_PERSIST_OV && ovoff; j++) {
            fb[ovoff + j] ^= 0x10;
            CHECK(validate(fb, (size_t)sx.st_size, 77) == DYN_PERSIST_E_CHECKSUM);
            fb[ovoff + j] ^= 0x10;
        }
        free(fb);
    }
    memset(dirty, 0, sizeof dirty);
    big[20000] ^= 1;
    CHECK(save_inc(&w, p, &lineage, &gen, &o) == 0 && o.wrote_incremental == 1);
    CHECK(dyn_persist_open(p, 0, 1, 77, &r, &e) == DYN_PERSIST_E_CHECKSUM);
    big[20000] ^= 1;

    {
        uint64_t stale = gen - 1;
        memset(dirty, 0xff, sizeof dirty);
        CHECK(stat(p, &st1) == 0);
        CHECK(save_inc(&w, p, &lineage, &stale, &o) == 0 && o.wrote_incremental == 0);
        CHECK(stat(p, &st2) == 0 && st1.st_ino != st2.st_ino);
        gen = stale;
        take(s0);
        CHECK(load_is(p, s0, 0));
    }
    {
        int rounds = 0;
        memset(&o, 0, sizeof o);
        do {
            mutate(rounds + 10);
            take(s0);
            CHECK(save_inc(&w, p, &lineage, &gen, &o) == 0);
            CHECK(load_is(p, s0, rounds & 1));
        } while (o.wrote_incremental && ++rounds < 400);
        CHECK(!o.wrote_incremental && rounds > 2);
        CHECK(stat(p, &st1) == 0 && (uint64_t)st1.st_size == o.file_size);
    }
    CHECK(count_tmp() == 0);
    unlink(p);
    dyn_persist_w_free(&w);
    free(s0);
    free(s1);
    free(s2);
}

#ifdef DYN_PERSIST_FAILPOINTS
static void crash_matrix(int incremental_rounds)
{
    char p[1100];
    dyn_persist_w w;
    dyn_persist_save_opt o;
    snap *old = malloc(sizeof *old), *neu = malloc(sizeof *neu);
    uint64_t lineage, gen;
    int k, torn, mode, steps = 0;
    path_of(p, sizeof p, "crash.dpf");
    dyn_persist_w_init(&w);
    for (torn = 0; torn < 4; torn++) {
        for (k = 1; k < 64; k++) {
            int rc, i;
            unlink(p);
            dyn_persist_fp_countdown = 0;
            mutate(k);
            CHECK(save_full(&w, p, &lineage, &gen) == 0);
            for (i = 0; i < incremental_rounds; i++) {
                mutate(k + 100 + i);
                CHECK(save_inc(&w, p, &lineage, &gen, &o) == 0 && o.wrote_incremental);
            }
            take(old);
            mutate(k + 1000);
            take(neu);
            dyn_persist_fp_hits = 0;
            dyn_persist_fp_torn = torn & 1;
            dyn_persist_fp_powerloss = torn >> 1;
            dyn_persist_fp_countdown = k;
            if (incremental_rounds < 0)
                rc = save_full(&w, p, &lineage, &gen);
            else
                rc = save_inc(&w, p, &lineage, &gen, &o);
            dyn_persist_fp_countdown = 0;
            if (!dyn_persist_fp_hits) {
                CHECK(rc == 0);
                for (mode = 0; mode < 2; mode++)
                    CHECK(load_is(p, neu, mode));
                break;
            }
            steps++;
            CHECK(rc != 0);
            for (mode = 0; mode < 2; mode++) {
                int is_old = load_is(p, old, mode), is_new = load_is(p, neu, mode);
                if (!(is_old || is_new))
                    fprintf(stderr, "  crash at step %d (torn=%d, rounds=%d, mode=%d): neither old nor new\n",
                        k, torn, incremental_rounds, mode);
                CHECK(is_old || is_new);
            }
            clean_tmp();
        }
    }
    dyn_persist_fp_powerloss = 0;
    dyn_persist_fp_torn = 0;
    CHECK(steps >= 12);
    unlink(p);
    dyn_persist_w_free(&w);
    free(old);
    free(neu);
}
#endif

static void test_thread_writer(void)
{
    dyn_persist_w* a = dyn_persist_w_thread();
    uint8_t* buf;
    size_t cap;
    CHECK(a != NULL);
    dyn_persist_w_open(a, 1, 0, 1, 0);
    dyn_persist_w_put(a, big, 100000);
    dyn_persist_w_close(a);
    buf = a->buf;
    cap = a->cap;
    CHECK(dyn_persist_w_thread() == a && a->buf == buf && a->cap == cap && a->len == 0 && a->ns == 0);
}

static void test_mark_range_and_align(void)
{
    uint64_t b[3];
    dyn_persist_w w;
    uint8_t* img;
    size_t n;
    int k;
    memset(b, 0, sizeof b);
    dyn_persist_dirty_mark_range(b, 14, 0, 0);
    CHECK(b[0] == 0);
    dyn_persist_dirty_mark_range(b, 14, 16383, 2);
    CHECK(b[0] == 3);
    dyn_persist_dirty_mark_range(b, 14, 5 * 16384, 16384);
    CHECK(b[0] == (3 | ((uint64_t)1 << 5)));
    memset(b, 0, sizeof b);
    dyn_persist_dirty_mark_range(b, 10, 62 * 1024 + 7, 3 * 1024);
    CHECK(b[0] == ((uint64_t)3 << 62) && b[1] == 3 && b[2] == 0);
    memset(b, 0, sizeof b);
    dyn_persist_dirty_mark_range(b, 4, 784, 784);
    CHECK(b[0] == (~(uint64_t)0 << 49) && b[1] == (((uint64_t)1 << 34) - 1) && b[2] == 0);
    dyn_persist_w_init(&w);
    dyn_persist_w_begin(&w, 3, 1);
    dyn_persist_w_ref(&w, 1, 0, 1, 0, rec, 5);
    dyn_persist_w_ref(&w, 1, 1, 8, 0, small, 16);
    dyn_persist_w_ref(&w, 1, 2, 8, DYN_PERSIST_S_PAGE, big, 4096);
    CHECK(dyn_persist_w_image(&w, &img, &n) == 0);
    for (k = 0; k < 16; k++) {
        dyn_persist_r* r = NULL;
        dyn_persist_err e;
        void* noise = malloc((size_t)(16 + 16 * k));
        CHECK(dyn_persist_from_bytes(img, n, 1, 3, &r, &e) == 0);
        if (r) {
            size_t hp = dyn_persist_host_page();
            CHECK(((uintptr_t)r->s[0].p % DYN_PERSIST_ALIGN) == 0 && ((uintptr_t)r->s[1].p % DYN_PERSIST_ALIGN) == 0);
            CHECK(((uintptr_t)r->s[2].p % (hp < DYN_PERSIST_PAGE ? hp : DYN_PERSIST_PAGE)) == 0);
            dyn_persist_release(r);
        }
        free(noise);
    }
    free(img);
    dyn_persist_w_free(&w);
}

static void test_chunk_diff(void)
{
    uint64_t h[80], d[2];
    CHECK(dyn_persist_chunks(sizeof big, 12) == 79);
    CHECK(dyn_persist_chunks(0, 12) == 0 && dyn_persist_chunks(1, 12) == 1);
    memset(d, 0, sizeof d);
    dyn_persist_chunk_diff(big, sizeof big, 12, h, 0, d);
    CHECK(d[0] == UINT64_MAX && d[1] == ((uint64_t)1 << 15) - 1);
    dyn_persist_chunk_diff(big, sizeof big, 12, h, 1, d);
    CHECK(d[0] == 0 && d[1] == 0);
    big[600] ^= 1;
    big[39999] ^= 1;
    dyn_persist_chunk_diff(big, sizeof big, 12, h, 1, d);
    CHECK(d[0] == ((uint64_t)1 << 1) && d[1] == ((uint64_t)1 << 14));
    big[600] ^= 1;
    big[39999] ^= 1;
}

static void test_writer_errors(void)
{
    dyn_persist_w w;
    dyn_persist_w_init(&w);
    CHECK(dyn_persist_w_ref(&w, 1, 0, 0, 0, big, 8) < 0);
    dyn_persist_w_reset(&w);
    CHECK(dyn_persist_w_ref(&w, 1, 0, 8, 0, big, 12) < 0);
    dyn_persist_w_reset(&w);
    CHECK(dyn_persist_w_ref(&w, 1, 0, 8, 2, big, 8) < 0);
    dyn_persist_w_reset(&w);
    dyn_persist_w_open(&w, 1, 0, 4, 0);
    dyn_persist_w_put(&w, big, 3);
    CHECK(dyn_persist_w_close(&w) < 0);
    CHECK(dyn_persist_w_layout(&w) < 0);
    dyn_persist_w_free(&w);
}

static int emit_one(dyn_persist_w* w, const char* d, const char* name)
{
    char p[1100];
    uint8_t* img;
    size_t n;
    FILE* f;
    snprintf(p, sizeof p, "%s/%s", d, name);
    w->lineage = 0x5eed5eed5eed5eedULL;
    if (dyn_persist_w_image(w, &img, &n) != 0)
        return -1;
    f = fopen(p, "wb");
    if (!f || fwrite(img, 1, n, f) != n || fclose(f) != 0) {
        free(img);
        return -1;
    }
    free(img);
    return 0;
}

static int emit_seeds(const char* d)
{
    static uint64_t page[4096];
    uint64_t pdirty[1] = { 0 };
    uint32_t a[16];
    char p[1100];
    dyn_persist_w w;
    dyn_persist_save_opt o;
    dyn_persist_err e;
    uint64_t gen;
    int i, rc = 0;
    for (i = 0; i < 16; i++)
        a[i] = (uint32_t)i * 7u;
    for (i = 0; i < 4096; i++)
        page[i] = (uint64_t)i * 2654435761u;
    dyn_persist_w_init(&w);
    dyn_persist_w_begin(&w, 7, 16);
    dyn_persist_w_param(&w, 0, 64);
    dyn_persist_w_ref(&w, 1, 0, 4, 0, a, sizeof a);
    dyn_persist_w_open(&w, 2, 0, 1, 0);
    dyn_persist_w_put(&w, "DYNSxxxx", 8);
    dyn_persist_w_close(&w);
    rc |= emit_one(&w, d, "valid_two_sections.bin");
    dyn_persist_w_reset(&w);
    dyn_persist_w_begin(&w, 1, 0);
    rc |= emit_one(&w, d, "valid_empty.bin");
    dyn_persist_w_reset(&w);
    dyn_persist_w_begin(&w, 9, 3);
    dyn_persist_w_ref(&w, 1, 0, 8, 0, a, 64);
    dyn_persist_w_ref(&w, 1, 1, 4, 0, a, 8);
    dyn_persist_w_ref(&w, 3, 0, 2, 0, a, 6);
    rc |= emit_one(&w, d, "valid_three_small.bin");
    snprintf(p, sizeof p, "%s/valid_overlay.bin", d);
    unlink(p);
    dyn_persist_w_reset(&w);
    dyn_persist_w_begin(&w, 5, 4096);
    w.lineage = 0x5eed5eed5eed5eedULL;
    dyn_persist_w_ref_dirty(&w, 1, 0, 8, DYN_PERSIST_S_PAGE, page, sizeof page, pdirty, 14);
    dyn_persist_w_ref(&w, 2, 0, 4, 0, a, sizeof a);
    memset(&o, 0, sizeof o);
    rc |= dyn_persist_save(&w, p, &o, &e);
    gen = w.gen;
    page[3000] = 7;
    pdirty[0] = 2;
    dyn_persist_w_reset(&w);
    dyn_persist_w_begin(&w, 5, 4096);
    w.lineage = 0x5eed5eed5eed5eedULL;
    w.gen = gen;
    dyn_persist_w_ref_dirty(&w, 1, 0, 8, DYN_PERSIST_S_PAGE, page, sizeof page, pdirty, 14);
    dyn_persist_w_ref_dirty(&w, 2, 0, 4, 0, a, sizeof a, pdirty, 14);
    memset(&o, 0, sizeof o);
    o.incremental = 1;
    rc |= dyn_persist_save(&w, p, &o, &e);
    if (!o.wrote_incremental)
        rc = -1;
    dyn_persist_w_free(&w);
    return rc;
}

static void test_seeds(void)
{
    static const char* const names[] = { "valid_two_sections.bin", "valid_empty.bin",
        "valid_three_small.bin", "valid_overlay.bin" };
    char d[1100], p[1100];
    size_t i;
    int mode;
    snprintf(d, sizeof d, "%s/seeds", dir);
    mkdir(d, 0755);
    CHECK(emit_seeds(d) == 0);
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", d, names[i]);
        for (mode = 0; mode < 2; mode++) {
            dyn_persist_r* r = NULL;
            dyn_persist_err e;
            CHECK(dyn_persist_open(p, mode, 1, 0, &r, &e) == 0);
            if (r) {
                CHECK(r->lineage == 0x5eed5eed5eed5eedULL);
                CHECK(i != 3 || (r->nov == 1 && r->gen == 2 && r->s[0].p[3000 * 8] == 7));
                dyn_persist_release(r);
            }
        }
    }
}

int main(int argc, char** argv)
{
    size_t i;
    if (argc == 3 && strcmp(argv[1], "--emit-seeds") == 0) {
        mkdir(argv[2], 0755);
        if (emit_seeds(argv[2]) != 0) {
            fprintf(stderr, "test_persist_core: --emit-seeds %s failed\n", argv[2]);
            return 1;
        }
        return 0;
    }
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : ".obj/persist-core-test");
    mkdir(dir, 0755);
    for (i = 0; i < NBIG; i++)
        big[i] = i * 0x9E3779B97F4A7C15ULL;
    for (i = 0; i < sizeof small / sizeof small[0]; i++)
        small[i] = (uint32_t)i * 3u;
    test_image_and_corruption();
    test_files();
    test_thread_writer();
    test_chunk_diff();
    test_mark_range_and_align();
    test_writer_errors();
    test_seeds();
#ifdef DYN_PERSIST_FAILPOINTS
    crash_matrix(-1);
    crash_matrix(0);
    crash_matrix(1);
    crash_matrix(3);
    printf("test_persist_core: crash matrix covered\n");
#endif
    if (fails) {
        fprintf(stderr, "test_persist_core: %d failures\n", fails);
        return 1;
    }
    printf("test_persist_core: ok\n");
    return 0;
}
