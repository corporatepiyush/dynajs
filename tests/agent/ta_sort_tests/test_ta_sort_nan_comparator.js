/* Regression test: typed-array sort must consult the USER COMPARATOR for
 * every pair, NaN elements included (ECMA-262 %TypedArray%.prototype.sort,
 * SortCompare, https://tc39.es/ecma262/#sec-%typedarray%.prototype.sort
 * importing Array.prototype.sort steps + §23.2.4.7 / §23.1.4.7 notes).
 *
 * Spec reading (the regression locked here):
 *   - The "NaN compares greater than everything" NOTE belongs to the DEFAULT
 *     SortCompare ONLY (typed-array default sort => NaN last; this engine
 *     implements it in js_cmp_doubles and that is correct).
 *   - When comparefn is PROVIDED, the comparator is called for ALL pairs,
 *     NaN elements included. A NaN RESULT from the comparator is treated as
 *     +0 (CompareTypedArrayElements: "if v is NaN, +0 is used"), so the sort
 *     is stable for those pairs (tiebreak by original index).
 *
 * Regression history: an earlier regression forced NaN ELEMENTS to sort last in the
 * comparator path too (skipping the comparator call). That is a spec misread
 * and diverged from the pristine control binary. Reverted; this test locks
 * comparator-literal semantics. Expected values below were captured from the
 * pristine control (/tmp/pip/opt/dynajs_baseline) and are byte-identical to
 * the patched build (see diff_combos.js / out_control.txt).
 *
 * NOTE on node: for the (a,b)=>a-b case node prints [-1, 0, 2, 3.5, NaN] on
 * [3.5,NaN,-1,2,0] — V8 moves NaN to the end even with a comparator. That is
 * V8's OWN implementation-defined deviation (spec-literal: comparator result
 * NaN -> +0 -> stable => NaN keeps its original index 1). We do NOT chase it.
 */
var out = (typeof print === 'function') ? print : console.log;
var fails = 0, runs = 0;

function eq(actual, expected, label) {
  runs++;
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    fails++;
    out("FAIL " + label + ": got " + JSON.stringify(actual) + ", want " + JSON.stringify(expected));
  }
}

function f64(a) { var t = new Float64Array(a); return t; }
function arr(t) { return Array.prototype.slice.call(t); }
function isNaNv(v) { return v !== v; }

/* 1. Comparator ordering NaN first => NaN FIRST (comparator is law). */
(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function (a, b) {
    var an = isNaNv(a), bn = isNaNv(b);
    if (an && bn) return 0;
    if (an) return -1;
    if (bn) return 1;
    return a < b ? -1 : (a > b ? 1 : 0);
  });
  eq(arr(t), [NaN, -1, 0, 2, 3.5], "nanFirst comparator on [3.5,NaN,-1,2,0]");
})();

/* 2. Comparator (a,b)=>a-b: comparator result is NaN for every pair that
 * involves a NaN element -> treated as +0 -> STABLE, NaN stays at its
 * ORIGINAL index. Spec-literal. (node/V8 prints NaN last here; that is V8's
 * impl-defined deviation, documented above -- not a bug.) */
(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function (a, b) { return a - b; });
  eq(arr(t), [3.5, NaN, -1, 0, 2], "x-y comparator on [3.5,NaN,-1,2,0] (NaN stays at index 1)");

  var t2 = f64([1, NaN, 2]);
  t2.sort(function (a, b) { return a - b; });
  eq(arr(t2), [1, NaN, 2], "x-y comparator on [1,NaN,2] (NaN stays at index 1)");

  var t3 = f64([NaN, NaN, 1]);
  t3.sort(function (a, b) { return a - b; });
  eq(arr(t3), [NaN, NaN, 1], "x-y comparator on [NaN,NaN,1] (stability)");
})();

/* 3. DEFAULT sort (no comparator): NaN LAST -- the spec NOTE applies here. */
(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort();
  eq(arr(t), [-1, 0, 2, 3.5, NaN], "default sort on [3.5,NaN,-1,2,0] (NaN last)");

  var t2 = f64([NaN, NaN, 1]);
  t2.sort();
  eq(arr(t2), [1, NaN, NaN], "default sort on [NaN,NaN,1] (NaN last)");

  /* plain-array default sort stays NaN-last too, plus -0/+0 ordering */
  eq([3, 1, 2].sort(), [1, 2, 3], "plain default sort [3,1,2]");
  var t4 = f64([0, -0, 1, NaN]);
  t4.sort();
  eq(arr(t4).map(String), ["0", "0", "1", "NaN"], "default sort -0/+0/NaN");
})();

/* 4. Comparator returning NaN for EVERYTHING -> all +0 -> fully stable. */
(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function () { return NaN; });
  eq(arr(t), [3.5, NaN, -1, 2, 0], "()=>NaN comparator keeps original order");
})();

/* 4b. Constant -1 and 0 comparators: captured from pristine control;
 * stable for +0, engine-deterministic for -1. NaN pairs are NOT forced
 * anywhere by the engine in the comparator path. */
(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function () { return -1; });
  eq(arr(t), [3.5, NaN, -1, 2, 0], "()=>-1 comparator (control-exact)");

  var t2 = f64([3.5, NaN, -1, 2, 0]);
  t2.sort(function () { return 0; });
  eq(arr(t2), [3.5, NaN, -1, 2, 0], "()=>0 comparator (fully stable)");
})();

/* 5. Comparator call counts: with a NaN element present the comparator must
 * still be CALLED for pairs involving it (was skipped by the regression). */
(function () {
  var t = f64([NaN, 1, 0]);
  var calls = 0;
  t.sort(function (a, b) { calls++; return a < b ? -1 : (a > b ? 1 : 0); });
  if (calls < 2) { fails++; out("FAIL call-count: comparator called " + calls + " times, expected >2 (NaN pairs must be consulted)"); }
  runs++;
  /* NOTE: this comparator returns 0 for NaN pairs -> +0 -> stable, so NaN
   * stays at its ORIGINAL index 0 here. */
  eq(arr(t).map(String), ["NaN", "0", "1"], "nan-aware comparator (stable for its own +0 on NaN pairs)");
})();

out((runs - fails) + "/" + runs + " passed" + (fails ? " -- FAILURES: " + fails : ""));
if (fails) throw new Error("ta_sort regression failures: " + fails);
