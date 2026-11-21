// covers: dyna:serialize.ValueHash
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_value_hash_vserialize");
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

__kitS.section("key ORDER does not change the hash -- the encoding is canonical", () => {
    eq(ValueHash({ a: 1, b: 2 }), ValueHash({ b: 2, a: 1 }),
       "key ORDER does not change the hash -- the encoding is canonical");
    assert(ValueHash({ a: 1 }) !== ValueHash({ a: 2 }), "but a value does");
    assert(ValueHash([1, 2]) !== ValueHash([2, 1]), "and array order does");
    assert(ValueHash("1") !== ValueHash(1), "a string is not its number");
    eq(ValueHash(null), ValueHash(null), "the same value hashes the same");
    eq(ValueHash({ a: { b: [1, "x"] } }).length, 16, "the digest is 16 hex digits");
    assert(/^[0-9a-f]{16}$/.test(ValueHash(42)), "and only hex");
    throws(() => ValueHash(), "a value is required");
    const cyc = {}; cyc.me = cyc;
    throws(() => ValueHash(cyc), "a cycle has no canonical encoding");
});
__kitS.end();

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
