/* Benchmark of the redesigned dyn_parr (src/core/ds/ds-parr.c) against its baselines:
 *   vec   a contiguous realloc array (x2 growth) of 16-byte records + a u32 generation array +
 *         a LIFO free-slot stack: the fair contiguous competitor (same handle check, hole reuse)
 *   v1    the frozen v1 dyn_parr (chunk directory, u32 handle chunk<<6|slot, no generations;
 *         partitions are capped at 64 there, so P=128/256 rows run v1 at 64)
 *   flat  new dyn_parr, flat mode: one VM reservation per column, element = base + i*16
 *         (typed accessor DYN_PARR_FLAT_REF)
 *   flatg new dyn_parr, flat mode through the mode-agnostic DYN_PARR_REF (flat/table branch per
 *         access) and the plain ctz DYN_PARR_FOREACH (flat/table use the hybrid DYN_PARR_EACH)
 *   table new dyn_parr, table mode: per-partition pre-biased base-pointer table (one extra load)
 *   flatl new dyn_parr, flat mode with reuse = LIFO (free-slot stack + high-water mark instead of
 *         lowest-free via the partition summaries); run on the mutation ops only
 * Every timed interval is CPU time (CLOCK_THREAD_CPUTIME_ID) over >= ~100us of work: small N is
 * batched over K independent instances so the timer (~250ns, ~200ns step) never dominates.
 * Variants are interleaved within each repetition (rotating order), >= 7 repetitions; the report
 * gives the median ns/op, the IQR as a percentage of the median, and a verdict per variant against
 * vec and v1: "tie" when the medians differ by less than max(3%, the two IQRs combined).
 * The worst-case section times every single append with the monotonic clock (wall time, the only
 * clock fine enough for one operation) and reports p50/p99/p99.9/max. Memory is the structure's
 * own accounting plus the RSS (phys_footprint) delta measured in a forked child per variant.
 * Usage: bench_ds_parr [--quick] [--reps=N] [--n=128,1K,1M] [--p=32,64] [--ops=churn,append]
 *        [--vars=flat,table] [--sections=1|2|4 (speed|latency|memory, OR-ed)] [--tsv=path] */
#include "../../src/core/dyn-snappend.h"
#include "../lib/dyn-xalloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#define DYN_DS_MALLOC(n) malloc(n)
#define DYN_DS_CALLOC(n, s) calloc(n, s)
#define DYN_DS_REALLOC(p, n) realloc(p, n)
#define DYN_DS_FREE(p) free(p)
#define V1_DS_MALLOC(n) malloc(n)
#define V1_DS_REALLOC(p, n) realloc(p, n)
#define V1_DS_FREE(p) free(p)
#include "ds/ds-parr.c"
#include "bench_parr_v1.h"

typedef struct {
    uint64_t key;
    uint64_t val;
} rec_t;

static volatile uint64_t sink;
static uint32_t* g_perm;
static uint32_t* g_idx;
static uint32_t* g_chain;
static uint64_t* g_h;
static uint32_t g_m = 1u << 20;

static uint64_t cpu_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t wall_ns(void)
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static int64_t rss_now(void)
{
#if defined(__APPLE__)
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) != KERN_SUCCESS)
        return -1;
    return (int64_t)vi.phys_footprint;
#else
    long a = 0, b = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f || fscanf(f, "%ld %ld", &a, &b) != 2)
        b = -1;
    if (f)
        fclose(f);
    return b < 0 ? -1 : (int64_t)b * sysconf(_SC_PAGESIZE);
#endif
}

static int64_t rss_peak(void)
{
#if defined(__APPLE__)
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) != KERN_SUCCESS)
        return -1;
    return (int64_t)vi.ledger_phys_footprint_peak;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (int64_t)ru.ru_maxrss * 1024;
#endif
}

static uint64_t rng_s = 0x1234567;
static uint64_t rnd(void)
{
    rng_s += 0x9e3779b97f4a7c15ULL;
    uint64_t z = rng_s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct {
    rec_t* a;
    uint32_t* gen;
    uint32_t* fs;
    uint32_t n, cap, nf, fcap, live;
} vec_t;

static void vec_init(vec_t* v, uint32_t P)
{
    (void)P;
    memset(v, 0, sizeof(*v));
}
static void vec_destroy(vec_t* v)
{
    free(v->a);
    free(v->gen);
    free(v->fs);
}
static __attribute__((noinline)) void vec_grow(vec_t* v)
{
    uint32_t nc = v->cap ? v->cap * 2 : 16;
    v->a = xrealloc(v->a, (size_t)nc * sizeof(rec_t));
    v->gen = xrealloc(v->gen, (size_t)nc * sizeof(uint32_t));
    memset(v->gen + v->cap, 0, (size_t)(nc - v->cap) * sizeof(uint32_t));
    v->cap = nc;
}
static inline uint32_t vec_alloc(vec_t* v)
{
    uint32_t s;
    if (v->nf)
        s = v->fs[--v->nf];
    else {
        if (v->n == v->cap)
            vec_grow(v);
        s = v->n++;
    }
    v->gen[s]++;
    v->live++;
    return s;
}
static inline rec_t* vec_ref(vec_t* v, uint32_t s) { return v->a + s; }
static inline uint64_t vec_handle(vec_t* v, uint32_t s) { return ((uint64_t)v->gen[s] << 32) | s; }
static inline rec_t* vec_resolve(vec_t* v, uint64_t h)
{
    uint32_t s = (uint32_t)h, g = (uint32_t)(h >> 32);
    if (s >= v->n || !(g & 1) || v->gen[s] != g)
        return NULL;
    return v->a + s;
}
static inline int vec_free(vec_t* v, uint64_t h)
{
    uint32_t s = (uint32_t)h;
    if (!vec_resolve(v, h))
        return -1;
    v->gen[s]++;
    if (v->nf == v->fcap) {
        v->fcap = v->fcap ? v->fcap * 2 : 16;
        v->fs = xrealloc(v->fs, (size_t)v->fcap * sizeof(uint32_t));
    }
    v->fs[v->nf++] = s;
    v->live--;
    return 0;
}
static inline uint64_t vec_scan(vec_t* v)
{
    uint64_t sum = 0;
    for (uint32_t s = 0; s < v->n; s++)
        if (v->gen[s] & 1)
            sum += v->a[s].val;
    return sum;
}
static size_t vec_bytes(vec_t* v) { return sizeof(*v) + (size_t)v->cap * (sizeof(rec_t) + 4) + (size_t)v->fcap * 4; }
static void vec_shrink(vec_t* v) { (void)v; }

typedef struct {
    v1parr_t p;
} v1_t;

static void v1_init(v1_t* v, uint32_t P)
{
    uint32_t esz[1] = { sizeof(rec_t) };
    unsigned sh = (unsigned)__builtin_ctz(P);
    if (v1parr_init(&v->p, sh > 6 ? 6 : sh, 1, esz) < 0)
        exit(2);
}
static void v1_destroy(v1_t* v) { v1parr_destroy(&v->p); }
static inline uint32_t v1_alloc(v1_t* v) { return v1parr_alloc(&v->p); }
static inline rec_t* v1_ref(v1_t* v, uint32_t s) { return (rec_t*)v1parr_at(&v->p, 0, s); }
static inline uint64_t v1_handle(v1_t* v, uint32_t s)
{
    (void)v;
    return s;
}
static inline rec_t* v1_resolve(v1_t* v, uint64_t h)
{
    uint32_t s = (uint32_t)h;
    return v1parr_live(&v->p, s) ? v1_ref(v, s) : NULL;
}
static inline int v1_free(v1_t* v, uint64_t h)
{
    if (!v1parr_live(&v->p, (uint32_t)h))
        return -1;
    v1parr_release(&v->p, (uint32_t)h);
    return 0;
}
static inline uint64_t v1_scan(v1_t* v)
{
    uint64_t sum = 0;
    for (uint32_t c = 0; c < v->p.nchunk; c++) {
        const rec_t* b = (const rec_t*)v->p.dir[0][c];
        for (uint64_t bits = v->p.occ[c]; bits; bits &= bits - 1)
            sum += b[__builtin_ctzll(bits)].val;
    }
    return sum;
}
static size_t v1_bytes(v1_t* v)
{
    size_t w = (v->p.dircap + 63) / 64;
    return sizeof(*v) + (size_t)v->p.nchunk * (sizeof(rec_t) << v->p.shift) + (size_t)v->p.dircap * 16 + w * 8 + (w + 63) / 64 * 8;
}
static void v1_shrink(v1_t* v) { (void)v; }

typedef struct {
    dyn_parr_t p;
} fl_t;
typedef fl_t flg_t;
typedef fl_t tb_t;

static void parr_open(dyn_parr_t* p, uint32_t P, uint8_t mode, uint8_t reuse)
{
    dyn_parr_opts_t o;
    uint32_t esz[1] = { sizeof(rec_t) };
    dyn_parr_opts_default(&o);
    o.partition = P;
    o.mode = mode;
    o.reuse = reuse;
    if (dyn_parr_init(p, &o, 1, esz) < 0)
        exit(2);
}

static void fl_init(fl_t* v, uint32_t P) { parr_open(&v->p, P, DYN_PARR_FLAT, DYN_PARR_REUSE_LOWEST); }
static void fl_destroy(fl_t* v) { dyn_parr_destroy(&v->p); }
static inline uint32_t fl_alloc(fl_t* v) { return dyn_parr_alloc(&v->p); }
static inline rec_t* fl_ref(fl_t* v, uint32_t s) { return DYN_PARR_FLAT_REF(&v->p, 0, s, rec_t); }
static inline uint64_t fl_handle(fl_t* v, uint32_t s) { return dyn_parr_handle(&v->p, s); }
static inline rec_t* fl_resolve(fl_t* v, uint64_t h)
{
    uint32_t s = dyn_parr_resolve(&v->p, h);
    return s == DYN_PARR_NIL ? NULL : DYN_PARR_FLAT_REF(&v->p, 0, s, rec_t);
}
static inline int fl_free(fl_t* v, uint64_t h) { return dyn_parr_free(&v->p, h); }
static inline uint64_t fl_scan(fl_t* v)
{
    uint64_t sum = 0;
    const rec_t* b = (const rec_t*)v->p.base[0];
    DYN_PARR_EACH(&v->p, s, sum += b[s].val;);
    return sum;
}
static size_t fl_bytes(fl_t* v) { return dyn_parr_bytes(&v->p); }
static void fl_shrink(fl_t* v) { (void)dyn_parr_compact(&v->p); }

#define flg_init fl_init
#define flg_destroy fl_destroy
#define flg_alloc fl_alloc
#define flg_handle fl_handle
#define flg_free fl_free
#define flg_bytes fl_bytes
#define flg_shrink fl_shrink
static inline rec_t* flg_ref(flg_t* v, uint32_t s) { return DYN_PARR_REF(&v->p, 0, s, rec_t); }
static inline rec_t* flg_resolve(flg_t* v, uint64_t h)
{
    uint32_t s = dyn_parr_resolve(&v->p, h);
    return s == DYN_PARR_NIL ? NULL : DYN_PARR_REF(&v->p, 0, s, rec_t);
}
static inline uint64_t flg_scan(flg_t* v)
{
    uint64_t sum = 0;
    DYN_PARR_FOREACH(&v->p, s)
    sum += DYN_PARR_REF(&v->p, 0, s, rec_t)->val;
    return sum;
}

static void tb_init(tb_t* v, uint32_t P) { parr_open(&v->p, P, DYN_PARR_TABLE, DYN_PARR_REUSE_LOWEST); }

typedef fl_t fll_t;
static void fll_init(fll_t* v, uint32_t P) { parr_open(&v->p, P, DYN_PARR_FLAT, DYN_PARR_REUSE_LIFO); }
#define fll_destroy fl_destroy
#define fll_alloc fl_alloc
#define fll_ref fl_ref
#define fll_handle fl_handle
#define fll_resolve fl_resolve
#define fll_free fl_free
#define fll_scan fl_scan
#define fll_bytes fl_bytes
#define fll_shrink fl_shrink
#define tb_destroy fl_destroy
#define tb_alloc fl_alloc
#define tb_handle fl_handle
#define tb_free fl_free
#define tb_bytes fl_bytes
#define tb_shrink fl_shrink
static inline rec_t* tb_ref(tb_t* v, uint32_t s) { return DYN_PARR_TABLE_REF(&v->p, 0, s, rec_t); }
static inline rec_t* tb_resolve(tb_t* v, uint64_t h)
{
    uint32_t s = dyn_parr_resolve(&v->p, h);
    return s == DYN_PARR_NIL ? NULL : DYN_PARR_TABLE_REF(&v->p, 0, s, rec_t);
}
static inline uint64_t tb_scan(tb_t* v)
{
    uint64_t sum = 0;
    DYN_PARR_EACH(&v->p, s, sum += DYN_PARR_TABLE_REF(&v->p, 0, s, rec_t)->val;);
    return sum;
}

#define VAR vec
#define VT vec_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT
#define VAR v1
#define VT v1_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT
#define VAR fl
#define VT fl_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT
#define VAR flg
#define VT flg_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT
#define VAR tb
#define VT tb_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT
#define VAR fll
#define VT fll_t
#include "bench_parr_ops.h"
#undef VAR
#undef VT

enum { V_VEC,
    V_V1,
    V_FLAT,
    V_FLATG,
    V_TABLE,
    V_FLATL,
    NV };
static const char* vname[NV] = { "vec", "v1", "flat", "flatg", "table", "flatl" };

enum { O_APPEND,
    O_FILL,
    O_RANDIDX,
    O_CHASE,
    O_RANDH,
    O_SCAN0,
    O_SCAN10,
    O_SCAN50,
    O_DELETE,
    O_CHURN,
    NO };
static const char* oname[NO] = { "append", "fill-holes", "rand-index", "chase-index", "rand-handle", "scan-dense", "scan-10%holes", "scan-50%holes", "delete-random", "churn" };
static const int access_op[NO] = { 0, 0, 1, 1, 1, 1, 1, 1, 0, 0 };

static double run_op(int v, int op, uint32_t N, uint32_t P)
{
    uint32_t K = N >= (1u << 17) ? 1 : (1u << 17) / N;
#define DISPATCH(X)                    \
    switch (op) {                      \
    case O_APPEND:                     \
        return X##_op_append(N, P, K); \
    case O_FILL:                       \
        return X##_op_fill(N, P, K);   \
    case O_RANDIDX:                    \
        return X##_op_randidx(N, P);   \
    case O_CHASE:                      \
        return X##_op_chase(N, P);     \
    case O_RANDH:                      \
        return X##_op_randh(N, P);     \
    case O_SCAN0:                      \
        return X##_op_scan(N, P, 0);   \
    case O_SCAN10:                     \
        return X##_op_scan(N, P, 10);  \
    case O_SCAN50:                     \
        return X##_op_scan(N, P, 50);  \
    case O_DELETE:                     \
        return X##_op_delete(N, P, K); \
    case O_CHURN:                      \
        return X##_op_churn(N, P);     \
    }                                  \
    return -1;
    switch (v) {
    case V_VEC:
        DISPATCH(vec)
    case V_V1:
        DISPATCH(v1)
    case V_FLAT:
        DISPATCH(fl)
    case V_FLATG:
        DISPATCH(flg)
    case V_TABLE:
        DISPATCH(tb)
    case V_FLATL:
        DISPATCH(fll)
    }
    return -1;
}

static int cmpd(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static int cmpu(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    double med, iqr;
} stat_t;

static stat_t stats(double* x, int n)
{
    stat_t s;
    qsort(x, (size_t)n, sizeof(double), cmpd);
    s.med = n % 2 ? x[n / 2] : (x[n / 2 - 1] + x[n / 2]) / 2;
    double q1 = x[n / 4], q3 = x[(3 * n) / 4 < n ? (3 * n) / 4 : n - 1];
    s.iqr = s.med > 0 ? (q3 - q1) / s.med : 0;
    return s;
}

static const char* verdict(stat_t x, stat_t base, double* ratio)
{
    double thr = x.iqr + base.iqr;
    if (thr < 0.03)
        thr = 0.03;
    *ratio = base.med > 0 ? x.med / base.med : 0;
    if (*ratio < 1 - thr)
        return "faster";
    if (*ratio > 1 + thr)
        return "slower";
    return "tie";
}

static void setup_n(uint32_t N)
{
    free(g_perm);
    free(g_idx);
    free(g_chain);
    free(g_h);
    g_perm = xcalloc(N, sizeof(uint32_t));
    g_chain = xcalloc(N, sizeof(uint32_t));
    g_h = xcalloc(N, sizeof(uint64_t));
    g_idx = xcalloc(g_m, sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++)
        g_perm[i] = i;
    for (uint32_t i = N - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(rnd() % (i + 1)), t = g_perm[i];
        g_perm[i] = g_perm[j];
        g_perm[j] = t;
    }
    uint32_t* cyc = xcalloc(N, sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++)
        cyc[i] = i;
    for (uint32_t i = N - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(rnd() % i), t = cyc[i];
        cyc[i] = cyc[j];
        cyc[j] = t;
    }
    for (uint32_t i = 0; i < N; i++)
        g_chain[cyc[i]] = cyc[(i + 1) % N];
    free(cyc);
    for (uint32_t j = 0; j < g_m; j++)
        g_idx[j] = (uint32_t)(rnd() % N);
}

static void lat_run(int v, uint32_t N, uint32_t P, uint64_t* lat)
{
    switch (v) {
    case V_VEC:
        vec_op_latency(N, P, lat);
        break;
    case V_V1:
        v1_op_latency(N, P, lat);
        break;
    case V_FLAT:
    case V_FLATG:
        fl_op_latency(N, P, lat);
        break;
    case V_TABLE:
        tb_op_latency(N, P, lat);
        break;
    case V_FLATL:
        fll_op_latency(N, P, lat);
        break;
    }
}

static size_t bytes_run(int v, uint32_t N, uint32_t P, size_t* half)
{
    switch (v) {
    case V_VEC:
        return vec_op_bytes(N, P, half);
    case V_V1:
        return v1_op_bytes(N, P, half);
    case V_TABLE:
        return tb_op_bytes(N, P, half);
    default:
        return fl_op_bytes(N, P, half);
    }
}

static void rss_run(int v, uint32_t N, uint32_t P, int64_t out[3])
{
    int fd[2];
    pid_t pid;
    out[0] = out[1] = out[2] = -1;
    if (pipe(fd) < 0)
        return;
    pid = fork();
    if (pid == 0) {
        int64_t r[3];
        close(fd[0]);
        memset(g_h, 0, (size_t)N * sizeof(uint64_t));
        switch (v) {
        case V_VEC:
            vec_op_rss(N, P, r);
            break;
        case V_V1:
            v1_op_rss(N, P, r);
            break;
        case V_TABLE:
            tb_op_rss(N, P, r);
            break;
        default:
            fl_op_rss(N, P, r);
            break;
        }
        if (write(fd[1], r, sizeof(r)) != (ssize_t)sizeof(r))
            _exit(1);
        _exit(0);
    }
    close(fd[1]);
    if (pid > 0) {
        if (read(fd[0], out, sizeof(int64_t) * 3) != (ssize_t)(sizeof(int64_t) * 3))
            out[0] = out[1] = out[2] = -1;
        waitpid(pid, NULL, 0);
    }
    close(fd[0]);
}

static int parse_list(const char* s, uint32_t* out, int max)
{
    int n = 0;
    while (*s && n < max) {
        char* e;
        unsigned long v = strtoul(s, &e, 10);
        if (e == s)
            break;
        if (*e == 'K' || *e == 'k') {
            v *= 1000;
            e++;
        } else if (*e == 'M' || *e == 'm') {
            v *= 1000000;
            e++;
        }
        out[n++] = (uint32_t)v;
        s = *e == ',' ? e + 1 : e;
    }
    return n;
}

int main(int argc, char** argv)
{
    uint32_t Ns[8] = { 128, 1000, 10000, 100000, 1000000 }, Ps[4] = { 32, 64, 128, 256 };
    int nN = 5, nP = 4, reps = 7;
    const char* tsv = NULL;
    FILE* tf = NULL;
    int op_on[NO], var_on[NV], sections = 7;
    for (int i = 0; i < NO; i++)
        op_on[i] = 1;
    for (int i = 0; i < NV; i++)
        var_on[i] = 1;
    for (int i = 1; i < argc; i++) {
        if (!argv[i][0])
            continue;
        if (!strcmp(argv[i], "--quick")) {
            nN = parse_list("128,10000,1000000", Ns, 8);
            nP = parse_list("64", Ps, 4);
            reps = 7;
            g_m = 1u << 19;
        } else if (!strncmp(argv[i], "--reps=", 7))
            reps = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--n=", 4))
            nN = parse_list(argv[i] + 4, Ns, 8);
        else if (!strncmp(argv[i], "--p=", 4))
            nP = parse_list(argv[i] + 4, Ps, 4);
        else if (!strncmp(argv[i], "--tsv=", 6))
            tsv = argv[i] + 6;
        else if (!strncmp(argv[i], "--ops=", 6)) {
            for (int k = 0; k < NO; k++)
                op_on[k] = strstr(argv[i] + 6, oname[k]) != NULL;
        } else if (!strncmp(argv[i], "--vars=", 7)) {
            for (int k = 0; k < NV; k++) {
                const char* q = strstr(argv[i] + 7, vname[k]);
                var_on[k] = q && (q[strlen(vname[k])] == ',' || q[strlen(vname[k])] == 0);
            }
            var_on[V_VEC] = var_on[V_V1] = 1;
        } else if (!strncmp(argv[i], "--sections=", 11))
            sections = atoi(argv[i] + 11);
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (reps < 7)
        reps = 7;
    if (tsv && !(tf = fopen(tsv, "w"))) {
        fprintf(stderr, "cannot write %s\n", tsv);
        return 2;
    }
    if (tf)
        fprintf(tf, "section\top\tN\tP\tvariant\tmedian\tiqr\tratio_vs_vec\tverdict_vs_vec\tratio_vs_v1\tverdict_vs_v1\traw\n");
    printf("dyn_parr benchmark: %d reps, %u random accesses per access op, CPU time, ns/op (median, IQR%% of median)\n", reps, g_m);
    printf("v1 caps partitions at 64 (its rows for P=128/256 run at 64)\n\n");
    double* samp = xcalloc((size_t)reps * NV * NO, sizeof(double));
    int total_verdicts[3][2][3] = { { { 0 } } };
    for (int ni = 0; ni < nN && (sections & 1); ni++) {
        uint32_t N = Ns[ni];
        setup_n(N);
        for (int pi = 0; pi < nP; pi++) {
            uint32_t P = Ps[pi];
            for (int r = 0; r < reps; r++)
                for (int op = 0; op < NO; op++)
                    for (int k = 0; k < NV; k++) {
                        int v = (k + r + op) % NV;
                        if (((v == V_FLATG && !access_op[op]) || (v == V_FLATL && access_op[op])) || !op_on[op] || !var_on[v])
                            continue;
                        samp[((size_t)op * NV + v) * reps + r] = run_op(v, op, N, P);
                    }
            printf("N=%u P=%u\n", N, P);
            printf("  %-14s", "op");
            for (int v = 0; v < NV; v++)
                printf(" %15s", vname[v]);
            printf("   | vs vec, vs v1: flat / table / flatl\n");
            for (int op = 0; op < NO; op++) {
                stat_t st[NV];
                if (!op_on[op])
                    continue;
                char raw[NV][256];
                for (int v = 0; v < NV; v++) {
                    double* x = samp + ((size_t)op * NV + v) * reps;
                    int o = 0;
                    raw[v][0] = 0;
                    if (((v == V_FLATG && !access_op[op]) || (v == V_FLATL && access_op[op])) || !var_on[v]) {
                        st[v].med = -1;
                        st[v].iqr = 0;
                        continue;
                    }
                    for (int r = 0; r < reps && o < 240; r++)
                        o = (int)dyn_snappend(raw[v], sizeof(raw[v]), (size_t)o, "%s%.3f", r ? "," : "", x[r]);
                    st[v] = stats(x, reps);
                }
                printf("  %-14s", oname[op]);
                for (int v = 0; v < NV; v++) {
                    if (st[v].med < 0)
                        printf(" %15s", "-");
                    else
                        printf(" %8.2f (%3.0f%%)", st[v].med, st[v].iqr * 100);
                }
                printf("   |");
                static const int cand[3] = { V_FLAT, V_TABLE, V_FLATL };
                for (int ci = 0; ci < 3; ci++) {
                    int v = cand[ci];
                    if (st[v].med < 0) {
                        printf(" %-29s|", " -");
                        continue;
                    }
                    double rv, r1;
                    const char* a = verdict(st[v], st[V_VEC], &rv);
                    const char* b = verdict(st[v], st[V_V1], &r1);
                    printf(" %5.2fx %-6s %5.2fx %-6s |", rv, a, r1, b);
                    total_verdicts[ci][0][a[0] == 'f' ? 0 : a[0] == 't' ? 1
                                                                        : 2]++;
                    total_verdicts[ci][1][b[0] == 'f' ? 0 : b[0] == 't' ? 1
                                                                        : 2]++;
                }
                printf("\n");
                if (tf)
                    for (int v = 0; v < NV; v++) {
                        if (st[v].med < 0)
                            continue;
                        double x1, x2;
                        const char* c1 = verdict(st[v], st[V_VEC], &x1);
                        const char* c2 = verdict(st[v], st[V_V1], &x2);
                        fprintf(tf, "speed\t%s\t%u\t%u\t%s\t%.4f\t%.4f\t%.4f\t%s\t%.4f\t%s\t%s\n", oname[op], N, P, vname[v], st[v].med, st[v].iqr, x1, c1, x2, c2, raw[v]);
                    }
            }
            printf("\n");
            fflush(stdout);
        }
    }
    {
        static const char* cn[3] = { "flat", "table", "flatl" };
        printf("verdict totals (faster/tie/slower):");
        for (int ci = 0; ci < 3; ci++)
            printf("  %s vs vec %d/%d/%d, vs v1 %d/%d/%d;", cn[ci], total_verdicts[ci][0][0], total_verdicts[ci][0][1], total_verdicts[ci][0][2],
                total_verdicts[ci][1][0], total_verdicts[ci][1][1], total_verdicts[ci][1][2]);
        printf("\n\n");
    }

    printf("worst-case single append latency (wall ns per op incl. ~15ns timer; median over %d reps of each percentile; max = max over reps)\n", reps);
    printf("  %-8s %-4s %-6s %8s %8s %9s %10s %10s\n", "N", "P", "var", "p50", "p99", "p99.9", "max(med)", "max(all)");
    for (int ni = 0; ni < nN && (sections & 2); ni++) {
        uint32_t N = Ns[ni];
        uint64_t* lat = xcalloc(N, sizeof(uint64_t));
        for (int pi = 0; pi < nP; pi++) {
            uint32_t P = Ps[pi];
            double pc[NV][4][32];
            uint64_t mx[NV] = { 0 };
            int R = reps > 32 ? 32 : reps;
            for (int r = 0; r < R; r++)
                for (int k = 0; k < NV; k++) {
                    int v = (k + r) % NV;
                    if (v == V_FLATG)
                        continue;
                    lat_run(v, N, P, lat);
                    qsort(lat, N, sizeof(uint64_t), cmpu);
                    pc[v][0][r] = (double)lat[N / 2];
                    pc[v][1][r] = (double)lat[(size_t)((double)N * 0.99)];
                    pc[v][2][r] = (double)lat[(size_t)((double)N * 0.999)];
                    pc[v][3][r] = (double)lat[N - 1];
                    if (lat[N - 1] > mx[v])
                        mx[v] = lat[N - 1];
                }
            for (int v = 0; v < NV; v++) {
                if (v == V_FLATG)
                    continue;
                stat_t s[4];
                for (int q = 0; q < 4; q++)
                    s[q] = stats(pc[v][q], R);
                printf("  %-8u %-4u %-6s %8.0f %8.0f %9.0f %10.0f %10llu\n", N, P, vname[v], s[0].med, s[1].med, s[2].med, s[3].med, (unsigned long long)mx[v]);
                if (tf)
                    fprintf(tf, "latency\tappend\t%u\t%u\t%s\t%.0f\t%.0f\t%.0f\t%.0f\t%llu\t\t\n", N, P, vname[v], s[0].med, s[1].med, s[2].med, s[3].med, (unsigned long long)mx[v]);
            }
        }
        free(lat);
    }
    printf("\nmemory: accounted bytes per element after a dense build of N, and after deleting half\n");
    printf("(+ dyn_parr_compact for flat/table; vec and v1 cannot return memory); RSS deltas from a forked child\n");
    printf("  %-8s %-4s %-6s %10s %10s %14s %14s %14s\n", "N", "P", "var", "B/elem", "B/elem½", "rss full", "rss half", "rss peak");
    for (int ni = 0; ni < nN && (sections & 4); ni++) {
        uint32_t N = Ns[ni];
        setup_n(N);
        for (int pi = 0; pi < nP; pi++) {
            uint32_t P = Ps[pi];
            for (int v = 0; v < NV; v++) {
                if (v == V_FLATG || v == V_FLATL)
                    continue;
                size_t half, full = bytes_run(v, N, P, &half);
                int64_t rs[3];
                rss_run(v, N, P, rs);
                printf("  %-8u %-4u %-6s %10.2f %10.2f %14lld %14lld %14lld\n", N, P, vname[v], (double)full / N, (double)half / N, (long long)rs[0], (long long)rs[1], (long long)rs[2]);
                if (tf)
                    fprintf(tf, "memory\tbytes\t%u\t%u\t%s\t%zu\t%zu\t%lld\t%lld\t%lld\t\t\n", N, P, vname[v], full, half, (long long)rs[0], (long long)rs[1], (long long)rs[2]);
            }
        }
    }
    if (tf)
        fclose(tf);
    free(samp);
    return 0;
}
