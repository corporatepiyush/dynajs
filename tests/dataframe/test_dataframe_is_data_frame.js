// covers: dyna:dataframe.isDataFrame
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_is_data_frame");
import { DataFrame, isDataFrame } from "dyna:dataframe";
import { cpuNow } from "../lib/cpuclock.js";

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
function same(a, b, what) { ok(Object.is(a, b), what, a + " is not " + b); }
function ulpNear(a, b, ulps, what) {
    const d = Math.abs(a - b), scale = Math.max(Math.abs(a), Math.abs(b));
    const got = scale === 0 ? 0 : d / (scale * Number.EPSILON);
    ok(got <= ulps, what, a + " vs " + b + " (" + got.toFixed(2) + " ulp > " + ulps + ")");
}
function near(a, b, what, tol) {
    tol = tol || 1e-12;
    if (Number.isNaN(a) && Number.isNaN(b)) { pass++; return; }
    ok(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b)), what, // not perf: a float tolerance, not time
       a + " vs " + b + " (rel " + (Math.abs(a - b) / Math.max(1e-300, Math.abs(b))) + ")");
}
function throws(fn, what) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    ok(threw !== null, what + " did not throw");
    return threw;
}
function throwsLike(fn, needle, what) {
    const e = throws(fn, what);
    if (e) ok(String(e.message).indexOf(needle) >= 0, what + " message names '" + needle + "'",
              e.message);
}
function elemEq(got, want, what, tol) {
    let n = -1, worst = 0;
    if (got.length !== want.length) { bad(what, "length " + got.length + " != " + want.length); return; }
    for (let i = 0; i < want.length; i++) {
        const g = got[i], w = want[i];
        if (Object.is(g, w)) continue;
        if (Number.isNaN(g) && Number.isNaN(w)) continue;
        if (g === w) continue;
        const rel = Math.abs(g - w) / Math.max(1e-300, Math.abs(w));
        if (tol && rel <= tol) continue;
        if (n < 0 || rel > worst) { n = i; worst = rel; }
    }
    ok(n < 0, what, n < 0 ? "" : "first/worst at [" + n + "] got " + got[n] +
                                 " want " + want[n] + " rel " + worst);
}
const arr = (a) => Array.from(a);

const touched = new Set();
function mark(...names) { for (const n of names) touched.add(n); }

const invoked = new Set();

S("isDataFrame");
const isdfFrame = new DataFrame({ x: new Float64Array([1, 2, 3]) });

__kitS.section("isDataFrame(frame) is true", () => {
ok(isDataFrame(isdfFrame) === true, "isDataFrame(frame) is true");
});

__kitS.section("derived frames are frames", () => {
ok(isDataFrame(isdfFrame.SELECT(["x"])) === true, "derived frames are frames");
});

__kitS.section("FROM_RECORDS result is a frame", () => {
ok(isDataFrame(isdfFrame.FROM_RECORDS([{ x: 1 }, { y: 2 }])) === true,
   "FROM_RECORDS result is a frame");
});
__kitS.end();

console.log("test_dataframe: " + pass + " passed, " + fail + " failed");
if (fail) {
    console.log("--- failures ---");
    for (const f of failures) console.log(f);
    throw new Error(fail + " dataframe test(s) failed");
}
