// covers: dyna:crypto.PBKDF2
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_pbkdf2_hash_split");
import * as hash from "dyna:hash";
import * as crypto from "dyna:crypto";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

await __kitS.section("RFC 6070 c", async () => {
    const cases = [
        ["password", "salt", 1, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6"],
        ["password", "salt", 2, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"],
        ["password", "salt", 4096, 20, "4b007901b765489abead49d926f721d065a429c1"],
        ["passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
         25, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"],
    ];
    for (const [pw, salt, iterations, length, want] of cases) {
        const dk = await crypto.PBKDF2({ hash: "sha1", password: pw, salt, iterations, length });
        eq(hex(dk), want, "RFC 6070 c=" + iterations + " dkLen=" + length);
    }
    eq(hex(await crypto.PBKDF2({ hash: "sha1",
                           password: new Uint8Array([0x70, 0x61, 0x73, 0x73, 0, 0x77, 0x6f, 0x72, 0x64]),
                           salt: new Uint8Array([0x73, 0x61, 0, 0x6c, 0x74]),
                           iterations: 4096, length: 16 })),
       "56fa6aa75548099dcc37d7f03425e0c3",
       "RFC 6070 embedded NUL (a string API would truncate here)");
    eq(hex(await crypto.PBKDF2({ hash: "sha256", password: "passwd", salt: "salt",
                           iterations: 1, length: 64 })),
       "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc" +
       "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
       "RFC 7914 PBKDF2-HMAC-SHA256");
    await throws(() => crypto.PBKDF2({ hash: "sha1", password: "p", salt: "s",
                                 iterations: 0, length: 20 }), "0 iterations");
    await throws(() => crypto.PBKDF2({ hash: "sha1", salt: "s", iterations: 1,
                                 length: 20 }), "PBKDF2 without a password");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
