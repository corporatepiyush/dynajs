// Signal lab — digital signal processing for machine-condition monitoring and telephony: the native FFT checked
// against the definition, windowed spectra, Kaiser-window FIR design, RBJ biquad IIR filters, decimation,
// Hilbert envelope analysis for bearing faults, overlap-save streaming, Goertzel DTMF decoding and WAV files.
//
// WHAT IT IS
//   FFT          dyna:mathx signal.fft (any length, mixed radix / Bluestein in C); verified here against an O(n²)
//                DFT written out in JS and by Parseval's theorem — the independent cross-check of the native kernels
//   spectra      Hann-windowed magnitude spectra with parabolic peak interpolation for sub-bin frequency accuracy
//   FIR          windowed-sinc low-pass designed with a Kaiser window (signal.kaiserOrd sizes it from the stop-band
//                attenuation, signal.firwin builds it), applied with signal.lfilter; measured attenuation (signal.freqz)
//                is checked against the design target
//   IIR          RBJ "audio EQ cookbook" biquads (high-pass, band-pass) from signal.biquad, run by signal.sosfilt
//   streaming    overlap-save FFT convolution (signal.rfft / irfft) over 4096-sample blocks gives the same output
//                as the direct-form filter, block by block, as a sensor stream arrives
//   diagnostics  a simulated accelerometer on a motor (shaft at 29.5 Hz, a gear mesh, noise) develops an outer-race
//                bearing defect: band-pass around the resonance, Hilbert envelope, envelope spectrum — the ball-pass
//                frequency (BPFO) and its harmonics appear only in the faulty recording
//   DTMF         a phone keypad recording is decoded with signal.goertzel on 40 ms frames with twist and
//                energy checks, including in noise
//   WAV          16-bit PCM RIFF files written and parsed back (and recognised as audio by content sniffing)
//   STFT         512-sample Hann frames hopping 128 track a linear chirp; each frame's interpolated peak is checked
//                against the analytic instantaneous frequency at the frame centre
//   Welch        signal.welch's averaged periodograms of overlapping segments: the white-noise floor must sit at 2σ²/fs, its
//                spread must shrink as 1/√segments, and a tone's integrated power must equal A²/2
//   GCC-PHAT     time-delay estimation between two microphones (fractional delay planted in the frequency
//                domain, plus a strong echo): the phase-transform correlation peaks at the direct path
//
// MODULES  dyna:simd (dot) · dyna:mathx (signal, stats) · dyna:file (sniffType) · dyna:random
//          · dyna:dataframe · dyna:cli (Table)
//
// RUN      dynajs examples/science/signal-lab.js     synthesises the signals and runs every analysis

import { dot } from "dyna:simd";
import { signal, stats } from "dyna:mathx";
import { makeTempDir, writeFile, readBytes, sniffType, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { DataFrame } from "dyna:dataframe";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("dsp");
const rng = new Random(440);

// =============================================================================
// Spectra
// =============================================================================
function spectrum(x, fs) {
    // Hann-windowed single-sided magnitude, zero-padded to a power of two
    const n = 1 << Math.ceil(Math.log2(x.length)),
        w = signal.getWindow("hann", x.length, { sym: true }),
        { re, im } = signal.rfft(
            x.map((v, i) => v * w[i]),
            n,
        ),
        wsum = w.reduce((a, b) => a + b, 0),
        mag = new Float64Array(n / 2);
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
    // kaiserOrd takes the transition width as a fraction of Nyquist; an odd length keeps the delay a whole sample
    let { numtaps: n, beta } = signal.kaiserOrd(attenDb, transition / (fs / 2));
    if (n % 2 === 0) n++;
    const h = signal.firwin(n, cutoff, { window: "kaiser", beta, fs });
    return { h, beta, taps: n };
}
const firFilter = (x, h) => signal.lfilter(h, [1], x);
function biquad(type, fs, f0, q) {
    const sos = signal.biquad(type, f0, { fs, q });
    return (x) => signal.sosfilt(sos, x);
}
function overlapSave(stream, h, block = 4096) {
    // FFT convolution, block by block
    const m = h.length,
        n = block,
        step = n - m + 1;
    const H = signal.rfft(h, n);
    const out = new Float64Array(stream.length),
        history = new Float64Array(n);
    for (let pos = 0; pos < stream.length; pos += step) {
        history.copyWithin(0, step);
        history.fill(0, m - 1);
        for (let i = 0; i < step; i++) history[m - 1 + i] = stream[pos + i] ?? 0;
        const { re, im } = signal.rfft(history);
        for (let k = 0; k < re.length; k++) {
            const r = re[k] * H.re[k] - im[k] * H.im[k],
                i2 = re[k] * H.im[k] + im[k] * H.re[k];
            re[k] = r;
            im[k] = i2;
        }
        const y = signal.irfft(re, im, n);
        for (let i = 0; i < step && pos + i < stream.length; i++) out[pos + i] = y[m - 1 + i];
    }
    return out;
}
function hilbertEnvelope(x) {
    const { re, im } = signal.hilbert(x);
    return Float64Array.from({ length: x.length }, (_, i) => Math.hypot(re[i], im[i]));
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
// Generalised Goertzel: evaluated at the exact tone frequency rather than the nearest DFT bin; |X(f)|².
const goertzel = (x, fs, f) => {
    const { re, im } = signal.goertzel(x, f, fs);
    return re * re + im * im;
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
const { re, im } = signal.fft(xr);
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
const lpResp = signal.freqz(lp.h, null, { n: 4096, fs: FS }),
    respAt = (f) => {
        const k = Math.round((f / FS) * 8192);
        return 20 * Math.log10(Math.hypot(lpResp.re[k], lpResp.im[k]));
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

// =============================================================================
// STFT chirp tracking, Welch PSD, GCC-PHAT time-delay estimation
// =============================================================================
// A linear chirp f(t) = 200 + 1200 t: each 512-sample Hann frame's interpolated peak must follow the analytic
// instantaneous frequency at the frame centre.
const FS4 = 8000,
    chirp = Float32Array.from({ length: 1.5 * FS4 }, (_, i) => {
        const t = i / FS4;
        return Math.sin(2 * Math.PI * (200 * t + 600 * t * t));
    });
const track = [];
for (let at = 0; at + 512 <= chirp.length; at += 128) {
    const est = peaks(spectrum(chirp.subarray(at, at + 512), FS4), 1)[0].hz,
        t = (at + 255.5) / FS4;
    track.push([t, est, 200 + 1200 * t]);
}
const chirpRms = Math.sqrt(track.reduce((a, [, e, f]) => a + (e - f) ** 2, 0) / track.length);
results[
    `STFT follows a 200 -> 2000 Hz chirp over ${track.length} frames (RMS error ${chirpRms.toFixed(2)} Hz, bin ${(FS4 / 512).toFixed(1)} Hz)`
] = chirpRms < 3;

// Welch: average |FFT|² of 50%-overlapping Hann segments. White noise of variance s² has the one-sided density
// 2 s² / fs; a sine of amplitude A carries A²/2 of power, wherever its energy leaks.
function welch(x, fs, seg = 256) {
    const { f, pxx } = signal.welch(x, { fs, nperseg: seg, detrend: false });
    return { psd: pxx, df: f[1] - f[0], segments: Math.floor((x.length - seg / 2) / (seg / 2)) };
}
const FS5 = 1000,
    SIGMA = 0.5,
    TONE_A = 0.8,
    noise = new Float64Array(60 * FS5);
rng.fillNormal(noise, 0, SIGMA);
const noisePsd = welch(noise, FS5),
    interior = noisePsd.psd.slice(4, -4),
    floorMean = interior.reduce((a, b) => a + b, 0) / interior.length,
    floorSd = Math.sqrt(interior.reduce((a, b) => a + (b - floorMean) ** 2, 0) / interior.length);
const toned = noise.map((v, i) => v + TONE_A * Math.sin((2 * Math.PI * 123 * i) / FS5)),
    tonePsd = welch(toned, FS5),
    k123 = Math.round(123 / tonePsd.df),
    tonePower =
        tonePsd.psd.slice(k123 - 4, k123 + 5).reduce((a, b, j) => a + (b - noisePsd.psd[k123 - 4 + j]), 0) * tonePsd.df;
results[
    `Welch PSD of white noise sits at 2s²/fs (${floorMean.toExponential(3)} vs ${((2 * SIGMA ** 2) / FS5).toExponential(3)}), ${noisePsd.segments} segments shrink its spread`
] = Math.abs(floorMean / ((2 * SIGMA ** 2) / FS5) - 1) < 0.03 && floorSd / floorMean < 2 / Math.sqrt(noisePsd.segments);
results[
    `integrating the PSD around 123 Hz recovers the tone's power A²/2 = ${(TONE_A ** 2 / 2).toFixed(3)} (${tonePower.toFixed(3)})`
] = Math.abs(tonePower / (TONE_A ** 2 / 2) - 1) < 0.05;

// GCC-PHAT: two microphones hear the same low-passed source 7.3 samples apart, the second also through a strong
// echo. Whitening the cross-spectrum keeps only phase, so the correlation peak is the direct-path delay.
const NM = 4096,
    DELAY = 7.3,
    src = new Float64Array(NM);
rng.fillNormal(src, 0, 1);
for (let i = 1; i < NM; i++) src[i] += 0.97 * src[i - 1]; // strongly low-passed: plain correlation smears its peak
const delayed = (x, d) => {
    // a fractional circular delay: multiply the spectrum by exp(-i 2 pi k d / n)
    const { re, im } = signal.fft(x),
        n = x.length;
    for (let k = 0; k < n; k++) {
        const f = k <= n / 2 ? k : k - n,
            a = (-2 * Math.PI * f * d) / n,
            c = Math.cos(a),
            sn = Math.sin(a),
            r = re[k];
        re[k] = r * c - im[k] * sn;
        im[k] = r * sn + im[k] * c;
    }
    if (n % 2 === 0) im[n / 2] = 0; // keep the Nyquist bin real so the result stays real
    return signal.ifft(re, im).re;
};
const mic1 = src.map((v) => v + 0.1 * rng.normal()),
    d1 = delayed(src, DELAY),
    d2 = delayed(src, DELAY + 6),
    mic2 = d1.map((v, i) => v + 0.6 * d2[i] + 0.1 * rng.normal());
function gccPhat(a, b, maxLag) {
    const { re: ar, im: ai } = signal.fft(a),
        { re: br, im: bi } = signal.fft(b);
    for (let k = 0; k < a.length; k++) {
        const r = br[k] * ar[k] + bi[k] * ai[k],
            i = bi[k] * ar[k] - br[k] * ai[k],
            m = Math.hypot(r, i) || 1;
        ar[k] = r / m;
        ai[k] = i / m;
    }
    const cc = signal.ifft(ar, ai).re,
        at = (lag) => cc[(lag + a.length) % a.length];
    let best = 0;
    for (let lag = -maxLag; lag <= maxLag; lag++) if (at(lag) > at(best)) best = lag;
    const y0 = at(best - 1),
        y1 = at(best),
        y2 = at(best + 1);
    return best + (0.5 * (y0 - y2)) / (y0 - 2 * y1 + y2);
}
const tdoa = gccPhat(mic1, mic2, 64);
results[
    `GCC-PHAT finds the ${DELAY}-sample direct path through a 0.6 echo 6 samples behind (${tdoa.toFixed(2)} samples)`
] = Math.abs(tdoa - DELAY) < 0.2;

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
        ["STFT chirp", `${track.length} frames, RMS ${chirpRms.toFixed(2)} Hz from the analytic sweep`],
        [
            "Welch PSD",
            `floor ${floorMean.toExponential(3)}/Hz (theory ${((2 * SIGMA ** 2) / FS5).toExponential(3)}); tone power ${tonePower.toFixed(3)}`,
        ],
        ["GCC-PHAT", `delay ${tdoa.toFixed(3)} samples (planted ${DELAY}, echo at +6)`],
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
