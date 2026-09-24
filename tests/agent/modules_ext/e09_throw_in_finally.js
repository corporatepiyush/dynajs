// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["n1:v1|finally|ret:EError|n2:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e09: .return() whose finally block THROWS: the return promise must
// reject with that error; a next() queued behind it must still settle (rejection or
// done) -- never hang. Sequence + names printed.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  try {
    yield 1;
  } finally {
    log.push('finally');
    throw new Error('fin-boom');
  }
}
const gen = g();
const n1 = gen.next().then(r => log.push('n1:v' + r.value));
const ret = gen.return('R').then(r => log.push('ret:v' + r.value), e => log.push('ret:E' + (e && e.name)));
const n2 = gen.next().then(r => log.push('n2:v' + r.value + 'd' + r.done), e => log.push('n2:E' + (e && e.name)));
Promise.all([n1, ret, n2]).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
