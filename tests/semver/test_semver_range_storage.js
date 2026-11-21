// covers: c:src/modules/semver/semver-parse.c
// A Range held 16 comparator sets x 24 comparators, each comparator a full SemVer with 2 x 24
// identifier slots: ~460 KB malloc'd (mmap + munmap) and released for every satisfies() and
// maxSatisfying() call, and every version parse memset its 1.2 KB struct. satisfies was 0.31 us
// per call with memset and the kernel unmap on top of the profile. Now each set grows its
// comparator array as comparators are added (still at most 24 per set, 16 sets) and a parse only
// resets the fields it reads (numbers and identifier counts). These cases pin the limits, the
// failure paths (a range that fails part way must not leave anything behind: the ASan/leak legs run
// this file), identifier reuse after a long prerelease, comparator slots that malloc hands back
// dirty from the previous range (the caret's <2.0.0-0 must not leak its -0 into a later <=2.0.0),
// and Range objects / intersects.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_range_storage");
import * as S from "dyna:semver";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const cmps = (k) => Array.from({ length: k }, (_, i) => ">=" + i + ".0.0").join(" ");
const sets = (k) => Array.from({ length: k }, (_, i) => i + ".0.0").join(" || ");
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", expected " + JSON.stringify(b) + ")");

__kitS.section("comparator limit per set", () => {
    eq(S.satisfies("30.0.0", cmps(24)), true, "24 comparators in one set are accepted");
    eq(S.satisfies("23.0.0", cmps(24)), true, "and all must hold");
    eq(S.satisfies("22.9.9", cmps(24)), false, "and the last one is tested");
    eq(S.satisfies("30.0.0", cmps(25)), false, "25 comparators make the range invalid");
    eq(S.satisfies("1.0.0", "^1.0.0 ~1.0 1.x >=1.0.0-0 <3.0.0 >0.9.0"), true, "expansions add several comparators each");
});

__kitS.section("set limit", () => {
    eq(S.satisfies("15.0.0", sets(16)), true, "16 sets: the last set matches");
    eq(S.satisfies("15.0.0", sets(17)), false, "17 sets make the range invalid");
    eq(S.maxSatisfying(["1.0.0", "15.0.0", "16.0.0"], sets(16)), "15.0.0", "maxSatisfying over 16 sets");
    eq(S.maxSatisfying(["1.0.0"], sets(17)), null, "maxSatisfying over an invalid range");
});

__kitS.section("invalid ranges leave nothing behind", () => {
    for (const bad of ["1.0.0 || >=x.y", "^1.2.3 || ~2 || 1.0.0 - ", ">=1.0.0 " + cmps(24), "1.0.0 || " + sets(16), "<<1.0.0", ">=1.0.0 ||| 2.0.0"]) {
        let threw = false, r;
        try { r = S.satisfies("1.0.0", bad); } catch (e) { threw = true; }
        ok(threw || r === false, "an invalid range is false or throws: " + bad.slice(0, 40));
        try { new S.Range(bad); } catch (e) { }
        try { S.intersects(bad, "1.0.0"); } catch (e) { }
    }
});

__kitS.section("identifier counts after long prereleases", () => {
    const long = "1.0.0-" + Array.from({ length: 24 }, (_, i) => (i % 2 ? "x" + i : String(i))).join(".");
    eq(S.compare(long, "1.0.0"), -1, "a 24-identifier prerelease sorts before the release");
    eq(S.compare("1.0.0", "1.0.0"), 0, "a plain parse after it carries no identifiers over");
    eq(S.compare("1.0.0-alpha", "1.0.0-alpha.0"), -1, "fewer identifiers sort first");
    eq(S.compare("1.0.0+b.1.2.3", "1.0.0"), 0, "build is ignored by compare");
    eq(S.compareBuild("1.0.0+b", "1.0.0"), 1, "compareBuild sees the build");
    eq(S.diff("1.0.0+a", "1.0.0+b"), "build", "diff sees differing builds");
    eq(S.diff("1.0.0", "1.0.0"), null, "and no diff without builds after a build-bearing parse");
    eq(JSON.stringify(S.prerelease("2.0.0")), "null", "prerelease of a release is null after long ones");
    eq(S.satisfies("1.2.3-beta.2", ">=1.2.3-beta.1 <1.2.4"), true, "prerelease on the same tuple");
    eq(S.satisfies("1.2.4-beta.2", ">=1.2.3-beta.1 <1.3.0"), false, "prerelease on another tuple");
    eq(S.satisfies("1.2.4-beta.2", "^1.2.3"), false, "caret excludes other prereleases");
});

__kitS.section("comparator slots reused from an earlier range start clean", () => {
    let bad = 0;
    for (let i = 0; i < 500; i++) {
        S.satisfies("1.5.0", "^1.2.3");
        if (!S.satisfies("2.0.0", ">=1.0.0 <=2.0.0")) bad++;
        S.maxSatisfying(["1.5.0"], "^1.2.3 || ^3.0.0");
        if (S.maxSatisfying(["2.0.0"], ">=1.0.0 <=2.0.0 || >=4.0.0 <=5.0.0") !== "2.0.0") bad++;
    }
    eq(bad, 0, "a plain <=2.0.0 after ^1.2.3 (whose <2.0.0-0 used the same slot) carries no prerelease");
});

__kitS.section("Range objects and intersects", () => {
    const R = new S.Range(">=1.2.0 <2.0.0 || 3.x || 5.1.0 - 5.2.0");
    eq(R.test("1.5.0") && R.test("3.9.9") && R.test("5.1.5"), true, "Range object tests each set");
    eq(R.test("2.0.0"), false, "and rejects between sets");
    eq(S.intersects(R, "4.0.0 || 3.1.0"), true, "intersects with a string range");
    eq(S.intersects(">=6.0.0", R), false, "and without overlap");
    eq(S.intersects(new S.Range("^1.4"), new S.Range("~1.4.5")), true, "two Range objects");
});

__kitS.section("repeated ranges and sort", () => {
    let n = 0;
    for (let i = 0; i < 3000; i++) n += S.satisfies(i % 7 + ".0.0", "^" + (i % 7) + ".0.0 || >=9.0.0 " + "<" + (i % 7 + 1) + ".0.0") ? 1 : 0;
    eq(n, 3000, "repeated satisfies with fresh ranges");
    eq(S.sort(["1.0.0-rc.1", "1.0.0", "0.9.0", "1.0.0-alpha", "1.0.0-alpha.beta"]).join(","), "0.9.0,1.0.0-alpha,1.0.0-alpha.beta,1.0.0-rc.1,1.0.0", "sort with prereleases");
});

__kitS.end();
print(`test_semver_range_storage: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
