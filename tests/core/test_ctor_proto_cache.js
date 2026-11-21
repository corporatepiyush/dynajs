// covers: c:src/object/shapes_objects_gc.inc.c
// A constructor whose body assigns this.<field> gets a presized instance shape
// (the fields are pre-added). That is only legal while no object on the
// instance's prototype chain has a setter or a non-writable property for one
// of those fields, and the chain check is cached by the shapes of the chain
// objects. Each case below changes the chain AFTER instances were created and
// requires the next `new` to see the change; the reference is what plain
// assignment semantics produce.

let failures = 0;
function eq(actual, expected, what) {
    if (actual !== expected) {
        failures++;
        console.log("FAIL", what, "got", JSON.stringify(actual), "expected", JSON.stringify(expected));
    }
}

function warm(C, n, ...args) {
    let last;
    for (let i = 0; i < n; i++)
        last = new C(...args);
    return last;
}

import("../lib/kit.js").then(async ({ sections }) => {
const S = sections("test_ctor_proto_cache");
S.section("setter added to the prototype after instances runs", () => {
    function P(x, y) { this.x = x; this.y = y; }
    const a = warm(P, 50, 1, 2);
    eq(Object.keys(a).join(), "x,y", "plain fields");
    let seen = 0;
    Object.defineProperty(P.prototype, "x", { set(v) { seen += v; }, get() { return "accessor"; }, configurable: true });
    const b = new P(5, 6);
    eq(seen, 5, "setter added to the prototype after instances runs");
    eq(Object.keys(b).join(), "y", "setter-shadowed field is not an own property");
    eq(b.x, "accessor", "getter visible");
    delete P.prototype.x;
    const c = new P(7, 8);
    eq(Object.keys(c).join(), "x,y", "deleting the accessor restores own fields");
    eq(c.x, 7, "field value after delete");
});

S.section("field after removing the non-writable proto property", () => {
    function Q(v) { this.v = v; }
    warm(Q, 50, 1);
    Object.defineProperty(Object.prototype, "v", { value: 0, writable: false, configurable: true });
    let threw = false;
    try {
        (function () { "use strict"; new Q(3); })();
        const q = new Q(3);
        eq(Object.prototype.hasOwnProperty.call(q, "v"), false, "non-writable on Object.prototype blocks the own field");
    } catch (e) {
        threw = true;
    }
    delete Object.prototype.v;
    const q2 = new Q(9);
    eq(q2.v, 9, "field after removing the non-writable proto property");
    eq(threw, false, "sloppy constructor does not throw on non-writable proto field");
});

S.section("prototype reassignment: setter on the new prototype runs", () => {
    function R(a) { this.a = a; }
    warm(R, 50, 1);
    const old = R.prototype;
    let hits = 0;
    R.prototype = { set a(v) { hits++; } };
    const r = new R(4);
    eq(hits, 1, "prototype reassignment: setter on the new prototype runs");
    eq(Object.getPrototypeOf(r) === R.prototype, true, "instance uses the new prototype");
    R.prototype = old;
    eq(new R(5).a, 5, "back to the old prototype");
});

S.section("setPrototypeOf on the prototype inserts a setter mid-chain", () => {
    function S(a) { this.a = a; }
    warm(S, 50, 1);
    let hits = 0;
    const mid = { set a(v) { hits += 10; } };
    Object.setPrototypeOf(S.prototype, mid);
    const s = new S(2);
    eq(hits, 10, "setPrototypeOf on the prototype inserts a setter mid-chain");
    eq(Object.keys(s).join(), "", "no own field under the inherited setter");
    Object.setPrototypeOf(S.prototype, Object.prototype);
    eq(new S(3).a, 3, "chain restored");
});

S.section("new.target with a different prototype is checked", () => {
    function T(a) { this.a = a; }
    function U() {}
    let hits = 0;
    U.prototype = { set a(v) { hits++; } };
    warm(T, 50, 1);
    const t = Reflect.construct(T, [8], U);
    eq(hits, 1, "new.target with a different prototype is checked");
    eq(Object.getPrototypeOf(t) === U.prototype, true, "new.target prototype used");
    eq(new T(2).a, 2, "plain new afterwards");
});

S.section("Proxy new.target: prototype read through the trap", () => {
    function V(a) { this.a = a; }
    warm(V, 50, 1);
    let protoReads = 0;
    const target = new Proxy(function () {}, {
        get(t, k) {
            if (k === "prototype") {
                protoReads++;
                return { marker: 1 };
            }
            return t[k];
        },
    });
    const v = Reflect.construct(V, [3], target);
    eq(protoReads, 1, "Proxy new.target: prototype read through the trap");
    eq(Object.getPrototypeOf(v).marker, 1, "Proxy-provided prototype used");
});

S.section("bound constructor sees a setter added later", () => {
    function W(a, b) { this.a = a; this.b = b; }
    const BW = W.bind(null, 1);
    warm(BW, 50, 2);
    let hits = 0;
    Object.defineProperty(W.prototype, "b", { set(v) { hits += v; }, configurable: true });
    const w = new BW(4);
    eq(hits, 4, "bound constructor sees a setter added later");
    eq(w.a, 1, "bound argument");
});

S.section("derived constructor: setter on the base prototype", () => {
    class Base { constructor(x) { this.x = x; } }
    class Derived extends Base { constructor(x) { super(x); this.y = x + 1; } }
    warm(Derived, 50, 1);
    let hits = 0;
    Object.defineProperty(Base.prototype, "y", { set(v) { hits += v; }, configurable: true });
    const d = new Derived(5);
    eq(hits, 6, "derived constructor: setter on the base prototype");
    eq(d.x, 5, "base field");
});

S.section("class with field initializer: setter for a constructor-assigned field", () => {
    class F { a = 1; constructor(b) { this.b = b; } }
    warm(F, 50, 2);
    let hits = 0;
    Object.defineProperty(F.prototype, "b", { set(v) { hits += v; }, configurable: true });
    const f = new F(3);
    eq(hits, 3, "class with field initializer: setter for a constructor-assigned field");
    eq(Object.getOwnPropertyDescriptor(f, "a").value, 1, "field initializer defines, not sets");
    Object.defineProperty(F.prototype, "a", { set(v) { hits += 100; }, configurable: true });
    const f2 = new F(0);
    eq(Object.getOwnPropertyDescriptor(f2, "a").value, 1, "field initializer ignores a prototype setter");
    eq(hits, 3, "field initializer did not call the setter");
});

S.section("a setter added and removed per round is seen each time", () => {
    function X(a) { this.a = a; }
    for (let round = 0; round < 3; round++) {
        warm(X, 20, round);
        let hits = 0;
        Object.defineProperty(X.prototype, "a", { set(v) { hits++; }, configurable: true });
        new X(1);
        eq(hits, 1, "round " + round + ": setter seen");
        delete X.prototype.a;
        eq(new X(round).a, round, "round " + round + ": own field again");
    }
});

S.section("inherited non-writable value", () => {
    function Y(k) { this.k = k; }
    const base = { k: 0 };
    Y.prototype = Object.create(base);
    warm(Y, 50, 1);
    Object.defineProperty(base, "k", { writable: false });
    const y = new Y(5);
    eq(Object.prototype.hasOwnProperty.call(y, "k"), false, "existing proto property made non-writable");
    eq(y.k, 0, "inherited non-writable value");
});

S.section("Object.freeze on a chain object blocks the own field", () => {
    function Z(m) { this.m = m; }
    const base = { m: 0, other: 1 };
    Z.prototype = Object.create(base);
    warm(Z, 50, 1);
    Object.freeze(base);
    const z = new Z(5);
    eq(Object.prototype.hasOwnProperty.call(z, "m"), false, "Object.freeze on a chain object");
});

S.section("setter added to a large (dictionary) chain object", () => {
    function G(q) { this.q = q; }
    const base = {};
    for (let i = 0; i < 40; i++) base["f" + i] = i;
    G.prototype = Object.create(base);
    warm(G, 50, 1);
    let hits = 0;
    Object.defineProperty(base, "q", { set(v) { hits++; }, configurable: true });
    new G(1);
    eq(hits, 1, "setter added to a large (dictionary) chain object");
    delete base.q;
    eq(new G(2).q, 2, "removed again");
    Object.defineProperty(base, "f3", { set(v) {}, configurable: true });
    eq(new G(3).q, 3, "unrelated accessor does not block the field");
});
S.end();

if (failures) {
    console.log("test_ctor_proto_cache: " + failures + " failure(s)");
    throw new Error("test_ctor_proto_cache failed");
}
console.log("test_ctor_proto_cache: ok");
});
