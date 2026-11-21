#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

int uib_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** items, size_t** lens, int* count)
{
    int64_t n;
    int i;
    *items = NULL;
    *lens = NULL;
    *count = 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs an array of strings", who);
        return -1;
    }
    if (uib_length(ctx, arr, &n))
        return -1;
    if (n > UIB_MAX_ITEMS) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s takes at most %d items", who, UIB_MAX_ITEMS);
        return -1;
    }
    if (n == 0)
        return 0;
    *items = dyn_nat_malloc((size_t)n * sizeof(char*));
    *lens = dyn_nat_malloc((size_t)n * sizeof(size_t));
    if (!*items || !*lens) {
        dyn_nat_free(*items);
        dyn_nat_free(*lens);
        *items = NULL;
        *lens = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char* s;
        if (JS_IsException(e))
            goto fail;
        s = uib_keep(ctx, u, e, &(*lens)[i]);
        JS_FreeValue(ctx, e);
        if (!s)
            goto fail;
        (*items)[i] = s;
    }
    *count = (int)n;
    return 0;
fail:
    dyn_nat_free(*items);
    dyn_nat_free(*lens);
    *items = NULL;
    *lens = NULL;
    return -1;
}

JSValue uib_popup_cmd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "openPopup", "openModal", "closePopup" };
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    if (magic != UIB_CLOSE_POPUP && uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (u) {
        if (magic == UIB_OPEN_POPUP)
            dyn_ui_w_open_popup(u->ui, s, n);
        else if (magic == UIB_OPEN_MODAL)
            dyn_ui_w_open_modal(u->ui, s, n);
        else
            dyn_ui_w_close_popup(u->ui);
    }
    if (s)
        JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue uib_popup(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "popup", "contextMenu", "modal", "menu" };
    static const char* const mkeys[] = { "width" };
    JSValueConst fn = argc > 1 ? argv[argc > 2 ? 2 : 1] : JS_UNDEFINED, opts = argc > 2 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double w = 0;
    int depth = 0, open, failed = 0;
    dyn_ui_menu_frame mf;
    if (uib_fn(ctx, fn, who[magic]))
        return JS_EXCEPTION;
    if (magic == UIB_MODAL && (uib_check_opts(ctx, opts, "modal", mkeys, 1) || uib_opt_num(ctx, opts, "width", "modal width", 0, 16384, &w)))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case UIB_POPUP:
        open = dyn_ui_w_popup_begin(u->ui, s, n, &depth);
        break;
    case UIB_CONTEXT:
        open = dyn_ui_w_context_begin(u->ui, s, n, &depth);
        break;
    case UIB_MODAL:
        open = dyn_ui_w_modal_begin(u->ui, s, n, uib_px(u, w), &depth);
        break;
    default:
        open = dyn_ui_w_menu_begin(u->ui, s, n, &mf);
        break;
    }
    JS_FreeCString(ctx, s);
    if (!open)
        return JS_FALSE;
    failed = uib_run(ctx, fn, this_val);
    if (magic == UIB_MODAL)
        dyn_ui_w_modal_end(u->ui, depth);
    else if (magic == UIB_MENU)
        dyn_ui_w_menu_end(u->ui, &mf);
    else
        dyn_ui_w_popup_end(u->ui, depth);
    return failed ? JS_EXCEPTION : JS_TRUE;
}

int uib_nav_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** pitems, size_t** plens, uint32_t** picons, int* pcount)
{
    const char** items;
    size_t* lens;
    uint32_t* icons;
    int64_t len;
    int i, count = 0;
    *pitems = NULL;
    *plens = NULL;
    *picons = NULL;
    *pcount = 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s items are strings or { label, icon }", who);
        return -1;
    }
    if (uib_length(ctx, arr, &len))
        return -1;
    if (len > 64) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s holds at most 64 destinations", who);
        return -1;
    }
    items = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(char*));
    lens = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(size_t));
    icons = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(uint32_t));
    if (!items || !lens || !icons) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i), lab;
        if (JS_IsException(e))
            goto fail;
        if (JS_IsString(e)) {
            items[i] = uib_keep(ctx, u, e, &lens[i]);
        } else if (JS_IsObject(e)) {
            static const char* const keys[] = { "label", "icon" };
            JSValue ic;
            if (uib_check_opts(ctx, e, who, keys, 2)) {
                JS_FreeValue(ctx, e);
                goto fail;
            }
            lab = JS_GetPropertyStr(ctx, e, "label");
            items[i] = JS_IsException(lab) ? NULL : uib_keep(ctx, u, lab, &lens[i]);
            JS_FreeValue(ctx, lab);
            ic = JS_GetPropertyStr(ctx, e, "icon");
            if (items[i] && !JS_IsUndefined(ic) && uib_handle(ctx, u, ic, &icons[i]))
                items[i] = NULL;
            JS_FreeValue(ctx, ic);
        } else
            JS_ThrowTypeError(ctx, "dyna:ui: %s items are strings or { label, icon }", who);
        JS_FreeValue(ctx, e);
        if (!items[i])
            goto fail;
        count++;
    }
    *pitems = items;
    *plens = lens;
    *picons = icons;
    *pcount = count;
    return 0;
fail:
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    return -1;
}

JSValue uib_nav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "navRail" : "navBar";
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    uint32_t* icons = NULL;
    int sel, r, count = 0;
    if (argc < 3 || !JS_IsArray(ctx, argv[1]) || uib_index(ctx, argv[2], who, &sel))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items, selected); items are strings or { label, icon }", who);
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u || uib_nav_items(ctx, u, argv[1], who, &items, &lens, &icons, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_nav(u->ui, s, n, items, lens, icons, count, sel < count ? sel : -1, magic);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

JSValue uib_overlay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "side", "width" };
    const char* who = magic ? "sheet" : "drawer";
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char *s, *side = NULL;
    size_t n, sn = 0;
    double w = 0;
    int right = 0, depth = 0, open, failed = 0;
    if (uib_fn(ctx, fn, who))
        return JS_EXCEPTION;
    if (!magic) {
        if (uib_check_opts(ctx, opts, "drawer", keys, 2) || uib_opt_num(ctx, opts, "width", "drawer width", 0, 16384, &w)
            || uib_opt_str(ctx, opts, "side", "drawer side", &side, &sn))
            return JS_EXCEPTION;
        if (side) {
            int ok = !strcmp(side, "left") || !strcmp(side, "right");
            right = !strcmp(side, "right");
            JS_FreeCString(ctx, side);
            if (!ok)
                return JS_ThrowTypeError(ctx, "dyna:ui: drawer side is \"left\" or \"right\"");
        }
    } else if (!JS_IsUndefined(opts) && uib_check_opts(ctx, opts, "sheet", keys, 0))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    open = magic ? dyn_ui_w_sheet_begin(u->ui, s, n, &depth) : dyn_ui_w_drawer_begin(u->ui, s, n, right, uib_px(u, w), &depth);
    JS_FreeCString(ctx, s);
    if (!open)
        return JS_FALSE;
    failed = uib_run(ctx, fn, this_val);
    dyn_ui_w_overlay_end(u->ui, depth);
    return failed ? JS_EXCEPTION : JS_TRUE;
}

JSValue uib_accordion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    int open, count = 0, k, out, failed = 0;
    if (argc < 4 || uib_index(ctx, argv[2], "accordion open", &open) || uib_fn(ctx, argv[3], "accordion"))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: accordion(label, sections, open, fn)");
    if (uib_str(ctx, argv[0], "accordion", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "accordion");
    if (!u || uib_items(ctx, u, argv[1], "accordion", &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (open >= count)
        open = -1;
    out = open;
    dyn_ui_w_accordion_begin(u->ui, s, n);
    for (k = 0; k < count; k++) {
        if (dyn_ui_w_accordion_item(u->ui, k, items[k], lens[k], k == open))
            out = k == open ? -1 : k;
        if (k == open && !failed) {
            JSValueConst a[2];
            JSValue r;
            a[0] = this_val;
            a[1] = JS_NewInt32(ctx, k);
            dyn_ui_w_accordion_section_begin(u->ui);
            r = JS_Call(ctx, argv[3], JS_UNDEFINED, 2, a);
            dyn_ui_w_accordion_section_end(u->ui);
            if (JS_IsException(r))
                failed = 1;
            JS_FreeValue(ctx, r);
        }
    }
    dyn_ui_w_accordion_end(u->ui);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewInt32(ctx, out);
}

#endif
