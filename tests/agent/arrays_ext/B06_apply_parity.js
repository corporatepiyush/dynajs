__EXP = {};
__EXP[0] = ["apply-parity => [1,2,3,\"a\",\"b\",\"c\"] [1,2,3,\"a\",\"b\",\"c\"] eq=true"];
__EXP[1] = ["unshift-parity => [null,\"mid\",null,0] [null,\"mid\",null,0] eq=true"];
__EXP[2] = ["apply-like => [\"x\",null,\"z\"] len=3"];
__EXP[3] = ["spread-like => TypeError"];
__EXP[4] = ["concat => [1,2,3] spread => [1,2,3]"];
__EXP[5] = ["chain => [2,5,5] [1,2,3,4,5]"];
__EXP[6] = ["ta-push => TypeError"];
const out = typeof console !== "undefined" ? console.log : print;

{
  const s1 = [1, 2, 3];
  const s2 = [1, 2, 3];
  const src = ["a", "b", "c"];
  Array.prototype.push.apply(s1, src);
  s2.push(...src);
  __L(0, "apply-parity => " + JSON.stringify(s1) + " " + JSON.stringify(s2) + " eq=" +
      (JSON.stringify(s1) === JSON.stringify(s2)));
}
{
  const sp = [];
  sp.length = 3;
  sp[1] = "mid";
  const u1 = [0];
  const u2 = [0];
  Array.prototype.unshift.apply(u1, sp);
  u2.unshift(...sp);
  __L(1, "unshift-parity => " + JSON.stringify(u1) + " " + JSON.stringify(u2) + " eq=" +
      (JSON.stringify(u1) === JSON.stringify(u2)));
}
{
  const like = { length: 3, 0: "x", 2: "z" };
  const d1 = [];
  Array.prototype.push.apply(d1, like);
  __L(2, "apply-like => " + JSON.stringify(d1) + " len=" + d1.length);
  let r;
  try { d1.push(...like); r = "no-throw"; } catch (e) { r = e.name; }
  __L(3, "spread-like => " + r);
}
{
  const nest = [[1, 2], [3]];
  __L(4, "concat => " + JSON.stringify([].concat(...nest)) + " spread => " + JSON.stringify([...nest].flat ? [...nest].flat() : "?"));
}
{
  let len = [];
  const a = [];
  len.push(a.push(...[1, 2]));
  len.push(a.push(...[3], ...[4, 5]));
  len.push(a.push());
  __L(5, "chain => " + JSON.stringify(len) + " " + JSON.stringify(a));
}
{
  const ta = new Uint8Array(2);
  let r;
  try { r = "ret=" + ta.push(...[1, 2, 3]); } catch (e) { r = e.name; }
  __L(6, "ta-push => " + r);
}

summary("arrays_ext");
