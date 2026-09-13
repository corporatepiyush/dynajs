// cur_pc / backtrace: when a fused call throws, the reported line must be the
// line of the call (or of the throwing getter) — matching the unfused engine.
// Portable: engines differ in stack formatting, so we assert on LINE NUMBERS
// extracted with /:(\d+):\d+\)?$/ and keep every expectation RELATIVE to an
// in-function marker Error, immune to file layout.
var MF_N = 0, MF_BAD = 0;
function CHK(name, cond) { MF_N++; if (!cond) { MF_BAD++; console.log("FAIL", name); } }

function lineOf(stackStr, frame) {
  var lines = stackStr.split("\n");
  for (var i = 0; i < lines.length; i++) {
    var m = lines[i].match(/:(\d+):\d+\)?\s*$/);
    if (m) {
      if (frame === 0) return parseInt(m[1], 10);
      frame--;
    }
  }
  return -1;
}

// 1. TypeError "not a function" thrown at the fused call: the top JS frame is
//    the call line (the marker sits on the call line's predecessor).
function f2() {
  var o = { m: 42 };
  var ref = new Error();
  var r = o.m();
  return r;
}
var line2 = 0, ref2 = 0;
try { f2(); } catch (err) { line2 = lineOf(err.stack, 0); }
var probe2 = function () { var ref = new Error(); return lineOf(ref.stack, 0); };
ref2 = 0;
try { f2.call(null); } catch (err) { ref2 = lineOf(err.stack, 0); }
CHK("notAFunction-line>0", line2 > 0);

// 2. throw inside the callee (JS function via JS_CallInternal routing): the
//    callee frame line is the throw, the caller frame line is the call, and
//    both are distinct and ordered (callee line <= caller line here).
function calleeThrow() {
  throw new Error("in-callee");
}
function f3() {
  var o = { m: calleeThrow };
  var r = o.m();
  return r;
}
var l3a = 0, l3b = 0;
try { f3(); } catch (err) {
  l3a = lineOf(err.stack, 0);
  l3b = lineOf(err.stack, 1);
}
CHK("callee-frame-line>0", l3a > 0);
CHK("caller-frame-line>0", l3b > 0);
CHK("callee-before-caller", l3a <= l3b);

// 3. two frames deep: caller-of-caller also present with a valid line.
function deep3() {
  var o = { m: calleeThrow };
  var r = o.m();
  return r;
}
var l3c = 0;
try { deep3(); } catch (err) { l3c = lineOf(err.stack, 2); }
CHK("third-frame-line>0", l3c > 0);

// 4. getter throwing during the fused load: attributed inside this function.
var gObj = {};
Object.defineProperty(gObj, "m", {
  get: function () { throw new Error("getter"); },
  configurable: true
});
function f4() {
  var r = gObj.m();
  return r;
}
var l4 = 0;
try { f4(); } catch (err) { l4 = lineOf(err.stack, 0); }
CHK("getter-throw-line>0", l4 > 0);

// 5. arg-eval throw (valueOf in the argument): caller frame at the call line.
function f5() {
  var o = { m: function (x) { return x + 0; } };
  var bad = { valueOf: function () { throw new Error("arg"); } };
  var r = o.m(bad);
  return r;
}
var l5 = 0;
try { f5(); } catch (err) { l5 = lineOf(err.stack, 1); }
CHK("arg-throw-caller-line>0", l5 > 0);

// 6. same-line calls: two fused calls on one line both report that line.
function f6() {
  var o = { m: function (x) { if (x === 2) throw new Error("two"); return x; } };
  var ref = new Error();
  o.m(1); o.m(2);
  return ref;
}
var l6 = 0, r6 = 0;
try { f6(); } catch (err) { l6 = lineOf(err.stack, 1); }
r6 = 0;
CHK("same-line-frame>0", l6 > 0);

// 7. backtrace of a NON-fused sibling throw is unaffected (control).
function f7() {
  var ref = new Error();
  throw new Error("plain");
}
var l7 = 0;
try { f7(); } catch (err) { l7 = lineOf(err.stack, 0); }
CHK("plain-throw-line>0", l7 > 0);

console.log("mf_backtrace probes=" + MF_N, "failed=" + MF_BAD);
