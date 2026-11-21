// covers: dyna:uuid.ULID
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_ulid_ids");
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

__kitS.section("1000 same-ms monotonic ULIDs strictly ascending", () => {
    const T = 4000000000000;
    let prev = null, strictlyAscending = true;
    for (let i = 0; i < 1000; i++) {
        const u = ULID(T, { monotonic: true });
        if (prev !== null && !(u > prev)) strictlyAscending = false;
        prev = u;
    }
    assert(strictlyAscending, "1000 same-ms monotonic ULIDs strictly ascending");
    assertEq(ULIDTime(prev), T, "timestamp roundtrips through ULIDTime");

    assertEq(prev.length, 26, "monotonic ULID is 26 chars");
    assert(!/[ILOU]/.test(prev), "no I L O U symbols");

    let p = null, ok = true;
    for (let i = 0; i < 500; i++) {
        const u = ULID(undefined, { monotonic: true });
        if (p !== null && !(u > p)) ok = false;
        p = u;
    }
    assert(ok, "500 clock-driven monotonic ULIDs strictly ascending");

    {
        const hi = ULID(T + 5000, { monotonic: true });
        const lo = ULID(T, { monotonic: true });
        assert(lo > hi, "explicit ts below the floor clamps UP (id still ascends)");
        assertEq(ULIDTime(lo), T + 5000, "clamped id carries the held millisecond");
    }

    {
        let allAsc = true, p2 = null;
        for (let i = 0; i < 50; i++) {
            const u = ULID(T + 20000);
            if (p2 !== null && !(u > p2)) allAsc = false;
            p2 = u;
        }
        assert(!allAsc, "default ULIDs are NOT monotonic within a ms");
        assertEq(ULIDTime(p2), T + 20000, "default ULIDTime roundtrip");
    }

    {
        const a = ULID(0, { monotonic: true });
        const b = ULID(2 ** 48 - 1, { monotonic: true });
        const c = ULID(0, { monotonic: true });
        assert(a < b && b < c, "ULID 0 -> 2^48-1 -> 0 still ascends (clamps up)");
    }
    {
        const ta = new Uint8Array(16);
        if (typeof ta.buffer.transfer === "function") {
            ta.buffer.transfer();
            assertThrows(() => v8({ fill: ta }), "detached fill", TypeError);
        }
    }
    {
        assertThrows(() => ULID(5, { get monotonic() { throw new Error("boom"); } }),
                     "ULID getter throws propagate");
        let sawTypeError = false;
        try { v8({ get oops() { throw new Error("boom"); } }); }
        catch (e) { sawTypeError = e instanceof TypeError; }
        assert(sawTypeError, "unknown-key check precedes the getter");
    }

    assertThrows(() => ULID(T, { mono: 1 }), "ULID unknown option key", TypeError);
    assertThrows(() => ULID(T, "monotonic"), "ULID non-object opts", TypeError);
    assertThrows(() => ULID(2 ** 48, { monotonic: true }), "atMillis above 48 bits still refused", RangeError);
    assertEq(ULID(0, { monotonic: true }).length, 26, "ULID(0, monotonic) valid");
});
__kitS.end();

print("test_uuid_ids: all tests passed (" + n + " assertions)");
