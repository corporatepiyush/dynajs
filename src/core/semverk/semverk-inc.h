#ifndef DYN_SEMVERK_SEMVERK_INC_H
#define DYN_SEMVERK_SEMVERK_INC_H

#include "semverk.h"

static inline int semverk_u64toa(uint64_t v, char* buf)
{
    char t[20];
    int n = 0, i;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    for (i = 0; i < n; i++)
        buf[i] = t[n - 1 - i];
    return n;
}

static inline int inc_cmp_id0(const IncId* a, const char* id, size_t idn)
{
    int id_num = idn > 0;
    size_t k, m;
    int c;
    for (k = 0; k < idn; k++)
        if (!sv_is_digit(id[k])) {
            id_num = 0;
            break;
        }
    if (a->is_num && id_num) {
        char tmp[24];
        int tl = semverk_u64toa(a->num, tmp);
        return sv_cmp_num(tmp, (size_t)tl, id, idn);
    }
    if (a->is_num)
        return -1;
    if (id_num)
        return 1;
    m = a->slen < idn ? a->slen : idn;
    c = memcmp(a->str, id, m);
    if (c)
        return c < 0 ? -1 : 1;
    if (a->slen != idn)
        return a->slen < idn ? -1 : 1;
    return 0;
}

static inline void inc_pre_step(IncVer* v, const char* id, size_t idn)
{
    if (v->n_pre == 0) {
        v->pre[0].is_num = 1;
        v->pre[0].num = 0;
        v->n_pre = 1;
    } else {
        int i = v->n_pre, found = 0;
        while (--i >= 0) {
            if (v->pre[i].is_num) {
                v->pre[i].num++;
                found = 1;
                break;
            }
        }
        if (!found && v->n_pre < (int)(sizeof(v->pre) / sizeof(v->pre[0]))) {
            v->pre[v->n_pre].is_num = 1;
            v->pre[v->n_pre].num = 0;
            v->n_pre++;
        }
    }
    if (id) {
        int replace = 1;
        if (inc_cmp_id0(&v->pre[0], id, idn) == 0 && v->n_pre >= 2 && v->pre[1].is_num)
            replace = 0;
        if (replace) {
            v->pre[0].is_num = 0;
            v->pre[0].str = id;
            v->pre[0].slen = idn;
            v->pre[1].is_num = 1;
            v->pre[1].num = 0;
            v->n_pre = 2;
        }
    }
}

static inline void semverk_inc_apply(IncVer* v, int rel, const char* id, size_t idn)
{
    switch (rel) {
    case REL_MAJOR:
        if (v->minor != 0 || v->patch != 0 || v->n_pre == 0)
            v->major++;
        v->minor = 0;
        v->patch = 0;
        v->n_pre = 0;
        break;
    case REL_MINOR:
        if (v->patch != 0 || v->n_pre == 0)
            v->minor++;
        v->patch = 0;
        v->n_pre = 0;
        break;
    case REL_PATCH:
        if (v->n_pre == 0)
            v->patch++;
        v->n_pre = 0;
        break;
    case REL_PREMAJOR:
        v->n_pre = 0;
        v->patch = 0;
        v->minor = 0;
        v->major++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREMINOR:
        v->n_pre = 0;
        v->patch = 0;
        v->minor++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PREPATCH:
        v->n_pre = 0;
        v->patch++;
        inc_pre_step(v, id, idn);
        break;
    case REL_PRERELEASE:
        if (v->n_pre == 0)
            v->patch++;
        inc_pre_step(v, id, idn);
        break;
    case REL_RELEASE:
        v->n_pre = 0;
        break;
    }
}

static inline int semverk_inc_release_id(const char* s, size_t n)
{
    static const struct {
        const char* name;
        int id;
    } m[] = {
        { "major", REL_MAJOR },
        { "minor", REL_MINOR },
        { "patch", REL_PATCH },
        { "premajor", REL_PREMAJOR },
        { "preminor", REL_PREMINOR },
        { "prepatch", REL_PREPATCH },
        { "prerelease", REL_PRERELEASE },
        { "release", REL_RELEASE },
    };
    size_t i;
    for (i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (strlen(m[i].name) == n && memcmp(m[i].name, s, n) == 0)
            return m[i].id;
    return -1;
}

#endif
