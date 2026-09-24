/* latency_harness.c -- D3 interrupt-latency harness (embedder metric).
 *
 * Measures how long after an embedder requests an interrupt DynaJS actually
 * stops executing JS. The engine polls for interrupts at:
 *   - every JS function entry           (JS_CallInternal entry)
 *   - every 10000 backward jumps        (OP_goto/goto8/goto16 back-edges)
 *   - conditional-branch back-edges     (OP_if_*8 loop forms)
 *   - every 10000 regexp VM steps       (lre_poll_timeout -> lre_check_timeout)
 *   - cyclic-prototype guards, GC sweep, module deserialization
 * and never inside pure-C builtins (JSON.parse/stringify, String ops, ...).
 *
 * Mechanics: a pthread arms a random-delay timer, then sets an atomic flag.
 * The JS interrupt handler (invoked by the engine at every poll) timestamps
 * the moment it observes the flag; latency = observed - set. 1000 interrupts
 * per loop class. Also reports eval-return time (latency + unwind) and how
 * many runs completed without ever noticing the flag.
 *
 * Build (from tree root):  see run_latency.sh
 * Output: one STATS line per class + raw samples CSV (DYNA_LAT_CSV=1).
 *
 * This harness embeds the engine exactly like examples do (libdynajs.a);
 * it makes NO engine changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#include "dyna-libc.h"

/* ---------------- public engine surface used (dynajs.h) ------------------
 * JSRuntime/JSContext, JS_NewRuntime, JS_NewContext, JS_Eval,
 * JS_SetInterruptHandler, JS_FreeContext, JS_FreeRuntime, JS_AddIntrinsic*.
 * (Same embed pattern as the generated examples; dynajs.h pulls in the
 * intrinsic declarations.) */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

typedef struct {
    atomic_int flag;        /* 1 = interrupt requested */
    atomic_int armed;       /* 1 = JS is running, thread may fire      */
    atomic_int stop;        /* 1 = shut the thread down                */
    _Atomic double set_ns;    /* when the flag was set                */
    _Atomic double served_ns; /* when the handler observed the flag   */
    double next_delay_ns;   /* armed by main before each sample        */
} isig_t;

static int handler(JSRuntime *rt, void *opaque) {
    isig_t *s = opaque;
    if (atomic_load_explicit(&s->flag, memory_order_relaxed)) {
        atomic_store_explicit(&s->served_ns, now_ns(), memory_order_relaxed);
        return 1; /* stop: throw uncatchable "interrupted" */
    }
    return 0;
}

static void *thread_fn(void *arg) {
    isig_t *s = arg;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        if (atomic_load_explicit(&s->armed, memory_order_relaxed)) {
            double d = s->next_delay_ns;
            double whole;
            double frac = modf(d / 1e9, &whole);
            struct timespec ts = { (time_t)whole, (long)(frac * 1e9) };
            nanosleep(&ts, NULL);
            if (atomic_load_explicit(&s->armed, memory_order_relaxed) &&
                !atomic_load_explicit(&s->flag, memory_order_relaxed)) {
                atomic_store_explicit(&s->set_ns, now_ns(), memory_order_relaxed);
                atomic_store_explicit(&s->flag, 1, memory_order_relaxed);
            }
        }
        struct timespec idle = { 0, 200000 }; /* 200us between arm checks */
        nanosleep(&idle, NULL);
    }
    return NULL;
}

/* deterministic PRNG so runs are reproducible */
static uint64_t rng = 0x243F6A8885A308D3ull;
static double rnd01(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct { double min, p10, p50, p90, p99, max, mean; int n, served, completed; } stats_t;

static void summarize(double *v, int n, int served, int completed, stats_t *st) {
    st->n = n; st->served = served; st->completed = completed;
    memset(st, 0, sizeof(*st));
    st->n = n; st->served = served; st->completed = completed;
    if (n == 0) return;
    qsort(v, n, sizeof(double), cmp_d);
    st->min = v[0];
    st->p10 = v[(int)(0.10 * (n - 1))];
    st->p50 = v[(int)(0.50 * (n - 1))];
    st->p90 = v[(int)(0.90 * (n - 1))];
    st->p99 = v[(int)(0.99 * (n - 1))];
    st->max = v[n - 1];
    double sum = 0; for (int i = 0; i < n; i++) sum += v[i];
    st->mean = sum / n;
}

/* ---------------------------- loop classes ------------------------------ */

typedef struct { const char *name; const char *src; const char *warm; int giant; } klass_t;

/* `warm` builds the same shapes with tiny iteration counts -- a warmup that
 * runs the real source would execute the whole (billions of iterations) loop
 * to completion, because warmup has no interrupt armed. */
static klass_t classes[] = {
    /* tight arithmetic: poll quantum = 10000 goto8 back-edges */
    { "tight_arith",
      "let s = 0; for (let i = 0; i < 2000000000; i++) { s = (s + i) | 0; } s",
      "let s = 0; for (let i = 0; i < 100000; i++) { s = (s + i) | 0; } s", 0 },
    /* property loop: back-edge + inline-cached gets */
    { "prop_loop",
      "const o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 };"
      " let s = 0; for (let i = 0; i < 2000000000; i++) { s += o.b + o.e; } s",
      "const o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 };"
      " let s = 0; for (let i = 0; i < 100000; i++) { s += o.b + o.e; } s", 0 },
    /* call loop: poll on EVERY call (function entry) + back-edge */
    { "call_loop",
      "function f(x) { return x + 1; } let s = 0;"
      " for (let i = 0; i < 2000000000; i++) { s = f(s); } s",
      "function f(x) { return x + 1; } let s = 0;"
      " for (let i = 0; i < 100000; i++) { s = f(s); } s", 0 },
    /* regexp loop: many tiny execs (JS back-edge poll + lre poll interleave) */
    { "regexp_loop",
      "const re = /(ab)+c?d*/; const subj = 'abababXab ababc 12345'; let s = 0;"
      " for (let i = 0; i < 100000000; i++) { if (re.exec(subj)) s++; } s",
      "const re = /(ab)+c?d*/; const subj = 'abababXab ababc 12345'; let s = 0;"
      " for (let i = 0; i < 50000; i++) { if (re.exec(subj)) s++; } s", 0 },
    /* ONE catastrophic exec: polls only via lre_poll_timeout's own
     * 10000-step counter; the JS interpreter is parked for the duration */
    { "regexp_catastrophic",
      "const re = /(a+)+b/; const r = re.exec('" "aaaaaaaaaaaaaaaaaaaaaaaaaax" "'); r ? r[0] : 'nomatch'",
      "const re = /(a+)+b/; const r = re.exec('aaax'); r ? r[0] : 'nomatch'", 0 },
    /* pure-C builtin, single op, NO poll sites: latency scales with input */
    { "json_parse_4mb", "", "", 1 },   /* src built at runtime (size parametrized) */
    { "string_repeat_32mb", "", "", 1 },
    { "array_fill_sort_1m", "", "", 1 },
};

#define N_CLASSES (int)(sizeof(classes) / sizeof(classes[0]))
#define N_SAMPLES 1000
#define N_SAMPLES_GIANT 200

static char json_src[256];
static char repeat_src[256];
static char sort_src[256];

static const char *class_source(const klass_t *k) {
    if (strcmp(k->name, "json_parse_4mb") == 0) return json_src;
    if (strcmp(k->name, "string_repeat_32mb") == 0) return repeat_src;
    if (strcmp(k->name, "array_fill_sort_1m") == 0) return sort_src;
    return k->src;
}

static void run_class(JSRuntime *rt, JSContext *ctx, isig_t *s,
                      const klass_t *k, FILE *csv) {
    const int n_target = k->giant ? N_SAMPLES_GIANT : N_SAMPLES;
    double *lat = calloc(n_target, sizeof(double));
    double *tot = calloc(n_target, sizeof(double));
    int n = 0, completed = 0;

    /* build the giant-class sources */
    snprintf(json_src, sizeof(json_src),
        "const big = JSON.stringify(Array.from({length: 64000}, (_, i) =>"
        " ({ id: i, name: 'row' + i, tags: ['x' + i, 'y' + i], score: i * 1.5, ok: i %% 3 === 0 })));"
        " let s = 0; for (let k = 0; k < 300; k++) { s += JSON.parse(big).length; } s");
    snprintf(repeat_src, sizeof(repeat_src),
        "let s = 0; for (let k = 0; k < 300; k++) { s += 'abcdefgh'.repeat(4194304).length; } s");
    snprintf(sort_src, sizeof(sort_src),
        "const a = Array.from({length: 1000000}, (_, i) => (i * 2654435761) >>> 0);"
        " let s = 0; for (let k = 0; k < 30; k++) { const c = a.slice();"
        " c.sort((x, y) => x - y); s += c[0]; } s");

    /* warmup (shapes, regexp compile); skipped for giant classes where the
     * full loop costs tens of seconds -- their first sample just runs cold */
    /* wrap in an IIFE: top-level let/const in a GLOBAL eval persist in the
     * global lexical environment and redeclaration throws on the next eval */
    char wrapped[16384];
    snprintf(wrapped, sizeof(wrapped), "(function(){\n%s\n})();", k->warm ? k->warm : "");
    if (!k->giant && k->warm) {
        JSValue w = JS_Eval(ctx, wrapped, strlen(wrapped),
                            "<warmup>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(w)) { fprintf(stderr, "[%s] warmup threw: ", k->name); js_std_dump_error(ctx); fprintf(stderr, " (ignored)\n"); }
        JS_FreeValue(ctx, w);
    }

    for (int i = 0; i < n_target; i++) {
        atomic_store_explicit(&s->flag, 0, memory_order_relaxed);
        atomic_store_explicit(&s->served_ns, 0.0, memory_order_relaxed);
        atomic_store_explicit(&s->set_ns, 0.0, memory_order_relaxed);
        s->next_delay_ns = 5e5 + rnd01() * 20e6;  /* fire at random 0.5-20.5ms */
        atomic_store_explicit(&s->armed, 1, memory_order_relaxed);

        snprintf(wrapped, sizeof(wrapped), "(function(){\n%s\n})();", class_source(k));
        double t0 = now_ns();
        JSValue r = JS_Eval(ctx, wrapped, strlen(wrapped),
                            "<loop>", JS_EVAL_TYPE_GLOBAL);
        double t1 = now_ns();
        atomic_store_explicit(&s->armed, 0, memory_order_relaxed);

        double setv = atomic_load(&s->set_ns);
        double servedv = atomic_load(&s->served_ns);
        int had_exception = JS_IsException(r);
        JS_FreeValue(ctx, r);

        if (servedv > 0 && setv > 0) {
            lat[n] = servedv - setv;
            tot[n] = t1 - setv;
            if (csv) fprintf(csv, "%s,%.0f,%.0f\n", k->name, lat[n], tot[n]);
            n++;
        } else {
            completed++; /* ran to completion / threw without seeing the flag */
            fprintf(stderr, "[%s] completed-run %d: eval=%.1fms set_after_eval=%d exception=%d\n",
                    k->name, i, (t1 - t0) / 1e6, setv > t1, had_exception);
        }
        /* let a stray late fire drain before the next sample re-arms */
        atomic_store_explicit(&s->flag, 0, memory_order_relaxed);
        struct timespec drain = { 0, 2000000 };
        nanosleep(&drain, NULL);
        if (had_exception && servedv == 0) {
            /* an exception with no signal consumed: report once */
            fprintf(stderr, "[%s] sample %d: exception without signal (uncatchable interrupt elsewhere?)\n", k->name, i);
            js_std_dump_error(ctx);
        }
    }

    stats_t ls, ts;
    summarize(lat, n, n, completed, &ls);
    summarize(tot, n, n, completed, &ts);
    printf("STATS %s latency_ns min=%.0f p10=%.0f p50=%.0f p90=%.0f p99=%.0f max=%.0f mean=%.0f n=%d served=%d completed=%d\n",
           k->name, ls.min, ls.p10, ls.p50, ls.p90, ls.p99, ls.max, ls.mean, ls.n, ls.served, ls.completed);
    printf("STATS %s total_ns   min=%.0f p10=%.0f p50=%.0f p90=%.0f p99=%.0f max=%.0f mean=%.0f n=%d\n",
           k->name, ts.min, ts.p10, ts.p50, ts.p90, ts.p99, ts.max, ts.mean, ts.n);
    free(lat); free(tot);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[harness] creating runtime\n");
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "JS_NewRuntime failed\n"); return 1; }
    /* Raw + explicit intrinsics, exactly like the generated examples
     * (JS_NewContext + JS_AddIntrinsic* re-adds and double-initializes). */
    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContextRaw failed\n"); return 1; }
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicDate(ctx);
    JS_AddIntrinsicEval(ctx);
    JS_AddIntrinsicStringNormalize(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicProxy(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicPromise(ctx);
    JS_AddIntrinsicWeakRef(ctx);
    fprintf(stderr, "[harness] intrinsics ready\n");
    static isig_t s;
    atomic_store(&s.flag, 0);
    atomic_store(&s.armed, 0);
    atomic_store(&s.stop, 0);
    JS_SetInterruptHandler(rt, handler, &s);

    pthread_t th;
    pthread_create(&th, NULL, thread_fn, &s);

    const char *csv_path = getenv("DYNA_LAT_CSV");
    FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (csv) fprintf(csv, "class,latency_ns,total_ns\n");

    int only_giant = (argc > 1 && strcmp(argv[1], "--giant") == 0);
    for (int i = 0; i < N_CLASSES; i++) {
        if (only_giant && !classes[i].giant) continue;
        if (!only_giant && classes[i].giant && argc > 1 && strcmp(argv[1], "--nogiant") == 0) continue;
        fprintf(stderr, "[harness] class %s\n", classes[i].name);
        run_class(rt, ctx, &s, &classes[i], csv);
    }

    atomic_store(&s.stop, 1);
    pthread_join(th, NULL);
    if (csv) fclose(csv);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
