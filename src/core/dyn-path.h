/*
 * dyn-path -- POSIX path-string algebra. PURE C: no JSValue, no JSContext.
 * Compiles with -Isrc/core alone (see tools/core-purity.sh).
 *
 * '/' is always the separator. This is the POSIX flavour only; there is no
 * win32 variant. Semantics are Node's `path.posix`, verified by a >35,000-case
 * differential against the real thing.
 *
 * ALLOCATION-FREE, like dyn-codec. Every function writes into a caller-supplied
 * buffer and returns the number of bytes written. Each has a matching *_cap
 * helper giving an upper bound on that length, so a caller sizes once and
 * writes once -- there is no two-pass "ask, then fill" protocol and no
 * truncation: passing a buffer smaller than the cap is a caller error, not a
 * runtime condition to be reported.
 *
 * Nothing here NUL-terminates. A length is returned; the caller decides whether
 * a terminator is wanted. (dyn_path_cstr_cap accounts for one if it is.)
 *
 * The notional cwd is "/": these functions never call the OS, so resolve() has
 * no real working directory to fall back on. dyn_path_resolve() therefore
 * always returns an absolute path and silently absorbs excess ".." above the
 * root, exactly like Node's does above its own cwd.
 */
#ifndef DYN_PATH_H
#define DYN_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_PATH_SEP       '/'
#define DYN_PATH_DELIM     ':'

/* ---- output sizing ------------------------------------------------------
 *
 * The normaliser never emits more bytes than it consumes: every emitted byte is
 * either copied verbatim from a segment of the input (segments are disjoint,
 * monotonically-advancing substrings) or is a literal ".." emitted only when
 * the input itself just contributed a ".." segment of at least 2 bytes. So the
 * caps below are all "input length plus a fixed, small amount for a leading
 * separator, a preserved trailing separator, or the notional-cwd byte". */

/* normalize(p) / clean(p). +2 for a leading '/' and a preserved trailing '/'. */
static inline size_t dyn_path_normalize_cap(size_t n) { return n + 2; }

/* join(parts). `sum` is the total length of the parts, `count` how many there
 * are: one separator between each, then the normalise cap on the result. */
static inline size_t dyn_path_join_cap(size_t sum, size_t count)
{
    return sum + (count ? count - 1 : 0) + 2;
}

/* resolve(parts). +1 for the notional-cwd byte on top of join's shape. */
static inline size_t dyn_path_resolve_cap(size_t sum, size_t count)
{
    return dyn_path_join_cap(sum, count) + 1;
}

/* relative(from, to): every "up" hop is a "/.." consuming a segment of the
 * resolved `from`, and the tail is a verbatim slice of the resolved `to`. Both
 * sides are resolved first, so both caps feed in -- and the scratch buffer has
 * to hold BOTH resolved sides plus the resolver's own working room, which is
 * where the factor of two comes from. One cap covers `out` and `scratch` alike
 * so a caller cannot pair them wrongly. */
static inline size_t dyn_path_relative_cap(size_t from_n, size_t to_n)
{
    return 2 * (from_n + to_n) + 16;
}

/* Add a NUL to any of the above when the caller wants a C string. */
static inline size_t dyn_path_cstr_cap(size_t n) { return n + 1; }

/* ---- the whole surface --------------------------------------------------
 *
 * Every function takes (pointer, length) pairs and writes `out`, returning the
 * byte count. `out` may not alias any input.
 */

/* normalize/clean: collapse "//" runs, resolve "." and "..", PRESERVE a
 * trailing slash if the input had one -- Node's rule, chosen over the
 * always-strip alternative, and both spellings of this function follow it.
 * "" -> ".". Needs dyn_path_normalize_cap(n). */
size_t dyn_path_normalize(const char *p, size_t n, char *out);

/* join: empty parts are skipped, the rest are '/'-joined, the result is
 * normalised. Zero parts, or all-empty parts, -> ".".
 * `out` and `scratch` each need dyn_path_join_cap(sum of lens, count); the raw
 * concatenation is built in `scratch` and normalised into `out`. Two distinct
 * buffers rather than an in-place pass: the normaliser's output does trail its
 * input, so in-place is *provably* safe, but "provably safe given an invariant
 * about index ordering" is exactly the kind of correctness that rots under a
 * later edit. The second buffer costs one allocation the caller was making
 * anyway. */
size_t dyn_path_join(const char *const *parts, const size_t *lens, size_t count,
                     char *out, char *scratch);

/* resolve: processes parts RIGHT TO LEFT, stopping at the rightmost part that
 * starts with '/'; falls back to the notional cwd "/". Always absolute.
 * `out` and `scratch` each need dyn_path_resolve_cap(sum of lens, count). */
size_t dyn_path_resolve(const char *const *parts, const size_t *lens,
                        size_t count, char *out, char *scratch);

/* relative(from, to): both sides are resolved against the same notional cwd,
 * then compared segment by segment. `out` and `scratch` each need
 * dyn_path_relative_cap(from_n, to_n) -- scratch holds both resolved sides plus
 * the resolver's own working room. Returns 0 for two paths that resolve to the
 * same place. */
size_t dyn_path_relative(const char *from, size_t from_n, const char *to,
                         size_t to_n, char *out, char *scratch);

/* ---- the lexical splits -------------------------------------------------
 *
 * dirname/basename/extname operate lexically -- no "."/".." resolution, exactly
 * like Node: dirname("a/..") is "a", not ".". They return a slice of the INPUT
 * rather than writing a buffer, because every one of them is a substring: the
 * out-params are an offset and a length into `p`. That is what lets a `Path`
 * handle cache three offsets at construction and make .extname a slice instead
 * of a scan.
 *
 * dirname is the one exception: for a path with exactly two leading slashes it
 * must report "//", which is not a substring of the input in the general case
 * -- it is, however, always p[0..2) when it fires, and the flag says so.
 */

/* The three splits of one path, computed in a single pass. Any out-param may
 * be NULL. Offsets are into `p`; a zero length means the empty string. */
typedef struct {
    size_t dir_off,  dir_len;    /* dirname  */
    size_t base_off, base_len;   /* basename, no suffix stripped */
    size_t ext_off,  ext_len;    /* extname, including the leading '.' */
    int    dir_is_dot;           /* dirname is "." (not a slice of p) */
    int    dir_is_root;          /* dirname is "/" (not a slice of p) */
    int    is_absolute;
} dyn_path_split_t;

/* Fill `s` from p[0..n). Never fails. */
void dyn_path_split(const char *p, size_t n, dyn_path_split_t *s);

/* basename(p, suffix): the suffix form is not a plain slice of the basename --
 * it strips a literal trailing `suffix` UNLESS doing so would empty the final
 * segment (basename("/foo/.html", ".html") is ".html", not ""). Only
 * basename(p, p) itself returns "". Reports the slice via the out-params.
 * Pass suffix=NULL/suffix_n=0 for the plain form. */
void dyn_path_basename(const char *p, size_t n, const char *suffix,
                       size_t suffix_n, size_t *off, size_t *len);

/* isAbsolute. */
static inline int dyn_path_is_absolute(const char *p, size_t n)
{
    return n > 0 && p[0] == DYN_PATH_SEP;
}

#ifdef __cplusplus
}
#endif

#endif /* DYN_PATH_H */
