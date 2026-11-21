#include "geok-int.h"
#include "../dyn-par.h"

#include <string.h>

typedef struct {
    double x, y, z;
} geo_v3;

static geo_v3 geo_vec(double lat, double lon)
{
    double p = lat * GEO_RAD, l = lon * GEO_RAD, c = cos(p);
    geo_v3 v = { c * cos(l), c * sin(l), sin(p) };
    return v;
}

static geo_v3 geo_cross(geo_v3 a, geo_v3 b)
{
    geo_v3 c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return c;
}

static double geo_dot(geo_v3 a, geo_v3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double geo_norm(geo_v3 a)
{
    return sqrt(geo_dot(a, a));
}

double dyn_geo_haversine(double lat1, double lon1, double lat2, double lon2, double radius)
{
    double p1 = lat1 * GEO_RAD, p2 = lat2 * GEO_RAD;
    double sp = sin(0.5 * (p2 - p1)), sl = sin(0.5 * (lon2 - lon1) * GEO_RAD);
    double a = sp * sp + cos(p1) * cos(p2) * sl * sl;
    if (a > 1.0)
        a = 1.0;
    return 2.0 * radius * atan2(sqrt(a), sqrt(1.0 - a));
}

typedef struct {
    double lat1, lon1, radius;
    const double *lat2, *lon2;
    double* out;
    size_t n;
} geo_many_t;

static void geo_many_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    geo_many_t* j = (geo_many_t*)arg;
    size_t lo = j->n * w / nw, hi = j->n * (w + 1) / nw, i;
    double p1 = j->lat1 * GEO_RAD, c1 = cos(p1);
    (void)par;
    for (i = lo; i < hi; i++) {
        double p2 = j->lat2[i] * GEO_RAD, sp = sin(0.5 * (p2 - p1)), sl = sin(0.5 * (j->lon2[i] - j->lon1) * GEO_RAD);
        double a = sp * sp + c1 * cos(p2) * sl * sl;
        if (a > 1.0)
            a = 1.0;
        j->out[i] = 2.0 * j->radius * atan2(sqrt(a), sqrt(1.0 - a));
    }
}

void dyn_geo_haversine_many(double lat1, double lon1, const double* lat2, const double* lon2, size_t n, double radius,
    double* out)
{
    geo_many_t j = { lat1, lon1, radius, lat2, lon2, out, n };
    unsigned nw = dyn_par_plan(n, 1u << 15, 0);
    dyn_par_run(nw ? nw : 1, geo_many_worker, &j);
}

typedef struct {
    double lat1, lon1, radius, maxd, hcut;
    const double *lat2, *lon2;
    uint32_t* idx;
    double* dist;
    size_t n;
    size_t cnt[64];
} geo_within_t;

static void geo_within_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    geo_within_t* j = (geo_within_t*)arg;
    size_t lo = j->n * w / nw, hi = j->n * (w + 1) / nw, i, k = lo;
    double p1 = j->lat1 * GEO_RAD, c1 = cos(p1);
    (void)par;
    for (i = lo; i < hi; i++) {
        double p2 = j->lat2[i] * GEO_RAD, sp = sin(0.5 * (p2 - p1)), sl = sin(0.5 * (j->lon2[i] - j->lon1) * GEO_RAD);
        double a = sp * sp + c1 * cos(p2) * sl * sl, d;
        if (a > j->hcut)
            continue;
        if (a > 1.0)
            a = 1.0;
        d = 2.0 * j->radius * atan2(sqrt(a), sqrt(1.0 - a));
        if (d <= j->maxd) {
            j->idx[k] = (uint32_t)i;
            j->dist[k++] = d;
        }
    }
    j->cnt[w] = k - lo;
}

size_t dyn_geo_haversine_within(double lat1, double lon1, const double* lat2, const double* lon2, size_t n, double maxd,
    double radius, uint32_t* idx, double* dist)
{
    geo_within_t j;
    unsigned nw, w;
    size_t total = 0;
    double h = maxd / (2.0 * radius);
    memset(&j, 0, sizeof j);
    j.lat1 = lat1;
    j.lon1 = lon1;
    j.radius = radius;
    j.maxd = maxd;
    j.hcut = h >= 0.5 * M_PI ? HUGE_VAL : sin(h) * sin(h) * (1.0 + 1e-9) + 1e-300;
    j.lat2 = lat2;
    j.lon2 = lon2;
    j.idx = idx;
    j.dist = dist;
    j.n = n;
    if (!(maxd >= 0.0))
        return 0;
    nw = dyn_par_plan(n, 1u << 15, 64);
    nw = dyn_par_run(nw ? nw : 1, geo_within_worker, &j);
    for (w = 0; w < nw; w++) {
        size_t lo = n * w / nw;
        if (lo != total && j.cnt[w]) {
            memmove(idx + total, idx + lo, j.cnt[w] * sizeof *idx);
            memmove(dist + total, dist + lo, j.cnt[w] * sizeof *dist);
        }
        total += j.cnt[w];
    }
    return total;
}

double dyn_geo_bearing(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * GEO_RAD, p2 = lat2 * GEO_RAD, dl = (lon2 - lon1) * GEO_RAD;
    double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    return geo_wrap360(atan2(y, x) * GEO_DEG);
}

dyn_geo_pt_t dyn_geo_destination(double lat, double lon, double bearing, double dist, double radius)
{
    double p1 = lat * GEO_RAD, th = bearing * GEO_RAD, d = dist / radius;
    double sp2 = sin(p1) * cos(d) + cos(p1) * sin(d) * cos(th);
    double p2 = asin(fmax(-1.0, fmin(1.0, sp2)));
    double l = atan2(sin(th) * sin(d) * cos(p1), cos(d) - sin(p1) * sp2);
    dyn_geo_pt_t o = { p2 * GEO_DEG, geo_wrap180(lon + l * GEO_DEG) };
    return o;
}

dyn_geo_pt_t dyn_geo_midpoint(double lat1, double lon1, double lat2, double lon2)
{
    geo_v3 a = geo_vec(lat1, lon1), b = geo_vec(lat2, lon2);
    geo_v3 m = { a.x + b.x, a.y + b.y, a.z + b.z };
    dyn_geo_pt_t o;
    o.lat = atan2(m.z, hypot(m.x, m.y)) * GEO_DEG;
    o.lon = (m.x == 0.0 && m.y == 0.0) ? lon1 : atan2(m.y, m.x) * GEO_DEG;
    return o;
}

void dyn_geo_cross_track(double lat, double lon, double lat1, double lon1, double lat2, double lon2, double radius,
    double* xt, double* at)
{
    geo_v3 p1 = geo_vec(lat1, lon1), p2 = geo_vec(lat2, lon2), p3 = geo_vec(lat, lon);
    geo_v3 n = geo_cross(p1, p2), q;
    double nn = geo_norm(n), s;
    if (nn < 1e-12) {
        *xt = (double)NAN;
        *at = (double)NAN;
        return;
    }
    n.x /= nn;
    n.y /= nn;
    n.z /= nn;
    s = geo_dot(p3, n);
    q.x = p3.x - s * n.x;
    q.y = p3.y - s * n.y;
    q.z = p3.z - s * n.z;
    *xt = -radius * atan2(s, geo_norm(q));
    *at = radius * atan2(geo_dot(geo_cross(p1, q), n), geo_dot(p1, q));
}

int dyn_geo_point_in_ring(double lat, double lon, const double* rlat, const double* rlon, size_t n)
{
    size_t i, j;
    int in = 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        double yi = rlat[i], yj = rlat[j];
        int s = (yi > lat) != (yj > lat);
        double x = rlon[i] + (lat - yi) * (rlon[j] - rlon[i]) / (s ? yj - yi : 1.0);
        in ^= s & (lon < x);
    }
    return in;
}

typedef struct {
    const double *lat, *lon, *rlat, *rlon;
    size_t np, n;
    uint8_t* out;
} geo_pip_t;

static void geo_pip_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    geo_pip_t* j = (geo_pip_t*)arg;
    size_t lo = j->np * w / nw, hi = j->np * (w + 1) / nw, i;
    (void)par;
    for (i = lo; i < hi; i++)
        j->out[i] = (uint8_t)dyn_geo_point_in_ring(j->lat[i], j->lon[i], j->rlat, j->rlon, j->n);
}

void dyn_geo_points_in_ring(const double* lat, const double* lon, size_t np, const double* rlat, const double* rlon,
    size_t n, uint8_t* out)
{
    geo_pip_t j = { lat, lon, rlat, rlon, np, n, out };
    unsigned nw = dyn_par_plan(np * (n ? n : 1), 1u << 16, 0);
    dyn_par_run(nw ? nw : 1, geo_pip_worker, &j);
}

double dyn_geo_ring_area(const double* rlat, const double* rlon, size_t n, double radius)
{
    double e = 0.0, turn = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t k = i + 1 == n ? 0 : i + 1;
        double t1 = tan(0.5 * rlat[i] * GEO_RAD), t2 = tan(0.5 * rlat[k] * GEO_RAD);
        double dl = geo_wrap180(rlon[k] - rlon[i]) * GEO_RAD;
        turn += dl;
        e += 2.0 * atan2(tan(0.5 * dl) * (t1 + t2), 1.0 + t1 * t2);
    }
    e = fabs(e - 2.0 * M_PI * round(turn / (2.0 * M_PI)));
    if (e > 2.0 * M_PI)
        e = 4.0 * M_PI - e;
    return e * radius * radius;
}
