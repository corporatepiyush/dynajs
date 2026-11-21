#include "metrics-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_METRICS)

static const char* const dyn_met_hist_keys[] = { "buckets" };

static const double MET_BOUND[MET_NBUCKET] = {
    0.005,
    0.01,
    0.05,
    0.1,
    0.5,
    1.0,
};

int met_slot(const char* name, const char* labels, int kind,
    const double* bounds, uint8_t n_bounds)
{
    uint32_t i, n;
    int slot = -1;

    pthread_mutex_lock(&met_lock);
    n = atomic_load_explicit(&met_used, memory_order_relaxed);
    for (i = 0; i < n; i++) {
        if (met_cold[i].kind != MET_FREE
            && strcmp(met_cold[i].name, name) == 0
            && strcmp(met_cold[i].labels, labels) == 0) {
            slot = met_cold[i].kind == kind ? (int)i : -2;
            pthread_mutex_unlock(&met_lock);
            return slot;
        }
    }
    if (n < MET_MAX) {
        size_t z;
        slot = (int)n;
        for (z = 0; z < MET_NBUCKET; z++)
            atomic_store_explicit(&met_hot[slot].bucket[z], 0,
                memory_order_relaxed);
        atomic_store_explicit(&met_hot[slot].v, 0, memory_order_relaxed);
        atomic_store_explicit(&met_hot[slot].sum_bits, 0,
            memory_order_relaxed);
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

static int dyn_met_check_buckets(JSContext* ctx, JSValueConst arr,
    double* out, uint64_t* out_n)
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
                                "edges",
            MET_NBUCKET);
        return -1;
    }
    {
        double prev = 0;
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            double d;
            if (JS_IsException(e))
                return -1;
            if (!JS_IsNumber(e) || JS_ToFloat64(ctx, &d, e) || !isfinite(d) || d <= 0) {
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

JSValue dyn_met_observe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* WHAT[] = { "counter", "gauge", "histogram" };
    const int kind = magic + MET_COUNTER;
    char labels[MET_LABELS];
    const char* name;
    size_t nlen;
    double value = 1.0;
    double custom[MET_NBUCKET];
    double* custom_buckets = NULL;
    uint8_t custom_n = 0;
    int slot, i;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Metrics.%s(name, ...): name is required",
            WHAT[magic]);
    if (magic != 0 && argc < 2)
        return JS_ThrowTypeError(ctx, "Metrics.%s(name, value): value is "
                                      "required",
            WHAT[magic]);
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
        JS_FreeCString(ctx, name);
        return e;
    }
    if (met_labels(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, labels,
            sizeof labels)
        < 0) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (magic == 2 && argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        uint64_t blen = 0;
        if (dyn_opts_strict(ctx, argv[3], dyn_met_hist_keys,
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
                                       "from growing it",
            MET_MAX);

    if (magic == 0) {
        atomic_fetch_add_explicit(&met_hot[slot].v, (uint64_t)value,
            memory_order_relaxed);
    } else if (magic == 1) {
        atomic_store_explicit(&met_hot[slot].v, met_dtou(value),
            memory_order_relaxed);
    } else {
        double bd[MET_NBUCKET];
        int nb;
        uint64_t old, nw;
        pthread_mutex_lock(&met_lock);
        nb = met_cold[slot].n_bounds;
        memcpy(bd, met_cold[slot].bounds, sizeof bd);
        pthread_mutex_unlock(&met_lock);
        for (i = 0; i < nb; i++)
            if (value <= bd[i])
                atomic_fetch_add_explicit(&met_hot[slot].bucket[i], 1,
                    memory_order_relaxed);
        atomic_fetch_add_explicit(&met_hot[slot].v, 1, memory_order_relaxed);
        do {
            old = atomic_load_explicit(&met_hot[slot].sum_bits,
                memory_order_relaxed);
            nw = met_dtou(met_utod(old) + value);
        } while (!atomic_compare_exchange_weak_explicit(
            &met_hot[slot].sum_bits, &old, nw,
            memory_order_relaxed, memory_order_relaxed));
    }
    return JS_UNDEFINED;
}

void met_record_slot(int kind, int slot, double value)
{
    int i;

    if (kind == DYN_MET_COUNTER) {
        atomic_fetch_add_explicit(&met_hot[slot].v, (uint64_t)value,
            memory_order_relaxed);
    } else if (kind == DYN_MET_GAUGE) {
        atomic_store_explicit(&met_hot[slot].v, met_dtou(value),
            memory_order_relaxed);
    } else {
        double bd[MET_NBUCKET];
        int nb;
        uint64_t old, nw;
        pthread_mutex_lock(&met_lock);
        nb = met_cold[slot].n_bounds;
        memcpy(bd, met_cold[slot].bounds, sizeof bd);
        pthread_mutex_unlock(&met_lock);
        for (i = 0; i < nb; i++)
            if (value <= bd[i])
                atomic_fetch_add_explicit(&met_hot[slot].bucket[i], 1,
                    memory_order_relaxed);
        atomic_fetch_add_explicit(&met_hot[slot].v, 1, memory_order_relaxed);
        do {
            old = atomic_load_explicit(&met_hot[slot].sum_bits,
                memory_order_relaxed);
            nw = met_dtou(met_utod(old) + value);
        } while (!atomic_compare_exchange_weak_explicit(
            &met_hot[slot].sum_bits, &old, nw,
            memory_order_relaxed, memory_order_relaxed));
    }
}

#endif
