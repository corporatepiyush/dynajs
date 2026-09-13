// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["0:vi1dfalse|1:vi2dfalse|delegated-return:idone|2:vafterdfalse|3:vundefineddtrue|b1:i1|ret:earlydtrue"];
__EXP[2] = ["DONE"];
// modules_ext e10: yield* delegation between async generators with CONCURRENT
// consumers on the OUTER generator: inner yields and its return value must be
// forwarded; return() during delegation must propagate to the inner generator.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* inner() {
  yield 'i1';
  yield 'i2';
  return 'idone';
}
async function* outer() {
  const r = yield* inner();
  log.push('delegated-return:' + r);
  yield 'after';
}
const gen = outer();
const ps = [gen.next(), gen.next(), gen.next(), gen.next()];
Promise.all(ps.map((p, i) => p.then(r => log.push(i + ':v' + r.value + 'd' + r.done)))).then(() => {
  const g2 = outer();
  const q = g2.next().then(r => log.push('b1:' + r.value)).then(() => g2.return('early')).then(r => log.push('ret:' + r.value + 'd' + r.done));
  return q;
}).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
