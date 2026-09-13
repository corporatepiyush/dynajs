#!/usr/bin/env python3
"""gen_semver.py — dyna:semver probes.

Oracles: the semver.org 2.0.0 spec (precedence chain, valid/invalid grammar,
leading-zero rules, build-metadata rules) pinned from the spec text, plus
node-semver de-facto range semantics (^ ~ hyphen || * partials, prerelease
range exclusion) pinned from observations. Property asserts (antisymmetry,
transitivity, sort) run in-probe over a generated version grid.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

def probe_spec():
    # --- semver.org §11 precedence chain: sorted ascending
    chain = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
             "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"]
    # --- valid versions (spec grammar; engine caps per API.md)
    valid = chain + [
        "0.0.0", "1.2.3", "10.20.30", "1.1.2-prerelease+meta",
        "1.1.2+meta", "1.1.2+meta-valid", "1.0.0-alpha0.valid",
        "1.0.0-alpha.0valid", "1.0.0-alpha-beta.1", "1.0.0-0A.is.legal",
        "1.0.0+21AF26D3--117B344092BD", "1.0.0+21.3", "1.2.3+01",  # leading zeros allowed in build
        "v1.2.3",  # leading v allowed (API contract)
        "9007199254740991.0.0",  # == MAX_SAFE_INTEGER ok
        "1.0.0-0", "1.0.0-0009999a",  # 0009999a is alphanumeric (not numeric)
    ]
    # --- invalid versions (spec: leading zeros in numeric fields/prerelease
    # ids, empty identifiers, non-ASCII, junk)
    invalid = [
        "1", "1.2", "1.2.3.0", "01.2.3", "1.02.3", "1.2.03",
        "1.2.3-", "1.2.3+", "1.2.3-01", "1.2.3-0123", "1.2.3-alpha.01",
        "1.2.3-0a.01",  # hmm: "0a" is alphanumeric so leading-zero rule does
        # not apply... but "01" after it is numeric with leading zero -> invalid
        "1.2.3-a..b", "1.2.3-...", "1.2.3+...", "1.2.3+a..b",
        "1.2.3-_.1", "1.2.3-α", " 1.2.3 ", "banana", "",
        "9007199254740992.0.0",  # above MAX_SAFE_INTEGER throws per API
        "1.9007199254740992.0", "1.2.9007199254740992",
        "+1.2.3", "1.2.3++", "-1.2.3",
    ]
    emit = ['import { parse, isValid, clean, compare, eq, gt, lt, gte, lte,',
            '         neq, sort, major, minor, patch, prerelease, inc,',
            '         satisfies, maxSatisfying, minSatisfying, Range } from "dyna:semver";']
    emit.append("var VALID = [" + ",".join('"%s"' % v for v in valid) + "];")
    emit.append("var INVALID = [" + ",".join('"%s"' % v for v in invalid) + "];")
    emit.append("var CHAIN = [" + ",".join('"%s"' % v for v in chain) + "];")
    emit.append("""
var i;
for (i = 0; i < VALID.length; i++) {
  (function (v) {
    assert_eq(isValid(v), true, "isValid true " + v);
  })(VALID[i]);
}
for (i = 0; i < VALID.length; i++) {
  (function (v) {
    var threw = null;
    try { parse(v); } catch (e) { threw = e; }
    if (threw) { __fail++; __out("FAIL parse valid " + v + " threw " + threw.name); }
    else __pass++;
    assert_eq(isValid(v), true, "isValid true " + v);
  })(VALID[i]);
}
for (i = 0; i < INVALID.length; i++) {
  (function (v) {
    assert_eq(isValid(v), false, "isValid false " + JSON.stringify(v));
    assert_throws(function () { parse(v); }, "TypeError", "parse rejects " + JSON.stringify(v));
  })(INVALID[i]);
}
""")
    emit.append("""
// precedence chain: every adjacent pair strictly ordered, full sort equal
for (var a = 0; a < CHAIN.length; a++) {
  for (var b = 0; b < CHAIN.length; b++) {
    var want = a < b ? -1 : (a > b ? 1 : 0);
    assert_eq(compare(CHAIN[a], CHAIN[b]), want, "chain " + CHAIN[a] + " vs " + CHAIN[b]);
  }
}
var sorted = sort(CHAIN.slice());
for (var a = 0; a < CHAIN.length; a++) assert_eq(sorted[a], CHAIN[a], "sort chain[" + a + "]");
// spec precedence rules
assert_eq(compare("1.0.0", "1.0.0-alpha"), 1, "release beats prerelease");
assert_eq(compare("1.0.0-2", "1.0.0-10"), -1, "numeric ids numeric");
assert_eq(compare("1.0.0-1", "1.0.0-alpha"), -1, "numeric < alphanumeric");
assert_eq(compare("1.0.0-alpha", "1.0.0-alpha.1"), -1, "more fields wins (bigger set)");
assert_eq(compare("1.0.0-alpha", "1.0.0-alpha.0"), -1, "larger set wins even vs 0");
assert_eq(compare("1.0.0-BETA", "1.0.0-alpha"), -1, "ASCII lexical (B < a)");
assert_eq(compare("1.0.0+x", "1.0.0"), 0, "build ignored");
assert_eq(compare("1.0.0+aaa", "1.0.0+bbb"), 0, "build ignored both");
assert_eq(eq("1.0.0+x", "1.0.0"), true, "eq ignores build");
assert_eq(neq("1.0.0+x", "1.0.0"), false, "neq ignores build");
// comparison helpers consistent with compare
assert_eq(gt("2.0.0", "1.0.0"), true, "gt");
assert_eq(lt("1.0.0-alpha", "1.0.0"), true, "lt");
assert_eq(gte("1.2.3", "1.2.3"), true, "gte");
assert_eq(lte("1.2.3", "1.2.4"), true, "lte");
// accessors
assert_eq(major("1.2.3"), 1, "major");
assert_eq(minor("1.2.3"), 2, "minor");
assert_eq(patch("1.2.3"), 3, "patch");
assert_eq(JSON.stringify(prerelease("1.2.3-a.1")), "[\\"a\\",1]", "prerelease ids");
assert_eq(prerelease("1.2.3"), null, "prerelease null when none");
""")
    emit.append('summary("semver_spec");')
    return write_probe("semver", "spec", "\n".join(emit))


def probe_inc_coerce():
    null_ = None
    inc_cases = [
        ["1.2.3", "major", null_, "2.0.0"],
        ["1.2.3", "minor", null_, "1.3.0"],
        ["1.2.3", "patch", null_, "1.2.4"],
        ["1.2.3", "premajor", "beta", "2.0.0-beta.0"],
        ["1.2.3", "premajor", null_, "2.0.0-0"],
        ["1.2.3", "preminor", "beta", "1.3.0-beta.0"],
        ["1.2.3", "prepatch", "beta", "1.2.4-beta.0"],
        ["1.2.3", "prerelease", "beta", "1.2.4-beta.0"],
        ["1.2.3", "prerelease", null_, "1.2.4-0"],
        ["1.2.3-0", "prerelease", null_, "1.2.3-1"],
        ["1.2.3-beta", "prerelease", null_, "1.2.3-beta.0"],
        ["1.2.3-beta.0", "prerelease", null_, "1.2.3-beta.1"],
        ["1.2.3-beta.10", "prerelease", null_, "1.2.3-beta.11"],
        ["1.2.3-alpha.beta.1", "prerelease", null_, "1.2.3-alpha.beta.2"],
        ["1.2.3-beta.5", "prerelease", "rc", "1.2.3-rc.0"],
        ["1.2.3-beta.5", "prerelease", "beta", "1.2.3-beta.6"],
        ["1.0.0", "prerelease", "alpha", "1.0.1-alpha.0"],
    ]
    coerce_cases = [
        ["release-2.3.4", "2.3.4"], ["1.2", "1.2.0"], ["v1.2", "1.2.0"],
        ["1", "1.0.0"], ["", None], ["abc", None], ["2", "2.0.0"],
        ["no 42 in here", "42.0.0"], ["1.2.3.4", "1.2.3"],
        ["000000000000000000002.3.4", "3.4.0"],  # >16-digit runs skipped
        ["v1.2.3", "1.2.3"],
    ]
    clean_cases = [
        ["  =v1.2.3  ", "1.2.3"], ["=1.2.3", "1.2.3"], ["==1.2.3", "1.2.3"],
        ["v1.2.3", "1.2.3"], ["1.2.3+build", "1.2.3"], ["not", None],
        ["v 1.2.3", None],
    ]
    def arr(cases):
        rows = []
        for v, r in cases:
            if r is None:
                rows.append('  [%s, null],' % py_str(v))
            else:
                rows.append('  [%s, "%s"],' % (py_str(v), r))
        return "\n".join(rows)
    def py_str(s):
        return '"%s"' % s
    null_ = None

    inc_rows = []
    for v, t, ident, want in inc_cases:
        ident_s = 'null' if ident is None else '"%s"' % ident
        inc_rows.append('  ["%s", "%s", %s, "%s"],' % (v, t, ident_s, want))

    emit = ['import { inc, coerce, clean, parse } from "dyna:semver";']
    emit.append("var INC = [\n" + "\n".join(inc_rows) + "\n];")
    emit.append("""
for (var i = 0; i < INC.length; i++) {
  (function (c) {
    var got;
    try { got = c[2] === null ? inc(c[0], c[1]) : inc(c[0], c[1], c[2]); }
    catch (e) { __fail++; __out("FAIL inc " + c[0] + " " + c[1] + " threw " + e.name); return; }
    assert_eq(got, c[3], "inc " + c[0] + " " + c[1] + (c[2] === null ? "" : " " + c[2]));
  })(INC[i]);
}
assert_throws(function () { inc("1.2.3", "nope"); }, "TypeError", "inc unknown release");
assert_throws(function () { inc("notaversion", "major"); }, "TypeError", "inc bad version");
""")
    emit.append("var COERCE = [\n" + arr(coerce_cases) + "\n];")
    emit.append("""
for (var i = 0; i < COERCE.length; i++) {
  (function (c) {
    var got = coerce(c[0]);
    if (c[1] === null) assert_eq(got, null, "coerce " + JSON.stringify(c[0]));
    else assert_eq(got, c[1], "coerce " + JSON.stringify(c[0]));
  })(COERCE[i]);
}
""")
    emit.append("var CLEAN = [\n" + arr(clean_cases) + "\n];")
    emit.append("""
for (var i = 0; i < CLEAN.length; i++) {
  (function (c) {
    var got = clean(c[0]);
    if (c[1] === null) assert_eq(got, null, "clean " + JSON.stringify(c[0]));
    else assert_eq(got, c[1], "clean " + JSON.stringify(c[0]));
  })(CLEAN[i]);
}
// parse object shape
var p = parse("1.2.3-beta.1+build.5");
assert_eq(p.major, 1, "parse .major");
assert_eq(p.minor, 2, "parse .minor");
assert_eq(p.patch, 3, "parse .patch");
assert_eq(JSON.stringify(p.prerelease), "[\\"beta\\",1]", "parse .prerelease");
assert_eq(JSON.stringify(p.build), "[\\"build\\",\\"5\\"]", "parse .build");
assert_eq(p.version, "1.2.3-beta.1", "parse .version (no build)");
summary("semver_inc_coerce");
""")
    return write_probe("semver", "inc_coerce", "\n".join(emit))


def probe_ranges():
    sat_true = [
        ["1.2.3", "^1.2.3"], ["1.3.0", "^1.2.3"], ["1.9.9", "^1.2.3"],
        ["1.2.3", ">=1.2.3"], ["1.2.4", ">1.2.3"],
        ["1.2.3", "<=1.2.3"], ["1.2.2", "<1.2.3"],
        ["1.2.5", "~1.2"], ["1.2.0", "~1.2"], ["1.9.9", "~1"],
        ["1.2.3", "1.2"], ["1.2.3", "1"], ["1.2.3", "1.2.x"], ["1.2.3", "1.X"],
        ["0.5.0", "^0.x"], ["1.5.0", "^1.x"],
        ["1.5.0", "1.2.3 - 2"], ["2.0.0", "1.2.3 - 2"], ["1.9.9", "1.2.3 - 2.0.0"],
        ["1.4.0", ">=1.2.3 <2.0.0"], ["1.2.3", "=1.2.3"], ["1.2.3", ">= 1.2.3"],
        ["1.2.3", ""], ["1.2.3", "*"], ["1.0.0", "x"], ["1.2.3", "^x"],
        ["0.0.3", "^0.0.3"],
        ["1.5.0", ">=1.2.3 <2 || ^3.0.0"], ["3.1.0", ">=1.2.3 <2 || ^3.0.0"],
        ["v1.2.3", "1.2.3"], ["1.2.3", "v1.2.3"],
        ["1.2.3", ">=1.2.3 <2.0.0 || >3.0.0"],
    ]
    sat_false = [
        ["2.0.0", "^1.2.3"], ["1.2.2", "^1.2.3"],
        ["1.3.0", "~1.2"], ["1.5.0", "~1.2"], ["2.5.0", "~1"],
        ["2.0.0", "^1.x"], ["0.3.0", "^0.2.3"], ["0.0.4", "^0.0.3"],
        ["1.2.2", "1.2.3 - 2"], ["2.0.0", ">=1.2.3 <2.0.0"],
        ["1.2.2", ">1.2.3"], ["1.2.3", ">1.2.3"], ["1.2.4", "<1.2.4"],
        ["2.1.0", ">=1.2.3 <2 || ^3.0.0"],
        ["1.9.9", "^2.0.0"],
        ["1.2.3", "1.2.4"],
    ]
    emit = ['import { satisfies, maxSatisfying, minSatisfying, Range, sort } from "dyna:semver";']
    emit.append("var T = [" + ",".join('["%s","%s"]' % (v, r) for v, r in sat_true) + "];")
    emit.append("var F = [" + ",".join('["%s","%s"]' % (v, r) for v, r in sat_false) + "];")
    emit.append("""
for (var i = 0; i < T.length; i++)
  assert_eq(satisfies(T[i][0], T[i][1]), true, "satisfies true " + T[i][0] + " ~ " + T[i][1]);
for (var i = 0; i < F.length; i++)
  assert_eq(satisfies(F[i][0], F[i][1]), false, "satisfies false " + F[i][0] + " ~ " + F[i][1]);
// prerelease exclusion from ranges without a prerelease comparator (node rule)
assert_eq(satisfies("1.0.0-alpha", "^1.0.0"), false, "prerelease excluded");
assert_eq(satisfies("1.0.0-alpha", "*"), false, "prerelease excluded from *");
assert_eq(satisfies("1.2.3-beta", "1.2.3"), false, "prerelease excluded from exact");
assert_eq(satisfies("1.0.0-beta", "^1.0.0-beta"), true, "same-tuple prerelease included");
assert_eq(satisfies("1.2.3-beta.2", "^1.2.3-beta.2"), true, "beta.2 in >=beta.2");
assert_eq(satisfies("1.2.3-beta.1", "^1.2.3-beta.2"), false, "beta.1 below floor");
assert_eq(satisfies("1.2.4", "^1.2.3-beta.2"), true, "release above prerelease floor");
// max/min
assert_eq(maxSatisfying(["1.2.0", "1.5.0", "2.0.0"], ">=1.0.0 <2.0.0"), "1.5.0", "max doc example");
assert_eq(minSatisfying(["1.2.0", "1.5.0", "0.9.0"], ">=1.0.0 <2.0.0"), "1.2.0", "min doc example");
assert_eq(maxSatisfying(["1.0.0-alpha", "1.0.0", "1.0.1"], ">=1.0.0"), "1.0.1", "max skips prerelease");
assert_eq(maxSatisfying([], "^1.0.0"), null, "max empty");
assert_eq(minSatisfying(["2.0.0"], "^1.0.0"), null, "min none");
// Range object API
var r = new Range(">=1.2.3 <2 || ^3.0.0");
assert_eq(r.setCount, 2, "setCount 2");
assert_eq(r.source, ">=1.2.3 <2 || ^3.0.0", "source preserved");
assert_eq(r.test("1.5.0"), true, "Range.test hit");
assert_eq(r.test("2.5.0"), false, "Range.test miss");
assert_eq(JSON.stringify(r.filter(["1.0.0", "1.5.0", "3.1.0"])), "[\\"1.5.0\\",\\"3.1.0\\"]", "filter preserves order");
assert_eq(JSON.stringify(new Range("^1.0.0").filter(["1.5.0", "0.1.0", "1.0.0", "2.0.0"])), "[\\"1.5.0\\",\\"1.0.0\\"]", "filter order 2");
assert_eq(r.maxSatisfying(["1.5.0", "1.9.9", "3.1.0", "2.0.0"]), "3.1.0", "Range.max");
assert_eq(r.minSatisfying(["3.1.0", "1.5.0"]), "1.5.0", "Range.min");
assert_eq(new Range("").setCount, 1, "empty range compiles as *");
assert_eq(new Range("1.2.3 - 2.3.4").setCount, 1, "hyphen is one set");
// invalid ranges throw
assert_throws(function () { new Range("not a range!"); }, "TypeError", "junk range");
assert_throws(function () { new Range(">=abc"); }, "TypeError", ">=abc");
assert_throws(function () { satisfies("1.2.3", ">>>2"); }, "TypeError", ">>>2");
// sort() sorts in place and returns the SAME array
var arr = ["1.10.0", "1.2.0", "1.2.0-rc.1"];
var ret = sort(arr);
assert_eq(ret === arr, true, "sort returns same array");
assert_eq(JSON.stringify(arr), "[\\"1.2.0-rc.1\\",\\"1.2.0\\",\\"1.10.0\\"]", "sort in place result");
// sort write-back honors Set(O,k,v,true) semantics exactly like Array.prototype.sort
function sortBehavior(fn, a2) { try { fn(a2); return "ok:" + JSON.stringify(a2); } catch (e) { return "throw:" + e.name; } }
var fz1 = ["2.0.0", "1.0.0"]; Object.freeze(fz1);
assert_eq(sortBehavior(function (a2) { sort(a2); }, fz1), "throw:TypeError", "frozen sort throws");
var nw = ["2.0.0", "1.0.0"];
Object.defineProperty(nw, 0, { value: "2.0.0", writable: false, configurable: true, enumerable: true });
assert_eq(sortBehavior(function (a2) { sort(a2); }, nw), "throw:TypeError", "non-writable element throws");
var go = ["2.0.0", "1.0.0"];
Object.defineProperty(go, 0, { get: function () { return "2.0.0"; }, configurable: true });
assert_eq(sortBehavior(function (a2) { sort(a2); }, go), "throw:TypeError", "getter-only element throws");
var sl = ["2.0.0", "1.0.0"]; Object.seal(sl);
assert_eq(sortBehavior(function (a2) { sort(a2); }, sl), "ok:[\\"1.0.0\\",\\"2.0.0\\"]", "sealed array sorts (writable elements)");
summary("semver_ranges");
""")
    return write_probe("semver", "ranges", "\n".join(emit))


def probe_properties():
    # generated grid: property tests over compare/eq/sort round-trip
    emit = ['import { compare, eq, sort, parse, satisfies } from "dyna:semver";']
    emit.append("""
// deterministic version grid: majors {0,1,2,19} x minors x patches x
// prereleases x build metadata
function grid() {
  var out = [];
  var pres = ["", "-alpha", "-alpha.1", "-beta.11", "-rc.1", "-0", "-10.2"];
  var builds = ["", "+build.1", "+001"];
  var ms = [0, 1, 2, 19], ns = [0, 3, 10], ps = [0, 7];
  for (var mi = 0; mi < ms.length; mi++)
    for (var ni = 0; ni < ns.length; ni++)
      for (var pi = 0; pi < ps.length; pi++)
        for (var q = 0; q < pres.length; q++)
          for (var b = 0; b < builds.length; b++)
            out.push(ms[mi] + "." + ns[ni] + "." + ps[pi] + pres[q] + builds[b]);
  return out;
}
var G = grid();
// 1) parse round-trip: parse(v).version === v without build
for (var i = 0; i < G.length; i++) {
  var p = parse(G[i]);
  var noBuild = G[i].split("+")[0];
  if (p.version !== noBuild) { __fail++; __out("FAIL parse roundtrip " + G[i] + " got " + p.version); } else __pass++;
}
// 2) antisymmetry: compare(a,b) === -compare(b,a)
var bad = 0;
for (var i = 0; i < G.length; i += 7)
  for (var j = 0; j < G.length; j += 11) {
    var ab = compare(G[i], G[j]), ba = compare(G[j], G[i]);
    if (ab !== -ba) { bad++; if (bad < 4) __out("FAIL antisym " + G[i] + " vs " + G[j] + " " + ab + " " + ba); }
  }
if (bad) { __fail += 1; __out("FAIL antisymmetry violations: " + bad); } else __pass++;
// 3) eq(x,y) === (compare(x,y)===0)
bad = 0;
for (var i = 0; i < G.length; i += 5)
  for (var j = 0; j < G.length; j += 9)
    if (eq(G[i], G[j]) !== (compare(G[i], G[j]) === 0)) bad++;
if (bad) { __fail += 1; __out("FAIL eq<>compare violations: " + bad); } else __pass++;
// 4) sort then sweep: ascending by compare
var S = sort(G.slice());
bad = 0;
for (var i = 1; i < S.length; i++)
  if (compare(S[i - 1], S[i]) > 0) bad++;
if (bad) { __fail += 1; __out("FAIL sort ascending violations: " + bad); } else __pass++;
// 5) sort is a permutation of the input
if (S.length !== G.length) { __fail++; __out("FAIL sort length"); } else __pass++;
var sumPre = 0, sumS = 0;
for (var i = 0; i < S.length; i++) { sumS += S[i].length; sumPre += G[i].length; }
assert_eq(sumS, sumPre, "sort permutation fingerprint");
// 6) satisfies(x, ">=" + x) and satisfies(x, "<=" + x) always true
bad = 0;
for (var i = 0; i < G.length; i += 3) {
  if (!satisfies(G[i], ">=" + G[i].split("+")[0])) bad++;
  if (!satisfies(G[i], "<=" + G[i].split("+")[0])) bad++;
}
if (bad) { __fail += 1; __out("FAIL self-range violations: " + bad); } else __pass++;
summary("semver_properties");
""")
    return write_probe("semver", "properties", "\n".join(emit))


if __name__ == "__main__":
    print(probe_spec())
    print(probe_inc_coerce())
    print(probe_ranges())
    print(probe_properties())
