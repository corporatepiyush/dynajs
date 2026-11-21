// covers: dyna:bytes.encodings
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_encodings_iconv");
import { decode, encode, encodingExists, encodings } from "dyna:bytes";

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
const B = (...xs) => new Uint8Array(xs);

__kitS.section("first mismatch:", () => {
    let checked = 0, bad = 0;
    for (const label of encodings()) {
        if (label === "utf-8" || label === "us-ascii") continue;
        for (let b = 0x80; b < 0x100; b++) {
            const s = decode(B(b), label);
            if (s === "\uFFFD") continue;
            const back = encode(s, label);
            checked++;
            if (back.length !== 1 || back[0] !== b) {
                bad++;
                if (bad === 1)
                    print("  first mismatch: " + label + " byte 0x" + b.toString(16));
            }
        }
    }
    eq(bad, 0, "every defined byte round-trips in every charset");
    assert(checked > 3000, "the round trip covered the tables (" + checked + " bytes)");
});

__kitS.section("encodings() returns an array", () => {
    const list = encodings();
    assert(Array.isArray(list), "encodings() returns an array");
    assert(list.indexOf("utf-8") >= 0, "encodings() includes utf-8");
    assert(list.indexOf("windows-1252") >= 0, "encodings() includes windows-1252");
    assert(list.indexOf("shift_jis") < 0, "encodings() does not claim shift_jis");
    for (const label of list)
        assert(encodingExists(label),
               "every label encodings() lists actually exists: " + label);
});
__kitS.end();

if (fails) {
    print("test_iconv: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_iconv failed");
}
print("test_iconv: " + n + " assertions, 0 failures");
