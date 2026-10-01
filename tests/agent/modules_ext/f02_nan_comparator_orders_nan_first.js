__EXP = {};
__EXP[1] = ["nanfirst: NaN,NaN,1,2,3"];
__EXP[2] = ["pair: NaN,1"];
__EXP[3] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
function nanFirst(a, b) {
  const an = Number.isNaN(a), bn = Number.isNaN(b);
  if (an && bn) return 0;
  if (an) return -1;
  if (bn) return 1;
  return a - b;
}
const f = new Float64Array([1, NaN, 3, NaN, 2]);
f.sort(nanFirst);
__L(1, 'nanfirst: ' + f.join(','));
const g = new Float64Array([NaN, 1]);
g.sort(nanFirst);
__L(2, 'pair: ' + g.join(','));
__L(3, 'DONE');

summary("modules_ext");
