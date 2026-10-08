// timeout: 120
// Wave-3 engine regression suite: OPT-019 (array ext intersperse/scan
// fast-array allocation), COMPAT-137 (typed-array join after detach/resize),
// OPT-013/OPT-015 (atom interning paths), SEC-162 companion behavior rows.

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eqArr(a, b, m) {
    let ja = JSON.stringify(Array.from(a, function (x) { return x === undefined ? "<undefined>" : x; }));
    let jb = JSON.stringify(Array.from(b, function (x) { return x === undefined ? "<undefined>" : x; }));
    ok(ja === jb, m + " (got " + ja + ", want " + jb + ")");
}
function catches(fn) { try { fn(); return null; } catch (e) { return e; } }

// OPT-019: intersperse/scan semantics across dense, sparse, array-like, empty
eqArr([1, 2, 3].intersperse(0), [1, 0, 2, 0, 3], "OPT-019 intersperse dense");
eqArr([1].intersperse(0), [1], "OPT-019 intersperse one");
eqArr([].intersperse(0), [], "OPT-019 intersperse empty");
eqArr(Array.prototype.intersperse.call({ length: 3, 0: "a", 1: "b", 2: "c" }, 0), ["a", 0, "b", 0, "c"], "OPT-019 intersperse array-like");
eqArr([1, , 3].intersperse(0), [1, 0, undefined, 0, 3], "OPT-019 intersperse sparse");
eqArr([1, 2, 3].intersperse(), [1, undefined, 2, undefined, 3], "OPT-019 intersperse default separator");
eqArr([1, 2, 3].intersperse("--"), [1, "--", 2, "--", 3], "OPT-019 intersperse string separator");
eqArr([1, 2, 3].scan(function (a, b) { return a + b; }, 10), [10, 11, 13, 16], "OPT-019 scan");
eqArr([].scan(function (a, b) { return a + b; }, 5), [5], "OPT-019 scan empty");
eqArr([1, , 3].scan(function (a, b) { return a + (b === undefined ? 100 : b); }, 0), [0, 1, 101, 104], "OPT-019 scan sparse");
(function () {
    let cnt = 0;
    let e = catches(function () { [1, 2, 3].scan(function (a, b) { cnt++; if (cnt === 2) throw new Error("boom"); return a + b; }, 0); });
    ok(e !== null && e.message === "boom", "OPT-019 scan propagates a callback throw");
})();

// OPT-019 timing: 200k-element intersperse, 20 passes, stays well under the
// per-element-define budget (linear fast-array fill)
(function () {
    let a = new Array(200000);
    for (let i = 0; i < a.length; i++) a[i] = i;
    let t = Date.now();
    let out;
    for (let r = 0; r < 20; r++) out = a.intersperse(0);
    let dt = Date.now() - t;
    ok(out.length === 399999, "OPT-019 intersperse 200k length");
    ok(dt < 70, "OPT-019 20x200k intersperse in < 70ms (got " + dt + "ms)");
})();

// COMPAT-137: typed-array join with a separator whose ToString mutates the buffer
function joinCase(action) {
    let ta = new Uint8Array([1, 2, 3]);
    return ta.join({ toString: function () { action(ta); return ","; } });
}
ok(joinCase(function (ta) { ta.buffer.transfer(); }) === ",,", "COMPAT-137 join after transfer()");
ok(joinCase(function (ta) { ta.buffer.transfer(0); }) === ",,", "COMPAT-137 join after transfer(0)");
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
ok(new Uint8Array([1, 2, 3]).join() === "1,2,3", "COMPAT-137 join default separator");

// OPT-013/OPT-015: atom interning stays correct for ASCII, non-ASCII and
// num-string keys, including repeated re-interning from source text
(function () {
    let nm = "\u65e5\u672c\u8a9e\u30c6\u30b9\u30c8".repeat(60);
    let code = "var " + nm + " = 41; " + nm + " + 1";
    ok(eval(code) === 42, "OPT-013/015 long non-ASCII identifier evaluates");
    let o = {};
    o[nm] = 7;
    ok(o[eval("'" + nm + "'")] === 7, "OPT-013/015 long non-ASCII property key round-trips");
    let t = Date.now();
    for (let i = 0; i < 200; i++) eval("var " + nm + " = i;");
    let dt = Date.now() - t;
    ok(dt < 5000, "OPT-015 200 evals of a 1.8KB non-ASCII identifier complete (got " + dt + "ms)");
    ok(eval("({a1:1}).a1") === 1, "OPT-013 ASCII keys unaffected");
    ok(eval("({1.5:2})[1.5]") === 2, "OPT-013 numeric-like keys unaffected");
    ok(eval("({\"\\u00e9\":9})[\"\\u00e9\"]") === 9, "OPT-013 Latin-1 escaped key unaffected");
})();

setTimeout(function () {
    if (n !== 0) print("w3_atoms_arrays: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_atoms_arrays: " + failed + " failures");
    print("w3_atoms_arrays: OK (" + n + " checks)");
}, 20);
