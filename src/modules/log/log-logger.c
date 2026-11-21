#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

static void dyn_logger_free(void* p)
{
    dyn_logger_t* L = (dyn_logger_t*)p;
    dyn_sink_unref(L->sink);
    free(L->name);
    free(L->base);
    free(L->base_text);
    free(L);
}

static void dyn_logger_free_rt(JSRuntime* rt, void* p)
{
    dyn_logger_t* L = (dyn_logger_t*)p;
    if (!L)
        return;
    JS_FreeValueRT(rt, L->dest_fn);
    L->dest_fn = JS_UNDEFINED;
    dyn_logger_free(p);
}

void dyn_logger_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_logger_t* L = (dyn_logger_t*)JS_GetOpaque(val, dyn_logger_class_id);
    if (L)
        dyn_logger_free_rt(rt, L);
}

void dyn_logger_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    dyn_logger_t* L = (dyn_logger_t*)JS_GetOpaque(val, dyn_logger_class_id);
    if (L && !JS_IsUndefined(L->dest_fn))
        JS_MarkValue(rt, L->dest_fn, mark_func);
}

static int dyn_level_of(const char* s)
{
    size_t i;
    for (i = 0; i < countof(DYN_LV_NAME); i++)
        if (strcmp(s, DYN_LV_NAME[i]) == 0)
            return (int)i;
    return -1;
}

static const char* dyn_hostname(void)
{
    static char host[256];
    static int have;
    pthread_mutex_lock(&dyn_log_mu);
    if (!have) {
        if (gethostname(host, sizeof host - 1) != 0)
            host[0] = '\0';
        host[sizeof host - 1] = '\0';
        have = 1;
    }
    pthread_mutex_unlock(&dyn_log_mu);
    return host;
}

dyn_logger_t* dyn_logger_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_logger_t*)dyn_plain_get(ctx, v, dyn_logger_class_id);
}

static int dyn_base_build(JSContext* ctx, JSValueConst v, dyn_logger_t* L,
    dyn_line_t* jb, dyn_line_t* tb)
{
    dyn_props_t p;
    uint32_t i;
    dyn_seen_t seen;
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    if (dyn_props_get(ctx, v, &p) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    seen.n = 0;
    seen.p[seen.n++] = JS_VALUE_GET_PTR(v);
    for (i = 0; i < p.len && !jb->truncated; i++) {
        JSValue pv = JS_GetProperty(ctx, v, p.tab[i].atom);
        const char* ks;
        size_t kn;
        int owned;
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
            if (!dyn_key_reserved(L, ks, kn, 0)) {
                dyn_line_json_str(jb, ks, kn);
                dyn_line_put(jb, ":", 1);
                dyn_line_value(ctx, jb, pv, 1, &seen);
                dyn_line_put(jb, ",", 1);
                dyn_line_esc_ctrl(tb, ks, kn);
                dyn_line_put(tb, "=", 1);
                dyn_text_value(ctx, tb, pv);
                dyn_line_put(tb, " ", 1);
            }
            if (owned)
                JS_FreeCString(ctx, ks);
        }
        JS_FreeValue(ctx, pv);
    }
    dyn_props_free(ctx, &p);
    return 0;
}

static const char* const dyn_logger_ctor_keys[] = {
    "level",
    "name",
    "timestamp",
    "format",
    "pid",
    "hostname",
    "base",
    "mkdir",
    "buffer",
    "sample",
    "rollover",
    "dest",
};

static const char* const dyn_logger_rollover_keys[] = {
    "size",
    "frequency",
    "count",
    "symlink",
};

static const char* const dyn_logger_child_keys[] = { "level" };

static int dyn_logger_opts(JSContext* ctx, JSValueConst opts, dyn_logger_t* L)
{
    JSValue v;
    const char* s;
    dyn_line_t jb, tb;

    L->level = DYN_LV_INFO;
    L->ts_mode = DYN_TS_EPOCH;
    L->fmt = DYN_FMT_JSON;
    L->name = NULL;
    L->base = NULL;
    L->base_len = 0;
    L->base_text = NULL;
    L->base_text_len = 0;
    L->pid_on = 0;
    L->host_on = 0;
    L->cached_sec = -1;
    L->cached_iso[0] = 0;
    if (!JS_IsObject(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, dyn_logger_ctor_keys,
            countof(dyn_logger_ctor_keys)))
        return -1;

    v = JS_GetPropertyStr(ctx, opts, "level");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
    } else {
        int lv;
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        lv = dyn_level_of(s);
        JS_FreeCString(ctx, s);
        if (lv < 0) {
            JS_ThrowRangeError(ctx, "new Logger({ level }): unknown level");
            return -1;
        }
        L->level = lv;
    }

    v = JS_GetPropertyStr(ctx, opts, "name");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            L->name = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "timestamp");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            if (strcmp(s, "iso") == 0)
                L->ts_mode = DYN_TS_ISO;
            else if (strcmp(s, "epoch") == 0 || strcmp(s, "epochMs") == 0)
                L->ts_mode = DYN_TS_EPOCH;
            else {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx,
                    "new Logger({ timestamp }): unknown timestamp");
                return -1;
            }
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsBool(v) && !JS_ToBool(ctx, v)) {
        L->ts_mode = DYN_TS_NONE;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "format");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        s = JS_ToCString(ctx, v);
        if (s) {
            if (strcmp(s, "text") == 0)
                L->fmt = DYN_FMT_TEXT;
            else if (strcmp(s, "json") != 0) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "new Logger({ format }): unknown format");
                return -1;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "pid");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        L->pid_on = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "hostname");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        L->host_on = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    dyn_line_init(&jb);
    dyn_line_init(&tb);
    if (L->pid_on) {
        dyn_line_puts(&jb, "\"pid\":");
        dyn_line_i64(&jb, (int64_t)getpid());
        dyn_line_put(&jb, ",", 1);
        dyn_line_puts(&tb, "pid=");
        dyn_line_i64(&tb, (int64_t)getpid());
        dyn_line_put(&tb, " ", 1);
    }
    if (L->host_on) {
        const char* h = dyn_hostname();
        dyn_line_puts(&jb, "\"hostname\":");
        dyn_line_json_str(&jb, h, strlen(h));
        dyn_line_put(&jb, ",", 1);
        dyn_line_puts(&tb, "hostname=");
        dyn_line_esc_ctrl(&tb, h, strlen(h));
        dyn_line_put(&tb, " ", 1);
    }
    v = JS_GetPropertyStr(ctx, opts, "base");
    if (JS_IsException(v) || dyn_base_build(ctx, v, L, &jb, &tb) < 0) {
        JS_FreeValue(ctx, v);
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (jb.n) {
        L->base = (char*)malloc(jb.n);
        if (!L->base) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(L->base, jb.p, jb.n);
        L->base_len = jb.n;
    }
    if (tb.n) {
        L->base_text = (char*)malloc(tb.n);
        if (!L->base_text) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(L->base_text, tb.p, tb.n);
        L->base_text_len = tb.n;
    }
    dyn_line_free(&jb);
    dyn_line_free(&tb);

    {
        char* dest = NULL;
        uint64_t max_size = 0;
        int64_t freq_ms = 0;
        int named_date = 0, keep_count = 0, use_symlink = 0, do_mkdir = 0;
        int has_rollover = 0;
        size_t bufcap = 0;

        v = JS_GetPropertyStr(ctx, opts, "mkdir");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v))
            do_mkdir = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "buffer");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (JS_IsBool(v)) {
            if (JS_ToBool(ctx, v))
                bufcap = 64u * 1024u;
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (d != 0 && (!(d >= 1) || d > 1048576.0)) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "Logger buffer: use true, 0 or 1..1048576 bytes");
                return -1;
            }
            bufcap = (size_t)d;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "sample");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!JS_IsNumber(v) || !(d >= 1 && d <= 4294967296.0) || d != (double)(int64_t)d) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx,
                    "new Logger({ sample }): sample must be an integer 1..2^32");
                return -1;
            }
            L->sample_n = (int64_t)d;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "rollover");
        if (JS_IsException(v))
            return -1;
        if (JS_IsObject(v)) {
            JSValue r;
            if (dyn_opts_strict(ctx, v, dyn_logger_rollover_keys,
                    countof(dyn_logger_rollover_keys))) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            r = JS_GetPropertyStr(ctx, v, "size");
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                if (dyn_parse_size(ctx, r, &max_size) < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "frequency");
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                if (dyn_parse_frequency(ctx, r, &freq_ms, &named_date) < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "count");
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (JS_IsNumber(r)) {
                int32_t c;
                if (JS_ToInt32(ctx, &c, r) || c < 0) {
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, v);
                    JS_ThrowRangeError(ctx, "Logger rollover.count: use 0..2^31-1");
                    return -1;
                }
                keep_count = c;
            } else if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "Logger rollover.count: must be a number");
                return -1;
            }
            JS_FreeValue(ctx, r);
            r = JS_GetPropertyStr(ctx, v, "symlink");
            if (JS_IsException(r)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!JS_IsUndefined(r) && !JS_IsNull(r))
                use_symlink = JS_ToBool(ctx, r);
            JS_FreeValue(ctx, r);
            has_rollover = 1;
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "new Logger({ rollover }): must be an object");
            return -1;
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "dest");
        if (JS_IsException(v))
            return -1;
        if (JS_IsString(v) || dyn_value_is_path(v)) {
            const char* cs = JS_ToCString(ctx, v);
            if (cs) {
                dest = strdup(cs);
                JS_FreeCString(ctx, cs);
            }
            if (!dest) {
                JS_FreeValue(ctx, v);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        } else if (JS_IsFunction(ctx, v)) {
            if (has_rollover) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx,
                    "new Logger({ rollover }): rollover needs a dest path");
                return -1;
            }
            L->dest_fn = JS_DupValue(ctx, v);
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                "new Logger({ dest }): dest must be a string path, a Path or a function");
            return -1;
        }
        JS_FreeValue(ctx, v);

        if (has_rollover && !dest) {
            JS_ThrowTypeError(ctx,
                "new Logger({ rollover }): rollover needs a dest path");
            return -1;
        }
        if (dest) {
            L->sink = dyn_sink_new(ctx, dest, max_size, freq_ms, named_date,
                keep_count, use_symlink, bufcap, do_mkdir);
            free(dest);
            if (!L->sink)
                return -1;
        }
    }
    return 0;
}

JSValue dyn_logger_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = (dyn_logger_t*)calloc(1, sizeof *L);
    if (!L)
        return JS_ThrowOutOfMemory(ctx);
    L->dest_fn = JS_UNDEFINED;
    L->sample_n = 1;
    if (dyn_logger_opts(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, L) < 0) {
        dyn_logger_free_rt(JS_GetRuntime(ctx), L);
        return JS_EXCEPTION;
    }
    return dyn_plain_wrap(ctx, new_target, dyn_logger_class_id, L, dyn_logger_free);
}

JSValue dyn_logger_child(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t *L = dyn_logger_of(ctx, this_val), *C;
    dyn_line_t jb, tb;
    int level;

    if (!L)
        return JS_EXCEPTION;
    level = L->level;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue v;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx,
                "Logger.child: options must be an object");
        if (dyn_opts_strict(ctx, argv[1], dyn_logger_child_keys,
                countof(dyn_logger_child_keys)))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            if (!s) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            level = dyn_level_of(s);
            JS_FreeCString(ctx, s);
            if (level < 0) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "Logger.child({ level }): unknown level");
            }
        } else if (!JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "Logger.child({ level }): level must be a string");
        }
        JS_FreeValue(ctx, v);
    }
    dyn_line_init(&jb);
    dyn_line_init(&tb);
    if (L->base)
        dyn_line_put(&jb, L->base, L->base_len);
    if (L->base_text)
        dyn_line_put(&tb, L->base_text, L->base_text_len);
    if (argc > 0 && dyn_base_build(ctx, argv[0], L, &jb, &tb) < 0) {
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return JS_EXCEPTION;
    }
    C = (dyn_logger_t*)calloc(1, sizeof *C);
    if (!C) {
        dyn_line_free(&jb);
        dyn_line_free(&tb);
        return JS_ThrowOutOfMemory(ctx);
    }
    C->level = level;
    C->ts_mode = L->ts_mode;
    C->fmt = L->fmt;
    C->pid_on = L->pid_on;
    C->host_on = L->host_on;
    C->cached_sec = -1;
    C->name = L->name ? strdup(L->name) : NULL;
    C->sink = L->sink;
    if (C->sink)
        C->sink->refs++;
    C->dest_fn = JS_IsUndefined(L->dest_fn) ? JS_UNDEFINED
                                            : JS_DupValue(ctx, L->dest_fn);
    C->sample_n = L->sample_n;
    C->emit_count = 0;
    if (jb.n) {
        C->base = (char*)malloc(jb.n);
        if (!C->base) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free_rt(JS_GetRuntime(ctx), C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base, jb.p, jb.n);
        C->base_len = jb.n;
    }
    if (tb.n) {
        C->base_text = (char*)malloc(tb.n);
        if (!C->base_text) {
            dyn_line_free(&jb);
            dyn_line_free(&tb);
            dyn_logger_free_rt(JS_GetRuntime(ctx), C);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(C->base_text, tb.p, tb.n);
        C->base_text_len = tb.n;
    }
    dyn_line_free(&jb);
    dyn_line_free(&tb);
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_logger_class_id, C, dyn_logger_free);
}

JSValue dyn_logger_enabled(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    const char* s;
    int lv;
    if (!L)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Logger.enabled(level): level must be a string");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    lv = dyn_level_of(s);
    JS_FreeCString(ctx, s);
    if (lv < 0)
        return JS_ThrowRangeError(ctx, "Logger.enabled(level): unknown level");
    return JS_NewBool(ctx, lv >= L->level);
}

JSValue dyn_logger_get_level(JSContext* ctx, JSValueConst this_val)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    if (!L)
        return JS_EXCEPTION;
    return JS_NewString(ctx, DYN_LV_NAME[L->level]);
}

JSValue dyn_logger_set_level(JSContext* ctx, JSValueConst this_val,
    JSValueConst val)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    const char* s;
    int lv;
    if (!L)
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s)
        return JS_EXCEPTION;
    lv = dyn_level_of(s);
    JS_FreeCString(ctx, s);
    if (lv < 0)
        return JS_ThrowRangeError(ctx, "Logger.level: unknown level");
    L->level = lv;
    return JS_UNDEFINED;
}

JSValue dyn_logger_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logger_t* L = dyn_logger_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!L)
        return JS_EXCEPTION;
    if (L->sink) {
        pthread_mutex_lock(&dyn_log_mu);
        dyn_sink_flush(L->sink);
        pthread_mutex_unlock(&dyn_log_mu);
    }
    return JS_UNDEFINED;
}

#endif
