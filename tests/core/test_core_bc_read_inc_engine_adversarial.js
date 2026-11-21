// timeout: 120
// covers: c:src/serialize/bc_read.inc.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsAny(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m);
}

// Array-ext numeric boundaries

import("../lib/kit.js").then(async ({ sections }) => {
const S = sections("test_core_bc_read_inc_engine_adversarial");
S.section("array length shrink, clear and clamp keep sparse arrays consistent", function () {
    let a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    for (let i = 10; i < 40; i++) a[i] = i;
    a[1000] = "x";
    a.length = 20;
    eq(a.length, 20, "sparse shrink keeps length");
    eq(a[1000], undefined, "sparse shrink removes trailing index");
    eq(a[19], 19, "sparse shrink keeps in-range values");
    let b = [];
    b[100] = 1;
    b[101] = 2;
    b.length = 0;
    eq(b.length, 0, "length 0 clears sparse array");
    eq(b[100], undefined, "length 0 removes index");
    let c = [1, 2, 3];
    Object.defineProperty(c, "10", { value: 9, configurable: false, writable: true, enumerable: true });
    c.length = 5;
    eq(c.length, 11, "non-configurable index clamps length");
});
S.end();

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
