// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["grow 5 1,2,3,4,5"];
__EXP[5] = ["ag 5 1,9,8,7,6"];
__EXP[6] = ["g2 3 1,1,2"];
// D: dense-prefix sparse + shrink & grow past the iterator's start length
const sparse = [1, 2, 3];
sparse[100] = 101;
sparse[5000] = 5001;
const out = [];
for (const v of sparse) out.push(v === undefined ? "H" : v);
__A("d03_forof_sparse.js:sparse-count", function () { assert_eq(out.length, 5001, "sparse-count"); });
__A("d03_forof_sparse.js:sparse-first5", function () { assert_eq(out.slice(0, 5).join(","), "1,2,3,H,H", "sparse-first5"); });
__A("d03_forof_sparse.js:sparse-last2", function () { assert_eq(out.slice(-2).join(","), "H,5001", "sparse-last2"); });
const grow = [1, 2];
let n = 0;
for (const v of grow) {
  n++;
  if (n === 1) grow.push(3, 4, 5);
}
__L(3, "grow", n, grow.join(","));
const shr = [1, 2, 3, 4];
let m = 0;
for (const v of shr) { m++; shr.length = 0; }
__A("d03_forof_sparse.js:shr", function () { assert_eq(m, 1, "shr"); });
const ag = [1, 2, 3];
let steps = 0;
for (const v of ag) {
  steps++;
  if (steps === 1) { ag.length = 1; ag.push(9, 8, 7, 6); }
}
__L(5, "ag", steps, ag.join(","));
// grow BEYOND 2^32 via push while iterating (length stays valid)
const g2 = [1];
let cnt = 0;
for (const v of g2) { cnt++; if (cnt >= 3) break; g2.push(cnt); }
__L(6, "g2", cnt, g2.join(","));
// iterator on sparse array made sparser mid-flight (delete ahead of cursor)
const sp2 = [0, 1, 2, 3, 4];
const got = [];
for (const v of sp2) {
  got.push(v === undefined ? "H" : v);
  if (got.length === 1) { delete sp2[2]; delete sp2[3]; }
}
__A("d03_forof_sparse.js:del-mid", function () { assert_eq(got.join(","), "0,1,H,H,4", "del-mid"); });

summary("bbreview");
