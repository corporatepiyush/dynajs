// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 throw at first next: got=Boom", "ok 2 throw after empties", "ok 3 throw mid-literal", "ok 4 computed member throw dominates"];
__EXP[3] = ["returned=0"];
__EXP[4] = ["RESULT all-pass"];
// modules_ext d06: spread whose iterator THROWS mid-way (after empty spreads, at
// position 0 of a bulk target): the target array must be left dense-correct (parts
// already appended stay), exception name propagates, and no "uninitialized" appears.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
class Boom extends Error { constructor() { super('b'); this.name = 'Boom'; } }
function throwing(n) {
  return { [Symbol.iterator]() { let i = 0; return { next() { if (i++ < n) return { value: i, done: false }; throw new Boom(); } }; } };
}
let caught = null;
try { const x = [...throwing(0)]; } catch (e) { caught = e; }
ok(caught && caught.name === 'Boom', 'throw at first next: got=' + (caught && caught.name));
caught = null;
try { const x = [...[], ...[], ...throwing(2)]; } catch (e) { caught = e; }
ok(caught && caught.name === 'Boom', 'throw after empties');
caught = null;
try { const x = [...[1], ...[2], ...throwing(0), ...[3]]; } catch (e) { caught = e; }
ok(caught && caught.name === 'Boom', 'throw mid-literal');
// iterator whose RETURN (bad) is invoked on abrupt completion
let returned = 0;
const badReturn = { [Symbol.iterator]() { let i = 0; return { next() { return i++ < 1 ? { value: 1, done: false } : { value: undefined, done: true }; }, return() { returned++; throw new TypeError('ret'); } }; } };
caught = null;
try { const x = [...badReturn, (() => { throw new Boom(); })()]; } catch (e) { caught = e; }
ok(caught && caught.name === 'Boom', 'computed member throw dominates');
__L(3, 'returned=' + returned);
__L(4, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
