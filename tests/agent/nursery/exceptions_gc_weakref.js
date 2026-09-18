// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["exc: 1 2 true"];
__EXP[1] = ["ctor-esc: 3 ctor boom"];
__EXP[2] = ["spike: 120 true"];
__EXP[5] = ["fin: registered ok"];
/* Lifetime torture 3: exceptions mid-construction, GC-forcing spikes with
 * retained subsets, and WeakRef semantics across churn. Diffed vs node and
 * the flag-off build. (Engine has WeakRef + FinalizationRegistry.)
 */
"use strict";
function churn(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += { a: i, b: { c: i } }.b.c;
    return s;
}

// 1. exception mid-construction: partially built object escapes via catch
function build(bad) {
    const partial = { a: 1, inner: { b: 2 } };
    if (bad) throw partial; /* throw the young object itself */
    return partial;
}
let caught = null;
try { churn(30000); build(true); } catch (e) { caught = e; churn(30000); }
__L(0, "exc:", caught.a, caught.inner.b, caught.inner === caught.inner);

// 2. constructor throwing after `this` escaped (this-escape pattern)
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

// 3. GC-forcing spikes with retained subsets (arena reset pressure)
const retained = [];
for (let round = 0; round < 40; round++) {
    const spike = [];
    for (let i = 0; i < 50000; i++) {
        spike.push({ i: i, o: { j: i } }); /* ~100k young objs per spike */
    }
    retained.push(spike[0], spike[24999], spike[49999]);
}
__L(2, "spike:", retained.length, retained.every(o => o.o.j === o.i));

// 4. WeakRef across churn. The STRONGLY-HELD case is the deterministic
//    oracle (both engines: deref lives). The unreachable case is NOT
//    asserted: when an object becomes unreachable is impl-defined and
//    dynajs (refcount-first) clears WeakRefs earlier than node's tracing
//    GC -- pre-existing divergence, identical in flag-off and flag-on
//    builds (verified); see CHANGELOG.
const wr = [];
let strongPin = null;
(function () {
    const watched = { k: 42 };
    strongPin = watched; /* strong ref => deref must live in any engine */
    churn(20000);
    wr.push(new WeakRef(watched));
    churn(20000);
})();
__A("exceptions_gc_weakref.js:weakref:", function () { assert_eq(wr[0].deref()?.k, 42, "weakref:"); });
churn(50000);
__A("exceptions_gc_weakref.js:weakref-after-churn:", function () { assert_eq(typeof wr[0].deref(), "object", "weakref-after-churn:"); });
strongPin = null; /* release; WeakRef may clear at any time from here on */

// 5. FinalizationRegistry cleanup must not crash under churn (ordering is
//    implementation-defined; we only prove no crash and no wrong output)
let finCount = 0;
const fr = new FinalizationRegistry(() => { finCount++; });
(function () {
    for (let i = 0; i < 100; i++) fr.register({ i: i }, i);
})();
churn(50000);
__L(5, "fin: registered ok");

summary("nursery");
