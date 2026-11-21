// covers: c:src/builtins/builtin-array.c
// tests/core/test_array_join_sparse.js -- B16: Array.prototype.join walked
// every index of a sparse array, one spec Get per hole, so [].join("") over a
// sparse array of length 2^32-1 ran for minutes and length 1e8 took ~3.7 s.
// After a run of holes join now asks the array for its next own index and
// skips the gap in one step, but only while every object on the prototype
// chain is a plain object or array with no indexed properties (so each skipped
// Get is provably undefined and side-effect free). Observable behaviour must
// match the spec walk exactly: indexed prototype properties, getters, element
// toString side effects that add or delete elements, proxies on the chain.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_array_join_sparse");
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

function refJoin(a, sep) {
    const len = a.length >>> 0;
    sep = sep === undefined ? "," : String(sep);
    let r = "";
    for (let i = 0; i < len; i++) {
        if (i > 0) r += sep;
        const e = a[i];
        if (e !== undefined && e !== null) r += String(e);
    }
    return r;
}

let seed = 12345;
function rnd(k) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % k; }
for (let trial = 0; trial < 300; trial++) {
    const len = rnd(400);
    const a = [];
    a.length = len;
    const fill = rnd(4);
    for (let k = 0; k < fill * 8; k++) a[rnd(len + 1)] = rnd(3) ? rnd(100) : (rnd(2) ? null : "s" + k);
    if (rnd(5) === 0) a.length = len;
    const dense = rnd(6) === 0;
    const b = dense ? Array.from({ length: rnd(60) }, (_, i) => i) : a;
    if (dense) b.length += rnd(200);
    for (const sep of ["", ",", "--", undefined]) {
        if (b.join(sep) !== refJoin(b, sep)) { check(false, "random sparse array " + trial + " sep " + JSON.stringify(sep)); break; }
    }
}
check(true, "300 random sparse arrays match the spec walk");

__kitS.section("an indexed Array.prototype property inside a gap is read", () => {
    const a = []; a.length = 1000; a[0] = "a"; a[999] = "z";
    Array.prototype[500] = "P";
    try {
        check(a.join("") === "aPz", "an indexed Array.prototype property inside a gap is read");
        check(a.join("-") === refJoin(a, "-"), "same with a separator");
    } finally { delete Array.prototype[500]; }
    Object.prototype[700] = "O";
    try { check(a.join("") === "aOz", "an indexed Object.prototype property inside a gap is read"); }
    finally { delete Object.prototype[700]; }
    const b = [1, 2, 3]; b.length = 1000;
    Array.prototype[900] = "F";
    try { check(b.join("") === "123F", "past the end of a dense array the prototype is still consulted"); }
    finally { delete Array.prototype[900]; }
});
__kitS.section("an element added by a getter mid-join is seen", () => {
    const a = []; a.length = 2000; a[0] = 0;
    Object.defineProperty(a, 10, { get() { a[1500] = "late"; return "g"; }, configurable: true });
    check(a.join("") === "0glate", "an element added by a getter mid-join is seen");
    const b = []; b.length = 2000;
    b[0] = { toString() { b[1800] = "x"; Object.prototype[1900] = "y"; return "o"; } };
    try { check(b.join("") === "oxy", "elements and prototype indices added by toString mid-join are seen"); }
    finally { delete Object.prototype[1900]; }
    const c = []; c.length = 3000; c[1] = "1"; c[2999] = "e";
    c[0] = { toString() { delete c[2999]; return "0"; } };
    check(c.join("") === "01", "an element deleted mid-join is not read");
    const d = []; d.length = 500; d[0] = 1;
    const log = [];
    Object.setPrototypeOf(d, new Proxy(Array.prototype, { get(t, k, r) { if (typeof k === "string" && /^\d+$/.test(k)) log.push(k); return Reflect.get(t, k, r); } }));
    d.join("");
    check(log.length === 499 && log[0] === "1" && log[498] === "499", "a proxy on the prototype chain sees every Get (" + log.length + ")");
    const p = new Proxy([], { get(t, k) { if (k === "length") return 300; if (k === "150") return "m"; return undefined; } });
    check(Array.prototype.join.call(p, "") === "m", "join on a proxy is unchanged");
    const o = { length: 400, 3: "a", 399: "b" };
    check(Array.prototype.join.call(o, "") === "ab", "join on an array-like object is unchanged");
    const u = Array.from({ length: 40 }, () => undefined).concat([null, null, 7, undefined, "e"]);
    check(u.join("") === "7e" && u.join("|") === refJoin(u, "|"), "a dense array of explicit undefined and null keeps its later elements");
    const sl = []; sl.length = 100; for (let k = 0; k < 60; k++) sl[k] = null; sl[80] = "q"; sl.x = 1;
    check(sl.join("") === "q", "a run of explicit nulls before a real element");
    const protoArr = Array.from({ length: 100 }, (_, i) => (i < 50 ? undefined : i % 10));
    const pa = []; pa.length = 100; pa[0] = "s";
    Object.setPrototypeOf(pa, protoArr);
    check(Array.prototype.join.call(pa, "") === "s" + protoArr.slice(1).join(""), "a dense array on the prototype chain fills the gaps");
    const big = []; big.length = 2 ** 31 + 10; big[2 ** 31 + 3] = "hi"; big[0] = "lo";
    check(big.join("") === "lohi", "an index above 2^31 is found by the skip");
});
__kitS.section("length 1e8 sparse join(\"\") skips the holes", () => {
    const a = []; a.length = 1e8; a[5] = 1; a[9e7] = 2;
    const t0 = cpuNow();
    const s = a.join("");
    const dt = cpuNow() - t0;
    check(s === "12" && dt < 1000, "length 1e8 sparse join(\"\") skips the holes (" + dt + " ms CPU)");
    if (dt < 1000) {
        const b = []; b.length = 2 ** 32 - 1; b[7] = "x"; b[2 ** 32 - 2] = "y";
        const t1 = cpuNow();
        const s2 = b.join("");
        const d2 = cpuNow() - t1;
        check(s2 === "xy" && d2 < 1000, "length 2^32-1 sparse join(\"\") (" + d2 + " ms CPU)");
        const c = [1, 2]; c.length = 2 ** 32 - 1;
        const t2 = cpuNow();
        check(c.join("") === "12" && cpuNow() - t2 < 1000, "a dense head then 2^32-1 holes");
        let thrown = null;
        try { b.join(","); } catch (e) { thrown = e; }
        check(thrown instanceof RangeError, "a non-empty separator over 2^32-1 still throws RangeError");
    }
});
__kitS.end();

print("test_array_join_sparse: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_array_join_sparse: " + failures + " failures");
