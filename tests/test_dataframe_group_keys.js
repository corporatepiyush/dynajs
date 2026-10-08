// timeout: 600
// timeout: 600
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0, section = "(none)";
const failures = [];
function S(name) { section = name; }
function bad(what, detail) {
    fail++;
    const line = "FAIL [" + section + "] " + what + (detail ? ": " + detail : "");
    failures.push(line);
    console.log(line);
}
function ok(cond, what, detail) { if (cond) pass++; else bad(what, detail); }
function eq(a, b, what) {
    ok((a === b) || (Number.isNaN(a) && Number.isNaN(b)), what, a + " !== " + b);
}
function throwsLike(fn, substr, what) {
    let msg = null;
    try { fn(); } catch (e) { msg = String(e && e.message); }
    ok(msg !== null && msg.indexOf(substr) >= 0, what,
       msg === null ? "did not throw" : msg);
}

const DINT = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array,
              Uint32Array];

S("a negative group key is still refused, on every integer dtype");
{
    for (const T of DINT) {
        const n = 64;
        const k = new T(n);
        for (let i = 0; i < n; i++) k[i] = i % 7;
        const v = new Float64Array(n);
        for (let i = 0; i < n; i++) v[i] = i;
        const good = new DataFrame({ k, v });
        let nkeys = -1;
        try { nkeys = good.GROUP_BY_SUM("k", "v").keys.length; } catch (e) {  }
        eq(nkeys, 7, T.name + ": a clean column gives one key per distinct value");
        const signed = T === Int8Array || T === Int16Array || T === Int32Array;
        if (signed || T === Uint32Array) {
            for (const pos of [0, 1, 31, n - 1]) {
                const bad1 = new T(k);
                bad1[pos] = -1;
                const d = new DataFrame({ k: bad1, v });
                let msg = null;
                try { d.GROUP_BY_SUM("k", "v"); } catch (e) { msg = e.message; }
                if (signed)
                    ok(msg !== null && msg.indexOf("negative group key") >= 0,
                       T.name + ": a -1 at index " + pos + " is refused as negative",
                       String(msg));
                else
                    ok(msg !== null && msg.indexOf("too many groups") >= 0,
                       T.name + ": a wrapped -1 at index " + pos +
                       " is refused as too many groups", String(msg));
            }
        }
        if (T === Uint8Array || T === Uint16Array) {
            const d = new DataFrame({ k, v });
            let nk = -1;
            try { nk = d.GROUP_BY_SUM("k", "v").keys.length; } catch (e) {  }
            eq(nk, 7, T.name + ": a narrow unsigned column still groups");
        }
        if (signed) {
            const mostneg = T === Int8Array ? -128 : T === Int16Array ? -32768
                          : -2147483648;
            for (const e of [mostneg, -2, -1]) {
                const b2 = new T(n);
                for (let i = 0; i < n; i++) b2[i] = 3;
                b2[5] = e;
                const d = new DataFrame({ k: b2, v });
                let msg = null;
                try { d.GROUP_BY_SUM("k", "v"); } catch (err) { msg = err.message; }
                ok(msg !== null && msg.indexOf("negative group key") >= 0,
                   T.name + ": " + e + " is refused as a negative key", String(msg));
            }
        }
    }
    const d = new DataFrame({ k: new Int32Array([-1, 0, 1]),
                              v: new Float64Array([1, 2, 3]) });
    let name = null;
    try { d.GROUP_BY_SUM("k", "v"); } catch (e) { name = e.name; }
    eq(name, "RangeError", "a negative group key is still a RangeError");
}

S("the group count is still the maximum key, clamped by the column's own length");
{
    for (const [n, kmax] of [[10, 0], [10, 1], [10, 9], [10, 1000], [5, 99],
                             [1, 0], [1, 50], [64, 63], [64, (1 << 20) - 1]]) {
        const k = new Int32Array(n), v = new Float64Array(n);
        for (let i = 0; i < n; i++) { k[i] = kmax; v[i] = i + 1; }
        const r = new DataFrame({ k, v }).GROUP_BY_SUM("k", "v");
        eq(r.keys.length, kmax + 1,
           "n=" + n + " all keys " + kmax + " gives " + (kmax + 1) + " keys");
        eq(r.values.length, kmax + 1, "and as many value slots");
        let want = 0;
        for (let i = 0; i < n; i++) want += i + 1;
        eq(r.values[kmax], want, "the last group holds every row");
        for (let g = 0; g < kmax; g++)
            eq(r.values[g], 0, "every earlier group is empty (g=" + g + ")");
    }
    const bigk = new Int32Array(3);
    const bigv = new Float64Array(3);
    bigk[0] = 0; bigk[1] = 1 << 20; bigk[2] = 2;
    throwsLike(() => new DataFrame({ k: bigk, v: bigv }).GROUP_BY_SUM("k", "v"),
               "too many groups",
               "a maximum key of 2^20 is still too many groups");
    const e = new DataFrame({ k: new Int32Array(0), v: new Float64Array(0) });
    eq(e.GROUP_BY_COUNT("k").keys.length, 0, "an empty key column has no groups");
    const one = new DataFrame({ k: new Int32Array([0]), v: new Float64Array([42]) });
    const ro = one.GROUP_BY_SUM("k", "v");
    eq(ro.keys.length, 1, "a one-row frame of key 0 has one group");
    eq(ro.values[0], 42, "and it holds the row");
    const k2 = new Int32Array(8);
    const v2 = new Float64Array(3);
    for (let i = 0; i < 8; i++) k2[i] = 5;
    let ragged = null;
    try { new DataFrame({ k: k2, v: v2 }); } catch (e) { ragged = e.message; }
    ok(ragged !== null && ragged.indexOf("same length") >= 0,
       "a ragged frame is still refused by the constructor", String(ragged));
}

S("grouped results are unchanged, against a Map reference");
{
    for (const [n, G] of [[1000, 1], [1000, 7], [999, 1000], [5000, 128]]) {
        const k = new Int32Array(n), v = new Float64Array(n);
        let s = 12345;
        for (let i = 0; i < n; i++) {
            s = (Math.imul(s, 1103515245) + 12345) >>> 0;
            k[i] = s % G; v[i] = (s >>> 8) % 1000;
        }
        const df = new DataFrame({ k, v });
        const ref = new Map();
        for (let i = 0; i < n; i++) {
            const g = k[i];
            if (!ref.has(g)) ref.set(g, { sum: 0, n: 0, mn: Infinity, mx: -Infinity });
            const e = ref.get(g);
            e.sum += v[i]; e.n++; if (v[i] < e.mn) e.mn = v[i]; if (v[i] > e.mx) e.mx = v[i];
        }
        const sm = df.GROUP_BY_SUM("k", "v");
        const mn = df.GROUP_BY_MIN("k", "v");
        const mx = df.GROUP_BY_MAX("k", "v");
        const ct = df.GROUP_BY_COUNT("k");
        const me = df.GROUP_BY_MEAN("k", "v");
        eq(sm.keys.length, G, "n=" + n + " G=" + G + ": one key per group");
        for (let g = 0; g < G; g++) {
            const e = ref.get(g) || { sum: 0, n: 0, mn: NaN, mx: NaN };
            eq(sm.values[g], e.sum, "SUM g=" + g);
            eq(ct.values[g], e.n, "COUNT g=" + g);
            eq(mn.values[g], e.mn, "MIN g=" + g);
            eq(mx.values[g], e.mx, "MAX g=" + g);
            eq(me.values[g], e.n ? e.sum / e.n : NaN, "MEAN g=" + g);
        }
    }
    const n = 4000, G = 37;
    const k = new Int32Array(n), v = new Float64Array(n);
    for (let i = 0; i < n; i++) { k[i] = (i * 13) % G; v[i] = i; }
    const df = new DataFrame({ k, v });
    const shapes = {
        GROUP_BY_SUM: df.GROUP_BY_SUM("k", "v"),
        GROUP_BY_MEAN: df.GROUP_BY_MEAN("k", "v"),
        GROUP_BY_MIN: df.GROUP_BY_MIN("k", "v"),
        GROUP_BY_MAX: df.GROUP_BY_MAX("k", "v"),
        GROUP_BY_COUNT: df.GROUP_BY_COUNT("k"),
    };
    let agree = true;
    for (const [name, r] of Object.entries(shapes)) {
        if (r.keys.length !== G || r.values.length !== G) {
            bad(name + " agrees on the group count", r.keys.length + "/" + r.values.length);
            agree = false;
        }
    }
    ok(agree, "every grouped verb sizes the accumulator from the same count");
}

S("GROUP_BY_COUNT sizes the accumulator from ONE pass, not two");
{
    const N = 2000000;
    const mk = (G) => {
        const k = new Int32Array(N), v = new Float64Array(N);
        let s = 777;
        for (let i = 0; i < N; i++) {
            s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
            k[i] = s % G; v[i] = s % 1000;
        }
        return new DataFrame({ k, v });
    };
    const d10 = mk(10), d1000 = mk(1000);
    const time = (df, reps) => {
        let best = Infinity;
        for (let r = 0; r < reps; r++) {
            const t0 = performance.now();
            df.GROUP_BY_COUNT("k");
            const dt = performance.now() - t0;
            if (dt < best) best = dt;
        }
        return best;
    };
    time(d10, 3); time(d1000, 3);
    const t10 = time(d10, 9), t1000 = time(d1000, 9);
    console.log("  GROUP_BY_COUNT n=" + N + ": G=10 " + t10.toFixed(3) +
                " ms, G=1000 " + t1000.toFixed(3) + " ms, ratio " +
                (t1000 / t10).toFixed(2) + "x");
    ok(t1000 / t10 < 12, "the group count must not scale with the group count",
       (t1000 / t10).toFixed(2) + "x for a 100x wider accumulator");
    eq(d1000.GROUP_BY_COUNT("k").keys.length, 1000, "and both still report their groups");
    eq(d10.GROUP_BY_COUNT("k").keys.length, 10, "both, in the other order too");
}

console.log(`group keys: ${pass} passed, ${fail} failed`);
if (fail) {
    console.log(failures.join("\n"));
    throw new Error(fail + " dataframe group-key test(s) failed");
}
