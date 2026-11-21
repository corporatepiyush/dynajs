// covers: dyna:crypto.JWTVerify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_jwt_verify_confusion");
import { JWTSign, JWTVerify } from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
const b64u = (s) => Base64URLEncode(new TextEncoder().encode(s));
const secret = "hush";
const t = JWTSign({ sub: "x" }, secret, { alg: "HS256" });
let threw = 0; try { JWTVerify(t, secret, { algorithms: ["RS256"] }); } catch (e) { threw = 1; }

__kitS.section("RS allowlist refuses HS token", () => {
ok(threw, "RS allowlist refuses HS token");
});

const forged = b64u(JSON.stringify({alg:"none"})) + "." + b64u(JSON.stringify({sub:"attacker"})) + ".";

__kitS.section("alg=none refused under HS allowlist", () => {
threw = 0; try { JWTVerify(forged, secret, { algorithms: ["HS256"] }); } catch (e) { threw = 1; }
ok(threw, "alg=none refused under HS allowlist");
});

__kitS.section("alg=none refused even when allowlisted", () => {
threw = 0; try { JWTVerify(forged, secret, { algorithms: ["none"] }); } catch (e) { threw = 1; }
ok(threw, "alg=none refused even when allowlisted");
});

__kitS.section("correct allowlist verifies", () => {
ok(JWTVerify(t, secret, { algorithms: ["HS256"] }).sub === "x", "correct allowlist verifies");
print("test_jwt_confusion: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_jwt_confusion: " + fail + " failures");
});
__kitS.end();
