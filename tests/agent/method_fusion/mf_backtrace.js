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

function deep3() {
  var o = { m: calleeThrow };
  var r = o.m();
  return r;
}
var l3c = 0;
try { deep3(); } catch (err) { l3c = lineOf(err.stack, 2); }
CHK("third-frame-line>0", l3c > 0);

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

function f5() {
  var o = { m: function (x) { return x + 0; } };
  var bad = { valueOf: function () { throw new Error("arg"); } };
  var r = o.m(bad);
  return r;
}
var l5 = 0;
try { f5(); } catch (err) { l5 = lineOf(err.stack, 1); }
CHK("arg-throw-caller-line>0", l5 > 0);

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

function f7() {
  var ref = new Error();
  throw new Error("plain");
}
var l7 = 0;
try { f7(); } catch (err) { l7 = lineOf(err.stack, 0); }
CHK("plain-throw-line>0", l7 > 0);

console.log("mf_backtrace probes=" + MF_N, "failed=" + MF_BAD);
