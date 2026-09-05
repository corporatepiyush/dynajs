/* JWT. A round trip proves almost nothing here -- the whole value of a JWT
   library is what it REFUSES, so these are the attacks. */
import { JWTSign, JWTVerify } from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const KEY = new TextEncoder().encode("a-shared-secret-of-some-length");
const A = { algorithms: ["HS256"] };

const t = JWTSign({ sub: "u1", n: 42 }, KEY);
ok(t.split(".").length === 3, "three parts");
const p = JWTVerify(t, KEY, A);
ok(p.sub === "u1" && p.n === 42, "round trip preserves the payload");

function throws(f, what) { n++; try { f(); bad++; print("FAIL: " + what); }
                           catch (e) { /* expected */ } }

/* THE attacks */
throws(() => JWTVerify(t, KEY, {}), "algorithms omitted must be refused");
throws(() => JWTVerify(t, KEY, { algorithms: [] }), "empty allowlist refuses");
throws(() => JWTVerify(t, KEY, { algorithms: ["HS512"] }), "alg not in allowlist");
/* alg:none -- a forged header with no signature */
const b64 = (o) => Base64URLEncode(new TextEncoder().encode(JSON.stringify(o)));
const none = b64({alg:"none",typ:"JWT"}) + "." + b64({sub:"admin"}) + ".";
throws(() => JWTVerify(none, KEY, { algorithms: ["none"] }), "alg:none refused even if allowlisted");
throws(() => JWTVerify(none, KEY, A), "alg:none refused");
/* a tampered payload must not verify */
const parts = t.split(".");
const tampered = parts[0] + "." + b64({sub:"admin",n:42}) + "." + parts[2];
throws(() => JWTVerify(tampered, KEY, A), "tampered payload");
/* wrong key */
throws(() => JWTVerify(t, new TextEncoder().encode("wrong-secret-entirely"), A), "wrong key");
/* malformed */
throws(() => JWTVerify("not.a", KEY, A), "too few parts");
throws(() => JWTVerify("", KEY, A), "empty token");
/* forged-oversize-signature: a valid HS256 header+payload with a signature
   segment too long for the fixed stack buffer. The decoder has no output
   capacity; a segment of 88+ base64url chars wrote past got[64] before the
   MAC compare. It must now throw cleanly, not corrupt the stack. */
for (const N of [88, 92, 180]) {
  const forged = parts[0] + "." + parts[1] + "." + "A".repeat(N);
  throws(() => JWTVerify(forged, KEY, A),
         "oversize signature (" + N + " chars) does not verify");
}
/* HS384/HS512 round trip */
for (const alg of ["HS384","HS512"]) {
  const tk = JWTSign({ x: 1 }, KEY, { alg });
  ok(JWTVerify(tk, KEY, { algorithms: [alg] }).x === 1, alg + " round trip");
  throws(() => JWTVerify(tk, KEY, A), alg + " refused when only HS256 allowed");
}
throws(() => JWTSign({}, KEY, { alg: "RS256" }), "unsupported alg refused at sign");

print("test_crypto_jwt: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
