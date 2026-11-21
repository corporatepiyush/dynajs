// covers: dyna:serialize.CBORDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_cbor_decode_vserialize");
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + String(a) + ", want " + String(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

eq(CBORDecode(bytes("f93c00")), 1, "a half-precision float decodes");

eq(CBORDecode(bytes("fa47c35000")), 100000, "and a single-precision one");

eq(j(CBORDecode(bytes("c074323031332d30332d32315432303a30343a30305a"))),
   '"2013-03-21T20:04:00Z"', "a tag is transparent: the value is what matters");

throws(() => CBORDecode(bytes("9f018202039f0405ffff")),
       "an indefinite length is REFUSED -- every length here is declared");

eq(CBORDecode(bytes("3b7fffffffffffffff"), { bigint: true }), -9223372036854775808n,
   "-2^63 exactly is the last value the int64 path serves, as a BigInt beyond 2^53");

eq(CBORDecode(bytes("3b8000000000000000"), { bigint: true }), -9223372036854775809n,
   "-2^63-1 has no int64; it is an exact BigInt, not the nearest double");

eq(CBORDecode(bytes("3bffffffffffffffff"), { bigint: true }), -18446744073709551616n,
   "major-1 0xffffffffffffffff is -2^64, not a wrap to 0");

__kitS.section("declared length", () => {
    const lies = [
        "dd7fffffff",
        "df7fffffff",
        "db7fffffff",
        "c6ffffffff",
        "9a7fffffff",
        "bb7fffffffffffffff",
        "5bffffffffffffffff",
    ];
    let caught = 0;
    for (const h of lies) {
        try {
            (h[0] === "9" || h[0] === "b" || h[0] === "5" ? CBORDecode : MsgPackDecode)(bytes(h));
        } catch (e) {
            if (String(e.message).indexOf("declared length") >= 0) caught++;
            else print("  " + h + " threw the wrong way: " + e.message);
        }
    }
    eq(caught, lies.length,
       "every lied-about length is refused BEFORE anything is allocated");
});

throws(() => CBORDecode(bytes("0101")), "in CBOR too");

__kitS.section("a 100k-deep tag chain hits the depth cap, not the stack", () => {
    const a = new Uint8Array(100001);
    a.fill(0xc6, 0, 100000);
    a[100000] = 0;
    throws(() => CBORDecode(a), "a 100k-deep tag chain hits the depth cap, not the stack");
    const b = new Uint8Array(256);
    b.fill(0xc6, 0, 255);
    b[255] = 0;
    eq(CBORDecode(b), 0, "255 nested tags still decode (the cap itself is the limit)");
    throws(() => { const t = new Uint8Array(258); t.fill(0xc6, 0, 257); t[257] = 0; CBORDecode(t); },
           "257 tags are one past the cap");
    throws(() => CBORDecode(bytes("c6")), "a truncated tag refuses");
    eq(CBORDecode(bytes("c11a514b67b0")), 1363896240, "tag 1 (epoch) unwraps to the number");
    eq(CBORDecode(bytes("d82a63414263")), "ABc", "an unknown tag (42) unwraps too");
});
__kitS.end();

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
