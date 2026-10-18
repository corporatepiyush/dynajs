// Signal lab — digital signal processing for machine-condition monitoring and telephony: a radix-2 FFT checked
// against the definition, windowed spectra, Kaiser-window FIR design, RBJ biquad IIR filters, decimation,
// Hilbert envelope analysis for bearing faults, overlap-save streaming, Goertzel DTMF decoding and WAV files.
//
// WHAT IT IS
//   FFT          iterative radix-2 Cooley–Tukey; verified against an O(n²) DFT and by Parseval's theorem
//   spectra      Hann-windowed magnitude spectra with parabolic peak interpolation for sub-bin frequency accuracy
//   FIR          windowed-sinc low-pass designed with a Kaiser window (β from the stop-band attenuation, via the
//                modified Bessel function I0), applied with SIMD dot products; measured attenuation is checked
//                against the design target
//   IIR          RBJ "audio EQ cookbook" biquads (high-pass, band-pass) in direct form I
//   streaming    overlap-save FFT convolution over 4096-sample blocks gives the same output as direct
//                convolution, block by block, as a sensor stream arrives
//   diagnostics  a simulated accelerometer on a motor (shaft at 29.5 Hz, a gear mesh, noise) develops an outer-race
//                bearing defect: band-pass around the resonance, Hilbert envelope, envelope spectrum — the ball-pass
//                frequency (BPFO) and its harmonics appear only in the faulty recording
//   DTMF         a phone keypad recording is decoded with the Goertzel algorithm on 40 ms frames with twist and
//                energy checks, including in noise
//   WAV          16-bit PCM RIFF files written and parsed back (and recognised as audio by content sniffing)
//
// MODULES  dyna:simd (dot, axpy) · dyna:mathx (besseli, stats) · dyna:file (sniffType) · dyna:random
//          · dyna:dataframe · dyna:cli (Table)
//
// RUN      dynajs examples/science/signal-lab.js     synthesises the signals and runs every analysis

import { dot, axpy } from "dyna:simd";
import { besseli, stats } from "dyna:mathx";
import { makeTempDir, writeFile, readBytes, sniffType, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { DataFrame } from "dyna:dataframe";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("dsp");
const rng = new Random(440);

// =============================================================================
// FFT
// =============================================================================
function fft(re, im, inverse = false) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            [re[i], re[j]] = [re[j], re[i]];
            [im[i], im[j]] = [im[j], im[i]];
        }
    }
    for (let len = 2; len <= n; len <<= 1) {
        const ang = ((2 * Math.PI) / len) * (inverse ? 1 : -1),
            wr = Math.cos(ang),
            wi = Math.sin(ang);
        for (let i = 0; i < n; i += len) {
            let cr = 1,
                ci = 0;
            for (let k = 0; k < len / 2; k++) {
                const ar = re[i + k],
                    ai = im[i + k],
                    br = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci,
                    bi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ar + br;
                im[i + k] = ai + bi;
                re[i + k + len / 2] = ar - br;
                im[i + k + len / 2] = ai - bi;
                const t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
    if (inverse)
        for (let i = 0; i < n; i++) {
            re[i] /= n;
            im[i] /= n;
        }
}
function spectrum(x, fs) {
    // Hann-windowed single-sided magnitude
    const n = 1 << Math.ceil(Math.log2(x.length)),
        re = new Float64Array(n),
        im = new Float64Array(n);
    let wsum = 0;
    for (let i = 0; i < x.length; i++) {
        const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (x.length - 1));
        re[i] = x[i] * w;
        wsum += w;
    }
    fft(re, im);
    const mag = new Float64Array(n / 2);
    for (let k = 0; k < n / 2; k++) mag[k] = (2 * Math.hypot(re[k], im[k])) / wsum;
    return { mag, df: fs / n };
}
function peaks(spec, count, minHz = 0) {
    const out = [];
    for (let k = 2; k < spec.mag.length - 2; k++) {
        if (k * spec.df < minHz || !(spec.mag[k] > spec.mag[k - 1] && spec.mag[k] >= spec.mag[k + 1])) continue;
        const a = spec.mag[k - 1],
            b = spec.mag[k],
            c = spec.mag[k + 1],
            p = (0.5 * (a - c)) / (a - 2 * b + c); // parabolic interpolation
        out.push({ hz: (k + p) * spec.df, amp: b - 0.25 * (a - c) * p });
    }
    return out.sort((x, y) => y.amp - x.amp).slice(0, count);
}

// =============================================================================
// FIR (Kaiser) and IIR (RBJ biquad)
// =============================================================================
function kaiserLowpass(fs, cutoff, transition, attenDb) {
    const beta =
        attenDb > 50
            ? 0.1102 * (attenDb - 8.7)
            : attenDb >= 21
              ? 0.5842 * (attenDb - 21) ** 0.4 + 0.07886 * (attenDb - 21)
              : 0;
    let n = Math.ceil((attenDb - 8) / (2.285 * 2 * Math.PI * (transition / fs))) + 1;
    if (n % 2 === 0) n++;
    const h = new Float32Array(n),
        m = (n - 1) / 2,
        fc = cutoff / fs,
        i0b = besseli(0, beta);
    for (let i = 0; i < n; i++) {
        const t = i - m,
            sinc = t === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * t) / (Math.PI * t);
        h[i] = sinc * (besseli(0, beta * Math.sqrt(1 - (t / m) ** 2)) / i0b);
    }
    const s = h.reduce((a, b) => a + b, 0);
    for (let i = 0; i < n; i++) h[i] /= s;
    return { h, beta, taps: n };
}
function firFilter(x, h) {
    // direct form with SIMD dot products
    const n = h.length,
        rev = Float32Array.from(h).reverse(),
        padded = new Float32Array(x.length + n - 1);
    padded.set(x, n - 1);
    const y = new Float32Array(x.length);
    for (let i = 0; i < x.length; i++) y[i] = dot(padded.subarray(i, i + n), rev);
    return y;
}
function biquad(type, fs, f0, q) {
    const w0 = (2 * Math.PI * f0) / fs,
        alpha = Math.sin(w0) / (2 * q),
        cw = Math.cos(w0);
    const c = type === "highpass" ? [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2] : [alpha, 0, -alpha];
    const a0 = 1 + alpha,
        a = [(-2 * cw) / a0, (1 - alpha) / a0],
        b = c.map((v) => v / a0);
    return (x) => {
        const y = new Float32Array(x.length);
        let x1 = 0,
            x2 = 0,
            y1 = 0,
            y2 = 0;
        for (let i = 0; i < x.length; i++) {
            const v = b[0] * x[i] + b[1] * x1 + b[2] * x2 - a[0] * y1 - a[1] * y2;
            x2 = x1;
            x1 = x[i];
            y2 = y1;
            y1 = v;
            y[i] = v;
        }
        return y;
    };
}
function overlapSave(stream, h, block = 4096) {
    // FFT convolution, block by block
    const m = h.length,
        n = block,
        step = n - m + 1;
    const H = { re: new Float64Array(n), im: new Float64Array(n) };
    H.re.set(h);
    fft(H.re, H.im);
    const out = new Float32Array(stream.length),
        history = new Float64Array(n);
    for (let pos = 0; pos < stream.length; pos += step) {
        history.copyWithin(0, step);
        history.fill(0, m - 1);
        for (let i = 0; i < step; i++) history[m - 1 + i] = stream[pos + i] ?? 0;
        const re = Float64Array.from(history),
            im = new Float64Array(n);
        fft(re, im);
        for (let k = 0; k < n; k++) {
            const r = re[k] * H.re[k] - im[k] * H.im[k],
                i2 = re[k] * H.im[k] + im[k] * H.re[k];
            re[k] = r;
            im[k] = i2;
        }
        fft(re, im, true);
        for (let i = 0; i < step && pos + i < stream.length; i++) out[pos + i] = re[m - 1 + i];
    }
    return out;
}
function hilbertEnvelope(x) {
    const n = 1 << Math.ceil(Math.log2(x.length)),
        re = new Float64Array(n),
        im = new Float64Array(n);
    re.set(x);
    fft(re, im);
    for (let k = 1; k < n / 2; k++) {
        re[k] *= 2;
        im[k] *= 2;
    }
    for (let k = n / 2 + 1; k < n; k++) {
        re[k] = 0;
        im[k] = 0;
    }
    fft(re, im, true);
    return Float32Array.from({ length: x.length }, (_, i) => Math.hypot(re[i], im[i]));
}

// =============================================================================
// WAV
// =============================================================================
function writeWav(path, samples, fs) {
    const n = samples.length,
        buf = new ArrayBuffer(44 + n * 2),
        v = new DataView(buf),
        str = (o, s) => [...s].forEach((c, i) => v.setUint8(o + i, c.charCodeAt(0)));
    str(0, "RIFF");
    v.setUint32(4, 36 + n * 2, true);
    str(8, "WAVE");
    str(12, "fmt ");
    v.setUint32(16, 16, true);
    v.setUint16(20, 1, true);
    v.setUint16(22, 1, true);
    v.setUint32(24, fs, true);
    v.setUint32(28, fs * 2, true);
    v.setUint16(32, 2, true);
    v.setUint16(34, 16, true);
    str(36, "data");
    v.setUint32(40, n * 2, true);
    const peak = Math.max(...samples.map(Math.abs)) || 1;
    for (let i = 0; i < n; i++) v.setInt16(44 + i * 2, Math.round((samples[i] / peak) * 32000), true);
    writeFile(path, new Uint8Array(buf));
}
function readWav(path) {
    const b = readBytes(path),
        v = new DataView(b.buffer, b.byteOffset, b.byteLength),
        tag = (o) => String.fromCharCode(...b.subarray(o, o + 4));
    if (tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("not a WAV file");
    let off = 12,
        fs = 0,
        bits = 0,
        data = null;
    while (off + 8 <= b.length) {
        const id = tag(off),
            len = v.getUint32(off + 4, true);
        if (id === "fmt ") {
            fs = v.getUint32(off + 12, true);
            bits = v.getUint16(off + 22, true);
        }
        if (id === "data") data = b.subarray(off + 8, off + 8 + len);
        off += 8 + len + (len & 1);
    }
    if (bits !== 16 || !data) throw new Error("only 16-bit PCM");
    return {
        fs,
        samples: Float32Array.from(
            { length: data.length / 2 },
            (_, i) => v.getInt16(data.byteOffset - b.byteOffset + i * 2, true) / 32768,
        ),
    };
}

// =============================================================================
// Goertzel DTMF
// =============================================================================
const ROWS = [697, 770, 852, 941],
    COLS = [1209, 1336, 1477, 1633],
    KEYS = ["123A", "456B", "789C", "*0#D"];
// Generalised Goertzel: evaluated at the exact tone frequency rather than the nearest DFT bin.
const goertzel = (x, fs, f) => {
    const w = (2 * Math.PI * f) / fs,
        c = 2 * Math.cos(w);
    let s1 = 0,
        s2 = 0;
    for (const v of x) {
        const s = v + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
};
function decodeDtmf(x, fs) {
    const frame = Math.round(fs * 0.04),
        out = [];
    let last = null,
        run = 0;
    for (let i = 0; i + frame <= x.length; i += frame) {
        const f = x.subarray(i, i + frame),
            energy = dot(f, f);
        const r = ROWS.map((hz) => goertzel(f, fs, hz)),
            c = COLS.map((hz) => goertzel(f, fs, hz));
        const ri = r.indexOf(Math.max(...r)),
            ci = c.indexOf(Math.max(...c));
        const tonal = (r[ri] + c[ci]) / ((energy * frame) / 2 + 1e-9),
            twist = r[ri] / c[ci];
        const key =
            tonal > 0.6 &&
            twist > 0.25 &&
            twist < 4 &&
            r.filter((v) => v > r[ri] / 4).length === 1 &&
            c.filter((v) => v > c[ci] / 4).length === 1
                ? KEYS[ri][ci]
                : null;
        if (key === last) run++;
        else {
            last = key;
            run = 1;
        }
        if (key && run === 2) out.push(key);
    }
    return out.join("");
}

// =============================================================================
// Experiments
// =============================================================================
const results = {};

// FFT against the definition and Parseval.
const N = 256,
    xr = Float64Array.from({ length: N }, () => rng.normal());
const re = Float64Array.from(xr),
    im = new Float64Array(N);
fft(re, im);
let maxErr = 0;
for (let k = 0; k < N; k++) {
    let sr = 0,
        si = 0;
    for (let t = 0; t < N; t++) {
        sr += xr[t] * Math.cos((2 * Math.PI * k * t) / N);
        si -= xr[t] * Math.sin((2 * Math.PI * k * t) / N);
    }
    maxErr = Math.max(maxErr, Math.hypot(sr - re[k], si - im[k]));
}
const energyT = xr.reduce((s, v) => s + v * v, 0),
    energyF = re.reduce((s, v, k) => s + v * v + im[k] * im[k], 0) / N;
results[`FFT matches the DFT definition (max error ${maxErr.toExponential(1)}) and conserves energy`] =
    maxErr < 1e-9 && Math.abs(energyT - energyF) < 1e-9 * energyT;

// Peak interpolation accuracy.
const FS = 12000;
const tone = Float32Array.from(
    { length: 8192 },
    (_, i) =>
        Math.sin((2 * Math.PI * 1234.5 * i) / FS) +
        0.5 * Math.sin((2 * Math.PI * 3210.25 * i) / FS) +
        rng.normal(0, 0.05),
);
const tp = peaks(spectrum(tone, FS), 2);
results[`peaks at ${tp.map((p) => p.hz.toFixed(2)).join(" and ")} Hz from ${(FS / 8192).toFixed(2)} Hz bins`] =
    Math.abs(tp[0].hz - 1234.5) < 0.3 &&
    Math.abs(tp[1].hz - 3210.25) < 0.3 &&
    Math.abs(tp[1].amp / tp[0].amp - 0.5) < 0.05;

// Kaiser FIR: design for 60 dB, measure.
const lp = kaiserLowpass(FS, 1150, 300, 60); // passband to 1 kHz, stopband from 1.3 kHz
const respAt = (f) => {
    const re2 = new Float64Array(8192),
        im2 = new Float64Array(8192);
    re2.set(lp.h);
    fft(re2, im2);
    const k = Math.round((f / FS) * 8192);
    return 20 * Math.log10(Math.hypot(re2[k], im2[k]));
};
const stopband = Math.max(...[1310, 1500, 2000, 3000, 5000].map(respAt)),
    passband = Math.min(...[100, 500, 990].map(respAt));
results[
    `Kaiser FIR (${lp.taps} taps, β=${lp.beta.toFixed(2)}): passband ${passband.toFixed(3)} dB, stopband ${stopband.toFixed(1)} dB`
] = stopband < -57 && passband > -0.1;
const direct = firFilter(tone, lp.h),
    streamed = overlapSave(tone, lp.h);
let osErr = 0;
for (let i = 0; i < tone.length; i++) osErr = Math.max(osErr, Math.abs(direct[i] - streamed[i]));
results[`overlap-save streaming matches direct convolution (max diff ${osErr.toExponential(1)})`] = osErr < 1e-4;
const mix = Float32Array.from(
    { length: 8192 },
    (_, i) => Math.sin((2 * Math.PI * 500 * i) / FS) + Math.sin((2 * Math.PI * 3000 * i) / FS),
);
const lpSpec = spectrum(firFilter(mix, lp.h).subarray(lp.taps), FS),
    ampAt = (hz) => Math.max(...lpSpec.mag.slice(Math.round(hz / lpSpec.df) - 2, Math.round(hz / lpSpec.df) + 3));
results[
    `filtering 500 Hz + 3 kHz keeps ${ampAt(500).toFixed(3)} of the first and ${ampAt(3000).toExponential(1)} of the second`
] = Math.abs(ampAt(500) - 1) < 0.1 && ampAt(3000) < 0.003;

// Bearing diagnostics.
const FS2 = 20000,
    SHAFT = 29.5,
    BPFO = 3.57 * SHAFT,
    RES = 3200;
function motor(faulty, seconds = 2) {
    const n = FS2 * seconds,
        x = new Float32Array(n);
    let ring = 0,
        phase = 0;
    for (let i = 0; i < n; i++) {
        const t = i / FS2;
        x[i] =
            0.8 * Math.sin(2 * Math.PI * SHAFT * t) + 0.3 * Math.sin(2 * Math.PI * SHAFT * 23 * t) + rng.normal(0, 0.4);
        if (faulty) {
            phase += BPFO / FS2;
            if (phase >= 1) {
                phase -= 1;
                ring = 1.2 * (0.8 + 0.4 * rng.nextFloat());
            }
            x[i] += ring * Math.sin(2 * Math.PI * RES * t);
            ring *= Math.exp(-1 / (0.0015 * FS2));
        }
    }
    return x;
}
const decim = (x, f) => Float32Array.from({ length: Math.floor(x.length / f) }, (_, i) => x[i * f]);
function envelopeAnalysis(x) {
    const band = biquad("bandpass", FS2, RES, 4)(biquad("highpass", FS2, 1000, 0.707)(x));
    const env = hilbertEnvelope(band),
        m = stats.mean(env);
    const envLp = firFilter(
        env.map((v) => v - m),
        kaiserLowpass(FS2, 900, 300, 50).h,
    );
    const d = decim(envLp, 10),
        spec = spectrum(d, FS2 / 10);
    const near = (f) => {
        const k = Math.round(f / spec.df);
        return Math.max(...spec.mag.slice(k - 2, k + 3));
    };
    const floor = stats.median(Array.from(spec.mag.slice(5, 400)));
    return { bpfo: near(BPFO) / floor, bpfo2: near(2 * BPFO) / floor, top: peaks(spec, 3, 20) };
}
const healthy = motor(false),
    faulty = motor(true);
writeWav(work.join("healthy.wav"), healthy, FS2);
writeWav(work.join("faulty.wav"), faulty, FS2);
const faultyBack = readWav(work.join("faulty.wav"));
results["WAV files round-trip and sniff as audio"] =
    sniffType(work.join("faulty.wav")).includes("wav") &&
    faultyBack.fs === FS2 &&
    faultyBack.samples.length === faulty.length;
const hEnv = envelopeAnalysis(readWav(work.join("healthy.wav")).samples),
    fEnv = envelopeAnalysis(faultyBack.samples);
results[
    `envelope spectrum: BPFO ${BPFO.toFixed(1)} Hz at ${fEnv.bpfo.toFixed(1)}x the noise floor when faulty, ${hEnv.bpfo.toFixed(1)}x when healthy`
] = fEnv.bpfo > 8 && fEnv.bpfo2 > 4 && hEnv.bpfo < 4;
results[`the strongest envelope line is a BPFO harmonic (${fEnv.top[0].hz.toFixed(1)} Hz)`] =
    Math.abs(fEnv.top[0].hz - BPFO) < 2 || Math.abs(fEnv.top[0].hz - 2 * BPFO) < 2;

// DTMF in noise.
const FS3 = 8000,
    number = "0157*24#9";
const dial = [];
for (const key of number) {
    const r = ROWS[KEYS.findIndex((row) => row.includes(key))],
        c = COLS[KEYS.find((row) => row.includes(key)).indexOf(key)];
    for (let i = 0; i < FS3 * 0.12; i++)
        dial.push(0.5 * Math.sin((2 * Math.PI * r * i) / FS3) + 0.45 * Math.sin((2 * Math.PI * c * i) / FS3));
    for (let i = 0; i < FS3 * 0.08; i++) dial.push(0);
}
const clean = Float32Array.from(dial),
    noisy = Float32Array.from(dial, (v) => v + rng.normal(0, 0.15)),
    speech = Float32Array.from(dial, (v, i) =>
        v === 0 ? 0.6 * Math.sin((2 * Math.PI * 300 * i) / FS3) * Math.sin((2 * Math.PI * 3 * i) / FS3) : v,
    );
results[`Goertzel decodes ${number} clean and in noise`] =
    decodeDtmf(clean, FS3) === number && decodeDtmf(noisy, FS3) === number;
results["speech-like tones between digits are not mistaken for keys"] = decodeDtmf(speech, FS3) === number;

const df = new DataFrame({ env: Float64Array.from(fEnv.top.map((p) => p.hz)) });
print("signal-lab");
print(
    table([
        ["test", "result"],
        ["FFT vs DFT", `max error ${maxErr.toExponential(1)}, Parseval ok`],
        ["peak interpolation", tp.map((p) => `${p.hz.toFixed(2)} Hz`).join(", ")],
        ["Kaiser low-pass", `${lp.taps} taps, β ${lp.beta.toFixed(3)}, stopband ${stopband.toFixed(1)} dB`],
        ["overlap-save", `max diff ${osErr.toExponential(1)}`],
        ["bearing (healthy)", `BPFO/floor ${hEnv.bpfo.toFixed(1)}`],
        [
            "bearing (faulty)",
            `BPFO/floor ${fEnv.bpfo.toFixed(1)}, 2xBPFO ${fEnv.bpfo2.toFixed(1)}; lines ${fEnv.top.map((p) => p.hz.toFixed(1)).join(", ")} Hz (mean ${df.MEAN("env").toFixed(1)})`,
        ],
        ["DTMF", `${decodeDtmf(noisy, FS3)} from a noisy line`],
    ]),
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
