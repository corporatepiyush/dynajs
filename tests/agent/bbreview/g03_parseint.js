// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = [["~pi2 1000000000000000", "~pi2 1000000000000000"], ["~pi2 1100000000000000", "~pi2 1100000000000000"], ["~pi2 1200000000000000", "~pi2 1200000000000000"], ["~pi2 1300000000000000", "~pi2 1300000000000000"], ["~pi2 1400000000000000", "~pi2 1400000000000000"], ["~pi2 1500000000000000", "~pi2 1500000000000000"], ["~pi2 1600000000000000", "~pi2 1600000000000000"]];
__EXP[3] = ["pi3 1.9 1", "pi3 -1.9 -1", "pi3 1e-7 1", "pi3 1e+21 1", "pi3 1e-21 1", ["~pi3", "~pi3"], ["~pi3", "~pi3"], "pi3 NaN NaN", "pi3 Infinity NaN", "pi3 -Infinity NaN", "pi3 0 0", ["~pi3", "~pi3"]];
__EXP[4] = ["pi4 ff 16 255", "pi4 777 8 511", "pi4 z 36 35", "pi4 10 2 2", "pi4 0x10 u 16", "pi4 1e3 u 1", "pi4   42   u 42", "pi4 -15 16 -21", "pi4 0b11 2 0", "pi4 0o17 8 0"];
__EXP[5] = ["pi5 NaN NaN NaN NaN 7 55"];
__EXP[6] = ["pi6 123 -0 7 26"];
// G: parseInt identity fast path correctness
const out = [];
for (let n = 0; n <= 100000; n += 1000) out.push(parseInt(n));
__A("g03_parseint.js:pi1", function () { assert_eq(out.join(","), "0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000,14000,15000,16000,17000,18000,19000,20000,21000,22000,23000,24000,25000,26000,27000,28000,29000,30000,31000,32000,33000,34000,35000,36000,37000,38000,39000,40000,41000,42000,43000,44000,45000,46000,47000,48000,49000,50000,51000,52000,53000,54000,55000,56000,57000,58000,59000,60000,61000,62000,63000,64000,65000,66000,67000,68000,69000,70000,71000,72000,73000,74000,75000,76000,77000,78000,79000,80000,81000,82000,83000,84000,85000,86000,87000,88000,89000,90000,91000,92000,93000,94000,95000,96000,97000,98000,99000,100000", "pi1"); });
let ok = true;
for (let n = 0; n <= 100000; n += 1000) if (parseInt(n) !== n) ok = false;
__A("g03_parseint.js:pi1-ok", function () { assert_eq(ok, true, "pi1-ok"); });
for (let i = 0; i <= 6; i++) {
  const x = 1e15 + i * 1e14;
  __L(2, "pi2", String(x), parseInt(x), parseInt(String(x)));
}
for (const v of [1.9, -1.9, 1e-7, 1e21, 1e-21, 2 ** 53, 2 ** 53 + 1, NaN, Infinity, -Infinity, -0, 9007199254740993]) {
  __L(3, "pi3", String(v), parseInt(v));
}
for (const [s, r] of [["ff", 16], ["777", 8], ["z", 36], ["10", 2], ["0x10"], ["1e3"], ["  42  "], ["-15", 16], ["0b11", 2], ["0o17", 8]]) {
  __L(4, "pi4", s, r === undefined ? "u" : r, parseInt(s, r));
}
__L(5, "pi5", parseInt(), parseInt(null), parseInt(true), parseInt([]), parseInt([7]), parseInt({ toString: () => "55" }));
__L(6, "pi6", parseInt("123abc"), parseInt("-0"), parseInt("+7"), parseInt(" 0x1A", 16));

summary("bbreview");
