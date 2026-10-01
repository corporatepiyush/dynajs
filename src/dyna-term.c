#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/select.h>
#if !defined(_WIN32)
#include <termios.h>
#endif

#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} dyn_sb_t;

static void dyn_sb_init(dyn_sb_t* b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
    b->oom = 0;
}
static void dyn_sb_free(dyn_sb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
}

static void dyn_sb_put(dyn_sb_t* b, const char* s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_sb_putc(dyn_sb_t* b, char c) { dyn_sb_put(b, &c, 1); }
static void dyn_sb_puts(dyn_sb_t* b, const char* s) { dyn_sb_put(b, s, strlen(s)); }

static int dyn_cli_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;
    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

#define DYN_CMD_MAX_OPTS 256
#define DYN_CMD_MAX_ARGS 64
#define DYN_CMD_MAX_SUBS 64

enum { DYN_T_BOOL,
    DYN_T_STRING,
    DYN_T_NUMBER };

typedef struct {
    char* shortname;
    char* longname;
    char* desc;
    char* placeholder;
    char* env;
    int type;
    int required;
    int variadic;
    JSValue dflt;
} dyn_opt_t;

typedef struct {
    char* name;
    char* desc;
    int required;
    int variadic;
} dyn_arg_t;

typedef struct dyn_cmd {
    char *name, *desc;
    char* version;
    JSValue action;
    dyn_opt_t opts[DYN_CMD_MAX_OPTS];
    int n_opts;
    dyn_arg_t args[DYN_CMD_MAX_ARGS];
    int n_args;
    JSValue subs[DYN_CMD_MAX_SUBS];
    int n_subs;
    int allow_unknown;
} dyn_cmd_t;

static JSClassID dyn_cmd_class_id;

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

static void dyn_cmd_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_cmd_t* c = (dyn_cmd_t*)JS_GetOpaque(val, dyn_cmd_class_id);
    if (c)
        dyn_cmd_free_rt(rt, c);
}

static void dyn_cmd_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
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

static JSClassDef dyn_cmd_class = {
    "Command",
    .finalizer = dyn_cmd_finalizer,
    .gc_mark = dyn_cmd_gc_mark,
};

static dyn_cmd_t* dyn_cmd_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_cmd_t*)dyn_plain_get(ctx, v, dyn_cmd_class_id);
}

static char* dyn_dup_span(const char* s, size_t n)
{
    char* r = (char*)malloc(n + 1);
    if (!r)
        return NULL;
    if (n)
        memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static char* dyn_arg_esc(const char* s)
{
    size_t n = strlen(s), i, j = 0;
    char* r = (char*)malloc(4 * n + 1);
    if (!r)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) {
            static const char hex[] = "0123456789abcdef";
            r[j++] = '\\';
            r[j++] = 'x';
            r[j++] = hex[ch >> 4];
            r[j++] = hex[ch & 15];
        } else {
            r[j++] = (char)ch;
        }
    }
    r[j] = 0;
    return r;
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

static JSValue dyn_cmd_option(JSContext* ctx, JSValueConst this_val,
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
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"--%s\"",
                    o->longname);
                free(o->shortname);
                free(o->longname);
                free(o->placeholder);
                free(o->desc);
                free(o->env);
                return ex;
            }
            if (o->shortname && c->opts[i].shortname
                && !strcmp(c->opts[i].shortname, o->shortname)) {
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"-%s\"",
                    o->shortname);
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
        if (dyn_cli_opts_strict(ctx, argv[2], opt_keys, 5)) {
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

static JSValue dyn_cmd_argument(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_cmd_describe(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_cmd_command(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_cmd_allow_unknown(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    c->allow_unknown = (argc < 1) ? 1 : JS_ToBool(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static dyn_opt_t* dyn_find_long(dyn_cmd_t* c, const char* s, size_t n)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (strlen(c->opts[i].longname) == n && memcmp(c->opts[i].longname, s, n) == 0)
            return &c->opts[i];
    return NULL;
}

static dyn_opt_t* dyn_find_short(dyn_cmd_t* c, char ch)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (c->opts[i].shortname && c->opts[i].shortname[0] == ch
            && c->opts[i].shortname[1] == 0)
            return &c->opts[i];
    return NULL;
}

static int dyn_number_span(const char* s);

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

static int dyn_number_span(const char* s)
{
    char* end;
    if (!*s)
        return 0;
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
        return JS_SetPropertyStr(ctx, obj, o->longname, val) < 0 ? -1 : 0;
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
            if (JS_SetPropertyStr(ctx, obj, o->longname, JS_DupValue(ctx, arr)) < 0) {
                JS_FreeValue(ctx, arr);
                JS_FreeValue(ctx, val);
                return -1;
            }
        } else {
            JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
            JS_ToInt64(ctx, &len, lv);
            JS_FreeValue(ctx, lv);
        }
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)len, val, JS_PROP_C_W_E);
        JS_FreeValue(ctx, arr);
    }
    return 0;
}

static JSValue dyn_cmd_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static JSValue dyn_default_copy(JSContext* ctx, JSValueConst v, int depth)
{
    uint32_t i, n = 0;
    JSValue r, lv, e;
    JSPropertyEnum* tab = NULL;
    int64_t len = 0;

    if (depth <= 0 || !JS_IsObject(v))
        return JS_DupValue(ctx, v);
    if (JS_IsArray(ctx, v)) {
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv))
            return lv;
        JS_ToInt64(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
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

static int dyn_parse_one(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
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
                    JS_ThrowTypeError(ctx,
                        "option \"--%s\" is a flag and takes no value",
                        o->longname);
                    return -1;
                }
                return JS_SetPropertyStr(ctx, opts, o->longname, JS_FALSE) < 0 ? -1 : 1;
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
                JS_ThrowTypeError(ctx,
                    "option \"--%s\" is a flag and takes no value",
                    o->longname);
                return -1;
            }
            return JS_SetPropertyStr(ctx, opts, o->longname, JS_TRUE) < 0 ? -1 : 1;
        }
        if (eq)
            return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, eq + 1)) < 0 ? -1 : 1;
        if (i + 1 >= n) {
            JS_ThrowTypeError(ctx, "option \"--%s\" expects a value", o->longname);
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
                if (JS_SetPropertyStr(ctx, opts, o->longname, JS_TRUE) < 0)
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
                JS_ThrowTypeError(ctx, "option \"-%s\" expects a value", o->shortname);
                return -1;
            }
            return dyn_store(ctx, opts, o, dyn_coerce(ctx, o, av[i + 1])) < 0 ? -1 : 2;
        }
    }
    return 1;
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
                subres = dyn_cmd_parse(ctx, c->subs[k], 1, (JSValueConst*)&tail);
                JS_FreeValue(ctx, tail);
                if (JS_IsException(subres))
                    return JS_EXCEPTION;
                res = JS_NewObject(ctx);
                if (JS_IsException(res)) {
                    JS_FreeValue(ctx, subres);
                    return JS_EXCEPTION;
                }
                JS_SetPropertyStr(ctx, res, "command", JS_NewString(ctx, av[i]));
                JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
                JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
                JS_SetPropertyStr(ctx, res, "result", subres);
                return res;
            }
            break;
        }
        i += arity - 1;
    }
    return JS_UNDEFINED;
}

static int dyn_env_apply(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
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
                if (JS_SetPropertyStr(ctx, opts, o->longname, arr) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_SetPropertyStr(ctx, opts, o->longname,
                        JS_NewBool(ctx, b))
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
                if (JS_SetPropertyStr(ctx, opts, o->longname, arr) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_SetPropertyStr(ctx, opts, o->longname, cv) < 0) {
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
                if (JS_SetPropertyStr(ctx, opts, o->longname, arr) < 0) {
                    JS_FreeValue(ctx, arr);
                    return -1;
                }
            } else {
                if (JS_SetPropertyStr(ctx, opts, o->longname, v) < 0) {
                    JS_FreeValue(ctx, v);
                    return -1;
                }
            }
        }
    }
    return 0;
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
        if (missing)
            return JS_ThrowTypeError(ctx, "required option \"--%s\" is missing",
                       c->opts[k].longname),
                   -1;
    }
    for (k = 0; k < c->n_args; k++)
        if (c->args[k].required && (uint32_t)k >= n_pos)
            return JS_ThrowTypeError(ctx, "required argument \"%s\" is missing",
                       c->args[k].name),
                   -1;
    return 0;
}

static JSValue dyn_cmd_parse(JSContext* ctx, JSValueConst this_val,
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
        av[i] = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!av[i])
            goto done;
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
        if (JS_IsException(dv) || JS_SetPropertyStr(ctx, opts, c->opts[k].longname, dv) < 0)
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
            JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
            JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
            JS_SetPropertyStr(ctx, res, "command", JS_NULL);
            JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
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
                JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
                JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
                JS_SetPropertyStr(ctx, res, "command", JS_NULL);
                JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
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
            JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
            JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
            JS_SetPropertyStr(ctx, res, "command", JS_NULL);
            JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
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
    JS_SetPropertyStr(ctx, res, "options", opts);
    JS_SetPropertyStr(ctx, res, "arguments", positional);
    JS_SetPropertyStr(ctx, res, "command", JS_NULL);
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
        JS_SetPropertyStr(ctx, res, "result", rval);
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

static JSValue dyn_cmd_help(JSContext* ctx, JSValueConst this_val,
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
                while (b.n - col < (size_t)colw)
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
                while (b.n - col < (size_t)namew)
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

static JSValue dyn_cmd_get_name(JSContext* ctx, JSValueConst this_val)
{
    dyn_cmd_t* c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->name ? c->name : "");
}

static JSValue dyn_cmd_ctor(JSContext* ctx, JSValueConst new_target,
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

static JSValue dyn_cmd_action(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_cmd_version(JSContext* ctx, JSValueConst this_val,
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

static const JSCFunctionListEntry dyn_cmd_proto[] = {
    JS_CFUNC_DEF("describe", 1, dyn_cmd_describe),
    JS_CFUNC_DEF("option", 1, dyn_cmd_option),
    JS_CFUNC_DEF("argument", 1, dyn_cmd_argument),
    JS_CFUNC_DEF("command", 1, dyn_cmd_command),
    JS_CFUNC_DEF("allowUnknown", 1, dyn_cmd_allow_unknown),
    JS_CFUNC_DEF("action", 1, dyn_cmd_action),
    JS_CFUNC_DEF("version", 0, dyn_cmd_version),
    JS_CFUNC_DEF("parse", 1, dyn_cmd_parse),
    JS_CFUNC_DEF("help", 0, dyn_cmd_help),
    JS_CGETSET_DEF("name", dyn_cmd_get_name, NULL),
};

typedef struct {
    const char* name;
    const char* on;
    const char* off;
} dyn_style_t;
static const dyn_style_t DYN_STYLES[] = {
    { "reset", "\033[0m", "\033[0m" },
    { "bold", "\033[1m", "\033[22m" },
    { "dim", "\033[2m", "\033[22m" },
    { "italic", "\033[3m", "\033[23m" },
    { "underline", "\033[4m", "\033[24m" },
    { "blink", "\033[5m", "\033[25m" },
    { "inverse", "\033[7m", "\033[27m" },
    { "hidden", "\033[8m", "\033[28m" },
    { "strikethrough", "\033[9m", "\033[29m" },
    { "doubleunderline", "\033[21m", "\033[24m" },
    { "black", "\033[30m", "\033[39m" },
    { "red", "\033[31m", "\033[39m" },
    { "green", "\033[32m", "\033[39m" },
    { "yellow", "\033[33m", "\033[39m" },
    { "blue", "\033[34m", "\033[39m" },
    { "magenta", "\033[35m", "\033[39m" },
    { "cyan", "\033[36m", "\033[39m" },
    { "white", "\033[37m", "\033[39m" },
    { "bgBlack", "\033[40m", "\033[49m" },
    { "bgRed", "\033[41m", "\033[49m" },
    { "bgGreen", "\033[42m", "\033[49m" },
    { "bgYellow", "\033[43m", "\033[49m" },
    { "bgBlue", "\033[44m", "\033[49m" },
    { "bgMagenta", "\033[45m", "\033[49m" },
    { "bgCyan", "\033[46m", "\033[49m" },
    { "bgWhite", "\033[47m", "\033[49m" },
    { "framed", "\033[51m", "\033[54m" },
    { "overlined", "\033[53m", "\033[55m" },
    { "gray", "\033[90m", "\033[39m" },
    { "grey", "\033[90m", "\033[39m" },
    { "redBright", "\033[91m", "\033[39m" },
    { "greenBright", "\033[92m", "\033[39m" },
    { "yellowBright", "\033[93m", "\033[39m" },
    { "blueBright", "\033[94m", "\033[39m" },
    { "magentaBright", "\033[95m", "\033[39m" },
    { "cyanBright", "\033[96m", "\033[39m" },
    { "whiteBright", "\033[97m", "\033[39m" },
    { "bgGray", "\033[100m", "\033[49m" },
    { "bgGrey", "\033[100m", "\033[49m" },
    { "bgRedBright", "\033[101m", "\033[49m" },
    { "bgGreenBright", "\033[102m", "\033[49m" },
    { "bgYellowBright", "\033[103m", "\033[49m" },
    { "bgBlueBright", "\033[104m", "\033[49m" },
    { "bgMagentaBright", "\033[105m", "\033[49m" },
    { "bgCyanBright", "\033[106m", "\033[49m" },
    { "bgWhiteBright", "\033[107m", "\033[49m" },
};

static const dyn_style_t* dyn_style_find(const char* n, size_t nl)
{
    size_t i;
    for (i = 0; i < countof(DYN_STYLES); i++)
        if (strlen(DYN_STYLES[i].name) == nl && memcmp(DYN_STYLES[i].name, n, nl) == 0)
            return &DYN_STYLES[i];
    return NULL;
}

static int dyn_hex_val(char ch, int* out)
{
    if (ch >= '0' && ch <= '9') {
        *out = ch - '0';
        return 1;
    }
    if (ch >= 'a' && ch <= 'f') {
        *out = ch - 'a' + 10;
        return 1;
    }
    if (ch >= 'A' && ch <= 'F') {
        *out = ch - 'A' + 10;
        return 1;
    }
    return 0;
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

static JSValue dyn_style_text(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char* txt = NULL;
    size_t txt_len = 0;
    const char** closes = NULL;
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
    }
    if (n_st > 0) {
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
    free(closes);
    dyn_sb_free(&b);
    JS_FreeCString(ctx, txt);
    return r;
}

static JSValue dyn_styles_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    (void)argc;
    (void)argv;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < countof(DYN_STYLES); i++)
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
            JS_NewString(ctx, DYN_STYLES[i].name), JS_PROP_C_W_E);
    return arr;
}

static int dyn_fd_of(JSContext* ctx, int argc, JSValueConst* argv,
    int32_t* pfd, const char* who)
{
    int32_t fd = 1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsNumber(argv[0])) {
            JS_ThrowTypeError(ctx, "%s(fd): fd must be a number", who);
            return -1;
        }
        if (JS_ToInt32(ctx, &fd, argv[0]))
            return -1;
    }
    *pfd = fd;
    return 0;
}

static JSValue dyn_is_tty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t fd;
    if (dyn_fd_of(ctx, argc, argv, &fd, "IsTTY"))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd) == 1);
}

static JSValue dyn_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* e = getenv("COLUMNS");
    (void)argc;
    (void)argv;
    if (e && *e) {
        char* end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v > 0 && v < 100000)
            return JS_NewInt32(ctx, (int32_t)v);
    }
    return JS_NewInt32(ctx, 80);
}

static JSValue dyn_color_depth(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* e;
    int32_t fd;
    if (dyn_fd_of(ctx, argc, argv, &fd, "ColorDepth"))
        return JS_EXCEPTION;
    if ((e = getenv("FORCE_COLOR")) != NULL && *e) {
        if (!strcmp(e, "0") || !strcmp(e, "false"))
            return JS_NewInt32(ctx, 0);
        if (!strcmp(e, "2"))
            return JS_NewInt32(ctx, 8);
        if (!strcmp(e, "3"))
            return JS_NewInt32(ctx, 24);
        return JS_NewInt32(ctx, 4);
    }
    if ((e = getenv("NO_COLOR")) != NULL && *e)
        return JS_NewInt32(ctx, 0);
    if (isatty(fd) != 1)
        return JS_NewInt32(ctx, 0);
    if ((e = getenv("COLORTERM")) != NULL
        && (strstr(e, "truecolor") || strstr(e, "24bit")))
        return JS_NewInt32(ctx, 24);
    if ((e = getenv("TERM")) != NULL && strstr(e, "256color"))
        return JS_NewInt32(ctx, 8);
    if (e && !strcmp(e, "dumb"))
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, 4);
}

#define DYN_INQ_SIZE 16384u
static unsigned char dyn_inq[DYN_INQ_SIZE];
static unsigned int dyn_inq_head, dyn_inq_n;

static void dyn_inq_unread(unsigned char c)
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq_head = (dyn_inq_head + DYN_INQ_SIZE - 1) & (DYN_INQ_SIZE - 1);
    dyn_inq[dyn_inq_head] = c;
    dyn_inq_n++;
}

static void dyn_inq_push(unsigned char c)
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq[(dyn_inq_head + dyn_inq_n) & (DYN_INQ_SIZE - 1)] = c;
    dyn_inq_n++;
}

static int dyn_inq_pop(void)
{
    unsigned char c;
    if (!dyn_inq_n)
        return -1;
    c = dyn_inq[dyn_inq_head];
    dyn_inq_head = (dyn_inq_head + 1) & (DYN_INQ_SIZE - 1);
    dyn_inq_n--;
    return c;
}

#if !defined(_WIN32)
typedef struct termios dyn_termios_t;
static int dyn_raw_on(dyn_termios_t* saved)
{
    struct termios t;
    if (tcgetattr(0, saved) != 0)
        return 0;
    t = *saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | INLCR | IGNCR | BRKINT);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(0, TCSADRAIN, &t) == 0 ? 1 : 0;
}
static void dyn_raw_off(dyn_termios_t* saved, int on)
{
    if (!on)
        return;
    for (;;) {
        unsigned char buf[512];
        fd_set rfds;
        struct timeval tv;
        size_t room = DYN_INQ_SIZE - dyn_inq_n;
        ssize_t r, i;
        if (room == 0)
            break;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        r = read(0, buf, room < sizeof buf ? room : sizeof buf);
        if (r <= 0)
            break;
        for (i = 0; i < r; i++)
            dyn_inq_push(buf[i]);
    }
    tcsetattr(0, TCSADRAIN, saved);
}
#else
typedef int dyn_termios_t;
static int dyn_raw_on(dyn_termios_t* saved)
{
    (void)saved;
    return 0;
}
static void dyn_raw_off(dyn_termios_t* saved, int on)
{
    (void)saved;
    (void)on;
}
#endif

static int dyn_byte_at(int fd, unsigned char* c, int timeout_ms)
{
    ssize_t r;
    int q = dyn_inq_pop();
    if (q >= 0) {
        *c = (unsigned char)q;
        return 1;
    }
    if (timeout_ms >= 0) {
        fd_set rfds;
        struct timeval tv;
        int s;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        s = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (s == 0)
            return -1;
        if (s < 0)
            return (errno == EINTR) ? dyn_byte_at(fd, c, timeout_ms) : 0;
    }
    do
        r = read(fd, c, 1);
    while (r < 0 && errno == EINTR);
    return r == 1 ? 1 : 0;
}

#define DYN_KEY_ESC_MS 50

typedef struct {
    int eof;
    char seq[16];
    int seqlen;
    char chbuf[8];
    const char* name;
    int ctrl, meta, shift;
} dyn_key_t;

static void dyn_key_csi(dyn_key_t* k, char* params, unsigned char f)
{
    int p0 = 0, mod = 0;
    const char* semi;
    if (params[0])
        p0 = (int)strtol(params, NULL, 10);
    semi = strchr(params, ';');
    if (semi && semi[1])
        mod = (int)strtol(semi + 1, NULL, 10) - 1;
    if (mod > 0) {
        k->shift |= (mod & 1) != 0;
        k->meta |= (mod & 2) != 0;
        k->ctrl |= (mod & 4) != 0;
    }
    switch (f) {
    case 'A':
        k->name = "up";
        break;
    case 'B':
        k->name = "down";
        break;
    case 'C':
        k->name = "right";
        break;
    case 'D':
        k->name = "left";
        break;
    case 'H':
        k->name = "home";
        break;
    case 'F':
        k->name = "end";
        break;
    case 'Z':
        k->name = "tab";
        k->shift = 1;
        break;
    case '~':
        switch (p0) {
        case 1:
            k->name = "home";
            break;
        case 2:
            k->name = "insert";
            break;
        case 3:
            k->name = "delete";
            break;
        case 4:
            k->name = "end";
            break;
        case 5:
            k->name = "pageup";
            break;
        case 6:
            k->name = "pagedown";
            break;
        }
        break;
    }
}

static void dyn_key_base(dyn_key_t* k, int fd, unsigned char b)
{
    if (b == '\r' || b == '\n') {
        k->name = "enter";
        return;
    }
    if (b == '\t') {
        k->name = "tab";
        return;
    }
    if (b == 0x7f || b == 0x08) {
        k->name = "backspace";
        return;
    }
    if (b == 0x1b) {
        k->name = "escape";
        return;
    }
    if (b < 0x20) {
        static const char map[] = "@abcdefghijklmnopqrstuvwxyz[\\]^_";
        k->ctrl = 1;
        k->chbuf[0] = map[b];
        k->chbuf[1] = 0;
        k->name = k->chbuf;
        return;
    }
    if (b < 0x80) {
        k->chbuf[0] = (char)b;
        k->chbuf[1] = 0;
        k->name = k->chbuf;
        return;
    }
    {
        int need = (b >= 0xf0) ? 3 : (b >= 0xe0) ? 2
            : (b >= 0xc0)                        ? 1
                                                 : 0;
        int i;
        if (need == 0)
            return;
        k->chbuf[0] = (char)b;
        for (i = 1; i <= need; i++) {
            unsigned char c;
            if (dyn_byte_at(fd, &c, DYN_KEY_ESC_MS) != 1)
                return;
            if ((c & 0xc0) != 0x80) {
                dyn_inq_unread(c);
                return;
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(c);
                return;
            }
            k->seq[k->seqlen++] = (char)c;
            k->chbuf[i] = (char)c;
        }
        k->chbuf[need + 1] = 0;
        k->name = k->chbuf;
    }
}

static int dyn_key_read(int fd, dyn_key_t* k)
{
    unsigned char b = 0;

    memset(k, 0, sizeof *k);
    if (dyn_byte_at(fd, &b, -1) != 1) {
        k->eof = 1;
        return 0;
    }
    k->seq[k->seqlen++] = (char)b;
    for (;;) {
        if (b != 0x1b) {
            dyn_key_base(k, fd, b);
            return 1;
        }
        {
            unsigned char b1;
            if (dyn_byte_at(fd, &b1, DYN_KEY_ESC_MS) != 1) {
                k->name = "escape";
                return 1;
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(b1);
                k->name = "escape";
                return 1;
            }
            k->seq[k->seqlen++] = (char)b1;
            if (b1 == 0x1b) {
                k->meta = 1;
                b = b1;
                continue;
            }
            if (b1 == '[' || b1 == 'O') {
                char params[12];
                int np = 0;
                unsigned char f;
                for (;;) {
                    if (dyn_byte_at(fd, &f, DYN_KEY_ESC_MS) != 1)
                        return 1;
                    if (k->seqlen >= (int)sizeof k->seq - 1) {
                        dyn_inq_unread(f);
                        return 1;
                    }
                    k->seq[k->seqlen++] = (char)f;
                    if (f >= 0x30 && f <= 0x3f && np < (int)sizeof params - 1) {
                        params[np++] = (char)f;
                        continue;
                    }
                    break;
                }
                params[np] = 0;
                if (b1 == 'O') {
                    switch (f) {
                    case 'A':
                        k->name = "up";
                        break;
                    case 'B':
                        k->name = "down";
                        break;
                    case 'C':
                        k->name = "right";
                        break;
                    case 'D':
                        k->name = "left";
                        break;
                    case 'H':
                        k->name = "home";
                        break;
                    case 'F':
                        k->name = "end";
                        break;
                    }
                    return 1;
                }
                dyn_key_csi(k, params, f);
                return 1;
            }
            k->meta = 1;
            dyn_key_base(k, fd, b1);
            return 1;
        }
    }
}

static JSValue dyn_key_obj(JSContext* ctx, const dyn_key_t* k)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "name",
        k->name ? JS_NewString(ctx, k->name) : JS_NULL);
    JS_SetPropertyStr(ctx, o, "sequence", JS_NewStringLen(ctx, k->seq, k->seqlen));
    JS_SetPropertyStr(ctx, o, "ctrl", JS_NewBool(ctx, k->ctrl));
    JS_SetPropertyStr(ctx, o, "meta", JS_NewBool(ctx, k->meta));
    JS_SetPropertyStr(ctx, o, "shift", JS_NewBool(ctx, k->shift));
    return o;
}

static JSValue dyn_keypress_fn(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_key_t k;
    dyn_termios_t saved;
    int raw;
    (void)this_val;
    (void)argc;
    (void)argv;
    raw = dyn_raw_on(&saved);
    dyn_key_read(0, &k);
    dyn_raw_off(&saved, raw);
    if (k.eof)
        return JS_NULL;
    return dyn_key_obj(ctx, &k);
}

#define DYN_LINE_MAX (1 << 20)

static int dyn_read_line(int fd, dyn_sb_t* b)
{
    unsigned char c;
    int over = 0;
    for (;;) {
        if (dyn_byte_at(fd, &c, -1) != 1)
            break;
        if (c == '\n') {
            if (over)
                return -1;
            if (b->n && b->p[b->n - 1] == '\r')
                b->n--;
            if (b->n > DYN_LINE_MAX) {
                b->n = 0;
                return -1;
            }
            return 1;
        }
        if (over)
            continue;
        if (b->n >= DYN_LINE_MAX + 1) {
            over = 1;
            b->n = 0;
            continue;
        }
        dyn_sb_putc(b, (char)c);
        if (b->oom)
            return -1;
    }
    if (over || b->n > DYN_LINE_MAX) {
        b->n = 0;
        return -1;
    }
    return b->n ? 1 : 0;
}

static JSValue dyn_prompt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *msg, *dflt = NULL;
    size_t mlen = 0, dlen = 0;
    dyn_sb_t line;
    JSValue r;
    int lr;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "prompt(message, opts): message must be a string");
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const pkeys[] = { "default" };
        JSValue v;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "prompt(message, opts): opts must be an object");
        if (dyn_cli_opts_strict(ctx, argv[1], pkeys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "default");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "prompt({default}): default must be a string");
            }
            dflt = JS_ToCStringLen(ctx, &dlen, v);
            JS_FreeValue(ctx, v);
            if (!dflt)
                return JS_EXCEPTION;
        }
    }
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg) {
        if (dflt)
            JS_FreeCString(ctx, dflt);
        return JS_EXCEPTION;
    }
    dyn_sb_init(&line);
    if (mlen && write(1, msg, mlen) < 0) {
    }
    lr = dyn_read_line(0, &line);
    if (lr < 0) {
        JS_ThrowRangeError(ctx, "prompt: the answer exceeds %d bytes without a newline",
            DYN_LINE_MAX);
        r = JS_EXCEPTION;
    } else if (line.n == 0 && dflt) {
        r = JS_NewStringLen(ctx, dflt, dlen);
    } else {
        r = JS_NewStringLen(ctx, line.p ? line.p : "", line.n);
    }
    dyn_sb_free(&line);
    JS_FreeCString(ctx, msg);
    if (dflt)
        JS_FreeCString(ctx, dflt);
    return r;
}

static JSValue dyn_confirm(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* msg;
    size_t mlen = 0;
    dyn_sb_t line;
    int yes = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "confirm(message): message must be a string");
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg)
        return JS_EXCEPTION;
    dyn_sb_init(&line);
    if (mlen && write(1, msg, mlen) < 0) {
    }
    if (dyn_read_line(0, &line) < 0) {
        JS_FreeCString(ctx, msg);
        dyn_sb_free(&line);
        return JS_ThrowRangeError(ctx, "confirm: the answer exceeds %d bytes without a newline",
            DYN_LINE_MAX);
    }
    if (line.n == 1 && (line.p[0] == 'y' || line.p[0] == 'Y'))
        yes = 1;
    else if (line.n == 3 && strncasecmp(line.p, "yes", 3) == 0)
        yes = 1;
    dyn_sb_free(&line);
    JS_FreeCString(ctx, msg);
    return JS_NewBool(ctx, yes);
}

static void dyn_sel_draw(char* const* o, const size_t* ol, int n,
    int cur, int redraw, int show)
{
    dyn_sb_t b;
    int i;
    dyn_sb_init(&b);
    if (redraw) {
        char up[24];
        snprintf(up, sizeof up, "\033[%dA", n);
        dyn_sb_puts(&b, up);
    }
    for (i = 0; i < n; i++) {
        dyn_sb_puts(&b, (show && i == cur) ? "> " : "  ");
        dyn_sb_put(&b, o[i], ol[i]);
        dyn_sb_puts(&b, "\033[K\n");
    }
    if (b.p && b.n && write(1, b.p, b.n) < 0) {
    }
    dyn_sb_free(&b);
}

static JSValue dyn_select(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* msg;
    size_t mlen = 0;
    char** o = NULL;
    size_t* ol = NULL;
    int64_t len64 = 0;
    int n = 0, i, cur = 0, ok;
    JSValue arr, r = JS_NULL;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "select(message, options): message must be a string");
    arr = argv[1];
    if (!JS_IsArray(ctx, arr))
        return JS_ThrowTypeError(ctx, "select(message, options): options must be an array of strings");
    {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        ok = JS_ToInt64(ctx, &len64, lv) == 0;
        JS_FreeValue(ctx, lv);
        if (!ok)
            return JS_EXCEPTION;
    }
    if (len64 < 1)
        return JS_ThrowTypeError(ctx, "select(message, options): options must be non-empty");
    if (len64 > 65536)
        return JS_ThrowRangeError(ctx, "select(message, options): more than 65536 options");
    n = (int)len64;
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg)
        return JS_EXCEPTION;
    o = (char**)calloc((size_t)n, sizeof *o);
    ol = (size_t*)calloc((size_t)n, sizeof *ol);
    if (!o || !ol) {
        free(o);
        free(ol);
        JS_FreeCString(ctx, msg);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char* s;
        if (JS_IsException(v)) {
            int bad = i;
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            (void)bad;
            return JS_EXCEPTION;
        }
        if (!JS_IsString(v)) {
            int bad = i;
            JS_FreeValue(ctx, v);
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowTypeError(ctx,
                "select(message, options): option %d must be a string", bad);
        }
        s = JS_ToCStringLen(ctx, &ol[i], v);
        JS_FreeValue(ctx, v);
        o[i] = dyn_dup_span(s, ol[i]);
        JS_FreeCString(ctx, s);
        if (!o[i]) {
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    {
        dyn_termios_t saved;
        int raw = dyn_raw_on(&saved);
        if (mlen && write(1, msg, mlen) < 0) {
        }
        if (write(1, "\n", 1) < 0) {
        }
        dyn_sel_draw(o, ol, n, cur, 0, 1);
        for (;;) {
            dyn_key_t k;
            dyn_key_read(0, &k);
            if (k.eof) {
                dyn_sel_draw(o, ol, n, cur, 1, 0);
                r = JS_NULL;
                break;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "up")) {
                cur = (cur + n - 1) % n;
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                continue;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "down")) {
                cur = (cur + 1) % n;
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                continue;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "enter")) {
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                r = JS_NewStringLen(ctx, o[cur], ol[cur]);
                break;
            }
            if ((k.name && !strcmp(k.name, "escape") && !k.ctrl && !k.meta) || (k.ctrl && k.name && !strcmp(k.name, "c"))) {
                dyn_sel_draw(o, ol, n, cur, 1, 0);
                r = JS_NULL;
                break;
            }
        }
        dyn_raw_off(&saved, raw);
    }
    for (i = 0; i < n; i++)
        free(o[i]);
    free(o);
    free(ol);
    JS_FreeCString(ctx, msg);
    return r;
}

#define DYN_PBAR_CELLS 20

typedef struct {
    double total;
    time_t t0;
    int done;
} dyn_pbar_t;

static JSClassID dyn_pbar_class_id;

static void dyn_pbar_dispose(void* native)
{
    free(native);
}

static void dyn_pbar_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_pbar_t* p = (dyn_pbar_t*)JS_GetOpaque(val, dyn_pbar_class_id);
    (void)rt;
    if (p)
        dyn_pbar_dispose(p);
}

static JSClassDef dyn_pbar_class = {
    "ProgressBar",
    .finalizer = dyn_pbar_finalizer,
};

static JSValue dyn_pbar_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "total" };
    dyn_pbar_t* p;
    JSValue v;
    double total = 0;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ProgressBar(opts): opts must be an object");
    if (dyn_cli_opts_strict(ctx, argv[0], keys, 1))
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[0], "total");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "ProgressBar({total}): total must be a number");
    }
    if (JS_ToFloat64(ctx, &total, v)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (!(total > 0) || total > 1e15)
        return JS_ThrowRangeError(ctx, "ProgressBar({total}): total must be positive");
    p = (dyn_pbar_t*)calloc(1, sizeof *p);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->total = total;
    p->t0 = time(NULL);
    return dyn_plain_wrap(ctx, new_target, dyn_pbar_class_id, p, dyn_pbar_dispose);
}

static JSValue dyn_pbar_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pbar_t* p = dyn_plain_get(ctx, this_val, dyn_pbar_class_id);
    char bar[DYN_PBAR_CELLS + 1], etxt[32], line[128];
    double n;
    int filled, pct, len;
    long eta;

    if (!p)
        return JS_EXCEPTION;
    if (p->done)
        return JS_ThrowRangeError(ctx, "ProgressBar.update: already finished");
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "ProgressBar.update(n): n must be a number");
    if (JS_ToFloat64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    if (!(n >= 0) || n > p->total)
        return JS_ThrowRangeError(ctx, "ProgressBar.update(n): n must be within 0..total");
    filled = (int)(n / p->total * DYN_PBAR_CELLS);
    pct = (int)(n * 100.0 / p->total);
    if (n >= p->total) {
        filled = DYN_PBAR_CELLS;
        pct = 100;
    }
    if (n > 0) {
        long el = (long)(time(NULL) - p->t0);
        if (el < 0)
            el = 0;
        eta = (long)(el * (p->total - n) / n);
        if (eta < 0)
            eta = 0;
        snprintf(etxt, sizeof etxt, "%lds", eta);
    } else {
        snprintf(etxt, sizeof etxt, "?");
    }
    memset(bar, '#', (size_t)filled);
    memset(bar + filled, '-', (size_t)(DYN_PBAR_CELLS - filled));
    bar[DYN_PBAR_CELLS] = 0;
    len = snprintf(line, sizeof line, "\r[%s] %d%% %g/%g ETA %s\033[K",
        bar, pct, n, p->total, etxt);
    if (len > 0 && (size_t)len < sizeof line && n >= p->total) {
        if ((size_t)len + 1 < sizeof line) {
            line[len++] = '\n';
            line[len] = 0;
            p->done = 1;
        }
    }
    if (len > 0 && write(1, line, (size_t)len) < 0) {
    }
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry dyn_pbar_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_pbar_update),
};

typedef struct {
    char* text;
    int state;
    int frame;
} dyn_spin_t;

static JSClassID dyn_spin_class_id;

static void dyn_spin_dispose(void* native)
{
    dyn_spin_t* s = (dyn_spin_t*)native;
    if (s) {
        free(s->text);
        free(s);
    }
}

static void dyn_spin_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_spin_t* s = (dyn_spin_t*)JS_GetOpaque(val, dyn_spin_class_id);
    (void)rt;
    if (s)
        dyn_spin_dispose(s);
}

static JSClassDef dyn_spin_class = {
    "Spinner",
    .finalizer = dyn_spin_finalizer,
};

static JSValue dyn_spin_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "text" };
    dyn_spin_t* s;

    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue v;
        if (!JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "Spinner(opts): opts must be an object");
        if (dyn_cli_opts_strict(ctx, argv[0], keys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "text");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* t;
            size_t tl;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "Spinner({text}): text must be a string");
            }
            t = JS_ToCStringLen(ctx, &tl, v);
            JS_FreeValue(ctx, v);
            if (!t)
                return JS_EXCEPTION;
            s = (dyn_spin_t*)calloc(1, sizeof *s);
            if (!s) {
                JS_FreeCString(ctx, t);
                return JS_ThrowOutOfMemory(ctx);
            }
            s->text = dyn_dup_span(t, tl);
            JS_FreeCString(ctx, t);
            if (!s->text) {
                free(s);
                return JS_ThrowOutOfMemory(ctx);
            }
            return dyn_plain_wrap(ctx, new_target, dyn_spin_class_id, s, dyn_spin_dispose);
        }
    }
    s = (dyn_spin_t*)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->text = dyn_dup_span("", 0);
    if (!s->text) {
        free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_spin_class_id, s, dyn_spin_dispose);
}

static dyn_spin_t* dyn_spin_of(JSContext* ctx, JSValueConst this_val)
{
    return (dyn_spin_t*)dyn_plain_get(ctx, this_val, dyn_spin_class_id);
}

static void dyn_spin_draw(const dyn_spin_t* s)
{
    static const char frames[] = "|/-\\";
    char line[8];
    int len = snprintf(line, sizeof line, "\r%c ", frames[s->frame & 3]);
    dyn_sb_t b;
    dyn_sb_init(&b);
    dyn_sb_put(&b, line, (size_t)len);
    dyn_sb_puts(&b, s->text ? s->text : "");
    dyn_sb_puts(&b, "\033[K");
    if (b.p && b.n && write(1, b.p, b.n) < 0) {
    }
    dyn_sb_free(&b);
}

static JSValue dyn_spin_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 0)
        return JS_ThrowRangeError(ctx, "Spinner.start: %s",
            s->state == 1 ? "already started" : "already stopped");
    s->state = 1;
    s->frame = 0;
    dyn_spin_draw(s);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_spin_tick(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.tick: %s",
            s->state == 0 ? "not started" : "already stopped");
    s->frame = (s->frame + 1) & 3;
    dyn_spin_draw(s);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_spin_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_spin_t* s = dyn_spin_of(ctx, this_val);
    const char* t = NULL;
    size_t tl = 0;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.stop: %s",
            s->state == 0 ? "not started" : "already stopped");
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "Spinner.stop(text): text must be a string");
        t = JS_ToCStringLen(ctx, &tl, argv[0]);
        if (!t)
            return JS_EXCEPTION;
    }
    s->state = 2;
    if (t) {
        dyn_sb_t b;
        dyn_sb_init(&b);
        dyn_sb_putc(&b, '\r');
        dyn_sb_put(&b, t, tl);
        dyn_sb_puts(&b, "\033[K\n");
        if (b.p && b.n && write(1, b.p, b.n) < 0) {
        }
        dyn_sb_free(&b);
        JS_FreeCString(ctx, t);
    } else {
        if (write(1, "\r\033[K", 4) < 0) {
        }
    }
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry dyn_spin_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_spin_start),
    JS_CFUNC_DEF("tick", 0, dyn_spin_tick),
    JS_CFUNC_DEF("stop", 0, dyn_spin_stop),
};

static int dyn_utf8_width(const char* s, size_t n)
{
    size_t i;
    int w = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            w++;
    return w;
}

static void dyn_sb_pad(dyn_sb_t* b, int n)
{
    while (n-- > 0)
        dyn_sb_putc(b, ' ');
}

static void dyn_csv_cell(dyn_sb_t* b, const char* s, size_t n)
{
    size_t i;
    int quote = 0;
    for (i = 0; i < n; i++)
        if (s[i] == '"' || s[i] == ',' || s[i] == '\r' || s[i] == '\n')
            quote = 1;
    if (!quote) {
        dyn_sb_put(b, s, n);
        return;
    }
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"')
            dyn_sb_putc(b, '"');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

static JSValue dyn_table(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    enum { DYN_ALIGN_LEFT,
        DYN_ALIGN_RIGHT,
        DYN_ALIGN_CENTER };
    JSValueConst rows, opts;
    JSValue head = JS_UNDEFINED;
    int64_t nrow64 = 0;
    uint32_t nrow = 0, ncols = 0, i, j;
    char** cells = NULL;
    size_t ncells;
    size_t* clen = NULL;
    int* cwid = NULL;
    char** hs = NULL;
    size_t* hl = NULL;
    int* hw = NULL;
    int* align = NULL;
    int fmt_grid = 1, fmt_tsv = 0, fmt_csv = 0;
    int fc_owned = 0;
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char* fc = "grid";

    dyn_sb_init(&b);
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Table(rows, opts): rows must be an array");
    rows = argv[0];
    opts = (argc > 1 && !JS_IsNull(argv[1])) ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        static const char* const tkeys[] = { "head", "align", "format" };
        if (!JS_IsObject(opts))
            return JS_ThrowTypeError(ctx, "Table(rows, opts): opts must be an object");
        if (dyn_cli_opts_strict(ctx, opts, tkeys, 3))
            return JS_EXCEPTION;
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length");
        int64_t dummy = 0;
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &dummy, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (dummy < 0)
            dummy = 0;
        if (dummy > 65536 * 4096)
            return JS_ThrowRangeError(ctx, "Table(rows): too many rows");
        nrow64 = dummy;
    }
    if (!JS_IsUndefined(opts)) {
        head = JS_GetPropertyStr(ctx, opts, "head");
        if (JS_IsException(head))
            return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(head) && !JS_IsNull(head)) {
        if (!JS_IsArray(ctx, head)) {
            JS_FreeValue(ctx, head);
            return JS_ThrowTypeError(ctx, "Table({head}): head must be an array of strings");
        }
        {
            JSValue lv = JS_GetPropertyStr(ctx, head, "length");
            int64_t hlen = 0;
            if (JS_IsException(lv) || JS_ToInt64(ctx, &hlen, lv)) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, head);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
            ncols = (uint32_t)(hlen < 0 ? 0 : hlen);
        }
    } else {
        JS_FreeValue(ctx, head);
        head = JS_UNDEFINED;
    }
    if (ncols == 0 && nrow64 > 0) {
        JSValue row0 = JS_GetPropertyUint32(ctx, rows, 0);
        JSValue lv;
        int64_t rl = 0;
        if (JS_IsException(row0))
            return JS_EXCEPTION;
        if (!JS_IsArray(ctx, row0)) {
            JS_FreeValue(ctx, row0);
            return JS_ThrowTypeError(ctx, "Table(rows): every row must be an array");
        }
        lv = JS_GetPropertyStr(ctx, row0, "length");
        JS_FreeValue(ctx, row0);
        if (JS_IsException(lv) || JS_ToInt64(ctx, &rl, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        ncols = (uint32_t)(rl < 0 ? 0 : rl);
    }
    nrow = (uint32_t)nrow64;
    if (ncols == 0) {
        JS_FreeValue(ctx, head);
        return JS_NewString(ctx, "");
    }
    if ((uint64_t)nrow * ncols > (uint64_t)1 << 28) {
        JS_FreeValue(ctx, head);
        return JS_ThrowRangeError(ctx, "Table(rows): too many cells");
    }
    ncells = (size_t)nrow * ncols + 1;
    cells = (char**)calloc(ncells, sizeof *cells);
    clen = (size_t*)calloc(ncells, sizeof *clen);
    cwid = (int*)calloc(ncells, sizeof *cwid);
    hs = (char**)calloc(ncols, sizeof *hs);
    hl = (size_t*)calloc(ncols, sizeof *hl);
    hw = (int*)calloc(ncols, sizeof *hw);
    align = (int*)calloc(ncols, sizeof *align);
    if (!cells || !clen || !cwid || !hs || !hl || !hw || !align)
        goto oom;
    if (!JS_IsUndefined(head)) {
        for (j = 0; j < ncols; j++) {
            JSValue hv = JS_GetPropertyUint32(ctx, head, j);
            const char* s;
            if (JS_IsException(hv))
                goto fail;
            if (!JS_IsString(hv)) {
                JS_FreeValue(ctx, hv);
                JS_ThrowTypeError(ctx, "Table({head}): head[%d] must be a string", (int)j);
                goto fail;
            }
            s = JS_ToCStringLen(ctx, &hl[j], hv);
            JS_FreeValue(ctx, hv);
            if (!s)
                goto fail;
            hs[j] = dyn_dup_span(s, hl[j]);
            JS_FreeCString(ctx, s);
            if (!hs[j])
                goto oom;
            hw[j] = dyn_utf8_width(hs[j], hl[j]);
        }
    }
    {
        if (!JS_IsUndefined(opts)) {
            JSValue v = JS_GetPropertyStr(ctx, opts, "format");
            if (JS_IsException(v))
                goto fail;
            if (!JS_IsUndefined(v)) {
                if (!JS_IsString(v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "Table({format}): format must be a string");
                    goto fail;
                }
                fc = JS_ToCString(ctx, v);
                JS_FreeValue(ctx, v);
                if (!fc)
                    goto fail;
                fc_owned = 1;
                fmt_grid = fmt_tsv = fmt_csv = 0;
                if (!strcmp(fc, "grid"))
                    fmt_grid = 1;
                else if (!strcmp(fc, "tsv"))
                    fmt_tsv = 1;
                else if (!strcmp(fc, "csv"))
                    fmt_csv = 1;
                else {
                    JS_ThrowRangeError(ctx,
                        "Table({format}): unknown format \"%s\" (valid: grid, tsv, csv)", fc);
                    goto fail;
                }
            }
            v = JS_GetPropertyStr(ctx, opts, "align");
            if (JS_IsException(v))
                goto fail;
            if (!JS_IsUndefined(v)) {
                int64_t al = 0, ai;
                JSValue lv;
                if (!JS_IsArray(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "Table({align}): align must be an array");
                    goto fail;
                }
                lv = JS_GetPropertyStr(ctx, v, "length");
                if (JS_IsException(lv) || JS_ToInt64(ctx, &al, lv)) {
                    JS_FreeValue(ctx, lv);
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                JS_FreeValue(ctx, lv);
                if (al > (int64_t)ncols) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowRangeError(ctx,
                        "Table({align}): %d entries for %d columns", (int)al, (int)ncols);
                    goto fail;
                }
                for (ai = 0; ai < al; ai++) {
                    JSValue ev = JS_GetPropertyUint32(ctx, v, (uint32_t)ai);
                    const char* a = NULL;
                    if (!JS_IsException(ev))
                        a = JS_ToCString(ctx, ev);
                    JS_FreeValue(ctx, ev);
                    if (!a) {
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                    if (!strcmp(a, "left"))
                        align[ai] = DYN_ALIGN_LEFT;
                    else if (!strcmp(a, "right"))
                        align[ai] = DYN_ALIGN_RIGHT;
                    else if (!strcmp(a, "center"))
                        align[ai] = DYN_ALIGN_CENTER;
                    else {
                        JS_ThrowRangeError(ctx,
                            "Table({align}): unknown alignment \"%s\" (valid: left, right, center)", a);
                        JS_FreeCString(ctx, a);
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                    JS_FreeCString(ctx, a);
                }
                JS_FreeValue(ctx, v);
            }
        }
    }
    for (i = 0; i < nrow; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        int64_t rl = 0;
        JSValue lv;
        if (JS_IsException(row))
            goto fail;
        if (!JS_IsArray(ctx, row)) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx, "Table(rows): row %d must be an array", (int)i);
            goto fail;
        }
        lv = JS_GetPropertyStr(ctx, row, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &rl, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, row);
            goto fail;
        }
        JS_FreeValue(ctx, lv);
        if (rl != (int64_t)ncols) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx,
                "Table(rows): row %d has %d cells, expected %d",
                (int)i, (int)rl, (int)ncols);
            goto fail;
        }
        for (j = 0; j < ncols; j++) {
            JSValue v = JS_GetPropertyUint32(ctx, row, j);
            JSValue sv;
            const char* s;
            size_t idx = (size_t)i * ncols + j;
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (JS_IsNumber(v)) {
                sv = JS_ToString(ctx, v);
                JS_FreeValue(ctx, v);
            } else if (JS_IsString(v)) {
                sv = v;
            } else {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, row);
                JS_ThrowTypeError(ctx,
                    "Table(rows): cell (%d, %d) must be a string or number",
                    (int)i, (int)j);
                goto fail;
            }
            if (JS_IsException(sv)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            s = JS_ToCStringLen(ctx, &clen[idx], sv);
            JS_FreeValue(ctx, sv);
            if (!s) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            cells[idx] = dyn_dup_span(s, clen[idx]);
            JS_FreeCString(ctx, s);
            if (!cells[idx]) {
                JS_FreeValue(ctx, row);
                goto oom;
            }
            cwid[idx] = dyn_utf8_width(cells[idx], clen[idx]);
        }
        JS_FreeValue(ctx, row);
    }
    {
        size_t* w = (size_t*)calloc(ncols, sizeof *w);
        if (!w)
            goto oom_b;
        for (j = 0; j < ncols; j++) {
            w[j] = (size_t)hw[j];
            for (i = 0; i < nrow; i++) {
                size_t idx = (size_t)i * ncols + j;
                if ((size_t)cwid[idx] > w[j])
                    w[j] = (size_t)cwid[idx];
            }
        }
        if (fmt_grid) {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    int pad = (int)w[j] - hw[j];
                    int pl = 0, pr = pad;
                    if (align[j] == DYN_ALIGN_CENTER) {
                        pl = pad / 2;
                        pr = pad - pl;
                    } else if (align[j] == DYN_ALIGN_LEFT) {
                        pl = 0;
                        pr = pad;
                    } else {
                        pl = pad;
                        pr = 0;
                    }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, hs[j], hl[j]);
                    if (j + 1 < ncols) {
                        dyn_sb_pad(&b, pr);
                        dyn_sb_puts(&b, "  ");
                    }
                }
                dyn_sb_putc(&b, '\n');
                for (j = 0; j < ncols; j++) {
                    size_t d;
                    for (d = 0; d < w[j]; d++)
                        dyn_sb_putc(&b, '-');
                    if (j + 1 < ncols)
                        dyn_sb_puts(&b, "  ");
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    int pad = (int)w[j] - cwid[idx];
                    int pl = 0, pr = pad;
                    if (align[j] == DYN_ALIGN_CENTER) {
                        pl = pad / 2;
                        pr = pad - pl;
                    } else if (align[j] == DYN_ALIGN_RIGHT) {
                        pl = pad;
                        pr = 0;
                    }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, cells[idx], clen[idx]);
                    if (j + 1 < ncols) {
                        dyn_sb_pad(&b, pr);
                        dyn_sb_puts(&b, "  ");
                    }
                }
                dyn_sb_putc(&b, '\n');
            }
        } else if (fmt_tsv) {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    if (j)
                        dyn_sb_putc(&b, '\t');
                    dyn_sb_put(&b, hs[j], hl[j]);
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    if (j)
                        dyn_sb_putc(&b, '\t');
                    dyn_sb_put(&b, cells[idx], clen[idx]);
                }
                dyn_sb_putc(&b, '\n');
            }
        } else {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    if (j)
                        dyn_sb_putc(&b, ',');
                    dyn_csv_cell(&b, hs[j], hl[j]);
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    if (j)
                        dyn_sb_putc(&b, ',');
                    dyn_csv_cell(&b, cells[idx], clen[idx]);
                }
                dyn_sb_putc(&b, '\n');
            }
        }
        free(w);
    }
    if (b.oom)
        goto oom_b;
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    goto cleanup;

oom:
    JS_ThrowOutOfMemory(ctx);
    goto fail;
oom_b:
    JS_ThrowOutOfMemory(ctx);
    goto fail;
fail:
    r = JS_EXCEPTION;
cleanup:
    dyn_sb_free(&b);
    if (cells) {
        size_t total = (size_t)nrow * ncols + 1, t;
        for (t = 0; t < total; t++)
            free(cells[t]);
    }
    if (hs) {
        uint32_t t;
        for (t = 0; t < ncols; t++)
            free(hs[t]);
    }
    free(cells);
    free(clen);
    free(cwid);
    free(hs);
    free(hl);
    free(hw);
    free(align);
    JS_FreeValue(ctx, head);
    if (fc_owned)
        JS_FreeCString(ctx, fc);
    return r;
}

static const JSCFunctionListEntry dyn_term_funcs[] = {
    JS_CFUNC_DEF("StyleText", 2, dyn_style_text),
    JS_CFUNC_DEF("Styles", 0, dyn_styles_list),
    JS_CFUNC_DEF("IsTTY", 0, dyn_is_tty),
    JS_CFUNC_DEF("Columns", 0, dyn_columns),
    JS_CFUNC_DEF("ColorDepth", 0, dyn_color_depth),
    JS_CFUNC_DEF("prompt", 2, dyn_prompt),
    JS_CFUNC_DEF("confirm", 1, dyn_confirm),
    JS_CFUNC_DEF("select", 2, dyn_select),
    JS_CFUNC_DEF("keypress", 0, dyn_keypress_fn),
    JS_CFUNC_DEF("Table", 2, dyn_table),
};

static int dyn_term_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_cmd_class_id, &dyn_cmd_class,
            dyn_cmd_proto, countof(dyn_cmd_proto),
            dyn_cmd_ctor, "Command")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pbar_class_id, &dyn_pbar_class,
            dyn_pbar_proto, countof(dyn_pbar_proto),
            dyn_pbar_ctor, "ProgressBar")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_spin_class_id, &dyn_spin_class,
            dyn_spin_proto, countof(dyn_spin_proto),
            dyn_spin_ctor, "Spinner")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

int js_nat_init_term(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:cli", dyn_term_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Command");
    JS_AddModuleExport(ctx, m, "ProgressBar");
    JS_AddModuleExport(ctx, m, "Spinner");
    return JS_AddModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

#endif
