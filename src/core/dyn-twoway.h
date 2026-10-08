#ifndef DYN_TWOWAY_H
#define DYN_TWOWAY_H

#include <stddef.h>
#include <stdint.h>

#define DYN_TWOWAY_FAIL_FLOOR 32
#define DYN_TWOWAY_WORK_RATIO 8
#define DYN_TWOWAY_SCAN_SLACK 64

static inline int dyn_twoway_over_budget(size_t fails, size_t scanned, size_t m)
{
    return fails > DYN_TWOWAY_FAIL_FLOOR && fails * m > DYN_TWOWAY_WORK_RATIO * (scanned + DYN_TWOWAY_SCAN_SLACK);
}

#define DYN_TWOWAY_DEFINE(NAME, PAT_T, TEXT_T, PAT_AT, TEXT_AT)                              \
    static ptrdiff_t NAME##_maxsuf(PAT_T x, ptrdiff_t m, ptrdiff_t* p, int rev)              \
    {                                                                                        \
        ptrdiff_t ms = -1, j = 0, k = 1;                                                     \
        *p = 1;                                                                              \
        while (j + k < m) {                                                                  \
            uint32_t a = (uint32_t)PAT_AT(x, j + k), b = (uint32_t)PAT_AT(x, ms + k);        \
            if (rev ? a > b : a < b) {                                                       \
                j += k;                                                                      \
                k = 1;                                                                       \
                *p = j - ms;                                                                 \
            } else if (a == b) {                                                             \
                if (k != *p) {                                                               \
                    k++;                                                                     \
                } else {                                                                     \
                    j += *p;                                                                 \
                    k = 1;                                                                   \
                }                                                                            \
            } else {                                                                         \
                ms = j;                                                                      \
                j = ms + 1;                                                                  \
                k = *p = 1;                                                                  \
            }                                                                                \
        }                                                                                    \
        return ms;                                                                           \
    }                                                                                        \
    static size_t NAME(TEXT_T y, size_t n_, PAT_T x, size_t m_)                              \
    {                                                                                        \
        ptrdiff_t n = (ptrdiff_t)n_, m = (ptrdiff_t)m_;                                      \
        ptrdiff_t i, j, k, ell, per, p, q, memory;                                           \
        int periodic = 1;                                                                    \
        if (m == 0)                                                                          \
            return 0;                                                                        \
        if (m > n)                                                                           \
            return SIZE_MAX;                                                                 \
        i = NAME##_maxsuf(x, m, &p, 0);                                                      \
        j = NAME##_maxsuf(x, m, &q, 1);                                                      \
        if (i > j) {                                                                         \
            ell = i;                                                                         \
            per = p;                                                                         \
        } else {                                                                             \
            ell = j;                                                                         \
            per = q;                                                                         \
        }                                                                                    \
        for (k = 0; k <= ell && k + per < m; k++) {                                          \
            if (PAT_AT(x, k) != PAT_AT(x, k + per)) {                                        \
                periodic = 0;                                                                \
                break;                                                                       \
            }                                                                                \
        }                                                                                    \
        j = 0;                                                                               \
        if (periodic) {                                                                      \
            memory = -1;                                                                     \
            while (j <= n - m) {                                                             \
                i = (ell > memory ? ell : memory) + 1;                                       \
                while (i < m && PAT_AT(x, i) == TEXT_AT(y, i + j))                           \
                    i++;                                                                     \
                if (i >= m) {                                                                \
                    i = ell;                                                                 \
                    while (i > memory && PAT_AT(x, i) == TEXT_AT(y, i + j))                  \
                        i--;                                                                 \
                    if (i <= memory)                                                         \
                        return (size_t)j;                                                    \
                    j += per;                                                                \
                    memory = m - per - 1;                                                    \
                } else {                                                                     \
                    j += i - ell;                                                            \
                    memory = -1;                                                             \
                }                                                                            \
            }                                                                                \
        } else {                                                                             \
            per = (ell + 1 > m - ell - 1 ? ell + 1 : m - ell - 1) + 1;                       \
            while (j <= n - m) {                                                             \
                i = ell + 1;                                                                 \
                while (i < m && PAT_AT(x, i) == TEXT_AT(y, i + j))                           \
                    i++;                                                                     \
                if (i >= m) {                                                                \
                    i = ell;                                                                 \
                    while (i >= 0 && PAT_AT(x, i) == TEXT_AT(y, i + j))                      \
                        i--;                                                                 \
                    if (i < 0)                                                               \
                        return (size_t)j;                                                    \
                    j += per;                                                                \
                } else {                                                                     \
                    j += i - ell;                                                            \
                }                                                                            \
            }                                                                                \
        }                                                                                    \
        return SIZE_MAX;                                                                     \
    }

#endif
