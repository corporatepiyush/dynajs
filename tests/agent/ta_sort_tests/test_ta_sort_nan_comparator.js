var out = (typeof print === 'function') ? print : console.log;
var fails = 0, runs = 0;

function eq(actual, expected, label) {
  runs++;
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    fails++;
    out("FAIL " + label + ": got " + JSON.stringify(actual) + ", want " + JSON.stringify(expected));
  }
}

function f64(a) { var t = new Float64Array(a); return t; }
function arr(t) { return Array.prototype.slice.call(t); }
function isNaNv(v) { return v !== v; }

(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function (a, b) {
    var an = isNaNv(a), bn = isNaNv(b);
    if (an && bn) return 0;
    if (an) return -1;
    if (bn) return 1;
    return a < b ? -1 : (a > b ? 1 : 0);
  });
  eq(arr(t), [NaN, -1, 0, 2, 3.5], "nanFirst comparator on [3.5,NaN,-1,2,0]");
})();

(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function (a, b) { return a - b; });
  eq(arr(t), [3.5, NaN, -1, 0, 2], "x-y comparator on [3.5,NaN,-1,2,0] (NaN stays at index 1)");

  var t2 = f64([1, NaN, 2]);
  t2.sort(function (a, b) { return a - b; });
  eq(arr(t2), [1, NaN, 2], "x-y comparator on [1,NaN,2] (NaN stays at index 1)");

  var t3 = f64([NaN, NaN, 1]);
  t3.sort(function (a, b) { return a - b; });
  eq(arr(t3), [NaN, NaN, 1], "x-y comparator on [NaN,NaN,1] (stability)");
})();

(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort();
  eq(arr(t), [-1, 0, 2, 3.5, NaN], "default sort on [3.5,NaN,-1,2,0] (NaN last)");

  var t2 = f64([NaN, NaN, 1]);
  t2.sort();
  eq(arr(t2), [1, NaN, NaN], "default sort on [NaN,NaN,1] (NaN last)");

  eq([3, 1, 2].sort(), [1, 2, 3], "plain default sort [3,1,2]");
  var t4 = f64([0, -0, 1, NaN]);
  t4.sort();
  eq(arr(t4).map(String), ["0", "0", "1", "NaN"], "default sort -0/+0/NaN");
})();

(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function () { return NaN; });
  eq(arr(t), [3.5, NaN, -1, 2, 0], "()=>NaN comparator keeps original order");
})();

(function () {
  var t = f64([3.5, NaN, -1, 2, 0]);
  t.sort(function () { return -1; });
  eq(arr(t), [3.5, NaN, -1, 2, 0], "()=>-1 comparator (control-exact)");

  var t2 = f64([3.5, NaN, -1, 2, 0]);
  t2.sort(function () { return 0; });
  eq(arr(t2), [3.5, NaN, -1, 2, 0], "()=>0 comparator (fully stable)");
})();

(function () {
  var t = f64([NaN, 1, 0]);
  var calls = 0;
  t.sort(function (a, b) { calls++; return a < b ? -1 : (a > b ? 1 : 0); });
  if (calls < 2) { fails++; out("FAIL call-count: comparator called " + calls + " times, expected >2 (NaN pairs must be consulted)"); }
  runs++;
  eq(arr(t).map(String), ["NaN", "0", "1"], "nan-aware comparator (stable for its own +0 on NaN pairs)");
})();

out((runs - fails) + "/" + runs + " passed" + (fails ? " -- FAILURES: " + fails : ""));
if (fails) throw new Error("ta_sort regression failures: " + fails);
