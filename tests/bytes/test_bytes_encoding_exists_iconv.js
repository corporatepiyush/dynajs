// covers: dyna:bytes.encodingExists
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_encoding_exists_iconv");
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

__kitS.section("utf-8 exists", () => {
assert(encodingExists("utf-8"), "utf-8 exists");
});

__kitS.section("windows-1252 exists", () => {
assert(encodingExists("windows-1252"), "windows-1252 exists");
});

__kitS.section("labels are case-insensitive", () => {
assert(encodingExists("WINDOWS-1252"), "labels are case-insensitive");
});

__kitS.section("windows-1252", () => {
assert(encodingExists("  windows-1252  "), "labels tolerate surrounding space");
});

__kitS.section("alias latin1", () => {
assert(encodingExists("latin1"), "alias latin1");
});

__kitS.section("alias cp1252", () => {
assert(encodingExists("cp1252"), "alias cp1252");
});

__kitS.section("alias ascii", () => {
assert(encodingExists("ascii"), "alias ascii");
});

__kitS.section("an invented label does not exist", () => {
assert(!encodingExists("not-a-charset"), "an invented label does not exist");
});
__kitS.end();

if (fails) {
    print("test_iconv: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_iconv failed");
}
print("test_iconv: " + n + " assertions, 0 failures");
