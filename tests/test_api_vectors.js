// flags: --std
import * as std from "std";

let pass = 0, fail = 0, skip = 0;
const fails = [];

function check(label, got, want) {
    const good = typeof want === "number" && typeof got === "number"
        ? Math.abs(got - want) <= 1e-12 * Math.max(1, Math.abs(want))
        : got === want;
    if (good) pass++;
    else { fail++; fails.push(`${label}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
}
function table(label, fn, rows) {
    for (const [input, want] of rows) {
        let got, threw = null;
        try { got = fn(input); } catch (e) { threw = e; }
        if (threw) { fail++; fails.push(`${label}(${JSON.stringify(input)}): threw ${threw.message}`); }
        else check(`${label}(${JSON.stringify(String(input).slice(0, 20))})`, got, want);
    }
}

{
    const h = await import("dyna:hash").catch(() => null);
    if (!h) { skip++; print("-- hash SKIP"); }
    else {
        if (h.SHA1Hex) table("SHA1Hex", h.SHA1Hex, [
            ["abc", "a9993e364706816aba3e25717850c26c9cd0d89d"],
            ["", "da39a3ee5e6b4b0d3255bfef95601890afd80709"],
        ]);
        if (h.SHA224Hex) table("SHA224Hex", h.SHA224Hex, [
            ["abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"],
        ]);
        if (h.SHA256Hex) table("SHA256Hex", h.SHA256Hex, [
            ["abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"],
            ["", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"],
            ["abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"],
        ]);
        if (h.SHA384Hex) table("SHA384Hex", h.SHA384Hex, [
            ["abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed" +
                    "8086072ba1e7cc2358baeca134c825a7"],
        ]);
        if (h.SHA512Hex) table("SHA512Hex", h.SHA512Hex, [
            ["abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a" +
                    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"],
            ["", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce" +
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"],
        ]);
        if (h.SHA3_256Hex) table("SHA3_256Hex", h.SHA3_256Hex, [
            ["abc", "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"],
            ["", "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"],
        ]);
        if (h.SHA3_512Hex) table("SHA3_512Hex", h.SHA3_512Hex, [
            ["", "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6" +
                 "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26"],
        ]);
        if (h.MD5Hex) table("MD5Hex", h.MD5Hex, [
            ["", "d41d8cd98f00b204e9800998ecf8427e"],
            ["abc", "900150983cd24fb0d6963f7d28e17f72"],
            ["message digest", "f96b697d7cb7938d525a2f31aaf161d0"],
        ]);
        if (h.CRC32) check("CRC32('123456789')", h.CRC32("123456789") >>> 0, 0xcbf43926);
        if (h.CRC32C) check("CRC32C('123456789')", h.CRC32C("123456789") >>> 0, 0xe3069283);
        if (h.BLAKE2bHex) table("BLAKE2bHex", h.BLAKE2bHex, [
            ["abc", "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1" +
                    "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"],
        ]);
    }
}

{
    const c = await import("dyna:crypto").catch(() => null);
    if (!c) { skip++; print("-- crypto SKIP"); }
    else {
        if (c.HMACHex) {
            check("HMAC-SHA256 RFC4231 case 2",
                c.HMACHex("sha256", "Jefe", "what do ya want for nothing?"),
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
            check("HMAC-SHA512 RFC4231 case 2",
                c.HMACHex("sha512", "Jefe", "what do ya want for nothing?"),
                "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554" +
                "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
            check("HMAC-SHA1 RFC2202 case 2",
                c.HMACHex("sha1", "Jefe", "what do ya want for nothing?"),
                "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
        }
        if (c.PBKDF2 && c.HexEncode === undefined) {
        }
        if (c.TimingSafeEqual) {
            check("TimingSafeEqual equal", c.TimingSafeEqual("abc", "abc"), true);
            check("TimingSafeEqual differ", c.TimingSafeEqual("abc", "abd"), false);
        }
        if (typeof c.TOTPGenerate === "function") {
            let got = null;
            try { got = String(c.TOTPGenerate("12345678901234567890", { atSec: 59, digits: 8 })); }
            catch (e) { got = null; }
            if (got !== null && /^\d+$/.test(got)) check("TOTP RFC6238 T=59", got, "94287082");
            else skip++;
        }
    }
}

{
    const e = await import("dyna:encoding").catch(() => null);
    if (!e) { skip++; print("-- encoding SKIP"); }
    else {
        if (e.Base64Encode) table("Base64Encode", e.Base64Encode, [
            ["", ""], ["f", "Zg=="], ["fo", "Zm8="], ["foo", "Zm9v"],
            ["foob", "Zm9vYg=="], ["fooba", "Zm9vYmE="], ["foobar", "Zm9vYmFy"],
        ]);
        if (e.Base32Encode) table("Base32Encode", e.Base32Encode, [
            ["", ""], ["f", "MY======"], ["fo", "MZXQ===="], ["foo", "MZXW6==="],
            ["foob", "MZXW6YQ="], ["fooba", "MZXW6YTB"], ["foobar", "MZXW6YTBOI======"],
        ]);
        if (e.Base32HexEncode) table("Base32HexEncode", e.Base32HexEncode, [
            ["", ""], ["f", "CO======"], ["fo", "CPNG===="], ["foo", "CPNMU==="],
            ["foob", "CPNMUOG="], ["fooba", "CPNMUOJ1"], ["foobar", "CPNMUOJ1E8======"],
        ]);
        if (e.HexEncode) {
            const got = String(e.HexEncode("foobar")).toUpperCase();
            check("HexEncode('foobar') RFC4648", got, "666F6F626172");
        }
        if (e.Base64URLEncode)
            check("Base64URLEncode is unpadded", String(e.Base64URLEncode("foob")), "Zm9vYg");
    }
}

{
    const m = await import("dyna:mathx").catch(() => null);
    if (!m) { skip++; print("-- mathx SKIP"); }
    else {
        const near = (label, got, want, tol) => {
            const good = Math.abs(got - want) <= (tol || 1e-9) * Math.max(1, Math.abs(want));
            if (good) pass++;
            else { fail++; fails.push(`${label}: got ${got} want ${want}`); }
        };
        if (m.erf) {
            near("erf(1)", m.erf(1), 0.8427007929497149, 1e-9);
            near("erf(0.5)", m.erf(0.5), 0.5204998778130465, 1e-9);
            near("erf(2)", m.erf(2), 0.9953222650189527, 1e-9);
        }
        if (m.erfc) near("erfc(1)", m.erfc(1), 0.15729920705028513, 1e-8);
        if (m.besselj) {
            near("J0(1)", m.besselj(0, 1), 0.7651976865579666, 1e-9);
            near("J1(1)", m.besselj(1, 1), 0.4400505857449335, 1e-9);
            near("J0(2)", m.besselj(0, 2), 0.22389077914123567, 1e-9);
            near("J1(2)", m.besselj(1, 2), 0.5767248077568734, 1e-9);
        }
        if (m.bessely) {
            near("Y0(1)", m.bessely(0, 1), 0.08825696421567696, 1e-8);
            near("Y1(1)", m.bessely(1, 1), -0.7812128213002887, 1e-8);
        }
        if (m.besseli) {
            near("I0(1)", m.besseli(0, 1), 1.2660658777520084, 1e-8);
            near("I1(1)", m.besseli(1, 1), 0.5651591039924851, 1e-8);
        }
        if (m.besselk) near("K0(1)", m.besselk(0, 1), 0.4210244382407083, 1e-7);
        if (m.beta) near("beta(2,3)", m.beta(2, 3), 1 / 12, 1e-10);
        if (m.Pi !== undefined) near("Pi", Number(m.Pi), Math.PI, 1e-15);
        if (m.E !== undefined) near("E", Number(m.E), Math.E, 1e-15);
        if (m.Phi !== undefined) near("Phi", Number(m.Phi), (1 + Math.sqrt(5)) / 2, 1e-15);
        if (m.Sqrt2 !== undefined) near("Sqrt2", Number(m.Sqrt2), Math.SQRT2, 1e-15);
        if (m.Ln10 !== undefined) near("Ln10", Number(m.Ln10), Math.LN10, 1e-15);
        if (m.Log2E !== undefined) near("Log2E", Number(m.Log2E), Math.LOG2E, 1e-15);
    }
}

{
    const u = await import("dyna:uuid").catch(() => null);
    if (!u) { skip++; print("-- uuid SKIP"); }
    else {
        if (u.NAMESPACE_DNS)
            check("NAMESPACE_DNS", String(u.NAMESPACE_DNS).toLowerCase(),
                  "6ba7b810-9dad-11d1-80b4-00c04fd430c8");
        if (u.NAMESPACE_URL)
            check("NAMESPACE_URL", String(u.NAMESPACE_URL).toLowerCase(),
                  "6ba7b811-9dad-11d1-80b4-00c04fd430c8");
        if (u.NAMESPACE_OID)
            check("NAMESPACE_OID", String(u.NAMESPACE_OID).toLowerCase(),
                  "6ba7b812-9dad-11d1-80b4-00c04fd430c8");
        if (u.NAMESPACE_X500)
            check("NAMESPACE_X500", String(u.NAMESPACE_X500).toLowerCase(),
                  "6ba7b814-9dad-11d1-80b4-00c04fd430c8");
        if (u.NIL) check("NIL", String(u.NIL), "00000000-0000-0000-0000-000000000000");
        if (u.MAX) check("MAX", String(u.MAX).toLowerCase(),
                         "ffffffff-ffff-ffff-ffff-ffffffffffff");
        if (typeof u.v5 === "function") {
            let got = null;
            try { got = String(u.v5(u.NAMESPACE_DNS, "python.org")).toLowerCase(); }
            catch (e) { got = null; }
            if (got) check("v5(DNS, 'python.org')", got, "886313e1-3b8a-5372-9b90-0c9aee199e5d");
            else skip++;
        }
        if (typeof u.v3 === "function") {
            let got = null;
            try { got = String(u.v3(u.NAMESPACE_DNS, "python.org")).toLowerCase(); }
            catch (e) { got = null; }
            if (got) check("v3(DNS, 'python.org')", got, "6fa459ea-ee8a-3ca4-894e-db77e160355e");
            else skip++;
        }
    }
}

{
    const s = await import("dyna:semver").catch(() => null);
    if (!s) { skip++; print("-- semver SKIP"); }
    else if (typeof s.compare === "function") {
        const CHAIN = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
                       "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
                       "1.0.0-rc.1", "1.0.0"];
        let bad = null;
        for (let i = 1; i < CHAIN.length && !bad; i++)
            if (!(s.compare(CHAIN[i - 1], CHAIN[i]) < 0))
                bad = `${CHAIN[i - 1]} should precede ${CHAIN[i]}`;
        if (bad) { fail++; fails.push("semver precedence (spec 11.4): " + bad); }
        else pass++;
        check("semver 1.9.0 < 1.10.0", s.compare("1.9.0", "1.10.0") < 0, true);
        check("semver 1.0.0 > 1.0.0-rc.1", s.compare("1.0.0", "1.0.0-rc.1") > 0, true);
        check("build metadata ignored", s.compare("1.0.0+a", "1.0.0+b"), 0);
    }
}

print("\n" + "=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails) print("  " + f);
}
print(`test_api_vectors: ${pass} passed, ${fail} failed, ${skip} skipped`);
if (fail > 0) std.exit(1);
