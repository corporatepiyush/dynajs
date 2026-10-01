__EXP = {};
__EXP[0] = ["A protos=true nextShared=true"];
__EXP[1] = ["B brand => ok TypeError"];
__EXP[2] = ["C 123 done=false"];
__EXP[3] = ["D true undefined:true"];
__EXP[4] = ["E {\"value\":9,\"done\":false} {\"done\":true} keys=[\"value\",\"done\"]"];
__EXP[5] = ["F self-it=true forof-twice=1"];
const out = typeof console !== "undefined" ? console.log : print;

{
  const ai = [][Symbol.iterator]();
  const ti = new Uint8Array()[Symbol.iterator]();
  __L(0, "A protos=" + (Object.getPrototypeOf(ai) === Object.getPrototypeOf(ti)) +
      " nextShared=" + (ai.next === ti.next));
}
{
  const it = [1][Symbol.iterator]();
  let r1, r2;
  try { Array.prototype[Symbol.iterator].call({ length: 1, 0: "x" }).next(); r1 = "ok"; } catch (e) { r1 = e.name; }
  try { it.next.call({}); r2 = "ok"; } catch (e) { r2 = e.name; }
  __L(1, "B brand => " + r1 + " " + r2);
}
{
  const a = [1, 2, 3];
  const it = a[Symbol.iterator]();
  const r1 = it.next();
  a[100000] = "slow";
  const r2 = it.next();
  const r3 = it.next();
  const r4 = it.next();
  __L(2, "C " + r1.value + r2.value + r3.value + " done=" + r4.done);
}
{
  const b = [0, 1, 2];
  const it2 = b[Symbol.iterator]();
  it2.next();
  b.length = 1;
  const d1 = it2.next().done;
  b.length = 5;
  b[1] = "one";
  const r = it2.next();
  __L(3, "D " + d1 + " " + r.value + ":" + r.done);
}
{
  const it3 = [9][Symbol.iterator]();
  const r1 = it3.next();
  const r2 = it3.next();
  __L(4, "E " + JSON.stringify(r1) + " " + JSON.stringify(r2) +
      " keys=" + JSON.stringify(Object.keys(r2)));
}
{
  const it4 = [1][Symbol.iterator]();
  __L(5, "F self-it=" + (it4[Symbol.iterator]() === it4) + " forof-twice=" + (() => {
    let n = 0;
    for (const v of { [Symbol.iterator]: () => it4 }) n++;
    for (const v of { [Symbol.iterator]: () => it4 }) n++;
    return n;
  })());
}

summary("arrays_ext");
