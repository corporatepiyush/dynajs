/* dyna:cli -- argument parsing, terminal styling and TTY queries.
   NOT src/dyna-cli.c: that name is the standalone interpreter's main().
   Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A local growable buffer over the shared core (core/dyn-sb.h): the struct
   name, the sticky-oom void-return convention and the seed (64) are this
   module's. */
typedef struct { char *p; size_t n, cap; int oom; } dyn_sb_t;

static void dyn_sb_init(dyn_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_sb_free(dyn_sb_t *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

static void dyn_sb_put(dyn_sb_t *b, const char *s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_sb_putc(dyn_sb_t *b, char c) { dyn_sb_put(b, &c, 1); }
static void dyn_sb_puts(dyn_sb_t *b, const char *s) { dyn_sb_put(b, s, strlen(s)); }

/* A spec is written by the program, not by its input, so these bound a
   programming mistake rather than an attack. */
#define DYN_CMD_MAX_OPTS 256
#define DYN_CMD_MAX_ARGS 64
#define DYN_CMD_MAX_SUBS 64

enum { DYN_T_BOOL, DYN_T_STRING, DYN_T_NUMBER };

typedef struct {
    char *shortname;        /* "v", or NULL */
    char *longname;         /* "verbose", never NULL */
    char *desc;
    char *placeholder;      /* "<path>", or NULL for a flag */
    int type;
    int required;
    int variadic;           /* repeated occurrences collect into an array */
    JSValue dflt;
} dyn_opt_t;

typedef struct {
    char *name;             /* "input" */
    char *desc;
    int required;
    int variadic;           /* "<input...>" takes the rest */
} dyn_arg_t;

typedef struct dyn_cmd {
    char *name, *desc;
    dyn_opt_t opts[DYN_CMD_MAX_OPTS];
    int n_opts;
    dyn_arg_t args[DYN_CMD_MAX_ARGS];
    int n_args;
    JSValue subs[DYN_CMD_MAX_SUBS];   /* Command objects, held alive by gc_mark */
    int n_subs;
    int allow_unknown;
} dyn_cmd_t;

static JSClassID dyn_cmd_class_id;

static void dyn_cmd_free_rt(JSRuntime *rt, dyn_cmd_t *c)
{
    int i;
    for (i = 0; i < c->n_opts; i++) {
        free(c->opts[i].shortname); free(c->opts[i].longname);
        free(c->opts[i].desc); free(c->opts[i].placeholder);
        JS_FreeValueRT(rt, c->opts[i].dflt);
    }
    for (i = 0; i < c->n_args; i++) { free(c->args[i].name); free(c->args[i].desc); }
    for (i = 0; i < c->n_subs; i++) JS_FreeValueRT(rt, c->subs[i]);
    free(c->name); free(c->desc);
    free(c);
}

static void dyn_cmd_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_cmd_t *c = (dyn_cmd_t *)JS_GetOpaque(val, dyn_cmd_class_id);
    if (c)
        dyn_cmd_free_rt(rt, c);
}

/* The struct holds JSValues -- option defaults and subcommands -- so the cycle
   collector must be able to see them or a Command in a cycle never frees. */
static void dyn_cmd_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    dyn_cmd_t *c = (dyn_cmd_t *)JS_GetOpaque(val, dyn_cmd_class_id);
    int i;
    if (!c)
        return;
    for (i = 0; i < c->n_opts; i++)
        JS_MarkValue(rt, c->opts[i].dflt, mark);
    for (i = 0; i < c->n_subs; i++)
        JS_MarkValue(rt, c->subs[i], mark);
}

static JSClassDef dyn_cmd_class = {
    "Command", .finalizer = dyn_cmd_finalizer, .gc_mark = dyn_cmd_gc_mark,
};

static dyn_cmd_t *dyn_cmd_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_cmd_t *)dyn_plain_get(ctx, v, dyn_cmd_class_id);
}

static char *dyn_dup_span(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* Argv text echoed into error messages must not carry C0 controls: an ESC
   byte in a thrown message is a terminal-injection vector the moment the
   message is printed. Controls become \xNN; every other byte passes. NULL
   on OOM -- the caller then falls back to the raw text. */
static char *dyn_arg_esc(const char *s)
{
    size_t n = strlen(s), i, j = 0;
    char *r = (char *)malloc(4 * n + 1);
    if (!r)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) {
            static const char hex[] = "0123456789abcdef";
            r[j++] = '\\'; r[j++] = 'x';
            r[j++] = hex[ch >> 4]; r[j++] = hex[ch & 15];
        } else {
            r[j++] = (char)ch;
        }
    }
    r[j] = 0;
    return r;
}

/* ------------------------------------------------------- the option spec */

/* Parse "-o, --out <path>" or "--verbose" or "-v". The flags string is the
   configuration; it is read once here, never per argv element. */
static int dyn_opt_spec(dyn_opt_t *o, const char *s, size_t n)
{
    size_t i = 0;
    o->shortname = o->longname = o->placeholder = NULL;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == ',')) i++;
        if (i >= n) break;
        if (s[i] == '<' || s[i] == '[') {
            char close = (s[i] == '<') ? '>' : ']';
            size_t b = ++i;
            while (i < n && s[i] != close) i++;
            free(o->placeholder);
            o->placeholder = dyn_dup_span(s + b, i - b);
            if (i < n) i++;
        } else if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
            size_t b = i + 2;
            i = b;
            while (i < n && s[i] != ' ' && s[i] != ',') i++;
            free(o->longname);
            o->longname = dyn_dup_span(s + b, i - b);
        } else if (s[i] == '-') {
            size_t b = i + 1;
            i = b;
            while (i < n && s[i] != ' ' && s[i] != ',') i++;
            free(o->shortname);
            o->shortname = dyn_dup_span(s + b, i - b);
        } else {
            while (i < n && s[i] != ' ' && s[i] != ',') i++;
        }
    }
    /* A long name is what the parsed object is keyed by, so it is required;
       a short-only option would have no stable name to report under. An
       empty one (`option("--")`) would key under "" and is refused the same
       way. */
    return (o->longname && o->longname[0]) ? 0 : -1;
}

static JSValue dyn_cmd_option(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    dyn_opt_t *o;
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
        free(o->shortname); free(o->longname); free(o->placeholder);
        return JS_ThrowTypeError(ctx,
            "Command.option(flags): a long name (--name) is required");
    }
    JS_FreeCString(ctx, flags);
    {   /* A duplicate long name would silently shadow the first spelling in
           both the parsed object and the help text -- and the same is true of
           a reused short name, whose lookups always resolve to the first
           registration. Refuse both at the declaration, where the mistake is
           visible. */
        int i;
        for (i = 0; i < c->n_opts; i++) {
            if (!strcmp(c->opts[i].longname, o->longname)) {
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"--%s\"",
                    o->longname);
                free(o->shortname); free(o->longname); free(o->placeholder);
                return ex;
            }
            if (o->shortname && c->opts[i].shortname
                && !strcmp(c->opts[i].shortname, o->shortname)) {
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"-%s\"",
                    o->shortname);
                free(o->shortname); free(o->longname); free(o->placeholder);
                return ex;
            }
        }
    }
    if (argc > 1 && JS_IsString(argv[1]) && (d = JS_ToCString(ctx, argv[1])) != NULL) {
        o->desc = dyn_dup_span(d, strlen(d));
        JS_FreeCString(ctx, d);
    }
    o->type = o->placeholder ? DYN_T_STRING : DYN_T_BOOL;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "type");
        if (JS_IsString(v)) {
            const char *t = JS_ToCString(ctx, v);
            if (t) {
                if      (!strcmp(t, "boolean")) o->type = DYN_T_BOOL;
                else if (!strcmp(t, "number"))  o->type = DYN_T_NUMBER;
                else if (!strcmp(t, "string"))  o->type = DYN_T_STRING;
                else {
                    JS_FreeCString(ctx, t); JS_FreeValue(ctx, v);
                    return JS_ThrowRangeError(ctx,
                        "Command.option({type}): type must be boolean, string or number");
                }
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "required");
        o->required = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "variadic");
        o->variadic = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        o->dflt = JS_GetPropertyStr(ctx, argv[2], "default");
        if (JS_IsException(o->dflt))
            o->dflt = JS_UNDEFINED;
    }
    c->n_opts++;
    return JS_DupValue(ctx, this_val);          /* chainable */
}

static JSValue dyn_cmd_argument(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    dyn_arg_t *a;
    const char *s;
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
    a->required = (n && s[0] == '<');           /* <req> vs [optional] */
    b = (n && (s[0] == '<' || s[0] == '[')) ? 1 : 0;
    e = n;
    if (e > b && (s[e - 1] == '>' || s[e - 1] == ']')) e--;
    if (e >= b + 3 && memcmp(s + e - 3, "...", 3) == 0) { a->variadic = 1; e -= 3; }
    a->name = dyn_dup_span(s + b, e - b);
    JS_FreeCString(ctx, s);
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *d = JS_ToCString(ctx, argv[1]);
        if (d) { a->desc = dyn_dup_span(d, strlen(d)); JS_FreeCString(ctx, d); }
    }
    if (!a->name)
        return JS_ThrowOutOfMemory(ctx);
    c->n_args++;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_cmd_describe(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    const char *d;
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

static JSValue dyn_cmd_command(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    dyn_cmd_t *sc;
    int i;

    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !dyn_cmd_of(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Command.command(sub): sub must be a Command");
    /* Two subcommands with one name would silently leave the first in charge
       of every parse: refuse the registration instead. */
    sc = dyn_cmd_of(ctx, argv[0]);
    if (sc && sc->name) {
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t *x = dyn_cmd_of(ctx, c->subs[i]);
            if (x && x->name && !strcmp(x->name, sc->name))
                return JS_ThrowTypeError(ctx,
                    "Command.command(sub): a subcommand named \"%s\" is already registered",
                    sc->name);
        }
    }
    if (sc == c)    /* argv[0] IS this command: a self-cycle is a mistake,
                       not a tree (dispatch into it is bounded only by argv) */
        return JS_ThrowTypeError(ctx,
            "Command.command(sub): a command cannot be its own subcommand");
    if (c->n_subs >= DYN_CMD_MAX_SUBS)
        return JS_ThrowRangeError(ctx, "Command.command: more than %d subcommands", DYN_CMD_MAX_SUBS);
    c->subs[c->n_subs++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_cmd_allow_unknown(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    c->allow_unknown = (argc < 1) ? 1 : JS_ToBool(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

/* ------------------------------------------------------------- the parse */

static dyn_opt_t *dyn_find_long(dyn_cmd_t *c, const char *s, size_t n)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (strlen(c->opts[i].longname) == n && memcmp(c->opts[i].longname, s, n) == 0)
            return &c->opts[i];
    return NULL;
}

static dyn_opt_t *dyn_find_short(dyn_cmd_t *c, char ch)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (c->opts[i].shortname && c->opts[i].shortname[0] == ch
            && c->opts[i].shortname[1] == 0)
            return &c->opts[i];
    return NULL;
}

static int dyn_number_span(const char *s);

/* Read-only mirror of dyn_parse_one's consumption for the subcommand scan:
   how many argv tokens does this one own? 0 = a plain token (a subcommand
   candidate), 1 = a self-contained option token, 2 = an option that also owns
   the NEXT token (a value option's separate value). Without this, the value
   of "--dir /x" would end the subcommand scan (so a subcommand after it is
   never found), or a value that happens to EQUAL a subcommand name would
   dispatch on the value. */
static int dyn_scan_arity(dyn_cmd_t *c, const char *a)
{
    size_t alen = strlen(a);
    if (a[0] != '-' || alen == 1)
        return 0;                    /* plain token, or "-" */
    if (alen == 2 && a[1] == '-')
        return 0;                    /* the terminator: the caller breaks */
    if (dyn_number_span(a))
        return 0;                    /* a negative number is a plain token */
    if (a[1] == '-') {
        const char *eq = strchr(a + 2, '=');
        size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
        dyn_opt_t *o = dyn_find_long(c, a + 2, nlen);
        if (!o && nlen > 3 && memcmp(a + 2, "no-", 3) == 0) {
            o = dyn_find_long(c, a + 5, nlen - 3);
            if (o && o->type != DYN_T_BOOL)
                o = NULL;            /* --no-x only negates flags */
        }
        if (!o)
            return 1;                /* unknown: never owns the next token */
        return (o->type == DYN_T_BOOL || eq) ? 1 : 2;
    }
    {   /* a short bundle: a value option with nothing attached ends it */
        size_t k;
        for (k = 1; k < alen; k++) {
            dyn_opt_t *o = dyn_find_short(c, a[k]);
            if (!o)
                return 1;            /* an unknown short ends the bundle */
            if (o->type != DYN_T_BOOL)
                return (k + 1 < alen) ? 1 : 2;
        }
        return 1;                    /* all-flag bundle */
    }
}

/* Does the token parse ENTIRELY as a number? A negative number ("-5", "-.5")
   is a positional VALUE, not an unknown option; "-v" and "-n5" are not
   numbers and stay on the option paths. */
static int dyn_number_span(const char *s)
{
    char *end;
    if (!*s)
        return 0;
    (void)strtod(s, &end);
    return *end == '\0' && end != s;
}

/* Coerce a raw argv string to the option's DECLARED type. yargs-style implicit
   coercion is declined: a guessed type is how a CLI surprises its caller. */
static JSValue dyn_coerce(JSContext *ctx, const dyn_opt_t *o, const char *s)
{
    if (o->type == DYN_T_NUMBER) {
        JSValue sv = JS_NewString(ctx, s), r;
        double d;
        int bad;
        if (JS_IsException(sv))
            return sv;
        bad = JS_ToFloat64(ctx, &d, sv) || (d != d && strcmp(s, "NaN") != 0);
        JS_FreeValue(ctx, sv);
        if (bad || !*s) {
            char *esc = dyn_arg_esc(s);   /* the value is attacker-controlled */
            JS_ThrowTypeError(ctx, "--%s expects a number, got \"%s\"",
                              o->longname, esc ? esc : s);
            free(esc);
            return JS_EXCEPTION;
        }
        r = JS_NewFloat64(ctx, d);
        return r;
    }
    return JS_NewString(ctx, s);
}

/* Store a value, collecting into an array when the option is variadic. */
static int dyn_store(JSContext *ctx, JSValueConst obj, const dyn_opt_t *o,
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
            if (JS_IsException(arr)) { JS_FreeValue(ctx, val); return -1; }
            if (JS_SetPropertyStr(ctx, obj, o->longname, JS_DupValue(ctx, arr)) < 0) {
                JS_FreeValue(ctx, arr); JS_FreeValue(ctx, val); return -1;
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

static JSValue dyn_cmd_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);

/* A default must not alias the spec: a variadic push into, or a plain write
   to, the parse result would otherwise mutate the registration and leak into
   every later parse. Plain arrays/objects are copied structure-deep with a
   depth cap; any other value, and any element past the cap, is shared -- the
   same reference a spec literal would hand back. Returns JS_EXCEPTION on
   allocation failure. */
static JSValue dyn_default_copy(JSContext *ctx, JSValueConst v, int depth)
{
    uint32_t i, n = 0;
    JSValue r, lv, e;
    JSPropertyEnum *tab = NULL;
    int64_t len = 0;

    if (depth <= 0 || !JS_IsObject(v))
        return JS_DupValue(ctx, v);
    if (JS_IsArray(ctx, v)) {
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv))
            return lv;
        JS_ToInt64(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        if (len < 0 || len > 65536)      /* absurd default: share it */
            return JS_DupValue(ctx, v);
        r = JS_NewArray(ctx);
        if (JS_IsException(r))
            return r;
        for (i = 0; (int64_t)i < len; i++) {
            JSValue c;
            e = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(e)) { JS_FreeValue(ctx, r); return e; }
            c = dyn_default_copy(ctx, e, depth - 1);
            JS_FreeValue(ctx, e);   /* JS_GetPropertyUint32 handed us a reference */
            e = c;
            if (JS_IsException(e)) { JS_FreeValue(ctx, r); return e; }
            if (JS_DefinePropertyValueUint32(ctx, r, i, e, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, r);
                return JS_EXCEPTION;
            }
        }
        return r;
    }
    /* a plain-ish object: own enumerable string keys, values copied deeper */
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        JSValue c;
        e = JS_GetProperty(ctx, v, tab[i].atom);
        if (!JS_IsException(e)) {
            c = dyn_default_copy(ctx, e, depth - 1);
            JS_FreeValue(ctx, e);   /* JS_GetProperty handed us a reference */
            e = c;
        }
        if (JS_IsException(e) ||
            JS_DefinePropertyValue(ctx, r, tab[i].atom, e, JS_PROP_C_W_E) < 0) {
            JS_FreePropertyEnum(ctx, tab, n);   /* owns every atom left + the tab */
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return r;
}

/* One argv element. Returns the number consumed, or -1 having thrown. */
static int dyn_parse_one(JSContext *ctx, dyn_cmd_t *c, JSValueConst opts,
                         const char **av, int n, int i, JSValueConst positional,
                         uint32_t *n_pos, int *no_more_opts)
{
    const char *a = av[i];
    size_t alen = strlen(a);

    if (*no_more_opts || a[0] != '-' || alen == 1) {
        JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
                                     JS_NewString(ctx, a), JS_PROP_C_W_E);
        return 1;
    }
    if (alen == 2 && a[1] == '-') {              /* the `--` terminator */
        *no_more_opts = 1;
        return 1;
    }
    if (dyn_number_span(a)) {                    /* "-5" is a value, not a flag */
        JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
                                     JS_NewString(ctx, a), JS_PROP_C_W_E);
        return 1;
    }
    if (a[1] == '-') {                            /* --long, --long=v, --no-x */
        const char *eq = strchr(a + 2, '=');
        size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
        dyn_opt_t *o = dyn_find_long(c, a + 2, nlen);
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
            {   /* the flag is attacker-controlled text; do not hand the
                   terminal an escape sequence in the error message */
                char *esc = dyn_arg_esc(a);
                JS_ThrowTypeError(ctx, "unknown option \"%s\"", esc ? esc : a);
                free(esc);
            }
            return -1;
        }
        if (o->type == DYN_T_BOOL) {
            if (eq) {   /* `--flag=value` on a flag: refuse rather than
                           silently set true and discard the value */
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
    {   /* -abc bundled flags, -n5 attached value, -n 5 */
        size_t k;
        for (k = 1; k < alen; k++) {
            dyn_opt_t *o = dyn_find_short(c, a[k]);
            if (!o) {
                if (c->allow_unknown) {
                    JS_DefinePropertyValueUint32(ctx, positional, (*n_pos)++,
                                                 JS_NewString(ctx, a), JS_PROP_C_W_E);
                    return 1;
                }
                {   /* escaped: the bundle text is attacker-controlled */
                    char tok[2] = { a[k], 0 };
                    char *esc = dyn_arg_esc(tok);
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
            if (k + 1 < alen) {                   /* -n5: the rest is the value */
                const char *val = a + k + 1;
                if (*val == '=')
                    val++;                        /* -o=x means -o x */
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

/* If argv names a subcommand, parse this command's own options over the prefix,
   hand the tail to the subcommand, and return the combined result. Returns
   JS_UNDEFINED when no subcommand matched, so the caller parses normally. */
static int dyn_cmd_check_required(JSContext *ctx, dyn_cmd_t *c,
                                  JSValueConst opts, uint32_t n_pos);
static JSValue dyn_cmd_dispatch(JSContext *ctx, dyn_cmd_t *c, const char **av,
                                int n, JSValueConst opts, JSValueConst positional,
                                uint32_t *n_pos, int *no_more)
{
    int i, k;
    if (c->n_subs == 0)
        return JS_UNDEFINED;    /* nothing can dispatch: no scan, no cost */
    for (i = 0; i < n; i++) {
        int arity;
        if (!strcmp(av[i], "--"))
            break;      /* past the terminator everything is positional: no
                           subcommand may hide behind it */
        /* an option token skips itself -- and the value it will consume, so
           the scan finds the first PLAIN token, never a value (dyn_scan_arity
           is read-only: nothing is parsed here) */
        arity = dyn_scan_arity(c, av[i]);
        if (arity == 0) {
            for (k = 0; k < c->n_subs; k++) {
                dyn_cmd_t *sc = dyn_cmd_of(ctx, c->subs[k]);
                JSValue tail, subres, res;
                int j, p = 0;
                uint32_t t = 0;
                if (!sc || !sc->name || strcmp(sc->name, av[i]))
                    continue;
                while (p < i) {          /* the root's own options precede the token */
                    int used = dyn_parse_one(ctx, c, opts, av, i, p, positional,
                                             n_pos, no_more);
                    if (used < 0)
                        return JS_EXCEPTION;
                    p += used;
                }
                /* the root's own requirements, satisfiable by the prefix alone:
                   everything after the subcommand belongs to the sub */
                if (dyn_cmd_check_required(ctx, c, opts, *n_pos) < 0)
                    return JS_EXCEPTION;
                tail = JS_NewArray(ctx);
                if (JS_IsException(tail))
                    return JS_EXCEPTION;
                for (j = i + 1; j < n; j++)
                    JS_DefinePropertyValueUint32(ctx, tail, t++,
                        JS_NewString(ctx, av[j]), JS_PROP_C_W_E);
                subres = dyn_cmd_parse(ctx, c->subs[k], 1, (JSValueConst *)&tail);
                JS_FreeValue(ctx, tail);
                if (JS_IsException(subres))
                    return JS_EXCEPTION;
                res = JS_NewObject(ctx);
                if (JS_IsException(res)) { JS_FreeValue(ctx, subres); return JS_EXCEPTION; }
                JS_SetPropertyStr(ctx, res, "command", JS_NewString(ctx, av[i]));
                JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
                JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
                JS_SetPropertyStr(ctx, res, "result", subres);
                return res;
            }
            break;                       /* not a subcommand: the caller parses */
        }
        i += arity - 1;                  /* -1 for the loop's own i++ */
    }
    return JS_UNDEFINED;
}

/* Every declared requirement, checked after the whole argv is consumed: an
   option may legitimately appear last. */
static int dyn_cmd_check_required(JSContext *ctx, dyn_cmd_t *c,
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
                                     c->opts[k].longname), -1;
    }
    for (k = 0; k < c->n_args; k++)
        if (c->args[k].required && (uint32_t)k >= n_pos)
            return JS_ThrowTypeError(ctx, "required argument \"%s\" is missing",
                                     c->args[k].name), -1;
    return 0;
}

static JSValue dyn_cmd_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    JSValue arr = JS_UNDEFINED, res = JS_EXCEPTION, opts = JS_UNDEFINED;
    JSValue positional = JS_UNDEFINED, subres = JS_UNDEFINED, sargs = JS_UNDEFINED;
    const char **av = NULL;
    int64_t n64 = 0;
    int n = 0, i, k, no_more = 0;
    uint32_t n_pos = 0;

    if (!c)
        return JS_EXCEPTION;
    if (argc < 1) {
        /* the contract: parse() with no argument parses the engine's own
           scriptArgs -- the tokens the engine itself was handed */
        JSValue g = JS_GetGlobalObject(ctx);
        if (!JS_IsException(g)) {
            sargs = JS_GetPropertyStr(ctx, g, "scriptArgs");
            JS_FreeValue(ctx, g);
            argv = (JSValueConst *)&sargs;
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
    if (n64 > 65536)
        return JS_ThrowRangeError(ctx, "Command.parse(argv): more than 65536 arguments");
    n = (int)n64;
    av = (const char **)calloc((size_t)(n ? n : 1), sizeof(char *));
    if (!av) {
        JS_FreeValue(ctx, sargs);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        av[i] = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!av[i]) goto done;
    }
    opts = JS_NewObject(ctx);
    positional = JS_NewArray(ctx);
    if (JS_IsException(opts) || JS_IsException(positional))
        goto done;
    /* Defaults first, so an explicit flag overwrites rather than merges. A
       copy, never the spec's own object: the result would otherwise mutate
       the registration across parses (variadic pushes, plain writes). */
    for (k = 0; k < c->n_opts; k++) {
        JSValue dv;
        if (JS_IsUndefined(c->opts[k].dflt))
            continue;
        dv = dyn_default_copy(ctx, c->opts[k].dflt, 4);
        if (JS_IsException(dv) || JS_SetPropertyStr(ctx, opts, c->opts[k].longname, dv) < 0)
            goto done;
    }

    {   /* a subcommand claims the first non-option token and all after it */
        JSValue sub = dyn_cmd_dispatch(ctx, c, av, n, opts, positional, &n_pos,
                                       &no_more);
        if (JS_IsException(sub)) goto done;
        /* dispatch took its OWN references via JS_DupValue, so the locals are
           still ours and `done:` must free them -- nulling them here leaked
           both objects and tripped the gc_obj_list assertion at teardown. */
        if (!JS_IsUndefined(sub)) { res = sub; goto done; }
    }

    for (i = 0; i < n; ) {
        int used = dyn_parse_one(ctx, c, opts, av, n, i, positional, &n_pos, &no_more);
        if (used < 0) goto done;
        i += used;
    }
    if (dyn_cmd_check_required(ctx, c, opts, n_pos) < 0)
        goto done;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) goto done;
    JS_SetPropertyStr(ctx, res, "options", opts);
    JS_SetPropertyStr(ctx, res, "arguments", positional);
    JS_SetPropertyStr(ctx, res, "command", JS_NULL);
    opts = positional = JS_UNDEFINED;
 done:
    for (i = 0; i < n; i++)
        if (av[i]) JS_FreeCString(ctx, av[i]);
    free(av);
    JS_FreeValue(ctx, opts);
    JS_FreeValue(ctx, positional);
    JS_FreeValue(ctx, subres);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, sargs);
    return res;
}

/* --------------------------------------------------------------- help */

static JSValue dyn_cmd_help(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    dyn_sb_t b;
    JSValue r;
    int i;
    (void)argc; (void)argv;
    if (!c)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    dyn_sb_puts(&b, "Usage: ");
    dyn_sb_puts(&b, c->name ? c->name : "program");
    if (c->n_opts) dyn_sb_puts(&b, " [options]");
    if (c->n_subs) dyn_sb_puts(&b, " <command>");
    for (i = 0; i < c->n_args; i++) {
        dyn_sb_putc(&b, ' ');
        dyn_sb_putc(&b, c->args[i].required ? '<' : '[');
        dyn_sb_puts(&b, c->args[i].name);
        if (c->args[i].variadic) dyn_sb_puts(&b, "...");
        dyn_sb_putc(&b, c->args[i].required ? '>' : ']');
    }
    dyn_sb_putc(&b, '\n');
    if (c->desc) { dyn_sb_putc(&b, '\n'); dyn_sb_puts(&b, c->desc); dyn_sb_putc(&b, '\n'); }
    if (c->n_opts) {
        /* Flag columns align: one shared width, computed from the longest
           spec and capped so one long name cannot push every description to
           the far edge. Padding stays byte-based; displayWidth is the one
           owner of terminal width math and wraps what is emitted here. */
        int colw = 0;
        for (i = 0; i < c->n_opts; i++) {
            /* the prefix each line emits: "  " + the 4-char short slot +
               "--" + the long name + " <ph>" */
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
            col = b.n;               /* start of this line's flag text */
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
            /* pad to the shared description column only when something
               follows: no trailing whitespace on bare flag lines */
            if ((c->opts[i].desc && c->opts[i].desc[0]) || c->opts[i].required
                || !JS_IsUndefined(c->opts[i].dflt))
                while (b.n - col < (size_t)colw)
                    dyn_sb_putc(&b, ' ');
            {
                int tail = 0;    /* anything emitted after the flag column */
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
                        const char *d = JS_ToCString(ctx, s);
                        if (d) {
                            dyn_sb_puts(&b, tail ? " (default: " : "  (default: ");
                            dyn_sb_puts(&b, d);
                            dyn_sb_putc(&b, ')');
                            JS_FreeCString(ctx, d);
                        }
                    }
                    JS_FreeValue(ctx, s);
                }
            }
            dyn_sb_putc(&b, '\n');
        }
    }
    if (c->n_subs) {
        int namew = 0;
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t *sc = dyn_cmd_of(ctx, c->subs[i]);
            int w;
            if (!sc || !sc->name) continue;
            w = 2 + (int)strlen(sc->name);
            if (w > namew)
                namew = w;
        }
        if (namew > 20)
            namew = 20;
        dyn_sb_puts(&b, "\nCommands:\n");
        for (i = 0; i < c->n_subs; i++) {
            dyn_cmd_t *sc = dyn_cmd_of(ctx, c->subs[i]);
            size_t col;
            if (!sc) continue;
            col = b.n;               /* start of this line's name */
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
    if (b.oom) { dyn_sb_free(&b); return JS_ThrowOutOfMemory(ctx); }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static JSValue dyn_cmd_get_name(JSContext *ctx, JSValueConst this_val)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->name ? c->name : "");
}

static JSValue dyn_cmd_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = (dyn_cmd_t *)calloc(1, sizeof *c);
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { c->name = dyn_dup_span(s, strlen(s)); JS_FreeCString(ctx, s); }
    }
    return dyn_plain_wrap(ctx, new_target, dyn_cmd_class_id, c, NULL);
}

static const JSCFunctionListEntry dyn_cmd_proto[] = {
    JS_CFUNC_DEF("describe", 1, dyn_cmd_describe),
    JS_CFUNC_DEF("option", 1, dyn_cmd_option),
    JS_CFUNC_DEF("argument", 1, dyn_cmd_argument),
    JS_CFUNC_DEF("command", 1, dyn_cmd_command),
    JS_CFUNC_DEF("allowUnknown", 1, dyn_cmd_allow_unknown),
    JS_CFUNC_DEF("parse", 1, dyn_cmd_parse),
    JS_CFUNC_DEF("help", 0, dyn_cmd_help),
    JS_CGETSET_DEF("name", dyn_cmd_get_name, NULL),
};

/* ------------------------------------------------------------ Style, TTY */

/* Node's util.styleText signature, deliberately: shipping a third spelling
   after chalk and styleText would be the synonym the conventions forbid. The
   names and bytes match Node's set exactly (verified against v22): the same
   46 names, open/close pairs, and "reset" closes with a second 0m. */
typedef struct { const char *name; const char *on; const char *off; } dyn_style_t;
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

/* Length-aware lookup: style names arrive as arbitrary JS strings, and a name
   holding a NUL byte must not match its truncated prefix -- "red\0evil" is not
   "red" any more than "chartreuse" is. */
static const dyn_style_t *dyn_style_find(const char *n, size_t nl)
{
    size_t i;
    for (i = 0; i < countof(DYN_STYLES); i++)
        if (strlen(DYN_STYLES[i].name) == nl && memcmp(DYN_STYLES[i].name, n, nl) == 0)
            return &DYN_STYLES[i];
    return NULL;
}

/* StyleText(style|style[], text) -> string. An unknown style is REFUSED, not
   ignored: a silently-dropped style is a bug nobody sees. */
static JSValue dyn_style_text(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char *txt = NULL;
    size_t txt_len = 0;
    const dyn_style_t **sts = NULL;
    int64_t n_st = 1, k;
    int is_arr;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "StyleText(style, text): two arguments required");
    is_arr = JS_IsArray(ctx, argv[0]);
    if (!is_arr && !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "StyleText(style, text): style must be a string or array");
    /* Node refuses a non-string text (ERR_INVALID_ARG_TYPE) rather than
       coercing: a styled "null" in a terminal is a bug nobody sees. */
    if (!JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "StyleText(style, text): text must be a string");
    /* JS_ToCStringLen, not JS_ToCString: a JS string may hold NUL bytes, and
       styling must neither drop the text after one nor let a truncated style
       name look up as its prefix. */
    txt = JS_ToCStringLen(ctx, &txt_len, argv[1]);
    if (!txt)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    if (is_arr) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToInt64(ctx, &n_st, lv)) { JS_FreeValue(ctx, lv); goto done; }
        JS_FreeValue(ctx, lv);
        if (n_st < 0)
            n_st = 0;                       /* a Proxy may report a negative length */
    }
    /* The close pass must undo exactly what the open pass did -- a getter in
       the array runs arbitrary JS and can mutate it between the two passes --
       so the resolved styles are snapshotted here instead of being re-derived
       on close (which also paid a second JS_ToCString per name). */
    if (n_st > 0) {
        /* A length a 32-bit size_t cannot express must fail cleanly here,
           not wrap the allocation and smear past the buffer below. */
        if ((uint64_t)n_st > SIZE_MAX / sizeof *sts) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        sts = (const dyn_style_t **)malloc((size_t)n_st * sizeof *sts);
        if (!sts) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    for (k = 0; k < n_st; k++) {
        JSValue sv = is_arr ? JS_GetPropertyUint32(ctx, argv[0], (uint32_t)k)
                            : JS_DupValue(ctx, argv[0]);
        size_t nl = 0;
        const char *nm = JS_IsException(sv) ? NULL : JS_ToCStringLen(ctx, &nl, sv);
        const dyn_style_t *st = nm ? dyn_style_find(nm, nl) : NULL;
        JS_FreeValue(ctx, sv);
        if (!st) {
            /* Node's class for an invalid format is TypeError; match it */
            JS_ThrowTypeError(ctx, "StyleText: unknown style \"%s\"", nm ? nm : "?");
            if (nm) JS_FreeCString(ctx, nm);
            goto done;
        }
        JS_FreeCString(ctx, nm);
        sts[k] = st;
        dyn_sb_puts(&b, st->on);
    }
    dyn_sb_put(&b, txt, txt_len);
    for (k = n_st - 1; k >= 0; k--)              /* close in reverse order */
        dyn_sb_puts(&b, sts[k]->off);
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto done; }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
 done:
    free(sts);
    dyn_sb_free(&b);
    JS_FreeCString(ctx, txt);
    return r;
}

static JSValue dyn_styles_list(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    (void)argc; (void)argv;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < countof(DYN_STYLES); i++)
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
            JS_NewString(ctx, DYN_STYLES[i].name), JS_PROP_C_W_E);
    return arr;
}

/* IsTTY(fd) / Columns() / ColorDepth(): the three questions a CLI asks before
   deciding whether to style anything. */
/* argc/argv are taken whole so the argc>0 guard precedes any argv[0] read:
   a length-0 JS_CFUNC call may hand us argv with no readable element. */
static int dyn_fd_of(JSContext *ctx, int argc, JSValueConst *argv,
                     int32_t *pfd, const char *who)
{
    int32_t fd = 1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        /* an fd argument must be a number: a silent ToInt32("banana") would
           quietly probe fd 0 (stdin) and answer a question nobody asked */
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

static JSValue dyn_is_tty(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    int32_t fd;
    if (dyn_fd_of(ctx, argc, argv, &fd, "IsTTY"))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd) == 1);
}

static JSValue dyn_columns(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *e = getenv("COLUMNS");
    (void)argc; (void)argv;
    if (e && *e) {
        /* The whole value must parse (same rule as dyn_number_span): a
           partial strtol would turn "1e9" into 1 column and silently wreck
           any layout built on the width. */
        char *end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v > 0 && v < 100000)
            return JS_NewInt32(ctx, (int32_t)v);
    }
    return JS_NewInt32(ctx, 80);                 /* the documented fallback */
}

/* 0 = no colour, 4 = 16, 8 = 256, 24 = truecolor. FORCE_COLOR, when set and
   non-empty, overrides everything -- the TTY probe included, which is the
   point of forcing: "0"/"false" refuse, "2" is 256, "3" is truecolor, and
   "1"/"true"/anything else is 16-colour (Node's convention). Otherwise
   NO_COLOR, set and non-empty, is a user's explicit refusal, not a hint. */
static JSValue dyn_color_depth(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const char *e;
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
        return JS_NewInt32(ctx, 4);   /* "1", "true", any other non-empty */
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

static const JSCFunctionListEntry dyn_term_funcs[] = {
    JS_CFUNC_DEF("StyleText", 2, dyn_style_text),
    JS_CFUNC_DEF("Styles", 0, dyn_styles_list),
    JS_CFUNC_DEF("IsTTY", 0, dyn_is_tty),
    JS_CFUNC_DEF("Columns", 0, dyn_columns),
    JS_CFUNC_DEF("ColorDepth", 0, dyn_color_depth),
};

static int dyn_term_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_cmd_class_id, &dyn_cmd_class,
                                 dyn_cmd_proto, countof(dyn_cmd_proto),
                                 dyn_cmd_ctor, "Command") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

int js_nat_init_term(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:cli", dyn_term_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Command");
    return JS_AddModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TERM */
