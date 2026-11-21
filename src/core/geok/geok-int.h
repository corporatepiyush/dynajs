#ifndef DYN_GEOK_INT_H
#define DYN_GEOK_INT_H

#include <math.h>

#include "../dyn-geok.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GEO_RAD (M_PI / 180.0)
#define GEO_DEG (180.0 / M_PI)

static inline double geo_wrap180(double d)
{
    d = fmod(d + 180.0, 360.0);
    if (d < 0.0)
        d += 360.0;
    return d - 180.0;
}

static inline double geo_wrap360(double d)
{
    d = fmod(d, 360.0);
    if (d < 0.0)
        d += 360.0;
    return d >= 360.0 ? 0.0 : d;
}

#endif
