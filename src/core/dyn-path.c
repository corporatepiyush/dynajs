/*
 * dyn-path -- POSIX path-string algebra, pure C. See dyn-path.h.
 *
 * The segment normaliser is an index-for-index port of Node's internal
 * normalizeString() (lib/path.js) specialised to '/'. Everything else is built
 * on top of it. The behaviour this file must reproduce byte for byte is pinned
 * by a >35,000-case differential against Node's real path.posix, exhaustive
 * over short strings from the alphabet {a, b, ., /} plus the edge cases.
 *
 * The shape here differs from the binding this was extracted from in exactly
 * one way, and it is the whole point of the extraction: nothing allocates and
 * nothing returns a string object. Every entry point writes into a caller
 * buffer sized by the matching *_cap and returns a length. The lexical splits
 * go further and return offsets into the *input*, because dirname, basename and
 * extname are all substrings -- which is what lets a Path handle compute them
 * once at construction and slice thereafter.
 */
#include "dyn-path.h"

#include <stdint.h>
#include <string.h>

/* ====================================================================
 * The segment normaliser.
 *
 * Scans `path` once, collapses "." segments and repeated slashes, and
 * backtracks over ".." against whatever has already been emitted -- unless
 * `allow_above_root`, in which case a ".." that cannot erase anything is kept
 * literally (a path that must stay relative). resolve() always passes 0,
 * because its result is absolute and excess ".." above the root is dropped.
 *
 * Writes into `res`, which must not alias `path`. Capacity >= path_len always
 * suffices: every emitted byte is either copied verbatim from a segment of
 * `path` (segments are disjoint and monotonically advancing) or is a literal
 * ".." emitted only when the input itself just contributed a ".." segment of at
 * least 2 bytes. Output length can therefore never exceed input length.
 *
 * Returns the number of bytes written.
 * ==================================================================== */
static size_t path_normalize_core(const char *path, size_t path_len,
                                  int allow_above_root, char *res)
{
    size_t res_len = 0;
    long last_segment_length = 0;
    long last_slash = -1;
    int dots = 0;
    int code = 0;
    long i;

    for (i = 0; i <= (long)path_len; i++) {
        if (i < (long)path_len)
            code = (unsigned char)path[i];
        else if (code == '/')
            break; /* trailing separator already handled at the prior i */
        else
            code = '/'; /* flush the final pending segment */

        if (code == '/') {
            if (last_slash == i - 1 || dots == 1) {
                /* "//" run, or a "." segment: nothing to emit */
            } else if (last_slash != i - 1 && dots == 2) {
                /* ".." segment: try to erase the previously emitted one */
                if (res_len < 2 || last_segment_length != 2 ||
                    res[res_len - 1] != '.' || res[res_len - 2] != '.') {
                    if (res_len > 2) {
                        long k, last_slash_index = -1;
                        for (k = (long)res_len - 1; k >= 0; k--) {
                            if (res[k] == '/') {
                                last_slash_index = k;
                                break;
                            }
                        }
                        if (last_slash_index != (long)res_len - 1) {
                            if (last_slash_index == -1) {
                                res_len = 0;
                                last_segment_length = 0;
                            } else {
                                long k2, new_last_slash = -1;
                                res_len = (size_t)last_slash_index;
                                for (k2 = (long)res_len - 1; k2 >= 0; k2--) {
                                    if (res[k2] == '/') {
                                        new_last_slash = k2;
                                        break;
                                    }
                                }
                                last_segment_length =
                                    (long)res_len - 1 - new_last_slash;
                            }
                            last_slash = i;
                            dots = 0;
                            continue;
                        }
                    } else if (res_len == 2 || res_len == 1) {
                        res_len = 0;
                        last_segment_length = 0;
                        last_slash = i;
                        dots = 0;
                        continue;
                    }
                }
                if (allow_above_root) {
                    if (res_len > 0)
                        res[res_len++] = '/';
                    res[res_len++] = '.';
                    res[res_len++] = '.';
                    last_segment_length = 2;
                }
            } else {
                size_t seg_start = (size_t)(last_slash + 1);
                size_t seg_len = (size_t)(i - (last_slash + 1));
                if (res_len > 0)
                    res[res_len++] = '/';
                memcpy(res + res_len, path + seg_start, seg_len);
                res_len += seg_len;
                last_segment_length = (long)seg_len;
            }
            last_slash = i;
            dots = 0;
        } else if (code == '.' && dots != -1) {
            dots++;
        } else {
            dots = -1;
        }
    }
    return res_len;
}

/* ---- normalize / clean -------------------------------------------------- */

/* Is `p` ALREADY normalised, so that normalize(p) == p?
 *
 * A BOSCC (CLAUDE.md sec.4): a cheap summary test that bypasses expensive
 * work, where the bypass is usually taken. Paths in real programs are almost
 * always already clean -- "/var/log/app.log", or anything built by .join() --
 * and the normaliser is a per-byte state machine tracking dots, slashes and
 * backtracking, measured at ~1.6 ns/byte. This test is a SWAR scan at roughly
 * a tenth of that, and a clean path then needs only a memcpy.
 *
 * normalize(p) == p exactly when p contains no "//" and no "/." anywhere (that
 * one pair covers "/./", "/../", and a trailing "/." or "/.."), and does not
 * begin with a "." segment ("." , ".." , "./" , "../"). A trailing slash is
 * PRESERVED by normalize, so "a/b/" is clean. A dotfile is clean too --
 * ".bashrc" has a leading dot but is not a "." SEGMENT, which is why the
 * leading check looks at what follows the dot.
 *
 * The SWAR half only skips 8-byte spans containing NO slash; a span with one
 * is byte-checked. That is deliberate -- the pair test needs the byte after
 * each slash, and paths are slash-dense enough that a cleverer vector form
 * would not pay for the complexity. */
static int dyn_path_is_clean(const char *p, size_t n)
{
    size_t i = 0;

    if (n == 0)
        return 0;                       /* "" normalises to "." */
    if (p[0] == '.') {
        if (n == 1)                     /* "."  */
            return 0;
        if (p[1] == '/')                /* "./" */
            return 0;
        if (p[1] == '.' && (n == 2 || p[2] == '/'))   /* ".." or "../" */
            return 0;
    }

    while (i + 8 <= n) {
        uint64_t w, v;
        memcpy(&w, p + i, 8);
        /* zero byte in v <=> that byte was '/' (0x2F) */
        v = w ^ 0x2F2F2F2F2F2F2F2FULL;
        if (!((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL)) {
            i += 8;                     /* no slash in these 8 bytes */
            continue;
        }
        {
            size_t e = i + 8;
            for (; i < e; i++)
                if (p[i] == '/' && i + 1 < n && (p[i + 1] == '/' || p[i + 1] == '.'))
                    return 0;
        }
    }
    for (; i < n; i++)
        if (p[i] == '/' && i + 1 < n && (p[i + 1] == '/' || p[i + 1] == '.'))
            return 0;
    return 1;
}

size_t dyn_path_normalize(const char *p, size_t n, char *out)
{
    int is_abs, trailing_sep;
    size_t core_len, total;

    if (n == 0) {
        out[0] = '.';
        return 1;
    }
    if (dyn_path_is_clean(p, n)) {
        memcpy(out, p, n);
        return n;
    }

    is_abs = (p[0] == '/');
    trailing_sep = (p[n - 1] == '/');

    /* The leading '/' is reserved BEFORE normalising, so the core writes
     * straight into its final position and there is no shift afterwards. */
    core_len = path_normalize_core(p, n, !is_abs, out + (is_abs ? 1 : 0));

    if (core_len == 0) {
        if (is_abs) {
            out[0] = '/';
            return 1;
        }
        out[0] = '.';
        if (trailing_sep) {
            out[1] = '/';
            return 2;
        }
        return 1;
    }

    total = core_len;
    if (is_abs) {
        out[0] = '/';
        total++;
    }
    if (trailing_sep)
        out[total++] = '/';
    return total;
}

/* ---- join --------------------------------------------------------------- */

size_t dyn_path_join(const char *const *parts, const size_t *lens, size_t count,
                     char *out, char *scratch)
{
    size_t i, o = 0;
    int first = 1;

    for (i = 0; i < count; i++) {
        if (lens[i] == 0)
            continue;
        if (!first)
            scratch[o++] = '/';
        memcpy(scratch + o, parts[i], lens[i]);
        o += lens[i];
        first = 0;
    }

    if (o == 0) { /* zero parts, or every part empty */
        out[0] = '.';
        return 1;
    }
    return dyn_path_normalize(scratch, o, out);
}

/* ---- resolve ------------------------------------------------------------ */

size_t dyn_path_resolve(const char *const *parts, const size_t *lens,
                        size_t count, char *out, char *scratch)
{
    size_t i, o, core_len;
    long root_idx;
    int first;

    /* The rightmost non-empty part that starts with '/'; -1 means "none, fall
     * back to the notional cwd". */
    root_idx = -1;
    for (i = count; i > 0; i--) {
        if (lens[i - 1] > 0 && parts[i - 1][0] == '/') {
            root_idx = (long)(i - 1);
            break;
        }
    }

    o = 0;
    first = 1;
    if (root_idx < 0) {
        scratch[o++] = '/'; /* notional cwd */
        first = 0;
        root_idx = 0;
    }
    for (i = (size_t)root_idx; i < count; i++) {
        if (lens[i] == 0)
            continue;
        if (!first)
            scratch[o++] = '/';
        memcpy(scratch + o, parts[i], lens[i]);
        o += lens[i];
        first = 0;
    }

    /* o >= 1 always: either the cwd byte was written, or root_idx pointed at a
     * part with a non-zero length that the loop just copied. */
    /* allow_above_root=0: the result is absolute, so a ".." that reaches the
     * root is dropped rather than kept. */
    core_len = path_normalize_core(scratch, o, 0, out + 1);
    out[0] = '/';
    return core_len + 1;
}

/* ---- relative ----------------------------------------------------------- */

size_t dyn_path_relative(const char *from, size_t from_n, const char *to,
                         size_t to_n, char *out, char *scratch)
{
    const char *one[1];
    size_t len1[1];
    char *from_r, *to_r, *resolve_scratch;
    size_t from_rlen, to_rlen, from_l, to_l, smallest, i, o, rc_from, rc_to;
    long last_common_sep;

    rc_from = dyn_path_resolve_cap(from_n, 1);
    rc_to = dyn_path_resolve_cap(to_n, 1);

    /* scratch layout: [resolved from][resolved to][the resolver's own room].
     * dyn_path_relative_cap covers all three -- see the header. */
    from_r = scratch;
    to_r = scratch + rc_from;
    resolve_scratch = scratch + rc_from + rc_to;

    one[0] = from;
    len1[0] = from_n;
    from_rlen = dyn_path_resolve(one, len1, 1, from_r, resolve_scratch);
    one[0] = to;
    len1[0] = to_n;
    to_rlen = dyn_path_resolve(one, len1, 1, to_r, resolve_scratch);

    if (from_rlen == to_rlen && memcmp(from_r, to_r, from_rlen) == 0)
        return 0;

    from_l = from_rlen - 1; /* length after the shared leading '/' */
    to_l = to_rlen - 1;
    smallest = from_l < to_l ? from_l : to_l;
    last_common_sep = -1;

    for (i = 0; i < smallest; i++) {
        char fc = from_r[1 + i];
        if (fc != to_r[1 + i])
            break;
        if (fc == '/')
            last_common_sep = (long)i;
    }

    if (i == smallest) {
        if (to_l > smallest) {
            if (to_r[1 + i] == '/') {
                size_t sl = to_rlen - (1 + i + 1);
                memcpy(out, to_r + 1 + i + 1, sl);
                return sl;
            }
            if (i == 0) {
                size_t sl = to_rlen - (1 + i);
                memcpy(out, to_r + 1 + i, sl);
                return sl;
            }
        } else if (from_l > smallest) {
            if (from_r[1 + i] == '/')
                last_common_sep = (long)i;
            else if (i == 0)
                last_common_sep = 0;
        }
    }

    o = 0;
    for (i = (size_t)(1 + last_common_sep + 1); i <= from_rlen; i++) {
        if (i == from_rlen || from_r[i] == '/') {
            if (o == 0) {
                out[o++] = '.';
                out[o++] = '.';
            } else {
                out[o++] = '/';
                out[o++] = '.';
                out[o++] = '.';
            }
        }
    }
    {
        size_t to_start = (size_t)(1 + last_common_sep);
        size_t suffix_len = to_rlen - to_start;
        memcpy(out + o, to_r + to_start, suffix_len);
        o += suffix_len;
    }
    return o;
}

/* ---- the lexical splits ------------------------------------------------- */

/* dirname, as a slice of p. Node's rule, purely lexical. */
static void path_dirname_slice(const char *p, size_t n, dyn_path_split_t *s)
{
    long end, i;
    int has_root, matched_slash;

    s->dir_is_dot = 0;
    s->dir_is_root = 0;
    s->dir_off = 0;
    s->dir_len = 0;

    if (n == 0) {
        s->dir_is_dot = 1;
        return;
    }

    has_root = (p[0] == '/');
    end = -1;
    matched_slash = 1;
    for (i = (long)n - 1; i >= 1; i--) {
        if (p[i] == '/') {
            if (!matched_slash) {
                end = i;
                break;
            }
        } else {
            matched_slash = 0;
        }
    }

    if (end == -1) {
        if (has_root) {
            s->dir_is_root = 1;
            s->dir_len = 1; /* p[0..1) is "/" -- a slice, but flagged too */
        } else {
            s->dir_is_dot = 1;
        }
        return;
    }
    /* POSIX keeps exactly two leading slashes; p[0..2) is "//" when this
     * fires, so it is still a slice of the input. */
    s->dir_len = (has_root && end == 1) ? 2 : (size_t)end;
}

/* basename with no suffix, as a slice of p. */
static void path_basename_slice(const char *p, size_t n, size_t *off,
                                size_t *len)
{
    long start = 0, end = -1, i;
    int matched_slash = 1;

    for (i = (long)n - 1; i >= 0; i--) {
        if (p[i] == '/') {
            if (!matched_slash) {
                start = i + 1;
                break;
            }
        } else if (end == -1) {
            matched_slash = 0;
            end = i + 1;
        }
    }
    if (end == -1) {
        *off = 0;
        *len = 0;
    } else {
        *off = (size_t)start;
        *len = (size_t)(end - start);
    }
}

/* extname, as a slice of p: the last '.' of the final segment. A leading dot
 * makes a dotfile, not an extension. */
static void path_extname_slice(const char *p, size_t n, size_t *off,
                               size_t *len)
{
    long start_dot = -1, start_part = 0, end = -1, i;
    int matched_slash = 1, pre_dot_state = 0;

    for (i = (long)n - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)p[i];
        if (c == '/') {
            if (!matched_slash) {
                start_part = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matched_slash = 0;
            end = i + 1;
        }
        if (c == '.') {
            if (start_dot == -1)
                start_dot = i;
            else if (pre_dot_state != 1)
                pre_dot_state = 1;
        } else if (start_dot != -1) {
            pre_dot_state = -1;
        }
    }

    if (start_dot == -1 || end == -1 || pre_dot_state == 0 ||
        (pre_dot_state == 1 && start_dot == end - 1 &&
         start_dot == start_part + 1)) {
        *off = 0;
        *len = 0;
    } else {
        *off = (size_t)start_dot;
        *len = (size_t)(end - start_dot);
    }
}

void dyn_path_split(const char *p, size_t n, dyn_path_split_t *s)
{
    path_dirname_slice(p, n, s);
    path_basename_slice(p, n, &s->base_off, &s->base_len);
    path_extname_slice(p, n, &s->ext_off, &s->ext_len);
    s->is_absolute = dyn_path_is_absolute(p, n);
}

void dyn_path_basename(const char *p, size_t n, const char *suffix,
                       size_t suffix_n, size_t *off, size_t *len)
{
    long ext_idx, first_non_slash_end, start, end, i;
    int matched_slash;

    if (!suffix || suffix_n == 0 || suffix_n > n) {
        path_basename_slice(p, n, off, len);
        return;
    }
    if (suffix_n == n && memcmp(p, suffix, n) == 0) {
        /* basename(p, p) -- the one case that really is empty. */
        *off = 0;
        *len = 0;
        return;
    }

    ext_idx = (long)suffix_n - 1;
    first_non_slash_end = -1;
    matched_slash = 1;
    start = 0;
    end = -1;

    for (i = (long)n - 1; i >= 0; i--) {
        unsigned char c = (unsigned char)p[i];
        if (c == '/') {
            if (!matched_slash) {
                start = i + 1;
                break;
            }
        } else {
            if (first_non_slash_end == -1) {
                matched_slash = 0;
                first_non_slash_end = i + 1;
            }
            if (ext_idx >= 0) {
                if (c == (unsigned char)suffix[ext_idx]) {
                    if (--ext_idx == -1)
                        end = i;
                } else {
                    ext_idx = -1;
                    end = first_non_slash_end;
                }
            }
        }
    }

    /* Stripping the suffix would empty the final segment: keep the segment. */
    if (start == end)
        end = first_non_slash_end;
    else if (end == -1)
        end = (long)n;

    if (end > start) {
        *off = (size_t)start;
        *len = (size_t)(end - start);
    } else {
        *off = 0;
        *len = 0;
    }
}
