#define DYN_APPROX_MAX_BYTES (16u * 1024u * 1024u)

#define DYN_LEV_WORD_BITS 64

static size_t dyn_utf8_count(const uint8_t* s, size_t n, int* ascii)
{
    size_t i = 0, cnt = 0;
    int hi = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        if (c < 0x80) {
            i++;
            cnt++;
            continue;
        }
        hi = 1;
        if ((c & 0xE0) == 0xC0)
            need = 2;
        else if ((c & 0xF0) == 0xE0)
            need = 3;
        else if ((c & 0xF8) == 0xF0)
            need = 4;
        else
            return (size_t)-1;
        if (i + need > n)
            return (size_t)-1;
        for (size_t k = 1; k < need; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return (size_t)-1;
        i += need;
        cnt++;
    }
    *ascii = !hi;
    return cnt;
}

static void dyn_utf8_decode(const uint8_t* s, size_t n, uint32_t* out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            out[o++] = c;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            out[o++] = ((uint32_t)(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            out[o++] = ((uint32_t)(c & 0x0F) << 12)
                | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            i += 3;
        } else {
            out[o++] = ((uint32_t)(c & 0x07) << 18)
                | ((uint32_t)(s[i + 1] & 0x3F) << 12)
                | ((uint32_t)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            i += 4;
        }
    }
}

#define DYN_EQ_EMPTY 0xFFFFFFFFu
#define DYN_EQ_SLOTS 256

typedef struct {
    uint32_t key[DYN_EQ_SLOTS];
    uint64_t mask[DYN_EQ_SLOTS];
} dyn_eq_t;

static uint32_t dyn_eq_slot(uint32_t c)
{
    c *= 2654435761u;
    return (c ^ (c >> 16)) & (DYN_EQ_SLOTS - 1);
}

static void dyn_eq_build(dyn_eq_t* e, const uint32_t* pat, size_t m)
{
    size_t i;
    for (i = 0; i < DYN_EQ_SLOTS; i++)
        e->key[i] = DYN_EQ_EMPTY;
    for (i = 0; i < m; i++) {
        uint32_t s = dyn_eq_slot(pat[i]);
        while (e->key[s] != DYN_EQ_EMPTY && e->key[s] != pat[i])
            s = (s + 1) & (DYN_EQ_SLOTS - 1);
        if (e->key[s] == DYN_EQ_EMPTY) {
            e->key[s] = pat[i];
            e->mask[s] = 0;
        }
        e->mask[s] |= (uint64_t)1 << i;
    }
}

static uint64_t dyn_eq_get(const dyn_eq_t* e, uint32_t c)
{
    uint32_t s = dyn_eq_slot(c);
    while (e->key[s] != DYN_EQ_EMPTY) {
        if (e->key[s] == c)
            return e->mask[s];
        s = (s + 1) & (DYN_EQ_SLOTS - 1);
    }
    return 0;
}

#define DYN_LEV_MYERS_BODY(EQ_OF)                     \
    uint64_t vp = ~(uint64_t)0, vn = 0;               \
    uint64_t top = (uint64_t)1 << (m - 1);            \
    size_t score = m, j;                              \
    for (j = 0; j < n; j++) {                         \
        uint64_t eq = (EQ_OF);                        \
        uint64_t xv = eq | vn;                        \
        uint64_t xh = ((((eq & vp) + vp) ^ vp) | eq); \
        uint64_t ph = vn | ~(xh | vp);                \
        uint64_t mh = vp & xh;                        \
        if (ph & top)                                 \
            score++;                                  \
        if (mh & top)                                 \
            score--;                                  \
        ph = (ph << 1) | 1;                           \
        mh = (mh << 1);                               \
        vp = mh | ~(xv | ph);                         \
        vn = ph & xv;                                 \
    }                                                 \
    return score;

static size_t dyn_lev_myers_u8(const uint8_t* pat, size_t m,
    const uint8_t* txt, size_t n)
{
    uint64_t eqt[128];
    size_t i;
    for (i = 0; i < 128; i++)
        eqt[i] = 0;
    for (i = 0; i < m; i++)
        eqt[pat[i]] |= (uint64_t)1 << i;
    DYN_LEV_MYERS_BODY(eqt[txt[j]])
}

static size_t dyn_lev_myers_cp(const uint32_t* pat, size_t m,
    const uint32_t* txt, size_t n)
{
    dyn_eq_t e;
    dyn_eq_build(&e, pat, m);
    DYN_LEV_MYERS_BODY(dyn_eq_get(&e, txt[j]))
}

#define DYN_LEV_DP_BODY                                        \
    size_t i, j, lo, hi;                                       \
    for (j = 0; j <= m; j++)                                   \
        prev[j] = j;                                           \
    for (i = 1; i <= n; i++) {                                 \
        lo = (i > band) ? i - band : 1;                        \
        hi = (i + band < m) ? i + band : m;                    \
        cur[lo - 1] = (lo == 1) ? i : DYN_LEV_INF;             \
        for (j = lo; j <= hi; j++) {                           \
            size_t del = prev[j] + 1;                          \
            size_t ins = cur[j - 1] + 1;                       \
            size_t sub = prev[j - 1] + (A[i - 1] != B[j - 1]); \
            size_t v = del < ins ? del : ins;                  \
            cur[j] = v < sub ? v : sub;                        \
        }                                                      \
        if (hi < m)                                            \
            cur[hi + 1] = DYN_LEV_INF;                         \
        {                                                      \
            size_t* t = prev;                                  \
            prev = cur;                                        \
            cur = t;                                           \
        }                                                      \
    }                                                          \
    return prev[m];

#define DYN_LEV_INF ((size_t)-1 / 4)

static size_t dyn_lev_dp_u8(const uint8_t* A, size_t n, const uint8_t* B,
    size_t m, size_t band, size_t* prev, size_t* cur)
{
    DYN_LEV_DP_BODY
}

static size_t dyn_lev_dp_cp(const uint32_t* A, size_t n, const uint32_t* B,
    size_t m, size_t band, size_t* prev, size_t* cur)
{
    DYN_LEV_DP_BODY
}

#ifdef DYN_APPROX_REFERENCE
static size_t dyn_lev_reference(const uint32_t* A, size_t n, const uint32_t* B,
    size_t m, size_t* scratch)
{
    size_t i, j;
    for (j = 0; j <= m; j++)
        scratch[j] = j;
    for (i = 1; i <= n; i++) {
        size_t diag = scratch[0];
        scratch[0] = i;
        for (j = 1; j <= m; j++) {
            size_t up = scratch[j], v;
            size_t sub = diag + (A[i - 1] != B[j - 1]);
            v = (scratch[j - 1] + 1 < up + 1) ? scratch[j - 1] + 1 : up + 1;
            scratch[j] = (v < sub) ? v : sub;
            diag = up;
        }
    }
    return scratch[m];
}
#endif

typedef struct {
    const char* cs;
    const uint8_t* u8;
    uint32_t* cp;
    size_t n;
    size_t bytes;
    int ascii;
} dyn_operand_t;

static void dyn_operand_free(JSContext* ctx, dyn_operand_t* o)
{
    if (o->cs)
        JS_FreeCString(ctx, o->cs);
    free(o->cp);
    o->cs = NULL;
    o->cp = NULL;
}

static int dyn_operand_get(JSContext* ctx, JSValueConst v, const char* what,
    dyn_operand_t* o)
{
    size_t blen, cnt;
    int ascii = 1;

    o->cs = NULL;
    o->u8 = NULL;
    o->cp = NULL;
    o->n = 0;
    o->bytes = 0;
    o->ascii = 1;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:matcher: %s must be a string", what);
        return -1;
    }
    o->cs = JS_ToCStringLen(ctx, &blen, v);
    if (!o->cs)
        return -1;
    if (blen > DYN_APPROX_MAX_BYTES) {
        JS_ThrowRangeError(ctx, "dyna:matcher: %s exceeds the %u-byte limit",
            what, DYN_APPROX_MAX_BYTES);
        goto fail;
    }
    cnt = dyn_utf8_count((const uint8_t*)o->cs, blen, &ascii);
    if (cnt == (size_t)-1) {
        JS_ThrowTypeError(ctx, "dyna:matcher: %s is not valid UTF-8", what);
        goto fail;
    }
    o->n = cnt;
    o->bytes = blen;
    o->ascii = ascii;
    if (ascii) {
        o->u8 = (const uint8_t*)o->cs;
        return 0;
    }
    o->cp = (uint32_t*)malloc(cnt ? cnt * sizeof(uint32_t) : 1);
    if (!o->cp) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    dyn_utf8_decode((const uint8_t*)o->cs, blen, o->cp);
    return 0;
fail:
    dyn_operand_free(ctx, o);
    return -1;
}

static int dyn_operand_widen(JSContext* ctx, dyn_operand_t* o)
{
    size_t i;
    if (!o->ascii)
        return 0;
    o->cp = (uint32_t*)malloc(o->n ? o->n * sizeof(uint32_t) : 1);
    if (!o->cp) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < o->n; i++)
        o->cp[i] = o->u8[i];
    o->ascii = 0;
    o->u8 = NULL;
    return 0;
}

static int dyn_lev_read_max(JSContext* ctx, int argc, JSValueConst* argv,
    int64_t* out, const char* fname)
{
    JSValue v;
    *out = -1;
    if (argc <= 2 || !JS_IsObject(argv[2]))
        return 0;
    v = JS_GetPropertyStr(ctx, argv[2], "max");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, out, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (*out < 0) {
        JS_ThrowRangeError(ctx, "%s(a, b, { max }): max must be >= 0", fname);
        return -1;
    }
    return 0;
}

#define DYN_LEV_MAX_CELLS 400000000ull

static JSValue dyn_levenshtein(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    size_t band = (size_t)-1, n, m, dist, diff;
    size_t* row = NULL;
    int64_t maxv = -1;
    JSValue ret = JS_EXCEPTION;
    const dyn_operand_t *pat, *txt;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (dyn_lev_read_max(ctx, argc, argv, &maxv, "Levenshtein") < 0)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Levenshtein(a, b[, options]): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;

    if (A.n <= B.n) {
        pat = &A;
        txt = &B;
    } else {
        pat = &B;
        txt = &A;
    }
    m = pat->n;
    n = txt->n;
    diff = n - m;
    if (maxv < 0 && m && n > DYN_LEV_MAX_CELLS / m) {
        JS_ThrowRangeError(ctx,
            "Levenshtein: %zu x %zu exceeds %llu cells; pass { max } to bound "
            "the distance, or shorten the input",
            n, m, (unsigned long long)DYN_LEV_MAX_CELLS);
        goto done;
    }
    if (maxv >= 0) {
        band = (size_t)maxv;
        if (diff > band) {
            ret = JS_NewInt64(ctx, (int64_t)band + 1);
            goto done;
        }
    }
    if (m == 0) {
        dist = n;
    } else if (m <= DYN_LEV_WORD_BITS) {
        if (pat->ascii && txt->ascii) {
            dist = dyn_lev_myers_u8(pat->u8, m, txt->u8, n);
        } else {
            if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
                goto done;
            dist = dyn_lev_myers_cp(pat->cp, m, txt->cp, n);
        }
    } else {
        size_t need, width;
        if (band == (size_t)-1)
            band = n;
        width = band >= m ? m : 2 * band + 1;
        if (n > DYN_LEV_MAX_CELLS / width) {
            JS_ThrowRangeError(ctx,
                "Levenshtein: %zu x %zu exceeds %llu cells even with { max }; "
                "shorten the input",
                n, m, (unsigned long long)DYN_LEV_MAX_CELLS);
            goto done;
        }
        need = (m + 2) * 2;
        row = (size_t*)malloc(need * sizeof(size_t));
        if (!row) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (pat->ascii && txt->ascii) {
            dist = dyn_lev_dp_u8(txt->u8, n, pat->u8, m, band, row, row + m + 2);
        } else {
            if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
                goto done;
            dist = dyn_lev_dp_cp(txt->cp, n, pat->cp, m, band, row, row + m + 2);
        }
    }
    if (maxv >= 0 && dist > (size_t)maxv)
        dist = (size_t)maxv + 1;
    ret = JS_NewInt64(ctx, (int64_t)dist);
done:
    free(row);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}

#define DYN_SW_MAX_CELLS 400000000ull
#define DYN_SW_MAX_TRACE 268435456ull
#define DYN_SW_SCORE_LIMIT 1000

typedef struct {
    const uint32_t* cp;
    const uint8_t* u8;
    size_t n;
} dyn_sw_seq_t;

typedef struct {
    int32_t match, mismatch, gap;
    int64_t band, diagonal;
    int traceback;
} dyn_sw_opts_t;

static inline uint32_t dyn_sw_at(const dyn_sw_seq_t* s, size_t i)
{
    return s->u8 ? s->u8[i] : s->cp[i];
}

static int dyn_sw_tokens(JSContext* ctx, JSValueConst v, const char* what,
    dyn_sw_seq_t* out)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (bpe != 4) {
        JS_ThrowTypeError(ctx, "SmithWaterman: %s must be a string or a 4-byte typed array (Int32Array, Uint32Array)", what);
        return -1;
    }
    if (!base) {
        JS_ThrowTypeError(ctx, "typed array buffer is detached");
        return -1;
    }
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    out->cp = (const uint32_t*)(base + off);
    out->u8 = NULL;
    out->n = len / 4;
    return 0;
}

static int dyn_sw_int_opt(JSContext* ctx, JSValueConst opts, const char* key,
    int64_t lo, int64_t hi, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= (double)lo && d <= (double)hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "SmithWaterman: %s must be an integer in [%lld, %lld]",
            key, (long long)lo, (long long)hi);
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static int dyn_sw_read_opts(JSContext* ctx, JSValueConst opts, dyn_sw_opts_t* o)
{
    static const char* const keys[] = { "match", "mismatch", "gap", "band", "diagonal", "traceback" };
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int64_t v;
    size_t k;

    o->match = 2;
    o->mismatch = -1;
    o->gap = -1;
    o->band = -1;
    o->diagonal = 0;
    o->traceback = 1;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "SmithWaterman: options must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        int known = 0;
        if (!name) {
            JS_FreePropertyEnum(ctx, props, nprops);
            return -1;
        }
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (!strcmp(name, keys[k]))
                known = 1;
        if (!known) {
            JS_ThrowTypeError(ctx, "SmithWaterman: unknown option \"%s\" (valid: match, mismatch, gap, band, diagonal, traceback)", name);
            JS_FreeCString(ctx, name);
            JS_FreePropertyEnum(ctx, props, nprops);
            return -1;
        }
        JS_FreeCString(ctx, name);
    }
    JS_FreePropertyEnum(ctx, props, nprops);
    v = o->match;
    if (dyn_sw_int_opt(ctx, opts, "match", 1, DYN_SW_SCORE_LIMIT, &v) < 0)
        return -1;
    o->match = (int32_t)v;
    v = o->mismatch;
    if (dyn_sw_int_opt(ctx, opts, "mismatch", -DYN_SW_SCORE_LIMIT, DYN_SW_SCORE_LIMIT, &v) < 0)
        return -1;
    o->mismatch = (int32_t)v;
    v = o->gap;
    if (dyn_sw_int_opt(ctx, opts, "gap", -DYN_SW_SCORE_LIMIT, 0, &v) < 0)
        return -1;
    o->gap = (int32_t)v;
    if (o->mismatch > o->match) {
        JS_ThrowRangeError(ctx, "SmithWaterman: mismatch must not exceed match");
        return -1;
    }
    if (dyn_sw_int_opt(ctx, opts, "band", 0, 1 << 30, &o->band) < 0
        || dyn_sw_int_opt(ctx, opts, "diagonal", -(1 << 30), 1 << 30, &o->diagonal) < 0)
        return -1;
    {
        JSValue t = JS_GetPropertyStr(ctx, opts, "traceback");
        if (JS_IsException(t))
            return -1;
        if (!JS_IsUndefined(t))
            o->traceback = JS_ToBool(ctx, t);
        JS_FreeValue(ctx, t);
    }
    return 0;
}

static JSValue dyn_sw_result(JSContext* ctx, int32_t score, size_t a0, size_t a1,
    size_t b0, size_t b1, size_t matches, const char* cigar, int traceback)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "score", JS_NewInt32(ctx, score), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "aStart", JS_NewInt64(ctx, (int64_t)a0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "aEnd", JS_NewInt64(ctx, (int64_t)a1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bStart", JS_NewInt64(ctx, (int64_t)b0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "bEnd", JS_NewInt64(ctx, (int64_t)b1), JS_PROP_C_W_E);
    if (traceback) {
        JS_DefinePropertyValueStr(ctx, o, "matches", JS_NewInt64(ctx, (int64_t)matches), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "cigar", JS_NewString(ctx, cigar ? cigar : ""), JS_PROP_C_W_E);
    }
    return o;
}

static char* dyn_sw_cigar(const uint8_t* ops, size_t nops)
{
    size_t cap = nops * 12 + 1, len = 0, i = 0;
    char* out = (char*)malloc(cap);
    if (!out)
        return NULL;
    while (i < nops) {
        size_t run = 1;
        while (i + run < nops && ops[i + run] == ops[i])
            run++;
        len += (size_t)snprintf(out + len, cap - len, "%zu%c", run, ops[i] == 1 ? 'M' : ops[i] == 2 ? 'I'
                                                                                                    : 'D');
        i += run;
    }
    out[len] = 0;
    return out;
}

typedef struct {
    const dyn_sw_seq_t* a;
    const dyn_sw_seq_t* b;
    dyn_sw_opts_t o;
    size_t n, m, width, bi, bj;
    int64_t lo_off, hi_off;
    uint8_t* trace;
    int32_t best;
} dyn_sw_grid_t;

static inline size_t dyn_sw_col(const dyn_sw_grid_t* g, size_t i, size_t j)
{
    return g->o.band >= 0 ? (size_t)((int64_t)j - (int64_t)i - g->lo_off) : j;
}

static void dyn_sw_fill(dyn_sw_grid_t* g, int32_t* prev, int32_t* cur)
{
    size_t i, j, m = g->m;
    for (i = 1; i <= g->n; i++) {
        int64_t jl = (int64_t)i + g->lo_off, jh = (int64_t)i + g->hi_off;
        size_t j0 = jl<1 ? 1 : (size_t)jl, j1 = jh>(int64_t) m ? m : (jh < 0 ? 0 : (size_t)jh);
        uint32_t ca = dyn_sw_at(g->a, i - 1);
        int32_t* t;
        if (j0 >= 2 && j0 - 1 <= m)
            cur[j0 - 1] = 0;
        if (j0 >= 3 && j0 - 2 <= m)
            cur[j0 - 2] = 0;
        cur[0] = 0;
        for (j = j0; j <= j1; j++) {
            int32_t d = prev[j - 1] + (ca == dyn_sw_at(g->b, j - 1) ? g->o.match : g->o.mismatch);
            int32_t u = prev[j] + g->o.gap, l = cur[j - 1] + g->o.gap, v = 0;
            v = d > v ? d : v;
            v = u > v ? u : v;
            v = l > v ? l : v;
            cur[j] = v;
            if (g->trace)
                g->trace[(i - 1) * g->width + dyn_sw_col(g, i, j)] = v == 0 ? 0 : v == d ? 1
                    : v == u                                                             ? 2
                                                                                         : 3;
            if (v > g->best) {
                g->best = v;
                g->bi = i;
                g->bj = j;
            }
        }
        if (j1 + 1 <= m && j1 + 1 >= j0)
            cur[j1 + 1] = 0;
        t = prev;
        prev = cur;
        cur = t;
    }
}

static size_t dyn_sw_traceback(const dyn_sw_grid_t* g, uint8_t* ops, size_t* pi,
    size_t* pj, size_t* matches)
{
    size_t i = g->bi, j = g->bj, nops = 0, x;
    while (i > 0 && j > 0) {
        int64_t k = g->o.band >= 0 ? (int64_t)j - (int64_t)i - g->lo_off : (int64_t)j;
        uint8_t p;
        if (k < 0 || k >= (int64_t)g->width)
            break;
        p = g->trace[(i - 1) * g->width + (size_t)k];
        if (!p)
            break;
        ops[nops++] = p;
        if (p == 1) {
            *matches += dyn_sw_at(g->a, i - 1) == dyn_sw_at(g->b, j - 1);
            i--;
            j--;
        } else if (p == 2) {
            i--;
        } else {
            j--;
        }
    }
    for (x = 0; x < nops / 2; x++) {
        uint8_t tmp = ops[x];
        ops[x] = ops[nops - 1 - x];
        ops[nops - 1 - x] = tmp;
    }
    *pi = i;
    *pj = j;
    return nops;
}

static JSValue dyn_smith_waterman(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    dyn_sw_seq_t sa, sb;
    dyn_sw_opts_t o;
    int strings;
    size_t n, m, width, cells;
    dyn_sw_grid_t g;
    int64_t lo_off, hi_off;
    int32_t *prev = NULL, *cur = NULL;
    uint8_t* trace = NULL;
    uint8_t* ops = NULL;
    char* cigar = NULL;
    JSValue ret = JS_EXCEPTION;

    (void)this_val;
    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "SmithWaterman(a, b[, options]): two sequences required");
    if (dyn_sw_read_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;
    strings = JS_IsString(argv[0]);
    if (strings != JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "SmithWaterman: a and b must both be strings or both be 4-byte typed arrays");
    if (strings) {
        if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
            return JS_EXCEPTION;
        if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
            goto done;
        if (A.ascii != B.ascii && (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0))
            goto done;
        sa.u8 = A.ascii ? A.u8 : NULL;
        sa.cp = A.cp;
        sa.n = A.n;
        sb.u8 = B.ascii ? B.u8 : NULL;
        sb.cp = B.cp;
        sb.n = B.n;
    } else if (dyn_sw_tokens(ctx, argv[0], "a", &sa) < 0 || dyn_sw_tokens(ctx, argv[1], "b", &sb) < 0) {
        goto done;
    }
    n = sa.n;
    m = sb.n;
    if (o.band >= 0) {
        lo_off = o.diagonal - o.band;
        hi_off = o.diagonal + o.band;
        width = (size_t)(2 * o.band + 1);
    } else {
        lo_off = -(int64_t)n;
        hi_off = (int64_t)m;
        width = m + 1;
    }
    cells = n ? (width > DYN_SW_MAX_CELLS / n ? (size_t)-1 : n * width) : 0;
    if (cells > DYN_SW_MAX_CELLS) {
        JS_ThrowRangeError(ctx, "SmithWaterman: %zu x %zu exceeds %llu cells; pass { band } or shorten the input",
            n, m, (unsigned long long)DYN_SW_MAX_CELLS);
        goto done;
    }
    if (o.traceback && cells > DYN_SW_MAX_TRACE) {
        JS_ThrowRangeError(ctx, "SmithWaterman: traceback over %zu cells exceeds %llu; pass { band }, { traceback: false } or shorten the input",
            cells, (unsigned long long)DYN_SW_MAX_TRACE);
        goto done;
    }
    prev = (int32_t*)calloc(m + 1, sizeof(int32_t));
    cur = (int32_t*)calloc(m + 1, sizeof(int32_t));
    if (!prev || !cur || (o.traceback && cells && !(trace = (uint8_t*)calloc(cells, 1)))) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    g.a = &sa;
    g.b = &sb;
    g.o = o;
    g.n = n;
    g.m = m;
    g.width = width;
    g.lo_off = lo_off;
    g.hi_off = hi_off;
    g.trace = trace;
    g.best = 0;
    g.bi = 0;
    g.bj = 0;
    dyn_sw_fill(&g, prev, cur);
    if (!o.traceback) {
        ret = dyn_sw_result(ctx, g.best, 0, g.bi, 0, g.bj, 0, NULL, 0);
        goto done;
    }
    {
        size_t nops, matches = 0, si, sj;
        ops = (uint8_t*)malloc(g.bi + g.bj + 1);
        if (!ops) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        nops = dyn_sw_traceback(&g, ops, &si, &sj, &matches);
        cigar = dyn_sw_cigar(ops, nops);
        if (!cigar) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        ret = g.best ? dyn_sw_result(ctx, g.best, si, g.bi, sj, g.bj, matches, cigar, 1)
                     : dyn_sw_result(ctx, 0, 0, 0, 0, 0, 0, "", 1);
    }
done:
    free(prev);
    free(cur);
    free(trace);
    free(ops);
    free(cigar);
    if (strings) {
        dyn_operand_free(ctx, &A);
        dyn_operand_free(ctx, &B);
    }
    return ret;
}

#ifdef DYN_APPROX_REFERENCE
static JSValue dyn_levenshtein_ref(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    size_t* scratch = NULL;
    JSValue ret = JS_EXCEPTION;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "LevenshteinReference(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;
    scratch = (size_t*)malloc((B.n + 1) * sizeof(size_t));
    if (!scratch) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = JS_NewInt64(ctx, (int64_t)dyn_lev_reference(A.cp, A.n, B.cp, B.n, scratch));
done:
    free(scratch);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}
#endif

static int dyn_is_ascii_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v';
}

static size_t dyn_strip_spaces(uint32_t* s, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++)
        if (!dyn_is_ascii_space(s[i]))
            s[k++] = s[i];
    return k;
}

typedef struct {
    uint64_t* key;
    uint32_t* cnt;
    uint8_t* occ;
    size_t mask;
    size_t used;
} dyn_gram_set_t;

static size_t dyn_gram_hash(uint64_t g, size_t mask)
{
    uint64_t h = g * 0x9E3779B97F4A7C15ull;
    return (size_t)((h ^ (h >> 29)) & mask);
}

static int dyn_gram_grow(dyn_gram_set_t* s)
{
    size_t ncap = (s->mask + 1) * 2;
    uint64_t* nkey;
    uint32_t* ncnt;
    uint8_t* nocc;
    size_t i;

    if (ncap < s->mask + 1)
        return -1;
    nkey = (uint64_t*)calloc(ncap, sizeof(*nkey));
    ncnt = (uint32_t*)calloc(ncap, sizeof(*ncnt));
    nocc = (uint8_t*)calloc(ncap, 1);
    if (!nkey || !ncnt || !nocc) {
        free(nkey);
        free(ncnt);
        free(nocc);
        return -1;
    }
    for (i = 0; i <= s->mask; i++) {
        size_t j;
        if (!s->occ[i])
            continue;
        j = dyn_gram_hash(s->key[i], ncap - 1);
        while (nocc[j])
            j = (j + 1) & (ncap - 1);
        nkey[j] = s->key[i];
        ncnt[j] = s->cnt[i];
        nocc[j] = 1;
    }
    free(s->key);
    free(s->cnt);
    free(s->occ);
    s->key = nkey;
    s->cnt = ncnt;
    s->occ = nocc;
    s->mask = ncap - 1;
    return 0;
}

static int dyn_gram_add(dyn_gram_set_t* s, uint64_t g)
{
    size_t i;

    if ((s->used + 1) * 4 > (s->mask + 1) * 3) {
        if (dyn_gram_grow(s) < 0)
            return -1;
    }
    i = dyn_gram_hash(g, s->mask);
    while (s->occ[i] && s->key[i] != g)
        i = (i + 1) & s->mask;
    if (!s->occ[i])
        s->used++;
    s->key[i] = g;
    s->occ[i] = 1;
    s->cnt[i]++;
    return 0;
}

static int dyn_gram_take(dyn_gram_set_t* s, uint64_t g)
{
    size_t i = dyn_gram_hash(g, s->mask);
    while (s->occ[i]) {
        if (s->key[i] == g) {
            if (s->cnt[i] == 0)
                return 0;
            s->cnt[i]--;
            return 1;
        }
        i = (i + 1) & s->mask;
    }
    return 0;
}

static JSValue dyn_dice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    dyn_gram_set_t set;
    size_t i, na, nb, an, bn, hits = 0;
    JSValue ret = JS_EXCEPTION;

    set.key = NULL;
    set.cnt = NULL;
    set.occ = NULL;
    set.used = 0;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "DiceCoefficient(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
#ifndef DYN_NO_BYPASS
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        ret = JS_NewFloat64(ctx, 1.0);
        goto done;
    }
#endif
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;

    na = dyn_strip_spaces(A.cp, A.n);
    nb = dyn_strip_spaces(B.cp, B.n);

    if (na == nb) {
        for (i = 0; i < na && A.cp[i] == B.cp[i]; i++)
            ;
        if (i == na) {
            ret = JS_NewFloat64(ctx, 1.0);
            goto done;
        }
    }
    if (na < 2 || nb < 2) {
        ret = JS_NewFloat64(ctx, 0.0);
        goto done;
    }

    an = na - 1;
    bn = nb - 1;
    set.mask = 15;
    set.key = (uint64_t*)calloc(16, sizeof(uint64_t));
    set.cnt = (uint32_t*)calloc(16, sizeof(uint32_t));
    set.occ = (uint8_t*)calloc(16, 1);
    if (!set.key || !set.cnt || !set.occ) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    for (i = 0; i < an; i++)
        if (dyn_gram_add(&set, ((uint64_t)A.cp[i] << 32) | A.cp[i + 1]) < 0) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    for (i = 0; i < bn; i++)
        hits += (size_t)dyn_gram_take(&set, ((uint64_t)B.cp[i] << 32) | B.cp[i + 1]);

    ret = JS_NewFloat64(ctx, (2.0 * (double)hits) / (double)(an + bn));
done:
    free(set.key);
    free(set.cnt);
    free(set.occ);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}

static double dyn_jaro_kernel(const uint32_t* a, size_t na,
    const uint32_t* b, size_t nb)
{
    uint8_t* bflag;
    uint32_t* amatch;
    size_t i, j, m = 0, t = 0, k = 0;
    size_t win = (na > nb ? na : nb) / 2;
    double jaro;

    if (win > 0)
        win--;
    bflag = (uint8_t*)calloc(nb ? nb : 1, 1);
    amatch = (uint32_t*)malloc((na ? na : 1) * sizeof(uint32_t));
    if (!bflag || !amatch) {
        free(bflag);
        free(amatch);
        return -1.0;
    }
    for (i = 0; i < na; i++) {
        size_t lo = i > win ? i - win : 0;
        size_t hi = i + win + 1 < nb ? i + win + 1 : nb;
        for (j = lo; j < hi; j++) {
            if (!bflag[j] && b[j] == a[i]) {
                bflag[j] = 1;
                amatch[m++] = a[i];
                break;
            }
        }
    }
    if (m == 0) {
        free(bflag);
        free(amatch);
        return 0.0;
    }
    for (j = 0; j < nb; j++) {
        if (!bflag[j])
            continue;
        if (b[j] != amatch[k])
            t++;
        k++;
    }
    free(bflag);
    free(amatch);
    t /= 2;
    jaro = ((double)m / (double)na + (double)m / (double)nb
               + ((double)m - (double)t) / (double)m)
        / 3.0;
    return jaro;
}

static JSValue dyn_jaro_winkler(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    size_t l = 0, span;
    double jaro;
    JSValue ret = JS_EXCEPTION;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "JaroWinkler(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
#ifndef DYN_NO_BYPASS
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        ret = JS_NewFloat64(ctx, 1.0);
        goto done;
    }
#endif
    if (A.n == 0 || B.n == 0) {
        ret = JS_NewFloat64(ctx, A.n == 0 && B.n == 0 ? 1.0 : 0.0);
        goto done;
    }
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;

    span = 2 * (((A.n > B.n ? A.n : B.n) / 2) ? ((A.n > B.n ? A.n : B.n) / 2) - 1 : 0) + 1;
    if (A.n > DYN_LEV_MAX_CELLS / span) {
        JS_ThrowRangeError(ctx,
            "JaroWinkler: %zu x %zu exceeds %llu cells; shorten the input",
            A.n, B.n, (unsigned long long)DYN_LEV_MAX_CELLS);
        goto done;
    }
    jaro = dyn_jaro_kernel(A.cp, A.n, B.cp, B.n);
    if (jaro < 0.0) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    if (jaro > 0.7) {
        while (l < 4 && l < A.n && l < B.n && A.cp[l] == B.cp[l])
            l++;
        jaro += (double)l * 0.1 * (1.0 - jaro);
    }
    ret = JS_NewFloat64(ctx, jaro);
done:
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}

#define DYN_OSA_DP_BODY(W)                                                                              \
    size_t i, j, lo, hi, base = 0;                                                                      \
    r1[0] = DYN_LEV_INF;                                                                                \
    for (j = 0; j <= m && j + 1 < (W); j++)                                                             \
        r1[j + 1] = j;                                                                                  \
    for (j = 0; j < (W); j++)                                                                           \
        r0[j] = DYN_LEV_INF;                                                                            \
    for (i = 1; i <= n; i++) {                                                                          \
        lo = (i > band) ? i - band : 1;                                                                 \
        hi = (i + band < m) ? i + band : m;                                                             \
        if (lo > base + 1) {                                                                            \
            memmove(r0, r0 + 1, ((W) - 1) * sizeof(size_t));                                            \
            memmove(r1, r1 + 1, ((W) - 1) * sizeof(size_t));                                            \
            base++;                                                                                     \
        }                                                                                               \
        r2[lo - base] = (lo == 1) ? i : DYN_LEV_INF;                                                    \
        for (j = lo; j <= hi; j++) {                                                                    \
            size_t jj = j - base;                                                                       \
            size_t del = r1[jj + 1] + 1;                                                                \
            size_t ins = r2[jj] + 1;                                                                    \
            size_t sub = r1[jj] + (A[i - 1] != B[j - 1]);                                               \
            size_t v = del < ins ? del : ins;                                                           \
            if (v > sub)                                                                                \
                v = sub;                                                                                \
            if (i >= 2 && j >= 2 && A[i - 1] == B[j - 2] && A[i - 2] == B[j - 1] && r0[jj - 1] + 1 < v) \
                v = r0[jj - 1] + 1;                                                                     \
            r2[jj + 1] = v;                                                                             \
        }                                                                                               \
        if (hi < m)                                                                                     \
            r2[hi + 2 - base] = DYN_LEV_INF;                                                            \
        {                                                                                               \
            size_t* t = r0;                                                                             \
            r0 = r1;                                                                                    \
            r1 = r2;                                                                                    \
            r2 = t;                                                                                     \
        }                                                                                               \
    }                                                                                                   \
    return r1[m + 1 - base];

static size_t dyn_osa_dp_u8(const uint8_t* A, size_t n, const uint8_t* B,
    size_t m, size_t band, size_t* r0, size_t* r1,
    size_t* r2, size_t width)
{
    DYN_OSA_DP_BODY(width)
}

static size_t dyn_osa_dp_cp(const uint32_t* A, size_t n, const uint32_t* B,
    size_t m, size_t band, size_t* r0, size_t* r1,
    size_t* r2, size_t width)
{
    DYN_OSA_DP_BODY(width)
}

static JSValue dyn_damerau_levenshtein(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    size_t band, n, m, dist, diff, width;
    size_t* rows = NULL;
    int64_t maxv = -1;
    JSValue ret = JS_EXCEPTION;
    const dyn_operand_t *pat, *txt;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (dyn_lev_read_max(ctx, argc, argv, &maxv, "DamerauLevenshtein") < 0)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "DamerauLevenshtein(a, b[, options]): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;

    if (A.n <= B.n) {
        pat = &A;
        txt = &B;
    } else {
        pat = &B;
        txt = &A;
    }
    m = pat->n;
    n = txt->n;
    diff = n - m;
    if (maxv >= 0 && (uint64_t)diff > (uint64_t)maxv) {
        ret = JS_NewInt64(ctx, maxv + 1);
        goto done;
    }
    band = maxv >= 0 ? (size_t)maxv : n;
    if (band > n)
        band = n;
    if (m == 0) {
        dist = n;
    } else {
        width = 2 * band + 4 < m + 3 ? 2 * band + 4 : m + 3;
        if (n > DYN_LEV_MAX_CELLS / width) {
            JS_ThrowRangeError(ctx,
                "DamerauLevenshtein: %zu x %zu exceeds %llu cells; pass "
                "{ max } to bound the distance, or shorten the input",
                n, m, (unsigned long long)DYN_LEV_MAX_CELLS);
            goto done;
        }
        rows = (size_t*)malloc(width * 3 * sizeof(size_t));
        if (!rows) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (pat->ascii && txt->ascii) {
            dist = dyn_osa_dp_u8(txt->u8, n, pat->u8, m, band,
                rows, rows + width, rows + 2 * width, width);
        } else {
            if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
                goto done;
            dist = dyn_osa_dp_cp(txt->cp, n, pat->cp, m, band,
                rows, rows + width, rows + 2 * width, width);
        }
    }
    if (maxv >= 0 && dist > (size_t)maxv)
        dist = (size_t)maxv + 1;
    ret = JS_NewInt64(ctx, (int64_t)dist);
done:
    free(rows);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}
