/* Edit distance and bigram similarity for dyna:matcher; included by
   dyna-matcher.c so the kernels inline into the entry points. They live here,
   not in a revived dyna:text, which was retired into dyna:bytes. */

/* Both sides of a comparison are capped so a quadratic fallback cannot be
 * driven past a bounded amount of work by input alone. */
#define DYN_APPROX_MAX_BYTES (16u * 1024u * 1024u)

/* Above this length the bit-parallel kernel no longer fits one machine word and
 * the two-row DP runs instead; the SHORTER side is always the pattern, so this
 * is a bound on min(|a|,|b|), not on either string. */
#define DYN_LEV_WORD_BITS 64

/* ---------------------------------------------------------------- decoding */

/* Number of code points in `n` UTF-8 bytes, or (size_t)-1 if malformed. Sets
 * *ascii when no byte has the high bit set -- the byte kernels' precondition. */
static size_t dyn_utf8_count(const uint8_t *s, size_t n, int *ascii)
{
    size_t i = 0, cnt = 0;
    int hi = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        if (c < 0x80) { i++; cnt++; continue; }
        hi = 1;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return (size_t)-1;
        if (i + need > n) return (size_t)-1;
        for (size_t k = 1; k < need; k++)
            if ((s[i + k] & 0xC0) != 0x80) return (size_t)-1;
        i += need;
        cnt++;
    }
    *ascii = !hi;
    return cnt;
}

/* Decode `n` UTF-8 bytes into `out` (which must hold dyn_utf8_count entries). */
static void dyn_utf8_decode(const uint8_t *s, size_t n, uint32_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { out[o++] = c; i++; }
        else if ((c & 0xE0) == 0xC0) {
            out[o++] = ((uint32_t)(c & 0x1F) << 6) | (s[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            out[o++] = ((uint32_t)(c & 0x0F) << 12)
                     | ((uint32_t)(s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F);
            i += 3;
        } else {
            out[o++] = ((uint32_t)(c & 0x07) << 18)
                     | ((uint32_t)(s[i+1] & 0x3F) << 12)
                     | ((uint32_t)(s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F);
            i += 4;
        }
    }
}

/* -------------------------------------------------- the Eq mask, two forms */

/* Code points are at most 0x10FFFF, so this cannot collide with a real key. */
#define DYN_EQ_EMPTY 0xFFFFFFFFu
#define DYN_EQ_SLOTS 256           /* >= 4x the 64-entry maximum: probes stay short */

typedef struct {
    uint32_t key[DYN_EQ_SLOTS];
    uint64_t mask[DYN_EQ_SLOTS];
} dyn_eq_t;

static uint32_t dyn_eq_slot(uint32_t c)
{
    c *= 2654435761u;                     /* Knuth multiplicative, then fold */
    return (c ^ (c >> 16)) & (DYN_EQ_SLOTS - 1);
}

static void dyn_eq_build(dyn_eq_t *e, const uint32_t *pat, size_t m)
{
    size_t i;
    for (i = 0; i < DYN_EQ_SLOTS; i++)
        e->key[i] = DYN_EQ_EMPTY;
    for (i = 0; i < m; i++) {
        uint32_t s = dyn_eq_slot(pat[i]);
        while (e->key[s] != DYN_EQ_EMPTY && e->key[s] != pat[i])
            s = (s + 1) & (DYN_EQ_SLOTS - 1);
        if (e->key[s] == DYN_EQ_EMPTY) { e->key[s] = pat[i]; e->mask[s] = 0; }
        e->mask[s] |= (uint64_t)1 << i;
    }
}

static uint64_t dyn_eq_get(const dyn_eq_t *e, uint32_t c)
{
    uint32_t s = dyn_eq_slot(c);
    while (e->key[s] != DYN_EQ_EMPTY) {
        if (e->key[s] == c)
            return e->mask[s];
        s = (s + 1) & (DYN_EQ_SLOTS - 1);
    }
    return 0;
}

/* ------------------------------------------------ Myers 1999, bit-parallel */

/* Global edit distance, Hyyro formulation: one word of state per text element,
   so O(n) words for m <= 64. `score` starts at m and is nudged by the
   horizontal delta bit at position m-1 each step. */
#define DYN_LEV_MYERS_BODY(EQ_OF)                                              \
    uint64_t vp = ~(uint64_t)0, vn = 0;                                        \
    uint64_t top = (uint64_t)1 << (m - 1);                                     \
    size_t score = m, j;                                                       \
    for (j = 0; j < n; j++) {                                                  \
        uint64_t eq = (EQ_OF);                                                 \
        uint64_t xv = eq | vn;                                                 \
        uint64_t xh = ((((eq & vp) + vp) ^ vp) | eq);                          \
        uint64_t ph = vn | ~(xh | vp);                                         \
        uint64_t mh = vp & xh;                                                 \
        if (ph & top) score++;                                                 \
        if (mh & top) score--;                                                 \
        ph = (ph << 1) | 1;                                                    \
        mh = (mh << 1);                                                        \
        vp = mh | ~(xv | ph);                                                  \
        vn = ph & xv;                                                          \
    }                                                                          \
    return score;

/* ASCII: Eq is a flat 128-entry table, one indexed load per text byte. */
static size_t dyn_lev_myers_u8(const uint8_t *pat, size_t m,
                               const uint8_t *txt, size_t n)
{
    uint64_t eqt[128];
    size_t i;
    for (i = 0; i < 128; i++)
        eqt[i] = 0;
    for (i = 0; i < m; i++)
        eqt[pat[i]] |= (uint64_t)1 << i;
    DYN_LEV_MYERS_BODY(eqt[txt[j]])
}

static size_t dyn_lev_myers_cp(const uint32_t *pat, size_t m,
                               const uint32_t *txt, size_t n)
{
    dyn_eq_t e;
    dyn_eq_build(&e, pat, m);
    DYN_LEV_MYERS_BODY(dyn_eq_get(&e, txt[j]))
}

/* ------------------------------------------------------ two-row DP, banded */

/* The fallback when both sides exceed one word. `band` bounds |i-j|: cells
   outside it already exceed the caller's max and are never computed, so a
   bounded query costs O(band * n) rather than O(m * n). */
#define DYN_LEV_DP_BODY                                                        \
    size_t i, j, lo, hi;                                                       \
    for (j = 0; j <= m; j++)                                                   \
        prev[j] = j;                                                           \
    for (i = 1; i <= n; i++) {                                                 \
        lo = (i > band) ? i - band : 1;                                        \
        hi = (i + band < m) ? i + band : m;                                    \
        cur[lo - 1] = (lo == 1) ? i : DYN_LEV_INF;                             \
        for (j = lo; j <= hi; j++) {                                           \
            size_t del = prev[j] + 1;                                          \
            size_t ins = cur[j - 1] + 1;                                       \
            size_t sub = prev[j - 1] + (A[i - 1] != B[j - 1]);                 \
            size_t v = del < ins ? del : ins;                                  \
            cur[j] = v < sub ? v : sub;                                        \
        }                                                                      \
        if (hi < m)                                                            \
            cur[hi + 1] = DYN_LEV_INF;                                         \
        {                                                                      \
            size_t *t = prev; prev = cur; cur = t;                             \
        }                                                                      \
    }                                                                          \
    return prev[m];

#define DYN_LEV_INF ((size_t)-1 / 4)

static size_t dyn_lev_dp_u8(const uint8_t *A, size_t n, const uint8_t *B,
                            size_t m, size_t band, size_t *prev, size_t *cur)
{
    DYN_LEV_DP_BODY
}

static size_t dyn_lev_dp_cp(const uint32_t *A, size_t n, const uint32_t *B,
                            size_t m, size_t band, size_t *prev, size_t *cur)
{
    DYN_LEV_DP_BODY
}

/* The obviously-correct full-matrix reference, compiled only for the
   differential oracle (CONFIG_APPROX_REFERENCE=y). It shares no code with the
   shipped kernels, which is the only thing that makes the diff mean anything. */
#ifdef DYN_APPROX_REFERENCE
static size_t dyn_lev_reference(const uint32_t *A, size_t n, const uint32_t *B,
                                size_t m, size_t *scratch)
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

/* ------------------------------------------------------------ entry points */

/* One decoded operand: ASCII borrows the UTF-8 bytes, anything else owns a
 * code-point array. `cs` is freed by the caller either way. */
typedef struct {
    const char *cs;         /* the JS_ToCStringLen result (always owned) */
    const uint8_t *u8;      /* set iff ascii */
    uint32_t *cp;           /* set iff !ascii (malloc'd) */
    size_t n;               /* length in code points */
    size_t bytes;           /* UTF-8 length; strlen() would stop at a U+0000 */
    int ascii;
} dyn_operand_t;

static void dyn_operand_free(JSContext *ctx, dyn_operand_t *o)
{
    if (o->cs)
        JS_FreeCString(ctx, o->cs);
    free(o->cp);
    o->cs = NULL;
    o->cp = NULL;
}

/* Coerce argv[i] to C storage. Coercion runs first and completely, before any
 * native state is touched (dyna-nat.h's rule). */
static int dyn_operand_get(JSContext *ctx, JSValueConst v, const char *what,
                           dyn_operand_t *o)
{
    size_t blen, cnt;
    int ascii = 1;

    o->cs = NULL; o->u8 = NULL; o->cp = NULL; o->n = 0; o->bytes = 0;
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
    cnt = dyn_utf8_count((const uint8_t *)o->cs, blen, &ascii);
    if (cnt == (size_t)-1) {
        JS_ThrowTypeError(ctx, "dyna:matcher: %s is not valid UTF-8", what);
        goto fail;
    }
    o->n = cnt;
    o->bytes = blen;
    o->ascii = ascii;
    if (ascii) {
        o->u8 = (const uint8_t *)o->cs;
        return 0;
    }
    o->cp = (uint32_t *)malloc(cnt ? cnt * sizeof(uint32_t) : 1);
    if (!o->cp) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    dyn_utf8_decode((const uint8_t *)o->cs, blen, o->cp);
    return 0;
 fail:
    dyn_operand_free(ctx, o);
    return -1;
}

/* Promote an ASCII operand to code points so a mixed pair shares one kernel. */
static int dyn_operand_widen(JSContext *ctx, dyn_operand_t *o)
{
    size_t i;
    if (!o->ascii)
        return 0;
    o->cp = (uint32_t *)malloc(o->n ? o->n * sizeof(uint32_t) : 1);
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

/* Read options.max into *out (-1 = absent). Coercion can run user JS, so it
   happens before any operand is bound. */
static int dyn_lev_read_max(JSContext *ctx, int argc, JSValueConst *argv,
                            int64_t *out)
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
        JS_ThrowRangeError(ctx, "Levenshtein(a, b, { max }): max must be >= 0");
        return -1;
    }
    return 0;
}

/* Cells the O(n*m) DP may visit without an explicit { max }. 4e8 is ~0.5 s
   here; 100k x 100k would be 1e10 and ~16 s. */
#define DYN_LEV_MAX_CELLS 400000000ull

/* Levenshtein(a, b[, { max }]) -> exact edit distance in CODE POINTS. With
   `max` the answer is exact while <= max and is max + 1 once it exceeds it, so
   `d <= max` is always a correct "within max" test. */
static JSValue dyn_levenshtein(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_operand_t A, B;
    size_t band = (size_t)-1, n, m, dist, diff;
    size_t *row = NULL;
    int64_t maxv = -1;
    JSValue ret = JS_EXCEPTION;
    const dyn_operand_t *pat, *txt;

    A.cs = NULL; A.cp = NULL; B.cs = NULL; B.cp = NULL;
    if (dyn_lev_read_max(ctx, argc, argv, &maxv) < 0)  /* coerce options FIRST */
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Levenshtein(a, b[, options]): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;

    /* The shorter side is the pattern: that is what puts min(|a|,|b|) under the
     * one-word bound rather than whichever argument the caller passed first. */
    if (A.n <= B.n) { pat = &A; txt = &B; } else { pat = &B; txt = &A; }
    m = pat->n; n = txt->n;
    diff = n - m;
    /* Levenshtein is O(n*m) by definition, so the caller controls the CPU cost
       entirely: 100k x 100k is 1e10 cells and measured ~16 s. Refuse rather
       than burn it, and name the option that bounds it -- `max` short-circuits
       on the length difference above and keeps the band narrow. */
    if (maxv < 0 && m && n > DYN_LEV_MAX_CELLS / m) {
        JS_ThrowRangeError(ctx,
            "Levenshtein: %zu x %zu exceeds %llu cells; pass { max } to bound "
            "the distance, or shorten the input",
            n, m, (unsigned long long)DYN_LEV_MAX_CELLS);
        goto done;
    }
    if (maxv >= 0) {
        band = (size_t)maxv;
        if (diff > band) {                 /* length alone already exceeds max */
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
        size_t need = (m + 2) * 2;
        row = (size_t *)malloc(need * sizeof(size_t));
        if (!row) { JS_ThrowOutOfMemory(ctx); goto done; }
        if (band == (size_t)-1)
            band = m;
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

#ifdef DYN_APPROX_REFERENCE
static JSValue dyn_levenshtein_ref(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_operand_t A, B;
    size_t *scratch = NULL;
    JSValue ret = JS_EXCEPTION;

    A.cs = NULL; A.cp = NULL; B.cs = NULL; B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "LevenshteinReference(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;
    scratch = (size_t *)malloc((B.n + 1) * sizeof(size_t));
    if (!scratch) { JS_ThrowOutOfMemory(ctx); goto done; }
    ret = JS_NewInt64(ctx, (int64_t)dyn_lev_reference(A.cp, A.n, B.cp, B.n, scratch));
 done:
    free(scratch);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}
#endif

/* -------------------------------------------------------- Dice coefficient */

/* DiceCoefficient(a, b) -> [0, 1], string-similarity's metric including its two
   surprises, which are contract and not accident: ASCII whitespace is stripped
   first, and a side under two characters scores 0 unless the two are equal. */
static int dyn_is_ascii_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v';
}

/* Drop ASCII whitespace in place; returns the surviving length. */
static size_t dyn_strip_spaces(uint32_t *s, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++)
        if (!dyn_is_ascii_space(s[i]))
            s[k++] = s[i];
    return k;
}

/* Open-addressed multiset of bigrams. Sized to a power of two >= 2n so the
 * load factor stays at 0.5 and probing is short; a linear scan of the left
 * bigrams instead would be O(|a|*|b|), which a 16 MiB pair can weaponise. */
typedef struct {
    uint64_t *key;
    uint32_t *cnt;
    uint8_t  *occ;      /* probe on THIS, not on cnt: a count decremented back
                         * to zero must not truncate a colliding key's chain */
    size_t mask;
} dyn_gram_set_t;

static size_t dyn_gram_slot(const dyn_gram_set_t *s, uint64_t g)
{
    uint64_t h = g * 0x9E3779B97F4A7C15ull;
    return (size_t)((h ^ (h >> 29)) & s->mask);
}

static void dyn_gram_add(dyn_gram_set_t *s, uint64_t g)
{
    size_t i = dyn_gram_slot(s, g);
    while (s->occ[i] && s->key[i] != g)
        i = (i + 1) & s->mask;
    s->key[i] = g;
    s->occ[i] = 1;
    s->cnt[i]++;
}

/* Consume one occurrence of `g`; 1 if the multiset still held one, else 0. */
static int dyn_gram_take(dyn_gram_set_t *s, uint64_t g)
{
    size_t i = dyn_gram_slot(s, g);
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

static JSValue dyn_dice(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    dyn_operand_t A, B;
    dyn_gram_set_t set;
    size_t i, na, nb, an, bn, hits = 0, cap;
    JSValue ret = JS_EXCEPTION;

    set.key = NULL; set.cnt = NULL; set.occ = NULL;

    A.cs = NULL; A.cp = NULL; B.cs = NULL; B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "DiceCoefficient(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
    /* Identical input scores 1 whatever it contains, and byte equality IS code
       point equality for valid UTF-8 -- so answer before decoding anything. */
#ifndef DYN_NO_BYPASS
    if (A.bytes == B.bytes && memcmp(A.cs, B.cs, A.bytes) == 0) {
        ret = JS_NewFloat64(ctx, 1.0);
        goto done;
    }
#endif
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;

    na = dyn_strip_spaces(A.cp, A.n);              /* strip whitespace in place */
    nb = dyn_strip_spaces(B.cp, B.n);

    if (na == nb) {
        for (i = 0; i < na && A.cp[i] == B.cp[i]; i++) ;
        if (i == na) { ret = JS_NewFloat64(ctx, 1.0); goto done; }
    }
    if (na < 2 || nb < 2) { ret = JS_NewFloat64(ctx, 0.0); goto done; }

    an = na - 1;
    bn = nb - 1;
    for (cap = 16; cap < an * 2; cap <<= 1) ;
    set.mask = cap - 1;
    set.key = (uint64_t *)calloc(cap, sizeof(uint64_t));
    set.cnt = (uint32_t *)calloc(cap, sizeof(uint32_t));
    set.occ = (uint8_t *)calloc(cap, 1);
    if (!set.key || !set.cnt || !set.occ) { JS_ThrowOutOfMemory(ctx); goto done; }

    /* Multiset intersection: each left bigram is consumed at most once, which
     * is what makes "aa" vs "aaaa" score below 1. */
    for (i = 0; i < an; i++)
        dyn_gram_add(&set, ((uint64_t)A.cp[i] << 32) | A.cp[i + 1]);
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
