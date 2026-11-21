#!/usr/bin/env python3
"""numpy timings for bench/bench_geo.js on the SAME points (the raw float64 file that script's 2nd argument writes).
haversine and point-in-polygon are vectorised numpy (the usual way they are written in Python); vincenty, geohash and
polygonArea are per-call loops as in the JS rows, using straightforward pure-Python implementations.

    ./dynajs bench/bench_geo.js 5 geo.f64
    python3 bench/bench_geo_numpy.py geo.f64 3
"""
import math
import sys
import time

import numpy as np

raw = np.fromfile(sys.argv[1], dtype="<f8")
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
N, RING, K, R = 2000000, 200, 20000, 6371008.8
LAT, LON, RLAT, RLON = raw[:N], raw[N:2 * N], raw[2 * N:2 * N + RING], raw[2 * N + RING:]


def best(f):
    b = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        f()
        b = min(b, time.perf_counter() - t)
    return b * 1000


def hav():
    p1, p2 = math.radians(52.52), np.radians(LAT)
    a = np.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * np.cos(p2) * np.sin(np.radians(LON - 13.405) / 2) ** 2
    return 2 * R * np.arctan2(np.sqrt(a), np.sqrt(1 - a))


def within():
    d = hav()
    i = np.flatnonzero(d <= 5e5)
    return i.astype(np.uint32), d[i]


def pip():
    inside = np.zeros(N, dtype=bool)
    for i in range(RING):
        j = i - 1
        yi, yj, xi, xj = RLAT[i], RLAT[j], RLON[i], RLON[j]
        cross = (yi > LAT) != (yj > LAT)
        x = xi + (LAT - yi) * (xj - xi) / (yj - yi if yj != yi else 1)
        inside ^= cross & (LON < x)
    return inside


def vincenty(la1, lo1, la2, lo2):
    a, f = 6378137.0, 1 / 298.257223563
    b = a * (1 - f)
    L = math.radians(lo2 - lo1)
    U1, U2 = math.atan((1 - f) * math.tan(math.radians(la1))), math.atan((1 - f) * math.tan(math.radians(la2)))
    sU1, cU1, sU2, cU2 = math.sin(U1), math.cos(U1), math.sin(U2), math.cos(U2)
    lam = L
    for _ in range(200):
        sl, cl = math.sin(lam), math.cos(lam)
        ss = math.hypot(cU2 * sl, cU1 * sU2 - sU1 * cU2 * cl)
        if ss == 0:
            return 0.0
        cs = sU1 * sU2 + cU1 * cU2 * cl
        sig = math.atan2(ss, cs)
        sa = cU1 * cU2 * sl / ss
        ca2 = 1 - sa * sa
        c2m = cs - 2 * sU1 * sU2 / ca2 if ca2 else 0.0
        C = f / 16 * ca2 * (4 + f * (4 - 3 * ca2))
        prev, lam = lam, L + (1 - C) * f * sa * (sig + C * ss * (c2m + C * cs * (-1 + 2 * c2m * c2m)))
        if abs(lam - prev) < 1e-12:
            break
    u2 = ca2 * (a * a - b * b) / (b * b)
    A = 1 + u2 / 16384 * (4096 + u2 * (-768 + u2 * (320 - 175 * u2)))
    B = u2 / 1024 * (256 + u2 * (-128 + u2 * (74 - 47 * u2)))
    ds = B * ss * (c2m + B / 4 * (cs * (-1 + 2 * c2m * c2m) - B / 6 * c2m * (-3 + 4 * ss * ss) * (-3 + 4 * c2m * c2m)))
    return b * A * (sig - ds)


B32 = "0123456789bcdefghjkmnpqrstuvwxyz"


def gh(lat, lon, p):
    la, lo, out, ch, bit, even = [-90.0, 90.0], [-180.0, 180.0], [], 0, 0, True
    while len(out) < p:
        r, v = (lo, lon) if even else (la, lat)
        m = (r[0] + r[1]) / 2
        if v >= m:
            ch, r[0] = ch * 2 + 1, m
        else:
            ch, r[1] = ch * 2, m
        even, bit = not even, bit + 1
        if bit == 5:
            out.append(B32[ch])
            ch = bit = 0
    return "".join(out)


def area():
    t1, t2 = np.tan(np.radians(RLAT) / 2), np.tan(np.radians(np.roll(RLAT, -1)) / 2)
    dl = np.radians((np.roll(RLON, -1) - RLON + 540) % 360 - 180)
    e = abs(np.sum(2 * np.arctan2(np.tan(dl / 2) * (t1 + t2), 1 + t1 * t2)))
    return min(e, 4 * math.pi - e) * R * R


L, O = LAT.tolist(), LON.tolist()
rows = [
    ("haversine 1 -> 2e6 points", hav),
    ("haversineWithin 2e6, 500 km", within),
    ("pointInPolygon 2e6 x 200-gon", pip),
    ("vincenty 2e4 pairs", lambda: [vincenty(L[i], O[i], L[i + K] * 0.3, O[i + K] * 0.3) for i in range(K)]),
    ("geohashEncode 2e4 (p=9)", lambda: [gh(L[i], O[i], 9) for i in range(K)]),
    ("polygonArea 200-gon x 1e3", lambda: [area() for _ in range(1000)]),
]
print("numpy %s: best of %d" % (np.__version__, runs))
for name, f in rows:
    print("%-32s%10.2f ms" % (name, best(f)))
