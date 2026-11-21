#include "metrics-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_METRICS)

_Alignas(64) met_hot_t met_hot[MET_MAX];

met_cold_t met_cold[MET_MAX];

_Atomic uint32_t met_used;

pthread_mutex_t met_lock = PTHREAD_MUTEX_INITIALIZER;

int met_name_ok(const char* s, size_t n)
{
    size_t i;

    if (n == 0 || n >= MET_NAME)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || c == '_' || c == ':';
        if (!(alpha || (i > 0 && c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

#endif
