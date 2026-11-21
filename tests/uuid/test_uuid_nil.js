// covers: dyna:uuid.NIL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_nil");
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

__kitS.section("NIL literal", () => {
    assertEq(NIL, "00000000-0000-0000-0000-000000000000", "NIL literal");
    assertEq(MAX, "ffffffff-ffff-ffff-ffff-ffffffffffff", "MAX literal");
    assert(validate(NIL), "validate(NIL)");
    assert(validate(MAX), "validate(MAX)");
    assertEq(version(NIL), 0, "version(NIL)");
    assertEq(version(MAX), 15, "version(MAX)");
    assertEq(variant(NIL), "NCS", "variant(NIL)");
    assertEq(variant(MAX), "Future", "variant(MAX)");
    assertEq(parse(NIL), NIL, "parse(NIL) idempotent");
    assertEq(parse(MAX), MAX, "parse(MAX) idempotent");
    const nb = bytes(NIL), xb = bytes(MAX);
    assert(nb instanceof Uint8Array && nb.length === 16, "bytes(NIL) is Uint8Array(16)");
    for (let i = 0; i < 16; i++) { assertEq(nb[i], 0x00, "NIL byte " + i); assertEq(xb[i], 0xff, "MAX byte " + i); }
    assertEq(fromBytes(nb), NIL, "fromBytes(bytes(NIL))");
    assertEq(fromBytes(xb), MAX, "fromBytes(bytes(MAX))");
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
