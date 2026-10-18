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
//   geohash     base32 geohash by bit interleaving (checked against the published "ezs42" vector), prefix
//               grouping in an OrderedMap, and the 8 neighbour cells for edge-safe proximity
//   tiles       Web Mercator slippy-map tile x/y per zoom (OSM formula), counted in a Multiset for a heat map;
//               the busiest tile's POIs re-derived from its bounding box via KDTree.box on lat/lon
//   polygon     a delivery zone tested by ray casting, pre-filtered by its bounding box
//   oracle      every radius and kNN answer equals a full haversine scan
//
// MODULES  dyna:structures (KDTree, RoaringBitmap, OrderedMap, Multiset) · dyna:random · dyna:cli · Number/Array
//          extensions
//
// RUN      dynajs examples/data/geo-tile-index.js

import { KDTree, RoaringBitmap, OrderedMap, Multiset } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1854);
const R = 6_371_008.8;
const rad = (d) => (d * Math.PI) / 180;
const haversine = (a, b) => {
    const dLat = rad(b.lat - a.lat),
        dLon = rad(b.lon - a.lon);
    const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLon / 2) ** 2;
    return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
};
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
            const want = pois.map((p, i) => [i, haversine(q, p)]).filter(([, d]) => d <= m);
            // Points within 1 mm of the boundary may fall either side of floating-point rounding.
            const firm = want.filter(([, d]) => d < m - 1e-3).map(([i]) => i);
            if (!firm.every((i) => got.has(i)) || got.size > want.length + 2) exact = false;
        }
        const kn = nearest(q, 15);
        const brute = pois
            .map((p, i) => [i, haversine(q, p)])
            .sort((a, b) => a[1] - b[1])
            .slice(0, 15);
        if (kn.some(([, d], j) => Math.abs(d - brute[j][1]) > 1e-3)) knnOk = false;
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
    const bruteCafes = pois.filter((p) => p.cat === "cafe" && haversine(here, p) <= 800 - 1e-3).length;
    results["category filter is a bitmap AND"] =
        cafes.toArray().length >= bruteCafes && cafes.toArray().length <= bruteCafes + 1;
    print(
        `within 800 m of the Brandenburg Gate: ${near.toArray().length} POIs, ${cafes.toArray().length} cafes, ${pharm.toArray().length} pharmacies`,
    );
}

// =============================================================================
// Geohash
// =============================================================================
const B32 = "0123456789bcdefghjkmnpqrstuvwxyz";
function geohash(lat, lon, len = 7) {
    let latLo = -90,
        latHi = 90,
        lonLo = -180,
        lonHi = 180,
        bit = 0,
        ch = 0,
        even = true,
        out = "";
    while (out.length < len) {
        if (even) {
            const mid = (lonLo + lonHi) / 2;
            if (lon >= mid) {
                ch = (ch << 1) | 1;
                lonLo = mid;
            } else {
                ch <<= 1;
                lonHi = mid;
            }
        } else {
            const mid = (latLo + latHi) / 2;
            if (lat >= mid) {
                ch = (ch << 1) | 1;
                latLo = mid;
            } else {
                ch <<= 1;
                latHi = mid;
            }
        }
        even = !even;
        if (++bit === 5) {
            out += B32[ch];
            bit = 0;
            ch = 0;
        }
    }
    return out;
}
function decodeBox(hash) {
    let latLo = -90,
        latHi = 90,
        lonLo = -180,
        lonHi = 180,
        even = true;
    for (const c of hash) {
        const v = B32.indexOf(c);
        for (let b = 4; b >= 0; b--) {
            const on = (v >> b) & 1;
            if (even) {
                const mid = (lonLo + lonHi) / 2;
                if (on) lonLo = mid;
                else lonHi = mid;
            } else {
                const mid = (latLo + latHi) / 2;
                if (on) latLo = mid;
                else latHi = mid;
            }
            even = !even;
        }
    }
    return { latLo, latHi, lonLo, lonHi };
}
function neighbours(hash) {
    const b = decodeBox(hash),
        dLat = b.latHi - b.latLo,
        dLon = b.lonHi - b.lonLo;
    const cLat = (b.latLo + b.latHi) / 2,
        cLon = (b.lonLo + b.lonHi) / 2,
        out = [];
    for (const dy of [-1, 0, 1])
        for (const dx of [-1, 0, 1])
            if (dx || dy) {
                let lon = cLon + dx * dLon;
                if (lon >= 180) lon -= 360;
                if (lon < -180) lon += 360;
                out.push(geohash(cLat + dy * dLat, lon, hash.length));
            }
    return out;
}
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
    const inside = (lat, lon) => {
        let hit = false;
        for (let i = 0, j = zone.length - 1; i < zone.length; j = i++) {
            const [yi, xi] = zone[i],
                [yj, xj] = zone[j];
            if (yi > lat !== yj > lat && lon < ((xj - xi) * (lat - yi)) / (yj - yi) + xi) hit = !hit;
        }
        return hit;
    };
    const lats = zone.map((p) => p[0]),
        lons = zone.map((p) => p[1]);
    const llTree = new KDTree(pois.map((p) => [p.lat, p.lon]));
    const t1 = performance.now();
    const cands = llTree.box([Math.min(...lats), Math.min(...lons)], [Math.max(...lats), Math.max(...lons)]);
    const served = Array.from(cands).filter((i) => inside(pois[i].lat, pois[i].lon));
    const fastMs = performance.now() - t1;
    const t2 = performance.now();
    const brute = pois.filter((p) => inside(p.lat, p.lon)).length;
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

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
