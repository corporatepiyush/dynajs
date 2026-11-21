#!/usr/bin/env python3
"""numpy.fft / scipy.signal timings for bench/bench_signal.js on the SAME samples (the raw float64 file that script's 2nd
argument writes). 'butter order 8 design' times 1000 designs, as the JS row does; goertzel has no scipy routine, so its
row times the equivalent single-bin dot product with numpy.

    ./dynajs bench/bench_signal.js 5 signal.f64
    python3 bench/bench_signal_scipy.py signal.f64 3
"""
import sys
import time

import numpy as np
import scipy
from scipy import signal as ss

raw = np.fromfile(sys.argv[1], dtype="<f8")
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
sizes = [1 << 20, 1 << 20, 1000003, 4000000, 2000, 200000]
X, Y, P, W, K, C = np.split(raw, np.cumsum(sizes)[:-1])
SOS, FIR, X1 = ss.butter(8, 0.2, output="sos"), ss.firwin(101, 0.3), X[:1000000]
Z, Z1 = X + 1j * Y, X1 + 1j * Y[:1000000]
T1 = np.arange(1000000)


def best(f):
    b = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        f()
        b = min(b, time.perf_counter() - t)
    return b * 1000


rows = [
    ("fft complex 2^20", lambda: np.fft.fft(Z)),
    ("fft complex 1e6 (2^6 5^6)", lambda: np.fft.fft(Z1)),
    ("fft prime 1000003 (Bluestein)", lambda: np.fft.fft(P)),
    ("rfft 2^20", lambda: np.fft.rfft(X)),
    ("irfft 2^20", lambda: np.fft.irfft(np.fft.rfft(X), len(X))),
    ("welch 4e6 nperseg=1024", lambda: ss.welch(W, fs=1000, nperseg=1024)),
    ("sosfilt butter8 1e6", lambda: ss.sosfilt(SOS, X1)),
    ("lfilter fir101 1e6", lambda: ss.lfilter(FIR, [1], X1)),
    ("convolve 2e5 * 2000 (fft)", lambda: ss.convolve(C, K)),
    ("hilbert 2^20", lambda: ss.hilbert(X)),
    ("butter order 8 design", lambda: [ss.butter(8, 0.2, output="sos") for _ in range(1000)]),
    ("firwin 1001 taps", lambda: ss.firwin(1001, 0.3)),
    ("goertzel 1e6", lambda: np.dot(X1, np.exp(-2j * np.pi * 50 / 1000 * T1))),
]
print("scipy %s numpy %s: best of %d" % (scipy.__version__, np.__version__, runs))
for name, f in rows:
    print("%-32s%10.2f ms" % (name, best(f)))
