#include <stdio.h>
#include <stdlib.h>
#include "dd.h"

enum {
    UOP_SET = 1,
    UOP_UNSET,
    UOP_INC,
    UOP_MUL,
    UOP_MIN,
    UOP_MAX,
    UOP_RENAME,
    UOP_PUSH,
    UOP_ADDTOSET,
    UOP_PULL,
    UOP_POP,
    UOP_CURRENTDATE,
    UOP_SETONINSERT,
    UOP_PULLALL,
};

typedef struct {
    int op;
    dd_path path;
    dd_path to;
    dyn_bson_val arg;
    dyn_bson_val each;
    int has_each;
    int64_t position;
    int has_position;
    int64_t slice;
    int has_slice;
    dyn_bson_val sort;
    int has_sort;
    dd_filter* pull;
    int pull_wrap;
} uop;

struct dd_upd {
    uint8_t* src;
    int replacement;
    uop* ops;
    int nops;
};

typedef struct un {
    uint8_t type;
    const char* key;
    uint32_t klen;
    char* okey;
    const uint8_t* raw;
    uint32_t rawlen;
    uint8_t* own;
    int expanded;
    struct un* kids;
    uint32_t nk, ck;
} un;

static const struct {
    const char* name;
    int op;
} uop_names[] = {
    { "$set", UOP_SET },
    { "$unset", UOP_UNSET },
    { "$inc", UOP_INC },
    { "$mul", UOP_MUL },
    { "$min", UOP_MIN },
    { "$max", UOP_MAX },
    { "$rename", UOP_RENAME },
    { "$push", UOP_PUSH },
    { "$addToSet", UOP_ADDTOSET },
    { "$pull", UOP_PULL },
    { "$pop", UOP_POP },
    { "$currentDate", UOP_CURRENTDATE },
    { "$setOnInsert", UOP_SETONINSERT },
    { "$pullAll", UOP_PULLALL },
};

static void uop_free(uop* o)
{
    dd_path_free(&o->path);
    dd_path_free(&o->to);
    if (o->pull) {
        dd_filter_free(o->pull);
        free(o->pull);
    }
}

void dd_update_free(dd_upd* u)
{
    int i;
    if (!u)
        return;
    for (i = 0; i < u->nops; i++)
        uop_free(&u->ops[i]);
    free(u->ops);
    free(u->src);
    free(u);
}

int dd_update_is_replacement(const dd_upd* u)
{
    return u->replacement;
}

static int path_prefix_conflict(const dd_path* a, const dd_path* b)
{
    int i, n = a->n < b->n ? a->n : b->n;
    for (i = 0; i < n; i++)
        if (a->plen[i] != b->plen[i] || memcmp(a->part[i], b->part[i], a->plen[i]) != 0)
            return 0;
    return 1;
}

static int is_number_type(uint8_t t)
{
    return dyn_bson_is_number(t);
}

static int parse_push_mods(uop* o, dyn_bson_val v, dd_err* e)
{
    dyn_bson_iter it;
    int has_mod = 0;
    if (v.type != DYN_BSON_DOC)
        return 0;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    if (dyn_bson_iter_next(&it) <= 0 || it.klen < 1 || it.key[0] != '$')
        return 0;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val a = dyn_bson_iter_val(&it);
        has_mod = 1;
        if (it.klen == 5 && memcmp(it.key, "$each", 5) == 0) {
            if (a.type != DYN_BSON_ARRAY)
                return dd_fail(e, DD_E_BADVALUE, "The argument to $each in %s must be an array but it was of type %d",
                    o->op == UOP_PUSH ? "$push" : "$addToSet", a.type);
            o->each = a;
            o->has_each = 1;
        } else if (o->op == UOP_PUSH && it.klen == 9 && memcmp(it.key, "$position", 9) == 0) {
            if (!is_number_type(a.type) || dyn_bson_num_f64(a) != (double)(int64_t)dyn_bson_num_f64(a))
                return dd_fail(e, DD_E_BADVALUE, "The value for $position must be an integer value");
            o->position = (int64_t)dyn_bson_num_f64(a);
            o->has_position = 1;
        } else if (o->op == UOP_PUSH && it.klen == 6 && memcmp(it.key, "$slice", 6) == 0) {
            if (!is_number_type(a.type) || dyn_bson_num_f64(a) != (double)(int64_t)dyn_bson_num_f64(a))
                return dd_fail(e, DD_E_BADVALUE, "The value for $slice must be an integer value");
            o->slice = (int64_t)dyn_bson_num_f64(a);
            o->has_slice = 1;
        } else if (o->op == UOP_PUSH && it.klen == 5 && memcmp(it.key, "$sort", 5) == 0) {
            if (!(is_number_type(a.type) && (dyn_bson_num_f64(a) == 1 || dyn_bson_num_f64(a) == -1))
                && a.type != DYN_BSON_DOC)
                return dd_fail(e, DD_E_BADVALUE, "The $sort is invalid: use 1/-1 to sort the whole element, or {field:1/-1}");
            o->sort = a;
            o->has_sort = 1;
        } else {
            return dd_fail(e, DD_E_BADVALUE, "Unrecognized clause in %s: %.*s", o->op == UOP_PUSH ? "$push" : "$addToSet",
                (int)it.klen, it.key);
        }
    }
    if (has_mod && !o->has_each)
        return dd_fail(e, DD_E_BADVALUE, "%s modifiers need $each", o->op == UOP_PUSH ? "$push" : "$addToSet");
    return 0;
}

static int compile_pull(uop* o, dyn_bson_val v, dd_err* e)
{
    dyn_bson_iter it;
    dyn_bson_w w;
    int ops = 0, rc;
    if (v.type != DYN_BSON_DOC)
        return 0;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    if (dyn_bson_iter_next(&it) > 0 && it.klen > 0 && it.key[0] == '$')
        ops = 1;
    o->pull = calloc(1, sizeof *o->pull);
    if (!o->pull)
        return dd_fail(e, DD_E_OOM, "out of memory");
    dyn_bson_w_init(&w);
    if (ops) {
        dyn_bson_w_doc_begin(&w);
        dyn_bson_w_value(&w, "v", 1, v);
        dyn_bson_w_doc_end(&w);
        o->pull_wrap = 1;
    } else {
        dyn_bson_w_bytes(&w, v.val, v.vlen);
    }
    if (w.oom) {
        dyn_bson_w_free(&w);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    rc = dd_filter_compile(o->pull, w.p, NULL, e);
    dyn_bson_w_free(&w);
    if (rc < 0) {
        free(o->pull);
        o->pull = NULL;
    }
    return rc;
}

static int op_cmp(const void* a, const void* b)
{
    const uop* x = a;
    const uop* y = b;
    const dd_path* px = x->op == UOP_RENAME ? &x->to : &x->path;
    const dd_path* py = y->op == UOP_RENAME ? &y->to : &y->path;
    size_t m = px->tlen < py->tlen ? px->tlen : py->tlen;
    int c = memcmp(px->text, py->text, m);
    if (c)
        return c;
    return px->tlen < py->tlen ? -1 : px->tlen > py->tlen;
}

static int check_path_syntax(const dd_path* p, dd_err* e)
{
    int i;
    for (i = 0; i < p->n; i++) {
        if (p->part[i][0] == '$') {
            if (p->plen[i] == 1 || (p->plen[i] == 3 && memcmp(p->part[i], "$[]", 3) == 0))
                continue;
            if (p->plen[i] >= 2 && p->part[i][1] == '[')
                return dd_fail(e, DD_E_BADVALUE, "filtered positional operators ($[<id>]) are not supported: '%s'", p->text);
            return dd_fail(e, DD_E_BADVALUE, "The dollar ($) prefixed field '%.*s' in '%s' is not allowed in the context of an update",
                (int)p->plen[i], p->part[i], p->text);
        }
    }
    return 0;
}

int dd_update_parse(dd_upd** out, const uint8_t* update, const uint8_t* array_filters, dd_err* e)
{
    dd_upd* u;
    dyn_bson_iter it;
    uint32_t n = dyn_bson_rd32(update);
    int i, j, nops = 0;
    *out = NULL;
    if (array_filters)
        return dd_fail(e, DD_E_BADVALUE, "arrayFilters are not supported");
    u = calloc(1, sizeof *u);
    if (!u || !(u->src = malloc(n))) {
        free(u);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    memcpy(u->src, update, n);
    dyn_bson_iter_init(&it, u->src, n);
    if (dyn_bson_iter_next(&it) <= 0) {
        {
            int rf = dd_fail(e, DD_E_FAILEDTOPARSE, "Update document requires at least one update operator");
            dd_update_free(u);
            return rf;
        }
    }
    if (it.klen == 0 || it.key[0] != '$') {
        u->replacement = 1;
        dyn_bson_iter_init(&it, u->src, n);
        while (dyn_bson_iter_next(&it) > 0)
            if (it.klen > 0 && it.key[0] == '$') {
                {
                    int rf = dd_fail(e, DD_E_BADVALUE, "a replacement document cannot contain $ operators: %.*s", (int)it.klen, it.key);
                    dd_update_free(u);
                    return rf;
                }
            }
        *out = u;
        return 0;
    }
    dyn_bson_iter_init(&it, u->src, n);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_iter fi;
        if (it.type != DYN_BSON_DOC) {
            {
                int rf = dd_fail(e, DD_E_FAILEDTOPARSE, "Modifiers operate on fields but we found type %d instead for %.*s", it.type, (int)it.klen, it.key);
                dd_update_free(u);
                return rf;
            }
        }
        dyn_bson_iter_init(&fi, it.val, dyn_bson_rd32(it.val));
        while (dyn_bson_iter_next(&fi) > 0)
            nops++;
    }
    u->ops = calloc((size_t)(nops ? nops : 1), sizeof *u->ops);
    if (!u->ops) {
        {
            int rf = dd_fail(e, DD_E_OOM, "out of memory");
            dd_update_free(u);
            return rf;
        }
    }
    dyn_bson_iter_init(&it, u->src, n);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_iter fi;
        int op = 0;
        size_t k;
        for (k = 0; k < sizeof uop_names / sizeof uop_names[0]; k++)
            if (strlen(uop_names[k].name) == it.klen && memcmp(uop_names[k].name, it.key, it.klen) == 0)
                op = uop_names[k].op;
        if (!op) {
            {
                int rf = dd_fail(e, DD_E_FAILEDTOPARSE, "Unknown modifier: %.*s. Expected a valid update modifier or pipeline-style update specified as an array", (int)it.klen, it.key);
                dd_update_free(u);
                return rf;
            }
        }
        dyn_bson_iter_init(&fi, it.val, dyn_bson_rd32(it.val));
        while (dyn_bson_iter_next(&fi) > 0) {
            uop* o = &u->ops[u->nops];
            dyn_bson_val a = dyn_bson_iter_val(&fi);
            o->op = op;
            o->arg = a;
            if (dd_path_parse(&o->path, fi.key, fi.klen, e) < 0) {
                dd_update_free(u);
                return -1;
            }
            u->nops++;
            if (check_path_syntax(&o->path, e) < 0) {
                dd_update_free(u);
                return -1;
            }
            switch (op) {
            case UOP_INC:
            case UOP_MUL:
                if (!is_number_type(a.type)) {
                    {
                        int rf = dd_fail(e, DD_E_TYPEMISMATCH, "Cannot %s with non-numeric argument: {%.*s: ...}", op == UOP_INC ? "increment" : "multiply", (int)fi.klen, fi.key);
                        dd_update_free(u);
                        return rf;
                    }
                }
                break;
            case UOP_RENAME:
                if (a.type != DYN_BSON_STRING) {
                    {
                        int rf = dd_fail(e, DD_E_BADVALUE, "The 'to' field for $rename must be a string: %.*s", (int)fi.klen, fi.key);
                        dd_update_free(u);
                        return rf;
                    }
                }
                if (dd_path_parse(&o->to, (const char*)a.val + 4, dyn_bson_rd32(a.val) - 1, e) < 0
                    || check_path_syntax(&o->to, e) < 0) {
                    dd_update_free(u);
                    return -1;
                }
                if (path_prefix_conflict(&o->path, &o->to)) {
                    {
                        int rf = dd_fail(e, DD_E_BADVALUE, "The source and target field for $rename must not be on the same path: %s", o->path.text);
                        dd_update_free(u);
                        return rf;
                    }
                }
                break;
            case UOP_PUSH:
            case UOP_ADDTOSET:
                if (parse_push_mods(o, a, e) < 0) {
                    dd_update_free(u);
                    return -1;
                }
                break;
            case UOP_PULL:
                if (compile_pull(o, a, e) < 0) {
                    dd_update_free(u);
                    return -1;
                }
                break;
            case UOP_PULLALL:
                if (a.type != DYN_BSON_ARRAY) {
                    {
                        int rf = dd_fail(e, DD_E_BADVALUE, "$pullAll requires an array argument");
                        dd_update_free(u);
                        return rf;
                    }
                }
                break;
            case UOP_POP:
                if (!is_number_type(a.type) || (dyn_bson_num_f64(a) != 1 && dyn_bson_num_f64(a) != -1)) {
                    {
                        int rf = dd_fail(e, DD_E_FAILEDTOPARSE, "$pop expects 1 or -1");
                        dd_update_free(u);
                        return rf;
                    }
                }
                break;
            case UOP_CURRENTDATE:
                if (a.type != DYN_BSON_BOOL && a.type != DYN_BSON_DOC) {
                    {
                        int rf = dd_fail(e, DD_E_BADVALUE, "$currentDate expects true or {$type: 'date'|'timestamp'}");
                        dd_update_free(u);
                        return rf;
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    if (u->nops == 0) {
        {
            int rf = dd_fail(e, DD_E_FAILEDTOPARSE, "Update document requires at least one update operator");
            dd_update_free(u);
            return rf;
        }
    }
    for (i = 0; i < u->nops; i++) {
        for (j = i + 1; j < u->nops; j++) {
            const dd_path* pi[2] = { &u->ops[i].path, u->ops[i].op == UOP_RENAME ? &u->ops[i].to : NULL };
            const dd_path* pj[2] = { &u->ops[j].path, u->ops[j].op == UOP_RENAME ? &u->ops[j].to : NULL };
            int a, b;
            for (a = 0; a < 2; a++)
                for (b = 0; b < 2; b++)
                    if (pi[a] && pj[b] && path_prefix_conflict(pi[a], pj[b])) {
                        char m[300];
                        snprintf(m, sizeof m, "Updating the path '%s' would create a conflict at '%s'", pj[b]->text,
                            pi[a]->n < pj[b]->n ? pi[a]->text : pj[b]->text);
                        {
                            int rf = dd_fail(e, DD_E_CONFLICTINGUPDATE, "%s", m);
                            dd_update_free(u);
                            return rf;
                        }
                    }
        }
    }
    qsort(u->ops, (size_t)u->nops, sizeof *u->ops, op_cmp);
    *out = u;
    return 0;
}

static void un_free(un* n)
{
    uint32_t i;
    for (i = 0; i < n->nk; i++)
        un_free(&n->kids[i]);
    free(n->kids);
    free(n->okey);
    free(n->own);
}

static int un_expand(un* n)
{
    dyn_bson_iter it;
    uint32_t cnt = 0;
    if (n->expanded)
        return 0;
    dyn_bson_iter_init(&it, n->raw, n->rawlen);
    while (dyn_bson_iter_next(&it) > 0)
        cnt++;
    n->kids = calloc(cnt ? cnt : 1, sizeof *n->kids);
    if (!n->kids)
        return -1;
    n->ck = cnt ? cnt : 1;
    dyn_bson_iter_init(&it, n->raw, n->rawlen);
    while (dyn_bson_iter_next(&it) > 0) {
        un* k = &n->kids[n->nk++];
        dyn_bson_val v = dyn_bson_iter_val(&it);
        k->type = v.type;
        k->key = it.key;
        k->klen = (uint32_t)it.klen;
        k->raw = v.val;
        k->rawlen = v.vlen;
    }
    n->expanded = 1;
    return 0;
}

static int un_reserve(un* n, uint32_t extra)
{
    if (n->nk + extra > n->ck) {
        uint32_t nc = n->ck ? n->ck : 4;
        un* nk;
        while (nc < n->nk + extra)
            nc *= 2;
        nk = realloc(n->kids, (size_t)nc * sizeof *nk);
        if (!nk)
            return -1;
        n->kids = nk;
        n->ck = nc;
    }
    return 0;
}

static void un_clear_value(un* n)
{
    uint32_t i;
    for (i = 0; i < n->nk; i++)
        un_free(&n->kids[i]);
    free(n->kids);
    n->kids = NULL;
    n->nk = n->ck = 0;
    n->expanded = 0;
    free(n->own);
    n->own = NULL;
    n->raw = NULL;
    n->rawlen = 0;
}

static int un_set_val(un* n, dyn_bson_val v, int copy)
{
    un_clear_value(n);
    n->type = v.type;
    if (copy && v.vlen) {
        n->own = malloc(v.vlen);
        if (!n->own)
            return -1;
        memcpy(n->own, v.val, v.vlen);
        n->raw = n->own;
    } else {
        n->raw = v.val;
    }
    n->rawlen = v.vlen;
    if (v.type == DYN_BSON_DOC || v.type == DYN_BSON_ARRAY)
        return un_expand(n);
    return 0;
}

static int un_set_bytes(un* n, uint8_t type, const void* p, uint32_t len)
{
    dyn_bson_val v;
    v.type = type;
    v.val = p;
    v.vlen = len;
    return un_set_val(n, v, 1);
}

static un* un_find(un* n, const char* k, uint32_t kl)
{
    uint32_t i;
    for (i = 0; i < n->nk; i++)
        if (n->kids[i].klen == kl && memcmp(n->kids[i].key, k, kl) == 0)
            return &n->kids[i];
    return NULL;
}

static un* un_append(un* n, const char* k, uint32_t kl)
{
    un* c;
    if (un_reserve(n, 1) < 0)
        return NULL;
    c = &n->kids[n->nk++];
    memset(c, 0, sizeof *c);
    if (k) {
        c->okey = malloc(kl + 1u);
        if (!c->okey) {
            n->nk--;
            return NULL;
        }
        memcpy(c->okey, k, kl);
        c->okey[kl] = 0;
        c->key = c->okey;
        c->klen = kl;
    }
    c->type = DYN_BSON_NULL;
    return c;
}

static void un_remove_at(un* n, uint32_t i)
{
    un_free(&n->kids[i]);
    memmove(&n->kids[i], &n->kids[i + 1], (size_t)(n->nk - i - 1) * sizeof *n->kids);
    n->nk--;
}

static void un_write(const un* n, dyn_bson_w* w);

static void un_write_kids(const un* n, dyn_bson_w* w)
{
    uint32_t i;
    for (i = 0; i < n->nk; i++) {
        const un* k = &n->kids[i];
        if (n->type == DYN_BSON_ARRAY)
            dyn_bson_w_index_key(w, k->type, i);
        else
            dyn_bson_w_key(w, k->type, k->key, k->klen);
        un_write(k, w);
    }
}

static void un_write(const un* n, dyn_bson_w* w)
{
    if ((n->type == DYN_BSON_DOC || n->type == DYN_BSON_ARRAY) && n->expanded) {
        dyn_bson_w_doc_begin(w);
        un_write_kids(n, w);
        dyn_bson_w_doc_end(w);
    } else {
        dyn_bson_w_bytes(w, n->raw, n->rawlen);
    }
}

static int un_to_val(const un* n, dyn_bson_w* scratch, dyn_bson_val* out)
{
    dyn_bson_w_reset(scratch);
    un_write(n, scratch);
    if (scratch->oom)
        return -1;
    out->type = n->type;
    out->val = scratch->p;
    out->vlen = (uint32_t)scratch->n;
    return 0;
}

typedef struct {
    int32_t pos;
    int is_insert;
    int64_t now;
    dd_err* e;
    dd_upd_desc* desc;
    dyn_bson_w scratch;
    char rp[1024];
    size_t rpn;
} uctx;

static void rp_push(uctx* c, const char* s, size_t n, size_t* save)
{
    *save = c->rpn;
    if (c->rpn + n + 2 >= sizeof c->rp) {
        c->rpn = sizeof c->rp - 1;
        return;
    }
    if (c->rpn)
        c->rp[c->rpn++] = '.';
    memcpy(c->rp + c->rpn, s, n);
    c->rpn += n;
}

static void desc_mark(uctx* c, int removed, const un* node)
{
    dyn_bson_val v;
    if (!c->desc)
        return;
    if (removed) {
        dyn_bson_w* r = &c->desc->removed;
        if (r->n == 0)
            dyn_bson_w_doc_begin(r);
        dyn_bson_w_index_key(r, DYN_BSON_STRING, (uint32_t)c->desc->n_removed);
        {
            uint8_t l[4];
            dyn_bson_wr32(l, (uint32_t)c->rpn + 1);
            dyn_bson_w_bytes(r, l, 4);
            dyn_bson_w_bytes(r, c->rp, c->rpn);
            dyn_bson_w_bytes(r, "", 1);
        }
        c->desc->n_removed++;
        return;
    }
    if (c->desc->updated.n == 0)
        dyn_bson_w_doc_begin(&c->desc->updated);
    if (node && un_to_val(node, &c->scratch, &v) == 0)
        dyn_bson_w_value(&c->desc->updated, c->rp, c->rpn, v);
    c->desc->n_updated++;
}

static const char* type_name(uint8_t t)
{
    switch (t) {
    case DYN_BSON_DOUBLE:
        return "double";
    case DYN_BSON_STRING:
        return "string";
    case DYN_BSON_DOC:
        return "object";
    case DYN_BSON_ARRAY:
        return "array";
    case DYN_BSON_BOOL:
        return "bool";
    case DYN_BSON_NULL:
        return "null";
    case DYN_BSON_INT32:
        return "int";
    case DYN_BSON_INT64:
        return "long";
    case DYN_BSON_DATE:
        return "date";
    case DYN_BSON_OID:
        return "objectId";
    default:
        return "other";
    }
}

static int num_arith(uctx* c, un* n, dyn_bson_val a, int mul, const char* path)
{
    dyn_bson_val cur;
    uint8_t buf[8];
    cur.type = n->type;
    cur.val = n->raw;
    cur.vlen = n->rawlen;
    if (!is_number_type(cur.type))
        return dd_fail(c->e, DD_E_TYPEMISMATCH, "Cannot apply %s to a value of non-numeric type. {_id: ...} has the field '%s' of non-numeric type %s",
            mul ? "$mul" : "$inc", path, type_name(cur.type));
    if (cur.type == DYN_BSON_DEC128 || a.type == DYN_BSON_DEC128) {
        double x = dyn_bson_num_f64(cur), y = dyn_bson_num_f64(a), r = mul ? x * y : x + y;
        uint64_t u;
        memcpy(&u, &r, 8);
        dyn_bson_wr64(buf, u);
        return un_set_bytes(n, DYN_BSON_DOUBLE, buf, 8);
    }
    if (cur.type == DYN_BSON_DOUBLE || a.type == DYN_BSON_DOUBLE) {
        double r = mul ? dyn_bson_num_f64(cur) * dyn_bson_num_f64(a) : dyn_bson_num_f64(cur) + dyn_bson_num_f64(a);
        uint64_t u;
        memcpy(&u, &r, 8);
        dyn_bson_wr64(buf, u);
        return un_set_bytes(n, DYN_BSON_DOUBLE, buf, 8);
    }
    {
        int64_t x = cur.type == DYN_BSON_INT32 ? (int32_t)dyn_bson_rd32(cur.val) : (int64_t)dyn_bson_rd64(cur.val);
        int64_t y = a.type == DYN_BSON_INT32 ? (int32_t)dyn_bson_rd32(a.val) : (int64_t)dyn_bson_rd64(a.val);
        int64_t r;
        int ovf = mul ? __builtin_mul_overflow(x, y, &r) : __builtin_add_overflow(x, y, &r);
        if (ovf) {
            double d = mul ? (double)x * (double)y : (double)x + (double)y;
            uint64_t u;
            memcpy(&u, &d, 8);
            dyn_bson_wr64(buf, u);
            return un_set_bytes(n, DYN_BSON_DOUBLE, buf, 8);
        }
        if (cur.type == DYN_BSON_INT32 && a.type == DYN_BSON_INT32 && r >= INT32_MIN && r <= INT32_MAX) {
            dyn_bson_wr32(buf, (uint32_t)(int32_t)r);
            return un_set_bytes(n, DYN_BSON_INT32, buf, 4);
        }
        dyn_bson_wr64(buf, (uint64_t)r);
        return un_set_bytes(n, DYN_BSON_INT64, buf, 8);
    }
}

static int bson_eq(dyn_bson_val a, dyn_bson_val b)
{
    if (dyn_bson_canon_rank(a.type) != dyn_bson_canon_rank(b.type))
        return 0;
    return dyn_bson_cmp(a, b) == 0;
}

static int un_val(const un* n, uctx* c, dyn_bson_val* v)
{
    if (n->expanded)
        return un_to_val(n, &c->scratch, v);
    v->type = n->type;
    v->val = n->raw;
    v->vlen = n->rawlen;
    return 0;
}

static int elem_sort_cmp(const un* a, const un* b, dyn_bson_val spec, uctx* c)
{
    dyn_bson_w s2;
    dyn_bson_val va, vb;
    int r;
    dyn_bson_w_init(&s2);
    if (un_val(a, c, &va) < 0)
        return 0;
    if (va.val == c->scratch.p) {
        dyn_bson_w_bytes(&s2, va.val, va.vlen);
        va.val = s2.p;
    }
    if (un_val(b, c, &vb) < 0) {
        dyn_bson_w_free(&s2);
        return 0;
    }
    if (spec.type == DYN_BSON_DOC) {
        dyn_bson_iter it;
        r = 0;
        dyn_bson_iter_init(&it, spec.val, spec.vlen);
        while (r == 0 && dyn_bson_iter_next(&it) > 0) {
            dyn_bson_val fa = { 0, NULL, 0 }, fb = { 0, NULL, 0 };
            int dir = dyn_bson_num_f64(dyn_bson_iter_val(&it)) < 0 ? -1 : 1;
            if (va.type == DYN_BSON_DOC)
                dd_lookup_dotted(va.val, it.key, it.klen, &fa);
            if (vb.type == DYN_BSON_DOC)
                dd_lookup_dotted(vb.val, it.key, it.klen, &fb);
            if (fa.type == 0)
                fa.type = DYN_BSON_NULL;
            if (fb.type == 0)
                fb.type = DYN_BSON_NULL;
            r = dyn_bson_cmp(fa, fb) * dir;
        }
    } else {
        r = dyn_bson_cmp(va, vb) * (dyn_bson_num_f64(spec) < 0 ? -1 : 1);
    }
    dyn_bson_w_free(&s2);
    return r;
}

static int push_apply(uctx* c, const uop* o, un* arr)
{
    dyn_bson_iter it;
    uint32_t i, j, nadd = 0, at;
    if (o->has_each) {
        dyn_bson_iter_init(&it, o->each.val, o->each.vlen);
        while (dyn_bson_iter_next(&it) > 0)
            nadd++;
    } else {
        nadd = 1;
    }
    if (un_reserve(arr, nadd) < 0)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    at = arr->nk;
    if (o->has_position) {
        int64_t p = o->position;
        if (p < 0)
            p = (int64_t)arr->nk + p < 0 ? 0 : (int64_t)arr->nk + p;
        at = p > (int64_t)arr->nk ? arr->nk : (uint32_t)p;
    }
    memmove(&arr->kids[at + nadd], &arr->kids[at], (size_t)(arr->nk - at) * sizeof *arr->kids);
    arr->nk += nadd;
    for (i = 0; i < nadd; i++)
        memset(&arr->kids[at + i], 0, sizeof arr->kids[0]);
    if (o->has_each) {
        i = 0;
        dyn_bson_iter_init(&it, o->each.val, o->each.vlen);
        while (dyn_bson_iter_next(&it) > 0)
            if (un_set_val(&arr->kids[at + i++], dyn_bson_iter_val(&it), 0) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
    } else if (un_set_val(&arr->kids[at], o->arg, 0) < 0) {
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    }
    if (o->has_sort) {
        for (i = 1; i < arr->nk; i++) {
            un x = arr->kids[i];
            for (j = i; j > 0 && elem_sort_cmp(&arr->kids[j - 1], &x, o->sort, c) > 0; j--)
                arr->kids[j] = arr->kids[j - 1];
            arr->kids[j] = x;
        }
    }
    if (o->has_slice) {
        if (o->slice >= 0) {
            while ((int64_t)arr->nk > o->slice)
                un_remove_at(arr, arr->nk - 1);
        } else {
            while ((int64_t)arr->nk > -o->slice)
                un_remove_at(arr, 0);
        }
    }
    return 0;
}

static int addtoset_one(uctx* c, un* arr, dyn_bson_val v)
{
    uint32_t i;
    for (i = 0; i < arr->nk; i++) {
        dyn_bson_val cur;
        if (un_val(&arr->kids[i], c, &cur) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        if (bson_eq(cur, v))
            return 0;
    }
    {
        un* k = un_append(arr, NULL, 0);
        if (!k || un_set_val(k, v, 0) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
    }
    return 0;
}

static int pull_match(uctx* c, const uop* o, dyn_bson_val v)
{
    if (o->op == UOP_PULLALL) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, o->arg.val, o->arg.vlen);
        while (dyn_bson_iter_next(&it) > 0)
            if (bson_eq(v, dyn_bson_iter_val(&it)))
                return 1;
        return 0;
    }
    if (o->pull) {
        dd_match_ctx mc = { .pos = -1, .err = c->e };
        if (o->pull_wrap) {
            dyn_bson_w w;
            int r;
            dyn_bson_w_init(&w);
            dyn_bson_w_doc_begin(&w);
            dyn_bson_w_value(&w, "v", 1, v);
            dyn_bson_w_doc_end(&w);
            r = !w.oom && dd_filter_match(o->pull, w.p, &mc);
            dyn_bson_w_free(&w);
            return r;
        }
        return v.type == DYN_BSON_DOC && dd_filter_match(o->pull, v.val, &mc);
    }
    return bson_eq(v, o->arg);
}

static int apply_leaf(uctx* c, const uop* o, un* parent, const char* k, uint32_t kl, int is_index, uint32_t idx)
{
    un* n = NULL;
    uint32_t pos = 0;
    int created = 0;
    if (is_index) {
        if (idx < parent->nk)
            n = &parent->kids[idx];
        pos = idx;
    } else {
        n = un_find(parent, k, kl);
        if (n)
            pos = (uint32_t)(n - parent->kids);
    }
    switch (o->op) {
    case UOP_UNSET:
        if (!n)
            return 0;
        if (parent->type == DYN_BSON_ARRAY)
            un_set_bytes(n, DYN_BSON_NULL, NULL, 0);
        else
            un_remove_at(parent, pos);
        desc_mark(c, 1, NULL);
        return 0;
    case UOP_POP:
        if (!n)
            return 0;
        if (n->type != DYN_BSON_ARRAY)
            return dd_fail(c->e, DD_E_TYPEMISMATCH, "Path '%.*s' contains an element of non-array type '%s'", (int)c->rpn,
                c->rp, type_name(n->type));
        if (un_expand(n) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        if (n->nk)
            un_remove_at(n, dyn_bson_num_f64(o->arg) < 0 ? 0 : n->nk - 1);
        desc_mark(c, 0, n);
        return 0;
    case UOP_PULL:
    case UOP_PULLALL: {
        uint32_t i;
        if (!n)
            return 0;
        if (n->type != DYN_BSON_ARRAY)
            return dd_fail(c->e, DD_E_BADVALUE, "Cannot apply $pull to a non-array value");
        if (un_expand(n) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        for (i = 0; i < n->nk;) {
            dyn_bson_val v;
            if (un_val(&n->kids[i], c, &v) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
            if (pull_match(c, o, v))
                un_remove_at(n, i);
            else
                i++;
        }
        desc_mark(c, 0, n);
        return 0;
    }
    case UOP_RENAME:
        return 0;
    default:
        break;
    }
    if (!n) {
        if (is_index) {
            while (parent->nk < idx) {
                un* pad = un_append(parent, NULL, 0);
                if (!pad)
                    return dd_fail(c->e, DD_E_OOM, "out of memory");
            }
            n = un_append(parent, NULL, 0);
        } else {
            n = un_append(parent, k, kl);
        }
        if (!n)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        n->type = 0;
        created = 1;
    }
    switch (o->op) {
    case UOP_SET:
    case UOP_SETONINSERT:
        if (un_set_val(n, o->arg, 0) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        break;
    case UOP_INC:
    case UOP_MUL:
        if (created) {
            if (o->op == UOP_INC) {
                if (un_set_val(n, o->arg, 0) < 0)
                    return dd_fail(c->e, DD_E_OOM, "out of memory");
            } else {
                uint8_t z[8] = { 0 };
                uint8_t t = o->arg.type == DYN_BSON_DEC128 ? DYN_BSON_DOUBLE : o->arg.type;
                if (un_set_bytes(n, t, z, t == DYN_BSON_INT32 ? 4 : 8) < 0)
                    return dd_fail(c->e, DD_E_OOM, "out of memory");
            }
        } else if (num_arith(c, n, o->arg, o->op == UOP_MUL, c->rp) < 0) {
            return -1;
        }
        break;
    case UOP_MIN:
    case UOP_MAX: {
        dyn_bson_val cur;
        int r;
        if (!created) {
            if (un_val(n, c, &cur) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
            r = dyn_bson_cmp(o->arg, cur);
            if ((o->op == UOP_MIN && r >= 0) || (o->op == UOP_MAX && r <= 0))
                return 0;
        }
        if (un_set_val(n, o->arg, 0) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        break;
    }
    case UOP_CURRENTDATE: {
        uint8_t b[8];
        int ts = 0;
        if (o->arg.type == DYN_BSON_DOC) {
            dyn_bson_val t;
            if (dyn_bson_find(o->arg.val, "$type", 5, &t) && t.type == DYN_BSON_STRING
                && strcmp((const char*)t.val + 4, "timestamp") == 0)
                ts = 1;
        }
        if (ts) {
            dyn_bson_wr32(b, 1);
            dyn_bson_wr32(b + 4, (uint32_t)(c->now / 1000));
            if (un_set_bytes(n, DYN_BSON_TIMESTAMP, b, 8) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
        } else {
            dyn_bson_wr64(b, (uint64_t)c->now);
            if (un_set_bytes(n, DYN_BSON_DATE, b, 8) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
        }
        break;
    }
    case UOP_PUSH:
    case UOP_ADDTOSET:
        if (created) {
            static const uint8_t empty[5] = { 5, 0, 0, 0, 0 };
            if (un_set_bytes(n, DYN_BSON_ARRAY, empty, 5) < 0)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
        } else if (n->type != DYN_BSON_ARRAY) {
            return dd_fail(c->e, DD_E_BADVALUE, "The field '%.*s' must be an array but is of type %s in document {_id: ...}",
                (int)c->rpn, c->rp, type_name(n->type));
        }
        if (un_expand(n) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        if (o->op == UOP_PUSH) {
            if (push_apply(c, o, n) < 0)
                return -1;
        } else if (o->has_each) {
            dyn_bson_iter it;
            dyn_bson_iter_init(&it, o->each.val, o->each.vlen);
            while (dyn_bson_iter_next(&it) > 0)
                if (addtoset_one(c, n, dyn_bson_iter_val(&it)) < 0)
                    return -1;
        } else if (addtoset_one(c, n, o->arg) < 0) {
            return -1;
        }
        break;
    default:
        break;
    }
    desc_mark(c, 0, n);
    return 0;
}

static int apply_path(uctx* c, const uop* o, const dd_path* p, int i, un* node);

static int descend(uctx* c, const uop* o, const dd_path* p, int i, un* node)
{
    const char* k = p->part[i];
    uint32_t kl = p->plen[i];
    size_t save;
    int rc;
    if (kl == 3 && memcmp(k, "$[]", 3) == 0) {
        uint32_t j;
        if (node->type != DYN_BSON_ARRAY)
            return dd_fail(c->e, DD_E_BADVALUE, "Cannot apply array updates to non-array element %.*s", (int)c->rpn, c->rp);
        if (un_expand(node) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        for (j = 0; j < node->nk; j++) {
            char ib[12];
            int il = (int)dyn_bson_u32_key(ib, j);
            rp_push(c, ib, (size_t)il, &save);
            if (i + 1 == p->n)
                rc = apply_leaf(c, o, node, NULL, 0, 1, j);
            else
                rc = apply_path(c, o, p, i + 1, &node->kids[j]);
            c->rpn = save;
            if (rc < 0)
                return rc;
        }
        return 0;
    }
    if (kl == 1 && k[0] == '$') {
        char ib[12];
        int il;
        if (c->pos < 0)
            return dd_fail(c->e, DD_E_BADVALUE, "The positional operator did not find the match needed from the query.");
        if (node->type != DYN_BSON_ARRAY)
            return dd_fail(c->e, DD_E_BADVALUE, "The positional operator needs an array at '%.*s'", (int)c->rpn, c->rp);
        if (un_expand(node) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        il = snprintf(ib, sizeof ib, "%d", c->pos);
        rp_push(c, ib, (size_t)il, &save);
        if (i + 1 == p->n)
            rc = apply_leaf(c, o, node, NULL, 0, 1, (uint32_t)c->pos);
        else if ((uint32_t)c->pos < node->nk)
            rc = apply_path(c, o, p, i + 1, &node->kids[c->pos]);
        else
            rc = dd_fail(c->e, DD_E_BADVALUE, "The positional operator did not find the match needed from the query.");
        c->rpn = save;
        return rc;
    }
    rp_push(c, k, kl, &save);
    if (node->type == DYN_BSON_ARRAY) {
        if (!p->numeric[i]) {
            rc = dd_fail(c->e, DD_E_PATHNOTVIABLE, "Cannot create field '%.*s' in element {%.*s: [...]}", (int)kl, k,
                (int)save, c->rp);
            c->rpn = save;
            return rc;
        }
        if (un_expand(node) < 0)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        if (i + 1 == p->n)
            rc = apply_leaf(c, o, node, NULL, 0, 1, p->index[i]);
        else {
            un* ch = p->index[i] < node->nk ? &node->kids[p->index[i]] : NULL;
            if (!ch) {
                if (o->op == UOP_UNSET || o->op == UOP_PULL || o->op == UOP_PULLALL || o->op == UOP_POP) {
                    c->rpn = save;
                    return 0;
                }
                while (node->nk < p->index[i])
                    if (!un_append(node, NULL, 0)) {
                        c->rpn = save;
                        return dd_fail(c->e, DD_E_OOM, "out of memory");
                    }
                ch = un_append(node, NULL, 0);
                if (!ch || un_set_bytes(ch, DYN_BSON_DOC, (const uint8_t[]) { 5, 0, 0, 0, 0 }, 5) < 0) {
                    c->rpn = save;
                    return dd_fail(c->e, DD_E_OOM, "out of memory");
                }
            }
            rc = apply_path(c, o, p, i + 1, ch);
        }
        c->rpn = save;
        return rc;
    }
    if (i + 1 == p->n)
        rc = apply_leaf(c, o, node, k, kl, 0, 0);
    else {
        un* ch = un_find(node, k, kl);
        if (!ch) {
            if (o->op == UOP_UNSET || o->op == UOP_PULL || o->op == UOP_PULLALL || o->op == UOP_POP) {
                c->rpn = save;
                return 0;
            }
            ch = un_append(node, k, kl);
            if (!ch || un_set_bytes(ch, DYN_BSON_DOC, (const uint8_t[]) { 5, 0, 0, 0, 0 }, 5) < 0) {
                c->rpn = save;
                return dd_fail(c->e, DD_E_OOM, "out of memory");
            }
        }
        rc = apply_path(c, o, p, i + 1, ch);
    }
    c->rpn = save;
    return rc;
}

static int apply_path(uctx* c, const uop* o, const dd_path* p, int i, un* node)
{
    if (node->type != DYN_BSON_DOC && node->type != DYN_BSON_ARRAY) {
        if (o->op == UOP_UNSET || o->op == UOP_PULL || o->op == UOP_PULLALL || o->op == UOP_POP)
            return 0;
        return dd_fail(c->e, DD_E_PATHNOTVIABLE, "Cannot create field '%.*s' in element {%.*s: %s}", (int)p->plen[i],
            p->part[i], (int)c->rpn, c->rp, type_name(node->type));
    }
    if (un_expand(node) < 0)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    return descend(c, o, p, i, node);
}

static un* lookup_node(un* root, const dd_path* p)
{
    un* n = root;
    int i;
    for (i = 0; i < p->n; i++) {
        if (n->type != DYN_BSON_DOC || un_expand(n) < 0)
            return NULL;
        n = un_find(n, p->part[i], p->plen[i]);
        if (!n)
            return NULL;
    }
    return n;
}

static int path_has_array(un* root, const dd_path* p)
{
    un* n = root;
    int i;
    for (i = 0; i < p->n; i++) {
        if (n->type == DYN_BSON_ARRAY)
            return 1;
        if (n->type != DYN_BSON_DOC || un_expand(n) < 0)
            return 0;
        n = un_find(n, p->part[i], p->plen[i]);
        if (!n)
            return 0;
    }
    return 0;
}

static int do_rename(uctx* c, const uop* o, un* root)
{
    un* src;
    dyn_bson_w hold;
    dyn_bson_val v;
    uop set;
    int rc;
    size_t save;
    if (path_has_array(root, &o->path) || path_has_array(root, &o->to))
        return dd_fail(c->e, DD_E_BADVALUE, "The source and target field for $rename must not be on an array path: %s", o->path.text);
    src = lookup_node(root, &o->path);
    if (!src)
        return 0;
    dyn_bson_w_init(&hold);
    if (un_val(src, c, &v) < 0) {
        dyn_bson_w_free(&hold);
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    }
    dyn_bson_w_bytes(&hold, v.val, v.vlen);
    if (hold.oom) {
        dyn_bson_w_free(&hold);
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    }
    v.val = hold.p;
    memset(&set, 0, sizeof set);
    set.op = UOP_UNSET;
    save = c->rpn;
    rc = apply_path(c, &set, &o->path, 0, root);
    c->rpn = save;
    if (rc == 0) {
        set.op = UOP_SET;
        set.arg = v;
        rc = apply_path(c, &set, &o->to, 0, root);
        c->rpn = save;
    }
    if (rc == 0) {
        un* dst = lookup_node(root, &o->to);
        if (dst && dst->raw == hold.p && !dst->expanded) {
            dst->own = hold.p;
            hold.p = NULL;
        } else if (dst && dst->expanded && dst->raw == hold.p) {
            dst->own = hold.p;
            hold.p = NULL;
        }
    }
    dyn_bson_w_free(&hold);
    return rc;
}

static int id_of(const uint8_t* doc, dyn_bson_val* v)
{
    return dyn_bson_find(doc, "_id", 3, v);
}

void dd_desc_init(dd_upd_desc* d)
{
    memset(d, 0, sizeof *d);
    dyn_bson_w_init(&d->updated);
    dyn_bson_w_init(&d->removed);
}

void dd_desc_free(dd_upd_desc* d)
{
    dyn_bson_w_free(&d->updated);
    dyn_bson_w_free(&d->removed);
}

static void desc_close(dd_upd_desc* d)
{
    if (d->updated.n)
        dyn_bson_w_doc_end(&d->updated);
    if (d->removed.n)
        dyn_bson_w_doc_end(&d->removed);
}

static int replace_apply(const dd_upd* u, const uint8_t* doc, dyn_bson_w* out, dd_err* e)
{
    dyn_bson_val oid, nid;
    dyn_bson_iter it;
    int has_old = doc && id_of(doc, &oid);
    int has_new = id_of(u->src, &nid);
    if (has_old && has_new && !bson_eq(oid, nid))
        return dd_fail(e, DD_E_IMMUTABLEFIELD, "After applying the update, the (immutable) field '_id' was found to have been altered");
    dyn_bson_w_doc_begin(out);
    if (has_old)
        dyn_bson_w_value(out, "_id", 3, oid);
    else if (has_new)
        dyn_bson_w_value(out, "_id", 3, nid);
    dyn_bson_iter_init(&it, u->src, dyn_bson_rd32(u->src));
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.klen == 3 && memcmp(it.key, "_id", 3) == 0)
            continue;
        dyn_bson_w_value(out, it.key, it.klen, dyn_bson_iter_val(&it));
    }
    dyn_bson_w_doc_end(out);
    return out->oom ? dd_fail(e, DD_E_OOM, "out of memory") : 0;
}

int dd_update_apply(const dd_upd* u, const uint8_t* doc, int32_t pos, int is_insert, int64_t now_ms, dyn_bson_w* out,
    dd_upd_desc* desc, dd_err* e)
{
    un root;
    uctx c;
    int i, rc = 0;
    dyn_bson_val oid, nid;
    int had_id;
    if (u->replacement) {
        if (desc)
            desc->replaced = 1;
        return replace_apply(u, doc, out, e);
    }
    memset(&root, 0, sizeof root);
    root.type = DYN_BSON_DOC;
    root.raw = doc;
    root.rawlen = dyn_bson_rd32(doc);
    memset(&c, 0, sizeof c);
    c.pos = pos;
    c.is_insert = is_insert;
    c.now = now_ms;
    c.e = e;
    c.desc = desc;
    dyn_bson_w_init(&c.scratch);
    if (un_expand(&root) < 0) {
        rc = dd_fail(e, DD_E_OOM, "out of memory");
        goto done;
    }
    had_id = id_of(doc, &oid);
    for (i = 0; i < u->nops && rc == 0; i++) {
        const uop* o = &u->ops[i];
        if (o->op == UOP_SETONINSERT && !is_insert)
            continue;
        c.rpn = 0;
        if (o->op == UOP_RENAME)
            rc = do_rename(&c, o, &root);
        else
            rc = apply_path(&c, o, &o->path, 0, &root);
    }
    if (rc < 0)
        goto done;
    {
        un* idn = un_find(&root, "_id", 3);
        if (idn && idn != root.kids) {
            un t = *idn;
            memmove(root.kids + 1, root.kids, (size_t)(idn - root.kids) * sizeof *idn);
            root.kids[0] = t;
        }
    }
    un_write(&root, out);
    if (out->oom) {
        rc = dd_fail(e, DD_E_OOM, "out of memory");
        goto done;
    }
    if (had_id && (!id_of(out->p, &nid) || nid.type != oid.type || !bson_eq(oid, nid)))
        rc = dd_fail(e, DD_E_IMMUTABLEFIELD, "Performing an update on the path '_id' would modify the immutable field '_id'");
    if (desc && rc == 0)
        desc_close(desc);
done:
    un_free(&root);
    dyn_bson_w_free(&c.scratch);
    return rc;
}

static int seed_from(const uint8_t* filter, un* root, uctx* c, int depth)
{
    dyn_bson_iter it;
    if (depth > 20)
        return 0;
    dyn_bson_iter_init(&it, filter, dyn_bson_rd32(filter));
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val v = dyn_bson_iter_val(&it);
        uop set;
        dd_path p;
        int rc;
        if (it.klen == 4 && memcmp(it.key, "$and", 4) == 0 && v.type == DYN_BSON_ARRAY) {
            dyn_bson_iter ai;
            dyn_bson_iter_init(&ai, v.val, v.vlen);
            while (dyn_bson_iter_next(&ai) > 0)
                if (ai.type == DYN_BSON_DOC && seed_from(ai.val, root, c, depth + 1) < 0)
                    return -1;
            continue;
        }
        if (it.klen && it.key[0] == '$')
            continue;
        if (v.type == DYN_BSON_DOC) {
            dyn_bson_iter oi;
            dyn_bson_iter_init(&oi, v.val, v.vlen);
            if (dyn_bson_iter_next(&oi) > 0 && oi.klen && oi.key[0] == '$') {
                dyn_bson_val eq;
                if (!dyn_bson_find(v.val, "$eq", 3, &eq))
                    continue;
                v = eq;
            }
        }
        if (v.type == DYN_BSON_REGEX)
            continue;
        if (dd_path_parse(&p, it.key, it.klen, c->e) < 0)
            return -1;
        memset(&set, 0, sizeof set);
        set.op = UOP_SET;
        set.arg = v;
        c->rpn = 0;
        rc = apply_path(c, &set, &p, 0, root);
        dd_path_free(&p);
        if (rc < 0)
            return rc;
    }
    return 0;
}

int dd_upsert_seed(const uint8_t* filter, dyn_bson_w* out, dd_err* e)
{
    static const uint8_t empty[5] = { 5, 0, 0, 0, 0 };
    un root;
    uctx c;
    int rc;
    memset(&root, 0, sizeof root);
    root.type = DYN_BSON_DOC;
    root.raw = empty;
    root.rawlen = 5;
    memset(&c, 0, sizeof c);
    c.pos = -1;
    c.e = e;
    dyn_bson_w_init(&c.scratch);
    rc = un_expand(&root) < 0 ? dd_fail(e, DD_E_OOM, "out of memory") : seed_from(filter, &root, &c, 0);
    if (rc == 0) {
        un_write(&root, out);
        if (out->oom)
            rc = dd_fail(e, DD_E_OOM, "out of memory");
    }
    un_free(&root);
    dyn_bson_w_free(&c.scratch);
    return rc;
}
