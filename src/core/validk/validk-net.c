#include "validk.h"

int dyn_v_ipv4(const char* s, size_t n)
{
    size_t i = 0, f;

    if (n < 7 || n > 15)
        return 0;
    for (f = 0; f < 4; f++) {
        size_t start = i, d;
        int v = 0;
        while (i < n && dyn_v_digit((unsigned char)s[i]))
            i++;
        d = i - start;
        if (d == 0 || d > 3)
            return 0;
        if (d > 1 && s[start] == '0')
            return 0;
        for (d = start; d < i; d++)
            v = v * 10 + (s[d] - '0');
        if (v > 255)
            return 0;
        if (f < 3) {
            if (i >= n || s[i] != '.')
                return 0;
            i++;
        }
    }
    return i == n;
}

static int dyn_v_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int dyn_v_datestring(const char* s, size_t n)
{
    static const int MDAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y, m, d;
    size_t i;

    if (n != 10)
        return 0;
    for (i = 0; i < n; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (!dyn_v_digit((unsigned char)s[i])) {
            return 0;
        }
    }
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    if (m < 1 || m > 12 || d < 1)
        return 0;
    if (m == 2 && dyn_v_leap(y))
        return d <= 29;
    return d <= MDAYS[m - 1];
}

