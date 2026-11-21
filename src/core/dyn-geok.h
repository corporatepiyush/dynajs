#ifndef DYN_GEOK_H
#define DYN_GEOK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DYN_GEO_OK = 0,
    DYN_GEO_E_DOMAIN = -1,
    DYN_GEO_E_NOCONV = -2,
    DYN_GEO_E_OOM = -3
};

#define DYN_GEO_MEAN_RADIUS 6371008.8
#define DYN_GEO_WGS84_A 6378137.0
#define DYN_GEO_WGS84_F (1.0 / 298.257223563)
#define DYN_GEO_HASH_MAX 12

typedef struct {
    double lat, lon;
} dyn_geo_pt_t;

double dyn_geo_haversine(double lat1, double lon1, double lat2, double lon2, double radius);
void dyn_geo_haversine_many(double lat1, double lon1, const double* lat2, const double* lon2, size_t n, double radius,
    double* out);
size_t dyn_geo_haversine_within(double lat1, double lon1, const double* lat2, const double* lon2, size_t n, double maxd,
    double radius, uint32_t* idx, double* dist);
double dyn_geo_bearing(double lat1, double lon1, double lat2, double lon2);
dyn_geo_pt_t dyn_geo_destination(double lat, double lon, double bearing, double dist, double radius);
dyn_geo_pt_t dyn_geo_midpoint(double lat1, double lon1, double lat2, double lon2);
void dyn_geo_cross_track(double lat, double lon, double lat1, double lon1, double lat2, double lon2, double radius,
    double* xt, double* at);

int dyn_geo_vincenty_inverse(double lat1, double lon1, double lat2, double lon2, double* dist, double* az1,
    double* az2);
int dyn_geo_vincenty_direct(double lat1, double lon1, double az1, double dist, double* lat2, double* lon2,
    double* az2);

int dyn_geo_hash_encode(double lat, double lon, unsigned precision, char* out);
int dyn_geo_hash_decode(const char* hash, size_t len, double* lat, double* lon, double* lat_err, double* lon_err);

int dyn_geo_point_in_ring(double lat, double lon, const double* rlat, const double* rlon, size_t n);
void dyn_geo_points_in_ring(const double* lat, const double* lon, size_t np, const double* rlat, const double* rlon,
    size_t n, uint8_t* out);
double dyn_geo_ring_area(const double* rlat, const double* rlon, size_t n, double radius);

#ifdef __cplusplus
}
#endif

#endif
