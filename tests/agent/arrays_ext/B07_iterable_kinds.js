__EXP = {};
__EXP[0] = ["set => [1,2,3] lit=[0,1,2,3]"];
__EXP[1] = ["map => [[\"k1\",1],[\"k2\",2]] keys=[\"k1\",\"k2\"] push=[1,2]"];
__EXP[2] = ["string => len=3 0=a 1=1f600 push=[\"x\",\"y\"]"];
__EXP[3] = ["inherited => [\"i0\",\"i1\"] lit=[\"i0\",\"i1\"]"];
__EXP[4] = ["like => TypeError TypeError"];
__EXP[5] = ["bad-next => TypeError"];
__EXP[6] = ["no-next => TypeError"];
const out = typeof console !== "undefined" ? console.log : print;

{
  const s = new Set([1, 2, 3]);
  const a = [];
  a.push(...s);
  __L(0, "set => " + JSON.stringify(a) + " lit=" + JSON.stringify([0, ...s]));
}
{
  const m = new Map([["k1", 1], ["k2", 2]]);
  const r = [...m];
  __L(1, "map => " + JSON.stringify(r) + " keys=" + JSON.stringify([...m.keys()]) +
      " push=" + (() => { const d = []; d.push(...m.values()); return JSON.stringify(d); })());
}
{
  const s = "a\u{1F600}b";
  const arr = [...s];
  __L(2, "string => len=" + arr.length + " 0=" + arr[0] + " 1=" + arr[1].codePointAt(0).toString(16) +
      " push=" + (() => { const d = []; d.push(..."xy"); return JSON.stringify(d); })());
}
{
  const proto = { [Symbol.iterator]: function* () { yield "i0"; yield "i1"; } };
  const obj = Object.create(proto);
  obj.x = 1;
  let r;
  try { const d = []; d.push(...obj); r = JSON.stringify(d) + " lit=" + JSON.stringify([...obj]); }
  catch (e) { r = e.name; }
  __L(3, "inherited => " + r);
}
{
  const like = { length: 3, 0: "a", 1: "b", 2: "c" };
  let r1, r2;
  try { r1 = "lit=" + JSON.stringify([...like]); } catch (e) { r1 = e.name; }
  try { const d = []; d.push(...like); r2 = "pushed"; } catch (e) { r2 = e.name; }
  __L(4, "like => " + r1 + " " + r2);
}
{
  const bad = { [Symbol.iterator]: () => ({ next: () => 42 }) };
  let r;
  try { [...bad]; r = "no-throw"; } catch (e) { r = e.name; }
  __L(5, "bad-next => " + r);
}
{
  const worse = { [Symbol.iterator]: () => ({}) };
  let r;
  try { [...worse]; r = "no-throw"; } catch (e) { r = e.name; }
  __L(6, "no-next => " + r);
}

summary("arrays_ext");
