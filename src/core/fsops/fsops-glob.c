#include "fsops.h"

#define DYN_FS_GLOB_MAX_DEPTH 512

int dyn_glob_match(const char* p, const char* s)
{
    const char *star_p = NULL, *star_s = NULL;

    for (;;) {
        unsigned char pc = (unsigned char)*p;

        if (pc == '*') {
            while (*p == '*')
                p++;
            if (*p == '\0')
                return 1;
            star_p = p;
            star_s = s;
            continue;
        }
        if (pc == '\0') {
            if (*s == '\0')
                return 1;
            goto backtrack;
        }
        if (*s == '\0')
            goto backtrack;

        if (pc == '?') {
            p++;
            s++;
            continue;
        }
        if (pc == '[') {
            const char* q = p + 1;
            const char* start;
            int negate = 0, matched = 0;
            unsigned char c = (unsigned char)*s;
            if (*q == '!' || *q == '^') {
                negate = 1;
                q++;
            }
            start = q;
            while (*q && !(*q == ']' && q != start)) {
                if (q[0] && q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char lo = (unsigned char)q[0];
                    unsigned char hi = (unsigned char)q[2];
                    if (lo <= c && c <= hi)
                        matched = 1;
                    q += 3;
                } else {
                    if ((unsigned char)*q == c)
                        matched = 1;
                    q++;
                }
            }
            if (*q != ']') {
                if (c != '[')
                    goto backtrack;
                p++;
                s++;
                continue;
            }
            q++;
            if (matched == negate)
                goto backtrack;
            p = q;
            s++;
            continue;
        }
        if (pc == (unsigned char)*s) {
            p++;
            s++;
            continue;
        }

    backtrack:
        if (!star_p)
            return 0;
        if (*star_s == '\0')
            return 0;
        star_s++;
        p = star_p;
        s = star_s;
    }
}

static int dyn_glob_match_name(const char* pat, const char* name)
{
    if (name[0] == '.' && pat[0] != '.')
        return 0;
    return dyn_glob_match(pat, name);
}

static void dyn_glob_res_push(dyn_glob_res* r, const char* s, size_t len)
{
    char* dup;
    if (r->oom)
        return;
    if (r->count == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 32;
        char** ni = (char**)realloc(r->items, nc * sizeof(*ni));
        if (!ni) {
            r->oom = 1;
            return;
        }
        r->items = ni;
        r->cap = nc;
    }
    dup = (char*)malloc(len + 1);
    if (!dup) {
        r->oom = 1;
        return;
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    r->items[r->count++] = dup;
}

void dyn_glob_res_free(dyn_glob_res* r)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        free(r->items[i]);
    free(r->items);
}

static void dyn_glob_emit(dyn_glob_res* res, const char* rel, int is_abs)
{
    if (is_abs) {
        char* disp = dyn_join("/", rel);
        if (!disp) {
            res->oom = 1;
            return;
        }
        dyn_glob_res_push(res, disp, strlen(disp));
        free(disp);
    } else if (rel[0] == '\0') {
        dyn_glob_res_push(res, ".", 1);
    } else {
        dyn_glob_res_push(res, rel, strlen(rel));
    }
}

static char* dyn_glob_fsdir(const char* base, const char* rel)
{
    if (rel[0] == '\0')
        return dyn_join(base, "");
    return dyn_join(base, rel);
}

void dyn_glob_walk(dyn_glob_res* res, const char* base, char** segs,
    int nseg, int si, const char* rel, int is_abs,
    int depth)
{
    const char* seg;
    char* fsdir;

    if (res->oom || depth > DYN_FS_GLOB_MAX_DEPTH)
        return;
    if (si == nseg) {
        dyn_glob_emit(res, rel, is_abs);
        return;
    }
    seg = segs[si];

    if (strcmp(seg, "**") == 0) {
        int is_last = (si == nseg - 1);
        DIR* d;
        struct dirent* e;

        if (is_last) {
            if (rel[0] != '\0')
                dyn_glob_emit(res, rel, is_abs);
        } else {
            dyn_glob_walk(res, base, segs, nseg, si + 1, rel, is_abs, depth);
        }

        fsdir = dyn_glob_fsdir(base, rel);
        if (!fsdir) {
            res->oom = 1;
            return;
        }
        d = opendir(fsdir);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char *childrel, *childfs;
                struct stat st;
                if (dyn_is_dot_or_dotdot(e->d_name) || e->d_name[0] == '.')
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) {
                    res->oom = 1;
                    break;
                }
                childfs = dyn_join(fsdir, e->d_name);
                if (!childfs) {
                    free(childrel);
                    res->oom = 1;
                    break;
                }
                if (lstat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                    dyn_glob_walk(res, base, segs, nseg, si, childrel, is_abs,
                        depth + 1);
                else if (is_last)
                    dyn_glob_emit(res, childrel, is_abs);
                free(childfs);
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
        free(fsdir);
        return;
    }

    if (!dyn_glob_has_wildcard(seg)) {
        char* childrel = (rel[0] == '\0') ? dyn_join("", seg)
                                          : dyn_join(rel, seg);
        char* childfs;
        struct stat st;
        if (!childrel) {
            res->oom = 1;
            return;
        }
        childfs = dyn_glob_fsdir(base, childrel);
        if (!childfs) {
            free(childrel);
            res->oom = 1;
            return;
        }
        if (si == nseg - 1) {
            if (lstat(childfs, &st) == 0)
                dyn_glob_emit(res, childrel, is_abs);
        } else {
            if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                dyn_glob_walk(res, base, segs, nseg, si + 1, childrel, is_abs,
                    depth + 1);
        }
        free(childfs);
        free(childrel);
        return;
    }

    fsdir = dyn_glob_fsdir(base, rel);
    if (!fsdir) {
        res->oom = 1;
        return;
    }
    {
        DIR* d = opendir(fsdir);
        int is_last = (si == nseg - 1);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                char* childrel;
                if (dyn_is_dot_or_dotdot(e->d_name))
                    continue;
                if (!dyn_glob_match_name(seg, e->d_name))
                    continue;
                childrel = (rel[0] == '\0') ? dyn_join("", e->d_name)
                                            : dyn_join(rel, e->d_name);
                if (!childrel) {
                    res->oom = 1;
                    break;
                }
                if (is_last) {
                    dyn_glob_emit(res, childrel, is_abs);
                } else {
                    char* childfs = dyn_join(fsdir, e->d_name);
                    struct stat st;
                    if (!childfs) {
                        free(childrel);
                        res->oom = 1;
                        break;
                    }
                    if (stat(childfs, &st) == 0 && S_ISDIR(st.st_mode))
                        dyn_glob_walk(res, base, segs, nseg, si + 1, childrel,
                            is_abs, depth + 1);
                    free(childfs);
                }
                free(childrel);
                if (res->oom)
                    break;
            }
            closedir(d);
        }
    }
    free(fsdir);
}

int dyn_glob_str_cmp(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static size_t dyn_glob_bracket_end(const char* p, size_t i)
{
    size_t q = i + 1;
    if (p[q] == '!' || p[q] == '^')
        q++;
    if (p[q] == ']')
        q++;
    while (p[q] && p[q] != ']')
        q++;
    return p[q] == ']' ? q : 0;
}

static long dyn_glob_brace_close(const char* p, size_t i, int* comma)
{
    size_t q = i + 1;
    int depth = 1;
    *comma = 0;
    while (p[q]) {
        char c = p[q];
        if (c == '[') {
            size_t e = dyn_glob_bracket_end(p, q);
            if (e) {
                q = e + 1;
                continue;
            }
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0)
                return (long)q;
        } else if (c == ',' && depth == 1) {
            *comma = 1;
        }
        q++;
    }
    return -1;
}

static int dyn_glob_alts_push(dyn_glob_alts* a, char* s)
{
    if (a->n >= DYN_GLOB_MAX_ALTS) {
        free(s);
        return -2;
    }
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 4;
        char** g = (char**)realloc(a->v, nc * sizeof(*g));
        if (!g) {
            free(s);
            return -1;
        }
        a->v = g;
        a->cap = nc;
    }
    a->v[a->n++] = s;
    return 0;
}

static int dyn_glob_expand_from(const char* p, size_t from, dyn_glob_alts* out)
{
    size_t len = strlen(p), i;
    char* s;
    for (i = from; i < len; i++) {
        long j;
        int comma;
        if (p[i] == '[') {
            size_t e = dyn_glob_bracket_end(p, i);
            if (e)
                i = e;
            continue;
        }
        if (p[i] != '{')
            continue;
        j = dyn_glob_brace_close(p, i, &comma);
        if (j < 0 || !comma)
            continue;
        {
            size_t a = i + 1, q = a, suf = (size_t)j + 1, slen = len - suf;
            int depth = 0, rc;
            for (;;) {
                char c = p[q];
                if (c == '[' && q < (size_t)j) {
                    size_t e = dyn_glob_bracket_end(p, q);
                    if (e) {
                        q = e + 1;
                        continue;
                    }
                }
                if (q == (size_t)j || (c == ',' && depth == 0)) {
                    size_t alen = q - a;
                    s = (char*)malloc(i + alen + slen + 1);
                    if (!s)
                        return -1;
                    memcpy(s, p, i);
                    memcpy(s + i, p + a, alen);
                    memcpy(s + i + alen, p + suf, slen + 1);
                    rc = dyn_glob_expand_from(s, i, out);
                    free(s);
                    if (rc)
                        return rc;
                    if (q == (size_t)j)
                        return 0;
                    a = q + 1;
                } else if (c == '{') {
                    depth++;
                } else if (c == '}') {
                    depth--;
                }
                q++;
            }
        }
    }
    s = strdup(p);
    if (!s)
        return -1;
    return dyn_glob_alts_push(out, s);
}

int dyn_glob_expand(const char* pat, dyn_glob_alts* out)
{
    int rc;
    out->v = NULL;
    out->n = out->cap = 0;
    rc = dyn_glob_expand_from(pat, 0, out);
    if (rc)
        dyn_glob_alts_free(out);
    return rc;
}

void dyn_glob_alts_free(dyn_glob_alts* a)
{
    size_t i;
    for (i = 0; i < a->n; i++)
        free(a->v[i]);
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

int dyn_glob_match_alts(const dyn_glob_alts* a, const char* s)
{
    size_t i;
    for (i = 0; i < a->n; i++)
        if (dyn_glob_match(a->v[i], s))
            return 1;
    return 0;
}
