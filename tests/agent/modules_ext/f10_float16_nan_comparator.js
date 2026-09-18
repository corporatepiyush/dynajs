// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["SKIP no-f16"];
__EXP[2] = ["f16-nancmp: 1,NaN,-1,0.5,NaN,2"];
__EXP[3] = ["f16-default: -65504,0,0,0.0000010132789611816406,65504,NaN"];
__EXP[4] = ["f16-nanfirst: NaN,1,3"];
__EXP[5] = ["DONE"];
__REQ = {"dynajs": {"2": 1, "3": 1, "4": 1, "5": 1}, "node": {"1": 1, "5": 1}};
// modules_ext f10: Float16Array with comparators (NaN-returning, NaN-first,
// default). node lacks Float16Array -> prints SKIP (whitelisted divergence); on
// dynajs this exercises the fp16 compare kernel incl. subnormals.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
if (typeof Float16Array === 'undefined') {
  __L(1, 'SKIP no-f16');
} else {
  const a = new Float16Array([1, NaN, -1, 0.5, NaN, 2]);
  a.sort(() => NaN);
  __L(2, 'f16-nancmp: ' + a.join(','));
  const b = new Float16Array([NaN, 65504, -65504, 0.000001, -0, 0]);
  b.sort();
  __L(3, 'f16-default: ' + b.join(','));
  const c = new Float16Array([3, NaN, 1]);
  c.sort((x, y) => (Number.isNaN(x) ? -1 : Number.isNaN(y) ? 1 : x - y));
  __L(4, 'f16-nanfirst: ' + c.join(','));
}
__L(5, 'DONE');

summary("modules_ext");
