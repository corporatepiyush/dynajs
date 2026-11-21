// covers: dyna:crypto.JWTVerify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_jwt_verify");
import { JWTSign, JWTVerify } from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const KEY = new TextEncoder().encode("a-shared-secret-of-some-length");
const A = { algorithms: ["HS256"] };

const t = JWTSign({ sub: "u1", n: 42 }, KEY);

const p = JWTVerify(t, KEY, A);

__kitS.section("round trip preserves the payload", () => {
ok(p.sub === "u1" && p.n === 42, "round trip preserves the payload");
});

function throws(f, what) { n++; try { f(); bad++; print("FAIL: " + what); }
                           catch (e) {  } }

__kitS.section("algorithms omitted must be refused", () => {
throws(() => JWTVerify(t, KEY, {}), "algorithms omitted must be refused");
});

__kitS.section("empty allowlist refuses", () => {
throws(() => JWTVerify(t, KEY, { algorithms: [] }), "empty allowlist refuses");
});

__kitS.section("alg not in allowlist", () => {
throws(() => JWTVerify(t, KEY, { algorithms: ["HS512"] }), "alg not in allowlist");
});

const b64 = (o) => Base64URLEncode(new TextEncoder().encode(JSON.stringify(o)));

const none = b64({alg:"none",typ:"JWT"}) + "." + b64({sub:"admin"}) + ".";

__kitS.section("alg:none refused even if allowlisted", () => {
throws(() => JWTVerify(none, KEY, { algorithms: ["none"] }), "alg:none refused even if allowlisted");
});

__kitS.section("alg:none refused", () => {
throws(() => JWTVerify(none, KEY, A), "alg:none refused");
});

const parts = t.split(".");

const tampered = parts[0] + "." + b64({sub:"admin",n:42}) + "." + parts[2];

__kitS.section("tampered payload", () => {
throws(() => JWTVerify(tampered, KEY, A), "tampered payload");
});

__kitS.section("wrong key", () => {
throws(() => JWTVerify(t, new TextEncoder().encode("wrong-secret-entirely"), A), "wrong key");
});

__kitS.section("too few parts", () => {
throws(() => JWTVerify("not.a", KEY, A), "too few parts");
});

__kitS.section("empty token", () => {
throws(() => JWTVerify("", KEY, A), "empty token");
});

__kitS.section("oversize signature", () => {
for (const N of [88, 92, 180]) {
  const forged = parts[0] + "." + parts[1] + "." + "A".repeat(N);
  throws(() => JWTVerify(forged, KEY, A),
         "oversize signature (" + N + " chars) does not verify");
}
});

__kitS.section("round trip", () => {
for (const alg of ["HS384","HS512"]) {
  const tk = JWTSign({ x: 1 }, KEY, { alg });
  ok(JWTVerify(tk, KEY, { algorithms: [alg] }).x === 1, alg + " round trip");
  throws(() => JWTVerify(tk, KEY, A), alg + " refused when only HS256 allowed");
}
});
__kitS.end();

print("test_crypto_jwt: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
