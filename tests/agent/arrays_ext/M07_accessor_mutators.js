// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A shift-gets=1 got=[1,2,\"G\"] left=[]"];
__EXP[1] = ["B slice-gets=1 [\"H\",2] ownH=true"];
__EXP[2] = ["C rev=TypeError gets=1 own0=true own1=true"];
__EXP[3] = ["D concat-gets=1 [\"C\",2,9]"];
__EXP[4] = ["E frozen => unshift=TypeError shift=TypeError"];
__EXP[5] = ["F sealed-unshift=TypeError"];
__EXP[6] = ["G rev-sets=1 [\"S1\",\"S0\"]"];
// M07: mutator reads of accessor elements — shift/unshift/reverse/concat trigger getters
// exactly per spec Get calls; non-writable length blocks unshift
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

{
  let gets = 0;
  const a = [1, 2, 3];
  Object.defineProperty(a, "2", { get() { gets++; return "G"; }, configurable: true });
  const r = a.shift(); // reads index 0 (no getter)
  const r2 = a.shift(); // index 1
  const r3 = a.shift(); // index 0 was original "2" (getter now at index 0 after two shifts)
  __L(0, "A shift-gets=" + gets + " got=" + JSON.stringify([r, r2, r3]) + " left=" + JSON.stringify([...a]));
}
{
  let gets = 0;
  const b = [1, 2];
  Object.defineProperty(b, "0", { get() { gets++; return "H"; }, configurable: true });
  const s = b.slice(); // slice reads every index via Get when HasProperty
  __L(1, "B slice-gets=" + gets + " " + JSON.stringify(s) + " ownH=" +
      Object.prototype.hasOwnProperty.call(s, 0));
}
{
  let gets = 0;
  const c = [1, 2];
  Object.defineProperty(c, "1", { get() { gets++; return "R"; }, configurable: true });
  let r;
  try { c.reverse(); r = JSON.stringify(c); } catch (e) { r = e.name + " gets=" + gets; }
  __L(2, "C rev=" + r + " own0=" +
      Object.prototype.hasOwnProperty.call(c, 0) + " own1=" +
      Object.prototype.hasOwnProperty.call(c, 1));
}
{
  let gets = 0;
  const d = [1, 2];
  Object.defineProperty(d, "0", { get() { gets++; return "C"; }, configurable: true });
  const out1 = d.concat([9]);
  __L(3, "D concat-gets=" + gets + " " + JSON.stringify(out1));
}
{
  // non-writable length blocks unshift (and shift) with TypeError
  const e = Object.freeze([1, 2]);
  let r1, r2;
  try { e.unshift(0); r1 = "no-throw " + JSON.stringify(e); } catch (x) { r1 = x.name; }
  try { e.shift(); r2 = "no-throw " + JSON.stringify(e); } catch (x) { r2 = x.name; }
  __L(4, "E frozen => unshift=" + r1 + " shift=" + r2);
}
{
  // sealed: length fixed + no new indices -> unshift MUST throw (TypeError both engines)
  const f = Object.seal([1, 2]);
  let r;
  try { f.unshift(0); r = "no-throw " + JSON.stringify(f); } catch (e) { r = e.name; }
  __L(5, "F sealed-unshift=" + r);
}
{
  // setter element + reverse: Set goes through the setter (spec Set with throw=true)
  let sets = 0;
  const g = [1, 2];
  Object.defineProperty(g, "0", {
    set(v) { sets++; },
    get() { return "S" + sets; },
    configurable: true,
  });
  g.reverse(); // reads a[1]=2, writes a[0]=2 (setter), reads a[0]="S1"?, writes a[1]
  __L(6, "G rev-sets=" + sets + " " + JSON.stringify(g));
}

summary("arrays_ext");
