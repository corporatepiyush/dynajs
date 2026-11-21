#ifndef DS_BKD_H
#define DS_BKD_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-roaring-bitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_BKD_MAX_DIMS 4
#define DYN_BKD_MAX_BPD 16
#define DYN_BKD_MAX_KEY (DYN_BKD_MAX_DIMS * DYN_BKD_MAX_BPD)
#define DYN_BKD_DEFAULT_LEAF 512
#define DYN_BKD_MAX_LEAF 65536

#define DYN_BKD_OUTSIDE 0
#define DYN_BKD_INSIDE 1
#define DYN_BKD_CROSSES 2

#define DYN_BKD_EARTH_M 6371008.7714

typedef struct {
    uint32_t dims, bpd, kb, n, leaf, nleaves;
    uint8_t* keys;
    uint32_t* docs;
    uint8_t* split;
    uint8_t* sdim;
    uint8_t minp[DYN_BKD_MAX_KEY];
    uint8_t maxp[DYN_BKD_MAX_KEY];
} dyn_bkd_t;

typedef int (*dyn_bkd_rel_fn)(const void* q, const uint8_t* cmin, const uint8_t* cmax);
typedef int (*dyn_bkd_hit_fn)(const void* q, const uint8_t* key);

typedef struct {
    uint32_t* v;
    size_t n, cap;
} dyn_bkd_sink;

typedef struct {
    uint32_t kb, bpd, dims;
    uint8_t lo[DYN_BKD_MAX_KEY];
    uint8_t hi[DYN_BKD_MAX_KEY];
} dyn_bkd_range_q;

typedef struct {
    double min_lat, max_lat, min_lon, max_lon;
} dyn_bkd_geo_box_q;

typedef struct {
    double lat, lon, meters;
    double bmin_lat, bmax_lat, bmin_lon, bmax_lon;
    int wrap;
} dyn_bkd_geo_dist_q;

void dyn_bkd_free(dyn_bkd_t* t);
int dyn_bkd_build(dyn_bkd_t* t, uint32_t dims, uint32_t bpd, uint32_t leaf, const uint8_t* keys, const uint32_t* docs, uint32_t n);
int dyn_bkd_adopt(dyn_bkd_t* t, uint32_t dims, uint32_t bpd, uint32_t leaf, uint32_t n, uint8_t* keys, uint32_t* docs, uint8_t* split, uint8_t* sdim);
uint32_t dyn_bkd_leaf_start(const dyn_bkd_t* t, uint32_t leaf);
int dyn_bkd_query(const dyn_bkd_t* t, dyn_bkd_rel_fn rel, dyn_bkd_hit_fn hit, const void* q, dyn_bkd_sink* out);
int dyn_bkd_sink_to_roar(dyn_bkd_sink* s, dyn_roar_t* out);
void dyn_bkd_sink_free(dyn_bkd_sink* s);

int dyn_bkd_range_rel(const void* q, const uint8_t* cmin, const uint8_t* cmax);
int dyn_bkd_range_hit(const void* q, const uint8_t* key);
int dyn_bkd_geo_box_rel(const void* q, const uint8_t* cmin, const uint8_t* cmax);
int dyn_bkd_geo_box_hit(const void* q, const uint8_t* key);
void dyn_bkd_geo_dist_init(dyn_bkd_geo_dist_q* q, double lat, double lon, double meters);
int dyn_bkd_geo_dist_rel(const void* q, const uint8_t* cmin, const uint8_t* cmax);
int dyn_bkd_geo_dist_hit(const void* q, const uint8_t* key);
double dyn_bkd_haversine(double lat1, double lon1, double lat2, double lon2);

void dyn_bkd_enc_f64(double x, uint8_t* out);
double dyn_bkd_dec_f64(const uint8_t* p);
void dyn_bkd_enc_i64(int64_t x, uint8_t* out);
int64_t dyn_bkd_dec_i64(const uint8_t* p);

#ifdef __cplusplus
}
#endif

#endif
