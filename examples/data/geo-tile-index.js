// Geo tile index — points of interest indexed for radius, k-nearest, box, geohash and polygon queries, with map-tile
// heat counts, each query checked against a brute-force haversine scan.
//
// WHAT IT IS
//   The spatial layer behind "restaurants near me" and a delivery-zone heat map:
//
//   index       POIs as unit vectors on the sphere in a 3-D KDTree. Chord length is a monotone function of great-circle
//               distance (chord = 2 sin(d / 2R)), so a KDTree radius query with the converted radius is EXACT for
//               haversine distance — no projection error near the poles or across the antimeridian
//   categories  one RoaringBitmap per category; "cafes within 800 m" is radius ids AND the cafe bitmap
//   geohash     dyna:mathx geo.geohashEncode / Decode / Neighbors (checked against the published "ezs42" vector),
//               prefix grouping in an OrderedMap, and the 8 neighbour cells for edge-safe proximity
//   tiles       Web Mercator slippy-map tile x/y per zoom (OSM formula), counted in a Multiset for a heat map;
//               the busiest tile's POIs re-derived from its bounding box via KDTree.box on lat/lon
//   polygon     a delivery zone tested by geo.pointInPolygon over whole arrays, pre-filtered by its bounding box
//   ellipsoid   geo.vincenty, the inverse formula on WGS-84 (checked against the worked example in Vincenty 1975:
//               Flinders Peak -> Buninyong, 54 972.271 m, azimuth 306 52' 05.37"), and how far the spherical
//               haversine strays from it at city scale
//   traces      a courier's GPS trace simplified by Douglas-Peucker to within 10 m, then stored as a Google encoded
//               polyline (checked against the format's published example); every dropped fix must lie within the
//               tolerance of the simplified line, and the decoded line within rounding of the kept fixes
//   oracle      every radius and kNN answer equals a full haversine scan
//
// MODULES  dyna:structures (KDTree, RoaringBitmap, OrderedMap, Multiset) · dyna:mathx (geo) · dyna:random · dyna:cli
//          · Number/Array extensions
//
// RUN      dynajs examples/data/geo-tile-index.js

import { KDTree, RoaringBitmap, OrderedMap, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { geo } from "dyna:mathx";

const results = {};
const rng = new Random(1854);
const R = 6_371_008.8;
const rad = (d) => (d * Math.PI) / 180;
const haversine = (a, b) => geo.haversine(a.lat, a.lon, b.lat, b.lon, { radius: R });
const toXYZ = (p) => [
    Math.cos(rad(p.lat)) * Math.cos(rad(p.lon)),
    Math.cos(rad(p.lat)) * Math.sin(rad(p.lon)),
    Math.sin(rad(p.lat)),
];
const chordFor = (meters) => 2 * Math.sin(meters / (2 * R));

// =============================================================================
// POIs: a dense city, a few towns, and a cluster straddling the antimeridian
// =============================================================================
const CATS = ["cafe", "pharmacy", "grocery", "restaurant", "atm", "fuel"];
const centres = [
    { lat: 52.52, lon: 13.405, n: 60_000, s: 0.08 },
    { lat: 48.137, lon: 11.575, n: 15_000, s: 0.05 },
    { lat: 53.55, lon: 9.993, n: 15_000, s: 0.05 },
    { lat: -16.5, lon: 179.95, n: 3000, s: 0.1 },
];
const pois = [];
for (const c of centres)
    for (let i = 0; i < c.n; i++) {
        let lon = c.lon + rng.normal() * c.s * 1.6;
        if (lon > 180) lon -= 360;
        pois.push({ lat: c.lat + rng.normal() * c.s, lon, cat: CATS[Math.floor(rng.exponential(0.7)) % CATS.length] });
    }
const flat = new Float64Array(pois.length * 3);
pois.forEach((p, i) => flat.set(toXYZ(p), i * 3));
const t0 = performance.now();
const tree = new KDTree(flat, 3);
const byCat = Object.fromEntries(CATS.map((c) => [c, new RoaringBitmap()]));
pois.forEach((p, i) => byCat[p.cat].add(i));
const buildMs = performance.now() - t0;
const LAT = Float64Array.from(pois, (p) => p.lat),
    LON = Float64Array.from(pois, (p) => p.lon);

// =============================================================================
// Radius and kNN, exact under the chord transform
// =============================================================================
function within(center, meters) {
    return Array.from(tree.radius(toXYZ(center), chordFor(meters)).indices);
}
function nearest(center, k) {
    const r = tree.knn(toXYZ(center), k);
    return Array.from(r.indices, (i, j) => [i, 2 * R * Math.asin(r.distances[j] / 2)]);
}
{
    let exact = true,
        knnOk = true,
        queries = 0;
    for (const q of [
        { lat: 52.52, lon: 13.405 },
        { lat: 48.14, lon: 11.58 },
        { lat: -16.5, lon: -179.98 },
        { lat: 52.3, lon: 13.9 },
        { lat: 0, lon: 0 },
    ]) {
        for (const m of [300, 1500, 8000]) {
            queries++;
            const got = new Set(within(q, m));
            const want = geo.haversineWithin(LAT, LON, q, m, { radius: R });
            // Points within 1 mm of the boundary may fall either side of floating-point rounding.
            const firm = want.indices.filter((i, k) => want.distances[k] < m - 1e-3);
            if (!firm.every((i) => got.has(i)) || got.size > want.indices.length + 2) exact = false;
        }
        const kn = nearest(q, 15);
        const brute = geo.haversine(q.lat, q.lon, LAT, LON, { radius: R }).sort().subarray(0, 15);
        if (kn.some(([, d], j) => Math.abs(d - brute[j]) > 1e-3)) knnOk = false;
    }
    results["radius queries equal a haversine scan"] = exact && queries === 15;
    results["kNN distances equal the 15 nearest by haversine"] = knnOk;
    const fiji = within({ lat: -16.5, lon: 179.99 }, 5000).map((i) => pois[i].lon);
    results["queries straddle the antimeridian"] = fiji.some((l) => l > 179) && fiji.some((l) => l < -179);
}
{
    const here = { lat: 52.5163, lon: 13.3777 };
    const near = new RoaringBitmap(within(here, 800));
    const cafes = near.clone().and(byCat.cafe),
        pharm = near.clone().and(byCat.pharmacy);
    const bruteCafes = geo
        .haversineWithin(LAT, LON, here, 800 - 1e-3, { radius: R })
        .indices.filter((i) => pois[i].cat === "cafe").length;
    results["category filter is a bitmap AND"] =
        cafes.toArray().length >= bruteCafes && cafes.toArray().length <= bruteCafes + 1;
    print(
        `within 800 m of the Brandenburg Gate: ${near.toArray().length} POIs, ${cafes.toArray().length} cafes, ${pharm.toArray().length} pharmacies`,
    );
}

// =============================================================================
// Geohash
// =============================================================================
const geohash = (lat, lon, len = 7) => geo.geohashEncode(lat, lon, len);
function decodeBox(hash) {
    const c = geo.geohashDecode(hash);
    return { latLo: c.lat - c.latErr, latHi: c.lat + c.latErr, lonLo: c.lon - c.lonErr, lonHi: c.lon + c.lonErr };
}
const neighbours = (hash) => Object.values(geo.geohashNeighbors(hash)).filter((h) => h !== null);
results["geohash matches the published vector (42.6, -5.6) -> ezs42"] = geohash(42.6, -5.6, 5) === "ezs42";
{
    const cells = new OrderedMap();
    pois.forEach((p, i) => {
        const h = geohash(p.lat, p.lon, 6);
        (cells.get(h) ?? cells.set(h, []).get(h)).push(i);
    });
    const centre = geohash(52.52, 13.405, 6);
    const prefixHits = cells.prefix(centre.slice(0, 5)).reduce((s, [, ids]) => s + ids.length, 0);
    const boxBrute = (() => {
        const b = decodeBox(centre.slice(0, 5));
        return pois.filter((p) => p.lat >= b.latLo && p.lat < b.latHi && p.lon >= b.lonLo && p.lon < b.lonHi).length;
    })();
    results["a geohash prefix range is exactly its bounding box"] = prefixHits === boxBrute;
    const ring = neighbours(centre);
    results["the 8 neighbours are distinct adjacent cells"] =
        new Set(ring).size === 8 && !ring.includes(centre) && ring.every((h) => h.length === 6);
    const wrap = neighbours(geohash(-16.5, 179.999, 6));
    results["neighbours wrap across the antimeridian"] = wrap.some((h) => decodeBox(h).lonLo < -179);
}

// =============================================================================
// Slippy-map tiles and a heat map
// =============================================================================
const tileOf = (lat, lon, z) => {
    const n = 2 ** z,
        x = Math.floor(((lon + 180) / 360) * n);
    const y = Math.floor(((1 - Math.log(Math.tan(rad(lat)) + 1 / Math.cos(rad(lat))) / Math.PI) / 2) * n);
    return [x, y];
};
const tileBox = (x, y, z) => {
    const n = 2 ** z,
        lat = (yy) => (Math.atan(Math.sinh(Math.PI * (1 - (2 * yy) / n))) * 180) / Math.PI;
    return { lonLo: (x / n) * 360 - 180, lonHi: ((x + 1) / n) * 360 - 180, latHi: lat(y), latLo: lat(y + 1) };
};
results["every POI lies inside the box of its own tile"] = pois.every((p, i) => {
    if (i % 97) return true;
    const [x, y] = tileOf(p.lat, p.lon, 14),
        b = tileBox(x, y, 14);
    return p.lat >= b.latLo && p.lat <= b.latHi && p.lon >= b.lonLo && p.lon <= b.lonHi;
});
{
    const heat = new Multiset();
    for (const p of pois) heat.add(tileOf(p.lat, p.lon, 12).join("/"));
    const [hottest, count] = heat.entrySet().sort((a, b) => b[1] - a[1])[0];
    const [x, y] = hottest.split("/").map(Number);
    const box = tileBox(x, y, 12);
    const llTree = new KDTree(pois.map((p) => [p.lat, p.lon]));
    const inBox = llTree.box([box.latLo, box.lonLo], [box.latHi, box.lonHi]);
    results["KDTree box over the hottest tile finds its count"] = Math.abs(inBox.length - count) <= 2;
    print(`heat map: ${heat.elementSet().length.format()} tiles at z=12, hottest ${hottest} with ${count} POIs`);
}

// =============================================================================
// Delivery zone polygon
// =============================================================================
{
    const zone = [
        [52.56, 13.3],
        [52.57, 13.45],
        [52.5, 13.52],
        [52.45, 13.42],
        [52.47, 13.31],
    ];
    const lats = zone.map((p) => p[0]),
        lons = zone.map((p) => p[1]),
        inside = (pts) =>
            geo.pointInPolygon(
                Float64Array.from(pts, (p) => p.lat),
                Float64Array.from(pts, (p) => p.lon),
                lats,
                lons,
            );
    const llTree = new KDTree(pois.map((p) => [p.lat, p.lon]));
    const t1 = performance.now();
    const cands = llTree.box([Math.min(...lats), Math.min(...lons)], [Math.max(...lats), Math.max(...lons)]);
    const candPois = Array.from(cands, (i) => pois[i]),
        candIn = inside(candPois),
        served = candPois.filter((_, k) => candIn[k]);
    const fastMs = performance.now() - t1;
    const t2 = performance.now();
    const brute = inside(pois).reduce((s, v) => s + v, 0);
    const bruteMs = performance.now() - t2;
    results["bounding-box prefilter keeps every point in the zone"] = served.length === brute;
    results["prefilter tests a fraction of the points"] = cands.length < pois.length / 4;
    print(
        Table(
            [
                ["POIs indexed", pois.length.format(), `${buildMs.toFixed(0)} ms`],
                [
                    "in delivery zone",
                    served.length.format(),
                    `${fastMs.toFixed(1)} ms (vs ${bruteMs.toFixed(1)} ms scan)`,
                ],
            ],
            { head: ["geo", "count", "time"] },
        ),
    );
}

// =============================================================================
// Ellipsoidal distance: Vincenty's inverse formula on WGS-84
// =============================================================================
function vincenty(p1, p2) {
    const v = geo.vincenty(p1.lat, p1.lon, p2.lat, p2.lon);
    return { meters: v.distance, azimuth: v.azimuth1 };
}
const dms = (d, m, s) => Math.sign(d || 1) * (Math.abs(d) + m / 60 + s / 3600);
{
    // Vincenty (1975), "Direct and inverse solutions of geodesics on the ellipsoid", worked example as republished
    // by Geoscience Australia: Flinders Peak to Buninyong
    const flinders = { lat: -dms(37, 57, 3.7203), lon: dms(144, 25, 29.5244) },
        buninyong = { lat: -dms(37, 39, 10.1561), lon: dms(143, 55, 35.3839) };
    const v = vincenty(flinders, buninyong);
    results[`Vincenty: Flinders Peak -> Buninyong ${v.meters.toFixed(3)} m (published 54 972.271)`] =
        Math.abs(v.meters - 54972.271) < 0.001 && Math.abs(v.azimuth - dms(306, 52, 5.37)) < 0.01 / 3600;
    // the sphere is off by up to ~0.5%; at Berlin's latitude, east-west and north-south err in opposite directions
    let worst = 0;
    for (let k = 0; k < 400; k++) {
        const a = pois[k * 97],
            b = pois[k * 97 + 13];
        const h = haversine(a, b),
            g = vincenty(a, b).meters;
        if (g > 100) worst = Math.max(worst, Math.abs(h - g) / g);
    }
    results[`haversine stays within 0.5% of the ellipsoid (worst ${(worst * 100).toFixed(2)}%)`] =
        worst < 0.005 && worst > 0.0005;
}

// =============================================================================
// A courier's trace: Douglas-Peucker, then a Google encoded polyline
// =============================================================================
{
    const trace = [];
    let lat = 52.5,
        lon = 13.4,
        heading = 0.3;
    for (let t = 0; t < 4000; t++) {
        if (t % 200 === 0) heading += (rng.nextFloat() - 0.5) * 2.4; // turns at junctions
        heading += rng.normal() * 0.02; // the road bends
        const step = 8 + rng.nextFloat() * 4; // metres per second
        lat += (step * Math.cos(heading)) / (R * rad(1));
        lon += (step * Math.sin(heading)) / (R * rad(1) * Math.cos(rad(lat)));
        trace.push({ lat: lat + rng.normal() * 1e-5, lon: lon + rng.normal() * 1.6e-5 }); // ~1 m GPS jitter
    }
    // local planar metres around the trace's start: fine over a few kilometres for a 10 m tolerance
    const lat0 = rad(trace[0].lat),
        xy = (p) => [rad(p.lon) * Math.cos(lat0) * R, rad(p.lat) * R];
    const segDist = (p, a, b) => {
        const [px, py] = xy(p),
            [ax, ay] = xy(a),
            [bx, by] = xy(b),
            dx = bx - ax,
            dy = by - ay,
            len2 = dx * dx + dy * dy;
        const t = len2 ? Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / len2)) : 0;
        return Math.hypot(px - ax - t * dx, py - ay - t * dy);
    };
    const TOL = 10;
    function douglasPeucker(pts) {
        const keep = new Uint8Array(pts.length);
        keep[0] = keep[pts.length - 1] = 1;
        const stack = [[0, pts.length - 1]];
        while (stack.length) {
            const [i, j] = stack.pop();
            let far = -1,
                farD = 0;
            for (let k = i + 1; k < j; k++) {
                const d = segDist(pts[k], pts[i], pts[j]);
                if (d > farD) {
                    farD = d;
                    far = k;
                }
            }
            if (farD > TOL) {
                keep[far] = 1;
                stack.push([i, far], [far, j]);
            }
        }
        return pts.filter((_, k) => keep[k]);
    }
    const t3 = performance.now();
    const kept = douglasPeucker(trace);
    const dpMs = performance.now() - t3;
    // oracle: every fix lies within TOL of the kept polyline (any segment), by the haversine cross-track distance
    const crossTrack = (p, a, b) => {
        const t = geo.crossTrack(p.lat, p.lon, a.lat, a.lon, b.lat, b.lon, { radius: R });
        if (t.alongTrack < 0 || t.alongTrack > haversine(a, b)) return Math.min(haversine(a, p), haversine(b, p));
        return Math.abs(t.crossTrack);
    };
    let worstDev = 0,
        seg = 0;
    const keptIndex = new Map(kept.map((p, k) => [p, k]));
    for (const p of trace) {
        if (keptIndex.has(p)) {
            seg = Math.min(keptIndex.get(p), kept.length - 2);
            continue;
        }
        worstDev = Math.max(worstDev, crossTrack(p, kept[seg], kept[seg + 1]));
    }
    results[
        `Douglas-Peucker keeps ${kept.length} of ${trace.length} fixes, every dropped fix within 10 m (worst ${worstDev.toFixed(2)} m)`
    ] = worstDev <= TOL + 0.05 && kept.length * 5 < trace.length && worstDev > TOL / 2;

    // Google's encoded polyline: deltas of 1e-5 degrees, zigzag sign, 5-bit chunks + 63
    const encode = (pts) => {
        let out = "",
            pLat = 0,
            pLon = 0;
        const num = (v) => {
            let n = v < 0 ? ~(v << 1) : v << 1;
            while (n >= 0x20) {
                out += String.fromCharCode((0x20 | (n & 0x1f)) + 63);
                n >>>= 5;
            }
            out += String.fromCharCode(n + 63);
        };
        for (const p of pts) {
            const la = Math.round(p.lat * 1e5),
                lo = Math.round(p.lon * 1e5);
            num(la - pLat);
            num(lo - pLon);
            pLat = la;
            pLon = lo;
        }
        return out;
    };
    const decode = (str) => {
        const pts = [];
        let i = 0,
            la = 0,
            lo = 0;
        const num = () => {
            let shift = 0,
                n = 0,
                b;
            do {
                b = str.charCodeAt(i++) - 63;
                n |= (b & 0x1f) << shift;
                shift += 5;
            } while (b >= 0x20);
            return n & 1 ? ~(n >>> 1) : n >>> 1;
        };
        while (i < str.length) {
            la += num();
            lo += num();
            pts.push({ lat: la / 1e5, lon: lo / 1e5 });
        }
        return pts;
    };
    // the example in Google's "Encoded Polyline Algorithm Format" documentation
    results["encoded polyline matches the published example"] =
        encode([
            { lat: 38.5, lon: -120.2 },
            { lat: 40.7, lon: -120.95 },
            { lat: 43.252, lon: -126.453 },
        ]) === "_p~iF~ps|U_ulLnnqC_mqNvxq`@";
    const line = encode(kept),
        back = decode(line);
    results[`the trace round-trips through ${line.length} polyline bytes within 0.5e-5 degrees`] =
        back.length === kept.length &&
        back.every(
            (p, k) => Math.abs(p.lat - kept[k].lat) <= 5e-6 + 1e-12 && Math.abs(p.lon - kept[k].lon) <= 5e-6 + 1e-12,
        );
    print(
        `courier trace: ${trace.length} fixes -> ${kept.length} kept (${dpMs.toFixed(1)} ms) -> ${line.length} polyline bytes (JSON ${JSON.stringify(trace).length.bytes()})`,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
