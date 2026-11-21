#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

JSValue uib_label_like(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "label", "heading", "bullet", "separator", "link", "button", "tooltip" };
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    double delay = 0.5;
    int r = 0;
    if (magic == 3 && (argc < 1 || JS_IsUndefined(argv[0]))) {
        u = uib_in_frame(ctx, this_val, who[magic]);
        if (!u)
            return JS_EXCEPTION;
        dyn_ui_w_separator(u->ui, NULL, 0);
        return JS_UNDEFINED;
    }
    if (magic == 6 && argc > 1) {
        static const char* const keys[] = { "delay" };
        if (uib_check_opts(ctx, argv[1], "tooltip", keys, 1) || uib_opt_num(ctx, argv[1], "delay", "tooltip delay", 0, 3600, &delay))
            return JS_EXCEPTION;
    }
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        dyn_ui_w_label(u->ui, s, n);
        break;
    case 1:
        dyn_ui_w_heading(u->ui, s, n);
        break;
    case 2:
        dyn_ui_w_bullet(u->ui, s, n);
        break;
    case 3:
        dyn_ui_w_separator(u->ui, s, n);
        break;
    case 4:
        r = dyn_ui_w_link(u->ui, s, n);
        break;
    case 5:
        r = dyn_ui_w_button(u->ui, s, n);
        break;
    default:
        dyn_ui_w_tooltip(u->ui, s, n, delay);
        break;
    }
    JS_FreeCString(ctx, s);
    return magic == 4 || magic == 5 ? JS_NewBool(ctx, r) : JS_UNDEFINED;
}

JSValue uib_image_w(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    uint32_t img, tint = 0;
    double w = 0, h = 0;
    if ((argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "image width", 0, 16384, &w))
        || (argc > 2 && !JS_IsUndefined(argv[2]) && uib_num(ctx, argv[2], "image height", 0, 16384, &h))
        || (argc > 3 && !JS_IsUndefined(argv[3]) && uib_color(ctx, argv[3], "image tint", &tint)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "image");
    if (!u || uib_handle(ctx, u, argc > 0 ? argv[0] : JS_UNDEFINED, &img))
        return JS_EXCEPTION;
    dyn_ui_w_image(u->ui, img, uib_px(u, w), uib_px(u, h), tint);
    return JS_UNDEFINED;
}

int uib_note_opts(JSContext* ctx, JSValueConst opts, const char** err, size_t* en, const char** help, size_t* hn)
{
    *err = *help = NULL;
    *en = *hn = 0;
    if (uib_opt_str(ctx, opts, "error", "error", err, en))
        return -1;
    if (uib_opt_str(ctx, opts, "help", "help", help, hn)) {
        if (*err)
            JS_FreeCString(ctx, *err);
        *err = NULL;
        return -1;
    }
    return 0;
}

void uib_note_free(JSContext* ctx, const char* err, const char* help)
{
    if (err)
        JS_FreeCString(ctx, err);
    if (help)
        JS_FreeCString(ctx, help);
}

void uib_note_begin(uib* u, size_t en, size_t hn)
{
    if (en || hn) {
        dyn_ui_layout l;
        memset(&l, 0, sizeof(l));
        l.w.kind = DYN_UI_GROW;
        l.w.value = 1;
        l.h.kind = DYN_UI_FIT;
        l.dir = DYN_UI_COLUMN;
        l.gap = roundf(2 * u->scale);
        dyn_ui_open(u->ui, 0, &l, NULL);
    }
    if (en)
        dyn_ui_w_field_error(u->ui, 1);
}

void uib_note_end(uib* u, const char* err, size_t en, const char* help, size_t hn)
{
    if (en)
        dyn_ui_w_field_error(u->ui, 0);
    if (en || hn) {
        dyn_ui_w_field_note(u->ui, en ? err : help, en ? en : hn, en != 0);
        dyn_ui_close(u->ui);
    }
}

static const char* uib_cell_fn(void* ud, int row, int col, size_t* n)
{
    uib* u = ud;
    JSContext* ctx = u->cb_ctx;
    JSValueConst a[2];
    JSValue r;
    const char* s;
    if (u->cb_failed)
        return NULL;
    a[0] = JS_NewInt32(ctx, row);
    a[1] = JS_NewInt32(ctx, col);
    r = JS_Call(ctx, u->cb_fn, JS_UNDEFINED, 2, a);
    if (JS_IsException(r)) {
        u->cb_failed = 1;
        return NULL;
    }
    if (!JS_IsString(r)) {
        JSValue str = JS_ToString(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(str)) {
            u->cb_failed = 1;
            return NULL;
        }
        r = str;
    }
    s = uib_keep(ctx, u, r, n);
    JS_FreeValue(ctx, r);
    if (!s)
        u->cb_failed = 1;
    return s;
}

JSValue uib_table(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "columns", "rows", "cell", "visibleRows", "selected", "sort", "selectedColumn", "edit" };
    static const char* const ckeys[] = { "name", "width" };
    static const char* const skeys[] = { "column", "descending" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    const char* heads[UIB_MAX_COLS];
    size_t hlens[UIB_MAX_COLS];
    float widths[UIB_MAX_COLS];
    double rows = 0, vis = 10, sel = -1, scol = -1, ccol = -2;
    int sdesc = 0, ncols = 0, i, r;
    uint32_t present;
    const char* edit = NULL;
    size_t edit_n = 0;
    int64_t nc;
    JSValue cols = JS_UNDEFINED, cell = JS_UNDEFINED, sort = JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "dyna:ui: table(label, { columns, rows, cell, ... })");
    if (uib_check_opts_m(ctx, opts, "table", keys, (int)countof(keys), &present) || uib_opt_num(ctx, opts, "rows", "table rows", 0, 1000000, &rows)
        || uib_opt_num(ctx, opts, "visibleRows", "table visibleRows", 1, 1000, &vis) || uib_opt_num(ctx, opts, "selected", "table selected", -1, 1000000, &sel)
        || ((present & 0x40u) && uib_opt_num(ctx, opts, "selectedColumn", "table selectedColumn", -1, UIB_MAX_COLS, &ccol)))
        return JS_EXCEPTION;
    if (rows != floor(rows) || sel != floor(sel) || vis != floor(vis) || ccol != floor(ccol))
        return JS_ThrowRangeError(ctx, "dyna:ui: table rows, visibleRows, selected and selectedColumn are integers");
    u = uib_in_frame(ctx, this_val, "table");
    if (!u)
        return JS_EXCEPTION;

    if (uib_get_opt(ctx, opts, "sort", &sort))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(sort)) {
        int rc = uib_check_opts(ctx, sort, "table sort", skeys, 2) || uib_opt_num(ctx, sort, "column", "sort column", -1, UIB_MAX_COLS, &scol)
            || uib_opt_bool(ctx, sort, "descending", &sdesc);
        JS_FreeValue(ctx, sort);
        if (rc)
            return JS_EXCEPTION;
        if (scol != floor(scol))
            return JS_ThrowRangeError(ctx, "dyna:ui: sort column is an integer");
    }
    if (uib_get_opt(ctx, opts, "cell", &cell))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, cell)) {
        JS_FreeValue(ctx, cell);
        return JS_ThrowTypeError(ctx, "dyna:ui: table cell is a function (row, column) => string");
    }
    if (uib_get_opt(ctx, opts, "columns", &cols) || !JS_IsArray(ctx, cols) || uib_length(ctx, cols, &nc) || nc < 1 || nc > UIB_MAX_COLS) {
        JS_FreeValue(ctx, cell);
        JS_FreeValue(ctx, cols);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "dyna:ui: table columns is an array of 1..%d { name, width? }", UIB_MAX_COLS);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, cols, (uint32_t)i), nm;
        double w = 0;
        int bad;
        if (JS_IsException(c))
            goto fail;
        bad = uib_check_opts(ctx, c, "table column", ckeys, 2) || !JS_IsObject(c) || uib_opt_num(ctx, c, "width", "column width", 0, 4000, &w);
        if (!bad) {
            nm = JS_GetPropertyStr(ctx, c, "name");
            heads[i] = JS_IsException(nm) ? NULL : uib_keep(ctx, u, nm, &hlens[i]);
            JS_FreeValue(ctx, nm);
            bad = !heads[i];
        }
        JS_FreeValue(ctx, c);
        if (bad) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "dyna:ui: a table column is { name: string, width?: number }");
            goto fail;
        }
        widths[i] = uib_px(u, w);
        ncols++;
    }
    JS_FreeValue(ctx, cols);
    cols = JS_UNDEFINED;
    if ((present & 0x80u) && uib_opt_str(ctx, opts, "edit", "table edit", &edit, &edit_n))
        goto fail;
    if (edit && ccol < -1) {
        JS_ThrowTypeError(ctx, "dyna:ui: table edit needs selectedColumn (it edits the selected cell)");
        goto fail;
    }
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "table", &s, &n))
        goto fail;
    {
        dyn_ui_table_state st;
        memset(&st, 0, sizeof(st));
        st.selected = (int)sel;
        st.column = ccol < -1 ? -1 : (int)ccol;
        st.cells = ccol >= -1;
        st.sort_col = scol < ncols ? (int)scol : -1;
        st.sort_desc = sdesc;
        st.edit = edit;
        st.edit_n = edit_n;
        u->cb_ctx = ctx;
        u->cb_fn = cell;
        u->cb_failed = 0;
        r = dyn_ui_w_table_ex(u->ui, s, n, heads, hlens, widths, ncols, (int)rows, uib_cell_fn, u, (int)vis, &st);
        u->cb_fn = JS_UNDEFINED;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, cell);
        if (st.activated)
            u->activated = r;
        if (u->cb_failed) {
            if (edit)
                JS_FreeCString(ctx, edit);
            return JS_EXCEPTION;
        }
        {
            JSValue v[8];
            v[0] = JS_NewInt32(ctx, r);
            v[1] = JS_NewInt32(ctx, st.sort_col);
            v[2] = JS_NewBool(ctx, st.sort_desc);
            v[3] = JS_NewInt32(ctx, st.column);
            v[4] = JS_NewBool(ctx, st.activated);
            v[5] = st.edit_out ? JS_NewStringLen(ctx, st.edit_out, st.edit_out_n) : JS_UNDEFINED;
            v[6] = JS_NewBool(ctx, st.committed);
            v[7] = JS_NewBool(ctx, st.cancelled);
            if (edit)
                JS_FreeCString(ctx, edit);
            return uib_fill(ctx, u->table_tpl, 8, v);
        }
    }
fail:
    if (edit)
        JS_FreeCString(ctx, edit);
    JS_FreeValue(ctx, cell);
    JS_FreeValue(ctx, cols);
    return JS_EXCEPTION;
}

JSValue uib_plot(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "min", "max", "height" };
    const char* who = magic ? "bars" : "plot";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double lo = 0, hi = 0, h = 80, *v = NULL;
    int64_t count, i;
    if (argc < 2 || uib_check_opts(ctx, opts, who, keys, 3) || uib_opt_num(ctx, opts, "min", "min", -1e300, 1e300, &lo)
        || uib_opt_num(ctx, opts, "max", "max", -1e300, 1e300, &hi) || uib_opt_num(ctx, opts, "height", "height", 8, 4096, &h))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: %s(label, values, opts?)", who) : JS_EXCEPTION;
    if (uib_length(ctx, argv[1], &count))
        return JS_EXCEPTION;
    if (!JS_IsObject(argv[1]) || count > (magic ? 4096 : 1000000))
        return JS_ThrowRangeError(ctx, "dyna:ui: %s values is an array or typed array of at most %d numbers", who, magic ? 4096 : 1000000);
    v = dyn_nat_malloc((size_t)(count ? count : 1) * sizeof(double));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    if (JS_GetClassID(argv[1]) == uib_f64_cid || JS_GetClassID(argv[1]) == uib_f32_cid) {
        size_t off, len, bpe, blen;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, &bpe);
        const uint8_t* base;
        if (JS_IsException(buf)) {
            dyn_nat_free(v);
            return JS_EXCEPTION;
        }
        base = JS_GetArrayBuffer(ctx, &blen, buf);
        JS_FreeValue(ctx, buf);
        if (!base || off > blen || len > blen - off || len / bpe < (size_t)count) {
            dyn_nat_free(v);
            return JS_ThrowTypeError(ctx, "dyna:ui: %s values: the typed array is detached or lies outside its buffer", who);
        }
        for (i = 0; i < count; i++) {
            if (bpe == 8)
                memcpy(&v[i], base + off + (size_t)i * 8, 8);
            else {
                float f;
                memcpy(&f, base + off + (size_t)i * 4, 4);
                v[i] = (double)f;
            }
        }
        count = -count - 1;
    }
    for (i = 0; i < count; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        int rc = JS_IsException(e) || uib_value(ctx, e, "a plotted value", &v[i]);
        JS_FreeValue(ctx, e);
        if (rc) {
            dyn_nat_free(v);
            return JS_EXCEPTION;
        }
    }
    if (count < 0)
        count = -count - 1;
    if (uib_str(ctx, argv[0], who, &s, &n)) {
        dyn_nat_free(v);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, who);
    if (u)
        dyn_ui_w_plot(u->ui, s, n, v, (int)count, lo, hi, uib_px(u, h), magic);
    JS_FreeCString(ctx, s);
    dyn_nat_free(v);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue uib_add_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uib* u;
    const uint8_t* p;
    size_t n;
    double w = 0, h = 0, hd = 0;
    int r;
    if (magic == 0) {
        if (argc < 3 || uib_num(ctx, argv[1], "image width", 1, 16384, &w) || uib_num(ctx, argv[2], "image height", 1, 16384, &h))
            return argc < 3 ? JS_ThrowTypeError(ctx, "dyna:ui: addImage(rgba, width, height)") : JS_EXCEPTION;
        if (w != floor(w) || h != floor(h) || w * h > (double)(1 << 26))
            return JS_ThrowRangeError(ctx, "dyna:ui: image size is whole pixels, at most 2^26 of them");
    } else if (argc < 2 || uib_num(ctx, argv[0], "image handle", 1, 4095, &hd))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: updateImage(handle, rgba)") : JS_EXCEPTION;
    if (uib_bytes(ctx, argv[magic ? 1 : 0], magic ? "updateImage" : "addImage", &p, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (uib_bytes(ctx, argv[magic ? 1 : 0], magic ? "updateImage" : "addImage", &p, &n))
        return JS_EXCEPTION;
    if (magic == 0) {
        if (n != (size_t)w * (size_t)h * 4)
            return JS_ThrowRangeError(ctx, "dyna:ui: addImage needs width * height * 4 bytes of RGBA (%zu given, %zu needed)", n, (size_t)w * (size_t)h * 4);
        r = dyn_ui_image_add(u->ui, (int)w, (int)h, p);
        if (r < 0)
            return JS_ThrowRangeError(ctx, "dyna:ui: addImage: out of image slots (4095) or memory");
        return JS_NewInt32(ctx, r);
    }
    {
        int iw, ih;
        if (hd != floor(hd) || dyn_ui_image_size(u->ui, (uint32_t)hd, &iw, &ih))
            return JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", hd);
        if (n != (size_t)iw * (size_t)ih * 4)
            return JS_ThrowRangeError(ctx, "dyna:ui: updateImage needs %zu bytes for this %dx%d image, %zu given", (size_t)iw * (size_t)ih * 4, iw, ih, n);
        dyn_ui_image_update(u->ui, (uint32_t)hd, p);
        u->raster_ok = 0;
    }
    return JS_UNDEFINED;
}

#endif
