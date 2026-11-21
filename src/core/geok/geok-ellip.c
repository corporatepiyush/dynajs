#include "geok-int.h"

#include <string.h>

#define GEO_B (DYN_GEO_WGS84_A * (1.0 - DYN_GEO_WGS84_F))
#define GEO_ITER 200
#define GEO_EPS 1e-12

static void geo_ab(double cos2a, double* A, double* B)
{
    double u2 = cos2a * (DYN_GEO_WGS84_A * DYN_GEO_WGS84_A - GEO_B * GEO_B) / (GEO_B * GEO_B);
    *A = 1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0 + u2 * (320.0 - 175.0 * u2)));
    *B = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - 47.0 * u2)));
}

static double geo_dsigma(double B, double ss, double cs, double c2m)
{
    return B * ss
        * (c2m + B / 4.0 * (cs * (-1.0 + 2.0 * c2m * c2m) - B / 6.0 * c2m * (-3.0 + 4.0 * ss * ss) * (-3.0 + 4.0 * c2m * c2m)));
}

int dyn_geo_vincenty_inverse(double lat1, double lon1, double lat2, double lon2, double* dist, double* az1, double* az2)
{
    const double f = DYN_GEO_WGS84_F;
    double L = geo_wrap180(lon2 - lon1) * GEO_RAD;
    double U1 = atan((1.0 - f) * tan(lat1 * GEO_RAD)), U2 = atan((1.0 - f) * tan(lat2 * GEO_RAD));
    double sU1 = sin(U1), cU1 = cos(U1), sU2 = sin(U2), cU2 = cos(U2);
    double lam = L, sl = 0.0, cl = 1.0, ss = 0.0, cs = 1.0, sig = 0.0, ca2 = 1.0, c2m = 0.0, A, B;
    int it;
    for (it = 0; it < GEO_ITER; it++) {
        double sa, C, prev = lam;
        sl = sin(lam);
        cl = cos(lam);
        ss = hypot(cU2 * sl, cU1 * sU2 - sU1 * cU2 * cl);
        if (ss < 1e-15) {
            *dist = 0.0;
            *az1 = 0.0;
            *az2 = 0.0;
            return DYN_GEO_OK;
        }
        cs = sU1 * sU2 + cU1 * cU2 * cl;
        sig = atan2(ss, cs);
        sa = cU1 * cU2 * sl / ss;
        ca2 = 1.0 - sa * sa;
        c2m = ca2 != 0.0 ? cs - 2.0 * sU1 * sU2 / ca2 : 0.0;
        C = f / 16.0 * ca2 * (4.0 + f * (4.0 - 3.0 * ca2));
        lam = L + (1.0 - C) * f * sa * (sig + C * ss * (c2m + C * cs * (-1.0 + 2.0 * c2m * c2m)));
        if (fabs(lam) > M_PI + 1e-9)
            return DYN_GEO_E_NOCONV;
        if (fabs(lam - prev) < GEO_EPS)
            break;
    }
    if (it == GEO_ITER)
        return DYN_GEO_E_NOCONV;
    geo_ab(ca2, &A, &B);
    *dist = GEO_B * A * (sig - geo_dsigma(B, ss, cs, c2m));
    *az1 = geo_wrap360(atan2(cU2 * sl, cU1 * sU2 - sU1 * cU2 * cl) * GEO_DEG);
    *az2 = geo_wrap360(atan2(cU1 * sl, -sU1 * cU2 + cU1 * sU2 * cl) * GEO_DEG);
    return DYN_GEO_OK;
}

int dyn_geo_vincenty_direct(double lat1, double lon1, double azi, double dist, double* lat2, double* lon2, double* az2)
{
    const double f = DYN_GEO_WGS84_F;
    double a1 = azi * GEO_RAD, sa1 = sin(a1), ca1 = cos(a1);
    double tU1 = (1.0 - f) * tan(lat1 * GEO_RAD), cU1 = 1.0 / sqrt(1.0 + tU1 * tU1), sU1 = tU1 * cU1;
    double s1 = atan2(tU1, ca1), sa = cU1 * sa1, ca2 = 1.0 - sa * sa, A, B, sig, ss = 0.0, cs = 1.0, c2m = 0.0, tmp, lam,
           C, L;
    int it;
    geo_ab(ca2, &A, &B);
    sig = dist / (GEO_B * A);
    for (it = 0; it < GEO_ITER; it++) {
        double prev = sig;
        c2m = cos(2.0 * s1 + sig);
        ss = sin(sig);
        cs = cos(sig);
        sig = dist / (GEO_B * A) + geo_dsigma(B, ss, cs, c2m);
        if (fabs(sig - prev) < GEO_EPS)
            break;
    }
    if (it == GEO_ITER)
        return DYN_GEO_E_NOCONV;
    c2m = cos(2.0 * s1 + sig);
    ss = sin(sig);
    cs = cos(sig);
    tmp = sU1 * ss - cU1 * cs * ca1;
    *lat2 = atan2(sU1 * cs + cU1 * ss * ca1, (1.0 - f) * hypot(sa, tmp)) * GEO_DEG;
    lam = atan2(ss * sa1, cU1 * cs - sU1 * ss * ca1);
    C = f / 16.0 * ca2 * (4.0 + f * (4.0 - 3.0 * ca2));
    L = lam - (1.0 - C) * f * sa * (sig + C * ss * (c2m + C * cs * (-1.0 + 2.0 * c2m * c2m)));
    *lon2 = geo_wrap180(lon1 + L * GEO_DEG);
    *az2 = geo_wrap360(atan2(sa, -tmp) * GEO_DEG);
    return DYN_GEO_OK;
}

static const char geo_b32[] = "0123456789bcdefghjkmnpqrstuvwxyz";

int dyn_geo_hash_encode(double lat, double lon, unsigned precision, char* out)
{
    double la0 = -90.0, la1 = 90.0, lo0 = -180.0, lo1 = 180.0;
    unsigned i, b, bit = 0;
    if (precision == 0 || precision > DYN_GEO_HASH_MAX || !(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0))
        return DYN_GEO_E_DOMAIN;
    for (i = 0; i < precision; i++) {
        unsigned ch = 0;
        for (b = 0; b < 5; b++, bit++) {
            ch <<= 1;
            if ((bit & 1u) == 0) {
                double m = 0.5 * (lo0 + lo1);
                if (lon >= m) {
                    ch |= 1u;
                    lo0 = m;
                } else
                    lo1 = m;
            } else {
                double m = 0.5 * (la0 + la1);
                if (lat >= m) {
                    ch |= 1u;
                    la0 = m;
                } else
                    la1 = m;
            }
        }
        out[i] = geo_b32[ch];
    }
    out[precision] = '\0';
    return DYN_GEO_OK;
}

int dyn_geo_hash_decode(const char* hash, size_t len, double* lat, double* lon, double* lat_err, double* lon_err)
{
    double la0 = -90.0, la1 = 90.0, lo0 = -180.0, lo1 = 180.0;
    size_t i;
    unsigned b, bit = 0;
    if (len == 0 || len > DYN_GEO_HASH_MAX)
        return DYN_GEO_E_DOMAIN;
    for (i = 0; i < len; i++) {
        char c = hash[i] >= 'A' && hash[i] <= 'Z' ? (char)(hash[i] - 'A' + 'a') : hash[i];
        const char* p = c ? strchr(geo_b32, c) : NULL;
        unsigned v;
        if (!p)
            return DYN_GEO_E_DOMAIN;
        v = (unsigned)(p - geo_b32);
        for (b = 0; b < 5; b++, bit++) {
            unsigned on = (v >> (4 - b)) & 1u;
            if ((bit & 1u) == 0) {
                double m = 0.5 * (lo0 + lo1);
                if (on)
                    lo0 = m;
                else
                    lo1 = m;
            } else {
                double m = 0.5 * (la0 + la1);
                if (on)
                    la0 = m;
                else
                    la1 = m;
            }
        }
    }
    *lat = 0.5 * (la0 + la1);
    *lon = 0.5 * (lo0 + lo1);
    *lat_err = 0.5 * (la1 - la0);
    *lon_err = 0.5 * (lo1 - lo0);
    return DYN_GEO_OK;
}
