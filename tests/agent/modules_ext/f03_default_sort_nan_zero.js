// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["f64: -Infinity,-3,0,0,5,Infinity,NaN,NaN"];
__EXP[2] = ["f32: -2.5,0,1.5,NaN,NaN"];
__EXP[3] = ["f64-zeros: false,false"];
__EXP[4] = ["f16: -1,0.5,1,NaN,NaN"];
__EXP[5] = ["f16: SKIP"];
__EXP[6] = ["DONE"];
__REQ = {"dynajs": {"1": 1, "2": 1, "3": 1, "4": 1, "6": 1}, "node": {"1": 1, "2": 1, "3": 1, "5": 1, "6": 1}};
// modules_ext f03: DEFAULT sort (no comparator): NaN last, -0 before +0, Infinity
// ends, Float32/Float64 and Float16 (guarded). This is the well-defined case both
// engines must match.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const f = new Float64Array([Infinity, -Infinity, 0, -0, NaN, 5, -3, NaN]);
f.sort();
__L(1, 'f64: ' + f.join(','));
const g = new Float32Array([NaN, 1.5, NaN, -2.5, 0]);
g.sort();
__L(2, 'f32: ' + g.join(','));
__L(3, 'f64-zeros: ' + (Object.is(f[3], -0)) + ',' + (Object.is(f[4], -0)));
if (typeof Float16Array !== 'undefined') {
  const h = new Float16Array([NaN, 1, NaN, -1, 0.5]);
  h.sort();
  __L(4, 'f16: ' + h.join(','));
} else {
  __L(5, 'f16: SKIP');
}
__L(6, 'DONE');

summary("modules_ext");
