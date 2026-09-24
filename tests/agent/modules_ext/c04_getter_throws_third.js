// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 values throws Boom, got=Boom", "ok 2 ORIGINAL exception object propagated", "ok 3 getter ran exactly 3 times, calls=3", "ok 4 entries same", "ok 5 entries getter calls=3", "ok 6 keys never invokes getters, calls=0"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext c04: getter throws on the 3rd key. values/entries must discard the
// partial result and propagate the ORIGINAL exception object (identity check via
// marker property). Error type is custom class -> name parity. Also keys() on the
// same object must NOT throw (KEY kind reads no getters).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
class Boom extends Error { constructor(m) { super(m); this.name = 'Boom'; this.marker = 42; } }
let calls = 0;
const o = { a: 1, b: 2, c: 3, d: 4 };
Object.defineProperty(o, 'a', { get() { calls++; return 1; }, enumerable: true, configurable: true });
Object.defineProperty(o, 'b', { get() { calls++; return 2; }, enumerable: true, configurable: true });
Object.defineProperty(o, 'c', { get() { calls++; throw new Boom('third'); }, enumerable: true, configurable: true });
Object.defineProperty(o, 'd', { get() { calls++; return 4; }, enumerable: true, configurable: true });
let caught = null;
try { Object.values(o); } catch (e) { caught = e; }
ok(caught !== null && caught.name === 'Boom', 'values throws Boom, got=' + (caught && caught.name));
ok(caught && caught.marker === 42, 'ORIGINAL exception object propagated');
ok(calls === 3, 'getter ran exactly 3 times, calls=' + calls);
calls = 0; caught = null;
try { Object.entries(o); } catch (e) { caught = e; }
ok(caught && caught.name === 'Boom' && caught.marker === 42, 'entries same');
ok(calls === 3, 'entries getter calls=' + calls);
calls = 0;
ok(JSON.stringify(Object.keys(o)) === '["a","b","c","d"]', 'keys never invokes getters, calls=' + calls);
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
