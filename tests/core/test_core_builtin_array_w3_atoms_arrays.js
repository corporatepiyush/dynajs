// timeout: 120
// covers: c:src/builtins/builtin-array.c
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.
import { sections as __kitSections } from "../lib/kit.js";
import { cpuNow } from "../lib/cpuclock.js";
const __kitS = __kitSections("test_core_builtin_array_w3_atoms_arrays");

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eqArr(a, b, m) {
    let ja, jb;
    ja = JSON.stringify(Array.from(a, function (x) { return x === undefined ? "<undefined>" : x; }));
    jb = JSON.stringify(Array.from(b, function (x) { return x === undefined ? "<undefined>" : x; }));
    ok(ja === jb, m + " (got " + ja + ", want " + jb + ")");
}
function catches(fn) { try { fn(); return null; } catch (e) { return e; } }

// OPT-019: intersperse/scan semantics across dense, sparse, array-like, empty

__kitS.section("OPT-019 intersperse array-like", () => {
eqArr(Array.prototype.intersperse.call({ length: 3, 0: "a", 1: "b", 2: "c" }, 0), ["a", 0, "b", 0, "c"], "OPT-019 intersperse array-like");
});

__kitS.section("OPT-019 200k intersperse is >= 3x a JS push loop", () => {
(function () {
    let a = new Array(200000);
    for (let i = 0; i < a.length; i++) a[i] = i;
    function manual(src, sep) {
        const o = [];
        for (let i = 0; i < src.length; i++) { if (i) o.push(sep); o.push(src[i]); }
        return o;
    }
    let best = 0, out, ref;
    for (let attempt = 0; attempt < 3 && best < 3; attempt++) {
        let t = cpuNow();
        for (let r = 0; r < 10; r++) out = a.intersperse(0);
        const fast = cpuNow() - t;
        t = cpuNow();
        for (let r = 0; r < 10; r++) ref = manual(a, 0);
        const slow = cpuNow() - t;
        best = Math.max(best, slow / Math.max(fast, 0.01));
    }
    ok(out.length === 399999, "OPT-019 intersperse 200k length");
    ok(ref.length === out.length && ref[399998] === out[399998] && ref[1] === out[1],
       "OPT-019 intersperse agrees with the JS loop");
    ok(best >= 3, "OPT-019 200k intersperse is >= 3x a JS push loop (got " + best.toFixed(1) + "x)");
})();
});

__kitS.section("COMPAT-137 join after resize(1)", () => {
(function () {
    function resizable() {
        let ab = new ArrayBuffer(3, { maxByteLength: 8 });
        let ta = new Uint8Array(ab);
        ta.set([1, 2, 3]);
        return [ab, ta];
    }
    let [ab1, ta1] = resizable();
    ok(ta1.join({ toString: function () { ab1.resize(1); return ","; } }) === "1,,", "COMPAT-137 join after resize(1)");
    let [ab0, ta0] = resizable();
    ok(ta0.join({ toString: function () { ab0.resize(0); return ","; } }) === ",,", "COMPAT-137 join after resize(0)");
    let [ab4, ta4] = resizable();
    ok(ta4.join({ toString: function () { ab4.resize(4); return ","; } }) === "1,2,3", "COMPAT-137 join after resize(4)");
})();
});
__kitS.end();
if (!__kitS.listing) {

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
}
