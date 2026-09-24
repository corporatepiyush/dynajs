// x6 differential: sort must materialize prototype-resolved holes as own
// elements (spec Get/Set round trip), and otherwise leave holes deleted.
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

// B1: proto at hole, value sorts back to its own index (the defect shape)
withProto({1: "P1"}, () => { const a = [3, , 1, 2]; a.sort(cmp); dump("B1", a); });
// B2: proto value sorts to the end
withProto({0: "z"}, () => { const a = [, 2, 1]; a.sort(cmp); dump("B2", a); });
// B3: default sort (ToString), proto at hole
withProto({2: "zz"}, () => { const a = [3, , 1]; a.sort(); dump("B3", a); });
// B4: proto UNDEFINED at hole -> own undefined materialized
withProto({1: undefined}, () => { const a = [3, , 1]; a.sort(cmp); dump("B4", a); });
// B5: no proto value: holes stay deleted
withProto({}, () => { const a = [3, , 1]; a.sort(cmp); dump("B5", a); });
// B6: multiple holes with proto values
withProto({0: "a", 3: "b"}, () => { const a = [, 3, , 2]; a.sort(cmp); dump("B6", a); });
// B7: slow (sparse) array with proto at a hole
withProto({5: "s"}, () => { const a = [4, , 2]; a.x = 1; a[10000] = 7; a.length = 3; a.sort(cmp); dump("B7", a); });
// B8: proto value on Array.prototype added via defineProperty with setter read count
withProto({1: "q"}, () => {
  const a = [9, , 8];
  let reads = 0;
  a.sort(function (x, y) { if (x === "q" || y === "q") reads++; return cmp(x, y); });
  dump("B8", a); out("B8 proto-seen", reads > 0);
});
// B9: throwing comparator mid-sort on holey+proto (impl-defined state; assert throw + own-ness is sane)
withProto({1: "p"}, () => {
  const a = [5, , 3];
  let r;
  try { a.sort((x, y) => { if (x === "p" || y === "p") throw new RangeError("X"); return cmp(x, y); }); r = "no-throw"; }
  catch (e) { r = e.name; }
  out("B9", r, "len=" + a.length);
});
// B10: subclass array with proto at hole
withProto({2: "k"}, () => {
  class A extends Array {}
  const a = A.from([2, , 1]);
  a.sort(cmp);
  dump("B10", a);
});
