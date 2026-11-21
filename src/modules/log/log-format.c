#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

static const struct {
    const char* name;
    size_t len;
    char padded[7];
} DYN_LV[countof(DYN_LV_NAME)] = {
    { "trace", 5, "trace" },
    { "debug", 5, "debug" },
    { "info", 4, "info " },
    { "warn", 4, "warn " },
    { "error", 5, "error" },
    { "fatal", 5, "fatal" },
    { "silent", 6, "silent" },
};

static void dyn_text_fields(JSContext* ctx, dyn_line_t* b, dyn_logger_t* L,
    JSValueConst v, int reserve_err)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return;
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    for (i = 0; i < p.len && !b->truncated; i++) {
        JSValue pv;
        const char* ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(pv)) {
            JS_FreeValue(ctx, pv);
            continue;
        }
        ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
        if (ks) {
            if (!dyn_key_reserved(L, ks, kn, reserve_err)) {
                dyn_line_esc_ctrl(b, ks, kn);
                dyn_line_put(b, "=", 1);
                dyn_text_value(ctx, b, pv);
                dyn_line_put(b, " ", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
}

static void dyn_emit_text(JSContext* ctx, dyn_logger_t* L, dyn_line_t* b,
    int64_t now, int magic, int has_err,
    JSValueConst err_v, int has_fields,
    JSValueConst fields_v, const char* msg, size_t msg_n)
{
    if (L->ts_mode == DYN_TS_EPOCH) {
        dyn_line_i64(b, now);
        dyn_line_put(b, " ", 1);
    } else if (L->ts_mode == DYN_TS_ISO) {
        int m;
        const char* iso = dyn_iso_sec(L, now, &m);
        char m3[3] = { (char)('0' + m / 100),
            (char)('0' + (m / 10) % 10),
            (char)('0' + m % 10) };
        dyn_line_put(b, iso, 19);
        dyn_line_put(b, ".", 1);
        dyn_line_put(b, m3, 3);
        dyn_line_put(b, "Z ", 2);
    }
    dyn_line_put(b, DYN_LV[magic].padded, 5);
    dyn_line_put(b, " ", 1);
    if (L->name) {
        dyn_line_esc_ctrl(b, L->name, strlen(L->name));
        dyn_line_puts(b, ": ");
    }
    dyn_line_esc_ctrl(b, msg ? msg : "", msg ? msg_n : 0);
    if (has_err) {
        const char* cs;
        JSValue f = JS_GetPropertyStr(ctx, err_v, "name");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_put(b, " ", 1);
        dyn_line_esc_ctrl(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
        if (cs)
            JS_FreeCString(ctx, cs);
        f = JS_GetPropertyStr(ctx, err_v, "message");
        cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
        JS_FreeValue(ctx, f);
        dyn_line_puts(b, ": ");
        dyn_line_esc_ctrl(b, cs ? cs : "", cs ? strlen(cs) : 0);
        if (cs)
            JS_FreeCString(ctx, cs);
    }
    if (L->base_text || has_fields)
        dyn_line_put(b, " ", 1);
    if (L->base_text)
        dyn_line_put(b, L->base_text, L->base_text_len);
    if (has_fields)
        dyn_text_fields(ctx, b, L, fields_v, has_err);
    if (b->truncated) {
        dyn_line_force(b, "...", 3);
        dyn_line_force(b, "\n", 1);
    } else if (!b->n || b->p[b->n - 1] == ' ') {
        if (b->n)
            b->p[b->n - 1] = '\n';
        else
            dyn_line_put(b, "\n", 1);
    } else {
        dyn_line_force(b, "\n", 1);
    }
}

JSValue dyn_log_emit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    dyn_line_t b;
    int has_err = 0, has_fields = 0, msg_idx = 0;
    const char* msg = NULL;
    const char* msg_owned = NULL;
    size_t msg_n = 0;
    int64_t now;

    if (!L)
        return JS_EXCEPTION;
    if (magic < L->level)
        return JS_UNDEFINED;
    if (L->sample_n > 1 && ++L->emit_count % L->sample_n != 0)
        return JS_UNDEFINED;
    now = (L->ts_mode != DYN_TS_NONE || (L->sink && (L->sink->max_size || L->sink->freq_ms))) ? log_now_ms() : 0;

    dyn_line_init(&b);
    if (argc > 0 && JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0])) {
        if (JS_IsError(ctx, argv[0])) {
            has_err = 1;
            msg_idx = 1;
            if (argc > 1 && JS_IsObject(argv[1]) && !JS_IsFunction(ctx, argv[1])) {
                has_fields = 1;
                msg_idx = 2;
            }
        } else {
            has_fields = 1;
            msg_idx = 1;
        }
    }
    if (argc > msg_idx && JS_IsString(argv[msg_idx])) {
        msg = dyn_str_bytes(ctx, &msg_owned, &msg_n, argv[msg_idx]);
    }

    if (L->fmt == DYN_FMT_TEXT) {
        dyn_emit_text(ctx, L, &b, now, magic, has_err,
            has_err ? argv[0] : JS_UNDEFINED,
            has_fields, has_fields ? argv[has_err ? 1 : 0] : JS_UNDEFINED,
            msg, msg_n);
    } else {
        dyn_line_put(&b, "{", 1);
        dyn_line_time(&b, L, now);
        dyn_line_puts(&b, "\"level\":\"");
        dyn_line_puts(&b, DYN_LV[magic].name);
        dyn_line_puts(&b, "\",");
        if (L->name) {
            dyn_line_puts(&b, "\"name\":");
            dyn_line_json_str(&b, L->name, strlen(L->name));
            dyn_line_put(&b, ",", 1);
        }
        if (L->base)
            dyn_line_put(&b, L->base, L->base_len);
        if (has_err)
            dyn_line_error(ctx, &b, argv[0]);
        if (has_fields) {
            if (dyn_line_fields(ctx, &b, L, argv[has_err ? 1 : 0], has_err) < 0)
                goto fail;
        }
        dyn_line_puts(&b, "\"msg\":");
        if (msg) {
            dyn_line_json_str(&b, msg, msg_n);
        } else if (argc > msg_idx && !JS_IsUndefined(argv[msg_idx])) {

            dyn_seen_t seen;
            seen.n = 0;
            dyn_line_value(ctx, &b, argv[msg_idx], 0, &seen);
        } else {
            dyn_line_json_str(&b, "", 0);
        }
        if (JS_HasUncatchableException(ctx))
            goto fail;
        if (!b.truncated) {
            dyn_line_puts(&b, "}\n");
        }
        if (b.truncated)
            dyn_line_json_repair(&b);
    }

    if (!JS_IsUndefined(L->dest_fn)) {
        JSValue line = JS_NewStringLen(ctx, b.p, b.n);
        JSValue res;
        if (JS_IsException(line))
            goto fail;
        res = JS_Call(ctx, L->dest_fn, JS_UNDEFINED, 1, &line);
        JS_FreeValue(ctx, line);
        if (JS_IsException(res))
            goto fail;
        JS_FreeValue(ctx, res);
    } else {
        dyn_sink_write(L->sink, now, b.p, b.n);
    }
    if (msg_owned)
        JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_UNDEFINED;
fail:
    if (msg_owned)
        JS_FreeCString(ctx, msg_owned);
    dyn_line_free(&b);
    return JS_EXCEPTION;
}

void dyn_text_value(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    if (JS_IsString(v)) {
        size_t n;
        const char* cs = NULL;
        const char* np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) {
            dyn_line_puts(b, "null");
            return;
        }
        dyn_line_esc_ctrl(b, np, n);
        if (cs)
            JS_FreeCString(ctx, cs);
        return;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        dyn_line_puts(b, "null");
        return;
    }
    if (JS_IsBool(v)) {
        dyn_line_puts(b, JS_ToBool(ctx, v) ? "true" : "false");
        return;
    }
    if (JS_IsNumber(v)) {
        dyn_line_number(ctx, b, v);
        return;
    }
    {
        dyn_line_t s2;
        dyn_seen_t seen;
        dyn_line_init(&s2);
        seen.n = 0;
        dyn_line_value(ctx, &s2, v, 0, &seen);
        dyn_line_put(b, s2.p, s2.n);
        dyn_line_free(&s2);
    }
}

#endif
