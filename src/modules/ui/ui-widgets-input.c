#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

int uib_check_opts_m(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys, uint32_t* present)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int j, rc = 0;
    if (present)
        *present = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts) || JS_IsFunction(ctx, opts) || JS_IsArray(ctx, opts)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s options must be a plain object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len && rc == 0; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int ok = 0;
        if (!k) {
            rc = -1;
            break;
        }
        for (j = 0; j < nkeys; j++)
            if (!strcmp(k, keys[j])) {
                ok = 1;
                if (present && j < 32)
                    *present |= 1u << j;
            }
        if (!ok) {
            JS_ThrowTypeError(ctx, "dyna:ui: %s: unknown option '%s'", who, k);
            rc = -1;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

int uib_check_opts(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys)
{
    return uib_check_opts_m(ctx, opts, who, keys, nkeys, NULL);
}

JSValue uib_choice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "radio", "segmented", "tabs", "combo" };
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    const char *err = NULL, *help = NULL;
    size_t en = 0, hn = 0;
    int sel, count, r, horiz = 0, search = 0;
    if (argc < 3 || uib_index(ctx, argv[2], who[magic], &sel))
        return argc < 3 ? JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items, selected)", who[magic]) : JS_EXCEPTION;
    if (magic == 0 && argc > 3) {
        static const char* const keys[] = { "horizontal" };
        if (uib_check_opts(ctx, argv[3], "radio", keys, 1) || uib_opt_bool(ctx, argv[3], "horizontal", &horiz))
            return JS_EXCEPTION;
    }
    if (magic == 3 && argc > 3) {
        static const char* const keys[] = { "search", "error", "help" };
        if (uib_check_opts(ctx, argv[3], "combo", keys, 3) || uib_opt_bool(ctx, argv[3], "search", &search))
            return JS_EXCEPTION;
    }
    if (uib_str(ctx, argv[0], who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u || uib_items(ctx, u, argv[1], who[magic], &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (magic == 3 && argc > 3 && uib_note_opts(ctx, argv[3], &err, &en, &help, &hn)) {
        dyn_nat_free(items);
        dyn_nat_free(lens);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (sel >= count)
        sel = -1;
    switch (magic) {
    case 0:
        r = dyn_ui_w_radio(u->ui, s, n, items, lens, count, sel, horiz);
        break;
    case 1:
        r = dyn_ui_w_segmented(u->ui, s, n, items, lens, count, sel);
        break;
    case 2:
        r = dyn_ui_w_tabs(u->ui, s, n, items, lens, count, sel);
        break;
    default:
        uib_note_begin(u, en, hn);
        r = dyn_ui_w_combo_ex(u->ui, s, n, items, lens, count, sel, search);
        uib_note_end(u, err, en, help, hn);
        uib_note_free(ctx, err, help);
        break;
    }
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

JSValue uib_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "decimals" };
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double a, b, a0, b0, lo, hi, step = 0;
    int dec = 2;
    int64_t len;
    JSValue e0, e1, out;
    if (argc < 4 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: range(label, [low, high], min, max, opts?)");
    if (uib_length(ctx, argv[1], &len))
        return JS_EXCEPTION;
    if (len != 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: range value is [low, high]");
    e0 = JS_GetPropertyUint32(ctx, argv[1], 0);
    e1 = JS_GetPropertyUint32(ctx, argv[1], 1);
    if (JS_IsException(e0) || JS_IsException(e1) || uib_value(ctx, e0, "range low", &a) || uib_value(ctx, e1, "range high", &b)) {
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, e0);
    JS_FreeValue(ctx, e1);
    if (uib_num(ctx, argv[2], "range min", -1e300, 1e300, &lo) || uib_num(ctx, argv[3], "range max", -1e300, 1e300, &hi)
        || uib_check_opts(ctx, opts, "range", keys, 2) || uib_opt_num(ctx, opts, "step", "range step", 0, 1e300, &step) || uib_decimals(ctx, opts, &dec))
        return JS_EXCEPTION;
    if (!(lo < hi))
        return JS_ThrowRangeError(ctx, "dyna:ui: range min must be below max");
    if (uib_str(ctx, argv[0], "range", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "range");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    a0 = a;
    b0 = b;
    dyn_ui_w_range(u->ui, s, n, &a, &b, lo, hi, step, dec);
    JS_FreeCString(ctx, s);
    if (a == a0 && b == b0)
        return JS_DupValue(ctx, argv[1]);
    out = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_DefinePropertyValueUint32(ctx, out, 0, JS_NewFloat64(ctx, a), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueUint32(ctx, out, 1, JS_NewFloat64(ctx, b), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

JSValue uib_number(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const dkeys[] = { "speed", "min", "max", "decimals" };
    static const char* const skeys[] = { "step", "min", "max", "decimals" };
    const char* who = magic ? "spin" : "drag";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, r, speed = 1, step = 1, lo = 0, hi = -1;
    int dec = magic ? 0 : 2;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, value, opts?)", who);
    if (uib_value(ctx, argv[1], who, &v) || uib_check_opts(ctx, opts, who, magic ? skeys : dkeys, 4)
        || uib_opt_num(ctx, opts, magic ? "step" : "speed", magic ? "spin step" : "drag speed", 1e-300, 1e300, magic ? &step : &speed)
        || uib_opt_num(ctx, opts, "min", "min", -1e300, 1e300, &lo) || uib_opt_num(ctx, opts, "max", "max", -1e300, 1e300, &hi)
        || uib_decimals(ctx, opts, &dec))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        JSValue a, b;
        int has_a, has_b;
        if (uib_get_opt(ctx, opts, "min", &a))
            return JS_EXCEPTION;
        if (uib_get_opt(ctx, opts, "max", &b)) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
        has_a = !JS_IsUndefined(a);
        has_b = !JS_IsUndefined(b);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (has_a != has_b)
            return JS_ThrowTypeError(ctx, "dyna:ui: %s takes min and max together", who);
        if (has_a && !(lo <= hi))
            return JS_ThrowRangeError(ctx, "dyna:ui: %s min must not exceed max", who);
    }
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = magic ? dyn_ui_w_spin(u->ui, s, n, v, lo, hi, step, dec) : dyn_ui_w_drag(u->ui, s, n, v, speed, lo, hi, dec);
    JS_FreeCString(ctx, s);
    return r == v || (r != r && v != v) ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, r);
}

JSValue uib_edit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "hint", "maxLength", "width", "password", "readOnly", "rows", "error", "help" };
    static const char* const who[] = { "textField", "textArea", "search" };
    const char *err = NULL, *help = NULL;
    size_t en = 0, hn2 = 0;
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s = NULL, *t = NULL, *hint = NULL, *out;
    size_t n, tn, hn = 0, on;
    double maxlen = 0, width = 0, rows = 4;
    int pw = 0, ro = 0, flags, events = 0;
    uint32_t present;
    JSValue ret;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, text, opts?)", who[magic]);
    if (uib_check_opts_m(ctx, opts, who[magic], keys, (int)countof(keys), &present) || uib_opt_num(ctx, opts, "maxLength", "maxLength", 0, 16777216, &maxlen)
        || uib_opt_num(ctx, opts, "width", "width", 0, 16384, &width) || uib_opt_bool(ctx, opts, "password", &pw) || uib_opt_bool(ctx, opts, "readOnly", &ro)
        || uib_opt_num(ctx, opts, "rows", "rows", 1, 1000, &rows))
        return JS_EXCEPTION;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[0], who[magic], &s, &n) || uib_str(ctx, argv[1], "text", &t, &tn)) {
        if (s)
            JS_FreeCString(ctx, s);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, t);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    flags = (pw ? DYN_UI_EDIT_PASSWORD : 0) | (ro ? DYN_UI_EDIT_READONLY : 0) | (magic == 1 ? DYN_UI_EDIT_MULTILINE : 0);
    if (uib_note_opts(ctx, present & 0xC0u ? opts : JS_UNDEFINED, &err, &en, &help, &hn2)) {
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, t);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    uib_note_begin(u, en, hn2);
    if (magic == 2) {
        out = dyn_ui_w_search(u->ui, s, n, t, tn, hint ? hint : "Search", hint ? hn : 6, uib_px(u, width), &on, &events);
    } else {
        out = dyn_ui_w_edit(u->ui, s, n, t, tn, flags, (int)maxlen, hint, hn, (float)rows, uib_px(u, width), &on, &events);
    }
    uib_note_end(u, err, en, help, hn2);
    uib_note_free(ctx, err, help);
    u->events = events;
    if (out == t || (on == tn && !memcmp(out, t, tn)))
        ret = JS_DupValue(ctx, argv[1]);
    else
        ret = JS_NewStringLen(ctx, out, on);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, t);
    if (hint)
        JS_FreeCString(ctx, hint);
    return ret;
}

JSValue uib_set_time(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double t;
    if (uib_num(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "setTime", 0, 1e12, &t))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (!u->manual_clock)
        return JS_ThrowTypeError(ctx, "dyna:ui: setTime() needs a Context made with { clock: \"manual\" }");
    dyn_ui_set_time(u->ui, t);
    return JS_UNDEFINED;
}

static int uib_date_field(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    int rc;
    if (JS_IsException(v))
        return -1;
    rc = uib_num(ctx, v, k, lo, hi, &d);
    JS_FreeValue(ctx, v);
    if (rc)
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: date %s is an integer", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

JSValue uib_date(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "firstDay" };
    static const char* const dkeys[] = { "year", "month", "day" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    int y, m, d, first = 1, changed;
    double fd = 1;
    JSValue o;
    if (argc < 2 || !JS_IsObject(argv[1]) || uib_check_opts(ctx, argv[1], "datePicker date", dkeys, 3) || uib_date_field(ctx, argv[1], "year", 1, 9999, &y)
        || uib_date_field(ctx, argv[1], "month", 1, 12, &m) || uib_date_field(ctx, argv[1], "day", 1, 31, &d) || uib_check_opts(ctx, opts, "datePicker", keys, 1)
        || uib_opt_num(ctx, opts, "firstDay", "datePicker firstDay", 0, 6, &fd))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: datePicker(label, { year, month, day }, opts?)");
    first = (int)fd;
    if (uib_str(ctx, argv[0], "datePicker", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "datePicker");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    changed = dyn_ui_w_date(u->ui, s, n, &y, &m, &d, first);
    JS_FreeCString(ctx, s);
    if (!changed)
        return JS_DupValue(ctx, argv[1]);
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "year", JS_NewInt32(ctx, y), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "month", JS_NewInt32(ctx, m), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "day", JS_NewInt32(ctx, d), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue uib_autocomplete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "hint", "width", "error", "help" };
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    uib* u;
    const char *s = NULL, *t = NULL, *hint = NULL, *err = NULL, *help = NULL, *out;
    size_t n, tn, hn = 0, en = 0, hpn = 0, on = 0, *lens = NULL;
    const char** items = NULL;
    double width = 0;
    int count = 0, picked = -1;
    JSValue ret = JS_EXCEPTION;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:ui: autocomplete(label, text, suggestions, opts?)");
    if (uib_check_opts(ctx, opts, "autocomplete", keys, 4) || uib_opt_num(ctx, opts, "width", "width", 0, 16384, &width))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[0], "autocomplete", &s, &n))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[1], "text", &t, &tn)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "autocomplete");
    if (!u || uib_items(ctx, u, argv[2], "autocomplete suggestions", &items, &lens, &count))
        goto done;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn) || uib_note_opts(ctx, opts, &err, &en, &help, &hpn))
        goto done;
    uib_note_begin(u, en, hpn);
    out = dyn_ui_w_autocomplete(u->ui, s, n, t, tn, items, lens, count, hint, hn, uib_px(u, width), &on, &picked);
    uib_note_end(u, err, en, help, hpn);
    if (picked < 0 && (out == t || (on == tn && !memcmp(out, t, tn))))
        ret = JS_DupValue(ctx, argv[1]);
    else
        ret = JS_NewStringLen(ctx, out, on);
done:
    dyn_nat_free(items);
    dyn_nat_free(lens);
    uib_note_free(ctx, err, help);
    if (hint)
        JS_FreeCString(ctx, hint);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, t);
    return ret;
}

JSValue uib_time_picker(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "h12", "error", "help" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s, *err = NULL, *help = NULL;
    size_t n, en = 0, hn = 0;
    double hour, minute, step = 1;
    int h12 = 0, hi, mi, changed;
    JSValue hv, mv, o;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: timePicker(label, { hour, minute }, opts?)");
    if (uib_check_opts(ctx, opts, "timePicker", keys, 4) || uib_opt_num(ctx, opts, "step", "timePicker step", 1, 60, &step) || uib_opt_bool(ctx, opts, "h12", &h12))
        return JS_EXCEPTION;
    if (step != floor(step))
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker step is an integer 1..60");
    if (60 % (int)step)
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker step must divide 60 (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30 or 60)");
    hv = JS_GetPropertyStr(ctx, argv[1], "hour");
    if (JS_IsException(hv))
        return JS_EXCEPTION;
    mv = JS_GetPropertyStr(ctx, argv[1], "minute");
    if (JS_IsException(mv)) {
        JS_FreeValue(ctx, hv);
        return JS_EXCEPTION;
    }
    if (uib_num(ctx, hv, "hour", 0, 23, &hour) || uib_num(ctx, mv, "minute", 0, 59, &minute)) {
        JS_FreeValue(ctx, hv);
        JS_FreeValue(ctx, mv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, hv);
    JS_FreeValue(ctx, mv);
    if (hour != floor(hour) || minute != floor(minute))
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker hour and minute are integers");
    if (uib_str(ctx, argv[0], "timePicker", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "timePicker");
    if (!u || uib_note_opts(ctx, opts, &err, &en, &help, &hn)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    hi = (int)hour;
    mi = (int)minute;
    uib_note_begin(u, en, hn);
    changed = dyn_ui_w_time(u->ui, s, n, &hi, &mi, (int)step, h12 ? DYN_UI_TIME_H12 : 0);
    uib_note_end(u, err, en, help, hn);
    uib_note_free(ctx, err, help);
    JS_FreeCString(ctx, s);
    if (!changed)
        return JS_DupValue(ctx, argv[1]);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "hour", JS_NewInt32(ctx, hi), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "minute", JS_NewInt32(ctx, mi), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue uib_tag_input(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "hint", "error", "help" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s, *hint = NULL, *err = NULL, *help = NULL, *add = NULL;
    size_t n, hn = 0, en = 0, hpn = 0, add_n = 0, *lens = NULL;
    const char** tags = NULL;
    int count = 0, removed, k, o;
    JSValue ret = JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: tagInput(label, tags, opts?)");
    if (uib_check_opts(ctx, opts, "tagInput", keys, 3) || uib_str(ctx, argv[0], "tagInput", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "tagInput");
    if (!u || uib_items(ctx, u, argv[1], "tagInput", &tags, &lens, &count))
        goto done;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn) || uib_note_opts(ctx, opts, &err, &en, &help, &hpn))
        goto done;
    uib_note_begin(u, en, hpn);
    removed = dyn_ui_w_tags(u->ui, s, n, tags, lens, count, hint, hn, &add, &add_n);
    uib_note_end(u, err, en, help, hpn);
    if (removed < 0 && !add) {
        ret = JS_DupValue(ctx, argv[1]);
        goto done;
    }
    ret = JS_NewArray(ctx);
    for (k = 0, o = 0; k < count && !JS_IsException(ret); k++) {
        if (k == removed)
            continue;
        if (JS_DefinePropertyValueUint32(ctx, ret, (uint32_t)o++, JS_NewStringLen(ctx, tags[k], lens[k]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, ret);
            ret = JS_EXCEPTION;
        }
    }
    for (k = 0; add && (size_t)k < add_n && !JS_IsException(ret);) {
        size_t l = (unsigned char)add[k];
        if (JS_DefinePropertyValueUint32(ctx, ret, (uint32_t)o++, JS_NewStringLen(ctx, add + k + 1, l), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, ret);
            ret = JS_EXCEPTION;
        }
        k += (int)(1 + l);
    }
done:
    dyn_nat_free(tags);
    dyn_nat_free(lens);
    uib_note_free(ctx, err, help);
    if (hint)
        JS_FreeCString(ctx, hint);
    JS_FreeCString(ctx, s);
    return ret;
}

#endif
