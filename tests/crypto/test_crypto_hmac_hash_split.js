// covers: dyna:crypto.Hmac
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac_hash_split");
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
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

__kitS.section("The quick brown fox jumps over the lazy dog", () => {
    const KEY = "key";
    const MSG = "The quick brown fox jumps over the lazy dog";
    const WANT = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";
    const m = new crypto.Hmac("sha256", KEY);
    eq(m.signHex(MSG), WANT, "Hmac matches the published vector");
    eq(m.signHex(MSG), WANT, "and again -- signing resets the context");
    eq(m.signHex(MSG), crypto.HMACHex("sha256", KEY, MSG),
       "Hmac equals the free function");
    eq(m.algorithm, "sha256", "algorithm getter");
    eq(m.digestSize, 32, "digestSize getter");
    eq(hex(m.sign(MSG)), WANT, "sign() returns bytes");

    m.update("The quick brown ").update("fox jumps over ").update("the lazy dog");
    eq(m.digestHex(), WANT, "streamed update()s equal the one-shot");
    eq(m.signHex(MSG), WANT, "and the object is reusable afterwards");

    assert(m.verify(MSG, WANT), "verify accepts the right hex tag");
    assert(m.verify(MSG, bytes(WANT)), "verify accepts the right byte tag");
    assert(!m.verify(MSG, "0".repeat(64)), "verify rejects a wrong tag");
    assert(!m.verify(MSG, WANT.slice(0, 62)), "verify rejects a short tag");
    assert(!m.verify(MSG, "zz" + WANT.slice(2)), "verify rejects a non-hex tag");
    assert(!m.verify("other", WANT), "verify rejects the wrong message");

    for (const algo of ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"]) {
        const h = new crypto.Hmac(algo, KEY);
        eq(h.signHex(MSG), crypto.HMACHex(algo, KEY, MSG), "Hmac/" + algo);
        h.close();
    }
    throws(() => new crypto.Hmac("nope", KEY), "an unknown algorithm");
    throws(() => new crypto.Hmac("sha256"), "a missing key");
    throws(() => crypto.Hmac("sha256", KEY), "calling without new");

    const c = new crypto.Hmac("sha256", KEY);
    c.signHex("x");
    c.close();
    c.close();
    throws(() => c.signHex("x"), "sign after close");
    throws(() => c.verify("x", WANT), "verify after close");
});

__kitS.section("valueOf is NOT the hook here -- a test written", () => {
    const m = new crypto.Hmac("sha256", "key");
    const WANT_INNER = m.signHex("inner");
    let innerResult = null, valueOfRan = false;
    const attack = {
        toString() { innerResult = m.signHex("inner"); return "outer"; },
        valueOf() { valueOfRan = true; return "wrong-hook"; },
    };
    const outer = m.signHex(attack);
    eq(valueOfRan, false, "valueOf is NOT the hook here -- a test written " +
       "against it would pass having exercised nothing");
    eq(innerResult, WANT_INNER, "the reentrant call completed correctly");
    eq(outer, m.signHex("outer"),
       "and the outer call is unaffected: the argument is fully coerced " +
       "before the key schedule is touched, so the order is well-defined");

    const c = new crypto.Hmac("sha256", "key");
    throws(() => c.signHex({ toString() { c.close(); return "data"; } }),
           "close() during coercion");
    const c2 = new crypto.Hmac("sha256", "key");
    throws(() => c2.verify("m", { toString() { c2.close(); return "tag"; } }),
           "close() during the tag's coercion");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
