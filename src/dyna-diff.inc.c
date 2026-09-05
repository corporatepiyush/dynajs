/* Myers O(ND) diff with the linear-space refinement, over interned tokens.
   Included by dyna-matcher.c after dyna-approx.inc.c. The divide-and-conquer
   runs on an EXPLICIT STACK: recursion depth here is attacker-controlled. */

/* Token count per side. 16 MiB of single characters is 16M tokens and ~200 MB
   of token arrays; this refuses rather than allocating what the input asks. */
#define DYN_DIFF_MAX_TOKENS (4u * 1024u * 1024u)

/* Middle-snake WORK budget (number of diagonal/equality steps). Myers is
   O((N+M)*D) and D is not otherwise bounded, so two ~4M-token fully-disjoint
   sides run ~6e13 inner steps on the loop thread (minutes-to-hours, a remote
   CPU-exhaustion vector via user text). The shared prefix/suffix trim does not
   help adversarial fully-disjoint input. This caps the sum of the 2d·snake
   diagonal cost: a diff that would need more than this refuses with a clean
   RangeError before the quadratic work grows unbounded. The budget sits well
   above the work of the largest legitimate diff in the suite (a fully-changed
   2000-line file, a few tens of millions of charged steps) yet trips a hostile
   input in well under a second. */
#define DYN_DIFF_MAX_DIAGONAL_STEPS (1u << 28)

/* Returned by dyn_middle_snake when the work budget is exhausted, to be told
   apart from the (unreachable-for-well-formed-input) -1. */
#define DYN_DIFF_TOO_COSTLY (-2)

enum { DYN_TOK_LINES, DYN_TOK_WORDS, DYN_TOK_CHARS };

/* A tokenised side: byte ranges into the source plus an interned id each.
   Concatenating every range reproduces the source exactly, which is what makes
   the reassembly property a real oracle. */
typedef struct {
    uint32_t *off, *len, *id;
    uint32_t n;
} dyn_toks_t;

static void dyn_toks_free(dyn_toks_t *t)
{
    free(t->off); free(t->len); free(t->id);
    t->off = t->len = t->id = NULL;
    t->n = 0;
}

/* Overshoot costs ABSOLUTE bytes, so the factor decays: doubling is free while
   small and wastes megabytes at scale. */
static size_t dyn_grow(size_t cur, size_t need)
{
    size_t nc = cur ? cur : 64;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static int dyn_toks_push(dyn_toks_t *t, uint32_t *cap, uint32_t off, uint32_t len)
{
    if (t->n == *cap) {
        uint32_t nc = (uint32_t)dyn_grow(*cap, (size_t)*cap + 1);
        uint32_t *o, *l, *i;
        if (nc > DYN_DIFF_MAX_TOKENS)
            nc = DYN_DIFF_MAX_TOKENS;
        if (nc == *cap)
            return -1;
        /* Commit all three or none: a partial growth leaves the three arrays
           at different capacities, which is an invariant nothing re-checks. */
        o = (uint32_t *)realloc(t->off, nc * sizeof(uint32_t));
        if (o) t->off = o;
        l = (uint32_t *)realloc(t->len, nc * sizeof(uint32_t));
        if (l) t->len = l;
        i = (uint32_t *)realloc(t->id, nc * sizeof(uint32_t));
        if (i) t->id = i;
        if (!o || !l || !i)
            return -1;                   /* caller frees; capacity stays unchanged */
        *cap = nc;
    }
    t->off[t->n] = off;
    t->len[t->n] = len;
    t->n++;
    return 0;
}

static int dyn_is_ws_byte(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v';
}

/* Split `s` by `mode`. Lines keep their trailing newline; words alternate
   word/whitespace runs; chars are whole UTF-8 code points. */
static int dyn_tokenize(const uint8_t *s, uint32_t n, int mode, dyn_toks_t *t)
{
    uint32_t cap = 0, i = 0;
    t->off = t->len = t->id = NULL;
    t->n = 0;
    while (i < n) {
        uint32_t start = i;
        if (mode == DYN_TOK_LINES) {
            while (i < n && s[i] != '\n')
                i++;
            if (i < n)
                i++;                       /* the newline belongs to its line */
        } else if (mode == DYN_TOK_WORDS) {
            int ws = dyn_is_ws_byte(s[i]);
            while (i < n && dyn_is_ws_byte(s[i]) == ws)
                i++;
        } else {
            uint8_t c = s[i++];
            if (c >= 0xF0)      i += 3;
            else if (c >= 0xE0) i += 2;
            else if (c >= 0xC0) i += 1;
            if (i > n)
                i = n;                     /* truncated tail: one short token */
        }
        if (dyn_toks_push(t, &cap, start, i - start) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- interning */

/* Both sides share one table so equal text gets equal ids across sides. */
typedef struct {
    uint32_t *slot;          /* 1-based index into ent, 0 = empty */
    uint32_t *ent_off, *ent_len;
    const uint8_t **ent_src;
    uint32_t n_ent, mask;
} dyn_intern_t;

static void dyn_intern_free(dyn_intern_t *t)
{
    free(t->slot); free(t->ent_off); free(t->ent_len); free(t->ent_src);
    t->slot = NULL; t->ent_off = NULL; t->ent_len = NULL; t->ent_src = NULL;
}

/* xxh64, per design 10. It was FNV-1a byte-at-a-time, in the interner's inner
   loop -- every token of both sides -- with xxhash already in core/dyn-hash.c. */
static uint64_t dyn_tok_hash(const uint8_t *p, uint32_t n)
{
    return dyn_xxh64(p, n, 0);
}

static int dyn_intern_init(dyn_intern_t *t, uint32_t total)
{
    uint32_t cap = 16;
    while (cap < total * 2 && cap < (1u << 30))
        cap <<= 1;
    t->mask = cap - 1;
    t->n_ent = 0;
    t->slot = (uint32_t *)calloc(cap, sizeof(uint32_t));
    t->ent_off = (uint32_t *)malloc((total + 1) * sizeof(uint32_t));
    t->ent_len = (uint32_t *)malloc((total + 1) * sizeof(uint32_t));
    t->ent_src = (const uint8_t **)malloc((total + 1) * sizeof(const uint8_t *));
    return (t->slot && t->ent_off && t->ent_len && t->ent_src) ? 0 : -1;
}

static uint32_t dyn_intern(dyn_intern_t *t, const uint8_t *src, uint32_t off,
                           uint32_t len)
{
    uint64_t h = dyn_tok_hash(src + off, len);
    uint32_t i = (uint32_t)(h & t->mask);
    while (t->slot[i]) {
        uint32_t e = t->slot[i] - 1;
        if (t->ent_len[e] == len
            && memcmp(t->ent_src[e] + t->ent_off[e], src + off, len) == 0)
            return e;
        i = (i + 1) & t->mask;
    }
    t->ent_off[t->n_ent] = off;
    t->ent_len[t->n_ent] = len;
    t->ent_src[t->n_ent] = src;
    t->slot[i] = t->n_ent + 1;
    return t->n_ent++;
}

/* ------------------------------------------------------ the middle snake */

/* The work budget is carried on the workspace so a single cap covers every
   divisor (hence all three of DiffLines/DiffWords/DiffChars) and so the
   divide-and-conquer can run the SAME cap on every piece it recurses into. */
typedef struct {
    int32_t *fwd, *rev;      /* both offset by `off` so index -d..d is legal */
    int32_t off;
    uint64_t budget;         /* diagonal/equality steps still allowed */
    int32_t overflow;        /* 1 once the budget is spent: refuse, don't OOM */
} dyn_snake_ws_t;

/* Myers' middle snake over A[a0..a1) and B[b0..b1). Writes the snake's start
   into (*px,*py) and its end into (*pu,*pv). Returns the edit distance found,
   or DYN_DIFF_TOO_COSTLY once this run has charged more than `w->budget`
   diagonal steps (the D&C caller must then refuse the whole diff). The budget
   is DECREMENTED, not just compared, so the total across every region the
   divide-and-conquer recurses into is bounded -- a per-region comparison could
   be evaded by splitting a hostile input into many just-under-cap pieces. */
static int32_t dyn_middle_snake(const uint32_t *A, int32_t a0, int32_t a1,
                                const uint32_t *B, int32_t b0, int32_t b1,
                                dyn_snake_ws_t *w,
                                int32_t *px, int32_t *py,
                                int32_t *pu, int32_t *pv)
{
    int32_t N = a1 - a0, M = b1 - b0;
    int32_t delta = N - M, odd = delta & 1;
    int32_t dmax = (N + M + 1) / 2, d, k;
    int32_t *fwd = w->fwd + w->off, *rev = w->rev + w->off;

    fwd[1] = 0;
    rev[1] = 0;
    for (d = 0; d <= dmax; d++) {
        for (k = -d; k <= d; k += 2) {
            int32_t x, y, x0, y0;
            if (k == -d || (k != d && fwd[k - 1] < fwd[k + 1]))
                x = fwd[k + 1];
            else
                x = fwd[k - 1] + 1;
            y = x - k;
            x0 = x; y0 = y;
            while (x < N && y < M && A[a0 + x] == B[b0 + y]) { x++; y++; }
            fwd[k] = x;
            if (odd && (k - delta) >= -(d - 1) && (k - delta) <= (d - 1)
                && x + rev[delta - k] >= N) {
                *px = a0 + x0; *py = b0 + y0;
                *pu = a0 + x;  *pv = b0 + y;
                return 2 * d - 1;
            }
        }
        /* This d level visited 2d+1 diagonals in the forward direction. Charge
           them: this is where the un-capped O(D^2) blowup lives (for disjoint
           input the snake walks are length 0 but the diagonals still double). */
        if (w->budget < (uint64_t)(2 * d + 1)) {
            w->budget = 0;
            w->overflow = 1;
            return DYN_DIFF_TOO_COSTLY;
        }
        w->budget -= (uint64_t)(2 * d + 1);
        for (k = -d; k <= d; k += 2) {
            int32_t x, y, x0, y0;
            if (k == -d || (k != d && rev[k - 1] < rev[k + 1]))
                x = rev[k + 1];
            else
                x = rev[k - 1] + 1;
            y = x - k;
            x0 = x; y0 = y;
            while (x < N && y < M && A[a1 - x - 1] == B[b1 - y - 1]) { x++; y++; }
            rev[k] = x;
            if (!odd && (k - delta) >= -d && (k - delta) <= d
                && x + fwd[delta - k] >= N) {
                *px = a1 - x;  *py = b1 - y;
                *pu = a1 - x0; *pv = b1 - y0;
                return 2 * d;
            }
        }
        if (w->budget < (uint64_t)(2 * d + 1)) {
            w->budget = 0;
            w->overflow = 1;
            return DYN_DIFF_TOO_COSTLY;
        }
        w->budget -= (uint64_t)(2 * d + 1);
    }
    /* Unreachable for well-formed input: d = dmax always finds an overlap. */
    *px = *pu = a0; *py = *pv = b0;
    return -1;
}

/* ------------------------------------------------------------- the script */

enum { DYN_OP_DEL = -1, DYN_OP_EQ = 0, DYN_OP_INS = 1 };

typedef struct {
    int8_t op;
    uint32_t a0, a1, b0, b1;
} dyn_hunk_t;

typedef struct {
    dyn_hunk_t *v;
    uint32_t n, cap;
} dyn_script_t;

/* Append, MERGING with the previous hunk when the op matches -- runs come out
   of the divide-and-conquer in pieces and a caller wants whole hunks. */
static int dyn_script_add(dyn_script_t *s, int8_t op, uint32_t a0, uint32_t a1,
                          uint32_t b0, uint32_t b1)
{
    if ((op == DYN_OP_DEL && a0 == a1) || (op == DYN_OP_INS && b0 == b1)
        || (op == DYN_OP_EQ && a0 == a1))
        return 0;
    if (s->n && s->v[s->n - 1].op == op) {
        s->v[s->n - 1].a1 = a1;
        s->v[s->n - 1].b1 = b1;
        return 0;
    }
    if (s->n == s->cap) {
        uint32_t nc = (uint32_t)dyn_grow(s->cap, (size_t)s->cap + 1);
        dyn_hunk_t *nv = (dyn_hunk_t *)realloc(s->v, nc * sizeof(dyn_hunk_t));
        if (!nv) return -1;
        s->v = nv; s->cap = nc;
    }
    s->v[s->n].op = op;
    s->v[s->n].a0 = a0; s->v[s->n].a1 = a1;
    s->v[s->n].b0 = b0; s->v[s->n].b1 = b1;
    s->n++;
    return 0;
}

/* One work item. DIFF splits; the EMIT kinds are already-decided output, held
   on the same stack so the pieces come out in order. */
enum { DYN_W_DIFF, DYN_W_EQ, DYN_W_DEL, DYN_W_INS };

typedef struct {
    int32_t a0, a1, b0, b1;
    int8_t kind;
} dyn_witem_t;

typedef struct {
    dyn_witem_t *v;
    uint32_t n, cap;
} dyn_wstack_t;

static int dyn_wpush(dyn_wstack_t *s, int8_t kind, int32_t a0, int32_t a1,
                     int32_t b0, int32_t b1)
{
    if (s->n == s->cap) {
        uint32_t nc = (uint32_t)dyn_grow(s->cap, (size_t)s->cap + 1);
        dyn_witem_t *nv = (dyn_witem_t *)realloc(s->v, nc * sizeof(dyn_witem_t));
        if (!nv) return -1;
        s->v = nv; s->cap = nc;
    }
    s->v[s->n].kind = kind;
    s->v[s->n].a0 = a0; s->v[s->n].a1 = a1;
    s->v[s->n].b0 = b0; s->v[s->n].b1 = b1;
    s->n++;
    return 0;
}

/* Divide and conquer on an explicit stack. Items are pushed right-to-left so
   they pop in source order, which is what lets hunks merge as they are added. */
static int dyn_diff_run(const uint32_t *A, uint32_t na, const uint32_t *B,
                        uint32_t nb, dyn_snake_ws_t *w, dyn_script_t *out)
{
    dyn_wstack_t st;
    int rc = 0;
    st.v = NULL; st.n = 0; st.cap = 0;
    if (dyn_wpush(&st, DYN_W_DIFF, 0, (int32_t)na, 0, (int32_t)nb) < 0)
        return -1;
    while (st.n) {
        dyn_witem_t it = st.v[--st.n];
        int32_t N, M, x, y, u, v;
        switch (it.kind) {
        case DYN_W_EQ:
            rc = dyn_script_add(out, DYN_OP_EQ, (uint32_t)it.a0, (uint32_t)it.a1,
                                (uint32_t)it.b0, (uint32_t)it.b1);
            break;
        case DYN_W_DEL:
            rc = dyn_script_add(out, DYN_OP_DEL, (uint32_t)it.a0, (uint32_t)it.a1,
                                (uint32_t)it.b0, (uint32_t)it.b0);
            break;
        case DYN_W_INS:
            rc = dyn_script_add(out, DYN_OP_INS, (uint32_t)it.a0, (uint32_t)it.a0,
                                (uint32_t)it.b0, (uint32_t)it.b1);
            break;
        default:
            N = it.a1 - it.a0;
            M = it.b1 - it.b0;
            if (N <= 0 && M <= 0)
                break;
            {
                /* Trim the shared ends first (BOSCC: most real diffs touch
                   little). It is also what keeps the middle snake off the
                   region boundary, where an empty split makes no progress. */
                int32_t p = 0, sfx = 0;
                while (p < N && p < M && A[it.a0 + p] == B[it.b0 + p])
                    p++;
                while (sfx < N - p && sfx < M - p
                       && A[it.a1 - 1 - sfx] == B[it.b1 - 1 - sfx])
                    sfx++;
                if (p || sfx) {
                    if (dyn_wpush(&st, DYN_W_EQ, it.a1 - sfx, it.a1,
                                  it.b1 - sfx, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_DIFF, it.a0 + p, it.a1 - sfx,
                                     it.b0 + p, it.b1 - sfx) < 0
                        || dyn_wpush(&st, DYN_W_EQ, it.a0, it.a0 + p,
                                     it.b0, it.b0 + p) < 0)
                        rc = -1;
                    break;
                }
            }
            if (N <= 0) {
                rc = dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1);
                break;
            }
            if (M <= 0) {
                rc = dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0);
                break;
            }
            /* ONE-TOKEN BASE CASE, AND IT IS LOAD-BEARING, NOT AN OPTIMISATION.
               With even delta the middle snake can land empty on the region
               boundary, making the left sub-region the whole region again --
               an infinite loop. Every such region has N==1 or M==1. */
            if (N == 1 || M == 1) {
                int32_t i, hit = -1;
                if (M == 1) {
                    for (i = it.a0; i < it.a1; i++)
                        if (A[i] == B[it.b0]) { hit = i; break; }
                } else {
                    for (i = it.b0; i < it.b1; i++)
                        if (B[i] == A[it.a0]) { hit = i; break; }
                }
                if (hit < 0) {                 /* nothing shared: replace it */
                    if (dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0) < 0)
                        rc = -1;
                    break;
                }
                /* keep the one common token; push right-to-left so it pops in order */
                if (M == 1) {
                    if (dyn_wpush(&st, DYN_W_DEL, hit + 1, it.a1, it.b1, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_EQ, hit, hit + 1, it.b0, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_DEL, it.a0, hit, it.b0, it.b0) < 0)
                        rc = -1;
                } else {
                    if (dyn_wpush(&st, DYN_W_INS, it.a1, it.a1, hit + 1, it.b1) < 0
                        || dyn_wpush(&st, DYN_W_EQ, it.a0, it.a1, hit, hit + 1) < 0
                        || dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, hit) < 0)
                        rc = -1;
                }
                break;
            }
            if (dyn_middle_snake(A, it.a0, it.a1, B, it.b0, it.b1, w,
                                 &x, &y, &u, &v) < 0) {
                rc = w->overflow ? DYN_DIFF_TOO_COSTLY : -1;
                break;
            }
            /* Progress guard: a split that hands back the whole region would
               loop forever. The base case above is what makes this unreachable;
               it is kept because the alternative failure mode is a hang. */
            if ((x == it.a0 && y == it.b0 && u == it.a0 && v == it.b0)
                || (x >= it.a1 && y >= it.b1) || (u <= it.a0 && v <= it.b0)) {
                if (dyn_wpush(&st, DYN_W_INS, it.a0, it.a0, it.b0, it.b1) < 0
                    || dyn_wpush(&st, DYN_W_DEL, it.a0, it.a1, it.b0, it.b0) < 0)
                    rc = -1;
                break;
            }
            if (dyn_wpush(&st, DYN_W_DIFF, u, it.a1, v, it.b1) < 0
                || dyn_wpush(&st, DYN_W_EQ, x, u, y, v) < 0
                || dyn_wpush(&st, DYN_W_DIFF, it.a0, x, it.b0, y) < 0)
                rc = -1;
            break;
        }
        if (rc < 0)
            break;
    }
    free(st.v);
    return rc;
}

/* ------------------------------------------------------------ entry point */

/* Build one { op, text } from a hunk's token range. */
static JSValue dyn_hunk_value(JSContext *ctx, const dyn_hunk_t *h,
                              const dyn_toks_t *ta, const uint8_t *sa,
                              const dyn_toks_t *tb, const uint8_t *sb)
{
    JSValue o = JS_NewObject(ctx);
    const uint8_t *src;
    uint32_t lo, hi, start, end;
    if (JS_IsException(o))
        return o;
    if (h->op == DYN_OP_INS) { src = sb; lo = h->b0; hi = h->b1; }
    else                     { src = sa; lo = h->a0; hi = h->a1; }
    {
        const dyn_toks_t *t = (h->op == DYN_OP_INS) ? tb : ta;
        start = t->off[lo];
        end = t->off[hi - 1] + t->len[hi - 1];
    }
    if (JS_DefinePropertyValueStr(ctx, o, "op", JS_NewInt32(ctx, h->op),
                                  JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "text",
                                     JS_NewStringLen(ctx, (const char *)src + start,
                                                     end - start),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

/* Diff{Lines,Words,Chars}(a, b) -> [{ op: -1|0|1, text }] in source order.
   Concatenating every hunk with op != 1 rebuilds `a`, and every hunk with
   op != -1 rebuilds `b`; that property is the test's oracle. */
static JSValue dyn_diff(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv, int magic)
{
    dyn_operand_t A, B;
    dyn_toks_t ta, tb;
    dyn_intern_t in;
    dyn_snake_ws_t ws;
    dyn_script_t sc;
    JSValue ret = JS_EXCEPTION, arr;
    uint32_t i;
    int32_t span;
    static const char *const NAMES[3] = { "DiffLines", "DiffWords", "DiffChars" };

    A.cs = NULL; A.cp = NULL; B.cs = NULL; B.cp = NULL;
    memset(&ta, 0, sizeof ta); memset(&tb, 0, sizeof tb);
    memset(&in, 0, sizeof in); memset(&sc, 0, sizeof sc);
    ws.fwd = NULL; ws.rev = NULL; ws.off = 0; ws.overflow = 0;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(a, b): two strings required", NAMES[magic]);
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;

    /* Unchanged input is the common case for a diff, and it needs no tokens,
       no interning and no search: one common hunk, decided on the bytes.
       -DDYN_NO_BYPASS builds the same code with this never firing (the A/B). */
#ifndef DYN_NO_BYPASS
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            goto done;
        if (A.bytes) {
            JSValue h = JS_NewObject(ctx);
            if (JS_IsException(h)) { JS_FreeValue(ctx, arr); goto done; }
            if (JS_DefinePropertyValueStr(ctx, h, "op", JS_NewInt32(ctx, 0),
                                          JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueStr(ctx, h, "text",
                                             JS_NewStringLen(ctx, A.cs, A.bytes),
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, h);        /* h is ours until the array takes it */
                JS_FreeValue(ctx, arr);
                goto done;
            }
            if (JS_DefinePropertyValueUint32(ctx, arr, 0, h, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);      /* the define consumed h */
                goto done;
            }
        }
        ret = arr;
        goto done;
    }
#endif
    if (dyn_tokenize((const uint8_t *)A.cs, (uint32_t)A.bytes, magic, &ta) < 0
        || dyn_tokenize((const uint8_t *)B.cs, (uint32_t)B.bytes, magic, &tb) < 0) {
        JS_ThrowRangeError(ctx, "%s(a, b): more than %u tokens per side",
                           NAMES[magic], DYN_DIFF_MAX_TOKENS);
        goto done;
    }
    if (dyn_intern_init(&in, ta.n + tb.n) < 0) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (i = 0; i < ta.n; i++)
        ta.id[i] = dyn_intern(&in, (const uint8_t *)A.cs, ta.off[i], ta.len[i]);
    for (i = 0; i < tb.n; i++)
        tb.id[i] = dyn_intern(&in, (const uint8_t *)B.cs, tb.off[i], tb.len[i]);

    span = (int32_t)(ta.n + tb.n) + 3;
    ws.off = span;
    ws.budget = DYN_DIFF_MAX_DIAGONAL_STEPS;
    ws.fwd = (int32_t *)calloc((size_t)span * 2 + 1, sizeof(int32_t));
    ws.rev = (int32_t *)calloc((size_t)span * 2 + 1, sizeof(int32_t));
    if (!ws.fwd || !ws.rev) { JS_ThrowOutOfMemory(ctx); goto done; }

    {
        int32_t rc = dyn_diff_run(ta.id, ta.n, tb.id, tb.n, &ws, &sc);
        if (rc == DYN_DIFF_TOO_COSTLY) {
            JS_ThrowRangeError(ctx,
                "%s(a, b): inputs too dissimilar (edit distance exceeds cap); "
                "shorten or align the input",
                NAMES[magic]);
            goto done;
        }
        if (rc < 0) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto done;
    for (i = 0; i < sc.n; i++) {
        JSValue h = dyn_hunk_value(ctx, &sc.v[i], &ta, (const uint8_t *)A.cs,
                                   &tb, (const uint8_t *)B.cs);
        if (JS_IsException(h)
            || JS_DefinePropertyValueUint32(ctx, arr, i, h, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            goto done;
        }
    }
    ret = arr;
 done:
    free(ws.fwd); free(ws.rev); free(sc.v);
    dyn_intern_free(&in);
    dyn_toks_free(&ta); dyn_toks_free(&tb);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}
