// Edge indices and argument values through fused charCodeAt / indexOf shapes.
var MF = MFHarness();

function cc(s, i) { var r = s.charCodeAt(i); return r; }

// NaN / edge indices
MF.near("cc-neg", cc("abc", -1), NaN);
MF.eq("cc-zero", cc("abc", 0), 97);
MF.eq("cc-last", cc("abc", 2), 99);
MF.near("cc-oob", cc("abc", 3), NaN);
MF.near("cc-big", cc("abc", 2147483647), NaN);
MF.near("cc-huge", cc("abc", 4294967295), NaN);
MF.near("cc-neg-big", cc("abc", -2147483648), NaN);
MF.eq("cc-float", cc("abc", 1.5), 98);     // ToInteger truncates toward zero
MF.eq("cc-nan", cc("abc", NaN), 97);       // NaN -> 0
MF.near("cc-inf", cc("abc", Infinity), NaN);
MF.near("cc-neg-inf", cc("abc", -Infinity), NaN);
MF.eq("cc-str-arg", cc("abc", "1"), 98);   // string coerced
MF.eq("cc-undef-arg", cc("abc", undefined), 97);
MF.eq("cc-null-arg", cc("abc", null), 97); // null -> 0
MF.eq("cc-true-arg", cc("abc", true), 98); // true -> 1
MF.near("cc-empty-str", cc("", 0), NaN);
MF.eq("cc-nonstr-this", (function () { try { return cc(5, 0); } catch (e) { return "T"; } })(), "T");

// float-literal arg -> push_const cpool form -> OP2_call_method1_const
function ccConst(s) { var r = s.charCodeAt(2.9); return r; }
MF.near("cc-const-form", ccConst("abc"), 99);

// fractional / negative through the fused imm paths
function startsWithAt(s, p) { var r = s.startsWith(p, 1.9); return r; }
MF.eq("startsWith-pos-coerce", startsWithAt("abcd", "b"), true);
MF.eq("startsWith-pos-coerce-miss", startsWithAt("acd", "b"), false);

// eval-order proof: arg evaluated after the method load
var evorder = [];
function loadCount(o, name) {
  Object.defineProperty(o, name, {
    get: function () { evorder.push("load"); return function (x) { evorder.push("arg" + x); return x; }; },
    configurable: true
  });
}
var eo = {};
loadCount(eo, "m");
function eoCall(o, v) { var r = o.m(v); return r; }
eoCall(eo, 9);
MF.eq("load-before-arg", evorder.join(","), "load,arg9");

// exceptions from inside the callee propagate with `this` intact
var calleeThis = null;
function throwsThis(o) { var r = o.m(); return r; }
var thrower = {
  m: function () { "use strict"; calleeThis = this; throw new Error("callee"); }
};
var caught = null;
try { throwsThis(thrower); } catch (e) { caught = e; }
MF.check("callee-throw-propagates", caught instanceof Error);
MF.eq("callee-throw-this", calleeThis === thrower, true);

// getter throwing during the load (cur_pc/backtrace path)
var gThrows = {};
Object.defineProperty(gThrows, "m", {
  get: function () { throw new TypeError("getter"); },
  configurable: true
});
function gThrowCall(o) { var r = o.m(); return r; }
caught = null;
try { gThrowCall(gThrows); } catch (e) { caught = e; }
MF.check("getter-throw", caught instanceof TypeError);

// recursion through a fused (non-tail) call: stack growth is bounded and safe
function rec(n) { if (n <= 0) return 0; var o = { m: rec }; return o.m(n - 1) + 1; }
MF.eq("fused-recursion", rec(300), 300);

// fused call result feeding further fused calls
function chained(s) {
  var a = s.toUpperCase();
  var b = a.charAt(0);
  return b.charCodeAt(0);
}
MF.eq("chained-fused", chained("abc"), 65);

// arg is the result of another fused call
function nestedArg(s) {
  var o = { m: function (x) { return x + 2; } };
  var j = s.charCodeAt(0) - 97;
  var r = o.m(j);
  return r;
}
MF.eq("nested-arg", nestedArg("abc"), 2);

// label between load and call (branch in receiver expression) still exact
function branchRecv(c) {
  var o = c ? { m: function () { return "t"; } } : { m: function () { return "f"; } };
  var r = o.m();
  return r;
}
MF.eq("branch-recv-true", branchRecv(1), "t");
MF.eq("branch-recv-false", branchRecv(0), "f");

// try/catch around a fused call
function inTry(s) {
  try { return s.charCodeAt(99); } catch (e) { return "E"; }
}
MF.near("fused-in-try", inTry("abc"), NaN);

MF.sum("mf_edges");
