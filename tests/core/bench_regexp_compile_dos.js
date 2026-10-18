// Regression probe for regexp compile-time superlinear work (SEC-023).
// Reports the best wall time over 3 reps for three hostile-pattern shapes:
//   alt:  'a|' repeated N  (front-inserted split chain)
//   fwd:  '\k<x>' repeated N then a single named group (forward backref scan)
//   dup:  N distinct named groups (duplicate-name scan)
// A linear-time compiler keeps every shape's time bounded; a quadratic one
// explodes with N. Run on the engine under test; compare runs at N and 2N.
// timeout: 300

function best_ms(fn) {
  var best = Infinity;
  for (var r = 0; r < 3; r++) {
    var t0 = Date.now();
    fn();
    var dt = Date.now() - t0;
    if (dt < best) best = dt;
  }
  return best;
}

function check(name, fn) {
  var ms = best_ms(fn);
  console.log(name + "=" + ms + "ms");
  return ms;
}

var sizes = [20000, 40000, 80000];
for (var i = 0; i < sizes.length; i++) {
  var n = sizes[i];
  var alt_src = "a|".repeat(n) + "a";
  (function (src, n) {
    check("alt_" + n, function () { new RegExp(src); });
  })(alt_src, n);
}
for (var i = 0; i < sizes.length; i++) {
  var n = sizes[i] / 2;
  var fwd_src = "\\k<x>".repeat(n) + "(?<x>a)";
  (function (src, n) {
    check("fwd_" + n, function () { new RegExp(src); });
  })(fwd_src, n);
}
var dup_sizes = [64, 128, 200];
for (var i = 0; i < dup_sizes.length; i++) {
  var n = dup_sizes[i];
  var parts = [];
  for (var k = 0; k < n; k++) parts.push("(?<g" + k + ">)");
  var dup_src = parts.join("");
  (function (src, n) {
    check("dup_" + n, function () { new RegExp(src); });
  })(dup_src, n);
}
console.log("done");
