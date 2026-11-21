// covers: dyna:uuid.fromBytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_from_bytes");
import {
    v4, v7, v3, v5, parse, validate, version, variant, bytes, fromBytes,
    NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500,
} from "dyna:uuid";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + " (got " + got + ", want " + want + ")");
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}

const CANON = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const VARIANT_CH = { "8": 1, "9": 1, "a": 1, "b": 1 };

function extractMs(u) {
    return parseInt(u.slice(0, 8) + u.slice(9, 13), 16);
}

__kitS.section("fromBytes(15) throws", () => {
    assertThrows(() => fromBytes(new Uint8Array(15)), "fromBytes(15) throws", RangeError);
    assertThrows(() => fromBytes(new Uint8Array(17)), "fromBytes(17) throws", RangeError);
    assertThrows(() => fromBytes("nope"), "fromBytes(string) throws", TypeError);
    const ab = new ArrayBuffer(16);
    new Uint8Array(ab).fill(0xab);
    assertEq(fromBytes(ab), "abababab-abab-abab-abab-abababababab",
        "fromBytes(ArrayBuffer)");
    assertEq(fromBytes(new Int8Array(16)), NIL, "fromBytes(Int8Array of zeros)");
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
