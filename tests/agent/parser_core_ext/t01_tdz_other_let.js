// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["i0,T0:ReferenceError,k0,i1,T1:ReferenceError,k2,i2,T2:ReferenceError,k4"];
__EXP[1] = ["Q1:ReferenceError,q101,Q2:ReferenceError,q102,Q3:ReferenceError,q103"];
__EXP[2] = ["0,Z:ReferenceError,10,20"];
// T1: loop-head TDZ elision on the loop var must not mask the TDZ throw of a
// DIFFERENT let declared in the same loop body block.
var log = [];
for (let i = 0; i < 3; i++) {
  log.push("i" + i);
  try { log.push(String(k)); } catch (e) { log.push("T" + i + ":" + e.constructor.name); }
  let k = i * 2;
  log.push("k" + k);
}
__L(0, log.join(","));
// same-shape while loop
var log2 = [];
var w = 0;
while (w < 3) {
  w++;
  try { log2.push(String(q)); } catch (e) { log2.push("Q" + w + ":" + e.constructor.name); }
  let q = w + 100;
  log2.push("q" + q);
}
__L(1, log2.join(","));
// TDZ let in an INNER block of the loop body, hit via closure before init
var log3 = [];
for (let i = 0; i < 3; i++) {
  const early = () => z;
  if (i === 1) { try { early(); } catch (e) { log3.push("Z:" + e.constructor.name); } }
  let z = i * 10;
  log3.push(String(early()));
}
__L(2, log3.join(","));

summary("parser_core_ext");
