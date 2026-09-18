// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 14_slice_arrays"];
/* arrays of slices: sort, join, indexOf, includes, reverse. */
var s = parent(400);
var arr = [];
for (var i = 0; i < 50; i++) arr.push(s.slice(i * 3, i * 3 + 20));
eq(arr.length, 50, "built");
var joined = arr.join("");
ok(joined.length === 50 * 20, "join len");
eq(arr.indexOf(s.slice(9 * 3, 9 * 3 + 20)), 9, "indexOf slice elem");
ok(arr.includes(s.slice(20 * 3, 20 * 3 + 20)), "includes");
var sorted = arr.slice().sort();
ok(sorted[0] <= sorted[1], "sorted start");
eq(sorted.length, 50, "sorted len");
arr.reverse();
eq(arr[49], s.slice(0, 20), "reverse");
/* sort comparator with slices */
var nums = [10, 2, 33].map(function (n) { return (parent(n) + "!").slice(0, n); });
nums.sort(function (x, y) { return x.length - y.length; });
eq(nums.map(function (x) { return x.length; }).join(","), "2,10,33", "sort by len");
/* concat arrays of slices */
eq(arr.slice(0, 3).concat(arr.slice(3, 5)).length, 5, "array concat");
/* array spread of slices */
var all = [].concat(arr.slice(0, 1), [s.slice(0, 1)]);
eq(all.length, 2, "concat spread");
__L(0, "PASS 14_slice_arrays");

summary("sliced_strings");
