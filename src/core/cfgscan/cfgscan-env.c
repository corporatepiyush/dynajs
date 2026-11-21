#include "cfgscan.h"

void dyn_env_read_value(const char* s, size_t n, size_t* i, size_t* vb,
    size_t* ve, char* q)
{
    size_t k = *i;
    char qc = 0;
    while (k < n && (s[k] == ' ' || s[k] == '\t'))
        k++;
    if (k < n && (s[k] == '"' || s[k] == '\'')) {
        qc = s[k];
        k++;
        *vb = k;
        if (qc == '"') {
            while (k < n && s[k] != '"') {
                if (s[k] == '\\' && k + 1 < n)
                    k++;
                k++;
            }
        } else {
            const char* e = (const char*)memchr(s + k, '\'', n - k);
            k = e ? (size_t)(e - s) : n;
        }
        *ve = k;
        if (k < n)
            k++;
    } else {
        size_t b = k, e;
        while (k < n && s[k] != '\n' && s[k] != '#')
            k++;
        e = k;
        dyn_trim(s, &b, &e);
        *vb = b;
        *ve = e;
    }
    if (k < n) {
        const char* nl = (const char*)memchr(s + k, '\n', n - k);
        k = nl ? (size_t)(nl - s) : n;
    }
    *q = qc;
    *i = k;
}

int dyn_env_key_ok(const char* k, size_t n)
{
    return n > 0 && n <= DYN_ENV_KEY_MAX && strlen(k) == n && memchr(k, '=', n) == NULL;
}

int dyn_ini_needs_quotes(const char* s, size_t n)
{
    size_t i;
    if (n == 0 || s[0] == ' ' || s[n - 1] == ' ')
        return 1;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ';' || c == '#' || c == '=' || c == '"' || c == '\'' || c == '\n' || c == '\r' || c == '\t' || c == '\\')
            return 1;
    }
    return 0;
}
