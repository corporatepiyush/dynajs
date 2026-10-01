__EXP = {};
__EXP[1] = ["mutated: 1,2,3 calls=3"];
__EXP[2] = ["rewritten: 1,2,3,4,5"];
__EXP[3] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const f = new Int32Array([3, 1, 2]);
let calls = 0;
f.sort(function (a, b) {
  calls++;
  if (calls === 1) { f[0] = 100; f[1] = 0; f[2] = 50; }
  return a - b;
});
__L(1, 'mutated: ' + f.join(',') + ' calls=' + calls);
const g = new Int32Array([5, 4, 3, 2, 1]);
let c2 = 0;
g.sort(function (a, b) {
  c2++;
  if (c2 === 2) { for (let i = 0; i < 5; i++) g[i] = i * 10; }
  return a - b;
});
__L(2, 'rewritten: ' + g.join(','));
__L(3, 'DONE');

summary("modules_ext");
