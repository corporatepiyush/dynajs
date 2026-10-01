__EXP = {};
__EXP[0] = ["exc: 1 2 true"];
__EXP[1] = ["ctor-esc: 3 ctor boom"];
__EXP[2] = ["spike: 120 true"];
__EXP[5] = ["fin: registered ok"];
"use strict";
function churn(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += { a: i, b: { c: i } }.b.c;
    return s;
}

function build(bad) {
    const partial = { a: 1, inner: { b: 2 } };
    if (bad) throw partial;
    return partial;
}
let caught = null;
try { churn(30000); build(true); } catch (e) { caught = e; churn(30000); }
__L(0, "exc:", caught.a, caught.inner.b, caught.inner === caught.inner);

let escapedThis = null;
class Escaper {
    constructor() {
        escapedThis = this;
        this.v = 3;
        churn(20000);
        throw new Error("ctor boom");
    }
}
try { new Escaper(); } catch (e) { __L(1, "ctor-esc:", escapedThis.v, e.message); }

const retained = [];
for (let round = 0; round < 40; round++) {
    const spike = [];
    for (let i = 0; i < 50000; i++) {
        spike.push({ i: i, o: { j: i } });
    }
    retained.push(spike[0], spike[24999], spike[49999]);
}
__L(2, "spike:", retained.length, retained.every(o => o.o.j === o.i));

const wr = [];
let strongPin = null;
(function () {
    const watched = { k: 42 };
    strongPin = watched;
    churn(20000);
    wr.push(new WeakRef(watched));
    churn(20000);
})();
__A("exceptions_gc_weakref.js:weakref:", function () { assert_eq(wr[0].deref()?.k, 42, "weakref:"); });
churn(50000);
__A("exceptions_gc_weakref.js:weakref-after-churn:", function () { assert_eq(typeof wr[0].deref(), "object", "weakref-after-churn:"); });
strongPin = null;

let finCount = 0;
const fr = new FinalizationRegistry(() => { finCount++; });
(function () {
    for (let i = 0; i < 100; i++) fr.register({ i: i }, i);
})();
churn(50000);
__L(5, "fin: registered ok");

summary("nursery");
