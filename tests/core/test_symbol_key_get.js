// covers: c:src/vm/tailcall.inc.c
// Computed reads with a symbol key (OP_get_array_el/get_array_el2, e.g. it[Symbol.dispose](), o[sym] += 1) skip the
// generic value-to-atom path: the symbol already is its atom, and js_tc_get_sym_prop walks the receiver and its
// prototypes with find_own_property while every object on the way is non-exotic and the hit is a plain data property.
// An exotic object anywhere on the chain (Proxy, array, typed array, arguments, String wrapper), an accessor or lazy
// property, or a chain deeper than 8 hands the read to JS_GetPropertyInternal from the receiver, which is safe because
// the walk itself has no side effects. Primitive, null and undefined receivers keep the old path. Every expected value
// below is what node 26 prints for the same code; the cases pin own and inherited hits, misses, null prototypes,
// getters (receiver binding, throwing, chain mutation inside the getter), setter-only accessors, Proxy traps at the
// receiver and mid-chain, primitive and nullish receivers, arrays and typed arrays, builtins and classes, method calls,
// fresh symbols whose only reference is the operand stack, chains past the walk limit, shape changes between reads,
// frozen and non-enumerable properties, the well-known symbols on every iterator kind, and compound assignment.
import { sections } from "../lib/kit.js";
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const S = sections("test_symbol_key_get");
const S1 = Symbol("s1"), S2 = Symbol.for("reg");
const R = (name, f) => S.section(name, () => { let v; try { v = f(); } catch (e) { v = "threw " + e.constructor.name + ": " + e.message; } const got = JSON.stringify(v); ok(got === WANT[name], name + ": got " + got + ", want " + WANT[name]); });
const WANT = {
    "own and proto data":"[1,1,1,3,null,null]",
    "missing and null proto":"[null,\"n\",null,true]",
    "getter receiver own and inherited":"[\"base!\",\"kid!\",[\"base\",\"kid\"]]",
    "getter throws":"\"RangeErrorg\"",
    "setter only":"[null,null]",
    "proxy traps":"[\"t\",\"t\",null,[\"Symbol(s1)\",\"Symbol(s1)\",\"Symbol(reg)\"]]",
    "proxy in middle of chain":"[\"trapped\",\"trapped\"]",
    "primitive receivers":"[\"len:3\",\"num\",null,null,null]",
    "null undefined receivers":"[\"TypeError\",\"TypeError\"]",
    "arrays and typed arrays":"[\"arr\",\"ta\",true,true,true]",
    "functions classes builtins":"[\"st\",\"inst\",\"function\",\"function\",\"Math\",\"JSON\",\"function\"]",
    "method call this binding":"[\"o\",\"k\",true,\"function\",null]",
    "fresh symbol freed after read":"[2000,true]",
    "symbol held only by stack":"1999000",
    "deep chain past walk limit":"[\"deep\",null]",
    "getter mutates chain":"[\"g\",null,null]",
    "shape changes between reads":"[null,null,\"p\",\"p\",\"own\",\"p\"]",
    "non-writable non-enumerable frozen":"[\"fz\",\"fz\"]",
    "well-known on iterators":"[[\"function\",\"function\",\"Array Iterator\"],[\"function\",\"function\",\"Map Iterator\"],[\"function\",\"function\",\"Set Iterator\"],[\"function\",\"function\",\"String Iterator\"],[\"function\",\"function\",\"Generator\"],[\"function\",\"function\",\"Iterator Helper\"]]",
    "arguments and bound and regexp":"[[\"args\",\"function\"],\"function\",\"function\",\"function\"]",
    "string object and wrapper":"[\"so\",\"function\",null]",
    "module-like namespace via import meta absent":"[\"Module\",\"Module\"]",
    "keep variant compound assignment":"[3,30,true]",
    "optional chaining and delete":"[\"deep\",null,true,null]",
};
R("own and proto data", () => { const a = { [S1]: 1 }; const b = Object.create(a); const c = Object.create(b); c[S2] = 3; return [a[S1], b[S1], c[S1], c[S2], b[S2], Object.create(null)[S1]]; });
R("missing and null proto", () => { const o = Object.create(null); o[S1] = "n"; const p = Object.create(o); return [o[S2], p[S1], p[Symbol.iterator], ({})[Symbol.dispose] === undefined]; });
R("getter receiver own and inherited", () => { const log = []; const base = { get [S1]() { log.push(this.tag); return this.tag + "!"; } }; const kid = Object.assign(Object.create(base), { tag: "kid" }); base.tag = "base"; return [base[S1], kid[S1], log]; });
R("getter throws", () => { const o = { get [S1]() { throw new RangeError("g"); } }; try { return o[S1]; } catch (e) { return e.constructor.name + e.message; } });
R("setter only", () => { const o = { set [S1](v) {} }; return [o[S1], Object.create(o)[S1]]; });
R("proxy traps", () => { const log = []; const t = { [S1]: "t" }; const p = new Proxy(t, { get(tt, k, r) { log.push(String(k)); return Reflect.get(tt, k, r); } }); const kid = Object.create(p); return [p[S1], kid[S1], kid[S2], log]; });
R("proxy in middle of chain", () => { const top = { [S1]: "top" }; const mid = new Proxy(Object.create(top), { get(t, k, r) { return k === S1 ? "trapped" : Reflect.get(t, k, r); } }); return [Object.create(mid)[S1], Object.create(Object.create(mid))[S1]]; });
R("primitive receivers", () => { Object.defineProperty(String.prototype, S1, { get() { return "len:" + this.length; }, configurable: true }); Number.prototype[S1] = "num"; const r = ["abc"[S1], (5)[S1], true[S1], 10n[S1], Symbol()[S1]]; delete String.prototype[S1]; delete Number.prototype[S1]; return r; });
R("null undefined receivers", () => { const r = []; for (const v of [null, undefined]) { try { r.push(v[S1]); } catch (e) { r.push(e.constructor.name); } } return r; });
R("arrays and typed arrays", () => { const a = [1, 2]; a[S1] = "arr"; const t = new Uint8Array(2); t[S1] = "ta"; return [a[S1], t[S1], a[Symbol.iterator] === Array.prototype.values, t[Symbol.iterator] === Uint8Array.prototype.values, a[Symbol.unscopables] !== undefined]; });
R("functions classes builtins", () => { class C { static [S1]() { return "st"; } [S1]() { return "inst"; } } return [C[S1](), new C()[S1](), typeof Function.prototype[Symbol.hasInstance], typeof Map.prototype[Symbol.iterator], Math[Symbol.toStringTag], JSON[Symbol.toStringTag], typeof Promise[Symbol.species]]; });
R("method call this binding", () => { const o = { tag: "o", [S1]() { return this.tag; } }; const k = Object.assign(Object.create(o), { tag: "k" }); const it = [1, 2].values(); return [o[S1](), k[S1](), it[Symbol.iterator]() === it, typeof it[Symbol.dispose], it[Symbol.dispose]()]; });
R("fresh symbol freed after read", () => { const r = []; for (let i = 0; i < 2000; i++) { const o = { [Symbol("x" + i)]: i }; r.push(o[Symbol("x" + i)]); } return [r.length, r.every((x) => x === undefined)]; });
R("symbol held only by stack", () => { let n = 0; for (let i = 0; i < 2000; i++) { const s = Symbol(); const o = { [s]: i }; n += o[s]; } return n; });
R("deep chain past walk limit", () => { let o = { [S1]: "deep" }; for (let i = 0; i < 40; i++) o = Object.create(o); let q = {}; for (let i = 0; i < 40; i++) q = Object.create(q); return [o[S1], q[S1]]; });
R("getter mutates chain", () => { const top = { [S2]: "top" }; const mid = Object.create(top); const o = Object.create(mid); Object.defineProperty(mid, S1, { get() { Object.setPrototypeOf(mid, null); delete top[S2]; return "g"; }, configurable: true }); return [o[S1], o[S2], top[S2]]; });
R("shape changes between reads", () => { const r = []; const proto = {}; const o = Object.create(proto); for (let i = 0; i < 6; i++) { if (i === 2) proto[S1] = "p"; if (i === 4) o[S1] = "own"; if (i === 5) delete o[S1]; r.push(o[S1]); } return r; });
R("non-writable non-enumerable frozen", () => { const o = Object.freeze(Object.defineProperty({}, S1, { value: "fz", enumerable: false })); return [o[S1], Object.create(o)[S1]]; });
R("well-known on iterators", () => { const its = [[1].values(), new Map().keys(), new Set().values(), "ab"[Symbol.iterator](), (function* () {})(), [1].values().map((x) => x)]; return its.map((it) => [typeof it[Symbol.iterator], typeof it[Symbol.dispose], it[Symbol.toStringTag]]); });
R("arguments and bound and regexp", () => { function f() { arguments[S1] = "args"; return [arguments[S1], typeof arguments[Symbol.iterator]]; } const re = /a/g; return [f(1), typeof re[Symbol.match], typeof re[Symbol.split], typeof f.bind(null)[Symbol.hasInstance]]; });
R("string object and wrapper", () => { const so = new String("xy"); so[S1] = "so"; return [so[S1], typeof so[Symbol.iterator], Object(1n)[S1]]; });
R("module-like namespace via import meta absent", () => { const ns = Object.freeze(Object.create(null, { [Symbol.toStringTag]: { value: "Module" } })); return [ns[Symbol.toStringTag], Object.create(ns)[Symbol.toStringTag]]; });
R("keep variant compound assignment", () => { const o = { [S1]: 1 }; o[S1] += 2; o[S1] ??= 9; const k = Object.create(o); k[S1] *= 10; return [o[S1], k[S1], Object.hasOwn(k, S1)]; });
R("optional chaining and delete", () => { const o = { [S1]: { [S2]: "deep" } }; const n = null; return [o?.[S1]?.[S2], n?.[S1], delete o[S1], o[S1]]; });
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
