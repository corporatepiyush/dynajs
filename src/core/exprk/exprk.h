#ifndef DYN_EXPRK_EXPRK_H
#define DYN_EXPRK_EXPRK_H

#include <stddef.h>
#include <math.h>
#include <stdint.h>

#include "../dyn-cdefs.h"

#define EX_MAX_SRC 4096u
#define EX_MAX_OPS 4096u
#define EX_MAX_STACK 256

enum { EX_NUM,
    EX_VAR,
    EX_OP,
    EX_FN };

enum { OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_POW,
    OP_NEG };

typedef struct {
    uint8_t kind;
    uint8_t op;
    union {
        uint16_t fn;
        double num;
        uint32_t var;
    } u;
} ex_ins_t;

_Static_assert(sizeof(ex_ins_t) == 16, "ex_ins_t regained padding");

typedef struct {
    ex_ins_t* code;
    uint32_t n, cap;
    char** names;
    uint32_t* nlens;
    uint32_t nname, cname;
    uint32_t max_depth;
} expr_t;

typedef struct {
    void* ud;
    int (*num)(void* ud, const char* s, size_t n, double* out);
    const char* s;
    size_t n, i;
    expr_t* e;
    char err[128];
} ex_parse_t;

enum { EXPRK_NUM_OK = 0,
    EXPRK_NUM_BAD = -1,
    EXPRK_NUM_NOMEM = -2 };

void exprk_free(expr_t* e);

int exprk_compile(ex_parse_t* p);


#define EXPRK_NFN 29

typedef struct {
    const char* name;
    int argc;
} exprk_fn_t;

extern const exprk_fn_t EX_FN_TAB[EXPRK_NFN];

static inline __attribute__((always_inline)) double ex_apply1(int fn, double a)
{
    switch (fn) {
    case 0:
        return sin(a);
    case 1:
        return cos(a);
    case 2:
        return tan(a);
    case 3:
        return asin(a);
    case 4:
        return acos(a);
    case 5:
        return atan(a);
    case 6:
        return sinh(a);
    case 7:
        return cosh(a);
    case 8:
        return tanh(a);
    case 9:
        return sqrt(a);
    case 10: {
        double r = cbrt(a);
        double whole = nearbyint(r);
        return (whole != r && whole * whole * whole == a) ? whole : r;
    }
    case 11:
        return exp(a);
    case 12:
        return log(a);
    case 13:
        return log2(a);
    case 14:
        return log10(a);
    case 15:
        return fabs(a);
    case 16:
        return floor(a);
    case 17:
        return ceil(a);
    case 18:
        return round(a);
    case 19:
        return trunc(a);
    case 20:
        return a > 0 ? 1 : (a < 0 ? -1 : a);
    case 21:
        return expm1(a);
    default:
        return log1p(a);
    }
}

static inline __attribute__((always_inline)) double ex_apply2(int fn, double a, double b)
{
    switch (fn) {
    case 23:
        return atan2(a, b);
    case 24:
        return pow(a, b);
    case 25:
        return hypot(a, b);
    case 26:
        return (isnan(a) || isnan(b)) ? DYN_NAN : (a < b ? a : b);
    case 27:
        return (isnan(a) || isnan(b)) ? DYN_NAN : (a > b ? a : b);
    default:
        return fmod(a, b);
    }
}

static inline __attribute__((always_inline)) double ex_apply_op(int op, double a, double b)
{
    switch (op) {
    case OP_ADD:
        return a + b;
    case OP_SUB:
        return a - b;
    case OP_MUL:
        return a * b;
    case OP_DIV:
        return a / b;
    case OP_MOD:
        return fmod(a, b);
    default:
        return pow(a, b);
    }
}

static inline int exprk_run(const expr_t* restrict e, const double* restrict vals, double* restrict stk, double* restrict out)
{
    const ex_ins_t* restrict code = e->code;
    uint32_t i, n = e->n;
    int sp = 0, lim = (int)e->max_depth + 2;

    for (i = 0; i < n; i++) {
        const ex_ins_t* in = &code[i];
        int need = in->kind == EX_NUM || in->kind == EX_VAR ? 0
                                                            : (in->kind == EX_OP ? (in->op == OP_NEG ? 1 : 2)
                                                                                 : EX_FN_TAB[in->u.fn].argc);
        if (sp < need || sp >= lim)
            return -1;
        switch (in->kind) {
        case EX_NUM:
            stk[sp++] = in->u.num;
            break;
        case EX_VAR:
            stk[sp++] = vals[in->u.var];
            break;
        case EX_OP:
            if (in->op == OP_NEG) {
                stk[sp - 1] = -stk[sp - 1];
                break;
            }
            stk[sp - 2] = ex_apply_op(in->op, stk[sp - 2], stk[sp - 1]);
            sp--;
            break;
        default:
            if (EX_FN_TAB[in->u.fn].argc == 1) {
                stk[sp - 1] = ex_apply1(in->u.fn, stk[sp - 1]);
            } else {
                stk[sp - 2] = ex_apply2(in->u.fn, stk[sp - 2], stk[sp - 1]);
                sp--;
            }
            break;
        }
    }
    *out = sp ? stk[sp - 1] : 0;
    return 0;
}

#endif
