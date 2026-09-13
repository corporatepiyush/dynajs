// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["frozen-src => [1,2,3] frozen(r)=false"];
__EXP[1] = ["frozen-res => push=TypeError set=v0=1 del=len=3"];
__EXP[2] = ["frozen-sparse => len=8 2=two 0=undefined holes=none idx=2 inc=true"];
__EXP[3] = ["sealed-res => push=TypeError set=changed"];
__EXP[4] = ["search => 1 2 true"];
__EXP[5] = ["frozen-bulk => [5,6]"];
// S07: spread result then Object.freeze — frozen spread results, then mutation attempts + search
"use strict";
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = Object.freeze([1, 2, 3]);
  const r = [...a]; // spread of frozen is fine
  __L(0, "frozen-src => " + JSON.stringify(r) + " frozen(r)=" + Object.isFrozen(r));
}
{
  const r = Object.freeze([...[1, 2, 3]]);
  let p1, p2, p3;
  try { r.push(4); p1 = "len=" + r.length; } catch (e) { p1 = e.name; }
  try { r[0] = 99; p2 = "v0=" + r[0]; } catch (e) { p2 = e.name; }
  try { delete r[1]; p3 = "len=" + r.length; } catch (e) { p3 = e.name; }
  __L(1, "frozen-res => push=" + p1 + " set=" + p2 + " del=" + p3);
}
{
  // frozen sparse spread result
  const s = [];
  s.length = 8;
  s[2] = "two";
  const fs = Object.freeze([...s]);
  __L(2, "frozen-sparse => len=" + fs.length + " 2=" + fs[2] + " 0=" + fs[0] +
      " holes=" + (Object.prototype.hasOwnProperty.call(fs, 0) ? "none" : "yes") +
      " idx=" + JSON.stringify(fs.indexOf("two")) + " inc=" + fs.includes(undefined));
}
{
  // sealed (not frozen): indices can't be added/removed, values CAN change
  const r2 = Object.seal([...[1, 2, 3]]);
  let p1, p2;
  try { r2.push(4); p1 = "len=" + r2.length; } catch (e) { p1 = e.name; }
  try { r2[1] = "changed"; p2 = r2[1]; } catch (e) { p2 = e.name; }
  __L(3, "sealed-res => push=" + p1 + " set=" + p2);
}
{
  // frozen result through mutator search fast paths
  const r3 = Object.freeze([..."abc"]);
  __L(4, "search => " + r3.indexOf("b") + " " + r3.lastIndexOf("c") + " " + r3.includes("a"));
}
{
  // freeze the SOURCE mid-spread is impossible (spread consumes a snapshot iterator),
  // but freeze before push bulk-append is observable
  const dst = [];
  dst.push(...Object.freeze([5, 6]));
  __L(5, "frozen-bulk => " + JSON.stringify(dst));
}

summary("arrays_ext");
