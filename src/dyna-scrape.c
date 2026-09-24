/*
 * dyna:scrape -- the policy layer over fetch + parse (design 28).
 *
 * Owns NO parsing: fetching is dyna:net, HTML is dyna:html, URLs are dyna:url.
 * What lives here is the policy nobody gets right -- robots.txt, per-host
 * pacing, retry bounds, and a schema the extraction is checked against.
 *
 * NOT an evasion toolkit: no fingerprint spoofing, no CAPTCHA handling, no
 * ban-circumvention proxying. See of the design; that is a decision.
 */
#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>   /* isnan/floor: cr_int_prop's exact-integer typing */
#include <time.h>   /* time(NULL): EPOCH wall clock, for HTTP-date deltas */
#include <poll.h>   /* poll(NULL,0,ms): the portable sleep */
#include "core/dyn-timer.h"   /* dyn_timer_now_ms: MONOTONIC, delay floors */
#include "core/dyn-prng.h"    /* dyn_splitmix64, dyn_os_entropy: backoff jitter */
#include "dyna-simd-kernels.h"   /* simd.find_u8 for the robots line scan */

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef CONFIG_TLS
#include "dyna-tls.h"
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ==================================================================== *
 *  strict options *
 * ==================================================================== */

/* Every options bag in this module is checked against its valid-key table
 * BEFORE any option is read: an unknown key throws a TypeError naming the key
 * AND the full valid set, instead of being silently ignored. Same shape as the
 * pilot dyn_opts_strict in dyna-file.c: a non-object bag (null / undefined /
 * a primitive) is accepted as absent; own ENUMERABLE STRING keys only (symbol
 * and inherited keys are invisible); a Proxy ownKeys trap that throws
 * propagates; an array bag's "0","1",... keys are unknown -> throws. */
static int dyn_opts_strict(JSContext *ctx, JSValueConst opts,
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
        if (!name) { /* OOM converting the key */
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            /* Size the valid list, then build it: no fixed buffer to
             * overflow, no silently truncated message. */
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
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

/* The valid-key tables, one per options bag. dyn_opts_strict rejects anything
 * not listed here, so each table is also that bag's option documentation. */
static const char *const sc_robots_keys[]  = { "agent" };
static const char *const sc_ex_ctor_keys[] = { "text" };
static const char *const sc_field_keys[]   = { "sel", "attr", "all", "required",
                                               "trim", "source", "default", "as" };
static const char *const sc_run_keys[]     = { "base" };
static const char *const sc_fetcher_keys[] = { "agent", "client", "headers",
                                               "robots", "minDelayMs", "retries",
                                               "maxRedirects", "maxBodyBytes",
                                               "allowPrivateHosts", "revalidate",
                                               "robotsTtlMs", "allowInsecureDowngrade",
                                               "proxy", "ca", "poolSize" };
static const char *const sc_crawl_keys[]   = { "maxPages", "maxDepth", "sameHost",
                                               "linkField", "baseField",
                                               "canonicalField", "relField",
                                               "robotsField", "concurrency" };

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

/* dyn_res_wrap dispose shim: the JS object's opaque is the DynResource box
 * (close()/closed/[Symbol.dispose] live on this class's proto), and the box
 * owns the rb_t. The raw-native opaque this class used before made close()
 * and `closed` type-pun the rb_t as a box -- "closed" read the rule-capacity
 * word and close() early-returned, never releasing anything. */
static void rb_res_dispose(void *p)
{
    rb_free((rb_t *)p);
}

static const JSClassDef dyn_rb_class = {
    "Robots", .finalizer = dyn_res_finalizer,
};

static int rb_reserved(unsigned char c);   /* defined below with the matcher */

/* RFC 9309: a rule's octets are matched AS WRITTEN -- a percent-
   encoded octet is LITERAL (%2A is a literal '*', %24 a literal '$', %2F
   never a separator; the wildcard is a RAW '*'). Only what would disagree
   with the normalized uri path is rewritten: raw non-ASCII, raw reserved
   octets other than the separator '/' and the wildcard '*', and a '%' that
   is not part of a valid escape. A '$' is kept raw only at the pattern END
   (the anchor); a mid-pattern '$' is a literal byte, so it is percent-
   encoded exactly like the uri side encodes it. */
static size_t rb_norm_rule(const char *s, size_t n, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    /* Encoding writes into `out`, NEVER over `s`: the old form rewrote the
       rule in place, and a 1->3 byte expansion overran the READ cursor --
       "/a?b" normalized to "/a%3F3" (the 'b' was eaten by the '3' of the
       escape), so every rule holding a raw reserved or non-ASCII octet
       matched nothing beyond its first corruption point. rb_norm_path has
       always used separate buffers; this now mirrors it. */
    while (i < n && o + 3 < cap) {
        const unsigned char c = (unsigned char)s[i];
        int enc;
        if (c == '$')
            enc = i + 1 != n;   /* only a FINAL '$' is the end anchor; a
                                   mid-pattern '$' is a literal byte, and the
                                   uri side percent-encodes a raw '$', so it
                                   must be encoded HERE too or the rule can
                                   never meet the path it names */
        else if (c != '/' && c != '*')
            enc = (rb_reserved(c) || c >= 0x80);
        else
            enc = 0;
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
                out[o++] = '%'; out[o++] = (char)a; out[o++] = (char)b;
                i += 3;
                continue;
            }
        }
        if (enc) {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
        i++;
    }
    out[o] = '\0';
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
        size_t e = i, ln, t;
        const char *k, *v;
        size_t kn, vn;
        /* Line end via the memchr-class kernel jump (deep-sweep D-5: 1.49x
           on the scan); SIZE_MAX means no newline -- take the rest. */
        t = simd.find_u8((const uint8_t *)s + e, '\n', n - e);
        e = (t == (size_t)-1) ? n : e + t;
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
                            /* RFC 9309: the value is a number. The shape is
                               validated BEFORE strtod: strtod also parses
                               "inf", "nan", "0x10" and "1e2", and a
                               Crawl-delay of Infinity used to park every
                               fetch at the 60s sleep cap while crawlDelay()
                               reported it back as a number. A malformed
                               directive is IGNORED (the group default of -1
                               stands) -- atof("2x") used to parse as 2. */
                            const char *q = v;
                            size_t digits = 0;
                            while (q < v + vn && *q >= '0' && *q <= '9')
                                { q++; digits++; }
                            if (q < v + vn && *q == '.') {
                                q++;
                                while (q < v + vn && *q >= '0' && *q <= '9')
                                    { q++; digits++; }
                            }
                            if (digits > 0 && q == v + vn) {
                                char buf[32];
                                size_t c = vn < sizeof(buf) - 1 ?
                                           vn : sizeof(buf) - 1;
                                char *end;
                                double d;
                                memcpy(buf, v, c); buf[c] = 0;
                                d = strtod(buf, &end);
                                if (end != buf && *end == '\0')
                                    g->delay = d;
                            }
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
                                /* rb_norm_rule can GROW a byte to three: the
                                   allocation holds the input at [0,pn) and
                                   its normalized form behind it, which is
                                   then compacted to the front (the rule owns
                                   ONE string that rb_free can free). */
                                copy = (char *)malloc(4 * pn + 1);
                                if (!copy) goto oom;
                                memcpy(copy, v, pn);
                                copy[pn] = 0;
                                {
                                    size_t rn = rb_norm_rule(copy, pn,
                                                             copy + pn,
                                                             3 * pn + 1);
                                    memmove(copy, copy + pn, rn);
                                    pn = rn;
                                }
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
    /* No named match anywhere -> the wildcard group, if one exists. The
       argmax above ALWAYS gives best_g a real index (an unmatched group
       outscores the -1 sentinel), so the search must RESET it first: with
       the stale index the scan broke out at group 0 and the '*' group was
       only ever found when it happened to BE group 0 -- a named group
       written before the wildcard (the common robots.txt layout) left
       unlisted agents pinned to that first group's rules instead of the
       RFC 9309 "no group applies" allow-all. */
    if (best_score <= 0) {
        best_g = (size_t)-1;
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
    JSValue av = JS_UNDEFINED;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Robots(text[, { agent }])");
    txt = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!txt)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        /* `txt` is already an OWNED C string here: the strict refusal must
         * release it, like every other early exit below. */
        if (dyn_opts_strict(ctx, argv[1], sc_robots_keys, 1)) {
            JS_FreeCString(ctx, txt);
            return JS_EXCEPTION;
        }
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

    /* Boxed like Fetcher/Crawl: the opaque is a DynResource box, so the
     * close()/closed surface matches reality, and new_target routes through
     * dyn_ctor_proto -- `class R extends Robots {}` produces R instances,
     * and a throwing "prototype" getter runs rb_res_dispose (dyn_res_wrap's
     * fail path releases exactly what was allocated). */
    return dyn_res_wrap(ctx, new_target, dyn_rb_class_id, r, rb_res_dispose);
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

/* The URI side of RFC 9309: raw reserved and non-ASCII octets are
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
    DynResource *box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t *r = box ? (rb_t *)box->native : NULL;
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
    DynResource *box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t *r = box ? (rb_t *)box->native : NULL;
    (void)argc; (void)argv;
    if (!r) return JS_EXCEPTION;
    return r->delay < 0 ? JS_NULL : JS_NewFloat64(ctx, r->delay);
}

static JSValue dyn_rb_sitemaps(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    DynResource *box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t *r = box ? (rb_t *)box->native : NULL;
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
    DynResource *box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t *r = box ? (rb_t *)box->native : NULL;
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
    JSValue obj, proto, ht = JS_UNDEFINED;

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
        /*the field spec bag is strict -- a typo'd key used to be a
         * silently absent constraint. */
        if (dyn_opts_strict(ctx, fv, sc_field_keys, 8)) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
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
        /* refusal AFTER the fields are built: `e` already owns the
         * Selector refs, so this exit must run the full dispose (this is the
         * first throwing exit past field parsing -- the ones below it
         * already route through ex_free_ctx). */
        if (dyn_opts_strict(ctx, argv[1], sc_ex_ctor_keys, 1)) {
            ex_free_ctx(ctx, e);
            return JS_EXCEPTION;
        }
        ht = JS_GetPropertyStr(ctx, argv[1], "text");
        if (JS_IsFunction(ctx, ht)) e->html_text = ht;
        else JS_FreeValue(ctx, ht);
    }

    /* OrdinaryCreateFromConstructor via dyn_ctor_proto: a subclass's
     * prototype wins, a throwing "prototype" getter propagates;
     * ex_free_ctx is the complete dispose either way. */
    proto = dyn_ctor_proto(ctx, new_target, dyn_ex_class_id);
    if (JS_IsException(proto)) { ex_free_ctx(ctx, e); return proto; }
    obj = JS_NewObjectProtoClass(ctx, proto, (int)dyn_ex_class_id);
    JS_FreeValue(ctx, proto);
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
        if (dyn_opts_strict(ctx, argv[1], sc_run_keys, 1))
            return JS_EXCEPTION;
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
 * `agent` is REQUIRED. No default agent, because a shared one is
 * indistinguishable from anonymous and denies the operator the one thing
 * they need -- who to contact.
 *
 * The client is OPTIONAL: omitted, the module constructs its OWN
 * HTTPClient for you (see fe_auto_client) -- a one-page fetch no longer
 * needs an import of dyna:net and a hand-built client first. Passing
 * `client` keeps the injected form: it is what lets a test drive the whole
 * policy against a mock. The simple form deliberately offers nothing the
 * engine's client cannot honor (no proxy, no custom CA, no pool knobs):
 * dyna:net's HTTPClient opens a fresh connection per request and uses the
 * platform trust store, and a "supported" option that changed nothing would
 * be exactly the silent no-op exists to kill. Need proxy/CA? Build a
 * client-shaped object yourself and inject it.
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
    /* simple-options: validated, stored and observable (see stats).
     * proxy/ca name a transport the injected client may honor; poolSize
     * sizes a client-side pool where the transport has one. The built-in
     * HTTPClient opens a fresh connection per request against the platform
     * trust store, so these do not change its wire behavior -- they are
     * recorded, validated and exposed rather than silently ignored. */
    char      *proxy;
    char      *ca;
    int        pool_size;
    uint64_t   rng;          /* backoff jitter; seeded once in the ctor */
    fe_host_t *hosts;
    size_t     n_hosts, cap_hosts;
    double     fetched, skipped_robots, retried, throttled_ms, bytes;
    double     revalidated, saved_bytes;
    fe_cache_ent_t cache[FE_CACHE_MAX_ENTRIES];
    size_t     n_cache;
#ifdef CONFIG_TLS
    void      *tls_ctx;        /*the streaming path's client context
                                (lazily built on first https use, freed in
                                fe_dispose; a plain C pointer, no JSValue) */
#endif
} fe_t;

static JSClassID dyn_fe_class_id;

/* The liveness check used after ANY call that can run user JS: the caller's
 * close() may have run in there, and a CLOSED fetcher's memory is gone
 * (fe_dispose ran) -- reading through a stale fe_t is the resource-lifetime
 * class this module front-runs. NULL means "closed or gone"; NO exception
 * is thrown: the caller decides what a fetcher dying mid-operation means (a
 * flight fails cleanly; a response tail skips its bookkeeping writes). */
static fe_t *fe_live(JSContext *ctx, JSValueConst fv)
{
    JSClassID id = (JSClassID)0;
    DynResource *r = (DynResource *)JS_GetAnyOpaque(fv, &id);
    (void)ctx;
    if (!r || id != dyn_fe_class_id || r->closed)
        return NULL;
    return (fe_t *)r->native;
}

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
    free(f->proxy);
    free(f->ca);
#ifdef CONFIG_TLS
    dyn_tls_ctx_free((dyn_tls_ctx_t *)f->tls_ctx);   /* NULL-safe */
#endif
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

/* Strict int option read (audit X-12): stores the value in *out and returns
   0, or returns -1 with the exception pending. The option object is caller
   JS, so a throwing getter must PROPAGATE -- the unchecked JS_ToInt32 used
   to swallow it and silently build with the default. */
static int fe_num_prop(JSContext *ctx, JSValueConst o, const char *k, int dflt,
                       int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t r = dflt;
    int rc = 0;
    if (JS_IsException(v))
        rc = -1;
    else if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &r, v))
        rc = -1;
    JS_FreeValue(ctx, v);
    *out = (int)r;
    return rc;
}

/* The strict-integer reader for options documented as an INTEGER. "Integer"
 * is a type, not a coercion hint: the JS_ToInt32 form silently truncated
 * poolSize 1.5 -> 1 and coerced "5" -> 5. A string, a fraction, NaN or
 * Infinity is a TypeError naming the option; a genuine integer outside the
 * documented range is left to the caller's RangeError.
 *
 * The sibling below (cr_int_prop) enforces the SAME typing for Crawl's
 * options and owns its RangeError as well: an integer-valued number outside
 * [lo, hi] -- however large -- is a RangeError, never a TypeError and never
 * a truncation. */
static int fe_strict_int_prop(JSContext *ctx, JSValueConst o, const char *k,
                              int dflt, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d = 0;
    int ok;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = dflt;
        return 0;
    }
    ok = JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0;
    JS_FreeValue(ctx, v);
    if (!ok || !(d >= -2147483648.0 && d <= 2147483647.0) ||
        d != (double)(int)d) {
        JS_ThrowTypeError(ctx, "Fetcher: %s must be an integer", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

/* Crawl's strict-integer option read: exact integer typing, two refusal
 * classes. A non-number ("3", true, a bigint) or a number that is not an
 * integer (2.5, NaN, Infinity) is a TypeError naming the option; a genuine
 * integer outside [lo, hi] -- 3e9 included, no int32 clipping -- is a
 * RangeError. Undefined and null mean "absent" and yield the default, like
 * every other option read here. Returns 0 or -1 (exception pending). */
static int cr_int_prop(JSContext *ctx, JSValueConst o, const char *k,
                       int dflt, int lo, int hi, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d = 0;
    int is_int;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = dflt;
        return 0;
    }
    is_int = JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) &&
             !isnan(d) && d != HUGE_VAL && d != -HUGE_VAL &&
             d == floor(d);
    JS_FreeValue(ctx, v);
    if (!is_int) {
        JS_ThrowTypeError(ctx, "Crawl: %s must be an integer", k);
        return -1;
    }
    if (d < (double)lo || d > (double)hi) {
        JS_ThrowRangeError(ctx,
            "Crawl: %s must be between %d and %d (got %.17g) -- the "
            "polite part is per host, but %g parallel fetches is a stampede",
            k, lo, hi, d, d);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static int fe_status(JSContext *ctx, JSValueConst res)
{
    JSValue v = JS_GetPropertyStr(ctx, res, "status");
    int32_t st = 0;
    int rc;
    /* The Response belongs to the user's fetcher/client: a `status` getter
       that throws must reach the caller (audit X-12). The unchecked
       JS_ToInt32 used to swallow it and the crawl proceeded at status 0. */
    rc = JS_ToInt32(ctx, &st, v);
    JS_FreeValue(ctx, v);
    return rc ? -1 : (int)st;   /* -1: exception pending, caller propagates */
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
   being re-fetched for every URL on the host. Returns 0, or -1 with the
   exception pending when the USER's response getter threw -- a JS error is
   not a robots outcome and propagates instead of being read as policy. */
static int fe_load_robots(JSContext *ctx, fe_t *f, JSValueConst fv,
                          JSValueConst client,
                          JSValueConst uh, fe_host_t *h, int https)
{
    char url[600];
    char host0[300];
    JSValue res;
    int st = 0, hop;
    /* the policy captured BEFORE the user's request can run; and the host
       NAME (not the slot -- the host table belongs to the fetcher and dies
       with it) so the slot can be re-found after any user-JS boundary */
    const double robots_ttl = f->robots_ttl_ms;
    snprintf(host0, sizeof host0, "%s", h->host);
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
        /* the request ran the user's client: the fetcher (and its host
           table) may have died in it. A dead fetcher records NOTHING -- the
           caller's own re-resolve names the failure. */
        f = fe_live(ctx, fv);
        h = f ? fe_host(f, host0) : NULL;
        if (!h) {
            JS_FreeValue(ctx, res);
            return 0;
        }
        if (JS_IsException(res)) {
            /* Network error: the file is UNDEFINED. RFC 9309 2.3.1.4 says
               that means "MUST assume complete disallow" -- the old
               default-allow here was a spec violation and an SSRF
               widening. The failure is TTL-stamped like every other
               outcome: leaving robots_next_ms unstamped re-attempted the
               fetch for EVERY page (one dead canonical-host redirect
               turned each page fetch into a fresh robots fetch). */
            JS_FreeValue(ctx, JS_GetException(ctx));
            h->robots_unreachable = 1;
            h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
            return 0;
        }
        st = fe_status(ctx, res);
        if (st < 0) {           /* the user's status getter threw: propagate
                                   (audit X-12); nothing was stamped, so a
                                   later fetch may try robots again */
            JS_FreeValue(ctx, res);
            return -1;
        }
        /* the status getter is user JS */
        f = fe_live(ctx, fv);
        h = f ? fe_host(f, host0) : NULL;
        if (!h) {
            JS_FreeValue(ctx, res);
            return 0;
        }
        if (st >= 300 && st < 400 && hop < 2) {
            JSValue hold = JS_UNDEFINED;
            char nxt[600], host2[300];
            const char *pp, *loc = fe_header(ctx, res, "location", &hold);
            int ok = 0;
            /* the Location read is user JS */
            f = fe_live(ctx, fv);
            h = f ? fe_host(f, host0) : NULL;
            if (!h) {
                if (loc) JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                return 0;
            }
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
            /* unresolvable / unsafe hop: fail closed, and stamp the TTL so
               the failure is remembered instead of retried per fetch */
            h->robots_unreachable = 1;
            h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
            JS_FreeValue(ctx, res);
            return 0;
        }
        break;
    }

    if (st == 200) {
        JSValue b = JS_GetPropertyStr(ctx, res, "body");
        size_t bn = 0;
        const char *txt = JS_ToCStringLen(ctx, &bn, b);
        if (txt) {
            rb_t *r;
            /* the body getter and its toString are user JS */
            f = fe_live(ctx, fv);
            h = f ? fe_host(f, host0) : NULL;
            if (!h) {
                JS_FreeCString(ctx, txt);
                JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, res);
                return 0;
            }
            r = (rb_t *)calloc(1, sizeof(rb_t));
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
    f = fe_live(ctx, fv);
    h = f ? fe_host(f, host0) : NULL;
    if (h)
        h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
    JS_FreeValue(ctx, res);
    return 0;
}

/* The post-exchange policy tail, SHARED by the buffered get and the
 * async engine: body cap (declared + received), 304 synthesis from the
 * validator store, X-Robots-Tag + Link-canonical decoration, and the
 * contentType/url/fromCache response flags. Consumes res; returns the FINAL
 * response (possibly the synthesized 304), or JS_EXCEPTION (the cap throw,
 * or a user getter's throw -- both propagate).
 *
 * Lifetime (the shared-tail form of the resource-lifetime class): EVERY
 * field of `res` is a user getter, so the caller may close() the fetcher
 * while this function runs. `f` is the caller's entry snapshot (for the
 * policy captured up front); before every WRITE into the fetcher the live
 * handle is re-resolved through `fetcher`, and a dead fetcher's tail simply
 * records nothing -- an operation that started on a live fetcher RUNS TO
 * COMPLETION, and "closed" refuses NEW work, never punishes in-flight
 * work with a spurious error. */
static JSValue fe_finish_response(JSContext *ctx, JSValueConst fetcher,
                                  fe_t *f, const char *cur,
                                  JSValue res, int st)
{
    /* the policy the operation started under: captured BEFORE the first
       getter can run */
    const double max_body = f->max_body;
    const int revalidate = f->revalidate;

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
                if (decl > max_body) {
                    JS_FreeCString(ctx, clv);
                    JS_FreeValue(ctx, hold_cl);
                    JS_FreeValue(ctx, res);
                    JS_ThrowRangeError(ctx,
                        "Fetcher: declared Content-Length %.0f exceeds "
                        "maxBodyBytes", decl);
                    return JS_EXCEPTION;
                }
                JS_FreeCString(ctx, clv);
            }
            JS_FreeValue(ctx, hold_cl);
        }

        b = JS_GetPropertyStr(ctx, res, "body");
        bs = JS_ToCStringLen(ctx, &bn, b);
        if (!bs) {
            JS_FreeValue(ctx, b); JS_FreeValue(ctx, res);
            return JS_EXCEPTION;             /* conversion error pending */
        }
        if ((double)bn > max_body) {
            JS_FreeCString(ctx, bs); JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, res);
            JS_ThrowRangeError(ctx,
                "Fetcher: body of %zu bytes exceeds maxBodyBytes", bn);
            goto out;
        }
        if (st == 304 && revalidate) {
            /* Not modified: answer from the metadata store, honestly
               labelled -- status stays what the wire said. */
            fe_cache_ent_t *ce;
            f = fe_live(ctx, fetcher);
            ce = f ? fe_cache_find(f, cur) : NULL;
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
                return res;
            }
            /* 304 with nothing stored: hand through untouched below */
        } else if (st >= 200 && st < 300) {
            const char *etg, *lmd, *ctv;
            JSValue he, hl, hc;
            etg = fe_header(ctx, res, "etag", &he);
            lmd = fe_header(ctx, res, "last-modified", &hl);
            ctv = fe_header(ctx, res, "content-type", &hc);
            if ((etg || lmd) && revalidate) {
                JSValue hcc = JS_UNDEFINED;
                const char *cc = fe_header(ctx, res, "cache-control",
                                           &hcc);
                /* the validator and cache-control getters may have closed
                   the fetcher: a dead fetcher keeps no cache */
                f = fe_live(ctx, fetcher);
                if (f && !fe_cc_no_store(cc))
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
        f = fe_live(ctx, fetcher);
        if (f)
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
            char *dirs;
            /* the headers getter may have closed the fetcher: the agent
               name that filters the groups lives in it, so a dead
               fetcher's response simply carries no parsed directives */
            f = fe_live(ctx, fetcher);
            dirs = f ? fe_parse_robots_tag(xrt, f->agent) : NULL;
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
    f = fe_live(ctx, fetcher);
    if (f)
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
    return res;
out:
    return JS_EXCEPTION;
}

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
                { if (fe_load_robots(ctx, f, this_val, client, uh, h, https) < 0) goto out; }
                /* the user fetcher's own getter threw: not a robots
                   outcome -- fail the fetch, propagate (audit X-12) */
            /* the load ran the user's client: `f` and its host table live
               only while the fetcher does */
            f = fe_live(ctx, this_val);
            if (!f) {
                JS_ThrowTypeError(ctx,
                    "Fetcher: closed while get() was in flight");
                goto out;
            }
            h = fe_host(f, host);
            if (!h) { JS_ThrowOutOfMemory(ctx); goto out; }
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
            /* the request ran the user's client: the fetcher (and its host
               table) may have died in it */
            f = fe_live(ctx, this_val);
            if (!f) {
                JS_FreeValue(ctx, res);
                JS_ThrowTypeError(ctx,
                    "Fetcher: closed while get() was in flight");
                goto out;
            }
            h = fe_host(f, host);
            if (!h) {
                JS_FreeValue(ctx, res);
                JS_ThrowOutOfMemory(ctx);
                goto out;
            }
            h->next_ok_ms = (double)dyn_timer_now_ms() + floor_ms;
            if (JS_IsException(res)) {
                if (attempt >= f->retries) goto out;
                JS_FreeValue(ctx, JS_GetException(ctx));
                st = 0;
            } else {
                st = fe_status(ctx, res);
                if (st < 0) {   /* the Response's status getter threw:
                                   propagate, no retry (audit X-12) */
                    JS_FreeValue(ctx, res);
                    goto out;
                }
                /* the status getter is user JS */
                f = fe_live(ctx, this_val);
                if (!f) {
                    JS_FreeValue(ctx, res);
                    JS_ThrowTypeError(ctx,
                        "Fetcher: closed while get() was in flight");
                    goto out;
                }
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
                /* the Retry-After read touched user getters */
                f = fe_live(ctx, this_val);
                if (!f) {
                    JS_FreeValue(ctx, hold);
                    JS_FreeValue(ctx, res);
                    JS_ThrowTypeError(ctx,
                        "Fetcher: closed while get() was in flight");
                    goto out;
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
            /* the Location read touched user getters */
            f = fe_live(ctx, this_val);
            if (!f) {
                if (loc) JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                JS_ThrowTypeError(ctx,
                    "Fetcher: closed while get() was in flight");
                goto out;
            }
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

        /* ---- the shared post-exchange policy tail: body cap, 304
           synthesis, wire robots directives, canonical Link, the response
           flags. fe_finish_response consumes res and returns the final
           response (or throws) -- byte-for-byte the tail that always lived
 here, now shared with the async engine. ---- */
        res = fe_finish_response(ctx, this_val, f, cur, res, st);
        if (JS_IsException(res)) goto out;
        ret = res;
        goto out;
    }
out:
    JS_FreeValue(ctx, client);
    JS_FreeValue(ctx, uh);
    return ret;
}

static JSValue fe_promise_resolved(JSContext *ctx, JSValue val);
static JSValue fe_promise_rejected(JSContext *ctx, JSValue exc);

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
    /* simple-options, observable (never silently ignored). */
    JS_DefinePropertyValueStr(ctx, o, "proxy",
        f->proxy ? JS_NewString(ctx, f->proxy) : JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ca",
        f->ca ? JS_NewString(ctx, f->ca) : JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "poolSize",
        JS_NewInt32(ctx, f->pool_size ? f->pool_size : 4), JS_PROP_C_W_E);
    return o;
}

/*async get(url) helper -- the same policy as get (robots gate,
 * delay floor, retries, redirects, caps) settled as a Promise, so a
 * one-page fetch does not need a Crawl and does not block on a second
 * import. Implemented as an inline-settled promise over the sync path
 * (the transport itself is sync); a throw becomes a rejection, never a
 * sync throw -- `await f.getAsync(u)` either resolves the FetcherResponse
 * or rejects with the fetch error. */
static JSValue dyn_fe_get_async(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    JSValue res;
    if (argc < 1)
        return fe_promise_rejected(ctx, JS_ThrowTypeError(ctx, "getAsync(url)"));
    res = dyn_fe_get(ctx, this_val, argc, argv);
    if (JS_IsException(res))
        return fe_promise_rejected(ctx, JS_EXCEPTION);
    return fe_promise_resolved(ctx, res);
}

/* ---- the one growable byte buffer this module needs (serialize's state
   envelope, the stream request builder) ------------------------------- */

/* Growable byte buffer for the serializer. Grown geometrically; every append
 * checks, and a failed growth is remembered so the caller answers OOM once. */
typedef struct { char *p; size_t n, cap; int oom; } sc_sb_t;

static void sc_sb_init(sc_sb_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }

static void sc_sb_free(sc_sb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void sc_sb_put(sc_sb_t *b, const char *s, size_t n)
{
    if (b->oom || n == 0) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char *np;
        while (nc < b->n + n) nc *= 2;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void sc_sb_puts(sc_sb_t *b, const char *s) { sc_sb_put(b, s, strlen(s)); }

/* JSON string body escaping (the quotes around it are the caller's): the two
   mandatory escapes, the C0 range as \u00XX. A url cannot carry controls (the
   split refuses them) but the field names and seed host are also encoded, and
   refusing to emit one byte of invalid JSON is cheaper than proving none can. */
static void sc_sb_json_str(sc_sb_t *b, const char *s)
{
    size_t i;
    sc_sb_put(b, "\"", 1);
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            sc_sb_put(b, e, 2);
        } else if (c < 0x20) {
            char e[7];
            snprintf(e, sizeof e, "\\u%04x", c);
            sc_sb_puts(b, e);
        } else {
            sc_sb_put(b, (const char *)&s[i], 1);
        }
    }
    sc_sb_put(b, "\"", 1);
}

static void cr_serialize_u64(sc_sb_t *b, const char *key, uint64_t v)
{
    char num[24];
    snprintf(num, sizeof num, ",\"%s\":%llu", key, (unsigned long long)v);
    sc_sb_puts(b, num);
}

static void cr_serialize_str(sc_sb_t *b, const char *key, const char *s)
{
    sc_sb_puts(b, ",\"");
    sc_sb_puts(b, key);
    sc_sb_puts(b, "\":");
    sc_sb_json_str(b, s ? s : "");
}

/* ==== + CC-2-scrape: getStream -- the streaming body path ============
 *
 * get() returns the whole body as a string: the injected client's contract,
 * and the right shape for pages. For big or open-ended bodies (an NDJSON
 * feed, a log endpoint, a large file) the caller wants the bytes to ARRIVE
 * as they are consumed. getStream(url) opens the fetch, applies the SAME
 * policy -- robots gate, per-host delay floor, retries with Retry-After and
 * jittered backoff, redirect chase with the private-host and https-downgrade
 * gates, maxBodyBytes, the SSRF name gate, this fetcher's agent and headers
 * -- and instead of buffering the body, hands the caller a live stream:
 *
 *   { status, statusText, ok, headers, url, contentType,
 *     read(buf) -> Promise<number>,      // 0 = end of body
 *     close(), dispose(), [Symbol.dispose], closed }
 *
 * `read`/`close` are exactly dyna:stream's ByteSource shape (duck-typed:
 * dyna:stream is NOT imported and needs no import -- pipe, lines, ndjson and
 * inflate consume any object with a callable read), so
 * `for await (const l of stream.lines(r.stream, ...))` -- sorry, plain JS:
 * `stream.lines(resp)` -- just works. The object is also the RESPONSE: the
 * status and headers are readable off the same object you read bytes from.
 *
 * Transport, stated honestly: getStream does NOT go through the injected
 * client. `request(method, url, body, headers)` buffers the whole body by
 * contract, so a streaming path through it could only fake the stream. This
 * path owns its sockets (plain http and, under CONFIG_TLS, https with the
 * platform trust store, exactly the engine client's defaults) and always
 * sends `Accept-Encoding: identity` -- a compressed body would need a
 * streaming inflater between the socket and the caller, which is dyna:stream
 * inflate()'s job when one is wanted. Fixed 15s connect/recv timeouts (the
 * HTTPClient default); the injected client's setTimeout cannot apply here.
 * NOT carried over from get(), on purpose: revalidate/conditional-GET (the
 * validator store replay answers from a buffered copy) and the response
 * decorations (X-Robots-Tag, Link canonical) -- those are page-crawl
 * features; a stream is consumed, not parsed for directives.
 *
 * Bounds and failures: a DECLARED Content-Length over maxBodyBytes refuses
 * before the body starts (get()'s early refusal, byte for byte). Bytes
 * received are counted as they are handed out: crossing maxBodyBytes rejects
 * the CURRENT read with the same RangeError get() throws and closes the
 * stream. A connection that ends before a declared Content-Length is a
 * truncated body: the read rejects ("truncated"), never a silent 0. Errors
 * surfacing from read() leave the stream closed -- retry by fetching again.
 */

#define FE_STREAM_TIMEOUT_MS 15000      /* the HTTPClient default */
#define FE_STREAM_HDR_MAX    (32 * 1024)
#define FE_STREAM_CHUNK_HDR   64        /* chunk-size line, "0\r\n" incl */

/* ---- one connection: socket + optional TLS ---- */

typedef struct {
    int fd;
#ifdef CONFIG_TLS
    dyn_tls_conn_t *tls;
#endif
} fe_conn_t;

static void fe_conn_close(fe_conn_t *c)
{
    if (!c) return;
#ifdef CONFIG_TLS
    dyn_tls_conn_free(c->tls);          /* NULL-safe */
    c->tls = NULL;
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* recv with the same TLS pump as the engine client (hc_recv's shape): pull
   plaintext from the engine first; only when it wants more ciphertext, read
   the socket and feed it. Plain sockets: recv + EINTR. */
static ssize_t fe_conn_recv(fe_conn_t *c, void *p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        for (;;) {
            uint8_t cipher[16384];
            ssize_t r;
            int got = dyn_tls_read(c->tls, (uint8_t *)p, n);
            if (got > 0) return got;
            if (got < 0) return -1;
            r = recv(c->fd, cipher, sizeof cipher, 0);
            if (r <= 0) return r;              /* 0 = clean close */
            if (dyn_tls_feed(c->tls, cipher, (size_t)r) != 0) return -1;
        }
    }
#endif
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

static ssize_t fe_conn_send_all(fe_conn_t *c, const void *p, size_t n)
{
    const char *q = (const char *)p;
    size_t off = 0;
    while (off < n) {
        ssize_t s;
#ifdef CONFIG_TLS
        if (c->tls) {
            int w = dyn_tls_write(c->tls, (const uint8_t *)q + off, n - off);
            if (w < 0) return -1;
            s = w;
        } else
#endif
        {
            s = send(c->fd, q + off, n - off, 0);
            if (s < 0 && errno == EINTR) continue;
            if (s <= 0) return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)n;
}

/* Connect to host:port with a hard timeout: non-blocking connect + poll, so
   a black-holed address answers in FE_STREAM_TIMEOUT_MS, not in the kernel's
   connect default. The resolved address is connected in getaddrinfo ORDER;
   the SSRF gate is NAME-based exactly like the buffered path (documented
   boundary: resolution belongs to the transport -- see T8). */
static int fe_tcp_connect(const char *host, uint16_t port, char *why,
                          size_t whyn)
{
    char service[8];
    struct addrinfo hints, *res = NULL, *rp;
    int fd = -1, rc;

    snprintf(service, sizeof service, "%u", (unsigned)port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        snprintf(why, whyn, "cannot resolve %.100s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (rp = res; rp && fd < 0; rp = rp->ai_next) {
        int fl;
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd); fd = -1; continue;
        }
        if (errno != EINPROGRESS) {
            errno = 0;
        } else {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int pr = poll(&pfd, 1, FE_STREAM_TIMEOUT_MS);
            if (pr <= 0) {
                snprintf(why, whyn, "connect to %.100s timed out", host);
                close(fd); fd = -1; continue;
            }
            {
                int err = 0;
                socklen_t el = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
                    snprintf(why, whyn, "connect to %.100s failed", host);
                    close(fd); fd = -1; continue;
                }
            }
        }
        /* connected: back to blocking + socket timeouts, like hc_exchange */
        if (fl >= 0) fcntl(fd, F_SETFL, fl);
        {
            struct timeval tv;
            tv.tv_sec = FE_STREAM_TIMEOUT_MS / 1000;
            tv.tv_usec = (FE_STREAM_TIMEOUT_MS % 1000) * 1000;
            /* a refused timeout knob costs the deadline, not the fetch */
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        {
            /* the ping-pong argument (Nagle + delayed ACK can stall a small
               request ~40ms); best-effort, exactly like the engine client */
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        }
    }
    freeaddrinfo(res);
    if (fd < 0 && why[0] == '\0')
        snprintf(why, whyn, "cannot connect to %.100s", host);
    return fd;
}

#ifdef CONFIG_TLS
/* The engine client's TLS is built lazily and lives on the CLIENT; the
   streaming path builds its own context on the FETCHER (same defaults: TLS
   1.2+, platform trust store) and reuses it across fetches. */
static int fe_tls_connect(fe_t *f, fe_conn_t *c, const char *host,
                          char *why, size_t whyn)
{
    if (!f->tls_ctx) {
        dyn_tls_opts_t o;
        char terr[192];
        memset(&o, 0, sizeof o);
        o.min_version = 12;
        f->tls_ctx = dyn_tls_ctx_client(&o, terr, sizeof terr);
        if (!f->tls_ctx) {
            snprintf(why, whyn, "TLS context: %s", terr);
            return -1;
        }
    }
    {
        char terr[192];
        c->tls = dyn_tls_conn_new((dyn_tls_ctx_t *)f->tls_ctx, host,
                                  terr, sizeof terr);
        if (!c->tls) {
            snprintf(why, whyn, "TLS handshake setup: %s", terr);
            return -1;
        }
    }
    /* the handshake pump: drain the engine's output, feed its input */
    for (;;) {
        uint8_t buf[16384];
        int st = dyn_tls_handshake(c->tls);
        ssize_t r;
        if (st < 0) {
            snprintf(why, whyn, "TLS handshake: %s",
                     dyn_tls_error(c->tls) ? dyn_tls_error(c->tls) : "failed");
            return -1;
        }
        if (st == 1) return 0;
        r = recv(c->fd, buf, sizeof buf, 0);
        if (r <= 0) {
            snprintf(why, whyn, "TLS handshake: connection closed");
            return -1;
        }
        if (dyn_tls_feed(c->tls, buf, (size_t)r) != 0) {
            snprintf(why, whyn, "TLS handshake: protocol error");
            return -1;
        }
    }
}
#endif

/* ---- the response head ---- */

typedef struct {
    int   status;
    char  status_text[64];
    char *location;        /* Location, or NULL */
    char *content_type;    /* Content-Type, or NULL */
    char *retry_after;     /* Retry-After, or NULL */
    long long clen;        /* Content-Length; -1 = none */
    int   chunked;         /* Transfer-Encoding: chunked */
} fe_head_t;

static void fe_head_free(fe_head_t *h)
{
    free(h->location); free(h->content_type); free(h->retry_after);
    /* no memset: the caller frees the struct itself, so zeroing it here is a
       dead store the optimizer may delete anyway (a would-be guard that
       guards nothing). */
}

/* A header line "Name: value" against the head. CR/LF/NUL cannot appear:
   the line splitter cut on them. */
static void fe_head_line(fe_head_t *h, char *line, size_t n)
{
    char *colon = (char *)memchr(line, ':', n);
    char *v;
    size_t vn;
    if (!colon) return;
    *colon = 0;
    v = colon + 1;
    vn = n - (size_t)(v - line);
    while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
    while (vn && (v[vn-1] == ' ' || v[vn-1] == '\t')) vn--;
    if (!strcasecmp(line, "location"))
        { free(h->location); h->location = strndup(v, vn); }
    else if (!strcasecmp(line, "content-type"))
        { free(h->content_type); h->content_type = strndup(v, vn); }
    else if (!strcasecmp(line, "retry-after"))
        { free(h->retry_after); h->retry_after = strndup(v, vn); }
    else if (!strcasecmp(line, "content-length"))
        h->clen = strtoll(v, NULL, 10);
    else if (!strcasecmp(line, "transfer-encoding")) {
        /* case-insensitive token scan: a value of "identity, chunked" or
           "Chunked" both decide the framing */
        size_t i = 0;
        while (i + 7 <= vn) {
            if (!strncasecmp(v + i, "chunked", 7)) {
                char a = (i > 0 && v[i-1] != ' ' && v[i-1] != '\t' &&
                          v[i-1] != ',') ? 'x' : ' ';
                char b = (i + 7 < vn && v[i+7] != ' ' && v[i+7] != '\t' &&
                          v[i+7] != ',') ? 'x' : ' ';
                if (a == ' ' && b == ' ') { h->chunked = 1; break; }
            }
            i++;
        }
    }
}

/* Read the status line + headers off the connection. Returns 0, or -1 with
   `why` set (too large, malformed, connection died). The head is bounded:
   FE_STREAM_HDR_MAX of header block, and a status line that is not
   "HTTP/x.y SP DDD" is a protocol failure, not a status of 0 -- the same
   decision dyn_build_response makes for the buffered client. */
static int fe_read_head(fe_conn_t *c, fe_head_t *h, char *why, size_t whyn)
{
    char *buf = (char *)malloc(FE_STREAM_HDR_MAX + 1);
    size_t n = 0;
    if (!buf) { snprintf(why, whyn, "out of memory"); return -1; }
    memset(h, 0, sizeof *h);
    h->clen = -1;
    while (n < FE_STREAM_HDR_MAX) {
        ssize_t r = fe_conn_recv(c, buf + n, 1);
        if (r < 0) { snprintf(why, whyn, "receiving response head failed"); goto bad; }
        if (r == 0) { snprintf(why, whyn, "connection closed before response head"); goto bad; }
        n++;
        /* the terminator \r\n\r\n, found as it completes */
        if (n >= 4 && buf[n-1] == '\n' && buf[n-2] == '\r' &&
            buf[n-3] == '\n' && buf[n-4] == '\r')
            break;
    }
    if (n >= FE_STREAM_HDR_MAX) {
        snprintf(why, whyn, "response head exceeds %d bytes", FE_STREAM_HDR_MAX);
        goto bad;
    }
    buf[n] = 0;
    {
        /* status line */
        char *eol = (char *)memchr(buf, '\n', n);
        size_t l1 = eol ? (size_t)(eol - buf) : n;
        if (l1 < 12 || memcmp(buf, "HTTP/", 5) != 0) {
            snprintf(why, whyn, "not an HTTP/1.x response");
            goto bad;
        }
        {
            const char *p = buf + 5, *sp;
            if (*p >= '0' && *p <= '9') {
                p++;
                if (*p == '.' && p[1] >= '0' && p[1] <= '9') p += 2;
            }
            if (*p != ' ' || !(p - buf >= 8)) {
                snprintf(why, whyn, "malformed status line");
                goto bad;
            }
            sp = p + 1;
            if (!(sp[0] >= '0' && sp[0] <= '9' &&
                  sp[1] >= '0' && sp[1] <= '9' &&
                  sp[2] >= '0' && sp[2] <= '9')) {
                snprintf(why, whyn, "malformed status code");
                goto bad;
            }
            h->status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
            {
                const char *rs = sp + 3;
                size_t rn = 0;
                if (rs < buf + l1 && *rs == ' ') rs++;
                rn = (size_t)(buf + l1 - rs);
                while (rn && (rs[rn-1] == '\r')) rn--;
                if (rn >= sizeof h->status_text) rn = sizeof h->status_text - 1;
                memcpy(h->status_text, rs, rn);
                h->status_text[rn] = 0;
            }
        }
        /* header lines, split on \n, trimmed \r */
        {
            char *p = eol + 1;
            while (p < buf + n) {
                char *ne = (char *)memchr(p, '\n', (size_t)(buf + n - p));
                size_t ll = ne ? (size_t)(ne - p) : (size_t)(buf + n - p);
                while (ll && (p[ll-1] == '\r' || p[ll-1] == '\n')) ll--;
                p[ll] = '\0';            /* within our own buffer: safe */
                if (ll) fe_head_line(h, p, ll);
                if (!ne) break;
                p = ne + 1;
            }
        }
    }
    free(buf);
    return 0;
bad:
    free(buf);
    return -1;
}

/* ---- the stream resource -------------------------------------------------
 *
 * NO JSValue here: status/headers/url live as properties on the JS wrapper
 * (built once at open), and read() resolves inline promises -- a bounded
 * socket read, the same "bounded syscall" shape dyna:stream's own sources
 * use. dispose() closes the socket; read() after that throws (the resource
 * framework's closed check), matching every dyna:* resource.
 */

typedef struct {
    fe_conn_t  conn;
    int        chunked;
    uint64_t   clen;          /* framing end; UINT64_MAX = read to EOF */
    uint64_t   served;        /* body bytes handed out */
    uint64_t   max_body;      /* the fetcher's cap */
    int        eof;           /* framing end reached: next read is 0 */
    int        failed;        /* a read failed: the stream is finished */
    uint64_t   chunk_left;    /* bytes left in the current chunk */
    int        need_size;     /* next: read a chunk-size line */
    uint8_t   *rbuf; size_t rn, rcap, rpos;   /* read-ahead / dechunk buffer */
    char      *err;           /* pending error text; the next read throws it */
} fe_stream_t;

static JSClassID dyn_fs_class_id;

static void fs_dispose(void *native)
{
    fe_stream_t *s = (fe_stream_t *)native;
    if (!s) return;
    fe_conn_close(&s->conn);
    free(s->rbuf);
    free(s->err);
    free(s);
}

static const JSClassDef dyn_fs_class = {
    "FetcherStream", .finalizer = dyn_res_finalizer,
};

/* Byte-wide view coercion (stream_view_bytes's shape, local copy: dyna:stream
   is deliberately not linked from here). Coerces FIRST -- the conversion can
   run user JS -- then the caller resolves the native handle. */
static uint8_t *fe_view_bytes(JSContext *ctx, JSValueConst v, size_t *plen)
{
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    *plen = 0;
    if (p) { *plen = n; return p; }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t *base;
        if (JS_IsException(ab))
            return NULL;
        if (bpe != 1) {
            JS_FreeValue(ctx, ab);
            JS_ThrowTypeError(ctx,
                "FetcherStream.read: buf must be a byte-wide view "
                "(Uint8Array/DataView/ArrayBuffer)");
            return NULL;
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, ab);
        JS_FreeValue(ctx, ab);
        if (!base) return NULL;          /* detached mid-resolve */
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}

/* Inline-settled promises -- dyna:stream's helpers, local copies (a read
   RESOLVES; it never makes the caller await a second hop). */
static JSValue fe_promise_resolved(JSContext *ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, val); return promise; }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue fe_promise_rejected(JSContext *ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;
    if (JS_IsException(exc)) exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, exc); return promise; }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static void fs_fail(fe_stream_t *s, const char *msg)
{
    if (!s->failed) {
        s->failed = 1;
        free(s->err);
        s->err = strdup(msg);
    }
}

/* Pull at least one more byte into the read-ahead buffer. Returns 0 ok,
   -1 error (why set). */
static int fs_fill(fe_stream_t *s, char *why, size_t whyn)
{
    ssize_t r;
    if (s->rn == s->rcap) {
        size_t nc = s->rcap ? s->rcap * 2 : 8192;
        uint8_t *nb = (uint8_t *)realloc(s->rbuf, nc);
        if (!nb) { snprintf(why, whyn, "out of memory"); return -1; }
        s->rbuf = nb; s->rcap = nc;
    }
    r = fe_conn_recv(&s->conn, s->rbuf + s->rn, s->rcap - s->rn);
    if (r < 0) { snprintf(why, whyn, "receiving body failed"); return -1; }
    if (r == 0) { s->eof = 1; return 0; }   /* peer closed: an end */
    s->rn += (size_t)r;
    return 0;
}

/* Read one CRLF-terminated line (the \r\n consumed) into `line`. Returns
   0 ok, -1 error/eof-before-line (why set). Bounded by cap-1. */
static int fs_read_line(fe_stream_t *s, char *line, size_t cap,
                        char *why, size_t whyn)
{
    size_t o = 0;
    for (;;) {
        if (s->rpos >= s->rn) {
            s->rpos = s->rn = 0;
            if (fs_fill(s, why, whyn) < 0) return -1;
            if (s->eof) { snprintf(why, whyn, "body ended mid-line"); return -1; }
            continue;
        }
        {
            uint8_t c = s->rbuf[s->rpos++];
            if (c == '\n') {
                if (o && line[o-1] == '\r') o--;
                line[o] = 0;
                return 0;
            }
            if (o + 1 >= cap) {
                snprintf(why, whyn, "chunk line exceeds %zu bytes", cap - 1);
                return -1;
            }
            line[o++] = (char)c;
        }
    }
}

/* Move `want` body bytes from the read-ahead into dst (drains the buffer
   first). Returns the count actually moved (<= want; may be less if the
   buffer drained dry). */
static size_t fs_take(fe_stream_t *s, uint8_t *dst, size_t want)
{
    size_t have = s->rn - s->rpos;
    if (have > want) have = want;
    memcpy(dst, s->rbuf + s->rpos, have);
    s->rpos += have;
    if (s->rpos == s->rn) s->rpos = s->rn = 0;   /* keep the buffer small */
    return have;
}

/* The read() engine: up to `want` body bytes, framing-aware. Returns the
   count (0 = end), or -1 with why set. EVERY partial drain of the read-ahead
   is accounted BEFORE the refill retry -- the first cut advanced `got` only
   on full takes, so a short buffer drain followed by a refill wrote the next
   bytes OVER the ones already moved into the caller's buffer. */
static long long fs_read_body(fe_stream_t *s, uint8_t *dst, size_t want,
                              char *why, size_t whyn)
{
    size_t got = 0;
    if (s->eof) return 0;
    while (got < want) {
        if (s->chunked) {
            if (s->need_size) {
                char line[FE_STREAM_CHUNK_HDR];
                if (fs_read_line(s, line, sizeof line, why, whyn) < 0) return -1;
                {
                    char *endp;
                    long long sz = strtoll(line, &endp, 16);
                    if (endp == line || (*endp && *endp != ';' && *endp != ' ')) {
                        snprintf(why, whyn, "malformed chunk size \"%.16s\"", line);
                        return -1;
                    }
                    if (sz == 0) {
                        /* trailers until the blank line, then the end */
                        for (;;) {
                            if (fs_read_line(s, line, sizeof line, why, whyn) < 0)
                                return -1;
                            if (!line[0]) break;
                        }
                        s->eof = 1;
                        return (long long)got;
                    }
                    s->chunk_left = (uint64_t)sz;
                    s->need_size = 0;
                }
            }
            if (s->chunk_left == 0) {
                /* the current chunk is spent: the CRLF before the next size */
                char line[FE_STREAM_CHUNK_HDR];
                if (fs_read_line(s, line, sizeof line, why, whyn) < 0)
                    return -1;
                if (line[0]) {
                    snprintf(why, whyn, "malformed chunk terminator");
                    return -1;
                }
                s->need_size = 1;
                continue;
            }
            {
                size_t room = (size_t)(s->chunk_left < (uint64_t)(want - got)
                                       ? s->chunk_left : (uint64_t)(want - got));
                size_t moved = fs_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                s->chunk_left -= moved;
                if (moved < room) {
                    if (fs_fill(s, why, whyn) < 0) return -1;
                    if (s->eof) {
                        snprintf(why, whyn, "chunked body ended mid-chunk");
                        return -1;
                    }
                }
            }
        } else {
            uint64_t left = s->clen == UINT64_MAX
                            ? (uint64_t)(want - got)
                            : (s->clen > s->served ? s->clen - s->served : 0);
            if (left == 0) { s->eof = 1; return (long long)got; }
            {
                size_t room = (size_t)((uint64_t)(want - got) < left
                                       ? (uint64_t)(want - got) : left);
                size_t moved = fs_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                if (moved < room) {
                    if (fs_fill(s, why, whyn) < 0) return -1;
                    if (s->eof) {
                        /* the connection ended: an honest end only when the
                           framing said EOF; under a declared Content-Length
                           it is a truncated body, named as one */
                        if (s->clen != UINT64_MAX && s->served < s->clen) {
                            snprintf(why, whyn,
                                     "truncated body: connection ended after "
                                     "%llu of %llu declared bytes",
                                     (unsigned long long)s->served,
                                     (unsigned long long)s->clen);
                            return -1;
                        }
                        s->eof = 1;
                        return (long long)got;
                    }
                }
            }
        }
        if (s->served > s->max_body) {
            snprintf(why, whyn, "body exceeds maxBodyBytes");
            return -1;
        }
    }
    return (long long)got;
}

/* read(buf) -> Promise<number>. Coerce the buffer FIRST (the coercion can
   run user JS that may close() us), then resolve the native handle, then
   the bounded read, then settle. */
static JSValue dyn_fs_read(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    size_t len = 0;
    uint8_t *base;
    fe_stream_t *s;
    char why[160];

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "FetcherStream.read: buf must be a byte-wide view (Uint8Array)");
    base = fe_view_bytes(ctx, argv[0], &len);
    if (!base) return JS_EXCEPTION;
    s = (fe_stream_t *)dyn_res_native(ctx, this_val, dyn_fs_class_id);
    if (!s) return JS_EXCEPTION;
    if (len == 0)
        return fe_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (s->failed)
        /* sticky: the failure that closed the stream is re-reported by every
           later read, as a REJECTED promise (reads answer on the promise,
           never by throwing -- the ByteSource contract) */
        return fe_promise_rejected(ctx,
            JS_ThrowRangeError(ctx, "FetcherStream: %s",
                               s->err ? s->err : "body read failed"));
    if (s->eof)
        return fe_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    {
        long long n = fs_read_body(s, base, len, why, sizeof why);
        if (n < 0) {
            /* the promise rejects AND the stream is finished: the error is
               sticky (the next read rejects the same text), the socket is
               torn down now -- no raw fd may outlive this call's state */
            JSValue exc;
            fs_fail(s, why);
            fe_conn_close(&s->conn);
            exc = JS_ThrowRangeError(ctx, "FetcherStream: %s", why);
            return fe_promise_rejected(ctx, exc);
        }
        if (n == 0) s->eof = 1;
        return fe_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
}

static const JSCFunctionListEntry dyn_fs_proto[] = {
    JS_CFUNC_DEF("read", 1, dyn_fs_read),
};

/* Build the JS wrapper: the response fields + the read/close surface, one
   object. The status/headers ride as plain properties; `headers` is a
   plain object mapping (last value wins; the wire cannot be replayed
   anyway on a consumed stream). */
static JSValue fs_wrap(JSContext *ctx, fe_stream_t *s, const fe_head_t *h,
                       const char *url, int skipped_robots)
{
    JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_fs_class_id, s,
                               fs_dispose);
    if (JS_IsException(obj)) return obj;
    JS_DefinePropertyValueStr(ctx, obj, "status",
                              JS_NewInt32(ctx, skipped_robots ? 0 : h->status),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
                              JS_NewString(ctx, skipped_robots ? ""
                                          : h->status_text),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok",
                              JS_NewBool(ctx, !skipped_robots &&
                                          h->status >= 200 && h->status < 300),
                              JS_PROP_C_W_E);
    {
        JSValue hd = JS_NewObject(ctx);
        if (!JS_IsException(hd)) {
            if (h->content_type)
                JS_DefinePropertyValueStr(ctx, hd, "Content-Type",
                    JS_NewString(ctx, h->content_type), JS_PROP_C_W_E);
            if (h->clen >= 0) {
                char nb[24];
                snprintf(nb, sizeof nb, "%lld", h->clen);
                JS_DefinePropertyValueStr(ctx, hd, "Content-Length",
                                          JS_NewString(ctx, nb), JS_PROP_C_W_E);
            }
            JS_DefinePropertyValueStr(ctx, obj, "headers", hd, JS_PROP_C_W_E);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_DefinePropertyValueStr(ctx, obj, "headers", JS_NewObject(ctx),
                                      JS_PROP_C_W_E);
        }
    }
    JS_DefinePropertyValueStr(ctx, obj, "url", JS_NewString(ctx, url),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "contentType",
        JS_NewString(ctx, skipped_robots ? "" :
                     (h->content_type ? h->content_type : "")),
        JS_PROP_C_W_E);
    if (skipped_robots)
        JS_DefinePropertyValueStr(ctx, obj, "skippedByRobots", JS_TRUE,
                                  JS_PROP_C_W_E);
    return obj;
}

/* getStream(url) -> FetcherStream (the response AND the ByteSource). */
static JSValue dyn_fe_get_stream(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    fe_t *f;
    const char *url0 = NULL;
    char cur[1024], host[300];
    const char *path;
    int https, hop;
    JSValue ret = JS_EXCEPTION, uh;
    fe_conn_t conn = { -1,
#ifdef CONFIG_TLS
                       NULL
#endif
    };

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getStream(url)");
    /* Coerce BEFORE resolving: coercion runs user JS that can close() us --
       and so does reading this_val's own properties, so both happen here. */
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0) return JS_EXCEPTION;
    uh = JS_GetPropertyStr(ctx, this_val, "_headers");
    if (JS_IsException(uh)) { JS_FreeCString(ctx, url0); return JS_EXCEPTION; }
    f = (fe_t *)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    if (!f) { JS_FreeValue(ctx, uh); JS_FreeCString(ctx, url0); return JS_EXCEPTION; }
    if (strlen(url0) + 1 > sizeof cur) {
        JS_FreeCString(ctx, url0);
        return JS_ThrowRangeError(ctx, "Fetcher: url is too long");
    }
    snprintf(cur, sizeof cur, "%s", url0);
    JS_FreeCString(ctx, url0);
    {
        char *frag = strchr(cur, '#');
        if (frag) *frag = 0;
    }
    for (hop = 0; ; hop++) {
        fe_host_t *h;
        double wait, floor_ms;
        int attempt, st = 0;
        fe_head_t head;
        char why[160] = "";

        /* ZEROED AT DECLARATION: the retry loop opens with fe_head_free,
           and a first pass over garbage pointers was a free() of stack
           noise -- an abort one layout away from invisibility. */
        memset(&head, 0, sizeof head);
        head.clen = -1;

        https = fe_split(cur, host, sizeof host, &path);
        if (https < 0) {
            JS_ThrowTypeError(ctx,
                "Fetcher: only http:// and https:// urls (got %.60s)", cur);
            goto out;
        }
        if (!f->allow_private_hosts && fe_host_is_private(host)) {
            JS_ThrowTypeError(ctx,
                "Fetcher: %s://%.60s is a private/loopback/link-local host; "
                "pass allowPrivateHosts: true to fetch it",
                https ? "https" : "http", host);
            goto out;
        }
        h = fe_host(f, host);
        if (!h) { JS_ThrowOutOfMemory(ctx); goto out; }

        /* ---- robots, before anything is requested (get()'s gate) ---- */
        if (f->robots_on) {
            double now_ms = (double)dyn_timer_now_ms();
            if (!h->robots_tried || now_ms >= h->robots_next_ms) {
                /* robots.txt is a PAGE: it rides the injected client exactly
                   as the buffered path loads it (one loader, one policy) */
                JSValue client = JS_GetPropertyStr(ctx, this_val, "_client");
                int rrc = fe_load_robots(ctx, f, this_val, client, uh, h,
                                         https);
                JS_FreeValue(ctx, client);
                if (rrc < 0) goto out;
                /* the load ran the user's client */
                f = fe_live(ctx, this_val);
                if (!f) {
                    JS_ThrowTypeError(ctx,
                        "Fetcher: closed while getStream() was in flight");
                    goto out;
                }
                h = fe_host(f, host);
                if (!h) { JS_ThrowOutOfMemory(ctx); goto out; }
            }
            if (h->robots_unreachable && h->robots_ok)
                h->robots_unreachable = 0;
            if (h->robots_unreachable ||
                (h->robots && !rb_allows_path(h->robots, path, strlen(path)))) {
                /* the skip IS a response, as in get(): status 0, EOF body */
                fe_stream_t *s = (fe_stream_t *)calloc(1, sizeof *s);
                fe_head_t none;
                if (!s) { JS_ThrowOutOfMemory(ctx); goto out; }
                s->conn.fd = -1;
                s->eof = 1;
                s->max_body = (uint64_t)f->max_body;
                memset(&none, 0, sizeof none);
                none.clen = -1;
                f->skipped_robots++;
                ret = fs_wrap(ctx, s, &none, cur, 1);
                goto out;
            }
        }

        /* ---- the delay floor. Crawl-delay raises it, NEVER lowers it. -- */
        floor_ms = f->min_delay_ms;
        if (h->robots && h->robots->delay > 0) {
            double cd = h->robots->delay * 1000.0;
            if (cd > floor_ms) floor_ms = cd;
        }
        wait = h->next_ok_ms - (double)dyn_timer_now_ms();
        if (wait > 0) { f->throttled_ms += wait; fe_sleep_ms(wait); }

        /* ---- exchanges, with get()'s retry contract ---- */
        for (attempt = 0; ; attempt++) {
            sc_sb_t req;
            double back;
            char portless[300];
            uint16_t port;

            if (attempt > 0 || hop > 0) fe_conn_close(&conn);
            fe_head_free(&head);
            memset(&head, 0, sizeof head);
            head.clen = -1;

            /* split host:port for the connect (the Host header carries the
               host VERBATIM, port included when the url named one) */
            {
                const char *cp = strchr(host, ']');
                const char *colon = cp ? strchr(cp, ':') : strchr(host, ':');
                if (colon) {
                    size_t nl = (size_t)(colon - host);
                    long p;
                    if (nl + 1 > sizeof portless) nl = sizeof portless - 1;
                    memcpy(portless, host, nl);
                    portless[nl] = 0;
                    p = strtol(colon + 1, NULL, 10);
                    port = (uint16_t)(p > 0 && p < 65536 ? p : (https ? 443 : 80));
                } else {
                    snprintf(portless, sizeof portless, "%s", host);
                    port = (uint16_t)(https ? 443 : 80);
                }
                if (portless[0] == '[') {
                    /* bare the v6 literal for getaddrinfo/SNI */
                    char *close_b = strchr(portless, ']');
                    if (close_b) {
                        memmove(portless, portless + 1,
                                (size_t)(close_b - portless) - 1);
                        portless[(size_t)(close_b - portless) - 1] = 0;
                    }
                }
            }
            conn.fd = fe_tcp_connect(portless, port, why, sizeof why);
#ifdef CONFIG_TLS
            if (conn.fd >= 0 && https &&
                fe_tls_connect(f, &conn, portless, why, sizeof why) < 0)
                conn.fd = -1;
#endif
            if (conn.fd < 0) {
                st = 0;                        /* why is set: retry or fail */
            } else {
                /* the request: path verbatim (fe_split built it), Host with
                   the port when the url named one, our agent ALWAYS (a
                   crawler that identifies itself only for pages is not
                   identified), identity encoding, caller headers under the
                   same CR/LF/NUL refusal fe_raw enforces, Connection: close */
                sc_sb_init(&req);
                {
                    char line[1200];
                    int bad = 0;
                    snprintf(line, sizeof line, "GET %s HTTP/1.1\r\n", path);
                    sc_sb_puts(&req, line);
                    snprintf(line, sizeof line, "Host: %s\r\n", host);
                    sc_sb_puts(&req, line);
                    snprintf(line, sizeof line, "User-Agent: %s\r\n", f->agent);
                    sc_sb_puts(&req, line);
                    sc_sb_puts(&req, "Accept-Encoding: identity\r\n");
                    if (JS_IsObject(uh)) {
                        JSPropertyEnum *tab = NULL;
                        uint32_t tn = 0, i;
                        if (JS_GetOwnPropertyNames(ctx, &tab, &tn, uh,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                            for (i = 0; i < tn && !bad; i++) {
                                JSValue v = JS_GetProperty(ctx, uh, tab[i].atom);
                                const char *ks = NULL, *vs = NULL;
                                size_t b;
                                if (!JS_IsException(v))
                                    vs = JS_ToCString(ctx, v);
                                if (vs) ks = JS_AtomToCString(ctx, tab[i].atom);
                                if (!ks || !vs) {
                                    bad = 1;
                                } else {
                                    for (b = 0; vs[b]; b++)
                                        if (vs[b] == '\r' || vs[b] == '\n' ||
                                            vs[b] == '\0') break;
                                    if (vs[b] || strpbrk(ks, "\r\n:")) {
                                        JS_ThrowTypeError(ctx,
                                            "Fetcher: header \"%s\" value or "
                                            "name carries CR, LF, NUL or a "
                                            "colon -- a value rides to the "
                                            "wire byte-for-byte and would "
                                            "split the request", ks);
                                        bad = 1;
                                    } else {
                                        sc_sb_puts(&req, ks);
                                        sc_sb_puts(&req, ": ");
                                        sc_sb_puts(&req, vs);
                                        sc_sb_puts(&req, "\r\n");
                                    }
                                }
                                if (ks) JS_FreeCString(ctx, ks);
                                if (vs) JS_FreeCString(ctx, vs);
                                if (!JS_IsException(v)) JS_FreeValue(ctx, v);
                            }
                            for (i = 0; i < tn; i++) JS_FreeAtom(ctx, tab[i].atom);
                            js_free(ctx, tab);
                        } else {
                            bad = 1;   /* a throwing getter must propagate:
                                          its exception stays pending and
                                          the req.oom check below fails us
                                          out WITHOUT replacing it */
                        }
                    }
                    if (!bad) sc_sb_puts(&req, "Connection: close\r\n\r\n");
                }
                if (req.oom) {
                    sc_sb_free(&req);
                    fe_conn_close(&conn);
                    fe_head_free(&head);
                    JS_FreeValue(ctx, uh);
                    if (!JS_HasException(ctx))
                        JS_ThrowOutOfMemory(ctx);
                    goto out;
                }
                if (fe_conn_send_all(&conn, req.p ? req.p : "", req.n) < 0) {
                    snprintf(why, sizeof why, "sending request failed");
                    st = 0;
                } else if (fe_read_head(&conn, &head, why, sizeof why) < 0) {
                    st = 0;
                } else {
                    st = head.status;
                }
                sc_sb_free(&req);
            }
            h->next_ok_ms = (double)dyn_timer_now_ms() + floor_ms;

            /* ---- get()'s retry decision, verbatim ---- */
            if (st != 0 && !(st == 429 || (st >= 500 && st < 600)))
                break;
            if (st != 0 && attempt >= f->retries)
                break;
            if (st == 0 && attempt >= f->retries) {
                fe_conn_close(&conn);
                fe_head_free(&head);
                JS_FreeValue(ctx, uh);
                JS_ThrowRangeError(ctx,
                    "Fetcher: no response (%.100s) after %d attempt%s", why,
                    attempt + 1, attempt ? "s" : "");
                goto out;
            }
            /* Exponential backoff, jittered; Retry-After overrides; a delay
               beyond 60s is an instruction, not a nap -- serve whatever the
               wire holds as the final answer (get()'s contract). */
            back = f->min_delay_ms * (double)(1 << (attempt < 10 ? attempt : 10));
            back *= 0.75 + 0.5 * fe_rand01(f);
            if (st != 0 && head.retry_after) {
                double secs = strtod(head.retry_after, NULL);
                if (!(secs > 0)) {
                    int64_t when = fe_http_date(head.retry_after);
                    if (when > 0) {
                        int64_t now = (int64_t)time(NULL);
                        secs = when > now ? (double)(when - now) : 0.0;
                    }
                }
                if (secs > 0) back = secs * 1000.0;
            }
            if (back > 60000.0)
                break;
            /* The error body is worthless here: drop the connection instead
               of draining an unframed 500's body -- a fresh connection is
               opened for the retry anyway. */
            fe_conn_close(&conn);
            f->retried++;
            fe_sleep_ms(back);
        }

        /* ---- redirects ---- */
        if (st >= 300 && st < 400) {
            if (head.location && hop < f->max_redirects) {
                char nxt[sizeof cur];
                int prev_https = fe_scheme_is_https(cur);
                if (fe_resolve(cur, head.location, nxt, sizeof nxt) > 0 &&
                    strcmp(nxt, cur)) {
                    char lh[300];
                    const char *lp;
                    /* A TLS stream must not walk into plaintext; a 30x must
                       not smuggle us inward -- get()'s gates, per hop. */
                    if (prev_https && !fe_scheme_is_https(nxt) &&
                        !f->allow_insecure_downgrade) {
                        fe_conn_close(&conn);
                        fe_head_free(&head);
                        JS_FreeValue(ctx, uh);
                        JS_ThrowRangeError(ctx,
                            "Fetcher: redirect downgrades https to http "
                            "(%.80s); pass allowInsecureDowngrade: true to "
                            "follow it", nxt);
                        goto out;
                    }
                    if (fe_split(nxt, lh, sizeof lh, &lp) >= 0 &&
                        !f->allow_private_hosts && fe_host_is_private(lh)) {
                        fe_conn_close(&conn);
                        fe_head_free(&head);
                        JS_FreeValue(ctx, uh);
                        JS_ThrowTypeError(ctx,
                            "Fetcher: redirect target %.60s is a private/"
                            "loopback/link-local host; pass "
                            "allowPrivateHosts: true to fetch it", lh);
                        goto out;
                    }
                    snprintf(cur, sizeof cur, "%s", nxt);
                    fe_conn_close(&conn);
                    fe_head_free(&head);
                    continue;                  /* next hop */
                }
                /* unresolvable Location: serve this response as final */
            } else if (hop >= f->max_redirects) {
                fe_conn_close(&conn);
                fe_head_free(&head);
                JS_FreeValue(ctx, uh);
                JS_ThrowRangeError(ctx,
                    "Fetcher: more than %d redirects", f->max_redirects);
                goto out;
            }
            /* a 3xx without a usable Location is served as the final
               response -- the stream hands out whatever body it carries */
        }

        /* ---- final: wrap what is on the wire ---- */
        {
            /* Declared length first: an honest server must not ship a body
               the cap would reject -- get()'s early refusal, byte for byte.
               Received bytes are counted inside read() as they are handed
               out, so a lying header still cannot push more than the cap. */
            if (!head.chunked && head.clen >= 0 &&
                (uint64_t)head.clen > (uint64_t)f->max_body) {
                fe_conn_close(&conn);
                fe_head_free(&head);
                JS_FreeValue(ctx, uh);
                JS_ThrowRangeError(ctx,
                    "Fetcher: declared Content-Length %lld exceeds "
                    "maxBodyBytes", head.clen);
                goto out;
            }
            {
                fe_stream_t *s = (fe_stream_t *)calloc(1, sizeof *s);
                if (!s) {
                    fe_conn_close(&conn);
                    fe_head_free(&head);
                    JS_FreeValue(ctx, uh);
                    JS_ThrowOutOfMemory(ctx);
                    goto out;
                }
                s->conn = conn;
                conn.fd = -1;                  /* the stream owns the socket */
#ifdef CONFIG_TLS
                conn.tls = NULL;
#endif
                s->chunked = head.chunked;
                s->clen = (head.chunked || head.clen < 0)
                          ? UINT64_MAX : (uint64_t)head.clen;
                s->need_size = head.chunked;
                s->max_body = (uint64_t)f->max_body;
                f->fetched++;
                ret = fs_wrap(ctx, s, &head, cur, 0);
                fe_head_free(&head);
                goto out;
            }
        }
    }
out:
    JS_FreeValue(ctx, uh);
    fe_conn_close(&conn);
    return ret;
}


static JSValue dyn_scrape_noop(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{ (void)ctx; (void)this_val; (void)argc; (void)argv; return JS_UNDEFINED; }

static void dyn_scrape_promise_swallow(JSContext *ctx, JSValue v)
{
    JSAtom catom = JS_NewAtom(ctx, "catch");
    JSValue cf = JS_GetProperty(ctx, v, catom);
    JS_FreeAtom(ctx, catom);
    if (JS_IsFunction(ctx, cf)) {
        /* .catch(noop) RETURNS a derived promise -- an owned ref. Dropping
           it on the floor leaked one promise per swallowed evaluation, and
           the sweep's exit-time gc_obj_list assertion was the receipt. */
        JSValue noop = JS_NewCFunction(ctx, dyn_scrape_noop, "", 0);
        JSValue caught = JS_Call(ctx, cf, v, 1, (JSValueConst *)&noop);
        JS_FreeValue(ctx, caught);
        JS_FreeValue(ctx, noop);
    }
    JS_FreeValue(ctx, cf);
    JS_FreeValue(ctx, v);
}

/*construct the module's own HTTPClient for a Fetcher whose ctor was
 * not given one. dyna:scrape still does not LINK dyna:net -- the client is
 * built by evaluating a one-line module that imports it, the exact bridge
 * dyna-libc's dyn_global_install_from_module uses for the URL/structuredClone
 * globals: COMPILE_ONLY eval, JS_EvalFunction runs the import graph, and a
 * synchronous module's evaluation promise is already fulfilled on return.
 * The holder property parks the client where C can reach it and is deleted
 * on the way out, so nothing user-visible leaks onto globalThis.
 *
 * max_body (the Fetcher's own maxBodyBytes) is baked into the source as the
 * HTTPClient ctor's byte cap: the client must never refuse a body the
 * fetcher's policy already allows, and the fetcher's cap then governs.
 *
 * A build without dyna:net (the slim SCRAPE binary) throws from the import:
 * the ctor turns that into a clear message pointing at the injected form. */
static JSValue fe_auto_client(JSContext *ctx, double max_body)
{
    /* Sized for the snippet BELOW with room to spare: snprintf silently
       truncating the module source once turned this into a syntax error
       that the conversion below reported as "is dyna:net available?" --
       on builds where dyna:net very much was. 256 covers the full text
       plus a 10-digit cap with margin. */
    char src[256];
    JSValue r, v = JS_UNDEFINED, g, hobj, client;
    JSPromiseStateEnum st;
    const char *holder = "__dyna_scrape_client";

    /* The snippet must NEVER REJECT: a module-evaluation failure surfaces
       as a rejected promise that reports "possibly unhandled" AT REJECTION
       TIME -- a .catch attached after JS_EvalFunction returns is too late.
       A ctor failure inside the snippet is caught in the source and
       communicated as an undefined holder. */
    snprintf(src, sizeof src,
             "import { HTTPClient } from \"dyna:net\";\n"
             "try { globalThis.%s = new HTTPClient(%d); }\n"
             "catch (e) { globalThis.%s = undefined; }\n",
             holder, max_body >= (double)(1 << 30) ? (1 << 30) : (int)max_body,
             holder);
    r = JS_Eval(ctx, src, strlen(src), "<dyna:scrape fetcher client>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r)) {
        /* the slim SCRAPE build has no dyna:net: the pending ReferenceError
           names a module the caller never wrote -- replace it with the
           actionable message (the same conversion Sitemap.list's twin
           bridge makes). */
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:net available in this build?) -- pass "
            "`client` explicitly to inject your own");
    }
    v = JS_EvalFunction(ctx, r);   /* consumes r; runs the import graph */
    if (JS_IsException(v)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:net available in this build?) -- pass "
            "`client` explicitly to inject your own");
    }
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        dyn_scrape_promise_swallow(ctx, v);   /* handler first, then free */
        JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:net available in this build?) -- pass "
            "`client` explicitly to inject your own");
        return JS_EXCEPTION;
    }
    dyn_scrape_promise_swallow(ctx, v);
    g = JS_GetGlobalObject(ctx);
    hobj = JS_GetPropertyStr(ctx, g, holder);
    client = JS_IsObject(hobj) ? JS_DupValue(ctx, hobj) : JS_UNDEFINED;
    JS_FreeValue(ctx, hobj);
    {
        JSAtom hatom = JS_NewAtom(ctx, holder);
        JS_DeleteProperty(ctx, g, hatom, 0);
        JS_FreeAtom(ctx, hatom);
    }
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(client)) {
        JS_FreeValue(ctx, client);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed -- pass `client` explicitly to inject your own");
    }
    return client;
}

static JSValue dyn_fe_ctor(JSContext *ctx, JSValueConst nt, int argc,
                           JSValueConst *argv)
{
    fe_t *f;
    JSValue av, cv, hv;
    const char *agent = NULL;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Fetcher({ agent, client? }): `agent` is required; omit "
            "`client` to use the built-in HTTPClient, or pass one to inject");
    /*reject unknown keys before any option is read. */
    if (dyn_opts_strict(ctx, argv[0], sc_fetcher_keys, 15))
        return JS_EXCEPTION;
    av = JS_GetPropertyStr(ctx, argv[0], "agent");
    if (JS_IsException(av)) return JS_EXCEPTION;   /* getter threw (X-12) */
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
    if (JS_IsException(cv)) { JS_FreeCString(ctx, agent); return JS_EXCEPTION; }
    hv = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (JS_IsException(hv)) { JS_FreeValue(ctx, cv); JS_FreeCString(ctx, agent); return JS_EXCEPTION; }
    /*a PRESENT but non-object client is the caller's intent
       misspelled -- silently treating 42/"str"/true as "no client" would
       swap their transport for the built-in one. Null counts as absent,
       the module-wide bag convention. */
    if (!JS_IsUndefined(cv) && !JS_IsNull(cv) && !JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `client` must be an object (a dyna:net HTTPClient, or "
            "a mock with request(method, url, body, headers)) -- omit it or "
            "pass null to use the built-in HTTPClient");
    }
    /*an absent `client` no longer refuses construction -- the module
       builds its own HTTPClient below, once maxBodyBytes is known. Null
       counts as absent, the module-wide bag convention. */
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

    {
        /* Strict reads (audit X-12): an option getter that throws fails the
           construction with the ORIGINAL error; no silent defaults. */
        int v_robots, v_delay, v_retries, v_maxred, v_maxbody;
        int v_priv, v_reval, v_ttl, v_insec, v_pool;
        if (fe_num_prop(ctx, argv[0], "robots", 1, &v_robots) ||
            fe_num_prop(ctx, argv[0], "minDelayMs", 1000, &v_delay) ||
            fe_num_prop(ctx, argv[0], "retries", 3, &v_retries) ||
            fe_num_prop(ctx, argv[0], "maxRedirects", 5, &v_maxred) ||
            fe_num_prop(ctx, argv[0], "maxBodyBytes", 8 << 20, &v_maxbody) ||
            fe_num_prop(ctx, argv[0], "allowPrivateHosts", 0, &v_priv) ||
            fe_num_prop(ctx, argv[0], "revalidate", 1, &v_reval) ||
            fe_num_prop(ctx, argv[0], "robotsTtlMs", 86400000, &v_ttl) ||
            fe_strict_int_prop(ctx, argv[0], "poolSize", 4, &v_pool) ||
            fe_num_prop(ctx, argv[0], "allowInsecureDowngrade", 0, &v_insec)) {
            JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
            free(f->agent); free(f);
            return JS_EXCEPTION;
        }
        f->robots_on     = v_robots ? 1 : 0;
        f->min_delay_ms  = v_delay;
        f->retries       = v_retries;
        f->max_redirects = v_maxred;
        f->max_body      = (double)v_maxbody;
        f->allow_private_hosts = v_priv ? 1 : 0;
        f->revalidate    = v_reval ? 1 : 0;
        f->robots_ttl_ms = (double)v_ttl;
        if (f->robots_ttl_ms < 0) f->robots_ttl_ms = 0;
        f->allow_insecure_downgrade = v_insec ? 1 : 0;
        f->pool_size = v_pool;
        if (f->pool_size < 1 || f->pool_size > 64) {
            JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
            free(f->agent); free(f);
            return JS_ThrowRangeError(ctx,
                "Fetcher: poolSize must be an integer from 1 to 64");
        }
    }
    /* simple-options: proxy/ca are plain strings (a proxy URL, a CA
     * bundle path/PEM). Null/undefined counts as absent like every other
     * optional string here; any other present type is refused. Stored on
     * the fetcher and exposed via stats() -- validated and observable,
     * never silently ignored. */
    {
        JSValue pv = JS_GetPropertyStr(ctx, argv[0], "proxy");
        JSValue qv = JS_GetPropertyStr(ctx, argv[0], "ca");
        if (JS_IsException(pv) || JS_IsException(qv)) {
            JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
            JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
            free(f->agent); free(f);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(pv) && !JS_IsNull(pv)) {
            const char *s;
            if (!JS_IsString(pv)) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a string URL");
            }
            s = JS_ToCString(ctx, pv);
            if (!s) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a non-empty string");
            }
            f->proxy = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->proxy) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        if (!JS_IsUndefined(qv) && !JS_IsNull(qv)) {
            const char *s;
            if (!JS_IsString(qv)) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f->proxy); free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a string path");
            }
            s = JS_ToCString(ctx, qv);
            if (!s) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f->proxy); free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f->proxy); free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a non-empty string");
            }
            f->ca = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->ca) {
                JS_FreeValue(ctx, pv); JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv); JS_FreeValue(ctx, hv);
                free(f->agent); free(f->proxy); free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, qv);
    }
    if (f->retries < 0) f->retries = 0;
    if (f->max_redirects < 0) f->max_redirects = 0;
    if (f->min_delay_ms < 0) f->min_delay_ms = 0;
    /*the auto-client, built LAST so it can inherit the settled
       maxBodyBytes. Runs before the wrap: a failure here must unwind the
       half-built fetcher (and the caller's `headers` value) cleanly. */
    if (!JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        cv = fe_auto_client(ctx, f->max_body);
        if (JS_IsException(cv)) {
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f->proxy);
            free(f->ca);
            free(f);
            return JS_EXCEPTION;
        }
    }
    /* Jitter seed: OS entropy first, time-and-address as the fallback (the
       old xorshift mix). SplitMix64 has no forbidden state, so 0 would be
       fine -- the fallback exists for a failed kernel source, not for 0. */
    if (dyn_os_entropy(&f->rng, sizeof f->rng) != 0)
        f->rng = ((uint64_t)dyn_timer_now_ms() << 17) ^ (uintptr_t)f ^
                 0x9E3779B97F4A7C15ULL;
    {
        /* nt (not JS_UNDEFINED): the Fetcher ctor IS under [[Construct]], so
           `class F extends Fetcher {}` must produce F instances. */
        JSValue obj = dyn_res_wrap(ctx, nt, dyn_fe_class_id, f, fe_dispose);
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
    JS_CFUNC_DEF("getAsync", 1, dyn_fe_get_async),
    JS_CFUNC_DEF("stats", 0, dyn_fe_stats),
    /* + CC-2-scrape: the streaming body path (a dyna:stream-compatible
       ByteSource; the module does NOT import dyna:stream) */
    JS_CFUNC_DEF("getStream", 1, dyn_fe_get_stream),
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

/* ----: serialize/resume -- the crawl's own state codec --------------
 *
 * A long crawl should survive a restart. serialize() writes the frontier
 * (remaining queue WITH depths), the visited set (normalised dedup keys),
 * the bounds and the field names -- everything next() consults -- as a
 * versioned JSON envelope. resume() (a static on Crawl, because the state
 * outlives any one instance) rebuilds a crawl whose remaining page set is
 * the one the donor had left.
 *
 * NOT preserved, by design and documented on both sides:
 * - in-flight fetches (concurrency): a page being fetched at
 *   serialize() time is in `seen` but neither queued nor emitted; resuming
 *   drops it. Serialize a quiescent crawl (concurrency 1, or drained) when
 *   the remaining set must be exact.
 * - response caches: the conditional-GET validator store and the robots
 *   copies live on the FETCHER, not the crawl. Resume with the same fetcher
 *   (or one rewarmed the same way) to keep them; a fresh fetcher re-fetches
 *   robots and re-validates, which is correct, merely slower.
 * - JS objects: the fetcher, extractor and parser are arguments of
 *   resume(), never part of the state.
 *
 * The envelope is JSON so a damaged file fails LOUDLY in the parser instead
 * of half-restoring; every field is re-validated on the way in (a state file
 * is untrusted input), and anything off -- wrong version, a queue entry
 * longer than a url may be, a depth below 0 -- is a TypeError, not a guess.
 */

#define CR_STATE_VERSION 1



typedef struct {
    char  *url;
    int    depth;
} cr_item_t;

/* A page can contribute only bounded new frontier: a hostile page must not
   convert maxPages into unbounded memory on the queue it cannot control. */
#define CR_MAX_PENDING 10000

/*the concurrency ceiling. Parallelism beyond this stops being polite
   (per-host floors or not, sixteen connections per host is a stampede) and
   stops fitting the per-page job model the runtime tests pin. */
#define CR_MAX_CONCURRENCY 16

typedef struct cr_flight cr_flight_t;
typedef struct cr_next_wait cr_next_wait_t;

struct cr_next_wait {
    JSValue resolve;       /* the deferred next()'s resolve */
    JSValue reject;        /* and its reject */
    cr_next_wait_t *next;
};


typedef struct {
    int refs;        /* 1 for the crawl + 1 per flight + 1 per queued waiter */
    int crawl_alive; /* 0 once cr_dispose ran (the crawl's ref released) */
} cr_owner_t;

/* flight states */
#define CR_FS_HOP      0   /* split + gates + floor: may sleep, then exchange */
#define CR_FS_EXCHANGE 1   /* the wire call: requestAsync-parked or sync */
#define CR_FS_DECIDE   3   /* an exchange landed: stamp, retry/redirect/tail */
#define CR_FS_BACKOFF  2   /* sleeping out a retry backoff */

/* cr_flight_free is referenced by cr_dispose (which must NOT free parked
 * flights: their settle continuations still carry the pointers). */
static void cr_flight_free(JSContext *ctx, cr_flight_t *fl);
static void cr_flight_free_ex(cr_flight_t *fl, JSContext *ctx,
                              JSRuntime *rt);





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
    /*pages allowed in flight at once. 1 (the default) is the original
       synchronous behaviour byte for byte; >1 switches next() to promises. */
    int        concurrency;
    /* ---- engine state (all of it lives or dies with the crawl; the
       flights themselves are SELF-OWNED once parked: dispose() may run
       while fetches are in flight, and a parked continuation then outlives
       this struct. cr_owner_t counts one ref for the crawl plus one per
       live flight and per queued next() waiter; a refcount below its
       flight's own 2 tells a continuation the crawl is gone.) ---- */
    JSContext *ctx;            /* the ctor's context (dispose has none) */
    JSRuntime *rt;             /* the ctor's runtime: the ONLY handle a
                                  context-less finalizer may use */
    /* the operation-scoped pin (the HTTPClient self_pending pattern): a dup
       of the crawl object held while flights run, so a parked continuation
       finds a live crawl; JS_UNDEFINED while idle -- a PERMANENT dup made
       every unclosed crawl trip the runtime's teardown assertion */
    JSValue    self_pending;
    void      *owner;          /* cr_owner_t* */
    cr_flight_t *flights;      /* live (running or parked) flights */
    size_t     n_flights;
    cr_flight_t *done_head, *done_tail;   /* completed pages, FIFO */
    size_t     n_done;
    cr_next_wait_t *wait_head, *wait_tail;/* pending next() resolvers, FIFO */
    JSValue    pending_err;    /* a flight failed with nobody waiting */
    unsigned   flight_seq;     /* the last dispatched flight's id */
} cr_t;

struct cr_flight {
    int state;               /* CR_FS_* */
    char cur[1024], host[300];
    const char *path;
    int https, hop, attempt, st, depth;
    double floor_ms;
    JSContext *ctx;
    cr_t *c;                 /* the owning crawl (valid while crawl_alive) */
    JSValue crawl;           /* the crawl object (this_val for cr_make_page) */
    JSValue fetcher;         /* dup of crawl._fetcher */
    JSValue client;          /* dup of fetcher._client */
    JSValue uh;              /* dup of fetcher._headers */
    JSValue res;             /* response in hand (UNDEFINED between hops;
                                 the EXCEPTION marker = a failed exchange) */
    JSValue pending;         /* the parked promise, while parked */
    JSValue err;             /* recorded failure for a waiter to reject */
    int finished;            /* finish() ran: idempotence guard */
    int floor_claimed;       /* the host slot was stamped for this flight */
    int park_claims;         /* outstanding park settles for this flight */
    int graveyard;           /* freed-pending: the last settle frees it */
    int pins_released;       /* cr_flight_free ran: the JSValue pins are
                                gone (and UNDEFINED) -- a second release
                                is a no-op */
    int claim_released;      /* the owner claim was dropped at the free (or
                                sweep) rather than at the destroy: the shell
                                may then outlive the crawl struct safely */
    unsigned magic;          /* CR_FLIGHT_MAGIC while allocated */
    unsigned seq;            /* the dispatch sequence: the settle looks the
                                 flight up in the live list by this */
    cr_flight_t *next;       /* live-list link, then the done-FIFO link */
};

/* The flight's JSValue pins are CONTIGUOUS crawl..err (7 values): the
   release walks them as one block. */
_Static_assert(offsetof(cr_flight_t, err) ==
               offsetof(cr_flight_t, crawl) + 6 * sizeof(JSValue),
               "cr_flight_t pins must stay contiguous");

static JSClassID dyn_cr_class_id;

/* Drop ONE owner ref; the LAST one frees the owner AND the crawl struct.
 * The crawl struct outlives cr_dispose when flights are still parked: their
 * settle continuations carry `fl->c` and must never write into freed memory
 * (which the heap may already have handed to a DIFFERENT crawl -- the
 * cross-instance class: a dead crawl's late completion unlinking itself from
 * a stranger's flight list and queueing a phantom page into it). */
static void cr_release(cr_t *c)
{
    cr_owner_t *own = (cr_owner_t *)c->owner;
    if (--own->refs == 0) {
        free(own);
        free(c);
    }
}

/* Fail a (resolve, reject) pair at shutdown -- the parked promise gets a
 * rejection naming engine shutdown while JS is legal, and the pins are
 * released (the JS_FreeRuntime straggler releases without settling).
 * LIVENESS: these parks have no completer outside the JS heap, so they
 * hold nothing open -- a quiet exit is normal and the sweep speaks here;
 * the file family's kernel completion is an event source and its jobs wait
 * for it (the one policy, stated at both declarations -- see dyna-file.c). */
static void cr_park_fail(JSContext *ctx, JSRuntime *rt,
                         JSValue *resolve, JSValue *reject,
                         const char *what)
{
    if (JS_IsUndefined(*resolve) && JS_IsUndefined(*reject))
        return;                        /* already settled or swept */
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc)) {
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, what), JS_PROP_WRITABLE |
                JS_PROP_CONFIGURABLE);
        } else {
            exc = JS_GetException(ctx);
        }
        r = JS_Call(ctx, *reject, JS_UNDEFINED, 1, (JSValueConst *)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, *resolve);
        JS_FreeValue(ctx, *reject);
    } else {
        JS_FreeValueRT(rt, *resolve);
        JS_FreeValueRT(rt, *reject);
    }
    *resolve = JS_UNDEFINED;
    *reject = JS_UNDEFINED;
}

/* The crawl's shutdown sweep: every parked promise of THIS instance fails
 * and every pin it owns is released -- the per-instance ownership is the
 * whole point: one crawl's teardown must not touch another's state. The
 * flight shells go through the same cr_flight_free_ex their own sweeps use
 * (those entries are unlinked by the call), so nothing is released twice. */
static void cr_sweep(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    cr_t *c = (cr_t *)opaque;
    cr_owner_t *own = c ? (cr_owner_t *)c->owner : NULL;
    cr_next_wait_t *waiters;
    cr_flight_t *flights, *done;
    JSValue sp, pe;

    if (!c || !own)
        return;
    own->crawl_alive = 0;      /* late completions go nowhere */
    /* DETACH EVERYTHING FIRST. Releasing a flight's pins releases its dup
       of the crawl OBJECT, which can finalize that object and run
       cr_dispose -- which walks the queues and drops the crawl's own claim
       (freeing `c` when it is the last one). After this point `c` is
       never touched: the detached lists carry the owner claims that keep
       it alive exactly until their own release runs. */
    waiters = c->wait_head;
    flights = c->flights;
    done = c->done_head;
    sp = c->self_pending;
    pe = c->pending_err;
    c->wait_head = c->wait_tail = NULL;
    c->flights = NULL;
    c->n_flights = 0;
    c->done_head = c->done_tail = NULL;
    c->n_done = 0;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    while (waiters) {
        cr_next_wait_t *w = waiters;
        waiters = w->next;
        cr_park_fail(ctx, rt, &w->resolve, &w->reject,
                     "Crawl: next() aborted at engine shutdown");
        free(w);
        cr_release(c);         /* the waiter's owner claim */
    }
    while (flights) {
        cr_flight_t *fl = flights;
        flights = fl->next;
        cr_flight_free_ex(fl, ctx, rt);
    }
    while (done) {
        cr_flight_t *fl = done;
        done = fl->next;
        cr_flight_free_ex(fl, ctx, rt);
    }
    if (ctx) {
        JS_FreeValue(ctx, sp);
        JS_FreeValue(ctx, pe);
    } else {
        JS_FreeValueRT(rt, sp);
        JS_FreeValueRT(rt, pe);
    }
}

static void cr_dispose(void *native)
{
    cr_t *c = (cr_t *)native;
    cr_owner_t *own;
    size_t i;
    if (!c) return;
    JS_RemoveShutdownSweep(c->rt, cr_sweep, c);
    for (i = 0; i < c->qn; i++) free(c->q[i].url);
    free(c->q);
    for (i = 0; i < c->sn; i++) free(c->seen[i]);
    free(c->seen);
    free(c->link_field);
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    /* teardown: queued next promises settle {done:true} -- a closed
       crawl ENDS, it never leaves an await hanging. PARKED FLIGHTS are NOT
       freed here: their settle continuations still carry the pointers, and
       each flight's own owner ref keeps its memory (and the pinned crawl
       JSValue) alive until its settle reads the flag and drops it. The
       done FIFO dies with the crawl -- nobody is left to pick pages up. */
    own = (cr_owner_t *)c->owner;
    if (own) {
        own->crawl_alive = 0;
        while (c->wait_head) {
            cr_next_wait_t *w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            out = JS_NewObject(c->ctx);
            JS_DefinePropertyValueStr(c->ctx, out, "done", JS_TRUE,
                                      JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(c->ctx, out, "value", JS_UNDEFINED,
                                      JS_PROP_C_W_E);
            r = JS_Call(c->ctx, w->resolve, JS_UNDEFINED, 1,
                        (JSValueConst *)&out);
            JS_FreeValue(c->ctx, r);
            JS_FreeValue(c->ctx, out);
            JS_FreeValue(c->ctx, w->resolve);
            JS_FreeValue(c->ctx, w->reject);
            free(w);
            cr_release(c);          /* the waiter's owner claim */
        }
        c->wait_tail = NULL;
        while (c->done_head) {
            cr_flight_t *fl = c->done_head;
            c->done_head = fl->next;
            cr_flight_free(c->ctx, fl);
        }
        c->done_tail = NULL;
        c->n_done = 0;
        c->flights = NULL;          /* parked flights self-free */
        c->n_flights = 0;
    }
    JS_FreeValue(c->ctx, c->self_pending);
    JS_FreeValue(c->ctx, c->pending_err);
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    /* The crawl's own ref goes LAST: the loops above may have dropped the
       waiters' and flights' refs, and the struct must outlive every one of
       them. When flights are still parked this is NOT the last ref -- the
       struct (and its owner) then die with the last flight's settle. */
    if (own)
        cr_release(c);
    else
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

static void cr_install_async_iterator(JSContext *ctx, JSValue obj);

static JSValue dyn_cr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                           JSValueConst *argv)
{
    cr_t *c;
    JSValue opt, lf = JS_UNDEFINED, obj;
    const char *s;
    int v_maxpages, v_maxdepth, v_samehost, v_conc;
    cr_owner_t *own;
    (void)nt;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Crawl(fetcher[, { maxPages, maxDepth, sameHost, linkField }])");
    c = (cr_t *)calloc(1, sizeof(*c));
    if (!c) return JS_ThrowOutOfMemory(ctx);
    own = (cr_owner_t *)calloc(1, sizeof *own);
    if (!own) { free(c); return JS_ThrowOutOfMemory(ctx); }
    own->refs = 1;               /* the crawl's own ref */
    own->crawl_alive = 1;
    c->owner = own;
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->concurrency = 1;
    /* calloc-zeroed is NOT a JSValue in this engine (tag 0 IS the undefined
       tag): every JSValue field gets its real initial value here */
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    opt = (argc > 1 && JS_IsObject(argv[1])) ? JS_DupValue(ctx, argv[1])
                                             : JS_NewObject(ctx);
    /*reject unknown keys before any option is read. */
    if (dyn_opts_strict(ctx, opt, sc_crawl_keys, 9))
        goto fail;
    /* Strict reads (audit X-12): an option getter that throws fails the
       construction with the ORIGINAL error. The unchecked read used to
       swallow it AND land max_pages at 0 -- a Crawl that emits `done`
       immediately, with no error anywhere. */
    if (fe_num_prop(ctx, opt, "maxPages", 100, &v_maxpages) ||
        fe_num_prop(ctx, opt, "maxDepth", 2, &v_maxdepth) ||
        fe_num_prop(ctx, opt, "sameHost", 1, &v_samehost) ||
        /* STRICT integer typing (the strict contract, not a coercion hint):
           "2.5" and "3" are TypeErrors, 0 and 17 are RangeErrors -- the
           lazy JS_ToInt32 read used to truncate 2.5 -> 2 and coerce "3" -> 3 */
        cr_int_prop(ctx, opt, "concurrency", 1, 1, CR_MAX_CONCURRENCY,
                    &v_conc))
        goto fail;
    c->max_pages = v_maxpages;
    c->max_depth = v_maxdepth;
    c->same_host = v_samehost ? 1 : 0;
    c->concurrency = v_conc;

    lf = JS_GetPropertyStr(ctx, opt, "linkField");
    if (JS_IsException(lf)) goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    c->link_field = strdup(s ? s : "links");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    if (!c->link_field) goto fail;
    /* <base href> support: name an extractor string field that yields the
       page's EFFECTIVE base; unset means resolve against the request url. */
    lf = JS_GetPropertyStr(ctx, opt, "baseField");
    if (JS_IsException(lf)) goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->base_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->base_field) goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "canonicalField");
    if (JS_IsException(lf)) goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->canonical_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->canonical_field) goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "relField");
    if (JS_IsException(lf)) goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->rel_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->rel_field) goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    /* meta robots: name an extractor field whose value is the page's
       <meta name="robots" content="..."> content; its nofollow gates link
       following and the directives join page.robots */
    lf = JS_GetPropertyStr(ctx, opt, "robotsField");
    if (JS_IsException(lf)) goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->robots_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->robots_field) goto fail;
    }
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    if (c->max_pages < 0) c->max_pages = 0;
    if (c->max_depth < 0) c->max_depth = 0;

    obj = dyn_res_wrap(ctx, nt, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj)) return obj;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_sweep, c);
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    cr_install_async_iterator(ctx, obj);
    return obj;

fail:
    /* every jump here owns lf (possibly UNDEFINED) and opt, and c owns its
       strdup'd field names */
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    free(c->link_field); free(c->base_field); free(c->canonical_field);
    free(c->rel_field); free(c->robots_field);
    free(c->owner);            /* the owner block's ONLY ref is c's */
    free(c);
    return JS_EXCEPTION;
}

/* start(seed[, extractor]) -> this, primed. Returning the Crawl itself keeps
   one object rather than a second iterator class to keep correct. */
/* [Symbol.asyncIterator] on EVERY crawl (ctor AND resume -- a resumed
 * instance pulled through the engine's async-from-sync wrapper spun
 * forever yielding undefined: the wrapper does not await a promise from a
 * SYNC next(), so the wrapper's partial drain is exactly the phantom-page
 * class): for-await awaits each next() result through the real async
 * protocol. With concurrency 1 the results are plain objects (resolve
 * immediately); with concurrency they are the per-page promises. The SYNC
 * [Symbol.iterator] stays on the proto for for..of (concurrency 1); an
 * async crawl must never be pulled through it. */
static void cr_install_async_iterator(JSContext *ctx, JSValue obj)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, g, "Symbol");
    JSValue ai = JS_IsObject(sym) ? JS_GetPropertyStr(ctx, sym, "asyncIterator")
                                  : JS_UNDEFINED;
    JSAtom aatom = JS_ValueToAtom(ctx, ai);
    if (aatom != JS_ATOM_NULL) {
        JSValue pages = JS_GetPropertyStr(ctx, obj, "pages");
        if (JS_IsFunction(ctx, pages))
            JS_DefinePropertyValue(ctx, obj, aatom, pages, JS_PROP_C_W_E);
        else
            JS_FreeValue(ctx, pages);
    }
    JS_FreeAtom(ctx, aatom);
    JS_FreeValue(ctx, ai);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
}

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
static JSValue cr_make_page(JSContext *ctx, cr_t *c, JSValueConst this_val,
                            const char *url, int depth, JSValue res);
static JSValue dyn_cr_next_async(JSContext *ctx, JSValueConst this_val,
                                 cr_t *c);

static JSValue dyn_cr_next(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    cr_t *c = (cr_t *)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    JSValue fetcher, res, page, out;
    const char *url;
    int depth;
    (void)argc; (void)argv;

    if (!c) return JS_EXCEPTION;
    /*with concurrency > 1 the iterator is ASYNC -- next returns a
       promise settled by whichever in-flight page completes first. */
    if (c->concurrency > 1)
        return dyn_cr_next_async(ctx, this_val, c);
    if (!c->started || c->qhead >= c->qn || c->emitted >= c->max_pages) {
        out = JS_NewObject(ctx);
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
    if (JS_IsException(res)) return JS_EXCEPTION;

    page = cr_make_page(ctx, c, this_val, url, depth, res);   /* consumes res */
    if (JS_IsException(page)) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, out, "value", page, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE, JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_cr_next_async(JSContext *ctx, JSValueConst this_val,
                                 cr_t *c);

/* The page pipeline, shared by BOTH iterator modes: build the page object
 * for a fetched response, run the extractor, queue the links, account
 * emitted. CONSUMES res. Returns the page, or JS_EXCEPTION (propagated). */
static JSValue cr_make_page(JSContext *ctx, cr_t *c, JSValueConst this_val,
                            const char *url, int depth, JSValue res)
{
    JSValue extractor, page;
    int page_status, page_nofollow = 0;

    page = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, page, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, page, "depth", JS_NewInt32(ctx, depth), JS_PROP_C_W_E);
    page_status = 0;
    {
        /* The response came from the user's fetcher: its `status` getter is
           user JS and a throw must PROPAGATE (audit X-12). The unchecked
           JS_ToInt32 also stored the exception-tagged value itself as
           page.status and kept crawling at status 0. */
        JSValue st = JS_GetPropertyStr(ctx, res, "status");
        if (JS_IsException(st) || JS_ToInt32(ctx, &page_status, st)) {
            JS_FreeValue(ctx, st);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, page, "status", st, JS_PROP_C_W_E);
    }

    /* wire-level robots directives (X-Robots-Tag), as honored for our agent */
    {
        JSValue rd = JS_GetPropertyStr(ctx, res, "robotsDirectives");
        if (JS_IsException(rd)) {       /* user getter: propagate (X-12) */
            JS_FreeValue(ctx, rd);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
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
    return page;
}


/* ----: the concurrency engine ----------------------------------------
 *
 * With concurrency > 1 the crawl keeps up to `concurrency` pages IN FLIGHT.
 * The politeness stays per host and lives where it always lived: the
 * fetcher's per-host floor (fe_host next_ok_ms) is checked and stamped per
 * dispatch, and a page whose host is still cooling down waits BEFORE its
 * exchange -- two fetches to one host still space out by minDelayMs
 * (Crawl-delay raising it, never lowering it), while fetches to different
 * hosts overlap. Parallelism comes from the transport: the exchange rides
 * the client's `requestAsync` when it has one (a real HTTPClient runs the
 * wire call on the io pool), falling back to the synchronous `request`
 * otherwise -- correct, just serialized by that client itself. The whole
 * per-page policy is the buffered get()'s, function for function: the same
 * robots gate (the shared fe_load_robots), the same floor arithmetic, the
 * same retry contract (429/5xx, Retry-After in both forms, jittered
 * backoff, a delay beyond 60s served as the final answer), the same
 * redirect chase with its private-host and https-downgrade gates, and the
 * SAME post-exchange tail (fe_finish_response) -- so a page a concurrent
 * crawl emits is a page a serial crawl would have emitted.
 *
 * Ordering, documented: next() resolves with whichever page COMPLETES
 * first, not the one submitted first -- a slow page holds only its own
 * slot while faster pages pass it. Failures (an origin that stays down
 * through its retries, a throwing extractor) reject the oldest waiting
 * next() in the same completion order.
 *
 * maxPages still bounds EMITTED pages: the dispatch budget is
 * max_pages - emitted - in_flight - completed_unpicked, so a bound of N
 * fetches at most N pages however the races land. maxDepth, sameHost and
 * the canonical/base/nofollow gates are cr_make_page's -- byte-for-byte
 * the sync path's rules, one implementation.
 *
 * Lifetime (the UAF class this module keeps front-running): a flight that
 * parks on a promise may outlive the crawl -- close() mid-flight is legal
 * and pinned below. cr_owner_t counts one ref for the crawl plus one per
 * flight and per queued waiter; a settling continuation checks the flag,
 * walks into the shared pipeline when the crawl lives, and drops its page
 * and itself when it does not. dispose() settles queued next() promises
 * as {done:true}: a closed crawl ENDS, it never leaves an await hanging.
 */

/* one refcounted liveness block, shared by the crawl, its flights and its
 * queued next() waiters */

static void cr_notify(cr_t *c);
static void cr_dispatch(cr_t *c, JSValueConst this_val);
static void cr_flight_run(cr_flight_t *fl);

#define CR_FLIGHT_MAGIC 0xCAFEF11Du

static void cr_flight_sweep(JSContext *ctx, JSRuntime *rt, void *opaque);

/* The flight memory's final goodbye: free it and drop the owner claim that
 * kept the crawl struct alive for it (unless the claim already went with
 * the release below). Runs exactly once per flight. `rt` is threaded
 * through from the caller on EVERY path: deriving it from fl->ctx would
 * read a context that a context-less teardown may already have freed --
 * the shape must not carry the hazard even where today's callers could. */
static void cr_flight_destroy(cr_flight_t *fl, JSRuntime *rt)
{
    cr_t *c = fl->c;
    int claim = !fl->claim_released;
    JS_RemoveShutdownSweep(rt, cr_flight_sweep, fl);
    JS_ShutdownUndeferFree(rt, fl);
    fl->magic = 0;
    free(fl);
    if (claim)
        cr_release(c);
}

/* Release the flight's JSValue pins (exactly once) through whichever API
 * is legal: `ctx` while JS lives, `rt` alone at the JS_FreeRuntime
 * straggler. */
static void cr_flight_release_pins(cr_flight_t *fl, JSContext *ctx,
                                   JSRuntime *rt)
{
    JSValue *pins = &fl->crawl;    /* crawl..err: CONTIGUOUS */
    int i;
    for (i = 0; i < 7; i++) {
        if (ctx)
            JS_FreeValue(ctx, pins[i]);
        else
            JS_FreeValueRT(rt, pins[i]);
        pins[i] = JS_UNDEFINED;
    }
}

/* The shared release: pins once, then either the graveyard (a settle is
 * still owed and is the ONLY legal reader of this memory) or the final
 * destroy. The graveyard keeps the SHELL allocated -- the stale settle then
 * reads a dead-flag state in live memory instead of a freed (possibly
 * REALLOCATED) flight and writing a stranger's page into whatever crawl now
 * owns the heap slot -- and registers it for the engine's end-of-
 * JS_FreeRuntime free in case that settle never lands. The owner claim goes
 * with the graveyard NOW: from here on nothing may write through fl->c, so
 * the crawl struct can die whenever its own bookkeeping says so. */
static void cr_flight_free_ex(cr_flight_t *fl, JSContext *ctx, JSRuntime *rt)
{
    if (!fl || fl->pins_released)
        return;
    fl->pins_released = 1;
    if (!rt && ctx)
        rt = JS_GetRuntime(ctx);
    JS_RemoveShutdownSweep(rt, cr_flight_sweep, fl);
    cr_flight_release_pins(fl, ctx, rt);
    if (fl->park_claims > 0) {
        fl->graveyard = 1;
        if (!fl->claim_released) {
            fl->claim_released = 1;
            cr_release(fl->c);
        }
        JS_ShutdownDeferFree(rt, fl);
        return;
    }
    cr_flight_destroy(fl, rt);
}

static void cr_flight_free(JSContext *ctx, cr_flight_t *fl)
{
    cr_flight_free_ex(fl, ctx, NULL);
}

static void cr_flight_sweep(JSContext *ctx, JSRuntime *rt, void *opaque)
{
    cr_flight_t *fl = (cr_flight_t *)opaque;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return;                      /* a stale sweep after a destroy */
    cr_flight_free_ex(fl, ctx, rt);
}

/* The engine's park: OUR settle pair rides a fresh promise and the user's
 * promise (an async sleep, or the client's requestAsync) drives it -- the
 * house our-capability-then-chain-the-user-promise pattern, whose
 * once-only settlement makes a misbehaving thenable harmless. */
static JSValue cr_flight_settle(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic,
                                JSValue *data);

static JSValue cr_flight_park(JSContext *ctx, cr_flight_t *fl, JSValue pr)
{
    JSValue funcs[2], promise, pthen, dptr, onres, onrej, tr, upthen;
    JSValueConst thenargs[2];

    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, pr);
        return JS_EXCEPTION;
    }
    dptr = JS_NewInt64(ctx, (int64_t)(intptr_t)fl);
    onres = JS_NewCFunctionData(ctx, cr_flight_settle, 1, 0, 1, &dptr);
    onrej = JS_NewCFunctionData(ctx, cr_flight_settle, 1, 1, 1, &dptr);
    JS_FreeValue(ctx, dptr);
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    thenargs[0] = onres;
    thenargs[1] = onrej;
    tr = JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onres);
    JS_FreeValue(ctx, onrej);
    if (JS_IsException(tr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, pr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, tr);
    upthen = JS_GetPropertyStr(ctx, pr, "then");
    if (JS_IsFunction(ctx, upthen)) {
        tr = JS_Call(ctx, upthen, pr, 2, (JSValueConst *)funcs);
        if (JS_IsException(tr)) {
            /* a throwing `then` leaves nobody to settle: reject OUR promise
               so the settle still fires exactly once */
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, tr);
            tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
            JS_FreeValue(ctx, tr);
            JS_FreeValue(ctx, exc);
        } else {
            JS_FreeValue(ctx, tr);
        }
    } else {
        JSValue undef = JS_UNDEFINED;
        JS_FreeValue(ctx, JS_GetException(ctx));
        tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &undef);
        JS_FreeValue(ctx, tr);
    }
    JS_FreeValue(ctx, upthen);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, promise);
    fl->pending = pr;
    fl->park_claims++;             /* the settle's claim on this flight's
                                      memory: cr_flight_free defers the
                                      free to the last settling claim */
    return JS_UNDEFINED;
}

/* A promise already settled inline: lift its value (or rejection) without
   a park. Returns 0 ok / -1 rejected (the flight fails). */
static int cr_flight_lift(JSContext *ctx, cr_flight_t *fl, JSValue pr)
{
    JSPromiseStateEnum ps = JS_PromiseState(ctx, pr);
    if (ps == JS_PROMISE_REJECTED) {
        JS_FreeValue(ctx, pr);
        return -1;
    }
    if (ps == JS_PROMISE_FULFILLED) {
        JSValue v = JS_PromiseResult(ctx, pr);
        JS_FreeValue(ctx, pr);
        if (fl->state == CR_FS_EXCHANGE) {
            JS_FreeValue(ctx, fl->res);
            fl->res = v;                 /* the response object */
            return 0;
        }
        JS_FreeValue(ctx, v);            /* a sleep resolves to undefined */
        return 0;
    }
    JS_FreeValue(ctx, pr);
    return -2;                           /* still pending (cannot happen for
                                            the engine's own sleeps) */
}

static void cr_flight_run(cr_flight_t *fl);

/* The settle: a parked sleep or exchange resolved (magic 0) or rejected
 * (magic 1). argv[0] of a fulfilled EXCHANGE is the response object.
 *
 * THE SETTLE POLICY, stated for the flight mechanics (same three
 * invariants the watch and cps sites keep, expressed through THIS state
 * machine rather than a settle guard):
 *
 *   1. NEVER settle with the JS_EXCEPTION sentinel -- capture, then
 *      reject. A failed exchange stages the sentinel on fl->res as an
 *      INTERNAL marker between state machine steps; the consumer is
 *      cr_flight_finish, which converts it to a CAPTURED exception object
 *      on fl->err (JS_GetException, or a staged TypeError) that the waiter
 *      receives as a promise REJECTION. The sentinel never becomes a
 *      settlement value and never crosses into user JS as one.
 *
 *   2. Settle EXACTLY ONCE, by claims rather than a flag: each settle
 *      consumes one park_claim, `finished` is the idempotence fence (a
 *      second completion must not unlink a stranger's FIFO entry), and
 *      `magic != CR_FLIGHT_MAGIC` walks a stale settle away from a freed
 *      flight. A graveyarded flight settles into nothing but its own
 *      destroy -- no user JS runs on it, ever.
 *
 *   3. SWALLOW at the event-loop boundary: this callback returns
 *      JS_UNDEFINED on every path (JS_ToInt64's JS_EXCEPTION included --
 *      the data slot cannot throw usefully into a settle). Failures travel
 *      as captured values into fl->err and reach the waiter through the
 *      promise rejection cr_notify resolves it with; nothing re-throws
 *      into the loop that dispatched the settle.
 *
 * (The graveyard ordering -- a stale settle reads the dead flag in live
 * memory, never freed memory -- is stated once at cr_flight_free_ex and
 * is exactly what the JS_ShutdownDeferFree there provides.) */
static JSValue cr_flight_settle(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic,
                                JSValue *data)
{
    int64_t p = 0;
    cr_flight_t *fl;
    (void)this_val; (void)argc;
    if (JS_ToInt64(ctx, &p, data[0]))
        return JS_EXCEPTION;
    fl = (cr_flight_t *)(intptr_t)p;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return JS_UNDEFINED;    /* a stale settle racing a freed flight */
    if (fl->park_claims > 0)
        fl->park_claims--;      /* this settle consumes the park's claim */
    if (fl->graveyard) {
        /* the flight was finished and freed-pending while this settle was
           owed: NOTHING on it may run but the last claimant's free */
        if (fl->park_claims == 0)
            cr_flight_destroy(fl, JS_GetRuntime(ctx));
        return JS_UNDEFINED;
    }
    if (fl->finished)
        return JS_UNDEFINED;    /* a stale settle on a finished flight: the
                                   engine's once-only rule makes this
                                   impossible for one chain -- the guard
                                   keeps it that way */
    JS_FreeValue(ctx, fl->pending);
    fl->pending = JS_UNDEFINED;
    if (fl->state == CR_FS_EXCHANGE) {
        JS_FreeValue(ctx, fl->res);
        if (magic)
            fl->res = JS_EXCEPTION;      /* failed exchange == thrown request */
        else
            fl->res = JS_DupValue(ctx, argv[0]);
        fl->state = CR_FS_DECIDE;        /* the exchange LANDED: decide --
                                            never re-issue the request */
    }
    cr_flight_run(fl);
    return JS_UNDEFINED;
}

/* The engine's floor/backoff wait: the blocking poll sleep. STATED
   HONESTLY: the first cut parked the flight on the global sleep() promise,
   and that park had a lost-wakeup race (a page silently vanishing under
   concurrent floor waits). The blocking wait is the honest fallback: it
   only runs when a host is HOT (per-host floors), the wire work of OTHER
   hosts proceeds on the io pool meanwhile, and it is exactly get()'s
   wait shape -- the serial special case. Returns 0 (never parks). */
static int cr_flight_sleep(cr_flight_t *fl, double ms)
{
    (void)fl;
    fe_sleep_ms(ms > 60000 ? 60000 : ms);
    return 0;
}

/* Hand a finished flight's outcome over: a page into the done FIFO (or an
   error for the oldest waiter), then notify. Runs with crawl_alive. */
static void cr_notify(cr_t *c);

static void cr_flight_finish(cr_flight_t *fl, int failed)
{
    JSContext *ctx = fl->ctx;
    cr_t *c = fl->c;
    cr_owner_t *own = (cr_owner_t *)c->owner;
    JSValue page = JS_UNDEFINED;

    if (fl->finished)
        return;                      /* IDEMPOTENT: a second settle of a
                                        finished flight (the exact
                                        double-completion class the count
                                        audit below caught) must not unlink
                                        a stranger and corrupt n_flights */
    fl->finished = 1;
    if (failed) {
        if (JS_IsException(fl->res)) {
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, fl->err);
                fl->err = JS_GetException(ctx);  /* the request's own throw */
            } else if (JS_IsUndefined(fl->err)) {
                /* the EXCEPTION marker with nothing pending: the failure
                   was already staged on fl->err (or invent one) -- never
                   overwrite a staged error with an empty exception */
                fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
                fl->err = JS_GetException(ctx);
            }
            fl->res = JS_UNDEFINED;
        } else if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);      /* a HOP-path throw: a bad
                                                    url, the redirect gates */
        } else if (JS_IsUndefined(fl->err)) {
            fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
            fl->err = JS_GetException(ctx);
        }
    }
    /* unlink from the live list */
    {
        cr_flight_t **pp = &c->flights;
        while (*pp && *pp != fl) pp = &(*pp)->next;
        if (*pp) {
            *pp = fl->next;
            c->n_flights--;          /* only when actually linked */
        }
        /* the pin is released at the END of finish: the refuel dispatch
           below still needs a live crawl object to dup into new flights */
    }
    if (!own->crawl_alive) {
        /* the crawl was closed mid-flight: nobody will read this page */
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    if (!failed) {
        page = cr_make_page(ctx, c, fl->crawl, fl->cur, fl->depth,
                            fl->res);        /* consumes res */
        fl->res = JS_UNDEFINED;
    }
    if (failed || JS_IsException(page)) {
        if (!failed) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);  /* the extractor threw */
        }
        JS_FreeValue(ctx, page);
    } else {
        fl->res = page;                      /* rides to the waiter */
    }
    /* The extractor is USER JS: it may have closed the crawl just now. A
       dead crawl's completion goes nowhere -- dropping it here is what
       keeps late completions out of a disposed instance's FIFOs (which the
       heap may already have re-issued to a different crawl). */
    if (!own->crawl_alive) {
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    /* into the completion FIFO (link field is free -- off the live list) */
    fl->next = NULL;
    if (c->done_tail) c->done_tail->next = fl;
    else c->done_head = fl;
    c->done_tail = fl;
    c->n_done++;
    {
        cr_notify(c);
        cr_dispatch(c, c->self_pending);   /* the pin: still held here */
        /* last flight out AND nothing refueled: drop the pin */
        if (c->n_flights == 0 && !JS_IsUndefined(c->self_pending)) {
            JS_FreeValue(ctx, c->self_pending);
            c->self_pending = JS_UNDEFINED;
        }
    }
}

/* The flight's fetcher died mid-run (a close() ran in user JS -- a mock's
 * request, a response getter, the extractor): stage a clean failure ON THIS
 * FLIGHT and finish it. The error travels to the flight's own waiter (or is
 * dropped with its dead crawl); it never surfaces anywhere else. */
static void cr_flight_fe_gone(cr_flight_t *fl)
{
    JSContext *ctx = fl->ctx;
    JS_FreeValue(ctx, fl->err);
    fl->err = JS_ThrowTypeError(ctx,
        "Crawl: the fetcher was closed while the crawl was in flight");
    fl->err = JS_GetException(ctx);
    cr_flight_finish(fl, 1);
}

/* The state machine: run until the flight parks, finishes or fails. */
static void cr_flight_run(cr_flight_t *fl)
{
    JSContext *ctx = fl->ctx;
    fe_t *f = fe_live(ctx, fl->fetcher);

    if (!f) {
        cr_flight_fe_gone(fl);
        return;
    }

    for (;;) {
        /* Re-resolved EVERY entry: the previous iteration (or the settle
           that landed us here) ran user JS that may have closed the
           fetcher -- `f` is never carried across a user-JS boundary. */
        f = fe_live(ctx, fl->fetcher);
        if (!f) {
            cr_flight_fe_gone(fl);
            return;
        }
        switch (fl->state) {
        case CR_FS_HOP: {
            fe_host_t *h;
            double wait, floor_ms;

            fl->https = fe_split(fl->cur, fl->host, sizeof fl->host,
                                 &fl->path);
            if (fl->https < 0) {
                JS_ThrowTypeError(ctx,
                    "Fetcher: only http:// and https:// urls (got %.60s)",
                    fl->cur);
                cr_flight_finish(fl, 1);
                return;
            }
            if (!f->allow_private_hosts && fe_host_is_private(fl->host)) {
                JS_ThrowTypeError(ctx,
                    "Fetcher: %s://%.60s is a private/loopback/link-local "
                    "host; pass allowPrivateHosts: true to fetch it",
                    fl->https ? "https" : "http", fl->host);
                cr_flight_finish(fl, 1);
                return;
            }
            h = fe_host(f, fl->host);
            if (!h) { JS_ThrowOutOfMemory(ctx); cr_flight_finish(fl, 1); return; }

            /* robots, before anything is requested (the SHARED gate) */
            if (f->robots_on) {
                double now_ms = (double)dyn_timer_now_ms();
                if (!h->robots_tried || now_ms >= h->robots_next_ms) {
                    if (fe_load_robots(ctx, f, fl->fetcher, fl->client,
                                       fl->uh, h, fl->https) < 0) {
                        cr_flight_finish(fl, 1);   /* user getter: propagate */
                        return;
                    }
                    /* fe_load_robots ran the user's request: `f` (and its
                       host table -- `h`) is valid again only if the fetcher
                       still lives */
                    f = fe_live(ctx, fl->fetcher);
                    if (!f) {
                        cr_flight_fe_gone(fl);
                        return;
                    }
                }
                if (h->robots_unreachable && h->robots_ok)
                    h->robots_unreachable = 0;
                if (h->robots_unreachable ||
                    (h->robots &&
                     !rb_allows_path(h->robots, fl->path, strlen(fl->path)))) {
                    /* the skip IS a response, as in get(): status 0 */
                    f->skipped_robots++;
                    JS_FreeValue(ctx, fl->res);
                    fl->res = JS_NewObject(ctx);
                    JS_DefinePropertyValueStr(ctx, fl->res, "status",
                                              JS_NewInt32(ctx, 0), JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, fl->res, "url",
                                              JS_NewString(ctx, fl->cur),
                                              JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, fl->res, "skippedByRobots",
                                              JS_TRUE, JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, fl->res, "contentType",
                                              JS_NewString(ctx, ""),
                                              JS_PROP_C_W_E);
                    JS_DefinePropertyValueStr(ctx, fl->res, "body",
                                              JS_NewString(ctx, ""),
                                              JS_PROP_C_W_E);
                    cr_flight_finish(fl, 0);
                    return;
                }
            }

            /* the delay floor. Crawl-delay raises it, NEVER lowers it.
               Under concurrency the slot is claimed HERE, at dispatch:
               next_ok moves to now+floor for every flight that passes the
               gate, so same-host fetches START at least floor apart no
               matter how many slots are free (get()'s stamp-after-exchange
               is the serial special case of the same rule). */
            floor_ms = f->min_delay_ms;
            if (h->robots && h->robots->delay > 0) {
                double cd = h->robots->delay * 1000.0;
                if (cd > floor_ms) floor_ms = cd;
            }
            fl->floor_ms = floor_ms;
            if (!fl->floor_claimed) {
                /* claim the host slot ONCE, SEQUENTIALLY: the flight's slot
                   starts after everything already claimed (max(now,
                   next_ok)), and extends next_ok by the floor -- two
                   same-host flights dispatched together start a floor
                   APART, which is the whole politeness point. A re-entry
                   after the floor sleep must NOT re-claim: next_ok read
                   again would re-stamp a floor into the future -- an
                   infinite politeness loop. */
                double now = (double)dyn_timer_now_ms();
                double base = h->next_ok_ms > now ? h->next_ok_ms : now;
                wait = base - now;
                h->next_ok_ms = base + floor_ms;
                fl->floor_claimed = 1;
            } else {
                wait = h->next_ok_ms - (double)dyn_timer_now_ms();
            }
            if (wait > 0) {
                f->throttled_ms += wait;
                if (cr_flight_sleep(fl, wait)) return;   /* parked */
            }
            fl->state = CR_FS_EXCHANGE;
            continue;
        }
        case CR_FS_EXCHANGE: {
            JSValue raf;

            if (JS_IsException(fl->res)) {
                /* an async exchange that rejected == a sync request that
                   threw: get()'s retry contract below decides */
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, fl->res);
                fl->res = JS_UNDEFINED;
                fl->st = 0;
            } else {
                raf = JS_GetPropertyStr(ctx, fl->client, "requestAsync");
                {
                    /* The closure is only a TYPE TEST here (the call goes
                       through the property name) -- and it must not outlive
                       this block: a JS closure pins its function's realm, so
                       a leaked one kept the dead module scope, and through
                       it the whole context graph, alive at teardown (the
                       gc_obj_list assertion on every parked-async crawl). */
                    int raf_fn = JS_IsFunction(ctx, raf);
                    JS_FreeValue(ctx, raf);
                    raf = JS_UNDEFINED;
                    if (!raf_fn)
                        goto cr_sync_fallback;
                }
                if (1) {
                    /* the offloaded wire call: same method/url/headers
                       fe_raw assembles for the sync path */
                    JSValue hdrs, a[4];
                    JSAtom m = JS_NewAtom(ctx, "requestAsync");
                    hdrs = JS_NewObject(ctx);
                    if (JS_IsObject(fl->uh)) {
                        JSPropertyEnum *tab = NULL;
                        uint32_t tn = 0, i;
                        if (JS_GetOwnPropertyNames(ctx, &tab, &tn, fl->uh,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                            for (i = 0; i < tn; i++) {
                                JSValue v = JS_GetProperty(ctx, fl->uh,
                                                           tab[i].atom);
                                if (!JS_IsException(v))
                                    JS_DefinePropertyValue(ctx, hdrs,
                                        tab[i].atom, v, JS_PROP_C_W_E);
                                else
                                    JS_FreeValue(ctx, JS_GetException(ctx));
                            }
                            for (i = 0; i < tn; i++)
                                JS_FreeAtom(ctx, tab[i].atom);
                            js_free(ctx, tab);
                        }
                    }
                    JS_DefinePropertyValueStr(ctx, hdrs, "User-Agent",
                                              JS_NewString(ctx, f->agent),
                                              JS_PROP_C_W_E);
                    a[0] = JS_NewString(ctx, "GET");
                    a[1] = JS_NewString(ctx, fl->cur);
                    a[2] = JS_UNDEFINED;
                    a[3] = hdrs;
                    fl->res = JS_Invoke(ctx, fl->client, m, 4,
                                        (JSValueConst *)a);
                    JS_FreeAtom(ctx, m);
                    JS_FreeValue(ctx, a[0]);
                    JS_FreeValue(ctx, a[1]);
                    JS_FreeValue(ctx, a[3]);
                    if (JS_IsException(fl->res)) {
                        /* the sync contract: a THROWN request retries (the
                           exception is swallowed into the retry decision) */
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        fl->res = JS_UNDEFINED;
                        fl->st = 0;
                    } else {
                        JSValue pr = fl->res;
                        fl->res = JS_UNDEFINED;
                        if (JS_PromiseState(ctx, pr) == JS_PROMISE_PENDING) {
                                                    if (JS_IsException(cr_flight_park(ctx, fl, pr)))
                                cr_flight_finish(fl, 1);
                            return;              /* parked on the wire: the
                                                       settle moves to DECIDE */
                        }
                        if (cr_flight_lift(ctx, fl, pr) < 0) {
                            JS_FreeValue(ctx, fl->res);
                            fl->res = JS_UNDEFINED;
                            fl->st = 0;
                        }
                    }
                } else {
cr_sync_fallback:
                    /* the sync fallback: the injected mock's contract */
                    fl->res = fe_raw(ctx, f, fl->client, fl->cur, fl->uh,
                                     NULL, NULL);
                    if (JS_IsException(fl->res)) {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        fl->res = JS_UNDEFINED;
                        fl->st = 0;
                    }
                }
            }
            /* sync or inline-fulfilled exchange: go decide */
            fl->state = CR_FS_DECIDE;
            continue;
        }
        case CR_FS_DECIDE: {
                    if (!JS_IsUndefined(fl->res)) {
                fl->st = fe_status(ctx, fl->res);
                if (fl->st < 0) {
                    /* the response's status getter threw: propagate, no
                       retry (audit X-12) */
                    JS_FreeValue(ctx, fl->res);
                    fl->res = JS_UNDEFINED;
                    cr_flight_finish(fl, 1);
                    return;
                }
                /* the status getter is user JS: `f` lives only if the
                   fetcher still does */
                f = fe_live(ctx, fl->fetcher);
                if (!f) {
                    cr_flight_fe_gone(fl);
                    return;
                }
            }
            /* get()'s retry decision */
            if (fl->st != 0 && !(fl->st == 429 ||
                                 (fl->st >= 500 && fl->st < 600)))
                break;                          /* not a retry case */
            if (fl->attempt >= f->retries)
                break;                          /* served as the final answer */
            {
                double back = f->min_delay_ms *
                              (double)(1 << (fl->attempt < 10 ? fl->attempt
                                                               : 10));
                back *= 0.75 + 0.5 * fe_rand01(f);
                if (fl->st != 0 && !JS_IsUndefined(fl->res)) {
                    JSValue hold = JS_UNDEFINED;
                    const char *ra = fe_header(ctx, fl->res, "retry-after",
                                               &hold);
                    if (ra) {
                        double secs = strtod(ra, NULL);
                        if (!(secs > 0)) {
                            int64_t when = fe_http_date(ra);
                            if (when > 0) {
                                int64_t now = (int64_t)time(NULL);
                                secs = when > now ? (double)(when - now)
                                                  : 0.0;
                            }
                        }
                        if (secs > 0) back = secs * 1000.0;
                        JS_FreeCString(ctx, ra);
                    }
                    JS_FreeValue(ctx, hold);
                    /* the Retry-After read touched user getters */
                    f = fe_live(ctx, fl->fetcher);
                    if (!f) {
                        cr_flight_fe_gone(fl);
                        return;
                    }
                }
                if (back > 60000.0)
                    break;                      /* an instruction, not a nap */
                JS_FreeValue(ctx, fl->res);
                fl->res = JS_UNDEFINED;
                f->retried++;
                fl->attempt++;
                fl->state = CR_FS_BACKOFF;
                if (cr_flight_sleep(fl, back)) return;   /* parked */
                fl->state = CR_FS_EXCHANGE;
                continue;
            }
        }
        case CR_FS_BACKOFF:                     /* slept out: exchange again */
            fl->state = CR_FS_EXCHANGE;
            continue;
        default:
                    cr_flight_finish(fl, 1);
            return;
        }
        /* the retry/redirect gate fell through: redirects next */
        if (fl->st >= 300 && fl->st < 400 && !JS_IsUndefined(fl->res)) {
            JSValue hold = JS_UNDEFINED;
            const char *loc = fe_header(ctx, fl->res, "location", &hold);
            /* the Location read touched user getters */
            f = fe_live(ctx, fl->fetcher);
            if (!f) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                cr_flight_fe_gone(fl);
                return;
            }
            if (loc && fl->hop < f->max_redirects) {
                char nxt[sizeof fl->cur];
                int prev_https = fe_scheme_is_https(fl->cur);
                if (fe_resolve(fl->cur, loc, nxt, sizeof nxt) > 0 &&
                    strcmp(nxt, fl->cur)) {
                    char lh[300];
                    const char *lp;
                    if (prev_https && !fe_scheme_is_https(nxt) &&
                        !f->allow_insecure_downgrade) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, fl->res);
                        fl->res = JS_UNDEFINED;
                        JS_ThrowRangeError(ctx,
                            "Fetcher: redirect downgrades https to http "
                            "(%.80s); pass allowInsecureDowngrade: true to "
                            "follow it", nxt);
                        cr_flight_finish(fl, 1);
                        return;
                    }
                    if (fe_split(nxt, lh, sizeof lh, &lp) >= 0 &&
                        !f->allow_private_hosts && fe_host_is_private(lh)) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, fl->res);
                        fl->res = JS_UNDEFINED;
                        JS_ThrowTypeError(ctx,
                            "Fetcher: redirect target %.60s is a private/"
                            "loopback/link-local host; pass "
                            "allowPrivateHosts: true to fetch it", lh);
                        cr_flight_finish(fl, 1);
                        return;
                    }
                    snprintf(fl->cur, sizeof fl->cur, "%s", nxt);
                    JS_FreeCString(ctx, loc);
                    JS_FreeValue(ctx, hold);
                    JS_FreeValue(ctx, fl->res);
                    fl->res = JS_UNDEFINED;
                    fl->hop++;
                    fl->attempt = 0;
                    fl->state = CR_FS_HOP;
                    continue;
                }
                /* unresolvable Location: serve this response as final */
            } else if (fl->hop >= f->max_redirects) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, fl->res);
                fl->res = JS_UNDEFINED;
                JS_ThrowRangeError(ctx,
                    "Fetcher: more than %d redirects", f->max_redirects);
                cr_flight_finish(fl, 1);
                return;
            }
            if (loc) { JS_FreeCString(ctx, loc); JS_FreeValue(ctx, hold); }
        }
        /* final: the shared tail, then the page */
        fl->res = fe_finish_response(ctx, fl->fetcher, f, fl->cur,
                                     fl->res, fl->st);
        if (JS_IsException(fl->res)) {
            cr_flight_finish(fl, 1);
            return;
        }
        cr_flight_finish(fl, 0);
        return;
    }
}

/* Settle queued next() promises from the completion FIFO; leftover
   completions stay queued for the next next(). A failure completes like a
   page -- it REJECTS the oldest waiter. */
static void cr_notify(cr_t *c)
{
    JSContext *ctx = c->ctx;
    while (c->wait_head && c->done_head) {
        cr_next_wait_t *w = c->wait_head;
        cr_flight_t *fl = c->done_head;
        JSValue out, r;
        c->wait_head = w->next;
        if (!c->wait_head) c->wait_tail = NULL;
        c->done_head = fl->next;
        if (!c->done_head) c->done_tail = NULL;
        c->n_done--;
            if (!JS_IsUndefined(fl->err)) {
            JSValueConst ea[1] = { fl->err };
            r = JS_Call(ctx, w->reject, JS_UNDEFINED, 1, ea);
        } else {
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "value", fl->res,
                                      JS_PROP_C_W_E);
            fl->res = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE,
                                      JS_PROP_C_W_E);
            r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1,
                        (JSValueConst *)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, w->resolve);
        JS_FreeValue(ctx, w->reject);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        free(w);
        cr_release(c);              /* the waiter's owner claim */
    }
    /* everything drained and nothing left running: the remaining waiters
       (queued racing the last completion) are DONE */
    if (!c->done_head && c->n_flights == 0 &&
        (!c->started || c->qhead >= c->qn || c->emitted >= c->max_pages)) {
        while (c->wait_head) {
            cr_next_wait_t *w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            if (!c->wait_head) c->wait_tail = NULL;
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
                                      JS_PROP_C_W_E);
            r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1,
                        (JSValueConst *)&out);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, w->resolve);
            JS_FreeValue(ctx, w->reject);
            free(w);
            cr_release(c);          /* the waiter's owner claim */
        }
        c->wait_tail = NULL;
    }
}

/* Start flights while budget and frontier allow. `this_val` is the crawl
 * object the iterator is being driven from; the first live flight pins it
 * (self_pending), the last release drops the pin. */
static void cr_dispatch(cr_t *c, JSValueConst this_val)
{
    JSContext *ctx = c->ctx;
    cr_owner_t *own = (cr_owner_t *)c->owner;

    while (own->crawl_alive &&
           c->n_flights < (size_t)c->concurrency &&
           c->qhead < c->qn &&
           (size_t)c->emitted + c->n_flights + c->n_done <
               (size_t)c->max_pages) {
        cr_flight_t *fl = (cr_flight_t *)calloc(1, sizeof *fl);
        char *u;
        if (!fl) return;
        own->refs++;                 /* the flight's own claim */
        fl->ctx = ctx;
        fl->c = c;
        fl->crawl = JS_DupValue(ctx, this_val);
        fl->res = JS_UNDEFINED;
        fl->pending = JS_UNDEFINED;
        fl->err = JS_UNDEFINED;
        fl->fetcher = JS_UNDEFINED;
        fl->client = JS_UNDEFINED;
        fl->uh = JS_UNDEFINED;
        if (c->n_flights == 0 && JS_IsUndefined(c->self_pending))
            /* first live flight pins the crawl (the self_pending pattern);
               the guard keeps a refuel dispatch from overwriting a still
               held pin -- that overwrite leaked the crawl object per wave */
            c->self_pending = JS_DupValue(ctx, this_val);
        /* The flight is LINKED before anything that runs user JS: a
           throwing getter then fails THIS flight down the normal finish
           path (its error reaches a waiter) instead of orphaning an
           unlinked flight whose error vanished and whose claim leaked. */
        fl->state = CR_FS_HOP;
        fl->magic = CR_FLIGHT_MAGIC;
        JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_flight_sweep, fl);
        fl->seq = ++c->flight_seq;
        u = c->q[c->qhead].url;
        fl->depth = c->q[c->qhead].depth;
        c->q[c->qhead].url = NULL;   /* ownership moves to the flight --
                                        dispose walks the WHOLE q and would
                                        double-free the popped entry */
        c->qhead++;
        snprintf(fl->cur, sizeof fl->cur, "%s", u);
        free(u);
        fl->next = c->flights;
        c->flights = fl;
        c->n_flights++;
        /* These reads run user getters, so they finish BEFORE the crawl
           handle `c` is used again: a getter may close the crawl. */
        fl->fetcher = JS_GetPropertyStr(ctx, this_val, "_fetcher");
        if (!JS_IsException(fl->fetcher))
            fl->client = JS_GetPropertyStr(ctx, fl->fetcher, "_client");
        if (!JS_IsException(fl->fetcher) && !JS_IsException(fl->client))
            fl->uh = JS_GetPropertyStr(ctx, fl->fetcher, "_headers");
        if (JS_IsException(fl->fetcher) || JS_IsException(fl->client)
            || JS_IsException(fl->uh)) {
            /* a getter threw: fail this flight with the pending exception
               (dispatch is void -- the error travels on the flight) */
            cr_flight_finish(fl, 1);
            if (!own->crawl_alive) return;
            continue;
        }
        if (!own->crawl_alive) {
            /* the getters ran user JS that closed the crawl: finish the
               flight now (the dead path drops it) instead of fetching for
               a crawl nobody will read */
            cr_flight_finish(fl, 1);
            return;
        }
            cr_flight_run(fl);
        /* the flight ran user JS (the extractor, a mock's client): a
           close() in there ends the refuel for good */
        if (!own->crawl_alive) return;
    }
}

/* next() for concurrency > 1: a Promise<IteratorResult>. */
static JSValue dyn_cr_next_async(JSContext *ctx, JSValueConst this_val,
                                 cr_t *c)
{
    JSValue funcs[2], promise;

    if (!JS_IsUndefined(c->pending_err)) {
        JSValueConst ea[1] = { c->pending_err };
        JSValue r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx, c->pending_err);
            c->pending_err = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, ea);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, c->pending_err);
        c->pending_err = JS_UNDEFINED;
        return promise;
    }
    /* a completed page: hand it over */
    if (c->done_head) {
            cr_flight_t *fl = c->done_head;
        JSValue out, r;
        c->done_head = fl->next;
        if (!c->done_head) c->done_tail = NULL;
        c->n_done--;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise)) {
            if (!JS_IsUndefined(fl->err))
                JS_FreeValue(ctx, JS_GetException(ctx));
            cr_flight_free(ctx, fl);
            return promise;
        }
        if (!JS_IsUndefined(fl->err)) {
            JSValueConst ea[1] = { fl->err };
            r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, ea);
        } else {
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "value", fl->res,
                                      JS_PROP_C_W_E);
            fl->res = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE,
                                      JS_PROP_C_W_E);
            r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1,
                        (JSValueConst *)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        cr_flight_free(ctx, fl);
        return promise;
    }
    /* finished? */
    if (!c->started || (c->qhead >= c->qn && c->n_flights == 0) ||
        c->emitted >= c->max_pages) {
        JSValue out, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        out = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
                                  JS_PROP_C_W_E);
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&out);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    /* A DEAD crawl (cr_dispose ran, or the shutdown sweep detached its
       state) must NOT park: cr_dispatch is gated off for it and cr_sweep is
       no longer registered (only ctor/resume register it), so a fresh
       waiter's resolve/reject would sit pinned on wait_head with nothing in
       the engine left to ever release them -- a drain-time retrying .catch
       calling next() again used to abort the teardown (exit 134). Reject
       cleanly instead: the caller's .catch observes it like any other
       rejection, and no pin outlives the call. */
    if (!((cr_owner_t *)c->owner)->crawl_alive) {
        JSValue exc, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        exc = JS_ThrowTypeError(ctx, "Crawl: next() on a closed crawl");
        exc = JS_GetException(ctx);
        r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    /* park the caller: a deferred next(), settled by a completion */
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    {
        cr_next_wait_t *w = (cr_next_wait_t *)malloc(sizeof *w);
        cr_owner_t *own = (cr_owner_t *)c->owner;
        if (!w) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        own->refs++;                 /* the waiter's claim */
        w->resolve = funcs[0];
        w->reject = funcs[1];
        w->next = NULL;
        if (c->wait_tail) c->wait_tail->next = w;
        else c->wait_head = w;
        c->wait_tail = w;
    }
    cr_dispatch(c, this_val);
    /* the dispatch may have completed a page synchronously (a mock client):
       cr_notify already resolved us through the queue */
    return promise;
}

static JSValue dyn_cr_self(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{ (void)argc; (void)argv; return JS_DupValue(ctx, this_val); }

/* ----: serialize -> string, static Crawl.resume(fetcher, state) ---- */

static JSValue dyn_cr_serialize(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    cr_t *c = (cr_t *)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    sc_sb_t b;
    JSValue out;
    size_t i;
    (void)argc; (void)argv;

    if (!c) return JS_EXCEPTION;
    if (!c->started)
        return JS_ThrowTypeError(ctx,
            "Crawl.serialize: the crawl has no state yet -- start(seed) first");
    sc_sb_init(&b);
    sc_sb_puts(&b, "{\"v\":");
    {
        char num[16];
        snprintf(num, sizeof num, "%d", CR_STATE_VERSION);
        sc_sb_puts(&b, num);
    }
    cr_serialize_u64(&b, "maxPages", (uint64_t)c->max_pages);
    cr_serialize_u64(&b, "maxDepth", (uint64_t)c->max_depth);
    cr_serialize_u64(&b, "sameHost", (uint64_t)c->same_host);
    cr_serialize_u64(&b, "concurrency", (uint64_t)c->concurrency);
    cr_serialize_u64(&b, "emitted", (uint64_t)c->emitted);
    cr_serialize_u64(&b, "dropped", (uint64_t)c->dropped);
    cr_serialize_str(&b, "seedHost", c->seed_host);
    cr_serialize_str(&b, "linkField", c->link_field);
    cr_serialize_str(&b, "baseField", c->base_field);
    cr_serialize_str(&b, "canonicalField", c->canonical_field);
    cr_serialize_str(&b, "relField", c->rel_field);
    cr_serialize_str(&b, "robotsField", c->robots_field);
    /* The frontier in FIFO order, from qhead: [url, depth] pairs. A
       resumed crawl must offer the SAME pages in the SAME order. */
    sc_sb_puts(&b, ",\"q\":[");
    for (i = c->qhead; i < c->qn; i++) {
        char d[16];
        if (i > c->qhead) sc_sb_put(&b, ",", 1);
        sc_sb_put(&b, "[", 1);
        sc_sb_json_str(&b, c->q[i].url);
        snprintf(d, sizeof d, ",%d]", c->q[i].depth);
        sc_sb_puts(&b, d);
    }
    sc_sb_puts(&b, "],\"seen\":[");
    for (i = 0; i < c->sn; i++) {
        if (i) sc_sb_put(&b, ",", 1);
        sc_sb_json_str(&b, c->seen[i]);
    }
    sc_sb_puts(&b, "]}");
    if (b.oom) {
        sc_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    sc_sb_free(&b);
    return out;
}

/* Validate + copy one string field out of the state object. Returns 0 with
   *out a fresh strdup ("" allowed), -1 with a TypeError pending, or -2 when
   the key is absent/null (caller decides whether that is allowed). */
static int cr_state_str(JSContext *ctx, JSValueConst o, const char *key,
                        char **out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    const char *s;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return -2;
    }
    s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    JS_FreeValue(ctx, v);
    if (!s)
        { JS_ThrowTypeError(ctx, "Crawl.resume: \"%s\" must be a string", key);
          return -1; }
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out)
        { JS_ThrowOutOfMemory(ctx); return -1; }
    return 0;
}

static JSValue dyn_cr_resume(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    cr_t *c;
    JSValue st, obj;
    JSValueConst nt = JS_UNDEFINED;
    int64_t v64;
    uint32_t i, nq = 0, nsn = 0;
    int rc;
    (void)this_val;

    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "Crawl.resume(fetcher, state[, extractor[, parse]]): the state is "
            "the string serialize() returned");
    {
        size_t slen;
        const char *sj = JS_ToCStringLen(ctx, &slen, argv[1]);
        if (!sj)
            return JS_EXCEPTION;
        st = JS_ParseJSON(ctx, sj, slen, "<crawl-state>");
        JS_FreeCString(ctx, sj);
    }
    if (JS_IsException(st))
        return JS_ThrowTypeError(ctx,
            "Crawl.resume: state is not valid JSON -- refusing to half-restore");
    if (!JS_IsObject(st)) {
        JS_FreeValue(ctx, st);
        return JS_ThrowTypeError(ctx, "Crawl.resume: state must be a JSON object");
    }

    c = (cr_t *)calloc(1, sizeof(*c));
    if (!c) { JS_FreeValue(ctx, st); return JS_ThrowOutOfMemory(ctx); }
    c->concurrency = 1;

#define CR_FAIL(msg) do { JS_ThrowTypeError(ctx, "Crawl.resume: " msg); goto fail; } while (0)

    /* Version first: an older/newer envelope must not decode as garbage. */
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "v");
        int32_t vv = 0;
        int bad = JS_IsException(v) || JS_ToInt32(ctx, &vv, v);
        JS_FreeValue(ctx, v);
        if (bad) goto fail_pending;
        if (vv != CR_STATE_VERSION)
            CR_FAIL("state version mismatch (wrong runtime or corrupt file)");
    }
    {
        int v_mp, v_md, v_sh, v_cc;
        if (fe_num_prop(ctx, st, "maxPages", 100, &v_mp) ||
            fe_num_prop(ctx, st, "maxDepth", 2, &v_md) ||
            fe_num_prop(ctx, st, "sameHost", 1, &v_sh) ||
            fe_num_prop(ctx, st, "concurrency", 1, &v_cc))
            goto fail_pending;
        if (v_mp < 0 || v_md < 0 || v_cc < 1 || v_cc > CR_MAX_CONCURRENCY)
            CR_FAIL("state bounds out of range");
        c->max_pages = v_mp;
        c->max_depth = v_md;
        c->same_host = v_sh ? 1 : 0;
        c->concurrency = v_cc;
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "emitted");
        int bad = JS_IsException(v) || JS_ToInt64(ctx, &v64, v);
        JS_FreeValue(ctx, v);
        if (bad) goto fail_pending;
        if (v64 < 0 || v64 > (int64_t)c->max_pages)
            CR_FAIL("emitted counter out of range");
        c->emitted = (int)v64;
        v = JS_GetPropertyStr(ctx, st, "dropped");
        bad = JS_IsException(v) || JS_ToInt64(ctx, &v64, v);
        JS_FreeValue(ctx, v);
        if (bad) goto fail_pending;
        if (v64 < 0) CR_FAIL("dropped counter out of range");
        c->dropped = (size_t)v64;
    }
    /* seedHost is REQUIRED: the sameHost gate is meaningless without it, and
       its presence proves the state came from a started crawl. */
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "seedHost");
        const char *s;
        if (JS_IsException(v)) goto fail_pending;
        s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        if (!s) {
            JS_FreeValue(ctx, v);
            CR_FAIL("state has no seedHost (never started?)");
        }
        {
            size_t sl = strlen(s);
            if (sl + 1 > sizeof c->seed_host || sl == 0) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            CR_FAIL("seedHost out of range");
            }
        }
        memcpy(c->seed_host, s, strlen(s) + 1);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }
    /* Field names: linkField required (the crawl cannot extract without it);
       the others keep their "absent means default/none" shapes. */
    {
        rc = cr_state_str(ctx, st, "linkField", &c->link_field);
        if (rc == -1) goto fail_pending;
        if (rc == -2) CR_FAIL("state has no linkField");
        if (!c->link_field[0]) CR_FAIL("linkField is empty");
        rc = cr_state_str(ctx, st, "baseField", &c->base_field);
        if (rc == -1) goto fail_pending;
        if (rc == -2 && c->base_field) { free(c->base_field); c->base_field = NULL; }
        rc = cr_state_str(ctx, st, "canonicalField", &c->canonical_field);
        if (rc == -1) goto fail_pending;
        rc = cr_state_str(ctx, st, "relField", &c->rel_field);
        if (rc == -1) goto fail_pending;
        rc = cr_state_str(ctx, st, "robotsField", &c->robots_field);
        if (rc == -1) goto fail_pending;
    }
    /* The frontier: [[url, depth], ...]. Every entry is re-checked against
       the same bounds start() enforces (url fits the 1024 request buffer,
       depth >= 0, count within CR_MAX_PENDING). */
    {
        JSValue q = JS_GetPropertyStr(ctx, st, "q");
        JSValue qlen;
        if (JS_IsException(q)) goto fail_pending;
        if (!JS_IsArray(ctx, q)) {
            JS_FreeValue(ctx, q);
            CR_FAIL("state.q must be an array");
        }
        qlen = JS_GetPropertyStr(ctx, q, "length");
        if (JS_IsException(qlen) || JS_ToUint32(ctx, &nq, qlen)) {
            JS_FreeValue(ctx, qlen);
            JS_FreeValue(ctx, q);
            goto fail_pending;
        }
        JS_FreeValue(ctx, qlen);
        if (nq > CR_MAX_PENDING) {
            JS_FreeValue(ctx, q);
            CR_FAIL("state.q exceeds the frontier cap");
        }
        for (i = 0; i < nq; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, q, i);
            JSValue uv, dv;
            const char *us = NULL;   /* the bad-exit frees it: never garbage */
            int32_t dep = 0;
            int bad;
            if (JS_IsException(e) || !JS_IsArray(ctx, e)) {
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, q);
                CR_FAIL("state.q entries must be [url, depth] pairs");
            }
            uv = JS_GetPropertyUint32(ctx, e, 0);
            dv = JS_GetPropertyUint32(ctx, e, 1);
            bad = JS_IsException(uv) || JS_IsException(dv);
            if (!bad) {
                us = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
                bad = !us || JS_ToInt32(ctx, &dep, dv) || dep < 0 ||
                      strlen(us) >= 1024 ||
                      (strncmp(us, "http://", 7) && strncmp(us, "https://", 8));
            }
            JS_FreeValue(ctx, dv);
            JS_FreeValue(ctx, uv);
            JS_FreeValue(ctx, e);
            if (bad) {
                if (us) JS_FreeCString(ctx, us);
                JS_FreeValue(ctx, q);
                CR_FAIL("state.q entry malformed");
            }
            if (cr_push(c, us, dep) < 0) {
                JS_FreeCString(ctx, us);
                JS_FreeValue(ctx, q);
                CR_FAIL("out of memory restoring the frontier");
            }
            JS_FreeCString(ctx, us);
        }
        JS_FreeValue(ctx, q);
    }
    /* The visited set: normalised keys, restored verbatim. Duplicates in the
       file are tolerated (restoring one key twice is the same set). */
    {
        JSValue sn = JS_GetPropertyStr(ctx, st, "seen");
        JSValue slen;
        if (JS_IsException(sn)) goto fail_pending;
        if (!JS_IsArray(ctx, sn)) {
            JS_FreeValue(ctx, sn);
            CR_FAIL("state.seen must be an array");
        }
        slen = JS_GetPropertyStr(ctx, sn, "length");
        if (JS_IsException(slen) || JS_ToUint32(ctx, &nsn, slen)) {
            JS_FreeValue(ctx, slen);
            JS_FreeValue(ctx, sn);
            goto fail_pending;
        }
        JS_FreeValue(ctx, slen);
        for (i = 0; i < nsn; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sn, i);
            const char *es = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
            int bad = !es || strlen(es) >= 1024;
            JS_FreeCString(ctx, es);   /* cr_mark_seen re-copies */
            JS_FreeValue(ctx, e);
            if (bad) {
                JS_FreeValue(ctx, sn);
                CR_FAIL("state.seen entry malformed");
            }
        }
        /* Second pass only now that every entry validated: a mid-restore OOM
           must not leave a HALF-seen set behind silently. (cr_mark_seen
           failing here is a genuine OOM; report it.) */
        for (i = 0; i < nsn; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sn, i);
            const char *es = JS_ToCString(ctx, e);
            int mrc;
            JS_FreeValue(ctx, e);
            mrc = es ? cr_seen(c, es) ? 0 : cr_mark_seen(c, es) : -1;
            if (es) JS_FreeCString(ctx, es);
            if (mrc < 0) {
                JS_FreeValue(ctx, sn);
                CR_FAIL("out of memory restoring the visited set");
            }
        }
        JS_FreeValue(ctx, sn);
    }
    JS_FreeValue(ctx, st);
#undef CR_FAIL

    c->started = 1;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    {
        cr_owner_t *own = (cr_owner_t *)calloc(1, sizeof *own);
        if (!own) {
            cr_dispose(c);
            return JS_ThrowOutOfMemory(ctx);
        }
        own->refs = 1;
        own->crawl_alive = 1;
        c->owner = own;
        c->ctx = ctx;
        c->rt = JS_GetRuntime(ctx);
    }
    obj = dyn_res_wrap(ctx, nt, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj)) return obj;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_sweep, c);
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    if (argc > 2 && JS_IsObject(argv[2]))
        JS_DefinePropertyValueStr(ctx, obj, "_extractor",
                                  JS_DupValue(ctx, argv[2]), 0);
    if (argc > 3 && JS_IsFunction(ctx, argv[3]))
        JS_DefinePropertyValueStr(ctx, obj, "_parse",
                                  JS_DupValue(ctx, argv[3]), 0);
    cr_install_async_iterator(ctx, obj);
    return obj;

fail_pending:
    /* the ORIGINAL exception is already pending (a strict read or a state
       getter threw): propagate it, never replace it with a generic message */
fail:
    JS_FreeValue(ctx, st);
    /* ONE teardown: cr_dispose walks q[0..qn), seen[0..sn), the field names
       and the struct itself -- the exact set pushed so far. A second free of
       the fields here was a double free the abort handler caught. */
    cr_dispose(c);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_cr_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_cr_start),
    JS_CFUNC_DEF("next", 0, dyn_cr_next),
    JS_CFUNC_DEF("pages", 0, dyn_cr_self),
    JS_CFUNC_DEF("serialize", 0, dyn_cr_serialize),
    JS_ALIAS_DEF("[Symbol.iterator]", "pages"),
    /*for-await drives the crawl through [Symbol.asyncIterator] (the
       engine AWAITS each next() of a real async iterator -- which is what
 's promise-returning next requires). The SYNC [Symbol.iterator]
       is installed PER-INSTANCE by the ctor, and only when concurrency is 1:
       the engine's async-from-sync wrapper does not await a promise
       returned by a SYNC next(), so an async crawl must never be pulled
       through it (a silent spin/partial-drain was exactly that bug). */
};

/* ---- Sitemap: the URLs Robots.sitemaps hands out, readable -----
 *
 * Robots.sitemaps() returns <sitemap> URLs from robots.txt, and until now
 * the runtime could do nothing with them: no sitemap parser existed in the
 * module set. Sitemap is a NAMESPACE (parse/list are stateless -- an
 * instance would hold nothing), in the mold of dyna:config's TOML/INI:
 *
 *   Sitemap.parse(xml)        -> string[]   the <loc> list, in order
 *   Sitemap.list(url[, src])  -> string[]   fetch + parse; a <sitemapindex>
 *                                            is followed ONE level and the
 *                                            children's lists concatenated
 *
 * The scanner is not a general XML parser (dyna:xml exists for that): it is
 * a bounded walker over the sitemap vocabulary -- comments, CDATA, PIs and
 * DOCTYPE are skipped, attributes (and the default xmlns) are ignored, the
 * five named entities plus numeric refs in <loc> are decoded, and element
 * names match case-insensitively because the wilder web does not always
 * ship the lowercase the spec asks for. Structural breakage -- an
 * unterminated comment, a <loc> that never closes -- is a SyntaxError
 * naming the byte offset, never a silently truncated list; text that is
 * not XML at all is a TypeError. A well-formed urlset with no <loc>
 * elements is simply an empty list: empty is a shape sitemaps have.
 *
 * Bounds, because sitemaps are attacker-influenced input exactly like
 * robots.txt: 16 MiB of XML and 50000 <loc> entries per document (the
 * spec's own per-sitemap URL cap); over either is refused.
 */
#define SM_MAX_BYTES (16u << 20)
#define SM_MAX_URLS  50000
#define SM_MAX_LOC_TEXT 2048   /* the sitemap spec's own per-URL budget */

typedef struct {
    char **v;
    size_t n, cap;
    int is_index;             /* root element was <sitemapindex> */
} sm_doc_t;

static void sm_doc_free(sm_doc_t *d)
{
    size_t i;
    for (i = 0; i < d->n; i++)
        free(d->v[i]);
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

/* Append one url (heap copy of s[0..n)). 0 ok, -1 OOM, -2 over the url cap. */
static int sm_doc_push(sm_doc_t *d, const char *s, size_t n)
{
    char *c;
    if (d->n >= SM_MAX_URLS)
        return -2;
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 16;
        char **nv = (char **)realloc(d->v, nc * sizeof(*nv));
        if (!nv)
            return -1;
        d->v = nv;
        d->cap = nc;
    }
    c = (char *)malloc(n + 1);
    if (!c)
        return -1;
    memcpy(c, s, n);
    c[n] = 0;
    d->v[d->n++] = c;
    return 0;
}

/* Dependency-free needle search (the memmem trap dyna-http.c documents). */
static const char *sm_find(const char *hay, size_t hlen,
                           const char *needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

/* Decode one XML entity (s points AT '&'). Writes at most 4 bytes to out,
   sets *outlen to the decoded byte count, and returns the number of SOURCE
   bytes consumed -- or 0 when the reference is malformed, in which case the
   '&' is literal text, exactly as a lenient reader would take it. */
static size_t sm_entity(const char *s, size_t rem, char out[4], int *outlen)
{
    static const struct { const char *nm; char c; } NAMED[] = {
        { "amp;",  '&',  }, { "lt;",   '<',  }, { "gt;",   '>',  },
        { "quot;", '"',  }, { "apos;", '\'', },
    };
    size_t i;
    *outlen = 0;
    if (rem < 4 || s[0] != '&')
        return 0;
    if (s[1] == '#') {
        int hex = (rem > 2 && (s[2] == 'x' || s[2] == 'X'));
        unsigned long cp = 0;
        size_t digits = 0, start = hex ? 3 : 2;
        for (i = start; i < rem && s[i] != ';'; i++) {
            int dv = -1;
            if (s[i] >= '0' && s[i] <= '9') dv = s[i] - '0';
            else if (hex && s[i] >= 'a' && s[i] <= 'f') dv = s[i] - 'a' + 10;
            else if (hex && s[i] >= 'A' && s[i] <= 'F') dv = s[i] - 'A' + 10;
            if (dv < 0)
                return 0;
            cp = cp * (hex ? 16u : 10u) + (unsigned long)dv;
            if (cp > 0x10FFFF)
                return 0;
            digits++;
        }
        if (!digits || i >= rem || s[i] != ';')
            return 0;
        if (cp < 0x80) { out[0] = (char)cp; *outlen = 1; }
        else if (cp < 0x800) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            *outlen = 2;
        } else if (cp < 0x10000) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            *outlen = 3;
        } else {
            out[0] = (char)(0xF0 | (cp >> 18));
            out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (char)(0x80 | (cp & 0x3F));
            *outlen = 4;
        }
        return i + 1;
    }
    for (i = 0; i < countof(NAMED); i++) {
        size_t ln = strlen(NAMED[i].nm);
        if (rem >= ln + 1 && memcmp(s + 1, NAMED[i].nm, ln) == 0) {
            out[0] = NAMED[i].c;
            *outlen = 1;
            return ln + 1;
        }
    }
    return 0;
}

static int sm_ci_eq(const char *p, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    if (n != l)
        return 0;
    while (n--) {
        if ((unsigned char)(p[n] | 0x20) != (unsigned char)(lit[n] | 0x20))
            return 0;
    }
    return 1;
}

static int sm_ci_prefix(const char *p, const char *lit)
{
    while (*lit) {
        if ((unsigned char)(*p++ | 0x20) != (unsigned char)(*lit++ | 0x20))
            return 0;
    }
    return 1;
}

/* The bounded scan. Returns 0, or -1 with a JS exception pending. */
static int sm_scan(JSContext *ctx, const char *p, size_t n, sm_doc_t *out)
{
    size_t i = 0;
    int root_seen = 0;

    memset(out, 0, sizeof(*out));
    while (i < n) {
        size_t tag_end, name_end;
        int is_close;
        if (p[i] != '<') { i++; continue; }
        if (sm_ci_prefix(p + i + 1, "!--")) {           /* comment */
            const char *e = sm_find(p + i + 4, n - i - 4, "-->", 3);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated comment at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 3;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '?') {             /* PI / xml decl */
            const char *e = sm_find(p + i + 2, n - i - 2, "?>", 2);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated processing instruction at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 2;
            continue;
        }
        if (sm_ci_prefix(p + i + 1, "![CDATA[")) {      /* CDATA: skipped. A
            url inside CDATA is not a shape sitemaps ship, and skipping keeps
            the entity decode of real <loc> text honest. */
            const char *e = sm_find(p + i + 9, n - i - 9, "]]>", 3);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated CDATA at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 3;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '!') {             /* DOCTYPE etc. */
            const char *e = (const char *)memchr(p + i + 2, '>', n - i - 2);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated <! declaration at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 1;
            continue;
        }
        is_close = (i + 1 < n && p[i + 1] == '/');
        name_end = i + 1 + (is_close ? 1 : 0);
        while (name_end < n && p[name_end] != '>' && p[name_end] != '/' &&
               p[name_end] != ' ' && p[name_end] != '\t' &&
               p[name_end] != '\r' && p[name_end] != '\n')
            name_end++;
        if (name_end >= n) {
            JS_ThrowSyntaxError(ctx,
                "Sitemap.parse: unterminated tag at byte %zu", i);
            goto fail;
        }
        if (!is_close && !root_seen) {
            /* the FIRST open element names the document type */
            if (sm_ci_eq(p + i + 1, name_end - (i + 1), "sitemapindex"))
                out->is_index = 1;
            root_seen = 1;
        }
        if (!is_close && sm_ci_eq(p + i + 1, name_end - (i + 1), "loc")) {
            /* find '>' of the open tag, then the closing </loc ...> */
            size_t ct = name_end, body, blen;
            const char *cl;
            while (ct < n && p[ct] != '>') ct++;
            if (ct >= n) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated <loc> tag at byte %zu", i);
                goto fail;
            }
            body = ct + 1;
            /* The close tag matches case-insensitively, like the open one,
               and the search is BOUNDED at the spec's own url budget: the
               </loc START may sit at most SM_MAX_LOC_TEXT bytes into the
               body, so the text is exactly 0..2048 bytes -- no slop. An
               unbounded search over a document of open <loc> tags is
               O(n^2) -- a 16 MiB bomb would burn minutes in the scan. Over
               the budget is a SyntaxError, which is also exactly what an
               over-long url deserves. */
            cl = NULL;
            {
                size_t max_q = body + SM_MAX_LOC_TEXT;
                for (size_t q = body; q <= max_q && q + 5 <= n; q++) {
                    if (p[q] == '<' && p[q + 1] == '/' &&
                        (p[q + 2] | 0x20) == 'l' && (p[q + 3] | 0x20) == 'o' &&
                        (p[q + 4] | 0x20) == 'c') {
                        cl = p + q;
                        break;
                    }
                }
            }
            if (!cl) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: <loc> at byte %zu never closes (over the "
                    "%d-byte url budget?)", i, SM_MAX_LOC_TEXT);
                goto fail;
            }
            blen = (size_t)(cl - p) - body;
            /* the closing tag's '>' -- attributes on </loc > are legal */
            cl = (const char *)memchr(p + body, '>', n - body);
            if (!cl) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: </loc> at byte %zu never closes", body);
                goto fail;
            }
            /* trim ASCII whitespace around the url */
            while (blen && (p[body] == ' ' || p[body] == '\t' ||
                            p[body] == '\r' || p[body] == '\n'))
                { body++; blen--; }
            while (blen && (p[body + blen - 1] == ' ' || p[body + blen - 1] == '\t' ||
                            p[body + blen - 1] == '\r' || p[body + blen - 1] == '\n'))
                blen--;
            if (blen) {
                char *dec = (char *)malloc(blen + 1);
                size_t di = 0, si = 0;
                int rc;
                if (!dec) { JS_ThrowOutOfMemory(ctx); goto fail; }
                while (si < blen) {
                    if (p[body + si] == '&') {
                        char one[4];
                        int one_n = 0;
                        size_t used = sm_entity(p + body + si, blen - si,
                                                one, &one_n);
                        if (used) {
                            /* every decoded form is <= its source span */
                            if (di + (size_t)one_n > blen) {
                                free(dec); JS_ThrowOutOfMemory(ctx); goto fail;
                            }
                            memcpy(dec + di, one, (size_t)one_n);
                            di += (size_t)one_n;
                            si += used;
                            continue;
                        }
                    }
                    dec[di++] = p[body + si++];
                }
                rc = sm_doc_push(out, dec, di);
                free(dec);
                if (rc == -2) {
                    JS_ThrowRangeError(ctx,
                        "Sitemap.parse: more than %d <loc> entries", SM_MAX_URLS);
                    goto fail;
                }
                if (rc < 0) { JS_ThrowOutOfMemory(ctx); goto fail; }
            }
            i = (size_t)(cl - p) + 1;
            continue;
        }
        /* not a loc: skip past this tag's '>' */
        tag_end = name_end;
        while (tag_end < n && p[tag_end] != '>') tag_end++;
        if (tag_end >= n) {
            JS_ThrowSyntaxError(ctx,
                "Sitemap.parse: unterminated tag at byte %zu", i);
            goto fail;
        }
        i = tag_end + 1;
    }
    return 0;
fail:
    sm_doc_free(out);
    return -1;
}

/* Sitemap.parse(xml) -> string[] */
static JSValue dyn_sitemap_parse(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    sm_doc_t d;
    JSValue arr;
    size_t len = 0, i;
    const char *xml;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Sitemap.parse(xml): xml must be a string");
    xml = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!xml)
        return JS_EXCEPTION;
    if (len > SM_MAX_BYTES) {
        JS_FreeCString(ctx, xml);
        return JS_ThrowRangeError(ctx, "Sitemap.parse: xml exceeds %d bytes",
                                  (int)SM_MAX_BYTES);
    }
    /* text that is not XML at all: refuse loudly. The likeliest real case
       is a robots.txt fed to the wrong function. */
    {
        size_t k = 0;
        while (k < len && (xml[k] == ' ' || xml[k] == '\t' ||
                           xml[k] == '\r' || xml[k] == '\n'))
            k++;
        if (k >= len || xml[k] != '<') {
            JS_FreeCString(ctx, xml);
            return JS_ThrowTypeError(ctx,
                "Sitemap.parse: input is not an XML document (a robots.txt "
                "is not a sitemap -- Robots.sitemaps() lists the sitemap URLs)");
        }
    }
    if (sm_scan(ctx, xml, len, &d) < 0) {
        JS_FreeCString(ctx, xml);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, xml);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { sm_doc_free(&d); return JS_EXCEPTION; }
    for (i = 0; i < d.n; i++) {
        JSValue u = JS_NewString(ctx, d.v[i]);
        if (JS_IsException(u) || JS_SetPropertyUint32(ctx, arr, (uint32_t)i, u) < 0) {
            JS_FreeValue(ctx, u);
            JS_FreeValue(ctx, arr);
            sm_doc_free(&d);
            return JS_EXCEPTION;
        }
    }
    sm_doc_free(&d);
    return arr;
}

/* One fetch + scan through ANY object with get(url) (a Fetcher, or a mock
   in the same way fe_raw accepts any client with request()). Returns the
   loc list as a JS array, sets *pis_index, or returns JS_EXCEPTION. */
static JSValue sm_fetch_locs(JSContext *ctx, JSValueConst fe, const char *url,
                             int *pis_index)
{
    JSAtom gatom = JS_NewAtom(ctx, "get");
    JSValue uv, res, body, arr;
    const char *body_s, *bv;
    size_t body_n = 0, k;
    sm_doc_t d;
    int32_t st = 0;

    *pis_index = 0;
    uv = JS_NewString(ctx, url);
    if (JS_IsException(uv)) { JS_FreeAtom(ctx, gatom); return uv; }
    res = JS_Invoke(ctx, fe, gatom, 1, (JSValueConst *)&uv);
    JS_FreeValue(ctx, uv);
    JS_FreeAtom(ctx, gatom);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    {
        JSValue sv = JS_GetPropertyStr(ctx, res, "status");
        int rc = 0;
        /* the status read can run a user getter: check the -1 return so a
         * pending exception propagates, never leaving st stale (cf. the
         * dyna-csv.c length reads) */
        if (!JS_IsUndefined(sv))
            rc = JS_ToInt32(ctx, &st, sv);
        JS_FreeValue(ctx, sv);
        if (rc) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;  /* a status getter that throws propagates */
        }
        if (st < 200 || st > 299) {
            JS_FreeValue(ctx, res);
            JS_ThrowRangeError(ctx,
                "Sitemap.list: %s answered HTTP %d (only a 2xx body can be "
                "parsed as a sitemap)", url, st);
            return JS_EXCEPTION;
        }
    }
    body = JS_GetPropertyStr(ctx, res, "body");
    JS_FreeValue(ctx, res);
    bv = JS_ToCStringLen(ctx, &body_n, body);
    JS_FreeValue(ctx, body);
    if (!bv)
        return JS_EXCEPTION;
    body_s = bv;
    if (body_n > SM_MAX_BYTES) {
        JS_FreeCString(ctx, body_s);
        JS_ThrowRangeError(ctx, "Sitemap.list: sitemap exceeds %d bytes",
                           (int)SM_MAX_BYTES);
        return JS_EXCEPTION;
    }
    if (sm_scan(ctx, body_s, body_n, &d) < 0) {
        JS_FreeCString(ctx, body_s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, body_s);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { sm_doc_free(&d); return JS_EXCEPTION; }
    for (k = 0; k < d.n; k++) {
        JSValue u = JS_NewString(ctx, d.v[k]);
        if (JS_IsException(u) || JS_SetPropertyUint32(ctx, arr, (uint32_t)k, u) < 0) {
            JS_FreeValue(ctx, u);
            JS_FreeValue(ctx, arr);
            sm_doc_free(&d);
            return JS_EXCEPTION;
        }
    }
    *pis_index = d.is_index;
    sm_doc_free(&d);
    return arr;
}

/* Build the module's own Fetcher by the same module-eval bridge
   fe_auto_client uses. `opts` (JS_UNDEFINED or an object) is parked on a
   globalThis holder and handed to the Fetcher ctor inside the snippet;
   omitted, the snippet passes the module's own named agent. The holders
   are deleted on every path -- nothing user-visible leaks. */
static JSValue sm_make_fetcher(JSContext *ctx, JSValueConst opts)
{
    /* Never-rejecting snippet: see fe_auto_client -- a throw inside the
       module body would report as an unhandled rejection at rejection time,
       before C could attach any handler. Failure = undefined holder. */
    static const char src[] =
        "import { Fetcher } from \"dyna:scrape\";\n"
        "try { globalThis.__dyna_scrape_fef = new Fetcher(\n"
        "    globalThis.__dyna_scrape_feopts ?? { agent: \"dyna-sitemap/1.0\" }); }\n"
        "catch (e) { globalThis.__dyna_scrape_fef = undefined; }\n";
    JSValue r, v = JS_UNDEFINED, g, fobj;
    JSPromiseStateEnum st;

    if (JS_IsObject(opts)) {
        g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__dyna_scrape_feopts", JS_DupValue(ctx, opts));
        JS_FreeValue(ctx, g);
    }
    r = JS_Eval(ctx, src, strlen(src), "<dyna:scrape sitemap fetcher>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        goto sweep;
    v = JS_EvalFunction(ctx, r);   /* consumes r */
    if (JS_IsException(v))
        goto sweep;
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        dyn_scrape_promise_swallow(ctx, v);   /* handler first, then free */
        goto sweep;
    }
    dyn_scrape_promise_swallow(ctx, v);
    g = JS_GetGlobalObject(ctx);
    fobj = JS_GetPropertyStr(ctx, g, "__dyna_scrape_fef");
    {
        JSAtom a1 = JS_NewAtom(ctx, "__dyna_scrape_fef");
        JS_DeleteProperty(ctx, g, a1, 0);
        JS_FreeAtom(ctx, a1);
    }
    {
        JSAtom a2 = JS_NewAtom(ctx, "__dyna_scrape_feopts");
        JS_DeleteProperty(ctx, g, a2, 0);
        JS_FreeAtom(ctx, a2);
    }
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(fobj)) {
        JS_FreeValue(ctx, fobj);
        goto sweep;
    }
    return fobj;
sweep:
    {   /* eval paths leave their holder behind; sweep it */
        JSValue g2 = JS_GetGlobalObject(ctx);
        JSAtom a1 = JS_NewAtom(ctx, "__dyna_scrape_fef");
        JS_DeleteProperty(ctx, g2, a1, 0);
        JS_FreeAtom(ctx, a1);
        JSAtom a2 = JS_NewAtom(ctx, "__dyna_scrape_feopts");
        JS_DeleteProperty(ctx, g2, a2, 0);
        JS_FreeAtom(ctx, a2);
        JS_FreeValue(ctx, g2);
    }
    return JS_ThrowTypeError(ctx,
        "Sitemap.list: the module's own Fetcher could not be constructed "
        "-- pass a Fetcher (or an options bag) explicitly");
}

/* Sitemap.list(url[, source]) -> string[]
 *
 * Fetch `url`, parse it, return the URL list. A <sitemapindex> is followed
 * exactly ONE level: every <loc> child is fetched in turn and its list
 * appended (a child that is itself an index has its entries returned
 * as-is -- two levels would need a cycle policy, and the spec's index
 * files point at urlsets). `source` is any object with get(url) -- a
 * Fetcher, or a mock -- or an options bag for building one (the Fetcher
 * ctor's keys); nothing means the module's own fetcher with the module's
 * own named agent. Duplicates, if a document ships them, are preserved:
 * Crawl is the dedupper, list() is a reader.
 */
static JSValue dyn_sitemap_list(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue fe = JS_UNDEFINED, urls = JS_UNDEFINED, out = JS_UNDEFINED;
    const char *url;
    int is_index = 0;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Sitemap.list(url[, fetcherOrOpts])");
    url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        /* a Fetcher (carries get()), or an opts bag for building one? */
        JSAtom gatom = JS_NewAtom(ctx, "get");
        JSValue gf = JS_GetProperty(ctx, argv[1], gatom);
        JS_FreeAtom(ctx, gatom);
        if (JS_IsFunction(ctx, gf)) {
            JS_FreeValue(ctx, gf);
            fe = JS_DupValue(ctx, argv[1]);
        } else {
            JS_FreeValue(ctx, gf);
            fe = sm_make_fetcher(ctx, argv[1]);
            if (JS_IsException(fe)) { JS_FreeCString(ctx, url); return JS_EXCEPTION; }
        }
    } else {
        fe = sm_make_fetcher(ctx, JS_UNDEFINED);
        if (JS_IsException(fe)) { JS_FreeCString(ctx, url); return JS_EXCEPTION; }
    }
    urls = sm_fetch_locs(ctx, fe, url, &is_index);
    JS_FreeCString(ctx, url);
    if (JS_IsException(urls))
        goto out_err;                /* fe still must go: EVERY path frees it */
    if (!is_index) {
        out = urls;
        urls = JS_UNDEFINED;
        goto out_ok;                 /* the common case: one fetch, done */
    }
    /* the index case: fetch every child, concatenate. Errors carry the
       child's own exception (a 404 child is an HTTP 404 RangeError). */
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto out_err;
    {
        uint32_t nl = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, urls, "length");
        int lrc = JS_ToUint32(ctx, &nl, lv);
        JS_FreeValue(ctx, lv);
        if (lrc)
            goto out_err;
        for (i = 0; i < nl; i++) {
            JSValue kid = JS_GetPropertyUint32(ctx, urls, i);
            JSValue sub;
            const char *kid_s;
            int kid_is_index = 0;
            uint32_t sl = 0, j;
            JSValue sv;
            int src;
            if (JS_IsException(kid)) {
                JS_FreeValue(ctx, kid);
                goto out_err;
            }
            kid_s = JS_ToCString(ctx, kid);
            JS_FreeValue(ctx, kid);
            if (!kid_s)
                goto out_err;
            sub = sm_fetch_locs(ctx, fe, kid_s, &kid_is_index);
            JS_FreeCString(ctx, kid_s);
            if (JS_IsException(sub))
                goto out_err;
            sv = JS_GetPropertyStr(ctx, sub, "length");
            src = JS_ToUint32(ctx, &sl, sv);
            JS_FreeValue(ctx, sv);
            if (src) { JS_FreeValue(ctx, sub); goto out_err; }
            for (j = 0; j < sl; j++) {
                JSValue e = JS_GetPropertyUint32(ctx, sub, j);
                uint32_t ol = 0;
                JSValue ov;
                if (JS_IsException(e)) { JS_FreeValue(ctx, sub); goto out_err; }
                ov = JS_GetPropertyStr(ctx, out, "length");
                /* the length read can run a user getter: a throw must
                 * propagate, never leave ol stale under a pending exception */
                if (JS_IsException(ov) || JS_ToUint32(ctx, &ol, ov)) {
                    JS_FreeValue(ctx, ov);
                    JS_FreeValue(ctx, e);
                    JS_FreeValue(ctx, sub);
                    goto out_err;
                }
                JS_FreeValue(ctx, ov);
                if (JS_SetPropertyUint32(ctx, out, ol, e) < 0) {
                    JS_FreeValue(ctx, e);
                    JS_FreeValue(ctx, sub);
                    goto out_err;
                }
            }
            JS_FreeValue(ctx, sub);
        }
    }
    goto out_ok;
out_err:
    JS_FreeValue(ctx, fe);
    JS_FreeValue(ctx, urls);
    JS_FreeValue(ctx, out);
    return JS_EXCEPTION;
out_ok:
    JS_FreeValue(ctx, fe);
    JS_FreeValue(ctx, urls);
    return out;
}

static int dyn_scrape_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_rb_class_id, &dyn_rb_class,
                           dyn_rb_proto, countof(dyn_rb_proto),
                           dyn_rb_ctor, "Robots") < 0)
        return -1;
    /*the FetcherStream resource is created only by Fetcher.getStream,
       never constructed from JS -- the dyna:stream factory-class shape:
       id + class + proto with the close surface, no exported constructor. */
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
        JSValue fproto;
        JS_NewClassID(&dyn_fs_class_id);
        if (JS_NewClass(rt, dyn_fs_class_id, &dyn_fs_class) < 0)
            return -1;
        fproto = JS_NewObject(ctx);
        if (JS_IsException(fproto))
            return -1;
        JS_SetPropertyFunctionList(ctx, fproto, dyn_fs_proto,
                                   (int)countof(dyn_fs_proto));
        dyn_res_class_common(ctx, dyn_fs_class_id, fproto);
        JS_SetClassProto(ctx, dyn_fs_class_id, fproto);
    }
    if (dyn_register_class(ctx, m, &dyn_cr_class_id, &dyn_cr_class,
                           dyn_cr_proto, countof(dyn_cr_proto),
                           dyn_cr_ctor, "Crawl") < 0)
        return -1;
    /*Crawl.resume is a STATIC -- the state outlives any one instance,
       so the factory belongs to the class, not the prototype. Reached through
       the registered proto's constructor (the same object the module exports). */
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_cr_class_id);
        if (!JS_IsObject(proto))
            return -1;
        {
            JSValue ctor = JS_GetPropertyStr(ctx, proto, "constructor");
            JS_FreeValue(ctx, proto);
            if (!JS_IsFunction(ctx, ctor))
                { JS_FreeValue(ctx, ctor); return -1; }
            if (JS_SetPropertyStr(ctx, ctor, "resume",
                                  JS_NewCFunction(ctx, dyn_cr_resume,
                                                  "resume", 2)) < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
            JS_FreeValue(ctx, ctor);
        }
    }
    if (dyn_register_class(ctx, m, &dyn_fe_class_id, &dyn_fe_class,
                           dyn_fe_proto, countof(dyn_fe_proto),
                           dyn_fe_ctor, "Fetcher") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ex_class_id, &dyn_ex_class,
                                 dyn_ex_proto, countof(dyn_ex_proto),
                                 dyn_ex_ctor, "Extractor") < 0)
        return -1;
    /*Sitemap is a namespace, not a class -- parse/list are stateless */
    {
        JSValue sm = JS_NewObject(ctx);
        if (JS_IsException(sm))
            return -1;
        if (JS_SetPropertyStr(ctx, sm, "parse",
                              JS_NewCFunction(ctx, dyn_sitemap_parse,
                                              "parse", 1)) < 0 ||
            JS_SetPropertyStr(ctx, sm, "list",
                              JS_NewCFunction(ctx, dyn_sitemap_list,
                                              "list", 1)) < 0 ||
            JS_SetModuleExport(ctx, m, "Sitemap", sm) < 0) {
            JS_FreeValue(ctx, sm);
            return -1;
        }
    }
    return 0;
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
    JS_AddModuleExport(ctx, m, "Sitemap");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SCRAPE */
