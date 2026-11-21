#ifndef DYN_STATK_INT_H
#define DYN_STATK_INT_H

#define _REENTRANT
#include <math.h>

#include "../dyn-statk.h"
#include "../dyn-mathx.h"
#include "../dyn-cdefs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

int dyn_st_sort_copy(const double* x, uint32_t n, double* out);
double dyn_st_p_from_t(double t, double df, dyn_st_alt_t alt);
double dyn_st_p_from_z(double z, dyn_st_alt_t alt);

static inline double dyn_st_lgamma(double x)
{
    return dyn_lgamma_cached(x);
}

static inline int dyn_st_has_nan(const double* x, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (isnan(x[i]))
            return 1;
    return 0;
}

#endif
