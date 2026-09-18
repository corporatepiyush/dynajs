// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["closure: 2 counter"];
__EXP[1] = ["gen: 2 2 true true"];
__EXP[2] = ["async: 10 11 true"];
/* Lifetime torture 1: objects escaping via closures, generators, async.
 * Every object here is born in a hot allocation loop (a nursery would
 * bump-allocate it) and then ESCAPES -- the exact cases a moving/promoting
 * nursery gets wrong and any correct engine must get byte-identical to node.
 * Output is deterministic; diffed vs node and vs the flag-off build.
 */
"use strict";
function churn(n) { /* allocation pressure around every escape site */
    let s = 0;
    for (let i = 0; i < n; i++) s += { a: i, b: { c: i } }.b.c;
    return s;
}

// 1. closure escape
function mkCounter() {
    const state = { count: 0, tag: "counter" };
    return {
        inc() { state.count++; return state.count; },
        peek() { return state; },
    };
}
const c1 = mkCounter();
churn(50000);
c1.inc(); c1.inc();
__L(0, "closure:", c1.peek().count, c1.peek().tag);

// 2. generator escape (object held across yields, generator resumed late)
function* gen() {
    const held = { step: 0, payload: [1, 2, 3] };
    for (let i = 0; i < 3; i++) {
        held.step = i;
        yield held;
    }
}
const g = gen();
churn(50000);
const first = g.next().value;
churn(50000);
g.next();
const last = g.next().value;
__L(1, "gen:", first.step, last.step, last === first, g.next().done);

// 3. async escape (object born before await, used after; churn interleaved)
async function run() {
    const before = { v: 10 };
    const p = Promise.resolve().then(() => ({ v: before.v + 1 }));
    churn(30000);
    const after = await p;
    __L(2, "async:", before.v, after.v, after.v === before.v + 1);
}
run();

// 4. bound function + promise capability holding young objects
const target = { n: 7 };
const bound = function (extra) { return { sum: target.n + extra.v }; }.bind(null, { v: 5 });
churn(50000);
Promise.resolve(bound()).then(r => __A("escape_closures.js:bound:", function () { assert_eq(r.sum, 12, "bound:"); }));

__FINISH("nursery");
