#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exprk.h"
#include "../dyn-cdefs.h"

typedef struct {
    int op;
    int is_fn;
    uint16_t fn;
    uint32_t argc;
} ex_stk_t;

const exprk_fn_t EX_FN_TAB[EXPRK_NFN] = {
    { "sin", 1 },
    { "cos", 1 },
    { "tan", 1 },
    { "asin", 1 },
    { "acos", 1 },
    { "atan", 1 },
    { "sinh", 1 },
    { "cosh", 1 },
    { "tanh", 1 },
    { "sqrt", 1 },
    { "cbrt", 1 },
    { "exp", 1 },
    { "log", 1 },
    { "log2", 1 },
    { "log10", 1 },
    { "abs", 1 },
    { "floor", 1 },
    { "ceil", 1 },
    { "round", 1 },
    { "trunc", 1 },
    { "sign", 1 },
    { "expm1", 1 },
    { "log1p", 1 },
    { "atan2", 2 },
    { "pow", 2 },
    { "hypot", 2 },
    { "min", 2 },
    { "max", 2 },
    { "fmod", 2 },
};

void exprk_free(expr_t* e)
{
    uint32_t i;
    if (!e)
        return;
    for (i = 0; i < e->nname; i++)
        free(e->names[i]);
    free(e->names);
    free(e->nlens);
    free(e->code);
    free(e);
}

static int ex_emit(expr_t* e, const ex_ins_t* ins)
{
    if (e->n == e->cap) {
        uint32_t nc = e->cap ? e->cap * 2 : 32;
        ex_ins_t* np;
        if (nc > EX_MAX_OPS)
            return -1;
        np = (ex_ins_t*)realloc(e->code, nc * sizeof *np);
        if (!np)
            return -1;
        e->code = np;
        e->cap = nc;
    }
    e->code[e->n++] = *ins;
    return 0;
}

static int ex_var_index(expr_t* e, const char* s, size_t n, uint32_t* out)
{
    uint32_t i;
    char* d;

    for (i = 0; i < e->nname; i++)
        if (e->nlens[i] == n && memcmp(e->names[i], s, n) == 0) {
            *out = i;
            return 0;
        }
    if (e->nname == e->cname) {
        uint32_t nc = e->cname ? e->cname * 2 : 8;
        char** np = (char**)realloc(e->names, nc * sizeof *np);
        uint32_t* nl;
        if (!np)
            return -1;
        e->names = np;
        nl = (uint32_t*)realloc(e->nlens, nc * sizeof *nl);
        if (!nl)
            return -1;
        e->nlens = nl;
        e->cname = nc;
    }
    d = (char*)malloc(n + 1);
    if (!d)
        return -1;
    memcpy(d, s, n);
    d[n] = 0;
    e->names[e->nname] = d;
    e->nlens[e->nname] = (uint32_t)n;
    *out = e->nname++;
    return 0;
}

static int ex_ident_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static int ex_prec(int op)
{
    switch (op) {
    case OP_ADD:
    case OP_SUB:
        return 1;
    case OP_MUL:
    case OP_DIV:
    case OP_MOD:
        return 2;
    case OP_POW:
        return 3;
    default:
        return 3;
    }
}

static int ex_right_assoc(int op) { return op == OP_POW || op == OP_NEG; }

static int ex_fail(ex_parse_t* p, const char* what)
{
    if (!p->err[0])
        snprintf(p->err, sizeof p->err, "%s at offset %u", what, (unsigned)p->i);
    return -1;
}

static int ex_pop_op(ex_parse_t* p, ex_stk_t* st, int* sp)
{
    ex_ins_t ins;
    memset(&ins, 0, sizeof ins);
    if (st[*sp - 1].is_fn) {
        ins.kind = EX_FN;
        ins.u.fn = st[*sp - 1].fn;
    } else {
        ins.kind = EX_OP;
        ins.op = (uint8_t)st[*sp - 1].op;
    }
    (*sp)--;
    return ex_emit(p->e, &ins) < 0 ? ex_fail(p, "expression too long") : 0;
}

int exprk_compile(ex_parse_t* p)
{
    ex_stk_t st[EX_MAX_STACK];
    int sp = 0, want_value = 1;
    uint32_t depth = 0, maxd = 0;

    for (;;) {
        char c;
        while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t'))
            p->i++;
        if (p->i >= p->n)
            break;
        c = p->s[p->i];
        if ((c >= '0' && c <= '9') || (c == '.' && want_value)) {
            size_t st0 = p->i;
            ex_ins_t ins;
            double d;
            int nr;
            if (!want_value)
                return ex_fail(p, "two values with no operator between them");
            while (p->i < p->n && ((p->s[p->i] >= '0' && p->s[p->i] <= '9') || p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E' || ((p->s[p->i] == '+' || p->s[p->i] == '-') && (p->s[p->i - 1] == 'e' || p->s[p->i - 1] == 'E'))))
                p->i++;
            nr = p->num(p->ud, p->s + st0, p->i - st0, &d);
            if (nr == EXPRK_NUM_NOMEM)
                return ex_fail(p, "out of memory");
            if (nr != EXPRK_NUM_OK || d != d)
                return ex_fail(p, "bad number");
            memset(&ins, 0, sizeof ins);
            ins.kind = EX_NUM;
            ins.u.num = d;
            if (ex_emit(p->e, &ins) < 0)
                return ex_fail(p, "expression too long");
            depth++;
            if (depth > maxd)
                maxd = depth;
            want_value = 0;
            continue;
        }
        if (ex_ident_char((unsigned char)c) && !(c >= '0' && c <= '9')) {
            size_t st0 = p->i;
            size_t len;
            uint32_t k;
            ex_ins_t ins;
            while (p->i < p->n && ex_ident_char((unsigned char)p->s[p->i]))
                p->i++;
            len = p->i - st0;
            if (!want_value)
                return ex_fail(p, "two values with no operator between them");
            while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t'))
                p->i++;
            if (p->i < p->n && p->s[p->i] == '(') {
                for (k = 0; k < sizeof(EX_FN_TAB) / sizeof(EX_FN_TAB[0]); k++)
                    if (strlen(EX_FN_TAB[k].name) == len
                        && memcmp(EX_FN_TAB[k].name, p->s + st0, len) == 0)
                        break;
                if (k >= sizeof(EX_FN_TAB) / sizeof(EX_FN_TAB[0])) {
                    snprintf(p->err, sizeof p->err,
                        "no function named %.*s", (int)len, p->s + st0);
                    return -1;
                }
                if (sp >= EX_MAX_STACK)
                    return ex_fail(p, "nested too deeply");
                st[sp].is_fn = 1;
                st[sp].fn = (uint16_t)k;
                st[sp].argc = 1;
                st[sp].op = -1;
                sp++;
                p->i++;
                if (sp >= EX_MAX_STACK)
                    return ex_fail(p, "nested too deeply");
                st[sp].is_fn = 0;
                st[sp].op = -1;
                sp++;
                want_value = 1;
                continue;
            }
            memset(&ins, 0, sizeof ins);
            if (len == 2 && memcmp(p->s + st0, "pi", 2) == 0) {
                ins.kind = EX_NUM;
                ins.u.num = 3.14159265358979323846;
            } else if (len == 1 && p->s[st0] == 'e') {
                ins.kind = EX_NUM;
                ins.u.num = 2.71828182845904523536;
            } else {
                ins.kind = EX_VAR;
                if (ex_var_index(p->e, p->s + st0, len, &ins.u.var) < 0)
                    return ex_fail(p, "out of memory");
            }
            if (ex_emit(p->e, &ins) < 0)
                return ex_fail(p, "expression too long");
            depth++;
            if (depth > maxd)
                maxd = depth;
            want_value = 0;
            continue;
        }
        if (c == '(') {
            if (!want_value)
                return ex_fail(p, "a value cannot be followed by (");
            if (sp >= EX_MAX_STACK)
                return ex_fail(p, "nested too deeply");
            st[sp].is_fn = 0;
            st[sp].op = -1;
            sp++;
            p->i++;
            want_value = 1;
            continue;
        }
        if (c == ')' || c == ',') {
            if (want_value)
                return ex_fail(p, c == ')' ? "empty parentheses" : "a missing argument before the comma");
            while (sp > 0 && !(st[sp - 1].op == -1 && !st[sp - 1].is_fn)) {
                if (ex_pop_op(p, st, &sp) < 0)
                    return -1;
                if (st[sp].op != OP_NEG)
                    depth--;
            }
            if (sp == 0)
                return ex_fail(p, c == ')' ? "unmatched )" : "a comma outside a call");
            if (c == ',') {
                if (sp < 2 || !st[sp - 2].is_fn)
                    return ex_fail(p, "a comma outside a call");
                st[sp - 2].argc++;
                p->i++;
                want_value = 1;
                continue;
            }
            sp--;
            p->i++;
            if (sp > 0 && st[sp - 1].is_fn) {
                uint32_t want = (uint32_t)EX_FN_TAB[st[sp - 1].fn].argc;
                if (st[sp - 1].argc != want) {
                    snprintf(p->err, sizeof p->err, "%s takes %u argument%s",
                        EX_FN_TAB[st[sp - 1].fn].name, want,
                        want == 1 ? "" : "s");
                    return -1;
                }
                if (ex_pop_op(p, st, &sp) < 0)
                    return -1;
                depth -= want - 1;
            }
            want_value = 0;
            continue;
        }
        {
            int op;
            switch (c) {
            case '+':
                op = want_value ? -2 : OP_ADD;
                break;
            case '-':
                op = want_value ? OP_NEG : OP_SUB;
                break;
            case '*':
                op = OP_MUL;
                break;
            case '/':
                op = OP_DIV;
                break;
            case '%':
                op = OP_MOD;
                break;
            case '^':
                op = OP_POW;
                break;
            default:
                snprintf(p->err, sizeof p->err, "unexpected character '%c'", c);
                return -1;
            }
            p->i++;
            if (op == -2) {
                want_value = 1;
                continue;
            }
            if (op != OP_NEG && want_value)
                return ex_fail(p, "an operator with no value before it");
            while (sp > 0 && !(st[sp - 1].op == -1) && !st[sp - 1].is_fn
                && (ex_prec(st[sp - 1].op) > ex_prec(op)
                    || (ex_prec(st[sp - 1].op) == ex_prec(op)
                        && !ex_right_assoc(op)))) {
                if (ex_pop_op(p, st, &sp) < 0)
                    return -1;
                if (st[sp].op != OP_NEG)
                    depth--;
            }
            if (sp >= EX_MAX_STACK)
                return ex_fail(p, "nested too deeply");
            st[sp].is_fn = 0;
            st[sp].op = op;
            sp++;
            want_value = 1;
            continue;
        }
    }
    if (want_value)
        return ex_fail(p, "the expression ends where a value should be");
    while (sp > 0) {
        if (st[sp - 1].op == -1 && !st[sp - 1].is_fn)
            return ex_fail(p, "unmatched (");
        if (ex_pop_op(p, st, &sp) < 0)
            return -1;
    }
    p->e->max_depth = maxd + 2;
    return 0;
}

