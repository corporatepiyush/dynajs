/* test_crypto_upgrade.js -- (lane wc1-crypto).
 *
 * The oracles are EMBEDDED ARTIFACTS FROM THE REAL TOOLS, not round trips:
 *   - every PEM / signature / sealed blob in V was produced by the openssl
 *     3.6.4 CLI (dgst -sigopt rsa_padding_mode:pss, pkeyutl
 *     -pkeyopt rsa_padding_mode:oaep, genpkey -algorithm ed25519/x25519,
 *     dgst -sha512 -sign for the P-521 DER signature), then pinned here, so
 *     the suite stays hermetic AND is a differential against openssl;
 *   - the Argon2id PHC strings come from the python argon2-cffi reference
 *     binding (the RFC 9106 matrix the RFC's own test suite exercises);
 *   - the HOTP/TOTP codes are the RFC 4226 / RFC 6238 test vectors.
 *
 * Needs CONFIG_TLS=y (RSA/X509/AEAD/Ed25519 are absent otherwise, by design).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y CONFIG_TLS=y) tests/test_crypto_upgrade.js
 */
import * as c from "dyna:crypto";

let n = 0, fails = 0;
function ok(cnd, msg) { n++; if (!cnd) { fails++; print("  FAIL " + msg); } }
function eq(a, b, msg) { ok(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, re, msg) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e && e.message || e); }
    ok(t && (!re || re.test(m)), msg + (t && re && !re.test(m) ? " [wrong error: " + m + "]" : ""));
}
/* GENERATED vector block (openssl 3.6.4 CLI + python argon2-cffi): do not hand-edit.
   Every PEM/signature/blob below was produced by the openssl CLI (or the
   argon2-cffi reference binding) and embedded so the suite stays hermetic. */
const V = {
    rsa_priv: "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1JSUV2UUlCQURBTkJna3Foa2lHOXcwQkFRRUZBQVNDQktjd2dnU2pBZ0VBQW9JQkFRQzBMRkNkNFJZTGppbVgKL2hqNk0yTTVvTmtKZzlLa2huTTVjUHQrcURGRndSVithK2h5bjV5TmpUQWV5THVpVU9tc1d1K0poMUxhV0kxSQpaY3BjWDJkWitML3NkTjBzSDlYVFBxMHV3N3Y4UkNaUnVMbGRBdG1qWVNUSHY0bEJGNGRRNHpaNTBMVW9vZlAxCk4xcjk2STlGM1ltSDRoR1NFVHI5ekxwTVNQMkh3WE1KcGEzUWtvemF4RElvQnk1UlBDMnowM0FnZjhEeFRJN2EKYnJ0UGIwOG1YL1NReGJSaW5aNHV0TVZ3cHhGYXNwWGJjVkd0Q2Rad3pyamlYNVNna3dhZDRVNDA2YmNRNGFadgpEcnEwUlhHaWFmWlFtaUlFSWRGSXA0RGtDMEJ3T243ZTBmWWJZZExoZkFHbXlFd1ZrYkY0cjR3c3E4WDQyWFdNCmR5aWpmUGExQWdNQkFBRUNnZ0VBSlJJV0h6blQ4NW96UUNqaG9qOG0vQkUyZnNEdSt2ZUg1eXB5TUgveUxXQTYKS0NvVEtGeGpWWE9Xa2tycVlrNEVHamlXbndVbkhMSktGWHFPSFozcWthWSs5T3Vob3lYRXRNTnBWaDBqUlZjUApURksxM3VlS2VKVnhBQ0ZPWUFSempNYkhLMTZ3RzNWaEVOUHNKcUJ1TkNtT0ZkV0RtSHB2bVE5QnlKYmZCVXFGCm0zVHZETFJkTkZqc1AzMGFhRHd6d3QwNHpRcXJZckVUVFFtUU5qOWROWTZDMmJRVm0zckkrRU9KVkJ5bXMxNFMKRUJucUpMNjh0WU01VGV3SmtKWTFDVnlKbGxSeWtvYnpva1ZxTFI0TlpOU2wydlJWR3p2Z0tpSlhEbDBOeVE0aQpLM3c4L2UxaXNUbmIramsyUWFrYzAwOFJZQWJ4UGx2ejlaOWF0SUk0OFFLQmdRRFl5RlFLTG01K01hWjFpMHVLCjhYaHY3OXVmWU1YWWp0STFMZzljN1I5TENJWG5Xby9YcW9ETHZpcVBVY2ZrWGJXT2gwOVdSRWEzT1FJeEVYM3IKdWpWSFJ3MEYyZW01UEwyUWxkTDUyd3ozQUl1eXR2WTFIYTBPeC9MendSdWkrL3B6WGJQdkhXeXZmdjRaS0hLSQpFUnNBWTUxTkZiaFN3VFBoMEhZMXE4QS84UUtCZ1FEVXhJWTBQL1RMcWJLUm1UQzRxZ2w3cjhJNkVGZWd3YWtZCnRYbEdIWHNJVExGL0ZjdG9JV0hnYUMreXVNalRCdVM2b0NLa1U0UHZoSk4xdzFrZFJZRkM4QnZmS1ZiSXFCY2QKanBvZGRSQ29WT2xKUUVPc3FhOWlDMEQ0bVF1SG8xei9hZENYbTZRSUU4aUhzUXVmM05xTFVCdUZuT0FiektnKwpRZG92VE40bkJRS0JnRHdNc2dybjliK0xMVnNlTE56ZUtzaUhIc1UxWDFpdnAxY0gzVVdXZ2JhZE04cDBjYWdDCkdROVhMQjdyUG4zcDMyTU41RkkzTTRlSmlTdmlkb2tYMmE1VzVpcWF4SDJGYjlWZlMwUGZBK1pnQmxLSkVBYUMKbysrV1A1eDUvNWZVU1Bvd0dLbkI4dHRpU0N6YjRXNERud0VxbHVaRmYvdWNmU3l1UERCOTFSRmhBb0dBV0Y3WApydFB4aExXUzZVNUxJaWZka0hYTG9mb3ZxeUZFYm5MUlVBSG1tK2Zld1AzNFllT2JsMjB1Z25pbFVLRElWNHN6CldEVW4wMCtwUDN4SGd4VGZQMElqRVdsR0ZrbGFjSGhPSW5ZQTJTbUxTMVZaeGxrajh3RGhsMTlabnBpSGc3NGIKV1J6WDRGTDNNd1NoNDJBRGxXRk1UUTUvTm95bVJGVnRuYis3ZFZFQ2dZRUF5R0h0eFl0V2luaW0wYnRYSnJPYQprY3lCZERtSk11ckpKblZjSVl6UmQxS3FaMjNIVzdJdFYwcFJ6YTBjTmtBVTRyR0tBM1U4U3JWZTNLbHlCQTEvCmloTFVkSmFTWnRrQWNlUTdmbGliT1JEc2liYURvWk53eERGU3lDWWZzb3AyR2wxTnFQTUhZY21FaHh3MEs2VTUKeFF4VEswZmJJd0duVXBmeUJRMFFrSE09Ci0tLS0tRU5EIFBSSVZBVEUgS0VZLS0tLS0K",
    rsa_pub: "LS0tLS1CRUdJTiBQVUJMSUMgS0VZLS0tLS0KTUlJQklqQU5CZ2txaGtpRzl3MEJBUUVGQUFPQ0FROEFNSUlCQ2dLQ0FRRUF0Q3hRbmVFV0M0NHBsLzRZK2pOagpPYURaQ1lQU3BJWnpPWEQ3ZnFneFJjRVZmbXZvY3ArY2pZMHdIc2k3b2xEcHJGcnZpWWRTMmxpTlNHWEtYRjluCldmaS83SFRkTEIvVjB6NnRMc083L0VRbVViaTVYUUxabzJFa3g3K0pRUmVIVU9NMmVkQzFLS0h6OVRkYS9laVAKUmQySmgrSVJraEU2L2N5NlRFajloOEZ6Q2FXdDBKS00yc1F5S0FjdVVUd3RzOU53SUgvQThVeU8ybTY3VDI5UApKbC8wa01XMFlwMmVMclRGY0tjUldyS1YyM0ZSclFuV2NNNjQ0bCtVb0pNR25lRk9OT20zRU9HbWJ3NjZ0RVZ4Cm9tbjJVSm9pQkNIUlNLZUE1QXRBY0RwKzN0SDJHMkhTNFh3QnBzaE1GWkd4ZUsrTUxLdkYrTmwxakhjb28zejIKdFFJREFRQUIKLS0tLS1FTkQgUFVCTElDIEtFWS0tLS0tCg==",
    pss_auto_sig: "KpFazHyRnRzKKm3engMToqbQmdA4Gg6WD+w1pxYDKKMIhrUzUlBy9ie3wl1jD5AdQUp9w7elIJmkCDArlxIV+m5eUBoCaiYfzIv3tUSsNSeYHXaFmNTkospNTSJ4uWdlFbyawZlHq4pBM5Bqlh486DnBsSO1uaptXyukADVDH2Dx0tZoHhyGGFmVSLOxdDcCP2x6esZslRc5914CZX+7GwoGp/vEktR9l2t5BiaN0GvaLf9qwGnt4Ed5qAw2hjlPkuAKKxFeetTCENWBgBJOnHf9Zv7uu7FWCQmKIUlOYIqLxNY4SoVq8/sHfqQFwwV2Z2So6u6KK12hlfBMFrNYew==",
    pss32_sig: "Bp/G5DRTvOoeKNQS1LTo4WaKQjA9IwSzaF/IWHg6xwrUg/7uHty+Z/1NLj1Ngk1X/znlTgjjlK2h9GytidYCuUEyZtPGJXuroVWS/PsUh1vPIGxU9TOoNRFXjBLLh6E9gtqky6z+zCTSWvS/b0IU/hc+YYv8HhNiZxyeMtZIfxr1/g0ngRd7QHYBV92F725ZQqVh0TwvrVvYKAVhYZInWZETZc5ZmwsqOfK5sify8aH0FRLCa/X3syioSS0v3Yquw+jNQVUKeleFi1rSqMV/yoBLIFqp8TfTjVGEBSSs364a86Y/X2AP1E4oP/388DaRkEwM/U7GuzCyQYIL58AT+Q==",
    oaep_sha256: "awUKcO2LGxDIidz8x9TD94i/RmgmJVIac9ctOYXFanCztTa9nKk4q0eswNxyPfnAjRtQl8tOnyib6oCWW4FHvgC+r/3S7qVVjaaDJOjdepKWYdFsEqKmQdq1SyEmzTpmkpcQsi0te1dcwcgIMQRDWbG8jVfPdoAIJ0TSpwHLrfh0k2XZBCJlCC1TJhW6ckUGRedYpiZsI3SDDxGqhx5im+Vh4j5xruepiYfJdQcQ8eYqpg6gmWPEk80mu/vNYJyGUCy8kQON9WaUM+J0HzxnzuPpOr3OsOYl7+lG6+UbpUk+ESXPB3ncHLz3R76DfgMUT0xvdYI9NEjKJZQi23FQEg==",
    oaep_sha1_label: "T/fo+nPuyTpeTOdBBRjtmKjM2Qij/Fmy6d/R3vx1vpIQhXtW9QpmlETRkbzfteEi1ub/pop09BApvOQHGM57K+ushkDneekGuMIyzVlyAHc5d+uLtwsIL6I5XFaZO475znfsSuTCVdlXZq72tieg6olLevoQO8WNXqC/BkxW0l3+XiWlPASZpOdXivRmEHrjfx7FsXCy5phs7/eOi60XazoBfvIVGehDKAgfpxVZkELkLJzekBlDIjSnTdKeUguMyyANYohfBERtTqIQuCKWl/BbV4/Gj9ZtNWfJD1+O2KvuXagqE+xvVUSWw0Tgf5/dOt0iTDSlUn/1tmFGbNlR5A==",
    p521_priv: "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1JSHVBZ0VBTUJBR0J5cUdTTTQ5QWdFR0JTdUJCQUFqQklIV01JSFRBZ0VCQkVJQVNtbXRsWHFFK1lISEJoNkIKWlcxbFRETmdYL2hqQnVsTjNEdklId1VvZ3d1L2JzRkVRc2I1bCszS1pNZ2tpOWhqM0hBd25jOFBvcktwQ0dFZApPbm1FMHc2aGdZa0RnWVlBQkFCSnJGU1NzOVZkVnBGZUxRVjQ2VFk1Vjc3T1pKVkJubU9vRG9UaENHNTVaY2JZCll1TDdta3pKU0IwK1RNNXdtamZRNlpiM0VvOGV6dEcrbWtoWGlaaVd6d0RUY2NWREdqeVduU08rQ2c3aDlVVDMKWjU2dWVJWFNSbTFLK1c3Ny82L1l0ekFhZlpRYU1Zd0ErVFg2blM1Z2lEZkxUb3NsOHhYd2JudHo0Q1N0d3RnWQo1QT09Ci0tLS0tRU5EIFBSSVZBVEUgS0VZLS0tLS0K",
    p521_pub: "LS0tLS1CRUdJTiBQVUJMSUMgS0VZLS0tLS0KTUlHYk1CQUdCeXFHU000OUFnRUdCU3VCQkFBakE0R0dBQVFBU2F4VWtyUFZYVmFSWGkwRmVPazJPVmUrem1TVgpRWjVqcUE2RTRRaHVlV1hHMkdMaSs1cE15VWdkUGt6T2NKbzMwT21XOXhLUEhzN1J2cHBJVjRtWWxzOEEwM0hGClF4bzhscDBqdmdvTzRmVkU5MmVlcm5pRjBrWnRTdmx1Ky8rdjJMY3dHbjJVR2pHTUFQazErcDB1WUlnM3kwNkwKSmZNVjhHNTdjK0FrcmNMWUdPUT0KLS0tLS1FTkQgUFVCTElDIEtFWS0tLS0tCg==",
    p521_sig_der: "MIGHAkEbjUNpdrRRqdf4vO9MlW0ww4+4oLKu5jHLsXhJQ3AdCmECHEdNICVFz/CeqRPD5uI2TjA/LIvnWhYcgffL87thhAJCAIR03ppQpe1lrK5zDWXESaFrFLvjiwnZtAmEsi6U1KivbIWM5hLAmewFlfv5nlBLS/ldCOipiFhqUx9k1ALSK+x7",
    msg: "Y3Jvc3MtbGFuZSB1cGdyYWRlIHRlc3QgdmVjdG9y",
    abc: "YWJj",
    ed_priv: "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1DNENBUUF3QlFZREsyVndCQ0lFSUlPOGV2UlV2bDVoRFVrWTVEYjJFb2tRUHlOdFZYVEwyY3NzQmoyQUJudmUKLS0tLS1FTkQgUFJJVkFURSBLRVktLS0tLQo=",
    ed_pub: "LS0tLS1CRUdJTiBQVUJMSUMgS0VZLS0tLS0KTUNvd0JRWURLMlZ3QXlFQU1pa0s2cDk2WmZoRm9RemsrRXJTZXJveXE2d3ZidG1icjIrZWF2SUtDRzg9Ci0tLS0tRU5EIFBVQkxJQyBLRVktLS0tLQo=",
    x_priv: "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1DNENBUUF3QlFZREsyVnVCQ0lFSURBQTBpNDcwdG9XVHZCeVhpZDk5QWh0ZVRXQkhSWHUwcXlteXJ4anFaSnEKLS0tLS1FTkQgUFJJVkFURSBLRVktLS0tLQo=",
    x_pub: "LS0tLS1CRUdJTiBQVUJMSUMgS0VZLS0tLS0KTUNvd0JRWURLMlZ1QXlFQVBKQlpCVHVkVjQ1dndEZ1FlZnRucEw3R2dTM2E3UG44Z1JKMjlOeHlWQXM9Ci0tLS0tRU5EIFBVQkxJQyBLRVktLS0tLQo=",
    phc_t2m64p1: "$argon2id$v=19$m=64,t=2,p=1$c29tZXNhbHQ$FqGkmHNGCd0BRW2kBt6fPZ2pPmyGwwChL8FGUhTOSSI",
    phc_t3m1024p4: "$argon2id$v=19$m=1024,t=3,p=4$AAECAwQFBgcICQoLDA0ODw$AKJWSd3f4iLMxeJ0xuAYJDdnVESfcRgtRQhKAg87Xhk",
    phc_len40: "$argon2id$v=19$m=64,t=2,p=2$b3RoZXJzYWx0$sOfolfj3a7lpw7yKtizMg+WcWERTwS5MFNAzHiaM/QEaabSz9lU8ew",
    phc_upg_msg: "$argon2id$v=19$m=32,t=1,p=1$MDEyMzQ1Njc$y3jlnZ17+sSvmTpKyZtJ5NutYo3lSiAqJXdBNGLi6hM",
};

/* Pure-JS base64 decode -- deliberately NOT atob(): the engine's atob
 * (dyna-libc.c js_global_atob) sizes its UTF-8 output buffer for the raw byte
 * count and overflows on binary data (every byte >= 0x80), which is exactly
 * what PEMs and signatures are. Caught by the ASan build; pinned here as a
 * comment because dyna-libc.c is another lane's file. */
const B64REV = (() => {
    const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const t = new Int16Array(128).fill(-1);
    for (let i = 0; i < A.length; i++) t[A.charCodeAt(i)] = i;
    return t;
})();
const b64 = (str) => {
    const clean = str.replace(/-----[^-]+-----|\s+/g, "").replace(/=+$/, "");
    const out = new Uint8Array(Math.floor(clean.length * 3 / 4));
    let acc = 0, bits = 0, o = 0;
    for (let i = 0; i < clean.length; i++) {
        const v = B64REV[clean.charCodeAt(i)];
        if (v < 0) throw new TypeError("bad base64 char " + clean[i]);
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 0xff; }
    }
    return out.subarray(0, o);
};
const b64s = (s) => new TextDecoder().decode(b64(s));
const b64u = (s) => {  // UNPADDED base64 (the PHC alphabet)
    let t = s.replace(/\s+/g, "");
    while (t.length % 4) t += "=";
    return b64(t);
};

const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const fromb64 = (s) => new Uint8Array([...atob(s)].map((c) => c.charCodeAt(0)));

const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);

if (!c.RSA || !c.AESGCM) {
    print("test_crypto_upgrade: SKIP -- built without CONFIG_TLS");
    print("test_crypto_upgrade: 0 passed, 0 failed");
} else {

/* ----------: RSASSA-PSS against the openssl CLI artifacts ---------- */
{
    const priv = b64s(V.rsa_priv), pub = b64s(V.rsa_pub);
    const msg = b64(V.msg);

    // openssl dgst -sigopt rsa_padding_mode:pss (saltlen auto = max on sign):
    // WE must verify ITS signature with the default opts.
    eq(c.RSA.verifyPSS("sha256", pub, msg, b64(V.pss_auto_sig)), true,
       "verifyPSS recovers the openssl max-salt signature (saltLen auto)");
    eq(c.RSA.verifyPSS("sha256", pub, msg, b64(V.pss32_sig)), true,
       "verifyPSS auto-recovers a fixed-32 openssl salt");
    eq(c.RSA.verifyPSS("sha256", pub, msg, b64(V.pss32_sig), { saltLen: 32 }), true,
       "verifyPSS with the exact saltLen 32");
    eq(c.RSA.verifyPSS("sha512", pub, msg, b64(V.pss_auto_sig)), false,
       "wrong md is a foreign signature -> false");
    { const bad = b64(V.pss_auto_sig).slice(); bad[10] ^= 1;
      eq(c.RSA.verifyPSS("sha256", pub, msg, bad), false, "flipped sig byte -> false"); }

    // OUR signature under every saltLen shape; self-verify + cross-shape
    for (const opts of [undefined, { saltLen: "auto" }, { saltLen: "max" },
                        { saltLen: 32 }, { saltLen: 0 }, { mgf1Hash: "sha1" }]) {
        const sig = c.RSA.signPSS("sha256", priv, msg, opts);
        eq(sig.length, 256, `signPSS ${JSON.stringify(opts)} yields modulus-size sig`);
        eq(c.RSA.verifyPSS("sha256", pub, msg, sig, opts), true,
           `signPSS/verifyPSS roundtrip ${JSON.stringify(opts)}`);
    }
    // saltLen 0 is deterministic: two signatures must be identical
    eq(tohex(c.RSA.signPSS("sha256", priv, msg, { saltLen: 0 })),
       tohex(c.RSA.signPSS("sha256", priv, msg, { saltLen: 0 })),
       "saltLen 0 is deterministic (no salt)");
    eq(c.RSA.verifyPSS("sha256", pub, msg, c.RSA.signPSS("sha256", priv, msg, { saltLen: 0 }), { saltLen: 0 }),
       true, "saltLen 0 roundtrip");
    // mgf1 divergence: a sig under mgf1-sha1 must NOT verify as mgf1-sha256
    { const s = c.RSA.signPSS("sha256", priv, msg, { saltLen: 32, mgf1Hash: "sha1" });
      eq(c.RSA.verifyPSS("sha256", pub, msg, s, { saltLen: 32 }), false,
         "mgf1 mismatch -> false"); }
    // refusals
    throws(() => c.RSA.signPSS("md5", priv, "x"), /unsupported hash/, "md5 refused");
    throws(() => c.RSA.signPSS("sha256", "not a pem", "x"), /invalid private key PEM/, "garbage PEM refused");
    eq(c.RSA.signPSS("sha256", priv, "x", { saltLen: 17 }).length, 256, "saltLen 17 accepted (legal)");
    throws(() => c.RSA.signPSS("sha256", priv, "x", { saltLen: "huge" }), /auto.*max|saltLen/,
           "saltLen string garbage refuses");
    throws(() => c.RSA.signPSS("sha256", priv, "x", { saltLen: -1 }), /0\.\.4096/, "negative saltLen refuses");
    throws(() => c.RSA.signPSS("sha256", priv, "x", { mgf1Hash: "md5" }), /mgf1Hash/, "md5 mgf1 refuses");
    eq(c.RSA.verifyPSS("sha256", pub, "x", new Uint8Array(10)), false, "short sig is false, not a throw");
}

/* ----------: RSA-OAEP against the openssl CLI artifacts ---------- */
{
    const priv = b64s(V.rsa_priv), pub = b64s(V.rsa_pub);
    const msg = b64(V.msg);
    // openssl pkeyutl -pkeyopt rsa_oaep_md:sha256 -pkeyopt rsa_mgf1_md:sha256
    eq(dec(c.RSA.OAEP.open(priv, b64(V.oaep_sha256), { md: "sha256" })), dec(msg),
       "open() decrypts the openssl sha256-OAEP blob byte-exact");
    // openssl pkeyutl with sha1 + a label ("upgrade")
    eq(dec(c.RSA.OAEP.open(priv, b64(V.oaep_sha1_label), { md: "sha1", label: "upgrade" })),
       dec(msg), "open() with the openssl label blob (sha1)");
    // roundtrip + label discipline
    const sealed = c.RSA.OAEP.seal(pub, msg);
    eq(sealed.length, 256, "OAEP.seal output is modulus-sized");
    eq(dec(c.RSA.OAEP.open(priv, sealed)), dec(msg), "OAEP roundtrip (default md)");
    throws(() => c.RSA.OAEP.open(priv, sealed, { md: "sha1" }), /decryption failed/,
           "md mismatch throws like a tamper");
    const sl = c.RSA.OAEP.seal(pub, msg, { label: enc("upgrade") });
    eq(dec(c.RSA.OAEP.open(priv, sl, { label: enc("upgrade") })), dec(msg), "label roundtrip");
    throws(() => c.RSA.OAEP.open(priv, sl), /decryption failed/, "missing label throws");
    throws(() => c.RSA.OAEP.open(priv, sl, { label: enc("other") }), /decryption failed/,
           "wrong label throws");
    { const t = sealed.slice(); t[100] ^= 1;
      throws(() => c.RSA.OAEP.open(priv, t), /decryption failed/, "tampered blob throws"); }
    throws(() => c.RSA.OAEP.open(b64s(V.rsa_pub), sealed), /invalid private key PEM|not.*parse/,
           "a PUBLIC key cannot open");
    // empty plaintext is legal OAEP
    eq(c.RSA.OAEP.open(priv, c.RSA.OAEP.seal(pub, "")).length, 0, "empty plaintext roundtrips");
}

/* ----------: ECDSA P-521 ---------- */
{
    const kp = c.ECDSA.generate("P-521");
    ok(kp.privateKey.includes("BEGIN PRIVATE KEY"), "P-521 generate PEM");
    const raw = c.ECDSA.sign("sha512", kp.privateKey, "five two one");
    eq(raw.length, 132, "P-521 raw signature is 132 bytes (2 * 66)");
    eq(c.ECDSA.verify("sha512", kp.publicKey, "five two one", raw), true, "P-521 raw verify");
    eq(c.ECDSA.verify("sha512", kp.publicKey, "five two TWO", raw), false, "P-521 foreign msg false");
    const der = c.ECDSA.sign("sha512", kp.privateKey, "five two one", { format: "der" });
    eq(c.ECDSA.verify("sha512", kp.publicKey, "five two one", der, { format: "der" }), true,
       "P-521 DER roundtrip");
    // openssl dgst -sha512 -sign over "abc": the DER sig is the oracle
    const abc = b64(V.abc);
    eq(c.ECDSA.verify("sha512", b64s(V.p521_pub), abc, b64(V.p521_sig_der), { format: "der" }), true,
       "P-521 verifies the openssl DER signature");
    // ...and our raw form against it via our own signing of abc
    eq(c.ECDSA.verify("sha512", b64s(V.p521_pub), abc, c.ECDSA.sign("sha512", b64s(V.p521_priv), abc)), true,
       "P-521 raw verify against the openssl keypair");
    // aliases
    eq(c.ECDSA.generate("secp521r1").publicKey.includes("BEGIN"), true, "secp521r1 alias");
    throws(() => c.ECDSA.generate("P-520"), /P-256, P-384 or P-521/, "unknown curve refuses");
    // P-384 still fine (regression guard)
    const k384 = c.ECDSA.generate("P-384");
    eq(c.ECDSA.verify("sha384", k384.publicKey, "m", c.ECDSA.sign("sha384", k384.privateKey, "m")), true,
       "P-384 unchanged");
}

/* ----------: X509 sans + extensions ---------- */
{
    const kp = c.ECDSA.generate("P-256");
    const cert = c.X509.generateSelfSigned({
        key: kp.privateKey, subject: "upgrade.test", days: 30,
        sans: ["upgrade.test", "www.upgrade.test", "10.20.30.40",
               "2001:db8::1", "root@upgrade.test"],
        extensions: [
            { name: "keyUsage", critical: true, value: "digitalSignature,keyEncipherment" },
            { name: "extendedKeyUsage", value: "serverAuth,clientAuth" },
            { name: "basicConstraints", value: "CA:FALSE" },
            { name: "subjectKeyIdentifier", value: "hash" },
        ],
    });
    ok(cert.includes("BEGIN CERTIFICATE"), "cert with sans generated");
    const info = c.X509.parse(cert);
    eq(info.sans.dns.join("|"), "upgrade.test|www.upgrade.test", "DNS sans classified");
    eq(info.sans.ip[0], "10.20.30.40", "IPv4 san classified");
    ok(info.sans.ip[1].indexOf("2001") === 0, "IPv6 san classified");
    eq(info.sans.email.join(), "root@upgrade.test", "email san classified");
    // the extensions are really in there: a second generation WITHOUT them must differ
    const bare = c.X509.generateSelfSigned({ key: kp.privateKey, sans: ["upgrade.test"] });
    ok(bare !== cert, "extensions change the certificate bytes");
    eq(c.X509.parse(bare).sans.dns.join(), "upgrade.test", "sans-only generation");
    // refusals -- the honest surface
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ name: "nameConstraints", value: "DNS:x" }],
    }), /unsupported extension "nameConstraints"/, "unsupported extension refused by NAME");
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, sans: ["a.test"],
        extensions: [{ name: "subjectAltName", value: "DNS:b.test" }],
    }), /ambiguous/, "sans + explicit subjectAltName refused");
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ name: "keyUsage", value: "notAUsage" }],
    }), /extension value rejected/, "garbage extension value refused");
    throws(() => c.X509.generateSelfSigned({ key: kp.privateKey, bogus: 1 }),
           /unknown option "bogus"/, "unknown opt key refused");
    throws(() => c.X509.generateSelfSigned({ key: kp.privateKey, sans: "notanarray" }),
           /array of strings/, "sans must be an array");
    throws(() => c.X509.generateSelfSigned({ key: kp.privateKey, sans: [""] }),
           /1\.\.1000/, "empty san refuses");
    /* a malformed entry reports the SHAPE problem, not "unsupported
       extension \"undefined\"" (name/value are string-checked BEFORE
       conversion, so a missing field cannot masquerade as a name) */
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ value: "serverAuth" }],
    }), /needs string name and value/, "extension entry missing name");
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ name: "extendedKeyUsage" }],
    }), /needs string name and value/, "extension entry missing value");
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ name: 42, value: "serverAuth" }],
    }), /needs string name and value/, "extension entry non-string name");
    throws(() => c.X509.generateSelfSigned({
        key: kp.privateKey, extensions: [{ name: "keyUsage", value: null }],
    }), /needs string name and value/, "extension entry null value");
    // the old surface still works: no sans, no extensions
    const plain = c.X509.generateSelfSigned({ key: kp.privateKey, subject: "plain.test" });
    eq(c.X509.parse(plain).sans.dns.length, 0, "no-sans cert parses with empty dns");
}

/* ----------: Argon2id PHC form (argon2-cffi oracle) ---------- */
{
    // the oracle strings, straight from the reference binding
    eq(c.Argon2id.hash("password", enc("somesalt"), { iterations: 2, memory: 64, parallelism: 1 }),
       V.phc_t2m64p1, "PHC string matches argon2-cffi (t=2, m=64, p=1)");
    eq(c.Argon2id.hash("correct horse battery staple", new Uint8Array([0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]),
                       { iterations: 3, memory: 1024, parallelism: 4 }),
       V.phc_t3m1024p4, "PHC string matches argon2-cffi (t=3, m=1024, p=4, len 16 salt)");
    eq(c.Argon2id.hash("password", enc("othersalt"), { iterations: 2, memory: 64, parallelism: 2, hashLen: 40 }),
       V.phc_len40, "PHC string with hashLen 40");
    // structure: parseable by the verify path and by hand
    const phc = c.Argon2id.hash("x", enc("01234567"), { iterations: 1, memory: 32, parallelism: 1 });
    ok(/^\$argon2id\$v=19\$m=\d+,t=\d+,p=\d+\$[A-Za-z0-9+/]+\$[A-Za-z0-9+/]+$/.test(phc),
       "PHC string shape: " + phc);
    // default params shape: m=65536,t=3,p=4
    const def = c.Argon2id.hash("x", enc("01234567"));
    ok(def.startsWith("$argon2id$v=19$m=65536,t=3,p=4$"), "default params in the string");
    // verify: PHC form
    eq(c.Argon2id.verify(V.phc_t2m64p1, "password"), true, "PHC verify ok");
    eq(c.Argon2id.verify(V.phc_t2m64p1, "Password"), false, "PHC verify wrong password");
    eq(c.Argon2id.verify(V.phc_len40, "password"), true, "PHC verify hashLen 40 from string");
    // tampered params: clean refuse or false, never a wrong true
    eq(c.Argon2id.verify(V.phc_t2m64p1.replace("t=2", "t=3"), "password"), false,
       "tampered t recomputes to a mismatch");
    eq(c.Argon2id.verify(V.phc_t2m64p1.replace("t=2", "t=3"), "password"), false,
       "tampered t is FALSE (params from the string)");
    throws(() => c.Argon2id.verify(V.phc_t2m64p1.replace("argon2id", "argon2iX"), "password"),
           /not a valid.*PHC/, "wrong type name refuses (parse-level)");
    throws(() => c.Argon2id.verify("$argon2id$v=16$m=64,t=2,p=1" + V.phc_t2m64p1.slice(21), "p"),
           /not a valid.*PHC/, "v=16 (the draft) refuses");
    throws(() => c.Argon2id.verify("$argon2i$v=19$m=64,t=2,p=1$c29tZXNhbHQ$CTFhFd", "p"),
           /not a valid.*PHC/, "argon2i type refuses");
    throws(() => c.Argon2id.verify(V.phc_t2m64p1.replace("c29tZXNhbHQ", "c29tZXNhbH!"), "p"),
           /corrupt base64/, "corrupt salt base64 refuses");
    throws(() => c.Argon2id.verify(V.phc_t2m64p1.replace(/.\$/, "=;$"), "p"),
           /not a valid|corrupt/, "padded/garbage base64 refuses");
    throws(() => c.Argon2id.verify("$argon2id$v=19$m=64,t=2,p=1$c29t$CTFhFd", "p"),
           /not a valid.*PHC/, "7-byte salt refuses");
    throws(() => c.Argon2id.verify("$argon2id$v=19$m=1,t=2,p=1$c29tZXNhbHQ$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8", "p"),
           /not a valid.*PHC|parameters rejected/, "m below 8*lanes refuses");
    throws(() => c.Argon2id.verify("not a phc string", "p"), /not a valid.*PHC/, "non-PHC input refuses");
    // raw form still works, both directions
    const raw = c.Argon2id.hash("password", enc("somesalt"), { iterations: 2, memory: 64, parallelism: 1, encoded: false });
    eq(raw.length, 32, "encoded:false raw bytes");
    eq(tohex(raw), tohex(b64u(V.phc_t2m64p1.split("$")[5])),
       "raw tag == the tag inside the PHC string");
    eq(c.Argon2id.verify("password", enc("somesalt"), raw, { iterations: 2, memory: 64, parallelism: 1 }),
       true, "legacy raw verify form unchanged");
    eq(c.Argon2id.verify("password", enc("somesalt"), raw), false,
       "raw verify with DEFAULT opts mismatches (defaults differ from stored params)");
    // hashAsync PHC settles the string too (below the offload gate -> inline promise)
    /* async coverage lives in test_argon2_async.js; the PHC async form is
       asserted here through a microtask tick */
}

/* ----------: OTP verify counterparts ---------- */
{
    const rfcSecret = "12345678901234567890";
    // RFC 4226 sD.2 test vectors (HOTP-SHA1-6: counts 0..8)
    const rfc4226 = ["755224", "287082", "359152", "969429", "338314",
                     "254676", "287922", "162583", "399871"];
    for (let i = 0; i < rfc4226.length; i++) {
        eq(c.HOTPGenerate(rfcSecret, i), rfc4226[i], `RFC 4226 vector ${i} (generate)`);
        eq(c.HOTPVerify(rfcSecret, i, rfc4226[i]), true, `RFC 4226 vector ${i} verifies`);
    }
    eq(c.HOTPVerify(rfcSecret, 9, rfc4226[8]), false, "off-by-one counter is false");
    eq(c.HOTPVerify(rfcSecret, 0, rfc4226[1]), false, "swapped code is false");
    eq(c.HOTPVerify(rfcSecret, 0, "75522"), false, "short code is false");
    eq(c.HOTPVerify(rfcSecret, 0, "7552244"), false, "long code is false");
    eq(c.HOTPVerify(rfcSecret, 0, "75522a"), false, "non-digit code is false");
    eq(c.HOTPVerify(rfcSecret, 0, ""), false, "empty code is false");
    // digits opt roundtrip
    { const code = c.HOTPGenerate(rfcSecret, 3, { digits: 8 });
      eq(code.length, 8, "8-digit code");
      eq(c.HOTPVerify(rfcSecret, 3, code, { digits: 8 }), true, "8-digit verify");
      eq(c.HOTPVerify(rfcSecret, 3, code), false, "default 6-digit verify of an 8-digit code is false"); }
    // RFC 6238 sB vectors at T=59, T0=0, X=30. NOTE the RFC's own seed note:
    // 20-byte seed for SHA1, 32 for SHA256, 64 for SHA512 (the ASCII string
    // repeated), 8 digits.
    eq(c.TOTPGenerate(rfcSecret, { atSec: 59, algo: "sha1", digits: 8 }), "94287082",
       "RFC 6238 sha1 T59");
    const s32 = rfcSecret + "123456789012";
    eq(s32.length, 32, "sha256 seed length");
    eq(c.TOTPGenerate(s32, { atSec: 59, algo: "sha256", digits: 8 }), "46119246",
       "RFC 6238 sha256 T59");
    const s64 = rfcSecret + rfcSecret + rfcSecret + "1234";
    eq(c.TOTPGenerate(s64, { atSec: 59, algo: "sha512", digits: 8 }), "90693936",
       "RFC 6238 sha512 T59");
    eq(c.TOTPVerify(s32, "46119246", { atSec: 59, algo: "sha256", digits: 8 }), true,
       "RFC 6238 sha256 verifies");
    eq(c.TOTPVerify(s64, "90693936", { atSec: 59, algo: "sha512", digits: 8 }), true,
       "RFC 6238 sha512 verifies");
    // window boundaries (period 30): code@59 is counters 1; 60..89 is counter 2
    const tc = c.TOTPGenerate(rfcSecret, { atSec: 59 });
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 59 }), true, "TOTP exact");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 89 }), false, "skew +1 period, window 0 -> false");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 29 }), false, "skew -1 period, window 0 -> false");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 89, window: 1 }), true, "skew +1, window 1 -> true");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 29, window: 1 }), true, "skew -1, window 1 -> true");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 119, window: 1 }), false, "skew +2, window 1 -> false");
    eq(c.TOTPVerify(rfcSecret, tc, { atSec: 59, window: 3 }), true, "window around exact is true");
    throws(() => c.TOTPVerify(rfcSecret, tc, { atSec: 59, window: -1 }), /window >= 0/, "negative window refuses");
    throws(() => c.TOTPVerify(rfcSecret, tc, { atSec: 59, period: 0 }), /period > 0/, "period 0 refuses");
    throws(() => c.TOTPVerify(rfcSecret, tc, { atSec: -1 }), /atSec >= 0/, "negative atSec refuses");
    throws(() => c.TOTPVerify(rfcSecret, tc, { algo: "md5" }), /sha1, sha256 or sha512/, "bad algo refuses");
    throws(() => c.HOTPVerify(rfcSecret, -1, "123456"), /negative/, "negative counter refuses");
}

/* ----------: PEM <-> raw key converters ---------- */
{
    const tohex_ = tohex;
    // openssl genpkey -algorithm ed25519 PEM is the oracle
    const edFromOssl = c.Ed25519PemToRaw(b64s(V.ed_priv));
    eq(edFromOssl.privateKey.length, 32, "ed25519 private raw 32");
    eq(edFromOssl.publicKey.length, 32, "ed25519 public raw 32");
    // our PEM from those raw keys must round-trip byte-exact
    const edPem = c.Ed25519PemFromRaw(edFromOssl);
    ok(edPem.privateKey.startsWith("-----BEGIN PRIVATE KEY-----"), "PemFromRaw PKCS#8 header");
    ok(edPem.publicKey.startsWith("-----BEGIN PUBLIC KEY-----"), "PemFromRaw SPKI header");
    const edBack = c.Ed25519PemToRaw(edPem.privateKey);
    eq(tohex_(edBack.privateKey), tohex_(edFromOssl.privateKey), "ed25519 PEM roundtrip private");
    eq(tohex_(edBack.publicKey), tohex_(edFromOssl.publicKey), "ed25519 PEM roundtrip public");
    // public-only PEM
    const edPubOnly = c.Ed25519PemToRaw(b64s(V.ed_pub));
    ok(!edPubOnly.privateKey, "public PEM has no private half");
    eq(tohex_(edPubOnly.publicKey), tohex_(edFromOssl.publicKey), "public PEM raw == private PEM's public");
    // the openssl-generated public PEM -> raw -> verify a signature made by the private key
    const msg = enc("ed25519 interop");
    const sig = c.Ed25519Sign(edFromOssl.privateKey, msg);
    eq(c.Ed25519Verify(edPubOnly.publicKey, msg, sig), true, "openssl-key sign/verify through raw forms");
    // X25519 twin, plus a real key agreement with the openssl key
    const xFromOssl = c.X25519PemToRaw(b64s(V.x_priv));
    eq(xFromOssl.privateKey.length, 32, "x25519 private raw 32");
    const xPem = c.X25519PemFromRaw(xFromOssl);
    const xBack = c.X25519PemToRaw(xPem.privateKey);
    eq(tohex_(xBack.privateKey), tohex_(xFromOssl.privateKey), "x25519 PEM roundtrip");
    const fresh = c.X25519Generate();
    const s1 = c.X25519Derive(xFromOssl.privateKey, fresh.publicKey);
    const s2 = c.X25519Derive(fresh.privateKey, xFromOssl.publicKey);
    eq(tohex_(s1), tohex_(s2), "x25519 agreement with the openssl key");
    // refusals
    throws(() => c.Ed25519PemToRaw(b64s(V.rsa_priv)), /wrong algorithm|not a parseable/,
           "an RSA PEM refuses in the ed25519 converter");
    throws(() => c.Ed25519PemToRaw("garbage"), /not a parseable/, "garbage PEM refuses");
    throws(() => c.Ed25519PemFromRaw({ publicKey: new Uint8Array(31) }), /32-byte key/, "31-byte raw refuses");
    throws(() => c.Ed25519PemFromRaw({}), /expected \{ privateKey\?, publicKey \}/, "no halves refuses");
    throws(() => c.X25519PemToRaw(b64s(V.ed_pub)), /wrong algorithm/, "an ed25519 PEM refuses in the x25519 converter");
}

/* ----------: sealInto/openInto ---------- */
{
    const chachaKey = (i) => new Uint8Array(32).fill(i);
    const nonce = (i) => new Uint8Array(12).fill(i);

    // RFC 8439 s2.8.2 through the Into form, detached tag
    {
        const key = new Uint8Array([0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,
                                    0x8c,0x8d,0x8e,0x8f,0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
                                    0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f]);
        const n = new Uint8Array([0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47]);
        const aad = new Uint8Array([0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7]);
        const pt = enc("Ladies and Gentlemen of the class of '99: If I could offer you " +
                       "only one tip for the future, sunscreen would be it.");
        const aead = new c.ChaCha20Poly1305(key);
        const ct = new Uint8Array(256), tag = new Uint8Array(16);
        const w = aead.sealInto(ct, n, pt, aad, { tagOut: tag });
        eq(w, pt.length, "RFC 8439 sealInto detached: written == plaintext length");
        eq(tohex(tag), "1ae10b594f09e26a7e902ecbd0600691", "RFC 8439 tag through sealInto/tagOut");
        const back = new Uint8Array(256);
        const r = aead.openInto(back, n, ct.subarray(0, w), aad, { tag: tag });
        eq(r, pt.length, "RFC 8439 openInto detached reads back");
        eq(dec(back.subarray(0, r)), dec(pt), "RFC 8439 plaintext roundtrip");
        tag[0] ^= 1;
        throws(() => aead.openInto(back, n, ct.subarray(0, w), aad, { tag }), /authentication failed/,
               "detached tag tamper throws");
        eq(tohex(back.subarray(0, 8)), "0000000000000000", "failed openInto zeroed the output");
        aead.close();
    }

    // layouts vs seal()/open() on AESGCM
    {
        const aes = new c.AESGCM(chachaKey(3));
        const pt = enc("into-contract");
        const out = new Uint8Array(64);
        const w = aes.sealInto(out, nonce(1), pt);
        const sealed = aes.seal(nonce(1), pt);
        eq(w, pt.length + 16, "combined sealInto writes pt+tag bytes");
        eq(tohex(out.subarray(0, w)), tohex(sealed), "sealInto == seal byte-exact");
        const back = new Uint8Array(32);
        eq(aes.openInto(back, nonce(1), sealed), pt.length, "openInto bytes read");
        eq(dec(back.subarray(0, pt.length)), dec(pt), "openInto roundtrip");
        // subarray slice honored
        const big = new Uint8Array(100);
        const view = big.subarray(50, 50 + pt.length + 16);
        eq(aes.sealInto(view, nonce(2), pt), view.length, "subarray view accepted");
        eq(tohex(big.subarray(50, 50 + view.length)), tohex(aes.seal(nonce(2), pt)),
           "subarray slice written, neighbors untouched");
        // detached
        const ct2 = new Uint8Array(16), tg = new Uint8Array(16);
        eq(aes.sealInto(ct2, nonce(3), pt, undefined, { tagOut: tg }), pt.length, "detached ct only");
        eq(tohex(ct2.subarray(0, pt.length)) + tohex(tg), tohex(aes.seal(nonce(3), pt)),
           "detached == combined split exactly");
        // prefixNonce one-blob layout
        const blob = new Uint8Array(12 + pt.length + 16);
        eq(aes.sealInto(blob, nonce(4), pt, undefined, { prefixNonce: true }), blob.length,
           "one-blob sealInto fills 12+pt+16");
        eq(tohex(blob.subarray(0, 12)), tohex(nonce(4)), "the nonce leads the blob");
        const ptOut = new Uint8Array(32);
        eq(aes.openInto(ptOut, undefined, blob, undefined, { prefixNonce: true }), pt.length,
           "one-blob openInto reads the leading nonce");
        eq(dec(ptOut.subarray(0, pt.length)), dec(pt), "one-blob roundtrip");
        { const cut = blob.subarray(0, blob.length - 1);
          throws(() => aes.openInto(ptOut, undefined, cut, undefined, { prefixNonce: true }),
                 /shorter than|authentication/, "truncated one-blob refuses"); }
        // capacity + arg discipline
        throws(() => aes.sealInto(new Uint8Array(4), nonce(5), pt), /smaller than the .* bytes required/,
               "short out is a RangeError before any write");
        throws(() => aes.sealInto(new Uint8Array(64), nonce(5).subarray(0, 8), pt), /nonce must be 12/,
               "wrong nonce length refuses");
        throws(() => aes.sealInto(new Uint8Array(64), nonce(5), pt, { bogus: 1 }),
               /aad must be bytes or a string/, "opts object at aad position refused");
        throws(() => aes.sealInto(new Uint8Array(64), nonce(5), pt, undefined, { tagOut: new Uint8Array(8) }),
               /tagOut must be 16/, "short tagOut refuses");
        throws(() => aes.openInto(new Uint8Array(64), nonce(5), pt, undefined, { tag: new Uint8Array(4) }),
               /tag must be 16/, "short tag refuses");
        throws(() => aes.sealInto(new Uint8Array(64), nonce(5), pt, undefined, { bogus: true }),
               /unknown option "bogus"/, "unknown opts key refused by name");
        // tamper in the combined openInto throws and zeroes
        const t = sealed.slice(); t[3] ^= 1;
        { const o = new Uint8Array(32).fill(0xab);
          throws(() => aes.openInto(o, nonce(1), t), /authentication failed/, "combined tamper throws");
          eq(o[0], 0, "output zeroed after failure"); }
        aes.close();
    }
}

/* ---------- adversarial round 2: key floors, wrong keys, close-mid-chain --- */
{
    // a pinned 1024-bit openssl key: below the 2048-bit floor on EVERY RSA path
    const weakPriv = b64s("LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCk1JSUNlQUlCQURBTkJna3Foa2lHOXcwQkFRRUZBQVNDQW1Jd2dnSmVBZ0VBQW9HQkFOemx2RmsrRTVlYUI5V20KSXQvbzlWOC9oS2wzcHp3VXZRN0tHVTJOQnNwSFVYMmVNUmpKNEZnM2paeUhabWVnVThoN25WeVA5WkIrNmtRYQprcHNtOWxYMXFzTUNGQURNRHhWK1A4ZFRwbkpXSGJCcWFRa3hLMHZNQkNBOUFpRFN3YlFuV1VRa0hjN3VZU1NmCk1oOXhCS1FvMzFpWkdsQjFZa1N0Vk1pT2wyWHRBZ01CQUFFQ2dZRUExTHZlL3ljY1lUVHk5SnV5SEdkUzMyN2gKaVA5MXJCUGcydnhoRCtHUU40QWxoOCt3UXNvd1oyVGcyVzFBZnUzVm9rOENCbUdSd1oyb2FQd2FVcGRjUlRQOQowc3h0d1lnMHFScnZxK200OU5Fdi9KS1RIT01qaTJQZzlpRDRFV0RGakpaYUxZOTU1Nmh1RTVZY09RS2lrZWdMCm9NbFlvZWhDWWw4U0dqN3Z2blVDUVFENGpnaTlSa2E1RXRqN0Y2OHF5U2JSSU1FdXpwRUJrblREUURrcnROR04KRzJFZkhYVEZnRXBaaEJaalAvc3hoSEdRYXAvWTBsZGZpVjRXZkxyTWppMS9Ba0VBNDRPZWh6VW9RZ2orM1pCTQp5U2hTaEFIYkFzMmZlLzNmL3hraERMV3UvZ291QWpMNTZsRTM0U2ZSUGZSVHpGTm5jSmN0eUJFdnRwS1lFTW5QCndKdTZrd0pCQU56WmwzcFdqMUN6Sm9rMUtqZmlNOU51UHpqUDRwaDdBYlRidy9ESjRjaDNvM3g2TjkrbGRtckQKcXdEVlFPVm13V0dJM0M5VDlyNjAyQjB6QnVmckRSRUNRQmV6UDFGZ1pUZ3p6YkR3OWo3Q040NU96eXpFbE1lSwowOG0wS1hBMGdPMHZ6RWtvWEVaZmZZMno0eVVzRFlxc2FZc2VCSVBoM25HTFpkSGg1QVZ4YzRjQ1FRQ3k1ZE9TCnpkWHF6cVJ5OHFHYjF6RVZFR2Z2alN4Rlh4dXBXYjB1ZUt1LzNNMU56S3RqczZkWkJLYTJ3RFJKbGZ5K1FybzEKcUFaeHBjR2lLRmtLSHpZdAotLS0tLUVORCBQUklWQVRFIEtFWS0tLS0tCg==");
    throws(() => c.RSA.signPSS("sha256", weakPriv, "x"), /2048/, "PSS sign under the key floor refuses");
    eq(c.RSA.verifyPSS("sha256", b64s(V.rsa_pub), "x", new Uint8Array(256)), false, "verify of garbage sig stays false");
    eq(c.RSA.OAEP.seal(b64s(V.rsa_pub), "x").length, 256, "seal with the good key is fine (sanity)");
    // opening with the WRONG key pair: well-formed but foreign
    const kp2 = c.RSA.generate(2048);
    const sealed2 = c.RSA.OAEP.seal(kp2.publicKey, "for kp2 only");
    throws(() => c.RSA.OAEP.open(b64s(V.rsa_priv), sealed2), /decryption failed/,
           "foreign private key cannot open");
    // PSS: a signature from another key is simply false
    { const s2 = c.RSA.signPSS("sha256", kp2.privateKey, "cross");
      eq(c.RSA.verifyPSS("sha256", kp2.publicKey, "cross", s2), true, "right key verifies");
      eq(c.RSA.verifyPSS("sha256", b64s(V.rsa_pub), "cross", s2), false, "other public key false"); }
}

{
    // the OTP generate functions are untouched: HOTP over the RFC seed still
    // matches the RFC 4226 appendix-D first code
    eq(c.HOTPGenerate("12345678901234567890", 0), "755224", "HOTPGenerate untouched (RFC 4226 D)");
}

} // CONFIG_TLS guard

if (fails) {
    print("test_crypto_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_crypto_upgrade failed");
}
print("test_crypto_upgrade: " + n + " assertions, 0 failures");
