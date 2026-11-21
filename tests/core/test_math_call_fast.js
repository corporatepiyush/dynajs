// covers: c:src/builtins/builtin-math.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_math_call_fast");
let pass = 0, fail = 0;
function same(name, got, want) {
    if (Object.is(got, want)) pass++;
    else { fail++; print(`FAIL ${name}: got ${got} want ${want}`); }
}

const one = ["abs", "floor", "ceil", "round", "sqrt", "acos", "asin", "atan", "cos", "exp", "log", "sin", "tan",
    "trunc", "sign", "cosh", "sinh", "tanh", "acosh", "asinh", "atanh", "expm1", "log1p", "log2", "log10", "cbrt",
    "fround", "f16round"];
const two = ["pow", "atan2"];
const vals = [0, -0, 1, -1, 2, 3, 7, 0.5, -0.5, 1.5, -1.5, 2.5, 1e-310, -1e-310, 1e300, -1e300, 2 ** 31, -(2 ** 31),
    2 ** 31 - 1, 2 ** 53, Infinity, -Infinity, NaN, Math.PI, 0.49999999999999994, 4503599627370495.5];

for (const f of one) {
    const fn = Math[f];
    for (const v of vals) {
        const want = Reflect.apply(fn, Math, [v]);
        same(`Math.${f}(${v}) method`, Math[f](v), want);
        const g = Math[f];
        same(`${f}(${v}) plain call`, g(v), want);
        let local = v;
        same(`Math.${f}(local ${v})`, fn.call(Math, local), want);
    }
}
same("floor fused local", (() => { const x = -2.5; return Math.floor(x); })(), -3);
same("round -0.4 keeps -0", Math.round(-0.4), -0);
same("ceil -0.5 is -0", Math.ceil(-0.5), -0);
for (const f of two) {
    const fn = Math[f];
    for (const a of vals)
        for (const b of vals) {
            const want = Reflect.apply(fn, Math, [a, b]);
            same(`Math.${f}(${a}, ${b})`, Math[f](a, b), want);
            const g = Math[f];
            same(`${f}(${a}, ${b}) plain`, g(a, b), want);
        }
}

same("floor() no arg", Math.floor(), NaN);
same("pow(2) one arg", Math.pow(2), NaN);
same("atan2(1) one arg", Math.atan2(1), NaN);
same("floor extra args", Math.floor(2.7, 99), 2);
__kitS.section("pow(2) in arrow", () => {
    const a = () => Math.pow(2);
    const c = () => { let s = 0; for (let i = 0; i < 3; i++) s += Math.max(i, 5, 9); return Math.pow(2); };
    const g = () => { const t = [1.5, 2.5, 3.5]; return Math.pow(t[0]); };
    const h = () => Math.atan2(1) + Math.pow(3) + (5 + 6);
    for (let r = 0; r < 3; r++) {
        same("pow(2) in arrow", a(), NaN);
        same("pow(2) after a loop of max calls", c(), NaN);
        same("pow(t[0]) one arg", g(), NaN);
        same("atan2/pow one arg sum", h(), NaN);
    }
});
__kitS.end();

let calls = 0;
const obj = { valueOf() { calls++; return 4.6; } };
same("floor valueOf", Math.floor(obj), 4);
same("valueOf ran once", calls, 1);
calls = 0;
same("pow valueOf second", Math.pow(2, obj), Math.pow(2, 4.6));
same("pow valueOf ran", calls, 1);
same("floor string", Math.floor("3.9"), 3);
same("pow string", Math.pow("2", 10), 1024);
same("sqrt bool", Math.sqrt(true), 1);
same("floor null", Math.floor(null), 0);
same("floor undefined", Math.floor(undefined), NaN);
let threw = false;
try { Math.floor(Symbol("s")); } catch (e) { threw = e instanceof TypeError; }
same("floor symbol throws", threw, true);
threw = false;
try { Math.floor(1n); } catch (e) { threw = e instanceof TypeError; }
same("floor bigint throws", threw, true);

const saved = Math.floor;
Math.floor = (x) => "patched:" + x;
same("patched floor", Math.floor(1.5), "patched:1.5");
Math.floor = saved;
same("restored floor", Math.floor(1.5), 1);

// The recursion depth a build allows depends on its frame size: under the same
// 1 MB budget a release build reaches ~650 frames and an ASan build ~290, so a
// fixed deep(500) was a RangeError under ASan only. Probe the limit and call the
// fast path at 80% of it, which is deep in every build.
function deep(n) { return n === 0 ? Math.sqrt(16) : deep(n - 1); }
let maxDepth = 0;
function probe(n) { maxDepth = n; return probe(n + 1) + 1; }
try { probe(0); } catch (e) { if (!(e instanceof RangeError)) throw e; }
same("recursion limit is a RangeError in the hundreds", maxDepth >= 200, true);
same("deep recursion", deep(Math.floor(maxDepth * 0.8)), 4);

let acc = 0;
for (let i = 0; i < 100000; i++) acc += Math.floor(i / 3) - ((i - (i % 3)) / 3);
same("loop agreement", acc, 0);

print(`math call fast path: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("math call fast path failed");
