#include "dyn-path.h"

#include <stdint.h>
#include <string.h>

static size_t path_normalize_core(const char* path, size_t path_len,
    int allow_above_root, char* res)
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
            break;
        else
            code = '/';

        if (code == '/') {
            if (last_slash == i - 1 || dots == 1) {
            } else if (last_slash != i - 1 && dots == 2) {
                if (res_len < 2 || last_segment_length != 2 || res[res_len - 1] != '.' || res[res_len - 2] != '.') {
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
                                last_segment_length = (long)res_len - 1 - new_last_slash;
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

static int dyn_path_is_clean(const char* p, size_t n)
{
    size_t i = 0;

    if (n == 0)
        return 0;
    if (p[0] == '.') {
        if (n == 1)
            return 0;
        if (p[1] == '/')
            return 0;
        if (p[1] == '.' && (n == 2 || p[2] == '/'))
            return 0;
    }

    while (i + 8 <= n) {
        uint64_t w, v;
        memcpy(&w, p + i, 8);
        v = w ^ 0x2F2F2F2F2F2F2F2FULL;
        if (!((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL)) {
            i += 8;
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

size_t dyn_path_normalize(const char* p, size_t n, char* out)
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

size_t dyn_path_join(const char* const* parts, const size_t* lens, size_t count,
    char* out, char* scratch)
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

    if (o == 0) {
        out[0] = '.';
        return 1;
    }
    return dyn_path_normalize(scratch, o, out);
}

size_t dyn_path_resolve(const char* const* parts, const size_t* lens,
    size_t count, char* out, char* scratch)
{
    size_t i, o, core_len;
    long root_idx;
    int first;

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
        scratch[o++] = '/';
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

    core_len = path_normalize_core(scratch, o, 0, out + 1);
    out[0] = '/';
    return core_len + 1;
}

size_t dyn_path_relative(const char* from, size_t from_n, const char* to,
    size_t to_n, char* out, char* scratch)
{
    const char* one[1];
    size_t len1[1];
    char *from_r, *to_r, *resolve_scratch;
    size_t from_rlen, to_rlen, from_l, to_l, smallest, i, o, rc_from, rc_to;
    long last_common_sep;

    rc_from = dyn_path_resolve_cap(from_n, 1);
    rc_to = dyn_path_resolve_cap(to_n, 1);

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

    from_l = from_rlen - 1;
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

static void path_dirname_slice(const char* p, size_t n, dyn_path_split_t* s)
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
            s->dir_len = 1;
        } else {
            s->dir_is_dot = 1;
        }
        return;
    }
    s->dir_len = (has_root && end == 1) ? 2 : (size_t)end;
}

static void path_basename_slice(const char* p, size_t n, size_t* off,
    size_t* len)
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

static void path_extname_slice(const char* p, size_t n, size_t* off,
    size_t* len)
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

    if (start_dot == -1 || end == -1 || pre_dot_state == 0 || (pre_dot_state == 1 && start_dot == end - 1 && start_dot == start_part + 1)) {
        *off = 0;
        *len = 0;
    } else {
        *off = (size_t)start_dot;
        *len = (size_t)(end - start_dot);
    }
}

void dyn_path_split(const char* p, size_t n, dyn_path_split_t* s)
{
    path_dirname_slice(p, n, s);
    path_basename_slice(p, n, &s->base_off, &s->base_len);
    path_extname_slice(p, n, &s->ext_off, &s->ext_len);
    s->is_absolute = dyn_path_is_absolute(p, n);
}

void dyn_path_basename(const char* p, size_t n, const char* suffix,
    size_t suffix_n, size_t* off, size_t* len)
{
    long ext_idx, first_non_slash_end, start, end, i;
    int matched_slash;

    if (!suffix || suffix_n == 0 || suffix_n > n) {
        path_basename_slice(p, n, off, len);
        return;
    }
    if (suffix_n == n && memcmp(p, suffix, n) == 0) {
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
