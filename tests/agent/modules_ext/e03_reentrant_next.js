__EXP = null;
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
const gen = (async function* () {
  const self = gen;
  const inner = self.next().then(r => log.push('inner:v' + r.value + 'd' + r.done));
  log.push('body-before-yield');
  yield 1;
  log.push('body-after');
  await inner;
  yield 2;
})();
gen.next().then(r => log.push('outer1:v' + r.value))
  .then(() => gen.next())
  .then(r => { log.push('outer2:v' + r.value + 'd' + r.done); })
  .then(() => { __L(1, log.join('|')); __L(2, 'DONE'); },
        e => { log.push('E:' + (e && e.name)); __L(3, log.join('|')); __L(4, 'DONE'); });

__FINISH("modules_ext");
