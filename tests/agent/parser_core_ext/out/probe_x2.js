function t(tag, build) { var fns = build(); console.log(tag, fns.map(f => f()).join(",")); }
var print = (s) => console.log(s);
// V1 (known): labeled continue to OUTER loop head
t("V1", function () {
  var fns = [];
  o1: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o1; } }
  return fns;
});
// V2: labeled continue to the SAME loop's label
t("V2", function () {
  var fns = [];
  o2: for (let p = 0; p < 5; p++) { fns.push(() => p); if (p === 2) continue o2; }
  return fns;
});
// V3: break at p==2 instead of continue
t("V3", function () {
  var fns = [];
  o3: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) break o3; } }
  return fns;
});
// V4: labeled continue but closure created AFTER the inner loop
t("V4", function () {
  var fns = [];
  o4: for (let p = 0; p < 4; p++) { for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o4; } fns.push(() => p); }
  return fns;
});
// V5: while-loop outer with labeled continue from inner for
t("V5", function () {
  var fns = [];
  var p = 0;
  o5: while (p < 4) { let c = p; fns.push(() => c); for (let q = 0; q < 2; q++) { p++; if (q === 1) continue o5; } }
  return fns;
});
