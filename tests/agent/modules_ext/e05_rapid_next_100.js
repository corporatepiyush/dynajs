// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["order-ok=true"];
__EXP[2] = ["per-promise-bad=0"];
__EXP[3] = ["final:v=endd=true"];
__EXP[4] = ["DONE"];
// modules_ext e05: 100 rapid next() calls WITHOUT awaiting (queue depth stress).
// Every promise must resolve with its queue position's yield value, in order.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
async function* g() {
  for (let i = 0; i < 100; i++) yield i;
  return 'end';
}
const gen = g();
const order = [];
const ps = [];
for (let i = 0; i < 100; i++) ps.push(gen.next().then(r => { order.push(r.value); return r; }));
Promise.all(ps).then(rs => {
  let bad = 0;
  for (let i = 0; i < 100; i++) if (rs[i].value !== i || rs[i].done !== false) bad++;
  __L(1, 'order-ok=' + (order.length === 100 && order.every((v, i) => v === i)));
  __L(2, 'per-promise-bad=' + bad);
  return gen.next();
}).then(r => { __L(3, 'final:v=' + r.value + 'd=' + r.done); __L(4, 'DONE'); });

__FINISH("modules_ext");
