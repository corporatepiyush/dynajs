#include "dyn-frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_LENGTH = 1, FRAME_DELIM, FRAME_FIXED };

#define FRAME_NONE ((size_t)-1)
#define FRAME_STALL 1

struct dyn_frame {
    int kind;
    int err;
    uint8_t* buf;
    size_t len;
    size_t cap;
    uint64_t copied;
    uint64_t max_frame;
    uint64_t offset;
    uint64_t end_off;
    uint64_t strip;
    int64_t adjustment;
    int width;
    int little_endian;
    int have_len;
    uint64_t frame_len;
    int fail_fast;
    int stop_on_error;
    int resume_err;
    int reject_unsafe;
    int hold_opt;
    int line;
    int discarding;
    uint64_t to_discard;
    uint64_t too_long;
    int stalled;
    int held;
    int final;
    size_t nd;
    size_t kmax;
    int strip_delimiter;
    uint8_t** dl;
    size_t* dlen;
    size_t* next;
    uint64_t fixed;
    dyn_frame_sink sink;
    void* ud;
};

static dyn_frame_t* frame_alloc(int kind)
{
    dyn_frame_t* d = (dyn_frame_t*)calloc(1, sizeof(*d));
    if (d)
        d->kind = kind;
    return d;
}

static int frame_fail(dyn_frame_t* d, int e)
{
    d->err = e;
    free(d->buf);
    d->buf = NULL;
    d->len = 0;
    d->cap = 0;
    return e;
}

static int frame_append(dyn_frame_t* d, const uint8_t* p, size_t n)
{
    if (n == 0)
        return 0;
    if (n > SIZE_MAX - d->len)
        return -1;
    if (d->len + n > d->cap) {
        size_t nc = d->cap ? d->cap : 256;
        uint8_t* nb;
        while (nc < d->len + n) {
            if (nc > SIZE_MAX / 2) {
                nc = d->len + n;
                break;
            }
            nc *= 2;
        }
        nb = (uint8_t*)realloc(d->buf, nc);
        if (!nb)
            return -1;
        d->buf = nb;
        d->cap = nc;
    }
    memcpy(d->buf + d->len, p, n);
    d->len += n;
    d->copied += n;
    return 0;
}

static void frame_emit(dyn_frame_t* d, const uint8_t* p, uint64_t n)
{
    d->sink(d->ud, DYN_FRAME_OK, p, n);
}

static int frame_raise(dyn_frame_t* d, int code, uint64_t v, int thrown)
{
    d->sink(d->ud, code, NULL, v);
    if (d->stop_on_error || code == DYN_FRAME_ENOMEM)
        return frame_fail(d, code);
    if (thrown && !d->resume_err) {
        d->stalled = 1;
        return FRAME_STALL;
    }
    return DYN_FRAME_OK;
}

static int frame_oom(dyn_frame_t* d)
{
    return frame_raise(d, DYN_FRAME_ENOMEM, 0, 1);
}

static int frame_rest(dyn_frame_t* d, int r, const uint8_t* p, size_t n)
{
    if (r < 0)
        return r;
    if (frame_append(d, p, n) < 0)
        return frame_oom(d);
    return DYN_FRAME_OK;
}

static uint64_t frame_read_raw(const dyn_frame_t* d, const uint8_t* h)
{
    uint64_t v = 0;
    int i;
    const uint8_t* p = h + d->offset;
    if (d->little_endian) {
        for (i = d->width - 1; i >= 0; i--)
            v = (v << 8) | p[i];
    } else {
        for (i = 0; i < d->width; i++)
            v = (v << 8) | p[i];
    }
    return v;
}

static int frame_eval(const dyn_frame_t* d, const uint8_t* h, uint64_t* out, uint64_t* v)
{
    uint64_t raw = frame_read_raw(d, h), fl;
    if (d->reject_unsafe && raw > DYN_FRAME_MAX_LENGTH) {
        *v = raw;
        return DYN_FRAME_ERANGE;
    }
    if (raw >> 63) {
        *v = raw;
        return DYN_FRAME_ENEG;
    }
    fl = raw + (uint64_t)d->adjustment + d->end_off;
    if ((fl >> 63) || fl < d->end_off) {
        *v = fl;
        return DYN_FRAME_ESHORT;
    }
    if (fl > d->max_frame) {
        *v = fl;
        return DYN_FRAME_ETOOLONG;
    }
    *out = fl;
    return DYN_FRAME_OK;
}

dyn_frame_t* dyn_frame_new_length(const dyn_frame_length_cfg* cfg, int* err)
{
    dyn_frame_t* d;
    int e = DYN_FRAME_EINVAL;
    if (!cfg || (cfg->width != 1 && cfg->width != 2 && cfg->width != 3 && cfg->width != 4 && cfg->width != 8)
        || cfg->max_frame == 0 || cfg->max_frame > DYN_FRAME_MAX_LENGTH
        || cfg->offset > cfg->max_frame || (uint64_t)cfg->width > cfg->max_frame - cfg->offset
        || cfg->adjustment > (int64_t)DYN_FRAME_MAX_LENGTH || cfg->adjustment < -(int64_t)DYN_FRAME_MAX_LENGTH
        || cfg->strip > DYN_FRAME_MAX_LENGTH)
        goto fail;
    e = DYN_FRAME_ENOMEM;
    d = frame_alloc(FRAME_LENGTH);
    if (!d)
        goto fail;
    d->offset = cfg->offset;
    d->width = cfg->width;
    d->little_endian = cfg->little_endian ? 1 : 0;
    d->adjustment = cfg->adjustment;
    d->strip = cfg->strip;
    d->max_frame = cfg->max_frame;
    d->end_off = cfg->offset + (uint64_t)cfg->width;
    d->fail_fast = cfg->fail_fast ? 1 : 0;
    d->stop_on_error = cfg->stop_on_error ? 1 : 0;
    d->resume_err = cfg->resume_after_error ? 1 : 0;
    d->reject_unsafe = cfg->reject_unsafe ? 1 : 0;
    if (err)
        *err = DYN_FRAME_OK;
    return d;
fail:
    if (err)
        *err = e;
    return NULL;
}

static int delim_is(const dyn_frame_t* d, size_t k, const char* s)
{
    size_t n = strlen(s);
    return d->dlen[k] == n && memcmp(d->dl[k], s, n) == 0;
}

dyn_frame_t* dyn_frame_new_delim(const dyn_frame_delim_cfg* cfg, int* err)
{
    dyn_frame_t* d = NULL;
    size_t i;
    int e = DYN_FRAME_EINVAL;
    if (!cfg || cfg->count == 0 || cfg->count > DYN_FRAME_MAX_DELIMS || !cfg->delims || !cfg->lens
        || cfg->max_frame == 0 || cfg->max_frame > DYN_FRAME_MAX_LENGTH)
        goto fail;
    for (i = 0; i < cfg->count; i++)
        if (!cfg->delims[i] || cfg->lens[i] == 0 || cfg->lens[i] > DYN_FRAME_MAX_DELIM_LEN)
            goto fail;
    e = DYN_FRAME_ENOMEM;
    d = frame_alloc(FRAME_DELIM);
    if (!d)
        goto fail;
    d->dl = (uint8_t**)calloc(cfg->count, sizeof(*d->dl));
    d->dlen = (size_t*)calloc(cfg->count, sizeof(*d->dlen));
    d->next = (size_t*)calloc(cfg->count, sizeof(*d->next));
    if (!d->dl || !d->dlen || !d->next)
        goto fail;
    d->nd = cfg->count;
    for (i = 0; i < cfg->count; i++) {
        d->dl[i] = (uint8_t*)malloc(cfg->lens[i]);
        if (!d->dl[i])
            goto fail;
        memcpy(d->dl[i], cfg->delims[i], cfg->lens[i]);
        d->dlen[i] = cfg->lens[i];
        if (cfg->lens[i] > d->kmax)
            d->kmax = cfg->lens[i];
    }
    d->strip_delimiter = cfg->strip_delimiter ? 1 : 0;
    d->max_frame = cfg->max_frame;
    d->fail_fast = cfg->fail_fast ? 1 : 0;
    d->stop_on_error = cfg->stop_on_error ? 1 : 0;
    d->resume_err = cfg->resume_after_error ? 1 : 0;
    d->hold_opt = cfg->hold_overlapping ? 1 : 0;
    d->line = !d->hold_opt && d->nd == 2
        && ((delim_is(d, 0, "\r\n") && delim_is(d, 1, "\n")) || (delim_is(d, 0, "\n") && delim_is(d, 1, "\r\n")));
    if (err)
        *err = DYN_FRAME_OK;
    return d;
fail:
    dyn_frame_free(d);
    if (err)
        *err = e;
    return NULL;
}

dyn_frame_t* dyn_frame_new_fixed(uint64_t frame_len, int* err)
{
    dyn_frame_t* d;
    if (frame_len == 0 || frame_len > DYN_FRAME_MAX_LENGTH || frame_len > (uint64_t)SIZE_MAX) {
        if (err)
            *err = DYN_FRAME_EINVAL;
        return NULL;
    }
    d = frame_alloc(FRAME_FIXED);
    if (!d) {
        if (err)
            *err = DYN_FRAME_ENOMEM;
        return NULL;
    }
    d->fixed = frame_len;
    d->max_frame = frame_len;
    if (err)
        *err = DYN_FRAME_OK;
    return d;
}

static int feed_fixed(dyn_frame_t* d, const uint8_t* data, size_t n)
{
    size_t pos = 0, f = (size_t)d->fixed;
    if (d->len > 0) {
        size_t take = f - d->len;
        if (take > n)
            take = n;
        if (frame_append(d, data, take) < 0)
            return frame_oom(d);
        pos = take;
        if (d->len < f)
            return DYN_FRAME_OK;
        frame_emit(d, d->buf, f);
        d->len = 0;
    }
    while (n - pos >= f) {
        frame_emit(d, data + pos, f);
        pos += f;
    }
    if (frame_append(d, data + pos, n - pos) < 0)
        return frame_oom(d);
    return DYN_FRAME_OK;
}

static int length_fin(dyn_frame_t* d, int first)
{
    if (d->to_discard == 0) {
        uint64_t t = d->too_long;
        d->too_long = 0;
        d->discarding = 0;
        if (!d->fail_fast || first)
            return frame_raise(d, DYN_FRAME_ETOOLONG, t, 1);
        return DYN_FRAME_OK;
    }
    if ((d->fail_fast || d->stop_on_error) && first)
        return frame_raise(d, DYN_FRAME_ETOOLONG, d->too_long, 1);
    return DYN_FRAME_OK;
}

static int length_exceeded(dyn_frame_t* d, uint64_t fl, uint64_t readable, uint64_t* skip)
{
    d->too_long = fl;
    if (fl < readable) {
        d->to_discard = 0;
        *skip = fl;
    } else {
        d->discarding = 1;
        d->to_discard = fl - readable;
        *skip = readable;
    }
    return length_fin(d, 1);
}

static int feed_length(dyn_frame_t* d, const uint8_t* data, size_t n)
{
    size_t pos = 0, eo = (size_t)d->end_off;
    uint64_t fl = 0, v = 0, skip = 0;
    int e, r;
    if (d->len > 0) {
        size_t take;
        if (d->len < eo) {
            take = eo - d->len;
            if (take > n)
                take = n;
            if (frame_append(d, data, take) < 0)
                return frame_oom(d);
            pos = take;
            if (d->len < eo)
                return DYN_FRAME_OK;
        }
        if (!d->have_len) {
            e = frame_eval(d, d->buf, &fl, &v);
            if (e == DYN_FRAME_ETOOLONG) {
                size_t had = d->len;
                r = length_exceeded(d, v, (uint64_t)had + (n - pos), &skip);
                if (r < 0)
                    return r;
                d->len = 0;
                pos += (size_t)(skip - had);
                if (r == FRAME_STALL)
                    return frame_rest(d, 0, data + pos, n - pos);
                goto scan;
            }
            if (e) {
                d->len = 0;
                r = frame_raise(d, e, v, 1);
                if (r < 0)
                    return r;
                if (r == FRAME_STALL)
                    return frame_rest(d, 0, data + pos, n - pos);
                goto scan;
            }
            d->have_len = 1;
            d->frame_len = fl;
        }
        fl = d->frame_len;
        take = (size_t)(fl - d->len);
        if (take > n - pos)
            take = n - pos;
        if (frame_append(d, data + pos, take) < 0)
            return frame_oom(d);
        pos += take;
        if (d->len < fl)
            return DYN_FRAME_OK;
        d->have_len = 0;
        if (d->strip > fl) {
            d->len = 0;
            r = frame_raise(d, DYN_FRAME_ESTRIP, fl, 1);
            if (r < 0)
                return r;
            if (r == FRAME_STALL)
                return frame_rest(d, 0, data + pos, n - pos);
            goto scan;
        }
        frame_emit(d, d->buf + d->strip, fl - d->strip);
        d->len = 0;
    }
scan:
    while (pos < n) {
        if (d->discarding) {
            uint64_t k = d->to_discard < (uint64_t)(n - pos) ? d->to_discard : (uint64_t)(n - pos);
            pos += (size_t)k;
            d->to_discard -= k;
            r = length_fin(d, 0);
            if (r < 0)
                return r;
            if (r == FRAME_STALL)
                return frame_rest(d, 0, data + pos, n - pos);
            continue;
        }
        if (n - pos < eo)
            break;
        e = frame_eval(d, data + pos, &fl, &v);
        if (e == DYN_FRAME_ETOOLONG) {
            r = length_exceeded(d, v, n - pos, &skip);
            if (r < 0)
                return r;
            pos += (size_t)skip;
            if (r == FRAME_STALL)
                return frame_rest(d, 0, data + pos, n - pos);
            continue;
        }
        if (e) {
            pos += eo;
            r = frame_raise(d, e, v, 1);
            if (r < 0)
                return r;
            if (r == FRAME_STALL)
                return frame_rest(d, 0, data + pos, n - pos);
            continue;
        }
        if ((uint64_t)(n - pos) < fl)
            break;
        if (d->strip > fl) {
            pos += (size_t)fl;
            r = frame_raise(d, DYN_FRAME_ESTRIP, fl, 1);
            if (r < 0)
                return r;
            if (r == FRAME_STALL)
                return frame_rest(d, 0, data + pos, n - pos);
            continue;
        }
        frame_emit(d, data + pos + d->strip, fl - d->strip);
        pos += (size_t)fl;
    }
    if (frame_append(d, data + pos, n - pos) < 0)
        return frame_oom(d);
    return DYN_FRAME_OK;
}

static size_t delim_find(const dyn_frame_t* d, size_t k, const uint8_t* p, size_t n, size_t from)
{
    const uint8_t* dk = d->dl[k];
    size_t L = d->dlen[k];
    while (from < n && n - from >= L) {
        const uint8_t* hit = (const uint8_t*)memchr(p + from, dk[0], n - from - L + 1);
        size_t i;
        if (!hit)
            return FRAME_NONE;
        i = (size_t)(hit - p);
        if (memcmp(p + i, dk, L) == 0)
            return i;
        from = i + 1;
    }
    return FRAME_NONE;
}

static size_t delim_find_joined(const dyn_frame_t* d, size_t k, const uint8_t* data, size_t n)
{
    const uint8_t* dk = d->dl[k];
    size_t L = d->dlen[k], B = d->len, i, j, r;
    if (d->held) {
        r = delim_find(d, k, d->buf, B, 0);
        if (r != FRAME_NONE)
            return r;
    }
    i = B >= L - 1 ? B - (L - 1) : 0;
    for (; i < B; i++) {
        for (j = 0; j < L; j++) {
            size_t at = i + j;
            uint8_t c;
            if (at < B)
                c = d->buf[at];
            else if (at - B < n)
                c = data[at - B];
            else
                break;
            if (c != dk[j])
                break;
        }
        if (j == L)
            return i;
    }
    r = delim_find(d, k, data, n, 0);
    return r == FRAME_NONE ? FRAME_NONE : r + B;
}

static int delim_held(const dyn_frame_t* d, const uint8_t* p1, size_t n1, const uint8_t* p2, size_t n2, size_t best, size_t bk)
{
    size_t A = n1 + n2, j, s, q;
    if (d->final)
        return 0;
    for (j = 0; j < d->nd; j++) {
        size_t L = d->dlen[j];
        s = A >= L - 1 ? A - (L - 1) : 0;
        for (; s <= best && s < A; s++) {
            if (s == best && j >= bk)
                break;
            if ((uint64_t)s > d->max_frame)
                break;
            for (q = s; q < A; q++) {
                uint8_t c = q < n1 ? p1[q] : p2[q - n1];
                if (c != d->dl[j][q - s])
                    break;
            }
            if (q == A)
                return 1;
        }
    }
    return 0;
}

static size_t delim_partial(const dyn_frame_t* d, const uint8_t* p1, size_t n1, const uint8_t* p2, size_t n2)
{
    size_t A = n1 + n2, best = 0, k, L, q;
    for (k = 0; k < d->nd; k++) {
        size_t top = d->dlen[k] - 1;
        if (top > A)
            top = A;
        for (L = top; L > best; L--) {
            for (q = 0; q < L; q++) {
                size_t at = A - L + q;
                uint8_t c = at < n1 ? p1[at] : p2[at - n1];
                if (c != d->dl[k][q])
                    break;
            }
            if (q == L) {
                best = L;
                break;
            }
        }
    }
    return best;
}

static int delim_keep_tail(dyn_frame_t* d, const uint8_t* p2, size_t n2, size_t keep)
{
    d->held = 0;
    if (keep <= n2) {
        d->len = 0;
        return frame_append(d, p2 + n2 - keep, keep);
    }
    keep -= n2;
    memmove(d->buf, d->buf + d->len - keep, keep);
    d->len = keep;
    return frame_append(d, p2, n2);
}

static int delim_none(dyn_frame_t* d, const uint8_t* p2, size_t n2)
{
    size_t R = d->len + n2, P = d->line ? 0 : delim_partial(d, d->buf, d->len, p2, n2), fb = R - P;
    if (!d->discarding) {
        if ((uint64_t)fb > d->max_frame) {
            d->too_long = fb;
            d->discarding = 1;
            if (delim_keep_tail(d, p2, n2, P) < 0)
                return frame_oom(d);
            if (d->fail_fast || d->stop_on_error)
                return frame_raise(d, d->line ? DYN_FRAME_EOVER : DYN_FRAME_ETOOLONG, fb, !d->line);
            return DYN_FRAME_OK;
        }
        d->held = 0;
        if (frame_append(d, p2, n2) < 0)
            return frame_oom(d);
        return DYN_FRAME_OK;
    }
    d->too_long += fb;
    if (delim_keep_tail(d, p2, n2, P) < 0)
        return frame_oom(d);
    return DYN_FRAME_OK;
}

static int delim_drop(dyn_frame_t* d, size_t flen, int* r)
{
    if (d->discarding) {
        uint64_t t = d->line ? d->too_long + flen : d->too_long;
        d->discarding = 0;
        d->too_long = 0;
        *r = d->fail_fast ? DYN_FRAME_OK : frame_raise(d, DYN_FRAME_ETOOLONG, t, !d->line);
        return 1;
    }
    if ((uint64_t)flen > d->max_frame) {
        *r = frame_raise(d, DYN_FRAME_ETOOLONG, flen, !d->line);
        return 1;
    }
    *r = DYN_FRAME_OK;
    return 0;
}

static int feed_delim(dyn_frame_t* d, const uint8_t* data, size_t n)
{
    size_t pos = 0, k, best, bk = 0;
    int r;
    while (d->len > 0) {
        size_t B = d->len, end, take, rest = n - pos;
        best = FRAME_NONE;
        for (k = 0; k < d->nd; k++) {
            size_t at = delim_find_joined(d, k, data + pos, rest);
            if (at < best) {
                best = at;
                bk = k;
            }
        }
        if (best == FRAME_NONE) {
            r = delim_none(d, data + pos, rest);
            return r < 0 ? r : DYN_FRAME_OK;
        }
        if (d->hold_opt && delim_held(d, d->buf, B, data + pos, rest, best, bk)) {
            if (rest == 0) {
                d->held = 1;
                return DYN_FRAME_OK;
            }
            take = rest < d->kmax ? rest : d->kmax;
            if (frame_append(d, data + pos, take) < 0)
                return frame_oom(d);
            pos += take;
            d->held = 1;
            continue;
        }
        end = best + d->dlen[bk];
        take = end > B ? end - B : 0;
        if (!delim_drop(d, best, &r)) {
            if (frame_append(d, data + pos, take) < 0)
                return frame_oom(d);
            frame_emit(d, d->buf, d->strip_delimiter ? best : end);
        } else if (r < 0) {
            return r;
        }
        pos += take;
        if (end < B) {
            memmove(d->buf, d->buf + end, B - end);
            d->len = B - end;
            d->held = 1;
        } else {
            d->len = 0;
            d->held = 0;
        }
        if (r == FRAME_STALL)
            return frame_rest(d, 0, data + pos, n - pos);
    }
    d->held = 0;
    for (k = 0; k < d->nd; k++)
        d->next[k] = FRAME_NONE - 1;
    while (pos < n) {
        size_t flen, dlk;
        best = FRAME_NONE;
        for (k = 0; k < d->nd; k++) {
            if (d->next[k] == FRAME_NONE - 1 || (d->next[k] != FRAME_NONE && d->next[k] < pos))
                d->next[k] = delim_find(d, k, data, n, pos);
            if (d->next[k] < best) {
                best = d->next[k];
                bk = k;
            }
        }
        if (best == FRAME_NONE) {
            r = delim_none(d, data + pos, n - pos);
            return r < 0 ? r : DYN_FRAME_OK;
        }
        if (d->hold_opt && delim_held(d, data + pos, n - pos, NULL, 0, best - pos, bk)) {
            d->held = 1;
            break;
        }
        flen = best - pos;
        dlk = d->dlen[bk];
        if (!delim_drop(d, flen, &r))
            frame_emit(d, data + pos, d->strip_delimiter ? flen : flen + dlk);
        else if (r < 0)
            return r;
        pos = best + dlk;
        if (r == FRAME_STALL)
            return frame_rest(d, 0, data + pos, n - pos);
    }
    if (frame_append(d, data + pos, n - pos) < 0)
        return frame_oom(d);
    return DYN_FRAME_OK;
}

static int feed_kind(dyn_frame_t* d, const uint8_t* data, size_t n)
{
    switch (d->kind) {
    case FRAME_LENGTH:
        return feed_length(d, data, n);
    case FRAME_DELIM:
        return feed_delim(d, data, n);
    case FRAME_FIXED:
        return feed_fixed(d, data, n);
    default:
        return DYN_FRAME_EINVAL;
    }
}

static int frame_resume(dyn_frame_t* d, const uint8_t* data, size_t n)
{
    uint8_t* tmp;
    size_t tn;
    uint64_t copied;
    int r = DYN_FRAME_OK;
    if (frame_append(d, data, n) < 0)
        return frame_oom(d);
    copied = d->copied;
    tmp = d->buf;
    tn = d->len;
    d->buf = NULL;
    d->len = 0;
    d->cap = 0;
    d->held = 0;
    d->stalled = 0;
    if (tn > 0)
        r = feed_kind(d, tmp, tn);
    if (!d->err)
        d->copied = copied;
    free(tmp);
    return r;
}

int dyn_frame_feed(dyn_frame_t* d, const uint8_t* data, size_t n, dyn_frame_sink sink, void* ud)
{
    if (!d)
        return DYN_FRAME_EINVAL;
    if (d->err)
        return d->err;
    if (n == 0)
        return DYN_FRAME_OK;
    if (!data || !sink)
        return DYN_FRAME_EINVAL;
    d->sink = sink;
    d->ud = ud;
    if (d->stalled)
        return frame_resume(d, data, n);
    return feed_kind(d, data, n);
}

int dyn_frame_finish(dyn_frame_t* d, dyn_frame_sink sink, void* ud)
{
    int r;
    if (!d || !sink)
        return DYN_FRAME_EINVAL;
    if (d->err || d->final)
        return d->err;
    d->final = 1;
    d->sink = sink;
    d->ud = ud;
    if (!d->stalled && !(d->hold_opt && d->held))
        return DYN_FRAME_OK;
    r = frame_resume(d, NULL, 0);
    if (r == DYN_FRAME_OK && d->stalled) {
        d->stalled = 0;
        d->len = 0;
    }
    return r;
}

int dyn_frame_error(const dyn_frame_t* d)
{
    return d ? d->err : DYN_FRAME_EINVAL;
}

int dyn_frame_line_mode(const dyn_frame_t* d)
{
    return d ? d->line : 0;
}

int dyn_frame_stalled(const dyn_frame_t* d)
{
    return d ? d->stalled : 0;
}

size_t dyn_frame_buffered(const dyn_frame_t* d)
{
    return d ? d->len : 0;
}

uint64_t dyn_frame_copied(const dyn_frame_t* d)
{
    return d ? d->copied : 0;
}

const char* dyn_frame_strerror(int err)
{
    switch (err) {
    case DYN_FRAME_OK:
        return "ok";
    case DYN_FRAME_ETOOLONG:
        return "frame exceeds maxFrameLength";
    case DYN_FRAME_ERANGE:
        return "length field exceeds 2^53";
    case DYN_FRAME_ESHORT:
        return "adjusted frame length is less than the length field end offset";
    case DYN_FRAME_ESTRIP:
        return "initialBytesToStrip exceeds the frame length";
    case DYN_FRAME_ENOMEM:
        return "out of memory";
    case DYN_FRAME_EINVAL:
        return "invalid decoder configuration or argument";
    case DYN_FRAME_ENEG:
        return "negative pre-adjustment length field";
    case DYN_FRAME_EOVER:
        return "frame exceeds maxFrameLength (no delimiter yet)";
    default:
        return "unknown frame error";
    }
}

int dyn_frame_describe(const dyn_frame_t* d, int ev, uint64_t v, char* out, size_t cap)
{
    unsigned long long max = d ? (unsigned long long)d->max_frame : 0, uv = (unsigned long long)v;
    long long sv = (long long)(int64_t)v;
    if (!out || cap == 0)
        return -1;
    if (!d)
        return snprintf(out, cap, "%s", dyn_frame_strerror(ev));
    switch (ev) {
    case DYN_FRAME_ETOOLONG:
        if (d->kind == FRAME_DELIM && d->line)
            return snprintf(out, cap, "frame length (%llu) exceeds the allowed maximum (%llu)", uv, max);
        if (v == 0)
            return snprintf(out, cap, "%s exceeds %llu - discarding", d->kind == FRAME_LENGTH ? "Adjusted frame length" : "frame length", max);
        return snprintf(out, cap, "%s exceeds %llu: %llu - discarded", d->kind == FRAME_LENGTH ? "Adjusted frame length" : "frame length", max, uv);
    case DYN_FRAME_EOVER:
        return snprintf(out, cap, "frame length (over %llu) exceeds the allowed maximum (%llu)", uv, max);
    case DYN_FRAME_ENEG:
        return snprintf(out, cap, "negative pre-adjustment length field: %lld", sv);
    case DYN_FRAME_ESHORT:
        return snprintf(out, cap, "Adjusted frame length (%lld) is less than lengthFieldEndOffset: %llu", sv, (unsigned long long)d->end_off);
    case DYN_FRAME_ESTRIP:
        return snprintf(out, cap, "Adjusted frame length (%llu) is less than initialBytesToStrip: %llu", uv, (unsigned long long)d->strip);
    case DYN_FRAME_ERANGE:
        return snprintf(out, cap, "length field %llu exceeds 2^53 (rejectUnsafeLength)", uv);
    default:
        return snprintf(out, cap, "%s", dyn_frame_strerror(ev));
    }
}

void dyn_frame_free(dyn_frame_t* d)
{
    size_t i;
    if (!d)
        return;
    if (d->dl)
        for (i = 0; i < d->nd; i++)
            free(d->dl[i]);
    free(d->dl);
    free(d->dlen);
    free(d->next);
    free(d->buf);
    free(d);
}
