// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["a:v1dfalse|cleanup|b:v2dfalse|d:vundefineddtrue|e:vZdtrue|c:vundefineddtrue"];
__EXP[2] = ["DONE"];
// modules_ext e12: deterministic chaos -- a fixed interleaving of next/throw/return
// from 3 consumers with yields in try/finally and awaits inside; every promise gets
// a settlement; ORDER of settlements is the node-parity oracle.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
async function* g() {
  try {
    await Promise.resolve();
    yield 1;
    await Promise.resolve();
    yield 2;
    yield 3;
  } finally {
    log.push('cleanup');
  }
}
const gen = g();
const mk = (p, tag) => p.then(r => log.push(tag + ':v' + r.value + 'd' + r.done), e => log.push(tag + ':E' + (e && e.name)));
const a = mk(gen.next(), 'a');
const b = mk(gen.next(), 'b');
const c = mk(gen.throw(new Error('x1')).catch(() => gen.next()).then(r => r), 'c'); // throw rejected; follow with next
const d = mk(gen.next(), 'd');
const e = mk(gen.return('Z'), 'e');
Promise.all([a, b, c, d, e]).then(() => { __L(1, log.join('|')); __L(2, 'DONE'); }, () => { __L(3, log.join('|')); __L(4, 'DONE'); });

__FINISH("modules_ext");
