// Black-box contract test for dyna:semver, generated from dynajs.d.ts lines 4426-4499. Engine sources not consulted.
// Table-driven: every expectation is a case row; each failure names its row.
// Precedence vectors cite semver.org (spec item 11 and its canonical example); inc/diff/
// compareBuild/intersects behaviors cite the node-semver rules dynajs.d.ts pins by name.
import { parse, isValid, clean, coerce, compare, eq, neq, gt, gte, lt, lte, sort, major, minor, patch, prerelease, inc, diff, compareBuild, intersects, satisfies, maxSatisfying, minSatisfying, Range } from "dyna:semver";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg);
}

// ------------------------------------------------------------------
// parse — d.ts: "Parses major.minor.patch[-pre][+build]".
// Rows: [version, major, minor, patch, prereleaseJSON, buildJSON, version].
// ------------------------------------------------------------------

{
    const PARSE = [
        // DOC-TENSION (resolved): the dynajs.d.ts example merely CALLS parse("1.2.3-beta.1+build.5")
        // without pinning .version; the d.ts exposes build separately (build: string[]) and the
        // repo's own long-standing contract (tests/test_semver.js) pins .version with build
        // STRIPPED ("1.0.0-rc.1+build.123" -> "1.0.0-rc.1"). We follow the more specific text.
        ["build is captured in p.build, stripped from p.version", "1.2.3-beta.1+build.5", 1, 2, 3, ["beta", 1], ["build", "5"], "1.2.3-beta.1"],
        ["a leading v is allowed and stripped (dynajs.d.ts)", "v1.2.3", 1, 2, 3, [], [], "1.2.3"],
        ["zeros parse", "0.0.0", 0, 0, 0, [], [], "0.0.0"],
    ];
    for (const [label, v, mj, mn, pt, pre, build, version] of PARSE) {
        const p = parse(v);
        assertEq(p.major, mj, "parse(" + v + "): major — " + label);
        assertEq(p.minor, mn, "parse(" + v + "): minor — " + label);
        assertEq(p.patch, pt, "parse(" + v + "): patch — " + label);
        assertDeepEq(p.prerelease, pre, "parse(" + v + "): prerelease ids (numeric ids as numbers, d.ts (string|number)[]) — " + label);
        assertDeepEq(p.build, build, "parse(" + v + "): build ids stay strings (d.ts build: string[]) — " + label);
        assertEq(p.version, version, "parse(" + v + "): normalized version — " + label);
    }
}

// d.ts: "Leading zeros, over-long fields, and values above MAX_SAFE throw" (dynajs.d.ts:
// "Returns whether the string parses at all"). Rows: [version, expected].
{
    const ISVALID = [
        ["a plain triple", "1.2.3", true],
        ["a leading v", "v1.2.3", true],
        ["a prerelease and build", "1.2.3-rc.1+b.2", true],
        ["an alphanumeric pre id containing 0 (semver.org section 9)", "1.2.3-0a", true],
        ["missing minor/patch is refused", "1.2", false],
        ["leading zeros throw (doc pins)", "1.02.0", false],
        ["a numeric pre id must not carry a leading zero (semver.org section 9)", "1.2.3-01", false],
        ["an empty build is refused", "1.2.3+", false],
        ["an empty prerelease is refused", "1.2.3-", false],
        ["a field above MAX_SAFE (2^53) throws (doc pins)", "9007199254740992.0.0", false],
        ["the MAX_SAFE cap applies to every field", "1.9007199254740992.0", false],
        ["garbage", "bananas", false],
        ["empty", "", false],
    ];
    for (const [label, v, expected] of ISVALID)
        assertEq(isValid(v), expected, "isValid(" + JSON.stringify(v) + "): " + label);
}

// ------------------------------------------------------------------
// comparison — semver.org item 11 precedence, exactly.
// ------------------------------------------------------------------

{
    // The canonical chain, asserted pairwise.
    const CHAIN = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"];
    for (let i = 0; i + 1 < CHAIN.length; i++) {
        assertEq(compare(CHAIN[i], CHAIN[i + 1]), -1, "semver.org item 11 order: " + CHAIN[i] + " < " + CHAIN[i + 1]);
        assertEq(compare(CHAIN[i + 1], CHAIN[i]), 1, "semver.org item 11 order: " + CHAIN[i + 1] + " > " + CHAIN[i]);
    }

    // Rows: [a, b, expected].
    const ORDER = [
        ["equal versions", "1.0.0", "1.0.0", 0],
        ["build metadata is ignored in precedence (semver.org item 10)", "1.0.0+a", "1.0.0+b", 0],
        ["numeric ids compare NUMERICALLY, not lexically", "1.0.0-alpha.2", "1.0.0-alpha.10", -1],
        ["numeric id ordering continues past 9", "1.0.0-alpha.10", "1.0.0-alpha.11", -1],
        ["alphanumeric ids compare in ASCII order", "1.0.0-alpha.a", "1.0.0-alpha.b", -1],
        ["numeric < alphanumeric (semver.org item 11.4)", "1.0.0-alpha.1", "1.0.0-alpha.beta", -1],
        ["a larger set of pre ids wins when the prefix is equal", "1.0.0-alpha", "1.0.0-alpha.1", -1],
        ["fields decide before prerelease is ever looked at", "2.1.0", "2.0.9", 1],
    ];
    for (const [label, a, b, expected] of ORDER)
        assertEq(compare(a, b), expected, "compare(" + a + ", " + b + "): " + label);
}

// Predicate table — rows: [fnName, a, b, expected].
{
    const PRED = [
        ["eq ignores the leading v", "eq", "v1.2.3", "1.2.3", true],
        ["eq cannot see build (semver.org item 10)", "eq", "1.0.0+a", "1.0.0+b", true],
        ["neq", "neq", "1.0.0", "1.0.1", true],
        ["gt: numeric id comparison through the predicate", "gt", "1.0.0-beta.11", "1.0.0-beta.2", true],
        ["gt is strict", "gt", "1.0.0", "1.0.0", false],
        ["gte: release beats rc", "gte", "1.0.0", "1.0.0-rc.1", true],
        ["gte at equality", "gte", "1.0.0", "1.0.0", true],
        ["lt", "lt", "1.0.0-rc.1", "1.0.0", true],
        ["lte at equality", "lte", "1.0.0", "1.0.0", true],
    ];
    const FNS = { eq, neq, gt, gte, lt, lte };
    for (const [label, name, a, b, expected] of PRED)
        assertEq(FNS[name](a, b), expected, name + "(" + a + ", " + b + "): " + label);
}

// d.ts: "Ascending sort by precedence; does not mutate the input."
{
    const input = ["1.10.0", "1.2.0", "1.2.0-rc.1", "0.9.0"];
    assertDeepEq(sort(input), ["0.9.0", "1.2.0-rc.1", "1.2.0", "1.10.0"], "sort: ascending by precedence");
    assertDeepEq(input, ["1.10.0", "1.2.0", "1.2.0-rc.1", "0.9.0"], "sort: the input array is not mutated (d.ts pins)");
    // DOC-TENSION: dynajs.d.ts says sort "returns the same array, sorted ascending in place";
    // dynajs.d.ts (the contract) says "does not mutate the input" — this file pins the .d.ts.
}

// Field accessors — rows: [fnName, version, expected].
{
    const ACCESS = [
        ["major ignores pre/build", "major", "1.2.3-beta.1+b", 1],
        ["minor ignores pre/build", "minor", "1.2.3-beta.1+b", 2],
        ["patch ignores pre/build", "patch", "1.2.3-beta.1+b", 3],
    ];
    for (const [label, name, v, expected] of ACCESS)
        assertEq({ major, minor, patch }[name](v), expected, name + "(" + v + "): " + label);

    assertEq(prerelease("1.2.3"), null, "prerelease: null when none (doc pins)");
    assertDeepEq(prerelease("1.2.3-alpha.1"), ["alpha", 1], "prerelease: numeric ids as numbers");
}

// ------------------------------------------------------------------
// clean / coerce — d.ts: "Trims, strips a leading `=`" / "The first run of digits".
// Rows: [input, expected].
// ------------------------------------------------------------------

{
    const CLEAN = [
        ["dynajs.d.ts example — trims and strips a leading =", "  =v1.2.3  ", "1.2.3"],
        ["strips v", "v1.2.3", "1.2.3"],
        ["normalized form drops build (see DOC-TENSION at parse)", "1.2.3-beta.1+b.2", "1.2.3-beta.1"],
        ["leading zeros -> null", "1.02.3", null],
        ["garbage -> null", "garbage", null],
        ["empty -> null", "", null],
    ];
    for (const [label, input, expected] of CLEAN)
        assertEq(clean(input), expected, "clean(" + JSON.stringify(input) + "): " + label);

    const COERCE = [
        ["dynajs.d.ts example", "release-2.3.4", "2.3.4"],
        ["missing minor/patch become 0 (doc pins)", "1.2", "1.2.0"],
        ["a bare number becomes X.0.0", "7", "7.0.0"],
        ["the FIRST version-like run wins", "1.2.3.4", "1.2.3"],
        ["null when no usable run exists", "no digits here", null],
        ["runs longer than 16 digits are skipped (dynajs.d.ts)", "12345678901234567", null],
        ["16 digits is inside the cap", "1234567890123456", "1234567890123456.0.0"],
    ];
    for (const [label, input, expected] of COERCE)
        assertEq(coerce(input), expected, "coerce(" + JSON.stringify(input) + "): " + label);
}

// ------------------------------------------------------------------
// inc — dynajs.d.ts: identifier "applies to the pre* steps (default base 0, as in 1.2.3-0)";
// "'release' strips the prerelease ... build metadata never survives inc";
// "when the input has no prerelease it returns null (node-semver pins null)".
// Rows: [version, release, identifier (null = omitted), expected | "THROW"].
// ------------------------------------------------------------------

{
    const INC = [
        ["inc major", "1.2.3", "major", null, "2.0.0"],
        ["inc minor", "1.2.3", "minor", null, "1.3.0"],
        ["inc patch", "1.2.3", "patch", null, "1.2.4"],
        ["inc major from 0.0.0", "0.0.0", "major", null, "1.0.0"],
        ["premajor defaults to base 0", "1.2.3", "premajor", null, "2.0.0-0"],
        ["preminor defaults to base 0", "1.2.3", "preminor", null, "1.3.0-0"],
        ["prepatch defaults to base 0", "1.2.3", "prepatch", null, "1.2.4-0"],
        ["with an identifier the base is the identifier (node-semver inc)", "1.2.3", "premajor", "beta", "2.0.0-beta.0"],
        ["preminor with an identifier", "1.2.3", "preminor", "rc", "1.3.0-rc.0"],
        ["prepatch with an identifier", "1.2.3", "prepatch", "dev", "1.2.4-dev.0"],
        ["prerelease on a release bumps patch and adds -0", "1.2.3", "prerelease", null, "1.2.4-0"],
        ["prerelease on a release with an identifier", "1.2.3", "prerelease", "beta", "1.2.4-beta.0"],
        ["the lowest pre component bumps (semver.org item 11)", "1.2.3-beta.1", "prerelease", null, "1.2.3-beta.2"],
        ["a numeric base bumps", "1.2.3-0", "prerelease", null, "1.2.3-1"],
        ["a non-numeric tail appends .0", "1.2.3-beta", "prerelease", null, "1.2.3-beta.0"],
        ["a new identifier restarts the base", "1.2.3-alpha", "prerelease", "beta", "1.2.3-beta.0"],
        ["release strips pre and build; build never survives inc (doc pins)", "1.2.3-rc.1+build.5", "release", null, "1.2.3"],
        ["release on a version without prerelease returns null (doc pins)", "1.2.3", "release", null, null],
        ["an unknown release type throws (doc pins)", "1.2.3", "frobnicate", null, "THROW"],
    ];
    for (const [label, version, release, identifier, expected] of INC) {
        if (expected === "THROW")
            assertThrows(() => inc(version, release), "inc(" + version + ", " + release + "): " + label);
        else if (identifier === null)
            assertEq(inc(version, release), expected, "inc(" + version + ", " + release + "): " + label);
        else
            assertEq(inc(version, release, identifier), expected, "inc(" + version + ", " + release + ", " + identifier + "): " + label);
    }
}

// ------------------------------------------------------------------
// diff / compareBuild — d.ts pins the node deviations: plain component names
// (never pre-prefixed) and "build" for a build-only difference.
// Rows: [a, b, expected] / [a, b, expected].
// ------------------------------------------------------------------

{
    const DIFF = [
        ["dynajs.d.ts example", "1.2.3", "1.3.0", "minor"],
        ["major", "1.2.3", "2.0.0", "major"],
        ["patch", "1.2.3", "1.2.4", "patch"],
        ["prerelease", "1.2.3", "1.2.3-beta", "prerelease"],
        ["a build-only difference is 'build' (doc pins the node deviation)", "1.2.3", "1.2.3+b2", "build"],
        ["identical versions diff null", "1.2.3", "1.2.3", null],
        ["the plain component name — never 'premajor' (doc pins)", "1.0.0", "2.0.0-alpha", "major"],
        ["the field difference outranks the prerelease difference", "1.2.3", "1.3.0-beta", "minor"],
    ];
    for (const [label, a, b, expected] of DIFF)
        assertEq(diff(a, b), expected, "diff(" + a + ", " + b + "): " + label);

    const CMPBUILD = [
        ["dynajs.d.ts example", "1.0.0+a", "1.0.0+b", -1],
        ["absent build sorts below present (dynajs.d.ts example)", "1.0.0", "1.0.0+rc1", -1],
        ["numeric build ids compare numerically (doc pins)", "1.0.0+2", "1.0.0+10", -1],
        ["numeric < alphanumeric (doc pins)", "1.0.0+1", "1.0.0+a", -1],
        ["identical build", "1.0.0+a", "1.0.0+a", 0],
        ["a longer id list sorts above its prefix (semver.org item 11.4 applied to build)", "1.0.0+a.b", "1.0.0+a", 1],
    ];
    for (const [label, a, b, expected] of CMPBUILD)
        assertEq(compareBuild(a, b), expected, "compareBuild(" + a + ", " + b + "): " + label);
}

// ------------------------------------------------------------------
// ranges — satisfies table. d.ts: "One-shot range match".
// Rows: [version, range, expected].
// ------------------------------------------------------------------

{
    const SATISFIES = [
        ["dynajs.d.ts example", "1.4.0", ">=1.2.3 <2.0.0", true],
        ["the comparator upper bound is exclusive", "2.0.0", ">=1.2.3 <2.0.0", false],
        ["^ includes its lower bound", "1.2.3", "^1.2.3", true],
        ["^1.2.3 spans the minor axis", "1.9.9", "^1.2.3", true],
        ["^1.2.3 excludes 2.0.0", "2.0.0", "^1.2.3", false],
        ["^1.2.3 excludes below", "1.2.2", "^1.2.3", false],
        ["^0.2.3 is >=0.2.3 <0.3.0 (left-most nonzero rule)", "0.2.9", "^0.2.3", true],
        ["^0.2.3 excludes 0.3.0", "0.3.0", "^0.2.3", false],
        ["^0.0.3 is >=0.0.3 <0.0.4", "0.0.3", "^0.0.3", true],
        ["^0.0.3 excludes 0.0.4", "0.0.4", "^0.0.3", false],
        ["~1.2.3 is >=1.2.3 <1.3.0", "1.2.9", "~1.2.3", true],
        ["~1.2.3 excludes 1.3.0", "1.3.0", "~1.2.3", false],
        ["~1 is >=1.0.0 <2.0.0", "1.9.0", "~1", true],
        ["~1 excludes 2.0.0", "2.0.0", "~1", false],
        ["a hyphen range includes both bounds", "1.5.0", "1.2.0 - 2.0.0", true],
        ["the upper hyphen bound is inclusive", "2.0.0", "1.2.0 - 2.0.0", true],
        ["past the upper hyphen bound", "2.0.1", "1.2.0 - 2.0.0", false],
        ["an x-range covers the patch axis", "1.2.5", "1.2.x", true],
        ["the x-range excludes the next minor", "1.3.0", "1.2.x", false],
        ["* matches everything", "9.9.9", "*", true],
        // TEST-FIX: node-semver expands ">2" (a partial with >) to ">=3.0.0" — the WHOLE 2.x
        // series is excluded (verified against node's bundled semver: satisfies('2.0.1','>2')
        // === false, satisfies('3.0.0','>2') === true). The row previously claimed ">2.0.0".
        ["the whole 2.x series is excluded by >2 (node-semver: >=3.0.0)", "2.0.1", ">2", false],
        [">2 admits the next major", "3.0.0", ">2", true],
        [">2.0.0 admits 2.0.1", "2.0.1", ">2.0.0", true],
        [">2 excludes 2.0.0 itself", "2.0.0", ">2", false],
        ["= is exact", "1.2.3", "=1.2.3", true],
        ["|| is a union", "1.0.0", "1.0.0 || 2.0.0", true],
        ["the union refuses neighbors", "1.0.1", "1.0.0 || 2.0.0", false],
        ["a prerelease version is not matched by a range without one (node-semver)", "1.3.0-beta.1", "^1.2.3", false],
        ["...unless a comparator carries a prerelease on the same [major,minor,patch] tuple", "1.3.0-beta.1", "^1.3.0-alpha", true],
        ["the comparator's prerelease opens only its own tuple (1.2.3, not 1.2.4)", "1.2.4-beta.1", ">=1.2.3-beta", false],
        ["same tuple: pre-id ordering decides", "1.2.3-beta.5", ">=1.2.3-beta", true],
    ];
    for (const [label, version, range, expected] of SATISFIES)
        assertEq(satisfies(version, range), expected, "satisfies(" + version + ", " + JSON.stringify(range) + "): " + label);
}

// Range and free selection — rows: [label, versions, range, expected].
{
    const MAXMIN = [
        ["maxSatisfying: dynajs.d.ts example", maxSatisfying, ["1.2.0", "1.5.0", "2.0.0"], ">=1.0.0 <2.0.0", "1.5.0"],
        ["minSatisfying: dynajs.d.ts example", minSatisfying, ["1.2.0", "1.5.0", "0.9.0"], ">=1.0.0 <2.0.0", "1.2.0"],
        ["no match -> null", maxSatisfying, ["0.1.0"], "^2.0.0", null],
    ];
    for (const [label, fn, versions, range, expected] of MAXMIN)
        assertEq(fn(versions, range), expected, fn.name + "(" + JSON.stringify(versions) + ", " + JSON.stringify(range) + "): " + label);

    assertThrows(() => maxSatisfying(["1.0.0", null], "*"), "maxSatisfying: a non-string element throws (dynajs.d.ts pins)");
    assertThrows(() => new Range("bananas"), "new Range: an invalid range throws (doc pins)");
    // TEST-FIX: node-semver's satisfies() catches an invalid range and returns false (it wraps
    // new Range in try/catch); nothing in d.ts/dynajs.d.ts pins a throw. Only the Range CONSTRUCTOR
    // throws on an invalid range (row above).
    assertEq(satisfies("1.0.0", "bananas"), false, "satisfies: an invalid range is simply false (node-semver parity)");
}

// Range object — rows: [rangeString, source, setCount] and [rangeString, version, expected].
{
    const META = [
        ["dynajs.d.ts example range", ">=1.2.3 <2 || ^3.0.0", ">=1.2.3 <2 || ^3.0.0", 2],
        ["a single set", "^1.0.0", "^1.0.0", 1],
    ];
    for (const [label, rangeString, source, setCount] of META) {
        const r = new Range(rangeString);
        assertEq(r.source, source, "Range.source of " + JSON.stringify(rangeString) + ": " + label);
        assertEq(r.setCount, setCount, "Range.setCount of " + JSON.stringify(rangeString) + ": " + label);
    }

    const TEST = [
        ["dynajs.d.ts example: 1.5.0 matches", ">=1.2.3 <2 || ^3.0.0", "1.5.0", true],
        ["below the lower bound", ">=1.2.3 <2 || ^3.0.0", "1.2.2", false],
        ["the ^3.0.0 set matches", ">=1.2.3 <2 || ^3.0.0", "3.1.0", true],
        ["neither set matches", ">=1.2.3 <2 || ^3.0.0", "2.5.0", false],
        ["16 comparator sets compile (the cap is 16)",
            "0.0.1||0.0.2||0.0.3||0.0.4||0.0.5||0.0.6||0.0.7||0.0.8||0.0.9||0.0.10||0.0.11||0.0.12||0.0.13||0.0.14||0.0.15||0.0.16", "0.0.16", true],
    ];
    for (const [label, rangeString, version, expected] of TEST)
        assertEq(new Range(rangeString).test(version), expected, "Range.test(" + version + "): " + label);

    const sets = [];
    for (let i = 0; i < 17; i++) sets.push("0.0." + (i + 1));
    assertThrows(() => new Range(sets.join("||")), "new Range: more than 16 comparator sets throw (dynajs.d.ts compile caps)");

    // Rows: [label, rangeString, versions, expected] and one selection row per method.
    const r = new Range(">=1.2.3 <2 || ^3.0.0");
    assertDeepEq(r.filter(["1.0.0", "1.5.0", "3.1.0"]), ["1.5.0", "3.1.0"],
        "Range.filter: keeps matches in INPUT order (doc pins)");
    assertEq(r.maxSatisfying(["1.5.0", "1.6.0", "3.1.0"]), "3.1.0", "Range.maxSatisfying: dynajs.d.ts example");
    assertEq(r.minSatisfying(["1.5.0", "3.1.0"]), "1.5.0", "Range.minSatisfying: dynajs.d.ts example");
    assertEq(r.minSatisfying(["0.1.0"]), null, "Range.minSatisfying: no satisfying version -> null");
}

// intersects — d.ts: "node-semver's comparator-pairwise rule, including its
// '>=0.0.0'-as-ANY normalization". Rows: [a, b, expected].
{
    const INTERSECTS = [
        ["doc pins the '>=0.0.0'-as-ANY normalization: even this pair intersects", ">=0.0.0", "<0.0.0", true],
        ["dynajs.d.ts example: overlapping caret/tilde", "^1.2.3", "~1.5.0", true],
        ["dynajs.d.ts example: distinct exact versions do not intersect", "1.2.3", "1.3.0", false],
        ["disjoint major ranges", "^1.2.3", "^2.0.0", false],
        ["disjoint x-ranges", "1.x", "2.x", false],
        ["overlapping comparator sets", ">=1.0.0 <1.5.0", ">1.4.0 <2.0.0", true],
    ];
    for (const [label, a, b, expected] of INTERSECTS)
        assertEq(intersects(a, b), expected, "intersects(" + JSON.stringify(a) + ", " + JSON.stringify(b) + "): " + label);

    const r = new Range("^1.2.3");
    assertEq(r.intersects("~1.5.0"), true, "Range.intersects accepts a range string");
    assertEq(r.intersects(new Range("^1.0.0")), true, "Range.intersects accepts a compiled Range");
    assertEq(intersects(r, "^3.0.0"), false, "the free intersects accepts a Range argument");
}

print("bb_semver: all tests passed (" + n + " assertions)");
