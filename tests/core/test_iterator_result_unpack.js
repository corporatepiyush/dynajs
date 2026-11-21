// covers: c:src/object/property_set_convert.inc.c
// Iterator result unpacking (js_iterator_result_unpack, used by for-of, spread, Array.from, destructuring, yield*
// and Iterator.from over a user iterator) reads done and value straight from the result object's own data
// properties when the result is a plain object; anything else (an accessor, an inherited property, a Proxy, an array or
// a function result) takes the ordinary [[Get]]. The spec order is Get(done), ToBoolean, then Get(value) only when not
// done, so a getter log shows each read exactly once. Every expected value below is what node 26 prints for the same
// code (bun 1.3 reads done twice in yield*; node is the oracle). The cases pin truthy and falsy done values,
// missing value, accessors own and inherited, class instances, Proxy traps, deleted and redefined properties,
// frozen and non-enumerable results, non-object results, throwing getters, a result object reused and mutated
// between steps, an accessor replaced by a data property, and a result object in dictionary mode.
import { sections } from "../lib/kit.js";
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
const S = sections("test_iterator_result_unpack");
const mk = (results) => ({ [Symbol.iterator]() { let i = 0; return { next: () => results[i++] }; } });
const consumers = (src) => { const r = []; const a = []; for (const x of src()) a.push(x); r.push(a); r.push([...src()]); r.push(Array.from(src())); r.push(Iterator.from(src()[Symbol.iterator]()).toArray()); const [p, q] = src(); r.push([p, q]); r.push([...(function* () { yield* src(); })()]); return r; };
const R = (name, f) => S.section(name, () => { let v; try { v = f(); } catch (e) { v = "threw " + e.constructor.name + ": " + e.message; } const got = JSON.stringify(v); ok(got === WANT[name], name + ": got " + got + ", want " + WANT[name]); });
const WANT = {
    "plain literal":"[[1,2],[1,2],[1,2],[1,2],[1,2],[1,2]]",
    "truthy and falsy done":"[[\"a\",\"b\",\"c\",\"d\",\"e\"],[\"a\",\"b\",\"c\",\"d\",\"e\"],[\"a\",\"b\",\"c\",\"d\",\"e\"],[\"a\",\"b\",\"c\",\"d\",\"e\"],[\"a\",\"b\"],[\"a\",\"b\",\"c\",\"d\",\"e\"]]",
    "done object and symbol":"[[[1],[1],[1],[1],[1,null],[1]],[[1],[1],[1],[1],[1,null],[1]]]",
    "missing value":"[[null,null],[null,null],[null,null],[null,null],[null,null],[null,null]]",
    "value order after done":"[[1,2],[1,2],[1,2],[1,2],[1,2],[1,2]]",
    "getter done and value":"[[[1,2],[1,2],[1,2],[1,2],[1,2],[1,2]],[\"d1\",\"v1\",\"d2\",\"v2\",\"d3\",\"d1\",\"v1\",\"d2\",\"v2\",\"d3\",\"d1\",\"v1\",\"d2\",\"v2\",\"d3\",\"d1\",\"v1\",\"d2\",\"v2\",\"d3\",\"d1\",\"v1\",\"d2\",\"v2\",\"d1\",\"d1\",\"v1\",\"d2\",\"d2\",\"v2\",\"d3\",\"v3\"]]",
    "getter value only":"[[[10],[10],[10],[10],[10,null],[10]],[\"v1\",\"v1\",\"v1\",\"v1\",\"v1\",\"v1\",\"v2\"]]",
    "inherited done and value":"[[\"P\",\"own\"],[\"P\",\"own\"],[\"P\",\"own\"],[\"P\",\"own\"],[\"P\",\"own\"],[\"P\",\"own\"]]",
    "inherited getter":"[[[1],[1],[1],[1],[1,null],[1]],20]",
    "class instance fields":"[[1,2],[1,2],[1,2],[1,2],[1,2],[1,2]]",
    "proxy result":"[[[1],[1],[1],[1],[1,null],[1]],[\"done\",\"value\",\"done\",\"done\",\"value\",\"done\",\"done\",\"value\",\"done\",\"done\",\"value\",\"done\",\"done\",\"value\",\"done\",\"done\",\"done\",\"value\",\"done\",\"value\"]]",
    "deleted and redefined":"[[1],[1],[1],[1],[1,null],[1]]",
    "frozen and nonwritable":"[[[1],[1],[1],[1],[1,null],[1]],[[3],[3],[3],[3],[3,null],[3]]]",
    "non-object result":"[\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\"]",
    "throwing getters":"[\"RangeErrord\",\"SyntaxErrorv\"]",
    "array and function results":"[[\"arr\",\"fn\"],[\"arr\",\"fn\"],[\"arr\",\"fn\"],[\"arr\",\"fn\"],[\"arr\",\"fn\"],[\"arr\",\"fn\"]]",
    "result mutated by consumer":"[[1,2,3],{\"value\":4,\"done\":true}]",
    "accessor replaced by data":"[[1],[1],[1],[1],[1,null],[1]]",
    "huge result object":"[[1,2],[1,2],[1,2],[1,2],[1,2],[1,2]]",
};
R("plain literal", () => consumers(() => mk([{ value: 1, done: false }, { value: 2, done: false }, { value: 3, done: true }])));
R("truthy and falsy done", () => consumers(() => mk([{ value: "a", done: 0 }, { value: "b", done: "" }, { value: "c", done: NaN }, { value: "d", done: null }, { value: "e", done: 0n }, { value: "z", done: "no" }])));
R("done object and symbol", () => [consumers(() => mk([{ value: 1, done: undefined }, { value: 2, done: {} }])), consumers(() => mk([{ value: 1 }, { value: 9, done: Symbol() }]))]);
R("missing value", () => consumers(() => mk([{ done: false }, { done: false, value: undefined }, { done: true }])));
R("value order after done", () => consumers(() => mk([{ done: false, value: 1 }, { done: false, value: 2, extra: 3 }, { done: true, value: 4 }])));
R("getter done and value", () => { const log = []; const res = (v, d) => ({ get done() { log.push("d" + v); return d; }, get value() { log.push("v" + v); return v; } }); const r = consumers(() => mk([res(1, false), res(2, false), res(3, true)])); return [r, log]; });
R("getter value only", () => { const log = []; const res = (v, d) => ({ done: d, get value() { log.push("v" + v); return v * 10; } }); return [consumers(() => mk([res(1, false), res(2, true)])), log]; });
R("inherited done and value", () => { const proto = { done: false, value: "P" }; const res = (o) => Object.assign(Object.create(proto), o); return consumers(() => mk([res({}), res({ value: "own" }), res({ done: true })])); });
R("inherited getter", () => { const log = []; class Res { constructor(v, d) { this.v = v; this.d = d; } get done() { log.push("d" + this.v); return this.d; } get value() { log.push("v" + this.v); return this.v; } } return [consumers(() => mk([new Res(1, false), new Res(2, true)])), log.length]; });
R("class instance fields", () => { class Res { constructor(v, d) { this.value = v; this.done = d; } } return consumers(() => mk([new Res(1, false), new Res(2, false), new Res(3, true)])); });
R("proxy result", () => { const log = []; const res = (v, d) => new Proxy({ value: v, done: d }, { get(t, k, r) { log.push(String(k)); return Reflect.get(t, k, r); } }); return [consumers(() => mk([res(1, false), res(2, true)])), log]; });
R("deleted and redefined", () => { const res = (v, d) => { const o = { value: v, done: !d, x: 1 }; delete o.done; o.done = d; delete o.x; return o; }; return consumers(() => mk([res(1, false), res(2, true)])); });
R("frozen and nonwritable", () => { const res = (v, d) => Object.freeze({ value: v, done: d }); const r2 = (v, d) => Object.defineProperties({}, { done: { value: d }, value: { value: v, enumerable: false } }); return [consumers(() => mk([res(1, false), res(2, true)])), consumers(() => mk([r2(3, false), r2(4, true)]))]; });
R("non-object result", () => { const r = []; for (const bad of [1, "s", undefined, null, true, Symbol.iterator]) { try { for (const x of mk([bad])) r.push(x); } catch (e) { r.push(e.constructor.name); } } return r; });
R("throwing getters", () => { const r = []; const bd = { get done() { throw new RangeError("d"); } }; const bv = { done: false, get value() { throw new SyntaxError("v"); } }; for (const b of [bd, bv]) { try { for (const x of mk([b])) r.push(x); } catch (e) { r.push(e.constructor.name + e.message); } } return r; });
R("array and function results", () => { const a = [5]; a.done = false; a.value = "arr"; const f = function () {}; f.done = false; f.value = "fn"; return consumers(() => mk([a, f, { done: true }])); });
R("result mutated by consumer", () => { const shared = { value: 0, done: false }; let n = 0; const src = { [Symbol.iterator]() { return { next() { n++; shared.value = n; if (n > 3) shared.done = true; return shared; } }; } }; const r = []; for (const x of src) r.push(x); return [r, shared]; });
R("accessor replaced by data", () => { const res = (v, d) => { const o = {}; Object.defineProperty(o, "done", { get() { return true; }, configurable: true }); Object.defineProperty(o, "done", { value: d, writable: true, configurable: true }); o.value = v; return o; }; return consumers(() => mk([res(1, false), res(2, true)])); });
R("huge result object", () => { const res = (v, d) => { const o = {}; for (let i = 0; i < 64; i++) o["k" + i] = i; o.done = d; o.value = v; return o; }; return consumers(() => mk([res(1, false), res(2, false), res(3, true)])); });
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
