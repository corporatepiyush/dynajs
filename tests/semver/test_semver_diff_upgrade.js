// covers: dyna:semver.diff
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_diff_upgrade");
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

__kitS.section("diff patch bump", () => {
eq(diff("1.2.3", "1.2.4"), "patch", "diff patch bump");
});

__kitS.section("diff minor bump", () => {
eq(diff("1.2.3", "1.3.0"), "minor", "diff minor bump");
});

__kitS.section("diff major bump", () => {
eq(diff("1.2.3", "2.0.0"), "major", "diff major bump");
});

__kitS.section("diff identical -> null", () => {
eq(diff("1.2.3", "1.2.3"), null, "diff identical -> null");
});

__kitS.section("diff prerelease id", () => {
eq(diff("1.2.3-rc.1", "1.2.3-rc.2"), "prerelease", "diff prerelease id");
});

__kitS.section("diff release vs prerelease is prerelease (contract: highest differing component)", () => {
eq(diff("1.2.3", "1.2.3-rc.1"), "prerelease",
   "diff release vs prerelease is prerelease (contract: highest differing component)");
});

__kitS.section("diff prerelease vs release is prerelease (contract: highest differing component)", () => {
eq(diff("1.2.3-rc.1", "1.2.3"), "prerelease",
   "diff prerelease vs release is prerelease (contract: highest differing component)");
});

__kitS.section("diff build-only (documented: node returns null)", () => {
eq(diff("1.2.3", "1.2.3+build.5"), "build", "diff build-only (documented: node returns null)");
});

__kitS.section("diff two builds -> build", () => {
eq(diff("1.2.3+b1", "1.2.3+b2"), "build", "diff two builds -> build");
});

__kitS.section("diff tuple beats prerelease", () => {
eq(diff("1.2.3-rc.1", "1.3.0"), "minor", "diff tuple beats prerelease");
});

__kitS.section("diff plain major (no 'pre' prefix)", () => {
eq(diff("1.0.0-alpha", "2.0.0"), "major", "diff plain major (no 'pre' prefix)");
});

__kitS.section("diff same build -> null", () => {
eq(diff("1.2.3+v5", "1.2.3+v5"), null, "diff same build -> null");
});
__kitS.end();

throwsTypeError(() => diff("1.2", "1.2.3"), "diff invalid version throws");

throwsTypeError(() => diff("1.2.3"), "diff missing arg throws");

throwsTypeError(() => diff(1, "1.2.3"), "diff non-string throws");

print("test_semver_upgrade: all tests passed (" + n + " assertions)");
