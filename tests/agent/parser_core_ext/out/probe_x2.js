function t(tag, build) { var fns = build(); console.log(tag, fns.map(f => f()).join(",")); }
var print = (s) => console.log(s);
t("V1", function () {
  var fns = [];
  o1: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o1; } }
  return fns;
});
t("V2", function () {
  var fns = [];
  o2: for (let p = 0; p < 5; p++) { fns.push(() => p); if (p === 2) continue o2; }
  return fns;
});
t("V3", function () {
  var fns = [];
  o3: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) break o3; } }
  return fns;
});
t("V4", function () {
  var fns = [];
  o4: for (let p = 0; p < 4; p++) { for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o4; } fns.push(() => p); }
  return fns;
});
t("V5", function () {
  var fns = [];
  var p = 0;
  o5: while (p < 4) { let c = p; fns.push(() => c); for (let q = 0; q < 2; q++) { p++; if (q === 1) continue o5; } }
  return fns;
});
