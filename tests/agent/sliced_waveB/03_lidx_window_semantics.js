/* T3: lastIndexOf differential matrix vs the ES reference implementation.
 * Exercises the budget-exhaustion forward fallback (haystacks > 1024
 * units with matches/absent needles), fromIndex clamping, zero-length
 * needles, overlapping patterns, and SLICE receivers (window search). */
var small = mkvalRnd(64, 11, false);          /* value parent */
var mid = mkvalRnd(1000, 12, false);          /* below budget */
var big = mkvalRnd(3000, 13, false);          /* crosses the 1024 budget */
var bigS = mkvalRnd(3000, 14, true);          /* uppercase alphabet */
var sBig = big.slice(100, 2900);              /* slice receiver */
var sBigU = bigS.slice(0, 2700);              /* slice of slice */
var periodic = ("aabaa" + "b").repeat(600);   /* overlapping matches */

var needles = [
  "",
  "a",
  small.substring(3, 4),
  mid.substring(100, 103),
  big.substring(1000, 1030),
  big.substring(0, 30),
  big.substring(2970, 3000),
  "ZZZZ-not-present-ZZZZ",
  big.substring(2000, 2030) + "qq", /* crosses parent end: absent */
  "aabaa",
  "baab"
];
var froms = [undefined, 0, 1, 2, 5, 500, 1500, 2999, 3000, 5000, -1, -100, NaN, Infinity];

function checkAll(hay, label) {
  for (var i = 0; i < needles.length; i++) {
    var nd = needles[i];
    if (nd === "") { /* zero-length needle: ES says min(n, pos) */
      eq(hay.lastIndexOf(nd), hay.length, label + " empty needle");
      eq(hay.lastIndexOf(nd, 7), Math.min(7, hay.length), label + " empty needle from 7");
      continue;
    }
    for (var f = 0; f < froms.length; f++) {
      var got = hay.lastIndexOf(nd, froms[f]);
      var want = refLastIndexOf(hay, nd, froms[f]);
      eq(got, want, label + " n=" + i + " from=" + froms[f]);
    }
    /* no-from variant */
    eq(hay.lastIndexOf(nd), refLastIndexOf(hay, nd, undefined), label + " n=" + i + " no-from");
  }
}
test("lidx_small", function () { checkAll(small, "small64"); });
test("lidx_mid", function () { checkAll(mid, "mid1000"); });
test("lidx_big_budget_fallback", function () { checkAll(big, "big3000"); });
test("lidx_big_slice_receiver", function () { checkAll(sBig, "slice2800"); });
test("lidx_slice_of_slice", function () { checkAll(sBigU, "sliceseq2700"); });
test("lidx_periodic_overlapping", function () {
  var pats = ["aabaa", "baab", "aab", "b", "aa"];
  for (var i = 0; i < pats.length; i++) {
    eq(periodic.lastIndexOf(pats[i]), refLastIndexOf(periodic, pats[i], undefined), "periodic " + pats[i]);
    eq(periodic.lastIndexOf(pats[i], 1500), refLastIndexOf(periodic, pats[i], 1500), "periodic from " + pats[i]);
  }
});
test("lidx_value_arity_and_types", function () {
  eq(big.lastIndexOf(big.substring(10, 40), undefined), 10, "explicit undefined fromIndex");
  ok(big.lastIndexOf("ZZZ") === -1, "absent");
  /* fromIndex as string/boolean: ES ToInteger path — engine must match node */
  eq(big.lastIndexOf(big.substring(10, 40), "1000"), refLastIndexOf(big, big.substring(10, 40), 1000), "string fromIndex");
  eq(big.lastIndexOf(big.substring(10, 40), true), refLastIndexOf(big, big.substring(10, 40), 1), "boolean fromIndex");
});
test("lidx_call_on_non_slice_same", function () {
  /* the same needles on the FLAT parent must give window-consistent
     answers: matches inside the slice window agree with the slice's own
     search (slice-relative) */
  for (var i = 0; i < needles.length; i++) {
    var nd = needles[i];
    if (nd === "" || nd.length > 100) continue;
    var inSlice = sBig.lastIndexOf(nd);
    if (inSlice < 0) continue;
    eq(big.lastIndexOf(nd, 100 + inSlice), 100 + inSlice, "parent agrees at slice-relative " + i);
  }
});
summary("sliced_waveB");
