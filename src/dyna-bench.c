/*
 * dyna:bench -- the performance persona's first import.
 *
 *   import { bench } from "dyna:bench";
 *   const r = bench("sum", () => sum(a), { timeMs: 200, warmupMs: 50 });
 *   // { name, iters, elapsedMs, opsPerSec, rsd, p50Ms, p99Ms }
 *   print(bench.table());
 *
 * Native timing via clock_gettime(CLOCK_MONOTONIC) around repeated JS_Call
 * invocations. Caller-owned: no allocation per iteration beyond what fn
 * itself does; latencies stored in a growable double array (one per timed
 * iteration, capped at 1M samples with reservoir decimation beyond).
 * Strict options: unknown keys throw. Timeless docs: no dates.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BENCH)

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int bench_opts_strict(JSContext *ctx, JSValueConst opts,
                             const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        int k;
        if (!name) { bad = 1; break; }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1;
            char *valid, *w;
            int l;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) { JS_FreeCString(ctx, name); bad = 1; break; }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = (int)strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
                memcpy(w, keys[k], (size_t)l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                              name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int bench_cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Recorded results for bench.table(). Bounded at 256 rows (oldest dropped). */
#define BENCH_MAX_ROWS 256
typedef struct {
    char name[128];
    double ops, rsd, p50, p99, elapsed;
    long iters;
} bench_row_t;
static bench_row_t bench_rows[BENCH_MAX_ROWS];
static int bench_nrows = 0;

static void bench_record(const char *name, double ops, double rsd,
                         double p50, double p99, double elapsed, long iters)
{
    bench_row_t *r;
    if (bench_nrows >= BENCH_MAX_ROWS) {
        memmove(bench_rows, bench_rows + 1,
                sizeof bench_rows[0] * (BENCH_MAX_ROWS - 1));
        bench_nrows = BENCH_MAX_ROWS - 1;
    }
    r = &bench_rows[bench_nrows++];
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    r->ops = ops; r->rsd = rsd; r->p50 = p50; r->p99 = p99;
    r->elapsed = elapsed; r->iters = iters;
}

static JSValue bench_run(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    char namebuf[128];
    JSValue fn;
    double timeMs = 500.0, warmupMs = 100.0;
    double *samp = NULL;
    size_t cap = 0, n = 0;
    double t0, deadline, elapsed;
    long iters = 0;
    double sum = 0.0, mean, sd, rsd, ops, p50, p99;
    static const char *const bench_keys[] = { "timeMs", "warmupMs", "warmup" };
    JSValue ret;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "bench(name, fn[, opts]): name and fn required");
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bench: name must be a string");
    fn = argv[1];
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "bench: fn must be a function");
    {
        size_t nl = 0;
        const char *s = JS_ToCStringLen(ctx, &nl, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        snprintf(namebuf, sizeof namebuf, "%s", s);
        JS_FreeCString(ctx, s);
        /* copy out: namebuf lives on the stack through record below */
    }
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        JSValue o = argv[2];
        JSValue v;
        double t;
        if (!JS_IsObject(o))
            return JS_ThrowTypeError(ctx, "bench: opts must be an object");
        if (bench_opts_strict(ctx, o, bench_keys, 3) < 0)
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, o, "timeMs");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToFloat64(ctx, &t, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (!(t >= 1.0 && t <= 60000.0)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "bench: timeMs must be 1..60000");
            }
            timeMs = t;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "warmupMs");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToFloat64(ctx, &t, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (!(t >= 0.0 && t <= 60000.0)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "bench: warmupMs must be 0..60000");
            }
            warmupMs = t;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "warmup");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToFloat64(ctx, &t, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (!(t >= 0.0 && t <= 60000.0)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "bench: warmup must be 0..60000");
            }
            warmupMs = t;
        }
        JS_FreeValue(ctx, v);
    }

    /* Warmup: run without sampling (JIT + caches settle). */
    if (warmupMs > 0) {
        double wend = bench_now_ms() + warmupMs;
        do {
            JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) {
                JSValue e = JS_GetException(ctx);
                JS_Throw(ctx, e);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, r);
        } while (bench_now_ms() < wend);
    }

    cap = 4096;
    samp = (double *)malloc(cap * sizeof *samp);
    if (!samp)
        return JS_ThrowOutOfMemory(ctx);
    t0 = bench_now_ms();
    deadline = t0 + timeMs;
    do {
        double s = bench_now_ms();
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        double e;
        if (JS_IsException(r)) {
            JSValue ex = JS_GetException(ctx);
            free(samp);
            JS_Throw(ctx, ex);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
        e = bench_now_ms();
        if (n >= cap) {
            if (cap >= 1048576) {
                /* Reservoir decimation past 1M: keep every other sample. */
                size_t j;
                for (j = 0; j + 1 < n; j += 2)
                    samp[j / 2] = samp[j];
                n /= 2;
            } else {
                size_t nc = cap * 2;
                double *ns = (double *)realloc(samp, nc * sizeof *ns);
                if (!ns) {
                    free(samp);
                    return JS_ThrowOutOfMemory(ctx);
                }
                samp = ns;
                cap = nc;
            }
        }
        samp[n++] = e - s;
        iters++;
    } while (bench_now_ms() < deadline);
    elapsed = bench_now_ms() - t0;
    if (!n) {
        free(samp);
        return JS_ThrowInternalError(ctx, "bench: no iterations ran");
    }
    for (size_t i = 0; i < n; i++)
        sum += samp[i];
    mean = sum / (double)n;
    {
        double v = 0.0;
        for (size_t i = 0; i < n; i++) {
            double d = samp[i] - mean;
            v += d * d;
        }
        sd = sqrt(v / (double)n);
    }
    rsd = mean > 0 ? sd / mean : 0.0;
    qsort(samp, n, sizeof *samp, bench_cmp_dbl);
    p50 = samp[n / 2];
    p99 = samp[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
    ops = mean > 0 ? 1000.0 / mean : 0.0;

    bench_record(namebuf, ops, rsd, p50, p99, elapsed, iters);

    ret = JS_NewObject(ctx);
    if (JS_IsException(ret)) { free(samp); return ret; }
    JS_SetPropertyStr(ctx, ret, "name", JS_NewString(ctx, namebuf));
    JS_SetPropertyStr(ctx, ret, "iters", JS_NewInt64(ctx, iters));
    JS_SetPropertyStr(ctx, ret, "elapsedMs", JS_NewFloat64(ctx, elapsed));
    JS_SetPropertyStr(ctx, ret, "opsPerSec", JS_NewFloat64(ctx, ops));
    JS_SetPropertyStr(ctx, ret, "rsd", JS_NewFloat64(ctx, rsd));
    JS_SetPropertyStr(ctx, ret, "p50Ms", JS_NewFloat64(ctx, p50));
    JS_SetPropertyStr(ctx, ret, "p99Ms", JS_NewFloat64(ctx, p99));
    /* Into-adjacent: per-op mean also exposed for buffer math. */
    JS_SetPropertyStr(ctx, ret, "meanMs", JS_NewFloat64(ctx, mean));
    free(samp);
    return ret;
}

static JSValue bench_table(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    const char *hdr = "name\titers\tops/sec\trsd\tp50ms\tp99ms\n";
    (void)this_val; (void)argc; (void)argv;
    {
        size_t hl = strlen(hdr);
        bcap = hl + (size_t)bench_nrows * 160 + 1;
        buf = (char *)malloc(bcap);
        if (!buf)
            return JS_ThrowOutOfMemory(ctx);
        memcpy(buf, hdr, hl);
        blen = hl;
    }
    for (int i = 0; i < bench_nrows; i++) {
        int w = snprintf(buf + blen, bcap - blen,
                         "%s\t%ld\t%.1f\t%.4f\t%.4f\t%.4f\n",
                         bench_rows[i].name, bench_rows[i].iters,
                         bench_rows[i].ops, bench_rows[i].rsd,
                         bench_rows[i].p50, bench_rows[i].p99);
        if (w < 0 || (size_t)w >= bcap - blen)
            break;
        blen += (size_t)w;
    }
    {
        JSValue s = JS_NewStringLen(ctx, buf, blen);
        free(buf);
        return s;
    }
}

static const JSCFunctionListEntry bench_funcs[] = {
    JS_CFUNC_DEF("bench", 2, bench_run),
    JS_CFUNC_DEF("table", 0, bench_table),
};

static int bench_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, bench_funcs, countof(bench_funcs));
}

int js_nat_init_bench(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:bench", bench_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, bench_funcs, countof(bench_funcs));
}

#endif
