var out = (typeof print === 'function') ? print : console.log;
function show(name, fn) {
  try { out(name + " => " + JSON.stringify(fn())); }
  catch (e) { out(name + " !! " + (e && e.name)); }  /* error MESSAGE wording is engine-specific (V8 vs this engine); behavior parity = error NAME + flow */
}
function range(n) { var a = new Array(n); for (var i = 0; i < n; i++) a[i] = i; return a; }
var canTransfer = typeof ArrayBuffer.prototype.transfer === "function";

/* ---- Array.from: dense / holey / empty ---- */
show("from.dense", function () { return Array.from(range(5)); });
show("from.dense.identity", function () { var r = Array.from(range(3)); return Array.isArray(r) + "," + (Object.getPrototypeOf(r) === Array.prototype) + "," + r.length; });
show("from.empty", function () { return Array.from([]); });
show("from.holey", function () { var a = [1,,3]; return [a.length, Array.from(a).join(","), Array.from(a).length]; });
show("from.holey.proto", function () { var a = [,,]; a.length = 3; Array.prototype[1] = "P1"; try { return Array.from(a).join("|"); } finally { delete Array.prototype[1]; } });
show("from.dense.shrink-map", function () { var a = [1,2,3,4,5]; return Array.from(a, function (v, k) { if (k === 1) a.length = 2; return v * 10; }).join("|"); });
show("from.dense.grow-map", function () { var a = [1,2,3]; return Array.from(a, function (v, k) { if (k === 1) a.length = 6; return v * 10; }).join("|"); });
show("from.dense.mutate-elems-map", function () { var a = [1,2,3]; return Array.from(a, function (v, k) { if (k === 0) a[1] = 100; return v; }).join("|"); });
show("from.dense.mapfn-grows-length-once", function () { var a = [1,2,3]; return Array.from(a, function (v, k) { if (k === 1) a[9] = 4; return v; }).join("|"); });

/* ---- Array.from: typed sources ---- */
show("from.u8", function () { return Array.from(new Uint8Array([3,1,2])); });
show("from.f64", function () { return Array.from(new Float64Array([1.5, -0, NaN, Infinity])); });
show("from.i32", function () { return Array.from(new Int32Array([-2147483648, 2147483647])); });
show("from.bi64", function () { return Array.from(new BigInt64Array([1n, -2n])); });
show("from.u8.map", function () { return Array.from(new Uint8Array([1,2,3]), function (v, k) { return v + k; }); });
show("from.u8.detached", function () {
  var u = new Uint8Array(4);
  if (!canTransfer) return "skip";
  u.buffer.transfer();
  return Array.from(u);
});
show("from.u8.detached.map", function () {
  var u = new Uint8Array(4);
  if (!canTransfer) return "skip";
  return Array.from(u, function (v, k) { if (k === 1) u.buffer.transfer(); return v; });
});

/* ---- Array.from: subclass / ctor this ---- */
show("from.subclass", function () {
  class Sub extends Array {}
  var r = Array.from.call(Sub, range(4));
  return [r instanceof Sub, r.constructor === Sub, r.join(","), r.length, Array.isArray(r)];
});
show("from.subclass.capture", function () {
  var captured = null, saw = [];
  class Sub extends Array {
    constructor(...args) { super(...args); captured = this; }
  }
  var src = [1,2,3];
  var r = Array.from.call(Sub, src, function (v, k) { saw.push(captured.length + ":" + captured.join(",")); return v; });
  return [r === captured, saw.join(" "), r.join(",")];
});
show("from.subclass.prefilled", function () {
  class Weird extends Array {}
  var pre = [9,9];
  Weird.prototype.constructor = Weird;
  var r = Array.from.call(function () { return pre; }, [1,2,3]);
  return [r === pre, pre.join(",")];
});
show("from.subarray.species", function () {
  var u = new Uint8Array([1,2,3]);
  var r = Uint8Array.from(u);
  return [r instanceof Uint8Array, r.length, r.join(",")];
});

/* ---- iterator with side effects / custom iterators stay generic ---- */
show("from.custom-iterator", function () {
  var calls = 0;
  var obj = {};
  obj[Symbol.iterator] = function () { calls++; var i = 0; return { next: function () { calls++; return i < 3 ? { value: i++, done: false } : { value: undefined, done: true }; } }; };
  var r = Array.from(obj);
  return [calls, r.join(",")];
});
show("from.generator", function () { function* g() { yield 1; yield* [2,3]; } return Array.from(g()).join(","); });
show("from.set", function () { return Array.from(new Set([1,2,2,3])).join("|"); });
show("from.map", function () { return Array.from(new Map([[1,"a"],[2,"b"]]), function (e) { return e[0] + e[1]; }).join("|"); });
show("from.string", function () { return Array.from("héllo").join("|"); });
show("from.string.map", function () { return Array.from("abc", function (v) { return v.toUpperCase(); }).join(""); });
show("from.arraylike", function () { return Array.from({0: "a", 1: "b", length: 2}).join("|"); });
show("from.arraylike.long", function () { return Array.from({length: 3}).join("|"); });
show("from.arraylike.map-mutate", function () {
  var o = {0: 1, 1: 2, 2: 3, length: 3};
  return Array.from(o, function (v, k) { if (k === 0) { o.length = 1; o[2] = 99; } return v; }).join("|");
});
show("from.arguments", function () { (function () { return Array.from(arguments); }).call(null, 7, 8); return "n/a"; });

/* ---- mapFn behaviors ---- */
show("from.mapfn.throw", function () {
  var a = [1,2,3], log = [];
  try { Array.from(a, function (v, k) { log.push(k); if (k === 1) throw new RangeError("boom"); return v; }); }
  catch (e) { log.push(e.name); }
  return log.join(",");
});
show("from.mapfn.throw.proto-return", function () {
  var a = [1,2,3], log = [];
  Object.getPrototypeOf([][Symbol.iterator]()).return = function () { log.push("ret"); return {value: 0, done: true}; };
  try { Array.from(a, function (v, k) { if (k === 1) throw new RangeError("boom"); return v; }); }
  catch (e) { log.push(e.name); }
  finally { delete Object.getPrototypeOf([][Symbol.iterator]()).return; }
  return log.join(",");
});
show("from.mapfn.throw.obj-proto-return", function () {
  var a = [1,2,3], log = [];
  Object.prototype.return = function () { log.push("ret2"); return {value: 0, done: true}; };
  try { Array.from(a, function (v, k) { if (k === 1) throw new RangeError("boom"); return v; }); }
  catch (e) { log.push(e.name); }
  finally { delete Object.prototype.return; }
  return log.join(",");
});
show("from.thisarg", function () { return Array.from([1,2], function (v) { return v * this.m; }, {m: 5}).join(","); });
show("from.mapfn-undefined", function () { return Array.from([1,2], undefined).join(","); });
show("from.mapfn-null", function () { return Array.from([1,2], null); });
show("from.mapfn-noncallable", function () { return Array.from([1,2], 42); });
show("from.mapfn.arity-and-order", function () { var log = []; Array.from([10,20], function () { log.push(arguments.length + ":" + arguments[0] + "@" + arguments[1]); return 0; }); return log.join(" "); });

/* ---- iterator result-object behaviors on explicit iterators ---- */
show("iter.keys-entries", function () {
  var a = ["x","y"];
  var k = a.keys(), e = a.entries();
  var ks = [], es = [];
  for (var o = k.next(); !o.done; o = k.next()) ks.push(o.value);
  for (var o2 = e.next(); !o2.done; o2 = e.next()) es.push(o2.value);
  return [ks.join(","), JSON.stringify(es)];
});
show("iter.result-shape", function () {
  var it = [1].values();
  var res = it.next();
  return [Object.keys(res).join(","), JSON.stringify(res), Object.getPrototypeOf(res) === Object.prototype,
          Object.getOwnPropertyDescriptor(res, "value").writable,
          Object.getOwnPropertyDescriptor(res, "done").enumerable];
});
show("iter.fresh-identity", function () {
  var it = [1,2].values();
  var r1 = it.next(), r2 = it.next();
  return [r1 !== r2, r1.value, r2.value, r1.done, r2.done, it.next().done];
});
show("iter.mutation-independence", function () {
  var it = ["a"].values();
  var r1 = it.next();
  r1.extra = 42;
  r1.value = "z";
  var r2 = it.next();
  return [r1.value, r1.extra, r2.value, r2.extra === undefined];
});
show("iter.break-resume", function () {
  var it = [1,2,3].values();
  it.next();
  var half = it.next();
  var again = it.next();
  return [half.value, again.value, it.next().done, it.next().done];
});
show("iter.iterate-while-mutating", function () {
  var a = [1,2,3,4];
  var seen = [];
  for (var v of a) { seen.push(v); if (v === 1) { a[3] = 40; a.push(5); } if (seen.length > 8) break; }
  return seen.join(",");
});
show("iter.shrink-during-forof", function () {
  var a = [1,2,3,4];
  var seen = [];
  for (var v of a) { seen.push(v); if (v === 2) a.length = 2; }
  return seen.join(",");
});
show("iter.keys.tampered-proto-next", function () {
  var log = [];
  var ap = Object.getPrototypeOf([][Symbol.iterator]());
  var orig = ap.next;
  ap.next = function () { log.push("impostor"); return orig.call(this); };
  try { return [Array.from([1,2]).join(","), log.length].join(" "); }
  finally { ap.next = orig; }
});
show("iter.exhausted-then-length", function () {
  var a = [1];
  var it = a.values();
  it.next(); it.next();
  return [it.next().done, it.next().value];
});

/* ---- %TypedArray%.from ---- */
show("ta.from.u8", function () { return Array.prototype.slice.call(Uint8Array.from(new Uint8Array([1,2,3]))).join(","); });
show("ta.from.u8.self", function () { var u = new Uint8Array([5,6,7]); var r = Uint8Array.from(u); return [r !== u, Array.prototype.join.call(r, ",")].join(" "); });
show("ta.from.f64", function () { var r = Float64Array.from(new Float64Array([1.25, -2.5])); return [r.constructor.name, r[0], r[1]].join(" "); });
show("ta.from.u8-to-f64", function () { var r = Float64Array.from(new Uint8Array([1,2,3])); return Array.prototype.join.call(r, ","); });
show("ta.from.f64-to-u8", function () { var r = Uint8Array.from(new Float64Array([1.9, 255.5, -3.5, NaN])); return Array.prototype.join.call(r, ","); });
show("ta.from.i16-to-u8", function () { var r = Uint8Array.from(new Int16Array([-1, 256, 257])); return Array.prototype.join.call(r, ","); });
show("ta.from.bi64-to-bi64", function () { var r = BigInt64Array.from(new BigInt64Array([-5n, 9n])); return [r[0], r[1]].join(" "); });
show("ta.from.bi64-to-u8", function () { return Uint8Array.from(new BigInt64Array([1n])); });
show("ta.from.bi64-to-f64", function () { return Float64Array.from(new BigInt64Array([1n])); });
show("ta.from.dense", function () { var r = Uint8Array.from([1,2,300,-1]); return Array.prototype.join.call(r, ","); });
show("ta.from.dense-map", function () { var r = Uint8Array.from([1,2,3], function (v, k) { return v + k; }); return Array.prototype.join.call(r, ","); });
show("ta.from.map-throw", function () {
  var log = [];
  try { Uint8Array.from([1,2,3], function (v, k) { if (k === 1) throw new TypeError("mid"); return v; }); }
  catch (e) { log.push(e.name); }
  return log.join(",");
});
show("ta.from.map-mutate-src", function () {
  var u = new Uint8Array([1,2,3]);
  var r = Uint8Array.from(u, function (v, k) { u[k] = 9; return v; });
  return Array.prototype.join.call(r, ",");
});
show("ta.from.subclass", function () {
  class T extends Uint8Array {}
  var r = T.from([1,2]);
  return [r instanceof T, Array.prototype.join.call(r, ",")].join(" ");
});
show("ta.from.small-ctor", function () {
  return Uint8Array.from.call(function (len) { return new Uint8Array(1); }, [1,2,3]);
});
show("ta.from.set", function () { return Array.prototype.join.call(Uint8Array.from(new Set([1,2,3])), ","); });
show("ta.from.string", function () { return Uint8Array.from("ab"); });
show("ta.from.u8.detached", function () {
  var u = new Uint8Array(3);
  if (!canTransfer) return "skip";
  u.buffer.transfer();
  return Uint8Array.from(u);
});
show("ta.from.u8.map-detach-mid", function () {
  var u = new Uint8Array(3);
  if (!canTransfer) return "skip";
  return Uint8Array.from(u, function (v, k) { if (k === 1) u.buffer.transfer(); return v; });
});
show("ta.from.ctor-mutates-src", function () {
  var src = [1,2,3];
  var seen;
  var r = Uint8Array.from.call(function (len) { seen = src.slice(); src[0] = 99; return new Uint8Array(len); }, src);
  return [seen.join(","), Array.prototype.join.call(r, ",")].join(" ");
});
show("ta.from.iterable-proxy", function () {
  var p = new Proxy([1,2,3], {});
  var r = Uint8Array.from(p);
  return Array.prototype.join.call(r, ",");
});

out("DIFF_A_END");
