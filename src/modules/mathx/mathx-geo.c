#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#include "core/dyn-geok.h"

#include <float.h>

static int mxg_num(JSContext* ctx, JSValueConst v, const char* fn, const char* what, double lo, double hi, double* out)
{
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "%s: %s must be a number", fn, what);
        return -1;
    }
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (!(*out >= lo && *out <= hi)) {
        JS_ThrowRangeError(ctx, "%s: %s must be in [%g, %g]", fn, what, lo, hi);
        return -1;
    }
    return 0;
}

static int mxg_lat(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* fn, const char* what, double* out)
{
    return mxg_num(ctx, i < argc ? argv[i] : JS_UNDEFINED, fn, what, -90.0, 90.0, out);
}

static int mxg_fin(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* fn, const char* what, double* out)
{
    return mxg_num(ctx, i < argc ? argv[i] : JS_UNDEFINED, fn, what, -DBL_MAX, DBL_MAX, out);
}

static int mxg_radius(JSContext* ctx, JSValueConst opts, const char* fn, double* r)
{
    static const char* const keys[] = { "radius" };
    if (mxt_opts_check(ctx, opts, fn, keys, 1) || mxt_num(ctx, opts, fn, "radius", DYN_GEO_MEAN_RADIUS, r))
        return -1;
    if (!(*r > 0.0) || !isfinite(*r)) {
        JS_ThrowRangeError(ctx, "%s: radius must be positive and finite", fn);
        return -1;
    }
    return 0;
}

static int mxg_pts(JSContext* ctx, JSValueConst la, JSValueConst lo, const char* fn, const char* what, double** lat,
    double** lon, uint32_t* n)
{
    uint32_t nl, k;
    if (mxt_vec(ctx, la, fn, what, lat, n))
        return -1;
    if (mxt_vec(ctx, lo, fn, what, lon, &nl)) {
        free(*lat);
        *lat = NULL;
        return -1;
    }
    if (nl != *n) {
        free(*lat);
        free(*lon);
        *lat = *lon = NULL;
        JS_ThrowRangeError(ctx, "%s: %s latitudes and longitudes differ in length (%u vs %u)", fn, what, *n, nl);
        return -1;
    }
    for (k = 0; k < *n; k++)
        if (!((*lat)[k] >= -90.0 && (*lat)[k] <= 90.0) || !isfinite((*lon)[k])) {
            free(*lat);
            free(*lon);
            *lat = *lon = NULL;
            JS_ThrowRangeError(ctx, "%s: %s point %u is not a finite latitude in [-90, 90] and longitude", fn, what, k);
            return -1;
        }
    return 0;
}

static JSValue mxg_obj2(JSContext* ctx, const char* k1, double v1, const char* k2, double v2, const char* k3, double v3)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_SetPropertyStr(ctx, o, k1, JS_NewFloat64(ctx, v1)) < 0 || JS_SetPropertyStr(ctx, o, k2, JS_NewFloat64(ctx, v2)) < 0
        || (k3 && JS_SetPropertyStr(ctx, o, k3, JS_NewFloat64(ctx, v3)) < 0)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue mxg_haversine(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.haversine";
    double la1, lo1, la2, lo2, r, *lat = NULL, *lon = NULL, *out;
    uint32_t n;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat1", &la1) || mxg_fin(ctx, argc, argv, 1, fn, "lon1", &lo1)
        || mxg_radius(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, fn, &r))
        return JS_EXCEPTION;
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (mxg_pts(ctx, argv[2], argc > 3 ? argv[3] : JS_UNDEFINED, fn, "lat2/lon2", &lat, &lon, &n))
            return JS_EXCEPTION;
        out = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
        if (!out) {
            free(lat);
            free(lon);
            return JS_ThrowOutOfMemory(ctx);
        }
        dyn_geo_haversine_many(la1, lo1, lat, lon, n, r, out);
        free(lat);
        free(lon);
        return mxt_f64(ctx, out, n);
    }
    if (mxg_lat(ctx, argc, argv, 2, fn, "lat2", &la2) || mxg_fin(ctx, argc, argv, 3, fn, "lon2", &lo2))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_geo_haversine(la1, lo1, la2, lo2, r));
}

static JSValue mxg_bearing(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int mid)
{
    const char* fn = mid ? "geo.midpoint" : "geo.bearing";
    double la1, lo1, la2, lo2;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat1", &la1) || mxg_fin(ctx, argc, argv, 1, fn, "lon1", &lo1)
        || mxg_lat(ctx, argc, argv, 2, fn, "lat2", &la2) || mxg_fin(ctx, argc, argv, 3, fn, "lon2", &lo2))
        return JS_EXCEPTION;
    if (mid) {
        dyn_geo_pt_t p = dyn_geo_midpoint(la1, lo1, la2, lo2);
        return mxg_obj2(ctx, "lat", p.lat, "lon", p.lon, NULL, 0.0);
    }
    return JS_NewFloat64(ctx, dyn_geo_bearing(la1, lo1, la2, lo2));
}

static JSValue mxg_destination(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.destination";
    double la, lo, b, d, r;
    dyn_geo_pt_t p;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat", &la) || mxg_fin(ctx, argc, argv, 1, fn, "lon", &lo)
        || mxg_fin(ctx, argc, argv, 2, fn, "bearing", &b) || mxg_fin(ctx, argc, argv, 3, fn, "distance", &d)
        || mxg_radius(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, fn, &r))
        return JS_EXCEPTION;
    p = dyn_geo_destination(la, lo, b, d, r);
    return mxg_obj2(ctx, "lat", p.lat, "lon", p.lon, NULL, 0.0);
}

static JSValue mxg_cross_track(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.crossTrack";
    double la, lo, la1, lo1, la2, lo2, r, xt, at;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat", &la) || mxg_fin(ctx, argc, argv, 1, fn, "lon", &lo)
        || mxg_lat(ctx, argc, argv, 2, fn, "lat1", &la1) || mxg_fin(ctx, argc, argv, 3, fn, "lon1", &lo1)
        || mxg_lat(ctx, argc, argv, 4, fn, "lat2", &la2) || mxg_fin(ctx, argc, argv, 5, fn, "lon2", &lo2)
        || mxg_radius(ctx, argc > 6 ? argv[6] : JS_UNDEFINED, fn, &r))
        return JS_EXCEPTION;
    dyn_geo_cross_track(la, lo, la1, lo1, la2, lo2, r, &xt, &at);
    if (isnan(xt))
        return JS_ThrowRangeError(ctx, "%s: the path endpoints coincide or are antipodal, so the great circle is undefined", fn);
    return mxg_obj2(ctx, "crossTrack", xt, "alongTrack", at, NULL, 0.0);
}

static JSValue mxg_vincenty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.vincenty";
    double la1, lo1, la2, lo2, d, a1, a2;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat1", &la1) || mxg_fin(ctx, argc, argv, 1, fn, "lon1", &lo1)
        || mxg_lat(ctx, argc, argv, 2, fn, "lat2", &la2) || mxg_fin(ctx, argc, argv, 3, fn, "lon2", &lo2))
        return JS_EXCEPTION;
    if (dyn_geo_vincenty_inverse(la1, lo1, la2, lo2, &d, &a1, &a2) != DYN_GEO_OK)
        return JS_ThrowRangeError(ctx, "%s: no convergence (the points are nearly antipodal)", fn);
    return mxg_obj2(ctx, "distance", d, "azimuth1", a1, "azimuth2", a2);
}

static JSValue mxg_vincenty_direct(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.vincentyDirect";
    double la, lo, az, d, la2, lo2, a2;
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat", &la) || mxg_fin(ctx, argc, argv, 1, fn, "lon", &lo)
        || mxg_fin(ctx, argc, argv, 2, fn, "azimuth", &az) || mxg_num(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, fn, "distance", 0.0, 2.1e7, &d))
        return JS_EXCEPTION;
    if (dyn_geo_vincenty_direct(la, lo, az, d, &la2, &lo2, &a2) != DYN_GEO_OK)
        return JS_ThrowRangeError(ctx, "%s: no convergence", fn);
    return mxg_obj2(ctx, "lat", la2, "lon", lo2, "azimuth", a2);
}

static JSValue mxg_hash_encode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.geohashEncode";
    double la, lo, p = 9.0;
    char buf[DYN_GEO_HASH_MAX + 1];
    (void)this_val;
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat", &la) || mxg_num(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "lon", -180.0, 180.0, &lo))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])
        && (mxg_num(ctx, argv[2], fn, "precision", 1.0, (double)DYN_GEO_HASH_MAX, &p)))
        return JS_EXCEPTION;
    if (p != floor(p))
        return JS_ThrowRangeError(ctx, "%s: precision must be an integer in [1, 12]", fn);
    if (dyn_geo_hash_encode(la, lo, (unsigned)p, buf) != DYN_GEO_OK)
        return JS_ThrowRangeError(ctx, "%s: lat/lon out of range", fn);
    return JS_NewString(ctx, buf);
}

static int mxg_hash_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* fn, double* la, double* lo, double* el,
    double* eo, size_t* len)
{
    const char* s;
    int rc;
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "%s: hash must be a string", fn);
        return -1;
    }
    s = JS_ToCStringLen(ctx, len, argv[0]);
    if (!s)
        return -1;
    rc = dyn_geo_hash_decode(s, *len, la, lo, el, eo);
    JS_FreeCString(ctx, s);
    if (rc != DYN_GEO_OK) {
        JS_ThrowRangeError(ctx, "%s: hash must be 1-12 base-32 geohash characters (0-9, b-h, j, k, m, n, p-z)", fn);
        return -1;
    }
    return 0;
}

static JSValue mxg_hash_decode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double la, lo, el, eo;
    size_t len;
    JSValue o;
    (void)this_val;
    if (mxg_hash_arg(ctx, argc, argv, "geo.geohashDecode", &la, &lo, &el, &eo, &len))
        return JS_EXCEPTION;
    o = mxg_obj2(ctx, "lat", la, "lon", lo, "latErr", el);
    if (!JS_IsException(o) && JS_SetPropertyStr(ctx, o, "lonErr", JS_NewFloat64(ctx, eo)) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue mxg_hash_neighbors(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const dirs[8] = { "n", "ne", "e", "se", "s", "sw", "w", "nw" };
    static const int dy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    double la, lo, el, eo;
    size_t len;
    int k;
    JSValue o;
    (void)this_val;
    if (mxg_hash_arg(ctx, argc, argv, "geo.geohashNeighbors", &la, &lo, &el, &eo, &len))
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    for (k = 0; k < 8; k++) {
        double nla = la + 2.0 * el * dy[k], nlo = lo + 2.0 * eo * dx[k];
        char buf[DYN_GEO_HASH_MAX + 1];
        JSValue v = JS_NULL;
        nlo = fmod(nlo + 540.0, 360.0) - 180.0;
        if (dyn_geo_hash_encode(nla, nlo, (unsigned)len, buf) == DYN_GEO_OK)
            v = JS_NewString(ctx, buf);
        if (JS_IsException(v) || JS_SetPropertyStr(ctx, o, dirs[k], v) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    return o;
}

static JSValue mxg_ta(JSContext* ctx, void* p, size_t bytes, JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;
    ab = dyn_nat_ab_take(ctx, (uint8_t*)p, bytes);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue mxg_u8(JSContext* ctx, uint8_t* p, uint32_t n)
{
    return mxg_ta(ctx, p, n, JS_TYPED_ARRAY_UINT8);
}

static int mxg_qpt(JSContext* ctx, JSValueConst q, const char* fn, double* la, double* lo)
{
    JSValue v[2];
    int rc;
    if (!JS_IsObject(q) || JS_IsArray(ctx, q) > 0) {
        JS_ThrowTypeError(ctx, "%s: q must be a {lat, lon} object", fn);
        return -1;
    }
    if (mxt_get(ctx, q, "lat", &v[0]))
        return -1;
    if (mxt_get(ctx, q, "lon", &v[1])) {
        JS_FreeValue(ctx, v[0]);
        return -1;
    }
    rc = mxg_num(ctx, v[0], fn, "q.lat", -90.0, 90.0, la) || mxg_num(ctx, v[1], fn, "q.lon", -DBL_MAX, DBL_MAX, lo);
    JS_FreeValue(ctx, v[0]);
    JS_FreeValue(ctx, v[1]);
    return rc ? -1 : 0;
}

static JSValue mxg_within(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.haversineWithin";
    double *lat, *lon, *dist, qa, qo, md, r;
    uint32_t n, *idx;
    size_t k;
    JSValue o, a;
    (void)this_val;
    if (mxg_qpt(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, fn, &qa, &qo)
        || mxg_num(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, fn, "meters", 0.0, DBL_MAX, &md)
        || mxg_radius(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, fn, &r)
        || mxg_pts(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "lat/lon", &lat, &lon, &n))
        return JS_EXCEPTION;
    idx = (uint32_t*)malloc((size_t)(n ? n : 1) * sizeof *idx);
    dist = (double*)malloc((size_t)(n ? n : 1) * sizeof *dist);
    if (!idx || !dist) {
        free(idx);
        free(dist);
        free(lat);
        free(lon);
        return JS_ThrowOutOfMemory(ctx);
    }
    k = dyn_geo_haversine_within(qa, qo, lat, lon, n, md, r, idx, dist);
    free(lat);
    free(lon);
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        free(idx);
        free(dist);
        return o;
    }
    a = mxg_ta(ctx, idx, k * sizeof *idx, JS_TYPED_ARRAY_UINT32);
    if (JS_IsException(a) || JS_SetPropertyStr(ctx, o, "indices", a) < 0) {
        free(dist);
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    a = mxt_f64(ctx, dist, (uint32_t)k);
    if (JS_IsException(a) || JS_SetPropertyStr(ctx, o, "distances", a) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int mxg_ring(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* fn, double** rl, double** ro, uint32_t* n)
{
    if (mxg_pts(ctx, i < argc ? argv[i] : JS_UNDEFINED, i + 1 < argc ? argv[i + 1] : JS_UNDEFINED, fn, "ring", rl, ro, n))
        return -1;
    if (*n < 3) {
        free(*rl);
        free(*ro);
        *rl = *ro = NULL;
        JS_ThrowRangeError(ctx, "%s: a ring needs at least 3 vertices", fn);
        return -1;
    }
    return 0;
}

static JSValue mxg_pip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.pointInPolygon";
    double *rl, *ro, *pl, *po, la, lo;
    uint32_t n, np;
    uint8_t* out;
    int in;
    (void)this_val;
    if (mxg_ring(ctx, argc, argv, 2, fn, &rl, &ro, &n))
        return JS_EXCEPTION;
    if (argc > 0 && JS_IsObject(argv[0])) {
        if (mxg_pts(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, fn, "points", &pl, &po, &np)) {
            free(rl);
            free(ro);
            return JS_EXCEPTION;
        }
        out = (uint8_t*)malloc(np ? np : 1);
        if (out)
            dyn_geo_points_in_ring(pl, po, np, rl, ro, n, out);
        free(pl);
        free(po);
        free(rl);
        free(ro);
        if (!out)
            return JS_ThrowOutOfMemory(ctx);
        return mxg_u8(ctx, out, np);
    }
    if (mxg_lat(ctx, argc, argv, 0, fn, "lat", &la) || mxg_fin(ctx, argc, argv, 1, fn, "lon", &lo)) {
        free(rl);
        free(ro);
        return JS_EXCEPTION;
    }
    in = dyn_geo_point_in_ring(la, lo, rl, ro, n);
    free(rl);
    free(ro);
    return JS_NewBool(ctx, in);
}

static JSValue mxg_area(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "geo.polygonArea";
    double *rl, *ro, r, a;
    uint32_t n;
    (void)this_val;
    if (mxg_radius(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, fn, &r) || mxg_ring(ctx, argc, argv, 0, fn, &rl, &ro, &n))
        return JS_EXCEPTION;
    a = dyn_geo_ring_area(rl, ro, n, r);
    free(rl);
    free(ro);
    return JS_NewFloat64(ctx, a);
}

static const JSCFunctionListEntry mxg_funcs[] = {
    JS_CFUNC_DEF("haversine", 4, mxg_haversine),
    JS_CFUNC_DEF("haversineWithin", 4, mxg_within),
    JS_CFUNC_MAGIC_DEF("bearing", 4, mxg_bearing, 0),
    JS_CFUNC_MAGIC_DEF("midpoint", 4, mxg_bearing, 1),
    JS_CFUNC_DEF("destination", 4, mxg_destination),
    JS_CFUNC_DEF("crossTrack", 6, mxg_cross_track),
    JS_CFUNC_DEF("vincenty", 4, mxg_vincenty),
    JS_CFUNC_DEF("vincentyDirect", 4, mxg_vincenty_direct),
    JS_CFUNC_DEF("geohashEncode", 2, mxg_hash_encode),
    JS_CFUNC_DEF("geohashDecode", 1, mxg_hash_decode),
    JS_CFUNC_DEF("geohashNeighbors", 1, mxg_hash_neighbors),
    JS_CFUNC_DEF("pointInPolygon", 4, mxg_pip),
    JS_CFUNC_DEF("polygonArea", 2, mxg_area),
};

int dyn_mathx_geo_register(JSContext* ctx, JSValue geo)
{
    return JS_SetPropertyFunctionList(ctx, geo, mxg_funcs, countof(mxg_funcs));
}

#endif
