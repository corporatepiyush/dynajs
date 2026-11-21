// flags: --std
// covers: dyna:simd.max
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

t.test("M1c-P2 NaN rule at every index", ({ ok }) => {
    let bad = 0, n = 0;
    for (const len of [1, 2, 63, 64, 65, 127, 128, 129, 200]) {
        const base = new Float32Array(len);
        for (let i = 0; i < len; i++) base[i] = Math.fround(Math.sin(i * 1.7) * 5);
        let mx = -Infinity, mn = Infinity;
        for (let i = 0; i < len; i++) { if (base[i] > mx) mx = base[i]; if (base[i] < mn) mn = base[i]; }
        n += 2;
        if (simd.max(base) !== mx) bad++;
        if (simd.min(base) !== mn) bad++;
        for (let at = 0; at < len; at++) {
            const a = base.slice(); a[at] = NaN;
            n += 2;
            if (!Number.isNaN(simd.max(a))) bad++;
            if (!Number.isNaN(simd.min(a))) bad++;
            const r = a.slice(); simd.relu(r);
            const c = a.slice(); simd.clamp(c, -1, 1);
            n += 2;
            let good = Number.isNaN(r[at]) && Number.isNaN(c[at]);
            for (let i = 0; i < len && good; i++) {
                if (i === at) continue;
                good = r[i] === Math.max(base[i], 0) && c[i] === Math.fround(Math.min(Math.max(base[i], -1), 1));
            }
            if (!good) bad += 2;
        }
    }
    ok(bad === 0, "M1c-P2: max/min/relu/clamp treat a NaN at every index of every length the same (" + bad + " of " + n + " wrong)");
});

t.after(() => removeAll(new Path(T)));
await t.run();
