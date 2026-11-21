// covers: dyna:crypto.JWTSign
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_jwt_sign");
import { JWTSign, JWTVerify } from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };
const KEY = new TextEncoder().encode("a-shared-secret-of-some-length");
const A = { algorithms: ["HS256"] };

const t = JWTSign({ sub: "u1", n: 42 }, KEY);

__kitS.section("three parts", () => {
ok(t.split(".").length === 3, "three parts");
});

function throws(f, what) { n++; try { f(); bad++; print("FAIL: " + what); }
                           catch (e) {  } }

const parts = t.split(".");

__kitS.section("unsupported alg refused at sign", () => {
throws(() => JWTSign({}, KEY, { alg: "RS256" }), "unsupported alg refused at sign");
});

__kitS.section("RS256 with an EC private key refused", () => {
if (typeof RSA !== "undefined" && typeof ECDSA !== "undefined") {
    const rsa = RSA.generate(2048);
    const ec = ECDSA.generate("P-256");
    const rs = JWTSign({ sub: "x" }, rsa.privateKey, { alg: "RS256" });
    throws(() => JWTSign({ sub: "x" }, ec.privateKey, { alg: "RS256" }),
           "RS256 with an EC private key refused");
    throws(() => JWTVerify(rs, ec.publicKey, { algorithms: ["RS256"] }),
           "RS256 verified against an EC public key refused");
    throws(() => JWTSign({ sub: "y" }, ec.privateKey, { alg: "ES512" }),
           "ES512 with a P-256 key refused");
    throws(() => JWTSign({ sub: "z" }, rsa.privateKey, { alg: "ES256" }),
           "ES256 with an RSA key refused");
    throws(() => JWTVerify(rs, rsa.publicKey, { algorithms: ["ES256"] }),
           "RS token vs ES allowlist still refused");
    ok(JWTVerify(rs, rsa.publicKey, { algorithms: ["RS256"] }).sub === "x",
       "RS256 roundtrip intact after the strictness fix");
}
});
__kitS.end();

print("test_crypto_jwt: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
