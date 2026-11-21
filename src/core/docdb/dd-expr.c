#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dd-agg.h"

enum { X_LIT = 1,
    X_FIELD,
    X_VAR,
    X_OBJ,
    X_ARR,
    X_OP,
    X_MAP,
    X_FILTER,
    X_SWITCH };

enum {
    O_ADD = 1,
    O_SUB,
    O_MUL,
    O_DIV,
    O_MOD,
    O_ABS,
    O_CEIL,
    O_FLOOR,
    O_ROUND,
    O_SQRT,
    O_POW,
    O_EQ,
    O_NE,
    O_GT,
    O_GTE,
    O_LT,
    O_LTE,
    O_CMP,
    O_AND,
    O_OR,
    O_NOT,
    O_COND,
    O_IFNULL,
    O_CONCAT,
    O_TOLOWER,
    O_TOUPPER,
    O_SUBSTR,
    O_STRLEN,
    O_SPLIT,
    O_TRIM,
    O_SIZE,
    O_ELEMAT,
    O_FIRST,
    O_LAST,
    O_IN,
    O_CONCATARR,
    O_SLICE,
    O_ISARRAY,
    O_TYPE,
    O_TOSTRING,
    O_TOINT,
    O_TOLONG,
    O_TODOUBLE,
    O_TOBOOL,
    O_YEAR,
    O_MONTH,
    O_DAY,
    O_HOUR,
    O_MINUTE,
    O_SECOND,
    O_MILLI,
    O_DOW,
    O_DOY,
    O_LITERAL,
    O_SUMA,
    O_AVGA,
    O_MINA,
    O_MAXA,
    O_TOARRAYOBJ,
    O_MERGEOBJ,
};

static const struct {
    const char* name;
    int op;
    int min, max;
} OPS[] = {
    { "$add", O_ADD, 0, 64 },
    { "$subtract", O_SUB, 2, 2 },
    { "$multiply", O_MUL, 0, 64 },
    { "$divide", O_DIV, 2, 2 },
    { "$mod", O_MOD, 2, 2 },
    { "$abs", O_ABS, 1, 1 },
    { "$ceil", O_CEIL, 1, 1 },
    { "$floor", O_FLOOR, 1, 1 },
    { "$round", O_ROUND, 1, 2 },
    { "$sqrt", O_SQRT, 1, 1 },
    { "$pow", O_POW, 2, 2 },
    { "$eq", O_EQ, 2, 2 },
    { "$ne", O_NE, 2, 2 },
    { "$gt", O_GT, 2, 2 },
    { "$gte", O_GTE, 2, 2 },
    { "$lt", O_LT, 2, 2 },
    { "$lte", O_LTE, 2, 2 },
    { "$cmp", O_CMP, 2, 2 },
    { "$and", O_AND, 0, 64 },
    { "$or", O_OR, 0, 64 },
    { "$not", O_NOT, 1, 1 },
    { "$cond", O_COND, 3, 3 },
    { "$ifNull", O_IFNULL, 2, 64 },
    { "$concat", O_CONCAT, 0, 64 },
    { "$toLower", O_TOLOWER, 1, 1 },
    { "$toUpper", O_TOUPPER, 1, 1 },
    { "$substrCP", O_SUBSTR, 3, 3 },
    { "$substr", O_SUBSTR, 3, 3 },
    { "$strLenCP", O_STRLEN, 1, 1 },
    { "$split", O_SPLIT, 2, 2 },
    { "$trim", O_TRIM, 1, 1 },
    { "$size", O_SIZE, 1, 1 },
    { "$arrayElemAt", O_ELEMAT, 2, 2 },
    { "$first", O_FIRST, 1, 1 },
    { "$last", O_LAST, 1, 1 },
    { "$in", O_IN, 2, 2 },
    { "$concatArrays", O_CONCATARR, 0, 64 },
    { "$slice", O_SLICE, 2, 3 },
    { "$isArray", O_ISARRAY, 1, 1 },
    { "$type", O_TYPE, 1, 1 },
    { "$toString", O_TOSTRING, 1, 1 },
    { "$toInt", O_TOINT, 1, 1 },
    { "$toLong", O_TOLONG, 1, 1 },
    { "$toDouble", O_TODOUBLE, 1, 1 },
    { "$toBool", O_TOBOOL, 1, 1 },
    { "$year", O_YEAR, 1, 1 },
    { "$month", O_MONTH, 1, 1 },
    { "$dayOfMonth", O_DAY, 1, 1 },
    { "$hour", O_HOUR, 1, 1 },
    { "$minute", O_MINUTE, 1, 1 },
    { "$second", O_SECOND, 1, 1 },
    { "$millisecond", O_MILLI, 1, 1 },
    { "$dayOfWeek", O_DOW, 1, 1 },
    { "$dayOfYear", O_DOY, 1, 1 },
    { "$literal", O_LITERAL, 1, 1 },
    { "$sum", O_SUMA, 0, 64 },
    { "$avg", O_AVGA, 0, 64 },
    { "$min", O_MINA, 0, 64 },
    { "$max", O_MAXA, 0, 64 },
    { "$objectToArray", O_TOARRAYOBJ, 1, 1 },
    { "$mergeObjects", O_MERGEOBJ, 0, 64 },
};

struct dd_xn {
    uint8_t kind;
    uint8_t op;
    uint32_t nk;
    struct dd_xn** k;
    char** keys;
    uint32_t* klens;
    dyn_bson_val lit;
    uint8_t* litbuf;
    dd_path path;
    int has_path;
    char* var;
    size_t varlen;
};

void dd_expr_free(dd_xn* x)
{
    uint32_t i;
    if (!x)
        return;
    for (i = 0; i < x->nk; i++) {
        dd_expr_free(x->k[i]);
        if (x->keys)
            free(x->keys[i]);
    }
    free(x->k);
    free(x->keys);
    free(x->klens);
    free(x->litbuf);
    if (x->has_path)
        dd_path_free(&x->path);
    free(x->var);
    free(x);
}

static dd_xn* xn_new(uint8_t kind)
{
    dd_xn* x = calloc(1, sizeof *x);
    if (x)
        x->kind = kind;
    return x;
}

static int xn_add(dd_xn* x, dd_xn* k)
{
    dd_xn** nk = realloc(x->k, (size_t)(x->nk + 1) * sizeof *nk);
    if (!nk)
        return -1;
    x->k = nk;
    x->k[x->nk++] = k;
    return 0;
}

static int compile_into(dd_xn** out, dyn_bson_val v, dd_err* e, int depth);

static int compile_args(dd_xn* x, dyn_bson_val v, dd_err* e, int depth)
{
    if (v.type == DYN_BSON_ARRAY) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            dd_xn* k = NULL;
            if (compile_into(&k, dyn_bson_iter_val(&it), e, depth + 1) < 0)
                return -1;
            if (xn_add(x, k) < 0) {
                dd_expr_free(k);
                return dd_fail(e, DD_E_OOM, "out of memory");
            }
        }
        return 0;
    }
    {
        dd_xn* k = NULL;
        if (compile_into(&k, v, e, depth + 1) < 0)
            return -1;
        if (xn_add(x, k) < 0) {
            dd_expr_free(k);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
    }
    return 0;
}

static int compile_named(dd_xn* x, dyn_bson_val v, const char* const* names, int n, dd_err* e, int depth, const char* who)
{
    int i;
    dyn_bson_iter it;
    if (v.type != DYN_BSON_DOC)
        return dd_fail(e, DD_E_FAILEDTOPARSE, "%s needs an object argument", who);
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        for (i = 0; i < n; i++)
            if (strlen(names[i]) == it.klen && memcmp(names[i], it.key, it.klen) == 0)
                break;
        if (i == n)
            return dd_fail(e, DD_E_FAILEDTOPARSE, "%s: unrecognized parameter '%.*s'", who, (int)it.klen, it.key);
    }
    for (i = 0; i < n; i++) {
        dyn_bson_val a;
        dd_xn* k = NULL;
        if (!dyn_bson_find(v.val, names[i], strlen(names[i]), &a)) {
            k = xn_new(X_LIT);
            if (!k)
                return dd_fail(e, DD_E_OOM, "out of memory");
        } else if (compile_into(&k, a, e, depth + 1) < 0) {
            return -1;
        }
        if (xn_add(x, k) < 0) {
            dd_expr_free(k);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
    }
    return 0;
}

static int compile_into(dd_xn** out, dyn_bson_val v, dd_err* e, int depth)
{
    dd_xn* x;
    *out = NULL;
    if (depth > 100)
        return dd_fail(e, DD_E_BADVALUE, "expression nesting exceeds 100 levels");
    if (v.type == DYN_BSON_STRING && dyn_bson_rd32(v.val) >= 2 && v.val[4] == '$') {
        const char* s = (const char*)v.val + 4;
        size_t n = dyn_bson_rd32(v.val) - 1;
        if (n >= 2 && s[1] == '$') {
            size_t j = 2;
            x = xn_new(X_VAR);
            if (!x)
                return dd_fail(e, DD_E_OOM, "out of memory");
            while (j < n && s[j] != '.')
                j++;
            x->var = malloc(j - 1);
            if (!x->var) {
                dd_expr_free(x);
                return dd_fail(e, DD_E_OOM, "out of memory");
            }
            memcpy(x->var, s + 2, j - 2);
            x->var[j - 2] = 0;
            x->varlen = j - 2;
            if (j < n) {
                if (dd_path_parse(&x->path, s + j + 1, n - j - 1, e) < 0) {
                    dd_expr_free(x);
                    return -1;
                }
                x->has_path = 1;
            }
            *out = x;
            return 0;
        }
        x = xn_new(X_FIELD);
        if (!x)
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (dd_path_parse(&x->path, s + 1, n - 1, e) < 0) {
            free(x);
            return -1;
        }
        x->has_path = 1;
        *out = x;
        return 0;
    }
    if (v.type == DYN_BSON_ARRAY) {
        x = xn_new(X_ARR);
        if (!x)
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (compile_args(x, v, e, depth) < 0) {
            dd_expr_free(x);
            return -1;
        }
        *out = x;
        return 0;
    }
    if (v.type == DYN_BSON_DOC) {
        dyn_bson_iter it;
        dyn_bson_iter_init(&it, v.val, v.vlen);
        if (dyn_bson_iter_next(&it) > 0 && it.klen > 0 && it.key[0] == '$') {
            size_t i;
            dyn_bson_val arg = dyn_bson_iter_val(&it);
            if (dyn_bson_iter_next(&it) > 0)
                return dd_fail(e, DD_E_FAILEDTOPARSE, "an expression specification must contain exactly one field");
            dyn_bson_iter_init(&it, v.val, v.vlen);
            dyn_bson_iter_next(&it);
            if (it.klen == 4 && memcmp(it.key, "$map", 4) == 0) {
                static const char* const nm[] = { "input", "as", "in" };
                x = xn_new(X_MAP);
                if (!x || compile_named(x, arg, nm, 3, e, depth, "$map") < 0) {
                    dd_expr_free(x);
                    return x ? -1 : dd_fail(e, DD_E_OOM, "out of memory");
                }
                *out = x;
                return 0;
            }
            if (it.klen == 7 && memcmp(it.key, "$filter", 7) == 0) {
                static const char* const nm[] = { "input", "as", "cond" };
                x = xn_new(X_FILTER);
                if (!x || compile_named(x, arg, nm, 3, e, depth, "$filter") < 0) {
                    dd_expr_free(x);
                    return x ? -1 : dd_fail(e, DD_E_OOM, "out of memory");
                }
                *out = x;
                return 0;
            }
            if (it.klen == 7 && memcmp(it.key, "$switch", 7) == 0) {
                dyn_bson_val br, def;
                dyn_bson_iter bi;
                x = xn_new(X_SWITCH);
                if (!x)
                    return dd_fail(e, DD_E_OOM, "out of memory");
                if (arg.type != DYN_BSON_DOC || !dyn_bson_find(arg.val, "branches", 8, &br) || br.type != DYN_BSON_ARRAY) {
                    dd_expr_free(x);
                    return dd_fail(e, DD_E_FAILEDTOPARSE, "$switch requires an array of branches");
                }
                dyn_bson_iter_init(&bi, br.val, br.vlen);
                while (dyn_bson_iter_next(&bi) > 0) {
                    static const char* const nm[] = { "case", "then" };
                    if (compile_named(x, dyn_bson_iter_val(&bi), nm, 2, e, depth, "$switch branch") < 0) {
                        dd_expr_free(x);
                        return -1;
                    }
                }
                if (dyn_bson_find(arg.val, "default", 7, &def)) {
                    dd_xn* d = NULL;
                    if (compile_into(&d, def, e, depth + 1) < 0 || xn_add(x, d) < 0) {
                        dd_expr_free(d);
                        dd_expr_free(x);
                        return e->code ? -1 : dd_fail(e, DD_E_OOM, "out of memory");
                    }
                    x->op = 1;
                }
                *out = x;
                return 0;
            }
            for (i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
                if (strlen(OPS[i].name) == it.klen && memcmp(OPS[i].name, it.key, it.klen) == 0)
                    break;
            if (i == sizeof OPS / sizeof OPS[0])
                return dd_fail(e, 168, "Unrecognized expression '%.*s'", (int)it.klen, it.key);
            x = xn_new(X_OP);
            if (!x)
                return dd_fail(e, DD_E_OOM, "out of memory");
            x->op = (uint8_t)OPS[i].op;
            if (OPS[i].op == O_COND && arg.type == DYN_BSON_DOC && arg.vlen > 5 && arg.val[4] != 0 && arg.val[5] != '$') {
                static const char* const nm[] = { "if", "then", "else" };
                dyn_bson_val t;
                if (!dyn_bson_find(arg.val, "if", 2, &t) || !dyn_bson_find(arg.val, "then", 4, &t) || !dyn_bson_find(arg.val, "else", 4, &t)) {
                    dd_expr_free(x);
                    return dd_fail(e, 17080, "Missing 'if', 'then' or 'else' parameter to $cond");
                }
                if (compile_named(x, arg, nm, 3, e, depth, "$cond") < 0) {
                    dd_expr_free(x);
                    return -1;
                }
                *out = x;
                return 0;
            }
            if (OPS[i].op == O_LITERAL) {
                x->kind = X_LIT;
                x->litbuf = malloc(arg.vlen ? arg.vlen : 1);
                if (!x->litbuf) {
                    free(x);
                    return dd_fail(e, DD_E_OOM, "out of memory");
                }
                memcpy(x->litbuf, arg.val, arg.vlen);
                x->lit = arg;
                x->lit.val = x->litbuf;
                *out = x;
                return 0;
            }
            if (compile_args(x, arg, e, depth) < 0) {
                dd_expr_free(x);
                return -1;
            }
            if ((int)x->nk < OPS[i].min || (int)x->nk > OPS[i].max) {
                int min = OPS[i].min;
                dd_expr_free(x);
                return dd_fail(e, 16020, "Expression %s takes %d argument(s)", OPS[i].name, min);
            }
            *out = x;
            return 0;
        }
        x = xn_new(X_OBJ);
        if (!x)
            return dd_fail(e, DD_E_OOM, "out of memory");
        dyn_bson_iter_init(&it, v.val, v.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            dd_xn* k = NULL;
            char** nk2;
            uint32_t* nl;
            if (it.klen == 0 || it.key[0] == '$' || memchr(it.key, '.', it.klen)) {
                int dot = it.klen > 0 && it.key[0] != '$';
                dd_expr_free(x);
                return dd_fail(e, dot ? 16412 : 16410, "FieldPath field names may not %s: '%.*s'", dot ? "contain '.'" : "be empty or start with '$'",
                    (int)it.klen, it.key);
            }
            if (compile_into(&k, dyn_bson_iter_val(&it), e, depth + 1) < 0) {
                dd_expr_free(x);
                return -1;
            }
            nk2 = realloc(x->keys, (size_t)(x->nk + 1) * sizeof *nk2);
            if (nk2)
                x->keys = nk2;
            nl = realloc(x->klens, (size_t)(x->nk + 1) * sizeof *nl);
            if (nl)
                x->klens = nl;
            if (!nk2 || !nl || !(x->keys[x->nk] = malloc(it.klen + 1))) {
                dd_expr_free(k);
                dd_expr_free(x);
                return dd_fail(e, DD_E_OOM, "out of memory");
            }
            memcpy(x->keys[x->nk], it.key, it.klen);
            x->keys[x->nk][it.klen] = 0;
            x->klens[x->nk] = (uint32_t)it.klen;
            if (xn_add(x, k) < 0) {
                free(x->keys[x->nk]);
                dd_expr_free(k);
                dd_expr_free(x);
                return dd_fail(e, DD_E_OOM, "out of memory");
            }
        }
        *out = x;
        return 0;
    }
    x = xn_new(X_LIT);
    if (!x)
        return dd_fail(e, DD_E_OOM, "out of memory");
    x->litbuf = malloc(v.vlen ? v.vlen : 1);
    if (!x->litbuf) {
        free(x);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    memcpy(x->litbuf, v.val, v.vlen);
    x->lit = v;
    x->lit.val = x->litbuf;
    *out = x;
    return 0;
}

int dd_expr_compile(dd_xn** out, dyn_bson_val spec, dd_err* e)
{
    return compile_into(out, spec, e, 0);
}

int dd_expr_is_field(const dd_xn* x, const dd_path** p)
{
    if (x && x->kind == X_FIELD) {
        *p = &x->path;
        return 1;
    }
    return 0;
}

int dd_expr_arith(const dd_xn* x, int* op, const dd_xn* const** args, uint32_t* n)
{
    if (!x || x->kind != X_OP || (x->op != O_ADD && x->op != O_SUB && x->op != O_MUL))
        return 0;
    *op = x->op == O_ADD ? DD_XA_ADD : x->op == O_SUB ? DD_XA_SUB
                                                      : DD_XA_MUL;
    *args = (const dd_xn* const*)x->k;
    *n = x->nk;
    return 1;
}

int dd_expr_lit(const dd_xn* x, dyn_bson_val* v)
{
    if (x && x->kind == X_LIT) {
        *v = x->lit;
        return 1;
    }
    return 0;
}

void dd_xctx_init(dd_xctx* c, int64_t now, dd_err* e)
{
    memset(c, 0, sizeof *c);
    dyn_bson_w_init(&c->buf);
    c->now = now;
    c->e = e;
}

void dd_xctx_reset(dd_xctx* c, const uint8_t* root)
{
    c->root = root;
    c->nvars = 0;
    dyn_bson_w_reset(&c->buf);
}

void dd_xctx_free(dd_xctx* c)
{
    dyn_bson_w_free(&c->buf);
}

dyn_bson_val dd_xv_val(const dd_xctx* c, const dd_xv* v)
{
    dyn_bson_val r;
    r.type = v->type;
    r.vlen = v->n;
    r.val = v->inbuf ? c->buf.p + v->off : v->p;
    return r;
}

int dd_xv_put(dd_xctx* c, uint8_t type, const void* p, uint32_t n, dd_xv* out)
{
    out->type = type;
    out->inbuf = 1;
    out->off = (uint32_t)c->buf.n;
    out->n = n;
    out->p = NULL;
    dyn_bson_w_bytes(&c->buf, p, n);
    if (c->buf.oom)
        return dd_fail(c->e, DD_E_OOM, "out of memory evaluating an expression");
    return 0;
}

static void xv_ext(dd_xv* out, dyn_bson_val v)
{
    out->type = v.type;
    out->inbuf = 0;
    out->n = v.vlen;
    out->off = 0;
    out->p = v.val;
}

static int put_null(dd_xctx* c, dd_xv* out)
{
    out->type = DYN_BSON_NULL;
    out->inbuf = 0;
    out->n = 0;
    out->p = (const uint8_t*)"";
    (void)c;
    return 0;
}

static int put_bool(dd_xctx* c, int b, dd_xv* out)
{
    uint8_t v = b ? 1 : 0;
    return dd_xv_put(c, DYN_BSON_BOOL, &v, 1, out);
}

static int put_i32(dd_xctx* c, int32_t i, dd_xv* out)
{
    uint8_t b[4];
    dyn_bson_wr32(b, (uint32_t)i);
    return dd_xv_put(c, DYN_BSON_INT32, b, 4, out);
}

static int put_i64(dd_xctx* c, int64_t i, dd_xv* out)
{
    uint8_t b[8];
    if (i >= INT32_MIN && i <= INT32_MAX)
        return put_i32(c, (int32_t)i, out);
    dyn_bson_wr64(b, (uint64_t)i);
    return dd_xv_put(c, DYN_BSON_INT64, b, 8, out);
}

static int put_i64_exact(dd_xctx* c, int64_t i, dd_xv* out)
{
    uint8_t b[8];
    dyn_bson_wr64(b, (uint64_t)i);
    return dd_xv_put(c, DYN_BSON_INT64, b, 8, out);
}

static int put_f64(dd_xctx* c, double d, dd_xv* out)
{
    uint8_t b[8];
    uint64_t u;
    memcpy(&u, &d, 8);
    dyn_bson_wr64(b, u);
    return dd_xv_put(c, DYN_BSON_DOUBLE, b, 8, out);
}

static int put_date(dd_xctx* c, int64_t ms, dd_xv* out)
{
    uint8_t b[8];
    dyn_bson_wr64(b, (uint64_t)ms);
    return dd_xv_put(c, DYN_BSON_DATE, b, 8, out);
}

static int put_str(dd_xctx* c, const char* s, size_t n, dd_xv* out)
{
    uint8_t h[4];
    out->type = DYN_BSON_STRING;
    out->inbuf = 1;
    out->off = (uint32_t)c->buf.n;
    out->n = (uint32_t)n + 5;
    out->p = NULL;
    dyn_bson_wr32(h, (uint32_t)n + 1);
    dyn_bson_w_bytes(&c->buf, h, 4);
    dyn_bson_w_bytes(&c->buf, s, n);
    dyn_bson_w_bytes(&c->buf, "", 1);
    if (c->buf.oom)
        return dd_fail(c->e, DD_E_OOM, "out of memory evaluating an expression");
    return 0;
}

int dd_xv_truthy(const dd_xctx* c, const dd_xv* v)
{
    dyn_bson_val b = dd_xv_val(c, v);
    switch (v->type) {
    case 0:
    case DYN_BSON_NULL:
    case DYN_BSON_UNDEFINED:
        return 0;
    case DYN_BSON_BOOL:
        return b.val[0] != 0;
    case DYN_BSON_INT32:
    case DYN_BSON_INT64:
    case DYN_BSON_DOUBLE:
    case DYN_BSON_DEC128:
        return dyn_bson_num_f64(b) != 0;
    default:
        return 1;
    }
}

static int is_nullish(uint8_t t)
{
    return t == 0 || t == DYN_BSON_NULL || t == DYN_BSON_UNDEFINED;
}

static int xcmp(const dd_xctx* c, const dd_xv* a, const dd_xv* b)
{
    dyn_bson_val va = dd_xv_val(c, a), vb = dd_xv_val(c, b);
    if (va.type == 0)
        va.type = DYN_BSON_UNDEFINED;
    if (vb.type == 0)
        vb.type = DYN_BSON_UNDEFINED;
    if (va.type == DYN_BSON_UNDEFINED && vb.type == DYN_BSON_UNDEFINED)
        return 0;
    return dyn_bson_cmp(va, vb);
}

static int64_t as_i64(dyn_bson_val v)
{
    if (v.type == DYN_BSON_INT32)
        return (int32_t)dyn_bson_rd32(v.val);
    if (v.type == DYN_BSON_INT64)
        return (int64_t)dyn_bson_rd64(v.val);
    return (int64_t)dyn_bson_num_f64(v);
}

static int build_array(dd_xctx* c, const dd_xv* items, uint32_t n, dd_xv* out)
{
    dyn_bson_w w;
    uint32_t i;
    int rc;
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    for (i = 0; i < n; i++) {
        dyn_bson_val v = dd_xv_val(c, &items[i]);
        if (v.type == 0)
            v.type = DYN_BSON_NULL;
        dyn_bson_w_index_key(&w, v.type, i);
        dyn_bson_w_bytes(&w, v.val, v.vlen);
    }
    dyn_bson_w_doc_end(&w);
    rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_ARRAY, w.p, (uint32_t)w.n, out);
    dyn_bson_w_free(&w);
    return rc;
}

static int eval_field(const dd_path* p, dyn_bson_val base, dd_xctx* c, dd_xv* out);

static int arr_count(dyn_bson_val a)
{
    dyn_bson_iter it;
    int n = 0;
    dyn_bson_iter_init(&it, a.val, a.vlen);
    while (dyn_bson_iter_next(&it) > 0)
        n++;
    return n;
}

static int eval_field_from(const dd_path* p, int i, dyn_bson_val cur, dd_xctx* c, dd_xv* out)
{
    for (; i < p->n; i++) {
        if (cur.type == DYN_BSON_DOC) {
            if (!dyn_bson_find(cur.val, p->part[i], p->plen[i], &cur)) {
                out->type = 0;
                return 0;
            }
        } else if (cur.type == DYN_BSON_ARRAY) {
            dyn_bson_iter it;
            dd_xv* items = NULL;
            uint32_t n = 0, cap = 0;
            uint8_t* own = NULL;
            int rc;
            if (c->buf.p && cur.val >= c->buf.p && cur.val < c->buf.p + c->buf.cap) {
                own = malloc(cur.vlen ? cur.vlen : 1);
                if (!own)
                    return dd_fail(c->e, DD_E_OOM, "out of memory");
                memcpy(own, cur.val, cur.vlen);
                cur.val = own;
            }
            dyn_bson_iter_init(&it, cur.val, cur.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                dd_xv sub;
                dyn_bson_val el = dyn_bson_iter_val(&it);
                if (el.type != DYN_BSON_DOC && el.type != DYN_BSON_ARRAY)
                    continue;
                if (eval_field_from(p, i, el, c, &sub) < 0) {
                    free(items);
                    free(own);
                    return -1;
                }
                if (sub.type == 0)
                    continue;
                if (n == cap) {
                    uint32_t nc = cap ? cap * 2 : 8;
                    dd_xv* ni = realloc(items, (size_t)nc * sizeof *ni);
                    if (!ni) {
                        free(items);
                        free(own);
                        return dd_fail(c->e, DD_E_OOM, "out of memory");
                    }
                    items = ni;
                    cap = nc;
                }
                items[n++] = sub;
            }
            rc = build_array(c, items, n, out);
            free(items);
            free(own);
            return rc;
        } else {
            out->type = 0;
            return 0;
        }
    }
    xv_ext(out, cur);
    if (cur.val >= c->buf.p && c->buf.p && cur.val < c->buf.p + c->buf.cap) {
        out->inbuf = 1;
        out->off = (uint32_t)(cur.val - c->buf.p);
    }
    return 0;
}

static int eval_field(const dd_path* p, dyn_bson_val base, dd_xctx* c, dd_xv* out)
{
    return eval_field_from(p, 0, base, c, out);
}

static int lookup_var(dd_xctx* c, const char* name, size_t n, dd_xv* out)
{
    int i;
    for (i = c->nvars - 1; i >= 0; i--)
        if (c->vars[i].nlen == n && memcmp(c->vars[i].name, name, n) == 0) {
            *out = c->vars[i].v;
            return 1;
        }
    if ((n == 4 && memcmp(name, "ROOT", 4) == 0) || (n == 7 && memcmp(name, "CURRENT", 7) == 0)) {
        dyn_bson_val r = { DYN_BSON_DOC, c->root, dyn_bson_rd32(c->root) };
        xv_ext(out, r);
        return 1;
    }
    if (n == 3 && memcmp(name, "NOW", 3) == 0)
        return put_date(c, c->now, out) < 0 ? -1 : 1;
    if (n == 6 && memcmp(name, "REMOVE", 6) == 0) {
        out->type = 0;
        return 1;
    }
    return 0;
}

static int num_result(dd_xctx* c, int any_double, int any_long, int overflow, int64_t isum, double dsum, dd_xv* out)
{
    if (any_double || overflow)
        return put_f64(c, dsum, out);
    if (any_long)
        return put_i64_exact(c, isum, out);
    return put_i64(c, isum, out);
}

static int date_part(int op, int64_t ms, int64_t* out)
{
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    switch (op) {
    case O_YEAR:
        *out = y;
        break;
    case O_MONTH:
        *out = m;
        break;
    case O_DAY:
        *out = d;
        break;
    case O_HOUR:
        *out = rem / 3600000;
        break;
    case O_MINUTE:
        *out = rem / 60000 % 60;
        break;
    case O_SECOND:
        *out = rem / 1000 % 60;
        break;
    case O_MILLI:
        *out = rem % 1000;
        break;
    case O_DOW:
        *out = ((days % 7) + 7 + 4) % 7 + 1;
        break;
    default: {
        static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        *out = cum[m - 1] + d + (leap && m > 2 ? 1 : 0);
        break;
    }
    }
    return 0;
}

static const char* type_alias(uint8_t t)
{
    switch (t) {
    case 0:
        return "missing";
    case DYN_BSON_DOUBLE:
        return "double";
    case DYN_BSON_STRING:
        return "string";
    case DYN_BSON_DOC:
        return "object";
    case DYN_BSON_ARRAY:
        return "array";
    case DYN_BSON_BINARY:
        return "binData";
    case DYN_BSON_UNDEFINED:
        return "undefined";
    case DYN_BSON_OID:
        return "objectId";
    case DYN_BSON_BOOL:
        return "bool";
    case DYN_BSON_DATE:
        return "date";
    case DYN_BSON_NULL:
        return "null";
    case DYN_BSON_REGEX:
        return "regex";
    case DYN_BSON_INT32:
        return "int";
    case DYN_BSON_TIMESTAMP:
        return "timestamp";
    case DYN_BSON_INT64:
        return "long";
    case DYN_BSON_DEC128:
        return "decimal";
    case DYN_BSON_MINKEY:
        return "minKey";
    case DYN_BSON_MAXKEY:
        return "maxKey";
    default:
        return "other";
    }
}

static int eval_args(const dd_xn* x, dd_xctx* c, dd_xv* av)
{
    uint32_t i;
    for (i = 0; i < x->nk; i++)
        if (dd_expr_eval(x->k[i], c, &av[i]) < 0)
            return -1;
    return 0;
}

static int str_of(const dd_xctx* c, const dd_xv* v, const char** s, size_t* n)
{
    dyn_bson_val b = dd_xv_val(c, v);
    if (v->type != DYN_BSON_STRING && v->type != DYN_BSON_SYMBOL)
        return 0;
    *s = (const char*)b.val + 4;
    *n = dyn_bson_rd32(b.val) - 1;
    return 1;
}

static int utf8_cp_offset(const char* s, size_t n, int64_t cp, size_t* off)
{
    size_t i = 0;
    int64_t k = 0;
    while (i < n && k < cp) {
        unsigned char ch = (unsigned char)s[i];
        i += ch < 0x80 ? 1 : ch < 0xE0 ? 2
            : ch < 0xF0                ? 3
                                       : 4;
        k++;
    }
    *off = i > n ? n : i;
    return 0;
}

static int eval_op(const dd_xn* x, dd_xctx* c, dd_xv* out)
{
    dd_xv avs[8];
    dd_xv* av = x->nk <= 8 ? avs : malloc((size_t)x->nk * sizeof *av);
    uint32_t i;
    int rc = 0;
    if (!av)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    if (x->op == O_AND || x->op == O_OR || x->op == O_COND || x->op == O_IFNULL) {
        int want = x->op == O_OR;
        if (x->op == O_COND) {
            dd_xv t;
            if (dd_expr_eval(x->k[0], c, &t) < 0)
                goto fail;
            rc = dd_expr_eval(x->k[dd_xv_truthy(c, &t) ? 1 : 2], c, out);
            goto done;
        }
        if (x->op == O_IFNULL) {
            for (i = 0; i < x->nk; i++) {
                if (dd_expr_eval(x->k[i], c, out) < 0)
                    goto fail;
                if (!is_nullish(out->type))
                    goto done;
            }
            goto done;
        }
        for (i = 0; i < x->nk; i++) {
            dd_xv t;
            if (dd_expr_eval(x->k[i], c, &t) < 0)
                goto fail;
            if (dd_xv_truthy(c, &t) == want) {
                rc = put_bool(c, want, out);
                goto done;
            }
        }
        rc = put_bool(c, !want, out);
        goto done;
    }
    if (eval_args(x, c, av) < 0)
        goto fail;
    switch (x->op) {
    case O_ADD:
    case O_MUL: {
        int any_double = 0, any_long = 0, overflow = 0, date_at = -1;
        int64_t isum = x->op == O_MUL ? 1 : 0;
        double dsum = x->op == O_MUL ? 1 : 0;
        for (i = 0; i < x->nk; i++) {
            dyn_bson_val v = dd_xv_val(c, &av[i]);
            if (is_nullish(v.type)) {
                rc = put_null(c, out);
                goto done;
            }
            if (v.type == DYN_BSON_DATE && x->op == O_ADD && date_at < 0) {
                date_at = (int)i;
                isum += (int64_t)dyn_bson_rd64(v.val);
                dsum += (double)(int64_t)dyn_bson_rd64(v.val);
                continue;
            }
            if (!dyn_bson_is_number(v.type)) {
                rc = dd_fail(c->e, 16554, "%s only supports numeric or date types, not %s", x->op == O_ADD ? "$add" : "$multiply",
                    type_alias(v.type));
                goto done;
            }
            if (v.type == DYN_BSON_DOUBLE || v.type == DYN_BSON_DEC128)
                any_double = 1;
            if (v.type == DYN_BSON_INT64)
                any_long = 1;
            if (x->op == O_ADD) {
                dsum += dyn_bson_num_f64(v);
                if (!any_double && __builtin_add_overflow(isum, as_i64(v), &isum))
                    overflow = 1;
            } else {
                dsum *= dyn_bson_num_f64(v);
                if (!any_double && __builtin_mul_overflow(isum, as_i64(v), &isum))
                    overflow = 1;
            }
        }
        if (date_at >= 0)
            rc = put_date(c, any_double ? (int64_t)llround(dsum) : isum, out);
        else
            rc = num_result(c, any_double, any_long, overflow, isum, dsum, out);
        break;
    }
    case O_SUB: {
        dyn_bson_val a = dd_xv_val(c, &av[0]), b = dd_xv_val(c, &av[1]);
        if (is_nullish(a.type) || is_nullish(b.type)) {
            rc = put_null(c, out);
            break;
        }
        if (a.type == DYN_BSON_DATE && b.type == DYN_BSON_DATE) {
            rc = put_i64_exact(c, (int64_t)dyn_bson_rd64(a.val) - (int64_t)dyn_bson_rd64(b.val), out);
            break;
        }
        if (a.type == DYN_BSON_DATE && dyn_bson_is_number(b.type)) {
            rc = put_date(c, (int64_t)dyn_bson_rd64(a.val) - (int64_t)llround(dyn_bson_num_f64(b)), out);
            break;
        }
        if (!dyn_bson_is_number(a.type) || !dyn_bson_is_number(b.type)) {
            rc = dd_fail(c->e, 16556, "can't $subtract %s from %s", type_alias(b.type), type_alias(a.type));
            break;
        }
        if (a.type == DYN_BSON_DOUBLE || b.type == DYN_BSON_DOUBLE || a.type == DYN_BSON_DEC128 || b.type == DYN_BSON_DEC128)
            rc = put_f64(c, dyn_bson_num_f64(a) - dyn_bson_num_f64(b), out);
        else {
            int64_t r;
            if (__builtin_sub_overflow(as_i64(a), as_i64(b), &r))
                rc = put_f64(c, dyn_bson_num_f64(a) - dyn_bson_num_f64(b), out);
            else
                rc = num_result(c, 0, a.type == DYN_BSON_INT64 || b.type == DYN_BSON_INT64, 0, r, 0, out);
        }
        break;
    }
    case O_DIV:
    case O_MOD:
    case O_POW: {
        dyn_bson_val a = dd_xv_val(c, &av[0]), b = dd_xv_val(c, &av[1]);
        if (is_nullish(a.type) || is_nullish(b.type)) {
            rc = put_null(c, out);
            break;
        }
        if (!dyn_bson_is_number(a.type) || !dyn_bson_is_number(b.type)) {
            rc = dd_fail(c->e, 16609, "%s only supports numeric types", x->op == O_DIV ? "$divide" : x->op == O_MOD ? "$mod"
                                                                                                                    : "$pow");
            break;
        }
        if (x->op != O_POW && dyn_bson_num_f64(b) == 0) {
            rc = dd_fail(c->e, 16608, "can't %s by zero", x->op == O_DIV ? "$divide" : "$mod");
            break;
        }
        if (x->op == O_DIV)
            rc = put_f64(c, dyn_bson_num_f64(a) / dyn_bson_num_f64(b), out);
        else if (x->op == O_POW)
            rc = put_f64(c, pow(dyn_bson_num_f64(a), dyn_bson_num_f64(b)), out);
        else if (a.type == DYN_BSON_DOUBLE || b.type == DYN_BSON_DOUBLE)
            rc = put_f64(c, fmod(dyn_bson_num_f64(a), dyn_bson_num_f64(b)), out);
        else if (as_i64(b) == -1)
            rc = put_i64(c, 0, out);
        else
            rc = num_result(c, 0, a.type == DYN_BSON_INT64 || b.type == DYN_BSON_INT64, 0, as_i64(a) % as_i64(b), 0, out);
        break;
    }
    case O_ABS:
    case O_CEIL:
    case O_FLOOR:
    case O_ROUND:
    case O_SQRT: {
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        double d;
        if (is_nullish(a.type)) {
            rc = put_null(c, out);
            break;
        }
        if (!dyn_bson_is_number(a.type)) {
            rc = dd_fail(c->e, 28765, "this math operator only supports numeric types, not %s", type_alias(a.type));
            break;
        }
        if (a.type != DYN_BSON_DOUBLE && a.type != DYN_BSON_DEC128 && x->op != O_SQRT) {
            int64_t v = as_i64(a);
            rc = x->op == O_ABS ? (v == INT64_MIN ? put_f64(c, 9223372036854775808.0, out)
                                                  : num_result(c, 0, a.type == DYN_BSON_INT64, 0, v < 0 ? -v : v, 0, out))
                                : num_result(c, 0, a.type == DYN_BSON_INT64, 0, v, 0, out);
            break;
        }
        d = dyn_bson_num_f64(a);
        if (x->op == O_ABS)
            d = fabs(d);
        else if (x->op == O_CEIL)
            d = ceil(d);
        else if (x->op == O_FLOOR)
            d = floor(d);
        else if (x->op == O_SQRT)
            d = sqrt(d);
        else {
            int64_t places = x->nk > 1 ? as_i64(dd_xv_val(c, &av[1])) : 0;
            double f = pow(10.0, (double)places), y = d * f, r = nearbyint(y);
            d = r / f;
        }
        rc = put_f64(c, d, out);
        break;
    }
    case O_EQ:
    case O_NE:
    case O_GT:
    case O_GTE:
    case O_LT:
    case O_LTE:
    case O_CMP: {
        int r = xcmp(c, &av[0], &av[1]);
        if (x->op == O_CMP)
            rc = put_i32(c, r < 0 ? -1 : r > 0 ? 1
                                               : 0,
                out);
        else
            rc = put_bool(c,
                x->op == O_EQ        ? r == 0
                    : x->op == O_NE  ? r != 0
                    : x->op == O_GT  ? r > 0
                    : x->op == O_GTE ? r >= 0
                    : x->op == O_LT  ? r < 0
                                     : r <= 0,
                out);
        break;
    }
    case O_NOT:
        rc = put_bool(c, !dd_xv_truthy(c, &av[0]), out);
        break;
    case O_CONCAT: {
        dyn_bson_w w;
        dyn_bson_w_init(&w);
        for (i = 0; i < x->nk; i++) {
            const char* s;
            size_t n;
            if (is_nullish(av[i].type)) {
                dyn_bson_w_free(&w);
                rc = put_null(c, out);
                goto done;
            }
            if (!str_of(c, &av[i], &s, &n)) {
                dyn_bson_w_free(&w);
                rc = dd_fail(c->e, 16702, "$concat only supports strings, not %s", type_alias(av[i].type));
                goto done;
            }
            dyn_bson_w_bytes(&w, s, n);
        }
        rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : put_str(c, w.p ? (const char*)w.p : "", w.n, out);
        dyn_bson_w_free(&w);
        break;
    }
    case O_TOLOWER:
    case O_TOUPPER:
    case O_TRIM:
    case O_TOSTRING: {
        const char* s;
        size_t n, j;
        char* t;
        char num[64];
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        if (is_nullish(a.type)) {
            rc = x->op == O_TOSTRING || x->op == O_TRIM ? put_null(c, out) : put_str(c, "", 0, out);
            break;
        }
        if (!str_of(c, &av[0], &s, &n)) {
            if (dyn_bson_is_number(a.type) || a.type == DYN_BSON_BOOL || a.type == DYN_BSON_DATE || a.type == DYN_BSON_OID) {
                if (a.type == DYN_BSON_INT32 || a.type == DYN_BSON_INT64)
                    n = (size_t)snprintf(num, sizeof num, "%lld", (long long)as_i64(a));
                else if (a.type == DYN_BSON_BOOL)
                    n = (size_t)snprintf(num, sizeof num, "%s", a.val[0] ? "true" : "false");
                else if (a.type == DYN_BSON_OID) {
                    static const char hx[] = "0123456789abcdef";
                    for (j = 0; j < 12; j++) {
                        num[2 * j] = hx[a.val[j] >> 4];
                        num[2 * j + 1] = hx[a.val[j] & 15];
                    }
                    n = 24;
                } else if (a.type == DYN_BSON_DATE) {
                    int64_t ms = (int64_t)dyn_bson_rd64(a.val), p[7];
                    int op;
                    for (op = O_YEAR; op <= O_MILLI; op++)
                        date_part(op, ms, &p[op - O_YEAR]);
                    n = (size_t)snprintf(num, sizeof num, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ", (long long)p[0],
                        (long long)p[1], (long long)p[2], (long long)p[3], (long long)p[4], (long long)p[5], (long long)p[6]);
                } else
                    n = (size_t)snprintf(num, sizeof num, "%.17g", dyn_bson_num_f64(a));
                if (x->op == O_TOSTRING || x->op == O_TOLOWER || x->op == O_TOUPPER) {
                    s = num;
                } else {
                    rc = dd_fail(c->e, 50699, "$trim requires its input to be a string");
                    break;
                }
            } else {
                rc = dd_fail(c->e, 16007, "can't convert %s to a string", type_alias(a.type));
                break;
            }
        }
        if (x->op == O_TOSTRING) {
            rc = put_str(c, s, n, out);
            break;
        }
        if (x->op == O_TRIM) {
            size_t a0 = 0, b0 = n;
            while (a0 < b0 && (s[a0] == ' ' || s[a0] == '\t' || s[a0] == '\n' || s[a0] == '\r' || s[a0] == '\v' || s[a0] == '\f' || s[a0] == 0))
                a0++;
            while (b0 > a0 && (s[b0 - 1] == ' ' || s[b0 - 1] == '\t' || s[b0 - 1] == '\n' || s[b0 - 1] == '\r' || s[b0 - 1] == '\v' || s[b0 - 1] == '\f' || s[b0 - 1] == 0))
                b0--;
            rc = put_str(c, s + a0, b0 - a0, out);
            break;
        }
        t = malloc(n + 1);
        if (!t) {
            rc = dd_fail(c->e, DD_E_OOM, "out of memory");
            break;
        }
        for (j = 0; j < n; j++) {
            char ch = s[j];
            if (x->op == O_TOLOWER && ch >= 'A' && ch <= 'Z')
                ch = (char)(ch + 32);
            if (x->op == O_TOUPPER && ch >= 'a' && ch <= 'z')
                ch = (char)(ch - 32);
            t[j] = ch;
        }
        rc = put_str(c, t, n, out);
        free(t);
        break;
    }
    case O_SUBSTR: {
        const char* s;
        size_t n, a0, b0;
        int64_t st = as_i64(dd_xv_val(c, &av[1])), ln = as_i64(dd_xv_val(c, &av[2]));
        if (is_nullish(av[0].type)) {
            rc = put_str(c, "", 0, out);
            break;
        }
        if (!str_of(c, &av[0], &s, &n) || !dyn_bson_is_number(av[1].type) || !dyn_bson_is_number(av[2].type)) {
            rc = dd_fail(c->e, 34450, "$substrCP needs a string and two integers");
            break;
        }
        if (st < 0 || ln < 0) {
            rc = dd_fail(c->e, 34455, "$substrCP: starting index and length must be non-negative");
            break;
        }
        utf8_cp_offset(s, n, st, &a0);
        utf8_cp_offset(s + a0, n - a0, ln, &b0);
        rc = put_str(c, s + a0, b0, out);
        break;
    }
    case O_STRLEN: {
        const char* s;
        size_t n, j;
        int64_t k = 0;
        if (!str_of(c, &av[0], &s, &n)) {
            rc = dd_fail(c->e, 34471, "$strLenCP requires a string argument, found: %s", type_alias(av[0].type));
            break;
        }
        for (j = 0; j < n; j++)
            if (((unsigned char)s[j] & 0xC0) != 0x80)
                k++;
        rc = put_i64(c, k, out);
        break;
    }
    case O_SPLIT: {
        const char *s, *d;
        size_t n, dn, j, start = 0;
        dyn_bson_w w;
        uint32_t idx = 0;
        if (is_nullish(av[0].type)) {
            rc = put_null(c, out);
            break;
        }
        if (!str_of(c, &av[0], &s, &n) || !str_of(c, &av[1], &d, &dn) || dn == 0) {
            rc = dd_fail(c->e, 40085, "$split needs a string and a non-empty string delimiter");
            break;
        }
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        for (j = 0; j + dn <= n;) {
            if (memcmp(s + j, d, dn) == 0) {
                char kb[12];
                int kl = (int)dyn_bson_u32_key(kb, idx++);
                dyn_bson_w_string(&w, kb, (size_t)kl, s + start, j - start);
                j += dn;
                start = j;
            } else {
                j++;
            }
        }
        {
            char kb[12];
            int kl = (int)dyn_bson_u32_key(kb, idx);
            dyn_bson_w_string(&w, kb, (size_t)kl, s + start, n - start);
        }
        dyn_bson_w_doc_end(&w);
        rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_ARRAY, w.p, (uint32_t)w.n, out);
        dyn_bson_w_free(&w);
        break;
    }
    case O_SIZE:
    case O_FIRST:
    case O_LAST:
    case O_ISARRAY: {
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        if (x->op == O_ISARRAY) {
            rc = put_bool(c, a.type == DYN_BSON_ARRAY, out);
            break;
        }
        if (a.type != DYN_BSON_ARRAY) {
            if ((x->op == O_FIRST || x->op == O_LAST) && is_nullish(a.type)) {
                rc = put_null(c, out);
                break;
            }
            rc = dd_fail(c->e, 17124, "The argument to %s must be an array but was of type: %s",
                x->op == O_SIZE ? "$size" : x->op == O_FIRST ? "$first"
                                                             : "$last",
                type_alias(a.type));
            break;
        }
        if (x->op == O_SIZE) {
            rc = put_i32(c, arr_count(a), out);
        } else {
            dyn_bson_iter it;
            dyn_bson_val last = { 0, NULL, 0 };
            int got = 0;
            dyn_bson_iter_init(&it, a.val, a.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                last = dyn_bson_iter_val(&it);
                got = 1;
                if (x->op == O_FIRST)
                    break;
            }
            if (got) {
                dd_xv base = av[0];
                xv_ext(out, last);
                if (base.inbuf) {
                    out->inbuf = 1;
                    out->off = (uint32_t)(last.val - c->buf.p);
                }
            } else {
                out->type = 0;
            }
        }
        break;
    }
    case O_ELEMAT: {
        dyn_bson_val a = dd_xv_val(c, &av[0]), b = dd_xv_val(c, &av[1]);
        int64_t k, cnt;
        dyn_bson_iter it;
        if (is_nullish(a.type) || is_nullish(b.type)) {
            rc = put_null(c, out);
            break;
        }
        if (a.type != DYN_BSON_ARRAY || !dyn_bson_is_number(b.type)) {
            rc = dd_fail(c->e, 28689, "$arrayElemAt needs an array and an integer index");
            break;
        }
        cnt = arr_count(a);
        k = as_i64(b);
        if (k < 0)
            k += cnt;
        out->type = 0;
        if (k < 0 || k >= cnt)
            break;
        dyn_bson_iter_init(&it, a.val, a.vlen);
        while (dyn_bson_iter_next(&it) > 0 && k-- > 0) {
        }
        {
            dyn_bson_val el = dyn_bson_iter_val(&it);
            xv_ext(out, el);
            if (av[0].inbuf) {
                out->inbuf = 1;
                out->off = (uint32_t)(el.val - c->buf.p);
            }
        }
        break;
    }
    case O_IN: {
        dyn_bson_val a = dd_xv_val(c, &av[0]), b = dd_xv_val(c, &av[1]);
        dyn_bson_iter it;
        int found = 0;
        if (b.type != DYN_BSON_ARRAY) {
            rc = dd_fail(c->e, 40081, "$in requires an array as a second argument, found: %s", type_alias(b.type));
            break;
        }
        if (a.type == 0)
            a.type = DYN_BSON_UNDEFINED;
        dyn_bson_iter_init(&it, b.val, b.vlen);
        while (!found && dyn_bson_iter_next(&it) > 0)
            found = dyn_bson_cmp(a, dyn_bson_iter_val(&it)) == 0;
        rc = put_bool(c, found, out);
        break;
    }
    case O_CONCATARR:
    case O_SLICE: {
        dyn_bson_w w;
        uint32_t idx = 0;
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        if (x->op == O_CONCATARR) {
            for (i = 0; i < x->nk; i++) {
                dyn_bson_val a = dd_xv_val(c, &av[i]);
                dyn_bson_iter it;
                if (is_nullish(a.type)) {
                    dyn_bson_w_free(&w);
                    rc = put_null(c, out);
                    goto done;
                }
                if (a.type != DYN_BSON_ARRAY) {
                    dyn_bson_w_free(&w);
                    rc = dd_fail(c->e, 28664, "$concatArrays only supports arrays, not %s", type_alias(a.type));
                    goto done;
                }
                dyn_bson_iter_init(&it, a.val, a.vlen);
                while (dyn_bson_iter_next(&it) > 0) {
                    dyn_bson_val el = dyn_bson_iter_val(&it);
                    dyn_bson_w_index_key(&w, el.type, idx++);
                    dyn_bson_w_bytes(&w, el.val, el.vlen);
                }
            }
        } else {
            dyn_bson_val a = dd_xv_val(c, &av[0]);
            int64_t cnt, from, num, k = 0;
            dyn_bson_iter it;
            if (is_nullish(a.type)) {
                dyn_bson_w_free(&w);
                rc = put_null(c, out);
                goto done;
            }
            if (a.type != DYN_BSON_ARRAY) {
                dyn_bson_w_free(&w);
                rc = dd_fail(c->e, 28724, "First argument to $slice must be an array, but is of type: %s", type_alias(a.type));
                goto done;
            }
            cnt = arr_count(a);
            if (x->nk == 2) {
                num = as_i64(dd_xv_val(c, &av[1]));
                from = num >= 0 ? 0 : (cnt + num < 0 ? 0 : cnt + num);
                num = num >= 0 ? num : -num;
            } else {
                from = as_i64(dd_xv_val(c, &av[1]));
                num = as_i64(dd_xv_val(c, &av[2]));
                if (from < 0)
                    from = cnt + from < 0 ? 0 : cnt + from;
                if (num <= 0) {
                    dyn_bson_w_free(&w);
                    rc = dd_fail(c->e, 28729, "Third argument to $slice must be positive");
                    goto done;
                }
            }
            dyn_bson_iter_init(&it, a.val, a.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                if (k >= from && k < from + num) {
                    dyn_bson_val el = dyn_bson_iter_val(&it);
                    dyn_bson_w_index_key(&w, el.type, idx++);
                    dyn_bson_w_bytes(&w, el.val, el.vlen);
                }
                k++;
            }
        }
        dyn_bson_w_doc_end(&w);
        rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_ARRAY, w.p, (uint32_t)w.n, out);
        dyn_bson_w_free(&w);
        break;
    }
    case O_TYPE: {
        const char* t = type_alias(av[0].type);
        rc = put_str(c, t, strlen(t), out);
        break;
    }
    case O_TOINT:
    case O_TOLONG:
    case O_TODOUBLE:
    case O_TOBOOL: {
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        if (is_nullish(a.type)) {
            rc = put_null(c, out);
            break;
        }
        if (x->op == O_TOBOOL) {
            rc = put_bool(c, dd_xv_truthy(c, &av[0]), out);
            break;
        }
        if (a.type == DYN_BSON_STRING) {
            const char* s = (const char*)a.val + 4;
            char* end;
            double d = strtod(s, &end);
            if (end == s || *end) {
                rc = dd_fail(c->e, 241, "Failed to parse number '%s' in $convert", s);
                break;
            }
            rc = x->op == O_TODOUBLE ? put_f64(c, d, out) : x->op == O_TOINT ? put_i32(c, (int32_t)d, out)
                                                                             : put_i64_exact(c, (int64_t)d, out);
            break;
        }
        if (a.type == DYN_BSON_BOOL || dyn_bson_is_number(a.type) || a.type == DYN_BSON_DATE) {
            double d = a.type == DYN_BSON_BOOL ? (double)a.val[0] : a.type == DYN_BSON_DATE ? (double)(int64_t)dyn_bson_rd64(a.val)
                                                                                            : dyn_bson_num_f64(a);
            if (x->op == O_TODOUBLE)
                rc = put_f64(c, d, out);
            else if (d != d || d >= 9.3e18 || d <= -9.3e18 || (x->op == O_TOINT && (d >= 2147483648.0 || d < -2147483648.0)))
                rc = dd_fail(c->e, 241, "Conversion would overflow target type in $convert");
            else if (x->op == O_TOINT)
                rc = put_i32(c, (int32_t)d, out);
            else
                rc = put_i64_exact(c, a.type == DYN_BSON_INT64 || a.type == DYN_BSON_DATE ? (int64_t)dyn_bson_rd64(a.val) : (int64_t)d, out);
            break;
        }
        rc = dd_fail(c->e, 241, "Unsupported conversion from %s in $convert", type_alias(a.type));
        break;
    }
    case O_YEAR:
    case O_MONTH:
    case O_DAY:
    case O_HOUR:
    case O_MINUTE:
    case O_SECOND:
    case O_MILLI:
    case O_DOW:
    case O_DOY: {
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        int64_t r;
        if (is_nullish(a.type)) {
            rc = put_null(c, out);
            break;
        }
        if (a.type != DYN_BSON_DATE && a.type != DYN_BSON_OID && a.type != DYN_BSON_TIMESTAMP) {
            rc = dd_fail(c->e, 16006, "can't convert from BSON type %s to Date", type_alias(a.type));
            break;
        }
        date_part(x->op,
            a.type == DYN_BSON_DATE      ? (int64_t)dyn_bson_rd64(a.val)
                : a.type == DYN_BSON_OID ? (int64_t)(((uint32_t)a.val[0] << 24) | ((uint32_t)a.val[1] << 16) | ((uint32_t)a.val[2] << 8) | a.val[3]) * 1000
                                         : (int64_t)dyn_bson_rd32(a.val + 4) * 1000,
            &r);
        rc = put_i32(c, (int32_t)r, out);
        break;
    }
    case O_SUMA:
    case O_AVGA:
    case O_MINA:
    case O_MAXA: {
        int any_double = 0, any_long = 0, overflow = 0, cnt = 0, best = -1;
        int64_t isum = 0;
        double dsum = 0;
        dd_xv* items = av;
        uint32_t nitems = x->nk, j;
        dd_xv* tmp = NULL;
        if (x->nk == 1 && av[0].type == DYN_BSON_ARRAY) {
            dyn_bson_val a = dd_xv_val(c, &av[0]);
            dyn_bson_iter it;
            nitems = (uint32_t)arr_count(a);
            tmp = malloc((size_t)(nitems ? nitems : 1) * sizeof *tmp);
            if (!tmp) {
                rc = dd_fail(c->e, DD_E_OOM, "out of memory");
                break;
            }
            j = 0;
            dyn_bson_iter_init(&it, a.val, a.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                dyn_bson_val el = dyn_bson_iter_val(&it);
                xv_ext(&tmp[j], el);
                if (av[0].inbuf) {
                    tmp[j].inbuf = 1;
                    tmp[j].off = (uint32_t)(el.val - c->buf.p);
                }
                j++;
            }
            items = tmp;
        }
        for (j = 0; j < nitems; j++) {
            dyn_bson_val v = dd_xv_val(c, &items[j]);
            if (x->op == O_MINA || x->op == O_MAXA) {
                if (is_nullish(v.type))
                    continue;
                if (best < 0 || (x->op == O_MINA ? xcmp(c, &items[j], &items[best]) < 0 : xcmp(c, &items[j], &items[best]) > 0))
                    best = (int)j;
                continue;
            }
            if (!dyn_bson_is_number(v.type))
                continue;
            cnt++;
            if (v.type == DYN_BSON_DOUBLE || v.type == DYN_BSON_DEC128)
                any_double = 1;
            if (v.type == DYN_BSON_INT64)
                any_long = 1;
            dsum += dyn_bson_num_f64(v);
            if (!overflow && __builtin_add_overflow(isum, as_i64(v), &isum))
                overflow = 1;
        }
        if (x->op == O_MINA || x->op == O_MAXA) {
            if (best < 0)
                rc = put_null(c, out);
            else
                *out = items[best];
        } else if (x->op == O_AVGA) {
            rc = cnt ? put_f64(c, dsum / cnt, out) : put_null(c, out);
        } else {
            rc = num_result(c, any_double, any_long, overflow, isum, dsum, out);
        }
        free(tmp);
        break;
    }
    case O_TOARRAYOBJ: {
        dyn_bson_val a = dd_xv_val(c, &av[0]);
        dyn_bson_w w;
        dyn_bson_iter it;
        uint32_t idx = 0;
        if (is_nullish(a.type)) {
            rc = put_null(c, out);
            break;
        }
        if (a.type != DYN_BSON_DOC) {
            rc = dd_fail(c->e, 40390, "$objectToArray requires a document input, found: %s", type_alias(a.type));
            break;
        }
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        dyn_bson_iter_init(&it, a.val, a.vlen);
        while (dyn_bson_iter_next(&it) > 0) {
            dyn_bson_w_index_key(&w, DYN_BSON_DOC, idx++);
            dyn_bson_w_doc_begin(&w);
            dyn_bson_w_string(&w, "k", 1, it.key, it.klen);
            dyn_bson_w_value(&w, "v", 1, dyn_bson_iter_val(&it));
            dyn_bson_w_doc_end(&w);
        }
        dyn_bson_w_doc_end(&w);
        rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_ARRAY, w.p, (uint32_t)w.n, out);
        dyn_bson_w_free(&w);
        break;
    }
    case O_MERGEOBJ: {
        dyn_bson_w w;
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        for (i = 0; i < x->nk; i++) {
            dyn_bson_val a = dd_xv_val(c, &av[i]);
            dyn_bson_iter it;
            if (is_nullish(a.type))
                continue;
            if (a.type != DYN_BSON_DOC) {
                dyn_bson_w_free(&w);
                rc = dd_fail(c->e, 40400, "$mergeObjects requires object inputs, but input is of type %s", type_alias(a.type));
                goto done;
            }
            dyn_bson_iter_init(&it, a.val, a.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                uint32_t j2;
                int earlier = 0;
                dyn_bson_val best = dyn_bson_iter_val(&it), tmpv;
                for (j2 = 0; j2 < i && !earlier; j2++) {
                    dyn_bson_val b = dd_xv_val(c, &av[j2]);
                    earlier = b.type == DYN_BSON_DOC && dyn_bson_find(b.val, it.key, it.klen, &tmpv);
                }
                if (earlier)
                    continue;
                for (j2 = i + 1; j2 < x->nk; j2++) {
                    dyn_bson_val b = dd_xv_val(c, &av[j2]);
                    if (b.type == DYN_BSON_DOC && dyn_bson_find(b.val, it.key, it.klen, &tmpv))
                        best = tmpv;
                }
                dyn_bson_w_value(&w, it.key, it.klen, best);
            }
        }
        dyn_bson_w_doc_end(&w);
        rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_DOC, w.p, (uint32_t)w.n, out);
        dyn_bson_w_free(&w);
        break;
    }
    default:
        rc = dd_fail(c->e, DD_E_INTERNAL, "unhandled expression operator %d", x->op);
        break;
    }
done:
    if (av != avs)
        free(av);
    return rc;
fail:
    if (av != avs)
        free(av);
    return -1;
}

static int eval_obj(const dd_xn* x, dd_xctx* c, dd_xv* out)
{
    dd_xv* vals = malloc((size_t)(x->nk ? x->nk : 1) * sizeof *vals);
    dyn_bson_w w;
    uint32_t i;
    int rc;
    if (!vals)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    for (i = 0; i < x->nk; i++)
        if (dd_expr_eval(x->k[i], c, &vals[i]) < 0) {
            free(vals);
            return -1;
        }
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    for (i = 0; i < x->nk; i++) {
        dyn_bson_val v = dd_xv_val(c, &vals[i]);
        if (v.type == 0)
            continue;
        dyn_bson_w_value(&w, x->keys[i], x->klens[i], v);
    }
    dyn_bson_w_doc_end(&w);
    free(vals);
    rc = w.oom ? dd_fail(c->e, DD_E_OOM, "out of memory") : dd_xv_put(c, DYN_BSON_DOC, w.p, (uint32_t)w.n, out);
    dyn_bson_w_free(&w);
    return rc;
}

static int eval_arr(const dd_xn* x, dd_xctx* c, dd_xv* out)
{
    dd_xv* vals = malloc((size_t)(x->nk ? x->nk : 1) * sizeof *vals);
    uint32_t i;
    int rc;
    if (!vals)
        return dd_fail(c->e, DD_E_OOM, "out of memory");
    for (i = 0; i < x->nk; i++)
        if (dd_expr_eval(x->k[i], c, &vals[i]) < 0) {
            free(vals);
            return -1;
        }
    rc = build_array(c, vals, x->nk, out);
    free(vals);
    return rc;
}

static int as_name(const dd_xn* asx, const char** name, size_t* n)
{
    if (!asx || (asx->kind == X_LIT && asx->lit.type == 0)) {
        *name = "this";
        *n = 4;
        return 0;
    }
    if (asx->kind != X_LIT || asx->lit.type != DYN_BSON_STRING)
        return -1;
    *name = (const char*)asx->lit.val + 4;
    *n = dyn_bson_rd32(asx->lit.val) - 1;
    return 0;
}

static int eval_mapfilter(const dd_xn* x, dd_xctx* c, dd_xv* out)
{
    dd_xv in, *items = NULL;
    dyn_bson_val a;
    const char* nm;
    size_t nl;
    uint32_t n = 0, cap = 0;
    uint8_t* own = NULL;
    dyn_bson_iter it;
    int rc = 0;
    if (dd_expr_eval(x->k[0], c, &in) < 0)
        return -1;
    a = dd_xv_val(c, &in);
    if (is_nullish(a.type))
        return put_null(c, out);
    if (a.type != DYN_BSON_ARRAY)
        return dd_fail(c->e, 16883, "input to %s must be an array not %s", x->kind == X_MAP ? "$map" : "$filter", type_alias(a.type));
    if (as_name(x->k[1], &nm, &nl) < 0)
        return dd_fail(c->e, DD_E_FAILEDTOPARSE, "'as' must be a string");
    if (c->nvars >= DD_XVARS)
        return dd_fail(c->e, DD_E_BADVALUE, "too many nested $map/$filter variables");
    if (in.inbuf) {
        own = malloc(a.vlen ? a.vlen : 1);
        if (!own)
            return dd_fail(c->e, DD_E_OOM, "out of memory");
        memcpy(own, a.val, a.vlen);
        a.val = own;
    }
    dyn_bson_iter_init(&it, a.val, a.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        dd_xv el, r;
        xv_ext(&el, dyn_bson_iter_val(&it));
        c->vars[c->nvars].name = nm;
        c->vars[c->nvars].nlen = nl;
        c->vars[c->nvars].v = el;
        c->nvars++;
        rc = dd_expr_eval(x->k[2], c, &r);
        c->nvars--;
        if (rc < 0)
            goto out;
        if (x->kind == X_FILTER) {
            if (!dd_xv_truthy(c, &r))
                continue;
            r = el;
        }
        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 8;
            dd_xv* ni = realloc(items, (size_t)nc * sizeof *ni);
            if (!ni) {
                rc = dd_fail(c->e, DD_E_OOM, "out of memory");
                goto out;
            }
            items = ni;
            cap = nc;
        }
        items[n++] = r;
    }
    rc = build_array(c, items, n, out);
out:
    free(items);
    free(own);
    return rc;
}

int dd_expr_eval(const dd_xn* x, dd_xctx* c, dd_xv* out)
{
    switch (x->kind) {
    case X_LIT:
        xv_ext(out, x->lit);
        return 0;
    case X_FIELD: {
        dyn_bson_val r = { DYN_BSON_DOC, c->root, dyn_bson_rd32(c->root) };
        return eval_field(&x->path, r, c, out);
    }
    case X_VAR: {
        dd_xv base;
        int f = lookup_var(c, x->var, x->varlen, &base);
        if (f < 0)
            return -1;
        if (!f)
            return dd_fail(c->e, 17276, "Use of undefined variable: %s", x->var);
        if (!x->has_path) {
            *out = base;
            return 0;
        }
        return eval_field_from(&x->path, 0, dd_xv_val(c, &base), c, out);
    }
    case X_OBJ:
        return eval_obj(x, c, out);
    case X_ARR:
        return eval_arr(x, c, out);
    case X_MAP:
    case X_FILTER:
        return eval_mapfilter(x, c, out);
    case X_SWITCH: {
        uint32_t i, nb = x->nk - (x->op ? 1 : 0);
        for (i = 0; i + 1 < nb; i += 2) {
            dd_xv t;
            if (dd_expr_eval(x->k[i], c, &t) < 0)
                return -1;
            if (dd_xv_truthy(c, &t))
                return dd_expr_eval(x->k[i + 1], c, out);
        }
        if (x->op)
            return dd_expr_eval(x->k[x->nk - 1], c, out);
        return dd_fail(c->e, 40066, "$switch could not find a matching branch for an input, and no default was specified.");
    }
    default:
        return eval_op(x, c, out);
    }
}
