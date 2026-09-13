/* A compiled arithmetic expression for dyna:mathx (design 26).
   Shunting-yard to an RPN program over doubles. THERE IS NO eval AND NO SCOPE:
   an identifier is a variable or one of the functions listed here, nothing
   reaches an object, and that is the whole reason to have this rather than
   reaching for a JS evaluator on config-driven formulas. Full API: see the module header. */

#define EX_MAX_SRC   4096u
#define EX_MAX_OPS   4096u
#define EX_MAX_STACK 256

enum { EX_NUM, EX_VAR, EX_OP, EX_FN };
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG };

/* Every function is resolved to the SAME C routine the module already exports,
   so there is one implementation of each, not two. */
static const struct { const char *name; int argc; } EX_FN_TAB[] = {
    {"sin",1},{"cos",1},{"tan",1},{"asin",1},{"acos",1},{"atan",1},
    {"sinh",1},{"cosh",1},{"tanh",1},{"sqrt",1},{"cbrt",1},{"exp",1},
    {"log",1},{"log2",1},{"log10",1},{"abs",1},{"floor",1},{"ceil",1},
    {"round",1},{"trunc",1},{"sign",1},{"expm1",1},{"log1p",1},
    {"atan2",2},{"pow",2},{"hypot",2},{"min",2},{"max",2},{"fmod",2},
};

/* kind selects exactly one payload, so they overlap: as separate members the
   double forced 8-byte alignment around them and a third of every instruction
   was padding. 24 -> 16 bytes, and the code array is an array of these. */
typedef struct {
    uint8_t  kind;
    uint8_t  op;                        /* EX_OP */
    union {
        uint16_t fn;                    /* EX_FN: index into EX_FN_TAB */
        double   num;                   /* EX_NUM */
        uint32_t var;                   /* EX_VAR: index into names */
    } u;
} ex_ins_t;

_Static_assert(sizeof(ex_ins_t) == 16, "ex_ins_t regained padding");

typedef struct {
    ex_ins_t *code;  uint32_t n, cap;
    char    **names; uint32_t nname, cname;   /* the free variables, in order */
    uint32_t  max_depth;                      /* the value stack this needs */
} expr_t;

static void expr_free(expr_t *e)
{
    uint32_t i;
    if (!e)
        return;
    for (i = 0; i < e->nname; i++)
        free(e->names[i]);
    free(e->names);
    free(e->code);
    free(e);
}

static void expr_free_v(void *p) { expr_free((expr_t *)p); }

static int ex_emit(expr_t *e, const ex_ins_t *ins)
{
    if (e->n == e->cap) {
        uint32_t nc = e->cap ? e->cap * 2 : 32;
        ex_ins_t *np;
        if (nc > EX_MAX_OPS)
            return -1;
        np = (ex_ins_t *)realloc(e->code, nc * sizeof *np);
        if (!np)
            return -1;
        e->code = np; e->cap = nc;
    }
    e->code[e->n++] = *ins;
    return 0;
}

static int ex_var_index(expr_t *e, const char *s, size_t n, uint32_t *out)
{
    uint32_t i;
    char *d;

    for (i = 0; i < e->nname; i++)
        if (strlen(e->names[i]) == n && memcmp(e->names[i], s, n) == 0) {
            *out = i;
            return 0;
        }
    if (e->nname == e->cname) {
        uint32_t nc = e->cname ? e->cname * 2 : 8;
        char **np = (char **)realloc(e->names, nc * sizeof *np);
        if (!np)
            return -1;
        e->names = np; e->cname = nc;
    }
    d = (char *)malloc(n + 1);
    if (!d)
        return -1;
    memcpy(d, s, n);
    d[n] = 0;
    e->names[e->nname] = d;
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
    case OP_ADD: case OP_SUB: return 1;
    case OP_MUL: case OP_DIV: case OP_MOD: return 2;
    case OP_POW: return 3;
    /* Unary minus sits WITH `^`, not above it, and is right-associative: at a
       higher precedence `-x^2` is (-x)^2 = 9 rather than -(x^2) = -9, which is
       what every calculator and JS's own ** operator mean. */
    default: return 3;                  /* OP_NEG */
    }
}

/* `^` is right-associative, which is the one place a left-associative shunting
   yard silently computes 2^3^2 as 64 instead of 512. */
static int ex_right_assoc(int op) { return op == OP_POW || op == OP_NEG; }

typedef struct {
    int      op;
    int      is_fn;
    uint16_t fn;
    uint32_t argc;
} ex_stk_t;

typedef struct {
    JSContext *ctx;
    const char *s;
    size_t n, i;
    expr_t *e;
    char err[128];
} ex_parse_t;

static int ex_fail(ex_parse_t *p, const char *what)
{
    if (!p->err[0])
        snprintf(p->err, sizeof p->err, "%s at offset %u", what, (unsigned)p->i);
    return -1;
}

static int ex_pop_op(ex_parse_t *p, ex_stk_t *st, int *sp)
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

/* The whole grammar: numbers, identifiers, five binary operators, unary minus,
   parentheses and a comma for the two-argument functions. */
static int ex_compile_into(ex_parse_t *p)
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
            JSValue sv;
            double d;
            if (!want_value)
                return ex_fail(p, "two values with no operator between them");
            while (p->i < p->n && ((p->s[p->i] >= '0' && p->s[p->i] <= '9')
                   || p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E'
                   || ((p->s[p->i] == '+' || p->s[p->i] == '-')
                       && (p->s[p->i - 1] == 'e' || p->s[p->i - 1] == 'E'))))
                p->i++;
            /* The engine's own ToNumber over the literal's text: correctly
               rounded and locale-independent, where strtod reads LC_NUMERIC. */
            sv = JS_NewStringLen(p->ctx, p->s + st0, p->i - st0);
            if (JS_IsException(sv))
                return ex_fail(p, "out of memory");
            if (JS_ToFloat64(p->ctx, &d, sv) < 0) {
                JS_FreeValue(p->ctx, sv);
                JS_GetException(p->ctx);
                return ex_fail(p, "bad number");
            }
            JS_FreeValue(p->ctx, sv);
            if (d != d)
                return ex_fail(p, "bad number");
            memset(&ins, 0, sizeof ins);
            ins.kind = EX_NUM;
            ins.u.num = d;
            if (ex_emit(p->e, &ins) < 0)
                return ex_fail(p, "expression too long");
            depth++;
            if (depth > maxd) maxd = depth;
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
                for (k = 0; k < countof(EX_FN_TAB); k++)
                    if (strlen(EX_FN_TAB[k].name) == len
                        && memcmp(EX_FN_TAB[k].name, p->s + st0, len) == 0)
                        break;
                if (k >= countof(EX_FN_TAB)) {
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
                p->i++;                 /* the '(' belongs to the call */
                if (sp >= EX_MAX_STACK)
                    return ex_fail(p, "nested too deeply");
                st[sp].is_fn = 0;
                st[sp].op = -1;         /* the matching paren marker */
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
            if (depth > maxd) maxd = depth;
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
            /* `()` and `f()` and `f(1,)` all reach here with no value produced.
               Accepting them emits a call with no operand, and the RPN then
               reads one slot BELOW the value stack -- a real overread, not a
               laxness. */
            if (want_value)
                return ex_fail(p, c == ')' ? "empty parentheses"
                                           : "a missing argument before the comma");
            while (sp > 0 && !(st[sp - 1].op == -1 && !st[sp - 1].is_fn)) {
                if (ex_pop_op(p, st, &sp) < 0)
                    return -1;
                /* OP_NEG folds in place and consumes no value-stack slot, so
                   it must not shrink `depth` here either -- the operator path
                   below skips it for the same reason, and an asymmetric count
                   can drive `depth` (unsigned) below zero and corrupt maxd. */
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
            sp--;                       /* drop the paren marker */
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
            case '+': op = want_value ? -2 : OP_ADD; break;
            case '-': op = want_value ? OP_NEG : OP_SUB; break;
            case '*': op = OP_MUL; break;
            case '/': op = OP_DIV; break;
            case '%': op = OP_MOD; break;
            case '^': op = OP_POW; break;
            default:
                snprintf(p->err, sizeof p->err, "unexpected character '%c'", c);
                return -1;
            }
            p->i++;
            if (op == -2) {             /* a unary plus is the identity */
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

/* ------------------------------------------------------------ evaluation */

static double ex_apply1(int fn, double a)
{
    switch (fn) {
    case 0: return sin(a);   case 1: return cos(a);   case 2: return tan(a);
    case 3: return asin(a);  case 4: return acos(a);  case 5: return atan(a);
    case 6: return sinh(a);  case 7: return cosh(a);  case 8: return tanh(a);
    case 9: return sqrt(a);  case 10: return cbrt(a); case 11: return exp(a);
    case 12: return log(a);  case 13: return log2(a); case 14: return log10(a);
    case 15: return fabs(a); case 16: return floor(a); case 17: return ceil(a);
    case 18: return round(a); case 19: return trunc(a);
    case 20: return a > 0 ? 1 : (a < 0 ? -1 : a);
    case 21: return expm1(a); default: return log1p(a);
    }
}

static double ex_apply2(int fn, double a, double b)
{
    switch (fn) {
    case 23: return atan2(a, b);
    case 24: return pow(a, b);
    case 25: return hypot(a, b);
    /* min/max propagate NaN like Math.min/Math.max: C's a<b?a:b would drop
       it (the comparison is false, so min(NaN,1) returned 1). */
    case 26: return (isnan(a) || isnan(b)) ? NAN : (a < b ? a : b);
    case 27: return (isnan(a) || isnan(b)) ? NAN : (a > b ? a : b);
    default: return fmod(a, b);
    }
}

static double ex_apply_op(int op, double a, double b)
{
    switch (op) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_MUL: return a * b;
    case OP_DIV: return a / b;
    case OP_MOD: return fmod(a, b);
    default:     return pow(a, b);
    }
}

static JSClassID dyn_expr_class_id;

static void dyn_expr_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    expr_free((expr_t *)JS_GetOpaque(val, dyn_expr_class_id));
}

static const JSClassDef dyn_expr_class = {
    "Expression", .finalizer = dyn_expr_finalizer,
};

static JSValue dyn_expr_ctor(JSContext *ctx, JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    ex_parse_t p;
    const char *s;
    size_t n;
    expr_t *e;

    (void)new_target;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Expression(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > EX_MAX_SRC) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new Expression: exceeds %u bytes",
                                  EX_MAX_SRC);
    }
    e = (expr_t *)calloc(1, sizeof *e);
    if (!e) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(&p, 0, sizeof p);
    p.ctx = ctx; p.s = s; p.n = n; p.e = e;
    if (ex_compile_into(&p) < 0) {
        JS_FreeCString(ctx, s);
        expr_free(e);
        return JS_ThrowSyntaxError(ctx, "new Expression: %s",
                                   p.err[0] ? p.err : "out of memory");
    }
    JS_FreeCString(ctx, s);
    return dyn_plain_wrap(ctx, dyn_expr_class_id, e, expr_free_v);
}

static JSValue dyn_expr_eval(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    expr_t *e = (expr_t *)dyn_plain_get(ctx, this_val, dyn_expr_class_id);
    double *vals = NULL, *stk = NULL;
    uint32_t i;
    int sp = 0;
    JSValue r = JS_EXCEPTION;

    if (!e)
        return JS_EXCEPTION;
    if (e->nname && (argc < 1 || !JS_IsObject(argv[0])))
        return JS_ThrowTypeError(ctx,
            "Expression.eval(vars): this expression has variables, so it needs "
            "an object of values");
    vals = (double *)calloc(e->nname ? e->nname : 1, sizeof *vals);
    stk = (double *)calloc(e->max_depth ? e->max_depth : 1, sizeof *stk);
    if (!vals || !stk) {
        free(vals); free(stk);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < e->nname; i++) {
        /* An OWN data property only: a getter would run code, which is the one
           thing this evaluator exists not to do. */
        JSAtom a = JS_NewAtom(ctx, e->names[i]);
        JSPropertyDescriptor d;
        int got;
        if (a == JS_ATOM_NULL)
            goto done;
        got = JS_GetOwnProperty(ctx, &d, argv[0], a);
        JS_FreeAtom(ctx, a);
        if (got < 0)
            goto done;
        if (got == 0 || (d.flags & JS_PROP_GETSET)) {
            if (got) {
                JS_FreeValue(ctx, d.getter);
                JS_FreeValue(ctx, d.setter);
                JS_FreeValue(ctx, d.value);
            }
            JS_ThrowRangeError(ctx, "Expression.eval: no value for %s",
                               e->names[i]);
            goto done;
        }
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
        if (!JS_IsNumber(d.value)) {
            JS_FreeValue(ctx, d.value);
            JS_ThrowTypeError(ctx, "Expression.eval: %s must be a number",
                              e->names[i]);
            goto done;
        }
        if (JS_ToFloat64(ctx, &vals[i], d.value) < 0) {
            JS_FreeValue(ctx, d.value);
            goto done;
        }
        JS_FreeValue(ctx, d.value);
    }
    for (i = 0; i < e->n; i++) {
        const ex_ins_t *in = &e->code[i];
        int need = in->kind == EX_NUM || in->kind == EX_VAR ? 0
                 : (in->kind == EX_OP ? (in->op == OP_NEG ? 1 : 2)
                                      : EX_FN_TAB[in->u.fn].argc);
        /* Unreachable by construction -- the parser refuses every source that
           would underflow -- and checked anyway, because the failure would be
           a read below the stack rather than a wrong answer. */
        if (sp < need || sp >= (int)e->max_depth + 2) {
            JS_ThrowInternalError(ctx, "Expression: the compiled program is "
                                       "inconsistent with its stack");
            goto done;
        }
        switch (in->kind) {
        case EX_NUM: stk[sp++] = in->u.num; break;
        case EX_VAR: stk[sp++] = vals[in->u.var]; break;
        case EX_OP:
            if (in->op == OP_NEG) { stk[sp - 1] = -stk[sp - 1]; break; }
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
    r = JS_NewFloat64(ctx, sp ? stk[sp - 1] : 0);
done:
    free(vals);
    free(stk);
    return r;
}

static JSValue dyn_expr_vars(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    expr_t *e = (expr_t *)dyn_plain_get(ctx, this_val, dyn_expr_class_id);
    JSValue out;
    uint32_t i;

    (void)argc; (void)argv;
    if (!e)
        return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < e->nname; i++)
        if (JS_DefinePropertyValueUint32(ctx, out, i,
                                         JS_NewString(ctx, e->names[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    return out;
}

static const JSCFunctionListEntry dyn_expr_proto[] = {
    JS_CFUNC_DEF("eval", 0, dyn_expr_eval),
    JS_CFUNC_DEF("variables", 0, dyn_expr_vars),
};
