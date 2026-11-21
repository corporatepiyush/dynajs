#ifndef DYN_TIMEK_TIMEK_H
#define DYN_TIMEK_TIMEK_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DYN_SECS_PER_DAY 86400LL

static inline int64_t dyn_time_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

static inline int64_t dyn_time_floor_mod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

static inline int64_t dyn_days_from_civil(int64_t y, int m, int64_t d)
{
    int64_t era, yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline int dyn_weekday_from_days(int64_t z)
{
    return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

static inline int dyn_time_days_in_month(int64_t y, int m)
{
    int64_t y2 = y;
    int m2 = m + 1;
    if (m2 > 12) {
        m2 = 1;
        y2++;
    }
    return (int)(dyn_days_from_civil(y2, m2, 1) - dyn_days_from_civil(y, m, 1));
}

void dyn_civil_from_days(int64_t z, int64_t* y, int* m, int* d);

int dyn_time_expect_digits(const char* s, size_t len, size_t pos,
    int n, int64_t* out);

enum {
    DYN_RRULE_YEARLY = 0,
    DYN_RRULE_MONTHLY,
    DYN_RRULE_WEEKLY,
    DYN_RRULE_DAILY,
    DYN_RRULE_HOURLY,
    DYN_RRULE_MINUTELY,
    DYN_RRULE_SECONDLY,
};

#define DYN_RRULE_MAX_STEPS 1000000u
#define DYN_RRULE_MAX_RESULTS 1000000u
#define DYN_RRULE_MAX_YEAR 9999
#define DYN_RRULE_LIMIT_STEPS 1
#define DYN_RRULE_LIMIT_WORK 2
#define DYN_RRULE_LIMIT_YEAR 3
#define DYN_RRULE_MONTH_CAP 16
#define DYN_RRULE_MDAY_CAP 32
#define DYN_RRULE_YDAY_CAP 800
#define DYN_RRULE_WKNO_CAP 128
#define DYN_RRULE_SETPOS_CAP 800
#define DYN_RRULE_MAX_WORK ((uint64_t)1 << 28)
#define DYN_RRULE_WD_CAP 8
#define DYN_RRULE_BYNWD_CAP 742
#define DYN_RR_ORIG_BYMONTH 0x001
#define DYN_RR_ORIG_BYMONTHDAY 0x002
#define DYN_RR_ORIG_BYYEARDAY 0x004
#define DYN_RR_ORIG_BYWEEKNO 0x008
#define DYN_RR_ORIG_BYWEEKDAY 0x010
#define DYN_RR_ORIG_BYSETPOS 0x020
#define DYN_RR_ORIG_WKST 0x040
#define DYN_RRULE_PMOD7(x) (((x) % 7 + 7) % 7)

typedef struct DynRRuleSpec {
    int freq;
    int has_freq;
    int32_t interval;
    int wkst;
    int64_t count, until, dtstart;
    int has_count, has_until, has_dtstart;
    uint32_t orig;
    int16_t bymonth[DYN_RRULE_MONTH_CAP];
    int n_bymonth;
    int16_t bymonthday[DYN_RRULE_MDAY_CAP];
    int n_bymonthday;
    int16_t bynmonthday[DYN_RRULE_MDAY_CAP];
    int n_bynmonthday;
    int16_t byyearday[DYN_RRULE_YDAY_CAP];
    int n_byyearday;
    int16_t byweekno[DYN_RRULE_WKNO_CAP];
    int n_byweekno;
    int16_t bysetpos[DYN_RRULE_SETPOS_CAP];
    int n_bysetpos;
    int16_t byweekday[DYN_RRULE_WD_CAP];
    int n_byweekday;
    int16_t bynwd[DYN_RRULE_BYNWD_CAP * 2];
    int n_bynwd;
} DynRRuleSpec;

typedef struct DynRRule {
    uint8_t freq, wkst;
    int32_t interval;
    int64_t dtstart, until, count;
    int has_until, has_count;
    int64_t ds_y;
    int ds_mo, ds_d, ds_h, ds_mi, ds_s;
    uint32_t orig;
    int16_t bymonth[DYN_RRULE_MONTH_CAP];
    int n_bymonth;
    int16_t bymonthday[DYN_RRULE_MDAY_CAP];
    int n_bymonthday;
    int16_t bynmonthday[DYN_RRULE_MDAY_CAP];
    int n_bynmonthday;
    int16_t byyearday[DYN_RRULE_YDAY_CAP];
    int n_byyearday;
    int16_t byweekno[DYN_RRULE_WKNO_CAP];
    int n_byweekno;
    int16_t bysetpos[DYN_RRULE_SETPOS_CAP];
    int n_bysetpos;
    int16_t byweekday[DYN_RRULE_WD_CAP];
    int n_byweekday;
    int16_t bynwd[DYN_RRULE_BYNWD_CAP * 2];
    int n_bynwd;
    uint8_t need_mmask, need_mday, need_nmday, need_wday, need_wno, need_nwd;
    uint8_t bits_ok;
    uint64_t bits_month, bits_wd, bits_mday, bits_nmday;
} DynRRule;

enum {
    DYN_RR_LIST_BYMONTH = 0,
    DYN_RR_LIST_BYMONTHDAY,
    DYN_RR_LIST_BYYEARDAY,
    DYN_RR_LIST_BYWEEKNO,
    DYN_RR_LIST_BYSETPOS,
};

typedef int (*DynRRuleYield)(int64_t sec, void* ud);

#define DYN_RRULE_YEAR_ARR 373

typedef struct DynRRuleGen {
    const DynRRule* rr;
    int64_t year;
    int month, day, hour, minute, second, weekday;
    int64_t yearordinal;
    int yearlen, nextyearlen, yearweekday;
    int lastyear, lastmonth;
    int mrange[13];
    uint8_t mmask[DYN_RRULE_YEAR_ARR];
    int16_t mdaymask[DYN_RRULE_YEAR_ARR];
    int16_t nmdaymask[DYN_RRULE_YEAR_ARR];
    uint8_t wdaymask[385];
    uint8_t wnomask[DYN_RRULE_YEAR_ARR];
    uint8_t nwdaymask[DYN_RRULE_YEAR_ARR];
    int n_timeset;
    int32_t timeset[4];
    uint32_t steps;
    uint64_t work;
    uint8_t exhausted;
    uint8_t limit;
    uint8_t bounded;
    int64_t lo_day, hi_day, stop_day;
} DynRRuleGen;

enum {
    DYN_RRULE_OK = 0,
    DYN_RRULE_E_DTSTART_YEAR = -1,
    DYN_RRULE_E_UNTIL_YEAR = -2
};

int dyn_rrule_set_add(int16_t* a, int* n, int cap, int v);

char dyn_rrule_upper(char c);

int dyn_rrule_parse_datetime(const char* s, size_t len, int64_t* out);

int dyn_rrule_compile(DynRRule* rr, const DynRRuleSpec* s, int64_t now, int64_t* bad_year);

const char* dyn_rrule_limit_desc(const DynRRuleGen* g);

void dyn_rrule_gen_init(DynRRuleGen* g, const DynRRule* rr,
    int64_t cursor_sec);

void dyn_rrule_gen_seek(DynRRuleGen* g, const DynRRule* rr, int64_t t);

void dyn_rrule_gen_loop(DynRRuleGen* g, DynRRuleYield fn, void* ud);

typedef struct {
    const char* name;
    const char* const* months;
    const char* const* months_abbr;
    const char* const* days;
    int day_first;
} DynDateLocale;

typedef struct {
    const DynDateLocale* loc;
    int64_t base;
    int has_base;
} DynDateParser;

const DynDateLocale* dyn_dp_locale_find(const char* name);


#endif
