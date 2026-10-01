__EXP = {};
__EXP[1] = ["p0:v1dfalse|0:EError|1:vundefineddtrue|2:EError|3:vundefineddtrue"];
__EXP[2] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  yield 1;
  throw new Error('kill');
}
const gen = g();
const p0 = gen.next().then(r => log.push('p0:v' + r.value + 'd' + r.done), e => log.push('p0:E' + (e && e.name)));
p0.then(() => {
  const ps = [gen.next(), gen.next(), gen.throw(new Error('late')), gen.next()];
  const logs = ps.map((p, i) => p.then(r => log.push(i + ':v' + r.value + 'd' + r.done), e => log.push(i + ':E' + (e && e.name))));
  return Promise.all(logs);
}).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
