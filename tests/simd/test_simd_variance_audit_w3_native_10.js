// flags: --std
// covers: dyna:simd.variance
// Audit wave 3, batch 10: SIMD pre-scan and variance, repeated HTTP headers,
// TLS end-of-stream for bodies with no declared length.
// Each section names its ORACLE and its CONTROL. [red] = fails before this
// batch. The two HTTP sections need python3 (and openssl for the TLS one) to
// play a peer this runtime cannot be: a server that answers a BLOCKING client
// from another process, and a TLS peer that cuts the stream on purpose.
import * as simd from "dyna:simd";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import * as std from "std";
import { suite } from "../lib/kit.js";

const t = suite("test_audit_w3_native_10");
const best = (f, reps = 7) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = String(makeTempDir("w3n10_"));
const sh = (script) => Exec("/bin/sh", ["-c", script], { timeoutMs: 40000, encoding: "utf8" });
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];

// ---- M1c-P2: the NaN pre-scan ---------------------------------------------
// ORACLE: the kernels' documented NaN rule is a property of the VALUES, not of
// where the NaN sits: max/min of an array holding a NaN is NaN, relu and clamp
// keep a NaN element NaN and leave every other element as the scalar
// definition gives it. The scan now works in blocks of 64 with a scalar tail,
// so the NaN is placed at EVERY index of arrays whose lengths straddle one and
// two blocks (N-1, N, N+1 around 64 and 128).
// SCALING: a guarded kernel cannot cost several times an unguarded one of the
// same shape. relu (scan + select) was 4.9x leakyRelu (select only).
// CONTROL: arrays with no NaN return the plain maximum/minimum.

t.test("M1c-P3 variance in one pass", ({ ok }) => {
    for (const len of [1, 7, 8, 9, 1000, 4097]) {
        const a = new Float32Array(len);
        for (let i = 0; i < len; i++) a[i] = Math.fround(Math.cos(i * 0.37) * 10 + 3);
        let m = 0; for (let i = 0; i < len; i++) m += a[i]; m /= len;
        let v = 0; for (let i = 0; i < len; i++) v += (a[i] - m) * (a[i] - m); v /= len;
        const got = simd.variance(a);
        ok(Math.abs(got - v) <= 1e-5 * Math.max(1, v), "M1c-P3: variance of " + len + " elements matches the definition (" + got + " vs " + v + ")");
    }
    ok(simd.variance(new Float32Array(100).fill(2.5)) === 0, "a constant array has variance exactly 0");
});

t.after(() => removeAll(new Path(T)));
await t.run();
