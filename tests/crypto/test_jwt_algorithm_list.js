// JWTVerify's algorithms list, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): every entry must be a string, so a hole or a sparse
// list of length 2^32-1 fails at index 0 instead of being walked, and a
// coercion that throws propagates instead of being left pending.
import { JWTSign, JWTVerify } from "dyna:crypto";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const t0 = performance.now();
    const m = err(f);
    const dt = performance.now() - t0;
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const tok = JWTSign({ sub: "a", exp: 4102444800 }, "k", { alg: "HS256" });
fast("a sparse algorithms list of 2^32-1 slots", () => JWTVerify(tok, "k", { algorithms: huge() }), /^TypeError: JWTVerify: algorithms\[0\] must be a string$/);
eq(err(() => JWTVerify(tok, "k", { algorithms: ["HS256", 1] })), "TypeError: JWTVerify: algorithms[1] must be a string", "a number entry is refused by index");
eq(err(() => JWTVerify(tok, "k", { algorithms: [Symbol("s")] })), "TypeError: JWTVerify: algorithms[0] must be a string", "a symbol entry is refused, not coerced");
{
    const a = ["HS256"];
    Object.defineProperty(a, 1, { get() { throw new RangeError("getter threw"); } });
    eq(err(() => JWTVerify(tok, "k", { algorithms: a })), "RangeError: getter threw", "a throwing getter's own error propagates");
}
eq(JWTVerify(tok, "k", { algorithms: ["RS256", "HS256"] }).sub, "a", "a string list still verifies (control)");

print("test_jwt_algorithm_list" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
