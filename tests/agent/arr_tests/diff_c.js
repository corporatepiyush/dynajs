var out = (typeof print === 'function') ? print : console.log;
function show(name, fn) {
  try { out(name + " => " + JSON.stringify(fn())); }
  catch (e) { out(name + " !! " + (e && e.name)); }
}
function range(n) { var a = new Array(n); for (var i = 0; i < n; i++) a[i] = i; return a; }
var canTransfer = typeof ArrayBuffer.prototype.transfer === "function";
var canRab = (function () { try { new ArrayBuffer(8, { maxByteLength: 16 }); return true; } catch (e) { return false; } })();

/* ctor mutates the SOURCE (iterator branch) */
show("adv.ctor-shrinks-src", function () {
  var src = range(6);
  class Sub extends Array { constructor(a) { super(a); src.length = 2; } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), src.length];
});
show("adv.ctor-slowifies-src", function () {
  var src = range(4);
  class Sub extends Array { constructor(a) { super(a); src[100000] = 1; } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), src.length];
});
show("adv.ctor-grows-src-length", function () {
  var src = range(3);
  class Sub extends Array { constructor(a) { super(a); src.length = 8; } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), src.length];
});
show("adv.ctor-pokes-proto-holes", function () {
  var src = range(3);
  class Sub extends Array { constructor(a) { super(a); Array.prototype[1] = "PP"; } }
  try { return Array.from.call(Sub, src).join("|"); }
  finally { delete Array.prototype[1]; }
});
show("adv.ctor-deletes-iterator-next", function () {
  var proto = Object.getPrototypeOf([][Symbol.iterator]());
  var orig = proto.next;
  class Sub extends Array { constructor(a) { super(a); delete proto.next; } }
  try { var r = Array.from.call(Sub, range(3)); return ["done", r.length]; }
  catch (e) { return ["throw", e.name]; }
  finally { proto.next = orig; }
});
show("adv.ctor-deletes-iterator-next.unmapped", function () {
  /* same, but iterator protocol survives via restored proto: sanity */
  var proto = Object.getPrototypeOf([][Symbol.iterator]());
  return typeof proto.next;
});
show("adv.ctor-appends-to-src", function () {
  var src = range(3);
  class Sub extends Array { constructor(a) { super(a); src.push(77); } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(",")];
});

/* ctor mutates the source (array-like branch: @@iterator nulled) */
show("adv.al.ctor-shrinks", function () {
  var src = range(4);
  src[Symbol.iterator] = null;
  class Sub extends Array { constructor(a) { super(a); src.length = 2; } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), src.length];
});
show("adv.al.ctor-slowifies", function () {
  var src = range(4);
  src[Symbol.iterator] = null;
  class Sub extends Array { constructor(a) { super(a); Object.defineProperty(src, 1, { get: function () { return 41; } }); } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), Object.getOwnPropertyDescriptor(src, 1).get !== undefined];
});
show("adv.al.ctor-grows-dense", function () {
  var src = range(2);
  src[Symbol.iterator] = null;
  class Sub extends Array { constructor(a) { super(a); src[100] = 5; } }
  var r = Array.from.call(Sub, src);
  return [r.length, r.join(","), src.length];
});

/* typed sources vs ctor side effects */
show("adv.ta.ctor-detaches-src", function () {
  if (!canTransfer) return "skip";
  var u = new Uint8Array([1, 2, 3, 4]);
  class Sub extends Uint8Array { constructor(a) { super(4); u.buffer.transfer(); } }
  try { var r = Array.from.call(Sub, u); return ["ok", Array.prototype.join.call(r, ",")]; }
  catch (e) { return ["throw", e.name]; }
});
show("adv.ta.from.ctor-detaches-src", function () {
  if (!canTransfer) return "skip";
  var u = new Uint8Array([1, 2, 3, 4]);
  var calls = 0;
  var r = Uint8Array.from.call(function (len) { calls++; u.buffer.transfer(); return new Uint8Array(len); }, u);
  return [calls, Array.prototype.join.call(r, ",")];
});
show("adv.ta.from.rab-shrink-in-ctor", function () {
  if (!canRab) return "skip";
  var rab = new ArrayBuffer(16, { maxByteLength: 16 });
  var u = new Uint8Array(rab);
  for (var i = 0; i < 16; i++) u[i] = i;
  var r = Uint8Array.from.call(function (len) { rab.resize(4); return new Uint8Array(len); }, u);
  return [r.length, Array.prototype.join.call(r, ",")];
});
show("adv.ta.from.rab-shrink.map", function () {
  if (!canRab) return "skip";
  var rab = new ArrayBuffer(16, { maxByteLength: 16 });
  var u = new Uint8Array(rab);
  for (var i = 0; i < 16; i++) u[i] = i;
  var r = Uint8Array.from(u, function (v, k) { if (k === 1) rab.resize(4); return v; });
  return [r.length, Array.prototype.join.call(r, ",")];
});
show("adv.from.sab-source", function () {
  if (typeof SharedArrayBuffer !== "function") return "skip";
  var sab = new SharedArrayBuffer(4);
  var u = new Uint8Array(sab); u[0] = 9;
  return Array.prototype.join.call(Array.from(u), ",");
});
show("adv.ta.from.sab-source", function () {
  if (typeof SharedArrayBuffer !== "function") return "skip";
  var sab = new SharedArrayBuffer(4);
  var u = new Uint8Array(sab); u[0] = 9; u[3] = 3;
  return Array.prototype.join.call(Uint8Array.from(u), ",");
});
show("adv.from.rab-tracking", function () {
  if (!canRab) return "skip";
  var rab = new ArrayBuffer(8, { maxByteLength: 32 });
  var u = new Uint8Array(rab); u[5] = 5;
  return [Array.from(u).join(","), Array.from(u).length];
});
show("adv.from.rab-shrunk", function () {
  if (!canRab) return "skip";
  var rab = new ArrayBuffer(16, { maxByteLength: 16 });
  var u = new Uint8Array(rab); u[2] = 2;
  rab.resize(4);
  return [Array.from(u).join(","), Array.from(u).length];
});

/* GC churn across iterator results */
show("adv.gc-churn-results", function () {
  var a = range(2000);
  var kept = [];
  for (var round = 0; round < 30; round++) {
    var it = a.values();
    var n = 0;
    var r = it.next();
    while (!r.done) {
      if (n % 997 === 0) kept.push([r.value, r.done, "value" in r]);
      n++;
      r = it.next();
    }
  }
  return [kept.length, kept[0][0], kept[kept.length - 1][0], kept.every(function (k) { return k[1] === false && k[2] === true; })];
});
show("adv.gc-churn-from", function () {
  var a = range(500);
  var sum = 0;
  for (var i = 0; i < 500; i++) { var r = Array.from(a); sum += r.length; }
  return sum;
});

/* result objects under mutation engines' own ops */
show("adv.result-json-roundtrip", function () {
  var it = range(3).values();
  var acc = [];
  var r = it.next();
  while (!r.done) { acc.push(JSON.stringify(r)); r = it.next(); }
  return acc.join(" ");
});
show("adv.result-object-assign", function () {
  var r1 = [1].values().next();
  var r2 = [2].values().next();
  var merged = Object.assign({}, r1, r2);
  return [JSON.stringify(merged), Object.keys(merged).join(",")];
});
show("adv.result-spread-into-array", function () {
  var r = [7].values().next();
  return JSON.stringify([...Object.keys(r)]);
});
show("adv.mapfn-returns-mutated-src-elem", function () {
  var a = range(4);
  return Array.from(a, function (v, k) { a[0] = 999; return v; }).join(",");
});
out("DIFF_C_END");
