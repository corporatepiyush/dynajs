/*
 * dyna:scrape -- the policy layer over fetch + parse (design 28).
 *
 * Owns NO parsing: fetching is dyna:net, HTML is dyna:html, URLs are dyna:url.
 * What lives here is the policy nobody gets right -- robots.txt, per-host
 * pacing, retry bounds, and a schema the extraction is checked against.
 *
 * NOT an evasion toolkit: no fingerprint spoofing, no CAPTCHA handling, no
 * ban-circumvention proxying. See §0 of the design; that is a decision.
 */
#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>   /* time(NULL): EPOCH wall clock, for HTTP-date deltas */
#include <poll.h>   /* poll(NULL,0,ms): the portable sleep */
#include "core/dyn-timer.h"   /* dyn_timer_now_ms: MONOTONIC, delay floors */
#include "core/dyn-prng.h"    /* dyn_splitmix64, dyn_os_entropy: backoff jitter */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* robots.txt is attacker-influenced input: every bound is checked WHILE
   parsing, not after. A robots bomb is a real shape. */
#define RB_MAX_BYTES  (512 * 1024)
#define RB_MAX_RULES  1000
#define RB_MAX_PATH   2048
#define RB_MAX_GROUPS 64
#define RB_MAX_UAS_PER_GROUP 8
#define RB_MAX_AGENT  128

typedef struct {
    char    *path;      /* the pattern, percent-decoded */
    size_t   len;
    size_t   grp;       /* index of the group this rule belongs to */
    unsigned allow : 1; /* Allow (1) vs Disallow (0) */
} rb_rule_t;

typedef struct {
    rb_rule_t *rules;
    size_t     n, cap;
    double     delay;   /* Crawl-delay seconds of the winning group, or -1 */
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

static int rb_reserved(unsigned char c);   /* defined below with the matcher */

/* RFC 9309 §2.2.2.1: a rule's octets are matched AS WRITTEN -- a percent-
   encoded octet is LITERAL (%2A is a literal '*', %24 a literal '$', %2F
   never a separator; the wildcard is a RAW '*'). Only what would disagree
   with the normalized uri path is rewritten: raw non-ASCII, raw reserved
   octets other than the separator '/' and the wildcard pair '*'/'$', and a
   '%' that is not part of a valid escape. */
static size_t rb_norm_rule(char *s, size_t n)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n) {
            int h = -1, l = -1;
            unsigned char a = (unsigned char)s[i+1], b = (unsigned char)s[i+2];
            if (a >= '0' && a <= '9') h = a - '0';
            else if (a >= 'a' && a <= 'f') h = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') h = a - 'A' + 10;
            if (b >= '0' && b <= '9') l = b - '0';
            else if (b >= 'a' && b <= 'f') l = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') l = b - 'A' + 10;
            if (h >= 0 && l >= 0) {
                s[o++] = '%'; s[o++] = (char)a; s[o++] = (char)b;
                i += 3;
                continue;
            }
        }
        if (c != '/' && c != '*' && c != '$' &&
            (rb_reserved(c) || c >= 0x80)) {
            s[o++] = '%'; s[o++] = HEX[c >> 4]; s[o++] = HEX[c & 0xF];
        } else {
            s[o++] = (char)c;
        }
        i++;
    }
    return o;
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

/* Case-insensitive compare of an[an] against bn[bn]. */
static int rb_ci_ncmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];
        if (ca != cb) return 0;
    }
    return 1;
}

typedef struct {
    size_t first, n;              /* rule range in the arena */
    double delay;
    int    is_star;
    char  *uas[RB_MAX_UAS_PER_GROUP];
    size_t n_uas;
} rb_grp_t;

/* The product token of a UA line: up to but not including the first '/'.
   RFC 9309 2.2.1.2: "examplebot" matches "examplebot/1.0". */
static size_t rb_token_len(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i] != '/'; i++)
        ;
    return i;
}

static void rb_groups_free(rb_grp_t *g, size_t n)
{
    size_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < g[i].n_uas; j++)
            free(g[i].uas[j]);
    free(g);
}

/* Parse into the RULE ARENA plus GROUPS, then pick the winning group:
   exact full-agent match > product-token match (longest UA wins ties) >
   '*'. RFC 9309 keeps specific groups above the wildcard; Google's parser
   additionally matches on the token so "ExampleBot/2.3" hits
   "User-agent: examplebot". Contiguity of each group's rules holds because
   a directive after a run of User-agent lines starts a NEW group. */
static int rb_parse(rb_t *r, const char *s, size_t n, const char *agent)
{
    rb_rule_t *arena = NULL;
    size_t n_arena = 0, cap_arena = 0;
    rb_grp_t *groups;
    size_t n_groups = 0, cap_groups = 16;
    size_t token_a, aglen, best_g = (size_t)-1;
    int best_score = -1;
    size_t best_uale = 0, gi, z;
    long gcur = -1;          /* active group: rules append to it */
    int prev_ua = 1;         /* a UA-run may absorb another line */
    size_t i = 0;

    if (n > RB_MAX_BYTES)
        n = RB_MAX_BYTES;                /* cap while reading, not after */
    /* a UTF-8 BOM must not orphan the first User-agent line: some CDNs
       serve BOM'd robots.txt, and with it the leading group is never seen */
    if (n >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s += 3; n -= 3;
    }
    aglen = strlen(agent);
    token_a = rb_token_len(agent, aglen);
    groups = (rb_grp_t *)calloc(cap_groups, sizeof(*groups));
    if (!groups)
        return -1;

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
                    char uabuf[RB_MAX_AGENT];
                    size_t ul = vn < sizeof(uabuf) - 1 ?
                                vn : sizeof(uabuf) - 1;
                    rb_grp_t *g;
                    /* After directives, a User-agent opens a fresh group. */
                    if (prev_ua && gcur >= 0)
                        g = &groups[gcur];
                    else {
                        if (n_groups >= RB_MAX_GROUPS) {
                            gcur = -1;      /* cap: further groups ignored */
                            prev_ua = 1;
                            goto next_line;
                        }
                        /* The cap is RB_MAX_GROUPS but the initial allocation
                           is 16: GROW, or the 17th group's memset lands past
                           the calloc (heap overflow, found by ASan on the
                           120-group record-storm test). */
                        if (n_groups == cap_groups) {
                            size_t nc = cap_groups * 2;
                            rb_grp_t *ng = (rb_grp_t *)realloc(
                                groups, nc * sizeof(*ng));
                            if (!ng) goto oom;
                            groups = ng; cap_groups = nc;
                        }
                        gcur = (long)n_groups++;
                        memset(&groups[gcur], 0, sizeof(rb_grp_t));
                        groups[gcur].delay = -1.0;
                        g = &groups[gcur];
                    }
                    memcpy(uabuf, v, ul);
                    uabuf[ul] = 0;
                    /* record EVERY ua of the run; any may match */
                    if (g->n_uas < RB_MAX_UAS_PER_GROUP) {
                        char *c = (char *)malloc(ul + 1);
                        if (!c) goto oom;
                        memcpy(c, uabuf, ul + 1);
                        g->uas[g->n_uas++] = c;
                    }
                    prev_ua = 1;
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
                    } else if (gcur >= 0) {
                        rb_grp_t *g = &groups[gcur];
                        if (rb_ci_eq(k, kn, "crawl-delay")) {
                            /* RFC 9309: the value is a number. A malformed
                               value makes the line meaningless, so the
                               directive is IGNORED (the group default of -1
                               stands) -- atof("2x") used to parse as 2. */
                            char buf[32];
                            size_t c = vn < sizeof(buf) - 1 ? vn : sizeof(buf) - 1;
                            char *end;
                            double d;
                            memcpy(buf, v, c); buf[c] = 0;
                            d = strtod(buf, &end);
                            if (end != buf && *end == '\0')
                                g->delay = d;
                        } else if (rb_ci_eq(k, kn, "disallow") ||
                                   rb_ci_eq(k, kn, "allow")) {
                            int allow = rb_ci_eq(k, kn, "allow");
                            if (!vn || n_arena >= RB_MAX_RULES)
                                goto next_line;  /* empty Disallow = allow-all;
                                                    rule bomb capped silently */
                            if (n_arena == cap_arena) {
                                size_t nc = cap_arena ? cap_arena * 2 : 32;
                                rb_rule_t *na = (rb_rule_t *)
                                    realloc(arena, nc * sizeof(*na));
                                if (!na) goto oom;
                                arena = na; cap_arena = nc;
                            }
                            {
                                char *copy;
                                size_t pn = vn > RB_MAX_PATH ? RB_MAX_PATH : vn;
                                /* rb_norm_rule can GROW (encoding), so the
                                   copy is 3x the pattern, not pattern+1 */
                                copy = (char *)malloc(3 * pn + 1);
                                if (!copy) goto oom;
                                memcpy(copy, v, pn);
                                copy[pn] = 0;
                                pn = rb_norm_rule(copy, pn);
                                copy[pn] = 0;
                                arena[n_arena].path = copy;
                                arena[n_arena].len = pn;
                                arena[n_arena].grp = (size_t)gcur;
                                arena[n_arena].allow = allow ? 1u : 0u;
                                if (g->n == 0)
                                    g->first = n_arena;
                                g->n++;
                                n_arena++;
                            }
                        }
                    }
                }
            }
        }
        next_line:
        i = e + 1;
    }

    /* ---- pick the winner ---- */
    for (gi = 0; gi < n_groups; gi++) {
        rb_grp_t *g = &groups[gi];
        int score = 0;
        size_t uale_max = 0, u;
        for (u = 0; u < g->n_uas; u++) {
            size_t ul = strlen(g->uas[u]);
            size_t utok = rb_token_len(g->uas[u], ul);
            if (rb_ci_ncmp(g->uas[u], ul, agent, aglen))
                score = score > 2 ? score : 2;
            else if (utok && utok == token_a &&
                     rb_ci_ncmp(g->uas[u], utok, agent, token_a))
                score = score > 1 ? score : 1;
            if (ul > uale_max) uale_max = ul;
        }
        if (score > best_score ||
            (score == best_score && uale_max > best_uale)) {
            best_score = score; best_g = gi; best_uale = uale_max;
        }
    }
    /* No named match anywhere -> the wildcard group, if one exists. */
    if (best_score <= 0) {
        for (gi = 0; gi < n_groups; gi++) {
            size_t u;
            for (u = 0; u < groups[gi].n_uas; u++)
                if (!strcmp(groups[gi].uas[u], "*"))
                    { best_g = gi; break; }
            if (best_g != (size_t)-1)
                break;
        }
    }

    if (best_g != (size_t)-1) {
        rb_grp_t *g = &groups[best_g];
        r->delay = g->delay;
        if (g->n) {
            r->rules = (rb_rule_t *)calloc(g->n, sizeof(*r->rules));
            if (!r->rules) goto oom;
            memcpy(r->rules, arena + g->first, g->n * sizeof(*r->rules));
            r->n = g->n; r->cap = g->n;   /* ownership MOVED, not dup'd */
        }
    }
    for (z = 0; z < n_arena; z++) {
        int keep = best_g != (size_t)-1 &&
                   z >= groups[best_g].first &&
                   z < groups[best_g].first + groups[best_g].n;
        if (!keep) free(arena[z].path);
    }
    free(arena);
    rb_groups_free(groups, n_groups);
    return 0;
oom:
    for (z = 0; z < n_arena; z++) free(arena[z].path);
    free(arena);
    rb_groups_free(groups, n_groups);
    return -1;
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

/* RFC 3986 reserved octets: a RAW reserved octet in the uri path is
   percent-encoded before comparison, so a rule "%2A" meets a literal '*'. */
static int rb_reserved(unsigned char c)
{
    switch (c) {
    case ':': case '/': case '?': case '#': case '[': case ']':
    case '@': case '!': case '$': case '&': case '\'': case '(':
    case ')': case '*': case '+': case ',': case ';': case '=':
        return 1;
    default:
        return 0;
    }
}

/* The URI side of RFC 9309 §2.2.2.1: raw reserved and non-ASCII octets are
   percent-encoded (the '/' SEPARATOR stays raw, as on the rule side), an
   existing %XX of a reserved/non-ASCII octet stays, and an encoded
   UNRESERVED octet decodes -- so both spellings of a path compare as the
   same octet run, and a raw '*' in a url meets a rule's literal %2A. */
static size_t rb_norm_path(const char *s, size_t n, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    while (i < n && o + 3 < cap) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n) {
            int h = -1, l = -1;
            unsigned char a = (unsigned char)s[i+1], b = (unsigned char)s[i+2];
            if (a >= '0' && a <= '9') h = a - '0';
            else if (a >= 'a' && a <= 'f') h = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') h = a - 'A' + 10;
            if (b >= '0' && b <= '9') l = b - '0';
            else if (b >= 'a' && b <= 'f') l = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') l = b - 'A' + 10;
            if (h >= 0 && l >= 0) {
                int v = (h << 4) | l;
                if (v == '/' || v == '*' || v == '$' || v >= 0x80 ||
                    rb_reserved((unsigned char)v)) {
                    out[o++] = '%'; out[o++] = HEX[h]; out[o++] = HEX[l];
                } else {
                    out[o++] = (char)v;
                }
                i += 3;
                continue;
            }
        }
        if (c != '/' && (rb_reserved(c) || c >= 0x80 || c < 0x21 || c == 0x7F)) {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
        i++;
    }
    out[o] = '\0';
    return o;
}

/* Longest match wins; Allow beats Disallow at equal length (RFC 9309).
   Factored out so Fetcher applies the SAME rule as Robots.allows() rather than
   a second copy that can drift. The path is normalised to the comparison
   form first (OOM errs toward allow, matching "no rules apply"). */
static int rb_allows_path(const rb_t *r, const char *p, size_t pn)
{
    size_t i, best = 0;
    int verdict = 1, found = 0;
    char *norm;
    size_t nn;

    if (pn > 2048)
        pn = 2048;
    norm = (char *)malloc(3 * pn + 1);
    if (!norm)
        return 1;
    nn = rb_norm_path(p, pn, norm, 3 * pn + 1);
    for (i = 0; i < r->n; i++) {
        if (!rb_match(r->rules[i].path, r->rules[i].len, norm, nn))
            continue;
        if (!found || r->rules[i].len > best ||
            (r->rules[i].len == best && r->rules[i].allow)) {
            best = r->rules[i].len;
            verdict = r->rules[i].allow;
            found = 1;
        }
    }
    free(norm);
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

static long fe_resolve(const char *base, const char *ref, char *out,
                       size_t cap);   /* defined in the Fetcher section below */

typedef struct {
    char    *name;
    JSValue  sel;        /* a Selector instance (dup'd) */
    JSValue dflt;        /* used when the field finds nothing */
    char    *attr;       /* NULL => take text */
    unsigned all : 1;
    unsigned required : 1;
    unsigned trim : 1;
    unsigned source : 1;   /* raw child source (script/style/ld+json) */
    unsigned as_number : 1;
    unsigned as_url : 1;
    unsigned as_json : 1;
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
        JS_FreeValue(ctx, e->f[i].dflt);
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
        JS_FreeValueRT(rt, e->f[i].dflt);
    }
    free(e->f);
    JS_FreeValueRT(rt, e->html_text);
    free(e);
}

/* The Selector instances and HTMLText are reachable only through us, so the
   cycle collector needs to see them; `default` values are ours too. */
static void dyn_ex_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    ex_t *e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e) return;
    for (i = 0; i < e->n; i++) {
        JS_MarkValue(rt, e->f[i].sel, mark);
        JS_MarkValue(rt, e->f[i].dflt, mark);
    }
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

/* Span of s[n] with surrounding whitespace removed; *ofs gets the start. */
static size_t ex_trimmed(const char *s, size_t n, size_t *ofs)
{
    size_t a = 0, e = n;
    while (a < n && (s[a] == ' ' || s[a] == '\t' ||
                     s[a] == '\r' || s[a] == '\n')) a++;
    while (e > a && (s[e-1] == ' ' || s[e-1] == '\t' ||
                     s[e-1] == '\r' || s[e-1] == '\n')) e--;
    *ofs = a;
    return e - a;
}

/* Concatenated STRING children of a node -- the raw source of
   <script>/<style>/ld+json, which HTMLText deliberately refuses to serve as
   text. Recursion is bounded because HTMLParse bounds nesting; the RESULT
   is capped so a page cannot hand us an unbounded string.
   Chunks keep (ptr, len): script SOURCE may contain NUL bytes, so joining
   by strlen would silently truncate.
   Returns 0 on success; -1 after throwing. */
#define EX_SOURCE_CAP (1u << 20)

typedef struct { char *p; size_t n; } ex_chunk_t;

static int ex_concat_kids(JSContext *ctx, JSValueConst node, int depth,
                          ex_chunk_t **chunks, size_t *n_ch, size_t *cap_ch,
                          size_t *total)
{
    JSValue kids;
    uint32_t n = 0, i;

    if (depth > 64)
        return 0;                        /* HTMLParse never exceeds this */
    if (!JS_IsObject(node))
        return 0;
    kids = JS_GetPropertyStr(ctx, node, "children");
    if (!JS_IsArray(ctx, kids)) { JS_FreeValue(ctx, kids); return 0; }
    {
        JSValue lv = JS_GetPropertyStr(ctx, kids, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
    }
    for (i = 0; i < n; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, kids, i);
        if (JS_IsString(c)) {
            const char *t; size_t tn;
            t = JS_ToCStringLen(ctx, &tn, c);
            if (!t) { JS_FreeValue(ctx, c); JS_FreeValue(ctx, kids);
                      JS_ThrowOutOfMemory(ctx); return -1; }
            if (*n_ch == *cap_ch) {
                size_t nc = *cap_ch ? *cap_ch * 2 : 8;
                ex_chunk_t *na = (ex_chunk_t *)
                    realloc(*chunks, nc * sizeof(ex_chunk_t));
                if (!na) { JS_FreeCString(ctx, t); JS_FreeValue(ctx, c);
                           JS_FreeValue(ctx, kids);
                           JS_ThrowOutOfMemory(ctx); return -1; }
                *chunks = na; *cap_ch = nc;
            }
            {
                ex_chunk_t *ch = &(*chunks)[(*n_ch)++];
                ch->p = (char *)malloc(tn + 1);
                if (!ch->p) {
                    (*n_ch)--;
                    JS_FreeCString(ctx, t); JS_FreeValue(ctx, c);
                    JS_FreeValue(ctx, kids);
                    JS_ThrowOutOfMemory(ctx); return -1;
                }
                memcpy(ch->p, t, tn);
                ch->p[tn] = 0;           /* embedded NULs stay inside len */
                ch->n = tn;
            }
            *total += tn;
            JS_FreeCString(ctx, t);
            if (*total > EX_SOURCE_CAP)
                { /* over cap: stop collecting; caller serves what we have */
                  JS_FreeValue(ctx, c); JS_FreeValue(ctx, kids); return 0; }
        } else if (JS_IsObject(c)) {
            int rc = ex_concat_kids(ctx, c, depth + 1,
                                    chunks, n_ch, cap_ch, total);
            if (rc < 0) { JS_FreeValue(ctx, c); JS_FreeValue(ctx, kids);
                          return -1; }
        }
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, kids);
    return 0;
}

static JSValue ex_node_source(JSContext *ctx, JSValueConst node)
{
    ex_chunk_t *chunks = NULL;
    size_t n_ch = 0, cap_ch = 0, total = 0, i, o = 0;
    char *joined;
    JSValue out;

    if (ex_concat_kids(ctx, node, 0, &chunks, &n_ch, &cap_ch, &total) < 0) {
        size_t z;
        for (z = 0; z < n_ch; z++) free(chunks[z].p);
        free(chunks);
        return JS_EXCEPTION;
    }
    joined = (char *)malloc(total + 1);
    if (!joined) {
        for (i = 0; i < n_ch; i++) free(chunks[i].p);
        free(chunks);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n_ch; i++) {
        memcpy(joined + o, chunks[i].p, chunks[i].n);
        o += chunks[i].n;
        free(chunks[i].p);
    }
    free(chunks);
    joined[o] = 0;
    out = JS_NewStringLen(ctx, joined, o);
    free(joined);
    return out;
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
        memset(f, 0, sizeof(*f));
        /* OWN the slot now: a mid-field validation failure below must free
           name/sel/default via ex_free_ctx, which walks to e->n */
        e->n++;
        f->dflt = JS_UNDEFINED;
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
        v = JS_GetPropertyStr(ctx, fv, "trim");
        f->trim = JS_ToBool(ctx, v) ? 1u : 0u;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "source");
        f->source = JS_ToBool(ctx, v) ? 1u : 0u;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "default");
        if (!JS_IsUndefined(v)) f->dflt = v; else JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "as");
        if (JS_IsString(v)) {
            const char *as = JS_ToCString(ctx, v);
            if (as) {
                if (!strcmp(as, "number")) f->as_number = 1u;
                else if (!strcmp(as, "url")) f->as_url = 1u;
                else if (!strcmp(as, "json")) f->as_json = 1u;
                else {
                    /* ANY unknown coercion refuses at construction: a
                       silently-ignored typo renders the field unchecked,
                       which is the exact failure `required` exists to catch. */
                    char bad[40];
                    snprintf(bad, sizeof bad, "%.39s", as);
                    JS_FreeCString(ctx, as);
                    JS_FreeValue(ctx, fv);
                    JS_ThrowTypeError(ctx,
                        "Extractor: field `%s` as:\"%s\" is not supported "
                        "(supported: number, url, json)",
                        f->name ? f->name : "?", bad);
                    goto fail;
                }
                JS_FreeCString(ctx, as);
            }
        }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, fv);
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
    const char *base_s = NULL;
    size_t i;
    int ok = 1;
    uint32_t nmiss = 0;

    if (!e) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "run(doc[, { base }])");
    if (argc > 1 && JS_IsObject(argv[1])) {
        base = JS_GetPropertyStr(ctx, argv[1], "base");
        if (JS_IsString(base)) {
            base_s = JS_ToCString(ctx, base);
            if (!base_s) { JS_FreeValue(ctx, base); return JS_EXCEPTION; }
        }
    }

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
            } else if (f->source) {
                piece = ex_node_source(ctx, node);
            } else if (JS_IsFunction(ctx, e->html_text)) {
                JSValueConst a1[1] = { node };
                piece = JS_Call(ctx, e->html_text, JS_UNDEFINED, 1, a1);
            } else {
                JS_FreeValue(ctx, node);
                JS_FreeValue(ctx, res); JS_FreeValue(ctx, nodes);
                JS_ThrowTypeError(ctx,
                    "Extractor: field `%s` wants text; pass { text: HTMLText } "
                    "to the constructor, or take attr/source instead",
                    f->name ? f->name : "?");
                goto fail;
            }
            JS_FreeValue(ctx, node);
            if (JS_IsException(piece)) { JS_FreeValue(ctx, res);
                                         JS_FreeValue(ctx, nodes); goto fail; }
            /* trim first: whitespace around a number or inside JSON is the
               single most common extraction artefact */
            if (f->trim && !JS_IsUndefined(piece)) {
                const char *ts; size_t tn2, ofs;
                if (JS_IsString(piece)) {
                    ts = JS_ToCStringLen(ctx, &tn2, piece);
                    if (ts) {
                        size_t tn3 = ex_trimmed(ts, tn2, &ofs);
                        JSValue nt = JS_NewStringLen(ctx, ts + ofs, tn3);
                        JS_FreeCString(ctx, ts);
                        JS_FreeValue(ctx, piece);
                        piece = nt;
                    }
                }
            }
            /* as: coerces AND validates -- a non-numeric string is a failure,
               not NaN; invalid JSON is refused, not thrown through; a url
               resolves against opts.base and relative ones without a base are
               refused. */
            if (f->as_number && !JS_IsUndefined(piece)) {
                double d;
                /* no Dup here: ToFloat64 borrows. The old form dup'd into a
                   temp it never freed -- one leaked string ref per numeric
                   field per run, invisible to refcount-less leak checks. */
                if (JS_ToFloat64(ctx, &d, piece) < 0 || d != d) {
                    JSValue ex = JS_GetException(ctx);   /* clear, not propagate */
                    JS_FreeValue(ctx, ex);
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;                /* refused, not NaN */
                } else {
                    JS_FreeValue(ctx, piece);
                    piece = JS_NewFloat64(ctx, d);
                }
            }
            if (f->as_json && !JS_IsUndefined(piece)) {
                if (JS_IsString(piece)) {
                    const char *js; size_t jl;
                    js = JS_ToCStringLen(ctx, &jl, piece);
                    if (js) {
                        JSValue jv = JS_ParseJSON(ctx, js, jl, "<extractor>");
                        JS_FreeCString(ctx, js);
                        if (JS_IsException(jv)) {
                            JS_FreeValue(ctx, JS_GetException(ctx));
                            JS_FreeValue(ctx, piece);
                            piece = JS_UNDEFINED;        /* invalid is refusal */
                        } else {
                            JS_FreeValue(ctx, piece);
                            piece = jv;
                        }
                    }
                } else {
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;
                }
            }
            if (f->as_url && !JS_IsUndefined(piece) && JS_IsString(piece)) {
                const char *us; size_t un;
                us = JS_ToCStringLen(ctx, &un, piece);
                if (!us || (!strncasecmp(us, "http://", 7) ||
                            !strncasecmp(us, "https://", 8))) {
                    /* absolute urls pass through */
                    if (us) JS_FreeCString(ctx, us);
                } else {
                    char rbuf[1024];
                    long rl = base_s ? fe_resolve(base_s, us, rbuf,
                                                  sizeof rbuf) : -1;
                    JS_FreeCString(ctx, us);
                    if (rl > 0) {
                        JSValue nv = JS_NewStringLen(ctx, rbuf, (size_t)rl);
                        JS_FreeValue(ctx, piece);
                        piece = nv;
                    } else {
                        JS_FreeValue(ctx, piece);
                        /* no base, or resolution impossible: refusal */
                        piece = JS_UNDEFINED;
                    }
                }
            }
            if (f->all) JS_SetPropertyUint32(ctx, res, j, piece);
            else { res = piece; break; }
        }
        JS_FreeValue(ctx, nodes);
        if (!f->all && cnt == 0) res = JS_UNDEFINED;

        /* default satisfies an empty field BEFORE required checks: for a
           scalar that is the undefined result; for `all` it is the EMPTY
           array, replaced wholesale (a caller asking for a list that did
           not exist gets its declared fallback, not `[]`). */
        if (!f->all && JS_IsUndefined(res) && !JS_IsUndefined(f->dflt))
            res = JS_DupValue(ctx, f->dflt);
        else if (f->all && cnt == 0 && !JS_IsUndefined(f->dflt)) {
            JS_FreeValue(ctx, res);
            res = JS_DupValue(ctx, f->dflt);
        }

        if (JS_IsUndefined(f->dflt) &&
            (cnt == 0 || (!f->all && JS_IsUndefined(res))) && f->required) {
            ok = 0;
            JS_SetPropertyUint32(ctx, missing, nmiss++,
                                 JS_NewString(ctx, f->name ? f->name : "?"));
        }
        JS_SetPropertyStr(ctx, val, f->name ? f->name : "?", res);
    }
    JS_FreeCString(ctx, base_s);
    JS_FreeValue(ctx, base);
    JS_SetPropertyStr(ctx, out, "ok", JS_NewBool(ctx, ok));
    JS_SetPropertyStr(ctx, out, "value", val);
    JS_SetPropertyStr(ctx, out, "missing", missing);
    return out;
fail:
    JS_FreeCString(ctx, base_s);
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
    int    robots_ok;           /* a good copy was parsed at least once */
    double robots_next_ms;      /* monotonic: refresh the file after this */
} fe_host_t;

/* Conditional-request metadata store: one entry per url that answered with
   a validator, FIFO-evicted past the entry or byte cap. The body is kept so
   a 304 can be answered without a second transfer of the content itself. */
#define FE_CACHE_MAX_ENTRIES 32
#define FE_CACHE_MAX_BYTES   (4u << 20)   /* total cached bodies */
#define FE_CACHE_BODY_CAP    (1u << 20)   /* per-body admission */

typedef struct {
    char  *url, *etag, *lm, *ct;   /* ct: content-type of the stored body */
    char  *body;          /* last 200 body under this validator */
    size_t blen;
} fe_cache_ent_t;

typedef struct {
    /* NO JSValue HERE. dispose() receives only the native pointer, so a
       resource cannot free a JSValue it owns -- which is why nothing else in
       this tree does. The client lives as a non-enumerable property on the JS
       object instead: the GC traces it, and no gc_mark or free is needed.
       `_headers` (caller-supplied extra request headers) lives there too. */
    char      *agent;
    int        robots_on, retries, max_redirects;
    double     robots_ttl_ms;      /* robots.txt refresh interval */
    int        allow_private_hosts;   /* SSRF gate: off = refuse private hosts */
    int        revalidate;            /* conditional GET via stored validators */
    int        allow_insecure_downgrade;  /* follow a redirect https -> http */
    double     min_delay_ms, max_body;
    uint64_t   rng;          /* backoff jitter; seeded once in the ctor */
    fe_host_t *hosts;
    size_t     n_hosts, cap_hosts;
    double     fetched, skipped_robots, retried, throttled_ms, bytes;
    double     revalidated, saved_bytes;
    fe_cache_ent_t cache[FE_CACHE_MAX_ENTRIES];
    size_t     n_cache;
} fe_t;

static JSClassID dyn_fe_class_id;

static void fe_cache_clear(fe_cache_ent_t *e)
{
    free(e->url); free(e->etag); free(e->lm); free(e->ct); free(e->body);
    memset(e, 0, sizeof(*e));
}

static fe_cache_ent_t *fe_cache_find(fe_t *f, const char *url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url))
            return &f->cache[i];
    return NULL;
}

static char *fe_strdup_n(const char *s, size_t n)
{
    char *o;
    if (n > FE_CACHE_BODY_CAP)
        return NULL;
    o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

/* Insert or refresh the entry for `url`. Returns the entry or NULL (full,
   too big, OOM -- all mean "fetch unconditionally next time", never an
   error: a cache refusal must not fail a fetch that would have worked). */
static void fe_cache_evict_url(fe_t *f, const char *url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url)) {
            fe_cache_clear(&f->cache[i]);
            memmove(&f->cache[i], &f->cache[i+1],
                    (f->n_cache - i - 1) * sizeof(*f->cache));
            f->n_cache--;
            return;
        }
}

static fe_cache_ent_t *fe_cache_put(fe_t *f, const char *url,
                                    const char *etag, const char *lm,
                                    const char *ct,
                                    const char *body, size_t blen)
{
    fe_cache_ent_t *e;
    double tot;
    size_t i;

    /* Refusals here mean "fetch unconditionally next time", never an error:
       a cache admission policy must not fail a fetch that would work. */
    if (!etag && !lm)
        return NULL;
    if (!body || blen > FE_CACHE_BODY_CAP)
        return NULL;
    /* ct (content-type) is OPTIONAL: validators are not. Drop OUR previous entry first, so no pointer is held across the
       memmove shifts of the capacity evictions below (and so keys stay
       unique whatever happens afterwards). */
    fe_cache_evict_url(f, url);

    tot = (double)(blen + strlen(url));
    for (i = 0; i < f->n_cache; i++)
        tot += (double)f->cache[i].blen;
    while ((f->n_cache >= FE_CACHE_MAX_ENTRIES ||
            tot > (double)FE_CACHE_MAX_BYTES) && f->n_cache) {
        tot -= (double)f->cache[0].blen;
        fe_cache_clear(&f->cache[0]);
        memmove(&f->cache[0], &f->cache[1],
                (f->n_cache - 1) * sizeof(*f->cache));
        f->n_cache--;
    }
    if (tot > (double)FE_CACHE_MAX_BYTES)
        return NULL;

    e = &f->cache[f->n_cache++];
    memset(e, 0, sizeof(*e));
    e->url = strdup(url);
    if (!e->url) { f->n_cache--; memset(e, 0, sizeof(*e)); return NULL; }
    e->etag = etag ? strdup(etag) : NULL;
    e->lm   = lm   ? strdup(lm)   : NULL;
    e->ct   = ct   ? strdup(ct)   : NULL;
    if ((etag && !e->etag) || (lm && !e->lm) || (ct && !e->ct)) {
        fe_cache_evict_url(f, url);       /* strdup failed mid-field */
        return NULL;
    }
    e->body = fe_strdup_n(body, blen);
    if (!e->body) { fe_cache_evict_url(f, url); return NULL; }
    e->blen = blen;
    return e;
}

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
    for (i = 0; i < f->n_cache; i++)
        fe_cache_clear(&f->cache[i]);
    free(f->agent);
    free(f);
}

/* No gc_mark: the struct holds no JSValue. See the note in fe_t. */
static const JSClassDef dyn_fe_class = {
    "Fetcher", .finalizer = dyn_res_finalizer,
};

/* 1 when url speaks https (case-exact prefix; fe_split built it). */
static int fe_scheme_is_https(const char *url)
{
    return !strncmp(url, "https://", 8);
}

/* scheme://authority/path. Returns 1 for https, 0 for http, -1 if neither. */
static int fe_split(const char *url, char *host, size_t cap, const char **path)
{    const char *p = url, *h;
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

/* ---- RFC 3986 reference resolution --------------------------------------
   A 30x Location is often RELATIVE (`/a`, `b`, `../c?x=1`) or
   protocol-relative (`//other.test/p`); copying it verbatim breaks the hop.
   Dot segments are removed too, so `/a/../c` cannot smuggle a path the
   robots check never saw. The authority always comes from BASE: dots act
   only inside a path buffer and can never change the host of the result.
   Any control char or space in the reference refuses (-1): CR/LF must not
   ride into a new request line.

   Returns the resolved length, or -1 when it does not fit or either url is
   not http(s). */
static void fe_rm_dots(char *s)
{
    /* seg[k] = write offset of segment k's first byte; '..' pops back there,
       overwriting the previous segment. Popping above root clamps (RFC). */
    size_t seg[128], ns = 0, i = 0, o = 0, rooted = s[0] == '/';
    if (rooted) s[o++] = s[i++];
    while (s[i]) {
        if (s[i] == '/') { i++; continue; }              /* dup slashes */
        if (s[i] == '.' && (s[i+1] == '/' || !s[i+1]))
            { i += s[i+1] ? 2 : 1; continue; }           /* /./ */
        if (s[i] == '.' && s[i+1] == '.' &&
            (s[i+2] == '/' || !s[i+2])) {                 /* /../ */
            i += s[i+2] ? 3 : 2;
            o = ns ? seg[--ns] : (rooted ? 1u : 0u);
            continue;
        }
        if (ns < countof(seg)) seg[ns++] = o;
        while (s[i] && s[i] != '/') s[o++] = s[i++];
        if (s[i]) s[o++] = s[i++];
    }
    s[o] = 0;
}

static long fe_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    const char *p, *auth, *bp0, *bpend, *rf;
    size_t slen, authlen, rn, wr;
    int kind;                 /* 0 abs-url 1 net-path 2 abs-path 3 query-only
                                 4 relative-path */

    for (p = ref; *p; p++)                            /* CRLF/space guard */
        if ((unsigned char)*p <= 0x20 || *p == 0x7f)
            return -1;

    if (!strncmp(base, "http://", 7))       { slen = 4; }
    else if (!strncmp(base, "https://", 8)) { slen = 5; }
    else return -1;

    auth = base + slen + 3;                           /* past "://" */
    p = strchr(auth, '/');
    if (!p) p = strchr(auth, '?');
    authlen = p ? (size_t)(p - auth) : strlen(auth);
    bp0 = auth + authlen;                             /* base path start */
    bpend = bp0 + strcspn(bp0, "#");                  /* base fragment gone */
    rf = strchr(ref, '#');
    rn = rf ? (size_t)(rf - ref) : strlen(ref);

    kind = 4;
    if (!strncmp(ref, "//", 2)) {
        kind = 1;
    } else if (rn > 0 && ref[0] == '/') {
        kind = 2;
    } else if (rn == 0 || ref[0] == '?') {
        /* empty or ?query-only reference: same path, query replaced
           (RFC 3986 5.3 -- an empty reference drops the old query too) */
        kind = 3;
    } else {                                          /* scheme-led? */
        p = ref;
        if ((*p | 32) >= 'a' && (*p | 32) <= 'z') {
            const char *q = p + 1;
            while ((size_t)(q - ref) < rn &&
                   (((*q | 32) >= 'a' && (*q | 32) <= 'z') ||
                    (*q >= '0' && *q <= '9') ||
                    *q == '+' || *q == '-' || *q == '.'))
                q++;
            if ((size_t)(q - ref) < rn && *q == ':')
                kind = 0;
        }
    }

#define FE_PUT(src_, n_) do { \
        if ((n_) + 1 > cap - wr) return -1; \
        memcpy(out + wr, (src_), (n_)); wr += (n_); \
        out[wr] = 0; } while (0)   /* terminate EARLY: rm_dots scans until NUL */

    /* A net-path reference carries its OWN authority: "scheme" + ref as-is
       ("//other.test/p"). Handle it before any base authority is copied. */
    if (kind == 1) {
        size_t k;
        memcpy(out, base, slen + 1);               /* "http:" */
        wr = slen + 1;
        out[wr] = 0;
        FE_PUT(ref, rn);                           /* //authority[/path] */
        k = slen + 3;                              /* past ':' and '//' */
        while (out[k] && out[k] != '/') k++;
        if (out[k]) {                              /* path present */
            fe_rm_dots(out + k);
            wr = k + strlen(out + k);
        } else {
            wr = strlen(out);
        }
        return (long)wr;
    }

    if (kind == 0) {                                  /* verbatim, no frag */
        if (rn >= cap) return -1;
        memcpy(out, ref, rn);
        out[rn] = 0;
        return (long)rn;
    }

    /* header: scheme://authority (never rewritten again) */
    if (slen + 3 + authlen + 2 > cap) return -1;
    memcpy(out, base, slen + 3);
    wr = slen + 3;
    memcpy(out + wr, auth, authlen);
    wr += authlen;

    switch (kind) {
    case 2:                                           /* /abs/path */
        FE_PUT("/", 1);
        FE_PUT(ref, rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        break;
    case 3: {                                         /* ?query: base path */
        const char *bq = memchr(bp0, '?', (size_t)(bpend - bp0));
        size_t pl = bq ? (size_t)(bq - bp0) : (size_t)(bpend - bp0);
        if (!pl) FE_PUT("/", 1);
        else FE_PUT(bp0, pl);
        FE_PUT(ref, rn);
        break;
    }
    case 4: {                                         /* merge with basedir */
        const char *be = bp0, *bs, *rq;
        while (be < bpend && *be != '?') be++;
        bs = be;
        while (bs > bp0 && *--bs != '/')
            ;
        if (*bs == '/')
            FE_PUT(bp0, (size_t)(bs - bp0) + 1);      /* incl. the slash */
        else
            FE_PUT("/", 1);                           /* empty base path */
        rq = memchr(ref, '?', rn);
        FE_PUT(ref, rq ? (size_t)(rq - ref) : rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        if (rq) FE_PUT(rq, rn - (size_t)(rq - ref));
        break;
    }
    }
    out[wr] = 0;
    return (long)wr;
#undef FE_PUT
}


/* ---- SSRF gate: refuse hosts that are not on the public internet ---------
   The transport (the injected client) owns DNS and connect, so THIS layer can
   only classify what the URL NAMES. IP literals are range-checked against the
   RFC 6890 / IANA special-purpose assignments; hostname shapes that can only
   mean "internal" (localhost, any single-label name, mDNS-style suffixes) are
   refused too. 64:ff9b::/96 (NAT64) is Global=True per RFC 6890 and is
   deliberately NOT blocked.

   WHAT THIS GATE DOES NOT SEE, stated plainly: a PUBLIC-LOOKING NAME that
   resolves to a private address (DNS rebinding) passes this layer -- getaddrinfo
   runs inside dyn:http's connect path, which today performs no address-class
   check. A literal-IP url IS fully gated here (nothing to resolve); a hostname
   is gated on shape only. A crawler that must not touch internal networks
   needs an egress allowlist at the network boundary -- no in-process check
   that classifies names can honestly claim more. */

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

/* IMF-fixdate (RFC 7231 7.1.1.1): "Sun, 06 Nov 1994 08:49:37 GMT". Returns
   epoch seconds, or -1 on ANY deviation: the obsolete RFC 850 and asctime
   forms are refused rather than half-parsed. Proleptic-Gregorian days from
   civil date (Hinnant); timezones other than GMT are not a form this parser
   accepts. */
static int64_t fe_http_date(const char *s)
{
    static const char *const mon[12] = { "jan","feb","mar","apr","may","jun",
        "jul","aug","sep","oct","nov","dec" };
    int64_t days, era, yoe, doy, doe;
    long y;
    int day = 0, m = -1, yr = 0, hr = 0, mi = 0, se = 0, i;

    if (!s) return -1;
    while (*s && *s != ',') s++;
    if (*s != ',') return -1;
    s++;
    if (*s++ != ' ') return -1;
    for (i = 0; i < 2; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        day = day * 10 + (s[i] - '0');
    }
    if (day < 1 || day > 31) return -1;
    s += 2;
    if (*s++ != ' ') return -1;
    for (i = 0; i < 12; i++)
        if (!strncasecmp(s, mon[i], 3)) { m = i; break; }
    if (m < 0) return -1;
    s += 3;
    if (*s++ != ' ') return -1;
    for (i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        yr = yr * 10 + (s[i] - '0');
    }
    if (yr < 1601 || yr > 9999) return -1;
    s += 4;
    if (*s++ != ' ') return -1;
    for (i = 0; i < 3; i++) {
        int *out = i == 0 ? &hr : i == 1 ? &mi : &se;
        if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9')
            return -1;
        *out = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
        if (i < 2) { if (*s != ':') return -1; s++; }
    }
    if (hr > 23 || mi > 59 || se > 60) return -1;
    if (strncmp(s, " GMT", 4) && strncmp(s, " UT", 3)) return -1;

    y = yr;
    y -= m < 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return days * 86400 + hr * 3600 + mi * 60 + se;
}

/* Cache-Control: the one directive this store must obey is no-store. We
   ALWAYS revalidate before replay (conditional GET), so no-cache and
   max-age=0 are already our behaviour by construction; `private` addresses
   shared caches only. Scan is bounded: a hostile header cannot spin here. */
static int fe_cc_no_store(const char *cc)
{
    const char *p = cc;
    if (!p) return 0;
    while (*p && (size_t)(p - cc) < 512) {
        if (!strncasecmp(p, "no-store", 8)) {
            const char *q = p + 8;
            if (!*q || *q == ',' || *q == ';' || *q == ' ' || *q == '\t')
                return 1;
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return 0;
}

/* Backoff jitter for RETRY PACING ONLY -- synchronized retry storms are a
   herd problem even among cooperating crawlers. Drawn from the SHARED core
   SplitMix64 (core/dyn-prng.h) instead of a fifth private xorshift; state
   lives on the Fetcher, seeded once at construction from OS entropy, so
   there is no lazily-initialised static and no first-use race. SplitMix64
   accepts every state value including 0. Never security-relevant. */
static double fe_rand01(fe_t *f)
{
    return (double)(dyn_splitmix64(&f->rng) >> 11) / 9007199254740992.0;
}

static JSValue fe_raw(JSContext *ctx, fe_t *f, JSValueConst client,
                      const char *url, JSValueConst uh,
                      const char *if_none_match, const char *if_mod_since)
{
    JSValue a[4], ret, hdrs;
    JSAtom m;

    a[0] = JS_NewString(ctx, "GET");
    a[1] = JS_NewString(ctx, url);
    a[2] = JS_UNDEFINED;
    hdrs = JS_NewObject(ctx);
    /* Caller-supplied headers go in first (atom-defined, so the caller's
       spelling of each key survives and __proto__ cannot retarget us). */
    if (JS_IsObject(uh)) {
        JSPropertyEnum *tab = NULL;
        uint32_t n = 0, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, uh,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (i = 0; i < n; i++) {
                JSValue v = JS_GetProperty(ctx, uh, tab[i].atom);
                if (JS_IsException(v)) {
                    for (; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
                    js_free(ctx, tab);
                    JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
                    JS_FreeValue(ctx, hdrs);
                    return JS_EXCEPTION;
                }
                /* A value rides to the wire byte-for-byte, so CR or LF in it
                   splits the request (an injected X-Evil header rides along)
                   and a NUL truncates it. Refuse, naming the key: this is the
                   caller's value, so sanitizing it would hide the bug. */
                {
                    size_t vn = 0, b;
                    const char *vs = JS_ToCStringLen(ctx, &vn, v);
                    int bad = 0;
                    if (!vs) {
                        JS_FreeValue(ctx, v);
                        for (; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
                        js_free(ctx, tab);
                        JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
                        JS_FreeValue(ctx, hdrs);
                        return JS_EXCEPTION;
                    }
                    for (b = 0; b < vn; b++)
                        if (vs[b] == '\r' || vs[b] == '\n' || vs[b] == '\0')
                            { bad = 1; break; }
                    JS_FreeCString(ctx, vs);
                    if (bad) {
                        const char *ks = JS_AtomToCString(ctx, tab[i].atom);
                        JS_ThrowTypeError(ctx,
                            "Fetcher: header \"%s\" value contains CR, LF or "
                            "NUL -- a value rides to the wire byte-for-byte "
                            "and would split the request", ks ? ks : "?");
                        if (ks) JS_FreeCString(ctx, ks);
                        JS_FreeValue(ctx, v);
                        for (; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
                        js_free(ctx, tab);
                        JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
                        JS_FreeValue(ctx, hdrs);
                        return JS_EXCEPTION;
                    }
                }
                JS_DefinePropertyValue(ctx, hdrs, tab[i].atom, v,
                                       JS_PROP_C_W_E);
            }
            for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
        }
    }
    if (if_none_match)
        JS_DefinePropertyValueStr(ctx, hdrs, "If-None-Match",
                                  JS_NewString(ctx, if_none_match),
                                  JS_PROP_C_W_E);
    if (if_mod_since)
        JS_DefinePropertyValueStr(ctx, hdrs, "If-Modified-Since",
                                  JS_NewString(ctx, if_mod_since),
                                  JS_PROP_C_W_E);
    /* On EVERY request including robots.txt: a crawler identifying itself
       only for pages is not identified. Ours last => wins collisions. */
    JS_DefinePropertyValueStr(ctx, hdrs, "User-Agent",
                              JS_NewString(ctx, f->agent), JS_PROP_C_W_E);
    a[3] = hdrs;
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

/* The `headers` option must never carry credentials: it is applied to EVERY
   request -- robots.txt pre-fetches and cross-host redirect hops included --
   so an Authorization or Cookie header would leak to whichever host a 30x
   names. Refuse at construction, naming the key. dyna:http is the place for
   authenticated fetches. Returns NULL if clean, else the offending key in
   buf. */
static const char *fe_credential_header(JSContext *ctx, JSValueConst hv,
                                        char *buf, size_t cap)
{
    static const char * const cred[] = {
        "authorization", "cookie", "proxy-authorization", "set-cookie"
    };
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;

    buf[0] = '\0';                       /* empty means clean; buf IS the flag */
    if (!JS_IsObject(hv))
        return NULL;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, hv,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return NULL;                     /* enumeration failed: fe_raw will
                                            simply copy nothing */
    for (i = 0; i < n; i++) {
        const char *k = JS_AtomToCString(ctx, tab[i].atom);
        size_t j;
        if (!k) continue;
        for (j = 0; j < countof(cred); j++) {
            if (!strcasecmp(k, cred[j])) {
                snprintf(buf, cap, "%s", k);
                JS_FreeCString(ctx, k);
                goto out;
            }
        }
        JS_FreeCString(ctx, k);
    }
out:
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    return buf[0] ? buf : NULL;
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

/* All values of a (case-insensitive) header name, joined with '\n' (multiple
   header lines stay separate so "bot:" scopes cannot leak across them).
   Returns a malloc'd string or NULL. */
static char *fe_headers_all(JSContext *ctx, JSValueConst res, const char *want)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    size_t cap = 0, len = 0;
    char *out = NULL;
    if (!JS_IsObject(h)) { JS_FreeValue(ctx, h); return NULL; }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) != 0) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        const char *k = JS_AtomToCString(ctx, tab[i].atom);
        int hit = k && !strcasecmp(k, want);
        if (k) JS_FreeCString(ctx, k);
        if (!hit) continue;
        {
            JSValue v = JS_GetProperty(ctx, h, tab[i].atom);
            const char *vs = JS_ToCString(ctx, v);
            if (vs) {
                size_t vl = strlen(vs);
                if (len + vl + 2 > cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    char *no;
                    while (nc < len + vl + 2) nc *= 2;
                    no = (char *)realloc(out, nc);
                    if (!no) { JS_FreeCString(ctx, vs); JS_FreeValue(ctx, v); break; }
                    out = no; cap = nc;
                }
                if (len) out[len++] = '\n';
                memcpy(out + len, vs, vl); len += vl;
                JS_FreeCString(ctx, vs);
            }
            JS_FreeValue(ctx, v);
        }
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, h);
    if (out)
        out[len] = '\0';
    return out;
}

/* Append one directive to the '\n'-joined list, lowercased, if not already
   present (case-insensitive). "none" was already expanded by the caller. */
static void fe_dirs_add(char **out, size_t *len, size_t *cap,
                        const char *tok, size_t tl)
{
    size_t i, b = 0;
    if (!*out && tl) {
        size_t nc = 64;
        while (nc < tl + 1) nc *= 2;
        *out = (char *)malloc(nc);
        if (!*out) return;
        *cap = nc;
    }
    for (i = 0; i < *len; i++) {
        if ((*out)[i] == '\n') {
            if (i - b == tl && !strncasecmp(*out + b, tok, tl)) return;
            b = i + 1;
        }
    }
    if (*len - b == tl && !strncasecmp(*out + b, tok, tl)) return;
    if (*len + tl + 2 > *cap) {
        size_t nc = *cap * 2;
        char *no;
        while (nc < *len + tl + 2) nc *= 2;
        no = (char *)realloc(*out, nc);
        if (!no) return;
        *out = no; *cap = nc;
    }
    if (*len) (*out)[(*len)++] = '\n';
    for (i = 0; i < tl; i++) {
        char c = tok[i];
        (*out)[*len] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        (*len)++;
    }
    (*out)[*len] = '\0';
}

/* X-Robots-Tag (de-facto REP; Google/MDN): comma-separated directives, each
   possibly scoped "bot: directive"; a scoped rule applies only when the bot
   name matches OUR agent's product token (case-insensitive exact, or '*').
   Directives that take an argument (max-snippet:, max-image-preview:,
   max-video-preview:, unavailable_after:) are never treated as a scope.
   "none" expands to noindex+nofollow, "all" is a no-op; the result is the
   HONORED directives, lowercased and deduped, '\n'-joined. */
static char *fe_parse_robots_tag(const char *joined, const char *agent)
{
    size_t agt = agent ? rb_token_len(agent, strlen(agent)) : 0;
    char *out = NULL;
    size_t len = 0, cap = 0, i = 0;
    while (joined && joined[i]) {
        size_t b = i;
        while (joined[i] && joined[i] != ',' && joined[i] != '\n') i++;
        {
            size_t t0 = b, t1 = i;
            int applies = 1;
            while (t0 < t1 && (joined[t0] == ' ' || joined[t0] == '\t')) t0++;
            while (t1 > t0 && (joined[t1-1] == ' ' || joined[t1-1] == '\t')) t1--;
            if (t1 > t0) {
                const char *c = (const char *)memchr(joined + t0, ':',
                                                     t1 - t0);
                if (c) {
                    size_t bn = (size_t)(c - (joined + t0));
                    int arg_directive =
                        (bn == 11 && !strncasecmp(joined + t0, "max-snippet", 11)) ||
                        (bn == 17 && !strncasecmp(joined + t0, "max-image-preview", 17)) ||
                        (bn == 17 && !strncasecmp(joined + t0, "max-video-preview", 17)) ||
                        (bn == 17 && !strncasecmp(joined + t0, "unavailable_after", 17));
                    if (!arg_directive && agent) {
                        size_t rs = t0 + bn + 1;
                        while (rs < t1 && (joined[rs] == ' ' || joined[rs] == '\t'))
                            rs++;
                        applies = (bn == 1 && joined[t0] == '*');
                        if (!applies && bn == agt &&
                            !strncasecmp(joined + t0, agent, agt))
                            applies = 1;
                        t0 = rs;
                    }
                }
                if (applies && t1 > t0) {
                    if (t1 - t0 == 4 && !strncasecmp(joined + t0, "none", 4)) {
                        fe_dirs_add(&out, &len, &cap, "noindex", 7);
                        fe_dirs_add(&out, &len, &cap, "nofollow", 8);
                    } else if (t1 - t0 == 3 &&
                               !strncasecmp(joined + t0, "all", 3)) {
                        /* "all": the default; no restriction to record */
                    } else {
                        fe_dirs_add(&out, &len, &cap, joined + t0, t1 - t0);
                    }
                }
            }
        }
        if (joined[i] == ',' || joined[i] == '\n') i++;
    }
    return out;
}

/* Does the rel parameter (rel= at offset eq within p) name "canonical"?
   eq is the offset of the '='; the value scan skips quotes and whitespace
   and consumes every non-token char, so j ALWAYS advances -- a malformed
   value cannot spin here. */
static int fe_link_rel_canonical(const char *p, size_t n, size_t eq)
{
    size_t j = eq + 1;
    while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == '"' || p[j] == '\''))
        j++;
    while (j < n) {
        size_t k = j;
        while (k < n && p[k] != ',' && p[k] != ';' && p[k] != ' ' &&
               p[k] != '\t' && p[k] != '"' && p[k] != '\'')
            k++;
        if (k - j == 9 && !strncasecmp(p + j, "canonical", 9))
            return 1;
        j = k;
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ',' ||
                         p[j] == '"' || p[j] == '\''))
            j++;
        if (j >= n || p[j] == ';')
            break;                       /* rel ended; next param */
    }
    return 0;
}

/* RFC 8288 Link: <url>; rel="canonical". First canonical wins; resolved
   against the request url; only http(s) results are exposed. */
static void fe_link_canonical(JSContext *ctx, JSValueConst res,
                              const char *base, JSValue out)
{
    char *all = fe_headers_all(ctx, res, "link");
    size_t i = 0;
    char *url = NULL;
    if (!all)
        return;
    while (all[i]) {
        if (all[i] == '<') {
            size_t e = i + 1, p, pend, k;
            while (all[e] && all[e] != '>' && all[e] != '\n') e++;
            if (all[e] != '>') { i++; continue; }
            p = e + 1;
            pend = p;
            while (all[pend] && all[pend] != ',' && all[pend] != '\n') pend++;
            for (k = p; k + 4 <= pend; k++)
                if ((all[k]=='r'||all[k]=='R') && (all[k+1]=='e'||all[k+1]=='E') &&
                    (all[k+2]=='l'||all[k+2]=='L') && all[k+3]=='=' &&
                    fe_link_rel_canonical(all + p, pend - p, (k + 3) - p)) {
                    size_t u0 = i + 1, u1 = e;
                    while (u0 < u1 && (all[u0] == ' ' || all[u0] == '\t')) u0++;
                    while (u1 > u0 && (all[u1-1] == ' ' || all[u1-1] == '\t')) u1--;
                    url = (char *)malloc(u1 - u0 + 1);
                    if (url) {
                        memcpy(url, all + u0, u1 - u0);
                        url[u1 - u0] = 0;
                    }
                    break;
                }
            i = pend;
            if (url)
                break;
            continue;
        }
        i++;
    }
    free(all);
    if (!url)
        return;
    {
        char rbuf[1024];
        long rl = fe_resolve(base, url, rbuf, sizeof rbuf);
        free(url);
        if (rl > 0 &&
            (!strncmp(rbuf, "http://", 7) || !strncmp(rbuf, "https://", 8)))
            JS_DefinePropertyValueStr(ctx, out, "canonicalUrl",
                                      JS_NewStringLen(ctx, rbuf, (size_t)rl),
                                      JS_PROP_C_W_E);
    }
}

/* robots.txt, once per host. A 5xx or a network error means the file is
   undefined and RFC 9309 2.3.1.4 REQUIRES complete disallow; only a 4xx
   "Unavailable" (2.3.1.3) MAY be treated as allow. `tried` stops a 404
   being re-fetched for every URL on the host. */
static void fe_load_robots(JSContext *ctx, fe_t *f, JSValueConst client,
                           JSValueConst uh, fe_host_t *h, int https)
{
    char url[600];
    JSValue res;
    int st = 0, hop;
    h->robots_tried = 1;
    snprintf(url, sizeof url, "%s%s/robots.txt",
             https ? "https://" : "http://", h->host);

    /* RFC 9309 2.3.1.2: robots fetching MUST follow redirects. Canonical-
       host 301s are the NORM on the web, and treating one as "undefined
       file" would disallow whole healthy sites. Follow up to 2 redirects
       (3 requests total); any hop that is unresolvable, unsafe (private
       target, https downgrade) or that leaves us on a 3xx past the cap is
       fail-closed as before. */
    res = JS_UNDEFINED;
    for (hop = 0; hop < 3; hop++) {
        JS_FreeValue(ctx, res);
        res = fe_raw(ctx, f, client, url, uh, NULL, NULL);
        if (JS_IsException(res)) {
            /* Network error: the file is UNDEFINED. RFC 9309 2.3.1.4 says
               that means "MUST assume complete disallow" -- the old
               default-allow here was a spec violation and an SSRF
               widening. */
            JS_FreeValue(ctx, JS_GetException(ctx));
            h->robots_unreachable = 1;
            return;
        }
        st = fe_status(ctx, res);
        if (st >= 300 && st < 400 && hop < 2) {
            JSValue hold = JS_UNDEFINED;
            char nxt[600], host2[300];
            const char *pp, *loc = fe_header(ctx, res, "location", &hold);
            int ok = 0;
            if (loc &&
                fe_resolve(url, loc, nxt, sizeof nxt) > 0 &&
                fe_split(nxt, host2, sizeof host2, &pp) >= 0) {
                ok = 1;
                if (!f->allow_private_hosts && fe_host_is_private(host2))
                    ok = 0;      /* a 30x must not smuggle us inward */
                else if (fe_scheme_is_https(url) && !fe_scheme_is_https(nxt))
                    ok = 0;      /* downgrade: fail closed, not thrown --
                                    robots policy refuses, it does not
                                    raise into the caller's fetch */
            }
            if (loc) JS_FreeCString(ctx, loc);
            JS_FreeValue(ctx, hold);
            if (ok) {
                snprintf(url, sizeof url, "%s", nxt);
                continue;        /* next hop */
            }
            h->robots_unreachable = 1;
            JS_FreeValue(ctx, res);
            return;
        }
        break;
    }

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
                    /* a REFRESH replaces the old copy; the new one is owned */
                    if (h->robots) rb_free(h->robots);
                    h->robots = r;
                    h->robots_ok = 1;
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
           4xx "Unavailable" (2.3.1.3) -- a redirect that survived the hop
           cap above, a status of 0, a non-standard code -- is unreachable:
           an undefined file MUST mean complete disallow (2.3.1.4), so only
           the two definitive outcomes may allow. Fail closed (audit 14.2). */
        h->robots_unreachable = 1;
    }
    /* 4xx "Unavailable" (2.3.1.3): MAY allow -- robots stays NULL. */
    h->robots_next_ms = (double)dyn_timer_now_ms() + f->robots_ttl_ms;
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
    JSValue res = JS_UNDEFINED, ret = JS_EXCEPTION, client, uh;

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
    {
        /* a fragment is client-side state, never part of the request, the
           robots check, or the dedup key (RFC 9110: it is not sent) */
        char *frag = strchr(cur, '#');
        if (frag)
            *frag = 0;
    }
    client = JS_GetPropertyStr(ctx, this_val, "_client");
    uh = JS_GetPropertyStr(ctx, this_val, "_headers");

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
            double now_ms = (double)dyn_timer_now_ms();
            if (!h->robots_tried || now_ms >= h->robots_next_ms)
                fe_load_robots(ctx, f, client, uh, h, https);
            /* A failed REFRESH must not discard a good copy: RFC 9309
               2.3.1.6 prefers the last-known-good file over a fresh
               disallow. The failure backoff was stamped by the load. */
            if (h->robots_unreachable && h->robots_ok)
                h->robots_unreachable = 0;
            if (h->robots_unreachable ||
                (h->robots && !rb_allows_path(h->robots, path, strlen(path)))) {
                f->skipped_robots++;
                ret = JS_NewObject(ctx);
                JS_DefinePropertyValueStr(ctx, ret, "status", JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "skippedByRobots", JS_TRUE, JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, ret, "contentType", JS_NewString(ctx, ""), JS_PROP_C_W_E);
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

            /* conditional request when a validator for THIS url is stored */
            {
                fe_cache_ent_t *ce = f->revalidate ? fe_cache_find(f, cur) : NULL;
                res = fe_raw(ctx, f, client, cur, uh,
                             ce && ce->etag ? ce->etag : NULL,
                             ce && ce->lm   ? ce->lm   : NULL);
            }
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
            /* Exponential backoff, jittered: synchronized retry storms are
               a herd problem even among cooperating crawlers. Retry-After,
               when the server names one, OVERRIDES the curve untouched --
               the server knows better than our jitter. */
            back = f->min_delay_ms * (double)(1 << (attempt < 10 ? attempt : 10));
            back *= 0.75 + 0.5 * fe_rand01(f);
            if (!JS_IsException(res)) {
                ra = fe_header(ctx, res, "retry-after", &hold);
                if (ra) {
                    /* Two legal forms (RFC 7231): delay-seconds, or an
                       IMF-fixdate naming WHEN to come back. Half-parsing a
                       date as seconds yields 0, which silently ignored the
                       server's instruction -- so both forms are honoured. */
                    double secs = strtod(ra, NULL);
                    if (!(secs > 0)) {
                        int64_t when = fe_http_date(ra);
                        if (when > 0) {
                            /* EPOCH wall clock, matching fe_http_date: the
                               monotonic dyn_timer_now_ms here once produced
                               a +1.7e9s "delta" and every date-form retry
                               slept the full 60s cap instead of the seconds
                               the server asked for. */
                            int64_t now = (int64_t)time(NULL);
                            secs = when > now ? (double)(when - now) : 0.0;
                        }
                    }
                    if (secs > 0) back = secs * 1000.0;
                    JS_FreeCString(ctx, ra);
                }
                /* A server-given delay beyond the sleep cap is an
                   INSTRUCTION we must not violate by sleeping less: retry
                   anyway and we are back in a second while it said an hour.
                   Serve the 429/5xx as the final answer instead -- the
                   caller sees the response and decides when to come back. */
                if (back > 60000.0) {
                    JS_FreeValue(ctx, hold);
                    break;
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
                char nxt[sizeof cur];
                int prev_https = fe_scheme_is_https(cur);
                if (fe_resolve(cur, loc, nxt, sizeof nxt) > 0 &&
                    strcmp(nxt, cur)) {
                    /* A TLS page must not walk its crawler into plaintext
                       just because a Location said so. Refused by default;
                       the opt-in is spelled at the call site, not global. */
                    if (prev_https && !fe_scheme_is_https(nxt) &&
                        !f->allow_insecure_downgrade) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, res);
                        JS_ThrowRangeError(ctx,
                            "Fetcher: redirect downgrades https to http "
                            "(%.80s); pass allowInsecureDowngrade: true to "
                            "follow it", nxt);
                        goto out;
                    }
                    snprintf(cur, sizeof cur, "%s", nxt);
                    JS_FreeCString(ctx, loc);
                    JS_FreeValue(ctx, hold);
                    JS_FreeValue(ctx, res);
                    res = JS_UNDEFINED;
                    continue;                   /* next hop */
                }
                /* unresolvable Location, or one that lands exactly on the
                   current url (#frag-only): serve this response as final */
            }
            if (loc && hop >= f->max_redirects) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                JS_ThrowRangeError(ctx,
                    "Fetcher: more than %d redirects", f->max_redirects);
                goto out;
            }
            if (loc) { JS_FreeCString(ctx, loc); JS_FreeValue(ctx, hold); }
        }

        /* ---- body cap, then validator store / 304 synthesis ------------- */
        {
            JSValue b, hold_cl = JS_UNDEFINED;
            size_t bn = 0;
            const char *bs;

            /* Declared length first: an honest server must not ship a body
               the cap would reject. The check below on RECEIVED bytes still
               bounds retention when the header lies or is absent -- but it
               runs after transfer, so this early refusal is what saves the
               bandwidth, and only the two together are honest. */
            {
                const char *clv = fe_header(ctx, res, "content-length",
                                            &hold_cl);
                if (clv) {
                    double decl = strtod(clv, NULL);
                    if (decl > f->max_body) {
                        JS_FreeCString(ctx, clv);
                        JS_FreeValue(ctx, hold_cl);
                        JS_FreeValue(ctx, res);
                        JS_ThrowRangeError(ctx,
                            "Fetcher: declared Content-Length %.0f exceeds "
                            "maxBodyBytes", decl);
                        goto out;
                    }
                    JS_FreeCString(ctx, clv);
                }
                JS_FreeValue(ctx, hold_cl);
            }

            b = JS_GetPropertyStr(ctx, res, "body");
            bs = JS_ToCStringLen(ctx, &bn, b);
            if (!bs) {
                JS_FreeValue(ctx, b); JS_FreeValue(ctx, res);
                goto out;                        /* conversion error pending */
            }
            if ((double)bn > f->max_body) {
                JS_FreeCString(ctx, bs); JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, res);
                JS_ThrowRangeError(ctx,
                    "Fetcher: body of %zu bytes exceeds maxBodyBytes", bn);
                goto out;
            }
            if (st == 304 && f->revalidate) {
                /* Not modified: answer from the metadata store, honestly
                   labelled -- status stays what the wire said. */
                fe_cache_ent_t *ce = fe_cache_find(f, cur);
                if (ce && ce->body) {
                    f->revalidated += 1;
                    f->saved_bytes += (double)ce->blen;
                    f->bytes += (double)bn;
                    JS_FreeCString(ctx, bs); JS_FreeValue(ctx, b);
                    JS_FreeValue(ctx, res);
                    res = JS_NewObject(ctx);
                    JS_DefinePropertyValueStr(ctx, res, "status",
                                              JS_NewInt32(ctx, 304), JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, res, "contentType",
                        JS_NewString(ctx, ce->ct ? ce->ct : ""), JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, res, "body",
                        JS_NewStringLen(ctx, ce->body, ce->blen), JS_PROP_C_W_E);
                    f->fetched++;
                    JS_DefinePropertyValueStr(ctx, res, "url",
                                              JS_NewString(ctx, cur), JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_TRUE, JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, res, "notModified", JS_TRUE, JS_PROP_C_W_E);
                    ret = res;
                    goto out;
                }
                /* 304 with nothing stored: hand through untouched below */
            } else if (st >= 200 && st < 300) {
                const char *etg, *lmd, *ctv;
                JSValue he, hl, hc;
                etg = fe_header(ctx, res, "etag", &he);
                lmd = fe_header(ctx, res, "last-modified", &hl);
                ctv = fe_header(ctx, res, "content-type", &hc);
                if ((etg || lmd) && f->revalidate) {
                    JSValue hcc = JS_UNDEFINED;
                    const char *cc = fe_header(ctx, res, "cache-control",
                                               &hcc);
                    if (!fe_cc_no_store(cc))
                        fe_cache_put(f, cur, etg, lmd, ctv, bs, bn);
                    if (cc) JS_FreeCString(ctx, cc);
                    JS_FreeValue(ctx, hcc);
                }
                if (etg) JS_FreeCString(ctx, etg);
                JS_FreeValue(ctx, he);
                if (lmd) JS_FreeCString(ctx, lmd);
                JS_FreeValue(ctx, hl);
                if (ctv) JS_FreeCString(ctx, ctv);
                JS_FreeValue(ctx, hc);
            }
            f->bytes += (double)bn;
            JS_FreeCString(ctx, bs);
            JS_FreeValue(ctx, b);
        }
        /* ---- robots directives on the wire (X-Robots-Tag) and the HTTP
           twin of rel=canonical (Link: <...>; rel="canonical", RFC 8288) --
           parsed once here, exposed on the response for the crawler. */
        {
            char *xrt = fe_headers_all(ctx, res, "x-robots-tag");
            if (xrt) {
                char *dirs = fe_parse_robots_tag(xrt, f->agent);
                free(xrt);
                if (dirs) {
                    JSValue arr = JS_NewArray(ctx);
                    size_t k = 0;
                    uint32_t ai = 0;
                    if (JS_IsException(arr)) {
                        free(dirs);
                        JS_FreeValue(ctx, JS_GetException(ctx));
                    } else {
                        while (dirs[k]) {
                            size_t b = k;
                            while (dirs[k] && dirs[k] != '\n') k++;
                            JS_SetPropertyUint32(ctx, arr, ai++,
                                JS_NewStringLen(ctx, dirs + b, k - b));
                            if (dirs[k]) k++;
                        }
                        JS_DefinePropertyValueStr(ctx, res, "robotsDirectives",
                                                  arr, JS_PROP_C_W_E);
                        free(dirs);
                    }
                }
            }
            fe_link_canonical(ctx, res, cur, res);
        }
        f->fetched++;
        {
            JSValue hold_ct = JS_UNDEFINED;
            const char *ctv = fe_header(ctx, res, "content-type", &hold_ct);
            JS_DefinePropertyValueStr(ctx, res, "contentType",
                                      JS_NewString(ctx, ctv ? ctv : ""),
                                      JS_PROP_C_W_E);
            if (ctv) JS_FreeCString(ctx, ctv);
            JS_FreeValue(ctx, hold_ct);
        }
        JS_DefinePropertyValueStr(ctx, res, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_FALSE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "notModified", JS_FALSE, JS_PROP_C_W_E);
        ret = res;
        goto out;
    }
out:
    JS_FreeValue(ctx, client);
    JS_FreeValue(ctx, uh);
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
    JS_DefinePropertyValueStr(ctx, o, "revalidated", JS_NewFloat64(ctx, f->revalidated), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "savedBytes", JS_NewFloat64(ctx, f->saved_bytes), JS_PROP_C_W_E);
    return o;
}

static JSValue dyn_fe_ctor(JSContext *ctx, JSValueConst nt, int argc,
                           JSValueConst *argv)
{
    fe_t *f;
    JSValue av, cv, hv;
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
    hv = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (!JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `client` is required -- pass an HTTPClient from dyna:net. "
            "It is injected rather than constructed here so dyna:scrape does "
            "not link dyna:net, and so a test can drive the policy against a "
            "mock.");
    }
    if (!JS_IsUndefined(hv) && !JS_IsNull(hv) && !JS_IsObject(hv)) {
        JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `headers` must be an object of extra request headers");
    }
    {
        char cred[64];
        if (fe_credential_header(ctx, hv, cred, sizeof cred)) {
            JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
            JS_FreeCString(ctx, agent);
            return JS_ThrowTypeError(ctx,
                "Fetcher: `headers` may not carry credentials (got \"%s\") -- "
                "extra headers are sent to every host, including redirect "
                "targets and robots.txt pre-fetches; fetch authenticated "
                "endpoints through dyna:http instead", cred);
        }
    }
    f = (fe_t *)calloc(1, sizeof(*f));
    if (!f) { JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
              JS_FreeCString(ctx, agent); return JS_ThrowOutOfMemory(ctx); }
    f->agent = strdup(agent);
    JS_FreeCString(ctx, agent);
    if (!f->agent) { JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                     free(f); return JS_ThrowOutOfMemory(ctx); }

    f->robots_on     = fe_num_prop(ctx, argv[0], "robots", 1) ? 1 : 0;
    f->min_delay_ms  = fe_num_prop(ctx, argv[0], "minDelayMs", 1000);
    f->retries       = fe_num_prop(ctx, argv[0], "retries", 3);
    f->max_redirects = fe_num_prop(ctx, argv[0], "maxRedirects", 5);
    f->max_body      = (double)fe_num_prop(ctx, argv[0], "maxBodyBytes", 8 << 20);
    f->allow_private_hosts =
        fe_num_prop(ctx, argv[0], "allowPrivateHosts", 0) ? 1 : 0;
    f->revalidate    = fe_num_prop(ctx, argv[0], "revalidate", 1) ? 1 : 0;
    f->robots_ttl_ms = (double)fe_num_prop(ctx, argv[0], "robotsTtlMs", 86400000);
    if (f->robots_ttl_ms < 0) f->robots_ttl_ms = 0;
    f->allow_insecure_downgrade =
        fe_num_prop(ctx, argv[0], "allowInsecureDowngrade", 0) ? 1 : 0;
    if (f->retries < 0) f->retries = 0;
    if (f->max_redirects < 0) f->max_redirects = 0;
    if (f->min_delay_ms < 0) f->min_delay_ms = 0;
    /* Jitter seed: OS entropy first, time-and-address as the fallback (the
       old xorshift mix). SplitMix64 has no forbidden state, so 0 would be
       fine -- the fallback exists for a failed kernel source, not for 0. */
    if (dyn_os_entropy(&f->rng, sizeof f->rng) != 0)
        f->rng = ((uint64_t)dyn_timer_now_ms() << 17) ^ (uintptr_t)f ^
                 0x9E3779B97F4A7C15ULL;
    {
        JSValue obj = dyn_res_wrap(ctx, dyn_fe_class_id, f, fe_dispose);
        if (JS_IsException(obj)) { JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv); return obj; }
        /* Non-enumerable and non-configurable: the GC traces them, so no
           gc_mark and no free are needed, and they are not part of the API.
           `_headers` may be undefined -- fe_raw checks before reading it. */
        JS_DefinePropertyValueStr(ctx, obj, "_client", cv, 0);
        JS_DefinePropertyValueStr(ctx, obj, "_headers",
                                  JS_IsObject(hv) ? hv : JS_UNDEFINED, 0);
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

/* A page can contribute only bounded new frontier: a hostile page must not
   convert maxPages into unbounded memory on the queue it cannot control. */
#define CR_MAX_PENDING 10000

typedef struct {
    cr_item_t *q;                /* frontier, FIFO */
    size_t     qn, qcap, qhead;
    char     **seen;             /* visited urls */
    size_t     sn, scap;
    int        max_pages, max_depth, same_host;
    size_t     dropped;
    char      *link_field;
    char      *base_field;       /* extractor field naming the page's <base>
                                  (or NULL: resolve against the request url) */
    char      *canonical_field;  /* rel=canonical: dedup key when absolute */
    char      *rel_field;        /* parallel array of link rel strings */
    char      *robots_field;     /* extractor field naming the meta robots */
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
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    free(c);
}

/* case-insensitive "nofollow" token test over one rel string. Token-exact:
   "nofollowed" or "x-nofollow" are not the directive; only a whitespace- or
   comma-separated token is. */
static int cr_is_nofollow(const char *rel)
{
    size_t n;
    if (!rel) return 0;
    n = strlen(rel);
    if (n < 8) return 0;
    {
        const char *q = rel;
        while ((size_t)(q - rel) + 8 <= n) {
            if (!strncasecmp(q, "nofollow", 8)) {
                char a = (q > rel && q[-1] != ' ' && q[-1] != '\t' &&
                          q[-1] != ',') ? 'x' : ' ';
                char b = (q[8] && q[8] != ' ' && q[8] != '\t' &&
                          q[8] != ',') ? 'x' : ' ';
                if (a == ' ' && b == ' ')
                    return 1;
            }
            q++;
        }
    }
    return 0;
}

/* Lowercase a host and drop the scheme's DEFAULT port, for the sameHost gate
   (URLs are case-insensitive in the host, and the default port is redundant:
   "EXAMPLE.com:80" IS "example.com"). */
static void cr_norm_host(const char *host, int https, char *out, size_t cap)
{
    size_t n = strlen(host), keep = n, i;
    if (n && host[0] == '[') {
        const char *cl = strchr(host, ']');
        if (cl) keep = (size_t)(cl - host) + 1;
    } else {
        const char *c = strchr(host, ':');
        if (c) {
            const char *pfx = https ? ":443" : ":80";
            keep = (size_t)(c - host);
            if (strcmp(c, pfx)) keep = n;      /* non-default port: keep */
        }
    }
    if (keep >= cap) keep = cap - 1;
    for (i = 0; i < keep; i++)
        out[i] = (char)((host[i] >= 'A' && host[i] <= 'Z') ? host[i] + 32
                                                           : host[i]);
    out[keep] = 0;
}

/* Normalised dedup key: scheme+authority lowercased with the default port
   dropped, path verbatim. Two spellings of the same url fetch once. */
static long cr_key(const char *url, char *out, size_t cap)
{
    const char *sp = strstr(url, "://");
    const char *path;
    size_t sl, al, keep, i, tl;
    int https;
    if (!sp)
        return -1;
    https = !strncmp(url, "https:", 6);
    sl = (size_t)(sp - url) + 3;
    path = strchr(sp + 3, '/');
    if (!path) path = strchr(sp + 3, '?');
    if (!path) path = url + strlen(url);
    al = (size_t)(path - (sp + 3));
    keep = al;
    if (al && (sp + 3)[0] == '[') {
        const char *cl = memchr(sp + 3, ']', al);
        if (cl) keep = (size_t)(cl - (sp + 3)) + 1;
    } else {
        const char *c = memchr(sp + 3, ':', al);
        if (c) {
            size_t pl = (size_t)(c - (sp + 3));
            size_t pn = al - pl;
            if ((!https && pn == 3 && !memcmp(c, ":80", 3)) ||
                (https && pn == 4 && !memcmp(c, ":443", 4)))
                keep = pl;
        }
    }
    tl = (size_t)(url + strlen(url) - path);
    if (sl + keep + tl + 1 > cap)
        return -1;
    for (i = 0; i < sl; i++)
        out[i] = (char)((url[i] >= 'A' && url[i] <= 'Z') ? url[i] + 32
                                                         : url[i]);
    for (i = 0; i < keep; i++) {
        char c = (sp + 3)[i];
        out[sl + i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    memcpy(out + sl + keep, path, tl);
    out[sl + keep + tl] = 0;
    return (long)(sl + keep + tl);
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
    if (c->qn - c->qhead >= CR_MAX_PENDING)
        return 1;                        /* frontier cap: dropped, counted */
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
    /* <base href> support: name an extractor string field that yields the
       page's EFFECTIVE base; unset means resolve against the request url. */
    lf = JS_GetPropertyStr(ctx, opt, "baseField");
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->base_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->base_field) { JS_FreeValue(ctx, lf); JS_FreeValue(ctx, opt);
                              free(c); return JS_ThrowOutOfMemory(ctx); }
    }
    JS_FreeValue(ctx, lf);
    lf = JS_GetPropertyStr(ctx, opt, "canonicalField");
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->canonical_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->canonical_field) { JS_FreeValue(ctx, lf);
                                   JS_FreeValue(ctx, opt);
                                   free(c); return JS_ThrowOutOfMemory(ctx); }
    }
    JS_FreeValue(ctx, lf);
    lf = JS_GetPropertyStr(ctx, opt, "relField");
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->rel_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->rel_field) { JS_FreeValue(ctx, lf); JS_FreeValue(ctx, opt);
                             free(c); return JS_ThrowOutOfMemory(ctx); }
    }
    JS_FreeValue(ctx, lf);
    /* meta robots: name an extractor field whose value is the page's
       <meta name="robots" content="..."> content; its nofollow gates link
       following and the directives join page.robots */
    lf = JS_GetPropertyStr(ctx, opt, "robotsField");
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->robots_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->robots_field) { JS_FreeValue(ctx, lf); JS_FreeValue(ctx, opt);
                                free(c); return JS_ThrowOutOfMemory(ctx); }
    }
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
    if (strlen(seed) + 1 > 1024) {
        JS_FreeCString(ctx, seed);
        return JS_ThrowRangeError(ctx, "Crawl: seed url is too long");
    }
    {
        char sbuf[1024];
        int https;
        memcpy(sbuf, seed, strlen(seed) + 1);
        JS_FreeCString(ctx, seed);
        {
            /* a fragment is client-side state, never a request or a dedup
               key: strip it so the seed and a link to the same page dedup */
            char *frag = strchr(sbuf, '#');
            if (frag) *frag = 0;
        }
        https = fe_split(sbuf, c->seed_host, sizeof c->seed_host, &path);
        if (https < 0)
            return JS_ThrowTypeError(ctx, "Crawl: seed must be an http(s) url");
        {
            /* the host gate compares case-insensitively, port-normalised */
            char nh[300];
            cr_norm_host(c->seed_host, https, nh, sizeof nh);
            memcpy(c->seed_host, nh, strlen(nh) + 1);
        }
        if (cr_push(c, sbuf, 0) < 0)
            return JS_ThrowOutOfMemory(ctx);
        {
            /* the seen set is keyed on the NORMALISED form */
            char key[1024];
            if (cr_key(sbuf, key, sizeof key) >= 0) {
                if (cr_mark_seen(c, key) < 0)
                    return JS_ThrowOutOfMemory(ctx);
            } else if (cr_mark_seen(c, sbuf) < 0) {
                return JS_ThrowOutOfMemory(ctx);
            }
        }
    }
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
    int depth, page_status, page_nofollow = 0;
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
    page_status = 0;
    {
        JSValue st = JS_GetPropertyStr(ctx, res, "status");
        JS_ToInt32(ctx, &page_status, st);
        JS_DefinePropertyValueStr(ctx, page, "status", st, JS_PROP_C_W_E);
    }

    /* wire-level robots directives (X-Robots-Tag), as honored for our agent */
    {
        JSValue rd = JS_GetPropertyStr(ctx, res, "robotsDirectives");
        if (JS_IsArray(ctx, rd))
            JS_DefinePropertyValueStr(ctx, page, "robots",
                                      JS_DupValue(ctx, rd), JS_PROP_C_W_E);
        JS_FreeValue(ctx, rd);
    }

    /* Extract, then take the links from the named field. The extractor uses
       the real HTML parser; Crawl does not scan markup itself.
       VALUE extraction runs on ANY status -- a 403 or 500 page can still
       carry data worth keeping, and a bad page is exactly when a scraper
       needs its extractor. LINK FOLLOWING is gated to 2xx: error pages are
       not trusted to point anywhere (and error pages pointing at pages is
       how a frontier fills with junk). */
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
            /* page-level robots: the meta robots content from the named
               extractor field, merged onto page.robots (the header
               directives were attached above). Meta content is never
               bot-scoped, so no agent is passed. */
            if (c->robots_field && JS_IsObject(val)) {
                JSValue rv = JS_GetPropertyStr(ctx, val, c->robots_field);
                if (JS_IsString(rv)) {
                    const char *rs = JS_ToCString(ctx, rv);
                    if (rs) {
                        char *dirs = fe_parse_robots_tag(rs, NULL);
                        JS_FreeCString(ctx, rs);
                        if (dirs) {
                            size_t k = 0;
                            JSValue rb = JS_GetPropertyStr(ctx, page, "robots");
                            if (!JS_IsArray(ctx, rb)) {
                                JS_FreeValue(ctx, rb);
                                rb = JS_NewArray(ctx);
                                if (!JS_IsException(rb))
                                    JS_DefinePropertyValueStr(ctx, page, "robots",
                                                              JS_DupValue(ctx, rb),
                                                              JS_PROP_C_W_E);
                            }
                            while (dirs[k] && !JS_IsException(rb)) {
                                size_t b = k;
                                uint32_t m2 = 0, j;
                                int dup2 = 0;
                                JSValue lv;
                                while (dirs[k] && dirs[k] != '\n') k++;
                                lv = JS_GetPropertyStr(ctx, rb, "length");
                                JS_ToUint32(ctx, &m2, lv);
                                JS_FreeValue(ctx, lv);
                                for (j = 0; j < m2 && !dup2; j++) {
                                    JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                                    const char *ts = JS_ToCString(ctx, t);
                                    if (ts) {
                                        if (k - b == strlen(ts) &&
                                            !strncasecmp(ts, dirs + b, k - b))
                                            dup2 = 1;
                                        JS_FreeCString(ctx, ts);
                                    }
                                    JS_FreeValue(ctx, t);
                                }
                                if (!dup2)
                                    JS_SetPropertyUint32(ctx, rb, m2,
                                        JS_NewStringLen(ctx, dirs + b, k - b));
                                if (dirs[k]) k++;
                            }
                            JS_FreeValue(ctx, rb);
                            free(dirs);
                        }
                    }
                }
                JS_FreeValue(ctx, rv);
            }
            /* nofollow -- from EITHER the header or the meta robots -- gates
               link following; the page itself still emits */
            {
                JSValue rb = JS_GetPropertyStr(ctx, page, "robots");
                if (JS_IsArray(ctx, rb)) {
                    JSValue lv = JS_GetPropertyStr(ctx, rb, "length");
                    uint32_t m2 = 0, j;
                    JS_ToUint32(ctx, &m2, lv);
                    JS_FreeValue(ctx, lv);
                    for (j = 0; j < m2 && !page_nofollow; j++) {
                        JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                        const char *ts = JS_ToCString(ctx, t);
                        if (ts) {
                            if (!strcasecmp(ts, "nofollow")) page_nofollow = 1;
                            JS_FreeCString(ctx, ts);
                        }
                        JS_FreeValue(ctx, t);
                    }
                }
                JS_FreeValue(ctx, rb);
            }
            /* rel=canonical dedup: a page whose canonical points at an
               ABSOLUTE http(s) url that is already handled is a DUPLICATE --
               emit it, but do not queue its links (its content was already
               crawled where it declares it lives). Malformed or relative
               canonicals are ignored, not guessed. */
            if (page_status >= 200 && page_status < 300 &&
                depth < c->max_depth && JS_IsObject(val) && !page_nofollow) {
                const char *ckey = url, *ccs = NULL, *canon_s = NULL;
                JSValue cval = JS_UNDEFINED, cval2 = JS_UNDEFINED;
                int dup = 0;
                if (c->canonical_field) {
                    cval = JS_GetPropertyStr(ctx, val, c->canonical_field);
                    if (JS_IsString(cval)) {
                        ccs = JS_ToCString(ctx, cval);
                        if (ccs &&
                            (!strncasecmp(ccs, "http://", 7) ||
                             !strncasecmp(ccs, "https://", 8)) &&
                            strcmp(ccs, url))
                            ckey = ccs;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                                  ? cr_seen(c, nk) : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                if (ckey == url) {
                    /* HTTP twin: Link: <...>; rel="canonical" (RFC 8288),
                       exposed by the Fetcher on the response */
                    cval2 = JS_GetPropertyStr(ctx, res, "canonicalUrl");
                    if (JS_IsString(cval2)) {
                        canon_s = JS_ToCString(ctx, cval2);
                        if (canon_s && strcmp(canon_s, url))
                            ckey = canon_s;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                                  ? cr_seen(c, nk) : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                /* <base href>: the page may declare its own resolution root
                   through a named extractor field. The declaration itself is
                   often RELATIVE ("/assets/"), so it resolves against the
                   page first; anything unusable falls back to the url. */
                if (!dup) {
                    char bbuf[1024];
                    const char *ebase = url;
                    JSValue bval = JS_UNDEFINED;
                    if (c->base_field) {
                        bval = JS_GetPropertyStr(ctx, val, c->base_field);
                        if (JS_IsString(bval)) {
                            const char *bsv = JS_ToCString(ctx, bval);
                            if (bsv) {
                                if (!strncasecmp(bsv, "http://", 7) ||
                                    !strncasecmp(bsv, "https://", 8))
                                    ebase = bsv;             /* absolute */
                                else if (fe_resolve(url, bsv,
                                                    bbuf, sizeof bbuf) > 0)
                                    ebase = bbuf;            /* made absolute */
                                JS_FreeCString(ctx, bsv);
                            }
                        }
                        JS_FreeValue(ctx, bval);
                    }
                    JSValue links = JS_GetPropertyStr(ctx, val, c->link_field);
                    /* relField: a parallel array of rel strings; a slot whose
                       text names nofollow politeness-skips THAT link */
                    JSValue rels = JS_UNDEFINED;
                    int have_rels = 0;
                    if (c->rel_field) {
                        rels = JS_GetPropertyStr(ctx, val, c->rel_field);
                        have_rels = JS_IsArray(ctx, rels);
                    }
                    if (JS_IsArray(ctx, links)) {
                        JSValue lv = JS_GetPropertyStr(ctx, links, "length");
                        uint32_t i, n = 0;
                        JS_ToUint32(ctx, &n, lv);
                        JS_FreeValue(ctx, lv);
                        for (i = 0; i < n; i++) {
                            JSValue e = JS_GetPropertyUint32(ctx, links, i);
                            const char *ls = JS_ToCString(ctx, e);
                            if (ls) {
                                char lh[300], resolved[1024];
                                const char *lp;
                                long rl;
                                if (have_rels) {
                                    JSValue rv = JS_GetPropertyUint32(ctx, rels, i);
                                    if (JS_IsString(rv)) {
                                        const char *rs = JS_ToCString(ctx, rv);
                                        if (rs && cr_is_nofollow(rs)) {
                                            JS_FreeCString(ctx, rs);
                                            JS_FreeValue(ctx, rv);
                                            JS_FreeCString(ctx, ls);
                                            JS_FreeValue(ctx, e);
                                            continue;
                                        }
                                        if (rs) JS_FreeCString(ctx, rs);
                                    }
                                    JS_FreeValue(ctx, rv);
                                }
                                /* Href shapes a crawler must not walk: empty,
                                   fragment-only, or non-navigational schemes. */
                                if (!ls[0] || ls[0] == '#' ||
                                    !strncasecmp(ls, "javascript:", 11) ||
                                    !strncasecmp(ls, "mailto:", 7) ||
                                    !strncasecmp(ls, "tel:", 4) ||
                                    !strncasecmp(ls, "data:", 5)) {
                                    JS_FreeCString(ctx, ls);
                                    JS_FreeValue(ctx, e);
                                    continue;
                                }
                                /* Relative hrefs resolve against the page's
                                   EFFECTIVE base (declared <base href> wins). */
                                if (fe_resolve(ebase, ls,
                                               resolved, sizeof resolved) > 0)
                                    rl = (long)strlen(resolved);
                                else
                                    rl = -1;
                                JS_FreeCString(ctx, ls);
                                if (rl <= 0 || fe_split(resolved, lh, sizeof lh, &lp) < 0 ||
                                    (strncmp(resolved, "http://", 7) &&
                                     strncmp(resolved, "https://", 8)))
                                    { JS_FreeValue(ctx, e); continue; }
                                if (c->same_host) {
                                    char nh[300];
                                    cr_norm_host(lh, resolved[4] == 's',
                                                 nh, sizeof nh);
                                    if (strcmp(nh, c->seed_host)) {
                                        JS_FreeValue(ctx, e);
                                        continue;   /* off-host, skipped */
                                    }
                                }
                                {
                                    char key[1024];
                                    if (cr_key(resolved, key, sizeof key) < 0) {
                                        JS_FreeValue(ctx, e);
                                        continue;
                                    }
                                    if (!cr_seen(c, key)) {
                                        int rc = cr_mark_seen(c, key);
                                        if (rc == 0) {
                                            rc = cr_push(c, resolved, depth + 1);
                                            if (rc > 0) c->dropped++;
                                        }
                                    }
                                }
                            }
                            JS_FreeValue(ctx, e);
                        }
                    }
                    JS_FreeValue(ctx, links);
                    JS_FreeValue(ctx, rels);
                }   /* end !dup (duplicate canonical pages contribute no links) */
                /* the canonical key's string/value are owned by the canonical
                   scope on BOTH the duplicate and the non-duplicate path */
                if (ccs) JS_FreeCString(ctx, ccs);
                JS_FreeValue(ctx, cval);
                if (canon_s) JS_FreeCString(ctx, canon_s);
                JS_FreeValue(ctx, cval2);
            }   /* end canonical/base/links scope */
            /* val is ours from GetPropertyStr on EVERY path past the define --
               including pages emitted at the depth cap, whose link scope the
               gate above skips (this exact placement once leaked it) */
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
