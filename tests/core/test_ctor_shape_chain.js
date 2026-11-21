// covers: integration c:src/object/shapes_objects_gc.inc.c c:src/vm/tailcall.inc.c c:src/vm/interpreter.inc.c -- one fast path spans the shape code and both mirrored interpreters
// Constructor shape-chain retention (js_ctor_presize_learn). A presized constructor's fields past the
// parse-time prefix used to rebuild their transition shapes for every new object (clone, rehash,
// resize), because nothing kept the intermediate shapes alive. After the first construction returns,
// the chain is replayed once on a scratch object and the transition shapes are kept in the presize
// record (GC-marked, freed with the bytecode, reset when the cached shape is rebuilt for another
// prototype). This is representation only: every this.f = v still runs the full [[Set]], so setters
// added to the prototype later are still called. Why: 2M new Tok() went 830 to 670 ms (ENGINE-PERF.md
// item 2, r01/r16). This suite pins semantics around the cache; a leaked chain shape fails the
// runtime teardown assertion.
import { suite } from "../lib/kit.js";
const t = suite("test_ctor_shape_chain");

function keys(o) {
    return Reflect.ownKeys(o).map((k) => {
        const d = Object.getOwnPropertyDescriptor(o, k);
        return String(k) + ("value" in d ? "=" + String(d.value) : "<acc>") + (d.enumerable ? "" : "!e");
    }).join(",");
}

t.test("records built by constructors keep fields, order and values", (a) => {
    function Tok(i) { this.k = 1; this.t = "x"; this.off = i; this.end = i + 1; this.line = 1; this.col = 1; this.sl = 0; this.ws = false; this.p = ""; }
    const all = [];
    for (let i = 0; i < 300; i++) all.push(new Tok(i));
    for (let i = 0; i < 300; i += 37) a.eq(keys(all[i]), `k=1,t=x,off=${i},end=${i + 1},line=1,col=1,sl=0,ws=false,p=`, "Tok " + i);
    class Node { constructor(i) { this.kind = 3; this.start = i; this.end = i + 2; this.kids = null; } }
    let s = 0;
    for (let i = 0; i < 500; i++) { const n = new Node(i); s += n.end; if (i === 499) a.eq(keys(n), "kind=3,start=499,end=501,kids=null", "Node"); }
    a.eq(s, 500 * 499 / 2 + 1000, "values");
});

t.test("conditional and varying fields", (a) => {
    function V(i) { this.a = 1; this.b = 2; if (i % 2) this.odd = i; else this.even = i; this.z = i % 3 ? "x" : undefined; if (i % 5 === 0) delete this.b; }
    for (let i = 0; i < 60; i++) {
        const v = new V(i);
        const want = "a=1" + (i % 5 === 0 ? "" : ",b=2") + (i % 2 ? ",odd=" + i : ",even=" + i) + ",z=" + (i % 3 ? "x" : "undefined");
        a.eq(keys(v), want, "V " + i);
    }
    function G(i) { this.a = i; this.b = 2; Object.defineProperty(this, "g", { get() { return 7; }, enumerable: true }); this.c = 3; }
    for (let i = 0; i < 20; i++) a.eq(keys(new G(i)), `a=${i},b=2,g<acc>,c=3`, "accessor in the middle " + i);
});

t.test("prototype setters added after the chain is learned are still called", (a) => {
    function T(i) { this.a = 1; this.b = 2; this.c = i + 1; this.d = i * 2; }
    for (let i = 0; i < 50; i++) new T(i);
    let hits = 0;
    Object.defineProperty(T.prototype, "d", { set(v) { hits++; }, configurable: true });
    const t1 = new T(5);
    a.eq(hits, 1, "setter for a chained field runs");
    a.eq(keys(t1), "a=1,b=2,c=6", "no own d when the setter swallows it");
    delete T.prototype.d;
    Object.defineProperty(T.prototype, "c", { value: 0, writable: false, configurable: true });
    a.throws(() => new T(5), TypeError, "read-only inherited c makes the strict-mode store throw");
    delete T.prototype.c;
    a.eq(keys(new T(5)), "a=1,b=2,c=6,d=10", "back to normal");
});

t.test("new.target prototypes, returned objects and subclasses", (a) => {
    function T(i) { this.a = 1; this.b = 2; this.c = i + 1; this.d = i; }
    for (let i = 0; i < 20; i++) new T(i);
    function Other() {}
    Other.prototype.marker = "other";
    for (let i = 0; i < 20; i++) {
        const o = Reflect.construct(T, [i], Other);
        a.eq(Object.getPrototypeOf(o), Other.prototype, "new.target prototype " + i);
        a.eq(keys(o), `a=1,b=2,c=${i + 1},d=${i}`, "fields under another prototype " + i);
        a.eq(o.marker, "other", "inherited from new.target.prototype");
    }
    for (let i = 0; i < 5; i++) a.eq(Object.getPrototypeOf(new T(i)), T.prototype, "back on T.prototype");
    function R(i) { this.a = 1; this.b = 2; this.c = i + 1; return { replaced: i }; }
    for (let i = 0; i < 10; i++) a.eq(keys(new R(i)), "replaced=" + i, "returned object wins");
    class Base { constructor(i) { this.a = 1; this.b = 2; this.c = i + 1; } }
    class Sub extends Base { constructor(i) { super(i); this.d = i; } }
    for (let i = 0; i < 20; i++) a.eq(keys(new Sub(i)), `a=1,b=2,c=${i + 1},d=${i}`, "subclass " + i);
});

t.test("many constructors and garbage collection", (a) => {
    const made = [];
    for (let k = 0; k < 50; k++) {
        const C = new Function("i", `this.a${k} = 1; this.b = 2; this.c = i + ${k}; this.d = [i];`);
        for (let i = 0; i < 5; i++) made.push(new C(i));
    }
    a.eq(made.length, 250, "constructed");
    a.eq(keys(made[249]), "a49=1,b=2,c=53,d=4", "last one");
    if (typeof std === "object" && std.gc) std.gc();
});

await t.run();
