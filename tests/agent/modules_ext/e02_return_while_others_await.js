// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["n1:v1|n2:v2dfalse|n3:vundefineddtrue|ret:Rdtrue|n4:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e02: .return() fired while other next() consumers are queued. return()
// jumps the queue; remaining queued next() get {value: undefined, done: true} AFTER
// the return completes (AWAITING_RETURN pause), then the drain continues. Sequence.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
async function* g() {
  yield 1;
  yield 2;
}
const gen = g();
const log = [];
const n1 = gen.next().then(r => { log.push('n1:v' + r.value); });
const n2 = gen.next().then(r => log.push('n2:v' + r.value + 'd' + r.done));
const n3 = gen.next().then(r => log.push('n3:v' + r.value + 'd' + r.done));
const r1 = gen.return('R').then(r => log.push('ret:' + r.value + 'd' + r.done));
const n4 = gen.next().then(r => log.push('n4:v' + r.value + 'd' + r.done));
Promise.all([n1, n2, n3, r1, n4]).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); });

__FINISH("modules_ext");
