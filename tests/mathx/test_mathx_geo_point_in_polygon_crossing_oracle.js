// covers: contract dyna:mathx.geo.pointInPolygon -- the even-odd crossing test is branch-free and decides exactly as before
// The ring scan now flips the inside bit with in ^= straddle & (lon < x) instead of branching on the straddle test; the
// crossing longitude x is computed with the edge's own latitude difference when the edge straddles the point and with
// a dummy divisor of 1 otherwise, so every decision uses the same arithmetic as the branchy loop. Oracle: the textbook
// even-odd JS loop must agree with the scalar and array forms for random points around a circle ring, a concave star,
// points exactly on vertices and edges, horizontal edges, a degenerate ring, the latitude and longitude limits and
// signed zeros, and a triangle.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_geo_point_in_polygon_crossing_oracle");
import { geo } from "dyna:mathx";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (!Object.is(got, want)) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
function pipRef(lat, lon, rl, ro) {
    const n = rl.length;
    let inside = false;
    for (let i = 0, j = n - 1; i < n; j = i++) {
        const yi = rl[i], yj = rl[j];
        if ((yi > lat) !== (yj > lat)) {
            const x = ro[i] + ((lat - yi) * (ro[j] - ro[i])) / (yj - yi);
            if (lon < x) inside = !inside;
        }
    }
    return inside;
}
let seed = 12345;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);

const rings = {
    circle: [Float64Array.from({ length: 64 }, (_, k) => 41 + 0.8 * Math.sin((k * Math.PI) / 32)), Float64Array.from({ length: 64 }, (_, k) => -73 + 0.8 * Math.cos((k * Math.PI) / 32))],
    star: [Float64Array.from({ length: 22 }, (_, k) => (k % 2 ? 0.35 : 1) * Math.sin((k * Math.PI) / 11)), Float64Array.from({ length: 22 }, (_, k) => (k % 2 ? 0.35 : 1) * Math.cos((k * Math.PI) / 11))],
    square: [Float64Array.of(0, 0, 1, 1), Float64Array.of(0, 1, 1, 0)],
    flatEdges: [Float64Array.of(0, 0, 0.5, 0.5, 1, 1), Float64Array.of(0, 2, 2, 1, 1, 0)],
    degenerate: [Float64Array.of(1, 1, 1), Float64Array.of(0, 1, 2)],
    triangle: [Float64Array.of(0, 1, 0), Float64Array.of(0, 0.5, 1)],
};

__kitS.section("scalar and array forms equal the even-odd JS loop", () => {
    for (const [nm, [rl, ro]] of Object.entries(rings)) {
        let mnA = Infinity, mxA = -Infinity, mnO = Infinity, mxO = -Infinity;
        for (let k = 0; k < rl.length; k++) { mnA = Math.min(mnA, rl[k]); mxA = Math.max(mxA, rl[k]); mnO = Math.min(mnO, ro[k]); mxO = Math.max(mxO, ro[k]); }
        const pts = [];
        for (let k = 0; k < 3000; k++) pts.push([mnA - 0.3 + (mxA - mnA + 0.6) * rnd(), mnO - 0.3 + (mxO - mnO + 0.6) * rnd()]);
        for (let k = 0; k < rl.length; k++) {
            const j = (k + 1) % rl.length;
            pts.push([rl[k], ro[k]], [(rl[k] + rl[j]) / 2, (ro[k] + ro[j]) / 2], [rl[k], (ro[k] + ro[j]) / 2], [(rl[k] + rl[j]) / 2, ro[k]]);
        }
        pts.push([90, 0.5], [-90, 0.5], [0.5, 180], [0.5, -180], [0, -180], [-0, -180], [-0, 0], [0, -0]);
        const lat = Float64Array.from(pts, (p) => p[0]), lon = Float64Array.from(pts, (p) => p[1]);
        const mask = geo.pointInPolygon(lat, lon, rl, ro);
        let badA = 0, badS = 0, ins = 0, first = "";
        for (let k = 0; k < pts.length; k++) {
            const want = pipRef(lat[k], lon[k], rl, ro);
            if (want) ins++;
            if (mask[k] !== (want ? 1 : 0)) { badA++; if (!first) first = "array@" + lat[k] + "," + lon[k]; }
            if (geo.pointInPolygon(lat[k], lon[k], rl, ro) !== want) { badS++; if (!first) first = "scalar@" + lat[k] + "," + lon[k]; }
        }
        eq(badA, 0, nm + " array mismatches (" + first + ")");
        eq(badS, 0, nm + " scalar mismatches (" + first + ")");
        if (nm === "circle" || nm === "star" || nm === "square") eq(ins > 100 && ins < pts.length - 100, true, nm + " mixes inside and outside points (" + ins + ")");
    }
});

__kitS.section("a large batch over the parallel path matches", () => {
    const [rl, ro] = rings.star, n = 200000;
    const lat = Float64Array.from({ length: n }, () => -1.2 + 2.4 * rnd()), lon = Float64Array.from({ length: n }, () => -1.2 + 2.4 * rnd());
    const mask = geo.pointInPolygon(lat, lon, rl, ro);
    let bad = 0;
    for (let k = 0; k < n; k++) if (mask[k] !== (pipRef(lat[k], lon[k], rl, ro) ? 1 : 0)) bad++;
    eq(bad, 0, "star 200K mismatches");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_mathx_geo_point_in_polygon_crossing_oracle: " + checks + " checks passed");
__kitS.end();
