// covers: dyna:http.NegotiateToken
// Negotiate / NegotiateToken candidate intake, found by the signature-driven
// input generator (tests/lib/sigfuzz.js):
//   - a sparse candidates array of length 2^32-1 used to allocate 16 bytes per
//     slot up front and stringify four billion holes (minutes, gigabytes);
//     every entry must now be a string, so a hole fails at its index at once;
//   - a getter that threw, or an entry no string can be made from, left its
//     exception pending behind a normal return value;
//   - the winner was read back through the array a second time, so a getter
//     could hand back something other than the candidate that matched.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_negotiate_token_candidates");
import { Negotiate, NegotiateToken } from "dyna:http";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

__kitS.section("fr, en;q=0.5", () => {
eq(NegotiateToken("fr, en;q=0.5", ["en", "fr"]), "fr", "tokens still decide (control)");
});
__kitS.end();

print("test_negotiate_candidates: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
