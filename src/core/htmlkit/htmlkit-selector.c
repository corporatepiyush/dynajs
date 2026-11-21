#include "htmlkit.h"

void sel_free(sel_t* s)
{
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->npart; i++) {
        free(s->parts[i].a);
        free(s->parts[i].b);
    }
    free(s->parts);
    free(s->units);
    free(s->groups);
    free(s);
}

void sel_free_v(void* p) { sel_free((sel_t*)p); }

char* sel_dup(const char* s, size_t n)
{
    char* p = (char*)malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

static int sel_add_part(sel_t* s, uint8_t kind, uint8_t op, char* a, char* b)
{
    if (s->npart == s->cpart) {
        uint32_t nc = s->cpart ? s->cpart * 2 : 8;
        sel_part_t* np = (sel_part_t*)realloc(s->parts, nc * sizeof *np);
        if (!np) {
            free(a);
            free(b);
            return -1;
        }
        s->parts = np;
        s->cpart = nc;
    }
    memset(&s->parts[s->npart], 0, sizeof s->parts[0]);
    s->parts[s->npart].kind = kind;
    s->parts[s->npart].op = op;
    s->parts[s->npart].a = a;
    s->parts[s->npart].b = b;
    s->npart++;
    return 0;
}

static int sel_add_unit(sel_t* s, uint8_t comb, uint32_t first, uint32_t n)
{
    if (s->nunit == s->cunit) {
        uint32_t nc = s->cunit ? s->cunit * 2 : 8;
        sel_unit_t* np = (sel_unit_t*)realloc(s->units, nc * sizeof *np);
        if (!np)
            return -1;
        s->units = np;
        s->cunit = nc;
    }
    s->units[s->nunit].comb = comb;
    s->units[s->nunit].first = first;
    s->units[s->nunit].n = n;
    s->nunit++;
    return 0;
}

static int sel_ident_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

static int sel_ident(const char* s, size_t n, size_t* i, char** out)
{
    size_t st = *i;
    while (*i < n && sel_ident_char((unsigned char)s[*i]))
        (*i)++;
    if (*i == st)
        return -1;
    *out = sel_dup(s + st, *i - st);
    return *out ? 0 : -1;
}

static int sel_compound(sel_t* sel, const char* s, size_t n, size_t* i,
    uint32_t* count, char* err, size_t errsz)
{
    *count = 0;
    for (;;) {
        char *a = NULL, *b = NULL;
        if (*i >= n)
            break;
        if (s[*i] == '*') {
            (*i)++;
            if (sel_add_part(sel, SP_ANY, 0, NULL, NULL) < 0)
                return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == '#' || s[*i] == '.') {
            uint8_t k = s[*i] == '#' ? SP_ID : SP_CLASS;
            (*i)++;
            if (sel_ident(s, n, i, &a) < 0) {
                snprintf(err, errsz, "expected a name after %c", s[*i - 1]);
                return -1;
            }
            if (sel_add_part(sel, k, 0, a, NULL) < 0)
                return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == '[') {
            size_t st;
            uint8_t op = AO_HAS;
            (*i)++;
            st = *i;
            while (*i < n && s[*i] != ']' && s[*i] != '=' && s[*i] != '^'
                && s[*i] != '$' && s[*i] != '*' && s[*i] != '~'
                && s[*i] != '|')
                (*i)++;
            if (*i == st) {
                snprintf(err, errsz, "empty attribute selector");
                return -1;
            }
            a = sel_dup(s + st, *i - st);
            if (!a)
                return -1;
            {
                size_t k, l = strlen(a);
                for (k = 0; k < l; k++)
                    a[k] = (char)ht_lower((unsigned char)a[k]);
            }
            if (*i < n && s[*i] != ']') {
                switch (s[*i]) {
                case '^':
                    op = AO_PREFIX;
                    (*i)++;
                    break;
                case '$':
                    op = AO_SUFFIX;
                    (*i)++;
                    break;
                case '*':
                    op = AO_CONTAINS;
                    (*i)++;
                    break;
                case '~':
                    op = AO_WORD;
                    (*i)++;
                    break;
                default:
                    op = AO_EQ;
                    break;
                }
                if (*i >= n || s[*i] != '=') {
                    free(a);
                    snprintf(err, errsz, "expected = in an attribute selector");
                    return -1;
                }
                (*i)++;
                if (op == AO_HAS)
                    op = AO_EQ;
                if (*i < n && (s[*i] == '"' || s[*i] == '\'')) {
                    char q = s[(*i)++];
                    st = *i;
                    while (*i < n && s[*i] != q)
                        (*i)++;
                    b = sel_dup(s + st, *i - st);
                    if (*i < n)
                        (*i)++;
                } else {
                    st = *i;
                    while (*i < n && s[*i] != ']')
                        (*i)++;
                    b = sel_dup(s + st, *i - st);
                }
                if (!b) {
                    free(a);
                    return -1;
                }
            }
            if (*i >= n || s[*i] != ']') {
                free(a);
                free(b);
                snprintf(err, errsz, "unterminated attribute selector");
                return -1;
            }
            (*i)++;
            if (sel_add_part(sel, SP_ATTR, op, a, b) < 0)
                return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == ':') {
            (*i)++;
            if (n - *i >= 11 && strncasecmp(s + *i, "first-child", 11) == 0) {
                *i += 11;
                if (sel_add_part(sel, SP_FIRST, 0, NULL, NULL) < 0)
                    return -1;
            } else if (n - *i >= 10 && strncasecmp(s + *i, "last-child", 10) == 0) {
                *i += 10;
                if (sel_add_part(sel, SP_LAST, 0, NULL, NULL) < 0)
                    return -1;
            } else {
                snprintf(err, errsz, "the pseudo-classes here are :first-child "
                                     "and :last-child");
                return -1;
            }
            (*count)++;
            continue;
        }
        if (sel_ident_char((unsigned char)s[*i])) {
            if (sel_ident(s, n, i, &a) < 0)
                return -1;
            {
                size_t k, l = strlen(a);
                for (k = 0; k < l; k++)
                    a[k] = (char)ht_lower((unsigned char)a[k]);
            }
            if (sel_add_part(sel, SP_TAG, 0, a, NULL) < 0)
                return -1;
            (*count)++;
            continue;
        }
        break;
    }
    return 0;
}

sel_t* sel_compile(const char* s, size_t n, char* err, size_t errsz)
{
    sel_t* sel = (sel_t*)calloc(1, sizeof *sel);
    size_t i = 0;
    uint8_t comb = CB_DESC;
    int started = 0;

    if (!sel)
        return NULL;
    for (;;) {
        uint32_t first = sel->npart, count = 0;
        while (i < n && ht_space((unsigned char)s[i]))
            i++;
        if (i >= n)
            break;
        if (s[i] == ',') {
            if (!started) {
                snprintf(err, errsz, "an empty selector in the group");
                goto fail;
            }
            i++;
            comb = CB_DESC;
            started = 0;
            continue;
        }
        if (s[i] == '>') {
            if (!started) {
                snprintf(err, errsz, "a combinator with nothing before it");
                goto fail;
            }
            comb = CB_CHILD;
            i++;
            continue;
        }
        if (!started) {
            if (sel->ngroup == sel->cgroup) {
                uint32_t nc = sel->cgroup ? sel->cgroup * 2 : 4;
                uint32_t* np = (uint32_t*)realloc(sel->groups, nc * sizeof *np);
                if (!np)
                    goto fail;
                sel->groups = np;
                sel->cgroup = nc;
            }
            sel->groups[sel->ngroup++] = sel->nunit;
        }
        if (sel_compound(sel, s, n, &i, &count, err, errsz) < 0)
            goto fail;
        if (count == 0) {
            snprintf(err, errsz, "unexpected character '%c'", s[i]);
            goto fail;
        }
        if (sel_add_unit(sel, comb, first, count) < 0)
            goto fail;
        comb = CB_DESC;
        started = 1;
    }
    if (!started) {
        snprintf(err, errsz, "an empty selector matches nothing and is a typo");
        goto fail;
    }
    if (comb != CB_DESC) {
        snprintf(err, errsz, "a combinator with nothing after it");
        goto fail;
    }
    {
        uint32_t pi;
        for (pi = 0; pi < sel->npart; pi++)
            if (sel->parts[pi].kind == SP_FIRST || sel->parts[pi].kind == SP_LAST) {
                sel->pos_used = 1;
                break;
            }
    }
    return sel;
fail:
    sel_free(sel);
    return NULL;
}
