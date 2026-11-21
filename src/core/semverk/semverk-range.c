#include "semverk.h"

static int sv_comp_is_any(const Comparator* c)
{
    return c->op == OP_GTE && c->v.major == 0 && c->v.minor == 0 && c->v.patch == 0 && c->v.n_pre == 0;
}

static int set_test(const SemVer* v, const CompSet* set)
{
    int i;
    for (i = 0; i < set->n; i++)
        if (!sv_comp_is_any(&set->c[i]) && !semverk_cmp_test(v, &set->c[i]))
            return 0;
    if (v->n_pre > 0) {
        for (i = 0; i < set->n; i++) {
            const SemVer* cv = &set->c[i].v;
            if (cv->n_pre > 0 && cv->major == v->major && cv->minor == v->minor && cv->patch == v->patch)
                return 1;
        }
        return 0;
    }
    return 1;
}

int semverk_range_test(const SemVer* v, const Range* rg)
{
    int i;
    if (v->n_pre < 0)
        return 0;
    for (i = 0; i < rg->n; i++)
        if (set_test(v, &rg->s[i]))
            return 1;
    return 0;
}

static int sv_eq_intersects(const Comparator* eq, const Comparator* c)
{
    if (eq->v.n_pre > 0) {
        if (c->v.n_pre == 0)
            return 0;
        if (c->v.major != eq->v.major || c->v.minor != eq->v.minor || c->v.patch != eq->v.patch)
            return 0;
    }
    return semverk_cmp_test(&eq->v, c);
}

static int sv_comp_matches_nothing(const Comparator* c)
{
    if (c->op != OP_LT || c->v.major != 0 || c->v.minor != 0 || c->v.patch != 0)
        return 0;
    return c->v.n_pre == 0 || (c->v.n_pre == 1 && c->v.pre[0].numeric && c->v.pre[0].len == 1 && c->v.pre[0].s[0] == '0');
}

static int sv_comp_intersects(const Comparator* a, const Comparator* b)
{
    int a_up, a_dn, b_up, b_dn, a_inc, b_inc, sc;

    if (sv_comp_is_any(a) || sv_comp_is_any(b))
        return 1;
    if (sv_comp_matches_nothing(a) || sv_comp_matches_nothing(b))
        return 0;

    if (a->op == OP_EQ && b->op != OP_EQ)
        return sv_eq_intersects(a, b);
    if (b->op == OP_EQ && a->op != OP_EQ)
        return sv_eq_intersects(b, a);

    a_up = a->op == OP_GT || a->op == OP_GTE;
    a_dn = a->op == OP_LT || a->op == OP_LTE;
    b_up = b->op == OP_GT || b->op == OP_GTE;
    b_dn = b->op == OP_LT || b->op == OP_LTE;

    if (a_up && b_up)
        return 1;
    if (a_dn && b_dn)
        return 1;

    a_inc = a->op == OP_GTE || a->op == OP_LTE || a->op == OP_EQ;
    b_inc = b->op == OP_GTE || b->op == OP_LTE || b->op == OP_EQ;
    sc = semver_compare(&a->v, &b->v);
    if (sc == 0 && a_inc && b_inc)
        return 1;
    if (a_up && b_dn && sc < 0)
        return 1;
    if (a_dn && b_up && sc > 0)
        return 1;
    return 0;
}

static int sv_sets_intersect(const CompSet* x, const CompSet* y)
{
    int i, j;
    for (i = 0; i < x->n; i++)
        for (j = i + 1; j < x->n; j++)
            if (!sv_comp_intersects(&x->c[i], &x->c[j]))
                return 0;
    for (i = 0; i < y->n; i++)
        for (j = i + 1; j < y->n; j++)
            if (!sv_comp_intersects(&y->c[i], &y->c[j]))
                return 0;
    for (i = 0; i < x->n; i++)
        for (j = 0; j < y->n; j++)
            if (!sv_comp_intersects(&x->c[i], &y->c[j]))
                return 0;
    return 1;
}

int sv_range_intersects(const Range* ra, const Range* rb)
{
    int i, j;
    for (i = 0; i < ra->n; i++)
        for (j = 0; j < rb->n; j++)
            if (sv_sets_intersect(&ra->s[i], &rb->s[j]))
                return 1;
    return 0;
}

