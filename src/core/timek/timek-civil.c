#include "timek.h"

void dyn_civil_from_days(int64_t z, int64_t* y, int* m, int* d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y += (*m <= 2);
}

int dyn_time_expect_digits(const char* s, size_t len, size_t pos,
    int n, int64_t* out)
{
    int64_t v = 0;
    int i;
    if (pos + (size_t)n > len)
        return -1;
    for (i = 0; i < n; i++) {
        char c = s[pos + (size_t)i];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return 0;
}
