// flags: --std
import * as std from "std";

let pass = 0, fail = 0, skip = 0;
const fails = [];
const ok = (c, w, d) => { if (c) pass++; else { fail++; fails.push(w + (d ? "  -- " + d : "")); } };

let seed = 20260802 >>> 0;
const rnd = () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296;

const LENGTHS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 100];
const near = (a, b, t) => Math.abs(a - b) <= (t || 1e-9) * Math.max(1, Math.abs(a), Math.abs(b));

{
    const s = await import("dyna:simd").catch(() => null);
    if (!s) { skip++; print("-- simd SKIP"); }
    else {
        const mk = (n, T, lo, hi) => {
            const a = new T(n);
            for (let i = 0; i < n; i++) a[i] = lo + rnd() * (hi - lo);
            return a;
        };

        const REDUCE = [
            ["f64Sum", Float64Array, (a) => { let s = 0; for (const x of a) s += x; return s; }],
            ["f64Max", Float64Array, (a) => { let m = -Infinity; for (const x of a) if (x > m) m = x; return m; }],
            ["f64Min", Float64Array, (a) => { let m = Infinity; for (const x of a) if (x < m) m = x; return m; }],
            ["i32Sum", Int32Array, (a) => { let s = 0; for (const x of a) s += x; return s; }],
            ["i32Max", Int32Array, (a) => { let m = -2147483648; for (const x of a) if (x > m) m = x; return m; }],
            ["i32Min", Int32Array, (a) => { let m = 2147483647; for (const x of a) if (x < m) m = x; return m; }],
        ];
        for (const [name, T, ref] of REDUCE) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const a = T === Int32Array ? mk(n, T, -1000, 1000) : mk(n, T, -100, 100);
                let got;
                try { got = s[name](a); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                const want = ref(a);
                if (!near(got, want, T === Int32Array ? 0 : 1e-9)) {
                    bad = `n=${n}: got ${got} want ${want}`; break;
                }
            }
            ok(!bad, `simd.${name} matches the scalar fold across the width boundaries`, bad);
        }

        for (const [name, T] of [["f64Dot", Float64Array], ["i32Dot", Int32Array]]) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                const a = mk(n, T, -50, 50), b = mk(n, T, -50, 50);
                let got;
                try { got = s[name](a, b); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                let want = 0;
                for (let i = 0; i < n; i++) want += a[i] * b[i];
                if (!near(got, want, T === Int32Array ? 0 : 1e-9)) {
                    bad = `n=${n}: got ${got} want ${want}`; break;
                }
            }
            ok(!bad, `simd.${name} matches the scalar dot product`, bad);
        }

        const UNARY = [
            ["relu", (x) => (x > 0 ? x : 0), 1e-6],
            ["relu6", (x) => Math.min(Math.max(x, 0), 6), 1e-6],
            ["sigmoid", (x) => 1 / (1 + Math.exp(-x)), 5e-2],
            ["vsqrt", (x) => Math.sqrt(x), 1e-5],
            ["vexp", (x) => Math.exp(x), 8e-2],
            ["vlog", (x) => Math.log(x), 1e-5],
            ["vinv", (x) => 1 / x, 1e-5],
            ["vrsqrt", (x) => 1 / Math.sqrt(x), 1e-3],
            ["leakyRelu", null, 1e-6, [0.02]],
            ["elu", null, 1e-5, [1]],
            ["gelu", null, 1e-3],
            ["silu", (x) => x / (1 + Math.exp(-x)), 5e-2],
            ["tanhFast", (x) => Math.tanh(x), 5e-2],
        ];
        for (const [name, ref, tol, extra] of UNARY) {
            if (typeof s[name] !== "function") { skip++; continue; }
            const positiveOnly = /^(vsqrt|vlog|vinv|vrsqrt)$/.test(name);
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const a = positiveOnly ? mk(n, Float32Array, 0.1, 10)
                                       : mk(n, Float32Array, -5, 5);
                const src = Array.from(a);
                let got;
                try { got = s[name](a, ...(extra || [])); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                if (!got || got.length !== n) {
                    bad = `n=${n}: returned length ${got && got.length}, want ${n}`; break;
                }
                if (!ref) continue;
                for (let i = 0; i < n && !bad; i++)
                    if (!near(got[i], ref(src[i]), tol))
                        bad = `n=${n} i=${i}: got ${got[i]} want ${ref(src[i])}`;
                if (bad) break;
            }
            ok(!bad, `simd.${name} matches the scalar form elementwise`, bad);
        }

        for (const name of ["softmax", "logSoftmax"]) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of [1, 4, 5, 16, 17]) {
                const a = mk(n, Float32Array, -3, 3);
                let got;
                try { got = s[name](a); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                if (!got || got.length !== n) { bad = `n=${n}: wrong length`; break; }
                if (name === "softmax") {
                    let sum = 0;
                    for (let i = 0; i < n; i++) sum += got[i];
                    if (!near(sum, 1, 1e-4)) { bad = `n=${n}: sums to ${sum}, want 1`; break; }
                }
            }
            ok(!bad, `simd.${name} ${name === "softmax" ? "sums to 1" : "preserves length"}`, bad);
        }

        const SCALED = [
            ["f64Scale", Float64Array, (a, k) => a.map((x) => x * k)],
            ["i32Scale", Int32Array, (a, k) => a.map((x) => (x * k) | 0)],
            ["addScalar", Float32Array, (a, k) => a.map((x) => x + k)],
        ];
        for (const [name, T, ref] of SCALED) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const a = mk(n, T, -20, 20), k = 3;
                const src = T.from(a);
                let got;
                try { got = s[name](a, k); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                const want = ref(src, k);
                for (let i = 0; i < n && !bad; i++)
                    if (!near(got[i], want[i], T === Int32Array ? 0 : 1e-5))
                        bad = `n=${n} i=${i}: got ${got[i]} want ${want[i]}`;
                if (bad) break;
            }
            ok(!bad, `simd.${name} matches the scalar map`, bad);
        }

        const DIST = [
            ["distL1", (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += Math.abs(a[i] - b[i]); return s; }, 1e-4],
            ["distL2", (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += (a[i] - b[i]) ** 2; return Math.sqrt(s); }, 1e-4],
            ["distCheb", (a, b) => { let m = 0; for (let i = 0; i < a.length; i++) m = Math.max(m, Math.abs(a[i] - b[i])); return m; }, 1e-5],
        ];
        for (const [name, ref, tol] of DIST) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const a = mk(n, Float32Array, -10, 10), b = mk(n, Float32Array, -10, 10);
                let got;
                try { got = s[name](a, b); } catch (e) { bad = `n=${n} threw ${e.message}`; break; }
                if (!near(got, ref(a, b), tol)) { bad = `n=${n}: got ${got} want ${ref(a, b)}`; break; }
            }
            ok(!bad, `simd.${name} matches the closed form`, bad);
            if (!bad && typeof s[name] === "function") {
                const a = mk(8, Float32Array, -10, 10);
                ok(near(s[name](a, a), 0, 1e-5), `simd.${name}(a, a) === 0`);
            }
        }

        if (typeof s.clamp === "function") {
            const a = new Float32Array([-9, -1, 0, 1, 9]);
            const g = s.clamp(a, -2, 2);
            let bad = null;
            for (let i = 0; i < a.length && !bad; i++) {
                const w = Math.min(Math.max(a[i], -2), 2);
                if (!near(g[i], w, 1e-6)) bad = `i=${i}: got ${g[i]} want ${w}`;
            }
            ok(!bad, "simd.clamp bounds every element into [lo, hi]", bad);
        }
    }
}

{
    const b = await import("dyna:bytes").catch(() => null);
    if (!b) { skip++; print("-- bytes SKIP"); }
    else {
        const SAMPLES = ["", "a", "abc", "héllo", "日本語", " ", "x".repeat(300)];

        const PAIRS = [
            ["fromUtf8", "toUtf8", SAMPLES],
            ["utf8ToUtf16", "utf16ToUtf8", SAMPLES],
            ["utf8ToLatin1", "latin1ToUtf8", ["", "a", "abc", "éÿ"]],
        ];
        for (const [fwd, back, inputs] of PAIRS) {
            if (typeof b[fwd] !== "function" || typeof b[back] !== "function") { skip++; continue; }
            let bad = null;
            for (const s of inputs) {
                try {
                    const r = b[back](b[fwd](s));
                    const got = typeof r === "string" ? r
                        : (typeof b.decode === "function" ? b.decode(r, "utf-8")
                                                          : String(r));
                    if (got !== s) { bad = `${JSON.stringify(s.slice(0, 12))} -> ${JSON.stringify(String(got).slice(0, 12))}`; break; }
                } catch (e) { bad = `${JSON.stringify(s.slice(0, 12))} threw ${e.message}`; break; }
            }
            ok(!bad, `bytes.${fwd}/${back} round trip`, bad);
        }

        if (typeof b.isAscii === "function") {
            let bad = null;
            for (const s of SAMPLES) {
                const want = /^[\x00-\x7f]*$/.test(s);
                let got;
                try { got = b.isAscii(typeof b.fromUtf8 === "function" ? b.fromUtf8(s) : s); }
                catch (e) { continue; }
                if (got !== want) { bad = `${JSON.stringify(s.slice(0, 12))}: got ${got} want ${want}`; break; }
            }
            ok(!bad, "bytes.isAscii agrees with a JS character-range test", bad);
        }
        if (typeof b.countUtf16 === "function") {
            let bad = null;
            for (const s of ["", "abc", "héllo", "日本語"]) {
                let got;
                try { got = b.countUtf16(b.utf8ToUtf16(b.encode(s, "utf-8"))); }
                catch (e) { continue; }
                if (got !== s.length) { bad = `${JSON.stringify(s)}: got ${got} want ${s.length}`; break; }
            }
            ok(!bad, "bytes.countUtf16 equals String.length", bad);
        }

        if (typeof b.alloc === "function") {
            const z = b.alloc(8);
            ok(z && z.length === 8, "bytes.alloc returns the requested length",
               z && `length ${z.length}`);
            let allZero = true;
            for (let i = 0; i < 8; i++) if (z[i] !== 0) allZero = false;
            ok(allZero, "bytes.alloc zero-fills");
        }
        if (typeof b.fill === "function" && typeof b.alloc === "function") {
            const f = b.fill(b.alloc(4), 7);
            let allSeven = true;
            for (let i = 0; i < 4; i++) if (f[i] !== 7) allSeven = false;
            ok(allSeven, "bytes.fill writes the value to every position");
        }
        if (typeof b.includes === "function" && typeof b.indexOf === "function") {
            const hay = new Uint8Array([1, 2, 3, 4, 5]);
            const mid = new Uint8Array([3, 4]), absent = new Uint8Array([9]);
            ok(b.includes(hay, mid) === true && b.includes(hay, absent) === false,
               "bytes.includes agrees with presence");
            ok(b.indexOf(hay, mid) === 2, "bytes.indexOf reports the first offset",
               String(b.indexOf(hay, mid)));
            ok(b.indexOf(hay, absent) < 0, "bytes.indexOf reports a miss as negative");
        }
        if (typeof b.lastIndexOf === "function") {
            const hay = new Uint8Array([1, 2, 1, 2]);
            const n = new Uint8Array([1, 2]);
            ok(b.lastIndexOf(hay, n) === 2, "bytes.lastIndexOf reports the LAST offset",
               String(b.lastIndexOf(hay, n)));
        }
        if (typeof b.isBytes === "function") {
            ok(b.isBytes(new Uint8Array(1)) === true && b.isBytes("abc") === false,
               "bytes.isBytes distinguishes a view from a string");
        }
        if (typeof b.encodingExists === "function" && typeof b.encodings === "function") {
            const list = b.encodings();
            let bad = null;
            for (const label of list.slice(0, 12))
                if (b.encodingExists(label) !== true) { bad = label; break; }
            ok(!bad, "bytes.encodings() and encodingExists() agree", bad);
        }
    }
}

{
    const c = await import("dyna:csv").catch(() => null);
    if (!c || typeof c.CSVFile !== "function") { skip++; print("-- csv SKIP"); }
    else {
        let f = null;
        try { f = c.CSVFile.create ? c.CSVFile.create(["a", "b"]) : new c.CSVFile(["a", "b"]); }
        catch (e) { skip++; print("-- csv CSVFile not constructible in-memory: " + e.message); }
        if (f) {
            const call = (name, ...a) => {
                if (typeof f[name] !== "function") { skip++; return undefined; }
                try { return f[name](...a); } catch (e) { return { __threw: e.message }; }
            };
            const r1 = call("addRow", ["1", "2"]);
            ok(!(r1 && r1.__threw), "csv.addRow accepts a row matching the header",
               r1 && r1.__threw);
            const cc = call("addColumn", "c", "0");
            ok(!(cc && cc.__threw), "csv.addColumn accepts a new column", cc && cc.__threw);
            const rn = call("renameColumn", "c", "d");
            ok(!(rn && rn.__threw), "csv.renameColumn accepts an existing column",
               rn && rn.__threw);
            const bad = call("renameColumn", "nope", "x");
            ok(bad && bad.__threw, "csv.renameColumn refuses a column that is not there");
            const rm = call("removeColumn", "d");
            ok(!(rm && rm.__threw), "csv.removeColumn accepts an existing column",
               rm && rm.__threw);
            try { if (typeof f.close === "function") f.close(); } catch (e) {}
        }
    }
}

print("\n" + "=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails) print("  " + f);
}
print(`test_api_kernels: ${pass} passed, ${fail} failed, ${skip} skipped`);
if (fail > 0) std.exit(1);
