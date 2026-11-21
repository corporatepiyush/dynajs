// covers: dyna:crypto.X509
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_x509_upgrade");
import * as c from "dyna:crypto";

let n = 0, fails = 0;
function ok(cnd, msg) { n++; if (!cnd) { fails++; print("  FAIL " + msg); } }
function eq(a, b, msg) { ok(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, re, msg) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e && e.message || e); }
    ok(t && (!re || re.test(m)), msg + (t && re && !re.test(m) ? " [wrong error: " + m + "]" : ""));
}
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
const b64u = (s) => {
    let t = s.replace(/\s+/g, "");
    while (t.length % 4) t += "=";
    return b64(t);
};

const tohex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const fromb64 = (s) => new Uint8Array([...atob(s)].map((c) => c.charCodeAt(0)));

const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);

__kitS.section("a hostname subject becomes the DNS san", () => {
    const kp = c.ECDSA.generate("P-256");
    const sansOf = (opts) => c.X509.parse(c.X509.generateSelfSigned(Object.assign({ key: kp.privateKey }, opts))).sans;
    eq(sansOf({ subject: "svc.local" }).dns.join(), "svc.local", "a hostname subject becomes the DNS san");
    eq(sansOf({}).dns.join(), "localhost", "the default subject localhost becomes the DNS san");
    eq(sansOf({ subject: "192.0.2.7" }).ip.join(), "192.0.2.7", "an IPv4 subject becomes an IP san");
    eq(sansOf({ subject: "*.svc.local" }).dns.join(), "*.svc.local", "a wildcard subject becomes the DNS san");
    eq(sansOf({ subject: "My Org" }).dns.length + sansOf({ subject: "My Org" }).ip.length, 0, "a subject that is no host name gets no san");
    eq(sansOf({ subject: "svc.local", sans: [] }).dns.length, 0, "an explicit empty sans means none");
    eq(sansOf({ subject: "svc.local", sans: ["other.local"] }).dns.join(), "other.local", "explicit sans replace the default");
    eq(sansOf({ subject: "svc.local", extensions: [{ name: "subjectAltName", value: "DNS:ext.local" }] }).dns.join(), "ext.local",
       "a subjectAltName extension is not doubled by the default");
});
__kitS.end();

if (fails) {
    print("test_crypto_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_crypto_upgrade failed");
}
print("test_crypto_upgrade: " + n + " assertions, 0 failures");
