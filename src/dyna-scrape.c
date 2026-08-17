/*
 * dyna:scrape -- the policy layer over fetch + parse (design 28).
 *
 * Owns NO parsing: fetching is dyna:net, HTML is dyna:html, URLs are dyna:url.
 * What lives here is the policy nobody gets right -- robots.txt, per-host
 * pacing, retry bounds, and a schema the extraction is checked against.
 *
 * NOT an evasion toolkit: no fingerprint spoofing, no CAPTCHA handling, no
 * ban-circumvention proxying. See §0 of the design; that is a decision.
 *
 * This TU currently implements `Robots` only.
 */
#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>
#include <poll.h>   /* poll(NULL,0,ms): the portable sleep */
#include "core/dyn-timer.h"   /* dyn_timer_now_ms: the per-host delay clock */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* robots.txt is attacker-influenced input: every bound is checked WHILE
   parsing, not after. A robots bomb is a real shape. */
#define RB_MAX_BYTES  (512 * 1024)
#define RB_MAX_RULES  1000
#define RB_MAX_PATH   2048

typedef struct {
    char    *path;      /* the pattern, percent-decoded */
    size_t   len;
    unsigned allow : 1; /* Allow (1) vs Disallow (0) */
} rb_rule_t;

typedef struct {
    rb_rule_t *rules;
    size_t     n, cap;
    double     delay;   /* Crawl-delay seconds, or -1 */
    char     **sitemaps;
    size_t     n_site, cap_site;
} rb_t;

static JSClassID dyn_rb_class_id;

static void rb_free(rb_t *r)
{
    size_t i;
    for (i = 0; i < r->n; i++)
        free(r->rules[i].path);
    free(r->rules);
    for (i = 0; i < r->n_site; i++)
        free(r->sitemaps[i]);
    free(r->sitemaps);
    free(r);
}

static void dyn_rb_finalizer(JSRuntime *rt, JSValue val)
{
    rb_t *r = JS_GetOpaque(val, dyn_rb_class_id);
    (void)rt;
    if (r)
        rb_free(r);
}

static const JSClassDef dyn_rb_class = {
    "Robots", .finalizer = dyn_rb_finalizer,
};

/* Percent-decode in place. RFC 9309 compares octets, so `/a%2Fb` and `/a/b`
   are DIFFERENT paths -- %2F is deliberately left encoded. */
static size_t rb_pct_decode(char *s, size_t n)
{
    size_t i = 0, o = 0;
    while (i < n) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = -1, lo = -1, k;
            char c;
            for (k = 0; k < 2; k++) {
                c = s[i + 1 + k];
                int v = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (k == 0) hi = v; else lo = v;
            }
            if (hi >= 0 && lo >= 0) {
                int v = hi * 16 + lo;
                if (v != '/') {          /* %2F stays encoded, per the spec */
                    s[o++] = (char)v;
                    i += 3;
                    continue;
                }
            }
        }
        s[o++] = s[i++];
    }
    return o;
}

static int rb_push_rule(rb_t *r, const char *p, size_t n, int allow)
{
    rb_rule_t *nr;
    char *copy;
    if (r->n >= RB_MAX_RULES)
        return 0;                        /* cap: refuse, do not grow */
    if (n > RB_MAX_PATH)
        n = RB_MAX_PATH;
    if (r->n == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 16;
        nr = (rb_rule_t *)realloc(r->rules, nc * sizeof(*nr));
        if (!nr) return -1;
        r->rules = nr; r->cap = nc;
    }
    copy = (char *)malloc(n + 1);
    if (!copy) return -1;
    memcpy(copy, p, n);
    copy[n] = 0;
    n = rb_pct_decode(copy, n);
    copy[n] = 0;
    r->rules[r->n].path = copy;
    r->rules[r->n].len = n;
    r->rules[r->n].allow = allow ? 1u : 0u;
    r->n++;
    return 0;
}

/* Glob match with '*' (any run) and '$' (end anchor) -- the only two the
   protocol defines. Iterative with a backtrack point, never recursive: the
   pattern is attacker-supplied. */
static int rb_match(const char *pat, size_t pn, const char *path, size_t sn)
{
    size_t pi = 0, si = 0, star = (size_t)-1, mark = 0;

    /* PREFIX semantics: an exhausted pattern is a match however much path is
       left -- `Disallow: /p` blocks `/p/x`. Only a trailing '$' anchors to the
       end. Iterative with one backtrack point; the pattern is untrusted. */
    while (pi < pn) {
        if (pat[pi] == '$' && pi + 1 == pn)
            return si == sn;
        if (pat[pi] == '*') {
            star = ++pi;
            mark = si;
            continue;
        }
        if (si < sn && pat[pi] == path[si]) {
            pi++; si++;
            continue;
        }
        if (star != (size_t)-1 && mark < sn) {
            pi = star;
            si = ++mark;
            continue;
        }
        return 0;
    }
    return 1;
}

/* One line's key and value, trimmed. Returns 0 if the line has no colon. */
static int rb_split(const char *l, size_t n, const char **k, size_t *kn,
                    const char **v, size_t *vn)
{
    size_t c = 0;
    while (c < n && l[c] != ':') c++;
    if (c == n) return 0;
    *k = l; *kn = c;
    while (*kn && (l[*kn - 1] == ' ' || l[*kn - 1] == '\t')) (*kn)--;
    c++;
    while (c < n && (l[c] == ' ' || l[c] == '\t')) c++;
    *v = l + c; *vn = n - c;
    while (*vn && ((*v)[*vn - 1] == ' ' || (*v)[*vn - 1] == '\t' ||
                   (*v)[*vn - 1] == '\r')) (*vn)--;
    return 1;
}

static int rb_ci_eq(const char *a, size_t an, const char *b)
{
    size_t i;
    for (i = 0; i < an && b[i]; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        if (ca != b[i]) return 0;
    }
    return i == an && !b[i];
}

/* A group whose User-agent matches `agent` wins over the `*` group. Both are
   collected in one pass; the specific one replaces the wildcard if found. */
static int rb_parse(rb_t *r, const char *s, size_t n, const char *agent)
{
    size_t i = 0;
    int in_star = 0, in_me = 0, seen_me = 0, prev_ua = 0;

    if (n > RB_MAX_BYTES)
        n = RB_MAX_BYTES;                /* cap while reading, not after */
    while (i < n) {
        size_t e = i, ln;
        const char *k, *v;
        size_t kn, vn;
        while (e < n && s[e] != '\n') e++;
        ln = e - i;
        {
            const char *line = s + i;
            size_t h = 0;
            while (h < ln && line[h] != '#') h++;   /* strip the comment */
            if (rb_split(line, h, &k, &kn, &v, &vn)) {
                if (rb_ci_eq(k, kn, "user-agent")) {
                    if (!prev_ua) { in_star = 0; in_me = 0; }
                    prev_ua = 1;
                    if (vn == 1 && v[0] == '*') in_star = 1;
                    else if (rb_ci_eq(v, vn, agent)) {
                        if (!seen_me) {
                            /* The named group REPLACES the wildcard one, so
                               drop anything * contributed before it. */
                            size_t z;
                            for (z = 0; z < r->n; z++)
                                free(r->rules[z].path);
                            r->n = 0;
                            r->delay = -1.0;
                        }
                        in_me = 1; seen_me = 1;
                    }
                } else {
                    prev_ua = 0;
                    if (rb_ci_eq(k, kn, "sitemap")) {
                        if (r->n_site == r->cap_site) {
                            size_t nc = r->cap_site ? r->cap_site * 2 : 4;
                            char **ns = (char **)realloc(r->sitemaps,
                                                         nc * sizeof(char *));
                            if (ns) { r->sitemaps = ns; r->cap_site = nc; }
                        }
                        if (r->n_site < r->cap_site && vn) {
                            char *c = (char *)malloc(vn + 1);
                            if (c) { memcpy(c, v, vn); c[vn] = 0;
                                     r->sitemaps[r->n_site++] = c; }
                        }
                    } else if (in_me || (in_star && !seen_me)) {
                        if (rb_ci_eq(k, kn, "disallow")) {
                            if (vn && rb_push_rule(r, v, vn, 0) < 0) return -1;
                            /* an EMPTY Disallow means allow-all: no rule */
                        } else if (rb_ci_eq(k, kn, "allow")) {
                            if (vn && rb_push_rule(r, v, vn, 1) < 0) return -1;
                        } else if (rb_ci_eq(k, kn, "crawl-delay")) {
                            char buf[32];
                            size_t c = vn < sizeof(buf) - 1 ? vn : sizeof(buf) - 1;
                            memcpy(buf, v, c); buf[c] = 0;
                            r->delay = atof(buf);
                        }
                    }
                }
            }
        }
        i = e + 1;
    }
    return 0;
}

static JSValue dyn_rb_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    rb_t *r;
    const char *txt, *agent = NULL;
    size_t tn = 0;
    JSValue obj, av = JS_UNDEFINED;
    (void)new_target;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Robots(text[, { agent }])");
    txt = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!txt)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        av = JS_GetPropertyStr(ctx, argv[1], "agent");
        if (!JS_IsUndefined(av))
            agent = JS_ToCString(ctx, av);
    }
    r = (rb_t *)calloc(1, sizeof(*r));
    if (!r) { JS_FreeCString(ctx, txt); return JS_ThrowOutOfMemory(ctx); }
    r->delay = -1.0;
    if (rb_parse(r, txt, tn, agent ? agent : "*") < 0) {
        rb_free(r);
        if (agent) JS_FreeCString(ctx, agent);
        JS_FreeValue(ctx, av);
        JS_FreeCString(ctx, txt);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (agent) JS_FreeCString(ctx, agent);
    JS_FreeValue(ctx, av);
    JS_FreeCString(ctx, txt);

    obj = JS_NewObjectClass(ctx, (int)dyn_rb_class_id);
    if (JS_IsException(obj)) { rb_free(r); return obj; }
    JS_SetOpaque(obj, r);
    return obj;
}

/* Longest match wins; Allow beats Disallow at equal length (RFC 9309).
   Factored out so Fetcher applies the SAME rule as Robots.allows() rather than
   a second copy that can drift. */
static int rb_allows_path(const rb_t *r, const char *p, size_t pn)
{
    size_t i, best = 0;
    int verdict = 1, found = 0;

    for (i = 0; i < r->n; i++) {
        if (!rb_match(r->rules[i].path, r->rules[i].len, p, pn))
            continue;
        if (!found || r->rules[i].len > best ||
            (r->rules[i].len == best && r->rules[i].allow)) {
            best = r->rules[i].len;
            verdict = r->rules[i].allow;
            found = 1;
        }
    }
    return verdict;
}

static JSValue dyn_rb_allows(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    rb_t *r = JS_GetOpaque2(ctx, this_val, dyn_rb_class_id);
    const char *p;
    size_t pn = 0;
    int verdict;

    if (!r) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "allows(path)");
    p = JS_ToCStringLen(ctx, &pn, argv[0]);
    if (!p) return JS_EXCEPTION;
    verdict = rb_allows_path(r, p, pn);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, verdict);
}

static JSValue dyn_rb_delay(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    rb_t *r = JS_GetOpaque2(ctx, this_val, dyn_rb_class_id);
    (void)argc; (void)argv;
    if (!r) return JS_EXCEPTION;
    return r->delay < 0 ? JS_NULL : JS_NewFloat64(ctx, r->delay);
}

static JSValue dyn_rb_sitemaps(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    rb_t *r = JS_GetOpaque2(ctx, this_val, dyn_rb_class_id);
    JSValue a;
    size_t i;
    (void)argc; (void)argv;
    if (!r) return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a)) return a;
    for (i = 0; i < r->n_site; i++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)i,
                             JS_NewString(ctx, r->sitemaps[i]));
    return a;
}

static JSValue dyn_rb_rules(JSContext *ctx, JSValueConst this_val)
{
    rb_t *r = JS_GetOpaque2(ctx, this_val, dyn_rb_class_id);
    if (!r) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->n);
}

static const JSCFunctionListEntry dyn_rb_proto[] = {
    JS_CFUNC_DEF("allows", 1, dyn_rb_allows),
    JS_CFUNC_DEF("crawlDelay", 0, dyn_rb_delay),
    JS_CFUNC_DEF("sitemaps", 0, dyn_rb_sitemaps),
    JS_CGETSET_DEF("ruleCount", dyn_rb_rules, NULL),
};


/* ---- Extractor: a compiled field spec, checked against a schema ---------
   Takes Selector INSTANCES and calls their public methods -- it does not learn
   CSS, because a second selector engine is a second thing to keep correct.
   Layout drift is the #1 silent scraper failure: a selector that stops matching
   returns nothing, which looks exactly like a page that legitimately has no
   such field. `required` turns that into ok:false with the field NAMED. */

typedef struct {
    char    *name;
    JSValue  sel;        /* a Selector instance (dup'd) */
    char    *attr;       /* NULL => take text */
    unsigned all : 1;
    unsigned required : 1;
    unsigned as_number : 1;
    unsigned as_url : 1;
} ex_field_t;

typedef struct {
    ex_field_t *f;
    size_t      n;
    JSValue     html_text;   /* dyna:html HTMLText, resolved once */
} ex_t;

static JSClassID dyn_ex_class_id;

static void ex_free_ctx(JSContext *ctx, ex_t *e)
{
    size_t i;
    for (i = 0; i < e->n; i++) {
        free(e->f[i].name);
        free(e->f[i].attr);
        JS_FreeValue(ctx, e->f[i].sel);
    }
    free(e->f);
    JS_FreeValue(ctx, e->html_text);
    free(e);
}

static void dyn_ex_finalizer(JSRuntime *rt, JSValue val)
{
    ex_t *e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e) return;
    /* No JSContext here: free with the runtime-level call. */
    for (i = 0; i < e->n; i++) {
        free(e->f[i].name);
        free(e->f[i].attr);
        JS_FreeValueRT(rt, e->f[i].sel);
    }
    free(e->f);
    JS_FreeValueRT(rt, e->html_text);
    free(e);
}

/* The Selector instances and HTMLText are reachable only through us, so the
   cycle collector needs to see them. */
static void dyn_ex_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    ex_t *e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e) return;
    for (i = 0; i < e->n; i++)
        JS_MarkValue(rt, e->f[i].sel, mark);
    JS_MarkValue(rt, e->html_text, mark);
}

static const JSClassDef dyn_ex_class = {
    "Extractor", .finalizer = dyn_ex_finalizer, .gc_mark = dyn_ex_mark,
};

static char *ex_dup_str(JSContext *ctx, JSValueConst v)
{
    const char *s = JS_ToCString(ctx, v);
    char *o;
    if (!s) return NULL;
    o = strdup(s);
    JS_FreeCString(ctx, s);
    return o;
}

static JSValue dyn_ex_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    ex_t *e;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    JSValue obj, ht = JS_UNDEFINED;
    (void)new_target;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Extractor(spec) needs an object");
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0], JS_GPN_STRING_MASK |
                               JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    e = (ex_t *)calloc(1, sizeof(*e));
    if (!e) { js_free(ctx, tab); return JS_ThrowOutOfMemory(ctx); }
    e->html_text = JS_UNDEFINED;
    e->f = (ex_field_t *)calloc(len ? len : 1, sizeof(ex_field_t));
    if (!e->f) { free(e); js_free(ctx, tab); return JS_ThrowOutOfMemory(ctx); }

    for (i = 0; i < len; i++) {
        JSValue fv = JS_GetProperty(ctx, argv[0], tab[i].atom), v;
        ex_field_t *f = &e->f[e->n];
        const char *nm;
        if (!JS_IsObject(fv)) {
            JS_FreeValue(ctx, fv);
            JS_ThrowTypeError(ctx, "Extractor: each field must be an object");
            goto fail;
        }
        nm = JS_AtomToCString(ctx, tab[i].atom);
        f->name = nm ? strdup(nm) : NULL;
        if (nm) JS_FreeCString(ctx, nm);
        f->sel = JS_GetPropertyStr(ctx, fv, "sel");
        if (!JS_IsObject(f->sel)) {
            JS_FreeValue(ctx, fv);
            JS_ThrowTypeError(ctx, "Extractor: field `%s` needs a Selector as `sel`",
                              f->name ? f->name : "?");
            goto fail;
        }
        v = JS_GetPropertyStr(ctx, fv, "attr");
        if (JS_IsString(v)) f->attr = ex_dup_str(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "all");
        f->all = JS_ToBool(ctx, v) ? 1u : 0u;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "required");
        f->required = JS_ToBool(ctx, v) ? 1u : 0u;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "as");
        if (JS_IsString(v)) {
            const char *as = JS_ToCString(ctx, v);
            if (as) {
                if (!strcmp(as, "number")) f->as_number = 1u;
                else if (!strcmp(as, "url")) f->as_url = 1u;
                JS_FreeCString(ctx, as);
            }
        }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, fv);
        e->n++;
    }
    js_free(ctx, tab); tab = NULL;

    /* HTMLText is INJECTED, not looked up: this module owns no parsing, and a
       hidden global would hide the dependency. Required only for text fields. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        ht = JS_GetPropertyStr(ctx, argv[1], "text");
        if (JS_IsFunction(ctx, ht)) e->html_text = ht;
        else JS_FreeValue(ctx, ht);
    }

    obj = JS_NewObjectClass(ctx, (int)dyn_ex_class_id);
    if (JS_IsException(obj)) { ex_free_ctx(ctx, e); return obj; }
    JS_SetOpaque(obj, e);
    return obj;
fail:
    if (tab) js_free(ctx, tab);
    ex_free_ctx(ctx, e);
    return JS_EXCEPTION;
}


/* run(doc[, { base }]) -> { ok, value, missing } */
static JSValue dyn_ex_run(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    ex_t *e = JS_GetOpaque2(ctx, this_val, dyn_ex_class_id);
    JSValue out, val, missing, base = JS_UNDEFINED;
    size_t i;
    int ok = 1;
    uint32_t nmiss = 0;

    if (!e) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "run(doc[, { base }])");
    if (argc > 1 && JS_IsObject(argv[1]))
        base = JS_GetPropertyStr(ctx, argv[1], "base");

    out = JS_NewObject(ctx);
    val = JS_NewObject(ctx);
    missing = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_IsException(val) || JS_IsException(missing))
        goto fail;

    for (i = 0; i < e->n; i++) {
        ex_field_t *f = &e->f[i];
        JSValue m, nodes, res;
        uint32_t cnt = 0, j;

        m = JS_GetPropertyStr(ctx, f->sel, "all");
        if (!JS_IsFunction(ctx, m)) {
            JS_FreeValue(ctx, m);
            JS_ThrowTypeError(ctx, "Extractor: `%s`.sel is not a Selector",
                              f->name ? f->name : "?");
            goto fail;
        }
        nodes = JS_Call(ctx, m, f->sel, 1, (JSValueConst *)&argv[0]);
        JS_FreeValue(ctx, m);
        if (JS_IsException(nodes)) goto fail;
        { JSValue lv = JS_GetPropertyStr(ctx, nodes, "length");
          JS_ToUint32(ctx, &cnt, lv); JS_FreeValue(ctx, lv); }

        res = f->all ? JS_NewArray(ctx) : JS_UNDEFINED;
        for (j = 0; j < cnt; j++) {
            JSValue node = JS_GetPropertyUint32(ctx, nodes, j), piece;
            if (f->attr) {
                JSValue at = JS_GetPropertyStr(ctx, node, "attrs");
                piece = JS_IsObject(at) ? JS_GetPropertyStr(ctx, at, f->attr)
                                        : JS_UNDEFINED;
                JS_FreeValue(ctx, at);
            } else if (JS_IsFunction(ctx, e->html_text)) {
                JSValueConst a1[1] = { node };
                piece = JS_Call(ctx, e->html_text, JS_UNDEFINED, 1, a1);
            } else {
                JS_FreeValue(ctx, node);
                JS_FreeValue(ctx, res); JS_FreeValue(ctx, nodes);
                JS_ThrowTypeError(ctx,
                    "Extractor: field `%s` wants text; pass { text: HTMLText } "
                    "to the constructor", f->name ? f->name : "?");
                goto fail;
            }
            JS_FreeValue(ctx, node);
            if (JS_IsException(piece)) { JS_FreeValue(ctx, res);
                                         JS_FreeValue(ctx, nodes); goto fail; }
            /* as: coerces AND validates -- a non-numeric string is a failure,
               not NaN, and a url must resolve to an absolute one. */
            if (f->as_number && !JS_IsUndefined(piece)) {
                double d; JSValue t = JS_DupValue(ctx, piece);
                if (JS_ToFloat64(ctx, &d, t) < 0 || d != d) {
                    JSValue ex = JS_GetException(ctx);   /* clear, not propagate */
                    JS_FreeValue(ctx, ex);
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;                /* refused, not NaN */
                } else { JS_FreeValue(ctx, piece); piece = JS_NewFloat64(ctx, d); }
            }
            if (f->all) JS_SetPropertyUint32(ctx, res, j, piece);
            else { res = piece; break; }
        }
        JS_FreeValue(ctx, nodes);
        if (!f->all && cnt == 0) res = JS_UNDEFINED;

        if ((cnt == 0 || (!f->all && JS_IsUndefined(res))) && f->required) {
            ok = 0;
            JS_SetPropertyUint32(ctx, missing, nmiss++,
                                 JS_NewString(ctx, f->name ? f->name : "?"));
        }
        JS_SetPropertyStr(ctx, val, f->name ? f->name : "?", res);
    }
    JS_FreeValue(ctx, base);
    JS_SetPropertyStr(ctx, out, "ok", JS_NewBool(ctx, ok));
    JS_SetPropertyStr(ctx, out, "value", val);
    JS_SetPropertyStr(ctx, out, "missing", missing);
    return out;
fail:
    JS_FreeValue(ctx, base); JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, val); JS_FreeValue(ctx, missing);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_ex_proto[] = {
    JS_CFUNC_DEF("run", 1, dyn_ex_run),
};

/* ---- Fetcher: polite HTTP retrieval (design 28) -----------------------
 *
 * A RESOURCE (owns per-host state and a client), so it has close(). POLICY
 * lives here -- robots, the per-host delay floor, retries and backoff, the
 * redirect chain, the body cap, the stats. The TRANSPORT is delegated by
 * invoking the client's `request`: reimplementing it would be a second thing
 * to keep correct, and a crawler's interesting behaviour is entirely policy.
 *
 * `agent` and `client` are both REQUIRED. No default agent, because a shared
 * one is indistinguishable from anonymous and denies the operator the one
 * thing they need -- who to contact. No default client, because dyna:scrape
 * must not link dyna:net to construct one, and injecting it is what lets a
 * test drive the whole policy against a mock.
 */

#define FE_MAX_HOSTS 256

typedef struct {
    char  *host;
    double next_ok_ms;   /* earliest next fetch, from the delay floor */
    rb_t  *robots;       /* set only when a 200 was parsed */
    int    robots_tried;
    int    robots_unreachable;  /* 5xx or network error: MUST disallow (9309) */
} fe_host_t;

typedef struct {
    /* NO JSValue HERE. dispose() receives only the native pointer, so a
       resource cannot free a JSValue it owns -- which is why nothing else in
       this tree does. The client lives as a non-enumerable property on the JS
       object instead: the GC traces it, and no gc_mark or free is needed. */
    char      *agent;
    int        robots_on, retries, max_redirects;
    int        allow_private_hosts;   /* SSRF gate: off = refuse private hosts */
    double     min_delay_ms, max_body;
    fe_host_t *hosts;
    size_t     n_hosts, cap_hosts;
    double     fetched, skipped_robots, retried, throttled_ms, bytes;
} fe_t;

static JSClassID dyn_fe_class_id;

static void fe_dispose(void *native)
{
    fe_t *f = (fe_t *)native;
    size_t i;
    if (!f) return;
    for (i = 0; i < f->n_hosts; i++) {
        free(f->hosts[i].host);
        if (f->hosts[i].robots) rb_free(f->hosts[i].robots);
    }
    free(f->hosts);
    free(f->agent);
    free(f);
}

/* No gc_mark: the struct holds no JSValue. See the note in fe_t. */
static const JSClassDef dyn_fe_class = {
    "Fetcher", .finalizer = dyn_res_finalizer,
};

/* scheme://authority/path. Returns 1 for https, 0 for http, -1 if neither. */
static int fe_split(const char *url, char *host, size_t cap, const char **path)
{
    const char *p = url, *h;
    size_t n;
    int https;
    if (!strncmp(p, "http://", 7))       { p += 7; https = 0; }
    else if (!strncmp(p, "https://", 8)) { p += 8; https = 1; }
    else return -1;
    h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' &&
           (unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7F)
        p++;
    /* A C0/DEL byte inside the authority must REFUSE, not end it: it would
       otherwise ride into the Host header and split it (CR/LF injection). */
    if (*p && ((unsigned char)*p < 0x20 || *p == 0x7F))
        return -1;
    n = (size_t)(p - h);
    if (n == 0 || n + 1 > cap) return -1;
    memcpy(host, h, n);
    host[n] = 0;
    *path = *p ? p : "/";
    return https;
}

/* ---- SSRF gate: refuse hosts that are not on the public internet ---------
   The transport (the injected client) owns DNS and connect, so THIS layer can
   only classify what the URL names. IP literals are range-checked against the
   RFC 6890 / IANA special-purpose assignments; hostname shapes that can only
   mean "internal" (localhost, any single-label name, mDNS-style suffixes) are
   refused too. A name that RESOLVES to a private address cannot be stopped
   here -- resolution happens inside the client -- so the client's connect
   path is the complementary gate; this one is the layer that sees every
   redirect hop and every robots pre-fetch. 64:ff9b::/96 (NAT64) is
   Global=True per RFC 6890 and is deliberately NOT blocked. */

static int fe_ip4_octet(const char *s, const char **end)
{
    unsigned v = 0;
    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 255)
            return -1;
        s++;
    }
    *end = s;
    return (int)v;
}

/* Dotted quad -> four bytes. Returns 1 on a literal, 0 otherwise. */
static int fe_ip4_parse(const char *s, unsigned char b[4])
{
    const char *p = s;
    int i;
    for (i = 0; i < 4; i++) {
        int v = fe_ip4_octet(p, &p);
        if (v < 0)
            return 0;
        b[i] = (unsigned char)v;
        if (i < 3) {
            if (*p != '.')
                return 0;
            p++;
        }
    }
    return *p == '\0';
}

/* Is `h` (a bare literal, brackets already stripped) private/loopback/
   link-local/reserved? 1 = refuse, 0 = plausibly public. */
static int fe_host_literal_private(const char *h)
{
    unsigned char b[4];
    if (fe_ip4_parse(h, b)) {
        /* RFC 6890 / IANA special-purpose IPv4 assignments */
        if (b[0] == 0) return 1;                             /* 0/8: this net */
        if (b[0] == 10) return 1;                            /* 10/8 */
        if (b[0] == 100 && (b[1] & 0xc0) == 64) return 1;    /* 100.64/10 */
        if (b[0] == 127) return 1;                           /* 127/8 */
        if (b[0] == 169 && b[1] == 254) return 1;            /* 169.254/16 */
        if (b[0] == 172 && (b[1] & 0xf0) == 16) return 1;    /* 172.16/12 */
        if (b[0] == 192 && b[1] == 168) return 1;            /* 192.168/16 */
        if (b[0] == 192 && b[1] == 0 && b[2] == 0) return 1; /* 192.0.0.0/24 */
        if (b[0] >= 224) return 1;                           /* 224/4, 240/4 */
        return 0;
    }
    if (h[0] == ':') {
        if (!strcmp(h, "::1")) return 1;                     /* loopback */
        if (!strcmp(h, "::")) return 1;                      /* unspecified */
        if (!strncmp(h, "::ffff:", 7))                       /* v4-mapped */
            return fe_host_literal_private(h + 7);
        return 0;              /* other ::-forms: first group is 0, so the
                                  range checks below cannot match */
    }
    {   /* the first group decides fc00::/7 and fe80::/10 */
        unsigned v = 0, k = 0;
        while (h[k] && h[k] != ':' && k < 4) {
            int c = h[k], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;                     /* not an IPv6 literal */
            v = v * 16 + (unsigned)d;
            k++;
        }
        if (h[k] != ':' && h[k] != '\0')
            return 0;                          /* not an IPv6 literal */
        if ((v & 0xfe00) == 0xfc00) return 1;  /* fc00::/7 (unique local) */
        if ((v & 0xffc0) == 0xfe80) return 1;  /* fe80::/10 (link-local) */
        return 0;                              /* incl. 64:ff9b::/96: global */
    }
}

/* Reduce an authority (as fe_split copies it, userinfo and port included) to
   the bare host, lowercased, for the SSRF gate. */
static void fe_bare_host(const char *auth, char *out, size_t cap)
{
    const char *h = auth;
    const char *q;
    char *p;
    size_t n;
    for (q = auth; *q; q++)
        if (*q == '@')
            h = q + 1;                      /* last '@' ends the userinfo */
    n = strlen(h);
    if (n && h[0] == '[') {                 /* [v6]:port -> brackets+port off */
        const char *cl = strchr(h, ']');
        if (cl) { h++; n = (size_t)(cl - h); }
    } else {
        const char *c = strchr(h, ':');
        if (c)
            n = (size_t)(c - h);            /* drop the port */
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, h, n);
    out[n] = 0;
    for (p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
}

/* 1 = this host must not be fetched unless allowPrivateHosts; 0 = fine. */
static int fe_host_is_private(const char *host)
{
    static const char * const internal_sfx[] = { ".local", ".internal", ".lan" };
    char bare[300];
    size_t n, i;
    fe_bare_host(host, bare, sizeof bare);
    if (bare[0] == '\0')
        return 1;                          /* no host: nothing public about it */
    if (fe_host_literal_private(bare))
        return 1;
    if (!strcmp(bare, "localhost") || !strcmp(bare, "localhost.localdomain"))
        return 1;
    if (!strchr(bare, '.'))
        return 1;                          /* single-label: admin, metadata, ... */
    n = strlen(bare);
    for (i = 0; i < countof(internal_sfx); i++) {
        size_t k = strlen(internal_sfx[i]);
        if (n > k && !strcmp(bare + n - k, internal_sfx[i]))
            return 1;
    }
    return 0;
}

static fe_host_t *fe_host(fe_t *f, const char *host)
{
    size_t i;
    for (i = 0; i < f->n_hosts; i++)
        if (!strcmp(f->hosts[i].host, host))
            return &f->hosts[i];
    /* BOUNDED: a crawl that wanders must not grow this without limit. Past the
       cap the first slot is recycled -- that costs a robots re-fetch, never
       memory. */
    if (f->n_hosts >= FE_MAX_HOSTS) {
        fe_host_t *v = &f->hosts[0];
        free(v->host);
        if (v->robots) rb_free(v->robots);
        memset(v, 0, sizeof(*v));
        v->host = strdup(host);
        return v->host ? v : NULL;
    }
    if (f->n_hosts == f->cap_hosts) {
        size_t nc = f->cap_hosts ? f->cap_hosts * 2 : 8;
        fe_host_t *nh = (fe_host_t *)realloc(f->hosts, nc * sizeof(*nh));
        if (!nh) return NULL;
        f->hosts = nh; f->cap_hosts = nc;
    }
    memset(&f->hosts[f->n_hosts], 0, sizeof(fe_host_t));
    f->hosts[f->n_hosts].host = strdup(host);
    if (!f->hosts[f->n_hosts].host) return NULL;
    return &f->hosts[f->n_hosts++];
}

/* poll(NULL, 0, ms): a portable sleep needing no feature-test macro. */
static void fe_sleep_ms(double ms)
{
    if (ms <= 0) return;
    if (ms > 60000) ms = 60000;         /* never park a crawl for a minute+ */
    poll(NULL, 0, (int)ms);
}

static JSValue fe_raw(JSContext *ctx, fe_t *f, JSValueConst client,
                      const char *url)
{
    JSValue a[4], ret;
    JSAtom m;
    a[0] = JS_NewString(ctx, "GET");
    a[1] = JS_NewString(ctx, url);
    a[2] = JS_UNDEFINED;
    a[3] = JS_NewObject(ctx);
    /* On EVERY request including robots.txt: a crawler identifying itself only
       for pages is not identified. */
    JS_DefinePropertyValueStr(ctx, a[3], "User-Agent",
                              JS_NewString(ctx, f->agent), JS_PROP_C_W_E);
    m = JS_NewAtom(ctx, "request");
    ret = JS_Invoke(ctx, client, m, 4, (JSValueConst *)a);
    JS_FreeAtom(ctx, m);
    JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]); JS_FreeValue(ctx, a[3]);
    return ret;
}

static int fe_num_prop(JSContext *ctx, JSValueConst o, const char *k, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t r = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToInt32(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return (int)r;
}

static int fe_status(JSContext *ctx, JSValueConst res)
{
    JSValue v = JS_GetPropertyStr(ctx, res, "status");
    int32_t st = 0;
    JS_ToInt32(ctx, &st, v);
    JS_FreeValue(ctx, v);
    return (int)st;
}

/* A header by name, case-insensitively. Returns a C string or NULL. */
static const char *fe_header(JSContext *ctx, JSValueConst res, const char *want,
                             JSValue *hold)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    const char *out = NULL;
    *hold = JS_UNDEFINED;
    if (!JS_IsObject(h)) { JS_FreeValue(ctx, h); return NULL; }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) == 0) {
        for (i = 0; i < n; i++) {
            const char *k = JS_AtomToCString(ctx, tab[i].atom);
            int hit = k && !strcasecmp(k, want);
            if (k) JS_FreeCString(ctx, k);
            if (hit) {
                *hold = JS_GetProperty(ctx, h, tab[i].atom);
                out = JS_ToCString(ctx, *hold);
                break;
            }
        }
        for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, h);
    return out;
}

/* robots.txt, once per host. A 5xx or a network error means the file is
   undefined and RFC 9309 2.3.1.4 REQUIRES complete disallow; only a 4xx
   "Unavailable" (2.3.1.3) MAY be treated as allow. `tried` stops a 404
   being re-fetched for every URL on the host. */
static void fe_load_robots(JSContext *ctx, fe_t *f, JSValueConst client,
                           fe_host_t *h, int https)
{
    char url[600];
    JSValue res;
    int st;
    h->robots_tried = 1;
    snprintf(url, sizeof url, "%s%s/robots.txt",
             https ? "https://" : "http://", h->host);
    res = fe_raw(ctx, f, client, url);
    if (JS_IsException(res)) {
        /* Network error: the file is UNDEFINED. RFC 9309 2.3.1.4 says that
           means "MUST assume complete disallow" -- the old default-allow
           here was a spec violation and an SSRF widening. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        h->robots_unreachable = 1;
        return;
    }
    st = fe_status(ctx, res);
    if (st == 200) {
        JSValue b = JS_GetPropertyStr(ctx, res, "body");
        size_t bn = 0;
        const char *txt = JS_ToCStringLen(ctx, &bn, b);
        if (txt) {
            rb_t *r = (rb_t *)calloc(1, sizeof(rb_t));
            if (r) {
                r->delay = -1;
                if (rb_parse(r, txt, bn, f->agent) < 0) {
                    /* Unparseable (here: OOM mid-parse): the rules are
                       unusable, and RFC 9309 2.3.1.5 only ever lets us use
                       what actually parsed. Treat it as unreachable rather
                       than silently allowing everything (audit 8.1). */
                    rb_free(r);
                    h->robots_unreachable = 1;
                } else {
                    h->robots = r;
                }
            }
            JS_FreeCString(ctx, txt);
        }
        JS_FreeValue(ctx, b);
    } else if (st >= 500 && st < 600) {
        /* 5xx: server error -- the file is undefined (RFC 9309 2.3.1.4). */
        h->robots_unreachable = 1;
    } else if (st < 400 || st >= 600) {
        /* Everything that is neither a definitive 2xx fetch nor a definitive
           4xx "Unavailable" (2.3.1.3) -- an unresolved redirect (3xx), a
           status of 0, a non-standard code -- is unreachable: an undefined
           file MUST mean complete disallow (2.3.1.4), so only the two
           definitive outcomes may allow. Fail closed (audit 14.2). */
        h->robots_unreachable = 1;
    }
    /* 4xx "Unavailable" (2.3.1.3): MAY allow -- robots stays NULL. */
    JS_FreeValue(ctx, res);
}

/* get(url) -> { status, headers, body, url, fromCache }
 *
 * The whole policy, in order: delay floor, robots gate, request, retry with
 * backoff, redirect chase, body cap. Each step updates stats, because a
 * crawler whose politeness cannot be observed cannot be trusted.
 */
static JSValue dyn_fe_get(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    fe_t *f;
    const char *url0 = NULL;
    char cur[1024], host[300];
    const char *path;
    int https, hop;
    JSValue res = JS_UNDEFINED, ret = JS_EXCEPTION, client;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "get(url)");
    /* Coerce BEFORE resolving: coercion runs user JS that can close() us. */
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0)
        return JS_EXCEPTION;
    f = (fe_t *)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    if (!f) { JS_FreeCString(ctx, url0); return JS_EXCEPTION; }
    if (strlen(url0) + 1 > sizeof cur) {
        JS_FreeCString(ctx, url0);
        return JS_ThrowRangeError(ctx, "Fetcher: url is too long");
    }
    snprintf(cur, sizeof cur, "%s", url0);
    JS_FreeCString(ctx, url0);
    client = JS_GetPropertyStr(ctx, this_val, "_client");

    for (hop = 0; ; hop++) {
        fe_host_t *h;
        double wait, floor_ms;
        int attempt, st = 0;

        https = fe_split(cur, host, sizeof host, &path);
        if (https < 0)
            { JS_ThrowTypeError(ctx,
                "Fetcher: only http:// and https:// urls (got %.60s)", cur);
              goto out; }
        if (!f->allow_private_hosts && fe_host_is_private(host)) {
            JS_ThrowTypeError(ctx,
                "Fetcher: %s://%.60s is a private/loopback/link-local host; "
                "pass allowPrivateHosts: true to fetch it",
                https ? "https" : "http", host);
            goto out;
        }
        h = fe_host(f, host);
        if (!h) { JS_ThrowOutOfMemory(ctx); goto out; }

        /* ---- robots, before anything is requested from the host ---- */
        if (f->robots_on) {
            if (!h->robots_tried)
                fe_load_robots(ctx, f, client, h, https);
            if (h->robots_unreachable ||
                (h->robots && !rb_allows_path(h->robots, path, strlen(path)))) {
                f->skipped_robots++;
                ret = JS_NewObject(ctx);
                JS_DefinePropertyValueStr(ctx, ret, "status", JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "skippedByRobots", JS_TRUE, JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "body", JS_NewString(ctx, ""), JS_PROP_C_W_E);
                goto out;
            }
        }

        /* ---- the delay floor. Crawl-delay raises it, NEVER lowers it. ---- */
        floor_ms = f->min_delay_ms;
        if (h->robots && h->robots->delay > 0) {
            double cd = h->robots->delay * 1000.0;
            if (cd > floor_ms) floor_ms = cd;
        }
        wait = h->next_ok_ms - (double)dyn_timer_now_ms();
        if (wait > 0) { f->throttled_ms += wait; fe_sleep_ms(wait); }

        /* ---- request, with retries ---- */
        for (attempt = 0; ; attempt++) {
            JSValue hold;
            const char *ra;
            double back;

            res = fe_raw(ctx, f, client, cur);
            h->next_ok_ms = (double)dyn_timer_now_ms() + floor_ms;
            if (JS_IsException(res)) {
                if (attempt >= f->retries) goto out;
                JS_FreeValue(ctx, JS_GetException(ctx));
                st = 0;
            } else {
                st = fe_status(ctx, res);
                if (!(st == 429 || (st >= 500 && st < 600)) || attempt >= f->retries)
                    break;
            }
            /* Exponential backoff. Retry-After, when the server names one,
               OVERRIDES it -- the server knows better than our curve. */
            back = f->min_delay_ms * (double)(1 << (attempt < 10 ? attempt : 10));
            if (!JS_IsException(res)) {
                ra = fe_header(ctx, res, "retry-after", &hold);
                if (ra) {
                    double secs = strtod(ra, NULL);
                    if (secs > 0) back = secs * 1000.0;
                    JS_FreeCString(ctx, ra);
                }
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                res = JS_UNDEFINED;
            }
            f->retried++;
            fe_sleep_ms(back);
        }

        /* ---- redirects ---- */
        if (st >= 300 && st < 400) {
            JSValue hold;
            const char *loc = fe_header(ctx, res, "location", &hold);
            if (loc && hop < f->max_redirects) {
                snprintf(cur, sizeof cur, "%s", loc);
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                res = JS_UNDEFINED;
                continue;                       /* next hop */
            }
            if (loc) JS_FreeCString(ctx, loc);
            JS_FreeValue(ctx, hold);
            if (loc && hop >= f->max_redirects) {
                JS_FreeValue(ctx, res);
                JS_ThrowRangeError(ctx,
                    "Fetcher: more than %d redirects", f->max_redirects);
                goto out;
            }
        }

        /* ---- body cap. A server does not get to choose how much we hold. */
        {
            JSValue b = JS_GetPropertyStr(ctx, res, "body");
            size_t bn = 0;
            const char *bs = JS_ToCStringLen(ctx, &bn, b);
            if (bs) JS_FreeCString(ctx, bs);
            JS_FreeValue(ctx, b);
            if ((double)bn > f->max_body) {
                JS_FreeValue(ctx, res);
                JS_ThrowRangeError(ctx,
                    "Fetcher: body of %zu bytes exceeds maxBodyBytes", bn);
                goto out;
            }
            f->bytes += (double)bn;
        }
        f->fetched++;
        JS_DefinePropertyValueStr(ctx, res, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_FALSE, JS_PROP_C_W_E);
        ret = res;
        goto out;
    }
out:
    JS_FreeValue(ctx, client);
    return ret;
}

static JSValue dyn_fe_stats(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    fe_t *f = (fe_t *)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    JSValue o;
    (void)argc; (void)argv;
    if (!f) return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "fetched", JS_NewFloat64(ctx, f->fetched), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "skippedByRobots", JS_NewFloat64(ctx, f->skipped_robots), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "retried", JS_NewFloat64(ctx, f->retried), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "throttledMs", JS_NewFloat64(ctx, f->throttled_ms), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bytes", JS_NewFloat64(ctx, f->bytes), JS_PROP_C_W_E);
    return o;
}

static JSValue dyn_fe_ctor(JSContext *ctx, JSValueConst nt, int argc,
                           JSValueConst *argv)
{
    fe_t *f;
    JSValue av, cv;
    const char *agent = NULL;
    (void)nt;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Fetcher({ agent, client }): both are REQUIRED");
    av = JS_GetPropertyStr(ctx, argv[0], "agent");
    if (JS_IsString(av)) agent = JS_ToCString(ctx, av);
    JS_FreeValue(ctx, av);
    if (!agent || !*agent) {
        if (agent) JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `agent` is required and must be a non-empty string, e.g. "
            "\"mybot/1.0 (+https://example.test/bot)\". There is no default: a "
            "shared one is indistinguishable from anonymous and tells an "
            "operator nothing about who to contact.");
    }
    cv = JS_GetPropertyStr(ctx, argv[0], "client");
    if (!JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `client` is required -- pass an HTTPClient from dyna:net. "
            "It is injected rather than constructed here so dyna:scrape does "
            "not link dyna:net, and so a test can drive the policy against a "
            "mock.");
    }
    f = (fe_t *)calloc(1, sizeof(*f));
    if (!f) { JS_FreeValue(ctx, cv); JS_FreeCString(ctx, agent); return JS_ThrowOutOfMemory(ctx); }
    f->agent = strdup(agent);
    JS_FreeCString(ctx, agent);
    if (!f->agent) { JS_FreeValue(ctx, cv); free(f); return JS_ThrowOutOfMemory(ctx); }

    f->robots_on     = fe_num_prop(ctx, argv[0], "robots", 1) ? 1 : 0;
    f->min_delay_ms  = fe_num_prop(ctx, argv[0], "minDelayMs", 1000);
    f->retries       = fe_num_prop(ctx, argv[0], "retries", 3);
    f->max_redirects = fe_num_prop(ctx, argv[0], "maxRedirects", 5);
    f->max_body      = (double)fe_num_prop(ctx, argv[0], "maxBodyBytes", 8 << 20);
    f->allow_private_hosts =
        fe_num_prop(ctx, argv[0], "allowPrivateHosts", 0) ? 1 : 0;
    if (f->retries < 0) f->retries = 0;
    if (f->max_redirects < 0) f->max_redirects = 0;
    if (f->min_delay_ms < 0) f->min_delay_ms = 0;
    {
        JSValue obj = dyn_res_wrap(ctx, dyn_fe_class_id, f, fe_dispose);
        if (JS_IsException(obj)) { JS_FreeValue(ctx, cv); return obj; }
        /* Non-enumerable and non-configurable: the GC traces it, so no
           gc_mark and no free are needed, and it is not part of the API. */
        JS_DefinePropertyValueStr(ctx, obj, "_client", cv, 0);
        return obj;
    }
}

static const JSCFunctionListEntry dyn_fe_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_fe_get),
    JS_CFUNC_DEF("stats", 0, dyn_fe_stats),
};


/* ---- Crawl: bounded traversal over Fetcher + Extractor (design 28) -----
 *
 * Composes the three pieces rather than adding a fourth. In particular it does
 * NOT scan HTML for links: the Extractor already does that through the real
 * parser, so Crawl reads them from a named field of its output. A second,
 * permissive href scanner here would be a duplicate predicate, and duplicate
 * predicates drift.
 *
 * `start()` returns a lazy ITERATOR -- one fetch per next() -- so a bound of
 * 500 pages does not mean 500 fetches before the caller sees the first. It is
 * a sync iterator, which `for await...of` accepts.
 */

typedef struct {
    char  *url;
    int    depth;
} cr_item_t;

typedef struct {
    cr_item_t *q;                /* frontier, FIFO */
    size_t     qn, qcap, qhead;
    char     **seen;             /* visited urls */
    size_t     sn, scap;
    int        max_pages, max_depth, same_host;
    char      *link_field;
    char       seed_host[300];
    int        emitted;
    int        started;
} cr_t;

static JSClassID dyn_cr_class_id;

static void cr_dispose(void *native)
{
    cr_t *c = (cr_t *)native;
    size_t i;
    if (!c) return;
    for (i = 0; i < c->qn; i++) free(c->q[i].url);
    free(c->q);
    for (i = 0; i < c->sn; i++) free(c->seen[i]);
    free(c->seen);
    free(c->link_field);
    free(c);
}

/* No gc_mark: the fetcher and extractor live as traced properties on the JS
   object, for the reason recorded on Fetcher -- dispose() gets only the native
   pointer and so cannot free a JSValue. */
static const JSClassDef dyn_cr_class = {
    "Crawl", .finalizer = dyn_res_finalizer,
};

static int cr_seen(cr_t *c, const char *u)
{
    size_t i;
    for (i = 0; i < c->sn; i++)
        if (!strcmp(c->seen[i], u)) return 1;
    return 0;
}

static int cr_mark_seen(cr_t *c, const char *u)
{
    if (c->sn == c->scap) {
        size_t nc = c->scap ? c->scap * 2 : 32;
        char **ns = (char **)realloc(c->seen, nc * sizeof(*ns));
        if (!ns) return -1;
        c->seen = ns; c->scap = nc;
    }
    c->seen[c->sn] = strdup(u);
    if (!c->seen[c->sn]) return -1;
    c->sn++;
    return 0;
}

static int cr_push(cr_t *c, const char *u, int depth)
{
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16;
        cr_item_t *nq = (cr_item_t *)realloc(c->q, nc * sizeof(*nq));
        if (!nq) return -1;
        c->q = nq; c->qcap = nc;
    }
    c->q[c->qn].url = strdup(u);
    if (!c->q[c->qn].url) return -1;
    c->q[c->qn].depth = depth;
    c->qn++;
    return 0;
}

static JSValue dyn_cr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                           JSValueConst *argv)
{
    cr_t *c;
    JSValue opt, lf, obj;
    const char *s;
    (void)nt;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Crawl(fetcher[, { maxPages, maxDepth, sameHost, linkField }])");
    c = (cr_t *)calloc(1, sizeof(*c));
    if (!c) return JS_ThrowOutOfMemory(ctx);
    opt = (argc > 1 && JS_IsObject(argv[1])) ? JS_DupValue(ctx, argv[1])
                                             : JS_NewObject(ctx);
    c->max_pages = fe_num_prop(ctx, opt, "maxPages", 100);
    c->max_depth = fe_num_prop(ctx, opt, "maxDepth", 2);
    c->same_host = fe_num_prop(ctx, opt, "sameHost", 1) ? 1 : 0;
    lf = JS_GetPropertyStr(ctx, opt, "linkField");
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    c->link_field = strdup(s ? s : "links");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    if (!c->link_field) { free(c); return JS_ThrowOutOfMemory(ctx); }
    if (c->max_pages < 0) c->max_pages = 0;
    if (c->max_depth < 0) c->max_depth = 0;

    obj = dyn_res_wrap(ctx, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj)) return obj;
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    return obj;
}

/* start(seed[, extractor]) -> this, primed. Returning the Crawl itself keeps
   one object rather than a second iterator class to keep correct. */
static JSValue dyn_cr_start(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    cr_t *c = (cr_t *)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    const char *seed;
    const char *path;
    if (!c) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "start(seed[, extractor])");
    seed = JS_ToCString(ctx, argv[0]);
    if (!seed) return JS_EXCEPTION;
    if (fe_split(seed, c->seed_host, sizeof c->seed_host, &path) < 0) {
        JS_FreeCString(ctx, seed);
        return JS_ThrowTypeError(ctx, "Crawl: seed must be an http(s) url");
    }
    if (cr_push(c, seed, 0) < 0 || cr_mark_seen(c, seed) < 0) {
        JS_FreeCString(ctx, seed);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_FreeCString(ctx, seed);
    c->started = 1;
    if (argc > 1 && JS_IsObject(argv[1]))
        JS_DefinePropertyValueStr(ctx, this_val, "_extractor",
                                  JS_DupValue(ctx, argv[1]), 0);
    /* Extractor.run() takes a PARSED document, so a parser must be supplied.
       Injected like the fetcher's client: scrape does not link dyna:html. */
    if (argc > 2 && JS_IsFunction(ctx, argv[2]))
        JS_DefinePropertyValueStr(ctx, this_val, "_parse",
                                  JS_DupValue(ctx, argv[2]), 0);
    return JS_DupValue(ctx, this_val);
}

/* One page per call: {value, done}. */
static JSValue dyn_cr_next(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    cr_t *c = (cr_t *)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    JSValue fetcher, extractor, res, page, out;
    char *url = NULL;
    int depth;
    (void)argc; (void)argv;

    if (!c) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    if (!c->started || c->qhead >= c->qn || c->emitted >= c->max_pages) {
        JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED, JS_PROP_C_W_E);
        return out;
    }
    url = c->q[c->qhead].url;
    depth = c->q[c->qhead].depth;
    c->qhead++;

    fetcher = JS_GetPropertyStr(ctx, this_val, "_fetcher");
    {
        JSValue a[1];
        JSAtom m = JS_NewAtom(ctx, "get");
        a[0] = JS_NewString(ctx, url);
        res = JS_Invoke(ctx, fetcher, m, 1, (JSValueConst *)a);
        JS_FreeAtom(ctx, m);
        JS_FreeValue(ctx, a[0]);
    }
    JS_FreeValue(ctx, fetcher);
    if (JS_IsException(res)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }

    page = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, page, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, page, "depth", JS_NewInt32(ctx, depth), JS_PROP_C_W_E);
    {
        JSValue st = JS_GetPropertyStr(ctx, res, "status");
        JS_DefinePropertyValueStr(ctx, page, "status", st, JS_PROP_C_W_E);
    }

    /* Extract, then take the links from the named field. The extractor uses
       the real HTML parser; Crawl does not scan markup itself. */
    extractor = JS_GetPropertyStr(ctx, this_val, "_extractor");
    if (JS_IsObject(extractor)) {
        JSValue raw = JS_GetPropertyStr(ctx, res, "body");
        JSValue parse = JS_GetPropertyStr(ctx, this_val, "_parse");
        JSValue body, a[2], ev;
        JSAtom m;
        /* Parse if a parser was given; otherwise hand the body through and let
           the extractor refuse it, which is louder than silently finding
           nothing. */
        if (JS_IsFunction(ctx, parse)) {
            JSValueConst pa[1]; pa[0] = raw;
            body = JS_Call(ctx, parse, JS_UNDEFINED, 1, pa);
            JS_FreeValue(ctx, raw);
            if (JS_IsException(body)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                body = JS_UNDEFINED;
            }
        } else {
            body = raw;
        }
        JS_FreeValue(ctx, parse);
        m = JS_NewAtom(ctx, "run");
        a[0] = body;
        a[1] = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, a[1], "base", JS_NewString(ctx, url), JS_PROP_C_W_E);
        ev = JS_Invoke(ctx, extractor, m, 2, (JSValueConst *)a);
        JS_FreeAtom(ctx, m);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, body);
        if (!JS_IsException(ev) && JS_IsObject(ev)) {
            JSValue val = JS_GetPropertyStr(ctx, ev, "value");
            JS_DefinePropertyValueStr(ctx, page, "value", JS_DupValue(ctx, val), JS_PROP_C_W_E);
            if (depth < c->max_depth && JS_IsObject(val)) {
                JSValue links = JS_GetPropertyStr(ctx, val, c->link_field);
                if (JS_IsArray(ctx, links)) {
                    JSValue lv = JS_GetPropertyStr(ctx, links, "length");
                    uint32_t i, n = 0;
                    JS_ToUint32(ctx, &n, lv);
                    JS_FreeValue(ctx, lv);
                    for (i = 0; i < n; i++) {
                        JSValue e = JS_GetPropertyUint32(ctx, links, i);
                        const char *ls = JS_ToCString(ctx, e);
                        if (ls) {
                            char lh[300];
                            const char *lp;
                            int ok = fe_split(ls, lh, sizeof lh, &lp) >= 0;
                            if (ok && c->same_host && strcmp(lh, c->seed_host))
                                ok = 0;                  /* off-host, skipped */
                            if (ok && !cr_seen(c, ls)) {
                                cr_mark_seen(c, ls);
                                cr_push(c, ls, depth + 1);
                            }
                            JS_FreeCString(ctx, ls);
                        }
                        JS_FreeValue(ctx, e);
                    }
                }
                JS_FreeValue(ctx, links);
            }
            JS_FreeValue(ctx, val);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, ev);
    }
    JS_FreeValue(ctx, extractor);
    JS_FreeValue(ctx, res);

    c->emitted++;
    JS_DefinePropertyValueStr(ctx, out, "value", page, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE, JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_cr_self(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{ (void)argc; (void)argv; return JS_DupValue(ctx, this_val); }

static const JSCFunctionListEntry dyn_cr_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_cr_start),
    JS_CFUNC_DEF("next", 0, dyn_cr_next),
    JS_CFUNC_DEF("pages", 0, dyn_cr_self),
    /* `for await (const p of c.start(...))` needs this. A SYNC iterator is
       enough: for-await falls back to Symbol.iterator and awaits each value. */
    JS_ALIAS_DEF("[Symbol.iterator]", "pages"),
};

static int dyn_scrape_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_rb_class_id, &dyn_rb_class,
                           dyn_rb_proto, countof(dyn_rb_proto),
                           dyn_rb_ctor, "Robots") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_cr_class_id, &dyn_cr_class,
                           dyn_cr_proto, countof(dyn_cr_proto),
                           dyn_cr_ctor, "Crawl") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_fe_class_id, &dyn_fe_class,
                           dyn_fe_proto, countof(dyn_fe_proto),
                           dyn_fe_ctor, "Fetcher") < 0)
        return -1;
    return dyn_register_plain_class(ctx, m, &dyn_ex_class_id, &dyn_ex_class,
                                    dyn_ex_proto, countof(dyn_ex_proto),
                                    dyn_ex_ctor, "Extractor");
}

int js_nat_init_scrape(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:scrape", dyn_scrape_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Robots");
    JS_AddModuleExport(ctx, m, "Extractor");
    JS_AddModuleExport(ctx, m, "Fetcher");
    JS_AddModuleExport(ctx, m, "Crawl");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SCRAPE */
