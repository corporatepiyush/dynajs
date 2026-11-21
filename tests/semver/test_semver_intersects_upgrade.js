// covers: dyna:semver.intersects
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_intersects_upgrade");
import {
    diff, compareBuild, intersects, Range,
} from "dyna:semver";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(actual, expected, msg) {
    assert(actual === expected,
        msg + " (got " + JSON.stringify(actual) +
        ", expected " + JSON.stringify(expected) + ")");
}
function throwsTypeError(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, msg + " (TypeError, got " + caught + ")");
}

__kitS.section("1.0.0-alpha", () => {
const MATRIX = [
    ["1.3.9", "~1.3.0", true],
    ["1.2.3", "~1.2.0", true],
    ["1.2.3", "1.3.0", false],
    ["~1.2.0", "~1.3.0", false],
    ["^1.0.0", "^2.0.0", false],
    ["^1.0.0", "^1.1.0", true],
    ["1.0.0-alpha", "^1.0.0", false],
    ["1.0.0-alpha", "<2.0.0", false],
    ["2.0.0-beta", "<2.0.0-0", false],
    ["1.4.0", "~1.3.0", false],
    ["1.3.9", ">1.3.10", false],
    [">=2.0.0 <3.0.0", ">=1.0.0 <2.0.0", false],
    ["1.0.0", "*", true],
    ["1.5.2", "1.x", true],
    ["1.0.0-0", "<0.0.1", false],
    ["0.0.1", "<0.0.1", false],
    [">=1.0.0 <=2.0.0", ">=1.5.0 <=2.5.0", true],
    [">2.0.0", "<1.0.0", false],
    [">=0.0.0", "<0.0.0", true],
    ["1.0.0 || 2.0.0", "2.0.0", true],
    ["1.0.0 - 1.5.0", "1.2.0", true],
    [">1.0.0", "<2.0.0", true],
];
for (const [a, b, want] of MATRIX)
    eq(intersects(a, b), want, "intersects(" + a + ", " + b + ")");
});

const r1 = new Range("~1.2.0");
eq(intersects(r1, "^1.2.0"), true, "intersects(Range, string)");

__kitS.section("intersects(string, Range)", () => {
eq(intersects("^1.2.0", r1), true, "intersects(string, Range)");
});

__kitS.section("intersects(Range, Range)", () => {
eq(intersects(r1, new Range("^1.3.0")), false, "intersects(Range, Range)");
});

throwsTypeError(() => intersects("~1.2.0", 42), "intersects non-range throws");

throwsTypeError(() => intersects("~1.2.0", "garbage!!"), "intersects invalid range throws");

__kitS.section("Range.intersects(string) true", () => {
eq(r1.intersects("^1.2.0"), true, "Range.intersects(string) true");
});

__kitS.section("Range.intersects(string) false", () => {
eq(r1.intersects("^1.3.0"), false, "Range.intersects(string) false");
});

__kitS.section("Range.intersects(Range)", () => {
eq(r1.intersects(new Range(">=1.2.5 <1.2.9")), true, "Range.intersects(Range)");
});

__kitS.section("Range.intersects(anything)", () => {
eq(new Range("*").intersects("0.0.1"), true, "Range.intersects(anything)");
});

__kitS.section("Range.intersects point misses", () => {
eq(new Range("2.0.0").intersects("2.0.1"), false, "Range.intersects point misses");
});
__kitS.end();

throwsTypeError(() => r1.intersects(7), "Range.intersects non-range throws");

print("test_semver_upgrade: all tests passed (" + n + " assertions)");
