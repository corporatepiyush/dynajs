#!/usr/bin/env python3
"""Reference vectors for the dyna:mathx signal namespace, from numpy.fft and scipy.signal.

    python3 tests/ref/gen_signal_ref_vectors.py > tests/corpus/ref/signal_vectors.js

Needs numpy and scipy (made with the versions recorded in the output). Inputs are not stored:
each case names a generator ["g", n, seed] that tests/mathx/test_mathx_signal_ref.js rebuilds with
the same closed form (sig() below), so the file holds only answers, printed to 11 significant
digits (see num()). Every result is flattened the way flat() in the suite flattens it ({re, im} -> re then im,
{w, re, im}, {f, pxx}); "stride" keeps every k-th value of a long answer. biquad has no scipy
counterpart with the same Q convention, so it is the RBJ Audio EQ Cookbook written out here.
"""
import json
import math
import os
import sys

import numpy as np
import scipy
from scipy import signal as ss


def sig(n, seed):
    i = np.arange(n, dtype=float)
    return np.sin(0.37 * seed * i + seed) + 0.5 * np.cos(1.3 * i + 0.11 * seed) + 0.01 * seed * ((i * 7 + seed) % 5)


def G(n, seed):
    return ["g", n, seed]



def pinned(name, have):
    """The version written into the vectors is the pin in tests/ref/requirements.txt, never the
    running one: a runtime version string made the file's sha256 differ between two machines
    that computed the same numbers. A different running version is reported on stderr."""
    here = os.path.dirname(os.path.abspath(__file__))
    for line in open(os.path.join(here, "requirements.txt")):
        if line.startswith(name + "=="):
            pin = line.strip().split("==", 1)[1]
            if pin != have:
                sys.stderr.write("%s: %s %s is running, the vectors record the pin %s\n" % (os.path.basename(__file__), name, have, pin))
            return pin
    raise SystemExit("tests/ref/requirements.txt pins no " + name)

JITTER = int(os.environ.get("DYNA_REF_ULP_JITTER", "0"))


def num(x, peak):
    """A value is written on a power-of-ten grid of at most 1e-11 of its part's peak and to at
    most 11 significant digits, and one under 1e-12 of the peak as 0: an FFT's near-zero bins are
    rounding noise whose exact bits depend on the FFT kernel and the machine, so printing them
    made the file's sha256 differ across machines. The suite compares with 1e-9 relative plus
    1e-11 of the part's peak; the rounding stays within half of that floor. DYNA_REF_ULP_JITTER=k
    moves each value by k ulps of its part's peak first, the way another FFT kernel would;
    `./dynajs --std tools/test-data.js repro` requires the file not to change."""
    x = float(x)
    if math.isnan(x):
        return "NaN"
    if math.isinf(x):
        return "Infinity" if x > 0 else "-Infinity"
    if JITTER and peak:
        x += JITTER * math.ulp(peak)
    if abs(x) < 1e-12 * peak or x == 0.0:
        return 0
    grid = 10.0 ** (math.floor(math.log10(peak)) - 11)
    return float("%.11g" % (round(x / grid) * grid))


def flat(*parts):
    out = []
    for p in parts:
        out.extend(np.asarray(p, dtype=float).ravel().tolist())
    return out


CASES = []


def case(op, args, want, stride=1):
    parts = want if isinstance(want, list) and want and hasattr(want[0], "__len__") else [want]
    w = []
    for p in parts:
        vals = flat(p)[::stride]
        peak = max((abs(v) for v in vals if math.isfinite(v)), default=0.0)
        w.extend(num(v, peak) for v in vals)
    c = {"op": op, "args": args, "want": w}
    if stride != 1:
        c["stride"] = stride
    CASES.append(c)


def cx(z):
    return [z.real, z.imag]


FFT_N = list(range(1, 25)) + [26, 30, 31, 32, 36, 40] + [45, 48, 49, 64, 77, 81, 97, 100, 121, 125, 127, 128, 210, 243, 256, 257, 289, 361,
                              529, 961, 1009, 1024, 2018, 3000, 4096, 6561, 10007]
for n in FFT_N:
    x = sig(n, 1 + n % 7)
    st = 1 if n <= 64 else max(1, n // 24)
    case("fft", [G(n, 1 + n % 7)], cx(np.fft.fft(x)), st)
for n in [1, 2, 3, 8, 15, 31, 37, 64]:
    re, im = sig(n, 2), sig(n, 5)
    z = np.fft.ifft(re + 1j * im)
    case("ifft", [G(n, 2), G(n, 5)], cx(z))
    case("fft", [G(n, 2), G(n, 5)], cx(np.fft.fft(re + 1j * im)))
for n in list(range(1, 18)) + [31, 32, 33, 100, 255, 256, 1000, 1001]:
    st = 1 if n <= 64 else max(1, n // 48)
    case("rfft", [G(n, 3)], cx(np.fft.rfft(sig(n, 3))), st)
for n, m in [(10, 16), (16, 10), (7, 9), (9, 7), (100, 64), (5, 1), (33, 128)]:
    case("rfft", [G(n, 4), m], cx(np.fft.rfft(sig(n, 4), m)))
for h in [1, 2, 3, 5, 9, 17, 33]:
    re, im = sig(h, 6), sig(h, 7)
    if h > 1:
        case("irfft", [G(h, 6), G(h, 7)], np.fft.irfft(re + 1j * im))
    for m in [1, 2 * h - 1, 2 * h + 3, max(1, h // 2)]:
        case("irfft", [G(h, 6), G(h, 7), m], np.fft.irfft(re + 1j * im, m))
for n, d in [(1, 1), (2, 1), (7, 0.5), (8, 0.1), (9, 2.5), (16, 1 / 48000)]:
    case("fftfreq", [n, d], np.fft.fftfreq(n, d))
    case("rfftfreq", [n, d], np.fft.rfftfreq(n, d))

for name in ["boxcar", "hann", "hamming", "blackman", "bartlett"]:
    for n in [1, 2, 3, 5, 8, 16]:
        for sym in [False, True]:
            case("getWindow", [name, n, {"sym": sym}], ss.get_window(name, n, fftbins=not sym))
for beta in [0, 0.5, 5, 8.6, 14]:
    for n in [1, 4, 9, 17]:
        for sym in [False, True]:
            case("getWindow", ["kaiser", n, {"sym": sym, "beta": beta}], ss.get_window(("kaiser", beta), n, fftbins=not sym))
for a in [0, 10, 21, 21.5, 30, 50, 50.5, 60, 120]:
    case("kaiserBeta", [a], [ss.kaiser_beta(a)])
for a, w in [(8, 0.5), (21, 0.1), (40, 0.05), (60, 0.1), (80, 0.02), (120, 1.0)]:
    nt, b = ss.kaiserord(a, w)
    case("kaiserOrd", [a, w], [[nt], [b]])

FIR = [
    (21, 0.3, {}), (20, 0.3, {}), (1, 0.4, {}), (2, 0.4, {}), (31, 0.3, {"passZero": False}),
    (31, [0.2, 0.5], {"passZero": False}), (31, [0.2, 0.5], {}), (41, [0.1, 0.3, 0.5, 0.7], {}),
    (41, [0.1, 0.3, 0.5, 0.7], {"passZero": False}), (40, [0.2, 0.5], {"passZero": False}),
    (31, 100, {"passZero": False, "fs": 1000}), (64, 0.25, {"window": "hann"}), (65, 0.25, {"window": "blackman"}),
    (33, 0.25, {"window": "kaiser", "beta": 5}), (33, 0.25, {"window": "boxcar", "scale": False}),
    (33, [0.25, 0.6], {"window": "bartlett", "passZero": False, "scale": False}), (101, 2000, {"fs": 44100}),
]
for nt, cut, o in FIR:
    win = o.get("window", "hamming")
    if win == "kaiser":
        win = ("kaiser", o["beta"])
    h = ss.firwin(nt, cut, window=win, pass_zero=o.get("passZero", True), scale=o.get("scale", True), fs=o.get("fs", 2))
    case("firwin", [nt, cut, o], h)

LF = [([0.2, 0.3], [1, -0.5, 0.1], 37), ([1, 2, 1], [4, -2, 0.5], 50), ([0.5], [1], 9), ([1, -1], [1, -0.99], 64),
      ([0.1, 0.2, 0.3, 0.2, 0.1], [2], 30), ([1], [1, -1.8, 0.81], 40), ([0.3, 0.1], [1, 0.2, 0.3, -0.1, 0.05], 33)]
for b, a, n in LF:
    case("lfilter", [b, a, G(n, 8)], ss.lfilter(b, a, sig(n, 8)))

BUT = []
for N in range(1, 11):
    BUT += [(N, 0.2, "lowpass", 2), (N, 0.45, "highpass", 2), (N, [0.2, 0.4], "bandpass", 2), (N, [0.15, 0.6], "bandstop", 2)]
BUT += [(20, 0.3, "lowpass", 2), (16, [0.1, 0.12], "bandpass", 2), (4, 1000, "lowpass", 48000), (6, 50, "highpass", 1000),
        (3, [300, 3400], "bandpass", 8000), (2, [49, 51], "bandstop", 500), (8, 0.999, "lowpass", 2), (8, 0.001, "highpass", 2)]
for N, wn, bt, fs in BUT:
    sos = ss.butter(N, wn, bt, fs=fs, output="sos")
    o = {"btype": bt} if bt != "lowpass" else {}
    if fs != 2:
        o["fs"] = fs
    case("butter", [N, wn, o], sos)
    if N in (1, 2, 4, 7, 8):
        x = sig(40, N)
        case("sosfilt", [["butter", N, wn, o], G(40, N)], ss.sosfilt(sos, x))
        w, hz = ss.sosfreqz(sos, worN=8, fs=fs)
        case("sosfreqz", [["butter", N, wn, o], {"n": 8, "fs": fs}], [w, hz.real, hz.imag])
for b, a, n in LF[:4]:
    w, hz = ss.freqz(b, a, worN=n)
    case("freqz", [b, a, {"n": n}], [w, hz.real, hz.imag])
w, hz = ss.freqz([1, 2, 3, 2, 1], worN=10, fs=100)
case("freqz", [[1, 2, 3, 2, 1], None, {"n": 10, "fs": 100}], [w, hz.real, hz.imag])

for na, nb in [(1, 1), (5, 3), (3, 5), (37, 4), (4, 37), (60, 60), (300, 300), (1000, 49), (49, 1000), (2000, 700)]:
    a, b = sig(na, 9), sig(nb, 10)
    for mode in ["full", "same", "valid"]:
        L = len(ss.convolve(a, b, mode))
        st = 1 if L <= 80 else max(1, L // 40)
        case("convolve", [G(na, 9), G(nb, 10), mode], [ss.convolve(a, b, mode, method="direct")], st)
        case("correlate", [G(na, 9), G(nb, 10), mode], [ss.correlate(a, b, mode, method="direct")], st)

WEL = [
    (37, {"nperseg": 16, "fs": 10}), (1000, {}), (1000, {"nperseg": 100, "noverlap": 0}),
    (1000, {"nperseg": 64, "nfft": 100, "window": "hamming"}), (513, {"nperseg": 33, "noverlap": 10, "scaling": "spectrum"}),
    (300, {"nperseg": 50, "detrend": False, "window": "blackman"}), (5000, {"nperseg": 512, "fs": 44100}),
    (200, {"nperseg": 40, "window": "kaiser", "beta": 8}), (20, {"nperseg": 64}), (100000, {"nperseg": 1024, "fs": 1000}),
    (999, {"nperseg": 99, "noverlap": 98, "window": "boxcar"}),
]
for n, o in WEL:
    win = o.get("window", "hann")
    if win == "kaiser":
        win = ("kaiser", o["beta"])
    f, p = ss.welch(sig(n, 11), fs=o.get("fs", 1.0), window=win, nperseg=min(o.get("nperseg", 256), n),
                    noverlap=o.get("noverlap"), nfft=o.get("nfft"), detrend="constant" if o.get("detrend", True) else False,
                    scaling=o.get("scaling", "density"))
    st = 1 if len(f) <= 64 else max(1, len(f) // 48)
    case("welch", [G(n, 11), o], [f, p], st)

for n, m in [(1, None), (2, None), (7, None), (8, None), (37, None), (37, 40), (37, 30), (100, None), (127, 128)]:
    z = ss.hilbert(sig(n, 12), m)
    case("hilbert", [G(n, 12)] + ([m] if m else []), cx(z))

for n, f, fs in [(37, 3, 37), (64, 5, 64), (64, 5.5, 64), (100, 0.1, 1), (1000, 60, 8000), (1, 0.2, 1), (5, 0, 1)]:
    x = sig(n, 13)
    z = np.sum(x * np.exp(-2j * np.pi * f / fs * np.arange(n)))
    case("goertzel", [G(n, 13), f, fs], [[z.real], [z.imag]])


def rbj(t, f0, q, fs, g):
    w0 = 2 * math.pi * f0 / fs
    c, s = math.cos(w0), math.sin(w0)
    al = s / (2 * q)
    A = 10 ** (g / 40)
    sa = 2 * math.sqrt(A) * al
    a = [1 + al, -2 * c, 1 - al]
    b = {"lowpass": [(1 - c) / 2, 1 - c, (1 - c) / 2], "highpass": [(1 + c) / 2, -(1 + c), (1 + c) / 2],
         "bandpass": [al, 0, -al], "notch": [1, -2 * c, 1], "allpass": [1 - al, -2 * c, 1 + al]}.get(t)
    if t == "peaking":
        b = [1 + al * A, -2 * c, 1 - al * A]
        a = [1 + al / A, -2 * c, 1 - al / A]
    if t == "lowshelf":
        b = [A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa)]
        a = [(A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa]
    if t == "highshelf":
        b = [A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa)]
        a = [(A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa]
    return [v / a[0] for v in b] + [1.0, a[1] / a[0], a[2] / a[0]]


for t in ["lowpass", "highpass", "bandpass", "notch", "allpass", "peaking", "lowshelf", "highshelf"]:
    for f0, q, fs, g in [(1000, math.sqrt(0.5), 48000, 6), (0.1, 2.5, 2, -12), (3000, 0.3, 8000, 3)]:
        o = {"q": q, "fs": fs, "gainDb": g}
        case("biquad", [t, f0, o], rbj(t, f0, q, fs, g))

out = {"scipy": pinned("scipy", scipy.__version__), "numpy": pinned("numpy", np.__version__), "cases": CASES}
print("// generated by tests/ref/gen_signal_ref_vectors.py; do not edit")
print("export default " + json.dumps(out, separators=(",", ":"), allow_nan=False) + ";")
