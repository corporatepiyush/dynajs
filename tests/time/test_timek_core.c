/*
 * test_timek_core.c -- src/core/timek (dyna:time civil-date helpers, the
 * DateParser grammar and the RRule expander) as a standalone C library.
 *
 * Every oracle here is written from the calendar rules, not from the code:
 * naive_days() walks years and months with the Gregorian leap rule, so
 * days_from_civil / civil_from_days / weekday / days_in_month are checked
 * against counting. DateParser is fed text rendered from random y-m-d h:m:s
 * in every locale and every grammar form (ISO dash/slash, month-name first,
 * day first, numeric with the locale's day/month order, 2-digit years,
 * weekday prefixes, am/pm, relative "in N unit" / "N unit ago" with month
 * clamping, next/last weekday by stepping days) and must give the counted
 * epoch; impossible dates (Feb 29 off leap years, day 31 in 30-day months,
 * month 13) must be rejected. A leading token that is also a weekday name
 * or its 3-letter form (fr/es "mar" = mardi/martes, pinned in
 * test_time_date_parser_names.js) is read as the weekday, so month-first
 * text starting with it is expected to fail. The RRule expander is checked against a
 * brute-force day scan that applies RFC 5545 period and BY* rules to every
 * day: DAILY/WEEKLY/MONTHLY/YEARLY, interval 1..3, BYMONTH, positive and
 * negative BYMONTHDAY, plain BYDAY and the per-freq dtstart defaults, with
 * COUNT and UNTIL. Also: BYDAY ordinals stored before FREQ is known fold
 * into plain weekdays for sub-monthly freqs (compile used to append to an
 * uninitialised count), and parse_datetime against the same counting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-timek.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            if (fails < 30) {                           \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                    \
                printf("\n");                           \
            }                                           \
        }                                               \
    } while (0)

static uint32_t rs = 4242;
static uint32_t rnd(uint32_t n)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) % n;
}

static int leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int mlen(int64_t y, int m)
{
    static const int T[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : T[m - 1];
}

static int64_t naive_days(int64_t y, int m, int d)
{
    int64_t z = 0, k;
    int i;
    if (y >= 1970)
        for (k = 1970; k < y; k++)
            z += leap(k) ? 366 : 365;
    else
        for (k = y; k < 1970; k++)
            z -= leap(k) ? 366 : 365;
    for (i = 1; i < m; i++)
        z += mlen(y, i);
    return z + d - 1;
}

static int naive_iso_wd(int64_t z)
{
    int64_t r = (z % 7 + 7) % 7;
    return (int)((r + 3) % 7);
}

static void test_civil(void)
{
    int it;
    for (it = 0; it < 4000; it++) {
        int64_t y = (int64_t)rnd(12000) - 1000, yy;
        int m = (int)rnd(12) + 1, d = (int)rnd((uint32_t)mlen(y, m)) + 1, mm, dd;
        int64_t z = naive_days(y, m, d);
        CHECK(dyn_days_from_civil(y, m, d) == z, "days_from_civil %lld-%d-%d", (long long)y, m, d);
        dyn_civil_from_days(z, &yy, &mm, &dd);
        CHECK(yy == y && mm == m && dd == d, "civil_from_days %lld", (long long)z);
        CHECK(dyn_time_days_in_month(y, m) == mlen(y, m), "days_in_month %lld-%d", (long long)y, m);
        CHECK((dyn_weekday_from_days(z) + 6) % 7 == naive_iso_wd(z), "weekday %lld", (long long)z);
        CHECK(dyn_time_floor_div(z, 7) * 7 + dyn_time_floor_mod(z, 7) == z && dyn_time_floor_mod(z, 7) >= 0, "floor div/mod %lld", (long long)z);
    }
    {
        int64_t v = -1;
        CHECK(dyn_time_expect_digits("x0123", 5, 1, 4, &v) == 0 && v == 123, "expect_digits");
        CHECK(dyn_time_expect_digits("012a", 4, 0, 4, &v) == -1, "expect_digits non-digit");
        CHECK(dyn_time_expect_digits("012", 3, 0, 4, &v) == -1, "expect_digits short");
    }
}

static void ymd(int64_t z, int64_t* y, int* m, int* d);

static const char* const LOCS[6] = { "en-US", "en-GB", "en", "fr", "de", "es" };

static void up_mix(char* s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z' && rnd(3) == 0)
            *s = (char)(*s - 32);
}

static int wd_token(const DynDateLocale* loc, const char* w)
{
    char b[40];
    size_t i, n = strlen(w);
    for (i = 0; i < n && i < sizeof b - 1; i++)
        b[i] = (char)(w[i] >= 'A' && w[i] <= 'Z' ? w[i] + 32 : w[i]);
    b[i] = 0;
    for (i = 0; i < 7; i++)
        if (strcmp(b, loc->days[i]) == 0 || (n == 3 && strncmp(b, loc->days[i], 3) == 0))
            return 1;
    return 0;
}

static void test_dateparse(void)
{
    int it;
    CHECK(dyn_dp_locale_find("xx") == NULL, "unknown locale");
    CHECK(dyn_dp_locale_find(NULL) == dyn_dp_locale_find("en-US"), "default locale is en-US");
    for (it = 0; it < 6000; it++) {
        const DynDateLocale* loc = dyn_dp_locale_find(LOCS[rnd(6)]);
        DynDateParser p = { .loc = loc, .base = 0, .has_base = 0 };
        int64_t y = 1900 + rnd(200), h = rnd(24), mi = rnd(60), se = rnd(60), out = -7, want;
        int m = (int)rnd(12) + 1, d = (int)rnd((uint32_t)mlen(y, m)) + 1, form = (int)rnd(7), rc, with_time = (int)rnd(2);
        char buf[160], mon[40], tm[40] = "";
        int64_t z = naive_days(y, m, d);
        if (!with_time)
            h = mi = se = 0;
        else if (rnd(3) == 0 && h >= 1 && h <= 12) {
            snprintf(tm, sizeof tm, " %lld:%02lld:%02lld %s", (long long)(h == 12 ? 12 : h), (long long)mi, (long long)se, rnd(2) ? "am" : "AM");
            if (h == 12)
                h = 0;
        } else if (rnd(3) == 0 && h >= 13) {
            snprintf(tm, sizeof tm, " %lld:%02lld pm", (long long)(h - 12), (long long)mi);
            se = 0;
        } else
            snprintf(tm, sizeof tm, "%s%02lld:%02lld:%02lld", rnd(2) ? " " : "T", (long long)h, (long long)mi, (long long)se);
        snprintf(mon, sizeof mon, "%s", rnd(2) ? loc->months[m - 1] : loc->months_abbr[m - 1]);
        up_mix(mon);
        switch (form) {
        case 0:
            snprintf(buf, sizeof buf, "%04lld-%02d-%02d%s", (long long)y, m, d, tm);
            break;
        case 1:
            snprintf(buf, sizeof buf, "%04lld/%d/%d%s", (long long)y, m, d, tm);
            break;
        case 2:
            snprintf(buf, sizeof buf, "%s %d %lld%s", mon, d, (long long)y, tm);
            break;
        case 3:
            snprintf(buf, sizeof buf, "%d %s %lld%s", d, mon, (long long)y, tm);
            break;
        case 4:
            if (loc->day_first)
                snprintf(buf, sizeof buf, "%d.%d.%lld%s", d, m, (long long)y, tm);
            else
                snprintf(buf, sizeof buf, "%d/%d/%lld%s", m, d, (long long)y, tm);
            break;
        case 5:
            if (y < 1970 || y > 2069)
                y = 1970 + rnd(100);
            if (d > mlen(y, m))
                d = mlen(y, m);
            z = naive_days(y, m, d);
            snprintf(buf, sizeof buf, "%s %d %02lld%s", mon, d, (long long)(y % 100), tm);
            break;
        default: {
            char wd[40];
            const char* full = loc->days[(naive_iso_wd(z) + 1) % 7];
            snprintf(wd, sizeof wd, "%s", full);
            if (rnd(2))
                wd[3] = 0;
            snprintf(buf, sizeof buf, "%s %s %d %lld%s", wd, mon, d, (long long)y, tm);
            break;
        }
        }
        want = z * 86400 + h * 3600 + mi * 60 + se;
        rc = dyn_dp_parse(&p, buf, strlen(buf), 0, &out);
        if ((form == 2 || form == 5) && wd_token(loc, mon)) {
            CHECK(rc == -1, "dateparse %s '%s': a leading weekday-form token is a weekday, not a month", loc->name, buf);
            continue;
        }
        CHECK(rc == 0 && out == want, "dateparse %s '%s': rc %d got %lld want %lld", loc->name, buf, rc, (long long)out, (long long)want);
    }
    {
        static const char* const BAD[] = { "2023-02-29", "2024-04-31", "2024-13-01", "2024-00-10", "February 30 2024", "31 juni 2024", "2024-1-5x", "12:00", "", "   ", "2024-01-01 25:00", "2024-01-01 10:61" };
        DynDateParser p = { .loc = dyn_dp_locale_find("en"), .base = 0, .has_base = 0 };
        size_t i;
        for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
            int64_t out = 0;
            const DynDateParser* pp = &p;
            DynDateParser de = { .loc = dyn_dp_locale_find("de"), .base = 0, .has_base = 0 };
            if (strstr(BAD[i], "juni"))
                pp = &de;
            CHECK(dyn_dp_parse(pp, BAD[i], strlen(BAD[i]), 0, &out) == -1, "dateparse rejects '%s'", BAD[i]);
        }
    }
    for (it = 0; it < 3000; it++) {
        static const char* const U[] = { "second", "minute", "hour", "day", "week", "month", "year" };
        static const int64_t S[] = { 1, 60, 3600, 86400, 604800, 0, 0 };
        DynDateParser p = { .loc = dyn_dp_locale_find("en"), .base = 0, .has_base = 0 };
        int64_t now = naive_days(1950 + rnd(120), 1, 1) * 86400 + (int64_t)rnd(366 * 86400), out = -1, want;
        int u = (int)rnd(7), ago = (int)rnd(2), rc;
        int64_t nv = rnd(40);
        char buf[80];
        snprintf(buf, sizeof buf, ago ? "%lld %s%s ago" : "in %lld %s%s", (long long)nv, U[u], nv == 1 ? "" : "s");
        if (S[u])
            want = now + (ago ? -1 : 1) * nv * S[u];
        else {
            int64_t z = dyn_time_floor_div(now, 86400), tod = now - z * 86400, y, k;
            int m, d;
            ymd(z, &y, &m, &d);
            for (k = 0; k < nv * (u == 5 ? 1 : 12); k++) {
                m += ago ? -1 : 1;
                if (m == 13) {
                    m = 1;
                    y++;
                } else if (m == 0) {
                    m = 12;
                    y--;
                }
            }
            want = naive_days(y, m, d > mlen(y, m) ? mlen(y, m) : d) * 86400 + tod;
        }
        rc = dyn_dp_parse(&p, buf, strlen(buf), now, &out);
        CHECK(rc == 0 && out == want, "relative '%s' now %lld: got %lld want %lld", buf, (long long)now, (long long)out, (long long)want);
    }
    for (it = 0; it < 700; it++) {
        const DynDateLocale* loc = dyn_dp_locale_find(LOCS[rnd(6)]);
        DynDateParser p = { .loc = loc, .base = 0, .has_base = 0 };
        int64_t now = (int64_t)rnd(2000000000u), out = -1, day = dyn_time_floor_div(now, 86400);
        int wd = (int)rnd(7), dir = rnd(2) ? 1 : -1, rc;
        char buf[60];
        int64_t k = day;
        do
            k += dir;
        while ((naive_iso_wd(k) + 1) % 7 != wd);
        snprintf(buf, sizeof buf, "%s %s", dir > 0 ? "next" : "last", loc->days[wd]);
        rc = dyn_dp_parse(&p, buf, strlen(buf), now, &out);
        CHECK(rc == 0 && out == k * 86400, "'%s' (%s) now %lld: got %lld want %lld", buf, loc->name, (long long)now, (long long)out, (long long)(k * 86400));
    }
    {
        DynDateParser p = { .loc = dyn_dp_locale_find("en"), .base = 0, .has_base = 0 };
        int64_t now = 1700000000 + 12345, out = 0, mid = (now / 86400) * 86400;
        CHECK(dyn_dp_parse(&p, " now ", 5, now, &out) == 0 && out == now, "now");
        CHECK(dyn_dp_parse(&p, "Today", 5, now, &out) == 0 && out == mid, "today");
        CHECK(dyn_dp_parse(&p, "tomorrow", 8, now, &out) == 0 && out == mid + 86400, "tomorrow");
        CHECK(dyn_dp_parse(&p, "yesterday", 9, now, &out) == 0 && out == mid - 86400, "yesterday");
    }
}

static void test_parse_datetime(void)
{
    int it;
    for (it = 0; it < 3000; it++) {
        int64_t y = 1 + rnd(9999), h = rnd(24), mi = rnd(60), se = rnd(60), oh = rnd(14), om = rnd(60), out = -1, want;
        int m = (int)rnd(12) + 1, d = (int)rnd((uint32_t)mlen(y, m)) + 1, f = (int)rnd(5), rc, sign = rnd(2) ? 1 : -1;
        char buf[80];
        want = naive_days(y, m, d) * 86400;
        switch (f) {
        case 0:
            snprintf(buf, sizeof buf, "%04lld%02d%02d", (long long)y, m, d);
            break;
        case 1:
            snprintf(buf, sizeof buf, "%04lld%02d%02dT%02lld%02lld%02lldZ", (long long)y, m, d, (long long)h, (long long)mi, (long long)se);
            want += h * 3600 + mi * 60 + se;
            break;
        case 2:
            snprintf(buf, sizeof buf, "%04lld-%02d-%02dT%02lld:%02lld:%02lld.250Z", (long long)y, m, d, (long long)h, (long long)mi, (long long)se);
            want += h * 3600 + mi * 60 + se;
            break;
        case 3:
            snprintf(buf, sizeof buf, "%04lld-%02d-%02dT%02lld:%02lld%c%02lld:%02lld", (long long)y, m, d, (long long)h, (long long)mi, sign > 0 ? '+' : '-', (long long)oh, (long long)om);
            want += h * 3600 + mi * 60 - sign * (oh * 3600 + om * 60);
            break;
        default:
            snprintf(buf, sizeof buf, "%04lld-%02d-%02d", (long long)y, m, d);
            break;
        }
        rc = dyn_rrule_parse_datetime(buf, strlen(buf), &out);
        CHECK(rc == 0 && out == want, "parse_datetime '%s': got %lld want %lld", buf, (long long)out, (long long)want);
    }
    {
        static const char* const BAD[] = { "20230229", "2024-02-30", "20241301", "2024-01-01T24:00:00Z", "2024-01-01T10:00:00+24:00", "2024-01-01T", "2024-01-0", "2024/01/01", "20240101T1000" };
        size_t i;
        for (i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
            int64_t out = 0;
            CHECK(dyn_rrule_parse_datetime(BAD[i], strlen(BAD[i]), &out) == -1, "parse_datetime rejects '%s'", BAD[i]);
        }
    }
}

typedef struct {
    int64_t v[400];
    int n;
} Coll;

static int coll_cb(int64_t sec, void* ud)
{
    Coll* c = ud;
    if (c->n >= 400)
        return 1;
    c->v[c->n++] = sec;
    return 0;
}

static void ymd(int64_t z, int64_t* y, int* m, int* d)
{
    *y = 1970;
    while (naive_days(*y + 1, 1, 1) <= z)
        (*y)++;
    while (naive_days(*y, 1, 1) > z)
        (*y)--;
    *m = 1;
    while (*m < 12 && naive_days(*y, *m + 1, 1) <= z)
        (*m)++;
    *d = (int)(z - naive_days(*y, *m, 1)) + 1;
}

static int in16(const int16_t* a, int n, int v)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == v)
            return 1;
    return 0;
}

static void test_rrule(void)
{
    int it;
    for (it = 0; it < 500; it++) {
        DynRRuleSpec s;
        DynRRule rr;
        DynRRuleGen g;
        Coll got = { .n = 0 };
        int64_t want[400], bad = 0, z, ds_day, ds_tod, ds_y, dsdim;
        int nw = 0, ds_m, ds_d, ds_wd, rc, i;
        int defaults_by;
        memset(&s, 0, sizeof s);
        s.freq = (int)rnd(4);
        s.has_freq = 1;
        s.interval = (int32_t)rnd(3) + 1;
        s.wkst = 0;
        s.dtstart = naive_days(1990 + rnd(40), (int)rnd(12) + 1, 1 + (int)rnd(28)) * 86400 + (int64_t)rnd(86400);
        s.has_dtstart = 1;
        s.count = 1 + rnd(40);
        s.has_count = 1;
        s.until = s.dtstart + (int64_t)(400 + rnd(3000)) * 86400;
        s.has_until = rnd(3) == 0;
        if (rnd(2)) {
            int k = 1 + (int)rnd(4);
            while (k--)
                dyn_rrule_set_add(s.bymonth, &s.n_bymonth, DYN_RRULE_MONTH_CAP, (int)rnd(12) + 1);
            s.orig |= DYN_RR_ORIG_BYMONTH;
        }
        if (rnd(2)) {
            int k = 1 + (int)rnd(4);
            while (k--) {
                int v = (int)rnd(31) + 1;
                if (rnd(3) == 0)
                    dyn_rrule_set_add(s.bynmonthday, &s.n_bynmonthday, DYN_RRULE_MDAY_CAP, -v);
                else
                    dyn_rrule_set_add(s.bymonthday, &s.n_bymonthday, DYN_RRULE_MDAY_CAP, v);
            }
            s.orig |= DYN_RR_ORIG_BYMONTHDAY;
        }
        if (rnd(2)) {
            int k = 1 + (int)rnd(3);
            while (k--)
                dyn_rrule_set_add(s.byweekday, &s.n_byweekday, DYN_RRULE_WD_CAP, (int)rnd(7));
            s.orig |= DYN_RR_ORIG_BYWEEKDAY;
        }
        rc = dyn_rrule_compile(&rr, &s, 0, &bad);
        CHECK(rc == 0, "compile it %d rc %d", it, rc);
        if (rc)
            continue;
        dyn_rrule_gen_init(&g, &rr, rr.dtstart);
        dyn_rrule_gen_loop(&g, coll_cb, &got);

        ds_day = dyn_time_floor_div(s.dtstart, 86400);
        ds_tod = s.dtstart - ds_day * 86400;
        ymd(ds_day, &ds_y, &ds_m, &ds_d);
        ds_wd = naive_iso_wd(ds_day);
        dsdim = 0;
        (void)dsdim;
        defaults_by = !(s.orig & (DYN_RR_ORIG_BYMONTHDAY | DYN_RR_ORIG_BYWEEKDAY));
        {
            int64_t cy = ds_y;
            int cm = ds_m, cd = ds_d - 1;
        for (z = ds_day; nw < (int)s.count && z < ds_day + 366 * 125; z++) {
            int64_t y, sec = z * 86400 + ds_tod;
            int m, d, wd = naive_iso_wd(z), ok = 1;
            if (s.has_until && sec > s.until)
                break;
            if (++cd > mlen(cy, cm)) {
                cd = 1;
                if (++cm > 12) {
                    cm = 1;
                    cy++;
                }
            }
            y = cy;
            m = cm;
            d = cd;
            switch (s.freq) {
            case 0:
                ok = (y - ds_y) % s.interval == 0;
                break;
            case 1:
                ok = ((y * 12 + m) - (ds_y * 12 + ds_m)) % s.interval == 0;
                break;
            case 2:
                ok = ((z - wd) - (ds_day - ds_wd)) / 7 % s.interval == 0;
                break;
            default:
                ok = (z - ds_day) % s.interval == 0;
                break;
            }
            if (ok && s.n_bymonth)
                ok = in16(s.bymonth, s.n_bymonth, m);
            if (ok && s.freq == 0 && defaults_by && !s.n_bymonth)
                ok = m == ds_m;
            if (ok && (s.n_bymonthday || s.n_bynmonthday))
                ok = in16(s.bymonthday, s.n_bymonthday, d) || in16(s.bynmonthday, s.n_bynmonthday, d - mlen(y, m) - 1);
            if (ok && defaults_by && (s.freq == 0 || s.freq == 1))
                ok = d == ds_d;
            if (ok && s.n_byweekday)
                ok = in16(s.byweekday, s.n_byweekday, wd);
            if (ok && defaults_by && s.freq == 2)
                ok = wd == ds_wd;
            if (ok && sec >= s.dtstart)
                want[nw++] = sec;
        }
        }
        {
            int horizon = z >= ds_day + 366 * 125 && nw < (int)s.count;
            int same = horizon ? got.n >= nw : got.n == nw;
            for (i = 0; same && i < nw; i++)
                same = got.v[i] == want[i];
            CHECK(same, "rrule it %d freq %d int %d nmon %d nmd %d nnmd %d nwd %d count %lld until %d: got %d want %d (first diff %lld vs %lld)",
                it, s.freq, s.interval, s.n_bymonth, s.n_bymonthday, s.n_bynmonthday, s.n_byweekday, (long long)s.count, s.has_until, got.n, nw,
                (long long)(got.n ? got.v[0] : -1), (long long)(nw ? want[0] : -1));
        }
    }
    {
        DynRRuleSpec s;
        DynRRule rr;
        int64_t bad = 0;
        memset(&s, 0, sizeof s);
        s.freq = 2;
        s.has_freq = 1;
        s.interval = 1;
        s.dtstart = naive_days(2024, 1, 1) * 86400;
        s.has_dtstart = 1;
        s.bynwd[0] = 0;
        s.bynwd[1] = 1;
        s.bynwd[2] = 2;
        s.bynwd[3] = 2;
        s.n_bynwd = 2;
        s.orig = DYN_RR_ORIG_BYWEEKDAY;
        CHECK(dyn_rrule_compile(&rr, &s, 0, &bad) == 0 && rr.n_byweekday == 2 && rr.byweekday[0] == 0 && rr.byweekday[1] == 2 && rr.n_bynwd == 0,
            "BYDAY ordinals fold to plain weekdays for WEEKLY (n %d)", rr.n_byweekday);
        s.dtstart = naive_days(10000, 1, 1) * 86400;
        CHECK(dyn_rrule_compile(&rr, &s, 0, &bad) == DYN_RRULE_E_DTSTART_YEAR && bad == 10000, "dtstart year bound");
        s.dtstart = naive_days(2024, 1, 1) * 86400;
        s.until = naive_days(10001, 1, 1) * 86400;
        s.has_until = 1;
        CHECK(dyn_rrule_compile(&rr, &s, 0, &bad) == DYN_RRULE_E_UNTIL_YEAR && bad == 10001, "until year bound");
        s.has_until = 0;
        s.has_dtstart = 0;
        CHECK(dyn_rrule_compile(&rr, &s, naive_days(2001, 2, 3) * 86400 + 3723, &bad) == 0 && rr.ds_y == 2001 && rr.ds_mo == 2 && rr.ds_d == 3 && rr.ds_h == 1 && rr.ds_mi == 2 && rr.ds_s == 3,
            "dtstart defaults to now");
    }
}

int main(void)
{
    test_civil();
    test_dateparse();
    test_parse_datetime();
    test_rrule();
    printf("test_timek_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
