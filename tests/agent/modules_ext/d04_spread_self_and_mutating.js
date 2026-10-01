__EXP = {};
__EXP[1] = ["ok 1 growing iterator got=[1,2,3]", "ok 2 double self spread", "ok 3 zero-yield generators", "ok 5 map spread"];
__EXP[2] = ["FAIL 4 gen then elision"];
__EXP[3] = ["RESULT FAILURES=1"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const src = [1, 2, 3];
const it = {
  [Symbol.iterator]() {
    let i = 0;
    return { next() { src.push(src.length); if (i < 3) return { value: src[i++], done: false }; return { value: undefined, done: true }; } };
  },
};
const r1 = [...it];
ok(JSON.stringify(r1) === '[1,2,3]', 'growing iterator got=' + JSON.stringify(r1));
const arr = [1, 2, 3];
const r2 = [...arr, ...arr];
ok(JSON.stringify(r2) === '[1,2,3,1,2,3]', 'double self spread');
function gen(n) { return { [Symbol.iterator]() { let i = 0; return { next() { return i < n ? { value: i++, done: false } : { value: undefined, done: true }; } }; } }; }
ok(JSON.stringify([...gen(0), ...gen(0), ...gen(3)]) === '[0,1,2]', 'zero-yield generators');
ok(JSON.stringify([...gen(2), , 9].length) === '3', 'gen then elision');
ok(JSON.stringify([...new Map([[1, 'a']]), ...[2]]) === '[[1,"a"],2]', 'map spread');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
