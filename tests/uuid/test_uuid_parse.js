// covers: dyna:uuid.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_parse");
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

__kitS.section("parse canonical", () => {
    const canon = v4();
    const upper = canon.toUpperCase();
    const raw   = canon.replace(/-/g, "");
    const braced = "{" + canon + "}";
    const urn   = "urn:uuid:" + canon;
    const urnUp = "URN:UUID:" + upper;

    assertEq(parse(canon), canon, "parse canonical");
    assertEq(parse(upper), canon, "parse uppercase -> lowercase canonical");
    assertEq(parse(raw), canon, "parse 32-char raw hex");
    assertEq(parse(braced), canon, "parse braced");
    assertEq(parse(urn), canon, "parse urn:uuid:");
    assertEq(parse(urnUp), canon, "parse URN:UUID: (case-insensitive)");
    for (const f of [canon, upper, raw, braced, urn, urnUp])
        assert(validate(f), "validate accepts form: " + f);
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
