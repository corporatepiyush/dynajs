// The compiler removes let/const TDZ checks it can prove redundant (a
// must-be-initialized dataflow over the whole function, so checks inside
// loop bodies, across if/else joins and on module bindings go). Every case
// here is one where a check MUST survive: the read can happen while the
// binding is still uninitialized, and the engine has to throw ReferenceError
// instead of reading the internal uninitialized marker as a value.
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsRef(f, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ReferenceError, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}
function same(a, b, m) { check(JSON.stringify(a) === JSON.stringify(b), m + " -- got " + JSON.stringify(a)); }

throwsRef(() => { { x; let x = 1; } }, "read before the declaration in one block");
throwsRef(() => { const r = []; for (let k = 0; k < 3; k++) { if (k > 0) r.push(y); let y = k; } }, "per-iteration binding is uninitialized again on the next iteration");
throwsRef(() => { for (let k = 0; k < 2; k++) { let t; if (k === 1) { z; } let z = k; void t; } }, "second iteration reads before init after the first initialized it");
throwsRef(() => { const v = 1; switch (v) { case 0: let z = 1; break; case 1: return z; } }, "switch case skips the declaration");
throwsRef(() => { let ok = false; try { if (!ok) throw 1; } catch (e) { return q; } let q = 2; }, "catch handler reads a later binding");
throwsRef(() => { try { throw 0; let w = 1; } catch { } finally { } return w2(); function w2() { return w; } }, "closure over a binding the throw skipped");
throwsRef(() => { f(); let a = 1; function f() { return a; } }, "hoisted function reads before init");
throwsRef(() => { new C(); class C {} }, "class used before its declaration");
throwsRef(() => { let i = 0; while (true) { if (i++ === 3) return u; } let u = 1; }, "loop exit reads a binding declared after the loop");
throwsRef(() => { lbl: { if (true) break lbl; var never = 1; } let after = before; let before = 1; void never; void after; }, "labeled break then read before init");
throwsRef(() => { const g = (function* () { yield 1; yield gv; })(); g.next(); g.next(); let gv = 1; }, "generator resumed before the outer binding exists");
throwsRef(() => { let s = 0; for (const c of [1, 2]) { s += c; if (c === 2) s += late; } let late = 1; }, "for-of body reads a binding declared after the loop");
throwsRef(() => { let k = 0; do { if (k === 1) return dd; let dd = 3; k++; } while (k < 3); }, "do-while body re-entered");
throwsRef(() => { const fns = []; for (let i = 0; i < 2; i++) { fns.push(() => cap); if (i === 0) fns[0](); let cap = i; } }, "closure created before its block binding initializes");
throwsRef(() => { let a = 1; { a = b; let b = 2; } }, "assignment from a binding in TDZ");
throwsRef(() => { { b = 3; let b; } }, "assignment to a binding in TDZ");
throwsRef(() => { let r = 0; try { r = 1; } finally { r += fin; } let fin = 1; }, "finally block reads a later binding");
throwsRef(() => { if (Math.random() < 2) { } else { let never; } let x1 = x2; let x2 = 1; }, "if/else join keeps an uninitialized sibling");

same((() => { let s = 0; for (let i = 0; i < 5; i++) { let t = i * 2; s += t; } return s; })(), 20, "plain loop still computes");
same((() => { let a = 1, out = []; for (let i = 0; i < 3; i++) { if (i % 2) { a += i; } else { a *= 2; } out.push(a); } return out; })(), [2, 3, 6], "if/else join over initialized bindings");
same((() => { const fns = []; for (let i = 0; i < 3; i++) fns.push(() => i); return fns.map(f => f()); })(), [0, 1, 2], "per-iteration capture still copies");
same((() => { let x; try { x = 1; throw 0; } catch { x += 1; } finally { x += 10; } return x; })(), 12, "try/catch/finally with an initialized binding");
same((() => { let r = 0; outer: for (let i = 0; i < 4; i++) { for (let j = 0; j < 4; j++) { if (j === 2) continue outer; if (i === 3) break outer; r += i * j; } } return r; })(), 3, "labeled continue/break");
same((() => { let acc = ""; switch (2) { case 1: acc += "a"; case 2: { let inner = "b"; acc += inner; } case 3: acc += "c"; } return acc; })(), "bc", "switch fall-through");
same((() => { function* g() { let v = 0; while (v < 3) { yield v; v++; } } return [...g()]; })(), [0, 1, 2], "generator loop");

{
    // A module body: its top-level bindings live in closure slots, and the
    // checks on them go once they are known initialized; TDZ must still fire.
    const src = `let m = 1; for (let i = 0; i < 3; i++) m += i; export const out = m; export function early() { return late; } export let late = 5;`;
    let threwEarly = null;
    const tdzSrc = `early(); let lateVal = 1; function early() { return lateVal; }`;
    try { (0, eval)(tdzSrc); } catch (e) { threwEarly = e; }
    check(threwEarly instanceof ReferenceError, "script top level: hoisted function reads a binding before init");
    void src;
}

print(`test_tdz_elision: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
