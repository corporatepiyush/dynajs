// covers: dyna:uuid.NAMESPACE_DNS
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_namespace_dns");
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

__kitS.section("namespace valid:", () => {
    assertEq(NAMESPACE_DNS, "6ba7b810-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_DNS");
    assertEq(NAMESPACE_URL, "6ba7b811-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_URL");
    assertEq(NAMESPACE_OID, "6ba7b812-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_OID");
    assertEq(NAMESPACE_X500, "6ba7b814-9dad-11d1-80b4-00c04fd430c8", "NAMESPACE_X500");
    for (const ns of [NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500])
        assert(validate(ns), "namespace valid: " + ns);
    assertEq(version(NAMESPACE_DNS), 1, "NAMESPACE_DNS is v1");
    assertEq(variant(NAMESPACE_DNS), "RFC4122", "NAMESPACE_DNS variant");
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
