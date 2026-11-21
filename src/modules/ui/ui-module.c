#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

enum { UIB_OPEN_DRAWER,
    UIB_OPEN_SHEET };

enum { UIB_BREADCRUMBS,
    UIB_STEPPER };

static const uint8_t ui_proggy[] = {
#include "proggy_clean.inc"
};

static const uint8_t ui_inter[] = {
#include "inter.inc"
};

JSClassID uib_cid, uib_u8_cid, uib_f64_cid, uib_f32_cid;

static int uib_view(JSContext* ctx, uib* u);

static void uib_dispose(void* p)
{
    uib* u = p;
    if (!u)
        return;
    if (u->ops)
        u->ops->release(u->be, u->target_obj);
    dyn_ui_free(u->ui);
    dyn_nat_free(u->px);
    dyn_nat_free(u->strs);
    if (u->rt) {
        JS_FreeValueRT(u->rt, u->target_obj);
        JS_FreeObjectTemplateRT(u->rt, u->rect_tpl);
        JS_FreeObjectTemplateRT(u->rt, u->table_tpl);
        JS_FreeValueRT(u->rt, u->dnd_payload);
    }
    dyn_nat_free(u);
}

static JSObjectTemplate* uib_template(JSContext* ctx, const char* const* names, int n)
{
    JSAtom at[8];
    JSObjectTemplate* t = NULL;
    int k, ok = 1;
    for (k = 0; k < n; k++)
        if ((at[k] = JS_NewAtom(ctx, names[k])) == JS_ATOM_NULL)
            ok = 0;
    if (ok)
        t = JS_NewObjectTemplate(ctx, at, (uint32_t)n);
    for (k = 0; k < n; k++)
        if (at[k] != JS_ATOM_NULL)
            JS_FreeAtom(ctx, at[k]);
    return t;
}

static void uib_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* r = JS_GetOpaque(val, uib_cid);
    uib* u = r ? r->native : NULL;
    if (u) {
        JS_MarkValue(rt, u->target_obj, mark);
        JS_MarkObjectTemplate(rt, u->rect_tpl, mark);
        JS_MarkObjectTemplate(rt, u->table_tpl, mark);
    }
}

int uib_str(JSContext* ctx, JSValueConst v, const char* who, const char** s, size_t* n)
{
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs a string", who);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    if (!*s)
        return -1;
    if (*n > (1u << 26)) {
        JS_FreeCString(ctx, *s);
        JS_ThrowRangeError(ctx, "dyna:ui: %s is longer than 64 MiB", who);
        return -1;
    }
    return 0;
}

static void uib_bound(char* buf, size_t cap, double x)
{
    if (x == floor(x) && fabs(x) < 1e15)
        snprintf(buf, cap, "%.0f", x);
    else
        snprintf(buf, cap, "%g", x);
}

int uib_num(JSContext* ctx, JSValueConst v, const char* who, double lo, double hi, double* out)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s must be a number", who);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi)) {
        char lb[32], hb[32];
        uib_bound(lb, sizeof(lb), lo);
        uib_bound(hb, sizeof(hb), hi);
        JS_ThrowRangeError(ctx, "dyna:ui: %s must be a finite number in %s..%s", who, lb, hb);
        return -1;
    }
    *out = d;
    return 0;
}

int uib_opt_str(JSContext* ctx, JSValueConst opts, const char* key, const char* who, const char** s, size_t* n)
{
    JSValue v;
    int rc;
    *s = NULL;
    *n = 0;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_str(ctx, v, who, s, n);
    JS_FreeValue(ctx, v);
    return rc;
}

int uib_length(JSContext* ctx, JSValueConst v, int64_t* out)
{
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    double d;
    int rc;
    if (JS_IsException(l))
        return -1;
    rc = JS_ToFloat64(ctx, &d, l);
    JS_FreeValue(ctx, l);
    if (rc)
        return -1;
    if (!(d >= 0 && d <= 9007199254740991.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: a length that is not a whole number");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static void uib_density(dyn_ui_theme* t, int touch)
{
    t->min_target = touch ? 44 : 0;
    t->pad_x = touch ? 10 : 6;
    t->pad_y = touch ? 8 : 4;
    t->spacing = touch ? 8 : 4;
}

static int uib_insets_opt(JSContext* ctx, JSValueConst opts, float out[4])
{
    JSValue v;
    int64_t n;
    int k, rc = 0;
    if (uib_get_opt(ctx, opts, "insets", &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsArray(ctx, v) || uib_length(ctx, v, &n) || n != 4) {
        JS_FreeValue(ctx, v);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "dyna:ui: Context insets is [left, top, right, bottom]");
        return -1;
    }
    for (k = 0; k < 4 && !rc; k++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
        double d = 0;
        rc = JS_IsException(e) || uib_num(ctx, e, "an inset", 0, 4096, &d);
        JS_FreeValue(ctx, e);
        out[k] = (float)d;
    }
    JS_FreeValue(ctx, v);
    return rc ? -1 : 0;
}

static JSValue uib_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "width", "height", "scale", "insets", "touch", "target", "present", "clock", "clear" };
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    double w = 0, h = 0, scale = 1;
    int present = 1, touch = 0, has_scale = 0, has_insets = 0, k;
    uint32_t clear = 0;
    const char* clock_s = NULL;
    size_t clock_n = 0;
    float insets[4] = { 0, 0, 0, 0 };
    JSValue target = JS_UNDEFINED, tv, obj;
    uib* u;
    if (!JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "dyna:ui: new Context({ width, height }) or new Context({ target })");
    if (uib_check_opts(ctx, opts, "Context", keys, (int)countof(keys)) || uib_opt_num(ctx, opts, "width", "Context width", 1, 16384, &w)
        || uib_opt_num(ctx, opts, "height", "Context height", 1, 16384, &h) || uib_opt_num(ctx, opts, "scale", "Context scale", 0.25, 16, &scale)
        || uib_opt_bool(ctx, opts, "present", &present) || uib_opt_color(ctx, opts, "clear", "Context clear", &clear) || uib_insets_opt(ctx, opts, insets))
        return JS_EXCEPTION;
    if (uib_get_opt(ctx, opts, "scale", &tv))
        return JS_EXCEPTION;
    has_scale = !JS_IsUndefined(tv);
    JS_FreeValue(ctx, tv);
    if (uib_get_opt(ctx, opts, "insets", &tv))
        return JS_EXCEPTION;
    has_insets = !JS_IsUndefined(tv);
    JS_FreeValue(ctx, tv);
    if (uib_get_opt(ctx, opts, "touch", &tv))
        return JS_EXCEPTION;
    if (JS_IsString(tv)) {
        const char* ts = JS_ToCString(ctx, tv);
        int ok = ts && !strcmp(ts, "auto");
        if (ts)
            JS_FreeCString(ctx, ts);
        JS_FreeValue(ctx, tv);
        if (!ok)
            return JS_ThrowTypeError(ctx, "dyna:ui: Context touch is true, false or \"auto\"");
        touch = 2;
    } else if (!JS_IsUndefined(tv)) {
        touch = JS_ToBool(ctx, tv);
        JS_FreeValue(ctx, tv);
    }
    if (uib_opt_str(ctx, opts, "clock", "Context clock", &clock_s, &clock_n))
        return JS_EXCEPTION;
    if (clock_s && strcmp(clock_s, "manual") && strcmp(clock_s, "monotonic")) {
        JS_FreeCString(ctx, clock_s);
        return JS_ThrowTypeError(ctx, "dyna:ui: Context clock is \"monotonic\" or \"manual\"");
    }
    if (uib_get_opt(ctx, opts, "target", &target)) {
        if (clock_s)
            JS_FreeCString(ctx, clock_s);
        return JS_EXCEPTION;
    }
    u = dyn_nat_calloc(1, sizeof(*u));
    if (!u) {
        if (clock_s)
            JS_FreeCString(ctx, clock_s);
        JS_FreeValue(ctx, target);
        return JS_ThrowOutOfMemory(ctx);
    }
    u->manual_clock = clock_s && !strcmp(clock_s, "manual");
    if (clock_s)
        JS_FreeCString(ctx, clock_s);
    u->rt = JS_GetRuntime(ctx);
    u->target_obj = JS_UNDEFINED;
    {
        static const char* const rect_names[4] = { "x", "y", "width", "height" };
        static const char* const table_names[8] = { "selected", "sortColumn", "descending", "column", "activated", "edit", "committed", "cancelled" };
        u->dnd_payload = JS_UNDEFINED;
        u->rect_tpl = uib_template(ctx, rect_names, 4);
        u->table_tpl = uib_template(ctx, table_names, 8);
        if (!u->rect_tpl || !u->table_tpl) {
            uib_dispose(u);
            JS_FreeValue(ctx, target);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    u->present = present;
    u->touch_mode = touch;
    u->activated = -1;
    u->ui = dyn_ui_new();
    if (!u->ui || dyn_ui_font_add(u->ui, ui_proggy, sizeof(ui_proggy), 0) != 0 || dyn_ui_font_add(u->ui, ui_inter, sizeof(ui_inter), 0) != 1) {
        uib_dispose(u);
        JS_FreeValue(ctx, target);
        return u->ui ? JS_ThrowInternalError(ctx, "dyna:ui: a built-in font did not load") : JS_ThrowOutOfMemory(ctx);
    }
    if (touch == 1)
        uib_density(dyn_ui_theme_of(u->ui), 1);
    u->clear = clear ? clear : dyn_ui_theme_of(u->ui)->bg;
    if (JS_IsUndefined(target)) {
        if (!(w > 0) || !(h > 0)) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: a Context without a target needs { width, height }");
        }
        if (touch == 2) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: touch \"auto\" asks the target's platform; a Context without a target takes true or false");
        }
        u->lw = (float)w;
        u->lh = (float)h;
        u->scale = (float)scale;
        memcpy(u->soft_insets, insets, sizeof(insets));
        for (k = 0; k < 4; k++)
            u->insets_px[k] = insets[k] * (float)scale;
        u->w = (int)lround(w * scale);
        u->h = (int)lround(h * scale);
        if (u->w < 1 || u->h < 1 || u->w > 16384 || u->h > 16384) {
            uib_dispose(u);
            return JS_ThrowRangeError(ctx, "dyna:ui: width * scale and height * scale must be 1..16384 pixels");
        }
        dyn_ui_set_clipboard(u->ui, NULL, NULL, NULL);
    } else {
        u->target_obj = target;
        target = JS_UNDEFINED;
        if (w || h || has_scale || has_insets) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: a Context with a target takes its size, scale and insets from the target");
        }
        u->scale = 1;
#ifdef CONFIG_SDL3
        if (!u->be && !JS_HasException(ctx))
            u->be = dyn_uib_sdl_attach(ctx, u->target_obj, u->ui, &u->ops);
#endif
#ifdef CONFIG_RAYLIB
        if (!u->be && !JS_HasException(ctx))
            u->be = dyn_uib_raylib_attach(ctx, u->target_obj, u->ui, &u->ops);
#endif
        if (!u->be) {
            uib_dispose(u);
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "dyna:ui: Context target is a dyna:sdl Renderer or a dyna:raylib Window (this build has %s)",
#if defined(CONFIG_SDL3) && defined(CONFIG_RAYLIB)
                    "both"
#elif defined(CONFIG_SDL3)
                    "dyna:sdl only"
#elif defined(CONFIG_RAYLIB)
                    "dyna:raylib only"
#else
                    "neither; build with CONFIG_SDL3=y or CONFIG_RAYLIB=y"
#endif
                );
            return JS_EXCEPTION;
        }
    }
    if (u->ops && uib_view(ctx, u)) {
        uib_dispose(u);
        return JS_EXCEPTION;
    }
    obj = dyn_res_wrap(ctx, new_target, uib_cid, u, uib_dispose);
    return obj;
}

static void uib_strs_free(JSContext* ctx, uib* u)
{
    size_t i;
    for (i = 0; i < u->nstrs; i++)
        JS_FreeCString(ctx, u->strs[i]);
    u->nstrs = 0;
}

const char* uib_keep(JSContext* ctx, uib* u, JSValueConst v, size_t* n)
{
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: items must be strings");
        return NULL;
    }
    if (u->nstrs == u->cstrs) {
        size_t nc = u->cstrs ? u->cstrs * 2 : 64;
        char** p = dyn_nat_realloc(u->strs, nc * sizeof(char*));
        if (!p) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        u->strs = p;
        u->cstrs = nc;
    }
    s = JS_ToCStringLen(ctx, n, v);
    if (!s)
        return NULL;
    u->strs[u->nstrs++] = (char*)(uintptr_t)s;
    return s;
}

static JSValue uib_ui_error(JSContext* ctx, uib* u)
{
    return JS_ThrowRangeError(ctx, "dyna:ui: %s", dyn_ui_error_text(u->ui));
}

static JSValue uib_call(JSContext* ctx, JSValueConst fn, JSValueConst self)
{
    JSValueConst a[1];
    a[0] = self;
    return JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
}

int uib_run(JSContext* ctx, JSValueConst fn, JSValueConst self)
{
    JSValue r = uib_call(ctx, fn, self);
    if (JS_IsException(r))
        return -1;
    if (JS_IsObject(r) && JS_PromiseState(ctx, r) >= 0) {
        JS_FreeValue(ctx, r);
        JS_ThrowTypeError(ctx, "dyna:ui: a layout callback is synchronous; an async function would lay its widgets out after the frame ends");
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

static int uib_view(JSContext* ctx, uib* u)
{
    float ins[4];
    int k;
    if (u->ops) {
        dyn_uib_view v;
        memset(&v, 0, sizeof(v));
        if (u->ops->begin(u->be, ctx, u->target_obj, u->ui, &v))
            return -1;
        u->w = (int)v.w;
        u->h = (int)v.h;
        u->scale = v.scale >= 0.25f && v.scale <= 16 ? v.scale : 1;
        memcpy(ins, v.insets, sizeof(ins));
        if (u->touch_mode == 2 && !u->touch_done) {
            uib_density(dyn_ui_theme_of(u->ui), v.touch);
            u->touch_done = 1;
        }
    } else {
        for (k = 0; k < 4; k++)
            ins[k] = u->soft_insets[k] * u->scale;
    }
    if (u->w < 1 || u->h < 1 || u->w > 16384 || u->h > 16384) {
        JS_ThrowRangeError(ctx, "dyna:ui: the target is %dx%d pixels; frames are 1..16384 on each side", u->w, u->h);
        return -1;
    }
    for (k = 0; k < 4; k++) {
        float lim = (float)(k % 2 ? u->h : u->w);
        ins[k] = ins[k] > 0 ? (ins[k] < lim ? ins[k] : lim) : 0;
    }
    dyn_ui_set_scale(u->ui, u->scale);
    dyn_ui_set_insets(u->ui, ins[0], ins[1], ins[2], ins[3]);
    memcpy(u->insets_px, ins, sizeof(ins));
    return 0;
}

static JSValue uib_frame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    int changed = 0, rc, failed;
    DynResource* res;
    if (argc < 1 || uib_fn(ctx, argv[0], "frame(fn)"))
        return JS_EXCEPTION;
    res = dyn_res_get(ctx, this_val, uib_cid);
    u = res ? res->native : NULL;
    if (!u)
        return JS_EXCEPTION;
    dyn_res_hold(ctx, res);
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: frame() inside frame() is refused");
    if (!u->manual_clock)
        dyn_ui_set_time(u->ui, uib_now());
    if (uib_view(ctx, u))
        return JS_EXCEPTION;
    if (dyn_ui_w_begin(u->ui, (float)u->w, (float)u->h))
        return uib_ui_error(ctx, u);
    u->busy = 1;
    u->events = 0;
    u->activated = -1;
    failed = uib_run(ctx, argv[0], this_val);
    while (dyn_ui_depth(u->ui) > 2)
        dyn_ui_close(u->ui);
    rc = dyn_ui_w_end(u->ui, &changed);
    u->busy = 0;
    if (!dyn_ui_dragging(u->ui) && !JS_IsUndefined(u->dnd_payload)) {
        JS_FreeValue(ctx, u->dnd_payload);
        u->dnd_payload = JS_UNDEFINED;
    }
    uib_strs_free(ctx, u);
    u->frames++;
    if (failed)
        return JS_EXCEPTION;
    if (rc)
        return uib_ui_error(ctx, u);
    if (u->ops) {
        int drawn = u->ops->draw(u->be, ctx, u->target_obj, u->ui, changed, u->present, u->clear);
        if (drawn < 0)
            return JS_EXCEPTION;
    } else if (changed)
        u->raster_ok = 0;
    return JS_NewBool(ctx, changed);
}

static JSValue uib_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    double d = 0;
    int failed, isnum = argc > 0 && JS_IsNumber(argv[0]);
    if (argc < 2 || uib_fn(ctx, argv[1], "id(key, fn)"))
        return JS_EXCEPTION;
    if (isnum ? uib_num(ctx, argv[0], "id key", -9007199254740991.0, 9007199254740991.0, &d) : uib_str(ctx, argv[0], "id key", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "id");
    if (!u) {
        if (s)
            JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    dyn_ui_push_id(u->ui, isnum ? dyn_ui_hash(0x1D, &d, sizeof(d)) : dyn_ui_hash(0, s, n));
    if (s)
        JS_FreeCString(ctx, s);
    failed = uib_run(ctx, argv[1], this_val);
    dyn_ui_pop_id(u->ui);
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue uib_spacer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double w = 0, h = 0;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "spacer width", 0, 16777216, &w))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "spacer height", 0, 16777216, &h)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "spacer");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_spacer(u->ui, uib_px(u, w), uib_px(u, h));
    return JS_UNDEFINED;
}

static JSValue uib_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "color", "wrap" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    uint32_t color = 0;
    int wrap = 1;
    if (uib_check_opts(ctx, opts, "text", keys, 2) || uib_opt_color(ctx, opts, "color", "text color", &color) || uib_opt_bool(ctx, opts, "wrap", &wrap)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "text", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "text");
    if (u)
        dyn_ui_w_text(u->ui, s, n, color ? color : dyn_ui_theme_of(u->ui)->text, wrap);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_badge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t color = 0;
    if ((argc > 1 && !JS_IsUndefined(argv[1]) && uib_color(ctx, argv[1], "badge color", &color)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "badge", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "badge");
    if (u)
        dyn_ui_w_badge(u->ui, s, n, color);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

int uib_handle(JSContext* ctx, uib* u, JSValueConst v, uint32_t* out)
{
    double d;
    int w, h;
    if (uib_num(ctx, v, "image handle", 1, 4095, &d))
        return -1;
    if (d != floor(d) || dyn_ui_image_size(u->ui, (uint32_t)d, &w, &h)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", d);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue uib_progress(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double f, w = 0;
    if (uib_value(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "progress", &f)
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "progress width", 0, 16384, &w)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "progress");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_progress(u->ui, f == f ? f : 0, uib_px(u, w));
    return JS_UNDEFINED;
}

static JSValue uib_spinner(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double z = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "spinner size", 4, 512, &z))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "spinner");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_spinner(u->ui, uib_px(u, z));
    return JS_UNDEFINED;
}

static JSValue uib_icon_button(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t img;
    double z = 0;
    int r;
    if ((argc > 2 && !JS_IsUndefined(argv[2]) && uib_num(ctx, argv[2], "iconButton size", 1, 512, &z))
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "iconButton", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "iconButton");
    if (!u || uib_handle(ctx, u, argc > 1 ? argv[1] : JS_UNDEFINED, &img)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_icon_button(u->ui, s, n, img, uib_px(u, z));
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "toggle", "checkbox", "switch", "selectable" };
    uib* u;
    const char* s;
    size_t n;
    int on, r = 0;
    if (argc < 2 || !JS_IsBool(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, value) takes a boolean value", who[magic]);
    on = JS_ToBool(ctx, argv[1]);
    if (uib_str(ctx, argv[0], who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        r = dyn_ui_w_toggle(u->ui, s, n, on);
        break;
    case 1:
        r = dyn_ui_w_checkbox(u->ui, s, n, on);
        break;
    case 2:
        r = dyn_ui_w_switch(u->ui, s, n, on);
        break;
    default:
        r = dyn_ui_w_selectable(u->ui, s, n, on);
        break;
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_slider(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "decimals", "vertical", "length" };
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, lo, hi, step = 0, len = 0, r;
    int dec = 2, vert = 0;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "dyna:ui: slider(label, value, min, max, opts?)");
    if (uib_value(ctx, argv[1], "slider value", &v) || uib_num(ctx, argv[2], "slider min", -1e300, 1e300, &lo)
        || uib_num(ctx, argv[3], "slider max", -1e300, 1e300, &hi) || uib_check_opts(ctx, opts, "slider", keys, 4)
        || uib_opt_num(ctx, opts, "step", "slider step", 0, 1e300, &step) || uib_decimals(ctx, opts, &dec) || uib_opt_bool(ctx, opts, "vertical", &vert)
        || uib_opt_num(ctx, opts, "length", "slider length", 0, 16384, &len))
        return JS_EXCEPTION;
    if (!(lo < hi))
        return JS_ThrowRangeError(ctx, "dyna:ui: slider min must be below max");
    if (uib_str(ctx, argv[0], "slider", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "slider");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_slider(u->ui, s, n, v, lo, hi, step, dec, vert, uib_px(u, len));
    JS_FreeCString(ctx, s);
    return r == v ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, r);
}

static const char* uib_item_fn(void* ud, int i, size_t* n)
{
    uib* u = ud;
    JSContext* ctx = u->cb_ctx;
    JSValue e;
    const char* s;
    if (u->cb_failed)
        return NULL;
    e = JS_GetPropertyUint32(ctx, u->cb_arr, (uint32_t)i);
    if (JS_IsException(e)) {
        u->cb_failed = 1;
        return NULL;
    }
    s = uib_keep(ctx, u, e, n);
    JS_FreeValue(ctx, e);
    if (!s)
        u->cb_failed = 1;
    return s;
}

static JSValue uib_list(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "rows", "follow" };
    JSValueConst opts = argc > (magic ? 2 : 3) ? argv[magic ? 2 : 3] : JS_UNDEFINED;
    const char* who = magic ? "log" : "list";
    uib* u;
    const char* s;
    size_t n;
    int64_t count;
    int sel = -1, follow = magic, r, act;
    double rows = 8;
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items%s, opts?) takes an array of strings", who, magic ? "" : ", selected");
    if ((!magic && (argc < 3 || uib_index(ctx, argv[2], "list selected", &sel))) || uib_check_opts(ctx, opts, who, keys, 2)
        || uib_opt_num(ctx, opts, "rows", "rows", 1, 1000, &rows) || uib_opt_bool(ctx, opts, "follow", &follow) || uib_length(ctx, argv[1], &count))
        return JS_EXCEPTION;
    if (count > 1000000)
        return JS_ThrowRangeError(ctx, "dyna:ui: %s holds at most 1000000 rows", who);
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    u->cb_ctx = ctx;
    u->cb_arr = argv[1];
    u->cb_failed = 0;
    r = dyn_ui_w_list(u->ui, s, n, (int)count, uib_item_fn, u, sel, (int)rows, !magic, follow, &act);
    JS_FreeCString(ctx, s);
    u->cb_arr = JS_UNDEFINED;
    if (u->cb_failed)
        return JS_EXCEPTION;
    u->activated = act;
    return magic ? JS_UNDEFINED : JS_NewInt32(ctx, r);
}

static JSValue uib_color_w(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t c, r;
    if (argc < 2 || uib_color(ctx, argv[1], "color value", &c) || uib_str(ctx, argv[0], "color", &s, &n))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: color(label, 0xRRGGBBAA)") : JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "color");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_color(u->ui, s, n, c);
    JS_FreeCString(ctx, s);
    return r == c ? JS_DupValue(ctx, argv[1]) : JS_NewUint32(ctx, r);
}

static JSValue uib_window(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "x", "y", "width", "height", "closable", "collapsible", "resizable", "fixed" };
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double x = 40, y = 40, w = 300, h = 200;
    int cl = 0, co = 1, re = 1, fx = 0, flags, closed = 0, failed = 0;
    dyn_ui_window_frame fr;
    if (uib_fn(ctx, fn, "window") || uib_check_opts(ctx, opts, "window", keys, (int)countof(keys)) || uib_opt_num(ctx, opts, "x", "x", -16384, 16384, &x)
        || uib_opt_num(ctx, opts, "y", "y", -16384, 16384, &y) || uib_opt_num(ctx, opts, "width", "width", 80, 16384, &w)
        || uib_opt_num(ctx, opts, "height", "height", 20, 16384, &h) || uib_opt_bool(ctx, opts, "closable", &cl)
        || uib_opt_bool(ctx, opts, "collapsible", &co) || uib_opt_bool(ctx, opts, "resizable", &re) || uib_opt_bool(ctx, opts, "fixed", &fx))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "window", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "window");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    flags = (cl ? DYN_UI_WIN_CLOSABLE : 0) | (co ? DYN_UI_WIN_COLLAPSIBLE : 0) | (re ? DYN_UI_WIN_RESIZABLE : 0) | (fx ? DYN_UI_WIN_FIXED : 0);
    if (dyn_ui_w_window_begin(u->ui, s, n, uib_px(u, x), uib_px(u, y), uib_px(u, w), uib_px(u, h), flags, &closed, &fr)) {
        dyn_ui_push_id(u->ui, fr.id);
        failed = uib_run(ctx, fn, this_val);
        dyn_ui_pop_id(u->ui);
    }
    dyn_ui_w_window_end(u->ui, &fr);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewBool(ctx, !closed);
}

static JSValue uib_collapsing(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    int open = 0, r;
    if ((argc > 1 && uib_bool(ctx, argv[1], &open)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "collapsing", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "collapsing");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_collapsing(u->ui, s, n, open);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_tree(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "open", "leaf" };
    JSValueConst opts = JS_UNDEFINED, fn = JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    int open = 0, leaf = 0, r, failed = 0, k;
    for (k = 1; k < argc && k < 3; k++) {
        if (JS_IsFunction(ctx, argv[k]))
            fn = argv[k];
        else if (!JS_IsUndefined(argv[k]))
            opts = argv[k];
    }
    if (uib_check_opts(ctx, opts, "tree", keys, 2) || uib_opt_bool(ctx, opts, "open", &open) || uib_opt_bool(ctx, opts, "leaf", &leaf)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "tree", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "tree");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_tree_begin(u->ui, s, n, open, leaf || JS_IsUndefined(fn));
    JS_FreeCString(ctx, s);
    if (r) {
        failed = uib_run(ctx, fn, this_val);
        dyn_ui_w_tree_end(u->ui);
    }
    return failed ? JS_EXCEPTION : JS_NewBool(ctx, r);
}

static JSValue uib_menu_item(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "shortcut", "checked", "enabled" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char *s, *key = NULL;
    size_t n, kn = 0;
    int checked = -1, enabled = 1, r;
    JSValue cv;
    if (uib_check_opts(ctx, opts, "menuItem", keys, 3) || uib_opt_bool(ctx, opts, "enabled", &enabled) || uib_get_opt(ctx, opts, "checked", &cv))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(cv)) {
        checked = JS_ToBool(ctx, cv);
        JS_FreeValue(ctx, cv);
    }
    if (uib_opt_str(ctx, opts, "shortcut", "shortcut", &key, &kn))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "menuItem", &s, &n)) {
        if (key)
            JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "menuItem");
    r = u ? dyn_ui_w_menu_item(u->ui, s, n, key, kn, checked, enabled) : 0;
    JS_FreeCString(ctx, s);
    if (key)
        JS_FreeCString(ctx, key);
    return u ? JS_NewBool(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_split(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "vertical" };
    uib* u;
    const char* s;
    size_t n;
    double ratio;
    int vert = 0, failed;
    float r;
    dyn_ui_split_frame fr;
    JSValueConst opts = argc > 4 ? argv[2] : JS_UNDEFINED, fa = argv[argc > 4 ? 3 : 2], fb = argv[argc > 4 ? 4 : 3];
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "dyna:ui: split(label, ratio, opts?, first, second)");
    if (uib_num(ctx, argv[1], "split ratio", 0, 1, &ratio) || uib_check_opts(ctx, opts, "split", keys, 1) || uib_opt_bool(ctx, opts, "vertical", &vert)
        || uib_fn(ctx, fa, "split first") || uib_fn(ctx, fb, "split second") || uib_str(ctx, argv[0], "split", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "split");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_split_begin(u->ui, s, n, (float)ratio, vert, &fr);
    JS_FreeCString(ctx, s);
    failed = uib_run(ctx, fa, this_val);
    dyn_ui_w_split_next(u->ui, &fr);
    if (!failed)
        failed = uib_run(ctx, fb, this_val);
    dyn_ui_w_split_end(u->ui);
    if (failed)
        return JS_EXCEPTION;
    return (float)ratio == r ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, (double)r);
}

static const struct {
    const char* name;
    int key;
} uib_keys[] = {
    { "left", DYN_UI_KEY_LEFT },
    { "right", DYN_UI_KEY_RIGHT },
    { "up", DYN_UI_KEY_UP },
    { "down", DYN_UI_KEY_DOWN },
    { "home", DYN_UI_KEY_HOME },
    { "end", DYN_UI_KEY_END },
    { "backspace", DYN_UI_KEY_BACKSPACE },
    { "delete", DYN_UI_KEY_DELETE },
    { "return", DYN_UI_KEY_RETURN },
    { "tab", DYN_UI_KEY_TAB },
    { "escape", DYN_UI_KEY_ESCAPE },
    { "pageup", DYN_UI_KEY_PAGEUP },
    { "pagedown", DYN_UI_KEY_PAGEDOWN },
    { "a", DYN_UI_KEY_A },
    { "c", DYN_UI_KEY_C },
    { "v", DYN_UI_KEY_V },
    { "x", DYN_UI_KEY_X },
    { "y", DYN_UI_KEY_Y },
    { "z", DYN_UI_KEY_Z },
};

static int uib_mods(JSContext* ctx, JSValueConst v, int* out)
{
    static const struct {
        const char* name;
        int bit;
    } m[] = { { "shift", DYN_UI_MOD_SHIFT }, { "ctrl", DYN_UI_MOD_CTRL }, { "alt", DYN_UI_MOD_ALT }, { "super", DYN_UI_MOD_SUPER } };
    static const char* const keys[] = { "shift", "ctrl", "alt", "super" };
    size_t i;
    *out = 0;
    if (JS_IsUndefined(v))
        return 0;
    if (uib_check_opts(ctx, v, "key modifiers", keys, 4))
        return -1;
    for (i = 0; i < countof(m); i++) {
        int b = 0;
        if (uib_opt_bool(ctx, v, m[i].name, &b))
            return -1;
        if (b)
            *out |= m[i].bit;
    }
    return 0;
}

static JSValue uib_key(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, i;
    int key = 0, down = 1, mods;
    if ((argc > 1 && uib_bool(ctx, argv[1], &down)) || uib_mods(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &mods)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "key", &s, &n))
        return JS_EXCEPTION;
    for (i = 0; i < countof(uib_keys); i++)
        if (!strcmp(s, uib_keys[i].name))
            key = uib_keys[i].key;
    if (!key) {
        JSValue e = JS_ThrowRangeError(ctx, "dyna:ui: key '%s' is not one of left right up down home end backspace delete return tab escape pageup pagedown a c v x y z", s);
        JS_FreeCString(ctx, s);
        return e;
    }
    JS_FreeCString(ctx, s);
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_input_key(u->ui, key, down, mods);
    return JS_UNDEFINED;
}

static JSValue uib_type(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "type", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u)
        dyn_ui_input_text(u->ui, s, n);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static const struct {
    const char* name;
    size_t off;
    int color;
    double lo, hi;
} uib_theme_fields[] = {
#define UIB_TF(n, f, c, lo, hi) { n, offsetof(dyn_ui_theme, f), c, lo, hi }
    UIB_TF("fontSize", font_px, 0, 1, 512),
    UIB_TF("padX", pad_x, 0, 0, 256),
    UIB_TF("padY", pad_y, 0, 0, 256),
    UIB_TF("spacing", spacing, 0, 0, 256),
    UIB_TF("windowPad", window_pad, 0, 0, 256),
    UIB_TF("indent", indent, 0, 0, 256),
    UIB_TF("radius", radius, 0, 0, 256),
    UIB_TF("borderWidth", border_w, 0, 0, 64),
    UIB_TF("labelWidth", label_w, 0, 0, 4096),
    UIB_TF("scrollbarWidth", scrollbar_w, 0, 0, 64),
    UIB_TF("minTarget", min_target, 0, 0, 256),
    UIB_TF("dragSlop", drag_slop, 0, 0, 256),
    UIB_TF("longPress", long_press, 0, 0.05, 10),
    UIB_TF("textScale", text_scale, 0, 0.25, 8),
    UIB_TF("text", text, 1, 0, 0),
    UIB_TF("textDim", text_dim, 1, 0, 0),
    UIB_TF("onAccent", on_accent, 1, 0, 0),
    UIB_TF("bg", bg, 1, 0, 0),
    UIB_TF("panel", panel, 1, 0, 0),
    UIB_TF("frame", frame, 1, 0, 0),
    UIB_TF("frameHot", frame_hot, 1, 0, 0),
    UIB_TF("frameActive", frame_active, 1, 0, 0),
    UIB_TF("button", button, 1, 0, 0),
    UIB_TF("buttonHot", button_hot, 1, 0, 0),
    UIB_TF("buttonActive", button_active, 1, 0, 0),
    UIB_TF("accent", accent, 1, 0, 0),
    UIB_TF("accentHot", accent_hot, 1, 0, 0),
    UIB_TF("border", border, 1, 0, 0),
    UIB_TF("title", title, 1, 0, 0),
    UIB_TF("titleActive", title_active, 1, 0, 0),
    UIB_TF("selection", selection, 1, 0, 0),
    UIB_TF("popup", popup, 1, 0, 0),
    UIB_TF("shade", shade, 1, 0, 0),
    UIB_TF("tooltip", tooltip, 1, 0, 0),
    UIB_TF("link", link, 1, 0, 0),
    UIB_TF("scrollbar", scrollbar, 1, 0, 0),
    UIB_TF("scrollbarHot", scrollbar_hot, 1, 0, 0),
    UIB_TF("error", error, 1, 0, 0),
#undef UIB_TF
};

static JSValue uib_theme(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    dyn_ui_theme next;
    size_t i;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char* keys[countof(uib_theme_fields) + 1];
        double font = -2;
        for (i = 0; i < countof(uib_theme_fields); i++)
            keys[i] = uib_theme_fields[i].name;
        keys[i] = "font";
        if (uib_check_opts(ctx, argv[0], "theme", keys, (int)countof(keys)))
            return JS_EXCEPTION;
        u = uib_get(ctx, this_val);
        if (!u)
            return JS_EXCEPTION;
        next = *dyn_ui_theme_of(u->ui);
        for (i = 0; i < countof(uib_theme_fields); i++) {
            char* f = (char*)&next + uib_theme_fields[i].off;
            if (uib_theme_fields[i].color) {
                uint32_t c = *(uint32_t*)(void*)f;
                if (uib_opt_color(ctx, argv[0], uib_theme_fields[i].name, uib_theme_fields[i].name, &c))
                    return JS_EXCEPTION;
                memcpy(f, &c, sizeof(c));
            } else {
                double d = (double)*(float*)(void*)f;
                if (uib_opt_num(ctx, argv[0], uib_theme_fields[i].name, uib_theme_fields[i].name, uib_theme_fields[i].lo, uib_theme_fields[i].hi, &d))
                    return JS_EXCEPTION;
                *(float*)(void*)f = (float)d;
            }
        }
        if (uib_opt_num(ctx, argv[0], "font", "theme font", -1, 63, &font))
            return JS_EXCEPTION;
        u = uib_get(ctx, this_val);
        if (!u)
            return JS_EXCEPTION;
        if (u->busy)
            return JS_ThrowTypeError(ctx, "dyna:ui: theme() changes take effect between frames; call it outside frame()");
        if (font >= -1) {
            if (font != floor(font) || (int)font >= dyn_ui_font_count(u->ui))
                return JS_ThrowRangeError(ctx, "dyna:ui: theme font %g is not a font index from loadFont()", font);
            next.font = (int)font;
        }
        *dyn_ui_theme_of(u->ui) = next;
        return JS_UNDEFINED;
    }
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    {
        JSValue o = JS_NewObject(ctx);
        const dyn_ui_theme* t = dyn_ui_theme_of(u->ui);
        if (JS_IsException(o))
            return o;
        for (i = 0; i < countof(uib_theme_fields); i++) {
            const char* f = (const char*)t + uib_theme_fields[i].off;
            JSValue v = uib_theme_fields[i].color ? JS_NewUint32(ctx, *(const uint32_t*)(const void*)f) : JS_NewFloat64(ctx, (double)*(const float*)(const void*)f);
            if (JS_DefinePropertyValueStr(ctx, o, uib_theme_fields[i].name, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
        }
        if (JS_DefinePropertyValueStr(ctx, o, "font", JS_NewInt32(ctx, t->font), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }
}

int uib_bytes_rw(JSContext* ctx, JSValueConst v, const char* who, uint8_t** p, size_t* n)
{
    size_t off, len, bpe, blen;
    JSValue buf;
    uint8_t* base;
    if (!uib_u8_cid || JS_GetClassID(v) != uib_u8_cid) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s takes a Uint8Array", who);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &blen, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s: the Uint8Array's buffer is detached", who);
        return -1;
    }
    if (off > blen || len > blen - off) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s: the view lies outside its buffer", who);
        return -1;
    }
    *p = base + off;
    *n = len;
    return 0;
}

static JSValue uib_load_font(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const uint8_t* p = NULL;
    size_t n = 0;
    uint8_t* owned = NULL;
    int idx;
    JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsString(src)) {
        size_t pl;
        const char* path = JS_ToCStringLen(ctx, &pl, src);
        FILE* f;
        long sz;
        if (!path)
            return JS_EXCEPTION;
        if (strlen(path) != pl) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: a path holds no NUL character");
        }
        f = fopen(path, "rb");
        if (!f) {
            JSValue e = JS_ThrowTypeError(ctx, "dyna:ui: loadFont: cannot open %s", path);
            JS_FreeCString(ctx, path);
            return e;
        }
        JS_FreeCString(ctx, path);
        if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || sz > (64L << 20) || fseek(f, 0, SEEK_SET)) {
            fclose(f);
            return JS_ThrowRangeError(ctx, "dyna:ui: loadFont: the file is unreadable or over 64 MiB");
        }
        owned = dyn_nat_malloc(sz ? (size_t)sz : 1);
        if (!owned) {
            fclose(f);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (fread(owned, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f);
            dyn_nat_free(owned);
            return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: short read");
        }
        fclose(f);
        p = owned;
        n = (size_t)sz;
    } else if (uib_bytes(ctx, src, "loadFont", &p, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u) {
        dyn_nat_free(owned);
        return JS_EXCEPTION;
    }
    if (owned == NULL && uib_bytes(ctx, src, "loadFont", &p, &n))
        return JS_EXCEPTION;
    idx = dyn_ui_font_add(u->ui, p, n, 1);
    dyn_nat_free(owned);
    if (idx < 0)
        return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: not a TrueType/OpenType font whose tables lie inside the file (or 64 fonts are loaded)");
    return JS_NewInt32(ctx, idx);
}

static JSValue uib_remove_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double hd;
    int iw, ih;
    if (uib_num(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "image handle", 1, 4095, &hd))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: removeImage() inside frame() is refused; the frame may still draw it");
    if (hd != floor(hd) || dyn_ui_image_size(u->ui, (uint32_t)hd, &iw, &ih))
        return JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", hd);
    dyn_ui_image_remove(u->ui, (uint32_t)hd);
    return JS_UNDEFINED;
}

JSValue uib_rect_obj(JSContext* ctx, uib* u, const dyn_ui_rect* r)
{
    JSValue o;
    double s = (double)u->scale;
    if (u->rect_tpl) {
        JSValue v[4];
        v[0] = JS_NewFloat64(ctx, (double)r->x / s);
        v[1] = JS_NewFloat64(ctx, (double)r->y / s);
        v[2] = JS_NewFloat64(ctx, (double)r->w / s);
        v[3] = JS_NewFloat64(ctx, (double)r->h / s);
        return uib_fill(ctx, u->rect_tpl, 4, v);
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "x", JS_NewFloat64(ctx, (double)r->x / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "y", JS_NewFloat64(ctx, (double)r->y / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewFloat64(ctx, (double)r->w / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewFloat64(ctx, (double)r->h / s), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    uib* u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, (double)u->w / (double)u->scale);
    case 1:
        return JS_NewFloat64(ctx, (double)u->h / (double)u->scale);
    case 2: {
        double t = dyn_ui_next_wake(u->ui) - dyn_ui_time(u->ui);
        return JS_NewFloat64(ctx, t < 0 ? 0 : t);
    }
    case 3:
        return JS_NewBool(ctx, (u->events & DYN_UI_EDIT_SUBMITTED) != 0);
    case 4:
        return JS_NewInt32(ctx, u->activated);
    case 5:
        return JS_NewBool(ctx, (u->events & DYN_UI_EDIT_FOCUSED) != 0);
    case 6:
        return JS_NewInt32(ctx, u->w);
    case 7:
        return JS_NewInt32(ctx, u->h);
    case 8:
        return JS_NewFloat64(ctx, (double)u->scale);
    case 9: {
        double lw = (double)u->w / (double)u->scale;
        return JS_NewString(ctx, lw < 600 ? "compact" : lw < 840 ? "medium"
                                                                 : "expanded");
    }
    case 10: {
        dyn_ui_rect r;
        if (!dyn_ui_text_input(u->ui, &r))
            return JS_NULL;
        return uib_rect_obj(ctx, u, &r);
    }
    case 12:
        return dyn_ui_dragging(u->ui) && !JS_IsUndefined(u->dnd_payload) ? JS_DupValue(ctx, u->dnd_payload) : JS_NULL;
    default: {
        JSValue a = JS_NewArray(ctx);
        int k;
        for (k = 0; k < 4 && !JS_IsException(a); k++)
            if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)k, JS_NewFloat64(ctx, (double)u->insets_px[k] / (double)u->scale), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
        return a;
    }
    }
}

static JSValue uib_last_rect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_rect r;
    double item = -2, col = -1;
    if (!u)
        return JS_EXCEPTION;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "lastRect item", -1, 2147483647.0, &item))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "lastRect column", 0, UIB_MAX_COLS, &col)))
        return JS_EXCEPTION;
    if (item != floor(item) || col != floor(col))
        return JS_ThrowRangeError(ctx, "dyna:ui: lastRect item and column are integers");
    if (col >= 0 && item < -1)
        return JS_ThrowTypeError(ctx, "dyna:ui: lastRect(item, column) needs the item (row) too");
    if (item == -1 && col < 0)
        return JS_ThrowRangeError(ctx, "dyna:ui: lastRect item -1 is a table header and needs its column");
    if (item >= -1 ? !dyn_ui_last_item_box(u->ui, (int)item, (int)col, &r) : !dyn_ui_last_box(u->ui, &r))
        return JS_NULL;
    return uib_rect_obj(ctx, u, &r);
}

static JSValue uib_chip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    int on, r;
    if (argc < 2 || !JS_IsBool(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: chip(label, selected) takes a boolean");
    on = JS_ToBool(ctx, argv[1]);
    if (uib_str(ctx, argv[0], "chip", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "chip");
    r = u ? dyn_ui_w_chip(u->ui, s, n, on) : 0;
    JS_FreeCString(ctx, s);
    return u ? JS_NewBool(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_card(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "width" };
    const char* who = magic ? "appBar" : "card";
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double w = 0;
    int failed;
    if (uib_fn(ctx, fn, who) || (!magic && (uib_check_opts(ctx, opts, "card", keys, 1) || uib_opt_num(ctx, opts, "width", "card width", 0, 16384, &w)))
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (magic)
        dyn_ui_w_app_bar_begin(u->ui, s, n);
    else
        dyn_ui_w_card_begin(u->ui, s, n, uib_px(u, w));
    JS_FreeCString(ctx, s);
    failed = uib_run(ctx, fn, this_val);
    if (magic)
        dyn_ui_w_bar_end(u->ui);
    else
        dyn_ui_w_card_end(u->ui);
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue uib_scaffold(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    uint32_t* icons = NULL;
    int sel, r, count = 0, failed;
    if (argc < 4 || uib_index(ctx, argv[2], "scaffold", &sel) || uib_fn(ctx, argv[3], "scaffold"))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: scaffold(label, items, selected, fn)");
    if (uib_str(ctx, argv[0], "scaffold", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "scaffold");
    if (!u || uib_nav_items(ctx, u, argv[1], "scaffold", &items, &lens, &icons, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (sel >= count)
        sel = -1;
    dyn_ui_w_scaffold_begin(u->ui, s, n, items, lens, icons, count, sel);
    failed = uib_run(ctx, argv[3], this_val);
    r = dyn_ui_w_scaffold_end(u->ui, s, n, items, lens, icons, count, sel);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewInt32(ctx, r);
}

static JSValue uib_open_overlay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "openDrawer", "openSheet" };
    uib* u;
    const char* s;
    size_t n;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (u)
        dyn_ui_w_open_overlay(u->ui, s, n);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_toast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "duration" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double d = 3;
    if (uib_check_opts(ctx, opts, "toast", keys, 1) || uib_opt_num(ctx, opts, "duration", "toast duration", 0.1, 3600, &d)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "toast", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u && !u->manual_clock && !u->busy)
        dyn_ui_set_time(u->ui, uib_now());
    if (u && dyn_ui_w_toast(u->ui, s, n, d)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_fab(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "icon" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    uint32_t icon = 0;
    JSValue iv = JS_UNDEFINED;
    int r;
    if (uib_check_opts(ctx, opts, "fab", keys, 1) || uib_get_opt(ctx, opts, "icon", &iv) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "fab", &s, &n)) {
        JS_FreeValue(ctx, iv);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "fab");
    if (!u || (!JS_IsUndefined(iv) && uib_handle(ctx, u, iv, &icon))) {
        JS_FreeValue(ctx, iv);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, iv);
    r = dyn_ui_w_fab(u->ui, s, n, icon);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_seq(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "stepper" : "breadcrumbs";
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    int count, cur = 0, r;
    if (argc < 2 || (magic && (argc < 3 || uib_index(ctx, argv[2], "stepper current", &cur))))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items%s)", who, magic ? ", current" : "");
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u || uib_items(ctx, u, argv[1], who, &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = magic ? dyn_ui_w_stepper(u->ui, s, n, items, lens, count, cur) : dyn_ui_w_breadcrumbs(u->ui, s, n, items, lens, count);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

static JSValue uib_pagination(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    double page, pages;
    int r;
    if (argc < 3 || uib_num(ctx, argv[1], "pagination page", 0, 1e9, &page) || uib_num(ctx, argv[2], "pagination pages", 1, 1e9, &pages))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: pagination(label, page, pages)");
    if (page != floor(page) || pages != floor(pages))
        return JS_ThrowRangeError(ctx, "dyna:ui: pagination page and pages are integers");
    if (uib_str(ctx, argv[0], "pagination", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "pagination");
    r = u ? dyn_ui_w_pagination(u->ui, s, n, (int)page, (int)pages) : 0;
    JS_FreeCString(ctx, s);
    return u ? JS_NewInt32(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_rating(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "max" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, max = 5;
    int r;
    if (argc < 2 || uib_num(ctx, argv[1], "rating value", 0, 20, &v) || uib_check_opts(ctx, opts, "rating", keys, 1)
        || uib_opt_num(ctx, opts, "max", "rating max", 1, 20, &max))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: rating(label, value, opts?)");
    if (v != floor(v) || max != floor(max))
        return JS_ThrowRangeError(ctx, "dyna:ui: rating value and max are integers");
    if (uib_str(ctx, argv[0], "rating", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "rating");
    r = u ? dyn_ui_w_rating(u->ui, s, n, (int)v, (int)max) : 0;
    JS_FreeCString(ctx, s);
    return !u ? JS_EXCEPTION : r == (int)v ? JS_DupValue(ctx, argv[1])
                                           : JS_NewInt32(ctx, r);
}

static JSValue uib_avatar(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "image", "size" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double z = 0;
    uint32_t img = 0;
    JSValue iv = JS_UNDEFINED;
    if (uib_check_opts(ctx, opts, "avatar", keys, 2) || uib_opt_num(ctx, opts, "size", "avatar size", 4, 1024, &z) || uib_get_opt(ctx, opts, "image", &iv)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "avatar", &s, &n)) {
        JS_FreeValue(ctx, iv);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "avatar");
    if (!u || (!JS_IsUndefined(iv) && uib_handle(ctx, u, iv, &img))) {
        JS_FreeValue(ctx, iv);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, iv);
    dyn_ui_w_avatar(u->ui, s, n, img, uib_px(u, z));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue uib_skeleton(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double w = 0, h = 0;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "skeleton width", 0, 16384, &w))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "skeleton height", 0, 16384, &h)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "skeleton");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_skeleton(u->ui, uib_px(u, w), uib_px(u, h));
    return JS_UNDEFINED;
}

static JSValue uib_composition(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    double cur = 0;
    if ((argc > 1 && uib_num(ctx, argv[1], "composition cursor", 0, 4096, &cur)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "composition", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u)
        dyn_ui_input_composition(u->ui, s, n, (int)cur);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_resize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "scale", "insets" };
    uib* u;
    double w, h, scale = -1;
    float ins[4] = { 0, 0, 0, 0 };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue iv;
    int has_ins, k;
    if (argc < 2 || uib_num(ctx, argv[0], "width", 1, 16384, &w) || uib_num(ctx, argv[1], "height", 1, 16384, &h) || uib_check_opts(ctx, opts, "resize", keys, 2)
        || uib_opt_num(ctx, opts, "scale", "scale", 0.25, 16, &scale) || uib_insets_opt(ctx, opts, ins) || uib_get_opt(ctx, opts, "insets", &iv))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: resize(width, height, { scale?, insets? })");
    has_ins = !JS_IsUndefined(iv);
    JS_FreeValue(ctx, iv);
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->ops)
        return JS_ThrowTypeError(ctx, "dyna:ui: resize() is for a Context without a target; a window's size comes from the window");
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: resize() inside frame() is refused");
    if (scale < 0)
        scale = (double)u->scale;
    if (lround(w * scale) < 1 || lround(h * scale) < 1 || lround(w * scale) > 16384 || lround(h * scale) > 16384)
        return JS_ThrowRangeError(ctx, "dyna:ui: width * scale and height * scale must be 1..16384 pixels");
    u->lw = (float)w;
    u->lh = (float)h;
    u->scale = (float)scale;
    u->w = (int)lround(w * scale);
    u->h = (int)lround(h * scale);
    if (has_ins)
        memcpy(u->soft_insets, ins, sizeof(ins));
    for (k = 0; k < 4; k++)
        u->insets_px[k] = u->soft_insets[k] * u->scale;
    dyn_nat_free(u->px);
    u->px = NULL;
    u->raster_ok = 0;
    return JS_UNDEFINED;
}

static JSValue uib_idle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double t = 1e9, wake;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "idle timeout", 0, 1e9, &t))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: idle() inside frame() is refused");
    wake = dyn_ui_next_wake(u->ui) - (u->manual_clock ? dyn_ui_time(u->ui) : uib_now());
    if (wake < t)
        t = wake < 0 ? 0 : wake;
    if (u->ops)
        u->ops->idle(u->be, t);
    return JS_NewFloat64(ctx, t);
}

static JSValue uib_atlas_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_atlas_view a;
    JSValue o, px;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    a = dyn_ui_atlas(u->ui);
    px = dyn_nat_u8_copy(ctx, a.pixels, (size_t)a.width * (size_t)a.height);
    if (JS_IsException(px))
        return px;
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, a.width), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, a.height), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "version", JS_NewUint32(ctx, a.version), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "alpha", px, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_stats_t s;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_stats(u->ui, &s);
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "elements", JS_NewUint32(ctx, s.elements), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "textBytes", JS_NewUint32(ctx, s.text_bytes), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "commands", JS_NewUint32(ctx, s.commands), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "glyphQuads", JS_NewUint32(ctx, s.quads), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "vertices", JS_NewUint32(ctx, s.vertices), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "hitBoxes", JS_NewUint32(ctx, s.hits), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "textRuns", JS_NewUint32(ctx, s.runs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "runRecords", JS_NewUint32(ctx, s.run_records), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "glyphs", JS_NewUint32(ctx, s.glyphs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "atlasHeight", JS_NewUint32(ctx, s.atlas_height), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "frames", JS_NewFloat64(ctx, (double)s.frames), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "reusedFrames", JS_NewFloat64(ctx, (double)s.reused), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_drop_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_in_frame(ctx, this_val, "dropTarget");
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    if (!dyn_ui_has_last(u->ui))
        return JS_ThrowTypeError(ctx, "dyna:ui: dropTarget() marks the widget drawn before it, and there is none in this frame");
    if (dyn_ui_w_drop_target(u->ui) == 2 && !JS_IsUndefined(u->dnd_payload))
        return JS_DupValue(ctx, u->dnd_payload);
    return JS_NULL;
}

static const JSCFunctionListEntry uib_proto[] = {
    JS_CFUNC_DEF("frame", 1, uib_frame),
    JS_CFUNC_DEF("pixels", 0, uib_pixels),
    JS_CFUNC_DEF("theme", 1, uib_theme),
    JS_CFUNC_DEF("loadFont", 1, uib_load_font),
    JS_CFUNC_MAGIC_DEF("addImage", 3, uib_add_image, 0),
    JS_CFUNC_MAGIC_DEF("updateImage", 2, uib_add_image, 1),
    JS_CFUNC_DEF("removeImage", 1, uib_remove_image),
    JS_CFUNC_DEF("pointer", 1, uib_pointer),
    JS_CFUNC_DEF("key", 3, uib_key),
    JS_CFUNC_DEF("type", 1, uib_type),
    JS_CFUNC_DEF("setTime", 1, uib_set_time),
    JS_CGETSET_MAGIC_DEF("width", uib_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", uib_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("wakeIn", uib_getter, NULL, 2),
    JS_CGETSET_MAGIC_DEF("submitted", uib_getter, NULL, 3),
    JS_CGETSET_MAGIC_DEF("activated", uib_getter, NULL, 4),
    JS_CGETSET_MAGIC_DEF("editing", uib_getter, NULL, 5),
    JS_CGETSET_MAGIC_DEF("pixelWidth", uib_getter, NULL, 6),
    JS_CGETSET_MAGIC_DEF("pixelHeight", uib_getter, NULL, 7),
    JS_CGETSET_MAGIC_DEF("scale", uib_getter, NULL, 8),
    JS_CGETSET_MAGIC_DEF("sizeClass", uib_getter, NULL, 9),
    JS_CGETSET_MAGIC_DEF("textInput", uib_getter, NULL, 10),
    JS_CGETSET_MAGIC_DEF("insets", uib_getter, NULL, 11),
    JS_CFUNC_MAGIC_DEF("row", 2, uib_container, 0),
    JS_CFUNC_MAGIC_DEF("column", 2, uib_container, 1),
    JS_CFUNC_MAGIC_DEF("box", 2, uib_container, 2),
    JS_CFUNC_DEF("id", 2, uib_id),
    JS_CFUNC_DEF("spacer", 2, uib_spacer),
    JS_CFUNC_MAGIC_DEF("label", 1, uib_label_like, 0),
    JS_CFUNC_MAGIC_DEF("heading", 1, uib_label_like, 1),
    JS_CFUNC_MAGIC_DEF("bullet", 1, uib_label_like, 2),
    JS_CFUNC_MAGIC_DEF("separator", 1, uib_label_like, 3),
    JS_CFUNC_MAGIC_DEF("link", 1, uib_label_like, 4),
    JS_CFUNC_MAGIC_DEF("button", 1, uib_label_like, 5),
    JS_CFUNC_MAGIC_DEF("tooltip", 2, uib_label_like, 6),
    JS_CFUNC_DEF("text", 2, uib_text),
    JS_CFUNC_DEF("badge", 2, uib_badge),
    JS_CFUNC_DEF("image", 4, uib_image_w),
    JS_CFUNC_DEF("progress", 2, uib_progress),
    JS_CFUNC_DEF("spinner", 1, uib_spinner),
    JS_CFUNC_DEF("iconButton", 3, uib_icon_button),
    JS_CFUNC_MAGIC_DEF("toggle", 2, uib_flag, 0),
    JS_CFUNC_MAGIC_DEF("checkbox", 2, uib_flag, 1),
    JS_CFUNC_MAGIC_DEF("switch", 2, uib_flag, 2),
    JS_CFUNC_MAGIC_DEF("selectable", 2, uib_flag, 3),
    JS_CFUNC_MAGIC_DEF("radio", 4, uib_choice, 0),
    JS_CFUNC_MAGIC_DEF("segmented", 3, uib_choice, 1),
    JS_CFUNC_MAGIC_DEF("tabs", 3, uib_choice, 2),
    JS_CFUNC_MAGIC_DEF("combo", 3, uib_choice, 3),
    JS_CFUNC_DEF("slider", 5, uib_slider),
    JS_CFUNC_DEF("range", 5, uib_range),
    JS_CFUNC_MAGIC_DEF("drag", 3, uib_number, 0),
    JS_CFUNC_MAGIC_DEF("spin", 3, uib_number, 1),
    JS_CFUNC_MAGIC_DEF("textField", 3, uib_edit, 0),
    JS_CFUNC_MAGIC_DEF("textArea", 3, uib_edit, 1),
    JS_CFUNC_MAGIC_DEF("search", 3, uib_edit, 2),
    JS_CFUNC_MAGIC_DEF("list", 4, uib_list, 0),
    JS_CFUNC_MAGIC_DEF("log", 3, uib_list, 1),
    JS_CFUNC_DEF("color", 2, uib_color_w),
    JS_CFUNC_DEF("table", 2, uib_table),
    JS_CFUNC_MAGIC_DEF("plot", 3, uib_plot, 0),
    JS_CFUNC_MAGIC_DEF("bars", 3, uib_plot, 1),
    JS_CFUNC_MAGIC_DEF("property", 2, uib_wrap, UIB_PROPERTY),
    JS_CFUNC_MAGIC_DEF("group", 2, uib_wrap, UIB_GROUP),
    JS_CFUNC_MAGIC_DEF("panel", 3, uib_wrap, UIB_PANEL),
    JS_CFUNC_MAGIC_DEF("scroll", 3, uib_wrap, UIB_SCROLL),
    JS_CFUNC_MAGIC_DEF("menubar", 1, uib_wrap, UIB_MENUBAR),
    JS_CFUNC_MAGIC_DEF("toolbar", 1, uib_wrap, UIB_TOOLBAR),
    JS_CFUNC_MAGIC_DEF("statusbar", 1, uib_wrap, UIB_STATUSBAR),
    JS_CFUNC_DEF("window", 3, uib_window),
    JS_CFUNC_DEF("collapsing", 2, uib_collapsing),
    JS_CFUNC_DEF("tree", 3, uib_tree),
    JS_CFUNC_MAGIC_DEF("openPopup", 1, uib_popup_cmd, UIB_OPEN_POPUP),
    JS_CFUNC_MAGIC_DEF("openModal", 1, uib_popup_cmd, UIB_OPEN_MODAL),
    JS_CFUNC_MAGIC_DEF("closePopup", 0, uib_popup_cmd, UIB_CLOSE_POPUP),
    JS_CFUNC_MAGIC_DEF("popup", 2, uib_popup, UIB_POPUP),
    JS_CFUNC_MAGIC_DEF("contextMenu", 2, uib_popup, UIB_CONTEXT),
    JS_CFUNC_MAGIC_DEF("modal", 3, uib_popup, UIB_MODAL),
    JS_CFUNC_MAGIC_DEF("menu", 2, uib_popup, UIB_MENU),
    JS_CFUNC_DEF("menuItem", 2, uib_menu_item),
    JS_CFUNC_DEF("split", 5, uib_split),
    JS_CFUNC_MAGIC_DEF("wrap", 2, uib_container, 3),
    JS_CFUNC_DEF("chip", 2, uib_chip),
    JS_CFUNC_MAGIC_DEF("card", 3, uib_card, 0),
    JS_CFUNC_MAGIC_DEF("appBar", 2, uib_card, 1),
    JS_CFUNC_MAGIC_DEF("navBar", 3, uib_nav, 0),
    JS_CFUNC_MAGIC_DEF("navRail", 3, uib_nav, 1),
    JS_CFUNC_MAGIC_DEF("openDrawer", 1, uib_open_overlay, UIB_OPEN_DRAWER),
    JS_CFUNC_MAGIC_DEF("openSheet", 1, uib_open_overlay, UIB_OPEN_SHEET),
    JS_CFUNC_MAGIC_DEF("drawer", 3, uib_overlay, 0),
    JS_CFUNC_MAGIC_DEF("sheet", 2, uib_overlay, 1),
    JS_CFUNC_DEF("toast", 2, uib_toast),
    JS_CFUNC_DEF("fab", 2, uib_fab),
    JS_CFUNC_MAGIC_DEF("breadcrumbs", 2, uib_seq, UIB_BREADCRUMBS),
    JS_CFUNC_MAGIC_DEF("stepper", 3, uib_seq, UIB_STEPPER),
    JS_CFUNC_DEF("pagination", 3, uib_pagination),
    JS_CFUNC_DEF("rating", 3, uib_rating),
    JS_CFUNC_DEF("datePicker", 3, uib_date),
    JS_CFUNC_DEF("avatar", 2, uib_avatar),
    JS_CFUNC_DEF("skeleton", 2, uib_skeleton),
    JS_CFUNC_DEF("composition", 2, uib_composition),
    JS_CFUNC_DEF("resize", 3, uib_resize),
    JS_CFUNC_DEF("idle", 1, uib_idle),
    JS_CFUNC_DEF("geometry", 0, uib_geometry),
    JS_CFUNC_DEF("atlas", 0, uib_atlas_pixels),
    JS_CFUNC_DEF("stats", 0, uib_stats),
    JS_CFUNC_DEF("lastRect", 0, uib_last_rect),
    JS_CFUNC_DEF("dragSource", 2, uib_drag_source),
    JS_CFUNC_DEF("dropTarget", 0, uib_drop_target),
    JS_CGETSET_MAGIC_DEF("dragging", uib_getter, NULL, 12),
    JS_CFUNC_DEF("autocomplete", 4, uib_autocomplete),
    JS_CFUNC_DEF("timePicker", 3, uib_time_picker),
    JS_CFUNC_DEF("tagInput", 3, uib_tag_input),
    JS_CFUNC_DEF("accordion", 4, uib_accordion),
    JS_CFUNC_DEF("scaffold", 4, uib_scaffold),
    JS_CFUNC_DEF("pixelsInto", 1, uib_pixels_into),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Context", JS_PROP_CONFIGURABLE),
};

static const struct {
    const char* name;
    uint32_t dark, light;
} uib_palettes[] = {
    { "text", 0xE6E6E6FFu, 0x1F2328FFu },
    { "textDim", 0x9AA0A6FFu, 0x5F6670FFu },
    { "onAccent", 0xFFFFFFFFu, 0xFFFFFFFFu },
    { "bg", 0x202428FFu, 0xF6F7F9FFu },
    { "panel", 0x262B30FFu, 0xFFFFFFFFu },
    { "frame", 0x15181BFFu, 0xFFFFFFFFu },
    { "frameHot", 0x1B1F23FFu, 0xF0F2F5FFu },
    { "frameActive", 0x101214FFu, 0xE6E9EEFFu },
    { "button", 0x3A4048FFu, 0xE9ECF0FFu },
    { "buttonHot", 0x4A525CFFu, 0xDDE2E8FFu },
    { "buttonActive", 0x2F343AFFu, 0xCFD5DDFFu },
    { "accent", 0x3D7EFFFFu, 0x2F6FEBFFu },
    { "accentHot", 0x5A92FFFFu, 0x4A84F0FFu },
    { "border", 0x50565EFFu, 0xC9CFD6FFu },
    { "title", 0x2C3238FFu, 0xECEFF3FFu },
    { "titleActive", 0x34506EFFu, 0xD6E4FBFFu },
    { "selection", 0x3D7EFF66u, 0x2F6FEB40u },
    { "popup", 0x2A2F35FFu, 0xFFFFFFFFu },
    { "shade", 0x00000080u, 0x00000059u },
    { "tooltip", 0x111315F0u, 0xFFFFFFF5u },
    { "link", 0x6FA8FFFFu, 0x1F5FD6FFu },
    { "scrollbar", 0x5A616A99u, 0x8A93A066u },
    { "scrollbarHot", 0x7A828CCCu, 0x6B7480AAu },
    { "error", 0xF85149FFu, 0xCF222EFFu },
};

static JSValue uib_presets(JSContext* ctx, int which)
{
    JSValue o = JS_NewObject(ctx);
    size_t i;
    if (JS_IsException(o))
        return o;
    if (which < 2) {
        for (i = 0; i < countof(uib_palettes); i++)
            if (JS_DefinePropertyValueStr(ctx, o, uib_palettes[i].name, JS_NewUint32(ctx, which ? uib_palettes[i].light : uib_palettes[i].dark), JS_PROP_ENUMERABLE) < 0)
                goto fail;
    } else {
        dyn_ui_theme t;
        uib_density(&t, which == 3);
        if (JS_DefinePropertyValueStr(ctx, o, "minTarget", JS_NewFloat64(ctx, (double)t.min_target), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "padX", JS_NewFloat64(ctx, (double)t.pad_x), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "padY", JS_NewFloat64(ctx, (double)t.pad_y), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "spacing", JS_NewFloat64(ctx, (double)t.spacing), JS_PROP_ENUMERABLE) < 0)
            goto fail;
    }
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static int uib_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue themes, dens;
    static const JSClassDef def = { "Context", .finalizer = dyn_res_finalizer, .gc_mark = uib_mark };
    JSValueConst args[1];
    JSValue ta;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    if (JS_IsException(ta))
        return -1;
    uib_u8_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(ta))
        return -1;
    uib_f64_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    if (JS_IsException(ta))
        return -1;
    uib_f32_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    themes = JS_NewObject(ctx);
    dens = JS_NewObject(ctx);
    if (JS_IsException(themes) || JS_IsException(dens) || JS_DefinePropertyValueStr(ctx, themes, "dark", uib_presets(ctx, 0), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, themes, "light", uib_presets(ctx, 1), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, dens, "compact", uib_presets(ctx, 2), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, dens, "touch", uib_presets(ctx, 3), JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, themes);
        JS_FreeValue(ctx, dens);
        return -1;
    }
    if (JS_SetModuleExport(ctx, m, "themes", themes) < 0 || JS_SetModuleExport(ctx, m, "densities", dens) < 0)
        return -1;
    return dyn_register_class(ctx, m, &uib_cid, &def, uib_proto, countof(uib_proto), uib_ctor, "Context");
}

int js_nat_init_ui(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:ui", uib_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "themes") < 0 || JS_AddModuleExport(ctx, m, "densities") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "Context");
}

#endif
