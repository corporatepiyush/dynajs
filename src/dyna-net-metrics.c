/*
 * dyna:net -- Metrics: counters, gauges and histograms with a Prometheus
 * text-format scrape.
 *
 * The point of doing this natively is that instrumenting the request path must
 * cost nothing measurable, and the request path is several worker THREADS. So
 * the layout is the design:
 *
 *   - hot and cold are separate arrays. The atomics a request touches are in
 *     one array; the name and label strings, written once at registration and
 *     read only by scrape(), are in another. A counter bump pulls one line.
 *   - one metric is EXACTLY one 64-byte cache line and the array is aligned,
 *     so two metrics bumped by two cores never share a line. Without that,
 *     unrelated counters ping-pong the line between cores and the "free"
 *     increment costs more than the work it measures.
 *   - increments are memory_order_relaxed: telemetry has no ordering
 *     relationship with anything, and seq_cst would put a barrier in the path.
 *
 * The registry is FIXED. A metric name that comes from a request would
 * otherwise let a peer allocate unbounded server memory by varying a label.
 */
#include "dyna-nat.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#define MET_MAX      256            /* fixed: a peer cannot grow this */
#define MET_NBUCKET  6
#define MET_NAME     56
#define MET_LABELS   96

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Strict options, the same rule dyna:csv and dyna:file apply: an unknown
 * own enumerable string key in the histogram options bag throws
 *   TypeError: unknown option "X" (valid: buckets)
 * instead of being silently ignored -- `{bucketz: [1]}` would otherwise
 * register the series with DEFAULT edges, a wrong-shaped series no scrape
 * consumer is told to expect. Symbol keys and non-enumerable properties
 * are not options and stay invisible (JS_GPN_STRING_MASK |
 * JS_GPN_ENUM_ONLY). Getters are NOT invoked here; the array is read
 * afterwards, so a throwing getter on a KNOWN key still propagates.
 * Returns 0 when the bag is clean, -1 with the TypeError pending. */
static int dyn_met_opts_check(JSContext *ctx, JSValueConst o,
                              const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { bad = 1; break; }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) { JS_FreeCString(ctx, name); bad = 1; break; }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
                memcpy(w, keys[k], l);
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

static const char *const dyn_met_hist_keys[] = { "buckets" };

enum { MET_FREE = 0, MET_COUNTER, MET_GAUGE, MET_HISTOGRAM };

/* Touched by every observation on every thread. Exactly one cache line. */
typedef struct {
    _Atomic uint64_t v;                 /* counter total, gauge bits, or
                                           histogram observation count */
    _Atomic uint64_t sum_bits;          /* histogram: sum, as double bits */
    _Atomic uint64_t bucket[MET_NBUCKET];
} met_hot_t;

_Static_assert(sizeof(met_hot_t) == 64,
               "met_hot_t must be exactly one cache line: two counters sharing "
               "a line is the false sharing this layout exists to prevent");

/* Written once at registration, read only by scrape(). A histogram may
   carry its OWN bucket edges (up to MET_NBUCKET of them, per series); the
   array stays MET_NBUCKET deep so the hot line above is untouched. */
typedef struct {
    char    name[MET_NAME];
    char    labels[MET_LABELS];
    uint8_t kind;
    uint8_t n_bounds;                    /* histograms: how many edges */
    double  bounds[MET_NBUCKET];         /* histograms: the edges (seconds) */
} met_cold_t;

/* Seconds. Cumulative in the exposition, so scrape() runs the running total. */
static const double MET_BOUND[MET_NBUCKET] = {
    0.005, 0.01, 0.05, 0.1, 0.5, 1.0,
};

static _Alignas(64) met_hot_t met_hot[MET_MAX];
static met_cold_t met_cold[MET_MAX];
static _Atomic uint32_t met_used;       /* high-water mark of live slots */

/* Registration takes a lock; observation never does. A metric is registered
   once per name and then bumped forever, so contention here is irrelevant. */
static pthread_mutex_t met_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t met_dtou(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static double met_utod(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* A Prometheus name is [a-zA-Z_:][a-zA-Z0-9_:]*. Refusing an invalid one beats
   emitting a scrape body the collector rejects wholesale. */
static int met_name_ok(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n >= MET_NAME)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || c == '_' || c == ':';
        if (!(alpha || (i > 0 && c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

/* Find, or claim, the slot for (name, labels). Returns -1 when the registry is
   full -- which is a refusal, not a silent overwrite of somebody else's metric. */
static int met_slot(const char *name, const char *labels, int kind,
                    const double *bounds, uint8_t n_bounds)
{
    uint32_t i, n;
    int slot = -1;

    pthread_mutex_lock(&met_lock);
    n = atomic_load_explicit(&met_used, memory_order_relaxed);
    for (i = 0; i < n; i++) {
        if (met_cold[i].kind != MET_FREE
            && strcmp(met_cold[i].name, name) == 0
            && strcmp(met_cold[i].labels, labels) == 0) {
            slot = met_cold[i].kind == kind ? (int)i : -2;  /* -2: wrong type */
            pthread_mutex_unlock(&met_lock);
            return slot;
        }
    }
    if (n < MET_MAX) {
        slot = (int)n;
        memset(&met_hot[slot], 0, sizeof met_hot[slot]);
        snprintf(met_cold[slot].name, MET_NAME, "%s", name);
        snprintf(met_cold[slot].labels, MET_LABELS, "%s", labels);
        met_cold[slot].kind = (uint8_t)kind;
        if (bounds && n_bounds > 0 && n_bounds <= MET_NBUCKET) {
            memcpy(met_cold[slot].bounds, bounds,
                   n_bounds * sizeof(met_cold[slot].bounds[0]));
            met_cold[slot].n_bounds = n_bounds;
        } else {
            memcpy(met_cold[slot].bounds, MET_BOUND, sizeof MET_BOUND);
            met_cold[slot].n_bounds = MET_NBUCKET;
        }
        atomic_store_explicit(&met_used, n + 1, memory_order_release);
    }
    pthread_mutex_unlock(&met_lock);
    return slot;
}

/* Labels come in as an object and are rendered once, at registration, into the
   exact bytes the exposition needs -- scrape() then does no formatting work. */
static int met_labels(JSContext *ctx, JSValueConst v, char *out, size_t cap)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, k;
    size_t off = 0;
    int rc = 0;

    out[0] = '\0';
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "Metrics: labels must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        const char *key = JS_AtomToCString(ctx, tab[k].atom);
        JSValue val;
        const char *sv;
        int nw;
        if (!key) { rc = -1; break; }
        /* A Prometheus label NAME is [a-zA-Z_][a-zA-Z0-9_]* (values are
           escaped below; a quote inside a KEY would break the exposition
           grammar in a way escaping cannot express), so it is validated
           rather than smuggled through. */
        {
            size_t ki;
            int bad = key[0] == '\0';
            for (ki = 0; key[ki]; ki++) {
                char c = key[ki];
                int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                            || c == '_';
                if (!(alpha || (ki > 0 && c >= '0' && c <= '9'))) {
                    bad = 1;
                    break;
                }
            }
            if (bad) {
                JS_ThrowTypeError(ctx, "Metrics: label name '%s' is not "
                                       "[a-zA-Z_][a-zA-Z0-9_]*", key);
                JS_FreeCString(ctx, key);
                rc = -1;
                break;
            }
        }
        val = JS_GetProperty(ctx, v, tab[k].atom);
        if (JS_IsException(val)) { JS_FreeCString(ctx, key); rc = -1; break; }
        sv = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);
        if (!sv) { JS_FreeCString(ctx, key); rc = -1; break; }
        /* A quote or backslash in a label value would break the exposition
           grammar, so they are escaped rather than passed through. */
        nw = snprintf(out + off, cap - off, "%s%s=\"", off ? "," : "", key);
        if (nw < 0 || (size_t)nw >= cap - off) rc = -1;
        else {
            size_t i;
            off += (size_t)nw;
            for (i = 0; sv[i] && rc == 0; i++) {
                char c = sv[i];
                if (off + 3 >= cap) { rc = -1; break; }
                if (c == '"' || c == '\\' || c == '\n') {
                    out[off++] = '\\';
                    out[off++] = c == '\n' ? 'n' : c;
                } else {
                    out[off++] = c;
                }
            }
            if (rc == 0) {
                if (off + 2 >= cap) rc = -1;
                else out[off++] = '"';
            }
            out[off] = '\0';
        }
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, sv);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (rc < 0 && !JS_HasException(ctx))
        JS_ThrowRangeError(ctx, "Metrics: the label set is too long (max %d "
                                "bytes rendered)", MET_LABELS - 1);
    return rc;
}

/* Validate a custom bucket-edge array: 1..MET_NBUCKET finite,
 * strictly increasing, positive SECONDS. The count cap keeps the hot
 * per-series struct at one cache line; that bound is the same one the
 * default edges live under. 0 / -1 with a pending exception. */
static int dyn_met_check_buckets(JSContext *ctx, JSValueConst arr,
                                 double *out, uint64_t *out_n)
{
    uint64_t len = 0, i;

    {
        int isarr = JS_IsArray(ctx, arr);
        if (isarr < 0)
            return -1;
        if (!isarr) {
            JS_ThrowTypeError(ctx, "Metrics.histogram: buckets must be an array");
            return -1;
        }
    }
    {
        JSValue l = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t ulen = 0;
        if (JS_IsException(l))
            return -1;
        if (JS_ToUint32(ctx, &ulen, l)) {
            JS_FreeValue(ctx, l);
            return -1;
        }
        JS_FreeValue(ctx, l);
        len = (uint64_t)ulen;
    }
    if (len < 1 || len > MET_NBUCKET) {
        JS_ThrowRangeError(ctx, "Metrics.histogram: buckets holds 1 to %d "
                                "edges", MET_NBUCKET);
        return -1;
    }
    {
        double prev = 0;
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            double d;
            if (JS_IsException(e))
                return -1;
            if (!JS_IsNumber(e) || JS_ToFloat64(ctx, &d, e) ||
                !isfinite(d) || d <= 0) {
                JS_FreeValue(ctx, e);
                JS_ThrowRangeError(ctx, "Metrics.histogram: every bucket edge "
                                        "must be a finite, positive number of "
                                        "seconds");
                return -1;
            }
            if (i > 0 && d <= prev) {
                JS_FreeValue(ctx, e);
                JS_ThrowRangeError(ctx, "Metrics.histogram: bucket edges must "
                                        "be strictly increasing");
                return -1;
            }
            prev = d;
            out[i] = d;
            JS_FreeValue(ctx, e);
        }
    }
    *out_n = len;
    return 0;
}

/* magic 0 = counter(name, value=1, labels), 1 = gauge(name, value, labels),
   2 = histogram(name, value, labels) */
static JSValue dyn_met_observe(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *WHAT[] = { "counter", "gauge", "histogram" };
    const int kind = magic + MET_COUNTER;
    char labels[MET_LABELS];
    const char *name;
    size_t nlen;
    double value = 1.0;
    /* Storage for per-series edges. It MUST live in this frame, not in the
       option-reading block below: the pointer handed to met_slot() is read
       (memcpy) on the registration path after that block has ended, and a
       block-scoped array there is dead by then (stack-use-after-scope). */
    double custom[MET_NBUCKET];
    double *custom_buckets = NULL;
    uint8_t custom_n = 0;
    int slot, i;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Metrics.%s(name, ...): name is required",
                                 WHAT[magic]);
    if (magic != 0 && argc < 2)
        return JS_ThrowTypeError(ctx, "Metrics.%s(name, value): value is "
                                      "required", WHAT[magic]);
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &value, argv[1]))
            return JS_EXCEPTION;
    }
    if (magic == 0 && (!isfinite(value) || value < 0 || value >= 9.2e18))
        return JS_ThrowRangeError(ctx, "Metrics.counter: an increment must be "
                                       "finite, >= 0 and < 2^63 -- a counter "
                                       "is a uint64, and casting a non-finite "
                                       "or out-of-range double is undefined");
    if (magic == 2 && (!isfinite(value)))
        return JS_ThrowRangeError(ctx, "Metrics.histogram: value must be finite");
    name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (!met_name_ok(name, nlen)) {
        JSValue e = JS_ThrowRangeError(ctx, "Metrics: '%s' is not a valid "
            "metric name -- [a-zA-Z_:][a-zA-Z0-9_:]* up to %d bytes",
            name, MET_NAME - 1);
        JS_FreeCString(ctx, name);      /* after the throw: it reads the name */
        return e;
    }
    if (met_labels(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, labels,
                   sizeof labels) < 0) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (magic == 2 && argc > 3 && !JS_IsUndefined(argv[3]) &&
        !JS_IsNull(argv[3])) {
        /*{buckets: [..]} -- per-series edges, read at registration.
         * Strict: wrong types are errors, and the KEY set is closed
         * too, before the array is read. */
        uint64_t blen = 0;
        if (dyn_met_opts_check(ctx, argv[3], dyn_met_hist_keys,
                               countof(dyn_met_hist_keys))) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        JSValue arr = JS_GetPropertyStr(ctx, argv[3], "buckets");
        if (JS_IsException(arr)) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        if (dyn_met_check_buckets(ctx, arr, custom, &blen)) {
            JS_FreeValue(ctx, arr);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        custom_buckets = custom;
        custom_n = (uint8_t)blen;
        JS_FreeValue(ctx, arr);
    }
    slot = met_slot(name, labels, kind, custom_buckets, custom_n);
    JS_FreeCString(ctx, name);
    if (slot == -2)
        return JS_ThrowTypeError(ctx, "Metrics: that name and label set is "
                                      "already registered as another type");
    if (slot < 0)
        return JS_ThrowRangeError(ctx, "Metrics: the registry holds %d series "
            "and is full -- the bound is what stops a request-derived label "
            "from growing it", MET_MAX);

    /* relaxed: telemetry orders nothing, and a barrier here would be the
       entire cost of instrumenting a fast path. */
    if (magic == 0) {
        atomic_fetch_add_explicit(&met_hot[slot].v, (uint64_t)value,
                                  memory_order_relaxed);
    } else if (magic == 1) {
        atomic_store_explicit(&met_hot[slot].v, met_dtou(value),
                              memory_order_relaxed);
    } else {
        const double *bd = met_cold[slot].bounds;
        int nb = met_cold[slot].n_bounds;
        uint64_t old, nw;
        for (i = 0; i < nb; i++)
            if (value <= bd[i])
                atomic_fetch_add_explicit(&met_hot[slot].bucket[i], 1,
                                          memory_order_relaxed);
        atomic_fetch_add_explicit(&met_hot[slot].v, 1, memory_order_relaxed);
        do {    /* a double sum has no atomic add: CAS on the bit pattern */
            old = atomic_load_explicit(&met_hot[slot].sum_bits,
                                       memory_order_relaxed);
            nw = met_dtou(met_utod(old) + value);
        } while (!atomic_compare_exchange_weak_explicit(
                     &met_hot[slot].sum_bits, &old, nw,
                     memory_order_relaxed, memory_order_relaxed));
    }
    return JS_UNDEFINED;
}

static int met_append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);

    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        char *np;
        while (nc < *len + n + 1)
            nc *= 2;
        np = (char *)realloc(*buf, nc);
        if (!np)
            return -1;
        *buf = np;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

/* The C half of the registry, for hot surfaces with no JS in the loop (the
   HTTP server's per-response bump). Labels stay a JS-boundary feature; from C
   the label set is always empty. A bad name, a full registry or a type clash
   DROPS the observation: telemetry must never fail the work it measures. */
void dyn_metrics_c_record(int kind, const char *name, double value)
{
    int slot, i;

    if (kind < DYN_MET_COUNTER || kind > DYN_MET_HISTOGRAM)
        return;
    if (!met_name_ok(name, strlen(name)))
        return;
    /* Same gate as the JS path: NaN passes `value < 0`, and casting a
       non-finite or out-of-range double to uint64 is undefined. */
    if (kind == DYN_MET_COUNTER &&
        (!(value >= 0) || !(value < 9.2e18)))
        return;
    slot = met_slot(name, "", MET_COUNTER + kind, NULL, 0);
    if (slot < 0)
        return;
    /* relaxed, exactly like the JS path: telemetry orders nothing */
    if (kind == DYN_MET_COUNTER) {
        atomic_fetch_add_explicit(&met_hot[slot].v, (uint64_t)value,
                                  memory_order_relaxed);
    } else if (kind == DYN_MET_GAUGE) {
        atomic_store_explicit(&met_hot[slot].v, met_dtou(value),
                              memory_order_relaxed);
    } else {
        const double *bd = met_cold[slot].bounds;
        int nb = met_cold[slot].n_bounds;
        uint64_t old, nw;
        for (i = 0; i < nb; i++)
            if (value <= bd[i])
                atomic_fetch_add_explicit(&met_hot[slot].bucket[i], 1,
                                          memory_order_relaxed);
        atomic_fetch_add_explicit(&met_hot[slot].v, 1, memory_order_relaxed);
        do {    /* a double sum has no atomic add: CAS on the bit pattern */
            old = atomic_load_explicit(&met_hot[slot].sum_bits,
                                       memory_order_relaxed);
            nw = met_dtou(met_utod(old) + value);
        } while (!atomic_compare_exchange_weak_explicit(
                     &met_hot[slot].sum_bits, &old, nw,
                     memory_order_relaxed, memory_order_relaxed));
    }
}

/* The registry as Prometheus text, malloc'd. Shared by the JS scrape() and
   the App's /metrics route. NULL on OOM. */
static char *met_scrape_buf(size_t *out_len)
{
    char *buf = NULL, line[512];
    size_t len = 0, cap = 0;
    uint32_t i, n;

    n = atomic_load_explicit(&met_used, memory_order_acquire);
    for (i = 0; i < n; i++) {
        const met_cold_t *c = &met_cold[i];
        const char *lb = c->labels;
        char open_[2] = "{", close_[2] = "}";
        if (!lb[0]) { open_[0] = '\0'; close_[0] = '\0'; }
        int k;
        if (c->kind == MET_FREE)
            continue;
        snprintf(line, sizeof line, "# TYPE %s %s\n", c->name,
                 c->kind == MET_COUNTER ? "counter"
                 : c->kind == MET_GAUGE ? "gauge" : "histogram");
        if (met_append(&buf, &len, &cap, line) < 0)
            goto oom;
        if (c->kind == MET_COUNTER) {
            snprintf(line, sizeof line, "%s%s%s%s %llu\n", c->name,
                     open_, lb, close_,
                     (unsigned long long)atomic_load_explicit(
                         &met_hot[i].v, memory_order_relaxed));
        } else if (c->kind == MET_GAUGE) {
            /* Non-finite values are legal input; the exposition grammar
               only accepts the +Inf/-Inf/NaN tokens, which %.17g would
               emit as inf/nan and a collector would refuse. */
            double gv = met_utod(atomic_load_explicit(&met_hot[i].v,
                                                      memory_order_relaxed));
            uint64_t gb = met_dtou(gv);
            if ((gb & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
                snprintf(line, sizeof line, "%s%s%s%s %s\n", c->name,
                         open_, lb, close_,
                         (gb & 0xFFFFFFFFFFFFFULL) ? "NaN"
                         : (gb >> 63) ? "-Inf" : "+Inf");
            } else {
                snprintf(line, sizeof line, "%s%s%s%s %.17g\n", c->name,
                         open_, lb, close_, gv);
            }
        } else {
            uint64_t cum = 0, total;
            uint8_t nb = c->n_bounds ? c->n_bounds : (uint8_t)MET_NBUCKET;
            for (k = 0; k < nb; k++) {
                /* Prometheus buckets are CUMULATIVE: le="0.01" counts
                   everything at or below, not just this band. */
                cum = atomic_load_explicit(&met_hot[i].bucket[k],
                                           memory_order_relaxed);
                snprintf(line, sizeof line, "%s_bucket{%s%sle=\"%g\"} %llu\n",
                         c->name, lb, lb[0] ? "," : "", c->bounds[k],
                         (unsigned long long)cum);
                if (met_append(&buf, &len, &cap, line) < 0)
                    goto oom;
            }
            total = atomic_load_explicit(&met_hot[i].v, memory_order_relaxed);
            snprintf(line, sizeof line, "%s_bucket{%s%sle=\"+Inf\"} %llu\n",
                     c->name, lb, lb[0] ? "," : "", (unsigned long long)total);
            if (met_append(&buf, &len, &cap, line) < 0)
                goto oom;
            snprintf(line, sizeof line, "%s_sum%s%s%s %.17g\n",
                     c->name, open_, lb, close_,
                     met_utod(atomic_load_explicit(&met_hot[i].sum_bits,
                                                   memory_order_relaxed)));
            if (met_append(&buf, &len, &cap, line) < 0)
                goto oom;
            snprintf(line, sizeof line, "%s_count%s%s%s %llu\n",
                     c->name, open_, lb, close_, (unsigned long long)total);
        }
        if (met_append(&buf, &len, &cap, line) < 0)
            goto oom;
    }
    if (!buf) {                 /* an empty registry is an empty document */
        buf = (char *)malloc(1);
        if (buf)
            buf[0] = '\0';
    }
    *out_len = len;
    return buf;
oom:
    free(buf);
    return NULL;
}

char *dyn_metrics_c_scrape(size_t *out_len)
{
    return met_scrape_buf(out_len);
}

static JSValue dyn_met_scrape(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    size_t len = 0;
    char *buf;
    JSValue r;
    (void)this_val; (void)argc; (void)argv;

    buf = met_scrape_buf(&len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    r = JS_NewStringLen(ctx, buf, len);
    free(buf);
    return r;
}

/* Tests need a clean registry, and a long-running process may want to drop a
   label set it will never see again. Nothing else should call this. */
static JSValue dyn_met_reset(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    pthread_mutex_lock(&met_lock);
    memset(met_hot, 0, sizeof met_hot);
    memset(met_cold, 0, sizeof met_cold);
    atomic_store_explicit(&met_used, 0, memory_order_release);
    pthread_mutex_unlock(&met_lock);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_met_funcs[] = {
    JS_CFUNC_MAGIC_DEF("counter", 3, dyn_met_observe, 0),
    JS_CFUNC_MAGIC_DEF("gauge", 3, dyn_met_observe, 1),
    JS_CFUNC_MAGIC_DEF("histogram", 4, dyn_met_observe, 2),
    JS_CFUNC_DEF("scrape", 0, dyn_met_scrape),
    JS_CFUNC_DEF("reset", 0, dyn_met_reset),
};

int dyn_metrics_register(JSContext *ctx, JSModuleDef *m)
{
    JSValue o = JS_NewObject(ctx);

    if (JS_IsException(o))
        return -1;
    JS_SetPropertyFunctionList(ctx, o, dyn_met_funcs,
                               (int)(sizeof(dyn_met_funcs)
                                     / sizeof(dyn_met_funcs[0])));
    return JS_SetModuleExport(ctx, m, "Metrics", o);
}

void dyn_metrics_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "Metrics");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
