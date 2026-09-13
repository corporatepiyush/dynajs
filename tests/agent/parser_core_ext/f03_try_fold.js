// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[7] = ["t4 42 boom2"];
// F3: folding inside try/catch/finally; folded values crossing exception edges;
// catch-param shadowing a folded-constant-named outer binding.
function t1() {
  try {
    var v = 3 * 4 + (1 << 3);
    throw v;
  } catch (e) {
    __A("f03_try_fold.js:c", function () { assert_eq(e + (10 - 10), 20, "c"); });
  } finally {
    __A("f03_try_fold.js:f", function () { assert_eq("a" + "b" === "ab", true, "f"); });
  }
  return (5 + 5) * 2;
}
__A("f03_try_fold.js:r", function () { assert_eq(t1(), 20, "r"); });
function t2() { try { return 2 + 3; } finally { return 4 + 5; } }
__A("f03_try_fold.js:t2", function () { assert_eq(t2(), 9, "t2"); });
const e = 7;
try { null.x; } catch (e) { __A("f03_try_fold.js:shadow", function () { assert_eq(e.constructor.name, "TypeError", "shadow"); }); }
__A("f03_try_fold.js:outer", function () { assert_eq(e * (2 + 3), 35, "outer"); });
// finally that mutates a folded-initialized local
function t3() {
  var out = [];
  for (var i = 0; i < 3; i++) {
    try {
      if (i === 1) { out.push("skip" + (2 + 2)); continue; }
      out.push("b" + i);
    } finally {
      out.push("F" + i);
    }
  }
  return out.join(",");
}
__A("f03_try_fold.js:t3", function () { assert_eq(t3(), "b0,F0,skip4,F1,b2,F2", "t3"); });
// throw a folded constant object, catch, inspect
try { throw { code: 40 + 2, msg: "boom" + (1 + 1) }; }
catch (err) { __L(7, "t4", err.code, err.msg); }

summary("parser_core_ext");
