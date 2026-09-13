// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["n1:v1|f0|f1|f2|f3|ret:vRdtrue|n2:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e08: return() whose finally block awaits a SLOW (multi-microtask)
// chain: a next() queued behind the return must wait for AWAITING_RETURN to finish
// (not receive done:true early). Two consumers total.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  try {
    yield 1;
  } finally {
    log.push('f0');
    await Promise.resolve().then(() => log.push('f1')).then(() => log.push('f2'));
    log.push('f3');
  }
}
const gen = g();
gen.next().then(r => {
  log.push('n1:v' + r.value);
  const ret = gen.return('R').then(r => log.push('ret:v' + r.value + 'd' + r.done));
  const nxt = gen.next().then(r => log.push('n2:v' + r.value + 'd' + r.done));
  return Promise.all([ret, nxt]);
}).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
