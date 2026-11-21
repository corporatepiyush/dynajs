#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#define DYN_PBAR_CELLS 20

typedef struct {
    double total;
    time_t t0;
    int done;
} dyn_pbar_t;

typedef struct {
    char* text;
    int state;
    int frame;
} dyn_spin_t;

static void dyn_pbar_dispose(void* native)
{
    free(native);
}

void dyn_pbar_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_pbar_t* p = (dyn_pbar_t*)JS_GetOpaque(val, dyn_pbar_class_id);
    (void)rt;
    if (p)
        dyn_pbar_dispose(p);
}

JSValue dyn_pbar_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "total" };
    dyn_pbar_t* p;
    JSValue v;
    double total = 0;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ProgressBar(opts): opts must be an object");
    if (dyn_opts_strict(ctx, argv[0], keys, 1))
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[0], "total");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "ProgressBar({total}): total must be a number");
    }
    if (JS_ToFloat64(ctx, &total, v)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (!(total > 0) || total > 1e15)
        return JS_ThrowRangeError(ctx, "ProgressBar({total}): total must be positive");
    p = (dyn_pbar_t*)calloc(1, sizeof *p);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->total = total;
    p->t0 = time(NULL);
    return dyn_plain_wrap(ctx, new_target, dyn_pbar_class_id, p, dyn_pbar_dispose);
}

JSValue dyn_pbar_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pbar_t* p = dyn_plain_get(ctx, this_val, dyn_pbar_class_id);
    char bar[DYN_PBAR_CELLS + 1], etxt[32], line[128];
    double n;
    int filled, pct, len;
    long eta;

    if (!p)
        return JS_EXCEPTION;
    if (p->done)
        return JS_ThrowRangeError(ctx, "ProgressBar.update: already finished");
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "ProgressBar.update(n): n must be a number");
    if (JS_ToFloat64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    if (!(n >= 0) || n > p->total)
        return JS_ThrowRangeError(ctx, "ProgressBar.update(n): n must be within 0..total");
    filled = (int)(n / p->total * DYN_PBAR_CELLS);
    pct = (int)(n * 100.0 / p->total);
    if (n >= p->total) {
        filled = DYN_PBAR_CELLS;
        pct = 100;
    }
    if (n > 0) {
        long el = (long)(time(NULL) - p->t0);
        if (el < 0)
            el = 0;
        eta = (long)(el * (p->total - n) / n);
        if (eta < 0)
            eta = 0;
        snprintf(etxt, sizeof etxt, "%lds", eta);
    } else {
        snprintf(etxt, sizeof etxt, "?");
    }
    memset(bar, '#', (size_t)filled);
    memset(bar + filled, '-', (size_t)(DYN_PBAR_CELLS - filled));
    bar[DYN_PBAR_CELLS] = 0;
    len = snprintf(line, sizeof line, "\r[%s] %d%% %g/%g ETA %s\033[K",
        bar, pct, n, p->total, etxt);
    if (len > 0 && (size_t)len < sizeof line && n >= p->total) {
        if ((size_t)len + 1 < sizeof line) {
            line[len++] = '\n';
            line[len] = 0;
            p->done = 1;
        }
    }
    dyn_stdio_sync();
    if (len > 0 && write(1, line, (size_t)len) < 0) {
    }
    return JS_DupValue(ctx, this_val);
}

static void dyn_spin_dispose(void* native)
{
    dyn_spin_t* s = (dyn_spin_t*)native;
    if (s) {
        free(s->text);
        free(s);
    }
}

void dyn_spin_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spin_t* s = (dyn_spin_t*)JS_GetOpaque(val, dyn_spin_class_id);
    (void)rt;
    if (s)
        dyn_spin_dispose(s);
}

JSValue dyn_spin_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "text" };
    dyn_spin_t* s;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue v;
        if (!JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "Spinner(opts): opts must be an object");
        if (dyn_opts_strict(ctx, argv[0], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "text");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* t;
            size_t tl;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "Spinner({text}): text must be a string");
            }
            t = JS_ToCStringLen(ctx, &tl, v);
            JS_FreeValue(ctx, v);
            if (!t)
                return JS_EXCEPTION;
            s = (dyn_spin_t*)calloc(1, sizeof *s);
            if (!s) {
                JS_FreeCString(ctx, t);
                return JS_ThrowOutOfMemory(ctx);
            }
            s->text = dyn_dup_span(t, tl);
            JS_FreeCString(ctx, t);
            if (!s->text) {
                free(s);
                return JS_ThrowOutOfMemory(ctx);
            }
            return dyn_plain_wrap(ctx, new_target, dyn_spin_class_id, s, dyn_spin_dispose);
        }
    }
    s = (dyn_spin_t*)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->text = dyn_dup_span("", 0);
    if (!s->text) {
        free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_spin_class_id, s, dyn_spin_dispose);
}

static dyn_spin_t* dyn_spin_of(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_spin_t*)dyn_plain_get(ctx, this_val, dyn_spin_class_id);
}

static void dyn_spin_draw(const dyn_spin_t* s)
{
    static const char frames[] = "|/-\\";
    char line[8];
    int len = snprintf(line, sizeof line, "\r%c ", frames[s->frame & 3]);
    dyn_sb_t b;
    dyn_sb_init(&b);
    dyn_sb_put(&b, line, (size_t)len);
    dyn_sb_puts(&b, s->text ? s->text : "");
    dyn_sb_puts(&b, "\033[K");
    dyn_stdio_sync();
    if (b.p && b.n && write(1, b.p, b.n) < 0) {
    }
    dyn_sb_free(&b);
}

JSValue dyn_spin_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 0)
        return JS_ThrowRangeError(ctx, "Spinner.start: %s",
            s->state == 1 ? "already started" : "already stopped");
    s->state = 1;
    s->frame = 0;
    dyn_spin_draw(s);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_spin_tick(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.tick: %s",
            s->state == 0 ? "not started" : "already stopped");
    s->frame = (s->frame + 1) & 3;
    dyn_spin_draw(s);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_spin_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    const char* t = NULL;
    size_t tl = 0;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.stop: %s",
            s->state == 0 ? "not started" : "already stopped");
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "Spinner.stop(text): text must be a string");
        t = JS_ToCStringLen(ctx, &tl, argv[0]);
        if (!t)
            return JS_EXCEPTION;
    }
    s->state = 2;
    if (t) {
        dyn_sb_t b;
        dyn_sb_init(&b);
        dyn_sb_putc(&b, '\r');
        dyn_sb_put(&b, t, tl);
        dyn_sb_puts(&b, "\033[K\n");
        dyn_stdio_sync();
        if (b.p && b.n && write(1, b.p, b.n) < 0) {
        }
        dyn_sb_free(&b);
        JS_FreeCString(ctx, t);
    } else {
        dyn_stdio_sync();
        if (write(1, "\r\033[K", 4) < 0) {
        }
    }
    return JS_DupValue(ctx, this_val);
}

#endif
