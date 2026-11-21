#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

static void dyn_cmd_free_rt(JSRuntime* rt, dyn_cmd_t* c)
{
    int i;
    for (i = 0; i < c->n_opts; i++) {
        free(c->opts[i].shortname);
        free(c->opts[i].longname);
        free(c->opts[i].desc);
        free(c->opts[i].placeholder);
        free(c->opts[i].env);
        JS_FreeValueRT(rt, c->opts[i].dflt);
    }
    for (i = 0; i < c->n_args; i++) {
        free(c->args[i].name);
        free(c->args[i].desc);
    }
    for (i = 0; i < c->n_subs; i++)
        JS_FreeValueRT(rt, c->subs[i]);
    JS_FreeValueRT(rt, c->action);
    free(c->name);
    free(c->desc);
    free(c->version);
    free(c);
}

void dyn_cmd_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_cmd_t* c = (dyn_cmd_t*)JS_GetOpaque(val, dyn_cmd_class_id);
    if (c)
        dyn_cmd_free_rt(rt, c);
}

void dyn_cmd_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    dyn_cmd_t* c = (dyn_cmd_t*)JS_GetOpaque(val, dyn_cmd_class_id);
    int i;
    if (!c)
        return;
    for (i = 0; i < c->n_opts; i++)
        JS_MarkValue(rt, c->opts[i].dflt, mark);
    for (i = 0; i < c->n_subs; i++)
        JS_MarkValue(rt, c->subs[i], mark);
    JS_MarkValue(rt, c->action, mark);
}

static dyn_cmd_t* dyn_cmd_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_cmd_t*)dyn_plain_get(ctx, v, dyn_cmd_class_id);
}

static int dyn_opt_spec(dyn_opt_t* o, const char* s, size_t n)
{
    size_t i = 0;
    o->shortname = o->longname = o->placeholder = NULL;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == ','))
            i++;
        if (i >= n)
            break;
        if (s[i] == '<' || s[i] == '[') {
            char close = (s[i] == '<') ? '>' : ']';
            size_t b = ++i;
            while (i < n && s[i] != close)
                i++;
            free(o->placeholder);
            o->placeholder = dyn_dup_span(s + b, i - b);
            if (i < n)
                i++;
        } else if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
            size_t b = i + 2;
            i = b;
            while (i < n && s[i] != ' ' && s[i] != ',')
                i++;
            free(o->longname);
            o->longname = dyn_dup_span(s + b, i - b);
        } else if (s[i] == '-') {
            size_t b = i + 1;
            i = b;
            while (i < n && s[i] != ' ' && s[i] != ',')
                i++;
            free(o->shortname);
            o->shortname = dyn_dup_span(s + b, i - b);
        } else {
            while (i < n && s[i] != ' ' && s[i] != ',')
                i++;
        }
    }
    return (o->longname && o->longname[0]) ? 0 : -1;
}

JSValue dyn_cmd_option(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    dyn_opt_t* o;
    const char *flags, *d = NULL;
    size_t fn;

    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.option(flags, desc, opts): flags must be a string");
    if (c->n_opts >= DYN_CMD_MAX_OPTS)
        return JS_ThrowRangeError(ctx, "Command.option: more than %d options", DYN_CMD_MAX_OPTS);
    flags = JS_ToCStringLen(ctx, &fn, argv[0]);
    if (!flags)
        return JS_EXCEPTION;
    o = &c->opts[c->n_opts];
    memset(o, 0, sizeof *o);
    o->dflt = JS_UNDEFINED;
    if (dyn_opt_spec(o, flags, fn) < 0) {
        JS_FreeCString(ctx, flags);
        free(o->shortname);
        free(o->longname);
        free(o->placeholder);
        free(o->desc);
        free(o->env);
        return JS_ThrowTypeError(ctx,
            "Command.option(flags): a long name (--name) is required");
    }
    JS_FreeCString(ctx, flags);
    {
        int i;
        if (!strcmp(o->longname, "version") && c->version) {
            JSValue ex = JS_ThrowTypeError(ctx,
                "Command.option(flags): \"--version\" collides with the automatic version flag");
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return ex;
        }
        for (i = 0; i < c->n_opts; i++) {
            if (!strcmp(c->opts[i].longname, o->longname)) {
                char* e = dyn_arg_esc(o->longname);
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"--%s\"",
                    e ? e : o->longname);
                free(e);
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                return ex;
            }
            if (o->shortname && c->opts[i].shortname
                && !strcmp(c->opts[i].shortname, o->shortname)) {
                char* e = dyn_arg_esc(o->shortname);
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"-%s\"",
                    e ? e : o->shortname);
                free(e);
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                return ex;
            }
        }
    }
    if (argc > 1 && JS_IsString(argv[1]) && (d = JS_ToCString(ctx, argv[1])) != NULL) {
        o->desc = dyn_dup_span(d, strlen(d));
        JS_FreeCString(ctx, d);
    }
    o->type = o->placeholder ? DYN_T_STRING : DYN_T_BOOL;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        static const char* const opt_keys[] = {
            "type", "required", "variadic", "default", "env"
        };
        JSValue v;
        if (!JS_IsObject(argv[2])) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return JS_ThrowTypeError(ctx,
                "Command.option(flags, desc, opts): opts must be an object");
        }
        if (dyn_opts_strict(ctx, argv[2], opt_keys, 5)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "type");
        if (JS_IsException(v)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return JS_EXCEPTION;
        }
        if (JS_IsString(v)) {
            const char* t = JS_ToCString(ctx, v);
            if (t) {
                if (!strcmp(t, "boolean"))
                    o->type = DYN_T_BOOL;
                else if (!strcmp(t, "number"))
                    o->type = DYN_T_NUMBER;
                else if (!strcmp(t, "string"))
                    o->type = DYN_T_STRING;
                else {
                    JS_FreeCString(ctx, t);
                    JS_FreeValue(ctx, v);
                    free(o->shortname);
                    free(o->longname);
                    free(o->placeholder);
                    free(o->desc);
                    free(o->env);
                    return JS_ThrowRangeError(ctx,
                        "Command.option({type}): type must be boolean, string or number");
                }
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "required");
        if (JS_IsException(v)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return JS_EXCEPTION;
        }
        o->required = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "variadic");
        if (JS_IsException(v)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            return JS_EXCEPTION;
        }
        o->variadic = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        o->dflt = JS_GetPropertyStr(ctx, argv[2], "default");
        if (JS_IsException(o->dflt)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            o->dflt = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "env");
        if (JS_IsException(v)) {
            free(o->shortname);
            free(o->longname);
            free(o->placeholder);
            free(o->desc);
            free(o->env);
            JS_FreeValue(ctx, o->dflt);
            o->dflt = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v)) {
            const char* e;
            size_t elen;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                JS_FreeValue(ctx, o->dflt);
                o->dflt = JS_UNDEFINED;
                return JS_ThrowTypeError(ctx,
                    "Command.option({env}): env must be a string");
            }
            e = JS_ToCStringLen(ctx, &elen, v);
            JS_FreeValue(ctx, v);
            if (!e) {
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                JS_FreeValue(ctx, o->dflt);
                o->dflt = JS_UNDEFINED;
                return JS_EXCEPTION;
            }
            if (elen == 0 || memchr(e, '\0', elen) != NULL || strchr(e, '=') != NULL) {
                JS_FreeCString(ctx, e);
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                JS_FreeValue(ctx, o->dflt);
                o->dflt = JS_UNDEFINED;
                return JS_ThrowTypeError(ctx,
                    "Command.option({env}): env must be a non-empty variable name without '='");
            }
            o->env = dyn_dup_span(e, elen);
            JS_FreeCString(ctx, e);
            if (!o->env) {
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                JS_FreeValue(ctx, o->dflt);
                o->dflt = JS_UNDEFINED;
                return JS_ThrowOutOfMemory(ctx);
            }
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    c->n_opts++;
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cmd_argument(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    dyn_arg_t* a;
    const char* s;
    size_t n, b, e;

    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.argument(spec, desc): spec must be a string");
    if (c->n_args >= DYN_CMD_MAX_ARGS)
        return JS_ThrowRangeError(ctx, "Command.argument: more than %d arguments", DYN_CMD_MAX_ARGS);
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    a = &c->args[c->n_args];
    memset(a, 0, sizeof *a);
    a->required = (n && s[0] == '<');
    b = (n && (s[0] == '<' || s[0] == '[')) ? 1 : 0;
    e = n;
    if (e > b && (s[e - 1] == '>' || s[e - 1] == ']'))
        e--;
    if (e >= b + 3 && memcmp(s + e - 3, "...", 3) == 0) {
        a->variadic = 1;
        e -= 3;
    }
    a->name = dyn_dup_span(s + b, e - b);
    JS_FreeCString(ctx, s);
    if (argc > 1 && JS_IsString(argv[1])) {
        const char* d = JS_ToCString(ctx, argv[1]);
        if (d) {
            a->desc = dyn_dup_span(d, strlen(d));
            JS_FreeCString(ctx, d);
        }
    }
    if (!a->name)
        return JS_ThrowOutOfMemory(ctx);
    c->n_args++;
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cmd_describe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    const char* d;
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.describe(text): text must be a string");
    d = JS_ToCString(ctx, argv[0]);
    if (!d)
        return JS_EXCEPTION;
    free(c->desc);
    c->desc = dyn_dup_span(d, strlen(d));
    JS_FreeCString(ctx, d);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cmd_command(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    dyn_cmd_t* sc;
    int i;

    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_cmd_of(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Command.command(sub): sub must be a Command");
    sc = dyn_cmd_of(ctx, argv[0]);
    if (sc && sc->name) {
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t* x = dyn_cmd_of(ctx, c->subs[i]);
            if (x && x->name && !strcmp(x->name, sc->name))
                return JS_ThrowTypeError(ctx,
                    "Command.command(sub): a subcommand named \"%s\" is already registered",
                    sc->name);
        }
    }
    if (sc == c)
        return JS_ThrowTypeError(ctx,
            "Command.command(sub): a command cannot be its own subcommand");
    if (c->n_subs >= DYN_CMD_MAX_SUBS)
        return JS_ThrowRangeError(ctx, "Command.command: more than %d subcommands", DYN_CMD_MAX_SUBS);
    c->subs[c->n_subs++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cmd_allow_unknown(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    c->allow_unknown = (argc < 1) ? 1 : JS_ToBool(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static int dyn_scan_arity(dyn_cmd_t* c, const char* a)
{
    size_t alen = strlen(a);
    if (a[0] != '-' || alen == 1)
        return 0;
    if (alen == 2 && a[1] == '-')
        return 0;
    if (dyn_number_span(a))
        return 0;
    if (a[1] == '-') {
        const char* eq = strchr(a + 2, '=');
        size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
        dyn_opt_t* o = dyn_find_long(c, a + 2, nlen);
        if (!o && nlen > 3 && memcmp(a + 2, "no-", 3) == 0) {
            o = dyn_find_long(c, a + 5, nlen - 3);
            if (o && o->type != DYN_T_BOOL)
                o = NULL;
        }
        if (!o)
            return 1;
        return (o->type == DYN_T_BOOL || eq) ? 1 : 2;
    }
    {
        size_t k;
        for (k = 1; k < alen; k++) {
            dyn_opt_t* o = dyn_find_short(c, a[k]);
            if (!o)
                return 1;
            if (o->type != DYN_T_BOOL)
                return (k + 1 < alen) ? 1 : 2;
        }
        return 1;
    }
}

static _Thread_local int dyn_cmd_sub_depth;

static JSValue dyn_default_copy(JSContext* ctx, JSValueConst v, int depth)
{
    uint32_t i, n = 0;
    JSValue r, e;
    JSPropertyEnum* tab = NULL;
    int64_t len = 0;

    if (depth <= 0 || !JS_IsObject(v))
        return JS_DupValue(ctx, v);
    if (JS_IsArray(ctx, v)) {
        if (dyn_js_len(ctx, v, &len))
            return JS_EXCEPTION;
        if (len < 0 || len > 65536)
            return JS_DupValue(ctx, v);
        r = JS_NewArray(ctx);
        if (JS_IsException(r))
            return r;
        for (i = 0; (int64_t)i < len; i++) {
            JSValue c;
            e = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, r);
                return e;
            }
            c = dyn_default_copy(ctx, e, depth - 1);
            JS_FreeValue(ctx, e);
            e = c;
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, r);
                return e;
            }
            if (JS_DefinePropertyValueUint32(ctx, r, i, e, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, r);
                return JS_EXCEPTION;
            }
        }
        return r;
    }
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        JSValue c;
        e = JS_GetProperty(ctx, v, tab[i].atom);
        if (!JS_IsException(e)) {
            c = dyn_default_copy(ctx, e, depth - 1);
            JS_FreeValue(ctx, e);
            e = c;
        }
        if (JS_IsException(e) || JS_DefinePropertyValue(ctx, r, tab[i].atom, e, JS_PROP_C_W_E) < 0) {
            JS_FreePropertyEnum(ctx, tab, n);
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return r;
}

static int dyn_cmd_check_required(JSContext* ctx, dyn_cmd_t* c,
    JSValueConst opts, uint32_t n_pos);

static JSValue dyn_cmd_dispatch(JSContext* ctx, dyn_cmd_t* c, const char** av,
    int n, JSValueConst opts, JSValueConst positional,
    uint32_t* n_pos, int* no_more)
{
    int i, k;
    if (c->n_subs == 0)
        return JS_UNDEFINED;
    for (i = 0; i < n; i++) {
        int arity;
        if (!strcmp(av[i], "--"))
            break;
        arity = dyn_scan_arity(c, av[i]);
        if (arity == 0) {
            for (k = 0; k < c->n_subs; k++) {
                dyn_cmd_t* sc = dyn_cmd_of(ctx, c->subs[k]);
                JSValue tail, subres, res;
                int j, p = 0;
                uint32_t t = 0;
                if (!sc || !sc->name || strcmp(sc->name, av[i]))
                    continue;
                while (p < i) {
                    int used = dyn_parse_one(ctx, c, opts, av, i, p, positional,
                        n_pos, no_more);
                    if (used < 0)
                        return JS_EXCEPTION;
                    p += used;
                }
                if (dyn_cmd_check_required(ctx, c, opts, *n_pos) < 0)
                    return JS_EXCEPTION;
                tail = JS_NewArray(ctx);
                if (JS_IsException(tail))
                    return JS_EXCEPTION;
                for (j = i + 1; j < n; j++)
                    JS_DefinePropertyValueUint32(ctx, tail, t++,
                        JS_NewString(ctx, av[j]), JS_PROP_C_W_E);
                if (dyn_cmd_sub_depth >= 64) {
                    JS_FreeValue(ctx, tail);
                    JS_ThrowRangeError(ctx,
                        "Command.parse: subcommand nesting is too deep");
                    return JS_EXCEPTION;
                }
                dyn_cmd_sub_depth++;
                subres = dyn_cmd_parse(ctx, c->subs[k], 1, (JSValueConst*)&tail);
                dyn_cmd_sub_depth--;
                JS_FreeValue(ctx, tail);
                if (JS_IsException(subres))
                    return JS_EXCEPTION;
                res = JS_NewObject(ctx);
                if (JS_IsException(res)) {
                    JS_FreeValue(ctx, subres);
                    return JS_EXCEPTION;
                }
                JS_DefinePropertyValueStr(ctx, res, "command", JS_NewString(ctx, av[i]), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "options", JS_DupValue(ctx, opts), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "arguments", JS_DupValue(ctx, positional), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "result", subres, JS_PROP_C_W_E);
                return res;
            }
            break;
        }
        i += arity - 1;
    }
    return JS_UNDEFINED;
}

static int dyn_version_scan(dyn_cmd_t* c, const char** av, int n, int start, int end)
{
    int i = start;
    while (i < end) {
        const char* a = av[i];
        int arity;
        if (!strcmp(a, "--"))
            break;
        arity = dyn_scan_arity(c, a);
        if (arity == 0) {
            if (!strcmp(a, "--version"))
                return 1;
            i++;
            continue;
        }
        if (!strcmp(a, "--version"))
            return 1;
        if (!strncmp(a, "--version=", 10))
            return 2;
        i += arity;
    }
    return 0;
}

static int dyn_sub_idx(JSContext* ctx, dyn_cmd_t* c, const char** av, int n)
{
    int i, k;
    if (c->n_subs == 0)
        return -1;
    for (i = 0; i < n; i++) {
        int arity;
        if (!strcmp(av[i], "--"))
            break;
        arity = dyn_scan_arity(c, av[i]);
        if (arity == 0) {
            for (k = 0; k < c->n_subs; k++) {
                dyn_cmd_t* sc = dyn_cmd_of(ctx, c->subs[k]);
                if (sc && sc->name && !strcmp(sc->name, av[i]))
                    return i;
            }
            return -1;
        }
        i += arity - 1;
    }
    return -1;
}

static void dyn_supplied_scan(dyn_cmd_t* c, const char** av, int n,
    unsigned char* supplied)
{
    int i;
    i = 0;
    while (i < n) {
        const char* a = av[i];
        size_t alen = strlen(a);
        int arity;
        if (!strcmp(a, "--"))
            break;
        arity = dyn_scan_arity(c, a);
        if (arity == 0) {
            i++;
            continue;
        }
        if (a[1] == '-') {
            const char* eq = strchr(a + 2, '=');
            size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
            dyn_opt_t* o = dyn_find_long(c, a + 2, nlen);
            if (!o && nlen > 3 && memcmp(a + 2, "no-", 3) == 0) {
                dyn_opt_t* neg = dyn_find_long(c, a + 5, nlen - 3);
                if (neg && neg->type == DYN_T_BOOL)
                    o = neg;
            }
            if (o)
                supplied[o - c->opts] = 1;
        } else {
            size_t k;
            for (k = 1; k < alen; k++) {
                dyn_opt_t* o = dyn_find_short(c, a[k]);
                if (!o)
                    break;
                supplied[o - c->opts] = 1;
                if (o->type != DYN_T_BOOL)
                    break;
            }
        }
        i += arity;
    }
}

static int dyn_cmd_check_required(JSContext* ctx, dyn_cmd_t* c,
    JSValueConst opts, uint32_t n_pos)
{
    int k;
    for (k = 0; k < c->n_opts; k++) {
        JSValue v;
        int missing;
        if (!c->opts[k].required)
            continue;
        v = JS_GetPropertyStr(ctx, opts, c->opts[k].longname);
        missing = JS_IsUndefined(v);
        JS_FreeValue(ctx, v);
        if (missing) {
            char* e = dyn_arg_esc(c->opts[k].longname);
            JS_ThrowTypeError(ctx, "required option \"--%s\" is missing",
                e ? e : c->opts[k].longname);
            free(e);
            return -1;
        }
    }
    for (k = 0; k < c->n_args; k++)
        if (c->args[k].required && (uint32_t)k >= n_pos)
            return JS_ThrowTypeError(ctx, "required argument \"%s\" is missing",
                       c->args[k].name),
                   -1;
    return 0;
}

JSValue dyn_cmd_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    JSValue arr = JS_UNDEFINED, res = JS_EXCEPTION, opts = JS_UNDEFINED;
    JSValue positional = JS_UNDEFINED, subres = JS_UNDEFINED, sargs = JS_UNDEFINED;
    const char** av = NULL;
    unsigned char supplied[DYN_CMD_MAX_OPTS];
    int64_t n64 = 0;
    int n = 0, i, k, no_more = 0;
    uint32_t n_pos = 0;

    if (!c)
        return JS_EXCEPTION;
    memset(supplied, 0, sizeof supplied);
    if (argc < 1) {
        JSValue g = JS_GetGlobalObject(ctx);
        if (!JS_IsException(g)) {
            sargs = JS_GetPropertyStr(ctx, g, "scriptArgs");
            JS_FreeValue(ctx, g);
            argv = (JSValueConst*)&sargs;
            argc = 1;
        }
    }
    if (argc < 1 || !JS_IsArray(ctx, argv[0])) {
        JS_FreeValue(ctx, sargs);
        return JS_ThrowTypeError(ctx, "Command.parse(argv): argv must be an array");
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToInt64(ctx, &n64, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, sargs);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n64 > 65536) {
        JS_ThrowRangeError(ctx, "Command.parse(argv): more than 65536 arguments");
        goto done;
    }
    n = (int)n64;
    av = (const char**)calloc((size_t)(n ? n : 1), sizeof(char*));
    if (!av) {
        JS_FreeValue(ctx, sargs);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t alen = 0;
        av[i] = JS_IsException(e) ? NULL : JS_ToCStringLen(ctx, &alen, e);
        JS_FreeValue(ctx, e);
        if (!av[i])
            goto done;
        if (strlen(av[i]) != alen) {
            JS_ThrowTypeError(ctx,
                "Command.parse: an argument contains a NUL byte");
            goto done;
        }
    }
    dyn_supplied_scan(c, av, n, supplied);
    opts = JS_NewObject(ctx);
    positional = JS_NewArray(ctx);
    if (JS_IsException(opts) || JS_IsException(positional))
        goto done;
    for (k = 0; k < c->n_opts; k++) {
        JSValue dv;
        if (JS_IsUndefined(c->opts[k].dflt))
            continue;
        dv = dyn_default_copy(ctx, c->opts[k].dflt, 4);
        if (JS_IsException(dv) || JS_DefinePropertyValueStr(ctx, opts, c->opts[k].longname, dv, JS_PROP_C_W_E) < 0)
            goto done;
    }
    if (dyn_env_apply(ctx, c, opts, supplied) < 0)
        goto done;

    if (c->version) {
        int sidx = dyn_sub_idx(ctx, c, av, n);
        int vend = (sidx >= 0) ? sidx : n;
        int hit = dyn_version_scan(c, av, n, 0, vend);
        if (hit == 2)
            goto done_throw_version_value;
        if (hit == 1) {
            res = JS_NewObject(ctx);
            if (JS_IsException(res))
                goto done;
            JS_DefinePropertyValueStr(ctx, res, "options", JS_DupValue(ctx, opts), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "arguments", JS_DupValue(ctx, positional), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "command", JS_NULL, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "version", JS_NewString(ctx, c->version), JS_PROP_C_W_E);
            goto done;
        }
        if (sidx < 0) {
            hit = dyn_version_scan(c, av, n, vend, n);
            if (hit == 2)
                goto done_throw_version_value;
            if (hit == 1) {
                res = JS_NewObject(ctx);
                if (JS_IsException(res))
                    goto done;
                JS_DefinePropertyValueStr(ctx, res, "options", JS_DupValue(ctx, opts), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "arguments", JS_DupValue(ctx, positional), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "command", JS_NULL, JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "version", JS_NewString(ctx, c->version), JS_PROP_C_W_E);
                goto done;
            }
        }
    }

    {
        JSValue sub = dyn_cmd_dispatch(ctx, c, av, n, opts, positional, &n_pos,
            &no_more);
        if (JS_IsException(sub))
            goto done;
        if (!JS_IsUndefined(sub)) {
            res = sub;
            goto done;
        }
    }

    if (c->version) {
        int hit = dyn_version_scan(c, av, n, 0, n);
        if (hit == 2)
            goto done_throw_version_value;
        if (hit == 1) {
            res = JS_NewObject(ctx);
            if (JS_IsException(res))
                goto done;
            JS_DefinePropertyValueStr(ctx, res, "options", JS_DupValue(ctx, opts), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "arguments", JS_DupValue(ctx, positional), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "command", JS_NULL, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "version", JS_NewString(ctx, c->version), JS_PROP_C_W_E);
            goto done;
        }
    }

    for (i = 0; i < n;) {
        int used = dyn_parse_one(ctx, c, opts, av, n, i, positional, &n_pos, &no_more);
        if (used < 0)
            goto done;
        i += used;
    }
    if (dyn_cmd_check_required(ctx, c, opts, n_pos) < 0)
        goto done;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto done;
    JS_DefinePropertyValueStr(ctx, res, "options", opts, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "arguments", positional, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "command", JS_NULL, JS_PROP_C_W_E);
    opts = positional = JS_UNDEFINED;
    if (!JS_IsUndefined(c->action)) {
        JSValue o = JS_GetPropertyStr(ctx, res, "options");
        JSValue a = JS_GetPropertyStr(ctx, res, "arguments");
        JSValue rval, args2[2];
        if (JS_IsException(o) || JS_IsException(a)) {
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        args2[0] = o;
        args2[1] = a;
        rval = JS_Call(ctx, c->action, this_val, 2, args2);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, a);
        if (JS_IsException(rval)) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        JS_DefinePropertyValueStr(ctx, res, "result", rval, JS_PROP_C_W_E);
    }
    goto done;
done_throw_version_value:
    JS_ThrowTypeError(ctx, "option \"--version\" is a flag and takes no value");
    goto done;
done:
    for (i = 0; i < n; i++)
        if (av[i])
            JS_FreeCString(ctx, av[i]);
    free(av);
    JS_FreeValue(ctx, opts);
    JS_FreeValue(ctx, positional);
    JS_FreeValue(ctx, subres);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, sargs);
    return res;
}

JSValue dyn_cmd_help(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    dyn_sb_t b;
    JSValue r;
    int i;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    dyn_sb_puts(&b, "Usage: ");
    dyn_sb_puts(&b, c->name ? c->name : "program");
    if (c->n_opts)
        dyn_sb_puts(&b, " [options]");
    if (c->n_subs)
        dyn_sb_puts(&b, " <command>");
    for (i = 0; i < c->n_args; i++) {
        dyn_sb_putc(&b, ' ');
        dyn_sb_putc(&b, c->args[i].required ? '<' : '[');
        dyn_sb_puts(&b, c->args[i].name);
        if (c->args[i].variadic)
            dyn_sb_puts(&b, "...");
        dyn_sb_putc(&b, c->args[i].required ? '>' : ']');
    }
    dyn_sb_putc(&b, '\n');
    if (c->desc) {
        dyn_sb_putc(&b, '\n');
        dyn_sb_puts(&b, c->desc);
        dyn_sb_putc(&b, '\n');
    }
    if (c->version) {
        dyn_sb_puts(&b, "\nVersion: ");
        dyn_sb_puts(&b, c->version);
        dyn_sb_putc(&b, '\n');
    }
    if (c->n_opts) {
        int colw = 0;
        for (i = 0; i < c->n_opts; i++) {
            size_t w = 8 + strlen(c->opts[i].longname)
                + (c->opts[i].placeholder ? 3 + strlen(c->opts[i].placeholder) : 0);
            if ((int)w > colw)
                colw = (int)w;
        }
        if (colw > 30)
            colw = 30;
        dyn_sb_puts(&b, "\nOptions:\n");
        for (i = 0; i < c->n_opts; i++) {
            size_t col;
            col = b.n;
            dyn_sb_puts(&b, "  ");
            if (c->opts[i].shortname) {
                dyn_sb_putc(&b, '-');
                dyn_sb_puts(&b, c->opts[i].shortname);
                dyn_sb_puts(&b, ", ");
            } else {
                dyn_sb_puts(&b, "    ");
            }
            dyn_sb_puts(&b, "--");
            dyn_sb_puts(&b, c->opts[i].longname);
            if (c->opts[i].placeholder) {
                dyn_sb_puts(&b, " <");
                dyn_sb_puts(&b, c->opts[i].placeholder);
                dyn_sb_putc(&b, '>');
            }
            if ((c->opts[i].desc && c->opts[i].desc[0]) || c->opts[i].required
                || !JS_IsUndefined(c->opts[i].dflt) || c->opts[i].env)
                while (!b.oom && b.n - col < (size_t)colw)
                    dyn_sb_putc(&b, ' ');
            {
                int tail = 0;
                if (c->opts[i].desc && c->opts[i].desc[0]) {
                    dyn_sb_puts(&b, "  ");
                    dyn_sb_puts(&b, c->opts[i].desc);
                    tail = 1;
                }
                if (c->opts[i].required) {
                    dyn_sb_puts(&b, tail ? " (required)" : "  (required)");
                    tail = 1;
                }
                if (!JS_IsUndefined(c->opts[i].dflt)) {
                    JSValue s = JS_JSONStringify(ctx, c->opts[i].dflt,
                        JS_UNDEFINED, JS_UNDEFINED);
                    if (JS_IsString(s)) {
                        const char* d = JS_ToCString(ctx, s);
                        if (d) {
                            dyn_sb_puts(&b, tail ? " (default: " : "  (default: ");
                            dyn_sb_puts(&b, d);
                            dyn_sb_putc(&b, ')');
                            JS_FreeCString(ctx, d);
                        }
                    }
                    JS_FreeValue(ctx, s);
                    tail = 1;
                }
                if (c->opts[i].env) {
                    dyn_sb_puts(&b, tail ? " (env: " : "  (env: ");
                    dyn_sb_puts(&b, c->opts[i].env);
                    dyn_sb_putc(&b, ')');
                    tail = 1;
                }
            }
            dyn_sb_putc(&b, '\n');
        }
    }
    if (c->n_subs) {
        int namew = 0;
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t* sc = dyn_cmd_of(ctx, c->subs[i]);
            int w;
            if (!sc || !sc->name)
                continue;
            w = 2 + (int)strlen(sc->name);
            if (w > namew)
                namew = w;
        }
        if (namew > 20)
            namew = 20;
        dyn_sb_puts(&b, "\nCommands:\n");
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t* sc = dyn_cmd_of(ctx, c->subs[i]);
            size_t col;
            if (!sc)
                continue;
            col = b.n;
            dyn_sb_puts(&b, "  ");
            dyn_sb_puts(&b, sc->name ? sc->name : "?");
            if (sc->desc && sc->desc[0]) {
                while (!b.oom && b.n - col < (size_t)namew)
                    dyn_sb_putc(&b, ' ');
                dyn_sb_putc(&b, ' ');
                dyn_sb_puts(&b, sc->desc);
            }
            dyn_sb_putc(&b, '\n');
        }
    }
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

JSValue dyn_cmd_get_name(JSContext* ctx, JSValueConst this_val)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->name ? c->name : "");
}

JSValue dyn_cmd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = (dyn_cmd_t*)calloc(1, sizeof *c);
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->action = JS_UNDEFINED;
    if (argc > 0 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) {
            c->name = dyn_dup_span(s, strlen(s));
            JS_FreeCString(ctx, s);
        }
    }
    return dyn_plain_wrap(ctx, new_target, dyn_cmd_class_id, c, NULL);
}

JSValue dyn_cmd_action(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.action(fn): fn must be a function");
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Command.action(fn): fn must be a function");
    JS_FreeValue(ctx, c->action);
    c->action = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_cmd_version(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_NewString(ctx, c->version ? c->version : "");
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.version(v): v must be a string");
    {
        int i;
        for (i = 0; i < c->n_opts; i++)
            if (!strcmp(c->opts[i].longname, "version"))
                return JS_ThrowTypeError(ctx,
                    "Command.version(v): \"--version\" is already a registered option");
    }
    {
        const char* s = JS_ToCString(ctx, argv[0]);
        char* dup;
        if (!s)
            return JS_EXCEPTION;
        dup = dyn_dup_span(s, strlen(s));
        JS_FreeCString(ctx, s);
        if (!dup)
            return JS_ThrowOutOfMemory(ctx);
        free(c->version);
        c->version = dup;
    }
    return JS_DupValue(ctx, this_val);
}

#endif
