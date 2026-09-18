// x2 differential: per-iteration binding identity for lexical for-heads.
function t(tag, build) { var fns = build(); console.log(tag, fns.map(f => f()).join(",")); }
// S1: labeled continue to same loop (the defect)
t("S1", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); if (p === 1) continue o; } return fns; });
// S2: unlabeled continue
t("S2", function () { var fns = []; for (let p = 0; p < 4; p++) { fns.push(() => p); if (p === 1) continue; } return fns; });
// S3: labeled continue from INNER loop to outer
t("S3", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { fns.push(() => q); if (q === 1 && p === 1) continue o; } } return fns; });
// S4: labeled continue, closure AFTER the continue point
t("S4", function () { var fns = []; o: for (let p = 0; p < 4; p++) { if (p === 1) continue o; fns.push(() => p); } return fns; });
// S5: break keeps identity (control)
t("S5", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) break o; } } return fns; });
// S6: two vars in the for-head
t("S6", function () { var fns = []; o: for (let p = 0, r = 10; p < 4; p++, r--) { fns.push(() => p + ":" + r); if (p === 1) continue o; } return fns; });
// S7: const-like capture via block-scoped alias
t("S7", function () { var fns = []; o: for (let p = 0; p < 4; p++) { let c = p; fns.push(() => c); if (p === 1) continue o; } return fns; });
// S8: while + labeled continue (no per-iteration semantics: shared binding)
t("S8", function () { var fns = []; var i = 0; o: while (i < 4) { let c = i; fns.push(() => c); i++; if (i === 2) continue o; } return fns; });
// S9: for-in with let + labeled continue
t("S9", function () { var fns = []; var obj = { a: 1, b: 2, c: 3 }; o: for (let k in obj) { fns.push(() => k); if (k === "b") continue o; } return fns; });
// S10: for-of with let + labeled continue
t("S10", function () { var fns = []; o: for (let v of [1, 2, 3]) { fns.push(() => v); if (v === 2) continue o; } return fns; });
// S11: continue then closure reading a later-initialized let (TDZ-safe reads only)
t("S11", function () { var fns = []; var done = false; o: for (let p = 0; p < 3; p++) { if (p === 1) { continue o; } let tdz = p * 3; fns.push(() => tdz); } return fns; });
// S12: nested same-name lexical with labeled continue
t("S12", function () { var fns = []; o: for (let p = 0; p < 3; p++) { for (let p2 = 0; p2 < 2; p2++) { fns.push(() => p + p2); if (p2 === 0 && p === 1) continue o; } } return fns; });
// S13: continue in do-while with let head (C-style do)
t("S13", function () { var fns = []; o: for (let p = 0; p < 3; p++) { let q = p * 2; fns.push(() => q); if (q === 2) continue o; } return fns; });
