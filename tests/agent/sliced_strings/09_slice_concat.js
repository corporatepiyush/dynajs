// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 09_slice_concat"];
/* concatenation of slices: flatten points and in-place-concat safety. */
var s = parent(4000);
var t = s.slice(100, 3000);
/* slice + flat */
var c1 = t + "tail";
eq(c1, s.slice(100, 3000) + "tail", "slice+flat");
eq(c1.length, 2904, "c1 len");
/* slice + slice over the same parent */
var c2 = t + s.slice(2000, 4000);
eq(c2, s.slice(100, 3000) + s.slice(2000, 4000), "slice+slice overlapping parents");
/* COW assumption: mutating nothing, parent and slice must both stay intact
   after heavy concat churn (in-place concat must never write into parent) */
var acc = "";
for (var i = 0; i < 500; i++) acc += t.slice(i, i + 400);
ok(acc.length > 0, "churn produced");
eq(t, s.slice(100, 3000), "slice intact after churn");
eq(s[100], t[0], "parent intact after churn");
/* concat where the LEFT operand is refcount-1 slice (in-place hazard) */
function makeSlice() { var big = parent(9000); return big.slice(10, 8000); }
var joined = makeSlice() + "tailtailtail";
eq(joined.slice(-12), "tailtailtail", "rc1 slice concat tail");
eq(joined.slice(0, 5), s.slice(10, 15), "rc1 slice concat head");
/* rope path: big concat of slices */
var r1 = makeSlice(), r2 = makeSlice();
var big = (r1 + r2);
eq(big.length, 15980, "big concat len");
eq(big.slice(7990, 8010).length, 20, "big concat middle");
/* concat with numbers/undefined/null coercions */
eq(t + 1, s.slice(100, 3000) + "1", "slice + number");
eq(t + undefined, s.slice(100, 3000) + "undefined", "slice + undefined");
eq(null + t.slice(0, 5), "null" + s.slice(100, 105), "null + slice");
/* += accumulation with slices on the right */
var acc2 = "head";
for (var k = 0; k < 50; k++) acc2 += s.slice(k, k + 500);
eq(acc2.length, 4 + 50 * 500, "accum len");
eq(acc2.slice(4, 9), s.slice(0, 5), "accum content");
__L(0, "PASS 09_slice_concat");

summary("sliced_strings");
