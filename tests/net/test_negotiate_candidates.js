// Negotiate / NegotiateToken candidate intake, found by the signature-driven
// input generator (tests/lib/sigfuzz.js):
//   - a sparse candidates array of length 2^32-1 used to allocate 16 bytes per
//     slot up front and stringify four billion holes (minutes, gigabytes);
//     every entry must now be a string, so a hole fails at its index at once;
//   - a getter that threw, or an entry no string can be made from, left its
//     exception pending behind a normal return value;
//   - the winner was read back through the array a second time, so a getter
//     could hand back something other than the candidate that matched.
import { Negotiate, NegotiateToken } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

for (const [name, fn, hdr] of [["Negotiate", Negotiate, "text/html"], ["NegotiateToken", NegotiateToken, "en"]]) {
    for (const len of [2 ** 20, 2 ** 31, 2 ** 32 - 1]) {
        const c = [];
        c.length = len;
        const t0 = performance.now();
        const m = err(() => fn(hdr, c));
        const dt = performance.now() - t0;
        ok(new RegExp("^TypeError: " + name + ": candidates\\[0\\] is not a string").test(m), `${name}: a ${len}-slot sparse array is refused at index 0 (${m})`);
        ok(dt < 500, `${name}: and refused before any work (${dt.toFixed(1)} ms)`);
    }
    eq(err(() => fn(hdr, ["a", 3])), `TypeError: ${name}: candidates[1] is not a string`, `${name}: a number entry is refused by index`);
    eq(err(() => fn(hdr, ["a", Symbol("s")])), `TypeError: ${name}: candidates[1] is not a string`, `${name}: a symbol entry is refused, not left pending`);
    const boom = ["a"];
    Object.defineProperty(boom, 1, { get() { throw new RangeError("getter threw"); } });
    eq(err(() => fn(hdr, boom)), "RangeError: getter threw", `${name}: a throwing getter's own exception propagates`);
    eq(err(() => fn("", ["x", 1])), `TypeError: ${name}: candidates[1] is not a string`, `${name}: an empty header validates the candidates too`);
    eq(fn("", []), null, `${name}: no candidates -> null (control)`);
}

{
    let reads = 0;
    const flip = ["text/plain", "text/html"];
    Object.defineProperty(flip, 1, { get() { reads++; return reads === 1 ? "text/html" : "application/evil"; } });
    eq(Negotiate("text/html", flip), "text/html", "the matched candidate is returned, not a second read of the array");
    eq(reads, 1, "each candidate is read exactly once");
}
eq(Negotiate("text/html;q=0.5, application/json", ["text/html", "application/json"]), "application/json", "q-values still decide (control)");
eq(NegotiateToken("fr, en;q=0.5", ["en", "fr"]), "fr", "tokens still decide (control)");
eq(Negotiate("", ["application/json", "text/html"]), "application/json", "an empty header still takes the first candidate (control)");

print("test_negotiate_candidates: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
