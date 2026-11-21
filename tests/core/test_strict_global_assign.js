// covers: integration c:src/parser/parser.inc.c c:src/object/property_set_convert.inc.c c:src/vm/interpreter.inc.c c:src/vm/tailcall.inc.c -- strict-mode assignment resolves a global reference before the right-hand side
// flags: --std
// Strict-mode `name = value` must resolve `name` before evaluating the
// right-hand side (test262 language/identifier-resolution/
// assign-to-global-undefined.js): a name that is unresolvable at that point
// throws ReferenceError at PutValue even if the right-hand side created it,
// and a global property deleted by the right-hand side also throws. The
// engine used to look the name up only after the right-hand side ran.
// Globals declared by the same script keep the direct var_ref store; any
// other global goes through check_var (before the RHS) and put_var_strict.
import { suite } from "../lib/kit.js";

const t = suite("strict_global_assign");
const G = globalThis;

const throwsRef = (a, f, what) => {
    let name = "none";
    try { f(); } catch (e) { name = e.name; }
    a.eq(name, "ReferenceError", what);
};

t.test("unresolvable before the RHS throws even when the RHS creates it", (a) => {
    throwsRef(a, () => { sgaU1 = (G.sgaU1 = 5); }, "plain statement");
    a.eq(G.sgaU1, 5, "the RHS still ran");
    delete G.sgaU1;
    throwsRef(a, () => { const y = (sgaU2 = (G.sgaU2 = 6)); return y; }, "value kept");
    delete G.sgaU2;
});

t.test("the RHS runs before the ReferenceError", (a) => {
    const log = [];
    throwsRef(a, () => { sgaU3 = (log.push("rhs"), 1); }, "throws");
    a.eq(log.join(), "rhs", "RHS side effect happened first");
    a.eq("sgaU3" in G, false, "nothing was created");
});

t.test("existing global properties are assigned", (a) => {
    G.sgaH = 1;
    sgaH = 3;
    a.eq(G.sgaH, 3, "statement");
    const y = (sgaH = 4);
    a.eq(y, 4, "expression value");
    a.eq(G.sgaH, 4, "expression store");
    let n = 0;
    for (let i = 0; i < 1000; i++) { sgaH = i; n += sgaH; }
    a.eq(n, 499500, "loop store and load");
    delete G.sgaH;
});

t.test("for-of, array and object destructuring targets", (a) => {
    G.sgaK = 0;
    for (sgaK of [7, 8]);
    a.eq(G.sgaK, 8, "for-of");
    [sgaK] = [9];
    a.eq(G.sgaK, 9, "array pattern");
    ({ sgaK } = { sgaK: 10 });
    a.eq(G.sgaK, 10, "object pattern");
    delete G.sgaK;
    throwsRef(a, () => { [sgaU4] = [1]; }, "array pattern, unresolvable");
});

t.test("a name referenced more than once in the same code", (a) => {
    throwsRef(a, () => { typeof sgaR1; sgaR1 = (G.sgaR1 = 1); }, "typeof then assignment");
    delete G.sgaR1;
    throwsRef(a, () => { try { sgaR2 = 1; } catch {} sgaR2 = (G.sgaR2 = 1); }, "two assignments");
    delete G.sgaR2;
    G.sgaR3 = 1;
    throwsRef(a, () => { sgaR3; sgaR3 = (delete G.sgaR3, 2); }, "read then deleted during RHS");
    throwsRef(a, () => std.evalScript(`"use strict"; typeof sgaS1; sgaS1 = (globalThis.sgaS1 = 1);`), "script: typeof then assignment");
    delete G.sgaS1;
    throwsRef(a, () => std.evalScript(`"use strict"; try { sgaS2 = 1; } catch {} sgaS2 = (globalThis.sgaS2 = 1);`), "script: two assignments");
    delete G.sgaS2;
    throwsRef(a, () => std.evalScript(`"use strict"; globalThis.sgaS3 = 1; sgaS3; sgaS3 = (delete globalThis.sgaS3, 2);`), "script: read then deleted during RHS");
    G.sgaS4 = 1;
    a.eq(std.evalScript(`"use strict"; sgaS4; sgaS4 = 5; sgaS4`), 5, "script: existing global read then assigned");
    delete G.sgaS4;
});

t.test("a global deleted by the RHS throws", (a) => {
    G.sgaD = 1;
    throwsRef(a, () => { sgaD = (delete G.sgaD, 2); }, "deleted during RHS");
    a.eq("sgaD" in G, false, "not recreated");
});

t.test("direct eval inside a strict function", (a) => {
    throwsRef(a, () => { typeof sgaU5; eval("sgaU5 = (globalThis.sgaU5 = 1)"); }, "eval code");
    delete G.sgaU5;
    throwsRef(a, () => std.evalScript(`"use strict"; typeof sgaU6; eval("sgaU6 = (globalThis.sgaU6 = 1)")`), "eval in a strict script");
    delete G.sgaU6;
    throwsRef(a, () => std.evalScript(`"use strict"; eval("sgaU7 = (globalThis.sgaU7 = 1)")`), "eval in a strict script, name unseen outside");
    delete G.sgaU7;
    throwsRef(a, () => std.evalScript(`(function () { "use strict"; typeof sgaU8; return () => eval("sgaU8 = (globalThis.sgaU8 = 1)"); })()()`), "eval in a nested strict arrow");
    delete G.sgaU8;
    const late = () => { typeof sgaE2; return eval("sgaE2 = 2"); };
    G.sgaE2 = 1;
    a.eq(late(), 2, "eval assigns a global created after the closure");
    delete G.sgaE2;
    G.sgaE2 = 0;
    a.eq(late(), 2, "eval assigns a global deleted and recreated");
    delete G.sgaE2;
    G.sgaE = 1;
    (() => { typeof sgaE; eval("sgaE = 2"); })();
    a.eq(G.sgaE, 2, "eval assigns an existing global");
    delete G.sgaE;
});

t.test("globals declared by another script", (a) => {
    std.evalScript("var sgaV = 1; const sgaC = 1; let sgaL = 1;");
    sgaV = 2;
    a.eq(G.sgaV, 2, "var");
    sgaL = 3;
    a.eq(std.evalScript("sgaL"), 3, "let");
    let name = "none";
    try { sgaC = 2; } catch (e) { name = e.name; }
    a.eq(name, "TypeError", "const");
});

t.test("globals declared by the same script keep working", (a) => {
    const r = std.evalScript(`"use strict"; var sgaS = 0; let sgaSL = 0; function sgaF() { for (let i = 0; i < 10; i++) { sgaS = i; sgaSL = i; } } sgaF(); [sgaS, sgaSL].join()`);
    a.eq(r, "9,9", "var and let");
    let name = "none";
    try { std.evalScript(`"use strict"; const sgaSC = 1; sgaSC = 2;`); } catch (e) { name = e.name; }
    a.eq(name, "TypeError", "const in the same script");
});

t.test("sloppy mode still creates the global", (a) => {
    (0, eval)("sgaW = 4");
    a.eq(G.sgaW, 4, "created");
    delete G.sgaW;
    a.eq(new Function("sgaW2 = (globalThis.sgaW2 = 1, 2); return sgaW2;")(), 2, "sloppy function");
    delete G.sgaW2;
});

await t.run();
