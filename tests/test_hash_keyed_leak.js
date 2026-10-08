import { BLAKE2b, BLAKE2s, BLAKE3, Murmur3_128 } from "dyna:hash";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

const hex = (u8) => Array.from(u8).map(b => b.toString(16).padStart(2, "0")).join("");

{
    const k32 = "k".repeat(32);
    const kb = "B".repeat(64);
    const ks = "s".repeat(32);
    let h1 = "", h2 = "", h3 = "";
    for (let i = 0; i < 10000; i++) {
        const data = "payload-" + i;
        h1 = hex(BLAKE3(data, { key: k32 }));
        h2 = hex(BLAKE2b(data, { key: kb }));
        h3 = hex(BLAKE2s(data, { key: ks }));
    }
    eq(h1.length, 64, "BLAKE3 keyed churn: 10k rounds, last digest 32 bytes");
    eq(h2.length, 128, "BLAKE2b keyed churn: 10k rounds, last digest 64 bytes");
    eq(h3.length, 64, "BLAKE2s keyed churn: 10k rounds, last digest 32 bytes");
    for (let i = 0; i < 2000; i++)
        BLAKE3("material-" + i, { deriveKey: "context/" + (i % 64) });
    for (let i = 0; i < 2000; i++)
        BLAKE3("material-" + i, { context: "ctx string " + i });
    assert(true, "derive/context churn completed");
}

{
    const long64 = "L".repeat(64);
    throwsMatch(() => BLAKE3("d", { key: "" }), /exactly 32/,
        "BLAKE3: empty string key refused");
    throwsMatch(() => BLAKE3("d", { key: "short" }), /exactly 32/,
        "BLAKE3: 5-byte string key refused");
    throwsMatch(() => BLAKE3("d", { key: "k".repeat(31) }), /exactly 32/,
        "BLAKE3: 31-byte string key refused (min-1)");
    throwsMatch(() => BLAKE3("d", { key: "k".repeat(33) }), /exactly 32/,
        "BLAKE3: 33-byte string key refused (max+1)");
    throwsMatch(() => BLAKE3("d", { key: long64 }), /exactly 32/,
        "BLAKE3: 64-byte string key refused");
    throwsMatch(() => BLAKE3("d", { key: "k".repeat(300) }), /exactly 32/,
        "BLAKE3: 300-byte string key refused");
    throwsMatch(() => BLAKE3("d", { key: 123456789 }), /a key or secret must be a string, an ArrayBuffer or a byte view/,
        "BLAKE3: numeric key refused as a type error, never stringified");
    throwsMatch(() => BLAKE2b("d", { key: "k".repeat(65) }), /0 to 64/,
        "BLAKE2b: 65-byte string key refused (max+1)");
    throwsMatch(() => BLAKE2s("d", { key: "k".repeat(33) }), /0 to 32/,
        "BLAKE2s: 33-byte string key refused (max+1)");
    throwsMatch(() => BLAKE2s("d", { key: long64 }), /0 to 32/,
        "BLAKE2s: 64-byte string key refused");
    eq(BLAKE2b("d", { key: "" }).length, 64, "BLAKE2b: empty key is legal");
    eq(BLAKE2s("d", { key: "" }).length, 32, "BLAKE2s: empty key is legal");
    eq(BLAKE2b("d", { key: "k".repeat(64) }).length, 64, "BLAKE2b: 64-byte key accepted (max)");
    eq(BLAKE2s("d", { key: "k".repeat(32) }).length, 32, "BLAKE2s: 32-byte key accepted (max)");
    eq(BLAKE3("d", { key: "k".repeat(32) }).length, 32, "BLAKE3: 32-byte key accepted (exact)");
    throwsMatch(() => BLAKE3("d", { key: "k".repeat(32), context: "ctx" }), /different modes|key/,
        "key + context conflict refused");
    throwsMatch(() => BLAKE3("d", { key: "k".repeat(32), deriveKey: "ctx" }), /different modes|key/,
        "key + deriveKey conflict refused");
    {
        const conflictMsg = (opts) => {
            try { BLAKE3("d", opts); return ""; }
            catch (e) { return String(e.message); }
        };
        const m1 = conflictMsg({ key: "k".repeat(32), context: "ctx" });
        assert(m1.indexOf("{key} and {context} are different modes") >= 0,
               "key + context conflict names {context} (got: " + m1 + ")");
        assert(m1.indexOf("{deriveKey}") < 0,
               "key + context conflict does NOT name {deriveKey} (got: " + m1 + ")");
        const m2 = conflictMsg({ key: "k".repeat(32), deriveKey: "ctx" });
        assert(m2.indexOf("{key} and {deriveKey} are different modes") >= 0,
               "key + deriveKey conflict names {deriveKey} (got: " + m2 + ")");
        assert(m2.indexOf("{context}") < 0,
               "key + deriveKey conflict does NOT name {context} (got: " + m2 + ")");
    }
    throwsMatch(() => BLAKE3("d", { context: "ctx", deriveKey: "ctx2" }), /spelled twice|either/,
        "context + deriveKey conflict refused");
    throwsMatch(() => BLAKE3("d", { context: "" }), /non-empty|context/,
        "empty context refused");
    throwsMatch(() => BLAKE3("d", { deriveKey: "" }), /non-empty|context|derive/,
        "empty deriveKey context refused");
    throwsMatch(() => BLAKE3("d", { deriveKey: "ctx", length: 16 }), /32 bytes|length/,
        "deriveKey + explicit length refused");
    throwsMatch(() => BLAKE3("d", "opts-not-an-object"), /object|options|length/,
        "non-object second argument refused");
    throwsMatch(() => BLAKE2s("d", { key: "k".repeat(32), nope: 1 }), /unknown|option|nope/,
        "unknown option key refused by name");
}

{
    const trap = (name) => {
        const o = { key: "k".repeat(32) };
        Object.defineProperty(o, name, { get() { throw new Error("trap " + name); } });
        throwsMatch(() => BLAKE3("d", o), new RegExp("trap " + name),
            "a throwing '" + name + "' getter propagates cleanly");
    };
    trap("context");
    trap("deriveKey");
    trap("length");
}

{
    eq(hex(BLAKE3("abc")), hex(BLAKE3("abc")), "unkeyed BLAKE3 is stable");
    eq(Murmur3_128("abc").length, 16, "Murmur3_128 digest is 16 bytes");
    eq(hex(Murmur3_128("abc", 42)), hex(Murmur3_128("abc", 42)), "seeded Murmur is stable");
}

print("test_hash_keyed_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)"));
if (fails) throw new Error(fails + " failures");
