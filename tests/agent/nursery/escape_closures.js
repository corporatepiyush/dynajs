__EXP = {};
__EXP[0] = ["closure: 2 counter"];
__EXP[1] = ["gen: 2 2 true true"];
__EXP[2] = ["async: 10 11 true"];
"use strict";
function churn(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += { a: i, b: { c: i } }.b.c;
    return s;
}

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

async function run() {
    const before = { v: 10 };
    const p = Promise.resolve().then(() => ({ v: before.v + 1 }));
    churn(30000);
    const after = await p;
    __L(2, "async:", before.v, after.v, after.v === before.v + 1);
}
run();

const target = { n: 7 };
const bound = function (extra) { return { sum: target.n + extra.v }; }.bind(null, { v: 5 });
churn(50000);
Promise.resolve(bound()).then(r => __A("escape_closures.js:bound:", function () { assert_eq(r.sum, 12, "bound:"); }));

__FINISH("nursery");
