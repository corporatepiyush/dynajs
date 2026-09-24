// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["0:v1dfalse|1:v2dfalse|2:Error:boom3|3:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e01: 4 concurrent next() consumers; the generator THROWS at k==3.
// All four queued requests must settle (values then rejection, in queue order).
// The drain fix: after the rejection the generator is COMPLETED and remaining
// queued requests must not hang. Resolution ORDER printed.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
async function* g() {
  for (let k = 1; k <= 10; k++) {
    if (k === 3) throw new Error('boom3');
    yield k;
  }
}
const gen = g();
const log = [];
const ps = [gen.next(), gen.next(), gen.next(), gen.next()];
Promise.all(ps.map((p, i) => p.then(r => log.push(i + ':v' + r.value + 'd' + r.done), e => log.push(i + ':' + (e && e.name) + ':' + (e && e.message))))
).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
