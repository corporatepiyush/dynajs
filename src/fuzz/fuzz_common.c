#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/fuzz/fuzz_common.h"
#include "../../tests/lib/dyn-oomrt.h"

void fuzz_drain_or_abort(JSContext* ctx, JSValueConst v)
{
    fuzz_loop(ctx);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        abort();
    }
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_Throw(ctx, JS_DupValue(ctx, JS_PromiseResult(ctx, v)));
        (void)err;
        js_std_dump_error(ctx);
        abort();
    }
}

static int interrupt_handler(JSRuntime* rt, void* opaque)
{
    nbinterrupts++;
    return (nbinterrupts > 100);
}

void reset_nbinterrupts(void)
{
    nbinterrupts = 0;
}

void fuzz_loop(JSContext* ctx)
{
    int err;
    for (;;) {
        err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
        if (err <= 0) {
            if (err < 0)
                js_std_dump_error(ctx);
            break;
        }
    }
}

void test_one_input_init(JSRuntime* rt, JSContext* ctx)
{
    JS_SetMaxStackSize(rt, 0x10000);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, NULL, NULL);
    JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);

    js_std_init_handlers(rt);
    reset_nbinterrupts();
}

static uint64_t fz_ticks, fz_max_ticks, fz_deadline, fz_fired_at;
static int fz_fire;
static fz_env_t* fz_cur;
static char fz_prefix[512];

static uint64_t fz_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void fz_fail(const char* what)
{
    fprintf(stderr, "fuzz oracle failed: %s\n", what);
    abort();
}

static int fz_interrupt(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (fz_fire)
        return 1;
    fz_ticks++;
    if ((fz_max_ticks && fz_ticks > fz_max_ticks) || (fz_deadline && fz_now() > fz_deadline)) {
        fz_fire = 1;
        fz_fired_at = fz_now();
        return 1;
    }
    return 0;
}

void fz_arm(uint64_t budget_ns, unsigned max_ticks)
{
    fz_ticks = 0;
    fz_fire = 0;
    fz_max_ticks = max_ticks;
    fz_deadline = budget_ns ? fz_now() + budget_ns : 0;
}

int fz_fired(void) { return fz_fire; }

void fz_args(int argc, char** argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], "-artifact_prefix=", 17))
            snprintf(fz_prefix, sizeof(fz_prefix), "%s", argv[i] + 17);
}

static void fz_rt_new(fz_env_t* e)
{
    int i;
    fz_arm(0, 0);
    oomrt_cap = 0x4000000;
    e->rt = oomrt_new();
    if (!e->rt)
        fz_fail("JS_NewRuntime");
    JS_SetMaxStackSize(e->rt, 0x40000);
    JS_SetInterruptHandler(e->rt, fz_interrupt, NULL);
    for (i = 0; i < e->nctx; i++) {
        e->ctx[i] = JS_NewContext(e->rt);
        if (!e->ctx[i])
            fz_fail("JS_NewContext");
        if (e->init)
            e->init(e->ctx[i]);
    }
}

static const char* fz_rt_free(fz_env_t* e)
{
    const char* bad = NULL;
    int i;
    for (i = 0; i < e->nctx; i++)
        if (e->ctx[i]) {
            JS_FreeContext(e->ctx[i]);
            e->ctx[i] = NULL;
        }
    if (!JS_IsJobPending(e->rt)) {
        JSMemoryUsage u;
        JS_RunGC(e->rt);
        JS_ComputeMemoryUsage(e->rt, &u);
        if (u.obj_count || u.shape_count || u.js_func_count || u.fast_array_count || u.binary_object_count) {
            bad = "memory not returned after freeing every context and GC";
        } else if (u.atom_count > e->base + 4)
            bad = "atoms not returned after freeing every context and GC";
    }
    if (bad) {
        e->leaked_rt[e->nleaked++ & 7] = e->rt;
    } else
        JS_FreeRuntime(e->rt);
    e->rt = NULL;
    e->used = 0;
    e->dirty = 0;
    return bad;
}

static void fz_save(const uint8_t* p, size_t n, const char* tag, unsigned k)
{
    char path[640];
    FILE* f;
    snprintf(path, sizeof(path), "%s%s-%u.bin", fz_prefix[0] ? fz_prefix : "./fz-", tag, k);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(p, 1, n, f);
    fclose(f);
    fprintf(stderr, "fuzz: wrote %s (%zu bytes)\n", path, n);
}

static void fz_bisect(fz_env_t* e, const char* what)
{
    unsigned k;
    fprintf(stderr, "fuzz: %s; replaying the %u inputs of the batch one per fresh runtime\n", what, e->nb);
    for (k = 0; k < e->nb; k++) {
        fz_rt_new(e);
        e->run(e, e->bq[k], e->bl[k]);
        if (fz_rt_free(e)) {
            fz_save(e->bq[k], e->bl[k], "leak", k);
            fz_fail(what);
        }
    }
    for (k = 0; k < e->nb; k++)
        fz_save(e->bq[k], e->bl[k], "leak-batch", k);
    fz_fail("leak needs several inputs of the batch together (written as leak-batch-*)");
}

static void fz_batch_clear(fz_env_t* e)
{
    unsigned k;
    for (k = 0; k < e->nb; k++)
        free(e->bq[k]);
    e->nb = 0;
}

void fz_env_recycle(fz_env_t* e)
{
    const char* bad;
    if (!e->rt)
        return;
    bad = fz_rt_free(e);
    e->batches++;
    if (bad)
        fz_bisect(e, bad);
    fz_batch_clear(e);
}

void fz_env_open(fz_env_t* e, int nctx, unsigned recycle, void (*init)(JSContext*), void (*run)(fz_env_t*, const uint8_t*, size_t))
{
    const char* s = getenv("FZ_RECYCLE");
    JSRuntime* rt;
    JSContext* c;
    JSMemoryUsage u;
    memset(e, 0, sizeof(*e));
    e->nctx = nctx < 1 ? 1 : nctx > 2 ? 2 : nctx;
    e->init = init;
    e->run = run;
    {
        unsigned long v = s && *s ? strtoul(s, NULL, 10) : 0;
        e->recycle = v > 0 ? (unsigned)(v > FZ_BATCH_MAX ? FZ_BATCH_MAX : v) : recycle;
    }
    if (e->recycle > FZ_BATCH_MAX)
        e->recycle = FZ_BATCH_MAX;
    fz_cur = e;
    oomrt_cap = 0x4000000;
    rt = oomrt_new();
    c = rt ? JS_NewContext(rt) : NULL;
    if (!c)
        fz_fail("calibration runtime");
    if (init)
        init(c);
    JS_FreeContext(c);
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &u);
    e->base = u.atom_count;
    JS_FreeRuntime(rt);
}

void fz_one(fz_env_t* e, const uint8_t* data, size_t size)
{
    uint8_t* copy;
    if (e->rt && (e->used >= e->recycle || e->dirty))
        fz_env_recycle(e);
    if (!e->rt)
        fz_rt_new(e);
    copy = malloc(size ? size : 1);
    if (!copy)
        fz_fail("malloc");
    memcpy(copy, data, size);
    e->bq[e->nb] = copy;
    e->bl[e->nb++] = size;
    e->used++;
    e->run(e, data, size);
}

int fz_is_budget_error(JSContext* ctx, JSValueConst exc)
{
    const char* s;
    int r = 0;
    if (!JS_IsError(ctx, exc))
        return 0;
    s = JS_ToCString(ctx, exc);
    if (s) {
        r = strstr(s, "out of memory") || strstr(s, "stack overflow") || strstr(s, "interrupted") || strstr(s, "too many") || strstr(s, "is too large") || strstr(s, "invalid array length");
        JS_FreeCString(ctx, s);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        r = 1;
    }
    return r;
}

void fz_drain(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    unsigned n = 0;
    JSContext* c;
    while (JS_IsJobPending(rt)) {
        if (++n > 20000 || fz_fire) {
            if (fz_cur)
                fz_cur->dirty = 1;
            return;
        }
        if (JS_ExecutePendingJob(rt, &c) < 0 && c) {
            JSValue x = JS_GetException(c);
            if (fz_is_budget_error(c, x) && fz_cur)
                fz_cur->dirty = 1;
            JS_FreeValue(c, x);
        }
    }
}

void fz_settle(JSContext* ctx, JSValue v, const char* what)
{
    if (what && fz_fire && fz_now() - fz_fired_at > 2000000000u)
        fz_fail("interrupt honoured more than 2 s late");
    if (JS_IsException(v)) {
        JSValue x = JS_GetException(ctx);
        if (fz_is_budget_error(ctx, x) && fz_cur)
            fz_cur->dirty = 1;
        JS_FreeValue(ctx, x);
    } else {
        if (fz_fire && what)
            fz_fail(what);
        JS_FreeValue(ctx, v);
    }
    fz_drain(ctx);
}

unsigned fz_u8(fz_in_t* in)
{
    unsigned c;
    if (!in->n)
        return 0;
    c = *in->p++;
    in->n--;
    return c;
}

unsigned fz_below(fz_in_t* in, unsigned n)
{
    unsigned v;
    if (n <= 1)
        return 0;
    v = fz_u8(in);
    if (n > 256)
        v = v << 8 | fz_u8(in);
    return v % n;
}

int fz_print(void)
{
    static int p = -1;
    if (p < 0)
        p = getenv("FZ_PRINT") != NULL;
    return p;
}
