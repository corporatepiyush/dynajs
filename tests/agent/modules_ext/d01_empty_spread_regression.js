// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 [..[],..[],x] got=[\"x\"]", "ok 2 4 empties", "ok 3 expr empties", "ok 4 interleaved", "ok 5 empty string spread", "ok 6 string then empty", "ok 8 mixed gaps", "ok 9 call-result empties", "ok 10 set empties", "ok 11 char spreads"];
__EXP[2] = ["FAIL 7 zeros preserved"];
__EXP[3] = ["RESULT FAILURES=1"];
// modules_ext d01: THE regression shape -- [...[], ...[], 'x'] threw "uninitialized"
// when expand_fast_array(new_len==0) returned failure-without-exception. Sweep all
// empty-spread prefixes/suffixes/infixes plus length-0 arrays from expressions.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const E = [];
ok(JSON.stringify([...[], ...[], 'x']) === '["x"]', '[..[],..[],x] got=' + JSON.stringify([...[], ...[], 'x']));
ok(JSON.stringify([...[], ...[], ...[], ...[], 'y']) === '["y"]', '4 empties');
ok(JSON.stringify([...E, ...E, 1]) === '[1]', 'expr empties');
ok(JSON.stringify([1, ...[], 2, ...[], 3]) === '[1,2,3]', 'interleaved');
ok(JSON.stringify([...'']) === '[]', 'empty string spread');
ok(JSON.stringify([...'ab', ...'']) === '["a","b"]', 'string then empty');
ok(JSON.stringify([0, ...[], -0]) === '[0,-0]', 'zeros preserved');
ok(JSON.stringify([...[], ...[], ...[1, 2], ...[], ...[3]]) === '[1,2,3]', 'mixed gaps');
function ret(a) { return a; }
ok(JSON.stringify([...ret([]), ...ret([]), 'z']) === '["z"]', 'call-result empties');
ok(JSON.stringify([...new Set(), ...new Set(), 's']) === '["s"]', 'set empties');
ok(JSON.stringify([..."a", ...[], ..."b"]) === '["a","b"]', 'char spreads');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
