#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "cutils.h"
#include "dyna-nat.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "core/dyn-ui.h"
#include "dyna-ui-backend.h"
#define UIB_MAX_ITEMS 65536
#define UIB_MAX_COLS 256

typedef struct {
    dyn_ui_t* ui;
    int w, h;
    float scale, lw, lh;
    float soft_insets[4];
    float insets_px[4];
    int touch_mode, touch_done;
    uint8_t* px;
    int present;
    int busy;
    int manual_clock;
    int events;
    int frames;
    int raster_ok;
    uint32_t clear;
    JSValue target_obj;
    JSRuntime* rt;
    const dyn_uib_ops* ops;
    void* be;
    char** strs;
    size_t nstrs, cstrs;
    JSContext* cb_ctx;
    JSValue cb_fn;
    JSValue cb_arr;
    int cb_failed;
    int activated;
    JSObjectTemplate* rect_tpl;
    JSObjectTemplate* table_tpl;
    JSValue dnd_payload;
} uib;

extern JSClassID uib_cid;

extern JSClassID uib_f64_cid;

extern JSClassID uib_f32_cid;

static inline float uib_px(const uib* u, double v)
{
    return (float)(v * (double)u->scale);
}

static inline JSValue uib_fill(JSContext* ctx, JSObjectTemplate* t, int n, JSValue* vals)
{
    JSValue o = JS_NewObjectFromTemplate(ctx, t);
    int k;
    for (k = 0; k < n; k++) {
        if (JS_IsException(o))
            JS_FreeValue(ctx, vals[k]);
        else
            JS_SetTemplateCell(ctx, o, (uint32_t)k, vals[k]);
    }
    return o;
}

static inline uib* uib_get(JSContext* ctx, JSValueConst this_val)
{
    uib* u = dyn_res_native(ctx, this_val, uib_cid);
    if (!u)
        return NULL;
    return u;
}

static inline uib* uib_in_frame(JSContext* ctx, JSValueConst this_val, const char* who)
{
    uib* u = uib_get(ctx, this_val);
    if (!u)
        return NULL;
    if (!u->busy) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s() is called inside frame(), between the widgets it lays out", who);
        return NULL;
    }
    return u;
}

int uib_str(JSContext* ctx, JSValueConst v, const char* who, const char** s, size_t* n);

int uib_num(JSContext* ctx, JSValueConst v, const char* who, double lo, double hi, double* out);

static inline int uib_value(JSContext* ctx, JSValueConst v, const char* who, double* out)
{
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s must be a number", who);
        return -1;
    }
    return JS_ToFloat64(ctx, out, v);
}

static inline int uib_color(JSContext* ctx, JSValueConst v, const char* who, uint32_t* out)
{
    double d;
    if (uib_num(ctx, v, who, 0, 4294967295.0, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s is a 0xRRGGBBAA integer", who);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static inline int uib_bool(JSContext* ctx, JSValueConst v, int* out)
{
    int b = JS_ToBool(ctx, v);
    if (b < 0)
        return -1;
    *out = b;
    return 0;
}

static inline int uib_fn(JSContext* ctx, JSValueConst v, const char* who)
{
    if (!JS_IsFunction(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs a function", who);
        return -1;
    }
    return 0;
}

int uib_check_opts_m(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys, uint32_t* present);

int uib_check_opts(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys);

static inline int uib_get_opt(JSContext* ctx, JSValueConst opts, const char* key, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return 0;
    *out = JS_GetPropertyStr(ctx, opts, key);
    return JS_IsException(*out) ? -1 : 0;
}

static inline int uib_opt_num(JSContext* ctx, JSValueConst opts, const char* key, const char* who, double lo, double hi, double* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_num(ctx, v, who, lo, hi, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static inline int uib_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_bool(ctx, v, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static inline int uib_opt_color(JSContext* ctx, JSValueConst opts, const char* key, const char* who, uint32_t* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_color(ctx, v, who, out);
    JS_FreeValue(ctx, v);
    return rc;
}

int uib_opt_str(JSContext* ctx, JSValueConst opts, const char* key, const char* who, const char** s, size_t* n);

int uib_length(JSContext* ctx, JSValueConst v, int64_t* out);

static inline double uib_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

const char* uib_keep(JSContext* ctx, uib* u, JSValueConst v, size_t* n);

int uib_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** items, size_t** lens, int* count);

int uib_run(JSContext* ctx, JSValueConst fn, JSValueConst self);

JSValue uib_container(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_label_like(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

int uib_handle(JSContext* ctx, uib* u, JSValueConst v, uint32_t* out);

JSValue uib_image_w(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static inline int uib_index(JSContext* ctx, JSValueConst v, const char* who, int* out)
{
    double d;
    if (uib_num(ctx, v, who, -1, UIB_MAX_ITEMS, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s is an integer index", who);
        return -1;
    }
    *out = (int)d;
    return 0;
}

int uib_note_opts(JSContext* ctx, JSValueConst opts, const char** err, size_t* en, const char** help, size_t* hn);

void uib_note_free(JSContext* ctx, const char* err, const char* help);

void uib_note_begin(uib* u, size_t en, size_t hn);

void uib_note_end(uib* u, const char* err, size_t en, const char* help, size_t hn);

JSValue uib_choice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

static inline int uib_decimals(JSContext* ctx, JSValueConst opts, int* dec)
{
    double d = *dec;
    if (uib_opt_num(ctx, opts, "decimals", "decimals", 0, 9, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: decimals is an integer 0..9");
        return -1;
    }
    *dec = (int)d;
    return 0;
}

JSValue uib_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_number(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_edit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_table(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_plot(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

enum { UIB_PROPERTY,
    UIB_GROUP,
    UIB_PANEL,
    UIB_SCROLL,
    UIB_MENUBAR,
    UIB_TOOLBAR,
    UIB_STATUSBAR };

JSValue uib_wrap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

enum { UIB_OPEN_POPUP,
    UIB_OPEN_MODAL,
    UIB_CLOSE_POPUP };

JSValue uib_popup_cmd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

enum { UIB_POPUP,
    UIB_CONTEXT,
    UIB_MODAL,
    UIB_MENU };

JSValue uib_popup(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_pointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_set_time(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int uib_bytes_rw(JSContext* ctx, JSValueConst v, const char* who, uint8_t** p, size_t* n);

static inline int uib_bytes(JSContext* ctx, JSValueConst v, const char* who, const uint8_t** p, size_t* n)
{
    uint8_t* q = NULL;
    int r = uib_bytes_rw(ctx, v, who, &q, n);
    *p = q;
    return r;
}

JSValue uib_pixels_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_add_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_rect_obj(JSContext* ctx, uib* u, const dyn_ui_rect* r);

int uib_nav_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** pitems, size_t** plens, uint32_t** picons, int* pcount);

JSValue uib_nav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_overlay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue uib_date(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_drag_source(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_autocomplete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_time_picker(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_tag_input(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue uib_accordion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#endif

#endif
