#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <poll.h>
#include "core/dyn-timer.h"
#include "core/dyn-prng.h"
#include "dyna-simd-kernels.h"

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

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
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
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
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

static const char* const sc_robots_keys[] = { "agent" };
static const char* const sc_ex_ctor_keys[] = { "text" };
static const char* const sc_field_keys[] = { "sel", "attr", "all", "required",
    "trim", "source", "default", "as" };
static const char* const sc_run_keys[] = { "base" };
static const char* const sc_fetcher_keys[] = { "agent", "client", "headers",
    "robots", "minDelayMs", "retries",
    "maxRedirects", "maxBodyBytes",
    "allowPrivateHosts", "revalidate",
    "robotsTtlMs", "allowInsecureDowngrade",
    "proxy", "ca", "poolSize" };
static const char* const sc_crawl_keys[] = { "maxPages", "maxDepth", "sameHost",
    "linkField", "baseField",
    "canonicalField", "relField",
    "robotsField", "concurrency" };

#define RB_MAX_BYTES (512 * 1024)
#define RB_MAX_RULES 1000
#define RB_MAX_PATH 2048
#define RB_MAX_GROUPS 64
#define RB_MAX_UAS_PER_GROUP 8
#define RB_MAX_AGENT 128

typedef struct {
    char* path;
    size_t len;
    size_t grp;
    unsigned allow : 1;
} rb_rule_t;

typedef struct {
    rb_rule_t* rules;
    size_t n, cap;
    double delay;
    char** sitemaps;
    size_t n_site, cap_site;
} rb_t;

static JSClassID dyn_rb_class_id;

static void rb_free(rb_t* r)
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

static void rb_res_dispose(void* p)
{
    rb_free((rb_t*)p);
}

static const JSClassDef dyn_rb_class = {
    "Robots",
    .finalizer = dyn_res_finalizer,
};

static int rb_reserved(unsigned char c);

static size_t rb_norm_rule(const char* s, size_t n, char* out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    while (i < n && o + 3 < cap) {
        const unsigned char c = (unsigned char)s[i];
        int enc;
        if (c == '$')
            enc = i + 1 != n;
        else if (c != '/' && c != '*')
            enc = (rb_reserved(c) || c >= 0x80);
        else
            enc = 0;
        if (c == '%' && i + 2 < n) {
            int h = -1, l = -1;
            unsigned char a = (unsigned char)s[i + 1], b = (unsigned char)s[i + 2];
            if (a >= '0' && a <= '9')
                h = a - '0';
            else if (a >= 'a' && a <= 'f')
                h = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F')
                h = a - 'A' + 10;
            if (b >= '0' && b <= '9')
                l = b - '0';
            else if (b >= 'a' && b <= 'f')
                l = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F')
                l = b - 'A' + 10;
            if (h >= 0 && l >= 0) {
                out[o++] = '%';
                out[o++] = (char)a;
                out[o++] = (char)b;
                i += 3;
                continue;
            }
        }
        if (enc) {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
        i++;
    }
    out[o] = '\0';
    return o;
}

static int rb_match(const char* pat, size_t pn, const char* path, size_t sn)
{
    size_t pi = 0, si = 0, star = (size_t)-1, mark = 0;

    while (pi < pn) {
        if (pat[pi] == '$' && pi + 1 == pn)
            return si == sn;
        if (pat[pi] == '*') {
            star = ++pi;
            mark = si;
            continue;
        }
        if (si < sn && pat[pi] == path[si]) {
            pi++;
            si++;
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

static int rb_split(const char* l, size_t n, const char** k, size_t* kn,
    const char** v, size_t* vn)
{
    size_t c = 0;
    while (c < n && l[c] != ':')
        c++;
    if (c == n)
        return 0;
    *k = l;
    *kn = c;
    while (*kn && (l[*kn - 1] == ' ' || l[*kn - 1] == '\t'))
        (*kn)--;
    c++;
    while (c < n && (l[c] == ' ' || l[c] == '\t'))
        c++;
    *v = l + c;
    *vn = n - c;
    while (*vn && ((*v)[*vn - 1] == ' ' || (*v)[*vn - 1] == '\t' || (*v)[*vn - 1] == '\r'))
        (*vn)--;
    return 1;
}

static int rb_ci_eq(const char* a, size_t an, const char* b)
{
    size_t i;
    for (i = 0; i < an && b[i]; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        if (ca != b[i])
            return 0;
    }
    return i == an && !b[i];
}

static int rb_ci_ncmp(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];
        if (ca != cb)
            return 0;
    }
    return 1;
}

typedef struct {
    size_t first, n;
    double delay;
    int is_star;
    char* uas[RB_MAX_UAS_PER_GROUP];
    size_t n_uas;
} rb_grp_t;

static size_t rb_token_len(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i] != '/'; i++)
        ;
    return i;
}

static void rb_groups_free(rb_grp_t* g, size_t n)
{
    size_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < g[i].n_uas; j++)
            free(g[i].uas[j]);
    free(g);
}

static int rb_parse(rb_t* r, const char* s, size_t n, const char* agent)
{
    rb_rule_t* arena = NULL;
    size_t n_arena = 0, cap_arena = 0;
    rb_grp_t* groups;
    size_t n_groups = 0, cap_groups = 16;
    size_t token_a, aglen, best_g = (size_t)-1;
    int best_score = -1;
    size_t best_uale = 0, gi, z;
    long gcur = -1;
    int prev_ua = 1;
    size_t i = 0;

    if (n > RB_MAX_BYTES)
        n = RB_MAX_BYTES;
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s += 3;
        n -= 3;
    }
    aglen = strlen(agent);
    token_a = rb_token_len(agent, aglen);
    groups = (rb_grp_t*)calloc(cap_groups, sizeof(*groups));
    if (!groups)
        return -1;

    while (i < n) {
        size_t e = i, ln, t;
        const char *k, *v;
        size_t kn, vn;
        t = simd.find_u8((const uint8_t*)s + e, '\n', n - e);
        e = (t == (size_t)-1) ? n : e + t;
        ln = e - i;
        {
            const char* line = s + i;
            size_t h = 0;
            while (h < ln && line[h] != '#')
                h++;
            if (rb_split(line, h, &k, &kn, &v, &vn)) {
                if (rb_ci_eq(k, kn, "user-agent")) {
                    char uabuf[RB_MAX_AGENT];
                    size_t ul = vn < sizeof(uabuf) - 1 ? vn : sizeof(uabuf) - 1;
                    rb_grp_t* g;
                    if (prev_ua && gcur >= 0)
                        g = &groups[gcur];
                    else {
                        if (n_groups >= RB_MAX_GROUPS) {
                            gcur = -1;
                            prev_ua = 1;
                            goto next_line;
                        }
                        if (n_groups == cap_groups) {
                            size_t nc = cap_groups * 2;
                            rb_grp_t* ng = (rb_grp_t*)realloc(
                                groups, nc * sizeof(*ng));
                            if (!ng)
                                goto oom;
                            groups = ng;
                            cap_groups = nc;
                        }
                        gcur = (long)n_groups++;
                        memset(&groups[gcur], 0, sizeof(rb_grp_t));
                        groups[gcur].delay = -1.0;
                        g = &groups[gcur];
                    }
                    memcpy(uabuf, v, ul);
                    uabuf[ul] = 0;
                    if (g->n_uas < RB_MAX_UAS_PER_GROUP) {
                        char* c = (char*)malloc(ul + 1);
                        if (!c)
                            goto oom;
                        memcpy(c, uabuf, ul + 1);
                        g->uas[g->n_uas++] = c;
                    }
                    prev_ua = 1;
                } else {
                    prev_ua = 0;
                    if (rb_ci_eq(k, kn, "sitemap")) {
                        if (r->n_site == r->cap_site) {
                            size_t nc = r->cap_site ? r->cap_site * 2 : 4;
                            char** ns = (char**)realloc(r->sitemaps,
                                nc * sizeof(char*));
                            if (ns) {
                                r->sitemaps = ns;
                                r->cap_site = nc;
                            }
                        }
                        if (r->n_site < r->cap_site && vn) {
                            char* c = (char*)malloc(vn + 1);
                            if (c) {
                                memcpy(c, v, vn);
                                c[vn] = 0;
                                r->sitemaps[r->n_site++] = c;
                            }
                        }
                    } else if (gcur >= 0) {
                        rb_grp_t* g = &groups[gcur];
                        if (rb_ci_eq(k, kn, "crawl-delay")) {
                            const char* q = v;
                            size_t digits = 0;
                            while (q < v + vn && *q >= '0' && *q <= '9') {
                                q++;
                                digits++;
                            }
                            if (q < v + vn && *q == '.') {
                                q++;
                                while (q < v + vn && *q >= '0' && *q <= '9') {
                                    q++;
                                    digits++;
                                }
                            }
                            if (digits > 0 && q == v + vn) {
                                char buf[32];
                                size_t c = vn < sizeof(buf) - 1 ? vn : sizeof(buf) - 1;
                                char* end;
                                double d;
                                memcpy(buf, v, c);
                                buf[c] = 0;
                                d = strtod(buf, &end);
                                if (end != buf && *end == '\0')
                                    g->delay = d;
                            }
                        } else if (rb_ci_eq(k, kn, "disallow") || rb_ci_eq(k, kn, "allow")) {
                            int allow = rb_ci_eq(k, kn, "allow");
                            if (!vn || n_arena >= RB_MAX_RULES)
                                goto next_line;
                            if (n_arena == cap_arena) {
                                size_t nc = cap_arena ? cap_arena * 2 : 32;
                                rb_rule_t* na = (rb_rule_t*)
                                    realloc(arena, nc * sizeof(*na));
                                if (!na)
                                    goto oom;
                                arena = na;
                                cap_arena = nc;
                            }
                            {
                                char* copy;
                                size_t pn = vn > RB_MAX_PATH ? RB_MAX_PATH : vn;
                                copy = (char*)malloc(4 * pn + 1);
                                if (!copy)
                                    goto oom;
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

    for (gi = 0; gi < n_groups; gi++) {
        rb_grp_t* g = &groups[gi];
        int score = 0;
        size_t uale_max = 0, u;
        for (u = 0; u < g->n_uas; u++) {
            size_t ul = strlen(g->uas[u]);
            size_t utok = rb_token_len(g->uas[u], ul);
            if (rb_ci_ncmp(g->uas[u], ul, agent, aglen))
                score = score > 2 ? score : 2;
            else if (utok && utok == token_a && rb_ci_ncmp(g->uas[u], utok, agent, token_a))
                score = score > 1 ? score : 1;
            if (ul > uale_max)
                uale_max = ul;
        }
        if (score > best_score || (score == best_score && uale_max > best_uale)) {
            best_score = score;
            best_g = gi;
            best_uale = uale_max;
        }
    }
    if (best_score <= 0) {
        best_g = (size_t)-1;
        for (gi = 0; gi < n_groups; gi++) {
            size_t u;
            for (u = 0; u < groups[gi].n_uas; u++)
                if (!strcmp(groups[gi].uas[u], "*")) {
                    best_g = gi;
                    break;
                }
            if (best_g != (size_t)-1)
                break;
        }
    }

    if (best_g != (size_t)-1) {
        rb_grp_t* g = &groups[best_g];
        r->delay = g->delay;
        if (g->n) {
            r->rules = (rb_rule_t*)calloc(g->n, sizeof(*r->rules));
            if (!r->rules)
                goto oom;
            memcpy(r->rules, arena + g->first, g->n * sizeof(*r->rules));
            r->n = g->n;
            r->cap = g->n;
        }
    }
    for (z = 0; z < n_arena; z++) {
        int keep = best_g != (size_t)-1 && z >= groups[best_g].first && z < groups[best_g].first + groups[best_g].n;
        if (!keep)
            free(arena[z].path);
    }
    free(arena);
    rb_groups_free(groups, n_groups);
    return 0;
oom:
    for (z = 0; z < n_arena; z++)
        free(arena[z].path);
    free(arena);
    rb_groups_free(groups, n_groups);
    return -1;
}

static JSValue dyn_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    rb_t* r;
    const char *txt, *agent = NULL;
    size_t tn = 0;
    JSValue av = JS_UNDEFINED;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Robots(text[, { agent }])");
    txt = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!txt)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_robots_keys, 1)) {
            JS_FreeCString(ctx, txt);
            return JS_EXCEPTION;
        }
        av = JS_GetPropertyStr(ctx, argv[1], "agent");
        if (!JS_IsUndefined(av))
            agent = JS_ToCString(ctx, av);
    }
    r = (rb_t*)calloc(1, sizeof(*r));
    if (!r) {
        JS_FreeCString(ctx, txt);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->delay = -1.0;
    if (rb_parse(r, txt, tn, agent ? agent : "*") < 0) {
        rb_free(r);
        if (agent)
            JS_FreeCString(ctx, agent);
        JS_FreeValue(ctx, av);
        JS_FreeCString(ctx, txt);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (agent)
        JS_FreeCString(ctx, agent);
    JS_FreeValue(ctx, av);
    JS_FreeCString(ctx, txt);

    return dyn_res_wrap(ctx, new_target, dyn_rb_class_id, r, rb_res_dispose);
}

static int rb_reserved(unsigned char c)
{
    switch (c) {
    case ':':
    case '/':
    case '?':
    case '#':
    case '[':
    case ']':
    case '@':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
        return 1;
    default:
        return 0;
    }
}

static size_t rb_norm_path(const char* s, size_t n, char* out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    while (i < n && o + 3 < cap) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n) {
            int h = -1, l = -1;
            unsigned char a = (unsigned char)s[i + 1], b = (unsigned char)s[i + 2];
            if (a >= '0' && a <= '9')
                h = a - '0';
            else if (a >= 'a' && a <= 'f')
                h = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F')
                h = a - 'A' + 10;
            if (b >= '0' && b <= '9')
                l = b - '0';
            else if (b >= 'a' && b <= 'f')
                l = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F')
                l = b - 'A' + 10;
            if (h >= 0 && l >= 0) {
                int v = (h << 4) | l;
                if (v == '/' || v == '*' || v == '$' || v >= 0x80 || rb_reserved((unsigned char)v)) {
                    out[o++] = '%';
                    out[o++] = HEX[h];
                    out[o++] = HEX[l];
                } else {
                    out[o++] = (char)v;
                }
                i += 3;
                continue;
            }
        }
        if (c != '/' && (rb_reserved(c) || c >= 0x80 || c < 0x21 || c == 0x7F)) {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
        i++;
    }
    out[o] = '\0';
    return o;
}

static int rb_allows_path(const rb_t* r, const char* p, size_t pn)
{
    size_t i, best = 0;
    int verdict = 1, found = 0;
    char* norm;
    size_t nn;

    if (pn > 2048)
        pn = 2048;
    norm = (char*)malloc(3 * pn + 1);
    if (!norm)
        return 1;
    nn = rb_norm_path(p, pn, norm, 3 * pn + 1);
    for (i = 0; i < r->n; i++) {
        if (!rb_match(r->rules[i].path, r->rules[i].len, norm, nn))
            continue;
        if (!found || r->rules[i].len > best || (r->rules[i].len == best && r->rules[i].allow)) {
            best = r->rules[i].len;
            verdict = r->rules[i].allow;
            found = 1;
        }
    }
    free(norm);
    return verdict;
}

static JSValue dyn_rb_allows(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    const char* p;
    size_t pn = 0;
    int verdict;

    if (!r)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "allows(path)");
    p = JS_ToCStringLen(ctx, &pn, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    verdict = rb_allows_path(r, p, pn);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, verdict);
}

static JSValue dyn_rb_delay(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    return r->delay < 0 ? JS_NULL : JS_NewFloat64(ctx, r->delay);
}

static JSValue dyn_rb_sitemaps(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    JSValue a;
    size_t i;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < r->n_site; i++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)i,
            JS_NewString(ctx, r->sitemaps[i]));
    return a;
}

static JSValue dyn_rb_rules(JSContext* ctx, JSValueConst this_val)
{
    DynResource* box = dyn_res_get(ctx, this_val, dyn_rb_class_id);
    rb_t* r = box ? (rb_t*)box->native : NULL;
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)r->n);
}

static const JSCFunctionListEntry dyn_rb_proto[] = {
    JS_CFUNC_DEF("allows", 1, dyn_rb_allows),
    JS_CFUNC_DEF("crawlDelay", 0, dyn_rb_delay),
    JS_CFUNC_DEF("sitemaps", 0, dyn_rb_sitemaps),
    JS_CGETSET_DEF("ruleCount", dyn_rb_rules, NULL),
};

static long fe_resolve(const char* base, const char* ref, char* out,
    size_t cap);

typedef struct {
    char* name;
    JSValue sel;
    JSValue dflt;
    char* attr;
    unsigned all : 1;
    unsigned required : 1;
    unsigned trim : 1;
    unsigned source : 1;
    unsigned as_number : 1;
    unsigned as_url : 1;
    unsigned as_json : 1;
} ex_field_t;

typedef struct {
    ex_field_t* f;
    size_t n;
    JSValue html_text;
} ex_t;

static JSClassID dyn_ex_class_id;

static void ex_free_ctx(JSContext* ctx, ex_t* e)
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

static void dyn_ex_finalizer(JSRuntime* rt, JSValue val)
{
    ex_t* e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e)
        return;
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

static void dyn_ex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    ex_t* e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e)
        return;
    for (i = 0; i < e->n; i++) {
        JS_MarkValue(rt, e->f[i].sel, mark);
        JS_MarkValue(rt, e->f[i].dflt, mark);
    }
    JS_MarkValue(rt, e->html_text, mark);
}

static const JSClassDef dyn_ex_class = {
    "Extractor",
    .finalizer = dyn_ex_finalizer,
    .gc_mark = dyn_ex_mark,
};

static char* ex_dup_str(JSContext* ctx, JSValueConst v)
{
    const char* s = JS_ToCString(ctx, v);
    char* o;
    if (!s)
        return NULL;
    o = strdup(s);
    JS_FreeCString(ctx, s);
    return o;
}

static size_t ex_trimmed(const char* s, size_t n, size_t* ofs)
{
    size_t a = 0, e = n;
    while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        a++;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        e--;
    *ofs = a;
    return e - a;
}

#define EX_SOURCE_CAP (1u << 20)

typedef struct {
    char* p;
    size_t n;
} ex_chunk_t;

static int ex_concat_kids(JSContext* ctx, JSValueConst node, int depth,
    ex_chunk_t** chunks, size_t* n_ch, size_t* cap_ch,
    size_t* total)
{
    JSValue kids;
    uint32_t n = 0, i;

    if (depth > 64)
        return 0;
    if (!JS_IsObject(node))
        return 0;
    kids = JS_GetPropertyStr(ctx, node, "children");
    if (!JS_IsArray(ctx, kids)) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, kids, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
    }
    for (i = 0; i < n; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, kids, i);
        if (JS_IsString(c)) {
            const char* t;
            size_t tn;
            t = JS_ToCStringLen(ctx, &tn, c);
            if (!t) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            if (*n_ch == *cap_ch) {
                size_t nc = *cap_ch ? *cap_ch * 2 : 8;
                ex_chunk_t* na = (ex_chunk_t*)
                    realloc(*chunks, nc * sizeof(ex_chunk_t));
                if (!na) {
                    JS_FreeCString(ctx, t);
                    JS_FreeValue(ctx, c);
                    JS_FreeValue(ctx, kids);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                *chunks = na;
                *cap_ch = nc;
            }
            {
                ex_chunk_t* ch = &(*chunks)[(*n_ch)++];
                ch->p = (char*)malloc(tn + 1);
                if (!ch->p) {
                    (*n_ch)--;
                    JS_FreeCString(ctx, t);
                    JS_FreeValue(ctx, c);
                    JS_FreeValue(ctx, kids);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                memcpy(ch->p, t, tn);
                ch->p[tn] = 0;
                ch->n = tn;
            }
            *total += tn;
            JS_FreeCString(ctx, t);
            if (*total > EX_SOURCE_CAP) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                return 0;
            }
        } else if (JS_IsObject(c)) {
            int rc = ex_concat_kids(ctx, c, depth + 1,
                chunks, n_ch, cap_ch, total);
            if (rc < 0) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                return -1;
            }
        }
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, kids);
    return 0;
}

static JSValue ex_node_source(JSContext* ctx, JSValueConst node)
{
    ex_chunk_t* chunks = NULL;
    size_t n_ch = 0, cap_ch = 0, total = 0, i, o = 0;
    char* joined;
    JSValue out;

    if (ex_concat_kids(ctx, node, 0, &chunks, &n_ch, &cap_ch, &total) < 0) {
        size_t z;
        for (z = 0; z < n_ch; z++)
            free(chunks[z].p);
        free(chunks);
        return JS_EXCEPTION;
    }
    joined = (char*)malloc(total + 1);
    if (!joined) {
        for (i = 0; i < n_ch; i++)
            free(chunks[i].p);
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

static JSValue dyn_ex_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ex_t* e;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    JSValue obj, proto, ht = JS_UNDEFINED;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Extractor(spec) needs an object");
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    e = (ex_t*)calloc(1, sizeof(*e));
    if (!e) {
        js_free(ctx, tab);
        return JS_ThrowOutOfMemory(ctx);
    }
    e->html_text = JS_UNDEFINED;
    e->f = (ex_field_t*)calloc(len ? len : 1, sizeof(ex_field_t));
    if (!e->f) {
        free(e);
        js_free(ctx, tab);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (i = 0; i < len; i++) {
        JSValue fv = JS_GetProperty(ctx, argv[0], tab[i].atom), v;
        ex_field_t* f = &e->f[e->n];
        const char* nm;
        memset(f, 0, sizeof(*f));
        e->n++;
        f->dflt = JS_UNDEFINED;
        if (!JS_IsObject(fv)) {
            JS_FreeValue(ctx, fv);
            JS_ThrowTypeError(ctx, "Extractor: each field must be an object");
            goto fail;
        }
        nm = JS_AtomToCString(ctx, tab[i].atom);
        f->name = nm ? strdup(nm) : NULL;
        if (nm)
            JS_FreeCString(ctx, nm);
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
        if (JS_IsString(v))
            f->attr = ex_dup_str(ctx, v);
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
        if (!JS_IsUndefined(v))
            f->dflt = v;
        else
            JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "as");
        if (JS_IsString(v)) {
            const char* as = JS_ToCString(ctx, v);
            if (as) {
                if (!strcmp(as, "number"))
                    f->as_number = 1u;
                else if (!strcmp(as, "url"))
                    f->as_url = 1u;
                else if (!strcmp(as, "json"))
                    f->as_json = 1u;
                else {
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
    js_free(ctx, tab);
    tab = NULL;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_ex_ctor_keys, 1)) {
            ex_free_ctx(ctx, e);
            return JS_EXCEPTION;
        }
        ht = JS_GetPropertyStr(ctx, argv[1], "text");
        if (JS_IsFunction(ctx, ht))
            e->html_text = ht;
        else
            JS_FreeValue(ctx, ht);
    }

    proto = dyn_ctor_proto(ctx, new_target, dyn_ex_class_id);
    if (JS_IsException(proto)) {
        ex_free_ctx(ctx, e);
        return proto;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, (int)dyn_ex_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        ex_free_ctx(ctx, e);
        return obj;
    }
    JS_SetOpaque(obj, e);
    return obj;
fail:
    if (tab)
        js_free(ctx, tab);
    ex_free_ctx(ctx, e);
    return JS_EXCEPTION;
}

static JSValue dyn_ex_run(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ex_t* e = JS_GetOpaque2(ctx, this_val, dyn_ex_class_id);
    JSValue out, val, missing, base = JS_UNDEFINED;
    const char* base_s = NULL;
    size_t i;
    int ok = 1;
    uint32_t nmiss = 0;

    if (!e)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "run(doc[, { base }])");
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_run_keys, 1))
            return JS_EXCEPTION;
        base = JS_GetPropertyStr(ctx, argv[1], "base");
        if (JS_IsString(base)) {
            base_s = JS_ToCString(ctx, base);
            if (!base_s) {
                JS_FreeValue(ctx, base);
                return JS_EXCEPTION;
            }
        }
    }

    out = JS_NewObject(ctx);
    val = JS_NewObject(ctx);
    missing = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_IsException(val) || JS_IsException(missing))
        goto fail;

    for (i = 0; i < e->n; i++) {
        ex_field_t* f = &e->f[i];
        JSValue m, nodes, res;
        uint32_t cnt = 0, j;

        m = JS_GetPropertyStr(ctx, f->sel, "all");
        if (!JS_IsFunction(ctx, m)) {
            JS_FreeValue(ctx, m);
            JS_ThrowTypeError(ctx, "Extractor: `%s`.sel is not a Selector",
                f->name ? f->name : "?");
            goto fail;
        }
        nodes = JS_Call(ctx, m, f->sel, 1, (JSValueConst*)&argv[0]);
        JS_FreeValue(ctx, m);
        if (JS_IsException(nodes))
            goto fail;
        {
            JSValue lv = JS_GetPropertyStr(ctx, nodes, "length");
            JS_ToUint32(ctx, &cnt, lv);
            JS_FreeValue(ctx, lv);
        }

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
                JS_FreeValue(ctx, res);
                JS_FreeValue(ctx, nodes);
                JS_ThrowTypeError(ctx,
                    "Extractor: field `%s` wants text; pass { text: HTMLText } "
                    "to the constructor, or take attr/source instead",
                    f->name ? f->name : "?");
                goto fail;
            }
            JS_FreeValue(ctx, node);
            if (JS_IsException(piece)) {
                JS_FreeValue(ctx, res);
                JS_FreeValue(ctx, nodes);
                goto fail;
            }
            if (f->trim && !JS_IsUndefined(piece)) {
                const char* ts;
                size_t tn2, ofs;
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
            if (f->as_number && !JS_IsUndefined(piece)) {
                double d;
                if (JS_ToFloat64(ctx, &d, piece) < 0 || d != d) {
                    JSValue ex = JS_GetException(ctx);
                    JS_FreeValue(ctx, ex);
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;
                } else {
                    JS_FreeValue(ctx, piece);
                    piece = JS_NewFloat64(ctx, d);
                }
            }
            if (f->as_json && !JS_IsUndefined(piece)) {
                if (JS_IsString(piece)) {
                    const char* js;
                    size_t jl;
                    js = JS_ToCStringLen(ctx, &jl, piece);
                    if (js) {
                        JSValue jv = JS_ParseJSON(ctx, js, jl, "<extractor>");
                        JS_FreeCString(ctx, js);
                        if (JS_IsException(jv)) {
                            JS_FreeValue(ctx, JS_GetException(ctx));
                            JS_FreeValue(ctx, piece);
                            piece = JS_UNDEFINED;
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
                const char* us;
                size_t un;
                us = JS_ToCStringLen(ctx, &un, piece);
                if (!us || (!strncasecmp(us, "http://", 7) || !strncasecmp(us, "https://", 8))) {
                    if (us)
                        JS_FreeCString(ctx, us);
                } else {
                    char rbuf[1024];
                    long rl = base_s ? fe_resolve(base_s, us, rbuf,
                                           sizeof rbuf)
                                     : -1;
                    JS_FreeCString(ctx, us);
                    if (rl > 0) {
                        JSValue nv = JS_NewStringLen(ctx, rbuf, (size_t)rl);
                        JS_FreeValue(ctx, piece);
                        piece = nv;
                    } else {
                        JS_FreeValue(ctx, piece);
                        piece = JS_UNDEFINED;
                    }
                }
            }
            if (f->all)
                JS_SetPropertyUint32(ctx, res, j, piece);
            else {
                res = piece;
                break;
            }
        }
        JS_FreeValue(ctx, nodes);
        if (!f->all && cnt == 0)
            res = JS_UNDEFINED;

        if (!f->all && JS_IsUndefined(res) && !JS_IsUndefined(f->dflt))
            res = JS_DupValue(ctx, f->dflt);
        else if (f->all && cnt == 0 && !JS_IsUndefined(f->dflt)) {
            JS_FreeValue(ctx, res);
            res = JS_DupValue(ctx, f->dflt);
        }

        if (JS_IsUndefined(f->dflt) && (cnt == 0 || (!f->all && JS_IsUndefined(res))) && f->required) {
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
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, missing);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_ex_proto[] = {
    JS_CFUNC_DEF("run", 1, dyn_ex_run),
};

#define FE_MAX_HOSTS 256

typedef struct {
    char* host;
    double next_ok_ms;
    rb_t* robots;
    int robots_tried;
    int robots_unreachable;
    int robots_ok;
    double robots_next_ms;
} fe_host_t;

#define FE_CACHE_MAX_ENTRIES 32
#define FE_CACHE_MAX_BYTES (4u << 20)
#define FE_CACHE_BODY_CAP (1u << 20)

typedef struct {
    char *url, *etag, *lm, *ct;
    char* body;
    size_t blen;
} fe_cache_ent_t;

typedef struct {
    char* agent;
    int robots_on, retries, max_redirects;
    double robots_ttl_ms;
    int allow_private_hosts;
    int revalidate;
    int allow_insecure_downgrade;
    double min_delay_ms, max_body;
    char* proxy;
    char* ca;
    int pool_size;
    uint64_t rng;
    fe_host_t* hosts;
    size_t n_hosts, cap_hosts;
    double fetched, skipped_robots, retried, throttled_ms, bytes;
    double revalidated, saved_bytes;
    fe_cache_ent_t cache[FE_CACHE_MAX_ENTRIES];
    size_t n_cache;
#ifdef CONFIG_TLS
    void* tls_ctx;
#endif
} fe_t;

static JSClassID dyn_fe_class_id;

static fe_t* fe_live(JSContext* ctx, JSValueConst fv)
{
    JSClassID id = (JSClassID)0;
    DynResource* r = (DynResource*)JS_GetAnyOpaque(fv, &id);
    (void)ctx;
    if (!r || id != dyn_fe_class_id || r->closed)
        return NULL;
    return (fe_t*)r->native;
}

static void fe_cache_clear(fe_cache_ent_t* e)
{
    free(e->url);
    free(e->etag);
    free(e->lm);
    free(e->ct);
    free(e->body);
    memset(e, 0, sizeof(*e));
}

static fe_cache_ent_t* fe_cache_find(fe_t* f, const char* url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url))
            return &f->cache[i];
    return NULL;
}

static char* fe_strdup_n(const char* s, size_t n)
{
    char* o;
    if (n > FE_CACHE_BODY_CAP)
        return NULL;
    o = (char*)malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

static void fe_cache_evict_url(fe_t* f, const char* url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url)) {
            fe_cache_clear(&f->cache[i]);
            memmove(&f->cache[i], &f->cache[i + 1],
                (f->n_cache - i - 1) * sizeof(*f->cache));
            f->n_cache--;
            return;
        }
}

static fe_cache_ent_t* fe_cache_put(fe_t* f, const char* url,
    const char* etag, const char* lm,
    const char* ct,
    const char* body, size_t blen)
{
    fe_cache_ent_t* e;
    double tot;
    size_t i;

    if (!etag && !lm)
        return NULL;
    if (!body || blen > FE_CACHE_BODY_CAP)
        return NULL;
    fe_cache_evict_url(f, url);

    tot = (double)(blen + strlen(url));
    for (i = 0; i < f->n_cache; i++)
        tot += (double)f->cache[i].blen;
    while ((f->n_cache >= FE_CACHE_MAX_ENTRIES || tot > (double)FE_CACHE_MAX_BYTES) && f->n_cache) {
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
    if (!e->url) {
        f->n_cache--;
        memset(e, 0, sizeof(*e));
        return NULL;
    }
    e->etag = etag ? strdup(etag) : NULL;
    e->lm = lm ? strdup(lm) : NULL;
    e->ct = ct ? strdup(ct) : NULL;
    if ((etag && !e->etag) || (lm && !e->lm) || (ct && !e->ct)) {
        fe_cache_evict_url(f, url);
        return NULL;
    }
    e->body = fe_strdup_n(body, blen);
    if (!e->body) {
        fe_cache_evict_url(f, url);
        return NULL;
    }
    e->blen = blen;
    return e;
}

static void fe_dispose(void* native)
{
    fe_t* f = (fe_t*)native;
    size_t i;
    if (!f)
        return;
    for (i = 0; i < f->n_hosts; i++) {
        free(f->hosts[i].host);
        if (f->hosts[i].robots)
            rb_free(f->hosts[i].robots);
    }
    free(f->hosts);
    for (i = 0; i < f->n_cache; i++)
        fe_cache_clear(&f->cache[i]);
    free(f->agent);
    free(f->proxy);
    free(f->ca);
#ifdef CONFIG_TLS
    dyn_tls_ctx_free((dyn_tls_ctx_t*)f->tls_ctx);
#endif
    free(f);
}

static const JSClassDef dyn_fe_class = {
    "Fetcher",
    .finalizer = dyn_res_finalizer,
};

static int fe_scheme_is_https(const char* url)
{
    return !strncmp(url, "https://", 8);
}

static int fe_split(const char* url, char* host, size_t cap, const char** path)
{
    const char *p = url, *h;
    size_t n;
    int https;
    if (!strncmp(p, "http://", 7)) {
        p += 7;
        https = 0;
    } else if (!strncmp(p, "https://", 8)) {
        p += 8;
        https = 1;
    } else
        return -1;
    h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && (unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7F)
        p++;
    if (*p && ((unsigned char)*p < 0x20 || *p == 0x7F))
        return -1;
    n = (size_t)(p - h);
    if (n == 0 || n + 1 > cap)
        return -1;
    memcpy(host, h, n);
    host[n] = 0;
    *path = *p ? p : "/";
    return https;
}

static void fe_rm_dots(char* s)
{
    size_t seg[128], ns = 0, i = 0, o = 0, rooted = s[0] == '/';
    if (rooted)
        s[o++] = s[i++];
    while (s[i]) {
        if (s[i] == '/') {
            i++;
            continue;
        }
        if (s[i] == '.' && (s[i + 1] == '/' || !s[i + 1])) {
            i += s[i + 1] ? 2 : 1;
            continue;
        }
        if (s[i] == '.' && s[i + 1] == '.' && (s[i + 2] == '/' || !s[i + 2])) {
            i += s[i + 2] ? 3 : 2;
            o = ns ? seg[--ns] : (rooted ? 1u : 0u);
            continue;
        }
        if (ns < countof(seg))
            seg[ns++] = o;
        while (s[i] && s[i] != '/')
            s[o++] = s[i++];
        if (s[i])
            s[o++] = s[i++];
    }
    s[o] = 0;
}

static long fe_resolve(const char* base, const char* ref, char* out, size_t cap)
{
    const char *p, *auth, *bp0, *bpend, *rf;
    size_t slen, authlen, rn, wr;
    int kind;

    for (p = ref; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f)
            return -1;

    if (!strncmp(base, "http://", 7)) {
        slen = 4;
    } else if (!strncmp(base, "https://", 8)) {
        slen = 5;
    } else
        return -1;

    auth = base + slen + 3;
    p = strchr(auth, '/');
    if (!p)
        p = strchr(auth, '?');
    authlen = p ? (size_t)(p - auth) : strlen(auth);
    bp0 = auth + authlen;
    bpend = bp0 + strcspn(bp0, "#");
    rf = strchr(ref, '#');
    rn = rf ? (size_t)(rf - ref) : strlen(ref);

    kind = 4;
    if (!strncmp(ref, "//", 2)) {
        kind = 1;
    } else if (rn > 0 && ref[0] == '/') {
        kind = 2;
    } else if (rn == 0 || ref[0] == '?') {
        kind = 3;
    } else {
        p = ref;
        if ((*p | 32) >= 'a' && (*p | 32) <= 'z') {
            const char* q = p + 1;
            while ((size_t)(q - ref) < rn && (((*q | 32) >= 'a' && (*q | 32) <= 'z') || (*q >= '0' && *q <= '9') || *q == '+' || *q == '-' || *q == '.'))
                q++;
            if ((size_t)(q - ref) < rn && *q == ':')
                kind = 0;
        }
    }

#define FE_PUT(src_, n_)                \
    do {                                \
        if ((n_) + 1 > cap - wr)        \
            return -1;                  \
        memcpy(out + wr, (src_), (n_)); \
        wr += (n_);                     \
        out[wr] = 0;                    \
    } while (0)

    if (kind == 1) {
        size_t k;
        memcpy(out, base, slen + 1);
        wr = slen + 1;
        out[wr] = 0;
        FE_PUT(ref, rn);
        k = slen + 3;
        while (out[k] && out[k] != '/')
            k++;
        if (out[k]) {
            fe_rm_dots(out + k);
            wr = k + strlen(out + k);
        } else {
            wr = strlen(out);
        }
        return (long)wr;
    }

    if (kind == 0) {
        if (rn >= cap)
            return -1;
        memcpy(out, ref, rn);
        out[rn] = 0;
        return (long)rn;
    }

    if (slen + 3 + authlen + 2 > cap)
        return -1;
    memcpy(out, base, slen + 3);
    wr = slen + 3;
    memcpy(out + wr, auth, authlen);
    wr += authlen;

    switch (kind) {
    case 2:
        FE_PUT("/", 1);
        FE_PUT(ref, rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        break;
    case 3: {
        const char* bq = memchr(bp0, '?', (size_t)(bpend - bp0));
        size_t pl = bq ? (size_t)(bq - bp0) : (size_t)(bpend - bp0);
        if (!pl)
            FE_PUT("/", 1);
        else
            FE_PUT(bp0, pl);
        FE_PUT(ref, rn);
        break;
    }
    case 4: {
        const char *be = bp0, *bs, *rq;
        while (be < bpend && *be != '?')
            be++;
        bs = be;
        while (bs > bp0 && *--bs != '/')
            ;
        if (*bs == '/')
            FE_PUT(bp0, (size_t)(bs - bp0) + 1);
        else
            FE_PUT("/", 1);
        rq = memchr(ref, '?', rn);
        FE_PUT(ref, rq ? (size_t)(rq - ref) : rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        if (rq)
            FE_PUT(rq, rn - (size_t)(rq - ref));
        break;
    }
    }
    out[wr] = 0;
    return (long)wr;
#undef FE_PUT
}

static int fe_ip4_private(const unsigned char b[4])
{
    if (b[0] == 0)
        return 1;
    if (b[0] == 10)
        return 1;
    if (b[0] == 100 && (b[1] & 0xc0) == 64)
        return 1;
    if (b[0] == 127)
        return 1;
    if (b[0] == 169 && b[1] == 254)
        return 1;
    if (b[0] == 172 && (b[1] & 0xf0) == 16)
        return 1;
    if (b[0] == 192 && b[1] == 168)
        return 1;
    if (b[0] == 192 && b[1] == 0 && b[2] == 0)
        return 1;
    if (b[0] >= 224)
        return 1;
    return 0;
}

static int fe_ip4_val(const char** pp, unsigned long long* out)
{
    const char* p = *pp;
    unsigned long long v = 0;
    int base = 10, any = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') {
        base = 8;
        p += 1;
    }
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        any = 1;
        v = v * (unsigned long long)base + (unsigned long long)d;
        if (v > 0xffffffffULL)
            return 0;
        p++;
    }
    if (!any)
        return 0;
    *pp = p;
    *out = v;
    return 1;
}

static int fe_ip4_parse(const char* s, unsigned char b[4])
{
    unsigned long long v[4], acc;
    const char* p = s;
    int n = 0, i;
    while (n < 4) {
        if (!fe_ip4_val(&p, &v[n]))
            return 0;
        n++;
        if (*p == '.') {
            p++;
            continue;
        }
        break;
    }
    if (*p == '.') {
        p++;
        if (*p != '\0')
            return 0;
    } else if (*p != '\0') {
        return 0;
    }
    if (n == 4) {
        for (i = 0; i < 4; i++)
            if (v[i] > 255ULL)
                return 0;
        acc = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    } else {
        unsigned long long lim = 1ULL << (8 * (4 - n + 1));
        for (i = 0; i < n - 1; i++)
            if (v[i] > 255ULL)
                return 0;
        if (v[n - 1] >= lim)
            return 0;
        acc = v[n - 1];
        for (i = n - 2; i >= 0; i--)
            acc |= v[i] << (8 * (3 - i));
    }
    b[0] = (unsigned char)((acc >> 24) & 0xff);
    b[1] = (unsigned char)((acc >> 16) & 0xff);
    b[2] = (unsigned char)((acc >> 8) & 0xff);
    b[3] = (unsigned char)(acc & 0xff);
    return 1;
}

static int fe_hex4(const char** pp, unsigned* out)
{
    const char* p = *pp;
    unsigned v = 0;
    int k = 0;
    while (k < 4) {
        int c = p[k], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        v = (v << 4) | (unsigned)d;
        k++;
    }
    if (!k)
        return 0;
    *pp = p + k;
    *out = v;
    return 1;
}

static int fe_ip6_parse(const char* s, unsigned char b[16])
{
    unsigned g[8];
    int n = 0, dc = -1, i, fill;
    const char* p = s;
    memset(g, 0, sizeof g);
    if (*p == ':') {
        if (p[1] != ':')
            return 0;
        dc = 0;
        p += 2;
        if (*p == '\0') {
            memset(b, 0, 16);
            return 1;
        }
    }
    while (*p) {
        if (n >= 8)
            return 0;
        if (*p == ':') {
            if (p[1] == ':') {
                if (dc >= 0)
                    return 0;
                dc = n;
                p += 2;
                if (*p == '\0')
                    break;
                continue;
            }
            if (n == 0)
                return 0;
            p++;
            if (*p == '\0')
                return 0;
            continue;
        }
        {
            unsigned char v4[4];
            if (fe_ip4_parse(p, v4)) {
                if (n > 6)
                    return 0;
                g[n++] = ((unsigned)v4[0] << 8) | v4[1];
                g[n++] = ((unsigned)v4[2] << 8) | v4[3];
                break;
            }
        }
        if (!fe_hex4(&p, &g[n]))
            return 0;
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')
            || (*p >= 'A' && *p <= 'F'))
            return 0;
        n++;
    }
    if (dc < 0) {
        if (n != 8)
            return 0;
        for (i = 0; i < 8; i++) {
            b[2 * i] = (unsigned char)(g[i] >> 8);
            b[2 * i + 1] = (unsigned char)(g[i] & 0xff);
        }
        return 1;
    }
    if (n > 7)
        return 0;
    fill = 8 - n;
    for (i = 0; i < dc; i++) {
        b[2 * i] = (unsigned char)(g[i] >> 8);
        b[2 * i + 1] = (unsigned char)(g[i] & 0xff);
    }
    memset(b + 2 * dc, 0, (size_t)(2 * fill));
    for (i = dc; i < n; i++) {
        int j = i + fill;
        b[2 * j] = (unsigned char)(g[i] >> 8);
        b[2 * j + 1] = (unsigned char)(g[i] & 0xff);
    }
    return 1;
}

static int fe_ip6_private(const unsigned char* b)
{
    static const unsigned char z12[12] = { 0 };
    int i;
    for (i = 0; i < 16; i++)
        if (b[i])
            break;
    if (i == 16)
        return 1;
    if (i == 15 && b[15] == 1)
        return 1;
    if (memcmp(b, z12, 10) == 0 && b[10] == 0xff && b[11] == 0xff)
        return fe_ip4_private(b + 12);
    if (memcmp(b, z12, 12) == 0)
        return fe_ip4_private(b + 12);
    if ((b[0] & 0xfe) == 0xfc)
        return 1;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
        return 1;
    return 0;
}

static int fe_host_literal_private(const char* h)
{
    unsigned char b[4], b6[16];
    if (fe_ip4_parse(h, b))
        return fe_ip4_private(b);
    if (fe_ip6_parse(h, b6))
        return fe_ip6_private(b6);
    return 0;
}

static void fe_bare_host(const char* auth, char* out, size_t cap)
{
    const char* h = auth;
    const char* q;
    char* p;
    size_t n;
    for (q = auth; *q; q++)
        if (*q == '@')
            h = q + 1;
    n = strlen(h);
    if (n && h[0] == '[') {
        const char* cl = strchr(h, ']');
        if (cl) {
            h++;
            n = (size_t)(cl - h);
        }
    } else {
        const char* c = strchr(h, ':');
        if (c)
            n = (size_t)(c - h);
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, h, n);
    out[n] = 0;
    for (p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
}

static int fe_host_is_private(const char* host)
{
    static const char* const internal_sfx[] = { ".local", ".internal", ".lan" };
    char bare[300];
    size_t n, i;
    fe_bare_host(host, bare, sizeof bare);
    if (bare[0] == '\0')
        return 1;
    if (fe_host_literal_private(bare))
        return 1;
    if (!strcmp(bare, "localhost") || !strcmp(bare, "localhost.localdomain"))
        return 1;
    if (!strchr(bare, '.'))
        return 1;
    n = strlen(bare);
    for (i = 0; i < countof(internal_sfx); i++) {
        size_t k = strlen(internal_sfx[i]);
        if (n > k && !strcmp(bare + n - k, internal_sfx[i]))
            return 1;
    }
    return 0;
}

static fe_host_t* fe_host(fe_t* f, const char* host)
{
    size_t i;
    for (i = 0; i < f->n_hosts; i++)
        if (!strcmp(f->hosts[i].host, host))
            return &f->hosts[i];
    if (f->n_hosts >= FE_MAX_HOSTS) {
        fe_host_t* v = &f->hosts[0];
        free(v->host);
        if (v->robots)
            rb_free(v->robots);
        memset(v, 0, sizeof(*v));
        v->host = strdup(host);
        return v->host ? v : NULL;
    }
    if (f->n_hosts == f->cap_hosts) {
        size_t nc = f->cap_hosts ? f->cap_hosts * 2 : 8;
        fe_host_t* nh = (fe_host_t*)realloc(f->hosts, nc * sizeof(*nh));
        if (!nh)
            return NULL;
        f->hosts = nh;
        f->cap_hosts = nc;
    }
    memset(&f->hosts[f->n_hosts], 0, sizeof(fe_host_t));
    f->hosts[f->n_hosts].host = strdup(host);
    if (!f->hosts[f->n_hosts].host)
        return NULL;
    return &f->hosts[f->n_hosts++];
}

static void fe_sleep_ms(double ms)
{
    if (ms <= 0)
        return;
    if (ms > 60000)
        ms = 60000;
    poll(NULL, 0, (int)ms);
}

static int64_t fe_http_date(const char* s)
{
    static const char* const mon[12] = { "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec" };
    int64_t days, era, yoe, doy, doe;
    long y;
    int day = 0, m = -1, yr = 0, hr = 0, mi = 0, se = 0, i;

    if (!s)
        return -1;
    while (*s && *s != ',')
        s++;
    if (*s != ',')
        return -1;
    s++;
    if (*s++ != ' ')
        return -1;
    for (i = 0; i < 2; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        day = day * 10 + (s[i] - '0');
    }
    if (day < 1 || day > 31)
        return -1;
    s += 2;
    if (*s++ != ' ')
        return -1;
    for (i = 0; i < 12; i++)
        if (!strncasecmp(s, mon[i], 3)) {
            m = i;
            break;
        }
    if (m < 0)
        return -1;
    s += 3;
    if (*s++ != ' ')
        return -1;
    for (i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        yr = yr * 10 + (s[i] - '0');
    }
    if (yr < 1601 || yr > 9999)
        return -1;
    s += 4;
    if (*s++ != ' ')
        return -1;
    for (i = 0; i < 3; i++) {
        int* out = i == 0 ? &hr : i == 1 ? &mi
                                         : &se;
        if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9')
            return -1;
        *out = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
        if (i < 2) {
            if (*s != ':')
                return -1;
            s++;
        }
    }
    if (hr > 23 || mi > 59 || se > 60)
        return -1;
    if (strncmp(s, " GMT", 4) && strncmp(s, " UT", 3))
        return -1;

    y = yr;
    y -= m < 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return days * 86400 + hr * 3600 + mi * 60 + se;
}

static int fe_cc_no_store(const char* cc)
{
    const char* p = cc;
    if (!p)
        return 0;
    while (*p && (size_t)(p - cc) < 512) {
        if (!strncasecmp(p, "no-store", 8)) {
            const char* q = p + 8;
            if (!*q || *q == ',' || *q == ';' || *q == ' ' || *q == '\t')
                return 1;
        }
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    return 0;
}

static double fe_rand01(fe_t* f)
{
    return (double)(dyn_splitmix64(&f->rng) >> 11) / 9007199254740992.0;
}

static JSValue fe_raw(JSContext* ctx, fe_t* f, JSValueConst client,
    const char* url, JSValueConst uh,
    const char* if_none_match, const char* if_mod_since)
{
    JSValue a[4], ret, hdrs;
    JSAtom m;

    a[0] = JS_NewString(ctx, "GET");
    a[1] = JS_NewString(ctx, url);
    a[2] = JS_UNDEFINED;
    hdrs = JS_NewObject(ctx);
    if (JS_IsObject(uh)) {
        JSPropertyEnum* tab = NULL;
        uint32_t n = 0, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, uh,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            == 0) {
            for (i = 0; i < n; i++) {
                JSValue v = JS_GetProperty(ctx, uh, tab[i].atom);
                if (JS_IsException(v)) {
                    for (; i < n; i++)
                        JS_FreeAtom(ctx, tab[i].atom);
                    js_free(ctx, tab);
                    JS_FreeValue(ctx, a[0]);
                    JS_FreeValue(ctx, a[1]);
                    JS_FreeValue(ctx, hdrs);
                    return JS_EXCEPTION;
                }
                {
                    size_t vn = 0, b;
                    const char* vs = JS_ToCStringLen(ctx, &vn, v);
                    int bad = 0;
                    if (!vs) {
                        JS_FreeValue(ctx, v);
                        for (; i < n; i++)
                            JS_FreeAtom(ctx, tab[i].atom);
                        js_free(ctx, tab);
                        JS_FreeValue(ctx, a[0]);
                        JS_FreeValue(ctx, a[1]);
                        JS_FreeValue(ctx, hdrs);
                        return JS_EXCEPTION;
                    }
                    for (b = 0; b < vn; b++)
                        if (vs[b] == '\r' || vs[b] == '\n' || vs[b] == '\0') {
                            bad = 1;
                            break;
                        }
                    JS_FreeCString(ctx, vs);
                    if (bad) {
                        const char* ks = JS_AtomToCString(ctx, tab[i].atom);
                        JS_ThrowTypeError(ctx,
                            "Fetcher: header \"%s\" value contains CR, LF or "
                            "NUL -- a value rides to the wire byte-for-byte "
                            "and would split the request",
                            ks ? ks : "?");
                        if (ks)
                            JS_FreeCString(ctx, ks);
                        JS_FreeValue(ctx, v);
                        for (; i < n; i++)
                            JS_FreeAtom(ctx, tab[i].atom);
                        js_free(ctx, tab);
                        JS_FreeValue(ctx, a[0]);
                        JS_FreeValue(ctx, a[1]);
                        JS_FreeValue(ctx, hdrs);
                        return JS_EXCEPTION;
                    }
                }
                JS_DefinePropertyValue(ctx, hdrs, tab[i].atom, v,
                    JS_PROP_C_W_E);
            }
            for (i = 0; i < n; i++)
                JS_FreeAtom(ctx, tab[i].atom);
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
    JS_DefinePropertyValueStr(ctx, hdrs, "User-Agent",
        JS_NewString(ctx, f->agent), JS_PROP_C_W_E);
    a[3] = hdrs;
    m = JS_NewAtom(ctx, "request");
    ret = JS_Invoke(ctx, client, m, 4, (JSValueConst*)a);
    JS_FreeAtom(ctx, m);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
    JS_FreeValue(ctx, a[3]);
    return ret;
}

static int fe_num_prop(JSContext* ctx, JSValueConst o, const char* k, int dflt,
    int* out)
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

static int fe_strict_int_prop(JSContext* ctx, JSValueConst o, const char* k,
    int dflt, int* out)
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
    if (!ok || !(d >= -2147483648.0 && d <= 2147483647.0) || d != (double)(int)d) {
        JS_ThrowTypeError(ctx, "Fetcher: %s must be an integer", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static int cr_int_prop(JSContext* ctx, JSValueConst o, const char* k,
    int dflt, int lo, int hi, int* out)
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
    is_int = JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) && !isnan(d) && d != HUGE_VAL && d != -HUGE_VAL && d == floor(d);
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

static int fe_status(JSContext* ctx, JSValueConst res)
{
    JSValue v = JS_GetPropertyStr(ctx, res, "status");
    int32_t st = 0;
    int rc;
    rc = JS_ToInt32(ctx, &st, v);
    JS_FreeValue(ctx, v);
    return rc ? -1 : (int)st;
}

static const char* fe_credential_header(JSContext* ctx, JSValueConst hv,
    char* buf, size_t cap)
{
    static const char* const cred[] = {
        "authorization", "cookie", "proxy-authorization", "set-cookie"
    };
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;

    buf[0] = '\0';
    if (!JS_IsObject(hv))
        return NULL;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, hv,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return NULL;
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        size_t j;
        if (!k)
            continue;
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
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    return buf[0] ? buf : NULL;
}

static const char* fe_header(JSContext* ctx, JSValueConst res, const char* want,
    JSValue* hold)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    const char* out = NULL;
    *hold = JS_UNDEFINED;
    if (!JS_IsObject(h)) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) == 0) {
        for (i = 0; i < n; i++) {
            const char* k = JS_AtomToCString(ctx, tab[i].atom);
            int hit = k && !strcasecmp(k, want);
            if (k)
                JS_FreeCString(ctx, k);
            if (hit) {
                *hold = JS_GetProperty(ctx, h, tab[i].atom);
                out = JS_ToCString(ctx, *hold);
                break;
            }
        }
        for (i = 0; i < n; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, h);
    return out;
}

static char* fe_headers_all(JSContext* ctx, JSValueConst res, const char* want)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    size_t cap = 0, len = 0;
    char* out = NULL;
    if (!JS_IsObject(h)) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) != 0) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int hit = k && !strcasecmp(k, want);
        if (k)
            JS_FreeCString(ctx, k);
        if (!hit)
            continue;
        {
            JSValue v = JS_GetProperty(ctx, h, tab[i].atom);
            const char* vs = JS_ToCString(ctx, v);
            if (vs) {
                size_t vl = strlen(vs);
                if (len + vl + 2 > cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    char* no;
                    while (nc < len + vl + 2)
                        nc *= 2;
                    no = (char*)realloc(out, nc);
                    if (!no) {
                        JS_FreeCString(ctx, vs);
                        JS_FreeValue(ctx, v);
                        break;
                    }
                    out = no;
                    cap = nc;
                }
                if (len)
                    out[len++] = '\n';
                memcpy(out + len, vs, vl);
                len += vl;
                JS_FreeCString(ctx, vs);
            }
            JS_FreeValue(ctx, v);
        }
    }
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, h);
    if (out)
        out[len] = '\0';
    return out;
}

static void fe_dirs_add(char** out, size_t* len, size_t* cap,
    const char* tok, size_t tl)
{
    size_t i, b = 0;
    if (!*out && tl) {
        size_t nc = 64;
        while (nc < tl + 1)
            nc *= 2;
        *out = (char*)malloc(nc);
        if (!*out)
            return;
        *cap = nc;
    }
    for (i = 0; i < *len; i++) {
        if ((*out)[i] == '\n') {
            if (i - b == tl && !strncasecmp(*out + b, tok, tl))
                return;
            b = i + 1;
        }
    }
    if (*len - b == tl && !strncasecmp(*out + b, tok, tl))
        return;
    if (*len + tl + 2 > *cap) {
        size_t nc = *cap * 2;
        char* no;
        while (nc < *len + tl + 2)
            nc *= 2;
        no = (char*)realloc(*out, nc);
        if (!no)
            return;
        *out = no;
        *cap = nc;
    }
    if (*len)
        (*out)[(*len)++] = '\n';
    for (i = 0; i < tl; i++) {
        char c = tok[i];
        (*out)[*len] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        (*len)++;
    }
    (*out)[*len] = '\0';
}

static char* fe_parse_robots_tag(const char* joined, const char* agent)
{
    size_t agt = agent ? rb_token_len(agent, strlen(agent)) : 0;
    char* out = NULL;
    size_t len = 0, cap = 0, i = 0;
    while (joined && joined[i]) {
        size_t b = i;
        while (joined[i] && joined[i] != ',' && joined[i] != '\n')
            i++;
        {
            size_t t0 = b, t1 = i;
            int applies = 1;
            while (t0 < t1 && (joined[t0] == ' ' || joined[t0] == '\t'))
                t0++;
            while (t1 > t0 && (joined[t1 - 1] == ' ' || joined[t1 - 1] == '\t'))
                t1--;
            if (t1 > t0) {
                const char* c = (const char*)memchr(joined + t0, ':',
                    t1 - t0);
                if (c) {
                    size_t bn = (size_t)(c - (joined + t0));
                    int arg_directive = (bn == 11 && !strncasecmp(joined + t0, "max-snippet", 11)) || (bn == 17 && !strncasecmp(joined + t0, "max-image-preview", 17)) || (bn == 17 && !strncasecmp(joined + t0, "max-video-preview", 17)) || (bn == 17 && !strncasecmp(joined + t0, "unavailable_after", 17));
                    if (!arg_directive && agent) {
                        size_t rs = t0 + bn + 1;
                        while (rs < t1 && (joined[rs] == ' ' || joined[rs] == '\t'))
                            rs++;
                        applies = (bn == 1 && joined[t0] == '*');
                        if (!applies && bn == agt && !strncasecmp(joined + t0, agent, agt))
                            applies = 1;
                        t0 = rs;
                    }
                }
                if (applies && t1 > t0) {
                    if (t1 - t0 == 4 && !strncasecmp(joined + t0, "none", 4)) {
                        fe_dirs_add(&out, &len, &cap, "noindex", 7);
                        fe_dirs_add(&out, &len, &cap, "nofollow", 8);
                    } else if (t1 - t0 == 3 && !strncasecmp(joined + t0, "all", 3)) {
                    } else {
                        fe_dirs_add(&out, &len, &cap, joined + t0, t1 - t0);
                    }
                }
            }
        }
        if (joined[i] == ',' || joined[i] == '\n')
            i++;
    }
    return out;
}

static int fe_link_rel_canonical(const char* p, size_t n, size_t eq)
{
    size_t j = eq + 1;
    while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == '"' || p[j] == '\''))
        j++;
    while (j < n) {
        size_t k = j;
        while (k < n && p[k] != ',' && p[k] != ';' && p[k] != ' ' && p[k] != '\t' && p[k] != '"' && p[k] != '\'')
            k++;
        if (k - j == 9 && !strncasecmp(p + j, "canonical", 9))
            return 1;
        j = k;
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ',' || p[j] == '"' || p[j] == '\''))
            j++;
        if (j >= n || p[j] == ';')
            break;
    }
    return 0;
}

static void fe_link_canonical(JSContext* ctx, JSValueConst res,
    const char* base, JSValue out)
{
    char* all = fe_headers_all(ctx, res, "link");
    size_t i = 0;
    char* url = NULL;
    if (!all)
        return;
    while (all[i]) {
        if (all[i] == '<') {
            size_t e = i + 1, p, pend, k;
            while (all[e] && all[e] != '>' && all[e] != '\n')
                e++;
            if (all[e] != '>') {
                i++;
                continue;
            }
            p = e + 1;
            pend = p;
            while (all[pend] && all[pend] != ',' && all[pend] != '\n')
                pend++;
            for (k = p; k + 4 <= pend; k++)
                if ((all[k] == 'r' || all[k] == 'R') && (all[k + 1] == 'e' || all[k + 1] == 'E') && (all[k + 2] == 'l' || all[k + 2] == 'L') && all[k + 3] == '=' && fe_link_rel_canonical(all + p, pend - p, (k + 3) - p)) {
                    size_t u0 = i + 1, u1 = e;
                    while (u0 < u1 && (all[u0] == ' ' || all[u0] == '\t'))
                        u0++;
                    while (u1 > u0 && (all[u1 - 1] == ' ' || all[u1 - 1] == '\t'))
                        u1--;
                    url = (char*)malloc(u1 - u0 + 1);
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
        if (rl > 0 && (!strncmp(rbuf, "http://", 7) || !strncmp(rbuf, "https://", 8)))
            JS_DefinePropertyValueStr(ctx, out, "canonicalUrl",
                JS_NewStringLen(ctx, rbuf, (size_t)rl),
                JS_PROP_C_W_E);
    }
}

static int fe_load_robots(JSContext* ctx, fe_t* f, JSValueConst fv,
    JSValueConst client,
    JSValueConst uh, fe_host_t* h, int https)
{
    char url[600];
    char host0[300];
    JSValue res;
    int st = 0, hop;
    const double robots_ttl = f->robots_ttl_ms;
    snprintf(host0, sizeof host0, "%s", h->host);
    h->robots_tried = 1;
    snprintf(url, sizeof url, "%s%s/robots.txt",
        https ? "https://" : "http://", h->host);

    res = JS_UNDEFINED;
    for (hop = 0; hop < 3; hop++) {
        JS_FreeValue(ctx, res);
        res = fe_raw(ctx, f, client, url, uh, NULL, NULL);
        f = fe_live(ctx, fv);
        h = f ? fe_host(f, host0) : NULL;
        if (!h) {
            JS_FreeValue(ctx, res);
            return 0;
        }
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            h->robots_unreachable = 1;
            h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
            return 0;
        }
        st = fe_status(ctx, res);
        if (st < 0) {
            JS_FreeValue(ctx, res);
            return -1;
        }
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
            f = fe_live(ctx, fv);
            h = f ? fe_host(f, host0) : NULL;
            if (!h) {
                if (loc)
                    JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                return 0;
            }
            if (loc && fe_resolve(url, loc, nxt, sizeof nxt) > 0 && fe_split(nxt, host2, sizeof host2, &pp) >= 0) {
                ok = 1;
                if (!f->allow_private_hosts && fe_host_is_private(host2))
                    ok = 0;
                else if (fe_scheme_is_https(url) && !fe_scheme_is_https(nxt))
                    ok = 0;
            }
            if (loc)
                JS_FreeCString(ctx, loc);
            JS_FreeValue(ctx, hold);
            if (ok) {
                snprintf(url, sizeof url, "%s", nxt);
                continue;
            }
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
        const char* txt = JS_ToCStringLen(ctx, &bn, b);
        if (txt) {
            rb_t* r;
            f = fe_live(ctx, fv);
            h = f ? fe_host(f, host0) : NULL;
            if (!h) {
                JS_FreeCString(ctx, txt);
                JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, res);
                return 0;
            }
            r = (rb_t*)calloc(1, sizeof(rb_t));
            if (r) {
                r->delay = -1;
                if (rb_parse(r, txt, bn, f->agent) < 0) {
                    rb_free(r);
                    h->robots_unreachable = 1;
                } else {
                    if (h->robots)
                        rb_free(h->robots);
                    h->robots = r;
                    h->robots_ok = 1;
                }
            }
            JS_FreeCString(ctx, txt);
        }
        JS_FreeValue(ctx, b);
    } else if (st >= 500 && st < 600) {
        h->robots_unreachable = 1;
    } else if (st < 400 || st >= 600) {
        h->robots_unreachable = 1;
    }
    f = fe_live(ctx, fv);
    h = f ? fe_host(f, host0) : NULL;
    if (h)
        h->robots_next_ms = (double)dyn_timer_now_ms() + robots_ttl;
    JS_FreeValue(ctx, res);
    return 0;
}

static JSValue fe_finish_response(JSContext* ctx, JSValueConst fetcher,
    fe_t* f, const char* cur,
    JSValue res, int st)
{
    const double max_body = f->max_body;
    const int revalidate = f->revalidate;

    {
        JSValue b, hold_cl = JS_UNDEFINED;
        size_t bn = 0;
        const char* bs;

        {
            const char* clv = fe_header(ctx, res, "content-length",
                &hold_cl);
            if (clv) {
                double decl = strtod(clv, NULL);
                if (decl > max_body) {
                    JS_FreeCString(ctx, clv);
                    JS_FreeValue(ctx, hold_cl);
                    JS_FreeValue(ctx, res);
                    JS_ThrowRangeError(ctx,
                        "Fetcher: declared Content-Length %.0f exceeds "
                        "maxBodyBytes",
                        decl);
                    return JS_EXCEPTION;
                }
                JS_FreeCString(ctx, clv);
            }
            JS_FreeValue(ctx, hold_cl);
        }

        b = JS_GetPropertyStr(ctx, res, "body");
        bs = JS_ToCStringLen(ctx, &bn, b);
        if (!bs) {
            JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if ((double)bn > max_body) {
            JS_FreeCString(ctx, bs);
            JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, res);
            JS_ThrowRangeError(ctx,
                "Fetcher: body of %zu bytes exceeds maxBodyBytes", bn);
            goto out;
        }
        if (st == 304 && revalidate) {
            fe_cache_ent_t* ce;
            f = fe_live(ctx, fetcher);
            ce = f ? fe_cache_find(f, cur) : NULL;
            if (ce && ce->body) {
                f->revalidated += 1;
                f->saved_bytes += (double)ce->blen;
                f->bytes += (double)bn;
                JS_FreeCString(ctx, bs);
                JS_FreeValue(ctx, b);
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
        } else if (st >= 200 && st < 300) {
            const char *etg, *lmd, *ctv;
            JSValue he, hl, hc;
            etg = fe_header(ctx, res, "etag", &he);
            lmd = fe_header(ctx, res, "last-modified", &hl);
            ctv = fe_header(ctx, res, "content-type", &hc);
            if ((etg || lmd) && revalidate) {
                JSValue hcc = JS_UNDEFINED;
                const char* cc = fe_header(ctx, res, "cache-control",
                    &hcc);
                f = fe_live(ctx, fetcher);
                if (f && !fe_cc_no_store(cc))
                    fe_cache_put(f, cur, etg, lmd, ctv, bs, bn);
                if (cc)
                    JS_FreeCString(ctx, cc);
                JS_FreeValue(ctx, hcc);
            }
            if (etg)
                JS_FreeCString(ctx, etg);
            JS_FreeValue(ctx, he);
            if (lmd)
                JS_FreeCString(ctx, lmd);
            JS_FreeValue(ctx, hl);
            if (ctv)
                JS_FreeCString(ctx, ctv);
            JS_FreeValue(ctx, hc);
        }
        f = fe_live(ctx, fetcher);
        if (f)
            f->bytes += (double)bn;
        JS_FreeCString(ctx, bs);
        JS_FreeValue(ctx, b);
    }
    {
        char* xrt = fe_headers_all(ctx, res, "x-robots-tag");
        if (xrt) {
            char* dirs;
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
                        while (dirs[k] && dirs[k] != '\n')
                            k++;
                        JS_SetPropertyUint32(ctx, arr, ai++,
                            JS_NewStringLen(ctx, dirs + b, k - b));
                        if (dirs[k])
                            k++;
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
        const char* ctv = fe_header(ctx, res, "content-type", &hold_ct);
        JS_DefinePropertyValueStr(ctx, res, "contentType",
            JS_NewString(ctx, ctv ? ctv : ""),
            JS_PROP_C_W_E);
        if (ctv)
            JS_FreeCString(ctx, ctv);
        JS_FreeValue(ctx, hold_ct);
    }
    JS_DefinePropertyValueStr(ctx, res, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_FALSE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "notModified", JS_FALSE, JS_PROP_C_W_E);
    return res;
out:
    return JS_EXCEPTION;
}

static JSValue dyn_fe_get(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    fe_t* f;
    const char* url0 = NULL;
    char cur[1024], host[300];
    const char* path;
    int https, hop;
    JSValue res = JS_UNDEFINED, ret = JS_EXCEPTION, client, uh;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "get(url)");
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0)
        return JS_EXCEPTION;
    f = (fe_t*)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    if (!f) {
        JS_FreeCString(ctx, url0);
        return JS_EXCEPTION;
    }
    if (strlen(url0) + 1 > sizeof cur) {
        JS_FreeCString(ctx, url0);
        return JS_ThrowRangeError(ctx, "Fetcher: url is too long");
    }
    snprintf(cur, sizeof cur, "%s", url0);
    JS_FreeCString(ctx, url0);
    {
        char* frag = strchr(cur, '#');
        if (frag)
            *frag = 0;
    }
    client = JS_GetPropertyStr(ctx, this_val, "_client");
    uh = JS_GetPropertyStr(ctx, this_val, "_headers");

    for (hop = 0;; hop++) {
        fe_host_t* h;
        double wait, floor_ms;
        int attempt, st = 0;

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
        if (!h) {
            JS_ThrowOutOfMemory(ctx);
            goto out;
        }

        if (f->robots_on) {
            double now_ms = (double)dyn_timer_now_ms();
            if (!h->robots_tried || now_ms >= h->robots_next_ms) {
                if (fe_load_robots(ctx, f, this_val, client, uh, h, https) < 0)
                    goto out;
            }
            f = fe_live(ctx, this_val);
            if (!f) {
                JS_ThrowTypeError(ctx,
                    "Fetcher: closed while get() was in flight");
                goto out;
            }
            h = fe_host(f, host);
            if (!h) {
                JS_ThrowOutOfMemory(ctx);
                goto out;
            }
            if (h->robots_unreachable && h->robots_ok)
                h->robots_unreachable = 0;
            if (h->robots_unreachable || (h->robots && !rb_allows_path(h->robots, path, strlen(path)))) {
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

        floor_ms = f->min_delay_ms;
        if (h->robots && h->robots->delay > 0) {
            double cd = h->robots->delay * 1000.0;
            if (cd > floor_ms)
                floor_ms = cd;
        }
        wait = h->next_ok_ms - (double)dyn_timer_now_ms();
        if (wait > 0) {
            f->throttled_ms += wait;
            fe_sleep_ms(wait);
        }

        for (attempt = 0;; attempt++) {
            JSValue hold;
            const char* ra;
            double back;

            {
                fe_cache_ent_t* ce = f->revalidate ? fe_cache_find(f, cur) : NULL;
                res = fe_raw(ctx, f, client, cur, uh,
                    ce && ce->etag ? ce->etag : NULL,
                    ce && ce->lm ? ce->lm : NULL);
            }
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
                if (attempt >= f->retries)
                    goto out;
                JS_FreeValue(ctx, JS_GetException(ctx));
                st = 0;
            } else {
                st = fe_status(ctx, res);
                if (st < 0) {
                    JS_FreeValue(ctx, res);
                    goto out;
                }
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
            back = f->min_delay_ms * (double)(1 << (attempt < 10 ? attempt : 10));
            back *= 0.75 + 0.5 * fe_rand01(f);
            if (!JS_IsException(res)) {
                ra = fe_header(ctx, res, "retry-after", &hold);
                if (ra) {
                    double secs = strtod(ra, NULL);
                    if (!(secs > 0)) {
                        int64_t when = fe_http_date(ra);
                        if (when > 0) {
                            int64_t now = (int64_t)time(NULL);
                            secs = when > now ? (double)(when - now) : 0.0;
                        }
                    }
                    if (secs > 0)
                        back = secs * 1000.0;
                    JS_FreeCString(ctx, ra);
                }
                f = fe_live(ctx, this_val);
                if (!f) {
                    JS_FreeValue(ctx, hold);
                    JS_FreeValue(ctx, res);
                    JS_ThrowTypeError(ctx,
                        "Fetcher: closed while get() was in flight");
                    goto out;
                }
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

        if (st >= 300 && st < 400) {
            JSValue hold;
            const char* loc = fe_header(ctx, res, "location", &hold);
            f = fe_live(ctx, this_val);
            if (!f) {
                if (loc)
                    JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                JS_ThrowTypeError(ctx,
                    "Fetcher: closed while get() was in flight");
                goto out;
            }
            if (loc && hop < f->max_redirects) {
                char nxt[sizeof cur];
                int prev_https = fe_scheme_is_https(cur);
                if (fe_resolve(cur, loc, nxt, sizeof nxt) > 0 && strcmp(nxt, cur)) {
                    if (prev_https && !fe_scheme_is_https(nxt) && !f->allow_insecure_downgrade) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, res);
                        JS_ThrowRangeError(ctx,
                            "Fetcher: redirect downgrades https to http "
                            "(%.80s); pass allowInsecureDowngrade: true to "
                            "follow it",
                            nxt);
                        goto out;
                    }
                    snprintf(cur, sizeof cur, "%s", nxt);
                    JS_FreeCString(ctx, loc);
                    JS_FreeValue(ctx, hold);
                    JS_FreeValue(ctx, res);
                    res = JS_UNDEFINED;
                    continue;
                }
            }
            if (loc && hop >= f->max_redirects) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
                JS_FreeValue(ctx, res);
                JS_ThrowRangeError(ctx,
                    "Fetcher: more than %d redirects", f->max_redirects);
                goto out;
            }
            if (loc) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
            }
        }

        res = fe_finish_response(ctx, this_val, f, cur, res, st);
        if (JS_IsException(res))
            goto out;
        ret = res;
        goto out;
    }
out:
    JS_FreeValue(ctx, client);
    JS_FreeValue(ctx, uh);
    return ret;
}

static JSValue fe_promise_resolved(JSContext* ctx, JSValue val);
static JSValue fe_promise_rejected(JSContext* ctx, JSValue exc);

static JSValue dyn_fe_stats(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    fe_t* f = (fe_t*)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    JSValue o;
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "fetched", JS_NewFloat64(ctx, f->fetched), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "skippedByRobots", JS_NewFloat64(ctx, f->skipped_robots), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "retried", JS_NewFloat64(ctx, f->retried), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "throttledMs", JS_NewFloat64(ctx, f->throttled_ms), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bytes", JS_NewFloat64(ctx, f->bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "revalidated", JS_NewFloat64(ctx, f->revalidated), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "savedBytes", JS_NewFloat64(ctx, f->saved_bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "proxy",
        f->proxy ? JS_NewString(ctx, f->proxy) : JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ca",
        f->ca ? JS_NewString(ctx, f->ca) : JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "poolSize",
        JS_NewInt32(ctx, f->pool_size ? f->pool_size : 4), JS_PROP_C_W_E);
    return o;
}

static JSValue dyn_fe_get_async(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    JSValue res;
    if (argc < 1)
        return fe_promise_rejected(ctx, JS_ThrowTypeError(ctx, "getAsync(url)"));
    res = dyn_fe_get(ctx, this_val, argc, argv);
    if (JS_IsException(res))
        return fe_promise_rejected(ctx, JS_EXCEPTION);
    return fe_promise_resolved(ctx, res);
}

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} sc_sb_t;

static void sc_sb_init(sc_sb_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

static void sc_sb_free(sc_sb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static void sc_sb_put(sc_sb_t* b, const char* s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char* np;
        while (nc < b->n + n)
            nc *= 2;
        np = (char*)realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void sc_sb_puts(sc_sb_t* b, const char* s) { sc_sb_put(b, s, strlen(s)); }

static void sc_sb_json_str(sc_sb_t* b, const char* s)
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
            sc_sb_put(b, (const char*)&s[i], 1);
        }
    }
    sc_sb_put(b, "\"", 1);
}

static void cr_serialize_u64(sc_sb_t* b, const char* key, uint64_t v)
{
    char num[24];
    snprintf(num, sizeof num, ",\"%s\":%llu", key, (unsigned long long)v);
    sc_sb_puts(b, num);
}

static void cr_serialize_str(sc_sb_t* b, const char* key, const char* s)
{
    sc_sb_puts(b, ",\"");
    sc_sb_puts(b, key);
    sc_sb_puts(b, "\":");
    sc_sb_json_str(b, s ? s : "");
}

#define FE_STREAM_TIMEOUT_MS 15000
#define FE_STREAM_HDR_MAX (32 * 1024)
#define FE_STREAM_CHUNK_HDR 64

typedef struct {
    int fd;
#ifdef CONFIG_TLS
    dyn_tls_conn_t* tls;
#endif
} fe_conn_t;

static void fe_conn_close(fe_conn_t* c)
{
    if (!c)
        return;
#ifdef CONFIG_TLS
    dyn_tls_conn_free(c->tls);
    c->tls = NULL;
#endif
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static ssize_t fe_conn_recv(fe_conn_t* c, void* p, size_t n)
{
#ifdef CONFIG_TLS
    if (c->tls) {
        for (;;) {
            uint8_t cipher[16384];
            ssize_t r;
            int got = dyn_tls_read(c->tls, (uint8_t*)p, n);
            if (got > 0)
                return got;
            if (got < 0)
                return -1;
            r = recv(c->fd, cipher, sizeof cipher, 0);
            if (r <= 0)
                return r;
            if (dyn_tls_feed(c->tls, cipher, (size_t)r) != 0)
                return -1;
        }
    }
#endif
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static ssize_t fe_conn_send_all(fe_conn_t* c, const void* p, size_t n)
{
    const char* q = (const char*)p;
    size_t off = 0;
    while (off < n) {
        ssize_t s;
#ifdef CONFIG_TLS
        if (c->tls) {
            int w = dyn_tls_write(c->tls, (const uint8_t*)q + off, n - off);
            if (w < 0)
                return -1;
            s = w;
        } else
#endif
        {
            s = send(c->fd, q + off, n - off, 0);
            if (s < 0 && errno == EINTR)
                continue;
            if (s <= 0)
                return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)n;
}

static int fe_peer_private(int fd)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr*)&ss, &sl) != 0)
        return 1;
    if (ss.ss_family == AF_INET)
        return fe_ip4_private(
            (const unsigned char*)&((struct sockaddr_in*)&ss)->sin_addr);
    if (ss.ss_family == AF_INET6)
        return fe_ip6_private(
            (const unsigned char*)&((struct sockaddr_in6*)&ss)->sin6_addr);
    return 1;
}

static int fe_tcp_connect(const char* host, uint16_t port, int allow_private,
    char* why, size_t whyn)
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
        if (fd < 0)
            continue;
        fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        if (errno != EINPROGRESS) {
            errno = 0;
        } else {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int pr = poll(&pfd, 1, FE_STREAM_TIMEOUT_MS);
            if (pr <= 0) {
                snprintf(why, whyn, "connect to %.100s timed out", host);
                close(fd);
                fd = -1;
                continue;
            }
            {
                int err = 0;
                socklen_t el = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
                    snprintf(why, whyn, "connect to %.100s failed", host);
                    close(fd);
                    fd = -1;
                    continue;
                }
            }
        }
        if (!allow_private && fe_peer_private(fd)) {
            snprintf(why, whyn,
                "connect to %.100s reached a private/loopback address; "
                "refused",
                host);
            close(fd);
            fd = -1;
            continue;
        }
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl);
        {
            struct timeval tv;
            tv.tv_sec = FE_STREAM_TIMEOUT_MS / 1000;
            tv.tv_usec = (FE_STREAM_TIMEOUT_MS % 1000) * 1000;
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        {
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
static int fe_tls_connect(fe_t* f, fe_conn_t* c, const char* host,
    char* why, size_t whyn)
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
        c->tls = dyn_tls_conn_new((dyn_tls_ctx_t*)f->tls_ctx, host,
            terr, sizeof terr);
        if (!c->tls) {
            snprintf(why, whyn, "TLS handshake setup: %s", terr);
            return -1;
        }
    }
    for (;;) {
        uint8_t buf[16384];
        int st = dyn_tls_handshake(c->tls);
        ssize_t r;
        if (st < 0) {
            snprintf(why, whyn, "TLS handshake: %s",
                dyn_tls_error(c->tls) ? dyn_tls_error(c->tls) : "failed");
            return -1;
        }
        if (st == 1)
            return 0;
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

typedef struct {
    int status;
    char status_text[64];
    char* location;
    char* content_type;
    char* retry_after;
    long long clen;
    int chunked;
} fe_head_t;

static void fe_head_free(fe_head_t* h)
{
    free(h->location);
    free(h->content_type);
    free(h->retry_after);
}

static void fe_head_line(fe_head_t* h, char* line, size_t n)
{
    char* colon = (char*)memchr(line, ':', n);
    char* v;
    size_t vn;
    if (!colon)
        return;
    *colon = 0;
    v = colon + 1;
    vn = n - (size_t)(v - line);
    while (vn && (*v == ' ' || *v == '\t')) {
        v++;
        vn--;
    }
    while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t'))
        vn--;
    if (!strcasecmp(line, "location")) {
        free(h->location);
        h->location = strndup(v, vn);
    } else if (!strcasecmp(line, "content-type")) {
        free(h->content_type);
        h->content_type = strndup(v, vn);
    } else if (!strcasecmp(line, "retry-after")) {
        free(h->retry_after);
        h->retry_after = strndup(v, vn);
    } else if (!strcasecmp(line, "content-length"))
        h->clen = strtoll(v, NULL, 10);
    else if (!strcasecmp(line, "transfer-encoding")) {
        size_t i = 0;
        while (i + 7 <= vn) {
            if (!strncasecmp(v + i, "chunked", 7)) {
                char a = (i > 0 && v[i - 1] != ' ' && v[i - 1] != '\t' && v[i - 1] != ',') ? 'x' : ' ';
                char b = (i + 7 < vn && v[i + 7] != ' ' && v[i + 7] != '\t' && v[i + 7] != ',') ? 'x' : ' ';
                if (a == ' ' && b == ' ') {
                    h->chunked = 1;
                    break;
                }
            }
            i++;
        }
    }
}

static const char* fe_memfind(const char* hay, size_t hlen,
    const char* needle, size_t nlen)
{
    size_t i;
    if (nlen == 0)
        return hay;
    if (hlen < nlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

static int fe_read_head(fe_conn_t* c, fe_head_t* h, char* why, size_t whyn,
    uint8_t** pextra, size_t* pextra_len)
{
    char* buf = (char*)malloc(FE_STREAM_HDR_MAX + 1);
    size_t n = 0, scanned = 0;
    int skips = 0;
    *pextra = NULL;
    *pextra_len = 0;
    if (!buf) {
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    memset(h, 0, sizeof *h);
    h->clen = -1;
    for (;;) {
        const char* marker = fe_memfind(buf + scanned, n - scanned,
            "\r\n\r\n", 4);
        while (!marker && n < FE_STREAM_HDR_MAX) {
            ssize_t r;
            scanned = n > 3 ? n - 3 : 0;
            r = fe_conn_recv(c, buf + n, FE_STREAM_HDR_MAX - n);
            if (r < 0) {
                snprintf(why, whyn, "receiving response head failed");
                goto bad;
            }
            if (r == 0) {
                snprintf(why, whyn, "connection closed before response head");
                goto bad;
            }
            n += (size_t)r;
            marker = fe_memfind(buf + scanned, n - scanned, "\r\n\r\n", 4);
        }
        if (!marker) {
            snprintf(why, whyn, "response head exceeds %d bytes",
                FE_STREAM_HDR_MAX);
            goto bad;
        }
        {
            size_t head_end = (size_t)(marker - buf) + 4;
            char* eol = (char*)memchr(buf, '\n', head_end);
            size_t l1 = eol ? (size_t)(eol - buf) : head_end;
            const char *p, *sp;
            if (head_end < 12 || memcmp(buf, "HTTP/", 5) != 0) {
                snprintf(why, whyn, "not an HTTP/1.x response");
                goto bad;
            }
            p = buf + 5;
            if (*p >= '0' && *p <= '9') {
                p++;
                if (*p == '.' && p[1] >= '0' && p[1] <= '9')
                    p += 2;
            }
            if (*p != ' ' || !(p - buf >= 8)) {
                snprintf(why, whyn, "malformed status line");
                goto bad;
            }
            sp = p + 1;
            if (!(sp[0] >= '0' && sp[0] <= '9' && sp[1] >= '0' && sp[1] <= '9' && sp[2] >= '0' && sp[2] <= '9')) {
                snprintf(why, whyn, "malformed status code");
                goto bad;
            }
            h->status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
            if (h->status >= 100 && h->status < 200 && h->status != 101) {
                if (++skips > 5) {
                    snprintf(why, whyn, "too many interim responses");
                    goto bad;
                }
                memmove(buf, buf + head_end, n - head_end);
                n -= head_end;
                scanned = 0;
                memset(h, 0, sizeof *h);
                h->clen = -1;
                continue;
            }
            {
                const char* rs = sp + 3;
                size_t rn = 0;
                if (rs < buf + l1 && *rs == ' ')
                    rs++;
                rn = (size_t)(buf + l1 - rs);
                while (rn && (rs[rn - 1] == '\r'))
                    rn--;
                if (rn >= sizeof h->status_text)
                    rn = sizeof h->status_text - 1;
                memcpy(h->status_text, rs, rn);
                h->status_text[rn] = 0;
            }
            {
                char* q = eol + 1;
                while (q < buf + head_end) {
                    char* ne = (char*)memchr(q, '\n',
                        (size_t)(buf + head_end - q));
                    size_t ll = ne ? (size_t)(ne - q)
                                   : (size_t)(buf + head_end - q);
                    while (ll && (q[ll - 1] == '\r' || q[ll - 1] == '\n'))
                        ll--;
                    q[ll] = '\0';
                    if (ll)
                        fe_head_line(h, q, ll);
                    if (!ne)
                        break;
                    q = ne + 1;
                }
            }
            if (n > head_end) {
                *pextra = (uint8_t*)malloc(n - head_end);
                if (!*pextra) {
                    snprintf(why, whyn, "out of memory");
                    goto bad;
                }
                memcpy(*pextra, buf + head_end, n - head_end);
                *pextra_len = n - head_end;
            }
            free(buf);
            return 0;
        }
    }
bad:
    free(buf);
    return -1;
}

typedef struct {
    fe_conn_t conn;
    int chunked;
    uint64_t clen;
    uint64_t served;
    uint64_t max_body;
    int eof;
    int failed;
    uint64_t chunk_left;
    int need_size;
    uint8_t* rbuf;
    size_t rn, rcap, rpos;
    char* err;
} fe_stream_t;

static JSClassID dyn_fs_class_id;

static void fs_dispose(void* native)
{
    fe_stream_t* s = (fe_stream_t*)native;
    if (!s)
        return;
    fe_conn_close(&s->conn);
    free(s->rbuf);
    free(s->err);
    free(s);
}

static const JSClassDef dyn_fs_class = {
    "FetcherStream",
    .finalizer = dyn_res_finalizer,
};

static uint8_t* fe_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
{
    size_t n = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &n, v);
    *plen = 0;
    if (p) {
        *plen = n;
        return p;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    {
        size_t off, len, bpe, ab_size;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
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
        if (!base)
            return NULL;
        if (off > ab_size || len > ab_size - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return NULL;
        }
        *plen = len;
        return base + off;
    }
}

static JSValue fe_promise_resolved(JSContext* ctx, JSValue val)
{
    JSValue funcs[2], promise, r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, val);
        return promise;
    }
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue fe_promise_rejected(JSContext* ctx, JSValue exc)
{
    JSValue funcs[2], promise, r;
    if (JS_IsException(exc))
        exc = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, exc);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static void fs_fail(fe_stream_t* s, const char* msg)
{
    if (!s->failed) {
        s->failed = 1;
        free(s->err);
        s->err = strdup(msg);
    }
}

static int fs_fill(fe_stream_t* s, char* why, size_t whyn)
{
    ssize_t r;
    if (s->rn == s->rcap) {
        size_t nc = s->rcap ? s->rcap * 2 : 8192;
        uint8_t* nb = (uint8_t*)realloc(s->rbuf, nc);
        if (!nb) {
            snprintf(why, whyn, "out of memory");
            return -1;
        }
        s->rbuf = nb;
        s->rcap = nc;
    }
    r = fe_conn_recv(&s->conn, s->rbuf + s->rn, s->rcap - s->rn);
    if (r < 0) {
        snprintf(why, whyn, "receiving body failed");
        return -1;
    }
    if (r == 0) {
        s->eof = 1;
        return 0;
    }
    s->rn += (size_t)r;
    return 0;
}

static int fs_read_line(fe_stream_t* s, char* line, size_t cap,
    char* why, size_t whyn)
{
    size_t o = 0;
    for (;;) {
        if (s->rpos >= s->rn) {
            s->rpos = s->rn = 0;
            if (fs_fill(s, why, whyn) < 0)
                return -1;
            if (s->eof) {
                snprintf(why, whyn, "body ended mid-line");
                return -1;
            }
            continue;
        }
        {
            uint8_t c = s->rbuf[s->rpos++];
            if (c == '\n') {
                if (o && line[o - 1] == '\r')
                    o--;
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

static size_t fs_take(fe_stream_t* s, uint8_t* dst, size_t want)
{
    size_t have = s->rn - s->rpos;
    if (have > want)
        have = want;
    memcpy(dst, s->rbuf + s->rpos, have);
    s->rpos += have;
    if (s->rpos == s->rn)
        s->rpos = s->rn = 0;
    return have;
}

static long long fs_read_body(fe_stream_t* s, uint8_t* dst, size_t want,
    char* why, size_t whyn)
{
    size_t got = 0;
    if (s->eof)
        return 0;
    while (got < want) {
        if (s->chunked) {
            if (s->need_size) {
                char line[FE_STREAM_CHUNK_HDR];
                if (fs_read_line(s, line, sizeof line, why, whyn) < 0)
                    return -1;
                {
                    char* endp;
                    long long sz = strtoll(line, &endp, 16);
                    if (endp == line || (*endp && *endp != ';' && *endp != ' ')) {
                        snprintf(why, whyn, "malformed chunk size \"%.16s\"", line);
                        return -1;
                    }
                    if (sz == 0) {
                        uint64_t budget = s->max_body > s->served
                            ? s->max_body - s->served
                            : 0;
                        uint64_t trailer = 0;
                        for (;;) {
                            if (fs_read_line(s, line, sizeof line, why, whyn) < 0)
                                return -1;
                            if (!line[0])
                                break;
                            trailer += (uint64_t)strlen(line) + 2;
                            if (trailer > budget) {
                                snprintf(why, whyn, "body exceeds maxBodyBytes");
                                return -1;
                            }
                        }
                        s->eof = 1;
                        return (long long)got;
                    }
                    s->chunk_left = (uint64_t)sz;
                    s->need_size = 0;
                }
            }
            if (s->chunk_left == 0) {
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
                        ? s->chunk_left
                        : (uint64_t)(want - got));
                size_t moved = fs_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                s->chunk_left -= moved;
                if (moved < room) {
                    if (fs_fill(s, why, whyn) < 0)
                        return -1;
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
            if (left == 0) {
                s->eof = 1;
                return (long long)got;
            }
            {
                size_t room = (size_t)((uint64_t)(want - got) < left
                        ? (uint64_t)(want - got)
                        : left);
                size_t moved = fs_take(s, dst + got, room);
                got += moved;
                s->served += moved;
                if (moved < room) {
                    if (fs_fill(s, why, whyn) < 0)
                        return -1;
                    if (s->eof) {
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

static JSValue dyn_fs_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    size_t len = 0;
    uint8_t* base;
    fe_stream_t* s;
    char why[160];

    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "FetcherStream.read: buf must be a byte-wide view (Uint8Array)");
    base = fe_view_bytes(ctx, argv[0], &len);
    if (!base)
        return JS_EXCEPTION;
    s = (fe_stream_t*)dyn_res_native(ctx, this_val, dyn_fs_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (len == 0)
        return fe_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    if (s->failed)
        return fe_promise_rejected(ctx,
            JS_ThrowRangeError(ctx, "FetcherStream: %s",
                s->err ? s->err : "body read failed"));
    if (s->eof)
        return fe_promise_resolved(ctx, JS_NewInt32(ctx, 0));
    {
        long long n = fs_read_body(s, base, len, why, sizeof why);
        if (n < 0) {
            JSValue exc;
            fs_fail(s, why);
            fe_conn_close(&s->conn);
            exc = JS_ThrowRangeError(ctx, "FetcherStream: %s", why);
            return fe_promise_rejected(ctx, exc);
        }
        if (n == 0)
            s->eof = 1;
        return fe_promise_resolved(ctx, JS_NewInt64(ctx, (int64_t)n));
    }
}

static const JSCFunctionListEntry dyn_fs_proto[] = {
    JS_CFUNC_DEF("read", 1, dyn_fs_read),
};

static JSValue fs_wrap(JSContext* ctx, fe_stream_t* s, const fe_head_t* h,
    const char* url, int skipped_robots)
{
    JSValue obj = dyn_res_wrap(ctx, JS_UNDEFINED, dyn_fs_class_id, s,
        fs_dispose);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueStr(ctx, obj, "status",
        JS_NewInt32(ctx, skipped_robots ? 0 : h->status),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "statusText",
        JS_NewString(ctx, skipped_robots ? "" : h->status_text),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "ok",
        JS_NewBool(ctx, !skipped_robots && h->status >= 200 && h->status < 300),
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
        JS_NewString(ctx, skipped_robots ? "" : (h->content_type ? h->content_type : "")),
        JS_PROP_C_W_E);
    if (skipped_robots)
        JS_DefinePropertyValueStr(ctx, obj, "skippedByRobots", JS_TRUE,
            JS_PROP_C_W_E);
    return obj;
}

static JSValue dyn_fe_get_stream(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    fe_t* f;
    const char* url0 = NULL;
    char cur[1024], host[300];
    const char* path;
    int https, hop;
    JSValue ret = JS_EXCEPTION, uh;
    fe_conn_t conn = { -1,
#ifdef CONFIG_TLS
        NULL
#endif
    };

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getStream(url)");
    url0 = JS_ToCString(ctx, argv[0]);
    if (!url0)
        return JS_EXCEPTION;
    uh = JS_GetPropertyStr(ctx, this_val, "_headers");
    if (JS_IsException(uh)) {
        JS_FreeCString(ctx, url0);
        return JS_EXCEPTION;
    }
    f = (fe_t*)dyn_res_native(ctx, this_val, dyn_fe_class_id);
    if (!f) {
        JS_FreeValue(ctx, uh);
        JS_FreeCString(ctx, url0);
        return JS_EXCEPTION;
    }
    if (strlen(url0) + 1 > sizeof cur) {
        JS_FreeCString(ctx, url0);
        return JS_ThrowRangeError(ctx, "Fetcher: url is too long");
    }
    snprintf(cur, sizeof cur, "%s", url0);
    JS_FreeCString(ctx, url0);
    {
        char* frag = strchr(cur, '#');
        if (frag)
            *frag = 0;
    }
    {
        uint8_t* extra = NULL;
        size_t extra_len = 0;
        for (hop = 0;; hop++) {
            fe_host_t* h;
            double wait, floor_ms;
            int attempt, st = 0;
            fe_head_t head;
            char why[160] = "";

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
            if (!h) {
                JS_ThrowOutOfMemory(ctx);
                goto out;
            }

            if (f->robots_on) {
                double now_ms = (double)dyn_timer_now_ms();
                if (!h->robots_tried || now_ms >= h->robots_next_ms) {
                    JSValue client = JS_GetPropertyStr(ctx, this_val, "_client");
                    int rrc = fe_load_robots(ctx, f, this_val, client, uh, h,
                        https);
                    JS_FreeValue(ctx, client);
                    if (rrc < 0)
                        goto out;
                    f = fe_live(ctx, this_val);
                    if (!f) {
                        JS_ThrowTypeError(ctx,
                            "Fetcher: closed while getStream() was in flight");
                        goto out;
                    }
                    h = fe_host(f, host);
                    if (!h) {
                        JS_ThrowOutOfMemory(ctx);
                        goto out;
                    }
                }
                if (h->robots_unreachable && h->robots_ok)
                    h->robots_unreachable = 0;
                if (h->robots_unreachable || (h->robots && !rb_allows_path(h->robots, path, strlen(path)))) {
                    fe_stream_t* s = (fe_stream_t*)calloc(1, sizeof *s);
                    fe_head_t none;
                    if (!s) {
                        JS_ThrowOutOfMemory(ctx);
                        goto out;
                    }
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

            floor_ms = f->min_delay_ms;
            if (h->robots && h->robots->delay > 0) {
                double cd = h->robots->delay * 1000.0;
                if (cd > floor_ms)
                    floor_ms = cd;
            }
            wait = h->next_ok_ms - (double)dyn_timer_now_ms();
            if (wait > 0) {
                f->throttled_ms += wait;
                fe_sleep_ms(wait);
            }

            for (attempt = 0;; attempt++) {
                sc_sb_t req;
                double back;
                char portless[300];
                uint16_t port;

                if (attempt > 0 || hop > 0)
                    fe_conn_close(&conn);
                fe_head_free(&head);
                free(extra);
                extra = NULL;
                extra_len = 0;
                memset(&head, 0, sizeof head);
                head.clen = -1;

                {
                    const char* cp = strchr(host, ']');
                    const char* colon = cp ? strchr(cp, ':') : strchr(host, ':');
                    if (colon) {
                        size_t nl = (size_t)(colon - host);
                        long p;
                        if (nl + 1 > sizeof portless)
                            nl = sizeof portless - 1;
                        memcpy(portless, host, nl);
                        portless[nl] = 0;
                        p = strtol(colon + 1, NULL, 10);
                        port = (uint16_t)(p > 0 && p < 65536 ? p : (https ? 443 : 80));
                    } else {
                        snprintf(portless, sizeof portless, "%s", host);
                        port = (uint16_t)(https ? 443 : 80);
                    }
                    if (portless[0] == '[') {
                        char* close_b = strchr(portless, ']');
                        if (close_b) {
                            memmove(portless, portless + 1,
                                (size_t)(close_b - portless) - 1);
                            portless[(size_t)(close_b - portless) - 1] = 0;
                        }
                    }
                }
                conn.fd = fe_tcp_connect(portless, port, f->allow_private_hosts,
                    why, sizeof why);
#ifdef CONFIG_TLS
                if (conn.fd >= 0 && https && fe_tls_connect(f, &conn, portless, why, sizeof why) < 0)
                    conn.fd = -1;
#endif
                if (conn.fd < 0) {
                    st = 0;
                } else {
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
                            JSPropertyEnum* tab = NULL;
                            uint32_t tn = 0, i;
                            if (JS_GetOwnPropertyNames(ctx, &tab, &tn, uh,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                                == 0) {
                                for (i = 0; i < tn && !bad; i++) {
                                    JSValue v = JS_GetProperty(ctx, uh, tab[i].atom);
                                    const char *ks = NULL, *vs = NULL;
                                    size_t b;
                                    if (!JS_IsException(v))
                                        vs = JS_ToCString(ctx, v);
                                    if (vs)
                                        ks = JS_AtomToCString(ctx, tab[i].atom);
                                    if (!ks || !vs) {
                                        bad = 1;
                                    } else {
                                        for (b = 0; vs[b]; b++)
                                            if (vs[b] == '\r' || vs[b] == '\n' || vs[b] == '\0')
                                                break;
                                        if (vs[b] || strpbrk(ks, "\r\n:")) {
                                            JS_ThrowTypeError(ctx,
                                                "Fetcher: header \"%s\" value or "
                                                "name carries CR, LF, NUL or a "
                                                "colon -- a value rides to the "
                                                "wire byte-for-byte and would "
                                                "split the request",
                                                ks);
                                            bad = 1;
                                        } else {
                                            sc_sb_puts(&req, ks);
                                            sc_sb_puts(&req, ": ");
                                            sc_sb_puts(&req, vs);
                                            sc_sb_puts(&req, "\r\n");
                                        }
                                    }
                                    if (ks)
                                        JS_FreeCString(ctx, ks);
                                    if (vs)
                                        JS_FreeCString(ctx, vs);
                                    if (!JS_IsException(v))
                                        JS_FreeValue(ctx, v);
                                }
                                for (i = 0; i < tn; i++)
                                    JS_FreeAtom(ctx, tab[i].atom);
                                js_free(ctx, tab);
                            } else {
                                bad = 1;
                            }
                        }
                        if (!bad)
                            sc_sb_puts(&req, "Connection: close\r\n\r\n");
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
                    } else if (fe_read_head(&conn, &head, why, sizeof why,
                                   &extra, &extra_len)
                        < 0) {
                        st = 0;
                    } else {
                        st = head.status;
                    }
                    sc_sb_free(&req);
                }
                h->next_ok_ms = (double)dyn_timer_now_ms() + floor_ms;

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
                    if (secs > 0)
                        back = secs * 1000.0;
                }
                if (back > 60000.0)
                    break;
                fe_conn_close(&conn);
                f->retried++;
                fe_sleep_ms(back);
            }

            if (st >= 300 && st < 400) {
                if (head.location && hop < f->max_redirects) {
                    char nxt[sizeof cur];
                    int prev_https = fe_scheme_is_https(cur);
                    if (fe_resolve(cur, head.location, nxt, sizeof nxt) > 0 && strcmp(nxt, cur)) {
                        char lh[300];
                        const char* lp;
                        if (prev_https && !fe_scheme_is_https(nxt) && !f->allow_insecure_downgrade) {
                            fe_conn_close(&conn);
                            fe_head_free(&head);
                            JS_FreeValue(ctx, uh);
                            JS_ThrowRangeError(ctx,
                                "Fetcher: redirect downgrades https to http "
                                "(%.80s); pass allowInsecureDowngrade: true to "
                                "follow it",
                                nxt);
                            goto out;
                        }
                        if (fe_split(nxt, lh, sizeof lh, &lp) >= 0 && !f->allow_private_hosts && fe_host_is_private(lh)) {
                            fe_conn_close(&conn);
                            fe_head_free(&head);
                            JS_FreeValue(ctx, uh);
                            JS_ThrowTypeError(ctx,
                                "Fetcher: redirect target %.60s is a private/"
                                "loopback/link-local host; pass "
                                "allowPrivateHosts: true to fetch it",
                                lh);
                            goto out;
                        }
                        snprintf(cur, sizeof cur, "%s", nxt);
                        fe_conn_close(&conn);
                        fe_head_free(&head);
                        continue;
                    }
                } else if (hop >= f->max_redirects) {
                    fe_conn_close(&conn);
                    fe_head_free(&head);
                    JS_FreeValue(ctx, uh);
                    JS_ThrowRangeError(ctx,
                        "Fetcher: more than %d redirects", f->max_redirects);
                    goto out;
                }
            }

            {
                if (!head.chunked && head.clen >= 0 && (uint64_t)head.clen > (uint64_t)f->max_body) {
                    fe_conn_close(&conn);
                    fe_head_free(&head);
                    JS_FreeValue(ctx, uh);
                    JS_ThrowRangeError(ctx,
                        "Fetcher: declared Content-Length %lld exceeds "
                        "maxBodyBytes",
                        head.clen);
                    goto out;
                }
                {
                    fe_stream_t* s = (fe_stream_t*)calloc(1, sizeof *s);
                    if (!s) {
                        fe_conn_close(&conn);
                        fe_head_free(&head);
                        JS_FreeValue(ctx, uh);
                        JS_ThrowOutOfMemory(ctx);
                        goto out;
                    }
                    s->conn = conn;
                    conn.fd = -1;
#ifdef CONFIG_TLS
                    conn.tls = NULL;
#endif
                    if (extra_len) {
                        s->rbuf = extra;
                        s->rcap = extra_len;
                        s->rn = extra_len;
                        extra = NULL;
                        extra_len = 0;
                    }
                    s->chunked = head.chunked;
                    s->clen = (head.chunked || head.clen < 0)
                        ? UINT64_MAX
                        : (uint64_t)head.clen;
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
        free(extra);
        JS_FreeValue(ctx, uh);
        fe_conn_close(&conn);
        return ret;
    }
}

static JSValue dyn_scrape_noop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static void dyn_scrape_promise_swallow(JSContext* ctx, JSValue v)
{
    JSAtom catom = JS_NewAtom(ctx, "catch");
    JSValue cf = JS_GetProperty(ctx, v, catom);
    JS_FreeAtom(ctx, catom);
    if (JS_IsFunction(ctx, cf)) {
        JSValue noop = JS_NewCFunction(ctx, dyn_scrape_noop, "", 0);
        JSValue caught = JS_Call(ctx, cf, v, 1, (JSValueConst*)&noop);
        JS_FreeValue(ctx, caught);
        JS_FreeValue(ctx, noop);
    }
    JS_FreeValue(ctx, cf);
    JS_FreeValue(ctx, v);
}

static JSValue fe_auto_client(JSContext* ctx, double max_body)
{
    char src[256];
    JSValue r, v = JS_UNDEFINED, g, hobj, client;
    JSPromiseStateEnum st;
    const char* holder = "__dyna_scrape_client";

    snprintf(src, sizeof src,
        "import { HTTPClient } from \"dyna:net\";\n"
        "try { globalThis.%s = new HTTPClient(%d); }\n"
        "catch (e) { globalThis.%s = undefined; }\n",
        holder, max_body >= (double)(1 << 30) ? (1 << 30) : (int)max_body,
        holder);
    r = JS_Eval(ctx, src, strlen(src), "<dyna:scrape fetcher client>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
        return JS_ThrowTypeError(ctx,
            "Fetcher: no client was passed and the built-in one could not "
            "be constructed (is dyna:net available in this build?) -- pass "
            "`client` explicitly to inject your own");
    }
    v = JS_EvalFunction(ctx, r);
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
        dyn_scrape_promise_swallow(ctx, v);
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

static JSValue dyn_fe_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    fe_t* f;
    JSValue av, cv, hv;
    const char* agent = NULL;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Fetcher({ agent, client? }): `agent` is required; omit "
            "`client` to use the built-in HTTPClient, or pass one to inject");
    if (dyn_opts_strict(ctx, argv[0], sc_fetcher_keys, 15))
        return JS_EXCEPTION;
    av = JS_GetPropertyStr(ctx, argv[0], "agent");
    if (JS_IsException(av))
        return JS_EXCEPTION;
    if (JS_IsString(av))
        agent = JS_ToCString(ctx, av);
    JS_FreeValue(ctx, av);
    if (!agent || !*agent) {
        if (agent)
            JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `agent` is required and must be a non-empty string, e.g. "
            "\"mybot/1.0 (+https://example.test/bot)\". There is no default: a "
            "shared one is indistinguishable from anonymous and tells an "
            "operator nothing about who to contact.");
    }
    cv = JS_GetPropertyStr(ctx, argv[0], "client");
    if (JS_IsException(cv)) {
        JS_FreeCString(ctx, agent);
        return JS_EXCEPTION;
    }
    hv = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (JS_IsException(hv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeCString(ctx, agent);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(cv) && !JS_IsNull(cv) && !JS_IsObject(cv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `client` must be an object (a dyna:net HTTPClient, or "
            "a mock with request(method, url, body, headers)) -- omit it or "
            "pass null to use the built-in HTTPClient");
    }
    if (!JS_IsUndefined(hv) && !JS_IsNull(hv) && !JS_IsObject(hv)) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowTypeError(ctx,
            "Fetcher: `headers` must be an object of extra request headers");
    }
    {
        char cred[64];
        if (fe_credential_header(ctx, hv, cred, sizeof cred)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            JS_FreeCString(ctx, agent);
            return JS_ThrowTypeError(ctx,
                "Fetcher: `headers` may not carry credentials (got \"%s\") -- "
                "extra headers are sent to every host, including redirect "
                "targets and robots.txt pre-fetches; fetch authenticated "
                "endpoints through dyna:http instead",
                cred);
        }
    }
    f = (fe_t*)calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        JS_FreeCString(ctx, agent);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->agent = strdup(agent);
    JS_FreeCString(ctx, agent);
    if (!f->agent) {
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, hv);
        free(f);
        return JS_ThrowOutOfMemory(ctx);
    }

    {
        int v_robots, v_delay, v_retries, v_maxred, v_maxbody;
        int v_priv, v_reval, v_ttl, v_insec, v_pool;
        if (fe_num_prop(ctx, argv[0], "robots", 1, &v_robots) || fe_num_prop(ctx, argv[0], "minDelayMs", 1000, &v_delay) || fe_num_prop(ctx, argv[0], "retries", 3, &v_retries) || fe_num_prop(ctx, argv[0], "maxRedirects", 5, &v_maxred) || fe_num_prop(ctx, argv[0], "maxBodyBytes", 8 << 20, &v_maxbody) || fe_num_prop(ctx, argv[0], "allowPrivateHosts", 0, &v_priv) || fe_num_prop(ctx, argv[0], "revalidate", 1, &v_reval) || fe_num_prop(ctx, argv[0], "robotsTtlMs", 86400000, &v_ttl) || fe_strict_int_prop(ctx, argv[0], "poolSize", 4, &v_pool) || fe_num_prop(ctx, argv[0], "allowInsecureDowngrade", 0, &v_insec)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_EXCEPTION;
        }
        f->robots_on = v_robots ? 1 : 0;
        f->min_delay_ms = v_delay;
        f->retries = v_retries;
        f->max_redirects = v_maxred;
        f->max_body = (double)v_maxbody;
        if (v_maxbody < 0) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_ThrowTypeError(ctx,
                "Fetcher: maxBodyBytes must be a non-negative number of "
                "bytes");
        }
        f->allow_private_hosts = v_priv ? 1 : 0;
        f->revalidate = v_reval ? 1 : 0;
        f->robots_ttl_ms = (double)v_ttl;
        if (f->robots_ttl_ms < 0)
            f->robots_ttl_ms = 0;
        f->allow_insecure_downgrade = v_insec ? 1 : 0;
        f->pool_size = v_pool;
        if (f->pool_size < 1 || f->pool_size > 64) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_ThrowRangeError(ctx,
                "Fetcher: poolSize must be an integer from 1 to 64");
        }
    }
    {
        JSValue pv = JS_GetPropertyStr(ctx, argv[0], "proxy");
        JSValue qv = JS_GetPropertyStr(ctx, argv[0], "ca");
        if (JS_IsException(pv) || JS_IsException(qv)) {
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, qv);
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            free(f->agent);
            free(f);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(pv) && !JS_IsNull(pv)) {
            const char* s;
            if (!JS_IsString(pv)) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a string URL");
            }
            s = JS_ToCString(ctx, pv);
            if (!s) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: proxy must be a non-empty string");
            }
            f->proxy = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->proxy) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        if (!JS_IsUndefined(qv) && !JS_IsNull(qv)) {
            const char* s;
            if (!JS_IsString(qv)) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a string path");
            }
            s = JS_ToCString(ctx, qv);
            if (!s) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_EXCEPTION;
            }
            if (!*s) {
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowTypeError(ctx, "Fetcher: ca must be a non-empty string");
            }
            f->ca = strdup(s);
            JS_FreeCString(ctx, s);
            if (!f->ca) {
                JS_FreeValue(ctx, pv);
                JS_FreeValue(ctx, qv);
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, hv);
                free(f->agent);
                free(f->proxy);
                free(f);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, qv);
    }
    if (f->retries < 0)
        f->retries = 0;
    if (f->max_redirects < 0)
        f->max_redirects = 0;
    if (f->min_delay_ms < 0)
        f->min_delay_ms = 0;
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
    if (dyn_os_entropy(&f->rng, sizeof f->rng) != 0)
        f->rng = ((uint64_t)dyn_timer_now_ms() << 17) ^ (uintptr_t)f ^ 0x9E3779B97F4A7C15ULL;
    {
        JSValue obj = dyn_res_wrap(ctx, nt, dyn_fe_class_id, f, fe_dispose);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, hv);
            return obj;
        }
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
    JS_CFUNC_DEF("getStream", 1, dyn_fe_get_stream),
};

#define CR_STATE_VERSION 1

typedef struct {
    char* url;
    int depth;
} cr_item_t;

#define CR_MAX_PENDING 10000

#define CR_MAX_CONCURRENCY 16

typedef struct cr_flight cr_flight_t;
typedef struct cr_next_wait cr_next_wait_t;

struct cr_next_wait {
    JSValue resolve;
    JSValue reject;
    cr_next_wait_t* next;
};

typedef struct {
    int refs;
    int crawl_alive;
} cr_owner_t;

#define CR_FS_HOP 0
#define CR_FS_EXCHANGE 1
#define CR_FS_DECIDE 3
#define CR_FS_BACKOFF 2

static void cr_flight_free(JSContext* ctx, cr_flight_t* fl);
static void cr_flight_free_ex(cr_flight_t* fl, JSContext* ctx,
    JSRuntime* rt);

typedef struct {
    cr_item_t* q;
    size_t qn, qcap, qhead;
    char** seen;
    size_t sn, scap;
    uint32_t* sidx;
    size_t sidx_mask;
    int max_pages, max_depth, same_host;
    size_t dropped;
    char* link_field;
    char* base_field;
    char* canonical_field;
    char* rel_field;
    char* robots_field;
    char seed_host[300];
    int emitted;
    int started;
    int concurrency;
    JSContext* ctx;
    JSRuntime* rt;
    JSValue self_pending;
    void* owner;
    cr_flight_t* flights;
    size_t n_flights;
    cr_flight_t *done_head, *done_tail;
    size_t n_done;
    cr_next_wait_t *wait_head, *wait_tail;
    JSValue pending_err;
    unsigned flight_seq;
} cr_t;

struct cr_flight {
    int state;
    char cur[1024], host[300];
    const char* path;
    int https, hop, attempt, st, depth;
    double floor_ms;
    JSContext* ctx;
    cr_t* c;
    JSValue crawl;
    JSValue fetcher;
    JSValue client;
    JSValue uh;
    JSValue res;
    JSValue pending;
    JSValue err;
    int finished;
    int floor_claimed;
    int park_claims;
    int graveyard;
    int pins_released;
    int claim_released;
    unsigned magic;
    unsigned seq;
    cr_flight_t* next;
};

_Static_assert(offsetof(cr_flight_t, err) == offsetof(cr_flight_t, crawl) + 6 * sizeof(JSValue),
    "cr_flight_t pins must stay contiguous");

static JSClassID dyn_cr_class_id;

static void cr_release(cr_t* c)
{
    cr_owner_t* own = (cr_owner_t*)c->owner;
    if (--own->refs == 0) {
        free(own);
        free(c);
    }
}

static void cr_park_fail(JSContext* ctx, JSRuntime* rt,
    JSValue* resolve, JSValue* reject,
    const char* what)
{
    if (JS_IsUndefined(*resolve) && JS_IsUndefined(*reject))
        return;
    if (ctx) {
        JSValue exc = JS_NewError(ctx);
        JSValue r;
        if (!JS_IsException(exc)) {
            JS_DefinePropertyValueStr(ctx, exc, "message",
                JS_NewString(ctx, what), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        } else {
            exc = JS_GetException(ctx);
        }
        r = JS_Call(ctx, *reject, JS_UNDEFINED, 1, (JSValueConst*)&exc);
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

static void cr_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    cr_t* c = (cr_t*)opaque;
    cr_owner_t* own = c ? (cr_owner_t*)c->owner : NULL;
    cr_next_wait_t* waiters;
    cr_flight_t *flights, *done;
    JSValue sp, pe;

    if (!c || !own)
        return;
    own->crawl_alive = 0;
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
        cr_next_wait_t* w = waiters;
        waiters = w->next;
        cr_park_fail(ctx, rt, &w->resolve, &w->reject,
            "Crawl: next() aborted at engine shutdown");
        free(w);
        cr_release(c);
    }
    while (flights) {
        cr_flight_t* fl = flights;
        flights = fl->next;
        cr_flight_free_ex(fl, ctx, rt);
    }
    while (done) {
        cr_flight_t* fl = done;
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

static void cr_dispose(void* native)
{
    cr_t* c = (cr_t*)native;
    cr_owner_t* own;
    size_t i;
    if (!c)
        return;
    JS_RemoveShutdownSweep(c->rt, cr_sweep, c);
    for (i = 0; i < c->qn; i++)
        free(c->q[i].url);
    free(c->q);
    for (i = 0; i < c->sn; i++)
        free(c->seen[i]);
    free(c->seen);
    free(c->sidx);
    free(c->link_field);
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    own = (cr_owner_t*)c->owner;
    if (own) {
        own->crawl_alive = 0;
        while (c->wait_head) {
            cr_next_wait_t* w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            out = JS_NewObject(c->ctx);
            JS_DefinePropertyValueStr(c->ctx, out, "done", JS_TRUE,
                JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(c->ctx, out, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            r = JS_Call(c->ctx, w->resolve, JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(c->ctx, r);
            JS_FreeValue(c->ctx, out);
            JS_FreeValue(c->ctx, w->resolve);
            JS_FreeValue(c->ctx, w->reject);
            free(w);
            cr_release(c);
        }
        c->wait_tail = NULL;
        while (c->done_head) {
            cr_flight_t* fl = c->done_head;
            c->done_head = fl->next;
            cr_flight_free(c->ctx, fl);
        }
        c->done_tail = NULL;
        c->n_done = 0;
        c->flights = NULL;
        c->n_flights = 0;
    }
    JS_FreeValue(c->ctx, c->self_pending);
    JS_FreeValue(c->ctx, c->pending_err);
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    if (own)
        cr_release(c);
    else
        free(c);
}

static int cr_is_nofollow(const char* rel)
{
    size_t n;
    if (!rel)
        return 0;
    n = strlen(rel);
    if (n < 8)
        return 0;
    {
        const char* q = rel;
        while ((size_t)(q - rel) + 8 <= n) {
            if (!strncasecmp(q, "nofollow", 8)) {
                char a = (q > rel && q[-1] != ' ' && q[-1] != '\t' && q[-1] != ',') ? 'x' : ' ';
                char b = (q[8] && q[8] != ' ' && q[8] != '\t' && q[8] != ',') ? 'x' : ' ';
                if (a == ' ' && b == ' ')
                    return 1;
            }
            q++;
        }
    }
    return 0;
}

static void cr_norm_host(const char* host, int https, char* out, size_t cap)
{
    size_t n = strlen(host), keep = n, i;
    if (n && host[0] == '[') {
        const char* cl = strchr(host, ']');
        if (cl)
            keep = (size_t)(cl - host) + 1;
    } else {
        const char* c = strchr(host, ':');
        if (c) {
            const char* pfx = https ? ":443" : ":80";
            keep = (size_t)(c - host);
            if (strcmp(c, pfx))
                keep = n;
        }
    }
    if (keep >= cap)
        keep = cap - 1;
    for (i = 0; i < keep; i++)
        out[i] = (char)((host[i] >= 'A' && host[i] <= 'Z') ? host[i] + 32
                                                           : host[i]);
    out[keep] = 0;
}

static long cr_key(const char* url, char* out, size_t cap)
{
    const char* sp = strstr(url, "://");
    const char* path;
    size_t sl, al, keep, i, tl;
    int https;
    if (!sp)
        return -1;
    https = !strncmp(url, "https:", 6);
    sl = (size_t)(sp - url) + 3;
    path = strchr(sp + 3, '/');
    if (!path)
        path = strchr(sp + 3, '?');
    if (!path)
        path = url + strlen(url);
    al = (size_t)(path - (sp + 3));
    keep = al;
    if (al && (sp + 3)[0] == '[') {
        const char* cl = memchr(sp + 3, ']', al);
        if (cl)
            keep = (size_t)(cl - (sp + 3)) + 1;
    } else {
        const char* c = memchr(sp + 3, ':', al);
        if (c) {
            size_t pl = (size_t)(c - (sp + 3));
            size_t pn = al - pl;
            if ((!https && pn == 3 && !memcmp(c, ":80", 3)) || (https && pn == 4 && !memcmp(c, ":443", 4)))
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

static const JSClassDef dyn_cr_class = {
    "Crawl",
    .finalizer = dyn_res_finalizer,
};

#define CR_SIDX_EMPTY 0xffffffffu

static uint32_t cr_url_hash(const char* u)
{
    uint32_t h = 2166136261u;
    const unsigned char* p = (const unsigned char*)u;
    for (; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static int cr_seen(cr_t* c, const char* u)
{
    size_t i;
    if (c->sidx_mask) {
        uint32_t h = cr_url_hash(u);
        size_t slot = h & c->sidx_mask;
        for (;;) {
            uint32_t idx = c->sidx[slot];
            if (idx == CR_SIDX_EMPTY)
                return 0;
            if (!strcmp(c->seen[idx], u))
                return 1;
            slot = (slot + 1) & c->sidx_mask;
        }
    }
    for (i = 0; i < c->sn; i++)
        if (!strcmp(c->seen[i], u))
            return 1;
    return 0;
}

static uint32_t* cr_sidx_alloc(size_t n)
{
    uint32_t* t = (uint32_t*)malloc(n * sizeof *t);
    if (!t)
        return NULL;
    memset(t, 0xff, n * sizeof *t);
    return t;
}

static int cr_mark_seen(cr_t* c, const char* u)
{
    uint32_t h;
    size_t slot;
    if (c->sn == c->scap) {
        size_t nc = c->scap ? c->scap * 2 : 32;
        char** ns = (char**)realloc(c->seen, nc * sizeof(*ns));
        if (!ns)
            return -1;
        c->seen = ns;
        c->scap = nc;
    }
    c->seen[c->sn] = strdup(u);
    if (!c->seen[c->sn])
        return -1;
    h = cr_url_hash(u);
    if (!c->sidx) {
        c->sidx = cr_sidx_alloc(64);
        if (c->sidx)
            c->sidx_mask = 63;
    }
    if (c->sidx_mask) {
        if ((c->sn + 1) * 4 > (c->sidx_mask + 1) * 3) {
            size_t n2 = (c->sidx_mask + 1) * 2;
            uint32_t* nt = cr_sidx_alloc(n2);
            size_t i, s2;
            if (nt) {
                for (i = 0; i < c->sn; i++) {
                    s2 = cr_url_hash(c->seen[i]) & (n2 - 1);
                    while (nt[s2] != CR_SIDX_EMPTY)
                        s2 = (s2 + 1) & (n2 - 1);
                    nt[s2] = (uint32_t)i;
                }
                free(c->sidx);
                c->sidx = nt;
                c->sidx_mask = n2 - 1;
            }
        }
        slot = h & c->sidx_mask;
        while (c->sidx[slot] != CR_SIDX_EMPTY)
            slot = (slot + 1) & c->sidx_mask;
        c->sidx[slot] = (uint32_t)c->sn;
    }
    c->sn++;
    return 0;
}

static int cr_push(cr_t* c, const char* u, int depth)
{
    if (c->qn - c->qhead >= CR_MAX_PENDING)
        return 1;
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16;
        cr_item_t* nq = (cr_item_t*)realloc(c->q, nc * sizeof(*nq));
        if (!nq)
            return -1;
        c->q = nq;
        c->qcap = nc;
    }
    c->q[c->qn].url = strdup(u);
    if (!c->q[c->qn].url)
        return -1;
    c->q[c->qn].depth = depth;
    c->qn++;
    return 0;
}

static void cr_install_async_iterator(JSContext* ctx, JSValue obj);

static JSValue dyn_cr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    cr_t* c;
    JSValue opt, lf = JS_UNDEFINED, obj;
    const char* s;
    int v_maxpages, v_maxdepth, v_samehost, v_conc;
    cr_owner_t* own;
    (void)nt;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Crawl(fetcher[, { maxPages, maxDepth, sameHost, linkField }])");
    c = (cr_t*)calloc(1, sizeof(*c));
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    own = (cr_owner_t*)calloc(1, sizeof *own);
    if (!own) {
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    own->refs = 1;
    own->crawl_alive = 1;
    c->owner = own;
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->concurrency = 1;
    c->self_pending = JS_UNDEFINED;
    c->pending_err = JS_UNDEFINED;
    opt = (argc > 1 && JS_IsObject(argv[1])) ? JS_DupValue(ctx, argv[1])
                                             : JS_NewObject(ctx);
    if (dyn_opts_strict(ctx, opt, sc_crawl_keys, 9))
        goto fail;
    if (fe_num_prop(ctx, opt, "maxPages", 100, &v_maxpages) || fe_num_prop(ctx, opt, "maxDepth", 2, &v_maxdepth) || fe_num_prop(ctx, opt, "sameHost", 1, &v_samehost) || cr_int_prop(ctx, opt, "concurrency", 1, 1, CR_MAX_CONCURRENCY, &v_conc))
        goto fail;
    c->max_pages = v_maxpages;
    c->max_depth = v_maxdepth;
    c->same_host = v_samehost ? 1 : 0;
    c->concurrency = v_conc;

    lf = JS_GetPropertyStr(ctx, opt, "linkField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    c->link_field = strdup(s ? s : "links");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    if (!c->link_field)
        goto fail;
    lf = JS_GetPropertyStr(ctx, opt, "baseField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->base_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->base_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "canonicalField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->canonical_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->canonical_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "relField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->rel_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->rel_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    lf = JS_UNDEFINED;
    lf = JS_GetPropertyStr(ctx, opt, "robotsField");
    if (JS_IsException(lf))
        goto fail;
    s = JS_IsString(lf) ? JS_ToCString(ctx, lf) : NULL;
    if (s) {
        c->robots_field = strdup(s);
        JS_FreeCString(ctx, s);
        if (!c->robots_field)
            goto fail;
    }
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    if (c->max_pages < 0)
        c->max_pages = 0;
    if (c->max_depth < 0)
        c->max_depth = 0;

    obj = dyn_res_wrap(ctx, nt, dyn_cr_class_id, c, cr_dispose);
    if (JS_IsException(obj))
        return obj;
    JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_sweep, c);
    JS_DefinePropertyValueStr(ctx, obj, "_fetcher", JS_DupValue(ctx, argv[0]), 0);
    cr_install_async_iterator(ctx, obj);
    return obj;

fail:
    JS_FreeValue(ctx, lf);
    JS_FreeValue(ctx, opt);
    free(c->link_field);
    free(c->base_field);
    free(c->canonical_field);
    free(c->rel_field);
    free(c->robots_field);
    free(c->owner);
    free(c);
    return JS_EXCEPTION;
}

static void cr_install_async_iterator(JSContext* ctx, JSValue obj)
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

static JSValue dyn_cr_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    const char* seed;
    const char* path;
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "start(seed[, extractor])");
    seed = JS_ToCString(ctx, argv[0]);
    if (!seed)
        return JS_EXCEPTION;
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
            char* frag = strchr(sbuf, '#');
            if (frag)
                *frag = 0;
        }
        https = fe_split(sbuf, c->seed_host, sizeof c->seed_host, &path);
        if (https < 0)
            return JS_ThrowTypeError(ctx, "Crawl: seed must be an http(s) url");
        {
            char nh[300];
            cr_norm_host(c->seed_host, https, nh, sizeof nh);
            memcpy(c->seed_host, nh, strlen(nh) + 1);
        }
        if (cr_push(c, sbuf, 0) < 0)
            return JS_ThrowOutOfMemory(ctx);
        {
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
    if (argc > 2 && JS_IsFunction(ctx, argv[2]))
        JS_DefinePropertyValueStr(ctx, this_val, "_parse",
            JS_DupValue(ctx, argv[2]), 0);
    return JS_DupValue(ctx, this_val);
}

static JSValue cr_make_page(JSContext* ctx, cr_t* c, JSValueConst this_val,
    const char* url, int depth, JSValue res);
static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c);

static JSValue dyn_cr_next(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    JSValue fetcher, res, page, out;
    const char* url;
    int depth;
    (void)argc;
    (void)argv;

    if (!c)
        return JS_EXCEPTION;
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
        res = JS_Invoke(ctx, fetcher, m, 1, (JSValueConst*)a);
        JS_FreeAtom(ctx, m);
        JS_FreeValue(ctx, a[0]);
    }
    JS_FreeValue(ctx, fetcher);
    if (JS_IsException(res))
        return JS_EXCEPTION;

    page = cr_make_page(ctx, c, this_val, url, depth, res);
    if (JS_IsException(page))
        return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, out, "value", page, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "done", JS_FALSE, JS_PROP_C_W_E);
    return out;
}

static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c);

static JSValue cr_make_page(JSContext* ctx, cr_t* c, JSValueConst this_val,
    const char* url, int depth, JSValue res)
{
    JSValue extractor, page;
    int page_status, page_nofollow = 0;

    page = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, page, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, page, "depth", JS_NewInt32(ctx, depth), JS_PROP_C_W_E);
    page_status = 0;
    {
        JSValue st = JS_GetPropertyStr(ctx, res, "status");
        if (JS_IsException(st) || JS_ToInt32(ctx, &page_status, st)) {
            JS_FreeValue(ctx, st);
            JS_FreeValue(ctx, res);
            JS_FreeValue(ctx, page);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, page, "status", st, JS_PROP_C_W_E);
    }

    {
        JSValue rd = JS_GetPropertyStr(ctx, res, "robotsDirectives");
        if (JS_IsException(rd)) {
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

    extractor = JS_GetPropertyStr(ctx, this_val, "_extractor");
    if (JS_IsObject(extractor)) {
        JSValue raw = JS_GetPropertyStr(ctx, res, "body");
        JSValue parse = JS_GetPropertyStr(ctx, this_val, "_parse");
        JSValue body, a[2], ev;
        JSAtom m;
        if (JS_IsFunction(ctx, parse)) {
            JSValueConst pa[1];
            pa[0] = raw;
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
        ev = JS_Invoke(ctx, extractor, m, 2, (JSValueConst*)a);
        JS_FreeAtom(ctx, m);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, body);
        if (!JS_IsException(ev) && JS_IsObject(ev)) {
            JSValue val = JS_GetPropertyStr(ctx, ev, "value");
            JS_DefinePropertyValueStr(ctx, page, "value", JS_DupValue(ctx, val), JS_PROP_C_W_E);
            if (c->robots_field && JS_IsObject(val)) {
                JSValue rv = JS_GetPropertyStr(ctx, val, c->robots_field);
                if (JS_IsString(rv)) {
                    const char* rs = JS_ToCString(ctx, rv);
                    if (rs) {
                        char* dirs = fe_parse_robots_tag(rs, NULL);
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
                                while (dirs[k] && dirs[k] != '\n')
                                    k++;
                                lv = JS_GetPropertyStr(ctx, rb, "length");
                                JS_ToUint32(ctx, &m2, lv);
                                JS_FreeValue(ctx, lv);
                                for (j = 0; j < m2 && !dup2; j++) {
                                    JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                                    const char* ts = JS_ToCString(ctx, t);
                                    if (ts) {
                                        if (k - b == strlen(ts) && !strncasecmp(ts, dirs + b, k - b))
                                            dup2 = 1;
                                        JS_FreeCString(ctx, ts);
                                    }
                                    JS_FreeValue(ctx, t);
                                }
                                if (!dup2)
                                    JS_SetPropertyUint32(ctx, rb, m2,
                                        JS_NewStringLen(ctx, dirs + b, k - b));
                                if (dirs[k])
                                    k++;
                            }
                            JS_FreeValue(ctx, rb);
                            free(dirs);
                        }
                    }
                }
                JS_FreeValue(ctx, rv);
            }
            {
                JSValue rb = JS_GetPropertyStr(ctx, page, "robots");
                if (JS_IsArray(ctx, rb)) {
                    JSValue lv = JS_GetPropertyStr(ctx, rb, "length");
                    uint32_t m2 = 0, j;
                    JS_ToUint32(ctx, &m2, lv);
                    JS_FreeValue(ctx, lv);
                    for (j = 0; j < m2 && !page_nofollow; j++) {
                        JSValue t = JS_GetPropertyUint32(ctx, rb, j);
                        const char* ts = JS_ToCString(ctx, t);
                        if (ts) {
                            if (!strcasecmp(ts, "nofollow"))
                                page_nofollow = 1;
                            JS_FreeCString(ctx, ts);
                        }
                        JS_FreeValue(ctx, t);
                    }
                }
                JS_FreeValue(ctx, rb);
            }
            if (page_status >= 200 && page_status < 300 && depth < c->max_depth && JS_IsObject(val) && !page_nofollow) {
                const char *ckey = url, *ccs = NULL, *canon_s = NULL;
                JSValue cval = JS_UNDEFINED, cval2 = JS_UNDEFINED;
                int dup = 0;
                if (c->canonical_field) {
                    cval = JS_GetPropertyStr(ctx, val, c->canonical_field);
                    if (JS_IsString(cval)) {
                        ccs = JS_ToCString(ctx, cval);
                        if (ccs && (!strncasecmp(ccs, "http://", 7) || !strncasecmp(ccs, "https://", 8)) && strcmp(ccs, url))
                            ckey = ccs;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                            ? cr_seen(c, nk)
                            : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                if (ckey == url) {
                    cval2 = JS_GetPropertyStr(ctx, res, "canonicalUrl");
                    if (JS_IsString(cval2)) {
                        canon_s = JS_ToCString(ctx, cval2);
                        if (canon_s && strcmp(canon_s, url))
                            ckey = canon_s;
                    }
                    if (ckey != url) {
                        char nk[1024];
                        dup = (cr_key(ckey, nk, sizeof nk) >= 0)
                            ? cr_seen(c, nk)
                            : cr_seen(c, ckey);
                        if (!dup) {
                            if (cr_key(ckey, nk, sizeof nk) >= 0)
                                cr_mark_seen(c, nk);
                            else
                                cr_mark_seen(c, ckey);
                        }
                    }
                }
                if (!dup) {
                    char bbuf[1024];
                    const char* ebase = url;
                    JSValue bval = JS_UNDEFINED;
                    if (c->base_field) {
                        bval = JS_GetPropertyStr(ctx, val, c->base_field);
                        if (JS_IsString(bval)) {
                            const char* bsv = JS_ToCString(ctx, bval);
                            if (bsv) {
                                if (!strncasecmp(bsv, "http://", 7) || !strncasecmp(bsv, "https://", 8))
                                    ebase = bsv;
                                else if (fe_resolve(url, bsv,
                                             bbuf, sizeof bbuf)
                                    > 0)
                                    ebase = bbuf;
                                JS_FreeCString(ctx, bsv);
                            }
                        }
                        JS_FreeValue(ctx, bval);
                    }
                    JSValue links = JS_GetPropertyStr(ctx, val, c->link_field);
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
                            const char* ls = JS_ToCString(ctx, e);
                            if (ls) {
                                char lh[300], resolved[1024];
                                const char* lp;
                                long rl;
                                if (have_rels) {
                                    JSValue rv = JS_GetPropertyUint32(ctx, rels, i);
                                    if (JS_IsString(rv)) {
                                        const char* rs = JS_ToCString(ctx, rv);
                                        if (rs && cr_is_nofollow(rs)) {
                                            JS_FreeCString(ctx, rs);
                                            JS_FreeValue(ctx, rv);
                                            JS_FreeCString(ctx, ls);
                                            JS_FreeValue(ctx, e);
                                            continue;
                                        }
                                        if (rs)
                                            JS_FreeCString(ctx, rs);
                                    }
                                    JS_FreeValue(ctx, rv);
                                }
                                if (!ls[0] || ls[0] == '#' || !strncasecmp(ls, "javascript:", 11) || !strncasecmp(ls, "mailto:", 7) || !strncasecmp(ls, "tel:", 4) || !strncasecmp(ls, "data:", 5)) {
                                    JS_FreeCString(ctx, ls);
                                    JS_FreeValue(ctx, e);
                                    continue;
                                }
                                if (fe_resolve(ebase, ls,
                                        resolved, sizeof resolved)
                                    > 0)
                                    rl = (long)strlen(resolved);
                                else
                                    rl = -1;
                                JS_FreeCString(ctx, ls);
                                if (rl <= 0 || fe_split(resolved, lh, sizeof lh, &lp) < 0 || (strncmp(resolved, "http://", 7) && strncmp(resolved, "https://", 8))) {
                                    JS_FreeValue(ctx, e);
                                    continue;
                                }
                                if (c->same_host) {
                                    char nh[300];
                                    cr_norm_host(lh, resolved[4] == 's',
                                        nh, sizeof nh);
                                    if (strcmp(nh, c->seed_host)) {
                                        JS_FreeValue(ctx, e);
                                        continue;
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
                                            if (rc > 0)
                                                c->dropped++;
                                        }
                                    }
                                }
                            }
                            JS_FreeValue(ctx, e);
                        }
                    }
                    JS_FreeValue(ctx, links);
                    JS_FreeValue(ctx, rels);
                }
                if (ccs)
                    JS_FreeCString(ctx, ccs);
                JS_FreeValue(ctx, cval);
                if (canon_s)
                    JS_FreeCString(ctx, canon_s);
                JS_FreeValue(ctx, cval2);
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
    return page;
}

static void cr_notify(cr_t* c);
static void cr_dispatch(cr_t* c, JSValueConst this_val);
static void cr_flight_run(cr_flight_t* fl);

#define CR_FLIGHT_MAGIC 0xCAFEF11Du

static void cr_flight_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

static void cr_flight_destroy(cr_flight_t* fl, JSRuntime* rt)
{
    cr_t* c = fl->c;
    int claim = !fl->claim_released;
    JS_RemoveShutdownSweep(rt, cr_flight_sweep, fl);
    JS_ShutdownUndeferFree(rt, fl);
    fl->magic = 0;
    free(fl);
    if (claim)
        cr_release(c);
}

static void cr_flight_release_pins(cr_flight_t* fl, JSContext* ctx,
    JSRuntime* rt)
{
    JSValue* pins = &fl->crawl;
    int i;
    for (i = 0; i < 7; i++) {
        if (ctx)
            JS_FreeValue(ctx, pins[i]);
        else
            JS_FreeValueRT(rt, pins[i]);
        pins[i] = JS_UNDEFINED;
    }
}

static void cr_flight_free_ex(cr_flight_t* fl, JSContext* ctx, JSRuntime* rt)
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

static void cr_flight_free(JSContext* ctx, cr_flight_t* fl)
{
    cr_flight_free_ex(fl, ctx, NULL);
}

static void cr_flight_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    cr_flight_t* fl = (cr_flight_t*)opaque;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return;
    cr_flight_free_ex(fl, ctx, rt);
}

static JSValue cr_flight_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data);

static JSValue cr_flight_park(JSContext* ctx, cr_flight_t* fl, JSValue pr)
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
        tr = JS_Call(ctx, upthen, pr, 2, (JSValueConst*)funcs);
        if (JS_IsException(tr)) {
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
    fl->park_claims++;
    return JS_UNDEFINED;
}

static int cr_flight_lift(JSContext* ctx, cr_flight_t* fl, JSValue pr)
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
            fl->res = v;
            return 0;
        }
        JS_FreeValue(ctx, v);
        return 0;
    }
    JS_FreeValue(ctx, pr);
    return -2;
}

static void cr_flight_run(cr_flight_t* fl);

static JSValue cr_flight_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    int64_t p = 0;
    cr_flight_t* fl;
    (void)this_val;
    (void)argc;
    if (JS_ToInt64(ctx, &p, data[0]))
        return JS_EXCEPTION;
    fl = (cr_flight_t*)(intptr_t)p;
    if (!fl || fl->magic != CR_FLIGHT_MAGIC)
        return JS_UNDEFINED;
    if (fl->park_claims > 0)
        fl->park_claims--;
    if (fl->graveyard) {
        if (fl->park_claims == 0)
            cr_flight_destroy(fl, JS_GetRuntime(ctx));
        return JS_UNDEFINED;
    }
    if (fl->finished)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, fl->pending);
    fl->pending = JS_UNDEFINED;
    if (fl->state == CR_FS_EXCHANGE) {
        JS_FreeValue(ctx, fl->res);
        if (magic)
            fl->res = JS_EXCEPTION;
        else
            fl->res = JS_DupValue(ctx, argv[0]);
        fl->state = CR_FS_DECIDE;
    }
    cr_flight_run(fl);
    return JS_UNDEFINED;
}

static int cr_flight_sleep(cr_flight_t* fl, double ms)
{
    (void)fl;
    fe_sleep_ms(ms > 60000 ? 60000 : ms);
    return 0;
}

static void cr_notify(cr_t* c);

static void cr_flight_finish(cr_flight_t* fl, int failed)
{
    JSContext* ctx = fl->ctx;
    cr_t* c = fl->c;
    cr_owner_t* own = (cr_owner_t*)c->owner;
    JSValue page = JS_UNDEFINED;

    if (fl->finished)
        return;
    fl->finished = 1;
    if (failed) {
        if (JS_IsException(fl->res)) {
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, fl->err);
                fl->err = JS_GetException(ctx);
            } else if (JS_IsUndefined(fl->err)) {
                fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
                fl->err = JS_GetException(ctx);
            }
            fl->res = JS_UNDEFINED;
        } else if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);
        } else if (JS_IsUndefined(fl->err)) {
            fl->err = JS_ThrowTypeError(ctx, "Crawl: fetch failed");
            fl->err = JS_GetException(ctx);
        }
    }
    {
        cr_flight_t** pp = &c->flights;
        while (*pp && *pp != fl)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = fl->next;
            c->n_flights--;
        }
    }
    if (!own->crawl_alive) {
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    if (!failed) {
        page = cr_make_page(ctx, c, fl->crawl, fl->cur, fl->depth,
            fl->res);
        fl->res = JS_UNDEFINED;
    }
    if (failed || JS_IsException(page)) {
        if (!failed) {
            JS_FreeValue(ctx, fl->err);
            fl->err = JS_GetException(ctx);
        }
        JS_FreeValue(ctx, page);
    } else {
        fl->res = page;
    }
    if (!own->crawl_alive) {
        JS_FreeValue(ctx, fl->res);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        return;
    }
    fl->next = NULL;
    if (c->done_tail)
        c->done_tail->next = fl;
    else
        c->done_head = fl;
    c->done_tail = fl;
    c->n_done++;
    {
        cr_notify(c);
        cr_dispatch(c, c->self_pending);
        if (c->n_flights == 0 && !JS_IsUndefined(c->self_pending)) {
            JS_FreeValue(ctx, c->self_pending);
            c->self_pending = JS_UNDEFINED;
        }
    }
}

static void cr_flight_fe_gone(cr_flight_t* fl)
{
    JSContext* ctx = fl->ctx;
    JS_FreeValue(ctx, fl->err);
    fl->err = JS_ThrowTypeError(ctx,
        "Crawl: the fetcher was closed while the crawl was in flight");
    fl->err = JS_GetException(ctx);
    cr_flight_finish(fl, 1);
}

static void cr_flight_run(cr_flight_t* fl)
{
    JSContext* ctx = fl->ctx;
    fe_t* f = fe_live(ctx, fl->fetcher);

    if (!f) {
        cr_flight_fe_gone(fl);
        return;
    }

    for (;;) {
        f = fe_live(ctx, fl->fetcher);
        if (!f) {
            cr_flight_fe_gone(fl);
            return;
        }
        switch (fl->state) {
        case CR_FS_HOP: {
            fe_host_t* h;
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
            if (!h) {
                JS_ThrowOutOfMemory(ctx);
                cr_flight_finish(fl, 1);
                return;
            }

            if (f->robots_on) {
                double now_ms = (double)dyn_timer_now_ms();
                if (!h->robots_tried || now_ms >= h->robots_next_ms) {
                    if (fe_load_robots(ctx, f, fl->fetcher, fl->client,
                            fl->uh, h, fl->https)
                        < 0) {
                        cr_flight_finish(fl, 1);
                        return;
                    }
                    f = fe_live(ctx, fl->fetcher);
                    if (!f) {
                        cr_flight_fe_gone(fl);
                        return;
                    }
                }
                if (h->robots_unreachable && h->robots_ok)
                    h->robots_unreachable = 0;
                if (h->robots_unreachable || (h->robots && !rb_allows_path(h->robots, fl->path, strlen(fl->path)))) {
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

            floor_ms = f->min_delay_ms;
            if (h->robots && h->robots->delay > 0) {
                double cd = h->robots->delay * 1000.0;
                if (cd > floor_ms)
                    floor_ms = cd;
            }
            fl->floor_ms = floor_ms;
            if (!fl->floor_claimed) {
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
                if (cr_flight_sleep(fl, wait))
                    return;
            }
            fl->state = CR_FS_EXCHANGE;
            continue;
        }
        case CR_FS_EXCHANGE: {
            JSValue raf;

            if (JS_IsException(fl->res)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, fl->res);
                fl->res = JS_UNDEFINED;
                fl->st = 0;
            } else {
                raf = JS_GetPropertyStr(ctx, fl->client, "requestAsync");
                {
                    int raf_fn = JS_IsFunction(ctx, raf);
                    JS_FreeValue(ctx, raf);
                    raf = JS_UNDEFINED;
                    if (!raf_fn)
                        goto cr_sync_fallback;
                }
                if (1) {
                    JSValue hdrs, a[4];
                    JSAtom m = JS_NewAtom(ctx, "requestAsync");
                    hdrs = JS_NewObject(ctx);
                    if (JS_IsObject(fl->uh)) {
                        JSPropertyEnum* tab = NULL;
                        uint32_t tn = 0, i;
                        if (JS_GetOwnPropertyNames(ctx, &tab, &tn, fl->uh,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                            == 0) {
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
                        (JSValueConst*)a);
                    JS_FreeAtom(ctx, m);
                    JS_FreeValue(ctx, a[0]);
                    JS_FreeValue(ctx, a[1]);
                    JS_FreeValue(ctx, a[3]);
                    if (JS_IsException(fl->res)) {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        fl->res = JS_UNDEFINED;
                        fl->st = 0;
                    } else {
                        JSValue pr = fl->res;
                        fl->res = JS_UNDEFINED;
                        if (JS_PromiseState(ctx, pr) == JS_PROMISE_PENDING) {
                            if (JS_IsException(cr_flight_park(ctx, fl, pr)))
                                cr_flight_finish(fl, 1);
                            return;
                        }
                        if (cr_flight_lift(ctx, fl, pr) < 0) {
                            JS_FreeValue(ctx, fl->res);
                            fl->res = JS_UNDEFINED;
                            fl->st = 0;
                        }
                    }
                } else {
                cr_sync_fallback:
                    fl->res = fe_raw(ctx, f, fl->client, fl->cur, fl->uh,
                        NULL, NULL);
                    if (JS_IsException(fl->res)) {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        fl->res = JS_UNDEFINED;
                        fl->st = 0;
                    }
                }
            }
            fl->state = CR_FS_DECIDE;
            continue;
        }
        case CR_FS_DECIDE: {
            if (!JS_IsUndefined(fl->res)) {
                fl->st = fe_status(ctx, fl->res);
                if (fl->st < 0) {
                    JS_FreeValue(ctx, fl->res);
                    fl->res = JS_UNDEFINED;
                    cr_flight_finish(fl, 1);
                    return;
                }
                f = fe_live(ctx, fl->fetcher);
                if (!f) {
                    cr_flight_fe_gone(fl);
                    return;
                }
            }
            if (fl->st != 0 && !(fl->st == 429 || (fl->st >= 500 && fl->st < 600)))
                break;
            if (fl->attempt >= f->retries)
                break;
            {
                double back = f->min_delay_ms * (double)(1 << (fl->attempt < 10 ? fl->attempt : 10));
                back *= 0.75 + 0.5 * fe_rand01(f);
                if (fl->st != 0 && !JS_IsUndefined(fl->res)) {
                    JSValue hold = JS_UNDEFINED;
                    const char* ra = fe_header(ctx, fl->res, "retry-after",
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
                        if (secs > 0)
                            back = secs * 1000.0;
                        JS_FreeCString(ctx, ra);
                    }
                    JS_FreeValue(ctx, hold);
                    f = fe_live(ctx, fl->fetcher);
                    if (!f) {
                        cr_flight_fe_gone(fl);
                        return;
                    }
                }
                if (back > 60000.0)
                    break;
                JS_FreeValue(ctx, fl->res);
                fl->res = JS_UNDEFINED;
                f->retried++;
                fl->attempt++;
                fl->state = CR_FS_BACKOFF;
                if (cr_flight_sleep(fl, back))
                    return;
                fl->state = CR_FS_EXCHANGE;
                continue;
            }
        }
        case CR_FS_BACKOFF:
            fl->state = CR_FS_EXCHANGE;
            continue;
        default:
            cr_flight_finish(fl, 1);
            return;
        }
        if (fl->st >= 300 && fl->st < 400 && !JS_IsUndefined(fl->res)) {
            JSValue hold = JS_UNDEFINED;
            const char* loc = fe_header(ctx, fl->res, "location", &hold);
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
                if (fe_resolve(fl->cur, loc, nxt, sizeof nxt) > 0 && strcmp(nxt, fl->cur)) {
                    char lh[300];
                    const char* lp;
                    if (prev_https && !fe_scheme_is_https(nxt) && !f->allow_insecure_downgrade) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, fl->res);
                        fl->res = JS_UNDEFINED;
                        JS_ThrowRangeError(ctx,
                            "Fetcher: redirect downgrades https to http "
                            "(%.80s); pass allowInsecureDowngrade: true to "
                            "follow it",
                            nxt);
                        cr_flight_finish(fl, 1);
                        return;
                    }
                    if (fe_split(nxt, lh, sizeof lh, &lp) >= 0 && !f->allow_private_hosts && fe_host_is_private(lh)) {
                        JS_FreeCString(ctx, loc);
                        JS_FreeValue(ctx, hold);
                        JS_FreeValue(ctx, fl->res);
                        fl->res = JS_UNDEFINED;
                        JS_ThrowTypeError(ctx,
                            "Fetcher: redirect target %.60s is a private/"
                            "loopback/link-local host; pass "
                            "allowPrivateHosts: true to fetch it",
                            lh);
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
            if (loc) {
                JS_FreeCString(ctx, loc);
                JS_FreeValue(ctx, hold);
            }
        }
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

static void cr_notify(cr_t* c)
{
    JSContext* ctx = c->ctx;
    while (c->wait_head && c->done_head) {
        cr_next_wait_t* w = c->wait_head;
        cr_flight_t* fl = c->done_head;
        JSValue out, r;
        c->wait_head = w->next;
        if (!c->wait_head)
            c->wait_tail = NULL;
        c->done_head = fl->next;
        if (!c->done_head)
            c->done_tail = NULL;
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
                (JSValueConst*)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, w->resolve);
        JS_FreeValue(ctx, w->reject);
        fl->res = JS_UNDEFINED;
        cr_flight_free(ctx, fl);
        free(w);
        cr_release(c);
    }
    if (!c->done_head && c->n_flights == 0 && (!c->started || c->qhead >= c->qn || c->emitted >= c->max_pages)) {
        while (c->wait_head) {
            cr_next_wait_t* w = c->wait_head;
            JSValue out, r;
            c->wait_head = w->next;
            if (!c->wait_head)
                c->wait_tail = NULL;
            out = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
                JS_PROP_C_W_E);
            r = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1,
                (JSValueConst*)&out);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, w->resolve);
            JS_FreeValue(ctx, w->reject);
            free(w);
            cr_release(c);
        }
        c->wait_tail = NULL;
    }
}

static void cr_dispatch(cr_t* c, JSValueConst this_val)
{
    JSContext* ctx = c->ctx;
    cr_owner_t* own = (cr_owner_t*)c->owner;

    while (own->crawl_alive && c->n_flights < (size_t)c->concurrency && c->qhead < c->qn && (size_t)c->emitted + c->n_flights + c->n_done < (size_t)c->max_pages) {
        cr_flight_t* fl = (cr_flight_t*)calloc(1, sizeof *fl);
        char* u;
        if (!fl)
            return;
        own->refs++;
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
            c->self_pending = JS_DupValue(ctx, this_val);
        fl->state = CR_FS_HOP;
        fl->magic = CR_FLIGHT_MAGIC;
        JS_AddShutdownSweep(JS_GetRuntime(ctx), cr_flight_sweep, fl);
        fl->seq = ++c->flight_seq;
        u = c->q[c->qhead].url;
        fl->depth = c->q[c->qhead].depth;
        c->q[c->qhead].url = NULL;
        c->qhead++;
        snprintf(fl->cur, sizeof fl->cur, "%s", u);
        free(u);
        fl->next = c->flights;
        c->flights = fl;
        c->n_flights++;
        fl->fetcher = JS_GetPropertyStr(ctx, this_val, "_fetcher");
        if (!JS_IsException(fl->fetcher))
            fl->client = JS_GetPropertyStr(ctx, fl->fetcher, "_client");
        if (!JS_IsException(fl->fetcher) && !JS_IsException(fl->client))
            fl->uh = JS_GetPropertyStr(ctx, fl->fetcher, "_headers");
        if (JS_IsException(fl->fetcher) || JS_IsException(fl->client)
            || JS_IsException(fl->uh)) {
            cr_flight_finish(fl, 1);
            if (!own->crawl_alive)
                return;
            continue;
        }
        if (!own->crawl_alive) {
            cr_flight_finish(fl, 1);
            return;
        }
        cr_flight_run(fl);
        if (!own->crawl_alive)
            return;
    }
}

static JSValue dyn_cr_next_async(JSContext* ctx, JSValueConst this_val,
    cr_t* c)
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
    if (c->done_head) {
        cr_flight_t* fl = c->done_head;
        JSValue out, r;
        c->done_head = fl->next;
        if (!c->done_head)
            c->done_tail = NULL;
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
                (JSValueConst*)&out);
            JS_FreeValue(ctx, out);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        cr_flight_free(ctx, fl);
        return promise;
    }
    if (!c->started || (c->qhead >= c->qn && c->n_flights == 0) || c->emitted >= c->max_pages) {
        JSValue out, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        out = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, out, "done", JS_TRUE, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "value", JS_UNDEFINED,
            JS_PROP_C_W_E);
        r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&out);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    if (!((cr_owner_t*)c->owner)->crawl_alive) {
        JSValue exc, r;
        promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(promise))
            return promise;
        exc = JS_ThrowTypeError(ctx, "Crawl: next() on a closed crawl");
        exc = JS_GetException(ctx);
        r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    {
        cr_next_wait_t* w = (cr_next_wait_t*)malloc(sizeof *w);
        cr_owner_t* own = (cr_owner_t*)c->owner;
        if (!w) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, promise);
            return JS_ThrowOutOfMemory(ctx);
        }
        own->refs++;
        w->resolve = funcs[0];
        w->reject = funcs[1];
        w->next = NULL;
        if (c->wait_tail)
            c->wait_tail->next = w;
        else
            c->wait_head = w;
        c->wait_tail = w;
    }
    cr_dispatch(c, this_val);
    return promise;
}

static JSValue dyn_cr_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_cr_serialize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    cr_t* c = (cr_t*)dyn_res_native(ctx, this_val, dyn_cr_class_id);
    sc_sb_t b;
    JSValue out;
    size_t i;
    (void)argc;
    (void)argv;

    if (!c)
        return JS_EXCEPTION;
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
    sc_sb_puts(&b, ",\"q\":[");
    for (i = c->qhead; i < c->qn; i++) {
        char d[16];
        if (i > c->qhead)
            sc_sb_put(&b, ",", 1);
        sc_sb_put(&b, "[", 1);
        sc_sb_json_str(&b, c->q[i].url);
        snprintf(d, sizeof d, ",%d]", c->q[i].depth);
        sc_sb_puts(&b, d);
    }
    sc_sb_puts(&b, "],\"seen\":[");
    for (i = 0; i < c->sn; i++) {
        if (i)
            sc_sb_put(&b, ",", 1);
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

static int cr_state_str(JSContext* ctx, JSValueConst o, const char* key,
    char** out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    const char* s;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return -2;
    }
    s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    JS_FreeValue(ctx, v);
    if (!s) {
        JS_ThrowTypeError(ctx, "Crawl.resume: \"%s\" must be a string", key);
        return -1;
    }
    *out = strdup(s);
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue dyn_cr_resume(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    cr_t* c;
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
        const char* sj = JS_ToCStringLen(ctx, &slen, argv[1]);
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

    c = (cr_t*)calloc(1, sizeof(*c));
    if (!c) {
        JS_FreeValue(ctx, st);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->concurrency = 1;

#define CR_FAIL(msg)                                  \
    do {                                              \
        JS_ThrowTypeError(ctx, "Crawl.resume: " msg); \
        goto fail;                                    \
    } while (0)

    {
        JSValue v = JS_GetPropertyStr(ctx, st, "v");
        int32_t vv = 0;
        int bad = JS_IsException(v) || JS_ToInt32(ctx, &vv, v);
        JS_FreeValue(ctx, v);
        if (bad)
            goto fail_pending;
        if (vv != CR_STATE_VERSION)
            CR_FAIL("state version mismatch (wrong runtime or corrupt file)");
    }
    {
        int v_mp, v_md, v_sh, v_cc;
        if (fe_num_prop(ctx, st, "maxPages", 100, &v_mp) || fe_num_prop(ctx, st, "maxDepth", 2, &v_md) || fe_num_prop(ctx, st, "sameHost", 1, &v_sh) || fe_num_prop(ctx, st, "concurrency", 1, &v_cc))
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
        if (bad)
            goto fail_pending;
        if (v64 < 0 || v64 > (int64_t)c->max_pages)
            CR_FAIL("emitted counter out of range");
        c->emitted = (int)v64;
        v = JS_GetPropertyStr(ctx, st, "dropped");
        bad = JS_IsException(v) || JS_ToInt64(ctx, &v64, v);
        JS_FreeValue(ctx, v);
        if (bad)
            goto fail_pending;
        if (v64 < 0)
            CR_FAIL("dropped counter out of range");
        c->dropped = (size_t)v64;
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, st, "seedHost");
        const char* s;
        if (JS_IsException(v))
            goto fail_pending;
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
    {
        rc = cr_state_str(ctx, st, "linkField", &c->link_field);
        if (rc == -1)
            goto fail_pending;
        if (rc == -2)
            CR_FAIL("state has no linkField");
        if (!c->link_field[0])
            CR_FAIL("linkField is empty");
        rc = cr_state_str(ctx, st, "baseField", &c->base_field);
        if (rc == -1)
            goto fail_pending;
        if (rc == -2 && c->base_field) {
            free(c->base_field);
            c->base_field = NULL;
        }
        rc = cr_state_str(ctx, st, "canonicalField", &c->canonical_field);
        if (rc == -1)
            goto fail_pending;
        rc = cr_state_str(ctx, st, "relField", &c->rel_field);
        if (rc == -1)
            goto fail_pending;
        rc = cr_state_str(ctx, st, "robotsField", &c->robots_field);
        if (rc == -1)
            goto fail_pending;
    }
    {
        JSValue q = JS_GetPropertyStr(ctx, st, "q");
        JSValue qlen;
        if (JS_IsException(q))
            goto fail_pending;
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
            const char* us = NULL;
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
                bad = !us || JS_ToInt32(ctx, &dep, dv) || dep < 0 || strlen(us) >= 1024 || (strncmp(us, "http://", 7) && strncmp(us, "https://", 8));
            }
            JS_FreeValue(ctx, dv);
            JS_FreeValue(ctx, uv);
            JS_FreeValue(ctx, e);
            if (bad) {
                if (us)
                    JS_FreeCString(ctx, us);
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
    {
        JSValue sn = JS_GetPropertyStr(ctx, st, "seen");
        JSValue slen;
        if (JS_IsException(sn))
            goto fail_pending;
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
            const char* es = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
            int bad = !es || strlen(es) >= 1024;
            JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (bad) {
                JS_FreeValue(ctx, sn);
                CR_FAIL("state.seen entry malformed");
            }
        }
        for (i = 0; i < nsn; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sn, i);
            const char* es = JS_ToCString(ctx, e);
            int mrc;
            JS_FreeValue(ctx, e);
            mrc = es ? cr_seen(c, es) ? 0 : cr_mark_seen(c, es) : -1;
            if (es)
                JS_FreeCString(ctx, es);
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
        cr_owner_t* own = (cr_owner_t*)calloc(1, sizeof *own);
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
    if (JS_IsException(obj))
        return obj;
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
fail:
    JS_FreeValue(ctx, st);
    cr_dispose(c);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_cr_proto[] = {
    JS_CFUNC_DEF("start", 1, dyn_cr_start),
    JS_CFUNC_DEF("next", 0, dyn_cr_next),
    JS_CFUNC_DEF("pages", 0, dyn_cr_self),
    JS_CFUNC_DEF("serialize", 0, dyn_cr_serialize),
    JS_ALIAS_DEF("[Symbol.iterator]", "pages"),
};

#define SM_MAX_BYTES (16u << 20)
#define SM_MAX_URLS 50000
#define SM_MAX_LOC_TEXT 2048

typedef struct {
    char** v;
    size_t n, cap;
    int is_index;
} sm_doc_t;

static void sm_doc_free(sm_doc_t* d)
{
    size_t i;
    for (i = 0; i < d->n; i++)
        free(d->v[i]);
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

static int sm_doc_push(sm_doc_t* d, const char* s, size_t n)
{
    char* c;
    if (d->n >= SM_MAX_URLS)
        return -2;
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 16;
        char** nv = (char**)realloc(d->v, nc * sizeof(*nv));
        if (!nv)
            return -1;
        d->v = nv;
        d->cap = nc;
    }
    c = (char*)malloc(n + 1);
    if (!c)
        return -1;
    memcpy(c, s, n);
    c[n] = 0;
    d->v[d->n++] = c;
    return 0;
}

static const char* sm_find(const char* hay, size_t hlen,
    const char* needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

static size_t sm_entity(const char* s, size_t rem, char out[4], int* outlen)
{
    static const struct {
        const char* nm;
        char c;
    } NAMED[] = {
        {
            "amp;",
            '&',
        },
        {
            "lt;",
            '<',
        },
        {
            "gt;",
            '>',
        },
        {
            "quot;",
            '"',
        },
        {
            "apos;",
            '\'',
        },
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
            if (s[i] >= '0' && s[i] <= '9')
                dv = s[i] - '0';
            else if (hex && s[i] >= 'a' && s[i] <= 'f')
                dv = s[i] - 'a' + 10;
            else if (hex && s[i] >= 'A' && s[i] <= 'F')
                dv = s[i] - 'A' + 10;
            if (dv < 0)
                return 0;
            cp = cp * (hex ? 16u : 10u) + (unsigned long)dv;
            if (cp > 0x10FFFF)
                return 0;
            digits++;
        }
        if (!digits || i >= rem || s[i] != ';')
            return 0;
        if (cp < 0x80) {
            out[0] = (char)cp;
            *outlen = 1;
        } else if (cp < 0x800) {
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

static int sm_ci_eq(const char* p, size_t n, const char* lit)
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

static int sm_ci_prefix(const char* p, const char* lit)
{
    while (*lit) {
        if ((unsigned char)(*p++ | 0x20) != (unsigned char)(*lit++ | 0x20))
            return 0;
    }
    return 1;
}

static int sm_scan(JSContext* ctx, const char* p, size_t n, sm_doc_t* out)
{
    size_t i = 0;
    int root_seen = 0;

    memset(out, 0, sizeof(*out));
    while (i < n) {
        size_t tag_end, name_end;
        int is_close;
        if (p[i] != '<') {
            i++;
            continue;
        }
        if (sm_ci_prefix(p + i + 1, "!--")) {
            const char* e = sm_find(p + i + 4, n - i - 4, "-->", 3);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated comment at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 3;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '?') {
            const char* e = sm_find(p + i + 2, n - i - 2, "?>", 2);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated processing instruction at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 2;
            continue;
        }
        if (sm_ci_prefix(p + i + 1, "![CDATA[")) {
            const char* e = sm_find(p + i + 9, n - i - 9, "]]>", 3);
            if (!e) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated CDATA at byte %zu", i);
                goto fail;
            }
            i = (size_t)(e - p) + 3;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '!') {
            const char* e = (const char*)memchr(p + i + 2, '>', n - i - 2);
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
        while (name_end < n && p[name_end] != '>' && p[name_end] != '/' && p[name_end] != ' ' && p[name_end] != '\t' && p[name_end] != '\r' && p[name_end] != '\n')
            name_end++;
        if (name_end >= n) {
            JS_ThrowSyntaxError(ctx,
                "Sitemap.parse: unterminated tag at byte %zu", i);
            goto fail;
        }
        if (!is_close && !root_seen) {
            if (sm_ci_eq(p + i + 1, name_end - (i + 1), "sitemapindex"))
                out->is_index = 1;
            root_seen = 1;
        }
        if (!is_close && sm_ci_eq(p + i + 1, name_end - (i + 1), "loc")) {
            size_t ct = name_end, body, blen;
            const char* cl;
            while (ct < n && p[ct] != '>')
                ct++;
            if (ct >= n) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: unterminated <loc> tag at byte %zu", i);
                goto fail;
            }
            body = ct + 1;
            cl = NULL;
            {
                size_t max_q = body + SM_MAX_LOC_TEXT;
                for (size_t q = body; q <= max_q && q + 5 <= n; q++) {
                    if (p[q] == '<' && p[q + 1] == '/' && (p[q + 2] | 0x20) == 'l' && (p[q + 3] | 0x20) == 'o' && (p[q + 4] | 0x20) == 'c') {
                        cl = p + q;
                        break;
                    }
                }
            }
            if (!cl) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: <loc> at byte %zu never closes (over the "
                    "%d-byte url budget?)",
                    i, SM_MAX_LOC_TEXT);
                goto fail;
            }
            blen = (size_t)(cl - p) - body;
            cl = (const char*)memchr(p + body, '>', n - body);
            if (!cl) {
                JS_ThrowSyntaxError(ctx,
                    "Sitemap.parse: </loc> at byte %zu never closes", body);
                goto fail;
            }
            while (blen && (p[body] == ' ' || p[body] == '\t' || p[body] == '\r' || p[body] == '\n')) {
                body++;
                blen--;
            }
            while (blen && (p[body + blen - 1] == ' ' || p[body + blen - 1] == '\t' || p[body + blen - 1] == '\r' || p[body + blen - 1] == '\n'))
                blen--;
            if (blen) {
                char* dec = (char*)malloc(blen + 1);
                size_t di = 0, si = 0;
                int rc;
                if (!dec) {
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                while (si < blen) {
                    if (p[body + si] == '&') {
                        char one[4];
                        int one_n = 0;
                        size_t used = sm_entity(p + body + si, blen - si,
                            one, &one_n);
                        if (used) {
                            if (di + (size_t)one_n > blen) {
                                free(dec);
                                JS_ThrowOutOfMemory(ctx);
                                goto fail;
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
                if (rc < 0) {
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
            }
            i = (size_t)(cl - p) + 1;
            continue;
        }
        tag_end = name_end;
        while (tag_end < n && p[tag_end] != '>')
            tag_end++;
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

static JSValue dyn_sitemap_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sm_doc_t d;
    JSValue arr;
    size_t len = 0, i;
    const char* xml;

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
    {
        size_t k = 0;
        while (k < len && (xml[k] == ' ' || xml[k] == '\t' || xml[k] == '\r' || xml[k] == '\n'))
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
    if (JS_IsException(arr)) {
        sm_doc_free(&d);
        return JS_EXCEPTION;
    }
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

static JSValue sm_fetch_locs(JSContext* ctx, JSValueConst fe, const char* url,
    int* pis_index)
{
    JSAtom gatom = JS_NewAtom(ctx, "get");
    JSValue uv, res, body, arr;
    const char *body_s, *bv;
    size_t body_n = 0, k;
    sm_doc_t d;
    int32_t st = 0;

    *pis_index = 0;
    uv = JS_NewString(ctx, url);
    if (JS_IsException(uv)) {
        JS_FreeAtom(ctx, gatom);
        return uv;
    }
    res = JS_Invoke(ctx, fe, gatom, 1, (JSValueConst*)&uv);
    JS_FreeValue(ctx, uv);
    JS_FreeAtom(ctx, gatom);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    {
        JSValue sv = JS_GetPropertyStr(ctx, res, "status");
        int rc = 0;
        if (!JS_IsUndefined(sv))
            rc = JS_ToInt32(ctx, &st, sv);
        JS_FreeValue(ctx, sv);
        if (rc) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if (st < 200 || st > 299) {
            JS_FreeValue(ctx, res);
            JS_ThrowRangeError(ctx,
                "Sitemap.list: %s answered HTTP %d (only a 2xx body can be "
                "parsed as a sitemap)",
                url, st);
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
    if (JS_IsException(arr)) {
        sm_doc_free(&d);
        return JS_EXCEPTION;
    }
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

static JSValue sm_make_fetcher(JSContext* ctx, JSValueConst opts)
{
    static const char src[] = "import { Fetcher } from \"dyna:scrape\";\n"
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
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v))
        goto sweep;
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        dyn_scrape_promise_swallow(ctx, v);
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
    {
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

static JSValue dyn_sitemap_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue fe = JS_UNDEFINED, urls = JS_UNDEFINED, out = JS_UNDEFINED;
    const char* url;
    int is_index = 0;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Sitemap.list(url[, fetcherOrOpts])");
    url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSAtom gatom = JS_NewAtom(ctx, "get");
        JSValue gf = JS_GetProperty(ctx, argv[1], gatom);
        JS_FreeAtom(ctx, gatom);
        if (JS_IsFunction(ctx, gf)) {
            JS_FreeValue(ctx, gf);
            fe = JS_DupValue(ctx, argv[1]);
        } else {
            JS_FreeValue(ctx, gf);
            fe = sm_make_fetcher(ctx, argv[1]);
            if (JS_IsException(fe)) {
                JS_FreeCString(ctx, url);
                return JS_EXCEPTION;
            }
        }
    } else {
        fe = sm_make_fetcher(ctx, JS_UNDEFINED);
        if (JS_IsException(fe)) {
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }
    urls = sm_fetch_locs(ctx, fe, url, &is_index);
    JS_FreeCString(ctx, url);
    if (JS_IsException(urls))
        goto out_err;
    if (!is_index) {
        out = urls;
        urls = JS_UNDEFINED;
        goto out_ok;
    }
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
            const char* kid_s;
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
            if (src) {
                JS_FreeValue(ctx, sub);
                goto out_err;
            }
            for (j = 0; j < sl; j++) {
                JSValue e = JS_GetPropertyUint32(ctx, sub, j);
                uint32_t ol = 0;
                JSValue ov;
                if (JS_IsException(e)) {
                    JS_FreeValue(ctx, sub);
                    goto out_err;
                }
                ov = JS_GetPropertyStr(ctx, out, "length");
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

static int dyn_scrape_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_rb_class_id, &dyn_rb_class,
            dyn_rb_proto, countof(dyn_rb_proto),
            dyn_rb_ctor, "Robots")
        < 0)
        return -1;
    {
        JSRuntime* rt = JS_GetRuntime(ctx);
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
            dyn_cr_ctor, "Crawl")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_cr_class_id);
        if (!JS_IsObject(proto))
            return -1;
        {
            JSValue ctor = JS_GetPropertyStr(ctx, proto, "constructor");
            JS_FreeValue(ctx, proto);
            if (!JS_IsFunction(ctx, ctor)) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
            if (JS_SetPropertyStr(ctx, ctor, "resume",
                    JS_NewCFunction(ctx, dyn_cr_resume,
                        "resume", 2))
                < 0) {
                JS_FreeValue(ctx, ctor);
                return -1;
            }
            JS_FreeValue(ctx, ctor);
        }
    }
    if (dyn_register_class(ctx, m, &dyn_fe_class_id, &dyn_fe_class,
            dyn_fe_proto, countof(dyn_fe_proto),
            dyn_fe_ctor, "Fetcher")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ex_class_id, &dyn_ex_class,
            dyn_ex_proto, countof(dyn_ex_proto),
            dyn_ex_ctor, "Extractor")
        < 0)
        return -1;
    {
        JSValue sm = JS_NewObject(ctx);
        if (JS_IsException(sm))
            return -1;
        if (JS_SetPropertyStr(ctx, sm, "parse",
                JS_NewCFunction(ctx, dyn_sitemap_parse,
                    "parse", 1))
                < 0
            || JS_SetPropertyStr(ctx, sm, "list",
                   JS_NewCFunction(ctx, dyn_sitemap_list,
                       "list", 1))
                < 0
            || JS_SetModuleExport(ctx, m, "Sitemap", sm) < 0) {
            JS_FreeValue(ctx, sm);
            return -1;
        }
    }
    return 0;
}

int js_nat_init_scrape(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:scrape", dyn_scrape_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Robots");
    JS_AddModuleExport(ctx, m, "Extractor");
    JS_AddModuleExport(ctx, m, "Fetcher");
    JS_AddModuleExport(ctx, m, "Crawl");
    JS_AddModuleExport(ctx, m, "Sitemap");
    return 0;
}

#endif
