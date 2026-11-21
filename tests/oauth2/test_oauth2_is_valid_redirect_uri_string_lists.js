// covers: dyna:oauth2.isValidRedirectUri
// oauth2 string lists (verifyJWT's algorithms/aud/requiredScope and
// isValidRedirectUri's registered list), found by the signature-driven input
// generator (tests/lib/sigfuzz.js): every entry must be a string, so a sparse
// list of length 2^32-1 fails at index 0, and an audience list that holds the
// token's audience matches it wherever it sits.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_is_valid_redirect_uri_string_lists");
import { verifyJWT, isValidRedirectUri } from "dyna:oauth2";
import { JWTSign } from "dyna:crypto";
import { bestMs } from "../lib/cpuclock.js";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const { ms: dt, value: m } = bestMs(() => err(f));
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const tok = JWTSign({ sub: "a", aud: "app", scope: "read write", exp: 4102444800 }, "k", { alg: "HS256" });
const base = { algorithms: ["HS256"] };
for (const key of ["algorithms", "aud", "requiredScope"])
    fast("verifyJWT: a sparse " + key + " list", () => verifyJWT(tok, "k", { ...base, [key]: huge() }), new RegExp("^TypeError: verifyJWT: " + key + "\\[0\\] must be a string$"));

__kitS.section("isValidRedirectUri: a sparse registered list", () => {
fast("isValidRedirectUri: a sparse registered list", () => isValidRedirectUri("https://a.example/cb", huge()), /^TypeError: isValidRedirectUri: registered\[0\] must be a string$/);
eq(err(() => isValidRedirectUri("https://a.example/cb", ["https://a.example/cb", {}])), "TypeError: isValidRedirectUri: registered[1] must be a string", "isValidRedirectUri: an object entry is refused by index");
});

__kitS.section("isValidRedirectUri: exact match (control)", () => {
eq(isValidRedirectUri("https://a.example/cb", ["https://b.example/", "https://a.example/cb"]), true, "isValidRedirectUri: exact match (control)");
});
__kitS.end();

print("test_oauth2_string_lists" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
