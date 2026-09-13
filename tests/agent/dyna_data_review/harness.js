// review harness (self-contained, independent of implementer files)
var __pass = 0, __fail = 0, __case = 0;
export function eq(got, want, name) {
  __case++;
  var g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { __pass++; }
  else { __fail++; print("FAIL [" + name + "] got=" + g + " want=" + w); }
}
export function ok(cond, name) { __case++; if (cond) __pass++; else { __fail++; print("FAIL [" + name + "]"); } }
export function throws(fn, etype, name) {
  __case++;
  try { fn(); __fail++; print("FAIL [" + name + "] did not throw"); }
  catch (e) {
    var ctor = etype === "RangeError" ? RangeError :
               etype === "TypeError" ? TypeError :
               etype === "SyntaxError" ? SyntaxError : Error;
    if (etype && !(e instanceof ctor)) { __fail++; print("FAIL [" + name + "] wrong type: " + e); }
    else __pass++;
  }
}
export function done(tag) {
  print("SUMMARY " + tag + " cases=" + __case + " pass=" + __pass + " fail=" + __fail);
  print("RESULT " + (__fail === 0 ? "PASS" : "FAIL"));
  if (__fail) throw new Error(tag + " had failures");
}
