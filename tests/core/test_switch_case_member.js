// covers: c:src/vm/tailcall.inc.c
// ENGINE-PERF section 8: `switch (k) { case N.A: ... }` compiles each case to one fused op that reads the
// variable, loads the property and compares. It now (1) also fuses a closure or module variable (get_var_ref and
// get_var_ref_check, not only a global), and (2) remembers, per bytecode site, the slot index where the property
// was found on the object itself; a hit is used only when that slot of the object's CURRENT shape still holds the
// same atom as a plain data property, so a delete, a redefinition as accessor, a new shape or another object
// with a different layout all fall back to the full lookup. Semantics that must not move: case expressions run
// in order and stop at the first match, getters and Proxy traps run exactly then, values are read fresh on every
// evaluation, inherited properties and TDZ errors behave as a plain `N.A` read does. Oracle: the same cases as
// an if-chain over computed reads (N[key], never fused), on random mutation sequences.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_switch_case_member");

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b), m + " (got " + String(a) + ", want " + String(b) + ")"); }
function throwsName(fn) { try { fn(); return null; } catch (e) { return e.constructor.name + ": " + e.message; } }

const N = { A: 1, B: 2, C: 3, D: 4 };
function pick(k) { switch (k) { case N.A: return "a"; case N.B: return "b"; case N.C: return "c"; case N.D: return "d"; } return "-"; }

__kitS.section("closure const object: values are read fresh every time", () => {
    for (let r = 0; r < 3; r++) {
        eq(pick(1), "a", "A"); eq(pick(4), "d", "D"); eq(pick(9), "-", "none");
    }
    N.B = 9;
    eq(pick(9), "b", "after N.B = 9");
    eq(pick(2), "-", "old value of B no longer matches");
    N.B = 2;
    delete N.A;
    eq(pick(1), "-", "deleted A reads undefined");
    eq(pick(undefined), "a", "undefined matches the deleted (undefined) A");
    N.A = 1;
    eq(pick(1), "a", "re-added A (new slot)");
    Object.defineProperty(N, "C", { get() { return 7; }, configurable: true, enumerable: true });
    eq(pick(7), "c", "C redefined as a getter");
    Object.defineProperty(N, "C", { value: 3, writable: true, configurable: true, enumerable: true });
    eq(pick(3), "c", "C back to data");
    for (let i = 0; i < 40; i++) N["x" + i] = i;
    for (let i = 0; i < 40; i++) delete N["x" + i];
    eq(pick(4), "d", "after many adds and deletes");
});

__kitS.section("evaluation order and short-circuit with getters", () => {
    const log = [];
    const G = {};
    for (const k of ["A", "B", "C", "D"]) Object.defineProperty(G, k, { get() { log.push(k); return k.charCodeAt(0) - 64; } });
    function g(k) { switch (k) { case G.A: return 1; case G.B: return 2; case G.C: return 3; case G.D: return 4; } return 0; }
    for (let r = 0; r < 3; r++) {
        log.length = 0;
        eq(g(3), 3, "match C");
        eq(log.join(""), "ABC", "getters A, B, C ran in order and D did not");
    }
    log.length = 0;
    eq(g(0), 0, "no match");
    eq(log.join(""), "ABCD", "every getter once");
});

__kitS.section("a case expression's side effect changes a later case", () => {
    const M = { a: 1, b: 2 };
    const S = { get f() { M.b = 5; return 100; } };
    function h(k) { switch (k) { case M.a: return "a"; case S.f: return "f"; case M.b: return "b"; } return "-"; }
    eq(h(2), "-", "S.f set M.b = 5 before M.b was read");
    eq(h(5), "b", "M.b now 5");
    const D = { a: 1, b: 2 };
    const T = { get f() { delete D.b; Object.defineProperty(D, "b", { get() { return 42; }, configurable: true }); return -1; } };
    function i2(k) { switch (k) { case D.a: return "a"; case T.f: return "f"; case D.b: return "b"; } return "-"; }
    eq(i2(2), "-", "b redefined as getter mid-switch");
    eq(i2(42), "b", "the getter is used");
});

__kitS.section("inherited properties, shadowing, prototypes changing", () => {
    const proto = { P: 10 };
    const o = Object.create(proto);
    o.Q = 20;
    function f(k) { switch (k) { case o.P: return "p"; case o.Q: return "q"; } return "-"; }
    for (let r = 0; r < 3; r++) eq(f(10), "p", "inherited P");
    o.P = 11;
    eq(f(11), "p", "own P shadows");
    eq(f(10), "-", "inherited value hidden");
    delete o.P;
    eq(f(10), "p", "shadow removed: inherited again");
    proto.P = 12;
    eq(f(12), "p", "prototype value changed");
    Object.setPrototypeOf(o, { P: 13 });
    eq(f(13), "p", "prototype replaced");
});

__kitS.section("one site, many objects of different layouts", () => {
    let cur;
    function f(k) { switch (k) { case cur.K: return "k"; case cur.L: return "l"; } return "-"; }
    const objs = [{ K: 1, L: 2 }, { L: 2, K: 1 }, { z: 0, K: 1, L: 2 }, { K: 1 }, { L: 2 }, Object.assign(Object.create(null), { K: 1, L: 2 }), [1, 2]];
    objs[6].K = 1; objs[6].L = 2;
    for (let r = 0; r < 4; r++) for (const o of objs) {
        cur = o;
        eq(f(1), o.K === 1 ? "k" : "-", "K on " + JSON.stringify(o));
        eq(f(2), o.L === 2 ? "l" : (o.K === 2 ? "k" : "-"), "L on " + JSON.stringify(o));
    }
});

__kitS.section("value kinds: strings, doubles, -0, NaN, objects", () => {
    const sym = Symbol("s"), obj = {};
    const V = { s: "str" + "ing", d: 1.5, z: -0, nan: NaN, o: obj, y: sym, big: 10n };
    function f(k) { switch (k) { case V.s: return "s"; case V.d: return "d"; case V.z: return "z"; case V.nan: return "nan"; case V.o: return "o"; case V.y: return "y"; case V.big: return "big"; } return "-"; }
    for (let r = 0; r < 2; r++) {
        eq(f("string"), "s", "string equal by value");
        eq(f(1.5), "d", "double");
        eq(f(0), "z", "0 === -0");
        eq(f(NaN), "-", "NaN never matches");
        eq(f(obj), "o", "object identity");
        eq(f({}), "-", "other object");
        eq(f(sym), "y", "symbol");
        eq(f(10n), "big", "bigint");
    }
});

__kitS.section("Proxy, primitives and arrays as the case object", () => {
    const seen = [];
    const P = new Proxy({ A: 1, B: 2 }, { get(t, k) { seen.push(String(k)); return t[k]; } });
    function f(k) { switch (k) { case P.A: return "a"; case P.B: return "b"; } return "-"; }
    eq(f(2), "b", "proxy B"); eq(seen.join(","), "A,B", "traps in order");
    let s = "abc";
    function g(k) { switch (k) { case s.length: return "len"; } return "-"; }
    eq(g(3), "len", "string primitive length"); s = "abcd"; eq(g(4), "len", "new string");
    const arr = [1, 2, 3];
    function h(k) { switch (k) { case arr.length: return "len"; } return "-"; }
    eq(h(3), "len", "array length"); arr.push(4); eq(h(4), "len", "array grew");
});

__kitS.section("null/undefined object and TDZ errors match a plain read", () => {
    let Z = null;
    function f(k) { switch (k) { case Z.A: return 1; } return 0; }
    function plain() { return Z.A; }
    eq(throwsName(() => f(1)), throwsName(plain), "null object");
    Z = undefined;
    eq(throwsName(() => f(1)), throwsName(plain), "undefined object");
    function tdz(k) {
        const early = () => { switch (k) { case L.A: return 1; } return 0; };
        const earlyPlain = () => L.A;
        const a = throwsName(early), b = throwsName(earlyPlain);
        const L = { A: 1 };
        return [a, b, early()];
    }
    const [a, b, after] = tdz(1);
    ok(a !== null && a === b, "TDZ in a closure: " + a + " vs " + b);
    eq(after, 1, "after init");
});

__kitS.section("differential: switch vs if-chain over computed reads, random mutations", () => {
    let seed = 12345;
    const rnd = (m) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % m; };
    const KEYS = ["a", "b", "c", "d", "e", "f"];
    const O = {};
    for (const k of KEYS) O[k] = rnd(5);
    const body = KEYS.map((k, i) => `case O.${k}: return ${i};`).join(" ");
    const sw = new Function("O", `return function (v) { switch (v) { ${body} } return -1; };`)(O);
    const oracle = (v) => { for (let i = 0; i < KEYS.length; i++) if (v === O[KEYS[i]]) return i; return -1; };
    let mism = 0;
    for (let step = 0; step < 4000; step++) {
        const op = rnd(10), k = KEYS[rnd(KEYS.length)];
        if (op === 0) delete O[k];
        else if (op === 1) { const d = Object.getOwnPropertyDescriptor(O, k); if (d && d.get) delete O[k]; O[k] = rnd(5); }
        else if (op === 2) Object.defineProperty(O, k, { get: ((x) => () => x)(rnd(5)), configurable: true, enumerable: true });
        else if (op === 3) Object.defineProperty(O, k, { value: rnd(5), writable: true, configurable: true, enumerable: true });
        else if (op === 4) O["t" + rnd(20)] = 1;
        else if (op === 5) delete O["t" + rnd(20)];
        const v = rnd(6);
        if (sw(v) !== oracle(v)) { mism++; if (mism < 5) print("FAIL: step " + step + " v=" + v + " switch=" + sw(v) + " oracle=" + oracle(v)); }
    }
    eq(mism, 0, "mismatches over 4000 random steps");
});

print("test_switch_case_member: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
__kitS.end();
