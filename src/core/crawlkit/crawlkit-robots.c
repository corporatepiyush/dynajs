#include "crawlkit.h"

#define RB_MAX_BYTES (512 * 1024)
#define RB_MAX_RULES 1000
#define RB_MAX_PATH 2048
#define RB_MAX_GROUPS 64
#define RB_MAX_UAS_PER_GROUP 8
#define RB_MAX_AGENT 128

typedef struct {
    size_t first, n;
    double delay;
    int is_star, score, picked;
    char* uas[RB_MAX_UAS_PER_GROUP];
    size_t n_uas;
} rb_grp_t;

void rb_free(rb_t* r)
{
    size_t i;
    for (i = 0; i < r->n; i++)
        free(r->rules[i].path);
    free(r->rules);
    free(r->order);
    for (i = 0; i < r->n_site; i++)
        free(r->sitemaps[i]);
    free(r->sitemaps);
    free(r);
}

static int rb_hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
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

static int rb_must_enc(unsigned c)
{
    return c >= 0x80 || c < 0x21 || c == 0x7F || rb_reserved((unsigned char)c);
}
static size_t rb_norm(const char* s, size_t n, char* out, size_t cap, int rule)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
    while (i < n && o + 3 < cap) {
        unsigned c = (unsigned char)s[i];
        int raw;
        if (c == '%' && i + 2 < n && rb_hexval((unsigned char)s[i + 1]) >= 0 && rb_hexval((unsigned char)s[i + 2]) >= 0) {
            c = (unsigned)(rb_hexval((unsigned char)s[i + 1]) << 4 | rb_hexval((unsigned char)s[i + 2]));
            i += 3;
            raw = !rb_must_enc(c);
        } else {
            i++;
            if (c == '/')
                raw = 1;
            else if (rule && c == '*')
                raw = 1;
            else if (rule && c == '$')
                raw = i == n;
            else
                raw = !rb_must_enc(c);
        }
        if (raw) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0xF];
        }
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

static unsigned rb_rule_key(const rb_rule_t* u)
{
    if (u->len < 2 || u->path[0] == '*' || u->path[1] == '*'
        || (u->path[1] == '$' && u->len == 2))
        return 256;
    return (unsigned char)u->path[1];
}

static int rb_rule_cmp(const void* a, const void* b)
{
    const rb_rule_t* x = (const rb_rule_t*)a;
    const rb_rule_t* y = (const rb_rule_t*)b;
    if (x->len != y->len)
        return x->len > y->len ? -1 : 1;
    return (int)y->allow - (int)x->allow;
}

static void rb_index(rb_t* r)
{
    size_t i;
    uint32_t k, at = 0;
    uint32_t fill[257];

    if (r->n < 2)
        return;
    qsort(r->rules, r->n, sizeof *r->rules, rb_rule_cmp);
    if (r->n > UINT32_MAX)
        return;
    r->order = (uint32_t*)malloc(r->n * sizeof *r->order);
    if (!r->order)
        return;
    memset(r->bstart, 0, sizeof r->bstart);
    for (i = 0; i < r->n; i++)
        r->bstart[rb_rule_key(&r->rules[i]) + 1]++;
    for (k = 0; k < 257; k++) {
        uint32_t c = r->bstart[k + 1];
        r->bstart[k] = at;
        fill[k] = at;
        at += c;
    }
    r->bstart[257] = at;
    for (i = 0; i < r->n; i++)
        r->order[fill[rb_rule_key(&r->rules[i])]++] = (uint32_t)i;
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

size_t rb_token_len(const char* s, size_t n)
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

int rb_parse(rb_t* r, const char* s, size_t n, const char* agent, rb_atod_fn atod)
{
    rb_rule_t* arena = NULL;
    size_t n_arena = 0, cap_arena = 0;
    rb_grp_t* groups;
    size_t n_groups = 0, cap_groups = 16;
    size_t token_a, aglen, n_pick = 0, gi, z;
    int best_score = 0;
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
                if (dyn_ascii_ieq_lower_lit(k, kn, "user-agent")) {
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
                    if (dyn_ascii_ieq_lower_lit(k, kn, "sitemap")) {
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
                        if (dyn_ascii_ieq_lower_lit(k, kn, "crawl-delay")) {
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
                                char sbuf[64];
                                char* buf = vn < sizeof sbuf ? sbuf : (char*)malloc(vn + 1);
                                const char* end;
                                double d;
                                if (!buf)
                                    goto oom;
                                memcpy(buf, v, vn);
                                buf[vn] = 0;
                                d = atod(buf, &end);
                                if (end != buf && *end == '\0')
                                    g->delay = d;
                                if (buf != sbuf)
                                    free(buf);
                            }
                        } else if (dyn_ascii_ieq_lower_lit(k, kn, "disallow") || dyn_ascii_ieq_lower_lit(k, kn, "allow")) {
                            int allow = dyn_ascii_ieq_lower_lit(k, kn, "allow");
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
                                    size_t rn = rb_norm(copy, pn,
                                        copy + pn,
                                        3 * pn + 1, 1);
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
        size_t u;
        for (u = 0; u < g->n_uas; u++) {
            size_t ul = strlen(g->uas[u]);
            size_t utok = rb_token_len(g->uas[u], ul);
            if (rb_ci_ncmp(g->uas[u], ul, agent, aglen))
                score = score > 2 ? score : 2;
            else if (utok && utok == token_a && rb_ci_ncmp(g->uas[u], utok, agent, token_a))
                score = score > 1 ? score : 1;
            else if (!strcmp(g->uas[u], "*"))
                g->is_star = 1;
        }
        g->score = score;
        if (score > best_score)
            best_score = score;
    }
    for (gi = 0; gi < n_groups; gi++) {
        rb_grp_t* g = &groups[gi];
        g->picked = best_score > 0 ? g->score == best_score : g->is_star;
        if (!g->picked)
            continue;
        n_pick += g->n;
        if (g->delay >= 0 && g->delay > r->delay)
            r->delay = g->delay;
    }
    if (n_pick) {
        r->rules = (rb_rule_t*)calloc(n_pick, sizeof(*r->rules));
        if (!r->rules)
            goto oom;
        for (gi = 0; gi < n_groups; gi++)
            if (groups[gi].picked) {
                memcpy(r->rules + r->n, arena + groups[gi].first, groups[gi].n * sizeof(*r->rules));
                r->n += groups[gi].n;
            }
        r->cap = n_pick;
        rb_index(r);
    }
    for (gi = 0; gi < n_groups; gi++)
        if (!groups[gi].picked)
            for (z = groups[gi].first; z < groups[gi].first + groups[gi].n; z++)
                free(arena[z].path);
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

int rb_allows_path(const rb_t* r, const char* p, size_t pn)
{
    size_t i, gn, bn, gi = 0, bi = 0;
    const uint32_t *g, *bk;
    int verdict = 1;
    char* norm;
    size_t nn;

    if (pn > 2048)
        pn = 2048;
    norm = (char*)malloc(3 * pn + 1);
    if (!norm)
        return 1;
    nn = rb_norm(p, pn, norm, 3 * pn + 1, 0);
    if (!r->order) {
        for (i = 0; i < r->n; i++)
            if (rb_match(r->rules[i].path, r->rules[i].len, norm, nn)) {
                verdict = r->rules[i].allow;
                break;
            }
        free(norm);
        return verdict;
    }
    g = r->order + r->bstart[256];
    gn = r->bstart[257] - r->bstart[256];
    bk = NULL;
    bn = 0;
    if (nn >= 2) {
        unsigned k = (unsigned char)norm[1];
        bk = r->order + r->bstart[k];
        bn = r->bstart[k + 1] - r->bstart[k];
    }
    while (gi < gn || bi < bn) {
        uint32_t x = (bi >= bn || (gi < gn && g[gi] < bk[bi])) ? g[gi++] : bk[bi++];
        if (rb_match(r->rules[x].path, r->rules[x].len, norm, nn)) {
            verdict = r->rules[x].allow;
            break;
        }
    }
    free(norm);
    return verdict;
}

void fe_dirs_add(char** out, size_t* len, size_t* cap,
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

char* fe_parse_robots_tag(const char* joined, const char* agent)
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
