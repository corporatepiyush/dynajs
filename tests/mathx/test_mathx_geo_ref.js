// covers: integration dyna:mathx.geo -- every geo function against independent numpy/scipy reference vectors
// Vectors: tests/corpus/ref/geo_vectors.js from tests/ref/gen_geo_ref_vectors.py. WGS84 geodesics there come from integrating
// the geodesic ODE with scipy solve_ivp (DOP853, rtol=1e-12, atol=1e-14 rad; inverse by least_squares shooting, xtol=1e-15),
// whose own error is below 1e-6 m. Vincenty must agree to 1 mm in distance (and 1e-8 deg in position) and 2e-7 deg in azimuth;
// sphere distances to 1e-9 relative, angles to 1e-9 deg; areas to 1e-9 relative; geohash and point-in-polygon exactly.
import { geo } from "dyna:mathx";
import V from "../corpus/ref/geo_vectors.js";

let pass = 0, fail = 0;
const angDiff = (a, b) => Math.abs(((a - b + 540) % 360) - 180);
const relClose = (a, b, rel, abs = 0) => Math.abs(a - b) <= Math.max(abs, rel * Math.abs(b));
function check(c) {
    const r = geo[c.op](...c.args), w = c.want;
    switch (c.op) {
        case "vincenty":
            return relClose(r.distance, w[0], 1e-12, 1e-3) && angDiff(r.azimuth1, w[1]) < 2e-7 && angDiff(r.azimuth2, w[2]) < 2e-7;
        case "vincentyDirect":
            return Math.abs(r.lat - w[0]) < 1e-8 && angDiff(r.lon, w[1]) < 1e-8 && angDiff(r.azimuth, w[2]) < 2e-7;
        case "haversine":
            return typeof r === "number" ? relClose(r, w[0], 1e-9, 1e-9) : r.length === w.length && w.every((v, i) => relClose(r[i], v, 1e-9, 1e-6));
        case "bearing":
            return angDiff(r, w[0]) < 1e-9;
        case "midpoint":
        case "destination":
            return Math.abs(r.lat - w[0]) < 1e-9 && angDiff(r.lon, w[1]) < 1e-9;
        case "crossTrack":
            return relClose(r.crossTrack, w[0], 1e-9, 1e-6) && relClose(r.alongTrack, w[1], 1e-9, 1e-6);
        case "geohashEncode":
            return r === w;
        case "geohashDecode":
            return [r.lat, r.lon, r.latErr, r.lonErr].every((v, i) => v === w[i]);
        case "geohashNeighbors":
            return Object.keys(w).every((k) => r[k] === w[k]) && Object.keys(r).length === 8;
        case "pointInPolygon":
            return typeof w === "boolean" ? r === w : r instanceof Uint8Array && r.length === w.length && w.every((v, i) => r[i] === v);
        case "polygonArea":
            return relClose(r, w[0], 1e-9);
    }
    return false;
}
for (const c of V.cases) {
    let good;
    try { good = check(c); } catch (e) { good = false; print(`  threw ${e}`); }
    if (good) pass++;
    else { fail++; print(`FAIL ${c.op}(${JSON.stringify(c.args).slice(0, 90)}) got ${JSON.stringify(geo[c.op](...c.args)).slice(0, 160)}, want ${JSON.stringify(c.want).slice(0, 160)}`); }
}
print(`test_mathx_geo_ref: ${pass} passed, ${fail} failed (numpy ${V.numpy}, scipy ${V.scipy})`);
if (fail) throw new Error(`${fail} reference mismatches`);
