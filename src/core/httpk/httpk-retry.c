#include "httpk.h"
#include "../dyn-prng.h"

int httpk_retry_status(int status, int policy)
{
    if (policy == HTTPK_RETRY_API)
        return status == 408 || status == 409 || status == 429 || status >= 500;
    return status == 429 || (status >= 500 && status < 600);
}

double httpk_backoff_ms(double base_ms, double cap_ms, int attempt,
    uint64_t* rng)
{
    double back, r;
    if (attempt < 0)
        attempt = 0;
    back = base_ms * (double)(1u << (attempt < 10 ? attempt : 10));
    r = (double)(dyn_splitmix64(rng) >> 11) / 9007199254740992.0;
    back *= 0.75 + 0.5 * r;
    if (cap_ms > 0 && back > cap_ms)
        back = cap_ms;
    return back;
}

double httpk_retry_after_ms(const char* retry_after, size_t ra_n,
    const char* retry_after_ms, size_t ram_n, int64_t now_s)
{
    if (retry_after_ms && ram_n) {
        char tmp[32];
        char* end;
        double v;
        size_t i = 0;
        while (i < ram_n && (retry_after_ms[i] == ' ' || retry_after_ms[i] == '\t'))
            i++;
        if (ram_n - i < sizeof tmp && i < ram_n && retry_after_ms[i] >= '0' && retry_after_ms[i] <= '9') {
            memcpy(tmp, retry_after_ms + i, ram_n - i);
            tmp[ram_n - i] = 0;
            v = strtod(tmp, &end);
            while (*end == ' ' || *end == '\t')
                end++;
            if (!*end && v > 0)
                return v;
        }
    }
    if (retry_after && ra_n) {
        double secs = httpk_delta_seconds(retry_after, ra_n);
        if (secs >= 0)
            return secs * 1000.0;
        {
            int64_t when = httpk_http_date(retry_after, ra_n);
            if (when >= 0)
                return when > now_s ? (double)(when - now_s) * 1000.0 : 0.0;
        }
    }
    return -1;
}
