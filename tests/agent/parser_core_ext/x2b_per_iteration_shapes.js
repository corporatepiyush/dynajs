__EXP = {};
__EXP[0] = ["S1 0,1,2,3", "S2 0,1,2,3", "S3 0,0,1,1,0,1,2,0,1,3,0,1", "S4 0,2,3", "S5 0,1,2", "S6 0:10,1:9,2:8,3:7", "S7 0,1,2,3", "S8 0,1,2,3", "S9 a,b,c", "S10 1,2,3", "S11 0,6", "S12 0,1,1,2,3", "S13 0,2,4"];
function t(tag, build) { var fns = build(); __L(0, tag, fns.map(f => f()).join(",")); }
t("S1", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); if (p === 1) continue o; } return fns; });
t("S2", function () { var fns = []; for (let p = 0; p < 4; p++) { fns.push(() => p); if (p === 1) continue; } return fns; });
t("S3", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { fns.push(() => q); if (q === 1 && p === 1) continue o; } } return fns; });
t("S4", function () { var fns = []; o: for (let p = 0; p < 4; p++) { if (p === 1) continue o; fns.push(() => p); } return fns; });
t("S5", function () { var fns = []; o: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) break o; } } return fns; });
t("S6", function () { var fns = []; o: for (let p = 0, r = 10; p < 4; p++, r--) { fns.push(() => p + ":" + r); if (p === 1) continue o; } return fns; });
t("S7", function () { var fns = []; o: for (let p = 0; p < 4; p++) { let c = p; fns.push(() => c); if (p === 1) continue o; } return fns; });
t("S8", function () { var fns = []; var i = 0; o: while (i < 4) { let c = i; fns.push(() => c); i++; if (i === 2) continue o; } return fns; });
t("S9", function () { var fns = []; var obj = { a: 1, b: 2, c: 3 }; o: for (let k in obj) { fns.push(() => k); if (k === "b") continue o; } return fns; });
t("S10", function () { var fns = []; o: for (let v of [1, 2, 3]) { fns.push(() => v); if (v === 2) continue o; } return fns; });
t("S11", function () { var fns = []; var done = false; o: for (let p = 0; p < 3; p++) { if (p === 1) { continue o; } let tdz = p * 3; fns.push(() => tdz); } return fns; });
t("S12", function () { var fns = []; o: for (let p = 0; p < 3; p++) { for (let p2 = 0; p2 < 2; p2++) { fns.push(() => p + p2); if (p2 === 0 && p === 1) continue o; } } return fns; });
t("S13", function () { var fns = []; o: for (let p = 0; p < 3; p++) { let q = p * 2; fns.push(() => q); if (q === 2) continue o; } return fns; });

summary("parser_core_ext");
