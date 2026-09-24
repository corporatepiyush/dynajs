var out = (typeof print === 'function') ? print : console.log;
function show(name, fn) {
  try { out(name + " => " + JSON.stringify(fn())); }
  catch (e) { out(name + " !! " + (e && e.name)); }
}
function range(n) { var a = new Array(n); for (var i = 0; i < n; i++) a[i] = i; return a; }

/* ---- result object shape / descriptors / identity ---- */
show("res.shape", function () {
  var r = [1].values().next();
  var dv = Object.getOwnPropertyDescriptor(r, "value");
  var dd = Object.getOwnPropertyDescriptor(r, "done");
  return [Object.keys(r).join(","), dv.value, dv.writable, dv.enumerable, dv.configurable,
          dd.value, dd.writable, dd.enumerable, dd.configurable,
          Object.getPrototypeOf(r) === Object.prototype,
          Object.getOwnPropertyNames(r).join(",")];
});
show("res.identity", function () {
  var it = range(3).values();
  var rs = [it.next(), it.next(), it.next(), it.next()];
  return [rs[0] !== rs[1], rs[1] !== rs[2], rs[2] !== rs[3],
          rs.map(function (r) { return r.value; }).join(","),
          rs.map(function (r) { return r.done; }).join(",")];
});
show("res.mutation", function () {
  var it = ["a","b"].values();
  var r1 = it.next();
  r1.value = "mutated"; r1.tag = "T1"; r1.done = "yes";
  var r2 = it.next();
  var r3 = it.next();
  return [JSON.stringify(r1), JSON.stringify(r2), r2.tag === undefined, r3.value, r3.done];
});
show("res.mutation2", function () {
  var it = [1,2].values();
  var r1 = it.next();
  r1.value = 99;
  return [r1.value, it.next().value];
});
show("res.delete-readd", function () {
  var it = [5].values();
  var r = it.next();
  delete r.value;
  r.value = 7;
  return [Object.keys(r).join(","), JSON.stringify(r)];
});
show("res.seal", function () {
  var it = [1].values();
  var r = it.next();
  Object.seal(r);
  r.done = true;
  return [JSON.stringify(r)];
});
show("res.preventext", function () {
  var it = [1].values();
  var r = it.next();
  Object.preventExtensions(r);
  try { r.extra = 1; return [JSON.stringify(r), "added"]; }
  catch (e) { return [JSON.stringify(r), "strict-blocked"]; }
});
show("res.defineprop", function () {
  var it = [1].values();
  var r = it.next();
  Object.defineProperty(r, "done", { writable: false });
  return [JSON.stringify(r), Object.getOwnPropertyDescriptor(r, "done").writable];
});
show("res.proto-mutate", function () {
  var r = [1].values().next();
  var before = JSON.stringify(r);
  Array.prototype.zzz = 1;
  try { return [before, r.zzz === 1]; } finally { delete Array.prototype.zzz; }
});
show("res.set-iterator", function () {
  var it = new Set([1,2]).values();
  var r1 = it.next(), r2 = it.next(), r3 = it.next();
  return [r1.value, r2.value, r3.done, Object.keys(r1).join(",")];
});
show("res.map-entries", function () {
  var it = new Map([["a",1]]).entries();
  var r = it.next();
  return [JSON.stringify(r.value), r.done];
});
show("res.set-entries-kind", function () {
  var it = new Set(["x"]).entries();
  var r = it.next();
  return [JSON.stringify(r.value), Object.keys(r).join(",")];
});
show("res.generator", function () {
  function* g() { yield 10; return 20; }
  var it = g();
  var a = it.next(), b = it.next(), c = it.next();
  return [JSON.stringify(a), JSON.stringify(b), JSON.stringify(c)];
});
show("res.regexp-string-iterator", function () {
  var it = "ab".matchAll(/a/g);
  var r = it.next();
  return [typeof r.value, r.done, Object.keys(r).join(",")];
});
show("res.json-order", function () {
  var r = [1].values().next();
  return JSON.stringify({ k: Object.keys(r), s: JSON.stringify(r) });
});
show("res.spread-keys-iterator", function () {
  return Array.from(range(4).keys()).join(",");
});
show("res.spread-entries-iterator", function () {
  return JSON.stringify(Array.from(range(2).entries()));
});
show("res.forof-entries", function () {
  var acc = [];
  for (var e of ["a","b"].entries()) acc.push(JSON.stringify(e));
  return acc.join(" ");
});
show("res.string-iterator-codepoints", function () {
  var acc = [];
  for (var c of "héllo") acc.push(c);
  return acc.join("|");
});
show("res.exhaustion-undefined", function () {
  var it = [9].values();
  it.next();
  var r = it.next();
  return [r.done, r.value, "value" in r, "done" in r];
});
show("res.typedarray-iterator-detach", function () {
  var u = new Uint8Array(3);
  if (!ArrayBuffer.prototype.transfer) return "skip";
  var it = u.values();
  var r1 = it.next();
  u.buffer.transfer();
  try { var r2 = it.next(); return [r1.value, r2.done]; }
  catch (e) { return [r1.value, e.name]; }
});
show("res.explicit-shrink", function () {
  var a = [1,2,3,4];
  var it = a.values();
  var out1 = [it.next().value];
  a.length = 2;
  var r = it.next();
  return [out1.join(","), r.value, r.done];
});
show("res.explicit-grow", function () {
  var a = [1];
  var it = a.values();
  var o1 = it.next();
  a.push(2, 3);
  var o2 = it.next(), o3 = it.next(), o4 = it.next();
  return [o1.value, o2.value, o3.value, o4.done];
});
show("res.holey-proto", function () {
  var a = [1,,3];
  Array.prototype[1] = "H";
  try { return Array.from(a.values()).join("|"); } finally { delete Array.prototype[1]; }
});
show("res.arguments-iterator", function () {
  function f() { return Array.from(arguments.values()); }
  return f(7,8).join(",");
});
show("res.generic-obj-iterator", function () {
  var o = { 0: "x", 1: "y", length: 2 };
  return Array.from(o[Symbol.iterator] === undefined ? { length: 0 } : o).length;
});
show("res.forof-map", function () {
  var m = new Map([[1,"a"],[2,"b"]]);
  var acc = [];
  for (var e of m) acc.push(JSON.stringify(e));
  return acc.join(" ");
});
show("res.forof-string", function () {
  var acc = [];
  for (var c of "ab") acc.push(c);
  return acc.join("+");
});
show("res.arrayfrom-uses-iterator-results", function () {
  var a = range(10);
  var it = a.values();
  var n = 0;
  for (var r = it.next(); !r.done; r = it.next()) n++;
  return n;
});
out("DIFF_B_END");
