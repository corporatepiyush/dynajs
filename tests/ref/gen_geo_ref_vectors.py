#!/usr/bin/env python3
"""Reference vectors for the dyna:mathx geo namespace, from independent formulations.

    python3 tests/ref/gen_geo_ref_vectors.py > tests/corpus/ref/geo_vectors.js

Needs numpy and scipy (versions recorded in the output). Nothing here reuses the native formulas:
  - WGS84 geodesics (vincenty / vincentyDirect) integrate the geodesic ODE in geodetic coordinates
        dphi/ds = cos(a)/M(phi), dlam/ds = sin(a)/(N(phi) cos(phi)), da/ds = sin(a) tan(phi)/N(phi)
    with scipy.integrate.solve_ivp (DOP853, rtol=1e-12, atol=1e-14 rad); the inverse problem shoots (azimuth1, s)
    with scipy.optimize.least_squares (xtol=ftol=gtol=1e-15) from a spherical first guess. Paths are kept below
    80 degrees of latitude, away from the ODE's polar singularity. Integration error is below 1e-6 m and 1e-9 deg.
  - sphere functions use 3-D unit vectors (distance = R atan2(|a x b|, a.b), bearing from the local north/east
    frame, destination by rotation in the tangent plane), cross-track uses the spherical-trigonometry asin/acos form.
  - polygonArea sums Van Oosterom-Strackee signed excesses over a fan triangulation.
  - pointInPolygon is an independent even-odd crossing count; geohash is a string-level re-implementation.
"""
import json
import os
import sys
import math

import numpy as np
import scipy
from scipy.integrate import solve_ivp
from scipy.optimize import least_squares

A = 6378137.0
F = 1 / 298.257223563
E2 = F * (2 - F)
R = 6371008.8
rng = np.random.default_rng(20261114)



def pinned(name, have):
    """The version written into the vectors is the pin in tests/ref/requirements.txt, never the
    running one: a runtime version string made the file's sha256 differ between two machines
    that computed the same numbers. A different running version is reported on stderr."""
    here = os.path.dirname(os.path.abspath(__file__))
    for line in open(os.path.join(here, "requirements.txt")):
        if line.startswith(name + "=="):
            pin = line.strip().split("==", 1)[1]
            if pin != have:
                sys.stderr.write("%s: %s %s is running, the vectors record the pin %s\n" % (os.path.basename(__file__), name, have, pin))
            return pin
    raise SystemExit("tests/ref/requirements.txt pins no " + name)

JITTER = int(os.environ.get("DYNA_REF_ULP_JITTER", "0"))


def num(x):
    """Results are written to 13 significant digits: 15 kept the last bits of libm and of
    solve_ivp, which differ across machines, in the file (and its sha256). 13 digits sit at
    least 5x under every tolerance of tests/mathx/test_mathx_geo_ref.js (1e-9 deg on a bearing
    near 360 is the tightest); 12 left a bearing within 1 ulp of a rounding boundary. DYNA_REF_ULP_JITTER=k moves each result by k ulps first, the way another libm
    would; `./dynajs --std tools/test-data.js repro` requires the file not to change."""
    x = float(x)
    if x == 0.0:
        return 0
    if JITTER:
        x += JITTER * math.ulp(x)
    return float("%.13g" % x)


CASES = []


def case(op, args, want):
    CASES.append({"op": op, "args": args, "want": want})


def rhs(s, y):
    phi, lam, a = y
    sp = math.sin(phi)
    w = math.sqrt(1 - E2 * sp * sp)
    n = A / w
    m = A * (1 - E2) / (w * w * w)
    return [math.cos(a) / m, math.sin(a) / (n * math.cos(phi)), math.sin(a) * math.tan(phi) / n]


def direct(lat, lon, az, s):
    sol = solve_ivp(rhs, (0, s), [math.radians(lat), math.radians(lon), math.radians(az)], method="DOP853",
                    rtol=1e-12, atol=1e-14, dense_output=False)
    phi, lam, a = sol.y[:, -1]
    return math.degrees(phi), (math.degrees(lam) + 540) % 360 - 180, math.degrees(a) % 360, np.max(np.abs(sol.y[0]))


def vec(lat, lon):
    p, l = math.radians(lat), math.radians(lon)
    return np.array([math.cos(p) * math.cos(l), math.cos(p) * math.sin(l), math.sin(p)])


def sph_dist(a, b):
    return R * math.atan2(np.linalg.norm(np.cross(a, b)), a @ b)


def frame(lat, lon):
    p, l = math.radians(lat), math.radians(lon)
    north = np.array([-math.sin(p) * math.cos(l), -math.sin(p) * math.sin(l), math.cos(p)])
    east = np.array([-math.sin(l), math.cos(l), 0.0])
    return north, east


def sph_bearing(lat1, lon1, lat2, lon2):
    a, b = vec(lat1, lon1), vec(lat2, lon2)
    n, e = frame(lat1, lon1)
    t = b - (a @ b) * a
    return math.degrees(math.atan2(t @ e, t @ n)) % 360


def inverse(lat1, lon1, lat2, lon2):
    s0 = sph_dist(vec(lat1, lon1), vec(lat2, lon2)) * (A / R)
    a0 = sph_bearing(lat1, lon1, lat2, lon2)

    def resid(x):
        la, lo, _, _ = direct(lat1, lon1, x[0], x[1])
        dlo = (lo - lon2 + 540) % 360 - 180
        return [(la - lat2) * 1e5, dlo * 1e5 * math.cos(math.radians(lat2))]

    r = least_squares(resid, [a0, s0], x_scale=[1.0, s0], xtol=1e-15, ftol=1e-15, gtol=1e-15, method="lm")
    az1, s = r.x
    la, lo, az2, top = direct(lat1, lon1, az1, s)
    assert abs(la - lat2) < 1e-9 and abs((lo - lon2 + 540) % 360 - 180) < 1e-9, (lat1, lon1, lat2, lon2)
    return s, az1 % 360, az2, top


def rand_pt(maxlat=70):
    return round(float(rng.uniform(-maxlat, maxlat)), 6), round(float(rng.uniform(-180, 180)), 6)


done = 0
while done < 40:
    (la1, lo1), (la2, lo2) = rand_pt(), rand_pt()
    if done < 10:
        la2, lo2 = round(la1 + float(rng.uniform(-1, 1)), 6), round(lo1 + float(rng.uniform(-1, 1)), 6)
    if sph_dist(vec(la1, lo1), vec(la2, lo2)) > 1.6e7:
        continue
    s, az1, az2, top = inverse(la1, lo1, la2, lo2)
    if math.degrees(top) > 80:
        continue
    case("vincenty", [la1, lo1, la2, lo2], [num(s), num(az1), num(az2)])
    done += 1
_s, _a1, _a2, _ = inverse(-37.95103342, 144.42486789, -37.65282114, 143.92649554)
assert abs(_s - 54972.271) < 1e-3 and abs(_a1 - (306 + 52 / 60 + 5.37 / 3600)) < 1e-5, (_s, _a1)
case("vincenty", [-37.95103342, 144.42486789, -37.65282114, 143.92649554], [num(_s), num(_a1), num(_a2)])
done = 0
while done < 30:
    la1, lo1 = rand_pt()
    az, s = round(float(rng.uniform(0, 360)), 6), round(float(rng.choice([1e3, 5e4, 1e6, 5e6, 1.5e7]) * rng.uniform(0.5, 1)), 3)
    la2, lo2, az2, top = direct(la1, lo1, az, s)
    if math.degrees(top) > 80:
        continue
    case("vincentyDirect", [la1, lo1, az, s], [num(la2), num(lo2), num(az2)])
    done += 1

for _ in range(40):
    (la1, lo1), (la2, lo2) = rand_pt(89), rand_pt(89)
    a, b = vec(la1, lo1), vec(la2, lo2)
    case("haversine", [la1, lo1, la2, lo2], [num(sph_dist(a, b))])
    if np.linalg.norm(np.cross(a, b)) > 1e-3:
        case("bearing", [la1, lo1, la2, lo2], [num(sph_bearing(la1, lo1, la2, lo2))])
    m = (a + b) / np.linalg.norm(a + b)
    case("midpoint", [la1, lo1, la2, lo2], [num(math.degrees(math.asin(m[2]))), num(math.degrees(math.atan2(m[1], m[0])))])
pts = [rand_pt(89) for _ in range(25)]
case("haversine", [10.0, 20.0, [p[0] for p in pts], [p[1] for p in pts]], [num(sph_dist(vec(10, 20), vec(*p))) for p in pts])
case("haversine", [10.0, 20.0, 11.0, 21.0, {"radius": 1.0}], [num(sph_dist(vec(10, 20), vec(11, 21)) / R)])
for _ in range(30):
    la, lo = rand_pt(80)
    th, d = round(float(rng.uniform(0, 360)), 6), round(float(rng.uniform(1, 9e6)), 3)
    n, e = frame(la, lo)
    a, dd = vec(la, lo), d / R
    t = math.cos(math.radians(th)) * n + math.sin(math.radians(th)) * e
    p = math.cos(dd) * a + math.sin(dd) * t
    case("destination", [la, lo, th, d], [num(math.degrees(math.asin(p[2]))), num(math.degrees(math.atan2(p[1], p[0])))])
for _ in range(25):
    (la, lo), (la1, lo1), (la2, lo2) = rand_pt(60), rand_pt(60), rand_pt(60)
    d13 = sph_dist(vec(la1, lo1), vec(la, lo)) / R
    t13, t12 = math.radians(sph_bearing(la1, lo1, la, lo)), math.radians(sph_bearing(la1, lo1, la2, lo2))
    xt = math.asin(math.sin(d13) * math.sin(t13 - t12))
    at = math.acos(max(-1, min(1, math.cos(d13) / math.cos(xt)))) * (1 if math.cos(t13 - t12) >= 0 else -1)
    case("crossTrack", [la, lo, la1, lo1, la2, lo2], [num(xt * R), num(at * R)])

B32 = "0123456789bcdefghjkmnpqrstuvwxyz"


def gh_encode(lat, lon, p):
    bits, lat_r, lon_r, even = "", [-90.0, 90.0], [-180.0, 180.0], True
    while len(bits) < 5 * p:
        r, v = (lon_r, lon) if even else (lat_r, lat)
        mid = (r[0] + r[1]) / 2
        if v >= mid:
            bits += "1"
            r[0] = mid
        else:
            bits += "0"
            r[1] = mid
        even = not even
    return "".join(B32[int(bits[i:i + 5], 2)] for i in range(0, len(bits), 5))


def gh_decode(h):
    bits = "".join(format(B32.index(c), "05b") for c in h)
    lat_r, lon_r = [-90.0, 90.0], [-180.0, 180.0]
    for i, b in enumerate(bits):
        r = lon_r if i % 2 == 0 else lat_r
        mid = (r[0] + r[1]) / 2
        r[0 if b == "1" else 1] = mid
    return (lat_r[0] + lat_r[1]) / 2, (lon_r[0] + lon_r[1]) / 2, (lat_r[1] - lat_r[0]) / 2, (lon_r[1] - lon_r[0]) / 2


for _ in range(40):
    la, lo = round(float(rng.uniform(-90, 90)), 7), round(float(rng.uniform(-180, 180)), 7)
    p = int(rng.integers(1, 13))
    h = gh_encode(la, lo, p)
    case("geohashEncode", [la, lo, p], h)
    case("geohashDecode", [h], [float(v) for v in gh_decode(h)])
case("geohashEncode", [42.6, -5.6, 5], "ezs42")
case("geohashEncode", [57.64911, 10.40744, 11], "u4pruydqqvj")
case("geohashNeighbors", ["ezs42"], {"n": "ezs48", "ne": "ezs49", "e": "ezs43", "se": "ezs41", "s": "ezs40", "sw": "ezefp", "w": "ezefr", "nw": "ezefx"})
for h in ["u4pruydq", "9q8yy", "s0000", "zzzz", "b", "pbpbpb"]:
    la, lo, el, eo = gh_decode(h)
    out = {}
    for k, (dy, dx) in {"n": (1, 0), "ne": (1, 1), "e": (0, 1), "se": (-1, 1), "s": (-1, 0), "sw": (-1, -1), "w": (0, -1), "nw": (1, -1)}.items():
        nla, nlo = la + 2 * el * dy, lo + 2 * eo * dx
        out[k] = gh_encode(nla, (nlo + 540) % 360 - 180, len(h)) if -90 < nla < 90 else None
    case("geohashNeighbors", [h], out)


def pip(lat, lon, rl, ro):
    inside, n = False, len(rl)
    for i in range(n):
        j = i - 1
        if (rl[i] > lat) != (rl[j] > lat):
            x = ro[i] + (lat - rl[i]) * (ro[j] - ro[i]) / (rl[j] - rl[i])
            if lon < x:
                inside = not inside
    return inside


def star(k, r1, r2, clat, clon):
    ang = np.linspace(0, 2 * np.pi, 2 * k, endpoint=False)
    rad = np.where(np.arange(2 * k) % 2 == 0, r1, r2)
    return [round(float(clat + r * math.sin(t)), 6) for t, r in zip(ang, rad)], [round(float(clon + r * math.cos(t)), 6) for t, r in zip(ang, rad)]


def area(rl, ro):
    v = [vec(a, b) for a, b in zip(rl, ro)]
    e = 0.0
    for i in range(1, len(v) - 1):
        a, b, c = v[0], v[i], v[i + 1]
        e += 2 * math.atan2(a @ np.cross(b, c), 1 + a @ b + b @ c + c @ a)
    e = abs(e)
    return min(e, 4 * math.pi - e) * R * R


rings = [star(5, 10, 4, 20, 30), star(7, 3, 1.2, -45, 170), ([0, 0, 1, 1], [0, 1, 1, 0]), star(3, 40, 15, 0, 0),
         ([60, 60, 70, 70], [-10, 10, 10, -10]), star(12, 0.01, 0.004, 51.5, -0.12),
         ([30, 30, 30, 30], [0, 90, 180, -90]), ([-5, -5, -5, -5, -5], [10, 80, 150, -140, -70])]
for rl, ro in rings:
    lats, lons = [], []
    clat, clon = sum(rl) / len(rl), sum(ro) / len(ro)
    if max(ro) - min(ro) > 180:
        case("polygonArea", [rl, ro], [num(area(rl, ro))])
        case("polygonArea", [rl[::-1], ro[::-1], {"radius": 1}], [num(area(rl, ro) / (R * R))])
        continue
    span = max(max(rl) - min(rl), max(ro) - min(ro))
    for _ in range(60):
        lats.append(round(float(clat + rng.uniform(-0.7, 0.7) * span), 6))
        lons.append(round(float(clon + rng.uniform(-0.7, 0.7) * span), 6))
    case("pointInPolygon", [lats, lons, rl, ro], [1 if pip(a, b, rl, ro) else 0 for a, b in zip(lats, lons)])
    case("pointInPolygon", [lats[0], lons[0], rl, ro], pip(lats[0], lons[0], rl, ro))
    case("polygonArea", [rl, ro], [num(area(rl, ro))])
    case("polygonArea", [rl[::-1], ro[::-1], {"radius": 1}], [num(area(rl, ro) / (R * R))])

out = {"scipy": pinned("scipy", scipy.__version__), "numpy": pinned("numpy", np.__version__), "cases": CASES}
print("// generated by tests/ref/gen_geo_ref_vectors.py; do not edit")
print("export default " + json.dumps(out, separators=(",", ":"), allow_nan=False) + ";")
