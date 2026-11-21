#include "semverk.h"

static const char SV_ZERO[] = "0";

int sv_cmp_num(const char* a, size_t al, const char* b, size_t bl)
{
    int c;
    while (al > 1 && *a == '0') {
        a++;
        al--;
    }
    while (bl > 1 && *b == '0') {
        b++;
        bl--;
    }
    if (al != bl)
        return al < bl ? -1 : 1;
    c = memcmp(a, b, al);
    return (c > 0) - (c < 0);
}

static int sv_cmp_id(const SemVerId* a, const SemVerId* b)
{
    size_t m;
    int c;
    if (a->numeric && b->numeric)
        return sv_cmp_num(a->s, a->len, b->s, b->len);
    if (a->numeric)
        return -1;
    if (b->numeric)
        return 1;
    m = a->len < b->len ? a->len : b->len;
    c = memcmp(a->s, b->s, m);
    if (c)
        return c < 0 ? -1 : 1;
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return 0;
}

int sv_cmp_pre(const SemVerId* a, int an, const SemVerId* b, int bn)
{
    int i, m = an < bn ? an : bn;
    for (i = 0; i < m; i++) {
        int c = sv_cmp_id(&a[i], &b[i]);
        if (c)
            return c;
    }
    if (an != bn)
        return an < bn ? -1 : 1;
    return 0;
}

int semver_compare(const SemVer* a, const SemVer* b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;
    if (a->n_pre == 0 && b->n_pre == 0)
        return 0;
    if (a->n_pre == 0)
        return 1;
    if (b->n_pre == 0)
        return -1;
    return sv_cmp_pre(a->pre, a->n_pre, b->pre, b->n_pre);
}

int sv_add_cmp(CompSet* set, int op, uint64_t M, uint64_t m, uint64_t p,
    const SemVerId* pre, int npre, int add_zero)
{
    Comparator* c;
    if (set->n >= SV_MAX_CMPS)
        return -1;
    if (set->n == set->cap) {
        int nc = set->cap ? set->cap * 2 : 4;
        Comparator* g;
        if (nc > SV_MAX_CMPS)
            nc = SV_MAX_CMPS;
        g = (Comparator*)realloc(set->c, (size_t)nc * sizeof(*g));
        if (!g)
            return -1;
        set->c = g;
        set->cap = nc;
    }
    c = &set->c[set->n++];
    c->v.n_pre = 0;
    c->v.n_build = 0;
    c->op = op;
    c->v.major = M;
    c->v.minor = m;
    c->v.patch = p;
    if (add_zero) {
        c->v.pre[0].s = SV_ZERO;
        c->v.pre[0].len = 1;
        c->v.pre[0].numeric = 1;
        c->v.n_pre = 1;
    } else if (npre > 0) {
        int i;
        if (npre > SEMVER_MAX_IDS)
            return -1;
        for (i = 0; i < npre; i++)
            c->v.pre[i] = pre[i];
        c->v.n_pre = npre;
    }
    return 0;
}

int semverk_cmp_test(const SemVer* v, const Comparator* c)
{
    int r = semver_compare(v, &c->v);
    switch (c->op) {
    case OP_LT:
        return r < 0;
    case OP_LTE:
        return r <= 0;
    case OP_GT:
        return r > 0;
    case OP_GTE:
        return r >= 0;
    case OP_EQ:
        return r == 0;
    }
    return 0;
}

void semverk_sort_indices(const SemVer* svs, uint32_t* idx, uint32_t* tmp,
    uint32_t n)
{
    uint32_t width;
    for (width = 1; width < n; width *= 2) {
        uint32_t i;
        for (i = 0; i < n; i += 2 * width) {
            uint32_t lo = i;
            uint32_t mid = (i + width < n) ? i + width : n;
            uint32_t hi = (i + 2 * width < n) ? i + 2 * width : n;
            uint32_t l = lo, r = mid, k = lo;
            while (l < mid && r < hi) {
                if (semver_compare(&svs[idx[l]], &svs[idx[r]]) <= 0)
                    tmp[k++] = idx[l++];
                else
                    tmp[k++] = idx[r++];
            }
            while (l < mid)
                tmp[k++] = idx[l++];
            while (r < hi)
                tmp[k++] = idx[r++];
        }
        memcpy(idx, tmp, (size_t)n * sizeof(uint32_t));
    }
}

