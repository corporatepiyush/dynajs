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

eq(diff("1.2.3", "1.2.4"), "patch", "diff patch bump");
eq(diff("1.2.3", "1.3.0"), "minor", "diff minor bump");
eq(diff("1.2.3", "2.0.0"), "major", "diff major bump");
eq(diff("1.2.3", "1.2.3"), null, "diff identical -> null");
eq(diff("1.2.3-rc.1", "1.2.3-rc.2"), "prerelease", "diff prerelease id");
// FIX2: these two rows previously expected "patch" ("node 7.8.5 parity"). The contract
// (dynajs.d.ts diff + API.md) returns "the highest-precedence component that differs" and pins
// only TWO deviations from node-semver (plain names, build-only -> "build"). With equal
// major/minor/patch the differing component is the prerelease, so diff() returns "prerelease".
eq(diff("1.2.3", "1.2.3-rc.1"), "prerelease",
   "diff release vs prerelease is prerelease (contract: highest differing component)");
eq(diff("1.2.3-rc.1", "1.2.3"), "prerelease",
   "diff prerelease vs release is prerelease (contract: highest differing component)");
eq(diff("1.2.3", "1.2.3+build.5"), "build", "diff build-only (documented: node returns null)");
eq(diff("1.2.3+b1", "1.2.3+b2"), "build", "diff two builds -> build");
eq(diff("1.2.3-rc.1", "1.3.0"), "minor", "diff tuple beats prerelease");
eq(diff("1.0.0-alpha", "2.0.0"), "major", "diff plain major (no 'pre' prefix)");
eq(diff("1.2.3+v5", "1.2.3+v5"), null, "diff same build -> null");
throwsTypeError(() => diff("1.2", "1.2.3"), "diff invalid version throws");
throwsTypeError(() => diff("1.2.3"), "diff missing arg throws");
throwsTypeError(() => diff(1, "1.2.3"), "diff non-string throws");

eq(compareBuild("1.0.0+build.1", "1.0.0+build.2"), -1, "compareBuild lexical ids");
eq(compareBuild("1.0.0+build.2", "1.0.0+build.1"), 1, "compareBuild reverse");
eq(compareBuild("1.0.0+build.1", "1.0.0+build.1"), 0, "compareBuild equal -> 0");
eq(compareBuild("1.0.0", "1.0.0+a"), -1, "compareBuild absent build is lower");
eq(compareBuild("1.0.0+a", "1.0.0"), 1, "compareBuild present build is higher");
eq(compareBuild("1.0.0", "1.0.0"), 0, "compareBuild both absent -> 0");
eq(compareBuild("1.0.0+2", "1.0.0+10"), -1, "compareBuild numeric ids compare numerically");
eq(compareBuild("1.0.0+10", "1.0.0+2"), 1, "compareBuild numeric reverse");
eq(compareBuild("1.0.0+1", "1.0.0+alpha"), -1, "compareBuild numeric < alphanumeric");
eq(compareBuild("2.0.0+1", "1.0.0+2"), 1, "compareBuild precedence first (node parity)");
eq(compareBuild("1.0.0+b.1.1", "1.0.0+b.1"), 1, "compareBuild longer id list wins");
throwsTypeError(() => compareBuild("nope", "1.0.0"), "compareBuild invalid throws");

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

const r1 = new Range("~1.2.0");
eq(intersects(r1, "^1.2.0"), true, "intersects(Range, string)");
eq(intersects("^1.2.0", r1), true, "intersects(string, Range)");
eq(intersects(r1, new Range("^1.3.0")), false, "intersects(Range, Range)");

throwsTypeError(() => intersects("~1.2.0", 42), "intersects non-range throws");
throwsTypeError(() => intersects("~1.2.0", "garbage!!"), "intersects invalid range throws");

eq(r1.intersects("^1.2.0"), true, "Range.intersects(string) true");
eq(r1.intersects("^1.3.0"), false, "Range.intersects(string) false");
eq(r1.intersects(new Range(">=1.2.5 <1.2.9")), true, "Range.intersects(Range)");
eq(new Range("*").intersects("0.0.1"), true, "Range.intersects(anything)");
eq(new Range("2.0.0").intersects("2.0.1"), false, "Range.intersects point misses");
throwsTypeError(() => r1.intersects(7), "Range.intersects non-range throws");

print("test_semver_upgrade: all tests passed (" + n + " assertions)");
