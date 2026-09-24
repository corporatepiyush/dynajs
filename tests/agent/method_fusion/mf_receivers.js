// Receiver semantics: proto-chain walks, primitive receivers, exotics,
// frozen/sealed objects, subclassed arrays, `this` binding, getters/proxies.
var MF = MFHarness();

// 1. proto-chain method found above the receiver (inline walk, one lookup).
function protoCase(s, i) { var r = s.charAt(i); return r; }
MF.eq("proto-charAt", protoCase("abc", 1), "b");

function toFixed2(n) { var r = n.toFixed(2); return r; }
MF.eq("primitive-receiver-toFixed", toFixed2(5), "5.00");
MF.eq("primitive-receiver-str", "abc".toUpperCase(), "ABC");
MF.eq("primitive-receiver-inline-charAt", "hello".charCodeAt(1), 101);
MF.eq("primitive-receiver-number", (255).toString(16), "ff");

// 2. deep proto chain
var base = { m: function () { return "base"; } };
var mid = Object.create(base);
var leaf = Object.create(mid);
function deepProto(o) { var r = o.m(); return r; }
MF.eq("deep-proto", deepProto(leaf), "base");

// 3. own property shadows the proto method
var shadow = Object.create({ m: function () { return "proto"; } });
shadow.m = function () { return "own"; };
function shadowed(o) { var r = o.m(); return r; }
MF.eq("own-shadows-proto", shadowed(shadow), "own");

// 4. frozen / sealed receivers: reads unaffected.
var frozen = Object.freeze({ m: function () { return "frozen"; } });
function frozenCall(o) { var r = o.m(); return r; }
MF.eq("frozen-receiver", frozenCall(frozen), "frozen");
var sealed = Object.seal({ m: function () { return "sealed"; } });
MF.eq("sealed-receiver", (function (o) { var r = o.m(); return r; })(sealed), "sealed");
var frozenArr = Object.freeze([1, 2, 3]);
MF.eq("frozen-array-method", (function (a) { var r = a.indexOf(2); return r; })(frozenArr), 1);

// 5. subclassed Array: method found on Array.prototype, exotic receiver.
function SubArr() { var a = []; a.push.apply(a, arguments); return a; }
SubArr.prototype = Object.create(Array.prototype);
var sa = new SubArr(10, 20, 30);
function subMap(a) { var r = a.map(function (x) { return x + 1; }); return r.join(","); }
MF.eq("subclassed-array-map", subMap(sa), "11,21,31");

// plain array .map (the exotic-miss fix path)
MF.eq("array-map", [1, 2, 3].map(function (x) { return x * 2; }).join(","), "2,4,6");

// 6. `this` semantics: method call passes the receiver; the fused op must not
//    change it (strict and sloppy callees).
var thisCaptures = [];
var sloppy = { m: function () { thisCaptures.push(this); return 1; } };
function callSloppy(o) { var r = o.m(); return r; }
callSloppy(sloppy);
MF.eq("sloppy-this", thisCaptures[0] === sloppy, true);
var strictObj = { m: function () { "use strict"; thisCaptures.push(this); return 2; } };
callSloppy(strictObj);
MF.eq("strict-this", thisCaptures[1] === strictObj, true);
// primitive receiver with a strict callee: this stays the primitive
Object.defineProperty(Number.prototype, "whoami", {
  value: function () { "use strict"; thisCaptures.push(this); return typeof this; },
  configurable: true
});
MF.eq("prim-strict-this-typeof", (5).whoami(), "number");
delete Number.prototype.whoami;

// 7. getter along a proto chain (TMASK on an ancestor object).
var gHost = {};
var gVal = 0;
Object.defineProperty(gHost, "m", {
  get: function () { gVal++; return function () { return "g"; }; },
  configurable: true
});
var gChild = Object.create(gHost);
function protoGetter(o) { var r = o.m(); return r; }
MF.eq("proto-getter", protoGetter(gChild), "g");
MF.eq("proto-getter-count", gVal, 1);

// 8. missing method -> undefined -> TypeError, on every receiver class.
function missing(o) { var r = o.m(); return r; }
var missErr = 0;
try { missing({}); } catch (e) { missErr = e instanceof TypeError ? 1 : 2; }
MF.eq("missing-on-plain-obj", missErr, 1);
missErr = 0;
try { missing(null); } catch (e) { missErr = e instanceof TypeError ? 1 : 2; }
MF.eq("null-receiver", missErr, 1);
missErr = 0;
try { missing(undefined); } catch (e) { missErr = e instanceof TypeError ? 1 : 2; }
MF.eq("undefined-receiver", missErr, 1);

// 9. Array exotic: integer-named own method on an array (exotic + tagged int
//    atom must take the slow path and still work).
var exotic = [function () { return "idx0"; }];
function exoticCall(a) { var r = a[0](); return r; }
MF.eq("array-idx-call", exoticCall(exotic), "idx0");

// 10. proxy receiver: generic routing (no C fast path for proxies).
var pTarget = { m: function (x) { return x * 3; } };
var p = new Proxy(pTarget, {});
function proxyCall(o, v) { var r = o.m(v); return r; }
MF.eq("proxy-receiver", proxyCall(p, 4), 12);

// 11. bound function as the method value.
var bound = { m: function (x) { return (this.b || 0) + x; }.bind({ b: 100 }) };
function boundCall(o, v) { var r = o.m(v); return r; }
MF.eq("bound-method", boundCall(bound, 2), 102);

// 12. Math builtin class (get_var receiver + fused call).
MF.eq("Math.max", Math.max(2, 9), 9);
MF.eq("Math.floor", Math.floor(3.7), 3);

// 13. method value loaded from an accessor that returns different functions
//     per call: fused = exactly one load -> one getter call, one returned fn.
var loads = 0;
var single = {};
Object.defineProperty(single, "m", {
  get: function () { loads++; return function () { return loads; }; },
  configurable: true
});
function singleLoad(o) { var r = o.m(); return r; }
MF.eq("single-load-fn", singleLoad(single), 1);
MF.eq("single-load-again", singleLoad(single), 2);

MF.sum("mf_receivers");
