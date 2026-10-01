__EXP = {};
__EXP[1] = ["body|c1:v1dfalse|caught:inj|c2:v2dfalse|c3:vhandleddtrue|c4:vundefineddtrue"];
__EXP[2] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  log.push('body');
  try {
    yield 1;
    yield 2;
  } catch (e) {
    log.push('caught:' + (e && e.message));
    return 'handled';
  }
  yield 3;
}
const gen = g();
const p1 = gen.next().then(r => log.push('c1:v' + r.value + 'd' + r.done));
const p2 = gen.next().then(r => log.push('c2:v' + r.value + 'd' + r.done));
const p3 = gen.throw(new Error('inj')).then(r => log.push('c3:v' + r.value + 'd' + r.done), e => log.push('c3:E' + (e && e.name)));
const p4 = gen.next().then(r => log.push('c4:v' + r.value + 'd' + r.done));
Promise.all([p1, p2, p3, p4]).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
