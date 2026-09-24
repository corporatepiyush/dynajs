/* dyna:cli -- argument parsing, terminal styling and TTY queries.
   NOT src/dyna-cli.c: that name is the standalone interpreter's main().
   Full API: see the dyna:* module in dyna-libc.h. */
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

/* strict options: every options bag rejects unknown keys with a TypeError
 * naming the key and the valid set. Non-object bags count as absent; symbol
 * and inherited keys are invisible (own enumerable string keys only). */
static int dyn_cli_opts_strict(JSContext *ctx, JSValueConst opts,
                               const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;
    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { bad = 1; break; }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) { JS_FreeCString(ctx, name); bad = 1; break; }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
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
    char *env;              /* "NAME", or NULL: env-var default source */
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
    char *version;            /* NULL when .version() never set */
    JSValue action;           /* JS_UNDEFINED when .action() never set */
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
        free(c->opts[i].env);
        JS_FreeValueRT(rt, c->opts[i].dflt);
    }
    for (i = 0; i < c->n_args; i++) { free(c->args[i].name); free(c->args[i].desc); }
    for (i = 0; i < c->n_subs; i++) JS_FreeValueRT(rt, c->subs[i]);
    JS_FreeValueRT(rt, c->action);
    free(c->name); free(c->desc); free(c->version);
    free(c);
}

static void dyn_cmd_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_cmd_t *c = (dyn_cmd_t *)JS_GetOpaque(val, dyn_cmd_class_id);
    if (c)
        dyn_cmd_free_rt(rt, c);
}

/* The struct holds JSValues -- option defaults, subcommands and the action
   handler -- so the cycle collector must see them or a Command in a cycle
   never frees. */
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
    JS_MarkValue(rt, c->action, mark);
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
        free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
        return JS_ThrowTypeError(ctx,
            "Command.option(flags): a long name (--name) is required");
    }
    JS_FreeCString(ctx, flags);
    {   /* A duplicate long name would silently shadow the first spelling in
           both the parsed object and the help text -- and the same is true of
           a reused short name, whose lookups always resolve to the first
           registration. Refuse both at the declaration, where the mistake is
           visible. "--version" also collides with the automatic version flag
           (dyn_cmd_version checks the mirror case). */
        int i;
        if (!strcmp(o->longname, "version") && c->version) {
            JSValue ex = JS_ThrowTypeError(ctx,
                "Command.option(flags): \"--version\" collides with the automatic version flag");
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return ex;
        }
        for (i = 0; i < c->n_opts; i++) {
            if (!strcmp(c->opts[i].longname, o->longname)) {
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"--%s\"",
                    o->longname);
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                return ex;
            }
            if (o->shortname && c->opts[i].shortname
                && !strcmp(c->opts[i].shortname, o->shortname)) {
                JSValue ex = JS_ThrowTypeError(ctx,
                    "Command.option(flags): duplicate option \"-%s\"",
                    o->shortname);
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                return ex;
            }
        }
    }
    if (argc > 1 && JS_IsString(argv[1]) && (d = JS_ToCString(ctx, argv[1])) != NULL) {
        o->desc = dyn_dup_span(d, strlen(d));
        JS_FreeCString(ctx, d);
    }
    o->type = o->placeholder ? DYN_T_STRING : DYN_T_BOOL;
    /* An absent bag is undefined or null (JS convention); any other
       non-object is refused loudly -- silently ignoring a 42 here is exactly
 the failure mode. */
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        static const char *const opt_keys[] = {
            "type", "required", "variadic", "default", "env"
        };
        JSValue v;
        if (!JS_IsObject(argv[2])) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return JS_ThrowTypeError(ctx,
                "Command.option(flags, desc, opts): opts must be an object");
        }
        if (dyn_cli_opts_strict(ctx, argv[2], opt_keys, 5)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "type");
        if (JS_IsException(v)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return JS_EXCEPTION;
        }
        if (JS_IsString(v)) {
            const char *t = JS_ToCString(ctx, v);
            if (t) {
                if      (!strcmp(t, "boolean")) o->type = DYN_T_BOOL;
                else if (!strcmp(t, "number"))  o->type = DYN_T_NUMBER;
                else if (!strcmp(t, "string"))  o->type = DYN_T_STRING;
                else {
                    JS_FreeCString(ctx, t); JS_FreeValue(ctx, v);
                    free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                    return JS_ThrowRangeError(ctx,
                        "Command.option({type}): type must be boolean, string or number");
                }
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "required");
        if (JS_IsException(v)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return JS_EXCEPTION;
        }
        o->required = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "variadic");
        if (JS_IsException(v)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            return JS_EXCEPTION;
        }
        o->variadic = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        o->dflt = JS_GetPropertyStr(ctx, argv[2], "default");
        if (JS_IsException(o->dflt)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            o->dflt = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, argv[2], "env");
        if (JS_IsException(v)) {
            free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
            JS_FreeValue(ctx, o->dflt); o->dflt = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v)) {
            const char *e;
            size_t elen;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                JS_FreeValue(ctx, o->dflt); o->dflt = JS_UNDEFINED;
                return JS_ThrowTypeError(ctx,
                    "Command.option({env}): env must be a string");
            }
            e = JS_ToCStringLen(ctx, &elen, v);
            JS_FreeValue(ctx, v);
            if (!e) {
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                JS_FreeValue(ctx, o->dflt); o->dflt = JS_UNDEFINED;
                return JS_EXCEPTION;
            }
            if (elen == 0 || memchr(e, '\0', elen) != NULL || strchr(e, '=') != NULL) {
                JS_FreeCString(ctx, e);
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                JS_FreeValue(ctx, o->dflt); o->dflt = JS_UNDEFINED;
                return JS_ThrowTypeError(ctx,
                    "Command.option({env}): env must be a non-empty variable name without '='");
            }
            o->env = dyn_dup_span(e, elen);
            JS_FreeCString(ctx, e);
            if (!o->env) {
                free(o->shortname); free(o->longname); free(o->placeholder); free(o->desc); free(o->env);
                JS_FreeValue(ctx, o->dflt); o->dflt = JS_UNDEFINED;
                return JS_ThrowOutOfMemory(ctx);
            }
        } else {
            JS_FreeValue(ctx, v);
        }
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

/* Does the token spell a finite DECIMAL number? Optional ASCII whitespace on
   both ends, an optional sign, digits with an optional fraction and an
   optional e/E exponent. JavaScript's own string-to-number rules are declined
   as the coercion grammar: "0x10" would be 16, "Infinity" would be a valid
   double and "NaN" a valid NaN, each silently -- none of them is what
   "expects a number" promises. CLI values and env-var values are validated
   through this one grammar, so the two can never drift apart. 1 = accepted
   (the value lands in *out, overflow to infinity refused), 0 = refused. */
static int dyn_number_text(const char *s, double *out)
{
    const char *p = s;
    char *end;
    double d;
    int digits = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f'
           || *p == '\v')
        p++;
    if (*p == '+' || *p == '-')
        p++;
    while (*p >= '0' && *p <= '9') { p++; digits = 1; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { p++; digits = 1; }
    }
    if (!digits)
        return 0;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
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
        return 0;                 /* "1e999" is finite-looking but is not */
    *out = d;
    return 1;
}

/* Coerce a raw argv string to the option's DECLARED type. yargs-style implicit
   coercion is declined: a guessed type is how a CLI surprises its caller. */
static JSValue dyn_coerce(JSContext *ctx, const dyn_opt_t *o, const char *s)
{
    if (o->type == DYN_T_NUMBER) {
        double d;
        if (!dyn_number_text(s, &d)) {
            char *esc = dyn_arg_esc(s);   /* the value is attacker-controlled */
            JS_ThrowTypeError(ctx, "--%s expects a number, got \"%s\"",
                              o->longname, esc ? esc : s);
            free(esc);
            return JS_EXCEPTION;
        }
        return JS_NewFloat64(ctx, d);
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

/* Env-var defaults: each option with {env: "NAME"} reads getenv(NAME) -- but
 * ONLY when the command line did not supply that option. Env is the fallback
 * for an absent flag, not a participant in a fight the CLI already won: a
 * poisoned variable must never refuse (or silently replace) an invocation
 * whose argv explicitly resolved the option, and an unusable variable is
 * therefore never even looked at in that case. Precedence per option:
 * CLI > env > default. Variadic: CLI values append onto the seeded array
 * exactly as they append onto a default; an env value (when consulted) seeds
 * a one-element array, replacing any default. Boolean env maps
 * 0/false/no/n/off/empty (case-insensitive) to false and any other non-empty
 * value to true. Number env is validated through the SAME strict decimal
 * grammar as a CLI value (dyn_coerce); a bad number throws naming the
 * variable. */
static int dyn_env_apply(JSContext *ctx, dyn_cmd_t *c, JSValueConst opts,
                         const unsigned char *supplied)
{
    int k;
    for (k = 0; k < c->n_opts; k++) {
        const char *ev;
        dyn_opt_t *o = &c->opts[k];
        JSValue v;
        if (!o->env || supplied[k])
            continue;
        ev = getenv(o->env);
        if (!ev)
            continue;
        if (o->type == DYN_T_BOOL) {
            int b;
            if (ev[0] == '\0' || !strcasecmp(ev, "0") || !strcasecmp(ev, "false") ||
                !strcasecmp(ev, "no") || !strcasecmp(ev, "n") || !strcasecmp(ev, "off"))
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
                                      JS_NewBool(ctx, b)) < 0)
                    return -1;
            }
        } else if (o->type == DYN_T_NUMBER) {
            JSValue cv = dyn_coerce(ctx, o, ev);
            if (JS_IsException(cv)) {
                /* name the variable: the --long message alone misleads */
                JS_FreeValue(ctx, JS_GetException(ctx));
                {
                    char *esc = dyn_arg_esc(ev);
                    JS_ThrowTypeError(ctx,
                        "env \"%s\" for --%s expects a number, got \"%s\"",
                        o->env, o->longname, esc ? esc : ev);
                    free(esc);
                }
                return -1;
            }
            if (o->variadic) {
                JSValue arr = JS_NewArray(ctx);
                if (JS_IsException(arr)) { JS_FreeValue(ctx, cv); return -1; }
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
                if (JS_IsException(arr)) { JS_FreeValue(ctx, v); return -1; }
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

/* --version scan: 1 when argv holds a standalone --version token in
 * [start, end), 2 when --version=... (flag takes no value), 0 otherwise.
 * Respects the -- terminator and option-value ownership via dyn_scan_arity,
 * so a value that merely spells --version never triggers. */
static int dyn_version_scan(dyn_cmd_t *c, const char **av, int n, int start, int end)
{
    int i = start;
    while (i < end) {
        const char *a = av[i];
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
        /* option token: exact --version hits, --version=... is refused */
        if (!strcmp(a, "--version"))
            return 1;
        if (!strncmp(a, "--version=", 10))
            return 2;
        i += arity;
    }
    return 0;
}

/* Index of the first plain token that names a subcommand, or -1. Mirrors the
 * dispatch scan (terminator, arity) without parsing anything. */
static int dyn_sub_idx(JSContext *ctx, dyn_cmd_t *c, const char **av, int n)
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
                dyn_cmd_t *sc = dyn_cmd_of(ctx, c->subs[k]);
                if (sc && sc->name && !strcmp(sc->name, av[i]))
                    return i;
            }
            return -1; /* first plain token is not a subcommand */
        }
        i += arity - 1;
    }
    return -1;
}

/* Which options does the command line itself supply? The env-var fallback is
   skipped for exactly these, so parsing resolves the CLI first and consults
   (and validates) a variable only where the CLI was silent. Read-only: it
   mirrors dyn_parse_one's token recognition without parsing anything -- the
   `--` terminator, option-value ownership via dyn_scan_arity (a "--level"
   that is merely another option's VALUE is not a supply), --no- negation and
   short bundles (a bundle's tail belongs to its value option). */
static void dyn_supplied_scan(dyn_cmd_t *c, const char **av, int n,
                              unsigned char *supplied)
{
    int i;
    i = 0;
    while (i < n) {
        const char *a = av[i];
        size_t alen = strlen(a);
        int arity;
        if (!strcmp(a, "--"))
            break;                      /* past the terminator: all positional */
        arity = dyn_scan_arity(c, a);
        if (arity == 0) {
            i++;
            continue;
        }
        if (a[1] == '-') {
            const char *eq = strchr(a + 2, '=');
            size_t nlen = eq ? (size_t)(eq - (a + 2)) : alen - 2;
            dyn_opt_t *o = dyn_find_long(c, a + 2, nlen);
            if (!o && nlen > 3 && memcmp(a + 2, "no-", 3) == 0) {
                dyn_opt_t *neg = dyn_find_long(c, a + 5, nlen - 3);
                if (neg && neg->type == DYN_T_BOOL)
                    o = neg;            /* --no-x only negates flags */
            }
            if (o)
                supplied[o - c->opts] = 1;
        } else {
            size_t k;
            for (k = 1; k < alen; k++) {
                dyn_opt_t *o = dyn_find_short(c, a[k]);
                if (!o)
                    break;              /* an unknown short ends the bundle */
                supplied[o - c->opts] = 1;
                if (o->type != DYN_T_BOOL)
                    break;              /* the rest is this option's value */
            }
        }
        i += arity;
    }
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
    unsigned char supplied[DYN_CMD_MAX_OPTS];
    int64_t n64 = 0;
    int n = 0, i, k, no_more = 0;
    uint32_t n_pos = 0;

    if (!c)
        return JS_EXCEPTION;
    memset(supplied, 0, sizeof supplied);
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
    if (n64 > 65536) {
        /* through the shared cleanup: on the no-arg path sargs holds a live
           reference to the scriptArgs array, and a bare return here left it
           unreleased -- the runtime then aborts at teardown asserting its GC
           list is empty */
        JS_ThrowRangeError(ctx, "Command.parse(argv): more than 65536 arguments");
        goto done;
    }
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
    /* resolve the CLI first: know which options it supplies before any env
       var is consulted (or validated) for the rest */
    dyn_supplied_scan(c, av, n, supplied);
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
    /* Env-var defaults sit between spec defaults and CLI: consulted only for
       options the CLI did not supply, so the command line wins by never
       letting the variable participate at all. */
    if (dyn_env_apply(ctx, c, opts, supplied) < 0)
        goto done;

    /* --version precedes dispatch only in the prefix: `tool --version sub`
       answers the root version; `tool sub --version` dispatches so the
       subcommand answers. With no subcommand the whole argv is the scope. */
    if (c->version) {
        int sidx = dyn_sub_idx(ctx, c, av, n);
        int vend = (sidx >= 0) ? sidx : n;
        int hit = dyn_version_scan(c, av, n, 0, vend);
        if (hit == 2)
            goto done_throw_version_value;
        if (hit == 1) {
            res = JS_NewObject(ctx);
            if (JS_IsException(res)) goto done;
            JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
            JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
            JS_SetPropertyStr(ctx, res, "command", JS_NULL);
            JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
            goto done;
        }
        if (sidx < 0) {
            /* no subcommand: a --version past the would-be dispatch point is
               the same flag (e.g. `tool --env prod --version`) */
            hit = dyn_version_scan(c, av, n, vend, n);
            if (hit == 2)
                goto done_throw_version_value;
            if (hit == 1) {
                res = JS_NewObject(ctx);
                if (JS_IsException(res)) goto done;
                JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
                JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
                JS_SetPropertyStr(ctx, res, "command", JS_NULL);
                JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
                goto done;
            }
        }
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

    /* No dispatch: a late --version still wins over required checks. */
    if (c->version) {
        int hit = dyn_version_scan(c, av, n, 0, n);
        if (hit == 2)
            goto done_throw_version_value;
        if (hit == 1) {
            res = JS_NewObject(ctx);
            if (JS_IsException(res)) goto done;
            JS_SetPropertyStr(ctx, res, "options", JS_DupValue(ctx, opts));
            JS_SetPropertyStr(ctx, res, "arguments", JS_DupValue(ctx, positional));
            JS_SetPropertyStr(ctx, res, "command", JS_NULL);
            JS_SetPropertyStr(ctx, res, "version", JS_NewString(ctx, c->version));
            goto done;
        }
    }

    for (i = 0; i < n; ) {
        int used = dyn_parse_one(ctx, c, opts, av, n, i, positional, &n_pos, &no_more);
        if (used < 0) goto done;
        /* a registered --version option would have been consumed above; a
           literal --version token reaching the parser means no version was
           declared, so it falls through to the unknown-option refusal */
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
    /* A leaf action observes the settled result and its return becomes
       `result`. Dispatch leaves take the subcommand path above, so only the
       leaf that actually matched runs its handler. */
    if (!JS_IsUndefined(c->action)) {
        JSValue o = JS_GetPropertyStr(ctx, res, "options");
        JSValue a = JS_GetPropertyStr(ctx, res, "arguments");
        JSValue rval, args2[2];
        if (JS_IsException(o) || JS_IsException(a)) {
            JS_FreeValue(ctx, o); JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, res); res = JS_EXCEPTION;
            goto done;
        }
        args2[0] = o; args2[1] = a;
        rval = JS_Call(ctx, c->action, this_val, 2, args2);
        JS_FreeValue(ctx, o); JS_FreeValue(ctx, a);
        if (JS_IsException(rval)) {
            JS_FreeValue(ctx, res); res = JS_EXCEPTION;
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
    if (c->version) {
        dyn_sb_puts(&b, "\nVersion: ");
        dyn_sb_puts(&b, c->version);
        dyn_sb_putc(&b, '\n');
    }
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
                || !JS_IsUndefined(c->opts[i].dflt) || c->opts[i].env)
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
    c->action = JS_UNDEFINED;
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { c->name = dyn_dup_span(s, strlen(s)); JS_FreeCString(ctx, s); }
    }
    return dyn_plain_wrap(ctx, new_target, dyn_cmd_class_id, c, NULL);
}

/* .action(fn): registers the handler parse() invokes on a leaf match.
 * fn must be callable; replaces any previous handler. Chainable. */
static JSValue dyn_cmd_action(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
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

/* .version(v): with a string sets the version (chainable); with no argument
 * returns the current version ("" when unset). The version enables the
 * automatic --version flag in parse(). */
static JSValue dyn_cmd_version(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_cmd_t *c = dyn_cmd_of(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_NewString(ctx, c->version ? c->version : "");
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Command.version(v): v must be a string");
    {   /* the mirror collision check: a registered --version option must not
           be silently swallowed by the automatic flag */
        int i;
        for (i = 0; i < c->n_opts; i++)
            if (!strcmp(c->opts[i].longname, "version"))
                return JS_ThrowTypeError(ctx,
                    "Command.version(v): \"--version\" is already a registered option");
    }
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        char *dup;
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

/* extended colors: 256-color and truecolor forms alongside the 46 names.
 * Styles() still enumerates the 46 names; these dynamic forms are documented
 * beside it. All are length-aware (embedded NUL never matches).
 *   "256:<0-255>" fg 256 (38;5), "bg256:<0-255>" bg 256 (48;5),
 *   "#rrggbb" fg truecolor (38;2), "bg#rrggbb" bg truecolor (48;2),
 *   "rgb:r,g,b" fg truecolor, "bgRgb:r,g,b" bg truecolor (0-255 each).
 * Returns 1 and fills *is_bg + open/close on match, 0 when not an extended
 * form at all (caller throws unknown-style). Malformed extended shapes throw
 * TypeError; numeric components past 255 throw RangeError. */
static int dyn_hex_val(char ch, int *out)
{
    if (ch >= '0' && ch <= '9') { *out = ch - '0'; return 1; }
    if (ch >= 'a' && ch <= 'f') { *out = ch - 'a' + 10; return 1; }
    if (ch >= 'A' && ch <= 'F') { *out = ch - 'A' + 10; return 1; }
    return 0;
}
static int dyn_parse_256body(const char *p, size_t n, int *v)
{
    size_t i;
    int acc = 0;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    if (n > 3)         /* every all-decimal body of 4+ digits is past 255:
                          still the extended SHAPE, so RangeError not unknown */
        return -1;
    for (i = 0; i < n; i++)
        acc = acc * 10 + (p[i] - '0');
    if (acc < 0 || acc > 255)
        return -1;
    *v = acc;
    return 1;
}
static int dyn_parse_rgbbody(const char *p, size_t n, int *r, int *g, int *b)
{
    int vals[3] = {0,0,0};
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
            acc = 0; digits = 0;
            continue;
        }
        return 0;
    }
    /* flush the final component: a trailing separator is not required */
    if (digits == 0 || vi >= 3)
        return 0;
    vals[vi++] = acc;
    if (vi != 3)
        return 0;
    *r = vals[0]; *g = vals[1]; *b = vals[2];
    return 1;
}
static int dyn_style_extended(JSContext *ctx, const char *nm, size_t nl,
                              int *is_bg, char *openbuf, size_t openlen,
                              const char **close)
{
    /* 256:<n> */
    if (nl > 4 && memcmp(nm, "256:", 4) == 0) {
        int v = 0, rc = dyn_parse_256body(nm + 4, nl - 4, &v);
        if (rc == 0) return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: 256 color index out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[38;5;%dm", v);
        *is_bg = 0; *close = "\033[39m";
        return 1;
    }
    if (nl > 6 && memcmp(nm, "bg256:", 6) == 0) {
        int v = 0, rc = dyn_parse_256body(nm + 6, nl - 6, &v);
        if (rc == 0) return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: 256 color index out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[48;5;%dm", v);
        *is_bg = 1; *close = "\033[49m";
        return 1;
    }
    /* #rrggbb */
    if (nl == 7 && nm[0] == '#') {
        int h[6], i, ok = 1;
        for (i = 0; i < 6; i++)
            if (!dyn_hex_val(nm[1+i], &h[i])) ok = 0;
        if (!ok) return 0;
        snprintf(openbuf, openlen, "\033[38;2;%d;%d;%dm",
                 h[0]*16+h[1], h[2]*16+h[3], h[4]*16+h[5]);
        *is_bg = 0; *close = "\033[39m";
        return 1;
    }
    if (nl == 9 && memcmp(nm, "bg#", 3) == 0) {
        int h[6], i, ok = 1;
        for (i = 0; i < 6; i++)
            if (!dyn_hex_val(nm[3+i], &h[i])) ok = 0;
        if (!ok) return 0;
        snprintf(openbuf, openlen, "\033[48;2;%d;%d;%dm",
                 h[0]*16+h[1], h[2]*16+h[3], h[4]*16+h[5]);
        *is_bg = 1; *close = "\033[49m";
        return 1;
    }
    /* rgb:r,g,b */
    if (nl > 4 && memcmp(nm, "rgb:", 4) == 0) {
        int r = 0, g = 0, b = 0, rc = dyn_parse_rgbbody(nm + 4, nl - 4, &r, &g, &b);
        if (rc == 0) return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: rgb component out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[38;2;%d;%d;%dm", r, g, b);
        *is_bg = 0; *close = "\033[39m";
        return 1;
    }
    if (nl > 6 && memcmp(nm, "bgRgb:", 6) == 0) {
        int r = 0, g = 0, b = 0, rc = dyn_parse_rgbbody(nm + 6, nl - 6, &r, &g, &b);
        if (rc == 0) return 0;
        if (rc < 0) {
            JS_ThrowRangeError(ctx, "StyleText: rgb component out of range (0-255)");
            return -2;
        }
        snprintf(openbuf, openlen, "\033[48;2;%d;%d;%dm", r, g, b);
        *is_bg = 1; *close = "\033[49m";
        return 1;
    }
    return 0;
}

/* auto-dim: EMISSION is gated, VALIDATION is not. FORCE_COLOR, when set
   and non-empty, overrides everything ("0"/"false" refuse); otherwise
   NO_COLOR, set and non-empty, is a user's explicit refusal; otherwise a
   non-tty stdout dims. When dimmed, StyleText returns the text unchanged --
   but style NAMES are still validated, so a typo throws identically whatever
   the environment says. */
static int dyn_color_on(void)
{
    const char *e;
    if ((e = getenv("FORCE_COLOR")) != NULL && *e)
        return !(strcmp(e, "0") == 0 || strcmp(e, "false") == 0);
    if ((e = getenv("NO_COLOR")) != NULL && *e)
        return 0;
    return isatty(1) == 1;
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
    const char **closes = NULL;
    int64_t n_st = 1, k;
    int is_arr;
    int emit = dyn_color_on();          /*dimmed output is text only */

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
        if ((uint64_t)n_st > SIZE_MAX / sizeof *closes) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        closes = (const char **)malloc((size_t)n_st * sizeof *closes);
        if (!closes) {
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
        if (st) {
            JS_FreeCString(ctx, nm);
            closes[k] = st->off;
            if (emit)
                dyn_sb_puts(&b, st->on);
            continue;
        }
        if (nm) {
            char openbuf[32];
            const char *close = NULL;
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
            /* erc == 0: not an extended form, fall through to unknown-style */
        }
        {
            /* Node's class for an invalid format is TypeError; match it */
            JS_ThrowTypeError(ctx, "StyleText: unknown style \"%s\"", nm ? nm : "?");
            if (nm) JS_FreeCString(ctx, nm);
            goto done;
        }
    }
    dyn_sb_put(&b, txt, txt_len);
    if (emit)
        for (k = n_st - 1; k >= 0; k--)          /* close in reverse order */
            dyn_sb_puts(&b, closes[k]);
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto done; }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
 done:
    free(closes);
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

/* ============================ stdin keys and lines (input) ===== */

/* ---- the reader's byte queue ------------------------------------------------
   Bytes that were read but must still reach a reader: the decoder UNREADS a
   byte that cannot belong to the event being built (so a malformed sequence
   re-delivers it instead of swallowing it), and raw-mode teardown RESCUES the
   input that arrived during the raw window (so leaving raw mode cannot flush
   keystrokes the next reader -- a prompt() after a keypress() -- is waiting
   for). Every stdin read below passes through here first. A fixed ring on
   purpose: a malloc'd queue with no owning object would itself surface as a
   teardown leak. */
#define DYN_INQ_SIZE 16384u
static unsigned char dyn_inq[DYN_INQ_SIZE];
static unsigned int dyn_inq_head, dyn_inq_n;

static void dyn_inq_unread(unsigned char c)   /* back to the FRONT */
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq_head = (dyn_inq_head + DYN_INQ_SIZE - 1) & (DYN_INQ_SIZE - 1);
    dyn_inq[dyn_inq_head] = c;
    dyn_inq_n++;
}

static void dyn_inq_push(unsigned char c)     /* to the BACK */
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq[(dyn_inq_head + dyn_inq_n) & (DYN_INQ_SIZE - 1)] = c;
    dyn_inq_n++;
}

static int dyn_inq_pop(void)                  /* -1 when empty */
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
/* Raw mode for one interactive session: canonical input and echo off, and
   ISIG off so Ctrl-C reaches the decoder as a key instead of a signal. The
   saved attributes go back on EVERY exit path -- a prompt that leaves the
   terminal unechoed is worse than no prompt. A non-tty stdin is left alone:
   piped bytes need no termios to arrive one at a time. Both switches are
   TCSADRAIN, never TCSAFLUSH: input already queued belongs to the program
   (type-ahead is a feature) and must survive the round trip. */
static int dyn_raw_on(dyn_termios_t *saved)
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
static void dyn_raw_off(dyn_termios_t *saved, int on)
{
    if (!on)
        return;
    /* Before the terminal driver takes the terminal back, rescue whatever
       arrived during the raw window into the queue: keystrokes typed while a
       keypress() was pending must reach the NEXT reader (a prompt(), say)
       instead of being flushed with the mode switch. The read size is bounded
       by the queue's free space, so an oversized burst simply stays in the
       kernel rather than being dropped. */
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
static int dyn_raw_on(dyn_termios_t *saved) { (void)saved; return 0; }
static void dyn_raw_off(dyn_termios_t *saved, int on) { (void)saved; (void)on; }
#endif

/* One byte. 1 = read, 0 = EOF (or error), -1 = nothing within timeout_ms.
   A negative timeout blocks. On a regular file an exhausted read answers 0,
   so EOF is deterministic wherever the input comes from. */
static int dyn_byte_at(int fd, unsigned char *c, int timeout_ms)
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

/* ---- key decoding ----------------------------------------------------
   A key event carries the exact bytes it consumed ("sequence"), a name
   ("up", "enter", "a", ...) or NULL when the bytes decode to no known key,
 and the ctrl/meta/shift modifiers. Escapes decode CSI and forms
   (arrows, home/end, insert/delete, page up/down, shift+tab, and the
   CSI ...;mod~ / ;mod<letter> modifier encoding: 2=shift 3=meta 5=ctrl).
   Byte accounting is exact: an event never consumes a byte it does not put
   in `sequence`. A byte that cannot extend the current event (a
   non-continuation after a truncated UTF-8 lead; input past the 15-byte
   sequence cap; a CSI parameter past its 11-byte parameter buffer ends the
   sequence at that byte) is UNREAD and becomes the next event's input.
   Timing rule (inherent to the protocol, not fixable deterministically):
   a lone ESC is told apart from the head of a sequence only by a quiet
   window of DYN_KEY_ESC_MS, and the same window bounds every gap INSIDE a
   sequence (escape continuation, UTF-8 continuation). Identical bytes split
   across reads decode identically as long as the gaps stay inside the
   window; push a gap past it and the event before the gap ends where it
   stands. */
#define DYN_KEY_ESC_MS 50        /* a lone ESC vs the head of a sequence */

typedef struct {
    int eof;                     /* no byte at all: keypress() answers null */
    char seq[16];
    int seqlen;
    char chbuf[8];               /* the name when it is a character itself */
    const char *name;            /* chbuf or a static word, NULL when unknown */
    int ctrl, meta, shift;
} dyn_key_t;

static void dyn_key_csi(dyn_key_t *k, char *params, unsigned char f)
{
    int p0 = 0, mod = 0;
    const char *semi;
    if (params[0])
        p0 = (int)strtol(params, NULL, 10);
    semi = strchr(params, ';');
    if (semi && semi[1])
        mod = (int)strtol(semi + 1, NULL, 10) - 1;
    if (mod > 0) {
        k->shift |= (mod & 1) != 0;
        k->meta  |= (mod & 2) != 0;
        k->ctrl  |= (mod & 4) != 0;
    }
    switch (f) {
    case 'A': k->name = "up"; break;
    case 'B': k->name = "down"; break;
    case 'C': k->name = "right"; break;
    case 'D': k->name = "left"; break;
    case 'H': k->name = "home"; break;
    case 'F': k->name = "end"; break;
    case 'Z': k->name = "tab"; k->shift = 1; break;
    case '~':
        switch (p0) {
        case 1: k->name = "home"; break;
        case 2: k->name = "insert"; break;
        case 3: k->name = "delete"; break;
        case 4: k->name = "end"; break;
        case 5: k->name = "pageup"; break;
        case 6: k->name = "pagedown"; break;
        }
        break;
    }
}

/* One non-escape byte (or one UTF-8 character) into name/ctrl. Modifiers are
   left to the caller (ESC-prefix sets meta). Bytes 0x00-0x1f are ctrl chords:
   the name is the shifted control glyph ("a".."z" for 1..26, "@" for 0,
   "\\" for 28, ...) except the four keys that have their own name. */
static void dyn_key_base(dyn_key_t *k, int fd, unsigned char b)
{
    if (b == '\r' || b == '\n') { k->name = "enter"; return; }
    if (b == '\t') { k->name = "tab"; return; }
    if (b == 0x7f || b == 0x08) { k->name = "backspace"; return; }
    if (b == 0x1b) { k->name = "escape"; return; }
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
    {   /* UTF-8: name is the whole character or NULL when malformed */
        int need = (b >= 0xf0) ? 3 : (b >= 0xe0) ? 2 : (b >= 0xc0) ? 1 : 0;
        int i;
        if (need == 0)
            return;              /* a stray continuation byte: unknown */
        k->chbuf[0] = (char)b;
        for (i = 1; i <= need; i++) {
            unsigned char c;
            if (dyn_byte_at(fd, &c, DYN_KEY_ESC_MS) != 1)
                return;          /* truncated: unknown, sequence keeps its bytes */
            if ((c & 0xc0) != 0x80) {
                dyn_inq_unread(c);
                return;          /* NOT a continuation: this byte is its own key,
                                    not part of this one -- put back so the next
                                    event delivers it (a keystroke cannot vanish) */
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(c);
                return;          /* no room left to REPORT the byte: never consume
                                    what sequence cannot carry */
            }
            k->seq[k->seqlen++] = (char)c;
            k->chbuf[i] = (char)c;
        }
        k->chbuf[need + 1] = 0;
        k->name = k->chbuf;
    }
}

/* Read and decode one key event. Returns 1 with *k filled, or 0 at EOF. */
static int dyn_key_read(int fd, dyn_key_t *k)
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
        {   /* the head of an escape form: sequence, or the meta chord */
            unsigned char b1;
            if (dyn_byte_at(fd, &b1, DYN_KEY_ESC_MS) != 1) {
                k->name = "escape";  /* nothing followed within the window */
                return 1;
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(b1);
                k->name = "escape";  /* the sequence buffer is full: the byte is
                                        put back and the run ENDS here -- the tail
                                        is delivered to the next event instead of
                                        being consumed past what sequence reports */
                return 1;
            }
            k->seq[k->seqlen++] = (char)b1;
            if (b1 == 0x1b) {
                k->meta = 1;         /* ESC ESC ...: another form follows */
                b = b1;
                continue;
            }
            if (b1 == '[' || b1 == 'O') {
                char params[12];
                int np = 0;
                unsigned char f;
                for (;;) {
                    if (dyn_byte_at(fd, &f, DYN_KEY_ESC_MS) != 1)
                        return 1;    /* truncated escape: name stays NULL */
                    if (k->seqlen >= (int)sizeof k->seq - 1) {
                        dyn_inq_unread(f);
                        return 1;    /* no room to report it: it becomes the next
                                        event's input instead of vanishing */
                    }
                    k->seq[k->seqlen++] = (char)f;
                    if (f >= 0x30 && f <= 0x3f && np < (int)sizeof params - 1) {
                        params[np++] = (char)f;
                        continue;
                    }
                    break;
                }
                params[np] = 0;
                if (b1 == 'O') {     /*no parameter bytes */
                    switch (f) {
                    case 'A': k->name = "up"; break;
                    case 'B': k->name = "down"; break;
                    case 'C': k->name = "right"; break;
                    case 'D': k->name = "left"; break;
                    case 'H': k->name = "home"; break;
                    case 'F': k->name = "end"; break;
                    }
                    return 1;
                }
                dyn_key_csi(k, params, f);
                return 1;
            }
            k->meta = 1;             /* ESC + key is the meta chord */
            dyn_key_base(k, fd, b1);
            return 1;
        }
    }
}

static JSValue dyn_key_obj(JSContext *ctx, const dyn_key_t *k)
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

/* keypress() -> {name, sequence, ctrl, meta, shift} | null. One key event
   from stdin; null at EOF. Raw mode is engaged for the single read and
   released before the event is handed to JS. */
static JSValue dyn_keypress_fn(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_key_t k;
    dyn_termios_t saved;
    int raw;
    (void)this_val; (void)argc; (void)argv;
    raw = dyn_raw_on(&saved);
    dyn_key_read(0, &k);
    dyn_raw_off(&saved, raw);
    if (k.eof)
        return JS_NULL;
    return dyn_key_obj(ctx, &k);
}

/* ---- line input for prompt() and confirm() -------------------------------- */

#define DYN_LINE_MAX (1 << 20)

/* One line from fd: 1 = a line (or a trailing partial line at EOF), 0 = EOF
   with nothing read, -1 = past DYN_LINE_MAX of content (or OOM). The
   terminator is dropped; a CRLF loses the CR too. The cap counts the ANSWER,
   i.e. after that CR drop -- the buffer runs one byte long so a lone CR in a
   CRLF never counts against it, and the length is judged when the line ends.
   On the length refusal the REST of the over-long line is drained first, so
   the stream stays line-aligned for the next reader. */
static int dyn_read_line(int fd, dyn_sb_t *b)
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
            continue;                    /* discard the tail of a refused line */
        if (b->n >= DYN_LINE_MAX + 1) {
            over = 1;
            b->n = 0;                    /* the partial answer is refused wholesale */
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

/* prompt(message, {default}) -> string. The message is written verbatim to
   stdout and one line is read from stdin. An empty answer, or EOF, yields
   the default -- or "" when none was given. */
static JSValue dyn_prompt(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
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
        static const char *const pkeys[] = { "default" };
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
        /* best-effort prompt: a closing pty must not abort the read */
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

/* confirm(message) -> boolean. One line is read from stdin; "y"/"yes"
   (ASCII case-insensitive) answers true, EVERYTHING ELSE -- an empty line,
   "n", "maybe", EOF -- answers false. */
static JSValue dyn_confirm(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *msg;
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
        /* best-effort prompt: a closing pty must not abort the read */
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

/* select(message, options) -> string | null. A menu is rendered to stdout and
   moved with the arrow keys in raw mode; Enter returns the highlighted
   option, Escape (or Ctrl-C, or EOF) returns null. Every other key is
   ignored. The marker survives the redraws: ">" on the highlighted row. */
static void dyn_sel_draw(char *const *o, const size_t *ol, int n,
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
        dyn_sb_puts(&b, "\033[K\n");   /* rows may shrink as the marker moves */
    }
    if (b.p && b.n && write(1, b.p, b.n) < 0) {
        /* best-effort redraw: a closed tty loses one frame, not data */
    }
    dyn_sb_free(&b);
}

static JSValue dyn_select(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    const char *msg;
    size_t mlen = 0;
    char **o = NULL;
    size_t *ol = NULL;
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
    o = (char **)calloc((size_t)n, sizeof *o);
    ol = (size_t *)calloc((size_t)n, sizeof *ol);
    if (!o || !ol) {
        free(o); free(ol);
        JS_FreeCString(ctx, msg);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char *s;
        if (JS_IsException(v)) {
            int bad = i;
            while (--i >= 0)
                free(o[i]);
            free(o); free(ol);
            JS_FreeCString(ctx, msg);
            (void)bad;
            return JS_EXCEPTION;
        }
        if (!JS_IsString(v)) {   /* JS_ToCStringLen would silently stringify */
            int bad = i;
            JS_FreeValue(ctx, v);
            while (--i >= 0)
                free(o[i]);
            free(o); free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowTypeError(ctx,
                "select(message, options): option %d must be a string", bad);
        }
        s = JS_ToCStringLen(ctx, &ol[i], v);
        JS_FreeValue(ctx, v);
        /* the menu must render what was DECLARED: a getter that mutates the
           array between draw and answer would shift rows under the cursor */
        o[i] = dyn_dup_span(s, ol[i]);
        JS_FreeCString(ctx, s);
        if (!o[i]) {
            while (--i >= 0)
                free(o[i]);
            free(o); free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    /* Raw mode goes on BEFORE anything is drawn: a keystroke typed while the
       menu is being written must not race the mode switch (a signal-mode
       Ctrl-C written before raw entry is consumed by the terminal driver and
       lost to the program). */
    {
        dyn_termios_t saved;
        int raw = dyn_raw_on(&saved);
        if (mlen && write(1, msg, mlen) < 0) {
            /* best-effort prompt: a closing pty must not abort selection */
        }
        if (write(1, "\n", 1) < 0) {
            /* ditto */
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
            if ((k.name && !strcmp(k.name, "escape") && !k.ctrl && !k.meta) ||
                (k.ctrl && k.name && !strcmp(k.name, "c"))) {
                dyn_sel_draw(o, ol, n, cur, 1, 0);
                r = JS_NULL;
                break;
            }
            /* any other key is ignored: the menu stays as it is */
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

/* ---- ProgressBar ---------------------------------------------------
   ProgressBar({total}) with .update(n): one line redrawn in place with a
   20-cell bar, the percentage, the count and a time-based ETA. The line
   terminates with a newline exactly once -- when n first reaches total --
   and any update after that is refused. */
#define DYN_PBAR_CELLS 20

typedef struct {
    double total;
    time_t t0;
    int done;
} dyn_pbar_t;

static JSClassID dyn_pbar_class_id;

static void dyn_pbar_dispose(void *native)
{
    free(native);
}

static void dyn_pbar_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_pbar_t *p = (dyn_pbar_t *)JS_GetOpaque(val, dyn_pbar_class_id);
    (void)rt;
    if (p)
        dyn_pbar_dispose(p);
}

static JSClassDef dyn_pbar_class = {
    "ProgressBar", .finalizer = dyn_pbar_finalizer,
};

static JSValue dyn_pbar_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    static const char *const keys[] = { "total" };
    dyn_pbar_t *p;
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
    if (!(total > 0) || total > 1e15)   /* NaN, 0, negatives and +Inf alike */
        return JS_ThrowRangeError(ctx, "ProgressBar({total}): total must be positive");
    p = (dyn_pbar_t *)calloc(1, sizeof *p);
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->total = total;
    p->t0 = time(NULL);
    return dyn_plain_wrap(ctx, new_target, dyn_pbar_class_id, p, dyn_pbar_dispose);
}

/* update(n) -> this. n must land within 0..total; reaching total completes
   the bar (newline, done), and a later update is refused. */
static JSValue dyn_pbar_update(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_pbar_t *p = dyn_plain_get(ctx, this_val, dyn_pbar_class_id);
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
        snprintf(etxt, sizeof etxt, "?"); /* no rate yet: no estimate to show */
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
        /* best-effort progress: a closed tty drops the frame */
    }
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry dyn_pbar_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_pbar_update),
};

/* ---- Spinner -------------------------------------------------------
   Spinner({text?}) with start()/tick()/stop(text?): the classic "|/-\"
   frames on one redrawn line. Each method is strict about the lifecycle --
   tick before start and any use after stop are refused, not guessed at. */
typedef struct {
    char *text;
    int state;                   /* 0 = fresh, 1 = running, 2 = stopped */
    int frame;
} dyn_spin_t;

static JSClassID dyn_spin_class_id;

static void dyn_spin_dispose(void *native)
{
    dyn_spin_t *s = (dyn_spin_t *)native;
    if (s) {
        free(s->text);
        free(s);
    }
}

static void dyn_spin_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_spin_t *s = (dyn_spin_t *)JS_GetOpaque(val, dyn_spin_class_id);
    (void)rt;
    if (s)
        dyn_spin_dispose(s);
}

static JSClassDef dyn_spin_class = {
    "Spinner", .finalizer = dyn_spin_finalizer,
};

static JSValue dyn_spin_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    static const char *const keys[] = { "text" };
    dyn_spin_t *s;

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
            const char *t;
            size_t tl;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "Spinner({text}): text must be a string");
            }
            t = JS_ToCStringLen(ctx, &tl, v);
            JS_FreeValue(ctx, v);
            if (!t)
                return JS_EXCEPTION;
            s = (dyn_spin_t *)calloc(1, sizeof *s);
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
    s = (dyn_spin_t *)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->text = dyn_dup_span("", 0);
    if (!s->text) {
        free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_spin_class_id, s, dyn_spin_dispose);
}

static dyn_spin_t *dyn_spin_of(JSContext *ctx, JSValueConst this_val)
{
    return (dyn_spin_t *)dyn_plain_get(ctx, this_val, dyn_spin_class_id);
}

static void dyn_spin_draw(const dyn_spin_t *s)
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
        /* best-effort redraw: a closed tty loses one frame, not data */
    }
    dyn_sb_free(&b);
}

/* start() -> this. */
static JSValue dyn_spin_start(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_spin_t *s = dyn_spin_of(ctx, this_val);
    (void)argc; (void)argv;
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

/* tick() -> this. Advances one frame and redraws. */
static JSValue dyn_spin_tick(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_spin_t *s = dyn_spin_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.tick: %s",
                                  s->state == 0 ? "not started" : "already stopped");
    s->frame = (s->frame + 1) & 3;
    dyn_spin_draw(s);
    return JS_DupValue(ctx, this_val);
}

/* stop(text?) -> this. Clears the line; with a string, leaves it as the
   final line instead (followed by a newline). */
static JSValue dyn_spin_stop(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_spin_t *s = dyn_spin_of(ctx, this_val);
    const char *t = NULL;
    size_t tl = 0;
    if (!s)
        return JS_EXCEPTION;
    if (s->state != 1)
        return JS_ThrowRangeError(ctx, "Spinner.stop: %s",
                                  s->state == 0 ? "not started" : "already stopped");
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "Spinner.stop(text): text must be a string");
        /* validated BEFORE the state flips: a refused stop() leaves the
           spinner running exactly as it found it */
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
            /* best-effort: a closing pty must not abort enumeration */
        }
        dyn_sb_free(&b);
        JS_FreeCString(ctx, t);
    } else {
        if (write(1, "\r\033[K", 4) < 0) {
            /* best-effort clear: a closed tty drops the frame */
        }
    }
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry dyn_spin_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_spin_start),
    JS_CFUNC_DEF("tick", 0, dyn_spin_tick),
    JS_CFUNC_DEF("stop", 0, dyn_spin_stop),
};

/* ---- Table ---------------------------------------------------------
   Table(rows, {head?, align?, format?}) -> string. rows is an array of equal-
   length arrays of string|number cells. Columns size to their content; each
   column's alignment is "left" (default), "right" or "center". Formats:
   "grid" (padded, two-space gutter, a dashed rule under the head), "tsv"
   (raw cells joined by tabs -- TSV quotes nothing), "csv" (RFC 4180: quotes
   only what needs quoting, doubles embedded quotes). Every rendered row
   ends with a newline, head first. */

static int dyn_utf8_width(const char *s, size_t n)
{
    size_t i;
    int w = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            w++;
    return w;
}

static void dyn_sb_pad(dyn_sb_t *b, int n)
{
    while (n-- > 0)
        dyn_sb_putc(b, ' ');
}

static void dyn_csv_cell(dyn_sb_t *b, const char *s, size_t n)
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

static JSValue dyn_table(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    enum { DYN_ALIGN_LEFT, DYN_ALIGN_RIGHT, DYN_ALIGN_CENTER };
    JSValueConst rows, opts;
    JSValue head = JS_UNDEFINED;
    int64_t nrow64 = 0;
    uint32_t nrow = 0, ncols = 0, i, j;
    char **cells = NULL;         /* nrow*ncols strings */
    size_t ncells;               /* nrow*ncols + 1, guarded above */
    size_t *clen = NULL;
    int *cwid = NULL;
    char **hs = NULL;            /* head strings, ncols */
    size_t *hl = NULL;
    int *hw = NULL;
    int *align = NULL;
    int fmt_grid = 1, fmt_tsv = 0, fmt_csv = 0;
    int fc_owned = 0;
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char *fc = "grid";

    dyn_sb_init(&b);          /* freed on EVERY exit through cleanup */
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Table(rows, opts): rows must be an array");
    rows = argv[0];
    opts = (argc > 1 && !JS_IsNull(argv[1])) ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        static const char *const tkeys[] = { "head", "align", "format" };
        if (!JS_IsObject(opts))
            return JS_ThrowTypeError(ctx, "Table(rows, opts): opts must be an object");
        if (dyn_cli_opts_strict(ctx, opts, tkeys, 3))
            return JS_EXCEPTION;
    }
    {   /* column count comes from the head when given, else the first row */
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length");
        int64_t dummy = 0;
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &dummy, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
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
            int64_t hl = 0;
            if (JS_IsException(lv) || JS_ToInt64(ctx, &hl, lv)) {
                JS_FreeValue(ctx, lv); JS_FreeValue(ctx, head);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
            ncols = (uint32_t)(hl < 0 ? 0 : hl);
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
        return JS_NewString(ctx, "");     /* no head, no columns: no output */
    }
    if ((uint64_t)nrow * ncols > (uint64_t)1 << 28) {
        JS_FreeValue(ctx, head);
        return JS_ThrowRangeError(ctx, "Table(rows): too many cells");
    }
    /* the 2^28-cell guard above bounds the product long before size_t */
    ncells = (size_t)nrow * ncols + 1;
    cells = (char **)calloc(ncells, sizeof *cells);
    clen = (size_t *)calloc(ncells, sizeof *clen);
    cwid = (int *)calloc(ncells, sizeof *cwid);
    hs = (char **)calloc(ncols, sizeof *hs);
    hl = (size_t *)calloc(ncols, sizeof *hl);
    hw = (int *)calloc(ncols, sizeof *hw);
    align = (int *)calloc(ncols, sizeof *align);
    if (!cells || !clen || !cwid || !hs || !hl || !hw || !align)
        goto oom;
    if (!JS_IsUndefined(head)) {
        /* the head is SNAPSHOTTED: a getter could mutate the array between
           the width pass and the render passes otherwise */
        for (j = 0; j < ncols; j++) {
            JSValue hv = JS_GetPropertyUint32(ctx, head, j);
            const char *s;
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
    {   /* format + align, both strict */
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
                if (!strcmp(fc, "grid")) fmt_grid = 1;
                else if (!strcmp(fc, "tsv")) fmt_tsv = 1;
                else if (!strcmp(fc, "csv")) fmt_csv = 1;
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
                    JS_FreeValue(ctx, lv); JS_FreeValue(ctx, v);
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
                    const char *a = NULL;
                    if (!JS_IsException(ev))
                        a = JS_ToCString(ctx, ev);
                    JS_FreeValue(ctx, ev);
                    if (!a) {
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                    if (!strcmp(a, "left")) align[ai] = DYN_ALIGN_LEFT;
                    else if (!strcmp(a, "right")) align[ai] = DYN_ALIGN_RIGHT;
                    else if (!strcmp(a, "center")) align[ai] = DYN_ALIGN_CENTER;
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
    /* head + cells: strings and numbers only, widths and column count pinned */
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
            JS_FreeValue(ctx, lv); JS_FreeValue(ctx, row);
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
            const char *s;
            size_t idx = (size_t)i * ncols + j;
            if (JS_IsException(v)) { JS_FreeValue(ctx, row); goto fail; }
            if (JS_IsNumber(v)) {
                sv = JS_ToString(ctx, v);      /* numbers render, nothing else */
                JS_FreeValue(ctx, v);
            } else if (JS_IsString(v)) {
                sv = v;                        /* ownership moves to sv */
            } else {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, row);
                JS_ThrowTypeError(ctx,
                    "Table(rows): cell (%d, %d) must be a string or number",
                    (int)i, (int)j);
                goto fail;
            }
            if (JS_IsException(sv)) { JS_FreeValue(ctx, row); goto fail; }
            s = JS_ToCStringLen(ctx, &clen[idx], sv);
            JS_FreeValue(ctx, sv);
            if (!s) { JS_FreeValue(ctx, row); goto fail; }
            cells[idx] = dyn_dup_span(s, clen[idx]);
            JS_FreeCString(ctx, s);
            if (!cells[idx]) { JS_FreeValue(ctx, row); goto oom; }
            cwid[idx] = dyn_utf8_width(cells[idx], clen[idx]);
        }
        JS_FreeValue(ctx, row);
    }
    {
        size_t *w = (size_t *)calloc(ncols, sizeof *w);
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
                    if (align[j] == DYN_ALIGN_CENTER) { pl = pad / 2; pr = pad - pl; }
                    else if (align[j] == DYN_ALIGN_LEFT) { pl = 0; pr = pad; }
                    else { pl = pad; pr = 0; }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, hs[j], hl[j]);
                    if (j + 1 < ncols) {
                        dyn_sb_pad(&b, pr);
                        dyn_sb_puts(&b, "  ");
                    }
                }
                dyn_sb_putc(&b, '\n');
                for (j = 0; j < ncols; j++) {   /* the dashed rule under head */
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
                    if (align[j] == DYN_ALIGN_CENTER) { pl = pad / 2; pr = pad - pl; }
                    else if (align[j] == DYN_ALIGN_RIGHT) { pl = pad; pr = 0; }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, cells[idx], clen[idx]);
                    if (j + 1 < ncols) {          /* last column: no trailing pad */
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
        } else {                                 /* csv */
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
    free(cells); free(clen); free(cwid); free(hs); free(hl); free(hw);
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

static int dyn_term_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_cmd_class_id, &dyn_cmd_class,
                                 dyn_cmd_proto, countof(dyn_cmd_proto),
                                 dyn_cmd_ctor, "Command") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pbar_class_id, &dyn_pbar_class,
                                 dyn_pbar_proto, countof(dyn_pbar_proto),
                                 dyn_pbar_ctor, "ProgressBar") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_spin_class_id, &dyn_spin_class,
                                 dyn_spin_proto, countof(dyn_spin_proto),
                                 dyn_spin_ctor, "Spinner") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

int js_nat_init_term(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:cli", dyn_term_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Command");
    JS_AddModuleExport(ctx, m, "ProgressBar");
    JS_AddModuleExport(ctx, m, "Spinner");
    return JS_AddModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_TERM */
