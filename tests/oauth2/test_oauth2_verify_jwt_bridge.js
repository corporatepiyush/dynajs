// covers: integration c:src/modules/oauth2/oauth2-verify.c c:src/modules/oauth2/oauth2-validate.c c:src/modules/oauth2/oauth2-pkce.c -- JWT bridge and char sets
// verifyJWT reaches dyna:crypto's JWTVerify through a bridge module it compiled on EVERY call
// (~70% of a verify); it now compiles it once per realm and reuses the hidden non-writable,
// non-configurable global the bridge defines. A writable, configurable (non-enumerable) global of
// that name planted before the first call must not be called: the first call still replaces it
// with the real JWTVerify. Over tokens that are valid, signed with another key, expired, for
// another audience or issuer, or missing the required scope, every call must give the outcome
// the claim rules give. The PKCE verifier and b64token character tests are now one load from a
// 256-bit set, indexed without a branch (7 range compares per byte were 20% of
// generateCodeChallenge; a mask picked by c < 64 / c < 128 branches was 20% slower than the
// compares); over every code unit 0..255 in a valid verifier / token, the answer must match the
// RFC 7636 unreserved / RFC 6750 b64token sets.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_verify_jwt_bridge");
import * as O from "dyna:oauth2";
import * as CR from "dyna:crypto";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
const enc = new TextEncoder();
const key = enc.encode("k".repeat(32)), other = enc.encode("o".repeat(32));
let planted = 0;
Object.defineProperty(globalThis, "__oauth_jwt_verify", { value: () => { planted++; return { sub: "planted", aud: "api", iss: "me", scope: "read", exp: 4e9 }; }, writable: true, configurable: true, enumerable: false });

__kitS.section("claims over many tokens, bridge reused", () => {
    const opts = { algorithms: ["HS256"], aud: "api", iss: "me", requiredScope: ["read"] };
    for (let i = 0; i < 600; i++) {
        const kind = i % 6;
        const claims = { sub: "u" + i, iat: 1700000000, exp: kind === 2 ? 1000 : 4000000000, aud: kind === 3 ? "web" : "api", iss: kind === 4 ? "you" : "me", scope: kind === 5 ? "write" : "read write" };
        const t = CR.JWTSign(claims, kind === 1 ? other : key, { alg: "HS256" });
        let got;
        try { got = "ok:" + O.verifyJWT(t, key, opts).sub; } catch (e) { got = "err"; }
        eq(got, kind === 0 ? "ok:u" + i : "err", "token kind " + kind + " #" + i);
    }
    eq(planted, 0, "the planted writable global was never called");
    const d = Object.getOwnPropertyDescriptor(globalThis, "__oauth_jwt_verify");
    eq(d.value === CR.JWTVerify && !d.writable && !d.configurable && !d.enumerable, true, "the bridge holds dyna:crypto JWTVerify, locked");
});

__kitS.section("verifier and b64token character sets", () => {
    const unres = (c) => /[A-Za-z0-9\-._~]/.test(c);
    const b64 = (c) => /[A-Za-z0-9\-._~+/]/.test(c);
    for (let u = 0; u < 256; u++) {
        const c = String.fromCharCode(u);
        for (const at of [0, 21, 42]) {
            const v = "a".repeat(at) + c + "b".repeat(42 - at);
            const ok = unres(c) && v.length === 43;
            eq(O.isValidCodeVerifier(v), ok, "verifier with code unit " + u + " at " + at);
            let ch;
            try { ch = O.generateCodeChallenge(v, "plain") === v; } catch (e) { ch = false; }
            eq(ch, ok, "challenge of a verifier with code unit " + u + " at " + at);
        }
        for (const t of [c, "ab" + c + "cd", c + "x=="]) {
            const want = [...t.replace(/=+$/, "")].every(b64) && t.replace(/=+$/, "").length > 0 && t.length - t.replace(/=+$/, "").length <= 2;
            eq(O.isValidBearerToken(t), want, "b64token " + JSON.stringify(t));
            let hb;
            try { hb = O.buildBearerHeader(t) === "Bearer " + t; } catch (e) { hb = false; }
            eq(hb, want, "bearer header for " + JSON.stringify(t));
        }
    }
});

__kitS.end();
print(`test_oauth2_verify_jwt_bridge: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
