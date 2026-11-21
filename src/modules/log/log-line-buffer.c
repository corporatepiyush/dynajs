#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

void dyn_line_i64(dyn_line_t* b, int64_t v)
{
    char tmp[24];
    size_t n = i64toa(tmp, v);
    dyn_line_put(b, tmp, n);
}

void dyn_line_time(dyn_line_t* b, dyn_logger_t* L, int64_t ms)
{
    if (L->ts_mode == DYN_TS_NONE)
        return;
    if (L->ts_mode == DYN_TS_EPOCH) {
        dyn_line_put(b, "\"time\":", 7);
        dyn_line_i64(b, ms);
        dyn_line_put(b, ",", 1);
        return;
    }
    {
        int m;
        const char* iso = dyn_iso_sec(L, ms, &m);
        char m3[3] = { (char)('0' + m / 100),
            (char)('0' + (m / 10) % 10),
            (char)('0' + m % 10) };
        dyn_line_put(b, "\"time\":\"", 8);
        dyn_line_put(b, iso, 19);
        dyn_line_put(b, ".", 1);
        dyn_line_put(b, m3, 3);
        dyn_line_put(b, "Z\",", 3);
    }
}

void dyn_line_number(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    int64_t i64;
    double d;
    char dbuf[64];
    JSDTOATempMem dm;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        dyn_line_i64(b, (int64_t)JS_VALUE_GET_INT(v));
        return;
    }
    if (!JS_ToInt64(ctx, &i64, v)) {
        double back;
        if (!JS_ToFloat64(ctx, &back, v) && back == (double)i64) {
            dyn_line_i64(b, i64);
            return;
        }
    }
    if (JS_ToFloat64(ctx, &d, v)) {
        dyn_line_puts(b, "null");
        return;
    }
    if (!isfinite(d)) {
        dyn_line_puts(b, "null");
        return;
    }
    {
        int n = js_dtoa(dbuf, d, 10, 0, JS_DTOA_FORMAT_FREE, &dm);
        dyn_line_put(b, dbuf, (size_t)n);
    }
}

void dyn_line_value(JSContext* ctx, dyn_line_t* b, JSValueConst v,
    int depth, dyn_seen_t* seen)
{
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
    if (JS_IsString(v)) {
        size_t n;
        const char* cs = NULL;
        const char* np = dyn_str_bytes(ctx, &cs, &n, v);
        if (!np) {
            dyn_line_puts(b, "null");
            return;
        }
        dyn_line_json_str(b, np, n);
        if (cs)
            JS_FreeCString(ctx, cs);
        return;
    }
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v)) {
        dyn_line_puts(b, "null");
        return;
    }
    {
        int k;
        for (k = 0; k < seen->n; k++)
            if (seen->p[k] == JS_VALUE_GET_PTR(v)) {
                dyn_line_puts(b, "\"[Circular]\"");
                return;
            }
    }
    if (depth >= DYN_LOG_MAX_DEPTH || seen->n >= DYN_LOG_MAX_DEPTH) {
        dyn_line_puts(b, "\"[deep]\"");
        return;
    }
    seen->p[seen->n++] = JS_VALUE_GET_PTR(v);
    if (JS_IsArray(ctx, v)) {
        int64_t len = 0, k;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv)) {
            len = 0;
            if (!JS_HasUncatchableException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, lv);
        if (len > 1000)
            len = 1000;
        dyn_line_put(b, "[", 1);
        for (k = 0; k < len; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
            if (k)
                dyn_line_put(b, ",", 1);
            if (JS_IsException(e)) {
                if (!JS_HasUncatchableException(ctx))
                    JS_FreeValue(ctx, JS_GetException(ctx));
                dyn_line_puts(b, "\"[unserializable]\"");
                break;
            }
            dyn_line_value(ctx, b, e, depth + 1, seen);
            JS_FreeValue(ctx, e);
            if (b->truncated)
                break;
        }
        dyn_line_put(b, "]", 1);
        seen->n--;
        return;
    }
    {
        dyn_props_t p;
        uint32_t i;
        int wrote = 0;
        if (dyn_props_get(ctx, v, &p) < 0) {
            if (!JS_HasUncatchableException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_line_puts(b, "\"[unserializable]\"");
            seen->n--;
            return;
        }
        dyn_line_put(b, "{", 1);
        for (i = 0; i < p.len && !b->truncated; i++) {
            JSValue pv = JS_GetProperty(ctx, v, p.tab[i].atom);
            const char* ks;
            size_t kn;
            int owned;
            if (JS_IsException(pv)) {
                if (JS_HasUncatchableException(ctx))
                    break;
                JS_FreeValue(ctx, JS_GetException(ctx));
                continue;
            }
            if (JS_IsUndefined(pv)) {
                JS_FreeValue(ctx, pv);
                continue;
            }
            ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
            if (ks) {
                if (wrote)
                    dyn_line_put(b, ",", 1);
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, depth + 1, seen);
                wrote = 1;
                if (owned)
                    JS_FreeCString(ctx, ks);
            }
            JS_FreeValue(ctx, pv);
        }
        dyn_line_put(b, "}", 1);
        dyn_props_free(ctx, &p);
        seen->n--;
    }
}

int dyn_line_fields(JSContext* ctx, dyn_line_t* b, dyn_logger_t* L,
    JSValueConst v, int reserve_err)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_line_puts(b, "\"fields\":\"[unserializable]\",");
        return 0;
    }
    for (i = 0; i < p.len && !b->truncated; i++) {
        JSValue pv;
        const char* ks;
        size_t kn;
        int owned;
        pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        if (JS_IsException(pv)) {
            if (JS_HasUncatchableException(ctx))
                break;
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
                dyn_line_json_str(b, ks, kn);
                dyn_line_put(b, ":", 1);
                dyn_line_value(ctx, b, pv, 1, &seen);
                dyn_line_put(b, ",", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

int dyn_line_error(JSContext* ctx, dyn_line_t* b, JSValueConst v)
{
    const char* cs;
    JSValue f;
    dyn_line_puts(b, "\"err\":{");
    f = JS_GetPropertyStr(ctx, v, "name");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, "\"type\":");
    dyn_line_json_str(b, cs ? cs : "Error", cs ? strlen(cs) : 5);
    if (cs)
        JS_FreeCString(ctx, cs);
    f = JS_GetPropertyStr(ctx, v, "message");
    cs = JS_IsException(f) ? NULL : JS_ToCString(ctx, f);
    JS_FreeValue(ctx, f);
    dyn_line_puts(b, ",\"message\":");
    dyn_line_json_str(b, cs ? cs : "", cs ? strlen(cs) : 0);
    if (cs)
        JS_FreeCString(ctx, cs);
    f = JS_GetPropertyStr(ctx, v, "stack");
    if (!JS_IsException(f) && !JS_IsUndefined(f)) {
        cs = JS_ToCString(ctx, f);
        if (cs) {
            dyn_line_puts(b, ",\"stack\":");
            dyn_line_json_str(b, cs, strlen(cs));
            JS_FreeCString(ctx, cs);
        }
    }
    JS_FreeValue(ctx, f);
    {
        dyn_props_t p;
        uint32_t i;
        dyn_seen_t seen;
        seen.n = 0;
        if (dyn_props_get(ctx, v, &p) >= 0) {
            for (i = 0; i < p.len && !b->truncated; i++) {
                JSValue pv;
                const char* ks;
                size_t kn;
                int owned;
                int skip = 0;
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    skip = (kn == 4 && memcmp(ks, "name", 4) == 0) || (kn == 7 && memcmp(ks, "message", 7) == 0) || (kn == 5 && memcmp(ks, "stack", 5) == 0);
                    if (owned)
                        JS_FreeCString(ctx, ks);
                }
                if (skip)
                    continue;
                pv = JS_GetProperty(ctx, v, p.tab[i].atom);
                if (JS_IsException(pv)) {
                    if (JS_HasUncatchableException(ctx))
                        break;
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    continue;
                }
                if (JS_IsUndefined(pv)) {
                    JS_FreeValue(ctx, pv);
                    continue;
                }
                ks = dyn_key_bytes(ctx, &kn, p.tab[i].atom, &owned);
                if (ks) {
                    dyn_line_put(b, ",", 1);
                    dyn_line_json_str(b, ks, kn);
                    dyn_line_put(b, ":", 1);
                    seen.n = 0;
                    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
                    dyn_line_value(ctx, b, pv, 1, &seen);
                    if (owned)
                        JS_FreeCString(ctx, ks);
                }
                JS_FreeValue(ctx, pv);
            }
            dyn_props_free(ctx, &p);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    dyn_line_puts(b, "},");
    return 0;
}

#endif
