#include "svg-doc.h"
#include <ctype.h>
#include <strings.h>

struct CssIndex;
static void css_index_build(Ctx* ctx);
static void css_index_free(struct CssIndex* ix);

static char* xstrdup(const char* s) {
    return s ? svg_strdup(s) : NULL;
}

static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static char* strip_comments(const char* s) {
    size_t cap = strlen(s) + 1, len = 0;
    char* out = (char*)svg_malloc(cap);
    if (!out) return NULL;
    for (size_t i = 0; s[i]; ) {
        if (s[i] == '/' && s[i + 1] == '*') {
            const char* e = strstr(s + i + 2, "*/");
            if (!e) break;
            i = (size_t)(e - s) + 2;
            continue;
        }
        out[len++] = s[i++];
    }
    out[len] = 0;
    return out;
}

static int class_contains(const char* cls, const char* want) {
    if (!cls || !want || !*want) return 0;
    const char* q = cls;
    while (*q) {
        while (*q && isspace((unsigned char)*q)) q++;
        const char* e = q;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e == q) break;
        size_t len = (size_t)(e - q);
        if (len == strlen(want) && !strncmp(q, want, len)) return 1;
        q = e;
    }
    return 0;
}

static int token_contains(const char* list, const char* want) {
    if (!list || !want || !*want) return 0;
    const char* q = list;
    while (*q) {
        while (*q && isspace((unsigned char)*q)) q++;
        const char* e = q;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e == q) break;
        size_t len = (size_t)(e - q);
        if (len == strlen(want) && !strncmp(q, want, len)) return 1;
        q = e;
    }
    return 0;
}

static int attr_values_equal(const char* a, const char* b) {
    if (!strcmp(a, b)) return 1;
    for (const char* q = a; *q; q++)
        if (!isalpha((unsigned char)*q)) return 0;
    for (const char* q = b; *q; q++)
        if (!isalpha((unsigned char)*q)) return 0;
    return !strcasecmp(a, b);
}

typedef struct {
    char  name[128], val[256];
    char  op[3];
    unsigned char vcolor;
    Color vc;
} AttrSel;

static int attr_parse(const char* expr, AttrSel* a) {
    size_t n = 0, vn = 0;
    const char* p = expr;
    memset(a->op, 0, sizeof a->op);
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '~' &&
           *p != '|' && *p != '^' && *p != '$' && *p != '*' && *p != ']') {
        if (n + 1 < sizeof a->name) a->name[n++] = *p;
        p++;
    }
    a->name[n] = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p && *p != ']') {
        a->op[0] = *p++;
        if (*p == '=') { a->op[1] = '='; p++; }
    }
    while (*p && isspace((unsigned char)*p)) p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') quote = *p++;
    while (*p && (quote ? *p != quote : *p != ']')) {
        if (vn + 1 < sizeof a->val) a->val[vn++] = *p;
        p++;
    }
    a->val[vn] = 0;
    while (vn > 0 && isspace((unsigned char)a->val[vn - 1])) a->val[--vn] = 0;
    a->vcolor = (unsigned char)color_parse(a->val, &a->vc);
    return a->name[0] != 0;
}

static int attr_match(const XMLNode* xn, const AttrSel* a) {
    const char* av = xml_attr(xn, a->name);
    if (!a->op[0]) return av != NULL;
    if (!av) return 0;
    const char* val = a->val;
    if (!strcmp(a->op, "=")) {
        if (attr_values_equal(av, val)) return 1;

        Color c;
        if (!a->vcolor || !color_parse(av, &c)) return 0;
        return fabsf(c.r - a->vc.r) < 1e-4f && fabsf(c.g - a->vc.g) < 1e-4f &&
               fabsf(c.b - a->vc.b) < 1e-4f && fabsf(c.a - a->vc.a) < 1e-4f;
    }
    if (!strcmp(a->op, "~=")) return token_contains(av, val);
    if (!strcmp(a->op, "|=")) {
        size_t l = strlen(val);
        return !strncmp(av, val, l) && (av[l] == 0 || av[l] == '-');
    }
    if (!strcmp(a->op, "^=")) return !strncmp(av, val, strlen(val));
    if (!strcmp(a->op, "$=")) {
        size_t la = strlen(av), lv = strlen(val);
        return la >= lv && !strcmp(av + la - lv, val);
    }
    if (!strcmp(a->op, "*=")) return strstr(av, val) != NULL;
    return 0;
}

static int attr_selector_match(const XMLNode* xn, const char* expr) {
    AttrSel a;
    if (!attr_parse(expr, &a)) return 0;
    return attr_match(xn, &a);
}

static int element_siblings(const XMLNode* xn, int* index, int* total) {
    if (!xn->parent) { *index = 1; *total = 1; return 0; }
    int idx = 0;
    for (int i = 0; i < xn->parent->nkids; i++) {
        if (xn->parent->kids[i] == xn) idx = i + 1;
    }
    *index = idx;
    *total = xn->parent->nkids;
    return 0;
}

static int nth_matches(const char* arg, int index) {
    while (*arg && isspace((unsigned char)*arg)) arg++;
    if (!strcasecmp(arg, "odd")) return index % 2 == 1;
    if (!strcasecmp(arg, "even")) return index % 2 == 0;
    const char* np = strchr(arg, 'n');
    if (!np) np = strchr(arg, 'N');
    if (np) {
        long a = 1, b = 0;
        const char* q = arg;
        if (q < np) {
            if (*q == '+') q++;
            else if (*q == '-') { a = -1; q++; }
            if (q < np) {
                char* end = NULL;
                long v = strtol(q, &end, 10);
                if (end != q) a = (*arg == '-') ? -v : v;
            }
        }
        q = np + 1;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '+' || *q == '-') {
            char* end = NULL;
            long v = strtol(q + 1, &end, 10);
            if (end != q + 1) b = (*q == '-') ? -v : v;
        }
        if (a == 0) return index == (int)b;
        long d = (long)index - b;
        if (d % a != 0) return 0;
        return d / a >= 0;
    }
    char* end = NULL;
    long v = strtol(arg, &end, 10);
    if (end == arg) return 0;
    return index == (int)v;
}

static int match_compound(const XMLNode* xn, const char* sel);

static int match_pseudo(const XMLNode* xn, const char* name, const char* arg,
                        int has_arg) {
    int idx = 1, total = 1;
    element_siblings(xn, &idx, &total);
    if (!strcasecmp(name, "root"))
        return !xn->parent || !strcmp(xn->parent->name, "#document");
    if (!strcasecmp(name, "first-child")) return idx == 1;
    if (!strcasecmp(name, "last-child")) return idx == total;
    if (!strcasecmp(name, "only-child")) return total == 1;
    if (!strcasecmp(name, "nth-child") && has_arg)
        return nth_matches(arg, idx);
    if (!strcasecmp(name, "empty"))
        return xn->nkids == 0 && (!xn->text || !*xn->text);
    if (!strcasecmp(name, "not") && has_arg) return !match_compound(xn, arg);
    return 0;
}

static int match_compound(const XMLNode* xn, const char* sel) {
    char buf[256];
    size_t n = strlen(sel);
    if (n >= sizeof buf) return 0;
    memcpy(buf, sel, n + 1);

    char* p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '*') {
        p++;
    } else if (*p && *p != '#' && *p != '.' && *p != '[' && *p != ':') {
        char* tag = p;
        while (*p && !strchr("#.[:", *p)) p++;
        char save = *p;
        *p = 0;
        int ok = !strcmp(tag, xn->name);
        *p = save;
        if (!ok) return 0;
    }

    while (*p) {
        if (*p == '#') {
            p++;
            char* id = p;
            while (*p && !strchr("#.[:", *p)) p++;
            char save = *p;
            *p = 0;
            const char* xid = xml_attr(xn, "id");
            int ok = xid && !strcmp(id, xid);
            *p = save;
            if (!ok) return 0;
        } else if (*p == '.') {
            p++;
            char* cls = p;
            while (*p && !strchr("#.[:", *p)) p++;
            char save = *p;
            *p = 0;
            int ok = class_contains(xml_attr(xn, "class"), cls);
            *p = save;
            if (!ok) return 0;
        } else if (*p == '[') {
            char* e = strchr(p + 1, ']');
            if (!e) return 0;
            char expr[256];
            size_t l = (size_t)(e - p - 1);
            if (l >= sizeof expr) l = sizeof expr - 1;
            memcpy(expr, p + 1, l);
            expr[l] = 0;
            if (!attr_selector_match(xn, expr)) return 0;
            p = e + 1;
        } else if (*p == ':') {
            p++;
            char* name = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '-')) p++;
            char namebuf[64];
            size_t nl = (size_t)(p - name);
            if (nl >= sizeof namebuf) return 0;
            memcpy(namebuf, name, nl);
            namebuf[nl] = 0;
            char arg[128] = "";
            int has_arg = 0;
            if (*p == '(') {
                char* e = strchr(p + 1, ')');
                if (!e) return 0;
                size_t al = (size_t)(e - p - 1);
                if (al >= sizeof arg) al = sizeof arg - 1;
                memcpy(arg, p + 1, al);
                arg[al] = 0;
                has_arg = 1;
                p = e + 1;
            }
            if (!match_pseudo(xn, namebuf, arg, has_arg)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

#define SEL_MAX_CLS 4
typedef struct SelTok {
    char comp[128];
    char comb;
    unsigned char compiled;
    unsigned char complex;
    unsigned char ncls;
    char tag[32];
    char id[64];
    char cls[SEL_MAX_CLS][32];
    unsigned char nattr;
    AttrSel* attr;
} SelTok;

static int sel_copy(char* dst, size_t cap, const char* p, const char* e) {
    size_t n = (size_t)(e - p);
    if (n >= cap) return 0;
    memcpy(dst, p, n);
    dst[n] = 0;
    return 1;
}

static void sel_compile(SelTok* t) {
    t->compiled = 0; t->complex = 0; t->ncls = 0; t->tag[0] = 0; t->id[0] = 0;
    t->nattr = 0; t->attr = NULL;
    const char* p = t->comp;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '*') {
        p++;
    } else if (*p && *p != '#' && *p != '.' && *p != '[' && *p != ':') {
        const char* e = p;
        while (*e && !strchr("#.[:", *e)) e++;
        if (!sel_copy(t->tag, sizeof t->tag, p, e)) return;
        p = e;
    }
    while (*p) {
        if (*p == '#' || *p == '.') {
            char kind = *p++;
            const char* e = p;
            while (*e && !strchr("#.[:", *e)) e++;
            if (kind == '#') {

                if (t->id[0]) t->complex = 1;
                else if (!sel_copy(t->id, sizeof t->id, p, e)) return;
            } else {
                if (t->ncls >= SEL_MAX_CLS) t->complex = 1;
                else if (!sel_copy(t->cls[t->ncls], sizeof t->cls[0], p, e)) return;
                else t->ncls++;
            }
            p = e;
        } else if (*p == '[') {
            const char* e = strchr(p + 1, ']');
            AttrSel tmp;
            char expr[256];
            if (!e || !sel_copy(expr, sizeof expr, p + 1, e) ||
                !attr_parse(expr, &tmp)) { t->complex = 1; break; }
            AttrSel* na = (AttrSel*)svg_realloc(t->attr, (size_t)(t->nattr + 1) * sizeof(AttrSel));
            if (!na) { t->complex = 1; break; }
            t->attr = na;
            t->attr[t->nattr++] = tmp;
            p = e + 1;
        } else if (*p == ':') {
            t->complex = 1;
            break;
        } else {
            return;
        }
    }
    t->compiled = 1;
}

static int match_compiled(const XMLNode* xn, const SelTok* t) {
    if (t->tag[0] && strcmp(t->tag, xn->name)) return 0;
    if (t->id[0]) {
        const char* xid = xml_attr(xn, "id");
        if (!xid || strcmp(t->id, xid)) return 0;
    }
    if (t->ncls) {
        const char* xc = xml_attr(xn, "class");
        for (int i = 0; i < t->ncls; i++)
            if (!class_contains(xc, t->cls[i])) return 0;
    }
    for (int i = 0; i < t->nattr; i++)
        if (!attr_match(xn, &t->attr[i])) return 0;
    return 1;
}

static int match_tok(const XMLNode* xn, const SelTok* t) {
    if (t->compiled) {
        if (!match_compiled(xn, t)) return 0;
        if (!t->complex) return 1;
    }
    return match_compound(xn, t->comp);
}

static int tokenize_selector(const char* sel, SelTok* t, int max) {
    int n = 0;
    const char* p = sel;
    char comb = 0;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '>') { comb = '>'; p++; continue; }
        const char* e = p;
        while (*e && !isspace((unsigned char)*e) && *e != '>') e++;
        size_t len = (size_t)(e - p);
        if (len >= sizeof t[0].comp) len = sizeof t[0].comp - 1;
        memcpy(t[n].comp, p, len);
        t[n].comp[len] = 0;
        t[n].comb = comb;
        sel_compile(&t[n]);
        comb = ' ';
        n++;
        p = e;
    }
    return n;
}

static int match_tokens(const XMLNode* xn, SelTok* t, int n) {
    if (!xn || n <= 0) return 0;
    if (!match_tok(xn, &t[n - 1])) return 0;
    if (n == 1) return 1;
    if (t[n - 1].comb == '>')
        return xn->parent && match_tokens(xn->parent, t, n - 1);
    for (const XMLNode* p = xn->parent; p; p = p->parent)
        if (match_tokens(p, t, n - 1)) return 1;
    return 0;
}

static void compound_key(const char* comp, char* kind, char** key) {
    *kind = CSS_KEY_NONE;
    *key = NULL;
    const char* best = NULL;
    char bk = CSS_KEY_NONE;
    const char* p = comp;
    if (*p && !strchr("*#.[:", *p)) { best = p; bk = CSS_KEY_TAG; }
    int depth = 0;
    for (; *p; p++) {
        if (*p == '[' || *p == '(') { depth++; continue; }
        if (*p == ']' || *p == ')') { if (depth) depth--; continue; }
        if (depth) continue;
        if (*p == '#') { best = p + 1; bk = CSS_KEY_ID; break; }
        if (*p == '.' && bk != CSS_KEY_CLASS) { best = p + 1; bk = CSS_KEY_CLASS; }
    }
    if (!best) return;
    size_t n = 0;
    while (best[n] && !strchr("#.[:", best[n])) n++;
    if (n == 0) return;
    char* k = (char*)svg_malloc(n + 1);
    if (!k) return;
    memcpy(k, best, n);
    k[n] = 0;
    *kind = bk;
    *key = k;
}

static int rule_may_match(const CssRule* r, const XMLNode* xn,
                          const char* xid, const char* xcls) {
    switch (r->key_kind) {
    case CSS_KEY_ID:    return xid && !strcmp(r->key, xid);
    case CSS_KEY_CLASS: return class_contains(xcls, r->key);
    case CSS_KEY_TAG:   return !strcmp(r->key, xn->name);
    default:            return 1;
    }
}

static int specificity_of(const char* sel) {
    int ids = 0, cls = 0, types = 0;
    for (const char* p = sel; *p; p++) {
        if (*p == '#') ids++;
        else if (*p == '.' || *p == '[' || *p == ':') cls++;
        else if (isalpha((unsigned char)*p)) {

            if (p == sel || isspace((unsigned char)p[-1]) || p[-1] == '>' ||
                p[-1] == ',')
                types++;
        }
    }
    return ids * 10000 + cls * 100 + types;
}

static void add_rule(Ctx* ctx, char* sel, char* decls, char* decls_imp) {
    if (ctx->nrules == ctx->caprules) {
        int cap = ctx->caprules ? ctx->caprules * 2 : 16;
        CssRule* r = (CssRule*)svg_realloc(ctx->rules, (size_t)cap * sizeof(CssRule));
        if (!r) return;
        ctx->rules = r;
        ctx->caprules = cap;
    }
    CssRule* r = &ctx->rules[ctx->nrules++];
    r->sel = xstrdup(trim(sel));
    r->decls = decls;
    r->decls_imp = decls_imp;
    r->spec = specificity_of(r->sel ? r->sel : "");
    r->toks = NULL;
    r->ntoks = 0;
    r->key_kind = CSS_KEY_NONE;
    r->key = NULL;
    if (r->sel) {
        SelTok t[32];
        int n = tokenize_selector(r->sel, t, 32);
        if (n > 0) {
            r->toks = (SelTok*)svg_malloc((size_t)n * sizeof(SelTok));
            if (r->toks) {
                memcpy(r->toks, t, (size_t)n * sizeof(SelTok));
                r->ntoks = n;
                compound_key(t[n - 1].comp, &r->key_kind, &r->key);
            } else {
                for (int i = 0; i < n; i++) svg_mfree(t[i].attr);
            }
        }
    }
}

static void parse_declarations(const char* body, char** out_normal,
                               char** out_imp) {

    size_t cap = strlen(body) + 2;
    char* normal = (char*)svg_malloc(cap);
    char* imp = (char*)svg_malloc(cap);
    if (!normal || !imp) { svg_mfree(normal); svg_mfree(imp); *out_normal = NULL; *out_imp = NULL; return; }
    normal[0] = 0;
    imp[0] = 0;
    const char* p = body;
    while (*p) {
        const char* e = p;
        while (*e && *e != ';') e++;
        char decl[512];
        size_t len = (size_t)(e - p);
        if (len >= sizeof decl) len = sizeof decl - 1;
        memcpy(decl, p, len);
        decl[len] = 0;
        char* d = trim(decl);
        if (*d) {
            char* bang = strstr(d, "!");
            int important = 0;
            if (bang) {
                char* q = bang + 1;
                while (*q && isspace((unsigned char)*q)) q++;
                if (!strncmp(q, "important", 9)) {
                    important = 1;
                    *bang = 0;
                    char* de = trim(d);
                    d = de;
                }
            }
            if (*d) {
                char* colon = strchr(d, ':');
                if (colon) {
                    size_t dl = strlen(d);
                    char* dst = important ? imp : normal;
                    size_t dl0 = strlen(dst);
                    memcpy(dst + dl0, d, dl);
                    dst[dl0 + dl] = ';';
                    dst[dl0 + dl + 1] = 0;
                }
            }
        }
        p = (*e == ';') ? e + 1 : e;
    }
    *out_normal = normal;
    *out_imp = imp;
}

static void collect_style_text(Ctx* ctx, const char* css) {
    char* src = strip_comments(css);
    if (!src) return;
    const char* p = src;
    while (*p) {
        const char* brace = strchr(p, '{');
        if (!brace) break;
        const char* end = strchr(brace, '}');
        if (!end) break;
        char sel[1024];
        size_t sl = (size_t)(brace - p);
        if (sl >= sizeof sel) sl = sizeof sel - 1;
        memcpy(sel, p, sl);
        sel[sl] = 0;

        char body[4096];
        size_t bl = (size_t)(end - brace - 1);
        if (bl >= sizeof body) bl = sizeof body - 1;
        memcpy(body, brace + 1, bl);
        body[bl] = 0;

        char* list = trim(sel);
        char* comma = list;
        while (comma) {
            char* next = strchr(comma, ',');
            if (next) *next++ = 0;
            char* one = trim(comma);
            if (*one) {
                char* normal = NULL, *imp = NULL;
                parse_declarations(body, &normal, &imp);
                add_rule(ctx, one, normal, imp);
            }
            comma = next;
        }
        p = end + 1;
    }
    svg_mfree(src);
}

void css_collect(Ctx* ctx, const XMLNode* root) {
    if (!ctx || !root) return;

    const XMLNode** stack = NULL;
    size_t n = 0, cap = 0;
    if (root->nkids > 0) {
        cap = 64;
        while (cap < (size_t)root->nkids) cap *= 2;
        stack = (const XMLNode**)svg_malloc(cap * sizeof(*stack));
        if (!stack) return;
        for (int i = root->nkids - 1; i >= 0; i--) stack[n++] = root->kids[i];
    }
    while (n > 0) {
        const XMLNode* k = stack[--n];
        if (!strcmp(k->name, "style") && k->text && *k->text)
            collect_style_text(ctx, k->text);
        if (k->nkids > 0) {
            if (n + (size_t)k->nkids > cap) {
                size_t nc = cap ? cap : 64;
                while (nc < n + (size_t)k->nkids) nc *= 2;
                const XMLNode** ns = (const XMLNode**)svg_realloc(
                    stack, nc * sizeof(*stack));
                if (!ns) { svg_mfree(stack); return; }
                stack = ns;
                cap = nc;
            }
            for (int i = k->nkids - 1; i >= 0; i--) stack[n++] = k->kids[i];
        }
    }
    svg_mfree(stack);
    css_index_build(ctx);
}

void css_free(Ctx* ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->nrules; i++) {
        svg_mfree(ctx->rules[i].sel);
        svg_mfree(ctx->rules[i].decls);
        svg_mfree(ctx->rules[i].decls_imp);
        for (int k = 0; k < ctx->rules[i].ntoks; k++) svg_mfree(ctx->rules[i].toks[k].attr);
        svg_mfree(ctx->rules[i].toks);
        svg_mfree(ctx->rules[i].key);
    }
    svg_mfree(ctx->rules);
    css_index_free(ctx->css_index);
    ctx->css_index = NULL;
    ctx->rules = NULL;
    ctx->nrules = ctx->caprules = 0;
    ctx->css_last = NULL;
}

typedef struct {
    uint32_t    hash;
    char        kind;
    const char* key;
    int         start, count;
} CssBucket;

struct CssIndex {
    CssBucket* tab;
    uint32_t   mask;
    int*       ridx;
    int        any_start, any_count;
    int*       cand;
};

static uint32_t css_hash(char kind, const char* k, size_t n) {
    uint32_t h = 2166136261u ^ (uint32_t)(unsigned char)kind;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)k[i]; h *= 16777619u; }
    return h ? h : 1;
}

static CssBucket* css_slot(const struct CssIndex* ix, char kind, const char* k,
                           size_t n, uint32_t h) {
    for (uint32_t i = h & ix->mask;; i = (i + 1) & ix->mask) {
        CssBucket* b = &ix->tab[i];
        if (!b->kind) return b;
        if (b->hash == h && b->kind == kind && strlen(b->key) == n &&
            !memcmp(b->key, k, n))
            return b;
    }
}

static void css_index_free(struct CssIndex* ix) {
    if (!ix) return;
    svg_mfree(ix->tab);
    svg_mfree(ix->ridx);
    svg_mfree(ix->cand);
    svg_mfree(ix);
}

static void css_index_build(Ctx* ctx) {
    css_index_free(ctx->css_index);
    ctx->css_index = NULL;
    int n = ctx->nrules;
    if (n <= 0) return;
    struct CssIndex* ix = (struct CssIndex*)svg_calloc(sizeof *ix);
    if (!ix) return;
    uint32_t cap = 16;
    while (cap < (uint32_t)n * 2u) cap <<= 1;
    ix->tab = (CssBucket*)svg_calloc((size_t)cap * sizeof(CssBucket));
    ix->ridx = (int*)svg_malloc((size_t)n * sizeof(int));
    ix->cand = (int*)svg_malloc((size_t)n * sizeof(int));
    if (!ix->tab || !ix->ridx || !ix->cand) { css_index_free(ix); return; }
    ix->mask = cap - 1;

    for (int i = 0; i < n; i++) {
        const CssRule* r = &ctx->rules[i];
        if (r->ntoks == 0) continue;
        if (r->key_kind == CSS_KEY_NONE || !r->key) { ix->any_count++; continue; }
        size_t kl = strlen(r->key);
        uint32_t h = css_hash(r->key_kind, r->key, kl);
        CssBucket* b = css_slot(ix, r->key_kind, r->key, kl, h);
        if (!b->kind) { b->kind = r->key_kind; b->key = r->key; b->hash = h; }
        b->count++;
    }

    int pos = ix->any_count;
    ix->any_start = 0;
    for (uint32_t i = 0; i <= ix->mask; i++)
        if (ix->tab[i].kind) { ix->tab[i].start = pos; pos += ix->tab[i].count; ix->tab[i].count = 0; }
    int any = 0;
    for (int i = 0; i < n; i++) {
        const CssRule* r = &ctx->rules[i];
        if (r->ntoks == 0) continue;
        if (r->key_kind == CSS_KEY_NONE || !r->key) { ix->ridx[any++] = i; continue; }
        size_t kl = strlen(r->key);
        CssBucket* b = css_slot(ix, r->key_kind, r->key, kl, css_hash(r->key_kind, r->key, kl));
        ix->ridx[b->start + b->count++] = i;
    }
    ctx->css_index = ix;
}

static int css_append_bucket(const struct CssIndex* ix, char kind, const char* k,
                             size_t kl, int nc) {
    if (!k || kl == 0) return nc;
    const CssBucket* b = css_slot(ix, kind, k, kl, css_hash(kind, k, kl));
    if (!b->kind) return nc;
    memcpy(ix->cand + nc, ix->ridx + b->start, (size_t)b->count * sizeof(int));
    return nc + b->count;
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static int css_candidates(const struct CssIndex* ix, const XMLNode* xn,
                          const char* xid, const char* xcls) {
    int nc = 0;
    memcpy(ix->cand, ix->ridx + ix->any_start, (size_t)ix->any_count * sizeof(int));
    nc = ix->any_count;
    if (xid) nc = css_append_bucket(ix, CSS_KEY_ID, xid, strlen(xid), nc);
    nc = css_append_bucket(ix, CSS_KEY_TAG, xn->name, strlen(xn->name), nc);
    int classes = 0;
    for (const char* q = xcls; q && *q;) {
        while (*q && isspace((unsigned char)*q)) q++;
        const char* e = q;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e > q) { nc = css_append_bucket(ix, CSS_KEY_CLASS, q, (size_t)(e - q), nc); classes++; }
        q = e;
    }
    if (nc > 1) {
        qsort(ix->cand, (size_t)nc, sizeof(int), cmp_int);
        if (classes > 1) {
            int w = 1;
            for (int i = 1; i < nc; i++)
                if (ix->cand[i] != ix->cand[w - 1]) ix->cand[w++] = ix->cand[i];
            nc = w;
        }
    }
    return nc;
}

static void apply_to(Ctx* ctx, const XMLNode* xn, Style* st, int important) {
    if (ctx->nrules == 0) return;
    int* idx = ctx->css_last_idx;
    int n;
    if (ctx->css_last == xn) {
        n = ctx->css_nlast;
    } else {
        const char* xid = xml_attr(xn, "id");
        const char* xcls = xml_attr(xn, "class");
        n = 0;
        const struct CssIndex* ix = ctx->css_index;
        if (ix) {
            int nc = css_candidates(ix, xn, xid, xcls);
            for (int c = 0; c < nc && n < 128; c++) {
                const CssRule* r = &ctx->rules[ix->cand[c]];
                if (!match_tokens(xn, r->toks, r->ntoks)) continue;
                idx[n++] = ix->cand[c];
            }
        } else {
            for (int i = 0; i < ctx->nrules && n < 128; i++) {
                const CssRule* r = &ctx->rules[i];
                if (r->ntoks == 0 || !rule_may_match(r, xn, xid, xcls)) continue;
                if (!match_tokens(xn, r->toks, r->ntoks)) continue;
                idx[n++] = i;
            }
        }
        ctx->css_last = xn;
        ctx->css_nlast = n;
    }
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && ctx->rules[idx[j]].spec > ctx->rules[v].spec) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        const char* d = important ? ctx->rules[idx[i]].decls_imp
                                  : ctx->rules[idx[i]].decls;
        if (d && *d) style_apply_decls(ctx, st, d);
    }
}

void css_apply(Ctx* ctx, const XMLNode* xn, Style* st) {
    if (ctx && xn && st) apply_to(ctx, xn, st, 0);
}

void css_apply_important(Ctx* ctx, const XMLNode* xn, Style* st) {
    if (ctx && xn && st) apply_to(ctx, xn, st, 1);
}
