// covers: dyna:uuid.variant
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_variant");
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

__kitS.section("variant 0xxx -> NCS", () => {
    function withByte8(hi) {
        const b = new Uint8Array(16);
        b[8] = hi;
        return fromBytes(b);
    }
    assertEq(variant(withByte8(0x00)), "NCS", "variant 0xxx -> NCS");
    assertEq(variant(withByte8(0x70)), "NCS", "variant 0111.. still NCS (top bit 0)");
    assertEq(variant(withByte8(0x80)), "RFC4122", "variant 10xx -> RFC4122");
    assertEq(variant(withByte8(0xb0)), "RFC4122", "variant 1011 -> RFC4122");
    assertEq(variant(withByte8(0xc0)), "Microsoft", "variant 110x -> Microsoft");
    assertEq(variant(withByte8(0xd0)), "Microsoft", "variant 1101 -> Microsoft");
    assertEq(variant(withByte8(0xe0)), "Future", "variant 111x -> Future");
    assertEq(variant(withByte8(0xf0)), "Future", "variant 1111 -> Future");
    function withByte6(hi) {
        const b = new Uint8Array(16);
        b[6] = hi << 4;
        return fromBytes(b);
    }
    for (let ver = 0; ver <= 15; ver++)
        assertEq(version(withByte6(ver)), ver, "version nibble " + ver);
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
