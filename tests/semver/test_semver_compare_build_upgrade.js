// covers: dyna:semver.compareBuild
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_compare_build_upgrade");
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

__kitS.section("compareBuild lexical ids", () => {
eq(compareBuild("1.0.0+build.1", "1.0.0+build.2"), -1, "compareBuild lexical ids");
});

__kitS.section("compareBuild reverse", () => {
eq(compareBuild("1.0.0+build.2", "1.0.0+build.1"), 1, "compareBuild reverse");
});

__kitS.section("compareBuild equal -> 0", () => {
eq(compareBuild("1.0.0+build.1", "1.0.0+build.1"), 0, "compareBuild equal -> 0");
});

__kitS.section("compareBuild absent build is lower", () => {
eq(compareBuild("1.0.0", "1.0.0+a"), -1, "compareBuild absent build is lower");
});

__kitS.section("compareBuild present build is higher", () => {
eq(compareBuild("1.0.0+a", "1.0.0"), 1, "compareBuild present build is higher");
});

__kitS.section("compareBuild both absent -> 0", () => {
eq(compareBuild("1.0.0", "1.0.0"), 0, "compareBuild both absent -> 0");
});

__kitS.section("compareBuild numeric ids compare numerically", () => {
eq(compareBuild("1.0.0+2", "1.0.0+10"), -1, "compareBuild numeric ids compare numerically");
});

__kitS.section("compareBuild numeric reverse", () => {
eq(compareBuild("1.0.0+10", "1.0.0+2"), 1, "compareBuild numeric reverse");
});

__kitS.section("compareBuild numeric < alphanumeric", () => {
eq(compareBuild("1.0.0+1", "1.0.0+alpha"), -1, "compareBuild numeric < alphanumeric");
});

__kitS.section("compareBuild precedence first (node parity)", () => {
eq(compareBuild("2.0.0+1", "1.0.0+2"), 1, "compareBuild precedence first (node parity)");
});

__kitS.section("compareBuild longer id list wins", () => {
eq(compareBuild("1.0.0+b.1.1", "1.0.0+b.1"), 1, "compareBuild longer id list wins");
});
__kitS.end();

throwsTypeError(() => compareBuild("nope", "1.0.0"), "compareBuild invalid throws");

print("test_semver_upgrade: all tests passed (" + n + " assertions)");
