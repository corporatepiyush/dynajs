// dyna:mathx geo, best of N wall-clock runs on seeded points. The same points are written as raw little-endian float64 for
// bench/bench_geo_numpy.py, which times the vectorised numpy equivalents (there is no geodesy in scipy), so the two tables
// compare like for like. Usage: dynajs bench/bench_geo.js [runs] [dataFile]
import { geo } from "dyna:mathx";
import { writeFile, Path } from "dyna:file";

const RUNS = Number(scriptArgs[1] ?? 5), OUT = scriptArgs[2];
let s = 0x9e3779b9;
const u = () => ((s = (Math.imul(s ^ (s >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 4294967296);
const N = 2000000, LAT = Float64Array.from({ length: N }, () => u() * 170 - 85), LON = Float64Array.from({ length: N }, () => u() * 360 - 180);
const K = 20000, RING = 200;
const RLAT = Float64Array.from({ length: RING }, (_, i) => 10 + (i % 2 ? 30 : 60) * Math.sin((2 * Math.PI * i) / RING));
const RLON = Float64Array.from({ length: RING }, (_, i) => 20 + (i % 2 ? 30 : 60) * Math.cos((2 * Math.PI * i) / RING));
if (OUT) {
    const all = new Float64Array(2 * N + 2 * RING);
    all.set(LAT, 0);
    all.set(LON, N);
    all.set(RLAT, 2 * N);
    all.set(RLON, 2 * N + RING);
    writeFile(new Path(OUT), new Uint8Array(all.buffer));
}
function best(f) {
    let b = Infinity;
    for (let r = 0; r < RUNS; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); }
    return b;
}
const rows = [
    ["haversine 1 -> 2e6 points", () => geo.haversine(52.52, 13.405, LAT, LON)],
    ["haversineWithin 2e6, 500 km", () => geo.haversineWithin(LAT, LON, { lat: 52.52, lon: 13.405 }, 5e5)],
    ["pointInPolygon 2e6 x 200-gon", () => geo.pointInPolygon(LAT, LON, RLAT, RLON)],
    ["vincenty 2e4 pairs", () => { for (let i = 0; i < K; i++) geo.vincenty(LAT[i], LON[i], LAT[i + K] * 0.3, LON[i + K] * 0.3); }],
    ["geohashEncode 2e4 (p=9)", () => { for (let i = 0; i < K; i++) geo.geohashEncode(LAT[i], LON[i], 9); }],
    ["polygonArea 200-gon x 1e3", () => { for (let i = 0; i < 1000; i++) geo.polygonArea(RLAT, RLON); }],
];
print(`best of ${RUNS}`);
for (const [name, f] of rows) print(name.padEnd(32) + best(f).toFixed(2).padStart(10) + " ms");
