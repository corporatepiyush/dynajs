#include <stdlib.h>
#include "dd.h"

#define DD_IN_LINEAR 16
#define DD_FILTER_MAX_DEPTH 100

typedef struct {
    void* opaque;
    dd_err* e;
    int depth;
    int has_regex;
} fc_ctx;

static dd_fnode* fn_new(uint8_t kind)
{
    dd_fnode* n = calloc(1, sizeof *n);
    if (n)
        n->kind = kind;
    return n;
}

static void fn_free(dd_fnode* n)
{
    uint32_t i;
    if (!n)
        return;
    for (i = 0; i < n->nkids; i++)
        fn_free(n->kids[i]);
    free(n->kids);
    if (n->path && n->owns_path) {
        dd_path_free(n->path);
        free(n->path);
    }
    if (n->re) {
        dd_regex_free(n->re);
        free(n->re);
    }
    for (i = 0; i < n->nin; i++)
        free(n->inkeys[i]);
    free(n->inkeys);
    free(n->inklen);
    if (n->inre) {
        for (i = 0; i < n->ninre; i++)
            dd_regex_free(&n->inre[i]);
        free(n->inre);
    }
    free(n);
}

static int fn_add(dd_fnode* p, dd_fnode* k, fc_ctx* c)
{
    dd_fnode** nk = realloc(p->kids, (size_t)(p->nkids + 1) * sizeof *nk);
    if (!nk) {
        fn_free(k);
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    }
    p->kids = nk;
    p->kids[p->nkids++] = k;
    return 0;
}

static dd_fnode* compile_doc(const uint8_t* doc, fc_ctx* c);

static int is_op_doc(dyn_bson_val v)
{
    dyn_bson_iter it;
    if (v.type != DYN_BSON_DOC)
        return 0;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    if (dyn_bson_iter_next(&it) <= 0)
        return 0;
    return it.klen > 0 && it.key[0] == '$';
}

static int key_is(const dyn_bson_iter* it, const char* s)
{
    size_t n = strlen(s);
    return it->klen == n && memcmp(it->key, s, n) == 0;
}

static int64_t num_i64(dyn_bson_val v)
{
    if (v.type == DYN_BSON_INT32)
        return (int32_t)dyn_bson_rd32(v.val);
    if (v.type == DYN_BSON_INT64)
        return (int64_t)dyn_bson_rd64(v.val);
    return (int64_t)dyn_bson_num_f64(v);
}

static int type_bit(const char* s, size_t n, uint32_t* mask)
{
    static const struct {
        const char* name;
        uint32_t bits;
    } names[] = {
        { "double", 1u << DYN_BSON_DOUBLE },
        { "string", 1u << DYN_BSON_STRING },
        { "object", 1u << DYN_BSON_DOC },
        { "array", 1u << DYN_BSON_ARRAY },
        { "binData", 1u << DYN_BSON_BINARY },
        { "undefined", 1u << DYN_BSON_UNDEFINED },
        { "objectId", 1u << DYN_BSON_OID },
        { "bool", 1u << DYN_BSON_BOOL },
        { "date", 1u << DYN_BSON_DATE },
        { "null", 1u << DYN_BSON_NULL },
        { "regex", 1u << DYN_BSON_REGEX },
        { "dbPointer", 1u << DYN_BSON_DBPOINTER },
        { "javascript", 1u << DYN_BSON_CODE },
        { "symbol", 1u << DYN_BSON_SYMBOL },
        { "javascriptWithScope", 1u << DYN_BSON_CODE_W_SCOPE },
        { "int", 1u << DYN_BSON_INT32 },
        { "timestamp", 1u << DYN_BSON_TIMESTAMP },
        { "long", 1u << DYN_BSON_INT64 },
        { "decimal", 1u << DYN_BSON_DEC128 },
        { "minKey", 1u << 30 },
        { "maxKey", 1u << 31 },
        { "number", (1u << DYN_BSON_DOUBLE) | (1u << DYN_BSON_INT32) | (1u << DYN_BSON_INT64) | (1u << DYN_BSON_DEC128) },
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strlen(names[i].name) == n && memcmp(names[i].name, s, n) == 0) {
            *mask |= names[i].bits;
            return 0;
        }
    }
    return -1;
}

static int type_arg(dyn_bson_val v, uint32_t* mask, fc_ctx* c)
{
    if (v.type == DYN_BSON_STRING) {
        if (type_bit((const char*)v.val + 4, dyn_bson_rd32(v.val) - 1, mask) < 0)
            return dd_fail(c->e, DD_E_BADVALUE, "Unknown type name alias: %s", (const char*)v.val + 4);
        return 0;
    }
    if (dyn_bson_is_number(v.type)) {
        double d = dyn_bson_num_f64(v);
        if (d == -1)
            *mask |= 1u << 30;
        else if (d == 127)
            *mask |= 1u << 31;
        else if (d >= 1 && d <= 19 && d == (int)d)
            *mask |= 1u << (int)d;
        else
            return dd_fail(c->e, DD_E_BADVALUE, "Invalid numerical type code: %g", d);
        return 0;
    }
    if (v.type == DYN_BSON_ARRAY) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0)
            if (type_arg(dyn_bson_iter_val(&it), mask, c) < 0)
                return -1;
        return 0;
    }
    return dd_fail(c->e, DD_E_BADVALUE, "type must be represented as a number or a string");
}

static int compile_regex(dd_fnode* n, const char* pat, size_t plen, const char* opts, size_t olen, fc_ctx* c)
{
    n->re = calloc(1, sizeof *n->re);
    if (!n->re)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    c->has_regex = 1;
    return dd_regex_compile(n->re, pat, plen, opts, olen, c->opaque, c->e);
}

static int ks_cmp_sort(const void* a, const void* b, const uint32_t* la, const uint32_t* lb)
{
    uint32_t m = *la < *lb ? *la : *lb;
    int r = memcmp(*(uint8_t* const*)a, *(uint8_t* const*)b, m);
    if (r)
        return r;
    return *la < *lb ? -1 : *la > *lb ? 1
                                      : 0;
}

static int compile_in(dd_fnode* n, dyn_bson_val arr, fc_ctx* c)
{
    dyn_bson_iter it;
    uint32_t count = 0, i, j;
    if (arr.type != DYN_BSON_ARRAY)
        return dd_fail(c->e, DD_E_BADVALUE, "$in needs an array");
    dyn_bson_iter_init(&it, arr.val, arr.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.type == DYN_BSON_REGEX) {
            dd_regex* nr = realloc(n->inre, (size_t)(n->ninre + 1) * sizeof *nr);
            const char* pat = (const char*)it.val;
            size_t pl = strlen(pat);
            if (!nr)
                return dd_fail(c->e, DD_E_OOM, "out of memory");
            n->inre = nr;
            c->has_regex = 1;
            if (dd_regex_compile(&n->inre[n->ninre], pat, pl, pat + pl + 1, strlen(pat + pl + 1), c->opaque, c->e) < 0)
                return -1;
            n->ninre++;
            continue;
        }
        if (it.type == DYN_BSON_DOC && is_op_doc(dyn_bson_iter_val(&it)))
            return dd_fail(c->e, DD_E_BADVALUE, "cannot nest $ under $in");
        if (it.type == DYN_BSON_NULL || it.type == DYN_BSON_UNDEFINED)
            n->in_null = 1;
        count++;
    }
    n->inkeys = calloc(count ? count : 1, sizeof *n->inkeys);
    n->inklen = calloc(count ? count : 1, sizeof *n->inklen);
    if (!n->inkeys || !n->inklen)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    dyn_bson_iter_init(&it, arr.val, arr.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_ks_buf kb;
        if (it.type == DYN_BSON_REGEX)
            continue;
        dyn_ks_init(&kb);
        dyn_ks_value(&kb, dyn_bson_iter_val(&it), 0);
        if (kb.oom)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        n->inkeys[n->nin] = kb.p;
        n->inklen[n->nin] = (uint32_t)kb.n;
        n->nin++;
    }
    for (i = 1; i < n->nin; i++) {
        uint8_t* k = n->inkeys[i];
        uint32_t kl = n->inklen[i];
        for (j = i; j > 0 && ks_cmp_sort(&n->inkeys[j - 1], &k, &n->inklen[j - 1], &kl) > 0; j--) {
            n->inkeys[j] = n->inkeys[j - 1];
            n->inklen[j] = n->inklen[j - 1];
        }
        n->inkeys[j] = k;
        n->inklen[j] = kl;
    }
    for (i = 0, j = 0; i < n->nin; i++) {
        if (j > 0 && n->inklen[j - 1] == n->inklen[i] && memcmp(n->inkeys[j - 1], n->inkeys[i], n->inklen[i]) == 0) {
            free(n->inkeys[i]);
            continue;
        }
        n->inkeys[j] = n->inkeys[i];
        n->inklen[j] = n->inklen[i];
        j++;
    }
    n->nin = j;
    return 0;
}

static dd_fnode* compile_ops(dd_path* path, dyn_bson_val ops, fc_ctx* c, int borrowed);

static dd_fnode* compile_one_op(dd_path* path, const dyn_bson_iter* it, dyn_bson_val opts_for_regex, fc_ctx* c)
{
    dd_fnode* n = fn_new(DD_F_FIELD);
    dyn_bson_val v = dyn_bson_iter_val(it);
    if (!n) {
        dd_fail(c->e, DD_E_OOM, "out of memory");
        return NULL;
    }
    n->path = path;
    n->path_borrowed = 1;
    n->arg = v;
    if (key_is(it, "$eq")) {
        n->op = DD_OP_EQ;
    } else if (key_is(it, "$ne")) {
        n->op = DD_OP_EQ;
        n->neg = 1;
    } else if (key_is(it, "$gt")) {
        n->op = DD_OP_GT;
    } else if (key_is(it, "$gte")) {
        n->op = DD_OP_GTE;
    } else if (key_is(it, "$lt")) {
        n->op = DD_OP_LT;
    } else if (key_is(it, "$lte")) {
        n->op = DD_OP_LTE;
    } else if (key_is(it, "$in") || key_is(it, "$nin")) {
        n->op = DD_OP_IN;
        n->neg = key_is(it, "$nin");
        if (compile_in(n, v, c) < 0)
            goto fail;
    } else if (key_is(it, "$exists")) {
        n->op = DD_OP_EXISTS;
        n->neg = !(v.type == DYN_BSON_BOOL ? v.val[0] : dyn_bson_is_number(v.type) ? dyn_bson_num_f64(v) != 0
                                                                                   : v.type != DYN_BSON_NULL);
    } else if (key_is(it, "$type")) {
        n->op = DD_OP_TYPE;
        if (type_arg(v, &n->type_mask, c) < 0)
            goto fail;
    } else if (key_is(it, "$regex")) {
        const char *pat, *opts = "";
        size_t pl, ol = 0;
        n->op = DD_OP_REGEX;
        if (v.type == DYN_BSON_REGEX) {
            pat = (const char*)v.val;
            pl = strlen(pat);
            opts = pat + pl + 1;
            ol = strlen(opts);
        } else if (v.type == DYN_BSON_STRING) {
            pat = (const char*)v.val + 4;
            pl = dyn_bson_rd32(v.val) - 1;
        } else {
            dd_fail(c->e, DD_E_BADVALUE, "$regex has to be a string");
            goto fail;
        }
        if (opts_for_regex.type == DYN_BSON_STRING) {
            opts = (const char*)opts_for_regex.val + 4;
            ol = dyn_bson_rd32(opts_for_regex.val) - 1;
        }
        if (compile_regex(n, pat, pl, opts, ol, c) < 0)
            goto fail;
    } else if (key_is(it, "$options")) {
        fn_free(n);
        return fn_new(DD_F_TRUE);
    } else if (key_is(it, "$elemMatch")) {
        if (v.type != DYN_BSON_DOC) {
            dd_fail(c->e, DD_E_BADVALUE, "$elemMatch needs an Object");
            goto fail;
        }
        if (is_op_doc(v)) {
            dyn_bson_iter in;
            int logical = 0;
            dyn_bson_iter_init(&in, v.val, v.vlen);
            dyn_bson_iter_next(&in);
            logical = key_is(&in, "$and") || key_is(&in, "$or") || key_is(&in, "$nor");
            if (logical) {
                dd_fnode* k = compile_doc(v.val, c);
                n->op = DD_OP_ELEMMATCH_DOC;
                if (!k || fn_add(n, k, c) < 0)
                    goto fail;
            } else {
                dd_fnode* k = compile_ops(NULL, v, c, 1);
                n->op = DD_OP_ELEMMATCH_VAL;
                if (!k || fn_add(n, k, c) < 0)
                    goto fail;
            }
        } else {
            dd_fnode* k = compile_doc(v.val, c);
            n->op = DD_OP_ELEMMATCH_DOC;
            if (!k || fn_add(n, k, c) < 0)
                goto fail;
        }
    } else if (key_is(it, "$size")) {
        double d = dyn_bson_is_number(v.type) ? dyn_bson_num_f64(v) : -1;
        n->op = DD_OP_SIZE;
        if (!(d >= 0 && d <= 2147483647.0) || d != (double)(int64_t)d) {
            dd_fail(c->e, DD_E_BADVALUE, "$size needs a non-negative integer");
            goto fail;
        }
        n->mod_d = (int64_t)d;
    } else if (key_is(it, "$all")) {
        dyn_bson_iter ai;
        n->op = DD_OP_ALL;
        if (v.type != DYN_BSON_ARRAY) {
            dd_fail(c->e, DD_E_BADVALUE, "$all needs an array");
            goto fail;
        }
        dyn_bson_iter_init(&ai, v.val, v.vlen);
        while (dyn_bson_iter_next(&ai) > 0) {
            dd_fnode* k;
            dyn_bson_val av = dyn_bson_iter_val(&ai);
            if (av.type == DYN_BSON_DOC && is_op_doc(av)) {
                dyn_bson_iter em;
                dyn_bson_iter_init(&em, av.val, av.vlen);
                dyn_bson_iter_next(&em);
                if (!key_is(&em, "$elemMatch")) {
                    dd_fail(c->e, DD_E_BADVALUE, "no $ expressions in $all");
                    goto fail;
                }
                k = compile_one_op(path, &em, (dyn_bson_val) { 0, NULL, 0 }, c);
            } else if (av.type == DYN_BSON_REGEX) {
                const char* pat = (const char*)av.val;
                size_t pl = strlen(pat);
                k = fn_new(DD_F_FIELD);
                if (k) {
                    k->path = path;
                    k->path_borrowed = 1;
                    k->op = DD_OP_REGEX;
                    if (compile_regex(k, pat, pl, pat + pl + 1, strlen(pat + pl + 1), c) < 0) {
                        fn_free(k);
                        goto fail;
                    }
                }
            } else {
                k = fn_new(DD_F_FIELD);
                if (k) {
                    k->path = path;
                    k->path_borrowed = 1;
                    k->op = DD_OP_EQ;
                    k->arg = av;
                }
            }
            if (!k || fn_add(n, k, c) < 0)
                goto fail;
        }
    } else if (key_is(it, "$mod")) {
        dyn_bson_iter mi;
        dyn_bson_val a[2];
        int na = 0;
        n->op = DD_OP_MOD;
        if (v.type != DYN_BSON_ARRAY) {
            dd_fail(c->e, DD_E_BADVALUE, "malformed mod, needs to be an array");
            goto fail;
        }
        dyn_bson_iter_init(&mi, v.val, v.vlen);
        while (dyn_bson_iter_next(&mi) > 0 && na < 3) {
            if (na < 2)
                a[na] = dyn_bson_iter_val(&mi);
            na++;
        }
        if (na != 2 || !dyn_bson_is_number(a[0].type) || !dyn_bson_is_number(a[1].type)) {
            dd_fail(c->e, DD_E_BADVALUE, "malformed mod, needs [divisor, remainder]");
            goto fail;
        }
        n->mod_d = num_i64(a[0]);
        n->mod_r = num_i64(a[1]);
        if (n->mod_d == 0) {
            dd_fail(c->e, DD_E_BADVALUE, "divisor cannot be 0");
            goto fail;
        }
    } else if (key_is(it, "$not")) {
        dd_fnode* k;
        n->op = DD_OP_NOT;
        n->neg = 1;
        if (v.type == DYN_BSON_REGEX) {
            const char* pat = (const char*)v.val;
            size_t pl = strlen(pat);
            k = fn_new(DD_F_FIELD);
            if (k) {
                k->path = path;
                k->path_borrowed = 1;
                k->op = DD_OP_REGEX;
                if (compile_regex(k, pat, pl, pat + pl + 1, strlen(pat + pl + 1), c) < 0) {
                    fn_free(k);
                    goto fail;
                }
            }
        } else if (v.type == DYN_BSON_DOC && is_op_doc(v)) {
            k = compile_ops(path, v, c, 1);
        } else {
            dd_fail(c->e, DD_E_BADVALUE, "$not needs a regex or a document");
            goto fail;
        }
        if (!k || fn_add(n, k, c) < 0)
            goto fail;
    } else {
        dd_fail(c->e, DD_E_BADVALUE, "unknown operator: %.*s", (int)it->klen, it->key);
        goto fail;
    }
    return n;
fail:
    if (c->e && c->e->code == 0)
        dd_fail(c->e, DD_E_OOM, "out of memory");
    fn_free(n);
    return NULL;
}

static dd_fnode* compile_ops(dd_path* path, dyn_bson_val ops, fc_ctx* c, int borrowed)
{
    dd_fnode* and = fn_new(DD_F_AND);
    dyn_bson_iter it;
    dyn_bson_val opts = { 0, NULL, 0 };
    int rc;
    if (!and) {
        dd_fail(c->e, DD_E_OOM, "out of memory");
        return NULL;
    }
    if (++c->depth > DD_FILTER_MAX_DEPTH) {
        fn_free(and);
        dd_fail(c->e, DD_E_BADVALUE, "filter nesting exceeds %d levels", DD_FILTER_MAX_DEPTH);
        return NULL;
    }
    (void)borrowed;
    dyn_bson_find(ops.val, "$options", 8, &opts);
    if (opts.type && !dyn_bson_find(ops.val, "$regex", 6, &(dyn_bson_val) { 0, NULL, 0 })) {
        fn_free(and);
        dd_fail(c->e, DD_E_BADVALUE, "$options needs a $regex");
        return NULL;
    }
    dyn_bson_iter_init(&it, ops.val, ops.vlen);
    while ((rc = dyn_bson_iter_next(&it)) > 0) {
        dd_fnode* k;
        if (it.klen == 0 || it.key[0] != '$') {
            fn_free(and);
            dd_fail(c->e, DD_E_BADVALUE, "unknown operator: %.*s (operators and fields cannot be mixed)", (int)it.klen, it.key);
            return NULL;
        }
        k = compile_one_op(path, &it, opts, c);
        if (!k || fn_add(and, k, c) < 0) {
            fn_free(and);
            return NULL;
        }
    }
    c->depth--;
    return and;
}

static dd_fnode* compile_doc(const uint8_t* doc, fc_ctx* c)
{
    dd_fnode* and = fn_new(DD_F_AND);
    dyn_bson_iter it;
    if (!and) {
        dd_fail(c->e, DD_E_OOM, "out of memory");
        return NULL;
    }
    if (++c->depth > DD_FILTER_MAX_DEPTH) {
        fn_free(and);
        dd_fail(c->e, DD_E_BADVALUE, "filter nesting exceeds %d levels", DD_FILTER_MAX_DEPTH);
        return NULL;
    }
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        dd_fnode* k = NULL;
        dyn_bson_val v = dyn_bson_iter_val(&it);
        if (it.klen > 0 && it.key[0] == '$') {
            int kind = key_is(&it, "$and") ? DD_F_AND : key_is(&it, "$or") ? DD_F_OR
                : key_is(&it, "$nor")                                      ? DD_F_NOR
                                                                           : 0;
            if (key_is(&it, "$comment"))
                continue;
            if (!kind) {
                fn_free(and);
                dd_fail(c->e, DD_E_BADVALUE, "unknown top level operator: %.*s", (int)it.klen, it.key);
                return NULL;
            }
            if (v.type != DYN_BSON_ARRAY || v.vlen <= 5) {
                fn_free(and);
                dd_fail(c->e, DD_E_BADVALUE, "%.*s must be a nonempty array", (int)it.klen, it.key);
                return NULL;
            }
            k = fn_new((uint8_t)kind);
            if (!k) {
                fn_free(and);
                dd_fail(c->e, DD_E_OOM, "out of memory");
                return NULL;
            }
            {
                dyn_bson_iter ai;
                dyn_bson_iter_init(&ai, v.val, v.vlen);
                while (dyn_bson_iter_next(&ai) > 0) {
                    dd_fnode* sub;
                    if (ai.type != DYN_BSON_DOC) {
                        fn_free(k);
                        fn_free(and);
                        dd_fail(c->e, DD_E_BADVALUE, "$or/$and/$nor entries need to be full objects");
                        return NULL;
                    }
                    sub = compile_doc(ai.val, c);
                    if (!sub || fn_add(k, sub, c) < 0) {
                        fn_free(k);
                        fn_free(and);
                        return NULL;
                    }
                }
            }
        } else {
            dd_path* p = calloc(1, sizeof *p);
            dd_fnode* holder;
            if (!p || dd_path_parse(p, it.key, it.klen, c->e) < 0) {
                if (!p)
                    dd_fail(c->e, DD_E_OOM, "out of memory");
                free(p);
                fn_free(and);
                return NULL;
            }
            holder = fn_new(DD_F_AND);
            if (!holder) {
                dd_path_free(p);
                free(p);
                fn_free(and);
                dd_fail(c->e, DD_E_OOM, "out of memory");
                return NULL;
            }
            holder->path = p;
            holder->owns_path = 1;
            if (is_op_doc(v)) {
                k = compile_ops(p, v, c, 1);
            } else if (v.type == DYN_BSON_REGEX) {
                const char* pat = (const char*)v.val;
                size_t pl = strlen(pat);
                k = fn_new(DD_F_FIELD);
                if (k) {
                    k->path = p;
                    k->path_borrowed = 1;
                    k->op = DD_OP_REGEX;
                    if (compile_regex(k, pat, pl, pat + pl + 1, strlen(pat + pl + 1), c) < 0) {
                        fn_free(k);
                        k = NULL;
                    }
                }
            } else {
                k = fn_new(DD_F_FIELD);
                if (k) {
                    k->path = p;
                    k->path_borrowed = 1;
                    k->op = DD_OP_EQ;
                    k->arg = v;
                }
            }
            if (!k || fn_add(holder, k, c) < 0) {
                if (c->e && c->e->code == 0)
                    dd_fail(c->e, DD_E_OOM, "out of memory");
                fn_free(holder);
                fn_free(and);
                return NULL;
            }
            k = holder;
        }
        if (fn_add(and, k, c) < 0) {
            fn_free(and);
            return NULL;
        }
    }
    c->depth--;
    return and;
}

static void mark_shared(dd_fnode* n)
{
    uint32_t i, j;
    for (i = 0; i < n->nkids; i++) {
        dd_fnode* a = n->kids[i];
        mark_shared(a);
        if (!a->path || a->path->n != 1)
            continue;
        for (j = i + 1; j < n->nkids; j++) {
            dd_fnode* b = n->kids[j];
            if (b->path && b->path->n == 1 && b->path->plen[0] == a->path->plen[0]
                && memcmp(b->path->part[0], a->path->part[0], a->path->plen[0]) == 0)
                a->shared_top = b->shared_top = 1;
        }
    }
}

int dd_filter_compile(dd_filter* f, const uint8_t* bson, void* re_opaque, dd_err* e)
{
    fc_ctx c;
    uint32_t n = dyn_bson_rd32(bson);
    memset(f, 0, sizeof *f);
    f->src = malloc(n);
    if (!f->src)
        return dd_fail(e, DD_E_OOM, "out of memory");
    memcpy(f->src, bson, n);
    f->src_n = n;
    c.opaque = re_opaque;
    c.e = e;
    c.depth = 0;
    c.has_regex = 0;
    if (e)
        e->code = 0;
    f->root = compile_doc(f->src, &c);
    if (!f->root) {
        free(f->src);
        f->src = NULL;
        return -1;
    }
    mark_shared(f->root);
    f->has_regex = c.has_regex;
    f->re_opaque = re_opaque;
    f->empty = f->root->nkids == 0;
    return 0;
}

void dd_filter_free(dd_filter* f)
{
    fn_free(f->root);
    free(f->src);
    memset(f, 0, sizeof *f);
}

typedef struct {
    const dd_fnode* n;
    dd_match_ctx* mc;
    int hit;
    int32_t aidx;
} vm_ctx;

static int in_has(const dd_fnode* n, dyn_bson_val v, dd_match_ctx* mc)
{
    uint32_t i;
    if (v.type == 0)
        return n->in_null;
    if ((v.type == DYN_BSON_STRING || v.type == DYN_BSON_SYMBOL) && n->ninre) {
        for (i = 0; i < n->ninre; i++) {
            int r = dd_regex_match(&n->inre[i], v.val + 4, dyn_bson_rd32(v.val) - 1, mc->re_opaque, mc->err);
            if (r < 0) {
                mc->failed = 1;
                return 0;
            }
            if (r)
                return 1;
        }
    }
    if (n->nin == 0)
        return 0;
    {
        uint8_t stackbuf[256];
        dyn_ks_buf kb;
        uint32_t lo = 0, hi = n->nin;
        int found = 0;
        kb.p = v.vlen <= 48 ? stackbuf : NULL;
        kb.n = 0;
        kb.cap = kb.p ? sizeof stackbuf : 0;
        kb.oom = 0;
        dyn_ks_value(&kb, v, 0);
        if (kb.oom) {
            if (kb.p != stackbuf)
                free(kb.p);
            mc->failed = 1;
            return 0;
        }
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t m = n->inklen[mid] < kb.n ? n->inklen[mid] : (uint32_t)kb.n;
            int c = memcmp(n->inkeys[mid], kb.p, m);
            if (c == 0)
                c = n->inklen[mid] < kb.n ? -1 : n->inklen[mid] > kb.n ? 1
                                                                       : 0;
            if (c == 0) {
                found = 1;
                break;
            }
            if (c < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (kb.p != stackbuf)
            free(kb.p);
        return found;
    }
}

static int cmp_ok(int op, dyn_bson_val v, dyn_bson_val arg)
{
    int c;
    if ((v.type == DYN_BSON_DOUBLE || v.type == DYN_BSON_INT32) && (arg.type == DYN_BSON_DOUBLE || arg.type == DYN_BSON_INT32)) {
        double a = v.type == DYN_BSON_INT32 ? (double)(int32_t)dyn_bson_rd32(v.val) : dyn_bson_rdf64(v.val);
        double b = arg.type == DYN_BSON_INT32 ? (double)(int32_t)dyn_bson_rd32(arg.val) : dyn_bson_rdf64(arg.val);
        if (a != a || b != b)
            return (a != a && b != b) && (op == DD_OP_GTE || op == DD_OP_LTE);
        switch (op) {
        case DD_OP_GT:
            return a > b;
        case DD_OP_GTE:
            return a >= b;
        case DD_OP_LT:
            return a < b;
        default:
            return a <= b;
        }
    }
    if (v.type == 0) {
        if (arg.type == DYN_BSON_NULL)
            return op == DD_OP_GTE || op == DD_OP_LTE;
        return 0;
    }
    if (v.type == DYN_BSON_UNDEFINED)
        v.type = DYN_BSON_NULL;
    if (arg.type != DYN_BSON_MINKEY && arg.type != DYN_BSON_MAXKEY
        && dyn_bson_canon_rank(v.type) != dyn_bson_canon_rank(arg.type))
        return 0;
    if (dyn_bson_is_number(v.type) && dyn_bson_is_number(arg.type)) {
        double a = dyn_bson_num_f64(v), b = dyn_bson_num_f64(arg);
        if (a != a || b != b)
            return (a != a && b != b) && (op == DD_OP_GTE || op == DD_OP_LTE);
    }
    c = dyn_bson_cmp(v, arg);
    switch (op) {
    case DD_OP_GT:
        return c > 0;
    case DD_OP_GTE:
        return c >= 0;
    case DD_OP_LT:
        return c < 0;
    default:
        return c <= 0;
    }
}

static int eq_ok(dyn_bson_val v, dyn_bson_val arg)
{
    if (v.type == DYN_BSON_STRING && arg.type == DYN_BSON_STRING)
        return dyn_bson_rd32(v.val) == dyn_bson_rd32(arg.val) && memcmp(v.val + 4, arg.val + 4, dyn_bson_rd32(v.val)) == 0;
    if (arg.type == DYN_BSON_NULL || arg.type == DYN_BSON_UNDEFINED)
        return v.type == 0 || v.type == DYN_BSON_NULL || v.type == DYN_BSON_UNDEFINED;
    if (v.type == 0)
        return 0;
    if (dyn_bson_canon_rank(v.type) != dyn_bson_canon_rank(arg.type))
        return 0;
    return dyn_bson_cmp(v, arg) == 0;
}

static int elem_match_val(const dd_fnode* n, dyn_bson_val arr, dd_match_ctx* mc)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, arr.val, arr.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val el = dyn_bson_iter_val(&it);
        if (n->op == DD_OP_ELEMMATCH_DOC) {
            if (el.type == DYN_BSON_DOC && dd_node_match(n->kids[0], el.val, mc))
                return 1;
        } else if (dd_value_matches_ops(n->kids[0], el, mc)) {
            return 1;
        }
        if (mc->failed)
            return 0;
    }
    return 0;
}

static int pos_test(const dd_fnode* n, dyn_bson_val v, int is_elem, dd_match_ctx* mc)
{
    switch (n->op) {
    case DD_OP_EQ:
        return eq_ok(v, n->arg);
    case DD_OP_GT:
    case DD_OP_GTE:
    case DD_OP_LT:
    case DD_OP_LTE:
        return cmp_ok(n->op, v, n->arg);
    case DD_OP_IN:
        return in_has(n, v, mc);
    case DD_OP_EXISTS:
        return v.type != 0;
    case DD_OP_TYPE:
        if (v.type == 0)
            return 0;
        if (v.type == DYN_BSON_MINKEY)
            return (n->type_mask >> 30) & 1;
        if (v.type == DYN_BSON_MAXKEY)
            return (n->type_mask >> 31) & 1;
        return (n->type_mask >> v.type) & 1;
    case DD_OP_REGEX:
        if (v.type == DYN_BSON_STRING || v.type == DYN_BSON_SYMBOL) {
            int r = dd_regex_match(n->re, v.val + 4, dyn_bson_rd32(v.val) - 1, mc->re_opaque, mc->err);
            if (r < 0) {
                mc->failed = 1;
                return 0;
            }
            return r;
        }
        return 0;
    case DD_OP_ELEMMATCH_DOC:
    case DD_OP_ELEMMATCH_VAL:
        return !is_elem && v.type == DYN_BSON_ARRAY && elem_match_val(n, v, mc);
    case DD_OP_SIZE: {
        dyn_bson_iter it;
        int64_t cnt = 0;
        if (is_elem || v.type != DYN_BSON_ARRAY)
            return 0;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0)
            cnt++;
        return cnt == n->mod_d;
    }
    case DD_OP_MOD:
        if (!dyn_bson_is_number(v.type))
            return 0;
        {
            double d = dyn_bson_num_f64(v);
            int64_t x;
            if (d != d || d > 9.2e18 || d < -9.2e18)
                return 0;
            x = v.type == DYN_BSON_INT64 ? (int64_t)dyn_bson_rd64(v.val) : (int64_t)d;
            if (n->mod_d == -1)
                return n->mod_r == 0;
            return x % n->mod_d == n->mod_r;
        }
    }
    return 0;
}

static int vm_visit(void* ud, dyn_bson_val v, int is_elem, int32_t aidx)
{
    vm_ctx* c = ud;
    if (pos_test(c->n, v, is_elem, c->mc)) {
        c->hit = 1;
        c->aidx = aidx;
        return 1;
    }
    return c->mc->failed;
}

static int top_value(const uint8_t* doc, const dd_path* p, int shared, dd_match_ctx* mc, dyn_bson_val* v)
{
    if (!shared) {
        if (dyn_bson_find(doc, p->part[0], p->plen[0], v))
            return 1;
        memset(v, 0, sizeof *v);
        return 0;
    }
    if (mc->cdoc == doc && mc->cklen == p->plen[0] && memcmp(mc->ckey, p->part[0], p->plen[0]) == 0) {
        *v = mc->cval;
        return mc->cfound;
    }
    mc->cfound = dyn_bson_find(doc, p->part[0], p->plen[0], &mc->cval);
    if (!mc->cfound)
        memset(&mc->cval, 0, sizeof mc->cval);
    mc->cdoc = doc;
    mc->ckey = p->part[0];
    mc->cklen = p->plen[0];
    *v = mc->cval;
    return mc->cfound;
}

static void top_walk(const uint8_t* doc, const dd_fnode* n, vm_ctx* c)
{
    dyn_bson_val v;
    dyn_bson_iter it;
    int32_t k = 0;
    top_value(doc, n->path, n->shared_top, c->mc, &v);
    if (vm_visit(c, v, 0, -1) || v.type != DYN_BSON_ARRAY)
        return;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        if (vm_visit(c, dyn_bson_iter_val(&it), 1, k))
            return;
        k++;
    }
}

static int field_match(const dd_fnode* n, const uint8_t* doc, dd_match_ctx* mc)
{
    vm_ctx c;
    if (n->op == DD_OP_NOT) {
        int32_t saved = mc->pos;
        int r = dd_node_match(n->kids[0], doc, mc);
        mc->pos = saved;
        return !r;
    }
    if (n->op == DD_OP_ALL) {
        uint32_t i;
        if (n->nkids == 0)
            return 0;
        for (i = 0; i < n->nkids; i++)
            if (!field_match(n->kids[i], doc, mc))
                return 0;
        return 1;
    }
    c.n = n;
    c.mc = mc;
    c.hit = 0;
    c.aidx = -1;
    if (n->path->n == 1)
        top_walk(doc, n, &c);
    else
        dd_path_walk(doc, n->path, vm_visit, &c);
    if (mc->failed)
        return 0;
    if (n->neg)
        return !c.hit;
    if (c.hit && c.aidx >= 0 && mc->pos < 0)
        mc->pos = c.aidx;
    return c.hit;
}

int dd_value_matches_ops(const dd_fnode* n, dyn_bson_val v, dd_match_ctx* mc)
{
    uint32_t i;
    if (n->kind == DD_F_AND) {
        for (i = 0; i < n->nkids; i++)
            if (!dd_value_matches_ops(n->kids[i], v, mc))
                return 0;
        return 1;
    }
    if (n->kind == DD_F_TRUE)
        return 1;
    if (n->op == DD_OP_NOT)
        return !dd_value_matches_ops(n->kids[0], v, mc);
    if (n->op == DD_OP_ALL) {
        for (i = 0; i < n->nkids; i++)
            if (!dd_value_matches_ops(n->kids[i], v, mc))
                return 0;
        return n->nkids > 0;
    }
    {
        int hit = pos_test(n, v, 0, mc);
        return n->neg ? !hit : hit;
    }
}

int dd_node_match(const dd_fnode* n, const uint8_t* doc, dd_match_ctx* mc)
{
    uint32_t i;
    switch (n->kind) {
    case DD_F_AND:
        for (i = 0; i < n->nkids; i++)
            if (!dd_node_match(n->kids[i], doc, mc))
                return 0;
        return 1;
    case DD_F_OR:
        for (i = 0; i < n->nkids; i++)
            if (dd_node_match(n->kids[i], doc, mc))
                return 1;
        return 0;
    case DD_F_NOR:
        for (i = 0; i < n->nkids; i++)
            if (dd_node_match(n->kids[i], doc, mc))
                return 0;
        return 1;
    case DD_F_TRUE:
        return 1;
    case DD_F_FALSE:
        return 0;
    default:
        return field_match(n, doc, mc);
    }
}

int dd_filter_match(const dd_filter* f, const uint8_t* doc, dd_match_ctx* mc)
{
    mc->cdoc = NULL;
    if (!mc->re_opaque)
        mc->re_opaque = f->re_opaque;
    return dd_node_match(f->root, doc, mc);
}
