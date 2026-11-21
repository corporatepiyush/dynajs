// covers: dyna:uuid.validate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_validate");
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

__kitS.section("validate rejects: '", () => {
    const bad = [
        "",
        "xyz",
        "00000000-0000-0000-0000-00000000000",
        "00000000-0000-0000-0000-0000000000000",
        "00000000-0000-0000-0000-000000000000-",
        "00000000-000-00000-0000-000000000000",
        "0000000g-0000-0000-0000-000000000000",
        "0000000000000000000000000000000g",
        "000000000000000000000000000000000",
        "0000000000000000000000000000000",
        "{00000000-0000-0000-0000-000000000000",
        "00000000-0000-0000-0000-000000000000}",
        "[00000000-0000-0000-0000-000000000000]",
        "urn:uxid:00000000-0000-0000-0000-000000000000",
        "00000000-0000-0000-0000-00000000000 ",
        " 0000000-0000-0000-0000-000000000000 ",
    ];
    for (const s of bad) {
        assert(!validate(s), "validate rejects: '" + s + "'");
        assertThrows(() => parse(s), "parse throws on: '" + s + "'", SyntaxError);
    }
    for (const v of [123, null, undefined, {}, [], true, 0, NaN, new String(NIL)])
        assert(!validate(v), "validate(non-string) is false: " + String(v));
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
