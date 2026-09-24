// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a ab two 48 d"];
__EXP[2] = ["c 2 5,xy"];
__EXP[3] = ["d 930 30"];
// RT1: dynajsc serialization round-trip showcase — folded constants across a
// label merge, small fused string switch, TDZ loop with closures, generator,
// try/finally. Runner additionally compiles this file with dynajsc and runs
// the embedded-bytecode binary; output must equal ./dynajs (and node).
function pick(x) {
  switch (x) {
    case "a" + "b": return "ab";
    case 1 + 1: return "two";
    case (3 << 4): return "48";
    default: return "d";
  }
}
__L(0, "a", pick("ab"), pick(2), pick(48), pick(7));
var fns = [];
for (let i = 0; i < 3; i++) {
  try { fns.push(() => i + (1 << 4)); } finally { }
}
__A("rt01_showcase.js:b", function () { assert_eq(fns.map(f => f()).join(","), "16,17,18", "b"); });
function* g() { var v = 2 + 3; yield v; yield "x" + "y"; return 6 * 7; }
__L(2, "c", JSON.stringify([...g()].length), [...g()].join(","));
var acc = 0;
for (var i = 0; i < 40; i++) { acc += i * (1 + 1); if (i === 30) break; }
__L(3, "d", acc, i);
var objs = { [(1 + 1)]: "two", s: "s" + "s" };
__A("rt01_showcase.js:e", function () { assert_eq(JSON.stringify(objs), "{\"2\":\"two\",\"s\":\"ss\"}", "e"); });
__A("rt01_showcase.js:f", function () { assert_eq((function () { try { return 1 + 1; } finally { console.log("ff", 2 + 2); } })(), 2, "f"); });

summary("parser_core_ext");
