// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A protos=true nextShared=true"];
__EXP[1] = ["B brand => ok TypeError"];
__EXP[2] = ["C 123 done=false"];
__EXP[3] = ["D true undefined:true"];
__EXP[4] = ["E {\"value\":9,\"done\":false} {\"done\":true} keys=[\"value\",\"done\"]"];
__EXP[5] = ["F self-it=true forof-twice=1"];
// F14: array iterator branding + %ArrayIteratorPrototype% identity — next() on wrong `this`,
// iterator over array that becomes slow mid-iteration, TA iterator after species-ish swaps
const out = typeof console !== "undefined" ? console.log : print;

{
  // all array iterators share one prototype; TA iterators TOO (per spec both use
  // %ArrayIteratorPrototype%... TA uses its own per ES2023? — oracle: node)
  const ai = [][Symbol.iterator]();
  const ti = new Uint8Array()[Symbol.iterator]();
  __L(0, "A protos=" + (Object.getPrototypeOf(ai) === Object.getPrototypeOf(ti)) +
      " nextShared=" + (ai.next === ti.next));
}
{
  // next() called with a foreign `this` -> TypeError (requires internal slot)
  const it = [1][Symbol.iterator]();
  let r1, r2;
  try { Array.prototype[Symbol.iterator].call({ length: 1, 0: "x" }).next(); r1 = "ok"; } catch (e) { r1 = e.name; }
  try { it.next.call({}); r2 = "ok"; } catch (e) { r2 = e.name; }
  __L(1, "B brand => " + r1 + " " + r2);
}
{
  // manual next() while the array converts fast->slow mid-flight
  const a = [1, 2, 3];
  const it = a[Symbol.iterator]();
  const r1 = it.next();
  a[100000] = "slow"; // dictionary conversion
  const r2 = it.next();
  const r3 = it.next();
  const r4 = it.next();
  __L(2, "C " + r1.value + r2.value + r3.value + " done=" + r4.done);
}
{
  // iterator survives length bounce small<->big repeatedly
  const b = [0, 1, 2];
  const it2 = b[Symbol.iterator]();
  it2.next();
  b.length = 1; // shrink below cursor
  const d1 = it2.next().done;
  b.length = 5; // regrow past cursor (1)
  b[1] = "one";
  const r = it2.next();
  __L(3, "D " + d1 + " " + r.value + ":" + r.done);
}
{
  // value/done shape: done:true with value undefined; missing keys -> undefined value
  const it3 = [9][Symbol.iterator]();
  const r1 = it3.next();
  const r2 = it3.next();
  __L(4, "E " + JSON.stringify(r1) + " " + JSON.stringify(r2) +
      " keys=" + JSON.stringify(Object.keys(r2)));
}
{
  // iterator is its own [Symbol.iterator]
  const it4 = [1][Symbol.iterator]();
  __L(5, "F self-it=" + (it4[Symbol.iterator]() === it4) + " forof-twice=" + (() => {
    let n = 0;
    for (const v of { [Symbol.iterator]: () => it4 }) n++;
    for (const v of { [Symbol.iterator]: () => it4 }) n++;
    return n;
  })());
}

summary("arrays_ext");
