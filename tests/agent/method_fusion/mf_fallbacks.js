// No-fold shapes must keep EXACT generic semantics: overrides are called,
// side effects keep their order, extra args stay generic, PTC still works.
var MF = MFHarness();

// 1. custom String.prototype.charCodeAt override MUST be called (proto walk,
//    not the builtin fast path — the load is get_field2, the callee is a JS
//    function so the fused op still routes through JS_CallInternal).
var overrideThis = [];
String.prototype.charCodeAt = function (i) {
  overrideThis.push(typeof this);
  return 65 + i;
};
function fusedOverride(s, i) { var r = s.charCodeAt(i); return r; }
MF.eq("override-called", fusedOverride("xy", 1), 66);
MF.eq("override-primitive-this", overrideThis.join(","), "object");
String.prototype.charCodeAt = undefined; // poison: fused load must respect it
function fusedPoisoned(s, i) { var r = s.charCodeAt(i); return r; }
var threw = 0;
try { fusedPoisoned("xy", 0); } catch (e) { threw = (e instanceof TypeError) ? 1 : 2; }
MF.eq("poisoned-typeof-check", threw, 1);
delete String.prototype.charCodeAt; // restore builtin

// 2. getter on the method name: loaded exactly once, receiver = the object.
var getterLoads = 0, seenReceiver = null;
var gobj = {};
Object.defineProperty(gobj, "m", {
  get: function () { getterLoads++; seenReceiver = this; return function (x) { return (x || 0) + 100; }; },
  enumerable: true, configurable: true
});
function fusedGetter(o, i) { var r = o.m(i); return r; }
MF.eq("getter-once", fusedGetter(gobj, 5), 105);
MF.eq("getter-load-count", getterLoads, 1);
MF.eq("getter-receiver", seenReceiver === gobj, true);

// 3. argument with a valueOf side effect (throws inside the arg eval).
var order = [];
var voObj = { valueOf: function () { order.push("valueOf"); return 7; } };
function fusedVo(o, a) { var r = o.m(a); return r; }
fusedVo({ m: function (x) { order.push("call"); return x + 0; } }, voObj); // +0 forces valueOf
MF.eq("valueOf-order", order.join(","), "call,valueOf");
var thrower = { valueOf: function () { throw new RangeError("boom"); } };
function fusedVoThrow(o, a) { var r = o.m(a); return r; }
var caught = null;
try { fusedVoThrow({ m: function (x) { return x + 0; } }, thrower); }
catch (e) { caught = e; }
MF.check("valueOf-throw", caught instanceof RangeError);

// 4. extra args (>= 2): generic path, exact arity + order.
var seen = [];
function two3(o) { var r = o.m(1, 2, 3); return r; }
MF.eq("3-args", two3({ m: function () { seen.push(arguments.length); return 9; } }), 9);
MF.eq("3-args-arity", seen[0], 3);
function twoArgsOrder(o) {
  var r = o.m(log(1), log(2));
  return r;
}
function log(x) { order.push("arg" + x); return x; }
order = [];
MF.eq("2-args-order", twoArgsOrder({ m: function (a, b) { return a * 10 + b; } }), 12);
MF.eq("2-args-order-log", order.join(","), "arg1,arg2");

// 5. tail-call position: `return o.m(x)` compiles to tail_call_method, never
//    folded; proper tail calls must still not grow the C stack.
function ptcLoop(n, acc) { if (n === 0) return acc; return ptcLoop(n - 1, acc + n); }
MF.eq("direct-ptc", ptcLoop(5000, 0), 12502500);
var ptcTarget = { m: function (n, a) { if (n === 0) return a; return ptcTarget.m(n - 1, a + n); } };
MF.eq("method-ptc", ptcTarget.m(5000, 0), 12502500);

// 6. closure-var argument (get_var_ref stays generic, exact value).
var closureV = 1234;
function fusedClosure(o) { var r = o.m(closureV); return r; }
MF.eq("get_var_ref-arg", fusedClosure({ m: function (x) { return x + 1; } }), 1235);
closureV = 5;
MF.eq("get_var_ref-live", fusedClosure({ m: function (x) { return x + 1; } }), 6);

// 7. logical/branch argument: window contains a label -> no fold, exact value.
function condArg(o, c) { var r = o.m(c > 0 ? 10 : 20); return r; }
MF.eq("ternary-arg-true", condArg({ m: function (x) { return x; } }, 1), 10);
MF.eq("ternary-arg-false", condArg({ m: function (x) { return x; } }, -1), 20);

// 8. spread / optional-chain receivers stay on the generic path.
function spreadCall(o, a, b) { return o.m.apply(o, [a, b]); }
MF.eq("apply-generic", spreadCall({ m: function (a, b) { return a + b; } }, 3, 4), 7);

// 9. deleted method -> undefined -> TypeError (typeof-function check).
var del = { m: function () { return 1; } };
function fusedDel(o) { var r = o.m(); return r; }
fusedDel(del);
delete del.m;
var delErr = 0;
try { fusedDel(del); } catch (e) { delErr = e instanceof TypeError ? 1 : 2; }
MF.eq("deleted-method-TypeError", delErr, 1);

// 10. method value replaced between load and call is impossible in fused code
//     (single load), but a plain data property works: o.m = number -> TypeError.
var numM = { m: 5 };
function fusedNumM(o) { var r = o.m(); return r; }
var numErr = 0;
try { fusedNumM(numM); } catch (e) { numErr = e instanceof TypeError ? 1 : 2; }
MF.eq("non-function-TypeError", numErr, 1);

MF.sum("mf_fallbacks");
