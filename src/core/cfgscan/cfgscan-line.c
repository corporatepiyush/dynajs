#include "cfgscan.h"

int dyn_next_line(dyn_cur_t* c, size_t* b, size_t* e)
{
    size_t j;
    if (c->i >= c->n)
        return 0;
    *b = c->i;
    for (j = c->i; j < c->n && c->s[j] != '\n'; j++)
        ;
    *e = j;
    if (*e > *b && c->s[*e - 1] == '\r')
        (*e)--;
    c->i = (j < c->n) ? j + 1 : j;
    return 1;
}

int dyn_frontmatter_find(const char* src, size_t n, size_t* pdstart, size_t* pdend, size_t* pbstart, const char** plang)
{
    size_t lb, le, dstart, dend = 0, bstart;
    dyn_cur_t cur;
    const char* lang = NULL;
    char fence;
    int closed = 0;

    cur.s = src;
    cur.n = n;
    cur.i = 0;
    if (!dyn_next_line(&cur, &lb, &le))
        return 0;
    {
        size_t b = lb, e = le;
        dyn_trim(src, &b, &e);
        if (e - b != 3)
            return 0;
        fence = src[b];
        if (src[b + 1] != fence || src[b + 2] != fence)
            return 0;
        if (fence == '-')
            lang = "yaml";
        else if (fence == '+')
            lang = "toml";
        else if (fence == ';')
            lang = "json";
        else
            return 0;
    }
    dstart = cur.i;
    while (dyn_next_line(&cur, &lb, &le)) {
        size_t b = lb, e = le;
        dyn_trim(src, &b, &e);
        if (e - b == 3 && src[b] == fence && src[b + 1] == fence
            && src[b + 2] == fence) {
            dend = lb;
            closed = 1;
            break;
        }
    }
    if (!closed)
        return 0;
    bstart = cur.i;
    if (dend > dstart && src[dend - 1] == '\n')
        dend--;
    if (dend > dstart && src[dend - 1] == '\r')
        dend--;
    *pdstart = dstart;
    *pdend = dend;
    *pbstart = bstart;
    *plang = lang;
    return 1;
}
