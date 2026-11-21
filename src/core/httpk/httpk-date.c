#include "httpk.h"

static int64_t httpk_days_from_civil(int64_t y, int m, int d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int httpk_month(const char* s)
{
    static const char mon[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int i;
    for (i = 0; i < 12; i++)
        if (httpk_lc(s[0]) == mon[i * 3] && httpk_lc(s[1]) == mon[i * 3 + 1]
            && httpk_lc(s[2]) == mon[i * 3 + 2])
            return i + 1;
    return 0;
}

static int httpk_dd(const char* s, size_t k)
{
    int v = 0;
    size_t i;
    for (i = 0; i < k; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int64_t httpk_mk(int y, int mo, int d, int hh, int mi, int ss)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (y < 1601 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] + (mo == 2 && leap)
        || hh > 23 || mi > 59 || ss > 60 || hh < 0 || mi < 0 || ss < 0)
        return -1;
    return httpk_days_from_civil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
}

static int httpk_hms(const char* s, int* hh, int* mi, int* ss)
{
    if (s[2] != ':' || s[5] != ':')
        return -1;
    *hh = httpk_dd(s, 2);
    *mi = httpk_dd(s + 3, 2);
    *ss = httpk_dd(s + 6, 2);
    return (*hh < 0 || *mi < 0 || *ss < 0) ? -1 : 0;
}

int64_t httpk_http_date(const char* s, size_t n)
{
    const char* comma;
    int d, mo, y, hh, mi, ss;
    if (!s)
        return -1;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    comma = (const char*)memchr(s, ',', n);
    if (comma && (size_t)(comma - s) == 3) {
        const char* p = comma + 1;
        size_t k = n - 4;
        if (k != 25 && k != 24)
            return -1;
        if (p[0] != ' ' || p[3] != ' ' || p[7] != ' ' || p[12] != ' ' || p[21] != ' ')
            return -1;
        if (!((k == 25 && memcmp(p + 22, "GMT", 3) == 0)
                || (k == 24 && memcmp(p + 22, "UT", 2) == 0)))
            return -1;
        d = httpk_dd(p + 1, 2);
        mo = httpk_month(p + 4);
        y = httpk_dd(p + 8, 4);
        if (d < 0 || !mo || y < 0 || httpk_hms(p + 13, &hh, &mi, &ss) < 0)
            return -1;
        return httpk_mk(y, mo, d, hh, mi, ss);
    }
    if (comma) {
        const char* p = comma + 1;
        size_t k = n - (size_t)(p - s);
        if (k != 23 || p[0] != ' ' || p[3] != '-' || p[7] != '-' || p[10] != ' '
            || p[19] != ' ' || memcmp(p + 20, "GMT", 3) != 0)
            return -1;
        d = httpk_dd(p + 1, 2);
        mo = httpk_month(p + 4);
        y = httpk_dd(p + 8, 2);
        if (d < 0 || !mo || y < 0 || httpk_hms(p + 11, &hh, &mi, &ss) < 0)
            return -1;
        y += y < 70 ? 2000 : 1900;
        return httpk_mk(y, mo, d, hh, mi, ss);
    }
    if (n == 24 && s[3] == ' ' && s[7] == ' ' && s[10] == ' ' && s[19] == ' ') {
        d = s[8] == ' ' ? httpk_dd(s + 9, 1) : httpk_dd(s + 8, 2);
        mo = httpk_month(s + 4);
        y = httpk_dd(s + 20, 4);
        if (d < 0 || !mo || y < 0 || httpk_hms(s + 11, &hh, &mi, &ss) < 0)
            return -1;
        return httpk_mk(y, mo, d, hh, mi, ss);
    }
    return -1;
}

static int httpk_cdelim(unsigned char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2f) || (c >= 0x3b && c <= 0x40)
        || (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
}

static size_t httpk_digits(const char* t, size_t n, size_t lo, size_t hi,
    int* v)
{
    size_t i = 0;
    int x = 0;
    while (i < n && i < hi && t[i] >= '0' && t[i] <= '9')
        x = x * 10 + (t[i++] - '0');
    if (i < lo || (i < n && t[i] >= '0' && t[i] <= '9'))
        return 0;
    *v = x;
    return i;
}

int64_t httpk_cookie_date(const char* s, size_t n)
{
    int ft = 0, fd = 0, fm = 0, fy = 0;
    int hh = 0, mi = 0, ss = 0, d = 0, mo = 0, y = 0;
    size_t i = 0;
    while (i < n) {
        size_t b, tl;
        while (i < n && httpk_cdelim((unsigned char)s[i]))
            i++;
        b = i;
        while (i < n && !httpk_cdelim((unsigned char)s[i]))
            i++;
        tl = i - b;
        if (!tl)
            continue;
        {
            const char* t = s + b;
            size_t k;
            int a, c2, c3;
            if (!ft && (k = httpk_digits(t, tl, 1, 2, &a)) && k < tl && t[k] == ':') {
                size_t k2 = httpk_digits(t + k + 1, tl - k - 1, 1, 2, &c2);
                if (k2 && k + 1 + k2 < tl && t[k + 1 + k2] == ':'
                    && httpk_digits(t + k + 2 + k2, tl - k - 2 - k2, 1, 2, &c3)) {
                    ft = 1;
                    hh = a;
                    mi = c2;
                    ss = c3;
                    continue;
                }
            }
            if (!fd && httpk_digits(t, tl, 1, 2, &a)) {
                fd = 1;
                d = a;
                continue;
            }
            if (!fm && tl >= 3 && (a = httpk_month(t)) != 0) {
                fm = 1;
                mo = a;
                continue;
            }
            if (!fy && httpk_digits(t, tl, 2, 4, &a)) {
                fy = 1;
                y = a;
                continue;
            }
        }
    }
    if (!ft || !fd || !fm || !fy)
        return -1;
    if (y >= 70 && y <= 99)
        y += 1900;
    else if (y >= 0 && y <= 69)
        y += 2000;
    if (d < 1 || d > 31 || y < 1601 || hh > 23 || mi > 59 || ss > 59)
        return -1;
    return httpk_mk(y, mo, d, hh, mi, ss);
}

double httpk_delta_seconds(const char* s, size_t n)
{
    double v = 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= n || s[i] < '0' || s[i] > '9')
        return -1;
    while (i < n && s[i] >= '0' && s[i] <= '9')
        v = v * 10 + (s[i++] - '0');
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i == n ? v : -1;
}

size_t httpk_imf_fixdate(int64_t t, char* out)
{
    static const char wd[] = "ThuFriSatSunMonTueWed";
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int64_t days, sod, z, era, doe, yoe, y, doy, mp;
    int d, m, w, hh, mi, ss;
    if (t < -11644473600LL || t > 253402300799LL)
        return 0;
    days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    sod = t - days * 86400;
    w = (int)(((days % 7) + 7) % 7);
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    hh = (int)(sod / 3600);
    mi = (int)(sod / 60 % 60);
    ss = (int)(sod % 60);
    memcpy(out, wd + w * 3, 3);
    out[3] = ',';
    out[4] = ' ';
    out[5] = (char)('0' + d / 10);
    out[6] = (char)('0' + d % 10);
    out[7] = ' ';
    memcpy(out + 8, mon + (m - 1) * 3, 3);
    out[11] = ' ';
    out[12] = (char)('0' + y / 1000);
    out[13] = (char)('0' + y / 100 % 10);
    out[14] = (char)('0' + y / 10 % 10);
    out[15] = (char)('0' + y % 10);
    out[16] = ' ';
    out[17] = (char)('0' + hh / 10);
    out[18] = (char)('0' + hh % 10);
    out[19] = ':';
    out[20] = (char)('0' + mi / 10);
    out[21] = (char)('0' + mi % 10);
    out[22] = ':';
    out[23] = (char)('0' + ss / 10);
    out[24] = (char)('0' + ss % 10);
    memcpy(out + 25, " GMT", 4);
    return HTTPK_IMF_FIXDATE_LEN;
}

static _Thread_local int64_t httpk_dl_sec = INT64_MIN;
static _Thread_local char httpk_dl_buf[HTTPK_DATE_LINE_LEN + 1];
static _Thread_local uint64_t httpk_dl_builds;

const char* httpk_date_line(int64_t now)
{
    if (now != httpk_dl_sec) {
        memcpy(httpk_dl_buf, "Date: ", 6);
        if (!httpk_imf_fixdate(now, httpk_dl_buf + 6))
            return NULL;
        httpk_dl_buf[6 + HTTPK_IMF_FIXDATE_LEN] = '\r';
        httpk_dl_buf[7 + HTTPK_IMF_FIXDATE_LEN] = '\n';
        httpk_dl_buf[HTTPK_DATE_LINE_LEN] = 0;
        httpk_dl_sec = now;
        httpk_dl_builds++;
    }
    return httpk_dl_buf;
}

uint64_t httpk_date_line_builds(void)
{
    return httpk_dl_builds;
}
