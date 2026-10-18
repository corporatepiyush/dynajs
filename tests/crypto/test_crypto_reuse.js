import { SHA256Hex, MD5Hex, SHA1, SHA1Hex, Hasher } from "dyna:hash";
import { Hmac, HMACHex } from "dyna:crypto";
import { Base64Encode } from "dyna:encoding";

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

const ALGOS = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"];

{
    const MSGS = ["", "a", "abc", "x".repeat(55), "x".repeat(56), "x".repeat(64),
                  "y".repeat(111), "y".repeat(112), "y".repeat(128),
                  "z".repeat(1000)];

    for (const algo of ALGOS) {
        const reused = new Hasher(algo);
        for (const m of MSGS) {
            const fresh = new Hasher(algo);
            const want = fresh.update(m).digestHex();
            reused.reset();
            const got = reused.update(m).digestHex();
            eq(got, want, `reuse ${algo} len=${m.length}`);
        }
        reused.update("CONTAMINANT");
        reused.reset();
        eq(reused.update("abc").digestHex(),
           new Hasher(algo).update("abc").digestHex(),
           `reset() discards a partial message (${algo})`);
    }
}

{
    const h = new Hasher("sha256");
    h.update("ab");
    const d1 = h.digestHex();
    const d2 = h.digestHex();
    eq(d2, d1, "digestHex() is idempotent (state survives finalization)");
    eq(d1, SHA256Hex("ab"), "partial digest matches the one-shot");
    h.update("c");
    eq(h.digestHex(), SHA256Hex("abc"),
       "the hasher keeps absorbing after a digest");
}

{
    const h = new Hasher("sha256");
    h.update("A");
    let fired = false;
    h.update({ valueOf() { fired = true; h.update("B"); return "C"; } });
    assert(!fired, "valueOf is NOT the coercion hook here (ToString wins)");
    eq(h.digestHex(), SHA256Hex("A[object Object]"),
       "a valueOf-only object coerces via Object.prototype.toString");
}
{
    const h = new Hasher("sha256");
    h.update("A");
    h.update({ toString() { h.update("B"); return "C"; } });
    eq(h.digestHex(), SHA256Hex("ABC"),
       "reentrant update() during coercion: inner bytes absorbed first");
}
{
    const h = new Hasher("md5");
    h.update("DISCARDED");
    h.update({ toString() { h.reset(); return "abc"; } });
    eq(h.digestHex(), MD5Hex("abc"),
       "reentrant reset() during coercion: prefix discarded, no corruption");
}
{
    const h = new Hasher("sha256");
    h.update("A");
    let inner = null;
    h.update({ toString() { inner = h.digestHex(); return "B"; } });
    eq(inner, SHA256Hex("A"), "reentrant digest() sees the prefix");
    eq(h.digestHex(), SHA256Hex("AB"), "outer update lands after the inner digest");
}
{
    const h = new Hasher("sha256");
    let depth = 0;
    const nest = {
        toString() {
            const me = String.fromCharCode(65 + depth);
            if (depth < 8) { depth++; h.update(nest); }
            return me;
        },
    };
    h.update(nest);
    eq(h.digestHex(), SHA256Hex("IHGFEDCBA"),
       "8-deep reentrancy absorbs innermost-first with no lost or duplicated byte");
}

{
    n++;
    let threw = false;
    try { new Hasher("sha3-256"); } catch (e) { threw = e instanceof TypeError; }
    if (!threw) throw new Error("assertion failed: unknown algorithm must throw TypeError");
}

{
    const key = "dGhlIHNhbXBsZSBub25jZQ==";
    const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    eq(SHA1Hex(key + GUID), "b37a4f2cc0624f1690f64606cf385945b2bec4ea",
       "RFC 6455 accept: SHA1 of key||GUID");
    eq(Base64Encode(SHA1(key + GUID)), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
       "RFC 6455 accept: the base64 the client compares against");
}

{
    for (const algo of ALGOS) {
        const h = new Hasher(algo);
        eq(h.algorithm, algo, `algorithm getter (${algo})`);
        eq(h.digest().length, h.digestSize, `digestSize matches digest() (${algo})`);
    }
}


{
    for (const algo of ["sha256", "sha512"]) {
        const h = new Hmac(algo, "key");
        h.update("ab");
        eq(h.signHex(""), HMACHex(algo, "key", ""),
           `sign() ignores a pending update() prefix (${algo})`);
        h.update("cd");
        eq(h.signHex("ef"), HMACHex(algo, "key", "ef"),
           `sign() MACs exactly its own message (${algo})`);
        h.update("gh");
        eq(h.verify("ij", HMACHex(algo, "key", "ij")), true,
           `verify() is also a complete MAC (${algo})`);
        h.close();
    }
    const s = new Hmac("sha256", "key");
    s.update("ab").update("cd");
    eq(s.digestHex(), HMACHex("sha256", "key", "abcd"), "update()+digest() still streams");
    s.close();
}

console.log("test_crypto_reuse: all " + n + " tests passed");
