#include "timek.h"
#include "../dyn-ctype.h"

static inline int dyn_rrule_gen_wdof(const DynRRuleGen* g, int i)
{
    return g->wdaymask[DYN_RRULE_PMOD7(i)];
}

int dyn_rrule_set_add(int16_t* a, int* n, int cap, int v)
{
    int i;
    for (i = 0; i < *n; i++) {
        if (a[i] == v)
            return 0;
        if (a[i] > v)
            break;
    }
    if (*n >= cap)
        return -1;
    memmove(a + i + 1, a + i, (size_t)(*n - i) * sizeof(a[0]));
    a[i] = (int16_t)v;
    (*n)++;
    return 0;
}

static uint64_t dyn_rrule_bits(const int16_t* a, int n, uint8_t* ok)
{
    uint64_t b = 0;
    int k;
    for (k = 0; k < n; k++) {
        if (a[k] < -32 || a[k] > 31)
            *ok = 0;
        else
            b |= 1ull << (a[k] + 32);
    }
    return b;
}

static int dyn_rrule_has(uint64_t bits, int v)
{
    unsigned u = (unsigned)(v + 32);
    return u < 64 && ((bits >> u) & 1);
}

static int dyn_rrule_in(const int16_t* a, int n, int v)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (a[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && a[lo] == v;
}

static int dyn_rrule_is_leap(int64_t y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

static int dyn_rrule_iso_wd(int64_t days)
{
    return (dyn_weekday_from_days(days) + 6) % 7;
}

char dyn_rrule_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

int dyn_rrule_parse_datetime(const char* s, size_t len, int64_t* out)
{
    size_t pos = 0;
    int64_t y, mo, d, h = 0, mi = 0, se = 0, off = 0;
    int compact;

    if (len < 8)
        return -1;
    if (dyn_time_expect_digits(s, len, pos, 4, &y))
        return -1;
    pos += 4;
    compact = (s[pos] >= '0' && s[pos] <= '9');
    if (compact) {
        if (dyn_time_expect_digits(s, len, pos, 2, &mo))
            return -1;
        pos += 2;
        if (dyn_time_expect_digits(s, len, pos, 2, &d))
            return -1;
        pos += 2;
    } else {
        if (pos >= len || s[pos] != '-')
            return -1;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &mo))
            return -1;
        pos += 2;
        if (pos >= len || s[pos] != '-')
            return -1;
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &d))
            return -1;
        pos += 2;
    }

    if (pos < len && (s[pos] == 'T' || s[pos] == 't')) {
        pos++;
        if (dyn_time_expect_digits(s, len, pos, 2, &h))
            return -1;
        pos += 2;
        if (pos < len && s[pos] == ':') {
            pos++;
            if (dyn_time_expect_digits(s, len, pos, 2, &mi))
                return -1;
            pos += 2;
            if (pos < len && s[pos] == ':') {
                pos++;
                if (dyn_time_expect_digits(s, len, pos, 2, &se))
                    return -1;
                pos += 2;
            }
        } else {
            if (dyn_time_expect_digits(s, len, pos, 2, &mi))
                return -1;
            pos += 2;
            if (dyn_time_expect_digits(s, len, pos, 2, &se))
                return -1;
            pos += 2;
        }
        if (pos < len && s[pos] == '.') {
            size_t start;
            pos++;
            start = pos;
            while (pos < len && s[pos] >= '0' && s[pos] <= '9')
                pos++;
            if (pos == start)
                return -1;
        }
        if (pos < len && (s[pos] == 'Z' || s[pos] == 'z')) {
            pos++;
        } else if (pos < len && (s[pos] == '+' || s[pos] == '-')) {
            int sign = (s[pos] == '-') ? -1 : 1;
            int64_t oh, om;
            pos++;
            if (dyn_time_expect_digits(s, len, pos, 2, &oh))
                return -1;
            pos += 2;
            if (pos < len && s[pos] == ':') {
                pos++;
                if (dyn_time_expect_digits(s, len, pos, 2, &om))
                    return -1;
                pos += 2;
            } else {
                if (dyn_time_expect_digits(s, len, pos, 2, &om))
                    return -1;
                pos += 2;
            }
            if (oh > 23 || om > 59)
                return -1;
            off = sign * (oh * 3600 + om * 60);
        }
    }
    if (pos != len)
        return -1;

    if (mo < 1 || mo > 12 || d < 1 || d > dyn_time_days_in_month(y, (int)mo))
        return -1;
    if (h > 23 || mi > 59 || se > 60)
        return -1;
    *out = dyn_days_from_civil(y, (int)mo, d) * DYN_SECS_PER_DAY + h * 3600 + mi * 60 + se - off;
    return 0;
}

static void dyn_rrule_sort_bynwd(int16_t* bynwd, int n)
{
    int i;
    for (i = 1; i < n; i++) {
        int16_t w = bynwd[i * 2], nn = bynwd[i * 2 + 1];
        int j = i - 1;
        while (j >= 0 && (bynwd[j * 2] > w || (bynwd[j * 2] == w && bynwd[j * 2 + 1] > nn))) {
            bynwd[(j + 1) * 2] = bynwd[j * 2];
            bynwd[(j + 1) * 2 + 1] = bynwd[j * 2 + 1];
            j--;
        }
        bynwd[(j + 1) * 2] = w;
        bynwd[(j + 1) * 2 + 1] = nn;
    }
}

int dyn_rrule_compile(DynRRule* rr, const DynRRuleSpec* s, int64_t now, int64_t* bad_year)
{
    int64_t ds_sec = s->has_dtstart ? s->dtstart : now;
    int no_by;

    memset(rr, 0, sizeof(*rr));
    rr->freq = (uint8_t)s->freq;
    rr->interval = s->interval;
    rr->wkst = (uint8_t)s->wkst;
    rr->dtstart = ds_sec;
    rr->until = s->until;
    rr->count = s->count;
    rr->has_until = s->has_until;
    rr->has_count = s->has_count;
    rr->orig = s->orig;
    memcpy(rr->bymonth, s->bymonth, sizeof(rr->bymonth));
    memcpy(rr->bymonthday, s->bymonthday, sizeof(rr->bymonthday));
    memcpy(rr->bynmonthday, s->bynmonthday, sizeof(rr->bynmonthday));
    memcpy(rr->byyearday, s->byyearday, sizeof(rr->byyearday));
    memcpy(rr->byweekno, s->byweekno, sizeof(rr->byweekno));
    memcpy(rr->bysetpos, s->bysetpos, sizeof(rr->bysetpos));
    memcpy(rr->byweekday, s->byweekday, sizeof(rr->byweekday));
    rr->n_byweekday = s->n_byweekday;
    if (s->freq > DYN_RRULE_MONTHLY && s->n_bynwd > 0) {
        int k;
        for (k = 0; k < s->n_bynwd; k++) {
            int wd = s->bynwd[k * 2];
            dyn_rrule_set_add(rr->byweekday, &rr->n_byweekday,
                DYN_RRULE_WD_CAP, wd);
        }
        rr->n_bynwd = 0;
    } else {
        memcpy(rr->bynwd, s->bynwd, sizeof(rr->bynwd));
        rr->n_bynwd = s->n_bynwd;
    }
    rr->n_bymonth = s->n_bymonth;
    rr->n_bymonthday = s->n_bymonthday;
    rr->n_bynmonthday = s->n_bynmonthday;
    rr->n_byyearday = s->n_byyearday;
    rr->n_byweekno = s->n_byweekno;
    rr->n_bysetpos = s->n_bysetpos;
    dyn_rrule_sort_bynwd(rr->bynwd, rr->n_bynwd);

    {
        int64_t days = dyn_time_floor_div(ds_sec, DYN_SECS_PER_DAY);
        int64_t tod = dyn_time_floor_mod(ds_sec, DYN_SECS_PER_DAY);
        dyn_civil_from_days(days, &rr->ds_y, &rr->ds_mo, &rr->ds_d);
        rr->ds_h = (int)(tod / 3600);
        rr->ds_mi = (int)((tod / 60) % 60);
        rr->ds_s = (int)(tod % 60);
    }

    if (rr->ds_y < 1 || rr->ds_y > DYN_RRULE_MAX_YEAR) {
        *bad_year = rr->ds_y;
        return DYN_RRULE_E_DTSTART_YEAR;
    }
    if (rr->has_until) {
        int64_t udays = dyn_time_floor_div(rr->until, DYN_SECS_PER_DAY);
        int64_t uy;
        int um, ud;
        dyn_civil_from_days(udays, &uy, &um, &ud);
        if (uy < 1 || uy > DYN_RRULE_MAX_YEAR) {
            *bad_year = uy;
            return DYN_RRULE_E_UNTIL_YEAR;
        }
    }

    no_by = !(rr->orig & (DYN_RR_ORIG_BYMONTHDAY | DYN_RR_ORIG_BYYEARDAY | DYN_RR_ORIG_BYWEEKNO | DYN_RR_ORIG_BYWEEKDAY));
    if (no_by) {
        if (rr->freq == DYN_RRULE_YEARLY) {
            if (!(rr->orig & DYN_RR_ORIG_BYMONTH))
                rr->bymonth[rr->n_bymonth++] = (int16_t)rr->ds_mo;
            rr->bymonthday[rr->n_bymonthday++] = (int16_t)rr->ds_d;
        } else if (rr->freq == DYN_RRULE_MONTHLY) {
            rr->bymonthday[rr->n_bymonthday++] = (int16_t)rr->ds_d;
        } else if (rr->freq == DYN_RRULE_WEEKLY) {
            int64_t days = dyn_time_floor_div(ds_sec, DYN_SECS_PER_DAY);
            rr->byweekday[rr->n_byweekday++] = (int16_t)dyn_rrule_iso_wd(days);
        }
    }

    rr->need_mmask = rr->n_bymonth > 0;
    rr->need_mday = rr->n_bymonthday > 0;
    rr->need_nmday = rr->n_bynmonthday > 0;
    rr->need_wday = rr->freq == DYN_RRULE_WEEKLY || rr->n_byweekday > 0 || rr->n_byweekno > 0 || rr->n_bynwd > 0;
    rr->need_wno = rr->n_byweekno > 0;
    rr->need_nwd = rr->n_bynwd > 0;
    rr->bits_ok = 1;
    rr->bits_month = dyn_rrule_bits(rr->bymonth, rr->n_bymonth, &rr->bits_ok);
    rr->bits_wd = dyn_rrule_bits(rr->byweekday, rr->n_byweekday, &rr->bits_ok);
    rr->bits_mday = dyn_rrule_bits(rr->bymonthday, rr->n_bymonthday, &rr->bits_ok);
    rr->bits_nmday = dyn_rrule_bits(rr->bynmonthday, rr->n_bynmonthday, &rr->bits_ok);

    return 0;
}

const char* dyn_rrule_limit_desc(const DynRRuleGen* g)
{
    switch (g->limit) {
    case DYN_RRULE_LIMIT_WORK:
        return "the 2^28 day/position work cap";
    case DYN_RRULE_LIMIT_YEAR:
        return "the year 9999 range bound";
    default:
        return "1000000 periods";
    }
}

static int dyn_rrule_i64_cmp(const void* a, const void* b)
{
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void dyn_rrule_build_wnomask(DynRRuleGen* g);

static void dyn_rrule_rebuild_year(DynRRuleGen* g)
{
    const DynRRule* rr = g->rr;
    int64_t y = g->year;
    int i;

    g->yearlen = 365 + dyn_rrule_is_leap(y);
    g->nextyearlen = 365 + dyn_rrule_is_leap(y + 1);
    g->yearordinal = dyn_days_from_civil(y, 1, 1);
    g->yearweekday = dyn_rrule_iso_wd(g->yearordinal);

    if (rr->need_wday) {
        int wd = g->yearweekday;
        for (i = 0; i < 385; i++) {
            g->wdaymask[i] = (uint8_t)wd;
            wd = wd == 6 ? 0 : wd + 1;
        }
    }

    for (i = 0; i < 12; i++)
        g->mrange[i] = (int)(dyn_days_from_civil(y, i + 1, 1) - g->yearordinal);
    g->mrange[12] = g->yearlen;

    if (rr->need_mmask || rr->need_mday || rr->need_nmday) {
        int mo, lim = g->yearlen + 7;
        i = 0;
        for (mo = 1; mo <= 13 && i < lim; mo++) {
            int m = mo == 13 ? 1 : mo, d;
            int dim = dyn_time_days_in_month(mo == 13 ? y + 1 : y, m);
            for (d = 1; d <= dim && i < lim; d++, i++) {
                g->mmask[i] = (uint8_t)m;
                g->mdaymask[i] = (int16_t)d;
                g->nmdaymask[i] = (int16_t)(d - dim - 1);
            }
        }
    }
    if (rr->need_wno)
        dyn_rrule_build_wnomask(g);
}

static void dyn_rrule_build_wnomask(DynRRuleGen* g)
{
    const DynRRule* rr = g->rr;
    int wkst = rr->wkst;
    int firstwkst = (7 - g->yearweekday + wkst) % 7;
    int no1wkst = firstwkst;
    int wyearlen, numweeks, i, j;

    if (no1wkst >= 4) {
        no1wkst = 0;
        wyearlen = g->yearlen + DYN_RRULE_PMOD7(g->yearweekday - wkst);
    } else {
        wyearlen = g->yearlen - no1wkst;
    }
    numweeks = wyearlen / 7 + (wyearlen % 7) / 4;

    memset(g->wnomask, 0, sizeof(g->wnomask));
    for (i = 0; i < rr->n_byweekno; i++) {
        int n = rr->byweekno[i];
        int idx;
        if (n < 0)
            n += numweeks + 1;
        if (!(0 < n && n <= numweeks))
            continue;
        if (n > 1) {
            idx = no1wkst + (n - 1) * 7;
            if (no1wkst != firstwkst)
                idx -= 7 - firstwkst;
        } else {
            idx = no1wkst;
        }
        for (j = 0; j < 7; j++) {
            g->wnomask[idx] = 1;
            idx++;
            if (g->wdaymask[idx] == wkst)
                break;
        }
    }
    if (dyn_rrule_in(rr->byweekno, rr->n_byweekno, 1)) {
        int idx = no1wkst + numweeks * 7;
        if (no1wkst != firstwkst)
            idx -= 7 - firstwkst;
        if (idx < g->yearlen) {
            for (j = 0; j < 7; j++) {
                g->wnomask[idx] = 1;
                idx++;
                if (g->wdaymask[idx] == wkst)
                    break;
            }
        }
    }
    if (no1wkst) {
        int lnumweeks;
        if (!dyn_rrule_in(rr->byweekno, rr->n_byweekno, -1)) {
            int lyearlen = 365 + dyn_rrule_is_leap(g->year - 1);
            int lyearweekday = dyn_rrule_iso_wd(g->yearordinal - lyearlen);
            int lno1wkst = (7 - lyearweekday + wkst) % 7;
            if (lno1wkst >= 4) {
                lno1wkst = 0;
                lnumweeks = 52 + (((lyearlen + DYN_RRULE_PMOD7(lyearweekday - wkst)) % 7) / 4);
            } else {
                lnumweeks = 52 + (((g->yearlen - no1wkst) % 7) / 4);
            }
        } else {
            lnumweeks = -1;
        }
        if (dyn_rrule_in(rr->byweekno, rr->n_byweekno, lnumweeks)) {
            for (i = 0; i < no1wkst; i++)
                g->wnomask[i] = 1;
        }
    }
}

static void dyn_rrule_mark_nwd_range(DynRRuleGen* g, int first, int last)
{
    const DynRRule* rr = g->rr;
    int p;
    for (p = 0; p < rr->n_bynwd; p++) {
        int wd = rr->bynwd[p * 2], n = rr->bynwd[p * 2 + 1];
        int i;
        if (n < 0) {
            i = last + (n + 1) * 7;
            i -= DYN_RRULE_PMOD7(dyn_rrule_gen_wdof(g, i) - wd);
        } else {
            i = first + (n - 1) * 7;
            i += (7 - dyn_rrule_gen_wdof(g, i) + wd) % 7;
        }
        if (first <= i && i <= last)
            g->nwdaymask[i] = 1;
    }
}

static void dyn_rrule_rebuild(DynRRuleGen* g)
{
    const DynRRule* rr = g->rr;
    if ((int)g->year != g->lastyear)
        dyn_rrule_rebuild_year(g);
    if (rr->n_bynwd && ((int)g->year != g->lastyear || g->month != g->lastmonth)) {
        memset(g->nwdaymask, 0, (size_t)g->yearlen);
        if (rr->freq == DYN_RRULE_YEARLY) {
            if (rr->n_bymonth) {
                int k;
                for (k = 0; k < rr->n_bymonth; k++) {
                    int m = rr->bymonth[k];
                    dyn_rrule_mark_nwd_range(g, g->mrange[m - 1],
                        g->mrange[m] - 1);
                }
            } else {
                dyn_rrule_mark_nwd_range(g, 0, g->yearlen - 1);
            }
        } else if (rr->freq == DYN_RRULE_MONTHLY) {
            dyn_rrule_mark_nwd_range(g, g->mrange[g->month - 1],
                g->mrange[g->month] - 1);
        }
    }
    g->lastyear = (int)g->year;
    g->lastmonth = g->month;
}

static int dyn_rrule_day_kept(const DynRRuleGen* g, int i)
{
    const DynRRule* rr = g->rr;
    if (rr->bits_ok) {
        if (rr->n_bymonth && !dyn_rrule_has(rr->bits_month, g->mmask[i]))
            return 0;
        if (rr->n_byweekno && !g->wnomask[i])
            return 0;
        if (rr->n_byweekday && !dyn_rrule_has(rr->bits_wd, g->wdaymask[i]))
            return 0;
        if (rr->n_bynwd && !g->nwdaymask[i])
            return 0;
        if ((rr->n_bymonthday || rr->n_bynmonthday) && !dyn_rrule_has(rr->bits_mday, g->mdaymask[i]) && !dyn_rrule_has(rr->bits_nmday, g->nmdaymask[i]))
            return 0;
        if (!rr->n_byyearday)
            return 1;
    } else if (rr->n_bymonth && !dyn_rrule_in(rr->bymonth, rr->n_bymonth, g->mmask[i]))
        return 0;
    if (rr->n_byweekno && !g->wnomask[i])
        return 0;
    if (rr->n_byweekday && !dyn_rrule_in(rr->byweekday, rr->n_byweekday, g->wdaymask[i]))
        return 0;
    if (rr->n_bynwd && !g->nwdaymask[i])
        return 0;
    if (rr->n_bymonthday || rr->n_bynmonthday) {
        if (!dyn_rrule_in(rr->bymonthday, rr->n_bymonthday, g->mdaymask[i]) && !dyn_rrule_in(rr->bynmonthday, rr->n_bynmonthday, g->nmdaymask[i]))
            return 0;
    }
    if (rr->n_byyearday) {
        int16_t v1, v2;
        if (i < g->yearlen) {
            v1 = (int16_t)(i + 1);
            v2 = (int16_t)(-g->yearlen + i);
        } else {
            v1 = (int16_t)(i + 1 - g->yearlen);
            v2 = (int16_t)(-g->nextyearlen + i - g->yearlen);
        }
        if (!dyn_rrule_in(rr->byyearday, rr->n_byyearday, v1) && !dyn_rrule_in(rr->byyearday, rr->n_byyearday, v2))
            return 0;
    }
    return 1;
}

static int dyn_rrule_emit(const DynRRule* rr, int64_t res, int64_t* count,
    DynRRuleYield fn, void* ud)
{
    if (rr->has_until && res > rr->until)
        return -1;
    if (res >= rr->dtstart) {
        if (rr->has_count) {
            (*count)--;
            if (*count < 0)
                return 1;
        }
        if (fn(res, ud))
            return 1;
    }
    return 0;
}

static void dyn_rrule_cursor_from_sec(DynRRuleGen* g, int64_t sec)
{
    const DynRRule* rr = g->rr;
    int64_t days = dyn_time_floor_div(sec, DYN_SECS_PER_DAY);
    int64_t tod = dyn_time_floor_mod(sec, DYN_SECS_PER_DAY);
    int64_t y;
    int mo, d;

    dyn_civil_from_days(days, &y, &mo, &d);
    g->year = y;
    g->month = mo;
    g->day = d;
    switch (rr->freq) {
    case DYN_RRULE_YEARLY:
        g->month = rr->ds_mo;
        g->day = rr->ds_d;
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_MONTHLY:
        g->day = rr->ds_d;
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_WEEKLY:
    case DYN_RRULE_DAILY:
        g->hour = rr->ds_h;
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_HOURLY:
        g->hour = (int)(tod / 3600);
        g->minute = rr->ds_mi;
        g->second = rr->ds_s;
        break;
    case DYN_RRULE_MINUTELY:
        g->hour = (int)(tod / 3600);
        g->minute = (int)((tod / 60) % 60);
        g->second = rr->ds_s;
        break;
    default:
        g->hour = (int)(tod / 3600);
        g->minute = (int)((tod / 60) % 60);
        g->second = (int)(tod % 60);
        break;
    }
    g->weekday = dyn_rrule_iso_wd(days);
}

void dyn_rrule_gen_init(DynRRuleGen* g, const DynRRule* rr,
    int64_t cursor_sec)
{
    memset(g, 0, sizeof(*g));
    g->rr = rr;
    g->exhausted = 0;
    g->stop_day = rr->has_until ? dyn_time_floor_div(rr->until, DYN_SECS_PER_DAY) : INT64_MAX;
    dyn_rrule_cursor_from_sec(g, cursor_sec);
    dyn_rrule_rebuild(g);
    if (rr->freq >= DYN_RRULE_HOURLY) {
        int64_t tod = dyn_time_floor_mod(cursor_sec, DYN_SECS_PER_DAY);
        int h = (int)(tod / 3600), mi = (int)((tod / 60) % 60),
            se = (int)(tod % 60);
        if (rr->freq == DYN_RRULE_HOURLY)
            g->timeset[0] = h * 3600 + rr->ds_mi * 60 + rr->ds_s;
        else if (rr->freq == DYN_RRULE_MINUTELY)
            g->timeset[0] = h * 3600 + mi * 60 + rr->ds_s;
        else
            g->timeset[0] = h * 3600 + mi * 60 + se;
    } else {
        g->timeset[0] = rr->ds_h * 3600 + rr->ds_mi * 60 + rr->ds_s;
    }
    g->n_timeset = 1;
}

static int64_t dyn_rrule_week_start(int64_t days, int wkst)
{
    return days - (int64_t)((dyn_rrule_iso_wd(days) - wkst + 7) % 7);
}

void dyn_rrule_gen_seek(DynRRuleGen* g, const DynRRule* rr, int64_t t)
{
    int64_t ds = rr->dtstart, dsd = dyn_time_floor_div(ds, DYN_SECS_PER_DAY);
    int64_t td = dyn_time_floor_div(t, DYN_SECS_PER_DAY), p, k, at;

    if (rr->has_count || t <= ds) {
        dyn_rrule_gen_init(g, rr, ds);
        return;
    }
    switch (rr->freq) {
    case DYN_RRULE_YEARLY: {
        int64_t y;
        int mo, d;
        dyn_civil_from_days(td, &y, &mo, &d);
        p = y - rr->ds_y;
        break;
    }
    case DYN_RRULE_MONTHLY: {
        int64_t y;
        int mo, d;
        dyn_civil_from_days(td, &y, &mo, &d);
        p = (y * 12 + mo) - (rr->ds_y * 12 + rr->ds_mo);
        break;
    }
    case DYN_RRULE_WEEKLY:
        p = (dyn_rrule_week_start(td, rr->wkst) - dyn_rrule_week_start(dsd, rr->wkst)) / 7;
        break;
    case DYN_RRULE_DAILY:
        p = td - dsd;
        break;
    case DYN_RRULE_HOURLY:
        p = dyn_time_floor_div(t, 3600) - dyn_time_floor_div(ds, 3600);
        break;
    case DYN_RRULE_MINUTELY:
        p = dyn_time_floor_div(t, 60) - dyn_time_floor_div(ds, 60);
        break;
    default:
        p = t - ds;
        break;
    }
    k = (p / rr->interval) * rr->interval;
    if (k <= 0) {
        dyn_rrule_gen_init(g, rr, ds);
        return;
    }
    switch (rr->freq) {
    case DYN_RRULE_YEARLY:
        at = dyn_days_from_civil(rr->ds_y + k, 1, 1) * DYN_SECS_PER_DAY;
        break;
    case DYN_RRULE_MONTHLY: {
        int64_t m = rr->ds_y * 12 + (rr->ds_mo - 1) + k;
        at = dyn_days_from_civil(dyn_time_floor_div(m, 12), (int)dyn_time_floor_mod(m, 12) + 1, 1) * DYN_SECS_PER_DAY;
        break;
    }
    case DYN_RRULE_WEEKLY:
        at = dyn_rrule_week_start(dsd, rr->wkst) + 7 * k;
        if (k == p && !rr->n_bysetpos && td > at)
            at = td;
        at *= DYN_SECS_PER_DAY;
        break;
    case DYN_RRULE_DAILY:
        at = (dsd + k) * DYN_SECS_PER_DAY;
        break;
    case DYN_RRULE_HOURLY:
        at = ds + k * 3600;
        break;
    case DYN_RRULE_MINUTELY:
        at = ds + k * 60;
        break;
    default:
        at = ds + k;
        break;
    }
    dyn_rrule_gen_init(g, rr, at);
}

void dyn_rrule_gen_loop(DynRRuleGen* g, DynRRuleYield fn, void* ud)
{
    const DynRRule* rr = g->rr;
    int freq = rr->freq, interval = rr->interval, wkst = rr->wkst;
    int64_t count = rr->count;
    int32_t kept[373];
    int64_t poslist[DYN_RRULE_SETPOS_CAP];

    for (;;) {
        int start, end, i, j, k;
        int n_kept = 0;
        int filtered = 0;

        if (++g->steps > DYN_RRULE_MAX_STEPS) {
            g->exhausted = 1;
            g->limit = DYN_RRULE_LIMIT_STEPS;
            break;
        }

        if (g->stop_day != INT64_MAX) {
            int64_t pday = freq == DYN_RRULE_YEARLY ? g->yearordinal
                : freq == DYN_RRULE_MONTHLY         ? g->yearordinal + g->mrange[g->month - 1]
                                                    : dyn_days_from_civil(g->year, g->month, g->day);
            if (pday > g->stop_day)
                return;
        }

        if (freq == DYN_RRULE_YEARLY) {
            start = 0;
            end = g->yearlen;
            if (g->bounded) {
                if (g->hi_day - g->yearordinal < start)
                    return;
                if (g->lo_day - g->yearordinal > start)
                    start = g->lo_day - g->yearordinal < end ? (int)(g->lo_day - g->yearordinal) : end;
                if (g->hi_day - g->yearordinal + 1 < end)
                    end = (int)(g->hi_day - g->yearordinal + 1);
            }
            g->work += (uint64_t)(end - start);
            for (i = start; i < end; i++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
            }
        } else if (freq == DYN_RRULE_MONTHLY) {
            start = g->mrange[g->month - 1];
            end = g->mrange[g->month];
            if (g->bounded) {
                if (g->hi_day - g->yearordinal < start)
                    return;
                if (g->lo_day - g->yearordinal > start)
                    start = g->lo_day - g->yearordinal < end ? (int)(g->lo_day - g->yearordinal) : end;
                if (g->hi_day - g->yearordinal + 1 < end)
                    end = (int)(g->hi_day - g->yearordinal + 1);
            }
            g->work += (uint64_t)(end - start);
            for (i = start; i < end; i++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
            }
        } else if (freq == DYN_RRULE_WEEKLY) {
            int64_t day0 = dyn_days_from_civil(g->year, g->month, g->day);
            int i0 = (int)(day0 - g->yearordinal);
            i = i0;
            for (j = 0; j < 7; j++) {
                if (dyn_rrule_day_kept(g, i))
                    kept[n_kept++] = i;
                else
                    filtered = 1;
                i++;
                if (g->wdaymask[i] == wkst)
                    break;
            }
        } else {
            int64_t day0 = dyn_days_from_civil(g->year, g->month, g->day);
            int i0 = (int)(day0 - g->yearordinal);
            if (dyn_rrule_day_kept(g, i0))
                kept[n_kept++] = i0;
            else
                filtered = 1;
        }

        if (g->work > DYN_RRULE_MAX_WORK) {
            g->exhausted = 1;
            g->limit = DYN_RRULE_LIMIT_WORK;
            break;
        }

        if (rr->n_bysetpos && g->n_timeset > 0) {
            int ntime = g->n_timeset;
            int n_pos = 0;
            g->work += (uint64_t)rr->n_bysetpos;
            for (k = 0; k < rr->n_bysetpos; k++) {
                int pos = rr->bysetpos[k];
                int daypos, timepos, idx;
                int64_t res;
                if (pos < 0) {
                    int q = pos / ntime, r = pos % ntime;
                    if (r < 0) {
                        r += ntime;
                        q--;
                    }
                    daypos = q;
                    timepos = r;
                } else {
                    daypos = (pos - 1) / ntime;
                    timepos = (pos - 1) % ntime;
                }
                idx = daypos < 0 ? n_kept + daypos : daypos;
                if (idx < 0 || idx >= n_kept)
                    continue;
                res = (g->yearordinal + kept[idx]) * DYN_SECS_PER_DAY + g->timeset[timepos];
                if (n_pos >= DYN_RRULE_SETPOS_CAP)
                    break;
                poslist[n_pos++] = res;
            }
            qsort(poslist, (size_t)n_pos, sizeof(*poslist), dyn_rrule_i64_cmp);
            {
                int m = 0;
                for (k = 0; k < n_pos; k++) {
                    if (m && poslist[k] == poslist[m - 1])
                        continue;
                    poslist[m++] = poslist[k];
                }
                n_pos = m;
            }
            for (k = 0; k < n_pos; k++) {
                int rc = dyn_rrule_emit(rr, poslist[k], &count, fn, ud);
                if (rc)
                    return;
            }
        } else {
            for (i = 0; i < n_kept; i++) {
                int64_t base = (g->yearordinal + kept[i]) * DYN_SECS_PER_DAY;
                for (j = 0; j < g->n_timeset; j++) {
                    int rc = dyn_rrule_emit(rr, base + g->timeset[j],
                        &count, fn, ud);
                    if (rc)
                        return;
                }
            }
        }

        if (g->bounded
            && ((freq == DYN_RRULE_YEARLY && g->hi_day - g->yearordinal < g->yearlen)
                || (freq == DYN_RRULE_MONTHLY && g->hi_day - g->yearordinal < g->mrange[g->month])))
            return;
        {
            int fixday = 0;
            if (freq == DYN_RRULE_YEARLY) {
                g->year += interval;
                if (g->year > DYN_RRULE_MAX_YEAR) {
                    g->exhausted = 1;
                    g->limit = DYN_RRULE_LIMIT_YEAR;
                    return;
                }
                dyn_rrule_rebuild(g);
            } else if (freq == DYN_RRULE_MONTHLY) {
                g->month += interval;
                if (g->month > 12) {
                    int div = g->month / 12, mod = g->month % 12;
                    g->month = mod;
                    g->year += div;
                    if (g->month == 0) {
                        g->month = 12;
                        g->year--;
                    }
                    if (g->year > DYN_RRULE_MAX_YEAR) {
                        g->exhausted = 1;
                        g->limit = DYN_RRULE_LIMIT_YEAR;
                        return;
                    }
                }
                dyn_rrule_rebuild(g);
            } else if (freq == DYN_RRULE_WEEKLY) {
                if (wkst > g->weekday)
                    g->day += -(g->weekday + 1 + (6 - wkst)) + interval * 7;
                else
                    g->day += -(g->weekday - wkst) + interval * 7;
                g->weekday = wkst;
                fixday = 1;
            } else if (freq == DYN_RRULE_DAILY) {
                g->day += interval;
                fixday = 1;
            } else if (freq == DYN_RRULE_HOURLY) {
                int ndays, h;
                if (filtered)
                    g->hour += ((23 - g->hour) / interval) * interval;
                h = g->hour + interval;
                ndays = h / 24;
                g->hour = h % 24;
                if (ndays) {
                    g->day += ndays;
                    fixday = 1;
                }
                g->timeset[0] = g->hour * 3600 + rr->ds_mi * 60 + rr->ds_s;
            } else if (freq == DYN_RRULE_MINUTELY) {
                int total, nhours, div, h;
                if (filtered)
                    g->minute += ((1439 - (g->hour * 60 + g->minute)) / interval) * interval;
                total = g->minute + interval;
                nhours = total / 60;
                g->minute = total % 60;
                h = g->hour + nhours;
                div = h / 24;
                g->hour = h % 24;
                if (div) {
                    g->day += div;
                    fixday = 1;
                }
                g->timeset[0] = g->hour * 3600 + g->minute * 60 + rr->ds_s;
            } else {
                int total, nmin, div2;
                if (filtered)
                    g->second += ((86399 - (g->hour * 3600 + g->minute * 60 + g->second)) / interval) * interval;
                total = g->second + interval;
                nmin = total / 60;
                g->second = total % 60;
                div2 = (g->minute + nmin) / 60;
                g->minute = (g->minute + nmin) % 60;
                if (div2) {
                    g->hour += div2;
                    div2 = g->hour / 24;
                    g->hour = g->hour % 24;
                    if (div2) {
                        g->day += div2;
                        fixday = 1;
                    }
                }
                g->timeset[0] = g->hour * 3600 + g->minute * 60 + g->second;
            }

            if (fixday && g->day > 28) {
                int dim = dyn_time_days_in_month(g->year, g->month);
                if (g->day > dim) {
                    while (g->day > dim) {
                        g->day -= dim;
                        g->month++;
                        if (g->month == 13) {
                            g->month = 1;
                            g->year++;
                            if (g->year > DYN_RRULE_MAX_YEAR) {
                                g->exhausted = 1;
                                g->limit = DYN_RRULE_LIMIT_YEAR;
                                return;
                            }
                        }
                        dim = dyn_time_days_in_month(g->year, g->month);
                    }
                    dyn_rrule_rebuild(g);
                }
            }
        }
    }
}
