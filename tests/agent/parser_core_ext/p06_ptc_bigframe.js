// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[4] = ["deep small-ok"];
__EXP[5] = ["deep RangeError"];
__REQ = {"dynajs": {"4": 1}, "node": {"5": 1}};
// WHITELIST-DIVERGE: deep-tail-recursion (node RangeError is acceptable at depth; see fallback note)
// P6: tail call to a function whose frame is LARGER than the caller's (many
// locals + args) — exercises the trampoline capacity fallback (documented:
// oversized callee frames fall back to C recursion).
var big;
eval("function bigFn(n) {\n" + (function () {
  var ls = [];
  for (var i = 0; i < 200; i++) ls.push("var l" + i + " = " + i + ";");
  return ls.join("\n");
})() + "\nif (n === 0) return 'big-ok' + (l0 + l199);\nreturn smallFn(n - 1);\n}");
function smallFn(n) {
  if (n === 0) return "small-ok";
  return bigFn(n - 1);
}
big = bigFn;
__A("p06_ptc_bigframe.js:a", function () { assert_eq(smallFn(3), "big-ok199", "a"); });
__A("p06_ptc_bigframe.js:b", function () { assert_eq(bigFn(4), "big-ok199", "b"); });
try { __A("p06_ptc_bigframe.js:mid", function () { assert_eq(smallFn(100), "small-ok", "mid"); }); } catch (e) { __L(3, "mid", e.constructor.name); }
try { __L(4, "deep", smallFn(200000)); } catch (e) { __L(5, "deep", e.constructor.name); }
// monotonic growth chain: every callee larger than the caller (no reuse at all)
var parts = ["function grow1(n){ if(n===0) return 'g1'; return grow2(n-1); }",
"function grow2(n){ var a0=0,a1=1,a2=2; if(n===0) return 'g2'; return grow3(n-1); }"];
for (var k = 3; k <= 12; k++) {
  var decls = [];
  for (var j = 0; j < k * 15; j++) decls.push("var b" + k + "_" + j + "=" + j + ";");
  parts.push("function grow" + k + "(n){ " + decls.join(" ") + (k < 12
    ? " if(n===0) return 'g" + k + "'; return grow" + (k + 1) + "(n-1); }"
    : " return n===0 ? 'g12' : grow1(n-1); }"));
}
eval(parts.join("\n"));
__A("p06_ptc_bigframe.js:c", function () { assert_eq(grow1(3), "g4", "c"); });
try { __A("p06_ptc_bigframe.js:mid2", function () { assert_eq(grow1(50), "g3", "mid2"); }); } catch (e) { __L(8, "mid2", e.constructor.name); }

summary("parser_core_ext");
