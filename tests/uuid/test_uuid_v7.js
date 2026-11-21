// covers: dyna:uuid.v7
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_v7");
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

__kitS.section("v7 not canonical:", () => {
    const N = 100000;
    const seen = new Set();
    const t0 = Date.now();
    let prevMs = 0;
    for (let i = 0; i < N; i++) {
        const u = v7();
        if (!CANON.test(u)) throw new Error("v7 not canonical: " + u);
        if (u[14] !== "7") throw new Error("v7 version char != 7: " + u);
        if (!VARIANT_CH[u[19]]) throw new Error("v7 variant bits != 10xx: " + u);
        if (!validate(u)) throw new Error("v7 does not validate: " + u);
        const ms = extractMs(u);
        if (ms < prevMs) throw new Error("v7 embedded ms decreased: " + prevMs + " -> " + ms);
        prevMs = ms;
        seen.add(u);
    }
    const t1 = Date.now();
    n += 5 * N;
    assertEq(seen.size, N, "v7 uniqueness (no collisions in " + N + ")");

    let it = 0;
    for (const u of seen) {
        if (it++ >= 1000) break;
        const ms = extractMs(u);
        assert(ms >= t0 - 5000 && ms <= t1 + 5000,
            "v7 embedded ms ~ Date.now(): ms=" + ms + " window=[" + t0 + "," + t1 + "]");
        assertEq(version(u), 7, "v7 version()");
        assertEq(variant(u), "RFC4122", "v7 variant()");
        assertEq(fromBytes(bytes(u)), u, "v7 bytes<->string round-trip");
    }
});

__kitS.section("v7 ms timestamps non-decreasing over a tight loop", () => {
    const N = 100;
    const ids = [];
    for (let i = 0; i < N; i++) ids.push(v7());

    let prevMs = 0, msOk = true;
    for (const u of ids) {
        const ms = extractMs(u);
        if (ms < prevMs) { msOk = false; break; }
        prevMs = ms;
    }
    assert(msOk, "v7 ms timestamps non-decreasing over a tight loop");

    let ordered = true;
    for (let i = 1; i < N; i++)
        if (!(ids[i] > ids[i - 1])) { ordered = false; break; }
    assert(ordered, "consecutive v7 strings strictly increase (same-ms counter)");

    const uniq = new Set(ids);
    assertEq(uniq.size, N, "v7 tight-loop uniqueness");
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
