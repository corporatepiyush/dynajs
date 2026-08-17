/* JSONPath -- the RFC 9535 subset, compiled once and reused (design 09).
   DATA-ONLY: an accessor property is skipped, never invoked, so a query cannot
   run user code. Grammar, subset and limits: see the parser source. */

#define JP_MAX_EXPR   4096u
#define JP_MAX_DEPTH  32          /* filter nesting; bounds the C recursion */
#define JP_MAX_NODES  (1u << 20)  /* a query is not a licence to allocate */

enum { JP_NAME, JP_INDEX, JP_WILD, JP_SLICE, JP_FILTER };
enum { JF_OR, JF_AND, JF_NOT, JF_CMP, JF_PATH, JF_NUM, JF_STR, JF_BOOL, JF_NULL };
enum { JC_EQ, JC_NE, JC_LT, JC_LE, JC_GT, JC_GE };

typedef struct {
    uint8_t  kind;
    uint8_t  has_start, has_end;
    JSAtom   name;                  /* JP_NAME */
    int64_t  start, end, step;      /* JP_INDEX uses start */
    uint32_t filter;                /* JP_FILTER: node index */
} jp_sel_t;

typedef struct {
    uint8_t  descendant;
    uint32_t first, n;              /* range into sels */
} jp_seg_t;

typedef struct {
    uint8_t  kind, op, root;        /* root: JF_PATH is $-relative, not @ */
    uint32_t a, b;                  /* children, or JF_PATH range into psels */
    double   num;
    char    *str;                   /* JF_STR, owned */
    uint32_t slen;
} jp_node_t;

/* Pointers first, then the counts: interleaved they cost 16 bytes of padding
   and a second cache line for a struct that is read on every query. */
typedef struct {
    JSRuntime *rt;
    jp_seg_t  *segs;
    jp_sel_t  *sels;
    jp_sel_t  *psels;               /* singular-query steps inside filters */
    jp_node_t *nodes;
    uint32_t   nseg, nsel, npsel, nnode;
} jp_t;

/* ------------------------------------------------------------ compilation */

static void jp_free(jp_t *p)
{
    uint32_t i;
    if (!p)
        return;
    for (i = 0; i < p->nsel; i++)
        if (p->sels[i].kind == JP_NAME)
            JS_FreeAtomRT(p->rt, p->sels[i].name);
    for (i = 0; i < p->npsel; i++)
        if (p->psels[i].kind == JP_NAME)
            JS_FreeAtomRT(p->rt, p->psels[i].name);
    for (i = 0; i < p->nnode; i++)
        free(p->nodes[i].str);
    free(p->segs); free(p->sels); free(p->psels); free(p->nodes);
    free(p);
}

static void jp_free_v(void *q) { jp_free((jp_t *)q); }

typedef struct {
    JSContext *ctx;
    const char *s;
    size_t n, i;
    jp_t *p;
    int depth, err;
    char msg[96];
} jp_parse_t;

/* First error wins: a later cascade describes the recovery, not the mistake. */
static int jp_err(jp_parse_t *q, const char *what)
{
    if (!q->err) {
        q->err = 1;
        snprintf(q->msg, sizeof q->msg, "%s at offset %u", what, (unsigned)q->i);
    }
    return -1;
}

static void jp_ws(jp_parse_t *q)
{
    while (q->i < q->n) {
        char c = q->s[q->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') q->i++;
        else break;
    }
}

static int jp_at(jp_parse_t *q, char c) { return q->i < q->n && q->s[q->i] == c; }

/* A member-name shorthand character (RFC 9535 name-first / name-char). */
static int jp_name_first(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
static int jp_name_char(unsigned char c)
{
    return jp_name_first(c) || (c >= '0' && c <= '9');
}

/* A quoted selector string. Both quote styles, with the JSON escapes. */
static int jp_qstring(jp_parse_t *q, char quote, dyn_buf_t *b)
{
    q->i++;
    while (q->i < q->n) {
        unsigned char c = (unsigned char)q->s[q->i];
        if (c == (unsigned char)quote) {
            q->i++;
            return b->oom ? jp_err(q, "out of memory") : 0;
        }
        if (c == '\\') {
            int cp;
            q->i++;
            if (q->i >= q->n)
                return jp_err(q, "unterminated escape");
            switch (q->s[q->i]) {
            case 'b': dyn_buf_put(b, '\b'); q->i++; break;
            case 'f': dyn_buf_put(b, '\f'); q->i++; break;
            case 'n': dyn_buf_put(b, '\n'); q->i++; break;
            case 'r': dyn_buf_put(b, '\r'); q->i++; break;
            case 't': dyn_buf_put(b, '\t'); q->i++; break;
            case '/': dyn_buf_put(b, '/');  q->i++; break;
            case '\\': dyn_buf_put(b, '\\'); q->i++; break;
            case '\'': dyn_buf_put(b, '\''); q->i++; break;
            case '"': dyn_buf_put(b, '"');  q->i++; break;
            case 'u':
                q->i++;
                cp = 0;
                if (q->n - q->i < 4)
                    return jp_err(q, "short \\u escape");
                {
                    int k, h;
                    for (k = 0; k < 4; k++) {
                        h = dyn_j5_hex((unsigned char)q->s[q->i + k]);
                        if (h < 0)
                            return jp_err(q, "bad \\u escape");
                        cp = cp * 16 + h;
                    }
                }
                q->i += 4;
                /* A surrogate pair is two escapes; combine so the atom is the
                   code point the query names, not two lone halves. */
                if (cp >= 0xD800 && cp <= 0xDBFF && q->n - q->i >= 6
                    && q->s[q->i] == '\\' && q->s[q->i + 1] == 'u') {
                    int k, h, lo = 0;
                    for (k = 0; k < 4; k++) {
                        h = dyn_j5_hex((unsigned char)q->s[q->i + 2 + k]);
                        if (h < 0) { lo = -1; break; }
                        lo = lo * 16 + h;
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        q->i += 6;
                    }
                }
                dyn_buf_putc(b, (uint32_t)cp);
                break;
            default:
                return jp_err(q, "bad escape");
            }
            continue;
        }
        if (c < 0x20)
            return jp_err(q, "control character in string");
        dyn_buf_put(b, c);
        q->i++;
    }
    return jp_err(q, "unterminated string");
}

static int jp_int(jp_parse_t *q, int64_t *out)
{
    int neg = 0;
    int64_t v = 0;
    size_t start = q->i;
    if (jp_at(q, '-')) { neg = 1; q->i++; }
    if (q->i >= q->n || q->s[q->i] < '0' || q->s[q->i] > '9')
        return jp_err(q, "expected an integer");
    while (q->i < q->n && q->s[q->i] >= '0' && q->s[q->i] <= '9') {
        if (v > (INT64_MAX - 9) / 10) {
            q->i = start;
            return jp_err(q, "integer out of range");
        }
        v = v * 10 + (q->s[q->i] - '0');
        q->i++;
    }
    *out = neg ? -v : v;
    return 0;
}

static int jp_filter_or(jp_parse_t *q, uint32_t *out);

/* A singular query: @ or $ followed by name and index steps only. RFC 9535
   restricts comparison operands to these, and so does the existence test here. */
static int jp_singular(jp_parse_t *q, uint32_t *out)
{
    jp_node_t *nd;
    uint32_t first = q->p->npsel;
    int root = jp_at(q, '$');

    if (!root && !jp_at(q, '@'))
        return jp_err(q, "expected @ or $");
    q->i++;
    for (;;) {
        jp_sel_t *s = &q->p->psels[q->p->npsel];
        if (jp_at(q, '.')) {
            size_t st;
            q->i++;
            st = q->i;
            if (q->i >= q->n || !jp_name_first((unsigned char)q->s[q->i]))
                return jp_err(q, "expected a member name");
            while (q->i < q->n && jp_name_char((unsigned char)q->s[q->i]))
                q->i++;
            s->kind = JP_NAME;
            s->name = JS_NewAtomLen(q->ctx, q->s + st, q->i - st);
            if (s->name == JS_ATOM_NULL)
                return jp_err(q, "out of memory");
            q->p->npsel++;
            continue;
        }
        if (jp_at(q, '[')) {
            q->i++;
            jp_ws(q);
            if (jp_at(q, '\'') || jp_at(q, '"')) {
                dyn_buf_t b;
                dyn_buf_init(&b);
                if (jp_qstring(q, q->s[q->i], &b) < 0) { dyn_buf_free(&b); return -1; }
                s->kind = JP_NAME;
                s->name = JS_NewAtomLen(q->ctx, (const char *)b.p, b.n);
                dyn_buf_free(&b);
                if (s->name == JS_ATOM_NULL)
                    return jp_err(q, "out of memory");
            } else {
                if (jp_int(q, &s->start) < 0)
                    return -1;
                s->kind = JP_INDEX;
            }
            q->p->npsel++;
            jp_ws(q);
            if (!jp_at(q, ']'))
                return jp_err(q, "expected ] (a filter operand is a single path)");
            q->i++;
            continue;
        }
        break;
    }
    nd = &q->p->nodes[q->p->nnode];
    memset(nd, 0, sizeof *nd);
    nd->kind = JF_PATH;
    nd->root = (uint8_t)root;
    nd->a = first;
    nd->b = q->p->npsel - first;
    *out = q->p->nnode++;
    return 0;
}

/* A literal, or -1 if the next token is not one. */
static int jp_literal(jp_parse_t *q, uint32_t *out)
{
    jp_node_t *nd = &q->p->nodes[q->p->nnode];
    memset(nd, 0, sizeof *nd);
    if (jp_at(q, '\'') || jp_at(q, '"')) {
        dyn_buf_t b;
        dyn_buf_init(&b);
        if (jp_qstring(q, q->s[q->i], &b) < 0) { dyn_buf_free(&b); return -1; }
        nd->kind = JF_STR;
        nd->str = (char *)malloc(b.n + 1);
        if (!nd->str) { dyn_buf_free(&b); return jp_err(q, "out of memory"); }
        if (b.n) memcpy(nd->str, b.p, b.n);
        nd->str[b.n] = 0;
        nd->slen = (uint32_t)b.n;
        dyn_buf_free(&b);
    } else if (q->n - q->i >= 4 && memcmp(q->s + q->i, "true", 4) == 0) {
        nd->kind = JF_BOOL; nd->num = 1; q->i += 4;
    } else if (q->n - q->i >= 5 && memcmp(q->s + q->i, "false", 5) == 0) {
        nd->kind = JF_BOOL; nd->num = 0; q->i += 5;
    } else if (q->n - q->i >= 4 && memcmp(q->s + q->i, "null", 4) == 0) {
        nd->kind = JF_NULL; q->i += 4;
    } else if (jp_at(q, '-') || (q->i < q->n && q->s[q->i] >= '0' && q->s[q->i] <= '9')) {
        /* ToNumber over the literal's text: correctly rounded and locale-
           independent, where strtod would read LC_NUMERIC for the radix. */
        size_t st = q->i;
        JSValue sv;
        double d;
        if (jp_at(q, '-')) q->i++;
        while (q->i < q->n && ((q->s[q->i] >= '0' && q->s[q->i] <= '9')
               || q->s[q->i] == '.' || q->s[q->i] == 'e' || q->s[q->i] == 'E'
               || ((q->s[q->i] == '+' || q->s[q->i] == '-')
                   && (q->s[q->i - 1] == 'e' || q->s[q->i - 1] == 'E'))))
            q->i++;
        if (q->i == st)
            return jp_err(q, "expected a number");
        sv = JS_NewStringLen(q->ctx, q->s + st, q->i - st);
        if (JS_IsException(sv))
            return jp_err(q, "out of memory");
        if (JS_ToFloat64(q->ctx, &d, sv)) {
            JS_FreeValue(q->ctx, sv);
            JS_GetException(q->ctx);
            return jp_err(q, "bad number");
        }
        JS_FreeValue(q->ctx, sv);
        if (d != d)
            return jp_err(q, "bad number");
        nd->kind = JF_NUM;
        nd->num = d;
    } else {
        return 1;                            /* not a literal */
    }
    *out = q->p->nnode++;
    return 0;
}

static int jp_cmp_op(jp_parse_t *q, uint8_t *op)
{
    if (q->n - q->i >= 2) {
        if (memcmp(q->s + q->i, "==", 2) == 0) { *op = JC_EQ; q->i += 2; return 0; }
        if (memcmp(q->s + q->i, "!=", 2) == 0) { *op = JC_NE; q->i += 2; return 0; }
        if (memcmp(q->s + q->i, "<=", 2) == 0) { *op = JC_LE; q->i += 2; return 0; }
        if (memcmp(q->s + q->i, ">=", 2) == 0) { *op = JC_GE; q->i += 2; return 0; }
    }
    if (jp_at(q, '<')) { *op = JC_LT; q->i++; return 0; }
    if (jp_at(q, '>')) { *op = JC_GT; q->i++; return 0; }
    return 1;
}

static int jp_filter_basic(jp_parse_t *q, uint32_t *out)
{
    uint32_t left, right, self;
    uint8_t op;
    int r;

    if (q->depth++ >= JP_MAX_DEPTH)
        return jp_err(q, "filter nested too deeply");
    jp_ws(q);
    if (jp_at(q, '!')) {
        q->i++;
        jp_ws(q);
        if (jp_filter_basic(q, &left) < 0)
            return -1;
        self = q->p->nnode++;
        memset(&q->p->nodes[self], 0, sizeof(jp_node_t));
        q->p->nodes[self].kind = JF_NOT;
        q->p->nodes[self].a = left;
        *out = self;
        q->depth--;
        return 0;
    }
    if (jp_at(q, '(')) {
        q->i++;
        if (jp_filter_or(q, out) < 0)
            return -1;
        jp_ws(q);
        if (!jp_at(q, ')'))
            return jp_err(q, "expected )");
        q->i++;
        q->depth--;
        return 0;
    }
    r = jp_literal(q, &left);
    if (r < 0)
        return -1;
    if (r > 0 && jp_singular(q, &left) < 0)
        return -1;
    jp_ws(q);
    if (jp_cmp_op(q, &op) != 0) {
        /* No operator: an existence test, which a literal cannot be. */
        if (q->p->nodes[left].kind != JF_PATH)
            return jp_err(q, "a literal is not a test expression");
        *out = left;
        q->depth--;
        return 0;
    }
    jp_ws(q);
    r = jp_literal(q, &right);
    if (r < 0)
        return -1;
    if (r > 0 && jp_singular(q, &right) < 0)
        return -1;
    self = q->p->nnode++;
    memset(&q->p->nodes[self], 0, sizeof(jp_node_t));
    q->p->nodes[self].kind = JF_CMP;
    q->p->nodes[self].op = op;
    q->p->nodes[self].a = left;
    q->p->nodes[self].b = right;
    *out = self;
    q->depth--;
    return 0;
}

static int jp_filter_and(jp_parse_t *q, uint32_t *out)
{
    uint32_t left, right, self;
    if (jp_filter_basic(q, &left) < 0)
        return -1;
    for (;;) {
        jp_ws(q);
        if (q->n - q->i < 2 || memcmp(q->s + q->i, "&&", 2) != 0)
            break;
        q->i += 2;
        if (jp_filter_basic(q, &right) < 0)
            return -1;
        self = q->p->nnode++;
        memset(&q->p->nodes[self], 0, sizeof(jp_node_t));
        q->p->nodes[self].kind = JF_AND;
        q->p->nodes[self].a = left;
        q->p->nodes[self].b = right;
        left = self;
    }
    *out = left;
    return 0;
}

static int jp_filter_or(jp_parse_t *q, uint32_t *out)
{
    uint32_t left, right, self;
    if (jp_filter_and(q, &left) < 0)
        return -1;
    for (;;) {
        jp_ws(q);
        if (q->n - q->i < 2 || memcmp(q->s + q->i, "||", 2) != 0)
            break;
        q->i += 2;
        if (jp_filter_and(q, &right) < 0)
            return -1;
        self = q->p->nnode++;
        memset(&q->p->nodes[self], 0, sizeof(jp_node_t));
        q->p->nodes[self].kind = JF_OR;
        q->p->nodes[self].a = left;
        q->p->nodes[self].b = right;
        left = self;
    }
    *out = left;
    return 0;
}

/* One selector inside [ ... ]. */
static int jp_selector(jp_parse_t *q)
{
    jp_sel_t *s = &q->p->sels[q->p->nsel];
    memset(s, 0, sizeof *s);
    jp_ws(q);
    if (jp_at(q, '*')) {
        q->i++;
        s->kind = JP_WILD;
        q->p->nsel++;
        return 0;
    }
    if (jp_at(q, '?')) {
        q->i++;
        q->depth = 0;
        if (jp_filter_or(q, &s->filter) < 0)
            return -1;
        s->kind = JP_FILTER;
        q->p->nsel++;
        return 0;
    }
    if (jp_at(q, '\'') || jp_at(q, '"')) {
        dyn_buf_t b;
        dyn_buf_init(&b);
        if (jp_qstring(q, q->s[q->i], &b) < 0) { dyn_buf_free(&b); return -1; }
        s->kind = JP_NAME;
        s->name = JS_NewAtomLen(q->ctx, (const char *)b.p, b.n);
        dyn_buf_free(&b);
        if (s->name == JS_ATOM_NULL)
            return jp_err(q, "out of memory");
        q->p->nsel++;
        return 0;
    }
    /* An index or a slice; a leading ':' is a slice with no start. */
    s->step = 1;
    if (!jp_at(q, ':')) {
        if (jp_int(q, &s->start) < 0)
            return -1;
        s->has_start = 1;
    }
    jp_ws(q);
    if (!jp_at(q, ':')) {
        if (!s->has_start)
            return jp_err(q, "expected a selector");
        s->kind = JP_INDEX;
        q->p->nsel++;
        return 0;
    }
    q->i++;                                   /* the first ':' */
    jp_ws(q);
    if (q->i < q->n && (q->s[q->i] == '-' || (q->s[q->i] >= '0' && q->s[q->i] <= '9'))) {
        if (jp_int(q, &s->end) < 0)
            return -1;
        s->has_end = 1;
        jp_ws(q);
    }
    if (jp_at(q, ':')) {
        q->i++;
        jp_ws(q);
        if (q->i < q->n && (q->s[q->i] == '-' || (q->s[q->i] >= '0' && q->s[q->i] <= '9'))) {
            if (jp_int(q, &s->step) < 0)
                return -1;
        }
    }
    s->kind = JP_SLICE;
    q->p->nsel++;
    return 0;
}

static int jp_segment(jp_parse_t *q, int descendant)
{
    jp_seg_t *g = &q->p->segs[q->p->nseg];
    g->descendant = (uint8_t)descendant;
    g->first = q->p->nsel;
    g->n = 0;
    if (jp_at(q, '[')) {
        q->i++;
        for (;;) {
            if (jp_selector(q) < 0)
                return -1;
            g->n++;
            jp_ws(q);
            if (jp_at(q, ',')) { q->i++; continue; }
            break;
        }
        if (!jp_at(q, ']'))
            return jp_err(q, "expected ]");
        q->i++;
    } else if (jp_at(q, '*')) {
        jp_sel_t *s = &q->p->sels[q->p->nsel];
        q->i++;
        memset(s, 0, sizeof *s);
        s->kind = JP_WILD;
        q->p->nsel++;
        g->n = 1;
    } else if (q->i < q->n && jp_name_first((unsigned char)q->s[q->i])) {
        jp_sel_t *s = &q->p->sels[q->p->nsel];
        size_t st = q->i;
        while (q->i < q->n && jp_name_char((unsigned char)q->s[q->i]))
            q->i++;
        memset(s, 0, sizeof *s);
        s->kind = JP_NAME;
        s->name = JS_NewAtomLen(q->ctx, q->s + st, q->i - st);
        if (s->name == JS_ATOM_NULL)
            return jp_err(q, "out of memory");
        q->p->nsel++;
        g->n = 1;
    } else {
        return jp_err(q, "expected a selector");
    }
    q->p->nseg++;
    return 0;
}

static jp_t *jp_compile(JSContext *ctx, const char *s, size_t n, char *emsg,
                        size_t emsgsz)
{
    jp_parse_t q;
    uint32_t cap = (uint32_t)n + 2;
    jp_t *p = (jp_t *)calloc(1, sizeof *p);

    if (!p) {
        snprintf(emsg, emsgsz, "out of memory");
        return NULL;
    }
    p->rt = JS_GetRuntime(ctx);
    /* Every table entry costs at least one character of the expression, so one
       sizing from its length needs no growth code at all. */
    p->segs  = (jp_seg_t  *)calloc(cap, sizeof *p->segs);
    p->sels  = (jp_sel_t  *)calloc(cap, sizeof *p->sels);
    p->psels = (jp_sel_t  *)calloc(cap, sizeof *p->psels);
    p->nodes = (jp_node_t *)calloc(cap, sizeof *p->nodes);
    if (!p->segs || !p->sels || !p->psels || !p->nodes) {
        jp_free(p);
        snprintf(emsg, emsgsz, "out of memory");
        return NULL;
    }
    memset(&q, 0, sizeof q);
    q.ctx = ctx; q.s = s; q.n = n; q.p = p;
    jp_ws(&q);
    if (!jp_at(&q, '$')) {
        snprintf(emsg, emsgsz, "a query starts with $");
        jp_free(p);
        return NULL;
    }
    q.i++;
    for (;;) {
        jp_ws(&q);
        if (q.i >= q.n)
            break;
        if (q.n - q.i >= 2 && q.s[q.i] == '.' && q.s[q.i + 1] == '.') {
            q.i += 2;
            if (jp_segment(&q, 1) < 0)
                break;
        } else if (q.s[q.i] == '.') {
            q.i++;
            if (jp_segment(&q, 0) < 0)
                break;
        } else if (q.s[q.i] == '[') {
            if (jp_segment(&q, 0) < 0)
                break;
        } else {
            jp_err(&q, "expected . or [");
            break;
        }
    }
    if (q.err) {
        snprintf(emsg, emsgsz, "%s", q.msg);
        jp_free(p);
        return NULL;
    }
    return p;
}

/* ------------------------------------------------------------- evaluation */

typedef struct {
    JSValue  v;
    int32_t  parent;
    JSAtom   name;                  /* JS_ATOM_NULL => an array element */
    int64_t  index;
} jp_nv_t;

typedef struct {
    JSContext *ctx;
    jp_t      *p;
    JSValue    root;            /* borrowed for the run: `$` inside a filter */
    jp_nv_t   *nv;  uint32_t nn, ncap;
    uint32_t  *cur; uint32_t ncur, ccur;
    uint32_t  *nxt; uint32_t nnxt, cnxt;
    /* Two scratch lists, NOT one: a descendant segment holds `tmp` as its input
       list while a filter selector inside it is appending its own children. */
    uint32_t  *tmp; uint32_t ntmp, ctmp;
    uint32_t  *kid; uint32_t nkid, ckid;
    JSAtom     a_length;
    int        err;                 /* 1 exception, 2 node cap, 3 out of memory */
} jp_eval_t;

static int jp_push_idx(jp_eval_t *e, uint32_t **a, uint32_t *n, uint32_t *cap,
                       uint32_t v)
{
    if (*n == *cap) {
        size_t nc = dyn_grow_cap(*cap, *n + 1, 32);
        uint32_t *np = (uint32_t *)realloc(*a, nc * sizeof(uint32_t));
        if (!np) { e->err = 3; return -1; }
        *a = np; *cap = (uint32_t)nc;
    }
    (*a)[(*n)++] = v;
    return 0;
}

/* Take ownership of `v`; returns the node index or -1 having set e->err. */
static int32_t jp_push_node(jp_eval_t *e, JSValue v, int32_t parent, JSAtom name,
                            int64_t index)
{
    jp_nv_t *slot;
    if (e->nn >= JP_MAX_NODES) {
        JS_FreeValue(e->ctx, v);
        e->err = 2;
        return -1;
    }
    if (e->nn == e->ncap) {
        size_t nc = dyn_grow_cap(e->ncap, e->nn + 1, 32);
        jp_nv_t *np = (jp_nv_t *)realloc(e->nv, nc * sizeof(jp_nv_t));
        if (!np) { JS_FreeValue(e->ctx, v); e->err = 3; return -1; }
        e->nv = np; e->ncap = (uint32_t)nc;
    }
    slot = &e->nv[e->nn];
    slot->v = v;
    slot->parent = parent;
    slot->name = name == JS_ATOM_NULL ? JS_ATOM_NULL : JS_DupAtom(e->ctx, name);
    slot->index = index;
    return (int32_t)e->nn++;
}

/* An own DATA property, or 0 if absent or an accessor. A getter is skipped,
   never called: this is a data query, and running user code is not part of it. */
static int jp_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValue *out)
{
    JSPropertyDescriptor d;
    int r;
    if (!JS_IsObject(obj))
        return 0;
    r = JS_GetOwnProperty(ctx, &d, obj, prop);
    if (r <= 0)
        return r;
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    if (d.flags & JS_PROP_GETSET) {
        JS_FreeValue(ctx, d.value);
        return 0;
    }
    *out = d.value;
    return 1;
}

/* Array length, or -1 for anything that is not an array. */
static int64_t jp_len(jp_eval_t *e, JSValueConst v)
{
    JSValue lv;
    int64_t n = -1;
    if (JS_IsArray(e->ctx, v) != 1)
        return -1;
    if (jp_own(e->ctx, v, e->a_length, &lv) != 1)
        return -1;
    if (JS_ToInt64(e->ctx, &n, lv) < 0) {
        JS_GetException(e->ctx);
        n = -1;
    }
    JS_FreeValue(e->ctx, lv);
    return n;
}

/* Every child of a node, in document order, appended to (a, n, cap). */
static int jp_children(jp_eval_t *e, uint32_t node, uint32_t **a, uint32_t *n,
                       uint32_t *cap)
{
    JSValueConst v = e->nv[node].v;
    int64_t len = jp_len(e, v), i;

    if (len >= 0) {
        for (i = 0; i < len; i++) {
            JSAtom k = JS_NewAtomUInt32(e->ctx, (uint32_t)i);
            JSValue cv;
            int r;
            if (k == JS_ATOM_NULL) { e->err = 3; return -1; }
            r = jp_own(e->ctx, v, k, &cv);
            JS_FreeAtom(e->ctx, k);
            if (r < 0) { e->err = 1; return -1; }
            if (r == 0) continue;
            {
                int32_t idx = jp_push_node(e, cv, (int32_t)node, JS_ATOM_NULL, i);
                if (idx < 0 || jp_push_idx(e, a, n, cap, (uint32_t)idx) < 0)
                    return -1;
            }
        }
        return 0;
    }
    if (!JS_IsObject(v))
        return 0;
    {
        JSPropertyEnum *tab = NULL;
        uint32_t len32 = 0, k;
        if (JS_GetOwnPropertyNames(e->ctx, &tab, &len32, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            e->err = 1;
            return -1;
        }
        for (k = 0; k < len32; k++) {
            JSValue cv;
            int r = jp_own(e->ctx, v, tab[k].atom, &cv);
            if (r < 0) { e->err = 1; break; }
            if (r == 0) continue;
            {
                int32_t idx = jp_push_node(e, cv, (int32_t)node, tab[k].atom, -1);
                if (idx < 0 || jp_push_idx(e, a, n, cap, (uint32_t)idx) < 0)
                    break;
            }
        }
        JS_FreePropertyEnum(e->ctx, tab, len32);
        return e->err ? -1 : 0;
    }
}

/* ------------------------------------------------------------ filter eval */

/* A resolved operand. `cstr` is owned and must be released. */
typedef struct {
    JSValue     owned;
    double      num;
    const char *s;
    size_t      slen;
    const char *cstr;
    int kind;                       /* 0 nothing 1 num 2 str 3 bool 4 null 5 other */
    int has_owned;
} jp_val_t;

static void jp_val_free(JSContext *ctx, jp_val_t *o)
{
    if (o->cstr) JS_FreeCString(ctx, o->cstr);
    if (o->has_owned) JS_FreeValue(ctx, o->owned);
    o->cstr = NULL;
    o->has_owned = 0;
}

static void jp_classify(JSContext *ctx, jp_val_t *o, JSValue v)
{
    o->owned = v;
    o->has_owned = 1;
    if (JS_IsNumber(v)) {
        o->kind = 1;
        if (JS_ToFloat64(ctx, &o->num, v) < 0) { JS_GetException(ctx); o->kind = 0; }
    } else if (JS_IsString(v)) {
        o->cstr = JS_ToCStringLen(ctx, &o->slen, v);
        if (!o->cstr) { JS_GetException(ctx); o->kind = 0; return; }
        o->s = o->cstr;
        o->kind = 2;
    } else if (JS_IsBool(v)) {
        o->kind = 3;
        o->num = JS_ToBool(ctx, v) ? 1 : 0;
    } else if (JS_IsNull(v)) {
        o->kind = 4;
    } else if (JS_IsUndefined(v)) {
        o->kind = 0;                /* absent is Nothing, not a value */
    } else {
        o->kind = 5;
    }
}

/* Walk a singular query from @ (cur) or $ (root). */
static int jp_resolve(jp_eval_t *e, const jp_node_t *nd, JSValueConst cur,
                      jp_val_t *out)
{
    JSValue v = JS_DupValue(e->ctx, nd->root ? e->root : cur);
    uint32_t k;
    memset(out, 0, sizeof *out);
    for (k = 0; k < nd->b; k++) {
        const jp_sel_t *s = &e->p->psels[nd->a + k];
        JSValue nx;
        int r;
        if (s->kind == JP_NAME) {
            r = jp_own(e->ctx, v, s->name, &nx);
        } else {
            int64_t len = jp_len(e, v), i = s->start;
            r = 0;
            if (i < 0) i += len;
            if (len >= 0 && i >= 0 && i < len) {
                JSAtom a = JS_NewAtomUInt32(e->ctx, (uint32_t)i);
                if (a == JS_ATOM_NULL) {
                    r = -2;                 /* freed once, below, like every path */
                } else {
                    r = jp_own(e->ctx, v, a, &nx);
                    JS_FreeAtom(e->ctx, a);
                }
            }
        }
        JS_FreeValue(e->ctx, v);
        if (r == -2) { e->err = 3; return -1; }
        if (r < 0) { e->err = 1; return -1; }
        if (r == 0) { out->kind = 0; return 0; }
        v = nx;
    }
    jp_classify(e->ctx, out, v);
    return 0;
}

static int jp_operand(jp_eval_t *e, uint32_t idx, JSValueConst cur, jp_val_t *out)
{
    const jp_node_t *nd = &e->p->nodes[idx];
    memset(out, 0, sizeof *out);
    switch (nd->kind) {
    case JF_PATH: return jp_resolve(e, nd, cur, out);
    case JF_NUM:  out->kind = 1; out->num = nd->num; return 0;
    case JF_STR:  out->kind = 2; out->s = nd->str; out->slen = nd->slen; return 0;
    case JF_BOOL: out->kind = 3; out->num = nd->num; return 0;
    default:      out->kind = 4; return 0;      /* JF_NULL */
    }
}

/* RFC 9535 comparison. Ordering is defined for numbers and strings only;
   everything else compares false, and Nothing equals only Nothing. UTF-8 byte
   order IS code point order, so memcmp is the spec's string ordering. */
static int jp_compare(uint8_t op, const jp_val_t *a, const jp_val_t *b)
{
    int eq = 0, lt = 0, ord = 0;

    if (a->kind == b->kind) {
        switch (a->kind) {
        case 0: case 4: eq = 1; break;
        case 1: eq = a->num == b->num; lt = a->num < b->num; ord = 1; break;
        case 3: eq = a->num == b->num; break;
        case 2: {
            size_t n = a->slen < b->slen ? a->slen : b->slen;
            int c = n ? memcmp(a->s, b->s, n) : 0;
            if (c == 0) c = a->slen < b->slen ? -1 : (a->slen > b->slen ? 1 : 0);
            eq = c == 0; lt = c < 0; ord = 1;
            break;
        }
        default: break;             /* object/array: see the subset note */
        }
    }
    switch (op) {
    case JC_EQ: return eq;
    case JC_NE: return !eq;
    case JC_LT: return ord && lt;
    case JC_LE: return ord && (lt || eq);
    case JC_GT: return ord && !lt && !eq;
    default:    return ord && !lt;  /* JC_GE */
    }
}

static int jp_filter(jp_eval_t *e, uint32_t idx, JSValueConst cur)
{
    const jp_node_t *nd = &e->p->nodes[idx];
    int r;
    switch (nd->kind) {
    case JF_OR:
        r = jp_filter(e, nd->a, cur);
        if (r != 0) return r;
        return jp_filter(e, nd->b, cur);
    case JF_AND:
        r = jp_filter(e, nd->a, cur);
        if (r != 1) return r;
        return jp_filter(e, nd->b, cur);
    case JF_NOT:
        r = jp_filter(e, nd->a, cur);
        return r < 0 ? r : !r;
    case JF_CMP: {
        jp_val_t a, b;
        int v;
        if (jp_operand(e, nd->a, cur, &a) < 0)
            return -1;
        if (jp_operand(e, nd->b, cur, &b) < 0) { jp_val_free(e->ctx, &a); return -1; }
        v = jp_compare(nd->op, &a, &b);
        jp_val_free(e->ctx, &a);
        jp_val_free(e->ctx, &b);
        return v;
    }
    case JF_PATH: {
        jp_val_t a;
        int v;
        if (jp_operand(e, idx, cur, &a) < 0)
            return -1;
        v = a.kind != 0;            /* existence: the query selected a node */
        jp_val_free(e->ctx, &a);
        return v;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------- selectors */

/* RFC 9535 slice bounds, including the negative-step case. */
static void jp_bounds(const jp_sel_t *s, int64_t len, int64_t *lower, int64_t *upper)
{
    int64_t st, en;
    if (s->step >= 0) {
        st = s->has_start ? s->start : 0;
        en = s->has_end ? s->end : len;
    } else {
        st = s->has_start ? s->start : len - 1;
        en = s->has_end ? s->end : -len - 1;
    }
    if (st < 0) st += len;
    if (en < 0) en += len;
    if (s->step >= 0) {
        *lower = st < 0 ? 0 : (st > len ? len : st);
        *upper = en < 0 ? 0 : (en > len ? len : en);
    } else {
        *upper = st < -1 ? -1 : (st > len - 1 ? len - 1 : st);
        *lower = en < -1 ? -1 : (en > len - 1 ? len - 1 : en);
    }
}

static int jp_take_index(jp_eval_t *e, uint32_t node, int64_t i)
{
    JSValueConst v = e->nv[node].v;
    int64_t len = jp_len(e, v);
    JSAtom a;
    JSValue cv;
    int r;
    int32_t idx;

    if (len < 0)
        return 0;
    if (i < 0) i += len;
    if (i < 0 || i >= len)
        return 0;
    a = JS_NewAtomUInt32(e->ctx, (uint32_t)i);
    if (a == JS_ATOM_NULL) { e->err = 3; return -1; }
    r = jp_own(e->ctx, v, a, &cv);
    JS_FreeAtom(e->ctx, a);
    if (r < 0) { e->err = 1; return -1; }
    if (r == 0) return 0;
    idx = jp_push_node(e, cv, (int32_t)node, JS_ATOM_NULL, i);
    if (idx < 0)
        return -1;
    return jp_push_idx(e, &e->nxt, &e->nnxt, &e->cnxt, (uint32_t)idx);
}

static int jp_apply_sel(jp_eval_t *e, uint32_t node, const jp_sel_t *s)
{
    switch (s->kind) {
    case JP_NAME: {
        JSValue cv;
        int r = jp_own(e->ctx, e->nv[node].v, s->name, &cv);
        int32_t idx;
        if (r < 0) { e->err = 1; return -1; }
        if (r == 0) return 0;
        idx = jp_push_node(e, cv, (int32_t)node, s->name, -1);
        if (idx < 0)
            return -1;
        return jp_push_idx(e, &e->nxt, &e->nnxt, &e->cnxt, (uint32_t)idx);
    }
    case JP_INDEX:
        return jp_take_index(e, node, s->start);
    case JP_WILD:
        return jp_children(e, node, &e->nxt, &e->nnxt, &e->cnxt);
    case JP_SLICE: {
        int64_t len = jp_len(e, e->nv[node].v), lo, up, i;
        if (len < 0 || s->step == 0)
            return 0;
        jp_bounds(s, len, &lo, &up);
        if (s->step > 0)
            for (i = lo; i < up; i += s->step) {
                if (jp_take_index(e, node, i) < 0) return -1;
            }
        else
            for (i = up; i > lo; i += s->step) {
                if (jp_take_index(e, node, i) < 0) return -1;
            }
        return 0;
    }
    default: {                      /* JP_FILTER */
        uint32_t k;
        e->nkid = 0;
        if (jp_children(e, node, &e->kid, &e->nkid, &e->ckid) < 0)
            return -1;
        for (k = 0; k < e->nkid; k++) {
            int r = jp_filter(e, s->filter, e->nv[e->kid[k]].v);
            if (r < 0)
                return -1;
            if (r && jp_push_idx(e, &e->nxt, &e->nnxt, &e->cnxt, e->kid[k]) < 0)
                return -1;
        }
        return 0;
    }
    }
}

/* Preorder self-and-descendants of every node in `cur`, into `tmp`. */
static int jp_descend(jp_eval_t *e)
{
    uint32_t k, stack_n = 0, stack_c = 0, *stack = NULL;
    int rc = 0;

    e->ntmp = 0;
    for (k = 0; k < e->ncur && rc == 0; k++) {
        stack_n = 0;
        if (jp_push_idx(e, &stack, &stack_n, &stack_c, e->cur[k]) < 0) { rc = -1; break; }
        while (stack_n) {
            uint32_t node = stack[--stack_n], j;
            uint32_t kids_n = 0, kids_c = 0, *kids = NULL;
            if (jp_push_idx(e, &e->tmp, &e->ntmp, &e->ctmp, node) < 0) { rc = -1; break; }
            if (jp_children(e, node, &kids, &kids_n, &kids_c) < 0) {
                free(kids);
                rc = -1;
                break;
            }
            for (j = kids_n; j > 0; j--)   /* reversed: preorder off a stack */
                if (jp_push_idx(e, &stack, &stack_n, &stack_c, kids[j - 1]) < 0) {
                    rc = -1;
                    break;
                }
            free(kids);
            if (rc)
                break;
        }
    }
    free(stack);
    return rc;
}

static void jp_eval_free(jp_eval_t *e)
{
    uint32_t i;
    for (i = 0; i < e->nn; i++) {
        JS_FreeValue(e->ctx, e->nv[i].v);
        JS_FreeAtom(e->ctx, e->nv[i].name);
    }
    free(e->nv); free(e->cur); free(e->nxt); free(e->tmp); free(e->kid);
    JS_FreeAtom(e->ctx, e->a_length);
}

/* Run the program; the result node indices are left in e->cur. */
static int jp_run(jp_eval_t *e, JSValueConst value)
{
    uint32_t g;
    int32_t r;

    e->root = value;                /* borrowed: what `$` means inside a filter */
    r = jp_push_node(e, JS_DupValue(e->ctx, value), -1, JS_ATOM_NULL, -1);
    if (r < 0 || jp_push_idx(e, &e->cur, &e->ncur, &e->ccur, (uint32_t)r) < 0)
        return -1;
    for (g = 0; g < e->p->nseg; g++) {
        const jp_seg_t *seg = &e->p->segs[g];
        uint32_t i, k;
        const uint32_t *from;
        uint32_t nfrom;

        if (seg->descendant) {
            if (jp_descend(e) < 0)
                return -1;
            from = e->tmp; nfrom = e->ntmp;
        } else {
            from = e->cur; nfrom = e->ncur;
        }
        e->nnxt = 0;
        for (i = 0; i < nfrom; i++)
            for (k = 0; k < seg->n; k++)
                if (jp_apply_sel(e, from[i], &e->p->sels[seg->first + k]) < 0)
                    return -1;
        e->ntmp = 0;
        {   /* the frontier becomes the new current; buffers are swapped, not
               reallocated, because a query has as many segments as it has. */
            uint32_t *sa = e->cur, sc = e->ccur;
            e->cur = e->nxt; e->ccur = e->cnxt; e->ncur = e->nnxt;
            e->nxt = sa; e->cnxt = sc; e->nnxt = 0;
        }
        if (e->ncur == 0)
            break;
    }
    return 0;
}

/* -------------------------------------------------------- normalized paths */

/* RFC 9535 s2.7: single-quoted, so `"` is NOT escaped and `'` is. */
static void jp_path_name(dyn_buf_t *b, const char *s, size_t n)
{
    size_t i;
    dyn_buf_put(b, '\'');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        switch (c) {
        case '\'': esc = "\\'";  break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default: break;
        }
        if (esc) {
            dyn_buf_write(b, (const uint8_t *)esc, 2);
        } else if (c < 0x20) {
            char t[8];
            int k = snprintf(t, sizeof t, "\\u%04x", c);
            dyn_buf_write(b, (const uint8_t *)t, (size_t)k);
        } else {
            dyn_buf_put(b, c);
        }
    }
    dyn_buf_put(b, '\'');
}

/* Walk the parent chain of one result node and emit `$['a'][0]`. */
static JSValue dyn_jp_path_of(JSContext *ctx, jp_eval_t *e, uint32_t node)
{
    dyn_buf_t b;
    uint32_t depth = 0, i, *chain;
    int32_t cur;
    JSValue out;

    for (cur = (int32_t)node; cur >= 0; cur = e->nv[cur].parent)
        depth++;
    chain = (uint32_t *)malloc((size_t)depth * sizeof *chain);
    if (!chain)
        return JS_ThrowOutOfMemory(ctx);
    i = depth;
    for (cur = (int32_t)node; cur >= 0; cur = e->nv[cur].parent)
        chain[--i] = (uint32_t)cur;
    dyn_buf_init(&b);
    dyn_buf_put(&b, '$');
    for (i = 1; i < depth; i++) {            /* chain[0] is the root itself */
        const jp_nv_t *nd = &e->nv[chain[i]];
        dyn_buf_put(&b, '[');
        if (nd->name != JS_ATOM_NULL) {
            size_t n;
            const char *s = JS_AtomToCStringLen(ctx, &n, nd->name);
            if (!s) {
                free(chain);
                dyn_buf_free(&b);
                return JS_EXCEPTION;
            }
            jp_path_name(&b, s, n);
            JS_FreeCString(ctx, s);
        } else {
            char t[24];
            int k = snprintf(t, sizeof t, "%lld", (long long)nd->index);
            dyn_buf_write(&b, (const uint8_t *)t, (size_t)k);
        }
        dyn_buf_put(&b, ']');
    }
    free(chain);
    if (b.oom) {
        dyn_buf_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char *)b.p, b.n);
    dyn_buf_free(&b);
    return out;
}

/* ---------------------------------------------------------- the JS class */

static JSClassID dyn_jp_class_id;

static void dyn_jp_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    jp_free((jp_t *)JS_GetOpaque(val, dyn_jp_class_id));
}

static const JSClassDef dyn_jp_class = {
    "JSONPath", .finalizer = dyn_jp_finalizer,
};

static JSValue dyn_jp_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    char emsg[96];
    jp_t *p;

    (void)new_target;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new JSONPath(expression): expression must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > JP_MAX_EXPR) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new JSONPath: expression exceeds %u bytes",
                                  JP_MAX_EXPR);
    }
    p = jp_compile(ctx, s, n, emsg, sizeof emsg);
    JS_FreeCString(ctx, s);
    if (!p)
        return JS_ThrowSyntaxError(ctx, "new JSONPath: %s", emsg);
    return dyn_plain_wrap(ctx, dyn_jp_class_id, p, jp_free_v);
}

static int dyn_jp_start(JSContext *ctx, JSValueConst this_val, jp_eval_t *e)
{
    jp_t *p = (jp_t *)dyn_plain_get(ctx, this_val, dyn_jp_class_id);
    if (!p)
        return -1;
    memset(e, 0, sizeof *e);
    e->ctx = ctx;
    e->p = p;
    e->a_length = JS_NewAtom(ctx, "length");
    return e->a_length == JS_ATOM_NULL ? -1 : 0;
}

static JSValue dyn_jp_fail(JSContext *ctx, jp_eval_t *e)
{
    int err = e->err;
    jp_eval_free(e);
    if (err == 2)
        return JS_ThrowRangeError(ctx, "JSONPath: query visits more than %u nodes",
                                  JP_MAX_NODES);
    if (err == 3)
        return JS_ThrowOutOfMemory(ctx);
    return JS_EXCEPTION;
}

/* magic: 0 = all, 1 = first, 2 = paths */
static JSValue dyn_jp_query(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    jp_eval_t e;
    JSValue out;
    uint32_t i;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "JSONPath query: a value is required");
    if (dyn_jp_start(ctx, this_val, &e) < 0)
        return JS_EXCEPTION;
    if (jp_run(&e, argv[0]) < 0)
        return dyn_jp_fail(ctx, &e);
    if (magic == 1) {
        out = e.ncur ? JS_DupValue(ctx, e.nv[e.cur[0]].v) : JS_UNDEFINED;
        jp_eval_free(&e);
        return out;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        jp_eval_free(&e);
        return JS_EXCEPTION;
    }
    for (i = 0; i < e.ncur; i++) {
        JSValue item = magic == 0 ? JS_DupValue(ctx, e.nv[e.cur[i]].v)
                                  : dyn_jp_path_of(ctx, &e, e.cur[i]);
        if (JS_IsException(item)
            || JS_DefinePropertyValueUint32(ctx, out, i, item, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            jp_eval_free(&e);
            return JS_EXCEPTION;
        }
    }
    jp_eval_free(&e);
    return out;
}

static const JSCFunctionListEntry dyn_jp_proto[] = {
    JS_CFUNC_MAGIC_DEF("all", 1, dyn_jp_query, 0),
    JS_CFUNC_MAGIC_DEF("first", 1, dyn_jp_query, 1),
    JS_CFUNC_MAGIC_DEF("paths", 1, dyn_jp_query, 2),
};
