/* test_jwt_asym.js -- JWT RS256/ES256 (design 13, on design 12's keys).
 *
 * THE ORACLE IS FOREIGN. A sign/verify round trip against ourselves would pass
 * for a token no other library accepts -- and ES* is exactly where that
 * happens, because JWS wants a raw R||S pair while OpenSSL emits DER. So every
 * signature produced here is checked by `openssl dgst -verify`, and the raw->DER
 * repacking needed to do that is done in PYTHON, not by the C under test, or
 * the check would be circular.
 *
 * Needs CONFIG_TLS=y, openssl(1) and python3. A skip is loud, and fatal under
 * DYNAJS_REQUIRE_TOOLS=1.
 */
import * as crypto from "dyna:crypto";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";
import { Base64URLDecode, Base64URLEncode } from "dyna:encoding";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
function ok(cond, what, detail) {
    if (cond) { pass++; print("  ok    " + what); }
    else { fail++; print("  FAIL  " + what + (detail ? "  [" + detail + "]" : "")); }
}
function throws(fn, what) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    ok(t, what, t ? "" : "did NOT throw");
}
function skipped(what) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + what); return; }
    skip++; print("  SKIP  " + what);
}
const sh = (cmd) => Exec("/bin/sh", ["-c", cmd]).code;
const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const b64url = (s) => Base64URLDecode(s);

if (!crypto.Ed25519Sign) {
    print("test_jwt_asym: SKIP -- built without CONFIG_TLS");
    print("test_jwt_asym: 0 passed, 0 failed");
} else if (!Which("openssl") || !Which("python3")) {
    skipped("openssl(1) or python3 missing -- the FOREIGN oracle cannot run");
    print("test_jwt_asym: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_jwt_asym: " + fail + " failures");
} else {
    const T = makeTempDir("jwtasym");
    /* dyna:file takes a Path, not a string. Keep the raw string for shell
       interpolation and wrap only where the module is called. */
    const P = (n) => T + "/" + n;
    const FP = (n) => new Path(P(n));
    const slurp = (n) => readFile(FP(n), { encoding: "utf8" });

    /* Keys come from openssl, so nothing about them originates in the code
       under test. */
    sh(`openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out ${P("rsa.key")} 2>/dev/null`);
    sh(`openssl pkey -in ${P("rsa.key")} -pubout -out ${P("rsa.pub")} 2>/dev/null`);
    sh(`openssl ecparam -name prime256v1 -genkey -noout -out ${P("ec.key")} 2>/dev/null`);
    sh(`openssl pkey -in ${P("ec.key")} -pubout -out ${P("ec.pub")} 2>/dev/null`);

    /* raw hex -> binary, and (for ES) raw R||S -> DER. Independent of our C. */
    writeFile(FP("pack.py"), [
        "import binascii,sys",
        "raw=binascii.unhexlify(open(sys.argv[1]).read().strip())",
        "if sys.argv[3]=='der':",
        "    n=len(raw)//2",
        "    r=int.from_bytes(raw[:n],'big'); s=int.from_bytes(raw[n:],'big')",
        "    def i(v):",
        "        b=v.to_bytes((v.bit_length()+8)//8 or 1,'big')",
        "        return b'\\x02'+bytes([len(b)])+b",
        "    body=i(r)+i(s); raw=b'\\x30'+bytes([len(body)])+body",
        "open(sys.argv[2],'wb').write(raw)",
    ].join("\n"));

    const payload = { iss: "dynajs", sub: "test", n: 1 };

    for (const [alg, keyf, pubf] of [["RS256", "rsa.key", "rsa.pub"],
                                     ["ES256", "ec.key",  "ec.pub"]]) {
        const priv = slurp(keyf), pub = slurp(pubf);
        const tok = crypto.JWTSign(payload, priv, { alg });
        ok(typeof tok === "string" && tok.split(".").length === 3,
           alg + ": JWTSign produces a three-part token");
        const back = crypto.JWTVerify(tok, pub, { algorithms: [alg] });
        ok(back && back.iss === "dynajs", alg + ": it verifies with the public key");

        const parts = tok.split("."), sig = b64url(parts[2]);
        if (alg === "ES256")
            ok(sig.length === 64,
               "ES256 signature is a raw 64-byte R||S pair, not DER " +
               "(got " + sig.length + ")");

        /* ---------- THE FOREIGN CHECK ---------- */
        writeFile(FP("input.bin"), parts[0] + "." + parts[1]);
        writeFile(FP("raw.hex"), tohex(sig));
        sh(`python3 ${P("pack.py")} ${P("raw.hex")} ${P("sig.bin")} ` +
           (alg === "ES256" ? "der" : "raw"));
        const rc = sh(`openssl dgst -sha256 -verify ${P(pubf)} ` +
                      `-signature ${P("sig.bin")} ${P("input.bin")} >/dev/null 2>&1`);
        ok(rc === 0, alg + ": openssl(1) ACCEPTS the signature we produced " +
                     "(foreign oracle, non-circular)");

        /* ---------- refusals ---------- */
        throws(() => crypto.JWTVerify(tok, pub, { algorithms: ["HS256"] }),
               alg + ": refused when its alg is not in the allowlist");
        throws(() => crypto.JWTVerify(
                   parts[0] + "." + parts[1].slice(0, -2) + "AA." + parts[2],
                   pub, { algorithms: [alg] }),
               alg + ": a tampered payload does not verify");
        throws(() => crypto.JWTVerify(
                   parts[0] + "." + parts[1] + "." + parts[2].slice(0, -2) +
                   (parts[2].endsWith("AA") ? "BB" : "AA"),
                   pub, { algorithms: [alg] }),
               alg + ": a tampered signature does not verify");
        throws(() => crypto.JWTVerify(tok, "not a pem", { algorithms: [alg] }),
               alg + ": a non-PEM key is refused");
        /* The classic confusion: sign asymmetric, then present the PUBLIC key
           as an HMAC secret. The allowlist alone closes it. */
        throws(() => crypto.JWTVerify(tok, pub, { algorithms: ["HS256", "HS512"] }),
               alg + ": RS/ES->HS confusion is refused");
    }

    /* alg:none stays refused even if a caller allowlists it. */
    {
        const enc = new TextEncoder();
        const hdr = Base64URLEncode(enc.encode('{"alg":"none"}'));
        const body = Base64URLEncode(enc.encode(JSON.stringify(payload)));
        throws(() => crypto.JWTVerify(hdr + "." + body + ".", "secret",
                                      { algorithms: ["none"] }),
               "alg:none is refused even when it is allowlisted");
    }

    /* CONTROL: the symmetric path must be untouched by all of this. */
    {
        const t = crypto.JWTSign(payload, "secret", { alg: "HS256" });
        const p = crypto.JWTVerify(t, "secret", { algorithms: ["HS256"] });
        ok(p && p.iss === "dynajs", "CONTROL: the HS256 path still round trips");
    }

    removeAll(new Path(T));
    print("test_jwt_asym: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_jwt_asym: " + fail + " failures");
}
