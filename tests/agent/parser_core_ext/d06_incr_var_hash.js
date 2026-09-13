// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// D6: incremental var_hash growth — a function's var set grows across
// sequential direct evals in a loop (2500 vars added in 5 rounds); hash must
// stay consistent for reads via both direct name lookup and eval lookup.
function grow() {
  var acc = [];
  for (var r = 0; r < 5; r++) {
    var lines = [];
    for (var i = 0; i < 500; i++) lines.push("var g_" + r + "_" + i + "=" + (r * 500 + i) + ";");
    eval(lines.join(""));
    acc.push(eval("g_" + r + "_499"));
    acc.push(g_0_0);
  }
  return acc.join(",") + "|" + (g_0_0 + g_4_499);
}
__A("d06_incr_var_hash.js:a", function () { assert_eq(grow(), "499,0,999,0,1499,0,1999,0,2499,0|2499", "a"); });
// names added AFTER closures were created stay visible through the closures
function grow2() {
  var fs = [];
  fs.push(function () { return late1; });
  eval("var late1 = 111;");
  fs.push(function () { return late1; });
  eval("var late1 = 222; var late2 = 333;");
  return fs.map(f => f()).join(",") + "|" + late2;
}
console.log("b", grow2());
// growth interleaved with shadow reads and a same-name redeclare in a later eval
function grow3() {
  eval("var sh = 1;");
  var out = [sh];
  eval("var sh = 2; var deep = function(){ return sh; };");
  out.push(sh, deep());
  eval("var sh = 3;");
  out.push(sh, deep());
  return out.join(",");
}
console.log("c", grow3());

summary("parser_core_ext");
