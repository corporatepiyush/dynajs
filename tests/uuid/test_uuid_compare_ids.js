// covers: dyna:uuid.compare
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_compare_ids");
import {
    v4, v7, v8, compare, parse, version, variant, bytes, fromBytes,
    ULID, ULIDTime, NIL, MAX,
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

__kitS.section("compare(NIL, NIL) == 0", () => {
    assertEq(compare(NIL, NIL), 0, "compare(NIL, NIL) == 0");
    assertEq(compare(NIL, MAX), -1, "NIL sorts before MAX");
    assertEq(compare(MAX, NIL), 1, "MAX sorts after NIL");
    assertEq(compare("00000000-0000-0000-0000-000000000001",
                     "00000000-0000-0000-0000-000000000002"), -1, "last-byte ordering");

    const u = v7();
    assertEq(compare(u, parse(u)), 0, "canonical vs parsed");
    assertEq(compare(u, "urn:uuid:" + u.toUpperCase()), 0, "urn + uppercase form");
    assertEq(compare(u, "{" + u + "}"), 0, "braced form");
    assertEq(compare(u, u.replace(/-/g, "")), 0, "raw hex form");

    let prev = null, ordered = true;
    for (let i = 0; i < 200; i++) {
        const id = v7();
        if (prev !== null && compare(prev, id) > 0) ordered = false;
        prev = id;
    }
    assert(ordered, "200 fresh v7 ids are non-decreasing under compare");

    const a = "0e000000-0000-7000-8000-000000000000";
    const b = "10000000-0000-7000-8000-000000000000";
    assertEq(compare(a, b), a < b ? -1 : 1, "byte order agrees with string order");

    assertThrows(() => compare("nope", NIL), "malformed a", TypeError);
    assertThrows(() => compare(NIL, "123"), "malformed b", TypeError);
    assertThrows(() => compare(42, NIL), "non-string a", TypeError);
    assertThrows(() => compare(NIL, null), "non-string b", TypeError);
    assertThrows(() => compare(NIL), "missing b", TypeError);
    assertThrows(() => compare("1234", NIL), "too-short raw hex", TypeError);
});
__kitS.end();

print("test_uuid_ids: all tests passed (" + n + " assertions)");
