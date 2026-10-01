const out = console.log;
function dump(tag, a) {
  const parts = [];
  for (let i = 0; i < a.length; i++)
    parts.push(i + ":" + String(a[i]) + ":" + Object.prototype.hasOwnProperty.call(a, i));
  out(tag, parts.join(" "));
}
function withProto(p, fn) {
  const keys = Object.keys(p);
  try { fn(); } finally { for (const k of keys) delete Array.prototype[k]; }
}
const cmp = (x, y) => (x > y ? 1 : x < y ? -1 : 0);

withProto({1: "P1"}, () => { const a = [3, , 1, 2]; a.sort(cmp); dump("B1", a); });
withProto({0: "z"}, () => { const a = [, 2, 1]; a.sort(cmp); dump("B2", a); });
withProto({2: "zz"}, () => { const a = [3, , 1]; a.sort(); dump("B3", a); });
withProto({1: undefined}, () => { const a = [3, , 1]; a.sort(cmp); dump("B4", a); });
withProto({}, () => { const a = [3, , 1]; a.sort(cmp); dump("B5", a); });
withProto({0: "a", 3: "b"}, () => { const a = [, 3, , 2]; a.sort(cmp); dump("B6", a); });
withProto({5: "s"}, () => { const a = [4, , 2]; a.x = 1; a[10000] = 7; a.length = 3; a.sort(cmp); dump("B7", a); });
withProto({1: "q"}, () => {
  const a = [9, , 8];
  let reads = 0;
  a.sort(function (x, y) { if (x === "q" || y === "q") reads++; return cmp(x, y); });
  dump("B8", a); out("B8 proto-seen", reads > 0);
});
withProto({1: "p"}, () => {
  const a = [5, , 3];
  let r;
  try { a.sort((x, y) => { if (x === "p" || y === "p") throw new RangeError("X"); return cmp(x, y); }); r = "no-throw"; }
  catch (e) { r = e.name; }
  out("B9", r, "len=" + a.length);
});
withProto({2: "k"}, () => {
  class A extends Array {}
  const a = A.from([2, , 1]);
  a.sort(cmp);
  dump("B10", a);
});
