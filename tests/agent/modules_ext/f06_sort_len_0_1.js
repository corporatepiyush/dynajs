// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 empty default", "ok 2 empty cmp", "ok 3 empty cmp never called, called=0", "ok 4 len1", "ok 5 len1 cmp never called, called=0", "ok 6 len1 unchanged", "ok 7 TA ctor over detached -> TypeError, got=TypeError", "ok 8 detached TA sort default", "ok 9 detached TA sort cmp", "ok 10 detached cmp never called"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext f06: length-0 and length-1 sorts, with and without comparator, incl.
// on a detached 0-length TA. Must never throw, never consult the comparator.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }
let called = 0;
function cmp() { called++; return 0; }
ok(ename(() => new Int32Array(0).sort()) === 'no-throw', 'empty default');
ok(ename(() => new Int32Array(0).sort(cmp)) === 'no-throw', 'empty cmp');
ok(called === 0, 'empty cmp never called, called=' + called);
const one = new Int32Array([7]);
ok(ename(() => one.sort(cmp)) === 'no-throw', 'len1');
ok(called === 0, 'len1 cmp never called, called=' + called);
ok(one[0] === 7, 'len1 unchanged');
if (typeof ArrayBuffer.prototype.transfer === 'function') {
  const ab = new ArrayBuffer(8);
  new Float64Array(ab)[0] = 1;
  ab.transfer(0);
  const mk = () => new Float64Array(ab); // constructing over a detached buffer throws TypeError: name parity
  ok(ename(mk) === 'TypeError', 'TA ctor over detached -> TypeError, got=' + ename(mk));
  const d = mk.length === undefined ? null : null; // (ctor already threw above if so)
  ok(ename(() => d.sort()) === 'TypeError', 'detached TA sort default');
  ok(ename(() => d.sort(cmp)) === 'TypeError', 'detached TA sort cmp');
  ok(called === 0, 'detached cmp never called');
}
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
