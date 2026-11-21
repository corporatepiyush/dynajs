// dyna:mathx signal transforms and filters, best of N wall-clock runs on seeded data. The same samples are written as raw
// little-endian float64 for bench/bench_signal_scipy.py, which times numpy.fft / scipy.signal on them, so the two tables
// compare like for like. Usage: dynajs bench/bench_signal.js [runs] [dataFile]
import { signal as S } from "dyna:mathx";
import { writeFile, Path } from "dyna:file";

const RUNS = Number(scriptArgs[1] ?? 5), OUT = scriptArgs[2];
let s = 0x9e3779b9;
const u = () => ((s = (Math.imul(s ^ (s >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 4294967296);
const vec = (n) => Float64Array.from({ length: n }, () => u() * 2 - 1);
const X = vec(1 << 20), Y = vec(1 << 20), P = vec(1000003), W = vec(4000000), K = vec(2000), C = vec(200000);
if (OUT) {
    const parts = [X, Y, P, W, K, C], total = parts.reduce((t, p) => t + p.length, 0), all = new Float64Array(total);
    let at = 0;
    for (const p of parts) { all.set(p, at); at += p.length; }
    writeFile(new Path(OUT), new Uint8Array(all.buffer));
}
const SOS = S.butter(8, 0.2), FIR = S.firwin(101, 0.3), X1 = X.subarray(0, 1000000);
function best(f) {
    let b = Infinity;
    for (let r = 0; r < RUNS; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); }
    return b;
}
const rows = [
    ["fft complex 2^20", () => S.fft(X, Y)],
    ["fft complex 1e6 (2^6 5^6)", () => S.fft(X1, Y.subarray(0, 1000000))],
    ["fft prime 1000003 (Bluestein)", () => S.fft(P)],
    ["rfft 2^20", () => S.rfft(X)],
    ["irfft 2^20", () => { const r = S.rfft(X); S.irfft(r.re, r.im, X.length); }],
    ["welch 4e6 nperseg=1024", () => S.welch(W, { nperseg: 1024, fs: 1000 })],
    ["sosfilt butter8 1e6", () => S.sosfilt(SOS, X1)],
    ["lfilter fir101 1e6", () => S.lfilter(FIR, [1], X1)],
    ["convolve 2e5 * 2000 (fft)", () => S.convolve(C, K)],
    ["hilbert 2^20", () => S.hilbert(X)],
    ["butter order 8 design", () => { for (let i = 0; i < 1000; i++) S.butter(8, 0.2); }],
    ["firwin 1001 taps", () => S.firwin(1001, 0.3)],
    ["goertzel 1e6", () => S.goertzel(X1, 50, 1000)],
];
print(`best of ${RUNS}`);
for (const [name, f] of rows) print(name.padEnd(32) + best(f).toFixed(2).padStart(10) + " ms");
