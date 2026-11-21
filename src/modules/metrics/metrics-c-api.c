#include "metrics-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_METRICS)

static _Atomic uint32_t met_gen = 1;

void dyn_metrics_c_record(int kind, const char* name, double value)
{
    int slot;

    if (kind < DYN_MET_COUNTER || kind > DYN_MET_HISTOGRAM)
        return;
    if (!met_name_ok(name, strlen(name)))
        return;
    if (kind == DYN_MET_COUNTER && (!(value >= 0) || !(value < 9.2e18)))
        return;
    if (kind == DYN_MET_HISTOGRAM && !isfinite(value))
        return;
    slot = met_slot(name, "", MET_COUNTER + kind, NULL, 0);
    if (slot < 0)
        return;
    met_record_slot(kind, slot, value);
}

void dyn_metrics_c_record_h(dyn_met_handle_t* h, int kind, const char* name, double value)
{
    uint32_t key = atomic_load_explicit(&h->key, memory_order_relaxed);
    uint32_t gen = atomic_load_explicit(&met_gen, memory_order_acquire);
    int slot;

    if (kind < DYN_MET_COUNTER || kind > DYN_MET_HISTOGRAM)
        return;
    if (kind == DYN_MET_COUNTER && (!(value >= 0) || !(value < 9.2e18)))
        return;
    if (kind == DYN_MET_HISTOGRAM && !isfinite(value))
        return;
    if (key != 0 && (key >> MET_HANDLE_SLOT_BITS) == gen) {
        slot = (int)(key & ((1u << MET_HANDLE_SLOT_BITS) - 1u)) - 1;
    } else {
        if (!met_name_ok(name, strlen(name)))
            return;
        slot = met_slot(name, "", MET_COUNTER + kind, NULL, 0);
        if (slot < 0)
            return;
        atomic_store_explicit(&h->key, (gen << MET_HANDLE_SLOT_BITS) | (uint32_t)(slot + 1),
            memory_order_relaxed);
    }
    met_record_slot(kind, slot, value);
}

char* dyn_metrics_c_scrape(size_t* out_len)
{
    return met_scrape_buf(out_len);
}

static JSValue dyn_met_reset(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    uint32_t i;
    pthread_mutex_lock(&met_lock);
    for (i = 0; i < MET_MAX; i++) {
        size_t z;
        for (z = 0; z < MET_NBUCKET; z++)
            atomic_store_explicit(&met_hot[i].bucket[z], 0,
                memory_order_relaxed);
        atomic_store_explicit(&met_hot[i].v, 0, memory_order_relaxed);
        atomic_store_explicit(&met_hot[i].sum_bits, 0,
            memory_order_relaxed);
    }
    memset(met_cold, 0, sizeof met_cold);
    atomic_store_explicit(&met_used, 0, memory_order_release);
    {
        uint32_t next = (atomic_load_explicit(&met_gen, memory_order_relaxed) + 1u) & 0x3fffffu;
        atomic_store_explicit(&met_gen, next ? next : 1u, memory_order_release);
    }
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

int dyn_metrics_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue o = JS_NewObject(ctx);

    if (JS_IsException(o))
        return -1;
    JS_SetPropertyFunctionList(ctx, o, dyn_met_funcs,
        (int)(sizeof(dyn_met_funcs)
            / sizeof(dyn_met_funcs[0])));
    return JS_SetModuleExport(ctx, m, "Metrics", o);
}

void dyn_metrics_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "Metrics");
}


int js_nat_init_metrics(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:metrics", dyn_metrics_register);
    if (!m)
        return -1;
    dyn_metrics_add_exports(ctx, m);
    return 0;
}

#endif
