#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dd-agg.h"

enum {
    S_MATCH = 1,
    S_PROJECT,
    S_ADDFIELDS,
    S_UNSET,
    S_UNWIND,
    S_GROUP,
    S_SORT,
    S_LIMIT,
    S_SKIP,
    S_COUNT,
    S_LOOKUP,
    S_FACET,
    S_REPLACEROOT,
};

enum { A_SUM = 1,
    A_AVG,
    A_MIN,
    A_MAX,
    A_FIRST,
    A_LAST,
    A_PUSH,
    A_ADDTOSET };

typedef struct rchunk {
    struct rchunk* next;
    size_t used, cap;
    uint8_t data[];
} rchunk;

struct dd_rows {
    const uint8_t** p;
    uint32_t n, cap;
    rchunk* chunks;
};

typedef struct ag_node {
    char* name;
    uint32_t nlen;
    struct ag_node* kids;
    uint32_t nkids;
    int leaf;
} ag_node;

typedef struct {
    char* name;
    uint32_t nlen;
    int op;
    dd_xn* arg;
} ag_acc;

typedef struct ag_stage {
    int kind;
    dd_filter f;
    int has_f;
    dd_proj proj;
    int has_proj;
    ag_node tree;
    dd_xn** ex;
    int nex;
    int id_mode;
    dd_xn* expr;
    dd_path path;
    int has_path;
    int has_index;
    int preserve;
    ag_acc* acc;
    int nacc;
    dd_sort sort;
    int has_sort;
    uint64_t n;
    char* name;
    size_t name_n;
    char* from;
    size_t from_n;
    dd_path local, foreign;
    int has_lf;
    struct dd_agg** facets;
    char** fnames;
    uint32_t* fnlens;
    int nfacets;
    uint8_t* bspec;
    dyn_bson_val* bounds;
    int nbounds;
    int bucket;
    int has_def;
    dyn_bson_val defv;
    uint8_t* spec;
} ag_stage;

struct dd_agg {
    ag_stage* st;
    int n;
    int has_regex;
    char** lnames;
    size_t* llens;
    int nl;
    int prefix;
    uint64_t pskip, plimit;
    const uint8_t* lmatch;
    const uint8_t* lsort;
};

typedef struct {
    const dd_agg_source* srcs;
    int nsrc;
    dd_err* e;
    dd_xctx x;
    dyn_bson_w w, w2;
    dyn_ks_buf kb;
} ag_run;

static dd_rows* rows_new(void)
{
    return calloc(1, sizeof(dd_rows));
}

void dd_rows_free(dd_rows* r)
{
    rchunk* c;
    if (!r)
        return;
    c = r->chunks;
    while (c) {
        rchunk* nx = c->next;
        free(c);
        c = nx;
    }
    free(r->p);
    free(r);
}

uint32_t dd_rows_count(const dd_rows* r)
{
    return r ? r->n : 0;
}

const uint8_t* dd_rows_at(const dd_rows* r, uint32_t i)
{
    return r && i < r->n ? r->p[i] : NULL;
}

static int rows_push(dd_rows* r, const uint8_t* d)
{
    if (r->n == r->cap) {
        uint32_t nc = r->cap ? r->cap * 2 : 64;
        const uint8_t** np = realloc(r->p, (size_t)nc * sizeof *np);
        if (!np)
            return -1;
        r->p = np;
        r->cap = nc;
    }
    r->p[r->n++] = d;
    return 0;
}

static int rows_push_copy(dd_rows* r, const uint8_t* p, size_t n)
{
    rchunk* c = r->chunks;
    uint8_t* d;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > (256u << 10) ? n : (256u << 10);
        c = malloc(sizeof *c + cap);
        if (!c)
            return -1;
        c->cap = cap;
        c->used = 0;
        c->next = r->chunks;
        r->chunks = c;
    }
    d = c->data + c->used;
    memcpy(d, p, n);
    c->used += n;
    return rows_push(r, d);
}

static void rows_adopt(dd_rows* dst, dd_rows* src)
{
    rchunk* c = src->chunks;
    if (!c)
        return;
    while (c->next)
        c = c->next;
    c->next = dst->chunks;
    dst->chunks = src->chunks;
    src->chunks = NULL;
}

static void node_free(ag_node* n)
{
    uint32_t i;
    for (i = 0; i < n->nkids; i++)
        node_free(&n->kids[i]);
    free(n->kids);
    free(n->name);
}

static int path_ok(const char* s, size_t n)
{
    size_t i, start = 0;
    if (n == 0 || s[0] == '$')
        return 0;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            if (i == start)
                return 0;
            start = i + 1;
        }
    }
    return 1;
}

static int tree_add(ag_node* root, const char* s, size_t n, int leaf, const char* who, dd_err* e)
{
    ag_node* cur = root;
    size_t start = 0, i;
    if (!path_ok(s, n))
        return dd_fail(e, 40352, "%s: FieldPath '%.*s' must not be empty, start with '$' or contain an empty part", who, (int)n, s);
    for (i = 0; i <= n; i++) {
        uint32_t k;
        ag_node* nk;
        if (i < n && s[i] != '.')
            continue;
        if (cur->leaf >= 0 && cur != root)
            return dd_fail(e, 31250, "%s: path collision at %.*s", who, (int)n, s);
        for (k = 0; k < cur->nkids; k++)
            if (cur->kids[k].nlen == i - start && memcmp(cur->kids[k].name, s + start, i - start) == 0)
                break;
        if (k == cur->nkids) {
            nk = realloc(cur->kids, (size_t)(cur->nkids + 1) * sizeof *nk);
            if (!nk)
                return dd_fail(e, DD_E_OOM, "out of memory");
            cur->kids = nk;
            memset(&nk[k], 0, sizeof nk[k]);
            nk[k].leaf = -1;
            nk[k].name = malloc(i - start + 1);
            if (!nk[k].name)
                return dd_fail(e, DD_E_OOM, "out of memory");
            memcpy(nk[k].name, s + start, i - start);
            nk[k].name[i - start] = 0;
            nk[k].nlen = (uint32_t)(i - start);
            cur->nkids++;
        } else if (i == n || cur->kids[k].leaf >= 0) {
            return dd_fail(e, 31250, "%s: path collision at %.*s", who, (int)n, s);
        }
        cur = &cur->kids[k];
        start = i + 1;
    }
    cur->leaf = leaf;
    return 0;
}

static void write_tree(dyn_bson_w* w, const ag_node* n, const uint8_t* doc, const dd_xv* vals, const dd_xctx* c);

static void write_node_fresh(dyn_bson_w* w, const ag_node* k, const char* key, size_t klen, int idx, const dd_xv* vals,
    const dd_xctx* c)
{
    if (k->leaf >= 0) {
        dyn_bson_val v = dd_xv_val(c, &vals[k->leaf]);
        if (v.type == 0)
            return;
        if (idx >= 0) {
            dyn_bson_w_index_key(w, v.type, (uint32_t)idx);
            dyn_bson_w_bytes(w, v.val, v.vlen);
        } else {
            dyn_bson_w_value(w, key, klen, v);
        }
        return;
    }
    if (idx >= 0) {
        dyn_bson_w_index_key(w, DYN_BSON_DOC, (uint32_t)idx);
        dyn_bson_w_doc_begin(w);
    } else {
        dyn_bson_w_sub_begin(w, DYN_BSON_DOC, key, klen);
    }
    write_tree(w, k, NULL, vals, c);
    dyn_bson_w_doc_end(w);
}

static void write_tree(dyn_bson_w* w, const ag_node* n, const uint8_t* doc, const dd_xv* vals, const dd_xctx* c)
{
    uint8_t seen_small[64];
    uint8_t* seen = n->nkids <= 64 ? seen_small : calloc(n->nkids, 1);
    uint32_t k;
    if (!seen) {
        w->oom = 1;
        return;
    }
    memset(seen, 0, n->nkids <= 64 ? 64 : 0);
    if (doc) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
        while (dyn_bson_iter_next(&it) > 0) {
            dyn_bson_val v = dyn_bson_iter_val(&it);
            for (k = 0; k < n->nkids; k++)
                if (n->kids[k].nlen == it.klen && memcmp(n->kids[k].name, it.key, it.klen) == 0)
                    break;
            if (k == n->nkids) {
                dyn_bson_w_value(w, it.key, it.klen, v);
                continue;
            }
            seen[k] = 1;
            if (n->kids[k].leaf >= 0) {
                write_node_fresh(w, &n->kids[k], it.key, it.klen, -1, vals, c);
            } else if (v.type == DYN_BSON_DOC) {
                dyn_bson_w_sub_begin(w, DYN_BSON_DOC, it.key, it.klen);
                write_tree(w, &n->kids[k], v.val, vals, c);
                dyn_bson_w_doc_end(w);
            } else if (v.type == DYN_BSON_ARRAY) {
                dyn_bson_iter ai;
                uint32_t idx = 0;
                dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, it.key, it.klen);
                dyn_bson_iter_init(&ai, v.val, v.vlen);
                while (dyn_bson_iter_next(&ai) > 0) {
                    dyn_bson_val el = dyn_bson_iter_val(&ai);
                    if (el.type == DYN_BSON_DOC) {
                        dyn_bson_w_index_key(w, DYN_BSON_DOC, idx++);
                        dyn_bson_w_doc_begin(w);
                        write_tree(w, &n->kids[k], el.val, vals, c);
                        dyn_bson_w_doc_end(w);
                    } else {
                        write_node_fresh(w, &n->kids[k], NULL, 0, (int)idx++, vals, c);
                    }
                }
                dyn_bson_w_doc_end(w);
            } else {
                write_node_fresh(w, &n->kids[k], it.key, it.klen, -1, vals, c);
            }
        }
    }
    for (k = 0; k < n->nkids; k++)
        if (!seen[k])
            write_node_fresh(w, &n->kids[k], n->kids[k].name, n->kids[k].nlen, -1, vals, c);
    if (seen != seen_small)
        free(seen);
}

static void stage_free(ag_stage* s)
{
    int i;
    if (s->has_f)
        dd_filter_free(&s->f);
    if (s->has_proj)
        dd_proj_free(&s->proj);
    node_free(&s->tree);
    for (i = 0; i < s->nex; i++)
        dd_expr_free(s->ex[i]);
    free(s->ex);
    dd_expr_free(s->expr);
    if (s->has_path)
        dd_path_free(&s->path);
    for (i = 0; i < s->nacc; i++) {
        free(s->acc[i].name);
        dd_expr_free(s->acc[i].arg);
    }
    free(s->acc);
    if (s->has_sort)
        dd_sort_free(&s->sort);
    free(s->name);
    free(s->from);
    if (s->has_lf) {
        dd_path_free(&s->local);
        dd_path_free(&s->foreign);
    }
    for (i = 0; i < s->nfacets; i++) {
        dd_agg_free(s->facets[i]);
        free(s->fnames[i]);
    }
    free(s->facets);
    free(s->fnames);
    free(s->fnlens);
    free(s->bspec);
    free(s->bounds);
    free(s->spec);
}

void dd_agg_free(dd_agg* a)
{
    int i;
    if (!a)
        return;
    for (i = 0; i < a->n; i++)
        stage_free(&a->st[i]);
    free(a->st);
    for (i = 0; i < a->nl; i++)
        free(a->lnames[i]);
    free(a->lnames);
    free(a->llens);
    free(a);
}

static ag_stage* new_stage(dd_agg* a, int kind)
{
    ag_stage* ns = realloc(a->st, (size_t)(a->n + 1) * sizeof *ns);
    if (!ns)
        return NULL;
    a->st = ns;
    memset(&ns[a->n], 0, sizeof ns[a->n]);
    ns[a->n].kind = kind;
    ns[a->n].tree.leaf = -1;
    return &ns[a->n++];
}

static int add_expr(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    dd_xn* x = NULL;
    dd_xn** nx;
    if (dd_expr_compile(&x, v, e) < 0)
        return -1;
    nx = realloc(s->ex, (size_t)(s->nex + 1) * sizeof *nx);
    if (!nx) {
        dd_expr_free(x);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    s->ex = nx;
    s->ex[s->nex] = x;
    return s->nex++;
}

static int is_flag(dyn_bson_val v)
{
    return dyn_bson_is_number(v.type) || v.type == DYN_BSON_BOOL;
}

static int is_leaf_spec(dyn_bson_val v)
{
    dyn_bson_iter it;
    if (v.type != DYN_BSON_DOC)
        return 1;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    if (dyn_bson_iter_next(&it) <= 0)
        return 1;
    return it.klen > 0 && it.key[0] == '$';
}

typedef struct {
    ag_stage* s;
    dyn_bson_w* pure;
    int inc, exc, id_flag, computed;
    int allow_flags;
    const char* who;
    dd_err* e;
} flat_ctx;

static int flatten(flat_ctx* fc, const uint8_t* doc, const char* prefix, size_t plen)
{
    dyn_bson_iter it;
    char buf[1024];
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val v = dyn_bson_iter_val(&it);
        size_t n = plen ? plen + 1 + it.klen : it.klen;
        if (n >= sizeof buf)
            return dd_fail(fc->e, DD_E_BADVALUE, "%s: field path too long", fc->who);
        if (plen) {
            memcpy(buf, prefix, plen);
            buf[plen] = '.';
            memcpy(buf + plen + 1, it.key, it.klen);
        } else {
            memcpy(buf, it.key, it.klen);
        }
        buf[n] = 0;
        if (!path_ok(buf, n))
            return dd_fail(fc->e, 40352, "%s: FieldPath '%s' must not be empty, start with '$' or contain an empty part", fc->who, buf);
        if (fc->allow_flags && is_flag(v)) {
            int on = v.type == DYN_BSON_BOOL ? v.val[0] != 0 : dyn_bson_num_f64(v) != 0;
            if (n == 3 && memcmp(buf, "_id", 3) == 0)
                fc->id_flag = on ? 1 : 0;
            else if (on)
                fc->inc = 1;
            else
                fc->exc = 1;
            if (on || n != 3 || memcmp(buf, "_id", 3) != 0) {
                if (on)
                    dyn_bson_w_int32(fc->pure, buf, n, 1);
                else
                    dyn_bson_w_int32(fc->pure, buf, n, 0);
            }
            continue;
        }
        if (!is_leaf_spec(v)) {
            if (flatten(fc, v.val, buf, n) < 0)
                return -1;
            continue;
        }
        {
            int ix = add_expr(fc->s, v, fc->e);
            if (ix < 0 || tree_add(&fc->s->tree, buf, n, ix, fc->who, fc->e) < 0)
                return -1;
            fc->computed = 1;
        }
    }
    return 0;
}

static int compile_project(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    flat_ctx fc;
    dyn_bson_w pure;
    int rc = 0;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 15969, "$project specification must be an object");
    memset(&fc, 0, sizeof fc);
    fc.s = s;
    fc.pure = &pure;
    fc.id_flag = -1;
    fc.allow_flags = 1;
    fc.who = "$project";
    fc.e = e;
    dyn_bson_w_init(&pure);
    dyn_bson_w_doc_begin(&pure);
    if (flatten(&fc, v.val, NULL, 0) < 0) {
        dyn_bson_w_free(&pure);
        return -1;
    }
    if (fc.id_flag == 0)
        dyn_bson_w_int32(&pure, "_id", 3, 0);
    dyn_bson_w_doc_end(&pure);
    if (pure.oom) {
        dyn_bson_w_free(&pure);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    if (!fc.inc && !fc.exc && !fc.computed && fc.id_flag < 0) {
        dyn_bson_w_free(&pure);
        return dd_fail(e, 51272, "$project specification must have at least one field");
    }
    if ((fc.inc || fc.computed) && fc.exc) {
        dyn_bson_w_free(&pure);
        return dd_fail(e, 31254, "Invalid $project :: caused by :: Cannot do exclusion on a field in inclusion projection");
    }
    s->id_mode = fc.id_flag;
    if (fc.inc || fc.exc || (!fc.computed && fc.id_flag == 0)) {
        rc = dd_proj_parse(&s->proj, pure.p, e);
        if (rc == 0)
            s->has_proj = 1;
    }
    dyn_bson_w_free(&pure);
    return rc;
}

static int compile_addfields(ag_stage* s, dyn_bson_val v, const char* who, dd_err* e)
{
    flat_ctx fc;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 40272, "%s specification stage must be an object", who);
    memset(&fc, 0, sizeof fc);
    fc.s = s;
    fc.who = who;
    fc.e = e;
    return flatten(&fc, v.val, NULL, 0);
}

static int compile_unset(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    dyn_bson_w w;
    int n = 0, rc;
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    if (v.type == DYN_BSON_STRING) {
        size_t l = dyn_bson_rd32(v.val) - 1;
        if (!path_ok((const char*)v.val + 4, l)) {
            dyn_bson_w_free(&w);
            return dd_fail(e, 40352, "$unset: invalid field path");
        }
        dyn_bson_w_int32(&w, (const char*)v.val + 4, l, 0);
        n = 1;
    } else if (v.type == DYN_BSON_ARRAY) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            dyn_bson_val el = dyn_bson_iter_val(&it);
            size_t l;
            if (el.type != DYN_BSON_STRING) {
                dyn_bson_w_free(&w);
                return dd_fail(e, 31120, "$unset specification must be a string or an array containing only string values");
            }
            l = dyn_bson_rd32(el.val) - 1;
            if (!path_ok((const char*)el.val + 4, l)) {
                dyn_bson_w_free(&w);
                return dd_fail(e, 40352, "$unset: invalid field path");
            }
            dyn_bson_w_int32(&w, (const char*)el.val + 4, l, 0);
            n++;
        }
    }
    dyn_bson_w_doc_end(&w);
    if (n == 0) {
        dyn_bson_w_free(&w);
        return dd_fail(e, 31119, "$unset specification must be a string or an array with at least one field");
    }
    if (w.oom) {
        dyn_bson_w_free(&w);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    rc = dd_proj_parse(&s->proj, w.p, e);
    if (rc == 0)
        s->has_proj = 1;
    dyn_bson_w_free(&w);
    return rc;
}

static int str_val(dyn_bson_val v, const char** s, size_t* n)
{
    if (v.type != DYN_BSON_STRING)
        return 0;
    *s = (const char*)v.val + 4;
    *n = dyn_bson_rd32(v.val) - 1;
    return 1;
}

static int dupn(char** out, const char* s, size_t n)
{
    *out = malloc(n + 1);
    if (!*out)
        return -1;
    memcpy(*out, s, n);
    (*out)[n] = 0;
    return 0;
}

static int compile_unwind(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    const char* p;
    size_t pn;
    dyn_bson_val pv = v, iv, nv;
    if (v.type == DYN_BSON_DOC) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            if (!((it.klen == 4 && memcmp(it.key, "path", 4) == 0) || (it.klen == 17 && memcmp(it.key, "includeArrayIndex", 17) == 0)
                    || (it.klen == 26 && memcmp(it.key, "preserveNullAndEmptyArrays", 26) == 0)))
                return dd_fail(e, 28811, "unrecognized option to $unwind stage: %.*s", (int)it.klen, it.key);
        }
        if (!dyn_bson_find(v.val, "path", 4, &pv))
            return dd_fail(e, 28812, "no path specified to $unwind stage");
        if (dyn_bson_find(v.val, "preserveNullAndEmptyArrays", 26, &nv)) {
            if (nv.type != DYN_BSON_BOOL)
                return dd_fail(e, 28809, "expected a boolean for the preserveNullAndEmptyArrays option to $unwind stage");
            s->preserve = nv.val[0] != 0;
        }
        if (dyn_bson_find(v.val, "includeArrayIndex", 17, &iv)) {
            const char* is;
            size_t in;
            if (!str_val(iv, &is, &in) || in == 0 || is[0] == '$' || !path_ok(is, in))
                return dd_fail(e, 28810, "includeArrayIndex must be a non-empty string field path not starting with '$'");
            s->has_index = 1;
            if (dupn(&s->name, is, in) < 0)
                return dd_fail(e, DD_E_OOM, "out of memory");
            s->name_n = in;
        }
    } else if (v.type != DYN_BSON_STRING) {
        return dd_fail(e, 15981, "expected either a string or an object as specification for $unwind stage");
    }
    if (!str_val(pv, &p, &pn))
        return dd_fail(e, 28808, "expected a string as the path for $unwind stage");
    if (pn < 2 || p[0] != '$' || !path_ok(p + 1, pn - 1))
        return dd_fail(e, 28818, "path option to $unwind stage should be prefixed with a '$': %.*s", (int)pn, p);
    if (dd_path_parse(&s->path, p + 1, pn - 1, e) < 0)
        return -1;
    s->has_path = 1;
    if (tree_add(&s->tree, p + 1, pn - 1, 0, "$unwind", e) < 0)
        return -1;
    if (s->has_index && tree_add(&s->tree, s->name, s->name_n, 1, "$unwind", e) < 0)
        return -1;
    return 0;
}

static const struct {
    const char* name;
    int op;
} ACCS[] = {
    { "$sum", A_SUM },
    { "$avg", A_AVG },
    { "$min", A_MIN },
    { "$max", A_MAX },
    { "$first", A_FIRST },
    { "$last", A_LAST },
    { "$push", A_PUSH },
    { "$addToSet", A_ADDTOSET },
    { "$count", A_SUM },
};

static int compile_accs(ag_stage* s, const uint8_t* doc, int skip_id, const char* who, dd_err* e)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val v = dyn_bson_iter_val(&it), arg;
        dyn_bson_iter ai;
        ag_acc* na;
        size_t i;
        int k;
        if (skip_id && it.klen == 3 && memcmp(it.key, "_id", 3) == 0)
            continue;
        if (it.klen == 0 || memchr(it.key, '.', it.klen) || it.key[0] == '$')
            return dd_fail(e, 40235, "%s: the field name '%.*s' cannot be an operator name or contain '.'", who, (int)it.klen, it.key);
        if (v.type != DYN_BSON_DOC)
            return dd_fail(e, 40234, "%s: the field '%.*s' must be an accumulator object", who, (int)it.klen, it.key);
        dyn_bson_iter_init(&ai, v.val, v.vlen);
        if (dyn_bson_iter_next(&ai) <= 0)
            return dd_fail(e, 40234, "%s: the field '%.*s' must be an accumulator object", who, (int)it.klen, it.key);
        for (i = 0; i < sizeof ACCS / sizeof ACCS[0]; i++)
            if (strlen(ACCS[i].name) == ai.klen && memcmp(ACCS[i].name, ai.key, ai.klen) == 0)
                break;
        if (i == sizeof ACCS / sizeof ACCS[0])
            return dd_fail(e, 15952, "unknown group operator '%.*s'", (int)ai.klen, ai.key);
        arg = dyn_bson_iter_val(&ai);
        if (dyn_bson_iter_next(&ai) > 0)
            return dd_fail(e, 40238, "%s: the field '%.*s' must specify one accumulator", who, (int)it.klen, it.key);
        for (k = 0; k < s->nacc; k++)
            if (s->acc[k].nlen == it.klen && memcmp(s->acc[k].name, it.key, it.klen) == 0)
                return dd_fail(e, 16406, "duplicate field name specified in object literal: %.*s", (int)it.klen, it.key);
        na = realloc(s->acc, (size_t)(s->nacc + 1) * sizeof *na);
        if (!na)
            return dd_fail(e, DD_E_OOM, "out of memory");
        s->acc = na;
        memset(&na[s->nacc], 0, sizeof na[s->nacc]);
        if (dupn(&na[s->nacc].name, it.key, it.klen) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        na[s->nacc].nlen = (uint32_t)it.klen;
        na[s->nacc].op = ACCS[i].op;
        s->nacc++;
        if (ACCS[i].name[1] == 'c') {
            static const uint8_t one[4] = { 1, 0, 0, 0 };
            dyn_bson_val ov = { DYN_BSON_INT32, one, 4 };
            dyn_bson_iter ci;
            dyn_bson_iter_init(&ci, arg.val, arg.vlen);
            if (arg.type != DYN_BSON_DOC || dyn_bson_iter_next(&ci) > 0)
                return dd_fail(e, 40181, "$count takes no arguments, i.e. $count: {}");
            arg = ov;
        } else if (arg.type == DYN_BSON_ARRAY) {
            return dd_fail(e, 40237, "The %s accumulator is a unary operator", ACCS[i].name);
        }
        if (dd_expr_compile(&na[s->nacc - 1].arg, arg, e) < 0)
            return -1;
    }
    return 0;
}

static int compile_group(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    dyn_bson_val id;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 15947, "a group's fields must be specified in an object");
    if (!dyn_bson_find(v.val, "_id", 3, &id))
        return dd_fail(e, 15955, "a group specification must include an _id");
    if (dd_expr_compile(&s->expr, id, e) < 0)
        return -1;
    return compile_accs(s, v.val, 1, "$group", e);
}

static int compile_bucket(ag_stage* s, dyn_bson_val v, dd_err* e)
{
    dyn_bson_val gb, bd, def, out;
    dyn_bson_iter it;
    int i;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 40201, "Argument to $bucket stage must be an object");
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0)
        if (!((it.klen == 7 && memcmp(it.key, "groupBy", 7) == 0) || (it.klen == 10 && memcmp(it.key, "boundaries", 10) == 0)
                || (it.klen == 7 && memcmp(it.key, "default", 7) == 0) || (it.klen == 6 && memcmp(it.key, "output", 6) == 0)))
            return dd_fail(e, 40197, "Unrecognized option to $bucket: %.*s", (int)it.klen, it.key);
    if (!dyn_bson_find(v.val, "groupBy", 7, &gb) || !dyn_bson_find(v.val, "boundaries", 10, &bd))
        return dd_fail(e, 40198, "$bucket requires 'groupBy' and 'boundaries' to be specified");
    if (bd.type != DYN_BSON_ARRAY)
        return dd_fail(e, 40200, "The $bucket 'boundaries' field must be an array");
    s->bucket = 1;
    s->bspec = malloc(v.vlen);
    if (!s->bspec)
        return dd_fail(e, DD_E_OOM, "out of memory");
    memcpy(s->bspec, v.val, v.vlen);
    dyn_bson_find(s->bspec, "boundaries", 10, &bd);
    dyn_bson_iter_init(&it, bd.val, bd.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val* nb = realloc(s->bounds, (size_t)(s->nbounds + 1) * sizeof *nb);
        if (!nb)
            return dd_fail(e, DD_E_OOM, "out of memory");
        s->bounds = nb;
        s->bounds[s->nbounds++] = dyn_bson_iter_val(&it);
    }
    if (s->nbounds < 2)
        return dd_fail(e, 40192, "The $bucket 'boundaries' field must have at least 2 values");
    for (i = 1; i < s->nbounds; i++) {
        if (dyn_bson_canon_rank(s->bounds[i].type) != dyn_bson_canon_rank(s->bounds[0].type))
            return dd_fail(e, 40193, "All values in the $bucket 'boundaries' field must have the same type");
        if (dyn_bson_cmp(s->bounds[i - 1], s->bounds[i]) >= 0)
            return dd_fail(e, 40194, "The $bucket 'boundaries' field must be sorted in ascending order");
    }
    if (dyn_bson_find(s->bspec, "default", 7, &def)) {
        if (dyn_bson_canon_rank(def.type) == dyn_bson_canon_rank(s->bounds[0].type) && dyn_bson_cmp(def, s->bounds[0]) >= 0
            && dyn_bson_cmp(def, s->bounds[s->nbounds - 1]) < 0)
            return dd_fail(e, 40199, "The $bucket 'default' field must be less than the lowest boundary or greater than or equal to the highest boundary");
        s->has_def = 1;
        s->defv = def;
    }
    if (dd_expr_compile(&s->expr, gb, e) < 0)
        return -1;
    if (dyn_bson_find(s->bspec, "output", 6, &out)) {
        if (out.type != DYN_BSON_DOC)
            return dd_fail(e, 40196, "The $bucket 'output' field must be an object");
        return compile_accs(s, out.val, 0, "$bucket", e);
    } else {
        static const uint8_t cnt[] = { 27, 0, 0, 0, 3, 'c', 'o', 'u', 'n', 't', 0, 15, 0, 0, 0, 0x10, '$', 's', 'u', 'm', 0, 1, 0, 0, 0, 0, 0 };
        return compile_accs(s, cnt, 0, "$bucket", e);
    }
}

static int parse_count(dyn_bson_val v, uint64_t* out, int positive, dd_err* e)
{
    double d;
    if (!dyn_bson_is_number(v.type))
        return dd_fail(e, positive ? 15957 : 15972, "the %s must be specified as a number", positive ? "limit" : "skip");
    d = dyn_bson_num_f64(v);
    if (d != floor(d) || d >= 9.2e18)
        return dd_fail(e, positive ? 15957 : 15972, "the %s must be an integer", positive ? "limit" : "skip");
    if (positive ? d <= 0 : d < 0)
        return dd_fail(e, positive ? 15958 : 15956, positive ? "the limit must be positive" : "the skip must be non-negative");
    *out = v.type == DYN_BSON_INT64 ? dyn_bson_rd64(v.val) : (uint64_t)d;
    return 0;
}

static int add_lname(dd_agg* a, const char* s, size_t n, dd_err* e);

static int compile_lookup(dd_agg* a, ag_stage* s, dyn_bson_val v, dd_err* e)
{
    static const char* const req[] = { "from", "localField", "foreignField", "as" };
    const char* sv[4];
    size_t sn[4];
    dyn_bson_iter it;
    int i;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 40319, "the $lookup specification must be an object");
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        for (i = 0; i < 4; i++)
            if (strlen(req[i]) == it.klen && memcmp(req[i], it.key, it.klen) == 0)
                break;
        if (i == 4)
            return dd_fail(e, 9, "unknown argument to $lookup: %.*s (only from, localField, foreignField and as are supported)", (int)it.klen, it.key);
    }
    for (i = 0; i < 4; i++) {
        dyn_bson_val f;
        if (!dyn_bson_find(v.val, req[i], strlen(req[i]), &f))
            return dd_fail(e, i == 3 ? 40321 : 40320, "$lookup requires '%s'", req[i]);
        if (!str_val(f, &sv[i], &sn[i]) || sn[i] == 0)
            return dd_fail(e, 4570, "$lookup argument '%s' must be a non-empty string", req[i]);
    }
    if (dupn(&s->from, sv[0], sn[0]) < 0 || dupn(&s->name, sv[3], sn[3]) < 0)
        return dd_fail(e, DD_E_OOM, "out of memory");
    s->from_n = sn[0];
    s->name_n = sn[3];
    if (dd_path_parse(&s->local, sv[1], sn[1], e) < 0)
        return -1;
    if (dd_path_parse(&s->foreign, sv[2], sn[2], e) < 0) {
        dd_path_free(&s->local);
        return -1;
    }
    s->has_lf = 1;
    if (tree_add(&s->tree, sv[3], sn[3], 0, "$lookup", e) < 0)
        return -1;
    return add_lname(a, sv[0], sn[0], e);
}

static int add_lname(dd_agg* a, const char* s, size_t n, dd_err* e)
{
    int i;
    char** nn;
    size_t* nl;
    for (i = 0; i < a->nl; i++)
        if (a->llens[i] == n && memcmp(a->lnames[i], s, n) == 0)
            return 0;
    nn = realloc(a->lnames, (size_t)(a->nl + 1) * sizeof *nn);
    if (!nn)
        return dd_fail(e, DD_E_OOM, "out of memory");
    a->lnames = nn;
    nl = realloc(a->llens, (size_t)(a->nl + 1) * sizeof *nl);
    if (!nl)
        return dd_fail(e, DD_E_OOM, "out of memory");
    a->llens = nl;
    if (dupn(&a->lnames[a->nl], s, n) < 0)
        return dd_fail(e, DD_E_OOM, "out of memory");
    a->llens[a->nl++] = n;
    return 0;
}

static int compile_pipeline(dd_agg* a, const uint8_t* pipeline, void* re_opaque, int depth, dd_err* e);

static int compile_facet(dd_agg* a, ag_stage* s, dyn_bson_val v, void* re_opaque, int depth, dd_err* e)
{
    dyn_bson_iter it;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, 15955, "the $facet specification must be a non-empty object");
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val p = dyn_bson_iter_val(&it);
        dd_agg* sub;
        int i;
        if (it.klen == 0 || it.key[0] == '$' || memchr(it.key, '.', it.klen))
            return dd_fail(e, 40352, "$facet output field '%.*s' must not be empty, start with '$' or contain '.'", (int)it.klen, it.key);
        if (p.type != DYN_BSON_ARRAY)
            return dd_fail(e, 40170, "arguments to $facet must be arrays, '%.*s' is not", (int)it.klen, it.key);
        {
            struct dd_agg** nf = realloc(s->facets, (size_t)(s->nfacets + 1) * sizeof *nf);
            char** nn;
            uint32_t* nl;
            if (!nf)
                return dd_fail(e, DD_E_OOM, "out of memory");
            s->facets = nf;
            if (!(nn = realloc(s->fnames, (size_t)(s->nfacets + 1) * sizeof *nn)))
                return dd_fail(e, DD_E_OOM, "out of memory");
            s->fnames = nn;
            if (!(nl = realloc(s->fnlens, (size_t)(s->nfacets + 1) * sizeof *nl)))
                return dd_fail(e, DD_E_OOM, "out of memory");
            s->fnlens = nl;
        }
        s->facets[s->nfacets] = NULL;
        s->fnames[s->nfacets] = NULL;
        if (dupn(&s->fnames[s->nfacets], it.key, it.klen) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        s->fnlens[s->nfacets] = (uint32_t)it.klen;
        s->nfacets++;
        sub = calloc(1, sizeof *sub);
        if (!sub)
            return dd_fail(e, DD_E_OOM, "out of memory");
        s->facets[s->nfacets - 1] = sub;
        if (compile_pipeline(sub, p.val, re_opaque, depth + 1, e) < 0)
            return -1;
        for (i = 0; i < sub->n; i++)
            if (sub->st[i].kind == S_FACET)
                return dd_fail(e, 40600, "$facet is not allowed to be used within a $facet stage");
        if (sub->has_regex)
            a->has_regex = 1;
        for (i = 0; i < sub->nl; i++)
            if (add_lname(a, sub->lnames[i], sub->llens[i], e) < 0)
                return -1;
    }
    if (s->nfacets == 0)
        return dd_fail(e, 40169, "the $facet specification must be a non-empty object");
    return 0;
}

static int compile_sort_spec(ag_stage* s, const uint8_t* spec, size_t n, dd_err* e)
{
    s->spec = malloc(n);
    if (!s->spec)
        return dd_fail(e, DD_E_OOM, "out of memory");
    memcpy(s->spec, spec, n);
    if (dd_sort_parse(&s->sort, s->spec, e) < 0)
        return -1;
    s->has_sort = 1;
    if (s->sort.n == 0)
        return dd_fail(e, 15976, "$sort stage must have at least one sort key");
    return 0;
}

static int compile_stage(dd_agg* a, const char* key, size_t klen, dyn_bson_val v, void* re_opaque, int depth, dd_err* e)
{
#define K(lit) (klen == sizeof(lit) - 1 && memcmp(key, lit, klen) == 0)
    ag_stage* s;
    if (K("$match")) {
        if (v.type != DYN_BSON_DOC)
            return dd_fail(e, 15959, "the match filter must be an expression in an object");
        if (!(s = new_stage(a, S_MATCH)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (dd_filter_compile(&s->f, v.val, re_opaque, e) < 0)
            return -1;
        s->has_f = 1;
        if (s->f.has_regex)
            a->has_regex = 1;
        return 0;
    }
    if (K("$project")) {
        if (!(s = new_stage(a, S_PROJECT)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_project(s, v, e);
    }
    if (K("$addFields") || K("$set")) {
        if (!(s = new_stage(a, S_ADDFIELDS)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_addfields(s, v, K("$set") ? "$set" : "$addFields", e);
    }
    if (K("$unset")) {
        if (!(s = new_stage(a, S_UNSET)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_unset(s, v, e);
    }
    if (K("$unwind")) {
        if (!(s = new_stage(a, S_UNWIND)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_unwind(s, v, e);
    }
    if (K("$group")) {
        if (!(s = new_stage(a, S_GROUP)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_group(s, v, e);
    }
    if (K("$bucket")) {
        if (!(s = new_stage(a, S_GROUP)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_bucket(s, v, e);
    }
    if (K("$sortByCount")) {
        static const uint8_t cnt[] = { 27, 0, 0, 0, 3, 'c', 'o', 'u', 'n', 't', 0, 15, 0, 0, 0, 0x10, '$', 's', 'u', 'm', 0, 1, 0, 0, 0, 0, 0 };
        static const uint8_t srt[] = { 16, 0, 0, 0, 0x10, 'c', 'o', 'u', 'n', 't', 0, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
        if (!((v.type == DYN_BSON_STRING && dyn_bson_rd32(v.val) > 2 && v.val[4] == '$')
                || (v.type == DYN_BSON_DOC && v.vlen > 5 && is_leaf_spec(v))))
            return dd_fail(e, 40148, "the $sortByCount field must be defined as a $-prefixed path or an expression object");
        if (!(s = new_stage(a, S_GROUP)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (dd_expr_compile(&s->expr, v, e) < 0 || compile_accs(s, cnt, 0, "$sortByCount", e) < 0)
            return -1;
        if (!(s = new_stage(a, S_SORT)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_sort_spec(s, srt, sizeof srt, e);
    }
    if (K("$sort")) {
        if (v.type != DYN_BSON_DOC)
            return dd_fail(e, 15973, "the $sort key specification must be an object");
        if (!(s = new_stage(a, S_SORT)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_sort_spec(s, v.val, v.vlen, e);
    }
    if (K("$limit") || K("$skip")) {
        if (!(s = new_stage(a, K("$limit") ? S_LIMIT : S_SKIP)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return parse_count(v, &s->n, s->kind == S_LIMIT, e);
    }
    if (K("$count")) {
        const char* cs;
        size_t cn;
        if (!str_val(v, &cs, &cn))
            return dd_fail(e, 40156, "the count field must be a non-empty string");
        if (cn == 0)
            return dd_fail(e, 40157, "the count field must be a non-empty string");
        if (cs[0] == '$')
            return dd_fail(e, 40158, "the count field cannot be a $-prefixed path");
        if (memchr(cs, '.', cn))
            return dd_fail(e, 40160, "the count field cannot contain '.'");
        if (!(s = new_stage(a, S_COUNT)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (dupn(&s->name, cs, cn) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        s->name_n = cn;
        return 0;
    }
    if (K("$lookup")) {
        if (!(s = new_stage(a, S_LOOKUP)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_lookup(a, s, v, e);
    }
    if (K("$facet")) {
        if (depth > 0)
            return dd_fail(e, 40600, "$facet is not allowed to be used within a $facet stage");
        if (!(s = new_stage(a, S_FACET)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return compile_facet(a, s, v, re_opaque, depth, e);
    }
    if (K("$replaceRoot") || K("$replaceWith")) {
        dyn_bson_val nr = v;
        if (K("$replaceRoot")) {
            dyn_bson_iter it;
            if (v.type != DYN_BSON_DOC || !dyn_bson_find(v.val, "newRoot", 7, &nr))
                return dd_fail(e, 40231, "no newRoot specified for the $replaceRoot stage");
            dyn_bson_iter_init(&it, v.val, v.vlen);
            while (dyn_bson_iter_next(&it) > 0)
                if (!(it.klen == 7 && memcmp(it.key, "newRoot", 7) == 0))
                    return dd_fail(e, 40415, "unrecognized option to $replaceRoot: %.*s", (int)it.klen, it.key);
        }
        if (!(s = new_stage(a, S_REPLACEROOT)))
            return dd_fail(e, DD_E_OOM, "out of memory");
        return dd_expr_compile(&s->expr, nr, e);
    }
#undef K
    return dd_fail(e, 40324, "Unrecognized pipeline stage name: '%.*s'", (int)klen, key);
}

static int compile_pipeline(dd_agg* a, const uint8_t* pipeline, void* re_opaque, int depth, dd_err* e)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, pipeline, dyn_bson_rd32(pipeline));
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val v = dyn_bson_iter_val(&it);
        dyn_bson_iter si;
        const char* key;
        size_t klen;
        if (v.type != DYN_BSON_DOC)
            return dd_fail(e, 14, "each element of the 'pipeline' array must be an object");
        dyn_bson_iter_init(&si, v.val, v.vlen);
        if (dyn_bson_iter_next(&si) <= 0)
            return dd_fail(e, 40323, "A pipeline stage specification object must contain exactly one field.");
        key = si.key;
        klen = si.klen;
        {
            dyn_bson_val sv = dyn_bson_iter_val(&si);
            if (dyn_bson_iter_next(&si) > 0)
                return dd_fail(e, 40323, "A pipeline stage specification object must contain exactly one field.");
            if (compile_stage(a, key, klen, sv, re_opaque, depth, e) < 0)
                return -1;
        }
    }
    return 0;
}

int dd_agg_compile(dd_agg** out, const uint8_t* pipeline, void* re_opaque, dd_err* e)
{
    dd_agg* a = calloc(1, sizeof *a);
    int i = 0;
    *out = NULL;
    if (!a)
        return dd_fail(e, DD_E_OOM, "out of memory");
    if (compile_pipeline(a, pipeline, re_opaque, 0, e) < 0) {
        dd_agg_free(a);
        return -1;
    }
    if (i < a->n && a->st[i].kind == S_MATCH) {
        a->lmatch = a->st[i].f.src;
        i++;
    }
    if (i < a->n && a->st[i].kind == S_SORT) {
        a->lsort = a->st[i].spec;
        i++;
    }
    if (i < a->n && a->st[i].kind == S_SKIP) {
        a->pskip = a->st[i].n;
        i++;
    }
    if (i < a->n && a->st[i].kind == S_LIMIT) {
        a->plimit = a->st[i].n;
        i++;
    }
    a->prefix = i;
    for (i = 0; i + 1 < a->n; i++)
        if (a->st[i].kind == S_SORT && a->st[i + 1].kind == S_LIMIT)
            a->st[i].n = a->st[i + 1].n;
    *out = a;
    return 0;
}

int dd_agg_has_regex(const dd_agg* a)
{
    return a->has_regex;
}

int dd_agg_lookup_count(const dd_agg* a)
{
    return a->nl;
}

const char* dd_agg_lookup_name(const dd_agg* a, int i, size_t* n)
{
    *n = a->llens[i];
    return a->lnames[i];
}

const uint8_t* dd_agg_leading_match(const dd_agg* a)
{
    return a->lmatch;
}

const uint8_t* dd_agg_leading_sort(const dd_agg* a)
{
    return a->lsort;
}

int dd_agg_prefix_len(const dd_agg* a)
{
    return a->prefix;
}

uint64_t dd_agg_prefix_skip(const dd_agg* a)
{
    return a->pskip;
}

uint64_t dd_agg_prefix_limit(const dd_agg* a)
{
    return a->plimit;
}

static int oom(ag_run* r)
{
    return dd_fail(r->e, DD_E_OOM, "out of memory");
}

static int emit_w(ag_run* r, dd_rows* out, dyn_bson_w* w)
{
    if (w->oom)
        return oom(r);
    if (w->n > 16u * 1024 * 1024)
        return dd_fail(r->e, DD_E_TOOLARGE, "an aggregation produced a document larger than 16MB");
    return rows_push_copy(out, w->p, w->n) < 0 ? oom(r) : 0;
}

static int st_match(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    uint32_t i;
    for (i = 0; i < in->n; i++) {
        dd_match_ctx mc = { .pos = -1, .err = r->e };
        int m = dd_filter_match(&s->f, in->p[i], &mc);
        if (mc.failed)
            return -1;
        if (m && rows_push(out, in->p[i]) < 0)
            return oom(r);
    }
    return 0;
}

static int eval_all(ag_stage* s, ag_run* r, dd_xv* vals)
{
    int i;
    for (i = 0; i < s->nex; i++)
        if (dd_expr_eval(s->ex[i], &r->x, &vals[i]) < 0)
            return -1;
    return 0;
}

static int st_project(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    uint32_t i;
    dd_xv small[16];
    dd_xv* vals = s->nex <= 16 ? small : malloc((size_t)s->nex * sizeof *vals);
    int rc = 0;
    if (!vals)
        return oom(r);
    for (i = 0; i < in->n && rc == 0; i++) {
        const uint8_t* doc = in->p[i];
        dyn_bson_w_reset(&r->w);
        if (s->nex == 0 && s->has_proj) {
            if (dd_proj_apply(&s->proj, doc, &r->w) < 0) {
                rc = oom(r);
                break;
            }
            rc = emit_w(r, out, &r->w);
            continue;
        }
        dd_xctx_reset(&r->x, doc);
        if (eval_all(s, r, vals) < 0) {
            rc = -1;
            break;
        }
        dyn_bson_w_reset(&r->w2);
        if (s->kind == S_PROJECT) {
            if (s->has_proj) {
                if (dd_proj_apply(&s->proj, doc, &r->w2) < 0) {
                    rc = oom(r);
                    break;
                }
            } else {
                dyn_bson_val id;
                dyn_bson_w_doc_begin(&r->w2);
                if (s->id_mode != 0 && dyn_bson_find(doc, "_id", 3, &id))
                    dyn_bson_w_value(&r->w2, "_id", 3, id);
                dyn_bson_w_doc_end(&r->w2);
            }
            if (r->w2.oom) {
                rc = oom(r);
                break;
            }
            doc = r->w2.p;
        }
        dyn_bson_w_doc_begin(&r->w);
        write_tree(&r->w, &s->tree, doc, vals, &r->x);
        dyn_bson_w_doc_end(&r->w);
        rc = emit_w(r, out, &r->w);
    }
    if (vals != small)
        free(vals);
    return rc;
}

static int st_replaceroot(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    uint32_t i;
    for (i = 0; i < in->n; i++) {
        dd_xv v;
        dyn_bson_val b;
        dd_xctx_reset(&r->x, in->p[i]);
        if (dd_expr_eval(s->expr, &r->x, &v) < 0)
            return -1;
        b = dd_xv_val(&r->x, &v);
        if (b.type != DYN_BSON_DOC) {
            char rb[96];
            if (b.type == 0)
                b.type = DYN_BSON_UNDEFINED;
            return dd_fail(r->e, 40228, "'newRoot' expression must evaluate to an object, but resulting value was: %s",
                dd_repr(b, rb, sizeof rb));
        }
        if (rows_push_copy(out, b.val, b.vlen) < 0)
            return oom(r);
    }
    return 0;
}

static int st_unwind(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    uint32_t i;
    for (i = 0; i < in->n; i++) {
        const uint8_t* doc = in->p[i];
        dyn_bson_val v;
        dd_xv vals[2];
        int found = dd_path_get_exact(doc, &s->path, &v);
        dd_xctx_reset(&r->x, doc);
        if (found && v.type == DYN_BSON_ARRAY && dyn_bson_rd32(v.val) > 5) {
            dyn_bson_iter it;
            int64_t k = 0;
            dyn_bson_iter_init(&it, v.val, v.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                dyn_bson_val el = dyn_bson_iter_val(&it);
                vals[0].type = el.type;
                vals[0].inbuf = 0;
                vals[0].n = el.vlen;
                vals[0].p = el.val;
                vals[0].off = 0;
                if (s->has_index) {
                    uint8_t b[8];
                    dyn_bson_w_reset(&r->x.buf);
                    dyn_bson_wr64(b, (uint64_t)k);
                    if (dd_xv_put(&r->x, DYN_BSON_INT64, b, 8, &vals[1]) < 0)
                        return -1;
                }
                k++;
                dyn_bson_w_reset(&r->w);
                dyn_bson_w_doc_begin(&r->w);
                write_tree(&r->w, &s->tree, doc, vals, &r->x);
                dyn_bson_w_doc_end(&r->w);
                if (emit_w(r, out, &r->w) < 0)
                    return -1;
            }
            continue;
        }
        if (found && v.type != DYN_BSON_ARRAY && v.type != DYN_BSON_NULL && v.type != DYN_BSON_UNDEFINED) {
            vals[0].type = v.type;
            vals[0].inbuf = 0;
            vals[0].n = v.vlen;
            vals[0].p = v.val;
            vals[0].off = 0;
        } else if (!s->preserve) {
            continue;
        } else if (found && v.type != DYN_BSON_ARRAY) {
            vals[0].type = v.type;
            vals[0].inbuf = 0;
            vals[0].n = v.vlen;
            vals[0].p = v.val;
            vals[0].off = 0;
        } else {
            vals[0].type = 0;
        }
        vals[1].type = DYN_BSON_NULL;
        vals[1].inbuf = 0;
        vals[1].n = 0;
        vals[1].p = (const uint8_t*)"";
        vals[1].off = 0;
        dyn_bson_w_reset(&r->w);
        dyn_bson_w_doc_begin(&r->w);
        write_tree(&r->w, &s->tree, doc, vals, &r->x);
        dyn_bson_w_doc_end(&r->w);
        if (emit_w(r, out, &r->w) < 0)
            return -1;
    }
    return 0;
}

typedef struct {
    int64_t isum;
    double dsum, comp;
    uint64_t cnt;
    uint8_t fl;
    uint8_t vt;
    uint32_t vn;
    uint8_t* vb;
    dyn_bson_w arr;
    uint32_t an;
    dd_kmap* set;
} ag_accst;

enum { F_DBL = 1,
    F_LONG = 2,
    F_OVF = 4,
    F_HAS = 8 };

typedef struct {
    dd_kmap map;
    uint32_t n, cap;
    uint8_t* kt;
    uint32_t* koff;
    uint32_t* kn;
    uint32_t* bidx;
    ag_accst* st;
    int nacc;
    uint8_t* pool;
    size_t pn, pcap;
    struct ag_kc* kc;
    uint32_t kc_look, kc_hit;
    int kc_off;
} ag_groups;

enum { AG_KC_N = 1024,
    AG_KC_MAX = 30 };

typedef struct ag_kc {
    uint32_t gi;
    uint8_t t, n;
    uint8_t b[AG_KC_MAX];
} ag_kc;

static void groups_free(ag_groups* g)
{
    uint32_t i;
    int k;
    if (g->st)
        for (i = 0; i < g->n; i++)
            for (k = 0; k < g->nacc; k++) {
                ag_accst* a = &g->st[(size_t)i * (size_t)g->nacc + (size_t)k];
                free(a->vb);
                dyn_bson_w_free(&a->arr);
                if (a->set) {
                    dd_kmap_free(a->set);
                    free(a->set);
                }
            }
    dd_kmap_free(&g->map);
    free(g->kc);
    free(g->kt);
    free(g->koff);
    free(g->kn);
    free(g->bidx);
    free(g->st);
    free(g->pool);
}

static int set_val(ag_accst* a, dyn_bson_val v)
{
    if (v.type == 0) {
        v.type = DYN_BSON_NULL;
        v.vlen = 0;
    }
    if (v.vlen > a->vn || !a->vb) {
        uint8_t* nb = realloc(a->vb, v.vlen ? v.vlen : 1);
        if (!nb)
            return -1;
        a->vb = nb;
    }
    if (v.vlen)
        memcpy(a->vb, v.val, v.vlen);
    a->vn = v.vlen;
    a->vt = v.type;
    a->fl |= F_HAS;
    return 0;
}

static dyn_bson_val acc_val(const ag_accst* a)
{
    dyn_bson_val v;
    v.type = a->vt;
    v.val = a->vb;
    v.vlen = a->vn;
    return v;
}

static int acc_add(ag_accst* a, int op, dyn_bson_val v, dyn_ks_buf* kb)
{
    switch (op) {
    case A_SUM:
    case A_AVG: {
        double x, t;
        int64_t iv;
        if (!dyn_bson_is_number(v.type))
            return 0;
        a->cnt++;
        if (v.type == DYN_BSON_DOUBLE || v.type == DYN_BSON_DEC128)
            a->fl |= F_DBL;
        if (v.type == DYN_BSON_INT64)
            a->fl |= F_LONG;
        x = dyn_bson_num_f64(v);
        t = a->dsum + x;
        if (fabs(a->dsum) >= fabs(x))
            a->comp += (a->dsum - t) + x;
        else
            a->comp += (x - t) + a->dsum;
        a->dsum = t;
        if (!(a->fl & (F_DBL | F_OVF))) {
            iv = v.type == DYN_BSON_INT32 ? (int32_t)dyn_bson_rd32(v.val) : (int64_t)dyn_bson_rd64(v.val);
            if (__builtin_add_overflow(a->isum, iv, &a->isum))
                a->fl |= F_OVF;
        }
        return 0;
    }
    case A_MIN:
    case A_MAX: {
        int c;
        if (v.type == 0 || v.type == DYN_BSON_NULL || v.type == DYN_BSON_UNDEFINED)
            return 0;
        if (!(a->fl & F_HAS))
            return set_val(a, v);
        c = dyn_bson_cmp(v, acc_val(a));
        if (op == A_MIN ? c < 0 : c > 0)
            return set_val(a, v);
        return 0;
    }
    case A_FIRST:
        return a->fl & F_HAS ? 0 : set_val(a, v);
    case A_LAST:
        return set_val(a, v);
    case A_PUSH:
    case A_ADDTOSET:
        if (v.type == 0)
            return 0;
        if (op == A_ADDTOSET) {
            uint32_t ex;
            int r;
            if (!a->set) {
                a->set = malloc(sizeof *a->set);
                if (!a->set)
                    return -1;
                dd_kmap_init(a->set);
            }
            kb->n = 0;
            dyn_ks_value(kb, v, 0);
            if (kb->oom)
                return -1;
            r = dd_kmap_put(a->set, kb->p, kb->n, a->an, &ex);
            if (r < 0)
                return -1;
            if (r == 1)
                return 0;
        }
        if (a->arr.n == 0)
            dyn_bson_w_doc_begin(&a->arr);
        dyn_bson_w_index_key(&a->arr, v.type, a->an++);
        dyn_bson_w_bytes(&a->arr, v.val, v.vlen);
        if (a->arr.n > 16u * 1024 * 1024)
            return -2;
        return a->arr.oom ? -1 : 0;
    default:
        return 0;
    }
}

static void acc_write(dyn_bson_w* w, const ag_acc* d, ag_accst* a)
{
    switch (d->op) {
    case A_SUM:
        if (a->fl & (F_DBL | F_OVF))
            dyn_bson_w_double(w, d->name, d->nlen, a->dsum + a->comp);
        else if ((a->fl & F_LONG) || a->isum > INT32_MAX || a->isum < INT32_MIN)
            dyn_bson_w_int64(w, d->name, d->nlen, a->isum);
        else
            dyn_bson_w_int32(w, d->name, d->nlen, (int32_t)a->isum);
        break;
    case A_AVG:
        if (a->cnt)
            dyn_bson_w_double(w, d->name, d->nlen, (a->dsum + a->comp) / (double)a->cnt);
        else
            dyn_bson_w_null(w, d->name, d->nlen);
        break;
    case A_MIN:
    case A_MAX:
    case A_FIRST:
    case A_LAST:
        if (a->fl & F_HAS)
            dyn_bson_w_value(w, d->name, d->nlen, acc_val(a));
        else
            dyn_bson_w_null(w, d->name, d->nlen);
        break;
    default:
        if (a->arr.n == 0)
            dyn_bson_w_doc_begin(&a->arr);
        dyn_bson_w_doc_end(&a->arr);
        if (a->arr.oom) {
            w->oom = 1;
            break;
        }
        dyn_bson_w_key(w, DYN_BSON_ARRAY, d->name, d->nlen);
        dyn_bson_w_bytes(w, a->arr.p, a->arr.n);
        break;
    }
}

#define FX_SLOTS 16
#define FX_REFS 32

typedef struct {
    const uint8_t* doc;
    const dd_path* up;
    dyn_bson_val el;
    int nref, nslot;
    const dd_path* ref[FX_REFS];
    uint8_t rslot[FX_REFS];
    const dd_path* sp[FX_SLOTS];
    uint8_t base[FX_SLOTS];
    uint8_t st[FX_SLOTS];
    dyn_bson_val fv[FX_SLOTS];
} fx_ctx;

typedef struct {
    dyn_bson_val v;
    uint8_t b[8];
} fx_slot;

static int fx_field_get(const dd_path* p, const fx_ctx* c, dyn_bson_val* out)
{
    dyn_bson_val cur;
    int i = 0;
    cur.type = DYN_BSON_DOC;
    cur.val = c->doc;
    cur.vlen = dyn_bson_rd32(c->doc);
    if (c->up) {
        int m = 0;
        while (m < c->up->n && m < p->n && p->plen[m] == c->up->plen[m] && !memcmp(p->part[m], c->up->part[m], p->plen[m]))
            m++;
        if (m == c->up->n) {
            cur = c->el;
            i = m;
            if (cur.type == 0) {
                out->type = 0;
                return 1;
            }
        } else if (m > 0 || p->n == 0)
            return 0;
    }
    for (; i < p->n; i++) {
        if (cur.type == DYN_BSON_ARRAY)
            return 0;
        if (cur.type != DYN_BSON_DOC || !dyn_bson_find(cur.val, p->part[i], p->plen[i], &cur)) {
            out->type = 0;
            return 1;
        }
    }
    *out = cur;
    return 1;
}

static int fx_prefix(const dd_path* p, const dd_path* up)
{
    int m = 0;
    while (m < up->n && m < p->n && p->plen[m] == up->plen[m] && !memcmp(p->part[m], up->part[m], p->plen[m]))
        m++;
    return m;
}

static int fx_field(const dd_path* p, fx_ctx* c, dyn_bson_val* out)
{
    int i, sl = -1, rc;
    for (i = 0; i < c->nref; i++)
        if (c->ref[i] == p) {
            sl = c->rslot[i];
            break;
        }
    if (sl >= 0 && c->st[sl]) {
        *out = c->fv[sl];
        return c->st[sl] == 1;
    }
    rc = fx_field_get(p, c, out);
    if (sl >= 0) {
        c->st[sl] = rc ? 1 : 2;
        c->fv[sl] = *out;
    }
    return rc;
}

static void fx_collect(fx_ctx* c, const dd_xn* x, int depth)
{
    const dd_path* p;
    const dd_xn* const* args;
    uint32_t n, i;
    int op, j;
    if (dd_expr_is_field(x, &p)) {
        if (c->nref == FX_REFS)
            return;
        for (j = 0; j < c->nslot; j++)
            if (c->sp[j]->tlen == p->tlen && !memcmp(c->sp[j]->text, p->text, p->tlen))
                break;
        if (j == c->nslot) {
            if (c->nslot == FX_SLOTS)
                return;
            c->sp[j] = p;
            c->base[j] = 0;
            if (c->up && p->n == c->up->n + 1 && fx_prefix(p, c->up) == c->up->n)
                c->base[j] = 2;
            else if (p->n == 1 && (!c->up || fx_prefix(p, c->up) == 0))
                c->base[j] = 1;
            c->nslot++;
        }
        c->ref[c->nref] = p;
        c->rslot[c->nref++] = (uint8_t)j;
        return;
    }
    if (depth >= 4 || !dd_expr_arith(x, &op, &args, &n))
        return;
    for (i = 0; i < n; i++)
        fx_collect(c, args[i], depth + 1);
}

static void fx_setup(fx_ctx* c, const ag_stage* s, const dd_path* up)
{
    int k;
    c->up = up;
    c->el.type = 0;
    c->nref = c->nslot = 0;
    fx_collect(c, s->expr, 0);
    for (k = 0; k < s->nacc; k++)
        fx_collect(c, s->acc[k].arg, 0);
}

static void fx_scan(fx_ctx* c, const uint8_t* doc, int base)
{
    uint32_t end = dyn_bson_rd32(doc), off = 4;
    int j, left = 0;
    for (j = 0; j < c->nslot; j++)
        left += c->base[j] == base;
    while (left && end >= 5 && off < end - 1) {
        uint8_t t = doc[off++];
        const uint8_t* k = doc + off;
        uint32_t kl = 0, vl;
        if (!t)
            break;
        while (off + kl < end - 1 && k[kl])
            kl++;
        if (off + kl >= end - 1)
            break;
        off += kl + 1;
        if (dyn_bson_value_size(t, doc + off, (size_t)(end - 1 - off), &vl) < 0)
            break;
        for (j = 0; j < c->nslot; j++) {
            const dd_path* p = c->sp[j];
            int last = p->n - 1;
            if (c->base[j] != base || c->st[j] || kl != p->plen[last] || memcmp(k, p->part[last], kl))
                continue;
            c->fv[j].type = t;
            c->fv[j].val = doc + off;
            c->fv[j].vlen = vl;
            c->st[j] = 1;
            left--;
        }
        off += vl;
    }
    for (j = 0; j < c->nslot; j++)
        if (c->base[j] == base && !c->st[j]) {
            c->fv[j].type = 0;
            c->st[j] = 1;
        }
}

static void fx_row(fx_ctx* c, int newdoc)
{
    int j;
    if (!c->nslot)
        return;
    for (j = 0; j < c->nslot; j++)
        if (newdoc || c->base[j] != 1)
            c->st[j] = 0;
    if (newdoc)
        fx_scan(c, c->doc, 1);
    if (c->el.type == DYN_BSON_DOC)
        fx_scan(c, c->el.val, 2);
}

static int fx_ok(const dd_xn* x, int depth)
{
    const dd_path* p;
    const dd_xn* const* args;
    dyn_bson_val v;
    uint32_t n, i;
    int op;
    if (dd_expr_is_field(x, &p))
        return 1;
    if (dd_expr_lit(x, &v))
        return v.type != DYN_BSON_DOC && v.type != DYN_BSON_ARRAY;
    if (depth >= 4 || !dd_expr_arith(x, &op, &args, &n) || n > 16)
        return 0;
    for (i = 0; i < n; i++)
        if (!fx_ok(args[i], depth + 1))
            return 0;
    return 1;
}

static int64_t fx_i64(dyn_bson_val v)
{
    return v.type == DYN_BSON_INT32 ? (int64_t)(int32_t)dyn_bson_rd32(v.val) : (int64_t)dyn_bson_rd64(v.val);
}

static void fx_put_f64(uint8_t* nb, double d, dyn_bson_val* out)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    dyn_bson_wr64(nb, u);
    out->type = DYN_BSON_DOUBLE;
    out->val = nb;
    out->vlen = 8;
}

static void fx_put_int(uint8_t* nb, int64_t i, int exact64, dyn_bson_val* out)
{
    out->val = nb;
    if (!exact64 && i >= INT32_MIN && i <= INT32_MAX) {
        dyn_bson_wr32(nb, (uint32_t)(int32_t)i);
        out->type = DYN_BSON_INT32;
        out->vlen = 4;
        return;
    }
    dyn_bson_wr64(nb, (uint64_t)i);
    out->type = DYN_BSON_INT64;
    out->vlen = 8;
}

static int fx_eval(const dd_xn* x, fx_ctx* c, uint8_t* nb, dyn_bson_val* out, int depth)
{
    const dd_path* p;
    const dd_xn* const* args;
    uint8_t tb[16][8];
    dyn_bson_val av[16];
    uint32_t n, i;
    int op, any_double = 0, any_long = 0, overflow = 0;
    int64_t isum;
    double dsum;
    if (dd_expr_is_field(x, &p))
        return fx_field(p, c, out);
    if (dd_expr_lit(x, out))
        return out->type != DYN_BSON_DOC && out->type != DYN_BSON_ARRAY;
    if (depth >= 4 || !dd_expr_arith(x, &op, &args, &n) || n > 16 || (op == DD_XA_SUB && n != 2))
        return 0;
    for (i = 0; i < n; i++)
        if (!fx_eval(args[i], c, tb[i], &av[i], depth + 1))
            return 0;
    for (i = 0; i < n; i++) {
        uint8_t t = av[i].type;
        if (t == 0 || t == DYN_BSON_NULL || t == DYN_BSON_UNDEFINED) {
            out->type = DYN_BSON_NULL;
            out->val = nb;
            out->vlen = 0;
            return 1;
        }
        if (t != DYN_BSON_INT32 && t != DYN_BSON_INT64 && t != DYN_BSON_DOUBLE)
            return 0;
    }
    if (op == DD_XA_SUB) {
        int64_t d;
        if (av[0].type == DYN_BSON_DOUBLE || av[1].type == DYN_BSON_DOUBLE)
            fx_put_f64(nb, dyn_bson_num_f64(av[0]) - dyn_bson_num_f64(av[1]), out);
        else if (__builtin_sub_overflow(fx_i64(av[0]), fx_i64(av[1]), &d))
            fx_put_f64(nb, dyn_bson_num_f64(av[0]) - dyn_bson_num_f64(av[1]), out);
        else
            fx_put_int(nb, d, av[0].type == DYN_BSON_INT64 || av[1].type == DYN_BSON_INT64, out);
        return 1;
    }
    isum = op == DD_XA_MUL ? 1 : 0;
    dsum = op == DD_XA_MUL ? 1 : 0;
    for (i = 0; i < n; i++) {
        dyn_bson_val v = av[i];
        if (v.type == DYN_BSON_DOUBLE)
            any_double = 1;
        if (v.type == DYN_BSON_INT64)
            any_long = 1;
        if (op == DD_XA_ADD) {
            dsum += dyn_bson_num_f64(v);
            if (!any_double && __builtin_add_overflow(isum, fx_i64(v), &isum))
                overflow = 1;
        } else {
            dsum *= dyn_bson_num_f64(v);
            if (!any_double && __builtin_mul_overflow(isum, fx_i64(v), &isum))
                overflow = 1;
        }
    }
    if (any_double || overflow)
        fx_put_f64(nb, dsum, out);
    else
        fx_put_int(nb, isum, any_long, out);
    return 1;
}

static uint32_t kc_slot(dyn_bson_val kv)
{
    uint64_t h = (uint64_t)kv.vlen << 8 | kv.type;
    uint32_t i;
    for (i = 0; i < kv.vlen; i += 8) {
        uint64_t w = 0;
        memcpy(&w, kv.val + i, kv.vlen - i < 8 ? kv.vlen - i : 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32;
    }
    h *= 0xBF58476D1CE4E5B9ull;
    return (uint32_t)(h >> 54);
}

static int group_key_slow(ag_stage* s, ag_groups* g, dyn_bson_val kv, ag_run* r, uint32_t* gi);

static int group_key(ag_stage* s, ag_groups* g, dyn_bson_val kv, ag_run* r, uint32_t* gi)
{
    ag_kc* k = NULL;
    int rc;
    if (!s->bucket && !g->kc_off && kv.type && kv.vlen <= AG_KC_MAX) {
        if (!g->kc)
            g->kc = calloc(AG_KC_N, sizeof *g->kc);
        if (g->kc) {
            k = &g->kc[kc_slot(kv)];
            if (++g->kc_look == 4096)
                g->kc_off = g->kc_hit * 2 < g->kc_look;
            if (k->t == kv.type && k->n == kv.vlen && (!kv.vlen || memcmp(k->b, kv.val, kv.vlen) == 0)) {
                g->kc_hit++;
                *gi = k->gi;
                return 0;
            }
        }
    }
    rc = group_key_slow(s, g, kv, r, gi);
    if (rc == 0 && k) {
        k->t = kv.type;
        k->n = (uint8_t)kv.vlen;
        if (kv.vlen)
            memcpy(k->b, kv.val, kv.vlen);
        k->gi = *gi;
    }
    return rc;
}

static int group_key_slow(ag_stage* s, ag_groups* g, dyn_bson_val kv, ag_run* r, uint32_t* gi)
{
    uint32_t ex;
    uint32_t bidx = 0;
    int pr;
    r->kb.n = 0;
    if (s->bucket) {
        int lo = 0, hi = s->nbounds - 1;
        dyn_bson_val q = kv;
        if (q.type == 0)
            q.type = DYN_BSON_NULL;
        if (dyn_bson_canon_rank(q.type) != dyn_bson_canon_rank(s->bounds[0].type) || dyn_bson_cmp(q, s->bounds[0]) < 0
            || dyn_bson_cmp(q, s->bounds[s->nbounds - 1]) >= 0) {
            if (!s->has_def)
                return dd_fail(r->e, 40066,
                    "$switch could not find a matching branch for an input, and no default was specified.");
            bidx = UINT32_MAX;
            kv = s->defv;
        } else {
            while (hi - lo > 1) {
                int mid = (lo + hi) / 2;
                if (dyn_bson_cmp(q, s->bounds[mid]) >= 0)
                    lo = mid;
                else
                    hi = mid;
            }
            bidx = (uint32_t)lo;
            kv = s->bounds[lo];
        }
        dyn_ks_raw(&r->kb, (const uint8_t*)&bidx, 4);
    } else {
        if (kv.type == 0) {
            kv.type = DYN_BSON_NULL;
            kv.vlen = 0;
        }
        dyn_ks_value(&r->kb, kv, 0);
    }
    if (r->kb.oom)
        return oom(r);
    pr = dd_kmap_put(&g->map, r->kb.p, r->kb.n, g->n, &ex);
    if (pr < 0)
        return oom(r);
    if (pr == 1) {
        *gi = ex;
        return 0;
    }
    if (g->n == g->cap) {
        uint32_t nc = g->cap ? g->cap * 2 : 64;
        uint8_t* nkt = realloc(g->kt, nc);
        uint32_t *nko, *nkn, *nbi;
        ag_accst* nst;
        if (nkt)
            g->kt = nkt;
        nko = realloc(g->koff, (size_t)nc * sizeof *nko);
        if (nko)
            g->koff = nko;
        nkn = realloc(g->kn, (size_t)nc * sizeof *nkn);
        if (nkn)
            g->kn = nkn;
        nbi = realloc(g->bidx, (size_t)nc * sizeof *nbi);
        if (nbi)
            g->bidx = nbi;
        nst = realloc(g->st, (size_t)nc * (size_t)(g->nacc ? g->nacc : 1) * sizeof *nst);
        if (nst)
            g->st = nst;
        if (!nkt || !nko || !nkn || !nbi || !nst)
            return oom(r);
        g->cap = nc;
    }
    if (g->pn + kv.vlen > g->pcap) {
        size_t nc = g->pcap ? g->pcap * 2 : 4096;
        uint8_t* np;
        while (nc < g->pn + kv.vlen)
            nc *= 2;
        np = realloc(g->pool, nc);
        if (!np)
            return oom(r);
        g->pool = np;
        g->pcap = nc;
    }
    if (kv.vlen)
        memcpy(g->pool + g->pn, kv.val, kv.vlen);
    g->kt[g->n] = kv.type;
    g->koff[g->n] = (uint32_t)g->pn;
    g->kn[g->n] = kv.vlen;
    g->bidx[g->n] = bidx;
    g->pn += kv.vlen;
    if (g->nacc) {
        int k;
        ag_accst* st = &g->st[(size_t)g->n * (size_t)g->nacc];
        memset(st, 0, (size_t)g->nacc * sizeof *st);
        for (k = 0; k < g->nacc; k++)
            dyn_bson_w_init(&st[k].arr);
    }
    *gi = g->n++;
    return 0;
}

static void sort_by_bidx(uint32_t* ord, uint32_t n, const uint32_t* bi)
{
    uint32_t i, j;
    for (i = 1; i < n; i++) {
        uint32_t t = ord[i];
        for (j = i; j > 0 && bi[ord[j - 1]] > bi[t]; j--)
            ord[j] = ord[j - 1];
        ord[j] = t;
    }
}

static int group_acc(ag_stage* s, ag_groups* g, uint32_t gi, int k, dyn_bson_val v, ag_run* r)
{
    int ar = acc_add(&g->st[(size_t)gi * (size_t)g->nacc + (size_t)k], s->acc[k].op, v, &r->kb);
    if (ar < 0)
        return ar == -2 ? dd_fail(r->e, DD_E_TOOLARGE, "$push/$addToSet used too much memory") : oom(r);
    return 0;
}

static int group_fast(ag_stage* s, ag_groups* g, fx_ctx* c, fx_slot* sl, ag_run* r)
{
    uint32_t gi;
    int k;
    if (!fx_eval(s->expr, c, sl[0].b, &sl[0].v, 0))
        return 0;
    for (k = 0; k < s->nacc; k++)
        if (!fx_eval(s->acc[k].arg, c, sl[k + 1].b, &sl[k + 1].v, 0))
            return 0;
    if (group_key(s, g, sl[0].v, r, &gi) < 0)
        return -1;
    for (k = 0; k < s->nacc; k++)
        if (group_acc(s, g, gi, k, sl[k + 1].v, r) < 0)
            return -1;
    return 1;
}

static int group_slow(ag_stage* s, ag_groups* g, const uint8_t* doc, ag_run* r)
{
    dd_xv xv;
    uint32_t gi;
    int k;
    dd_xctx_reset(&r->x, doc);
    if (dd_expr_eval(s->expr, &r->x, &xv) < 0)
        return -1;
    if (group_key(s, g, dd_xv_val(&r->x, &xv), r, &gi) < 0)
        return -1;
    for (k = 0; k < s->nacc; k++) {
        dyn_bson_w_reset(&r->x.buf);
        if (dd_expr_eval(s->acc[k].arg, &r->x, &xv) < 0)
            return -1;
        if (group_acc(s, g, gi, k, dd_xv_val(&r->x, &xv), r) < 0)
            return -1;
    }
    return 0;
}

static int group_fast_ok(const ag_stage* s)
{
    int k;
    if (!fx_ok(s->expr, 0))
        return 0;
    for (k = 0; k < s->nacc; k++)
        if (!fx_ok(s->acc[k].arg, 0))
            return 0;
    return 1;
}

static int group_emit(ag_stage* s, ag_groups* g, dd_rows* out, ag_run* r)
{
    uint32_t i, *ord = malloc((size_t)(g->n ? g->n : 1) * sizeof *ord);
    int k, rc = 0;
    if (!ord)
        return oom(r);
    for (i = 0; i < g->n; i++)
        ord[i] = i;
    if (s->bucket)
        sort_by_bidx(ord, g->n, g->bidx);
    for (i = 0; i < g->n && rc == 0; i++) {
        uint32_t gi = ord[i];
        dyn_bson_val kv;
        kv.type = g->kt[gi];
        kv.val = g->pool ? g->pool + g->koff[gi] : (const uint8_t*)"";
        kv.vlen = g->kn[gi];
        dyn_bson_w_reset(&r->w);
        dyn_bson_w_doc_begin(&r->w);
        dyn_bson_w_value(&r->w, "_id", 3, kv);
        for (k = 0; k < s->nacc; k++)
            acc_write(&r->w, &s->acc[k], &g->st[(size_t)gi * (size_t)g->nacc + (size_t)k]);
        dyn_bson_w_doc_end(&r->w);
        rc = emit_w(r, out, &r->w);
    }
    free(ord);
    return rc;
}

static fx_slot* group_begin(ag_stage* s, ag_groups* g, ag_run* r)
{
    fx_slot* sl = malloc((size_t)(s->nacc + 1) * sizeof *sl);
    memset(g, 0, sizeof *g);
    dd_kmap_init(&g->map);
    g->nacc = s->nacc;
    if (!sl)
        oom(r);
    return sl;
}

static int st_group(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    ag_groups g;
    fx_ctx c;
    uint32_t i;
    int rc = 0, fast = group_fast_ok(s);
    fx_slot* sl = group_begin(s, &g, r);
    if (!sl) {
        groups_free(&g);
        return -1;
    }
    fx_setup(&c, s, NULL);
    for (i = 0; i < in->n && rc == 0; i++) {
        c.doc = in->p[i];
        if (fast)
            fx_row(&c, 1);
        rc = fast ? group_fast(s, &g, &c, sl, r) : 0;
        if (rc == 0)
            rc = group_slow(s, &g, c.doc, r);
        else if (rc > 0)
            rc = 0;
    }
    if (rc == 0)
        rc = group_emit(s, &g, out, r);
    free(sl);
    groups_free(&g);
    return rc < 0 ? -1 : 0;
}

static void xv_ext(dd_xv* v, dyn_bson_val b)
{
    v->type = b.type;
    v->inbuf = 0;
    v->n = b.vlen;
    v->p = b.val;
    v->off = 0;
}

static int unwind_group_one(ag_stage* su, ag_stage* sg, ag_groups* g, fx_ctx* c, fx_slot* sl, int fast, ag_run* r)
{
    dd_xv vals[2];
    int rc;
    if (fast)
        fx_row(c, 0);
    rc = fast ? group_fast(sg, g, c, sl, r) : 0;
    if (rc != 0)
        return rc < 0 ? -1 : 0;
    if (c->el.type)
        xv_ext(&vals[0], c->el);
    else
        vals[0].type = 0;
    vals[1].type = DYN_BSON_NULL;
    vals[1].inbuf = 0;
    vals[1].n = 0;
    vals[1].p = (const uint8_t*)"";
    vals[1].off = 0;
    dd_xctx_reset(&r->x, c->doc);
    dyn_bson_w_reset(&r->w2);
    dyn_bson_w_doc_begin(&r->w2);
    write_tree(&r->w2, &su->tree, c->doc, vals, &r->x);
    dyn_bson_w_doc_end(&r->w2);
    if (r->w2.oom)
        return oom(r);
    return group_slow(sg, g, r->w2.p, r);
}

static int st_unwind_group(ag_stage* su, ag_stage* sg, dd_rows* in, dd_rows* out, ag_run* r)
{
    ag_groups g;
    fx_ctx c;
    uint32_t i;
    int rc = 0, fast = group_fast_ok(sg);
    fx_slot* sl = group_begin(sg, &g, r);
    if (!sl) {
        groups_free(&g);
        return -1;
    }
    fx_setup(&c, sg, &su->path);
    for (i = 0; i < in->n && rc == 0; i++) {
        dyn_bson_val v;
        int found;
        c.doc = in->p[i];
        c.el.type = 0;
        if (fast)
            fx_row(&c, 1);
        found = dd_path_get_exact(c.doc, &su->path, &v);
        if (found && v.type == DYN_BSON_ARRAY && dyn_bson_rd32(v.val) > 5) {
            dyn_bson_iter it;
            dyn_bson_iter_init(&it, v.val, v.vlen);
            while (rc == 0 && dyn_bson_iter_next(&it) > 0) {
                c.el = dyn_bson_iter_val(&it);
                rc = unwind_group_one(su, sg, &g, &c, sl, fast, r);
            }
            continue;
        }
        if (found && v.type != DYN_BSON_ARRAY && v.type != DYN_BSON_NULL && v.type != DYN_BSON_UNDEFINED)
            c.el = v;
        else if (!su->preserve)
            continue;
        else if (found && v.type != DYN_BSON_ARRAY)
            c.el = v;
        else
            c.el.type = 0;
        rc = unwind_group_one(su, sg, &g, &c, sl, fast, r);
    }
    if (rc == 0)
        rc = group_emit(sg, &g, out, r);
    free(sl);
    groups_free(&g);
    return rc < 0 ? -1 : 0;
}

static int st_sort(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    dd_sortrec* rec;
    uint32_t i;
    int k;
    if (in->n == 0)
        return 0;
    rec = malloc((size_t)in->n * sizeof *rec);
    if (!rec)
        return oom(r);
    r->kb.n = 0;
    for (i = 0; i < in->n; i++) {
        size_t from = r->kb.n;
        dd_sort_key(&s->sort, in->p[i], &r->kb);
        if (r->kb.oom || r->kb.n > UINT32_MAX) {
            free(rec);
            return oom(r);
        }
        rec[i].off = (uint32_t)from;
        rec[i].len = (uint32_t)(r->kb.n - from);
        rec[i].id = i;
    }
    k = dd_sort_records(rec, in->n, r->kb.p, s->n);
    if (k < 0) {
        free(rec);
        return oom(r);
    }
    for (i = 0; i < (uint32_t)k; i++)
        if (rows_push(out, in->p[rec[i].id]) < 0) {
            free(rec);
            return oom(r);
        }
    free(rec);
    return 0;
}

static int st_slice(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    uint64_t from = s->kind == S_SKIP ? s->n : 0, to = s->kind == S_LIMIT ? s->n : UINT64_MAX;
    uint64_t i;
    for (i = from; i < in->n && i < to; i++)
        if (rows_push(out, in->p[i]) < 0)
            return oom(r);
    return 0;
}

static int st_count(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    if (in->n == 0)
        return 0;
    dyn_bson_w_reset(&r->w);
    dyn_bson_w_doc_begin(&r->w);
    dyn_bson_w_int32(&r->w, s->name, s->name_n, (int32_t)in->n);
    dyn_bson_w_doc_end(&r->w);
    return emit_w(r, out, &r->w);
}

typedef struct {
    dyn_ks_buf* kb;
    size_t* offs;
    uint32_t n, cap;
    int local;
    int oom;
} key_ctx;

static int key_visit(void* ud, dyn_bson_val v, int is_elem, int32_t aidx)
{
    key_ctx* k = ud;
    (void)aidx;
    if (k->local && !is_elem && v.type == DYN_BSON_ARRAY)
        return 0;
    if (v.type == 0 || v.type == DYN_BSON_UNDEFINED) {
        v.type = DYN_BSON_NULL;
        v.vlen = 0;
    }
    if (k->n + 1 >= k->cap) {
        uint32_t nc = k->cap ? k->cap * 2 : 16;
        size_t* no = realloc(k->offs, (size_t)nc * sizeof *no);
        if (!no) {
            k->oom = 1;
            return 1;
        }
        k->offs = no;
        k->cap = nc;
    }
    k->offs[k->n++] = k->kb->n;
    dyn_ks_value(k->kb, v, 0);
    if (k->kb->oom) {
        k->oom = 1;
        return 1;
    }
    return 0;
}

static int collect_keys(const uint8_t* doc, const dd_path* p, int local, dyn_ks_buf* kb, key_ctx* k)
{
    static const dyn_bson_val nul = { DYN_BSON_NULL, NULL, 0 };
    k->kb = kb;
    k->n = 0;
    k->local = local;
    k->oom = 0;
    kb->n = 0;
    dd_path_walk(doc, p, key_visit, k);
    if (!k->oom && k->n == 0)
        key_visit(k, nul, 0, -1);
    if (k->oom)
        return -1;
    if (k->n + 1 > k->cap) {
        size_t* no = realloc(k->offs, (size_t)(k->n + 1) * sizeof *no);
        if (!no)
            return -1;
        k->offs = no;
        k->cap = k->n + 1;
    }
    k->offs[k->n] = kb->n;
    return 0;
}

static int u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y ? 1
                              : 0;
}

static int st_lookup(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    const dd_view* fv = NULL;
    dd_kmap heads;
    uint32_t* next = NULL;
    uint32_t* chain_doc = NULL;
    uint32_t* head = NULL;
    uint32_t nkeys = 0, ckeys = 0;
    uint32_t nch = 0, cch = 0, i, j;
    uint32_t* hits = NULL;
    uint32_t nh = 0, ch = 0;
    key_ctx kc;
    dyn_bson_w aw;
    int q, rc = 0;
    memset(&kc, 0, sizeof kc);
    dd_kmap_init(&heads);
    dyn_bson_w_init(&aw);
    for (q = 0; q < r->nsrc; q++)
        if (r->srcs[q].name_n == s->from_n && memcmp(r->srcs[q].name, s->from, s->from_n) == 0) {
            fv = &r->srcs[q].view;
            break;
        }
    for (j = 0; fv && j < fv->ndocs && rc == 0; j++) {
        uint32_t t;
        if (!fv->docs[j])
            continue;
        if (collect_keys(fv->docs[j], &s->foreign, 0, &r->kb, &kc) < 0) {
            rc = oom(r);
            break;
        }
        for (t = 0; t < kc.n; t++) {
            uint32_t ex, kid;
            int pr;
            if (nch == cch) {
                uint32_t nc = cch ? cch * 2 : 256;
                uint32_t* nn = realloc(next, (size_t)nc * sizeof *nn);
                uint32_t* nd;
                if (nn)
                    next = nn;
                nd = realloc(chain_doc, (size_t)nc * sizeof *nd);
                if (nd)
                    chain_doc = nd;
                if (!nn || !nd) {
                    rc = oom(r);
                    break;
                }
                cch = nc;
            }
            pr = dd_kmap_put(&heads, r->kb.p + kc.offs[t], kc.offs[t + 1] - kc.offs[t], nkeys, &ex);
            if (pr < 0) {
                rc = oom(r);
                break;
            }
            if (pr == 1) {
                kid = ex;
            } else {
                if (nkeys == ckeys) {
                    uint32_t nc = ckeys ? ckeys * 2 : 256;
                    uint32_t* nh2 = realloc(head, (size_t)nc * sizeof *nh2);
                    if (!nh2) {
                        rc = oom(r);
                        break;
                    }
                    head = nh2;
                    ckeys = nc;
                }
                kid = nkeys++;
                head[kid] = UINT32_MAX;
            }
            next[nch] = head[kid];
            chain_doc[nch] = j;
            head[kid] = nch++;
        }
    }
    for (i = 0; i < in->n && rc == 0; i++) {
        const uint8_t* doc = in->p[i];
        dd_xv vals[1];
        uint32_t t, idx = 0;
        nh = 0;
        if (collect_keys(doc, &s->local, 1, &r->kb, &kc) < 0) {
            rc = oom(r);
            break;
        }
        for (t = 0; t < kc.n && rc == 0; t++) {
            uint32_t h, kid;
            if (!dd_kmap_get(&heads, r->kb.p + kc.offs[t], kc.offs[t + 1] - kc.offs[t], &kid))
                continue;
            for (h = head[kid]; h != UINT32_MAX; h = next[h]) {
                if (nh == ch) {
                    uint32_t nc = ch ? ch * 2 : 64;
                    uint32_t* nhp = realloc(hits, (size_t)nc * sizeof *nhp);
                    if (!nhp) {
                        rc = oom(r);
                        break;
                    }
                    hits = nhp;
                    ch = nc;
                }
                hits[nh++] = chain_doc[h];
            }
        }
        if (rc)
            break;
        if (nh > 1)
            qsort(hits, nh, sizeof *hits, u32_cmp);
        dyn_bson_w_reset(&aw);
        dyn_bson_w_doc_begin(&aw);
        for (t = 0; t < nh; t++) {
            const uint8_t* fd;
            if (t > 0 && hits[t] == hits[t - 1])
                continue;
            fd = fv->docs[hits[t]];
            dyn_bson_w_index_key(&aw, DYN_BSON_DOC, idx++);
            dyn_bson_w_bytes(&aw, fd, dyn_bson_rd32(fd));
        }
        dyn_bson_w_doc_end(&aw);
        if (aw.oom) {
            rc = oom(r);
            break;
        }
        if (aw.n > 16u * 1024 * 1024) {
            rc = dd_fail(r->e, DD_E_TOOLARGE, "$lookup produced an array larger than 16MB");
            break;
        }
        vals[0].type = DYN_BSON_ARRAY;
        vals[0].inbuf = 0;
        vals[0].n = (uint32_t)aw.n;
        vals[0].p = aw.p;
        vals[0].off = 0;
        dyn_bson_w_reset(&r->w);
        dyn_bson_w_doc_begin(&r->w);
        write_tree(&r->w, &s->tree, doc, vals, &r->x);
        dyn_bson_w_doc_end(&r->w);
        rc = emit_w(r, out, &r->w);
    }
    free(kc.offs);
    free(next);
    free(chain_doc);
    free(head);
    free(hits);
    dyn_bson_w_free(&aw);
    dd_kmap_free(&heads);
    return rc;
}

static int run_from(dd_agg* a, int from, dd_rows** io, ag_run* r);

static int st_facet(ag_stage* s, dd_rows* in, dd_rows* out, ag_run* r)
{
    dyn_bson_w w;
    int f, rc = 0;
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    for (f = 0; f < s->nfacets && rc == 0; f++) {
        dd_rows* sub = rows_new();
        uint32_t i;
        if (!sub || (in->n && !(sub->p = malloc((size_t)in->n * sizeof *sub->p)))) {
            dd_rows_free(sub);
            rc = oom(r);
            break;
        }
        if (in->n)
            memcpy(sub->p, in->p, (size_t)in->n * sizeof *sub->p);
        sub->n = sub->cap = in->n;
        if (run_from(s->facets[f], 0, &sub, r) < 0) {
            dd_rows_free(sub);
            rc = -1;
            break;
        }
        dyn_bson_w_sub_begin(&w, DYN_BSON_ARRAY, s->fnames[f], s->fnlens[f]);
        for (i = 0; i < sub->n; i++) {
            dyn_bson_w_index_key(&w, DYN_BSON_DOC, i);
            dyn_bson_w_bytes(&w, sub->p[i], dyn_bson_rd32(sub->p[i]));
        }
        dyn_bson_w_doc_end(&w);
        dd_rows_free(sub);
        if (w.n > 16u * 1024 * 1024)
            rc = dd_fail(r->e, DD_E_TOOLARGE, "$facet produced a document larger than 16MB");
    }
    dyn_bson_w_doc_end(&w);
    if (rc == 0)
        rc = emit_w(r, out, &w);
    dyn_bson_w_free(&w);
    return rc;
}

static int run_from(dd_agg* a, int from, dd_rows** io, ag_run* r)
{
    int i;
    for (i = from; i < a->n; i++) {
        ag_stage* s = &a->st[i];
        dd_rows* in = *io;
        dd_rows* out = rows_new();
        int rc, fresh = 1;
        if (!out)
            return oom(r);
        switch (s->kind) {
        case S_MATCH:
            rc = st_match(s, in, out, r);
            fresh = 0;
            break;
        case S_PROJECT:
        case S_ADDFIELDS:
        case S_UNSET:
            rc = st_project(s, in, out, r);
            break;
        case S_REPLACEROOT:
            rc = st_replaceroot(s, in, out, r);
            break;
        case S_UNWIND:
            if (i + 1 < a->n && a->st[i + 1].kind == S_GROUP && !s->has_index) {
                rc = st_unwind_group(s, &a->st[i + 1], in, out, r);
                i++;
            } else
                rc = st_unwind(s, in, out, r);
            break;
        case S_GROUP:
            rc = st_group(s, in, out, r);
            break;
        case S_SORT:
            rc = st_sort(s, in, out, r);
            fresh = 0;
            break;
        case S_LIMIT:
        case S_SKIP:
            rc = st_slice(s, in, out, r);
            fresh = 0;
            break;
        case S_COUNT:
            rc = st_count(s, in, out, r);
            break;
        case S_LOOKUP:
            rc = st_lookup(s, in, out, r);
            break;
        case S_FACET:
            rc = st_facet(s, in, out, r);
            break;
        default:
            rc = dd_fail(r->e, DD_E_INTERNAL, "unknown stage kind %d", s->kind);
            break;
        }
        if (rc < 0) {
            dd_rows_free(out);
            return -1;
        }
        if (!fresh)
            rows_adopt(out, in);
        dd_rows_free(in);
        *io = out;
    }
    return 0;
}

int dd_agg_stream_ok(const dd_agg* a)
{
    int i;
    for (i = 0; i < a->n; i++)
        switch (a->st[i].kind) {
        case S_MATCH:
        case S_PROJECT:
        case S_ADDFIELDS:
        case S_UNSET:
        case S_REPLACEROOT:
            break;
        default:
            return 0;
        }
    return 1;
}

static int agg_exec(dd_agg* a, int from, dd_rows* rows, const dd_agg_source* srcs, int nsrc, int64_t now_ms,
    dd_rows** out, dd_err* e)
{
    ag_run r;
    int rc;
    memset(&r, 0, sizeof r);
    r.srcs = srcs;
    r.nsrc = nsrc;
    r.e = e;
    dd_xctx_init(&r.x, now_ms, e);
    dyn_bson_w_init(&r.w);
    dyn_bson_w_init(&r.w2);
    dyn_ks_init(&r.kb);
    rc = run_from(a, from, &rows, &r);
    dd_xctx_free(&r.x);
    dyn_bson_w_free(&r.w);
    dyn_bson_w_free(&r.w2);
    dyn_ks_free(&r.kb);
    if (rc < 0) {
        dd_rows_free(rows);
        return -1;
    }
    *out = rows;
    return 0;
}

int dd_agg_run(dd_agg* a, int whole, const uint8_t* const* input, uint32_t ninput, const dd_agg_source* srcs, int nsrc,
    int64_t now_ms, dd_rows** out, dd_err* e)
{
    dd_rows* rows = rows_new();
    uint32_t i;
    *out = NULL;
    if (!rows)
        return dd_fail(e, DD_E_OOM, "out of memory");
    for (i = 0; i < ninput; i++)
        if (input[i] && rows_push(rows, input[i]) < 0) {
            dd_rows_free(rows);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
    return agg_exec(a, whole ? 0 : a->prefix, rows, srcs, nsrc, now_ms, out, e);
}

int dd_agg_count_ok(const dd_agg* a)
{
    const ag_stage* s;
    dyn_bson_val v;
    int k;
    if (a->prefix >= a->n)
        return 0;
    s = &a->st[a->prefix];
    if (s->kind == S_COUNT)
        return 1;
    if (s->kind != S_GROUP || s->bucket || s->nacc < 1 || !dd_expr_lit(s->expr, &v) || v.type != DYN_BSON_NULL)
        return 0;
    for (k = 0; k < s->nacc; k++)
        if (s->acc[k].op != A_SUM || !dd_expr_lit(s->acc[k].arg, &v) || v.type != DYN_BSON_INT32)
            return 0;
    return 1;
}

int dd_agg_run_count(dd_agg* a, uint32_t n, const dd_agg_source* srcs, int nsrc, int64_t now_ms, dd_rows** out,
    dd_err* e)
{
    dd_rows* rows = rows_new();
    const ag_stage* s = &a->st[a->prefix];
    dyn_bson_w w;
    int k;
    *out = NULL;
    if (!rows)
        return dd_fail(e, DD_E_OOM, "out of memory");
    if (n) {
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        if (s->kind == S_COUNT) {
            dyn_bson_w_int32(&w, s->name, s->name_n, (int32_t)n);
        } else {
            dyn_bson_w_null(&w, "_id", 3);
            for (k = 0; k < s->nacc; k++) {
                dyn_bson_val v;
                int64_t t;
                dd_expr_lit(s->acc[k].arg, &v);
                t = (int64_t)(int32_t)dyn_bson_rd32(v.val) * (int64_t)n;
                if (t > INT32_MAX || t < INT32_MIN)
                    dyn_bson_w_int64(&w, s->acc[k].name, s->acc[k].nlen, t);
                else
                    dyn_bson_w_int32(&w, s->acc[k].name, s->acc[k].nlen, (int32_t)t);
            }
        }
        dyn_bson_w_doc_end(&w);
        if (w.oom || rows_push_copy(rows, w.p, w.n) < 0) {
            dyn_bson_w_free(&w);
            dd_rows_free(rows);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
        dyn_bson_w_free(&w);
    }
    return agg_exec(a, a->prefix + 1, rows, srcs, nsrc, now_ms, out, e);
}
