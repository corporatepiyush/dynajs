#ifndef DYN_TIMEK_TIMEK_DP_H
#define DYN_TIMEK_TIMEK_DP_H

#include "timek.h"
#include "../dyn-ctype.h"

static inline int dyn_dp_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline void dyn_dp_skip_space(const char* s, size_t len, size_t* pos)
{
    while (*pos < len && dyn_in_set64((unsigned char)s[*pos], DYN_WS_DATE))
        (*pos)++;
}

static inline int dyn_dp_digits(const char* s, size_t len, size_t* pos, int max,
    int64_t* out)
{
    int n = 0;
    int64_t v = 0;
    while (*pos < len && n < max && s[*pos] >= '0' && s[*pos] <= '9') {
        v = v * 10 + (s[*pos] - '0');
        (*pos)++;
        n++;
    }
    *out = v;
    return n;
}

static inline __attribute__((always_inline)) size_t dyn_dp_word(const char* s, size_t len, size_t pos,
    const char* word)
{
    size_t k = 0;
    while (word[k]) {
        if (pos + k >= len || dyn_dp_lower((unsigned char)s[pos + k]) != word[k])
            return 0;
        k++;
    }
    if (pos + k < len) {
        int c = dyn_dp_lower((unsigned char)s[pos + k]);
        if (c >= 'a' && c <= 'z')
            return 0;
    }
    return k;
}

static inline int dyn_dp_month(const DynDateParser* p, const char* s, size_t len,
    size_t* pos)
{
    int i;
    char c0;
    if (*pos >= len)
        return 0;
    c0 = (char)dyn_dp_lower((unsigned char)s[*pos]);
    for (i = 0; i < 12; i++) {
        size_t k;
        if (p->loc->months[i][0] != c0)
            continue;
        k = dyn_dp_word(s, len, *pos, p->loc->months[i]);
        if (k) {
            *pos += k;
            return i + 1;
        }
    }
    for (i = 0; i < 12; i++) {
        size_t k;
        if (p->loc->months_abbr[i][0] != c0)
            continue;
        k = dyn_dp_word(s, len, *pos, p->loc->months_abbr[i]);
        if (k) {
            *pos += k;
            return i + 1;
        }
    }
    return 0;
}

static inline int dyn_dp_weekday(const DynDateParser* p, const char* s, size_t len,
    size_t* pos)
{
    int i;
    char c0;
    if (*pos >= len)
        return -1;
    c0 = (char)dyn_dp_lower((unsigned char)s[*pos]);
    for (i = 0; i < 7; i++) {
        size_t k;
        if (p->loc->days[i][0] != c0)
            continue;
        k = dyn_dp_word(s, len, *pos, p->loc->days[i]);
        if (k) {
            *pos += k;
            return i;
        }
        {
            const char* d = p->loc->days[i];
            if (*pos + 3 <= len && dyn_dp_lower((unsigned char)s[*pos]) == d[0] && dyn_dp_lower((unsigned char)s[*pos + 1]) == d[1] && dyn_dp_lower((unsigned char)s[*pos + 2]) == d[2]) {
                int nx = (*pos + 3 < len)
                    ? dyn_dp_lower((unsigned char)s[*pos + 3])
                    : 0;
                if (!(nx >= 'a' && nx <= 'z')) {
                    *pos += 3;
                    return i;
                }
            }
        }
    }
    return -1;
}

static inline int64_t dyn_dp_expand_year(int64_t y, int digits)
{
    if (digits > 2)
        return y;
    return (y <= 69) ? 2000 + y : 1900 + y;
}

static inline int dyn_dp_time(const char* s, size_t len, size_t* pos,
    int64_t* h, int64_t* mi, int64_t* se)
{
    size_t p = *pos;
    int64_t v;
    int n;

    n = dyn_dp_digits(s, len, &p, 2, &v);
    if (n == 0 || p >= len || s[p] != ':')
        return 0;
    *h = v;
    p++;
    if (dyn_dp_digits(s, len, &p, 2, &v) == 0)
        return 0;
    *mi = v;
    *se = 0;
    if (p < len && s[p] == ':') {
        size_t q = p + 1;
        if (dyn_dp_digits(s, len, &q, 2, &v)) {
            *se = v;
            p = q;
        }
    }
    while (p < len && s[p] == ' ')
        p++;
    if (dyn_dp_word(s, len, p, "pm")) {
        if (*h < 12)
            *h += 12;
        p += 2;
    } else if (dyn_dp_word(s, len, p, "am")) {
        if (*h == 12)
            *h = 0;
        p += 2;
    }
    if (*h > 23 || *mi > 59 || *se > 60)
        return 0;
    *pos = p;
    return 1;
}

static inline int dyn_dp_valid_ymd(int64_t y, int64_t mo, int64_t d)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int max;
    if (mo < 1 || mo > 12 || d < 1)
        return 0;
    max = mdays[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        max = 29;
    return d <= max;
}

static inline __attribute__((always_inline)) int dyn_dp_unit(const char* s, size_t len, size_t* pos, int64_t* secs,
    int* months)
{
    static const struct {
        const char* w;
        int64_t s;
        int mo;
    } units[] = {
        { "seconds", 1, 0 },
        { "second", 1, 0 },
        { "secs", 1, 0 },
        { "sec", 1, 0 },
        { "minutes", 60, 0 },
        { "minute", 60, 0 },
        { "mins", 60, 0 },
        { "min", 60, 0 },
        { "hours", 3600, 0 },
        { "hour", 3600, 0 },
        { "hrs", 3600, 0 },
        { "hr", 3600, 0 },
        { "days", 86400, 0 },
        { "day", 86400, 0 },
        { "weeks", 604800, 0 },
        { "week", 604800, 0 },
        { "months", 0, 1 },
        { "month", 0, 1 },
        { "years", 0, 12 },
        { "year", 0, 12 },
    };
    size_t i;
    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        size_t k = dyn_dp_word(s, len, *pos, units[i].w);
        if (k) {
            *pos += k;
            *secs = units[i].s;
            *months = units[i].mo;
            return 1;
        }
    }
    return 0;
}

static inline int64_t dyn_dp_add_months(int64_t t, int64_t n)
{
    int64_t days = t / DYN_SECS_PER_DAY, tod = t % DYN_SECS_PER_DAY, y;
    int mo, d;
    if (tod < 0) {
        tod += DYN_SECS_PER_DAY;
        days--;
    }
    dyn_civil_from_days(days, &y, &mo, &d);
    {
        int64_t total = (int64_t)mo - 1 + n;
        int64_t ny = y + (total >= 0 ? total / 12 : -((-total + 11) / 12));
        int nm = (int)(total - (ny - y) * 12) + 1;
        while (!dyn_dp_valid_ymd(ny, nm, d))
            d--;
        return dyn_days_from_civil(ny, nm, d) * DYN_SECS_PER_DAY + tod;
    }
}

static inline int64_t dyn_dp_midnight(int64_t t)
{
    int64_t r = t % DYN_SECS_PER_DAY;
    if (r < 0)
        r += DYN_SECS_PER_DAY;
    return t - r;
}

static inline int dyn_dp_parse(const DynDateParser* p, const char* s, size_t len,
    int64_t now, int64_t* out)
{
    size_t pos = 0;
    int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, v;
    int n, have_date = 0, have_time = 0, wd;

    dyn_dp_skip_space(s, len, &pos);
    if (pos >= len)
        return -1;

    if (dyn_dp_word(s, len, pos, "now")) {
        *out = now;
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "today")) {
        *out = dyn_dp_midnight(now);
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "tomorrow")) {
        *out = dyn_dp_midnight(now) + DYN_SECS_PER_DAY;
        return 0;
    }
    if (dyn_dp_word(s, len, pos, "yesterday")) {
        *out = dyn_dp_midnight(now) - DYN_SECS_PER_DAY;
        return 0;
    }
    {
        size_t q = pos;
        int forward = 1, months = 0;
        int64_t unit = 0;
        size_t k = dyn_dp_word(s, len, q, "in");
        if (k) {
            q += k;
            dyn_dp_skip_space(s, len, &q);
        }
        n = dyn_dp_digits(s, len, &q, 9, &v);
        if (n > 0) {
            size_t after = q;
            dyn_dp_skip_space(s, len, &after);
            if (dyn_dp_unit(s, len, &after, &unit, &months)) {
                size_t tail = after;
                dyn_dp_skip_space(s, len, &tail);
                if (dyn_dp_word(s, len, tail, "ago")) {
                    forward = 0;
                    tail += 3;
                } else if (!k) {
                    goto not_relative;
                }
                dyn_dp_skip_space(s, len, &tail);
                if (tail != len)
                    goto not_relative;
                *out = months
                    ? dyn_dp_add_months(now, forward ? v * months : -(v * months))
                    : now + (forward ? v * unit : -(v * unit));
                return 0;
            }
        }
    }
not_relative:
    {
        size_t q = pos;
        int dir = 0;
        size_t k = dyn_dp_word(s, len, q, "next");
        if (k) {
            dir = 1;
            q += k;
        } else if ((k = dyn_dp_word(s, len, q, "last")) != 0) {
            dir = -1;
            q += k;
        }
        if (dir) {
            dyn_dp_skip_space(s, len, &q);
            wd = dyn_dp_weekday(p, s, len, &q);
            dyn_dp_skip_space(s, len, &q);
            if (wd >= 0 && q == len) {
                int64_t day0 = dyn_dp_midnight(now);
                int cur = (int)(((day0 / DYN_SECS_PER_DAY) % 7 + 7 + 4) % 7);
                int delta = dir > 0 ? (wd - cur + 7) % 7 : -(((cur - wd) + 7) % 7);
                if (delta == 0)
                    delta = dir > 0 ? 7 : -7;
                *out = day0 + (int64_t)delta * DYN_SECS_PER_DAY;
                return 0;
            }
        }
    }

    {
        size_t q = pos;
        if (dyn_dp_weekday(p, s, len, &q) >= 0) {
            dyn_dp_skip_space(s, len, &q);
            if (q < len)
                pos = q;
        }
    }
    dyn_dp_skip_space(s, len, &pos);

    if ((mo = dyn_dp_month(p, s, len, &pos)) > 0) {
        dyn_dp_skip_space(s, len, &pos);
        if (dyn_dp_digits(s, len, &pos, 2, &d) == 0)
            return -1;
        dyn_dp_skip_space(s, len, &pos);
        n = dyn_dp_digits(s, len, &pos, 4, &y);
        if (n == 0)
            return -1;
        y = dyn_dp_expand_year(y, n);
        have_date = 1;
    } else {
        n = dyn_dp_digits(s, len, &pos, 4, &v);
        if (n == 0)
            return -1;
        if (n == 4 && pos < len && (s[pos] == '-' || s[pos] == '/')) {
            char sep = s[pos];
            y = v;
            pos++;
            if (dyn_dp_digits(s, len, &pos, 2, &mo) == 0)
                return -1;
            if (pos >= len || s[pos] != sep)
                return -1;
            pos++;
            if (dyn_dp_digits(s, len, &pos, 2, &d) == 0)
                return -1;
            have_date = 1;
        } else {
            size_t q = pos;
            dyn_dp_skip_space(s, len, &q);
            {
                size_t mq = q;
                int named = dyn_dp_month(p, s, len, &mq);
                if (named > 0) {
                    d = v;
                    mo = named;
                    pos = mq;
                    dyn_dp_skip_space(s, len, &pos);
                    n = dyn_dp_digits(s, len, &pos, 4, &y);
                    if (n == 0)
                        return -1;
                    y = dyn_dp_expand_year(y, n);
                    have_date = 1;
                }
            }
            if (!have_date) {
                int64_t b;
                if (pos >= len || (s[pos] != '/' && s[pos] != '-' && s[pos] != '.'))
                    return -1;
                pos++;
                if (dyn_dp_digits(s, len, &pos, 2, &b) == 0)
                    return -1;
                if (pos >= len || (s[pos] != '/' && s[pos] != '-' && s[pos] != '.'))
                    return -1;
                pos++;
                n = dyn_dp_digits(s, len, &pos, 4, &y);
                if (n == 0)
                    return -1;
                y = dyn_dp_expand_year(y, n);
                if (p->loc->day_first) {
                    d = v;
                    mo = b;
                } else {
                    mo = v;
                    d = b;
                }
                have_date = 1;
            }
        }
    }
    if (!have_date)
        return -1;
    if (!dyn_dp_valid_ymd(y, mo, d))
        return -1;

    dyn_dp_skip_space(s, len, &pos);
    if (pos < len && (s[pos] == 'T' || s[pos] == 't'))
        pos++;
    if (pos < len)
        have_time = dyn_dp_time(s, len, &pos, &h, &mi, &se);
    if (pos < len && !have_time)
        return -1;
    dyn_dp_skip_space(s, len, &pos);
    if (pos != len)
        return -1;

    *out = dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se;
    return 0;
}

#endif
