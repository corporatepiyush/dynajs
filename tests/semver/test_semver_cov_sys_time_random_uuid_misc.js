// Split from tests/test_cov_sys_time_random_uuid_misc.js: the semver sections.
import * as semver from "dyna:semver";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== semver ===");
{
    const pv = semver.parse("1.2.3-alpha.1+build.5");
    eq(pv.major, 1, "parse major");
    eq(pv.minor, 2, "parse minor");
    eq(pv.patch, 3, "parse patch");
    eq(JSON.stringify(pv.prerelease), '["alpha",1]', "parse prerelease typed");
    eq(pv.build.join("."), "build.5", "parse build metadata");
    for (const good of ["0.0.0", "1.2.3", "1.2.3-rc.1", "1.2.3+b1", "1.2.3-rc.1+b1"])
        eq(semver.isValid(good), true, "isValid ok " + good);
    for (const bad of ["", "1", "1.2", "01.2.3", "1.2.3.4", "not-semver", "1.2.x"])
        eq(semver.isValid(bad), false, "isValid bad " + bad);
    eq(semver.isValid("v1.2.3"), true, "isValid ok v prefix (parse contract)");
    eq(semver.compare("1.0.0", "1.0.0"), 0, "compare equal");
    eq(semver.compare("1.0.0", "2.0.0"), -1, "compare major less");
    eq(semver.compare("1.2.0", "1.1.9"), 1, "compare minor greater");
    eq(semver.compare("1.0.1", "1.0.0"), 1, "compare patch greater");
    eq(semver.compare("1.0.0-alpha", "1.0.0"), -1, "prerelease < release");
    eq(semver.compare("1.0.0-alpha", "1.0.0-beta"), -1, "alpha < beta");
    eq(semver.compare("1.0.0-alpha.1", "1.0.0-alpha"), 1, "longer prerelease greater");
    eq(semver.compare("1.0.0+b", "1.0.0+a"), 0, "build ignored in compare");
    eq(semver.gt("2.0.0", "1.9.9"), true, "gt");
    eq(semver.gte("1.0.0", "1.0.0"), true, "gte equal");
    eq(semver.lt("1.0.0", "1.0.1"), true, "lt");
    eq(semver.lte("1.0.0", "1.0.0"), true, "lte equal");
    eq(semver.eq("1.0.0+meta", "1.0.0"), true, "eq ignores build");
    eq(semver.neq("1.0.0", "1.0.1"), true, "neq");
    eq(semver.major("3.4.5"), 3, "major");
    eq(semver.minor("3.4.5"), 4, "minor");
    eq(semver.patch("3.4.5"), 5, "patch");
    eq(semver.major("3.4.5-beta"), 3, "major with prerelease");
    const unsorted = ["2.0.0", "0.1.0", "1.5.0"];
    const outSorted = semver.sort(unsorted);
    eq(JSON.stringify(outSorted), '["0.1.0","1.5.0","2.0.0"]', "sort ascending");
    eq(outSorted === unsorted, false, "sort returns a fresh array");
    eq(JSON.stringify(unsorted), '["2.0.0","0.1.0","1.5.0"]', "sort does not mutate the input");
    eq(semver.coerce("v1.2.3-beta"), "1.2.3", "coerce v-prefix");
    eq(semver.coerce("release 4.5 candidate"), "4.5.0", "coerce embedded pads to X.Y.Z");
    eq(semver.coerce("nothing"), null, "coerce none -> null");
    eq(semver.clean("=v1.2.3"), "1.2.3", "clean strips =v");
    eq(semver.clean("1.2.3"), "1.2.3", "clean passthrough");
    eq(semver.clean("junk"), null, "clean junk null");
    eq(semver.inc("1.2.3", "major"), "2.0.0", "inc major");
    eq(semver.inc("1.2.3", "minor"), "1.3.0", "inc minor");
    eq(semver.inc("1.2.3", "patch"), "1.2.4", "inc patch");
    eq(semver.inc("1.2.3", "premajor"), "2.0.0-0", "inc premajor");
    eq(semver.inc("1.2.3", "prepatch"), "1.2.4-0", "inc prepatch");
    eq(semver.inc("1.2.3-beta", "prerelease"), "1.2.3-beta.0", "inc prerelease");
    eq(semver.satisfies("1.2.3", "^1.0.0"), true, "caret satisfied");
    eq(semver.satisfies("2.0.0", "^1.0.0"), false, "caret unsatisfied major");
    eq(semver.satisfies("1.2.9", "~1.2.0"), true, "tilde satisfied");
    eq(semver.satisfies("1.3.0", "~1.2.0"), false, "tilde unsatisfied minor");
    eq(semver.satisfies("1.5.0", ">1.0.0 <2.0.0"), true, "AND range");
    eq(semver.satisfies("1.0.0", "1.0.0 || 2.0.0"), true, "OR range");
    eq(semver.satisfies("3.0.0", "*"), true, "star matches");
    const pool = ["1.0.0", "1.5.0", "2.0.0"];
    eq(semver.maxSatisfying(pool, "^1.0.0"), "1.5.0", "maxSatisfying caret");
    eq(semver.maxSatisfying(pool, "^1.0.0") !== "2.0.0", true, "maxSatisfying excludes major bump"); n--;
    n++;
    eq(semver.minSatisfying(pool, ">=1.0.0"), "1.0.0", "minSatisfying floor");
    eq(semver.maxSatisfying(pool, "^9.0.0"), null, "maxSatisfying none null");
    if (semver.Range) {
        const rg = new semver.Range("^1.0.0");
        assert(rg.test ? rg.test("1.2.3") : true, "Range.test");
    }
}
if (fails) {
    print("test_semver_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_semver_cov_sys_time_random_uuid_misc failed");
}
print("test_semver_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
