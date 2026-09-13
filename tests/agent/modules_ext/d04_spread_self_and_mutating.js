// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 growing iterator got=[1,2,3]", "ok 2 double self spread", "ok 3 zero-yield generators", "ok 5 map spread"];
__EXP[2] = ["FAIL 4 gen then elision"];
__EXP[3] = ["RESULT FAILURES=1"];
// modules_ext d04: self-spread and mutating iterables -- spread of an array WHILE it
// grows (proxy/iterator side effects), and spread where source === target candidate
// (must take the define fallback, not realloc the buffer out from under arrp).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
// iterator that grows its own backing array as it is consumed
const src = [1, 2, 3];
const it = {
  [Symbol.iterator]() {
    let i = 0;
    return { next() { src.push(src.length); if (i < 3) return { value: src[i++], done: false }; return { value: undefined, done: true }; } };
  },
};
const r1 = [...it];
ok(JSON.stringify(r1) === '[1,2,3]', 'growing iterator got=' + JSON.stringify(r1));
// spread source equal to the freshly created target slot context: classic [arr] vs [...arr, ...arr] aliasing
const arr = [1, 2, 3];
const r2 = [...arr, ...arr];
ok(JSON.stringify(r2) === '[1,2,3,1,2,3]', 'double self spread');
// iterator returning exactly 0 items then more items later
function gen(n) { return { [Symbol.iterator]() { let i = 0; return { next() { return i < n ? { value: i++, done: false } : { value: undefined, done: true }; } }; } }; }
ok(JSON.stringify([...gen(0), ...gen(0), ...gen(3)]) === '[0,1,2]', 'zero-yield generators');
ok(JSON.stringify([...gen(2), , 9].length) === '3', 'gen then elision');
// non-array iterables mixed with arrays
ok(JSON.stringify([...new Map([[1, 'a']]), ...[2]]) === '[[1,"a"],2]', 'map spread');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
