__EXP = {};
__EXP[1] = ["try|n1:v1|finally-start|finally-end|ret:vRETdtrue|n2:vundefineddtrue"];
__EXP[2] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  try {
    log.push('try');
    yield 1;
    log.push('unreachable');
  } finally {
    log.push('finally-start');
    await Promise.resolve();
    log.push('finally-end');
  }
}
const gen = g();
gen.next().then(r => {
  log.push('n1:v' + r.value);
  return gen.return('RET');
}).then(r => {
  log.push('ret:v' + r.value + 'd' + r.done);
  return gen.next();
}).then(r => {
  log.push('n2:v' + r.value + 'd' + r.done);
  __L(1, log.join('|'));
  __L(2, 'DONE');
});

__FINISH("modules_ext");
