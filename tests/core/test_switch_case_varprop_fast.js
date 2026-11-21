// covers: integration c:src/vm/tailcall.inc.c c:src/vm/interpreter.inc.c -- the same fast path in both interpreters
// `switch (x) { case OBJ.prop: }` compiles each case to streq_varprop_if_false (closure var OBJ, atom
// prop, branch offset). Both interpreters (tailcall, and the switch build) now have a fast path: OBJ an object, x and the
// property's value both ints or both strings, the per-pc case IC (slot index, validated by the slot's
// atom and a plain-data flag check on the object's current shape) hits, forward branch. Everything else
// (TDZ, a non-object OBJ, getters, proxies, inherited or missing properties, an IC miss, mixed
// int/double, other types) jumps to the unchanged general handler. Taking a branch on the compare, not
// a select, keeps the next dispatch speculative: a csel made the 20-case micro 157 -> 245 ms; the
// branch makes it 157 -> 135 ms (int values) and 262 -> 204 ms (string values). The remaining gap to
// literal int cases (61 ms) is structural: those compile to a jump table (OP_switch), OBJ.prop cases
// stay a chain of compares because the values can change between runs.
// Oracle: the same dispatch written with if/else and ===, run on every input after every mutation.
import { suite } from "../lib/kit.js";

const t = suite("switch case OBJ.prop fast path");
let N = { A: 1, B: 2, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 };
function sw(k) {
    switch (k) {
        case N.A: return "A";
        case N.B: return "B";
        case N.C: return "C";
        case N.S: return "S";
        case N.T: return "T";
        case N.U: return "U";
        case N.D: return "D";
    }
    return "-";
}
function ref(k) {
    for (const key of ["A", "B", "C", "S", "T", "U", "D"]) if (k === N[key]) return key;
    return "-";
}
const INPUTS = [0, 1, 2, 3, 4, 2.5, 1.0, -0, NaN, "kind1", "kind2", "kind3", "x", "y", "", "1", null, undefined, true, 1n];
function agree(a, what) {
    const want = INPUTS.map(ref);
    for (let r = 0; r < 3; r++) a.deep(INPUTS.map(sw), want, what + " run " + r);
}
t.test("plain values", (a) => agree(a, "plain"));
t.test("value changes, type changes, int stored as double", (a) => {
    agree(a, "warm");
    N.A = 4; agree(a, "A=4");
    N.B = "kind3"; agree(a, "B string");
    N.C = 0.5 + 0.5; agree(a, "C computed double 1");
    N.S = 1; agree(a, "S int");
    N.T = "y" + ""; agree(a, "T concat");
    N.U = null; agree(a, "U null");
    N.D = 2; agree(a, "D int");
});
t.test("getter installed after warm-up, delete and re-add, inherited, missing", (a) => {
    N = { A: 1, B: 2, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 };
    agree(a, "fresh");
    let gets = 0;
    Object.defineProperty(N, "B", { configurable: true, enumerable: true, get() { gets++; return 3; } });
    a.eq(sw(3), "B", "getter value used");
    a.ok(gets >= 1, "getter ran");
    agree(a, "getter");
    delete N.B; agree(a, "deleted");
    N.B = 2; agree(a, "re-added");
    delete N.A; N.A = 0; agree(a, "A moved to the end");
    N = Object.create({ A: 1, B: 2, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 });
    agree(a, "inherited");
    N = {};
    agree(a, "all missing");
});
t.test("OBJ rebound, proxy, array, string OBJ", (a) => {
    N = { A: 1, B: 2, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 };
    agree(a, "warm");
    N = { D: 9, U: 8, T: 7, S: 6, C: 5, B: 4, A: 3 };
    agree(a, "rebound, other shape");
    const seen = [];
    N = new Proxy({ A: 1, B: 2, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 }, { get(o, k) { seen.push(k); return o[k]; } });
    a.eq(sw(3), "C", "proxy result");
    a.deep(seen, ["A", "B", "C"], "proxy gets in case order, stop at the match");
    N = [5, 6];
    N.A = 5; N.B = 6;
    agree(a, "array OBJ");
    N = "str";
    agree(a, "primitive OBJ");
});
t.test("TDZ and order of evaluation", (a) => {
    const log = [];
    N = { get A() { log.push("A"); return 1; }, get B() { log.push("B"); return 2; }, C: 3, S: "kind1", T: "kind2", U: "x", D: 2.5 };
    for (let i = 0; i < 3; i++) sw(2);
    a.deep(log, ["A", "B", "A", "B", "A", "B"], "cases evaluated in order up to the match");
});
function early(k) {
    switch (k) {
        case M.A: return 1;
    }
    return 0;
}
let flag = 0;
try { early(1); } catch (e) { flag = e instanceof ReferenceError ? 1 : 2; }
const M = { A: 1 };
t.test("after TDZ ends", (a) => {
    a.eq(flag, 1);
    a.eq(early(1), 1);
    a.eq(early(2), 0);
});
await t.run();
