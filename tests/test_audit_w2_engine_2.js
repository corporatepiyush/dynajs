// tests/test_audit_w2_engine_2.js -- value-level regressions for the engine
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
{
    Object.set({}, "__proto__.polluted_w2", "yes");
    eq(({}).polluted_w2, undefined, "Object.set __proto__ path does not pollute");
    Object.set({}, ["constructor", "prototype", "polluted_w2b"], 1);
    eq(({}).polluted_w2b, undefined, "Object.set constructor.prototype path does not pollute");
    const o = Object.set({}, "__proto__.x", 1);
    ok(Object.hasOwn(o, "__proto__"), "Object.set creates an own __proto__ key");
    eq(Object.getPrototypeOf(o), Object.prototype, "Object.set leaves the prototype alone");
    const t = { a: { b: 1 } };
    Object.set(t, "a.c.d", 5);
    eq(t.a.c.d, 5, "Object.set creates missing hops");
    eq(t.a.b, 1, "Object.set keeps siblings");
    const arr = { list: [1, 2, 3] };
    Object.set(arr, "list.1", 9);
    eq(arr.list[1], 9, "Object.set writes an own array index");
    let hit = 0;
    const acc = { set v(x) { hit = x; }, get v() { return 0; } };
    Object.set(acc, "v", 7);
    eq(hit, 7, "Object.set still runs an OWN setter on the leaf");
    const lensed = Lens.prop("__proto__").set({ evil: 1 }, {});
    eq(Object.getPrototypeOf(lensed), Object.prototype, "Lens key set defines, never re-parents");
}

// ---- E5-04 ---------------------------------------------------------------
{
    const deep = new Array(2000000).fill("a").join(".");
    let name = "";
    try { Object.assocPath(deep, 1, {}); } catch (e) { name = e.name; }
    ok(name === "RangeError" || name === "InternalError", "assocPath runaway path throws (got " + name + ")");
    eq(Object.assocPath(["a", "b"], 1, {}).a.b, 1, "assocPath still works");
    eq(Object.dissocPath(["a", "b"], { a: { b: 1, c: 2 } }).a.c, 2, "dissocPath still works");
}

// ---- E5-06 ---------------------------------------------------------------
{
    const base = { p1: 1, shared: "base" };
    const mid = Object.create(base); mid.p2 = 2; mid.shared = "mid";
    const top = Object.create(mid); top.p3 = 3; top.shared = "top";
    eq(Object.keysIn(top).join(","), "p3,shared,p2,p1", "keysIn order and cross-level dedupe");
    eq(Object.valuesIn(top).join(","), "3,top,2,1", "valuesIn reads the shadowing value");
    const big = {};
    for (let i = 0; i < 30000; i++) big["k" + i] = i;
    const kin = Object.keysIn(big);
    eq(kin.length, 30000, "keysIn keeps every own key");
    eq(kin[29999], "k29999", "keysIn keeps insertion order");
    const drop = [];
    for (let i = 0; i < 30000; i += 2) drop.push("k" + i);
    const om = Object.omit(drop, big);
    const omk = Object.keys(om);
    eq(omk.length, 15000, "omit drops exactly the listed keys");
    ok(omk.every((k) => k[0] === "k" && (+k.slice(1)) % 2 === 1), "omit keeps only the unlisted keys");
    eq(om.k29999, 29999, "omit keeps the values of the surviving keys");
    eq(Object.keys(Object.omit(["b", "zz", "b"], { a: 1, b: 2, c: 3 })).join(","), "a,c", "omit small list with duplicates and misses");
}

// ---- E5-07 ---------------------------------------------------------------
{
    let seen = 0, stored;
    Object.defineProperty(Array.prototype, "0", {
        configurable: true,
        get() { return stored; },
        set(v) { seen++; stored = v; },
    });
    let ran = 0, out;
    try {
        const f = (function () { ran++; return 42; }).once();
        f(); f(); out = f();
    } finally {
        delete Array.prototype[0];
    }
    eq(ran, 1, "once runs its body once under an Array.prototype index accessor");
    eq(out, 42, "once returns the cached value");
    eq(seen, 0, "helper state never reaches an inherited setter");
}

// ---- E2c-02 --------------------------------------------------------------
{
    const b = new BigInt64Array(1);
    const k = 5;
    throws(() => { b[k] = 1; }, "BigInt64Array OOB int-key store of a Number throws");
    throws(() => { b[k] = 1.5; }, "BigInt64Array OOB int-key store of a double throws");
    throws(() => { b["5"] = 1; }, "BigInt64Array OOB string-key store throws (control)");
    const u = new BigUint64Array(1);
    throws(() => { u[k] = 1; }, "BigUint64Array OOB int-key store of a Number throws");
    const f = new Float64Array(1);
    f[k] = 1;
    eq(f.length, 1, "Float64Array OOB store stays a silent no-op (control)");
}

// ---- E3-06 ---------------------------------------------------------------
{
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
        const t0 = performance.now();
        for (let r = 0; r < 3; r++) h.indexOf(nd);
        return performance.now() - t0;
    };
    cost(20000);
    const c1 = cost(40000), c4 = cost(160000);
    ok(c4 < Math.max(c1, 0.05) * 10, "wide indexOf scales linearly (40k " + c1.toFixed(2) + "ms, 160k " + c4.toFixed(2) + "ms)");
}

// ---- E3-07 ---------------------------------------------------------------
{
    const n = 1000000, v = Math.fround(0.1);
    const f = new Float32Array(n).fill(0.1);
    ok(Math.abs(f.sum() - n * v) < 1e-6, "Float32Array.sum accumulates in double (got " + f.sum() + ")");
    ok(Math.abs(f.mean() - v) < 1e-12, "Float32Array.mean accumulates in double (got " + f.mean() + ")");
    for (const len of [0, 1, 3, 4, 5, 7, 8, 9, 17]) {
        const t = new Float32Array(len);
        let want = 0;
        for (let i = 0; i < len; i++) { t[i] = i + 0.25; want += t[i]; }
        eq(t.sum(), want, "Float32Array.sum length " + len);
    }
}

// ---- E3-09 ---------------------------------------------------------------
{
    eq((1234567.891).format(2, " ", ","), "1 234 567,89", "Number.format NBSP thousands separator");
    eq((1234567).format(0, "٬"), "1٬234٬567", "Number.format non-Latin-1 separator");
    eq((1234567.5).format(1), "1,234,567.5", "Number.format defaults (control)");
    eq((-1234.5).format(1, ".", ","), "-1.234,5", "Number.format swapped ASCII separators (control)");
}

// ---- E3-10 ---------------------------------------------------------------
{
    eq("{constructor}|{toString}".format({}), "|", "String.format ignores inherited properties");
    eq("{a} {b}".format({ a: 1, b: "x" }), "1 x", "String.format own properties (control)");
    eq("{0}-{1}-{2}".format("a", "b"), "a-b-", "String.format positional (control)");
    eq("{99999999999999999999}".format("a"), "", "String.format huge index is undefined, not wrapped");
    const longKey = "k".repeat(200);
    eq(("{" + longKey + "}").format({ [longKey]: "L", ["k".repeat(63)]: "short" }), "L", "String.format long key is not truncated");
    eq("{é}".format({ "é": "E", "?": "wrong" }), "E", "String.format non-ASCII key is not mangled");
    eq("{{x}}".format({ x: 1 }), "{x}", "String.format escapes (control)");
}

// ---- E2b-01 --------------------------------------------------------------
{
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
    const time = (src) => { const t0 = performance.now(); const o = JSON.parse(src); return [performance.now() - t0, Object.keys(o).length]; };
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
}

if (failures) {
    print("test_audit_w2_engine_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_engine_2 failed");
}
print("test_audit_w2_engine_2: " + checks + " checks passed");
