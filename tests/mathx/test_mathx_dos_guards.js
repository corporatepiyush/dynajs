// flags: --std
// timeout: 180
// Split from tests/test_dos_guards.js: the mathx sections.
import { nchoosek, besselk, besselkScaled, besselh } from "dyna:mathx";
import { Random } from "dyna:random";

// --- WS-PLATFORM suite (SEC-024 nchoosek, SEC-025 besselk, SEC-026 sample) ---
{
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function timed(fn) { const t0 = Date.now(); const v = fn(); return { v, dt: Date.now() - t0 }; }

{
    const r = timed(() => nchoosek(1e18, 5e17));
    assert(r.v === Infinity, "nchoosek(1e18,5e17) overflows to Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "nchoosek(1e18,5e17) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => nchoosek(1e12, 5e11));
    assert(r.v === Infinity, "nchoosek(1e12,5e11) overflows to Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "nchoosek(1e12,5e11) returns promptly (" + r.dt + " ms)");
}
assert(nchoosek(5, 2) === 10, "nchoosek(5,2) is 10");
assert(nchoosek(10, 0) === 1, "nchoosek(10,0) is 1");
assert(nchoosek(0, 0) === 1, "nchoosek(0,0) is 1");
assert(nchoosek(4, 5) === 0, "nchoosek(4,5) is 0");
assert(nchoosek(1e18, 1) === 1e18, "nchoosek(1e18,1) is n");
assert(nchoosek(67, 33) < Infinity, "nchoosek(67,33) stays finite");

{
    const r = timed(() => besselk(1e9, 0.1));
    assert(r.v === Infinity, "besselk(1e9,0.1) is +Infinity (the limit in nu), not an O(nu) recurrence (got " + r.v + ")");
    assert(r.dt < 2000, "besselk(1e9,0.1) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(3e9, 0.1));
    assert(r.v === Infinity, "besselk(3e9,0.1) is +Infinity without an int cast (got " + r.v + ")");
    assert(r.dt < 2000, "besselk(3e9,0.1) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselkScaled(1e9, 0.25));
    assert(r.v === Infinity, "besselkScaled(1e9,0.25) is +Infinity (got " + r.v + ")");
    assert(r.dt < 2000, "besselkScaled(1e9,0.25) returns promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(1e9, 1));
    assert(r.dt < 2000, "besselk(1e9,1) terminates promptly (" + r.dt + " ms)");
}
{
    const r = timed(() => besselk(1e300, 0.25));
    assert(r.v === Infinity, "besselk(1e300,0.25) keeps the +Infinity convention");
    assert(r.dt < 2000, "besselk(1e300,0.25) prompt");
}
{
    const r = timed(() => besselk(1e6, 0.1));
    assert(r.dt < 2000, "besselk(1e6,0.1) bounded recurrence (" + r.dt + " ms)");
}
{
    let threw = false;
    try { besselh(2e7, 2e8); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "besselh(2e7,2e8) refuses an order that would recurse for seconds");
}
assert(besselh(3, 2.5)[0] !== undefined, "besselh small order still answers");

{
    const r0 = new Random(1234);
    const huge = new Array(4294967295);
    const r = timed(() => r0.sample(huge, 0));
    assert(Array.isArray(r.v) && r.v.length === 0, "sample(huge,0) is empty");
    assert(r.dt < 2000, "sample(huge,0) does not materialize indices (" + r.dt + " ms)");
}
{
    const r0 = new Random(1234);
    const huge = new Array(4294967295);
    huge[0] = "a";
    huge[7] = "b";
    huge[4294967294] = "z";
    const r = timed(() => r0.sample(huge, 2));
    assert(r.v.length === 2, "sample(huge,2) yields 2 entries");
    assert(r.dt < 2000, "sample(huge,2) does not materialize indices (" + r.dt + " ms)");
}
{
    const r0 = new Random(99);
    const small = [10, 20, 30, 40, 50];
    const got = r0.sample(small, 5).slice().sort((a, b) => a - b);
    assert(got.join(",") === "10,20,30,40,50", "sample of all elements is a permutation");
    const three = r0.sample(small, 3);
    assert(three.length === 3 && new Set(three).size === 3, "sample(n) has distinct entries");
    assert(three.every((v) => small.includes(v)), "sample entries come from the source");
}
{
    const r0 = new Random(7);
    const ta = new Uint8Array([1, 2, 3, 4, 5, 6]);
    const got = r0.sample(ta, 3);
    assert(got instanceof Uint8Array && got.length === 3, "typed-array sample keeps the type");
    assert(got.every((v) => v >= 1 && v <= 6), "typed-array sample entries in range");
}
{
    const r0 = new Random(7);
    let threw = false;
    try { r0.sample([1, 2, 3], -1); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "sample with negative n throws RangeError");
}

print("test_mathx_dos_guards: all tests passed (" + n + " assertions)");
}
