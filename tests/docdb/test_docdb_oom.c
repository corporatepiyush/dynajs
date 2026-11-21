/* tests/docdb/test_docdb_oom.c -- docdb under allocation failure. The docdb sources are built with
 * malloc/calloc/realloc/free/strdup/strndup renamed to the shims below; each case counts the tracked
 * allocations of one clean run, then fails the k-th one for every k (sampled past 600) and asserts: no crash
 * (ASan), a clean DD_E_OOM error, and no tracked block left (live-set oracle). Cases: dd_agg_compile of a
 * random $facet pipeline, dd_snap_read of a random 1-3 collection snapshot (with or without a secondary index,
 * so snap_docs grows buffers it already owns); an OOM inside
 * document or index rebuild must be DD_E_OOM, not DD_E_BADSNAPSHOT. Found by tools/js/defects.js D03 (x = realloc(x)
 * in compile_facet and snap_docs: the NULL result replaced the only pointer, so the facet free walked NULL
 * and the snapshot buffers leaked). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/docdb/dd.h"
#include "../lib/dyn-fuzzkit.h"

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
#undef strndup
void* malloc(size_t n);
void* calloc(size_t a, size_t b);
void* realloc(void* p, size_t n);
void free(void* p);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);

#define LIVE_BITS 20
#define LIVE_CAP (1u << LIVE_BITS)
static void* live[LIVE_CAP];
static size_t nlive;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long fail_at, calls;

static size_t slot(const void* p)
{
    uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ULL;
    return (size_t)(h >> (64 - LIVE_BITS));
}

static void live_add(void* p)
{
    size_t i = slot(p);
    while (live[i])
        i = (i + 1) & (LIVE_CAP - 1);
    live[i] = p;
    nlive++;
}

static void live_del(void* p)
{
    size_t i = slot(p), j, k;
    while (live[i] && live[i] != p)
        i = (i + 1) & (LIVE_CAP - 1);
    if (!live[i])
        return;
    live[i] = NULL;
    nlive--;
    for (j = (i + 1) & (LIVE_CAP - 1); live[j]; j = (j + 1) & (LIVE_CAP - 1)) {
        k = slot(live[j]);
        if ((j > i && (k <= i || k > j)) || (j < i && k <= i && k > j)) {
            live[i] = live[j];
            live[j] = NULL;
            i = j;
        }
    }
}

static int t_fail(void)
{
    int f;
    pthread_mutex_lock(&mu);
    f = fail_at && ++calls == fail_at;
    if (!fail_at)
        calls++;
    pthread_mutex_unlock(&mu);
    return f;
}

static void* track(void* p)
{
    if (p) {
        pthread_mutex_lock(&mu);
        live_add(p);
        pthread_mutex_unlock(&mu);
    }
    return p;
}

void* dd_t_malloc(size_t n)
{
    return t_fail() ? NULL : track(malloc(n));
}

void* dd_t_calloc(size_t a, size_t b)
{
    return t_fail() ? NULL : track(calloc(a, b));
}

char* dd_t_strdup(const char* s)
{
    return t_fail() ? NULL : track(strdup(s));
}

char* dd_t_strndup(const char* s, size_t n)
{
    return t_fail() ? NULL : track(strndup(s, n));
}

void* dd_t_realloc(void* p, size_t n)
{
    uintptr_t old = (uintptr_t)p;
    void* q;
    if (t_fail())
        return NULL;
    q = realloc(p, n);
    if (q) {
        pthread_mutex_lock(&mu);
        if (old)
            live_del((void*)old);
        live_add(q);
        pthread_mutex_unlock(&mu);
    }
    return q;
}

void dd_t_free(void* p)
{
    if (p) {
        pthread_mutex_lock(&mu);
        live_del(p);
        pthread_mutex_unlock(&mu);
    }
    free(p);
}

int lre_check_stack_overflow(void* opaque, size_t alloca_size)
{
    (void)opaque;
    (void)alloca_size;
    return 0;
}

int lre_check_timeout(void* opaque)
{
    (void)opaque;
    return 0;
}

void* lre_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    if (size == 0) {
        dd_t_free(ptr);
        return NULL;
    }
    return dd_t_realloc(ptr, size);
}

static size_t live_now(void)
{
    size_t n;
    pthread_mutex_lock(&mu);
    n = nlive;
    pthread_mutex_unlock(&mu);
    return n;
}

static void arm(long k)
{
    pthread_mutex_lock(&mu);
    fail_at = k;
    calls = 0;
    pthread_mutex_unlock(&mu);
}

static long disarm(void)
{
    long n;
    pthread_mutex_lock(&mu);
    n = calls;
    fail_at = 0;
    pthread_mutex_unlock(&mu);
    return n;
}

static void put_stage(fk_rng_t* r, dyn_bson_w* w, const char* idx)
{
    dyn_bson_w_sub_begin(w, DYN_BSON_DOC, idx, strlen(idx));
    switch (fk_below(r, 3)) {
    case 0:
        dyn_bson_w_int64(w, "$limit", 6, 1 + (int64_t)fk_below(r, 9));
        break;
    case 1:
        dyn_bson_w_int64(w, "$skip", 5, (int64_t)fk_below(r, 9));
        break;
    default:
        dyn_bson_w_sub_begin(w, DYN_BSON_DOC, "$match", 6);
        dyn_bson_w_int32(w, "a", 1, (int32_t)fk_below(r, 7));
        dyn_bson_w_sub_end(w);
        break;
    }
    dyn_bson_w_sub_end(w);
}

static void build_facet(fk_rng_t* r, dyn_bson_w* w)
{
    unsigned nf = 1 + fk_below(r, 6), f, k, ns;
    char key[16], idx[16];
    dyn_bson_w_init(w);
    dyn_bson_w_doc_begin(w);
    dyn_bson_w_sub_begin(w, DYN_BSON_DOC, "0", 1);
    dyn_bson_w_sub_begin(w, DYN_BSON_DOC, "$facet", 6);
    for (f = 0; f < nf; f++) {
        snprintf(key, sizeof key, "f%u", f);
        dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, key, strlen(key));
        ns = 1 + fk_below(r, 3);
        for (k = 0; k < ns; k++) {
            snprintf(idx, sizeof idx, "%u", k);
            put_stage(r, w, idx);
        }
        dyn_bson_w_sub_end(w);
    }
    dyn_bson_w_sub_end(w);
    dyn_bson_w_sub_end(w);
    dyn_bson_w_doc_end(w);
}

static long sweep_step(long n, long k)
{
    return n <= 600 ? 1 : n / 600 + (k & 1);
}

static void case_facet(void)
{
    unsigned it, iters = (unsigned)fk_iters(12);
    fk_rng_t r = fk_stream("facet");
    for (it = 0; it < iters; it++) {
        dyn_bson_w w;
        dd_agg* a = NULL;
        dd_err e;
        long n, k;
        size_t base;
        build_facet(&r, &w);
        FK_CHECK(!w.oom, "pipeline build");
        memset(&e, 0, sizeof e);
        base = live_now();
        arm(0);
        FK_CHECK(dd_agg_compile(&a, w.p, NULL, &e) == 0, "clean compile failed: %d", e.code);
        n = disarm();
        dd_agg_free(a);
        FK_CHECK(live_now() == base, "clean compile leaked %zu blocks", live_now() - base);
        for (k = 1; k <= n; k += sweep_step(n, k)) {
            int rc;
            memset(&e, 0, sizeof e);
            a = NULL;
            arm(k);
            rc = dd_agg_compile(&a, w.p, NULL, &e);
            disarm();
            if (rc == 0)
                dd_agg_free(a);
            else
                FK_CHECK(e.code == DD_E_OOM && a == NULL, "compile fail_at %ld/%ld: rc %d code %d", k, n, rc, e.code);
            FK_CHECK(live_now() == base, "compile fail_at %ld/%ld leaked %zu blocks", k, n, live_now() - base);
        }
        dyn_bson_w_free(&w);
    }
}

static int fill_coll(fk_rng_t* r, dd_coll* c)
{
    unsigned nd = fk_below(r, 400), i;
    dd_err e;
    int rc;
    memset(&e, 0, sizeof e);
    for (i = 0; i < nd; i++) {
        dyn_bson_w d;
        dyn_bson_w_init(&d);
        dyn_bson_w_doc_begin(&d);
        dyn_bson_w_int32(&d, "_id", 3, (int32_t)i);
        dyn_bson_w_int32(&d, "a", 1, (int32_t)fk_below(r, 7));
        if (fk_chance(r, 30))
            dyn_bson_w_string(&d, "s", 1, "xyz", 1 + fk_below(r, 3));
        dyn_bson_w_doc_end(&d);
        rc = dd_coll_insert(c, d.p, d.n, NULL, &e);
        dyn_bson_w_free(&d);
        if (rc < 0)
            return -1;
    }
    if (fk_chance(r, 70)) {
        dyn_bson_w ks, ko;
        dd_index* ix;
        dyn_bson_w_init(&ks);
        dyn_bson_w_init(&ko);
        dyn_bson_w_doc_begin(&ks);
        dyn_bson_w_int32(&ks, "a", 1, fk_chance(r, 50) ? 1 : -1);
        dyn_bson_w_doc_end(&ks);
        dyn_bson_w_doc_begin(&ko);
        dyn_bson_w_doc_end(&ko);
        rc = dd_index_create(c, ks.p, ko.p, &ix, &e);
        dyn_bson_w_free(&ks);
        dyn_bson_w_free(&ko);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int write_snap(fk_rng_t* r, const char* path)
{
    static const char* const names[] = { "c0", "c1", "c2" };
    dd_db* db = dd_db_new("src");
    dd_err e;
    dyn_bson_w cat, meta[3];
    dd_snap_coll sc[3];
    unsigned nc = 1 + fk_below(r, 3), i;
    int rc = 0;
    memset(&e, 0, sizeof e);
    dyn_bson_w_init(&cat);
    dyn_bson_w_doc_begin(&cat);
    dyn_bson_w_sub_begin(&cat, DYN_BSON_ARRAY, "collections", 11);
    for (i = 0; i < nc; i++) {
        char idx[4];
        dd_coll* c = dd_db_create(db, names[i], 2, &e);
        dyn_bson_w_init(&meta[i]);
        if (!c || fill_coll(r, c) < 0 || dd_snap_coll_meta(c, &meta[i]) < 0)
            rc = -1;
        snprintf(idx, sizeof idx, "%u", i);
        dyn_bson_w_string(&cat, idx, strlen(idx), names[i], 2);
        sc[i].name = (char*)names[i];
        sc[i].name_n = 2;
        sc[i].docs = c ? (const uint8_t* const*)c->doc : NULL;
        sc[i].ndocs = c ? c->nslots : 0;
        sc[i].meta = meta[i].p;
    }
    dyn_bson_w_sub_end(&cat);
    dyn_bson_w_doc_end(&cat);
    if (rc == 0)
        rc = dd_snap_write(path, cat.p, sc, (int)nc, 0, NULL, &e);
    dyn_bson_w_free(&cat);
    for (i = 0; i < nc; i++)
        dyn_bson_w_free(&meta[i]);
    dd_db_free(db);
    return rc;
}

static void case_snap(void)
{
    unsigned it, iters = (unsigned)fk_iters(6);
    fk_rng_t r = fk_stream("snap");
    const char* path = ".obj/test_docdb_oom.snap";
    for (it = 0; it < iters; it++) {
        dd_db* owner = dd_db_new("dst");
        dd_snap_loaded out;
        dd_err e;
        long n, k;
        size_t base;
        if (write_snap(&r, path) < 0) {
            FK_CHECK(0, "snapshot write failed");
            dd_db_free(owner);
            return;
        }
        memset(&e, 0, sizeof e);
        base = live_now();
        arm(0);
        FK_CHECK(dd_snap_read(path, owner, &out, &e) == 0, "clean read failed: %d", e.code);
        n = disarm();
        dd_snap_loaded_free(&out);
        FK_CHECK(live_now() == base, "clean read leaked %zu blocks", live_now() - base);
        for (k = 1; k <= n; k += sweep_step(n, k)) {
            int rc;
            memset(&e, 0, sizeof e);
            arm(k);
            rc = dd_snap_read(path, owner, &out, &e);
            disarm();
            if (rc == 0)
                dd_snap_loaded_free(&out);
            else
                FK_CHECK(e.code == DD_E_OOM, "read fail_at %ld/%ld: rc %d code %d: %s", k, n, rc, e.code, e.msg);
            FK_CHECK(live_now() == base, "read fail_at %ld/%ld leaked %zu blocks", k, n, live_now() - base);
        }
        dd_db_free(owner);
    }
    remove(path);
}

int main(void)
{
    fk_init("test_docdb_oom");
    if (fk_only("facet"))
        case_facet();
    if (fk_only("snap"))
        case_snap();
    return fk_done("test_docdb_oom");
}
