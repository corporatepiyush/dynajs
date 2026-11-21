// covers: contract dyna:mathx.geo -- argument contract and definitional identities of the geo namespace
// Independent of the reference vectors: published values (Vincenty 1975 Flinders Peak -> Buninyong; the geohash
// 'ezs42' example), closed-form areas, round trips between paired functions, and every documented TypeError / RangeError.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_geo_contract");
import { geo as G } from "dyna:mathx";

let pass = 0, fail = 0;
function ok(c, msg) {
    if (c) pass++;
    else { fail++; print("FAIL " + msg); }
}
function throws(fn, type, re, msg) {
    try { fn(); ok(false, msg + " (did not throw)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
const ang = (a, b) => Math.abs(((a - b + 540) % 360) - 180);
const R = 6371008.8;

__kitS.section("vincentyDirect inverts vincenty", () => {
    const v = G.vincenty(-37.95103342, 144.42486789, -37.65282114, 143.92649554);
    ok(Math.abs(v.distance - 54972.271) < 5e-4 && ang(v.azimuth1, 306 + 52 / 60 + 5.37 / 3600) < 1e-5 && ang(v.azimuth2, 127 + 10 / 60 + 25.07 / 3600 + 180) < 1e-5,
        `Vincenty's Flinders Peak -> Buninyong (${JSON.stringify(v)})`);
    const d = G.vincentyDirect(-37.95103342, 144.42486789, v.azimuth1, v.distance);
    ok(Math.abs(d.lat + 37.65282114) < 1e-9 && Math.abs(d.lon - 143.92649554) < 1e-9 && ang(d.azimuth, v.azimuth2) < 1e-8, "vincentyDirect inverts vincenty");
    const z = G.vincenty(12, 34, 12, 34);
    ok(z.distance === 0, "coincident points are 0 apart");
    ok(Math.abs(G.vincenty(0, 0, 0, 1).distance - 111319.49079327357) < 1e-6, "one degree of the WGS84 equator is a * pi / 180");
    ok(Math.abs(G.vincenty(0, 0, 0, 361).distance - G.vincenty(0, 0, 0, 1).distance) < 1e-9, "longitudes wrap");
});
__kitS.section("quarter and half great circles", () => {
    ok(Math.abs(G.haversine(0, 0, 0, 90) - R * Math.PI / 2) < 1e-6 && Math.abs(G.haversine(90, 0, -90, 0) - R * Math.PI) < 1e-6, "quarter and half great circles");
    ok(G.haversine(1, 2, 1, 2) === 0 && Math.abs(G.haversine(0, 0, 0, 1, { radius: 1 }) - Math.PI / 180) < 1e-15, "zero distance; radius option");
    const lats = Float64Array.of(0, 10, -20), lons = [0, 20, 30], many = G.haversine(5, 5, lats, lons);
    ok(many instanceof Float64Array && many.length === 3 && many.every((v, i) => v === G.haversine(5, 5, lats[i], lons[i])), "array form equals the scalar form");
    ok(G.bearing(0, 0, 0, 10) === 90 && G.bearing(0, 0, 10, 0) === 0 && G.bearing(0, 0, -10, 0) === 180 && G.bearing(0, 10, 0, 0) === 270, "cardinal bearings");
    const p = G.destination(51.5, -0.12, 77, 123456), b = G.bearing(51.5, -0.12, p.lat, p.lon);
    ok(Math.abs(G.haversine(51.5, -0.12, p.lat, p.lon) - 123456) < 1e-6 && ang(b, 77) < 1e-9, "destination is undone by haversine + bearing");
    const m = G.midpoint(0, 0, 0, 90);
    ok(Math.abs(m.lat) < 1e-12 && Math.abs(m.lon - 45) < 1e-12, "midpoint on the equator");
    const x = G.crossTrack(10, 45, 0, 0, 0, 90);
    ok(Math.abs(x.crossTrack + R * 10 * Math.PI / 180) < 1e-6 && Math.abs(x.alongTrack - R * Math.PI / 4) < 1e-6, `left of an eastbound path is negative (${JSON.stringify(x)})`);
    ok(G.crossTrack(-10, 45, 0, 0, 0, 90).crossTrack > 0, "right of the path is positive");
});
__kitS.section("geohash 'ezs42'; default precision 9", () => {
    ok(G.geohashEncode(42.6, -5.6, 5) === "ezs42" && G.geohashEncode(42.6, -5.6).length === 9, "geohash 'ezs42'; default precision 9");
    const d = G.geohashDecode("EZS42");
    ok(Math.abs(d.lat - 42.6) < d.latErr && Math.abs(d.lon + 5.6) < d.lonErr && d.latErr === 180 / 2 ** 13, "decode is case-insensitive and brackets the point");
    const n = G.geohashNeighbors("ezs42");
    ok(n.n === "ezs48" && n.sw === "ezefp" && Object.keys(n).length === 8, "ezs42 neighbours");
    ok(G.geohashEncode(0, 0, 5) === "s0000" && G.geohashEncode(-0.0000001, -0.0000001, 2) === "7z" && G.geohashEncode(45, 90, 3) === "y00",
        "a point on a cell boundary goes to the upper / eastern cell");
    ok(G.geohashEncode(90, 180, 3) === "zzz" && G.geohashEncode(-90, -180, 3) === "000", "the corners of the world");
    ok(G.geohashNeighbors("u").n === null && G.geohashNeighbors("0").s === null && G.geohashNeighbors("0").w === "p", "no neighbour beyond a pole; longitude wraps");
});
__kitS.section("point in / out of a square", () => {
    const rl = [0, 0, 1, 1], ro = [0, 1, 1, 0];
    ok(G.pointInPolygon(0.5, 0.5, rl, ro) === true && G.pointInPolygon(1.5, 0.5, rl, ro) === false, "point in / out of a square");
    const r = G.pointInPolygon([0.5, 2, 0.1], [0.5, 2, 0.9], rl, ro);
    ok(r instanceof Uint8Array && r.join() === "1,0,1", "array form returns a Uint8Array");
    ok(Math.abs(G.polygonArea([0, 0, 90], [0, 90, 0]) / (Math.PI / 2 * R * R) - 1) < 1e-12, "the octant is an eighth of the sphere");
    ok(Math.abs(G.polygonArea([90, 0, -90, 0], [0, 0, 0, 50], { radius: 1 }) - 2 * 50 * Math.PI / 180) < 1e-12, "a lune of width theta has area 2 theta");
    ok(G.polygonArea([0, 0, 1], [0, 1, 1]) === G.polygonArea([1, 0, 0], [1, 1, 0]), "area ignores orientation");
});
__kitS.section("haversineWithin returns exactly the haversine array-form points with d <= meters", () => {
    let seed = 7;
    const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
    const N = 200000, lat = new Float64Array(N), lon = new Float64Array(N);
    for (let i = 0; i < N; i++) {
        const c = i % 3 === 0;
        lat[i] = c ? 48 + rnd() : Math.asin(2 * rnd() - 1) * 180 / Math.PI;
        lon[i] = c ? 11 + rnd() * 1.5 : rnd() * 360 - 180;
    }
    const jsHav = (a1, o1, a2, o2) => {
        const r = Math.PI / 180, s1 = Math.sin((a2 - a1) * r / 2), s2 = Math.sin((o2 - o1) * r / 2);
        return 2 * R * Math.asin(Math.min(1, Math.sqrt(s1 * s1 + Math.cos(a1 * r) * Math.cos(a2 * r) * s2 * s2)));
    };
    let same = true, indep = true, sorted = true, cases = 0;
    for (const q of [{ lat: 48.5, lon: 11.7 }, { lat: -89.9, lon: 3 }, { lat: 0, lon: 179.9 }]) {
        const all = G.haversine(q.lat, q.lon, lat, lon);
        for (const m of [0, 2500, 40000, 1.5e6, 2.5e7]) {
            cases++;
            const w = G.haversineWithin(lat, lon, q, m);
            const want = [];
            for (let i = 0; i < N; i++) if (all[i] <= m) want.push(i);
            if (!(w.indices instanceof Uint32Array) || !(w.distances instanceof Float64Array) || w.indices.length !== want.length
                || w.distances.length !== want.length) { same = false; continue; }
            for (let k = 0; k < want.length; k++) {
                if (w.indices[k] !== want[k] || !Object.is(w.distances[k], all[want[k]])) same = false;
                if (k && w.indices[k] <= w.indices[k - 1]) sorted = false;
            }
            const got = new Set(w.indices);
            for (let i = 0; i < N; i += 7) {
                const d = jsHav(q.lat, q.lon, lat[i], lon[i]);
                if (Math.abs(d - m) > 1e-3 && got.has(i) !== d <= m) indep = false;
            }
        }
    }
    ok(same && cases === 15, "haversineWithin returns exactly the haversine array-form points with d <= meters, distances bitwise equal");
    ok(sorted, "haversineWithin indices ascend across worker chunks");
    ok(indep, "haversineWithin membership agrees with an independent JS haversine away from a 1 mm boundary band");
    const L = [10, 10, 10.001, -10], O = [20, 20, 20, 20], q = { lat: 10, lon: 20 };
    const z = G.haversineWithin(L, O, q, 0);
    ok(z.indices.join() === "0,1" && z.distances.join() === "0,0", "meters 0 keeps only coincident points");
    const d2 = G.haversine(10, 20, 10.001, 20);
    ok(G.haversineWithin(L, O, q, d2).indices.join() === "0,1,2", "a point exactly at meters is included");
    ok(G.haversineWithin(L, O, q, d2 * (1 - 2 ** -50)).indices.join() === "0,1", "a point just beyond meters is excluded");
    ok(G.haversineWithin(L, O, q, 1e9).indices.length === 4 && G.haversineWithin(L, O, q, Math.PI * R).indices.length === 4, "meters beyond half the circumference returns every point");
    ok(G.haversineWithin(L, O, q, 1.2e5, { radius: 1e6 }).indices.join() === "0,1,2", "radius option scales distances");
    const e = G.haversineWithin([], [], q, 10);
    ok(e.indices.length === 0 && e.distances.length === 0, "empty input");
});
__kitS.end();
throws(() => G.haversineWithin([0], [0], [0, 0], 1), TypeError, /geo\.haversineWithin: q must be a \{lat, lon\} object/, "array query point");
throws(() => G.haversineWithin([0], [0], { lat: 91, lon: 0 }, 1), RangeError, /q\.lat must be in/, "query latitude 91");
throws(() => G.haversineWithin([0], [0], { lat: 0 }, 1), TypeError, /q\.lon must be a number/, "query without lon");
throws(() => G.haversineWithin([0], [0], { lat: 0, lon: 0 }, -1), RangeError, /meters must be in/, "negative meters");
throws(() => G.haversineWithin([0], [0], { lat: 0, lon: 0 }, NaN), RangeError, /meters must be in/, "NaN meters");
throws(() => G.haversineWithin([0], [0], { lat: 0, lon: 0 }), TypeError, /meters must be a number/, "missing meters");
throws(() => G.haversineWithin([0, 1], [0], { lat: 0, lon: 0 }, 1), RangeError, /differ in length/, "within lat/lon mismatch");
throws(() => G.haversineWithin([0], [0], { lat: 0, lon: 0 }, 1, { k: 1 }), TypeError, /unknown option 'k'/, "within unknown option");
throws(() => G.haversine(91, 0, 0, 0), RangeError, /geo\.haversine: lat1 must be in \[-90, 90\]/, "latitude above 90");
throws(() => G.haversine("1", 0, 0, 0), TypeError, /lat1 must be a number/, "string latitude");
throws(() => G.haversine(0, Infinity, 0, 0), RangeError, /lon1/, "infinite longitude");
throws(() => G.haversine(0, 0, [1, 2], [1]), RangeError, /differ in length/, "lat/lon array mismatch");
throws(() => G.haversine(0, 0, [100], [1]), RangeError, /point 0/, "array latitude out of range");
throws(() => G.haversine(0, 0, 1, 1, { radius: -1 }), RangeError, /radius must be positive/, "negative radius");
throws(() => G.haversine(0, 0, 1, 1, { r: 1 }), TypeError, /unknown option 'r'/, "unknown option");
throws(() => G.vincenty(0, 0, 0.5, 179.7), RangeError, /no convergence/, "nearly antipodal vincenty");
throws(() => G.vincentyDirect(0, 0, 0, -1), RangeError, /distance must be in/, "negative direct distance");
throws(() => G.crossTrack(0, 0, 1, 1, 1, 1), RangeError, /great circle is undefined/, "degenerate path");
throws(() => G.geohashEncode(0, 0, 13), RangeError, /precision/, "precision 13");
throws(() => G.geohashEncode(0, 0, 2.5), RangeError, /precision must be an integer/, "fractional precision");
throws(() => G.geohashEncode(0, 181), RangeError, /lon must be in \[-180, 180\]/, "encode longitude 181");
throws(() => G.geohashDecode("ezs4a"), RangeError, /base-32 geohash/, "invalid geohash digit");
throws(() => G.geohashDecode(""), RangeError, /1-12/, "empty geohash");
throws(() => G.geohashNeighbors(5), TypeError, /hash must be a string/, "non-string hash");
throws(() => G.pointInPolygon(0, 0, [0, 1], [0, 1]), RangeError, /at least 3 vertices/, "two-vertex ring");
throws(() => G.polygonArea([0, 1, 2], [0, 1]), RangeError, /differ in length/, "ring length mismatch");
print(`test_mathx_geo_contract: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} contract failures`);
