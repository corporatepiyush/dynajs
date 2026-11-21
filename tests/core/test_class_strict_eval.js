// covers: integration c:src/parser/parser.inc.c c:src/vm/interpreter.inc.c c:src/vm/tailcall.inc.c c:src/dyna-opcode2.h -- class heritage and computed keys run in strict mode inside sloppy code
// flags: --std
// All parts of a ClassDeclaration/ClassExpression are strict mode code
// (ECMA-262 11.2.2; test262 staging/sm/class/strictExecution.js). The parser
// compiled the heritage and computed keys as strict, but runtime strictness
// comes from the frame mode, which kept the enclosing sloppy mode. A sloppy
// frame now brackets the class with OP2_enter_strict / OP2_leave_strict
// (leave is emitted before OP_set_class_name so NamedEvaluation still
// patches define_class), and a catch in that frame restores the function's
// own mode. Global/eval code runs on the switch interpreter, normal
// functions on the tailcall one; both are covered. A return that leaves a
// generator suspended in a key (it.return) leaves strict before running
// finally blocks (emit_return in a sloppy-hosted class), and a direct eval
// takes strictness from the frame, so eval in a key or heritage is strict.
import { suite } from "../lib/kit.js";

const t = suite("class_strict_eval");
const G = globalThis;
const glob = (src) => (0, eval)(src);
const fn = (src, ...args) => new Function(src)(...args);

const errName = (f) => {
    try { f(); } catch (e) { return e.name; }
    return "none";
};

const PRE = `var cseFrozen = Object.freeze({ k: 1 });`;
const KEY_WRITE = `(class { [cseFrozen.k = 2]() {} })`;
const HERITAGE = `(class extends (cseUndeclH = Object) {})`;
const KEY_UNDECL = `(class { [cseUndeclK = "m"]() {} })`;

t.test("computed key write to a frozen object throws TypeError", (a) => {
    a.eq(errName(() => glob(`${PRE} ${KEY_WRITE}; 0`)), "TypeError", "global class expression");
    a.eq(errName(() => glob(`${PRE} class C1 { [cseFrozen.k = 2]() {} }`)), "TypeError", "global declaration");
    a.eq(errName(() => fn(`${PRE} return ${KEY_WRITE};`)), "TypeError", "function class expression");
    a.eq(errName(() => fn(`${PRE} class C2 { static [cseFrozen.k = 2] = 1 }`)), "TypeError", "function static field key");
});

t.test("computed key and heritage assignment to an undeclared name throw ReferenceError", (a) => {
    a.eq(errName(() => glob(`${KEY_UNDECL}; 0`)), "ReferenceError", "global key");
    a.eq(errName(() => fn(`return ${KEY_UNDECL};`)), "ReferenceError", "function key");
    a.eq(errName(() => glob(`${HERITAGE}; 0`)), "ReferenceError", "global heritage");
    a.eq(errName(() => fn(`return ${HERITAGE};`)), "ReferenceError", "function heritage");
    a.eq("cseUndeclK" in G, false, "no global created by key");
    a.eq("cseUndeclH" in G, false, "no global created by heritage");
});

t.test("code after the class is sloppy again", (a) => {
    a.eq(glob(`class C3 { ["m"]() {} } cseAfterG = 3; cseAfterG`), 3, "global after declaration");
    a.eq(fn(`const C = class { ["m"]() {} }; cseAfterF = 4; return cseAfterF;`), 4, "function after expression");
    a.eq(fn(`const C = class extends Object {}; const o = Object.freeze({ k: 1 }); o.k = 2; return o.k;`), 1, "frozen write after class is silent");
    delete G.cseAfterG;
    delete G.cseAfterF;
});

t.test("a catch in the same frame restores sloppy mode", (a) => {
    const thrower = `(() => { throw new Error("x"); })()`;
    a.eq(glob(`try { (class { [${thrower}]() {} }); } catch (e) {} cseCatchG = 5; cseCatchG`), 5, "global");
    a.eq(fn(`try { (class { [${thrower}]() {} }); } catch (e) {} cseCatchF = 6; return cseCatchF;`), 6, "function");
    a.eq(fn(`${PRE} try { (class { [cseFrozen.k = 2]() {} }); } catch (e) {} cseFrozen.k = 3; cseCatchF2 = 7; return cseFrozen.k + cseCatchF2;`), 8, "after a strict TypeError");
    delete G.cseCatchG;
    delete G.cseCatchF;
    delete G.cseCatchF2;
});

t.test("generator and async computed keys", async (a) => {
    const gen = glob(`(function* () { const C = class { [yield "k"]() {} }; cseGen = 8; return Object.getOwnPropertyNames(C.prototype).join(); })`);
    const it = gen();
    a.eq(it.next().value, "k", "yielded from the key");
    a.eq(it.next("m").value, "constructor,m", "key used and sloppy afterwards");
    a.eq(G.cseGen, 8, "sloppy assignment after resume");
    delete G.cseGen;
    const asyncKey = glob(`(async function () { const C = class { [await "n"]() {} }; cseAsync = 9; return typeof C.prototype.n; })`);
    a.eq(await asyncKey(), "function", "await in key");
    a.eq(G.cseAsync, 9, "sloppy assignment after await");
    delete G.cseAsync;
    const asyncStrict = glob(`(async function () { (class { [cseUndeclA = await 1]() {} }); })`);
    let name = "none";
    try { await asyncStrict(); } catch (e) { name = e.name; }
    a.eq(name, "ReferenceError", "strict after await inside the key");
});

t.test("anonymous classes keep their inferred names", (a) => {
    a.eq(glob(`var cseN1 = class { ["m"]() {} }; cseN1.name`), "cseN1", "var initializer");
    a.eq(fn(`let x = class extends Object {}; return x.name;`), "x", "let initializer");
    a.eq(fn(`const o = { p: class { static [1 + 1] = 2 } }; return o.p.name;`), "p", "property");
    a.eq(fn(`let y; y ??= class {}; return y.name;`), "y", "logical assignment");
    delete G.cseN1;
});

t.test("strict callers and nested classes are unchanged", (a) => {
    a.eq(errName(() => fn(`"use strict"; return ${KEY_UNDECL};`)), "ReferenceError", "strict function");
    a.eq(fn(`const C = class { m() { return class { [1]() {} }; } }; new C().m(); cseNest = 10; return cseNest;`), 10, "nested class then sloppy");
    a.eq(fn(`return (function () { return this; })() === globalThis;`), true, "sloppy this after classes");
    delete G.cseNest;
});

t.test("hostile keys, Proxy heritage, NUL keys and limits", (a) => {
    a.eq(fn(`try { (class { [{ toString() { throw new Error("k"); } }]() {} }); } catch (e) {} cseHost1 = 1; return cseHost1;`), 1, "throwing toString key then sloppy");
    a.eq(errName(() => fn(`(class { [{ toString() { cseHostU = 1; return "m"; } }]() {} });`)), "ReferenceError", "a method written inside the key is strict code");
    a.eq(errName(() => fn(`const h = { toString() { cseHostU = 1; return "m"; } }; (class { [h]() {} });`)), "none", "toString defined outside keeps its sloppy mode");
    a.eq(errName(() => fn(`(class { [{ toString() { return "m"; }, valueOf() { throw 1; } }]() {} });`)), "none", "valueOf is not used for keys");
    a.eq(errName(() => fn(`(class { [{ [Symbol.toPrimitive]() { return cseUndeclP = "m"; } }]() {} });`)), "ReferenceError", "toPrimitive written inside the key is strict");
    a.eq(errName(() => fn(`const h = { [Symbol.toPrimitive]() { return cseUndeclP = "m"; } }; (class { [h]() {} });`)), "none", "toPrimitive defined outside is sloppy");
    const P = `new Proxy(function () {}, { get(t, k) { if (k === "prototype") throw new TypeError("p"); return t[k]; } })`;
    a.eq(errName(() => fn(`(class extends ${P} {});`)), "TypeError", "Proxy heritage prototype getter throws");
    a.eq(fn(`try { (class extends ${P} {}); } catch (e) {} cseHost2 = 2; return cseHost2;`), 2, "sloppy after Proxy heritage throw");
    a.eq(fn(`const o = { get k() { return "g"; } }; const C = class { [o.k]() {} }; cseHost3 = 3; return typeof C.prototype.g + cseHost3;`), "function3", "getter key");
    a.eq(fn(`const C = class { ["a\\0b"]() { return 5; } }; cseHost4 = 4; return new C()["a\\0b"]() + cseHost4;`), 9, "NUL in key");
    a.eq(fn(`return (class { [""]() { return 0; } }).prototype[""]();`), 0, "empty key");
    let deep = "return 1";
    for (let i = 0; i < 200; i++) deep = `const C${i} = class { [(() => 1)()]() {} static m() { ${deep} } }; return C${i}.m();`;
    a.eq(fn(deep), 1, "200 nested classes");
    a.eq(errName(() => fn(`(class { static m() { cseDeepU = 1; } }).m();`)), "ReferenceError", "method body is strict");
    a.eq(errName(() => fn(`(class { [cseUndeclZ = 0]() {} });`)), "ReferenceError", "zero key");
    for (const k of ["cseHost1", "cseHostU", "cseUndeclP", "cseHost2", "cseHost3", "cseHost4", "cseDeep"]) delete G[k];
});

t.test("GC while a generator is suspended inside a computed key", (a) => {
    const gen = glob(`(function* () { const C = class { [yield 1]() {} }; cseGc = 1; return Object.getOwnPropertyNames(C.prototype).length; })`);
    const its = [];
    for (let i = 0; i < 64; i++) { const it = gen(); it.next(); its.push(it); }
    std.gc();
    let sum = 0;
    for (const it of its) sum += it.next("k").value;
    std.gc();
    a.eq(sum, 128, "every resumed generator finished the class");
    a.eq(G.cseGc, 1, "sloppy after resume");
    delete G.cseGc;
    const dropped = gen();
    dropped.next();
    std.gc();
    a.eq(dropped.return(7).value, 7, "a suspended class key can be closed");
});

t.test("generator return through finally after a key yield runs sloppy", (a) => {
    const g = fn(`return function* () { try { (class { [yield 1]() {} }); } finally { cseFinF = 1; } };`);
    const it = g();
    it.next();
    a.eq(JSON.stringify(it.return(5)), `{"value":5,"done":true}`, "return completes");
    a.eq(G.cseFinF, 1, "finally assigned an undeclared name (tailcall-created generator)");
    const g2 = glob(`(function* () { try { (class extends (yield 1) {}); } finally { cseFinG = 2; } })`);
    const it2 = g2();
    it2.next();
    it2.return(6);
    a.eq(G.cseFinG, 2, "heritage yield, global function");
    const g3 = glob(`(async function* () { try { (class { [yield 1]() {} }); } finally { cseFinA = 3; } })`);
    const it3 = g3();
    return it3.next().then(() => it3.return(7)).then((r) => {
        a.eq(r.value, 7, "async generator return value");
        a.eq(G.cseFinA, 3, "async generator finally sloppy");
        const g4 = glob(`(function* () { for (const x of [1]) { (class { [yield x]() {} }); } cseFinD = 4; })`);
        const it4 = g4();
        it4.next();
        a.eq(it4.return(8).value, 8, "return closes the for-of iterator from inside a key");
        for (const k of ["cseFinF", "cseFinG", "cseFinA", "cseFinD"]) delete G[k];
    });
});

t.test("direct eval inside a class key or heritage is strict code", (a) => {
    a.eq(errName(() => fn(`(class { [eval("cseEvU = 1; 'm'")]() {} });`)), "ReferenceError", "function key");
    a.eq(errName(() => glob(`(class { [eval("cseEvG = 1; 'm'")]() {} }); 0`)), "ReferenceError", "global key");
    a.eq(errName(() => fn(`(class extends eval("cseEvH = 1; Object") {});`)), "ReferenceError", "heritage");
    a.eq(fn(`(class { [eval("var cseEvV = 1; 'm'")]() {} }); return typeof cseEvV;`), "undefined", "strict eval keeps its vars");
    a.eq(fn(`(class { [1]() {} }); eval("var cseEvAfter = 2"); return cseEvAfter;`), 2, "eval after the class is sloppy");
    a.eq(fn(`(class { [1]() {} }); return eval("cseEvS = 3");`), 3, "sloppy eval after the class creates a global");
    a.eq(fn(`return Function("cseFk = 4; return 'm'")() + typeof (class { [Function("cseFk2 = 5; return 'n'")()]() {} }).prototype.n;`), "mfunction", "Function constructor is never strict by context");
    a.eq(errName(() => fn(`(class { [(() => (cseArrowK = 1, "m"))()]() {} });`)), "ReferenceError", "arrow written in a key is strict");
    for (const k of ["cseEvAfter", "cseEvS", "cseFk", "cseFk2"]) delete G[k];
});

await t.run();
