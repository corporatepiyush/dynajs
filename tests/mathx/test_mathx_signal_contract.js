// covers: contract dyna:mathx.signal -- argument contract and definitional identities of the signal namespace
// Independent of numpy/scipy: transforms are checked against a naive DFT and their own algebra (inverse, Parseval,
// linearity, shift theorem), filters against the time-domain recurrences and the analytic biquad responses, and
// every documented TypeError / RangeError is provoked.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_signal_contract");
import { signal as S } from "dyna:mathx";

let pass = 0, fail = 0;
function ok(c, msg) {
    if (c) pass++;
    else { fail++; print("FAIL " + msg); }
}
function throws(fn, type, re, msg) {
    try { fn(); ok(false, msg + " (did not throw)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
const maxAbs = (a) => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
const maxDiff = (a, b) => { let m = 0; for (let i = 0; i < a.length; i++) m = Math.max(m, Math.abs(a[i] - b[i])); return m; };
let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648) * 2 - 1;
const randVec = (n) => Float64Array.from({ length: n }, rnd);

function dft(re, im, inv = false) {
    const n = re.length, or = new Float64Array(n), oi = new Float64Array(n);
    for (let k = 0; k < n; k++) {
        let sr = 0, si = 0;
        for (let t = 0; t < n; t++) {
            const a = (inv ? 2 : -2) * Math.PI * ((k * t) % n) / n, c = Math.cos(a), s = Math.sin(a);
            sr += re[t] * c - im[t] * s;
            si += re[t] * s + im[t] * c;
        }
        or[k] = inv ? sr / n : sr;
        oi[k] = inv ? si / n : si;
    }
    return { re: or, im: oi };
}

for (const n of [1, 2, 3, 5, 7, 12, 17, 30, 37, 41, 53, 64, 97, 120]) {
    const re = randVec(n), im = randVec(n), want = dft(re, im), got = S.fft(re, im);
    const peak = Math.max(maxAbs(want.re), maxAbs(want.im), 1);
    ok(maxDiff(got.re, want.re) < 1e-12 * peak && maxDiff(got.im, want.im) < 1e-12 * peak, `fft n=${n} equals the naive DFT`);
    const back = S.ifft(got.re, got.im);
    ok(maxDiff(back.re, re) < 1e-13 && maxDiff(back.im, im) < 1e-13, `ifft(fft(x)) == x for n=${n}`);
    ok(re.every((v, i) => v === re[i]), `fft does not modify its input (n=${n})`);
}
for (const n of [65537, 100003, 1 << 18, 3 * 5 * 7 * 11 * 13 * 2]) {
    const x = randVec(n), f = S.fft(x), back = S.ifft(f.re, f.im);
    ok(maxDiff(back.re, x) < 1e-12 && maxAbs(back.im) < 1e-12, `ifft(fft(x)) == x for n=${n}`);
    let e1 = 0, e2 = 0;
    for (let i = 0; i < n; i++) { e1 += x[i] * x[i]; e2 += f.re[i] * f.re[i] + f.im[i] * f.im[i]; }
    ok(Math.abs(e2 / n - e1) < 1e-9 * e1, `Parseval holds for n=${n}`);
}
__kitS.section("fft is linear", () => {
    const n = 50, x = randVec(n), y = randVec(n), fx = S.fft(x), fy = S.fft(y), fs = S.fft(x.map((v, i) => 2 * v - 3 * y[i]));
    ok(maxDiff(fs.re, fx.re.map((v, i) => 2 * v - 3 * fy.re[i])) < 1e-12, "fft is linear");
    const sh = S.fft(x.map((_, i) => x[(i + 3) % n]));
    let e = 0;
    for (let k = 0; k < n; k++) {
        const a = 2 * Math.PI * 3 * k / n, c = Math.cos(a), s = Math.sin(a);
        e = Math.max(e, Math.abs(sh.re[k] - (fx.re[k] * c - fx.im[k] * s)), Math.abs(sh.im[k] - (fx.re[k] * s + fx.im[k] * c)));
    }
    ok(e < 1e-12, "circular shift multiplies bin k by e^{2 pi i 3k/n}");
    ok(S.fft([1, 2, 3]).re instanceof Float64Array && S.fft([5]).re[0] === 5 && S.fft([]).re.length === 0, "fft returns Float64Arrays; n=1 and n=0 are identities");
});
for (const n of [1, 2, 3, 8, 9, 31, 64, 1000, 1001]) {
    const x = randVec(n), r = S.rfft(x), c = S.fft(x), h = (n >> 1) + 1;
    ok(r.re.length === h && maxDiff(r.re, c.re.subarray(0, h)) < 1e-12 * n && maxDiff(r.im, c.im.subarray(0, h)) < 1e-12 * n, `rfft n=${n} is the first n/2+1 FFT bins`);
    ok(r.im[0] === 0 && (n % 2 === 1 || r.im[h - 1] === 0), `rfft n=${n} DC/Nyquist are exactly real`);
    ok(maxDiff(S.irfft(r.re, r.im, n), x) < 1e-13 * Math.max(1, Math.log2(n)), `irfft(rfft(x), n) == x for n=${n}`);
}
ok(S.rfft([1, 2, 3, 4], 8).re.length === 5 && S.rfft([1, 2, 3, 4], 2).re[0] === 3, "rfft n pads with zeros or truncates");
ok(S.irfft([1, 0, 0], [0, 0, 0]).length === 4, "irfft default length is 2*(m-1)");
__kitS.section("block L72", () => {
    const f = S.fftfreq(5, 0.1), r = S.rfftfreq(6, 0.5);
    ok(f.join() === [0, 2, 4, -4, -2].join() && r.join() === [0, 1 / 3, 2 / 3, 1].join(), `fftfreq/rfftfreq (${f}; ${r})`);
});

for (const [name, n] of [["hann", 9], ["hamming", 9], ["blackman", 9], ["bartlett", 9], ["kaiser", 9]]) {
    const o = name === "kaiser" ? { sym: true, beta: 4 } : { sym: true };
    const w = S.getWindow(name, n, o);
    ok(w.every((v, i) => Math.abs(v - w[n - 1 - i]) < 1e-15) && Math.abs(w[4] - 1) < 1e-15, `${name} symmetric window is symmetric with peak 1`);
    const p = S.getWindow(name, 8, name === "kaiser" ? { beta: 4 } : undefined);
    ok(maxDiff(p, w.subarray(0, 8)) < 1e-15, `${name} periodic window of 8 is the symmetric 9 without its last point`);
}
ok(S.getWindow("boxcar", 4).every((v) => v === 1) && S.getWindow("hann", 1)[0] === 1, "boxcar is ones; a 1-point window is [1]");
ok(S.getWindow("kaiser", 7, { beta: 0, sym: true }).every((v) => Math.abs(v - 1) < 1e-15), "kaiser with beta 0 is the boxcar");
ok(S.kaiserBeta(10) === 0 && Math.abs(S.kaiserBeta(60) - 0.1102 * (60 - 8.7)) < 1e-12, "kaiserBeta piecewise formula");
__kitS.section("kaiser", () => {
    const { numtaps, beta } = S.kaiserOrd(65, 0.05);
    const h = S.firwin(numtaps, 0.3, { window: "kaiser", beta });
    const r = S.freqz(h, undefined, { n: 2048 });
    let stop = 0;
    for (let i = 0; i < r.w.length; i++) if (r.w[i] / Math.PI >= 0.3 + 0.05 / 2 + 0.002) stop = Math.max(stop, Math.hypot(r.re[i], r.im[i]));
    ok(numtaps > 0 && -20 * Math.log10(stop) > 64, `kaiserOrd(65 dB) design reaches ${(-20 * Math.log10(stop)).toFixed(1)} dB in the stopband`);
});
__kitS.section("firwin lowpass: unit DC gain, linear phase", () => {
    const h = S.firwin(51, 0.4);
    ok(Math.abs(h.reduce((a, b) => a + b, 0) - 1) < 1e-14 && h.every((v, i) => Math.abs(v - h[50 - i]) < 1e-16), "firwin lowpass: unit DC gain, linear phase");
    const hp = S.firwin(51, 0.4, { passZero: false });
    ok(Math.abs(Math.abs(hp.reduce((a, v, i) => a + v * (i % 2 ? -1 : 1), 0)) - 1) < 1e-14, "firwin highpass: unit gain at Nyquist");
    ok(S.firwin(11, 10, { fs: 100 }).join() === S.firwin(11, 0.2).join(), "firwin fs scales the cutoff");
});

__kitS.section("lfilter is the a[0]-normalised difference equation", () => {
    const b = [0.3, -0.2, 0.1], a = [2, -0.4, 0.3], x = randVec(200), y = S.lfilter(b, a, x), w = new Float64Array(200);
    for (let n = 0; n < 200; n++) {
        let s = 0;
        for (let k = 0; k < 3; k++) if (n >= k) s += b[k] * x[n - k];
        for (let k = 1; k < 3; k++) if (n >= k) s -= a[k] * w[n - k];
        w[n] = s / a[0];
    }
    ok(maxDiff(y, w) < 1e-14, "lfilter is the a[0]-normalised difference equation");
    ok(maxDiff(S.lfilter([1, 2, 3], [1], x), S.convolve(x, [1, 2, 3]).subarray(0, 200)) < 1e-13, "FIR lfilter is the truncated full convolution");
});
__kitS.section("sosfilt takes flat or row-form sections", () => {
    const sos = S.butter(6, 0.25), x = randVec(300);
    const rows = Array.from({ length: sos.length / 6 }, (_, i) => Array.from(sos.subarray(6 * i, 6 * i + 6)));
    ok(sos.length === 18 && S.sosfilt(rows, x).join() === S.sosfilt(sos, x).join(), "sosfilt takes flat or row-form sections");
    let y = x;
    for (const r of rows) y = S.lfilter(r.slice(0, 3), r.slice(3), y);
    ok(maxDiff(S.sosfilt(sos, x), y) < 1e-13, "sosfilt equals cascaded lfilter");
    const f1 = S.sosfreqz(sos, { n: 64 }), f2 = S.sosfreqz(rows, { n: 64, fs: 8 });
    ok(f1.re.join() === f2.re.join() && Math.abs(f2.w[63] - 4 * 63 / 64) < 1e-15, "sosfreqz w scales to fs");
    ok(Math.abs(Math.hypot(f1.re[0], f1.im[0]) - 1) < 1e-12, "butter lowpass has unit DC gain");
    const at = S.sosfreqz(S.butter(5, [0.2, 0.5], { btype: "bandpass" }), { n: 1024 });
    let pk = 0;
    for (let i = 0; i < 1024; i++) pk = Math.max(pk, Math.hypot(at.re[i], at.im[i]));
    ok(Math.abs(pk - 1) < 1e-6, `butter bandpass peaks at unity (${pk})`);
    const bs = S.sosfreqz(S.butter(3, [0.2, 0.5], { btype: "bandstop" }), { n: 1024 });
    ok(Math.hypot(bs.re[0], bs.im[0]) > 0.999999 && Math.hypot(bs.re[1023], bs.im[1023]) > 0.999, "butter bandstop passes DC and Nyquist");
});
__kitS.section("biquad lowpass: |H(0)| = 1, |H(w0)| = Q", () => {
    const fs = 48000, f0 = 1500, q = 2, g = 9, w0 = 2 * Math.PI * f0 / fs;
    const mag = (s, w) => {
        const r = S.sosfreqz(s, { n: 1 });
        const z1 = [Math.cos(w), -Math.sin(w)], z2 = [Math.cos(2 * w), -Math.sin(2 * w)];
        const nr = s[0] + s[1] * z1[0] + s[2] * z2[0], ni = s[1] * z1[1] + s[2] * z2[1];
        const dr = s[3] + s[4] * z1[0] + s[5] * z2[0], di = s[4] * z1[1] + s[5] * z2[1];
        return r && Math.hypot(nr, ni) / Math.hypot(dr, di);
    };
    const bq = (t) => S.biquad(t, f0, { fs, q, gainDb: g });
    ok(Math.abs(mag(bq("lowpass"), w0) - q) < 1e-12 && Math.abs(mag(bq("lowpass"), 0) - 1) < 1e-12, "biquad lowpass: |H(0)| = 1, |H(w0)| = Q");
    ok(Math.abs(mag(bq("highpass"), Math.PI) - 1) < 1e-12 && Math.abs(mag(bq("highpass"), w0) - q) < 1e-12, "biquad highpass: |H(pi)| = 1, |H(w0)| = Q");
    ok(Math.abs(mag(bq("bandpass"), w0) - 1) < 1e-12 && mag(bq("bandpass"), 0) < 1e-12, "biquad bandpass: 0 dB at w0, zero at DC");
    ok(mag(bq("notch"), w0) < 1e-9 && Math.abs(mag(bq("notch"), 0) - 1) < 1e-12, "biquad notch: zero at w0");
    ok([0.1, 0.5, 1, 2, 3].every((w) => Math.abs(mag(bq("allpass"), w) - 1) < 1e-12), "biquad allpass has unit magnitude");
    const A2 = Math.pow(10, g / 20);
    ok(Math.abs(mag(bq("peaking"), w0) - A2) < 1e-12 && Math.abs(mag(bq("peaking"), 0) - 1) < 1e-12, "biquad peaking: gain at w0, unity at DC");
    ok(Math.abs(mag(bq("lowshelf"), 0) - A2) < 1e-12 && Math.abs(mag(bq("lowshelf"), Math.PI) - 1) < 1e-12, "biquad lowshelf: gain at DC");
    ok(Math.abs(mag(bq("highshelf"), Math.PI) - A2) < 1e-12 && Math.abs(mag(bq("highshelf"), 0) - 1) < 1e-12, "biquad highshelf: gain at Nyquist");
    ok(bq("lowpass")[3] === 1 && bq("lowpass").length === 6, "biquad returns a normalised [b0, b1, b2, 1, a1, a2] section");
});
__kitS.section("convolve full is the direct sum", () => {
    const a = randVec(40), b = randVec(7), full = S.convolve(a, b);
    const naive = new Float64Array(46);
    for (let i = 0; i < 40; i++) for (let j = 0; j < 7; j++) naive[i + j] += a[i] * b[j];
    ok(maxDiff(full, naive) < 1e-14, "convolve full is the direct sum");
    ok(maxDiff(S.convolve(a, b, "same"), naive.subarray(3, 43)) < 1e-14 && maxDiff(S.convolve(a, b, "valid"), naive.subarray(6, 40)) < 1e-14, "same / valid windows");
    ok(maxDiff(S.convolve(b, a), full) < 1e-14 && S.convolve(b, a, "valid").length === 34, "convolve is commutative; valid uses the longer length");
    const c = S.correlate(a, b), rb = Array.from(b).reverse();
    ok(maxDiff(c, S.convolve(a, rb)) < 1e-14, "correlate(a, b) = convolve(a, reversed b)");
    const big1 = randVec(5000), big2 = randVec(3000), fb = S.convolve(big1, big2);
    let e = 0;
    for (let i = 0; i < fb.length; i += 997) {
        let s = 0;
        for (let j = 0; j < 3000; j++) if (i - j >= 0 && i - j < 5000) s += big1[i - j] * big2[j];
        e = Math.max(e, Math.abs(s - fb[i]));
    }
    ok(e < 1e-10, `FFT-path convolve matches direct sums (${e})`);
    const o1 = randVec(1025), o2 = randVec(1025), fo = S.convolve(o1, o2);
    let e2 = 0;
    for (const i of [0, 1, 2, 1024, 2046, 2047, 2048]) {
        let s2 = 0;
        for (let j = 0; j < 1025; j++) if (i - j >= 0 && i - j < 1025) s2 += o1[i - j] * o2[j];
        e2 = Math.max(e2, Math.abs(s2 - fo[i]));
    }
    ok(fo.length === 2049 && e2 < 1e-10, `odd full length 2049 = 2*1024+1 does not wrap (${e2})`);
});
__kitS.section("nperseg is clamped to the input length", () => {
    const fs = 1000, n = 20000, x = Float64Array.from({ length: n }, (_, i) => Math.sin(2 * Math.PI * 125 * i / fs) + 0.01 * rnd());
    const { f, pxx } = S.welch(x, { fs, nperseg: 512 });
    let best = 0;
    for (let i = 1; i < pxx.length; i++) if (pxx[i] > pxx[best]) best = i;
    ok(f.length === 257 && f[best] === 125, `welch finds the 125 Hz tone (${f[best]})`);
    const df = f[1] - f[0];
    const power = pxx.reduce((s, v) => s + v, 0) * df;
    ok(Math.abs(power - 0.5) < 0.01, `welch density integrates to the signal power (${power})`);
    const sp = S.welch(x, { fs, nperseg: 512, scaling: "spectrum" }).pxx;
    ok(Math.abs(sp[best] - 0.5) < 0.05, `spectrum scaling reads the tone's power in its bin (${sp[best]})`);
    ok(S.welch(randVec(10)).f.length === 6, "nperseg is clamped to the input length");
});
__kitS.section("hilbert(cos) = cos + i sin", () => {
    const n = 256, x = Float64Array.from({ length: n }, (_, i) => Math.cos(2 * Math.PI * 8 * i / n));
    const h = S.hilbert(x);
    ok(maxDiff(h.re, x) < 1e-13 && maxDiff(h.im, x.map((_, i) => Math.sin(2 * Math.PI * 8 * i / n))) < 1e-13, "hilbert(cos) = cos + i sin");
    const env = h.re.map((v, i) => Math.hypot(v, h.im[i]));
    ok(env.every((v) => Math.abs(v - 1) < 1e-12), "the analytic signal's envelope of a pure tone is 1");
    ok(S.hilbert([1, 2, 3], 8).re.length === 8, "hilbert n zero-pads");
});
__kitS.section("goertzel at an integer bin equals that FFT bin", () => {
    const x = randVec(100), F = S.fft(x);
    const g = S.goertzel(x, 7, 100);
    ok(Math.abs(g.re - F.re[7]) < 1e-12 && Math.abs(g.im - F.im[7]) < 1e-12, "goertzel at an integer bin equals that FFT bin");
    let sr = 0, si = 0;
    for (let i = 0; i < 100; i++) { sr += x[i] * Math.cos(2 * Math.PI * 0.123 * i); si -= x[i] * Math.sin(2 * Math.PI * 0.123 * i); }
    const g2 = S.goertzel(x, 0.123);
    ok(Math.abs(g2.re - sr) < 1e-11 && Math.abs(g2.im - si) < 1e-11, "goertzel at a fractional frequency is the DTFT");
    ok(S.goertzel([], 1).re === 0, "goertzel of an empty signal is 0");
});
__kitS.end();

throws(() => S.fft(), TypeError, /signal\.fft: re must be an array/, "fft without input");
throws(() => S.fft([1, 2], [1]), RangeError, /lengths differ/, "fft re/im length mismatch");
throws(() => S.ifft([1], "x"), TypeError, /signal\.ifft: im must be/, "ifft bad im");
throws(() => S.rfft([1, 2], 0), RangeError, /signal\.rfft: n must be an integer/, "rfft n = 0");
throws(() => S.rfft([1, 2], 2.5), RangeError, /n must be an integer/, "rfft fractional n");
throws(() => S.irfft([1], [0]), RangeError, /explicit n/, "irfft one bin without n");
throws(() => S.irfft([1, 2], [0]), RangeError, /same length/, "irfft re/im mismatch");
throws(() => S.fftfreq(0), RangeError, /signal\.fftfreq: n/, "fftfreq n = 0");
throws(() => S.rfftfreq(4, -1), RangeError, /d must be positive/, "rfftfreq negative d");
throws(() => S.getWindow("tukey", 8), RangeError, /unknown window 'tukey'/, "unknown window");
throws(() => S.getWindow(3, 8), TypeError, /name must be a string/, "window name type");
throws(() => S.getWindow("kaiser", 8), TypeError, /kaiser window needs/, "kaiser without beta");
throws(() => S.getWindow("hann", 8, { symmetric: true }), TypeError, /unknown option 'symmetric'/, "unknown window option");
throws(() => S.kaiserOrd(5, 0.1), RangeError, /at least 8 dB/, "kaiserOrd tiny ripple");
throws(() => S.kaiserOrd(60, 0), RangeError, /width must be in/, "kaiserOrd zero width");
throws(() => S.firwin(10, 0.3, { passZero: false }), RangeError, /odd numtaps/, "even highpass firwin");
throws(() => S.firwin(11, [0.5, 0.3]), RangeError, /strictly/, "firwin decreasing cutoffs");
throws(() => S.firwin(11, 1.2), RangeError, /strictly/, "firwin cutoff above Nyquist");
throws(() => S.firwin(11, 0.3, { window: "kaiser" }), TypeError, /kaiser window needs/, "firwin kaiser without beta");
throws(() => S.lfilter([1], [0, 1], [1, 2]), RangeError, /a\[0\] must be/, "lfilter a[0] = 0");
throws(() => S.lfilter([], [1], [1, 2]), RangeError, /non-empty/, "lfilter empty b");
throws(() => S.sosfilt([1, 2, 3], [1]), RangeError, /6 coefficients per section/, "sos length not a multiple of 6");
throws(() => S.sosfilt([[1, 0, 0, 0, 0, 0]], [1]), RangeError, /a0 = 0/, "sos a0 = 0");
throws(() => S.sosfilt([[1, 0, 0, 1, 0]], [1]), RangeError, /row 0 has 5/, "sos short row");
throws(() => S.biquad("lowpass", 30000, { fs: 48000 }), RangeError, /0 < f0 < fs\/2/, "biquad f0 above Nyquist");
throws(() => S.biquad("bell", 100), RangeError, /unknown type 'bell'/, "unknown biquad type");
throws(() => S.butter(0, 0.3), RangeError, /order must be an integer/, "butter order 0");
throws(() => S.butter(41, 0.3), RangeError, /order must be in \[1, 40\]/, "butter order 41");
throws(() => S.butter(4, 1), RangeError, /strictly inside/, "butter cutoff at Nyquist");
throws(() => S.butter(4, 0.3, { btype: "bandpass" }), RangeError, /needs wn = \[low, high\]/, "bandpass with one cutoff");
throws(() => S.butter(4, [0.4, 0.2], { btype: "bandstop" }), RangeError, /low < high/, "bandstop reversed");
throws(() => S.butter(4, 0.3, { btype: "notch" }), RangeError, /btype/, "unknown btype");
throws(() => S.freqz([1], [1], { n: 0 }), RangeError, /n must be an integer/, "freqz n = 0");
throws(() => S.convolve([], [1]), RangeError, /non-empty/, "convolve empty");
throws(() => S.convolve([1], [1], "circular"), RangeError, /unknown mode 'circular'/, "convolve unknown mode");
throws(() => S.welch([1, 2, 3], { nperseg: 2, noverlap: 2 }), RangeError, /noverlap/, "welch noverlap >= nperseg");
throws(() => S.welch([1, 2, 3], { nfft: 1 }), RangeError, /nfft/, "welch nfft < nperseg");
throws(() => S.welch([1, 2, 3], { detrend: "linear" }), RangeError, /detrend/, "welch linear detrend");
throws(() => S.welch([]), RangeError, /non-empty/, "welch empty");
throws(() => S.hilbert([]), RangeError, /non-empty/, "hilbert empty");
throws(() => S.goertzel([1], "a"), TypeError, /freq and fs must be numbers/, "goertzel freq type");

print(`test_mathx_signal_contract: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} contract failures`);
