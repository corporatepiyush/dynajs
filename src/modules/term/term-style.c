#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#define DYN_TERM_STYLE_MAX 256

int dyn_number_span(const char* s)
{
    char* end;
    if (!*s)
        return 0;
    if (*s == '-' || *s == '+') {
        unsigned char c = (unsigned char)s[1];
        if (!(c >= '0' && c <= '9') && c != '.' && (c | 0x20) != 'i' && (c | 0x20) != 'n')
            return 0;
    }
    (void)strtod(s, &end);
    return *end == '\0' && end != s;
}

static int dyn_number_text(const char* s, double* out)
{
    const char* p = s;
    char* end;
    double d;
    int digits = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'
        || *p == '\v')
        p++;
    if (*p == '+' || *p == '-')
        p++;
    while (*p >= '0' && *p <= '9') {
        p++;
        digits = 1;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            p++;
            digits = 1;
        }
    }
    if (!digits)
        return 0;
    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1;
        if (*q == '+' || *q == '-')
            q++;
        if (!(*q >= '0' && *q <= '9'))
            return 0;
        while (*q >= '0' && *q <= '9')
            q++;
        p = q;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'
        || *p == '\v')
        p++;
    if (*p)
        return 0;
    errno = 0;
    d = strtod(s, &end);
    (void)end;
    if (errno == ERANGE && (d >= HUGE_VAL || d <= -HUGE_VAL))
        return 0;
    *out = d;
    return 1;
}

static JSValue dyn_coerce(JSContext* ctx, const dyn_opt_t* o, const char* s)
{
    if (o->type == DYN_T_NUMBER) {
        double d;
        if (!dyn_number_text(s, &d)) {
            char* esc = dyn_arg_esc(s);
            JS_ThrowTypeError(ctx, "--%s expects a number, got \"%s\"",
                o->longname, esc ? esc : s);
            free(esc);
            return JS_EXCEPTION;
        }
        return JS_NewFloat64(ctx, d);
    }
    return JS_NewString(ctx, s);
}

static int dyn_store(JSContext* ctx, JSValueConst obj, const dyn_opt_t* o,
    JSValue val)
{
    if (JS_IsException(val))
        return -1;
    if (!o->variadic)
        return JS_DefinePropertyValueStr(ctx, obj, o->longname, val, JS_PROP_C_W_E) < 0 ? -1 : 0;
    {
        JSValue arr = JS_GetPropertyStr(ctx, obj, o->longname);
        int64_t len = 0;
        if (!JS_IsArray(ctx, arr)) {
            JS_FreeValue(ctx, arr);
            arr = JS_NewArray(ctx);
            if (JS_IsException(arr)) {
                JS_FreeValue(ctx, val);
                return -1;
            }
            if (JS_DefinePropertyValueStr(ctx, obj, o->longname, JS_DupValue(ctx, arr), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                JS_FreeValue(ctx, val);
                return -1;
            }
        } else if (dyn_js_len(ctx, arr, &len)) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, val);
            return -1;
        }
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)len, val, JS_PROP_C_W_E);
        JS_FreeValue(ctx, arr);
    }
    return 0;
}

int dyn_parse_one(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
    const char** av, int n, int i, JSValueConst positional,
    uint32_t* n_pos, int* no_more_opts)
{
    const char* a = av[i];
    size_t alen = strlen(a);

    if (*no_more_opts || a[0] != '-' || alen == 1) {
        JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
            JS_NewString(ctx, a), JS_PROP_C_W_E);
        return 1;
    }
    if (alen == 2 && a[1] == '-') {
        *no_more_opts = 1;
        return 1;
    }
    if (dyn_number_span(a)) {
        JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
            JS_NewString(ctx, a), JS_PROP_C_W_E);
        return 1;
    }
    if (a[1] == '-') {
        const char* eq = strchr(a + 2, '=');
        size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
        dyn_opt_t* o = dyn_find_long(c, a + 2, nlen);
        if (!o && nlen > 3 && memcmp(a + 2, "no-", 3) == 0) {
            o = dyn_find_long(c, a + 5, nlen - 3);
            if (o && o->type == DYN_T_BOOL) {
                if (eq) {
                    char* e = dyn_arg_esc(o->longname);
                    JS_ThrowTypeError(ctx,
                        "option \"--%s\" is a flag and takes no value",
                        e ? e : o->longname);
                    free(e);
                    return -1;
                }
                return JS_DefinePropertyValueStr(ctx, opts, o->longname, JS_FALSE, JS_PROP_C_W_E) < 0 ? -1 : 1;
            }
            o = NULL;
        }
        if (!o) {
            if (c->allow_unknown) {
                JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
                    JS_NewString(ctx, a), JS_PROP_C_W_E);
                return 1;
            }
            {
                char* esc = dyn_arg_esc(a);
                JS_ThrowTypeError(ctx, "unknown option \"%s\"", esc ? esc : a);
                free(esc);
            }
            return -1;
        }
        if (o->type == DYN_T_BOOL) {
            if (eq) {
                char* e = dyn_arg_esc(o->longname);
                JS_ThrowTypeError(ctx,
                    "option \"--%s\" is a flag and takes no value",
                    e ? e : o->longname);
                free(e);
                return -1;
            }
            return JS_DefinePropertyValueStr(ctx, opts, o->longname, JS_TRUE, JS_PROP_C_W_E) < 0 ? -1 : 1;
        }
        if (eq)
            return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, eq + 1)) < 0 ? -1 : 1;
        if (i + 1 >= n) {
            char* e = dyn_arg_esc(o->longname);
            JS_ThrowTypeError(ctx, "option \"--%s\" expects a value",
                e ? e : o->longname);
            free(e);
            return -1;
        }
        return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, av[i + 1])) < 0 ? -1 : 2;
    }
    {
        size_t k;
        for (k = 1; k < alen; k++) {
            dyn_opt_t* o = dyn_find_short(c, a[k]);
            if (!o) {
                if (c->allow_unknown) {
                    JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
                        JS_NewString(ctx, a), JS_PROP_C_W_E);
                    return 1;
                }
                {
                    char tok[2] = { a[k], 0 };
                    char* esc = dyn_arg_esc(tok);
                    JS_ThrowTypeError(ctx, "unknown option \"-%s\"",
                        esc ? esc : tok);
                    free(esc);
                }
                return -1;
            }
            if (o->type == DYN_T_BOOL) {
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, JS_TRUE, JS_PROP_C_W_E) < 0)
                    return -1;
                continue;
            }
            if (k + 1 < alen) {
                const char* val = a + k + 1;
                if (*val == '=')
                    val++;
                return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, val)) < 0 ? -1 : 1;
            }
            if (i + 1 >= n) {
                char* e = dyn_arg_esc(o->shortname);
                JS_ThrowTypeError(ctx, "option \"-%s\" expects a value",
                    e ? e : o->shortname);
                free(e);
                return -1;
            }
            return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, av[i + 1])) < 0 ? -1 : 2;
        }
    }
    return 1;
}

int dyn_env_apply(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
    const unsigned char* supplied)
{
    int k;
    for (k = 0; k < c->n_opts; k++) {
        const char* ev;
        dyn_opt_t* o = &c->opts[k];
        JSValue v;
        if (!o->env || supplied[k])
            continue;
        ev = getenv(o->env);
        if (!ev)
            continue;
        if (o->type == DYN_T_BOOL) {
            int b;
            if (ev[0] == '\0' || !strcasecmp(ev, "0") || !strcasecmp(ev, "false") || !strcasecmp(ev, "no") || !strcasecmp(ev, "n") || !strcasecmp(ev, "off"))
                b = 0;
            else
                b = 1;
            if (o->variadic) {
                JSValue arr = JS_NewArray(ctx);
                if (JS_IsException(arr))
                    return -1;
                JS_DefinePropertyValueUint32(ctx, arr, 0,
                    JS_NewBool(ctx, b), JS_PROP_C_W_E);
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, arr, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname,
                        JS_NewBool(ctx, b), JS_PROP_C_W_E)
                    < 0)
                    return -1;
            }
        } else if (o->type == DYN_T_NUMBER) {
            JSValue cv = dyn_coerce(ctx, o, ev);
            if (JS_IsException(cv)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                {
                    char* esc = dyn_arg_esc(ev);
                    JS_ThrowTypeError(ctx,
                        "env \"%s\" for --%s expects a number, got \"%s\"",
                        o->env, o->longname, esc ? esc : ev);
                    free(esc);
                }
                return -1;
            }
            if (o->variadic) {
                JSValue arr = JS_NewArray(ctx);
                if (JS_IsException(arr)) {
                    JS_FreeValue(ctx, cv);
                    return -1;
                }
                JS_DefinePropertyValueUint32(ctx, arr, 0, cv, JS_PROP_C_W_E);
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, arr, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, cv, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, cv);
                    return -1;
                }
            }
        } else {
            v = JS_NewString(ctx, ev);
            if (JS_IsException(v))
                return -1;
            if (o->variadic) {
                JSValue arr = JS_NewArray(ctx);
                if (JS_IsException(arr)) {
                    JS_FreeValue(ctx, v);
                    return -1;
                }
                JS_DefinePropertyValueUint32(ctx, arr, 0, v, JS_PROP_C_W_E);
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, arr, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_DefinePropertyValueStr(ctx, opts, o->longname, v, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
        }
    }
    return 0;
}

static const dyn_style_t* dyn_style_find(const char* n, size_t nl)
{
    size_t i;
    for (i = 0; i < countof(DYN_STYLES); i++)
        if (DYN_STYLES[i].name[0] == n[0] && strlen(DYN_STYLES[i].name) == nl && memcmp(DYN_STYLES[i].name, n, nl) == 0)
            return &DYN_STYLES[i];
    return NULL;
}

static int dyn_hex_val(char ch, int* out)
{
    int v = dyn_hex_digit((unsigned char)ch);
    if (v < 0)
        return 0;
    *out = v;
    return 1;
}

static int dyn_parse_256body(const char* p, size_t n, int* v)
{
    size_t i;
    int acc = 0;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    if (n > 3)
        return -1;
    for (i = 0; i < n; i++)
        acc = acc * 10 + (p[i] - '0');
    if (acc < 0 || acc > 255)
        return -1;
    *v = acc;
    return 1;
}

static int dyn_parse_rgbbody(const char* p, size_t n, int* r, int* g, int* b)
{
    int vals[3] = { 0, 0, 0 };
    int vi = 0, acc = 0, digits = 0;
    size_t i;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        char ch = p[i];
        if (ch >= '0' && ch <= '9') {
            acc = acc * 10 + (ch - '0');
            digits++;
            if (digits > 3 || acc > 255)
                return -1;
            continue;
        }
        if (ch == ',') {
            if (digits == 0)
                return 0;
            if (vi >= 3)
                return 0;
            vals[vi++] = acc;
            acc = 0;
            digits = 0;
            continue;
        }
        return 0;
    }
    if (digits == 0 || vi >= 3)
        return 0;
    vals[vi++] = acc;
    if (vi != 3)
        return 0;
    *r = vals[0];
    *g = vals[1];
    *b = vals[2];
    return 1;
}

static int dyn_style_extended(JSContext* ctx, const char* nm, size_t nl,
    int* is_bg, char* openbuf, size_t openlen,
    const char** close)
{
    if (nl > 4 && memcmp(nm, "256:", 4) == 0) {
        int v = 0, rc = dyn_parse_256body(nm + 4, nl - 4, &v);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: 256 color index out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[38;5;%dm", v);
        *is_bg = 0;
        *close = "\033[39m";
        return 1;
    }
    if (nl > 6 && memcmp(nm, "bg256:", 6) == 0) {
        int v = 0, rc = dyn_parse_256body(nm + 6, nl - 6, &v);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: 256 color index out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[48;5;%dm", v);
        *is_bg = 1;
        *close = "\033[49m";
        return 1;
    }
    if (nl == 7 && nm[0] == '#') {
        int h[6], i, ok = 1;
        for (i = 0; i < 6; i++)
            if (!dyn_hex_val(nm[1 + i], &h[i]))
                ok = 0;
        if (!ok)
            return 0;
        snprintf(openbuf, openlen, "\033[38;2;%d;%d;%dm",
            h[0] * 16 + h[1], h[2] * 16 + h[3], h[4] * 16 + h[5]);
        *is_bg = 0;
        *close = "\033[39m";
        return 1;
    }
    if (nl == 9 && memcmp(nm, "bg#", 3) == 0) {
        int h[6], i, ok = 1;
        for (i = 0; i < 6; i++)
            if (!dyn_hex_val(nm[3 + i], &h[i]))
                ok = 0;
        if (!ok)
            return 0;
        snprintf(openbuf, openlen, "\033[48;2;%d;%d;%dm",
            h[0] * 16 + h[1], h[2] * 16 + h[3], h[4] * 16 + h[5]);
        *is_bg = 1;
        *close = "\033[49m";
        return 1;
    }
    if (nl > 4 && memcmp(nm, "rgb:", 4) == 0) {
        int r = 0, g = 0, b = 0, rc = dyn_parse_rgbbody(nm + 4, nl - 4, &r, &g, &b);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: rgb component out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[38;2;%d;%d;%dm", r, g, b);
        *is_bg = 0;
        *close = "\033[39m";
        return 1;
    }
    if (nl > 6 && memcmp(nm, "bgRgb:", 6) == 0) {
        int r = 0, g = 0, b = 0, rc = dyn_parse_rgbbody(nm + 6, nl - 6, &r, &g, &b);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: rgb component out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[48;2;%d;%d;%dm", r, g, b);
        *is_bg = 1;
        *close = "\033[49m";
        return 1;
    }
    return 0;
}

static int dyn_color_on(void)
{
    const char* e;
    if ((e = getenv("FORCE_COLOR")) != NULL && *e)
        return !(strcmp(e, "0") == 0 || strcmp(e, "false") == 0);
    if ((e = getenv("NO_COLOR")) != NULL && *e)
        return 0;
    return isatty(1) == 1;
}

JSValue dyn_style_text(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char* txt = NULL;
    size_t txt_len = 0;
    const char** closes = NULL;
    const char* closes_inl[16];
    int64_t n_st = 1, k;
    int is_arr;
    int emit = dyn_color_on();

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "StyleText(style, text): two arguments required");
    is_arr = JS_IsArray(ctx, argv[0]);
    if (!is_arr && !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "StyleText(style, text): style must be a string or array");
    if (!JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "StyleText(style, text): text must be a string");
    txt = JS_ToCStringLen(ctx, &txt_len, argv[1]);
    if (!txt)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    if (is_arr) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToInt64(ctx, &n_st, lv)) {
            JS_FreeValue(ctx, lv);
            goto done;
        }
        JS_FreeValue(ctx, lv);
        if (n_st < 0)
            n_st = 0;
        if (n_st > DYN_TERM_STYLE_MAX) {
            JS_ThrowRangeError(ctx, "StyleText: more than %d styles",
                DYN_TERM_STYLE_MAX);
            goto done;
        }
    }
    if (n_st > 0 && n_st <= 16) {
        closes = closes_inl;
    } else if (n_st > 0) {
        if ((uint64_t)n_st > SIZE_MAX / sizeof *closes) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        closes = (const char**)malloc((size_t)n_st * sizeof *closes);
        if (!closes) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    if (!dyn_sb_reserve((void**)&b.p, &b.cap, txt_len + (emit ? (size_t)n_st * 48 : 0) + 1, 64)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (k = 0; k < n_st; k++) {
        JSValue sv = is_arr ? JS_GetPropertyUint32(ctx, argv[0], (uint32_t)k)
                            : JS_DupValue(ctx, argv[0]);
        size_t nl = 0;
        const char* nm = JS_IsException(sv) ? NULL : JS_ToCStringLen(ctx, &nl, sv);
        const dyn_style_t* st = nm ? dyn_style_find(nm, nl) : NULL;
        JS_FreeValue(ctx, sv);
        if (st) {
            JS_FreeCString(ctx, nm);
            closes[k] = st->off;
            if (emit)
                dyn_sb_puts(&b, st->on);
            continue;
        }
        if (nm) {
            char openbuf[32];
            const char* close = NULL;
            int is_bg = 0;
            int erc = dyn_style_extended(ctx, nm, nl, &is_bg, openbuf, sizeof openbuf, &close);
            (void)is_bg;
            if (erc == 1) {
                JS_FreeCString(ctx, nm);
                closes[k] = close;
                if (emit)
                    dyn_sb_puts(&b, openbuf);
                continue;
            }
            if (erc == -2) {
                JS_FreeCString(ctx, nm);
                goto done;
            }
        }
        {
            JS_ThrowTypeError(ctx, "StyleText: unknown style \"%s\"", nm ? nm : "?");
            if (nm)
                JS_FreeCString(ctx, nm);
            goto done;
        }
    }
    dyn_sb_put(&b, txt, txt_len);
    if (emit)
        for (k = n_st - 1; k >= 0; k--)
            dyn_sb_puts(&b, closes[k]);
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
done:
    if (closes != closes_inl)
        free(closes);
    dyn_sb_free(&b);
    JS_FreeCString(ctx, txt);
    return r;
}

#endif
