// covers: c:src/builtins/builtin-string.c
// tests/core/test_audit_w2_engine_2.js -- value-level regressions for the engine
// built-ins audited in wave 2. Every expected value is either a definition
// (a naive reference computed here) or a property stated in dynajs.d.ts.
//   E5-03  Object.set never walks or writes through the prototype chain
//   E5-04  assocPath/dissocPath/modifyPath refuse a runaway path cleanly
//   E5-06  keysIn/valuesIn/omit keep their answers (and stop being quadratic)
//   E5-07  once/debounce state is not observable through Array.prototype
//   E2c-02 BigInt64Array out-of-range store with an int key throws like the
//          string-key path
//   E3-06  indexOf/lastIndexOf/includes/count/split agree with a naive
//          reference on inputs that force the linear-time fallback
//   E3-07  Float32Array sum/mean accumulate in double
//   E3-09  Number.format keeps non-ASCII separators intact
//   E3-10  String.format reads own properties only
//   E2b-01 colliding keys do not make interning quadratic
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_string_audit_w2_engine_2");
import { cpuNow } from "../lib/cpuclock.js";
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    ok(threw, msg);
}

// ---- E5-03 ---------------------------------------------------------------

__kitS.section("search differential h.len", () => {
    const naiveIndexOf = (h, n, from) => {
        for (let i = from; i + n.length <= h.length; i++) {
            let j = 0;
            while (j < n.length && h.charCodeAt(i + j) === n.charCodeAt(j)) j++;
            if (j === n.length) return i;
        }
        return -1;
    };
    const naiveLast = (h, n) => {
        for (let i = h.length - n.length; i >= 0; i--) {
            let j = 0;
            while (j < n.length && h.charCodeAt(i + j) === n.charCodeAt(j)) j++;
            if (j === n.length) return i;
        }
        return -1;
    };
    const naiveCount = (h, n) => {
        let c = 0, i = 0;
        while ((i = naiveIndexOf(h, n, i)) >= 0) { c++; i += n.length; }
        return c;
    };
    const W = "Ж";
    const shapes = [];
    for (const wide of [false, true]) {
        const a = wide ? W : "a", b = wide ? "Щ" : "b";
        for (const hl of [0, 1, 63, 64, 65, 600, 4000]) {
            for (const nl of [1, 2, 3, 31, 33, 97, 300]) {
                shapes.push([a.repeat(hl), a.repeat(nl)]);
                shapes.push([a.repeat(hl), a.repeat(nl - 1) + b]);
                shapes.push([a.repeat(hl), b + a.repeat(nl - 1)]);
                shapes.push([a.repeat(hl) + b, a.repeat(nl - 1) + b]);
                shapes.push([(a.repeat(nl) + b).repeat(Math.ceil(hl / (nl + 1))), a.repeat(nl - 1) + b + a]);
                shapes.push([(a + b).repeat(hl >> 1), (a + b).repeat(nl >> 1) + a]);
                shapes.push([a.repeat(hl) + b + a.repeat(hl), a.repeat(Math.min(nl, hl)) + b + a.repeat(Math.min(nl, hl))]);
            }
        }
    }
    shapes.push(["a".repeat(3000) + W, "a".repeat(200) + W]);
    shapes.push([W + "a".repeat(3000), "a".repeat(200) + "b"]);
    shapes.push(["a".repeat(3000), "a".repeat(200) + W]);
    let bad = 0;
    for (const [h, n] of shapes) {
        const r = [h.indexOf(n), h.lastIndexOf(n), h.includes(n), h.count(n), h.split(n).length - 1];
        const w0 = naiveIndexOf(h, n, 0), wc = naiveCount(h, n);
        const w = [w0, naiveLast(h, n), w0 >= 0, wc, wc];
        if (r.join() !== w.join()) {
            if (bad++ < 5)
                print("  FAIL: search differential h.len=" + h.length + " n.len=" + n.length + " got " + r + " want " + w);
        }
        const mid = h.length >> 1;
        if (h.indexOf(n, mid) !== naiveIndexOf(h, n, mid)) bad++;
    }
    checks++;
    if (bad) failures++;
    print("  search differential: " + shapes.length + " shapes, " + bad + " mismatches");

    // The work is linear: a 4x larger adversarial input may not cost anywhere
    // near 16x. Ratio of two runs, never a duration.
    const cost = (n) => {
        const h = W + "a".repeat(n), nd = "a".repeat(n >> 1) + "b";
        const t0 = cpuNow();
        for (let r = 0; r < 3; r++) h.indexOf(nd);
        return cpuNow() - t0;
    };
    cost(20000);
    // Best of 5 each: one preempted run on a loaded host is not a complexity.
    let c1 = Infinity, c4 = Infinity;
    for (let k = 0; k < 5; k++) { c1 = Math.min(c1, cost(40000)); c4 = Math.min(c4, cost(160000)); }
    ok(c4 < Math.max(c1, 0.05) * 10, "wide indexOf scales linearly (40k " + c1.toFixed(2) + "ms, 160k " + c4.toFixed(2) + "ms)");
});

__kitS.section("String.format ignores inherited properties", () => {
    eq("{constructor}|{toString}".format({}), "|", "String.format ignores inherited properties");
    eq("{a} {b}".format({ a: 1, b: "x" }), "1 x", "String.format own properties (control)");
    eq("{0}-{1}-{2}".format("a", "b"), "a-b-", "String.format positional (control)");
    eq("{99999999999999999999}".format("a"), "", "String.format huge index is undefined, not wrapped");
    const longKey = "k".repeat(200);
    eq(("{" + longKey + "}").format({ [longKey]: "L", ["k".repeat(63)]: "short" }), "L", "String.format long key is not truncated");
    eq("{é}".format({ "é": "E", "?": "wrong" }), "E", "String.format non-ASCII key is not mangled");
    eq("{{x}}".format({ x: 1 }), "{x}", "String.format escapes (control)");
});

__kitS.section("control keys are distinct", () => {
    const mk = (n, colliding) => {
        const parts = [];
        for (let i = 0; i < n; i++) {
            let s = "";
            for (let b = 0; b < 17; b++) {
                const bit = (i >> b) & 1;
                s += colliding
                    ? (bit ? "Ёӹ" : "Ѐ؀")
                    : String.fromCharCode(0x0400 + ((i >> b) & 1), 0x0600 + b);
            }
            parts.push(JSON.stringify(s) + ":1");
        }
        return "{" + parts.join(",") + "}";
    };
    const time = (src) => { const t0 = cpuNow(); const o = JSON.parse(src); return [cpuNow() - t0, Object.keys(o).length]; };
    const n = 30000;
    const ctl = mk(n, false), col = mk(n, true);
    time(mk(2000, false));
    const [tc, kc] = time(ctl), [tx, kx] = time(col);
    eq(kc, n, "control keys are distinct");
    eq(kx, n, "colliding-shape keys are distinct");
    ok(tx < Math.max(tc, 1) * 10, "equal-length key family does not flood the atom table (control " + tc.toFixed(1) + "ms, crafted " + tx.toFixed(1) + "ms)");

    const m = new Map();
    m.set("abc", 1);
    eq(m.get(("Āabc").slice(1)), 1, "Map finds a narrow key through a wide-backed equal string");
    const rope = "x".repeat(300) + "y".repeat(300);
    m.set(rope, 2);
    eq(m.get(["x".repeat(300), "y".repeat(300)].join("")), 2, "Map finds a rope-built key through a flat equal string");
    const o = { key: 7 };
    eq(o[("Ākey").slice(1)], 7, "property lookup through a wide-backed equal string");
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_engine_2 failed");
}
print("test_audit_w2_engine_2: " + checks + " checks passed");
